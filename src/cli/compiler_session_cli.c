// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "compiler/unity_compiler_client.h"
#include "compiler/unity_compiler_session_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DXBC_COMPILER_SESSION_DEFAULT_PROJECT_ROOT
#define DXBC_COMPILER_SESSION_DEFAULT_PROJECT_ROOT "."
#endif

#ifndef DXBC_COMPILER_SESSION_DEFAULT_INCLUDES
#define DXBC_COMPILER_SESSION_DEFAULT_INCLUDES \
    ""
#endif

typedef enum {
    OUTPUT_TABLE = 0,
    OUTPUT_JSON,
} OutputFormat;

typedef struct {
    const char* project_root;
    const char* includes_dir;
    const char* report_path;
    OutputFormat format;
} Options;

static void print_usage(FILE* output, const char* program) {
    fprintf(
        output,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Capture the pinned Unity 2021.3 UnityShaderCompiler session without\n"
        "compiling a shader. Exactly one compiler process is initialized and\n"
        "then shut down cleanly. The result contains the raw platform mask,\n"
        "derived valid_apis, all 25 feature/version records, resolved paths,\n"
        "and exact compiler/environment fingerprints.\n"
        "\n"
        "Options:\n"
        "  --project-root DIR  project/environment authority root\n"
        "                      (default: %s)\n"
        "  --includes DIR      explicit shader include authority\n"
        "                      (default: %s)\n"
        "  --format table|json stdout format (default: table)\n"
        "  --report FILE       publish deterministic JSON without overwriting\n"
        "  -h, --help          show this help\n"
        "\n"
        "Unity path resolution uses DXBC_UNITY_CONTENTS_PATH, DXBC_UNITY_APP,\n"
        "UNITY_EDITOR_PATH, then /Applications/Unity/Unity.app/Contents.\n",
        program, DXBC_COMPILER_SESSION_DEFAULT_PROJECT_ROOT,
        DXBC_COMPILER_SESSION_DEFAULT_INCLUDES);
}

static bool parse_options(
    int argc, char** argv, Options* options, bool* out_help) {
    if (!argv || !options || !out_help) return false;
    *options = (Options){
        .project_root = DXBC_COMPILER_SESSION_DEFAULT_PROJECT_ROOT,
        .includes_dir = DXBC_COMPILER_SESSION_DEFAULT_INCLUDES,
        .format = OUTPUT_TABLE,
    };
    *out_help = false;
    unsigned present = 0U;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (strcmp(argument, "--help") == 0 ||
            strcmp(argument, "-h") == 0) {
            *out_help = true;
            continue;
        }
        unsigned bit = 0U;
        if (strcmp(argument, "--project-root") == 0) {
            bit = 1U << 0U;
        } else if (strcmp(argument, "--includes") == 0) {
            bit = 1U << 1U;
        } else if (strcmp(argument, "--format") == 0) {
            bit = 1U << 2U;
        } else if (strcmp(argument, "--report") == 0) {
            bit = 1U << 3U;
        } else {
            return false;
        }
        if ((present & bit) != 0U || ++index >= argc ||
            argv[index][0] == '\0') {
            return false;
        }
        present |= bit;
        const char* value = argv[index];
        if (bit == (1U << 0U)) {
            options->project_root = value;
        } else if (bit == (1U << 1U)) {
            options->includes_dir = value;
        } else if (bit == (1U << 2U)) {
            if (strcmp(value, "table") == 0) {
                options->format = OUTPUT_TABLE;
            } else if (strcmp(value, "json") == 0) {
                options->format = OUTPUT_JSON;
            } else {
                return false;
            }
        } else {
            options->report_path = value;
        }
    }
    return true;
}

static bool publish_report(const char* path, const char* report) {
    const size_t size = strlen(report);
    CommonFileStatus status = common_file_write_new_atomic(
        path, report, size);
    if (status == COMMON_FILE_OK) return true;
    if (status != COMMON_FILE_ALREADY_EXISTS) return false;
    CommonFileBytes existing = {0};
    status = common_file_read_regular(path, SIZE_MAX, &existing);
    const bool identical = status == COMMON_FILE_OK &&
        existing.size == size &&
        (size == 0U || memcmp(existing.data, report, size) == 0);
    common_file_bytes_dispose(&existing);
    return identical;
}

int main(int argc, char** argv) {
    Options options;
    bool help = false;
    if (!parse_options(argc, argv, &options, &help)) {
        print_usage(stderr, argv && argv[0] ? argv[0] :
                    "dxbc-compiler-session");
        return 2;
    }
    if (help) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    UnityCompilerChannel channel = {.socket_fd = -1, .process_id = 0};
    if (!unity_compiler_start_lazy(
            &channel, options.project_root, options.includes_dir)) {
        fputs("Could not configure the pinned UnityShaderCompiler.\n", stderr);
        unity_compiler_shutdown(&channel);
        return 1;
    }

    UnityCompilerSessionReport report = {
        .project_root = options.project_root,
        .includes_dir = options.includes_dir,
    };
    if (!unity_compiler_get_toolchain_provenance(
            &channel, &report.toolchain)) {
        fputs("Could not pin the UnityShaderCompiler toolchain snapshot.\n",
              stderr);
        unity_compiler_shutdown(&channel);
        return 1;
    }
    if (!unity_compiler_capture_session_capabilities(
            &channel, &report.session)) {
        fputs("Could not capture initializeCompiler session authority. "
              "Cache-only mode cannot perform this live operation.\n",
              stderr);
        unity_compiler_shutdown(&channel);
        return 1;
    }

    char* json = unity_compiler_session_report_format_json(&report);
    char* display = options.format == OUTPUT_JSON
        ? unity_compiler_session_report_format_json(&report)
        : unity_compiler_session_report_format_table(&report);
    unity_compiler_shutdown(&channel);
    if (!json || !display) {
        free(display);
        free(json);
        fputs("Could not render the compiler session report.\n", stderr);
        return 1;
    }

    if (options.report_path &&
        !publish_report(options.report_path, json)) {
        fprintf(stderr,
                "Could not publish '%s' without overwriting different data.\n",
                options.report_path);
        free(display);
        free(json);
        return 1;
    }
    const size_t display_size = strlen(display);
    const bool output_ok = display_size == 0U ||
        fwrite(display, 1U, display_size, stdout) == display_size;
    free(display);
    free(json);
    if (!output_ok) {
        fputs("Could not write compiler session output.\n", stderr);
        return 1;
    }
    return 0;
}
