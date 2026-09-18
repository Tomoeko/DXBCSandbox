// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMPUTE_SHADER_OBJECT_H
#define COMPUTE_SHADER_OBJECT_H

#include "io/serialized_file.h"

#define COMPUTE_SHADER_OBJECT_LAYOUT_AUTHORITY \
    "unity-2021.3-player-class72-abd9135b8ce83d043fef4e9ec7f53366"
#define COMPUTE_SHADER_OBJECT_LAYOUT_NODE_COUNT 146U

/*
 * Exact Unity 2021.3 player ComputeShader (ClassID 72) decoding boundary.
 *
 * The layout is pinned by the complete 146-node ClassID 72 TypeTree emitted
 * by Unity 2021.3.35f1 and by the exact serialized type identity carried by
 * the 2021.3.29f1/2021.3.35f1 player files admitted below. It is not a
 * version-family guess. Strings, the complete object span, and compiled code
 * spans in a decoded object borrow the SerializedFile backing bytes; dispose
 * the object before closing the file or releasing its raw_data.
 */
typedef enum {
    COMPUTE_SHADER_INVENTORY_NOT_APPLICABLE = 0,
    COMPUTE_SHADER_INVENTORY_OK,
    COMPUTE_SHADER_INVENTORY_INVALID_ARGUMENT,
    COMPUTE_SHADER_INVENTORY_INVALID_SOURCE_FILE,
    COMPUTE_SHADER_INVENTORY_UNSUPPORTED_FILE_VERSION,
    COMPUTE_SHADER_INVENTORY_UNSUPPORTED_UNITY_VERSION,
    COMPUTE_SHADER_INVENTORY_OBJECT_NOT_OWNED,
    COMPUTE_SHADER_INVENTORY_OBJECT_RANGE_INVALID,
    COMPUTE_SHADER_INVENTORY_NOT_COMPUTE_SHADER,
    COMPUTE_SHADER_INVENTORY_TYPE_INDEX_INVALID,
    COMPUTE_SHADER_INVENTORY_TYPE_RECORD_MISMATCH,
    COMPUTE_SHADER_INVENTORY_TYPE_IDENTITY_UNSUPPORTED,
    COMPUTE_SHADER_INVENTORY_NAME_LENGTH_INVALID,
    COMPUTE_SHADER_INVENTORY_NAME_TRUNCATED,
    COMPUTE_SHADER_INVENTORY_NAME_CONTAINS_NUL,
    COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_TRUNCATED,
    COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_INVALID,
} ComputeShaderInventoryStatus;

typedef struct {
    const uint8_t* name_bytes;
    size_t name_size;
    uint32_t declared_platform_variant_count;
} ComputeShaderNameView;

typedef struct {
    const uint8_t* bytes;
    size_t size;
} ComputeShaderStringView;

typedef struct {
    ComputeShaderStringView name;
    ComputeShaderStringView generated_name;
    int32_t bind_point;
    int32_t sampler_bind_point;
    int32_t texture_dimension;
} ComputeShaderResource;

typedef struct {
    uint32_t sampler;
    int32_t bind_point;
} ComputeShaderBuiltinSampler;

typedef struct {
    ComputeShaderStringView name;
    int32_t type;
    uint32_t offset;
    uint32_t array_size;
    uint32_t row_count;
    uint32_t column_count;
} ComputeShaderParameter;

typedef struct {
    ComputeShaderStringView name;
    int32_t byte_size;
    ComputeShaderParameter* parameters;
    size_t parameter_count;
} ComputeShaderConstantBuffer;

typedef struct {
    ComputeShaderStringView keyword_key;
    uint32_t* constant_buffer_variant_indices;
    size_t constant_buffer_variant_index_count;
    ComputeShaderResource* constant_buffers;
    size_t constant_buffer_count;
    ComputeShaderResource* textures;
    size_t texture_count;
    ComputeShaderBuiltinSampler* builtin_samplers;
    size_t builtin_sampler_count;
    ComputeShaderResource* input_buffers;
    size_t input_buffer_count;
    ComputeShaderResource* output_buffers;
    size_t output_buffer_count;
    const uint8_t* code;
    size_t code_size;
    uint32_t* thread_group_size;
    size_t thread_group_size_count;
    int64_t requirements;
} ComputeShaderKernelVariant;

typedef struct {
    ComputeShaderStringView name;
    ComputeShaderKernelVariant* variants;
    size_t variant_count;
    ComputeShaderStringView* global_keywords;
    size_t global_keyword_count;
    ComputeShaderStringView* local_keywords;
    size_t local_keyword_count;
} ComputeShaderKernelParent;

typedef struct {
    int32_t target_renderer;
    int32_t target_level;
    ComputeShaderKernelParent* kernels;
    size_t kernel_count;
    ComputeShaderConstantBuffer* constant_buffers;
    size_t constant_buffer_count;
    bool resources_resolved;
} ComputeShaderPlatformVariant;

typedef struct {
    ComputeShaderStringView name;
    ComputeShaderStringView unity_version;
    const uint8_t* serialized_object_bytes;
    size_t serialized_object_size;
    ComputeShaderPlatformVariant* platforms;
    size_t platform_count;
    int64_t path_id;
    uint32_t target_platform;
    uint32_t serialized_file_version;
    uint8_t serialized_type_hash[16];
    bool decoded;
} ComputeShaderObject;

typedef enum {
    COMPUTE_SHADER_OBJECT_NOT_APPLICABLE = 0,
    COMPUTE_SHADER_OBJECT_OK,
    COMPUTE_SHADER_OBJECT_INVALID_ARGUMENT,
    COMPUTE_SHADER_OBJECT_INVALID_SOURCE_FILE,
    COMPUTE_SHADER_OBJECT_UNSUPPORTED_FILE_VERSION,
    COMPUTE_SHADER_OBJECT_UNSUPPORTED_UNITY_VERSION,
    COMPUTE_SHADER_OBJECT_OBJECT_NOT_OWNED,
    COMPUTE_SHADER_OBJECT_OBJECT_RANGE_INVALID,
    COMPUTE_SHADER_OBJECT_NOT_COMPUTE_SHADER,
    COMPUTE_SHADER_OBJECT_TYPE_INDEX_INVALID,
    COMPUTE_SHADER_OBJECT_TYPE_RECORD_MISMATCH,
    COMPUTE_SHADER_OBJECT_TYPE_IDENTITY_UNSUPPORTED,
    COMPUTE_SHADER_OBJECT_STRING_LENGTH_INVALID,
    COMPUTE_SHADER_OBJECT_STRING_TRUNCATED,
    COMPUTE_SHADER_OBJECT_STRING_CONTAINS_NUL,
    COMPUTE_SHADER_OBJECT_COUNT_INVALID,
    COMPUTE_SHADER_OBJECT_ALLOCATION_FAILED,
    COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED,
    COMPUTE_SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED,
    COMPUTE_SHADER_OBJECT_MODEL_INVALID,
    COMPUTE_SHADER_OBJECT_NOT_DECODED,
} ComputeShaderObjectStatus;

typedef struct {
    size_t platform_count;
    size_t kernel_parent_count;
    size_t kernel_variant_count;
    size_t code_blob_count;
    size_t dxbc_code_blob_count;
    size_t empty_code_blob_count;
    size_t non_dxbc_code_blob_count;
    size_t exact_thread_group_count;
    size_t invalid_thread_group_count;
    size_t resource_count;
    size_t constant_buffer_count;
    size_t parameter_count;
} ComputeShaderObjectSummary;

typedef enum {
    COMPUTE_SHADER_SOURCE_AUTHORITY_NOT_APPLICABLE = 0,
    COMPUTE_SHADER_SOURCE_AUTHORITY_EXACT,
    COMPUTE_SHADER_SOURCE_AUTHORITY_NOT_DECODED,
    COMPUTE_SHADER_SOURCE_AUTHORITY_NO_KERNELS,
    COMPUTE_SHADER_SOURCE_AUTHORITY_CODE_UNAVAILABLE,
    COMPUTE_SHADER_SOURCE_AUTHORITY_NON_DXBC_PROGRAMS_UNINVERTED,
    COMPUTE_SHADER_SOURCE_AUTHORITY_THREAD_GROUP_INVALID,
    /* Names and bind points survive serialization, but exact raw/structured
     * buffer declarations still require a certified DXBC declaration
     * inverse. Never guess a .compute declaration at this boundary. */
    COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE,
} ComputeShaderSourceAuthorityStatus;

void compute_shader_object_init(ComputeShaderObject* object);
void compute_shader_object_dispose(ComputeShaderObject* object);
bool compute_shader_object_has_layout_authority(
    const ComputeShaderObject* object);

/* Strong output guarantee: failure leaves destination initialized/unchanged;
 * success replaces it. */
ComputeShaderObjectStatus compute_shader_object_decode_borrowed(
    ComputeShaderObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object);

bool compute_shader_object_summarize(
    const ComputeShaderObject* object, ComputeShaderObjectSummary* summary);

ComputeShaderSourceAuthorityStatus compute_shader_object_source_authority(
    const ComputeShaderObject* object);

/* Prefix-only inventory remains available to catalog corrupt or unsupported
 * objects without allocating the complete nested representation. */
ComputeShaderInventoryStatus compute_shader_object_name_view(
    const SerializedFile* file, const AssetObjectInfo* object,
    ComputeShaderNameView* out_view);

const char* compute_shader_inventory_status_name(
    ComputeShaderInventoryStatus status);
const char* compute_shader_object_status_name(ComputeShaderObjectStatus status);
const char* compute_shader_source_authority_status_name(
    ComputeShaderSourceAuthorityStatus status);

#endif /* COMPUTE_SHADER_OBJECT_H */
