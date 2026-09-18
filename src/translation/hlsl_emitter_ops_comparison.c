// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>

void emit_comparison_op(HLSLEmitterContext* ctx, const USILInstruction* inst,
                                bool isInt, bool isUint,
                                int comp, int dm, int format_mask, int current_dest_mask,
                                const char* dest,
                                const char* src0, const char* src1, const char* src2,
                                const char* src3, const char* src4,
                                char* line_buf, size_t line_buf_sz,
                                char* rhs_expr, size_t rhs_len, bool* is_custom, bool* wrap_swizzle) {
    StringBuilder* sb = ctx->sb;
    int i = ctx->current_instruction_index;
    (void)comp;
    (void)dm;
    (void)format_mask;
    (void)current_dest_mask;
    (void)src2;
    (void)src3;
    (void)src4;
    (void)line_buf;
    (void)line_buf_sz;
    (void)wrap_swizzle;
    (void)dest;

    switch (inst->opcode) {
      case USIL_OP_LT:
      case USIL_OP_ILT:
      case USIL_OP_ULT: {
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                              "((%s < %s) ? 0xFFFFFFFFu : 0u)", src0, src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                              "asfloat((%s < %s) ? 0xFFFFFFFFu : 0u)", src0,
                              src1);
        }
        break;
      }
      case USIL_OP_GE:
      case USIL_OP_IGE:
      case USIL_OP_UGE: {
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                              "((%s >= %s) ? 0xFFFFFFFFu : 0u)", src0,
                              src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                              "asfloat((%s >= %s) ? 0xFFFFFFFFu : 0u)", src0,
                              src1);
        }
        break;
      }
      case USIL_OP_EQ:
      case USIL_OP_IEQ: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        int dest_m = inst->operands[0].destination_mask;
        bool is_vector_literal = (inst->operands[2].type == OPERAND_TYPE_IMMEDIATE32 && inst->operands[2].imm_value_count > 1);
        if (is_vector_literal && get_mask_component_count(dest_m) > 1) {
            *is_custom = true;
            const char* op_str = "==";
            const DXBCOperand *first_operand =
                swap ? &inst->operands[2] : &inst->operands[1];
            const DXBCOperand *second_operand =
                swap ? &inst->operands[1] : &inst->operands[2];
            for (int c = 0; c < 4; c++) {
                if (dest_m & (16 << c)) {
                    char comp_dest[128];
                    format_dest_operand_hlsl(ctx, &inst->operands[0], false,
                                             ctx->use_uint_temps, 16 << c,
                                             false, comp_dest,
                                             sizeof(comp_dest));
                    char first_source[256];
                    char second_source[256];
                    format_operand_hlsl(ctx, first_operand, isInt, isUint,
                                        16 << c, false, first_source,
                                        sizeof(first_source));
                    format_operand_hlsl(ctx, second_operand, isInt, isUint,
                                        16 << c, false, second_source,
                                        sizeof(second_source));
                    if (ctx->sb->failed) return;
                    sb_append_spaces(sb, ctx->indent);
                    if (ctx->use_uint_temps) {
                        sb_appendf(sb,
                                   "%s = ((%s %s %s) ? 0xFFFFFFFFu : 0u);\n",
                                   comp_dest, first_source, op_str,
                                   second_source);
                    } else {
                        sb_appendf(
                            sb,
                            "%s = asfloat((%s %s %s) ? 0xFFFFFFFFu : 0u);\n",
                            comp_dest, first_source, op_str, second_source);
                    }
                }
            }
        } else {
            if (ctx->use_uint_temps) {
              hlsl_format_checked(ctx, rhs_expr, rhs_len,
                                  "((%s == %s) ? 0xFFFFFFFFu : 0u)",
                                  final_src0, final_src1);
            } else {
              hlsl_format_checked(
                  ctx, rhs_expr, rhs_len,
                  "asfloat((%s == %s) ? 0xFFFFFFFFu : 0u)", final_src0,
                  final_src1);
            }
        }
        break;
      }
      case USIL_OP_NE:
      case USIL_OP_INE: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        int dest_m = inst->operands[0].destination_mask;
        bool is_vector_literal = (inst->operands[2].type == OPERAND_TYPE_IMMEDIATE32 && inst->operands[2].imm_value_count > 1);
        if (is_vector_literal && get_mask_component_count(dest_m) > 1) {
            *is_custom = true;
            const char* op_str = "!=";
            const DXBCOperand *first_operand =
                swap ? &inst->operands[2] : &inst->operands[1];
            const DXBCOperand *second_operand =
                swap ? &inst->operands[1] : &inst->operands[2];
            for (int c = 0; c < 4; c++) {
                if (dest_m & (16 << c)) {
                    char comp_dest[128];
                    format_dest_operand_hlsl(ctx, &inst->operands[0], false,
                                             ctx->use_uint_temps, 16 << c,
                                             false, comp_dest,
                                             sizeof(comp_dest));
                    char first_source[256];
                    char second_source[256];
                    format_operand_hlsl(ctx, first_operand, isInt, isUint,
                                        16 << c, false, first_source,
                                        sizeof(first_source));
                    format_operand_hlsl(ctx, second_operand, isInt, isUint,
                                        16 << c, false, second_source,
                                        sizeof(second_source));
                    if (ctx->sb->failed) return;
                    sb_append_spaces(sb, ctx->indent);
                    if (ctx->use_uint_temps) {
                        sb_appendf(sb,
                                   "%s = ((%s %s %s) ? 0xFFFFFFFFu : 0u);\n",
                                   comp_dest, first_source, op_str,
                                   second_source);
                    } else {
                        sb_appendf(
                            sb,
                            "%s = asfloat((%s %s %s) ? 0xFFFFFFFFu : 0u);\n",
                            comp_dest, first_source, op_str, second_source);
                    }
                }
            }
        } else {
            if (ctx->use_uint_temps) {
              hlsl_format_checked(ctx, rhs_expr, rhs_len,
                                  "((%s != %s) ? 0xFFFFFFFFu : 0u)",
                                  final_src0, final_src1);
            } else {
              hlsl_format_checked(
                  ctx, rhs_expr, rhs_len,
                  "asfloat((%s != %s) ? 0xFFFFFFFFu : 0u)", final_src0,
                  final_src1);
            }
        }
        break;
      }
      default:
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                   HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
                                   ctx->current_instruction_index, -1);
        break;
    }
}

