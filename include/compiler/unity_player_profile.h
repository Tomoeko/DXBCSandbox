// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_PLAYER_PROFILE_H
#define UNITY_PLAYER_PROFILE_H

#include "app/whole_shader_evidence.h"
#include "common/sha256.h"
#include "compiler/unity_compile_profile.h"
#include "io/unity_player_build_settings.h"
#include "io/unity_player_shader_caps.h"

typedef struct UnityPlayerProfileAuthority UnityPlayerProfileAuthority;

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

#endif
