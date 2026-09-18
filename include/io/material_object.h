// SPDX-License-Identifier: GPL-3.0-only

#ifndef MATERIAL_OBJECT_H
#define MATERIAL_OBJECT_H

#include "io/serialized_file.h"

/*
 * Exact Unity 2021.3.29f1 / 2021.3.35f1 player Material (ClassID 21)
 * boundary.
 *
 * The decoder accepts only the complete 107-node Material v8 / PropertySheet
 * v3 TypeTree and its exact Unity serialized type identity.  It never infers
 * a layout from object bytes.  Strings in the projected model borrow the
 * owned TypeTreeValue tree; every other projected allocation is owned by the
 * MaterialObject.  A successfully decoded object therefore outlives its
 * source SerializedFile.
 *
 * Floating-point values are exposed as raw IEEE-754 binary32 bits.  This is
 * intentional: converting through double would discard distinctions needed
 * by an exact exporter, including negative zero and NaN payloads.
 */
#define MATERIAL_OBJECT_LAYOUT_AUTHORITY \
    "unity-2021.3.29f1+2021.3.35f1-player-class21-c60098ac66a28b50aa0580db11bf018c"
#define MATERIAL_OBJECT_LAYOUT_NODE_COUNT 107U
#define MATERIAL_OBJECT_SERIALIZED_VERSION 8U
#define MATERIAL_PROPERTY_SHEET_SERIALIZED_VERSION 3U

typedef struct {
    const uint8_t* bytes;
    size_t size;
} MaterialStringView;

typedef struct {
    uint32_t bits;
} MaterialFloat32;

typedef struct {
    MaterialFloat32 x;
    MaterialFloat32 y;
} MaterialVector2;

typedef struct {
    MaterialFloat32 r;
    MaterialFloat32 g;
    MaterialFloat32 b;
    MaterialFloat32 a;
} MaterialColor;

typedef struct {
    MaterialStringView key;
    MaterialStringView value;
} MaterialStringTag;

typedef struct {
    MaterialStringView name;
    AssetPPtr texture;
    MaterialVector2 scale;
    MaterialVector2 offset;
} MaterialTextureProperty;

typedef struct {
    MaterialStringView name;
    int32_t value;
} MaterialIntProperty;

typedef struct {
    MaterialStringView name;
    MaterialFloat32 value;
} MaterialFloatProperty;

typedef struct {
    MaterialStringView name;
    MaterialColor value;
} MaterialColorProperty;

typedef struct {
    MaterialStringView group_name;
    MaterialStringView item_name;
} MaterialTextureStackReference;

typedef enum {
    MATERIAL_OBJECT_OK = 0,
    MATERIAL_OBJECT_INVALID_ARGUMENT,
    MATERIAL_OBJECT_INVALID_SOURCE_FILE,
    MATERIAL_OBJECT_UNSUPPORTED_FILE_VERSION,
    MATERIAL_OBJECT_UNSUPPORTED_UNITY_VERSION,
    MATERIAL_OBJECT_OBJECT_NOT_OWNED,
    MATERIAL_OBJECT_OBJECT_RANGE_INVALID,
    MATERIAL_OBJECT_NOT_MATERIAL,
    MATERIAL_OBJECT_TYPE_INDEX_INVALID,
    MATERIAL_OBJECT_TYPE_RECORD_MISMATCH,
    MATERIAL_OBJECT_SCHEMA_UNRESOLVED,
    MATERIAL_OBJECT_SCHEMA_INVALID,
    MATERIAL_OBJECT_TYPE_IDENTITY_UNSUPPORTED,
    MATERIAL_OBJECT_SCHEMA_SIZE_OVERFLOW,
    MATERIAL_OBJECT_ALLOCATION_FAILED,
    MATERIAL_OBJECT_TYPETREE_PARSE_FAILED,
    MATERIAL_OBJECT_TYPETREE_NODES_NOT_EXHAUSTED,
    MATERIAL_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED,
    MATERIAL_OBJECT_STRING_INVALID,
    MATERIAL_OBJECT_COUNT_INVALID,
    MATERIAL_OBJECT_DUPLICATE_KEY,
    MATERIAL_OBJECT_PAYLOAD_TRUNCATED,
    MATERIAL_OBJECT_MODEL_INVALID,
    MATERIAL_OBJECT_NOT_DECODED,
} MaterialObjectStatus;

typedef struct {
    TypeTreeType schema;
    TypeTreeValue root;

    MaterialStringView name;
    AssetPPtr shader;
    MaterialStringView* valid_keywords;
    size_t valid_keyword_count;
    MaterialStringView* invalid_keywords;
    size_t invalid_keyword_count;
    uint32_t lightmap_flags;
    bool enable_instancing_variants;
    bool double_sided_gi;
    int32_t custom_render_queue;

    MaterialStringTag* string_tags;
    size_t string_tag_count;
    MaterialStringView* disabled_shader_passes;
    size_t disabled_shader_pass_count;

    MaterialTextureProperty* texture_properties;
    size_t texture_property_count;
    MaterialIntProperty* int_properties;
    size_t int_property_count;
    MaterialFloatProperty* float_properties;
    size_t float_property_count;
    MaterialColorProperty* color_properties;
    size_t color_property_count;
    MaterialTextureStackReference* texture_stacks;
    size_t texture_stack_count;

    int64_t path_id;
    uint64_t byte_offset;
    uint32_t byte_size;
    int32_t source_type_index;
    uint32_t serialized_file_version;
    uint32_t target_platform;
    uint8_t serialized_type_hash[16];
    bool decoded;
} MaterialObject;

void material_object_init(MaterialObject* object);
void material_object_dispose(MaterialObject* object);

/* Strong output guarantee: failure leaves an initialized destination
 * unchanged; success replaces it. */
MaterialObjectStatus material_object_decode(
    MaterialObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object);

/* Converts bits to a host float without numeric conversion. */
float material_float32_value(MaterialFloat32 value);

const char* material_object_status_name(MaterialObjectStatus status);

#endif /* MATERIAL_OBJECT_H */
