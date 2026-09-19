// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_player_profile.h"

#include <stdlib.h>
#include <string.h>

struct UnityPlayerProfileAuthority {
    UnityPlayerProfileSummary summary;
};

static const char profile_domain[] = "DXBCSandbox.PlayerProfile.Windows64.D3D11.v1";

void unity_player_profile_free(UnityPlayerProfileAuthority *authority) { free(authority); }

bool unity_player_profile_describe(const UnityPlayerProfileAuthority *authority,
                                   UnityPlayerProfileSummary *summary) {
    if (!authority || !summary)
        return false;
    *summary = authority->summary;
    return true;
}

UnityPlayerProfileStatus unity_player_profile_capture_d3d11(
    const uint8_t *bytes, size_t size, const UnityCompileProfile *compiler_profile,
    uint8_t selected_tiers, UnityPlayerProfileAuthority **out_authority,
    UnityPlayerProfileDiagnostic *diagnostic) {
    if (out_authority)
        *out_authority = NULL;
    if (diagnostic) {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->build_settings_status = UNITY_PLAYER_BUILD_SETTINGS_INVALID_ARGUMENT;
        diagnostic->shader_caps_status = UNITY_PLAYER_SHADER_CAPS_INVALID_ARGUMENT;
    }
    if (!bytes || !size || !out_authority || !diagnostic || !selected_tiers ||
        (selected_tiers & ~7U) || !unity_compile_profile_validate(compiler_profile)) {
        return UNITY_PLAYER_PROFILE_INVALID_ARGUMENT;
    }
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, bytes, size)) {
        return UNITY_PLAYER_PROFILE_SERIALIZED_FILE_INVALID;
    }
    UnityPlayerBuildSettings settings;
    UnityPlayerShaderCaps caps;
    unity_player_build_settings_init(&settings);
    unity_player_shader_caps_init(&caps);
    diagnostic->build_settings_status = unity_player_build_settings_decode(&settings, &file);
    diagnostic->shader_caps_status = unity_player_shader_caps_decode(&caps, &file);
    UnityPlayerProfileStatus status;
    UnityPlayerProfileAuthority *result = NULL;
    if (diagnostic->build_settings_status != UNITY_PLAYER_BUILD_SETTINGS_OK) {
        status = UNITY_PLAYER_PROFILE_BUILD_SETTINGS_UNAVAILABLE;
    } else if (diagnostic->shader_caps_status != UNITY_PLAYER_SHADER_CAPS_OK) {
        status = UNITY_PLAYER_PROFILE_SHADER_CAPS_UNAVAILABLE;
    } else if (settings.target_platform != 19U || compiler_profile->build_platform != 19U) {
        status = UNITY_PLAYER_PROFILE_BUILD_TARGET_MISMATCH;
    } else if (settings.graphics_api_count != 1U || settings.graphics_apis[0] != 2) {
        status = UNITY_PLAYER_PROFILE_GRAPHICS_API_UNSUPPORTED;
    } else {
        status = UNITY_PLAYER_PROFILE_OK;
        for (uint32_t tier = 1U; tier <= 3U; ++tier) {
            if (!(selected_tiers & (1U << (tier - 1U))))
                continue;
            uint64_t capabilities = 0U;
            diagnostic->shader_caps_status = unity_player_shader_caps_get(
                &caps, UNITY_PLAYER_SHADER_PLATFORM_D3D11, tier, &capabilities);
            if (diagnostic->shader_caps_status != UNITY_PLAYER_SHADER_CAPS_OK) {
                status = UNITY_PLAYER_PROFILE_SHADER_CAPS_UNAVAILABLE;
            } else if (capabilities != compiler_profile->d3d11_capabilities) {
                status = UNITY_PLAYER_PROFILE_CAPABILITIES_MISMATCH;
            }
            if (status != UNITY_PLAYER_PROFILE_OK) {
                diagnostic->mismatched_tier = tier;
                break;
            }
        }
    }
    if (status == UNITY_PLAYER_PROFILE_OK) {
        result = calloc(1U, sizeof(*result));
        if (!result) {
            status = UNITY_PLAYER_PROFILE_ALLOCATION_FAILED;
        } else {
            UnityPlayerProfileSummary *summary = &result->summary;
            summary->selected_tiers = selected_tiers;
            common_sha256(bytes, size, summary->serialized_file_digest);
            common_sha256(bytes + file.data_offset + settings.byte_offset, settings.byte_size,
                          summary->build_settings_digest);
            common_sha256(bytes + file.data_offset + caps.byte_offset, caps.byte_size,
                          summary->shader_caps_digest);
            if (unity_compile_profile_fingerprint(compiler_profile,
                                                  summary->compiler_profile_digest) !=
                UNITY_COMPILE_PROFILE_OK) {
                status = UNITY_PLAYER_PROFILE_INVALID_ARGUMENT;
            } else {
                CommonSha256Context hash;
                common_sha256_init(&hash);
                common_sha256_update(&hash, profile_domain, sizeof(profile_domain));
                static const char layout[] = UNITY_PLAYER_BUILD_SETTINGS_LAYOUT_AUTHORITY
                    "\n" UNITY_PLAYER_SHADER_CAPS_LAYOUT_AUTHORITY;
                common_sha256_update(&hash, layout, sizeof(layout));
                common_sha256_update(&hash, summary->serialized_file_digest, 32U);
                common_sha256_update(&hash, summary->build_settings_digest, 32U);
                common_sha256_update(&hash, summary->shader_caps_digest, 32U);
                common_sha256_update(&hash, summary->compiler_profile_digest, 32U);
                common_sha256_update(&hash, &selected_tiers, sizeof(selected_tiers));
                common_sha256_final(&hash, summary->profile_digest);
            }
        }
    }
    unity_player_build_settings_dispose(&settings);
    serialized_file_close(&file);
    if (status == UNITY_PLAYER_PROFILE_OK)
        *out_authority = result;
    else
        free(result);
    return status;
}

WholeShaderEvidenceStatus
unity_player_profile_make_evidence(const UnityPlayerProfileAuthority *authority,
                                   const WholeShaderSubject *subject,
                                   WholeShaderEvidence **out_evidence) {
    if (!out_evidence)
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    *out_evidence = NULL;
    WholeShaderSubjectDescriptor descriptor;
    if (!authority || authority->summary.selected_tiers != 7U ||
        whole_shader_subject_describe(subject, &descriptor) != WHOLE_SHADER_SUBJECT_OK ||
        descriptor.serialized_target_platform != 19U || descriptor.build_platform != 19U ||
        descriptor.compiler_platform != 4 || descriptor.graphics_api != 2U ||
        strcmp(descriptor.unity_version, "2021.3.35f1") != 0 ||
        memcmp(descriptor.compiler_profile_digest, authority->summary.compiler_profile_digest,
               COMMON_SHA256_DIGEST_SIZE) != 0)
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;

    WholeShaderEvidenceComparisonItem item;
    common_sha256(profile_domain, sizeof(profile_domain), item.identity_digest);
    memcpy(item.expected_digest, descriptor.player_profile_digest, sizeof(item.expected_digest));
    memcpy(item.observed_digest, authority->summary.profile_digest, sizeof(item.observed_digest));
    WholeShaderComparisonEvidenceDescriptor evidence = {0};
    evidence.plane = WHOLE_SHADER_PLANE_PLAYER_PROFILE;
    evidence.producer = "dxbc-player-profile";
    evidence.producer_version = 1U;
    memcpy(evidence.authority_digest, authority->summary.profile_digest,
           sizeof(evidence.authority_digest));
    evidence.items = &item;
    evidence.item_count = 1U;
    return whole_shader_evidence_create_comparison(out_evidence, subject, &evidence);
}
