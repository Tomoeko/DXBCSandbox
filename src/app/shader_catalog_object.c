// SPDX-License-Identifier: GPL-3.0-only

#include "app/shader_catalog_object.h"
#include "app/shader_catalog_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const ShaderCatalogRecord *record;
    const TypeTreeSchemaRegistry *registry;
    ShaderObject object;
    ShaderCatalogObjectReport report;
    bool coordinates_match;
} DecodeContext;

static bool same_string(const char *left, const char *right) {
    return left && right ? strcmp(left, right) == 0 : left == right;
}

static bool source_matches_record(const UnitySerializedSource *source,
                                  const ShaderCatalogRecord *record) {
    if (!same_string(source->outer_path, record->outer_path) ||
        !same_string(source->member_name, record->member_name) ||
        source->member_index != record->member_index ||
        source->is_bundle_member != record->is_bundle_member)
        return false;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source->data, source->size, digest);
    if (memcmp(digest, record->serialized_digest, sizeof(digest)) != 0)
        return false;
    char serialized_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char occurrence_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char content_id[sizeof(record->content_id)];
    char occurrence_id[sizeof(record->occurrence_id)];
    common_sha256_digest_to_hex(digest, serialized_hex);
    shader_catalog_source_occurrence_digest(source, digest, digest);
    common_sha256_digest_to_hex(digest, occurrence_hex);
    int content_size =
        snprintf(content_id, sizeof(content_id), "s:%s:%" PRId64, serialized_hex, record->path_id);
    int occurrence_size = snprintf(occurrence_id, sizeof(occurrence_id), "o:%s:%" PRId64,
                                   occurrence_hex, record->path_id);
    return content_size > 0 && (size_t)content_size < sizeof(content_id) && occurrence_size > 0 &&
           (size_t)occurrence_size < sizeof(occurrence_id) &&
           memcmp(serialized_hex, record->serialized_digest_hex, sizeof(serialized_hex)) == 0 &&
           memcmp(occurrence_hex, record->occurrence_digest_hex, sizeof(occurrence_hex)) == 0 &&
           strncmp(content_id, record->content_id, sizeof(content_id)) == 0 &&
           strncmp(occurrence_id, record->occurrence_id, sizeof(occurrence_id)) == 0;
}

static bool decode_source(const UnitySerializedSource *source, void *opaque) {
    DecodeContext *context = opaque;
    const ShaderCatalogRecord *record = context->record;
    if (!source_matches_record(source, record))
        return true;
    if (++context->report.source_matches != 1U)
        return true;

    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        context->report.object_status = SHADER_OBJECT_INVALID_SOURCE_FILE;
        return true;
    }
    const AssetObjectInfo *asset = serialized_file_get_object(&file, record->path_id);
    context->coordinates_match = asset && asset->type_id == record->class_id &&
                                 asset->byte_size == record->object_size &&
                                 file.target_platform == record->target_platform &&
                                 same_string(file.unity_version, record->unity_version);
    if (context->coordinates_match) {
        context->report.schema_status =
            serialized_file_resolve_class_schema(&file, 48, context->registry);
        if (context->report.schema_status == TYPETREE_SCHEMA_OK) {
            context->report.object_status = shader_object_decode(&context->object, &file, asset);
            if (context->report.object_status == SHADER_OBJECT_OK) {
                /* Decode already checked both additions against the source bounds. */
                common_sha256(source->data + file.data_offset + asset->byte_offset,
                              asset->byte_size, context->report.payload_digest);
                if (!typetree_schema_shape_digest(&context->object.schema,
                                                  context->report.schema_digest)) {
                    context->report.schema_status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
                }
            }
        }
    }
    serialized_file_close(&file);
    return true;
}

ShaderCatalogObjectStatus shader_catalog_decode_object(const ShaderCatalog *catalog,
                                                       const ShaderCatalogRecord *record,
                                                       const TypeTreeSchemaRegistry *registry,
                                                       ShaderObject *destination,
                                                       ShaderCatalogObjectReport *report) {
    if (!report)
        return SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->source_status = UNITY_INPUT_INVALID_ARGUMENT;
    report->schema_status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
    report->object_status = SHADER_OBJECT_NOT_DECODED;
    if (!catalog || !record || !destination)
        return SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    bool owned = false;
    for (size_t index = 0U; index < catalog->record_count; ++index) {
        if (record == &catalog->records[index])
            owned = true;
    }
    if (!owned)
        return SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED;
    if (record->class_id != 48)
        return SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    UnityInputSnapshot *snapshot = shader_catalog_retained_snapshot(catalog, record->outer_path);
    if (!snapshot)
        return SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE;

    DecodeContext context = {0};
    context.record = record;
    context.registry = registry;
    context.report = *report;
    shader_object_init(&context.object);
    UnityInputVisitStats stats;
    context.report.source_status =
        unity_input_snapshot_visit(snapshot, decode_source, &context, &stats);
    ShaderCatalogObjectStatus status;
    if (context.report.source_status != UNITY_INPUT_OK) {
        status = SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE;
    } else if (context.report.source_matches != 1U || !context.coordinates_match) {
        status = SHADER_CATALOG_OBJECT_COORDINATE_MISMATCH;
    } else if (context.report.schema_status != TYPETREE_SCHEMA_OK) {
        status = SHADER_CATALOG_OBJECT_SCHEMA_UNAVAILABLE;
    } else if (context.report.object_status != SHADER_OBJECT_OK) {
        status = SHADER_CATALOG_OBJECT_DECODE_FAILED;
    } else {
        shader_object_dispose(destination);
        *destination = context.object;
        shader_object_init(&context.object);
        status = SHADER_CATALOG_OBJECT_OK;
    }
    if (status != SHADER_CATALOG_OBJECT_OK) {
        memset(context.report.payload_digest, 0, sizeof(context.report.payload_digest));
        memset(context.report.schema_digest, 0, sizeof(context.report.schema_digest));
    }
    *report = context.report;
    shader_object_dispose(&context.object);
    return status;
}
