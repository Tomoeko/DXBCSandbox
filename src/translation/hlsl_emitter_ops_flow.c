// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>

void emit_flow_control_op(HLSLEmitterContext* ctx, const USILInstruction* inst,
                                  bool isInt, bool isUint,
                                  int comp, int dm, int format_mask, int current_dest_mask,
                                  const char* dest,
                                  const char* src0, const char* src1, const char* src2,
                                  const char* src3, const char* src4,
                                  char* line_buf, size_t line_buf_sz,
                                  char* rhs_expr, size_t rhs_len, bool* is_custom, bool* wrap_swizzle) {
    const USILProgram* program = ctx->program;
    StringBuilder* sb = ctx->sb;
    int i = ctx->current_instruction_index;
    (void)isInt;
    (void)isUint;
    (void)comp;
    (void)dm;
    (void)format_mask;
    (void)current_dest_mask;
    (void)src0;
    (void)src1;
    (void)src2;
    (void)src3;
    (void)src4;
    (void)rhs_expr;
    (void)rhs_len;
    (void)wrap_swizzle;

    switch (inst->opcode) {
      case USIL_OP_IF:
        *is_custom = true;
        if (inst->condition_test == DXBC_INSTRUCTION_TEST_NONZERO) {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "[branch] if (asuint(%s)) {\n", dest))
            return;
        } else {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "[branch] if (!asuint(%s)) {\n", dest))
            return;
        }
        sb_append(sb, line_buf);
        ctx->indent += 4;
        break;
      case USIL_OP_ELSE:
        *is_custom = true;
        sb_append(sb, "} else {\n");
        ctx->indent += 4;
        break;
      case USIL_OP_ENDIF:
        *is_custom = true;
        sb_append(sb, "}\n");
        break;
      case USIL_OP_LOOP: {
        *is_custom = true;
        bool loop_optimized = false;
        const LoopOptimizationInfo *info = &ctx->loop_info[i];
        if (info->is_optimized) {
          const USILInstruction *next1 = &program->instructions[info->comparison_inst_idx];
          const USILInstruction *next2 = &program->instructions[info->breakc_inst_idx];
          int inc_idx = info->inc_inst_idx;

          char s0[128] = "";
          char s1[128] = "";
          bool next1_isInt = is_signed_int_op(next1->opcode) ||
                             is_unsigned_int_op(next1->opcode);
          bool next1_isUint = is_unsigned_int_op(next1->opcode);
          int dest_m = next1->operands[0].destination_mask;
          bool pv = (next1->operands[1].type == OPERAND_TYPE_TEMP ||
                     next1->operands[1].type == OPERAND_TYPE_INDEXABLE_TEMP);
          format_operand_hlsl(ctx, &next1->operands[1], next1_isInt, next1_isUint,
                              dest_m, pv, s0, sizeof(s0));

          pv = (next1->operands[2].type == OPERAND_TYPE_TEMP ||
                next1->operands[2].type == OPERAND_TYPE_INDEXABLE_TEMP);
          format_operand_hlsl(ctx, &next1->operands[2], next1_isInt, next1_isUint,
                              dest_m, pv, s1, sizeof(s1));

          bool is_nz = next2->condition_test ==
                       DXBC_INSTRUCTION_TEST_NONZERO;
          const char *op_str = "";
          bool recompile = ctx->emit_mode == HLSL_EMIT_MODE_RECOMPILE;
          if (next1->opcode == USIL_OP_GE || next1->opcode == USIL_OP_IGE ||
              next1->opcode == USIL_OP_UGE) {
            op_str = recompile ? ">=" : (is_nz ? "<" : ">=");
          } else if (next1->opcode == USIL_OP_LT ||
                     next1->opcode == USIL_OP_ILT ||
                     next1->opcode == USIL_OP_ULT) {
            op_str = recompile ? "<" : (is_nz ? ">=" : "<");
          } else if (next1->opcode == USIL_OP_EQ ||
                     next1->opcode == USIL_OP_IEQ) {
            op_str = recompile ? "==" : (is_nz ? "!=" : "==");
          } else if (next1->opcode == USIL_OP_NE ||
                     next1->opcode == USIL_OP_INE) {
            op_str = recompile ? "!=" : (is_nz ? "==" : "!=");
          }

          if (recompile &&
              compiler_model_swaps_binary_operands(ctx,
                                                   info->comparison_inst_idx) &&
              (next1->opcode == USIL_OP_EQ ||
               next1->opcode == USIL_OP_IEQ ||
               next1->opcode == USIL_OP_NE ||
               next1->opcode == USIL_OP_INE)) {
            char temporary[128];
            if (!hlsl_copy_checked(ctx, temporary, sizeof(temporary), s0))
              return;
            if (!hlsl_copy_checked(ctx, s0, sizeof(s0), s1) ||
                !hlsl_copy_checked(ctx, s1, sizeof(s1), temporary))
              return;
          }

          const DXBCOperand *loop_counter = &next1->operands[1];

          if (recompile && info->typed_bound_ftoi_idx >= 0 &&
              info->typed_bound_alias[0]) {
            /* The typed-bound recognizer proves IGE+BREAKC_NZ and an
             * unconditional first-body overwrite of the predicate lane.
             * Referencing the typed clamp value here prevents FXC's
             * float-bit loop simulator from replacing a 3..N loop with zero
             * iterations, without dropping any observable DXBC assignment. */
            if (!hlsl_format_checked(
                    ctx, line_buf, line_buf_sz,
                    "[loop] while (%s < %s) {\n", s0,
                    info->typed_bound_alias))
              return;
          } else if (recompile) {
            char comparison_dest[128] = "";
            format_native_dest_operand_hlsl(
                ctx, &next1->operands[0], dest_m, false,
                comparison_dest, sizeof(comparison_dest));
            if (ctx->sb->failed) return;
            if (ctx->use_uint_temps) {
              if (!hlsl_format_checked(
                      ctx, line_buf, line_buf_sz,
                      "[loop] while (%s(%s = ((%s %s %s) ? "
                      "0xFFFFFFFFu : 0u))) {\n",
                      is_nz ? "!" : "", comparison_dest, s0, op_str, s1))
                return;
            } else {
              if (!hlsl_format_checked(
                      ctx, line_buf, line_buf_sz,
                      "[loop] while (%sasuint(%s = asfloat((%s %s %s) ? "
                      "0xFFFFFFFFu : 0u))) {\n",
                      is_nz ? "!" : "", comparison_dest, s0, op_str, s1))
                return;
            }
          } else if (inc_idx != -1) {
            char dest_s0[128] = "";
            format_native_dest_operand_hlsl(ctx, loop_counter, dest_m, false,
                                            dest_s0, sizeof(dest_s0));
            char inc_str[256];
            if (program->instructions[inc_idx].opcode == USIL_OP_IADD) {
              if (ctx->use_uint_temps) {
                if (!hlsl_format_checked(
                        ctx, inc_str, sizeof(inc_str), "%s = %s + 1",
                        dest_s0, dest_s0)) return;
              } else {
                if (!hlsl_format_checked(
                        ctx, inc_str, sizeof(inc_str),
                        "%s = asfloat(asint(%s) + 1)", dest_s0,
                        dest_s0)) return;
              }
            } else {
              if (ctx->use_uint_temps) {
                if (!hlsl_format_checked(
                        ctx, inc_str, sizeof(inc_str),
                        "%s = asuint(asfloat(%s) + 1.0f)", dest_s0,
                        dest_s0)) return;
              } else {
                if (!hlsl_format_checked(
                        ctx, inc_str, sizeof(inc_str), "%s = %s + 1.0f",
                        dest_s0, dest_s0)) return;
              }
            }

            if (!hlsl_format_checked(
                    ctx, line_buf, line_buf_sz,
                    "[loop] for (; %s %s %s; %s) {\n", s0, op_str, s1,
                    inc_str)) return;
          } else {
            if (!hlsl_format_checked(
                    ctx, line_buf, line_buf_sz,
                    "[loop] while (%s %s %s) {\n", s0, op_str, s1))
              return;
          }
          sb_append(sb, line_buf);
          ctx->indent += 4;
          loop_optimized = true;
        }
        if (!loop_optimized) {
          sb_append(sb, "[loop] while (true) {\n");
          ctx->indent += 4;
        }
        break;
      }
      case USIL_OP_ENDLOOP:
        *is_custom = true;
        sb_append(sb, "}\n");
        break;
      case USIL_OP_SWITCH:
        *is_custom = true;
        if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                 "switch (%s) {\n", dest)) return;
        sb_append(sb, line_buf);
        ctx->indent += 4;
        break;
      case USIL_OP_CASE:
        *is_custom = true;
        if (!hlsl_format_checked(ctx, line_buf, line_buf_sz, "case %s:\n",
                                 dest)) return;
        sb_append(sb, line_buf);
        break;
      case USIL_OP_DEFAULT:
        *is_custom = true;
        sb_append(sb, "default:\n");
        break;
      case USIL_OP_ENDSWITCH:
        *is_custom = true;
        sb_append(sb, "}\n");
        break;
      case USIL_OP_BREAK:
        if (!hlsl_copy_checked(ctx, line_buf, line_buf_sz, "break;")) return;
        break;
      case USIL_OP_BREAKC:
        if (inst->condition_test == DXBC_INSTRUCTION_TEST_NONZERO) {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "if (asuint(%s)) break;", dest)) return;
        } else {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "if (!asuint(%s)) break;", dest)) return;
        }
        break;
      case USIL_OP_CONTINUE:
        if (!hlsl_copy_checked(ctx, line_buf, line_buf_sz, "continue;"))
          return;
        break;
      case USIL_OP_CONTINUEC:
        if (inst->condition_test == DXBC_INSTRUCTION_TEST_NONZERO) {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "if (asuint(%s)) continue;", dest)) return;
        } else {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "if (!asuint(%s)) continue;", dest))
            return;
        }
        break;
      case USIL_OP_RET: {
        bool is_vertex = (strncmp(program->shader_type_model, "vs", 2) == 0);
        if (ctx->is_geometry) {
          if (!hlsl_copy_checked(ctx, line_buf, line_buf_sz, "return;"))
            return;
        } else if (is_vertex) {
          /* The caller has already emitted indentation for this instruction.
           * Append the complete return block directly so the maximum-size
           * signature cannot overflow or truncate an intermediate buffer. */
          *is_custom = true;
          int emitted_output = 0;
          for (int output = 0; output < program->output_count; output++) {
            const DXBCSignatureElement *el = &program->outputs[output];
            const uint8_t written_mask = hlsl_output_written_mask(program, el);
            if (written_mask == 0u) continue;
            bool dup = false;
            for (int j = 0; j < output; j++) {
              if (program->outputs[j].register_id == el->register_id) {
                dup = true;
                break;
              }
            }
            const char *semantic = dxbc_signature_semantic_name(el);
            bool omit_index =
                (el->semantic_index == 0 &&
                 strcmp(semantic, "TEXCOORD") != 0);
            char field_name[128];
            if (dup) {
              if (omit_index) {
                if (!hlsl_format_checked(ctx, field_name,
                                         sizeof(field_name), "o_%s",
                                         semantic)) return;
              } else {
                if (!hlsl_format_checked(ctx, field_name,
                                         sizeof(field_name), "o_%s%d",
                                         semantic,
                                         el->semantic_index)) return;
              }
            } else {
              if (!hlsl_format_checked(ctx, field_name, sizeof(field_name),
                                       "o%u", el->register_id)) return;
            }
            char swizzle[16];
            DXBCSignatureElement written_element = *el;
            written_element.mask = written_mask;
            get_signature_swizzle(&written_element, swizzle,
                                  sizeof(swizzle));
            if (emitted_output > 0) {
              sb_append_char(sb, '\n');
              sb_append_spaces(sb, ctx->indent);
            }
            sb_appendf(sb, "output.%s = o%u%s;", field_name,
                       el->register_id, swizzle);
            ++emitted_output;
          }
          sb_append_char(sb, '\n');
          sb_append_spaces(sb, ctx->indent);
          sb_append(sb, "return output;\n");
        } else if (program->output_count == 1) {
          const DXBCSignatureElement *el = &program->outputs[0];
          if (el->register_id == UINT32_MAX &&
              (strcmp(dxbc_signature_semantic_name(el), "SV_Depth") == 0 ||
               strcmp(dxbc_signature_semantic_name(el),
                      "SV_DepthGreaterEqual") == 0 ||
               strcmp(dxbc_signature_semantic_name(el),
                      "SV_DepthLessEqual") == 0)) {
            if (!hlsl_copy_checked(ctx, line_buf, line_buf_sz,
                                   "return oDepth;")) return;
          } else {
            char swizzle[16];
            get_signature_swizzle(el, swizzle, sizeof(swizzle));
            if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                     "return o%u%s;", el->register_id,
                                     swizzle)) return;
          }
        } else {
          if (!hlsl_copy_checked(ctx, line_buf, line_buf_sz, "return;"))
            return;
        }
        break;
      }
      case USIL_OP_DISCARD:
        // discard_nz: discard if operand is non-zero; discard_z: discard if zero
        if (inst->condition_test == DXBC_INSTRUCTION_TEST_NONZERO) {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "clip(asuint(%s) ? -1.0f : 1.0f);",
                                   dest)) return;
        } else {
          if (!hlsl_format_checked(ctx, line_buf, line_buf_sz,
                                   "clip(asuint(%s) ? 1.0f : -1.0f);",
                                   dest)) return;
        }
        break;
      default:
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                   HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
                                   ctx->current_instruction_index, -1);
        break;
    }
}
