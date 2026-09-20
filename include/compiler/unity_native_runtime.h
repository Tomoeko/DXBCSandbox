// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_NATIVE_RUNTIME_H
#define UNITY_NATIVE_RUNTIME_H

#include "app/shader_catalog_object.h"
#include "compiler/unity_player_profile.h"
#include "common/process.h"

typedef struct UnityNativeRuntime UnityNativeRuntime;

typedef enum {
    UNITY_NATIVE_RUNTIME_OK = 0,
    UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT,
    UNITY_NATIVE_RUNTIME_SOURCE_UNAVAILABLE,
    UNITY_NATIVE_RUNTIME_TOOL_UNAVAILABLE,
    UNITY_NATIVE_RUNTIME_OUTPUT_EXISTS,
    UNITY_NATIVE_RUNTIME_PROCESS_FAILED,
    UNITY_NATIVE_RUNTIME_CAPTURE_INVALID,
    UNITY_NATIVE_RUNTIME_BINDING_MISMATCH,
    UNITY_NATIVE_RUNTIME_OBSERVATIONS_DIFFER,
    UNITY_NATIVE_RUNTIME_INPUT_CHANGED,
    UNITY_NATIVE_RUNTIME_ALLOCATION_FAILED
} UnityNativeRuntimeStatus;

typedef struct {
    const ShaderCatalog *target_catalog;
    const ShaderCatalogRecord *target_record;
    const ShaderCatalog *candidate_catalog;
    const ShaderCatalogRecord *candidate_record;
    const TypeTreeSchemaRegistry *registry;
    const UnityPlayerPackageAuthority *player;
    /* Trusted selected Python executable and D3D11Validation client directory.
     * Use real absolute paths, not executable symlinks or PATH searches. */
    const char *python;
    const char *client_directory;
    const char *ssh_config;
    const char *policy_path;
    const char *jobs_path;
    const char *output_path; /* New private observation artifact; never overwrite. */
    uint32_t timeout_ms; /* 1..600000, including the twelve authenticated retrievals. */
} UnityNativeRuntimeOptions;

typedef struct {
    uint32_t member_count;
    uint32_t observation_count;
    uint32_t equal_pair_count;
    uint8_t deployment_digest[32];
    uint8_t package_manifest_digest[32];
    uint8_t native_environment_digest[32];
    uint8_t worker_epoch[32];
    uint8_t target_artifact_digest[32];
    uint8_t candidate_artifact_digest[32];
    uint8_t target_release_digest[32];
    uint8_t candidate_release_digest[32];
    uint8_t player_package_digest[32];
    uint8_t tool_digest[32];
    uint8_t observation_digest[32];
    uint8_t authority_digest[32];
} UnityNativeRuntimeSummary;

typedef struct {
    CommonProcessStatus process_status;
    int exit_code;
    CommonFileStatus file_status;
    ShaderCatalogObjectStatus target_status;
    ShaderCatalogObjectStatus candidate_status;
    size_t input_index; /* SIZE_MAX unless a held input failed. */
} UnityNativeRuntimeDiagnostic;

/* Invoke D3D11Validation's authenticated collector, binding both bundle hashes
 * to independently decoded retained catalogs. Hold selected interpreter/client,
 * configuration, job and policy-file snapshots throughout retrieval; reject
 * observed drift. Validate the binary framing, every package member against the
 * opaque captured player, and full DXBC/raw pixels for each of six paired cases.
 * No callback or caller-written report can construct this owner.
 *
 * This is finite native observation authority, not runtime winner selection or
 * logical equivalence. It asserts neither complete loaded-module closure nor
 * counter absence between samples. The selected Python/SSH installation is a
 * trusted toolchain, not a fully inventoried interpreter dependency closure.
 * Only a subsequent selection producer may combine it with the closed shader
 * contract. A failure leaves output NULL; the private artifact remains available
 * for diagnosis and cannot be reused as capture authority. */
UnityNativeRuntimeStatus unity_native_runtime_capture(const UnityNativeRuntimeOptions *options,
                                                       UnityNativeRuntime **output,
                                                       UnityNativeRuntimeDiagnostic *diagnostic);
bool unity_native_runtime_describe(const UnityNativeRuntime *runtime,
                                   UnityNativeRuntimeSummary *summary);
void unity_native_runtime_free(UnityNativeRuntime *runtime);
const char *unity_native_runtime_status_name(UnityNativeRuntimeStatus status);

#endif
