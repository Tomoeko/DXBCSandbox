// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_SHADER_CONTRACT_H
#define UNITY_SHADER_CONTRACT_H

#include "compiler/unity_shaderlab_lift_capture.h"
#include "compiler/unity_shader_bundle_evidence.h"
#include "compiler/unity_player_profile.h"

typedef struct UnityShaderContract UnityShaderContract;

typedef enum {
    UNITY_SHADER_CONTRACT_CAPTURED = 0,
    UNITY_SHADER_CONTRACT_INVALID_ARGUMENT,
    UNITY_SHADER_CONTRACT_ALLOCATION_FAILED,
    UNITY_SHADER_CONTRACT_PLAYER_UNAVAILABLE,
    UNITY_SHADER_CONTRACT_LIFT_UNAVAILABLE,
    UNITY_SHADER_CONTRACT_SOURCE_UNAVAILABLE,
    UNITY_SHADER_CONTRACT_IMPORT_UNAVAILABLE,
    UNITY_SHADER_CONTRACT_SUBJECT_UNAVAILABLE,
    UNITY_SHADER_CONTRACT_EVIDENCE_UNAVAILABLE
} UnityShaderContractStatus;

typedef struct {
    UnityShaderLabLiftCaptureInput lift;
    /* inputs/input_count are replaced with the actual accepted source below. */
    UnityShaderBundleGateOptions bundle;
    const char *candidate_source_path; /* New .shader file; never overwritten. */
    const char *player_root;
    const char *player_metadata_path;
    ShaderRuntimeCaptureLimits player_limits;
} UnityShaderContractOptions;

typedef struct {
    UnityShaderContractStatus status;
    UnityPlayerPackageStatus player_status;
    UnityPlayerPackageDiagnostic player;
    UnityShaderLabLiftCaptureStatus lift_status;
    UnityShaderLabLiftCaptureReport lift;
    CommonFileStatus source_status;
    bool source_published;
    UnityShaderBundleEvidenceStatus import_status;
    UnityShaderBundleEvidenceDiagnostic import;
    uint64_t constructed_plane_mask;
    WholeShaderEvidenceSummary planes[WHOLE_SHADER_PLANE_COUNT];
    WholeShaderCertificateReport certificate;
} UnityShaderContractReport;

/* Execute production capture, import and comparison, never aggregate caller
 * reports. One accepted source feeds both the compiler authority and the actual
 * isolated import. The resulting catalog supplies every candidate comparison.
 * Player metadata is decoded from its revalidated package. Every evidence plane
 * shares one subject; the complete D3D11 logical mask is always requested.
 *
 * CAPTURED means evidence collection completed, not equivalence. Inspect the
 * derived certificate and per-plane outcomes. Runtime selection currently has
 * no connected production authority and therefore yields explicit UNAVAILABLE.
 * There is no switch to promote package membership or finite pixels to it.
 * Failed calls leave output NULL. Published source/bundle/logs remain available
 * for diagnosis, with their individual status recorded; they are not a partial
 * whole-shader certificate. Borrowed inputs must remain valid during the call. */
UnityShaderContractStatus unity_shader_contract_capture(const UnityShaderContractOptions *options,
                                                        UnityShaderContract **output,
                                                        UnityShaderContractReport *report);
const WholeShaderSubject *unity_shader_contract_subject(const UnityShaderContract *contract);
const WholeShaderEvidence *unity_shader_contract_evidence(const UnityShaderContract *contract,
                                                          WholeShaderVerificationPlane plane);
const UnityShaderLabLiftResult *unity_shader_contract_lift(const UnityShaderContract *contract);
void unity_shader_contract_free(UnityShaderContract *contract);
const char *unity_shader_contract_status_name(UnityShaderContractStatus status);

#endif
