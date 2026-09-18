// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_CBUFFER_PROJECTION_H
#define DXBC_CBUFFER_PROJECTION_H

#include "translation/usil.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Constant-buffer bytecode is a flat array of 16-byte registers.  Variable
 * names and layouts are separate authority.  This module deliberately keeps
 * those two facts separate: it first computes the bytes addressed by DXBC,
 * then projects those bytes onto caller-supplied, non-overlapping variable
 * ranges.
 *
 * A relative cbuffer index has no recoverable range in the operand token.
 * Such an access is projected conservatively over every declared register and
 * returns DXBC_CBUFFER_PROJECTION_REQUIRES_INDEX_AUTHORITY.  Callers seeking
 * exact source-level declarations must not silently treat that result as
 * exact; they need a proven index range or preserved compiler/source
 * authority.
 */

typedef enum DXBCCBufferProjectionStatus {
    DXBC_CBUFFER_PROJECTION_EXACT = 0,
    DXBC_CBUFFER_PROJECTION_REQUIRES_INDEX_AUTHORITY = 1,
    DXBC_CBUFFER_PROJECTION_INVALID = 2
} DXBCCBufferProjectionStatus;

typedef struct DXBCCBufferVariableRange {
    uint32_t byte_offset;
    uint32_t byte_size;
} DXBCCBufferVariableRange;

typedef struct DXBCCBufferVariableUse {
    bool referenced;
    bool dynamically_addressed;
    /* Physical x/y/z/w lanes observed for this variable. */
    uint8_t component_mask;
} DXBCCBufferVariableUse;

typedef struct DXBCCBufferProjection {
    bool saw_access;
    bool saw_dynamic_access;
    bool saw_padding_access;
    /* Keep static and conservatively-expanded dynamic padding distinct.  A
     * dynamic operand is projected over every row because DXBC carries no
     * index range; that must not make a proven static padding read
     * indistinguishable from a merely possible dynamic one. */
    bool saw_static_padding_access;
    bool saw_dynamic_padding_access;
} DXBCCBufferProjection;

/* Clears the aggregate result and variable-use array. */
bool dxbc_cbuffer_projection_reset(DXBCCBufferProjection *projection,
                                   DXBCCBufferVariableUse *uses,
                                   size_t variable_count);

/*
 * Projects one cbuffer source operand.  logical_lane_mask uses bits 0..3 for
 * the operand result lanes consumed by its instruction.  Swizzling is applied
 * by this function, so uses[].component_mask contains physical cbuffer lanes.
 * Operands for other cbuffer registers are ignored and return EXACT.
 */
DXBCCBufferProjectionStatus dxbc_cbuffer_project_operand(
    const DXBCOperand *operand, uint8_t logical_lane_mask,
    uint32_t cbuffer_register, uint32_t cbuffer_size_bytes,
    const DXBCCBufferVariableRange *variables, size_t variable_count,
    DXBCCBufferVariableUse *uses, DXBCCBufferProjection *projection);

/*
 * Projects every cbuffer read in a USIL program, including cbuffer operands
 * nested inside relative-address expressions.  The supported USIL instruction
 * set is exhaustively classified; malformed operand roles fail closed.
 */
DXBCCBufferProjectionStatus dxbc_cbuffer_project_program(
    const USILProgram *program, uint32_t cbuffer_register,
    uint32_t cbuffer_size_bytes,
    const DXBCCBufferVariableRange *variables, size_t variable_count,
    DXBCCBufferVariableUse *uses, DXBCCBufferProjection *projection);

#endif
