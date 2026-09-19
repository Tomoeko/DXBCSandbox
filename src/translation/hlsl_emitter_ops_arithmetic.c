// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>

static int sincos_destination_mask(const DXBCOperand* operand) {
    if (!operand || operand->type == OPERAND_TYPE_NULL) return 0;
    return operand->destination_mask & 0xf0;
}

/* A sincos call is evaluated over the union of both destination masks. When
 * one result consumes only a subset, map its logical x/y/z/w lanes back to
 * the packed HLSL temporary returned by the intrinsic. */
static bool format_packed_temp_value(HLSLEmitterContext* ctx,
                                     const char* temp_name,
                                     int active_mask, int destination_mask,
                                     char* output, size_t output_size) {
    if (!ctx || !temp_name || !output || output_size == 0 ||
        destination_mask == 0 ||
        (destination_mask & ~active_mask) != 0) {
        if (ctx) {
          hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                                     HLSL_EMIT_REASON_INTERNAL_INVARIANT,
                                     ctx->current_instruction_index, -1);
        }
        if (output && output_size > 0) output[0] = '\0';
        return false;
    }
    if (destination_mask == active_mask) {
        return hlsl_copy_checked(ctx, output, output_size, temp_name);
    }

    char projection[6] = ".";
    int projection_length = 1;
    int packed_component = 0;
    for (int lane = 0; lane < 4; ++lane) {
        const int lane_mask = 16 << lane;
        if ((active_mask & lane_mask) == 0) continue;
        if ((destination_mask & lane_mask) != 0) {
            projection[projection_length++] = "xyzw"[packed_component];
        }
        ++packed_component;
    }
    projection[projection_length] = '\0';
    if (projection_length == 1) {
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                                   HLSL_EMIT_REASON_INTERNAL_INVARIANT,
                                   ctx->current_instruction_index, -1);
        output[0] = '\0';
        return false;
    }
    return hlsl_format_checked(ctx, output, output_size, "%s%s", temp_name,
                               projection);
}

/* DXBC evaluates every source lane before committing either destination of a
 * multi-output instruction.  HLSL out parameters have the same copy-out
 * contract, but two aliased output lvalues have no faithful ordering that we
 * can infer from the decoded instruction.  Reject only physical destination
 * lanes that are provably the same; a source may freely alias either output. */
static bool multi_output_destinations_overlap(const DXBCOperand* first,
                                              const DXBCOperand* second) {
    if (!first || !second || first->type == OPERAND_TYPE_NULL ||
        second->type == OPERAND_TYPE_NULL || first->type != second->type ||
        (first->destination_mask & second->destination_mask & 0xf0) == 0) {
        return false;
    }

    switch (first->type) {
        case OPERAND_TYPE_TEMP:
        case OPERAND_TYPE_OUTPUT:
        case OPERAND_TYPE_INDEXABLE_TEMP:
            if (first->register_index != second->register_index) return false;
            /* Immediate indices can prove two elements disjoint.  A relative
             * address cannot, so equal parent registers conservatively
             * overlap until runtime authority can establish otherwise. */
            if (!first->rel_op0 && !first->rel_op1 && !first->rel_op2 &&
                !second->rel_op0 && !second->rel_op1 && !second->rel_op2 &&
                first->register_index_dim == second->register_index_dim) {
                for (int dimension = 0;
                     dimension < first->register_index_dim && dimension < 3;
                     ++dimension) {
                    if (first->index_has_immediate[dimension] &&
                        second->index_has_immediate[dimension] &&
                        first->index_values[dimension] !=
                            second->index_values[dimension]) {
                        return false;
                    }
                }
            }
            return true;
        case OPERAND_TYPE_OUTPUT_DEPTH:
        case OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
        case OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL:
            return true;
        default:
            return false;
    }
}

static HLSLBackingStorage integer_destination_storage(
    const HLSLEmitterContext* ctx, const DXBCOperand* destination,
    int destination_mask) {
    if (ctx && destination && destination->type == OPERAND_TYPE_OUTPUT) {
        const uint32_t component_type = hlsl_output_register_component_type(
            ctx->program, (uint32_t)destination->register_index);
        if (component_type == 1u) return HLSL_BACKING_STORAGE_UINT;
        if (component_type == 2u) return HLSL_BACKING_STORAGE_SINT;
    }
    return hlsl_operand_backing_storage(ctx, destination, destination_mask,
                                        true);
}

/* Scatter a packed integer result only after every source/result temporary
 * for the decoded instruction has been computed.  Scalar scatter supports
 * mixed/decomposed backing storage without changing the intrinsic width. */
static void emit_packed_integer_assignments(
    HLSLEmitterContext* ctx, const DXBCOperand* destination,
    int destination_mask, const char* temp_name, int active_mask,
    bool result_is_signed, char* line, size_t line_size) {
    if (!ctx || !destination || !temp_name || destination_mask == 0) {
        if (ctx) {
            hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                                       HLSL_EMIT_REASON_INTERNAL_INVARIANT,
                                       ctx->current_instruction_index, -1);
        }
        return;
    }

    for (int component = 0; component < 4; ++component) {
        const int component_mask = 16 << component;
        if ((destination_mask & component_mask) == 0) continue;

        char destination_text[256];
        char value[128];
        format_native_dest_operand_hlsl(ctx, destination, component_mask,
                                        false, destination_text,
                                        sizeof(destination_text));
        format_packed_temp_value(ctx, temp_name, active_mask, component_mask,
                                 value, sizeof(value));
        const HLSLBackingStorage storage = integer_destination_storage(
            ctx, destination, component_mask);
        if (ctx->sb->failed ||
            storage == HLSL_BACKING_STORAGE_INVALID ||
            storage == HLSL_BACKING_STORAGE_MIXED) {
            hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
                                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT,
                                       ctx->current_instruction_index, -1);
            return;
        }

        const char* conversion = NULL;
        if (storage == HLSL_BACKING_STORAGE_FLOAT) {
            conversion = "asfloat";
        } else if (storage == HLSL_BACKING_STORAGE_UINT && result_is_signed) {
            conversion = "asuint";
        } else if (storage == HLSL_BACKING_STORAGE_SINT && !result_is_signed) {
            conversion = "asint";
        }

        sb_append_spaces(ctx->sb, ctx->indent);
        if (conversion) {
            hlsl_format_checked(ctx, line, line_size, "%s = %s(%s);\n",
                                destination_text, conversion, value);
        } else {
            hlsl_format_checked(ctx, line, line_size, "%s = %s;\n",
                                destination_text, value);
        }
        sb_append(ctx->sb, line);
    }
}

static void emit_sincos_assignment(HLSLEmitterContext* ctx,
                                   const DXBCOperand* destination,
                                   int destination_mask,
                                   const char* temp_name, int active_mask,
                                   bool saturate,
                                   char* line, size_t line_size) {
    char destination_text[256];
    char value[128];
    format_dest_operand_hlsl(ctx, destination, false, ctx->use_uint_temps,
                             destination_mask, false, destination_text,
                             sizeof(destination_text));
    format_packed_temp_value(ctx, temp_name, active_mask, destination_mask,
                             value, sizeof(value));
    sb_append_spaces(ctx->sb, ctx->indent);
    if (ctx->use_uint_temps) {
        if (saturate) {
            hlsl_format_checked(ctx, line, line_size,
                                "%s = asuint(saturate(%s));\n",
                                destination_text, value);
        } else {
            hlsl_format_checked(ctx, line, line_size,
                                "%s = asuint(%s);\n", destination_text,
                                value);
        }
    } else if (saturate) {
        hlsl_format_checked(ctx, line, line_size,
                            "%s = saturate(%s);\n", destination_text,
                            value);
    } else {
        hlsl_format_checked(ctx, line, line_size, "%s = %s;\n",
                            destination_text, value);
    }
    sb_append(ctx->sb, line);
}

void emit_arithmetic_op(HLSLEmitterContext* ctx, const USILInstruction* inst,
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
    (void)wrap_swizzle;
    (void)isInt;
    (void)isUint;
    (void)src3;
    (void)src4;
    (void)is_custom;

    HLSLTypedLoopBoundPhase typed_bound_phase =
        HLSL_TYPED_LOOP_BOUND_NONE;
    const LoopOptimizationInfo *typed_bound =
        hlsl_typed_loop_bound_for_instruction(
            ctx, i, &typed_bound_phase);
    if (typed_bound &&
        (typed_bound_phase == HLSL_TYPED_LOOP_BOUND_IMAX ||
         typed_bound_phase == HLSL_TYPED_LOOP_BOUND_IMIN)) {
      const char *alias = typed_bound->typed_bound_alias;
      bool swap = compiler_model_swaps_binary_operands(ctx, i);
      const char *final_src0 = swap ? src1 : alias;
      const char *final_src1 = swap ? alias : src1;
      sb_appendf(sb, "%s = %s(%s, %s);\n", alias,
                 typed_bound_phase == HLSL_TYPED_LOOP_BOUND_IMAX
                     ? "max" : "min",
                 final_src0, final_src1);
      sb_append_spaces(sb, ctx->indent);
      hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s)", alias);
      return;
    }

    switch (inst->opcode) {
      case USIL_OP_ADD: {
        if (inst->operand_count >= 3 && inst->operands[1].has_neg &&
            !inst->operands[2].has_neg && src0[0] == '-') {
          /* D3DCompiler represents subtraction as ADD with the negated source
           * first.  Emitting the subtraction form avoids ReorderBinary moving
           * that source behind the positive addend. */
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s - %s", src1, src0 + 1);
          break;
        }
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        
        if (compiler_add_uses_mad(inst)) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "mad(%s, 1.0, %s)", src0, src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s + %s", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_SUB:
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s - %s", src0, src1);
        break;
      case USIL_OP_MUL: {
        if (ctx->saved_mul_is_definition[i]) {
          const char *saved_src0 =
              ctx->saved_mul_reverse_definition[i] ? src1 : src0;
          const char *saved_src1 =
              ctx->saved_mul_reverse_definition[i] ? src0 : src1;
          char saved_line[2304];
          hlsl_format_checked(ctx, saved_line, sizeof(saved_line),
                   "dxbc_saved_mul%d = %s * %s;\n",
                   ctx->saved_mul_id[i] - 1, saved_src0, saved_src1);
          sb_append(sb, saved_line);
          /* emit_instruction has already appended indentation for this first
           * statement; restore it after the newline for the decoded MUL
           * destination assignment assembled by the common path. */
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "dxbc_saved_mul%d",
                   ctx->saved_mul_id[i] - 1);
          break;
        }
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s * %s", final_src0, final_src1);
        break;
      }
      case USIL_OP_DIV:
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s / %s", src0, src1);
        break;
      case USIL_OP_MAD: {
        if (ctx->saved_mul_id[i] != 0) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "dxbc_saved_mul%d + %s",
                   ctx->saved_mul_id[i] - 1, src2);
          break;
        }
        if (src1[0] == '-' && src0[0] != '-') {
          /*
           * Subtraction spelling loses a negate on the second multiplicand:
           * D3DCompiler canonicalizes c - a*b by moving it onto a.  The mad
           * intrinsic preserves the decoded second-operand modifier.
           */
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "mad(%s, %s, %s)", src0, src1, src2);
          break;
        }
        if (src0[0] == '-') {
          /*
           * For a negated first multiplicand, subtraction spelling is the
           * compiler-stable form.  In particular, mad(-x, literal, c) moves
           * the negate onto the literal.
           */
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s - %s * %s", src2, src0 + 1, src1);
          break;
        }
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s * %s + %s", src0, src1, src2);
        break;
      }
      case USIL_OP_MOV:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s;", dest, src0);
        break;
      case USIL_OP_MOVC: {
        char truthiness_alias[64];
        if (semantic_truthiness_condition_alias(
                ctx, ctx->current_instruction_index, 1, comp,
                truthiness_alias, sizeof(truthiness_alias))) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s ? %s : %s", truthiness_alias,
                   src1, src2);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asuint(%s) ? %s : %s", src0, src1,
                   src2);
        }
        break;
      }
      case USIL_OP_DP2:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = dot(%s, %s);", dest, src0, src1);
        break;
      case USIL_OP_DP3:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = dot(%s, %s);", dest, src0, src1);
        break;
      case USIL_OP_DP4:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = dot(%s, %s);", dest, src0, src1);
        break;
      case USIL_OP_RCP:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = 1.0f / %s;", dest, src0);
        break;
      case USIL_OP_RSQ:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = rsqrt(%s);", dest, src0);
        break;
      case USIL_OP_SQRT:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = sqrt(%s);", dest, src0);
        break;
      case USIL_OP_MIN: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "min(%s, %s)", final_src0, final_src1);
        break;
      }
      case USIL_OP_MAX: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        hlsl_format_checked(ctx, rhs_expr, rhs_len, "max(%s, %s)", final_src0, final_src1);
        break;
      }
      case USIL_OP_LOG:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = log2(%s);", dest, src0);
        break;
      case USIL_OP_EXP:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = exp2(%s);", dest, src0);
        break;
      case USIL_OP_SIN:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = sin(%s);", dest, src0);
        break;
      case USIL_OP_COS:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = cos(%s);", dest, src0);
        break;
      case USIL_OP_FRC:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = frac(%s);", dest, src0);
        break;
      case USIL_OP_ROUND_NE:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = round(%s);", dest, src0);
        break;
      case USIL_OP_ROUND_NI:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = floor(%s);", dest, src0);
        break;
      case USIL_OP_ROUND_PI:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = ceil(%s);", dest, src0);
        break;
      case USIL_OP_ROUND_Z:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = trunc(%s);", dest, src0);
        break;
      case USIL_OP_IADD: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s + %s", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s + %s)", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_IMUL: {
        if (inst->operand_count == 4) {
          *is_custom = true;
          const bool has_high =
              inst->operands[0].type != OPERAND_TYPE_NULL;
          const bool has_low = inst->operands[1].type != OPERAND_TYPE_NULL;
          const int high_mask = has_high
                                    ? inst->operands[0].destination_mask
                                    : 0;
          const int low_mask = has_low
                                   ? inst->operands[1].destination_mask
                                   : 0;
          if ((!has_high && !has_low) ||
              (has_high && get_mask_component_count(high_mask) == 0) ||
              (has_low && get_mask_component_count(low_mask) == 0) ||
              inst->saturate) {
            hlsl_emit_fail_instruction(
                ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
                HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE,
                ctx->current_instruction_index, -1);
            return;
          }
          /* Shader Model 5 HLSL has no source intrinsic whose contract is the
           * signed high/low result of DXBC IMUL.  `imulExtended` is a GLSL
           * spelling, not an HLSL intrinsic.  Emitting it produces invalid
           * ShaderLab source, while expanding the high word into arithmetic
           * cannot recompile to the decoded instruction.  Preserve the valid
           * low-only form and fail closed whenever the high result is live. */
          if (has_high) {
            hlsl_emit_fail_instruction(
                ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
                ctx->current_instruction_index, 0);
            return;
          }

          char low_dest[128];
          char source0[256];
          char source1[256];
          format_dest_operand_hlsl(ctx, &inst->operands[1], false,
                                   ctx->use_uint_temps, low_mask, false,
                                   low_dest, sizeof(low_dest));
          format_operand_hlsl(ctx, &inst->operands[2], true, false,
                              low_mask, false, source0, sizeof(source0));
          format_operand_hlsl(ctx, &inst->operands[3], true, false,
                              low_mask, false, source1, sizeof(source1));
          if (ctx->sb->failed) return;
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                              "%s = %s(%s * %s);\n", low_dest,
                              ctx->use_uint_temps ? "asuint" : "asfloat",
                              source0, source1);
          sb_append(sb, line_buf);
          break;
        }
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s * %s", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s * %s)", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_IMAD:
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "%s * %s + %s", src0, src1, src2);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(%s * %s + %s)", src0, src1,
                   src2);
        }
        break;
      case USIL_OP_IMAX: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "max(%s, %s)", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(max(%s, %s))", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_IMIN: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "min(%s, %s)", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(min(%s, %s))", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_UMAX: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "max(%s, %s)", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(max(%s, %s))", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_UMIN: {
        bool swap = compiler_model_swaps_binary_operands(ctx, i);
        const char *final_src0 = swap ? src1 : src0;
        const char *final_src1 = swap ? src0 : src1;
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "min(%s, %s)", final_src0, final_src1);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(min(%s, %s))", final_src0, final_src1);
        }
        break;
      }
      case USIL_OP_UDIV: {
        *is_custom = true;
        const bool has_quotient =
            inst->operands[0].type != OPERAND_TYPE_NULL;
        const bool has_remainder =
            inst->operands[1].type != OPERAND_TYPE_NULL;
        const int quotient_mask = has_quotient
                                      ? inst->operands[0].destination_mask
                                      : 0;
        const int remainder_mask = has_remainder
                                       ? inst->operands[1].destination_mask
                                       : 0;
        const int active_mask = quotient_mask | remainder_mask;
        if ((!has_quotient && !has_remainder) ||
            (has_quotient &&
             get_mask_component_count(quotient_mask) == 0) ||
            (has_remainder &&
             get_mask_component_count(remainder_mask) == 0) ||
            inst->saturate ||
            multi_output_destinations_overlap(&inst->operands[0],
                                              &inst->operands[1])) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
              HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE,
              ctx->current_instruction_index, -1);
          return;
        }

        const int component_count = get_mask_component_count(active_mask);
        const char* uint_type =
            get_cast_type_str("uint", component_count);
        char dividend[512];
        char divisor[512];
        char dividend_temp[64];
        char divisor_temp[64];
        char quotient_temp[64];
        char remainder_temp[64];
        format_operand_hlsl(ctx, &inst->operands[2], false, true,
                            active_mask, false, dividend,
                            sizeof(dividend));
        format_operand_hlsl(ctx, &inst->operands[3], false, true,
                            active_mask, false, divisor, sizeof(divisor));
        hlsl_format_checked(ctx, dividend_temp, sizeof(dividend_temp),
                            "udiv_dividend_%d", i);
        hlsl_format_checked(ctx, divisor_temp, sizeof(divisor_temp),
                            "udiv_divisor_%d", i);
        hlsl_format_checked(ctx, quotient_temp, sizeof(quotient_temp),
                            "udiv_quotient_%d", i);
        hlsl_format_checked(ctx, remainder_temp, sizeof(remainder_temp),
                            "udiv_remainder_%d", i);
        if (ctx->sb->failed) return;

        /* Capture the complete component-wise source tuple before either
         * output is written.  The typed declarations also force `/` and `%`
         * to operate at the exact decoded uintN width. */
        sb_append_spaces(sb, ctx->indent);
        hlsl_format_checked(ctx, line_buf, line_buf_sz,
                            "%s %s = %s;\n", uint_type, dividend_temp,
                            dividend);
        sb_append(sb, line_buf);
        sb_append_spaces(sb, ctx->indent);
        hlsl_format_checked(ctx, line_buf, line_buf_sz,
                            "%s %s = %s;\n", uint_type, divisor_temp,
                            divisor);
        sb_append(sb, line_buf);

        if (has_quotient) {
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                              "%s %s = %s / %s;\n", uint_type,
                              quotient_temp, dividend_temp, divisor_temp);
          sb_append(sb, line_buf);
        }
        if (has_remainder) {
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                              "%s %s = %s %% %s;\n", uint_type,
                              remainder_temp, dividend_temp, divisor_temp);
          sb_append(sb, line_buf);
        }

        if (has_quotient) {
          emit_packed_integer_assignments(
              ctx, &inst->operands[0], quotient_mask, quotient_temp,
              active_mask, false, line_buf, line_buf_sz);
        }
        if (has_remainder) {
          emit_packed_integer_assignments(
              ctx, &inst->operands[1], remainder_mask, remainder_temp,
              active_mask, false, line_buf, line_buf_sz);
        }
        break;
      }
      case USIL_OP_INEG:
        if (ctx->use_uint_temps) {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "-%s", src0);
        } else {
          hlsl_format_checked(ctx, rhs_expr, rhs_len, "asfloat(-%s)", src0);
        }
        break;
      case USIL_OP_SINCOS: {
        *is_custom = true;
        const int sin_mask = sincos_destination_mask(&inst->operands[0]);
        const int cos_mask = sincos_destination_mask(&inst->operands[1]);
        const int active_mask = sin_mask | cos_mask;
        if (active_mask == 0 ||
            multi_output_destinations_overlap(&inst->operands[0],
                                              &inst->operands[1])) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
              HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE,
              ctx->current_instruction_index, -1);
          break;
        }
        const int component_count = get_mask_component_count(active_mask);
        const char* float_type = get_cast_type_str("float", component_count);
        char angle[512];
        format_operand_hlsl(ctx, &inst->operands[2], false, false,
                            active_mask, false, angle, sizeof(angle));
        if (component_count > 1 &&
            is_replicate_swizzle(&inst->operands[2])) {
          char scalar_angle[sizeof(angle)];
          hlsl_copy_checked(ctx, scalar_angle, sizeof(scalar_angle), angle);
          hlsl_format_checked(ctx, angle, sizeof(angle), "(%s)(%s)",
                              float_type, scalar_angle);
        }

        const bool has_sin = sin_mask != 0;
        const bool has_cos = cos_mask != 0;
        /* DXBC's null destination is authoritative.  A one-output SINCOS is
         * D3DCompiler's lowering of the corresponding HLSL unary intrinsic.
         * A fabricated dead output has no decoded width or data-flow
         * authority and can make HLSL's output-parameter types disagree.
         * Preserve the decoded scalar/vector width by emitting sin/cos over
         * exactly the active destination lanes.  Whether adjacent unary
         * operations are packed is governed by the surrounding expression
         * DAG, whose physical masked writes are preserved by the main
         * instruction emitter. */
        if (has_sin != has_cos) {
          const DXBCOperand* output_operand =
              has_sin ? &inst->operands[0] : &inst->operands[1];
          const int output_mask = has_sin ? sin_mask : cos_mask;
          const char* intrinsic = has_sin ? "sin" : "cos";
          char output[256];
          format_dest_operand_hlsl(ctx, output_operand, false,
                                   ctx->use_uint_temps, output_mask, false,
                                   output, sizeof(output));
          if (ctx->use_uint_temps) {
            if (inst->saturate) {
              hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                  "%s = asuint(saturate(%s(%s)));",
                                  output, intrinsic, angle);
            } else {
              hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                  "%s = asuint(%s(%s));", output,
                                  intrinsic, angle);
            }
          } else if (inst->saturate) {
            hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                "%s = saturate(%s(%s));", output,
                                intrinsic, angle);
          } else {
            hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                "%s = %s(%s);", output, intrinsic, angle);
          }
          sb_append(sb, line_buf);
          sb_append(sb, "\n");
          break;
        }
        const bool masks_differ = has_sin && has_cos && sin_mask != cos_mask;
        const bool stage_outputs =
            ctx->use_uint_temps || inst->saturate || masks_differ;
        if (stage_outputs) {
          char sin_temp_name[64];
          char cos_temp_name[64];
          hlsl_format_checked(ctx, sin_temp_name, sizeof(sin_temp_name),
                              "sincos_sin_temp_%d",
                              ctx->current_instruction_index);
          hlsl_format_checked(ctx, cos_temp_name, sizeof(cos_temp_name),
                              "sincos_cos_temp_%d",
                              ctx->current_instruction_index);
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s %s;\n",
                              float_type, sin_temp_name);
          sb_append(sb, line_buf);
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s %s;\n",
                              float_type, cos_temp_name);
          sb_append(sb, line_buf);
          
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                              "sincos(%s, %s, %s);", angle, sin_temp_name,
                              cos_temp_name);
          sb_append(sb, line_buf);
          sb_append(sb, "\n");
          
          if (has_sin) {
              emit_sincos_assignment(ctx, &inst->operands[0], sin_mask,
                                     sin_temp_name, active_mask,
                                     inst->saturate, line_buf, line_buf_sz);
          }
          if (has_cos) {
              emit_sincos_assignment(ctx, &inst->operands[1], cos_mask,
                                     cos_temp_name, active_mask,
                                     inst->saturate, line_buf, line_buf_sz);
          }
        } else {
          char s_out[128];
          char c_out[128];
          bool need_s_temp = !has_sin;
          bool need_c_temp = !has_cos;
          
          if (need_s_temp) {
            sb_append_spaces(sb, ctx->indent);
            hlsl_format_checked(ctx, line_buf, line_buf_sz,
                     "%s sincos_sin_temp_%d;\n", float_type,
                     ctx->current_instruction_index);
            sb_append(sb, line_buf);
            hlsl_format_checked(ctx, s_out, sizeof(s_out), "sincos_sin_temp_%d",
                     ctx->current_instruction_index);
          } else {
            format_dest_operand_hlsl(ctx, &inst->operands[0], false, false,
                                     sin_mask, false, s_out, sizeof(s_out));
          }
          
          if (need_c_temp) {
            sb_append_spaces(sb, ctx->indent);
            hlsl_format_checked(ctx, line_buf, line_buf_sz,
                     "%s sincos_cos_temp_%d;\n", float_type,
                     ctx->current_instruction_index);
            sb_append(sb, line_buf);
            hlsl_format_checked(ctx, c_out, sizeof(c_out), "sincos_cos_temp_%d",
                     ctx->current_instruction_index);
          } else {
            format_dest_operand_hlsl(ctx, &inst->operands[1], false, false,
                                     cos_mask, false, c_out, sizeof(c_out));
          }
          
          sb_append_spaces(sb, ctx->indent);
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                              "sincos(%s, %s, %s);", angle, s_out, c_out);
          sb_append(sb, line_buf);
          sb_append(sb, "\n");
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

