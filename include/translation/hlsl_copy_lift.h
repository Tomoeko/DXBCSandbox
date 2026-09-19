// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_COPY_LIFT_H
#define HLSL_COPY_LIFT_H

#include "translation/usil.h"

#define HLSL_COPY_LIFT_ID "copy-swizzle"
#define HLSL_COPY_LIFT_VERSION 1U
#define HLSL_RESULT_LIFT_ID "single-use-result"
#define HLSL_RESULT_LIFT_VERSION 1U

typedef enum {
    HLSL_COPY_LIFT_OK = 0,
    HLSL_COPY_LIFT_INVALID_PROGRAM,
    HLSL_COPY_LIFT_UNSUPPORTED_STAGE,
    HLSL_COPY_LIFT_NOT_PLAIN_COPY,
    HLSL_COPY_LIFT_PRECISION_CONTROL,
    HLSL_COPY_LIFT_EFFECTFUL_REGION,
    HLSL_COPY_LIFT_RELATIVE_ADDRESSING,
    HLSL_COPY_LIFT_PARTIAL_USE,
    HLSL_COPY_LIFT_UNDEFINED_SOURCE,
    HLSL_COPY_LIFT_SOURCE_CHANGED,
    HLSL_COPY_LIFT_NO_USES,
    HLSL_COPY_LIFT_NOT_PLAIN_RESULT,
    HLSL_COPY_LIFT_MULTIPLE_USES,
    HLSL_COPY_LIFT_NONBIJECTIVE_LANES,
    HLSL_COPY_LIFT_OUT_OF_MEMORY
} HLSLCopyLiftStatus;

typedef struct {
    int instruction_index;
    int operand_index;
    uint8_t logical_lane_mask;
    uint8_t source_components[4];
} HLSLCopyLiftEdit;

typedef struct HLSLCopyLift HLSLCopyLift;

/* Plan one versioned transformation on emitter-validated, immutable USIL.
 * The caller must retain the original metadata validation; this pass checks
 * additional local lifting preconditions, not every shader ABI invariant.
 * The admitted
 * region is straight-line SM4/5 vertex/fragment value operations. A plain MOV
 * copies 32-bit lanes without conversion; every rewritten use must consume
 * only its defined lanes while the original source still has the same value.
 * No arithmetic or observable effect is moved, duplicated, or reassociated.
 *
 * The candidate borrows all baseline metadata and nested operand storage;
 * baseline must outlive it and remain immutable. Never pass its program view
 * to usil_free(). Destroying the candidate is the complete rollback operation.
 * This is a local precondition proof, NOT a compiler or shader certificate. */
HLSLCopyLiftStatus hlsl_copy_lift_create(const USILProgram *baseline, int copy_instruction,
                                         HLSLCopyLift **out_candidate);
/* Eliminate a MOV by forwarding its immediately preceding component-wise
 * ADD/MUL/AND/OR/XOR result into the MOV destination. NOPs may intervene, but
 * no executable operation is crossed. Every producer lane must be consumed
 * exactly once, only by this copy. This preserves a single 32-bit operation
 * per lane without nesting, duplication, reassociation or changed rounding
 * boundaries. Modified/precise/minimum-precision operands reject. The same
 * ownership and compiler-verification requirements as create() apply.
 * Edits describe the rewritten producer operands at the copy's position;
 * producer_instruction identifies the elided operation's original position. */
HLSLCopyLiftStatus hlsl_result_lift_create(const USILProgram *baseline, int copy_instruction,
                                           HLSLCopyLift **out_candidate);
int hlsl_copy_lift_producer_instruction(const HLSLCopyLift *candidate);
const USILProgram *hlsl_copy_lift_program(const HLSLCopyLift *candidate);
const HLSLCopyLiftEdit *hlsl_copy_lift_edits(const HLSLCopyLift *candidate, size_t *out_count);
int hlsl_copy_lift_instruction(const HLSLCopyLift *candidate);
void hlsl_copy_lift_destroy(HLSLCopyLift *candidate);
const char *hlsl_copy_lift_status_name(HLSLCopyLiftStatus status);

#endif
