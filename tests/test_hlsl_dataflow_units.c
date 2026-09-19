// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_copy_lift.h"
#include "translation/hlsl_lift_transaction.h"
#include "dxbc/dxbc_hash.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static DXBCOperand reg(DXBCOperandType type, int index, uint8_t mask) {
    DXBCOperand result = {0};
    result.type = type;
    result.register_index = index;
    result.destination_mask = mask;
    result.swizzle_mode = 1;
    for (uint8_t lane = 0; lane < 4; ++lane)
        result.swizzle[lane] = lane;
    return result;
}

static USILInstruction move(DXBCOperand destination, DXBCOperand source) {
    USILInstruction instruction = {0};
    instruction.opcode = USIL_OP_MOV;
    instruction.operand_count = 2;
    instruction.operands[0] = destination;
    instruction.operands[1] = source;
    return instruction;
}

static bool analyze(HLSLEmitterContext *ctx, USILProgram *program) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->program = program;
    return build_control_flow_graph(ctx) && compute_dominance(&ctx->cfg) &&
           build_hlsl_ssa_graph(ctx) && build_component_provenance(ctx) &&
           build_hlsl_use_def_graph(ctx);
}

static void dispose(HLSLEmitterContext *ctx) {
    free_hlsl_use_def_graph(ctx);
    free_component_provenance(ctx);
    free_hlsl_ssa_graph(ctx);
    free_control_flow_graph(ctx);
}

static int variable(const HLSLEmitterContext *ctx, int instruction, int operand, int lane) {
    return ctx->ssa
        .operand_ssa_vars[((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u +
                          (size_t)lane];
}

static bool check_multiple_results(void) {
    const USILOpcode opcodes[] = {USIL_OP_IMUL, USIL_OP_UDIV, USIL_OP_SINCOS};
    for (size_t kind = 0; kind < sizeof(opcodes) / sizeof(opcodes[0]); ++kind) {
        USILInstruction instructions[5] = {0};
        instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0xf0), reg(OPERAND_TYPE_INPUT, 0, 0));
        instructions[1].opcode = opcodes[kind];
        instructions[1].operand_count = opcodes[kind] == USIL_OP_SINCOS ? 3 : 4;
        instructions[1].operands[0] = reg(OPERAND_TYPE_TEMP, 1, 0x10);
        instructions[1].operands[1] = reg(OPERAND_TYPE_TEMP, 2, 0x10);
        instructions[1].operands[2] = reg(OPERAND_TYPE_TEMP, 0, 0);
        instructions[1].operands[2].swizzle[0] = 3;
        instructions[1].operands[3] = reg(OPERAND_TYPE_TEMP, 0, 0);
        instructions[2] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10), reg(OPERAND_TYPE_TEMP, 2, 0));
        instructions[3] = move(reg(OPERAND_TYPE_OUTPUT, 1, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
        instructions[4].opcode = USIL_OP_RET;
        USILProgram program = {
            .instructions = instructions, .instruction_count = 5, .temp_count = 3};
        HLSLEmitterContext ctx;
        CHECK(analyze(&ctx, &program));
        CHECK(hlsl_operand_definition(&ctx, 2, 1, 0) == 1);
        CHECK(variable(&ctx, 1, 0, 0) != variable(&ctx, 1, 1, 0));
        CHECK(variable(&ctx, 1, 1, 0) == variable(&ctx, 2, 1, 0));
        CHECK(hlsl_operand_definition(&ctx, 1, 2, 1) == HLSL_DEFINITION_UNKNOWN);
        CHECK(hlsl_definition_use_count(&ctx, 0, 1) == 0);
        CHECK(hlsl_definition_use_count(&ctx, 0, 2) == 0);
        CHECK(hlsl_definition_use_count(&ctx, 0, 3) == 1);
        CHECK(hlsl_definition_use_count(&ctx, 1, 0) == 1);
        CHECK(get_component_provenance(&ctx, 2, 2, 0)->definition_instruction == 1);
        CHECK(get_component_provenance(&ctx, 2, 2, 1)->definition_instruction == -1);
        dispose(&ctx);

        /* Reading the previous value and overwriting the same lane happens
         * simultaneously; the second destination is not a phantom source. */
        instructions[1].operands[1].register_index = 0;
        instructions[2].operands[1].register_index = 0;
        CHECK(analyze(&ctx, &program));
        CHECK(hlsl_operand_definition(&ctx, 1, 2, 0) == 0);
        CHECK(hlsl_operand_definition(&ctx, 2, 1, 0) == 1);
        if (instructions[1].operand_count == 4) {
            CHECK(variable(&ctx, 1, 3, 0) == variable(&ctx, 0, 0, 0));
            CHECK(variable(&ctx, 1, 3, 0) != variable(&ctx, 1, 1, 0));
        }
        dispose(&ctx);
    }
    return true;
}

static bool check_modified_moves(void) {
    for (int modifier = 0; modifier < 5; ++modifier) {
        USILInstruction instructions[3] = {0};
        instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, 0, 0));
        instructions[1] = move(reg(OPERAND_TYPE_TEMP, 1, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
        instructions[1].operands[1].has_neg = modifier == 1;
        instructions[1].operands[1].has_abs = modifier == 2;
        instructions[1].saturate = modifier == 3;
        instructions[1].operands[1].min_precision = modifier == 4 ? 1 : 0;
        instructions[2].opcode = USIL_OP_RET;
        USILProgram program = {
            .instructions = instructions, .instruction_count = 3, .temp_count = 2};
        HLSLEmitterContext ctx;
        CHECK(analyze(&ctx, &program));
        CHECK(get_component_provenance(&ctx, 2, 1, 0)->root_instruction == (modifier == 0 ? 0 : 1));
        dispose(&ctx);
    }
    return true;
}

static bool check_merge_and_undefined_lanes(void) {
    USILInstruction instructions[5] = {0};
    instructions[0].opcode = USIL_OP_IF;
    instructions[0].operand_count = 1;
    instructions[0].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[2].opcode = USIL_OP_ENDIF;
    instructions[3] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[4].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions, .instruction_count = 5, .temp_count = 1};
    HLSLEmitterContext ctx;
    CHECK(analyze(&ctx, &program));
    CHECK(hlsl_operand_definition(&ctx, 3, 1, 0) == HLSL_DEFINITION_AMBIGUOUS);
    CHECK(hlsl_operand_definition(&ctx, 3, 1, 1) == HLSL_DEFINITION_UNKNOWN);
    HLSLBlockPhis *phis = &ctx.ssa.block_phis[ctx.cfg.instruction_block[3]];
    CHECK(phis->phi_count == 1);
    CHECK(ctx.cfg.blocks[ctx.cfg.instruction_block[3]].predecessor_count == 2);
    int undefined = 0, defined = 0;
    for (int incoming = 0; incoming < 2; ++incoming) {
        int value = phis->phis[0].incoming_vars[incoming];
        if (value < 0)
            ++undefined;
        else if (ctx.ssa.ssa_var_defs[value] == 1)
            ++defined;
    }
    CHECK(undefined == 1 && defined == 1);
    CHECK(hlsl_definition_use_count(&ctx, 1, 0) == 1);
    dispose(&ctx);
    return true;
}

static bool check_loop_phi(void) {
    USILInstruction instructions[7] = {0};
    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[1].opcode = USIL_OP_LOOP;
    instructions[2].opcode = USIL_OP_ADD;
    instructions[2].operand_count = 3;
    instructions[2].operands[0] = reg(OPERAND_TYPE_TEMP, 0, 0x10);
    instructions[2].operands[1] = reg(OPERAND_TYPE_TEMP, 0, 0);
    instructions[2].operands[2] = reg(OPERAND_TYPE_INPUT, 0, 0);
    instructions[3].opcode = USIL_OP_BREAKC;
    instructions[3].operand_count = 1;
    instructions[3].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    instructions[4].opcode = USIL_OP_ENDLOOP;
    instructions[5] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[6].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions, .instruction_count = 7, .temp_count = 1};
    HLSLEmitterContext ctx;
    CHECK(analyze(&ctx, &program));
    CHECK(hlsl_operand_definition(&ctx, 2, 1, 0) == HLSL_DEFINITION_AMBIGUOUS);
    CHECK(hlsl_definition_use_count(&ctx, 0, 0) == 1);
    CHECK(hlsl_definition_use_count(&ctx, 2, 0) >= 1);
    dispose(&ctx);

    instructions[2].operands[1].swizzle[0] = 4;
    CHECK(!analyze(&ctx, &program));
    dispose(&ctx);
    instructions[2].operands[1].swizzle[0] = 0;
    instructions[2].operands[1].register_index = 1;
    CHECK(!analyze(&ctx, &program));
    dispose(&ctx);
    return true;
}

static bool has_flow_edge(const HLSLEmitterContext *ctx, int from, int to) {
    const HLSLBasicBlock *block = &ctx->cfg.blocks[ctx->cfg.instruction_block[from]];
    for (int i = 0; i < block->successor_count; ++i)
        if (block->successors[i] == ctx->cfg.instruction_block[to])
            return true;
    return false;
}

static bool check_structured_flow_edges(void) {
    /* A DXBC loop has no implicit exit at ENDLOOP. Only the conditional
     * break reaches the output, so its preceding definition dominates it. */
    USILInstruction instructions[8] = {0};
    instructions[0].opcode = USIL_OP_LOOP;
    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[2].opcode = USIL_OP_BREAKC;
    instructions[2].operand_count = 1;
    instructions[2].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    instructions[3] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, 1, 0));
    instructions[4].opcode = USIL_OP_ENDLOOP;
    instructions[5] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[6].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions, .instruction_count = 7, .temp_count = 1};
    HLSLEmitterContext ctx;
    CHECK(analyze(&ctx, &program));
    CHECK(!has_flow_edge(&ctx, 4, 5));
    CHECK(has_flow_edge(&ctx, 4, 1));
    CHECK(has_flow_edge(&ctx, 2, 5));
    CHECK(hlsl_operand_definition(&ctx, 5, 1, 0) == 1);
    dispose(&ctx);

    /* An empty unconditional loop is a self-edge, with an unreachable tail. */
    memset(instructions, 0, sizeof(instructions));
    instructions[0].opcode = USIL_OP_LOOP;
    instructions[1].opcode = USIL_OP_ENDLOOP;
    instructions[2].opcode = USIL_OP_RET;
    program.instruction_count = 3;
    CHECK(analyze(&ctx, &program));
    CHECK(has_flow_edge(&ctx, 1, 1));
    CHECK(!has_flow_edge(&ctx, 1, 2));
    CHECK(ctx.cfg.idom[ctx.cfg.instruction_block[2]] == -1);
    dispose(&ctx);
    return true;
}

static bool check_switch_flow_and_scope(void) {
    USILInstruction instructions[24] = {0};
    instructions[0].opcode = USIL_OP_SWITCH;
    instructions[0].operand_count = 1;
    instructions[0].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    for (int arm = 0; arm < 5; ++arm) {
        const int start = 1 + arm * 3;
        instructions[start].opcode = arm < 4 ? USIL_OP_CASE : USIL_OP_DEFAULT;
        if (arm < 4) {
            instructions[start].operand_count = 1;
            instructions[start].operands[0].type = OPERAND_TYPE_IMMEDIATE32;
            instructions[start].operands[0].imm_value_count = 1;
            instructions[start].operands[0].imm_values[0] = (uint32_t)arm;
        }
        instructions[start + 1] =
            move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, arm, 0));
        instructions[start + 2].opcode = USIL_OP_BREAK;
    }
    instructions[16].opcode = USIL_OP_ENDSWITCH;
    instructions[17] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[18].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions, .instruction_count = 19, .temp_count = 1};
    HLSLEmitterContext ctx;
    CHECK(analyze(&ctx, &program));
    CHECK(ctx.cfg.blocks[0].successor_count == 5);
    CHECK(!has_flow_edge(&ctx, 0, 17));
    CHECK(hlsl_operand_definition(&ctx, 17, 1, 0) == HLSL_DEFINITION_AMBIGUOUS);
    for (int arm = 0; arm < 5; ++arm) {
        CHECK(has_flow_edge(&ctx, 0, 1 + arm * 3));
        CHECK(has_flow_edge(&ctx, 3 + arm * 3, 17));
        CHECK(hlsl_definition_use_count(&ctx, 2 + arm * 3, 0) == 1);
    }
    dispose(&ctx);

    /* Without default, an unmatched selector bypasses every assignment. */
    instructions[13].opcode = USIL_OP_CASE;
    instructions[13].operand_count = 1;
    instructions[13].operands[0] = instructions[1].operands[0];
    instructions[13].operands[0].imm_values[0] = 4;
    CHECK(analyze(&ctx, &program));
    CHECK(ctx.cfg.blocks[0].successor_count == 6);
    CHECK(has_flow_edge(&ctx, 0, 17));
    HLSLBlockPhis *phis = &ctx.ssa.block_phis[ctx.cfg.instruction_block[17]];
    CHECK(phis->phi_count == 1);
    bool entry_is_undefined = false;
    for (int edge = 0; edge < ctx.cfg.blocks[ctx.cfg.instruction_block[17]].predecessor_count;
         ++edge)
        if (phis->phis[0].incoming_blocks[edge] == 0)
            entry_is_undefined = phis->phis[0].incoming_vars[edge] == -1;
    CHECK(entry_is_undefined);
    dispose(&ctx);

    /* BREAK leaves the inner switch; CONTINUE still targets the outer loop. */
    memset(instructions, 0, sizeof(instructions));
    const USILOpcode opcodes[] = {USIL_OP_LOOP,  USIL_OP_SWITCH,    USIL_OP_DEFAULT,
                                  USIL_OP_BREAK, USIL_OP_ENDSWITCH, USIL_OP_CONTINUEC,
                                  USIL_OP_BREAK, USIL_OP_ENDLOOP,   USIL_OP_RET};
    for (size_t i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); ++i)
        instructions[i].opcode = opcodes[i];
    for (int i = 1; i <= 5; i += 4) {
        instructions[i].operand_count = 1;
        instructions[i].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    }
    program.instruction_count = 9;
    CHECK(analyze(&ctx, &program));
    CHECK(has_flow_edge(&ctx, 3, 5) && !has_flow_edge(&ctx, 3, 8));
    CHECK(has_flow_edge(&ctx, 5, 7) && has_flow_edge(&ctx, 5, 6));
    CHECK(has_flow_edge(&ctx, 6, 8));
    CHECK(has_flow_edge(&ctx, 7, 1) && !has_flow_edge(&ctx, 7, 8));
    dispose(&ctx);

    instructions[3].opcode = USIL_OP_BREAKC;
    instructions[3].operand_count = 1;
    instructions[3].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    CHECK(analyze(&ctx, &program));
    CHECK(has_flow_edge(&ctx, 3, 4) && has_flow_edge(&ctx, 3, 5));
    CHECK(!has_flow_edge(&ctx, 3, 8));
    dispose(&ctx);

    instructions[3].opcode = USIL_OP_CONTINUE;
    instructions[3].operand_count = 0;
    CHECK(analyze(&ctx, &program));
    CHECK(has_flow_edge(&ctx, 3, 7) && !has_flow_edge(&ctx, 3, 5));
    dispose(&ctx);
    return true;
}

static bool check_flow_structure_validation(void) {
    USILInstruction instructions[132] = {0};
    USILProgram program = {.instructions = instructions, .instruction_count = 8};
    HLSLEmitterContext ctx = {.program = &program};
    const USILOpcode nested[] = {USIL_OP_IF,    USIL_OP_ELSE,  USIL_OP_IF,  USIL_OP_ELSE,
                                 USIL_OP_ENDIF, USIL_OP_ENDIF, USIL_OP_NOP, USIL_OP_RET};
    for (size_t i = 0; i < sizeof(nested) / sizeof(nested[0]); ++i)
        instructions[i].opcode = nested[i];
    CHECK(build_control_flow_graph(&ctx) && analyze_block_nesting(&ctx));
    CHECK(ctx.cfg.nesting[ctx.cfg.instruction_block[2]].parent_block ==
          ctx.cfg.instruction_block[1]);
    CHECK(ctx.cfg.nesting[ctx.cfg.instruction_block[6]].parent_block == -1);
    free_control_flow_graph(&ctx);

    const USILOpcode malformed[][4] = {
        {USIL_OP_BREAK, USIL_OP_NOP, USIL_OP_NOP, USIL_OP_RET},
        {USIL_OP_CONTINUE, USIL_OP_NOP, USIL_OP_NOP, USIL_OP_RET},
        {USIL_OP_IF, USIL_OP_ELSE, USIL_OP_ELSE, USIL_OP_ENDIF},
        {USIL_OP_IF, USIL_OP_LOOP, USIL_OP_ENDIF, USIL_OP_ENDLOOP},
        {USIL_OP_IF, USIL_OP_NOP, USIL_OP_NOP, USIL_OP_RET},
        {USIL_OP_ELSE, USIL_OP_NOP, USIL_OP_NOP, USIL_OP_RET},
        {USIL_OP_CASE, USIL_OP_NOP, USIL_OP_NOP, USIL_OP_RET},
        {USIL_OP_SWITCH, USIL_OP_DEFAULT, USIL_OP_DEFAULT, USIL_OP_ENDSWITCH},
        {USIL_OP_SWITCH, USIL_OP_CONTINUE, USIL_OP_NOP, USIL_OP_ENDSWITCH},
        {USIL_OP_ENDIF, USIL_OP_NOP, USIL_OP_NOP, USIL_OP_RET}};
    program.instruction_count = 4;
    for (size_t case_index = 0; case_index < sizeof(malformed) / sizeof(malformed[0]);
         ++case_index) {
        for (int i = 0; i < 4; ++i)
            instructions[i].opcode = malformed[case_index][i];
        CHECK(!build_control_flow_graph(&ctx));
        CHECK(!ctx.cfg.blocks && !ctx.cfg.successor_storage && !ctx.cfg.nesting);
    }
    for (int depth = 64; depth <= 65; ++depth) {
        for (int i = 0; i < depth; ++i)
            instructions[i].opcode = USIL_OP_IF;
        for (int i = depth; i < depth * 2; ++i)
            instructions[i].opcode = USIL_OP_ENDIF;
        instructions[depth * 2].opcode = USIL_OP_RET;
        program.instruction_count = depth * 2 + 1;
        CHECK(build_control_flow_graph(&ctx) == (depth == 64));
        free_control_flow_graph(&ctx);
    }
    return true;
}

static bool check_control_region_proofs(void) {
    USILInstruction instructions[12] = {0};
    const USILOpcode branch[] = {USIL_OP_IF, USIL_OP_NOP, USIL_OP_ELSE, USIL_OP_NOP,
                                USIL_OP_ENDIF, USIL_OP_NOP, USIL_OP_RET};
    for (size_t i = 0; i < sizeof(branch) / sizeof(branch[0]); ++i)
        instructions[i].opcode = branch[i];
    USILProgram program = {.instructions = instructions, .instruction_count = 7};
    HLSLEmitterContext ctx = {.program = &program};
    HLSLIfRegion region;
    CHECK(build_control_flow_graph(&ctx) && compute_dominance(&ctx.cfg));
    CHECK(ctx.cfg.instruction_count == 7);
    CHECK(ctx.cfg.instruction_flow[0].end == 4);
    CHECK(ctx.cfg.instruction_flow[0].alternate == 2);
    CHECK(ctx.cfg.instruction_flow[1].parent == 0);
    CHECK(ctx.cfg.instruction_flow[3].parent == 2);
    CHECK(ctx.cfg.instruction_flow[4].jump_scope == 0);
    CHECK(ctx.cfg.instruction_flow[5].parent == -1);
    CHECK(hlsl_cfg_if_region(&ctx, 0, &region));
    CHECK(region.header_block == ctx.cfg.instruction_block[0]);
    CHECK(region.true_block == ctx.cfg.instruction_block[1]);
    CHECK(region.false_block == ctx.cfg.instruction_block[3]);
    CHECK(region.join_block == ctx.cfg.instruction_block[5]);
    CHECK(region.else_instruction == 2 && region.end_instruction == 4);
    CHECK(hlsl_cfg_must_reach(&ctx.cfg, region.header_block, region.join_block));
    CHECK(hlsl_cfg_must_reach(&ctx.cfg, region.true_block, region.join_block));
    CHECK(!hlsl_cfg_dominates(&ctx.cfg, region.true_block, region.join_block));
    CHECK(!hlsl_cfg_must_reach(&ctx.cfg, region.header_block, region.true_block));
    CHECK(!hlsl_cfg_if_region(&ctx, 1, &region));
    CHECK(!hlsl_cfg_if_region(&ctx, -1, &region));
    CHECK(!hlsl_cfg_if_region(&ctx, 0, NULL));
    CHECK(!hlsl_cfg_must_reach(&ctx.cfg, -1, region.join_block));
    CHECK(!hlsl_cfg_dominates(&ctx.cfg, 0, ctx.cfg.block_count));
    free_control_flow_graph(&ctx);
    CHECK(!ctx.cfg.instruction_flow && !ctx.cfg.instruction_count);

    /* Missing ELSE has the header itself as one of the join's predecessors. */
    instructions[2].opcode = USIL_OP_NOP;
    CHECK(build_control_flow_graph(&ctx) && compute_dominance(&ctx.cfg));
    CHECK(hlsl_cfg_if_region(&ctx, 0, &region));
    CHECK(region.else_instruction == -1 && region.false_block == region.join_block);
    free_control_flow_graph(&ctx);
    instructions[2].opcode = USIL_OP_ELSE;

    /* Both conditional termination and unconditional return defeat a merge
     * proof. DISCARD still has a continuing edge for the reaching-value SSA. */
    for (int conditional = 0; conditional < 2; ++conditional) {
        instructions[1].opcode = conditional ? USIL_OP_DISCARD : USIL_OP_RET;
        CHECK(build_control_flow_graph(&ctx) && compute_dominance(&ctx.cfg));
        const HLSLBasicBlock *exit = &ctx.cfg.blocks[ctx.cfg.instruction_block[1]];
        CHECK(exit->may_exit && exit->successor_count == conditional);
        HLSLIfRegion unchanged = region;
        CHECK(!hlsl_cfg_if_region(&ctx, 0, &region));
        CHECK(memcmp(&unchanged, &region, sizeof(region)) == 0);
        CHECK(!hlsl_cfg_must_reach(&ctx.cfg, ctx.cfg.instruction_block[0],
                                 ctx.cfg.instruction_block[5]));
        free_control_flow_graph(&ctx);
    }

    /* An exit-reachable loop may run forever. Post-dominance of only finite
     * paths would incorrectly authorize lifting the outer IF to its join. */
    const USILOpcode looping[] = {USIL_OP_IF, USIL_OP_LOOP, USIL_OP_BREAKC,
                                 USIL_OP_ENDLOOP, USIL_OP_ELSE, USIL_OP_NOP,
                                 USIL_OP_ENDIF, USIL_OP_RET};
    for (size_t i = 0; i < sizeof(looping) / sizeof(looping[0]); ++i)
        instructions[i].opcode = looping[i];
    program.instruction_count = 8;
    CHECK(build_control_flow_graph(&ctx) && compute_dominance(&ctx.cfg));
    CHECK(!hlsl_cfg_if_region(&ctx, 0, &region));
    CHECK(hlsl_cfg_dominates(&ctx.cfg, 0, ctx.cfg.instruction_block[7]));
    CHECK(!hlsl_cfg_must_reach(&ctx.cfg, 0, ctx.cfg.instruction_block[7]));
    CHECK(hlsl_cfg_must_reach(&ctx.cfg, ctx.cfg.instruction_block[1],
                             ctx.cfg.instruction_block[2]));
    free_control_flow_graph(&ctx);

    instructions[2].opcode = USIL_OP_NOP; /* No loop exit: join still reachable via ELSE. */
    CHECK(build_control_flow_graph(&ctx) && compute_dominance(&ctx.cfg));
    CHECK(!hlsl_cfg_if_region(&ctx, 0, &region));
    CHECK(!hlsl_cfg_must_reach(&ctx.cfg, 0, ctx.cfg.instruction_block[7]));
    free_control_flow_graph(&ctx);

    /* A conditional whose lexical join is outside the instruction stream has
     * no material merge value to own. The CFG records program fallthrough. */
    instructions[0].opcode = USIL_OP_IF;
    instructions[1].opcode = USIL_OP_ENDIF;
    program.instruction_count = 2;
    CHECK(build_control_flow_graph(&ctx) && compute_dominance(&ctx.cfg));
    CHECK(ctx.cfg.blocks[0].may_exit && ctx.cfg.blocks[1].may_exit);
    CHECK(!hlsl_cfg_if_region(&ctx, 0, &region));
    free_control_flow_graph(&ctx);

    instructions[0].opcode = USIL_OP_RET;
    instructions[1].opcode = USIL_OP_IF;
    instructions[2].opcode = USIL_OP_ENDIF;
    instructions[3].opcode = USIL_OP_RET;
    program.instruction_count = 4;
    CHECK(build_control_flow_graph(&ctx) && compute_dominance(&ctx.cfg));
    CHECK(!hlsl_cfg_if_region(&ctx, 1, &region));
    CHECK(!hlsl_cfg_must_reach(&ctx.cfg, 1, 1));
    CHECK(!hlsl_cfg_dominates(&ctx.cfg, 1, 1));
    free_control_flow_graph(&ctx);
    return true;
}

/* Independent bounded-path oracle: a walk of N edges avoiding target in an
 * N-node graph contains a cycle. Enumerating paths rather than reverse fixed
 * points checks termination, self-edges, fan-out, and reconvergence together. */
static bool all_short_paths_reach(const HLSLControlFlowGraph *cfg, int block, int target,
                                  int length) {
    if (block == target)
        return true;
    const HLSLBasicBlock *value = &cfg->blocks[block];
    if (length == cfg->block_count || value->may_exit || !value->successor_count)
        return false;
    for (int edge = 0; edge < value->successor_count; ++edge)
        if (!all_short_paths_reach(cfg, value->successors[edge], target, length + 1))
            return false;
    return true;
}

static bool check_exhaustive_postdominance(void) {
    enum { N = 3, CHOICES = 1 << (N + 1), GRAPHS = CHOICES * CHOICES * CHOICES };
    for (int graph = 0; graph < GRAPHS; ++graph) {
        HLSLEmitterContext ctx = {0};
        HLSLControlFlowGraph *cfg = &ctx.cfg;
        cfg->block_count = N;
        cfg->blocks = calloc(N, sizeof(*cfg->blocks));
        cfg->successor_storage = calloc(N * N, sizeof(int));
        cfg->predecessor_storage = calloc(N * N, sizeof(int));
        CHECK(cfg->blocks && cfg->successor_storage && cfg->predecessor_storage);
        for (int block = 0; block < N; ++block) {
            cfg->blocks[block].successors = cfg->successor_storage + block * N;
            cfg->blocks[block].predecessors = cfg->predecessor_storage + block * N;
        }
        for (int block = 0, choices = graph; block < N; ++block, choices /= CHOICES) {
            const int mask = choices % CHOICES;
            cfg->blocks[block].may_exit = (mask & (1 << N)) != 0;
            for (int to = 0; to < N; ++to)
                if (mask & (1 << to)) {
                    cfg->blocks[block].successors[cfg->blocks[block].successor_count++] = to;
                    cfg->blocks[to].predecessors[cfg->blocks[to].predecessor_count++] = block;
                }
        }
        CHECK(compute_dominance(cfg));
        for (int start = 0; start < N; ++start)
            for (int target = 0; target < N; ++target) {
                const bool expected = cfg->idom[start] >= 0 && cfg->idom[target] >= 0 &&
                                      all_short_paths_reach(cfg, start, target, 0);
                CHECK(hlsl_cfg_must_reach(cfg, start, target) == expected);
            }
        free_control_flow_graph(&ctx);
    }
    return true;
}

static bool check_long_dominator_chain(void) {
    /* Sequential diamonds keep source nesting shallow while producing a deep
     * dominator tree. Traversal must not depend on the C call-stack limit or
     * allocate a dense block-by-block frontier matrix. */
    enum { BRANCHES = 2048, COUNT = BRANCHES * 3 + 3 };
    USILInstruction *instructions = calloc(COUNT, sizeof(*instructions));
    CHECK(instructions);
    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, 0, 0));
    for (int branch = 0; branch < BRANCHES; ++branch) {
        const int start = 1 + branch * 3;
        instructions[start].opcode = USIL_OP_IF;
        instructions[start].operand_count = 1;
        instructions[start].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
        instructions[start + 1] =
            move(reg(OPERAND_TYPE_TEMP, 0, 0x10), reg(OPERAND_TYPE_INPUT, 1, 0));
        instructions[start + 2].opcode = USIL_OP_ENDIF;
    }
    instructions[COUNT - 2] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[COUNT - 1].opcode = USIL_OP_RET;
    USILProgram program = {
        .instructions = instructions, .instruction_count = COUNT, .temp_count = 1};
    HLSLEmitterContext ctx;
    CHECK(analyze(&ctx, &program));
    CHECK(hlsl_operand_definition(&ctx, COUNT - 2, 1, 0) == HLSL_DEFINITION_AMBIGUOUS);
    size_t phis = 0;
    for (int block = 0; block < ctx.cfg.block_count; ++block) {
        phis += (size_t)ctx.ssa.block_phis[block].phi_count;
        CHECK(ctx.cfg.idom[block] >= 0);
    }
    CHECK(phis == BRANCHES);
    CHECK(hlsl_cfg_must_reach(&ctx.cfg, 0, ctx.cfg.instruction_block[COUNT - 1]));
    CHECK(!hlsl_cfg_must_reach(&ctx.cfg, ctx.cfg.block_count - 1, 0));
    dispose(&ctx);
    free(instructions);
    return true;
}

static bool check_copy_candidates(void) {
    USILInstruction instructions[6] = {0};
    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0xf0), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 1, 0x50), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[1].operands[1].swizzle[0] = 3;
    instructions[1].operands[1].swizzle[2] = 1;
    instructions[2].opcode = USIL_OP_NOP;
    instructions[3].opcode = USIL_OP_ADD;
    instructions[3].operand_count = 3;
    instructions[3].operands[0] = reg(OPERAND_TYPE_TEMP, 2, 0x10);
    instructions[3].operands[1] = reg(OPERAND_TYPE_TEMP, 1, 0);
    instructions[3].operands[1].swizzle[0] = 2;
    instructions[3].operands[2] = reg(OPERAND_TYPE_TEMP, 1, 0);
    instructions[4] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10), reg(OPERAND_TYPE_TEMP, 2, 0));
    instructions[5].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions,
                           .instruction_count = 6,
                           .temp_count = 3,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_VERTEX,
                           .shader_model_major = 5};
    USILInstruction before[6];
    memcpy(before, instructions, sizeof(before));
    HLSLCopyLift *candidate = NULL;
    CHECK(hlsl_copy_lift_create(&program, 1, &candidate) == HLSL_COPY_LIFT_OK);
    CHECK(candidate && hlsl_copy_lift_instruction(candidate) == 1);
    const USILProgram *view = hlsl_copy_lift_program(candidate);
    CHECK(view->instructions[1].opcode == USIL_OP_NOP);
    CHECK(view->instructions[3].operands[1].register_index == 0);
    CHECK(view->instructions[3].operands[1].swizzle[0] == 1);
    CHECK(view->instructions[3].operands[2].swizzle[0] == 3);
    size_t edit_count = 0;
    const HLSLCopyLiftEdit *edits = hlsl_copy_lift_edits(candidate, &edit_count);
    CHECK(edits && edit_count == 2 && edits[0].logical_lane_mask == 1);
    CHECK(memcmp(before, instructions, sizeof(before)) == 0);
    hlsl_copy_lift_destroy(candidate);
    CHECK(memcmp(before, instructions, sizeof(before)) == 0);

    instructions[2] = move(reg(OPERAND_TYPE_TEMP, 0, 0x20), reg(OPERAND_TYPE_INPUT, 0, 0));
    CHECK(hlsl_copy_lift_create(&program, 1, &candidate) == HLSL_COPY_LIFT_SOURCE_CHANGED);
    CHECK(!candidate);
    instructions[2] = before[2];
    instructions[1].precise_mask = 1;
    CHECK(hlsl_copy_lift_create(&program, 1, &candidate) == HLSL_COPY_LIFT_PRECISION_CONTROL);
    instructions[1].precise_mask = 0;
    instructions[1].operands[1].has_neg = true;
    CHECK(hlsl_copy_lift_create(&program, 1, &candidate) == HLSL_COPY_LIFT_NOT_PLAIN_COPY);
    instructions[1].operands[1].has_neg = false;
    instructions[1].operands[1].rel_op0 = &instructions[0].operands[1];
    CHECK(hlsl_copy_lift_create(&program, 1, &candidate) == HLSL_COPY_LIFT_RELATIVE_ADDRESSING);
    instructions[1].operands[1].rel_op0 = NULL;
    instructions[0].opcode = USIL_OP_NOP;
    instructions[0].operand_count = 0;
    CHECK(hlsl_copy_lift_create(&program, 1, &candidate) == HLSL_COPY_LIFT_UNDEFINED_SOURCE);
    instructions[0] = before[0];
    instructions[2] = move(reg(OPERAND_TYPE_TEMP, 2, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[2].opcode = USIL_OP_DERIV_RTX;
    CHECK(hlsl_copy_lift_create(&program, 1, &candidate) == HLSL_COPY_LIFT_EFFECTFUL_REGION);
    instructions[2] = before[2];

    /* A vector use spanning the old and new definition cannot be rewritten
     * as one operand. Preserve the full baseline instead of guessing a pack. */
    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 1, 0xf0), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[2] = before[1];
    instructions[2].operands[0].destination_mask = 0x10;
    instructions[3].operands[0].destination_mask = 0x30;
    instructions[3].operands[1] = reg(OPERAND_TYPE_TEMP, 1, 0);
    instructions[3].operands[2] = reg(OPERAND_TYPE_INPUT, 0, 0);
    CHECK(hlsl_copy_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_PARTIAL_USE);
    CHECK(!candidate);
    return true;
}

static bool check_result_candidates(void) {
    USILInstruction instructions[5] = {0};
    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0xf0), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[0].opcode = USIL_OP_ADD;
    instructions[0].operand_count = 3;
    instructions[0].operands[2] = reg(OPERAND_TYPE_INPUT, 1, 0);
    instructions[0].source_instruction_index = 23;
    instructions[1].opcode = USIL_OP_NOP;
    instructions[2] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0xf0), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[3].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions,
                           .instruction_count = 4,
                           .temp_count = 1,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                           .shader_model_major = 5};
    HLSLCopyLift *candidate = NULL;
    const uint32_t a[4] = {0x80000000u, 0x7fc01234u, 0xdeadbeefu, 0x12345678u};
    const uint32_t b[4] = {0xffff0000u, 0x00ffff00u, 0x0000ffffu, 0xf0f0f0f0u};
    const USILOpcode opcodes[] = {USIL_OP_ADD, USIL_OP_MUL, USIL_OP_AND, USIL_OP_OR, USIL_OP_XOR};
    /* All 24 permutations, all five admitted operations; independent bitwise
     * evaluation checks composition rather than duplicating the planner. */
    for (size_t op = 0; op < sizeof(opcodes) / sizeof(opcodes[0]); ++op) {
        instructions[0].opcode = opcodes[op];
        for (int x = 0; x < 4; ++x)
            for (int y = 0; y < 4; ++y)
                for (int z = 0; z < 4; ++z)
                    for (int w = 0; w < 4; ++w) {
                        if (x == y || x == z || x == w || y == z || y == w || z == w)
                            continue;
                        uint8_t permutation[4] = {(uint8_t)x, (uint8_t)y, (uint8_t)z, (uint8_t)w};
                        memcpy(instructions[2].operands[1].swizzle, permutation,
                               sizeof(permutation));
                        instructions[0].operands[1].swizzle[0] = 3;
                        instructions[0].operands[1].swizzle[3] = 0;
                        CHECK(hlsl_result_lift_create(&program, 2, &candidate) ==
                              HLSL_COPY_LIFT_OK);
                        const USILProgram *view = hlsl_copy_lift_program(candidate);
                        CHECK(view->instructions[0].opcode == USIL_OP_NOP);
                        CHECK(view->instructions[2].opcode == opcodes[op]);
                        CHECK(view->instructions[2].source_instruction_index == 23);
                        CHECK(hlsl_copy_lift_producer_instruction(candidate) == 0);
                        CHECK(hlsl_copy_lift_instruction(candidate) == 2);
                        for (int lane = 0; lane < 4; ++lane) {
                            uint32_t left =
                                a[instructions[0].operands[1].swizzle[permutation[lane]]];
                            uint32_t right = b[permutation[lane]];
                            uint32_t new_left = a[view->instructions[2].operands[1].swizzle[lane]];
                            uint32_t new_right = b[view->instructions[2].operands[2].swizzle[lane]];
                            CHECK(left == new_left && right == new_right);
                            if (opcodes[op] == USIL_OP_AND)
                                CHECK((left & right) == (new_left & new_right));
                            if (opcodes[op] == USIL_OP_OR)
                                CHECK((left | right) == (new_left | new_right));
                            if (opcodes[op] == USIL_OP_XOR)
                                CHECK((left ^ right) == (new_left ^ new_right));
                        }
                        hlsl_copy_lift_destroy(candidate);
                    }
    }
    instructions[0].opcode = USIL_OP_ADD;
    for (int lane = 0; lane < 4; ++lane)
        instructions[2].operands[1].swizzle[lane] = (uint8_t)(3 - lane);
    instructions[0].operands[2] = (DXBCOperand){
        .type = OPERAND_TYPE_IMMEDIATE32, .imm_value_count = 4, .immediate_word_count = 4};
    memcpy(instructions[0].operands[2].imm_values, a, sizeof(a));
    memcpy(instructions[0].operands[2].immediate_words, a, sizeof(a));
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_OK);
    const DXBCOperand *literal = &hlsl_copy_lift_program(candidate)->instructions[2].operands[2];
    for (int lane = 0; lane < 4; ++lane) {
        CHECK(literal->imm_values[lane] == a[3 - lane]);
        CHECK(literal->immediate_words[lane] == a[3 - lane]);
    }
    hlsl_copy_lift_destroy(candidate);
    instructions[0].operands[2].imm_value_count = 1;
    instructions[0].operands[2].immediate_word_count = 1;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_OK);
    CHECK(hlsl_copy_lift_program(candidate)->instructions[2].operands[2].imm_values[0] == a[0]);
    hlsl_copy_lift_destroy(candidate);
    instructions[0].operands[2] = reg(OPERAND_TYPE_INPUT, 1, 0);

    instructions[3] = move(reg(OPERAND_TYPE_OUTPUT, 1, 0x10), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[4].opcode = USIL_OP_RET;
    program.instruction_count = 5;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_MULTIPLE_USES);
    instructions[3] = (USILInstruction){.opcode = USIL_OP_RET};
    program.instruction_count = 4;
    instructions[2].operands[1].swizzle[0] = 2;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_NONBIJECTIVE_LANES);
    instructions[2].operands[1].swizzle[0] = 3;
    instructions[2].operands[0].destination_mask = 0x70;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_NONBIJECTIVE_LANES);
    instructions[2].operands[0].destination_mask = 0xf0;
    instructions[0].operands[0].destination_mask = 0xa0;
    instructions[2].operands[0].destination_mask = 0x50;
    instructions[2].operands[1].swizzle[0] = 3;
    instructions[2].operands[1].swizzle[2] = 1;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_OK);
    const USILInstruction *partial = &hlsl_copy_lift_program(candidate)->instructions[2];
    CHECK(partial->operands[0].destination_mask == 0x50);
    CHECK(partial->operands[1].swizzle[0] == 0 && partial->operands[1].swizzle[2] == 1);
    hlsl_copy_lift_destroy(candidate);
    instructions[0].operands[0].destination_mask = 0xf0;
    instructions[2].operands[0].destination_mask = 0xf0;
    instructions[0].saturate = true;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_NOT_PLAIN_RESULT);
    instructions[0].saturate = false;
    instructions[0].precise_mask = 1;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_PRECISION_CONTROL);
    instructions[0].precise_mask = 0;
    instructions[0].operands[1].has_abs = true;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_NOT_PLAIN_RESULT);
    instructions[0].operands[1].has_abs = false;
    instructions[0].operands[1].type = OPERAND_TYPE_TEMP;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_UNDEFINED_SOURCE);
    instructions[0].operands[1].type = OPERAND_TYPE_INPUT;
    instructions[1] = move(reg(OPERAND_TYPE_OUTPUT, 1, 0x10), reg(OPERAND_TYPE_INPUT, 1, 0));
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_NOT_PLAIN_RESULT);
    instructions[1] = (USILInstruction){.opcode = USIL_OP_NOP};
    instructions[0].opcode = USIL_OP_DERIV_RTX;
    instructions[0].operand_count = 2;
    CHECK(hlsl_result_lift_create(&program, 2, &candidate) == HLSL_COPY_LIFT_EFFECTFUL_REGION);
    CHECK(candidate == NULL && instructions[2].opcode == USIL_OP_MOV);
    return true;
}

static bool check_effects(void) {
    USILTexture texture = {.reg_idx = 0, .dimension = "2d"};
    USILProgram program = {.textures = &texture, .texture_count = 1};
    USILInstruction instruction =
        move(reg(OPERAND_TYPE_TEMP, 0, 0xf0), reg(OPERAND_TYPE_INPUT, 0, 0));
    USILEffectFlags effects = USIL_EFFECT_UNKNOWN;
    CHECK(usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == USIL_EFFECT_NONE);
    instruction.opcode = USIL_OP_DERIV_RTX_FINE;
    CHECK(usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == USIL_EFFECT_QUAD_CONTEXT);
    instruction.opcode = USIL_OP_SAMPLE;
    instruction.operand_count = 4;
    instruction.operands[2] = reg(OPERAND_TYPE_RESOURCE, 0, 0);
    instruction.operands[3] = reg(OPERAND_TYPE_SAMPLER, 0, 0);
    CHECK(usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == (USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_QUAD_CONTEXT));
    instruction.opcode = USIL_OP_SAMPLE_L;
    instruction.operand_count = 5;
    instruction.operands[4] = reg(OPERAND_TYPE_INPUT, 1, 0);
    CHECK(usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == USIL_EFFECT_RESOURCE_READ);
    instruction.opcode = USIL_OP_IMM_ATOMIC_IADD;
    instruction.operand_count = 4;
    instruction.operands[1] = reg(OPERAND_TYPE_UAV, 0, 0);
    instruction.operands[2] = reg(OPERAND_TYPE_INPUT, 0, 0);
    instruction.operands[3] = reg(OPERAND_TYPE_INPUT, 1, 0);
    CHECK(usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == (USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_EXTERNAL_WRITE));
    instruction.opcode = USIL_OP_DISCARD;
    instruction.operand_count = 1;
    instruction.operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    CHECK(usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == (USIL_EFFECT_CONTROL | USIL_EFFECT_QUAD_CONTEXT));
    instruction.opcode = USIL_OP_GEOMETRY_APPEND;
    instruction.operand_count = 0;
    instruction.geometry_effect = USIL_GEOMETRY_EFFECT_APPEND;
    CHECK(usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == USIL_EFFECT_GEOMETRY_OUTPUT);
    instruction.opcode = (USILOpcode)999;
    CHECK(!usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == USIL_EFFECT_UNKNOWN);
    instruction.opcode = USIL_OP_ADD;
    CHECK(!usil_instruction_effects(&program, &instruction, &effects));
    CHECK(effects == USIL_EFFECT_UNKNOWN);
    return true;
}

/* Structurally valid SM5 vertex RET container. Mock compilation below tests
 * transaction ownership and rejection; live tests establish compiler behavior. */
static uint8_t transaction_target[] = {'D', 'X', 'B',  'C', 0,  0, 0, 0, 0,   0,   0,   0,   0,  0,
                                       0,   0,   0,    0,   0,  0, 1, 0, 0,   0,   56,  0,   0,  0,
                                       1,   0,   0,    0,   36, 0, 0, 0, 'S', 'H', 'D', 'R', 12, 0,
                                       0,   0,   0x50, 0,   1,  0, 3, 0, 0,   0,   62,  0,   0,  1};

typedef struct {
    uint64_t now;
    uint64_t compile_elapsed;
    size_t calls;
    bool cancelled;
    bool clock_failed;
    bool mutate;
    bool malformed;
    bool omit_source;
    bool request_identity;
    bool drift_controls;
    HLSLLiftStatus status;
} TransactionFixture;

static bool transaction_clock(void *context, uint64_t *now) {
    TransactionFixture *fixture = context;
    *now = fixture->now;
    return !fixture->clock_failed;
}

static bool transaction_cancelled(void *context) {
    return ((TransactionFixture *)context)->cancelled;
}

static HLSLLiftStatus transaction_compile(void *context, const USILProgram *program,
                                          uint64_t remaining_ms, HLSLLiftArtifact *artifact) {
    TransactionFixture *fixture = context;
    ++fixture->calls;
    fixture->now += fixture->compile_elapsed;
    if (!program || !remaining_ms)
        return HLSL_LIFT_INVALID_ARGUMENT;
    const char *text = program->instructions[0].opcode == USIL_OP_NOP ? "accepted copy candidate"
                                                                      : "baseline program";
    artifact->source = fixture->omit_source ? NULL : malloc(strlen(text) + 1u);
    artifact->dxbc = malloc(sizeof(transaction_target));
    if ((!artifact->source && !fixture->omit_source) || !artifact->dxbc)
        return HLSL_LIFT_OUT_OF_MEMORY;
    if (artifact->source)
        strcpy(artifact->source, text);
    memcpy(artifact->dxbc, transaction_target, sizeof(transaction_target));
    artifact->dxbc_size = sizeof(transaction_target);
    artifact->cache_hit = fixture->calls > 1u;
    artifact->has_request_identity = fixture->request_identity;
    memset(artifact->request_digest, (int)fixture->calls, sizeof(artifact->request_digest));
    memset(artifact->controls_digest, fixture->drift_controls ? 2 : 1,
           sizeof(artifact->controls_digest));
    /* A well-formed but wrong instruction must fail the exact gate. */
    if (fixture->mutate) {
        artifact->dxbc[52] = 58; /* NOP instead of RET. */
        uint8_t hash[16];
        if (!dxbc_compute_hash(artifact->dxbc, artifact->dxbc_size, hash))
            return HLSL_LIFT_INVALID_ARGUMENT;
        memcpy(artifact->dxbc + 4, hash, sizeof(hash));
    }
    if (fixture->malformed)
        artifact->dxbc[0] = 'X';
    return fixture->status;
}

static bool check_transactions(void) {
    uint8_t hash[16];
    CHECK(dxbc_compute_hash(transaction_target, sizeof(transaction_target), hash));
    memcpy(transaction_target + 4, hash, sizeof(hash));
    USILInstruction instructions[4] = {0};
    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0xf0), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 1, 0xf0), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[2] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0xf0), reg(OPERAND_TYPE_TEMP, 1, 0));
    instructions[3].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions,
                           .instruction_count = 4,
                           .temp_count = 2,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_VERTEX,
                           .shader_model_major = 5};
    const HLSLLiftLimits limits = {.max_candidates = 20, .max_compiles = 20, .max_elapsed_ms = 100};
    TransactionFixture fixture = {0};
    HLSLLiftServices services = {.compile = transaction_compile,
                                 .monotonic_ms = transaction_clock,
                                 .cancelled = transaction_cancelled,
                                 .context = &fixture};
    HLSLLiftTransaction *transaction = NULL;
    HLSLLiftResult result;
    CHECK(hlsl_lift_transaction_begin(&program, transaction_target, sizeof(transaction_target),
                                      &services, &limits, &transaction,
                                      &result) == HLSL_LIFT_VERIFIED);
    CHECK(result.compared && result.comparison.status == DXBC_COMPARE_EQUAL);
    CHECK(strlen(result.source_sha256) == 64 && strlen(result.output_sha256) == 64);
    CHECK(hlsl_lift_transaction_step_count(transaction) == 0 &&
          hlsl_lift_transaction_step(transaction, 0) == NULL &&
          hlsl_lift_transaction_step(NULL, 0) == NULL &&
          hlsl_lift_transaction_step_count(NULL) == 0);
    const HLSLLiftArtifact *accepted = hlsl_lift_transaction_artifact(transaction);
    const char *baseline_source = accepted->source;
    const uint8_t *baseline_bytes = accepted->dxbc;
    fixture.mutate = true;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_DXBC_MISMATCH);
    CHECK(result.compared && result.comparison.status == DXBC_COMPARE_INSTRUCTION_OPCODE);
    CHECK(hlsl_lift_transaction_program(transaction) == &program);
    CHECK(accepted->source == baseline_source && accepted->dxbc == baseline_bytes);
    CHECK(hlsl_lift_transaction_step_count(transaction) == 0);
    fixture.mutate = false;
    fixture.malformed = true;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_INVALID_DXBC);
    fixture.malformed = false;
    fixture.omit_source = true;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_EMISSION_REJECTED);
    fixture.omit_source = false;
    const HLSLLiftStatus failures[] = {HLSL_LIFT_COMPILER_REJECTED, HLSL_LIFT_COMPILER_UNAVAILABLE,
                                       HLSL_LIFT_EMISSION_REJECTED};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        fixture.status = failures[i];
        CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == failures[i]);
        CHECK(!result.compared && accepted->source == baseline_source);
    }
    fixture.status = HLSL_LIFT_VERIFIED;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_VERIFIED);
    CHECK(hlsl_lift_transaction_program(transaction)->instructions[0].opcode == USIL_OP_NOP);
    CHECK(strcmp(accepted->source, "accepted copy candidate") == 0);
    CHECK(hlsl_lift_transaction_step_count(transaction) == 1);
    const HLSLLiftStep *first_step = hlsl_lift_transaction_step(transaction, 0);
    CHECK(first_step && !strcmp(first_step->identifier, HLSL_COPY_LIFT_ID) &&
          first_step->version == HLSL_COPY_LIFT_VERSION && first_step->instruction_index == 0 &&
          first_step->before.status == HLSL_LIFT_VERIFIED &&
          first_step->after.status == HLSL_LIFT_VERIFIED && first_step->edit_count == 1 &&
          first_step->edits[0].instruction_index == 1 && first_step->edits[0].logical_lane_mask == 15 &&
          !strcmp(first_step->after.source_sha256, result.source_sha256));
    size_t calls = fixture.calls;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) ==
          HLSL_LIFT_PRECONDITION_REJECTED);
    CHECK(result.precondition == HLSL_COPY_LIFT_NOT_PLAIN_COPY && calls == fixture.calls);
    /* The second copy is re-proven on the accepted first copy, not on a stale
     * baseline. Its borrowed metadata remains alive until session destruction. */
    CHECK(hlsl_lift_transaction_try_copy(transaction, 1, &result) == HLSL_LIFT_VERIFIED);
    CHECK(hlsl_lift_transaction_program(transaction)->instructions[2].operands[1].type ==
          OPERAND_TYPE_INPUT);
    CHECK(instructions[0].opcode == USIL_OP_MOV && instructions[1].opcode == USIL_OP_MOV);
    const HLSLLiftStep *second_step = hlsl_lift_transaction_step(transaction, 1);
    CHECK(hlsl_lift_transaction_step_count(transaction) == 2 && second_step &&
          hlsl_lift_transaction_step(transaction, 0) == first_step &&
          second_step->instruction_index == 1 &&
          !strcmp(first_step->after.source_sha256, second_step->before.source_sha256) &&
          !strcmp(first_step->after.output_sha256, second_step->before.output_sha256) &&
          !strcmp(second_step->after.source_sha256, result.source_sha256));
    CHECK(!hlsl_lift_transaction_step(transaction, 2) &&
          !hlsl_lift_transaction_step(transaction, SIZE_MAX));
    HLSLLiftStats stats;
    hlsl_lift_transaction_stats(transaction, &stats);
    CHECK(stats.accepted == 2 && stats.compiles == fixture.calls);
    CHECK(stats.cache_hits + 1u == stats.compiles);
    fixture.cancelled = true;
    calls = fixture.calls;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_CANCELLED);
    CHECK(fixture.calls == calls && hlsl_lift_transaction_artifact(transaction)->source);
    fixture.cancelled = false;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_CANCELLED);
    hlsl_lift_transaction_destroy(transaction);

    /* Baseline failure never produces an accepted transaction. */
    fixture = (TransactionFixture){.mutate = true};
    CHECK(hlsl_lift_transaction_begin(&program, transaction_target, sizeof(transaction_target),
                                      &services, &limits, &transaction,
                                      &result) == HLSL_LIFT_DXBC_MISMATCH &&
          !transaction);
    for (int budget = 0; budget < 5; ++budget) {
        fixture = (TransactionFixture){0};
        HLSLLiftLimits bounded = limits;
        if (budget == 0)
            bounded.max_candidates = 0;
        if (budget == 1)
            bounded.max_compiles = 1;
        CHECK(hlsl_lift_transaction_begin(&program, transaction_target, sizeof(transaction_target),
                                          &services, &bounded, &transaction,
                                          &result) == HLSL_LIFT_VERIFIED);
        if (budget == 2)
            fixture.compile_elapsed = 100;
        if (budget == 3)
            fixture.now = 100;
        if (budget == 4)
            fixture.clock_failed = true;
        HLSLLiftStatus expected =
            budget == 4 ? HLSL_LIFT_CLOCK_UNAVAILABLE : HLSL_LIFT_BUDGET_EXHAUSTED;
        CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == expected);
        CHECK(hlsl_lift_transaction_program(transaction) == &program);
        CHECK(fixture.calls == (budget == 2 ? 2u : 1u));
        CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == expected);
        hlsl_lift_transaction_destroy(transaction);
    }
    /* A byte-identical result cannot authorize changed compiler controls. */
    services.require_request_identity = true;
    services.compile_high_level = transaction_compile;
    fixture = (TransactionFixture){0};
    CHECK(hlsl_lift_transaction_begin(&program, transaction_target, sizeof(transaction_target),
                                      &services, &limits, &transaction,
                                      &result) == HLSL_LIFT_AUTHORITY_MISMATCH &&
          !transaction);
    fixture.request_identity = true;
    CHECK(hlsl_lift_transaction_begin(&program, transaction_target, sizeof(transaction_target),
                                      &services, &limits, &transaction,
                                      &result) == HLSL_LIFT_VERIFIED);
    CHECK(strlen(result.request_sha256) == 64 && strlen(result.controls_sha256) == 64);
    baseline_source = hlsl_lift_transaction_artifact(transaction)->source;
    fixture.drift_controls = true;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_AUTHORITY_MISMATCH);
    CHECK(!result.compared &&
          hlsl_lift_transaction_artifact(transaction)->source == baseline_source);
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) ==
          HLSL_LIFT_AUTHORITY_MISMATCH);
    fixture.drift_controls = false;
    fixture.request_identity = false;
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) ==
          HLSL_LIFT_AUTHORITY_MISMATCH);
    CHECK(!result.compared && result.request_sha256[0] == '\0');
    fixture.request_identity = true;
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) == HLSL_LIFT_VERIFIED);
    CHECK(hlsl_lift_transaction_is_high_level(transaction));
    hlsl_lift_transaction_destroy(transaction);
    services.require_expression_source_map = true;
    CHECK(hlsl_lift_transaction_begin(&program, transaction_target, sizeof(transaction_target),
                                      &services, &limits, &transaction,
                                      &result) == HLSL_LIFT_VERIFIED);
    baseline_source = hlsl_lift_transaction_artifact(transaction)->source;
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) ==
          HLSL_LIFT_PROVENANCE_MISMATCH);
    CHECK(!result.compared &&
          hlsl_lift_transaction_artifact(transaction)->source == baseline_source);
    hlsl_lift_transaction_destroy(transaction);
    return true;
}

static bool check_result_transactions(void) {
    USILInstruction instructions[4] = {0};
    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0xf0), reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[0].opcode = USIL_OP_MUL;
    instructions[0].operand_count = 3;
    instructions[0].operands[2] = reg(OPERAND_TYPE_INPUT, 1, 0);
    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 1, 0xf0), reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[2] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0xf0), reg(OPERAND_TYPE_TEMP, 1, 0));
    instructions[3].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions,
                           .instruction_count = 4,
                           .temp_count = 2,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_VERTEX,
                           .shader_model_major = 5};
    instructions[0].source_instruction_index = 20;
    instructions[1].source_instruction_index = 30;
    instructions[2].source_instruction_index = 40;
    instructions[3].source_instruction_index = 50;
    TransactionFixture fixture = {0};
    HLSLLiftServices services = {.compile = transaction_compile,
                                 .monotonic_ms = transaction_clock,
                                 .context = &fixture,
                                 .compile_high_level = transaction_compile};
    HLSLLiftLimits limits = {.max_candidates = 8, .max_compiles = 8, .max_elapsed_ms = 1000};
    HLSLLiftTransaction *transaction = NULL;
    HLSLLiftResult result;
    CHECK(hlsl_lift_transaction_begin(&program, transaction_target, sizeof(transaction_target),
                                      &services, &limits, &transaction,
                                      &result) == HLSL_LIFT_VERIFIED);
    fixture.mutate = true;
    CHECK(hlsl_lift_transaction_try_result(transaction, 1, &result) == HLSL_LIFT_DXBC_MISMATCH);
    CHECK(hlsl_lift_transaction_program(transaction) == &program);
    fixture.mutate = false;
    CHECK(hlsl_lift_transaction_try_result(transaction, 1, &result) == HLSL_LIFT_VERIFIED);
    const HLSLLiftStep *first = hlsl_lift_transaction_step(transaction, 0);
    CHECK(first && !strcmp(first->identifier, HLSL_RESULT_LIFT_ID) &&
          first->version == HLSL_RESULT_LIFT_VERSION && first->instruction_index == 1 &&
          first->source_instruction_index == 30 && first->producer_instruction_index == 0 &&
          first->producer_source_instruction_index == 20 && first->edit_count == 2 &&
          first->edits[0].operand_index == 1 && first->edits[1].operand_index == 2);
    const USILProgram *accepted = hlsl_lift_transaction_program(transaction);
    CHECK(accepted->instructions[0].opcode == USIL_OP_NOP);
    CHECK(accepted->instructions[1].opcode == USIL_OP_MUL);
    CHECK(hlsl_lift_transaction_try_copy(transaction, 1, &result) ==
          HLSL_LIFT_PRECONDITION_REJECTED);
    CHECK(hlsl_lift_transaction_program(transaction) == accepted);
    CHECK(hlsl_lift_transaction_try_result(transaction, 2, &result) == HLSL_LIFT_VERIFIED);
    CHECK(hlsl_lift_transaction_program(transaction)->instructions[1].opcode == USIL_OP_NOP);
    CHECK(hlsl_lift_transaction_program(transaction)->instructions[2].opcode == USIL_OP_MUL);
    const HLSLLiftStep *second = hlsl_lift_transaction_step(transaction, 1);
    CHECK(second && second->source_instruction_index == 40 &&
          second->producer_instruction_index == 1 && second->producer_source_instruction_index == 20 &&
          !strcmp(second->before.source_sha256, first->after.source_sha256));
    CHECK(instructions[0].opcode == USIL_OP_MUL && instructions[1].opcode == USIL_OP_MOV);
    const HLSLLiftArtifact *artifact = hlsl_lift_transaction_artifact(transaction);
    const char *accepted_source = artifact->source;
    fixture.mutate = true;
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) == HLSL_LIFT_DXBC_MISMATCH);
    CHECK(!hlsl_lift_transaction_is_high_level(transaction) && artifact->source == accepted_source);
    fixture.mutate = false;
    fixture.status = HLSL_LIFT_EMISSION_REJECTED;
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) ==
          HLSL_LIFT_EMISSION_REJECTED);
    CHECK(artifact->source == accepted_source);
    fixture.status = HLSL_LIFT_VERIFIED;
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) == HLSL_LIFT_VERIFIED);
    CHECK(hlsl_lift_transaction_is_high_level(transaction));
    const HLSLLiftStep *expression = hlsl_lift_transaction_step(transaction, 2);
    CHECK(hlsl_lift_transaction_step_count(transaction) == 3 && expression &&
          !strcmp(expression->identifier, HLSL_HIGH_LEVEL_LIFT_ID) &&
          expression->version == HLSL_HIGH_LEVEL_LIFT_VERSION &&
          expression->instruction_index == -1 && expression->producer_instruction_index == -1 &&
          !expression->edits && expression->edit_count == 0 &&
          !strcmp(expression->before.source_sha256, second->after.source_sha256) &&
          !strcmp(expression->after.source_sha256, result.source_sha256));
    CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) ==
          HLSL_LIFT_COMPOSITION_UNSUPPORTED);
    CHECK(hlsl_lift_transaction_try_result(transaction, 2, &result) ==
          HLSL_LIFT_COMPOSITION_UNSUPPORTED);
    CHECK(hlsl_lift_transaction_step_count(transaction) == 3 &&
          hlsl_lift_transaction_step(transaction, 0) == first &&
          hlsl_lift_transaction_step(transaction, 1) == second &&
          hlsl_lift_transaction_step(transaction, 2) == expression);
    hlsl_lift_transaction_destroy(transaction);
    return true;
}

static DXBCOperand emission_reg(DXBCOperandType type, int index) {
    DXBCOperand operand = reg(type, index, 0xf0);
    operand.register_index_dim = 1;
    operand.index_has_immediate[0] = true;
    operand.index_values[0] = (uint32_t)index;
    return operand;
}

static bool check_expression_emission(void) {
    USILInstruction instructions[4] = {0};
    instructions[0].opcode = USIL_OP_MUL;
    instructions[0].operand_count = 3;
    instructions[0].operands[0] = emission_reg(OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[1] = emission_reg(OPERAND_TYPE_INPUT, 0);
    instructions[0].operands[2] = emission_reg(OPERAND_TYPE_INPUT, 0);
    for (int lane = 0; lane < 4; ++lane)
        instructions[0].operands[2].swizzle[lane] = (uint8_t)(3 - lane);
    instructions[1] = instructions[0];
    instructions[1].opcode = USIL_OP_ADD;
    instructions[1].operands[0] = emission_reg(OPERAND_TYPE_TEMP, 1);
    instructions[1].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 0);
    instructions[2] = instructions[0];
    instructions[2].operands[0] = emission_reg(OPERAND_TYPE_OUTPUT, 0);
    instructions[2].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 1);
    instructions[3].opcode = USIL_OP_RET;
    DXBCSignatureElement input = {
        .semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15};
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram program = {.shader_type_model = "ps_5_0",
                           .instructions = instructions,
                           .instruction_count = 4,
                           .instruction_alloc = 4,
                           .temp_count = 2,
                           .inputs = &input,
                           .input_count = 1,
                           .input_alloc = 1,
                           .outputs = &output,
                           .output_count = 1,
                           .output_alloc = 1,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                           .shader_model_major = 5};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap source_map;
    options.expression_source_map = &source_map;
    for (int index = 0; index < 4; ++index)
        instructions[index].source_instruction_index = (uint32_t)(100 + index);
    HLSLEmitDiagnostic diagnostic;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                            &diagnostic));
    CHECK(strstr(source.buf, "o0 = ((") && strstr(source.buf, " * ") && strstr(source.buf, " + "));
    CHECK(!strstr(source.buf, "float4 r") && !strstr(source.buf, "u_xlat_temp"));
    CHECK(source_map.complete && source_map.count == 4);
    for (size_t index = 0; index < source_map.count; ++index) {
        const HLSLExpressionOrigin *origin = &source_map.origins[index];
        CHECK(origin->instruction_index == (int)index &&
              origin->source_instruction_index == 100 + index);
        CHECK(origin->source_begin < origin->source_end && origin->source_end <= source.len);
        CHECK(origin->kind ==
              (index == 3 ? HLSL_EXPRESSION_ORIGIN_RETURN : HLSL_EXPRESSION_ORIGIN_EXPRESSION));
    }
    CHECK(source_map.origins[2].source_begin <= source_map.origins[1].source_begin &&
          source_map.origins[1].source_begin <= source_map.origins[0].source_begin &&
          source_map.origins[0].source_end <= source_map.origins[1].source_end &&
          source_map.origins[1].source_end <= source_map.origins[2].source_end);
    CHECK(source_map.origins[0].destination_lanes == 15);
    CHECK(hlsl_expression_source_map_matches(&source_map, &program, source.buf));
    HLSLExpressionSourceMap valid_map = source_map;
    for (int mutation = 0; mutation < 8; ++mutation) {
        if (mutation == 0)
            source_map.complete = false;
        if (mutation == 1)
            source_map.count--;
        if (mutation == 2)
            source_map.origins[0].instruction_index++;
        if (mutation == 3)
            source_map.origins[0].source_instruction_index++;
        if (mutation == 4)
            source_map.origins[0].destination_lanes = 1;
        if (mutation == 5)
            source_map.origins[0].source_end = source.len + 1;
        if (mutation == 6)
            source_map.origins[0].source_end = source_map.origins[0].source_begin;
        if (mutation == 7)
            source_map.origins[2].kind = HLSL_EXPRESSION_ORIGIN_DEAD;
        CHECK(!hlsl_expression_source_map_matches(&source_map, &program, source.buf));
        source_map = valid_map;
    }
    char *first = malloc(source.len + 1);
    CHECK(first);
    memcpy(first, source.buf, source.len + 1);
    sb_free(&source);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strcmp(first, source.buf) == 0);
    free(first);
    /* A dead consumer also disposes of its nested single-use producers. */
    DXBCOperand saved_source = instructions[2].operands[1];
    instructions[2].operands[1] = emission_reg(OPERAND_TYPE_INPUT, 0);
    sb_free(&source);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(source_map.complete && source_map.origins[0].kind == HLSL_EXPRESSION_ORIGIN_DEAD &&
          source_map.origins[1].kind == HLSL_EXPRESSION_ORIGIN_DEAD);
    CHECK(source_map.origins[0].source_end == 0 && source_map.origins[1].source_end == 0);
    instructions[2].operands[1] = saved_source;
    /* Two uses retain one evaluated typed value instead of duplicating MUL. */
    instructions[1].operands[2] = emission_reg(OPERAND_TYPE_TEMP, 0);
    instructions[1].operands[2].swizzle[0] = 3;
    sb_free(&source);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "const float4 dxbc_value_i0 = "));
    CHECK(strstr(source.buf, "dxbc_value_i0.wyzw"));
    CHECK(source_map.complete &&
          source_map.origins[0].source_end < source_map.origins[2].source_begin);
    CHECK(source_map.origins[2].source_begin <= source_map.origins[1].source_begin &&
          source_map.origins[1].source_end <= source_map.origins[2].source_end);
    const char *reserved[] = {"dxbc_value_i0"};
    options.reserved_preprocessor_identifiers = reserved;
    options.reserved_preprocessor_identifier_count = 1;
    sb_free(&source);
    sb_init(&source);
    CHECK(!hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                             &diagnostic));
    CHECK(diagnostic.reason == HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY);
    CHECK(!source_map.complete && source_map.count == 0);
    const char *collisions[] = {"float4", "mad", "main", "appdata", "v0", "o0", "SV_Target"};
    for (size_t index = 0; index < sizeof(collisions) / sizeof(collisions[0]); ++index) {
        options.reserved_preprocessor_identifiers = &collisions[index];
        sb_free(&source);
        sb_init(&source);
        CHECK(!hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                                 &diagnostic));
        CHECK(diagnostic.reason == HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY);
    }
    const char *non_collisions[] = {"float", "dxbc_value_i", "v", "unused_keyword"};
    options.reserved_preprocessor_identifiers = non_collisions;
    options.reserved_preprocessor_identifier_count =
        sizeof(non_collisions) / sizeof(non_collisions[0]);
    sb_free(&source);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    options.reserved_preprocessor_identifiers = NULL;
    options.reserved_preprocessor_identifier_count = 0;
    for (int mutation = 0; mutation < 7; ++mutation) {
        USILInstruction saved = instructions[0];
        if (mutation == 0)
            instructions[0].precise_mask = 1;
        if (mutation == 1)
            instructions[0].saturate = true;
        if (mutation == 2)
            instructions[0].operands[0].destination_mask = 0x70;
        if (mutation == 3)
            instructions[0].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 1);
        if (mutation == 4)
            instructions[0].operands[1].has_abs = true;
        if (mutation == 5)
            input.component_type = 1;
        if (mutation == 6) {
            instructions[0].opcode = USIL_OP_DERIV_RTX;
            instructions[0].operand_count = 2;
        }
        sb_free(&source);
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
        instructions[0] = saved;
        input.component_type = 3;
    }
    /* MOV literals retain every raw word through expression emission. */
    USILInstruction saved_instructions[4];
    memcpy(saved_instructions, instructions, sizeof(instructions));
    memset(instructions, 0, sizeof(instructions));
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0] = emission_reg(OPERAND_TYPE_OUTPUT, 0);
    DXBCOperand *literal = &instructions[0].operands[1];
    literal->type = OPERAND_TYPE_IMMEDIATE32;
    literal->imm_value_count = 4;
    literal->imm_values[0] = UINT32_C(0x80000000);
    literal->imm_values[1] = 1;
    literal->imm_values[2] = UINT32_C(0x7fc12345);
    literal->imm_values[3] = UINT32_C(0x3f800000);
    instructions[1].opcode = USIL_OP_RET;
    program.instruction_count = 2;
    sb_free(&source);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "float4(-0.0f, asfloat(0x00000001u), asfloat(0x7FC12345u), 1.0f)"));
    literal->imm_value_count = 1;
    sb_free(&source);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "float4(-0.0f)"));
    memcpy(instructions, saved_instructions, sizeof(instructions));
    program.instruction_count = 4;
    USILInstruction *long_program =
        calloc(HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT + 1u, sizeof(*long_program));
    CHECK(long_program);
    for (int index = 0; index <= HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++index)
        long_program[index].opcode = USIL_OP_NOP;
    memcpy(long_program, instructions, 3u * sizeof(*instructions));
    long_program[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT].opcode = USIL_OP_RET;
    program.instructions = long_program;
    program.instruction_count = program.instruction_alloc = HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT + 1;
    sb_free(&source);
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    free(long_program);
    sb_free(&source);
    return true;
}

static bool check_conditional_emission(void) {
    USILInstruction instructions[12] = {0};
    USILInstruction condition = {
        .opcode = USIL_OP_IF, .operand_count = 1, .condition_test = DXBC_INSTRUCTION_TEST_NONZERO};
    condition.operands[0] = emission_reg(OPERAND_TYPE_INPUT, 1);
    condition.operands[0].swizzle_mode = 2;
    USILInstruction multiply = {.opcode = USIL_OP_MUL, .operand_count = 3};
    multiply.operands[0] = emission_reg(OPERAND_TYPE_TEMP, 0);
    multiply.operands[1] = multiply.operands[2] = emission_reg(OPERAND_TYPE_INPUT, 0);
    for (int lane = 0; lane < 4; ++lane)
        multiply.operands[2].swizzle[lane] = (uint8_t)((lane + 1) % 4);
    instructions[0] = condition;
    instructions[1] = multiply;
    instructions[2].opcode = USIL_OP_ELSE;
    instructions[3] = multiply;
    instructions[3].operands[2].swizzle[0] = 3;
    instructions[4].opcode = USIL_OP_ENDIF;
    instructions[5] = multiply;
    instructions[5].operands[0] = emission_reg(OPERAND_TYPE_OUTPUT, 0);
    instructions[5].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 0);
    instructions[6].opcode = USIL_OP_RET;
    USILInstruction output = instructions[5];
    DXBCSignatureElement inputs[2] = {
        {.semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15},
        {.semantic_name = "TEXCOORD",
         .semantic_index = 1,
         .register_id = 1,
         .component_type = 3,
         .mask = 15,
         .rw_mask = 1}};
    DXBCSignatureElement signature = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram program = {.shader_type_model = "ps_5_0",
                           .instructions = instructions,
                           .instruction_count = 7,
                           .instruction_alloc = 12,
                           .temp_count = 2,
                           .inputs = inputs,
                           .input_count = 2,
                           .input_alloc = 2,
                           .outputs = &signature,
                           .output_count = 1,
                           .output_alloc = 1,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                           .shader_model_major = 5};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    HLSLEmitDiagnostic diagnostic;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    bool emitted = hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                                     &diagnostic);
    if (!emitted)
        fprintf(stderr, "Conditional emission: %s at instruction %d, phase %d\n",
                hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index,
                (int)diagnostic.phase);
    CHECK(emitted);
    CHECK(strstr(source.buf, "float4 dxbc_merge_i5_r0;"));
    CHECK(strstr(source.buf, "dxbc_merge_i5_r0 = dxbc_value_i1;"));
    CHECK(strstr(source.buf, "dxbc_merge_i5_r0 = dxbc_value_i3;"));
    CHECK(strstr(source.buf, "[branch] if (asuint((v1.x)))"));
    CHECK(!strstr(source.buf, "float4 r") && !strstr(source.buf, "u_xlat_temp"));
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    for (int index = 0; index < 7; index += 2) {
        if (index == 6)
            break;
        CHECK(map.origins[index].kind == HLSL_EXPRESSION_ORIGIN_CONTROL);
        CHECK(map.origins[index].destination_lanes == 0);
        CHECK(map.origins[index].source_begin < map.origins[index].source_end);
    }
    map.origins[0].kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
    CHECK(!hlsl_expression_source_map_matches(&map, &program, source.buf));
    sb_free(&source);
    const char *reserved = "dxbc_merge_i5_r0";
    options.reserved_preprocessor_identifiers = &reserved;
    options.reserved_preprocessor_identifier_count = 1;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                             &diagnostic));
    CHECK(diagnostic.reason == HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY && !map.complete);
    sb_free(&source);
    options.reserved_preprocessor_identifiers = NULL;
    options.reserved_preprocessor_identifier_count = 0;
    for (int mutation = 0; mutation < 7; ++mutation) {
        USILInstruction saved = instructions[1];
        if (mutation == 0)
            instructions[1].operands[0].destination_mask = 0x70;
        if (mutation == 1)
            instructions[1].precise_mask = 1;
        if (mutation == 2)
            instructions[1] = (USILInstruction){.opcode = USIL_OP_NOP};
        if (mutation == 3)
            instructions[1] = (USILInstruction){.opcode = USIL_OP_RET};
        if (mutation == 4)
            instructions[1].operands[0] = emission_reg(OPERAND_TYPE_OUTPUT, 0);
        if (mutation == 5)
            instructions[1].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 1);
        if (mutation == 6) {
            instructions[1].opcode = USIL_OP_DERIV_RTX;
            instructions[1].operand_count = 2;
        }
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
        CHECK(!map.complete && map.count == 0);
        sb_free(&source);
        instructions[1] = saved;
    }
    /* Resolve an inner phi as an outer incoming vector, retaining lexical
     * scope and all four lanes rather than using a guessed register value. */
    instructions[1] = condition;
    instructions[2] = multiply;
    instructions[3] = (USILInstruction){.opcode = USIL_OP_ELSE};
    instructions[4] = multiply;
    instructions[5] = (USILInstruction){.opcode = USIL_OP_ENDIF};
    instructions[6] = (USILInstruction){.opcode = USIL_OP_ELSE};
    instructions[7] = multiply;
    instructions[8] = (USILInstruction){.opcode = USIL_OP_ENDIF};
    instructions[9] = output;
    instructions[10] = (USILInstruction){.opcode = USIL_OP_RET};
    program.instruction_count = 11;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "dxbc_merge_i9_r0 = dxbc_merge_i6_r0;"));
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    sb_free(&source);

    instructions[0] = move(emission_reg(OPERAND_TYPE_TEMP, 0), emission_reg(OPERAND_TYPE_INPUT, 0));
    instructions[1] = condition;
    instructions[1].operands[0] = emission_reg(OPERAND_TYPE_TEMP, 0);
    instructions[1].operands[0].swizzle_mode = 2;
    instructions[1].operands[0].swizzle[0] = 2;
    instructions[1].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
    instructions[2] = multiply;
    instructions[3] = (USILInstruction){.opcode = USIL_OP_ENDIF};
    instructions[4] = output;
    instructions[5] = (USILInstruction){.opcode = USIL_OP_RET};
    program.instruction_count = 6;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "[branch] if (!asuint(dxbc_value_i0.z))"));
    CHECK(strstr(source.buf, "dxbc_merge_i4_r0 = dxbc_value_i0;"));
    CHECK(strstr(source.buf, "dxbc_merge_i4_r0 = dxbc_value_i2;"));
    sb_free(&source);
    /* A dead unpruned phi has an undefined false arm. It stays un-emitted. */
    instructions[2].operands[0] = emission_reg(OPERAND_TYPE_TEMP, 1);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(!strstr(source.buf, "dxbc_merge"));
    sb_free(&source);
    return true;
}

int main(void) {
    if (!check_multiple_results() || !check_modified_moves() ||
        !check_merge_and_undefined_lanes() || !check_loop_phi() || !check_structured_flow_edges() ||
        !check_switch_flow_and_scope() || !check_flow_structure_validation() ||
        !check_control_region_proofs() || !check_exhaustive_postdominance() ||
        !check_long_dominator_chain() || !check_copy_candidates() || !check_result_candidates() ||
        !check_effects() || !check_transactions() || !check_result_transactions() ||
        !check_expression_emission() || !check_conditional_emission())
        return 1;
    puts("HLSL dataflow contracts passed");
    return 0;
}
