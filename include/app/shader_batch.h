// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_BATCH_H
#define SHADER_BATCH_H

#include "app/shader_catalog.h"
#include "common/unity_asset_guid.h"
#include "common/output_publish.h"
#include "io/compute_shader_artifact.h"
#include "translation/shaderlab_emitter.h"
#include "translation/shaderlab_structural_certificate.h"

typedef enum {
    SHADER_BATCH_UNSELECTED = 0,
    SHADER_BATCH_PENDING,
    SHADER_BATCH_EMITTED,
    SHADER_BATCH_UNCHANGED,
    SHADER_BATCH_UNAVAILABLE,
    SHADER_BATCH_FAILED,
} ShaderBatchRecordStatus;

typedef enum {
    SHADER_BATCH_FAILURE_NONE = 0,
    SHADER_BATCH_FAILURE_CATALOG_NOT_READY,
    SHADER_BATCH_FAILURE_SOURCE_REOPEN,
    SHADER_BATCH_FAILURE_SOURCE_NOT_FOUND,
    SHADER_BATCH_FAILURE_SERIALIZED_METADATA,
    SHADER_BATCH_FAILURE_SCHEMA,
    SHADER_BATCH_FAILURE_OBJECT_NOT_FOUND,
    SHADER_BATCH_FAILURE_OBJECT_DECODE,
    SHADER_BATCH_FAILURE_COMPUTE_OBJECT_DECODE,
    SHADER_BATCH_FAILURE_D3D11_ARCHIVE,
    SHADER_BATCH_FAILURE_COMPUTE_ARTIFACT_BUILD,
    SHADER_BATCH_FAILURE_CANDIDATE_EMISSION,
    SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE,
    SHADER_BATCH_FAILURE_OUTPUT_DIRECTORY,
    SHADER_BATCH_FAILURE_OUTPUT_NAME,
    SHADER_BATCH_FAILURE_OUTPUT_COLLISION,
    SHADER_BATCH_FAILURE_OUTPUT_IO,
    SHADER_BATCH_FAILURE_ASSET_GUID,
    SHADER_BATCH_FAILURE_SHADER_META_EMISSION,
} ShaderBatchFailure;

/* Exact per-member ledger for a variable-size ClassID 72 package. Aggregate
 * fields below are convenient report summaries; this ledger preserves which
 * binary/manifest was actually preflighted, attempted, or left as residue. */
typedef struct {
    char filename[224];
    CommonOutputPreflightStatus preflight_status;
    CommonOutputPublishStatus publish_status;
    bool preflight_attempted;
    bool publish_attempted;
    bool publication_residue;
    bool is_manifest;
} ShaderBatchComputeArtifactPublication;

typedef struct {
    ShaderBatchRecordStatus status;
    ShaderBatchFailure failure;
    TypeTreeSchemaStatus schema_status;
    ShaderObjectStatus object_status;
    ComputeShaderObjectStatus compute_object_status;
    ComputeShaderArtifactStatus compute_artifact_status;
    ComputeShaderSourceAuthorityStatus compute_source_authority_status;
    UnityInputStatus input_status;
    ShaderLabCandidateDiagnostic candidate_diagnostic;
    ShaderLabStructuralDiagnostic structural_diagnostic;
    char* output_path;
    char asset_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    char* output_meta_path;
    bool has_asset_meta;
    CommonOutputPreflightStatus shader_preflight_status;
    CommonOutputPreflightStatus meta_preflight_status;
    CommonOutputPublishStatus shader_publish_status;
    CommonOutputPublishStatus meta_publish_status;
    bool shader_preflight_attempted;
    bool meta_preflight_attempted;
    bool shader_publish_attempted;
    bool meta_publish_attempted;
    bool shader_publication_residue;
    bool meta_publication_residue;
    size_t published_shader_size;
    size_t published_meta_size;
    uint8_t published_shader_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t published_meta_digest[COMMON_SHA256_DIGEST_SIZE];
    bool published_shader_content_recorded;
    bool published_meta_content_recorded;
    /* Dedicated failure-only paths. Authorized outputs remain separate so a
     * failed transaction cannot be mistaken for a usable Shader asset. */
    char* shader_publication_residue_path;
    char* meta_publication_residue_path;
    ShaderBatchComputeArtifactPublication* compute_artifact_publications;
    size_t compute_artifact_publication_count;
    /* Preflight aggregate precedence is collision, I/O error, missing,
     * unchanged. Publish is the first failure, otherwise emitted if any
     * member was emitted and unchanged only when every member was unchanged. */
    CommonOutputPreflightStatus compute_preflight_status;
    CommonOutputPublishStatus compute_publish_status;
    bool compute_preflight_attempted;
    bool compute_publish_attempted;
    bool compute_publication_residue;
    bool publication_residue;
    /* True only when the complete output artifact set was published after
     * the exact source identity was closed successfully. Deferred identity
     * ownership is reported explicitly and never claims final authority. */
    bool publication_authorized;
    bool source_identity_close_deferred;
    /* Immutable-at-publication catalog coordinates copied before extraction.
     * Downstream Material export rejects same-sized or later-mutated batches
     * that do not match the exact originating ShaderCatalog record. */
    uint8_t catalog_serialized_digest[COMMON_SHA256_DIGEST_SIZE];
    int64_t catalog_path_id;
    int32_t catalog_class_id;
    bool catalog_provenance_recorded;
} ShaderBatchRecordResult;

typedef struct {
    size_t selected;
    size_t emitted;
    size_t unchanged;
    size_t unavailable;
    size_t failed;
} ShaderBatchStats;

typedef struct {
    const ShaderCatalog* catalog_authority;
    ShaderBatchRecordResult* records;
    size_t record_count;
    ShaderBatchStats stats;
} ShaderBatchResult;

typedef enum {
    SHADER_BATCH_OK = 0,
    SHADER_BATCH_INVALID_ARGUMENT,
    SHADER_BATCH_ALLOCATION_FAILED,
} ShaderBatchStatus;

typedef struct {
    /* Emit a deterministic ShaderImporter .meta next to each selected
     * graphics .shader. Required by associated Material export. */
    bool emit_shader_meta;
    /* Place graphics .shader files directly in output_directory and derive
     * their portable filenames from the serialized Shader name. Equal flat
     * names receive a deterministic source-identity suffix. Compute package
     * layout is unchanged. The default remains digest-scoped directories. */
    bool flat_graphics_output;
    /* Keep catalog-owned identity anchors open for a subsequent dependency
     * pass. The current mapping is suspended after shader extraction; the
     * dependency pass must validate and close every retained snapshot before
     * exposing generated references. */
    bool defer_source_snapshot_close;
} ShaderBatchOptions;

void shader_batch_options_default(ShaderBatchOptions* options);

void shader_batch_result_init(ShaderBatchResult* result);
void shader_batch_result_dispose(ShaderBatchResult* result);

/* selected has catalog->record_count entries. Output files use the selected
 * options layout and are never overwritten. Existing identical bytes are
 * reported as unchanged; differing bytes are collisions. */
ShaderBatchStatus shader_batch_extract(
    const ShaderCatalog* catalog, const bool* selected,
    const TypeTreeSchemaRegistry* schema_registry,
    const char* output_directory, ShaderBatchResult* result);

ShaderBatchStatus shader_batch_extract_ex(
    const ShaderCatalog* catalog, const bool* selected,
    const TypeTreeSchemaRegistry* schema_registry,
    const char* output_directory, const ShaderBatchOptions* options,
    ShaderBatchResult* result);

bool shader_batch_is_complete(const ShaderBatchResult* result);
const char* shader_batch_record_status_name(ShaderBatchRecordStatus status);
const char* shader_batch_failure_name(ShaderBatchFailure failure);
const char* shader_batch_status_name(ShaderBatchStatus status);

#endif /* SHADER_BATCH_H */
