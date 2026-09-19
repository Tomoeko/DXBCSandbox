// SPDX-License-Identifier: GPL-3.0-only

#ifndef TEST_SHADER_SUBJECT_H
#define TEST_SHADER_SUBJECT_H

#include "app/shader_catalog_object.h"
#include "app/whole_shader_subject.h"
#include <stdio.h>
#include <string.h>

/* Bind actual captured object coordinates for producer integration tests.
 * Unrelated compiler/player/scope authorities deliberately remain absent. */
static inline bool test_shader_subject_from_catalogs(const ShaderCatalog *target,
                                                     const ShaderCatalog *candidate,
                                                     const TypeTreeSchemaRegistry *registry,
                                                     WholeShaderSubjectDescriptor *descriptor,
                                                     ShaderCatalogObjectReport reports[2]) {
    if (!target || !candidate || target->record_count != 1U || candidate->record_count != 1U ||
        !descriptor || !reports)
        return false;
    memset(descriptor, 0, sizeof(*descriptor));
    ShaderObject object;
    shader_object_init(&object);
    bool decoded = shader_catalog_decode_object(target, target->records, registry, &object,
                                                &reports[0]) == SHADER_CATALOG_OBJECT_OK &&
                   shader_catalog_decode_object(candidate, candidate->records, registry, &object,
                                                &reports[1]) == SHADER_CATALOG_OBJECT_OK;
    shader_object_dispose(&object);
    if (!decoded)
        return false;
    const ShaderCatalogRecord *record = target->records;
    descriptor->target_shader_path_id = record->path_id;
    descriptor->target_class_id = 48;
    descriptor->serialized_target_platform = record->target_platform;
    descriptor->build_platform = record->target_platform;
    descriptor->compiler_platform = 4;
    descriptor->graphics_api = 2U;
    descriptor->source_residency = record->is_bundle_member
                                       ? WHOLE_SHADER_SOURCE_BUNDLE_MEMBER
                                       : WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE;
    descriptor->target_member_index = record->member_index;
    descriptor->target_member_identity = record->member_name ? record->member_name : "";
    descriptor->candidate_logical_name = candidate->records[0].name;
    descriptor->unity_version = record->unity_version;
    for (unsigned index = 0U; index < 32U; ++index) {
        unsigned byte;
        if (sscanf(record->occurrence_digest_hex + index * 2U, "%2x", &byte) != 1)
            return false;
        descriptor->target_occurrence_digest[index] = (uint8_t)byte;
    }
    memcpy(descriptor->target_serialized_file_digest, record->serialized_digest, 32U);
    memcpy(descriptor->target_object_payload_digest, reports[0].payload_digest, 32U);
    memcpy(descriptor->schema_authority_digest, reports[0].schema_digest, 32U);
    memcpy(descriptor->candidate_release_digest, reports[1].release_digest, 32U);
    return true;
}

#endif
