// SPDX-License-Identifier: GPL-3.0-only

#ifndef NATIVE_TEXTURE_BATCH_H
#define NATIVE_TEXTURE_BATCH_H

#include "app/material_batch.h"
#include "app/shader_catalog.h"
#include "common/file_io.h"
#include "common/output_publish.h"
#include "io/unity_texture_object.h"
#include "translation/native_texture_yaml_emitter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NATIVE_TEXTURE_BATCH_UNSELECTED = 0,
    NATIVE_TEXTURE_BATCH_PENDING,
    NATIVE_TEXTURE_BATCH_EMITTED,
    NATIVE_TEXTURE_BATCH_UNCHANGED,
    NATIVE_TEXTURE_BATCH_FAILED,
} NativeTextureBatchRecordStatus;

typedef enum {
    NATIVE_TEXTURE_BATCH_FAILURE_NONE = 0,
    NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_NOT_RETAINED,
    NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_REOPEN,
    NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_IDENTITY,
    NATIVE_TEXTURE_BATCH_FAILURE_SERIALIZED_METADATA,
    NATIVE_TEXTURE_BATCH_FAILURE_OBJECT_NOT_FOUND,
    NATIVE_TEXTURE_BATCH_FAILURE_OBJECT_DECODE,
    NATIVE_TEXTURE_BATCH_FAILURE_EXPORT_SHAPE_UNSUPPORTED,
    NATIVE_TEXTURE_BATCH_FAILURE_COPY_ALLOCATION,
    NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_PATH,
    NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_OPEN,
    NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_RANGE,
    NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_IDENTITY,
    NATIVE_TEXTURE_BATCH_FAILURE_STAGING_DIRECTORY,
    NATIVE_TEXTURE_BATCH_FAILURE_STAGING_IO,
    NATIVE_TEXTURE_BATCH_FAILURE_STAGING_IDENTITY,
    NATIVE_TEXTURE_BATCH_FAILURE_STAGING_CLEANUP,
    NATIVE_TEXTURE_BATCH_FAILURE_ASSET_GUID,
    NATIVE_TEXTURE_BATCH_FAILURE_YAML,
    NATIVE_TEXTURE_BATCH_FAILURE_META,
    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_DIRECTORY,
    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_NAME,
    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_COLLISION,
    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_IO,
} NativeTextureBatchFailure;

typedef struct {
    NativeTextureBatchRecordStatus status;
    NativeTextureBatchFailure failure;
    size_t source_index;
    uint8_t source_serialized_digest[COMMON_SHA256_DIGEST_SIZE];
    char source_occurrence_id[96];
    int32_t class_id;
    int64_t path_id;
    UnityTextureObjectStatus object_status;
    UnityNativeTextureYamlStatus yaml_status;
    CommonFileStatus resource_status;
    CommonOutputPublishStatus asset_publish_status;
    CommonOutputPublishStatus meta_publish_status;
    bool asset_publish_attempted;
    bool meta_publish_attempted;
    bool yaml_attempted;
    bool stage_complete;
    bool staging_residue;
    /* True when this failed transaction created at least one companion that
     * cannot be safely removed without a pathname replacement race. */
    bool publication_residue;
    uint8_t resource_digest[COMMON_SHA256_DIGEST_SIZE];
    char resource_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char artifact_identity_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char asset_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    uint8_t published_asset_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t published_meta_digest[COMMON_SHA256_DIGEST_SIZE];
    size_t published_asset_size;
    size_t published_meta_size;
    bool published_content_recorded;
    char* output_path;
    char* output_meta_path;
    char* staging_path;
    /* Exact source bytes copied before source/resource views close. Keeping
     * staging in owned memory avoids every pathname-replacement cleanup race
     * on both POSIX and Windows. */
    uint8_t* owned_staged_bytes;
    size_t owned_staged_size;

    /* Internal owned projection retained until publication and exposed only
     * so result disposal remains allocation-transparent. */
    UnityTextureObject object;
    uint8_t* owned_name;
    uint8_t* owned_stream_path;
    size_t resource_anchor_index;
} NativeTextureBatchRecordResult;

typedef struct {
    size_t selected;
    size_t texture2d;
    size_t render_texture;
    size_t emitted;
    size_t unchanged;
    size_t failed;
} NativeTextureBatchStats;

typedef struct {
    /* Borrowed authority: resolver requests must come from this exact catalog
     * instance and must also match the retained source occurrence identity. */
    const ShaderCatalog* catalog_authority;
    /* Borrowed exact selection authority.  The caller must keep this
     * catalog->record_count-entry array alive and unchanged for the result's
     * lifetime.  Completeness rebuilds the unique native target population
     * from its current bits, so a later mutation fails closed. */
    const bool* selection_authority;
    bool source_transaction_complete;
    bool staging_cleanup_complete;
    NativeTextureBatchFailure source_transaction_failure;
    size_t staging_residue_count;
    char* staging_directory;
    NativeTextureBatchRecordResult* records;
    size_t record_count;
    NativeTextureBatchStats stats;
} NativeTextureBatchResult;

typedef enum {
    NATIVE_TEXTURE_BATCH_OK = 0,
    NATIVE_TEXTURE_BATCH_INVALID_ARGUMENT,
    NATIVE_TEXTURE_BATCH_ALLOCATION_FAILED,
    NATIVE_TEXTURE_BATCH_RESOLVER_GRAPH_FAILED,
} NativeTextureBatchStatus;

void native_texture_batch_result_init(NativeTextureBatchResult* result);
void native_texture_batch_result_dispose(NativeTextureBatchResult* result);

/* Resolves the exact non-null ClassID 28/84 dependencies of selected
 * Materials, consumes catalog-retained source identity anchors, embeds exact
 * loose .resS slices, and publishes deterministic native assets.  Per-object
 * failures are retained in an otherwise successful batch result.  catalog
 * and selected_shaders are borrowed by a successful result and must remain
 * alive and unchanged until native_texture_batch_result_dispose(result). */
NativeTextureBatchStatus native_texture_batch_export(
    ShaderCatalog* catalog, const bool* selected_shaders,
    const char* output_directory, NativeTextureBatchResult* result);

/* Pure helpers used by the production batch and its platform-independent
 * regression tests.  The resource path is always resolved beside the exact
 * loose SerializedFile, never from a recursive discovery/PPtr scope root. */
char* native_texture_batch_resolve_loose_resource_path(
    const ShaderCatalogSource* source, UnityTextureByteView stream_path);

bool native_texture_batch_derive_asset_identity(
    const uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE],
    int64_t path_id, int32_t class_id,
    const uint8_t resource_digest[COMMON_SHA256_DIGEST_SIZE],
    char identity_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U],
    char guid[UNITY_ASSET_GUID_TEXT_CAPACITY]);

/* Publication can leave an ambiguous exact file only after the corresponding
 * write was actually attempted.  This pure ledger predicate is shared by the
 * production transaction and its race-policy regression. */
bool native_texture_batch_publication_residue_possible(
    bool publish_attempted,
    CommonOutputPreflightStatus preflight_status,
    CommonOutputPublishStatus publish_status,
    const char* path, const void* data, size_t size);

/* Pure MaterialBatch resolver over a finished NativeTextureBatchResult. */
MaterialBatchTextureReferenceStatus native_texture_batch_resolve_reference(
    const MaterialBatchTextureReferenceRequest* request, void* context,
    MaterialBatchTextureReference* reference);

/* Closes and validates both held publication identities returned by the
 * resolver. MaterialBatch calls this only after the referencing Material's
 * publication attempt has completed. */
bool native_texture_batch_close_publication_lease(void* lease);

bool native_texture_batch_is_complete(const NativeTextureBatchResult* result);

/* Validates aggregate counters, every record state, and the exact published
 * asset/meta identities in one census.  When record_publications is non-NULL
 * it must contain record_count entries and receives the same per-record
 * publication snapshot used to derive the returned completeness value. */
bool native_texture_batch_validate_publication_snapshot(
    const NativeTextureBatchResult* result, bool* record_publications,
    size_t record_count);
bool native_texture_batch_record_is_published(
    const NativeTextureBatchRecordResult* record);

const char* native_texture_batch_record_status_name(
    NativeTextureBatchRecordStatus status);
const char* native_texture_batch_failure_name(NativeTextureBatchFailure failure);
const char* native_texture_batch_status_name(NativeTextureBatchStatus status);

#endif /* NATIVE_TEXTURE_BATCH_H */
