// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shader_contract.h"
#include "app/release_shader_evidence.h"

#include <stdlib.h>
#include <string.h>

struct UnityShaderContract {
    UnityShaderLabLiftCapture *lift;
    UnityShaderBundleAuthority *import;
    UnityPlayerPackageAuthority *player;
    UnityNativeRuntime *native;
    WholeShaderSubject *subject;
    WholeShaderEvidence *planes[WHOLE_SHADER_PLANE_COUNT];
};

void unity_shader_contract_free(UnityShaderContract *contract) {
    if (!contract)
        return;
    for (unsigned plane = 0; plane < WHOLE_SHADER_PLANE_COUNT; ++plane)
        whole_shader_evidence_free(contract->planes[plane]);
    whole_shader_subject_free(contract->subject);
    unity_shader_bundle_authority_free(contract->import);
    unity_player_package_free(contract->player);
    unity_native_runtime_free(contract->native);
    unity_shaderlab_lift_capture_free(contract->lift);
    free(contract);
}

const WholeShaderSubject *unity_shader_contract_subject(const UnityShaderContract *contract) {
    return contract ? contract->subject : NULL;
}

const WholeShaderEvidence *unity_shader_contract_evidence(const UnityShaderContract *contract,
                                                          WholeShaderVerificationPlane plane) {
    return contract && plane >= 0 && plane < WHOLE_SHADER_PLANE_COUNT ? contract->planes[plane]
                                                                      : NULL;
}

const UnityShaderLabLiftResult *unity_shader_contract_lift(const UnityShaderContract *contract) {
    return contract ? unity_shaderlab_lift_capture_result(contract->lift) : NULL;
}

static void initialize_report(UnityShaderContractReport *report) {
    memset(report, 0, sizeof(*report));
    report->status = UNITY_SHADER_CONTRACT_INVALID_ARGUMENT;
    report->player_status = UNITY_PLAYER_PACKAGE_INVALID_ARGUMENT;
    report->lift_status = UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT;
    report->source_status = COMMON_FILE_INVALID_ARGUMENT;
    report->import_status = UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT;
    report->import.gate_status = UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT;
    report->import.gate.unity_exit_code = -1;
    report->import.catalog_status = SHADER_CATALOG_INVALID_ARGUMENT;
    report->import.object_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    report->native_status = UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT;
    report->native.exit_code = -1;
    report->native.input_index = SIZE_MAX;
    for (unsigned plane = 0; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        report->planes[plane].plane = (WholeShaderVerificationPlane)plane;
        report->planes[plane].status =
            (WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK & WHOLE_SHADER_PLANE_BIT(plane))
                ? WHOLE_SHADER_PLANE_NOT_RUN
                : WHOLE_SHADER_PLANE_NOT_REQUESTED;
    }
    whole_shader_certificate_report_init(&report->certificate);
}

static bool bind_subject(UnityShaderContract *contract) {
    WholeShaderSubjectDescriptor descriptor;
    UnityShaderBundleAuthoritySummary imported;
    UnityPlayerPackageSummary player;
    if (!unity_shaderlab_lift_capture_subject(contract->lift, &descriptor) ||
        !unity_shader_bundle_authority_describe(contract->import, &imported) ||
        !unity_player_package_describe(contract->player, &player) ||
        memcmp(descriptor.candidate_source_digest, imported.source_digest, 32) != 0 ||
        memcmp(descriptor.compiler_profile_digest, player.player.compiler_profile_digest, 32) != 0)
        return false;
    memcpy(descriptor.player_profile_digest, player.player.profile_digest, 32);
    memcpy(descriptor.candidate_release_digest, imported.release_digest, 32);
    UnityNativeRuntimeSummary native = {0};
    if (contract->native) {
        if (!unity_native_runtime_describe(contract->native, &native))
            return false;
        memcpy(descriptor.runtime_environment_digest, native.native_environment_digest, 32);
    }
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char domain[] = "DXBCSandbox.ShaderContractProducer.v2";
    common_sha256_update(&hash, domain, sizeof(domain));
    common_sha256_update(&hash, imported.authority_digest, 32);
    common_sha256_update(&hash, player.package_digest, 32);
    common_sha256_update(&hash, native.authority_digest, 32);
    common_sha256_final(&hash, descriptor.producer_fingerprint);
    return whole_shader_subject_create(&contract->subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK;
}

static bool collect_evidence(UnityShaderContract *contract,
                             const UnityShaderContractOptions *options,
                             UnityShaderContractReport *report) {
    const ShaderCatalog *candidate = unity_shader_bundle_authority_catalog(contract->import);
    if (!candidate || !shader_catalog_is_complete(candidate) || candidate->record_count != 1)
        return false;
    const WholeShaderSubject *subject = contract->subject;
    const TypeTreeSchemaRegistry *registry = options->lift.registry;
    const WholeShaderVerificationPlane compile_planes[] = {
        WHOLE_SHADER_PLANE_VARIANT_DOMAIN, WHOLE_SHADER_PLANE_FULL_DXBC,
        WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS, WHOLE_SHADER_PLANE_REFLECTION_BINDING};
    for (size_t i = 0; i < sizeof(compile_planes) / sizeof(compile_planes[0]); ++i) {
        const WholeShaderVerificationPlane plane = compile_planes[i];
        if (unity_shaderlab_lift_capture_make_evidence(contract->lift, subject, plane,
                                                       &contract->planes[plane]) !=
            WHOLE_SHADER_EVIDENCE_OK)
            return false;
    }
    if (unity_shader_bundle_authority_make_evidence(
            contract->import, registry, subject, &contract->planes[WHOLE_SHADER_PLANE_IMPORTER]) !=
            WHOLE_SHADER_EVIDENCE_OK ||
        unity_player_profile_make_evidence(unity_player_package_profile(contract->player), subject,
                                           &contract->planes[WHOLE_SHADER_PLANE_PLAYER_PROFILE]) !=
            WHOLE_SHADER_EVIDENCE_OK)
        return false;
    ReleaseShaderEvidenceReport release_report;
    if (release_shader_make_reextraction_evidence(
            options->lift.catalog, options->lift.record, candidate, candidate->records, registry,
            subject, &contract->planes[WHOLE_SHADER_PLANE_REEXTRACTION],
            &release_report) != RELEASE_SHADER_EVIDENCE_OK ||
        release_shader_make_render_state_evidence(
            options->lift.catalog, options->lift.record, candidate, candidate->records, registry,
            subject, &contract->planes[WHOLE_SHADER_PLANE_RENDER_STATE],
            &release_report) != RELEASE_SHADER_EVIDENCE_OK)
        return false;
    UnityShaderLabStructuralEvidenceReport structural;
    UnityShaderDependencyEvidenceReport dependencies;
    if (unity_shaderlab_lift_capture_structural_evidence(
            contract->lift, candidate, candidate->records, registry, subject,
            &contract->planes[WHOLE_SHADER_PLANE_STRUCTURAL],
            &structural) != WHOLE_SHADER_EVIDENCE_OK ||
        unity_shaderlab_lift_capture_dependency_evidence(
            contract->lift, candidate, candidate->records, registry, subject,
            &contract->planes[WHOLE_SHADER_PLANE_DEPENDENCY_CLOSURE],
            &dependencies) != WHOLE_SHADER_EVIDENCE_OK)
        return false;

    if (unity_shaderlab_lift_capture_selection_evidence(
            contract->lift, candidate, candidate->records, registry, contract->player,
            contract->native, subject, &contract->planes[WHOLE_SHADER_PLANE_RUNTIME_SELECTION],
            &report->selection) != WHOLE_SHADER_EVIDENCE_OK)
        return false;

    WholeShaderCertificateInput *input = NULL;
    if (whole_shader_certificate_input_create(&input, subject,
                                              WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) !=
        WHOLE_SHADER_CERTIFICATE_OK)
        return false;
    bool complete = true;
    for (unsigned plane = 0; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        if (!(WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK & WHOLE_SHADER_PLANE_BIT(plane)))
            continue;
        if (whole_shader_evidence_describe(contract->planes[plane], &report->planes[plane]) !=
                WHOLE_SHADER_EVIDENCE_OK ||
            whole_shader_certificate_input_add_evidence(input, contract->planes[plane]) !=
                WHOLE_SHADER_CERTIFICATE_ADD_OK) {
            complete = false;
            break;
        }
        report->constructed_plane_mask |= WHOLE_SHADER_PLANE_BIT(plane);
    }
    if (complete)
        (void)whole_shader_certificate_evaluate(input, &report->certificate);
    whole_shader_certificate_input_free(input);
    return complete;
}

UnityShaderContractStatus unity_shader_contract_capture(const UnityShaderContractOptions *options,
                                                        UnityShaderContract **output,
                                                        UnityShaderContractReport *report) {
    if (output)
        *output = NULL;
    if (!report)
        return UNITY_SHADER_CONTRACT_INVALID_ARGUMENT;
    initialize_report(report);
    if (!options || !output || !options->candidate_source_path || !options->player_root ||
        !options->player_metadata_path || !options->lift.catalog || !options->lift.record ||
        !options->lift.broker || !options->lift.limits ||
        !unity_compile_profile_validate(options->lift.profile))
        return report->status;
    const size_t source_length = strlen(options->candidate_source_path);
    if (source_length < 8 ||
        strcmp(options->candidate_source_path + source_length - 7, ".shader") != 0)
        return report->status;
    UnityShaderBundleGateOptions bundle = options->bundle;
    bundle.inputs = &options->candidate_source_path;
    bundle.input_count = 1;
    if (!unity_shader_bundle_gate_options_validate(&bundle) ||
        bundle.target != UNITY_SHADER_BUNDLE_TARGET_WINDOWS64 ||
        bundle.backend != UNITY_SHADER_BUNDLE_BACKEND_D3D11 ||
        bundle.warning_policy != UNITY_SHADER_IMPORT_WARNINGS_FAIL ||
        strcmp(bundle.expected_unity_version, "2021.3.35f1") != 0)
        return report->status;
    UnityShaderContract *contract = calloc(1, sizeof(*contract));
    if (!contract) {
        report->status = UNITY_SHADER_CONTRACT_ALLOCATION_FAILED;
        return report->status;
    }
    report->status = UNITY_SHADER_CONTRACT_PLAYER_UNAVAILABLE;
    report->player_status = unity_player_package_capture_d3d11(
        options->player_root, options->player_metadata_path, &options->player_limits,
        options->lift.profile, 7, &contract->player, &report->player);
    if (report->player_status != UNITY_PLAYER_PACKAGE_OK)
        goto failure;
    report->status = UNITY_SHADER_CONTRACT_LIFT_UNAVAILABLE;
    report->lift_status =
        unity_shaderlab_lift_capture(&options->lift, &contract->lift, &report->lift);
    if (report->lift_status != UNITY_SHADERLAB_CAPTURE_OK)
        goto failure;
    const UnityShaderLabLiftArtifact *accepted =
        unity_shaderlab_lift_accepted(unity_shaderlab_lift_capture_result(contract->lift));
    if (!accepted)
        goto failure;
    report->status = UNITY_SHADER_CONTRACT_SOURCE_UNAVAILABLE;
    report->source_status = common_file_write_new_atomic(
        options->candidate_source_path, accepted->source.buf, accepted->source.len);
    if (report->source_status != COMMON_FILE_OK)
        goto failure;
    report->source_published = true;
    report->status = UNITY_SHADER_CONTRACT_IMPORT_UNAVAILABLE;
    report->import_status = unity_shader_bundle_capture(&bundle, options->lift.registry,
                                                        &contract->import, &report->import);
    if (report->import_status != UNITY_SHADER_BUNDLE_EVIDENCE_OK)
        goto failure;
    if (options->native) {
        const ShaderCatalog *candidate = unity_shader_bundle_authority_catalog(contract->import);
        if (!candidate || candidate->record_count != 1)
            goto failure;
        UnityNativeRuntimeOptions native = *options->native;
        native.target_catalog = options->lift.catalog;
        native.target_record = options->lift.record;
        native.candidate_catalog = candidate;
        native.candidate_record = candidate->records;
        native.registry = options->lift.registry;
        native.player = contract->player;
        report->native_status =
            unity_native_runtime_capture(&native, &contract->native, &report->native);
        /* Retrieval failure remains visible while the other ten independently
         * produced planes are retained. No stale native capture is reused. */
    }
    report->status = UNITY_SHADER_CONTRACT_SUBJECT_UNAVAILABLE;
    if (!bind_subject(contract))
        goto failure;
    report->status = UNITY_SHADER_CONTRACT_EVIDENCE_UNAVAILABLE;
    if (!collect_evidence(contract, options, report))
        goto failure;
    report->status = UNITY_SHADER_CONTRACT_CAPTURED;
    *output = contract;
    return report->status;
failure:
    unity_shader_contract_free(contract);
    return report->status;
}

const char *unity_shader_contract_status_name(UnityShaderContractStatus status) {
    switch (status) {
    case UNITY_SHADER_CONTRACT_CAPTURED:
        return "captured";
    case UNITY_SHADER_CONTRACT_INVALID_ARGUMENT:
        return "invalid-argument";
    case UNITY_SHADER_CONTRACT_ALLOCATION_FAILED:
        return "allocation-failed";
    case UNITY_SHADER_CONTRACT_PLAYER_UNAVAILABLE:
        return "player-unavailable";
    case UNITY_SHADER_CONTRACT_LIFT_UNAVAILABLE:
        return "lift-unavailable";
    case UNITY_SHADER_CONTRACT_SOURCE_UNAVAILABLE:
        return "source-unavailable";
    case UNITY_SHADER_CONTRACT_IMPORT_UNAVAILABLE:
        return "import-unavailable";
    case UNITY_SHADER_CONTRACT_SUBJECT_UNAVAILABLE:
        return "subject-unavailable";
    case UNITY_SHADER_CONTRACT_EVIDENCE_UNAVAILABLE:
        return "evidence-unavailable";
    }
    return "unknown";
}
