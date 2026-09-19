// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shaderlab_lift_capture.h"
#include "compiler/unity_shaderlab_lift_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct UnityShaderLabLiftCapture {
    UnityShaderLabLiftResult *result;
    WholeShaderSubject *subject;
    UnityShaderLabLiftCaptureReport authority;
};

void unity_shaderlab_lift_capture_free(UnityShaderLabLiftCapture *capture) {
    if (!capture)
        return;
    unity_shaderlab_lift_result_free(capture->result);
    whole_shader_subject_free(capture->subject);
    free(capture);
}

const UnityShaderLabLiftResult *
unity_shaderlab_lift_capture_result(const UnityShaderLabLiftCapture *capture) {
    return capture ? capture->result : NULL;
}

static bool capture_subject(UnityShaderLabLiftCapture *capture, const ShaderCatalogRecord *record,
                            const ShaderObject *object, const UnityCompileProfile *profile) {
    WholeShaderSubjectDescriptor descriptor = {0};
    descriptor.target_shader_path_id = record->path_id;
    descriptor.target_class_id = record->class_id;
    descriptor.serialized_target_platform = record->target_platform;
    descriptor.build_platform = profile->build_platform;
    descriptor.compiler_platform = 4;
    descriptor.graphics_api = 2;
    descriptor.source_residency = record->is_bundle_member
                                      ? WHOLE_SHADER_SOURCE_BUNDLE_MEMBER
                                      : WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE;
    descriptor.target_member_index = record->member_index;
    descriptor.target_member_identity = record->member_name ? record->member_name : "";
    descriptor.candidate_logical_name = object->shader.name;
    descriptor.unity_version = record->unity_version;
    if (strlen(record->occurrence_digest_hex) != 64U)
        return false;
    for (size_t i = 0; i < 32U; ++i) {
        unsigned value;
        if (sscanf(record->occurrence_digest_hex + i * 2U, "%2x", &value) != 1)
            return false;
        descriptor.target_occurrence_digest[i] = (uint8_t)value;
    }
    memcpy(descriptor.target_serialized_file_digest, record->serialized_digest, 32);
    memcpy(descriptor.target_object_payload_digest, capture->authority.target.payload_digest, 32);
    memcpy(descriptor.schema_authority_digest, capture->authority.target.schema_digest, 32);
    memcpy(descriptor.candidate_source_digest, capture->authority.accepted_source_digest, 32);
    memcpy(descriptor.compiler_profile_digest, capture->authority.profile_digest, 32);
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char session[] = "DXBCSandbox.CapturedLift.CompilerSession.v1";
    common_sha256_update(&hash, session, sizeof(session));
    common_sha256_update(&hash, capture->result->compiler_digest, 32);
    common_sha256_update(&hash, capture->result->environment_digest, 32);
    common_sha256_update(&hash, capture->result->source_path_digest, 32);
    common_sha256_update(&hash, capture->result->source_directory_digest, 32);
    common_sha256_update(&hash, capture->result->source_basename_digest, 32);
    common_sha256_final(&hash, descriptor.compiler_session_digest);
    static const char scope[] = "DXBCSandbox.AllEmittedLocalD3D11Passes.FullGeneratedDomain.v1";
    common_sha256(scope, sizeof(scope), descriptor.verification_scope_digest);
    return whole_shader_subject_create(&capture->subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK;
}

bool unity_shaderlab_lift_capture_subject(const UnityShaderLabLiftCapture *capture,
                                          WholeShaderSubjectDescriptor *descriptor) {
    return capture &&
           whole_shader_subject_describe(capture->subject, descriptor) == WHOLE_SHADER_SUBJECT_OK;
}

/* Compare all capture-owned fields through the existing canonical subject
 * encoding, while preserving fields supplied by independent producers. */
static bool subject_matches_capture(const UnityShaderLabLiftCapture *capture,
                                    const WholeShaderSubject *subject) {
    WholeShaderSubjectDescriptor actual, expected;
    if (!unity_shaderlab_lift_capture_subject(capture, &expected) ||
        whole_shader_subject_describe(subject, &actual) != WHOLE_SHADER_SUBJECT_OK)
        return false;
    memcpy(expected.player_profile_digest, actual.player_profile_digest, 32);
    memcpy(expected.dependency_map_digest, actual.dependency_map_digest, 32);
    memcpy(expected.producer_fingerprint, actual.producer_fingerprint, 32);
    memcpy(expected.candidate_release_digest, actual.candidate_release_digest, 32);
    WholeShaderSubject *bound = NULL;
    if (whole_shader_subject_create(&bound, &expected) != WHOLE_SHADER_SUBJECT_OK)
        return false;
    const bool matched = whole_shader_subject_equal(bound, subject);
    whole_shader_subject_free(bound);
    return matched;
}

static void compile_item_word(CommonSha256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(value >> (8U * i));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static void compile_item_identity(const UnityShaderLabLiftPassReport *pass,
                                  const UnityGeneratedDomainCompilerResponseRecord *compile,
                                  uint8_t digest[32]) {
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char domain[] = "DXBCSandbox.CapturedLift.CompileItem.v1";
    common_sha256_update(&hash, domain, sizeof(domain));
    compile_item_word(&hash, (uint32_t)pass->subshader_index);
    compile_item_word(&hash, (uint32_t)pass->pass_index);
    compile_item_word(&hash, (uint32_t)pass->serialized_pass_index);
    compile_item_word(&hash, (uint32_t)compile->stage_index);
    compile_item_word(&hash, (uint32_t)compile->hardware_tier_group);
    compile_item_word(&hash, compile->generated_state_index);
    compile_item_word(&hash, compile->aliased_state_index);
    compile_item_word(&hash, (uint32_t)compile->subprogram_index);
    common_sha256_final(&hash, digest);
}

static bool keyword_domain_item(const UnityGeneratedDomainReport *domain, int stage,
                                WholeShaderEvidenceComparisonItem *item) {
    if (stage < 0 || stage >= 5 || !domain->active_stage_count ||
        domain->active_stage_count != domain->attested_stage_count ||
        !domain->generated_state_count)
        return false;
    CommonSha256Context expected, observed;
    common_sha256_init(&expected);
    common_sha256_init(&observed);
    static const char name[] = "DXBCSandbox.CapturedLift.OrderedStageFamilies.v1";
    common_sha256_update(&expected, name, sizeof(name));
    common_sha256_update(&observed, name, sizeof(name));
    for (size_t family = 0; family < 3; ++family) {
        const UnityGeneratedKeywordFamilyEvidence *rows = &domain->keyword_families[stage][family];
        if (!rows->expected_valid || !rows->observed_valid)
            return false;
        common_sha256_update(&expected, rows->expected_digest, 32);
        common_sha256_update(&observed, rows->observed_digest, 32);
    }
    common_sha256_final(&expected, item->expected_digest);
    common_sha256_final(&observed, item->observed_digest);
    return true;
}

WholeShaderEvidenceStatus unity_shaderlab_lift_capture_make_evidence(
    const UnityShaderLabLiftCapture *capture, const WholeShaderSubject *subject,
    WholeShaderVerificationPlane plane, WholeShaderEvidence **output) {
    if (output)
        *output = NULL;
    if (!output || !capture || !subject_matches_capture(capture, subject))
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    if (plane != WHOLE_SHADER_PLANE_VARIANT_DOMAIN && plane != WHOLE_SHADER_PLANE_FULL_DXBC &&
        plane != WHOLE_SHADER_PLANE_REFLECTION_BINDING &&
        plane != WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS)
        return WHOLE_SHADER_EVIDENCE_INVALID_PLANE;
    const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(capture->result);
    if (!accepted || accepted->status != HLSL_LIFT_VERIFIED || !accepted->pass_count ||
        accepted->pass_count != accepted->certified_pass_count)
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    size_t count = 0;
    for (size_t p = 0; p < accepted->pass_count; ++p) {
        const UnityShaderLabLiftPassReport *pass = &accepted->passes[p];
        const UnityGeneratedDomainReport *domain = &pass->domain;
        if (!pass->certification_attempted || domain->status != UNITY_GENERATED_DOMAIN_OK ||
            !domain->planned_compile_count ||
            domain->compile_attempt_count != domain->planned_compile_count ||
            domain->matched_dxbc_count != domain->planned_compile_count ||
            domain->compiler_response_count != domain->planned_compile_count ||
            domain->compiler_response_count > WHOLE_SHADER_EVIDENCE_MAX_ITEMS - count)
            return WHOLE_SHADER_EVIDENCE_INVALID_COUNT;
        count += domain->compiler_response_count;
    }
    WholeShaderEvidenceComparisonItem *items = calloc(count, sizeof(*items));
    if (!items)
        return WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED;
    WholeShaderEvidenceStatus status = WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    size_t cursor = 0;
    for (size_t p = 0; p < accepted->pass_count; ++p) {
        const UnityShaderLabLiftPassReport *pass = &accepted->passes[p];
        for (size_t c = 0; c < pass->domain.compiler_response_count; ++c) {
            const UnityGeneratedDomainCompilerResponseRecord *compile =
                &pass->domain.compiler_responses[c];
            const UnityGeneratedCompileProvenance *provenance = &compile->provenance;
            if (!provenance->recorded || !provenance->response_received ||
                !provenance->has_request_identity || !provenance->has_output_digest)
                goto cleanup;
            WholeShaderEvidenceComparisonItem *item = &items[cursor++];
            compile_item_identity(pass, compile, item->identity_digest);
            if (plane == WHOLE_SHADER_PLANE_VARIANT_DOMAIN) {
                if (!keyword_domain_item(&pass->domain, compile->stage_index, item))
                    goto cleanup;
            } else if (plane == WHOLE_SHADER_PLANE_FULL_DXBC) {
                memcpy(item->expected_digest, provenance->target_digest, 32);
                memcpy(item->observed_digest, provenance->output_digest, 32);
            } else if (plane == WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS) {
                /* Strict policy: no diagnostics, including informational rows.
                 * Hash the actual complete multiset, never just clean-success. */
                const UnityCompilerResponseStatus expected = {0};
                if (!unity_generated_domain_diagnostics_fingerprint(&expected,
                                                                    item->expected_digest) ||
                    !unity_generated_domain_diagnostics_fingerprint(&compile->response,
                                                                    item->observed_digest))
                    goto cleanup;
            } else {
                const UnityReflectionCertificateReport *bindings = &compile->reflection_certificate;
                if (!compile->reflection_certificate_present ||
                    !bindings->expected_bindings_digest_valid ||
                    !bindings->observed_bindings_digest_valid ||
                    (bindings->status != UNITY_REFLECTION_CERTIFICATE_OK &&
                     bindings->status != UNITY_REFLECTION_CERTIFICATE_COMPATIBLE))
                    goto cleanup;
                memcpy(item->expected_digest, bindings->expected_bindings_digest, 32);
                memcpy(item->observed_digest, bindings->observed_bindings_digest, 32);
            }
        }
    }
    WholeShaderComparisonEvidenceDescriptor evidence = {
        .plane = plane,
        .producer = "dxbc-captured-lift",
        .producer_version = 1,
        .items = items,
        .item_count = count,
    };
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char authority[] = "DXBCSandbox.CapturedLift.StrictDiagnostics.Authority.v1";
    common_sha256_update(&hash, authority, sizeof(authority));
    common_sha256_update(&hash, capture->authority.target.release_digest, 32);
    uint8_t subject_digest[32];
    if (whole_shader_subject_digest(capture->subject, subject_digest) != WHOLE_SHADER_SUBJECT_OK)
        goto cleanup;
    common_sha256_update(&hash, subject_digest, 32);
    common_sha256_final(&hash, evidence.authority_digest);
    status = whole_shader_evidence_create_comparison(output, subject, &evidence);
cleanup:
    free(items);
    return status;
}

UnityShaderLabLiftCaptureStatus
unity_shaderlab_lift_capture(const UnityShaderLabLiftCaptureInput *input,
                             UnityShaderLabLiftCapture **output,
                             UnityShaderLabLiftCaptureReport *report) {
    if (output)
        *output = NULL;
    if (!report)
        return UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->source_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    report->archive_status = SHADER_OBJECT_NOT_DECODED;
    report->lift_status = HLSL_LIFT_INVALID_ARGUMENT;
    if (!input || !output || !input->catalog || !input->record || !input->broker ||
        !input->limits || !unity_compile_profile_validate(input->profile) || !input->source_path ||
        !input->source_path[0] || !input->source_directory || !input->source_directory[0] ||
        !input->source_basename || !input->source_basename[0])
        return UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT;

    UnityShaderLabLiftCapture *capture = calloc(1, sizeof(*capture));
    if (!capture)
        return UNITY_SHADERLAB_CAPTURE_OUT_OF_MEMORY;
    ShaderObject target, revalidated;
    shader_object_init(&target);
    shader_object_init(&revalidated);
    ShaderBlobArchive archive = {0};
    UnityShaderLabLiftCaptureStatus status = UNITY_SHADERLAB_CAPTURE_SOURCE_UNAVAILABLE;
    report->source_status = shader_catalog_decode_object(input->catalog, input->record,
                                                         input->registry, &target, &report->target);
    if (report->source_status != SHADER_CATALOG_OBJECT_OK)
        goto cleanup;
    report->archive_status = shader_object_open_d3d11_archive(&target, &archive);
    if (report->archive_status != SHADER_OBJECT_OK) {
        status = UNITY_SHADERLAB_CAPTURE_ARCHIVE_UNAVAILABLE;
        goto cleanup;
    }
    const UnityCompileProfile profile = *input->profile;
    const UnityShaderLabLiftInput lift = {
        .shader = &target.shader,
        .archive = &archive,
        .profile = &profile,
        .broker = input->broker,
        .source_path = input->source_path,
        .source_directory = input->source_directory,
        .source_basename = input->source_basename,
    };
    report->lift_status = unity_shaderlab_lift_run(&lift, NULL, input->limits, &capture->result);
    const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(capture->result);
    if (!accepted || !capture->result->authority_pinned || !sb_ok(&accepted->source)) {
        status = UNITY_SHADERLAB_CAPTURE_NO_ACCEPTED_SOURCE;
        goto cleanup;
    }
    ShaderCatalogObjectReport after;
    report->source_status = shader_catalog_decode_object(input->catalog, input->record,
                                                         input->registry, &revalidated, &after);
    if (report->source_status != SHADER_CATALOG_OBJECT_OK ||
        memcmp(report->target.release_digest, after.release_digest, 32) != 0) {
        status = UNITY_SHADERLAB_CAPTURE_SOURCE_CHANGED;
        goto cleanup;
    }
    common_sha256(accepted->source.buf, accepted->source.len, report->accepted_source_digest);
    memcpy(report->compiler_digest, capture->result->compiler_digest, 32);
    memcpy(report->environment_digest, capture->result->environment_digest, 32);
    memcpy(report->profile_digest, capture->result->profile_digest, 32);
    capture->authority = *report;
    if (!capture_subject(capture, input->record, &target, &profile)) {
        status = UNITY_SHADERLAB_CAPTURE_OUT_OF_MEMORY;
        goto cleanup;
    }
    *output = capture;
    capture = NULL;
    status = UNITY_SHADERLAB_CAPTURE_OK;
cleanup:
    shader_blob_archive_close(&archive);
    shader_object_dispose(&revalidated);
    shader_object_dispose(&target);
    unity_shaderlab_lift_capture_free(capture);
    return status;
}
