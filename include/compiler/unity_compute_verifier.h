// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPUTE_VERIFIER_H
#define UNITY_COMPUTE_VERIFIER_H

#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_stage_contract.h"
#include "io/unity_compute_binary.h"

/* This is an independent comparison of one selected expected kernel against
 * one complete native ComputeShaderBinary payload. The expectation is borrowed
 * caller-owned data, not proof that it came from an immutable player object.
 * Compiler identity, preprocessing controls, keyword selection, source quality,
 * import, runtime and semantic equivalence remain separate obligations. */
typedef struct {
    ComputeShaderStringView kernel_name;
    const ComputeShaderKernelVariant* variant;
    int32_t target_level;
    bool resources_resolved;
    /* Explicit selected declarations are reserved for a proven player/native
     * selection relationship. Nonempty declarations or player variant indices
     * currently return UNSUPPORTED_SELECTION, never a guessed comparison. */
    const ComputeShaderConstantBuffer* selected_buffer_definitions;
    size_t selected_buffer_definition_count;
} UnityComputeKernelExpectation;

typedef enum {
    UNITY_COMPUTE_VERIFY_OK = 0,
    UNITY_COMPUTE_VERIFY_INVALID_ARGUMENT,
    UNITY_COMPUTE_VERIFY_EXPECTATION_INVALID,
    UNITY_COMPUTE_VERIFY_NATIVE_PAYLOAD_INVALID,
    UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION,
    UNITY_COMPUTE_VERIFY_NATIVE_KERNEL_COUNT,
    UNITY_COMPUTE_VERIFY_KERNEL_NAME_MISMATCH,
    UNITY_COMPUTE_VERIFY_DXBC_MISMATCH,
    UNITY_COMPUTE_VERIFY_STAGE_INVALID,
    UNITY_COMPUTE_VERIFY_THREAD_GROUP_MISMATCH,
    UNITY_COMPUTE_VERIFY_TARGET_LEVEL_MISMATCH,
    UNITY_COMPUTE_VERIFY_RESOLUTION_MISMATCH,
    UNITY_COMPUTE_VERIFY_RESOURCE_MISMATCH,
    UNITY_COMPUTE_VERIFY_SAMPLER_MISMATCH
} UnityComputeVerifyStatus;

typedef enum {
    UNITY_COMPUTE_VERIFY_RESOURCE_NONE = 0,
    UNITY_COMPUTE_VERIFY_RESOURCE_CONSTANT_BUFFER,
    UNITY_COMPUTE_VERIFY_RESOURCE_TEXTURE,
    UNITY_COMPUTE_VERIFY_RESOURCE_INPUT_BUFFER,
    UNITY_COMPUTE_VERIFY_RESOURCE_OUTPUT_BUFFER,
    UNITY_COMPUTE_VERIFY_RESOURCE_BUILTIN_SAMPLER
} UnityComputeVerifyResourceRole;

typedef struct {
    UnityComputeVerifyStatus status;
    ComputeShaderObjectStatus native_decode_status;
    DXBCCompareResult dxbc;
    DXBCStageContractDiagnostic expected_stage;
    DXBCStageContractDiagnostic actual_stage;
    bool dxbc_compared;
    bool dxbc_equal;
    bool expected_stage_valid;
    bool actual_stage_valid;
    bool expected_group_matches_code;
    bool actual_group_matches_code;
    bool common_metadata_compared;
    bool common_metadata_equal;
    UnityComputeVerifyResourceRole resource_role;
    size_t resource_index; /* SIZE_MAX when no individual record differs. */
    size_t native_kernel_count;
    size_t native_buffer_variant_count;
} UnityComputeVerifyReport;

void unity_compute_verify_report_init(UnityComputeVerifyReport* report);

/* Always reparses the complete raw native payload with the existing bounded
 * decoder; caller-created decoded models cannot stand in for observed bytes.
 * Initial scope is exactly one kernel and one empty native buffer variant,
 * with no player buffer-selection indices or selected declarations. Ordered
 * common resources, every retained name/binding/dimension field, builtin
 * sampler states, target level, resolution flag and groups are compared.
 * Native keyword keys and requirements are absent, so are never inferred or
 * compared. Complete DXBC equality includes checksums and unknown/padding
 * bytes. The report owns no storage and is reset on every call. */
UnityComputeVerifyStatus unity_compute_verify_kernel(const UnityComputeKernelExpectation* expected,
                                                     const uint8_t* native_payload,
                                                     size_t native_payload_size,
                                                     UnityComputeVerifyReport* report);

const char* unity_compute_verify_status_name(UnityComputeVerifyStatus status);

#endif
