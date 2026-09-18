// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shader_import_gate.h"
#include "common/executable_resource.h"
#include "common/windows_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNITY_IMPORT_GATE_BRIDGE_RESOURCE \
    "dxbc-sandbox/unity/Editor/DXBCShaderImportGate.cs"

static void print_usage(FILE* stream, const char* executable) {
    fprintf(stream,
        "Usage: %s --unity PATH --unity-version VERSION --report PATH \\\n"
        "          --log PATH [OPTIONS] INPUT...\n\n"
        "Recursively validates generated .shader candidates through Unity's\n"
        "ShaderImporter in a new isolated temporary project. INPUT may be a\n"
        "shader file or directory. Discovery is deterministic and never opens\n"
        "a caller-owned Unity project.\n\n"
        "Required:\n"
        "  --unity PATH             Exact Unity Editor executable\n"
        "  --unity-version VERSION  Required Application.unityVersion\n"
        "  --report PATH            Stable JSON diagnostic report output\n"
        "  --log PATH               Complete Unity Editor log output\n\n"
        "Options:\n"
        "  --bridge PATH            Editor bridge source (default: packaged)\n"
        "  --warnings fail|allow    Warning policy (default: fail; allow is\n"
        "                           an explicit diagnostic opt-out)\n"
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

static int unity_shader_import_gate_main(int argc, char** argv) {
    UnityShaderImportGateOptions options;
    unity_shader_import_gate_options_init(&options);
    const char** inputs = (const char**)calloc(
        argc > 0 ? (size_t)argc : 1U, sizeof(*inputs));
    if (!inputs) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }
    bool parse_ok = true;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (strcmp(argument, "-h") == 0 ||
            strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            free(inputs);
            return 0;
        }
        if (strcmp(argument, "--unity") == 0) {
            options.unity_executable = option_value(
                argc, argv, &index, argument);
            parse_ok = options.unity_executable != NULL;
        } else if (strcmp(argument, "--unity-version") == 0) {
            options.expected_unity_version = option_value(
                argc, argv, &index, argument);
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
        } else if (strcmp(argument, "--temp-root") == 0) {
            options.temporary_root = option_value(
                argc, argv, &index, argument);
            parse_ok = options.temporary_root != NULL;
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
            argv[0], UNITY_IMPORT_GATE_BRIDGE_RESOURCE);
        options.bridge_path = packaged_bridge;
        if (!packaged_bridge) {
            fprintf(stderr,
                    "packaged ShaderImporter bridge was not found; "
                    "use --bridge PATH\n");
            parse_ok = false;
        }
    }
    if (!parse_ok || !unity_shader_import_gate_options_validate(&options)) {
        print_usage(stderr, argv[0]);
        free(packaged_bridge);
        free(inputs);
        return 1;
    }
    UnityShaderImportGateRunResult result;
    UnityShaderImportGateStatus status = unity_shader_import_gate_run(
        &options, &result);
    if (status == UNITY_SHADER_IMPORT_GATE_OK ||
        status == UNITY_SHADER_IMPORT_GATE_DIAGNOSTICS_FOUND) {
        fprintf(stdout,
            "status=%s candidates=%llu messages=%llu errors=%llu "
            "warnings=%llu unity_exit=%d\n",
            unity_shader_import_gate_status_name(status),
            (unsigned long long)result.summary.candidate_count,
            (unsigned long long)result.summary.message_count,
            (unsigned long long)result.summary.error_count,
            (unsigned long long)result.summary.warning_count,
            result.unity_exit_code);
    } else {
        fprintf(stderr, "Unity ShaderImporter gate failed: %s",
                unity_shader_import_gate_status_name(status));
        if (result.unity_exit_code >= 0) {
            fprintf(stderr, " (Unity exit %d)", result.unity_exit_code);
        }
        fputc('\n', stderr);
    }
    if (result.workspace_preserved) {
        fprintf(stderr, "isolated project preserved: %s\n",
                result.workspace_path);
    }
    free(packaged_bridge);
    free(inputs);
    if (status == UNITY_SHADER_IMPORT_GATE_OK) return 0;
    if (status == UNITY_SHADER_IMPORT_GATE_DIAGNOSTICS_FOUND) return 2;
    return 1;
}

COMMON_DEFINE_UTF8_MAIN(unity_shader_import_gate_main)
