// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shader_bundle_evidence.h"
#include "app/release_shader_evidence.h"
#include "common/file_io.h"
#include "test_shader_subject.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static bool boundaries(void) {
    UnityShaderBundleAuthority *authority = NULL;
    UnityShaderBundleEvidenceDiagnostic diagnostic;
    CHECK(unity_shader_bundle_capture(NULL, NULL, &authority, &diagnostic) ==
          UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!authority && diagnostic.gate.unity_exit_code == -1);
    UnityShaderBundleGateOptions options;
    unity_shader_bundle_gate_options_init(&options);
    const char *source = "absent-producer-source.shader";
    options.unity_executable = "absent-producer-editor";
    options.bridge_path = "absent-producer-bridge.cs";
    options.expected_unity_version = "2021.3.35f1";
    options.inputs = &source;
    options.input_count = 1U;
    options.output_bundle_path = "absent-producer.bundle";
    options.report_path = "absent-producer.json";
    options.log_path = "absent-producer.log";
    options.target = UNITY_SHADER_BUNDLE_TARGET_WINDOWS64;
    options.backend = UNITY_SHADER_BUNDLE_BACKEND_D3D11;
    options.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
    CHECK(unity_shader_bundle_gate_options_validate(&options));
    for (unsigned mutation = 0U; mutation < 5U; ++mutation) {
        UnityShaderBundleGateOptions changed = options;
        switch (mutation) {
        case 0U:
            changed.input_count = 0U;
            break;
        case 1U:
            changed.target = UNITY_SHADER_BUNDLE_TARGET_MACOS;
            break;
        case 2U:
            changed.backend = UNITY_SHADER_BUNDLE_BACKEND_VULKAN;
            break;
        case 3U:
            changed.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_ALLOW;
            break;
        case 4U:
            changed.expected_unity_version = "2021.3.34f1";
            break;
        }
        CHECK(unity_shader_bundle_capture(&changed, NULL, &authority, &diagnostic) ==
              UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT);
        CHECK(!authority);
    }
    CHECK(unity_shader_bundle_capture(&options, NULL, &authority, &diagnostic) ==
          UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_UNAVAILABLE);
    CHECK(!authority && !diagnostic.gate.bundle_published);
    source = DXBC_BUNDLE_EVIDENCE_SOURCE;
    CHECK(unity_shader_bundle_capture(&options, NULL, &authority, &diagnostic) ==
          UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_UNAVAILABLE);
    CHECK(!authority && !diagnostic.gate.bundle_published);
    options.unity_executable = source;
    CHECK(unity_shader_bundle_capture(&options, NULL, &authority, &diagnostic) ==
          UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_UNAVAILABLE);
    CHECK(!authority && !diagnostic.gate.bundle_published);
    CHECK(unity_shader_bundle_capture(&options, NULL, NULL, &diagnostic) ==
          UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT);
    CHECK(unity_shader_bundle_capture(&options, NULL, &authority, NULL) ==
          UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT);
    UnityShaderBundleAuthoritySummary summary;
    CHECK(!unity_shader_bundle_authority_describe(NULL, &summary));
    CHECK(!unity_shader_bundle_authority_catalog(NULL));
    WholeShaderEvidence *evidence = NULL;
    CHECK(unity_shader_bundle_authority_make_evidence(NULL, NULL, NULL, &evidence) ==
          WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!evidence);
    unity_shader_bundle_authority_free(NULL);
    return true;
}

static bool live_capture(char **argv) {
    UnityShaderBundleGateOptions options;
    unity_shader_bundle_gate_options_init(&options);
    options.unity_executable = argv[1];
    options.bridge_path = argv[2];
    options.expected_unity_version = "2021.3.35f1";
    const char *source = argv[3];
    options.inputs = &source;
    options.input_count = 1U;
    options.target = UNITY_SHADER_BUNDLE_TARGET_WINDOWS64;
    options.backend = UNITY_SHADER_BUNDLE_BACKEND_D3D11;
    options.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
    options.keep_policy = UNITY_SHADER_IMPORT_KEEP_ALWAYS;
    char output[4096], report_path[4096], log_path[4096];
    CHECK(snprintf(output, sizeof(output), "%s.bundle", argv[5]) > 0);
    CHECK(snprintf(report_path, sizeof(report_path), "%s.json", argv[5]) > 0);
    CHECK(snprintf(log_path, sizeof(log_path), "%s.log", argv[5]) > 0);
    CHECK(strlen(argv[5]) + 8U < sizeof(output));
    options.output_bundle_path = output;
    options.report_path = report_path;
    options.log_path = log_path;
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(&registry, DXBC_RELEASE_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    UnityShaderBundleAuthority *authority = NULL;
    UnityShaderBundleEvidenceDiagnostic diagnostic;
    UnityShaderBundleEvidenceStatus status =
        unity_shader_bundle_capture(&options, &registry, &authority, &diagnostic);
    printf("capture=%s gate=%s published=%u catalog=%u object=%u\n",
           unity_shader_bundle_evidence_status_name(status),
           unity_shader_bundle_gate_status_name(diagnostic.gate_status),
           diagnostic.gate.bundle_published ? 1U : 0U, (unsigned)diagnostic.catalog_status,
           (unsigned)diagnostic.object_status);
    fflush(stdout);
    /* Explicit negative execution mode, useful for a controlled launcher
     * that changes a held input only after the real import completes. */
    if (strcmp(argv[4], "expect-input-drift") == 0) {
        CHECK(status == UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_CHANGED && !authority);
        CHECK(diagnostic.gate_status == UNITY_SHADER_BUNDLE_GATE_OK &&
              diagnostic.gate.bundle_published);
        typetree_schema_registry_dispose(&registry);
        return true;
    }
    CHECK(status == UNITY_SHADER_BUNDLE_EVIDENCE_OK && authority);
    UnityShaderBundleAuthoritySummary summary;
    CHECK(unity_shader_bundle_authority_describe(authority, &summary));
    const ShaderCatalog *candidate = unity_shader_bundle_authority_catalog(authority);
    CHECK(candidate && candidate->record_count == 1U);
    ShaderCatalog target;
    shader_catalog_init(&target);
    ShaderCatalogOptions catalog_options;
    shader_catalog_options_default(&catalog_options);
    catalog_options.retain_source_snapshots = true;
    catalog_options.schema_registry = &registry;
    const char *target_path = argv[4];
    CHECK(shader_catalog_build(&target_path, 1U, &catalog_options, &target) == SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(&target) && target.record_count == 1U);
    WholeShaderSubjectDescriptor descriptor;
    ShaderCatalogObjectReport captures[2];
    CHECK(test_shader_subject_from_catalogs(&target, candidate, &registry, &descriptor, captures));
    memcpy(descriptor.candidate_source_digest, summary.source_digest, 32U);
    WholeShaderSubject *subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
    WholeShaderEvidence *imported = NULL, *extracted = NULL;
    CHECK(unity_shader_bundle_authority_make_evidence(authority, &registry, subject, &imported) ==
          WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary evidence_summary;
    CHECK(whole_shader_evidence_describe(imported, &evidence_summary) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_summary.status == WHOLE_SHADER_PLANE_PASS);
    ReleaseShaderEvidenceReport release;
    CHECK(release_shader_make_reextraction_evidence(
              &target, target.records, candidate, candidate->records, &registry, subject,
              &extracted, &release) == RELEASE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_evidence_describe(extracted, &evidence_summary) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_summary.status == WHOLE_SHADER_PLANE_PASS);
    WholeShaderCertificateInput *input = NULL;
    CHECK(whole_shader_certificate_input_create(&input, subject,
                                                WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, imported) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, extracted) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    WholeShaderCertificateReport result;
    CHECK(whole_shader_certificate_evaluate(input, &result) ==
          WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE);
    CHECK(result.importer_acceptance_certified && result.release_object_equivalence_certified);
    CHECK(!result.d3d11_logical_equivalence_certified &&
          !result.finite_pixel_observations_certified);
    printf("import=pass reextraction=pass logical=incomplete fields=%zu/%zu artifacts=%zu/%zu\n",
           release.canonical.matched_field_count, release.canonical.evaluated_field_count,
           release.canonical.matched_artifact_count, release.canonical.expected_artifact_count);
    whole_shader_certificate_input_free(input);
    whole_shader_evidence_free(imported);
    whole_shader_evidence_free(extracted);
    for (unsigned mutation = 0U; mutation < 7U; ++mutation) {
        WholeShaderSubjectDescriptor bad = descriptor;
        switch (mutation) {
        case 0U:
            bad.candidate_source_digest[0] ^= 1U;
            break;
        case 1U:
            bad.candidate_release_digest[0] ^= 1U;
            break;
        case 2U:
            ++bad.build_platform;
            break;
        case 3U:
            ++bad.graphics_api;
            break;
        case 4U:
            bad.unity_version = "2021.3.34f1";
            break;
        case 5U:
            bad.candidate_logical_name = "Experiment/Wrong";
            break;
        case 6U:
            ++bad.compiler_platform;
            break;
        }
        WholeShaderSubject *changed = NULL;
        CHECK(whole_shader_subject_create(&changed, &bad) == WHOLE_SHADER_SUBJECT_OK);
        CHECK(
            unity_shader_bundle_authority_make_evidence(authority, &registry, changed, &imported) ==
            (mutation < 2U ? WHOLE_SHADER_EVIDENCE_OK : WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT));
        if (mutation < 2U) {
            CHECK(whole_shader_evidence_describe(imported, &evidence_summary) ==
                  WHOLE_SHADER_EVIDENCE_OK);
            CHECK(evidence_summary.status == WHOLE_SHADER_PLANE_FAIL);
        } else {
            CHECK(!imported);
        }
        whole_shader_evidence_free(imported);
        whole_shader_subject_free(changed);
    }
    whole_shader_subject_free(subject);
    shader_catalog_dispose(&target);
    unity_shader_bundle_authority_free(authority);
    typetree_schema_registry_dispose(&registry);
    return true;
}

int main(int argc, char **argv) {
    if (argc == 1)
        return boundaries() ? 0 : 1;
    if (argc == 6)
        return live_capture(argv) ? 0 : 1;
    fprintf(stderr, "Usage: %s [LAUNCHER BRIDGE SOURCE TARGET_BUNDLE OUTPUT_PREFIX]\n", argv[0]);
    return 2;
}
