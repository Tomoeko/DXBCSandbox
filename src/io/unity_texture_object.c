// SPDX-License-Identifier: GPL-3.0-only

#include "io/unity_texture_object.h"

#include "common/stream.h"

#include <limits.h>
#include <string.h>

enum {
    UNITY_TEXTURE_SERIALIZED_FILE_VERSION = 22,
};

static const uint8_t k_texture2d_type_hash[16] = {
    0x0dU, 0x08U, 0x41U, 0x4cU, 0xfdU, 0x5bU, 0xdbU, 0x0dU,
    0x22U, 0x79U, 0x20U, 0x11U, 0xbdU, 0xa9U, 0xabU, 0x26U,
};

static const uint8_t k_render_texture_type_hash[16] = {
    0xaeU, 0x59U, 0x18U, 0xd7U, 0x86U, 0x68U, 0x29U, 0x7fU,
    0x97U, 0xb9U, 0x8eU, 0x4bU, 0x8bU, 0x72U, 0xcfU, 0xc2U,
};

typedef struct {
    ByteStream stream;
    UnityTextureObjectStatus status;
} TextureReader;

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

static const uint8_t* expected_hash_for_class(int32_t class_id) {
    if (class_id == UNITY_TEXTURE2D_CLASS_ID) return k_texture2d_type_hash;
    if (class_id == UNITY_RENDER_TEXTURE_CLASS_ID) {
        return k_render_texture_type_hash;
    }
    return NULL;
}

static UnityTextureObjectStatus validate_identity(
    const SerializedFile* file, const AssetObjectInfo* object) {
    if (!file || !object) return UNITY_TEXTURE_OBJECT_INVALID_ARGUMENT;
    if (!source_file_is_structurally_valid(file)) {
        return UNITY_TEXTURE_OBJECT_INVALID_SOURCE_FILE;
    }
    if (file->version != UNITY_TEXTURE_SERIALIZED_FILE_VERSION) {
        return UNITY_TEXTURE_OBJECT_UNSUPPORTED_FILE_VERSION;
    }
    if (!unity_version_is_pinned(file->unity_version)) {
        return UNITY_TEXTURE_OBJECT_UNSUPPORTED_UNITY_VERSION;
    }
    if (file->target_platform != 5U && file->target_platform != 19U) {
        return UNITY_TEXTURE_OBJECT_UNSUPPORTED_TARGET_PLATFORM;
    }
    if (file->big_endian) {
        return UNITY_TEXTURE_OBJECT_UNSUPPORTED_BYTE_ORDER;
    }
    if (!source_file_owns_object(file, object)) {
        return UNITY_TEXTURE_OBJECT_OBJECT_NOT_OWNED;
    }
    if (!object_range_is_valid(file, object) || object->byte_size == 0U) {
        return UNITY_TEXTURE_OBJECT_OBJECT_RANGE_INVALID;
    }
    const uint8_t* expected_hash = expected_hash_for_class(object->type_id);
    if (!expected_hash) return UNITY_TEXTURE_OBJECT_CLASS_UNSUPPORTED;
    if (object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        return UNITY_TEXTURE_OBJECT_TYPE_INDEX_INVALID;
    }
    const TypeTreeType* type = &file->types[object->type_id_or_index];
    if (type->type_id != object->type_id || type->is_ref_type ||
        type->script_type_index != object->script_type_index) {
        return UNITY_TEXTURE_OBJECT_TYPE_RECORD_MISMATCH;
    }
    if (type->is_stripped || type->script_type_index != UINT16_MAX ||
        !bytes_are_zero(type->script_id_hash, sizeof(type->script_id_hash)) ||
        memcmp(type->type_hash, expected_hash, 16U) != 0) {
        return UNITY_TEXTURE_OBJECT_TYPE_IDENTITY_UNSUPPORTED;
    }
    return UNITY_TEXTURE_OBJECT_OK;
}

static void reader_fail(TextureReader* reader,
                        UnityTextureObjectStatus status) {
    if (reader->status == UNITY_TEXTURE_OBJECT_OK) reader->status = status;
}

static bool reader_u8(TextureReader* reader, uint8_t* value) {
    if (reader->status != UNITY_TEXTURE_OBJECT_OK) return false;
    if (!stream_read_uint8(&reader->stream, value)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_u32(TextureReader* reader, uint32_t* value) {
    if (reader->status != UNITY_TEXTURE_OBJECT_OK) return false;
    if (!stream_read_uint32(&reader->stream, value)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_i32(TextureReader* reader, int32_t* value) {
    uint32_t wire = 0U;
    if (!reader_u32(reader, &wire)) return false;
    memcpy(value, &wire, sizeof(wire));
    return true;
}

static bool reader_u64(TextureReader* reader, uint64_t* value) {
    if (reader->status != UNITY_TEXTURE_OBJECT_OK) return false;
    if (!stream_read_uint64(&reader->stream, value)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_align4(TextureReader* reader) {
    if (reader->status != UNITY_TEXTURE_OBJECT_OK) return false;
    if (!stream_align(&reader->stream, 4U)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool reader_bool(TextureReader* reader, bool* value) {
    uint8_t wire = 0U;
    if (!reader_u8(reader, &wire)) return false;
    if (wire > 1U) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_BOOLEAN_INVALID);
        return false;
    }
    *value = wire != 0U;
    return true;
}

static bool reader_string(TextureReader* reader,
                          UnityTextureByteView* value) {
    memset(value, 0, sizeof(*value));
    uint32_t size = 0U;
    if (!reader_u32(reader, &size)) return false;
    if (size > INT32_MAX) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_STRING_LENGTH_INVALID);
        return false;
    }
    if ((size_t)size > stream_remaining(&reader->stream)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_STRING_TRUNCATED);
        return false;
    }
    const uint8_t* bytes = reader->stream.data + reader->stream.position;
    if (memchr(bytes, 0, size)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_STRING_CONTAINS_NUL);
        return false;
    }
    if (!stream_skip(&reader->stream, size) || !reader_align4(reader)) {
        if (reader->status == UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED) {
            reader->status = UNITY_TEXTURE_OBJECT_STRING_TRUNCATED;
        }
        return false;
    }
    value->bytes = bytes;
    value->size = size;
    return true;
}

static bool reader_byte_array(TextureReader* reader,
                              UnityTextureByteView* value) {
    memset(value, 0, sizeof(*value));
    uint32_t size = 0U;
    if (!reader_u32(reader, &size)) return false;
    if ((size_t)size > stream_remaining(&reader->stream)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_ARRAY_LENGTH_INVALID);
        return false;
    }
    value->bytes = reader->stream.data + reader->stream.position;
    value->size = size;
    if (!stream_skip(&reader->stream, size) || !reader_align4(reader)) {
        reader_fail(reader, UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool parse_settings(TextureReader* reader,
                           UnityTextureSettings* settings) {
    return reader_i32(reader, &settings->filter_mode) &&
        reader_i32(reader, &settings->aniso) &&
        reader_u32(reader, &settings->mip_bias_bits) &&
        reader_i32(reader, &settings->wrap_u) &&
        reader_i32(reader, &settings->wrap_v) &&
        reader_i32(reader, &settings->wrap_w);
}

static bool parse_texture2d(TextureReader* reader,
                            UnityTexture2DObject* texture) {
    return reader_i32(reader, &texture->width) &&
        reader_i32(reader, &texture->height) &&
        reader_u32(reader, &texture->complete_image_size) &&
        reader_i32(reader, &texture->mips_stripped) &&
        reader_i32(reader, &texture->texture_format) &&
        reader_i32(reader, &texture->mip_count) &&
        reader_bool(reader, &texture->is_readable) &&
        reader_bool(reader, &texture->is_pre_processed) &&
        reader_bool(reader, &texture->ignore_master_texture_limit) &&
        reader_bool(reader, &texture->streaming_mipmaps) &&
        reader_align4(reader) &&
        reader_i32(reader, &texture->streaming_mipmaps_priority) &&
        reader_i32(reader, &texture->image_count) &&
        reader_i32(reader, &texture->texture_dimension) &&
        parse_settings(reader, &texture->settings) &&
        reader_i32(reader, &texture->lightmap_format) &&
        reader_i32(reader, &texture->color_space) &&
        reader_byte_array(reader, &texture->platform_blob) &&
        reader_byte_array(reader, &texture->inline_image_data) &&
        reader_u64(reader, &texture->stream_offset) &&
        reader_u32(reader, &texture->stream_size) &&
        reader_string(reader, &texture->stream_path);
}

static bool parse_render_texture(TextureReader* reader,
                                 UnityRenderTextureObject* texture) {
    return reader_i32(reader, &texture->width) &&
        reader_i32(reader, &texture->height) &&
        reader_i32(reader, &texture->anti_aliasing) &&
        reader_i32(reader, &texture->mip_count) &&
        reader_i32(reader, &texture->depth_stencil_format) &&
        reader_i32(reader, &texture->color_format) &&
        reader_bool(reader, &texture->mip_map) &&
        reader_bool(reader, &texture->generate_mips) &&
        reader_bool(reader, &texture->srgb) &&
        reader_bool(reader, &texture->use_dynamic_scale) &&
        reader_bool(reader, &texture->bind_ms) &&
        reader_bool(reader, &texture->enable_compatible_format) &&
        reader_bool(reader, &texture->enable_random_write) &&
        reader_align4(reader) &&
        parse_settings(reader, &texture->settings) &&
        reader_i32(reader, &texture->dimension) &&
        reader_i32(reader, &texture->volume_depth) &&
        reader_i32(reader, &texture->shadow_sampling_mode);
}

static bool model_is_valid(const UnityTextureObject* object) {
    if (!object) return false;
    if (object->class_id == UNITY_TEXTURE2D_CLASS_ID) {
        const UnityTexture2DObject* texture = &object->payload.texture2d;
        bool ordinary = texture->width > 0 && texture->height > 0 &&
            texture->mip_count > 0 && texture->image_count > 0;
        bool empty_font_shape = texture->width == 0 && texture->height == 0 &&
            texture->complete_image_size == 0U &&
            texture->mips_stripped == 0 && texture->mip_count == 1 &&
            texture->image_count == 0 &&
            texture->platform_blob.size == 0U &&
            texture->inline_image_data.size == 0U &&
            texture->stream_offset == 0U && texture->stream_size == 0U &&
            texture->stream_path.size == 0U;
        return texture->mips_stripped >= 0 &&
            (ordinary || empty_font_shape);
    }
    const UnityRenderTextureObject* texture =
        &object->payload.render_texture;
    return object->class_id == UNITY_RENDER_TEXTURE_CLASS_ID &&
        texture->width > 0 && texture->height > 0 &&
        texture->anti_aliasing > 0 && texture->volume_depth > 0;
}

void unity_texture_object_init(UnityTextureObject* object) {
    if (object) memset(object, 0, sizeof(*object));
}

UnityTextureObjectStatus unity_texture_object_decode_borrowed(
    UnityTextureObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object) {
    if (!destination) return UNITY_TEXTURE_OBJECT_INVALID_ARGUMENT;
    UnityTextureObjectStatus identity = validate_identity(file, object);
    if (identity != UNITY_TEXTURE_OBJECT_OK) return identity;

    size_t payload_size = 0U;
    const uint8_t* payload = serialized_file_get_object_data(
        (SerializedFile*)file, object, &payload_size);
    if (!payload || payload_size != object->byte_size) {
        return UNITY_TEXTURE_OBJECT_OBJECT_RANGE_INVALID;
    }

    UnityTextureObject pending;
    unity_texture_object_init(&pending);
    pending.class_id = object->type_id;
    pending.path_id = object->path_id;
    pending.target_platform = file->target_platform;
    pending.serialized_file_version = file->version;
    pending.serialized_big_endian = file->big_endian;
    const TypeTreeType* type = &file->types[object->type_id_or_index];
    memcpy(pending.serialized_type_hash, type->type_hash,
           sizeof(pending.serialized_type_hash));

    TextureReader reader;
    stream_init(&reader.stream, payload, payload_size);
    stream_set_endian(&reader.stream, file->big_endian);
    reader.status = UNITY_TEXTURE_OBJECT_OK;
    bool decoded = reader_string(&reader, &pending.name) &&
        reader_i32(&reader, &pending.forced_fallback_format) &&
        reader_bool(&reader, &pending.downscale_fallback) &&
        reader_bool(&reader, &pending.alpha_channel_optional) &&
        reader_align4(&reader);
    if (decoded && pending.class_id == UNITY_TEXTURE2D_CLASS_ID) {
        decoded = parse_texture2d(&reader, &pending.payload.texture2d);
    } else if (decoded) {
        decoded = parse_render_texture(
            &reader, &pending.payload.render_texture);
    }
    if (!decoded) return reader.status;
    if (stream_remaining(&reader.stream) != 0U) {
        return UNITY_TEXTURE_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED;
    }
    if (!model_is_valid(&pending)) {
        return UNITY_TEXTURE_OBJECT_MODEL_INVALID;
    }
    pending.decoded = true;
    *destination = pending;
    return UNITY_TEXTURE_OBJECT_OK;
}

bool unity_texture_object_has_layout_authority(
    const UnityTextureObject* object) {
    if (!object || !object->decoded ||
        object->serialized_file_version !=
            UNITY_TEXTURE_SERIALIZED_FILE_VERSION ||
        object->serialized_big_endian ||
        (object->target_platform != 5U && object->target_platform != 19U)) {
        return false;
    }
    const uint8_t* expected_hash = expected_hash_for_class(object->class_id);
    return expected_hash &&
        memcmp(object->serialized_type_hash, expected_hash, 16U) == 0;
}

const char* unity_texture_object_status_name(UnityTextureObjectStatus status) {
    switch (status) {
        case UNITY_TEXTURE_OBJECT_NOT_APPLICABLE: return "not-applicable";
        case UNITY_TEXTURE_OBJECT_OK: return "ok";
        case UNITY_TEXTURE_OBJECT_INVALID_ARGUMENT: return "invalid-argument";
        case UNITY_TEXTURE_OBJECT_INVALID_SOURCE_FILE:
            return "invalid-source-file";
        case UNITY_TEXTURE_OBJECT_UNSUPPORTED_FILE_VERSION:
            return "unsupported-file-version";
        case UNITY_TEXTURE_OBJECT_UNSUPPORTED_UNITY_VERSION:
            return "unsupported-unity-version";
        case UNITY_TEXTURE_OBJECT_UNSUPPORTED_TARGET_PLATFORM:
            return "unsupported-target-platform";
        case UNITY_TEXTURE_OBJECT_UNSUPPORTED_BYTE_ORDER:
            return "unsupported-byte-order";
        case UNITY_TEXTURE_OBJECT_OBJECT_NOT_OWNED: return "object-not-owned";
        case UNITY_TEXTURE_OBJECT_OBJECT_RANGE_INVALID:
            return "object-range-invalid";
        case UNITY_TEXTURE_OBJECT_CLASS_UNSUPPORTED:
            return "class-unsupported";
        case UNITY_TEXTURE_OBJECT_TYPE_INDEX_INVALID:
            return "type-index-invalid";
        case UNITY_TEXTURE_OBJECT_TYPE_RECORD_MISMATCH:
            return "type-record-mismatch";
        case UNITY_TEXTURE_OBJECT_TYPE_IDENTITY_UNSUPPORTED:
            return "type-identity-unsupported";
        case UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED:
            return "payload-truncated";
        case UNITY_TEXTURE_OBJECT_STRING_LENGTH_INVALID:
            return "string-length-invalid";
        case UNITY_TEXTURE_OBJECT_STRING_TRUNCATED:
            return "string-truncated";
        case UNITY_TEXTURE_OBJECT_STRING_CONTAINS_NUL:
            return "string-contains-nul";
        case UNITY_TEXTURE_OBJECT_ARRAY_LENGTH_INVALID:
            return "array-length-invalid";
        case UNITY_TEXTURE_OBJECT_BOOLEAN_INVALID: return "boolean-invalid";
        case UNITY_TEXTURE_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED:
            return "object-bytes-not-exhausted";
        case UNITY_TEXTURE_OBJECT_MODEL_INVALID: return "model-invalid";
    }
    return "unknown";
}
