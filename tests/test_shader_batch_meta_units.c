#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "app/shader_batch.h"
#include "app/shader_batch_internal.h"
#include "app/shader_catalog.h"
#include "common/file_io.h"
#include "common/string_builder.h"
#include "common/unity_asset_guid.h"
#include "io/typetree_schema_registry.h"
#include "translation/material_yaml_emitter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define TEST_PROCESS_ID() ((unsigned long)_getpid())
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_PATH_SEPARATOR '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_PROCESS_ID() ((unsigned long)getpid())
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

static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    char* copy = (char*)malloc(size + 1U);
    if (copy) memcpy(copy, value, size + 1U);
    return copy;
}

static char* duplicate_parent_path(const char* path) {
    char* copy = duplicate_string(path);
    if (!copy) return NULL;
    char* separator = NULL;
    for (char* cursor = copy; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') separator = cursor;
    }
    if (!separator) {
        free(copy);
        return NULL;
    }
    *separator = '\0';
    return copy;
}

static bool path_has_meta_suffix(const char* path, const char* meta_path) {
    if (!path || !meta_path) return false;
    size_t path_size = strlen(path);
    return strlen(meta_path) == path_size + sizeof(".meta") - 1U &&
        memcmp(meta_path, path, path_size) == 0 &&
        strcmp(meta_path + path_size, ".meta") == 0;
}

static bool derive_expected_shader_guid(
    const ShaderCatalogRecord* record,
    char output[UNITY_ASSET_GUID_TEXT_CAPACITY]) {
    uint8_t identity[COMMON_SHA256_DIGEST_SIZE + 8U];
    memcpy(identity, record->serialized_digest, COMMON_SHA256_DIGEST_SIZE);
    uint64_t path_id = (uint64_t)record->path_id;
    for (unsigned index = 0U; index < 8U; ++index) {
        identity[COMMON_SHA256_DIGEST_SIZE + index] =
            (uint8_t)(path_id >> (index * 8U));
    }
    return unity_asset_guid_derive(
        UNITY_ASSET_GUID_DOMAIN_SHADER, identity, sizeof(identity), output);
}

static int test_graphics_residue_path_ownership(void) {
    int status = 0;
    ShaderBatchResult result;
    shader_batch_result_init(&result);
    char* staged_shader = NULL;
    char* staged_meta = NULL;

    result.records = (ShaderBatchRecordResult*)calloc(
        1U, sizeof(*result.records));
    CHECK(result.records != NULL);
    result.record_count = 1U;
    staged_shader = duplicate_string("failed.shader");
    staged_meta = duplicate_string("failed.shader.meta");
    CHECK(staged_shader != NULL && staged_meta != NULL);
    result.records[0].shader_publication_residue = true;
    result.records[0].meta_publication_residue = true;
    shader_batch_retain_graphics_publication_residue_paths(
        &result.records[0], &staged_shader, &staged_meta);
    CHECK(staged_shader == NULL && staged_meta == NULL);
    CHECK(strcmp(result.records[0].shader_publication_residue_path,
                 "failed.shader") == 0);
    CHECK(strcmp(result.records[0].meta_publication_residue_path,
                 "failed.shader.meta") == 0);
    shader_batch_result_dispose(&result);

    result.records = (ShaderBatchRecordResult*)calloc(
        1U, sizeof(*result.records));
    CHECK(result.records != NULL);
    result.record_count = 1U;
    staged_shader = duplicate_string("not-residue.shader");
    staged_meta = duplicate_string("not-residue.shader.meta");
    CHECK(staged_shader != NULL && staged_meta != NULL);
    shader_batch_retain_graphics_publication_residue_paths(
        &result.records[0], &staged_shader, &staged_meta);
    CHECK(staged_shader != NULL && staged_meta != NULL);
    CHECK(result.records[0].shader_publication_residue_path == NULL);
    CHECK(result.records[0].meta_publication_residue_path == NULL);

cleanup:
    free(staged_meta);
    free(staged_shader);
    shader_batch_result_dispose(&result);
    return status;
}

static int test_flat_graphics_filename_collisions(void) {
    int status = 0;
    ShaderCatalogRecord records[3];
    memset(records, 0, sizeof(records));
    records[0].class_id = 48;
    records[0].name = (char*)"Hidden/Effects/Analog Scan";
    records[0].path_id = 101;
    memset(records[0].serialized_digest, 0x11,
           sizeof(records[0].serialized_digest));
    memset(records[0].serialized_digest_hex, '1',
           COMMON_SHA256_DIGEST_SIZE * 2U);
    records[1].class_id = 48;
    records[1].name = (char*)"hidden/effects/analog scan";
    records[1].path_id = 202;
    memset(records[1].serialized_digest, 0x22,
           sizeof(records[1].serialized_digest));
    memset(records[1].serialized_digest_hex, '2',
           COMMON_SHA256_DIGEST_SIZE * 2U);
    records[2].class_id = 48;
    records[2].name = (char*)"Example/Layered Surface Cutout";
    records[2].path_id = 303;
    memset(records[2].serialized_digest, 0x33,
           sizeof(records[2].serialized_digest));
    memset(records[2].serialized_digest_hex, '3',
           COMMON_SHA256_DIGEST_SIZE * 2U);

    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    catalog.records = records;
    catalog.record_count = 3U;
    char filename[384];
    CHECK(shader_batch_flat_graphics_filename(
        &catalog, 0U, records[0].name, filename, sizeof(filename)));
    CHECK(strstr(filename,
                 "Hidden_Effects_Analog Scan__1111111111111111") == filename);
    CHECK(strcmp(filename + strlen(filename) - sizeof("_101.shader") + 1U,
                 "_101.shader") == 0);
    CHECK(shader_batch_flat_graphics_filename(
        &catalog, 1U, records[1].name, filename, sizeof(filename)));
    CHECK(strstr(filename,
                 "hidden_effects_analog scan__2222222222222222") == filename);
    CHECK(shader_batch_flat_graphics_filename(
        &catalog, 2U, records[2].name, filename, sizeof(filename)));
    CHECK(strcmp(filename, "Example_Layered Surface Cutout.shader") == 0);
    CHECK(!shader_batch_flat_graphics_filename(
        &catalog, 2U, "not-the-decoded-name", filename, sizeof(filename)));

    /* Relocated aliases with the same exact serialized identity intentionally
     * share the clean flat filename and identical bytes. */
    records[1] = records[0];
    catalog.record_count = 2U;
    CHECK(shader_batch_flat_graphics_filename(
        &catalog, 0U, records[0].name, filename, sizeof(filename)));
    CHECK(strcmp(filename, "Hidden_Effects_Analog Scan.shader") == 0);

cleanup:
    return status;
}

typedef struct {
    size_t record_index;
    size_t calls;
    int failure_mode;
    size_t source_size;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
} CandidateSelector;

static bool select_test_candidate(void *opaque, const ShaderBatchCandidateInput *input,
                                  StringBuilder *source, ShaderLabCandidateDiagnostic *diagnostic) {
    CandidateSelector *selector = opaque;
    ++selector->calls;
    if (input->catalog_record_index != selector->record_index || !input->object || !input->archive ||
        !input->source_directory || !input->source_basename || !input->source_path || source->len ||
        strcmp(input->source_basename, "Hidden_SeparableBlur.shader") != 0 ||
        !strstr(input->source_path, input->source_directory)) return false;
    if (selector->failure_mode == 1) {
        sb_append(source, "partial failed selection");
        return false;
    }
    if (selector->failure_mode == 2) return true; /* Empty success must fail closed. */
    const ShaderBlobArchive *archive = input->archive;
    if (!shaderlab_emit_candidate_with_diagnostic(
            &input->object->shader, archive->entries, archive->entry_count, archive->segments,
            archive->segment_lengths, archive->segment_count, source, diagnostic)) return false;
    /* No final newline: the batch must publish precisely the selected bytes. */
    sb_append(source, "// selected candidate terminator");
    selector->source_size = source->len;
    common_sha256(source->buf, source->len, selector->source_digest);
    return sb_ok(source);
}

int main(void) {
    int status = 0;
    TypeTreeSchemaRegistry registry;
    ShaderCatalog catalog;
    ShaderBatchResult batch;
    bool* selected = NULL;
    char* shader_path = NULL;
    char* meta_path = NULL;
    char* digest_directory = NULL;
    char* flat_shader_path = NULL;
    char* flat_meta_path = NULL;
    char output_root[512] = {0};
    char flat_output_root[512] = {0};
    bool root_created = false;
    bool flat_root_created = false;
    bool shader_file_present = false;
    CommonFileBytes actual_meta = {0};
    StringBuilder expected_meta;

    typetree_schema_registry_init(&registry);
    shader_catalog_init(&catalog);
    shader_batch_result_init(&batch);
    sb_init(&expected_meta);

    CHECK(test_graphics_residue_path_ownership() == 0);
    CHECK(test_flat_graphics_filename_collisions() == 0);

    int written = snprintf(
        output_root, sizeof(output_root), "dxbc_shader_meta_%lu",
        TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(output_root));
    (void)TEST_RMDIR(output_root);

    CHECK(typetree_schema_registry_import_file_replace(
              &registry, DXBC_TEST_PLAYER_SCHEMA_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    const char* inputs[] = {DXBC_TEST_SHADER_BUNDLE};
    ShaderCatalogOptions catalog_options;
    shader_catalog_options_default(&catalog_options);
    catalog_options.schema_registry = &registry;
    CHECK(shader_catalog_build(
              inputs, 1U, &catalog_options, &catalog) == SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));

    selected = (bool*)calloc(catalog.record_count, sizeof(*selected));
    CHECK(selected != NULL);
    size_t chosen = SIZE_MAX;
    for (size_t index = 0U; index < catalog.record_count; ++index) {
        if (catalog.records[index].class_id == 48 &&
            catalog.records[index].name &&
            strcmp(catalog.records[index].name,
                   "Hidden/SeparableBlur") == 0) {
            chosen = index;
            break;
        }
    }
    CHECK(chosen != SIZE_MAX);
    selected[chosen] = true;

    written = snprintf(
        flat_output_root, sizeof(flat_output_root),
        "dxbc_shader_flat_%lu", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(flat_output_root));
    (void)TEST_RMDIR(flat_output_root);
    ShaderBatchOptions flat_options;
    shader_batch_options_default(&flat_options);
    CHECK(!flat_options.flat_graphics_output);
    flat_options.emit_shader_meta = true;
    flat_options.flat_graphics_output = true;
    CHECK(shader_batch_extract_ex(
              &catalog, selected, &registry, flat_output_root, &flat_options,
              &batch) == SHADER_BATCH_OK);
    flat_root_created = true;
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.records[chosen].output_path != NULL);
    CHECK(batch.records[chosen].output_meta_path != NULL);
    char expected_flat_path[768];
    written = snprintf(
        expected_flat_path, sizeof(expected_flat_path), "%s%c%s",
        flat_output_root, TEST_PATH_SEPARATOR,
        "Hidden_SeparableBlur.shader");
    CHECK(written > 0 && (size_t)written < sizeof(expected_flat_path));
    CHECK(strcmp(batch.records[chosen].output_path, expected_flat_path) == 0);
    flat_shader_path = duplicate_string(batch.records[chosen].output_path);
    flat_meta_path = duplicate_string(batch.records[chosen].output_meta_path);
    CHECK(flat_shader_path != NULL && flat_meta_path != NULL);
    shader_batch_result_dispose(&batch);
    CHECK(remove(flat_meta_path) == 0);
    CHECK(remove(flat_shader_path) == 0);
    CHECK(TEST_RMDIR(flat_output_root) == 0);
    flat_root_created = false;
    free(flat_meta_path);
    flat_meta_path = NULL;
    free(flat_shader_path);
    flat_shader_path = NULL;

    CandidateSelector selector = {.record_index = chosen};
    flat_options.select_candidate = select_test_candidate;
    flat_options.candidate_context = &selector;
    for (int failure = 1; failure <= 2; ++failure) {
        selector.failure_mode = failure;
        CHECK(shader_batch_extract_ex(&catalog, selected, &registry, flat_output_root,
                                      &flat_options, &batch) == SHADER_BATCH_OK);
        CHECK(batch.stats.failed == 1 && !shader_batch_is_complete(&batch));
        CHECK(batch.records[chosen].failure == SHADER_BATCH_FAILURE_CANDIDATE_SELECTION);
        CHECK(!batch.records[chosen].shader_publish_attempted &&
              !batch.records[chosen].publication_authorized && !batch.records[chosen].output_path);
        shader_batch_result_dispose(&batch);
    }
    selector.failure_mode = 0;
    CHECK(shader_batch_extract_ex(&catalog, selected, &registry, flat_output_root,
                                  &flat_options, &batch) == SHADER_BATCH_OK);
    flat_root_created = true;
    CHECK(selector.calls == 3 && shader_batch_is_complete(&batch));
    CHECK(batch.records[chosen].published_shader_size == selector.source_size);
    CHECK(memcmp(batch.records[chosen].published_shader_digest, selector.source_digest,
                 sizeof(selector.source_digest)) == 0);
    flat_shader_path = duplicate_string(batch.records[chosen].output_path);
    flat_meta_path = duplicate_string(batch.records[chosen].output_meta_path);
    CHECK(flat_shader_path && flat_meta_path);
    CHECK(common_file_read_regular(flat_shader_path, SIZE_MAX, &actual_meta) == COMMON_FILE_OK);
    CHECK(actual_meta.size == selector.source_size && actual_meta.data[actual_meta.size - 1] != '\n');
    common_file_bytes_dispose(&actual_meta);
    shader_batch_result_dispose(&batch);
    CHECK(remove(flat_meta_path) == 0 && remove(flat_shader_path) == 0);
    CHECK(TEST_RMDIR(flat_output_root) == 0);
    flat_root_created = false;
    free(flat_meta_path);
    free(flat_shader_path);
    flat_meta_path = NULL;
    flat_shader_path = NULL;

    ShaderBatchOptions batch_options;
    shader_batch_options_default(&batch_options);
    batch_options.emit_shader_meta = true;
    CHECK(shader_batch_extract_ex(
              &catalog, selected, &registry, output_root, &batch_options,
              &batch) == SHADER_BATCH_OK);
    root_created = true;
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted == 1U);
    CHECK(batch.stats.unchanged == 0U);
    CHECK(batch.stats.failed == 0U);
    CHECK(batch.records[chosen].status == SHADER_BATCH_EMITTED);
    CHECK(batch.records[chosen].failure == SHADER_BATCH_FAILURE_NONE);
    CHECK(batch.records[chosen].output_path != NULL);
    CHECK(batch.records[chosen].output_meta_path != NULL);
    CHECK(batch.records[chosen].has_asset_meta);
    CHECK(batch.records[chosen].meta_publish_status ==
          COMMON_OUTPUT_PUBLISH_EMITTED);
    CHECK(batch.records[chosen].shader_preflight_attempted);
    CHECK(batch.records[chosen].meta_preflight_attempted);
    CHECK(batch.records[chosen].shader_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_MISSING);
    CHECK(batch.records[chosen].meta_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_MISSING);
    CHECK(batch.records[chosen].shader_publish_attempted);

    /* Aggregate counters cannot certify a contradictory record ledger. */
    ShaderBatchRecordStatus saved_record_status =
        batch.records[chosen].status;
    batch.records[chosen].status = SHADER_BATCH_FAILED;
    CHECK(!shader_batch_is_complete(&batch));
    batch.records[chosen].status = saved_record_status;
    CHECK(shader_batch_is_complete(&batch));
    batch.records[chosen].shader_publish_attempted = false;
    CHECK(!shader_batch_is_complete(&batch));
    batch.records[chosen].shader_publish_attempted = true;
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.records[chosen].meta_publish_attempted);
    CHECK(batch.records[chosen].shader_publish_status ==
          COMMON_OUTPUT_PUBLISH_EMITTED);
    CHECK(!batch.records[chosen].publication_residue);
    CHECK(batch.records[chosen].publication_authorized);
    CHECK(!batch.records[chosen].source_identity_close_deferred);
    shader_file_present = true;
    CHECK(unity_asset_guid_is_valid(batch.records[chosen].asset_guid));
    CHECK(path_has_meta_suffix(batch.records[chosen].output_path,
                               batch.records[chosen].output_meta_path));

    char expected_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    CHECK(derive_expected_shader_guid(
        &catalog.records[chosen], expected_guid));
    CHECK(strcmp(batch.records[chosen].asset_guid, expected_guid) == 0);
    CHECK(unity_shader_importer_meta_emit(expected_guid, &expected_meta) ==
          UNITY_MATERIAL_YAML_OK);
    CHECK(common_file_read_regular(
              batch.records[chosen].output_meta_path, SIZE_MAX,
              &actual_meta) == COMMON_FILE_OK);
    CHECK(actual_meta.size == expected_meta.len);
    CHECK(memcmp(actual_meta.data, expected_meta.buf, actual_meta.size) == 0);

    shader_path = duplicate_string(batch.records[chosen].output_path);
    meta_path = duplicate_string(batch.records[chosen].output_meta_path);
    digest_directory = duplicate_parent_path(shader_path);
    CHECK(shader_path != NULL && meta_path != NULL &&
          digest_directory != NULL);

    /* Re-emitting the exact pair is deterministic and never overwrites. */
    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract_ex(
              &catalog, selected, &registry, output_root, &batch_options,
              &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted == 0U);
    CHECK(batch.stats.unchanged == 1U);
    CHECK(batch.stats.failed == 0U);
    CHECK(batch.records[chosen].status == SHADER_BATCH_UNCHANGED);
    CHECK(batch.records[chosen].meta_publish_status ==
          COMMON_OUTPUT_PUBLISH_UNCHANGED);
    CHECK(batch.records[chosen].shader_preflight_attempted);
    CHECK(batch.records[chosen].meta_preflight_attempted);
    CHECK(batch.records[chosen].shader_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_UNCHANGED);
    CHECK(batch.records[chosen].meta_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_UNCHANGED);
    CHECK(batch.records[chosen].shader_publish_attempted);
    CHECK(batch.records[chosen].meta_publish_attempted);
    CHECK(batch.records[chosen].shader_publish_status ==
          COMMON_OUTPUT_PUBLISH_UNCHANGED);
    CHECK(!batch.records[chosen].publication_residue);
    CHECK(batch.records[chosen].publication_authorized);
    CHECK(strcmp(batch.records[chosen].asset_guid, expected_guid) == 0);
    CHECK(strcmp(batch.records[chosen].output_path, shader_path) == 0);
    CHECK(strcmp(batch.records[chosen].output_meta_path, meta_path) == 0);

    /* A differing existing .meta is an explicit collision. It is not
     * accepted as an asset-meta success, and the conflicting bytes remain
     * untouched. */
    CHECK(remove(meta_path) == 0);
    static const uint8_t conflicting_meta[] = "conflicting meta\n";
    CHECK(common_file_write_new_atomic(
              meta_path, conflicting_meta,
              sizeof(conflicting_meta) - 1U) == COMMON_FILE_OK);
    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract_ex(
              &catalog, selected, &registry, output_root, &batch_options,
              &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted == 0U);
    CHECK(batch.stats.unchanged == 0U);
    CHECK(batch.stats.failed == 1U);
    CHECK(batch.records[chosen].status == SHADER_BATCH_FAILED);
    CHECK(batch.records[chosen].failure ==
          SHADER_BATCH_FAILURE_OUTPUT_COLLISION);
    CHECK(batch.records[chosen].meta_publish_status ==
          COMMON_OUTPUT_PUBLISH_COLLISION);
    CHECK(batch.records[chosen].shader_preflight_attempted);
    CHECK(batch.records[chosen].meta_preflight_attempted);
    CHECK(batch.records[chosen].shader_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_UNCHANGED);
    CHECK(batch.records[chosen].meta_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_COLLISION);
    CHECK(!batch.records[chosen].shader_publish_attempted);
    CHECK(!batch.records[chosen].meta_publish_attempted);
    CHECK(!batch.records[chosen].publication_residue);
    CHECK(!batch.records[chosen].publication_authorized);
    CHECK(!batch.records[chosen].has_asset_meta);
    CHECK(batch.records[chosen].output_path == NULL);
    CHECK(batch.records[chosen].output_meta_path == NULL);
    common_file_bytes_dispose(&actual_meta);
    CHECK(common_file_read_regular(meta_path, SIZE_MAX, &actual_meta) ==
          COMMON_FILE_OK);
    CHECK(actual_meta.size == sizeof(conflicting_meta) - 1U);
    CHECK(memcmp(actual_meta.data, conflicting_meta,
                 sizeof(conflicting_meta) - 1U) == 0);

    /* Artifact-set preflight runs before the first publication.  A
     * conflicting sibling .meta must not leave a newly-created .shader
     * behind when that shader did not exist at entry. */
    CHECK(remove(shader_path) == 0);
    shader_file_present = false;
    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract_ex(
              &catalog, selected, &registry, output_root, &batch_options,
              &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.failed == 1U);
    CHECK(batch.records[chosen].status == SHADER_BATCH_FAILED);
    CHECK(batch.records[chosen].failure ==
          SHADER_BATCH_FAILURE_OUTPUT_COLLISION);
    CHECK(batch.records[chosen].meta_publish_status ==
          COMMON_OUTPUT_PUBLISH_COLLISION);
    CHECK(batch.records[chosen].shader_preflight_attempted);
    CHECK(batch.records[chosen].meta_preflight_attempted);
    CHECK(batch.records[chosen].shader_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_MISSING);
    CHECK(batch.records[chosen].meta_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_COLLISION);
    CHECK(!batch.records[chosen].shader_publish_attempted);
    CHECK(!batch.records[chosen].meta_publish_attempted);
    CHECK(!batch.records[chosen].publication_residue);
    CHECK(!batch.records[chosen].publication_authorized);
    common_file_bytes_dispose(&actual_meta);
    CHECK(common_file_read_regular(shader_path, SIZE_MAX, &actual_meta) ==
          COMMON_FILE_NOT_FOUND);
    CHECK(common_file_read_regular(meta_path, SIZE_MAX, &actual_meta) ==
          COMMON_FILE_OK);
    CHECK(actual_meta.size == sizeof(conflicting_meta) - 1U);
    CHECK(memcmp(actual_meta.data, conflicting_meta,
                 sizeof(conflicting_meta) - 1U) == 0);

cleanup:
    common_file_bytes_dispose(&actual_meta);
    sb_free(&expected_meta);
    shader_batch_result_dispose(&batch);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    free(selected);
    if (flat_meta_path) (void)remove(flat_meta_path);
    if (flat_shader_path) (void)remove(flat_shader_path);
    if (flat_root_created && TEST_RMDIR(flat_output_root) != 0) status = 1;
    if (meta_path && remove(meta_path) != 0) status = 1;
    if (shader_file_present && shader_path && remove(shader_path) != 0) {
        status = 1;
    }
    if (digest_directory && TEST_RMDIR(digest_directory) != 0) status = 1;
    if (root_created && TEST_RMDIR(output_root) != 0) status = 1;
    free(digest_directory);
    free(flat_meta_path);
    free(flat_shader_path);
    free(meta_path);
    free(shader_path);
    if (status == 0) puts("Shader batch meta unit tests passed.");
    return status;
}
