#include "common/sha256.h"
#include "common/variant_key.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint32_t read_le32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t read_le64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        value |= (uint64_t)data[i] << (i * 8U);
    }
    return value;
}

static bool digest_matches_hex(const uint8_t digest[32], const char* hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        if (hex[i * 2U] != digits[digest[i] >> 4U] ||
            hex[i * 2U + 1U] != digits[digest[i] & 15U]) {
            fprintf(stderr, "actual digest: ");
            for (size_t j = 0; j < 32; ++j) {
                fprintf(stderr, "%02x", digest[j]);
            }
            fputc('\n', stderr);
            return false;
        }
    }
    return hex[64] == '\0';
}

static int test_sha256_vectors(void) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(NULL, 0, digest);
    CHECK(digest_matches_hex(
        digest,
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855"));
    common_sha256("abc", 3, digest);
    CHECK(digest_matches_hex(
        digest,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad"));
    return 0;
}

static VariantKeyDescriptor make_descriptor(
    const uint32_t* global_indices, size_t global_count,
    const uint32_t* local_indices, size_t local_count) {
    static const char* const enabled_platform_keywords[] = {
        "SHADER_API_DESKTOP",
        "UNITY_COLORSPACE_GAMMA",
    };
    static const char* const disabled_keywords[] = {
        "STEREO_INSTANCING_ON",
        "UNITY_SINGLE_PASS_STEREO",
    };
    static const char* const variant_keywords[] = {
        "FEATURE_A",
        "FEATURE_B",
    };
    static const char* const user_keywords[] = {
        "USER_A",
    };
    VariantKeyDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.shader_path_id = INT64_C(-1234567890123);
    descriptor.subshader_index = 2;
    descriptor.pass_index = 4;
    descriptor.stage = 1;
    descriptor.compiler_platform = 4;
    descriptor.archive_platform_index = 0;
    descriptor.hardware_tier_group = 3;
    descriptor.subprogram_index = 17;
    descriptor.parameter_blob_index = 99;
    descriptor.serialized_program_type = 15;
    descriptor.serialized_hardware_tier_present = true;
    descriptor.serialized_hardware_tier = 2;
    descriptor.serialized_requirements = UINT64_C(0x0123456789abcdef);
    descriptor.serialized_program_mask = UINT32_C(0x36);
    descriptor.player_metadata_present = true;
    descriptor.player_program_type = 15;
    descriptor.player_header_words[0] = UINT32_C(0x41);
    descriptor.player_header_words[1] = 8U;
    descriptor.player_header_words[2] = 4U;
    descriptor.player_header_words[3] = 7U;
    descriptor.player_source_map = UINT32_C(0x12345678);
    descriptor.keyword_scopes_are_explicit = true;
    descriptor.global_keyword_indices = global_indices;
    descriptor.global_keyword_index_count = global_count;
    descriptor.local_keyword_indices = local_indices;
    descriptor.local_keyword_index_count = local_count;

    descriptor.compiler.schema_version = 3;
    descriptor.compiler.build_platform = UINT32_C(0x1234);
    descriptor.compiler.compiler_flags = UINT32_C(0x9000);
    descriptor.compiler.language = 5;
    descriptor.compiler.shader_type = 1;
    descriptor.compiler.request_requirements =
        UINT64_C(0xfedcba9876543210);
    descriptor.compiler.request_program_mask = 6;
    descriptor.compiler.program_start = 0;
    descriptor.compiler.render_state_length = 0;
    descriptor.compiler.valid_apis = UINT32_C(0x8008);
    descriptor.compiler.caching_preprocessor = true;
    descriptor.compiler.preprocess_only = false;
    descriptor.compiler.strip_line_directives = true;
    descriptor.compiler.source_directory = "Assets/Shaders";
    descriptor.compiler.source_basename = "Opaque.shader";
    descriptor.compiler.pass_name = "ForwardLit";
    descriptor.compiler.variant_keywords = variant_keywords;
    descriptor.compiler.variant_keyword_count =
        sizeof(variant_keywords) / sizeof(variant_keywords[0]);
    descriptor.compiler.user_keywords = user_keywords;
    descriptor.compiler.user_keyword_count =
        sizeof(user_keywords) / sizeof(user_keywords[0]);
    descriptor.compiler.enabled_platform_keywords =
        enabled_platform_keywords;
    descriptor.compiler.enabled_platform_keyword_count =
        sizeof(enabled_platform_keywords) /
        sizeof(enabled_platform_keywords[0]);
    descriptor.compiler.disabled_keywords = disabled_keywords;
    descriptor.compiler.disabled_keyword_count =
        sizeof(disabled_keywords) / sizeof(disabled_keywords[0]);
    descriptor.compiler.compiler_fingerprint.present = true;
    descriptor.compiler.source_fingerprint.present = true;
    descriptor.compiler.request_input_fingerprint.present = true;
    for (size_t i = 0; i < VARIANT_KEY_DIGEST_SIZE; i++) {
        descriptor.compiler.compiler_fingerprint.bytes[i] = (uint8_t)i;
        descriptor.compiler.source_fingerprint.bytes[i] =
            (uint8_t)(0xa0U + i);
        descriptor.compiler.request_input_fingerprint.bytes[i] =
            (uint8_t)(0x55U ^ i);
    }
    return descriptor;
}

static int test_stable_serialization_and_copy(void) {
    uint32_t global_indices[] = {7, 3};
    uint32_t local_indices[] = {2, 2, 9};
    VariantKeyDescriptor descriptor = make_descriptor(
        global_indices, 2, local_indices, 3);
    char source_directory[] = "Assets/Shaders";
    char source_basename[] = "Opaque.shader";
    char pass_name[] = "ForwardLit";
    char enabled_zero[] = "SHADER_API_DESKTOP";
    char enabled_one[] = "UNITY_COLORSPACE_GAMMA";
    const char* enabled_keywords[] = {enabled_zero, enabled_one};
    descriptor.compiler.source_directory = source_directory;
    descriptor.compiler.source_basename = source_basename;
    descriptor.compiler.pass_name = pass_name;
    descriptor.compiler.enabled_platform_keywords = enabled_keywords;
    VariantKey* key = NULL;
    CHECK(variant_key_init(&key, &descriptor) == VARIANT_KEY_OK);
    CHECK(key != NULL);
    VariantKeyDescriptor view;
    CHECK(variant_key_describe(key, &view) == VARIANT_KEY_OK);
    CHECK(view.shader_path_id == descriptor.shader_path_id);
    CHECK(view.archive_platform_index == descriptor.archive_platform_index);
    CHECK(view.hardware_tier_group == descriptor.hardware_tier_group);
    CHECK(view.serialized_program_type ==
          descriptor.serialized_program_type);
    CHECK(view.serialized_hardware_tier_present);
    CHECK(view.serialized_hardware_tier == 2);
    CHECK(view.player_metadata_present);
    CHECK(view.player_program_type == 15);
    CHECK(memcmp(view.player_header_words, descriptor.player_header_words,
                 sizeof(view.player_header_words)) == 0);
    CHECK(view.player_source_map == descriptor.player_source_map);
    CHECK(view.keyword_scopes_are_explicit);
    CHECK(view.global_keyword_index_count == 2);
    CHECK(view.global_keyword_indices[0] == 7);
    CHECK(strcmp(view.compiler.source_directory, "Assets/Shaders") == 0);
    CHECK(strcmp(view.compiler.pass_name, "ForwardLit") == 0);
    CHECK(view.compiler.enabled_platform_keyword_count == 2);
    CHECK(view.compiler.variant_keyword_count == 2);
    CHECK(strcmp(view.compiler.variant_keywords[0], "FEATURE_A") == 0);
    CHECK(view.compiler.user_keyword_count == 1);
    CHECK(strcmp(view.compiler.user_keywords[0], "USER_A") == 0);
    CHECK(strcmp(view.compiler.enabled_platform_keywords[0],
                 "SHADER_API_DESKTOP") == 0);

    uint8_t digest_before[VARIANT_KEY_DIGEST_SIZE];
    CHECK(variant_key_digest(key, digest_before) == VARIANT_KEY_OK);

    uint8_t* bytes = NULL;
    size_t size = 0;
    CHECK(variant_key_serialize(key, &bytes, &size) == VARIANT_KEY_OK);
    CHECK(size == 545U);
    CHECK(memcmp(bytes, "DXBCVKEY", 8) == 0);
    CHECK(read_le32(bytes + 8) == VARIANT_KEY_FORMAT_VERSION);
    CHECK(read_le64(bytes + 12) == size);
    CHECK((int64_t)read_le64(bytes + 20) == descriptor.shader_path_id);
    CHECK(read_le32(bytes + 40) == 4U);
    CHECK(read_le32(bytes + 48) == 3U);
    CHECK(read_le32(bytes + 60) == 15U);
    CHECK(read_le32(bytes + 64) == 1U);
    CHECK(read_le32(bytes + 68) == 2U);
    CHECK(read_le64(bytes + 72) == descriptor.serialized_requirements);
    CHECK(read_le32(bytes + 84) == 1U);
    CHECK(read_le32(bytes + 88) == 15U);
    CHECK(read_le32(bytes + 92) == UINT32_C(0x41));
    CHECK(read_le32(bytes + 104) == 7U);
    CHECK(read_le32(bytes + 108) == UINT32_C(0x12345678));
    CHECK(read_le32(bytes + 112) == 1U);
    CHECK(read_le32(bytes + 116) == 2U);
    CHECK(read_le32(bytes + 120) == 7U);
    CHECK(read_le32(bytes + 124) == 3U);
    CHECK(read_le32(bytes + 128) == 3U);
    CHECK(read_le32(bytes + 132) == 2U);
    CHECK(read_le32(bytes + 136) == 2U);
    CHECK(read_le32(bytes + 140) == 9U);
    CHECK(read_le32(bytes + 144) == descriptor.compiler.schema_version);
    variant_key_serialized_free(bytes);

    /* Initialization owns the arrays; caller mutation cannot change the key. */
    global_indices[0] = 99;
    local_indices[2] = 77;
    source_directory[0] = 'X';
    source_basename[0] = 'X';
    pass_name[0] = 'X';
    enabled_zero[0] = 'X';
    uint8_t digest_after[VARIANT_KEY_DIGEST_SIZE];
    CHECK(variant_key_digest(key, digest_after) == VARIANT_KEY_OK);
    CHECK(memcmp(digest_before, digest_after, sizeof(digest_before)) == 0);

    VariantKey* copy = NULL;
    CHECK(variant_key_copy(&copy, key) == VARIANT_KEY_OK);
    CHECK(copy != key);
    CHECK(variant_key_equal(copy, key));
    variant_key_free(key);
    key = NULL;
    CHECK(variant_key_digest(copy, digest_after) == VARIANT_KEY_OK);
    CHECK(memcmp(digest_before, digest_after, sizeof(digest_before)) == 0);

    /* This constant freezes every byte and the field order of the v4 schema. */
    CHECK(digest_matches_hex(
        digest_after,
        "800cf4ac3e9d14cb61cbfae196e219ff"
        "6216c11beb487164eb719d118b9f5259"));

    variant_key_free(copy);
    return 0;
}

static int test_scope_order_and_authority_are_identity(void) {
    uint32_t global_a[] = {7, 3};
    uint32_t local_a[] = {2, 2, 9};
    VariantKeyDescriptor descriptor_a = make_descriptor(
        global_a, 2, local_a, 3);
    VariantKey* key_a = NULL;
    CHECK(variant_key_init(&key_a, &descriptor_a) == VARIANT_KEY_OK);

    uint32_t global_scope_changed[] = {7, 3, 2};
    uint32_t local_scope_changed[] = {2, 9};
    VariantKeyDescriptor descriptor_scope = make_descriptor(
        global_scope_changed, 3, local_scope_changed, 2);
    VariantKey* key_scope = NULL;
    CHECK(variant_key_init(&key_scope, &descriptor_scope) == VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_scope));

    VariantKeyDescriptor descriptor_legacy_scope_encoding = descriptor_a;
    descriptor_legacy_scope_encoding.keyword_scopes_are_explicit = false;
    VariantKey* key_legacy_scope_encoding = NULL;
    CHECK(variant_key_init(&key_legacy_scope_encoding,
                           &descriptor_legacy_scope_encoding) ==
          VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_legacy_scope_encoding));

    uint32_t global_order_changed[] = {3, 7};
    VariantKeyDescriptor descriptor_order = make_descriptor(
        global_order_changed, 2, local_a, 3);
    VariantKey* key_order = NULL;
    CHECK(variant_key_init(&key_order, &descriptor_order) == VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_order));

    VariantKeyDescriptor descriptor_authority = descriptor_a;
    descriptor_authority.compiler.request_requirements ^= 1U;
    VariantKey* key_authority = NULL;
    CHECK(variant_key_init(&key_authority, &descriptor_authority) ==
          VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_authority));

    VariantKeyDescriptor descriptor_typetree = descriptor_a;
    descriptor_typetree.serialized_hardware_tier = 1;
    VariantKey* key_typetree = NULL;
    CHECK(variant_key_init(&key_typetree, &descriptor_typetree) ==
          VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_typetree));

    VariantKeyDescriptor descriptor_requirements = descriptor_a;
    descriptor_requirements.serialized_requirements ^=
        UINT64_C(0x0000010000000000);
    VariantKey* key_requirements = NULL;
    CHECK(variant_key_init(&key_requirements, &descriptor_requirements) ==
          VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_requirements));

    VariantKeyDescriptor descriptor_player = descriptor_a;
    descriptor_player.player_header_words[2] ^= 1U;
    VariantKey* key_player = NULL;
    CHECK(variant_key_init(&key_player, &descriptor_player) ==
          VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_player));

    VariantKeyDescriptor descriptor_source_map = descriptor_a;
    descriptor_source_map.player_source_map ^= 1U;
    VariantKey* key_source_map = NULL;
    CHECK(variant_key_init(&key_source_map, &descriptor_source_map) ==
          VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_source_map));

    const char* enabled_reordered[] = {
        "UNITY_COLORSPACE_GAMMA",
        "SHADER_API_DESKTOP",
    };
    VariantKeyDescriptor descriptor_platform_order = descriptor_a;
    descriptor_platform_order.compiler.enabled_platform_keywords =
        enabled_reordered;
    VariantKey* key_platform_order = NULL;
    CHECK(variant_key_init(&key_platform_order,
                           &descriptor_platform_order) == VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_platform_order));

    const char* variant_reordered[] = {"FEATURE_B", "FEATURE_A"};
    VariantKeyDescriptor descriptor_variant_order = descriptor_a;
    descriptor_variant_order.compiler.variant_keywords = variant_reordered;
    VariantKey* key_variant_order = NULL;
    CHECK(variant_key_init(&key_variant_order,
                           &descriptor_variant_order) == VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_variant_order));

    const char* user_changed[] = {"USER_B"};
    VariantKeyDescriptor descriptor_user = descriptor_a;
    descriptor_user.compiler.user_keywords = user_changed;
    VariantKey* key_user = NULL;
    CHECK(variant_key_init(&key_user, &descriptor_user) == VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_user));

    const char* enabled_scope_changed[] = {"SHADER_API_DESKTOP"};
    const char* disabled_scope_changed[] = {
        "UNITY_COLORSPACE_GAMMA",
        "STEREO_INSTANCING_ON",
        "UNITY_SINGLE_PASS_STEREO",
    };
    VariantKeyDescriptor descriptor_platform_scope = descriptor_a;
    descriptor_platform_scope.compiler.enabled_platform_keywords =
        enabled_scope_changed;
    descriptor_platform_scope.compiler.enabled_platform_keyword_count = 1;
    descriptor_platform_scope.compiler.disabled_keywords =
        disabled_scope_changed;
    descriptor_platform_scope.compiler.disabled_keyword_count = 3;
    VariantKey* key_platform_scope = NULL;
    CHECK(variant_key_init(&key_platform_scope,
                           &descriptor_platform_scope) == VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_platform_scope));

    VariantKeyDescriptor descriptor_source_context = descriptor_a;
    descriptor_source_context.compiler.pass_name = "ShadowCaster";
    VariantKey* key_source_context = NULL;
    CHECK(variant_key_init(&key_source_context,
                           &descriptor_source_context) == VARIANT_KEY_OK);
    CHECK(!variant_key_equal(key_a, key_source_context));

    uint8_t digest_a[VARIANT_KEY_DIGEST_SIZE];
    uint8_t digest_other[VARIANT_KEY_DIGEST_SIZE];
    CHECK(variant_key_digest(key_a, digest_a) == VARIANT_KEY_OK);
    CHECK(variant_key_digest(key_scope, digest_other) == VARIANT_KEY_OK);
    CHECK(memcmp(digest_a, digest_other, sizeof(digest_a)) != 0);

    variant_key_free(key_source_context);
    variant_key_free(key_user);
    variant_key_free(key_variant_order);
    variant_key_free(key_platform_scope);
    variant_key_free(key_platform_order);
    variant_key_free(key_source_map);
    variant_key_free(key_player);
    variant_key_free(key_requirements);
    variant_key_free(key_typetree);
    variant_key_free(key_authority);
    variant_key_free(key_order);
    variant_key_free(key_legacy_scope_encoding);
    variant_key_free(key_scope);
    variant_key_free(key_a);
    return 0;
}

static int test_absent_authority_payloads_are_canonical(void) {
    uint32_t global_indices[] = {7, 3};
    uint32_t local_indices[] = {2, 2, 9};
    VariantKeyDescriptor dirty = make_descriptor(
        global_indices, 2, local_indices, 3);
    dirty.serialized_hardware_tier_present = false;
    dirty.serialized_hardware_tier = -123;
    dirty.player_metadata_present = false;
    dirty.player_program_type = -456;
    dirty.player_header_words[0] = UINT32_MAX;
    dirty.player_header_words[3] = 99U;
    dirty.player_source_map = UINT32_MAX;

    VariantKeyDescriptor clean = dirty;
    clean.serialized_hardware_tier = 0;
    clean.player_program_type = 0;
    memset(clean.player_header_words, 0, sizeof(clean.player_header_words));
    clean.player_source_map = 0;

    VariantKey* dirty_key = NULL;
    VariantKey* clean_key = NULL;
    CHECK(variant_key_init(&dirty_key, &dirty) == VARIANT_KEY_OK);
    CHECK(variant_key_init(&clean_key, &clean) == VARIANT_KEY_OK);
    CHECK(variant_key_equal(dirty_key, clean_key));

    VariantKeyDescriptor view;
    CHECK(variant_key_describe(dirty_key, &view) == VARIANT_KEY_OK);
    CHECK(!view.serialized_hardware_tier_present);
    CHECK(view.serialized_hardware_tier == 0);
    CHECK(!view.player_metadata_present);
    CHECK(view.player_program_type == 0);
    CHECK(view.player_header_words[0] == 0);
    CHECK(view.player_header_words[3] == 0);
    CHECK(view.player_source_map == 0);

    uint8_t dirty_digest[VARIANT_KEY_DIGEST_SIZE];
    uint8_t clean_digest[VARIANT_KEY_DIGEST_SIZE];
    CHECK(variant_key_digest(dirty_key, dirty_digest) == VARIANT_KEY_OK);
    CHECK(variant_key_digest(clean_key, clean_digest) == VARIANT_KEY_OK);
    CHECK(memcmp(dirty_digest, clean_digest, sizeof(dirty_digest)) == 0);

    variant_key_free(clean_key);
    variant_key_free(dirty_key);
    return 0;
}

static int test_validation(void) {
    VariantKey* key = (VariantKey*)(uintptr_t)1;
    uint32_t one_index = 1;
    VariantKeyDescriptor descriptor = make_descriptor(
        &one_index, 1, NULL, 0);

    CHECK(variant_key_init(NULL, &descriptor) == VARIANT_KEY_INVALID_ARGUMENT);
    CHECK(variant_key_init(&key, NULL) == VARIANT_KEY_INVALID_ARGUMENT);
    CHECK(key == NULL);

    descriptor.global_keyword_indices = NULL;
    CHECK(variant_key_init(&key, &descriptor) ==
          VARIANT_KEY_INCONSISTENT_ARRAY);
    CHECK(key == NULL);

    descriptor.global_keyword_indices = &one_index;
    descriptor.global_keyword_index_count = 0;
    CHECK(variant_key_init(&key, &descriptor) ==
          VARIANT_KEY_INCONSISTENT_ARRAY);

    descriptor.global_keyword_index_count =
        (size_t)VARIANT_KEY_MAX_KEYWORD_INDICES + 1U;
    CHECK(variant_key_init(&key, &descriptor) == VARIANT_KEY_INVALID_COUNT);

    descriptor.global_keyword_index_count = 1;
    descriptor.compiler_platform = -1;
    CHECK(variant_key_init(&key, &descriptor) == VARIANT_KEY_INVALID_VALUE);

    descriptor.compiler_platform = 4;
    descriptor.hardware_tier_group = 4;
    CHECK(variant_key_init(&key, &descriptor) == VARIANT_KEY_INVALID_VALUE);

    descriptor.hardware_tier_group = 3;
    descriptor.serialized_hardware_tier = 4;
    CHECK(variant_key_init(&key, &descriptor) == VARIANT_KEY_INVALID_VALUE);

    descriptor.serialized_hardware_tier = 2;
    descriptor.compiler.source_directory = NULL;
    CHECK(variant_key_init(&key, &descriptor) == VARIANT_KEY_INVALID_VALUE);

    descriptor.compiler.source_directory = "Assets/Shaders";
    descriptor.compiler.enabled_platform_keyword_count = 0;
    CHECK(variant_key_init(&key, &descriptor) ==
          VARIANT_KEY_INCONSISTENT_ARRAY);

    descriptor.compiler.enabled_platform_keyword_count = 2;
    descriptor.compiler.environment_fingerprint.bytes[3] = 1;
    CHECK(!descriptor.compiler.environment_fingerprint.present);
    CHECK(variant_key_init(&key, &descriptor) == VARIANT_KEY_INVALID_VALUE);

    CHECK(variant_key_copy(&key, NULL) == VARIANT_KEY_INVALID_ARGUMENT);
    CHECK(key == NULL);
    CHECK(variant_key_describe(NULL, &descriptor) ==
          VARIANT_KEY_INVALID_ARGUMENT);
    CHECK(variant_key_describe((VariantKey*)(uintptr_t)1, NULL) ==
          VARIANT_KEY_INVALID_ARGUMENT);
    CHECK(!variant_key_equal(NULL, (VariantKey*)(uintptr_t)1));
    CHECK(variant_key_equal(NULL, NULL));
    CHECK(variant_key_digest(NULL, NULL) == VARIANT_KEY_INVALID_ARGUMENT);
    CHECK(strcmp(variant_key_status_string(VARIANT_KEY_OK), "ok") == 0);
    CHECK(strcmp(variant_key_status_string((VariantKeyStatus)999),
                 "unknown status") == 0);
    return 0;
}

int main(void) {
    CHECK(test_sha256_vectors() == 0);
    CHECK(test_stable_serialization_and_copy() == 0);
    CHECK(test_scope_order_and_authority_are_identity() == 0);
    CHECK(test_absent_authority_payloads_are_canonical() == 0);
    CHECK(test_validation() == 0);
    printf("variant key unit tests passed\n");
    return 0;
}
