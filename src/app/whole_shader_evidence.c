// SPDX-License-Identifier: GPL-3.0-only

#include "app/whole_shader_evidence.h"

#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(WHOLE_SHADER_EVIDENCE_DIGEST_SIZE == COMMON_SHA256_DIGEST_SIZE,
               "whole-shader evidence uses SHA-256 digests");

struct WholeShaderEvidence {
    WholeShaderVerificationPlane plane;
    WholeShaderPlaneStatus status;
    bool complete;
    uint8_t subject_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    char* producer;
    uint32_t producer_version;
    uint8_t authority_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint64_t expected_item_count;
    uint64_t observed_item_count;
    uint64_t matched_item_count;
    uint32_t reason_code;
    uint8_t reason_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    WholeShaderEvidenceComparisonItem* items;
    size_t item_count;
    uint8_t evidence_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t coverage_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
};

static const uint8_t evidence_magic[8] = {
    'D', 'X', 'W', 'S', 'E', 'V', 'I', 'D'
};

static const uint8_t coverage_magic[8] = {
    'D', 'X', 'W', 'S', 'C', 'O', 'V', '1'
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

static bool size_multiply(size_t left, size_t right, size_t* output) {
    if (!output || (left != 0U && right > SIZE_MAX / left)) return false;
    *output = left * right;
    return true;
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

static int compare_digest(const void* left, const void* right) {
    return memcmp(left, right, WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
}

static bool valid_plane(WholeShaderVerificationPlane plane) {
    return plane >= WHOLE_SHADER_PLANE_STRUCTURAL &&
        plane < WHOLE_SHADER_PLANE_COUNT;
}

static WholeShaderEvidenceStatus prepare_base(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    WholeShaderVerificationPlane plane, const char* producer,
    uint32_t producer_version,
    const uint8_t authority_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE]) {
    if (!out_evidence) return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    *out_evidence = NULL;
    if (!subject || !producer || !authority_digest) {
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    if (!valid_plane(plane)) return WHOLE_SHADER_EVIDENCE_INVALID_PLANE;
    size_t producer_size = 0U;
    if (!bounded_string_size(producer,
                             WHOLE_SHADER_EVIDENCE_MAX_PRODUCER_BYTES,
                             &producer_size) ||
        producer_size == 0U || producer_version == 0U) {
        return WHOLE_SHADER_EVIDENCE_INVALID_PRODUCER;
    }
    WholeShaderEvidence* evidence =
        (WholeShaderEvidence*)calloc(1U, sizeof(*evidence));
    if (!evidence) return WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED;
    evidence->producer = (char*)malloc(producer_size + 1U);
    if (!evidence->producer) {
        free(evidence);
        return WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED;
    }
    memcpy(evidence->producer, producer, producer_size + 1U);
    evidence->plane = plane;
    evidence->producer_version = producer_version;
    memcpy(evidence->authority_digest, authority_digest,
           sizeof(evidence->authority_digest));
    const WholeShaderSubjectStatus subject_status =
        whole_shader_subject_digest(subject, evidence->subject_digest);
    if (subject_status != WHOLE_SHADER_SUBJECT_OK) {
        whole_shader_evidence_free(evidence);
        if (subject_status == WHOLE_SHADER_SUBJECT_ALLOCATION_FAILED) {
            return WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED;
        }
        if (subject_status == WHOLE_SHADER_SUBJECT_SIZE_OVERFLOW) {
            return WHOLE_SHADER_EVIDENCE_SIZE_OVERFLOW;
        }
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    *out_evidence = evidence;
    return WHOLE_SHADER_EVIDENCE_OK;
}

static WholeShaderEvidenceStatus finalize_digest(
    WholeShaderEvidence* evidence) {
    uint8_t* data = NULL;
    size_t size = 0U;
    const WholeShaderEvidenceStatus status =
        whole_shader_evidence_serialize(evidence, &data, &size);
    if (status != WHOLE_SHADER_EVIDENCE_OK) return status;
    common_sha256(data, size, evidence->evidence_digest);
    free(data);
    return WHOLE_SHADER_EVIDENCE_OK;
}

WholeShaderEvidenceStatus whole_shader_evidence_create_comparison(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    const WholeShaderComparisonEvidenceDescriptor* descriptor) {
    if (!out_evidence) return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    *out_evidence = NULL;
    if (!descriptor || !descriptor->items) {
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    if (descriptor->item_count == 0U ||
        descriptor->item_count > WHOLE_SHADER_EVIDENCE_MAX_ITEMS) {
        return WHOLE_SHADER_EVIDENCE_INVALID_COUNT;
    }
    WholeShaderEvidence* evidence = NULL;
    WholeShaderEvidenceStatus status = prepare_base(
        &evidence, subject, descriptor->plane, descriptor->producer,
        descriptor->producer_version, descriptor->authority_digest);
    if (status != WHOLE_SHADER_EVIDENCE_OK) return status;

    size_t items_size = 0U;
    size_t identities_size = 0U;
    if (!size_multiply(descriptor->item_count, sizeof(*evidence->items),
                       &items_size) ||
        !size_multiply(descriptor->item_count,
                       WHOLE_SHADER_EVIDENCE_DIGEST_SIZE,
                       &identities_size)) {
        whole_shader_evidence_free(evidence);
        return WHOLE_SHADER_EVIDENCE_SIZE_OVERFLOW;
    }
    evidence->items = (WholeShaderEvidenceComparisonItem*)malloc(items_size);
    uint8_t* identities = (uint8_t*)malloc(identities_size);
    if (!evidence->items || !identities) {
        free(identities);
        whole_shader_evidence_free(evidence);
        return WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED;
    }
    memcpy(evidence->items, descriptor->items, items_size);
    for (size_t index = 0U; index < descriptor->item_count; ++index) {
        memcpy(identities + index * WHOLE_SHADER_EVIDENCE_DIGEST_SIZE,
               descriptor->items[index].identity_digest,
               WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
    }
    qsort(identities, descriptor->item_count,
          WHOLE_SHADER_EVIDENCE_DIGEST_SIZE, compare_digest);
    for (size_t index = 1U; index < descriptor->item_count; ++index) {
        if (memcmp(identities + (index - 1U) *
                       WHOLE_SHADER_EVIDENCE_DIGEST_SIZE,
                   identities + index * WHOLE_SHADER_EVIDENCE_DIGEST_SIZE,
                   WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) == 0) {
            free(identities);
            whole_shader_evidence_free(evidence);
            return WHOLE_SHADER_EVIDENCE_DUPLICATE_ITEM;
        }
    }
    free(identities);

    evidence->complete = true;
    evidence->item_count = descriptor->item_count;
    evidence->expected_item_count = descriptor->item_count;
    evidence->observed_item_count = descriptor->item_count;
    for (size_t index = 0U; index < descriptor->item_count; ++index) {
        if (memcmp(descriptor->items[index].expected_digest,
                   descriptor->items[index].observed_digest,
                   WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) == 0) {
            ++evidence->matched_item_count;
        }
    }
    evidence->status =
        evidence->matched_item_count == evidence->expected_item_count
            ? WHOLE_SHADER_PLANE_PASS
            : WHOLE_SHADER_PLANE_FAIL;
    CommonSha256Context coverage;
    common_sha256_init(&coverage);
    common_sha256_update(&coverage, coverage_magic, sizeof(coverage_magic));
    uint8_t count_bytes[8];
    uint8_t* count_cursor = count_bytes;
    write_u64_le(&count_cursor, (uint64_t)evidence->item_count);
    common_sha256_update(&coverage, count_bytes, sizeof(count_bytes));
    for (size_t index = 0U; index < evidence->item_count; ++index) {
        common_sha256_update(&coverage,
                             evidence->items[index].identity_digest,
                             WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
    }
    common_sha256_final(&coverage, evidence->coverage_digest);
    status = finalize_digest(evidence);
    if (status != WHOLE_SHADER_EVIDENCE_OK) {
        whole_shader_evidence_free(evidence);
        return status;
    }
    *out_evidence = evidence;
    return WHOLE_SHADER_EVIDENCE_OK;
}

static WholeShaderEvidenceStatus create_nonpass(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    const WholeShaderNonpassEvidenceDescriptor* descriptor,
    WholeShaderPlaneStatus plane_status) {
    if (!out_evidence) return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    *out_evidence = NULL;
    if (!descriptor) return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    if (descriptor->observed_item_count > descriptor->expected_item_count ||
        descriptor->reason_code == 0U) {
        return WHOLE_SHADER_EVIDENCE_INVALID_COUNT;
    }
    WholeShaderEvidence* evidence = NULL;
    WholeShaderEvidenceStatus status = prepare_base(
        &evidence, subject, descriptor->plane, descriptor->producer,
        descriptor->producer_version, descriptor->authority_digest);
    if (status != WHOLE_SHADER_EVIDENCE_OK) return status;
    evidence->status = plane_status;
    evidence->complete = false;
    evidence->expected_item_count = descriptor->expected_item_count;
    evidence->observed_item_count = descriptor->observed_item_count;
    evidence->reason_code = descriptor->reason_code;
    memcpy(evidence->reason_digest, descriptor->reason_digest,
           sizeof(evidence->reason_digest));
    status = finalize_digest(evidence);
    if (status != WHOLE_SHADER_EVIDENCE_OK) {
        whole_shader_evidence_free(evidence);
        return status;
    }
    *out_evidence = evidence;
    return WHOLE_SHADER_EVIDENCE_OK;
}

WholeShaderEvidenceStatus whole_shader_evidence_create_unavailable(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    const WholeShaderNonpassEvidenceDescriptor* descriptor) {
    return create_nonpass(out_evidence, subject, descriptor,
                          WHOLE_SHADER_PLANE_UNAVAILABLE);
}

WholeShaderEvidenceStatus whole_shader_evidence_create_not_run(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    const WholeShaderNonpassEvidenceDescriptor* descriptor) {
    return create_nonpass(out_evidence, subject, descriptor,
                          WHOLE_SHADER_PLANE_NOT_RUN);
}

void whole_shader_evidence_free(WholeShaderEvidence* evidence) {
    if (!evidence) return;
    free(evidence->producer);
    free(evidence->items);
    memset(evidence, 0, sizeof(*evidence));
    free(evidence);
}

WholeShaderEvidenceStatus whole_shader_evidence_describe(
    const WholeShaderEvidence* evidence,
    WholeShaderEvidenceSummary* out_summary) {
    if (!evidence || !out_summary) {
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->plane = evidence->plane;
    out_summary->status = evidence->status;
    out_summary->complete = evidence->complete;
    memcpy(out_summary->subject_digest, evidence->subject_digest,
           sizeof(out_summary->subject_digest));
    memcpy(out_summary->evidence_digest, evidence->evidence_digest,
           sizeof(out_summary->evidence_digest));
    memcpy(out_summary->coverage_digest, evidence->coverage_digest,
           sizeof(out_summary->coverage_digest));
    out_summary->expected_item_count = evidence->expected_item_count;
    out_summary->observed_item_count = evidence->observed_item_count;
    out_summary->matched_item_count = evidence->matched_item_count;
    return WHOLE_SHADER_EVIDENCE_OK;
}

static bool comparison_items_equal(const WholeShaderEvidence* left,
                                   const WholeShaderEvidence* right) {
    if (!left || !right || left->item_count != right->item_count) {
        return false;
    }
    for (size_t index = 0U; index < left->item_count; ++index) {
        if (memcmp(left->items[index].identity_digest,
                   right->items[index].identity_digest,
                   WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) != 0 ||
            memcmp(left->items[index].expected_digest,
                   right->items[index].expected_digest,
                   WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) != 0 ||
            memcmp(left->items[index].observed_digest,
                   right->items[index].observed_digest,
                   WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) != 0) {
            return false;
        }
    }
    return true;
}

bool whole_shader_evidence_equal(const WholeShaderEvidence* left,
                                 const WholeShaderEvidence* right) {
    if (!left || !right) return false;
    if (left == right) return true;
    return memcmp(left->evidence_digest, right->evidence_digest,
                  WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) == 0 &&
        left->plane == right->plane && left->status == right->status &&
        left->complete == right->complete &&
        left->producer_version == right->producer_version &&
        left->expected_item_count == right->expected_item_count &&
        left->observed_item_count == right->observed_item_count &&
        left->matched_item_count == right->matched_item_count &&
        left->reason_code == right->reason_code &&
        left->item_count == right->item_count &&
        strcmp(left->producer, right->producer) == 0 &&
        memcmp(left->subject_digest, right->subject_digest,
               WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) == 0 &&
        memcmp(left->authority_digest, right->authority_digest,
               WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) == 0 &&
        memcmp(left->coverage_digest, right->coverage_digest,
               WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) == 0 &&
        memcmp(left->reason_digest, right->reason_digest,
               WHOLE_SHADER_EVIDENCE_DIGEST_SIZE) == 0 &&
        comparison_items_equal(left, right);
}

WholeShaderEvidenceStatus whole_shader_evidence_serialize(
    const WholeShaderEvidence* evidence, uint8_t** out_data,
    size_t* out_size) {
    if (!out_data || !out_size) {
        return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    }
    *out_data = NULL;
    *out_size = 0U;
    if (!evidence) return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    const size_t producer_size = strlen(evidence->producer);
    size_t item_bytes = 0U;
    if (!size_multiply(evidence->item_count,
                       3U * WHOLE_SHADER_EVIDENCE_DIGEST_SIZE,
                       &item_bytes)) {
        return WHOLE_SHADER_EVIDENCE_SIZE_OVERFLOW;
    }
    size_t total = 8U + 4U + 8U + 4U + 4U + 4U +
        WHOLE_SHADER_EVIDENCE_DIGEST_SIZE + 4U + 4U +
        WHOLE_SHADER_EVIDENCE_DIGEST_SIZE + 3U * 8U + 4U +
        WHOLE_SHADER_EVIDENCE_DIGEST_SIZE + 8U;
    if (!size_add(&total, producer_size) || !size_add(&total, item_bytes) ||
        total > UINT64_MAX) {
        return WHOLE_SHADER_EVIDENCE_SIZE_OVERFLOW;
    }
    uint8_t* output = (uint8_t*)malloc(total);
    if (!output) return WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED;
    uint8_t* cursor = output;
    write_bytes(&cursor, evidence_magic, sizeof(evidence_magic));
    write_u32_le(&cursor, WHOLE_SHADER_EVIDENCE_FORMAT_VERSION);
    write_u64_le(&cursor, (uint64_t)total);
    write_u32_le(&cursor, (uint32_t)evidence->plane);
    write_u32_le(&cursor, (uint32_t)evidence->status);
    write_u32_le(&cursor, evidence->complete ? 1U : 0U);
    write_bytes(&cursor, evidence->subject_digest,
                WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
    write_u32_le(&cursor, (uint32_t)producer_size);
    write_bytes(&cursor, evidence->producer, producer_size);
    write_u32_le(&cursor, evidence->producer_version);
    write_bytes(&cursor, evidence->authority_digest,
                WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
    write_u64_le(&cursor, evidence->expected_item_count);
    write_u64_le(&cursor, evidence->observed_item_count);
    write_u64_le(&cursor, evidence->matched_item_count);
    write_u32_le(&cursor, evidence->reason_code);
    write_bytes(&cursor, evidence->reason_digest,
                WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
    write_u64_le(&cursor, (uint64_t)evidence->item_count);
    for (size_t index = 0U; index < evidence->item_count; ++index) {
        write_bytes(&cursor, evidence->items[index].identity_digest,
                    WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
        write_bytes(&cursor, evidence->items[index].expected_digest,
                    WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
        write_bytes(&cursor, evidence->items[index].observed_digest,
                    WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
    }
    if ((size_t)(cursor - output) != total) {
        free(output);
        return WHOLE_SHADER_EVIDENCE_SIZE_OVERFLOW;
    }
    *out_data = output;
    *out_size = total;
    return WHOLE_SHADER_EVIDENCE_OK;
}

void whole_shader_evidence_serialized_free(uint8_t* data) {
    free(data);
}

WholeShaderEvidenceStatus whole_shader_evidence_digest(
    const WholeShaderEvidence* evidence,
    uint8_t digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE]) {
    if (!evidence || !digest) return WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT;
    memcpy(digest, evidence->evidence_digest,
           WHOLE_SHADER_EVIDENCE_DIGEST_SIZE);
    return WHOLE_SHADER_EVIDENCE_OK;
}

const char* whole_shader_evidence_status_name(
    WholeShaderEvidenceStatus status) {
    switch (status) {
        case WHOLE_SHADER_EVIDENCE_OK: return "ok";
        case WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT:
            return "invalid-argument";
        case WHOLE_SHADER_EVIDENCE_INVALID_PLANE: return "invalid-plane";
        case WHOLE_SHADER_EVIDENCE_INVALID_PRODUCER:
            return "invalid-producer";
        case WHOLE_SHADER_EVIDENCE_INVALID_COUNT: return "invalid-count";
        case WHOLE_SHADER_EVIDENCE_DUPLICATE_ITEM: return "duplicate-item";
        case WHOLE_SHADER_EVIDENCE_SIZE_OVERFLOW: return "size-overflow";
        case WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED:
            return "allocation-failed";
    }
    return "unknown";
}
