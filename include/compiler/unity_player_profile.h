// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_PLAYER_PROFILE_H
#define UNITY_PLAYER_PROFILE_H

#include "app/whole_shader_evidence.h"
#include "app/shader_runtime_capture.h"
#include "common/sha256.h"
#include "compiler/unity_compile_profile.h"
#include "io/unity_player_build_settings.h"
#include "io/unity_player_shader_caps.h"

typedef struct UnityPlayerProfileAuthority UnityPlayerProfileAuthority;
typedef struct UnityPlayerPackageAuthority UnityPlayerPackageAuthority;

typedef enum {
    UNITY_PLAYER_PROFILE_OK = 0,
    UNITY_PLAYER_PROFILE_INVALID_ARGUMENT,
    UNITY_PLAYER_PROFILE_SERIALIZED_FILE_INVALID,
    UNITY_PLAYER_PROFILE_BUILD_SETTINGS_UNAVAILABLE,
    UNITY_PLAYER_PROFILE_SHADER_CAPS_UNAVAILABLE,
    UNITY_PLAYER_PROFILE_BUILD_TARGET_MISMATCH,
    UNITY_PLAYER_PROFILE_GRAPHICS_API_UNSUPPORTED,
    UNITY_PLAYER_PROFILE_CAPABILITIES_MISMATCH,
    UNITY_PLAYER_PROFILE_ALLOCATION_FAILED
} UnityPlayerProfileStatus;

typedef struct {
    UnityPlayerBuildSettingsStatus build_settings_status;
    UnityPlayerShaderCapsStatus shader_caps_status;
    uint32_t mismatched_tier;
} UnityPlayerProfileDiagnostic;

typedef struct {
    uint8_t serialized_file_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t build_settings_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t shader_caps_digest[COMMON_SHA256_DIGEST_SIZE];
    /* unity_compile_profile_fingerprint(), not a hash of profile-file spelling. */
    uint8_t compiler_profile_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t profile_digest[COMMON_SHA256_DIGEST_SIZE];
    /* Bits 0..2 select Unity tiers 1..3. At least one must be selected. */
    uint8_t selected_tiers;
} UnityPlayerProfileSummary;

/* Captures exact Class141/Class30 metadata from one complete SerializedFile.
 * Currently admits Windows64 with D3D11 as its sole configured graphics API.
 * Every requested tier's serialized capabilities must match the separately
 * captured compiler profile; validAPIs is never inferred from player data.
 * The authority owns its fingerprints and outlives the input bytes.
 * This proves pinned build metadata, not execution of a particular player or
 * runtime tier/variant selection. Those need the separate runtime planes. */
UnityPlayerProfileStatus unity_player_profile_capture_d3d11(
    const uint8_t *serialized_bytes, size_t size, const UnityCompileProfile *compiler_profile,
    uint8_t selected_tiers, UnityPlayerProfileAuthority **out_authority,
    UnityPlayerProfileDiagnostic *diagnostic);
void unity_player_profile_free(UnityPlayerProfileAuthority *authority);
bool unity_player_profile_describe(const UnityPlayerProfileAuthority *authority,
                                   UnityPlayerProfileSummary *summary);

/* Compare the actual capture digest against subject.player_profile_digest.
 * A different valid capture produces FAIL evidence. Inconsistent subject
 * coordinates or compiler-profile fingerprints are invalid arguments.
 * Whole-profile evidence requires all three tiers. Narrow captures cannot
 * establish that plane. No caller-supplied report or Boolean can assert success. */
WholeShaderEvidenceStatus
unity_player_profile_make_evidence(const UnityPlayerProfileAuthority *authority,
                                   const WholeShaderSubject *subject,
                                   WholeShaderEvidence **out_evidence);

typedef enum {
    UNITY_PLAYER_PACKAGE_OK = 0,
    UNITY_PLAYER_PACKAGE_INVALID_ARGUMENT,
    UNITY_PLAYER_PACKAGE_CAPTURE_FAILED,
    UNITY_PLAYER_PACKAGE_METADATA_ABSENT,
    UNITY_PLAYER_PACKAGE_PROFILE_REJECTED,
    UNITY_PLAYER_PACKAGE_INPUT_CHANGED,
    UNITY_PLAYER_PACKAGE_ALLOCATION_FAILED
} UnityPlayerPackageStatus;

typedef struct {
    ShaderRuntimeCaptureStatus capture_status;
    ShaderRuntimeCaptureDiagnostic capture;
    UnityPlayerProfileStatus profile_status;
    UnityPlayerProfileDiagnostic profile;
} UnityPlayerPackageDiagnostic;

typedef struct {
    ShaderRuntimeImageSummary image;
    UnityPlayerProfileSummary player;
    /* Binds image, the exact relative metadata member and decoded profile. */
    uint8_t package_digest[32];
} UnityPlayerPackageSummary;

/* Decode the selected Class141/Class30 file from the captured package's own
 * immutable bytes, then revalidate the whole package before returning either
 * authority. metadata_path is an exact portable relative path, not a second
 * independent host input. A failed capture never publishes a profile owner.
 * This binds build metadata to file membership; it does not prove that a
 * process loaded these files or selected this metadata. Native execution must
 * independently establish that association and the environment before any
 * runtime plane can pass. The package digest is not runtime_environment_digest. */
UnityPlayerPackageStatus unity_player_package_capture_d3d11(
    const char *root, const char *metadata_path, const ShaderRuntimeCaptureLimits *limits,
    const UnityCompileProfile *compiler_profile, uint8_t selected_tiers,
    UnityPlayerPackageAuthority **output, UnityPlayerPackageDiagnostic *diagnostic);
bool unity_player_package_describe(const UnityPlayerPackageAuthority *authority,
                                   UnityPlayerPackageSummary *summary);
/* Borrowed opaque profile; valid only until the package authority is freed. */
const UnityPlayerProfileAuthority *
unity_player_package_profile(const UnityPlayerPackageAuthority *authority);
void unity_player_package_free(UnityPlayerPackageAuthority *authority);
const char *unity_player_package_status_name(UnityPlayerPackageStatus status);

#endif
