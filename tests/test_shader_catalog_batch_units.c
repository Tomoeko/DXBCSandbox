#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "app/shader_batch.h"
#include "app/shader_catalog.h"
#include "app/shader_catalog_object.h"
#include "common/file_io.h"
#include "io/typetree_schema_registry.h"
#include "test_support/file_mutation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#include <direct.h>
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        status = 1; \
        goto cleanup; \
    } \
} while (0)

typedef struct {
    uint8_t bytes[256];
    size_t size;
} SerializedFixture;

static bool fixture_append(SerializedFixture* fixture, const void* bytes,
                           size_t size) {
    if (!fixture || (!bytes && size != 0U) ||
        size > sizeof(fixture->bytes) - fixture->size) {
        return false;
    }
    memcpy(fixture->bytes + fixture->size, bytes, size);
    fixture->size += size;
    return true;
}

static bool fixture_u8(SerializedFixture* fixture, uint8_t value) {
    return fixture_append(fixture, &value, sizeof(value));
}

static bool fixture_le16(SerializedFixture* fixture, uint16_t value) {
    uint8_t bytes[] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return fixture_append(fixture, bytes, sizeof(bytes));
}

static bool fixture_le32(SerializedFixture* fixture, uint32_t value) {
    uint8_t bytes[] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    return fixture_append(fixture, bytes, sizeof(bytes));
}

static bool fixture_le64(SerializedFixture* fixture, uint64_t value) {
    return fixture_le32(fixture, (uint32_t)value) &&
           fixture_le32(fixture, (uint32_t)(value >> 32));
}

static bool fixture_align(SerializedFixture* fixture, size_t alignment) {
    while ((fixture->size % alignment) != 0U) {
        if (!fixture_u8(fixture, 0U)) return false;
    }
    return true;
}

static void fixture_patch_be32(SerializedFixture* fixture, size_t offset,
                               uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) {
        fixture->bytes[offset + i] =
            (uint8_t)(value >> (24U - i * 8U));
    }
}

static void fixture_patch_be64(SerializedFixture* fixture, size_t offset,
                               uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i) {
        fixture->bytes[offset + i] =
            (uint8_t)(value >> (56U - i * 8U));
    }
}

static bool build_compute_shader_serialized_fixture(
    SerializedFixture* fixture) {
    static const uint8_t zeros[16] = {0};
    static const uint8_t compute_type_hash[16] = {
        0xabU, 0xd9U, 0x13U, 0x5bU, 0x8cU, 0xe8U, 0x3dU, 0x04U,
        0x3fU, 0xefU, 0x4eU, 0x9eU, 0xc7U, 0xf5U, 0x33U, 0x66U,
    };
    static const char unity_version[] = "2021.3.35f1";
    static const char compute_name[] = "ComputeFixture";
    if (!fixture) return false;
    memset(fixture, 0, sizeof(*fixture));

    /* SerializedFile v22 extended header. */
    if (!fixture_append(fixture, zeros, 8U) ||
        !fixture_u8(fixture, 0U) || !fixture_u8(fixture, 0U) ||
        !fixture_u8(fixture, 0U) || !fixture_u8(fixture, 22U) ||
        !fixture_append(fixture, zeros, 8U) ||
        !fixture_append(fixture, zeros, 16U) ||
        !fixture_append(fixture, zeros, 12U)) {
        return false;
    }
    const size_t metadata_start = fixture->size;
    if (!fixture_append(fixture, unity_version, sizeof(unity_version)) ||
        !fixture_le32(fixture, 19U) || /* target platform */
        !fixture_u8(fixture, 0U) ||    /* TypeTree disabled */
        !fixture_le32(fixture, 1U) ||  /* one type */
        !fixture_le32(fixture, 72U) || /* ComputeShader ClassID */
        !fixture_u8(fixture, 0U) ||    /* not stripped */
        !fixture_le16(fixture, UINT16_MAX) ||
        !fixture_append(fixture, compute_type_hash,
                        sizeof(compute_type_hash)) || /* exact type hash */
        !fixture_le32(fixture, 1U) ||            /* one object */
        !fixture_align(fixture, 4U) ||
        !fixture_le64(fixture, 101U) || /* path ID */
        !fixture_le64(fixture, 0U) ||   /* object data offset */
        !fixture_le32(fixture, 24U) ||  /* bounded name/count prefix */
        !fixture_le32(fixture, 0U) ||   /* type table index */
        !fixture_le32(fixture, 0U) ||   /* scripts */
        !fixture_le32(fixture, 0U) ||   /* externals */
        !fixture_le32(fixture, 0U) ||   /* reference types */
        !fixture_u8(fixture, 0U)) {     /* user information */
        return false;
    }
    const size_t metadata_size = fixture->size - metadata_start;
    if (!fixture_align(fixture, 16U) || metadata_size > UINT32_MAX) {
        return false;
    }
    const size_t data_offset = fixture->size;
    if (!fixture_le32(fixture, (uint32_t)(sizeof(compute_name) - 1U)) ||
        !fixture_append(fixture, compute_name, sizeof(compute_name) - 1U) ||
        !fixture_align(fixture, 4U) ||
        !fixture_le32(fixture, 0U)) { /* declared platform variants */
        return false;
    }
    fixture_patch_be32(fixture, 20U, (uint32_t)metadata_size);
    fixture_patch_be64(fixture, 24U, fixture->size);
    fixture_patch_be64(fixture, 32U, data_offset);
    return true;
}

static bool build_catalog_source_fixture(
    SerializedFixture* fixture, bool type_tree_enabled,
    int32_t class_id, bool include_object) {
    static const uint8_t zeros[16] = {0};
    static const char unity_version[] = "2021.3.35f1";
    static const uint8_t strings[] = "int\0Base\0";
    if (!fixture || class_id < 0) return false;
    memset(fixture, 0, sizeof(*fixture));
    if (!fixture_append(fixture, zeros, 8U) ||
        !fixture_u8(fixture, 0U) || !fixture_u8(fixture, 0U) ||
        !fixture_u8(fixture, 0U) || !fixture_u8(fixture, 22U) ||
        !fixture_append(fixture, zeros, 8U) ||
        !fixture_append(fixture, zeros, 16U) ||
        !fixture_append(fixture, zeros, 12U)) {
        return false;
    }
    const size_t metadata_start = fixture->size;
    if (!fixture_append(fixture, unity_version, sizeof(unity_version)) ||
        !fixture_le32(fixture, 19U) ||
        !fixture_u8(fixture, type_tree_enabled ? 1U : 0U) ||
        !fixture_le32(fixture, 1U) ||
        !fixture_le32(fixture, (uint32_t)class_id) ||
        !fixture_u8(fixture, 0U) ||
        !fixture_le16(fixture, UINT16_MAX)) {
        return false;
    }
    for (uint8_t index = 0U; index < 16U; ++index) {
        if (!fixture_u8(fixture, (uint8_t)(0x40U + index))) return false;
    }
    if (type_tree_enabled &&
        (!fixture_le32(fixture, 1U) ||
         !fixture_le32(fixture, (uint32_t)sizeof(strings)) ||
         !fixture_le16(fixture, 1U) || !fixture_u8(fixture, 0U) ||
         !fixture_u8(fixture, 0U) || !fixture_le32(fixture, 0U) ||
         !fixture_le32(fixture, 4U) || !fixture_le32(fixture, 4U) ||
         !fixture_le32(fixture, 0U) || !fixture_le32(fixture, 0x4000U) ||
         !fixture_le64(fixture, 0U) ||
         !fixture_append(fixture, strings, sizeof(strings)) ||
         !fixture_le32(fixture, 0U))) {
        return false;
    }
    if (!fixture_le32(fixture, include_object ? 1U : 0U) ||
        (include_object &&
         (!fixture_align(fixture, 4U) ||
          !fixture_le64(fixture, 101U) || !fixture_le64(fixture, 0U) ||
          !fixture_le32(fixture, 4U) || !fixture_le32(fixture, 0U))) ||
        !fixture_le32(fixture, 0U) || !fixture_le32(fixture, 0U) ||
        !fixture_le32(fixture, 0U) || !fixture_u8(fixture, 0U)) {
        return false;
    }
    const size_t metadata_size = fixture->size - metadata_start;
    if (!fixture_align(fixture, 16U) || metadata_size > UINT32_MAX) {
        return false;
    }
    const size_t data_offset = fixture->size;
    if (include_object && !fixture_le32(fixture, 0U)) return false;
    fixture_patch_be32(fixture, 20U, (uint32_t)metadata_size);
    fixture_patch_be64(fixture, 24U, fixture->size);
    fixture_patch_be64(fixture, 32U, data_offset);
    return true;
}

static bool write_fixture_new(const char* path,
                              const SerializedFixture* fixture) {
    (void)remove(path);
    return common_file_write_new_atomic(path, fixture->bytes,
                                        fixture->size) == COMMON_FILE_OK;
}

static bool current_directory_utf8(char* output, size_t capacity);

static bool verify_catalog_source_provenance(void) {
    bool ok = false;
    char embedded_path[160] = {0};
    char resolved_path[160] = {0};
    char empty_path[160] = {0};
    char failure_path[160] = {0};
    char recursive_root[160] = {0};
    char recursive_child[192] = {0};
    char recursive_path[224] = {0};
#ifndef _WIN32
    char literal_backslash_path[192] = {0};
#endif
    TypeTreeSchemaRegistry registry;
    TypeTreeSchemaRegistry empty_registry;
    ShaderCatalog catalog;
    typetree_schema_registry_init(&registry);
    typetree_schema_registry_init(&empty_registry);
    shader_catalog_init(&catalog);

#define SOURCE_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "SOURCE_CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        goto source_cleanup; \
    } \
} while (0)

    int written = snprintf(embedded_path, sizeof(embedded_path),
                           "dxbc_source_embedded_%lu.assets",
                           TEST_PROCESS_ID());
    SOURCE_CHECK(written > 0 && (size_t)written < sizeof(embedded_path));
    written = snprintf(resolved_path, sizeof(resolved_path),
                       "dxbc_source_resolved_%lu.assets",
                       TEST_PROCESS_ID());
    SOURCE_CHECK(written > 0 && (size_t)written < sizeof(resolved_path));
    written = snprintf(empty_path, sizeof(empty_path),
                       "dxbc_source_empty_%lu.assets",
                       TEST_PROCESS_ID());
    SOURCE_CHECK(written > 0 && (size_t)written < sizeof(empty_path));
    written = snprintf(failure_path, sizeof(failure_path),
                       "dxbc_source_failure_%lu.assets",
                       TEST_PROCESS_ID());
    SOURCE_CHECK(written > 0 && (size_t)written < sizeof(failure_path));

    SerializedFixture embedded;
    SerializedFixture resolved;
    SerializedFixture empty;
    SOURCE_CHECK(build_catalog_source_fixture(&embedded, true, 48, true));
    SOURCE_CHECK(build_catalog_source_fixture(&resolved, false, 48, true));
    SOURCE_CHECK(build_catalog_source_fixture(&empty, false, 21, false));
    SOURCE_CHECK(write_fixture_new(embedded_path, &embedded));
    SOURCE_CHECK(write_fixture_new(resolved_path, &resolved));
    SOURCE_CHECK(write_fixture_new(empty_path, &empty));
    SOURCE_CHECK(write_fixture_new(failure_path, &resolved));

    SerializedFile evidence;
    SOURCE_CHECK(serialized_file_open_with_schema_registry_ex(
        &evidence, embedded.bytes, embedded.size, &registry,
        TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT));
    serialized_file_close(&evidence);
    SOURCE_CHECK(typetree_schema_registry_count(&registry) == 1U);

    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = &registry;
    const char* embedded_input[] = {embedded_path};
    SOURCE_CHECK(shader_catalog_build(
        embedded_input, 1U, &options, &catalog) == SHADER_CATALOG_OK);
    SOURCE_CHECK(catalog.source_count == 1U);
    SOURCE_CHECK(catalog.stats.visited_serialized_sources == 1U);
    const ShaderCatalogSource* source = &catalog.sources[0];
    SOURCE_CHECK(source->metadata_status ==
                 SHADER_CATALOG_SOURCE_METADATA_OK);
    SOURCE_CHECK(source->serialized_file_version == 22U);
    SOURCE_CHECK(strcmp(source->unity_version, "2021.3.35f1") == 0);
    SOURCE_CHECK(source->target_platform == 19U);
    SOURCE_CHECK(source->type_tree_enabled);
    SOURCE_CHECK(source->object_count == 1U);
    SOURCE_CHECK(source->class_id_48_count == 1U);
    SOURCE_CHECK(source->class_id_72_count == 0U);
    SOURCE_CHECK(source->class_id_48_schema_required);
    SOURCE_CHECK(source->class_id_48_schema_status == TYPETREE_SCHEMA_OK);
    SOURCE_CHECK(source->class_id_48_schema_key_count == 1U);
    SOURCE_CHECK(!source->class_id_48_schema_keys[0].lookup_required);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].provenance ==
                 SHADER_CATALOG_SCHEMA_PROVENANCE_EMBEDDED_TYPETREE);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].profile ==
                 TYPETREE_SCHEMA_PROFILE_UNKNOWN);

    const char* resolved_input[] = {resolved_path};
    SOURCE_CHECK(shader_catalog_build(
        resolved_input, 1U, &options, &catalog) == SHADER_CATALOG_OK);
    source = &catalog.sources[0];
    SOURCE_CHECK(!source->type_tree_enabled);
    SOURCE_CHECK(source->class_id_48_schema_status == TYPETREE_SCHEMA_OK);
    SOURCE_CHECK(source->class_id_48_schema_key_count == 1U);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].lookup_required);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].lookup_status ==
                 TYPETREE_SCHEMA_OK);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].provenance ==
                 SHADER_CATALOG_SCHEMA_PROVENANCE_EXACT_REGISTRY);

    const char* empty_input[] = {empty_path};
    SOURCE_CHECK(shader_catalog_build(
        empty_input, 1U, &options, &catalog) == SHADER_CATALOG_OK);
    SOURCE_CHECK(catalog.record_count == 0U && catalog.source_count == 1U);
    source = &catalog.sources[0];
    SOURCE_CHECK(source->object_count == 0U);
    SOURCE_CHECK(source->class_id_48_count == 0U);
    SOURCE_CHECK(source->class_id_72_count == 0U);
    SOURCE_CHECK(!source->class_id_48_schema_required);
    SOURCE_CHECK(source->class_id_48_schema_key_count == 0U);

#ifndef _WIN32
    /* A backslash is part of a POSIX basename, not a directory separator.
     * The default scope root must therefore remain the actual cwd. */
    written = snprintf(literal_backslash_path,
                       sizeof(literal_backslash_path),
                       "dxbc_source_literal_%lu\\name.assets",
                       TEST_PROCESS_ID());
    SOURCE_CHECK(written > 0 &&
                 (size_t)written < sizeof(literal_backslash_path));
    SOURCE_CHECK(write_fixture_new(literal_backslash_path, &empty));
    const char* literal_backslash_input[] = {literal_backslash_path};
    SOURCE_CHECK(shader_catalog_build(
        literal_backslash_input, 1U, &options, &catalog) ==
        SHADER_CATALOG_OK);
    SOURCE_CHECK(catalog.source_count == 1U);
    char expected_scope_root[4096];
    SOURCE_CHECK(getcwd(expected_scope_root,
                        sizeof(expected_scope_root)) != NULL);
    SOURCE_CHECK(strcmp(catalog.sources[0].scope_root,
                        expected_scope_root) == 0);
#endif

    /* UnityInput exposes an absolute outer path.  A relative recursive input
     * must therefore be made absolute before discovery, or its lexical scope
     * silently collapses from the caller's root to this file's parent. */
    written = snprintf(recursive_root, sizeof(recursive_root),
                       "dxbc_source_scope_%lu", TEST_PROCESS_ID());
    SOURCE_CHECK(written > 0 &&
                 (size_t)written < sizeof(recursive_root));
    written = snprintf(recursive_child, sizeof(recursive_child),
                       "%s%cchild", recursive_root,
                       TEST_PATH_SEPARATOR);
    SOURCE_CHECK(written > 0 &&
                 (size_t)written < sizeof(recursive_child));
    written = snprintf(recursive_path, sizeof(recursive_path),
                       "%s%csource.assets", recursive_child,
                       TEST_PATH_SEPARATOR);
    SOURCE_CHECK(written > 0 &&
                 (size_t)written < sizeof(recursive_path));
    SOURCE_CHECK(TEST_MKDIR(recursive_root) == 0);
    SOURCE_CHECK(TEST_MKDIR(recursive_child) == 0);
    SOURCE_CHECK(write_fixture_new(recursive_path, &empty));
    const char* recursive_input[] = {recursive_root};
    SOURCE_CHECK(shader_catalog_build(
        recursive_input, 1U, &options, &catalog) == SHADER_CATALOG_OK);
    SOURCE_CHECK(catalog.source_count == 1U);
    char recursive_cwd[4096];
    SOURCE_CHECK(current_directory_utf8(
        recursive_cwd, sizeof(recursive_cwd)));
    char expected_recursive_root[4300];
    written = snprintf(expected_recursive_root,
                       sizeof(expected_recursive_root), "%s%c%s",
                       recursive_cwd, TEST_PATH_SEPARATOR,
                       recursive_root);
    SOURCE_CHECK(written > 0 &&
                 (size_t)written < sizeof(expected_recursive_root));
    SOURCE_CHECK(strcmp(catalog.sources[0].scope_root,
                        expected_recursive_root) == 0);

    options.schema_registry = &empty_registry;
    const char* failure_input[] = {failure_path};
    SOURCE_CHECK(shader_catalog_build(
        failure_input, 1U, &options, &catalog) == SHADER_CATALOG_OK);
    SOURCE_CHECK(catalog.record_count == 1U && catalog.source_count == 1U);
    source = &catalog.sources[0];
    SOURCE_CHECK(source->class_id_48_schema_status ==
                 TYPETREE_SCHEMA_NOT_FOUND);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].lookup_required);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].lookup_status ==
                 TYPETREE_SCHEMA_NOT_FOUND);
    SOURCE_CHECK(source->class_id_48_schema_keys[0].provenance ==
                 SHADER_CATALOG_SCHEMA_PROVENANCE_NONE);
    SOURCE_CHECK(catalog.records[0].status ==
                 SHADER_CATALOG_RECORD_SCHEMA_ERROR);
    ok = true;

source_cleanup:
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&empty_registry);
    typetree_schema_registry_dispose(&registry);
    if (embedded_path[0]) (void)remove(embedded_path);
    if (resolved_path[0]) (void)remove(resolved_path);
    if (empty_path[0]) (void)remove(empty_path);
    if (failure_path[0]) (void)remove(failure_path);
    if (recursive_path[0]) (void)remove(recursive_path);
    if (recursive_child[0]) (void)TEST_RMDIR(recursive_child);
    if (recursive_root[0]) (void)TEST_RMDIR(recursive_root);
#ifndef _WIN32
    if (literal_backslash_path[0])
        (void)remove(literal_backslash_path);
#endif
#undef SOURCE_CHECK
    return ok;
}

static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    char* copy = (char*)malloc(size + 1U);
    if (copy) memcpy(copy, value, size + 1U);
    return copy;
}

static char* parent_path(char* path) {
    if (!path) return NULL;
    char* last = NULL;
    for (char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') last = cursor;
    }
    if (!last) return NULL;
    *last = '\0';
    return path;
}

static char* sibling_path(const char* path, const char* name) {
    char* directory = duplicate_string(path);
    if (!directory || !parent_path(directory)) {
        free(directory);
        return NULL;
    }
    const size_t directory_size = strlen(directory);
    const size_t name_size = strlen(name);
    if (directory_size > SIZE_MAX - name_size - 2U) {
        free(directory);
        return NULL;
    }
    char* sibling = (char*)malloc(directory_size + name_size + 2U);
    if (sibling) {
        memcpy(sibling, directory, directory_size);
        sibling[directory_size] = TEST_PATH_SEPARATOR;
        memcpy(sibling + directory_size + 1U, name, name_size + 1U);
    }
    free(directory);
    return sibling;
}

static bool current_directory_utf8(char* output, size_t capacity) {
    if (!output || capacity == 0U) return false;
#ifdef _WIN32
    DWORD required = GetCurrentDirectoryW(0U, NULL);
    if (required == 0U || required > 32768U) return false;
    wchar_t* wide = (wchar_t*)malloc((size_t)required * sizeof(*wide));
    if (!wide) return false;
    DWORD size = GetCurrentDirectoryW(required, wide);
    if (size == 0U || size >= required) {
        free(wide);
        return false;
    }
    char* utf8 = common_windows_wide_to_utf8(wide);
    free(wide);
    if (!utf8) return false;
    size_t length = strlen(utf8);
    bool fits = length < capacity;
    if (fits) memcpy(output, utf8, length + 1U);
    free(utf8);
    return fits;
#else
    return getcwd(output, capacity) != NULL;
#endif
}

int main(void) {
    int status = 0;
    TypeTreeSchemaRegistry registry;
    ShaderCatalog catalog;
    ShaderBatchResult batch;
    ShaderObject decoded;
    ShaderCatalogObjectReport decoded_report;
    bool* selected = NULL;
    char* emitted_path = NULL;
    char* compute_emitted_path = NULL;
    char* compute_serialized_path = NULL;
    char compute_input[160] = {0};
    bool compute_input_created = false;
    char compute_stale_output_root[1024] = {0};
    char compute_stale_digest_directory[1200] = {0};
    char compute_stale_manifest_path[1480] = {0};
    char compute_stale_binary_path[1480] = {0};
    char stale_input[160] = {0};
    bool stale_input_created = false;
    CommonFileBytes bundle_copy = {0};
    CommonFileBytes escaped_compute_artifact = {0};
#ifndef _WIN32
    char* symlink_emitted_path = NULL;
    char symlink_real_root[1024] = {0};
    char symlink_alias_root[1024] = {0};
    bool symlink_real_created = false;
    bool symlink_alias_created = false;
#endif
    char working_directory[768] = {0};
    char output_root[1024];

    typetree_schema_registry_init(&registry);
    shader_catalog_init(&catalog);
    shader_object_init(&decoded);
    shader_batch_result_init(&batch);
    CHECK(current_directory_utf8(
              working_directory, sizeof(working_directory)));
    int written = snprintf(output_root, sizeof(output_root),
                           "%s%cdxbc_catalog_batch_%lu",
                           working_directory, TEST_PATH_SEPARATOR,
                           TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(output_root));
    CHECK(verify_catalog_source_provenance());

    CHECK(typetree_schema_registry_import_file_replace(
              &registry, DXBC_TEST_PLAYER_SCHEMA_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    const char* inputs[] = {DXBC_TEST_SHADER_BUNDLE};
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = &registry;
    CHECK(shader_catalog_build(inputs, 1U, &options, &catalog) ==
          SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.issue_count == 0U);
    CHECK(catalog.record_count == 290U);
    CHECK(catalog.stats.shader_objects == 290U);
    CHECK(catalog.stats.compute_shader_objects == 0U);
    CHECK(catalog.stats.decoded_shaders == 290U);
    CHECK(catalog.stats.ready_shaders == 290U);
    CHECK(catalog.stats.failed_shaders == 0U);
    CHECK(catalog.stats.serialized_sources == 1U);
    CHECK(catalog.stats.visited_serialized_sources == 1U);
    CHECK(catalog.source_count == 1U);
    CHECK(catalog.stats.unity_container_files == 1U);
    for (size_t i = 0U; i < catalog.record_count; ++i) {
        CHECK(catalog.records[i].class_id == 48);
        CHECK(strncmp(catalog.records[i].occurrence_id, "o:", 2U) == 0);
        CHECK(strncmp(catalog.records[i].content_id, "s:", 2U) == 0);
        for (size_t j = 0U; j < i; ++j) {
            CHECK(strcmp(catalog.records[i].occurrence_id,
                         catalog.records[j].occurrence_id) != 0);
        }
    }

    selected = (bool*)calloc(catalog.record_count, sizeof(*selected));
    CHECK(selected != NULL);
    size_t chosen = SIZE_MAX;
    for (size_t i = 0U; i < catalog.record_count; ++i) {
        if (catalog.records[i].name &&
            strcmp(catalog.records[i].name, "Hidden/SeparableBlur") == 0) {
            chosen = i;
            break;
        }
    }
    CHECK(chosen != SIZE_MAX);
    CHECK(catalog.records[chosen].status == SHADER_CATALOG_RECORD_READY);
    char chosen_content_id[sizeof(catalog.records[chosen].content_id)];
    memcpy(chosen_content_id, catalog.records[chosen].content_id,
           sizeof(chosen_content_id));
    char chosen_occurrence_id[sizeof(catalog.records[chosen].occurrence_id)];
    memcpy(chosen_occurrence_id, catalog.records[chosen].occurrence_id,
           sizeof(chosen_occurrence_id));
    selected[chosen] = true;

    CHECK(shader_batch_extract(&catalog, selected, &registry, output_root,
                               &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted == 1U);
    CHECK(batch.stats.unchanged == 0U);
    CHECK(batch.records[chosen].status == SHADER_BATCH_EMITTED);
    CHECK(batch.records[chosen].output_path != NULL);
    emitted_path = duplicate_string(batch.records[chosen].output_path);
    CHECK(emitted_path != NULL);
    shader_batch_result_dispose(&batch);

    CHECK(shader_batch_extract(&catalog, selected, &registry, output_root,
                               &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted == 0U);
    CHECK(batch.stats.unchanged == 1U);
    CHECK(batch.records[chosen].status == SHADER_BATCH_UNCHANGED);

    /* Opting into retained identity changes neither catalog digests nor
     * emitted bytes. The same artifact must be recognized as unchanged. */
    options.retain_source_snapshots = true;
    CHECK(shader_catalog_build(inputs, 1U, &options, &catalog) ==
          SHADER_CATALOG_OK);
    CHECK(catalog.retained_source_snapshot_count == 1U);
    chosen = SIZE_MAX;
    for (size_t i = 0U; i < catalog.record_count; ++i) {
        if (catalog.records[i].name &&
            strcmp(catalog.records[i].name,
                   "Hidden/SeparableBlur") == 0) {
            chosen = i;
            break;
        }
    }
    CHECK(chosen != SIZE_MAX);
    CHECK(strcmp(catalog.records[chosen].content_id,
                 chosen_content_id) == 0);
    CHECK(strcmp(catalog.records[chosen].occurrence_id,
                 chosen_occurrence_id) == 0);
    memset(selected, 0, catalog.record_count * sizeof(*selected));
    selected[chosen] = true;
    ShaderCatalog missing_records = catalog;
    missing_records.records = NULL;
    CHECK(shader_batch_extract(
              &missing_records, selected, &registry, output_root,
              &batch) == SHADER_BATCH_INVALID_ARGUMENT);
    ShaderCatalog missing_snapshots = catalog;
    missing_snapshots.retained_source_snapshots = NULL;
    CHECK(shader_batch_extract(
              &missing_snapshots, selected, &registry, output_root,
              &batch) == SHADER_BATCH_INVALID_ARGUMENT);
    shader_batch_result_dispose(&batch);
    ShaderBatchOptions deferred_batch_options;
    shader_batch_options_default(&deferred_batch_options);
    deferred_batch_options.defer_source_snapshot_close = true;
    CHECK(shader_batch_extract_ex(
              &catalog, selected, &registry, output_root,
              &deferred_batch_options, &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch));
    CHECK(batch.records[chosen].status == SHADER_BATCH_UNCHANGED);
    CHECK(batch.records[chosen].source_identity_close_deferred);
    CHECK(!batch.records[chosen].publication_authorized);
    CHECK(unity_input_snapshot_is_open(
              &catalog.retained_source_snapshots[0]));

    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract(&catalog, selected, &registry, output_root,
                               &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted == 0U);
    CHECK(batch.stats.unchanged == 1U);
    CHECK(batch.records[chosen].status == SHADER_BATCH_UNCHANGED);
    CHECK(!unity_input_snapshot_is_open(
              &catalog.retained_source_snapshots[0]));

#ifndef _WIN32
    /* Existing directory symlinks are valid path ancestors.  This exercises
     * the same case as macOS /tmp -> /private/tmp using an absolute path so a
     * regression to lstat()-and-reject is caught on every POSIX host. */
    written = snprintf(symlink_real_root, sizeof(symlink_real_root),
                       "%s/dxbc_catalog_real_%lu", working_directory,
                       TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(symlink_real_root));
    written = snprintf(symlink_alias_root, sizeof(symlink_alias_root),
                       "%s/dxbc_catalog_link_%lu", working_directory,
                       TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(symlink_alias_root));
    (void)remove(symlink_alias_root);
    CHECK(mkdir(symlink_real_root, 0755) == 0);
    symlink_real_created = true;
    CHECK(symlink(symlink_real_root, symlink_alias_root) == 0);
    symlink_alias_created = true;

    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract(&catalog, selected, &registry,
                               symlink_alias_root, &batch) ==
          SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.records[chosen].status == SHADER_BATCH_EMITTED);
    CHECK(batch.records[chosen].output_path != NULL);
    symlink_emitted_path =
        duplicate_string(batch.records[chosen].output_path);
    CHECK(symlink_emitted_path != NULL);
#endif

    /* Extraction may reuse the catalog SHA only while it owns the exact
     * already-hashed file identity. Replace the pathname with byte-identical
     * content after cataloging: content IDs stay identical, but the batch
     * must revoke even an already-produced/unchanged result when close finds
     * the stale source object. */
    written = snprintf(stale_input, sizeof(stale_input),
                       "dxbc_stale_catalog_%lu.bundle",
                       TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(stale_input));
    (void)remove(stale_input);
    CHECK(common_file_read_regular(
              DXBC_TEST_SHADER_BUNDLE, SIZE_MAX, &bundle_copy) ==
          COMMON_FILE_OK);
    CHECK(common_file_write_new_atomic(
              stale_input, bundle_copy.data, bundle_copy.size) ==
          COMMON_FILE_OK);
    stale_input_created = true;
    const char* stale_inputs[] = {stale_input};
    CHECK(shader_catalog_build(stale_inputs, 1U, &options, &catalog) ==
          SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.retained_source_snapshot_count == 1U);
    size_t stale_chosen = SIZE_MAX;
    for (size_t i = 0U; i < catalog.record_count; ++i) {
        if (catalog.records[i].name &&
            strcmp(catalog.records[i].name,
                   "Hidden/SeparableBlur") == 0) {
            stale_chosen = i;
            break;
        }
    }
    CHECK(stale_chosen != SIZE_MAX);
    CHECK(strcmp(catalog.records[stale_chosen].content_id,
                 chosen_content_id) == 0);
    memset(selected, 0, catalog.record_count * sizeof(*selected));
    selected[stale_chosen] = true;
    CHECK(shader_catalog_decode_object(&catalog, &catalog.records[stale_chosen],
              &registry, &decoded, &decoded_report) == SHADER_CATALOG_OBJECT_OK);
    const void* decoded_schema_nodes = decoded.schema.nodes;
    CHECK(test_replace_regular_file(
        stale_input, bundle_copy.data, bundle_copy.size));
    CHECK(shader_catalog_decode_object(&catalog, &catalog.records[stale_chosen],
              &registry, &decoded, &decoded_report) == SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE);
    CHECK(decoded.schema.nodes == decoded_schema_nodes);
    shader_object_dispose(&decoded);
    stale_input_created = true;
    common_file_bytes_dispose(&bundle_copy);

    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract(&catalog, selected, &registry, output_root,
                               &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted == 0U);
    CHECK(batch.stats.unchanged == 0U);
    CHECK(batch.stats.failed == 1U);
    CHECK(batch.records[stale_chosen].status == SHADER_BATCH_FAILED);
    CHECK(batch.records[stale_chosen].failure ==
          SHADER_BATCH_FAILURE_SOURCE_REOPEN);
    CHECK(batch.records[stale_chosen].input_status ==
          UNITY_INPUT_FILE_ERROR);
    CHECK(batch.records[stale_chosen].output_path == NULL);
    CHECK(!unity_input_snapshot_is_open(
              &catalog.retained_source_snapshots[0]));
    CHECK(remove(stale_input) == 0);
    stale_input_created = false;
    options.retain_source_snapshots = false;

    char missing[160];
    written = snprintf(missing, sizeof(missing),
                       "dxbc_missing_catalog_input_%lu",
                       TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(missing));
    (void)remove(missing);
    const char* mixed_inputs[] = {missing, DXBC_TEST_SHADER_BUNDLE};
    CHECK(shader_catalog_build(mixed_inputs, 2U, &options, &catalog) ==
          SHADER_CATALOG_OK);
    CHECK(!shader_catalog_is_complete(&catalog));
    CHECK(catalog.record_count == 290U);
    CHECK(catalog.issue_count == 1U);
    CHECK(catalog.issues[0].code == SHADER_CATALOG_ISSUE_DISCOVERY);
    CHECK(catalog.issues[0].discovery_status ==
          COMMON_PATH_DISCOVERY_NOT_FOUND);

    /* ClassID 72 is a first-class exact binary/manifest occurrence. The
     * complete code-pinned 2021.3 layout decoder is independent of the
     * ClassID 48 registry plane; source emission remains a separate,
     * fail-closed authority. */
    SerializedFixture compute_fixture;
    CHECK(build_compute_shader_serialized_fixture(&compute_fixture));
    written = snprintf(compute_input, sizeof(compute_input),
                       "dxbc_compute_catalog_%lu.assets",
                       TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(compute_input));
    (void)remove(compute_input);
    FILE* compute_file = fopen(compute_input, "wb");
    CHECK(compute_file != NULL);
    CHECK(fwrite(compute_fixture.bytes, 1U, compute_fixture.size,
                 compute_file) == compute_fixture.size);
    CHECK(fclose(compute_file) == 0);
    compute_input_created = true;
    const char* compute_inputs[] = {compute_input};
    options.retain_source_snapshots = true;
    CHECK(shader_catalog_build(compute_inputs, 1U, &options, &catalog) ==
          SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.issue_count == 0U);
    CHECK(catalog.record_count == 1U);
    CHECK(catalog.stats.shader_objects == 1U);
    CHECK(catalog.stats.compute_shader_objects == 1U);
    CHECK(catalog.stats.named_compute_shaders == 1U);
    CHECK(catalog.stats.decoded_shaders == 1U);
    CHECK(catalog.stats.ready_shaders == 1U);
    CHECK(catalog.stats.failed_shaders == 0U);
    CHECK(catalog.records[0].class_id == 72);
    CHECK(catalog.records[0].path_id == 101);
    CHECK(catalog.records[0].name != NULL);
    CHECK(strcmp(catalog.records[0].name, "ComputeFixture") == 0);
    CHECK(catalog.records[0].compute_inventory_status ==
          COMPUTE_SHADER_INVENTORY_OK);
    CHECK(catalog.records[0].compute_platform_variants_declared == 0U);
    CHECK(catalog.records[0].status == SHADER_CATALOG_RECORD_READY);
    CHECK(catalog.records[0].schema_status == TYPETREE_SCHEMA_NOT_FOUND);
    CHECK(catalog.records[0].object_status == SHADER_OBJECT_NOT_DECODED);
    CHECK(catalog.records[0].compute_object_status ==
          COMPUTE_SHADER_OBJECT_OK);
    CHECK(catalog.records[0].compute_source_authority_status ==
          COMPUTE_SHADER_SOURCE_AUTHORITY_NO_KERNELS);
    CHECK(strcmp(shader_catalog_record_status_name(catalog.records[0].status),
                 "ready") == 0);

    memset(selected, 0, 290U * sizeof(*selected));
    selected[0] = true;
    shader_batch_result_dispose(&batch);
    ShaderBatchOptions deferred_compute_options;
    shader_batch_options_default(&deferred_compute_options);
    deferred_compute_options.defer_source_snapshot_close = true;
    CHECK(shader_batch_extract_ex(
              &catalog, selected, &registry, output_root,
              &deferred_compute_options, &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch));
    CHECK(batch.records[0].status == SHADER_BATCH_EMITTED);
    CHECK(batch.records[0].compute_artifact_publication_count == 2U);
    CHECK(batch.records[0].compute_preflight_attempted);
    CHECK(batch.records[0].compute_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_MISSING);
    CHECK(batch.records[0].compute_publish_attempted);
    CHECK(batch.records[0].compute_publish_status ==
          COMMON_OUTPUT_PUBLISH_EMITTED);
    CHECK(!batch.records[0].compute_publication_residue);
    CHECK(!batch.records[0].publication_residue);
    CHECK(batch.records[0].source_identity_close_deferred);
    CHECK(!batch.records[0].publication_authorized);
    CHECK(unity_input_snapshot_is_open(
              &catalog.retained_source_snapshots[0]));
    for (size_t artifact_index = 0U;
         artifact_index <
             batch.records[0].compute_artifact_publication_count;
         ++artifact_index) {
        const ShaderBatchComputeArtifactPublication* publication =
            &batch.records[0]
                 .compute_artifact_publications[artifact_index];
        CHECK(publication->preflight_attempted);
        CHECK(publication->preflight_status ==
              COMMON_OUTPUT_PREFLIGHT_MISSING);
        CHECK(publication->publish_attempted);
        CHECK(publication->publish_status ==
              COMMON_OUTPUT_PUBLISH_EMITTED);
        CHECK(!publication->publication_residue);
    }

    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract(&catalog, selected, &registry, output_root,
                               &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch));
    CHECK(batch.stats.selected == 1U);
    CHECK(batch.stats.emitted + batch.stats.unchanged == 1U);
    CHECK(batch.stats.failed == 0U);
    CHECK(batch.records[0].status == SHADER_BATCH_EMITTED ||
          batch.records[0].status == SHADER_BATCH_UNCHANGED);
    CHECK(batch.records[0].failure == SHADER_BATCH_FAILURE_NONE);
    CHECK(batch.records[0].compute_object_status == COMPUTE_SHADER_OBJECT_OK);
    CHECK(batch.records[0].compute_artifact_status ==
          COMPUTE_SHADER_ARTIFACT_OK);
    CHECK(batch.records[0].compute_source_authority_status ==
          COMPUTE_SHADER_SOURCE_AUTHORITY_NO_KERNELS);
    CHECK(batch.records[0].compute_artifact_publication_count == 2U);
    CHECK(batch.records[0].compute_preflight_attempted);
    CHECK(batch.records[0].compute_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_UNCHANGED);
    CHECK(batch.records[0].compute_publish_attempted);
    CHECK(batch.records[0].compute_publish_status ==
          COMMON_OUTPUT_PUBLISH_UNCHANGED);
    CHECK(!batch.records[0].compute_publication_residue);
    CHECK(!batch.records[0].publication_residue);
    CHECK(!batch.records[0].source_identity_close_deferred);
    CHECK(batch.records[0].publication_authorized);
    CHECK(!unity_input_snapshot_is_open(
              &catalog.retained_source_snapshots[0]));
    CHECK(batch.records[0].output_path != NULL);
    compute_emitted_path = duplicate_string(batch.records[0].output_path);
    CHECK(compute_emitted_path != NULL);
    compute_serialized_path = sibling_path(
        compute_emitted_path,
        "compute_ComputeFixture__101.serialized-object.bin");
    CHECK(compute_serialized_path != NULL);

    bool saved_compute_member_attempted =
        batch.records[0].compute_artifact_publications[0].publish_attempted;
    batch.records[0].compute_artifact_publications[0].publish_attempted =
        false;
    CHECK(!shader_batch_is_complete(&batch));
    batch.records[0].compute_artifact_publications[0].publish_attempted =
        saved_compute_member_attempted;
    CHECK(shader_batch_is_complete(&batch));

    /* Every package member is preflighted before the first publish. A known
     * manifest collision therefore cannot rewrite or partially republish any
     * binary, and the per-member attempted ledger remains exact. */
    CHECK(remove(compute_emitted_path) == 0);
    static const uint8_t compute_manifest_conflict[] =
        "conflicting compute manifest\n";
    CHECK(common_file_write_new_atomic(
              compute_emitted_path, compute_manifest_conflict,
              sizeof(compute_manifest_conflict) - 1U) == COMMON_FILE_OK);
    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract(&catalog, selected, &registry, output_root,
                               &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch));
    CHECK(batch.records[0].status == SHADER_BATCH_FAILED);
    CHECK(batch.records[0].failure ==
          SHADER_BATCH_FAILURE_OUTPUT_COLLISION);
    CHECK(batch.records[0].compute_artifact_publication_count == 2U);
    CHECK(batch.records[0].compute_preflight_attempted);
    CHECK(batch.records[0].compute_preflight_status ==
          COMMON_OUTPUT_PREFLIGHT_COLLISION);
    CHECK(!batch.records[0].compute_publish_attempted);
    CHECK(!batch.records[0].compute_publication_residue);
    CHECK(!batch.records[0].publication_authorized);
    bool saw_manifest_collision = false;
    bool saw_unchanged_binary = false;
    for (size_t artifact_index = 0U;
         artifact_index <
             batch.records[0].compute_artifact_publication_count;
         ++artifact_index) {
        const ShaderBatchComputeArtifactPublication* publication =
            &batch.records[0]
                 .compute_artifact_publications[artifact_index];
        CHECK(publication->preflight_attempted);
        CHECK(!publication->publish_attempted);
        CHECK(!publication->publication_residue);
        if (publication->is_manifest) {
            CHECK(publication->preflight_status ==
                  COMMON_OUTPUT_PREFLIGHT_COLLISION);
            saw_manifest_collision = true;
        } else {
            CHECK(publication->preflight_status ==
                  COMMON_OUTPUT_PREFLIGHT_UNCHANGED);
            saw_unchanged_binary = true;
        }
    }
    CHECK(saw_manifest_collision && saw_unchanged_binary);

    /* A pathname identity change after cataloging must fail before any
     * compute package preflight/publication, whether the retained snapshot
     * detects it at visit or close validation. */
    CHECK(shader_catalog_build(compute_inputs, 1U, &options, &catalog) ==
          SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&catalog));
    CHECK(catalog.retained_source_snapshot_count == 1U);
    CHECK(test_replace_regular_file(
        compute_input, compute_fixture.bytes, compute_fixture.size));
    written = snprintf(
        compute_stale_output_root, sizeof(compute_stale_output_root),
        "%s%cdxbc_compute_stale_batch_%lu", working_directory,
        TEST_PATH_SEPARATOR, TEST_PROCESS_ID());
    CHECK(written > 0 &&
          (size_t)written < sizeof(compute_stale_output_root));
    written = snprintf(
        compute_stale_digest_directory,
        sizeof(compute_stale_digest_directory), "%s%c%s",
        compute_stale_output_root, TEST_PATH_SEPARATOR,
        catalog.records[0].serialized_digest_hex);
    CHECK(written > 0 &&
          (size_t)written < sizeof(compute_stale_digest_directory));
    written = snprintf(
        compute_stale_manifest_path, sizeof(compute_stale_manifest_path),
        "%s%ccompute_ComputeFixture__101.compute.json",
        compute_stale_digest_directory, TEST_PATH_SEPARATOR);
    CHECK(written > 0 &&
          (size_t)written < sizeof(compute_stale_manifest_path));
    written = snprintf(
        compute_stale_binary_path, sizeof(compute_stale_binary_path),
        "%s%ccompute_ComputeFixture__101.serialized-object.bin",
        compute_stale_digest_directory, TEST_PATH_SEPARATOR);
    CHECK(written > 0 &&
          (size_t)written < sizeof(compute_stale_binary_path));

    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract(
              &catalog, selected, &registry, compute_stale_output_root,
              &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch));
    CHECK(batch.records[0].status == SHADER_BATCH_FAILED);
    CHECK(batch.records[0].failure ==
          SHADER_BATCH_FAILURE_SOURCE_REOPEN);
    CHECK(batch.records[0].input_status == UNITY_INPUT_FILE_ERROR);
    CHECK(batch.records[0].compute_artifact_publication_count == 0U);
    CHECK(!batch.records[0].compute_preflight_attempted);
    CHECK(!batch.records[0].compute_publish_attempted);
    CHECK(!batch.records[0].publication_residue);
    CHECK(!batch.records[0].publication_authorized);
    CHECK(common_file_read_regular(
              compute_stale_manifest_path, SIZE_MAX,
              &escaped_compute_artifact) == COMMON_FILE_NOT_FOUND);
    CHECK(common_file_read_regular(
              compute_stale_binary_path, SIZE_MAX,
              &escaped_compute_artifact) == COMMON_FILE_NOT_FOUND);

cleanup:
    shader_object_dispose(&decoded);
    shader_batch_result_dispose(&batch);
    common_file_bytes_dispose(&bundle_copy);
    common_file_bytes_dispose(&escaped_compute_artifact);
    free(selected);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    if (stale_input_created && remove(stale_input) != 0) status = 1;
    if (compute_input_created && remove(compute_input) != 0) status = 1;
    if (compute_stale_manifest_path[0])
        (void)remove(compute_stale_manifest_path);
    if (compute_stale_binary_path[0])
        (void)remove(compute_stale_binary_path);
    if (compute_stale_digest_directory[0])
        (void)TEST_RMDIR(compute_stale_digest_directory);
    if (compute_stale_output_root[0])
        (void)TEST_RMDIR(compute_stale_output_root);
    if (compute_serialized_path && remove(compute_serialized_path) != 0) {
        status = 1;
    }
    free(compute_serialized_path);
    if (compute_emitted_path) {
        char* directory = duplicate_string(compute_emitted_path);
        if (remove(compute_emitted_path) != 0) status = 1;
        if (!directory || !parent_path(directory) ||
            TEST_RMDIR(directory) != 0) {
            status = 1;
        }
        free(directory);
    }
    free(compute_emitted_path);
    if (emitted_path) {
        char* directory = duplicate_string(emitted_path);
        if (remove(emitted_path) != 0) status = 1;
        if (!directory || !parent_path(directory) ||
            TEST_RMDIR(directory) != 0 || TEST_RMDIR(output_root) != 0) {
            status = 1;
        }
        free(directory);
    }
    free(emitted_path);
#ifndef _WIN32
    if (symlink_emitted_path) {
        char* directory = duplicate_string(symlink_emitted_path);
        if (remove(symlink_emitted_path) != 0) status = 1;
        if (!directory || !parent_path(directory) ||
            TEST_RMDIR(directory) != 0) {
            status = 1;
        }
        free(directory);
    }
    free(symlink_emitted_path);
    if (symlink_alias_created && remove(symlink_alias_root) != 0) status = 1;
    if (symlink_real_created && TEST_RMDIR(symlink_real_root) != 0) status = 1;
#endif
    if (status == 0) puts("Shader catalog/batch unit tests passed.");
    return status;
}
