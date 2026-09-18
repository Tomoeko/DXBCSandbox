// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compiler_session_report.h"

#include "common/string_builder.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_REPORT_SCHEMA "dxbc-unity-compiler-session-report-v1"

static bool string_is_present(const char* value) {
    return value && value[0] != '\0';
}

static bool fingerprint_has_value(
    const uint8_t fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!fingerprint) return false;
    uint8_t combined = 0U;
    for (size_t index = 0U;
         index < UNITY_COMPILER_FINGERPRINT_SIZE; ++index) {
        combined |= fingerprint[index];
    }
    return combined != 0U;
}

bool unity_compiler_session_report_validate(
    const UnityCompilerSessionReport* report) {
    return report && string_is_present(report->project_root) &&
        report->includes_dir != NULL &&
        string_is_present(report->toolchain.unity_contents_path) &&
        string_is_present(report->toolchain.compiler_path) &&
        string_is_present(report->toolchain.builtin_includes_dir) &&
        string_is_present(report->toolchain.playback_engines_dir) &&
        string_is_present(report->toolchain.glslang_path) &&
        string_is_present(report->toolchain.dxcompiler_path) &&
        fingerprint_has_value(report->toolchain.compiler_fingerprint) &&
        fingerprint_has_value(report->toolchain.environment_fingerprint) &&
        unity_compiler_session_capabilities_validate(&report->session);
}

static void append_json_string(StringBuilder* output, const char* value) {
    static const char hex[] = "0123456789abcdef";
    sb_append_char(output, '"');
    for (const unsigned char* cursor = (const unsigned char*)value;
         cursor && *cursor != 0U; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') {
            sb_append_char(output, '\\');
            sb_append_char(output, (char)*cursor);
        } else if (*cursor < 0x20U) {
            sb_append(output, "\\u00");
            sb_append_char(output, hex[*cursor >> 4U]);
            sb_append_char(output, hex[*cursor & 0x0fU]);
        } else {
            sb_append_char(output, (char)*cursor);
        }
    }
    sb_append_char(output, '"');
}

static void append_fingerprint(
    StringBuilder* output,
    const uint8_t fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U;
         index < UNITY_COMPILER_FINGERPRINT_SIZE; ++index) {
        sb_append_char(output, hex[fingerprint[index] >> 4U]);
        sb_append_char(output, hex[fingerprint[index] & 0x0fU]);
    }
}

static void append_json_path_field(
    StringBuilder* output, const char* name, const char* value,
    bool prepend_comma) {
    if (prepend_comma) sb_append_char(output, ',');
    append_json_string(output, name);
    sb_append_char(output, ':');
    append_json_string(output, value);
}

char* unity_compiler_session_report_format_json(
    const UnityCompilerSessionReport* report) {
    if (!unity_compiler_session_report_validate(report)) return NULL;
    char* session_json = unity_compiler_session_capabilities_format_json(
        &report->session);
    if (!session_json) return NULL;

    StringBuilder output;
    sb_init(&output);
    sb_append(&output, "{\"schema\":\"" SESSION_REPORT_SCHEMA "\",");
    sb_append(&output, "\"invocation\":{");
    append_json_path_field(
        &output, "project_root", report->project_root, false);
    append_json_path_field(
        &output, "includes_dir", report->includes_dir, true);
    sb_append(&output, "},\"toolchain\":{");
    append_json_path_field(
        &output, "unity_contents_path",
        report->toolchain.unity_contents_path, false);
    append_json_path_field(
        &output, "compiler_path", report->toolchain.compiler_path, true);
    append_json_path_field(
        &output, "builtin_includes_dir",
        report->toolchain.builtin_includes_dir, true);
    append_json_path_field(
        &output, "playback_engines_dir",
        report->toolchain.playback_engines_dir, true);
    append_json_path_field(
        &output, "glslang_path", report->toolchain.glslang_path, true);
    append_json_path_field(
        &output, "dxcompiler_path", report->toolchain.dxcompiler_path,
        true);
    sb_append(&output, ",\"compiler_fingerprint_sha256\":\"");
    append_fingerprint(
        &output, report->toolchain.compiler_fingerprint);
    sb_append(&output, "\",\"environment_fingerprint_sha256\":\"");
    append_fingerprint(
        &output, report->toolchain.environment_fingerprint);
    sb_append(&output, "\"},\"session\":");
    sb_append(&output, session_json);
    sb_append(&output, "}\n");
    free(session_json);
    if (!sb_ok(&output)) {
        sb_free(&output);
        return NULL;
    }
    char* result = sb_detach(&output);
    sb_free(&output);
    return result;
}

static void append_table_path(
    StringBuilder* output, const char* name, const char* value) {
    sb_appendf(output, "%s: %s\n", name, value);
}

char* unity_compiler_session_report_format_table(
    const UnityCompilerSessionReport* report) {
    uint32_t valid_apis = 0U;
    if (!unity_compiler_session_report_validate(report) ||
        !unity_compiler_session_capabilities_valid_apis(
            &report->session, &valid_apis)) {
        return NULL;
    }

    StringBuilder output;
    sb_init(&output);
    sb_append(&output, "UnityShaderCompiler session\n");
    sb_append(&output, "schema: " SESSION_REPORT_SCHEMA "\n");
    append_table_path(&output, "project_root", report->project_root);
    append_table_path(&output, "includes_dir", report->includes_dir);
    append_table_path(
        &output, "unity_contents_path",
        report->toolchain.unity_contents_path);
    append_table_path(
        &output, "compiler_path", report->toolchain.compiler_path);
    append_table_path(
        &output, "builtin_includes_dir",
        report->toolchain.builtin_includes_dir);
    append_table_path(
        &output, "playback_engines_dir",
        report->toolchain.playback_engines_dir);
    append_table_path(
        &output, "glslang_path", report->toolchain.glslang_path);
    append_table_path(
        &output, "dxcompiler_path", report->toolchain.dxcompiler_path);
    sb_append(&output, "compiler_fingerprint_sha256: ");
    append_fingerprint(&output, report->toolchain.compiler_fingerprint);
    sb_append(&output, "\nenvironment_fingerprint_sha256: ");
    append_fingerprint(&output, report->toolchain.environment_fingerprint);
    sb_appendf(
        &output,
        "\nraw_available_platform_mask: %" PRIu32 " (0x%08" PRIx32 ")\n"
        "valid_apis: %" PRIu32 " (0x%08" PRIx32 ")\n"
        "platform_records:\n"
        "  platform  supported_features      version\n",
        report->session.raw_available_platform_mask,
        report->session.raw_available_platform_mask,
        valid_apis, valid_apis);
    for (size_t index = 0U;
         index < UNITY_COMPILER_PLATFORM_COUNT; ++index) {
        sb_appendf(
            &output, "  %8zu  0x%016" PRIx64 "  %" PRId32 "\n",
            index, report->session.platforms[index].supported_features,
            report->session.platforms[index].version);
    }
    if (!sb_ok(&output)) {
        sb_free(&output);
        return NULL;
    }
    char* result = sb_detach(&output);
    sb_free(&output);
    return result;
}
