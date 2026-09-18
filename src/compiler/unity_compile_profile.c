// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "compiler/unity_compile_profile.h"

#include "common/file_io.h"
#include "common/sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#define PROFILE_HEADER "DXBC_UNITY_COMPILE_PROFILE 1\n"
#define PROFILE_MAX_FILE_SIZE 2048U

static const char k_fingerprint_domain[] =
    "DXBCSandbox.UnityCompileProfile.v1";

static void store_le32(uint8_t output[4], uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static void store_le64(uint8_t output[8], uint64_t value) {
    for (unsigned i = 0; i < 8U; i++) {
        output[i] = (uint8_t)(value >> (i * 8U));
    }
}

static bool valid_provenance(const char* provenance, size_t* length) {
    if (!provenance) return false;
    const char* terminator = (const char*)memchr(
        provenance, '\0', UNITY_COMPILE_PROFILE_PROVENANCE_MAX + 1U);
    if (!terminator) return false;
    size_t size = (size_t)(terminator - provenance);
    if (size == 0U || size > UNITY_COMPILE_PROFILE_PROVENANCE_MAX ||
        provenance[0] == ' ' || provenance[size - 1U] == ' ') {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        unsigned char character = (unsigned char)provenance[i];
        if (character < 0x20U || character > 0x7eU || character == '=') {
            return false;
        }
    }
    if (length) *length = size;
    return true;
}

void unity_compile_profile_init(UnityCompileProfile* profile) {
    if (!profile) return;
    memset(profile, 0, sizeof(*profile));
    profile->schema_version = UNITY_COMPILE_PROFILE_SCHEMA_VERSION;
}

bool unity_compile_profile_validate(const UnityCompileProfile* profile) {
    const uint64_t valid_capabilities =
        (UINT64_C(1) << UNITY_COMPILE_PROFILE_CAPABILITY_COUNT) - UINT64_C(1);
    return profile &&
           profile->schema_version == UNITY_COMPILE_PROFILE_SCHEMA_VERSION &&
           (profile->d3d11_capabilities & ~valid_capabilities) == 0U &&
           (profile->glcore_capabilities & ~valid_capabilities) == 0U &&
           valid_provenance(profile->provenance, NULL);
}

UnityCompileProfileStatus unity_compile_profile_fingerprint(
    const UnityCompileProfile* profile,
    uint8_t fingerprint[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE]) {
    if (!profile || !fingerprint) {
        return UNITY_COMPILE_PROFILE_INVALID_ARGUMENT;
    }
    if (!unity_compile_profile_validate(profile)) {
        if (profile->schema_version !=
            UNITY_COMPILE_PROFILE_SCHEMA_VERSION) {
            return UNITY_COMPILE_PROFILE_UNSUPPORTED_VERSION;
        }
        const uint64_t valid_capabilities =
            (UINT64_C(1) << UNITY_COMPILE_PROFILE_CAPABILITY_COUNT) -
            UINT64_C(1);
        return ((profile->d3d11_capabilities |
                 profile->glcore_capabilities) &
                ~valid_capabilities) != 0U
                   ? UNITY_COMPILE_PROFILE_VALUE_OUT_OF_RANGE
                   : UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }
    size_t provenance_size = 0;
    (void)valid_provenance(profile->provenance, &provenance_size);
    uint8_t scalars[4U + 4U + 4U + 8U + 8U + 4U];
    uint8_t* cursor = scalars;
    store_le32(cursor, profile->schema_version);
    cursor += 4U;
    store_le32(cursor, profile->build_platform);
    cursor += 4U;
    store_le32(cursor, profile->valid_apis);
    cursor += 4U;
    store_le64(cursor, profile->d3d11_capabilities);
    cursor += 8U;
    store_le64(cursor, profile->glcore_capabilities);
    cursor += 8U;
    store_le32(cursor, (uint32_t)provenance_size);

    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(
        &context, k_fingerprint_domain, sizeof(k_fingerprint_domain));
    common_sha256_update(&context, scalars, sizeof(scalars));
    common_sha256_update(&context, profile->provenance, provenance_size);
    common_sha256_final(&context, fingerprint);
    return UNITY_COMPILE_PROFILE_OK;
}

static void digest_to_hex(
    const uint8_t digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE],
    char hex[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U + 1U]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE; i++) {
        hex[i * 2U] = digits[digest[i] >> 4U];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    hex[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U] = '\0';
}

UnityCompileProfileStatus unity_compile_profile_serialize(
    const UnityCompileProfile* profile, uint8_t** data, size_t* size) {
    if (!profile || !data || !size) {
        return UNITY_COMPILE_PROFILE_INVALID_ARGUMENT;
    }
    *data = NULL;
    *size = 0;
    uint8_t digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE];
    UnityCompileProfileStatus status =
        unity_compile_profile_fingerprint(profile, digest);
    if (status != UNITY_COMPILE_PROFILE_OK) return status;
    char hex[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U + 1U];
    digest_to_hex(digest, hex);
    int required = snprintf(
        NULL, 0,
        PROFILE_HEADER
        "build_platform=%" PRIu32 "\n"
        "valid_apis=%" PRIu32 "\n"
        "d3d11_capabilities=%" PRIu64 "\n"
        "glcore_capabilities=%" PRIu64 "\n"
        "provenance=%s\n"
        "fingerprint_sha256=%s\n",
        profile->build_platform, profile->valid_apis,
        profile->d3d11_capabilities, profile->glcore_capabilities,
        profile->provenance, hex);
    if (required < 0 || (size_t)required > PROFILE_MAX_FILE_SIZE) {
        return UNITY_COMPILE_PROFILE_TOO_LARGE;
    }
    uint8_t* output = (uint8_t*)malloc((size_t)required + 1U);
    if (!output) return UNITY_COMPILE_PROFILE_OUT_OF_MEMORY;
    int written = snprintf(
        (char*)output, (size_t)required + 1U,
        PROFILE_HEADER
        "build_platform=%" PRIu32 "\n"
        "valid_apis=%" PRIu32 "\n"
        "d3d11_capabilities=%" PRIu64 "\n"
        "glcore_capabilities=%" PRIu64 "\n"
        "provenance=%s\n"
        "fingerprint_sha256=%s\n",
        profile->build_platform, profile->valid_apis,
        profile->d3d11_capabilities, profile->glcore_capabilities,
        profile->provenance, hex);
    if (written != required) {
        free(output);
        return UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }
    *data = output;
    *size = (size_t)required;
    return UNITY_COMPILE_PROFILE_OK;
}

static bool take_line(
    const char** cursor, const char* end, const char** line,
    size_t* line_size) {
    if (!cursor || !*cursor || *cursor >= end) return false;
    const char* newline = (const char*)memchr(
        *cursor, '\n', (size_t)(end - *cursor));
    if (!newline) return false;
    *line = *cursor;
    *line_size = (size_t)(newline - *cursor);
    *cursor = newline + 1;
    return true;
}

static bool line_value(
    const char* line, size_t line_size, const char* key,
    const char** value, size_t* value_size) {
    size_t key_size = strlen(key);
    if (line_size <= key_size || memcmp(line, key, key_size) != 0) {
        return false;
    }
    *value = line + key_size;
    *value_size = line_size - key_size;
    return true;
}

typedef enum {
    DECIMAL_OK = 0,
    DECIMAL_INVALID_FORMAT,
    DECIMAL_OUT_OF_RANGE,
} DecimalParseStatus;

static DecimalParseStatus parse_decimal(
    const char* value, size_t size, uint64_t maximum, uint64_t* result) {
    if (!value || !result || size == 0U ||
        (size > 1U && value[0] == '0')) {
        return DECIMAL_INVALID_FORMAT;
    }
    uint64_t parsed = 0;
    for (size_t i = 0; i < size; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return DECIMAL_INVALID_FORMAT;
        }
        uint64_t digit = (uint64_t)(value[i] - '0');
        if (digit > maximum ||
            parsed > (maximum - digit) / UINT64_C(10)) {
            return DECIMAL_OUT_OF_RANGE;
        }
        parsed = parsed * UINT64_C(10) + digit;
    }
    *result = parsed;
    return DECIMAL_OK;
}

static bool hex_to_digest(
    const char* hex, size_t size,
    uint8_t digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE]) {
    if (!hex || size != UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U) {
        return false;
    }
    for (size_t i = 0; i < UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE; i++) {
        unsigned values[2];
        for (size_t nibble = 0; nibble < 2U; nibble++) {
            char character = hex[i * 2U + nibble];
            if (character >= '0' && character <= '9') {
                values[nibble] = (unsigned)(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                values[nibble] = (unsigned)(character - 'a') + 10U;
            } else {
                return false;
            }
        }
        digest[i] = (uint8_t)((values[0] << 4U) | values[1]);
    }
    return true;
}

UnityCompileProfileStatus unity_compile_profile_parse(
    const uint8_t* data, size_t size, UnityCompileProfile* profile) {
    if ((!data && size > 0U) || !profile) {
        return UNITY_COMPILE_PROFILE_INVALID_ARGUMENT;
    }
    if (size == 0U || size > PROFILE_MAX_FILE_SIZE ||
        memchr(data, '\0', size) != NULL) {
        return size > PROFILE_MAX_FILE_SIZE
                   ? UNITY_COMPILE_PROFILE_TOO_LARGE
                   : UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }
    const char* cursor = (const char*)data;
    const char* end = cursor + size;
    const char* line = NULL;
    size_t line_size = 0;
    if (!take_line(&cursor, end, &line, &line_size)) {
        return UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }
    static const char header[] = "DXBC_UNITY_COMPILE_PROFILE 1";
    if (line_size != sizeof(header) - 1U ||
        memcmp(line, header, sizeof(header) - 1U) != 0) {
        return line_size > 27U &&
                       memcmp(line, "DXBC_UNITY_COMPILE_PROFILE ", 27U) == 0
                   ? UNITY_COMPILE_PROFILE_UNSUPPORTED_VERSION
                   : UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }

    UnityCompileProfile parsed;
    unity_compile_profile_init(&parsed);
    const char* value = NULL;
    size_t value_size = 0;
    uint64_t number = 0;
#define PARSE_NUMBER_LINE(key, maximum, type, destination)                    \
    do {                                                                       \
        if (!take_line(&cursor, end, &line, &line_size) ||                     \
            !line_value(line, line_size, key, &value, &value_size)) {          \
            return UNITY_COMPILE_PROFILE_INVALID_FORMAT;                       \
        }                                                                      \
        DecimalParseStatus decimal_status =                                    \
            parse_decimal(value, value_size, maximum, &number);                \
        if (decimal_status != DECIMAL_OK) {                                    \
            return decimal_status == DECIMAL_OUT_OF_RANGE                      \
                       ? UNITY_COMPILE_PROFILE_VALUE_OUT_OF_RANGE               \
                       : UNITY_COMPILE_PROFILE_INVALID_FORMAT;                  \
        }                                                                      \
        destination = (type)number;                                            \
    } while (0)
    PARSE_NUMBER_LINE(
        "build_platform=", UINT32_MAX, uint32_t, parsed.build_platform);
    PARSE_NUMBER_LINE("valid_apis=", UINT32_MAX, uint32_t, parsed.valid_apis);
    PARSE_NUMBER_LINE(
        "d3d11_capabilities=",
        (UINT64_C(1) << UNITY_COMPILE_PROFILE_CAPABILITY_COUNT) - UINT64_C(1),
        uint64_t, parsed.d3d11_capabilities);
    PARSE_NUMBER_LINE(
        "glcore_capabilities=",
        (UINT64_C(1) << UNITY_COMPILE_PROFILE_CAPABILITY_COUNT) - UINT64_C(1),
        uint64_t, parsed.glcore_capabilities);
#undef PARSE_NUMBER_LINE

    if (!take_line(&cursor, end, &line, &line_size) ||
        !line_value(
            line, line_size, "provenance=", &value, &value_size) ||
        value_size == 0U ||
        value_size > UNITY_COMPILE_PROFILE_PROVENANCE_MAX) {
        return UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }
    memcpy(parsed.provenance, value, value_size);
    parsed.provenance[value_size] = '\0';
    if (!valid_provenance(parsed.provenance, NULL)) {
        return UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }
    if (!take_line(&cursor, end, &line, &line_size) || cursor != end ||
        !line_value(
            line, line_size, "fingerprint_sha256=", &value, &value_size) ||
        !hex_to_digest(value, value_size, parsed.fingerprint)) {
        return UNITY_COMPILE_PROFILE_INVALID_FORMAT;
    }
    uint8_t expected[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE];
    UnityCompileProfileStatus status =
        unity_compile_profile_fingerprint(&parsed, expected);
    if (status != UNITY_COMPILE_PROFILE_OK) return status;
    if (memcmp(expected, parsed.fingerprint, sizeof(expected)) != 0) {
        return UNITY_COMPILE_PROFILE_FINGERPRINT_MISMATCH;
    }
    uint8_t* canonical = NULL;
    size_t canonical_size = 0;
    status = unity_compile_profile_serialize(
        &parsed, &canonical, &canonical_size);
    if (status != UNITY_COMPILE_PROFILE_OK) return status;
    bool exact = canonical_size == size &&
                 memcmp(canonical, data, size) == 0;
    free(canonical);
    if (!exact) return UNITY_COMPILE_PROFILE_NONCANONICAL;
    *profile = parsed;
    return UNITY_COMPILE_PROFILE_OK;
}

UnityCompileProfileStatus unity_compile_profile_load(
    const char* path, UnityCompileProfile* profile) {
    if (!path || !path[0] || !profile) {
        return UNITY_COMPILE_PROFILE_INVALID_ARGUMENT;
    }
    CommonFileBytes file;
    CommonFileStatus file_status = common_file_read_regular(
        path, PROFILE_MAX_FILE_SIZE, &file);
    if (file_status == COMMON_FILE_TOO_LARGE) {
        return UNITY_COMPILE_PROFILE_TOO_LARGE;
    }
    if (file_status == COMMON_FILE_ALLOCATION_FAILED) {
        return UNITY_COMPILE_PROFILE_OUT_OF_MEMORY;
    }
    if (file_status != COMMON_FILE_OK) {
        return UNITY_COMPILE_PROFILE_IO_ERROR;
    }
    UnityCompileProfileStatus status =
        unity_compile_profile_parse(file.data, file.size, profile);
    common_file_bytes_dispose(&file);
    return status;
}

UnityCompileProfileStatus unity_compile_profile_write(
    const char* path, const UnityCompileProfile* profile) {
    if (!path || !path[0] || !profile) {
        return UNITY_COMPILE_PROFILE_INVALID_ARGUMENT;
    }
    uint8_t* data = NULL;
    size_t size = 0;
    UnityCompileProfileStatus status =
        unity_compile_profile_serialize(profile, &data, &size);
    if (status != UNITY_COMPILE_PROFILE_OK) return status;
    size_t path_size = strlen(path);
    static const char suffix[] = ".tmp.XXXXXX";
    if (path_size > SIZE_MAX - sizeof(suffix)) {
        free(data);
        return UNITY_COMPILE_PROFILE_TOO_LARGE;
    }
    char* temporary = (char*)malloc(path_size + sizeof(suffix));
    if (!temporary) {
        free(data);
        return UNITY_COMPILE_PROFILE_OUT_OF_MEMORY;
    }
    memcpy(temporary, path, path_size);
    memcpy(temporary + path_size, suffix, sizeof(suffix));
    FILE* file = NULL;
#ifdef _WIN32
    /* _mktemp_s chooses a candidate; _O_EXCL closes the race before open. */
    for (unsigned attempt = 0; attempt < 100U && !file; attempt++) {
        memcpy(temporary + path_size, suffix, sizeof(suffix));
        if (_mktemp_s(temporary, path_size + sizeof(suffix)) != 0) break;
        int descriptor = _open(
            temporary, _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY,
            _S_IREAD | _S_IWRITE);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            break;
        }
        file = _fdopen(descriptor, "wb");
        if (!file) {
            _close(descriptor);
            (void)remove(temporary);
        }
    }
#else
    int descriptor = mkstemp(temporary);
    if (descriptor >= 0) {
        file = fdopen(descriptor, "wb");
        if (!file) {
            close(descriptor);
            (void)remove(temporary);
        }
    }
#endif
    bool ok = file != NULL;
    if (ok) ok = size == 0U || fwrite(data, 1, size, file) == size;
    if (ok) ok = fflush(file) == 0;
    if (file && fclose(file) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(
                 temporary, path,
                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(temporary, path) == 0;
#endif
    }
    if (!ok) (void)remove(temporary);
    free(temporary);
    free(data);
    return ok ? UNITY_COMPILE_PROFILE_OK
              : UNITY_COMPILE_PROFILE_IO_ERROR;
}

const char* unity_compile_profile_status_string(
    UnityCompileProfileStatus status) {
    switch (status) {
        case UNITY_COMPILE_PROFILE_OK: return "ok";
        case UNITY_COMPILE_PROFILE_INVALID_ARGUMENT:
            return "invalid argument";
        case UNITY_COMPILE_PROFILE_IO_ERROR: return "I/O error";
        case UNITY_COMPILE_PROFILE_OUT_OF_MEMORY: return "out of memory";
        case UNITY_COMPILE_PROFILE_TOO_LARGE: return "profile is too large";
        case UNITY_COMPILE_PROFILE_INVALID_FORMAT:
            return "invalid profile format";
        case UNITY_COMPILE_PROFILE_UNSUPPORTED_VERSION:
            return "unsupported profile version";
        case UNITY_COMPILE_PROFILE_VALUE_OUT_OF_RANGE:
            return "profile value is out of range";
        case UNITY_COMPILE_PROFILE_NONCANONICAL:
            return "profile is not canonically encoded";
        case UNITY_COMPILE_PROFILE_FINGERPRINT_MISMATCH:
            return "profile fingerprint mismatch";
        default: return "unknown compile-profile status";
    }
}
