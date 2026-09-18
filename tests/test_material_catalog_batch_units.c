#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "app/material_batch.h"
#include "app/shader_catalog.h"
#include "common/file_io.h"
#include "common/sha256.h"
#include "common/string_builder.h"
#include "common/unity_asset_guid.h"
#include "io/serialized_file.h"
#include "io/shader_object.h"
#include "io/typetree_schema_registry.h"
#include "io/unity_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define TEST_PROCESS_ID() ((unsigned long)_getpid())
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_PATH_SEPARATOR '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_PROCESS_ID() ((unsigned long)getpid())
#define TEST_MKDIR(path) mkdir((path), 0755)
#define TEST_RMDIR(path) rmdir(path)
#define TEST_PATH_SEPARATOR '/'
#endif

#ifndef DXBC_TEST_PLAYER_SCHEMA_REGISTRY
#error DXBC_TEST_PLAYER_SCHEMA_REGISTRY must name the pinned registry
#endif

#ifndef DXBC_TEST_SHADER_BUNDLE
#error DXBC_TEST_SHADER_BUNDLE must name the pinned regression corpus
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        status = 1; \
        goto cleanup; \
    } \
} while (0)

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} ByteBuffer;

typedef struct {
    const TypeTreeSchemaRegistry* registry;
    uint8_t* payload;
    size_t payload_size;
    uint8_t type_hash[16];
    bool found;
} ShaderPayloadSearch;

typedef struct {
    MaterialBatchTextureReferenceStatus status;
    int32_t expected_class_id;
    bool asset_exported;
    bool lease_stable;
    size_t calls;
    size_t lease_closes;
} TextureReferenceFixture;

static void buffer_dispose(ByteBuffer* buffer) {
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static bool buffer_reserve(ByteBuffer* buffer, size_t additional) {
    if (!buffer || additional > SIZE_MAX - buffer->size) return false;
    size_t required = buffer->size + additional;
    if (required <= buffer->capacity) return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 256U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    uint8_t* data = (uint8_t*)realloc(buffer->data, capacity);
    if (!data) return false;
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static bool buffer_append(ByteBuffer* buffer, const void* bytes,
                          size_t size) {
    if ((!bytes && size != 0U) || !buffer_reserve(buffer, size)) return false;
    if (size != 0U) memcpy(buffer->data + buffer->size, bytes, size);
    buffer->size += size;
    return true;
}

static bool buffer_u8(ByteBuffer* buffer, uint8_t value) {
    return buffer_append(buffer, &value, sizeof(value));
}

static bool buffer_le16(ByteBuffer* buffer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8U)};
    return buffer_append(buffer, bytes, sizeof(bytes));
}

static bool buffer_le32(ByteBuffer* buffer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U), (uint8_t)(value >> 24U),
    };
    return buffer_append(buffer, bytes, sizeof(bytes));
}

static bool buffer_le64(ByteBuffer* buffer, uint64_t value) {
    return buffer_le32(buffer, (uint32_t)value) &&
        buffer_le32(buffer, (uint32_t)(value >> 32U));
}

static bool buffer_align(ByteBuffer* buffer, size_t alignment) {
    if (!buffer || alignment == 0U) return false;
    while ((buffer->size % alignment) != 0U) {
        if (!buffer_u8(buffer, 0U)) return false;
    }
    return true;
}

static bool buffer_cstring(ByteBuffer* buffer, const char* value) {
    return value && buffer_append(buffer, value, strlen(value) + 1U);
}

static bool buffer_serialized_string(ByteBuffer* buffer, const char* value) {
    size_t size = value ? strlen(value) : 0U;
    return size <= UINT32_MAX && buffer_le32(buffer, (uint32_t)size) &&
        buffer_append(buffer, value, size) && buffer_align(buffer, 4U);
}

static bool buffer_pptr(ByteBuffer* buffer, int32_t file_id,
                        int64_t path_id) {
    return buffer_le32(buffer, (uint32_t)file_id) &&
        buffer_le64(buffer, (uint64_t)path_id);
}

static void patch_be32(ByteBuffer* buffer, size_t offset, uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        buffer->data[offset + index] =
            (uint8_t)(value >> (24U - index * 8U));
    }
}

static void patch_be64(ByteBuffer* buffer, size_t offset, uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        buffer->data[offset + index] =
            (uint8_t)(value >> (56U - index * 8U));
    }
}

static bool extract_shader_payload(const UnitySerializedSource* source,
                                   void* opaque) {
    ShaderPayloadSearch* search = (ShaderPayloadSearch*)opaque;
    if (search->found) return true;
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        return true;
    }
    if (strcmp(file.unity_version, "2021.3.35f1") != 0 ||
        serialized_file_resolve_class_schema(
            &file, 48, search->registry) != TYPETREE_SCHEMA_OK) {
        serialized_file_close(&file);
        return true;
    }
    for (int index = 0; index < file.object_count && !search->found;
         ++index) {
        const AssetObjectInfo* info = &file.objects[index];
        if (info->type_id != 48 || info->type_id_or_index < 0 ||
            info->type_id_or_index >= file.type_count) {
            continue;
        }
        ShaderObject object;
        shader_object_init(&object);
        if (shader_object_decode_borrowed(&object, &file, info) ==
                SHADER_OBJECT_OK && object.shader.name &&
            strcmp(object.shader.name, "Hidden/SeparableBlur") == 0) {
            size_t size = 0U;
            const uint8_t* bytes = serialized_file_get_object_data(
                &file, info, &size);
            uint8_t* copy = size ? (uint8_t*)malloc(size) : NULL;
            if (bytes && (size == 0U || copy)) {
                if (size != 0U) memcpy(copy, bytes, size);
                search->payload = copy;
                search->payload_size = size;
                memcpy(search->type_hash,
                       file.types[info->type_id_or_index].type_hash,
                       sizeof(search->type_hash));
                search->found = true;
            }
        }
        shader_object_dispose(&object);
    }
    serialized_file_close(&file);
    return true;
}

static bool build_material_payload(ByteBuffer* payload,
                                   const char* name,
                                   int32_t shader_file_id,
                                   int64_t shader_path_id,
                                   int64_t texture_path_id) {
    return buffer_serialized_string(payload, name) &&
        buffer_pptr(payload, shader_file_id, shader_path_id) &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "EXACT_KEYWORD") &&
        buffer_le32(payload, 0U) &&
        buffer_le32(payload, UINT32_C(0x80000005)) &&
        buffer_u8(payload, 1U) && buffer_u8(payload, 0U) &&
        buffer_align(payload, 4U) &&
        buffer_le32(payload, UINT32_MAX) &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "RenderType") &&
        buffer_serialized_string(payload, "Opaque") &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "ShadowCaster") &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "_MainTex") &&
        buffer_pptr(payload, 0, texture_path_id) &&
        buffer_le32(payload, UINT32_C(0x3f800000)) &&
        buffer_le32(payload, UINT32_C(0x3f800000)) &&
        buffer_le32(payload, 0U) &&
        buffer_le32(payload, UINT32_C(0x80000000)) &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "_Mode") &&
        buffer_le32(payload, UINT32_C(0xfffffff9)) &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "_Cutoff") &&
        buffer_le32(payload, UINT32_C(0x80000000)) &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "_Color") &&
        buffer_le32(payload, UINT32_C(0x3f800000)) &&
        buffer_le32(payload, UINT32_C(0xbf800000)) &&
        buffer_le32(payload, UINT32_C(0x00000001)) &&
        buffer_le32(payload, UINT32_C(0x80000000)) &&
        buffer_le32(payload, 1U) &&
        buffer_serialized_string(payload, "Layer") &&
        buffer_serialized_string(payload, "Item");
}

static bool build_compute_payload(ByteBuffer* payload) {
    return buffer_serialized_string(payload, "ComputeFixture") &&
        buffer_le32(payload, 0U);
}

static bool build_single_object_file(
    ByteBuffer* file, int32_t class_id, const uint8_t type_hash[16],
    int64_t path_id, const uint8_t* payload, size_t payload_size,
    const char* external_path) {
    static const uint8_t zeros[48] = {0};
    static const uint8_t zero_guid[16] = {0};
    if (!file || !type_hash || (!payload && payload_size != 0U) ||
        payload_size > UINT32_MAX) {
        return false;
    }
    if (!buffer_append(file, zeros, sizeof(zeros))) return false;
    const size_t metadata_start = file->size;
    if (!buffer_cstring(file, "2021.3.35f1") ||
        !buffer_le32(file, 19U) || !buffer_u8(file, 0U) ||
        !buffer_le32(file, 1U) ||
        !buffer_le32(file, (uint32_t)class_id) ||
        !buffer_u8(file, 0U) || !buffer_le16(file, UINT16_MAX) ||
        !buffer_append(file, type_hash, 16U) ||
        !buffer_le32(file, 1U) || !buffer_align(file, 4U) ||
        !buffer_le64(file, (uint64_t)path_id) ||
        !buffer_le64(file, 0U) ||
        !buffer_le32(file, (uint32_t)payload_size) ||
        !buffer_le32(file, 0U) ||
        !buffer_le32(file, 0U) ||
        !buffer_le32(file, external_path ? 1U : 0U)) {
        return false;
    }
    if (external_path &&
        (!buffer_cstring(file, "") ||
         !buffer_append(file, zero_guid, sizeof(zero_guid)) ||
         !buffer_le32(file, 0U) ||
         !buffer_cstring(file, external_path))) {
        return false;
    }
    if (!buffer_le32(file, 0U) || !buffer_cstring(file, "")) return false;
    const size_t metadata_size = file->size - metadata_start;
    if (metadata_size > UINT32_MAX || !buffer_align(file, 16U)) return false;
    const size_t data_offset = file->size;
    if (!buffer_append(file, payload, payload_size)) return false;
    patch_be32(file, 8U, 22U);
    patch_be32(file, 20U, (uint32_t)metadata_size);
    patch_be64(file, 24U, file->size);
    patch_be64(file, 32U, data_offset);
    return true;
}

static bool write_new(const char* path, const ByteBuffer* bytes) {
    (void)remove(path);
    return common_file_write_new_atomic(path, bytes->data, bytes->size) ==
        COMMON_FILE_OK;
}

static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    char* copy = (char*)malloc(size + 1U);
    if (copy) memcpy(copy, value, size + 1U);
    return copy;
}

static char* duplicate_parent(const char* path) {
    char* copy = duplicate_string(path);
    if (!copy) return NULL;
    char* separator = NULL;
    for (char* cursor = copy; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') separator = cursor;
    }
    if (!separator) {
        free(copy);
        return NULL;
    }
    *separator = '\0';
    return copy;
}

static bool bytes_contain(const uint8_t* bytes, size_t size,
                          const char* needle) {
    if ((!bytes && size != 0U) || !needle) return false;
    size_t needle_size = strlen(needle);
    if (needle_size == 0U) return true;
    if (needle_size > size) return false;
    for (size_t index = 0U; index <= size - needle_size; ++index) {
        if (memcmp(bytes + index, needle, needle_size) == 0) return true;
    }
    return false;
}

static bool setup_fake_shader_batch(const ShaderCatalog* catalog,
                                    size_t shader_index,
                                    ShaderBatchResult* batch) {
    shader_batch_result_dispose(batch);
    batch->catalog_authority = catalog;
    batch->record_count = catalog->record_count;
    batch->records = (ShaderBatchRecordResult*)calloc(
        batch->record_count, sizeof(*batch->records));
    if (batch->record_count != 0U && !batch->records) return false;
    for (size_t index = 0U; index < batch->record_count; ++index) {
        ShaderBatchRecordResult* provenance = &batch->records[index];
        memcpy(provenance->catalog_serialized_digest,
               catalog->records[index].serialized_digest,
               sizeof(provenance->catalog_serialized_digest));
        provenance->catalog_path_id = catalog->records[index].path_id;
        provenance->catalog_class_id = catalog->records[index].class_id;
        provenance->catalog_provenance_recorded = true;
    }
    ShaderBatchRecordResult* shader = &batch->records[shader_index];
    shader->status = SHADER_BATCH_EMITTED;
    shader->publication_authorized = true;
    shader->has_asset_meta = true;
    uint8_t identity[COMMON_SHA256_DIGEST_SIZE + 8U];
    memcpy(identity, catalog->records[shader_index].serialized_digest,
           COMMON_SHA256_DIGEST_SIZE);
    uint64_t path_bits =
        (uint64_t)catalog->records[shader_index].path_id;
    for (unsigned index = 0U; index < 8U; ++index) {
        identity[COMMON_SHA256_DIGEST_SIZE + index] =
            (uint8_t)(path_bits >> (index * 8U));
    }
    if (!unity_asset_guid_derive(
            UNITY_ASSET_GUID_DOMAIN_SHADER, identity, sizeof(identity),
            shader->asset_guid)) {
        return false;
    }
    shader->output_path = duplicate_string("associated.shader");
    shader->output_meta_path = duplicate_string("associated.shader.meta");
    static const uint8_t shader_bytes[] = "Shader \"Fixture\" {}\n";
    StringBuilder meta;
    sb_init(&meta);
    if (unity_shader_importer_meta_emit(shader->asset_guid, &meta) !=
            UNITY_MATERIAL_YAML_OK || !sb_ok(&meta)) {
        sb_free(&meta);
        return false;
    }
    CommonOutputPublishStatus shader_status = common_output_publish_exact(
        "associated.shader", shader_bytes, sizeof(shader_bytes) - 1U);
    CommonOutputPublishStatus meta_status = common_output_publish_exact(
        "associated.shader.meta", meta.buf, meta.len);
    shader->published_shader_size = sizeof(shader_bytes) - 1U;
    shader->published_meta_size = meta.len;
    common_sha256(shader_bytes, sizeof(shader_bytes) - 1U,
                  shader->published_shader_digest);
    common_sha256(meta.buf, meta.len,
                  shader->published_meta_digest);
    shader->published_shader_content_recorded = true;
    shader->published_meta_content_recorded = true;
    bool shader_ok = shader_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        shader_status == COMMON_OUTPUT_PUBLISH_UNCHANGED;
    bool meta_ok = meta_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        meta_status == COMMON_OUTPUT_PUBLISH_UNCHANGED;
    bool ok = shader->output_path && shader->output_meta_path &&
        shader_ok && meta_ok;
    sb_free(&meta);
    return ok;
}

static bool append_resolver_object(ShaderCatalogSource* source,
                                   int32_t class_id, int64_t path_id) {
    if (!source || source->object_reference_count == SIZE_MAX ||
        source->object_reference_count + 1U >
            SIZE_MAX / sizeof(*source->objects)) {
        return false;
    }
    size_t count = source->object_reference_count + 1U;
    AssetObjectInfo* objects = (AssetObjectInfo*)realloc(
        source->objects, count * sizeof(*objects));
    if (!objects) return false;
    source->objects = objects;
    AssetObjectInfo* object = &objects[source->object_reference_count];
    memset(object, 0, sizeof(*object));
    object->type_id = class_id;
    object->path_id = path_id;
    source->object_reference_count = count;
    return true;
}

static MaterialBatchTextureReferenceStatus resolve_texture_fixture(
    const MaterialBatchTextureReferenceRequest* request, void* opaque,
    MaterialBatchTextureReference* reference) {
    TextureReferenceFixture* fixture = (TextureReferenceFixture*)opaque;
    if (!request || !fixture || !reference || !request->target_object ||
        request->target_object->type_id != fixture->expected_class_id) {
        return MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED;
    }
    ++fixture->calls;
    if (fixture->status != MATERIAL_BATCH_TEXTURE_REFERENCE_OK) {
        return fixture->status;
    }
    reference->file_id = 8900000;
    memcpy(reference->guid,
           "22222222222222222222222222222222", 33U);
    reference->type = 3;
    reference->asset_exported = fixture->asset_exported;
    if (fixture->asset_exported) {
        reference->publication_lease = fixture;
    }
    return MATERIAL_BATCH_TEXTURE_REFERENCE_OK;
}

static bool close_texture_fixture_lease(void* opaque) {
    TextureReferenceFixture* fixture = (TextureReferenceFixture*)opaque;
    if (!fixture) return false;
    ++fixture->lease_closes;
    return fixture->lease_stable;
}

static void locate_records(const ShaderCatalog* catalog,
                           size_t* shader_index, size_t* compute_index) {
    *shader_index = SIZE_MAX;
    *compute_index = SIZE_MAX;
    for (size_t index = 0U; index < catalog->record_count; ++index) {
        if (catalog->records[index].class_id == 48) *shader_index = index;
        if (catalog->records[index].class_id == 72) *compute_index = index;
    }
}

static int test_content_canonical_dependency_evidence(
    const TypeTreeSchemaRegistry* registry,
    const ShaderPayloadSearch* search) {
    int status = 0;
    ShaderCatalog catalog;
    ShaderBatchResult shader_batch;
    MaterialBatchResult material_batch;
    ByteBuffer shader_file = {0};
    ByteBuffer material_payload = {0};
    ByteBuffer material_file = {0};
    bool* selected = NULL;
    char input_root[320] = {0};
    char output_root[320] = {0};
    char shader_path[512] = {0};
    char material_path[512] = {0};
    char duplicate_material_path[512] = {0};
    char* emitted_material_path = NULL;
    char* emitted_meta_path = NULL;
    char* emitted_evidence_path = NULL;
    char* emitted_directory = NULL;
    CommonFileBytes first_evidence = {0};
    CommonFileBytes repeated_evidence = {0};

    shader_catalog_init(&catalog);
    shader_batch_result_init(&shader_batch);
    material_batch_result_init(&material_batch);
    MaterialBatchResult malformed_completion;
    material_batch_result_init(&malformed_completion);
    ShaderCatalog completion_catalog;
    ShaderCatalogRecord completion_shader;
    ShaderCatalogMaterialRecord completion_material;
    shader_catalog_init(&completion_catalog);
    memset(&completion_shader, 0, sizeof(completion_shader));
    memset(&completion_material, 0, sizeof(completion_material));
    completion_material.status = SHADER_CATALOG_MATERIAL_READY;
    completion_material.object.decoded = true;
    completion_material.shader_record_index = 0U;
    completion_catalog.materials_included = true;
    completion_catalog.records = &completion_shader;
    completion_catalog.record_count = 1U;
    completion_catalog.materials = &completion_material;
    completion_catalog.material_count = 1U;
    bool completion_selected[] = {true};
    malformed_completion.catalog_authority = &completion_catalog;
    malformed_completion.selection_authority = completion_selected;
    malformed_completion.record_count = 1U;

    /* A counter-consistent all-unselected ledger is incomplete when its
     * originating selection chose the associated shader. */
    MaterialBatchRecordResult omitted_completion_record;
    memset(&omitted_completion_record, 0,
           sizeof(omitted_completion_record));
    omitted_completion_record.status = MATERIAL_BATCH_UNSELECTED;
    omitted_completion_record.shader_record_index = 0U;
    MaterialBatchResult omitted_completion;
    material_batch_result_init(&omitted_completion);
    omitted_completion.catalog_authority = &completion_catalog;
    omitted_completion.selection_authority = completion_selected;
    omitted_completion.records = &omitted_completion_record;
    omitted_completion.record_count = 1U;
    omitted_completion.stats.catalog_materials = 1U;
    omitted_completion.stats.unselected = 1U;
    CHECK(!material_batch_is_complete(&omitted_completion));
    bool completion_unselected[] = {false};
    omitted_completion.selection_authority = completion_unselected;
    CHECK(material_batch_is_complete(&omitted_completion));

    CHECK(!material_batch_is_complete(&malformed_completion));
    CHECK(!material_batch_texture_dependencies_are_closed(
        &malformed_completion));
    MaterialBatchRecordResult dummy_completion_record;
    memset(&dummy_completion_record, 0, sizeof(dummy_completion_record));
    malformed_completion.records = &dummy_completion_record;
    CHECK(!material_batch_is_complete(&malformed_completion));
    malformed_completion.stats.catalog_materials = 1U;
    malformed_completion.stats.selected = 1U;
    malformed_completion.stats.emitted = 1U;
    malformed_completion.stats.shader_dependency_groups = 1U;
    dummy_completion_record.status = MATERIAL_BATCH_FAILED;
    CHECK(!material_batch_is_complete(&malformed_completion));
    dummy_completion_record.status = MATERIAL_BATCH_EMITTED;
    dummy_completion_record.failure = MATERIAL_BATCH_FAILURE_NONE;
    dummy_completion_record.has_artifacts = true;
    dummy_completion_record.output_path = (char*)"fixture.mat";
    dummy_completion_record.output_meta_path = (char*)"fixture.mat.meta";
    dummy_completion_record.dependency_evidence_path =
        (char*)"fixture.mat.dependencies.json";
    memcpy(dummy_completion_record.asset_guid,
           "0123456789abcdef0123456789abcdef",
           UNITY_ASSET_GUID_TEXT_CAPACITY);
    memset(dummy_completion_record.artifact_identity_hex, 'a',
           COMMON_SHA256_DIGEST_SIZE * 2U);
    dummy_completion_record.artifact_identity_hex[
        COMMON_SHA256_DIGEST_SIZE * 2U] = '\0';
    dummy_completion_record.material_publish_attempted = true;
    dummy_completion_record.meta_publish_attempted = true;
    dummy_completion_record.evidence_publish_attempted = true;
    dummy_completion_record.material_publish_status =
        COMMON_OUTPUT_PUBLISH_EMITTED;
    dummy_completion_record.meta_publish_status =
        COMMON_OUTPUT_PUBLISH_EMITTED;
    dummy_completion_record.evidence_publish_status =
        COMMON_OUTPUT_PUBLISH_EMITTED;
    CHECK(material_batch_is_complete(&malformed_completion));
    malformed_completion.selection_authority = NULL;
    CHECK(!material_batch_is_complete(&malformed_completion));
    malformed_completion.selection_authority = completion_unselected;
    CHECK(!material_batch_is_complete(&malformed_completion));
    malformed_completion.selection_authority = completion_selected;
    malformed_completion.catalog_authority = NULL;
    CHECK(!material_batch_is_complete(&malformed_completion));
    malformed_completion.catalog_authority = &completion_catalog;
    dummy_completion_record.asset_guid[0] = 'A';
    CHECK(!material_batch_is_complete(&malformed_completion));
    dummy_completion_record.asset_guid[0] = '0';
    dummy_completion_record.artifact_identity_hex[0] = 'G';
    CHECK(!material_batch_is_complete(&malformed_completion));
    dummy_completion_record.artifact_identity_hex[0] = 'a';

    /* Aggregate status must be derived from the exact three-artifact
     * publication ledger, not independently trusted. */
    dummy_completion_record.material_publish_status =
        COMMON_OUTPUT_PUBLISH_UNCHANGED;
    dummy_completion_record.meta_publish_status =
        COMMON_OUTPUT_PUBLISH_UNCHANGED;
    dummy_completion_record.evidence_publish_status =
        COMMON_OUTPUT_PUBLISH_UNCHANGED;
    CHECK(!material_batch_is_complete(&malformed_completion));
    dummy_completion_record.material_publish_status =
        COMMON_OUTPUT_PUBLISH_EMITTED;
    dummy_completion_record.status = MATERIAL_BATCH_UNCHANGED;
    malformed_completion.stats.emitted = 0U;
    malformed_completion.stats.unchanged = 1U;
    CHECK(!material_batch_is_complete(&malformed_completion));
    dummy_completion_record.status = MATERIAL_BATCH_EMITTED;
    malformed_completion.stats.emitted = 1U;
    malformed_completion.stats.unchanged = 0U;
    dummy_completion_record.meta_publish_status =
        COMMON_OUTPUT_PUBLISH_EMITTED;
    dummy_completion_record.evidence_publish_status =
        COMMON_OUTPUT_PUBLISH_EMITTED;

    MaterialBatchTextureDependency contradictory_dependency;
    memset(&contradictory_dependency, 0,
           sizeof(contradictory_dependency));
    static uint8_t contradictory_property_name[] = "_MainTex";
    contradictory_dependency.property_name = contradictory_property_name;
    contradictory_dependency.property_name_size =
        sizeof(contradictory_property_name) - 1U;
    contradictory_dependency.source_index = 0U;
    contradictory_dependency.status =
        MATERIAL_BATCH_DEPENDENCY_RESOLVED_NOT_EXPORTED;
    dummy_completion_record.dependencies = &contradictory_dependency;
    dummy_completion_record.dependency_count = 1U;
    dummy_completion_record.texture_dependency_closure_complete = true;
    malformed_completion.stats.texture_dependencies = 1U;
    malformed_completion.stats.resolved_texture_dependencies = 1U;
    malformed_completion.stats.exported_texture_dependencies = 1U;
    CHECK(!material_batch_texture_dependencies_are_closed(
        &malformed_completion));

    /* An exported enum and matching counters are not dependency closure
     * without the exact PPtr target and exported YAML tuple evidence. */
    contradictory_dependency.status =
        MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED;
    CHECK(!material_batch_texture_dependencies_are_closed(
        &malformed_completion));
    contradictory_dependency.serialized_path_id = 77;
    contradictory_dependency.resolve_status = UNITY_PPTR_RESOLVE_LOCAL;
    contradictory_dependency.reference_status =
        MATERIAL_BATCH_TEXTURE_REFERENCE_OK;
    contradictory_dependency.source_index = 0U;
    contradictory_dependency.target_source_index = 0U;
    contradictory_dependency.external_index = SIZE_MAX;
    contradictory_dependency.has_target = true;
    contradictory_dependency.target_class_id = 28;
    contradictory_dependency.target_path_id = 77;
    memset(contradictory_dependency.target_serialized_digest_hex, 'a',
           COMMON_SHA256_DIGEST_SIZE * 2U);
    contradictory_dependency.target_serialized_digest_hex[
        COMMON_SHA256_DIGEST_SIZE * 2U] = '\0';
    contradictory_dependency.has_yaml_reference = true;
    contradictory_dependency.yaml_reference.file_id = 2800000;
    memcpy(contradictory_dependency.yaml_reference.guid,
           "0123456789abcdef0123456789abcdef",
           UNITY_ASSET_GUID_TEXT_CAPACITY);
    contradictory_dependency.yaml_reference.type = 2;
    contradictory_dependency.yaml_reference.asset_exported = true;
    CHECK(material_batch_texture_dependencies_are_closed(
        &malformed_completion));
    contradictory_dependency.target_class_id = 0;
    CHECK(!material_batch_texture_dependencies_are_closed(
        &malformed_completion));
    contradictory_dependency.target_class_id = 28;
    contradictory_dependency.property_name_size = 0U;
    CHECK(!material_batch_texture_dependencies_are_closed(
        &malformed_completion));
    contradictory_dependency.property_name_size =
        sizeof(contradictory_property_name) - 1U;
    contradictory_dependency.has_yaml_reference = false;
    CHECK(!material_batch_texture_dependencies_are_closed(
        &malformed_completion));

    int written = snprintf(
        input_root, sizeof(input_root), "dxbc_material_canonical_input_%lu",
        TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(input_root));
    written = snprintf(
        output_root, sizeof(output_root),
        "dxbc_material_canonical_output_%lu", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(output_root));
    (void)TEST_RMDIR(input_root);
    (void)TEST_RMDIR(output_root);
    CHECK(TEST_MKDIR(input_root) == 0);
    written = snprintf(shader_path, sizeof(shader_path),
                       "%s%cshader.assets", input_root,
                       TEST_PATH_SEPARATOR);
    CHECK(written > 0 && (size_t)written < sizeof(shader_path));
    written = snprintf(material_path, sizeof(material_path),
                       "%s%cmaterial.assets", input_root,
                       TEST_PATH_SEPARATOR);
    CHECK(written > 0 && (size_t)written < sizeof(material_path));
    written = snprintf(duplicate_material_path,
                       sizeof(duplicate_material_path),
                       "%s%caaa_material.assets", input_root,
                       TEST_PATH_SEPARATOR);
    CHECK(written > 0 &&
          (size_t)written < sizeof(duplicate_material_path));

    static const uint8_t material_type_hash[16] = {
        0xc6U, 0x00U, 0x98U, 0xacU, 0x66U, 0xa2U, 0x8bU, 0x50U,
        0xaaU, 0x05U, 0x80U, 0xdbU, 0x11U, 0xbfU, 0x01U, 0x8cU,
    };
    CHECK(build_single_object_file(
        &shader_file, 48, search->type_hash, 101, search->payload,
        search->payload_size, NULL));
    CHECK(build_material_payload(
        &material_payload, "Canonical Material", 1, 101, 0));
    CHECK(build_single_object_file(
        &material_file, 21, material_type_hash, 201,
        material_payload.data, material_payload.size, "shader.assets"));
    CHECK(write_new(shader_path, &shader_file));
    CHECK(write_new(material_path, &material_file));

    ShaderCatalogOptions catalog_options;
    shader_catalog_options_default(&catalog_options);
    catalog_options.schema_registry = registry;
    catalog_options.include_materials = true;
    const char* inputs[] = {input_root};
    CHECK(shader_catalog_build(
              inputs, 1U, &catalog_options, &catalog) == SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.material_count == 1U && catalog.record_count == 1U);
    size_t shader_index;
    size_t compute_index;
    locate_records(&catalog, &shader_index, &compute_index);
    CHECK(shader_index != SIZE_MAX && compute_index == SIZE_MAX);
    selected = (bool*)calloc(catalog.record_count, sizeof(*selected));
    CHECK(selected != NULL);
    selected[shader_index] = true;
    CHECK(setup_fake_shader_batch(&catalog, shader_index, &shader_batch));
    ShaderCatalog unrelated_catalog = catalog;
    ShaderBatchResult unrelated_batch = shader_batch;
    unrelated_batch.catalog_authority = &unrelated_catalog;
    CHECK(material_batch_export(
              &catalog, selected, &unrelated_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_INVALID_ARGUMENT);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(material_batch_is_complete(&material_batch));
    selected[shader_index] = false;
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(!material_batch_texture_dependencies_are_closed(
        &material_batch));
    selected[shader_index] = true;
    CHECK(material_batch_is_complete(&material_batch));
    CHECK(material_batch.stats.emitted == 1U &&
          material_batch.stats.failed == 0U);
    CHECK(material_batch.stats.shader_dependency_groups == 1U);
    emitted_material_path = duplicate_string(
        material_batch.records[0].output_path);
    emitted_meta_path = duplicate_string(
        material_batch.records[0].output_meta_path);
    emitted_evidence_path = duplicate_string(
        material_batch.records[0].dependency_evidence_path);
    emitted_directory = duplicate_parent(emitted_material_path);
    CHECK(emitted_material_path && emitted_meta_path &&
          emitted_evidence_path && emitted_directory);
    CHECK(common_file_read_regular(
              emitted_evidence_path, SIZE_MAX, &first_evidence) ==
          COMMON_FILE_OK);
    CHECK(bytes_contain(
        first_evidence.data, first_evidence.size,
        "DXBCSandbox.material-dependencies.v2"));
    CHECK(!bytes_contain(
        first_evidence.data, first_evidence.size, "\"record_index\""));
    CHECK(!bytes_contain(
        first_evidence.data, first_evidence.size, "\"source_index\""));
    CHECK(!bytes_contain(
        first_evidence.data, first_evidence.size,
        "\"target_source_index\""));
    CHECK(!bytes_contain(
        first_evidence.data, first_evidence.size, "\"external_index\""));

    /* A byte-identical source with an earlier lexical name shifts every
     * subsequent catalog source index.  Both occurrences address the same
     * content-derived artifact, so neither may produce a false collision. */
    CHECK(write_new(duplicate_material_path, &material_file));
    material_batch_result_dispose(&material_batch);
    shader_batch_result_dispose(&shader_batch);
    shader_catalog_dispose(&catalog);
    free(selected);
    selected = NULL;
    CHECK(shader_catalog_build(
              inputs, 1U, &catalog_options, &catalog) == SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.material_count == 2U && catalog.record_count == 1U);
    CHECK(catalog.materials[0].source_index !=
          catalog.materials[1].source_index);
    locate_records(&catalog, &shader_index, &compute_index);
    CHECK(shader_index != SIZE_MAX && compute_index == SIZE_MAX);
    selected = (bool*)calloc(catalog.record_count, sizeof(*selected));
    CHECK(selected != NULL);
    selected[shader_index] = true;
    CHECK(setup_fake_shader_batch(&catalog, shader_index, &shader_batch));
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(material_batch_is_complete(&material_batch));
    CHECK(material_batch.stats.selected == 2U);
    CHECK(material_batch.stats.emitted == 0U);
    CHECK(material_batch.stats.unchanged == 2U);
    CHECK(material_batch.stats.failed == 0U);
    /* Both catalog occurrences share one exact Shader dependency.  The
     * exporter must hold and verify that large shader/meta pair once for the
     * complete group, not once per Material. */
    CHECK(material_batch.stats.shader_dependency_groups == 1U);
    CHECK(common_file_read_regular(
              emitted_evidence_path, SIZE_MAX, &repeated_evidence) ==
          COMMON_FILE_OK);
    CHECK(first_evidence.size == repeated_evidence.size);
    CHECK(memcmp(first_evidence.data, repeated_evidence.data,
                 first_evidence.size) == 0);

cleanup:
    (void)remove("associated.shader.meta");
    (void)remove("associated.shader");
    common_file_bytes_dispose(&first_evidence);
    common_file_bytes_dispose(&repeated_evidence);
    material_batch_result_dispose(&material_batch);
    shader_batch_result_dispose(&shader_batch);
    shader_catalog_dispose(&catalog);
    free(selected);
    buffer_dispose(&shader_file);
    buffer_dispose(&material_payload);
    buffer_dispose(&material_file);
    if (emitted_meta_path) (void)remove(emitted_meta_path);
    if (emitted_material_path) (void)remove(emitted_material_path);
    if (emitted_evidence_path) (void)remove(emitted_evidence_path);
    if (emitted_directory) (void)TEST_RMDIR(emitted_directory);
    if (duplicate_material_path[0]) (void)remove(duplicate_material_path);
    if (material_path[0]) (void)remove(material_path);
    if (shader_path[0]) (void)remove(shader_path);
    if (input_root[0]) (void)TEST_RMDIR(input_root);
    if (output_root[0]) (void)TEST_RMDIR(output_root);
    free(emitted_directory);
    free(emitted_evidence_path);
    free(emitted_meta_path);
    free(emitted_material_path);
    return status;
}

int main(void) {
    int status = 0;
    static const uint8_t material_type_hash[16] = {
        0xc6U, 0x00U, 0x98U, 0xacU, 0x66U, 0xa2U, 0x8bU, 0x50U,
        0xaaU, 0x05U, 0x80U, 0xdbU, 0x11U, 0xbfU, 0x01U, 0x8cU,
    };
    static const uint8_t compute_type_hash[16] = {
        0xabU, 0xd9U, 0x13U, 0x5bU, 0x8cU, 0xe8U, 0x3dU, 0x04U,
        0x3fU, 0xefU, 0x4eU, 0x9eU, 0xc7U, 0xf5U, 0x33U, 0x66U,
    };
    TypeTreeSchemaRegistry registry;
    ShaderCatalog catalog;
    ShaderBatchResult shader_batch;
    MaterialBatchResult material_batch;
    ShaderPayloadSearch search;
    ByteBuffer shader_file = {0};
    ByteBuffer material_payload = {0};
    ByteBuffer material_file = {0};
    ByteBuffer compute_payload = {0};
    ByteBuffer compute_file = {0};
    bool* selected = NULL;
    char input_root[320] = {0};
    char output_root[320] = {0};
    char invalid_output_root[320] = {0};
    char shader_path[512] = {0};
    char material_path[512] = {0};
    char compute_path[512] = {0};
    bool input_root_created = false;
    bool output_root_created = false;
    char* emitted_material_path = NULL;
    char* emitted_meta_path = NULL;
    char* emitted_evidence_path = NULL;
    char* emitted_directory = NULL;
    char* cubemap_material_path = NULL;
    char* cubemap_meta_path = NULL;
    char* cubemap_evidence_path = NULL;
    char* cubemap_directory = NULL;
    CommonFileBytes file_bytes = {0};

    typetree_schema_registry_init(&registry);
    shader_catalog_init(&catalog);
    shader_batch_result_init(&shader_batch);
    material_batch_result_init(&material_batch);
    memset(&search, 0, sizeof(search));

    CHECK(typetree_schema_registry_import_file_replace(
              &registry, DXBC_TEST_PLAYER_SCHEMA_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    search.registry = &registry;
    UnityInputVisitStats visit_stats;
    CHECK(unity_input_visit_serialized(
              DXBC_TEST_SHADER_BUNDLE, extract_shader_payload, &search,
              &visit_stats) == UNITY_INPUT_OK);
    CHECK(search.found && search.payload && search.payload_size != 0U);
    CHECK(test_content_canonical_dependency_evidence(
              &registry, &search) == 0);

    int written = snprintf(input_root, sizeof(input_root),
                           "dxbc_material_input_%lu", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(input_root));
    written = snprintf(output_root, sizeof(output_root),
                       "dxbc_material_output_%lu", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(output_root));
    written = snprintf(invalid_output_root, sizeof(invalid_output_root),
                       "dxbc_material_invalid_%lu", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(invalid_output_root));
    (void)TEST_RMDIR(input_root);
    (void)TEST_RMDIR(output_root);
    (void)TEST_RMDIR(invalid_output_root);
    CHECK(TEST_MKDIR(input_root) == 0);
    input_root_created = true;
    written = snprintf(shader_path, sizeof(shader_path), "%s%cshader.assets",
                       input_root, TEST_PATH_SEPARATOR);
    CHECK(written > 0 && (size_t)written < sizeof(shader_path));
    written = snprintf(material_path, sizeof(material_path),
                       "%s%cmaterial.assets", input_root,
                       TEST_PATH_SEPARATOR);
    CHECK(written > 0 && (size_t)written < sizeof(material_path));
    written = snprintf(compute_path, sizeof(compute_path),
                       "%s%ccompute.assets", input_root,
                       TEST_PATH_SEPARATOR);
    CHECK(written > 0 && (size_t)written < sizeof(compute_path));

    CHECK(build_single_object_file(
        &shader_file, 48, search.type_hash, 101, search.payload,
        search.payload_size, NULL));
    CHECK(build_material_payload(
        &material_payload, "Exact Material", 1, 101, 0));
    CHECK(build_single_object_file(
        &material_file, 21, material_type_hash, 201,
        material_payload.data, material_payload.size, "shader.assets"));
    CHECK(build_compute_payload(&compute_payload));
    CHECK(build_single_object_file(
        &compute_file, 72, compute_type_hash, 301,
        compute_payload.data, compute_payload.size, NULL));
    CHECK(write_new(shader_path, &shader_file));
    CHECK(write_new(material_path, &material_file));
    CHECK(write_new(compute_path, &compute_file));

    ShaderCatalogOptions catalog_options;
    shader_catalog_options_default(&catalog_options);
    catalog_options.schema_registry = &registry;
    catalog_options.include_materials = true;
    const char* inputs[] = {input_root};
    CHECK(shader_catalog_build(
              inputs, 1U, &catalog_options, &catalog) == SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.materials_included);
    CHECK(catalog.material_count == 1U);
    CHECK(catalog.stats.material_objects == 1U);
    CHECK(catalog.stats.decoded_materials == 1U);
    CHECK(catalog.stats.resolved_material_shader_links == 1U);
    CHECK(catalog.stats.unresolved_material_shader_links == 0U);
    CHECK(catalog.stats.failed_materials == 0U);
    CHECK(catalog.materials[0].status == SHADER_CATALOG_MATERIAL_READY);
    CHECK(catalog.materials[0].object_status == MATERIAL_OBJECT_OK);
    CHECK(catalog.materials[0].shader_link_status ==
          UNITY_PPTR_RESOLVE_EXTERNAL_EXACT);
    CHECK(catalog.materials[0].object.shader.file_id == 1);
    CHECK(catalog.materials[0].object.shader.path_id == 101);
    CHECK(catalog.materials[0].object.texture_property_count == 1U);
    CHECK(catalog.materials[0].object.float_property_count == 1U);
    CHECK(catalog.materials[0].object.float_properties[0].value.bits ==
          UINT32_C(0x80000000));

    size_t shader_index;
    size_t compute_index;
    locate_records(&catalog, &shader_index, &compute_index);
    CHECK(shader_index != SIZE_MAX && compute_index != SIZE_MAX);
    CHECK(catalog.materials[0].shader_record_index == shader_index);
    selected = (bool*)calloc(catalog.record_count, sizeof(*selected));
    CHECK(selected != NULL);
    CHECK(setup_fake_shader_batch(&catalog, shader_index, &shader_batch));
    ShaderCatalog missing_records = catalog;
    missing_records.records = NULL;
    CHECK(material_batch_export(
              &missing_records, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_INVALID_ARGUMENT);
    ShaderCatalogMaterialRecord malformed_material = catalog.materials[0];
    malformed_material.object.texture_properties = NULL;
    ShaderCatalog malformed_catalog = catalog;
    malformed_catalog.materials = &malformed_material;
    CHECK(material_batch_export(
              &malformed_catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_INVALID_ARGUMENT);

    /* A compute-only shader selection has no associated Material and is a
     * complete zero-material operation, not an error or an implicit export. */
    selected[compute_index] = true;
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(material_batch_is_complete(&material_batch));
    CHECK(material_batch.stats.catalog_materials == 1U);
    CHECK(material_batch.stats.selected == 0U);
    CHECK(material_batch.stats.unselected == 1U);
    CHECK(material_batch.stats.emitted == 0U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_UNSELECTED);

    memset(selected, 0, catalog.record_count * sizeof(*selected));
    selected[shader_index] = true;
    material_batch_result_dispose(&material_batch);
    shader_batch.records[shader_index].publication_authorized = false;
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(material_batch.stats.selected == 1U);
    CHECK(material_batch.stats.failed == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_FAILED);
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_SHADER_BATCH_NOT_READY);
    CHECK(!material_batch.records[0].has_artifacts);

    material_batch_result_dispose(&material_batch);
    shader_batch.records[shader_index].publication_authorized = true;
    char saved_shader_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    memcpy(saved_shader_guid, shader_batch.records[shader_index].asset_guid,
           sizeof(saved_shader_guid));
    shader_batch.records[shader_index].asset_guid[0] =
        shader_batch.records[shader_index].asset_guid[0] == '0' ? '1' : '0';
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY);
    memcpy(shader_batch.records[shader_index].asset_guid, saved_shader_guid,
           sizeof(saved_shader_guid));
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    output_root_created = true;
    CHECK(material_batch_is_complete(&material_batch));
    CHECK(material_batch_texture_dependencies_are_closed(&material_batch));
    CHECK(material_batch.stats.selected == 1U);
    CHECK(material_batch.stats.emitted == 1U);
    CHECK(material_batch.stats.failed == 0U);
    CHECK(material_batch.stats.shader_dependency_groups == 1U);
    CHECK(material_batch.stats.texture_dependencies == 1U);
    CHECK(material_batch.stats.null_texture_dependencies == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_EMITTED);
    CHECK(material_batch.records[0].has_artifacts);
    CHECK(material_batch.records[0].dependency_count == 1U);
    CHECK(material_batch.records[0].dependencies[0].status ==
          MATERIAL_BATCH_DEPENDENCY_NULL);
    CHECK(material_batch.records[0].dependencies[0].resolve_status ==
          UNITY_PPTR_RESOLVE_NULL);
    CHECK(material_batch.records[0].texture_dependency_closure_complete);

    CHECK(common_file_read_regular(
              material_batch.records[0].output_path, SIZE_MAX,
              &file_bytes) == COMMON_FILE_OK);
    uint8_t yaml_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(file_bytes.data, file_bytes.size, yaml_digest);
    uint8_t material_identity[COMMON_SHA256_DIGEST_SIZE + 8U +
                              COMMON_SHA256_DIGEST_SIZE];
    memcpy(material_identity, catalog.materials[0].serialized_digest,
           COMMON_SHA256_DIGEST_SIZE);
    for (unsigned index = 0U; index < 8U; ++index) {
        material_identity[COMMON_SHA256_DIGEST_SIZE + index] =
            (uint8_t)((uint64_t)catalog.materials[0].path_id >>
                      (index * 8U));
    }
    memcpy(material_identity + COMMON_SHA256_DIGEST_SIZE + 8U,
           yaml_digest, sizeof(yaml_digest));
    char expected_material_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    CHECK(unity_asset_guid_derive(
        UNITY_ASSET_GUID_DOMAIN_MATERIAL, material_identity,
        sizeof(material_identity), expected_material_guid));
    CHECK(strcmp(material_batch.records[0].asset_guid,
                 expected_material_guid) == 0);
    uint8_t artifact_identity[COMMON_SHA256_DIGEST_SIZE];
    char artifact_identity_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    common_sha256(material_identity, sizeof(material_identity),
                  artifact_identity);
    common_sha256_digest_to_hex(artifact_identity,
                                artifact_identity_hex);
    CHECK(strcmp(material_batch.records[0].artifact_identity_hex,
                 artifact_identity_hex) == 0);
    char shader_guid_yaml[48];
    int shader_guid_written = snprintf(
        shader_guid_yaml, sizeof(shader_guid_yaml), "guid: %s",
        shader_batch.records[shader_index].asset_guid);
    CHECK(shader_guid_written > 0 &&
          (size_t)shader_guid_written < sizeof(shader_guid_yaml));
    CHECK(bytes_contain(
        file_bytes.data, file_bytes.size, shader_guid_yaml));
    common_file_bytes_dispose(&file_bytes);

    emitted_material_path = duplicate_string(
        material_batch.records[0].output_path);
    emitted_meta_path = duplicate_string(
        material_batch.records[0].output_meta_path);
    emitted_evidence_path = duplicate_string(
        material_batch.records[0].dependency_evidence_path);
    emitted_directory = duplicate_parent(emitted_material_path);
    CHECK(emitted_material_path && emitted_meta_path &&
          emitted_evidence_path && emitted_directory);

    /* Historical shader-batch flags are insufficient dependency authority.
     * The exact published shader and metadata stay mapped and identity-checked
     * through each Material commit. */
    CHECK(remove("associated.shader") == 0);
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY);
    CHECK(setup_fake_shader_batch(&catalog, shader_index, &shader_batch));

    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(material_batch_is_complete(&material_batch));
    CHECK(material_batch.stats.emitted == 0U);
    CHECK(material_batch.stats.unchanged == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_UNCHANGED);
    CHECK(material_batch.records[0].material_publish_status ==
          COMMON_OUTPUT_PUBLISH_UNCHANGED);
    CHECK(material_batch.records[0].meta_publish_status ==
          COMMON_OUTPUT_PUBLISH_UNCHANGED);
    CHECK(material_batch.records[0].evidence_publish_status ==
          COMMON_OUTPUT_PUBLISH_UNCHANGED);

    /* The three-file artifact set is preflighted before publication.  A
     * conflicting .meta with an absent .mat cannot leave a partial .mat. */
    CHECK(remove(emitted_material_path) == 0);
    CHECK(remove(emitted_meta_path) == 0);
    static const uint8_t conflict[] = "conflicting material meta\n";
    CHECK(common_file_write_new_atomic(
              emitted_meta_path, conflict, sizeof(conflict) - 1U) ==
          COMMON_FILE_OK);
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(material_batch.stats.failed == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_FAILED);
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_OUTPUT_COLLISION);
    CHECK(!material_batch.records[0].has_artifacts);
    CHECK(material_batch.records[0].output_path == NULL);
    CHECK(common_file_read_regular(
              emitted_material_path, SIZE_MAX, &file_bytes) ==
          COMMON_FILE_NOT_FOUND);
    CHECK(common_file_read_regular(
              emitted_meta_path, SIZE_MAX, &file_bytes) == COMMON_FILE_OK);
    CHECK(file_bytes.size == sizeof(conflict) - 1U);
    CHECK(memcmp(file_bytes.data, conflict, sizeof(conflict) - 1U) == 0);
    common_file_bytes_dispose(&file_bytes);

    /* Rebuild the exact material with a non-null local pointer whose target
     * does not exist. Catalog shader association remains exact, while the
     * exporter records and fails the texture dependency instead of silently
     * emitting {fileID: 0}. */
    material_batch_result_dispose(&material_batch);
    shader_batch_result_dispose(&shader_batch);
    free(selected);
    selected = NULL;
    buffer_dispose(&material_payload);
    buffer_dispose(&material_file);
    CHECK(build_material_payload(
        &material_payload, "Invalid Texture Material", 1, 101, 9999));
    CHECK(build_single_object_file(
        &material_file, 21, material_type_hash, 202,
        material_payload.data, material_payload.size, "shader.assets"));
    CHECK(write_new(material_path, &material_file));
    CHECK(shader_catalog_build(
              inputs, 1U, &catalog_options, &catalog) == SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.material_count == 1U);
    CHECK(catalog.materials[0].status == SHADER_CATALOG_MATERIAL_READY);
    locate_records(&catalog, &shader_index, &compute_index);
    CHECK(shader_index != SIZE_MAX && compute_index != SIZE_MAX);
    selected = (bool*)calloc(catalog.record_count, sizeof(*selected));
    CHECK(selected != NULL);
    selected[shader_index] = true;
    CHECK(setup_fake_shader_batch(&catalog, shader_index, &shader_batch));
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, invalid_output_root, NULL,
              &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(material_batch.stats.selected == 1U);
    CHECK(material_batch.stats.failed == 1U);
    CHECK(material_batch.stats.failed_texture_dependencies == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_FAILED);
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_TEXTURE_RESOLUTION);
    CHECK(material_batch.records[0].dependency_count == 1U);
    CHECK(material_batch.records[0].dependencies[0].resolve_status ==
          UNITY_PPTR_RESOLVE_TARGET_MISSING);
    CHECK(material_batch.records[0].dependencies[0].status ==
          MATERIAL_BATCH_DEPENDENCY_RESOLUTION_FAILED);
    CHECK(!material_batch.records[0].has_artifacts);

    /* The production batch owns no hard-coded Texture subclass list.  Once
     * exact callback authority accepts this concrete ClassID 89 Cubemap, its
     * YAML main-object identity is emitted unchanged. */
    ShaderCatalogSource* material_source =
        &catalog.sources[catalog.materials[0].source_index];
    CHECK(append_resolver_object(material_source, 89, 9999));
    TextureReferenceFixture texture_fixture = {
        .status = MATERIAL_BATCH_TEXTURE_REFERENCE_OK,
        .expected_class_id = 89,
        .asset_exported = true,
        .lease_stable = true,
    };
    MaterialBatchOptions material_options;
    material_batch_options_default(&material_options);
    material_options.resolve_texture_reference = resolve_texture_fixture;
    material_options.texture_reference_context = &texture_fixture;
    material_options.close_texture_reference_publication_lease =
        close_texture_fixture_lease;

    /* Global source/dependency authority is checked even when every texture
     * property is null and the per-reference callback would never run. */
    AssetPPtr saved_texture =
        catalog.materials[0].object.texture_properties[0].texture;
    catalog.materials[0].object.texture_properties[0].texture.file_id = 0;
    catalog.materials[0].object.texture_properties[0].texture.path_id = 0;
    material_options.dependency_authority_complete = false;
    texture_fixture.calls = 0U;
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, invalid_output_root,
              &material_options, &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_FAILED);
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_DEPENDENCY_AUTHORITY_INCOMPLETE);
    CHECK(material_batch.records[0].dependency_count == 0U);
    CHECK(texture_fixture.calls == 0U);
    CHECK(!material_batch.records[0].has_artifacts);
    CHECK(!material_batch.records[0].meta_publish_attempted);
    CHECK(!material_batch.records[0].evidence_publish_attempted);
    CHECK(!material_batch.records[0].material_publish_attempted);
    CHECK(material_batch.records[0].meta_publish_status ==
          COMMON_OUTPUT_PUBLISH_IO_ERROR);
    CHECK(material_batch.records[0].evidence_publish_status ==
          COMMON_OUTPUT_PUBLISH_IO_ERROR);
    CHECK(material_batch.records[0].material_publish_status ==
          COMMON_OUTPUT_PUBLISH_IO_ERROR);
    CHECK(!material_batch.records[0].publication_residue);
    catalog.materials[0].object.texture_properties[0].texture = saved_texture;
    material_options.dependency_authority_complete = true;
    texture_fixture.calls = 0U;
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, invalid_output_root,
              &material_options, &material_batch) == MATERIAL_BATCH_OK);
    CHECK(material_batch_is_complete(&material_batch));
    CHECK(material_batch_texture_dependencies_are_closed(&material_batch));
    CHECK(texture_fixture.calls == 1U);
    CHECK(texture_fixture.lease_closes == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_EMITTED);
    CHECK(material_batch.records[0].dependency_count == 1U);
    CHECK(material_batch.records[0].dependencies[0].target_class_id == 89);
    CHECK(material_batch.records[0].dependencies[0].reference_status ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(material_batch.records[0].dependencies[0].status ==
          MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED);
    CHECK(material_batch.records[0].dependencies[0].has_yaml_reference);
    CHECK(material_batch.records[0].dependencies[0].yaml_reference.file_id ==
          8900000);
    CHECK(strcmp(material_batch_texture_reference_status_name(
                     MATERIAL_BATCH_TEXTURE_REFERENCE_OK), "ok") == 0);
    cubemap_material_path = duplicate_string(
        material_batch.records[0].output_path);
    cubemap_meta_path = duplicate_string(
        material_batch.records[0].output_meta_path);
    cubemap_evidence_path = duplicate_string(
        material_batch.records[0].dependency_evidence_path);
    cubemap_directory = duplicate_parent(cubemap_material_path);
    CHECK(cubemap_material_path && cubemap_meta_path &&
          cubemap_evidence_path && cubemap_directory);
    CHECK(common_file_read_regular(
              cubemap_material_path, SIZE_MAX, &file_bytes) ==
          COMMON_FILE_OK);
    CHECK(bytes_contain(
        file_bytes.data, file_bytes.size,
        "{fileID: 8900000, guid: 22222222222222222222222222222222, "
        "type: 3}"));
    common_file_bytes_dispose(&file_bytes);

    /* Exported texture asset/meta identity is leased through the Material
     * commit.  A close-time mutation invalidates the transaction and leaves
     * any already-created Material files explicitly marked as residue. */
    CHECK(remove(cubemap_material_path) == 0);
    CHECK(remove(cubemap_meta_path) == 0);
    CHECK(remove(cubemap_evidence_path) == 0);
    texture_fixture.lease_stable = false;
    texture_fixture.calls = 0U;
    texture_fixture.lease_closes = 0U;
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, invalid_output_root,
              &material_options, &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(texture_fixture.calls == 1U);
    CHECK(texture_fixture.lease_closes == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_FAILED);
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_TEXTURE_DEPENDENCY_IDENTITY);
    CHECK(material_batch.records[0].publication_residue);
    CHECK(strcmp(material_batch_failure_name(
                     MATERIAL_BATCH_FAILURE_TEXTURE_DEPENDENCY_IDENTITY),
                 "texture-dependency-identity") == 0);
    texture_fixture.lease_stable = true;

    /* A callback may own an exact YAML tuple for an asset that is outside the
     * current export set.  The Material remains a complete deterministic
     * projection, while the stronger texture-closure claim stays false. */
    CHECK(remove(cubemap_material_path) == 0);
    CHECK(remove(cubemap_meta_path) == 0);
    CHECK(remove(cubemap_evidence_path) == 0);
    texture_fixture.asset_exported = false;
    texture_fixture.calls = 0U;
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, invalid_output_root,
              &material_options, &material_batch) == MATERIAL_BATCH_OK);
    CHECK(material_batch_is_complete(&material_batch));
    CHECK(!material_batch_texture_dependencies_are_closed(&material_batch));
    CHECK(texture_fixture.calls == 1U);
    CHECK(material_batch.stats.failed_texture_dependencies == 0U);
    CHECK(material_batch.stats.unexported_texture_dependencies == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_EMITTED);
    CHECK(material_batch.records[0].failure == MATERIAL_BATCH_FAILURE_NONE);
    CHECK(material_batch.records[0].dependencies[0].status ==
          MATERIAL_BATCH_DEPENDENCY_RESOLVED_NOT_EXPORTED);
    CHECK(material_batch.records[0].dependencies[0].has_yaml_reference);
    CHECK(!material_batch.records[0].dependencies[0]
               .yaml_reference.asset_exported);
    CHECK(common_file_read_regular(
              cubemap_evidence_path, SIZE_MAX, &file_bytes) ==
          COMMON_FILE_OK);
    CHECK(bytes_contain(file_bytes.data, file_bytes.size,
                        "\"status\": \"resolved-not-exported\""));
    CHECK(bytes_contain(file_bytes.data, file_bytes.size,
                        "\"asset_exported\": false"));
    common_file_bytes_dispose(&file_bytes);

    /* A recognized target whose artifact/importer has not been exported has
     * no exact YAML tuple yet.  That condition is explicit and fails closed;
     * it is not confused with a supported but merely out-of-set reference. */
    texture_fixture.status =
        MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED;
    texture_fixture.asset_exported = false;
    texture_fixture.calls = 0U;
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, invalid_output_root,
              &material_options, &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(!material_batch_texture_dependencies_are_closed(&material_batch));
    CHECK(texture_fixture.calls == 1U);
    CHECK(material_batch.stats.unexported_texture_dependencies == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_FAILED);
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_TEXTURE_TARGET_ASSET_UNEXPORTED);
    CHECK(material_batch.records[0].dependencies[0].status ==
          MATERIAL_BATCH_DEPENDENCY_TARGET_ASSET_UNEXPORTED);
    CHECK(material_batch.records[0].dependencies[0].reference_status ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(strcmp(material_batch_texture_reference_status_name(
                     MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED),
                 "target-asset-unexported") == 0);
    CHECK(!material_batch.records[0].has_artifacts);

    /* An explicit callback class-policy rejection is distinct from malformed
     * authority and remains visible in both failure and dependency evidence. */
    texture_fixture.status =
        MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_CLASS_UNSUPPORTED;
    texture_fixture.calls = 0U;
    material_batch_result_dispose(&material_batch);
    CHECK(material_batch_export(
              &catalog, selected, &shader_batch, invalid_output_root,
              &material_options, &material_batch) == MATERIAL_BATCH_OK);
    CHECK(!material_batch_is_complete(&material_batch));
    CHECK(texture_fixture.calls == 1U);
    CHECK(material_batch.records[0].status == MATERIAL_BATCH_FAILED);
    CHECK(material_batch.records[0].failure ==
          MATERIAL_BATCH_FAILURE_TEXTURE_CLASS_UNSUPPORTED);
    CHECK(material_batch.records[0].dependencies[0].status ==
          MATERIAL_BATCH_DEPENDENCY_CLASS_UNSUPPORTED);
    CHECK(material_batch.records[0].dependencies[0].reference_status ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_CLASS_UNSUPPORTED);
    CHECK(strcmp(material_batch_texture_reference_status_name(
                     MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_CLASS_UNSUPPORTED),
                 "target-class-unsupported") == 0);
    CHECK(!material_batch.records[0].has_artifacts);

cleanup:
    (void)remove("associated.shader.meta");
    (void)remove("associated.shader");
    common_file_bytes_dispose(&file_bytes);
    material_batch_result_dispose(&material_batch);
    shader_batch_result_dispose(&shader_batch);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    free(selected);
    free(search.payload);
    buffer_dispose(&shader_file);
    buffer_dispose(&material_payload);
    buffer_dispose(&material_file);
    buffer_dispose(&compute_payload);
    buffer_dispose(&compute_file);
    if (emitted_meta_path) (void)remove(emitted_meta_path);
    if (emitted_material_path) (void)remove(emitted_material_path);
    if (emitted_evidence_path) (void)remove(emitted_evidence_path);
    if (emitted_directory) (void)TEST_RMDIR(emitted_directory);
    if (cubemap_meta_path) (void)remove(cubemap_meta_path);
    if (cubemap_material_path) (void)remove(cubemap_material_path);
    if (cubemap_evidence_path) (void)remove(cubemap_evidence_path);
    if (cubemap_directory) (void)TEST_RMDIR(cubemap_directory);
    if (output_root_created) (void)TEST_RMDIR(output_root);
    if (shader_path[0]) (void)remove(shader_path);
    if (material_path[0]) (void)remove(material_path);
    if (compute_path[0]) (void)remove(compute_path);
    if (input_root_created) (void)TEST_RMDIR(input_root);
    (void)TEST_RMDIR(invalid_output_root);
    free(emitted_directory);
    free(emitted_evidence_path);
    free(emitted_meta_path);
    free(emitted_material_path);
    free(cubemap_directory);
    free(cubemap_evidence_path);
    free(cubemap_meta_path);
    free(cubemap_material_path);
    if (status == 0) puts("Material catalog/batch unit tests passed.");
    return status;
}
