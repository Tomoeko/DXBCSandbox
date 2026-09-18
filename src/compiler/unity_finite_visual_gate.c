// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_finite_visual_gate.h"

#include "common/sha256.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define FINITE_VISUAL_SCHEMA "dxbc-sandbox-unity-finite-visual/v1"
#define FINITE_VISUAL_MAX_LINE (16U * 1024U * 1024U)
#define FINITE_VISUAL_FIXED_FIELD_COUNT 28U

typedef struct {
    const uint8_t* data;
    size_t size;
} Field;

typedef struct {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
} DigestBytes;

enum {
    SEEN_STATUS = 1U << 0,
    SEEN_UNITY_VERSION = 1U << 1,
    SEEN_BACKEND = 1U << 2,
    SEEN_DEVICE_NAME = 1U << 3,
    SEEN_DEVICE_VENDOR = 1U << 4,
    SEEN_DEVICE_VERSION = 1U << 5,
    SEEN_REQUESTED_COLOR = 1U << 6,
    SEEN_COLOR = 1U << 7,
    SEEN_WIDTH = 1U << 8,
    SEEN_HEIGHT = 1U << 9,
    SEEN_PASS = 1U << 10,
    SEEN_FIXTURE_SHA = 1U << 11,
    SEEN_BRIDGE_SHA = 1U << 12,
    SEEN_BASELINE_SOURCE_SHA = 1U << 13,
    SEEN_CANDIDATE_SOURCE_SHA = 1U << 14,
    SEEN_BASELINE_IMPORTED = 1U << 15,
    SEEN_CANDIDATE_IMPORTED = 1U << 16,
    SEEN_BASELINE_STABLE = 1U << 17,
    SEEN_CANDIDATE_STABLE = 1U << 18,
    SEEN_PIXEL_EQUAL = 1U << 19,
    SEEN_PIXEL_BYTES = 1U << 20,
    SEEN_FIRST_MISMATCH = 1U << 21,
    SEEN_BASELINE_PIXEL_SHA = 1U << 22,
    SEEN_CANDIDATE_PIXEL_SHA = 1U << 23,
    SEEN_DIAGNOSTIC_COUNT = 1U << 24,
    SEEN_ERROR_COUNT = 1U << 25,
    SEEN_WARNING_COUNT = 1U << 26,
    SEEN_FAILURE_COUNT = 1U << 27,
};

static const uint32_t required_fields =
    SEEN_STATUS | SEEN_UNITY_VERSION | SEEN_BACKEND | SEEN_DEVICE_NAME |
    SEEN_DEVICE_VENDOR | SEEN_DEVICE_VERSION | SEEN_REQUESTED_COLOR |
    SEEN_COLOR | SEEN_WIDTH | SEEN_HEIGHT | SEEN_PASS | SEEN_FIXTURE_SHA |
    SEEN_BRIDGE_SHA | SEEN_BASELINE_SOURCE_SHA |
    SEEN_CANDIDATE_SOURCE_SHA | SEEN_BASELINE_IMPORTED |
    SEEN_CANDIDATE_IMPORTED | SEEN_BASELINE_STABLE |
    SEEN_CANDIDATE_STABLE | SEEN_PIXEL_EQUAL | SEEN_PIXEL_BYTES |
    SEEN_FIRST_MISMATCH | SEEN_BASELINE_PIXEL_SHA |
    SEEN_CANDIDATE_PIXEL_SHA | SEEN_DIAGNOSTIC_COUNT | SEEN_ERROR_COUNT |
    SEEN_WARNING_COUNT | SEEN_FAILURE_COUNT;

_Static_assert(FINITE_VISUAL_FIXED_FIELD_COUNT == 28U,
               "finite visual fixed-field count changed");
_Static_assert(WHOLE_SHADER_EVIDENCE_DIGEST_SIZE ==
                   COMMON_SHA256_DIGEST_SIZE,
               "finite visual evidence uses SHA-256");

static bool nonempty(const char* value) {
    return value && value[0] != '\0';
}

void unity_finite_visual_gate_options_init(
    UnityFiniteVisualGateOptions* options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->backend = UNITY_FINITE_VISUAL_BACKEND_METAL;
    options->keep_policy = UNITY_FINITE_VISUAL_KEEP_ON_FAILURE;
}

bool unity_finite_visual_gate_options_validate(
    const UnityFiniteVisualGateOptions* options) {
    if (!options || !nonempty(options->unity_executable) ||
        !nonempty(options->expected_unity_version) ||
        !nonempty(options->bridge_path) ||
        !nonempty(options->baseline_shader_path) ||
        !nonempty(options->candidate_shader_path) ||
        !nonempty(options->fixture_path) || !nonempty(options->report_path) ||
        !nonempty(options->log_path) ||
        !nonempty(options->baseline_pixels_path) ||
        !nonempty(options->candidate_pixels_path)) {
        return false;
    }
    if (strlen(options->expected_unity_version) >=
        UNITY_FINITE_VISUAL_VERSION_CAPACITY) {
        return false;
    }
    for (const unsigned char* at =
             (const unsigned char*)options->expected_unity_version;
         *at; ++at) {
        if (*at < 0x20U || *at == 0x7fU) return false;
    }
    if (options->backend < UNITY_FINITE_VISUAL_BACKEND_METAL ||
        options->backend > UNITY_FINITE_VISUAL_BACKEND_VULKAN ||
        options->keep_policy < UNITY_FINITE_VISUAL_KEEP_ON_FAILURE ||
        options->keep_policy > UNITY_FINITE_VISUAL_KEEP_NEVER) {
        return false;
    }
    const char* outputs[] = {
        options->report_path, options->log_path,
        options->baseline_pixels_path, options->candidate_pixels_path,
    };
    for (size_t left = 0U; left < sizeof(outputs) / sizeof(outputs[0]); ++left) {
        for (size_t right = left + 1U;
             right < sizeof(outputs) / sizeof(outputs[0]); ++right) {
            if (strcmp(outputs[left], outputs[right]) == 0) return false;
        }
    }
    return true;
}

const char* unity_finite_visual_backend_name(UnityFiniteVisualBackend backend) {
    switch (backend) {
        case UNITY_FINITE_VISUAL_BACKEND_METAL: return "metal";
        case UNITY_FINITE_VISUAL_BACKEND_D3D11: return "d3d11";
        case UNITY_FINITE_VISUAL_BACKEND_OPENGLCORE: return "openglcore";
        case UNITY_FINITE_VISUAL_BACKEND_VULKAN: return "vulkan";
        case UNITY_FINITE_VISUAL_BACKEND_UNSUPPORTED: return "unsupported";
        default: return "unknown";
    }
}

const char* unity_finite_visual_gate_status_name(
    UnityFiniteVisualGateStatus status) {
    switch (status) {
        case UNITY_FINITE_VISUAL_GATE_OK: return "ok";
        case UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH:
            return "pixel-mismatch";
        case UNITY_FINITE_VISUAL_GATE_DIAGNOSTICS_FOUND:
            return "diagnostics-found";
        case UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC:
            return "nondeterministic";
        case UNITY_FINITE_VISUAL_GATE_UNSUPPORTED_FIXTURE:
            return "unsupported-fixture";
        case UNITY_FINITE_VISUAL_GATE_INVALID_ARGUMENT:
            return "invalid-argument";
        case UNITY_FINITE_VISUAL_GATE_OUTPUT_EXISTS: return "output-exists";
        case UNITY_FINITE_VISUAL_GATE_ALLOCATION_FAILED:
            return "allocation-failed";
        case UNITY_FINITE_VISUAL_GATE_IO_FAILED: return "io-failed";
        case UNITY_FINITE_VISUAL_GATE_PROCESS_FAILED: return "process-failed";
        case UNITY_FINITE_VISUAL_GATE_RESULT_INVALID: return "result-invalid";
        case UNITY_FINITE_VISUAL_GATE_PATH_TOO_LONG: return "path-too-long";
        default: return "unknown";
    }
}

static bool field_equal(Field field, const char* literal) {
    const size_t length = strlen(literal);
    return field.size == length && memcmp(field.data, literal, length) == 0;
}

static bool parse_u64(Field field, uint64_t* output) {
    if (!output || field.size == 0U) return false;
    uint64_t value = 0U;
    if (field.size > 1U && field.data[0] == '0') return false;
    for (size_t index = 0U; index < field.size; ++index) {
        const uint8_t byte = field.data[index];
        if (byte < '0' || byte > '9') return false;
        const unsigned digit = (unsigned)(byte - '0');
        if (value > (UINT64_MAX - digit) / UINT64_C(10)) return false;
        value = value * UINT64_C(10) + digit;
    }
    *output = value;
    return true;
}

static bool parse_u32(Field field, uint32_t* output) {
    uint64_t value = 0U;
    if (!parse_u64(field, &value) || value > UINT32_MAX) return false;
    *output = (uint32_t)value;
    return true;
}

static bool parse_bool(Field field, bool* output) {
    if (!output || field.size != 1U ||
        (field.data[0] != '0' && field.data[0] != '1')) {
        return false;
    }
    *output = field.data[0] == '1';
    return true;
}

static int hex_value(uint8_t byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

static bool parse_sha256(Field field,
                         char output[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY]) {
    if (field.size != 64U) return false;
    for (size_t index = 0U; index < field.size; ++index) {
        if (hex_value(field.data[index]) < 0) return false;
    }
    memcpy(output, field.data, 64U);
    output[64] = '\0';
    return true;
}

static bool decode_sha256(
    const char hex[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY],
    uint8_t output[COMMON_SHA256_DIGEST_SIZE]) {
    if (!hex || strlen(hex) != 64U) return false;
    for (size_t index = 0U; index < COMMON_SHA256_DIGEST_SIZE; ++index) {
        const int high = hex_value((uint8_t)hex[index * 2U]);
        const int low = hex_value((uint8_t)hex[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        output[index] = (uint8_t)((unsigned)high << 4U | (unsigned)low);
    }
    return true;
}

static int base64_value(uint8_t byte) {
    if (byte >= 'A' && byte <= 'Z') return byte - 'A';
    if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9') return byte - '0' + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    return -1;
}

static bool base64_decoded_size(Field field, size_t* output) {
    if (!output || field.size % 4U != 0U) return false;
    size_t padding = 0U;
    if (field.size != 0U && field.data[field.size - 1U] == '=') ++padding;
    if (field.size > 1U && field.data[field.size - 2U] == '=') ++padding;
    for (size_t index = 0U; index < field.size; ++index) {
        if (field.data[index] == '=') {
            if (index < field.size - padding) return false;
        } else if (base64_value(field.data[index]) < 0) {
            return false;
        }
    }
    if (field.size == 0U) {
        *output = 0U;
        return true;
    }
    if (padding > 2U) return false;
    if (padding != 0U && field.size < 4U) return false;
    if (padding == 1U) {
        const int final_value = base64_value(field.data[field.size - 2U]);
        if (final_value < 0 || (final_value & 0x03) != 0) return false;
    } else if (padding == 2U) {
        const int final_value = base64_value(field.data[field.size - 3U]);
        if (final_value < 0 || (final_value & 0x0f) != 0) return false;
    }
    *output = field.size / 4U * 3U - padding;
    return true;
}

static bool utf8_is_valid(const uint8_t* data, size_t size) {
    size_t index = 0U;
    while (index < size) {
        const uint8_t first = data[index++];
        if (first <= 0x7fU) {
            if (first == 0U) return false;
            continue;
        }
        uint32_t value = 0U;
        size_t continuation_count = 0U;
        uint32_t minimum = 0U;
        if (first >= 0xc2U && first <= 0xdfU) {
            value = first & 0x1fU;
            continuation_count = 1U;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            value = first & 0x0fU;
            continuation_count = 2U;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            value = first & 0x07U;
            continuation_count = 3U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation_count > size - index) return false;
        for (size_t offset = 0U; offset < continuation_count; ++offset) {
            const uint8_t next = data[index++];
            if ((next & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (uint32_t)(next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

static bool decode_base64(Field field, uint8_t* output,
                          size_t expected_size) {
    size_t decoded_size = 0U;
    if ((!output && expected_size != 0U) ||
        !base64_decoded_size(field, &decoded_size) ||
        decoded_size != expected_size) {
        return false;
    }
    size_t next = 0U;
    for (size_t index = 0U; index < field.size; index += 4U) {
        const int a = base64_value(field.data[index]);
        const int b = base64_value(field.data[index + 1U]);
        const int c = field.data[index + 2U] == '=' ? 0 :
            base64_value(field.data[index + 2U]);
        const int d = field.data[index + 3U] == '=' ? 0 :
            base64_value(field.data[index + 3U]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        const uint32_t value = ((uint32_t)a << 18U) |
            ((uint32_t)b << 12U) | ((uint32_t)c << 6U) | (uint32_t)d;
        output[next++] = (uint8_t)(value >> 16U);
        if (field.data[index + 2U] != '=') {
            output[next++] = (uint8_t)(value >> 8U);
        }
        if (field.data[index + 3U] != '=') output[next++] = (uint8_t)value;
    }
    return next == decoded_size;
}

static bool decode_base64_text(Field field, char* output, size_t capacity) {
    size_t decoded_size = 0U;
    if (!output || !base64_decoded_size(field, &decoded_size) ||
        decoded_size >= capacity) {
        return false;
    }
    if (!decode_base64(field, (uint8_t*)output, decoded_size) ||
        !utf8_is_valid((const uint8_t*)output, decoded_size)) {
        return false;
    }
    output[decoded_size] = '\0';
    return true;
}

static size_t split_fields(const uint8_t* line, size_t size,
                           Field* fields, size_t capacity) {
    if (!line || !fields || capacity == 0U) return 0U;
    size_t count = 0U;
    size_t start = 0U;
    for (size_t index = 0U; index <= size; ++index) {
        if (index != size && line[index] != '\t') continue;
        if (count == capacity) return 0U;
        fields[count].data = line + start;
        fields[count].size = index - start;
        ++count;
        start = index + 1U;
    }
    return count;
}

static bool parse_report_status(Field field,
                                UnityFiniteVisualGateStatus* output) {
    if (field_equal(field, "ok")) {
        *output = UNITY_FINITE_VISUAL_GATE_OK;
    } else if (field_equal(field, "pixel-mismatch")) {
        *output = UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH;
    } else if (field_equal(field, "diagnostics-found")) {
        *output = UNITY_FINITE_VISUAL_GATE_DIAGNOSTICS_FOUND;
    } else if (field_equal(field, "nondeterministic")) {
        *output = UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC;
    } else if (field_equal(field, "unsupported-fixture")) {
        *output = UNITY_FINITE_VISUAL_GATE_UNSUPPORTED_FIXTURE;
    } else {
        return false;
    }
    return true;
}

static bool parse_backend(Field field, UnityFiniteVisualBackend* output) {
    for (int backend = (int)UNITY_FINITE_VISUAL_BACKEND_METAL;
         backend <= (int)UNITY_FINITE_VISUAL_BACKEND_UNSUPPORTED; ++backend) {
        if (field_equal(field, unity_finite_visual_backend_name(
                                   (UnityFiniteVisualBackend)backend))) {
            *output = (UnityFiniteVisualBackend)backend;
            return true;
        }
    }
    return false;
}

static bool set_once(uint32_t* seen, uint32_t bit) {
    if ((*seen & bit) != 0U) return false;
    *seen |= bit;
    return true;
}

static bool parse_fixed_record(Field* fields, size_t field_count,
                               uint32_t* seen,
                               UnityFiniteVisualGateSummary* summary) {
    if (field_count != 2U) return false;
#define KEY(literal, bit) \
    (field_equal(fields[0], (literal)) && set_once(seen, (bit)))
    if (KEY("status", SEEN_STATUS)) {
        return parse_report_status(fields[1], &summary->reported_status);
    }
    if (KEY("unity_version", SEEN_UNITY_VERSION)) {
        return decode_base64_text(fields[1], summary->unity_version,
                                  sizeof(summary->unity_version));
    }
    if (KEY("backend", SEEN_BACKEND)) {
        return parse_backend(fields[1], &summary->backend);
    }
    if (KEY("device_name", SEEN_DEVICE_NAME)) {
        return decode_base64_text(fields[1], summary->graphics_device_name,
                                  sizeof(summary->graphics_device_name));
    }
    if (KEY("device_vendor", SEEN_DEVICE_VENDOR)) {
        return decode_base64_text(fields[1], summary->graphics_device_vendor,
                                  sizeof(summary->graphics_device_vendor));
    }
    if (KEY("device_version", SEEN_DEVICE_VERSION)) {
        return decode_base64_text(fields[1], summary->graphics_device_version,
                                  sizeof(summary->graphics_device_version));
    }
    if (KEY("requested_color_space", SEEN_REQUESTED_COLOR)) {
        return decode_base64_text(fields[1], summary->requested_color_space,
                                  sizeof(summary->requested_color_space));
    }
    if (KEY("color_space", SEEN_COLOR)) {
        return decode_base64_text(fields[1], summary->color_space,
                                  sizeof(summary->color_space));
    }
    if (KEY("width", SEEN_WIDTH)) return parse_u32(fields[1], &summary->width);
    if (KEY("height", SEEN_HEIGHT)) return parse_u32(fields[1], &summary->height);
    if (KEY("pass", SEEN_PASS)) return parse_u32(fields[1], &summary->pass_index);
    if (KEY("fixture_sha256", SEEN_FIXTURE_SHA)) {
        return parse_sha256(fields[1], summary->fixture_sha256);
    }
    if (KEY("bridge_sha256", SEEN_BRIDGE_SHA)) {
        return parse_sha256(fields[1], summary->bridge_sha256);
    }
    if (KEY("baseline_source_sha256", SEEN_BASELINE_SOURCE_SHA)) {
        return parse_sha256(fields[1], summary->baseline_source_sha256);
    }
    if (KEY("candidate_source_sha256", SEEN_CANDIDATE_SOURCE_SHA)) {
        return parse_sha256(fields[1], summary->candidate_source_sha256);
    }
    if (KEY("baseline_imported", SEEN_BASELINE_IMPORTED)) {
        return parse_bool(fields[1], &summary->baseline_imported);
    }
    if (KEY("candidate_imported", SEEN_CANDIDATE_IMPORTED)) {
        return parse_bool(fields[1], &summary->candidate_imported);
    }
    if (KEY("baseline_stable", SEEN_BASELINE_STABLE)) {
        return parse_bool(fields[1], &summary->baseline_stable);
    }
    if (KEY("candidate_stable", SEEN_CANDIDATE_STABLE)) {
        return parse_bool(fields[1], &summary->candidate_stable);
    }
    if (KEY("pixel_equal", SEEN_PIXEL_EQUAL)) {
        return parse_bool(fields[1], &summary->pixel_equal);
    }
    if (KEY("pixel_byte_count", SEEN_PIXEL_BYTES)) {
        return parse_u64(fields[1], &summary->pixel_byte_count);
    }
    if (KEY("first_mismatch_offset", SEEN_FIRST_MISMATCH)) {
        return parse_u64(fields[1], &summary->first_mismatch_offset);
    }
    if (KEY("baseline_pixels_sha256", SEEN_BASELINE_PIXEL_SHA)) {
        return parse_sha256(fields[1], summary->baseline_pixels_sha256);
    }
    if (KEY("candidate_pixels_sha256", SEEN_CANDIDATE_PIXEL_SHA)) {
        return parse_sha256(fields[1], summary->candidate_pixels_sha256);
    }
    if (KEY("diagnostic_count", SEEN_DIAGNOSTIC_COUNT)) {
        return parse_u64(fields[1], &summary->diagnostic_count);
    }
    if (KEY("error_count", SEEN_ERROR_COUNT)) {
        return parse_u64(fields[1], &summary->error_count);
    }
    if (KEY("warning_count", SEEN_WARNING_COUNT)) {
        return parse_u64(fields[1], &summary->warning_count);
    }
    if (KEY("failure_count", SEEN_FAILURE_COUNT)) {
        return parse_u64(fields[1], &summary->failure_count);
    }
#undef KEY
    return false;
}

static bool validate_dynamic_text_field(Field field) {
    size_t decoded_size = 0U;
    if (!base64_decoded_size(field, &decoded_size) ||
        decoded_size > FINITE_VISUAL_MAX_LINE) {
        return false;
    }
    uint8_t* decoded = decoded_size == 0U ? NULL :
        (uint8_t*)malloc(decoded_size);
    if (decoded_size != 0U && !decoded) return false;
    const bool valid = decode_base64(field, decoded, decoded_size) &&
        utf8_is_valid(decoded, decoded_size);
    free(decoded);
    return valid;
}

static bool parse_diagnostic(Field* fields, size_t count,
                             uint64_t* diagnostics, uint64_t* errors,
                             uint64_t* warnings) {
    /* diagnostic side severity line file message platform details */
    if (count != 8U ||
        (!field_equal(fields[1], "baseline") &&
         !field_equal(fields[1], "candidate") &&
         !field_equal(fields[1], "gate")) ||
        (!field_equal(fields[2], "error") &&
         !field_equal(fields[2], "warning") &&
         !field_equal(fields[2], "info"))) {
        return false;
    }
    uint64_t line = 0U;
    if (!parse_u64(fields[3], &line) || line > UINT32_MAX) return false;
    for (size_t index = 4U; index < count; ++index) {
        if (!validate_dynamic_text_field(fields[index])) return false;
    }
    if (*diagnostics == UINT64_MAX) return false;
    ++*diagnostics;
    if (field_equal(fields[2], "error")) {
        if (*errors == UINT64_MAX) return false;
        ++*errors;
    } else if (field_equal(fields[2], "warning")) {
        if (*warnings == UINT64_MAX) return false;
        ++*warnings;
    }
    return true;
}

static bool parse_failure(Field* fields, size_t count, uint64_t* failures) {
    /* failure kind message, both base64 UTF-8 */
    if (count != 3U || !validate_dynamic_text_field(fields[1]) ||
        !validate_dynamic_text_field(fields[2]) || *failures == UINT64_MAX) {
        return false;
    }
    ++*failures;
    return true;
}

static void hash_length_prefixed(CommonSha256Context* hash,
                                 const void* data, size_t size) {
    uint8_t encoded[8];
    uint64_t length = (uint64_t)size;
    for (unsigned index = 0U; index < 8U; ++index) {
        encoded[index] = (uint8_t)(length >> (index * 8U));
    }
    common_sha256_update(hash, encoded, sizeof(encoded));
    common_sha256_update(hash, data, size);
}

static void hash_u32_le(CommonSha256Context* hash, uint32_t value) {
    uint8_t encoded[4];
    for (unsigned index = 0U; index < 4U; ++index) {
        encoded[index] = (uint8_t)(value >> (index * 8U));
    }
    hash_length_prefixed(hash, encoded, sizeof(encoded));
}

static void compute_coordinates(UnityFiniteVisualGateSummary* summary,
                                const char* expected_version,
                                UnityFiniteVisualBackend expected_backend) {
    DigestBytes fixture;
    DigestBytes bridge;
    (void)decode_sha256(summary->fixture_sha256, fixture.digest);
    (void)decode_sha256(summary->bridge_sha256, bridge.digest);

    static const char case_domain[] = "DXBCSandbox finite render case/v1";
    CommonSha256Context hash;
    common_sha256_init(&hash);
    hash_length_prefixed(&hash, case_domain, sizeof(case_domain) - 1U);
    hash_length_prefixed(&hash, fixture.digest, sizeof(fixture.digest));
    hash_length_prefixed(&hash, bridge.digest, sizeof(bridge.digest));
    hash_u32_le(&hash, summary->width);
    hash_u32_le(&hash, summary->height);
    hash_u32_le(&hash, summary->pass_index);
    common_sha256_final(&hash, summary->render_case_identity);

    static const char authority_domain[] =
        "DXBCSandbox finite visual authority/v1";
    common_sha256_init(&hash);
    hash_length_prefixed(&hash, authority_domain,
                         sizeof(authority_domain) - 1U);
    hash_length_prefixed(&hash, summary->unity_version,
                         strlen(summary->unity_version));
    const char* actual_backend = unity_finite_visual_backend_name(
        summary->backend);
    hash_length_prefixed(&hash, actual_backend, strlen(actual_backend));
    hash_length_prefixed(&hash, summary->color_space,
                         strlen(summary->color_space));
    hash_length_prefixed(&hash, summary->graphics_device_name,
                         strlen(summary->graphics_device_name));
    hash_length_prefixed(&hash, summary->graphics_device_vendor,
                         strlen(summary->graphics_device_vendor));
    hash_length_prefixed(&hash, summary->graphics_device_version,
                         strlen(summary->graphics_device_version));
    hash_length_prefixed(&hash, bridge.digest, sizeof(bridge.digest));
    common_sha256_final(&hash, summary->authority_digest);

    static const char runtime_domain[] =
        "DXBCSandbox finite runtime inputs/v1";
    common_sha256_init(&hash);
    hash_length_prefixed(&hash, runtime_domain, sizeof(runtime_domain) - 1U);
    hash_length_prefixed(&hash, fixture.digest, sizeof(fixture.digest));
    hash_length_prefixed(&hash, expected_version, strlen(expected_version));
    const char* expected_backend_name =
        unity_finite_visual_backend_name(expected_backend);
    hash_length_prefixed(&hash, expected_backend_name,
                         strlen(expected_backend_name));
    hash_length_prefixed(&hash, summary->requested_color_space,
                         strlen(summary->requested_color_space));
    hash_length_prefixed(&hash, bridge.digest, sizeof(bridge.digest));
    common_sha256_final(&hash, summary->expected_runtime_inputs_digest);

    common_sha256_init(&hash);
    hash_length_prefixed(&hash, runtime_domain, sizeof(runtime_domain) - 1U);
    hash_length_prefixed(&hash, fixture.digest, sizeof(fixture.digest));
    hash_length_prefixed(&hash, summary->unity_version,
                         strlen(summary->unity_version));
    hash_length_prefixed(&hash, actual_backend, strlen(actual_backend));
    hash_length_prefixed(&hash, summary->color_space,
                         strlen(summary->color_space));
    hash_length_prefixed(&hash, bridge.digest, sizeof(bridge.digest));
    common_sha256_final(&hash, summary->observed_runtime_inputs_digest);
}

static bool summary_consistent(UnityFiniteVisualGateSummary* summary,
                               uint64_t actual_diagnostics,
                               uint64_t actual_errors,
                               uint64_t actual_warnings,
                               uint64_t actual_failures) {
    if (summary->width == 0U || summary->height == 0U ||
        summary->width > 4096U || summary->height > 4096U ||
        summary->pass_index > 65535U ||
        summary->diagnostic_count != actual_diagnostics ||
        summary->error_count != actual_errors ||
        summary->warning_count != actual_warnings ||
        summary->failure_count != actual_failures ||
        summary->unity_version[0] == '\0' ||
        (strcmp(summary->requested_color_space, "linear") != 0 &&
         strcmp(summary->requested_color_space, "gamma") != 0) ||
        (strcmp(summary->color_space, "linear") != 0 &&
         strcmp(summary->color_space, "gamma") != 0)) {
        return false;
    }
    const uint64_t pixels = (uint64_t)summary->width * summary->height;
    if (pixels > UINT64_MAX / 16U ||
        summary->pixel_byte_count != pixels * 16U) {
        return false;
    }
    if (summary->pixel_equal) {
        if (summary->first_mismatch_offset != UINT64_MAX ||
            strcmp(summary->baseline_pixels_sha256,
                   summary->candidate_pixels_sha256) != 0) {
            return false;
        }
    } else if (summary->baseline_stable && summary->candidate_stable &&
               summary->baseline_imported && summary->candidate_imported) {
        if (summary->first_mismatch_offset >= summary->pixel_byte_count ||
            strcmp(summary->baseline_pixels_sha256,
                   summary->candidate_pixels_sha256) == 0) {
            return false;
        }
    }
    const bool capture_status =
        summary->reported_status == UNITY_FINITE_VISUAL_GATE_OK ||
        summary->reported_status == UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH ||
        summary->reported_status == UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC;
    static const char zero_sha[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    if (!capture_status &&
        (summary->baseline_stable || summary->candidate_stable ||
         summary->pixel_equal || summary->first_mismatch_offset != UINT64_MAX ||
         strcmp(summary->baseline_pixels_sha256, zero_sha) != 0 ||
         strcmp(summary->candidate_pixels_sha256, zero_sha) != 0)) {
        return false;
    }
    switch (summary->reported_status) {
        case UNITY_FINITE_VISUAL_GATE_OK:
            if (!summary->baseline_imported || !summary->candidate_imported ||
                !summary->baseline_stable || !summary->candidate_stable ||
                !summary->pixel_equal || actual_errors != 0U ||
                actual_warnings != 0U || actual_failures != 0U ||
                strcmp(summary->requested_color_space,
                       summary->color_space) != 0) {
                return false;
            }
            summary->passed = true;
            return true;
        case UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH:
            return summary->baseline_imported && summary->candidate_imported &&
                summary->baseline_stable && summary->candidate_stable &&
                !summary->pixel_equal && actual_errors == 0U &&
                actual_warnings == 0U && actual_failures == 0U;
        case UNITY_FINITE_VISUAL_GATE_DIAGNOSTICS_FOUND:
            return actual_errors != 0U || actual_warnings != 0U;
        case UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC:
            return summary->baseline_imported && summary->candidate_imported &&
                (!summary->baseline_stable || !summary->candidate_stable) &&
                actual_errors == 0U && actual_warnings == 0U &&
                actual_failures == 0U;
        case UNITY_FINITE_VISUAL_GATE_UNSUPPORTED_FIXTURE:
            return actual_failures != 0U && actual_errors == 0U &&
                actual_warnings == 0U;
        default: return false;
    }
}

UnityFiniteVisualGateStatus unity_finite_visual_gate_parse_result(
    const uint8_t* data, size_t size,
    UnityFiniteVisualGateSummary* out_summary) {
    if (!out_summary || (!data && size != 0U)) {
        return UNITY_FINITE_VISUAL_GATE_INVALID_ARGUMENT;
    }
    if (size == 0U || data[size - 1U] != '\n') {
        return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
    }
    UnityFiniteVisualGateSummary parsed;
    memset(&parsed, 0, sizeof(parsed));
    uint32_t seen = 0U;
    uint64_t actual_diagnostics = 0U;
    uint64_t actual_errors = 0U;
    uint64_t actual_warnings = 0U;
    uint64_t actual_failures = 0U;
    size_t offset = 0U;
    size_t line_index = 0U;
    bool ended = false;
    while (offset < size) {
        const size_t start = offset;
        while (offset < size && data[offset] != '\n') {
            if (data[offset] == '\r' || data[offset] == '\0') {
                return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
            }
            ++offset;
            if (offset - start > FINITE_VISUAL_MAX_LINE) {
                return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
            }
        }
        if (offset >= size) return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
        const size_t line_size = offset - start;
        ++offset;
        if (line_index++ == 0U) {
            if (line_size != sizeof(FINITE_VISUAL_SCHEMA) - 1U ||
                memcmp(data + start, FINITE_VISUAL_SCHEMA,
                       sizeof(FINITE_VISUAL_SCHEMA) - 1U) != 0) {
                return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
            }
            continue;
        }
        if (ended || line_size == 0U) {
            return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
        }
        Field fields[8];
        const size_t field_count = split_fields(
            data + start, line_size, fields,
            sizeof(fields) / sizeof(fields[0]));
        if (field_count == 0U) return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
        if (field_equal(fields[0], "end")) {
            if (field_count != 1U || offset != size) {
                return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
            }
            ended = true;
        } else if (field_equal(fields[0], "diagnostic")) {
            if (!parse_diagnostic(fields, field_count, &actual_diagnostics,
                                  &actual_errors, &actual_warnings)) {
                return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
            }
        } else if (field_equal(fields[0], "failure")) {
            if (!parse_failure(fields, field_count, &actual_failures)) {
                return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
            }
        } else if (!parse_fixed_record(fields, field_count, &seen, &parsed)) {
            return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
        }
    }
    if (!ended || seen != required_fields ||
        !summary_consistent(&parsed, actual_diagnostics, actual_errors,
                            actual_warnings, actual_failures)) {
        return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
    }
    *out_summary = parsed;
    return UNITY_FINITE_VISUAL_GATE_OK;
}

static bool digest_is_zero(const uint8_t* digest) {
    uint8_t value = 0U;
    for (size_t index = 0U; index < WHOLE_SHADER_EVIDENCE_DIGEST_SIZE;
         ++index) {
        value |= digest[index];
    }
    return value == 0U;
}

WholeShaderEvidenceStatus unity_finite_visual_gate_make_evidence(
    const UnityFiniteVisualGateSummary* summary,
    const WholeShaderSubject* subject,
    WholeShaderEvidence** out_runtime_inputs,
    WholeShaderEvidence** out_empirical_pixels) {
    if (!out_runtime_inputs || !out_empirical_pixels) {
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    *out_runtime_inputs = NULL;
    *out_empirical_pixels = NULL;
    if (!summary || !subject ||
        (summary->reported_status != UNITY_FINITE_VISUAL_GATE_OK &&
         summary->reported_status != UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH) ||
        !summary->baseline_imported || !summary->candidate_imported ||
        !summary->baseline_stable || !summary->candidate_stable ||
        summary->pixel_byte_count == 0U ||
        digest_is_zero(summary->render_case_identity) ||
        digest_is_zero(summary->authority_digest) ||
        memcmp(summary->expected_runtime_inputs_digest,
               summary->observed_runtime_inputs_digest,
               WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) != 0) {
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    WholeShaderEvidenceComparisonItem input_item;
    memset(&input_item, 0, sizeof(input_item));
    memcpy(input_item.identity_digest, summary->render_case_identity,
           sizeof(input_item.identity_digest));
    memcpy(input_item.expected_digest,
           summary->expected_runtime_inputs_digest,
           sizeof(input_item.expected_digest));
    memcpy(input_item.observed_digest,
           summary->observed_runtime_inputs_digest,
           sizeof(input_item.observed_digest));
    WholeShaderComparisonEvidenceDescriptor input_descriptor;
    memset(&input_descriptor, 0, sizeof(input_descriptor));
    input_descriptor.plane = WHOLE_SHADER_PLANE_RUNTIME_INPUTS;
    input_descriptor.producer = "dxbc-unity-finite-visual-gate";
    input_descriptor.producer_version = 1U;
    memcpy(input_descriptor.authority_digest, summary->authority_digest,
           sizeof(input_descriptor.authority_digest));
    input_descriptor.items = &input_item;
    input_descriptor.item_count = 1U;
    WholeShaderEvidenceStatus status = whole_shader_evidence_create_comparison(
        out_runtime_inputs, subject, &input_descriptor);
    if (status != WHOLE_SHADER_EVIDENCE_OK) return status;

    WholeShaderEvidenceComparisonItem pixel_item;
    memset(&pixel_item, 0, sizeof(pixel_item));
    memcpy(pixel_item.identity_digest, summary->render_case_identity,
           sizeof(pixel_item.identity_digest));
    if (!decode_sha256(summary->baseline_pixels_sha256,
                       pixel_item.expected_digest) ||
        !decode_sha256(summary->candidate_pixels_sha256,
                       pixel_item.observed_digest)) {
        whole_shader_evidence_free(*out_runtime_inputs);
        *out_runtime_inputs = NULL;
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    WholeShaderComparisonEvidenceDescriptor pixel_descriptor;
    memset(&pixel_descriptor, 0, sizeof(pixel_descriptor));
    pixel_descriptor.plane = WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS;
    pixel_descriptor.producer = "dxbc-unity-finite-visual-gate";
    pixel_descriptor.producer_version = 1U;
    memcpy(pixel_descriptor.authority_digest, summary->authority_digest,
           sizeof(pixel_descriptor.authority_digest));
    pixel_descriptor.items = &pixel_item;
    pixel_descriptor.item_count = 1U;
    status = whole_shader_evidence_create_comparison(
        out_empirical_pixels, subject, &pixel_descriptor);
    if (status != WHOLE_SHADER_EVIDENCE_OK) {
        whole_shader_evidence_free(*out_runtime_inputs);
        *out_runtime_inputs = NULL;
    }
    return status;
}

/* The platform/process implementation lives in a separate translation unit.
 * It calls this private hook after validating requested files and pins. */
void unity_finite_visual_gate_compute_requested_coordinates(
    UnityFiniteVisualGateSummary* summary, const char* expected_version,
    UnityFiniteVisualBackend expected_backend) {
    compute_coordinates(summary, expected_version, expected_backend);
}
