// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>


void detect_readable_screen_pos_pattern(HLSLEmitterContext* ctx) {
    const USILProgram* program = ctx->program;
    ctx->readable_screen_pos_mul_y_idx = -1;
    ctx->readable_screen_pos_mul_xzw_idx = -1;
    ctx->readable_screen_pos_add_idx = -1;
    ctx->readable_screen_pos_mov_idx = -1;

    /* The helper rewrite operates on floating-point lvalues. When temporary
     * registers use uint bit storage, emitting the original four DXBC
     * operations preserves their exact types and lets the compiler fold the
     * sequence without constructing invalid asfloat lvalues. */
    if (ctx->use_uint_temps) return;

    /* D3DCompiler also lowers ComputeScreenPos as three split multiplies:
     * projection-scaled y, half-scaled w, half-scaled xz, followed by the
     * clip zw copy and xy add.  Normalize that complete dataflow to the same
     * helper used by the combined .xzw form below. */
    for (int i = 0; i + 4 < program->instruction_count; i++) {
        const USILInstruction *scale_y = &program->instructions[i];
        const USILInstruction *scale_w = &program->instructions[i + 1];
        const USILInstruction *scale_xz = &program->instructions[i + 2];
        const USILInstruction *copy_zw = &program->instructions[i + 3];
        const USILInstruction *add_xy = &program->instructions[i + 4];
        if (scale_y->opcode != USIL_OP_MUL ||
            scale_w->opcode != USIL_OP_MUL ||
            scale_xz->opcode != USIL_OP_MUL ||
            copy_zw->opcode != USIL_OP_MOV ||
            add_xy->opcode != USIL_OP_ADD || scale_y->operand_count < 3 ||
            scale_w->operand_count < 3 || scale_xz->operand_count < 3 ||
            copy_zw->operand_count < 2 || add_xy->operand_count < 3) {
            continue;
        }
        const DXBCOperand *work = &scale_y->operands[0];
        const DXBCOperand *clip = &scale_y->operands[1];
        if (work->type != OPERAND_TYPE_TEMP || work->destination_mask != 16 ||
            clip->type != OPERAND_TYPE_TEMP ||
            scale_y->operands[2].type != OPERAND_TYPE_CONSTANT_BUFFER ||
            scale_w->operands[0].type != OPERAND_TYPE_TEMP ||
            scale_w->operands[0].register_index != work->register_index ||
            scale_w->operands[0].destination_mask != 128 ||
            scale_w->operands[1].type != OPERAND_TYPE_TEMP ||
            scale_w->operands[1].register_index != work->register_index ||
            scale_w->operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
            scale_w->operands[2].imm_values[0] != 0x3f000000u ||
            scale_xz->operands[0].type != OPERAND_TYPE_TEMP ||
            scale_xz->operands[0].register_index != work->register_index ||
            scale_xz->operands[0].destination_mask != 80 ||
            scale_xz->operands[1].type != OPERAND_TYPE_TEMP ||
            scale_xz->operands[1].register_index != clip->register_index ||
            scale_xz->operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
            scale_xz->operands[2].imm_values[0] != 0x3f000000u ||
            copy_zw->operands[0].type != OPERAND_TYPE_OUTPUT ||
            (copy_zw->operands[0].destination_mask != 192 &&
             copy_zw->operands[0].destination_mask != 128) ||
            copy_zw->operands[1].type != OPERAND_TYPE_TEMP ||
            copy_zw->operands[1].register_index != clip->register_index ||
            add_xy->operands[0].type != OPERAND_TYPE_OUTPUT ||
            add_xy->operands[0].register_index !=
                copy_zw->operands[0].register_index ||
            add_xy->operands[0].destination_mask != 48) {
            continue;
        }
        ctx->readable_screen_pos_mul_y_idx = i;
        ctx->readable_screen_pos_mul_xzw_idx = i + 2;
        ctx->readable_screen_pos_mov_idx = i + 3;
        ctx->readable_screen_pos_add_idx = i + 4;
        return;
    }
    
    // Find the vector scale instruction first
    int mul_xzw_idx = -1;
    int regX = -1;
    int regY = -1;
    
    for (int i = 0; i < program->instruction_count; i++) {
        const USILInstruction* inst = &program->instructions[i];
        if (inst->opcode == USIL_OP_MUL && inst->operand_count == 3) {
            const DXBCOperand* dest = &inst->operands[0];
            const DXBCOperand* src0 = &inst->operands[1];
            const DXBCOperand* src1 = &inst->operands[2];
            
            if (dest->type == OPERAND_TYPE_TEMP && dest->destination_mask == 208) { // .xzw
                if (src0->type == OPERAND_TYPE_TEMP && src0->swizzle_mode == 1) { // swizzle mode 1 (xyzw)
                    if (src0->swizzle[0] == 0 && src0->swizzle[1] == 0 &&
                        src0->swizzle[2] == 3 && src0->swizzle[3] == 1) { // .xxwy
                        if (src1->type == OPERAND_TYPE_IMMEDIATE32 && src1->imm_value_count == 4) {
                            float* imm = (float*)src1->imm_values;
                            if (imm[0] == 0.5f && imm[1] == 0.0f && imm[2] == 0.5f && imm[3] == 0.5f) {
                                mul_xzw_idx = i;
                                regX = src0->register_index;
                                regY = dest->register_index;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (mul_xzw_idx == -1) {
        return;
    }
    
    // 1. Scan backwards for the projection scaling: mul regX.y, regX.y, cb[B].x
    int mul_y_idx = -1;
    for (int i = mul_xzw_idx - 1; i >= 0; i--) {
        const USILInstruction* inst = &program->instructions[i];
        if (inst->opcode == USIL_OP_MUL && inst->operand_count == 3) {
            const DXBCOperand* dest = &inst->operands[0];
            const DXBCOperand* src0 = &inst->operands[1];
            const DXBCOperand* src1 = &inst->operands[2];
            
            if (dest->type == OPERAND_TYPE_TEMP && dest->register_index == regX && dest->destination_mask == 32) { // .y
                if (src0->type == OPERAND_TYPE_TEMP && src0->register_index == regX) {
                    int src0_comp = 1;
                    if (src0->swizzle_mode == 1) {
                        src0_comp = src0->swizzle[1];
                    } else if (src0->swizzle_mode == 2) {
                        src0_comp = src0->swizzle[0];
                    }
                    if (src0_comp == 1) {
                        if (src1->type == OPERAND_TYPE_CONSTANT_BUFFER) {
                            mul_y_idx = i;
                            break;
                        }
                    }
                }
            }
        }
    }
    
    // 2. Scan forwards for the offset addition: add oOut.xy/zw, regY.zzzz, regY.xxxw
    int add_idx = -1;
    int regOut = -1;
    int out_mask = 0;
    for (int i = mul_xzw_idx + 1; i < program->instruction_count; i++) {
        const USILInstruction* inst = &program->instructions[i];
        if (inst->opcode == USIL_OP_ADD && inst->operand_count == 3) {
            const DXBCOperand* dest = &inst->operands[0];
            const DXBCOperand* src0 = &inst->operands[1];
            const DXBCOperand* src1 = &inst->operands[2];
            
            if (dest->type == OPERAND_TYPE_OUTPUT && (dest->destination_mask == 48 || dest->destination_mask == 192)) { // .xy or .zw
                if (src0->type == OPERAND_TYPE_TEMP && src0->register_index == regY) {
                    int src0_comp = 2;
                    if (src0->swizzle_mode == 1) {
                        src0_comp = src0->swizzle[0];
                    } else if (src0->swizzle_mode == 2) {
                        src0_comp = src0->swizzle[0];
                    }
                    if (src0_comp == 2) { // .zzzz
                        if (src1->type == OPERAND_TYPE_TEMP && src1->register_index == regY && src1->swizzle_mode == 1) {
                            int swiz0 = -1;
                            int swiz1 = -1;
                            if (dest->destination_mask == 48) { // .xy
                                swiz0 = src1->swizzle[0];
                                swiz1 = src1->swizzle[1];
                            } else if (dest->destination_mask == 192) { // .zw
                                swiz0 = src1->swizzle[2];
                                swiz1 = src1->swizzle[3];
                            }
                            if (swiz0 == 0 && swiz1 == 3) { // .xwxx equivalent components
                                add_idx = i;
                                regOut = dest->register_index;
                                out_mask = dest->destination_mask;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (add_idx == -1) {
        return;
    }
    
    // 3. Scan for the optional mov: mov oOut.zw/xy, regX.zw (if out_mask is xy, we copy to zw. if out_mask is zw, there's no mov)
    int mov_idx = -1;
    if (out_mask == 48) { // .xy
        // We look for: mov oOut.zw, regX.zw
        for (int i = 0; i < program->instruction_count; i++) {
            const USILInstruction* inst = &program->instructions[i];
            if (inst->opcode == USIL_OP_MOV && inst->operand_count == 2) {
                const DXBCOperand* dest = &inst->operands[0];
                const DXBCOperand* src0 = &inst->operands[1];
                
                if (dest->type == OPERAND_TYPE_OUTPUT && dest->register_index == regOut && dest->destination_mask == 192) { // .zw
                    if (src0->type == OPERAND_TYPE_TEMP && src0->register_index == regX) {
                        if (src0->swizzle_mode == 1 && src0->swizzle[2] == 2 && src0->swizzle[3] == 3) { // .zw
                            mov_idx = i;
                            break;
                        }
                    }
                }
            }
        }
    }
    
    // Successfully matched!
    ctx->readable_screen_pos_mul_y_idx = mul_y_idx;
    ctx->readable_screen_pos_mul_xzw_idx = mul_xzw_idx;
    ctx->readable_screen_pos_add_idx = add_idx;
    ctx->readable_screen_pos_mov_idx = mov_idx;

}

// Build the sampler → texture register mapping by scanning sample instructions.
static int get_src_logical_comp(HLSLEmitterContext* ctx, int inst_idx, const DXBCOperand* src_op, int c) {
    if (src_op->type != OPERAND_TYPE_TEMP) {
        return c;
    }
    int phys_comp = c;
    if (src_op->swizzle_mode == 1) {
        phys_comp = src_op->swizzle[c];
    } else if (src_op->swizzle_mode == 2) {
        phys_comp = src_op->swizzle[0];
    }
    
    int src_reg = src_op->register_index;
    if (src_reg >= 0 && src_reg < ctx->temp_state_count) {
        return (*hlsl_register_permutation_at_const(
            ctx, inst_idx, src_reg))[phys_comp];
    }
    return phys_comp;
}

void PreScanScrambledRegisters(HLSLEmitterContext* ctx) {
    const USILProgram* program = ctx->program;
    /* Every state is explicitly initialized to the physical identity layout.
     * This is the complete recompile-mode behavior.  READABLE may then
     * propagate inferred permutations for presentation only. */
    for (int state = 0; state <= program->instruction_count; ++state) {
        for (int r = 0; r < ctx->temp_state_count; r++) {
            for (int c = 0; c < 4; c++) {
                (*hlsl_register_permutation_at(ctx, state, r))[c] = c;
            }
            *hlsl_register_scrambled_at(ctx, state, r) = false;
        }
    }
    if (!hlsl_readability_transforms_enabled(ctx)) return;

    for (int i = 0; i < program->instruction_count; i++) {
        const USILInstruction* inst = &program->instructions[i];
        // Copy the layout from instruction i to i + 1
        for (int r = 0; r < ctx->temp_state_count; r++) {
            for (int c = 0; c < 4; c++) {
                (*hlsl_register_permutation_at(ctx, i + 1, r))[c] =
                    (*hlsl_register_permutation_at_const(ctx, i, r))[c];
            }
        }
        
        // If the instruction writes to a temp register, update its layout
        if (inst_writes_to_dest(inst)) {
            const DXBCOperand* dest = &inst->operands[0];
            if (dest->type == OPERAND_TYPE_TEMP) {
                int dest_reg = dest->register_index;
                if (dest_reg >= 0 && dest_reg < ctx->temp_state_count) {
                    int dest_mask = dest->destination_mask;
                    if (dest_mask == 0) dest_mask = 240;
                    
                    for (int c = 0; c < 4; c++) {
                        if (dest_mask & (16 << c)) {
                            if (inst->opcode == USIL_OP_MOV || inst->opcode == USIL_OP_MOVC) {
                                int logical_comp = -1;
                                if (inst->opcode == USIL_OP_MOV) {
                                    logical_comp = get_src_logical_comp(ctx, i, &inst->operands[1], c);
                                } else { // MOVC
                                    const DXBCOperand* src0 = &inst->operands[2];
                                    const DXBCOperand* src1 = &inst->operands[3];
                                    if (src0->type == OPERAND_TYPE_TEMP || src0->type == OPERAND_TYPE_INDEXABLE_TEMP) {
                                        logical_comp = get_src_logical_comp(ctx, i, src0, c);
                                    } else {
                                        logical_comp = get_src_logical_comp(ctx, i, src1, c);
                                    }
                                }
                                (*hlsl_register_permutation_at(
                                    ctx, i + 1, dest_reg))[c] = logical_comp;
                            } else {
                                (*hlsl_register_permutation_at(
                                    ctx, i + 1, dest_reg))[c] = c;
                            }
                        }
                    }
                    
                    bool is_permutation = true;
                    bool seen[4] = {false, false, false, false};
                    for (int c = 0; c < 4; c++) {
                        int val = (*hlsl_register_permutation_at_const(
                            ctx, i + 1, dest_reg))[c];
                        if (val < 0 || val > 3 || seen[val]) {
                            is_permutation = false;
                            break;
                        }
                        seen[val] = true;
                    }
                    if (!is_permutation) {
                        for (int c = 0; c < 4; c++) {
                            (*hlsl_register_permutation_at(
                                ctx, i + 1, dest_reg))[c] = c;
                        }
                    }
                }
            }
        }
        
        // Update is_scrambled for step i + 1
        for (int r = 0; r < ctx->temp_state_count; r++) {
            bool scrambled = false;
            if (is_register_decomposed(ctx, r)) {
                for (int c = 0; c < 4; c++) {
                    (*hlsl_register_permutation_at(ctx, i + 1, r))[c] = c;
                }
            } else {
                for (int c = 0; c < 4; c++) {
                    if ((*hlsl_register_permutation_at_const(
                            ctx, i + 1, r))[c] != c) {
                        scrambled = true;
                        break;
                    }
                }
            }
            *hlsl_register_scrambled_at(ctx, i + 1, r) = scrambled;
        }
        
    }
}

void detect_signed_modulo_pattern(HLSLEmitterContext* ctx) {
    const USILProgram* program = ctx->program;
    for (int i = 0; i < program->instruction_count; i++) {
        ctx->modulo_divisor[i] = 0;
    }
    if (!hlsl_readability_transforms_enabled(ctx)) return;

    for (int i = 0; i + 4 < program->instruction_count; i++) {
        const USILInstruction* inst0 = &program->instructions[i];
        const USILInstruction* inst1 = &program->instructions[i + 1];
        const USILInstruction* inst2 = &program->instructions[i + 2];
        const USILInstruction* inst3 = &program->instructions[i + 3];
        const USILInstruction* inst4 = &program->instructions[i + 4];

        // 1. Inst i: AND rA.compX, rB.compY, 0x80000000
        if (inst0->opcode != USIL_OP_AND || inst0->operand_count < 3) continue;
        const DXBCOperand* dest0 = &inst0->operands[0];
        const DXBCOperand* src0_0 = &inst0->operands[1];
        const DXBCOperand* src0_1 = &inst0->operands[2];
        if (dest0->type != OPERAND_TYPE_TEMP) continue;
        if (src0_1->type != OPERAND_TYPE_IMMEDIATE32 || src0_1->imm_value_count != 1 || src0_1->imm_values[0] != 0x80000000u) continue;

        // 2. Inst i+1: IMAX rA.compY, rB.compY, -rB.compY (or MAX)
        if (inst1->opcode != USIL_OP_IMAX && inst1->opcode != USIL_OP_MAX) continue;
        if (inst1->operand_count < 3) continue;
        const DXBCOperand* dest1 = &inst1->operands[0];
        const DXBCOperand* src1_0 = &inst1->operands[1];
        const DXBCOperand* src1_1 = &inst1->operands[2];
        if (dest1->type != OPERAND_TYPE_TEMP || dest1->register_index != dest0->register_index) continue;
        if (src1_0->type != src0_0->type || src1_0->register_index != src0_0->register_index) continue;
        if (src1_0->swizzle_mode != src0_0->swizzle_mode || src1_0->swizzle[0] != src0_0->swizzle[0]) continue;
        if (src1_1->type != src0_0->type || src1_1->register_index != src0_0->register_index) continue;
        if (src1_1->swizzle_mode != src0_0->swizzle_mode || src1_1->swizzle[0] != src0_0->swizzle[0] || !src1_1->has_neg) continue;

        // Get compX and compY component indices
        int compX = -1;
        for (int c = 0; c < 4; c++) {
            if (dest0->destination_mask & (16 << c)) { compX = c; break; }
        }
        int compY = -1;
        for (int c = 0; c < 4; c++) {
            if (dest1->destination_mask & (16 << c)) { compY = c; break; }
        }
        if (compX == -1 || compY == -1) continue;

        // 3. Inst i+2: AND rA.compY, rA.compY, l(N)
        if (inst2->opcode != USIL_OP_AND || inst2->operand_count < 3) continue;
        const DXBCOperand* dest2 = &inst2->operands[0];
        const DXBCOperand* src2_0 = &inst2->operands[1];
        const DXBCOperand* src2_1 = &inst2->operands[2];
        if (dest2->type != OPERAND_TYPE_TEMP || dest2->register_index != dest0->register_index) continue;
        if (!(dest2->destination_mask & (16 << compY))) continue;
        if (src2_0->type != OPERAND_TYPE_TEMP || src2_0->register_index != dest0->register_index) continue;
        if (src2_0->swizzle_mode != 1 && src2_0->swizzle_mode != 2) continue;
        if (src2_0->swizzle[0] != compY) continue;
        if (src2_1->type != OPERAND_TYPE_IMMEDIATE32 || src2_1->imm_value_count != 1) continue;
        uint32_t val_N = src2_1->imm_values[0];
        // N must be 2^k - 1 (e.g. 1, 3, 7, 15, 31, 63, 127)
        if ((val_N & (val_N + 1)) != 0 || val_N == 0) continue;

        // 4. Inst i+3: INEG rA.compZ, rA.compY
        if (inst3->opcode != USIL_OP_INEG || inst3->operand_count < 2) continue;
        const DXBCOperand* dest3 = &inst3->operands[0];
        const DXBCOperand* src3_0 = &inst3->operands[1];
        if (dest3->type != OPERAND_TYPE_TEMP || dest3->register_index != dest0->register_index) continue;
        int compZ = -1;
        for (int c = 0; c < 4; c++) {
            if (dest3->destination_mask & (16 << c)) { compZ = c; break; }
        }
        if (compZ == -1) continue;
        if (src3_0->type != OPERAND_TYPE_TEMP || src3_0->register_index != dest0->register_index) continue;
        if (src3_0->swizzle_mode != 1 && src3_0->swizzle_mode != 2) continue;
        if (src3_0->swizzle[0] != compY) continue;

        // 5. Inst i+4: MOVC rA.compX, rA.compX, rA.compZ, rA.compY
        if (inst4->opcode != USIL_OP_MOVC || inst4->operand_count < 4) continue;
        const DXBCOperand* dest4 = &inst4->operands[0];
        const DXBCOperand* src4_0 = &inst4->operands[1];
        const DXBCOperand* src4_1 = &inst4->operands[2];
        const DXBCOperand* src4_2 = &inst4->operands[3];
        if (dest4->type != OPERAND_TYPE_TEMP || dest4->register_index != dest0->register_index) continue;
        if (!(dest4->destination_mask & (16 << compX))) continue;
        if (src4_0->type != OPERAND_TYPE_TEMP || src4_0->register_index != dest0->register_index) continue;
        if (src4_0->swizzle_mode != 1 && src4_0->swizzle_mode != 2) continue;
        if (src4_0->swizzle[0] != compX) continue;
        if (src4_1->type != OPERAND_TYPE_TEMP || src4_1->register_index != dest0->register_index) continue;
        if (src4_1->swizzle_mode != 1 && src4_1->swizzle_mode != 2) continue;
        if (src4_1->swizzle[0] != compZ) continue;
        if (src4_2->type != OPERAND_TYPE_TEMP || src4_2->register_index != dest0->register_index) continue;
        if (src4_2->swizzle_mode != 1 && src4_2->swizzle_mode != 2) continue;
        if (src4_2->swizzle[0] != compY) continue;

        // Pattern matched!
        int divisor = val_N + 1;
        ctx->modulo_divisor[i] = divisor;
    }
}

void detect_loop_patterns(HLSLEmitterContext* ctx) {
    const USILProgram* program = ctx->program;
    for (int i = 0; i < program->instruction_count; i++) {
        ctx->loop_info[i].is_optimized = false;
        ctx->loop_info[i].comparison_inst_idx = -1;
        ctx->loop_info[i].breakc_inst_idx = -1;
        ctx->loop_info[i].inc_inst_idx = -1;
        ctx->loop_info[i].typed_bound_ftoi_idx = -1;
        ctx->loop_info[i].typed_bound_imax_idx = -1;
        ctx->loop_info[i].typed_bound_imin_idx = -1;
        ctx->loop_info[i].typed_bound_itof_idx = -1;
        ctx->loop_info[i].typed_bound_alias[0] = '\0';
        if (ctx->typed_loop_bound_instructions) {
            ctx->typed_loop_bound_instructions[i].loop_instruction = -1;
            ctx->typed_loop_bound_instructions[i].phase =
                HLSL_TYPED_LOOP_BOUND_NONE;
        }
    }
    for (int i = 0; i < program->instruction_count; i++) {
        const USILInstruction* inst = &program->instructions[i];
        if (inst->opcode != USIL_OP_LOOP) continue;

        if (i + 2 < program->instruction_count) {
            const USILInstruction *next1 = &program->instructions[i + 1];
            const USILInstruction *next2 = &program->instructions[i + 2];
            bool comparison_opcode =
                next1->opcode == USIL_OP_GE || next1->opcode == USIL_OP_LT ||
                next1->opcode == USIL_OP_EQ || next1->opcode == USIL_OP_NE ||
                next1->opcode == USIL_OP_IGE || next1->opcode == USIL_OP_ILT ||
                next1->opcode == USIL_OP_IEQ || next1->opcode == USIL_OP_INE ||
                next1->opcode == USIL_OP_UGE || next1->opcode == USIL_OP_ULT;
            int comparison_component = -1;
            if (next1->operand_count == 3 &&
                !next1->saturate &&
                next1->operands[0].type == OPERAND_TYPE_TEMP &&
                !next1->operands[0].has_abs &&
                !next1->operands[0].has_neg &&
                next1->operands[0].register_index_dim <= 1 &&
                !next1->operands[0].rel_op0 &&
                !next1->operands[0].rel_op1) {
                for (int component = 0; component < 4; component++) {
                    if (next1->operands[0].destination_mask ==
                        (16 << component)) {
                        comparison_component = component;
                        break;
                    }
                }
            }
            bool exact_break_operand =
                next2->opcode == USIL_OP_BREAKC &&
                next2->operand_count == 1 && comparison_component >= 0 &&
                next2->operands[0].type == OPERAND_TYPE_TEMP &&
                next2->operands[0].register_index ==
                    next1->operands[0].register_index &&
                next2->operands[0].swizzle_mode == 2 &&
                next2->operands[0].swizzle[0] == comparison_component &&
                !next2->operands[0].has_abs &&
                !next2->operands[0].has_neg &&
                next2->operands[0].register_index_dim <= 1 &&
                !next2->operands[0].rel_op0 &&
                !next2->operands[0].rel_op1;

            if (comparison_opcode && exact_break_operand) {
                // Find loop counter register
                const DXBCOperand *loop_counter = &next1->operands[1];
                /* The CFG owns structured-flow matching. Reusing its map
                 * also keeps nested SWITCH/IF scopes under one authority. */
                if (!ctx->cfg.instruction_flow ||
                    ctx->cfg.instruction_count != program->instruction_count)
                    continue;
                int end_idx = ctx->cfg.instruction_flow[i].end;
                if (end_idx <= i || end_idx >= program->instruction_count)
                    continue;

                // Search for a presentation-only increment lift.  Recompile
                // mode leaves the decoded latch instruction in place; only
                // the proven comparison/BREAKC prefix is reconstructed.
                int inc_idx = -1;
                if (hlsl_readability_transforms_enabled(ctx)) {
                    for (int k = i + 3; k < end_idx; k++) {
                        if (is_increment_of(&program->instructions[k], loop_counter)) {
                            inc_idx = k;
                            break;
                        }
                    }
                }

                // Store info
                LoopOptimizationInfo* info = &ctx->loop_info[i];
                info->is_optimized = true;
                info->comparison_inst_idx = i + 1;
                info->breakc_inst_idx = i + 2;
                info->inc_inst_idx = inc_idx;

            }
        }
    }
}

void detect_cross_product_patterns(HLSLEmitterContext* ctx) {
    const USILProgram* program = ctx->program;
    for (int i = 0; i < program->instruction_count; i++) {
        ctx->cross_info[i].is_cross_mul = false;
        ctx->cross_info[i].is_cross_mad = false;
        ctx->cross_info[i].source_instruction = -1;
    }
    if (!hlsl_readability_transforms_enabled(ctx)) return;

    for (int i = 0; i + 1 < program->instruction_count; i++) {
        int dummy_op1 = -1, dummy_op2 = -1;
        if (is_cross_product_pattern(program, i, &dummy_op1, &dummy_op2)) {
            ctx->cross_info[i].is_cross_mul = true;
            ctx->cross_info[i + 1].is_cross_mad = true;
            ctx->cross_info[i + 1].source_instruction = i;
        }
    }
}

