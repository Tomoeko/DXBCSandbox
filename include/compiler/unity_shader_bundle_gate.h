// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADER_BUNDLE_GATE_H
#define UNITY_SHADER_BUNDLE_GATE_H

#include "compiler/unity_shader_import_gate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Isolated Unity ShaderImporter + AssetBundle publication boundary.
 *
 * The launcher is portable C11.  It creates a disposable Unity project,
 * copies only the selected .shader candidates and the supplied Editor bridge,
 * pins the Editor version, selects one explicit BuildTarget/graphics backend,
 * and atomically publishes a single bundle without overwriting an existing
 * destination.  Caller-owned Unity projects are never opened or modified.
 *
 * This gate proves import/build diagnostics and produces a Release bundle for
 * subsequent portable re-extraction.  It does not itself claim Shader-object,
 * DXBC, GLSL, runtime-input, or pixel equivalence.
 */

typedef enum {
    UNITY_SHADER_BUNDLE_TARGET_MACOS = 0,
    UNITY_SHADER_BUNDLE_TARGET_WINDOWS64,
    UNITY_SHADER_BUNDLE_TARGET_LINUX64,
} UnityShaderBundleTarget;

typedef enum {
    UNITY_SHADER_BUNDLE_BACKEND_METAL = 0,
    UNITY_SHADER_BUNDLE_BACKEND_D3D11,
    UNITY_SHADER_BUNDLE_BACKEND_OPENGLCORE,
    UNITY_SHADER_BUNDLE_BACKEND_VULKAN,
} UnityShaderBundleBackend;

typedef enum {
    UNITY_SHADER_BUNDLE_GATE_OK = 0,
    UNITY_SHADER_BUNDLE_GATE_DIAGNOSTICS_FOUND,
    UNITY_SHADER_BUNDLE_GATE_TARGET_UNAVAILABLE,
    UNITY_SHADER_BUNDLE_GATE_BACKEND_UNAVAILABLE,
    UNITY_SHADER_BUNDLE_GATE_BUILD_FAILED,
    UNITY_SHADER_BUNDLE_GATE_OUTPUT_EXISTS,
    UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT,
    UNITY_SHADER_BUNDLE_GATE_DISCOVERY_FAILED,
    UNITY_SHADER_BUNDLE_GATE_NO_CANDIDATES,
    UNITY_SHADER_BUNDLE_GATE_ALLOCATION_FAILED,
    UNITY_SHADER_BUNDLE_GATE_IO_FAILED,
    UNITY_SHADER_BUNDLE_GATE_PROCESS_FAILED,
    UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID,
    UNITY_SHADER_BUNDLE_GATE_PATH_TOO_LONG,
} UnityShaderBundleGateStatus;

typedef struct {
    const char* unity_executable;
    const char* expected_unity_version;
    const char* bridge_path;
    const char* report_path;
    const char* log_path;
    const char* output_bundle_path;
    const char* temporary_root;
    const char* const* inputs;
    size_t input_count;
    UnityShaderImportWarningPolicy warning_policy;
    UnityShaderImportKeepPolicy keep_policy;
    UnityShaderBundleTarget target;
    UnityShaderBundleBackend backend;
} UnityShaderBundleGateOptions;

#define UNITY_SHADER_BUNDLE_SHA256_CAPACITY 65U
#define UNITY_SHADER_BUNDLE_PATH_CAPACITY 4096U
#define UNITY_SHADER_BUNDLE_VERSION_CAPACITY 64U

typedef struct {
    uint64_t candidate_count;
    uint64_t message_count;
    uint64_t error_count;
    uint64_t warning_count;
    uint64_t bundle_size;
    bool passed;
    bool all_candidates_imported;
    bool bundle_built;
    UnityShaderImportWarningPolicy warning_policy;
    UnityShaderBundleTarget target;
    UnityShaderBundleBackend backend;
    UnityShaderBundleGateStatus reported_status;
    char unity_version[UNITY_SHADER_BUNDLE_VERSION_CAPACITY];
    char output_bundle_path[UNITY_SHADER_BUNDLE_PATH_CAPACITY];
    char bundle_sha256[UNITY_SHADER_BUNDLE_SHA256_CAPACITY];
    /* Parser-computed digest of ordered source_path/asset_path/
     * bundle_asset_name records. The launcher compares this against its own
     * discovered candidate list rather than trusting bridge counts alone. */
    char candidate_mapping_sha256[UNITY_SHADER_BUNDLE_SHA256_CAPACITY];
} UnityShaderBundleGateSummary;

#define UNITY_SHADER_BUNDLE_WORKSPACE_CAPACITY 4096U

typedef struct {
    UnityShaderBundleGateSummary summary;
    int unity_exit_code;
    bool workspace_preserved;
    bool bundle_published;
    char workspace_path[UNITY_SHADER_BUNDLE_WORKSPACE_CAPACITY];
} UnityShaderBundleGateRunResult;

void unity_shader_bundle_gate_options_init(
    UnityShaderBundleGateOptions* options);

bool unity_shader_bundle_gate_options_validate(
    const UnityShaderBundleGateOptions* options);

/* True only for backend/target pairs for which the bridge has an explicit,
 * pinned mapping.  Installed module availability remains a live Editor fact
 * and is reported separately as TARGET_UNAVAILABLE. */
bool unity_shader_bundle_target_supports_backend(
    UnityShaderBundleTarget target, UnityShaderBundleBackend backend);

/* Stable Unity .meta GUID used for the candidate at a particular manifest
 * index. The domain-separated digest includes the exact source bytes and the
 * index so duplicate shaders remain distinct without using host paths. */
bool unity_shader_bundle_candidate_guid(
    const void* source, size_t source_size, uint64_t candidate_index,
    char guid[33]);

/* Strictly validates the stable bridge report.  Nested diagnostic totals and
 * shader counts are recomputed.  A claimed pass requires every candidate to
 * import, a nonempty bundle hash/path, and a built bundle.  On failure the
 * output is unchanged. */
UnityShaderBundleGateStatus unity_shader_bundle_gate_parse_result_json(
    const uint8_t* json, size_t json_size,
    UnityShaderBundleGateSummary* out_summary);

UnityShaderBundleGateStatus unity_shader_bundle_gate_run(
    const UnityShaderBundleGateOptions* options,
    UnityShaderBundleGateRunResult* out_result);

const char* unity_shader_bundle_target_name(UnityShaderBundleTarget target);
const char* unity_shader_bundle_backend_name(UnityShaderBundleBackend backend);
const char* unity_shader_bundle_gate_status_name(
    UnityShaderBundleGateStatus status);

#endif /* UNITY_SHADER_BUNDLE_GATE_H */
