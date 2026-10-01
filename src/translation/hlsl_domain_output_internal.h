// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_DOMAIN_OUTPUT_INTERNAL_H
#define HLSL_DOMAIN_OUTPUT_INTERNAL_H

#include "translation/usil.h"

enum { HLSL_DOMAIN_OUTPUT_PIECE_COUNT = 2 };

#define HLSL_DOMAIN_OUTPUT_LOGICAL_ID UINT64_C(0x2000000000050000)

/* The bounded producer admits only the consecutive XYZ arithmetic and scalar
 * immediate W writes before RET. Each piece keeps its actual decoded owner;
 * constructing the interface value does not widen either instruction's mask. */
typedef struct {
    int instruction_index;
    uint32_t source_instruction_index;
    USILOpcode opcode;
    uint32_t destination_register, destination_raw_token;
    uint8_t mask, width;
    bool scalar_immediate;
    uint32_t immediate_bits;
} HLSLDomainOutputPiece;

/* Independently owned by value. The complete admitted output semantic fits
 * inline; semantic_name_extended is always NULL. Absent plans are canonical
 * zero, including every signature field, piece and return coordinate. */
typedef struct {
    bool present;
    uint32_t output_signature_index;
    DXBCSignatureElement output;
    HLSLDomainOutputPiece pieces[HLSL_DOMAIN_OUTPUT_PIECE_COUNT];
    int return_instruction_index;
    uint32_t return_source_instruction_index;
} HLSLDomainOutputPlan;

/* Acquisition derives the plan from the actual current parsed program.
 * Equality compares typed fields and complete inline names, never C padding. */
bool hlsl_domain_output_plan_prepare(const USILProgram *program, HLSLDomainOutputPlan *plan);
bool hlsl_domain_output_plans_equal(const HLSLDomainOutputPlan *left, const HLSLDomainOutputPlan *right);

#endif
