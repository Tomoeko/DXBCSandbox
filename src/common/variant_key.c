// SPDX-License-Identifier: GPL-3.0-only

#include "common/variant_key.h"

#include "common/sha256.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t VARIANT_KEY_MAGIC[8] = {
    'D', 'X', 'B', 'C', 'V', 'K', 'E', 'Y'
};

struct VariantKey {
    int64_t shader_path_id;
    uint32_t subshader_index;
    uint32_t pass_index;
    uint32_t stage;
    int32_t compiler_platform;
    uint32_t archive_platform_index;
    uint32_t hardware_tier_group;
    uint32_t subprogram_index;
    int32_t parameter_blob_index;
    int32_t serialized_program_type;
    bool serialized_hardware_tier_present;
    int32_t serialized_hardware_tier;
    uint64_t serialized_requirements;
    uint32_t serialized_program_mask;
    bool player_metadata_present;
    int32_t player_program_type;
    uint32_t player_header_words[4];
    uint32_t player_source_map;
    bool keyword_scopes_are_explicit;
    uint32_t* global_keyword_indices;
    size_t global_keyword_index_count;
    uint32_t* local_keyword_indices;
    size_t local_keyword_index_count;
    VariantCompilerAuthority compiler;
};

typedef struct {
    uint8_t* data;
    size_t size;
    size_t offset;
} VariantKeyWriter;

static bool bytes_are_zero(const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

static bool fingerprint_is_valid(const VariantKeyFingerprint* fingerprint) {
    return fingerprint->present ||
           bytes_are_zero(fingerprint->bytes, sizeof(fingerprint->bytes));
}

static bool bounded_string_length(const char* value, size_t* out_length) {
    if (!value || !out_length) return false;
    for (size_t i = 0; i <= VARIANT_KEY_MAX_STRING_BYTES; i++) {
        if (value[i] == '\0') {
            *out_length = i;
            return true;
        }
    }
    return false;
}

static VariantKeyStatus validate_string_array(
    const char* const* values, size_t count, bool reject_empty) {
    if (count > VARIANT_KEY_MAX_COMPILER_KEYWORDS || count > UINT32_MAX) {
        return VARIANT_KEY_INVALID_COUNT;
    }
    if ((count == 0) != (values == NULL)) {
        return VARIANT_KEY_INCONSISTENT_ARRAY;
    }
    for (size_t i = 0; i < count; i++) {
        size_t length = 0;
        if (!bounded_string_length(values[i], &length) ||
            (reject_empty && length == 0)) {
            return VARIANT_KEY_INVALID_VALUE;
        }
    }
    return VARIANT_KEY_OK;
}

static VariantKeyStatus validate_descriptor(
    const VariantKeyDescriptor* descriptor) {
    if (!descriptor) return VARIANT_KEY_INVALID_ARGUMENT;

    if (descriptor->global_keyword_index_count >
            VARIANT_KEY_MAX_KEYWORD_INDICES ||
        descriptor->local_keyword_index_count >
            VARIANT_KEY_MAX_KEYWORD_INDICES ||
        descriptor->global_keyword_index_count > UINT32_MAX ||
        descriptor->local_keyword_index_count > UINT32_MAX) {
        return VARIANT_KEY_INVALID_COUNT;
    }
    if ((descriptor->global_keyword_index_count == 0) !=
            (descriptor->global_keyword_indices == NULL) ||
        (descriptor->local_keyword_index_count == 0) !=
            (descriptor->local_keyword_indices == NULL)) {
        return VARIANT_KEY_INCONSISTENT_ARRAY;
    }
    size_t ignored_length = 0;
    if (!bounded_string_length(descriptor->compiler.source_directory,
                               &ignored_length) ||
        !bounded_string_length(descriptor->compiler.source_basename,
                               &ignored_length) ||
        !bounded_string_length(descriptor->compiler.pass_name,
                               &ignored_length)) {
        return VARIANT_KEY_INVALID_VALUE;
    }
    VariantKeyStatus string_array_status = validate_string_array(
        descriptor->compiler.variant_keywords,
        descriptor->compiler.variant_keyword_count, true);
    if (string_array_status != VARIANT_KEY_OK) return string_array_status;
    string_array_status = validate_string_array(
        descriptor->compiler.user_keywords,
        descriptor->compiler.user_keyword_count, true);
    if (string_array_status != VARIANT_KEY_OK) return string_array_status;
    string_array_status = validate_string_array(
        descriptor->compiler.enabled_platform_keywords,
        descriptor->compiler.enabled_platform_keyword_count, true);
    if (string_array_status != VARIANT_KEY_OK) return string_array_status;
    string_array_status = validate_string_array(
        descriptor->compiler.disabled_keywords,
        descriptor->compiler.disabled_keyword_count, true);
    if (string_array_status != VARIANT_KEY_OK) return string_array_status;
    if (descriptor->compiler_platform < 0 ||
        descriptor->hardware_tier_group > 3U ||
        descriptor->parameter_blob_index < -1 ||
        descriptor->serialized_program_type < 0 ||
        (descriptor->serialized_hardware_tier_present &&
         (descriptor->serialized_hardware_tier < 0 ||
          descriptor->serialized_hardware_tier > 3)) ||
        (descriptor->player_metadata_present &&
         descriptor->player_program_type < 0) ||
        descriptor->compiler.language < 0 ||
        descriptor->compiler.shader_type < 0 ||
        descriptor->compiler.program_start < 0 ||
        descriptor->compiler.render_state_length < 0) {
        return VARIANT_KEY_INVALID_VALUE;
    }
    if (!fingerprint_is_valid(&descriptor->compiler.compiler_fingerprint) ||
        !fingerprint_is_valid(&descriptor->compiler.environment_fingerprint) ||
        !fingerprint_is_valid(&descriptor->compiler.source_fingerprint) ||
        !fingerprint_is_valid(
            &descriptor->compiler.request_input_fingerprint)) {
        return VARIANT_KEY_INVALID_VALUE;
    }
    return VARIANT_KEY_OK;
}

static uint32_t* copy_indices(const uint32_t* indices, size_t count) {
    if (count == 0) return NULL;
    if (count > SIZE_MAX / sizeof(*indices)) return NULL;
    uint32_t* result = (uint32_t*)malloc(count * sizeof(*result));
    if (!result) return NULL;
    memcpy(result, indices, count * sizeof(*result));
    return result;
}

static char* copy_string(const char* value) {
    size_t length = strlen(value);
    char* result = (char*)malloc(length + 1U);
    if (!result) return NULL;
    memcpy(result, value, length + 1U);
    return result;
}

static char** copy_string_array(const char* const* values, size_t count) {
    if (count == 0) return NULL;
    if (count > SIZE_MAX / sizeof(char*)) return NULL;
    char** result = (char**)calloc(count, sizeof(*result));
    if (!result) return NULL;
    for (size_t i = 0; i < count; i++) {
        result[i] = copy_string(values[i]);
        if (!result[i]) {
            for (size_t j = 0; j < i; j++) free(result[j]);
            free(result);
            return NULL;
        }
    }
    return result;
}

static void free_string_array(const char* const* values, size_t count) {
    if (!values) return;
    for (size_t i = 0; i < count; i++) free((void*)values[i]);
    free((void*)values);
}

static void canonicalize_fingerprint(VariantKeyFingerprint* fingerprint) {
    if (!fingerprint->present) {
        memset(fingerprint->bytes, 0, sizeof(fingerprint->bytes));
    }
}

VariantKeyStatus variant_key_init(
    VariantKey** out_key, const VariantKeyDescriptor* descriptor) {
    if (!out_key) return VARIANT_KEY_INVALID_ARGUMENT;
    *out_key = NULL;

    VariantKeyStatus status = validate_descriptor(descriptor);
    if (status != VARIANT_KEY_OK) return status;

    VariantKey* key = (VariantKey*)calloc(1, sizeof(*key));
    if (!key) return VARIANT_KEY_ALLOCATION_FAILED;

    key->global_keyword_indices = copy_indices(
        descriptor->global_keyword_indices,
        descriptor->global_keyword_index_count);
    if (descriptor->global_keyword_index_count > 0 &&
        !key->global_keyword_indices) {
        variant_key_free(key);
        return VARIANT_KEY_ALLOCATION_FAILED;
    }
    key->local_keyword_indices = copy_indices(
        descriptor->local_keyword_indices,
        descriptor->local_keyword_index_count);
    if (descriptor->local_keyword_index_count > 0 &&
        !key->local_keyword_indices) {
        variant_key_free(key);
        return VARIANT_KEY_ALLOCATION_FAILED;
    }

    key->shader_path_id = descriptor->shader_path_id;
    key->subshader_index = descriptor->subshader_index;
    key->pass_index = descriptor->pass_index;
    key->stage = descriptor->stage;
    key->compiler_platform = descriptor->compiler_platform;
    key->archive_platform_index = descriptor->archive_platform_index;
    key->hardware_tier_group = descriptor->hardware_tier_group;
    key->subprogram_index = descriptor->subprogram_index;
    key->parameter_blob_index = descriptor->parameter_blob_index;
    key->serialized_program_type = descriptor->serialized_program_type;
    key->serialized_hardware_tier_present =
        descriptor->serialized_hardware_tier_present;
    key->serialized_hardware_tier =
        descriptor->serialized_hardware_tier_present
            ? descriptor->serialized_hardware_tier
            : 0;
    key->serialized_requirements = descriptor->serialized_requirements;
    key->serialized_program_mask = descriptor->serialized_program_mask;
    key->player_metadata_present = descriptor->player_metadata_present;
    key->player_program_type = descriptor->player_metadata_present
                                   ? descriptor->player_program_type
                                   : 0;
    if (descriptor->player_metadata_present) {
        memcpy(key->player_header_words, descriptor->player_header_words,
               sizeof(key->player_header_words));
        key->player_source_map = descriptor->player_source_map;
    }
    key->keyword_scopes_are_explicit =
        descriptor->keyword_scopes_are_explicit;
    key->global_keyword_index_count =
        descriptor->global_keyword_index_count;
    key->local_keyword_index_count = descriptor->local_keyword_index_count;
    key->compiler = descriptor->compiler;
    key->compiler.source_directory = NULL;
    key->compiler.source_basename = NULL;
    key->compiler.pass_name = NULL;
    key->compiler.variant_keywords = NULL;
    key->compiler.variant_keyword_count = 0;
    key->compiler.user_keywords = NULL;
    key->compiler.user_keyword_count = 0;
    key->compiler.enabled_platform_keywords = NULL;
    key->compiler.enabled_platform_keyword_count = 0;
    key->compiler.disabled_keywords = NULL;
    key->compiler.disabled_keyword_count = 0;
    canonicalize_fingerprint(&key->compiler.compiler_fingerprint);
    canonicalize_fingerprint(&key->compiler.environment_fingerprint);
    canonicalize_fingerprint(&key->compiler.source_fingerprint);
    canonicalize_fingerprint(&key->compiler.request_input_fingerprint);

    key->compiler.source_directory = copy_string(
        descriptor->compiler.source_directory);
    key->compiler.source_basename = copy_string(
        descriptor->compiler.source_basename);
    key->compiler.pass_name = copy_string(descriptor->compiler.pass_name);
    if (!key->compiler.source_directory || !key->compiler.source_basename ||
        !key->compiler.pass_name) {
        variant_key_free(key);
        return VARIANT_KEY_ALLOCATION_FAILED;
    }
    key->compiler.variant_keywords = (const char* const*)copy_string_array(
        descriptor->compiler.variant_keywords,
        descriptor->compiler.variant_keyword_count);
    if (descriptor->compiler.variant_keyword_count > 0 &&
        !key->compiler.variant_keywords) {
        variant_key_free(key);
        return VARIANT_KEY_ALLOCATION_FAILED;
    }
    key->compiler.variant_keyword_count =
        descriptor->compiler.variant_keyword_count;
    key->compiler.user_keywords = (const char* const*)copy_string_array(
        descriptor->compiler.user_keywords,
        descriptor->compiler.user_keyword_count);
    if (descriptor->compiler.user_keyword_count > 0 &&
        !key->compiler.user_keywords) {
        variant_key_free(key);
        return VARIANT_KEY_ALLOCATION_FAILED;
    }
    key->compiler.user_keyword_count = descriptor->compiler.user_keyword_count;
    key->compiler.enabled_platform_keywords =
        (const char* const*)copy_string_array(
            descriptor->compiler.enabled_platform_keywords,
            descriptor->compiler.enabled_platform_keyword_count);
    if (descriptor->compiler.enabled_platform_keyword_count > 0 &&
        !key->compiler.enabled_platform_keywords) {
        variant_key_free(key);
        return VARIANT_KEY_ALLOCATION_FAILED;
    }
    key->compiler.enabled_platform_keyword_count =
        descriptor->compiler.enabled_platform_keyword_count;
    key->compiler.disabled_keywords = (const char* const*)copy_string_array(
        descriptor->compiler.disabled_keywords,
        descriptor->compiler.disabled_keyword_count);
    if (descriptor->compiler.disabled_keyword_count > 0 &&
        !key->compiler.disabled_keywords) {
        variant_key_free(key);
        return VARIANT_KEY_ALLOCATION_FAILED;
    }
    key->compiler.disabled_keyword_count =
        descriptor->compiler.disabled_keyword_count;

    *out_key = key;
    return VARIANT_KEY_OK;
}

VariantKeyStatus variant_key_copy(
    VariantKey** out_key, const VariantKey* source) {
    if (!out_key) return VARIANT_KEY_INVALID_ARGUMENT;
    *out_key = NULL;
    if (!source) return VARIANT_KEY_INVALID_ARGUMENT;

    VariantKeyDescriptor descriptor;
    VariantKeyStatus status = variant_key_describe(source, &descriptor);
    if (status != VARIANT_KEY_OK) return status;
    return variant_key_init(out_key, &descriptor);
}

void variant_key_free(VariantKey* key) {
    if (!key) return;
    free(key->global_keyword_indices);
    free(key->local_keyword_indices);
    free((void*)key->compiler.source_directory);
    free((void*)key->compiler.source_basename);
    free((void*)key->compiler.pass_name);
    free_string_array(key->compiler.variant_keywords,
                      key->compiler.variant_keyword_count);
    free_string_array(key->compiler.user_keywords,
                      key->compiler.user_keyword_count);
    free_string_array(key->compiler.enabled_platform_keywords,
                      key->compiler.enabled_platform_keyword_count);
    free_string_array(key->compiler.disabled_keywords,
                      key->compiler.disabled_keyword_count);
    memset(key, 0, sizeof(*key));
    free(key);
}

VariantKeyStatus variant_key_describe(
    const VariantKey* key, VariantKeyDescriptor* out_descriptor) {
    if (!key || !out_descriptor) return VARIANT_KEY_INVALID_ARGUMENT;
    memset(out_descriptor, 0, sizeof(*out_descriptor));
    out_descriptor->shader_path_id = key->shader_path_id;
    out_descriptor->subshader_index = key->subshader_index;
    out_descriptor->pass_index = key->pass_index;
    out_descriptor->stage = key->stage;
    out_descriptor->compiler_platform = key->compiler_platform;
    out_descriptor->archive_platform_index = key->archive_platform_index;
    out_descriptor->hardware_tier_group = key->hardware_tier_group;
    out_descriptor->subprogram_index = key->subprogram_index;
    out_descriptor->parameter_blob_index = key->parameter_blob_index;
    out_descriptor->serialized_program_type = key->serialized_program_type;
    out_descriptor->serialized_hardware_tier_present =
        key->serialized_hardware_tier_present;
    out_descriptor->serialized_hardware_tier =
        key->serialized_hardware_tier;
    out_descriptor->serialized_requirements = key->serialized_requirements;
    out_descriptor->serialized_program_mask = key->serialized_program_mask;
    out_descriptor->player_metadata_present = key->player_metadata_present;
    out_descriptor->player_program_type = key->player_program_type;
    memcpy(out_descriptor->player_header_words, key->player_header_words,
           sizeof(out_descriptor->player_header_words));
    out_descriptor->player_source_map = key->player_source_map;
    out_descriptor->keyword_scopes_are_explicit =
        key->keyword_scopes_are_explicit;
    out_descriptor->global_keyword_indices = key->global_keyword_indices;
    out_descriptor->global_keyword_index_count =
        key->global_keyword_index_count;
    out_descriptor->local_keyword_indices = key->local_keyword_indices;
    out_descriptor->local_keyword_index_count = key->local_keyword_index_count;
    out_descriptor->compiler = key->compiler;
    return VARIANT_KEY_OK;
}

static bool fingerprints_equal(const VariantKeyFingerprint* left,
                               const VariantKeyFingerprint* right) {
    return left->present == right->present &&
           (!left->present ||
            memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0);
}

static bool string_arrays_equal(const char* const* left,
                                const char* const* right, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(left[i], right[i]) != 0) return false;
    }
    return true;
}

static bool compiler_authority_equal(const VariantCompilerAuthority* left,
                                     const VariantCompilerAuthority* right) {
    return left->schema_version == right->schema_version &&
           left->build_platform == right->build_platform &&
           left->compiler_flags == right->compiler_flags &&
           left->language == right->language &&
           left->shader_type == right->shader_type &&
           left->request_requirements == right->request_requirements &&
           left->request_program_mask == right->request_program_mask &&
           left->program_start == right->program_start &&
           left->render_state_length == right->render_state_length &&
           left->valid_apis == right->valid_apis &&
           left->caching_preprocessor == right->caching_preprocessor &&
           left->preprocess_only == right->preprocess_only &&
           left->strip_line_directives == right->strip_line_directives &&
           strcmp(left->source_directory, right->source_directory) == 0 &&
           strcmp(left->source_basename, right->source_basename) == 0 &&
           strcmp(left->pass_name, right->pass_name) == 0 &&
           left->variant_keyword_count == right->variant_keyword_count &&
           left->user_keyword_count == right->user_keyword_count &&
           left->enabled_platform_keyword_count ==
               right->enabled_platform_keyword_count &&
           left->disabled_keyword_count == right->disabled_keyword_count &&
           string_arrays_equal(left->variant_keywords,
                               right->variant_keywords,
                               left->variant_keyword_count) &&
           string_arrays_equal(left->user_keywords,
                               right->user_keywords,
                               left->user_keyword_count) &&
           string_arrays_equal(left->enabled_platform_keywords,
                               right->enabled_platform_keywords,
                               left->enabled_platform_keyword_count) &&
           string_arrays_equal(left->disabled_keywords,
                               right->disabled_keywords,
                               left->disabled_keyword_count) &&
           fingerprints_equal(&left->compiler_fingerprint,
                              &right->compiler_fingerprint) &&
           fingerprints_equal(&left->environment_fingerprint,
                              &right->environment_fingerprint) &&
           fingerprints_equal(&left->source_fingerprint,
                              &right->source_fingerprint) &&
           fingerprints_equal(&left->request_input_fingerprint,
                              &right->request_input_fingerprint);
}

bool variant_key_equal(const VariantKey* left, const VariantKey* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    if (left->shader_path_id != right->shader_path_id ||
        left->subshader_index != right->subshader_index ||
        left->pass_index != right->pass_index || left->stage != right->stage ||
        left->compiler_platform != right->compiler_platform ||
        left->archive_platform_index != right->archive_platform_index ||
        left->hardware_tier_group != right->hardware_tier_group ||
        left->subprogram_index != right->subprogram_index ||
        left->parameter_blob_index != right->parameter_blob_index ||
        left->serialized_program_type != right->serialized_program_type ||
        left->serialized_hardware_tier_present !=
            right->serialized_hardware_tier_present ||
        left->serialized_hardware_tier !=
            right->serialized_hardware_tier ||
        left->serialized_requirements != right->serialized_requirements ||
        left->serialized_program_mask != right->serialized_program_mask ||
        left->player_metadata_present != right->player_metadata_present ||
        left->player_program_type != right->player_program_type ||
        memcmp(left->player_header_words, right->player_header_words,
               sizeof(left->player_header_words)) != 0 ||
        left->player_source_map != right->player_source_map ||
        left->keyword_scopes_are_explicit !=
            right->keyword_scopes_are_explicit ||
        left->global_keyword_index_count !=
            right->global_keyword_index_count ||
        left->local_keyword_index_count != right->local_keyword_index_count ||
        !compiler_authority_equal(&left->compiler, &right->compiler)) {
        return false;
    }
    return (left->global_keyword_index_count == 0 ||
            memcmp(left->global_keyword_indices,
                   right->global_keyword_indices,
                   left->global_keyword_index_count * sizeof(uint32_t)) == 0) &&
           (left->local_keyword_index_count == 0 ||
            memcmp(left->local_keyword_indices,
                   right->local_keyword_indices,
                   left->local_keyword_index_count * sizeof(uint32_t)) == 0);
}

static bool checked_add_size(size_t* total, size_t amount) {
    if (*total > SIZE_MAX - amount) return false;
    *total += amount;
    return true;
}

static bool checked_add_encoded_string_size(size_t* total,
                                            const char* value) {
    size_t length = strlen(value);
    return length <= UINT32_MAX && checked_add_size(total, 4U) &&
           checked_add_size(total, length);
}

static bool checked_add_string_array_size(
    size_t* total, const char* const* values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!checked_add_encoded_string_size(total, values[i])) return false;
    }
    return true;
}

static bool writer_bytes(VariantKeyWriter* writer,
                         const void* data, size_t size) {
    if (writer->offset > writer->size ||
        size > writer->size - writer->offset) {
        return false;
    }
    if (size > 0) memcpy(writer->data + writer->offset, data, size);
    writer->offset += size;
    return true;
}

static bool writer_u32(VariantKeyWriter* writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static bool writer_u64(VariantKeyWriter* writer, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static bool writer_i32(VariantKeyWriter* writer, int32_t value) {
    return writer_u32(writer, (uint32_t)value);
}

static bool writer_i64(VariantKeyWriter* writer, int64_t value) {
    return writer_u64(writer, (uint64_t)value);
}

static bool writer_string(VariantKeyWriter* writer, const char* value) {
    size_t length = strlen(value);
    return length <= UINT32_MAX &&
           writer_u32(writer, (uint32_t)length) &&
           writer_bytes(writer, value, length);
}

static bool writer_string_array(VariantKeyWriter* writer,
                                const char* const* values, size_t count) {
    if (count > UINT32_MAX || !writer_u32(writer, (uint32_t)count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!writer_string(writer, values[i])) return false;
    }
    return true;
}

static bool writer_fingerprint(VariantKeyWriter* writer,
                               const VariantKeyFingerprint* fingerprint) {
    static const uint8_t zero_digest[VARIANT_KEY_DIGEST_SIZE] = {0};
    return writer_u32(writer, fingerprint->present ? 1U : 0U) &&
           writer_bytes(writer,
                        fingerprint->present ? fingerprint->bytes : zero_digest,
                        VARIANT_KEY_DIGEST_SIZE);
}

VariantKeyStatus variant_key_serialize(
    const VariantKey* key, uint8_t** out_data, size_t* out_size) {
    if (!out_data || !out_size) return VARIANT_KEY_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0;
    if (!key) return VARIANT_KEY_INVALID_ARGUMENT;

    /*
     * v4 fixed fields, compiler authority, and four
     * presence-tagged fingerprints.  String payload bytes are added below.
     */
    size_t size = 340U;
    if (key->global_keyword_index_count >
            SIZE_MAX / sizeof(uint32_t) ||
        key->local_keyword_index_count >
            SIZE_MAX / sizeof(uint32_t) ||
        !checked_add_size(&size,
            key->global_keyword_index_count * sizeof(uint32_t)) ||
        !checked_add_size(&size,
            key->local_keyword_index_count * sizeof(uint32_t)) ||
        !checked_add_encoded_string_size(
            &size, key->compiler.source_directory) ||
        !checked_add_encoded_string_size(
            &size, key->compiler.source_basename) ||
        !checked_add_encoded_string_size(&size, key->compiler.pass_name) ||
        !checked_add_string_array_size(
            &size, key->compiler.variant_keywords,
            key->compiler.variant_keyword_count) ||
        !checked_add_string_array_size(
            &size, key->compiler.user_keywords,
            key->compiler.user_keyword_count) ||
        !checked_add_string_array_size(
            &size, key->compiler.enabled_platform_keywords,
            key->compiler.enabled_platform_keyword_count) ||
        !checked_add_string_array_size(
            &size, key->compiler.disabled_keywords,
            key->compiler.disabled_keyword_count)) {
        return VARIANT_KEY_SIZE_OVERFLOW;
    }

    uint8_t* data = (uint8_t*)malloc(size);
    if (!data) return VARIANT_KEY_ALLOCATION_FAILED;
    VariantKeyWriter writer = {data, size, 0};

#define WRITE_OR_FAIL(expression) do { \
    if (!(expression)) { \
        free(data); \
        return VARIANT_KEY_SIZE_OVERFLOW; \
    } \
} while (0)

    WRITE_OR_FAIL(writer_bytes(&writer, VARIANT_KEY_MAGIC,
                               sizeof(VARIANT_KEY_MAGIC)));
    WRITE_OR_FAIL(writer_u32(&writer, VARIANT_KEY_FORMAT_VERSION));
    WRITE_OR_FAIL(writer_u64(&writer, (uint64_t)size));
    WRITE_OR_FAIL(writer_i64(&writer, key->shader_path_id));
    WRITE_OR_FAIL(writer_u32(&writer, key->subshader_index));
    WRITE_OR_FAIL(writer_u32(&writer, key->pass_index));
    WRITE_OR_FAIL(writer_u32(&writer, key->stage));
    WRITE_OR_FAIL(writer_i32(&writer, key->compiler_platform));
    WRITE_OR_FAIL(writer_u32(&writer, key->archive_platform_index));
    WRITE_OR_FAIL(writer_u32(&writer, key->hardware_tier_group));
    WRITE_OR_FAIL(writer_u32(&writer, key->subprogram_index));
    WRITE_OR_FAIL(writer_i32(&writer, key->parameter_blob_index));
    WRITE_OR_FAIL(writer_i32(&writer, key->serialized_program_type));
    WRITE_OR_FAIL(writer_u32(
        &writer, key->serialized_hardware_tier_present ? 1U : 0U));
    WRITE_OR_FAIL(writer_i32(&writer, key->serialized_hardware_tier));
    WRITE_OR_FAIL(writer_u64(&writer, key->serialized_requirements));
    WRITE_OR_FAIL(writer_u32(&writer, key->serialized_program_mask));
    WRITE_OR_FAIL(writer_u32(
        &writer, key->player_metadata_present ? 1U : 0U));
    WRITE_OR_FAIL(writer_i32(&writer, key->player_program_type));
    for (size_t word_index = 0; word_index < 4U; ++word_index) {
        WRITE_OR_FAIL(writer_u32(
            &writer, key->player_header_words[word_index]));
    }
    WRITE_OR_FAIL(writer_u32(&writer, key->player_source_map));
    WRITE_OR_FAIL(writer_u32(
        &writer, key->keyword_scopes_are_explicit ? 1U : 0U));
    WRITE_OR_FAIL(writer_u32(
        &writer, (uint32_t)key->global_keyword_index_count));
    for (size_t i = 0; i < key->global_keyword_index_count; i++) {
        WRITE_OR_FAIL(writer_u32(&writer, key->global_keyword_indices[i]));
    }
    WRITE_OR_FAIL(writer_u32(
        &writer, (uint32_t)key->local_keyword_index_count));
    for (size_t i = 0; i < key->local_keyword_index_count; i++) {
        WRITE_OR_FAIL(writer_u32(&writer, key->local_keyword_indices[i]));
    }

    WRITE_OR_FAIL(writer_u32(&writer, key->compiler.schema_version));
    WRITE_OR_FAIL(writer_u32(&writer, key->compiler.build_platform));
    WRITE_OR_FAIL(writer_u32(&writer, key->compiler.compiler_flags));
    WRITE_OR_FAIL(writer_i32(&writer, key->compiler.language));
    WRITE_OR_FAIL(writer_i32(&writer, key->compiler.shader_type));
    WRITE_OR_FAIL(writer_u64(&writer, key->compiler.request_requirements));
    WRITE_OR_FAIL(writer_u32(&writer, key->compiler.request_program_mask));
    WRITE_OR_FAIL(writer_i32(&writer, key->compiler.program_start));
    WRITE_OR_FAIL(writer_i32(&writer, key->compiler.render_state_length));
    WRITE_OR_FAIL(writer_u32(&writer, key->compiler.valid_apis));
    WRITE_OR_FAIL(writer_u32(
        &writer, key->compiler.caching_preprocessor ? 1U : 0U));
    WRITE_OR_FAIL(writer_u32(
        &writer, key->compiler.preprocess_only ? 1U : 0U));
    WRITE_OR_FAIL(writer_u32(
        &writer, key->compiler.strip_line_directives ? 1U : 0U));
    WRITE_OR_FAIL(writer_string(&writer, key->compiler.source_directory));
    WRITE_OR_FAIL(writer_string(&writer, key->compiler.source_basename));
    WRITE_OR_FAIL(writer_string(&writer, key->compiler.pass_name));
    WRITE_OR_FAIL(writer_string_array(
        &writer, key->compiler.variant_keywords,
        key->compiler.variant_keyword_count));
    WRITE_OR_FAIL(writer_string_array(
        &writer, key->compiler.user_keywords,
        key->compiler.user_keyword_count));
    WRITE_OR_FAIL(writer_string_array(
        &writer, key->compiler.enabled_platform_keywords,
        key->compiler.enabled_platform_keyword_count));
    WRITE_OR_FAIL(writer_string_array(
        &writer, key->compiler.disabled_keywords,
        key->compiler.disabled_keyword_count));
    WRITE_OR_FAIL(writer_fingerprint(
        &writer, &key->compiler.compiler_fingerprint));
    WRITE_OR_FAIL(writer_fingerprint(
        &writer, &key->compiler.environment_fingerprint));
    WRITE_OR_FAIL(writer_fingerprint(
        &writer, &key->compiler.source_fingerprint));
    WRITE_OR_FAIL(writer_fingerprint(
        &writer, &key->compiler.request_input_fingerprint));

#undef WRITE_OR_FAIL

    if (writer.offset != size) {
        free(data);
        return VARIANT_KEY_SIZE_OVERFLOW;
    }
    *out_data = data;
    *out_size = size;
    return VARIANT_KEY_OK;
}

void variant_key_serialized_free(uint8_t* data) {
    free(data);
}

VariantKeyStatus variant_key_digest(
    const VariantKey* key, uint8_t digest[VARIANT_KEY_DIGEST_SIZE]) {
    if (!digest) return VARIANT_KEY_INVALID_ARGUMENT;
    uint8_t* data = NULL;
    size_t size = 0;
    VariantKeyStatus status = variant_key_serialize(key, &data, &size);
    if (status != VARIANT_KEY_OK) return status;
    common_sha256(data, size, digest);
    variant_key_serialized_free(data);
    return VARIANT_KEY_OK;
}

const char* variant_key_status_string(VariantKeyStatus status) {
    switch (status) {
        case VARIANT_KEY_OK: return "ok";
        case VARIANT_KEY_INVALID_ARGUMENT: return "invalid argument";
        case VARIANT_KEY_INVALID_COUNT: return "invalid count";
        case VARIANT_KEY_INCONSISTENT_ARRAY: return "inconsistent array";
        case VARIANT_KEY_INVALID_VALUE: return "invalid value";
        case VARIANT_KEY_ALLOCATION_FAILED: return "allocation failed";
        case VARIANT_KEY_SIZE_OVERFLOW: return "size overflow";
        default: return "unknown status";
    }
}
