// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/sha256.h"
#include "compiler/unity_compiler_client.h"
#include "compiler/unity_compute_verifier.h"
#include "io/unity_compute_binary.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/usil_validation.h"
#include "translation/hlsl_source_quality.h"
#include "../../src/translation/hlsl_compute_source_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_FILE_LIMIT (16U * 1024U * 1024U)
#define PROBE_PLATFORM 4U
#define PROBE_BUILD_PLATFORM 1U
#define PROBE_DECODE_LIMIT 32U
#define PROBE_DECODE_DEPTH_LIMIT 8U
#define PROBE_DIAGNOSTIC_LIMIT 8U
#define PROBE_DIAGNOSTIC_TEXT_LIMIT 384U
#define PROBE_INVERSE_SOURCE_LIMIT 4096U
#define PROBE_INVERSE_NAME_LIMIT 96U

/* Manual native protocol observation only. Preprocessing has no native
 * success flag; this probe grants no Class72, import or runtime certificate. */
static void print_bytes(const char* label, const void* bytes, size_t size) {
    printf("%s[%zu]=", label, size);
    if (size != 0U) fwrite(bytes, 1U, size, stdout);
    putchar('\n');
}

static void print_text(const char* label, const char* text) {
    print_bytes(label, text, strlen(text));
}

static void print_hash(const char* label, const void* bytes, size_t size) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, hex);
    printf("%s=%s bytes=%zu\n", label, hex, size);
}

static void print_digest(const char* label, const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(digest, hex);
    printf("%s=%s\n", label, hex);
}

static bool capture_request_provenance(UnityCompilerChannel* channel, const char* source,
                                       UnityCompilerToolchainProvenance* provenance) {
    if (!channel->cache_source_root) return false;
    size_t root_size = strlen(channel->cache_source_root) + 1U;
    char* source_root = malloc(root_size);
    if (!source_root) return false;
    memcpy(source_root, channel->cache_source_root, root_size);
    /* Refreshing a lease may replace the channel's root; do not lend that
     * owned pointer back to the provenance API. */
    bool captured = unity_compiler_get_request_provenance(channel, source_root, source, provenance);
    free(source_root);
    return captured;
}

static void print_request_provenance(const char* phase,
                                    const UnityCompilerToolchainProvenance* provenance,
                                    size_t transcript_size) {
    printf("request_provenance_phase=%s canonical_digest_matches=1 canonical_bytes=%zu "
           "source_authority_revision=%" PRIu64 "\n",
           phase, transcript_size, provenance->source_authority_revision);
    print_digest("request_compiler_fingerprint", provenance->compiler_fingerprint);
    print_digest("request_environment_fingerprint", provenance->environment_fingerprint);
    print_text("toolchain_contents", provenance->unity_contents_path);
    print_text("toolchain_compiler", provenance->compiler_path);
    print_text("toolchain_builtin_includes", provenance->builtin_includes_dir);
    print_text("toolchain_playback_engines", provenance->playback_engines_dir);
    print_text("toolchain_glslang", provenance->glslang_path);
    print_text("toolchain_dxcompiler", provenance->dxcompiler_path);
}

static bool verify_preprocess_provenance(
    UnityCompilerChannel* channel, const UnityCompilerComputePreprocessRequest* request,
    const uint8_t retained_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    UnityCompilerToolchainProvenance provenance;
    uint8_t* transcript = NULL;
    size_t transcript_size = 0U;
    uint8_t current_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    bool matches = capture_request_provenance(channel, request->source, &provenance) &&
                   unity_compiler_serialize_compute_preprocess_request(
                       channel, request, &transcript, &transcript_size, current_digest) &&
                   memcmp(current_digest, retained_digest, sizeof(current_digest)) == 0 &&
                   provenance.source_authority_revision == channel->source_authority_revision;
    free(transcript);
    if (matches) print_request_provenance("preprocess", &provenance, transcript_size);
    return matches;
}

static bool verify_compile_provenance(
    UnityCompilerChannel* channel, const UnityCompilerComputeKernelRequest* request,
    const uint8_t retained_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    UnityCompilerToolchainProvenance provenance;
    uint8_t* transcript = NULL;
    size_t transcript_size = 0U;
    uint8_t current_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    bool matches = capture_request_provenance(channel, request->source, &provenance) &&
                   unity_compiler_serialize_compute_request(
                       channel, request, &transcript, &transcript_size, current_digest) &&
                   memcmp(current_digest, retained_digest, sizeof(current_digest)) == 0 &&
                   provenance.source_authority_revision == channel->source_authority_revision;
    free(transcript);
    if (matches) print_request_provenance("compile", &provenance, transcript_size);
    return matches;
}

static size_t print_diagnostics(const UnityCompilerDiagnostic* diagnostics, size_t count) {
    size_t actionable = 0U;
    printf("diagnostics=%zu\n", count);
    for (size_t i = 0U; i < count; ++i) {
        bool reported = unity_compiler_diagnostic_is_actionable(&diagnostics[i]);
        actionable += reported ? 1U : 0U;
        printf("diagnostic[%zu] actionable=%d\n", i, reported ? 1 : 0);
        print_text("record", diagnostics[i].record);
        print_text("file", diagnostics[i].file);
        print_text("message", diagnostics[i].message);
    }
    return actionable;
}

static void print_diagnostic_text(const char* label, const char* value) {
    const size_t size = strlen(value);
    const size_t retained = size < PROBE_DIAGNOSTIC_TEXT_LIMIT ? size : PROBE_DIAGNOSTIC_TEXT_LIMIT;
    print_bytes(label, value, retained);
    if (retained != size) printf("diagnostic_text_truncated=%s bytes=%zu\n", label, size);
}

static size_t print_diagnostics_selected(const UnityCompilerDiagnostic* diagnostics,
                                         size_t count, bool decode) {
    if (!decode) return print_diagnostics(diagnostics, count);
    size_t actionable = 0U;
    printf("diagnostics=%zu\n", count);
    for (size_t index = 0U; index < count; ++index) {
        const bool reported = unity_compiler_diagnostic_is_actionable(&diagnostics[index]);
        actionable += reported ? 1U : 0U;
        if (index >= PROBE_DIAGNOSTIC_LIMIT) continue;
        printf("diagnostic[%zu] actionable=%d\n", index, reported ? 1 : 0);
        print_diagnostic_text("record", diagnostics[index].record);
        print_diagnostic_text("file", diagnostics[index].file);
        print_diagnostic_text("message", diagnostics[index].message);
    }
    if (count > PROBE_DIAGNOSTIC_LIMIT)
        printf("diagnostic_records_truncated=%zu\n", count - PROBE_DIAGNOSTIC_LIMIT);
    return actionable;
}

static const UnityCompilerComputePreprocessedKernel* print_preprocess(
    const UnityCompilerComputePreprocessResult* result, const char* selected_name) {
    printf("preprocess flags=0x%08" PRIx32 " requirements=0x%016" PRIx64
           " supported_apis=0x%08" PRIx32 " use_dxc=0x%08" PRIx32
           " never_use_dxc=0x%08" PRIx32 "\n",
           result->compilation_flags, result->requirements, (uint32_t)result->supported_apis,
           result->use_dxc_mask, result->never_use_dxc_mask);
    printf("include_hash_words=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
           result->include_hash_words[0], result->include_hash_words[1],
           result->include_hash_words[2], result->include_hash_words[3]);
    const UnityCompilerComputeKeywordLines* families[] = {&result->user_global, &result->user_local};
    for (size_t family = 0U; family < 2U; ++family) {
        printf("keyword_family=%s lines=%zu\n", family == 0U ? "global" : "local",
               families[family]->line_count);
        for (size_t i = 0U; i < families[family]->line_count; ++i)
            print_text("keyword_line", families[family]->lines[i]);
    }
    printf("dependencies=%zu kernels=%zu conditionals=%zu\n", result->dependency_count,
           result->kernel_count, result->conditional_requirement_count);
    for (size_t i = 0U; i < result->dependency_count; ++i)
        print_text("dependency", result->dependencies[i]);
    for (size_t i = 0U; i < result->conditional_requirement_count; ++i) {
        print_text("conditional_keyword", result->conditional_requirements[i].keyword);
        printf("conditional_requirements=0x%016" PRIx64 "\n",
               result->conditional_requirements[i].requirements);
    }
    const UnityCompilerComputePreprocessedKernel* selected = NULL;
    for (size_t i = 0U; i < result->kernel_count; ++i) {
        const UnityCompilerComputePreprocessedKernel* entry = &result->kernels[i];
        print_text("kernel", entry->name);
        printf("kernel_macros=%zu\n", entry->macro_count);
        for (size_t j = 0U; j < entry->macro_count; ++j) {
            print_text("macro_name", entry->macros[j].name);
            print_text("macro_value", entry->macros[j].value);
        }
        if (strcmp(entry->name, selected_name) == 0) selected = entry;
    }
    print_hash("returned_source_sha256", result->source, result->source_size);
    return selected;
}

static void print_resources(const char* role, const ComputeShaderResource* resources, size_t count) {
    printf("resource_role=%s count=%zu\n", role, count);
    for (size_t i = 0U; i < count; ++i) {
        print_bytes("name", resources[i].name.bytes, resources[i].name.size);
        print_bytes("generated_name", resources[i].generated_name.bytes, resources[i].generated_name.size);
        printf("binding=%" PRId32 " sampler=%" PRId32 " dimension=%" PRId32 "\n",
               resources[i].bind_point, resources[i].sampler_bind_point, resources[i].texture_dimension);
    }
}

static void print_native_declarations(const UnityComputeBinary* binary) {
    for (size_t i = 0U; i < binary->directive_count; ++i) {
        const UnityComputeKernelDirective* directive = &binary->directives[i];
        printf("native_directive[%zu] macros=%zu\n", i, directive->macro_count);
        print_bytes("name", directive->name.bytes, directive->name.size);
        for (size_t j = 0U; j < directive->macro_count; ++j) {
            printf("native_macro[%zu]\n", j);
            print_bytes("name", directive->macros[j].name.bytes, directive->macros[j].name.size);
            print_bytes("value", directive->macros[j].value.bytes, directive->macros[j].value.size);
        }
    }
    for (size_t i = 0U; i < binary->buffer_variant_count; ++i) {
        const UnityComputeBufferVariant* variant = &binary->buffer_variants[i];
        printf("native_buffer_variant[%zu] buffers=%zu\n", i, variant->buffer_count);
        for (size_t j = 0U; j < variant->buffer_count; ++j) {
            const ComputeShaderConstantBuffer* buffer = &variant->buffers[j];
            printf("native_cbuffer[%zu] byte_size=%" PRId32 " parameters=%zu\n",
                   j, buffer->byte_size, buffer->parameter_count);
            print_bytes("name", buffer->name.bytes, buffer->name.size);
            for (size_t k = 0U; k < buffer->parameter_count; ++k) {
                const ComputeShaderParameter* parameter = &buffer->parameters[k];
                printf("native_parameter[%zu] type=%" PRId32 " offset=%" PRIu32
                       " array_size=%" PRIu32 " rows=%" PRIu32 " columns=%" PRIu32 "\n",
                       k, parameter->type, parameter->offset, parameter->array_size,
                       parameter->row_count, parameter->column_count);
                print_bytes("name", parameter->name.bytes, parameter->name.size);
            }
        }
    }
}

static void print_kernel(const UnityComputeKernelBinary* kernel) {
    const ComputeShaderKernelVariant* data = &kernel->data;
    print_bytes("compiled_kernel", kernel->name.bytes, kernel->name.size);
    print_hash("complete_dxbc_sha256", data->code, data->code_size);
    printf("groups=%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
           data->thread_group_size[0], data->thread_group_size[1], data->thread_group_size[2]);
    print_resources("cb", data->constant_buffers, data->constant_buffer_count);
    print_resources("texture", data->textures, data->texture_count);
    print_resources("input_buffer", data->input_buffers, data->input_buffer_count);
    print_resources("output_buffer", data->output_buffers, data->output_buffer_count);
    printf("builtin_samplers=%zu\n", data->builtin_sampler_count);
    for (size_t i = 0U; i < data->builtin_sampler_count; ++i)
        printf("sampler=%" PRIu32 " binding=%" PRId32 "\n",
               data->builtin_samplers[i].sampler, data->builtin_samplers[i].bind_point);
}

static bool reserve_decode_records(size_t* remaining, size_t count) {
    if (count > *remaining) return false;
    *remaining -= count;
    return true;
}

static bool native_decode_counts_bounded(const UnityComputeBinary* binary) {
    size_t resources = PROBE_DECODE_LIMIT;
    size_t declarations = PROBE_DECODE_LIMIT;
    if (binary->kernel_count != 1U || !binary->kernels ||
        !reserve_decode_records(&declarations, binary->directive_count) ||
        !reserve_decode_records(&declarations, binary->buffer_variant_count)) return false;
    for (size_t index = 0U; index < binary->directive_count; ++index)
        if (!reserve_decode_records(&declarations, binary->directives[index].macro_count)) return false;
    for (size_t index = 0U; index < binary->buffer_variant_count; ++index) {
        const UnityComputeBufferVariant* variant = &binary->buffer_variants[index];
        if (!reserve_decode_records(&resources, variant->buffer_count)) return false;
        for (size_t buffer = 0U; buffer < variant->buffer_count; ++buffer)
            if (!reserve_decode_records(&declarations, variant->buffers[buffer].parameter_count)) return false;
    }
    const ComputeShaderKernelVariant* data = &binary->kernels[0].data;
    return reserve_decode_records(&resources, data->constant_buffer_count) &&
           reserve_decode_records(&resources, data->texture_count) &&
           reserve_decode_records(&resources, data->input_buffer_count) &&
           reserve_decode_records(&resources, data->output_buffer_count) &&
           reserve_decode_records(&resources, data->builtin_sampler_count);
}

static bool decoded_count_bounded(int count, int capacity, const void* records) {
    return count >= 0 && count <= (int)PROBE_DECODE_LIMIT && capacity >= count &&
           (count == 0 || records != NULL);
}

static bool print_decoded_operand(const DXBCOperand* operand, uint32_t raw_instruction,
                                  int operand_index, unsigned path, unsigned depth,
                                  size_t* remaining) {
    if (!operand || depth > PROBE_DECODE_DEPTH_LIMIT ||
        !reserve_decode_records(remaining, 1U) || operand->register_index_dim < 0 ||
        operand->register_index_dim > 3 || operand->imm_value_count < 0 ||
        operand->imm_value_count > 4 || operand->immediate_word_count < 0 ||
        operand->immediate_word_count > 8) return false;
    printf("compute_operand raw_instruction=%" PRIu32 " operand=%d path=%u depth=%u "
           "type=%u raw_token=0x%08" PRIx32 " register=%d dimensions=%d mode=%u "
           "destination_mask_raw=%u components=%u,%u,%u,%u neg=%d abs=%d "
           "precision=%u extended_tokens=%zu literal_values=%d literal_words=%d\n",
           raw_instruction, operand_index, path, depth, (unsigned)operand->type,
           operand->raw_token, operand->register_index, operand->register_index_dim,
           (unsigned)operand->swizzle_mode, (unsigned)operand->destination_mask,
           (unsigned)operand->swizzle[0], (unsigned)operand->swizzle[1],
           (unsigned)operand->swizzle[2], (unsigned)operand->swizzle[3],
           operand->has_neg, operand->has_abs, (unsigned)operand->min_precision,
           operand->extended_token_count, operand->imm_value_count, operand->immediate_word_count);
    for (int dimension = 0; dimension < operand->register_index_dim; ++dimension)
        printf("compute_operand_index raw_instruction=%" PRIu32 " operand=%d path=%u "
               "dimension=%d representation=%u immediate=%d value=%" PRIu64 " exceeds_int=%d\n",
               raw_instruction, operand_index, path, dimension,
               (unsigned)operand->index_representations[dimension],
               operand->index_has_immediate[dimension], operand->index_values[dimension],
               operand->index_value_exceeds_int[dimension]);
    for (int word = 0; word < operand->immediate_word_count; ++word)
        printf("compute_operand_literal raw_instruction=%" PRIu32 " operand=%d path=%u "
               "word=%d bits=0x%08" PRIx32 "\n", raw_instruction, operand_index,
               path, word, operand->immediate_words[word]);
    const DXBCOperand* relative[] = {operand->rel_op0, operand->rel_op1, operand->rel_op2};
    for (unsigned dimension = 0U; dimension < 3U; ++dimension)
        if (relative[dimension] && !print_decoded_operand(relative[dimension], raw_instruction,
                operand_index, path * 4U + dimension + 1U, depth + 1U, remaining)) return false;
    return true;
}

static const DXBCDocumentInstruction* decoded_raw_owner(const DXBCDocument* document,
                                                       const DXBCInstruction* semantic) {
    if (!semantic->has_raw_instruction_index) return NULL;
    for (size_t index = 0U; index < document->instruction_count; ++index) {
        const DXBCDocumentInstruction* raw = &document->instructions[index];
        if (raw->instruction_index == semantic->raw_instruction_index &&
            raw->byte_offset == semantic->file_offset && raw->byte_size == semantic->byte_length &&
            raw->opcode == semantic->opcode) return raw;
    }
    return NULL;
}

static bool print_decoded_resources(const DXBCContainer* semantic) {
    size_t remaining = PROBE_DECODE_LIMIT;
    if (!decoded_count_bounded(semantic->resource_count, semantic->resource_alloc, semantic->resources) ||
        !decoded_count_bounded(semantic->uav_count, semantic->uav_alloc, semantic->uavs) ||
        !reserve_decode_records(&remaining, (size_t)semantic->resource_count) ||
        !reserve_decode_records(&remaining, (size_t)semantic->uav_count)) return false;
    for (unsigned role = 0U; role < 2U; ++role) {
        const DXBCResourceDecl* records = role == 0U ? semantic->resources : semantic->uavs;
        const int count = role == 0U ? semantic->resource_count : semantic->uav_count;
        for (int index = 0; index < count; ++index) {
            const DXBCResourceDecl* resource = &records[index];
            printf("compute_resource role=%s index=%d register=%d declared=%d dimension=%" PRIu32
                   " return_types=%u,%u,%u,%u samples=%" PRIu32 " stride=%" PRIu32
                   " structured=%d coherent=%d rasterizer_ordered=%d counter=%d\n",
                   role == 0U ? "srv" : "uav", index, resource->register_index, resource->declared,
                   resource->dimension, (unsigned)resource->return_types[0],
                   (unsigned)resource->return_types[1], (unsigned)resource->return_types[2],
                   (unsigned)resource->return_types[3], resource->sample_count, resource->stride,
                   resource->is_structured, resource->globally_coherent, resource->rasterizer_ordered,
                   resource->has_order_preserving_counter);
        }
    }
    return true;
}

static bool print_decoded_memory(const USILProgram* program, const USILInstruction* instruction,
                                 int index) {
    if (!usil_opcode_has_memory_access(instruction->opcode)) return true;
    USILMemoryAccess access;
    if (!usil_instruction_memory_access(program, instruction, &access)) return false;
    printf("compute_memory instruction=%d raw_instruction=%" PRIu32 " kind=%u space=%u "
           "register=%" PRIu32 " dimension=%.16s stride=%" PRIu32 " shared_bytes=%" PRIu32
           " return_types=%u,%u,%u,%u reads=%d writes=%d atomic=%d coherent=%d "
           "rasterizer_ordered=%d counter=%d counter_mode=%u destination_operand=%d "
           "binding_operand=%d address_operand=%d byte_offset_operand=%d value_operand=%d "
           "compare_operand=%d destination_lanes=%u address_lanes=%u value_lanes=%u "
           "memory_component_lanes=%u\n",
           index, instruction->source_instruction_index, (unsigned)access.kind, (unsigned)access.space,
           access.register_id, access.dimension, access.byte_stride, access.shared_memory_byte_count,
           (unsigned)access.return_types[0], (unsigned)access.return_types[1],
           (unsigned)access.return_types[2], (unsigned)access.return_types[3],
           access.reads, access.writes, access.atomic, access.globally_coherent,
           access.rasterizer_ordered, access.has_order_preserving_counter, (unsigned)access.counter_mode,
           access.destination_operand, access.binding_operand, access.address_operand,
           access.byte_offset_operand, access.value_operand, access.compare_operand,
           (unsigned)access.destination_lanes, (unsigned)access.address_lanes,
           (unsigned)access.value_lanes, (unsigned)access.memory_component_lanes);
    return true;
}

/* The inverse path borrows the original complete native payload until all
 * three observations finish. It never creates a player object or changes the
 * expected code. Generated text is a canonical current semantic representation,
 * not a recovery claim about the authored source spelling. */
typedef struct {
    UnityCompilerChannel* channel;
    const UnityCompilerComputePreprocessRequest* preprocess_request;
    const UnityCompilerComputePreprocessInfo* preprocess_info;
    const UnityCompilerComputePreprocessResult* preprocess_result;
    const UnityCompilerComputePreprocessedKernel* selected;
    const UnityCompilerComputeKernelRequest* compile_request;
    const UnityCompilerBinaryResponse* original_response;
    const UnityComputeBinary* original_binary;
    UnityCompilerToolchainProvenance lease;
    pid_t process_id;
    char entry_name[PROBE_INVERSE_NAME_LIMIT];
    char resource_name[PROBE_INVERSE_NAME_LIMIT];
    uint32_t mutation_raw_owner;
    uint32_t original_value_bits;
    uint32_t changed_value_bits;
} ProbeInverseContext;

typedef struct {
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t preprocess_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t compile_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    size_t source_size;
} ProbeInverseColdIdentity;

static bool inverse_identifier(const uint8_t* value, size_t size) {
    if (!value || size == 0U || size >= PROBE_INVERSE_NAME_LIMIT) return false;
    for (size_t index = 0U; index < size; ++index) {
        const unsigned char c = (unsigned char)value[index];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && (index == 0U || c < '0' || c > '9')) return false;
    }
    return true;
}

static bool inverse_string_view_equal(ComputeShaderStringView left, ComputeShaderStringView right) {
    return left.size == right.size && (left.size == 0U ||
        (left.bytes && right.bytes && memcmp(left.bytes, right.bytes, left.size) == 0));
}

static bool inverse_native_shape(const UnityComputeBinary* binary) {
    if (!binary->decoded || binary->kernel_count != 1U || !binary->kernels ||
        binary->directive_count != 0U || binary->buffer_variant_count != 1U ||
        !binary->buffer_variants || binary->buffer_variants[0].buffer_count != 0U) return false;
    const ComputeShaderKernelVariant* kernel = &binary->kernels[0].data;
    return kernel->constant_buffer_count == 0U && kernel->texture_count == 0U &&
        kernel->input_buffer_count == 0U && kernel->output_buffer_count == 1U &&
        kernel->output_buffers && kernel->builtin_sampler_count == 0U &&
        kernel->constant_buffer_variant_index_count == 0U &&
        kernel->thread_group_size_count == 3U && kernel->thread_group_size && kernel->code &&
        kernel->code_size > 0U && kernel->code_size <= PROBE_INVERSE_SOURCE_LIMIT &&
        binary->kernels[0].name.size < PROBE_INVERSE_NAME_LIMIT &&
        kernel->output_buffers[0].name.size < PROBE_INVERSE_NAME_LIMIT &&
        kernel->output_buffers[0].generated_name.size < PROBE_INVERSE_NAME_LIMIT;
}

/* A deliberately tiny native atomic metadata observation. The shared verifier
 * returns at the byte mismatch before metadata comparison, so warm mutation
 * keeps that original-target report and checks this admitted tuple separately. */
static bool inverse_native_metadata_equal(const UnityComputeBinary* original,
                                          const UnityComputeBinary* current) {
    if (!inverse_native_shape(original) || !inverse_native_shape(current) ||
        original->target_level != current->target_level ||
        original->resources_resolved != current->resources_resolved ||
        !inverse_string_view_equal(original->kernels[0].name, current->kernels[0].name)) return false;
    const ComputeShaderKernelVariant* left = &original->kernels[0].data;
    const ComputeShaderKernelVariant* right = &current->kernels[0].data;
    const ComputeShaderResource* a = &left->output_buffers[0];
    const ComputeShaderResource* b = &right->output_buffers[0];
    return memcmp(left->thread_group_size, right->thread_group_size, 3U * sizeof(uint32_t)) == 0 &&
        inverse_string_view_equal(a->name, b->name) &&
        inverse_string_view_equal(a->generated_name, b->generated_name) &&
        a->bind_point == b->bind_point && a->sampler_bind_point == b->sampler_bind_point &&
        a->texture_dimension == b->texture_dimension;
}

static bool inverse_actual_groups_valid(const ComputeShaderKernelVariant* actual) {
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContractDiagnostic stage_diagnostic;
    bool valid = actual->thread_group_size_count == 3U && actual->thread_group_size &&
        dxbc_document_parse(&document, actual->code, actual->code_size, &document_diagnostic) &&
        dxbc_document_decode_semantic(&document, &semantic) &&
        dxbc_stage_contract_decode(&document, &semantic, &contract, &stage_diagnostic) &&
        contract.program_type == DXBC_PROGRAM_TYPE_COMPUTE && contract.has_thread_group_size &&
        memcmp(contract.thread_group_size, actual->thread_group_size, sizeof(contract.thread_group_size)) == 0;
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    return valid;
}

static bool inverse_same_lease(const ProbeInverseContext* context, const char* source) {
    UnityCompilerToolchainProvenance current;
    return capture_request_provenance(context->channel, source, &current) &&
        memcmp(context->lease.compiler_fingerprint, current.compiler_fingerprint,
               UNITY_COMPILER_FINGERPRINT_SIZE) == 0 &&
        memcmp(context->lease.environment_fingerprint, current.environment_fingerprint,
               UNITY_COMPILER_FINGERPRINT_SIZE) == 0 &&
        context->lease.source_authority_revision == current.source_authority_revision &&
        context->channel->source_authority_revision == current.source_authority_revision &&
        context->process_id == context->channel->process_id;
}

static bool inverse_lines_equal(const UnityCompilerComputeKeywordLines* left,
                                const UnityCompilerComputeKeywordLines* right) {
    if (left->line_count > PROBE_DECODE_LIMIT || left->line_count != right->line_count ||
        (left->line_count && (!left->lines || !right->lines))) return false;
    for (size_t index = 0U; index < left->line_count; ++index)
        if (!left->lines[index] || !right->lines[index] ||
            strcmp(left->lines[index], right->lines[index]) != 0) return false;
    return true;
}

static bool inverse_preprocess_controls_equal(const ProbeInverseContext* context,
    const UnityCompilerComputePreprocessResult* current,
    const UnityCompilerComputePreprocessedKernel** selected) {
    *selected = NULL;
    const UnityCompilerComputePreprocessResult* original = context->preprocess_result;
    if (!current || original->kernel_count != 1U || current->kernel_count != 1U ||
        !current->kernels || !current->source || current->source_size > PROBE_INVERSE_SOURCE_LIMIT ||
        original->compilation_flags != current->compilation_flags ||
        original->requirements != current->requirements || original->supported_apis != current->supported_apis ||
        original->use_dxc_mask != current->use_dxc_mask ||
        original->never_use_dxc_mask != current->never_use_dxc_mask ||
        !inverse_lines_equal(&original->user_global, &current->user_global) ||
        !inverse_lines_equal(&original->user_local, &current->user_local) ||
        original->conditional_requirement_count > PROBE_DECODE_LIMIT ||
        original->conditional_requirement_count != current->conditional_requirement_count ||
        (original->conditional_requirement_count &&
            (!original->conditional_requirements || !current->conditional_requirements))) return false;
    for (size_t index = 0U; index < original->conditional_requirement_count; ++index) {
        const UnityCompilerComputeConditionalRequirement* a = &original->conditional_requirements[index];
        const UnityCompilerComputeConditionalRequirement* b = &current->conditional_requirements[index];
        if (!a->keyword || !b->keyword || strcmp(a->keyword, b->keyword) != 0 ||
            a->requirements != b->requirements) return false;
    }
    const UnityCompilerComputePreprocessedKernel* kernel = &current->kernels[0];
    if (!kernel->name || strcmp(kernel->name, context->selected->name) != 0 ||
        kernel->macro_count != context->selected->macro_count ||
        kernel->macro_count > PROBE_DECODE_LIMIT ||
        (kernel->macro_count && (!kernel->macros || !context->selected->macros))) return false;
    for (size_t index = 0U; index < kernel->macro_count; ++index) {
        const UnityCompilerComputePreprocessMacro* a = &context->selected->macros[index];
        const UnityCompilerComputePreprocessMacro* b = &kernel->macros[index];
        if (!a->name || !b->name || !a->value || !b->value ||
            strcmp(a->name, b->name) != 0 || strcmp(a->value, b->value) != 0) return false;
    }
    *selected = kernel;
    return true;
}

static bool inverse_source(USILProgram* program, const ProbeInverseContext* context,
                           StringBuilder* source) {
    const ComputeShaderResource* native_resource = &context->original_binary->kernels[0].data.output_buffers[0];
    if (native_resource->bind_point < 0 || program->uav_count != 1 || !program->uavs ||
        program->uavs[0].reg_idx != native_resource->bind_point) return false;
    const HLSLComputeTypedResource resource = {.name = context->resource_name,
        .binding_register = (uint32_t)native_resource->bind_point, .writable = true,
        .scalar_type = AST_SCALAR_UINT32, .scalar_atomic = true};
    HLSLComputeTypedSource typed = {.resources = &resource, .resource_count = 1U, .emit_declarations = true};
    HLSLSourceQualityResult quality = {.classification = HLSL_SOURCE_QUALITY_FAILED};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.source_quality = &quality;
    const HLSLEmitNames names = {.entry_point = context->entry_name};
    HLSLEmitDiagnostic diagnostic;
    hlsl_emit_diagnostic_init(&diagnostic);
    sb_appendf(source, "#pragma kernel %s", context->entry_name);
    for (size_t index = 0U; index < context->selected->macro_count; ++index) {
        const UnityCompilerComputePreprocessMacro* macro = &context->selected->macros[index];
        if (!macro->name || !macro->value ||
            !inverse_identifier((const uint8_t*)macro->name, strlen(macro->name)) ||
            strlen(macro->value) >= PROBE_INVERSE_NAME_LIMIT) return false;
        for (const char* c = macro->value; *c; ++c)
            if ((unsigned char)*c <= 32U || *c == '#' || *c == '\\') return false;
        sb_appendf(source, " %s%s%s", macro->name, macro->value[0] ? "=" : "", macro->value);
    }
    sb_append(source, "\n#pragma only_renderers d3d11\n");
    const bool emitted = hlsl_emit_compute_typed_stage(program, source, &names, &options, &typed, &diagnostic);
    printf("inverse_declared_stage emitted=%d status=%u quality=%s reasons=0x%08" PRIx32
           " incomplete_units=%zu residual=%zu unknown=%zu\n", emitted, (unsigned)diagnostic.status,
           hlsl_source_quality_class_name(quality.classification), quality.reasons,
           quality.counts.incomplete_units, quality.counts.residual_total, quality.counts.unknown_provenance);
    if (!emitted || !sb_ok(source) || source->len > PROBE_INVERSE_SOURCE_LIMIT ||
        quality.classification != HLSL_SOURCE_QUALITY_CLEAN) return false;
    StringBuilder body;
    sb_init(&body);
    typed.emit_declarations = false;
    quality.classification = HLSL_SOURCE_QUALITY_FAILED;
    const bool entry_emitted = hlsl_emit_compute_typed_stage(program, &body, &names, &options, &typed, &diagnostic);
    printf("inverse_entry emitted=%d quality=%s reasons=0x%08" PRIx32
           " incomplete_units=%zu residual=%zu unknown=%zu\n", entry_emitted,
           hlsl_source_quality_class_name(quality.classification), quality.reasons,
           quality.counts.incomplete_units, quality.counts.residual_total, quality.counts.unknown_provenance);
    const bool complete = entry_emitted && sb_ok(&body) && body.len <= PROBE_INVERSE_SOURCE_LIMIT &&
        quality.classification == HLSL_SOURCE_QUALITY_CLEAN;
    sb_free(&body);
    return complete;
}

static bool inverse_observe(USILProgram* program, const ProbeInverseContext* context,
                            unsigned cycle, ProbeInverseColdIdentity* cold) {
    static const char* const phases[] = {"cold", "warm-mutation", "warm-restoration"};
    StringBuilder source;
    sb_init(&source);
    UnityCompilerComputePreprocessResponse* preprocess = NULL;
    UnityCompilerBinaryResponse response;
    unity_compiler_binary_response_init(&response);
    UnityComputeBinary binary;
    unity_compute_binary_init(&binary);
    bool complete = false;
    const char* phase = "source";
    if (cycle >= 3U || !inverse_source(program, context, &source)) goto done;
    printf("inverse_cycle=%s\n", phases[cycle]);
    print_hash("inverse_generated_source_sha256", source.buf, source.len);
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source.buf, source.len, source_digest);
    UnityCompilerComputePreprocessRequest preprocess_request = *context->preprocess_request;
    preprocess_request.source = source.buf;
    phase = "preprocess";
    if (!unity_compiler_preprocess_compute_response(context->channel, &preprocess_request, &preprocess)) goto done;
    UnityCompilerComputePreprocessInfo info;
    size_t diagnostic_count = 0U;
    const UnityCompilerDiagnostic* diagnostics =
        unity_compiler_compute_preprocess_response_diagnostics(preprocess, &diagnostic_count);
    if (!unity_compiler_compute_preprocess_response_info(preprocess, &info) ||
        !info.transport_complete || info.native_success_present || !info.has_request_identity ||
        info.availability != UNITY_COMPILER_RESPONSE_AVAILABLE ||
        info.valid_apis_authority.status != UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED ||
        info.valid_apis_authority.expected_valid_apis != context->preprocess_request->valid_apis ||
        info.valid_apis_authority.observed_valid_apis != context->preprocess_request->valid_apis ||
        print_diagnostics_selected(diagnostics, diagnostic_count, true) != 0U ||
        memcmp(info.controls_digest, context->preprocess_info->controls_digest,
               UNITY_COMPILER_FINGERPRINT_SIZE) != 0 ||
        !verify_preprocess_provenance(context->channel, &preprocess_request, info.request_digest) ||
        !inverse_same_lease(context, source.buf)) goto done;
    const UnityCompilerComputePreprocessResult* result =
        unity_compiler_compute_preprocess_response_result(preprocess);
    const UnityCompilerComputePreprocessedKernel* selected = NULL;
    phase = "preprocess-controls";
    const bool controls_equal = inverse_preprocess_controls_equal(context, result, &selected);
    printf("inverse_preprocess_controls_equal=%d\n", controls_equal);
    if (!controls_equal) goto done;
    UnityCompilerComputeMacro macros[PROBE_DECODE_LIMIT] = {{0}};
    for (size_t index = 0U; index < selected->macro_count; ++index) {
        macros[index].name = selected->macros[index].name;
        macros[index].value = selected->macros[index].value;
    }
    UnityCompilerComputeKernelRequest request = *context->compile_request;
    request.source = result->source;
    request.kernel_name = selected->name;
    request.kernel_macros = selected->macro_count ? macros : NULL;
    request.kernel_macro_count = (int)selected->macro_count;
    phase = "compile";
    const bool compile_transport = unity_compiler_compile_compute_response(context->channel, &request, &response);
    printf("inverse_compile transport=%d success=%d availability=%u payload_bytes=%zu\n",
           compile_transport, response.status.compiler_success, (unsigned)response.status.availability, response.size);
    const bool compile_diagnostics_clean = print_diagnostics_selected(response.status.diagnostics,
        response.status.diagnostic_count, true) == 0U;
    if (!compile_transport || !compile_diagnostics_clean || !response.has_request_identity ||
        !unity_compiler_response_status_is_clean_success(&response.status) ||
        response.size > PROBE_INVERSE_SOURCE_LIMIT * 4U ||
        memcmp(response.controls_digest, context->original_response->controls_digest,
               UNITY_COMPILER_FINGERPRINT_SIZE) != 0 ||
        !verify_compile_provenance(context->channel, &request, response.request_digest) ||
        !inverse_same_lease(context, request.source)) goto done;
    print_digest("inverse_preprocess_request_sha256", info.request_digest);
    print_digest("inverse_compile_request_sha256", response.request_digest);
    print_digest("inverse_compile_controls_sha256", response.controls_digest);
    phase = "native-decode";
    if (unity_compute_binary_decode(&binary, response.data, response.size) != COMPUTE_SHADER_OBJECT_OK ||
        !inverse_native_shape(&binary)) goto done;
    const ComputeShaderKernelVariant* actual = &binary.kernels[0].data;
    print_hash("inverse_complete_dxbc_sha256", actual->code, actual->code_size);
    phase = "comparison";
    const UnityComputeKernelExpectation expectation = {
        .kernel_name = context->original_binary->kernels[0].name,
        .variant = &context->original_binary->kernels[0].data,
        .target_level = context->original_binary->target_level,
        .resources_resolved = context->original_binary->resources_resolved};
    UnityComputeVerifyReport report;
    const UnityComputeVerifyStatus status =
        unity_compute_verify_kernel(&expectation, response.data, response.size, &report);
    DXBCCompareResult validation;
    const bool container_valid = dxbc_compare_exact(actual->code, actual->code_size,
        actual->code, actual->code_size, &validation) == DXBC_COMPARE_EQUAL;
    const bool bounded_metadata_equal = inverse_native_metadata_equal(context->original_binary, &binary);
    const bool actual_groups_valid = inverse_actual_groups_valid(actual);
    printf("inverse_comparison=%s dxbc_equal=%d difference=%s instruction=%" PRIu32
           " token=%" PRIu32 " shared_metadata_compared=%d shared_metadata_equal=%d "
           "bounded_atomic_metadata_equal=%d complete_container_valid=%d actual_groups_valid=%d\n",
           unity_compute_verify_status_name(status), report.dxbc_equal,
           dxbc_compare_status_name(report.dxbc.status), report.dxbc.instruction_index,
           report.dxbc.token_index, report.common_metadata_compared, report.common_metadata_equal,
           bounded_metadata_equal, container_valid, actual_groups_valid);
    if (!container_valid || !bounded_metadata_equal || !actual_groups_valid) goto done;
    if (cycle == 1U) {
        complete = status == UNITY_COMPUTE_VERIFY_DXBC_MISMATCH && report.dxbc_compared &&
            !report.dxbc_equal && report.dxbc.status == DXBC_COMPARE_INSTRUCTION_TOKEN &&
            report.dxbc.instruction_index == context->mutation_raw_owner && report.dxbc.token_index != UINT32_MAX &&
            report.dxbc.expected_value == context->original_value_bits &&
            report.dxbc.actual_value == context->changed_value_bits &&
            memcmp(source_digest, cold->source_digest, sizeof(source_digest)) != 0 &&
            memcmp(response.request_digest, cold->compile_digest, sizeof(cold->compile_digest)) != 0;
    } else {
        complete = status == UNITY_COMPUTE_VERIFY_OK;
        if (cycle == 0U && complete) {
            memcpy(cold->source_digest, source_digest, sizeof(source_digest));
            memcpy(cold->preprocess_digest, info.request_digest, sizeof(cold->preprocess_digest));
            memcpy(cold->compile_digest, response.request_digest, sizeof(cold->compile_digest));
            cold->source_size = source.len;
        } else if (cycle == 2U) {
            complete = complete && source.len == cold->source_size &&
                memcmp(source_digest, cold->source_digest, sizeof(source_digest)) == 0 &&
                memcmp(info.request_digest, cold->preprocess_digest, sizeof(cold->preprocess_digest)) == 0 &&
                memcmp(response.request_digest, cold->compile_digest, sizeof(cold->compile_digest)) == 0;
        }
    }
done:
    printf("inverse_cycle_complete=%d cycle=%u phase=%s\n", complete, cycle, complete ? "none" : phase);
    unity_compute_binary_dispose(&binary);
    unity_compiler_binary_response_free(&response);
    unity_compiler_compute_preprocess_response_free(preprocess);
    sb_free(&source);
    return complete;
}

static bool qualify_atomic_inverse(USILProgram* program, ProbeInverseContext* context) {
    puts("inverse_scope=isolated-compute-stage exact-selected-controls; authored-source-text=unknown "
         "Class72=not-certified import=not-run semantic-runtime=not-run physical-Windows=not-run");
    if (!inverse_native_shape(context->original_binary) || !program->instructions ||
        context->selected->macro_count > PROBE_DECODE_LIMIT) return false;
    const bool no_result = program->instruction_count == 2 &&
        program->instructions[0].opcode == USIL_OP_ATOMIC_IADD &&
        program->instructions[1].opcode == USIL_OP_RET;
    const bool returning = program->instruction_count == 3 &&
        program->instructions[0].opcode == USIL_OP_IMM_ATOMIC_IADD &&
        program->instructions[1].opcode == USIL_OP_STORE_UAV_TYPED &&
        program->instructions[2].opcode == USIL_OP_RET;
    /* This hint selects the measurement shape only. The shared typed producer
     * still proves every return definition, store consumer and operand owner. */
    if (!no_result && !returning) return false;
    const UnityComputeKernelBinary* native = &context->original_binary->kernels[0];
    const ComputeShaderResource* resource = &native->data.output_buffers[0];
    if (!inverse_identifier(native->name.bytes, native->name.size) ||
        !inverse_identifier(resource->name.bytes, resource->name.size)) return false;
    memcpy(context->entry_name, native->name.bytes, native->name.size);
    memcpy(context->resource_name, resource->name.bytes, resource->name.size);
    USILInstruction* atomic = &program->instructions[0];
    USILMemoryAccess access;
    if (!usil_instruction_memory_access(program, atomic, &access) || !access.atomic ||
        access.destination_operand != (returning ? 0 : -1) || access.value_operand < 0 ||
        access.value_operand >= atomic->operand_count || access.value_lanes != 1U ||
        access.memory_component_lanes != 1U) return false;
    DXBCOperand* value = &atomic->operands[access.value_operand];
    if (value->type != OPERAND_TYPE_IMMEDIATE32 || value->imm_value_count != 1 ||
        value->immediate_word_count != 1 || value->imm_values[0] != value->immediate_words[0] ||
        value->rel_op0 || value->rel_op1 || value->rel_op2) return false;
    const uint32_t original_bits = value->imm_values[0];
    const uint32_t changed_bits = original_bits == 3U ? 1U : 3U;
    const DXBCOperand saved = *value;
    context->mutation_raw_owner = atomic->source_instruction_index;
    context->original_value_bits = original_bits;
    context->changed_value_bits = changed_bits;
    uint8_t target_digest[COMMON_SHA256_DIGEST_SIZE], restored_target_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(native->data.code, native->data.code_size, target_digest);
    printf("inverse_mutation_owner instruction=0 raw_instruction=%" PRIu32 " operand=%d "
           "demanded_lanes=%u original_bits=0x%08" PRIx32 " changed_bits=0x%08" PRIx32
           " opcode=%u name=%s returning=%d\n",
           atomic->source_instruction_index, access.value_operand, (unsigned)access.value_lanes,
           original_bits, changed_bits, (unsigned)atomic->opcode,
           hlsl_emit_opcode_name((int)atomic->opcode), returning);
    print_hash("inverse_original_value_bits_sha256", saved.immediate_words, sizeof(uint32_t));
    if (!capture_request_provenance(context->channel, context->compile_request->source, &context->lease))
        return false;
    context->process_id = context->channel->process_id;
    ProbeInverseColdIdentity cold = {0};
    bool complete = inverse_observe(program, context, 0U, &cold);
    if (complete) {
        value->imm_values[0] = changed_bits;
        value->immediate_words[0] = changed_bits;
        complete = inverse_observe(program, context, 1U, &cold);
        *value = saved;
        const bool restoration = inverse_observe(program, context, 2U, &cold);
        complete = complete && restoration;
    } else {
        puts("inverse_warm_mutation=not-run inverse_warm_restoration=not-run");
    }
    *value = saved;
    common_sha256(native->data.code, native->data.code_size, restored_target_digest);
    const bool original_held = memcmp(target_digest, restored_target_digest, sizeof(target_digest)) == 0;
    printf("inverse_original_target_unchanged=%d inverse_complete=%d process=%jd "
           "source_authority_revision=%" PRIu64 "\n", original_held, complete && original_held,
           (intmax_t)context->process_id, context->lease.source_authority_revision);
    return complete && original_held;
}

/* This observation reads the original native kernel bytes only. Raw owners
 * are printed before lowering so an unsupported USIL grammar remains visible.
 * None of these records grants source, Class72, import or runtime authority. */
static bool print_decoded_kernel(const ComputeShaderKernelVariant* native, ProbeInverseContext* inverse) {
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    USILProgram program = {0};
    DXBCDocumentDiagnostic document_diagnostic = {0};
    DXBCStageContractDiagnostic stage_diagnostic = {0};
    const char* phase = "document";
    bool complete = false;
    bool inverse_complete = inverse == NULL;
    if (!dxbc_document_parse(&document, native->code, native->code_size, &document_diagnostic)) {
        printf("compute_decode_document status=%s offset=%zu raw_instruction=%" PRIu32 "\n",
               dxbc_document_diagnostic_code_name(document_diagnostic.code),
               document_diagnostic.byte_offset, document_diagnostic.instruction_index);
        goto done;
    }
    phase = "raw-instruction-limit";
    if (document.instruction_count > PROBE_DECODE_LIMIT ||
        document.instruction_capacity < document.instruction_count ||
        (document.instruction_count && !document.instructions)) goto done;
    for (size_t index = 0U; index < document.instruction_count; ++index) {
        const DXBCDocumentInstruction* raw = &document.instructions[index];
        uint32_t token = 0U;
        if (!dxbc_document_instruction_token(raw, 0U, &token)) goto done;
        printf("compute_raw_instruction index=%zu raw_instruction=%" PRIu32 " chunk=%" PRIu32
               " opcode=%" PRIu32 " token=0x%08" PRIx32 " offset=%zu bytes=%zu words=%" PRIu32
               " extensions=%" PRIu32 " customdata=%d\n", index, raw->instruction_index,
               raw->chunk_index, raw->opcode, token, raw->byte_offset, raw->byte_size,
               raw->token_count, raw->extended_opcode_token_count, raw->uses_customdata_length);
    }
    phase = "semantic";
    if (!dxbc_document_decode_semantic(&document, &semantic)) goto done;
    phase = "semantic-instruction-limit";
    if (!decoded_count_bounded(semantic.instruction_count, semantic.instruction_alloc, semantic.instructions))
        goto done;
    phase = "resource-limit";
    if (!print_decoded_resources(&semantic)) goto done;
    size_t remaining_operands = PROBE_DECODE_LIMIT;
    phase = "semantic-owner-or-operand-limit";
    for (int index = 0; index < semantic.instruction_count; ++index) {
        const DXBCInstruction* instruction = &semantic.instructions[index];
        const DXBCDocumentInstruction* raw = decoded_raw_owner(&document, instruction);
        if (!raw || instruction->operand_count < 0 || instruction->operand_count > DXBC_MAX_OPERANDS)
            goto done;
        printf("compute_semantic_instruction index=%d raw_instruction=%" PRIu32
               " opcode=%" PRIu32 " declaration=%d operands=%d dimension_present=%d dimension=%" PRIu32
               " return_types_present=%d return_types=%u,%u,%u,%u stride=%d "
               "saturate=%d precise=%u condition=%u\n", index, instruction->raw_instruction_index,
               instruction->opcode, instruction->is_decl, instruction->operand_count,
               instruction->has_resource_dimension, instruction->resource_dimension,
               instruction->has_resource_return_types, (unsigned)instruction->resource_return_types[0],
               (unsigned)instruction->resource_return_types[1], (unsigned)instruction->resource_return_types[2],
               (unsigned)instruction->resource_return_types[3], instruction->structured_stride,
               instruction->saturate, (unsigned)instruction->precise_mask, (unsigned)instruction->condition_test);
        for (int operand = 0; operand < instruction->operand_count; ++operand)
            if (!print_decoded_operand(&instruction->operands[operand], instruction->raw_instruction_index,
                    operand, 0U, 0U, &remaining_operands)) goto done;
    }
    phase = "stage-contract";
    if (!dxbc_stage_contract_decode(&document, &semantic, &contract, &stage_diagnostic)) {
        printf("compute_decode_contract status=%s raw_instruction=%" PRIu32 " opcode=%" PRIu32 "\n",
               dxbc_stage_contract_status_name(stage_diagnostic.status), stage_diagnostic.instruction_index,
               stage_diagnostic.opcode);
        goto done;
    }
    bool groups_match = contract.program_type == DXBC_PROGRAM_TYPE_COMPUTE && contract.has_thread_group_size;
    for (unsigned axis = 0U; axis < 3U; ++axis)
        if (contract.thread_group_size[axis] != native->thread_group_size[axis]) groups_match = false;
    printf("compute_contract stage=%u model=%u.%u groups=%" PRIu32 ",%" PRIu32 ",%" PRIu32
           " declaration_owner=%" PRIu32 " native_groups_equal=%d shared_bytes=%" PRIu32
           " shared_records=%zu barriers=%zu\n", (unsigned)contract.program_type,
           (unsigned)contract.shader_model_major, (unsigned)contract.shader_model_minor,
           contract.thread_group_size[0], contract.thread_group_size[1], contract.thread_group_size[2],
           contract.thread_group_declaration_instruction_index, groups_match,
           contract.thread_group_shared_memory_bytes, contract.thread_group_shared_memory_count,
           contract.memory_barrier_count);
    phase = "native-group-tuple";
    if (!groups_match) goto done;
    phase = "usil";
    if (!usil_translate_with_stage_contract(&program, &semantic, &contract)) goto done;
    phase = "usil-count-limit";
    if (!decoded_count_bounded(program.instruction_count, program.instruction_alloc, program.instructions) ||
        !decoded_count_bounded(program.texture_count, program.texture_alloc, program.textures) ||
        !decoded_count_bounded(program.uav_count, program.uav_alloc, program.uavs)) goto done;
    printf("compute_usil instructions=%d temps=%d stage_contract=%d compute_valid=%d "
           "signature_authority=%d system_values=%u srvs=%d uavs=%d shared_records=%zu barriers=%zu\n",
           program.instruction_count, program.temp_count, program.has_stage_contract, program.compute.valid,
           program.has_parsed_signature_authority, (unsigned)program.compute.system_value_mask,
           program.texture_count, program.uav_count, program.compute.shared_memory_count,
           program.compute.barrier_count);
    phase = "usil-instruction-authority";
    for (int index = 0; index < program.instruction_count; ++index) {
        const USILInstruction* instruction = &program.instructions[index];
        USILEffectFlags effects = USIL_EFFECT_UNKNOWN;
        const bool shape_valid = usil_instruction_shape_valid(&program, instruction);
        const bool effects_valid = usil_instruction_effects(&program, instruction, &effects);
        printf("compute_instruction index=%d raw_instruction=%" PRIu32 " opcode=%u name=%s "
               "operands=%d shape_valid=%d effects_valid=%d effects=0x%x\n", index,
               instruction->source_instruction_index, (unsigned)instruction->opcode,
               hlsl_emit_opcode_name((int)instruction->opcode), instruction->operand_count,
               shape_valid, effects_valid, (unsigned)effects);
        if (!shape_valid || !effects_valid || !print_decoded_memory(&program, instruction, index)) goto done;
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(&program, instruction, operand, &use)) goto done;
            uint8_t components = 0U;
            for (int lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1U << lane))) continue;
                const DXBCOperand* source_operand = &instruction->operands[operand];
                const int component = source_operand->type == OPERAND_TYPE_IMMEDIATE32 &&
                    source_operand->imm_value_count == 1 ? 0 :
                    usil_operand_source_component(source_operand, lane);
                if (component < 0 || component > 3) goto done;
                components |= (uint8_t)(1U << component);
            }
            printf("compute_operand_use instruction=%d raw_instruction=%" PRIu32 " operand=%d "
                   "use=%u demanded_lanes=%u physical_components=%u\n", index,
                   instruction->source_instruction_index, operand, (unsigned)use.use,
                   (unsigned)use.source_lane_mask, (unsigned)components);
        }
    }
    complete = true;
    if (inverse) inverse_complete = qualify_atomic_inverse(&program, inverse);
done:
    printf("compute_decode=%s phase=%s\n", complete ? "complete" : "failed", complete ? "none" : phase);
    if (inverse) printf("compute_inverse=%s\n", complete && inverse_complete ? "complete" : "failed");
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    return complete && inverse_complete;
}

static void usage(const char* executable) {
    fprintf(stderr, "usage: %s SOURCE.compute PROJECT_ROOT INCLUDES_DIR KERNEL "
                    "[--target DXBC_FILE] [--decode] [--inverse] [--] [USER_KEYWORD ...]\n"
                    "Use '-' for no additional includes directory. Optional target comparison "
                    "uses every byte. --decode reports bounded actual native instruction, operand "
                    "and resource facts. --inverse implies decoding and checks a bounded atomic inverse\n"
                    "under the observed controls, including actual value mutation and restoration. Options precede keywords; -- ends option parsing. "
                    "No files are written.\n", executable);
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (argc < 5 || !argv[4][0]) {
        usage(argv[0]);
        return 2;
    }
    const char* target_path = NULL;
    bool decode = false;
    bool inverse_requested = false;
    bool decode_supplied = false;
    int keyword_start = 5;
    while (keyword_start < argc) {
        if (strcmp(argv[keyword_start], "--decode") == 0) {
            if (decode_supplied) {
                fputs("--decode may be supplied only once\n", stderr);
                return 2;
            }
            decode_supplied = true;
            decode = true;
            ++keyword_start;
        } else if (strcmp(argv[keyword_start], "--inverse") == 0) {
            if (inverse_requested) {
                fputs("--inverse may be supplied only once\n", stderr);
                return 2;
            }
            inverse_requested = true;
            decode = true;
            ++keyword_start;
        } else if (strcmp(argv[keyword_start], "--target") == 0) {
            if (target_path || keyword_start + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            target_path = argv[keyword_start + 1];
            keyword_start += 2;
        } else if (strcmp(argv[keyword_start], "--") == 0) {
            ++keyword_start;
            break;
        } else {
            break;
        }
    }
    int keyword_count = argc - keyword_start;
    if (keyword_count > 1024) {
        fputs("at most 1024 explicit user keywords are supported\n", stderr);
        return 2;
    }
    CommonFileBytes source = {0}, target = {0};
    UnityCompilerChannel channel = {.socket_fd = -1};
    UnityCompilerComputePreprocessResponse* preprocess = NULL;
    UnityCompilerBinaryResponse compiled;
    unity_compiler_binary_response_init(&compiled);
    UnityComputeBinary binary;
    unity_compute_binary_init(&binary);
    UnityCompilerComputeMacro* macros = NULL;
    int exit_code = 1;
    if (common_file_read_regular_terminated(argv[1], PROBE_FILE_LIMIT, &source) != COMMON_FILE_OK ||
        !source.data || memchr(source.data, 0, source.size)) {
        fputs("could not read a bounded complete source without embedded NUL bytes\n", stderr);
        goto done;
    }
    if (target_path && common_file_read_regular(target_path, PROBE_FILE_LIMIT, &target) != COMMON_FILE_OK) {
        fputs("could not read explicit target\n", stderr);
        goto done;
    }
    print_text("source_filename", argv[1]);
    print_hash("original_source_sha256", source.data, source.size);
    const char* includes = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
    UnityCompilerSessionCapabilities capabilities;
    uint32_t valid_apis = 0U;
    if (!unity_compiler_start_lazy(&channel, argv[2], includes) ||
        !unity_compiler_capture_session_capabilities(&channel, &capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(&capabilities, &valid_apis) ||
        !unity_compiler_set_expected_valid_apis(&channel, valid_apis)) {
        fputs("could not capture the selected compiler's session authority\n", stderr);
        goto done;
    }
    printf("session raw_mask=0x%08" PRIx32 " valid_apis=0x%08" PRIx32
           " compiler_platform=%u build_platform=%u\n",
           capabilities.raw_available_platform_mask, valid_apis, PROBE_PLATFORM, PROBE_BUILD_PLATFORM);
    const uint32_t platform_bit = UINT32_C(1) << PROBE_PLATFORM;
    if ((valid_apis & platform_bit) == 0U) {
        fputs("the session does not expose the requested D3D11 compiler platform\n", stderr);
        goto done;
    }
    UnityCompilerComputePreprocessRequest preprocess_request = {
        .source = (const char*)source.data, .source_filename = argv[1], .caching_preprocessor = true,
        .build_platform = PROBE_BUILD_PLATFORM, .valid_apis = valid_apis};
    if (!unity_compiler_preprocess_compute_response(&channel, &preprocess_request, &preprocess)) {
        fputs("native preprocessing transport failed\n", stderr);
        goto done;
    }
    UnityCompilerComputePreprocessInfo info;
    if (!unity_compiler_compute_preprocess_response_info(preprocess, &info)) goto done;
    printf("preprocess transport_complete=%d availability=%d native_success_present=%d\n",
           info.transport_complete ? 1 : 0, (int)info.availability, info.native_success_present ? 1 : 0);
    if (info.has_request_identity) {
        print_digest("preprocess_request_sha256", info.request_digest);
        print_digest("preprocess_controls_sha256", info.controls_digest);
    }
    size_t diagnostic_count = 0U;
    const UnityCompilerDiagnostic* diagnostics =
        unity_compiler_compute_preprocess_response_diagnostics(preprocess, &diagnostic_count);
    bool clean_diagnostics = print_diagnostics_selected(diagnostics, diagnostic_count, decode) == 0U;
    const UnityCompilerComputePreprocessResult* result =
        unity_compiler_compute_preprocess_response_result(preprocess);
    if (!result || !info.transport_complete || !info.has_request_identity || info.native_success_present) goto done;
    if (!verify_preprocess_provenance(&channel, &preprocess_request, info.request_digest)) {
        fputs("preprocessing request or compiler/include lease changed before provenance replay\n", stderr);
        goto done;
    }
    const UnityCompilerComputePreprocessedKernel* selected = print_preprocess(result, argv[4]);
    if (!selected || selected->macro_count > (size_t)INT_MAX ||
        (((uint32_t)result->supported_apis & platform_bit) == 0U)) {
        fputs("selected kernel or compiler platform is absent from the native result\n", stderr);
        goto done;
    }
    if (selected->macro_count != 0U) {
        macros = calloc(selected->macro_count, sizeof(*macros));
        if (!macros) goto done;
    }
    for (size_t i = 0U; i < selected->macro_count; ++i) {
        macros[i].name = selected->macros[i].name;
        macros[i].value = selected->macros[i].value;
    }
    uint64_t requirements = result->requirements;
    for (size_t i = 0U; i < result->conditional_requirement_count; ++i)
        for (int j = keyword_start; j < argc; ++j)
            if (strcmp(result->conditional_requirements[i].keyword, argv[j]) == 0)
                requirements |= result->conditional_requirements[i].requirements;
    for (int i = keyword_start; i < argc; ++i) print_text("selected_user_keyword", argv[i]);
    UnityCompilerComputeKernelRequest request = {
        .source = result->source, .source_filename = argv[1], .kernel_name = selected->name,
        .caching_preprocessor = preprocess_request.caching_preprocessor,
        .build_platform = PROBE_BUILD_PLATFORM, .kernel_macros = macros,
        .kernel_macro_count = (int)selected->macro_count,
        .user_keywords = keyword_count ? argv + keyword_start : NULL, .user_keyword_count = keyword_count,
        .compiler_platform = (int32_t)PROBE_PLATFORM, .compilation_flags = result->compilation_flags,
        .requirements = requirements, .force_dxc = result->use_dxc_mask, .force_fxc = result->never_use_dxc_mask};
    printf("compile caching_preprocessor=%d preprocess_only=0 strip_line_directives=0 "
           "requirements=0x%016" PRIx64 " flags=0x%08" PRIx32
           " force_dxc=0x%08" PRIx32 " force_fxc=0x%08" PRIx32 "\n",
           request.caching_preprocessor ? 1 : 0, request.requirements,
           request.compilation_flags, request.force_dxc, request.force_fxc);
    if (!unity_compiler_compile_compute_response(&channel, &request, &compiled)) {
        fputs("native compute compilation transport failed\n", stderr);
        goto done;
    }
    printf("compile success=%d availability=%d payload_bytes=%zu\n",
           compiled.status.compiler_success ? 1 : 0, (int)compiled.status.availability, compiled.size);
    if (compiled.has_request_identity) {
        print_digest("compile_request_sha256", compiled.request_digest);
        print_digest("compile_controls_sha256", compiled.controls_digest);
    }
    clean_diagnostics = print_diagnostics_selected(compiled.status.diagnostics,
                                                  compiled.status.diagnostic_count, decode) == 0U &&
                        clean_diagnostics;
    if (!compiled.has_request_identity ||
        !verify_compile_provenance(&channel, &request, compiled.request_digest)) {
        fputs("compute request or compiler/include lease changed before provenance replay\n", stderr);
        goto done;
    }
    if (!compiled.has_request_identity || !compiled.status.compiler_success ||
        compiled.status.availability != UNITY_COMPILER_RESPONSE_AVAILABLE ||
        (decode && !unity_compiler_response_status_is_clean_success(&compiled.status)) ||
        unity_compute_binary_decode(&binary, compiled.data, compiled.size) != COMPUTE_SHADER_OBJECT_OK) goto done;
    printf("native_payload directives=%zu target_level=%" PRId32
           " kernels=%zu cb_variants=%zu resources_resolved=%d\n",
           binary.directive_count, binary.target_level, binary.kernel_count,
           binary.buffer_variant_count, binary.resources_resolved ? 1 : 0);
    if (decode && !native_decode_counts_bounded(&binary)) {
        puts("compute_decode=failed phase=native-record-limit");
        goto done;
    }
    print_native_declarations(&binary);
    for (size_t i = 0U; i < binary.kernel_count; ++i) print_kernel(&binary.kernels[i]);
    if (binary.kernel_count != 1U || binary.kernels[0].name.size != strlen(selected->name) ||
        memcmp(binary.kernels[0].name.bytes, selected->name, binary.kernels[0].name.size) != 0) goto done;
    const ComputeShaderKernelVariant* data = &binary.kernels[0].data;
    DXBCCompareResult comparison;
    /* The shared comparator validates the complete raw container even when
     * comparing it to itself. That is distinct from an external target match. */
    DXBCCompareStatus validation_status = dxbc_compare_exact(
        data->code, data->code_size, data->code, data->code_size, &comparison);
    printf("complete_dxbc_container_validation=%s validation_status=%s\n",
           validation_status == DXBC_COMPARE_EQUAL ? "valid" : "failed",
           dxbc_compare_status_name(validation_status));
    DXBCCompareStatus comparison_status = validation_status;
    if (target_path) {
        comparison_status = dxbc_compare_exact(target.data, target.size, data->code, data->code_size, &comparison);
        printf("complete_dxbc_target_equality=%s\n", dxbc_compare_status_name(comparison_status));
    } else {
        puts("complete_dxbc_target_equality=not-requested");
    }
    printf("clean_compile_diagnostics=%d\n", clean_diagnostics ? 1 : 0);
    ProbeInverseContext inverse = {.channel = &channel, .preprocess_request = &preprocess_request,
        .preprocess_info = &info, .preprocess_result = result, .selected = selected,
        .compile_request = &request, .original_response = &compiled, .original_binary = &binary};
    const bool decode_complete = !decode ||
        (clean_diagnostics && validation_status == DXBC_COMPARE_EQUAL &&
         print_decoded_kernel(data, inverse_requested ? &inverse : NULL));
    exit_code = clean_diagnostics && validation_status == DXBC_COMPARE_EQUAL &&
                comparison_status == DXBC_COMPARE_EQUAL && decode_complete ? 0 : 1;
done:
    unity_compute_binary_dispose(&binary);
    unity_compiler_binary_response_free(&compiled);
    free(macros);
    unity_compiler_compute_preprocess_response_free(preprocess);
    unity_compiler_shutdown(&channel);
    common_file_bytes_dispose(&target);
    common_file_bytes_dispose(&source);
    return exit_code;
}
