// SPDX-License-Identifier: GPL-3.0-only

#include "app/release_shader_evidence.h"

#include <string.h>

static const char producer_domain[] = "DXBCSandbox.Reextraction.ByteBound.v1";

static bool subject_matches(const WholeShaderSubjectDescriptor *subject,
                            const ShaderCatalogRecord *target, const ShaderCatalogRecord *candidate,
                            const ShaderObject *candidate_object,
                            const ReleaseShaderEvidenceReport *report) {
    char occurrence[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(subject->target_occurrence_digest, occurrence);
    const char *member = target->member_name ? target->member_name : "";
    return subject->target_shader_path_id == target->path_id &&
           subject->target_class_id == target->class_id &&
           subject->serialized_target_platform == target->target_platform &&
           subject->build_platform == target->target_platform &&
           subject->source_residency == (target->is_bundle_member
                                             ? WHOLE_SHADER_SOURCE_BUNDLE_MEMBER
                                             : WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE) &&
           subject->target_member_index == target->member_index &&
           strcmp(subject->target_member_identity, member) == 0 &&
           strcmp(subject->unity_version, target->unity_version) == 0 &&
           strcmp(subject->unity_version, candidate->unity_version) == 0 &&
           candidate->target_platform == subject->serialized_target_platform &&
           candidate_object->shader.name &&
           strcmp(subject->candidate_logical_name, candidate_object->shader.name) == 0 &&
           strcmp(occurrence, target->occurrence_digest_hex) == 0 &&
           memcmp(subject->target_serialized_file_digest, target->serialized_digest, 32U) == 0 &&
           memcmp(subject->target_object_payload_digest, report->target.payload_digest, 32U) == 0 &&
           memcmp(subject->schema_authority_digest, report->target.schema_digest, 32U) == 0 &&
           memcmp(subject->candidate_release_digest, report->candidate.release_digest, 32U) == 0;
}

static void authority_digest(const ReleaseShaderEvidenceReport *report, uint8_t digest[32]) {
    CommonSha256Context hash;
    common_sha256_init(&hash);
    common_sha256_update(&hash, producer_domain, sizeof(producer_domain));
    common_sha256_update(&hash, report->target.release_digest, 32U);
    common_sha256_update(&hash, report->candidate.release_digest, 32U);
    common_sha256_final(&hash, digest);
}

static WholeShaderEvidenceStatus comparison_evidence(const WholeShaderSubject *subject,
                                                     const ReleaseShaderEvidenceReport *report,
                                                     WholeShaderEvidence **out_evidence) {
    WholeShaderEvidenceComparisonItem items[2] = {0};
    static const char payload[] = "DXBCSandbox.Reextraction.ObjectPayload.v1";
    static const char schema[] = "DXBCSandbox.Reextraction.SchemaShape.v1";
    common_sha256(payload, sizeof(payload), items[0].identity_digest);
    common_sha256(schema, sizeof(schema), items[1].identity_digest);
    memcpy(items[0].expected_digest, report->target.payload_digest, 32U);
    memcpy(items[0].observed_digest, report->candidate.payload_digest, 32U);
    memcpy(items[1].expected_digest, report->target.schema_digest, 32U);
    memcpy(items[1].observed_digest, report->candidate.schema_digest, 32U);
    WholeShaderComparisonEvidenceDescriptor descriptor = {0};
    descriptor.plane = WHOLE_SHADER_PLANE_REEXTRACTION;
    descriptor.producer = "dxbc-release-byte-bound";
    descriptor.producer_version = 1U;
    authority_digest(report, descriptor.authority_digest);
    descriptor.items = items;
    descriptor.item_count = 2U;
    return whole_shader_evidence_create_comparison(out_evidence, subject, &descriptor);
}

static WholeShaderEvidenceStatus unavailable_evidence(const WholeShaderSubject *subject,
                                                      const ReleaseShaderEvidenceReport *report,
                                                      WholeShaderEvidence **out_evidence) {
    WholeShaderNonpassEvidenceDescriptor descriptor = {0};
    descriptor.plane = WHOLE_SHADER_PLANE_REEXTRACTION;
    descriptor.producer = "dxbc-release-byte-bound";
    descriptor.producer_version = 1U;
    authority_digest(report, descriptor.authority_digest);
    descriptor.expected_item_count = 2U;
    descriptor.reason_code = (uint32_t)report->canonical.status;
    /* The existing comparator's stable diagnostic names identify missing
     * authority, not a claimed success or a serialized report's PASS field. */
    const char *field = release_shader_object_field_name(report->canonical.first_problem_field);
    common_sha256(field, strlen(field), descriptor.reason_digest);
    return whole_shader_evidence_create_unavailable(out_evidence, subject, &descriptor);
}

ReleaseShaderEvidenceStatus release_shader_make_reextraction_evidence(
    const ShaderCatalog *target_catalog, const ShaderCatalogRecord *target_record,
    const ShaderCatalog *candidate_catalog, const ShaderCatalogRecord *candidate_record,
    const TypeTreeSchemaRegistry *registry, const WholeShaderSubject *subject,
    WholeShaderEvidence **out_evidence, ReleaseShaderEvidenceReport *report) {
    if (out_evidence)
        *out_evidence = NULL;
    if (!report)
        return RELEASE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->target_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    report->candidate_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    report->evidence_status = WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    release_shader_object_certificate_report_init(&report->canonical);
    WholeShaderSubjectDescriptor descriptor;
    if (!out_evidence || !target_catalog || !target_record || !candidate_catalog ||
        !candidate_record ||
        whole_shader_subject_describe(subject, &descriptor) != WHOLE_SHADER_SUBJECT_OK)
        return RELEASE_SHADER_EVIDENCE_INVALID_ARGUMENT;

    ShaderObject target, candidate;
    shader_object_init(&target);
    shader_object_init(&candidate);
    ReleaseShaderEvidenceStatus status;
    report->target_status = shader_catalog_decode_object(target_catalog, target_record, registry,
                                                         &target, &report->target);
    if (report->target_status != SHADER_CATALOG_OBJECT_OK) {
        status = RELEASE_SHADER_EVIDENCE_TARGET_UNAVAILABLE;
        goto cleanup;
    }
    report->candidate_status = shader_catalog_decode_object(
        candidate_catalog, candidate_record, registry, &candidate, &report->candidate);
    if (report->candidate_status != SHADER_CATALOG_OBJECT_OK) {
        status = RELEASE_SHADER_EVIDENCE_CANDIDATE_UNAVAILABLE;
        goto cleanup;
    }
    if (!subject_matches(&descriptor, target_record, candidate_record, &candidate, report)) {
        status = RELEASE_SHADER_EVIDENCE_SUBJECT_MISMATCH;
        goto cleanup;
    }
    ReleaseShaderObjectCertificateStatus compared =
        release_shader_object_certify_equal(&target, &candidate, NULL, &report->canonical);
    if (compared == RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_UNAVAILABLE) {
        report->evidence_status = unavailable_evidence(subject, report, out_evidence);
    } else if (compared == RELEASE_SHADER_OBJECT_CERTIFICATE_OK ||
               compared == RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER) {
        /* Decoded equal payloads under the same schema cannot contradict the
         * canonical gate. Treat that as an authority error, never a PASS. */
        if (compared != RELEASE_SHADER_OBJECT_CERTIFICATE_OK &&
            memcmp(report->target.payload_digest, report->candidate.payload_digest, 32U) == 0 &&
            memcmp(report->target.schema_digest, report->candidate.schema_digest, 32U) == 0) {
            status = RELEASE_SHADER_EVIDENCE_COMPARISON_INVALID;
            goto cleanup;
        }
        report->evidence_status = comparison_evidence(subject, report, out_evidence);
    } else {
        status = RELEASE_SHADER_EVIDENCE_COMPARISON_INVALID;
        goto cleanup;
    }
    status = report->evidence_status == WHOLE_SHADER_EVIDENCE_OK
                 ? RELEASE_SHADER_EVIDENCE_OK
                 : RELEASE_SHADER_EVIDENCE_CONSTRUCTION_FAILED;
cleanup:
    shader_object_dispose(&candidate);
    shader_object_dispose(&target);
    return status;
}
