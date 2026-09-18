// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_PLAYER_SHADER_CAPS_H
#define UNITY_PLAYER_SHADER_CAPS_H

#include "io/serialized_file.h"

#define UNITY_PLAYER_SHADER_CAPS_CLASS_ID 30
#define UNITY_PLAYER_SHADER_CAPS_PLATFORM_COUNT 25U
#define UNITY_PLAYER_SHADER_CAPS_TIER_COUNT 3U
#define UNITY_PLAYER_SHADER_CAPS_LAYOUT_AUTHORITY \
    "unity-2021.3.35f1-player-class30-9b60e765e21d199e3eaef5540018e8da"

/* Unity 2021.3.35f1 ShaderCompilerPlatform value for Direct3D 11. */
#define UNITY_PLAYER_SHADER_PLATFORM_D3D11 4U

/*
 * Exact player platform-capability authority.
 *
 * A release GraphicsSettings object stores
 * m_ShaderDefinesPerShaderCompiler.  Unity's editor populates that field by
 * calling platform_caps_keywords::InitFromSettings followed by
 * UpdateFromTierSettings for every enabled ShaderCompilerPlatform and every
 * graphics tier before it serializes the player.  Reading this field is
 * therefore stronger than re-deriving the mask from an incomplete subset of
 * stripped PlayerSettings data.
 *
 * The decoder is pinned to SerializedFile v22, Unity 2021.3.35f1, and the
 * exact ClassID 30 type hash above.  It consumes the complete release wire
 * layout, requires every fixed_bitset<33, uint32_t> to contain exactly two
 * words, rejects duplicate/out-of-range platforms, and rejects non-zero bits
 * above bit 32.  Unsupported versions and layouts fail closed.
 *
 * Research provenance (not runtime dependencies):
 *   - Unity Editor arm64, GraphicsSettings::Transfer<StreamedBinaryWrite>
 *     calls CollectShaderDefinesForCurrentBuildTarget(..., false) at
 *     0x10066c1e0 immediately before serializing the field at 0x10066c1f8.
 *     The collector is at 0x1006523f0 and the platform_caps_keywords
 *     functions are at 0x101b7a514/0x101b7a85c/0x101b7ab8c.
 *   - The exact ClassID 30 type hash and payload are carried by Unity's
 *     serialized player data and are consumed completely below; no
 *     third-party schema package is layout authority.
 *
 * Neither Unity nor UnityShaderCompiler is used by this runtime path.
 */

typedef enum {
    UNITY_PLAYER_SHADER_CAPS_OK = 0,
    UNITY_PLAYER_SHADER_CAPS_INVALID_ARGUMENT,
    UNITY_PLAYER_SHADER_CAPS_INVALID_SERIALIZED_FILE,
    UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_FILE_VERSION,
    UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_UNITY_VERSION,
    UNITY_PLAYER_SHADER_CAPS_MISSING,
    UNITY_PLAYER_SHADER_CAPS_DUPLICATE_OBJECT,
    UNITY_PLAYER_SHADER_CAPS_OBJECT_RANGE_INVALID,
    UNITY_PLAYER_SHADER_CAPS_TYPE_INDEX_INVALID,
    UNITY_PLAYER_SHADER_CAPS_TYPE_RECORD_MISMATCH,
    UNITY_PLAYER_SHADER_CAPS_TYPE_IDENTITY_UNSUPPORTED,
    UNITY_PLAYER_SHADER_CAPS_PAYLOAD_TRUNCATED,
    UNITY_PLAYER_SHADER_CAPS_COUNT_INVALID,
    UNITY_PLAYER_SHADER_CAPS_BOOLEAN_INVALID,
    UNITY_PLAYER_SHADER_CAPS_ALIGNMENT_INVALID,
    UNITY_PLAYER_SHADER_CAPS_PLATFORM_INVALID,
    UNITY_PLAYER_SHADER_CAPS_PLATFORM_DUPLICATE,
    UNITY_PLAYER_SHADER_CAPS_BITSET_WORD_COUNT_INVALID,
    UNITY_PLAYER_SHADER_CAPS_BITSET_HIGH_BITS_INVALID,
    UNITY_PLAYER_SHADER_CAPS_OBJECT_BYTES_NOT_EXHAUSTED,
    UNITY_PLAYER_SHADER_CAPS_PLATFORM_MISSING,
    UNITY_PLAYER_SHADER_CAPS_TIER_INVALID,
} UnityPlayerShaderCapsStatus;

typedef struct {
    uint32_t shader_platform;
    uint64_t tier_capabilities[UNITY_PLAYER_SHADER_CAPS_TIER_COUNT];
    bool present;
} UnityPlayerShaderCapsEntry;

typedef struct {
    uint32_t serialized_file_version;
    uint32_t target_platform;
    int64_t path_id;
    uint64_t byte_offset;
    uint32_t byte_size;
    size_t entry_count;
    UnityPlayerShaderCapsEntry
        entries[UNITY_PLAYER_SHADER_CAPS_PLATFORM_COUNT];
    bool decoded;
} UnityPlayerShaderCaps;

void unity_player_shader_caps_init(UnityPlayerShaderCaps* caps);

/* Strong output guarantee: failure leaves an initialized destination
 * unchanged; success replaces it with the decoded fixed-size result. */
UnityPlayerShaderCapsStatus unity_player_shader_caps_decode(
    UnityPlayerShaderCaps* destination, const SerializedFile* file);

UnityPlayerShaderCapsStatus unity_player_shader_caps_decode_serialized_bytes(
    UnityPlayerShaderCaps* destination,
    const uint8_t* bytes, size_t size);

/* tier is the Unity graphics tier number 1, 2, or 3. */
UnityPlayerShaderCapsStatus unity_player_shader_caps_get(
    const UnityPlayerShaderCaps* caps, uint32_t shader_platform,
    uint32_t tier, uint64_t* out_capabilities);

const char* unity_player_shader_caps_status_name(
    UnityPlayerShaderCapsStatus status);

#endif /* UNITY_PLAYER_SHADER_CAPS_H */
