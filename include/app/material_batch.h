// SPDX-License-Identifier: GPL-3.0-only

#ifndef MATERIAL_BATCH_H
#define MATERIAL_BATCH_H

#include "app/shader_batch.h"
#include "app/shader_catalog_pptr.h"
#include "common/output_publish.h"
#include "common/unity_asset_guid.h"
#include "translation/material_yaml_emitter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A player SerializedFile PPtr identifies an exact object, but it does not
 * by itself establish the AssetDatabase GUID/fileID/type tuple needed by a
 * Unity text Material.  In particular, converting AssetFileExternal.guid
 * bytes to GUID text is an authority-sensitive operation and local player
 * objects normally have no serialized asset GUID at all.  The batch exporter
 * therefore requires this explicit boundary for every non-null texture.
 */
typedef enum {
    MATERIAL_BATCH_TEXTURE_REFERENCE_OK = 0,
    MATERIAL_BATCH_TEXTURE_REFERENCE_AUTHORITY_MISSING,
    /* The callback has exact target-object evidence, but its policy does not
     * authorize that concrete Unity class as an exported Texture asset.  The
     * production batch layer deliberately does not maintain a Texture class
     * hierarchy of its own. */
    MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_CLASS_UNSUPPORTED,
    /* The callback recognizes the target class, but no concrete texture
     * artifact/importer has been exported from which an exact YAML
     * fileID/guid/type tuple can be derived. */
    MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED,
    MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED,
} MaterialBatchTextureReferenceStatus;

typedef struct {
    int64_t file_id;
    char guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    int32_t type;
    /* True only when the referenced texture asset and its matching .meta are
     * present in the exported dependency set.  A valid but non-exported
     * reference permits exact Material emission while keeping texture/visual
     * closure explicitly false. */
    bool asset_exported;
    /* Opaque simultaneous asset/meta identity lease. An exported non-null
     * tuple is accepted only when the resolver supplies this lease and the
     * matching closer; MaterialBatch holds it through final publication. */
    void* publication_lease;
} MaterialBatchTextureReference;

typedef struct {
    const ShaderCatalog* catalog;
    size_t material_index;
    size_t texture_property_index;
    size_t source_index;
    const AssetPPtr* serialized_pointer;
    const UnityPPtrResolveResult* resolution;
    const ShaderCatalogSource* target_source;
    const AssetObjectInfo* target_object;
} MaterialBatchTextureReferenceRequest;

typedef MaterialBatchTextureReferenceStatus
(*MaterialBatchTextureReferenceResolver)(
    const MaterialBatchTextureReferenceRequest* request,
    void* context,
    MaterialBatchTextureReference* reference);

typedef bool (*MaterialBatchTextureReferenceLeaseCloser)(void* lease);

typedef struct {
    /* Global prerequisite for this material publication transaction.  Native
     * texture export sets this false when any retained source/resource/stage
     * authority is incomplete, including for null-only Materials that never
     * invoke the per-reference resolver. */
    bool dependency_authority_complete;
    MaterialBatchTextureReferenceResolver resolve_texture_reference;
    void* texture_reference_context;
    MaterialBatchTextureReferenceLeaseCloser
        close_texture_reference_publication_lease;
} MaterialBatchOptions;

typedef enum {
    MATERIAL_BATCH_UNSELECTED = 0,
    MATERIAL_BATCH_UNASSOCIATED,
    MATERIAL_BATCH_EMITTED,
    MATERIAL_BATCH_UNCHANGED,
    MATERIAL_BATCH_FAILED,
} MaterialBatchRecordStatus;

typedef enum {
    MATERIAL_BATCH_FAILURE_NONE = 0,
    MATERIAL_BATCH_FAILURE_CATALOG_MATERIAL_NOT_READY,
    MATERIAL_BATCH_FAILURE_SHADER_INDEX_INVALID,
    MATERIAL_BATCH_FAILURE_SHADER_BATCH_NOT_READY,
    MATERIAL_BATCH_FAILURE_SHADER_META_MISSING,
    MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY,
    MATERIAL_BATCH_FAILURE_TEXTURE_DEPENDENCY_IDENTITY,
    MATERIAL_BATCH_FAILURE_DEPENDENCY_AUTHORITY_INCOMPLETE,
    MATERIAL_BATCH_FAILURE_TEXTURE_RESOLUTION,
    MATERIAL_BATCH_FAILURE_TEXTURE_CLASS_UNSUPPORTED,
    MATERIAL_BATCH_FAILURE_TEXTURE_TARGET_ASSET_UNEXPORTED,
    MATERIAL_BATCH_FAILURE_TEXTURE_REFERENCE_AUTHORITY_MISSING,
    MATERIAL_BATCH_FAILURE_TEXTURE_REFERENCE_INVALID,
    MATERIAL_BATCH_FAILURE_PROJECTION_ALLOCATION,
    MATERIAL_BATCH_FAILURE_MATERIAL_GUID,
    MATERIAL_BATCH_FAILURE_MATERIAL_YAML,
    MATERIAL_BATCH_FAILURE_MATERIAL_META,
    MATERIAL_BATCH_FAILURE_DEPENDENCY_EVIDENCE,
    MATERIAL_BATCH_FAILURE_OUTPUT_DIRECTORY,
    MATERIAL_BATCH_FAILURE_OUTPUT_NAME,
    MATERIAL_BATCH_FAILURE_OUTPUT_COLLISION,
    MATERIAL_BATCH_FAILURE_OUTPUT_IO,
} MaterialBatchFailure;

typedef enum {
    MATERIAL_BATCH_DEPENDENCY_NULL = 0,
    MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED,
    MATERIAL_BATCH_DEPENDENCY_RESOLVED_NOT_EXPORTED,
    MATERIAL_BATCH_DEPENDENCY_RESOLUTION_FAILED,
    MATERIAL_BATCH_DEPENDENCY_CLASS_UNSUPPORTED,
    MATERIAL_BATCH_DEPENDENCY_TARGET_ASSET_UNEXPORTED,
    MATERIAL_BATCH_DEPENDENCY_REFERENCE_AUTHORITY_MISSING,
    MATERIAL_BATCH_DEPENDENCY_REFERENCE_INVALID,
} MaterialBatchDependencyStatus;

typedef struct {
    size_t property_index;
    uint8_t* property_name;
    size_t property_name_size;
    int32_t serialized_file_id;
    int64_t serialized_path_id;

    UnityPPtrResolveStatus resolve_status;
    MaterialBatchDependencyStatus status;
    MaterialBatchTextureReferenceStatus reference_status;

    size_t source_index;
    size_t target_source_index;
    size_t external_index;
    bool has_target;
    int32_t target_class_id;
    int64_t target_path_id;
    char target_serialized_digest_hex[
        COMMON_SHA256_DIGEST_SIZE * 2U + 1U];

    bool has_external_serialized_guid;
    uint8_t external_serialized_guid[16];
    int32_t external_serialized_type;

    bool has_yaml_reference;
    MaterialBatchTextureReference yaml_reference;
} MaterialBatchTextureDependency;

typedef struct {
    MaterialBatchRecordStatus status;
    MaterialBatchFailure failure;
    size_t shader_record_index;

    char asset_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    char artifact_identity_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char* output_path;
    char* output_meta_path;
    char* dependency_evidence_path;
    bool has_artifacts;
    bool publication_residue;
    bool material_publish_attempted;
    bool meta_publish_attempted;
    bool evidence_publish_attempted;
    CommonOutputPublishStatus material_publish_status;
    CommonOutputPublishStatus meta_publish_status;
    CommonOutputPublishStatus evidence_publish_status;
    UnityMaterialYamlStatus yaml_status;

    MaterialBatchTextureDependency* dependencies;
    size_t dependency_count;
    bool texture_dependency_closure_complete;
} MaterialBatchRecordResult;

typedef struct {
    size_t catalog_materials;
    size_t selected;
    size_t unselected;
    size_t unassociated;
    size_t emitted;
    size_t unchanged;
    size_t failed;
    /* Distinct selected Shader dependencies bracketed by one held
     * shader/meta verification pair during publication. */
    size_t shader_dependency_groups;

    size_t texture_dependencies;
    size_t null_texture_dependencies;
    size_t resolved_texture_dependencies;
    size_t exported_texture_dependencies;
    size_t unexported_texture_dependencies;
    size_t failed_texture_dependencies;
} MaterialBatchStats;

typedef struct {
    /* Borrowed authority for the complete result ledger.  Report rendering
     * and completeness validation must not detach per-index Material records
     * from the exact catalog that was used to produce them. */
    const ShaderCatalog* catalog_authority;
    /* Borrowed exact selection authority.  The caller must keep this
     * catalog->record_count-entry array alive and unchanged for the result's
     * lifetime.  Completeness re-censuses its current bits against every
     * Material status, so a later mutation fails closed. */
    const bool* selection_authority;
    MaterialBatchRecordResult* records;
    size_t record_count;
    MaterialBatchStats stats;
} MaterialBatchResult;

typedef enum {
    MATERIAL_BATCH_OK = 0,
    MATERIAL_BATCH_INVALID_ARGUMENT,
    MATERIAL_BATCH_ALLOCATION_FAILED,
    MATERIAL_BATCH_RESOLVER_GRAPH_FAILED,
} MaterialBatchStatus;

void material_batch_options_default(MaterialBatchOptions* options);
void material_batch_texture_reference_init(
    MaterialBatchTextureReference* reference);
void material_batch_result_init(MaterialBatchResult* result);
void material_batch_result_dispose(MaterialBatchResult* result);

/*
 * Exports every exact ClassID 21 record whose resolved shader is selected.
 * Catalog material errors are retained as failures instead of being silently
 * omitted.  Shader-null materials are explicitly unassociated.  The shader
 * batch must have emitted deterministic ShaderImporter metadata.
 *
 * catalog and selected_shaders are borrowed by a successful result and must
 * remain alive and unchanged until material_batch_result_dispose(result).
 *
 * The operation has a strong result guarantee.  Artifact publication never
 * overwrites a pre-existing file: byte-identical files are unchanged and
 * differing files are collisions.  Per-material failures are retained in an
 * otherwise successful MATERIAL_BATCH_OK result.
 */
MaterialBatchStatus material_batch_export(
    const ShaderCatalog* catalog,
    const bool* selected_shaders,
    const ShaderBatchResult* shader_batch,
    const char* output_directory,
    const MaterialBatchOptions* options,
    MaterialBatchResult* result);

/* Exact Material/.meta/evidence publication completeness. */
bool material_batch_is_complete(const MaterialBatchResult* result);

/* Stronger dependency gate.  This is deliberately separate from Material
 * emission and is false while any non-null referenced texture asset is not
 * part of the exported dependency set. */
bool material_batch_texture_dependencies_are_closed(
    const MaterialBatchResult* result);

const char* material_batch_texture_reference_status_name(
    MaterialBatchTextureReferenceStatus status);
const char* material_batch_record_status_name(MaterialBatchRecordStatus status);
const char* material_batch_failure_name(MaterialBatchFailure failure);
const char* material_batch_dependency_status_name(
    MaterialBatchDependencyStatus status);
const char* material_batch_status_name(MaterialBatchStatus status);

#endif /* MATERIAL_BATCH_H */
