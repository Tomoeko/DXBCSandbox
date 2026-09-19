// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shaderlab_lift_capture.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #x);                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int check_evidence(const UnityShaderLabLiftCapture *capture) {
    WholeShaderSubjectDescriptor descriptor;
    CHECK(unity_shaderlab_lift_capture_subject(capture, &descriptor));
    WholeShaderSubject *subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
    WholeShaderEvidence *dxbc = NULL, *bindings = NULL, *domain = NULL, *diagnostics = NULL,
                        *invalid = NULL;
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject, WHOLE_SHADER_PLANE_FULL_DXBC,
                                                     &dxbc) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject,
                                                     WHOLE_SHADER_PLANE_REFLECTION_BINDING,
                                                     &bindings) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject,
                                                     WHOLE_SHADER_PLANE_VARIANT_DOMAIN,
                                                     &domain) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject,
                                                     WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS,
                                                     &diagnostics) == WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary a, b, d, diagnostic_summary;
    CHECK(whole_shader_evidence_describe(diagnostics, &diagnostic_summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(diagnostic_summary.status == WHOLE_SHADER_PLANE_PASS);
    CHECK(whole_shader_evidence_describe(domain, &d) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(d.status == WHOLE_SHADER_PLANE_PASS);
    CHECK(whole_shader_evidence_describe(dxbc, &a) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_evidence_describe(bindings, &b) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(a.status == WHOLE_SHADER_PLANE_PASS && b.status == WHOLE_SHADER_PLANE_PASS);
    CHECK(a.expected_item_count > 0 && a.expected_item_count == b.expected_item_count);
    CHECK(memcmp(a.coverage_digest, b.coverage_digest, 32) == 0);
    CHECK(memcmp(a.coverage_digest, d.coverage_digest, 32) == 0);
    CHECK(memcmp(a.coverage_digest, diagnostic_summary.coverage_digest, 32) == 0);
    CHECK(unity_shaderlab_lift_capture_make_evidence(
              capture, subject, WHOLE_SHADER_PLANE_RUNTIME_SELECTION, &invalid) ==
          WHOLE_SHADER_EVIDENCE_INVALID_PLANE);
    CHECK(!invalid);
    printf("domain=pass dxbc=pass diagnostics=pass bindings=pass compile_items=%llu\n",
           (unsigned long long)a.expected_item_count);
    for (size_t mutation = 0; mutation < 15; ++mutation) {
        WholeShaderSubjectDescriptor changed = descriptor;
        switch (mutation) {
        case 0:
            changed.target_shader_path_id++;
            break;
        case 1:
            changed.target_occurrence_digest[0] ^= 1;
            break;
        case 2:
            changed.target_serialized_file_digest[0] ^= 1;
            break;
        case 3:
            changed.target_object_payload_digest[0] ^= 1;
            break;
        case 4:
            changed.schema_authority_digest[0] ^= 1;
            break;
        case 5:
            changed.candidate_source_digest[0] ^= 1;
            break;
        case 6:
            changed.compiler_profile_digest[0] ^= 1;
            break;
        case 7:
            changed.compiler_session_digest[0] ^= 1;
            break;
        case 8:
            changed.verification_scope_digest[0] ^= 1;
            break;
        case 9:
            changed.build_platform++;
            break;
        case 10:
            changed.compiler_platform++;
            break;
        case 11:
            changed.graphics_api++;
            break;
        case 12:
            changed.candidate_logical_name = "Different/Shader";
            break;
        case 13:
            changed.unity_version = "different";
            break;
        case 14:
            changed.serialized_target_platform++;
            break;
        }
        WholeShaderSubject *mismatch = NULL;
        CHECK(whole_shader_subject_create(&mismatch, &changed) == WHOLE_SHADER_SUBJECT_OK);
        CHECK(unity_shaderlab_lift_capture_make_evidence(capture, mismatch,
                                                         WHOLE_SHADER_PLANE_FULL_DXBC, &invalid) ==
              WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
        CHECK(!invalid);
        whole_shader_subject_free(mismatch);
    }
    /* These two planes alone cannot certify whole-shader equivalence. */
    WholeShaderCertificateInput *certificate = NULL;
    CHECK(whole_shader_certificate_input_create(&certificate, subject,
                                                WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(certificate, dxbc) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    CHECK(whole_shader_certificate_input_add_evidence(certificate, bindings) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    WholeShaderCertificateReport report;
    (void)whole_shader_certificate_evaluate(certificate, &report);
    CHECK(!report.d3d11_byte_equivalence_certified && !report.d3d11_logical_equivalence_certified);
    whole_shader_certificate_input_free(certificate);
    whole_shader_evidence_free(diagnostics);
    whole_shader_evidence_free(domain);
    whole_shader_evidence_free(dxbc);
    whole_shader_evidence_free(bindings);
    whole_shader_subject_free(subject);
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 5);
    UnityShaderLabLiftCapture *capture = NULL;
    UnityShaderLabLiftCaptureReport report;
    CHECK(unity_shaderlab_lift_capture(NULL, &capture, &report) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    CHECK(!capture && !unity_shaderlab_lift_capture_result(NULL));
    unity_shaderlab_lift_capture_free(NULL);
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(&registry, CAPTURE_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = &registry;
    options.retain_source_snapshots = true;
    const char *path = argc == 5 ? argv[1] : CAPTURE_EMPTY_FIXTURE;
    CHECK(shader_catalog_build(&path, 1, &options, &catalog) == SHADER_CATALOG_OK);
    CHECK(catalog.record_count == 1 && shader_catalog_is_complete(&catalog));
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19;
    profile.valid_apis = 295472;
    strcpy(profile.provenance, "synthetic-unit-test");
    if (argc == 5)
        CHECK(unity_compile_profile_load(argv[2], &profile) == UNITY_COMPILE_PROFILE_OK);
    UnityCompilerBroker *broker =
        unity_compiler_broker_create_lazy(argc == 5 ? argv[3] : ".", argc == 5 ? argv[4] : NULL);
    CHECK(broker);
    HLSLLiftLimits limits = {2, 128, 60000};
    UnityShaderLabLiftCaptureInput input = {
        .catalog = &catalog,
        .record = catalog.records,
        .registry = &registry,
        .profile = &profile,
        .broker = broker,
        .source_path = "Assets/Captured.shader",
        .source_directory = argc == 5 ? argv[3] : ".",
        .source_basename = "Captured.shader",
        .limits = &limits,
    };
    CHECK(unity_shaderlab_lift_capture(&input, NULL, &report) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    CHECK(unity_shaderlab_lift_capture(&input, &capture, NULL) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    ShaderCatalogRecord unowned = *catalog.records;
    input.record = &unowned;
    CHECK(unity_shaderlab_lift_capture(&input, &capture, &report) ==
          UNITY_SHADERLAB_CAPTURE_SOURCE_UNAVAILABLE);
    CHECK(report.source_status == SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED && !capture);
    input.record = catalog.records;
    UnityShaderLabLiftCaptureStatus status =
        unity_shaderlab_lift_capture(&input, &capture, &report);
    printf("capture=%d lift=%d archive=%d\n", status, report.lift_status, report.archive_status);
    if (argc == 1) {
        CHECK(status == UNITY_SHADERLAB_CAPTURE_ARCHIVE_UNAVAILABLE && !capture);
        CHECK(report.archive_status == SHADER_OBJECT_D3D11_PLATFORM_ABSENT);
        UnityCompilerBrokerStats stats;
        unity_compiler_broker_get_stats(broker, &stats);
        CHECK(stats.compiler_process_starts == 0);
    } else {
        CHECK(status == UNITY_SHADERLAB_CAPTURE_OK && capture);
        const UnityShaderLabLiftArtifact *accepted =
            unity_shaderlab_lift_accepted(unity_shaderlab_lift_capture_result(capture));
        CHECK(accepted && accepted->high_level && accepted->certified_pass_count > 0);
        uint8_t digest[32];
        common_sha256(accepted->source.buf, accepted->source.len, digest);
        CHECK(memcmp(digest, report.accepted_source_digest, 32) == 0);
        CHECK(unity_compile_profile_fingerprint(&profile, digest) == UNITY_COMPILE_PROFILE_OK);
        CHECK(memcmp(digest, report.profile_digest, 32) == 0);
        CHECK(check_evidence(capture) == 0);
        printf("high_level=%d helper=%d passes=%zu\n", accepted->high_level,
               accepted->unity_uv_helpers, accepted->certified_pass_count);
    }
    unity_shaderlab_lift_capture_free(capture);
    unity_compiler_broker_destroy(broker);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    return 0;
}
