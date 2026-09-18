// SPDX-License-Identifier: GPL-3.0-only

#include "io/compute_shader_object.h"

#include "common/stream.h"
#include "dxbc/dxbc_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    COMPUTE_SHADER_CLASS_ID = 72,
    COMPUTE_SHADER_SERIALIZED_FILE_VERSION = 22,
};

/* Exact ClassID 72 type identity present in both admitted player versions. */
static const uint8_t k_compute_shader_type_hash[16] = {
    0xabU, 0xd9U, 0x13U, 0x5bU, 0x8cU, 0xe8U, 0x3dU, 0x04U,
    0x3fU, 0xefU, 0x4eU, 0x9eU, 0xc7U, 0xf5U, 0x33U, 0x66U,
};

typedef struct {
    ByteStream stream;
    ComputeShaderObjectStatus status;
} ComputeReader;

static bool source_file_is_structurally_valid(const SerializedFile* file) {
    return file && file->raw_data && file->unity_version &&
        file->file_size == (uint64_t)file->raw_size &&
        file->data_offset <= file->file_size && file->type_count >= 0 &&
        file->object_count >= 0 &&
        (file->type_count == 0 || file->types) &&
        (file->object_count == 0 || file->objects);
}

static bool source_file_owns_object(const SerializedFile* file,
                                    const AssetObjectInfo* object) {
    for (int index = 0; index < file->object_count; ++index) {
        if (&file->objects[index] == object) return true;
    }
    return false;
}

static bool object_range_is_valid(const SerializedFile* file,
                                  const AssetObjectInfo* object) {
    const uint64_t data_size = file->file_size - file->data_offset;
    return object->byte_offset <= data_size &&
        (uint64_t)object->byte_size <= data_size - object->byte_offset;
}

static bool unity_version_is_pinned(const char* unity_version) {
    return strcmp(unity_version, "2021.3.29f1") == 0 ||
        strcmp(unity_version, "2021.3.35f1") == 0;
}

static bool string_view_equals_literal(ComputeShaderStringView value,
                                       const char* literal) {
    size_t literal_size = strlen(literal);
    return value.bytes && value.size == literal_size &&
        memcmp(value.bytes, literal, literal_size) == 0;
}

static bool bytes_are_zero(const uint8_t* bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) return false;
    }
    return true;
}

static ComputeShaderObjectStatus validate_identity(
    const SerializedFile* file, const AssetObjectInfo* object) {
    if (!file || !object) return COMPUTE_SHADER_OBJECT_INVALID_ARGUMENT;
    if (!source_file_is_structurally_valid(file)) {
        return COMPUTE_SHADER_OBJECT_INVALID_SOURCE_FILE;
    }
    if (file->version != COMPUTE_SHADER_SERIALIZED_FILE_VERSION) {
        return COMPUTE_SHADER_OBJECT_UNSUPPORTED_FILE_VERSION;
    }
    if (!unity_version_is_pinned(file->unity_version)) {
        return COMPUTE_SHADER_OBJECT_UNSUPPORTED_UNITY_VERSION;
    }
    if (!source_file_owns_object(file, object)) {
        return COMPUTE_SHADER_OBJECT_OBJECT_NOT_OWNED;
    }
    if (!object_range_is_valid(file, object) || object->byte_size == 0U) {
        return COMPUTE_SHADER_OBJECT_OBJECT_RANGE_INVALID;
    }
    if (object->type_id != COMPUTE_SHADER_CLASS_ID) {
        return COMPUTE_SHADER_OBJECT_NOT_COMPUTE_SHADER;
    }
    if (object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        return COMPUTE_SHADER_OBJECT_TYPE_INDEX_INVALID;
    }

    const TypeTreeType* type = &file->types[object->type_id_or_index];
    if (type->type_id != COMPUTE_SHADER_CLASS_ID || type->is_ref_type ||
        type->script_type_index != object->script_type_index) {
        return COMPUTE_SHADER_OBJECT_TYPE_RECORD_MISMATCH;
    }
    if (type->is_stripped || type->script_type_index != UINT16_MAX ||
        !bytes_are_zero(type->script_id_hash, sizeof(type->script_id_hash)) ||
        memcmp(type->type_hash, k_compute_shader_type_hash,
               sizeof(k_compute_shader_type_hash)) != 0) {
        return COMPUTE_SHADER_OBJECT_TYPE_IDENTITY_UNSUPPORTED;
    }
    return COMPUTE_SHADER_OBJECT_OK;
}

static void reader_fail(ComputeReader* reader,
                        ComputeShaderObjectStatus status) {
    if (reader->status == COMPUTE_SHADER_OBJECT_OK) reader->status = status;
}

static bool reader_u32(ComputeReader* reader, uint32_t* value) {
    if (reader->status != COMPUTE_SHADER_OBJECT_OK) return false;
    if (!stream_read_uint32(&reader->stream, value)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_i32(ComputeReader* reader, int32_t* value) {
    if (reader->status != COMPUTE_SHADER_OBJECT_OK) return false;
    if (!stream_read_int32(&reader->stream, value)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_i64(ComputeReader* reader, int64_t* value) {
    if (reader->status != COMPUTE_SHADER_OBJECT_OK) return false;
    if (!stream_read_int64(&reader->stream, value)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_align4(ComputeReader* reader) {
    if (reader->status != COMPUTE_SHADER_OBJECT_OK) return false;
    if (!stream_align(&reader->stream, 4U)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_string(ComputeReader* reader,
                          ComputeShaderStringView* string) {
    memset(string, 0, sizeof(*string));
    uint32_t size = 0U;
    if (!reader_u32(reader, &size)) {
        if (reader->status == COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED) {
            reader->status = COMPUTE_SHADER_OBJECT_STRING_TRUNCATED;
        }
        return false;
    }
    if (size > INT32_MAX) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_STRING_LENGTH_INVALID);
        return false;
    }
    if ((size_t)size > stream_remaining(&reader->stream)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_STRING_TRUNCATED);
        return false;
    }
    const uint8_t* bytes = reader->stream.data + reader->stream.position;
    if (memchr(bytes, 0, size)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_STRING_CONTAINS_NUL);
        return false;
    }
    if (!stream_skip(&reader->stream, size) || !reader_align4(reader)) {
        if (reader->status == COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED) {
            reader->status = COMPUTE_SHADER_OBJECT_STRING_TRUNCATED;
        }
        return false;
    }
    string->bytes = bytes;
    string->size = size;
    return true;
}

static bool reader_count(ComputeReader* reader, size_t minimum_wire_size,
                         size_t* count) {
    int32_t value = 0;
    *count = 0U;
    if (!reader_i32(reader, &value)) return false;
    if (value < 0 || (value != 0 && minimum_wire_size != 0U &&
        (size_t)value > stream_remaining(&reader->stream) /
                            minimum_wire_size)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_COUNT_INVALID);
        return false;
    }
    *count = (size_t)value;
    return true;
}

static bool reader_array_alloc(ComputeReader* reader, size_t count,
                               size_t element_size, void** allocation) {
    *allocation = NULL;
    if (count == 0U) return true;
    if (element_size == 0U || count > SIZE_MAX / element_size) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_COUNT_INVALID);
        return false;
    }
    void* result = calloc(count, element_size);
    if (!result) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_ALLOCATION_FAILED);
        return false;
    }
    *allocation = result;
    return true;
}

static void dispose_kernel_variant(ComputeShaderKernelVariant* variant) {
    if (!variant) return;
    free(variant->constant_buffer_variant_indices);
    free(variant->constant_buffers);
    free(variant->textures);
    free(variant->builtin_samplers);
    free(variant->input_buffers);
    free(variant->output_buffers);
    free(variant->thread_group_size);
    memset(variant, 0, sizeof(*variant));
}

static void dispose_kernel_parent(ComputeShaderKernelParent* kernel) {
    if (!kernel) return;
    for (size_t index = 0U; index < kernel->variant_count; ++index) {
        dispose_kernel_variant(&kernel->variants[index]);
    }
    free(kernel->variants);
    free(kernel->global_keywords);
    free(kernel->local_keywords);
    memset(kernel, 0, sizeof(*kernel));
}

static void dispose_platform(ComputeShaderPlatformVariant* platform) {
    if (!platform) return;
    for (size_t index = 0U; index < platform->kernel_count; ++index) {
        dispose_kernel_parent(&platform->kernels[index]);
    }
    for (size_t index = 0U; index < platform->constant_buffer_count;
         ++index) {
        free(platform->constant_buffers[index].parameters);
    }
    free(platform->kernels);
    free(platform->constant_buffers);
    memset(platform, 0, sizeof(*platform));
}

void compute_shader_object_init(ComputeShaderObject* object) {
    if (object) memset(object, 0, sizeof(*object));
}

void compute_shader_object_dispose(ComputeShaderObject* object) {
    if (!object) return;
    for (size_t index = 0U; index < object->platform_count; ++index) {
        dispose_platform(&object->platforms[index]);
    }
    free(object->platforms);
    compute_shader_object_init(object);
}

bool compute_shader_object_has_layout_authority(
    const ComputeShaderObject* object) {
    return object && object->decoded &&
        object->serialized_object_bytes &&
        object->serialized_object_size != 0U &&
        object->serialized_file_version ==
            COMPUTE_SHADER_SERIALIZED_FILE_VERSION &&
        (string_view_equals_literal(object->unity_version, "2021.3.29f1") ||
         string_view_equals_literal(object->unity_version, "2021.3.35f1")) &&
        memcmp(object->serialized_type_hash, k_compute_shader_type_hash,
               sizeof(k_compute_shader_type_hash)) == 0;
}

static bool parse_resource(ComputeReader* reader,
                           ComputeShaderResource* resource) {
    return reader_string(reader, &resource->name) &&
        reader_string(reader, &resource->generated_name) &&
        reader_i32(reader, &resource->bind_point) &&
        reader_i32(reader, &resource->sampler_bind_point) &&
        reader_i32(reader, &resource->texture_dimension);
}

static bool parse_resources(ComputeReader* reader,
                            ComputeShaderResource** resources,
                            size_t* resource_count) {
    if (!reader_count(reader, 20U, resource_count) ||
        !reader_array_alloc(reader, *resource_count, sizeof(**resources),
                            (void**)resources)) {
        return false;
    }
    for (size_t index = 0U; index < *resource_count; ++index) {
        if (!parse_resource(reader, &(*resources)[index])) return false;
    }
    return reader_align4(reader);
}

static bool parse_builtin_samplers(
    ComputeReader* reader, ComputeShaderBuiltinSampler** samplers,
    size_t* sampler_count) {
    if (!reader_count(reader, 8U, sampler_count) ||
        !reader_array_alloc(reader, *sampler_count, sizeof(**samplers),
                            (void**)samplers)) {
        return false;
    }
    for (size_t index = 0U; index < *sampler_count; ++index) {
        if (!reader_u32(reader, &(*samplers)[index].sampler) ||
            !reader_i32(reader, &(*samplers)[index].bind_point)) {
            return false;
        }
    }
    return reader_align4(reader);
}

static bool parse_u32_array(ComputeReader* reader, uint32_t** values,
                            size_t* count, bool align) {
    if (!reader_count(reader, 4U, count) ||
        !reader_array_alloc(reader, *count, sizeof(**values),
                            (void**)values)) {
        return false;
    }
    for (size_t index = 0U; index < *count; ++index) {
        if (!reader_u32(reader, &(*values)[index])) return false;
    }
    return !align || reader_align4(reader);
}

static bool parse_kernel_variant(ComputeReader* reader,
                                 ComputeShaderKernelVariant* variant) {
    if (!parse_u32_array(reader,
                        &variant->constant_buffer_variant_indices,
                        &variant->constant_buffer_variant_index_count,
                        true) ||
        !parse_resources(reader, &variant->constant_buffers,
                         &variant->constant_buffer_count) ||
        !parse_resources(reader, &variant->textures,
                         &variant->texture_count) ||
        !parse_builtin_samplers(reader, &variant->builtin_samplers,
                                &variant->builtin_sampler_count) ||
        !parse_resources(reader, &variant->input_buffers,
                         &variant->input_buffer_count) ||
        !parse_resources(reader, &variant->output_buffers,
                         &variant->output_buffer_count)) {
        return false;
    }

    size_t code_size = 0U;
    if (!reader_count(reader, 1U, &code_size)) return false;
    if (code_size > stream_remaining(&reader->stream)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    variant->code = reader->stream.data + reader->stream.position;
    variant->code_size = code_size;
    if (!stream_skip(&reader->stream, code_size) ||
        !reader_align4(reader) ||
        !parse_u32_array(reader, &variant->thread_group_size,
                         &variant->thread_group_size_count, false) ||
        !reader_i64(reader, &variant->requirements)) {
        return false;
    }
    return true;
}

static bool parse_string_array(ComputeReader* reader,
                               ComputeShaderStringView** strings,
                               size_t* count) {
    if (!reader_count(reader, 4U, count) ||
        !reader_array_alloc(reader, *count, sizeof(**strings),
                            (void**)strings)) {
        return false;
    }
    for (size_t index = 0U; index < *count; ++index) {
        if (!reader_string(reader, &(*strings)[index])) return false;
    }
    return reader_align4(reader);
}

static bool parse_kernel_parent(ComputeReader* reader,
                                ComputeShaderKernelParent* kernel) {
    if (!reader_string(reader, &kernel->name) ||
        !reader_count(reader, 12U, &kernel->variant_count) ||
        !reader_array_alloc(reader, kernel->variant_count,
                            sizeof(*kernel->variants),
                            (void**)&kernel->variants)) {
        return false;
    }
    for (size_t index = 0U; index < kernel->variant_count; ++index) {
        ComputeShaderKernelVariant* variant = &kernel->variants[index];
        if (!reader_string(reader, &variant->keyword_key) ||
            !parse_kernel_variant(reader, variant)) {
            return false;
        }
    }
    return parse_string_array(reader, &kernel->global_keywords,
                              &kernel->global_keyword_count) &&
        parse_string_array(reader, &kernel->local_keywords,
                           &kernel->local_keyword_count);
}

static bool parse_parameter(ComputeReader* reader,
                            ComputeShaderParameter* parameter) {
    return reader_string(reader, &parameter->name) &&
        reader_i32(reader, &parameter->type) &&
        reader_u32(reader, &parameter->offset) &&
        reader_u32(reader, &parameter->array_size) &&
        reader_u32(reader, &parameter->row_count) &&
        reader_u32(reader, &parameter->column_count);
}

static bool parse_constant_buffer(ComputeReader* reader,
                                  ComputeShaderConstantBuffer* buffer) {
    if (!reader_string(reader, &buffer->name) ||
        !reader_i32(reader, &buffer->byte_size) || buffer->byte_size < 0) {
        if (reader->status == COMPUTE_SHADER_OBJECT_OK) {
            reader_fail(reader, COMPUTE_SHADER_OBJECT_MODEL_INVALID);
        }
        return false;
    }
    if (!reader_count(reader, 24U, &buffer->parameter_count) ||
        !reader_array_alloc(reader, buffer->parameter_count,
                            sizeof(*buffer->parameters),
                            (void**)&buffer->parameters)) {
        return false;
    }
    for (size_t index = 0U; index < buffer->parameter_count; ++index) {
        if (!parse_parameter(reader, &buffer->parameters[index])) {
            return false;
        }
    }
    return reader_align4(reader);
}

static bool parse_platform(ComputeReader* reader,
                           ComputeShaderPlatformVariant* platform) {
    if (!reader_i32(reader, &platform->target_renderer) ||
        !reader_i32(reader, &platform->target_level) ||
        !reader_count(reader, 8U, &platform->kernel_count) ||
        !reader_array_alloc(reader, platform->kernel_count,
                            sizeof(*platform->kernels),
                            (void**)&platform->kernels)) {
        return false;
    }
    for (size_t index = 0U; index < platform->kernel_count; ++index) {
        if (!parse_kernel_parent(reader, &platform->kernels[index])) {
            return false;
        }
    }
    if (!reader_align4(reader) ||
        !reader_count(reader, 12U, &platform->constant_buffer_count) ||
        !reader_array_alloc(reader, platform->constant_buffer_count,
                            sizeof(*platform->constant_buffers),
                            (void**)&platform->constant_buffers)) {
        return false;
    }
    for (size_t index = 0U; index < platform->constant_buffer_count;
         ++index) {
        if (!parse_constant_buffer(reader,
                                   &platform->constant_buffers[index])) {
            return false;
        }
    }
    if (!reader_align4(reader)) return false;
    uint8_t resolved = 0U;
    if (!stream_read_uint8(&reader->stream, &resolved)) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    if (resolved > 1U) {
        reader_fail(reader, COMPUTE_SHADER_OBJECT_MODEL_INVALID);
        return false;
    }
    platform->resources_resolved = resolved != 0U;
    return reader_align4(reader);
}

ComputeShaderObjectStatus compute_shader_object_decode_borrowed(
    ComputeShaderObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object) {
    if (!destination) return COMPUTE_SHADER_OBJECT_INVALID_ARGUMENT;
    ComputeShaderObjectStatus status = validate_identity(file, object);
    if (status != COMPUTE_SHADER_OBJECT_OK) return status;

    const uint64_t absolute_offset = file->data_offset + object->byte_offset;
    const uint8_t* object_data = file->raw_data + (size_t)absolute_offset;
    ComputeReader reader;
    stream_init(&reader.stream, object_data, object->byte_size);
    stream_set_endian(&reader.stream, file->big_endian);
    reader.status = COMPUTE_SHADER_OBJECT_OK;

    ComputeShaderObject candidate;
    compute_shader_object_init(&candidate);
    if (!reader_string(&reader, &candidate.name) ||
        !reader_count(&reader, 12U, &candidate.platform_count) ||
        !reader_array_alloc(&reader, candidate.platform_count,
                            sizeof(*candidate.platforms),
                            (void**)&candidate.platforms)) {
        goto fail;
    }
    for (size_t index = 0U; index < candidate.platform_count; ++index) {
        if (!parse_platform(&reader, &candidate.platforms[index])) goto fail;
    }
    if (!reader_align4(&reader)) goto fail;
    if (reader.stream.position != object->byte_size) {
        reader_fail(&reader,
                    COMPUTE_SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED);
        goto fail;
    }

    candidate.path_id = object->path_id;
    candidate.target_platform = file->target_platform;
    candidate.serialized_object_bytes = object_data;
    candidate.serialized_object_size = object->byte_size;
    candidate.serialized_file_version = file->version;
    candidate.unity_version.bytes = (const uint8_t*)file->unity_version;
    candidate.unity_version.size = strlen(file->unity_version);
    memcpy(candidate.serialized_type_hash, k_compute_shader_type_hash,
           sizeof(candidate.serialized_type_hash));
    candidate.decoded = true;
    compute_shader_object_dispose(destination);
    *destination = candidate;
    return COMPUTE_SHADER_OBJECT_OK;

fail:
    status = reader.status == COMPUTE_SHADER_OBJECT_OK
        ? COMPUTE_SHADER_OBJECT_MODEL_INVALID : reader.status;
    compute_shader_object_dispose(&candidate);
    return status;
}

static bool size_add(size_t* destination, size_t value) {
    if (*destination > SIZE_MAX - value) return false;
    *destination += value;
    return true;
}

static bool valid_compute_dxbc(const uint8_t* data, size_t size) {
    if (!data || size == 0U) return false;
    DXBCContainer container;
    memset(&container, 0, sizeof(container));
    bool valid = dxbc_parse(&container, data, size) &&
        container.program_type == DXBC_PROGRAM_TYPE_COMPUTE;
    dxbc_free(&container);
    return valid;
}

bool compute_shader_object_summarize(
    const ComputeShaderObject* object, ComputeShaderObjectSummary* summary) {
    if (!summary) return false;
    memset(summary, 0, sizeof(*summary));
    if (!compute_shader_object_has_layout_authority(object) ||
        (object->platform_count != 0U && !object->platforms)) {
        return false;
    }
    summary->platform_count = object->platform_count;
    for (size_t platform_index = 0U;
         platform_index < object->platform_count; ++platform_index) {
        const ComputeShaderPlatformVariant* platform =
            &object->platforms[platform_index];
        if ((platform->kernel_count != 0U && !platform->kernels) ||
            (platform->constant_buffer_count != 0U &&
             !platform->constant_buffers) ||
            !size_add(&summary->kernel_parent_count,
                      platform->kernel_count) ||
            !size_add(&summary->constant_buffer_count,
                      platform->constant_buffer_count)) {
            return false;
        }
        for (size_t buffer_index = 0U;
             buffer_index < platform->constant_buffer_count;
             ++buffer_index) {
            if (!size_add(&summary->parameter_count,
                          platform->constant_buffers[buffer_index]
                              .parameter_count)) {
                return false;
            }
        }
        for (size_t kernel_index = 0U;
             kernel_index < platform->kernel_count; ++kernel_index) {
            const ComputeShaderKernelParent* kernel =
                &platform->kernels[kernel_index];
            if (kernel->variant_count != 0U && !kernel->variants) {
                return false;
            }
            if (!size_add(&summary->kernel_variant_count,
                          kernel->variant_count)) {
                return false;
            }
            for (size_t variant_index = 0U;
                 variant_index < kernel->variant_count; ++variant_index) {
                const ComputeShaderKernelVariant* variant =
                    &kernel->variants[variant_index];
                if (!size_add(&summary->code_blob_count, 1U) ||
                    !size_add(&summary->resource_count,
                              variant->constant_buffer_count) ||
                    !size_add(&summary->resource_count,
                              variant->texture_count) ||
                    !size_add(&summary->resource_count,
                              variant->input_buffer_count) ||
                    !size_add(&summary->resource_count,
                              variant->output_buffer_count)) {
                    return false;
                }
                if (variant->code_size == 0U) {
                    ++summary->empty_code_blob_count;
                } else if (valid_compute_dxbc(variant->code,
                                              variant->code_size)) {
                    ++summary->dxbc_code_blob_count;
                } else {
                    ++summary->non_dxbc_code_blob_count;
                }
                if (variant->thread_group_size_count == 3U &&
                    variant->thread_group_size &&
                    variant->thread_group_size[0] != 0U &&
                    variant->thread_group_size[1] != 0U &&
                    variant->thread_group_size[2] != 0U) {
                    ++summary->exact_thread_group_count;
                } else {
                    ++summary->invalid_thread_group_count;
                }
            }
        }
    }
    return true;
}

ComputeShaderSourceAuthorityStatus compute_shader_object_source_authority(
    const ComputeShaderObject* object) {
    ComputeShaderObjectSummary summary;
    if (!compute_shader_object_summarize(object, &summary)) {
        return COMPUTE_SHADER_SOURCE_AUTHORITY_NOT_DECODED;
    }
    if (summary.kernel_variant_count == 0U) {
        return COMPUTE_SHADER_SOURCE_AUTHORITY_NO_KERNELS;
    }
    if (summary.empty_code_blob_count != 0U) {
        return COMPUTE_SHADER_SOURCE_AUTHORITY_CODE_UNAVAILABLE;
    }
    if (summary.non_dxbc_code_blob_count != 0U ||
        summary.dxbc_code_blob_count != summary.code_blob_count) {
        return COMPUTE_SHADER_SOURCE_AUTHORITY_NON_DXBC_PROGRAMS_UNINVERTED;
    }
    if (summary.invalid_thread_group_count != 0U ||
        summary.exact_thread_group_count != summary.kernel_variant_count) {
        return COMPUTE_SHADER_SOURCE_AUTHORITY_THREAD_GROUP_INVALID;
    }
    return COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE;
}

ComputeShaderInventoryStatus compute_shader_object_name_view(
    const SerializedFile* file, const AssetObjectInfo* object,
    ComputeShaderNameView* out_view) {
    if (!out_view) return COMPUTE_SHADER_INVENTORY_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    ComputeShaderObjectStatus identity = validate_identity(file, object);
    switch (identity) {
        case COMPUTE_SHADER_OBJECT_OK: break;
        case COMPUTE_SHADER_OBJECT_INVALID_ARGUMENT:
            return COMPUTE_SHADER_INVENTORY_INVALID_ARGUMENT;
        case COMPUTE_SHADER_OBJECT_INVALID_SOURCE_FILE:
            return COMPUTE_SHADER_INVENTORY_INVALID_SOURCE_FILE;
        case COMPUTE_SHADER_OBJECT_UNSUPPORTED_FILE_VERSION:
            return COMPUTE_SHADER_INVENTORY_UNSUPPORTED_FILE_VERSION;
        case COMPUTE_SHADER_OBJECT_UNSUPPORTED_UNITY_VERSION:
            return COMPUTE_SHADER_INVENTORY_UNSUPPORTED_UNITY_VERSION;
        case COMPUTE_SHADER_OBJECT_OBJECT_NOT_OWNED:
            return COMPUTE_SHADER_INVENTORY_OBJECT_NOT_OWNED;
        case COMPUTE_SHADER_OBJECT_OBJECT_RANGE_INVALID:
            return COMPUTE_SHADER_INVENTORY_OBJECT_RANGE_INVALID;
        case COMPUTE_SHADER_OBJECT_NOT_COMPUTE_SHADER:
            return COMPUTE_SHADER_INVENTORY_NOT_COMPUTE_SHADER;
        case COMPUTE_SHADER_OBJECT_TYPE_INDEX_INVALID:
            return COMPUTE_SHADER_INVENTORY_TYPE_INDEX_INVALID;
        case COMPUTE_SHADER_OBJECT_TYPE_RECORD_MISMATCH:
            return COMPUTE_SHADER_INVENTORY_TYPE_RECORD_MISMATCH;
        default:
            return COMPUTE_SHADER_INVENTORY_TYPE_IDENTITY_UNSUPPORTED;
    }

    const uint64_t absolute_offset = file->data_offset + object->byte_offset;
    const uint8_t* object_data = file->raw_data + (size_t)absolute_offset;
    ByteStream stream;
    stream_init(&stream, object_data, object->byte_size);
    stream_set_endian(&stream, file->big_endian);
    int32_t signed_name_size = 0;
    if (!stream_read_int32(&stream, &signed_name_size)) {
        return COMPUTE_SHADER_INVENTORY_NAME_TRUNCATED;
    }
    if (signed_name_size < 0) {
        return COMPUTE_SHADER_INVENTORY_NAME_LENGTH_INVALID;
    }
    const size_t name_size = (size_t)signed_name_size;
    if (name_size > stream_remaining(&stream)) {
        return COMPUTE_SHADER_INVENTORY_NAME_TRUNCATED;
    }
    const uint8_t* name_bytes = object_data + stream.position;
    if (memchr(name_bytes, 0, name_size)) {
        return COMPUTE_SHADER_INVENTORY_NAME_CONTAINS_NUL;
    }
    if (!stream_skip(&stream, name_size) || !stream_align(&stream, 4U)) {
        return COMPUTE_SHADER_INVENTORY_NAME_TRUNCATED;
    }
    int32_t signed_variant_count = 0;
    if (!stream_read_int32(&stream, &signed_variant_count)) {
        return COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_TRUNCATED;
    }
    if (signed_variant_count < 0) {
        return COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_INVALID;
    }

    out_view->name_bytes = name_bytes;
    out_view->name_size = name_size;
    out_view->declared_platform_variant_count =
        (uint32_t)signed_variant_count;
    return COMPUTE_SHADER_INVENTORY_OK;
}

const char* compute_shader_inventory_status_name(
    ComputeShaderInventoryStatus status) {
    switch (status) {
        case COMPUTE_SHADER_INVENTORY_NOT_APPLICABLE: return "not-applicable";
        case COMPUTE_SHADER_INVENTORY_OK: return "ok";
        case COMPUTE_SHADER_INVENTORY_INVALID_ARGUMENT: return "invalid-argument";
        case COMPUTE_SHADER_INVENTORY_INVALID_SOURCE_FILE: return "invalid-source-file";
        case COMPUTE_SHADER_INVENTORY_UNSUPPORTED_FILE_VERSION: return "unsupported-file-version";
        case COMPUTE_SHADER_INVENTORY_UNSUPPORTED_UNITY_VERSION: return "unsupported-unity-version";
        case COMPUTE_SHADER_INVENTORY_OBJECT_NOT_OWNED: return "object-not-owned";
        case COMPUTE_SHADER_INVENTORY_OBJECT_RANGE_INVALID: return "object-range-invalid";
        case COMPUTE_SHADER_INVENTORY_NOT_COMPUTE_SHADER: return "not-compute-shader";
        case COMPUTE_SHADER_INVENTORY_TYPE_INDEX_INVALID: return "type-index-invalid";
        case COMPUTE_SHADER_INVENTORY_TYPE_RECORD_MISMATCH: return "type-record-mismatch";
        case COMPUTE_SHADER_INVENTORY_TYPE_IDENTITY_UNSUPPORTED: return "type-identity-unsupported";
        case COMPUTE_SHADER_INVENTORY_NAME_LENGTH_INVALID: return "name-length-invalid";
        case COMPUTE_SHADER_INVENTORY_NAME_TRUNCATED: return "name-truncated";
        case COMPUTE_SHADER_INVENTORY_NAME_CONTAINS_NUL: return "name-contains-nul";
        case COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_TRUNCATED: return "variant-count-truncated";
        case COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_INVALID: return "variant-count-invalid";
        default: return "unknown";
    }
}

const char* compute_shader_object_status_name(ComputeShaderObjectStatus status) {
    switch (status) {
        case COMPUTE_SHADER_OBJECT_NOT_APPLICABLE: return "not-applicable";
        case COMPUTE_SHADER_OBJECT_OK: return "ok";
        case COMPUTE_SHADER_OBJECT_INVALID_ARGUMENT: return "invalid-argument";
        case COMPUTE_SHADER_OBJECT_INVALID_SOURCE_FILE: return "invalid-source-file";
        case COMPUTE_SHADER_OBJECT_UNSUPPORTED_FILE_VERSION: return "unsupported-file-version";
        case COMPUTE_SHADER_OBJECT_UNSUPPORTED_UNITY_VERSION: return "unsupported-unity-version";
        case COMPUTE_SHADER_OBJECT_OBJECT_NOT_OWNED: return "object-not-owned";
        case COMPUTE_SHADER_OBJECT_OBJECT_RANGE_INVALID: return "object-range-invalid";
        case COMPUTE_SHADER_OBJECT_NOT_COMPUTE_SHADER: return "not-compute-shader";
        case COMPUTE_SHADER_OBJECT_TYPE_INDEX_INVALID: return "type-index-invalid";
        case COMPUTE_SHADER_OBJECT_TYPE_RECORD_MISMATCH: return "type-record-mismatch";
        case COMPUTE_SHADER_OBJECT_TYPE_IDENTITY_UNSUPPORTED: return "type-identity-unsupported";
        case COMPUTE_SHADER_OBJECT_STRING_LENGTH_INVALID: return "string-length-invalid";
        case COMPUTE_SHADER_OBJECT_STRING_TRUNCATED: return "string-truncated";
        case COMPUTE_SHADER_OBJECT_STRING_CONTAINS_NUL: return "string-contains-nul";
        case COMPUTE_SHADER_OBJECT_COUNT_INVALID: return "count-invalid";
        case COMPUTE_SHADER_OBJECT_ALLOCATION_FAILED: return "allocation-failed";
        case COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED: return "payload-truncated";
        case COMPUTE_SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED: return "object-bytes-not-exhausted";
        case COMPUTE_SHADER_OBJECT_MODEL_INVALID: return "model-invalid";
        case COMPUTE_SHADER_OBJECT_NOT_DECODED: return "not-decoded";
        default: return "unknown";
    }
}

const char* compute_shader_source_authority_status_name(
    ComputeShaderSourceAuthorityStatus status) {
    switch (status) {
        case COMPUTE_SHADER_SOURCE_AUTHORITY_NOT_APPLICABLE:
            return "not-applicable";
        case COMPUTE_SHADER_SOURCE_AUTHORITY_EXACT: return "exact";
        case COMPUTE_SHADER_SOURCE_AUTHORITY_NOT_DECODED: return "not-decoded";
        case COMPUTE_SHADER_SOURCE_AUTHORITY_NO_KERNELS: return "no-kernels";
        case COMPUTE_SHADER_SOURCE_AUTHORITY_CODE_UNAVAILABLE: return "code-unavailable";
        case COMPUTE_SHADER_SOURCE_AUTHORITY_NON_DXBC_PROGRAMS_UNINVERTED:
            return "non-dxbc-platform-programs-uninverted";
        case COMPUTE_SHADER_SOURCE_AUTHORITY_THREAD_GROUP_INVALID: return "thread-group-invalid";
        case COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE:
            return "declaration-inverse-unavailable";
        default: return "unknown";
    }
}
