// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_FINITE_VISUAL_GATE_H
#define UNITY_FINITE_VISUAL_GATE_H

#include "app/whole_shader_evidence.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Isolated finite-observation visual gate.
 *
 * This is deliberately not a universal visual-equivalence oracle. One run
 * binds one baseline ShaderLab file, one candidate ShaderLab file, one
 * canonical finite-fixture manifest, an exact Unity Editor version, and one
 * graphics backend. The managed bridge renders the same explicit mesh and
 * material inputs twice with each shader, reads an uncompressed RGBAFloat
 * image, and compares the bytes. The C11 launcher validates and publishes the
 * report and raw artifacts without opening a caller-owned Unity project.
 */

#define UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY 65U
#define UNITY_FINITE_VISUAL_PATH_CAPACITY 4096U
#define UNITY_FINITE_VISUAL_VERSION_CAPACITY 64U
#define UNITY_FINITE_VISUAL_DEVICE_CAPACITY 512U

typedef enum {
    UNITY_FINITE_VISUAL_BACKEND_METAL = 0,
    UNITY_FINITE_VISUAL_BACKEND_D3D11,
    UNITY_FINITE_VISUAL_BACKEND_OPENGLCORE,
    UNITY_FINITE_VISUAL_BACKEND_VULKAN,
    /* Report-only value. It is never a valid requested backend. */
    UNITY_FINITE_VISUAL_BACKEND_UNSUPPORTED,
} UnityFiniteVisualBackend;

typedef enum {
    UNITY_FINITE_VISUAL_KEEP_ON_FAILURE = 0,
    UNITY_FINITE_VISUAL_KEEP_ALWAYS,
    UNITY_FINITE_VISUAL_KEEP_NEVER,
} UnityFiniteVisualKeepPolicy;

typedef enum {
    UNITY_FINITE_VISUAL_GATE_OK = 0,
    UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH,
    UNITY_FINITE_VISUAL_GATE_DIAGNOSTICS_FOUND,
    UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC,
    UNITY_FINITE_VISUAL_GATE_UNSUPPORTED_FIXTURE,
    UNITY_FINITE_VISUAL_GATE_INVALID_ARGUMENT,
    UNITY_FINITE_VISUAL_GATE_OUTPUT_EXISTS,
    UNITY_FINITE_VISUAL_GATE_ALLOCATION_FAILED,
    UNITY_FINITE_VISUAL_GATE_IO_FAILED,
    UNITY_FINITE_VISUAL_GATE_PROCESS_FAILED,
    UNITY_FINITE_VISUAL_GATE_RESULT_INVALID,
    UNITY_FINITE_VISUAL_GATE_PATH_TOO_LONG,
} UnityFiniteVisualGateStatus;

typedef struct {
    const char* unity_executable;
    const char* expected_unity_version;
    const char* bridge_path;
    const char* baseline_shader_path;
    const char* candidate_shader_path;
    const char* fixture_path;
    const char* report_path;
    const char* log_path;
    const char* baseline_pixels_path;
    const char* candidate_pixels_path;
    const char* temporary_root;
    UnityFiniteVisualBackend backend;
    UnityFiniteVisualKeepPolicy keep_policy;
} UnityFiniteVisualGateOptions;

typedef struct {
    UnityFiniteVisualGateStatus reported_status;
    UnityFiniteVisualBackend backend;
    bool passed;
    bool baseline_imported;
    bool candidate_imported;
    bool baseline_stable;
    bool candidate_stable;
    bool pixel_equal;
    uint32_t width;
    uint32_t height;
    uint32_t pass_index;
    uint64_t pixel_byte_count;
    uint64_t first_mismatch_offset;
    uint64_t diagnostic_count;
    uint64_t error_count;
    uint64_t warning_count;
    uint64_t failure_count;
    char unity_version[UNITY_FINITE_VISUAL_VERSION_CAPACITY];
    char requested_color_space[16];
    char color_space[16];
    char graphics_device_name[UNITY_FINITE_VISUAL_DEVICE_CAPACITY];
    char graphics_device_vendor[UNITY_FINITE_VISUAL_DEVICE_CAPACITY];
    char graphics_device_version[UNITY_FINITE_VISUAL_DEVICE_CAPACITY];
    char fixture_sha256[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char bridge_sha256[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char baseline_source_sha256[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char candidate_source_sha256[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char baseline_pixels_sha256[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char candidate_pixels_sha256[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    /* C-computed evidence coordinates; these are never accepted from the
     * managed report as trusted PASS fields. */
    uint8_t render_case_identity[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t authority_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t expected_runtime_inputs_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t observed_runtime_inputs_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
} UnityFiniteVisualGateSummary;

typedef struct {
    UnityFiniteVisualGateSummary summary;
    int unity_exit_code;
    bool workspace_preserved;
    bool report_published;
    bool baseline_pixels_published;
    bool candidate_pixels_published;
    char workspace_path[UNITY_FINITE_VISUAL_PATH_CAPACITY];
} UnityFiniteVisualGateRunResult;

void unity_finite_visual_gate_options_init(
    UnityFiniteVisualGateOptions* options);
bool unity_finite_visual_gate_options_validate(
    const UnityFiniteVisualGateOptions* options);

/* Strict parser for the line-oriented v1 bridge certificate. Unknown,
 * duplicated, malformed, or internally inconsistent records fail closed.
 * On failure, out_summary is unchanged. */
UnityFiniteVisualGateStatus unity_finite_visual_gate_parse_result(
    const uint8_t* data, size_t size,
    UnityFiniteVisualGateSummary* out_summary);

UnityFiniteVisualGateStatus unity_finite_visual_gate_run(
    const UnityFiniteVisualGateOptions* options,
    UnityFiniteVisualGateRunResult* out_result);

/* Creates the two evidence planes produced by this finite gate. Both use the
 * same render-case identity, so the whole-shader certificate can reject a
 * split fixture scope. Pixel PASS remains a finite observation only. */
WholeShaderEvidenceStatus unity_finite_visual_gate_make_evidence(
    const UnityFiniteVisualGateSummary* summary,
    const WholeShaderSubject* subject,
    WholeShaderEvidence** out_runtime_inputs,
    WholeShaderEvidence** out_empirical_pixels);

const char* unity_finite_visual_backend_name(UnityFiniteVisualBackend backend);
const char* unity_finite_visual_gate_status_name(
    UnityFiniteVisualGateStatus status);

#endif /* UNITY_FINITE_VISUAL_GATE_H */
