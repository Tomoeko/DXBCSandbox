// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>

void emit_bitwise_op(HLSLEmitterContext* ctx, const USILInstruction* inst,
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
    (void)current_dest_mask;
    (void)dest;
    (void)src3;
    (void)src4;
    (void)line_buf;
    (void)line_buf_sz;
    (void)is_custom;
    (void)wrap_swizzle;

    switch (inst->opcode) {
      case USIL_OP_AND: {
        if (hlsl_readability_transforms_enabled(ctx) &&
            ctx->modulo_divisor[ctx->current_instruction_index] > 0) {
            int divisor = ctx->modulo_divisor[ctx->current_instruction_index];
            char src0_signed[128];
            format_operand_hlsl(ctx, &inst->operands[1], true, false, format_mask, false, src0_signed, sizeof(src0_signed));
            /* The formatter was explicitly asked for a signed-int value.
             * Whether that value needs an asint bitcast is typed operand
             * metadata, not something to infer from the rendered prefix. */
            if (ctx->use_uint_temps) {
                hlsl_format_checked(ctx, rhs_expr, rhs_len, "asuint(%s %% %d)", src0_signed,
                         divisor);
            } else {
                hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s %% %d)",
                         src0_signed, divisor);
            }
        } else {
            const char *final_src0 = src1;
            const char *final_src1 = src0;
            if (ctx->use_uint_temps) {
              hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s & %s", final_src0, final_src1);
            } else {
              hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s & %s)", final_src0, final_src1);
            }
        }
        break;
      }
      case USIL_OP_OR: {
        const char *final_src0 = src1;
        const char *final_src1 = src0;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s | %s", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s | %s)", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_XOR: {
        const char *final_src0 = src1;
        const char *final_src1 = src0;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s ^ %s", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s ^ %s)", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_NOT:
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "~%s", src0);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(~%s)", src0);
        }
        break;
      case USIL_OP_ISHL:
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                              "asuint(%s) << (asuint(%s) & 31u)", src0,
                              src1);
        } else {
          /* Left shift is defined on the 32-bit lane bits; signedness does
           * not affect the low result bits.  Making both operands unsigned
           * also gives literal operands an unambiguous SM5 width. */
          hlsl_format_checked(
              ctx, rhs_expr, rhs_len,
              "asfloat(asuint(%s) << (asuint(%s) & 31u))", src0, src1);
        }
        break;
      case USIL_OP_ISHR:
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s >> %s", src0, src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s >> %s)", src0, src1);
        }
        break;
      case USIL_OP_USHR:
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s >> %s", src0, src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s >> %s)", src0, src1);
        }
        break;
      case USIL_OP_UBFE:
        /* DXBC order is width, offset, value.  Only the low five bits of
         * width and offset participate; a zero width must produce zero.  The
         * unsigned shift-and-mask spelling is the source pattern recognized
         * by the SM5 compiler as UBFE and avoids signed right-shift behavior. */
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                   "(%s >> (%s & 31u)) & ((1u << (%s & 31u)) - 1u)",
                   src2, src1, src0);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len,
                   "asfloat((%s >> (%s & 31u)) & "
                   "((1u << (%s & 31u)) - 1u))",
                   src2, src1, src0);
        }
        break;
      default:
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                   HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
                                   ctx->current_instruction_index, -1);
        break;
    }
}

