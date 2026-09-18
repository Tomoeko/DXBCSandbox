#include "io/compute_shader_object.h"
#include "io/compute_shader_artifact.h"

#include "dxbc/dxbc_hash.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint8_t bytes[1024];
    TypeTreeType type;
    AssetObjectInfo object;
    SerializedFile file;
} Fixture;

static const uint8_t k_type_hash[16] = {
    0xabU, 0xd9U, 0x13U, 0x5bU, 0x8cU, 0xe8U, 0x3dU, 0x04U,
    0x3fU, 0xefU, 0x4eU, 0x9eU, 0xc7U, 0xf5U, 0x33U, 0x66U,
};

static void store_le32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static size_t build_compute_dxbc(uint8_t bytes[56]) {
    memset(bytes, 0, 56U);
    memcpy(bytes, "DXBC", 4U);
    store_le32(bytes + 20U, 1U);
    store_le32(bytes + 24U, 56U);
    store_le32(bytes + 28U, 1U);
    store_le32(bytes + 32U, 36U);
    memcpy(bytes + 36U, "SHDR", 4U);
    store_le32(bytes + 40U, 12U);
    store_le32(bytes + 44U, UINT32_C(0x00050050)); /* cs_5_0 */
    store_le32(bytes + 48U, 3U);
    store_le32(bytes + 52U, UINT32_C(0x0100003e)); /* ret */
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, 56U, hash)) return 0U;
    memcpy(bytes + 4U, hash, sizeof(hash));
    return 56U;
}

static void fixture_init(Fixture* fixture, const char* unity_version);

typedef struct {
    Fixture base;
    size_t size;
    size_t platform_count_offset;
    size_t kernel_name_size_offset;
} CapturedFixture;

static bool captured_append(CapturedFixture* fixture, const void* data,
                            size_t size) {
    if (fixture->size > sizeof(fixture->base.bytes) ||
        size > sizeof(fixture->base.bytes) - fixture->size) return false;
    memcpy(fixture->base.bytes + fixture->size, data, size);
    fixture->size += size;
    return true;
}

static bool captured_u8(CapturedFixture* fixture, uint8_t value) {
    return captured_append(fixture, &value, sizeof(value));
}

static bool captured_u32(CapturedFixture* fixture, uint32_t value) {
    uint8_t bytes[4];
    store_le32(bytes, value);
    return captured_append(fixture, bytes, sizeof(bytes));
}

static bool captured_i64(CapturedFixture* fixture, int64_t value) {
    uint8_t bytes[8];
    uint64_t raw = (uint64_t)value;
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(raw >> (index * 8U));
    }
    return captured_append(fixture, bytes, sizeof(bytes));
}

static bool captured_align4(CapturedFixture* fixture) {
    while ((fixture->size & 3U) != 0U) {
        if (!captured_u8(fixture, 0U)) return false;
    }
    return true;
}

static bool captured_string(CapturedFixture* fixture, const char* value) {
    size_t size = strlen(value);
    return size <= UINT32_MAX && captured_u32(fixture, (uint32_t)size) &&
        captured_append(fixture, value, size) && captured_align4(fixture);
}

static bool captured_resource(CapturedFixture* fixture, const char* name,
                              int32_t bind, int32_t sampler,
                              int32_t dimension) {
    return captured_string(fixture, name) && captured_string(fixture, "") &&
        captured_u32(fixture, (uint32_t)bind) &&
        captured_u32(fixture, (uint32_t)sampler) &&
        captured_u32(fixture, (uint32_t)dimension);
}

static bool captured_resource_array(CapturedFixture* fixture,
                                    const char* name, int32_t bind,
                                    int32_t sampler, int32_t dimension) {
    return captured_u32(fixture, 1U) &&
        captured_resource(fixture, name, bind, sampler, dimension) &&
        captured_align4(fixture);
}

static bool build_captured_fixture(CapturedFixture* fixture) {
    uint8_t compute_dxbc[56];
    size_t compute_dxbc_size = build_compute_dxbc(compute_dxbc);
    if (compute_dxbc_size == 0U) return false;
    memset(fixture, 0, sizeof(*fixture));
    fixture_init(&fixture->base, "2021.3.35f1");
    memset(fixture->base.bytes, 0, sizeof(fixture->base.bytes));
    fixture->size = 0U;
    if (!captured_string(fixture, "CapturedCompute")) return false;
    fixture->platform_count_offset = fixture->size;
    if (!captured_u32(fixture, 1U) ||       /* variants */
        !captured_u32(fixture, 4U) ||       /* targetRenderer: D3D11 */
        !captured_u32(fixture, 50U) ||      /* targetLevel */
        !captured_u32(fixture, 1U)) {       /* kernels */
        return false;
    }
    fixture->kernel_name_size_offset = fixture->size;
    if (!captured_string(fixture, "CSMain") ||
        !captured_u32(fixture, 1U) ||       /* variantMap */
        !captured_string(fixture, "") ||    /* keyword key */
        !captured_u32(fixture, 1U) ||       /* cbVariantIndices */
        !captured_u32(fixture, 0U) ||
        !captured_align4(fixture) ||
        !captured_resource_array(fixture, "Params", 0, -1, -1) ||
        !captured_resource_array(fixture, "Source", 0, 1, 2) ||
        !captured_u32(fixture, 1U) ||       /* builtinSamplers */
        !captured_u32(fixture, 0x1234U) ||
        !captured_u32(fixture, 1U) ||
        !captured_align4(fixture) ||
        !captured_resource_array(fixture, "Input", 1, -1, -1) ||
        !captured_resource_array(fixture, "Output", 0, -1, -1) ||
        !captured_u32(fixture, (uint32_t)compute_dxbc_size) ||
        !captured_append(fixture, compute_dxbc, compute_dxbc_size) ||
        !captured_align4(fixture) ||
        !captured_u32(fixture, 3U) ||       /* threadGroupSize */
        !captured_u32(fixture, 8U) ||
        !captured_u32(fixture, 4U) ||
        !captured_u32(fixture, 1U) ||
        !captured_i64(fixture, 0x4001) ||
        !captured_u32(fixture, 1U) ||       /* globalKeywords */
        !captured_string(fixture, "GLOBAL") ||
        !captured_align4(fixture) ||
        !captured_u32(fixture, 1U) ||       /* localKeywords */
        !captured_string(fixture, "LOCAL") ||
        !captured_align4(fixture) ||
        !captured_align4(fixture) ||         /* kernels vector */
        !captured_u32(fixture, 1U) ||       /* constantBuffers */
        !captured_string(fixture, "Params") ||
        !captured_u32(fixture, 16U) ||
        !captured_u32(fixture, 1U) ||       /* params */
        !captured_string(fixture, "Scale") ||
        !captured_u32(fixture, 0U) ||       /* type */
        !captured_u32(fixture, 0U) ||       /* offset */
        !captured_u32(fixture, 0U) ||       /* arraySize */
        !captured_u32(fixture, 1U) ||       /* rows */
        !captured_u32(fixture, 4U) ||       /* columns */
        !captured_align4(fixture) ||
        !captured_align4(fixture) ||
        !captured_u8(fixture, 1U) ||        /* resourcesResolved */
        !captured_align4(fixture) ||
        !captured_align4(fixture)) {         /* variants vector */
        return false;
    }
    fixture->base.object.byte_size = (uint32_t)fixture->size;
    fixture->base.file.file_size = sizeof(fixture->base.bytes);
    fixture->base.file.raw_size = sizeof(fixture->base.bytes);
    return true;
}

static void captured_rebind(CapturedFixture* fixture) {
    fixture->base.file.types = &fixture->base.type;
    fixture->base.file.objects = &fixture->base.object;
    fixture->base.file.raw_data = fixture->base.bytes;
}

static void fixture_init(Fixture* fixture, const char* unity_version) {
    static const char name[] = "ExactCompute";
    memset(fixture, 0, sizeof(*fixture));
    store_le32(fixture->bytes, (uint32_t)(sizeof(name) - 1U));
    memcpy(fixture->bytes + 4U, name, sizeof(name) - 1U);
    store_le32(fixture->bytes + 16U, 3U);

    fixture->type.type_id = 72;
    fixture->type.script_type_index = UINT16_MAX;
    memcpy(fixture->type.type_hash, k_type_hash, sizeof(k_type_hash));
    fixture->object.path_id = 101;
    fixture->object.byte_offset = 0U;
    fixture->object.byte_size = 20U;
    fixture->object.type_id_or_index = 0;
    fixture->object.type_id = 72;
    fixture->object.script_type_index = UINT16_MAX;

    fixture->file.file_size = sizeof(fixture->bytes);
    fixture->file.version = 22U;
    fixture->file.data_offset = 0U;
    fixture->file.unity_version = (char*)unity_version;
    fixture->file.type_count = 1;
    fixture->file.types = &fixture->type;
    fixture->file.object_count = 1;
    fixture->file.objects = &fixture->object;
    fixture->file.raw_data = fixture->bytes;
    fixture->file.raw_size = sizeof(fixture->bytes);
}

static bool view_is_clear(const ComputeShaderNameView* view) {
    return !view->name_bytes && view->name_size == 0U &&
        view->declared_platform_variant_count == 0U;
}

int main(void) {
    Fixture fixture;
    ComputeShaderNameView view = {(const uint8_t*)1, 2U, 3U};
    CHECK(compute_shader_object_name_view(NULL, NULL, &view) ==
          COMPUTE_SHADER_INVENTORY_INVALID_ARGUMENT);
    CHECK(view_is_clear(&view));

    fixture_init(&fixture, "2021.3.35f1");
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_OK);
    CHECK(view.name_size == strlen("ExactCompute"));
    CHECK(memcmp(view.name_bytes, "ExactCompute", view.name_size) == 0);
    CHECK(view.declared_platform_variant_count == 3U);

    fixture_init(&fixture, "2021.3.29f1");
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_OK);

    fixture_init(&fixture, "2021.3.35f2");
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_UNSUPPORTED_UNITY_VERSION);
    CHECK(view_is_clear(&view));

    fixture_init(&fixture, "2021.3.35f1");
    fixture.file.version = 21U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_UNSUPPORTED_FILE_VERSION);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.file.file_size -= 1U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_INVALID_SOURCE_FILE);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.object.byte_offset = sizeof(fixture.bytes) - 1U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_OBJECT_RANGE_INVALID);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.object.type_id = 48;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_NOT_COMPUTE_SHADER);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.object.type_id_or_index = 1;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_TYPE_INDEX_INVALID);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.type.type_id = 48;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_TYPE_RECORD_MISMATCH);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.type.script_id_hash[0] = 1U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_TYPE_IDENTITY_UNSUPPORTED);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.type.type_hash[0] ^= 1U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_TYPE_IDENTITY_UNSUPPORTED);
    CHECK(view_is_clear(&view));

    fixture_init(&fixture, "2021.3.35f1");
    AssetObjectInfo foreign = fixture.object;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &foreign, &view) ==
          COMPUTE_SHADER_INVENTORY_OBJECT_NOT_OWNED);
    CHECK(view_is_clear(&view));

    fixture_init(&fixture, "2021.3.35f1");
    fixture.object.byte_size = 3U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_NAME_TRUNCATED);

    fixture_init(&fixture, "2021.3.35f1");
    store_le32(fixture.bytes, UINT32_MAX);
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_NAME_LENGTH_INVALID);

    fixture_init(&fixture, "2021.3.35f1");
    store_le32(fixture.bytes, 32U);
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_NAME_TRUNCATED);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.bytes[6] = 0U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_NAME_CONTAINS_NUL);

    fixture_init(&fixture, "2021.3.35f1");
    fixture.object.byte_size = 16U;
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_TRUNCATED);

    fixture_init(&fixture, "2021.3.35f1");
    store_le32(fixture.bytes + 16U, UINT32_MAX);
    CHECK(compute_shader_object_name_view(
              &fixture.file, &fixture.object, &view) ==
          COMPUTE_SHADER_INVENTORY_VARIANT_COUNT_INVALID);

    /* Captured 2021.3 ClassID 72 field order: platform, kernel parent,
     * variant map, five resource planes, code, thread group, keyword planes,
     * constant-buffer metadata, and resourcesResolved. */
    CapturedFixture captured;
    CHECK(build_captured_fixture(&captured));
    ComputeShaderObject decoded;
    compute_shader_object_init(&decoded);
    CHECK(compute_shader_object_decode_borrowed(
              &decoded, &captured.base.file, &captured.base.object) ==
          COMPUTE_SHADER_OBJECT_OK);
    CHECK(decoded.decoded);
    CHECK(compute_shader_object_has_layout_authority(&decoded));
    CHECK(decoded.serialized_file_version == 22U);
    CHECK(decoded.unity_version.size == strlen("2021.3.35f1"));
    CHECK(decoded.name.size == strlen("CapturedCompute"));
    CHECK(memcmp(decoded.name.bytes, "CapturedCompute",
                 decoded.name.size) == 0);
    CHECK(decoded.platform_count == 1U);
    const ComputeShaderPlatformVariant* platform = &decoded.platforms[0];
    CHECK(platform->target_renderer == 4);
    CHECK(platform->target_level == 50);
    CHECK(platform->resources_resolved);
    CHECK(platform->kernel_count == 1U);
    CHECK(platform->constant_buffer_count == 1U);
    CHECK(platform->constant_buffers[0].parameter_count == 1U);
    CHECK(platform->constant_buffers[0].parameters[0].column_count == 4U);
    const ComputeShaderKernelParent* kernel = &platform->kernels[0];
    CHECK(kernel->variant_count == 1U);
    CHECK(kernel->global_keyword_count == 1U);
    CHECK(kernel->local_keyword_count == 1U);
    const ComputeShaderKernelVariant* variant = &kernel->variants[0];
    CHECK(variant->constant_buffer_variant_index_count == 1U);
    CHECK(variant->constant_buffer_count == 1U);
    CHECK(variant->texture_count == 1U);
    CHECK(variant->builtin_sampler_count == 1U);
    CHECK(variant->input_buffer_count == 1U);
    CHECK(variant->output_buffer_count == 1U);
    CHECK(variant->thread_group_size_count == 3U);
    CHECK(variant->thread_group_size[0] == 8U);
    CHECK(variant->thread_group_size[1] == 4U);
    CHECK(variant->thread_group_size[2] == 1U);
    CHECK(variant->requirements == 0x4001);
    ComputeShaderObjectSummary summary;
    CHECK(compute_shader_object_summarize(&decoded, &summary));
    CHECK(summary.platform_count == 1U);
    CHECK(summary.kernel_parent_count == 1U);
    CHECK(summary.kernel_variant_count == 1U);
    CHECK(summary.code_blob_count == 1U);
    CHECK(summary.dxbc_code_blob_count == 1U);
    CHECK(summary.empty_code_blob_count == 0U);
    CHECK(summary.non_dxbc_code_blob_count == 0U);
    CHECK(summary.exact_thread_group_count == 1U);
    CHECK(summary.resource_count == 4U);
    CHECK(summary.constant_buffer_count == 1U);
    CHECK(summary.parameter_count == 1U);
    CHECK(compute_shader_object_source_authority(&decoded) ==
          COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE);

    ComputeShaderArtifactPackage package;
    compute_shader_artifact_package_init(&package);
    CHECK(compute_shader_artifact_build(&decoded, &package) ==
          COMPUTE_SHADER_ARTIFACT_OK);
    CHECK(package.binary_count == 2U);
    CHECK(package.binaries[0].kind ==
          COMPUTE_SHADER_BINARY_SERIALIZED_OBJECT);
    CHECK(package.binaries[0].size == captured.base.object.byte_size);
    CHECK(memcmp(package.binaries[0].data, captured.base.bytes,
                 captured.base.object.byte_size) == 0);
    CHECK(strstr(package.binaries[0].filename,
                 ".serialized-object.bin") != NULL);
    CHECK(package.binaries[1].kind == COMPUTE_SHADER_BINARY_DXBC);
    CHECK(package.binaries[1].size == 56U);
    CHECK(strstr(package.binaries[1].filename, ".dxbc") != NULL);
    CHECK(strstr(package.manifest.buf,
                 "\"binary_exact\":true") != NULL);
    CHECK(strstr(package.manifest.buf,
                 "\"typetree_nodes\":146") != NULL);
    CHECK(strstr(package.manifest.buf,
                 "\"unity_version\":\"2021.3.35f1\"") != NULL);
    CHECK(strstr(package.manifest.buf,
                 COMPUTE_SHADER_OBJECT_LAYOUT_AUTHORITY) != NULL);
    CHECK(strstr(package.manifest.buf,
                 "unity-serialized-compute-object") != NULL);
    CHECK(strstr(package.manifest.buf,
                 "\"source_ready\":false") != NULL);
    CHECK(strstr(package.manifest.buf,
                 "declaration-inverse-unavailable") != NULL);
    compute_shader_artifact_package_dispose(&package);

    decoded.serialized_type_hash[0] ^= 1U;
    CHECK(!compute_shader_object_has_layout_authority(&decoded));
    CHECK(compute_shader_artifact_build(&decoded, &package) ==
          COMPUTE_SHADER_ARTIFACT_NOT_DECODED);
    decoded.serialized_type_hash[0] ^= 1U;
    compute_shader_artifact_package_dispose(&package);

    /* Decode is transactional. A truncated replacement cannot destroy the
     * valid decoded object already held by the caller. */
    CapturedFixture invalid = captured;
    captured_rebind(&invalid);
    --invalid.base.object.byte_size;
    CHECK(compute_shader_object_decode_borrowed(
              &decoded, &invalid.base.file, &invalid.base.object) ==
          COMPUTE_SHADER_OBJECT_PAYLOAD_TRUNCATED);
    CHECK(decoded.decoded && decoded.platform_count == 1U);

    invalid = captured;
    captured_rebind(&invalid);
    ++invalid.base.object.byte_size;
    CHECK(compute_shader_object_decode_borrowed(
              &decoded, &invalid.base.file, &invalid.base.object) ==
          COMPUTE_SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED);

    invalid = captured;
    captured_rebind(&invalid);
    store_le32(invalid.base.bytes + invalid.platform_count_offset,
               UINT32_MAX);
    CHECK(compute_shader_object_decode_borrowed(
              &decoded, &invalid.base.file, &invalid.base.object) ==
          COMPUTE_SHADER_OBJECT_COUNT_INVALID);

    invalid = captured;
    captured_rebind(&invalid);
    store_le32(invalid.base.bytes + invalid.kernel_name_size_offset,
               UINT32_MAX);
    CHECK(compute_shader_object_decode_borrowed(
              &decoded, &invalid.base.file, &invalid.base.object) ==
          COMPUTE_SHADER_OBJECT_STRING_LENGTH_INVALID);

    invalid = captured;
    captured_rebind(&invalid);
    invalid.base.bytes[invalid.kernel_name_size_offset + 4U] = 0U;
    CHECK(compute_shader_object_decode_borrowed(
              &decoded, &invalid.base.file, &invalid.base.object) ==
          COMPUTE_SHADER_OBJECT_STRING_CONTAINS_NUL);
    compute_shader_object_dispose(&decoded);

    CHECK(strcmp(compute_shader_inventory_status_name(
                     COMPUTE_SHADER_INVENTORY_OK), "ok") == 0);
    CHECK(strcmp(compute_shader_inventory_status_name(
                     COMPUTE_SHADER_INVENTORY_TYPE_IDENTITY_UNSUPPORTED),
                 "type-identity-unsupported") == 0);
    CHECK(strcmp(compute_shader_object_status_name(
                     COMPUTE_SHADER_OBJECT_COUNT_INVALID),
                 "count-invalid") == 0);
    CHECK(strcmp(compute_shader_artifact_status_name(
                     COMPUTE_SHADER_ARTIFACT_NOT_APPLICABLE),
                 "not-applicable") == 0);
    CHECK(strcmp(compute_shader_source_authority_status_name(
                     COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE),
                 "declaration-inverse-unavailable") == 0);
    return 0;
}
