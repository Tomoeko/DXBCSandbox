// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPILE_PROFILE_H
#define UNITY_COMPILE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNITY_COMPILE_PROFILE_SCHEMA_VERSION 1U
#define UNITY_COMPILE_PROFILE_CAPABILITY_COUNT 33U
#define UNITY_COMPILE_PROFILE_PROVENANCE_MAX 255U
#define UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE 32U

/*
 * Portable authority for the project/settings values that Unity supplies
 * outside an individual serialized shader.  The fingerprint integrity-checks
 * the authority fields; it neither authenticates the file nor asserts that
 * the provenance claim itself is true.  Only profiles captured from a known
 * Unity invocation may be described as verified.
 */
typedef struct {
    uint32_t schema_version;
    uint32_t build_platform;
    uint32_t valid_apis;
    uint64_t d3d11_capabilities;
    uint64_t glcore_capabilities;
    char provenance[UNITY_COMPILE_PROFILE_PROVENANCE_MAX + 1U];
    uint8_t fingerprint[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE];
} UnityCompileProfile;

typedef enum {
    UNITY_COMPILE_PROFILE_OK = 0,
    UNITY_COMPILE_PROFILE_INVALID_ARGUMENT,
    UNITY_COMPILE_PROFILE_IO_ERROR,
    UNITY_COMPILE_PROFILE_OUT_OF_MEMORY,
    UNITY_COMPILE_PROFILE_TOO_LARGE,
    UNITY_COMPILE_PROFILE_INVALID_FORMAT,
    UNITY_COMPILE_PROFILE_UNSUPPORTED_VERSION,
    UNITY_COMPILE_PROFILE_VALUE_OUT_OF_RANGE,
    UNITY_COMPILE_PROFILE_NONCANONICAL,
    UNITY_COMPILE_PROFILE_FINGERPRINT_MISMATCH,
} UnityCompileProfileStatus;

void unity_compile_profile_init(UnityCompileProfile* profile);

bool unity_compile_profile_validate(const UnityCompileProfile* profile);

UnityCompileProfileStatus unity_compile_profile_fingerprint(
    const UnityCompileProfile* profile,
    uint8_t fingerprint[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE]);

/* The returned canonical UTF-8/ASCII text is owned by the caller. */
UnityCompileProfileStatus unity_compile_profile_serialize(
    const UnityCompileProfile* profile, uint8_t** data, size_t* size);

/* Parsing and loading preserve the destination on every failure. */
UnityCompileProfileStatus unity_compile_profile_parse(
    const uint8_t* data, size_t size, UnityCompileProfile* profile);
UnityCompileProfileStatus unity_compile_profile_load(
    const char* path, UnityCompileProfile* profile);

/*
 * Writes the canonical representation, including its computed fingerprint,
 * through an exclusive same-directory temporary followed by atomic replace.
 */
UnityCompileProfileStatus unity_compile_profile_write(
    const char* path, const UnityCompileProfile* profile);

const char* unity_compile_profile_status_string(
    UnityCompileProfileStatus status);

#endif /* UNITY_COMPILE_PROFILE_H */
