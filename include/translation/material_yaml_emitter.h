// SPDX-License-Identifier: GPL-3.0-only

#ifndef MATERIAL_YAML_EMITTER_H
#define MATERIAL_YAML_EMITTER_H

#include "common/string_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    UNITY_MATERIAL_YAML_OK = 0,
    UNITY_MATERIAL_YAML_INVALID_ARGUMENT,
    UNITY_MATERIAL_YAML_INVALID_UTF8,
    UNITY_MATERIAL_YAML_INVALID_GUID,
    UNITY_MATERIAL_YAML_UNRESOLVED_REFERENCE,
    UNITY_MATERIAL_YAML_DUPLICATE_KEY,
    UNITY_MATERIAL_YAML_OUTPUT_FAILED
} UnityMaterialYamlStatus;

const char* unity_material_yaml_status_name(UnityMaterialYamlStatus status);

/* Serialized Material strings are length-delimited.  Keeping that boundary
 * here avoids hidden strlen assumptions and permits zero-copy projection from
 * the decoded TypeTree.  Empty strings may use {NULL, 0}. */
typedef struct {
    const uint8_t* bytes;
    size_t size;
} UnityMaterialYamlString;

/* A null reference must have every payload field zero/empty.  A non-null
 * reference is considered resolved only when it has a nonzero file ID, a
 * valid lowercase Unity GUID, and an explicit Unity reference type in [0,3].
 * This prevents an unresolved non-null serialized PPtr from degrading into
 * {fileID: 0} during export. */
typedef struct {
    bool is_null;
    int64_t file_id;
    const char* guid;
    int32_t type;
} UnityMaterialYamlReference;

typedef struct {
    UnityMaterialYamlString key;
    UnityMaterialYamlString value;
} UnityMaterialYamlStringPair;

typedef struct {
    UnityMaterialYamlString name;
    int32_t value;
} UnityMaterialYamlIntProperty;

typedef struct {
    UnityMaterialYamlString name;
    uint32_t value_bits;
} UnityMaterialYamlFloatProperty;

typedef struct {
    UnityMaterialYamlString name;
    uint32_t red_bits;
    uint32_t green_bits;
    uint32_t blue_bits;
    uint32_t alpha_bits;
} UnityMaterialYamlColorProperty;

typedef struct {
    UnityMaterialYamlString name;
    UnityMaterialYamlReference texture;
    uint32_t scale_x_bits;
    uint32_t scale_y_bits;
    uint32_t offset_x_bits;
    uint32_t offset_y_bits;
} UnityMaterialYamlTextureProperty;

typedef struct {
    UnityMaterialYamlString group_name;
    UnityMaterialYamlString item_name;
} UnityMaterialYamlBuildTextureStack;

/* Every array is an explicitly ordered vector.  Emission preserves that
 * order byte-for-byte and rejects duplicate keys in map-like vectors.  This
 * keeps the serialized TypeTree order authoritative rather than relying on a
 * process-specific hash-table traversal. */
typedef struct {
    UnityMaterialYamlString name;
    UnityMaterialYamlReference shader;

    const UnityMaterialYamlString* valid_keywords;
    size_t valid_keyword_count;
    const UnityMaterialYamlString* invalid_keywords;
    size_t invalid_keyword_count;
    uint32_t lightmap_flags;
    bool enable_instancing_variants;
    bool double_sided_gi;
    int32_t custom_render_queue;

    const UnityMaterialYamlStringPair* string_tags;
    size_t string_tag_count;
    const UnityMaterialYamlString* disabled_shader_passes;
    size_t disabled_shader_pass_count;

    const UnityMaterialYamlTextureProperty* textures;
    size_t texture_count;
    const UnityMaterialYamlIntProperty* ints;
    size_t int_count;
    const UnityMaterialYamlFloatProperty* floats;
    size_t float_count;
    const UnityMaterialYamlColorProperty* colors;
    size_t color_count;
    const UnityMaterialYamlBuildTextureStack* build_texture_stacks;
    size_t build_texture_stack_count;
} UnityMaterialYamlDocument;

/* Append a Unity 2021.3 Material YAML document (Material serializedVersion 8,
 * UnityPropertySheet serializedVersion 3).  The function validates the full
 * document before appending any bytes. */
UnityMaterialYamlStatus unity_material_yaml_emit(
    const UnityMaterialYamlDocument* document,
    StringBuilder* output);

/* Deterministic Unity 2021.3 .meta emitters for generated Shader and Material
 * assets. */
UnityMaterialYamlStatus unity_shader_importer_meta_emit(
    const char* guid,
    StringBuilder* output);
UnityMaterialYamlStatus unity_material_native_meta_emit(
    const char* guid,
    StringBuilder* output);

#endif /* MATERIAL_YAML_EMITTER_H */
