// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_EMITTER_OPS_INTERNAL_H
#define HLSL_EMITTER_OPS_INTERNAL_H

#include "translation/hlsl_emitter_internal.h"

typedef void HLSLOperationEmitter(
    HLSLEmitterContext* ctx, const USILInstruction* inst,
    bool is_int, bool is_uint,
    int component, int destination_mask, int format_mask,
    int current_destination_mask, const char* destination,
    const char* source0, const char* source1, const char* source2,
    const char* source3, const char* source4,
    char* line_buffer, size_t line_buffer_size,
    char* rhs_expression, size_t rhs_expression_size,
    bool* is_custom, bool* wrap_swizzle);

HLSLOperationEmitter emit_arithmetic_op;
HLSLOperationEmitter emit_comparison_op;
HLSLOperationEmitter emit_bitwise_op;
HLSLOperationEmitter emit_flow_control_op;
HLSLOperationEmitter emit_texture_op;
HLSLOperationEmitter emit_conversion_op;

#endif
