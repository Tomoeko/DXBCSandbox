// SPDX-License-Identifier: GPL-3.0-only

#ifndef WHOLE_SHADER_EVIDENCE_H
#define WHOLE_SHADER_EVIDENCE_H

#include "app/whole_shader_certificate.h"

#include <stddef.h>

#define WHOLE_SHADER_EVIDENCE_FORMAT_VERSION 1U
#define WHOLE_SHADER_EVIDENCE_DIGEST_SIZE WHOLE_SHADER_CERTIFICATE_DIGEST_SIZE
#define WHOLE_SHADER_EVIDENCE_MAX_PRODUCER_BYTES UINT32_C(4096)
#define WHOLE_SHADER_EVIDENCE_MAX_ITEMS UINT32_C(1048576)

/* Ordered semantic item. Identity digests must be unique within one plane.
 * Producers canonicalize semantically unordered domains before this API. */
typedef struct {
    uint8_t identity_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t expected_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t observed_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
} WholeShaderEvidenceComparisonItem;

typedef struct {
    WholeShaderVerificationPlane plane;
    const char* producer;
    uint32_t producer_version;
    uint8_t authority_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    const WholeShaderEvidenceComparisonItem* items;
    size_t item_count;
} WholeShaderComparisonEvidenceDescriptor;

typedef struct {
    WholeShaderVerificationPlane plane;
    const char* producer;
    uint32_t producer_version;
    uint8_t authority_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint64_t expected_item_count;
    uint64_t observed_item_count;
    uint32_t reason_code;
    uint8_t reason_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
} WholeShaderNonpassEvidenceDescriptor;

typedef struct {
    WholeShaderVerificationPlane plane;
    WholeShaderPlaneStatus status;
    bool complete;
    uint8_t subject_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t evidence_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    /* Plane-independent digest of ordered identity_digest values. D3D domain,
     * container, diagnostics, and reflection producers use the same canonical
     * compile-target identities; runtime-input and pixel producers use the
     * same render-case identities. The certificate rejects split scope. */
    uint8_t coverage_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint64_t expected_item_count;
    uint64_t observed_item_count;
    uint64_t matched_item_count;
} WholeShaderEvidenceSummary;

typedef enum {
    WHOLE_SHADER_EVIDENCE_OK = 0,
    WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT,
    WHOLE_SHADER_EVIDENCE_INVALID_PLANE,
    WHOLE_SHADER_EVIDENCE_INVALID_PRODUCER,
    WHOLE_SHADER_EVIDENCE_INVALID_COUNT,
    WHOLE_SHADER_EVIDENCE_DUPLICATE_ITEM,
    WHOLE_SHADER_EVIDENCE_SIZE_OVERFLOW,
    WHOLE_SHADER_EVIDENCE_ALLOCATION_FAILED
} WholeShaderEvidenceStatus;

/* PASS/FAIL and counts are derived by comparing every expected/observed
 * digest. Callers cannot provide those result fields. */
WholeShaderEvidenceStatus whole_shader_evidence_create_comparison(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    const WholeShaderComparisonEvidenceDescriptor* descriptor);
WholeShaderEvidenceStatus whole_shader_evidence_create_unavailable(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    const WholeShaderNonpassEvidenceDescriptor* descriptor);
WholeShaderEvidenceStatus whole_shader_evidence_create_not_run(
    WholeShaderEvidence** out_evidence, const WholeShaderSubject* subject,
    const WholeShaderNonpassEvidenceDescriptor* descriptor);

void whole_shader_evidence_free(WholeShaderEvidence* evidence);
WholeShaderEvidenceStatus whole_shader_evidence_describe(
    const WholeShaderEvidence* evidence,
    WholeShaderEvidenceSummary* out_summary);
bool whole_shader_evidence_equal(const WholeShaderEvidence* left,
                                 const WholeShaderEvidence* right);
/* Stable little-endian `DXWSEVID` v1 encoding. Scalar status/count fields are
 * internally derived. Producer is u32-length-prefixed; comparison items are
 * emitted in supplied canonical order as three explicit 32-byte digests. */
WholeShaderEvidenceStatus whole_shader_evidence_serialize(
    const WholeShaderEvidence* evidence, uint8_t** out_data,
    size_t* out_size);
void whole_shader_evidence_serialized_free(uint8_t* data);
WholeShaderEvidenceStatus whole_shader_evidence_digest(
    const WholeShaderEvidence* evidence,
    uint8_t digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE]);

const char* whole_shader_evidence_status_name(
    WholeShaderEvidenceStatus status);

#endif /* WHOLE_SHADER_EVIDENCE_H */
