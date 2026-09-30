// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMPUTE_SOURCE_CANDIDATE_H
#define COMPUTE_SOURCE_CANDIDATE_H

#include "common/sha256.h"
#include "dxbc/dxbc_stage_contract.h"
#include "io/compute_shader_object.h"
#include "translation/hlsl_source_quality.h"

/* Explicit analysis bounds. Rejected inputs remain requested; these limits
 * cannot turn missing/unrepresented variants into successful coverage. */
enum {
    COMPUTE_SOURCE_MAX_KEYWORDS = 8,
    COMPUTE_SOURCE_MAX_KERNELS = 32,
    COMPUTE_SOURCE_MAX_VARIANTS = 4096,
    COMPUTE_SOURCE_MAX_INSTRUCTIONS = 64,
    COMPUTE_SOURCE_MAX_BYTES = 8 * 1024 * 1024
};

typedef enum {
    COMPUTE_SOURCE_CANDIDATE_UNVERIFIED = 0,
    COMPUTE_SOURCE_INVALID_ARGUMENT,
    COMPUTE_SOURCE_LAYOUT_UNAVAILABLE,
    COMPUTE_SOURCE_LIMIT_EXCEEDED,
    COMPUTE_SOURCE_PLATFORM_UNSUPPORTED,
    COMPUTE_SOURCE_NAME_UNREPRESENTABLE,
    COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE,
    COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE,
    COMPUTE_SOURCE_REQUIREMENTS_UNSUPPORTED,
    COMPUTE_SOURCE_DXBC_UNAVAILABLE,
    COMPUTE_SOURCE_STAGE_CONTRACT_FAILED,
    COMPUTE_SOURCE_THREAD_GROUP_MISMATCH,
    COMPUTE_SOURCE_BODY_UNSUPPORTED,
    COMPUTE_SOURCE_EMISSION_FAILED,
    COMPUTE_SOURCE_ALLOCATION_FAILED,
    COMPUTE_SOURCE_QUALITY_FAILED
} ComputeSourceStatus;

typedef struct {
    ComputeSourceStatus status;
    size_t platform_index;
    size_t kernel_index;
    size_t variant_index;
    size_t requested_kernels;
    size_t requested_variants;
    size_t examined_variants;
    size_t represented_variants;
    bool requested_counts_known;
    DXBCDocumentDiagnostic document;
    DXBCStageContractDiagnostic stage_contract;
    HLSLEmitDiagnostic emission;
} ComputeSourceDiagnostic;

typedef struct {
    size_t platform_index;
    size_t kernel_index;
    size_t variant_index;
    uint32_t source_unit_id;
    /* Owned C strings retain the actual selected model's spelling. */
    char *kernel_name;
    char *keyword_key;
    uint64_t keyword_mask;
    uint64_t requirements;
    uint32_t thread_group_size[3];
    uint8_t serialized_program_sha256[COMMON_SHA256_DIGEST_SIZE];
    uint8_t dxbc_sha256[COMMON_SHA256_DIGEST_SIZE];
    uint8_t source_sha256[COMMON_SHA256_DIGEST_SIZE];
    HLSLSourceQualityResult entry_quality;
    /* Owned effect/syntax ledger. RET/SYNC-only admission never flattens an
     * AST expression into these events or borrows formatter state. */
    HLSLSourceQualityFacts *emission_facts;
    size_t emission_fact_count;
} ComputeSourceVariant;

typedef struct {
    StringBuilder source;
    ComputeSourceVariant *variants;
    size_t variant_count;
    char **keywords;
    size_t global_keyword_count;
    size_t local_keyword_count;
    size_t kernel_count;
    uint8_t serialized_object_sha256[COMMON_SHA256_DIGEST_SIZE];
    /* Binds the canonical selected model as well as its serialized span.
     * Caller-owned decoded fields are not assumed immutable or proven equal
     * to the original serialization merely because decoded is true. */
    uint8_t modeled_input_sha256[COMMON_SHA256_DIGEST_SIZE];
    uint8_t source_sha256[COMMON_SHA256_DIGEST_SIZE];
    HLSLSourceQualityResult source_quality;
    bool domain_complete;
    /* Always CANDIDATE_UNVERIFIED. This API supplies no compiler-equality,
     * semantic, import, native, or generic Class72 EXACT authority. */
    ComputeSourceStatus status;
} ComputeSourceCandidate;

void compute_source_candidate_init(ComputeSourceCandidate *candidate);
void compute_source_candidate_dispose(ComputeSourceCandidate *candidate);

/* Object and all borrowed storage must stay valid and unchanged during this
 * call. Success owns all source/evidence and borrows no object data. Failure
 * leaves the initialized destination unchanged; diagnostics retain requested
 * counts and the first precise unsupported coordinate. Initial source support
 * is one Windows64 D3D11 platform, exhaustive Boolean keyword domains, no
 * resource/shared-memory declarations, and cs5 RET/SYNC-only bodies. */
ComputeSourceStatus compute_source_candidate_build(
    const ComputeShaderObject *object, ComputeSourceCandidate *candidate,
    ComputeSourceDiagnostic *diagnostic);

const char *compute_source_status_name(ComputeSourceStatus status);

#endif
