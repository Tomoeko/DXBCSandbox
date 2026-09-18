// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "compiler/unity_shader_import_gate.h"

#include "common/file_io.h"
#include "common/path_discovery.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define IMPORT_GATE_SCHEMA "dxbc-sandbox-unity-shader-import/v1"
#define IMPORT_GATE_MAX_REPORT_SIZE (128U * 1024U * 1024U)
#define IMPORT_GATE_JSON_MAX_DEPTH 64U

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t offset;
    unsigned depth;
} JsonCursor;

typedef struct {
    uint64_t messages;
    uint64_t errors;
    uint64_t warnings;
} MessageTotals;

typedef struct {
    char** paths;
    size_t count;
} CandidateList;

static bool checked_increment(uint64_t* value) {
    if (*value == UINT64_MAX) return false;
    ++*value;
    return true;
}

static void json_skip_space(JsonCursor* cursor) {
    while (cursor->offset < cursor->size) {
        uint8_t value = cursor->data[cursor->offset];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            break;
        }
        ++cursor->offset;
    }
}

static bool json_consume(JsonCursor* cursor, uint8_t expected) {
    json_skip_space(cursor);
    if (cursor->offset >= cursor->size ||
        cursor->data[cursor->offset] != expected) {
        return false;
    }
    ++cursor->offset;
    return true;
}

static bool json_hex_digit(uint8_t value) {
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

static bool json_parse_string(JsonCursor* cursor, char* output,
                              size_t output_capacity) {
    json_skip_space(cursor);
    if (cursor->offset >= cursor->size ||
        cursor->data[cursor->offset++] != '"') {
        return false;
    }
    size_t output_size = 0U;
    while (cursor->offset < cursor->size) {
        uint8_t value = cursor->data[cursor->offset++];
        if (value == '"') {
            if (output) {
                if (output_size >= output_capacity) return false;
                output[output_size] = '\0';
            }
            return true;
        }
        if (value < 0x20U) return false;
        if (value == '\\') {
            if (cursor->offset >= cursor->size) return false;
            uint8_t escape = cursor->data[cursor->offset++];
            switch (escape) {
                case '"': value = '"'; break;
                case '\\': value = '\\'; break;
                case '/': value = '/'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'u':
                    if (cursor->size - cursor->offset < 4U) return false;
                    for (size_t index = 0U; index < 4U; ++index) {
                        if (!json_hex_digit(cursor->data[cursor->offset + index])) {
                            return false;
                        }
                    }
                    cursor->offset += 4U;
                    value = 0xffU;
                    break;
                default: return false;
            }
        }
        if (output) {
            if (output_size + 1U >= output_capacity) return false;
            output[output_size++] = (char)value;
        }
    }
    return false;
}

static bool json_parse_uint64(JsonCursor* cursor, uint64_t* output) {
    json_skip_space(cursor);
    if (!output || cursor->offset >= cursor->size ||
        cursor->data[cursor->offset] < '0' ||
        cursor->data[cursor->offset] > '9') {
        return false;
    }
    uint64_t value = 0U;
    size_t start = cursor->offset;
    if (cursor->data[cursor->offset] == '0') {
        ++cursor->offset;
        if (cursor->offset < cursor->size &&
            cursor->data[cursor->offset] >= '0' &&
            cursor->data[cursor->offset] <= '9') {
            return false;
        }
    } else {
        while (cursor->offset < cursor->size &&
               cursor->data[cursor->offset] >= '0' &&
               cursor->data[cursor->offset] <= '9') {
            unsigned digit = (unsigned)(cursor->data[cursor->offset] - '0');
            if (value > (UINT64_MAX - digit) / 10U) return false;
            value = value * 10U + digit;
            ++cursor->offset;
        }
    }
    if (cursor->offset == start) return false;
    *output = value;
    return true;
}

static bool json_parse_bool(JsonCursor* cursor, bool* output) {
    json_skip_space(cursor);
    if (!output) return false;
    if (cursor->size - cursor->offset >= 4U &&
        memcmp(cursor->data + cursor->offset, "true", 4U) == 0) {
        cursor->offset += 4U;
        *output = true;
        return true;
    }
    if (cursor->size - cursor->offset >= 5U &&
        memcmp(cursor->data + cursor->offset, "false", 5U) == 0) {
        cursor->offset += 5U;
        *output = false;
        return true;
    }
    return false;
}

static bool json_skip_value(JsonCursor* cursor);

static bool json_skip_array(JsonCursor* cursor) {
    if (!json_consume(cursor, '[') ||
        cursor->depth >= IMPORT_GATE_JSON_MAX_DEPTH) {
        return false;
    }
    ++cursor->depth;
    json_skip_space(cursor);
    if (json_consume(cursor, ']')) {
        --cursor->depth;
        return true;
    }
    for (;;) {
        if (!json_skip_value(cursor)) return false;
        json_skip_space(cursor);
        if (json_consume(cursor, ']')) {
            --cursor->depth;
            return true;
        }
        if (!json_consume(cursor, ',')) return false;
    }
}

static bool json_skip_object(JsonCursor* cursor) {
    if (!json_consume(cursor, '{') ||
        cursor->depth >= IMPORT_GATE_JSON_MAX_DEPTH) {
        return false;
    }
    ++cursor->depth;
    json_skip_space(cursor);
    if (json_consume(cursor, '}')) {
        --cursor->depth;
        return true;
    }
    for (;;) {
        if (!json_parse_string(cursor, NULL, 0U) ||
            !json_consume(cursor, ':') || !json_skip_value(cursor)) {
            return false;
        }
        json_skip_space(cursor);
        if (json_consume(cursor, '}')) {
            --cursor->depth;
            return true;
        }
        if (!json_consume(cursor, ',')) return false;
    }
}

static bool json_skip_number(JsonCursor* cursor) {
    json_skip_space(cursor);
    size_t start = cursor->offset;
    if (cursor->offset < cursor->size &&
        cursor->data[cursor->offset] == '-') {
        ++cursor->offset;
    }
    if (cursor->offset >= cursor->size) return false;
    if (cursor->data[cursor->offset] == '0') {
        ++cursor->offset;
    } else if (cursor->data[cursor->offset] >= '1' &&
               cursor->data[cursor->offset] <= '9') {
        do {
            ++cursor->offset;
        } while (cursor->offset < cursor->size &&
                 cursor->data[cursor->offset] >= '0' &&
                 cursor->data[cursor->offset] <= '9');
    } else {
        return false;
    }
    if (cursor->offset < cursor->size &&
        cursor->data[cursor->offset] == '.') {
        ++cursor->offset;
        size_t fraction = cursor->offset;
        while (cursor->offset < cursor->size &&
               cursor->data[cursor->offset] >= '0' &&
               cursor->data[cursor->offset] <= '9') {
            ++cursor->offset;
        }
        if (cursor->offset == fraction) return false;
    }
    if (cursor->offset < cursor->size &&
        (cursor->data[cursor->offset] == 'e' ||
         cursor->data[cursor->offset] == 'E')) {
        ++cursor->offset;
        if (cursor->offset < cursor->size &&
            (cursor->data[cursor->offset] == '+' ||
             cursor->data[cursor->offset] == '-')) {
            ++cursor->offset;
        }
        size_t exponent = cursor->offset;
        while (cursor->offset < cursor->size &&
               cursor->data[cursor->offset] >= '0' &&
               cursor->data[cursor->offset] <= '9') {
            ++cursor->offset;
        }
        if (cursor->offset == exponent) return false;
    }
    return cursor->offset > start;
}

static bool json_skip_value(JsonCursor* cursor) {
    json_skip_space(cursor);
    if (cursor->offset >= cursor->size) return false;
    switch (cursor->data[cursor->offset]) {
        case '"': return json_parse_string(cursor, NULL, 0U);
        case '{': return json_skip_object(cursor);
        case '[': return json_skip_array(cursor);
        case 't':
            if (cursor->size - cursor->offset >= 4U &&
                memcmp(cursor->data + cursor->offset, "true", 4U) == 0) {
                cursor->offset += 4U;
                return true;
            }
            return false;
        case 'f':
            if (cursor->size - cursor->offset >= 5U &&
                memcmp(cursor->data + cursor->offset, "false", 5U) == 0) {
                cursor->offset += 5U;
                return true;
            }
            return false;
        case 'n':
            if (cursor->size - cursor->offset >= 4U &&
                memcmp(cursor->data + cursor->offset, "null", 4U) == 0) {
                cursor->offset += 4U;
                return true;
            }
            return false;
        default: return json_skip_number(cursor);
    }
}

static bool parse_message(JsonCursor* cursor, MessageTotals* totals) {
    if (!json_consume(cursor, '{')) return false;
    unsigned seen = 0U;
    unsigned severity = 0U;
    for (;;) {
        json_skip_space(cursor);
        if (json_consume(cursor, '}')) break;
        char key[32];
        if (!json_parse_string(cursor, key, sizeof(key)) ||
            !json_consume(cursor, ':')) {
            return false;
        }
        unsigned bit = 0U;
        if (strcmp(key, "severity") == 0) bit = 1U << 0U;
        else if (strcmp(key, "file") == 0) bit = 1U << 1U;
        else if (strcmp(key, "line") == 0) bit = 1U << 2U;
        else if (strcmp(key, "message") == 0) bit = 1U << 3U;
        else if (strcmp(key, "platform") == 0) bit = 1U << 4U;
        else if (strcmp(key, "details") == 0) bit = 1U << 5U;
        if (bit != 0U && (seen & bit) != 0U) return false;
        seen |= bit;
        if (bit == (1U << 0U)) {
            char value[16];
            if (!json_parse_string(cursor, value, sizeof(value))) return false;
            if (strcmp(value, "error") == 0) severity = 1U;
            else if (strcmp(value, "warning") == 0) severity = 2U;
            else if (strcmp(value, "info") == 0) severity = 3U;
            else return false;
        } else if (bit == (1U << 2U)) {
            uint64_t line = 0U;
            if (!json_parse_uint64(cursor, &line) || line > UINT32_MAX) {
                return false;
            }
        } else if (bit != 0U) {
            if (!json_parse_string(cursor, NULL, 0U)) return false;
        } else if (!json_skip_value(cursor)) {
            return false;
        }
        json_skip_space(cursor);
        if (json_consume(cursor, '}')) break;
        if (!json_consume(cursor, ',')) return false;
    }
    if (seen != 0x3fU || severity == 0U ||
        !checked_increment(&totals->messages)) {
        return false;
    }
    if (severity == 1U && !checked_increment(&totals->errors)) return false;
    if (severity == 2U && !checked_increment(&totals->warnings)) return false;
    return true;
}

static bool parse_messages(JsonCursor* cursor, MessageTotals* totals) {
    if (!json_consume(cursor, '[')) return false;
    json_skip_space(cursor);
    if (json_consume(cursor, ']')) return true;
    for (;;) {
        if (!parse_message(cursor, totals)) return false;
        json_skip_space(cursor);
        if (json_consume(cursor, ']')) return true;
        if (!json_consume(cursor, ',')) return false;
    }
}

static bool json_parse_nonempty_string(JsonCursor* cursor, bool* nonempty_value) {
    if (!nonempty_value) return false;
    json_skip_space(cursor);
    size_t start = cursor->offset;
    if (!json_parse_string(cursor, NULL, 0U)) return false;
    *nonempty_value = cursor->offset >= start + 2U &&
        cursor->offset - start > 2U;
    return true;
}

static bool parse_shader(JsonCursor* cursor, MessageTotals* totals,
                         bool* valid_import) {
    if (!json_consume(cursor, '{')) return false;
    unsigned seen = 0U;
    bool imported = false;
    bool shader_name_nonempty = false;
    for (;;) {
        json_skip_space(cursor);
        if (json_consume(cursor, '}')) break;
        char key[32];
        if (!json_parse_string(cursor, key, sizeof(key)) ||
            !json_consume(cursor, ':')) {
            return false;
        }
        unsigned bit = 0U;
        if (strcmp(key, "source_path") == 0) bit = 1U << 0U;
        else if (strcmp(key, "asset_path") == 0) bit = 1U << 1U;
        else if (strcmp(key, "shader_name") == 0) bit = 1U << 2U;
        else if (strcmp(key, "imported") == 0) bit = 1U << 3U;
        else if (strcmp(key, "messages") == 0) bit = 1U << 4U;
        if (bit != 0U && (seen & bit) != 0U) return false;
        seen |= bit;
        if (bit == (1U << 2U)) {
            if (!json_parse_nonempty_string(cursor, &shader_name_nonempty)) {
                return false;
            }
        } else if ((bit & 0x03U) != 0U) {
            if (!json_parse_string(cursor, NULL, 0U)) return false;
        } else if (bit == (1U << 3U)) {
            if (!json_parse_bool(cursor, &imported)) return false;
        } else if (bit == (1U << 4U)) {
            if (!parse_messages(cursor, totals)) return false;
        } else if (!json_skip_value(cursor)) {
            return false;
        }
        json_skip_space(cursor);
        if (json_consume(cursor, '}')) break;
        if (!json_consume(cursor, ',')) return false;
    }
    if (seen != 0x1fU) return false;
    *valid_import = imported && shader_name_nonempty;
    return true;
}

static bool parse_shaders(JsonCursor* cursor, uint64_t* shader_count,
                          MessageTotals* totals, bool* all_imported) {
    if (!json_consume(cursor, '[')) return false;
    json_skip_space(cursor);
    if (json_consume(cursor, ']')) return true;
    for (;;) {
        bool valid_import = false;
        if (!parse_shader(cursor, totals, &valid_import) ||
            !checked_increment(shader_count)) {
            return false;
        }
        if (!valid_import) *all_imported = false;
        json_skip_space(cursor);
        if (json_consume(cursor, ']')) return true;
        if (!json_consume(cursor, ',')) return false;
    }
}

UnityShaderImportGateStatus unity_shader_import_gate_parse_result_json(
    const uint8_t* json, size_t json_size,
    UnityShaderImportGateSummary* out_summary) {
    if ((!json && json_size != 0U) || !out_summary) {
        return UNITY_SHADER_IMPORT_GATE_INVALID_ARGUMENT;
    }
    JsonCursor cursor = {json, json_size, 0U, 0U};
    if (!json_consume(&cursor, '{')) {
        return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
    }
    enum {
        TOP_SCHEMA = 1U << 0U,
        TOP_VERSION = 1U << 1U,
        TOP_POLICY = 1U << 2U,
        TOP_CANDIDATES = 1U << 3U,
        TOP_MESSAGES = 1U << 4U,
        TOP_ERRORS = 1U << 5U,
        TOP_WARNINGS = 1U << 6U,
        TOP_STATUS = 1U << 7U,
        TOP_SHADERS = 1U << 8U,
        TOP_ALL = (1U << 9U) - 1U,
    };
    unsigned seen = 0U;
    bool status_passed = false;
    UnityShaderImportGateSummary declared;
    memset(&declared, 0, sizeof(declared));
    uint64_t actual_candidates = 0U;
    MessageTotals actual = {0U, 0U, 0U};
    bool all_imported = true;
    for (;;) {
        json_skip_space(&cursor);
        if (json_consume(&cursor, '}')) break;
        char key[40];
        if (!json_parse_string(&cursor, key, sizeof(key)) ||
            !json_consume(&cursor, ':')) {
            return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
        }
        unsigned bit = 0U;
        if (strcmp(key, "schema") == 0) bit = TOP_SCHEMA;
        else if (strcmp(key, "unity_version") == 0) bit = TOP_VERSION;
        else if (strcmp(key, "warning_policy") == 0) bit = TOP_POLICY;
        else if (strcmp(key, "candidate_count") == 0) bit = TOP_CANDIDATES;
        else if (strcmp(key, "message_count") == 0) bit = TOP_MESSAGES;
        else if (strcmp(key, "error_count") == 0) bit = TOP_ERRORS;
        else if (strcmp(key, "warning_count") == 0) bit = TOP_WARNINGS;
        else if (strcmp(key, "status") == 0) bit = TOP_STATUS;
        else if (strcmp(key, "shaders") == 0) bit = TOP_SHADERS;
        if (bit != 0U && (seen & bit) != 0U) {
            return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
        }
        seen |= bit;
        if (bit == TOP_SCHEMA) {
            char value[64];
            if (!json_parse_string(&cursor, value, sizeof(value)) ||
                strcmp(value, IMPORT_GATE_SCHEMA) != 0) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_VERSION) {
            char value[64];
            if (!json_parse_string(&cursor, value, sizeof(value)) ||
                value[0] == '\0') {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_POLICY) {
            char value[16];
            if (!json_parse_string(&cursor, value, sizeof(value))) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
            if (strcmp(value, "allow") == 0) {
                declared.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_ALLOW;
            } else if (strcmp(value, "fail") == 0) {
                declared.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
            } else {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_CANDIDATES) {
            if (!json_parse_uint64(&cursor, &declared.candidate_count)) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_MESSAGES) {
            if (!json_parse_uint64(&cursor, &declared.message_count)) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_ERRORS) {
            if (!json_parse_uint64(&cursor, &declared.error_count)) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_WARNINGS) {
            if (!json_parse_uint64(&cursor, &declared.warning_count)) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_STATUS) {
            char value[16];
            if (!json_parse_string(&cursor, value, sizeof(value))) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
            if (strcmp(value, "passed") == 0) status_passed = true;
            else if (strcmp(value, "failed") != 0) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_SHADERS) {
            if (!parse_shaders(&cursor, &actual_candidates, &actual,
                               &all_imported)) {
                return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
            }
        } else if (!json_skip_value(&cursor)) {
            return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
        }
        json_skip_space(&cursor);
        if (json_consume(&cursor, '}')) break;
        if (!json_consume(&cursor, ',')) {
            return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
        }
    }
    json_skip_space(&cursor);
    bool calculated_pass = actual.errors == 0U &&
        (declared.warning_policy == UNITY_SHADER_IMPORT_WARNINGS_ALLOW ||
         actual.warnings == 0U);
    if (seen != TOP_ALL || cursor.offset != cursor.size ||
        actual_candidates == 0U || (status_passed && !all_imported) ||
        declared.candidate_count != actual_candidates ||
        declared.message_count != actual.messages ||
        declared.error_count != actual.errors ||
        declared.warning_count != actual.warnings ||
        status_passed != calculated_pass) {
        return UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
    }
    declared.passed = calculated_pass;
    *out_summary = declared;
    return UNITY_SHADER_IMPORT_GATE_OK;
}

void unity_shader_import_gate_options_init(
    UnityShaderImportGateOptions* options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
    options->keep_policy = UNITY_SHADER_IMPORT_KEEP_ON_FAILURE;
}

static bool nonempty(const char* value) {
    return value && value[0] != '\0';
}

bool unity_shader_import_gate_options_validate(
    const UnityShaderImportGateOptions* options) {
    if (!options || !nonempty(options->unity_executable) ||
        !nonempty(options->expected_unity_version) ||
        !nonempty(options->bridge_path) || !nonempty(options->report_path) ||
        !nonempty(options->log_path) || !options->inputs ||
        options->input_count == 0U ||
        options->warning_policy > UNITY_SHADER_IMPORT_WARNINGS_FAIL ||
        options->keep_policy > UNITY_SHADER_IMPORT_KEEP_NEVER ||
        strcmp(options->report_path, options->log_path) == 0) {
        return false;
    }
    for (size_t index = 0U; index < options->input_count; ++index) {
        if (!nonempty(options->inputs[index])) return false;
    }
    return true;
}

const char* unity_shader_import_gate_status_name(
    UnityShaderImportGateStatus status) {
    switch (status) {
        case UNITY_SHADER_IMPORT_GATE_OK: return "ok";
        case UNITY_SHADER_IMPORT_GATE_DIAGNOSTICS_FOUND:
            return "diagnostics-found";
        case UNITY_SHADER_IMPORT_GATE_INVALID_ARGUMENT:
            return "invalid-argument";
        case UNITY_SHADER_IMPORT_GATE_DISCOVERY_FAILED:
            return "discovery-failed";
        case UNITY_SHADER_IMPORT_GATE_NO_CANDIDATES:
            return "no-candidates";
        case UNITY_SHADER_IMPORT_GATE_ALLOCATION_FAILED:
            return "allocation-failed";
        case UNITY_SHADER_IMPORT_GATE_IO_FAILED: return "io-failed";
        case UNITY_SHADER_IMPORT_GATE_PROCESS_FAILED:
            return "process-failed";
        case UNITY_SHADER_IMPORT_GATE_RESULT_INVALID:
            return "result-invalid";
        case UNITY_SHADER_IMPORT_GATE_PATH_TOO_LONG:
            return "path-too-long";
        default: return "unknown";
    }
}

#ifndef _WIN32
static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    if (length == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(length + 1U);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1U);
    return copy;
}
#endif

static bool is_separator(char value) {
#ifdef _WIN32
    return value == '/' || value == '\\';
#else
    return value == '/';
#endif
}

static char* join_path(const char* left, const char* right) {
    if (!left || !right) return NULL;
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool separator = left_length != 0U &&
        !is_separator(left[left_length - 1U]);
    if (left_length > SIZE_MAX - right_length - (separator ? 2U : 1U)) {
        return NULL;
    }
    size_t total = left_length + right_length + (separator ? 1U : 0U);
    char* path = (char*)malloc(total + 1U);
    if (!path) return NULL;
    memcpy(path, left, left_length);
    if (separator) path[left_length++] = '/';
    memcpy(path + left_length, right, right_length + 1U);
    return path;
}

static char* absolute_path(const char* path) {
    if (!path) return NULL;
#ifdef _WIN32
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return NULL;
    DWORD needed = GetFullPathNameW(wide, 0U, NULL, NULL);
    if (needed == 0U) {
        free(wide);
        return NULL;
    }
    wchar_t* full = (wchar_t*)malloc((size_t)needed * sizeof(*full));
    if (!full) {
        free(wide);
        return NULL;
    }
    DWORD written = GetFullPathNameW(wide, needed, full, NULL);
    free(wide);
    if (written == 0U || written >= needed) {
        free(full);
        return NULL;
    }
    char* utf8 = common_windows_wide_to_utf8(full);
    free(full);
    return utf8;
#else
    if (path[0] == '/') return duplicate_string(path);
    char* current = getcwd(NULL, 0U);
    if (!current) return NULL;
    char* joined = join_path(current, path);
    free(current);
    return joined;
#endif
}

static bool has_shader_extension(const char* path) {
    size_t length = strlen(path);
    static const char suffix[] = ".shader";
    if (length < sizeof(suffix) - 1U) return false;
    const char* tail = path + length - (sizeof(suffix) - 1U);
    for (size_t index = 0U; index < sizeof(suffix) - 1U; ++index) {
        unsigned char value = (unsigned char)tail[index];
        if ((char)tolower(value) != suffix[index]) return false;
    }
    return true;
}

static void candidate_list_dispose(CandidateList* list) {
    if (!list) return;
    for (size_t index = 0U; index < list->count; ++index) {
        free(list->paths[index]);
    }
    free(list->paths);
    memset(list, 0, sizeof(*list));
}

static UnityShaderImportGateStatus discover_candidates(
    const UnityShaderImportGateOptions* options, CandidateList* candidates) {
    CommonPathDiscoveryOptions discovery_options;
    common_path_discovery_options_default(&discovery_options);
    CommonPathDiscoveryResult discovered;
    common_path_discovery_result_init(&discovered);
    CommonPathDiscoveryStatus discovery = common_path_discover(
        options->inputs, options->input_count, &discovery_options, &discovered);
    if (discovery != COMMON_PATH_DISCOVERY_OK) {
        common_path_discovery_result_dispose(&discovered);
        return discovery == COMMON_PATH_DISCOVERY_ALLOCATION_FAILED
            ? UNITY_SHADER_IMPORT_GATE_ALLOCATION_FAILED
            : UNITY_SHADER_IMPORT_GATE_DISCOVERY_FAILED;
    }
    size_t count = 0U;
    for (size_t index = 0U; index < discovered.count; ++index) {
        if (has_shader_extension(discovered.paths[index].path)) ++count;
    }
    if (count == 0U) {
        common_path_discovery_result_dispose(&discovered);
        return UNITY_SHADER_IMPORT_GATE_NO_CANDIDATES;
    }
    char** paths = (char**)calloc(count, sizeof(*paths));
    if (!paths) {
        common_path_discovery_result_dispose(&discovered);
        return UNITY_SHADER_IMPORT_GATE_ALLOCATION_FAILED;
    }
    size_t next = 0U;
    for (size_t index = 0U; index < discovered.count; ++index) {
        if (!has_shader_extension(discovered.paths[index].path)) continue;
        paths[next] = absolute_path(discovered.paths[index].path);
        if (!paths[next]) {
            CandidateList partial = {paths, next};
            candidate_list_dispose(&partial);
            common_path_discovery_result_dispose(&discovered);
            return UNITY_SHADER_IMPORT_GATE_ALLOCATION_FAILED;
        }
        ++next;
    }
    common_path_discovery_result_dispose(&discovered);
    candidates->paths = paths;
    candidates->count = count;
    return UNITY_SHADER_IMPORT_GATE_OK;
}

static bool make_directory(const char* path) {
#ifdef _WIN32
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return false;
    int result = _wmkdir(wide);
    free(wide);
    return result == 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static bool write_bytes(const char* path, const void* data, size_t size) {
    FILE* file = fopen(path, "wb");
    if (!file) return false;
    bool success = size == 0U || fwrite(data, 1U, size, file) == size;
    if (success && fflush(file) != 0) success = false;
    if (fclose(file) != 0) success = false;
    return success;
}

static bool copy_file(const char* source, const char* destination) {
    CommonFileBytes contents;
    CommonFileStatus status = common_file_read_regular(
        source, SIZE_MAX, &contents);
    if (status != COMMON_FILE_OK) return false;
    bool success = write_bytes(destination, contents.data, contents.size);
    common_file_bytes_dispose(&contents);
    return success;
}

static char* make_temporary_workspace(const char* requested_root) {
    const char* root = requested_root;
#ifdef _WIN32
    char default_root[MAX_PATH + 1U];
    if (!nonempty(root)) {
        DWORD length = GetTempPathA((DWORD)sizeof(default_root), default_root);
        if (length == 0U || length >= sizeof(default_root)) return NULL;
        root = default_root;
    }
    char* pattern = join_path(root, "dxbc-shader-import.tmp");
    if (!pattern) return NULL;
    char* unique = (char*)malloc(MAX_PATH + 1U);
    if (!unique) {
        free(pattern);
        return NULL;
    }
    if (GetTempFileNameA(root, "dsi", 0U, unique) == 0U) {
        free(unique);
        free(pattern);
        return NULL;
    }
    free(pattern);
    if (!DeleteFileA(unique) || !CreateDirectoryA(unique, NULL)) {
        free(unique);
        return NULL;
    }
    return unique;
#else
    if (!nonempty(root)) {
        root = getenv("TMPDIR");
        if (!nonempty(root)) root = "/tmp";
    }
    char* pattern = join_path(root, "dxbc-shader-import.XXXXXX");
    if (!pattern) return NULL;
    int descriptor = mkstemp(pattern);
    if (descriptor < 0) {
        free(pattern);
        return NULL;
    }
    bool created = close(descriptor) == 0 && unlink(pattern) == 0 &&
        make_directory(pattern);
    if (!created) {
        (void)unlink(pattern);
        free(pattern);
        return NULL;
    }
    return pattern;
#endif
}

static char* base64_encode(const uint8_t* data, size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (size > (SIZE_MAX - 1U) / 4U * 3U) return NULL;
    size_t output_size = ((size + 2U) / 3U) * 4U;
    char* output = (char*)malloc(output_size + 1U);
    if (!output) return NULL;
    size_t input = 0U;
    size_t next = 0U;
    while (input < size) {
        uint32_t value = (uint32_t)data[input++] << 16U;
        bool second = input < size;
        if (second) value |= (uint32_t)data[input++] << 8U;
        bool third = input < size;
        if (third) value |= data[input++];
        output[next++] = alphabet[(value >> 18U) & 63U];
        output[next++] = alphabet[(value >> 12U) & 63U];
        output[next++] = second ? alphabet[(value >> 6U) & 63U] : '=';
        output[next++] = third ? alphabet[value & 63U] : '=';
    }
    output[next] = '\0';
    return output;
}

static bool write_manifest(const char* path, const CandidateList* candidates) {
    FILE* file = fopen(path, "wb");
    if (!file) return false;
    bool success = true;
    for (size_t index = 0U; index < candidates->count; ++index) {
        char asset[64];
        int length = snprintf(asset, sizeof(asset),
                              "Assets/DXBCImportGate/%08zu.shader", index);
        char* encoded = base64_encode(
            (const uint8_t*)candidates->paths[index],
            strlen(candidates->paths[index]));
        if (length < 0 || (size_t)length >= sizeof(asset) || !encoded ||
            fprintf(file, "%s\t%s\n", asset, encoded) < 0) {
            success = false;
        }
        free(encoded);
        if (!success) break;
    }
    if (fflush(file) != 0 || fclose(file) != 0) success = false;
    return success;
}

static bool prepare_project(const UnityShaderImportGateOptions* options,
                            const CandidateList* candidates,
                            const char* workspace, char** out_manifest,
                            char** out_raw_result) {
    char* assets = join_path(workspace, "Assets");
    char* editor = assets ? join_path(assets, "Editor") : NULL;
    char* candidates_dir = assets ? join_path(assets, "DXBCImportGate") : NULL;
    char* settings = join_path(workspace, "ProjectSettings");
    char* packages = join_path(workspace, "Packages");
    bool success = assets && editor && candidates_dir && settings && packages &&
        make_directory(assets) && make_directory(editor) &&
        make_directory(candidates_dir) && make_directory(settings) &&
        make_directory(packages);
    char* bridge = editor ? join_path(editor, "DXBCShaderImportGate.cs") : NULL;
    char* project_version = settings
        ? join_path(settings, "ProjectVersion.txt") : NULL;
    char* package_manifest = packages ? join_path(packages, "manifest.json") : NULL;
    char* manifest = join_path(workspace, "candidate-manifest.tsv");
    char* raw_result = join_path(workspace, "shader-import-result.json");
    if (!bridge || !project_version || !package_manifest || !manifest ||
        !raw_result) {
        success = false;
    }
    if (success) success = copy_file(options->bridge_path, bridge);
    if (success) {
        size_t version_length = strlen(options->expected_unity_version);
        static const char prefix[] = "m_EditorVersion: ";
        char* text = (char*)malloc(sizeof(prefix) + version_length + 1U);
        if (!text) {
            success = false;
        } else {
            int length = snprintf(text, sizeof(prefix) + version_length + 1U,
                                  "%s%s\n", prefix,
                                  options->expected_unity_version);
            success = length > 0 && write_bytes(
                project_version, text, (size_t)length);
            free(text);
        }
    }
    static const char packages_json[] = "{\n  \"dependencies\": {}\n}\n";
    if (success) {
        success = write_bytes(package_manifest, packages_json,
                              sizeof(packages_json) - 1U);
    }
    for (size_t index = 0U; success && index < candidates->count; ++index) {
        char name[32];
        int length = snprintf(name, sizeof(name), "%08zu.shader", index);
        char* destination = length > 0 && (size_t)length < sizeof(name)
            ? join_path(candidates_dir, name) : NULL;
        success = destination && copy_file(candidates->paths[index], destination);
        free(destination);
    }
    if (success) success = write_manifest(manifest, candidates);
    free(assets);
    free(editor);
    free(candidates_dir);
    free(settings);
    free(packages);
    free(bridge);
    free(project_version);
    free(package_manifest);
    if (!success) {
        free(manifest);
        free(raw_result);
        return false;
    }
    *out_manifest = manifest;
    *out_raw_result = raw_result;
    return true;
}

#ifdef _WIN32

static bool append_windows_argument(char** command, size_t* size,
                                    size_t* capacity, const char* argument) {
    size_t needed = 3U;
    for (const char* at = argument; *at; ++at) {
        needed += *at == '"' || *at == '\\' ? 2U : 1U;
    }
    if (*size > SIZE_MAX - needed) return false;
    size_t target = *size + needed;
    if (target > *capacity) {
        size_t grown = *capacity == 0U ? 256U : *capacity;
        while (grown < target) {
            if (grown > SIZE_MAX / 2U) {
                grown = target;
                break;
            }
            grown *= 2U;
        }
        char* allocation = (char*)realloc(*command, grown);
        if (!allocation) return false;
        *command = allocation;
        *capacity = grown;
    }
    if (*size != 0U) (*command)[(*size)++] = ' ';
    (*command)[(*size)++] = '"';
    size_t slash_count = 0U;
    for (const char* at = argument;; ++at) {
        if (*at == '\\') {
            ++slash_count;
            continue;
        }
        if (*at == '"') {
            while (slash_count-- != 0U) (*command)[(*size)++] = '\\';
            (*command)[(*size)++] = '\\';
            (*command)[(*size)++] = '"';
            slash_count = 0U;
            continue;
        }
        if (*at == '\0') {
            while (slash_count != 0U) {
                (*command)[(*size)++] = '\\';
                (*command)[(*size)++] = '\\';
                --slash_count;
            }
            break;
        }
        while (slash_count-- != 0U) (*command)[(*size)++] = '\\';
        slash_count = 0U;
        (*command)[(*size)++] = *at;
    }
    (*command)[(*size)++] = '"';
    (*command)[*size] = '\0';
    return true;
}

static bool launch_unity(const char* const* arguments, int* exit_code) {
    char* command = NULL;
    size_t size = 0U;
    size_t capacity = 0U;
    for (size_t index = 0U; arguments[index]; ++index) {
        if (!append_windows_argument(&command, &size, &capacity,
                                     arguments[index])) {
            free(command);
            return false;
        }
    }
    wchar_t* application = common_windows_utf8_to_wide(arguments[0]);
    wchar_t* wide_command = common_windows_utf8_to_wide(command);
    free(command);
    if (!application || !wide_command) {
        free(application);
        free(wide_command);
        return false;
    }
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    BOOL created = CreateProcessW(application, wide_command, NULL, NULL,
                                  FALSE, 0U, NULL, NULL, &startup, &process);
    free(application);
    free(wide_command);
    if (!created) return false;
    bool success = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0;
    DWORD code = 0U;
    if (success) success = GetExitCodeProcess(process.hProcess, &code) != 0;
    (void)CloseHandle(process.hThread);
    (void)CloseHandle(process.hProcess);
    if (!success || code > INT_MAX) return false;
    *exit_code = (int)code;
    return true;
}

#else

static bool launch_unity(const char* const* arguments, int* exit_code) {
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (strchr(arguments[0], '/')) {
            execv(arguments[0], (char* const*)arguments);
        } else {
            execvp(arguments[0], (char* const*)arguments);
        }
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
    } else {
        return false;
    }
    return true;
}

#endif

#ifdef _WIN32
static bool remove_tree_wide(const wchar_t* path) {
    size_t length = wcslen(path);
    wchar_t* pattern = (wchar_t*)malloc((length + 3U) * sizeof(*pattern));
    if (!pattern) return false;
    memcpy(pattern, path, length * sizeof(*pattern));
    pattern[length] = L'\\';
    pattern[length + 1U] = L'*';
    pattern[length + 2U] = L'\0';
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    free(pattern);
    bool success = true;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            size_t name_length = wcslen(data.cFileName);
            wchar_t* child = (wchar_t*)malloc(
                (length + name_length + 2U) * sizeof(*child));
            if (!child) {
                success = false;
                break;
            }
            memcpy(child, path, length * sizeof(*child));
            child[length] = L'\\';
            memcpy(child + length + 1U, data.cFileName,
                   (name_length + 1U) * sizeof(*child));
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U) {
                if (!remove_tree_wide(child)) success = false;
            } else if (!DeleteFileW(child)) {
                success = false;
            }
            free(child);
        } while (success && FindNextFileW(search, &data));
        (void)FindClose(search);
    }
    return success && RemoveDirectoryW(path) != 0;
}

static bool remove_tree(const char* path) {
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return false;
    bool success = remove_tree_wide(wide);
    free(wide);
    return success;
}
#else
static bool remove_tree(const char* path) {
    struct stat information;
    if (lstat(path, &information) != 0) return false;
    if (!S_ISDIR(information.st_mode)) return unlink(path) == 0;
    DIR* directory = opendir(path);
    if (!directory) return false;
    bool success = true;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char* child = join_path(path, entry->d_name);
        if (!child || !remove_tree(child)) success = false;
        free(child);
        if (!success) break;
    }
    if (closedir(directory) != 0) success = false;
    return success && rmdir(path) == 0;
}
#endif

UnityShaderImportGateStatus unity_shader_import_gate_run(
    const UnityShaderImportGateOptions* options,
    UnityShaderImportGateRunResult* out_result) {
    if (!out_result) return UNITY_SHADER_IMPORT_GATE_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    out_result->unity_exit_code = -1;
    if (!unity_shader_import_gate_options_validate(options)) {
        return UNITY_SHADER_IMPORT_GATE_INVALID_ARGUMENT;
    }
    CandidateList candidates = {NULL, 0U};
    UnityShaderImportGateStatus status = discover_candidates(options, &candidates);
    if (status != UNITY_SHADER_IMPORT_GATE_OK) return status;

    char* workspace = make_temporary_workspace(options->temporary_root);
    char* manifest = NULL;
    char* raw_result = NULL;
    char* report_path = absolute_path(options->report_path);
    char* log_path = absolute_path(options->log_path);
    if (!workspace || !report_path || !log_path) {
        status = UNITY_SHADER_IMPORT_GATE_ALLOCATION_FAILED;
        goto done;
    }
    size_t workspace_length = strlen(workspace);
    if (workspace_length >= sizeof(out_result->workspace_path)) {
        status = UNITY_SHADER_IMPORT_GATE_PATH_TOO_LONG;
        goto done;
    }
    memcpy(out_result->workspace_path, workspace, workspace_length + 1U);
    if (!prepare_project(options, &candidates, workspace, &manifest,
                         &raw_result) || !write_bytes(log_path, NULL, 0U)) {
        status = UNITY_SHADER_IMPORT_GATE_IO_FAILED;
        goto done;
    }
    const char* policy = options->warning_policy ==
        UNITY_SHADER_IMPORT_WARNINGS_FAIL ? "fail" : "allow";
    const char* arguments[] = {
        options->unity_executable,
        "-batchmode",
        "-nographics",
        /* The isolated project has no package dependencies.  Keep the
         * verifier's resource envelope deterministic: UPM is unnecessary,
         * the JobQueue is bounded, and Unity's documented diagnostic switch
         * constrains ShaderCompiler to one process.  The latter changes only
         * process multiplicity/timeout; compiler request authority continues
         * to come from the pinned Editor and the imported Shader asset. */
        "-noUpm",
        "-job-worker-count", "1",
        "-diag-debug-shader-compiler",
        "-disableManagedDebugger",
        "-projectPath", workspace,
        "-logFile", log_path,
        "-executeMethod", "DXBCSandbox.Editor.DXBCShaderImportGate.Run",
        "-dxbc-import-manifest", manifest,
        "-dxbc-import-result", raw_result,
        "-dxbc-expected-unity-version", options->expected_unity_version,
        "-dxbc-warning-policy", policy,
        NULL,
    };
    if (!launch_unity(arguments, &out_result->unity_exit_code)) {
        status = UNITY_SHADER_IMPORT_GATE_PROCESS_FAILED;
        goto done;
    }
    CommonFileBytes result_bytes;
    CommonFileStatus read_status = common_file_read_regular(
        raw_result, IMPORT_GATE_MAX_REPORT_SIZE, &result_bytes);
    if (read_status != COMMON_FILE_OK) {
        status = UNITY_SHADER_IMPORT_GATE_PROCESS_FAILED;
        goto done;
    }
    bool published = write_bytes(report_path, result_bytes.data,
                                 result_bytes.size);
    UnityShaderImportGateSummary summary;
    UnityShaderImportGateStatus parse_status =
        unity_shader_import_gate_parse_result_json(
            result_bytes.data, result_bytes.size, &summary);
    common_file_bytes_dispose(&result_bytes);
    if (!published) {
        status = UNITY_SHADER_IMPORT_GATE_IO_FAILED;
        goto done;
    }
    if (parse_status != UNITY_SHADER_IMPORT_GATE_OK ||
        summary.candidate_count != (uint64_t)candidates.count ||
        summary.warning_policy != options->warning_policy) {
        status = UNITY_SHADER_IMPORT_GATE_RESULT_INVALID;
        goto done;
    }
    out_result->summary = summary;
    if (summary.passed && out_result->unity_exit_code == 0) {
        status = UNITY_SHADER_IMPORT_GATE_OK;
    } else if (!summary.passed && out_result->unity_exit_code == 2) {
        status = UNITY_SHADER_IMPORT_GATE_DIAGNOSTICS_FOUND;
    } else {
        status = UNITY_SHADER_IMPORT_GATE_PROCESS_FAILED;
    }

done:
    if (workspace) {
        bool preserve = options->keep_policy == UNITY_SHADER_IMPORT_KEEP_ALWAYS ||
            (options->keep_policy == UNITY_SHADER_IMPORT_KEEP_ON_FAILURE &&
             status != UNITY_SHADER_IMPORT_GATE_OK);
        if (!preserve && !remove_tree(workspace)) {
            if (status == UNITY_SHADER_IMPORT_GATE_OK) {
                status = UNITY_SHADER_IMPORT_GATE_IO_FAILED;
            }
            preserve = true;
        }
        out_result->workspace_preserved = preserve;
    }
    free(workspace);
    free(manifest);
    free(raw_result);
    free(report_path);
    free(log_path);
    candidate_list_dispose(&candidates);
    return status;
}
