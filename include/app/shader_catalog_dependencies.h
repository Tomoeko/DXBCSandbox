// SPDX-License-Identifier: GPL-3.0-only
#ifndef SHADER_CATALOG_DEPENDENCIES_H
#define SHADER_CATALOG_DEPENDENCIES_H

#include "app/shader_catalog_object.h"
#include "translation/shaderlab_source_quality.h"

typedef struct ShaderCatalogDependencies ShaderCatalogDependencies;

typedef struct {
    ShaderCatalogObjectReport release;
    size_t outer_reference_count, nonmodifiable_texture_count, parsed_dependency_count;
    bool has_emitted_source;
    ShaderLabSourceQualityStatus source_status;
    /* Present only with has_emitted_source. This is the unchanged ordinary
     * source observation, including every existing gap. */
    ShaderLabSourceQualityResult base_quality;
} ShaderCatalogDependenciesSummary;

typedef bool (*ShaderCatalogDependenciesObserver)(void *context,
    const ShaderCatalogDependenciesSummary *summary);

typedef struct {
    const ShaderCatalog *catalog;
    const ShaderCatalogRecord *record;
    const TypeTreeSchemaRegistry *registry;
    /* Called once before capture commits; never during replay. Input storage
     * must remain immutable and alive throughout either operation. */
    ShaderCatalogDependenciesObserver observer;
    void *observer_context;
} ShaderCatalogDependenciesInput;

typedef enum {
    SHADER_CATALOG_DEPENDENCIES_OK = 0,
    SHADER_CATALOG_DEPENDENCIES_INVALID_ARGUMENT,
    SHADER_CATALOG_DEPENDENCIES_ALLOCATION_FAILED,
    SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE,
    SHADER_CATALOG_DEPENDENCIES_SCHEMA_UNAVAILABLE,
    SHADER_CATALOG_DEPENDENCIES_INVALID_METADATA,
    SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE,
    SHADER_CATALOG_DEPENDENCIES_SOURCE_FAILED,
    SHADER_CATALOG_DEPENDENCIES_REPLAY_MISMATCH,
    SHADER_CATALOG_DEPENDENCIES_OBSERVER_REJECTED
} ShaderCatalogDependenciesStatus;

typedef struct {
    ShaderCatalogDependenciesStatus status;
    ShaderCatalogObjectStatus object_status;
    ShaderObjectStatus archive_status;
    TypeTreeSchemaProfileResult schema_profile;
    ShaderCatalogDependenciesSummary observed;
    ShaderLabSourceQualityDiagnostic source;
} ShaderCatalogDependenciesDiagnostic;

/* Internally decodes an owned Class48 record from its unique retained input
 * lease, including the complete known schema (even empty element shapes).
 * Initial scope requires empty outer PPtr, immutable-texture and parsed
 * dependency tables, empty fallback/custom-editor edges, and ordinary passes.
 * Nonempty tables, including null PPtrs, are unavailable. No pathname reopen,
 * caller object/source/inventory or raw ID equality can supply authority.
 * A representable source is observed through the existing normal emitter;
 * a source-unavailable object may still have an empty-table observation.
 * This is not graph resolution, include closure, source-quality promotion,
 * compilation, reflection, import or runtime certification.
 * Requires *output == NULL. Failure preserves it. Diagnostic is optional. */
ShaderCatalogDependenciesStatus shader_catalog_dependencies_capture(
    const ShaderCatalogDependenciesInput *input, ShaderCatalogDependencies **output,
    ShaderCatalogDependenciesDiagnostic *diagnostic);
/* Fresh current lease decoding, complete release comparison, and independent
 * ordinary inventory replay. An absent/disposed lease always fails. */
bool shader_catalog_dependencies_replay(const ShaderCatalogDependenciesInput *current,
    const ShaderCatalogDependencies *owned);
/* Copies historical observations; no catalog/registry lifetime is required. */
bool shader_catalog_dependencies_describe(const ShaderCatalogDependencies *owned,
    ShaderCatalogDependenciesSummary *summary);
void shader_catalog_dependencies_free(ShaderCatalogDependencies *owned);
const char *shader_catalog_dependencies_status_name(ShaderCatalogDependenciesStatus status);

#endif
