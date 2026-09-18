// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_TEXTURE_OBJECT_H
#define UNITY_TEXTURE_OBJECT_H

#include "io/serialized_file.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNITY_TEXTURE2D_CLASS_ID 28
#define UNITY_RENDER_TEXTURE_CLASS_ID 84
#define UNITY_TEXTURE2D_LOCAL_FILE_ID INT64_C(2800000)
#define UNITY_RENDER_TEXTURE_LOCAL_FILE_ID INT64_C(8400000)

/* Unity 2021.3.35f1 authored the exact Windows-player TypeTrees used by this
 * decoder.  The pinned bundle and schema-shape digests are documented in
 * README.md.  A TypeTree-disabled player object is admitted only
 * when its complete SerializedFile identity carries the corresponding exact
 * Unity type hash. */
#define UNITY_TEXTURE2D_LAYOUT_AUTHORITY \
    "unity-2021.3-player-class28-0d08414cfd5bdb0d22792011bda9ab26"
#define UNITY_RENDER_TEXTURE_LAYOUT_AUTHORITY \
    "unity-2021.3-player-class84-ae5918d78668297f97b98e4b8b72cfc2"
#define UNITY_TEXTURE2D_LAYOUT_NODE_COUNT 44U
#define UNITY_RENDER_TEXTURE_LAYOUT_NODE_COUNT 31U

typedef struct {
    const uint8_t* bytes;
    size_t size;
} UnityTextureByteView;

typedef struct {
    int32_t filter_mode;
    int32_t aniso;
    uint32_t mip_bias_bits;
    int32_t wrap_u;
    int32_t wrap_v;
    int32_t wrap_w;
} UnityTextureSettings;

typedef struct {
    int32_t width;
    int32_t height;
    uint32_t complete_image_size;
    int32_t mips_stripped;
    int32_t texture_format;
    int32_t mip_count;
    bool is_readable;
    bool is_pre_processed;
    bool ignore_master_texture_limit;
    bool streaming_mipmaps;
    int32_t streaming_mipmaps_priority;
    int32_t image_count;
    int32_t texture_dimension;
    UnityTextureSettings settings;
    int32_t lightmap_format;
    int32_t color_space;
    UnityTextureByteView platform_blob;
    UnityTextureByteView inline_image_data;
    uint64_t stream_offset;
    uint32_t stream_size;
    UnityTextureByteView stream_path;
} UnityTexture2DObject;

typedef struct {
    int32_t width;
    int32_t height;
    int32_t anti_aliasing;
    int32_t mip_count;
    int32_t depth_stencil_format;
    int32_t color_format;
    bool mip_map;
    bool generate_mips;
    bool srgb;
    bool use_dynamic_scale;
    bool bind_ms;
    bool enable_compatible_format;
    bool enable_random_write;
    UnityTextureSettings settings;
    int32_t dimension;
    int32_t volume_depth;
    int32_t shadow_sampling_mode;
} UnityRenderTextureObject;

typedef struct {
    UnityTextureByteView name;
    int32_t forced_fallback_format;
    bool downscale_fallback;
    bool alpha_channel_optional;
    int32_t class_id;
    int64_t path_id;
    uint32_t target_platform;
    uint32_t serialized_file_version;
    bool serialized_big_endian;
    uint8_t serialized_type_hash[16];
    union {
        UnityTexture2DObject texture2d;
        UnityRenderTextureObject render_texture;
    } payload;
    bool decoded;
} UnityTextureObject;

typedef enum {
    UNITY_TEXTURE_OBJECT_NOT_APPLICABLE = 0,
    UNITY_TEXTURE_OBJECT_OK,
    UNITY_TEXTURE_OBJECT_INVALID_ARGUMENT,
    UNITY_TEXTURE_OBJECT_INVALID_SOURCE_FILE,
    UNITY_TEXTURE_OBJECT_UNSUPPORTED_FILE_VERSION,
    UNITY_TEXTURE_OBJECT_UNSUPPORTED_UNITY_VERSION,
    UNITY_TEXTURE_OBJECT_UNSUPPORTED_TARGET_PLATFORM,
    UNITY_TEXTURE_OBJECT_UNSUPPORTED_BYTE_ORDER,
    UNITY_TEXTURE_OBJECT_OBJECT_NOT_OWNED,
    UNITY_TEXTURE_OBJECT_OBJECT_RANGE_INVALID,
    UNITY_TEXTURE_OBJECT_CLASS_UNSUPPORTED,
    UNITY_TEXTURE_OBJECT_TYPE_INDEX_INVALID,
    UNITY_TEXTURE_OBJECT_TYPE_RECORD_MISMATCH,
    UNITY_TEXTURE_OBJECT_TYPE_IDENTITY_UNSUPPORTED,
    UNITY_TEXTURE_OBJECT_PAYLOAD_TRUNCATED,
    UNITY_TEXTURE_OBJECT_STRING_LENGTH_INVALID,
    UNITY_TEXTURE_OBJECT_STRING_TRUNCATED,
    UNITY_TEXTURE_OBJECT_STRING_CONTAINS_NUL,
    UNITY_TEXTURE_OBJECT_ARRAY_LENGTH_INVALID,
    UNITY_TEXTURE_OBJECT_BOOLEAN_INVALID,
    UNITY_TEXTURE_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED,
    UNITY_TEXTURE_OBJECT_MODEL_INVALID,
} UnityTextureObjectStatus;

void unity_texture_object_init(UnityTextureObject* object);

/* The decoded byte/string views borrow SerializedFile.raw_data.  Failure has
 * a strong output guarantee and leaves destination initialized/unchanged. */
UnityTextureObjectStatus unity_texture_object_decode_borrowed(
    UnityTextureObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object);

bool unity_texture_object_has_layout_authority(
    const UnityTextureObject* object);

const char* unity_texture_object_status_name(UnityTextureObjectStatus status);

#endif /* UNITY_TEXTURE_OBJECT_H */
