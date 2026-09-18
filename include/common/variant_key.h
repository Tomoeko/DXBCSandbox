// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_VARIANT_KEY_H
#define COMMON_VARIANT_KEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VARIANT_KEY_FORMAT_VERSION 4U
#define VARIANT_KEY_DIGEST_SIZE 32U
#define VARIANT_KEY_MAX_KEYWORD_INDICES UINT32_C(1048576)
#define VARIANT_KEY_MAX_COMPILER_KEYWORDS UINT32_C(1048576)
#define VARIANT_KEY_MAX_STRING_BYTES UINT32_C(16777216)

/*
 * A fingerprint is presence-tagged.  An absent fingerprint is different from
 * a known SHA-256 value containing 32 zero bytes.
 */
typedef struct {
    bool present;
    uint8_t bytes[VARIANT_KEY_DIGEST_SIZE];
} VariantKeyFingerprint;

/*
 * Values that authorize the exact compiler request.  The serialized values
 * in VariantKeyDescriptor remain separate because Unity's preprocessing may
 * resolve them into different wire-request values.
 *
 * request_input_fingerprint covers canonical ordered inputs that are not
 * intrinsic shader-variant metadata (source, keyword spellings, defines,
 * entry point, and other length-delimited request strings).
 *
 * Platform keyword arrays are kept separately from serialized keyword indices
 * and from one another.  Ordering and enabled/disabled state are authoritative
 * compiler inputs and must not be reconstructed from a desktop/API heuristic.
 */
typedef struct {
    uint32_t schema_version;
    uint32_t build_platform;
    uint32_t compiler_flags;
    int32_t language;
    int32_t shader_type;
    uint64_t request_requirements;
    uint32_t request_program_mask;
    int32_t program_start;
    int32_t render_state_length;
    uint32_t valid_apis;
    bool caching_preprocessor;
    bool preprocess_only;
    bool strip_line_directives;
    const char* source_directory;
    const char* source_basename;
    const char* pass_name;
    const char* const* variant_keywords;
    size_t variant_keyword_count;
    const char* const* user_keywords;
    size_t user_keyword_count;
    const char* const* enabled_platform_keywords;
    size_t enabled_platform_keyword_count;
    const char* const* disabled_keywords;
    size_t disabled_keyword_count;
    VariantKeyFingerprint compiler_fingerprint;
    VariantKeyFingerprint environment_fingerprint;
    VariantKeyFingerprint source_fingerprint;
    VariantKeyFingerprint request_input_fingerprint;
} VariantCompilerAuthority;

/*
 * Initialization copies both keyword arrays.  Their order and scope are
 * significant: neither array is sorted, deduplicated, or merged.
 */
typedef struct {
    int64_t shader_path_id;
    uint32_t subshader_index;
    uint32_t pass_index;
    uint32_t stage;
    int32_t compiler_platform;       /* serialized platform value */
    uint32_t archive_platform_index; /* index in root archive planes */
    uint32_t hardware_tier_group;    /* m_PlayerSubPrograms outer bucket */
    uint32_t subprogram_index;       /* index within that bucket */
    int32_t parameter_blob_index;
    /* SerializedPlayerSubProgram TypeTree authority. */
    int32_t serialized_program_type;
    bool serialized_hardware_tier_present;
    int32_t serialized_hardware_tier;
    uint64_t serialized_requirements;
    uint32_t serialized_program_mask;
    /* LoadVariantFromData player-blob wire authority. */
    bool player_metadata_present;
    int32_t player_program_type;
    uint32_t player_header_words[4];
    uint32_t player_source_map;
    /* Distinguishes legacy m_KeywordIndices from split global/local arrays. */
    bool keyword_scopes_are_explicit;
    const uint32_t* global_keyword_indices;
    size_t global_keyword_index_count;
    const uint32_t* local_keyword_indices;
    size_t local_keyword_index_count;
    VariantCompilerAuthority compiler;
} VariantKeyDescriptor;

typedef enum {
    VARIANT_KEY_OK = 0,
    VARIANT_KEY_INVALID_ARGUMENT,
    VARIANT_KEY_INVALID_COUNT,
    VARIANT_KEY_INCONSISTENT_ARRAY,
    VARIANT_KEY_INVALID_VALUE,
    VARIANT_KEY_ALLOCATION_FAILED,
    VARIANT_KEY_SIZE_OVERFLOW,
} VariantKeyStatus;

/* Opaque after initialization: callers cannot mutate identity fields. */
typedef struct VariantKey VariantKey;

VariantKeyStatus variant_key_init(
    VariantKey** out_key, const VariantKeyDescriptor* descriptor);
VariantKeyStatus variant_key_copy(
    VariantKey** out_key, const VariantKey* source);
void variant_key_free(VariantKey* key);

/*
 * Returns a scalar copy plus borrowed const views of owned strings/arrays.
 * The views remain valid until key is freed and must not be freed by callers.
 */
VariantKeyStatus variant_key_describe(
    const VariantKey* key, VariantKeyDescriptor* out_descriptor);

bool variant_key_equal(const VariantKey* left, const VariantKey* right);

/*
 * Stable little-endian encoding with an eight-byte magic, u32 schema version,
 * and u64 total byte length.  The returned bytes use malloc-compatible owned
 * storage and must be released with variant_key_serialized_free().
 */
VariantKeyStatus variant_key_serialize(
    const VariantKey* key, uint8_t** out_data, size_t* out_size);
void variant_key_serialized_free(uint8_t* data);

/* SHA-256 of exactly the bytes produced by variant_key_serialize(). */
VariantKeyStatus variant_key_digest(
    const VariantKey* key, uint8_t digest[VARIANT_KEY_DIGEST_SIZE]);

const char* variant_key_status_string(VariantKeyStatus status);

#endif /* COMMON_VARIANT_KEY_H */
