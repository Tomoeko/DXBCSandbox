// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compile_profile.h"
#include "common/windows_utf8.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* program) {
    fprintf(stderr,
            "usage:\n"
            "  %s validate PROFILE\n"
            "  %s check-value build-platform|valid-apis|capabilities VALUE\n"
            "  %s create PROFILE --build-platform N --valid-apis N "
            "--d3d11-platform-caps N --glcore-platform-caps N "
            "--provenance TEXT\n",
            program, program, program);
}

static int parse_unsigned(
    const char* text, uint64_t maximum, uint64_t* value) {
    if (!text || !text[0] || text[0] == '-' || !value) return 0;
    errno = 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' ||
        (uint64_t)parsed > maximum) {
        return 0;
    }
    *value = (uint64_t)parsed;
    return 1;
}

static void fingerprint_to_hex(
    const uint8_t digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE],
    char output[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U + 1U]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE; i++) {
        output[i * 2U] = digits[digest[i] >> 4U];
        output[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    output[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U] = '\0';
}

static int print_profile(const UnityCompileProfile* profile) {
    char fingerprint[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U + 1U];
    fingerprint_to_hex(profile->fingerprint, fingerprint);
    printf("schema_version=%" PRIu32 "\n", profile->schema_version);
    printf("build_platform=%" PRIu32 "\n", profile->build_platform);
    printf("valid_apis=%" PRIu32 "\n", profile->valid_apis);
    printf("d3d11_capabilities=%" PRIu64 "\n",
           profile->d3d11_capabilities);
    printf("glcore_capabilities=%" PRIu64 "\n",
           profile->glcore_capabilities);
    printf("provenance=%s\n", profile->provenance);
    printf("fingerprint_sha256=%s\n", fingerprint);
    return 0;
}

static int validate_profile(const char* path) {
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    UnityCompileProfileStatus status =
        unity_compile_profile_load(path, &profile);
    if (status != UNITY_COMPILE_PROFILE_OK) {
        fprintf(stderr, "invalid compile profile '%s': %s\n", path,
                unity_compile_profile_status_string(status));
        return 1;
    }
    return print_profile(&profile);
}

static int check_value(const char* kind, const char* text) {
    uint64_t maximum = 0;
    if (strcmp(kind, "build-platform") == 0 ||
        strcmp(kind, "valid-apis") == 0) {
        maximum = UINT32_MAX;
    } else if (strcmp(kind, "capabilities") == 0) {
        maximum = (UINT64_C(1) << UNITY_COMPILE_PROFILE_CAPABILITY_COUNT) -
                  UINT64_C(1);
    } else {
        fprintf(stderr, "unknown compile-profile value kind: %s\n", kind);
        return 2;
    }
    uint64_t value = 0;
    if (!parse_unsigned(text, maximum, &value)) {
        fprintf(stderr, "invalid %s value: %s\n", kind, text);
        return 1;
    }
    printf("%" PRIu64 "\n", value);
    return 0;
}

static int create_profile(int argc, char** argv) {
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    unsigned present = 0U;
    for (int i = 3; i < argc; i++) {
        const char* option = argv[i];
        if (++i >= argc || !argv[i][0]) {
            fprintf(stderr, "%s requires a nonempty value\n", option);
            return 2;
        }
        const char* value = argv[i];
        uint64_t parsed = 0;
        unsigned bit = 0U;
        if (strcmp(option, "--build-platform") == 0) {
            bit = 1U << 0;
            if (!parse_unsigned(value, UINT32_MAX, &parsed)) goto invalid;
            profile.build_platform = (uint32_t)parsed;
        } else if (strcmp(option, "--valid-apis") == 0) {
            bit = 1U << 1;
            if (!parse_unsigned(value, UINT32_MAX, &parsed)) goto invalid;
            profile.valid_apis = (uint32_t)parsed;
        } else if (strcmp(option, "--d3d11-platform-caps") == 0) {
            bit = 1U << 2;
            if (!parse_unsigned(
                    value,
                    (UINT64_C(1) <<
                     UNITY_COMPILE_PROFILE_CAPABILITY_COUNT) - UINT64_C(1),
                    &parsed)) {
                goto invalid;
            }
            profile.d3d11_capabilities = parsed;
        } else if (strcmp(option, "--glcore-platform-caps") == 0) {
            bit = 1U << 3;
            if (!parse_unsigned(
                    value,
                    (UINT64_C(1) <<
                     UNITY_COMPILE_PROFILE_CAPABILITY_COUNT) - UINT64_C(1),
                    &parsed)) {
                goto invalid;
            }
            profile.glcore_capabilities = parsed;
        } else if (strcmp(option, "--provenance") == 0) {
            bit = 1U << 4;
            size_t size = strlen(value);
            if (size > UNITY_COMPILE_PROFILE_PROVENANCE_MAX) goto invalid;
            memcpy(profile.provenance, value, size + 1U);
        } else {
            fprintf(stderr, "unknown create option: %s\n", option);
            return 2;
        }
        if ((present & bit) != 0U) {
            fprintf(stderr, "duplicate create option: %s\n", option);
            return 2;
        }
        present |= bit;
        continue;

invalid:
        fprintf(stderr, "invalid value for %s: %s\n", option, value);
        return 2;
    }
    if (present != 0x1fU) {
        fprintf(stderr, "create requires all five authority options\n");
        return 2;
    }
    UnityCompileProfileStatus status =
        unity_compile_profile_write(argv[2], &profile);
    if (status != UNITY_COMPILE_PROFILE_OK) {
        fprintf(stderr, "could not write compile profile '%s': %s\n",
                argv[2], unity_compile_profile_status_string(status));
        return 1;
    }
    return validate_profile(argv[2]);
}

static int compile_profile_main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "validate") == 0) {
        return validate_profile(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "check-value") == 0) {
        return check_value(argv[2], argv[3]);
    }
    if (argc >= 3 && strcmp(argv[1], "create") == 0) {
        return create_profile(argc, argv);
    }
    print_usage(argv[0]);
    return 2;
}

COMMON_DEFINE_UTF8_MAIN(compile_profile_main)
