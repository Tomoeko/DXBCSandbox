// SPDX-License-Identifier: GPL-3.0-only

#include "translation/native_texture_yaml_emitter.h"

#include "common/unity_asset_guid.h"
#include "translation/unity_yaml.h"

#include <inttypes.h>
#include <limits.h>

static bool integer_is_one_of(int32_t value, const int32_t* values,
                              size_t count) {
    for (size_t index = 0U; index < count; ++index) {
        if (value == values[index]) return true;
    }
    return false;
}

static bool stream_path_is_safe_basename(UnityTextureByteView path) {
    if (!path.bytes || path.size == 0U || path.size > 255U) return false;
    if ((path.size == 1U && path.bytes[0] == '.') ||
        (path.size == 2U && path.bytes[0] == '.' && path.bytes[1] == '.')) {
        return false;
    }
    for (size_t index = 0U; index < path.size; ++index) {
        uint8_t value = path.bytes[index];
        if (value == 0U || value == '/' || value == ':'
#ifdef _WIN32
            || value == '\\'
#endif
        ) {
            return false;
        }
    }
    return true;
}

static bool common_texture_fields_are_supported(
    const UnityTextureObject* object) {
    return object->forced_fallback_format == 4 &&
        !object->downscale_fallback;
}

static bool checked_mul_u64(uint64_t left, uint64_t right,
                            uint64_t* product) {
    if (!product || (right != 0U && left > UINT64_MAX / right)) {
        return false;
    }
    *product = left * right;
    return true;
}

/* Unity's proved player formats store a complete, tightly packed mip chain.
 * Block-compressed levels round each axis up to a complete 4x4 block. */
static bool texture2d_mip_chain_size_is_coherent(
    const UnityTexture2DObject* texture) {
    if (!texture || texture->width <= 0 || texture->height <= 0 ||
        texture->mip_count <= 0) {
        return false;
    }
    uint64_t width = (uint32_t)texture->width;
    uint64_t height = (uint32_t)texture->height;
    uint64_t total = 0U;
    for (int32_t level = 0; level < texture->mip_count; ++level) {
        uint64_t level_size = 0U;
        if (texture->texture_format == 3 ||
            texture->texture_format == 4) {
            uint64_t pixels = 0U;
            uint64_t bytes_per_pixel =
                texture->texture_format == 3 ? 3U : 4U;
            if (!checked_mul_u64(width, height, &pixels) ||
                !checked_mul_u64(pixels, bytes_per_pixel, &level_size)) {
                return false;
            }
        } else {
            uint64_t block_width = (width + 3U) / 4U;
            uint64_t block_height = (height + 3U) / 4U;
            uint64_t blocks = 0U;
            uint64_t bytes_per_block =
                texture->texture_format == 10 ? 8U : 16U;
            if (!checked_mul_u64(block_width, block_height, &blocks) ||
                !checked_mul_u64(blocks, bytes_per_block, &level_size)) {
                return false;
            }
        }
        if (level_size > UINT32_MAX || total > UINT32_MAX - level_size) {
            return false;
        }
        total += level_size;
        if (level + 1 < texture->mip_count && width == 1U && height == 1U) {
            return false;
        }
        if (width > 1U) width /= 2U;
        if (height > 1U) height /= 2U;
    }
    return total == texture->complete_image_size;
}

static bool texture2d_shape_is_supported(
    const UnityTextureObject* object) {
    static const int32_t formats[] = {3, 4, 10, 12};
    static const int32_t filters[] = {0, 1};
    static const int32_t wraps[] = {0, 1};
    static const int32_t lightmaps[] = {0, 3, 6};
    static const int32_t color_spaces[] = {0, 1};
    const UnityTexture2DObject* texture = &object->payload.texture2d;
    bool common = common_texture_fields_are_supported(object) &&
        texture->mips_stripped == 0 && !texture->is_readable &&
        !texture->is_pre_processed &&
        !texture->ignore_master_texture_limit &&
        !texture->streaming_mipmaps &&
        texture->streaming_mipmaps_priority == 0 &&
        texture->texture_dimension == 2 && texture->settings.aniso == 1 &&
        texture->settings.mip_bias_bits == 0U &&
        texture->platform_blob.size == 0U &&
        texture->inline_image_data.size == 0U &&
        integer_is_one_of(texture->texture_format, formats,
                          sizeof(formats) / sizeof(formats[0])) &&
        integer_is_one_of(texture->settings.filter_mode, filters,
                          sizeof(filters) / sizeof(filters[0])) &&
        integer_is_one_of(texture->settings.wrap_u, wraps,
                          sizeof(wraps) / sizeof(wraps[0])) &&
        integer_is_one_of(texture->settings.wrap_v, wraps,
                          sizeof(wraps) / sizeof(wraps[0])) &&
        integer_is_one_of(texture->settings.wrap_w, wraps,
                          sizeof(wraps) / sizeof(wraps[0])) &&
        integer_is_one_of(texture->lightmap_format, lightmaps,
                          sizeof(lightmaps) / sizeof(lightmaps[0])) &&
        integer_is_one_of(texture->color_space, color_spaces,
                          sizeof(color_spaces) / sizeof(color_spaces[0]));
    bool streamed = texture->width > 0 && texture->height > 0 &&
        texture->complete_image_size != 0U &&
        texture->complete_image_size == texture->stream_size &&
        texture->mip_count > 0 && texture->image_count == 1 &&
        texture2d_mip_chain_size_is_coherent(texture) &&
        stream_path_is_safe_basename(texture->stream_path);
    bool coherent_empty = texture->width == 0 && texture->height == 0 &&
        texture->complete_image_size == 0U && texture->texture_format == 4 &&
        texture->mip_count == 1 && texture->image_count == 0 &&
        texture->stream_offset == 0U && texture->stream_size == 0U &&
        texture->stream_path.size == 0U;
    return common && (streamed || coherent_empty);
}

static bool render_texture_shape_is_supported(
    const UnityTextureObject* object) {
    static const int32_t filters[] = {0, 1};
    static const int32_t depth_stencil_formats[] = {0, 90, 94};
    const UnityRenderTextureObject* texture =
        &object->payload.render_texture;
    return common_texture_fields_are_supported(object) &&
        !object->alpha_channel_optional &&
        texture->width > 0 && texture->height > 0 &&
        texture->anti_aliasing == 1 &&
        texture->mip_count == -1 &&
        integer_is_one_of(texture->depth_stencil_format,
                          depth_stencil_formats,
                          sizeof(depth_stencil_formats) /
                              sizeof(depth_stencil_formats[0])) &&
        texture->color_format == 8 && !texture->mip_map &&
        texture->generate_mips && !texture->srgb &&
        !texture->use_dynamic_scale && !texture->bind_ms &&
        !texture->enable_random_write &&
        integer_is_one_of(texture->settings.filter_mode, filters,
                          sizeof(filters) / sizeof(filters[0])) &&
        texture->settings.aniso == 0 &&
        texture->settings.mip_bias_bits == 0U &&
        texture->settings.wrap_u == 1 && texture->settings.wrap_v == 1 &&
        texture->settings.wrap_w == 1 && texture->dimension == 2 &&
        texture->volume_depth == 1 && texture->shadow_sampling_mode == 2;
}

bool unity_native_texture_yaml_export_shape_is_supported(
    const UnityTextureObject* object) {
    if (!object || !unity_texture_object_has_layout_authority(object)) {
        return false;
    }
    if (object->class_id == UNITY_TEXTURE2D_CLASS_ID) {
        return texture2d_shape_is_supported(object);
    }
    return object->class_id == UNITY_RENDER_TEXTURE_CLASS_ID &&
        render_texture_shape_is_supported(object);
}

static void append_name(StringBuilder* output, UnityTextureByteView name) {
    (void)unity_yaml_append_quoted_n(output, name.bytes, name.size);
}

static void append_settings(StringBuilder* output,
                            const UnityTextureSettings* settings) {
    sb_appendf(output,
        "  m_TextureSettings:\n"
        "    serializedVersion: 2\n"
        "    m_FilterMode: %" PRId32 "\n"
        "    m_Aniso: %" PRId32 "\n"
        "    m_MipBias: ",
        settings->filter_mode, settings->aniso);
    (void)unity_yaml_append_float32(output, settings->mip_bias_bits);
    sb_appendf(output,
        "\n    m_WrapU: %" PRId32 "\n"
        "    m_WrapV: %" PRId32 "\n"
        "    m_WrapW: %" PRId32 "\n",
        settings->wrap_u, settings->wrap_v, settings->wrap_w);
}

static void append_hex(StringBuilder* output, const uint8_t* bytes,
                       size_t size) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0U; index < size && sb_ok(output); ++index) {
        sb_append_char(output, digits[bytes[index] >> 4U]);
        sb_append_char(output, digits[bytes[index] & 0x0fU]);
    }
}

static void append_common_header(StringBuilder* output,
                                 const UnityTextureObject* object,
                                 int64_t local_file_id,
                                 const char* type_name) {
    sb_append(output,
        "%YAML 1.1\n"
        "%TAG !u! tag:unity3d.com,2011:\n");
    sb_appendf(output, "--- !u!%" PRId32 " &%" PRId64 "\n%s:\n",
               object->class_id, local_file_id, type_name);
    sb_append(output,
        "  m_ObjectHideFlags: 0\n"
        "  m_CorrespondingSourceObject: {fileID: 0}\n"
        "  m_PrefabInstance: {fileID: 0}\n"
        "  m_PrefabAsset: {fileID: 0}\n"
        "  m_Name: ");
    append_name(output, object->name);
    sb_append(output,
        "\n  m_ImageContentsHash:\n"
        "    serializedVersion: 2\n"
        "    Hash: 00000000000000000000000000000000\n");
    sb_appendf(output,
        "  m_ForcedFallbackFormat: %" PRId32 "\n"
        "  m_DownscaleFallback: %d\n"
        "  m_IsAlphaChannelOptional: %d\n",
        object->forced_fallback_format,
        object->downscale_fallback ? 1 : 0,
        object->alpha_channel_optional ? 1 : 0);
}

static void append_texture2d(StringBuilder* output,
                             const UnityTextureObject* object,
                             const uint8_t* pixels, size_t pixel_size) {
    const UnityTexture2DObject* texture = &object->payload.texture2d;
    append_common_header(output, object, UNITY_TEXTURE2D_LOCAL_FILE_ID,
                         "Texture2D");
    sb_appendf(output,
        "  serializedVersion: 2\n"
        "  m_Width: %" PRId32 "\n"
        "  m_Height: %" PRId32 "\n"
        "  m_CompleteImageSize: %" PRIu32 "\n"
        "  m_MipsStripped: %" PRId32 "\n"
        "  m_TextureFormat: %" PRId32 "\n"
        "  m_MipCount: %" PRId32 "\n"
        "  m_IsReadable: %d\n"
        "  m_IsPreProcessed: %d\n"
        "  m_IgnoreMasterTextureLimit: %d\n"
        "  m_StreamingMipmaps: %d\n"
        "  m_StreamingMipmapsPriority: %" PRId32 "\n"
        "  m_VTOnly: 0\n"
        "  m_AlphaIsTransparency: 0\n"
        "  m_ImageCount: %" PRId32 "\n"
        "  m_TextureDimension: %" PRId32 "\n",
        texture->width, texture->height, texture->complete_image_size,
        texture->mips_stripped, texture->texture_format,
        texture->mip_count, texture->is_readable ? 1 : 0,
        texture->is_pre_processed ? 1 : 0,
        texture->ignore_master_texture_limit ? 1 : 0,
        texture->streaming_mipmaps ? 1 : 0,
        texture->streaming_mipmaps_priority, texture->image_count,
        texture->texture_dimension);
    append_settings(output, &texture->settings);
    sb_appendf(output,
        "  m_LightmapFormat: %" PRId32 "\n"
        "  m_ColorSpace: %" PRId32 "\n"
        "  m_PlatformBlob: \n"
        "  image data: %zu\n"
        "  _typelessdata: ",
        texture->lightmap_format, texture->color_space, pixel_size);
    append_hex(output, pixels, pixel_size);
    sb_append(output,
        "\n  m_StreamData:\n"
        "    serializedVersion: 2\n"
        "    offset: 0\n"
        "    size: 0\n"
        "    path: \n");
}

static void append_render_texture(StringBuilder* output,
                                  const UnityTextureObject* object) {
    const UnityRenderTextureObject* texture =
        &object->payload.render_texture;
    append_common_header(output, object, UNITY_RENDER_TEXTURE_LOCAL_FILE_ID,
                         "RenderTexture");
    sb_appendf(output,
        "  serializedVersion: 5\n"
        "  m_Width: %" PRId32 "\n"
        "  m_Height: %" PRId32 "\n"
        "  m_AntiAliasing: %" PRId32 "\n"
        "  m_MipCount: %" PRId32 "\n"
        "  m_DepthStencilFormat: %" PRId32 "\n"
        "  m_ColorFormat: %" PRId32 "\n"
        "  m_MipMap: %d\n"
        "  m_GenerateMips: %d\n"
        "  m_SRGB: %d\n"
        "  m_UseDynamicScale: %d\n"
        "  m_BindMS: %d\n"
        "  m_EnableCompatibleFormat: %d\n"
        "  m_EnableRandomWrite: %d\n",
        texture->width, texture->height, texture->anti_aliasing,
        texture->mip_count, texture->depth_stencil_format,
        texture->color_format, texture->mip_map ? 1 : 0,
        texture->generate_mips ? 1 : 0, texture->srgb ? 1 : 0,
        texture->use_dynamic_scale ? 1 : 0,
        texture->bind_ms ? 1 : 0,
        texture->enable_compatible_format ? 1 : 0,
        texture->enable_random_write ? 1 : 0);
    append_settings(output, &texture->settings);
    sb_appendf(output,
        "  m_Dimension: %" PRId32 "\n"
        "  m_VolumeDepth: %" PRId32 "\n"
        "  m_ShadowSamplingMode: %" PRId32 "\n",
        texture->dimension, texture->volume_depth,
        texture->shadow_sampling_mode);
}

UnityNativeTextureYamlStatus unity_native_texture_yaml_emit(
    const UnityTextureObject* object, const uint8_t* pixel_bytes,
    size_t pixel_size, StringBuilder* output) {
    if (!object || !output || (!pixel_bytes && pixel_size != 0U)) {
        return UNITY_NATIVE_TEXTURE_YAML_INVALID_ARGUMENT;
    }
    if (!object->decoded) return UNITY_NATIVE_TEXTURE_YAML_NOT_DECODED;
    if (!unity_texture_object_has_layout_authority(object)) {
        return UNITY_NATIVE_TEXTURE_YAML_LAYOUT_AUTHORITY_MISSING;
    }
    if (!unity_native_texture_yaml_export_shape_is_supported(object)) {
        return UNITY_NATIVE_TEXTURE_YAML_EXPORT_SHAPE_UNSUPPORTED;
    }
    if (!unity_yaml_utf8_is_valid(object->name.bytes, object->name.size)) {
        return UNITY_NATIVE_TEXTURE_YAML_INVALID_UTF8;
    }
    if (object->class_id == UNITY_TEXTURE2D_CLASS_ID) {
        if (pixel_size != object->payload.texture2d.complete_image_size) {
            return UNITY_NATIVE_TEXTURE_YAML_PIXEL_SIZE_MISMATCH;
        }
        append_texture2d(output, object, pixel_bytes, pixel_size);
    } else if (object->class_id == UNITY_RENDER_TEXTURE_CLASS_ID) {
        if (pixel_size != 0U) {
            return UNITY_NATIVE_TEXTURE_YAML_PIXEL_SIZE_MISMATCH;
        }
        append_render_texture(output, object);
    } else {
        return UNITY_NATIVE_TEXTURE_YAML_LAYOUT_AUTHORITY_MISSING;
    }
    return sb_ok(output) ? UNITY_NATIVE_TEXTURE_YAML_OK
                         : UNITY_NATIVE_TEXTURE_YAML_OUTPUT_FAILED;
}

UnityNativeTextureYamlStatus unity_native_texture_meta_emit(
    int32_t class_id, const char* guid, StringBuilder* output) {
    if (!guid || !output) return UNITY_NATIVE_TEXTURE_YAML_INVALID_ARGUMENT;
    if (!unity_asset_guid_is_valid(guid)) {
        return UNITY_NATIVE_TEXTURE_YAML_INVALID_GUID;
    }
    int64_t file_id = class_id == UNITY_TEXTURE2D_CLASS_ID
        ? UNITY_TEXTURE2D_LOCAL_FILE_ID
        : class_id == UNITY_RENDER_TEXTURE_CLASS_ID
            ? UNITY_RENDER_TEXTURE_LOCAL_FILE_ID : 0;
    if (file_id == 0) {
        return UNITY_NATIVE_TEXTURE_YAML_LAYOUT_AUTHORITY_MISSING;
    }
    sb_append(output, "fileFormatVersion: 2\nguid: ");
    sb_append(output, guid);
    sb_appendf(output,
        "\nNativeFormatImporter:\n"
        "  externalObjects: {}\n"
        "  mainObjectFileID: %" PRId64 "\n"
        "  userData: \n"
        "  assetBundleName: \n"
        "  assetBundleVariant: \n",
        file_id);
    return sb_ok(output) ? UNITY_NATIVE_TEXTURE_YAML_OK
                         : UNITY_NATIVE_TEXTURE_YAML_OUTPUT_FAILED;
}

const char* unity_native_texture_yaml_status_name(
    UnityNativeTextureYamlStatus status) {
    switch (status) {
        case UNITY_NATIVE_TEXTURE_YAML_OK: return "ok";
        case UNITY_NATIVE_TEXTURE_YAML_INVALID_ARGUMENT:
            return "invalid-argument";
        case UNITY_NATIVE_TEXTURE_YAML_NOT_DECODED: return "not-decoded";
        case UNITY_NATIVE_TEXTURE_YAML_LAYOUT_AUTHORITY_MISSING:
            return "layout-authority-missing";
        case UNITY_NATIVE_TEXTURE_YAML_EXPORT_SHAPE_UNSUPPORTED:
            return "export-shape-unsupported";
        case UNITY_NATIVE_TEXTURE_YAML_INVALID_UTF8: return "invalid-utf8";
        case UNITY_NATIVE_TEXTURE_YAML_PIXEL_SIZE_MISMATCH:
            return "pixel-size-mismatch";
        case UNITY_NATIVE_TEXTURE_YAML_INVALID_GUID: return "invalid-guid";
        case UNITY_NATIVE_TEXTURE_YAML_OUTPUT_FAILED: return "output-failed";
    }
    return "unknown";
}
