// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shader_bundle_evidence.h"

#include "common/file_io.h"

#include <stdlib.h>
#include <string.h>

struct UnityShaderBundleAuthority {
    ShaderCatalog catalog;
    UnityShaderBundleAuthoritySummary summary;
};

static const char authority_domain[] = "DXBCSandbox.Import.Windows64.D3D11.Strict.v1";

const char *unity_shader_bundle_evidence_status_name(UnityShaderBundleEvidenceStatus status) {
    switch (status) {
    case UNITY_SHADER_BUNDLE_EVIDENCE_OK:
        return "ok";
    case UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT:
        return "invalid-argument";
    case UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_UNAVAILABLE:
        return "input-unavailable";
    case UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_CHANGED:
        return "input-changed";
    case UNITY_SHADER_BUNDLE_EVIDENCE_GATE_FAILED:
        return "gate-failed";
    case UNITY_SHADER_BUNDLE_EVIDENCE_CAPTURE_FAILED:
        return "capture-failed";
    case UNITY_SHADER_BUNDLE_EVIDENCE_ARTIFACT_MISMATCH:
        return "artifact-mismatch";
    case UNITY_SHADER_BUNDLE_EVIDENCE_ALLOCATION_FAILED:
        return "allocation-failed";
    }
    return "unknown";
}

static void fingerprint(UnityShaderBundleAuthoritySummary *summary) {
    CommonSha256Context hash;
    common_sha256_init(&hash);
    common_sha256_update(&hash, authority_domain, sizeof(authority_domain));
    common_sha256_update(&hash, summary->source_digest, 32U);
    common_sha256_update(&hash, summary->launcher_digest, 32U);
    common_sha256_update(&hash, summary->bridge_digest, 32U);
    common_sha256_update(&hash, summary->artifact_digest, 32U);
    common_sha256_update(&hash, summary->release_digest, 32U);
    common_sha256_final(&hash, summary->authority_digest);
}

void unity_shader_bundle_authority_free(UnityShaderBundleAuthority *authority) {
    if (!authority)
        return;
    shader_catalog_dispose(&authority->catalog);
    free(authority);
}

bool unity_shader_bundle_authority_describe(const UnityShaderBundleAuthority *authority,
                                            UnityShaderBundleAuthoritySummary *summary) {
    if (!authority || !summary)
        return false;
    *summary = authority->summary;
    return true;
}

const ShaderCatalog *
unity_shader_bundle_authority_catalog(const UnityShaderBundleAuthority *authority) {
    return authority ? &authority->catalog : NULL;
}

UnityShaderBundleEvidenceStatus unity_shader_bundle_capture(
    const UnityShaderBundleGateOptions *options, const TypeTreeSchemaRegistry *registry,
    UnityShaderBundleAuthority **out_authority, UnityShaderBundleEvidenceDiagnostic *diagnostic) {
    if (out_authority)
        *out_authority = NULL;
    if (!diagnostic)
        return UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->gate_status = UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT;
    diagnostic->gate.unity_exit_code = -1;
    diagnostic->catalog_status = SHADER_CATALOG_INVALID_ARGUMENT;
    diagnostic->object_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    if (!out_authority || !unity_shader_bundle_gate_options_validate(options) ||
        options->input_count != 1U || options->target != UNITY_SHADER_BUNDLE_TARGET_WINDOWS64 ||
        options->backend != UNITY_SHADER_BUNDLE_BACKEND_D3D11 ||
        options->warning_policy != UNITY_SHADER_IMPORT_WARNINGS_FAIL ||
        strcmp(options->expected_unity_version, "2021.3.35f1") != 0)
        return UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT;

    UnityShaderBundleAuthority *authority = calloc(1U, sizeof(*authority));
    if (!authority)
        return UNITY_SHADER_BUNDLE_EVIDENCE_ALLOCATION_FAILED;
    shader_catalog_init(&authority->catalog);
    UnityShaderBundleEvidenceStatus status = UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_UNAVAILABLE;
    CommonFileView files[3] = {0};
    const char *paths[] = {options->inputs[0], options->unity_executable, options->bridge_path};
    uint8_t *digests[] = {authority->summary.source_digest, authority->summary.launcher_digest,
                          authority->summary.bridge_digest};
    size_t opened = 0U;
    for (; opened < 3U; ++opened) {
        if (common_file_view_open_regular(paths[opened], SIZE_MAX, &files[opened]) !=
            COMMON_FILE_OK)
            goto cleanup;
        if (!common_file_view_sha256(&files[opened], digests[opened])) {
            ++opened;
            goto cleanup;
        }
    }
    diagnostic->gate_status = unity_shader_bundle_gate_run(options, &diagnostic->gate);
    if (diagnostic->gate_status != UNITY_SHADER_BUNDLE_GATE_OK) {
        status = UNITY_SHADER_BUNDLE_EVIDENCE_GATE_FAILED;
        goto cleanup;
    }
    const UnityShaderBundleGateSummary *gate = &diagnostic->gate.summary;
    if (!diagnostic->gate.bundle_published || gate->candidate_count != 1U ||
        !gate->all_candidates_imported || !gate->bundle_built || !gate->passed ||
        gate->error_count != 0U || gate->warning_count != 0U) {
        status = UNITY_SHADER_BUNDLE_EVIDENCE_ARTIFACT_MISMATCH;
        goto cleanup;
    }
    ShaderCatalogOptions catalog_options;
    shader_catalog_options_default(&catalog_options);
    catalog_options.retain_source_snapshots = true;
    catalog_options.schema_registry = registry;
    const char *bundle_path = gate->output_bundle_path;
    diagnostic->catalog_status =
        shader_catalog_build(&bundle_path, 1U, &catalog_options, &authority->catalog);
    status = UNITY_SHADER_BUNDLE_EVIDENCE_CAPTURE_FAILED;
    if (diagnostic->catalog_status != SHADER_CATALOG_OK ||
        !shader_catalog_is_complete(&authority->catalog) || authority->catalog.record_count != 1U)
        goto cleanup;
    const ShaderCatalogRecord *record = authority->catalog.records;
    if (record->class_id != 48 || !record->is_bundle_member || record->target_platform != 19U ||
        !record->unity_version ||
        strcmp(record->unity_version, options->expected_unity_version) != 0)
        goto cleanup;
    ShaderObject object;
    shader_object_init(&object);
    diagnostic->object_status = shader_catalog_decode_object(&authority->catalog, record, registry,
                                                             &object, &diagnostic->object);
    shader_object_dispose(&object);
    if (diagnostic->object_status != SHADER_CATALOG_OBJECT_OK)
        goto cleanup;
    char artifact_hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(diagnostic->object.source_artifact_digest, artifact_hex);
    if (strcmp(artifact_hex, gate->bundle_sha256) != 0) {
        status = UNITY_SHADER_BUNDLE_EVIDENCE_ARTIFACT_MISMATCH;
        goto cleanup;
    }
    memcpy(authority->summary.artifact_digest, diagnostic->object.source_artifact_digest, 32U);
    memcpy(authority->summary.release_digest, diagnostic->object.release_digest, 32U);
    fingerprint(&authority->summary);
    status = UNITY_SHADER_BUNDLE_EVIDENCE_OK;
cleanup:
    for (size_t index = 0U; index < opened; ++index) {
        if (common_file_view_close(&files[index]) != COMMON_FILE_OK)
            status = UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_CHANGED;
    }
    if (status == UNITY_SHADER_BUNDLE_EVIDENCE_OK)
        *out_authority = authority;
    else
        unity_shader_bundle_authority_free(authority);
    return status;
}

WholeShaderEvidenceStatus unity_shader_bundle_authority_make_evidence(
    const UnityShaderBundleAuthority *authority, const TypeTreeSchemaRegistry *registry,
    const WholeShaderSubject *subject, WholeShaderEvidence **out_evidence) {
    if (!out_evidence)
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    *out_evidence = NULL;
    WholeShaderSubjectDescriptor descriptor;
    if (!authority ||
        whole_shader_subject_describe(subject, &descriptor) != WHOLE_SHADER_SUBJECT_OK ||
        descriptor.serialized_target_platform != 19U || descriptor.build_platform != 19U ||
        descriptor.compiler_platform != 4 || descriptor.graphics_api != 2U ||
        strcmp(descriptor.unity_version, "2021.3.35f1") != 0 ||
        strcmp(descriptor.candidate_logical_name, authority->catalog.records[0].name) != 0)
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    ShaderObject object;
    ShaderCatalogObjectReport capture;
    shader_object_init(&object);
    ShaderCatalogObjectStatus status = shader_catalog_decode_object(
        &authority->catalog, authority->catalog.records, registry, &object, &capture);
    shader_object_dispose(&object);
    if (status != SHADER_CATALOG_OBJECT_OK ||
        memcmp(capture.release_digest, authority->summary.release_digest, 32U) != 0)
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;

    WholeShaderEvidenceComparisonItem items[2] = {0};
    static const char source_item[] = "DXBCSandbox.Import.Source.v1";
    static const char release_item[] = "DXBCSandbox.Import.Release.v1";
    common_sha256(source_item, sizeof(source_item), items[0].identity_digest);
    common_sha256(release_item, sizeof(release_item), items[1].identity_digest);
    memcpy(items[0].expected_digest, descriptor.candidate_source_digest, 32U);
    memcpy(items[0].observed_digest, authority->summary.source_digest, 32U);
    memcpy(items[1].expected_digest, descriptor.candidate_release_digest, 32U);
    memcpy(items[1].observed_digest, authority->summary.release_digest, 32U);
    WholeShaderComparisonEvidenceDescriptor evidence = {0};
    evidence.plane = WHOLE_SHADER_PLANE_IMPORTER;
    evidence.producer = "dxbc-isolated-bundle-import";
    evidence.producer_version = 1U;
    memcpy(evidence.authority_digest, authority->summary.authority_digest, 32U);
    evidence.items = items;
    evidence.item_count = 2U;
    return whole_shader_evidence_create_comparison(out_evidence, subject, &evidence);
}
