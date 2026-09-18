#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "compiler/unity_compile_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static UnityCompileProfile make_profile(void) {
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19U;
    profile.valid_apis = 295472U;
    profile.d3d11_capabilities = UINT64_C(305419896);
    profile.glcore_capabilities = UINT64_C(305419896);
    strcpy(profile.provenance,
           "test-capture:Unity-2021.3.35f1:fixture-only");
    return profile;
}

static int make_existing_test_path(char* path, size_t capacity) {
#ifdef _WIN32
    char directory[MAX_PATH];
    DWORD length = GetTempPathA((DWORD)sizeof(directory), directory);
    if (length == 0U || length >= sizeof(directory) || capacity < MAX_PATH) {
        return 0;
    }
    return GetTempFileNameA(directory, "dcp", 0U, path) != 0U;
#else
    static const char pattern[] = "/tmp/dxbc-compile-profile-test.XXXXXX";
    if (capacity < sizeof(pattern)) return 0;
    memcpy(path, pattern, sizeof(pattern));
    int descriptor = mkstemp(path);
    if (descriptor < 0) return 0;
    return close(descriptor) == 0;
#endif
}

static int profiles_equal(
    const UnityCompileProfile* left, const UnityCompileProfile* right) {
    return left->schema_version == right->schema_version &&
           left->build_platform == right->build_platform &&
           left->valid_apis == right->valid_apis &&
           left->d3d11_capabilities == right->d3d11_capabilities &&
           left->glcore_capabilities == right->glcore_capabilities &&
           strcmp(left->provenance, right->provenance) == 0 &&
           memcmp(left->fingerprint, right->fingerprint,
                  sizeof(left->fingerprint)) == 0;
}

static int digest_matches_hex(
    const uint8_t digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE],
    const char* hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE; i++) {
        if (hex[i * 2U] != digits[digest[i] >> 4U] ||
            hex[i * 2U + 1U] != digits[digest[i] & 0x0fU]) {
            return 0;
        }
    }
    return hex[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U] == '\0';
}

static int test_roundtrip_and_canonical_encoding(void) {
    UnityCompileProfile profile = make_profile();
    uint8_t expected_digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE];
    CHECK(unity_compile_profile_fingerprint(&profile, expected_digest) ==
          UNITY_COMPILE_PROFILE_OK);
    CHECK(digest_matches_hex(
        expected_digest,
        "13240033dec3c9a219cd0291abe5a31e"
        "478df02dbbbb9e0228782f60d8727786"));
    uint8_t* data = NULL;
    size_t size = 0;
    CHECK(unity_compile_profile_serialize(&profile, &data, &size) ==
          UNITY_COMPILE_PROFILE_OK);
    CHECK(data != NULL && size > 0U && data[size - 1U] == '\n');
    CHECK(memcmp(data, "DXBC_UNITY_COMPILE_PROFILE 1\n", 29U) == 0);
    CHECK(strstr((const char*)data, "build_platform=19\n") != NULL);
    CHECK(strstr((const char*)data, "valid_apis=295472\n") != NULL);
    CHECK(strstr((const char*)data,
                 "d3d11_capabilities=305419896\n") != NULL);

    UnityCompileProfile parsed;
    memset(&parsed, 0xa5, sizeof(parsed));
    CHECK(unity_compile_profile_parse(data, size, &parsed) ==
          UNITY_COMPILE_PROFILE_OK);
    memcpy(profile.fingerprint, expected_digest, sizeof(expected_digest));
    CHECK(profiles_equal(&profile, &parsed));

    uint8_t* second = NULL;
    size_t second_size = 0;
    CHECK(unity_compile_profile_serialize(&parsed, &second, &second_size) ==
          UNITY_COMPILE_PROFILE_OK);
    CHECK(second_size == size && memcmp(second, data, size) == 0);
    free(second);
    free(data);
    return 0;
}

static int expect_parse_failure_unchanged(
    const uint8_t* data, size_t size,
    UnityCompileProfileStatus expected) {
    UnityCompileProfile output = make_profile();
    output.build_platform = 777U;
    memset(output.fingerprint, 0x5a, sizeof(output.fingerprint));
    UnityCompileProfile before = output;
    CHECK(unity_compile_profile_parse(data, size, &output) == expected);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    return 0;
}

static int expect_any_parse_failure_unchanged(
    const uint8_t* data, size_t size) {
    UnityCompileProfile output = make_profile();
    output.build_platform = 777U;
    memset(output.fingerprint, 0x5a, sizeof(output.fingerprint));
    UnityCompileProfile before = output;
    CHECK(unity_compile_profile_parse(data, size, &output) !=
          UNITY_COMPILE_PROFILE_OK);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    return 0;
}

static int test_strict_rejection(void) {
    UnityCompileProfile profile = make_profile();
    uint8_t* data = NULL;
    size_t size = 0;
    CHECK(unity_compile_profile_serialize(&profile, &data, &size) ==
          UNITY_COMPILE_PROFILE_OK);

    for (size_t cut = 0; cut < size; cut++) {
        CHECK(expect_any_parse_failure_unchanged(data, cut) == 0);
    }

    CHECK(expect_parse_failure_unchanged(
              data, size - 1U, UNITY_COMPILE_PROFILE_INVALID_FORMAT) == 0);
    uint8_t* corrupt = (uint8_t*)malloc(size + 2U);
    CHECK(corrupt != NULL);
    for (size_t offset = 0; offset < size; offset++) {
        memcpy(corrupt, data, size + 1U);
        corrupt[offset] = corrupt[offset] == '0' ? '1' : '0';
        CHECK(expect_any_parse_failure_unchanged(corrupt, size) == 0);
    }
    memcpy(corrupt, data, size + 1U);
    const char* fingerprint = strstr(
        (const char*)corrupt, "fingerprint_sha256=");
    CHECK(fingerprint != NULL);
    size_t digest_offset =
        (size_t)(fingerprint - (const char*)corrupt) + 19U;
    corrupt[digest_offset] = corrupt[digest_offset] == '0' ? '1' : '0';
    CHECK(expect_parse_failure_unchanged(
              corrupt, size,
              UNITY_COMPILE_PROFILE_FINGERPRINT_MISMATCH) == 0);

    memcpy(corrupt, data, size + 1U);
    const char* build = strstr((const char*)corrupt, "build_platform=19");
    CHECK(build != NULL);
    size_t build_offset = (size_t)(build - (const char*)corrupt) + 15U;
    memmove(corrupt + build_offset + 1U, corrupt + build_offset,
            size - build_offset);
    corrupt[build_offset] = '0';
    CHECK(expect_parse_failure_unchanged(
              corrupt, size + 1U,
              UNITY_COMPILE_PROFILE_INVALID_FORMAT) == 0);

    memcpy(corrupt, data, size + 1U);
    corrupt[size] = '\n';
    CHECK(expect_parse_failure_unchanged(
              corrupt, size + 1U,
              UNITY_COMPILE_PROFILE_INVALID_FORMAT) == 0);

    memcpy(corrupt, data, size + 1U);
    const char* provenance = strstr((const char*)corrupt, "provenance=");
    CHECK(provenance != NULL);
    corrupt[(size_t)(provenance - (const char*)corrupt) + 11U] = '=';
    CHECK(expect_parse_failure_unchanged(
              corrupt, size, UNITY_COMPILE_PROFILE_INVALID_FORMAT) == 0);

    static const char range_profile[] =
        "DXBC_UNITY_COMPILE_PROFILE 1\n"
        "build_platform=19\n"
        "valid_apis=295472\n"
        "d3d11_capabilities=8589934592\n"
        "glcore_capabilities=0\n"
        "provenance=test\n"
        "fingerprint_sha256="
        "0000000000000000000000000000000000000000000000000000000000000000\n";
    CHECK(expect_parse_failure_unchanged(
              (const uint8_t*)range_profile, sizeof(range_profile) - 1U,
              UNITY_COMPILE_PROFILE_VALUE_OUT_OF_RANGE) == 0);

    free(corrupt);
    free(data);
    return 0;
}

static int test_validation_and_file_io(void) {
    UnityCompileProfile profile = make_profile();
    CHECK(unity_compile_profile_validate(&profile));
    profile.d3d11_capabilities = UINT64_C(1) << 33U;
    CHECK(!unity_compile_profile_validate(&profile));
    uint8_t* data = NULL;
    size_t size = 0;
    CHECK(unity_compile_profile_serialize(&profile, &data, &size) ==
          UNITY_COMPILE_PROFILE_VALUE_OUT_OF_RANGE);
    profile = make_profile();
    strcpy(profile.provenance, " leading-space");
    CHECK(!unity_compile_profile_validate(&profile));
    profile = make_profile();

    char path[1024];
    CHECK(make_existing_test_path(path, sizeof(path)));
    CHECK(unity_compile_profile_write(path, &profile) ==
          UNITY_COMPILE_PROFILE_OK);
    UnityCompileProfile loaded;
    unity_compile_profile_init(&loaded);
    CHECK(unity_compile_profile_load(path, &loaded) ==
          UNITY_COMPILE_PROFILE_OK);
    uint8_t expected[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE];
    CHECK(unity_compile_profile_fingerprint(&profile, expected) ==
          UNITY_COMPILE_PROFILE_OK);
    memcpy(profile.fingerprint, expected, sizeof(expected));
    CHECK(profiles_equal(&profile, &loaded));

#ifndef _WIN32
    /* A predictable legacy <destination>.tmp symlink must never be opened. */
    char victim[1024];
    CHECK(make_existing_test_path(victim, sizeof(victim)));
    FILE* victim_file = fopen(victim, "wb");
    CHECK(victim_file != NULL);
    static const char marker[] = "do-not-clobber";
    CHECK(fwrite(marker, 1, sizeof(marker), victim_file) == sizeof(marker));
    CHECK(fclose(victim_file) == 0);
    char planted[1100];
    CHECK(snprintf(planted, sizeof(planted), "%s.tmp", path) > 0);
    CHECK(symlink(victim, planted) == 0);
    CHECK(unity_compile_profile_write(path, &profile) ==
          UNITY_COMPILE_PROFILE_OK);
    victim_file = fopen(victim, "rb");
    CHECK(victim_file != NULL);
    char observed[sizeof(marker)];
    CHECK(fread(observed, 1, sizeof(observed), victim_file) ==
          sizeof(observed));
    CHECK(fclose(victim_file) == 0);
    CHECK(memcmp(observed, marker, sizeof(marker)) == 0);
    CHECK(remove(planted) == 0);
    CHECK(remove(victim) == 0);

    /* Concurrent writers may race to win, but cannot produce a torn file. */
    UnityCompileProfile alternate = make_profile();
    alternate.build_platform = 20U;
    strcpy(alternate.provenance, "test-capture:concurrent-alternate");
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        _exit(unity_compile_profile_write(path, &alternate) ==
                      UNITY_COMPILE_PROFILE_OK
                  ? 0
                  : 1);
    }
    CHECK(unity_compile_profile_write(path, &profile) ==
          UNITY_COMPILE_PROFILE_OK);
    int child_status = 0;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(unity_compile_profile_load(path, &loaded) ==
          UNITY_COMPILE_PROFILE_OK);
    CHECK((loaded.build_platform == profile.build_platform &&
           strcmp(loaded.provenance, profile.provenance) == 0) ||
          (loaded.build_platform == alternate.build_platform &&
           strcmp(loaded.provenance, alternate.provenance) == 0));
#endif

    CHECK(remove(path) == 0);
    CHECK(unity_compile_profile_load(path, &loaded) ==
          UNITY_COMPILE_PROFILE_IO_ERROR);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--write-fixture") == 0) {
        UnityCompileProfile profile = make_profile();
        UnityCompileProfileStatus status =
            unity_compile_profile_write(argv[2], &profile);
        if (status != UNITY_COMPILE_PROFILE_OK) {
            fprintf(stderr, "could not write fixture: %s\n",
                    unity_compile_profile_status_string(status));
            return 1;
        }
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--write-fixture PATH]\n", argv[0]);
        return 2;
    }
    if (test_roundtrip_and_canonical_encoding() != 0) return 1;
    if (test_strict_rejection() != 0) return 1;
    if (test_validation_and_file_io() != 0) return 1;
    printf("compile profile unit tests passed\n");
    return 0;
}
