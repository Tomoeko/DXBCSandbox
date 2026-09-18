// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_finite_visual_gate.h"

#include "common/executable_resource.h"
#include "common/windows_utf8.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNITY_FINITE_VISUAL_BRIDGE_RESOURCE \
    "dxbc-sandbox/unity/Editor/DXBCFiniteVisualGate.cs"

static void print_usage(FILE* stream, const char* executable) {
    fprintf(stream,
        "Usage: %s --unity PATH --unity-version VERSION --backend BACKEND \\\n"
        "          --baseline FILE --candidate FILE --fixture FILE \\\n"
        "          --report FILE --log FILE --baseline-pixels FILE \\\n"
        "          --candidate-pixels FILE [OPTIONS]\n\n"
        "Renders one explicit finite fixture against a baseline and candidate\n"
        "ShaderLab file in a fresh isolated Unity project. Both shaders are\n"
        "rendered twice. Import warnings/errors, nondeterminism, and raw\n"
        "RGBAFloat byte differences fail closed. This is finite empirical\n"
        "evidence, never a universal visual-equivalence certificate.\n\n"
        "Required:\n"
        "  --unity PATH             Exact Unity Editor executable\n"
        "  --unity-version VERSION  Required Application.unityVersion\n"
        "  --backend BACKEND        metal|d3d11|openglcore|vulkan\n"
        "  --baseline FILE          Baseline ShaderLab source\n"
        "  --candidate FILE         Candidate ShaderLab source\n"
        "  --fixture FILE           Canonical v1 finite-fixture TSV\n"
        "  --report FILE            Validated line-oriented report\n"
        "  --log FILE               Complete Unity Editor log\n"
        "  --baseline-pixels FILE   Baseline raw RGBA32F output\n"
        "  --candidate-pixels FILE  Candidate raw RGBA32F output\n\n"
        "Options:\n"
        "  --bridge FILE            Editor bridge source (default: packaged)\n"
        "  --keep on-failure|always|never\n"
        "                           Isolated-project retention policy\n"
        "                           (default: on-failure)\n"
        "  --temp-root PATH         Existing parent for isolated projects\n"
        "  -h, --help               Show this help\n",
        executable);
}

static const char* option_value(int argc, char** argv, int* index,
                                const char* option) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "%s requires a value\n", option);
        return NULL;
    }
    ++*index;
    return argv[*index];
}

static bool parse_backend(const char* value,
                          UnityFiniteVisualBackend* backend) {
    if (!value || !backend) return false;
    for (int candidate = (int)UNITY_FINITE_VISUAL_BACKEND_METAL;
         candidate <= (int)UNITY_FINITE_VISUAL_BACKEND_VULKAN; ++candidate) {
        if (strcmp(value, unity_finite_visual_backend_name(
                              (UnityFiniteVisualBackend)candidate)) == 0) {
            *backend = (UnityFiniteVisualBackend)candidate;
            return true;
        }
    }
    return false;
}

static int unity_finite_visual_gate_main(int argc, char** argv) {
    UnityFiniteVisualGateOptions options;
    unity_finite_visual_gate_options_init(&options);
    bool parse_ok = true;
    bool backend_set = false;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (strcmp(argument, "-h") == 0 ||
            strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        const char* value = NULL;
        if (strcmp(argument, "--unity") == 0) {
            options.unity_executable = option_value(
                argc, argv, &index, argument);
            parse_ok = options.unity_executable != NULL;
        } else if (strcmp(argument, "--unity-version") == 0) {
            options.expected_unity_version = option_value(
                argc, argv, &index, argument);
            parse_ok = options.expected_unity_version != NULL;
        } else if (strcmp(argument, "--backend") == 0) {
            value = option_value(argc, argv, &index, argument);
            backend_set = value && parse_backend(value, &options.backend);
            if (!backend_set && value) {
                fprintf(stderr, "invalid --backend value: %s\n", value);
            }
            parse_ok = backend_set;
        } else if (strcmp(argument, "--bridge") == 0) {
            options.bridge_path = option_value(argc, argv, &index, argument);
            parse_ok = options.bridge_path != NULL;
        } else if (strcmp(argument, "--baseline") == 0) {
            options.baseline_shader_path = option_value(
                argc, argv, &index, argument);
            parse_ok = options.baseline_shader_path != NULL;
        } else if (strcmp(argument, "--candidate") == 0) {
            options.candidate_shader_path = option_value(
                argc, argv, &index, argument);
            parse_ok = options.candidate_shader_path != NULL;
        } else if (strcmp(argument, "--fixture") == 0) {
            options.fixture_path = option_value(argc, argv, &index, argument);
            parse_ok = options.fixture_path != NULL;
        } else if (strcmp(argument, "--report") == 0) {
            options.report_path = option_value(argc, argv, &index, argument);
            parse_ok = options.report_path != NULL;
        } else if (strcmp(argument, "--log") == 0) {
            options.log_path = option_value(argc, argv, &index, argument);
            parse_ok = options.log_path != NULL;
        } else if (strcmp(argument, "--baseline-pixels") == 0) {
            options.baseline_pixels_path = option_value(
                argc, argv, &index, argument);
            parse_ok = options.baseline_pixels_path != NULL;
        } else if (strcmp(argument, "--candidate-pixels") == 0) {
            options.candidate_pixels_path = option_value(
                argc, argv, &index, argument);
            parse_ok = options.candidate_pixels_path != NULL;
        } else if (strcmp(argument, "--temp-root") == 0) {
            options.temporary_root = option_value(
                argc, argv, &index, argument);
            parse_ok = options.temporary_root != NULL;
        } else if (strcmp(argument, "--keep") == 0) {
            value = option_value(argc, argv, &index, argument);
            if (!value) {
                parse_ok = false;
            } else if (strcmp(value, "on-failure") == 0) {
                options.keep_policy = UNITY_FINITE_VISUAL_KEEP_ON_FAILURE;
            } else if (strcmp(value, "always") == 0) {
                options.keep_policy = UNITY_FINITE_VISUAL_KEEP_ALWAYS;
            } else if (strcmp(value, "never") == 0) {
                options.keep_policy = UNITY_FINITE_VISUAL_KEEP_NEVER;
            } else {
                fprintf(stderr, "invalid --keep value: %s\n", value);
                parse_ok = false;
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            parse_ok = false;
        }
        if (!parse_ok) break;
    }

    char* packaged_bridge = NULL;
    if (parse_ok && !options.bridge_path) {
        packaged_bridge = common_executable_resource_find(
            argv[0], UNITY_FINITE_VISUAL_BRIDGE_RESOURCE);
        options.bridge_path = packaged_bridge;
        if (!packaged_bridge) {
            fprintf(stderr,
                    "packaged finite-visual bridge was not found; "
                    "use --bridge FILE\n");
            parse_ok = false;
        }
    }
    if (!parse_ok || !backend_set ||
        !unity_finite_visual_gate_options_validate(&options)) {
        print_usage(stderr, argv[0]);
        free(packaged_bridge);
        return 1;
    }

    UnityFiniteVisualGateRunResult result;
    const UnityFiniteVisualGateStatus status =
        unity_finite_visual_gate_run(&options, &result);
    const bool semantic_result =
        status == UNITY_FINITE_VISUAL_GATE_OK ||
        status == UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH ||
        status == UNITY_FINITE_VISUAL_GATE_DIAGNOSTICS_FOUND ||
        status == UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC ||
        status == UNITY_FINITE_VISUAL_GATE_UNSUPPORTED_FIXTURE;
    FILE* stream = semantic_result ? stdout : stderr;
    fprintf(stream, "status=%s unity_exit=%d",
            unity_finite_visual_gate_status_name(status),
            result.unity_exit_code);
    if (semantic_result) {
        fprintf(stream,
                " size=%ux%u pass=%u bytes=%llu diagnostics=%llu "
                "errors=%llu warnings=%llu failures=%llu",
                result.summary.width, result.summary.height,
                result.summary.pass_index,
                (unsigned long long)result.summary.pixel_byte_count,
                (unsigned long long)result.summary.diagnostic_count,
                (unsigned long long)result.summary.error_count,
                (unsigned long long)result.summary.warning_count,
                (unsigned long long)result.summary.failure_count);
    }
    fputc('\n', stream);
    if (result.workspace_preserved) {
        fprintf(stderr, "isolated project preserved: %s\n",
                result.workspace_path);
    }
    free(packaged_bridge);
    if (status == UNITY_FINITE_VISUAL_GATE_OK) return 0;
    return semantic_result ? 2 : 1;
}

COMMON_DEFINE_UTF8_MAIN(unity_finite_visual_gate_main)
