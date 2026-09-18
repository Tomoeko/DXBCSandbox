// SPDX-License-Identifier: GPL-3.0-only

#include "io/unity_player_shader_caps.h"

#include "common/stream.h"

#include <string.h>

enum {
    UNITY_PLAYER_SHADER_CAPS_SERIALIZED_FILE_VERSION = 22,
    UNITY_PLAYER_SHADER_CAPS_BUILTIN_SHADER_COUNT = 8,
};

static const char k_supported_unity_version[] = "2021.3.35f1";
static const uint8_t k_graphics_settings_type_hash[16] = {
    0x9bU, 0x60U, 0xe7U, 0x65U, 0xe2U, 0x1dU, 0x19U, 0x9eU,
    0x3eU, 0xaeU, 0xf5U, 0x54U, 0x00U, 0x18U, 0xe8U, 0xdaU,
};

typedef struct {
    ByteStream stream;
    UnityPlayerShaderCapsStatus status;
} ShaderCapsReader;

static bool bytes_are_zero(const uint8_t* bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) return false;
    }
    return true;
}

static bool source_file_is_structurally_valid(const SerializedFile* file) {
    return file && file->raw_data && file->unity_version &&
        file->file_size == (uint64_t)file->raw_size &&
        file->data_offset <= file->file_size && file->type_count >= 0 &&
        file->object_count >= 0 &&
        (file->type_count == 0 || file->types) &&
        (file->object_count == 0 || file->objects);
}

static bool object_range_is_valid(const SerializedFile* file,
                                  const AssetObjectInfo* object) {
    const uint64_t data_size = file->file_size - file->data_offset;
    return object->byte_offset <= data_size &&
        (uint64_t)object->byte_size <= data_size - object->byte_offset;
}

static void reader_fail(ShaderCapsReader* reader,
                        UnityPlayerShaderCapsStatus status) {
    if (reader->status == UNITY_PLAYER_SHADER_CAPS_OK) {
        reader->status = status;
    }
}

static bool reader_u8(ShaderCapsReader* reader, uint8_t* value) {
    if (reader->status != UNITY_PLAYER_SHADER_CAPS_OK) return false;
    if (!stream_read_uint8(&reader->stream, value)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_i32(ShaderCapsReader* reader, int32_t* value) {
    if (reader->status != UNITY_PLAYER_SHADER_CAPS_OK) return false;
    if (!stream_read_int32(&reader->stream, value)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_u32(ShaderCapsReader* reader, uint32_t* value) {
    if (reader->status != UNITY_PLAYER_SHADER_CAPS_OK) return false;
    if (!stream_read_uint32(&reader->stream, value)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_skip(ShaderCapsReader* reader, size_t size) {
    if (reader->status != UNITY_PLAYER_SHADER_CAPS_OK) return false;
    if (!stream_skip(&reader->stream, size)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_align4_zero(ShaderCapsReader* reader) {
    if (reader->status != UNITY_PLAYER_SHADER_CAPS_OK) return false;
    const size_t remainder = reader->stream.position & 3U;
    const size_t padding = remainder == 0U ? 0U : 4U - remainder;
    if (padding > stream_remaining(&reader->stream)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_PAYLOAD_TRUNCATED);
        return false;
    }
    if (!bytes_are_zero(reader->stream.data + reader->stream.position,
                        padding)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_ALIGNMENT_INVALID);
        return false;
    }
    return stream_skip(&reader->stream, padding);
}

static bool reader_boolean(ShaderCapsReader* reader, bool* value) {
    uint8_t serialized = 0U;
    if (!reader_u8(reader, &serialized)) return false;
    if (serialized > 1U) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_BOOLEAN_INVALID);
        return false;
    }
    if (value) *value = serialized != 0U;
    return true;
}

static bool reader_count(ShaderCapsReader* reader, size_t minimum_size,
                         size_t* out_count) {
    *out_count = 0U;
    int32_t signed_count = 0;
    if (!reader_i32(reader, &signed_count)) return false;
    if (signed_count < 0 ||
        (minimum_size != 0U &&
         (size_t)signed_count >
             stream_remaining(&reader->stream) / minimum_size)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_COUNT_INVALID);
        return false;
    }
    *out_count = (size_t)signed_count;
    return true;
}

static bool reader_pptr(ShaderCapsReader* reader) {
    /* int m_FileID; SInt64 m_PathID */
    return reader_skip(reader, 12U);
}

static bool reader_pptr_array(ShaderCapsReader* reader) {
    size_t count = 0U;
    if (!reader_count(reader, 12U, &count)) return false;
    if (dxbc_size_multiply_overflows(count, 12U)) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_COUNT_INVALID);
        return false;
    }
    if (!reader_skip(reader, count * 12U)) return false;
    return reader_align4_zero(reader);
}

static bool reader_aligned_string(ShaderCapsReader* reader) {
    size_t size = 0U;
    if (!reader_count(reader, 1U, &size)) return false;
    if (!reader_skip(reader, size)) return false;
    return reader_align4_zero(reader);
}

static bool reader_tier_settings(ShaderCapsReader* reader) {
    /* renderingPath, hdrMode, realtimeGICPUUsage */
    if (!reader_skip(reader, 3U * sizeof(uint32_t))) return false;
    /* useCascadedShadowMaps, prefer32BitShadowMaps, enableLPPV, useHDR */
    for (size_t index = 0U; index < 4U; ++index) {
        if (!reader_boolean(reader, NULL)) return false;
    }
    return reader_align4_zero(reader);
}

static bool reader_fixed_bitset_33(ShaderCapsReader* reader,
                                   uint64_t* out_bits) {
    int32_t signed_word_count = 0;
    if (!reader_i32(reader, &signed_word_count)) return false;
    if (signed_word_count != 2) {
        reader_fail(reader,
                    UNITY_PLAYER_SHADER_CAPS_BITSET_WORD_COUNT_INVALID);
        return false;
    }
    uint32_t low = 0U;
    uint32_t high = 0U;
    if (!reader_u32(reader, &low) || !reader_u32(reader, &high)) {
        return false;
    }
    if ((high & ~UINT32_C(1)) != 0U) {
        reader_fail(reader,
                    UNITY_PLAYER_SHADER_CAPS_BITSET_HIGH_BITS_INVALID);
        return false;
    }
    *out_bits = (uint64_t)low | ((uint64_t)high << 32U);
    return reader_align4_zero(reader);
}

static bool reader_platform_defines(ShaderCapsReader* reader,
                                    UnityPlayerShaderCaps* result) {
    size_t count = 0U;
    /* PlatformShaderDefines is at least 4 + 3*(4 + 2*4) bytes. */
    if (!reader_count(reader, 40U, &count)) return false;
    if (count > UNITY_PLAYER_SHADER_CAPS_PLATFORM_COUNT) {
        reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_COUNT_INVALID);
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        int32_t signed_platform = 0;
        if (!reader_i32(reader, &signed_platform)) return false;
        if (signed_platform < 0 ||
            (uint32_t)signed_platform >=
                UNITY_PLAYER_SHADER_CAPS_PLATFORM_COUNT) {
            reader_fail(reader, UNITY_PLAYER_SHADER_CAPS_PLATFORM_INVALID);
            return false;
        }
        UnityPlayerShaderCapsEntry* entry =
            &result->entries[(uint32_t)signed_platform];
        if (entry->present) {
            reader_fail(reader,
                        UNITY_PLAYER_SHADER_CAPS_PLATFORM_DUPLICATE);
            return false;
        }
        entry->shader_platform = (uint32_t)signed_platform;
        for (size_t tier = 0U;
             tier < UNITY_PLAYER_SHADER_CAPS_TIER_COUNT; ++tier) {
            if (!reader_fixed_bitset_33(
                    reader, &entry->tier_capabilities[tier])) {
                return false;
            }
        }
        entry->present = true;
        ++result->entry_count;
    }
    return reader_align4_zero(reader);
}

static bool reader_srp_default_settings(ShaderCapsReader* reader) {
    size_t count = 0U;
    /* Empty aligned string (4 bytes) plus PPtr<Object> (12 bytes). */
    if (!reader_count(reader, 16U, &count)) return false;
    for (size_t index = 0U; index < count; ++index) {
        if (!reader_aligned_string(reader) || !reader_pptr(reader)) {
            return false;
        }
    }
    return reader_align4_zero(reader);
}

static UnityPlayerShaderCapsStatus validate_type_identity(
    const SerializedFile* file, const AssetObjectInfo* object) {
    if (object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        return UNITY_PLAYER_SHADER_CAPS_TYPE_INDEX_INVALID;
    }
    const TypeTreeType* type = &file->types[object->type_id_or_index];
    if (type->type_id != UNITY_PLAYER_SHADER_CAPS_CLASS_ID ||
        type->is_ref_type ||
        type->script_type_index != object->script_type_index) {
        return UNITY_PLAYER_SHADER_CAPS_TYPE_RECORD_MISMATCH;
    }
    if (type->is_stripped || type->script_type_index != UINT16_MAX ||
        !bytes_are_zero(type->script_id_hash,
                        sizeof(type->script_id_hash)) ||
        memcmp(type->type_hash, k_graphics_settings_type_hash,
               sizeof(k_graphics_settings_type_hash)) != 0) {
        return UNITY_PLAYER_SHADER_CAPS_TYPE_IDENTITY_UNSUPPORTED;
    }
    return UNITY_PLAYER_SHADER_CAPS_OK;
}

void unity_player_shader_caps_init(UnityPlayerShaderCaps* caps) {
    if (caps) memset(caps, 0, sizeof(*caps));
}

static UnityPlayerShaderCapsStatus decode_payload(
    UnityPlayerShaderCaps* result, const SerializedFile* file,
    const AssetObjectInfo* object) {
    if (!object_range_is_valid(file, object) || object->byte_size == 0U) {
        return UNITY_PLAYER_SHADER_CAPS_OBJECT_RANGE_INVALID;
    }
    UnityPlayerShaderCapsStatus status =
        validate_type_identity(file, object);
    if (status != UNITY_PLAYER_SHADER_CAPS_OK) return status;

    const uint64_t absolute_offset = file->data_offset + object->byte_offset;
    ShaderCapsReader reader;
    stream_init(&reader.stream, file->raw_data + absolute_offset,
                object->byte_size);
    stream_set_endian(&reader.stream, file->big_endian);
    reader.status = UNITY_PLAYER_SHADER_CAPS_OK;

    result->serialized_file_version = file->version;
    result->target_platform = file->target_platform;
    result->path_id = object->path_id;
    result->byte_offset = object->byte_offset;
    result->byte_size = object->byte_size;

    /* Eight BuiltinShaderSettings: int mode + PPtr<Shader>. */
    for (size_t index = 0U;
         index < UNITY_PLAYER_SHADER_CAPS_BUILTIN_SHADER_COUNT; ++index) {
        if (!reader_skip(&reader, sizeof(uint32_t)) ||
            !reader_pptr(&reader)) {
            return reader.status;
        }
    }
    /* m_VideoShadersIncludeMode */
    if (!reader_skip(&reader, sizeof(uint32_t)) ||
        !reader_pptr_array(&reader) || /* m_AlwaysIncludedShaders */
        !reader_pptr_array(&reader) || /* m_PreloadedShaders */
        !reader_skip(&reader, sizeof(uint32_t)) ||
        !reader_pptr(&reader) ||       /* m_SpritesDefaultMaterial */
        !reader_pptr(&reader) ||       /* m_CustomRenderPipeline */
        !reader_skip(&reader, sizeof(uint32_t)) ||
        !reader_skip(&reader, 3U * sizeof(uint32_t))) {
        return reader.status;
    }
    for (size_t tier = 0U;
         tier < UNITY_PLAYER_SHADER_CAPS_TIER_COUNT; ++tier) {
        if (!reader_tier_settings(&reader)) return reader.status;
    }
    if (!reader_platform_defines(&reader, result)) return reader.status;

    if (!reader_boolean(&reader, NULL) || /* linear light intensity */
        !reader_boolean(&reader, NULL) || /* color temperature */
        !reader_align4_zero(&reader) ||
        !reader_skip(&reader, sizeof(uint32_t)) ||
        !reader_boolean(&reader, NULL) || /* log compiled shader */
        !reader_align4_zero(&reader) ||
        !reader_srp_default_settings(&reader) ||
        !reader_boolean(&reader, NULL) || /* relative light culling */
        !reader_boolean(&reader, NULL)) { /* relative shadow culling */
        return reader.status;
    }

    if (reader.stream.position != reader.stream.size) {
        return UNITY_PLAYER_SHADER_CAPS_OBJECT_BYTES_NOT_EXHAUSTED;
    }
    result->decoded = true;
    return UNITY_PLAYER_SHADER_CAPS_OK;
}

UnityPlayerShaderCapsStatus unity_player_shader_caps_decode(
    UnityPlayerShaderCaps* destination, const SerializedFile* file) {
    if (!destination || !file) {
        return UNITY_PLAYER_SHADER_CAPS_INVALID_ARGUMENT;
    }
    if (!source_file_is_structurally_valid(file)) {
        return UNITY_PLAYER_SHADER_CAPS_INVALID_SERIALIZED_FILE;
    }
    if (file->version !=
        UNITY_PLAYER_SHADER_CAPS_SERIALIZED_FILE_VERSION) {
        return UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_FILE_VERSION;
    }
    if (strcmp(file->unity_version, k_supported_unity_version) != 0) {
        return UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_UNITY_VERSION;
    }

    const AssetObjectInfo* graphics_settings_object = NULL;
    for (int index = 0; index < file->object_count; ++index) {
        if (file->objects[index].type_id !=
            UNITY_PLAYER_SHADER_CAPS_CLASS_ID) {
            continue;
        }
        if (graphics_settings_object) {
            return UNITY_PLAYER_SHADER_CAPS_DUPLICATE_OBJECT;
        }
        graphics_settings_object = &file->objects[index];
    }
    if (!graphics_settings_object) return UNITY_PLAYER_SHADER_CAPS_MISSING;

    UnityPlayerShaderCaps temporary;
    unity_player_shader_caps_init(&temporary);
    UnityPlayerShaderCapsStatus status = decode_payload(
        &temporary, file, graphics_settings_object);
    if (status != UNITY_PLAYER_SHADER_CAPS_OK) return status;
    *destination = temporary;
    return UNITY_PLAYER_SHADER_CAPS_OK;
}

UnityPlayerShaderCapsStatus unity_player_shader_caps_decode_serialized_bytes(
    UnityPlayerShaderCaps* destination,
    const uint8_t* bytes, size_t size) {
    if (!destination || !bytes || size == 0U) {
        return UNITY_PLAYER_SHADER_CAPS_INVALID_ARGUMENT;
    }
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, bytes, size)) {
        return UNITY_PLAYER_SHADER_CAPS_INVALID_SERIALIZED_FILE;
    }
    UnityPlayerShaderCapsStatus status =
        unity_player_shader_caps_decode(destination, &file);
    serialized_file_close(&file);
    return status;
}

UnityPlayerShaderCapsStatus unity_player_shader_caps_get(
    const UnityPlayerShaderCaps* caps, uint32_t shader_platform,
    uint32_t tier, uint64_t* out_capabilities) {
    if (!caps || !out_capabilities || !caps->decoded) {
        return UNITY_PLAYER_SHADER_CAPS_INVALID_ARGUMENT;
    }
    if (tier < 1U || tier > UNITY_PLAYER_SHADER_CAPS_TIER_COUNT) {
        return UNITY_PLAYER_SHADER_CAPS_TIER_INVALID;
    }
    if (shader_platform >= UNITY_PLAYER_SHADER_CAPS_PLATFORM_COUNT) {
        return UNITY_PLAYER_SHADER_CAPS_PLATFORM_INVALID;
    }
    const UnityPlayerShaderCapsEntry* entry =
        &caps->entries[shader_platform];
    if (!entry->present) return UNITY_PLAYER_SHADER_CAPS_PLATFORM_MISSING;
    *out_capabilities = entry->tier_capabilities[tier - 1U];
    return UNITY_PLAYER_SHADER_CAPS_OK;
}

const char* unity_player_shader_caps_status_name(
    UnityPlayerShaderCapsStatus status) {
    switch (status) {
        case UNITY_PLAYER_SHADER_CAPS_OK: return "ok";
        case UNITY_PLAYER_SHADER_CAPS_INVALID_ARGUMENT:
            return "invalid-argument";
        case UNITY_PLAYER_SHADER_CAPS_INVALID_SERIALIZED_FILE:
            return "invalid-serialized-file";
        case UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_FILE_VERSION:
            return "unsupported-file-version";
        case UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_UNITY_VERSION:
            return "unsupported-unity-version";
        case UNITY_PLAYER_SHADER_CAPS_MISSING:
            return "graphics-settings-missing";
        case UNITY_PLAYER_SHADER_CAPS_DUPLICATE_OBJECT:
            return "graphics-settings-duplicate";
        case UNITY_PLAYER_SHADER_CAPS_OBJECT_RANGE_INVALID:
            return "object-range-invalid";
        case UNITY_PLAYER_SHADER_CAPS_TYPE_INDEX_INVALID:
            return "type-index-invalid";
        case UNITY_PLAYER_SHADER_CAPS_TYPE_RECORD_MISMATCH:
            return "type-record-mismatch";
        case UNITY_PLAYER_SHADER_CAPS_TYPE_IDENTITY_UNSUPPORTED:
            return "type-identity-unsupported";
        case UNITY_PLAYER_SHADER_CAPS_PAYLOAD_TRUNCATED:
            return "payload-truncated";
        case UNITY_PLAYER_SHADER_CAPS_COUNT_INVALID:
            return "count-invalid";
        case UNITY_PLAYER_SHADER_CAPS_BOOLEAN_INVALID:
            return "boolean-invalid";
        case UNITY_PLAYER_SHADER_CAPS_ALIGNMENT_INVALID:
            return "alignment-invalid";
        case UNITY_PLAYER_SHADER_CAPS_PLATFORM_INVALID:
            return "platform-invalid";
        case UNITY_PLAYER_SHADER_CAPS_PLATFORM_DUPLICATE:
            return "platform-duplicate";
        case UNITY_PLAYER_SHADER_CAPS_BITSET_WORD_COUNT_INVALID:
            return "bitset-word-count-invalid";
        case UNITY_PLAYER_SHADER_CAPS_BITSET_HIGH_BITS_INVALID:
            return "bitset-high-bits-invalid";
        case UNITY_PLAYER_SHADER_CAPS_OBJECT_BYTES_NOT_EXHAUSTED:
            return "object-bytes-not-exhausted";
        case UNITY_PLAYER_SHADER_CAPS_PLATFORM_MISSING:
            return "platform-missing";
        case UNITY_PLAYER_SHADER_CAPS_TIER_INVALID:
            return "tier-invalid";
        default: return "unknown";
    }
}
