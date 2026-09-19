// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitter_ops_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static const char *operand_expression_text(const StringBuilder *expression) {
    return expression && expression->buf ? expression->buf : "";
}

static void free_operand_expressions(
    StringBuilder expressions[DXBC_MAX_OPERANDS]) {
    for (int index = 0; index < DXBC_MAX_OPERANDS; ++index)
        sb_free(&expressions[index]);
}

static void format_uint_bits_as_float(HLSLEmitterContext* ctx,
                                      const char* expression,
                                      const DXBCOperand* source,
                                      char* output, size_t output_size) {
    const char* inner = expression;
    if (source->has_abs && source->has_neg) {
        size_t length = strlen(expression);
        if (strncmp(expression, "-abs(", 5) != 0 || length <= 6 ||
            expression[length - 1] != ')') {
            ctx->sb->failed = true;
            if (output && output_size > 0) output[0] = '\0';
            return;
        }
        char value[1024];
        size_t value_length = length - 6;
        if (value_length >= sizeof(value)) {
            ctx->sb->failed = true;
            if (output && output_size > 0) output[0] = '\0';
            return;
        }
        memcpy(value, expression + 5, value_length);
        value[value_length] = '\0';
        hlsl_format_checked(ctx, output, output_size,
                            "-abs(asfloat(%s))", value);
        return;
    } else if (source->has_abs) {
        size_t length = strlen(expression);
        if (strncmp(expression, "abs(", 4) != 0 || length <= 5 ||
            expression[length - 1] != ')') {
            ctx->sb->failed = true;
            if (output && output_size > 0) output[0] = '\0';
            return;
        }
        char value[1024];
        size_t value_length = length - 5;
        if (value_length >= sizeof(value)) {
            ctx->sb->failed = true;
            if (output && output_size > 0) output[0] = '\0';
            return;
        }
        memcpy(value, expression + 4, value_length);
        value[value_length] = '\0';
        hlsl_format_checked(ctx, output, output_size,
                            "abs(asfloat(%s))", value);
        return;
    } else if (source->has_neg) {
        if (expression[0] != '-') {
            ctx->sb->failed = true;
            if (output && output_size > 0) output[0] = '\0';
            return;
        }
        inner = expression + 1;
        hlsl_format_checked(ctx, output, output_size, "-asfloat(%s)", inner);
        return;
    }
    hlsl_format_checked(ctx, output, output_size, "asfloat(%s)", expression);
}

static bool copy_span_checked(HLSLEmitterContext* ctx, char* destination,
                              size_t destination_size, const char* source,
                              size_t source_length) {
    if (!destination || destination_size == 0 || !source ||
        source_length >= destination_size) {
        if (destination && destination_size > 0) destination[0] = '\0';
        if (ctx && ctx->sb) ctx->sb->failed = true;
        return false;
    }
    memcpy(destination, source, source_length);
    destination[source_length] = '\0';
    return true;
}

static bool split_emitted_assignment(HLSLEmitterContext* ctx,
                                     const char* assignment, char* left,
                                     size_t left_size, char* right,
                                     size_t right_size) {
    if (!assignment) goto fail;
    const char* equals = strchr(assignment, '=');
    size_t length = strlen(assignment);
    if (!equals || equals == assignment || length == 0 ||
        assignment[length - 1] != ';')
        goto fail;
    const char* right_start = equals + 1;
    while (*right_start == ' ') ++right_start;
    const char* right_end = assignment + length - 1;
    if (right_start >= right_end) goto fail;
    if (!copy_span_checked(ctx, left, left_size, assignment,
                           (size_t)(equals - assignment)) ||
        !copy_span_checked(ctx, right, right_size, right_start,
                           (size_t)(right_end - right_start)))
        return false;
    return true;

fail:
    if (ctx && ctx->sb) ctx->sb->failed = true;
    if (left && left_size > 0) left[0] = '\0';
    if (right && right_size > 0) right[0] = '\0';
    return false;
}

void hlsl_emit_instruction(HLSLEmitterContext* ctx, const USILInstruction* inst) {
    /* NOP also represents an elided instruction in a lifting transaction.
     * Its original index remains available for provenance and diagnostics. */
    if (inst->opcode == USIL_OP_NOP) return;
    const USILProgram* program = ctx->program;
    const SerializedProgramParameters* params = ctx->params;
    StringBuilder* sb = ctx->sb;
    int i = ctx->current_instruction_index;

    // Check for swizzle decomposition mov pattern
    if (hlsl_readability_transforms_enabled(ctx) &&
        inst->opcode == USIL_OP_MOV && inst->operand_count == 2 &&
        inst->operands[0].type == OPERAND_TYPE_TEMP &&
        inst->operands[1].type == OPERAND_TYPE_TEMP) {
        int dstRegIdx = inst->operands[0].register_index;
        int movSrcReg = inst->operands[1].register_index;
        if (ctx->has_decomposition[dstRegIdx] && ctx->decompositions[dstRegIdx].SrcRegIdx == movSrcReg) {
            sb_append_spaces(sb, ctx->indent);
            sb_append(sb, "// ");
            sb_append(sb, inst->original_asm);
            sb_append(sb, "\n");
            for (int comp = 0; comp < 4; comp++) {
                if (ctx->has_deferred_float[movSrcReg][comp]) {
                    sb_append_spaces(sb, ctx->indent);
                    sb_appendf(sb, "%s = %s;\n",
                               ctx->write_redirects[movSrcReg][comp],
                               ctx->deferred_floats[movSrcReg][comp]);
                }
            }
            return;
        }
    }

    // Skip other multi-writes to decomposed registers
    if (hlsl_readability_transforms_enabled(ctx) &&
        inst->operand_count >= 1 &&
        inst->operands[0].type == OPERAND_TYPE_TEMP) {
        int dstRegIdx = inst->operands[0].register_index;
        int destCount = get_mask_component_count(inst->operands[0].destination_mask);
        if (destCount > 1 && ctx->has_decomposition[dstRegIdx]) {
            sb_append_spaces(sb, ctx->indent);
            sb_append(sb, "// ");
            sb_append(sb, inst->original_asm);
            sb_append(sb, " (skipped decomposed multi-write)\n");
            return;
        }
    }

    // Check for cross product pattern
    if (hlsl_readability_transforms_enabled(ctx) &&
        ctx->cross_info[i].is_cross_mul) {
      // This is the mul of a cross product. We skip it, as it will be emitted
      // as part of cross() on the next instruction.
      sb_append_spaces(sb, ctx->indent);
      sb_append(sb, "// ");
      sb_append(sb, inst->original_asm);
      sb_append(sb, " (merged into cross)\n");
      return;
    }

    if (hlsl_readability_transforms_enabled(ctx) &&
        ctx->cross_info[i].is_cross_mad) {
      // This is the mad of a cross product. Emit it as cross().
      sb_append_spaces(sb, ctx->indent);
      sb_append(sb, "// ");
      sb_append(sb, inst->original_asm);
      sb_append(sb, "\n");

      char dest[128] = "";
      char src0_clean[128] = "";
      char src1_clean[128] = "";

      // Format destination
      format_native_dest_operand_hlsl(
          ctx, &inst->operands[0], inst->operands[0].destination_mask,
          false, dest, sizeof(dest));
      // Format sources by unswizzling cross-product-induced swizzles
      int source_instruction = ctx->cross_info[i].source_instruction;
      const USILInstruction *cross_source =
          &program->instructions[source_instruction];
      DXBCOperand temp_op0 = cross_source->operands[1];
      if (temp_op0.swizzle_mode == 1) {
        uint8_t s0 = temp_op0.swizzle[0];
        uint8_t s1 = temp_op0.swizzle[1];
        uint8_t s2 = temp_op0.swizzle[2];
        temp_op0.swizzle[0] = s1;
        temp_op0.swizzle[1] = s2;
        temp_op0.swizzle[2] = s0;
      }
      format_operand_hlsl(ctx, &temp_op0, false, false, 112, true, src0_clean,
                          sizeof(src0_clean));

      DXBCOperand temp_op1 = cross_source->operands[2];
      if (temp_op1.swizzle_mode == 1 && temp_op1.swizzle[0] == 0 &&
          temp_op1.swizzle[1] == 1 && temp_op1.swizzle[2] == 2) {
        temp_op1.swizzle[0] = 2; // z
        temp_op1.swizzle[1] = 0; // x
        temp_op1.swizzle[2] = 1; // y
      }
      format_operand_hlsl(ctx, &temp_op1, false, false, 112, true, src1_clean,
                          sizeof(src1_clean));

      char line_buf[1024];
      if (ctx->use_uint_temps &&
          (inst->operands[0].type == OPERAND_TYPE_TEMP ||
           inst->operands[0].type == OPERAND_TYPE_INDEXABLE_TEMP)) {
        if (inst->saturate) {
          hlsl_format_checked(ctx, line_buf, sizeof(line_buf),
                              "%s = asuint(saturate(cross(%s, %s)));", dest,
                              src0_clean, src1_clean);
        } else {
          hlsl_format_checked(ctx, line_buf, sizeof(line_buf),
                              "%s = asuint(cross(%s, %s));", dest, src0_clean,
                              src1_clean);
        }
      } else {
        if (inst->saturate) {
          hlsl_format_checked(ctx, line_buf, sizeof(line_buf),
                              "%s = saturate(cross(%s, %s));", dest,
                              src0_clean, src1_clean);
        } else {
          hlsl_format_checked(ctx, line_buf, sizeof(line_buf),
                              "%s = cross(%s, %s);", dest, src0_clean,
                              src1_clean);
        }
      }

      sb_append_spaces(sb, ctx->indent);
      sb_append(sb, line_buf);
      sb_append(sb, "\n");
      return;
    }

    bool is_signed_comparison = is_signed_int_op(inst->opcode);
    bool is_unsigned_comparison = is_unsigned_int_op(inst->opcode);

    bool isInt =
        (inst->opcode == USIL_OP_AND) || (inst->opcode == USIL_OP_OR) ||
        (inst->opcode == USIL_OP_XOR) || (inst->opcode == USIL_OP_NOT) ||
        (inst->opcode == USIL_OP_ISHL) || (inst->opcode == USIL_OP_ISHR) ||
        (inst->opcode == USIL_OP_USHR) || (inst->opcode == USIL_OP_UBFE) ||
        (inst->opcode == USIL_OP_SWITCH) ||
        (inst->opcode == USIL_OP_CASE) || (inst->opcode == USIL_OP_IADD) ||
        (inst->opcode == USIL_OP_IMUL) || (inst->opcode == USIL_OP_IMAD) ||
        (inst->opcode == USIL_OP_IMAX) ||
        (inst->opcode == USIL_OP_IMIN) || (inst->opcode == USIL_OP_INEG) ||
        (inst->opcode == USIL_OP_IMM_ATOMIC_IADD) ||
        (inst->opcode == USIL_OP_SAMPLEINFO &&
         inst->sample_info_return_type == 1u) ||
        (inst->opcode == USIL_OP_ILT) || (inst->opcode == USIL_OP_IGE) ||
        (inst->opcode == USIL_OP_IEQ) || (inst->opcode == USIL_OP_INE) ||
        (inst->opcode == USIL_OP_ULT) || (inst->opcode == USIL_OP_UGE) ||
        is_signed_comparison || is_unsigned_comparison;

    bool isUint =
        (inst->opcode == USIL_OP_AND) || (inst->opcode == USIL_OP_OR) ||
        (inst->opcode == USIL_OP_XOR) || (inst->opcode == USIL_OP_NOT) ||
        (inst->opcode == USIL_OP_USHR) || (inst->opcode == USIL_OP_UBFE) ||
        (inst->opcode == USIL_OP_UMAX) ||
        (inst->opcode == USIL_OP_UMIN) || (inst->opcode == USIL_OP_UDIV) ||
        (inst->opcode == USIL_OP_SAMPLEINFO &&
         inst->sample_info_return_type == 1u) ||
        (inst->opcode == USIL_OP_ULT) || (inst->opcode == USIL_OP_UGE) ||
        (inst->opcode == USIL_OP_UTOF) ||
        is_unsigned_comparison ||
        (ctx->use_uint_temps &&
         (inst->opcode == USIL_OP_MOV || inst->opcode == USIL_OP_MOVC));

    bool is_component_wise =
        (inst->opcode == USIL_OP_ADD) || (inst->opcode == USIL_OP_SUB) ||
        (inst->opcode == USIL_OP_MUL) || (inst->opcode == USIL_OP_DIV) ||
        (inst->opcode == USIL_OP_MAD) || (inst->opcode == USIL_OP_MOV) ||
        (inst->opcode == USIL_OP_MOVC) || (inst->opcode == USIL_OP_MIN) ||
        (inst->opcode == USIL_OP_MAX) || (inst->opcode == USIL_OP_LT) ||
        (inst->opcode == USIL_OP_GE) || (inst->opcode == USIL_OP_EQ) ||
        (inst->opcode == USIL_OP_NE) || (inst->opcode == USIL_OP_AND) ||
        (inst->opcode == USIL_OP_OR) || (inst->opcode == USIL_OP_XOR) ||
        (inst->opcode == USIL_OP_NOT) || (inst->opcode == USIL_OP_ISHL) ||
        (inst->opcode == USIL_OP_ISHR) || (inst->opcode == USIL_OP_USHR) ||
        (inst->opcode == USIL_OP_UBFE) ||
        (inst->opcode == USIL_OP_FTOI) || (inst->opcode == USIL_OP_FTOU) ||
        (inst->opcode == USIL_OP_ITOF) || (inst->opcode == USIL_OP_UTOF) ||
        (inst->opcode == USIL_OP_ILT) || (inst->opcode == USIL_OP_IGE) ||
        (inst->opcode == USIL_OP_IEQ) || (inst->opcode == USIL_OP_INE) ||
        (inst->opcode == USIL_OP_ULT) || (inst->opcode == USIL_OP_UGE) ||
        (inst->opcode == USIL_OP_RCP) || (inst->opcode == USIL_OP_RSQ) ||
        (inst->opcode == USIL_OP_SQRT) || (inst->opcode == USIL_OP_LOG) ||
        (inst->opcode == USIL_OP_EXP) || (inst->opcode == USIL_OP_SIN) ||
        (inst->opcode == USIL_OP_COS) || (inst->opcode == USIL_OP_FRC) ||
        (inst->opcode == USIL_OP_ROUND_NE) ||
        (inst->opcode == USIL_OP_ROUND_NI) ||
        (inst->opcode == USIL_OP_ROUND_PI) ||
        (inst->opcode == USIL_OP_ROUND_Z) ||
        (inst->opcode == USIL_OP_DERIV_RTX) ||
        (inst->opcode == USIL_OP_DERIV_RTY) ||
        (inst->opcode == USIL_OP_DERIV_RTX_COARSE) ||
        (inst->opcode == USIL_OP_DERIV_RTY_COARSE) ||
        (inst->opcode == USIL_OP_DERIV_RTX_FINE) ||
        (inst->opcode == USIL_OP_DERIV_RTY_FINE) ||
        (inst->opcode == USIL_OP_IADD) ||
        (inst->opcode == USIL_OP_IMUL && inst->operand_count != 4) ||
        (inst->opcode == USIL_OP_IMAD) ||
        (inst->opcode == USIL_OP_IMAX) || (inst->opcode == USIL_OP_IMIN) ||
        (inst->opcode == USIL_OP_UMAX) || (inst->opcode == USIL_OP_UMIN) ||
        (inst->opcode == USIL_OP_INEG);

    int dest_mask = 0;
    if (is_component_wise && inst->operand_count >= 1) {
      dest_mask = inst->operands[0].destination_mask;
    } else if (inst->opcode == USIL_OP_DP2) {
      dest_mask = 16 | 32;
    } else if (inst->opcode == USIL_OP_DP3) {
      dest_mask = 16 | 32 | 64;
    } else if (inst->opcode == USIL_OP_DP4) {
      dest_mask = 16 | 32 | 64 | 128;
    }

    bool use_constructor = false;
    bool split_mask = false;

    // Removed has_non_contig_src logic to prevent unwanted pixel shader swizzle splitting

    bool has_special_dest = false;
    if (inst->operand_count >= 1 && inst->operands[0].type == OPERAND_TYPE_TEMP) {
        int dstRegIdx = inst->operands[0].register_index;
        if (is_register_decomposed(ctx, dstRegIdx)) {
            has_special_dest = true;
        }
        for (int c = 0; c < 4; c++) {
            if (inst->operands[0].destination_mask & (16 << c)) {
                if (ctx->has_deferred_float[dstRegIdx][c] || ctx->has_write_redirect[dstRegIdx][c]) {
                    has_special_dest = true;
                }
            }
        }
    }

    if (has_special_dest) {
        split_mask = true;
    }

    /* A DXBC component-wise write is one simultaneous vector assignment even
     * when its physical destination lanes are non-contiguous.  HLSL writable
     * masks express that contract directly (for example, r2.xw = ...).
     * Splitting solely because the mask has a gap changes the expression DAG:
     * D3DCompiler may hoist or combine one now-independent scalar lane and
     * thereby change otherwise unrelated instruction packing.  Keep splitting
     * only when a destination really has separate backing storage, as proven
     * by has_special_dest above, or for the explicit output/storage cases
     * below. */
    if (inst->operand_count >= 1) {
        int comp_cnt = get_mask_component_count(inst->operands[0].destination_mask);
        if (comp_cnt <= 1) {
            split_mask = false;
            use_constructor = false;
        } else if (comp_cnt >= 3 && !has_special_dest) {
            if (inst->opcode == USIL_OP_MOVC && inst->operand_count >= 4) {
                bool has_identity_temp = false;
                bool has_non_identity_temp = false;
                for (int op_idx = 2; op_idx <= 3; op_idx++) {
                    const DXBCOperand* op = &inst->operands[op_idx];
                    if (op->type == OPERAND_TYPE_TEMP || op->type == OPERAND_TYPE_INDEXABLE_TEMP) {
                        if (op->swizzle_mode == 1) {
                            bool is_id = (op->swizzle[0] == 0 && op->swizzle[1] == 1 && op->swizzle[2] == 2 && op->swizzle[3] == 3);
                            if (is_id) {
                                has_identity_temp = true;
                            } else {
                                has_non_identity_temp = true;
                            }
                        } else if (op->swizzle_mode == 0) {
                            has_identity_temp = true;
                        }
                    }
                }
                bool swizzles_different = false;
                const DXBCOperand* op2 = &inst->operands[2];
                const DXBCOperand* op3 = &inst->operands[3];
                if ((op2->type == OPERAND_TYPE_TEMP || op2->type == OPERAND_TYPE_INDEXABLE_TEMP) &&
                    (op3->type == OPERAND_TYPE_TEMP || op3->type == OPERAND_TYPE_INDEXABLE_TEMP)) {
                    if (op2->swizzle_mode != op3->swizzle_mode) {
                        swizzles_different = true;
                    } else if (op2->swizzle_mode == 1) {
                        for (int c = 0; c < 4; c++) {
                            if (op2->swizzle[c] != op3->swizzle[c]) {
                                swizzles_different = true;
                                break;
                            }
                        }
                    } else if (op2->swizzle_mode == 2) {
                        if (op2->swizzle[0] != op3->swizzle[0]) {
                            swizzles_different = true;
                        }
                    }
                }
                if (has_non_identity_temp && !has_identity_temp && swizzles_different) {
                    split_mask = true;
                } else {
                    split_mask = false;
                    use_constructor = false;
                }
            } else if (!is_mask_non_contiguous(inst->operands[0].destination_mask)) {
                split_mask = false;
                use_constructor = false;
            }
        }
    }

    bool has_scrambled_op = false;
    for (int operand_index = 0; operand_index < inst->operand_count; operand_index++) {
        if (inst->operands[operand_index].type == OPERAND_TYPE_TEMP) {
            int reg = inst->operands[operand_index].register_index;
            if (reg >= 0 && reg < ctx->temp_state_count) {
                int inst_idx = ctx->current_instruction_index;
                if (operand_index == 0 && inst_writes_to_dest(inst)) {
                    inst_idx = ctx->current_instruction_index + 1;
                }
                if (hlsl_readability_transforms_enabled(ctx) &&
                    hlsl_register_is_scrambled(ctx, inst_idx, reg)) {
                    has_scrambled_op = true;
                    break;
                }
            }
        }
    }
    if (has_scrambled_op) {
        split_mask = false;
        use_constructor = false;
    }

    StringBuilder comp_exprs[4];
    for (int component = 0; component < 4; ++component) {
      sb_init(&comp_exprs[component]);
    }
    if (is_component_wise && inst->opcode != USIL_OP_MOV && inst->opcode != USIL_OP_MOVC &&
        inst->operand_count >= 1 && inst->operands[0].type == OPERAND_TYPE_OUTPUT) {
      int comp_cnt =
          get_mask_component_count(inst->operands[0].destination_mask);
      if (comp_cnt > 2 && comp_cnt < 4 &&
          !compiler_model_preserves_vector_output(
              ctx, ctx->current_instruction_index) &&
          !semantic_instruction_has_flag(
              ctx, ctx->current_instruction_index,
              HLSL_SEMANTIC_FLAG_UNITY_SH_VECTOR_OUTPUT)) {
        split_mask = true;
      }
    }
    int start_comp = 0;
    int end_comp = 4;
    if (!split_mask) {
      start_comp = 0;
      end_comp = 1;
    }

    if (inst->opcode == USIL_OP_ELSE || inst->opcode == USIL_OP_ENDIF ||
        inst->opcode == USIL_OP_ENDLOOP || inst->opcode == USIL_OP_ENDSWITCH) {
      ctx->indent -= 4;
      if (ctx->indent < 4)
        ctx->indent = 4;
    }

    sb_append_spaces(sb, ctx->indent);
    sb_append(sb, "// ");
    sb_append(sb, inst->original_asm);
    sb_append(sb, "\n");

    int comps_order[4] = {0, 1, 2, 3};
    int comp_limit = end_comp;
    if (split_mask && inst->opcode == USIL_OP_MOV && inst->operand_count >= 2 &&
        inst->operands[1].swizzle_mode == 1) {
      // Sort comps_order based on source swizzle index to execute in ascending
      // order of source components
      for (int a = 0; a < 4; a++) {
        for (int b = a + 1; b < 4; b++) {
          int src_a = inst->operands[1].swizzle[comps_order[a]];
          int src_b = inst->operands[1].swizzle[comps_order[b]];
          if (src_a > src_b) {
            int temp = comps_order[a];
            comps_order[a] = comps_order[b];
            comps_order[b] = temp;
          }
        }
      }
    }
    for (int comp_idx = start_comp; comp_idx < comp_limit; comp_idx++) {
      int comp = split_mask ? comps_order[comp_idx] : comp_idx;
      if (split_mask && !(inst->operands[0].destination_mask & (16 << comp))) {
        continue;
      }
      int current_dest_mask = split_mask ? (16 << comp) : dest_mask;
      int format_mask = current_dest_mask;
      bool wrap_swizzle = false;
      int dm = split_mask ? (16 << comp) : (inst->operand_count >= 1 ? inst->operands[0].destination_mask : 0);

      StringBuilder operand_expressions[DXBC_MAX_OPERANDS];
      for (int operand = 0; operand < DXBC_MAX_OPERANDS; ++operand)
        sb_init(&operand_expressions[operand]);
      char destination_override[128] = "";
      const char *dest = "";
      const char *src0 = "";
      const char *src1 = "";
      const char *src2 = "";
      const char *src3 = "";
      const char *src4 = "";
      HLSLBackingStorage dest_storage = HLSL_BACKING_STORAGE_FLOAT;

      if (split_mask) {
        hlsl_format_checked(ctx, destination_override,
                            sizeof(destination_override), "u_xlat_temp_%c",
                            "xyzw"[comp]);
        dest = destination_override;
      } else if (inst->operand_count >= 1) {
        if (inst_writes_to_dest(inst)) {
          dest_storage = hlsl_operand_backing_storage(
              ctx, &inst->operands[0], dm, true);
          format_native_dest_operand_hlsl_sb(
              ctx, &inst->operands[0], dm, false,
              &operand_expressions[0]);
        } else {
          format_operand_hlsl_sb(ctx, &inst->operands[0], isInt, isUint, dm,
                                 false, &operand_expressions[0]);
        }
        dest = operand_expression_text(&operand_expressions[0]);

        // Swizzle Decomposition Override for dest
        if (inst->operands[0].type == OPERAND_TYPE_TEMP) {
            int dstRegIdx = inst->operands[0].register_index;
            int dstComp = 0;
            for (int b = 0; b < 4; b++) {
                if (dm & (16 << b)) {
                    dstComp = b;
                    break;
                }
            }
            if (ctx->has_deferred_float[dstRegIdx][dstComp]) {
                hlsl_format_checked(ctx, destination_override,
                                    sizeof(destination_override), "float %s",
                                    ctx->deferred_floats[dstRegIdx][dstComp]);
                dest = destination_override;
            } else if (ctx->has_write_redirect[dstRegIdx][dstComp]) {
                hlsl_copy_checked(ctx, destination_override,
                                  sizeof(destination_override),
                                  ctx->write_redirects[dstRegIdx][dstComp]);
                dest = destination_override;
            }
        }
      }



      if (inst->operand_count >= 2) {
        bool is_dp = (inst->opcode == USIL_OP_DP2) ||
                     (inst->opcode == USIL_OP_DP3) ||
                     (inst->opcode == USIL_OP_DP4);
        int mask_to_use =
            (current_dest_mask != 0 &&
             (is_dp || !is_operand_scalar(&inst->operands[1], params)))
                ? format_mask
                : 0;
        bool op_isInt = isInt || (inst->opcode == USIL_OP_LD ||
                                  inst->opcode == USIL_OP_LDMS ||
                                  inst->opcode == USIL_OP_LD_MS) ||
                                 (inst->opcode == USIL_OP_ITOF);
        bool is_op_replicate = is_replicate_swizzle(&inst->operands[1]);
        bool pv = !split_mask && (is_dp && is_op_replicate);
        format_operand_hlsl_sb(ctx, &inst->operands[1], op_isInt, isUint,
                               mask_to_use, pv, &operand_expressions[1]);
        src0 = operand_expression_text(&operand_expressions[1]);
      }
      if (inst->operand_count >= 3) {
        bool is_dp = (inst->opcode == USIL_OP_DP2) ||
                     (inst->opcode == USIL_OP_DP3) ||
                     (inst->opcode == USIL_OP_DP4);
        int mask_to_use =
            (current_dest_mask != 0 &&
             (is_dp || !is_operand_scalar(&inst->operands[2], params)))
                ? format_mask
                : 0;
        bool is_op_replicate = is_replicate_swizzle(&inst->operands[2]);
        bool pv = !split_mask && (is_dp && is_op_replicate);
        format_operand_hlsl_sb(ctx, &inst->operands[2], isInt, isUint,
                               mask_to_use, pv, &operand_expressions[2]);
        src1 = operand_expression_text(&operand_expressions[2]);
      }
      if (inst->operand_count >= 4) {
        bool is_dp = (inst->opcode == USIL_OP_DP2) ||
                     (inst->opcode == USIL_OP_DP3) ||
                     (inst->opcode == USIL_OP_DP4);
        int mask_to_use =
            (current_dest_mask != 0 &&
             (is_dp || !is_operand_scalar(&inst->operands[3], params)))
                ? format_mask
                : 0;
        bool op_isInt = isInt || (inst->opcode == USIL_OP_LD ||
                                  inst->opcode == USIL_OP_LDMS ||
                                  inst->opcode == USIL_OP_LD_MS);
        bool is_op_replicate = is_replicate_swizzle(&inst->operands[3]);
        bool pv = !split_mask && (is_dp && is_op_replicate);
        format_operand_hlsl_sb(ctx, &inst->operands[3], op_isInt, isUint,
                               mask_to_use, pv, &operand_expressions[3]);
        src2 = operand_expression_text(&operand_expressions[3]);
      }
      if (inst->operand_count >= 5) {
        bool is_dp = (inst->opcode == USIL_OP_DP2) ||
                     (inst->opcode == USIL_OP_DP3) ||
                     (inst->opcode == USIL_OP_DP4);
        int mask_to_use =
            (current_dest_mask != 0 &&
             (is_dp || !is_operand_scalar(&inst->operands[4], params)))
                ? format_mask
                : 0;
        bool is_op_replicate = is_replicate_swizzle(&inst->operands[4]);
        bool pv = !split_mask && (is_dp && is_op_replicate);
        format_operand_hlsl_sb(ctx, &inst->operands[4], isInt, isUint,
                               mask_to_use, pv, &operand_expressions[4]);
        src3 = operand_expression_text(&operand_expressions[4]);
      }
      if (inst->operand_count >= 6) {
        bool is_dp = (inst->opcode == USIL_OP_DP2) ||
                     (inst->opcode == USIL_OP_DP3) ||
                     (inst->opcode == USIL_OP_DP4);
        int mask_to_use =
            (current_dest_mask != 0 &&
             (is_dp || !is_operand_scalar(&inst->operands[5], params)))
                ? format_mask
                : 0;
        bool is_op_replicate = is_replicate_swizzle(&inst->operands[5]);
        bool pv = !split_mask && (is_dp && is_op_replicate);
        format_operand_hlsl_sb(ctx, &inst->operands[5], isInt, isUint,
                               mask_to_use, pv, &operand_expressions[5]);
        src4 = operand_expression_text(&operand_expressions[5]);
      }

      /* Exact-mode temporary registers use uint backing storage so an
       * untyped DXBC MOV can remain a literal bit transfer.  Operand
       * modifiers and saturation are different: DXBC applies those in the
       * floating-point domain.  Applying '-' or abs() directly to the uint
       * spelling is both semantically wrong and diagnosed by Unity's HLSL
       * compiler.  Cross the bit/float boundary only when the instruction
       * proves that it is needed; an unmodified, unsaturated temporary MOV or
       * MOVC deliberately stays a raw uint copy/select. */
      bool move_value_uses_float_semantics = false;
      bool destination_is_float_output = false;
      char move_src0_float[4096] = "";
      char move_src1_float[4096] = "";
      char move_src2_float[4096] = "";
      if (ctx->use_uint_temps && inst->operand_count >= 1) {
        uint32_t output_component_type = 0;
        if (ctx->is_vertex &&
            inst->operands[0].type == OPERAND_TYPE_OUTPUT) {
          output_component_type = hlsl_output_register_component_type(
              program, (uint32_t)inst->operands[0].register_index);
        }
        destination_is_float_output =
            inst->operands[0].type == OPERAND_TYPE_OUTPUT_DEPTH ||
            (inst->operands[0].type == OPERAND_TYPE_OUTPUT &&
             (!ctx->is_vertex || (output_component_type != 1 &&
                                  output_component_type != 2)));
        if (inst->opcode == USIL_OP_MOV && inst->operand_count >= 2) {
          move_value_uses_float_semantics =
              inst->saturate || destination_is_float_output ||
              inst->operands[1].has_abs || inst->operands[1].has_neg;
          if (move_value_uses_float_semantics) {
            format_uint_bits_as_float(ctx, src0, &inst->operands[1],
                                      move_src0_float,
                                      sizeof(move_src0_float));
            src0 = move_src0_float;
          }
        } else if (inst->opcode == USIL_OP_MOVC &&
                   inst->operand_count >= 4) {
          /* The condition remains a raw DXBC truth value unless its own
           * operand modifier requires a floating-point operation. */
          if (inst->operands[1].has_abs || inst->operands[1].has_neg) {
            format_uint_bits_as_float(ctx, src0, &inst->operands[1],
                                      move_src0_float,
                                      sizeof(move_src0_float));
            src0 = move_src0_float;
          }
          move_value_uses_float_semantics =
              inst->saturate || destination_is_float_output ||
              inst->operands[2].has_abs || inst->operands[2].has_neg ||
              inst->operands[3].has_abs || inst->operands[3].has_neg;
          if (move_value_uses_float_semantics) {
            format_uint_bits_as_float(ctx, src1, &inst->operands[2],
                                      move_src1_float,
                                      sizeof(move_src1_float));
            format_uint_bits_as_float(ctx, src2, &inst->operands[3],
                                      move_src2_float,
                                      sizeof(move_src2_float));
            src1 = move_src1_float;
            src2 = move_src2_float;
          }
        }
      }

      char line_buf[4096] = "";
      char rhs_expr[4096] = "";
      sb_append_spaces(sb, ctx->indent);
      bool is_custom = false;

      switch (inst->opcode) {
          // Arithmetic Opcodes
          case USIL_OP_ADD:
          case USIL_OP_SUB:
          case USIL_OP_MUL:
          case USIL_OP_DIV:
          case USIL_OP_MAD:
          case USIL_OP_MOV:
          case USIL_OP_MOVC:
          case USIL_OP_DP2:
          case USIL_OP_DP3:
          case USIL_OP_DP4:
          case USIL_OP_RCP:
          case USIL_OP_RSQ:
          case USIL_OP_SQRT:
          case USIL_OP_MIN:
          case USIL_OP_MAX:
          case USIL_OP_LOG:
          case USIL_OP_EXP:
          case USIL_OP_SIN:
          case USIL_OP_COS:
          case USIL_OP_FRC:
          case USIL_OP_ROUND_NE:
          case USIL_OP_ROUND_NI:
          case USIL_OP_ROUND_PI:
          case USIL_OP_ROUND_Z:
          case USIL_OP_IADD:
          case USIL_OP_IMUL:
          case USIL_OP_IMAD:
          case USIL_OP_IMAX:
          case USIL_OP_IMIN:
          case USIL_OP_UMAX:
          case USIL_OP_UMIN:
          case USIL_OP_UDIV:
          case USIL_OP_INEG:
          case USIL_OP_SINCOS:
              emit_arithmetic_op(ctx, inst, isInt, isUint, comp, dm, format_mask, current_dest_mask,
                                 dest, src0, src1, src2, src3, src4, line_buf, sizeof(line_buf),
                                 rhs_expr, sizeof(rhs_expr), &is_custom, &wrap_swizzle);
              break;

          // Comparison Opcodes
          case USIL_OP_LT:
          case USIL_OP_ILT:
          case USIL_OP_ULT:
          case USIL_OP_GE:
          case USIL_OP_IGE:
          case USIL_OP_UGE:
          case USIL_OP_EQ:
          case USIL_OP_IEQ:
          case USIL_OP_NE:
          case USIL_OP_INE:
              emit_comparison_op(ctx, inst, isInt, isUint, comp, dm, format_mask, current_dest_mask,
                                 dest, src0, src1, src2, src3, src4, line_buf, sizeof(line_buf),
                                 rhs_expr, sizeof(rhs_expr), &is_custom, &wrap_swizzle);
              break;

          // Bitwise Opcodes
          case USIL_OP_AND:
          case USIL_OP_OR:
          case USIL_OP_XOR:
          case USIL_OP_NOT:
          case USIL_OP_ISHL:
          case USIL_OP_ISHR:
          case USIL_OP_USHR:
          case USIL_OP_UBFE:
              emit_bitwise_op(ctx, inst, isInt, isUint, comp, dm, format_mask, current_dest_mask,
                              dest, src0, src1, src2, src3, src4, line_buf, sizeof(line_buf),
                              rhs_expr, sizeof(rhs_expr), &is_custom, &wrap_swizzle);
              break;

          // Flow Control Opcodes
          case USIL_OP_IF:
          case USIL_OP_ELSE:
          case USIL_OP_ENDIF:
          case USIL_OP_LOOP:
          case USIL_OP_ENDLOOP:
          case USIL_OP_SWITCH:
          case USIL_OP_CASE:
          case USIL_OP_DEFAULT:
          case USIL_OP_ENDSWITCH:
          case USIL_OP_BREAK:
          case USIL_OP_BREAKC:
          case USIL_OP_CONTINUE:
          case USIL_OP_CONTINUEC:
          case USIL_OP_RET:
          case USIL_OP_DISCARD:
              emit_flow_control_op(ctx, inst, isInt, isUint, comp, dm, format_mask, current_dest_mask,
                                   dest, src0, src1, src2, src3, src4, line_buf, sizeof(line_buf),
                                   rhs_expr, sizeof(rhs_expr), &is_custom, &wrap_swizzle);
              break;

          // Texture & Atomic Opcodes
          case USIL_OP_SAMPLE:
          case USIL_OP_SAMPLE_C:
          case USIL_OP_SAMPLE_C_LZ:
          case USIL_OP_SAMPLE_L:
          case USIL_OP_SAMPLE_D:
          case USIL_OP_SAMPLE_B:
          case USIL_OP_LD:
          case USIL_OP_LD_STRUCTURED:
          case USIL_OP_LD_MS:
          case USIL_OP_RESINFO:
          case USIL_OP_SAMPLEINFO:
          case USIL_OP_DERIV_RTX:
          case USIL_OP_DERIV_RTY:
          case USIL_OP_DERIV_RTX_COARSE:
          case USIL_OP_DERIV_RTY_COARSE:
          case USIL_OP_DERIV_RTX_FINE:
          case USIL_OP_DERIV_RTY_FINE:
          case USIL_OP_IMM_ATOMIC_IADD:
          case USIL_OP_LDMS:
              emit_texture_op(ctx, inst, isInt, isUint, comp, dm, format_mask, current_dest_mask,
                              dest, src0, src1, src2, src3, src4, line_buf, sizeof(line_buf),
                              rhs_expr, sizeof(rhs_expr), &is_custom, &wrap_swizzle);
              break;

          // Conversion Opcodes
          case USIL_OP_FTOI:
          case USIL_OP_FTOU:
          case USIL_OP_ITOF:
          case USIL_OP_UTOF:
              emit_conversion_op(ctx, inst, isInt, isUint, comp, dm, format_mask, current_dest_mask,
                                 dest, src0, src1, src2, src3, src4, line_buf, sizeof(line_buf),
                                 rhs_expr, sizeof(rhs_expr), &is_custom, &wrap_swizzle);
              break;

          default:
              hlsl_emit_fail_instruction(
                  ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                  HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
                  ctx->current_instruction_index, -1);
              line_buf[0] = '\0';
              break;
      }

      /* Decomposed signed lanes have native int declarations.  Retarget the
       * opcode result from typed expression metadata; identifier spelling is
       * never consulted and only the proven outer bitcast may be removed. */
      if (!is_custom && dest_storage == HLSL_BACKING_STORAGE_SINT) {
        hlsl_retarget_expression_for_storage(
            ctx, hlsl_instruction_expression_kind(ctx, inst), dest_storage,
            rhs_expr);
      }

      if (!is_custom && is_componentwise_op(inst->opcode)) {
        if (wrap_swizzle) {
            char mask_str[8] = "";
            int idx = 0;
            if (dest_mask & 16) mask_str[idx++] = 'x';
            if (dest_mask & 32) mask_str[idx++] = 'y';
            if (dest_mask & 64) mask_str[idx++] = 'z';
            if (dest_mask & 128) mask_str[idx++] = 'w';
            mask_str[idx] = '\0';
            hlsl_format_checked(ctx, line_buf, sizeof(line_buf),
                                "%s = (%s).%s;", dest, rhs_expr, mask_str);
        } else {
            hlsl_format_checked(ctx, line_buf, sizeof(line_buf), "%s = %s;",
                                dest, rhs_expr);
        }
      }

      if (inst->saturate && !is_custom) {
        char left[128];
        char right[1024];
        if (split_emitted_assignment(ctx, line_buf, left, sizeof(left), right,
                                     sizeof(right))) {
          hlsl_format_checked(ctx, line_buf, sizeof(line_buf),
                              "%s= saturate(%s);", left, right);
        }
      }

      if (ctx->use_uint_temps && !is_custom && inst->operand_count >= 1 &&
          inst_writes_to_dest(inst) &&
          (inst->operands[0].type == OPERAND_TYPE_TEMP ||
           inst->operands[0].type == OPERAND_TYPE_INDEXABLE_TEMP)) {
        bool is_dest_decomposed_float = false;
        if (inst->operands[0].type == OPERAND_TYPE_TEMP) {
            int dstRegIdx = inst->operands[0].register_index;
            int dstComp = 0;
            for (int b = 0; b < 4; b++) {
                if (dm & (16 << b)) {
                    dstComp = b;
                    break;
                }
            }
            if (ctx->has_deferred_float[dstRegIdx][dstComp] || ctx->has_ftoi_temp[dstRegIdx][dstComp]) {
                is_dest_decomposed_float = true;
            }
        }
        bool is_float_writer =
            !(inst->opcode == USIL_OP_IADD || inst->opcode == USIL_OP_IMUL ||
              inst->opcode == USIL_OP_IMAD ||
              inst->opcode == USIL_OP_IMAX || inst->opcode == USIL_OP_IMIN ||
              inst->opcode == USIL_OP_UMAX || inst->opcode == USIL_OP_UMIN ||
              inst->opcode == USIL_OP_UDIV || inst->opcode == USIL_OP_INEG ||
              inst->opcode == USIL_OP_ISHL || inst->opcode == USIL_OP_ISHR ||
              inst->opcode == USIL_OP_USHR || inst->opcode == USIL_OP_UBFE ||
              inst->opcode == USIL_OP_AND ||
              inst->opcode == USIL_OP_OR || inst->opcode == USIL_OP_XOR ||
              inst->opcode == USIL_OP_NOT || inst->opcode == USIL_OP_FTOI ||
              inst->opcode == USIL_OP_FTOU || inst->opcode == USIL_OP_CASE ||
              inst->opcode == USIL_OP_SWITCH || inst->opcode == USIL_OP_LT ||
              inst->opcode == USIL_OP_GE || inst->opcode == USIL_OP_EQ ||
              inst->opcode == USIL_OP_NE || inst->opcode == USIL_OP_ILT ||
              inst->opcode == USIL_OP_IGE || inst->opcode == USIL_OP_IEQ ||
              inst->opcode == USIL_OP_INE || inst->opcode == USIL_OP_ULT ||
              inst->opcode == USIL_OP_UGE || inst->opcode == USIL_OP_MOV ||
              inst->opcode == USIL_OP_MOVC);
        if ((is_float_writer || move_value_uses_float_semantics) &&
            !is_dest_decomposed_float) {
          char left[128];
          char right[1024];
          if (split_emitted_assignment(ctx, line_buf, left, sizeof(left),
                                       right, sizeof(right))) {
            hlsl_format_checked(ctx, line_buf, sizeof(line_buf),
                                "%s= asuint(%s);", left, right);
          }
        }
      }

      if (ctx->use_uint_temps && !is_custom && !split_mask &&
          destination_is_float_output &&
          inst->operand_count >= 1 && inst_writes_to_dest(inst) &&
          (inst->operands[0].type == OPERAND_TYPE_OUTPUT ||
           inst->operands[0].type == OPERAND_TYPE_OUTPUT_DEPTH)) {
        char left[128];
        char right[1024];
        if (split_emitted_assignment(ctx, line_buf, left, sizeof(left), right,
                                     sizeof(right))) {
          char converted[1200];
          if (move_value_uses_float_semantics) {
            hlsl_copy_checked(ctx, converted, sizeof(converted), right);
          } else if (inst->opcode == USIL_OP_MOV &&
                     inst->operand_count >= 2) {
            format_uint_bits_as_float(ctx, right, &inst->operands[1],
                                      converted, sizeof(converted));
          } else {
            hlsl_format_checked(ctx, converted, sizeof(converted),
                                "asfloat(%s)", right);
          }
          hlsl_format_checked(ctx, line_buf, sizeof(line_buf), "%s= %s;",
                              left, converted);
        }
      }

      if (use_constructor) {
        char left[128];
        char right[1024];
        if (split_emitted_assignment(ctx, line_buf, left, sizeof(left), right,
                                     sizeof(right)))
          sb_append(&comp_exprs[comp], right);
        free_operand_expressions(operand_expressions);
        continue;
      }

      if (!is_custom) {
        sb_append(sb, line_buf);
        sb_append(sb, "\n");
      }
      free_operand_expressions(operand_expressions);
    }

    if (split_mask && !use_constructor) {
      int active_count = 0;
      char mask_chars[5] = "";
      for (int c = 0; c < 4; c++) {
        if (inst->operands[0].destination_mask & (16 << c)) {
          mask_chars[active_count] = "xyzw"[c];
          active_count++;
        }
      }
      mask_chars[active_count] = '\0';

      bool has_split_special_dest = false;
      if (inst->operands[0].type == OPERAND_TYPE_TEMP) {
        int dstRegIdx = inst->operands[0].register_index;
        if (is_register_decomposed(ctx, dstRegIdx)) {
          has_split_special_dest = true;
        }
        for (int c = 0; c < 4; c++) {
          if (inst->operands[0].destination_mask & (16 << c)) {
            if (ctx->has_deferred_float[dstRegIdx][c] || ctx->has_write_redirect[dstRegIdx][c]) {
              has_split_special_dest = true;
            }
          }
        }
      }

      if (active_count > 1 && !has_split_special_dest && inst->operands[0].type == OPERAND_TYPE_OUTPUT) {
        char dest_base[128] = "";
        format_native_dest_operand_hlsl(ctx, &inst->operands[0], 240, false,
                                        dest_base, sizeof(dest_base));

        const char *constructor_type = (ctx->use_uint_temps && inst->operands[0].type != OPERAND_TYPE_OUTPUT) ? "uint" : "float";
        sb_append_spaces(sb, ctx->indent);
        sb_appendf(sb, "%s.%s = %s%d(", dest_base, mask_chars,
                   constructor_type, active_count);
        for (int component = 0; component < active_count; ++component) {
          if (component > 0) sb_append(sb, ", ");
          sb_appendf(sb, "u_xlat_temp_%c", mask_chars[component]);
        }
        sb_append(sb, ");\n");
      } else {
        for (int c = 0; c < 4; c++) {
          if (inst->operands[0].destination_mask & (16 << c)) {
            char comp_dest[128] = "";
            format_native_dest_operand_hlsl(
                ctx, &inst->operands[0], 16 << c, false, comp_dest,
                sizeof(comp_dest));
            if (inst->operands[0].type == OPERAND_TYPE_TEMP) {
                int dstRegIdx = inst->operands[0].register_index;
                if (ctx->has_deferred_float[dstRegIdx][c]) {
                    hlsl_format_checked(ctx, comp_dest, sizeof(comp_dest),
                                        "float %s",
                                        ctx->deferred_floats[dstRegIdx][c]);
                } else if (ctx->has_write_redirect[dstRegIdx][c]) {
                    hlsl_copy_checked(ctx, comp_dest, sizeof(comp_dest),
                                      ctx->write_redirects[dstRegIdx][c]);
                }
            }
            sb_append_spaces(sb, ctx->indent);
            sb_appendf(sb, "%s = u_xlat_temp_%c;\n", comp_dest, "xyzw"[c]);
          }
        }
      }
    }

    if (use_constructor) {
      char dest_full[128] = "";
      format_native_dest_operand_hlsl(ctx, &inst->operands[0], 240, false,
                                      dest_full, sizeof(dest_full));

      const char *constructor_type = (ctx->use_uint_temps && inst->operands[0].type != OPERAND_TYPE_OUTPUT) ? "uint4" : "float4";

      char inactive_vals[4][128];
      for (int c = 0; c < 4; c++) {
        if (!(inst->operands[0].destination_mask & (16 << c))) {
          format_native_operand_hlsl(ctx, &inst->operands[0], 16 << c,
                                     false, inactive_vals[c],
                                     sizeof(inactive_vals[c]));
        }
      }

      int mask = inst->operands[0].destination_mask;
      sb_append_spaces(sb, ctx->indent);
      sb_appendf(sb, "%s = %s(", dest_full, constructor_type);
      for (int c = 0; c < 4; c++) {
        if (c > 0) sb_append(sb, ", ");
        if (mask & (16 << c)) {
          if (!sb_ok(&comp_exprs[c])) sb->failed = true;
          sb_append(sb, comp_exprs[c].buf ? comp_exprs[c].buf : "");
        } else {
          sb_append(sb, inactive_vals[c]);
        }
      }
      sb_append(sb, ");\n");
    }

    for (int component = 0; component < 4; ++component) {
      if (!sb_ok(&comp_exprs[component])) sb->failed = true;
      sb_free(&comp_exprs[component]);
    }
}
