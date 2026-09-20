// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shader_contract.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition);                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int check_failure(const UnityShaderContractOptions *options,
                         UnityShaderContractStatus expected) {
    UnityShaderContract *contract = NULL;
    UnityShaderContractReport report;
    CHECK(unity_shader_contract_capture(options, &contract, &report) == expected);
    CHECK(!contract && !report.source_published && !report.import.gate.bundle_published);
    CHECK(report.constructed_plane_mask == 0);
    CHECK(!report.certificate.requested_scope_certified);
    CHECK(!report.certificate.d3d11_logical_equivalence_certified);
    for (unsigned plane = 0; plane < WHOLE_SHADER_PLANE_COUNT; ++plane)
        CHECK(report.planes[plane].status ==
              ((WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK & WHOLE_SHADER_PLANE_BIT(plane))
                   ? WHOLE_SHADER_PLANE_NOT_RUN
                   : WHOLE_SHADER_PLANE_NOT_REQUESTED));
    return 0;
}

static int check_boundaries(const UnityShaderContractOptions *options) {
    CHECK(check_failure(NULL, UNITY_SHADER_CONTRACT_INVALID_ARGUMENT) == 0);
    UnityShaderContract *contract = NULL;
    UnityShaderContractReport report;
    CHECK(unity_shader_contract_capture(options, NULL, &report) ==
          UNITY_SHADER_CONTRACT_INVALID_ARGUMENT);
    CHECK(unity_shader_contract_capture(options, &contract, NULL) ==
          UNITY_SHADER_CONTRACT_INVALID_ARGUMENT);
    CHECK(!contract && !unity_shader_contract_subject(NULL) && !unity_shader_contract_lift(NULL));
    CHECK(!unity_shader_contract_evidence(NULL, WHOLE_SHADER_PLANE_FULL_DXBC));
    unity_shader_contract_free(NULL);
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        UnityShaderContractOptions changed = *options;
        switch (mutation) {
        case 0:
            changed.candidate_source_path = "wrong.extension";
            break;
        case 1:
            changed.player_root = NULL;
            break;
        case 2:
            changed.lift.profile = NULL;
            break;
        case 3:
            changed.lift.broker = NULL;
            break;
        case 4:
            changed.lift.record = NULL;
            break;
        case 5:
            changed.bundle.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_ALLOW;
            break;
        case 6:
            changed.bundle.expected_unity_version = "2021.3.34f1";
            break;
        case 7:
            changed.bundle.backend = UNITY_SHADER_BUNDLE_BACKEND_VULKAN;
            break;
        case 8:
            changed.bundle.target = UNITY_SHADER_BUNDLE_TARGET_MACOS;
            break;
        case 9:
            changed.bundle.output_bundle_path = NULL;
            break;
        case 10:
            changed.lift.limits = NULL;
            break;
        }
        CHECK(check_failure(&changed, UNITY_SHADER_CONTRACT_INVALID_ARGUMENT) == 0);
    }
    UnityShaderContractOptions changed = *options;
    changed.player_metadata_path = "absent-metadata.assets";
    CHECK(check_failure(&changed, UNITY_SHADER_CONTRACT_PLAYER_UNAVAILABLE) == 0);
    changed = *options;
    changed.player_limits.max_total_bytes = 1;
    CHECK(check_failure(&changed, UNITY_SHADER_CONTRACT_PLAYER_UNAVAILABLE) == 0);
    UnityCompileProfile mismatch = *options->lift.profile;
    mismatch.d3d11_capabilities ^= 1;
    changed = *options;
    changed.lift.profile = &mismatch;
    CHECK(check_failure(&changed, UNITY_SHADER_CONTRACT_PLAYER_UNAVAILABLE) == 0);
    ShaderCatalogRecord unowned = *options->lift.record;
    changed = *options;
    changed.lift.record = &unowned;
    CHECK(check_failure(&changed, UNITY_SHADER_CONTRACT_LIFT_UNAVAILABLE) == 0);
    CHECK(check_failure(options, UNITY_SHADER_CONTRACT_LIFT_UNAVAILABLE) == 0);
    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(options->lift.broker, &stats);
    CHECK(stats.compiler_process_starts == 0);
    return 0;
}

static int check_live(const UnityShaderContractOptions *options) {
    UnityShaderContract *contract = NULL;
    UnityShaderContractReport report;
    const UnityShaderContractStatus status =
        unity_shader_contract_capture(options, &contract, &report);
    printf("contract=%s player=%s lift=%d transaction=%s source=%d import=%s gate=%s\n",
           unity_shader_contract_status_name(status),
           unity_player_package_status_name(report.player_status), report.lift_status,
           hlsl_lift_status_name(report.lift.lift_status),
           report.source_status, unity_shader_bundle_evidence_status_name(report.import_status),
           unity_shader_bundle_gate_status_name(report.import.gate_status));
    CHECK(status == UNITY_SHADER_CONTRACT_CAPTURED && contract);
    CHECK(report.source_published && report.import.gate.bundle_published);
    CHECK(report.constructed_plane_mask == WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK);
    const bool native_requested = options->native != NULL;
    if (native_requested) {
        printf("native=%s process=%s exit=%d file=%s input=%zu target=%d candidate=%d binding=%d\n",
               unity_native_runtime_status_name(report.native_status),
               common_process_status_name(report.native.process_status), report.native.exit_code,
               common_file_status_name(report.native.file_status), report.native.input_index,
               report.native.target_status, report.native.candidate_status, report.native.binding);
        CHECK(report.native_status == UNITY_NATIVE_RUNTIME_OK);
    }
    const UnityShaderLabLiftArtifact *accepted =
        unity_shaderlab_lift_accepted(unity_shader_contract_lift(contract));
    CHECK(accepted && accepted->high_level && accepted->unity_uv_helpers);
    uint8_t subject_digest[32];
    CHECK(whole_shader_subject_digest(unity_shader_contract_subject(contract), subject_digest) ==
          WHOLE_SHADER_SUBJECT_OK);
    for (unsigned plane = 0; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        if (!(WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK & WHOLE_SHADER_PLANE_BIT(plane))) {
            CHECK(!unity_shader_contract_evidence(contract, (WholeShaderVerificationPlane)plane));
            continue;
        }
        const WholeShaderEvidenceSummary *summary = &report.planes[plane];
        printf("%s=%s matched=%llu/%llu\n", whole_shader_verification_plane_name(summary->plane),
               whole_shader_plane_status_name(summary->status),
               (unsigned long long)summary->matched_item_count,
               (unsigned long long)summary->expected_item_count);
        CHECK(summary->status == (plane == WHOLE_SHADER_PLANE_RUNTIME_SELECTION && !native_requested
                                      ? WHOLE_SHADER_PLANE_UNAVAILABLE
                                      : WHOLE_SHADER_PLANE_PASS));
        CHECK(memcmp(summary->subject_digest, subject_digest, 32) == 0);
    }
    CHECK(!unity_shader_contract_evidence(contract, (WholeShaderVerificationPlane)-1));
    CHECK(!unity_shader_contract_evidence(contract, WHOLE_SHADER_PLANE_COUNT));
    CHECK(report.certificate.d3d11_byte_equivalence_certified);
    CHECK(report.certificate.importer_acceptance_certified);
    CHECK(report.certificate.release_object_equivalence_certified);
    CHECK(report.certificate.d3d11_logical_equivalence_certified == native_requested);
    CHECK(!report.certificate.source_identity_certified &&
          !report.certificate.universal_visual_equivalence_certified);
    CHECK(report.certificate.status == (native_requested
              ? WHOLE_SHADER_CERTIFICATE_OK : WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_UNAVAILABLE));
    CHECK(!report.certificate.finite_pixel_observations_certified &&
          !report.certificate.empirical_visual_equivalence_certified);
    CHECK(report.selection.availability == (native_requested
              ? UNITY_SHADER_SELECTION_AVAILABLE : UNITY_SHADER_SELECTION_NATIVE_UNAVAILABLE));
    CommonFileBytes published = {0};
    CHECK(common_file_read_regular(options->candidate_source_path, SIZE_MAX, &published) ==
          COMMON_FILE_OK);
    CHECK(published.size == accepted->source.len &&
          memcmp(published.data, accepted->source.buf, published.size) == 0);
    unity_shader_contract_free(contract);
    contract = NULL;
    /* A replay must retain the existing source and must not launch another import. */
    CHECK(check_failure(options, UNITY_SHADER_CONTRACT_SOURCE_UNAVAILABLE) == 0);
    CommonFileBytes retained = {0};
    CHECK(common_file_read_regular(options->candidate_source_path, SIZE_MAX, &retained) ==
          COMMON_FILE_OK);
    CHECK(published.size == retained.size &&
          memcmp(published.data, retained.data, published.size) == 0);
    common_file_bytes_dispose(&retained);
    common_file_bytes_dispose(&published);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 1 && argc != 8 && argc != 13) {
        fprintf(stderr,
                "Usage: %s [TARGET PROFILE PROJECT INCLUDES EDITOR PLAYER_ROOT OUTPUT_PREFIX "
                "[PYTHON CLIENT SSH_CONFIG POLICY JOBS]]\n",
                argv[0]);
        return 2;
    }
    const bool live = argc >= 8;
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(&registry, CONTRACT_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    ShaderCatalogOptions catalog_options;
    shader_catalog_options_default(&catalog_options);
    catalog_options.schema_registry = &registry;
    catalog_options.retain_source_snapshots = true;
    const char *target = live ? argv[1] : CONTRACT_EMPTY;
    CHECK(shader_catalog_build(&target, 1, &catalog_options, &catalog) == SHADER_CATALOG_OK);
    CHECK(catalog.record_count == 1 && shader_catalog_is_complete(&catalog));
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19;
    profile.valid_apis = 311856;
    profile.d3d11_capabilities = profile.glcore_capabilities = 147179016;
    strcpy(profile.provenance, "synthetic-contract-test");
    if (live)
        CHECK(unity_compile_profile_load(argv[2], &profile) == UNITY_COMPILE_PROFILE_OK);
    const char *project = live ? argv[3] : ".";
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(project, live ? argv[4] : NULL);
    CHECK(broker);
    HLSLLiftLimits limits = {2, 128, 60000};
    UnityShaderContractOptions options = {0};
    options.lift = (UnityShaderLabLiftCaptureInput){.catalog = &catalog,
                                                    .record = catalog.records,
                                                    .registry = &registry,
                                                    .profile = &profile,
                                                    .broker = broker,
                                                    .source_path = "Assets/Contract.shader",
                                                    .source_directory = project,
                                                    .source_basename = "Contract.shader",
                                                    .limits = &limits};
    options.player_root = live ? argv[6] : CONTRACT_PLAYER_ROOT;
    options.player_metadata_path =
        live ? "RuntimeProbe_Data/globalgamemanagers" : "metadata.assets";
    options.player_limits =
        (ShaderRuntimeCaptureLimits){256, 128, 16, 4096, 128 * 1024 * 1024, 256 * 1024 * 1024};
    char source[4096], bundle[4096], report[4096], log[4096];
    const char *prefix = live ? argv[7] : "absent-contract-output";
    CHECK(strlen(prefix) + 8 < sizeof(source));
    snprintf(source, sizeof(source), "%s.shader", prefix);
    snprintf(bundle, sizeof(bundle), "%s.bundle", prefix);
    snprintf(report, sizeof(report), "%s.json", prefix);
    snprintf(log, sizeof(log), "%s.log", prefix);
    options.candidate_source_path = source;
    unity_shader_bundle_gate_options_init(&options.bundle);
    options.bundle.unity_executable = live ? argv[5] : "absent-contract-editor";
    options.bundle.bridge_path = CONTRACT_BRIDGE;
    options.bundle.expected_unity_version = "2021.3.35f1";
    options.bundle.output_bundle_path = bundle;
    options.bundle.report_path = report;
    options.bundle.log_path = log;
    options.bundle.target = UNITY_SHADER_BUNDLE_TARGET_WINDOWS64;
    options.bundle.backend = UNITY_SHADER_BUNDLE_BACKEND_D3D11;
    options.bundle.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
    options.bundle.keep_policy = UNITY_SHADER_IMPORT_KEEP_ALWAYS;
    char observations[4096];
    UnityNativeRuntimeOptions native = {0};
    if (argc == 13) {
        CHECK(strlen(prefix) + 14 < sizeof(observations));
        snprintf(observations, sizeof(observations), "%s.observations", prefix);
        native.python = argv[8];
        native.client_directory = argv[9];
        native.ssh_config = argv[10];
        native.policy_path = argv[11];
        native.jobs_path = argv[12];
        native.output_path = observations;
        native.timeout_ms = 180000;
        options.native = &native;
    }
    const int result = live ? check_live(&options) : check_boundaries(&options);
    unity_compiler_broker_destroy(broker);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    return result;
}
