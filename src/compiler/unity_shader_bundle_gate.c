// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "compiler/unity_shader_bundle_gate.h"

#include "common/file_io.h"
#include "common/path_discovery.h"
#include "common/sha256.h"

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

#define BUNDLE_GATE_SCHEMA "dxbc-sandbox-unity-shader-bundle/v1"
#define BUNDLE_GATE_NAME "dxbc-release-shaders.bundle"
#define BUNDLE_GATE_MAX_REPORT_SIZE (256U * 1024U * 1024U)
#define BUNDLE_GATE_JSON_MAX_DEPTH 64U

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

static bool nonempty(const char* value) {
    return value && value[0] != '\0';
}

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

static int json_hex_value(uint8_t value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool append_utf8(char* output, size_t capacity, size_t* size,
                        uint32_t codepoint) {
    uint8_t bytes[4];
    size_t count = 0U;
    if (codepoint <= 0x7fU) {
        bytes[count++] = (uint8_t)codepoint;
    } else if (codepoint <= 0x7ffU) {
        bytes[count++] = (uint8_t)(0xc0U | (codepoint >> 6U));
        bytes[count++] = (uint8_t)(0x80U | (codepoint & 0x3fU));
    } else if (codepoint <= 0xffffU) {
        if (codepoint >= 0xd800U && codepoint <= 0xdfffU) return false;
        bytes[count++] = (uint8_t)(0xe0U | (codepoint >> 12U));
        bytes[count++] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3fU));
        bytes[count++] = (uint8_t)(0x80U | (codepoint & 0x3fU));
    } else if (codepoint <= 0x10ffffU) {
        bytes[count++] = (uint8_t)(0xf0U | (codepoint >> 18U));
        bytes[count++] = (uint8_t)(0x80U | ((codepoint >> 12U) & 0x3fU));
        bytes[count++] = (uint8_t)(0x80U | ((codepoint >> 6U) & 0x3fU));
        bytes[count++] = (uint8_t)(0x80U | (codepoint & 0x3fU));
    } else {
        return false;
    }
    if (output) {
        if (*size > capacity || count >= capacity - *size) return false;
        for (size_t index = 0U; index < count; ++index) {
            output[(*size)++] = (char)bytes[index];
        }
    }
    return true;
}

static bool parse_hex4(JsonCursor* cursor, uint32_t* value) {
    if (cursor->size - cursor->offset < 4U) return false;
    uint32_t result = 0U;
    for (size_t index = 0U; index < 4U; ++index) {
        int digit = json_hex_value(cursor->data[cursor->offset++]);
        if (digit < 0) return false;
        result = (result << 4U) | (uint32_t)digit;
    }
    *value = result;
    return true;
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
        if (value != '\\') {
            if (output) {
                if (output_size + 1U >= output_capacity) return false;
                output[output_size++] = (char)value;
            }
            continue;
        }
        if (cursor->offset >= cursor->size) return false;
        uint8_t escape = cursor->data[cursor->offset++];
        uint32_t codepoint = 0U;
        switch (escape) {
            case '"': codepoint = '"'; break;
            case '\\': codepoint = '\\'; break;
            case '/': codepoint = '/'; break;
            case 'b': codepoint = '\b'; break;
            case 'f': codepoint = '\f'; break;
            case 'n': codepoint = '\n'; break;
            case 'r': codepoint = '\r'; break;
            case 't': codepoint = '\t'; break;
            case 'u': {
                if (!parse_hex4(cursor, &codepoint)) return false;
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (cursor->size - cursor->offset < 6U ||
                        cursor->data[cursor->offset] != '\\' ||
                        cursor->data[cursor->offset + 1U] != 'u') {
                        return false;
                    }
                    cursor->offset += 2U;
                    uint32_t low = 0U;
                    if (!parse_hex4(cursor, &low) || low < 0xdc00U ||
                        low > 0xdfffU) {
                        return false;
                    }
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                        (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    return false;
                }
                break;
            }
            default: return false;
        }
        if (!append_utf8(output, output_capacity, &output_size, codepoint)) {
            return false;
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
    if (cursor->data[cursor->offset] == '0') {
        ++cursor->offset;
        if (cursor->offset < cursor->size &&
            isdigit((unsigned char)cursor->data[cursor->offset])) {
            return false;
        }
    } else {
        while (cursor->offset < cursor->size &&
               isdigit((unsigned char)cursor->data[cursor->offset])) {
            unsigned digit = (unsigned)(cursor->data[cursor->offset] - '0');
            if (value > (UINT64_MAX - digit) / 10U) return false;
            value = value * 10U + digit;
            ++cursor->offset;
        }
    }
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
        cursor->depth >= BUNDLE_GATE_JSON_MAX_DEPTH) {
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
        if (json_consume(cursor, ']')) {
            --cursor->depth;
            return true;
        }
        if (!json_consume(cursor, ',')) return false;
    }
}

static bool json_skip_object(JsonCursor* cursor) {
    if (!json_consume(cursor, '{') ||
        cursor->depth >= BUNDLE_GATE_JSON_MAX_DEPTH) {
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
    if (cursor->offset < cursor->size && cursor->data[cursor->offset] == '-') {
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
                 isdigit((unsigned char)cursor->data[cursor->offset]));
    } else {
        return false;
    }
    if (cursor->offset < cursor->size && cursor->data[cursor->offset] == '.') {
        ++cursor->offset;
        size_t fraction = cursor->offset;
        while (cursor->offset < cursor->size &&
               isdigit((unsigned char)cursor->data[cursor->offset])) {
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
               isdigit((unsigned char)cursor->data[cursor->offset])) {
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

static bool parse_diagnostic(JsonCursor* cursor, MessageTotals* totals) {
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

static bool parse_diagnostics(JsonCursor* cursor, MessageTotals* totals) {
    if (!json_consume(cursor, '[')) return false;
    json_skip_space(cursor);
    if (json_consume(cursor, ']')) return true;
    for (;;) {
        if (!parse_diagnostic(cursor, totals)) return false;
        if (json_consume(cursor, ']')) return true;
        if (!json_consume(cursor, ',')) return false;
    }
}

static bool json_parse_nonempty_string(JsonCursor* cursor, bool* nonempty_value) {
    if (!nonempty_value) return false;
    json_skip_space(cursor);
    size_t start = cursor->offset;
    if (!json_parse_string(cursor, NULL, 0U)) return false;
    *nonempty_value = cursor->offset > start + 2U;
    return true;
}

static void mapping_update_u64(CommonSha256Context* context, uint64_t value) {
    uint8_t bytes[8];
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    common_sha256_update(context, bytes, sizeof(bytes));
}

static void mapping_update_string(CommonSha256Context* context,
                                  const char* value) {
    size_t length = strlen(value);
    mapping_update_u64(context, (uint64_t)length);
    common_sha256_update(context, value, length);
}

static bool parse_shader(JsonCursor* cursor, MessageTotals* totals,
                         uint64_t shader_index,
                         CommonSha256Context* mapping,
                         bool* valid_import) {
    if (!json_consume(cursor, '{')) return false;
    unsigned seen = 0U;
    bool imported = false;
    bool shader_name_nonempty = false;
    bool source_nonempty = false;
    bool asset_nonempty = false;
    bool bundle_asset_nonempty = false;
    char source_path[UNITY_SHADER_BUNDLE_PATH_CAPACITY];
    char asset_path[96];
    char bundle_asset_name[96];
    source_path[0] = '\0';
    asset_path[0] = '\0';
    bundle_asset_name[0] = '\0';
    for (;;) {
        json_skip_space(cursor);
        if (json_consume(cursor, '}')) break;
        char key[40];
        if (!json_parse_string(cursor, key, sizeof(key)) ||
            !json_consume(cursor, ':')) {
            return false;
        }
        unsigned bit = 0U;
        if (strcmp(key, "source_path") == 0) bit = 1U << 0U;
        else if (strcmp(key, "asset_path") == 0) bit = 1U << 1U;
        else if (strcmp(key, "bundle_asset_name") == 0) bit = 1U << 2U;
        else if (strcmp(key, "shader_name") == 0) bit = 1U << 3U;
        else if (strcmp(key, "imported") == 0) bit = 1U << 4U;
        else if (strcmp(key, "messages") == 0) bit = 1U << 5U;
        if (bit != 0U && (seen & bit) != 0U) return false;
        seen |= bit;
        if (bit == (1U << 0U)) {
            if (!json_parse_string(cursor, source_path, sizeof(source_path))) {
                return false;
            }
            source_nonempty = source_path[0] != '\0';
        } else if (bit == (1U << 1U)) {
            if (!json_parse_string(cursor, asset_path, sizeof(asset_path))) {
                return false;
            }
            asset_nonempty = asset_path[0] != '\0';
        } else if (bit == (1U << 2U)) {
            if (!json_parse_string(cursor, bundle_asset_name,
                                   sizeof(bundle_asset_name))) {
                return false;
            }
            bundle_asset_nonempty = bundle_asset_name[0] != '\0';
        } else if (bit == (1U << 3U)) {
            if (!json_parse_nonempty_string(cursor, &shader_name_nonempty)) {
                return false;
            }
        } else if (bit == (1U << 4U)) {
            if (!json_parse_bool(cursor, &imported)) return false;
        } else if (bit == (1U << 5U)) {
            if (!parse_diagnostics(cursor, totals)) return false;
        } else if (!json_skip_value(cursor)) {
            return false;
        }
        if (json_consume(cursor, '}')) break;
        if (!json_consume(cursor, ',')) return false;
    }
    if (seen != 0x3fU || !source_nonempty || !asset_nonempty ||
        !bundle_asset_nonempty) {
        return false;
    }
    char expected_asset[80];
    int expected_size = snprintf(expected_asset, sizeof(expected_asset),
        "Assets/DXBCReleaseBundle/%08llu.shader",
        (unsigned long long)shader_index);
    if (expected_size < 0 || (size_t)expected_size >= sizeof(expected_asset) ||
        strcmp(asset_path, expected_asset) != 0 ||
        strcmp(bundle_asset_name, expected_asset) != 0) {
        return false;
    }
    mapping_update_u64(mapping, shader_index);
    mapping_update_string(mapping, source_path);
    mapping_update_string(mapping, asset_path);
    mapping_update_string(mapping, bundle_asset_name);
    *valid_import = imported && shader_name_nonempty;
    return true;
}

static bool parse_shaders(JsonCursor* cursor, uint64_t* count,
                          MessageTotals* totals,
                          CommonSha256Context* mapping,
                          bool* all_imported) {
    if (!json_consume(cursor, '[')) return false;
    json_skip_space(cursor);
    if (json_consume(cursor, ']')) return true;
    for (;;) {
        bool valid_import = false;
        if (!parse_shader(cursor, totals, *count, mapping, &valid_import) ||
            !checked_increment(count)) {
            return false;
        }
        if (!valid_import) *all_imported = false;
        if (json_consume(cursor, ']')) return true;
        if (!json_consume(cursor, ',')) return false;
    }
}

static bool parse_failure(JsonCursor* cursor) {
    if (!json_consume(cursor, '{')) return false;
    unsigned seen = 0U;
    bool kind_nonempty = false;
    for (;;) {
        json_skip_space(cursor);
        if (json_consume(cursor, '}')) break;
        char key[24];
        if (!json_parse_string(cursor, key, sizeof(key)) ||
            !json_consume(cursor, ':')) {
            return false;
        }
        unsigned bit = 0U;
        if (strcmp(key, "kind") == 0) bit = 1U << 0U;
        else if (strcmp(key, "message") == 0) bit = 1U << 1U;
        if (bit != 0U && (seen & bit) != 0U) return false;
        seen |= bit;
        if (bit == (1U << 0U)) {
            if (!json_parse_nonempty_string(cursor, &kind_nonempty)) return false;
        } else if (bit == (1U << 1U)) {
            if (!json_parse_string(cursor, NULL, 0U)) return false;
        } else if (!json_skip_value(cursor)) {
            return false;
        }
        if (json_consume(cursor, '}')) break;
        if (!json_consume(cursor, ',')) return false;
    }
    return seen == 0x03U && kind_nonempty;
}

static bool parse_failures(JsonCursor* cursor, uint64_t* count) {
    if (!json_consume(cursor, '[')) return false;
    json_skip_space(cursor);
    if (json_consume(cursor, ']')) return true;
    for (;;) {
        if (!parse_failure(cursor) || !checked_increment(count)) return false;
        if (json_consume(cursor, ']')) return true;
        if (!json_consume(cursor, ',')) return false;
    }
}

static bool parse_target(const char* value, UnityShaderBundleTarget* target) {
    if (strcmp(value, "StandaloneOSX") == 0) {
        *target = UNITY_SHADER_BUNDLE_TARGET_MACOS;
        return true;
    }
    if (strcmp(value, "StandaloneWindows64") == 0) {
        *target = UNITY_SHADER_BUNDLE_TARGET_WINDOWS64;
        return true;
    }
    if (strcmp(value, "StandaloneLinux64") == 0) {
        *target = UNITY_SHADER_BUNDLE_TARGET_LINUX64;
        return true;
    }
    return false;
}

static bool parse_backend(const char* value, UnityShaderBundleBackend* backend) {
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

static bool parse_status(const char* value,
                         UnityShaderBundleGateStatus* status) {
    if (strcmp(value, "passed") == 0) {
        *status = UNITY_SHADER_BUNDLE_GATE_OK;
        return true;
    }
    if (strcmp(value, "diagnostics_found") == 0) {
        *status = UNITY_SHADER_BUNDLE_GATE_DIAGNOSTICS_FOUND;
        return true;
    }
    if (strcmp(value, "target_unavailable") == 0) {
        *status = UNITY_SHADER_BUNDLE_GATE_TARGET_UNAVAILABLE;
        return true;
    }
    if (strcmp(value, "backend_unavailable") == 0) {
        *status = UNITY_SHADER_BUNDLE_GATE_BACKEND_UNAVAILABLE;
        return true;
    }
    if (strcmp(value, "build_failed") == 0) {
        *status = UNITY_SHADER_BUNDLE_GATE_BUILD_FAILED;
        return true;
    }
    return false;
}

UnityShaderBundleGateStatus unity_shader_bundle_gate_parse_result_json(
    const uint8_t* json, size_t json_size,
    UnityShaderBundleGateSummary* out_summary) {
    if ((!json && json_size != 0U) || !out_summary) {
        return UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT;
    }
    JsonCursor cursor = {json, json_size, 0U, 0U};
    if (!json_consume(&cursor, '{')) {
        return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
    }
    enum {
        TOP_SCHEMA = 1U << 0U,
        TOP_VERSION = 1U << 1U,
        TOP_TARGET = 1U << 2U,
        TOP_BACKEND = 1U << 3U,
        TOP_POLICY = 1U << 4U,
        TOP_CANDIDATES = 1U << 5U,
        TOP_MESSAGES = 1U << 6U,
        TOP_ERRORS = 1U << 7U,
        TOP_WARNINGS = 1U << 8U,
        TOP_FAILURE_COUNT = 1U << 9U,
        TOP_STATUS = 1U << 10U,
        TOP_OUTPUT = 1U << 11U,
        TOP_BUNDLE_BUILT = 1U << 12U,
        TOP_BUNDLE_SIZE = 1U << 13U,
        TOP_BUNDLE_HASH = 1U << 14U,
        TOP_SHADERS = 1U << 15U,
        TOP_BUILD_MESSAGES = 1U << 16U,
        TOP_FAILURES = 1U << 17U,
        TOP_ALL = (1U << 18U) - 1U,
    };
    unsigned seen = 0U;
    bool schema_valid = false;
    bool output_nonempty = false;
    bool hash_nonempty = false;
    bool all_imported = true;
    uint64_t actual_candidates = 0U;
    uint64_t actual_failures = 0U;
    uint64_t declared_failures = 0U;
    MessageTotals actual = {0U, 0U, 0U};
    UnityShaderBundleGateSummary declared;
    memset(&declared, 0, sizeof(declared));
    CommonSha256Context mapping;
    static const char mapping_domain[] =
        "DXBCSandbox/reimport-candidate-mapping/v1";
    common_sha256_init(&mapping);
    common_sha256_update(&mapping, mapping_domain,
                         sizeof(mapping_domain) - 1U);
    for (;;) {
        json_skip_space(&cursor);
        if (json_consume(&cursor, '}')) break;
        char key[48];
        if (!json_parse_string(&cursor, key, sizeof(key)) ||
            !json_consume(&cursor, ':')) {
            return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        }
        unsigned bit = 0U;
        if (strcmp(key, "schema") == 0) bit = TOP_SCHEMA;
        else if (strcmp(key, "unity_version") == 0) bit = TOP_VERSION;
        else if (strcmp(key, "build_target") == 0) bit = TOP_TARGET;
        else if (strcmp(key, "graphics_backend") == 0) bit = TOP_BACKEND;
        else if (strcmp(key, "warning_policy") == 0) bit = TOP_POLICY;
        else if (strcmp(key, "candidate_count") == 0) bit = TOP_CANDIDATES;
        else if (strcmp(key, "message_count") == 0) bit = TOP_MESSAGES;
        else if (strcmp(key, "error_count") == 0) bit = TOP_ERRORS;
        else if (strcmp(key, "warning_count") == 0) bit = TOP_WARNINGS;
        else if (strcmp(key, "failure_count") == 0) bit = TOP_FAILURE_COUNT;
        else if (strcmp(key, "status") == 0) bit = TOP_STATUS;
        else if (strcmp(key, "output_bundle_path") == 0) bit = TOP_OUTPUT;
        else if (strcmp(key, "bundle_built") == 0) bit = TOP_BUNDLE_BUILT;
        else if (strcmp(key, "bundle_size") == 0) bit = TOP_BUNDLE_SIZE;
        else if (strcmp(key, "bundle_sha256") == 0) bit = TOP_BUNDLE_HASH;
        else if (strcmp(key, "shaders") == 0) bit = TOP_SHADERS;
        else if (strcmp(key, "build_messages") == 0) bit = TOP_BUILD_MESSAGES;
        else if (strcmp(key, "failures") == 0) bit = TOP_FAILURES;
        if (bit != 0U && (seen & bit) != 0U) {
            return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        }
        seen |= bit;
        if (bit == TOP_SCHEMA) {
            char value[64];
            if (!json_parse_string(&cursor, value, sizeof(value))) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
            schema_valid = strcmp(value, BUNDLE_GATE_SCHEMA) == 0;
        } else if (bit == TOP_VERSION) {
            if (!json_parse_string(&cursor, declared.unity_version,
                                   sizeof(declared.unity_version)) ||
                declared.unity_version[0] == '\0') {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_TARGET) {
            char value[32];
            if (!json_parse_string(&cursor, value, sizeof(value)) ||
                !parse_target(value, &declared.target)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_BACKEND) {
            char value[24];
            if (!json_parse_string(&cursor, value, sizeof(value)) ||
                !parse_backend(value, &declared.backend)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_POLICY) {
            char value[16];
            if (!json_parse_string(&cursor, value, sizeof(value))) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
            if (strcmp(value, "fail") == 0) {
                declared.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
            } else if (strcmp(value, "allow") == 0) {
                declared.warning_policy = UNITY_SHADER_IMPORT_WARNINGS_ALLOW;
            } else {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_CANDIDATES) {
            if (!json_parse_uint64(&cursor, &declared.candidate_count)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_MESSAGES) {
            if (!json_parse_uint64(&cursor, &declared.message_count)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_ERRORS) {
            if (!json_parse_uint64(&cursor, &declared.error_count)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_WARNINGS) {
            if (!json_parse_uint64(&cursor, &declared.warning_count)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_FAILURE_COUNT) {
            if (!json_parse_uint64(&cursor, &declared_failures)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_STATUS) {
            char value[32];
            if (!json_parse_string(&cursor, value, sizeof(value)) ||
                !parse_status(value, &declared.reported_status)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_OUTPUT) {
            if (!json_parse_string(&cursor, declared.output_bundle_path,
                                   sizeof(declared.output_bundle_path))) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
            output_nonempty = declared.output_bundle_path[0] != '\0';
        } else if (bit == TOP_BUNDLE_BUILT) {
            if (!json_parse_bool(&cursor, &declared.bundle_built)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_BUNDLE_SIZE) {
            if (!json_parse_uint64(&cursor, &declared.bundle_size)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_BUNDLE_HASH) {
            if (!json_parse_string(&cursor, declared.bundle_sha256,
                                   sizeof(declared.bundle_sha256))) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
            hash_nonempty = declared.bundle_sha256[0] != '\0';
        } else if (bit == TOP_SHADERS) {
            if (!parse_shaders(&cursor, &actual_candidates, &actual,
                               &mapping, &all_imported)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_BUILD_MESSAGES) {
            if (!parse_diagnostics(&cursor, &actual)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (bit == TOP_FAILURES) {
            if (!parse_failures(&cursor, &actual_failures)) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (!json_skip_value(&cursor)) {
            return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        }
        if (json_consume(&cursor, '}')) break;
        if (!json_consume(&cursor, ',')) {
            return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        }
    }
    json_skip_space(&cursor);
    if (cursor.offset != cursor.size || seen != TOP_ALL || !schema_valid ||
        declared.candidate_count == 0U ||
        declared.candidate_count != actual_candidates ||
        declared.message_count != actual.messages ||
        declared.error_count != actual.errors ||
        declared.warning_count != actual.warnings ||
        declared_failures != actual_failures ||
        !unity_shader_bundle_target_supports_backend(
            declared.target, declared.backend)) {
        return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
    }
    bool policy_break = declared.error_count != 0U ||
        (declared.warning_policy == UNITY_SHADER_IMPORT_WARNINGS_FAIL &&
         declared.warning_count != 0U);
    declared.all_candidates_imported = all_imported;
    declared.passed = declared.reported_status == UNITY_SHADER_BUNDLE_GATE_OK;
    if (declared.passed) {
        if (policy_break || !all_imported || !declared.bundle_built ||
            declared.bundle_size == 0U || !output_nonempty || !hash_nonempty ||
            strlen(declared.bundle_sha256) != 64U || actual_failures != 0U) {
            return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        }
        for (size_t index = 0U; index < 64U; ++index) {
            char value = declared.bundle_sha256[index];
            if (!((value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f'))) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        }
    } else {
        if (declared.bundle_built || declared.bundle_size != 0U ||
            hash_nonempty) {
            return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        }
        if (declared.reported_status ==
                UNITY_SHADER_BUNDLE_GATE_DIAGNOSTICS_FOUND) {
            if (!policy_break) {
                return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
            }
        } else if (actual_failures == 0U) {
            return UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        }
    }
    uint8_t mapping_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256_final(&mapping, mapping_digest);
    common_sha256_digest_to_hex(mapping_digest,
                                declared.candidate_mapping_sha256);
    *out_summary = declared;
    return UNITY_SHADER_BUNDLE_GATE_OK;
}

void unity_shader_bundle_gate_options_init(
    UnityShaderBundleGateOptions* options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->warning_policy = UNITY_SHADER_IMPORT_WARNINGS_FAIL;
    options->keep_policy = UNITY_SHADER_IMPORT_KEEP_ON_FAILURE;
    options->target = UNITY_SHADER_BUNDLE_TARGET_MACOS;
    options->backend = UNITY_SHADER_BUNDLE_BACKEND_METAL;
}

bool unity_shader_bundle_target_supports_backend(
    UnityShaderBundleTarget target, UnityShaderBundleBackend backend) {
    switch (target) {
        case UNITY_SHADER_BUNDLE_TARGET_MACOS:
            return backend == UNITY_SHADER_BUNDLE_BACKEND_METAL ||
                backend == UNITY_SHADER_BUNDLE_BACKEND_OPENGLCORE;
        case UNITY_SHADER_BUNDLE_TARGET_WINDOWS64:
            return backend == UNITY_SHADER_BUNDLE_BACKEND_D3D11 ||
                backend == UNITY_SHADER_BUNDLE_BACKEND_VULKAN ||
                backend == UNITY_SHADER_BUNDLE_BACKEND_OPENGLCORE;
        case UNITY_SHADER_BUNDLE_TARGET_LINUX64:
            return backend == UNITY_SHADER_BUNDLE_BACKEND_VULKAN ||
                backend == UNITY_SHADER_BUNDLE_BACKEND_OPENGLCORE;
        default: return false;
    }
}

bool unity_shader_bundle_gate_options_validate(
    const UnityShaderBundleGateOptions* options) {
    if (!options || !nonempty(options->unity_executable) ||
        !nonempty(options->expected_unity_version) ||
        !nonempty(options->bridge_path) || !nonempty(options->report_path) ||
        !nonempty(options->log_path) ||
        !nonempty(options->output_bundle_path) || !options->inputs ||
        options->input_count == 0U ||
        options->warning_policy < UNITY_SHADER_IMPORT_WARNINGS_ALLOW ||
        options->warning_policy > UNITY_SHADER_IMPORT_WARNINGS_FAIL ||
        options->keep_policy < UNITY_SHADER_IMPORT_KEEP_ON_FAILURE ||
        options->keep_policy > UNITY_SHADER_IMPORT_KEEP_NEVER ||
        !unity_shader_bundle_target_supports_backend(options->target,
                                                     options->backend)) {
        return false;
    }
    if (strlen(options->expected_unity_version) >=
        UNITY_SHADER_BUNDLE_VERSION_CAPACITY) {
        return false;
    }
    if (strcmp(options->report_path, options->log_path) == 0 ||
        strcmp(options->report_path, options->output_bundle_path) == 0 ||
        strcmp(options->log_path, options->output_bundle_path) == 0) {
        return false;
    }
    for (size_t index = 0U; index < options->input_count; ++index) {
        if (!nonempty(options->inputs[index])) return false;
    }
    return true;
}

const char* unity_shader_bundle_target_name(UnityShaderBundleTarget target) {
    switch (target) {
        case UNITY_SHADER_BUNDLE_TARGET_MACOS: return "StandaloneOSX";
        case UNITY_SHADER_BUNDLE_TARGET_WINDOWS64:
            return "StandaloneWindows64";
        case UNITY_SHADER_BUNDLE_TARGET_LINUX64:
            return "StandaloneLinux64";
        default: return "invalid";
    }
}

const char* unity_shader_bundle_backend_name(UnityShaderBundleBackend backend) {
    switch (backend) {
        case UNITY_SHADER_BUNDLE_BACKEND_METAL: return "metal";
        case UNITY_SHADER_BUNDLE_BACKEND_D3D11: return "d3d11";
        case UNITY_SHADER_BUNDLE_BACKEND_OPENGLCORE: return "openglcore";
        case UNITY_SHADER_BUNDLE_BACKEND_VULKAN: return "vulkan";
        default: return "invalid";
    }
}

const char* unity_shader_bundle_gate_status_name(
    UnityShaderBundleGateStatus status) {
    switch (status) {
        case UNITY_SHADER_BUNDLE_GATE_OK: return "ok";
        case UNITY_SHADER_BUNDLE_GATE_DIAGNOSTICS_FOUND:
            return "diagnostics_found";
        case UNITY_SHADER_BUNDLE_GATE_TARGET_UNAVAILABLE:
            return "target_unavailable";
        case UNITY_SHADER_BUNDLE_GATE_BACKEND_UNAVAILABLE:
            return "backend_unavailable";
        case UNITY_SHADER_BUNDLE_GATE_BUILD_FAILED: return "build_failed";
        case UNITY_SHADER_BUNDLE_GATE_OUTPUT_EXISTS: return "output_exists";
        case UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT:
            return "invalid_argument";
        case UNITY_SHADER_BUNDLE_GATE_DISCOVERY_FAILED:
            return "discovery_failed";
        case UNITY_SHADER_BUNDLE_GATE_NO_CANDIDATES: return "no_candidates";
        case UNITY_SHADER_BUNDLE_GATE_ALLOCATION_FAILED:
            return "allocation_failed";
        case UNITY_SHADER_BUNDLE_GATE_IO_FAILED: return "io_failed";
        case UNITY_SHADER_BUNDLE_GATE_PROCESS_FAILED: return "process_failed";
        case UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID: return "result_invalid";
        case UNITY_SHADER_BUNDLE_GATE_PATH_TOO_LONG: return "path_too_long";
        default: return "unknown";
    }
}

bool unity_shader_bundle_candidate_guid(
    const void* source, size_t source_size, uint64_t candidate_index,
    char guid[33]) {
    if ((!source && source_size != 0U) || !guid) return false;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source, source_size, source_digest);
    static const char domain[] = "DXBCSandbox/reimport-shader-guid/v1";
    uint8_t index_bytes[8];
    for (size_t byte = 0U; byte < sizeof(index_bytes); ++byte) {
        index_bytes[byte] = (uint8_t)(candidate_index >> (byte * 8U));
    }
    CommonSha256Context context;
    uint8_t guid_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256_init(&context);
    common_sha256_update(&context, domain, sizeof(domain) - 1U);
    common_sha256_update(&context, index_bytes, sizeof(index_bytes));
    common_sha256_update(&context, source_digest, sizeof(source_digest));
    common_sha256_final(&context, guid_digest);
    static const char digits[] = "0123456789abcdef";
    for (size_t byte = 0U; byte < 16U; ++byte) {
        guid[byte * 2U] = digits[guid_digest[byte] >> 4U];
        guid[byte * 2U + 1U] = digits[guid_digest[byte] & 0x0fU];
    }
    guid[32] = '\0';
    return true;
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
        if ((char)tolower((unsigned char)tail[index]) != suffix[index]) {
            return false;
        }
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

static UnityShaderBundleGateStatus discover_candidates(
    const UnityShaderBundleGateOptions* options, CandidateList* candidates) {
    CommonPathDiscoveryOptions discovery_options;
    common_path_discovery_options_default(&discovery_options);
    CommonPathDiscoveryResult discovered;
    common_path_discovery_result_init(&discovered);
    CommonPathDiscoveryStatus discovery = common_path_discover(
        options->inputs, options->input_count, &discovery_options, &discovered);
    if (discovery != COMMON_PATH_DISCOVERY_OK) {
        common_path_discovery_result_dispose(&discovered);
        return discovery == COMMON_PATH_DISCOVERY_ALLOCATION_FAILED
            ? UNITY_SHADER_BUNDLE_GATE_ALLOCATION_FAILED
            : UNITY_SHADER_BUNDLE_GATE_DISCOVERY_FAILED;
    }
    size_t count = 0U;
    for (size_t index = 0U; index < discovered.count; ++index) {
        if (has_shader_extension(discovered.paths[index].path)) ++count;
    }
    if (count == 0U) {
        common_path_discovery_result_dispose(&discovered);
        return UNITY_SHADER_BUNDLE_GATE_NO_CANDIDATES;
    }
    char** paths = (char**)calloc(count, sizeof(*paths));
    if (!paths) {
        common_path_discovery_result_dispose(&discovered);
        return UNITY_SHADER_BUNDLE_GATE_ALLOCATION_FAILED;
    }
    size_t next = 0U;
    for (size_t index = 0U; index < discovered.count; ++index) {
        if (!has_shader_extension(discovered.paths[index].path)) continue;
        paths[next] = absolute_path(discovered.paths[index].path);
        if (!paths[next]) {
            CandidateList partial = {paths, next};
            candidate_list_dispose(&partial);
            common_path_discovery_result_dispose(&discovered);
            return UNITY_SHADER_BUNDLE_GATE_ALLOCATION_FAILED;
        }
        if (strlen(paths[next]) >= UNITY_SHADER_BUNDLE_PATH_CAPACITY) {
            CandidateList partial = {paths, next + 1U};
            candidate_list_dispose(&partial);
            common_path_discovery_result_dispose(&discovered);
            return UNITY_SHADER_BUNDLE_GATE_PATH_TOO_LONG;
        }
        ++next;
    }
    common_path_discovery_result_dispose(&discovered);
    candidates->paths = paths;
    candidates->count = count;
    return UNITY_SHADER_BUNDLE_GATE_OK;
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

static FILE* open_write_binary(const char* path) {
#ifdef _WIN32
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return NULL;
    FILE* file = _wfopen(wide, L"wb");
    free(wide);
    return file;
#else
    return fopen(path, "wb");
#endif
}

static bool write_bytes(const char* path, const void* data, size_t size) {
    FILE* file = open_write_binary(path);
    if (!file) return false;
    bool success = size == 0U || fwrite(data, 1U, size, file) == size;
    if (success && fflush(file) != 0) success = false;
    if (fclose(file) != 0) success = false;
    return success;
}

static bool copy_file(const char* source, const char* destination) {
    CommonFileBytes contents;
    CommonFileStatus status = common_file_read_regular(source, SIZE_MAX,
                                                       &contents);
    if (status != COMMON_FILE_OK) return false;
    bool success = write_bytes(destination, contents.data, contents.size);
    common_file_bytes_dispose(&contents);
    return success;
}

static bool copy_candidate_with_meta(const char* source,
                                     const char* destination,
                                     size_t candidate_index) {
    CommonFileBytes contents;
    CommonFileStatus read_status = common_file_read_regular(
        source, SIZE_MAX, &contents);
    if (read_status != COMMON_FILE_OK) return false;
    bool success = write_bytes(destination, contents.data, contents.size);
    char guid[33];
    bool guid_valid = unity_shader_bundle_candidate_guid(
        contents.data, contents.size, (uint64_t)candidate_index, guid);
    common_file_bytes_dispose(&contents);
    if (!success || !guid_valid) return false;
    static const char format[] =
        "fileFormatVersion: 2\n"
        "guid: %s\n"
        "ShaderImporter:\n"
        "  externalObjects: {}\n"
        "  defaultTextures: []\n"
        "  nonModifiableTextures: []\n"
        "  preprocessorOverride: 0\n"
        "  userData: \n"
        "  assetBundleName: \n"
        "  assetBundleVariant: \n";
    char metadata[512];
    int metadata_size = snprintf(metadata, sizeof(metadata), format, guid);
    size_t destination_length = strlen(destination);
    char* meta_path = destination_length <= SIZE_MAX - 6U
        ? (char*)malloc(destination_length + 6U) : NULL;
    if (!meta_path || metadata_size < 0 ||
        (size_t)metadata_size >= sizeof(metadata)) {
        free(meta_path);
        return false;
    }
    memcpy(meta_path, destination, destination_length);
    memcpy(meta_path + destination_length, ".meta", 6U);
    success = write_bytes(meta_path, metadata, (size_t)metadata_size);
    free(meta_path);
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
    char* unique = (char*)malloc(MAX_PATH + 1U);
    if (!unique) return NULL;
    if (GetTempFileNameA(root, "dsb", 0U, unique) == 0U ||
        !DeleteFileA(unique) || !CreateDirectoryA(unique, NULL)) {
        free(unique);
        return NULL;
    }
    return unique;
#else
    if (!nonempty(root)) {
        root = getenv("TMPDIR");
        if (!nonempty(root)) root = "/tmp";
    }
    char* pattern = join_path(root, "dxbc-shader-bundle.XXXXXX");
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
    FILE* file = open_write_binary(path);
    if (!file) return false;
    bool success = true;
    for (size_t index = 0U; index < candidates->count; ++index) {
        char asset[80];
        int length = snprintf(asset, sizeof(asset),
            "Assets/DXBCReleaseBundle/%08zu.shader", index);
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

static bool candidate_mapping_sha256(
    const CandidateList* candidates,
    char output[COMMON_SHA256_HEX_SIZE]) {
    if (!candidates || !output || candidates->count == 0U) return false;
    static const char domain[] =
        "DXBCSandbox/reimport-candidate-mapping/v1";
    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(&context, domain, sizeof(domain) - 1U);
    for (size_t index = 0U; index < candidates->count; ++index) {
        char asset[80];
        int asset_size = snprintf(asset, sizeof(asset),
            "Assets/DXBCReleaseBundle/%08zu.shader", index);
        if (asset_size < 0 || (size_t)asset_size >= sizeof(asset)) return false;
        mapping_update_u64(&context, (uint64_t)index);
        mapping_update_string(&context, candidates->paths[index]);
        mapping_update_string(&context, asset);
        mapping_update_string(&context, asset);
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256_final(&context, digest);
    common_sha256_digest_to_hex(digest, output);
    return true;
}

static bool prepare_project(const UnityShaderBundleGateOptions* options,
                            const CandidateList* candidates,
                            const char* workspace, char** out_manifest,
                            char** out_raw_result, char** out_internal_bundle) {
    char* assets = join_path(workspace, "Assets");
    char* editor = assets ? join_path(assets, "Editor") : NULL;
    char* candidates_dir = assets
        ? join_path(assets, "DXBCReleaseBundle") : NULL;
    char* settings = join_path(workspace, "ProjectSettings");
    char* packages = join_path(workspace, "Packages");
    char* bundle_output = join_path(workspace, "BundleOutput");
    bool success = assets && editor && candidates_dir && settings && packages &&
        bundle_output && make_directory(assets) && make_directory(editor) &&
        make_directory(candidates_dir) && make_directory(settings) &&
        make_directory(packages) && make_directory(bundle_output);
    char* bridge = editor
        ? join_path(editor, "DXBCShaderBundleGate.cs") : NULL;
    char* project_version = settings
        ? join_path(settings, "ProjectVersion.txt") : NULL;
    char* package_manifest = packages
        ? join_path(packages, "manifest.json") : NULL;
    char* manifest = join_path(workspace, "candidate-manifest.tsv");
    char* raw_result = join_path(workspace, "shader-bundle-result.json");
    char* internal_bundle = bundle_output
        ? join_path(bundle_output, BUNDLE_GATE_NAME) : NULL;
    if (!bridge || !project_version || !package_manifest || !manifest ||
        !raw_result || !internal_bundle) {
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
            success = length > 0 &&
                write_bytes(project_version, text, (size_t)length);
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
        success = destination && copy_candidate_with_meta(
            candidates->paths[index], destination, index);
        free(destination);
    }
    if (success) success = write_manifest(manifest, candidates);
    free(assets);
    free(editor);
    free(candidates_dir);
    free(settings);
    free(packages);
    free(bundle_output);
    free(bridge);
    free(project_version);
    free(package_manifest);
    if (!success) {
        free(manifest);
        free(raw_result);
        free(internal_bundle);
        return false;
    }
    *out_manifest = manifest;
    *out_raw_result = raw_result;
    *out_internal_bundle = internal_bundle;
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
    bool success = WaitForSingleObject(process.hProcess, INFINITE) ==
        WAIT_OBJECT_0;
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

static int path_existence(const char* path) {
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return -1;
    DWORD attributes = GetFileAttributesW(wide);
    DWORD error = GetLastError();
    free(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES) return 1;
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? 0 : -1;
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

static int path_existence(const char* path) {
    struct stat information;
    if (lstat(path, &information) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}
#endif

static UnityShaderBundleGateStatus publish_bundle(
    const char* internal_bundle, const char* output_bundle,
    const UnityShaderBundleGateSummary* summary) {
    CommonFileView view;
    CommonFileStatus open_status = common_file_view_open_regular(
        internal_bundle, SIZE_MAX, &view);
    if (open_status != COMMON_FILE_OK) return UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
    UnityShaderBundleGateStatus status = UNITY_SHADER_BUNDLE_GATE_OK;
    if (view.size == 0U || (uint64_t)view.size != summary->bundle_size) {
        status = UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
    } else {
        uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
        char hex[COMMON_SHA256_HEX_SIZE];
        if (!common_file_view_sha256(&view, digest)) {
            status = UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        } else {
            common_sha256_digest_to_hex(digest, hex);
        }
        if (status == UNITY_SHADER_BUNDLE_GATE_OK &&
            strcmp(hex, summary->bundle_sha256) != 0) {
            status = UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        } else {
            CommonFileStatus write_status = common_file_write_new_atomic(
                output_bundle, view.data, view.size);
            if (write_status == COMMON_FILE_ALREADY_EXISTS) {
                status = UNITY_SHADER_BUNDLE_GATE_OUTPUT_EXISTS;
            } else if (write_status != COMMON_FILE_OK) {
                status = UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
            }
        }
    }
    if (common_file_view_close(&view) != COMMON_FILE_OK &&
        status == UNITY_SHADER_BUNDLE_GATE_OK) {
        status = UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
    }
    return status;
}

UnityShaderBundleGateStatus unity_shader_bundle_gate_run(
    const UnityShaderBundleGateOptions* options,
    UnityShaderBundleGateRunResult* out_result) {
    if (!out_result) return UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    out_result->unity_exit_code = -1;
    if (!unity_shader_bundle_gate_options_validate(options)) {
        return UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT;
    }
    char* output_bundle = absolute_path(options->output_bundle_path);
    char* report_path = absolute_path(options->report_path);
    char* log_path = absolute_path(options->log_path);
    if (!output_bundle || !report_path || !log_path) {
        free(output_bundle);
        free(report_path);
        free(log_path);
        return UNITY_SHADER_BUNDLE_GATE_ALLOCATION_FAILED;
    }
    if (strlen(output_bundle) >= UNITY_SHADER_BUNDLE_PATH_CAPACITY ||
        strlen(report_path) >= UNITY_SHADER_BUNDLE_PATH_CAPACITY ||
        strlen(log_path) >= UNITY_SHADER_BUNDLE_PATH_CAPACITY) {
        free(output_bundle);
        free(report_path);
        free(log_path);
        return UNITY_SHADER_BUNDLE_GATE_PATH_TOO_LONG;
    }
    if (strcmp(output_bundle, report_path) == 0 ||
        strcmp(output_bundle, log_path) == 0 ||
        strcmp(report_path, log_path) == 0) {
        free(output_bundle);
        free(report_path);
        free(log_path);
        return UNITY_SHADER_BUNDLE_GATE_INVALID_ARGUMENT;
    }
    int exists = path_existence(output_bundle);
    if (exists != 0) {
        free(output_bundle);
        free(report_path);
        free(log_path);
        return exists > 0 ? UNITY_SHADER_BUNDLE_GATE_OUTPUT_EXISTS
                          : UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
    }

    CandidateList candidates = {NULL, 0U};
    UnityShaderBundleGateStatus status = discover_candidates(options,
                                                              &candidates);
    if (status != UNITY_SHADER_BUNDLE_GATE_OK) goto done_without_workspace;

    char* workspace = make_temporary_workspace(options->temporary_root);
    char* manifest = NULL;
    char* raw_result = NULL;
    char* internal_bundle = NULL;
    if (!workspace) {
        status = UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
        goto done_workspace;
    }
    size_t workspace_length = strlen(workspace);
    if (workspace_length >= sizeof(out_result->workspace_path)) {
        status = UNITY_SHADER_BUNDLE_GATE_PATH_TOO_LONG;
        goto done_workspace;
    }
    memcpy(out_result->workspace_path, workspace, workspace_length + 1U);
    if (!prepare_project(options, &candidates, workspace, &manifest,
                         &raw_result, &internal_bundle) ||
        !write_bytes(log_path, NULL, 0U)) {
        status = UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
        goto done_workspace;
    }
    char* bundle_output = join_path(workspace, "BundleOutput");
    if (!bundle_output) {
        status = UNITY_SHADER_BUNDLE_GATE_ALLOCATION_FAILED;
        goto done_workspace;
    }
    const char* policy = options->warning_policy ==
        UNITY_SHADER_IMPORT_WARNINGS_FAIL ? "fail" : "allow";
    const char* arguments[] = {
        options->unity_executable,
        "-batchmode",
        "-nographics",
        /* Match the import gate's bounded, package-free execution profile.
         * In particular, -diag-debug-shader-compiler is Unity's documented
         * single-ShaderCompiler mode and prevents a full-corpus certificate
         * from multiplying the largest generated sources across workers. */
        "-noUpm",
        "-job-worker-count", "1",
        "-diag-debug-shader-compiler",
        "-disableManagedDebugger",
        "-projectPath", workspace,
        "-logFile", log_path,
        "-executeMethod", "DXBCSandbox.Editor.DXBCShaderBundleGate.Run",
        "-dxbc-bundle-manifest", manifest,
        "-dxbc-bundle-result", raw_result,
        "-dxbc-bundle-output-dir", bundle_output,
        "-dxbc-published-bundle-path", output_bundle,
        "-dxbc-expected-unity-version", options->expected_unity_version,
        "-dxbc-warning-policy", policy,
        "-dxbc-build-target", unity_shader_bundle_target_name(options->target),
        "-dxbc-graphics-backend",
            unity_shader_bundle_backend_name(options->backend),
        NULL,
    };
    if (!launch_unity(arguments, &out_result->unity_exit_code)) {
        free(bundle_output);
        status = UNITY_SHADER_BUNDLE_GATE_PROCESS_FAILED;
        goto done_workspace;
    }
    free(bundle_output);

    CommonFileBytes result_bytes;
    CommonFileStatus read_status = common_file_read_regular(
        raw_result, BUNDLE_GATE_MAX_REPORT_SIZE, &result_bytes);
    if (read_status != COMMON_FILE_OK) {
        status = UNITY_SHADER_BUNDLE_GATE_PROCESS_FAILED;
        goto done_workspace;
    }
    bool report_published = write_bytes(report_path, result_bytes.data,
                                        result_bytes.size);
    UnityShaderBundleGateSummary summary;
    UnityShaderBundleGateStatus parse_status =
        unity_shader_bundle_gate_parse_result_json(
            result_bytes.data, result_bytes.size, &summary);
    common_file_bytes_dispose(&result_bytes);
    if (!report_published) {
        status = UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
        goto done_workspace;
    }
    if (parse_status != UNITY_SHADER_BUNDLE_GATE_OK ||
        summary.candidate_count != (uint64_t)candidates.count ||
        summary.warning_policy != options->warning_policy ||
        summary.target != options->target || summary.backend != options->backend ||
        strcmp(summary.unity_version, options->expected_unity_version) != 0 ||
        strcmp(summary.output_bundle_path, output_bundle) != 0) {
        status = UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        goto done_workspace;
    }
    char expected_mapping[COMMON_SHA256_HEX_SIZE];
    if (!candidate_mapping_sha256(&candidates, expected_mapping) ||
        strcmp(summary.candidate_mapping_sha256, expected_mapping) != 0) {
        status = UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID;
        goto done_workspace;
    }
    out_result->summary = summary;
    status = summary.reported_status;
    int expected_exit = status == UNITY_SHADER_BUNDLE_GATE_OK ? 0 :
        status == UNITY_SHADER_BUNDLE_GATE_DIAGNOSTICS_FOUND ? 2 :
        status == UNITY_SHADER_BUNDLE_GATE_TARGET_UNAVAILABLE ? 4 :
        status == UNITY_SHADER_BUNDLE_GATE_BACKEND_UNAVAILABLE ? 5 :
        status == UNITY_SHADER_BUNDLE_GATE_BUILD_FAILED ? 6 : -1;
    if (expected_exit < 0 || out_result->unity_exit_code != expected_exit) {
        status = UNITY_SHADER_BUNDLE_GATE_PROCESS_FAILED;
        goto done_workspace;
    }
    if (status == UNITY_SHADER_BUNDLE_GATE_OK) {
        status = publish_bundle(internal_bundle, output_bundle, &summary);
        out_result->bundle_published = status == UNITY_SHADER_BUNDLE_GATE_OK;
    }

done_workspace:
    if (workspace) {
        bool preserve = options->keep_policy == UNITY_SHADER_IMPORT_KEEP_ALWAYS ||
            (options->keep_policy == UNITY_SHADER_IMPORT_KEEP_ON_FAILURE &&
             status != UNITY_SHADER_BUNDLE_GATE_OK);
        if (!preserve && !remove_tree(workspace)) {
            if (status == UNITY_SHADER_BUNDLE_GATE_OK) {
                status = UNITY_SHADER_BUNDLE_GATE_IO_FAILED;
            }
            preserve = true;
        }
        out_result->workspace_preserved = preserve;
    }
    free(workspace);
    free(manifest);
    free(raw_result);
    free(internal_bundle);
done_without_workspace:
    candidate_list_dispose(&candidates);
    free(output_bundle);
    free(report_path);
    free(log_path);
    return status;
}
