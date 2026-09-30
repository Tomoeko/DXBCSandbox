// SPDX-License-Identifier: GPL-3.0-only

#include "app/shader_catalog_dependencies.h"
#include "app/shader_catalog_dependencies_internal.h"
#include "common/file_io.h"
#include "io/serialized_shader_profile.h"
#include "test_support/file_mutation.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define TEST_PID() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define TEST_PID() ((unsigned long)getpid())
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

enum {
    FIXTURE_SIZE = 272,
    FIXTURE_DATA_OFFSET = 144,
    FIXTURE_OBJECT_SIZE = 128,
    FIXTURE_CUSTOM_EDITOR = 216,
    FIXTURE_FALLBACK = 220,
    FIXTURE_PARSED_DEPENDENCIES = 224,
    FIXTURE_PIPELINE_EDITORS = 228,
    FIXTURE_ROOT_DEPENDENCIES = 260,
    FIXTURE_TEXTURES = 264
};

typedef struct {
    size_t calls;
    bool accept;
    ShaderCatalogDependenciesSummary last;
} Observer;

typedef struct {
    size_t calls;
    const char *path;
    const uint8_t *bytes;
    size_t size;
    bool replaced;
} ReplacementObserver;

static bool observe(void *opaque, const ShaderCatalogDependenciesSummary *summary) {
    Observer *observer = opaque;
    ++observer->calls;
    observer->last = *summary;
    return observer->accept;
}

static bool replace_during_observation(void *opaque,
                                      const ShaderCatalogDependenciesSummary *summary) {
    ReplacementObserver *observer = opaque;
    ++observer->calls;
    observer->replaced = summary != NULL &&
                         test_replace_regular_file(observer->path, observer->bytes, observer->size);
    return observer->replaced;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index)
        if (bytes[index]) return true;
    return false;
}

static void store_le32(uint8_t *destination, uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index)
        destination[index] = (uint8_t)(value >> (index * 8U));
}

static void store_be64(uint8_t *destination, uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index)
        destination[7U - index] = (uint8_t)(value >> (index * 8U));
}

static bool load_catalog(const char *path, const TypeTreeSchemaRegistry *registry,
                         bool retain, ShaderCatalog *catalog) {
    shader_catalog_init(catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = registry;
    options.retain_source_snapshots = retain;
    CHECK(shader_catalog_build(&path, 1U, &options, catalog) == SHADER_CATALOG_OK);
    CHECK(catalog->record_count == 1U && catalog->records[0].class_id == 48);
    return true;
}

static ShaderCatalogDependenciesInput catalog_input(const ShaderCatalog *catalog,
                                                    const TypeTreeSchemaRegistry *registry) {
    ShaderCatalogDependenciesInput input = {0};
    input.catalog = catalog;
    input.record = catalog->records;
    input.registry = registry;
    return input;
}

static bool temporary_path(char *path, size_t capacity, unsigned index) {
    int written = snprintf(path, capacity, "shader_dependencies_%lu_%u.assets", TEST_PID(), index);
    return written > 0 && (size_t)written < capacity;
}

/* Every mutation starts from the public synthetic fixture. All inserted
 * elements are four-byte aligned. Metadata and data_offset stay fixed; only
 * the v22 total-size and object-size fields change. */
static size_t insert_fixture(const uint8_t *fixture, size_t at, const uint8_t *element,
                             size_t element_size, uint8_t destination[304]) {
    if (at < FIXTURE_DATA_OFFSET || at > FIXTURE_SIZE || element_size > 32U ||
        (element_size & 3U) != 0U)
        return 0U;
    memcpy(destination, fixture, at);
    memcpy(destination + at, element, element_size);
    memcpy(destination + at + element_size, fixture + at, FIXTURE_SIZE - at);
    size_t size = FIXTURE_SIZE + element_size;
    store_be64(destination + 24U, size);
    store_le32(destination + 112U, (uint32_t)(FIXTURE_OBJECT_SIZE + element_size));
    return size;
}

static bool expect_file_status(const uint8_t *bytes, size_t size, unsigned index,
                               const TypeTreeSchemaRegistry *registry,
                               ShaderCatalogDependenciesStatus expected) {
    char path[160];
    CHECK(temporary_path(path, sizeof(path), index));
    CHECK(common_file_write_new_atomic(path, bytes, size) == COMMON_FILE_OK);
    ShaderCatalog catalog;
    CHECK(load_catalog(path, registry, true, &catalog));
    ShaderCatalogDependenciesInput input = catalog_input(&catalog, registry);
    ShaderCatalogDependencies *owned = NULL;
    ShaderCatalogDependenciesDiagnostic diagnostic;
    CHECK(shader_catalog_dependencies_capture(&input, &owned, &diagnostic) == expected);
    CHECK(diagnostic.status == expected && owned == NULL);
    shader_catalog_dispose(&catalog);
    CHECK(remove(path) == 0);
    return true;
}

static bool graph_cases(const uint8_t *fixture, const TypeTreeSchemaRegistry *registry) {
    uint8_t changed[304], element[32] = {0};
    unsigned index = 10U;
    for (unsigned nonnull = 0U; nonnull < 2U; ++nonnull) {
        memset(element, 0, sizeof(element));
        /* Null and self PPtrs both leave a nonempty dependency table. */
        element[4] = nonnull ? 7U : 0U;
        size_t size = insert_fixture(fixture, FIXTURE_ROOT_DEPENDENCIES + 4U,
                                     element, 12U, changed);
        CHECK(size == 284U);
        store_le32(changed + FIXTURE_ROOT_DEPENDENCIES, 1U);
        CHECK(expect_file_status(changed, size, index++, registry,
                                  SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE));
    }
    for (unsigned nonnull = 0U; nonnull < 2U; ++nonnull) {
        memset(element, 0, sizeof(element));
        /* A real texture key and a null/self PPtr<Texture>. There is no
         * eight-byte alignment between fileID and pathID in this schema. */
        store_le32(element, 1U);
        element[4] = 'T';
        element[12] = nonnull ? 7U : 0U;
        size_t size = insert_fixture(fixture, FIXTURE_TEXTURES + 4U, element, 20U, changed);
        CHECK(size == 292U);
        store_le32(changed + FIXTURE_TEXTURES, 1U);
        CHECK(expect_file_status(changed, size, index++, registry,
                                  SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE));
    }
    const size_t strings[] = {FIXTURE_CUSTOM_EDITOR, FIXTURE_FALLBACK};
    for (size_t field = 0U; field < sizeof(strings) / sizeof(strings[0]); ++field) {
        memset(element, 0, sizeof(element));
        element[0] = 'X';
        size_t size = insert_fixture(fixture, strings[field] + 4U, element, 4U, changed);
        CHECK(size == 276U);
        store_le32(changed + strings[field], 1U);
        CHECK(expect_file_status(changed, size, index++, registry,
                                  SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE));
    }
    const size_t pair_arrays[] = {FIXTURE_PARSED_DEPENDENCIES, FIXTURE_PIPELINE_EDITORS};
    for (size_t field = 0U; field < sizeof(pair_arrays) / sizeof(pair_arrays[0]); ++field) {
        for (unsigned named = 0U; named < 2U; ++named) {
            memset(element, 0, sizeof(element));
            size_t element_size = named ? 16U : 8U;
            if (named) {
                store_le32(element, 1U);
                element[4] = 'A';
                store_le32(element + 8U, 1U);
                element[12] = 'B';
            }
            size_t size = insert_fixture(fixture, pair_arrays[field] + 4U,
                                         element, element_size, changed);
            CHECK(size == FIXTURE_SIZE + element_size);
            store_le32(changed + pair_arrays[field], 1U);
            CHECK(expect_file_status(changed, size, index++, registry,
                                      SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE));
        }
    }
    /* Impossible element count exercises decoding, rather than being
     * reported as an ordinary unsupported dependency graph. */
    memcpy(changed, fixture, FIXTURE_SIZE);
    store_le32(changed + FIXTURE_ROOT_DEPENDENCIES, UINT32_MAX);
    CHECK(expect_file_status(changed, FIXTURE_SIZE, index++, registry,
                              SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE));
    /* Declared nonempty PPtr with only the original eight-byte tail. */
    memcpy(changed, fixture, FIXTURE_SIZE);
    store_le32(changed + FIXTURE_ROOT_DEPENDENCIES, 1U);
    CHECK(expect_file_status(changed, FIXTURE_SIZE, index, registry,
                              SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE));
    return true;
}

static bool unknown_schema_case(const uint8_t *fixture, const TypeTreeSchemaRegistry *registry) {
    TypeTreeSchemaKey key;
    const TypeTreeType *view = NULL;
    bool found = false;
    for (size_t index = 0U; index < typetree_schema_registry_count(registry); ++index) {
        CHECK(typetree_schema_registry_entry_view(registry, index, &key, &view) ==
              TYPETREE_SCHEMA_OK);
        if (key.class_id == 48 && key.unity_version_size == 11U &&
            memcmp(key.unity_version, "2021.3.35f1", 11U) == 0 &&
            memcmp(key.type_hash, fixture + 76U, 16U) == 0) {
            found = true;
            break;
        }
    }
    CHECK(found && view != NULL);
    TypeTreeType schema = {0};
    CHECK(typetree_schema_registry_lookup(registry, &key, &schema) == TYPETREE_SCHEMA_OK);
    /* A controlled exact registry entry can be structurally decodable while
     * its external type identity has no reviewed semantic shape profile. */
    key.type_hash[0] ^= 1U;
    schema.type_hash[0] ^= 1U;
    TypeTreeSchemaRegistry unknown;
    typetree_schema_registry_init(&unknown);
    CHECK(typetree_schema_registry_learn(&unknown, &key, &schema,
                                         TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_validate_known_profile(key.unity_version, key.unity_version_size,
                                                 key.class_id, key.type_hash, &schema) ==
          TYPETREE_SCHEMA_PROFILE_UNKNOWN);
    uint8_t changed[FIXTURE_SIZE];
    memcpy(changed, fixture, sizeof(changed));
    changed[76] ^= 1U;
    CHECK(expect_file_status(changed, sizeof(changed), 40U, &unknown,
                              SHADER_CATALOG_DEPENDENCIES_SCHEMA_UNAVAILABLE));
    typetree_free_type(&schema);
    typetree_schema_registry_dispose(&unknown);
    return true;
}

static bool owned_replay_cases(ShaderCatalogDependencies *owned,
                               const ShaderCatalogDependenciesInput *input) {
    CHECK(shader_catalog_dependencies_replay(input, owned));
    owned->sealed = false;
    bool accepted = shader_catalog_dependencies_replay(input, owned);
    owned->sealed = true;
    CHECK(!accepted);

    owned->root_digest[0] ^= 1U;
    accepted = shader_catalog_dependencies_replay(input, owned);
    owned->root_digest[0] ^= 1U;
    CHECK(!accepted);
    owned->summary.release.release_digest[0] ^= 1U;
    accepted = shader_catalog_dependencies_replay(input, owned);
    owned->summary.release.release_digest[0] ^= 1U;
    CHECK(!accepted);

    TypeTreeValue *name = NULL;
    for (int index = 0; index < owned->object.root.struct_val.count; ++index) {
        TypeTreeValue *child = &owned->object.root.struct_val.members[index];
        if (child->name && strcmp(child->name, "m_Name") == 0) name = child;
    }
    CHECK(name && name->type == VAL_TYPE_STRING && name->string_length > 0U);
    name->string_val[0] ^= 1;
    accepted = shader_catalog_dependencies_replay(input, owned);
    name->string_val[0] ^= 1;
    CHECK(!accepted);

    const char *model_name = owned->object.shader.name;
    owned->object.shader.name = "Experiment/ChangedModel";
    accepted = shader_catalog_dependencies_replay(input, owned);
    owned->object.shader.name = model_name;
    CHECK(!accepted);
    int dependency_count = owned->object.shader.dependency_count;
    owned->object.shader.dependency_count = 1;
    accepted = shader_catalog_dependencies_replay(input, owned);
    owned->object.shader.dependency_count = dependency_count;
    CHECK(!accepted);

    int dependency_node = -1;
    for (int index = 0; index < owned->object.schema.node_count; ++index) {
        const TypeTreeNode *node = &owned->object.schema.nodes[index];
        if (node->level == 1U && node->name_str && strcmp(node->name_str, "m_Dependencies") == 0)
            dependency_node = index;
    }
    CHECK(dependency_node >= 0);
    int path_node = -1;
    for (int index = dependency_node + 1; index < owned->object.schema.node_count; ++index) {
        const TypeTreeNode *node = &owned->object.schema.nodes[index];
        if (node->level <= 1U) break;
        if (node->name_str && strcmp(node->name_str, "m_PathID") == 0) path_node = index;
    }
    CHECK(path_node >= 0);
    /* This element never appears in the empty runtime array. A value-only
     * validator would miss a corrupted complete schema behind that array. */
    owned->object.schema.nodes[path_node].meta_flags ^= UINT32_C(0x20);
    CHECK(serialized_shader_profile_validate_value(&owned->object.root, owned->object.profile));
    accepted = shader_catalog_dependencies_replay(input, owned);
    owned->object.schema.nodes[path_node].meta_flags ^= UINT32_C(0x20);
    CHECK(!accepted);
    CHECK(shader_catalog_dependencies_replay(input, owned));
    return true;
}

static bool lease_cases(const uint8_t *fixture, const TypeTreeSchemaRegistry *registry) {
    char path[160];
    CHECK(temporary_path(path, sizeof(path), 50U));
    CHECK(common_file_write_new_atomic(path, fixture, FIXTURE_SIZE) == COMMON_FILE_OK);
    ShaderCatalog catalog;
    CHECK(load_catalog(path, registry, true, &catalog));
    ShaderCatalogDependenciesInput input = catalog_input(&catalog, registry);
    ShaderCatalogDependencies *owned = NULL;
    ShaderCatalogDependenciesDiagnostic diagnostic;
    CHECK(shader_catalog_dependencies_capture(&input, &owned, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_OK);
    CHECK(shader_catalog_dependencies_replay(&input, owned));
    CHECK(test_replace_regular_file(path, fixture, FIXTURE_SIZE));
    CHECK(!shader_catalog_dependencies_replay(&input, owned));
    ShaderCatalogDependencies *fresh = NULL;
    CHECK(shader_catalog_dependencies_capture(&input, &fresh, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE);
    CHECK(!fresh && diagnostic.object_status == SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE);
    CHECK(diagnostic.observed.release.source_status == UNITY_INPUT_FILE_ERROR);
    ShaderCatalogDependenciesSummary summary;
    CHECK(shader_catalog_dependencies_describe(owned, &summary));
    CHECK(!summary.has_emitted_source && summary.outer_reference_count == 0U);
    shader_catalog_dispose(&catalog);
    shader_catalog_dependencies_free(owned);

    /* A callback runs before commit. A byte-identical replacement during
     * observation must revoke capture in its final lease validation. */
    CHECK(load_catalog(path, registry, true, &catalog));
    input = catalog_input(&catalog, registry);
    ReplacementObserver observer = {0U, path, fixture, FIXTURE_SIZE, false};
    input.observer = replace_during_observation;
    input.observer_context = &observer;
    CHECK(shader_catalog_dependencies_capture(&input, &fresh, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_REPLAY_MISMATCH);
    CHECK(observer.replaced && observer.calls == 1U && fresh == NULL);
    CHECK(diagnostic.status == SHADER_CATALOG_DEPENDENCIES_REPLAY_MISMATCH);
    shader_catalog_dispose(&catalog);
    CHECK(remove(path) == 0);
    return true;
}

static bool capture_cases(TypeTreeSchemaRegistry *registry) {
    ShaderCatalog catalog;
    CHECK(load_catalog(DXBC_DEPENDENCIES_FIXTURE, registry, true, &catalog));
    CHECK(shader_catalog_is_complete(&catalog));
    ShaderCatalogDependenciesInput input = catalog_input(&catalog, registry);
    ShaderCatalogDependencies *owned = NULL;
    ShaderCatalogDependenciesDiagnostic diagnostic;
    CHECK(shader_catalog_dependencies_capture(NULL, &owned, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_INVALID_ARGUMENT);
    CHECK(!owned);
    CHECK(shader_catalog_dependencies_capture(&input, NULL, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_INVALID_ARGUMENT);
    CHECK(shader_catalog_dependencies_capture(&input, &owned, NULL) ==
          SHADER_CATALOG_DEPENDENCIES_OK);
    CHECK(owned && shader_catalog_dependencies_replay(&input, owned));
    shader_catalog_dependencies_free(owned);
    owned = NULL;

    ShaderCatalogRecord copied = catalog.records[0];
    input.record = &copied;
    CHECK(shader_catalog_dependencies_capture(&input, &owned, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE);
    CHECK(!owned && diagnostic.object_status == SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED);
    input.record = catalog.records;
    uint32_t object_size = catalog.records[0].object_size;
    ++catalog.records[0].object_size;
    ShaderCatalogDependenciesStatus coordinate_status =
        shader_catalog_dependencies_capture(&input, &owned, &diagnostic);
    catalog.records[0].object_size = object_size;
    CHECK(coordinate_status == SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE && !owned &&
          diagnostic.object_status == SHADER_CATALOG_OBJECT_COORDINATE_MISMATCH);
    input.registry = NULL;
    CHECK(shader_catalog_dependencies_capture(&input, &owned, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE);
    CHECK(!owned && diagnostic.object_status == SHADER_CATALOG_OBJECT_SCHEMA_UNAVAILABLE);
    input.registry = registry;

    Observer observer = {0};
    input.observer = observe;
    input.observer_context = &observer;
    CHECK(shader_catalog_dependencies_capture(&input, &owned, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_OBSERVER_REJECTED);
    CHECK(!owned && observer.calls == 1U);
    observer.accept = true;
    CHECK(shader_catalog_dependencies_capture(&input, &owned, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_OK);
    CHECK(owned && observer.calls == 2U);
    ShaderCatalogDependenciesSummary summary;
    CHECK(shader_catalog_dependencies_describe(owned, &summary));
    CHECK(summary.outer_reference_count == 0U && summary.nonmodifiable_texture_count == 0U &&
          summary.parsed_dependency_count == 0U);
    /* This authored object has no subshaders, archive or rendering program.
     * Its empty dependency tables cannot certify a source inventory. */
    CHECK(!summary.has_emitted_source &&
          summary.source_status == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
    CHECK(!summary.base_quality.wrapper_complete && summary.base_quality.wrapper_receipt_count == 0U &&
          summary.base_quality.linked_entry_count == 0U &&
          summary.base_quality.required_external_include_root_count == 0U);
    CHECK(bytes_nonzero(summary.release.payload_digest, COMMON_SHA256_DIGEST_SIZE) &&
          bytes_nonzero(summary.release.schema_digest, COMMON_SHA256_DIGEST_SIZE) &&
          bytes_nonzero(summary.release.source_artifact_digest, COMMON_SHA256_DIGEST_SIZE) &&
          bytes_nonzero(summary.release.release_digest, COMMON_SHA256_DIGEST_SIZE));
    ShaderCatalogDependencies *saved = owned;
    CHECK(shader_catalog_dependencies_capture(&input, &owned, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_INVALID_ARGUMENT);
    CHECK(owned == saved && observer.calls == 2U);
    observer.accept = false;
    CHECK(owned_replay_cases(owned, &input));
    CHECK(observer.calls == 2U); /* Replay never invokes observers. */

    ShaderCatalog fresh;
    CHECK(load_catalog(DXBC_DEPENDENCIES_FIXTURE, registry, true, &fresh));
    ShaderCatalogDependenciesInput current = catalog_input(&fresh, registry);
    shader_catalog_dispose(&catalog);
    CHECK(shader_catalog_dependencies_replay(&current, owned));
    ShaderCatalogDependenciesSummary historical;
    CHECK(shader_catalog_dependencies_describe(owned, &historical));
    CHECK(memcmp(historical.release.release_digest, summary.release.release_digest,
                 COMMON_SHA256_DIGEST_SIZE) == 0);

    ShaderCatalog unretained;
    CHECK(load_catalog(DXBC_DEPENDENCIES_FIXTURE, registry, false, &unretained));
    current = catalog_input(&unretained, registry);
    CHECK(!shader_catalog_dependencies_replay(&current, owned));
    ShaderCatalogDependencies *missing = NULL;
    CHECK(shader_catalog_dependencies_capture(&current, &missing, &diagnostic) ==
          SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE);
    CHECK(!missing && diagnostic.object_status == SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE);
    shader_catalog_dispose(&unretained);
    shader_catalog_dispose(&fresh);
    typetree_schema_registry_dispose(registry);
    CHECK(shader_catalog_dependencies_describe(owned, &historical));
    CHECK(memcmp(historical.release.release_digest, summary.release.release_digest,
                 COMMON_SHA256_DIGEST_SIZE) == 0);
    CHECK(!shader_catalog_dependencies_replay(NULL, owned));
    shader_catalog_dependencies_free(owned);
    return true;
}

int main(void) {
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    if (typetree_schema_registry_import_file_replace(&registry, DXBC_DEPENDENCIES_REGISTRY) !=
        TYPETREE_SCHEMA_OK)
        return 1;
    CommonFileBytes fixture = {0};
    if (common_file_read_regular(DXBC_DEPENDENCIES_FIXTURE, 4096U, &fixture) != COMMON_FILE_OK ||
        fixture.size != FIXTURE_SIZE) {
        common_file_bytes_dispose(&fixture);
        typetree_schema_registry_dispose(&registry);
        return 1;
    }
    bool ok = graph_cases(fixture.data, &registry) && unknown_schema_case(fixture.data, &registry) &&
              lease_cases(fixture.data, &registry) && capture_cases(&registry);
    common_file_bytes_dispose(&fixture);
    typetree_schema_registry_dispose(&registry);
    if (ok) puts("owned empty dependency observation: synthetic capture/replay/graph/schema/lease cases passed");
    return ok ? 0 : 1;
}
