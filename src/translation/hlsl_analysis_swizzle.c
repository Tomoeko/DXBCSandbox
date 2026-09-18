// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>


void PreScanSwizzleDecomposition(HLSLEmitterContext* ctx) {
    const USILProgram* program = ctx->program;
    const char* comp_names[] = { "x", "y", "z", "w" };

    /* This is a source-readability transform: it redirects physical register
     * writes into inferred typed locals and may suppress later multi-writes.
     * Such an inference is never admissible in the recompilation path. */
    if (ctx->temp_state_count > 0) {
        const size_t count = (size_t)ctx->temp_state_count;
        memset(ctx->decompositions, 0,
               count * sizeof(*ctx->decompositions));
        memset(ctx->has_decomposition, 0,
               count * sizeof(*ctx->has_decomposition));
        memset(ctx->has_ftoi_temp, 0,
               count * sizeof(*ctx->has_ftoi_temp));
        memset(ctx->has_int_temp, 0,
               count * sizeof(*ctx->has_int_temp));
        memset(ctx->has_write_redirect, 0,
               count * sizeof(*ctx->has_write_redirect));
        memset(ctx->write_redirect_storage, 0,
               count * sizeof(*ctx->write_redirect_storage));
        memset(ctx->has_deferred_float, 0,
               count * sizeof(*ctx->has_deferred_float));
    }
    if (!hlsl_readability_transforms_enabled(ctx)) return;
    
    for (int i = 0; i < program->instruction_count; i++) {
        const USILInstruction* inst = &program->instructions[i];
        if (inst->opcode != USIL_OP_MOV) continue;
        if (inst->operand_count != 2) continue;
        
        const DXBCOperand* src = &inst->operands[1];
        const DXBCOperand* dest = &inst->operands[0];
        
        if (src->type != OPERAND_TYPE_TEMP || dest->type != OPERAND_TYPE_TEMP) continue;
        if (src->has_neg || src->has_abs) continue;
        
        int writeMask = dest->destination_mask;
        int componentCount = get_mask_component_count(writeMask);
        if (componentCount != 3) continue; // Only handle 3-component moves
        
        int destLanes[3];
        int srcComponents[3];
        int idx = 0;
        for (int c = 0; c < 4; c++) {
            if (writeMask & (16 << c)) {
                destLanes[idx] = c;
                if (src->swizzle_mode == 1) {
                    srcComponents[idx] = src->swizzle[c];
                } else {
                    srcComponents[idx] = c;
                }
                idx++;
            }
        }
        
        bool isIdentity = true;
        for (int k = 0; k < componentCount; k++) {
            if (srcComponents[k] != destLanes[k]) { isIdentity = false; break; }
        }
        if (isIdentity) continue;
        
        int srcReg = src->register_index;
        int destReg = dest->register_index;
        if (srcReg < 0 || destReg < 0 || srcReg == destReg) continue;
        
        // Verify source register is only written by single-component ops in local scope
        int writeInstIndices[3];
        int writeInstCount = 0;
        bool writtenComps[4] = {false};
        int writtenCompsCount = 0;
        bool valid = true;
        
        for (int j = i - 1; j >= 0; j--) {
            const USILInstruction* prev = &program->instructions[j];
            if (prev->opcode == USIL_OP_IF || prev->opcode == USIL_OP_ELSE || prev->opcode == USIL_OP_ENDIF ||
                prev->opcode == USIL_OP_LOOP || prev->opcode == USIL_OP_ENDLOOP ||
                prev->opcode == USIL_OP_SWITCH || prev->opcode == USIL_OP_CASE ||
                prev->opcode == USIL_OP_DEFAULT || prev->opcode == USIL_OP_ENDSWITCH) {
                break;
            }
            
            // Check for reads of srcReg (other than our mov)
            bool readsSrcReg = false;
            for (int op_idx = 1; op_idx < prev->operand_count; op_idx++) {
                const DXBCOperand* prevSrc = &prev->operands[op_idx];
                if (prevSrc->type == OPERAND_TYPE_TEMP && prevSrc->register_index == srcReg) {
                    readsSrcReg = true;
                    break;
                }
            }
            if (readsSrcReg) {
                valid = false;
                break;
            }
            
            // Check if prev writes to srcReg
            if (prev->operand_count >= 1 && prev->operands[0].type == OPERAND_TYPE_TEMP &&
                prev->operands[0].register_index == srcReg) {
                int prevMask = prev->operands[0].destination_mask;
                int prevBits = get_mask_component_count(prevMask);
                if (prevBits != 1) {
                    valid = false;
                    break;
                }
                
                int writtenComp = 0;
                for (int b = 0; b < 4; b++) {
                    if (prevMask & (16 << b)) {
                        writtenComp = b;
                        break;
                    }
                }
                
                if (!writtenComps[writtenComp]) {
                    writtenComps[writtenComp] = true;
                    writtenCompsCount++;
                }
                writeInstIndices[writeInstCount++] = j;
                if (writtenCompsCount == componentCount) {
                    break;
                }
            }
        }
        
        if (!valid || writtenCompsCount != componentCount) continue;
        
        // Determine which write instruction is an integer op
        int intSrcComp = -1;
        for (int k = 0; k < writeInstCount; k++) {
            int j = writeInstIndices[k];
            const USILInstruction* prev = &program->instructions[j];
            if (is_scalar_integer_op(prev->opcode)) {
                int prevMask = prev->operands[0].destination_mask;
                for (int b = 0; b < 4; b++) {
                    if (prevMask & (16 << b)) {
                        intSrcComp = b;
                        break;
                    }
                }
                break;
            }
        }
        if (intSrcComp < 0) continue;
        
        // Find which dest component receives the integer
        int intDestComp = -1;
        for (int k = 0; k < componentCount; k++) {
            if (srcComponents[k] == intSrcComp) {
                intDestComp = destLanes[k];
                break;
            }
        }
        if (intDestComp < 0) continue;
        
        // Build float dest components
        int floatDestComps[2];
        int floatDestCompsCount = 0;
        for (int k = 0; k < componentCount; k++) {
            if (destLanes[k] != intDestComp) {
                floatDestComps[floatDestCompsCount++] = destLanes[k];
            }
        }
        if (floatDestCompsCount != 2) continue;
        
        // Build decomposition info
        SwizzleDecomposition decomp;
        memset(&decomp, 0, sizeof(decomp));
        decomp.DestRegIdx = destReg;
        decomp.SrcRegIdx = srcReg;
        decomp.IntDestComp = intDestComp;
        decomp.FloatDestComps[0] = floatDestComps[0];
        decomp.FloatDestComps[1] = floatDestComps[1];
        
        if (!hlsl_format_checked(ctx, decomp.IntVarName,
                                 sizeof(decomp.IntVarName), "u_xlati%d_%s",
                                 destReg, comp_names[intDestComp]) ||
            !hlsl_format_checked(ctx, decomp.Float2VarName,
                                 sizeof(decomp.Float2VarName),
                                 "u_xlat%d_%s%s", destReg,
                                 comp_names[floatDestComps[0]],
                                 comp_names[floatDestComps[1]])) return;
                 
        for (int k = 0; k < componentCount; k++) {
            decomp.Permutation[k] = srcComponents[k];
        }
        
        ctx->decompositions[destReg] = decomp;
        ctx->has_decomposition[destReg] = true;
        
        // Populate IntTemps and FtoiTemps
        if (!hlsl_copy_checked(ctx, ctx->int_temps[destReg][intDestComp], 64,
                               decomp.IntVarName)) return;
        ctx->has_int_temp[destReg][intDestComp] = true;
        
        if (!hlsl_format_checked(
                ctx, ctx->ftoi_temps[destReg][floatDestComps[0]], 64,
                "%s.x", decomp.Float2VarName)) return;
        ctx->has_ftoi_temp[destReg][floatDestComps[0]] = true;
        
        if (!hlsl_format_checked(
                ctx, ctx->ftoi_temps[destReg][floatDestComps[1]], 64,
                "%s.y", decomp.Float2VarName)) return;
        ctx->has_ftoi_temp[destReg][floatDestComps[1]] = true;
        
        // Build write redirects on source register
        for (int k = 0; k < componentCount; k++) {
            int srcComp = srcComponents[k];
            int dstLane = destLanes[k];
            
            if (dstLane == intDestComp) {
                if (!hlsl_copy_checked(ctx,
                                       ctx->write_redirects[srcReg][srcComp],
                                       64, decomp.IntVarName)) return;
                ctx->has_write_redirect[srcReg][srcComp] = true;
                ctx->write_redirect_storage[srcReg][srcComp] =
                    HLSL_BACKING_STORAGE_SINT;
            } else {
                int float2Idx = (dstLane == floatDestComps[0]) ? 0 : 1;
                if (!hlsl_format_checked(
                        ctx, ctx->write_redirects[srcReg][srcComp], 64,
                        "%s.%c", decomp.Float2VarName,
                        "xy"[float2Idx])) return;
                if (!hlsl_format_checked(
                        ctx, ctx->deferred_floats[srcReg][srcComp], 64,
                        "_sw%d", srcComp)) return;
                ctx->has_deferred_float[srcReg][srcComp] = true;
                ctx->write_redirect_storage[srcReg][srcComp] =
                    HLSL_BACKING_STORAGE_FLOAT;
            }
        }
    }
}

bool is_register_decomposed(HLSLEmitterContext* ctx, int regIdx) {
    if (!hlsl_readability_transforms_enabled(ctx) || regIdx < 0 ||
        regIdx >= ctx->temp_state_count) return false;
    if (ctx->has_decomposition[regIdx]) return true;
    for (int idx = 0; idx < ctx->temp_state_count; idx++) {
        if (ctx->has_decomposition[idx] && ctx->decompositions[idx].SrcRegIdx == regIdx) {
            return true;
        }
    }
    return false;
}

bool is_increment_of(const USILInstruction *inst, const DXBCOperand *loop_counter) {
  if ((inst->opcode == USIL_OP_IADD || inst->opcode == USIL_OP_ADD) && inst->operand_count >= 3) {
    const DXBCOperand *dest = &inst->operands[0];
    const DXBCOperand *src0 = &inst->operands[1];
    const DXBCOperand *src1 = &inst->operands[2];

    if (dest->type == loop_counter->type && dest->register_index == loop_counter->register_index &&
        src0->type == loop_counter->type && src0->register_index == loop_counter->register_index) {

      int dest_mask = dest->destination_mask;
      int lc_mask = 0;
      if (loop_counter->swizzle_mode == 1) {
        lc_mask = 16 << loop_counter->swizzle[0];
      } else if (loop_counter->swizzle_mode == 2) {
        lc_mask = 16 << loop_counter->swizzle[0];
      } else {
        lc_mask = dest_mask;
      }

      if (dest_mask == lc_mask) {
        if (src1->type == OPERAND_TYPE_IMMEDIATE32) {
          if (inst->opcode == USIL_OP_IADD) {
            return (src1->imm_values[0] == 1);
          } else {
            float f;
            memcpy(&f, &src1->imm_values[0], sizeof(f));
            return (f == 1.0f);
          }
        }
      }
    }
  }
  return false;
}

