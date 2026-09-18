// SPDX-License-Identifier: GPL-3.0-only

#include "common/oracle_metadata.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t k_metadata_magic[8] = {
    'D', 'X', 'B', 'C', 'M', 'E', 'T', 'A'
};
static const uint8_t k_resource_key_magic[8] = {
    'D', 'X', 'B', 'C', 'R', 'K', 'E', 'Y'
};

typedef struct {
    uint8_t* data;
    size_t capacity;
    size_t offset;
    OracleMetadataStatus status;
} MetadataSink;

static bool checked_add(size_t left, size_t right, size_t* output) {
    if (!output || left > SIZE_MAX - right) return false;
    *output = left + right;
    return true;
}

static bool checked_multiply(size_t left, size_t right, size_t* output) {
    if (!output || (left != 0U && right > SIZE_MAX / left)) return false;
    *output = left * right;
    return true;
}

static bool exact_array_shape(const void* values, int count) {
    return count >= 0 && ((count == 0) == (values == NULL));
}

static bool precision_is_valid(OraclePackResourcePrecision precision) {
    return precision >= ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN &&
           precision <= ORACLE_PACK_RESOURCE_PRECISION_HIGH;
}

static bool constant_buffer_role_is_valid(
    SerializedConstantBufferRole role) {
    return role == SERIALIZED_CBUFFER_NAMED ||
           role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
}

static void sink_fail(MetadataSink* sink, OracleMetadataStatus status) {
    if (sink->status == ORACLE_METADATA_OK) sink->status = status;
}

static void sink_bytes(MetadataSink* sink, const void* bytes, size_t size) {
    if (!sink || sink->status != ORACLE_METADATA_OK) return;
    size_t end = 0;
    if (!checked_add(sink->offset, size, &end)) {
        sink_fail(sink, ORACLE_METADATA_SIZE_OVERFLOW);
        return;
    }
    if (sink->data) {
        if (end > sink->capacity || (size > 0U && !bytes)) {
            sink_fail(sink, ORACLE_METADATA_INVALID_ARGUMENT);
            return;
        }
        if (size > 0U) memcpy(sink->data + sink->offset, bytes, size);
    }
    sink->offset = end;
}

static void sink_u32(MetadataSink* sink, uint32_t value) {
    const uint8_t encoded[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };
    sink_bytes(sink, encoded, sizeof(encoded));
}

static void sink_u64(MetadataSink* sink, uint64_t value) {
    uint8_t encoded[8];
    for (unsigned byte = 0; byte < sizeof(encoded); byte++) {
        encoded[byte] = (uint8_t)(value >> (byte * 8U));
    }
    sink_bytes(sink, encoded, sizeof(encoded));
}

static void sink_name(MetadataSink* sink, const char* name) {
    if (!sink || sink->status != ORACLE_METADATA_OK) return;
    if (!name) {
        sink_fail(sink, ORACLE_METADATA_INVALID_VALUE);
        return;
    }
    size_t size = strlen(name);
    if (size > UINT32_MAX) {
        sink_fail(sink, ORACLE_METADATA_SIZE_OVERFLOW);
        return;
    }
    sink_u32(sink, (uint32_t)size);
    sink_bytes(sink, name, size);
}

static void sink_variable(MetadataSink* sink,
                          const SerializedVariable* variable) {
    sink_name(sink, variable->name);
    for (size_t word = 0; word < 6U; word++) {
        sink_u32(sink, variable->layout[word]);
    }
}

static void sink_struct(MetadataSink* sink,
                        const SerializedStructParam* parameter) {
    sink_name(sink, parameter->name);
    for (size_t word = 0; word < 3U; word++) {
        sink_u32(sink, parameter->layout[word]);
    }
    if (!exact_array_shape(parameter->members, parameter->member_count)) {
        sink_fail(sink, ORACLE_METADATA_INVALID_SHAPE);
        return;
    }
    sink_u64(sink, (uint64_t)parameter->member_count);
    for (int member = 0;
         member < parameter->member_count &&
         sink->status == ORACLE_METADATA_OK;
         member++) {
        sink_variable(sink, &parameter->members[member]);
    }
}

static void sink_constant_buffer(
    MetadataSink* sink, const SerializedConstantBuffer* buffer) {
    if (!constant_buffer_role_is_valid(buffer->role)) {
        sink_fail(sink, ORACLE_METADATA_INVALID_VALUE);
        return;
    }
    sink_name(sink, buffer->name);
    sink_u32(sink, (uint32_t)buffer->role);
    sink_u32(sink, buffer->size);
    sink_u32(sink, buffer->has_is_partial ? 1U : 0U);
    sink_u32(sink, buffer->is_partial ? 1U : 0U);
    if (!exact_array_shape(buffer->variables, buffer->var_count) ||
        !exact_array_shape(buffer->struct_params, buffer->struct_count)) {
        sink_fail(sink, ORACLE_METADATA_INVALID_SHAPE);
        return;
    }
    sink_u64(sink, (uint64_t)buffer->var_count);
    for (int variable = 0;
         variable < buffer->var_count && sink->status == ORACLE_METADATA_OK;
         variable++) {
        sink_variable(sink, &buffer->variables[variable]);
    }
    sink_u64(sink, (uint64_t)buffer->struct_count);
    for (int parameter = 0;
         parameter < buffer->struct_count &&
         sink->status == ORACLE_METADATA_OK;
         parameter++) {
        sink_struct(sink, &buffer->struct_params[parameter]);
    }
}

static void sink_resource(MetadataSink* sink,
                          const SerializedResourceParam* resource) {
    if (resource->bind_type < SERIALIZED_RESOURCE_TEXTURE ||
        resource->bind_type > SERIALIZED_RESOURCE_SAMPLER) {
        sink_fail(sink, ORACLE_METADATA_INVALID_VALUE);
        return;
    }
    sink_name(sink, resource->name);
    sink_u32(sink, (uint32_t)resource->bind_type);
    sink_u32(sink, resource->bind_index);
    sink_u32(sink, resource->array_size);
    sink_u32(sink, resource->dimension);
    sink_u32(sink, resource->sampler_index);
    sink_u32(sink, resource->multisampled ? 1U : 0U);
    sink_u32(sink, resource->original_index);
    sink_u32(sink, resource->sampler_state);
    sink_u32(sink, resource->extra[0]);
    sink_u32(sink, resource->extra[1]);
}

static OracleMetadataStatus encode_metadata(
    const SerializedProgramParameters* parameters, uint8_t* data,
    size_t capacity, size_t encoded_size, size_t* output_size) {
    if (!parameters || !output_size ||
        !exact_array_shape(parameters->constant_buffers,
                           parameters->cb_count) ||
        !exact_array_shape(parameters->resources, parameters->res_count)) {
        return parameters && output_size ? ORACLE_METADATA_INVALID_SHAPE
                                         : ORACLE_METADATA_INVALID_ARGUMENT;
    }
    MetadataSink sink = {
        .data = data,
        .capacity = capacity,
        .status = ORACLE_METADATA_OK,
    };
    sink_bytes(&sink, k_metadata_magic, sizeof(k_metadata_magic));
    sink_u32(&sink, ORACLE_METADATA_FORMAT_VERSION);
    sink_u32(&sink, ORACLE_METADATA_HEADER_SIZE);
    sink_u64(&sink, encoded_size);
    sink_u32(&sink, parameters->version);
    sink_u32(&sink, parameters->is_binary ? 1U : 0U);
    sink_u64(&sink, (uint64_t)parameters->cb_count);
    for (int buffer = 0;
         buffer < parameters->cb_count && sink.status == ORACLE_METADATA_OK;
         buffer++) {
        sink_constant_buffer(&sink, &parameters->constant_buffers[buffer]);
    }
    sink_u64(&sink, (uint64_t)parameters->res_count);
    for (int resource = 0;
         resource < parameters->res_count &&
         sink.status == ORACLE_METADATA_OK;
         resource++) {
        sink_resource(&sink, &parameters->resources[resource]);
    }
    if (sink.status != ORACLE_METADATA_OK) return sink.status;
    if (data && sink.offset != capacity) return ORACLE_METADATA_INVALID_VALUE;
    *output_size = sink.offset;
    return ORACLE_METADATA_OK;
}

static OracleMetadataStatus resource_key_size(
    const SerializedResourceParam* resource, size_t* output_size) {
    if (!resource || !output_size) return ORACLE_METADATA_INVALID_ARGUMENT;
    if (!resource->name) return ORACLE_METADATA_INVALID_VALUE;
    size_t name_size = strlen(resource->name);
    if (name_size > UINT32_MAX) return ORACLE_METADATA_SIZE_OVERFLOW;
    if (!checked_add(ORACLE_METADATA_RESOURCE_KEY_HEADER_SIZE, name_size,
                     output_size)) {
        return ORACLE_METADATA_SIZE_OVERFLOW;
    }
    return ORACLE_METADATA_OK;
}

static OracleMetadataStatus encode_resource_key(
    uint8_t* data, size_t size, size_t resource_index,
    const SerializedResourceParam* resource) {
    MetadataSink sink = {
        .data = data,
        .capacity = size,
        .status = ORACLE_METADATA_OK,
    };
    if (!resource->name) return ORACLE_METADATA_INVALID_VALUE;
    size_t name_size = strlen(resource->name);
    if (name_size > UINT32_MAX) return ORACLE_METADATA_SIZE_OVERFLOW;
    sink_bytes(&sink, k_resource_key_magic, sizeof(k_resource_key_magic));
    sink_u32(&sink, ORACLE_METADATA_RESOURCE_KEY_FORMAT_VERSION);
    sink_u32(&sink, ORACLE_METADATA_RESOURCE_KEY_HEADER_SIZE);
    sink_u64(&sink, size);
    sink_u64(&sink, resource_index);
    sink_u32(&sink, (uint32_t)resource->bind_type);
    sink_u32(&sink, resource->bind_index);
    sink_u32(&sink, (uint32_t)name_size);
    sink_u32(&sink, 0U);
    sink_bytes(&sink, resource->name, name_size);
    return sink.status == ORACLE_METADATA_OK && sink.offset == size
               ? ORACLE_METADATA_OK
               : sink.status == ORACLE_METADATA_OK
                     ? ORACLE_METADATA_INVALID_VALUE
                     : sink.status;
}

void oracle_metadata_normalization_init(
    OracleMetadataNormalization* normalization) {
    if (normalization) memset(normalization, 0, sizeof(*normalization));
}

void oracle_metadata_normalization_free(
    OracleMetadataNormalization* normalization) {
    if (!normalization) return;
    free((void*)normalization->pack_input.canonical_bytes.data);
    free((void*)normalization->pack_input.resource_precisions);
    free(normalization->resource_key_storage);
    memset(normalization, 0, sizeof(*normalization));
}

OracleMetadataStatus oracle_metadata_normalize(
    const SerializedProgramParameters* parameters,
    const OracleMetadataPrecisionOverride* precision_overrides,
    size_t precision_override_count,
    OracleMetadataNormalization* output) {
    if (!parameters || !output ||
        (precision_override_count > 0U && !precision_overrides)) {
        return ORACLE_METADATA_INVALID_ARGUMENT;
    }

    size_t canonical_size = 0;
    OracleMetadataStatus status = encode_metadata(
        parameters, NULL, 0U, 0U, &canonical_size);
    if (status != ORACLE_METADATA_OK) return status;
    if (precision_override_count > (size_t)parameters->res_count) {
        return ORACLE_METADATA_INVALID_VALUE;
    }
    if (canonical_size < ORACLE_METADATA_HEADER_SIZE) {
        return ORACLE_METADATA_INVALID_VALUE;
    }

    OracleMetadataNormalization normalized;
    oracle_metadata_normalization_init(&normalized);
    uint8_t* canonical = (uint8_t*)malloc(canonical_size);
    if (!canonical) return ORACLE_METADATA_ALLOCATION_FAILED;
    size_t written_size = 0;
    status = encode_metadata(parameters, canonical, canonical_size,
                             canonical_size, &written_size);
    if (status != ORACLE_METADATA_OK || written_size != canonical_size) {
        free(canonical);
        return status == ORACLE_METADATA_OK ? ORACLE_METADATA_INVALID_VALUE
                                            : status;
    }
    normalized.pack_input.canonical_bytes =
        (OraclePackBytes){canonical, canonical_size};

    size_t resource_count = (size_t)parameters->res_count;
    size_t precision_bytes = 0;
    if (!checked_multiply(resource_count,
                          sizeof(OraclePackResourcePrecisionInput),
                          &precision_bytes)) {
        status = ORACLE_METADATA_SIZE_OVERFLOW;
        goto fail;
    }
    OraclePackResourcePrecisionInput* precisions = NULL;
    if (precision_bytes > 0U) {
        precisions = (OraclePackResourcePrecisionInput*)calloc(
            resource_count, sizeof(*precisions));
        if (!precisions) {
            status = ORACLE_METADATA_ALLOCATION_FAILED;
            goto fail;
        }
    }
    normalized.pack_input.resource_precisions = precisions;
    normalized.pack_input.resource_precision_count = resource_count;

    size_t key_storage_size = 0;
    for (size_t resource = 0; resource < resource_count; resource++) {
        size_t key_size = 0;
        status = resource_key_size(&parameters->resources[resource],
                                   &key_size);
        if (status != ORACLE_METADATA_OK ||
            !checked_add(key_storage_size, key_size, &key_storage_size)) {
            if (status == ORACLE_METADATA_OK)
                status = ORACLE_METADATA_SIZE_OVERFLOW;
            goto fail;
        }
    }
    if (key_storage_size > 0U) {
        normalized.resource_key_storage =
            (uint8_t*)malloc(key_storage_size);
        if (!normalized.resource_key_storage) {
            status = ORACLE_METADATA_ALLOCATION_FAILED;
            goto fail;
        }
        normalized.resource_key_storage_size = key_storage_size;
    }

    size_t key_offset = 0;
    for (size_t resource = 0; resource < resource_count; resource++) {
        size_t key_size = 0;
        status = resource_key_size(&parameters->resources[resource],
                                   &key_size);
        if (status != ORACLE_METADATA_OK) goto fail;
        uint8_t* key = normalized.resource_key_storage + key_offset;
        status = encode_resource_key(key, key_size, resource,
                                     &parameters->resources[resource]);
        if (status != ORACLE_METADATA_OK) goto fail;
        precisions[resource].resource_key =
            (OraclePackBytes){key, key_size};
        precisions[resource].precision =
            ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN;
        key_offset += key_size;
    }

    uint8_t* override_seen = NULL;
    if (precision_override_count > 0U) {
        if (resource_count == 0U) {
            status = ORACLE_METADATA_INVALID_VALUE;
            goto fail;
        }
        override_seen = (uint8_t*)calloc(resource_count, 1U);
        if (!override_seen) {
            status = ORACLE_METADATA_ALLOCATION_FAILED;
            goto fail;
        }
    }
    for (size_t override = 0; override < precision_override_count;
         override++) {
        size_t index = precision_overrides[override].resource_index;
        OraclePackResourcePrecision precision =
            precision_overrides[override].precision;
        if (index >= resource_count || override_seen[index] != 0U ||
            !precision_is_valid(precision)) {
            free(override_seen);
            status = ORACLE_METADATA_INVALID_VALUE;
            goto fail;
        }
        override_seen[index] = 1U;
        precisions[index].precision = precision;
    }
    free(override_seen);

    oracle_metadata_normalization_free(output);
    *output = normalized;
    return ORACLE_METADATA_OK;

fail:
    oracle_metadata_normalization_free(&normalized);
    return status;
}

const char* oracle_metadata_status_string(OracleMetadataStatus status) {
    switch (status) {
        case ORACLE_METADATA_OK: return "ok";
        case ORACLE_METADATA_INVALID_ARGUMENT: return "invalid_argument";
        case ORACLE_METADATA_INVALID_SHAPE: return "invalid_shape";
        case ORACLE_METADATA_INVALID_VALUE: return "invalid_value";
        case ORACLE_METADATA_SIZE_OVERFLOW: return "size_overflow";
        case ORACLE_METADATA_ALLOCATION_FAILED: return "allocation_failed";
        default: return "unknown_status";
    }
}
