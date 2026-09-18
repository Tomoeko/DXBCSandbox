// SPDX-License-Identifier: GPL-3.0-only

#include "app/native_texture_batch.h"

#include "app/shader_catalog_pptr.h"
#include "common/output_publish.h"
#include "common/sha256.h"
#include "common/shader_artifact.h"
#include "common/string_builder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* path;
    CommonFileView view;
} TextureResourceAnchor;

typedef struct {
    ShaderCatalog* catalog;
    NativeTextureBatchResult* result;
    const char* output_directory;
    TextureResourceAnchor* resource_anchors;
    size_t resource_anchor_count;
    bool allocation_failed;
    bool resource_identity_failed;
} TextureVisitContext;

static bool nullable_strings_equal(const char* left, const char* right) {
    return (!left && !right) || (left && right && strcmp(left, right) == 0);
}

static bool source_matches(const ShaderCatalogSource* catalog_source,
                           const UnitySerializedSource* source) {
    return catalog_source && source && catalog_source->outer_path &&
        strcmp(catalog_source->outer_path, source->outer_path) == 0 &&
        nullable_strings_equal(catalog_source->member_name,
                               source->member_name) &&
        catalog_source->member_index == source->member_index &&
        catalog_source->is_bundle_member == source->is_bundle_member;
}

static bool object_has_supported_export_shape(
    const UnityTextureObject* object) {
    return unity_native_texture_yaml_export_shape_is_supported(object);
}

void native_texture_batch_result_init(NativeTextureBatchResult* result) {
    if (result) memset(result, 0, sizeof(*result));
}

void native_texture_batch_result_dispose(NativeTextureBatchResult* result) {
    if (!result) return;
    for (size_t index = 0U; index < result->record_count; ++index) {
        NativeTextureBatchRecordResult* record = &result->records[index];
        free(record->owned_name);
        free(record->owned_stream_path);
        free(record->output_path);
        free(record->output_meta_path);
        free(record->owned_staged_bytes);
        free(record->staging_path);
    }
    free(result->records);
    free(result->staging_directory);
    native_texture_batch_result_init(result);
}

static void record_fail(NativeTextureBatchResult* result, size_t index,
                        NativeTextureBatchFailure failure) {
    NativeTextureBatchRecordResult* record = &result->records[index];
    if (record->status != NATIVE_TEXTURE_BATCH_FAILED) {
        if (record->status == NATIVE_TEXTURE_BATCH_EMITTED ||
            record->status == NATIVE_TEXTURE_BATCH_UNCHANGED) {
            record->publication_residue =
                (record->asset_publish_attempted &&
                 record->asset_publish_status ==
                     COMMON_OUTPUT_PUBLISH_EMITTED) ||
                (record->meta_publish_attempted &&
                 record->meta_publish_status ==
                     COMMON_OUTPUT_PUBLISH_EMITTED);
            if (record->status == NATIVE_TEXTURE_BATCH_EMITTED &&
                result->stats.emitted != 0U) {
                --result->stats.emitted;
            } else if (record->status == NATIVE_TEXTURE_BATCH_UNCHANGED &&
                       result->stats.unchanged != 0U) {
                --result->stats.unchanged;
            }
        }
        record->status = NATIVE_TEXTURE_BATCH_FAILED;
        ++result->stats.failed;
    }
    if (record->failure == NATIVE_TEXTURE_BATCH_FAILURE_NONE) {
        record->failure = failure;
    }
}

static void stage_record(TextureVisitContext* context, size_t record_index,
                         const CommonFileView* resource_view);
static void commit_record(TextureVisitContext* context,
                          size_t record_index);
static void finalize_staged_records(TextureVisitContext* context);

static bool stream_paths_equal(const NativeTextureBatchRecordResult* left,
                               const NativeTextureBatchRecordResult* right) {
    if (!left || !right ||
        left->class_id != UNITY_TEXTURE2D_CLASS_ID ||
        right->class_id != UNITY_TEXTURE2D_CLASS_ID) {
        return false;
    }
    UnityTextureByteView a = left->object.payload.texture2d.stream_path;
    UnityTextureByteView b = right->object.payload.texture2d.stream_path;
    return a.size == b.size &&
        (a.size == 0U || memcmp(a.bytes, b.bytes, a.size) == 0);
}

static bool record_uses_stream(
    const NativeTextureBatchRecordResult* record) {
    return record && record->class_id == UNITY_TEXTURE2D_CLASS_ID &&
        record->object.payload.texture2d.stream_size != 0U;
}

static bool acquire_resource_anchor(TextureVisitContext* context,
                                    char* owned_path,
                                    size_t* anchor_index,
                                    CommonFileStatus* open_status) {
    if (!context || !owned_path || !anchor_index || !open_status) {
        free(owned_path);
        return false;
    }
    for (size_t index = 0U; index < context->resource_anchor_count; ++index) {
        if (strcmp(context->resource_anchors[index].path, owned_path) == 0) {
            free(owned_path);
            *anchor_index = index;
            *open_status = COMMON_FILE_OK;
            return true;
        }
    }
    CommonFileView view = {0};
    *open_status = common_file_view_open_regular(
        owned_path, SIZE_MAX, &view);
    if (*open_status != COMMON_FILE_OK) {
        free(owned_path);
        return false;
    }
    if (context->resource_anchor_count == SIZE_MAX ||
        context->resource_anchor_count + 1U >
            SIZE_MAX / sizeof(*context->resource_anchors)) {
        (void)common_file_view_close(&view);
        free(owned_path);
        context->allocation_failed = true;
        return false;
    }
    size_t count = context->resource_anchor_count + 1U;
    TextureResourceAnchor* anchors =
        (TextureResourceAnchor*)realloc(
            context->resource_anchors, count * sizeof(*anchors));
    if (!anchors) {
        (void)common_file_view_close(&view);
        free(owned_path);
        context->allocation_failed = true;
        return false;
    }
    context->resource_anchors = anchors;
    TextureResourceAnchor* anchor =
        &anchors[context->resource_anchor_count];
    anchor->path = owned_path;
    anchor->view = view;
    *anchor_index = context->resource_anchor_count;
    context->resource_anchor_count = count;
    return true;
}

static void fail_pending_stream_group(TextureVisitContext* context,
                                      size_t source_index,
                                      const NativeTextureBatchRecordResult* key,
                                      NativeTextureBatchFailure failure,
                                      CommonFileStatus resource_status) {
    for (size_t index = 0U; index < context->result->record_count; ++index) {
        NativeTextureBatchRecordResult* record =
            &context->result->records[index];
        if (record->source_index == source_index &&
            record->status == NATIVE_TEXTURE_BATCH_PENDING &&
            stream_paths_equal(record, key)) {
            record->resource_status = resource_status;
            record_fail(context->result, index, failure);
        }
    }
}

static void stage_source_records(TextureVisitContext* context,
                                 size_t source_index) {
    const ShaderCatalogSource* source =
        &context->catalog->sources[source_index];
    for (size_t index = 0U; index < context->result->record_count; ++index) {
        NativeTextureBatchRecordResult* record =
            &context->result->records[index];
        if (record->source_index == source_index &&
            record->status == NATIVE_TEXTURE_BATCH_PENDING &&
            record->object.decoded && !record->stage_complete &&
            !record_uses_stream(record)) {
            stage_record(context, index, NULL);
        }
    }
    for (size_t index = 0U; index < context->result->record_count; ++index) {
        NativeTextureBatchRecordResult* key =
            &context->result->records[index];
        if (key->source_index != source_index ||
            key->status != NATIVE_TEXTURE_BATCH_PENDING ||
            !key->object.decoded || key->stage_complete ||
            !record_uses_stream(key)) {
            continue;
        }
        char* resource_path = native_texture_batch_resolve_loose_resource_path(
            source, key->object.payload.texture2d.stream_path);
        if (!resource_path) {
            fail_pending_stream_group(
                context, source_index, key,
                NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_PATH,
                COMMON_FILE_INVALID_ARGUMENT);
            continue;
        }
        size_t anchor_index = SIZE_MAX;
        CommonFileStatus open_status = COMMON_FILE_INVALID_ARGUMENT;
        if (!acquire_resource_anchor(
                context, resource_path, &anchor_index, &open_status)) {
            fail_pending_stream_group(
                context, source_index, key,
                context->allocation_failed
                    ? NATIVE_TEXTURE_BATCH_FAILURE_COPY_ALLOCATION
                    : NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_OPEN,
                open_status);
            continue;
        }
        const CommonFileView* view =
            &context->resource_anchors[anchor_index].view;
        for (size_t member = 0U;
             member < context->result->record_count; ++member) {
            NativeTextureBatchRecordResult* record =
                &context->result->records[member];
            if (record->source_index == source_index &&
                record->status == NATIVE_TEXTURE_BATCH_PENDING &&
                !record->stage_complete &&
                stream_paths_equal(record, key)) {
                record->resource_status = COMMON_FILE_OK;
                record->resource_anchor_index = anchor_index;
                stage_record(context, member, view);
            }
        }
    }
}

static bool append_unique_target(NativeTextureBatchResult* result,
                                 size_t source_index,
                                 const ShaderCatalogSource* source,
                                 const AssetObjectInfo* object) {
    if (!source) return false;
    for (size_t index = 0U; index < result->record_count; ++index) {
        const NativeTextureBatchRecordResult* existing =
            &result->records[index];
        if (existing->source_index == source_index &&
            existing->path_id == object->path_id) {
            return true;
        }
    }
    if (result->record_count == SIZE_MAX ||
        result->record_count + 1U >
            SIZE_MAX / sizeof(*result->records)) {
        return false;
    }
    size_t count = result->record_count + 1U;
    NativeTextureBatchRecordResult* records =
        (NativeTextureBatchRecordResult*)realloc(
            result->records, count * sizeof(*records));
    if (!records) return false;
    result->records = records;
    NativeTextureBatchRecordResult* record =
        &records[result->record_count];
    memset(record, 0, sizeof(*record));
    record->status = NATIVE_TEXTURE_BATCH_PENDING;
    record->source_index = source_index;
    memcpy(record->source_serialized_digest, source->serialized_digest,
           sizeof(record->source_serialized_digest));
    memcpy(record->source_occurrence_id, source->occurrence_id,
           sizeof(record->source_occurrence_id));
    record->source_occurrence_id[sizeof(record->source_occurrence_id) - 1U] =
        '\0';
    record->class_id = object->type_id;
    record->path_id = object->path_id;
    record->resource_anchor_index = SIZE_MAX;
    record->object_status = UNITY_TEXTURE_OBJECT_NOT_APPLICABLE;
    record->resource_status = COMMON_FILE_OK;
    record->asset_publish_status = COMMON_OUTPUT_PUBLISH_IO_ERROR;
    record->meta_publish_status = COMMON_OUTPUT_PUBLISH_IO_ERROR;
    ++result->record_count;
    ++result->stats.selected;
    if (object->type_id == UNITY_TEXTURE2D_CLASS_ID) {
        ++result->stats.texture2d;
    } else {
        ++result->stats.render_texture;
    }
    return true;
}

static NativeTextureBatchStatus gather_targets(
    const ShaderCatalog* catalog, const bool* selected_shaders,
    NativeTextureBatchResult* result) {
    ShaderCatalogPPtrGraph resolver;
    shader_catalog_pptr_graph_init(&resolver);
    if (!shader_catalog_pptr_graph_build(catalog, &resolver)) {
        return NATIVE_TEXTURE_BATCH_RESOLVER_GRAPH_FAILED;
    }
    bool allocation_failed = false;
    for (size_t material_index = 0U;
         material_index < catalog->material_count && !allocation_failed;
         ++material_index) {
        const ShaderCatalogMaterialRecord* material =
            &catalog->materials[material_index];
        if (material->status != SHADER_CATALOG_MATERIAL_READY ||
            !material->object.decoded ||
            material->shader_record_index >= catalog->record_count ||
            !selected_shaders[material->shader_record_index]) {
            continue;
        }
        for (size_t property_index = 0U;
             property_index < material->object.texture_property_count;
             ++property_index) {
            const AssetPPtr* pointer =
                &material->object.texture_properties[property_index].texture;
            UnityPPtrResolveResult resolution;
            unity_pptr_resolve_result_init(&resolution);
            if (!unity_pptr_resolve(
                    &resolver.graph, material->source_index, pointer,
                    UNITY_PPTR_TARGET_CLASS_ANY, &resolution) ||
                resolution.status == UNITY_PPTR_RESOLVE_NULL ||
                !unity_pptr_resolve_status_is_success(resolution.status) ||
                !resolution.object ||
                resolution.target_index >= catalog->source_count) {
                continue;
            }
            if (resolution.object->type_id != UNITY_TEXTURE2D_CLASS_ID &&
                resolution.object->type_id !=
                    UNITY_RENDER_TEXTURE_CLASS_ID) {
                continue;
            }
            if (!append_unique_target(
                                      result, resolution.target_index,
                                      &catalog->sources[
                                          resolution.target_index],
                                      resolution.object)) {
                allocation_failed = true;
                break;
            }
        }
    }
    shader_catalog_pptr_graph_dispose(&resolver);
    return allocation_failed ? NATIVE_TEXTURE_BATCH_ALLOCATION_FAILED
                             : NATIVE_TEXTURE_BATCH_OK;
}

static bool copy_object_projection(NativeTextureBatchRecordResult* record,
                                   const UnityTextureObject* object) {
    size_t name_capacity = object->name.size + 1U;
    if (name_capacity == 0U) return false;
    uint8_t* name = (uint8_t*)malloc(name_capacity);
    if (!name) return false;
    if (object->name.size != 0U) {
        memcpy(name, object->name.bytes, object->name.size);
    }
    name[object->name.size] = 0U;

    uint8_t* stream_path = NULL;
    size_t stream_path_size = 0U;
    if (object->class_id == UNITY_TEXTURE2D_CLASS_ID) {
        stream_path_size = object->payload.texture2d.stream_path.size;
        if (stream_path_size != 0U) {
            if (stream_path_size == SIZE_MAX) {
                free(name);
                return false;
            }
            stream_path = (uint8_t*)malloc(stream_path_size + 1U);
            if (!stream_path) {
                free(name);
                return false;
            }
            memcpy(stream_path,
                   object->payload.texture2d.stream_path.bytes,
                   stream_path_size);
            stream_path[stream_path_size] = 0U;
        }
    }

    record->object = *object;
    record->owned_name = name;
    record->owned_stream_path = stream_path;
    record->object.name.bytes = name;
    if (record->object.class_id == UNITY_TEXTURE2D_CLASS_ID) {
        UnityTexture2DObject* texture = &record->object.payload.texture2d;
        texture->platform_blob.bytes = NULL;
        texture->inline_image_data.bytes = NULL;
        texture->stream_path.bytes = stream_path;
        texture->stream_path.size = stream_path_size;
    }
    return true;
}

static bool process_texture_source(const UnitySerializedSource* source,
                                   void* opaque) {
    TextureVisitContext* context = (TextureVisitContext*)opaque;
    size_t source_index = SIZE_MAX;
    for (size_t index = 0U; index < context->catalog->source_count; ++index) {
        if (source_matches(&context->catalog->sources[index], source)) {
            source_index = index;
            break;
        }
    }
    if (source_index == SIZE_MAX) return true;

    bool needed = false;
    for (size_t index = 0U; index < context->result->record_count; ++index) {
        if (context->result->records[index].source_index == source_index &&
            context->result->records[index].status ==
                NATIVE_TEXTURE_BATCH_PENDING) {
            needed = true;
            break;
        }
    }
    if (!needed) return true;

    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        for (size_t index = 0U; index < context->result->record_count;
             ++index) {
            if (context->result->records[index].source_index == source_index) {
                record_fail(context->result, index,
                            NATIVE_TEXTURE_BATCH_FAILURE_SERIALIZED_METADATA);
            }
        }
        return true;
    }
    for (size_t index = 0U; index < context->result->record_count; ++index) {
        NativeTextureBatchRecordResult* record =
            &context->result->records[index];
        if (record->source_index != source_index ||
            record->status != NATIVE_TEXTURE_BATCH_PENDING ||
            record->object.decoded) {
            continue;
        }
        const AssetObjectInfo* info = serialized_file_get_object(
            &file, record->path_id);
        if (!info || info->type_id != record->class_id) {
            record_fail(context->result, index,
                        NATIVE_TEXTURE_BATCH_FAILURE_OBJECT_NOT_FOUND);
            continue;
        }
        UnityTextureObject object;
        unity_texture_object_init(&object);
        record->object_status = unity_texture_object_decode_borrowed(
            &object, &file, info);
        if (record->object_status != UNITY_TEXTURE_OBJECT_OK) {
            record_fail(context->result, index,
                        NATIVE_TEXTURE_BATCH_FAILURE_OBJECT_DECODE);
            continue;
        }
        if (!object_has_supported_export_shape(&object)) {
            record_fail(
                context->result, index,
                NATIVE_TEXTURE_BATCH_FAILURE_EXPORT_SHAPE_UNSUPPORTED);
            continue;
        }
        if (!copy_object_projection(record, &object)) {
            record_fail(context->result, index,
                        NATIVE_TEXTURE_BATCH_FAILURE_COPY_ALLOCATION);
            context->allocation_failed = true;
            serialized_file_close(&file);
            return false;
        }
    }
    /* Each distinct sibling path is acquired through the transaction-wide
     * anchor cache. A replacement between any two source/texture records
     * therefore invalidates and revokes the complete resource group. */
    stage_source_records(context, source_index);
    serialized_file_close(&file);
    return true;
}

static bool snapshot_path_is_needed(const ShaderCatalog* catalog,
                                    const NativeTextureBatchResult* result,
                                    const char* outer_path) {
    for (size_t index = 0U; index < result->record_count; ++index) {
        const NativeTextureBatchRecordResult* record = &result->records[index];
        if (record->source_index < catalog->source_count &&
            strcmp(catalog->sources[record->source_index].outer_path,
                   outer_path) == 0) {
            return true;
        }
    }
    return false;
}

static void fail_records_for_outer_path(
    const ShaderCatalog* catalog, NativeTextureBatchResult* result,
    const char* outer_path, NativeTextureBatchFailure failure) {
    for (size_t index = 0U; index < result->record_count; ++index) {
        NativeTextureBatchRecordResult* record = &result->records[index];
        if (record->source_index < catalog->source_count &&
            strcmp(catalog->sources[record->source_index].outer_path,
                   outer_path) == 0) {
            record_fail(result, index, failure);
        }
    }
}

static void close_resource_anchors(TextureVisitContext* context) {
    for (size_t anchor_index = 0U;
         anchor_index < context->resource_anchor_count; ++anchor_index) {
        TextureResourceAnchor* anchor =
            &context->resource_anchors[anchor_index];
        CommonFileStatus close_status = common_file_view_close(&anchor->view);
        for (size_t record_index = 0U;
             record_index < context->result->record_count; ++record_index) {
            NativeTextureBatchRecordResult* record =
                &context->result->records[record_index];
            if (record->resource_anchor_index != anchor_index) continue;
            record->resource_status = close_status;
            if (close_status != COMMON_FILE_OK) {
                record_fail(
                    context->result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_IDENTITY);
            }
        }
        if (close_status != COMMON_FILE_OK) {
            context->resource_identity_failed = true;
        }
        free(anchor->path);
    }
    free(context->resource_anchors);
    context->resource_anchors = NULL;
    context->resource_anchor_count = 0U;
}

static bool catalog_sources_have_open_snapshots(
    const ShaderCatalog* catalog) {
    if (!catalog) return false;
    for (size_t source_index = 0U;
         source_index < catalog->source_count; ++source_index) {
        const char* source_path = catalog->sources[source_index].outer_path;
        bool found = false;
        for (size_t snapshot_index = 0U;
             source_path &&
             snapshot_index < catalog->retained_source_snapshot_count;
             ++snapshot_index) {
            const UnityInputSnapshot* snapshot =
                &catalog->retained_source_snapshots[snapshot_index];
            const char* snapshot_path = unity_input_snapshot_path(snapshot);
            if (unity_input_snapshot_is_open(snapshot) && snapshot_path &&
                strcmp(snapshot_path, source_path) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static void consume_retained_sources(ShaderCatalog* catalog,
                                     const char* output_directory,
                                     NativeTextureBatchResult* result) {
    TextureVisitContext context = {
        .catalog = catalog,
        .result = result,
        .output_directory = output_directory,
    };
    bool transaction_complete = catalog_sources_have_open_snapshots(catalog);
    for (size_t index = 0U;
         index < catalog->retained_source_snapshot_count; ++index) {
        UnityInputSnapshot* snapshot =
            &catalog->retained_source_snapshots[index];
        if (!unity_input_snapshot_is_open(snapshot)) {
            transaction_complete = false;
            continue;
        }
        const char* borrowed_path = unity_input_snapshot_path(snapshot);
        char* path = NULL;
        if (borrowed_path) {
            size_t size = strlen(borrowed_path);
            path = (char*)malloc(size + 1U);
            if (path) memcpy(path, borrowed_path, size + 1U);
            else context.allocation_failed = true;
        } else {
            transaction_complete = false;
        }
        bool needed = !context.allocation_failed && path &&
            snapshot_path_is_needed(catalog, result, path);
        UnityInputStatus status = UNITY_INPUT_OK;
        if (needed) {
            UnityInputVisitStats stats;
            status = unity_input_snapshot_visit(
                snapshot, process_texture_source, &context, &stats);
        }
        UnityInputStatus close_status = unity_input_snapshot_close(snapshot);
        if (status != UNITY_INPUT_OK || close_status != UNITY_INPUT_OK) {
            transaction_complete = false;
            if (needed) {
                fail_records_for_outer_path(
                    catalog, result, path,
                    status == UNITY_INPUT_OK
                        ? NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_IDENTITY
                        : NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_REOPEN);
            } else {
                /* A deferred shader source also belongs to this extraction
                 * transaction.  Its failed close invalidates every pending
                 * dependency rather than exposing tuples from a mixed input
                 * snapshot. */
                for (size_t record_index = 0U;
                     record_index < result->record_count; ++record_index) {
                    record_fail(
                        result, record_index,
                        NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_IDENTITY);
                }
            }
        }
        free(path);
    }
    /* Resource views are cached by absolute path across every owning source.
     * Thus two SerializedFiles sharing one sidecar always read one held file
     * generation, and any mutation/replacement revokes the complete group. */
    close_resource_anchors(&context);
    result->source_transaction_complete =
        transaction_complete && !context.allocation_failed &&
        !context.resource_identity_failed;
    result->source_transaction_failure =
        result->source_transaction_complete
            ? NATIVE_TEXTURE_BATCH_FAILURE_NONE
            : (context.allocation_failed
                   ? NATIVE_TEXTURE_BATCH_FAILURE_COPY_ALLOCATION
                   : (context.resource_identity_failed
                          ? NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_IDENTITY
                          : NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_IDENTITY));
    if (!result->source_transaction_complete) {
        for (size_t index = 0U; index < result->record_count; ++index) {
            if (result->records[index].status ==
                NATIVE_TEXTURE_BATCH_PENDING) {
                record_fail(result, index, result->source_transaction_failure);
            }
        }
    }
    for (size_t index = 0U; index < result->record_count; ++index) {
        NativeTextureBatchRecordResult* record = &result->records[index];
        if (record->status == NATIVE_TEXTURE_BATCH_PENDING &&
            !record->object.decoded) {
            record_fail(result, index,
                        NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_NOT_RETAINED);
        }
    }
    finalize_staged_records(&context);
}

static char* append_suffix(const char* path, const char* suffix) {
    if (!path || !suffix) return NULL;
    size_t path_size = strlen(path);
    size_t suffix_size = strlen(suffix);
    if (path_size > SIZE_MAX - suffix_size - 1U) return NULL;
    char* result = (char*)malloc(path_size + suffix_size + 1U);
    if (!result) return NULL;
    memcpy(result, path, path_size);
    memcpy(result + path_size, suffix, suffix_size + 1U);
    return result;
}

static void store_le64(uint8_t bytes[8], uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void store_le32(uint8_t bytes[4], uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

bool native_texture_batch_derive_asset_identity(
    const uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE],
    int64_t path_id, int32_t class_id,
    const uint8_t resource_digest[COMMON_SHA256_DIGEST_SIZE],
    char identity_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U],
    char guid[UNITY_ASSET_GUID_TEXT_CAPACITY]) {
    if (!serialized_digest || !resource_digest || !identity_hex || !guid) {
        return false;
    }
    uint8_t identity[COMMON_SHA256_DIGEST_SIZE + 8U + 4U +
                     COMMON_SHA256_DIGEST_SIZE];
    size_t offset = 0U;
    memcpy(identity + offset, serialized_digest,
           COMMON_SHA256_DIGEST_SIZE);
    offset += COMMON_SHA256_DIGEST_SIZE;
    store_le64(identity + offset, (uint64_t)path_id);
    offset += 8U;
    store_le32(identity + offset, (uint32_t)class_id);
    offset += 4U;
    memcpy(identity + offset, resource_digest,
           COMMON_SHA256_DIGEST_SIZE);
    offset += COMMON_SHA256_DIGEST_SIZE;
    uint8_t artifact_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(identity, offset, artifact_digest);
    common_sha256_digest_to_hex(artifact_digest, identity_hex);
    return unity_asset_guid_derive(
        UNITY_ASSET_GUID_DOMAIN_TEXTURE, identity, offset,
        guid);
}

static char* duplicate_parent_path(const char* path) {
    if (!path || !path[0]) return NULL;
    const char* last = NULL;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '/'
#ifdef _WIN32
            || *cursor == '\\'
#endif
        ) {
            last = cursor;
        }
    }
    if (!last) {
        char* dot = (char*)malloc(2U);
        if (dot) memcpy(dot, ".", 2U);
        return dot;
    }
    size_t size = (size_t)(last - path);
    if (size == 0U) size = 1U;
    if (size == 2U && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\')) {
        size = 3U;
    }
    char* parent = (char*)malloc(size + 1U);
    if (!parent) return NULL;
    memcpy(parent, path, size);
    parent[size] = '\0';
    return parent;
}

char* native_texture_batch_resolve_loose_resource_path(
    const ShaderCatalogSource* source, UnityTextureByteView stream_path) {
    if (!source || !source->outer_path || source->is_bundle_member ||
        !stream_path.bytes || stream_path.size == 0U ||
        stream_path.size > 255U) {
        return NULL;
    }
    for (size_t index = 0U; index < stream_path.size; ++index) {
        uint8_t value = stream_path.bytes[index];
        if (value == 0U || value == '/' || value == ':'
#ifdef _WIN32
            || value == '\\'
#endif
        ) {
            return NULL;
        }
    }
    if ((stream_path.size == 1U && stream_path.bytes[0] == '.') ||
        (stream_path.size == 2U && stream_path.bytes[0] == '.' &&
         stream_path.bytes[1] == '.')) {
        return NULL;
    }
    char* parent = duplicate_parent_path(source->outer_path);
    if (!parent) return NULL;
    char name[256];
    memcpy(name, stream_path.bytes, stream_path.size);
    name[stream_path.size] = '\0';
    char* result = common_output_join_path(parent, name);
    free(parent);
    return result;
}

static bool stage_bytes_for_record(TextureVisitContext* context,
                                   size_t record_index,
                                   const uint8_t* bytes, size_t size,
                                   NativeTextureBatchRecordResult* record) {
    if (!context || (!bytes && size != 0U) || !record) {
        return false;
    }
    (void)record_index;
    uint8_t* copy = size ? (uint8_t*)malloc(size) : NULL;
    if (size != 0U && !copy) return false;
    if (size != 0U) memcpy(copy, bytes, size);
    record->owned_staged_bytes = copy;
    record->owned_staged_size = size;
    return true;
}

static void stage_record(TextureVisitContext* context, size_t record_index,
                         const CommonFileView* resource_view) {
    NativeTextureBatchResult* result = context->result;
    NativeTextureBatchRecordResult* record = &result->records[record_index];
    const ShaderCatalogSource* source =
        &context->catalog->sources[record->source_index];
    const uint8_t* pixels = NULL;
    size_t pixel_size = 0U;
    if (record->class_id == UNITY_TEXTURE2D_CLASS_ID) {
        const UnityTexture2DObject* texture = &record->object.payload.texture2d;
        pixel_size = texture->stream_size;
        if (pixel_size != 0U) {
            if (!record->owned_stream_path || !resource_view) {
                record_fail(result, record_index,
                            NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_PATH);
                return;
            }
            uint64_t offset = texture->stream_offset;
            if (offset > (uint64_t)resource_view->size ||
                (uint64_t)texture->stream_size >
                    (uint64_t)resource_view->size - offset) {
                record_fail(result, record_index,
                            NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_RANGE);
                return;
            }
            pixels = resource_view->data + (size_t)offset;
        }
    }
    common_sha256(pixels, pixel_size, record->resource_digest);
    common_sha256_digest_to_hex(record->resource_digest,
                                record->resource_digest_hex);
    if (!native_texture_batch_derive_asset_identity(
            source->serialized_digest, record->path_id, record->class_id,
            record->resource_digest, record->artifact_identity_hex,
            record->asset_guid)) {
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_ASSET_GUID);
        return;
    }

    if (pixel_size != 0U) {
        if (!stage_bytes_for_record(
                context, record_index, pixels, pixel_size, record)) {
            record_fail(result, record_index,
                        NATIVE_TEXTURE_BATCH_FAILURE_COPY_ALLOCATION);
            return;
        }
    }
    record->stage_complete = true;
}

static bool cleanup_record_staging(TextureVisitContext* context,
    NativeTextureBatchResult* result,
    NativeTextureBatchRecordResult* record) {
    if (!context || !result || !record) return false;
    free(record->owned_staged_bytes);
    record->owned_staged_bytes = NULL;
    record->owned_staged_size = 0U;
    if (record->staging_path) {
        if (!record->staging_residue) {
            record->staging_residue = true;
            ++result->staging_residue_count;
        }
        result->staging_cleanup_complete = false;
        return false;
    }
    return true;
}

static bool take_staged_record_exact(
    NativeTextureBatchRecordResult* record, size_t expected_size,
    uint8_t** bytes) {
    if (!record || !bytes) return false;
    *bytes = NULL;
    if (record->owned_staged_size != expected_size ||
        (expected_size != 0U && !record->owned_staged_bytes)) {
        return false;
    }
    *bytes = record->owned_staged_bytes;
    record->owned_staged_bytes = NULL;
    record->owned_staged_size = 0U;
    return true;
}

static void commit_record(TextureVisitContext* context,
                          size_t record_index) {
    NativeTextureBatchResult* result = context->result;
    const char* output_directory = context->output_directory;
    NativeTextureBatchRecordResult* record = &result->records[record_index];
    const uint8_t* pixels = NULL;
    size_t pixel_size = 0U;
    uint8_t* staged_bytes = NULL;
    if (record->class_id == UNITY_TEXTURE2D_CLASS_ID) {
        pixel_size = record->object.payload.texture2d.stream_size;
    }
    if (pixel_size != 0U) {
        if (!take_staged_record_exact(
                record, pixel_size, &staged_bytes)) {
            record_fail(result, record_index,
                        NATIVE_TEXTURE_BATCH_FAILURE_STAGING_IDENTITY);
            (void)cleanup_record_staging(context, result, record);
            return;
        }
        uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
        common_sha256(staged_bytes, pixel_size, digest);
        if (memcmp(digest, record->resource_digest, sizeof(digest)) != 0) {
            free(staged_bytes);
            record_fail(result, record_index,
                        NATIVE_TEXTURE_BATCH_FAILURE_STAGING_IDENTITY);
            (void)cleanup_record_staging(context, result, record);
            return;
        }
        pixels = staged_bytes;
    }

    if (pixel_size > (SIZE_MAX - 4096U) / 2U) {
        free(staged_bytes);
        (void)cleanup_record_staging(context, result, record);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_YAML);
        return;
    }
    StringBuilder yaml;
    StringBuilder meta;
    sb_init_with_capacity(&yaml, pixel_size * 2U + 4096U);
    sb_init(&meta);
    record->yaml_attempted = true;
    record->yaml_status = unity_native_texture_yaml_emit(
        &record->object, pixels, pixel_size, &yaml);
    if (record->yaml_status != UNITY_NATIVE_TEXTURE_YAML_OK ||
        !sb_ok(&yaml)) {
        free(staged_bytes);
        (void)cleanup_record_staging(context, result, record);
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_YAML);
        return;
    }
    if (unity_native_texture_meta_emit(
            record->class_id, record->asset_guid, &meta) !=
            UNITY_NATIVE_TEXTURE_YAML_OK || !sb_ok(&meta)) {
        free(staged_bytes);
        (void)cleanup_record_staging(context, result, record);
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_META);
        return;
    }
    record->published_asset_size = yaml.len;
    record->published_meta_size = meta.len;
    common_sha256(yaml.buf, yaml.len, record->published_asset_digest);
    common_sha256(meta.buf, meta.len, record->published_meta_digest);
    record->published_content_recorded = true;

    free(staged_bytes);
    if (!cleanup_record_staging(context, result, record)) {
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_STAGING_CLEANUP);
        return;
    }

    char artifact[384];
    const char* name = record->owned_name && record->owned_name[0]
        ? (const char*)record->owned_name : "_";
    const char* prefix = record->class_id == UNITY_TEXTURE2D_CLASS_ID
        ? "texture2d" : "render_texture";
    const char* extension = record->class_id == UNITY_TEXTURE2D_CLASS_ID
        ? "texture2D" : "renderTexture";
    if (!unity_asset_artifact_filename(
            artifact, sizeof(artifact), prefix, name,
            record->artifact_identity_hex, record->path_id, extension)) {
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_NAME);
        return;
    }
    char* root = common_output_join_path(
        output_directory, "texture-dependencies");
    char* directory = root ? common_output_join_path(
        root, record->artifact_identity_hex) : NULL;
    free(root);
    if (!directory || !common_output_ensure_directory_tree(directory)) {
        free(directory);
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_DIRECTORY);
        return;
    }
    char* asset_path = common_output_join_path(directory, artifact);
    free(directory);
    char* meta_path = append_suffix(asset_path, ".meta");
    if (!asset_path || !meta_path) {
        free(asset_path);
        free(meta_path);
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_NAME);
        return;
    }
    CommonOutputPreflightStatus asset_preflight =
        common_output_preflight_exact(asset_path, yaml.buf, yaml.len);
    CommonOutputPreflightStatus meta_preflight =
        common_output_preflight_exact(meta_path, meta.buf, meta.len);
    if (asset_preflight == COMMON_OUTPUT_PREFLIGHT_COLLISION ||
        meta_preflight == COMMON_OUTPUT_PREFLIGHT_COLLISION) {
        free(asset_path);
        free(meta_path);
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_COLLISION);
        return;
    }
    if (asset_preflight == COMMON_OUTPUT_PREFLIGHT_IO_ERROR ||
        meta_preflight == COMMON_OUTPUT_PREFLIGHT_IO_ERROR) {
        free(asset_path);
        free(meta_path);
        sb_free(&yaml);
        sb_free(&meta);
        record_fail(result, record_index,
                    NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_IO);
        return;
    }
    /* Publish metadata first.  If the second publication loses a race or
     * fails, the residue is a non-importable orphan .meta rather than a
     * Unity asset with missing identity.  The resolver still rejects the
     * record unless both exact publications completed. */
    record->meta_publish_attempted = true;
    record->meta_publish_status = common_output_publish_exact(
        meta_path, meta.buf, meta.len);
    if (record->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        record->meta_publish_status == COMMON_OUTPUT_PUBLISH_UNCHANGED) {
        record->asset_publish_attempted = true;
        record->asset_publish_status = common_output_publish_exact(
            asset_path, yaml.buf, yaml.len);
    }
    bool collision =
        record->asset_publish_status == COMMON_OUTPUT_PUBLISH_COLLISION ||
        record->meta_publish_status == COMMON_OUTPUT_PUBLISH_COLLISION;
    bool io_error =
        record->asset_publish_status == COMMON_OUTPUT_PUBLISH_IO_ERROR ||
        record->meta_publish_status == COMMON_OUTPUT_PUBLISH_IO_ERROR;
    bool asset_ambiguous =
        native_texture_batch_publication_residue_possible(
            record->asset_publish_attempted, asset_preflight,
            record->asset_publish_status, asset_path, yaml.buf, yaml.len);
    bool meta_ambiguous =
        native_texture_batch_publication_residue_possible(
            record->meta_publish_attempted, meta_preflight,
            record->meta_publish_status, meta_path, meta.buf, meta.len);
    sb_free(&yaml);
    sb_free(&meta);
    if (collision || io_error ||
        (record->asset_publish_status != COMMON_OUTPUT_PUBLISH_EMITTED &&
         record->asset_publish_status != COMMON_OUTPUT_PUBLISH_UNCHANGED) ||
        (record->meta_publish_status != COMMON_OUTPUT_PUBLISH_EMITTED &&
         record->meta_publish_status != COMMON_OUTPUT_PUBLISH_UNCHANGED)) {
        if (record->asset_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
            asset_ambiguous) {
            record->publication_residue = true;
            record->output_path = asset_path;
            asset_path = NULL;
        }
        if (record->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
            meta_ambiguous) {
            /* Never unlink by pathname after publication: another actor
             * could replace it before cleanup.  Retain an explicit residue
             * ledger while the resolver rejects this record. */
            record->publication_residue = true;
            record->output_meta_path = meta_path;
            meta_path = NULL;
        }
        free(asset_path);
        free(meta_path);
        record_fail(result, record_index,
                    collision
                        ? NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_COLLISION
                        : NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_IO);
        return;
    }
    record->output_path = asset_path;
    record->output_meta_path = meta_path;
    bool emitted =
        record->asset_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        record->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED;
    record->status = emitted ? NATIVE_TEXTURE_BATCH_EMITTED
                             : NATIVE_TEXTURE_BATCH_UNCHANGED;
    if (emitted) ++result->stats.emitted;
    else ++result->stats.unchanged;
}

static void finalize_staged_records(TextureVisitContext* context) {
    NativeTextureBatchResult* result = context->result;
    result->staging_cleanup_complete = true;
    if (result->source_transaction_complete) {
        for (size_t index = 0U; index < result->record_count; ++index) {
            NativeTextureBatchRecordResult* record = &result->records[index];
            if (record->status != NATIVE_TEXTURE_BATCH_PENDING) continue;
            if (!record->object.decoded || !record->stage_complete) {
                record_fail(result, index,
                            NATIVE_TEXTURE_BATCH_FAILURE_STAGING_IO);
                continue;
            }
            commit_record(context, index);
        }
    }
    for (size_t index = 0U; index < result->record_count; ++index) {
        NativeTextureBatchRecordResult* record = &result->records[index];
        if (!cleanup_record_staging(context, result, record) &&
            record->status != NATIVE_TEXTURE_BATCH_FAILED) {
            record_fail(result, index,
                        NATIVE_TEXTURE_BATCH_FAILURE_STAGING_CLEANUP);
        }
    }
}

typedef struct {
    CommonFileView asset;
    CommonFileView meta;
    bool asset_open;
    bool meta_open;
} NativeTexturePublicationLease;

static bool published_file_view_open_matches(
    const char* path, size_t expected_size,
    const uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE],
    CommonFileView* view) {
    if (!path || !expected_digest || !view) return false;
    CommonFileStatus open_status = common_file_view_open_regular(
        path, expected_size, view);
    if (open_status != COMMON_FILE_OK || view->size != expected_size) {
        if (open_status == COMMON_FILE_OK) {
            (void)common_file_view_close(view);
        }
        return false;
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    bool matches = common_file_view_sha256(view, digest) &&
        memcmp(digest, expected_digest, sizeof(digest)) == 0;
    if (!matches) (void)common_file_view_close(view);
    return matches;
}

static bool record_publication_flags_are_valid(
    const NativeTextureBatchRecordResult* record) {
    if (!record || !record->output_path || !record->output_meta_path ||
        !record->asset_publish_attempted || !record->meta_publish_attempted ||
        !record->published_content_recorded ||
        !unity_asset_guid_is_valid(record->asset_guid) ||
        (record->status != NATIVE_TEXTURE_BATCH_EMITTED &&
         record->status != NATIVE_TEXTURE_BATCH_UNCHANGED)) {
        return false;
    }
    bool asset_ok =
        record->asset_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        record->asset_publish_status == COMMON_OUTPUT_PUBLISH_UNCHANGED;
    bool meta_ok =
        record->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        record->meta_publish_status == COMMON_OUTPUT_PUBLISH_UNCHANGED;
    const bool emitted =
        record->asset_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        record->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED;
    return asset_ok && meta_ok &&
        ((record->status == NATIVE_TEXTURE_BATCH_EMITTED) == emitted);
}

static bool record_asset_identity_is_valid(
    const NativeTextureBatchRecordResult* record,
    const uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE],
    char derived_guid[UNITY_ASSET_GUID_TEXT_CAPACITY]) {
    if (!record || !serialized_digest || !derived_guid) return false;
    char derived_identity[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char derived_resource_digest[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    common_sha256_digest_to_hex(record->resource_digest,
                                derived_resource_digest);
    return native_texture_batch_derive_asset_identity(
               serialized_digest, record->path_id, record->class_id,
               record->resource_digest, derived_identity, derived_guid) &&
        memcmp(record->source_serialized_digest, serialized_digest,
               COMMON_SHA256_DIGEST_SIZE) == 0 &&
        memcmp(record->resource_digest_hex, derived_resource_digest,
               sizeof(record->resource_digest_hex)) == 0 &&
        memcmp(record->artifact_identity_hex, derived_identity,
               sizeof(record->artifact_identity_hex)) == 0 &&
        memcmp(record->asset_guid, derived_guid,
               sizeof(record->asset_guid)) == 0;
}

static NativeTexturePublicationLease* open_publication_lease(
    const NativeTextureBatchRecordResult* record,
    const uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE]) {
    char derived_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    if (!record_publication_flags_are_valid(record) ||
        !record_asset_identity_is_valid(
            record, serialized_digest, derived_guid)) {
        return NULL;
    }
    NativeTexturePublicationLease* lease =
        (NativeTexturePublicationLease*)calloc(1U, sizeof(*lease));
    if (!lease) return NULL;
    lease->asset_open = published_file_view_open_matches(
        record->output_path, record->published_asset_size,
        record->published_asset_digest, &lease->asset);
    if (!lease->asset_open) {
        free(lease);
        return NULL;
    }
    lease->meta_open = published_file_view_open_matches(
        record->output_meta_path, record->published_meta_size,
        record->published_meta_digest, &lease->meta);
    if (!lease->meta_open) {
        (void)common_file_view_close(&lease->asset);
        free(lease);
        return NULL;
    }
    StringBuilder expected_meta;
    sb_init(&expected_meta);
    bool meta_matches = unity_native_texture_meta_emit(
                            record->class_id, derived_guid,
                            &expected_meta) ==
                            UNITY_NATIVE_TEXTURE_YAML_OK &&
        sb_ok(&expected_meta) &&
        expected_meta.len == lease->meta.size &&
        (expected_meta.len == 0U ||
         memcmp(expected_meta.buf, lease->meta.data,
                expected_meta.len) == 0);
    sb_free(&expected_meta);
    if (!meta_matches) {
        (void)common_file_view_close(&lease->meta);
        (void)common_file_view_close(&lease->asset);
        free(lease);
        return NULL;
    }
    return lease;
}

bool native_texture_batch_close_publication_lease(void* opaque) {
    NativeTexturePublicationLease* lease =
        (NativeTexturePublicationLease*)opaque;
    if (!lease) return false;
    bool ok = true;
    if (lease->meta_open &&
        common_file_view_close(&lease->meta) != COMMON_FILE_OK) {
        ok = false;
    }
    if (lease->asset_open &&
        common_file_view_close(&lease->asset) != COMMON_FILE_OK) {
        ok = false;
    }
    free(lease);
    return ok;
}

bool native_texture_batch_record_is_published(
    const NativeTextureBatchRecordResult* record) {
    if (!record) return false;
    NativeTexturePublicationLease* lease = open_publication_lease(
        record, record->source_serialized_digest);
    return lease && native_texture_batch_close_publication_lease(lease);
}

bool native_texture_batch_publication_residue_possible(
    bool publish_attempted,
    CommonOutputPreflightStatus preflight_status,
    CommonOutputPublishStatus publish_status,
    const char* path, const void* data, size_t size) {
    return publish_attempted &&
        common_output_publish_exact_residue_possible(
            preflight_status, publish_status, path, data, size);
}

static bool catalog_material_texture_shapes_are_valid(
    const ShaderCatalog* catalog) {
    if (!catalog ||
        (catalog->material_count != 0U && !catalog->materials)) {
        return false;
    }
    for (size_t index = 0U; index < catalog->material_count; ++index) {
        const MaterialObject* object = &catalog->materials[index].object;
        if (object->texture_property_count != 0U &&
            !object->texture_properties) {
            return false;
        }
    }
    return true;
}

NativeTextureBatchStatus native_texture_batch_export(
    ShaderCatalog* catalog, const bool* selected_shaders,
    const char* output_directory, NativeTextureBatchResult* result) {
    if (!catalog || !catalog->materials_included ||
        (catalog->material_count != 0U && !catalog->materials) ||
        !catalog_material_texture_shapes_are_valid(catalog) ||
        (catalog->retained_source_snapshot_count != 0U &&
         !catalog->retained_source_snapshots) ||
        (catalog->record_count != 0U && !catalog->records) ||
        (!selected_shaders && catalog->record_count != 0U) ||
        !output_directory || output_directory[0] == '\0' || !result) {
        return NATIVE_TEXTURE_BATCH_INVALID_ARGUMENT;
    }
    NativeTextureBatchResult pending;
    native_texture_batch_result_init(&pending);
    pending.catalog_authority = catalog;
    pending.selection_authority = selected_shaders;
    NativeTextureBatchStatus status = gather_targets(
        catalog, selected_shaders, &pending);
    if (status != NATIVE_TEXTURE_BATCH_OK) {
        native_texture_batch_result_dispose(&pending);
        return status;
    }
    consume_retained_sources(catalog, output_directory, &pending);
    native_texture_batch_result_dispose(result);
    *result = pending;
    return NATIVE_TEXTURE_BATCH_OK;
}

static bool catalog_source_owns_object(
    const ShaderCatalogSource* source, const AssetObjectInfo* object) {
    if (!source || !object ||
        (source->object_reference_count != 0U && !source->objects)) {
        return false;
    }
    for (size_t index = 0U; index < source->object_reference_count;
         ++index) {
        if (&source->objects[index] == object) return true;
    }
    return false;
}

static bool reference_request_matches_canonical_material(
    const MaterialBatchTextureReferenceRequest* request,
    UnityPPtrResolveResult* canonical_resolution) {
    if (!request || !canonical_resolution || !request->catalog ||
        !request->catalog->materials_included ||
        (request->catalog->material_count != 0U &&
         !request->catalog->materials) ||
        request->material_index >= request->catalog->material_count) {
        return false;
    }
    const ShaderCatalogMaterialRecord* material =
        &request->catalog->materials[request->material_index];
    if (material->status != SHADER_CATALOG_MATERIAL_READY ||
        !material->object.decoded ||
        material->source_index >= request->catalog->source_count ||
        request->source_index != material->source_index ||
        (material->object.texture_property_count != 0U &&
         !material->object.texture_properties) ||
        request->texture_property_index >=
            material->object.texture_property_count) {
        return false;
    }
    const AssetPPtr* canonical_pointer =
        &material->object
             .texture_properties[request->texture_property_index]
             .texture;
    if (request->serialized_pointer != canonical_pointer) return false;

    ShaderCatalogPPtrGraph graph;
    shader_catalog_pptr_graph_init(&graph);
    bool graph_built = shader_catalog_pptr_graph_build(
        request->catalog, &graph);
    bool resolved = graph_built && unity_pptr_resolve(
        &graph.graph, material->source_index, canonical_pointer,
        UNITY_PPTR_TARGET_CLASS_ANY, canonical_resolution);
    shader_catalog_pptr_graph_dispose(&graph);
    return resolved &&
        unity_pptr_resolve_status_is_success(
            canonical_resolution->status) &&
        canonical_resolution->object &&
        canonical_resolution->target_index <
            request->catalog->source_count;
}

MaterialBatchTextureReferenceStatus native_texture_batch_resolve_reference(
    const MaterialBatchTextureReferenceRequest* request, void* context,
    MaterialBatchTextureReference* reference) {
    const NativeTextureBatchResult* result =
        (const NativeTextureBatchResult*)context;
    if (!reference) {
        return MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED;
    }
    material_batch_texture_reference_init(reference);
    UnityPPtrResolveResult canonical_resolution;
    unity_pptr_resolve_result_init(&canonical_resolution);
    if (!request || !result || !request->catalog ||
        result->catalog_authority != request->catalog ||
        (result->record_count != 0U && !result->records) ||
        (request->catalog->source_count != 0U &&
         !request->catalog->sources) ||
        !request->target_source || !request->target_object ||
        !request->serialized_pointer || !request->resolution ||
        !unity_pptr_resolve_status_is_success(
            request->resolution->status) ||
        !reference_request_matches_canonical_material(
            request, &canonical_resolution) ||
        request->resolution->status != canonical_resolution.status ||
        request->resolution->source_index !=
            canonical_resolution.source_index ||
        request->resolution->target_index !=
            canonical_resolution.target_index ||
        request->resolution->external_index !=
            canonical_resolution.external_index ||
        request->resolution->external != canonical_resolution.external ||
        request->resolution->object != canonical_resolution.object ||
        request->resolution->source_index != request->source_index ||
        request->target_source !=
            &request->catalog->sources[
                canonical_resolution.target_index] ||
        request->target_object != canonical_resolution.object ||
        request->resolution->object != request->target_object ||
        !catalog_source_owns_object(
            request->target_source, request->target_object)) {
        return MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED;
    }
    if (!result->source_transaction_complete ||
        !result->staging_cleanup_complete) {
        return MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED;
    }
    int32_t class_id = request->target_object->type_id;
    if (class_id != UNITY_TEXTURE2D_CLASS_ID &&
        class_id != UNITY_RENDER_TEXTURE_CLASS_ID) {
        return MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_CLASS_UNSUPPORTED;
    }
    for (size_t index = 0U; index < result->record_count; ++index) {
        const NativeTextureBatchRecordResult* record = &result->records[index];
        if (record->source_index != request->resolution->target_index ||
            record->path_id != request->target_object->path_id ||
            record->class_id != class_id ||
            memcmp(record->source_serialized_digest,
                   request->target_source->serialized_digest,
                   sizeof(record->source_serialized_digest)) != 0 ||
            memcmp(record->source_occurrence_id,
                   request->target_source->occurrence_id,
                   sizeof(record->source_occurrence_id)) != 0) {
            continue;
        }
        NativeTexturePublicationLease* lease =
            open_publication_lease(
                record, request->target_source->serialized_digest);
        if (!lease) {
            return MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED;
        }
        reference->file_id = class_id == UNITY_TEXTURE2D_CLASS_ID
            ? UNITY_TEXTURE2D_LOCAL_FILE_ID
            : UNITY_RENDER_TEXTURE_LOCAL_FILE_ID;
        memcpy(reference->guid, record->asset_guid,
               sizeof(reference->guid));
        reference->type = 2;
        reference->asset_exported = true;
        reference->publication_lease = lease;
        return MATERIAL_BATCH_TEXTURE_REFERENCE_OK;
    }
    return MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED;
}

static bool target_population_matches_selection_authority(
    const NativeTextureBatchResult* result) {
    NativeTextureBatchResult expected;
    native_texture_batch_result_init(&expected);
    NativeTextureBatchStatus status = gather_targets(
        result->catalog_authority, result->selection_authority, &expected);
    bool matches = status == NATIVE_TEXTURE_BATCH_OK &&
        expected.record_count == result->record_count;
    for (size_t index = 0U; matches && index < expected.record_count;
         ++index) {
        const NativeTextureBatchRecordResult* expected_record =
            &expected.records[index];
        const NativeTextureBatchRecordResult* record =
            &result->records[index];
        matches = expected_record->source_index == record->source_index &&
            expected_record->class_id == record->class_id &&
            expected_record->path_id == record->path_id &&
            memcmp(expected_record->source_serialized_digest,
                   record->source_serialized_digest,
                   sizeof(record->source_serialized_digest)) == 0 &&
            memcmp(expected_record->source_occurrence_id,
                   record->source_occurrence_id,
                   sizeof(record->source_occurrence_id)) == 0;
    }
    native_texture_batch_result_dispose(&expected);
    return matches;
}

bool native_texture_batch_validate_publication_snapshot(
    const NativeTextureBatchResult* result, bool* record_publications,
    size_t record_count) {
    if (!result || !result->catalog_authority ||
        !result->catalog_authority->materials_included ||
        !catalog_material_texture_shapes_are_valid(
            result->catalog_authority) ||
        (result->catalog_authority->record_count != 0U &&
         (!result->catalog_authority->records ||
          !result->selection_authority)) ||
        (result->catalog_authority->source_count != 0U &&
         !result->catalog_authority->sources) ||
        (result->record_count != 0U && !result->records) ||
        (record_publications && record_count != result->record_count) ||
        (!record_publications && record_count != 0U) ||
        result->staging_directory) {
        return false;
    }
    if (record_publications && record_count != 0U) {
        memset(record_publications, 0,
               record_count * sizeof(*record_publications));
    }
    if (!target_population_matches_selection_authority(result)) {
        return false;
    }
    size_t emitted = 0U;
    size_t unchanged = 0U;
    size_t failed = 0U;
    size_t texture2d = 0U;
    size_t render_texture = 0U;
    size_t staging_residues = 0U;
    bool records_complete = true;
    for (size_t index = 0U; index < result->record_count; ++index) {
        const NativeTextureBatchRecordResult* record =
            &result->records[index];
        bool published = false;
        const ShaderCatalogSource* source = NULL;
        bool source_object_found = false;
        if (record->source_index <
            result->catalog_authority->source_count) {
            source = &result->catalog_authority
                          ->sources[record->source_index];
            if ((source->object_reference_count == 0U || source->objects) &&
                memcmp(record->source_serialized_digest,
                       source->serialized_digest,
                       sizeof(record->source_serialized_digest)) == 0 &&
                memcmp(record->source_occurrence_id, source->occurrence_id,
                       sizeof(record->source_occurrence_id)) == 0) {
                for (size_t object_index = 0U;
                     object_index < source->object_reference_count;
                     ++object_index) {
                    if (source->objects[object_index].type_id ==
                            record->class_id &&
                        source->objects[object_index].path_id ==
                            record->path_id) {
                        source_object_found = true;
                        break;
                    }
                }
            }
        }
        bool owned_projection_valid = source_object_found &&
            record->object_status == UNITY_TEXTURE_OBJECT_OK &&
            record->object.decoded &&
            record->object.class_id == record->class_id &&
            record->object.path_id == record->path_id &&
            record->owned_name &&
            record->object.name.bytes == record->owned_name &&
            unity_native_texture_yaml_export_shape_is_supported(
                &record->object) &&
            record->yaml_attempted &&
            record->yaml_status == UNITY_NATIVE_TEXTURE_YAML_OK &&
            record->resource_status == COMMON_FILE_OK &&
            !record->owned_staged_bytes &&
            record->owned_staged_size == 0U && !record->staging_path;
        if (record->class_id == UNITY_TEXTURE2D_CLASS_ID) ++texture2d;
        else if (record->class_id == UNITY_RENDER_TEXTURE_CLASS_ID) {
            ++render_texture;
        } else {
            records_complete = false;
        }
        if (record->staging_residue) ++staging_residues;
        switch (record->status) {
            case NATIVE_TEXTURE_BATCH_EMITTED:
            case NATIVE_TEXTURE_BATCH_UNCHANGED:
                if (record->class_id == UNITY_TEXTURE2D_CLASS_ID) {
                    const UnityTexture2DObject* texture =
                        &record->object.payload.texture2d;
                    owned_projection_valid = owned_projection_valid &&
                        texture->stream_path.bytes ==
                            record->owned_stream_path &&
                        ((texture->stream_path.size == 0U) ==
                         (record->owned_stream_path == NULL)) &&
                        ((texture->stream_size == 0U) ==
                         (record->resource_anchor_index == SIZE_MAX));
                } else if (record->class_id ==
                           UNITY_RENDER_TEXTURE_CLASS_ID) {
                    owned_projection_valid = owned_projection_valid &&
                        !record->owned_stream_path &&
                        record->resource_anchor_index == SIZE_MAX;
                }
                published = owned_projection_valid &&
                    native_texture_batch_record_is_published(record);
                if (record->failure != NATIVE_TEXTURE_BATCH_FAILURE_NONE ||
                    !record->stage_complete || record->staging_residue ||
                    record->publication_residue || !published) {
                    records_complete = false;
                }
                if (record->status == NATIVE_TEXTURE_BATCH_EMITTED) ++emitted;
                else ++unchanged;
                break;
            case NATIVE_TEXTURE_BATCH_FAILED:
                ++failed;
                records_complete = false;
                break;
            case NATIVE_TEXTURE_BATCH_UNSELECTED:
            case NATIVE_TEXTURE_BATCH_PENDING:
            default:
                records_complete = false;
                break;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (result->records[prior].source_index == record->source_index &&
                result->records[prior].path_id == record->path_id) {
                records_complete = false;
            }
        }
        if (record_publications) record_publications[index] = published;
    }
    return records_complete && result->source_transaction_complete &&
        result->source_transaction_failure ==
            NATIVE_TEXTURE_BATCH_FAILURE_NONE &&
        result->staging_cleanup_complete &&
        result->staging_residue_count == staging_residues &&
        staging_residues == 0U && result->record_count == result->stats.selected &&
        result->stats.texture2d == texture2d &&
        result->stats.render_texture == render_texture &&
        result->stats.emitted == emitted &&
        result->stats.unchanged == unchanged &&
        result->stats.failed == failed && failed == 0U &&
        result->stats.selected == emitted + unchanged;
}

bool native_texture_batch_is_complete(const NativeTextureBatchResult* result) {
    return native_texture_batch_validate_publication_snapshot(
        result, NULL, 0U);
}

const char* native_texture_batch_record_status_name(
    NativeTextureBatchRecordStatus status) {
    switch (status) {
        case NATIVE_TEXTURE_BATCH_UNSELECTED: return "unselected";
        case NATIVE_TEXTURE_BATCH_PENDING: return "pending";
        case NATIVE_TEXTURE_BATCH_EMITTED: return "emitted";
        case NATIVE_TEXTURE_BATCH_UNCHANGED: return "unchanged";
        case NATIVE_TEXTURE_BATCH_FAILED: return "failed";
    }
    return "unknown";
}

const char* native_texture_batch_failure_name(
    NativeTextureBatchFailure failure) {
    switch (failure) {
        case NATIVE_TEXTURE_BATCH_FAILURE_NONE: return "none";
        case NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_NOT_RETAINED:
            return "source-not-retained";
        case NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_REOPEN:
            return "source-reopen";
        case NATIVE_TEXTURE_BATCH_FAILURE_SOURCE_IDENTITY:
            return "source-identity";
        case NATIVE_TEXTURE_BATCH_FAILURE_SERIALIZED_METADATA:
            return "serialized-metadata";
        case NATIVE_TEXTURE_BATCH_FAILURE_OBJECT_NOT_FOUND:
            return "object-not-found";
        case NATIVE_TEXTURE_BATCH_FAILURE_OBJECT_DECODE:
            return "object-decode";
        case NATIVE_TEXTURE_BATCH_FAILURE_EXPORT_SHAPE_UNSUPPORTED:
            return "export-shape-unsupported";
        case NATIVE_TEXTURE_BATCH_FAILURE_COPY_ALLOCATION:
            return "copy-allocation";
        case NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_PATH:
            return "resource-path";
        case NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_OPEN:
            return "resource-open";
        case NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_RANGE:
            return "resource-range";
        case NATIVE_TEXTURE_BATCH_FAILURE_RESOURCE_IDENTITY:
            return "resource-identity";
        case NATIVE_TEXTURE_BATCH_FAILURE_STAGING_DIRECTORY:
            return "staging-directory";
        case NATIVE_TEXTURE_BATCH_FAILURE_STAGING_IO:
            return "staging-io";
        case NATIVE_TEXTURE_BATCH_FAILURE_STAGING_IDENTITY:
            return "staging-identity";
        case NATIVE_TEXTURE_BATCH_FAILURE_STAGING_CLEANUP:
            return "staging-cleanup";
        case NATIVE_TEXTURE_BATCH_FAILURE_ASSET_GUID: return "asset-guid";
        case NATIVE_TEXTURE_BATCH_FAILURE_YAML: return "yaml";
        case NATIVE_TEXTURE_BATCH_FAILURE_META: return "meta";
        case NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_DIRECTORY:
            return "output-directory";
        case NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_NAME: return "output-name";
        case NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_COLLISION:
            return "output-collision";
        case NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_IO: return "output-io";
    }
    return "unknown";
}

const char* native_texture_batch_status_name(NativeTextureBatchStatus status) {
    switch (status) {
        case NATIVE_TEXTURE_BATCH_OK: return "ok";
        case NATIVE_TEXTURE_BATCH_INVALID_ARGUMENT: return "invalid-argument";
        case NATIVE_TEXTURE_BATCH_ALLOCATION_FAILED:
            return "allocation-failed";
        case NATIVE_TEXTURE_BATCH_RESOLVER_GRAPH_FAILED:
            return "resolver-graph-failed";
    }
    return "unknown";
}
