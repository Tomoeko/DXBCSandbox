// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "app/shader_batch.h"
#include "app/shader_catalog_internal.h"
#include "app/shader_batch_internal.h"

#include "common/output_publish.h"
#include "common/sha256.h"
#include "common/shader_artifact.h"
#include "common/unity_asset_guid.h"
#include "translation/material_yaml_emitter.h"
#include "translation/shaderlab_emitter.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char filename[224];
    char* path;
    uint8_t* bytes;
    size_t size;
    bool is_manifest;
} StagedComputeArtifact;

typedef struct {
    size_t record_index;
    char* directory_path;
    char* output_path;
    char* meta_path;
    char* shader_bytes;
    size_t shader_size;
    char* meta_bytes;
    size_t meta_size;
    bool emit_shader_meta;
    bool is_compute;
    StagedComputeArtifact* compute_artifacts;
    size_t compute_artifact_count;
    size_t compute_manifest_index;
} StagedShaderPublication;

typedef struct {
    const ShaderCatalog* catalog;
    const bool* selected;
    const TypeTreeSchemaRegistry* registry;
    const char* output_directory;
    ShaderBatchResult* result;
    bool emit_shader_meta;
    bool flat_graphics_output;
    bool defer_source_snapshot_close;
    bool allocation_failed;
    ShaderBatchCandidateSelector select_candidate;
    void *candidate_context;
    /* The source is borrowed from the exact still-open file identity which
     * the catalog already hashed. */
    bool source_digest_prevalidated;
    StagedShaderPublication* staged;
    size_t staged_count;
    size_t staged_capacity;
} BatchVisitorContext;

static void staged_shader_publication_dispose(
    StagedShaderPublication* staged) {
    if (!staged) return;
    free(staged->directory_path);
    free(staged->output_path);
    free(staged->meta_path);
    free(staged->shader_bytes);
    free(staged->meta_bytes);
    for (size_t index = 0U;
         index < staged->compute_artifact_count; ++index) {
        free(staged->compute_artifacts[index].path);
        free(staged->compute_artifacts[index].bytes);
    }
    free(staged->compute_artifacts);
    memset(staged, 0, sizeof(*staged));
}

static void discard_staged_shader_publications(
    BatchVisitorContext* context) {
    if (!context) return;
    for (size_t index = 0U; index < context->staged_count; ++index) {
        staged_shader_publication_dispose(&context->staged[index]);
    }
    context->staged_count = 0U;
}

static bool append_staged_shader_publication(
    BatchVisitorContext* context, StagedShaderPublication* staged) {
    if (!context || !staged) return false;
    if (context->staged_count == context->staged_capacity) {
        size_t capacity = context->staged_capacity
            ? context->staged_capacity * 2U : 4U;
        if (capacity < context->staged_capacity ||
            capacity > SIZE_MAX / sizeof(*context->staged)) {
            return false;
        }
        StagedShaderPublication* resized =
            (StagedShaderPublication*)realloc(
                context->staged, capacity * sizeof(*context->staged));
        if (!resized) return false;
        context->staged = resized;
        context->staged_capacity = capacity;
    }
    context->staged[context->staged_count++] = *staged;
    memset(staged, 0, sizeof(*staged));
    return true;
}

void shader_batch_options_default(ShaderBatchOptions* options) {
    if (options) memset(options, 0, sizeof(*options));
}

static char* duplicate_path(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1U);
    if (copy) memcpy(copy, value, size + 1U);
    return copy;
}

static bool portable_filenames_equal(
    const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right) {
        unsigned char left_value = (unsigned char)*left++;
        unsigned char right_value = (unsigned char)*right++;
        if (left_value >= 'A' && left_value <= 'Z') {
            left_value = (unsigned char)(left_value + ('a' - 'A'));
        }
        if (right_value >= 'A' && right_value <= 'Z') {
            right_value = (unsigned char)(right_value + ('a' - 'A'));
        }
        if (left_value != right_value) return false;
    }
    return *left == *right;
}

bool shader_batch_flat_graphics_filename(
    const ShaderCatalog* catalog, size_t record_index,
    const char* decoded_shader_name, char* output, size_t output_size) {
    if (!catalog || !catalog->records || record_index >= catalog->record_count ||
        !decoded_shader_name || !output || output_size == 0U) return false;
    const ShaderCatalogRecord* record = &catalog->records[record_index];
    if (record->class_id != 48 || !record->name ||
        strcmp(record->name, decoded_shader_name) != 0) return false;

    char base[384];
    if (!shader_flat_artifact_filename(
            base, sizeof(base), decoded_shader_name, NULL, record->path_id)) {
        return false;
    }
    bool disambiguate = false;
    for (size_t index = 0U; index < catalog->record_count; ++index) {
        if (index == record_index || catalog->records[index].class_id != 48 ||
            !catalog->records[index].name) continue;
        char other[384];
        if (!shader_flat_artifact_filename(
                other, sizeof(other), catalog->records[index].name, NULL,
                catalog->records[index].path_id)) return false;
        if (!portable_filenames_equal(base, other)) continue;
        if (record->path_id != catalog->records[index].path_id ||
            memcmp(record->serialized_digest,
                   catalog->records[index].serialized_digest,
                   COMMON_SHA256_DIGEST_SIZE) != 0) {
            disambiguate = true;
            break;
        }
    }
    return shader_flat_artifact_filename(
        output, output_size, decoded_shader_name,
        disambiguate ? record->serialized_digest_hex : NULL,
        record->path_id);
}

static bool nullable_strings_equal(const char* left, const char* right) {
    return (!left && !right) || (left && right && strcmp(left, right) == 0);
}

static bool source_matches_record(
    const ShaderCatalogRecord* record,
    const UnitySerializedSource* source,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
    bool digest_prevalidated) {
    return record && source &&
        strcmp(record->outer_path, source->outer_path) == 0 &&
        nullable_strings_equal(record->member_name, source->member_name) &&
        record->member_index == source->member_index &&
        record->is_bundle_member == source->is_bundle_member &&
        (digest_prevalidated ||
         memcmp(record->serialized_digest, digest,
                COMMON_SHA256_DIGEST_SIZE) == 0);
}

void shader_batch_result_init(ShaderBatchResult* result) {
    if (result) memset(result, 0, sizeof(*result));
}

void shader_batch_result_dispose(ShaderBatchResult* result) {
    if (!result) return;
    for (size_t i = 0U; i < result->record_count; ++i) {
        free(result->records[i].output_path);
        free(result->records[i].output_meta_path);
        free(result->records[i].shader_publication_residue_path);
        free(result->records[i].meta_publication_residue_path);
        free(result->records[i].compute_artifact_publications);
    }
    free(result->records);
    shader_batch_result_init(result);
}

static void mark_failure(ShaderBatchResult* result, size_t index,
                         ShaderBatchFailure failure) {
    ShaderBatchRecordResult* record = &result->records[index];
    if (record->status == SHADER_BATCH_PENDING) {
        record->status = SHADER_BATCH_FAILED;
        record->failure = failure;
        ++result->stats.failed;
    }
}

void shader_batch_retain_graphics_publication_residue_paths(
    ShaderBatchRecordResult* result,
    char** owned_shader_path, char** owned_meta_path) {
    if (!result) return;
    if (result->shader_publication_residue && owned_shader_path &&
        *owned_shader_path && !result->shader_publication_residue_path) {
        result->shader_publication_residue_path = *owned_shader_path;
        *owned_shader_path = NULL;
    }
    if (result->meta_publication_residue && owned_meta_path &&
        *owned_meta_path && !result->meta_publication_residue_path) {
        result->meta_publication_residue_path = *owned_meta_path;
        *owned_meta_path = NULL;
    }
}

static bool stage_candidate(
    const ShaderCatalogRecord* catalog_record,
    const ShaderObject* object, const ShaderBlobArchive* archive,
    size_t record_index, BatchVisitorContext* context,
    ShaderBatchRecordResult* result) {
    if (!context) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        return false;
    }
    StagedShaderPublication staged;
    memset(&staged, 0, sizeof(staged));
    staged.record_index = record_index;
    staged.emit_shader_meta = context->emit_shader_meta;
    bool staged_appended = false;

    char artifact[384];
    bool artifact_named = context->flat_graphics_output
        ? shader_batch_flat_graphics_filename(
              context->catalog, record_index, object->shader.name,
              artifact, sizeof(artifact))
        : shader_artifact_filename(
              artifact, sizeof(artifact), object->shader.name,
              object->path_id);
    if (!artifact_named) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        return false;
    }

    StringBuilder shaderlab;
    StringBuilder meta;
    sb_init_with_capacity(&shaderlab, 16384U);
    sb_init(&meta);
    staged.directory_path = context->flat_graphics_output
        ? duplicate_path(context->output_directory)
        : common_output_join_path(
              context->output_directory,
              catalog_record->serialized_digest_hex);
    if (!staged.directory_path) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_DIRECTORY;
        goto stage_done;
    }
    staged.output_path = common_output_join_path(
        staged.directory_path, artifact);
    if (!staged.output_path) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto stage_done;
    }

    bool emitted;
    if (context->select_candidate) {
        const ShaderBatchCandidateInput input = {
            .catalog_record_index = record_index,
            .object = object,
            .archive = archive,
            .source_path = staged.output_path,
            .source_directory = staged.directory_path,
            .source_basename = artifact,
        };
        emitted = context->select_candidate(context->candidate_context, &input, &shaderlab,
                                             &result->candidate_diagnostic) &&
                  sb_ok(&shaderlab) && shaderlab.len != 0;
    } else {
        emitted = shaderlab_emit_candidate_with_diagnostic(
            &object->shader, archive->entries, archive->entry_count,
            archive->segments, archive->segment_lengths,
            archive->segment_count, &shaderlab, &result->candidate_diagnostic);
        if (emitted) {
            sb_append_char(&shaderlab, '\n');
            emitted = sb_ok(&shaderlab);
            if (!emitted)
                result->candidate_diagnostic.status = SHADERLAB_CANDIDATE_OUTPUT_FAILED;
        }
    }
    if (!emitted) {
        result->failure = context->select_candidate
            ? SHADER_BATCH_FAILURE_CANDIDATE_SELECTION : SHADER_BATCH_FAILURE_CANDIDATE_EMISSION;
        goto stage_done;
    }
    if (shaderlab_structural_certify(object, &result->structural_diagnostic) != SHADERLAB_STRUCTURE_OK) {
        result->failure = SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE;
        goto stage_done;
    }

    if (staged.emit_shader_meta) {
        uint8_t identity[COMMON_SHA256_DIGEST_SIZE + 8U];
        memcpy(identity, catalog_record->serialized_digest,
               COMMON_SHA256_DIGEST_SIZE);
        uint64_t path_bits = (uint64_t)catalog_record->path_id;
        for (unsigned i = 0U; i < 8U; ++i) {
            identity[COMMON_SHA256_DIGEST_SIZE + i] =
                (uint8_t)(path_bits >> (i * 8U));
        }
        if (!unity_asset_guid_derive(
                UNITY_ASSET_GUID_DOMAIN_SHADER, identity, sizeof(identity),
                result->asset_guid)) {
            result->failure = SHADER_BATCH_FAILURE_ASSET_GUID;
            goto stage_done;
        }
        size_t path_size = strlen(staged.output_path);
        if (path_size > SIZE_MAX - sizeof(".meta")) {
            result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
            goto stage_done;
        }
        staged.meta_path = (char*)malloc(path_size + sizeof(".meta"));
        if (!staged.meta_path) {
            result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
            goto stage_done;
        }
        memcpy(staged.meta_path, staged.output_path, path_size);
        memcpy(staged.meta_path + path_size, ".meta", sizeof(".meta"));
        if (unity_shader_importer_meta_emit(result->asset_guid, &meta) !=
                UNITY_MATERIAL_YAML_OK ||
            !sb_ok(&meta)) {
            result->failure = SHADER_BATCH_FAILURE_SHADER_META_EMISSION;
            goto stage_done;
        }
    }

    staged.shader_size = shaderlab.len;
    staged.shader_bytes = sb_detach(&shaderlab);
    if (!staged.shader_bytes) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto stage_done;
    }
    if (staged.emit_shader_meta) {
        staged.meta_size = meta.len;
        staged.meta_bytes = sb_detach(&meta);
        if (!staged.meta_bytes) {
            result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
            goto stage_done;
        }
    }
    if (!append_staged_shader_publication(context, &staged)) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto stage_done;
    }
    staged_appended = true;

stage_done:
    sb_free(&shaderlab);
    sb_free(&meta);
    staged_shader_publication_dispose(&staged);
    return staged_appended;
}

static bool publish_status_is_success(CommonOutputPublishStatus status) {
    return status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        status == COMMON_OUTPUT_PUBLISH_UNCHANGED;
}

static void commit_staged_graphics_publication(
    BatchVisitorContext* context, StagedShaderPublication* staged,
    bool source_identity_close_deferred) {
    ShaderBatchRecordResult* result =
        &context->result->records[staged->record_index];
    if (result->status != SHADER_BATCH_PENDING) return;
    result->source_identity_close_deferred =
        source_identity_close_deferred;
    result->published_shader_size = staged->shader_size;
    common_sha256(staged->shader_bytes, staged->shader_size,
                  result->published_shader_digest);
    result->published_shader_content_recorded = true;
    if (staged->emit_shader_meta) {
        result->published_meta_size = staged->meta_size;
        common_sha256(staged->meta_bytes, staged->meta_size,
                      result->published_meta_digest);
        result->published_meta_content_recorded = true;
    }

    if (!common_output_ensure_directory_tree(staged->directory_path)) {
        mark_failure(context->result, staged->record_index,
                     SHADER_BATCH_FAILURE_OUTPUT_DIRECTORY);
        return;
    }

    result->shader_preflight_attempted = true;
    result->shader_preflight_status = common_output_preflight_exact(
        staged->output_path, staged->shader_bytes, staged->shader_size);
    if (result->shader_preflight_status ==
            COMMON_OUTPUT_PREFLIGHT_COLLISION) {
        result->shader_publish_status = COMMON_OUTPUT_PUBLISH_COLLISION;
    } else if (result->shader_preflight_status ==
               COMMON_OUTPUT_PREFLIGHT_IO_ERROR) {
        result->shader_publish_status = COMMON_OUTPUT_PUBLISH_IO_ERROR;
    }
    if (staged->emit_shader_meta) {
        result->meta_preflight_attempted = true;
        result->meta_preflight_status = common_output_preflight_exact(
            staged->meta_path, staged->meta_bytes, staged->meta_size);
        if (result->meta_preflight_status ==
                COMMON_OUTPUT_PREFLIGHT_COLLISION) {
            result->meta_publish_status = COMMON_OUTPUT_PUBLISH_COLLISION;
        } else if (result->meta_preflight_status ==
                   COMMON_OUTPUT_PREFLIGHT_IO_ERROR) {
            result->meta_publish_status = COMMON_OUTPUT_PUBLISH_IO_ERROR;
        }
    }
    if (result->shader_preflight_status ==
            COMMON_OUTPUT_PREFLIGHT_COLLISION ||
        (staged->emit_shader_meta &&
         result->meta_preflight_status ==
             COMMON_OUTPUT_PREFLIGHT_COLLISION)) {
        mark_failure(context->result, staged->record_index,
                     SHADER_BATCH_FAILURE_OUTPUT_COLLISION);
        return;
    }
    if (result->shader_preflight_status ==
            COMMON_OUTPUT_PREFLIGHT_IO_ERROR ||
        (staged->emit_shader_meta &&
         result->meta_preflight_status ==
             COMMON_OUTPUT_PREFLIGHT_IO_ERROR)) {
        mark_failure(context->result, staged->record_index,
                     SHADER_BATCH_FAILURE_OUTPUT_IO);
        return;
    }

    bool emitted_any = false;
    if (staged->emit_shader_meta) {
        /* Publish the non-importable companion first. A failed first
         * companion can never leave a newly importable .shader behind. */
        result->meta_publish_attempted = true;
        result->meta_publish_status = common_output_publish_exact(
            staged->meta_path, staged->meta_bytes, staged->meta_size);
        if (!publish_status_is_success(result->meta_publish_status)) {
            result->meta_publication_residue =
                common_output_publish_exact_residue_possible(
                    result->meta_preflight_status,
                    result->meta_publish_status, staged->meta_path,
                    staged->meta_bytes, staged->meta_size);
            result->publication_residue =
                result->meta_publication_residue;
            shader_batch_retain_graphics_publication_residue_paths(
                result, &staged->output_path, &staged->meta_path);
            mark_failure(
                context->result, staged->record_index,
                result->meta_publish_status ==
                        COMMON_OUTPUT_PUBLISH_COLLISION
                    ? SHADER_BATCH_FAILURE_OUTPUT_COLLISION
                    : SHADER_BATCH_FAILURE_OUTPUT_IO);
            return;
        }
        emitted_any = result->meta_publish_status ==
            COMMON_OUTPUT_PUBLISH_EMITTED;
    }

    result->shader_publish_attempted = true;
    result->shader_publish_status = common_output_publish_exact(
        staged->output_path, staged->shader_bytes, staged->shader_size);
    if (!publish_status_is_success(result->shader_publish_status)) {
        result->shader_publication_residue =
            common_output_publish_exact_residue_possible(
                result->shader_preflight_status,
                result->shader_publish_status, staged->output_path,
                staged->shader_bytes, staged->shader_size);
        result->meta_publication_residue = staged->emit_shader_meta &&
            result->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED;
        result->publication_residue =
            result->shader_publication_residue ||
            result->meta_publication_residue;
        shader_batch_retain_graphics_publication_residue_paths(
            result, &staged->output_path, &staged->meta_path);
        mark_failure(
            context->result, staged->record_index,
            result->shader_publish_status ==
                    COMMON_OUTPUT_PUBLISH_COLLISION
                ? SHADER_BATCH_FAILURE_OUTPUT_COLLISION
                : SHADER_BATCH_FAILURE_OUTPUT_IO);
        return;
    }
    emitted_any = emitted_any || result->shader_publish_status ==
        COMMON_OUTPUT_PUBLISH_EMITTED;

    result->status = emitted_any ? SHADER_BATCH_EMITTED
                                 : SHADER_BATCH_UNCHANGED;
    result->output_path = staged->output_path;
    staged->output_path = NULL;
    if (staged->emit_shader_meta) {
        result->output_meta_path = staged->meta_path;
        staged->meta_path = NULL;
        result->has_asset_meta = true;
    }
    result->publication_authorized =
        !source_identity_close_deferred;
    if (result->status == SHADER_BATCH_EMITTED) {
        ++context->result->stats.emitted;
    } else {
        ++context->result->stats.unchanged;
    }
}

static CommonOutputPreflightStatus combine_compute_preflight_status(
    CommonOutputPreflightStatus aggregate,
    CommonOutputPreflightStatus status) {
    if (aggregate == COMMON_OUTPUT_PREFLIGHT_COLLISION ||
        status == COMMON_OUTPUT_PREFLIGHT_COLLISION) {
        return COMMON_OUTPUT_PREFLIGHT_COLLISION;
    }
    if (aggregate == COMMON_OUTPUT_PREFLIGHT_IO_ERROR ||
        status == COMMON_OUTPUT_PREFLIGHT_IO_ERROR) {
        return COMMON_OUTPUT_PREFLIGHT_IO_ERROR;
    }
    if (aggregate == COMMON_OUTPUT_PREFLIGHT_MISSING ||
        status == COMMON_OUTPUT_PREFLIGHT_MISSING) {
        return COMMON_OUTPUT_PREFLIGHT_MISSING;
    }
    return COMMON_OUTPUT_PREFLIGHT_UNCHANGED;
}

static bool prepare_compute_publication_ledger(
    ShaderBatchRecordResult* result,
    const StagedShaderPublication* staged) {
    if (staged->compute_artifact_count == 0U ||
        staged->compute_artifact_count >
            SIZE_MAX / sizeof(*result->compute_artifact_publications)) {
        return false;
    }
    result->compute_artifact_publications =
        (ShaderBatchComputeArtifactPublication*)calloc(
            staged->compute_artifact_count,
            sizeof(*result->compute_artifact_publications));
    if (!result->compute_artifact_publications) return false;
    result->compute_artifact_publication_count =
        staged->compute_artifact_count;
    for (size_t index = 0U;
         index < staged->compute_artifact_count; ++index) {
        ShaderBatchComputeArtifactPublication* ledger =
            &result->compute_artifact_publications[index];
        const StagedComputeArtifact* artifact =
            &staged->compute_artifacts[index];
        memcpy(ledger->filename, artifact->filename,
               strlen(artifact->filename) + 1U);
        ledger->is_manifest = artifact->is_manifest;
    }
    return true;
}

static void mark_compute_publication_residues(
    ShaderBatchRecordResult* result, size_t through_index) {
    for (size_t index = 0U; index <= through_index; ++index) {
        ShaderBatchComputeArtifactPublication* ledger =
            &result->compute_artifact_publications[index];
        if (ledger->publish_attempted &&
            ledger->publish_status == COMMON_OUTPUT_PUBLISH_EMITTED) {
            ledger->publication_residue = true;
        }
        result->compute_publication_residue =
            result->compute_publication_residue ||
            ledger->publication_residue;
    }
    result->publication_residue = result->publication_residue ||
        result->compute_publication_residue;
}

static void commit_staged_compute_publication(
    BatchVisitorContext* context, StagedShaderPublication* staged,
    bool source_identity_close_deferred) {
    ShaderBatchRecordResult* result =
        &context->result->records[staged->record_index];
    if (result->status != SHADER_BATCH_PENDING) return;
    result->source_identity_close_deferred =
        source_identity_close_deferred;
    if (!prepare_compute_publication_ledger(result, staged)) {
        mark_failure(context->result, staged->record_index,
                     SHADER_BATCH_FAILURE_OUTPUT_NAME);
        return;
    }
    if (!common_output_ensure_directory_tree(staged->directory_path)) {
        mark_failure(context->result, staged->record_index,
                     SHADER_BATCH_FAILURE_OUTPUT_DIRECTORY);
        return;
    }

    result->compute_preflight_attempted = true;
    result->compute_preflight_status = COMMON_OUTPUT_PREFLIGHT_UNCHANGED;
    for (size_t index = 0U;
         index < staged->compute_artifact_count; ++index) {
        const StagedComputeArtifact* artifact =
            &staged->compute_artifacts[index];
        ShaderBatchComputeArtifactPublication* ledger =
            &result->compute_artifact_publications[index];
        ledger->preflight_attempted = true;
        ledger->preflight_status = common_output_preflight_exact(
            artifact->path, artifact->bytes, artifact->size);
        result->compute_preflight_status =
            combine_compute_preflight_status(
                result->compute_preflight_status,
                ledger->preflight_status);
    }
    if (result->compute_preflight_status ==
            COMMON_OUTPUT_PREFLIGHT_COLLISION) {
        mark_failure(context->result, staged->record_index,
                     SHADER_BATCH_FAILURE_OUTPUT_COLLISION);
        return;
    }
    if (result->compute_preflight_status ==
            COMMON_OUTPUT_PREFLIGHT_IO_ERROR) {
        mark_failure(context->result, staged->record_index,
                     SHADER_BATCH_FAILURE_OUTPUT_IO);
        return;
    }

    bool emitted_any = false;
    result->compute_publish_attempted = true;
    for (size_t index = 0U;
         index < staged->compute_artifact_count; ++index) {
        const StagedComputeArtifact* artifact =
            &staged->compute_artifacts[index];
        ShaderBatchComputeArtifactPublication* ledger =
            &result->compute_artifact_publications[index];
        ledger->publish_attempted = true;
        ledger->publish_status = common_output_publish_exact(
            artifact->path, artifact->bytes, artifact->size);
        if (!publish_status_is_success(ledger->publish_status)) {
            ledger->publication_residue =
                common_output_publish_exact_residue_possible(
                    ledger->preflight_status, ledger->publish_status,
                    artifact->path, artifact->bytes, artifact->size);
            result->compute_publish_status = ledger->publish_status;
            mark_compute_publication_residues(result, index);
            mark_failure(
                context->result, staged->record_index,
                ledger->publish_status ==
                        COMMON_OUTPUT_PUBLISH_COLLISION
                    ? SHADER_BATCH_FAILURE_OUTPUT_COLLISION
                    : SHADER_BATCH_FAILURE_OUTPUT_IO);
            return;
        }
        emitted_any = emitted_any ||
            ledger->publish_status == COMMON_OUTPUT_PUBLISH_EMITTED;
    }

    result->compute_publish_status = emitted_any
        ? COMMON_OUTPUT_PUBLISH_EMITTED
        : COMMON_OUTPUT_PUBLISH_UNCHANGED;
    result->status = emitted_any ? SHADER_BATCH_EMITTED
                                 : SHADER_BATCH_UNCHANGED;
    StagedComputeArtifact* manifest =
        &staged->compute_artifacts[staged->compute_manifest_index];
    result->output_path = manifest->path;
    manifest->path = NULL;
    result->publication_authorized =
        !source_identity_close_deferred;
    if (result->status == SHADER_BATCH_EMITTED) {
        ++context->result->stats.emitted;
    } else {
        ++context->result->stats.unchanged;
    }
}

static void commit_staged_shader_publications(
    BatchVisitorContext* context, bool source_identity_close_deferred) {
    for (size_t index = 0U; index < context->staged_count; ++index) {
        if (context->staged[index].is_compute) {
            commit_staged_compute_publication(
                context, &context->staged[index],
                source_identity_close_deferred);
        } else {
            commit_staged_graphics_publication(
                context, &context->staged[index],
                source_identity_close_deferred);
        }
        staged_shader_publication_dispose(&context->staged[index]);
    }
    context->staged_count = 0U;
}

static bool duplicate_staged_compute_bytes(
    StagedComputeArtifact* staged, const uint8_t* bytes, size_t size) {
    if (!staged || (!bytes && size != 0U) || size == SIZE_MAX) return false;
    staged->bytes = (uint8_t*)malloc(size == 0U ? 1U : size);
    if (!staged->bytes) return false;
    if (size != 0U) memcpy(staged->bytes, bytes, size);
    staged->size = size;
    return true;
}

static bool stage_compute_artifact(
    const ShaderCatalogRecord* catalog_record,
    const ComputeShaderObject* object, size_t record_index,
    BatchVisitorContext* context,
    ShaderBatchRecordResult* result) {
    ComputeShaderArtifactPackage package;
    compute_shader_artifact_package_init(&package);
    result->compute_artifact_status = compute_shader_artifact_build(
        object, &package);
    if (result->compute_artifact_status != COMPUTE_SHADER_ARTIFACT_OK) {
        result->failure = SHADER_BATCH_FAILURE_COMPUTE_ARTIFACT_BUILD;
        compute_shader_artifact_package_dispose(&package);
        return false;
    }
    result->compute_source_authority_status =
        compute_shader_object_source_authority(object);

    StagedShaderPublication staged;
    memset(&staged, 0, sizeof(staged));
    staged.record_index = record_index;
    staged.is_compute = true;
    bool staged_appended = false;
    staged.directory_path = common_output_join_path(
        context->output_directory, catalog_record->serialized_digest_hex);
    if (!staged.directory_path) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_DIRECTORY;
        compute_shader_artifact_package_dispose(&package);
        return false;
    }
    if (package.binary_count == SIZE_MAX) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto compute_stage_done;
    }
    staged.compute_artifact_count = package.binary_count + 1U;
    if (staged.compute_artifact_count >
            SIZE_MAX / sizeof(*staged.compute_artifacts)) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto compute_stage_done;
    }
    staged.compute_artifacts = (StagedComputeArtifact*)calloc(
        staged.compute_artifact_count, sizeof(*staged.compute_artifacts));
    if (!staged.compute_artifacts) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto compute_stage_done;
    }
    for (size_t index = 0U; index < package.binary_count; ++index) {
        const ComputeShaderBinaryArtifact* binary =
            &package.binaries[index];
        StagedComputeArtifact* artifact =
            &staged.compute_artifacts[index];
        memcpy(artifact->filename, binary->filename,
               strlen(binary->filename) + 1U);
        artifact->path = common_output_join_path(
            staged.directory_path, artifact->filename);
        if (!artifact->path ||
            !duplicate_staged_compute_bytes(
                artifact, binary->data, binary->size)) {
            result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
            goto compute_stage_done;
        }
    }
    staged.compute_manifest_index = package.binary_count;
    StagedComputeArtifact* manifest =
        &staged.compute_artifacts[staged.compute_manifest_index];
    memcpy(manifest->filename, package.manifest_filename,
           strlen(package.manifest_filename) + 1U);
    manifest->is_manifest = true;
    manifest->path = common_output_join_path(
        staged.directory_path, manifest->filename);
    if (!manifest->path ||
        !duplicate_staged_compute_bytes(
            manifest, (const uint8_t*)package.manifest.buf,
            package.manifest.len)) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto compute_stage_done;
    }
    if (!append_staged_shader_publication(context, &staged)) {
        result->failure = SHADER_BATCH_FAILURE_OUTPUT_NAME;
        goto compute_stage_done;
    }
    staged_appended = true;

compute_stage_done:
    compute_shader_artifact_package_dispose(&package);
    staged_shader_publication_dispose(&staged);
    return staged_appended;
}

static bool process_source(const UnitySerializedSource* source,
                           void* opaque_context) {
    BatchVisitorContext* context = (BatchVisitorContext*)opaque_context;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE] = {0};
    if (!context->source_digest_prevalidated) {
        common_sha256(source->data, source->size, digest);
    }

    bool has_pending = false;
    for (size_t i = 0U; i < context->catalog->record_count; ++i) {
        const ShaderCatalogRecord* catalog_record =
            &context->catalog->records[i];
        if (context->result->records[i].status == SHADER_BATCH_PENDING &&
            source_matches_record(
                catalog_record, source, digest,
                context->source_digest_prevalidated)) {
            has_pending = true;
            break;
        }
    }
    if (!has_pending) return true;

    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        for (size_t i = 0U; i < context->catalog->record_count; ++i) {
            const ShaderCatalogRecord* record = &context->catalog->records[i];
            if (context->result->records[i].status == SHADER_BATCH_PENDING &&
                source_matches_record(
                    record, source, digest,
                    context->source_digest_prevalidated)) {
                mark_failure(context->result, i,
                             SHADER_BATCH_FAILURE_SERIALIZED_METADATA);
            }
        }
        return true;
    }
    TypeTreeSchemaStatus schema_status =
        serialized_file_resolve_class_schema(
            &file, 48, context->registry);

    for (size_t i = 0U; i < context->catalog->record_count; ++i) {
        const ShaderCatalogRecord* catalog_record =
            &context->catalog->records[i];
        ShaderBatchRecordResult* batch_record =
            &context->result->records[i];
        if (batch_record->status != SHADER_BATCH_PENDING ||
            !source_matches_record(
                catalog_record, source, digest,
                context->source_digest_prevalidated)) {
            continue;
        }
        if (catalog_record->class_id == 48 &&
            schema_status != TYPETREE_SCHEMA_OK) {
            batch_record->schema_status = schema_status;
            mark_failure(context->result, i, SHADER_BATCH_FAILURE_SCHEMA);
            continue;
        }
        const AssetObjectInfo* object = serialized_file_get_object(
            &file, catalog_record->path_id);
        if (!object || object->type_id != catalog_record->class_id) {
            mark_failure(context->result, i,
                         SHADER_BATCH_FAILURE_OBJECT_NOT_FOUND);
            continue;
        }
        if (catalog_record->class_id == 72) {
            ComputeShaderObject decoded_compute;
            compute_shader_object_init(&decoded_compute);
            ComputeShaderObjectStatus compute_status =
                compute_shader_object_decode_borrowed(
                    &decoded_compute, &file, object);
            batch_record->compute_object_status = compute_status;
            if (compute_status != COMPUTE_SHADER_OBJECT_OK) {
                mark_failure(context->result, i,
                    SHADER_BATCH_FAILURE_COMPUTE_OBJECT_DECODE);
            } else if (stage_compute_artifact(
                           catalog_record, &decoded_compute, i,
                           context, batch_record)) {
                /* The package owns copies of every borrowed source byte and
                 * is committed only after source identity close succeeds. */
            } else {
                mark_failure(context->result, i, batch_record->failure);
            }
            compute_shader_object_dispose(&decoded_compute);
            continue;
        }
        if (catalog_record->class_id != 48) {
            mark_failure(context->result, i,
                         SHADER_BATCH_FAILURE_OBJECT_NOT_FOUND);
            continue;
        }
        ShaderObject decoded;
        shader_object_init(&decoded);
        ShaderObjectStatus object_status = shader_object_decode_borrowed(
            &decoded, &file, object);
        if (object_status != SHADER_OBJECT_OK) {
            batch_record->object_status = object_status;
            mark_failure(context->result, i,
                         SHADER_BATCH_FAILURE_OBJECT_DECODE);
            shader_object_dispose(&decoded);
            continue;
        }
        ShaderBlobArchive archive;
        memset(&archive, 0, sizeof(archive));
        object_status = shader_object_open_d3d11_archive(
            &decoded, &archive);
        if (object_status == SHADER_OBJECT_D3D11_PLATFORM_ABSENT) {
            batch_record->status = SHADER_BATCH_UNAVAILABLE;
            batch_record->object_status = object_status;
            ++context->result->stats.unavailable;
        } else if (object_status != SHADER_OBJECT_OK) {
            batch_record->object_status = object_status;
            mark_failure(context->result, i,
                         SHADER_BATCH_FAILURE_D3D11_ARCHIVE);
        } else if (stage_candidate(
                       catalog_record, &decoded, &archive, i,
                       context, batch_record)) {
            /* Publication is committed only by the outer-path owner after
             * visit and source-identity close/suspend succeed. */
        } else {
            mark_failure(context->result, i, batch_record->failure);
        }
        shader_blob_archive_close(&archive);
        shader_object_dispose(&decoded);
    }
    serialized_file_close(&file);
    return true;
}

static bool outer_path_was_visited(const ShaderCatalog* catalog,
                                   const bool* selected,
                                   size_t before_index) {
    for (size_t i = 0U; i < before_index; ++i) {
        if (selected[i] &&
            catalog->records[i].status == SHADER_CATALOG_RECORD_READY &&
            strcmp(catalog->records[i].outer_path,
                   catalog->records[before_index].outer_path) == 0) {
            return true;
        }
    }
    return false;
}

static void revoke_outer_path_results(
    const ShaderCatalog* catalog, const bool* selected,
    const char* outer_path, UnityInputStatus input_status,
    ShaderBatchResult* result) {
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (!selected[i] ||
            strcmp(catalog->records[i].outer_path, outer_path) != 0) {
            continue;
        }
        ShaderBatchRecordResult* record = &result->records[i];
        bool was_failed = record->status == SHADER_BATCH_FAILED;
        switch (record->status) {
            case SHADER_BATCH_EMITTED:
                --result->stats.emitted;
                break;
            case SHADER_BATCH_UNCHANGED:
                --result->stats.unchanged;
                break;
            case SHADER_BATCH_UNAVAILABLE:
                --result->stats.unavailable;
                break;
            default:
                break;
        }
        if (!was_failed) ++result->stats.failed;
        free(record->output_path);
        record->output_path = NULL;
        free(record->output_meta_path);
        record->output_meta_path = NULL;
        record->has_asset_meta = false;
        record->publication_authorized = false;
        record->source_identity_close_deferred = false;
        record->status = SHADER_BATCH_FAILED;
        record->failure = SHADER_BATCH_FAILURE_SOURCE_REOPEN;
        record->input_status = input_status;
    }
}

ShaderBatchStatus shader_batch_extract_ex(
    const ShaderCatalog* catalog, const bool* selected,
    const TypeTreeSchemaRegistry* schema_registry,
    const char* output_directory, const ShaderBatchOptions* options,
    ShaderBatchResult* result) {
    if (!catalog ||
        (catalog->record_count != 0U && !catalog->records) ||
        (catalog->retained_source_snapshot_count != 0U &&
         !catalog->retained_source_snapshots) ||
        (!selected && catalog->record_count != 0U) ||
        !output_directory || !output_directory[0] || !result) {
        return SHADER_BATCH_INVALID_ARGUMENT;
    }
    if (catalog->record_count != 0U &&
        dxbc_size_multiply_overflows(
            catalog->record_count, sizeof(*result->records))) {
        return SHADER_BATCH_ALLOCATION_FAILED;
    }
    ShaderBatchResult pending;
    shader_batch_result_init(&pending);
    pending.catalog_authority = catalog;
    pending.record_count = catalog->record_count;
    if (pending.record_count != 0U) {
        pending.records = (ShaderBatchRecordResult*)calloc(
            pending.record_count, sizeof(*pending.records));
        if (!pending.records) return SHADER_BATCH_ALLOCATION_FAILED;
    }
    for (size_t i = 0U; i < pending.record_count; ++i) {
        memcpy(pending.records[i].catalog_serialized_digest,
               catalog->records[i].serialized_digest,
               sizeof(pending.records[i].catalog_serialized_digest));
        pending.records[i].catalog_path_id = catalog->records[i].path_id;
        pending.records[i].catalog_class_id = catalog->records[i].class_id;
        pending.records[i].catalog_provenance_recorded = true;
        if (!selected[i]) continue;
        ++pending.stats.selected;
        pending.records[i].schema_status = catalog->records[i].schema_status;
        pending.records[i].object_status = catalog->records[i].object_status;
        pending.records[i].compute_object_status =
            catalog->records[i].compute_object_status;
        pending.records[i].compute_source_authority_status =
            catalog->records[i].compute_source_authority_status;
        if (catalog->records[i].status ==
            SHADER_CATALOG_RECORD_D3D11_UNAVAILABLE) {
            pending.records[i].status = SHADER_BATCH_UNAVAILABLE;
            pending.records[i].object_status =
                SHADER_OBJECT_D3D11_PLATFORM_ABSENT;
            ++pending.stats.unavailable;
        } else if (catalog->records[i].status !=
                   SHADER_CATALOG_RECORD_READY) {
            pending.records[i].status = SHADER_BATCH_FAILED;
            pending.records[i].failure =
                SHADER_BATCH_FAILURE_CATALOG_NOT_READY;
            ++pending.stats.failed;
        } else {
            pending.records[i].status = SHADER_BATCH_PENDING;
        }
    }

    ShaderBatchOptions defaults;
    shader_batch_options_default(&defaults);
    if (!options) options = &defaults;
    BatchVisitorContext context = {
        .catalog = catalog,
        .selected = selected,
        .registry = schema_registry,
        .output_directory = output_directory,
        .result = &pending,
        .emit_shader_meta = options->emit_shader_meta,
        .select_candidate = options->select_candidate,
        .candidate_context = options->candidate_context,
        .flat_graphics_output = options->flat_graphics_output,
        .defer_source_snapshot_close =
            options->defer_source_snapshot_close,
    };
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (!selected[i] ||
            pending.records[i].status != SHADER_BATCH_PENDING ||
            outer_path_was_visited(catalog, selected, i)) {
            continue;
        }
        UnityInputVisitStats stats;
        UnityInputSnapshot* snapshot = shader_catalog_retained_snapshot(
            catalog, catalog->records[i].outer_path);
        UnityInputStatus status;
        bool source_identity_close_deferred = false;
        if (snapshot) {
            context.source_digest_prevalidated = true;
            status = unity_input_snapshot_visit(
                snapshot, process_source, &context, &stats);
            if (status == UNITY_INPUT_OK &&
                context.defer_source_snapshot_close) {
                status = unity_input_snapshot_suspend_mapping(snapshot);
                source_identity_close_deferred =
                    status == UNITY_INPUT_OK;
            }
            if (status != UNITY_INPUT_OK ||
                !context.defer_source_snapshot_close) {
                UnityInputStatus close_status =
                    unity_input_snapshot_close(snapshot);
                if (close_status != UNITY_INPUT_OK) status = close_status;
            }
        } else {
            context.source_digest_prevalidated = false;
            status = unity_input_visit_serialized(
                catalog->records[i].outer_path,
                process_source, &context, &stats);
        }
        if (status != UNITY_INPUT_OK) {
            discard_staged_shader_publications(&context);
            revoke_outer_path_results(
                catalog, selected, catalog->records[i].outer_path,
                status, &pending);
        } else {
            commit_staged_shader_publications(
                &context, source_identity_close_deferred);
        }
    }
    discard_staged_shader_publications(&context);
    free(context.staged);
    for (size_t i = 0U; i < pending.record_count; ++i) {
        if (pending.records[i].status == SHADER_BATCH_PENDING) {
            mark_failure(&pending, i,
                         SHADER_BATCH_FAILURE_SOURCE_NOT_FOUND);
        }
    }
    shader_batch_result_dispose(result);
    *result = pending;
    return SHADER_BATCH_OK;
}

ShaderBatchStatus shader_batch_extract(
    const ShaderCatalog* catalog, const bool* selected,
    const TypeTreeSchemaRegistry* schema_registry,
    const char* output_directory, ShaderBatchResult* result) {
    return shader_batch_extract_ex(
        catalog, selected, schema_registry, output_directory, NULL, result);
}

static bool preflight_status_is_ready(CommonOutputPreflightStatus status) {
    return status == COMMON_OUTPUT_PREFLIGHT_MISSING ||
        status == COMMON_OUTPUT_PREFLIGHT_UNCHANGED;
}

static bool successful_record_provenance_matches(
    const ShaderBatchResult* result, size_t index) {
    if (!result || !result->catalog_authority ||
        !result->catalog_authority->records ||
        result->catalog_authority->record_count != result->record_count ||
        index >= result->record_count) {
        return false;
    }
    const ShaderCatalogRecord* catalog_record =
        &result->catalog_authority->records[index];
    const ShaderBatchRecordResult* record = &result->records[index];
    return record->catalog_provenance_recorded &&
        record->catalog_path_id == catalog_record->path_id &&
        record->catalog_class_id == catalog_record->class_id &&
        memcmp(record->catalog_serialized_digest,
               catalog_record->serialized_digest,
               COMMON_SHA256_DIGEST_SIZE) == 0;
}

static bool graphics_publication_ledger_is_complete(
    const ShaderBatchRecordResult* record) {
    if (!record || !record->shader_preflight_attempted ||
        !preflight_status_is_ready(record->shader_preflight_status) ||
        !record->shader_publish_attempted ||
        !publish_status_is_success(record->shader_publish_status) ||
        !record->published_shader_content_recorded) {
        return false;
    }
    bool emitted = record->shader_publish_status ==
        COMMON_OUTPUT_PUBLISH_EMITTED;
    if (record->has_asset_meta) {
        if (!record->output_meta_path ||
            !unity_asset_guid_is_valid(record->asset_guid) ||
            !record->meta_preflight_attempted ||
            !preflight_status_is_ready(record->meta_preflight_status) ||
            !record->meta_publish_attempted ||
            !publish_status_is_success(record->meta_publish_status) ||
            !record->published_meta_content_recorded) {
            return false;
        }
        emitted = emitted || record->meta_publish_status ==
            COMMON_OUTPUT_PUBLISH_EMITTED;
    } else if (record->output_meta_path || record->meta_preflight_attempted ||
               record->meta_publish_attempted ||
               record->published_meta_content_recorded) {
        return false;
    }
    return (record->status == SHADER_BATCH_EMITTED) == emitted;
}

static bool compute_publication_ledger_is_complete(
    const ShaderBatchRecordResult* record) {
    if (!record || record->compute_artifact_status !=
                       COMPUTE_SHADER_ARTIFACT_OK ||
        !record->compute_preflight_attempted ||
        !preflight_status_is_ready(record->compute_preflight_status) ||
        !record->compute_publish_attempted ||
        !publish_status_is_success(record->compute_publish_status) ||
        record->compute_publication_residue ||
        record->compute_artifact_publication_count == 0U ||
        !record->compute_artifact_publications) {
        return false;
    }
    bool preflight_missing = false;
    bool publish_emitted = false;
    size_t manifest_count = 0U;
    for (size_t index = 0U;
         index < record->compute_artifact_publication_count; ++index) {
        const ShaderBatchComputeArtifactPublication* artifact =
            &record->compute_artifact_publications[index];
        if (!memchr(artifact->filename, '\0', sizeof(artifact->filename)) ||
            artifact->filename[0] == '\0' ||
            !artifact->preflight_attempted ||
            !preflight_status_is_ready(artifact->preflight_status) ||
            !artifact->publish_attempted ||
            !publish_status_is_success(artifact->publish_status) ||
            artifact->publication_residue) {
            return false;
        }
        if (artifact->is_manifest) ++manifest_count;
        if (artifact->preflight_status ==
            COMMON_OUTPUT_PREFLIGHT_MISSING) {
            preflight_missing = true;
        }
        if (artifact->publish_status == COMMON_OUTPUT_PUBLISH_EMITTED) {
            publish_emitted = true;
        }
    }
    const CommonOutputPreflightStatus expected_preflight =
        preflight_missing ? COMMON_OUTPUT_PREFLIGHT_MISSING
                          : COMMON_OUTPUT_PREFLIGHT_UNCHANGED;
    const CommonOutputPublishStatus expected_publish =
        publish_emitted ? COMMON_OUTPUT_PUBLISH_EMITTED
                        : COMMON_OUTPUT_PUBLISH_UNCHANGED;
    return manifest_count == 1U &&
        record->compute_preflight_status == expected_preflight &&
        record->compute_publish_status == expected_publish &&
        (record->status == SHADER_BATCH_EMITTED) == publish_emitted;
}

bool shader_batch_is_complete(const ShaderBatchResult* result) {
    if (!result || (result->record_count != 0U && !result->records)) {
        return false;
    }
    size_t emitted = 0U;
    size_t unchanged = 0U;
    size_t unavailable = 0U;
    size_t failed = 0U;
    for (size_t index = 0U; index < result->record_count; ++index) {
        const ShaderBatchRecordResult* record = &result->records[index];
        switch (record->status) {
            case SHADER_BATCH_UNSELECTED:
                break;
            case SHADER_BATCH_EMITTED:
            case SHADER_BATCH_UNCHANGED:
                if (record->failure != SHADER_BATCH_FAILURE_NONE ||
                    !record->publication_authorized ||
                    record->source_identity_close_deferred ||
                    record->publication_residue ||
                    record->shader_publication_residue ||
                    record->meta_publication_residue ||
                    !record->output_path) {
                    return false;
                }
                if (!successful_record_provenance_matches(result, index) ||
                    (record->catalog_class_id == 48
                         ? !graphics_publication_ledger_is_complete(record)
                         : (record->catalog_class_id == 72
                                ? !compute_publication_ledger_is_complete(
                                      record)
                                : true))) {
                    return false;
                }
                if (record->status == SHADER_BATCH_EMITTED) ++emitted;
                else ++unchanged;
                break;
            case SHADER_BATCH_UNAVAILABLE:
                ++unavailable;
                break;
            case SHADER_BATCH_FAILED:
                ++failed;
                break;
            case SHADER_BATCH_PENDING:
            default:
                return false;
        }
    }
    return result->stats.emitted == emitted &&
        result->stats.unchanged == unchanged &&
        result->stats.unavailable == unavailable &&
        result->stats.failed == failed &&
        result->stats.selected ==
            emitted + unchanged + unavailable + failed &&
        unavailable == 0U && failed == 0U;
}

const char* shader_batch_record_status_name(ShaderBatchRecordStatus status) {
    switch (status) {
        case SHADER_BATCH_UNSELECTED: return "unselected";
        case SHADER_BATCH_PENDING: return "pending";
        case SHADER_BATCH_EMITTED: return "emitted";
        case SHADER_BATCH_UNCHANGED: return "unchanged";
        case SHADER_BATCH_UNAVAILABLE: return "unavailable";
        case SHADER_BATCH_FAILED: return "failed";
        default: return "unknown";
    }
}

const char* shader_batch_failure_name(ShaderBatchFailure failure) {
    switch (failure) {
        case SHADER_BATCH_FAILURE_NONE: return "none";
        case SHADER_BATCH_FAILURE_CATALOG_NOT_READY:
            return "catalog-not-ready";
        case SHADER_BATCH_FAILURE_SOURCE_REOPEN: return "source-reopen";
        case SHADER_BATCH_FAILURE_SOURCE_NOT_FOUND:
            return "source-not-found";
        case SHADER_BATCH_FAILURE_SERIALIZED_METADATA:
            return "serialized-metadata";
        case SHADER_BATCH_FAILURE_SCHEMA: return "schema";
        case SHADER_BATCH_FAILURE_OBJECT_NOT_FOUND:
            return "object-not-found";
        case SHADER_BATCH_FAILURE_OBJECT_DECODE: return "object-decode";
        case SHADER_BATCH_FAILURE_COMPUTE_OBJECT_DECODE:
            return "compute-object-decode";
        case SHADER_BATCH_FAILURE_D3D11_ARCHIVE: return "d3d11-archive";
        case SHADER_BATCH_FAILURE_COMPUTE_ARTIFACT_BUILD:
            return "compute-artifact-build";
        case SHADER_BATCH_FAILURE_CANDIDATE_SELECTION:
            return "candidate-selection-failed";
        case SHADER_BATCH_FAILURE_CANDIDATE_EMISSION:
            return "candidate-emission";
        case SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE:
            return "structural-coverage";
        case SHADER_BATCH_FAILURE_OUTPUT_DIRECTORY:
            return "output-directory";
        case SHADER_BATCH_FAILURE_OUTPUT_NAME: return "output-name";
        case SHADER_BATCH_FAILURE_OUTPUT_COLLISION:
            return "output-collision";
        case SHADER_BATCH_FAILURE_OUTPUT_IO: return "output-io";
        case SHADER_BATCH_FAILURE_ASSET_GUID: return "asset-guid";
        case SHADER_BATCH_FAILURE_SHADER_META_EMISSION:
            return "shader-meta-emission";
        default: return "unknown";
    }
}

const char* shader_batch_status_name(ShaderBatchStatus status) {
    switch (status) {
        case SHADER_BATCH_OK: return "ok";
        case SHADER_BATCH_INVALID_ARGUMENT: return "invalid-argument";
        case SHADER_BATCH_ALLOCATION_FAILED: return "allocation-failed";
        default: return "unknown";
    }
}
