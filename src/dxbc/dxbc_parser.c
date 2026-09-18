// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_parser_internal.h"
#include "dxbc/dxbc_hash.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DXBC_HEADER_SIZE 32u

static bool range_within(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

static bool signature_is_streamed(const char* chunk_name) {
    return strcmp(chunk_name, "ISG1") == 0 ||
           strcmp(chunk_name, "OSG1") == 0 ||
           strcmp(chunk_name, "OSG5") == 0 ||
           strcmp(chunk_name, "PSG1") == 0;
}

static bool signature_has_min_precision(const char* chunk_name) {
    return strcmp(chunk_name, "ISG1") == 0 ||
           strcmp(chunk_name, "OSG1") == 0 ||
           strcmp(chunk_name, "PSG1") == 0;
}

static DXBCSignatureRole signature_role_for_chunk(const char* chunk_name) {
    if (strcmp(chunk_name, "ISGN") == 0 ||
        strcmp(chunk_name, "ISG1") == 0) {
        return DXBC_SIGNATURE_ROLE_INPUT;
    }
    if (strcmp(chunk_name, "PCSG") == 0 ||
        strcmp(chunk_name, "PSG1") == 0) {
        return DXBC_SIGNATURE_ROLE_PATCH_CONSTANT;
    }
    return DXBC_SIGNATURE_ROLE_OUTPUT;
}

static bool signature_min_precision_is_valid(uint32_t value) {
    return value == 0u || value == 1u || value == 2u || value == 4u ||
           value == 5u || value == 0xf0u || value == 0xf1u;
}

static bool signature_system_value_is_valid(uint32_t value) {
    switch (value) {
        case 0u:
        case 1u:
        case 2u:
        case 3u:
        case 4u:
        case 5u:
        case 6u:
        case 7u:
        case 8u:
        case 9u:
        case 10u:
        case 11u:
        case 12u:
        case 13u:
        case 14u:
        case 15u:
        case 16u:
        case 23u:
        case 24u:
        case 25u:
        case 64u:
        case 65u:
        case 66u:
        case 67u:
        case 68u:
        case 69u:
        case 70u:
            return true;
        default:
            return false;
    }
}

static unsigned char ascii_lower(unsigned char value) {
    if (value >= (unsigned char)'A' && value <= (unsigned char)'Z') {
        return (unsigned char)(value + ('a' - 'A'));
    }
    return value;
}

static bool semantic_equals_ascii_case_insensitive(const char* semantic,
                                                    const char* expected) {
    if (!semantic || !expected) return false;
    while (*semantic != '\0' && *expected != '\0') {
        if (ascii_lower((unsigned char)*semantic) !=
            ascii_lower((unsigned char)*expected)) {
            return false;
        }
        ++semantic;
        ++expected;
    }
    return *semantic == '\0' && *expected == '\0';
}

static const char* signature_system_semantic(uint32_t value) {
    switch (value) {
        case 1u: return "SV_Position";
        case 2u: return "SV_ClipDistance";
        case 3u: return "SV_CullDistance";
        case 4u: return "SV_RenderTargetArrayIndex";
        case 5u: return "SV_ViewportArrayIndex";
        case 6u: return "SV_VertexID";
        case 7u: return "SV_PrimitiveID";
        case 8u: return "SV_InstanceID";
        case 9u: return "SV_IsFrontFace";
        case 10u: return "SV_SampleIndex";
        case 11u:
        case 13u:
        case 15u:
        case 16u:
            return "SV_TessFactor";
        case 12u:
        case 14u:
            return "SV_InsideTessFactor";
        case 23u: return "SV_Barycentrics";
        case 24u: return "SV_ShadingRate";
        case 25u: return "SV_CullPrimitive";
        case 64u: return "SV_Target";
        case 65u: return "SV_Depth";
        case 66u: return "SV_Coverage";
        case 67u: return "SV_DepthGreaterEqual";
        case 68u: return "SV_DepthLessEqual";
        case 69u: return "SV_StencilRef";
        case 70u: return "SV_InnerCoverage";
        default: return NULL;
    }
}

static bool signature_semantic_is_passthrough_system_value(
    const char* semantic) {
    /* These values can be generated for an early stage and then carried as
     * ordinary inter-stage data.  In the latter form FXC records the reserved
     * semantic spelling while leaving D3D_NAME as UNDEFINED. */
    static const uint32_t passthrough_system_values[] = {
        6u, /* SV_VertexID */
        7u, /* SV_PrimitiveID */
        8u, /* SV_InstanceID */
        9u  /* SV_IsFrontFace */
    };
    for (size_t index = 0u;
         index < sizeof(passthrough_system_values) /
                     sizeof(passthrough_system_values[0]);
         ++index) {
        if (semantic_equals_ascii_case_insensitive(
                semantic,
                signature_system_semantic(
                    passthrough_system_values[index]))) {
            return true;
        }
    }
    return false;
}

static bool semantic_is_hlsl_identifier(const char* semantic) {
    if (!semantic || semantic[0] == '\0') return false;
    const unsigned char first = (unsigned char)semantic[0];
    if (!((first >= (unsigned char)'A' && first <= (unsigned char)'Z') ||
          (first >= (unsigned char)'a' && first <= (unsigned char)'z') ||
          first == (unsigned char)'_')) {
        return false;
    }
    for (size_t index = 1u; semantic[index] != '\0'; ++index) {
        const unsigned char value = (unsigned char)semantic[index];
        if (!((value >= (unsigned char)'A' &&
               value <= (unsigned char)'Z') ||
              (value >= (unsigned char)'a' &&
               value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              value == (unsigned char)'_')) {
            return false;
        }
    }
    return true;
}

static bool reserve_array(void** array, int* allocation, int required,
                          size_t element_size) {
    if (!array || !allocation || *allocation < 0 || required < 0 ||
        ((*allocation == 0) != (*array == NULL))) return false;
    if (required <= *allocation) return true;

    int new_allocation = *allocation == 0 ? 16 : *allocation;
    while (new_allocation < required) {
        if (new_allocation > INT_MAX / 2) {
            new_allocation = required;
            break;
        }
        new_allocation *= 2;
    }
    if (dxbc_size_multiply_overflows((size_t)new_allocation, element_size) ||
        dxbc_size_multiply_overflows((size_t)*allocation, element_size)) {
        return false;
    }
    const size_t old_size = (size_t)*allocation * element_size;
    const size_t new_size = (size_t)new_allocation * element_size;
    void* replacement = mem_realloc(*array, old_size, new_size);
    if (!replacement) return false;
    *array = replacement;
    *allocation = new_allocation;
    return true;
}

const char* dxbc_signature_semantic_name(
    const DXBCSignatureElement* element) {
    if (!element) return NULL;
    return element->semantic_name_extended
               ? element->semantic_name_extended
               : element->semantic_name;
}

void dxbc_signature_element_free(DXBCSignatureElement* element) {
    if (!element) return;
    if (element->semantic_name_extended) {
        mem_free(element->semantic_name_extended,
                 element->semantic_name_length + 1u);
    }
    memset(element, 0, sizeof(*element));
}

bool dxbc_signature_element_is_valid(
    const DXBCSignatureElement* element, DXBCSignatureRole role) {
    if (!element || role < DXBC_SIGNATURE_ROLE_INPUT ||
        role > DXBC_SIGNATURE_ROLE_PATCH_CONSTANT ||
        element->component_type < 1u || element->component_type > 3u ||
        element->mask == 0u || (element->mask & 0xf0u) != 0u ||
        (element->rw_mask & 0xf0u) != 0u || element->stream_index > 3u ||
        element->interpolation_mode > 7u ||
        !signature_min_precision_is_valid(element->min_precision) ||
        !signature_system_value_is_valid(element->system_value)) {
        return false;
    }
    if (role == DXBC_SIGNATURE_ROLE_INPUT &&
        ((element->rw_mask & (uint8_t)~element->mask) != 0u ||
         element->stream_index != 0u)) {
        return false;
    }
    if (role != DXBC_SIGNATURE_ROLE_INPUT &&
        element->interpolation_mode != 0u) {
        return false;
    }

    const char* semantic = dxbc_signature_semantic_name(element);
    if (!semantic || !semantic_is_hlsl_identifier(semantic)) return false;
    if (element->semantic_name_extended) {
        if (element->semantic_name_length == SIZE_MAX ||
            semantic[element->semantic_name_length] != '\0' ||
            memchr(semantic, '\0', element->semantic_name_length)) {
            return false;
        }
    } else if (!memchr(element->semantic_name, '\0',
                       sizeof(element->semantic_name))) {
        return false;
    }

    const char* expected = signature_system_semantic(element->system_value);
    if (expected) {
        return semantic_equals_ascii_case_insensitive(semantic, expected);
    }
    if (signature_semantic_is_passthrough_system_value(semantic)) {
        return true;
    }
    /* SM4/5 OSGN commonly encodes pixel special outputs with D3D_NAME 0 even
     * though the semantic text is authoritative (for example SV_Target0).
     * Keep that compiler-authored form distinct from an arbitrary SV_ name. */
    static const char* implicit_output_system_semantics[] = {
        "SV_Target", "SV_Depth", "SV_Coverage",
        "SV_DepthGreaterEqual", "SV_DepthLessEqual", "SV_StencilRef",
        "SV_InnerCoverage"
    };
    for (size_t index = 0u;
         index < sizeof(implicit_output_system_semantics) /
                     sizeof(implicit_output_system_semantics[0]);
         ++index) {
        if (semantic_equals_ascii_case_insensitive(
                semantic, implicit_output_system_semantics[index])) {
            return role != DXBC_SIGNATURE_ROLE_INPUT;
        }
    }
    /* The reserved SV_ namespace cannot be claimed by an arbitrary semantic.
     * This is the only semantic-name restriction for D3D_NAME_UNDEFINED. */
    return !(ascii_lower((unsigned char)semantic[0]) == (unsigned char)'s' &&
             ascii_lower((unsigned char)semantic[1]) == (unsigned char)'v' &&
             semantic[2] == '_');
}

bool dxbc_signature_element_clone(DXBCSignatureElement* destination,
                                  const DXBCSignatureElement* source) {
    if (!destination || !source || destination == source) return false;
    memset(destination, 0, sizeof(*destination));
    memcpy(destination, source, sizeof(*destination));
    destination->semantic_name_extended = NULL;
    if (!source->semantic_name_extended) return true;
    if (source->semantic_name_length == SIZE_MAX ||
        source->semantic_name_extended[source->semantic_name_length] != '\0') {
        memset(destination, 0, sizeof(*destination));
        return false;
    }
    destination->semantic_name_extended =
        mem_alloc(source->semantic_name_length + 1u);
    if (!destination->semantic_name_extended) {
        memset(destination, 0, sizeof(*destination));
        return false;
    }
    memcpy(destination->semantic_name_extended,
           source->semantic_name_extended,
           source->semantic_name_length + 1u);
    return true;
}

static bool set_signature_semantic_name(DXBCSignatureElement* element,
                                        const uint8_t* name,
                                        size_t length) {
    if (!element || !name || length == SIZE_MAX) return false;
    element->semantic_name_length = length;
    if (length < sizeof(element->semantic_name)) {
        memcpy(element->semantic_name, name, length);
        element->semantic_name[length] = '\0';
        return true;
    }
    element->semantic_name_extended = mem_alloc(length + 1u);
    if (!element->semantic_name_extended) return false;
    memcpy(element->semantic_name_extended, name, length);
    element->semantic_name_extended[length] = '\0';
    return true;
}

static void free_signature_list_elements(DXBCSignatureElement* elements,
                                         int count) {
    if (!elements || count <= 0) return;
    for (int index = 0; index < count; ++index) {
        dxbc_signature_element_free(&elements[index]);
    }
}

static bool parse_signature(ByteStream* stream, size_t chunk_start,
                            size_t chunk_end, DXBCSignatureElement** list,
                            int* count, int* allocation,
                            const char* chunk_name) {
    uint32_t element_count;
    uint32_t parameter_offset;
    const bool has_stream = signature_is_streamed(chunk_name);
    const bool has_min_precision = signature_has_min_precision(chunk_name);
    const DXBCSignatureRole role = signature_role_for_chunk(chunk_name);
    const size_t element_size = (has_stream ? 4u : 0u) + 24u +
                                (has_min_precision ? 4u : 0u);

    if (!list || !count || !allocation ||
        !stream_read_uint32(stream, &element_count) ||
        !stream_read_uint32(stream, &parameter_offset) ||
        stream->position > chunk_end ||
        *count < 0 || *allocation < *count ||
        (*allocation > 0 && !*list) ||
        parameter_offset > chunk_end - chunk_start ||
        chunk_start + parameter_offset < stream->position ||
        element_count >
            (chunk_end - (chunk_start + parameter_offset)) / element_size ||
        element_count > (uint32_t)(INT_MAX - *count) ||
        !reserve_array((void**)list, allocation,
                       *count + (int)element_count,
                       sizeof(**list))) {
        return false;
    }

    stream->position = chunk_start + parameter_offset;

    for (uint32_t i = 0; i < element_count; ++i) {
        DXBCSignatureElement* el = &(*list)[*count];
        uint32_t name_offset;
        uint32_t mask_token;
        memset(el, 0, sizeof(*el));

        if (has_stream && !stream_read_uint32(stream, &el->stream_index)) {
            return false;
        }
        if (!stream_read_uint32(stream, &name_offset) ||
            !stream_read_uint32(stream, &el->semantic_index) ||
            !stream_read_uint32(stream, &el->system_value) ||
            !stream_read_uint32(stream, &el->component_type) ||
            !stream_read_uint32(stream, &el->register_id) ||
            !stream_read_uint32(stream, &mask_token) ||
            (has_min_precision &&
             (!stream_read_uint32(stream, &el->min_precision) ||
              !signature_min_precision_is_valid(el->min_precision)))) {
            return false;
        }

        el->mask = (uint8_t)(mask_token & 0xffu);
        el->rw_mask = (uint8_t)((mask_token >> 8) & 0xffu);
        if ((mask_token & UINT32_C(0xffff0000)) != 0u ||
            name_offset >= chunk_end - chunk_start) {
            return false;
        }

        const uint8_t* name = stream->data + chunk_start + name_offset;
        const size_t available = chunk_end - (chunk_start + name_offset);
        const uint8_t* terminator = memchr(name, 0, available);
        if (!terminator ||
            !set_signature_semantic_name(
                el, name, (size_t)(terminator - name))) {
            return false;
        }
        if (!dxbc_signature_element_is_valid(el, role)) {
            if (getenv("DXBC_DEBUG_STAGE")) {
                fprintf(stderr,
                        "[dxbc] invalid %s signature %s%u sv=%u type=%u "
                        "reg=%u mask=0x%02x rw=0x%02x stream=%u min=%u\n",
                        chunk_name, dxbc_signature_semantic_name(el),
                        el->semantic_index, el->system_value,
                        el->component_type, el->register_id, el->mask,
                        el->rw_mask, el->stream_index, el->min_precision);
            }
            dxbc_signature_element_free(el);
            return false;
        }
        ++(*count);
    }
    return true;
}

static bool read_chunk_header(ByteStream* stream, uint32_t offset,
                              uint32_t total_size, char name[5],
                              uint32_t* chunk_size, size_t* chunk_start,
                              size_t* chunk_end) {
    if ((offset & 3u) != 0 || !range_within(offset, 8, total_size)) {
        return false;
    }
    stream->position = offset;
    if (!stream_read_bytes(stream, (uint8_t*)name, 4) ||
        !stream_read_uint32(stream, chunk_size)) {
        return false;
    }
    name[4] = '\0';
    *chunk_start = stream->position;
    if (!range_within(*chunk_start, *chunk_size, total_size)) {
        return false;
    }
    *chunk_end = *chunk_start + *chunk_size;
    return true;
}

bool dxbc_parse(DXBCContainer* container, const uint8_t* data, size_t size) {
    DXBCContainerView view;
    ByteStream stream;
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t chunk_count;
    uint32_t* offsets = NULL;
    bool success = false;

    if (!container) {
        return false;
    }
    memset(container, 0, sizeof(*container));
    container->program_type = DXBC_PROGRAM_TYPE_INVALID;
    bool view_valid = dxbc_container_view_first(data, size, &view);
    bool hash_valid = view_valid && dxbc_verify_hash(view.data, view.size);
    if (!view_valid || !hash_valid) {
        if (getenv("DXBC_DEBUG_STAGE"))
            fprintf(stderr, "[dxbc] view=%d hash=%d input=%zu view_size=%zu\n",
                    view_valid, hash_valid, size, view_valid ? view.size : 0);
        return false;
    }

    stream_init(&stream, view.data, view.size);
    stream_set_endian(&stream, false);
    uint8_t hash[16];
    if (!stream_read_uint32(&stream, &magic) || magic != 0x43425844u ||
        !stream_read_bytes(&stream, hash, sizeof(hash)) ||
        !stream_read_uint32(&stream, &version) || version != 1 ||
        !stream_read_uint32(&stream, &total_size) || total_size != view.size ||
        total_size < DXBC_HEADER_SIZE ||
        !stream_read_uint32(&stream, &chunk_count) ||
        chunk_count > (total_size - DXBC_HEADER_SIZE) / sizeof(uint32_t) ||
        chunk_count > INT_MAX) {
        return false;
    }

    if (chunk_count > 0) {
        offsets = (uint32_t*)malloc((size_t)chunk_count * sizeof(*offsets));
        if (!offsets) {
            return false;
        }
    }
    for (uint32_t i = 0; i < chunk_count; ++i) {
        if (!stream_read_uint32(&stream, &offsets[i]) ||
            offsets[i] < DXBC_HEADER_SIZE + chunk_count * sizeof(uint32_t)) {
            goto cleanup;
        }
    }

    if (chunk_count > 0) {
        if (dxbc_size_multiply_overflows(
                (size_t)chunk_count, sizeof(*container->chunks))) {
            goto cleanup;
        }
        const size_t chunks_size =
            (size_t)chunk_count * sizeof(*container->chunks);
        container->chunks = (DXBCChunkInfo*)mem_alloc(chunks_size);
        if (!container->chunks) goto cleanup;
        container->chunk_alloc = (int)chunk_count;
    }

    for (uint32_t i = 0; i < chunk_count; ++i) {
        char chunk_name[5];
        uint32_t chunk_size;
        size_t chunk_start;
        size_t chunk_end;
        if (!read_chunk_header(&stream, offsets[i], total_size, chunk_name,
                               &chunk_size, &chunk_start, &chunk_end)) {
            goto cleanup;
        }

        DXBCChunkInfo* info = &container->chunks[container->chunk_count++];
        memcpy(info->name, chunk_name, sizeof(info->name));
        info->size = chunk_size;
        info->offset = offsets[i];

        if (strcmp(chunk_name, "ISGN") == 0 ||
            strcmp(chunk_name, "ISG1") == 0) {
            if (!parse_signature(&stream, chunk_start, chunk_end,
                                 &container->input_signature,
                                 &container->input_signature_count,
                                 &container->input_signature_alloc,
                                 chunk_name)) {
                goto cleanup;
            }
        } else if (strcmp(chunk_name, "OSGN") == 0 ||
                   strcmp(chunk_name, "OSG1") == 0 ||
                   strcmp(chunk_name, "OSG5") == 0) {
            if (!parse_signature(&stream, chunk_start, chunk_end,
                                 &container->output_signature,
                                 &container->output_signature_count,
                                 &container->output_signature_alloc,
                                 chunk_name)) {
                goto cleanup;
            }
        } else if (strcmp(chunk_name, "PCSG") == 0 ||
                   strcmp(chunk_name, "PSG1") == 0) {
            if (!parse_signature(&stream, chunk_start, chunk_end,
                                 &container->patch_constant_signature,
                                 &container->patch_constant_signature_count,
                                 &container->patch_constant_signature_alloc,
                                 chunk_name)) {
                goto cleanup;
            }
        }
    }

    for (uint32_t i = 0; i < chunk_count; ++i) {
        char chunk_name[5];
        uint32_t chunk_size;
        size_t chunk_start;
        size_t chunk_end;
        if (!read_chunk_header(&stream, offsets[i], total_size, chunk_name,
                               &chunk_size, &chunk_start, &chunk_end)) {
            goto cleanup;
        }
        if ((strcmp(chunk_name, "SHEX") == 0 ||
             strcmp(chunk_name, "SHDR") == 0) &&
            !parse_shader_logic(&stream, chunk_end, container)) {
            goto cleanup;
        }
    }
    success = container->has_executable_program;
    if (success) container->parsed_signature_authority = true;

cleanup:
    free(offsets);
    if (!success) {
        if (getenv("DXBC_DEBUG_STAGE")) {
            fprintf(stderr, "[dxbc] parse cleanup chunks=%d model=%s\n",
                    container->chunk_count, container->shader_type_model);
            for (int i = 0; i < container->chunk_count; i++)
                fprintf(stderr, "[dxbc] chunk %s size=%u offset=%u\n",
                        container->chunks[i].name, container->chunks[i].size,
                        container->chunks[i].offset);
        }
        dxbc_free(container);
    }
    return success;
}

const DXBCResourceDecl* dxbc_find_resource_declaration(
    const DXBCContainer* container, int register_index, bool uav) {
    if (!container || register_index < 0) return NULL;
    const DXBCResourceDecl* declarations =
        uav ? container->uavs : container->resources;
    const int count = uav ? container->uav_count : container->resource_count;
    if (count < 0 || (count > 0 && !declarations)) return NULL;
    for (int index = 0; index < count; ++index) {
        if (declarations[index].register_index == register_index)
            return &declarations[index];
        if (declarations[index].register_index > register_index) break;
    }
    return NULL;
}

DXBCResourceDecl* dxbc_get_or_add_resource_declaration(
    DXBCContainer* container, int register_index, bool uav) {
    if (!container || register_index < 0) return NULL;
    DXBCResourceDecl** declarations =
        uav ? &container->uavs : &container->resources;
    int* count = uav ? &container->uav_count : &container->resource_count;
    int* allocation = uav ? &container->uav_alloc : &container->resource_alloc;
    if (*count < 0 || *allocation < *count ||
        ((*allocation == 0) != (*declarations == NULL))) {
        return NULL;
    }

    int position = 0;
    while (position < *count &&
           (*declarations)[position].register_index < register_index) {
        ++position;
    }
    if (position < *count &&
        (*declarations)[position].register_index == register_index) {
        return &(*declarations)[position];
    }
    if (*count == INT_MAX ||
        !reserve_array((void**)declarations, allocation, *count + 1,
                       sizeof(**declarations))) {
        return NULL;
    }
    memmove(&(*declarations)[position + 1], &(*declarations)[position],
            (size_t)(*count - position) * sizeof(**declarations));
    DXBCResourceDecl* declaration = &(*declarations)[position];
    memset(declaration, 0, sizeof(*declaration));
    declaration->register_index = register_index;
    ++(*count);
    return declaration;
}

void dxbc_free(DXBCContainer* container) {
    if (!container) {
        return;
    }
    if (container->instructions) {
        for (int i = 0; i < container->instruction_count; ++i) {
            for (int j = 0; j < container->instructions[i].operand_count; ++j) {
                free_operand(&container->instructions[i].operands[j]);
            }
        }
        mem_free(container->instructions,
                 (size_t)container->instruction_alloc * sizeof(DXBCInstruction));
    }
    mem_free(container->resources,
             (size_t)container->resource_alloc * sizeof(*container->resources));
    mem_free(container->uavs,
             (size_t)container->uav_alloc * sizeof(*container->uavs));
    free_signature_list_elements(container->input_signature,
                                 container->input_signature_count);
    free_signature_list_elements(container->output_signature,
                                 container->output_signature_count);
    free_signature_list_elements(container->patch_constant_signature,
                                 container->patch_constant_signature_count);
    mem_free(container->input_signature,
             (size_t)container->input_signature_alloc *
                 sizeof(*container->input_signature));
    mem_free(container->output_signature,
             (size_t)container->output_signature_alloc *
                 sizeof(*container->output_signature));
    mem_free(container->patch_constant_signature,
             (size_t)container->patch_constant_signature_alloc *
                 sizeof(*container->patch_constant_signature));
    mem_free(container->chunks,
             (size_t)container->chunk_alloc * sizeof(*container->chunks));
    mem_free(container->icb_values,
             (size_t)container->icb_value_alloc *
                 sizeof(*container->icb_values));
    memset(container, 0, sizeof(*container));
}
