// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADER_IMPORT_GATE_H
#define UNITY_SHADER_IMPORT_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Portable launcher for Unity's ShaderImporter validation boundary.  The
 * launcher itself is C11; Unity's required Editor entry point is supplied as
 * a small C# source file through bridge_path.  Every run creates a new Unity
 * project and never opens or modifies a caller-owned project.
 */

typedef enum {
    UNITY_SHADER_IMPORT_WARNINGS_ALLOW = 0,
    UNITY_SHADER_IMPORT_WARNINGS_FAIL,
} UnityShaderImportWarningPolicy;

typedef enum {
    UNITY_SHADER_IMPORT_KEEP_ON_FAILURE = 0,
    UNITY_SHADER_IMPORT_KEEP_ALWAYS,
    UNITY_SHADER_IMPORT_KEEP_NEVER,
} UnityShaderImportKeepPolicy;

typedef enum {
    UNITY_SHADER_IMPORT_GATE_OK = 0,
    UNITY_SHADER_IMPORT_GATE_DIAGNOSTICS_FOUND,
    UNITY_SHADER_IMPORT_GATE_INVALID_ARGUMENT,
    UNITY_SHADER_IMPORT_GATE_DISCOVERY_FAILED,
    UNITY_SHADER_IMPORT_GATE_NO_CANDIDATES,
    UNITY_SHADER_IMPORT_GATE_ALLOCATION_FAILED,
    UNITY_SHADER_IMPORT_GATE_IO_FAILED,
    UNITY_SHADER_IMPORT_GATE_PROCESS_FAILED,
    UNITY_SHADER_IMPORT_GATE_RESULT_INVALID,
    UNITY_SHADER_IMPORT_GATE_PATH_TOO_LONG,
} UnityShaderImportGateStatus;

typedef struct {
    const char* unity_executable;
    const char* expected_unity_version;
    const char* bridge_path;
    const char* report_path;
    const char* log_path;
    const char* temporary_root;
    const char* const* inputs;
    size_t input_count;
    UnityShaderImportWarningPolicy warning_policy;
    UnityShaderImportKeepPolicy keep_policy;
} UnityShaderImportGateOptions;

typedef struct {
    uint64_t candidate_count;
    uint64_t message_count;
    uint64_t error_count;
    uint64_t warning_count;
    bool passed;
    UnityShaderImportWarningPolicy warning_policy;
} UnityShaderImportGateSummary;

#define UNITY_SHADER_IMPORT_GATE_WORKSPACE_CAPACITY 4096U

typedef struct {
    UnityShaderImportGateSummary summary;
    int unity_exit_code;
    bool workspace_preserved;
    char workspace_path[UNITY_SHADER_IMPORT_GATE_WORKSPACE_CAPACITY];
} UnityShaderImportGateRunResult;

void unity_shader_import_gate_options_init(
    UnityShaderImportGateOptions* options);

bool unity_shader_import_gate_options_validate(
    const UnityShaderImportGateOptions* options);

/*
 * Strictly validates the stable JSON schema emitted by the Editor bridge.
 * Counts are recomputed from the nested shader/message arrays and must agree
 * with the declared totals.  On failure, out_summary is unchanged.
 */
UnityShaderImportGateStatus unity_shader_import_gate_parse_result_json(
    const uint8_t* json, size_t json_size,
    UnityShaderImportGateSummary* out_summary);

/*
 * Runs the complete gate.  A valid diagnostic report is published even when
 * the status is UNITY_SHADER_IMPORT_GATE_DIAGNOSTICS_FOUND.  Infrastructure
 * failure preserves the isolated project under KEEP_ON_FAILURE.
 */
UnityShaderImportGateStatus unity_shader_import_gate_run(
    const UnityShaderImportGateOptions* options,
    UnityShaderImportGateRunResult* out_result);

const char* unity_shader_import_gate_status_name(
    UnityShaderImportGateStatus status);

#endif /* UNITY_SHADER_IMPORT_GATE_H */
