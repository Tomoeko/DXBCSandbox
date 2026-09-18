// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>

void emit_conversion_op(HLSLEmitterContext* ctx, const USILInstruction* inst,
                                bool isInt, bool isUint,
                                int comp, int dm, int format_mask, int current_dest_mask,
                                const char* dest,
                                const char* src0, const char* src1, const char* src2,
                                const char* src3, const char* src4,
                                char* line_buf, size_t line_buf_sz,
                                char* rhs_expr, size_t rhs_len, bool* is_custom, bool* wrap_swizzle) {
    (void)isInt;
    (void)isUint;
    (void)comp;
    (void)dm;
    (void)format_mask;
    (void)current_dest_mask;
    (void)dest;
    (void)src1;
    (void)src2;
    (void)src3;
    (void)src4;
    (void)line_buf;
    (void)line_buf_sz;
    (void)is_custom;
    (void)wrap_swizzle;

    HLSLTypedLoopBoundPhase typed_bound_phase =
        HLSL_TYPED_LOOP_BOUND_NONE;
    const LoopOptimizationInfo *typed_bound =
        hlsl_typed_loop_bound_for_instruction(
            ctx, ctx->current_instruction_index, &typed_bound_phase);

    switch (inst->opcode) {
      case USIL_OP_FTOI: {
        if (typed_bound &&
            typed_bound_phase == HLSL_TYPED_LOOP_BOUND_FTOI) {
          sb_appendf(ctx->sb,
                     "#ifdef %s\n"
                     "#error DXBCSandbox_typed_loop_witness_macro_collision_%s\n"
                     "#endif\n",
                     typed_bound->typed_bound_alias,
                     typed_bound->typed_bound_alias);
          sb_append_spaces(ctx->sb, ctx->indent);
          sb_appendf(ctx->sb, "int %s = (int)(%s);\n",
                     typed_bound->typed_bound_alias, src0);
          sb_append_spaces(ctx->sb, ctx->indent);
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                              "asfloat(%s)",
                              typed_bound->typed_bound_alias);
          break;
        }
        int comps = get_mask_component_count(inst->operands[0].destination_mask);
        const char *cast_t = get_cast_type_str("int", comps);
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "(%s)(%s)", cast_t, src0);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat((%s)(%s))", cast_t, src0);
        }
        break;
      }
      case USIL_OP_FTOU: {
        int comps = get_mask_component_count(inst->operands[0].destination_mask);
        const char *cast_t = get_cast_type_str("uint", comps);
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "(%s)(%s)", cast_t, src0);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat((%s)(%s))", cast_t, src0);
        }
        break;
      }
      case USIL_OP_ITOF: {
        if (typed_bound &&
            typed_bound_phase == HLSL_TYPED_LOOP_BOUND_ITOF) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                              "(float)(%s)",
                              typed_bound->typed_bound_alias);
          break;
        }
        int comps = get_mask_component_count(inst->operands[0].destination_mask);
        const char *cast_t = get_cast_type_str("float", comps);
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "(%s)(%s)", cast_t, src0);
        break;
      }
      case USIL_OP_UTOF: {
        int comps = get_mask_component_count(inst->operands[0].destination_mask);
        const char *cast_t = get_cast_type_str("float", comps);
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "(%s)(%s)", cast_t, src0);
        break;
      }
      default:
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                   HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
                                   ctx->current_instruction_index, -1);
        break;
    }
}

