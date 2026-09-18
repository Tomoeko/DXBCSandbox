// SPDX-License-Identifier: GPL-3.0-only

#ifndef NATIVE_TEXTURE_YAML_EMITTER_H
#define NATIVE_TEXTURE_YAML_EMITTER_H

#include "common/string_builder.h"
#include "io/unity_texture_object.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    UNITY_NATIVE_TEXTURE_YAML_OK = 0,
    UNITY_NATIVE_TEXTURE_YAML_INVALID_ARGUMENT,
    UNITY_NATIVE_TEXTURE_YAML_NOT_DECODED,
    UNITY_NATIVE_TEXTURE_YAML_LAYOUT_AUTHORITY_MISSING,
    UNITY_NATIVE_TEXTURE_YAML_EXPORT_SHAPE_UNSUPPORTED,
    UNITY_NATIVE_TEXTURE_YAML_INVALID_UTF8,
    UNITY_NATIVE_TEXTURE_YAML_PIXEL_SIZE_MISMATCH,
    UNITY_NATIVE_TEXTURE_YAML_INVALID_GUID,
    UNITY_NATIVE_TEXTURE_YAML_OUTPUT_FAILED,
} UnityNativeTextureYamlStatus;

/* Pinned Unity 2021.3 production capability gate.  This is deliberately
 * narrower than the wire decoder.  Descriptor dimensions follow Unity's
 * independent positive-width/height API contract; all other admitted field
 * combinations remain within the Unity-authored import/reload profile. */
bool unity_native_texture_yaml_export_shape_is_supported(
    const UnityTextureObject* object);

/* Emits one Unity 2021.3 native Texture2D or RenderTexture document.  Pixel
 * bytes are required only for Texture2D and are written as lowercase inline
 * _typelessdata; m_StreamData is deliberately cleared.  Editor-only fields
 * absent from the player TypeTree use Unity's native-asset defaults. */
UnityNativeTextureYamlStatus unity_native_texture_yaml_emit(
    const UnityTextureObject* object, const uint8_t* pixel_bytes,
    size_t pixel_size, StringBuilder* output);

/* Emits the Unity-authored NativeFormatImporter shape with the exact main
 * object file ID established by AssetDatabase for ClassID 28/84. */
UnityNativeTextureYamlStatus unity_native_texture_meta_emit(
    int32_t class_id, const char* guid, StringBuilder* output);

const char* unity_native_texture_yaml_status_name(
    UnityNativeTextureYamlStatus status);

#endif /* NATIVE_TEXTURE_YAML_EMITTER_H */
