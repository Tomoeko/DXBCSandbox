#include "common/oracle_pack.h"
#include "common/sha256.h"
#include "dxbc/dxbc_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define FIXTURE_DXBC_SIZE 52U
#define ORACLE_PACK_V4_HEADER_SIZE 192U
#define ORACLE_PACK_V4_ENTRY_HEADER_SIZE 136U
#define ORACLE_PACK_V4_PREPROCESS_HEADER_SIZE 112U

static const uint8_t k_compiler_fingerprint[ORACLE_PACK_DIGEST_SIZE] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
};

static const uint8_t k_environment_fingerprint[ORACLE_PACK_DIGEST_SIZE] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
};

static const OraclePackAuthorityInput k_authority = {
    k_compiler_fingerprint,
    k_environment_fingerprint,
};

static uint32_t read_le32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t read_le64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8U; ++i) {
        value |= (uint64_t)data[i] << (i * 8U);
    }
    return value;
}

static void write_le64(uint8_t* data, uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i) {
        data[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void write_le32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

/* Produces a minimal strict DXBCDocument with an executable SHDR chunk. */
static bool make_dxbc(uint8_t bytes[FIXTURE_DXBC_SIZE], bool fragment) {
    memset(bytes, 0, FIXTURE_DXBC_SIZE);
    memcpy(bytes, "DXBC", 4U);
    write_le32(bytes + 20U, 1U);
    write_le32(bytes + 24U, FIXTURE_DXBC_SIZE);
    write_le32(bytes + 28U, 1U);
    write_le32(bytes + 32U, 36U);
    memcpy(bytes + 36U, "SHDR", 4U);
    write_le32(bytes + 40U, 8U);
    /* ps_5_0 is stage 0, vs_5_0 is stage 1. */
    write_le32(bytes + 44U, fragment ? UINT32_C(0x00000050) :
                                      UINT32_C(0x00010050));
    write_le32(bytes + 48U, 2U);
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, FIXTURE_DXBC_SIZE, hash)) return false;
    memcpy(bytes + 4U, hash, sizeof(hash));
    return true;
}

static bool bytes_equal(OraclePackBytes bytes,
                        const uint8_t* expected, size_t expected_size) {
    return bytes.size == expected_size &&
           (expected_size == 0U ||
            memcmp(bytes.data, expected, expected_size) == 0);
}

static bool digest_matches_hex(const uint8_t digest[32], const char* hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32U; ++i) {
        if (hex[i * 2U] != digits[digest[i] >> 4U] ||
            hex[i * 2U + 1U] != digits[digest[i] & 15U]) {
            fprintf(stderr, "actual digest: ");
            for (size_t j = 0; j < 32U; ++j) {
                fprintf(stderr, "%02x", digest[j]);
            }
            fputc('\n', stderr);
            return false;
        }
    }
    return hex[64] == '\0';
}

/* Recomputes the format checksum after a deliberate structural mutation. */
static void rewrite_pack_checksum(uint8_t* data, size_t size) {
    static const uint8_t zeros[32] = {0};
    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(&context, data, 152U);
    common_sha256_update(&context, zeros, sizeof(zeros));
    if (size > 184U) {
        common_sha256_update(&context, data + 184U, size - 184U);
    }
    common_sha256_final(&context, data + 152U);
}

static VariantKey* make_key(int64_t path_id) {
    static const uint32_t global_keywords[] = {9U, 2U};
    static const char* const variant_keywords[] = {
        "DIRECTIONAL", "LIGHTMAP_ON"
    };
    static const char* const user_keywords[] = {"_ALPHATEST_ON"};
    static const char* const platform_keywords[] = {"SHADER_API_DESKTOP"};
    VariantKeyDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.shader_path_id = path_id;
    descriptor.compiler_platform = 4;
    descriptor.archive_platform_index = 0;
    descriptor.hardware_tier_group = 3;
    descriptor.parameter_blob_index = -1;
    descriptor.serialized_program_type = 15;
    descriptor.serialized_hardware_tier_present = true;
    descriptor.serialized_hardware_tier = 2;
    descriptor.player_metadata_present = true;
    descriptor.player_program_type = 15;
    descriptor.player_header_words[0] = UINT32_C(0x41);
    descriptor.player_header_words[1] = 8U;
    descriptor.player_header_words[2] = 4U;
    descriptor.player_header_words[3] = 7U;
    descriptor.player_source_map = UINT32_C(0x2000);
    descriptor.global_keyword_indices = global_keywords;
    descriptor.global_keyword_index_count = 2;
    descriptor.compiler.schema_version = 1;
    descriptor.compiler.language = 5;
    descriptor.compiler.shader_type = 1;
    descriptor.compiler.source_directory = "Assets";
    descriptor.compiler.source_basename = "Oracle.shader";
    descriptor.compiler.pass_name = "Forward";
    descriptor.compiler.variant_keywords = variant_keywords;
    descriptor.compiler.variant_keyword_count = 2;
    descriptor.compiler.user_keywords = user_keywords;
    descriptor.compiler.user_keyword_count = 1;
    descriptor.compiler.enabled_platform_keywords = platform_keywords;
    descriptor.compiler.enabled_platform_keyword_count = 1;
    VariantKey* key = NULL;
    if (variant_key_init(&key, &descriptor) != VARIANT_KEY_OK) return NULL;
    return key;
}

static OraclePackStatus add_fixture_entry(
    OraclePackWriter* writer, const VariantKey* key, bool second,
    bool reverse_precisions) {
    uint8_t dxbc[FIXTURE_DXBC_SIZE];
    if (!make_dxbc(dxbc, second)) return ORACLE_PACK_INVALID_DXBC;
    static const uint8_t glcore[] = {
        '#', 'i', 'f', 'd', 'e', 'f', ' ', 'V', 'E', 'R', 'T', 'E', 'X', '\n'
    };
    static const uint8_t transcript_a[] = {
        0x10, 0, 0, 0, 'c', 'o', 'm', 'p', 'i', 'l', 'e', 'S', 'n', 'i', 'p', 'p', 'e', 't'
    };
    static const uint8_t transcript_b[] = {
        0x20, 0, 0, 0, 'p', 'r', 'e', 'p', 'r', 'o', 'c', 'e', 's', 's'
    };
    static const uint8_t canonical_metadata[] = {
        1, 0, 0, 0, 2, 0, 0, 0, 0xde, 0xad, 0xbe, 0xef
    };
    static const uint8_t sampler_key[] = {
        's', 'a', 'm', 'p', 'l', 'e', 'r', ':', 's', '0'
    };
    static const uint8_t texture_key[] = {
        't', 'e', 'x', 't', 'u', 'r', 'e', ':', 't', '0'
    };
    OraclePackResourcePrecisionInput precisions[2] = {
        {{sampler_key, sizeof(sampler_key)},
         ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN},
        {{texture_key, sizeof(texture_key)},
         ORACLE_PACK_RESOURCE_PRECISION_HIGH},
    };
    if (reverse_precisions) {
        OraclePackResourcePrecisionInput temporary = precisions[0];
        precisions[0] = precisions[1];
        precisions[1] = temporary;
    }
    OraclePackEntryInput input;
    memset(&input, 0, sizeof(input));
    input.variant_key = key;
    input.stripped_dxbc = (OraclePackBytes){dxbc, sizeof(dxbc)};
    input.linked_glcore = second ?
        (OraclePackBytes){glcore, sizeof(glcore)} :
        (OraclePackBytes){NULL, 0};
    input.compile_request_transcript = second ?
        (OraclePackBytes){transcript_b, sizeof(transcript_b)} :
        (OraclePackBytes){transcript_a, sizeof(transcript_a)};
    input.normalized_metadata.canonical_bytes =
        (OraclePackBytes){canonical_metadata, sizeof(canonical_metadata)};
    input.normalized_metadata.resource_precisions = precisions;
    input.normalized_metadata.resource_precision_count = 2;
    input.authority = k_authority;
    return oracle_pack_writer_add(writer, &input);
}

static OraclePackStatus add_fixture_preprocess(
    OraclePackWriter* writer, bool second) {
    static const uint8_t request_a[] = {
        0x11, 0, 0, 0, 'p', 'r', 'e', 'p', 'a'
    };
    static const uint8_t request_b[] = {
        0x22, 0, 0, 0, 'p', 'r', 'e', 'p', 'b'
    };
    static const uint8_t result_a[] = {
        'U', 'S', 'C', 'P', 'R', 'E', 'P', '4', 1, 2, 3
    };
    static const uint8_t result_b[] = {
        'U', 'S', 'C', 'P', 'R', 'E', 'P', '4', 4, 5, 6, 7
    };
    OraclePackPreprocessInput input = {
        .request_transcript = second
            ? (OraclePackBytes){request_b, sizeof(request_b)}
            : (OraclePackBytes){request_a, sizeof(request_a)},
        .serialized_result = second
            ? (OraclePackBytes){result_b, sizeof(result_b)}
            : (OraclePackBytes){result_a, sizeof(result_a)},
        .authority = {
            k_compiler_fingerprint,
            k_environment_fingerprint,
        },
    };
    return oracle_pack_writer_add_preprocess(writer, &input);
}

static int build_fixture_pack(bool reverse_entries, bool reverse_precisions,
                              uint8_t** out_data, size_t* out_size,
                              VariantKey** out_first,
                              VariantKey** out_second) {
    VariantKey* first = make_key(INT64_C(1001));
    VariantKey* second = make_key(INT64_C(2002));
    CHECK(first != NULL && second != NULL);
    OraclePackWriter* writer = NULL;
    CHECK(oracle_pack_writer_create(&k_authority, &writer) == ORACLE_PACK_OK);
    if (reverse_entries) {
        CHECK(add_fixture_entry(writer, second, true, reverse_precisions) ==
              ORACLE_PACK_OK);
        CHECK(add_fixture_entry(writer, first, false, reverse_precisions) ==
              ORACLE_PACK_OK);
        CHECK(add_fixture_preprocess(writer, true) == ORACLE_PACK_OK);
        CHECK(add_fixture_preprocess(writer, false) == ORACLE_PACK_OK);
    } else {
        CHECK(add_fixture_entry(writer, first, false, reverse_precisions) ==
              ORACLE_PACK_OK);
        CHECK(add_fixture_preprocess(writer, false) == ORACLE_PACK_OK);
        CHECK(add_fixture_preprocess(writer, true) == ORACLE_PACK_OK);
        CHECK(add_fixture_entry(writer, second, true, reverse_precisions) ==
              ORACLE_PACK_OK);
    }
    CHECK(oracle_pack_writer_finalize(writer, out_data, out_size) ==
          ORACLE_PACK_OK);
    CHECK(oracle_pack_writer_finalize(writer, out_data, out_size) ==
          ORACLE_PACK_ALREADY_FINALIZED);
    oracle_pack_writer_free(writer);
    *out_first = first;
    *out_second = second;
    return 0;
}

static int test_round_trip_and_golden(void) {
    uint8_t* encoded = NULL;
    size_t encoded_size = 0;
    VariantKey* first = NULL;
    VariantKey* second = NULL;
    CHECK(build_fixture_pack(true, true, &encoded, &encoded_size,
                             &first, &second) == 0);
    CHECK(encoded != NULL);
    CHECK(encoded_size > ORACLE_PACK_V4_HEADER_SIZE);
    CHECK(memcmp(encoded, "DXBCORCL", 8U) == 0);
    CHECK(read_le32(encoded + 8U) == ORACLE_PACK_FORMAT_VERSION);
    CHECK(read_le32(encoded + 12U) == ORACLE_PACK_V4_HEADER_SIZE);
    CHECK(read_le64(encoded + 16U) == encoded_size);
    CHECK(read_le64(encoded + 24U) == 2U);
    CHECK(read_le64(encoded + 48U) == 2U);

    uint8_t whole_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(encoded, encoded_size, whole_digest);
    /* Frozen canonical pack-v4 bytes: field/order changes require a version. */
    CHECK(digest_matches_hex(
        whole_digest,
        "bdf3e29f108cd07e891bae9654ef33ad"
        "44071030533460844a3a1bf67bc266bd"));

    OraclePack* pack = NULL;
    CHECK(oracle_pack_open_memory(encoded, encoded_size, &pack) ==
          ORACLE_PACK_OK);
    CHECK(oracle_pack_entry_count(pack) == 2U);
    CHECK(oracle_pack_preprocess_count(pack) == 2U);
    OraclePackAuthorityView authority;
    CHECK(oracle_pack_authority(pack, &authority) == ORACLE_PACK_OK);
    CHECK(memcmp(authority.compiler_fingerprint, k_compiler_fingerprint,
                 ORACLE_PACK_DIGEST_SIZE) == 0);
    CHECK(memcmp(authority.environment_fingerprint,
                 k_environment_fingerprint,
                 ORACLE_PACK_DIGEST_SIZE) == 0);

    static const uint8_t expected_preprocess_request[] = {
        0x11, 0, 0, 0, 'p', 'r', 'e', 'p', 'a'
    };
    uint8_t preprocess_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(expected_preprocess_request,
                  sizeof(expected_preprocess_request), preprocess_digest);
    OraclePackPreprocessView preprocess_view;
    CHECK(oracle_pack_lookup_preprocess_digest(
              pack, preprocess_digest, &preprocess_view) == ORACLE_PACK_OK);
    CHECK(bytes_equal(preprocess_view.request_transcript,
                      expected_preprocess_request,
                      sizeof(expected_preprocess_request)));
    CHECK(bytes_equal(preprocess_view.serialized_result,
                      (const uint8_t*)"USCPREP4\x01\x02\x03", 11U));
    CHECK(memcmp(preprocess_view.request_digest, preprocess_digest,
                 sizeof(preprocess_digest)) == 0);
    uint8_t preprocess_result_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(preprocess_view.serialized_result.data,
                  preprocess_view.serialized_result.size,
                  preprocess_result_digest);
    CHECK(memcmp(preprocess_view.result_digest, preprocess_result_digest,
                 sizeof(preprocess_result_digest)) == 0);
    CHECK(oracle_pack_preprocess_at(pack, 2U, &preprocess_view) ==
          ORACLE_PACK_NOT_FOUND);

    OraclePackEntryView first_view;
    CHECK(oracle_pack_lookup_variant_key(pack, first, &first_view) ==
          ORACLE_PACK_OK);
    uint8_t expected_dxbc[FIXTURE_DXBC_SIZE];
    CHECK(make_dxbc(expected_dxbc, false));
    CHECK(bytes_equal(first_view.stripped_dxbc, expected_dxbc,
                      sizeof(expected_dxbc)));
    CHECK(first_view.linked_glcore.data == NULL);
    CHECK(first_view.linked_glcore.size == 0U);
    CHECK(first_view.variant_key_bytes.size > 20U);
    static const uint8_t expected_transcript[] = {
        0x10, 0, 0, 0, 'c', 'o', 'm', 'p', 'i', 'l', 'e', 'S', 'n', 'i',
        'p', 'p', 'e', 't'
    };
    CHECK(bytes_equal(first_view.compile_request_transcript,
                      expected_transcript, sizeof(expected_transcript)));
    uint8_t first_digest[VARIANT_KEY_DIGEST_SIZE];
    CHECK(variant_key_digest(first, first_digest) == VARIANT_KEY_OK);
    CHECK(memcmp(first_view.variant_key_digest, first_digest,
                 sizeof(first_digest)) == 0);
    uint8_t transcript_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(first_view.compile_request_transcript.data,
                  first_view.compile_request_transcript.size,
                  transcript_digest);
    CHECK(memcmp(first_view.compile_request_digest, transcript_digest,
                 sizeof(transcript_digest)) == 0);

    OraclePackNormalizedMetadataView metadata;
    CHECK(oracle_pack_metadata_view(first_view.normalized_metadata_bytes,
                                    &metadata) == ORACLE_PACK_OK);
    CHECK(metadata.canonical_bytes.size == 12U);
    CHECK(metadata.resource_precision_count == 2U);
    OraclePackResourcePrecisionView precision;
    CHECK(oracle_pack_metadata_precision_at(first_view.normalized_metadata_bytes,
                                            0, &precision) == ORACLE_PACK_OK);
    CHECK(bytes_equal(precision.resource_key,
                      (const uint8_t*)"sampler:s0", 10U));
    CHECK(precision.precision == ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN);
    CHECK(oracle_pack_metadata_precision_at(first_view.normalized_metadata_bytes,
                                            1, &precision) == ORACLE_PACK_OK);
    CHECK(bytes_equal(precision.resource_key,
                      (const uint8_t*)"texture:t0", 10U));
    CHECK(precision.precision == ORACLE_PACK_RESOURCE_PRECISION_HIGH);
    CHECK(oracle_pack_metadata_precision_at(first_view.normalized_metadata_bytes,
                                            2, &precision) ==
          ORACLE_PACK_NOT_FOUND);

    static const uint8_t canonical_metadata[] = {
        1, 0, 0, 0, 2, 0, 0, 0, 0xde, 0xad, 0xbe, 0xef
    };
    static const uint8_t sampler_key[] = "sampler:s0";
    static const uint8_t texture_key[] = "texture:t0";
    OraclePackResourcePrecisionInput current_precisions[2] = {
        {{texture_key, sizeof(texture_key) - 1U},
         ORACLE_PACK_RESOURCE_PRECISION_HIGH},
        {{sampler_key, sizeof(sampler_key) - 1U},
         ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN},
    };
    OraclePackNormalizedMetadataInput current_metadata = {
        .canonical_bytes = {canonical_metadata, sizeof(canonical_metadata)},
        .resource_precisions = current_precisions,
        .resource_precision_count = 2U,
    };
    bool metadata_equal = false;
    CHECK(oracle_pack_metadata_matches_input(
              first_view.normalized_metadata_bytes, &current_metadata,
              &metadata_equal) == ORACLE_PACK_OK);
    CHECK(metadata_equal);
    current_precisions[0].precision = ORACLE_PACK_RESOURCE_PRECISION_LOW;
    CHECK(oracle_pack_metadata_matches_input(
              first_view.normalized_metadata_bytes, &current_metadata,
              &metadata_equal) == ORACLE_PACK_OK);
    CHECK(!metadata_equal);

    OraclePackEntryView second_view;
    CHECK(oracle_pack_lookup_variant_key(pack, second, &second_view) ==
          ORACLE_PACK_OK);
    CHECK(make_dxbc(expected_dxbc, true));
    CHECK(bytes_equal(second_view.stripped_dxbc, expected_dxbc,
                      sizeof(expected_dxbc)));
    CHECK(bytes_equal(second_view.linked_glcore,
                      (const uint8_t*)"#ifdef VERTEX\n", 14U));

    uint8_t missing_digest[ORACLE_PACK_DIGEST_SIZE];
    memset(missing_digest, 0x5a, sizeof(missing_digest));
    CHECK(oracle_pack_lookup_digest(pack, missing_digest, &second_view) ==
          ORACLE_PACK_NOT_FOUND);
    CHECK(oracle_pack_entry_at(pack, 2, &second_view) ==
          ORACLE_PACK_NOT_FOUND);

    /* The opened pack owns an immutable copy. */
    uint8_t saved_byte = first_view.stripped_dxbc.data[0];
    memset(encoded, 0, encoded_size);
    CHECK(first_view.stripped_dxbc.data[0] == saved_byte);

    oracle_pack_free(pack);
    oracle_pack_bytes_free(encoded);
    variant_key_free(second);
    variant_key_free(first);
    return 0;
}

static int test_deterministic_sorting(void) {
    uint8_t* forward = NULL;
    uint8_t* reverse = NULL;
    size_t forward_size = 0;
    size_t reverse_size = 0;
    VariantKey* forward_first = NULL;
    VariantKey* forward_second = NULL;
    VariantKey* reverse_first = NULL;
    VariantKey* reverse_second = NULL;
    CHECK(build_fixture_pack(false, false, &forward, &forward_size,
                             &forward_first, &forward_second) == 0);
    CHECK(build_fixture_pack(true, true, &reverse, &reverse_size,
                             &reverse_first, &reverse_second) == 0);
    CHECK(forward_size == reverse_size);
    CHECK(memcmp(forward, reverse, forward_size) == 0);
    oracle_pack_bytes_free(reverse);
    oracle_pack_bytes_free(forward);
    variant_key_free(reverse_second);
    variant_key_free(reverse_first);
    variant_key_free(forward_second);
    variant_key_free(forward_first);
    return 0;
}

static int test_duplicate_and_invalid_inputs(void) {
    VariantKey* key = make_key(INT64_C(99));
    CHECK(key != NULL);
    OraclePackWriter* writer = NULL;
    CHECK(oracle_pack_writer_create(&k_authority, &writer) == ORACLE_PACK_OK);
    CHECK(add_fixture_entry(writer, key, false, false) == ORACLE_PACK_OK);
    CHECK(add_fixture_entry(writer, key, true, true) ==
          ORACLE_PACK_DUPLICATE_KEY);
    oracle_pack_writer_free(writer);

    static const uint8_t transcript[] = {1U};
    OraclePackEntryInput input;
    memset(&input, 0, sizeof(input));
    uint8_t valid_dxbc[FIXTURE_DXBC_SIZE];
    CHECK(make_dxbc(valid_dxbc, false));
    input.variant_key = key;
    input.stripped_dxbc =
        (OraclePackBytes){valid_dxbc, sizeof(valid_dxbc)};
    input.compile_request_transcript =
        (OraclePackBytes){transcript, sizeof(transcript)};
    input.authority = k_authority;

    CHECK(oracle_pack_writer_create(&k_authority, &writer) == ORACLE_PACK_OK);
    static const uint8_t token_only[] = {'S', 'H', 'E', 'X'};
    input.stripped_dxbc =
        (OraclePackBytes){token_only, sizeof(token_only)};
    CHECK(oracle_pack_writer_add(writer, &input) == ORACLE_PACK_INVALID_DXBC);
    input.stripped_dxbc =
        (OraclePackBytes){valid_dxbc, sizeof(valid_dxbc)};
    valid_dxbc[4] ^= 1U;
    CHECK(oracle_pack_writer_add(writer, &input) == ORACLE_PACK_INVALID_DXBC);
    CHECK(make_dxbc(valid_dxbc, false));
    input.linked_glcore = (OraclePackBytes){transcript, 0U};
    CHECK(oracle_pack_writer_add(writer, &input) ==
          ORACLE_PACK_INVALID_ARGUMENT);
    input.linked_glcore = (OraclePackBytes){NULL, 0U};

    static const uint8_t duplicate_key[] = {'t', '0'};
    OraclePackResourcePrecisionInput duplicate_precisions[2] = {
        {{duplicate_key, sizeof(duplicate_key)},
         ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN},
        {{duplicate_key, sizeof(duplicate_key)},
         ORACLE_PACK_RESOURCE_PRECISION_HIGH},
    };
    input.normalized_metadata.resource_precisions = duplicate_precisions;
    input.normalized_metadata.resource_precision_count = 2;
    CHECK(oracle_pack_writer_add(writer, &input) ==
          ORACLE_PACK_DUPLICATE_KEY);
    duplicate_precisions[1].resource_key =
        (OraclePackBytes){(const uint8_t*)"t1", 2U};
    duplicate_precisions[1].precision =
        (OraclePackResourcePrecision)99;
    CHECK(oracle_pack_writer_add(writer, &input) ==
          ORACLE_PACK_INVALID_METADATA);
    duplicate_precisions[1].precision =
        ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN;
    CHECK(oracle_pack_writer_add(writer, &input) == ORACLE_PACK_OK);

    uint8_t other_environment[ORACLE_PACK_DIGEST_SIZE];
    memcpy(other_environment, k_environment_fingerprint,
           sizeof(other_environment));
    other_environment[0] ^= 1U;
    input.authority.environment_fingerprint = other_environment;
    VariantKey* mismatched_key = make_key(INT64_C(100));
    CHECK(mismatched_key != NULL);
    input.variant_key = mismatched_key;
    CHECK(oracle_pack_writer_add(writer, &input) ==
          ORACLE_PACK_AUTHORITY_MISMATCH);
    variant_key_free(mismatched_key);
    oracle_pack_writer_free(writer);

    CHECK(oracle_pack_writer_create(&k_authority, &writer) == ORACLE_PACK_OK);
    CHECK(add_fixture_preprocess(writer, false) == ORACLE_PACK_OK);
    /* Exact duplicate capture is deterministic and idempotent. */
    CHECK(add_fixture_preprocess(writer, false) == ORACLE_PACK_OK);
    static const uint8_t preprocess_request[] = {
        0x11, 0, 0, 0, 'p', 'r', 'e', 'p', 'a'
    };
    static const uint8_t conflicting_result[] = {9U};
    OraclePackPreprocessInput preprocess_input = {
        .request_transcript = {
            preprocess_request, sizeof(preprocess_request)},
        .serialized_result = {
            conflicting_result, sizeof(conflicting_result)},
        .authority = {
            k_compiler_fingerprint,
            k_environment_fingerprint,
        },
    };
    CHECK(oracle_pack_writer_add_preprocess(writer, &preprocess_input) ==
          ORACLE_PACK_DUPLICATE_KEY);
    uint8_t other_compiler[ORACLE_PACK_DIGEST_SIZE];
    memcpy(other_compiler, k_compiler_fingerprint, sizeof(other_compiler));
    other_compiler[0] ^= 1U;
    preprocess_input.authority.compiler_fingerprint = other_compiler;
    CHECK(oracle_pack_writer_add_preprocess(writer, &preprocess_input) ==
          ORACLE_PACK_AUTHORITY_MISMATCH);
    preprocess_input.authority.compiler_fingerprint =
        k_compiler_fingerprint;
    preprocess_input.request_transcript =
        (OraclePackBytes){preprocess_request, 0U};
    CHECK(oracle_pack_writer_add_preprocess(writer, &preprocess_input) ==
          ORACLE_PACK_INVALID_ARGUMENT);
    preprocess_input.request_transcript = (OraclePackBytes){NULL, 0U};
    CHECK(oracle_pack_writer_add_preprocess(writer, &preprocess_input) ==
          ORACLE_PACK_INVALID_VALUE);
    oracle_pack_writer_free(writer);

    uint8_t zeros[ORACLE_PACK_DIGEST_SIZE] = {0};
    OraclePackAuthorityInput invalid_authority = {
        zeros, k_environment_fingerprint};
    writer = (OraclePackWriter*)(uintptr_t)1U;
    CHECK(oracle_pack_writer_create(&invalid_authority, &writer) ==
          ORACLE_PACK_INVALID_VALUE);
    CHECK(writer == NULL);
    CHECK(oracle_pack_writer_create(NULL, &writer) ==
          ORACLE_PACK_INVALID_ARGUMENT);
    CHECK(writer == NULL);
    variant_key_free(key);
    return 0;
}

static int test_corruption_and_truncation(void) {
    uint8_t* encoded = NULL;
    size_t encoded_size = 0;
    VariantKey* first = NULL;
    VariantKey* second = NULL;
    CHECK(build_fixture_pack(false, false, &encoded, &encoded_size,
                             &first, &second) == 0);

    OraclePack* pack = NULL;
    for (size_t truncated = 0; truncated < encoded_size; ++truncated) {
        OraclePackStatus status = oracle_pack_open_memory(
            encoded, truncated, &pack);
        CHECK(status == ORACLE_PACK_TRUNCATED);
        CHECK(pack == NULL);
    }

    uint8_t* mutated = (uint8_t*)malloc(encoded_size);
    CHECK(mutated != NULL);
    memcpy(mutated, encoded, encoded_size);
    mutated[encoded_size - 1U] ^= 0x80U;
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    /* Header authority is mandatory even when the outer checksum is valid. */
    memcpy(mutated, encoded, encoded_size);
    memset(mutated + 88U, 0, ORACLE_PACK_DIGEST_SIZE);
    rewrite_pack_checksum(mutated, encoded_size);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    memcpy(mutated, encoded, encoded_size);
    mutated[0] = 'Q';
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_BAD_MAGIC);

    memcpy(mutated, encoded, encoded_size);
    write_le32(mutated + 8U, 3U);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_UNSUPPORTED_VERSION);

    /* A complete v3-sized prefix is still an explicitly unsupported format,
     * not a candidate for v4 layout guessing or migration. */
    uint8_t legacy_v3_header[128] = {0};
    memcpy(legacy_v3_header, "DXBCORCL", 8U);
    write_le32(legacy_v3_header + 8U, 3U);
    write_le32(legacy_v3_header + 12U, 128U);
    CHECK(oracle_pack_open_memory(legacy_v3_header,
                                  sizeof(legacy_v3_header), &pack) ==
          ORACLE_PACK_UNSUPPORTED_VERSION);

    /* Directory offset is canonical and bounds-checked even with a valid hash. */
    memcpy(mutated, encoded, encoded_size);
    write_le64(mutated + 32U, ORACLE_PACK_V4_HEADER_SIZE + 1U);
    rewrite_pack_checksum(mutated, encoded_size);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    /* Never subtract or form a pointer from an unbounded directory offset. */
    memcpy(mutated, encoded, encoded_size);
    write_le64(mutated + ORACLE_PACK_V4_HEADER_SIZE + 32U, UINT64_MAX);
    rewrite_pack_checksum(mutated, encoded_size);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    /* A forged payload cannot survive the per-entry VariantKey digest check. */
    memcpy(mutated, encoded, encoded_size);
    size_t first_entry_offset = (size_t)read_le64(
        mutated + ORACLE_PACK_V4_HEADER_SIZE + 32U);
    size_t variant_offset =
        first_entry_offset + ORACLE_PACK_V4_ENTRY_HEADER_SIZE;
    mutated[variant_offset + 20U] ^= 1U;
    rewrite_pack_checksum(mutated, encoded_size);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    /* Outer integrity cannot hide a DXBCDocument checksum violation. */
    memcpy(mutated, encoded, encoded_size);
    first_entry_offset = (size_t)read_le64(
        mutated + ORACLE_PACK_V4_HEADER_SIZE + 32U);
    size_t first_variant_size =
        (size_t)read_le64(mutated + first_entry_offset + 32U);
    size_t first_dxbc_offset = first_entry_offset +
                               ORACLE_PACK_V4_ENTRY_HEADER_SIZE +
                               first_variant_size;
    mutated[first_dxbc_offset + 44U] ^= 1U;
    rewrite_pack_checksum(mutated, encoded_size);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    /* The transcript has its own digest, independent of the pack checksum. */
    memcpy(mutated, encoded, encoded_size);
    first_entry_offset = (size_t)read_le64(
        mutated + ORACLE_PACK_V4_HEADER_SIZE + 32U);
    size_t first_dxbc_size =
        (size_t)read_le64(mutated + first_entry_offset + 40U);
    size_t first_glcore_size =
        (size_t)read_le64(mutated + first_entry_offset + 48U);
    size_t transcript_offset = first_entry_offset +
        ORACLE_PACK_V4_ENTRY_HEADER_SIZE +
        first_variant_size + first_dxbc_size + first_glcore_size;
    mutated[transcript_offset] ^= 1U;
    rewrite_pack_checksum(mutated, encoded_size);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    /* Preprocess payloads retain independent request/result digests. */
    memcpy(mutated, encoded, encoded_size);
    size_t preprocess_directory_offset =
        (size_t)read_le64(mutated + 56U);
    size_t first_preprocess_offset = (size_t)read_le64(
        mutated + preprocess_directory_offset + 32U);
    size_t preprocess_transcript_size = (size_t)read_le64(
        mutated + first_preprocess_offset + 32U);
    size_t preprocess_result_offset = first_preprocess_offset +
        ORACLE_PACK_V4_PREPROCESS_HEADER_SIZE +
        preprocess_transcript_size;
    mutated[preprocess_result_offset] ^= 1U;
    rewrite_pack_checksum(mutated, encoded_size);
    CHECK(oracle_pack_open_memory(mutated, encoded_size, &pack) ==
          ORACLE_PACK_CORRUPT);

    free(mutated);
    oracle_pack_bytes_free(encoded);
    variant_key_free(second);
    variant_key_free(first);
    return 0;
}

static int test_empty_pack_and_api_contract(void) {
    OraclePackWriter* writer = NULL;
    CHECK(oracle_pack_writer_create(&k_authority, &writer) == ORACLE_PACK_OK);
    uint8_t* encoded = NULL;
    size_t encoded_size = 0;
    CHECK(oracle_pack_writer_finalize(writer, &encoded, &encoded_size) ==
          ORACLE_PACK_OK);
    CHECK(encoded_size == ORACLE_PACK_V4_HEADER_SIZE);
    OraclePack* pack = NULL;
    CHECK(oracle_pack_open_memory(encoded, encoded_size, &pack) ==
          ORACLE_PACK_OK);
    CHECK(oracle_pack_entry_count(pack) == 0U);
    CHECK(oracle_pack_preprocess_count(pack) == 0U);
    OraclePackAuthorityView authority;
    CHECK(oracle_pack_authority(pack, &authority) == ORACLE_PACK_OK);
    CHECK(memcmp(authority.compiler_fingerprint, k_compiler_fingerprint,
                 ORACLE_PACK_DIGEST_SIZE) == 0);
    CHECK(memcmp(authority.environment_fingerprint,
                 k_environment_fingerprint,
                 ORACLE_PACK_DIGEST_SIZE) == 0);
    CHECK(strcmp(oracle_pack_status_string(ORACLE_PACK_CORRUPT),
                 "corrupt") == 0);
    CHECK(strcmp(oracle_pack_status_string((OraclePackStatus)999),
                 "unknown") == 0);
    oracle_pack_free(pack);
    oracle_pack_bytes_free(encoded);
    oracle_pack_writer_free(writer);
    return 0;
}

int main(void) {
    int result = 0;
    result |= test_round_trip_and_golden();
    result |= test_deterministic_sorting();
    result |= test_duplicate_and_invalid_inputs();
    result |= test_corruption_and_truncation();
    result |= test_empty_pack_and_api_contract();
    if (result == 0) puts("All oracle-pack unit tests passed.");
    return result;
}
