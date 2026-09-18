// SPDX-License-Identifier: GPL-3.0-only

#include "app/whole_shader_subject.h"

#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(WHOLE_SHADER_SUBJECT_DIGEST_SIZE == COMMON_SHA256_DIGEST_SIZE,
               "whole-shader subjects use SHA-256 digests");

struct WholeShaderSubject {
    WholeShaderSubjectDescriptor descriptor;
    char* target_member_identity;
    char* candidate_logical_name;
    char* unity_version;
};

static const uint8_t subject_magic[8] = {
    'D', 'X', 'W', 'S', 'S', 'U', 'B', 'J'
};

static bool bounded_string_size(const char* value, size_t limit,
                                size_t* out_size) {
    if (!value || !out_size) return false;
    size_t size = 0U;
    while (size <= limit && value[size] != '\0') ++size;
    if (size > limit) return false;
    *out_size = size;
    return true;
}

static bool size_add(size_t* total, size_t value) {
    if (!total || value > SIZE_MAX - *total) return false;
    *total += value;
    return true;
}

static char* duplicate_string(const char* value, size_t size) {
    if (size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1U);
    if (!copy) return NULL;
    if (size != 0U) memcpy(copy, value, size);
    copy[size] = '\0';
    return copy;
}

static void write_u32_le(uint8_t** cursor, uint32_t value) {
    uint8_t* output = *cursor;
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
    *cursor += 4U;
}

static void write_u64_le(uint8_t** cursor, uint64_t value) {
    uint8_t* output = *cursor;
    for (unsigned index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (index * 8U));
    }
    *cursor += 8U;
}

static void write_bytes(uint8_t** cursor, const void* data, size_t size) {
    if (size != 0U) memcpy(*cursor, data, size);
    *cursor += size;
}

static void write_string(uint8_t** cursor, const char* value, size_t size) {
    write_u32_le(cursor, (uint32_t)size);
    write_bytes(cursor, value, size);
}

static bool absolute_host_path(const char* value, size_t size) {
    if (size == 0U) return false;
    if (value[0] == '/' || value[0] == '\\') return true;
    return size >= 3U &&
        ((value[0] >= 'A' && value[0] <= 'Z') ||
         (value[0] >= 'a' && value[0] <= 'z')) &&
        value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

WholeShaderSubjectStatus whole_shader_subject_create(
    WholeShaderSubject** out_subject,
    const WholeShaderSubjectDescriptor* descriptor) {
    if (!out_subject) return WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT;
    *out_subject = NULL;
    if (!descriptor) return WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT;

    size_t member_size = 0U;
    size_t name_size = 0U;
    size_t version_size = 0U;
    if (!bounded_string_size(descriptor->target_member_identity,
                             WHOLE_SHADER_SUBJECT_MAX_STRING_BYTES,
                             &member_size) ||
        !bounded_string_size(descriptor->candidate_logical_name,
                             WHOLE_SHADER_SUBJECT_MAX_STRING_BYTES,
                             &name_size) ||
        !bounded_string_size(descriptor->unity_version,
                             WHOLE_SHADER_SUBJECT_MAX_STRING_BYTES,
                             &version_size)) {
        return WHOLE_SHADER_SUBJECT_INVALID_VALUE;
    }
    if (descriptor->target_shader_path_id == 0 ||
        descriptor->target_class_id != WHOLE_SHADER_SUBJECT_SHADER_CLASS_ID ||
        name_size == 0U ||
        version_size == 0U ||
        absolute_host_path(descriptor->target_member_identity, member_size) ||
        (descriptor->source_residency !=
             WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE &&
         descriptor->source_residency != WHOLE_SHADER_SOURCE_BUNDLE_MEMBER) ||
        (descriptor->source_residency ==
             WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE &&
         (member_size != 0U || descriptor->target_member_index != 0U)) ||
        (descriptor->source_residency ==
             WHOLE_SHADER_SOURCE_BUNDLE_MEMBER &&
         member_size == 0U)) {
        return WHOLE_SHADER_SUBJECT_INVALID_VALUE;
    }

    WholeShaderSubject* subject =
        (WholeShaderSubject*)calloc(1U, sizeof(*subject));
    if (!subject) return WHOLE_SHADER_SUBJECT_ALLOCATION_FAILED;
    subject->target_member_identity = duplicate_string(
        descriptor->target_member_identity, member_size);
    subject->candidate_logical_name = duplicate_string(
        descriptor->candidate_logical_name, name_size);
    subject->unity_version = duplicate_string(
        descriptor->unity_version, version_size);
    if (!subject->target_member_identity || !subject->candidate_logical_name ||
        !subject->unity_version) {
        whole_shader_subject_free(subject);
        return WHOLE_SHADER_SUBJECT_ALLOCATION_FAILED;
    }

    subject->descriptor = *descriptor;
    subject->descriptor.target_member_identity =
        subject->target_member_identity;
    subject->descriptor.candidate_logical_name =
        subject->candidate_logical_name;
    subject->descriptor.unity_version = subject->unity_version;
    *out_subject = subject;
    return WHOLE_SHADER_SUBJECT_OK;
}

void whole_shader_subject_free(WholeShaderSubject* subject) {
    if (!subject) return;
    free(subject->target_member_identity);
    free(subject->candidate_logical_name);
    free(subject->unity_version);
    memset(subject, 0, sizeof(*subject));
    free(subject);
}

WholeShaderSubjectStatus whole_shader_subject_describe(
    const WholeShaderSubject* subject,
    WholeShaderSubjectDescriptor* out_descriptor) {
    if (!subject || !out_descriptor) {
        return WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT;
    }
    *out_descriptor = subject->descriptor;
    return WHOLE_SHADER_SUBJECT_OK;
}

bool whole_shader_subject_equal(const WholeShaderSubject* left,
                                const WholeShaderSubject* right) {
    if (!left || !right) return false;
    if (left == right) return true;
    const WholeShaderSubjectDescriptor* a = &left->descriptor;
    const WholeShaderSubjectDescriptor* b = &right->descriptor;
    return a->target_shader_path_id == b->target_shader_path_id &&
        a->target_class_id == b->target_class_id &&
        a->serialized_target_platform == b->serialized_target_platform &&
        a->build_platform == b->build_platform &&
        a->compiler_platform == b->compiler_platform &&
        a->graphics_api == b->graphics_api &&
        a->source_residency == b->source_residency &&
        a->target_member_index == b->target_member_index &&
        strcmp(a->target_member_identity, b->target_member_identity) == 0 &&
        strcmp(a->candidate_logical_name, b->candidate_logical_name) == 0 &&
        strcmp(a->unity_version, b->unity_version) == 0 &&
        memcmp(a->target_occurrence_digest, b->target_occurrence_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->target_serialized_file_digest,
               b->target_serialized_file_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->target_object_payload_digest,
               b->target_object_payload_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->candidate_source_digest, b->candidate_source_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->schema_authority_digest, b->schema_authority_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->compiler_profile_digest, b->compiler_profile_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->compiler_session_digest, b->compiler_session_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->player_profile_digest, b->player_profile_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->verification_scope_digest, b->verification_scope_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->dependency_map_digest, b->dependency_map_digest,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0 &&
        memcmp(a->producer_fingerprint, b->producer_fingerprint,
               WHOLE_SHADER_SUBJECT_DIGEST_SIZE) == 0;
}

WholeShaderSubjectStatus whole_shader_subject_serialize(
    const WholeShaderSubject* subject, uint8_t** out_data, size_t* out_size) {
    if (!out_data || !out_size) {
        return WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT;
    }
    *out_data = NULL;
    *out_size = 0U;
    if (!subject) return WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT;

    const WholeShaderSubjectDescriptor* descriptor = &subject->descriptor;
    const size_t member_size = strlen(descriptor->target_member_identity);
    const size_t name_size = strlen(descriptor->candidate_logical_name);
    const size_t version_size = strlen(descriptor->unity_version);
    size_t total = 8U + 4U + 8U + 8U + 6U * 4U + 8U +
        3U * 4U + 11U * WHOLE_SHADER_SUBJECT_DIGEST_SIZE;
    if (!size_add(&total, member_size) || !size_add(&total, name_size) ||
        !size_add(&total, version_size) || total > UINT64_MAX) {
        return WHOLE_SHADER_SUBJECT_SIZE_OVERFLOW;
    }

    uint8_t* output = (uint8_t*)malloc(total);
    if (!output) return WHOLE_SHADER_SUBJECT_ALLOCATION_FAILED;
    uint8_t* cursor = output;
    write_bytes(&cursor, subject_magic, sizeof(subject_magic));
    write_u32_le(&cursor, WHOLE_SHADER_SUBJECT_FORMAT_VERSION);
    write_u64_le(&cursor, (uint64_t)total);
    write_u64_le(&cursor, (uint64_t)descriptor->target_shader_path_id);
    write_u32_le(&cursor, (uint32_t)descriptor->target_class_id);
    write_u32_le(&cursor, descriptor->serialized_target_platform);
    write_u32_le(&cursor, descriptor->build_platform);
    write_u32_le(&cursor, (uint32_t)descriptor->compiler_platform);
    write_u32_le(&cursor, descriptor->graphics_api);
    write_u32_le(&cursor, (uint32_t)descriptor->source_residency);
    write_u64_le(&cursor, descriptor->target_member_index);
    write_string(&cursor, descriptor->target_member_identity, member_size);
    write_string(&cursor, descriptor->candidate_logical_name, name_size);
    write_string(&cursor, descriptor->unity_version, version_size);
    write_bytes(&cursor, descriptor->target_occurrence_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->target_serialized_file_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->target_object_payload_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->candidate_source_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->schema_authority_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->compiler_profile_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->compiler_session_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->player_profile_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->verification_scope_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->dependency_map_digest,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    write_bytes(&cursor, descriptor->producer_fingerprint,
                WHOLE_SHADER_SUBJECT_DIGEST_SIZE);
    if ((size_t)(cursor - output) != total) {
        free(output);
        return WHOLE_SHADER_SUBJECT_SIZE_OVERFLOW;
    }
    *out_data = output;
    *out_size = total;
    return WHOLE_SHADER_SUBJECT_OK;
}

void whole_shader_subject_serialized_free(uint8_t* data) {
    free(data);
}

WholeShaderSubjectStatus whole_shader_subject_digest(
    const WholeShaderSubject* subject,
    uint8_t digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE]) {
    if (!subject || !digest) return WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT;
    uint8_t* data = NULL;
    size_t size = 0U;
    const WholeShaderSubjectStatus status =
        whole_shader_subject_serialize(subject, &data, &size);
    if (status != WHOLE_SHADER_SUBJECT_OK) return status;
    common_sha256(data, size, digest);
    free(data);
    return WHOLE_SHADER_SUBJECT_OK;
}

const char* whole_shader_subject_status_name(WholeShaderSubjectStatus status) {
    switch (status) {
        case WHOLE_SHADER_SUBJECT_OK: return "ok";
        case WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT:
            return "invalid-argument";
        case WHOLE_SHADER_SUBJECT_INVALID_VALUE: return "invalid-value";
        case WHOLE_SHADER_SUBJECT_SIZE_OVERFLOW: return "size-overflow";
        case WHOLE_SHADER_SUBJECT_ALLOCATION_FAILED:
            return "allocation-failed";
    }
    return "unknown";
}
