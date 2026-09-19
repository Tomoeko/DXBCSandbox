// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_LIFT_TRANSACTION_H
#define HLSL_LIFT_TRANSACTION_H

#include "dxbc/dxbc_compare.h"
#include "translation/hlsl_copy_lift.h"

typedef enum {
    HLSL_LIFT_VERIFIED = 0,
    HLSL_LIFT_PRECONDITION_REJECTED,
    HLSL_LIFT_EMISSION_REJECTED,
    HLSL_LIFT_COMPILER_REJECTED,
    HLSL_LIFT_COMPILER_UNAVAILABLE,
    HLSL_LIFT_CANCELLED,
    HLSL_LIFT_BUDGET_EXHAUSTED,
    HLSL_LIFT_DXBC_MISMATCH,
    HLSL_LIFT_INVALID_DXBC,
    HLSL_LIFT_INVALID_ARGUMENT,
    HLSL_LIFT_OUT_OF_MEMORY,
    HLSL_LIFT_CLOCK_UNAVAILABLE
} HLSLLiftStatus;

/* The compile service transfers malloc-owned source and one complete released
 * DXBC container, including on failure. It must compile this exact source from
 * the supplied program under immutable request controls. A cache hit has the
 * same compiler authority requirements as a live result. VERIFIED here only
 * means clean compilation; the transaction independently compares the bytes.
 * The service must enforce its own bounded I/O timeout and cancellation. */
typedef struct {
    char *source;
    uint8_t *dxbc;
    size_t dxbc_size;
    bool cache_hit;
} HLSLLiftArtifact;

typedef struct {
    HLSLLiftStatus (*compile)(void *context, const USILProgram *program, uint64_t remaining_ms,
                              HLSLLiftArtifact *artifact);
    bool (*monotonic_ms)(void *context, uint64_t *milliseconds);
    bool (*cancelled)(void *context); /* Optional; checked at work boundaries. */
    void *context;
} HLSLLiftServices;

typedef struct {
    size_t max_candidates;
    size_t max_compiles; /* Includes the mandatory baseline compilation. */
    /* Admission and acceptance deadline. An in-flight service has its own
     * bounded I/O timeout; a late response is discarded, never accepted. */
    uint64_t max_elapsed_ms;
} HLSLLiftLimits;

typedef struct {
    HLSLLiftStatus status;
    HLSLCopyLiftStatus precondition;
    DXBCCompareResult comparison;
    bool compared;
    bool cache_hit;
    char source_sha256[65];
    char output_sha256[65];
} HLSLLiftResult;

typedef struct {
    size_t candidates;
    size_t compiles;
    size_t cache_hits;
    size_t accepted;
    uint64_t elapsed_ms;
} HLSLLiftStats;

typedef struct HLSLLiftTransaction HLSLLiftTransaction;

/* Establishes a baseline by emission, compilation and full-container equality.
 * No transaction is returned unless that gate passes. The immutable baseline
 * must outlive the transaction; target bytes are copied. Services/context must
 * remain valid. This evidence is for this one container and these compiler
 * controls, not a whole variant domain or ShaderLab semantic certificate. */
HLSLLiftStatus hlsl_lift_transaction_begin(const USILProgram *baseline, const uint8_t *target,
                                           size_t target_size, const HLSLLiftServices *services,
                                           const HLSLLiftLimits *limits,
                                           HLSLLiftTransaction **out_transaction,
                                           HLSLLiftResult *result);

/* Plans against the last accepted program. Every unsuccessful attempt keeps
 * the same accepted program, source and bytes. Overlapping prior copies are
 * composed by re-proving all preconditions on the current program. A removed
 * copy is a NOP and cannot be claimed twice. Budget exhaustion is terminal. */
HLSLLiftStatus hlsl_lift_transaction_try_copy(HLSLLiftTransaction *transaction, int instruction,
                                              HLSLLiftResult *result);

const USILProgram *hlsl_lift_transaction_program(const HLSLLiftTransaction *transaction);
const HLSLLiftArtifact *hlsl_lift_transaction_artifact(const HLSLLiftTransaction *transaction);
void hlsl_lift_transaction_stats(const HLSLLiftTransaction *transaction, HLSLLiftStats *stats);
void hlsl_lift_transaction_destroy(HLSLLiftTransaction *transaction);
const char *hlsl_lift_status_name(HLSLLiftStatus status);

#endif
