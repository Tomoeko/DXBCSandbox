// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shader_bundle_gate.h"
#include "common/executable_resource.h"
#include "common/windows_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNITY_BUNDLE_GATE_BRIDGE_RESOURCE \
    "dxbc-sandbox/unity/Editor/DXBCShaderBundleGate.cs"

static void print_usage(FILE* stream, const char* executable) {
    fprintf(stream,
        "Usage: %s --unity PATH --unity-version VERSION --target TARGET \\\n"
        "          --backend BACKEND --bundle PATH --report PATH --log PATH \\\n"
        "          [OPTIONS] INPUT...\n\n"
        "Imports selected .shader candidates and builds one deterministic,\n"
        "write-once AssetBundle in a new isolated Unity project. INPUT may be\n"
        "a shader file or directory. No caller-owned Unity project is opened.\n\n"
        "Required:\n"
        "  --unity PATH             Exact Unity Editor executable\n"
        "  --unity-version VERSION  Required Application.unityVersion\n"
        "  --target TARGET          macos, windows64, or linux64\n"
        "  --backend BACKEND        metal, d3d11, openglcore, or vulkan\n"
        "  --bundle PATH            New output bundle; never overwritten\n"
        "  --report PATH            Stable JSON manifest/diagnostic report\n"
        "  --log PATH               Complete Unity Editor log\n\n"
        "Options:\n"
        "  --bridge PATH            Editor bridge source (default: packaged)\n"
        "  --warnings fail|allow    Default fail; allow is explicit opt-out\n"
        "  --keep on-failure|always|never\n"
        "                           Isolated project retention policy\n"
        "  --temp-root PATH         Existing parent for isolated projects\n"
        "  -h, --help               Show this help\n\n"
        "Defined target/backend pairs:\n"
        "  macos: metal, openglcore\n"
        "  windows64: d3d11, vulkan, openglcore\n"
        "  linux64: vulkan, openglcore\n\n"
        "A defined pair can still be unavailable when its Unity support module\n"
        "or graphics backend is not installed. Those outcomes are reported\n"
        "distinctly and no bundle is published. This command only creates the\n"
        "re-import artifact; exact Shader-object and pixel certification are\n"
        "separate gates.\n",
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

static bool parse_target(const char* value, UnityShaderBundleTarget* target) {
    if (strcmp(value, "macos") == 0) {
        *target = UNITY_SHADER_BUNDLE_TARGET_MACOS;
        return true;
    }
    if (strcmp(value, "windows64") == 0) {
        *target = UNITY_SHADER_BUNDLE_TARGET_WINDOWS64;
        return true;
    }
    if (strcmp(value, "linux64") == 0) {
        *target = UNITY_SHADER_BUNDLE_TARGET_LINUX64;
        return true;
    }
    return false;
}

static bool parse_backend(const char* value,
                          UnityShaderBundleBackend* backend) {
    if (strcmp(value, "metal") == 0) {
        *backend = UNITY_SHADER_BUNDLE_BACKEND_METAL;
        return true;
    }
    if (strcmp(value, "d3d11") == 0) {
        *backend = UNITY_SHADER_BUNDLE_BACKEND_D3D11;
        return true;
    }
    if (strcmp(value, "openglcore") == 0) {
        *backend = UNITY_SHADER_BUNDLE_BACKEND_OPENGLCORE;
        return true;
    }
    if (strcmp(value, "vulkan") == 0) {
        *backend = UNITY_SHADER_BUNDLE_BACKEND_VULKAN;
        return true;
    }
    return false;
}

static int unity_shader_bundle_gate_main(int argc, char** argv) {
    UnityShaderBundleGateOptions options;
    unity_shader_bundle_gate_options_init(&options);
    const char** inputs = (const char**)calloc(
        argc > 0 ? (size_t)argc : 1U, sizeof(*inputs));
    if (!inputs) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    bool parse_ok = true;
    bool target_seen = false;
    bool backend_seen = false;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (strcmp(argument, "-h") == 0 ||
            strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            free(inputs);
            return 0;
        }
        if (strcmp(argument, "--unity") == 0) {
            options.unity_executable = option_value(argc, argv, &index,
                                                    argument);
            parse_ok = options.unity_executable != NULL;
        } else if (strcmp(argument, "--unity-version") == 0) {
            options.expected_unity_version = option_value(argc, argv, &index,
                                                          argument);
            parse_ok = options.expected_unity_version != NULL;
        } else if (strcmp(argument, "--bridge") == 0) {
            options.bridge_path = option_value(argc, argv, &index, argument);
            parse_ok = options.bridge_path != NULL;
        } else if (strcmp(argument, "--report") == 0) {
            options.report_path = option_value(argc, argv, &index, argument);
            parse_ok = options.report_path != NULL;
        } else if (strcmp(argument, "--log") == 0) {
            options.log_path = option_value(argc, argv, &index, argument);
            parse_ok = options.log_path != NULL;
        } else if (strcmp(argument, "--bundle") == 0) {
            options.output_bundle_path = option_value(argc, argv, &index,
                                                      argument);
            parse_ok = options.output_bundle_path != NULL;
        } else if (strcmp(argument, "--temp-root") == 0) {
            options.temporary_root = option_value(argc, argv, &index, argument);
            parse_ok = options.temporary_root != NULL;
        } else if (strcmp(argument, "--target") == 0) {
            const char* value = option_value(argc, argv, &index, argument);
            target_seen = value != NULL && parse_target(value, &options.target);
            parse_ok = target_seen;
            if (value && !target_seen)
                fprintf(stderr, "invalid --target value: %s\n", value);
        } else if (strcmp(argument, "--backend") == 0) {
            const char* value = option_value(argc, argv, &index, argument);
            backend_seen = value != NULL && parse_backend(value,
                                                          &options.backend);
            parse_ok = backend_seen;
            if (value && !backend_seen)
                fprintf(stderr, "invalid --backend value: %s\n", value);
        } else if (strcmp(argument, "--warnings") == 0) {
            const char* value = option_value(argc, argv, &index, argument);
            if (!value) {
                parse_ok = false;
            } else if (strcmp(value, "allow") == 0) {
                options.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_ALLOW;
            } else if (strcmp(value, "fail") == 0) {
                options.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
            } else {
                fprintf(stderr, "invalid --warnings value: %s\n", value);
                parse_ok = false;
            }
        } else if (strcmp(argument, "--keep") == 0) {
            const char* value = option_value(argc, argv, &index, argument);
            if (!value) {
                parse_ok = false;
            } else if (strcmp(value, "on-failure") == 0) {
                options.keep_policy = UNITY_SHADER_IMPORT_KEEP_ON_FAILURE;
            } else if (strcmp(value, "always") == 0) {
                options.keep_policy = UNITY_SHADER_IMPORT_KEEP_ALWAYS;
            } else if (strcmp(value, "never") == 0) {
                options.keep_policy = UNITY_SHADER_IMPORT_KEEP_NEVER;
            } else {
                fprintf(stderr, "invalid --keep value: %s\n", value);
                parse_ok = false;
            }
        } else if (argument[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argument);
            parse_ok = false;
        } else {
            inputs[options.input_count++] = argument;
        }
        if (!parse_ok) break;
    }
    options.inputs = inputs;
    char* packaged_bridge = NULL;
    if (parse_ok && !options.bridge_path) {
        packaged_bridge = common_executable_resource_find(
            argv[0], UNITY_BUNDLE_GATE_BRIDGE_RESOURCE);
        options.bridge_path = packaged_bridge;
        if (!packaged_bridge) {
            fprintf(stderr,
                    "packaged ShaderImporter bundle bridge was not found; "
                    "use --bridge PATH\n");
            parse_ok = false;
        }
    }
    if (!target_seen || !backend_seen || !parse_ok ||
        !unity_shader_bundle_gate_options_validate(&options)) {
        print_usage(stderr, argv[0]);
        free(packaged_bridge);
        free(inputs);
        return 1;
    }

    UnityShaderBundleGateRunResult result;
    UnityShaderBundleGateStatus status = unity_shader_bundle_gate_run(
        &options, &result);
    FILE* stream = status == UNITY_SHADER_BUNDLE_GATE_OK ? stdout : stderr;
    fprintf(stream,
        "status=%s candidates=%llu messages=%llu errors=%llu warnings=%llu "
        "bundle_published=%s unity_exit=%d\n",
        unity_shader_bundle_gate_status_name(status),
        (unsigned long long)result.summary.candidate_count,
        (unsigned long long)result.summary.message_count,
        (unsigned long long)result.summary.error_count,
        (unsigned long long)result.summary.warning_count,
        result.bundle_published ? "true" : "false",
        result.unity_exit_code);
    if (result.workspace_preserved) {
        fprintf(stderr, "isolated project preserved: %s\n",
                result.workspace_path);
    }
    free(packaged_bridge);
    free(inputs);
    if (status == UNITY_SHADER_BUNDLE_GATE_OK) return 0;
    if (status == UNITY_SHADER_BUNDLE_GATE_DIAGNOSTICS_FOUND) return 2;
    if (status == UNITY_SHADER_BUNDLE_GATE_TARGET_UNAVAILABLE) return 4;
    if (status == UNITY_SHADER_BUNDLE_GATE_BACKEND_UNAVAILABLE) return 5;
    if (status == UNITY_SHADER_BUNDLE_GATE_BUILD_FAILED) return 6;
    return 1;
}

COMMON_DEFINE_UTF8_MAIN(unity_shader_bundle_gate_main)
