// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_copy_lift.h"
#include "translation/hlsl_lift_transaction.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "dxbc/dxbc_decoder.h"
#include "translation/hlsl_source_quality_internal.h"
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

/* Independent, deliberately tiny lane machine for generated MOV-only programs.
 * It reads all sources before committing a write and rejects undefined lanes. */
static bool evaluate_moves(const USILProgram *program, const uint32_t input[4],
                            uint32_t output[4], unsigned *output_mask) {
    uint32_t temporaries[2][4] = {{0}};
    unsigned defined[2] = {0};
    memset(output, 0, sizeof(uint32_t) * 4);
    *output_mask = 0;
    for (int i = 0; i < program->instruction_count; ++i) {
        const USILInstruction *instruction = &program->instructions[i];
        if (instruction->opcode == USIL_OP_RET)
            return true;
        if (instruction->opcode == USIL_OP_NOP)
            continue;
        if (instruction->opcode != USIL_OP_MOV || instruction->operand_count != 2)
            return false;
        const DXBCOperand *destination = &instruction->operands[0];
        const DXBCOperand *source = &instruction->operands[1];
        const unsigned mask = destination->destination_mask >> 4;
        if (source->swizzle_mode != 1 || source->has_abs || source->has_neg ||
            instruction->saturate || source->register_index < 0 || source->register_index >= 2 ||
            destination->register_index < 0 || destination->register_index >= 2)
            return false;
        uint32_t values[4] = {0};
        for (unsigned lane = 0; lane < 4; ++lane) {
            if (!(mask & (1U << lane)))
                continue;
            const unsigned selected = source->swizzle[lane];
            if (selected >= 4)
                return false;
            if (source->type == OPERAND_TYPE_INPUT && source->register_index == 0) {
                values[lane] = input[selected];
            } else if (source->type == OPERAND_TYPE_TEMP &&
                       (defined[source->register_index] & (1U << selected))) {
                values[lane] = temporaries[source->register_index][selected];
            } else {
                return false;
            }
        }
        for (unsigned lane = 0; lane < 4; ++lane) {
            if (!(mask & (1U << lane)))
                continue;
            if (destination->type == OPERAND_TYPE_OUTPUT && destination->register_index == 0) {
                output[lane] = values[lane];
                *output_mask |= 1U << lane;
            } else if (destination->type == OPERAND_TYPE_TEMP) {
                temporaries[destination->register_index][lane] = values[lane];
                defined[destination->register_index] |= 1U << lane;
            } else {
                return false;
            }
        }
    }
    return false;
}

static bool check_generated_copy_lanes(void) {
    const uint32_t input[4] = {0x80000000U, 0x7fc01234U, 0x00000001U, 0xffffffffU};
    unsigned cases = 0, admitted = 0;
    for (unsigned stage = 0; stage < 2; ++stage)
        for (unsigned model = 4; model <= 5; ++model)
            for (unsigned mask = 1; mask < 16; ++mask)
                for (unsigned selection = 0; selection < 256; ++selection) {
                    USILInstruction instructions[4] = {0};
                    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0xf0),
                                           reg(OPERAND_TYPE_INPUT, 0, 0));
                    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 1, (uint8_t)(mask << 4)),
                                           reg(OPERAND_TYPE_TEMP, 0, 0));
                    const unsigned output_lane = (selection >> 4) & 3U;
                    const unsigned read_lane = selection & 3U;
                    instructions[2] = move(reg(OPERAND_TYPE_OUTPUT, 0, (uint8_t)(0x10U << output_lane)),
                                           reg(OPERAND_TYPE_TEMP, 1, 0));
                    instructions[2].operands[1].swizzle[output_lane] = (uint8_t)read_lane;
                    for (unsigned lane = 0; lane < 4; ++lane)
                        instructions[1].operands[1].swizzle[lane] =
                            (uint8_t)((selection >> (lane * 2)) & 3U);
                    instructions[3].opcode = USIL_OP_RET;
                    USILProgram program = {.instructions = instructions, .instruction_count = 4,
                        .temp_count = 2, .has_stage_contract = true,
                        .program_type = stage ? DXBC_PROGRAM_TYPE_PIXEL : DXBC_PROGRAM_TYPE_VERTEX,
                        .shader_model_major = (uint8_t)model};
                    USILInstruction before[4];
                    memcpy(before, instructions, sizeof(before));
                    HLSLCopyLift *candidate = NULL;
                    const HLSLCopyLiftStatus status = hlsl_copy_lift_create(&program, 1, &candidate);
                    const bool defined = (mask & (1U << read_lane)) != 0;
                    CHECK(status == (defined ? HLSL_COPY_LIFT_OK : HLSL_COPY_LIFT_UNDEFINED_SOURCE));
                    CHECK((candidate != NULL) == defined);
                    if (defined) {
                        uint32_t baseline[4], lifted[4];
                        unsigned baseline_mask, lifted_mask;
                        CHECK(evaluate_moves(&program, input, baseline, &baseline_mask));
                        CHECK(evaluate_moves(hlsl_copy_lift_program(candidate), input, lifted, &lifted_mask));
                        CHECK(baseline_mask == lifted_mask && memcmp(baseline, lifted, sizeof(baseline)) == 0);
                        ++admitted;
                    }
                    hlsl_copy_lift_destroy(candidate);
                    CHECK(memcmp(before, instructions, sizeof(before)) == 0);
                    ++cases;
                }
    CHECK(cases == 15360 && admitted == 8192);
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
    CHECK(strstr(source.buf, "return ((") && strstr(source.buf, " * ") && strstr(source.buf, " + "));
    CHECK(strstr(source.buf, "float4 main(float4 texcoord0 : TEXCOORD0) : SV_Target"));
    CHECK(!strstr(source.buf, "v0") && !strstr(source.buf, "o0") && !strstr(source.buf, "appdata"));
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
    const char *collisions[] = {"float4", "mad", "main", "SV_Target"};
    for (size_t index = 0; index < sizeof(collisions) / sizeof(collisions[0]); ++index) {
        options.reserved_preprocessor_identifiers = &collisions[index];
        sb_free(&source);
        sb_init(&source);
        CHECK(!hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                                 &diagnostic));
        CHECK(diagnostic.reason == HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY);
    }
    const char *non_collisions[] = {"float", "dxbc_value_i", "v", "unused_keyword", "appdata", "v0", "o0"};
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
            instructions[0].operands[1].min_precision = 1;
        if (mutation == 5)
            input.component_type = 1;
        if (mutation == 6) {
            instructions[0].opcode = USIL_OP_DERIV_RTX;
            instructions[0].operand_count = 2;
            program.program_type = DXBC_PROGRAM_TYPE_VERTEX;
            memcpy(program.shader_type_model, "vs_5_0", sizeof("vs_5_0"));
        }
        sb_free(&source);
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
        instructions[0] = saved;
        input.component_type = 3;
        program.program_type = DXBC_PROGRAM_TYPE_PIXEL;
        memcpy(program.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
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

typedef struct {
    DXBCDocument document;
    DXBCContainer semantic;
    DXBCStageContract contract;
    USILProgram program;
    int condition, then_value, else_value, join_value, output;
} NaturalIfFixture;

static void natural_if_write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

/* This is authored signature/token grammar, not a retained compiler target.
 * The normal document, stage-contract and USIL decoders supply its authority. */
static size_t natural_if_signature(uint8_t *bytes, bool output,
    const DXBCSignatureElement *fields, unsigned count) {
    if (!count || count > 3) return 0;
    const size_t name_offset = 8u + count * 24u;
    size_t offsets[3], payload_size = name_offset;
    for (unsigned index = 0; index < count; ++index) {
        unsigned previous = 0;
        while (previous < index && strcmp(fields[index].semantic_name,
            fields[previous].semantic_name)) ++previous;
        offsets[index] = previous < index ? offsets[previous] : payload_size;
        if (previous == index) payload_size += strlen(fields[index].semantic_name) + 1u;
    }
    memcpy(bytes, output ? "OSGN" : "ISGN", 4);
    natural_if_write_u32(bytes + 4, (uint32_t)payload_size);
    uint8_t *payload = bytes + 8;
    natural_if_write_u32(payload, count);
    natural_if_write_u32(payload + 4, 8);
    for (unsigned index = 0; index < count; ++index) {
        uint8_t *element = payload + 8u + index * 24u;
        natural_if_write_u32(element, (uint32_t)offsets[index]);
        natural_if_write_u32(element + 4, fields[index].semantic_index);
        natural_if_write_u32(element + 8, fields[index].system_value);
        natural_if_write_u32(element + 12, fields[index].component_type);
        natural_if_write_u32(element + 16, fields[index].register_id);
        natural_if_write_u32(element + 20, fields[index].mask | (uint32_t)fields[index].rw_mask << 8u);
        memcpy(payload + offsets[index], fields[index].semantic_name,
            strlen(fields[index].semantic_name) + 1u);
    }
    return payload_size + 8u;
}

static uint32_t natural_if_source_token(DXBCOperandType type, const uint8_t components[4]) {
    uint32_t token = UINT32_C(0x00100006) | (uint32_t)type << 12u;
    for (unsigned lane = 0; lane < 4; ++lane)
        token |= (uint32_t)components[lane] << (4u + lane * 2u);
    return token;
}

static void natural_if_fixture_dispose(NaturalIfFixture *fixture) {
    usil_free(&fixture->program);
    dxbc_free(&fixture->semantic);
    dxbc_stage_contract_free(&fixture->contract);
    dxbc_document_free(&fixture->document);
}

static bool natural_if_fixture_decode_words(NaturalIfFixture *fixture, uint32_t program_token,
    const uint32_t *words, size_t word_count, const DXBCSignatureElement *inputs,
    unsigned input_count, const DXBCSignatureElement *outputs, unsigned output_count) {
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    natural_if_write_u32(bytes + 20, 1);
    natural_if_write_u32(bytes + 28, 3);
    size_t offset = 44;
    for (unsigned index = 0; index < 2; ++index) {
        natural_if_write_u32(bytes + 32u + index * 4u, (uint32_t)offset);
        const size_t signature_size = natural_if_signature(bytes + offset, index != 0,
            index ? outputs : inputs, index ? output_count : input_count);
        CHECK(signature_size);
        offset += signature_size;
        offset = (offset + 3u) & ~(size_t)3u;
    }
    CHECK(word_count <= 128 && offset + 16u + word_count * 4u <= sizeof(bytes));
    natural_if_write_u32(bytes + 40, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    natural_if_write_u32(bytes + offset + 4, (uint32_t)(word_count + 2u) * 4u);
    natural_if_write_u32(bytes + offset + 8, program_token);
    natural_if_write_u32(bytes + offset + 12, (uint32_t)word_count + 2u);
    for (size_t index = 0; index < word_count; ++index)
        natural_if_write_u32(bytes + offset + 16u + index * 4u, words[index]);
    const size_t byte_count = offset + 16u + word_count * 4u;
    natural_if_write_u32(bytes + 24, (uint32_t)byte_count);
    CHECK(dxbc_compute_hash(bytes, byte_count, bytes + 4));
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContractDiagnostic contract_diagnostic;
    CHECK(dxbc_document_parse(&fixture->document, bytes, byte_count, &document_diagnostic));
    CHECK(dxbc_document_decode_semantic(&fixture->document, &fixture->semantic));
    CHECK(dxbc_stage_contract_decode(&fixture->document, &fixture->semantic,
        &fixture->contract, &contract_diagnostic));
    CHECK(usil_translate_with_stage_contract(&fixture->program, &fixture->semantic,
        &fixture->contract));
    CHECK(fixture->program.has_parsed_signature_authority);
    return true;
}

static uint32_t natural_comparison_raw_opcode(USILOpcode opcode) {
    switch (opcode) {
    case USIL_OP_LT: return 49;
    case USIL_OP_GE: return 29;
    case USIL_OP_EQ: return 24;
    case USIL_OP_NE: return 57;
    default: return UINT32_MAX;
    }
}

typedef enum {
    NATURAL_IF_RIGHT_SCALAR_LITERAL,
    NATURAL_IF_RIGHT_VECTOR_LITERAL,
    NATURAL_IF_RIGHT_VECTOR_INPUT,
    NATURAL_IF_RIGHT_SCALAR_INPUT
} NaturalIfRightOperand;

typedef struct {
    USILOpcode then_opcode, else_opcode, join_opcode;
    NaturalIfRightOperand arm_right, join_right;
    bool custom_literals;
    uint32_t then_bits, else_bits, join_bits;
} NaturalIfArithmetic;

static uint32_t natural_arithmetic_raw_opcode(USILOpcode opcode) {
    /* Public decoder names and the USIL decoder retain these actual DXBC
     * opcodes. There is no separate raw-opcode enum in the public headers. */
    switch (opcode) {
    case USIL_OP_ADD: return 0;
    case USIL_OP_MUL: return 56;
    case USIL_OP_DIV: return 14;
    case USIL_OP_MIN: return 51;
    case USIL_OP_MAX: return 52;
    default: return UINT32_MAX;
    }
}

static bool natural_if_fixture_init_arithmetic(NaturalIfFixture *fixture, unsigned width,
    uint8_t temp_mask, bool nonzero, bool dead_phi, bool vector_literal, bool temp_condition,
    USILOpcode comparison, uint8_t predicate_mask, uint32_t comparison_bits,
    const NaturalIfArithmetic *arithmetic) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    CHECK(width >= 1 && width <= 3);
    CHECK(!arithmetic || (natural_arithmetic_raw_opcode(arithmetic->then_opcode) != UINT32_MAX &&
        natural_arithmetic_raw_opcode(arithmetic->else_opcode) != UINT32_MAX &&
        natural_arithmetic_raw_opcode(arithmetic->join_opcode) != UINT32_MAX &&
        arithmetic->arm_right <= NATURAL_IF_RIGHT_SCALAR_INPUT && arithmetic->join_right <= NATURAL_IF_RIGHT_SCALAR_INPUT));
    const bool compared = comparison != USIL_OP_NOP;
    CHECK(!compared || (!temp_condition && !dead_phi && predicate_mask &&
        !(predicate_mask & (predicate_mask - 1u)) && predicate_mask <= 8 &&
        natural_comparison_raw_opcode(comparison) != UINT32_MAX));
    const uint8_t output_mask = (uint8_t)((1u << width) - 1u);
    uint8_t physical[4] = {0};
    unsigned physical_count = 0;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (temp_mask & (1u << lane)) physical[physical_count++] = (uint8_t)lane;
    CHECK(physical_count == width);
    const uint8_t identity[4] = {0, 1, 2, 3};
    uint8_t input_for_temp[4] = {0}, temp_for_output[4] = {0};
    for (unsigned lane = 0; lane < width; ++lane) {
        input_for_temp[physical[lane]] = (uint8_t)lane;
        temp_for_output[lane] = physical[lane];
    }
    for (unsigned lane = width; lane < 4; ++lane)
        temp_for_output[lane] = physical[0];
    uint32_t words[128];
    size_t word_count = 0;
#define NATURAL_WORD(value) do { \
    CHECK(word_count < sizeof(words) / sizeof(words[0])); \
    words[word_count++] = (value); \
} while (0)
#define NATURAL_INST(opcode, length) ((uint32_t)(opcode) | (uint32_t)(length) << 24u)
#define NATURAL_DEST(type, mask) (UINT32_C(0x00100002) | \
    (uint32_t)(type) << 12u | (uint32_t)(mask) << 4u)
#define NATURAL_RIGHT(mode, bits, components) do { \
    if ((mode) == NATURAL_IF_RIGHT_VECTOR_LITERAL) { \
        const uint32_t payload[] = {UINT32_C(0x80000000), 1, UINT32_C(0x7fc12345), UINT32_C(0x3f800000)}; \
        NATURAL_WORD(UINT32_C(0x00004002)); \
        for (unsigned component = 0; component < 4; ++component) NATURAL_WORD(payload[component]); \
    } else if ((mode) == NATURAL_IF_RIGHT_VECTOR_INPUT) { \
        NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, (components))); NATURAL_WORD(0); \
    } else if ((mode) == NATURAL_IF_RIGHT_SCALAR_INPUT) { \
        NATURAL_WORD(UINT32_C(0x0010100a)); NATURAL_WORD(1); \
    } else { \
        NATURAL_WORD(UINT32_C(0x00004001)); NATURAL_WORD(bits); \
    } \
} while (0)
    const NaturalIfRightOperand arm_right = arithmetic ? arithmetic->arm_right : NATURAL_IF_RIGHT_SCALAR_LITERAL;
    const NaturalIfRightOperand join_right = arithmetic ? arithmetic->join_right : NATURAL_IF_RIGHT_VECTOR_INPUT;
    NATURAL_WORD(NATURAL_INST(106, 1) | 1u << 11u);
    NATURAL_WORD(NATURAL_INST(98, 3) | 2u << 11u);
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_INPUT, output_mask)); NATURAL_WORD(0);
    NATURAL_WORD(NATURAL_INST(98, 3) | 2u << 11u);
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_INPUT, 1)); NATURAL_WORD(1);
    NATURAL_WORD(NATURAL_INST(101, 3));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_OUTPUT, output_mask)); NATURAL_WORD(0);
    NATURAL_WORD(NATURAL_INST(104, 2)); NATURAL_WORD(3);
    if (temp_condition) {
        NATURAL_WORD(NATURAL_INST(54, 5));
        NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(2);
        NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, input_for_temp)); NATURAL_WORD(0);
    }
    unsigned predicate_lane = 0;
    if (compared) {
        while (!(predicate_mask & (1u << predicate_lane))) ++predicate_lane;
        NATURAL_WORD(NATURAL_INST(natural_comparison_raw_opcode(comparison), 7));
        NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, predicate_mask)); NATURAL_WORD(2);
        NATURAL_WORD(UINT32_C(0x0010100a)); NATURAL_WORD(1);
        NATURAL_WORD(UINT32_C(0x00004001)); NATURAL_WORD(comparison_bits);
    }
    NATURAL_WORD(NATURAL_INST(31, 3) | (nonzero ? UINT32_C(0x40000) : 0));
    NATURAL_WORD(compared ? UINT32_C(0x0010000a) | (uint32_t)predicate_lane << 4u
        : temp_condition ? UINT32_C(0x0010000a) |
            (uint32_t)physical[width - 1u] << 4u : UINT32_C(0x0010100a));
    NATURAL_WORD(compared || temp_condition ? 2 : 1);
    NATURAL_WORD(NATURAL_INST(54, vector_literal ? 8 : 5));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(0);
    if (vector_literal) {
        const uint32_t bits[4] = {UINT32_C(0x80000000), 1,
            UINT32_C(0x7fc12345), UINT32_C(0x3f800000)};
        NATURAL_WORD(UINT32_C(0x00004002));
        for (unsigned lane = 0; lane < 4; ++lane) NATURAL_WORD(bits[lane]);
    } else {
        NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, input_for_temp)); NATURAL_WORD(0);
    }
    if (dead_phi) {
        /* TEMP2 has an undefined false edge but is never demanded at the join. */
        NATURAL_WORD(NATURAL_INST(54, 5));
        NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(2);
        NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, input_for_temp)); NATURAL_WORD(0);
    }
    NATURAL_WORD(NATURAL_INST(arithmetic ? natural_arithmetic_raw_opcode(arithmetic->then_opcode) : 0,
        arm_right == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 10 : 7));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(0);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, identity)); NATURAL_WORD(0);
    NATURAL_RIGHT(arm_right, arithmetic && arithmetic->custom_literals ? arithmetic->then_bits : UINT32_C(0x3fa00000), input_for_temp);
    NATURAL_WORD(NATURAL_INST(18, 1));
    NATURAL_WORD(NATURAL_INST(54, 5));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(0);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, input_for_temp)); NATURAL_WORD(0);
    NATURAL_WORD(NATURAL_INST(arithmetic ? natural_arithmetic_raw_opcode(arithmetic->else_opcode) : 56,
        arm_right == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 10 : 7));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(0);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, identity)); NATURAL_WORD(0);
    NATURAL_RIGHT(arm_right, arithmetic && arithmetic->custom_literals ? arithmetic->else_bits : UINT32_C(0x40000000), input_for_temp);
    NATURAL_WORD(NATURAL_INST(21, 1));
    NATURAL_WORD(NATURAL_INST(arithmetic ? natural_arithmetic_raw_opcode(arithmetic->join_opcode) : 0,
        join_right == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 10 : 7));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, output_mask)); NATURAL_WORD(1);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, temp_for_output)); NATURAL_WORD(0);
    NATURAL_RIGHT(join_right, arithmetic && arithmetic->custom_literals ? arithmetic->join_bits : UINT32_C(0x3f000000), identity);
    NATURAL_WORD(NATURAL_INST(54, 5));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_OUTPUT, output_mask)); NATURAL_WORD(0);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, identity)); NATURAL_WORD(1);
    NATURAL_WORD(NATURAL_INST(62, 1));
#undef NATURAL_DEST
#undef NATURAL_INST
#undef NATURAL_WORD
#undef NATURAL_RIGHT
    const DXBCSignatureElement inputs[2] = {
        {.semantic_name = "TEXCOORD", .component_type = 3, .mask = output_mask, .rw_mask = output_mask},
        {.semantic_name = "TEXCOORD", .semantic_index = 1, .register_id = 1,
            .component_type = 3, .mask = 1, .rw_mask = 1}};
    const DXBCSignatureElement output = {.semantic_name = "SV_Target", .system_value = 64,
        .component_type = 3, .mask = output_mask, .rw_mask = (uint8_t)(15u & ~output_mask)};
    CHECK(natural_if_fixture_decode_words(fixture, UINT32_C(0x00000050), words, word_count,
        inputs, 2, &output, 1));
    CHECK(fixture->program.input_count == 2 && fixture->program.output_count == 1);
    CHECK(fixture->program.inputs[0].mask == output_mask &&
        fixture->program.inputs[0].rw_mask == output_mask &&
        fixture->program.inputs[1].mask == 1 && fixture->program.inputs[1].rw_mask == 1);
    CHECK(fixture->program.outputs[0].mask == output_mask &&
        fixture->program.outputs[0].rw_mask == (15u & ~output_mask));
    const int prefix = temp_condition || compared ? 1 : 0;
    const int extra = dead_phi ? 1 : 0;
    CHECK(fixture->program.instruction_count == 10 + prefix + extra);
    fixture->condition = prefix;
    fixture->then_value = 2 + prefix + extra;
    fixture->else_value = 5 + prefix + extra;
    fixture->join_value = 7 + prefix + extra;
    fixture->output = 8 + prefix + extra;
    CHECK(fixture->program.instructions[fixture->condition].source_instruction_index ==
        (uint32_t)(5 + prefix));
    CHECK(fixture->program.instructions[fixture->condition].condition_test == (nonzero ?
        DXBC_INSTRUCTION_TEST_NONZERO : DXBC_INSTRUCTION_TEST_ZERO));
    CHECK(usil_operand_destination_lane_mask(
        &fixture->program.instructions[fixture->then_value].operands[0]) == temp_mask);
    if (arithmetic) CHECK(fixture->program.instructions[fixture->then_value].opcode == arithmetic->then_opcode &&
        fixture->program.instructions[fixture->else_value].opcode == arithmetic->else_opcode &&
        fixture->program.instructions[fixture->join_value].opcode == arithmetic->join_opcode);
    return true;
}

static bool natural_if_fixture_init_mode(NaturalIfFixture *fixture, unsigned width,
    uint8_t temp_mask, bool nonzero, bool dead_phi, bool vector_literal, bool temp_condition,
    USILOpcode comparison, uint8_t predicate_mask, uint32_t comparison_bits) {
    return natural_if_fixture_init_arithmetic(fixture, width, temp_mask, nonzero, dead_phi,
        vector_literal, temp_condition, comparison, predicate_mask, comparison_bits, NULL);
}

static bool natural_if_fixture_init(NaturalIfFixture *fixture, unsigned width,
    uint8_t temp_mask, bool nonzero, bool dead_phi, bool vector_literal, bool temp_condition) {
    return natural_if_fixture_init_mode(fixture, width, temp_mask, nonzero, dead_phi,
        vector_literal, temp_condition, USIL_OP_NOP, 0, 0);
}

typedef struct {
    const USILProgram *program;
    uint32_t logical_widths;
    size_t observations;
    size_t declaration_events, return_events;
    size_t comparison_events;
    uint64_t arithmetic_owners;
    size_t reject_at;
    USILProgram *mutable_program;
    StringBuilder *mutable_source;
    HLSLExpressionSourceMap *mutable_map;
    size_t source_offset;
    size_t mutate_at;
    unsigned mutation;
    int arithmetic_instruction, condition_instruction;
    USILOpcode replacement_opcode;
    bool mutated;
    bool wrong_owner;
} NaturalIfObservations;

static bool observe_natural_if(void *context, const HLSLSourceQualityObservation *observation) {
    NaturalIfObservations *ledger = context;
    ++ledger->observations;
    if (ledger->reject_at && ledger->observations == ledger->reject_at) return false;
    if (observation->stage != ledger->program->program_type || observation->pass_index != 3 ||
        observation->entry_point_index != 4 || observation->source_unit_id != 0)
        ledger->wrong_owner = true;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION) {
        if (facts->instruction_index < 0) ++ledger->declaration_events;
        else if (facts->instruction_index < ledger->program->instruction_count &&
            ledger->program->instructions[facts->instruction_index].opcode == USIL_OP_RET)
            ++ledger->return_events;
    }
    if (facts->instruction_index >= 0 &&
        (facts->instruction_index >= ledger->program->instruction_count ||
         facts->source_instruction_index != ledger->program->instructions[
            facts->instruction_index].source_instruction_index))
        ledger->wrong_owner = true;
    if (facts->known && facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
        facts->components >= 1 && facts->components <= 4)
        ledger->logical_widths |= 1u << facts->components;
    if (observation->ast_kind == AST_EXPR_COMPARISON) {
        ++ledger->comparison_events;
        if (!facts->known || facts->value_kind != HLSL_SOURCE_VALUE_LOGICAL ||
            facts->components != 1 || facts->instruction_index < 0 ||
            facts->instruction_index >= ledger->program->instruction_count ||
            facts->lanes != usil_operand_destination_lane_mask(
                &ledger->program->instructions[facts->instruction_index].operands[0]))
            ledger->wrong_owner = true;
    }
    if (facts->instruction_index >= 0 && facts->instruction_index < ledger->program->instruction_count &&
        facts->instruction_index < 64 && facts->known && facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
        (observation->ast_kind == AST_EXPR_CALL || observation->ast_kind == AST_EXPR_BINARY)) {
        const USILInstruction *owner = &ledger->program->instructions[facts->instruction_index];
        if (owner->opcode == USIL_OP_MIN || owner->opcode == USIL_OP_MAX || owner->opcode == USIL_OP_DIV) {
            if (facts->lanes != usil_operand_destination_lane_mask(&owner->operands[0])) ledger->wrong_owner = true;
            else ledger->arithmetic_owners |= UINT64_C(1) << (unsigned)facts->instruction_index;
        }
    }
    if (ledger->mutable_program && ledger->observations == ledger->mutate_at) {
        USILProgram *program = ledger->mutable_program;
        switch (ledger->mutation) {
        case 0:
            program->instructions[0].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
            break;
        case 1:
            program->instructions[5].operands[2].imm_values[0] = UINT32_C(0x40400000);
            program->instructions[5].operands[2].immediate_words[0] = UINT32_C(0x40400000);
            break;
        case 2:
            memset(program->inputs[0].semantic_name, 0, sizeof(program->inputs[0].semantic_name));
            memcpy(program->inputs[0].semantic_name, "COORDINATE", sizeof("COORDINATE"));
            program->inputs[0].semantic_name_length = sizeof("COORDINATE") - 1u;
            break;
        case 3: {
            DXBCOperand *source = &program->instructions[7].operands[1];
            source->swizzle[0] = 3;
            source->swizzle[1] = 2;
            source->raw_token &= ~UINT32_C(0x00000ff0);
            for (unsigned lane = 0; lane < 4; ++lane)
                source->raw_token |= (uint32_t)source->swizzle[lane] << (4u + lane * 2u);
            break;
        }
        case 4:
            if (!ledger->mutable_source || ledger->source_offset >= ledger->mutable_source->len)
                return false;
            ledger->mutable_source->buf[ledger->source_offset] ^= 1;
            break;
        case 5:
            if (!ledger->mutable_map || ledger->mutable_map->count <= 2)
                return false;
            ++ledger->mutable_map->origins[2].source_end;
            break;
        case 6:
            program->instructions[0].opcode = USIL_OP_GE;
            break;
        case 7:
            program->instructions[0].operands[2].imm_values[0] = UINT32_C(0x3f000000);
            program->instructions[0].operands[2].immediate_words[0] = UINT32_C(0x3f000000);
            break;
        case 8: {
            DXBCOperand *source = &program->instructions[0].operands[1];
            source->register_index = 0;
            source->index_values[0] = 0;
            source->swizzle[0] = 1;
            source->raw_token = (source->raw_token & ~UINT32_C(0x30)) | UINT32_C(0x10);
            break;
        }
        case 9:
            program->instructions[1].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
            break;
        case 10:
            program->instructions[ledger->arithmetic_instruction].opcode = ledger->replacement_opcode;
            break;
        case 11: {
            USILInstruction *owner = &program->instructions[ledger->arithmetic_instruction];
            const DXBCOperand left = owner->operands[1];
            owner->operands[1] = owner->operands[2]; owner->operands[2] = left;
            break;
        }
        case 12: {
            DXBCOperand *denominator = &program->instructions[ledger->arithmetic_instruction].operands[2];
            memset(denominator->swizzle, 1, sizeof(denominator->swizzle));
            denominator->raw_token &= ~UINT32_C(0x00000ff0);
            for (unsigned lane = 0; lane < 4; ++lane)
                denominator->raw_token |= (uint32_t)denominator->swizzle[lane] << (4u + lane * 2u);
            break;
        }
        case 13:
            program->instructions[ledger->arithmetic_instruction].operands[2].imm_values[0] = UINT32_C(0x40600000);
            program->instructions[ledger->arithmetic_instruction].operands[2].immediate_words[0] = UINT32_C(0x40600000);
            break;
        case 14:
            program->instructions[ledger->condition_instruction].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
            break;
        case 15:
            if (!ledger->mutable_map || ledger->arithmetic_instruction < 0 ||
                (size_t)ledger->arithmetic_instruction >= ledger->mutable_map->count) return false;
            ++ledger->mutable_map->origins[ledger->arithmetic_instruction].source_end;
            break;
        }
        ledger->mutated = true;
    }
    return true;
}

static bool natural_if_maps_equal(const HLSLExpressionSourceMap *a,
    const HLSLExpressionSourceMap *b) {
    CHECK(a->complete == b->complete && a->count == b->count);
    for (size_t index = 0; index < a->count; ++index)
        CHECK(hlsl_expression_origins_equal(&a->origins[index], &b->origins[index]));
    return true;
}

static bool natural_if_emit(const USILProgram *program, StringBuilder *source,
    HLSLExpressionSourceMap *map, HLSLSourceQualityResult *quality,
    NaturalIfObservations *observations, HLSLEmitDiagnostic *diagnostic) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = map;
    options.source_quality = quality;
    options.source_quality_pass_index = 3;
    options.source_quality_entry_point_index = 4;
    options.source_quality_observer = observe_natural_if;
    options.source_quality_observer_context = observations;
    return hlsl_emit_with_options_diagnostic(program, source, NULL, NULL, NULL,
        &options, diagnostic);
}

static bool check_natural_if_positive(unsigned width, uint8_t temp_mask, bool nonzero,
    bool dead_phi, bool vector_literal, bool temp_condition) {
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init(&fixture, width, temp_mask, nonzero, dead_phi,
        vector_literal, temp_condition));
    HLSLExpressionSourceMap map, repeated_map;
    HLSLSourceQualityResult quality, repeated_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations observations = {.program = &fixture.program};
    StringBuilder source, repeated;
    sb_init(&source);
    sb_init(&repeated);
    const bool emitted = natural_if_emit(&fixture.program, &source, &map, &quality,
        &observations, &diagnostic);
    if (!emitted)
        fprintf(stderr, "Natural IF width %u/mask %u: %s at instruction %d, phase %d\n",
            width, temp_mask, hlsl_emit_reason_name(diagnostic.reason),
            diagnostic.instruction_index, (int)diagnostic.phase);
    CHECK(emitted && map.complete && map.count == (size_t)fixture.program.instruction_count);
    CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
    CHECK(observations.observations && !observations.wrong_owner);
    CHECK(observations.logical_widths & (1u << width));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.reasons &&
        quality.counts.inspected_units == 1 && !quality.counts.incomplete_units &&
        !quality.counts.residual_total && !quality.counts.unknown_provenance);
    CHECK(quality.counts.real_bitcasts > 0);
    CHECK(strstr(source.buf, nonzero ? "[branch] if (asuint(" : "[branch] if (!asuint("));
    if (temp_condition) {
        CHECK(fixture.program.instructions[fixture.condition].operands[0].type == OPERAND_TYPE_TEMP);
        CHECK(usil_operand_source_component(
            &fixture.program.instructions[fixture.condition].operands[0], 0) ==
            (temp_mask == 2 ? 1 : 3));
        CHECK(strstr(source.buf, width == 1 ? "asuint(dxbc_value_i0)" :
            "asuint(dxbc_value_i0.y)"));
    }
    CHECK(!strstr(source.buf, "float4 dxbc_value") && !strstr(source.buf, "float4 dxbc_merge"));
    CHECK(!strstr(source.buf, "u_xlat") && !strstr(source.buf, "float4 r0"));
    char declaration[96];
    if (width == 1)
        snprintf(declaration, sizeof(declaration), "float dxbc_merge_i%d_r0;", fixture.join_value);
    else
        snprintf(declaration, sizeof(declaration), "float%u dxbc_merge_i%d_r0;",
            width, fixture.join_value);
    CHECK(strstr(source.buf, declaration));
    char assignment[96];
    snprintf(assignment, sizeof(assignment), "dxbc_merge_i%d_r0 = dxbc_value_i%d;",
        fixture.join_value, fixture.then_value);
    CHECK(strstr(source.buf, assignment));
    snprintf(assignment, sizeof(assignment), "dxbc_merge_i%d_r0 = dxbc_value_i%d;",
        fixture.join_value, fixture.else_value);
    CHECK(strstr(source.buf, assignment));
    if (dead_phi) CHECK(!strstr(source.buf, "_r2;"));
    if (temp_mask == 2 || temp_mask == 12) {
        /* A named scalar/float2 is compact. Projecting its old .y/.zw physical
         * register coordinates would read the wrong logical components. */
        snprintf(assignment, sizeof(assignment), "dxbc_merge_i%d_r0.y", fixture.join_value);
        CHECK(!strstr(source.buf, assignment));
        snprintf(assignment, sizeof(assignment), "dxbc_merge_i%d_r0.z", fixture.join_value);
        CHECK(!strstr(source.buf, assignment));
        snprintf(assignment, sizeof(assignment), "dxbc_merge_i%d_r0.w", fixture.join_value);
        CHECK(!strstr(source.buf, assignment));
    }
    if (vector_literal)
        CHECK(strstr(source.buf, "-0.0f") && strstr(source.buf, "asfloat(0x00000001u)") &&
            strstr(source.buf, "asfloat(0x7FC12345u)"));
    /* Physical TEMP lane coordinates must survive compaction in the map. */
    for (int index = 0; index < fixture.program.instruction_count; ++index) {
        const USILInstruction *instruction = &fixture.program.instructions[index];
        const HLSLExpressionOrigin *origin = &map.origins[index];
        CHECK(origin->instruction_index == index && origin->source_instruction_index ==
            instruction->source_instruction_index);
        if (instruction->opcode == USIL_OP_MOV || instruction->opcode == USIL_OP_ADD ||
            instruction->opcode == USIL_OP_MUL)
            CHECK(origin->destination_lanes == usil_operand_destination_lane_mask(
                &instruction->operands[0]));
        if (instruction->opcode == USIL_OP_IF || instruction->opcode == USIL_OP_ELSE ||
            instruction->opcode == USIL_OP_ENDIF)
            CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_CONTROL && !origin->destination_lanes &&
                origin->source_begin < origin->source_end);
    }
    HLSLExpressionSourceMap changed = map;
    changed.origins[fixture.then_value].destination_lanes ^= 1u;
    CHECK(!hlsl_expression_source_map_matches(&changed, &fixture.program, source.buf));
    changed = map;
    changed.origins[fixture.condition].kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
    CHECK(!hlsl_expression_source_map_matches(&changed, &fixture.program, source.buf));
    changed = map;
    changed.origins[fixture.join_value].source_end = source.len + 1u;
    CHECK(!hlsl_expression_source_map_matches(&changed, &fixture.program, source.buf));
    NaturalIfObservations repeated_observations = {.program = &fixture.program};
    CHECK(natural_if_emit(&fixture.program, &repeated, &repeated_map, &repeated_quality,
        &repeated_observations, &diagnostic));
    CHECK(source.len == repeated.len && !strcmp(source.buf, repeated.buf));
    CHECK(natural_if_maps_equal(&map, &repeated_map));
    CHECK(hlsl_source_quality_results_equal(&quality, &repeated_quality));
    CHECK(observations.observations == repeated_observations.observations &&
        observations.logical_widths == repeated_observations.logical_widths &&
        !repeated_observations.wrong_owner);
    const size_t rejection_points[] = {1, observations.observations / 2u,
        observations.observations};
    for (size_t index = 0; index < sizeof(rejection_points) / sizeof(rejection_points[0]); ++index) {
        sb_free(&repeated);
        sb_init(&repeated);
        repeated_observations = (NaturalIfObservations){.program = &fixture.program,
            .reject_at = rejection_points[index]};
        CHECK(!natural_if_emit(&fixture.program, &repeated, &repeated_map, &repeated_quality,
            &repeated_observations, &diagnostic));
        CHECK(repeated_observations.observations == rejection_points[index] &&
            !repeated_map.complete && !repeated_map.count &&
            repeated_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
        sb_free(&repeated);
        sb_init(&repeated);
        repeated_observations = (NaturalIfObservations){.program = &fixture.program};
        CHECK(natural_if_emit(&fixture.program, &repeated, &repeated_map, &repeated_quality,
            &repeated_observations, &diagnostic));
        CHECK(!strcmp(source.buf, repeated.buf) && natural_if_maps_equal(&map, &repeated_map));
        CHECK(hlsl_source_quality_results_equal(&quality, &repeated_quality));
    }
    sb_free(&repeated);
    sb_free(&source);
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool natural_if_rejected(USILProgram *program, HLSLEmitDiagnostic *failure) {
    StringBuilder source;
    sb_init(&source);
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations observations = {.program = program};
    const bool emitted = natural_if_emit(program, &source, &map, &quality,
        &observations, &diagnostic);
    if (emitted) fprintf(stderr, "Unexpected natural IF admission\n");
    CHECK(!emitted && diagnostic.status != HLSL_EMIT_STATUS_OK);
    CHECK(!map.complete && !map.count && quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    if (failure) *failure = diagnostic;
    sb_free(&source);
    return true;
}

static bool check_natural_if_rejections(void) {
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init(&fixture, 2, 12, true, false, false, false));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 10);
    USILInstruction saved[10];
    memcpy(saved, program->instructions, sizeof(saved));
    const DXBCSignatureElement saved_input = program->inputs[0];
    const DXBCSignatureElement saved_output = program->outputs[0];
    for (unsigned mutation = 0; mutation < 20; ++mutation) {
        /* Every case starts from the same valid parsed grammar. These edits
         * exercise current decoded-model preconditions, not target authority. */
        memcpy(program->instructions, saved, sizeof(saved));
        program->inputs[0] = saved_input;
        program->outputs[0] = saved_output;
        switch (mutation) {
        case 0: /* Different producer widths on the two incoming edges. */
            program->instructions[5].operands[0].destination_mask = 0x40;
            break;
        case 1: /* Same width, different physical masks are not one phi value. */
            program->instructions[5].operands[0].destination_mask = 0x30;
            program->instructions[5].operands[1].swizzle[0] = 2;
            program->instructions[5].operands[1].swizzle[1] = 3;
            break;
        case 2: /* Undefined temporary source. */
            program->instructions[1].operands[1].type = OPERAND_TYPE_TEMP;
            program->instructions[1].operands[1].register_index = 2;
            program->instructions[1].operands[1].index_values[0] = 2;
            break;
        case 3: /* The join combines true-arm lanes from distinct generations. */
            program->instructions[2].operands[0].destination_mask = 0x40;
            break;
        case 4: /* A branch-local output cannot stand in for an unconditional one. */
            program->instructions[2].operands[0].type = OPERAND_TYPE_OUTPUT;
            program->instructions[2].operands[0].destination_mask = 0x30;
            break;
        case 5: /* No explicit false arm is outside this bounded natural route. */
            program->instructions[3] = (USILInstruction){.opcode = USIL_OP_NOP,
                .source_instruction_index = saved[3].source_instruction_index};
            break;
        case 6:
            program->instructions[2].operands[1].has_neg = true;
            break;
        case 7:
            program->instructions[2].operands[1].has_abs = true;
            break;
        case 8:
            program->instructions[2].saturate = true;
            break;
        case 9:
            program->instructions[2].precise_mask = 1;
            break;
        case 10:
            program->instructions[1].operands[1].type = OPERAND_TYPE_RESOURCE;
            break;
        case 11:
            program->instructions[1].operands[1].type = OPERAND_TYPE_CONSTANT_BUFFER;
            break;
        case 12:
            program->instructions[2].opcode = USIL_OP_DERIV_RTX;
            program->instructions[2].operand_count = 2;
            break;
        case 13:
            program->instructions[2].opcode = USIL_OP_DISCARD;
            program->instructions[2].operand_count = 1;
            program->instructions[2].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
            program->instructions[2].operands[0] = program->instructions[0].operands[0];
            break;
        case 14: /* Output does not cover the complete actual signature. */
            program->instructions[8].operands[0].destination_mask = 0x10;
            break;
        case 15:
            program->inputs[0].component_type = 2;
            break;
        case 16:
            program->inputs[0].mask = 1;
            program->inputs[0].rw_mask = 1;
            break;
        case 17:
            program->outputs[0].mask = 7;
            program->outputs[0].rw_mask = 8;
            break;
        case 18:
            program->instructions[0].operands[0].has_neg = true;
            break;
        case 19: /* An undeclared physical TEMP lane has no logical component. */
            program->instructions[7].operands[1].swizzle[0] = 1;
            break;
        }
        CHECK(natural_if_rejected(program, NULL));
    }
    memcpy(program->instructions, saved, sizeof(saved));
    program->inputs[0] = saved_input;
    program->outputs[0] = saved_output;
    /* Well-formed nested IF has an explicit ELSE at both levels; its rejection
     * tests the bounded scope rather than an unbalanced control-flow stream. */
    USILInstruction nested[16] = {0};
    nested[0] = saved[0];
    nested[1] = saved[0];
    nested[2] = saved[1]; nested[3] = saved[2];
    nested[4] = saved[3]; nested[5] = saved[4]; nested[6] = saved[5];
    nested[7] = saved[6];
    nested[8] = saved[3]; nested[9] = saved[4]; nested[10] = saved[5];
    nested[11] = saved[6]; nested[12] = saved[7];
    nested[13] = saved[8]; nested[14] = saved[9];
    for (int index = 0; index < 15; ++index)
        nested[index].source_instruction_index = (uint32_t)index + 5u;
    USILProgram extended = *program;
    extended.instructions = nested;
    extended.instruction_count = 15;
    extended.instruction_alloc = 16;
    CHECK(natural_if_rejected(&extended, NULL));
    /* A balanced loop containing the complete IF is also outside the route. */
    nested[0] = (USILInstruction){.opcode = USIL_OP_LOOP};
    memcpy(nested + 1, saved, 9u * sizeof(*saved));
    nested[10] = (USILInstruction){.opcode = USIL_OP_ENDLOOP};
    nested[11] = saved[9];
    for (int index = 0; index < 12; ++index)
        nested[index].source_instruction_index = (uint32_t)index + 5u;
    extended.instruction_count = 12;
    CHECK(natural_if_rejected(&extended, NULL));
    /* An extra output write after the first would silently replace its value. */
    memcpy(nested, saved, 9u * sizeof(*saved));
    nested[9] = saved[8]; nested[10] = saved[9];
    nested[9].source_instruction_index = 14;
    nested[10].source_instruction_index = 15;
    extended.instruction_count = 11;
    CHECK(natural_if_rejected(&extended, NULL));
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_if_optional_outputs(void) {
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init(&fixture, 2, 12, true, false, false, false));
    StringBuilder original, selected;
    sb_init(&original);
    sb_init(&selected);
    HLSLExpressionSourceMap original_map, selected_map;
    HLSLSourceQualityResult original_quality, selected_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations observations = {.program = &fixture.program};
    CHECK(natural_if_emit(&fixture.program, &original, &original_map, &original_quality,
        &observations, &diagnostic));
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = (outputs & 1u) ? &selected_map : NULL;
        options.source_quality = (outputs & 2u) ? &selected_quality : NULL;
        options.source_quality_pass_index = 3;
        options.source_quality_entry_point_index = 4;
        sb_free(&selected);
        sb_init(&selected);
        CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &selected, NULL, NULL, NULL,
            &options, &diagnostic));
        CHECK(original.len == selected.len && !strcmp(original.buf, selected.buf));
        if (outputs & 1u) {
            CHECK(hlsl_expression_source_map_matches(&selected_map, &fixture.program, selected.buf));
            CHECK(natural_if_maps_equal(&original_map, &selected_map));
        }
        if (outputs & 2u)
            CHECK(hlsl_source_quality_results_equal(&original_quality, &selected_quality));
    }
    sb_free(&selected);
    sb_free(&original);
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_if_callback_drift(void) {
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init(&fixture, 2, 12, true, false, false, false));
    USILProgram *program = &fixture.program;
    USILInstruction saved[10];
    memcpy(saved, program->instructions, sizeof(saved));
    const DXBCSignatureElement saved_input = program->inputs[0];
    StringBuilder original, changed;
    sb_init(&original);
    sb_init(&changed);
    HLSLExpressionSourceMap original_map, changed_map;
    HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations baseline = {.program = program};
    CHECK(natural_if_emit(program, &original, &original_map, &original_quality,
        &baseline, &diagnostic));
    CHECK(baseline.observations > 2);
    const size_t mutation_points[] = {1, baseline.observations / 2u, baseline.observations};
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        /* Source/map corruption waits until the selected actual ADD span has
         * been emitted; model drift additionally covers early callbacks. */
        const size_t first_point = mutation >= 4 ? 2u : 0u;
        for (size_t point = first_point; point < sizeof(mutation_points) / sizeof(mutation_points[0]); ++point) {
            NaturalIfObservations drift = {.program = program, .mutable_program = program,
                .mutation = mutation, .mutate_at = mutation_points[point],
                .mutable_source = &changed, .mutable_map = &changed_map,
                .source_offset = original_map.origins[2].source_begin};
            sb_free(&changed);
            sb_init(&changed);
            CHECK(!natural_if_emit(program, &changed, &changed_map, &changed_quality,
                &drift, &diagnostic));
            CHECK(drift.mutated && diagnostic.status != HLSL_EMIT_STATUS_OK &&
                !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
            /* Model edits are admission-valid in a fresh emission. This
             * separates frozen-owner rejection from unsupported shapes.
             * External source/map edits leave the original model unchanged. */
            NaturalIfObservations fresh = {.program = program};
            sb_free(&changed);
            sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality,
                &fresh, &diagnostic));
            CHECK(changed_map.complete && hlsl_expression_source_map_matches(
                &changed_map, program, changed.buf));
            if (mutation < 4) CHECK(strcmp(original.buf, changed.buf));
            else CHECK(!strcmp(original.buf, changed.buf));
            memcpy(program->instructions, saved, sizeof(saved));
            program->inputs[0] = saved_input;
            fresh = (NaturalIfObservations){.program = program};
            sb_free(&changed);
            sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality,
                &fresh, &diagnostic));
            CHECK(original.len == changed.len && !strcmp(original.buf, changed.buf));
            CHECK(natural_if_maps_equal(&original_map, &changed_map));
            CHECK(hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    sb_free(&changed);
    sb_free(&original);
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool natural_if_multiple_outputs_fixture_mode(NaturalIfFixture *fixture, bool vertex,
    USILOpcode comparison, uint8_t predicate_mask, bool nonzero) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    const bool compared = comparison != USIL_OP_NOP;
    CHECK(!compared || (predicate_mask && !(predicate_mask & (predicate_mask - 1u)) &&
        predicate_mask <= 8 && natural_comparison_raw_opcode(comparison) != UINT32_MAX));
    uint32_t words[96];
    size_t count = 0;
#define MULTI_WORD(value) do { \
    CHECK(count < sizeof(words) / sizeof(words[0])); \
    words[count++] = (value); \
} while (0)
#define MULTI_INST(opcode, length) ((uint32_t)(opcode) | (uint32_t)(length) << 24u)
    MULTI_WORD(MULTI_INST(106, 1) | 1u << 11u);
    const uint8_t masks[3] = {15, 7, 1};
    for (unsigned input = 0; input < 3; ++input) {
        MULTI_WORD(MULTI_INST(vertex ? 95 : 98, 3) | (vertex ? 0 : 2u << 11u));
        MULTI_WORD(UINT32_C(0x00101002) | (uint32_t)masks[input] << 4u);
        MULTI_WORD(input);
    }
    MULTI_WORD(MULTI_INST(vertex ? 103 : 101, vertex ? 4 : 3));
    MULTI_WORD(UINT32_C(0x001020f2)); MULTI_WORD(0);
    if (vertex) MULTI_WORD(1);
    MULTI_WORD(MULTI_INST(101, 3));
    MULTI_WORD(UINT32_C(0x00102072)); MULTI_WORD(1);
    MULTI_WORD(MULTI_INST(104, 2)); MULTI_WORD(compared ? 2 : 1);
    unsigned predicate_lane = 0;
    if (compared) {
        while (!(predicate_mask & (1u << predicate_lane))) ++predicate_lane;
        MULTI_WORD(MULTI_INST(natural_comparison_raw_opcode(comparison), 7));
        MULTI_WORD(UINT32_C(0x00100002) | (uint32_t)predicate_mask << 4u); MULTI_WORD(1);
        MULTI_WORD(UINT32_C(0x0010100a)); MULTI_WORD(2);
        MULTI_WORD(UINT32_C(0x00004001)); MULTI_WORD(UINT32_C(0x3ec00000));
    }
    MULTI_WORD(MULTI_INST(31, 3) | (nonzero ? UINT32_C(0x40000) : 0));
    MULTI_WORD(compared ? UINT32_C(0x0010000a) | (uint32_t)predicate_lane << 4u
        : UINT32_C(0x0010100a)); MULTI_WORD(compared ? 1 : 2);
    MULTI_WORD(MULTI_INST(0, 7));
    MULTI_WORD(UINT32_C(0x00100072)); MULTI_WORD(0);
    MULTI_WORD(UINT32_C(0x00101246)); MULTI_WORD(1);
    MULTI_WORD(UINT32_C(0x00004001)); MULTI_WORD(UINT32_C(0x3f800000));
    MULTI_WORD(MULTI_INST(18, 1));
    MULTI_WORD(MULTI_INST(0, 7));
    MULTI_WORD(UINT32_C(0x00100072)); MULTI_WORD(0);
    MULTI_WORD(UINT32_C(0x00101246)); MULTI_WORD(1);
    MULTI_WORD(UINT32_C(0x00004001)); MULTI_WORD(UINT32_C(0x40400000));
    MULTI_WORD(MULTI_INST(21, 1));
    MULTI_WORD(MULTI_INST(56, 7));
    MULTI_WORD(UINT32_C(0x00102072)); MULTI_WORD(1);
    MULTI_WORD(UINT32_C(0x00100246)); MULTI_WORD(0);
    MULTI_WORD(UINT32_C(0x00004001)); MULTI_WORD(UINT32_C(0x3fa00000));
    MULTI_WORD(MULTI_INST(54, 5));
    MULTI_WORD(UINT32_C(0x001020f2)); MULTI_WORD(0);
    MULTI_WORD(UINT32_C(0x00101e46)); MULTI_WORD(0);
    MULTI_WORD(MULTI_INST(62, 1));
#undef MULTI_INST
#undef MULTI_WORD
    DXBCSignatureElement inputs[3] = {
        {.semantic_name = "POSITION", .component_type = 3, .mask = 15, .rw_mask = 15},
        {.semantic_name = "NORMAL", .register_id = 1, .component_type = 3, .mask = 7, .rw_mask = 7},
        {.semantic_name = "TEXCOORD", .register_id = 2, .component_type = 3, .mask = 1, .rw_mask = 1}};
    DXBCSignatureElement outputs[2] = {
        {.semantic_name = "SV_POSITION", .system_value = 1, .component_type = 3, .mask = 15},
        {.semantic_name = "TEXCOORD", .register_id = 1, .component_type = 3, .mask = 7, .rw_mask = 8}};
    if (!vertex) {
        memset(inputs[0].semantic_name, 0, sizeof(inputs[0].semantic_name));
        memcpy(inputs[0].semantic_name, "TEXCOORD", sizeof("TEXCOORD"));
        inputs[2].semantic_index = 1;
        for (unsigned output = 0; output < 2; ++output) {
            memset(outputs[output].semantic_name, 0, sizeof(outputs[output].semantic_name));
            memcpy(outputs[output].semantic_name, "SV_Target", sizeof("SV_Target"));
            outputs[output].semantic_index = output;
            outputs[output].system_value = 64;
        }
    }
    CHECK(natural_if_fixture_decode_words(fixture,
        vertex ? UINT32_C(0x00010050) : UINT32_C(0x00000050), words, count, inputs, 3, outputs, 2));
    CHECK(fixture->program.program_type == (vertex ? DXBC_PROGRAM_TYPE_VERTEX : DXBC_PROGRAM_TYPE_PIXEL));
    CHECK(fixture->program.input_count == 3 && fixture->program.output_count == 2 &&
        fixture->program.instruction_count == (compared ? 9 : 8) &&
        fixture->program.signature_declaration_count == 5);
    for (unsigned input = 0; input < 3; ++input)
        CHECK(fixture->program.inputs[input].mask == masks[input] &&
            fixture->program.inputs[input].rw_mask == masks[input]);
    CHECK(fixture->program.outputs[0].mask == 15 && !fixture->program.outputs[0].rw_mask &&
        fixture->program.outputs[1].mask == 7 && fixture->program.outputs[1].rw_mask == 8);
    const int prefix = compared ? 1 : 0;
    fixture->condition = prefix;
    fixture->then_value = 1 + prefix;
    fixture->else_value = 3 + prefix;
    fixture->join_value = 5 + prefix;
    fixture->output = 5 + prefix;
    CHECK(fixture->program.instructions[0].source_instruction_index == 7);
    return true;
}

static bool natural_if_multiple_outputs_fixture(NaturalIfFixture *fixture, bool vertex) {
    return natural_if_multiple_outputs_fixture_mode(fixture, vertex, USIL_OP_NOP, 0, true);
}

static bool check_natural_if_multiple_outputs(bool vertex) {
    NaturalIfFixture fixture;
    CHECK(natural_if_multiple_outputs_fixture(&fixture, vertex));
    USILProgram *program = &fixture.program;
    StringBuilder source, selected;
    sb_init(&source);
    sb_init(&selected);
    HLSLExpressionSourceMap map, selected_map;
    HLSLSourceQualityResult quality, selected_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations observations = {.program = program};
    const bool emitted = natural_if_emit(program, &source, &map, &quality,
        &observations, &diagnostic);
    if (!emitted)
        fprintf(stderr, "Natural IF %s two-output emission: %s at instruction %d, phase %d\n",
            vertex ? "vertex" : "pixel", hlsl_emit_reason_name(diagnostic.reason),
            diagnostic.instruction_index, (int)diagnostic.phase);
    CHECK(emitted && map.complete && map.count == 8);
    CHECK(quality.stage == program->program_type && quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
        !quality.reasons && quality.counts.inspected_units == 1 && !quality.counts.incomplete_units &&
        !quality.counts.residual_total && !quality.counts.unknown_provenance && quality.counts.real_bitcasts);
    CHECK(observations.observations && !observations.wrong_owner &&
        (observations.logical_widths & (1u << 3)) && (observations.logical_widths & (1u << 4)));
    /* Struct begin/two fields/end, three parameters, result local and entry
     * signature are nine declaration records; RET owns the one return record. */
    CHECK(observations.declaration_events == 9 && observations.return_events == 1);
    CHECK(strstr(source.buf, "float3 dxbc_merge_i5_r0;") && strstr(source.buf, "return output;"));
    CHECK(strstr(source.buf, vertex ? "float4 clipPosition : SV_POSITION;" : "float4 color : SV_Target;") &&
        strstr(source.buf, vertex ? "float3 texcoord0_1 : TEXCOORD0;" : "float3 color1 : SV_Target1;"));
    CHECK(!strstr(source.buf, "u_xlat") && !strstr(source.buf, "float4 r0"));
    CHECK(hlsl_expression_source_map_matches(&map, program, source.buf));
    CHECK(map.origins[5].destination_lanes == 7 && map.origins[6].destination_lanes == 15 &&
        map.origins[7].kind == HLSL_EXPRESSION_ORIGIN_RETURN && !map.origins[7].destination_lanes);
    CHECK(map.origins[7].source_begin < map.origins[7].source_end &&
        map.origins[7].source_end == source.len);
    for (size_t index = 0; index < map.count; ++index)
        CHECK(map.origins[index].instruction_index == (int)index &&
            map.origins[index].source_instruction_index == (uint32_t)index + 7u);
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = (outputs & 1u) ? &selected_map : NULL;
        options.source_quality = (outputs & 2u) ? &selected_quality : NULL;
        options.source_quality_pass_index = 3;
        options.source_quality_entry_point_index = 4;
        sb_free(&selected);
        sb_init(&selected);
        CHECK(hlsl_emit_with_options_diagnostic(program, &selected, NULL, NULL, NULL,
            &options, &diagnostic));
        CHECK(source.len == selected.len && !strcmp(source.buf, selected.buf));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&map, &selected_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &selected_quality));
    }
    USILInstruction saved[8];
    memcpy(saved, program->instructions, sizeof(saved));
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        memcpy(program->instructions, saved, sizeof(saved));
        if (mutation == 0) { /* Second output is written only in the true arm. */
            program->instructions[1] = saved[5];
            program->instructions[1].operands[1] = saved[1].operands[1];
            program->instructions[1].source_instruction_index = saved[1].source_instruction_index;
            program->instructions[5] = (USILInstruction){.opcode = USIL_OP_NOP,
                .source_instruction_index = saved[5].source_instruction_index};
        } else if (mutation == 1) {
            program->instructions[5].operands[0].destination_mask = 0x30;
        } else if (mutation == 2) { /* Both writes target output1; output0 is missing. */
            program->instructions[6].operands[0] = saved[5].operands[0];
        } else {
            program->instructions[5] = (USILInstruction){.opcode = USIL_OP_NOP,
                .source_instruction_index = saved[5].source_instruction_index};
        }
        CHECK(natural_if_rejected(program, &diagnostic));
        if (!vertex && mutation == 1)
            CHECK(diagnostic.status == HLSL_EMIT_STATUS_UNSUPPORTED &&
                diagnostic.phase == HLSL_EMIT_PHASE_INTERFACE_EMISSION &&
                diagnostic.reason == HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    }
    memcpy(program->instructions, saved, sizeof(saved));
    sb_free(&selected);
    sb_init(&selected);
    NaturalIfObservations restored = {.program = program};
    CHECK(natural_if_emit(program, &selected, &selected_map, &selected_quality, &restored, &diagnostic));
    CHECK(!strcmp(source.buf, selected.buf) && natural_if_maps_equal(&map, &selected_map) &&
        hlsl_source_quality_results_equal(&quality, &selected_quality));
    sb_free(&selected);
    sb_free(&source);
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_comparison_fixture(NaturalIfFixture *fixture,
    USILOpcode comparison, uint8_t predicate_mask, bool nonzero) {
    USILProgram *program = &fixture->program;
    CHECK(program->has_parsed_signature_authority && fixture->condition == 1 &&
        program->instructions[0].opcode == comparison &&
        usil_operand_destination_lane_mask(&program->instructions[0].operands[0]) == predicate_mask);
    CHECK(program->instructions[1].opcode == USIL_OP_IF &&
        program->instructions[1].condition_test == (nonzero ?
            DXBC_INSTRUCTION_TEST_NONZERO : DXBC_INSTRUCTION_TEST_ZERO));
    unsigned predicate_lane = 0;
    while (!(predicate_mask & (1u << predicate_lane))) ++predicate_lane;
    CHECK(usil_operand_source_component(&program->instructions[1].operands[0], 0) ==
        (int)predicate_lane);
    StringBuilder original, selected;
    sb_init(&original);
    sb_init(&selected);
    HLSLExpressionSourceMap original_map, selected_map;
    HLSLSourceQualityResult original_quality, selected_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations observations = {.program = program};
    const bool emitted = natural_if_emit(program, &original, &original_map, &original_quality,
        &observations, &diagnostic);
    if (!emitted)
        fprintf(stderr, "Scalar comparison %d/mask %u/%s, stage %d: %s at %d, phase %d\n",
            (int)comparison, predicate_mask, nonzero ? "NZ" : "Z", (int)program->program_type,
            hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index, (int)diagnostic.phase);
    CHECK(emitted && original_map.complete && original_map.count == (size_t)program->instruction_count);
    CHECK(original_quality.stage == program->program_type &&
        original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !original_quality.reasons &&
        original_quality.counts.inspected_units == 1 && !original_quality.counts.incomplete_units &&
        !original_quality.counts.residual_total && !original_quality.counts.unknown_provenance);
    /* AST comparison nodes are accepted only with a complete scalar BOOL
     * origin; its real instruction/raw ordinal and physical lane stay visible. */
    CHECK(observations.comparison_events && !observations.wrong_owner);
    CHECK(strstr(original.buf, "const bool dxbc_value_i0 = ") &&
        !strstr(original.buf, "asuint(") && !strstr(original.buf, "0xffffffff") &&
        !strstr(original.buf, "float4 r") && !strstr(original.buf, "u_xlat"));
    const char *operator_text = comparison == USIL_OP_LT ? " < " :
        comparison == USIL_OP_GE ? " >= " : comparison == USIL_OP_EQ ? " == " : " != ";
    CHECK(strstr(original.buf, operator_text));
    const HLSLExpressionOrigin *condition = &original_map.origins[fixture->condition];
    CHECK(original_map.origins[0].kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION &&
        original_map.origins[0].destination_lanes == predicate_mask &&
        condition->kind == HLSL_EXPRESSION_ORIGIN_CONTROL && !condition->destination_lanes &&
        condition->source_begin < condition->source_end && condition->source_end <= original.len);
    const char *predicate = strstr(original.buf + condition->source_begin, "dxbc_value_i0");
    CHECK(predicate && predicate < original.buf + condition->source_end);
    const char *negation = memchr(original.buf + condition->source_begin, '!',
        condition->source_end - condition->source_begin);
    CHECK((negation != NULL) == !nonzero);
    CHECK(hlsl_expression_source_map_matches(&original_map, program, original.buf));
    const uint32_t first_raw = program->instructions[0].source_instruction_index;
    for (size_t index = 0; index < original_map.count; ++index)
        CHECK(original_map.origins[index].instruction_index == (int)index &&
            original_map.origins[index].source_instruction_index == first_raw + (uint32_t)index);
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = (outputs & 1u) ? &selected_map : NULL;
        options.source_quality = (outputs & 2u) ? &selected_quality : NULL;
        options.source_quality_pass_index = 3;
        options.source_quality_entry_point_index = 4;
        sb_free(&selected);
        sb_init(&selected);
        CHECK(hlsl_emit_with_options_diagnostic(program, &selected, NULL, NULL, NULL,
            &options, &diagnostic));
        CHECK(original.len == selected.len && !strcmp(original.buf, selected.buf));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&original_map, &selected_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&original_quality, &selected_quality));
    }
    /* First and final observer vetoes cannot leave a sealed map or clean result. */
    const size_t rejection_points[] = {1, observations.observations};
    for (size_t point = 0; point < sizeof(rejection_points) / sizeof(rejection_points[0]); ++point) {
        NaturalIfObservations rejected = {.program = program, .reject_at = rejection_points[point]};
        sb_free(&selected);
        sb_init(&selected);
        CHECK(!natural_if_emit(program, &selected, &selected_map, &selected_quality, &rejected, &diagnostic));
        CHECK(!selected_map.complete && !selected_map.count &&
            selected_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    }
    NaturalIfObservations restored = {.program = program};
    sb_free(&selected);
    sb_init(&selected);
    CHECK(natural_if_emit(program, &selected, &selected_map, &selected_quality, &restored, &diagnostic));
    CHECK(original.len == selected.len && !strcmp(original.buf, selected.buf) &&
        natural_if_maps_equal(&original_map, &selected_map) &&
        hlsl_source_quality_results_equal(&original_quality, &selected_quality));
    sb_free(&selected);
    sb_free(&original);
    return true;
}

static bool check_natural_comparison_callback_drift(void) {
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init_mode(&fixture, 2, 12, true, false, false, false,
        USIL_OP_LT, 2, UINT32_C(0x3ec00000)));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 11);
    USILInstruction saved[11];
    memcpy(saved, program->instructions, sizeof(saved));
    StringBuilder original, changed;
    sb_init(&original);
    sb_init(&changed);
    HLSLExpressionSourceMap original_map, changed_map;
    HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations baseline = {.program = program};
    CHECK(natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic));
    CHECK(baseline.observations > 2);
    const size_t mutation_points[] = {1, baseline.observations / 2u, baseline.observations};
    for (unsigned mutation = 6; mutation <= 9; ++mutation) {
        for (size_t point = 0; point < sizeof(mutation_points) / sizeof(mutation_points[0]); ++point) {
            NaturalIfObservations drift = {.program = program, .mutable_program = program,
                .mutation = mutation, .mutate_at = mutation_points[point]};
            sb_free(&changed);
            sb_init(&changed);
            CHECK(!natural_if_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic));
            CHECK(drift.mutated && diagnostic.status != HLSL_EMIT_STATUS_OK &&
                !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
            /* Each changed comparator, literal, scalar field or IF polarity
             * remains admitted when it is the starting model, not a callback edit. */
            NaturalIfObservations fresh = {.program = program};
            sb_free(&changed);
            sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic));
            CHECK(strcmp(original.buf, changed.buf) && changed_map.complete &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                hlsl_expression_source_map_matches(&changed_map, program, changed.buf));
            memcpy(program->instructions, saved, sizeof(saved));
            fresh = (NaturalIfObservations){.program = program};
            sb_free(&changed);
            sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic));
            CHECK(original.len == changed.len && !strcmp(original.buf, changed.buf) &&
                natural_if_maps_equal(&original_map, &changed_map) &&
                hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    sb_free(&changed);
    sb_free(&original);
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_comparison_rejections(void) {
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init_mode(&fixture, 2, 12, true, false, false, false,
        USIL_OP_LT, 2, UINT32_C(0x3ec00000)));
    USILProgram *program = &fixture.program;
    USILInstruction saved[11];
    CHECK(program->instruction_count == 11);
    memcpy(saved, program->instructions, sizeof(saved));
    for (unsigned mutation = 0; mutation < 16; ++mutation) {
        memcpy(program->instructions, saved, sizeof(saved));
        switch (mutation) {
        case 0: program->instructions[0].operands[0].destination_mask = 0x30; break;
        case 1: /* Undefined scalar predicate input. */
            program->instructions[0].operands[1] = saved[1].operands[0]; break;
        case 2: program->instructions[0].operands[1].type = OPERAND_TYPE_RESOURCE; break;
        case 3: program->instructions[0].operands[1].has_abs = true; break;
        case 4: program->instructions[0].operands[1].has_neg = true; break;
        case 5: program->instructions[0].precise_mask = 2; break;
        case 6: program->instructions[0].saturate = true; break;
        case 7: program->instructions[0].opcode = USIL_OP_IGE; break;
        case 8: program->instructions[1].operands[0].swizzle[0] = 3; break;
        case 9: /* The raw comparison mask cannot participate in float arithmetic. */
            program->instructions[fixture.join_value].operands[2] = saved[1].operands[0]; break;
        case 10: program->instructions[0].operands[1].min_precision = 1; break;
        case 11: program->instructions[0].operands[1].type = OPERAND_TYPE_CONSTANT_BUFFER; break;
        case 12: program->instructions[0].opcode = USIL_OP_DERIV_RTX; program->instructions[0].operand_count = 2; break;
        case 13: program->instructions[fixture.output].operands[0].destination_mask = 0x10; break;
        case 14: program->instructions[0].operands[0].register_index = 3;
            program->instructions[0].operands[0].index_values[0] = 3; break;
        case 15: program->instructions[1].operands[0].has_neg = true; break;
        }
        CHECK(natural_if_rejected(program, NULL));
    }
    memcpy(program->instructions, saved, sizeof(saved));
    USILInstruction extended[15];
    USILProgram changed = *program;
    changed.instructions = extended;
    changed.instruction_alloc = (int)(sizeof(extended) / sizeof(extended[0]));
    changed.temp_count = 4;
    /* An otherwise valid MOV transport is not an owned direct predicate. */
    memcpy(extended, saved, sizeof(saved[0]));
    extended[1] = (USILInstruction){.opcode = USIL_OP_MOV, .operand_count = 2,
        .source_instruction_index = 6};
    extended[1].operands[0] = saved[0].operands[0];
    extended[1].operands[0].register_index = 3;
    extended[1].operands[0].index_values[0] = 3;
    extended[1].operands[1] = saved[1].operands[0];
    memcpy(extended + 2, saved + 1, sizeof(saved) - sizeof(saved[0]));
    extended[2].operands[0].register_index = 3;
    extended[2].operands[0].index_values[0] = 3;
    changed.instruction_count = 12;
    for (int index = 0; index < changed.instruction_count; ++index)
        extended[index].source_instruction_index = saved[0].source_instruction_index + (uint32_t)index;
    CHECK(natural_if_rejected(&changed, NULL));
    /* Even a dead phi is an extra use of the raw mask. Keeping the original
     * direct IF consumer does not permit a branch overwrite at a later join. */
    memcpy(extended, saved, sizeof(saved));
    memmove(extended + 4, extended + 3, (11u - 3u) * sizeof(extended[0]));
    extended[3] = (USILInstruction){.opcode = USIL_OP_MOV, .operand_count = 2,
        .source_instruction_index = 8};
    extended[3].operands[0] = saved[0].operands[0];
    extended[3].operands[1] = saved[0].operands[2];
    changed.instruction_count = 12;
    for (int index = 0; index < changed.instruction_count; ++index)
        extended[index].source_instruction_index = saved[0].source_instruction_index + (uint32_t)index;
    CHECK(natural_if_rejected(&changed, NULL));
    /* Nesting retains two balanced arms and is still outside this one-IF proof. */
    memcpy(extended, saved, 2u * sizeof(extended[0]));
    extended[2] = saved[1];
    memcpy(extended + 3, saved + 2, 2u * sizeof(extended[0]));
    extended[5] = saved[4];
    extended[6] = saved[7];
    memcpy(extended + 7, saved + 4, 7u * sizeof(extended[0]));
    changed.instruction_count = 14;
    for (int index = 0; index < changed.instruction_count; ++index)
        extended[index].source_instruction_index = saved[0].source_instruction_index + (uint32_t)index;
    CHECK(natural_if_rejected(&changed, NULL));
    NaturalIfObservations restored = {.program = program};
    StringBuilder source;
    sb_init(&source);
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    HLSLEmitDiagnostic diagnostic;
    CHECK(natural_if_emit(program, &source, &map, &quality, &restored, &diagnostic));
    CHECK(map.complete && quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
        hlsl_expression_source_map_matches(&map, program, source.buf));
    sb_free(&source);
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_comparison_emission(void) {
    const USILOpcode comparisons[] = {USIL_OP_LT, USIL_OP_GE, USIL_OP_EQ, USIL_OP_NE};
    const struct {unsigned width; uint8_t mask;} shapes[] = {{1, 2}, {2, 12}, {3, 7}};
    for (size_t opcode = 0; opcode < sizeof(comparisons) / sizeof(comparisons[0]); ++opcode) {
        for (size_t shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); ++shape) {
            for (unsigned predicate = 0; predicate < 2; ++predicate) {
                for (unsigned nonzero = 0; nonzero < 2; ++nonzero) {
                    NaturalIfFixture fixture;
                    CHECK(natural_if_fixture_init_mode(&fixture, shapes[shape].width, shapes[shape].mask,
                        nonzero != 0, false, false, false, comparisons[opcode], predicate ? 8 : 2,
                        UINT32_C(0x3ec00000)));
                    CHECK(check_natural_comparison_fixture(&fixture, comparisons[opcode],
                        predicate ? 8 : 2, nonzero != 0));
                    natural_if_fixture_dispose(&fixture);
                }
            }
        }
        for (unsigned vertex = 0; vertex < 2; ++vertex) {
            NaturalIfFixture fixture;
            CHECK(natural_if_multiple_outputs_fixture_mode(&fixture, vertex != 0,
                comparisons[opcode], vertex ? 8 : 2, vertex != 0));
            CHECK(check_natural_comparison_fixture(&fixture, comparisons[opcode],
                vertex ? 8 : 2, vertex != 0));
            natural_if_fixture_dispose(&fixture);
        }
        const uint32_t exceptional_bits[] = {UINT32_C(0x80000000), 1, UINT32_C(0x7fc12345)};
        for (size_t bits = 0; bits < sizeof(exceptional_bits) / sizeof(exceptional_bits[0]); ++bits) {
            for (unsigned nonzero = 0; nonzero < 2; ++nonzero) {
                NaturalIfFixture fixture;
                CHECK(natural_if_fixture_init_mode(&fixture, 1, 2, nonzero != 0,
                    false, false, false, comparisons[opcode], 8, exceptional_bits[bits]));
                CHECK(fixture.program.instructions[0].operands[2].imm_values[0] == exceptional_bits[bits] &&
                    fixture.program.instructions[0].operands[2].immediate_words[0] == exceptional_bits[bits]);
                CHECK(check_natural_comparison_fixture(&fixture, comparisons[opcode], 8, nonzero != 0));
                natural_if_fixture_dispose(&fixture);
            }
        }
    }
    /* NaN breaks relational complementation: IF_Z must negate the original
     * comparison, rather than spell LT as GE or GE as LT. */
    const uint32_t nan_bits = UINT32_C(0x7fc12345), negative_zero_bits = UINT32_C(0x80000000);
    float nan_value, negative_zero;
    memcpy(&nan_value, &nan_bits, sizeof(nan_value));
    memcpy(&negative_zero, &negative_zero_bits, sizeof(negative_zero));
    CHECK(!(nan_value < 1.0f) && !(nan_value >= 1.0f) &&
        nan_value != 1.0f && !(nan_value == 1.0f));
    CHECK(negative_zero == 0.0f && !(negative_zero < 0.0f) && negative_zero >= 0.0f);
    CHECK(check_natural_comparison_rejections());
    CHECK(check_natural_comparison_callback_drift());
    return true;
}

static bool check_natural_arithmetic_fixture(NaturalIfFixture *fixture, unsigned width,
    uint8_t temp_mask, bool nonzero, bool compared, const NaturalIfArithmetic *arithmetic) {
    USILProgram *program = &fixture->program;
    CHECK(program->has_parsed_signature_authority && usil_signature_authority_is_valid(program) &&
        program->program_type == DXBC_PROGRAM_TYPE_PIXEL);
    const int owners[] = {fixture->then_value, fixture->else_value, fixture->join_value};
    const USILOpcode opcodes[] = {arithmetic->then_opcode, arithmetic->else_opcode, arithmetic->join_opcode};
    const uint8_t output_mask = (uint8_t)((1u << width) - 1u);
    CHECK(program->instructions[fixture->condition].condition_test == (nonzero ?
        DXBC_INSTRUCTION_TEST_NONZERO : DXBC_INSTRUCTION_TEST_ZERO));
    StringBuilder original, selected;
    sb_init(&original); sb_init(&selected);
    HLSLExpressionSourceMap original_map, selected_map;
    HLSLSourceQualityResult original_quality, selected_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations observations = {.program = program};
    const bool emitted = natural_if_emit(program, &original, &original_map, &original_quality, &observations, &diagnostic);
    if (!emitted) fprintf(stderr, "Natural arithmetic opcode=%d width=%u mask=%u compared=%d: status=%s reason=%s instruction=%d\n",
        (int)arithmetic->then_opcode, width, temp_mask, compared, hlsl_emit_status_name(diagnostic.status),
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted && original_map.complete && original_map.count == (size_t)program->instruction_count &&
        hlsl_expression_source_map_matches(&original_map, program, original.buf));
    CHECK(original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !original_quality.reasons &&
        original_quality.counts.inspected_units == 1 && !original_quality.counts.incomplete_units &&
        !original_quality.counts.residual_total && !original_quality.counts.unknown_provenance &&
        observations.observations && !observations.wrong_owner);
    CHECK((observations.comparison_events != 0) == compared);
    if (compared) CHECK(strstr(original.buf, "const bool dxbc_value_i0 = ") && !strstr(original.buf, "asuint("));
    else CHECK(strstr(original.buf, nonzero ? "[branch] if (asuint(" : "[branch] if (!asuint("));
    const HLSLExpressionOrigin *condition = &original_map.origins[fixture->condition];
    CHECK(condition->kind == HLSL_EXPRESSION_ORIGIN_CONTROL && !condition->destination_lanes &&
        (memchr(original.buf + condition->source_begin, '!', condition->source_end - condition->source_begin) != NULL) == !nonzero);
    char declaration[96];
    if (width == 1) snprintf(declaration, sizeof(declaration), "float dxbc_merge_i%d_r0;", fixture->join_value);
    else snprintf(declaration, sizeof(declaration), "float%u dxbc_merge_i%d_r0;", width, fixture->join_value);
    CHECK(strstr(original.buf, declaration) && !strstr(original.buf, "float4 dxbc_merge") && !strstr(original.buf, "float4 dxbc_value"));
    for (unsigned owner = 0; owner < 3; ++owner) {
        const int index = owners[owner];
        const HLSLExpressionOrigin *origin = &original_map.origins[index];
        CHECK(program->instructions[index].opcode == opcodes[owner] && origin->instruction_index == index &&
            origin->source_instruction_index == program->instructions[index].source_instruction_index &&
            origin->destination_lanes == (owner == 2 ? output_mask : temp_mask) &&
            origin->source_begin < origin->source_end && origin->source_end <= original.len &&
            (observations.arithmetic_owners & (UINT64_C(1) << (unsigned)index)));
        const char *operation = strstr(original.buf + origin->source_begin,
            opcodes[owner] == USIL_OP_MIN ? "min(" : opcodes[owner] == USIL_OP_MAX ? "max(" : " / ");
        CHECK(operation && operation < original.buf + origin->source_end);
        if (width == 1) snprintf(declaration, sizeof(declaration), "const float dxbc_value_i%d = ", index);
        else snprintf(declaration, sizeof(declaration), "const float%u dxbc_value_i%d = ", width, index);
        CHECK(strstr(original.buf, declaration));
    }
    char physical_suffix[64];
    snprintf(physical_suffix, sizeof(physical_suffix), "dxbc_merge_i%d_r0.", fixture->join_value);
    CHECK(!strstr(original.buf, physical_suffix));
    for (size_t index = 0; index < original_map.count; ++index)
        CHECK(original_map.origins[index].instruction_index == (int)index &&
            original_map.origins[index].source_instruction_index == program->instructions[index].source_instruction_index);
    HLSLExpressionSourceMap corrupt = original_map;
    corrupt.origins[fixture->then_value].destination_lanes ^= 1u;
    CHECK(!hlsl_expression_source_map_matches(&corrupt, program, original.buf));
    corrupt = original_map; ++corrupt.origins[fixture->join_value].source_instruction_index;
    CHECK(!hlsl_expression_source_map_matches(&corrupt, program, original.buf));
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = outputs & 1u ? &selected_map : NULL;
        options.source_quality = outputs & 2u ? &selected_quality : NULL;
        options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
        sb_free(&selected); sb_init(&selected);
        CHECK(hlsl_emit_with_options_diagnostic(program, &selected, NULL, NULL, NULL, &options, &diagnostic) &&
            selected.len == original.len && !strcmp(selected.buf, original.buf));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&original_map, &selected_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&original_quality, &selected_quality));
    }
    const size_t veto_points[] = {1, observations.observations / 2u, observations.observations};
    for (size_t point = 0; point < sizeof(veto_points) / sizeof(*veto_points); ++point) {
        NaturalIfObservations veto = {.program = program, .reject_at = veto_points[point]};
        sb_free(&selected); sb_init(&selected);
        CHECK(!natural_if_emit(program, &selected, &selected_map, &selected_quality, &veto, &diagnostic) &&
            veto.observations == veto_points[point] && !selected_map.complete && !selected_map.count &&
            selected_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    }
    NaturalIfObservations restored = {.program = program};
    sb_free(&selected); sb_init(&selected);
    CHECK(natural_if_emit(program, &selected, &selected_map, &selected_quality, &restored, &diagnostic) &&
        selected.len == original.len && !strcmp(selected.buf, original.buf) &&
        natural_if_maps_equal(&original_map, &selected_map) &&
        hlsl_source_quality_results_equal(&original_quality, &selected_quality));
    sb_free(&selected); sb_free(&original);
    return true;
}

static bool check_natural_arithmetic_callback_drift(USILOpcode opcode, bool compared) {
    const NaturalIfArithmetic arithmetic = {.then_opcode = opcode, .else_opcode = opcode, .join_opcode = opcode,
        .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_right = NATURAL_IF_RIGHT_VECTOR_INPUT};
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init_arithmetic(&fixture, 2, 12, true, false, false, false,
        compared ? USIL_OP_LT : USIL_OP_NOP, compared ? 2 : 0, UINT32_C(0x3ec00000), &arithmetic));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count <= 11);
    USILInstruction saved[11];
    memcpy(saved, program->instructions, (size_t)program->instruction_count * sizeof(*saved));
    const DXBCSignatureElement input = program->inputs[0];
    StringBuilder original, changed;
    sb_init(&original); sb_init(&changed);
    HLSLExpressionSourceMap original_map, changed_map;
    HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations baseline = {.program = program};
    CHECK(natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic) && baseline.observations > 2);
    const size_t mutation_points[] = {1, baseline.observations / 2u, baseline.observations};
    const unsigned mutations[] = {10, opcode == USIL_OP_DIV ? 12u : 11u, 13, 14, 2, 4, 15};
    for (size_t action = 0; action < sizeof(mutations) / sizeof(*mutations); ++action) {
        const unsigned mutation = mutations[action];
        for (size_t point = mutation == 4 || mutation == 15 ? 2u : 0; point < 3; ++point) {
            NaturalIfObservations drift = {.program = program, .mutable_program = program, .mutation = mutation,
                .mutate_at = mutation_points[point], .arithmetic_instruction = mutation == 12 ? fixture.join_value : fixture.then_value,
                .condition_instruction = fixture.condition, .replacement_opcode = opcode == USIL_OP_MIN ? USIL_OP_MAX : USIL_OP_MIN,
                .mutable_source = &changed, .mutable_map = &changed_map,
                .source_offset = original_map.origins[fixture.then_value].source_begin};
            sb_free(&changed); sb_init(&changed);
            CHECK(!natural_if_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
                drift.mutated && diagnostic.status != HLSL_EMIT_STATUS_OK && !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
            NaturalIfObservations fresh = {.program = program};
            sb_free(&changed); sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && changed_map.complete &&
                hlsl_expression_source_map_matches(&changed_map, program, changed.buf));
            if (mutation == 4 || mutation == 15) CHECK(!strcmp(original.buf, changed.buf));
            else CHECK(strcmp(original.buf, changed.buf));
            memcpy(program->instructions, saved, (size_t)program->instruction_count * sizeof(*saved));
            program->inputs[0] = input;
            fresh = (NaturalIfObservations){.program = program};
            sb_free(&changed); sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed.len == original.len && !strcmp(changed.buf, original.buf) &&
                natural_if_maps_equal(&original_map, &changed_map) &&
                hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_arithmetic_rejections(USILOpcode opcode) {
    const NaturalIfArithmetic arithmetic = {.then_opcode = opcode, .else_opcode = opcode, .join_opcode = opcode,
        .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_right = NATURAL_IF_RIGHT_VECTOR_INPUT};
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init_arithmetic(&fixture, 2, 12, true, false, false, false,
        USIL_OP_LT, 2, UINT32_C(0x3ec00000), &arithmetic));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 11);
    USILInstruction saved[11]; memcpy(saved, program->instructions, sizeof(saved));
    const DXBCSignatureElement input = program->inputs[0], output = program->outputs[0];
    for (unsigned mutation = 0; mutation < 15; ++mutation) {
        USILInstruction *owner = &program->instructions[fixture.then_value];
        switch (mutation) {
        case 0: owner->operands[1].has_neg = true; break;
        case 1: owner->operands[2].has_abs = true; break;
        case 2: owner->precise_mask = 12; break;
        case 3: owner->saturate = true; break;
        case 4: owner->operand_count = 2; break;
        case 5: owner->operands[1].type = OPERAND_TYPE_RESOURCE; break;
        case 6: owner->operands[2].type = OPERAND_TYPE_CONSTANT_BUFFER; break;
        case 7: program->instructions[fixture.else_value].operands[0].destination_mask = 0x40; break;
        case 8: owner->operands[1].register_index = 1; owner->operands[1].index_values[0] = 1; break;
        case 9: program->inputs[0].component_type = 2; break;
        case 10: program->outputs[0].component_type = 1; break;
        case 11: /* A sole-control BOOL writer cannot enter a numeric operation. */
            owner->operands[1] = program->instructions[fixture.condition].operands[0]; break;
        case 12: program->instructions[fixture.join_value].operands[2].swizzle[0] = 3; break;
        case 13: program->instructions[fixture.output].operands[0].destination_mask = 0x10; break;
        case 14: /* One incoming tuple now combines two true-arm generations. */
            owner->operands[0].destination_mask = 0x40; break;
        }
        CHECK(natural_if_rejected(program, NULL));
        memcpy(program->instructions, saved, sizeof(saved));
        program->inputs[0] = input; program->outputs[0] = output;
    }
    CHECK(check_natural_arithmetic_fixture(&fixture, 2, 12, true, true, &arithmetic));
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_arithmetic_emission(void) {
    const USILOpcode operations[] = {USIL_OP_MIN, USIL_OP_MAX, USIL_OP_DIV};
    const char *const names[] = {"MIN", "MAX", "DIV"};
    const struct {unsigned width; uint8_t mask; bool compared, nonzero;} shapes[] = {
        {1, 2, false, true}, {2, 12, true, false}, {3, 7, false, false}, {2, 12, true, true}};
    for (size_t operation = 0; operation < sizeof(operations) / sizeof(*operations); ++operation) {
        const USILOpcode opcode = operations[operation];
        CHECK(!strcmp(dxbc_opcode_name(natural_arithmetic_raw_opcode(opcode)), names[operation]));
        NaturalIfArithmetic arithmetic = {.then_opcode = opcode, .else_opcode = opcode, .join_opcode = opcode,
            .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_right = NATURAL_IF_RIGHT_VECTOR_INPUT};
        for (size_t shape = 0; shape < sizeof(shapes) / sizeof(*shapes); ++shape) {
            NaturalIfFixture fixture;
            CHECK(natural_if_fixture_init_arithmetic(&fixture, shapes[shape].width, shapes[shape].mask, shapes[shape].nonzero,
                false, false, false, shapes[shape].compared ? USIL_OP_LT : USIL_OP_NOP, shapes[shape].compared ? 8 : 0,
                UINT32_C(0x3ec00000), &arithmetic));
            CHECK(check_natural_arithmetic_fixture(&fixture, shapes[shape].width, shapes[shape].mask, shapes[shape].nonzero,
                shapes[shape].compared, &arithmetic));
            natural_if_fixture_dispose(&fixture);
        }
        for (unsigned right = (unsigned)NATURAL_IF_RIGHT_VECTOR_LITERAL; right <= (unsigned)NATURAL_IF_RIGHT_SCALAR_INPUT; ++right) {
            arithmetic.arm_right = (NaturalIfRightOperand)right;
            arithmetic.join_right = (NaturalIfRightOperand)right;
            NaturalIfFixture fixture;
            CHECK(natural_if_fixture_init_arithmetic(&fixture, 3, 7, true, false, false, false,
                USIL_OP_NOP, 0, 0, &arithmetic));
            if (right == NATURAL_IF_RIGHT_VECTOR_LITERAL) {
                const DXBCOperand *literal = &fixture.program.instructions[fixture.then_value].operands[2];
                CHECK(literal->imm_value_count == 4 && literal->immediate_word_count == 4 &&
                    literal->imm_values[0] == UINT32_C(0x80000000) && literal->imm_values[1] == 1 && literal->imm_values[2] == UINT32_C(0x7fc12345));
            }
            CHECK(check_natural_arithmetic_fixture(&fixture, 3, 7, true, false, &arithmetic));
            natural_if_fixture_dispose(&fixture);
        }
        CHECK(check_natural_arithmetic_rejections(opcode));
        CHECK(check_natural_arithmetic_callback_drift(opcode, false));
        CHECK(check_natural_arithmetic_callback_drift(opcode, true));
    }
    /* Division keeps its original literal payloads and operation. Zero,
     * negative zero and NaN are not rewritten into a different denominator. */
    const NaturalIfArithmetic exceptional = {.then_opcode = USIL_OP_DIV, .else_opcode = USIL_OP_DIV, .join_opcode = USIL_OP_DIV,
        .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .custom_literals = true,
        .then_bits = UINT32_C(0x80000000), .else_bits = 0, .join_bits = UINT32_C(0x7fc12345)};
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init_arithmetic(&fixture, 1, 2, true, false, false, false, USIL_OP_NOP, 0, 0, &exceptional));
    const int owners[] = {fixture.then_value, fixture.else_value, fixture.join_value};
    const uint32_t bits[] = {exceptional.then_bits, exceptional.else_bits, exceptional.join_bits};
    for (unsigned index = 0; index < 3; ++index)
        CHECK(fixture.program.instructions[owners[index]].operands[2].imm_values[0] == bits[index] &&
            fixture.program.instructions[owners[index]].operands[2].immediate_words[0] == bits[index]);
    CHECK(check_natural_arithmetic_fixture(&fixture, 1, 2, true, false, &exceptional));
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_conditional_emission(void) {
    const struct {unsigned width; uint8_t mask;} shapes[] = {
        {1, 1}, {2, 3}, {3, 7}, {1, 2}, {2, 12}};
    for (size_t index = 0; index < sizeof(shapes) / sizeof(shapes[0]); ++index)
        for (unsigned nonzero = 0; nonzero < 2; ++nonzero)
            CHECK(check_natural_if_positive(shapes[index].width, shapes[index].mask,
                nonzero != 0, false, false, false));
    CHECK(check_natural_if_positive(2, 12, true, true, false, false));
    CHECK(check_natural_if_positive(3, 7, false, false, true, false));
    for (unsigned nonzero = 0; nonzero < 2; ++nonzero) {
        CHECK(check_natural_if_positive(1, 2, nonzero != 0, false, false, true));
        CHECK(check_natural_if_positive(2, 12, nonzero != 0, false, false, true));
    }
    CHECK(check_natural_if_rejections());
    CHECK(check_natural_if_optional_outputs());
    CHECK(check_natural_if_callback_drift());
    CHECK(check_natural_if_multiple_outputs(true));
    CHECK(check_natural_if_multiple_outputs(false));
    CHECK(check_natural_comparison_emission());
    CHECK(check_natural_arithmetic_emission());
    /* DXBC IF_Z/NZ compares the raw DWORD, including the float sign bit. A
     * numeric float comparison would take the opposite branch for -0. */
    const struct {uint32_t bits; bool nonzero;} conditions[] = {
        {0, false}, {UINT32_C(0x80000000), true}, {1, true},
        {UINT32_C(0x3f800000), true}, {UINT32_C(0x7fc12345), true}};
    for (size_t index = 0; index < sizeof(conditions) / sizeof(conditions[0]); ++index) {
        CHECK((conditions[index].bits != 0) == conditions[index].nonzero);
        CHECK((conditions[index].bits == 0) != conditions[index].nonzero);
    }
    float negative_zero;
    const uint32_t negative_zero_bits = UINT32_C(0x80000000);
    memcpy(&negative_zero, &negative_zero_bits, sizeof(negative_zero));
    CHECK(negative_zero == 0.0f && negative_zero_bits != 0);
    return true;
}

static bool check_counted_loop_emission(void) {
    USILInstruction instructions[10] = {0};
    instructions[0] = move(emission_reg(OPERAND_TYPE_TEMP, 0), emission_reg(OPERAND_TYPE_INPUT, 0));
    instructions[1] = move(emission_reg(OPERAND_TYPE_TEMP, 1),
                           (DXBCOperand){.type = OPERAND_TYPE_IMMEDIATE32, .imm_value_count = 1});
    instructions[1].operands[0].destination_mask = 0x10;
    instructions[2].opcode = USIL_OP_LOOP;
    instructions[3] = (USILInstruction){.opcode = USIL_OP_UGE, .operand_count = 3};
    instructions[3].operands[0] = emission_reg(OPERAND_TYPE_TEMP, 1);
    instructions[3].operands[0].destination_mask = 0x20;
    instructions[3].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 1);
    instructions[3].operands[1].swizzle_mode = 2;
    instructions[3].operands[2] = emission_reg(OPERAND_TYPE_INPUT, 1);
    /* The UGE writes .y, so an identity source swizzle reads bound.y, not .x. */
    instructions[4] = (USILInstruction){.opcode = USIL_OP_BREAKC,
                                        .operand_count = 1,
                                        .condition_test = DXBC_INSTRUCTION_TEST_NONZERO};
    instructions[4].operands[0] = emission_reg(OPERAND_TYPE_TEMP, 1);
    instructions[4].operands[0].swizzle_mode = 2;
    instructions[4].operands[0].swizzle[0] = 1;
    instructions[5] = (USILInstruction){.opcode = USIL_OP_MUL, .operand_count = 3};
    instructions[5].operands[0] = instructions[5].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 0);
    instructions[5].operands[2] = emission_reg(OPERAND_TYPE_INPUT, 0);
    for (int lane = 0; lane < 4; ++lane)
        instructions[5].operands[2].swizzle[lane] = (uint8_t)((lane + 1) % 4);
    instructions[6] = (USILInstruction){.opcode = USIL_OP_IADD, .operand_count = 3};
    instructions[6].operands[0] = instructions[1].operands[0];
    instructions[6].operands[1] = instructions[3].operands[1];
    instructions[6].operands[2] =
        (DXBCOperand){.type = OPERAND_TYPE_IMMEDIATE32, .imm_value_count = 1, .imm_values = {1}};
    instructions[7].opcode = USIL_OP_ENDLOOP;
    instructions[8] =
        move(emission_reg(OPERAND_TYPE_OUTPUT, 0), emission_reg(OPERAND_TYPE_TEMP, 0));
    instructions[9].opcode = USIL_OP_RET;
    DXBCSignatureElement inputs[2] = {
        {.semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15},
        {.semantic_name = "TEXCOORD",
         .semantic_index = 1,
         .register_id = 1,
         .component_type = 3,
         .mask = 15,
         .rw_mask = 2}};
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram program = {.shader_type_model = "ps_5_0",
                           .instructions = instructions,
                           .instruction_count = 10,
                           .instruction_alloc = 10,
                           .temp_count = 2,
                           .inputs = inputs,
                           .input_count = 2,
                           .input_alloc = 2,
                           .outputs = &output,
                           .output_count = 1,
                           .output_alloc = 1,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                           .shader_model_major = 5};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    options.expression_source_map = &map;
    HLSLEmitDiagnostic diagnostic;
    StringBuilder source;
    sb_init(&source);
    bool emitted = hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                                     &diagnostic);
    if (!emitted)
        fprintf(stderr, "Loop emission: %s at instruction %d, phase %d\n",
                hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index,
                (int)diagnostic.phase);
    CHECK(emitted);
    CHECK(strstr(source.buf, "const uint dxbc_initial_i1 = 0u;"));
    CHECK(strstr(source.buf, "float4 dxbc_merge_i3_r0 = dxbc_value_i0;"));
    CHECK(strstr(source.buf, "[loop] for (uint dxbc_index_i2 = dxbc_initial_i1; dxbc_index_i2 < "
                             "asuint((v1.y)); ++dxbc_index_i2)"));
    CHECK(strstr(source.buf, "dxbc_merge_i3_r0 = dxbc_value_i5;"));
    CHECK(!strstr(source.buf, "dxbc_merge_i3_r1") && !strstr(source.buf, "float4 r"));
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    CHECK(map.origins[1].kind == HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL &&
          map.origins[1].destination_lanes == 1 && map.origins[3].destination_lanes == 2);
    CHECK(map.origins[2].destination_lanes == 0 && map.origins[4].destination_lanes == 0);
    CHECK(map.origins[2].source_begin == map.origins[3].source_begin &&
          map.origins[3].source_end == map.origins[4].source_end &&
          map.origins[4].source_begin == map.origins[6].source_begin);
    sb_free(&source);
    USILInstruction saved[10];
    memcpy(saved, instructions, sizeof(saved));
    for (int mutation = 0; mutation < 10; ++mutation) {
        if (mutation == 0)
            instructions[6].operands[2].imm_values[0] = 2;
        if (mutation == 1)
            instructions[3].opcode = USIL_OP_IGE;
        if (mutation == 2)
            instructions[4].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
        if (mutation == 3)
            instructions[1] = (USILInstruction){.opcode = USIL_OP_NOP};
        if (mutation == 4)
            instructions[6].operands[1] = emission_reg(OPERAND_TYPE_INPUT, 0);
        if (mutation == 5)
            instructions[8].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 1);
        if (mutation == 6)
            instructions[5].precise_mask = 1;
        if (mutation == 7)
            instructions[5].operands[0].destination_mask = 0x70;
        if (mutation == 8)
            instructions[3].operands[2].min_precision = 1;
        if (mutation == 9) {
            instructions[5].opcode = USIL_OP_DERIV_RTX;
            instructions[5].operand_count = 2;
        }
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
        CHECK(!map.complete);
        sb_free(&source);
        memcpy(instructions, saved, sizeof(saved));
    }
    /* Every distinct lane packing must preserve the scalar counter, predicate,
     * and the comparison's logical bound lane. Nonzero bit initializers remain
     * candidates only: concrete acceptance still requires the baseline gate. */
    for (int counter = 0; counter < 4; ++counter) {
        for (int predicate = 0; predicate < 4; ++predicate) {
            if (counter == predicate)
                continue;
            instructions[1].operands[0].destination_mask = 0x10u << counter;
            instructions[3].operands[0].destination_mask = 0x10u << predicate;
            instructions[3].operands[1].swizzle[0] = (uint8_t)counter;
            instructions[4].operands[0].swizzle[0] = (uint8_t)predicate;
            instructions[6].operands[0].destination_mask = 0x10u << counter;
            instructions[6].operands[1].swizzle[0] = (uint8_t)counter;
            inputs[1].rw_mask = (uint8_t)(1u << predicate);
            for (int initial = 0; initial < 3; ++initial) {
                instructions[1].operands[1].imm_values[0] =
                    initial == 2 ? UINT32_MAX : (uint32_t)initial;
                sb_init(&source);
                CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
                CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
                char bound[24];
                snprintf(bound, sizeof(bound), "asuint((v1.%c))", "xyzw"[predicate]);
                CHECK(strstr(source.buf, bound));
                CHECK(map.origins[1].destination_lanes == (1u << counter));
                CHECK(map.origins[3].destination_lanes == (1u << predicate));
                sb_free(&source);
            }
        }
    }
    memcpy(instructions, saved, sizeof(saved));
    inputs[1].rw_mask = 2;
    instructions[1].operands[1].imm_values[0] = UINT32_MAX;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "dxbc_initial_i1 = 4294967295u;"));
    sb_free(&source);
    const char *reserved = "dxbc_index_i2";
    options.reserved_preprocessor_identifiers = &reserved;
    options.reserved_preprocessor_identifier_count = 1;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, &options,
                                             &diagnostic));
    CHECK(diagnostic.reason == HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY && !map.complete);
    sb_free(&source);
    return true;
}

static bool check_function_emission(void) {
    USILInstruction instructions[6] = {0};
    for (int i = 0; i < 5; ++i)
        instructions[i] = (USILInstruction){.opcode = USIL_OP_MUL, .operand_count = 3};
    for (int chain = 0; chain < 2; ++chain) {
        USILInstruction *first = &instructions[chain * 2], *second = first + 1;
        first->operands[0] = second->operands[0] = emission_reg(OPERAND_TYPE_TEMP, chain);
        first->operands[1] = emission_reg(OPERAND_TYPE_INPUT, chain);
        first->operands[2] = emission_reg(OPERAND_TYPE_INPUT, 1 - chain);
        second->operands[1] = first->operands[0];
        second->operands[2] = emission_reg(OPERAND_TYPE_INPUT, chain);
    }
    instructions[4].operands[0] = emission_reg(OPERAND_TYPE_OUTPUT, 0);
    instructions[4].operands[1] = emission_reg(OPERAND_TYPE_TEMP, 0);
    instructions[4].operands[2] = emission_reg(OPERAND_TYPE_TEMP, 1);
    instructions[5].opcode = USIL_OP_RET;
    DXBCSignatureElement inputs[2] = {
        {.semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15},
        {.semantic_name = "TEXCOORD",
         .semantic_index = 1,
         .register_id = 1,
         .component_type = 3,
         .mask = 15,
         .rw_mask = 15}};
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram program = {.shader_type_model = "ps_5_0",
                           .instructions = instructions,
                           .instruction_count = 6,
                           .instruction_alloc = 6,
                           .temp_count = 2,
                           .inputs = inputs,
                           .input_count = 2,
                           .input_alloc = 2,
                           .outputs = &output,
                           .output_count = 1,
                           .output_alloc = 1,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                           .shader_model_major = 5};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    CHECK(strstr(source.buf, "float4 dxbc_mul_chain_") &&
          strstr(source.buf, "const float4 dxbc_value_i1 = dxbc_mul_chain_") &&
          strstr(source.buf, "const float4 dxbc_value_i3 = dxbc_mul_chain_"));
    for (int i = 0; i < 4; ++i) {
        const HLSLExpressionOrigin *origin = &map.origins[i];
        CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_FUNCTION);
        CHECK(origin->definition_begin < origin->definition_end &&
              origin->definition_end < origin->source_begin);
        CHECK(strncmp(source.buf + origin->source_begin, "dxbc_mul_chain_", 15) == 0);
        CHECK(source.buf[origin->definition_begin] == '(');
    }
    CHECK(map.origins[0].source_begin == map.origins[1].source_begin);
    CHECK(map.origins[2].source_begin == map.origins[3].source_begin);
    CHECK(map.origins[0].source_end < map.origins[2].source_begin);
    CHECK(map.origins[0].definition_begin == map.origins[2].definition_begin);
    CHECK(map.origins[1].definition_begin == map.origins[3].definition_begin);
    HLSLExpressionSourceMap original = map;
    for (int mutation = 0; mutation < 4; ++mutation) {
        if (mutation == 0)
            map.origins[0].definition_end = SIZE_MAX;
        if (mutation == 1)
            map.origins[0].definition_begin = map.origins[0].definition_end;
        if (mutation == 2)
            map.origins[0].kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
        if (mutation == 3)
            map.origins[4].definition_end = 1;
        CHECK(!hlsl_expression_source_map_matches(&map, &program, source.buf));
        map = original;
    }
    CHECK(!hlsl_expression_source_map_offset(&map, SIZE_MAX));
    CHECK(memcmp(&map, &original, sizeof(map)) == 0);
    CHECK(hlsl_expression_source_map_offset(&map, 17));
    for (int i = 0; i < 4; ++i) {
        CHECK(map.origins[i].source_begin == original.origins[i].source_begin + 17);
        CHECK(map.origins[i].definition_begin == original.origins[i].definition_begin + 17);
    }
    sb_free(&source);
    /* One identity-swizzled producer use changed: only one reusable chain
     * remains, so both helpers disappear while ordinary expressions remain. */
    instructions[1].operands[1].swizzle[0] = 1;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(!strstr(source.buf, "dxbc_mul_chain_"));
    for (size_t i = 0; i < map.count; ++i)
        CHECK(!map.origins[i].definition_begin && !map.origins[i].definition_end);
    sb_free(&source);
    instructions[1].operands[1].swizzle[0] = 0;
    /* Immediate scale arguments use the opposite compiler inverse signature.
     * Keep the product's side in the outer operation; never commute it here. */
    DXBCOperand scale0 = instructions[1].operands[2], scale1 = instructions[3].operands[2];
    instructions[1].operands[2] = instructions[3].operands[2] =
        (DXBCOperand){.type = OPERAND_TYPE_IMMEDIATE32,
                      .imm_value_count = 1,
                      .imm_values = {UINT32_C(0x3f000000)}};
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "float4 dxbc_mul_chain_left("));
    CHECK(strstr(source.buf, "return (product * scale);"));
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    sb_free(&source);
    instructions[1].operands[2] = scale0;
    instructions[3].operands[2] = scale1;
    const char *reserved = "dxbc_mul_chain_right";
    options.reserved_preprocessor_identifiers = &reserved;
    options.reserved_preprocessor_identifier_count = 1;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(!map.complete);
    sb_free(&source);
    return true;
}

static bool check_unity_uv_emission(void) {
    USILInstruction instructions[2] = {0};
    instructions[0].opcode = USIL_OP_MAD;
    instructions[0].operand_count = 4;
    instructions[0].operands[0] = emission_reg(OPERAND_TYPE_OUTPUT, 0);
    instructions[0].operands[1] = emission_reg(OPERAND_TYPE_INPUT, 0);
    instructions[0].operands[2] = emission_reg(OPERAND_TYPE_INPUT, 1);
    instructions[0].operands[3] = emission_reg(OPERAND_TYPE_INPUT, 1);
    for (int lane = 0; lane < 4; ++lane) {
        instructions[0].operands[2].swizzle[lane] = (uint8_t)(lane % 2);
        instructions[0].operands[3].swizzle[lane] = (uint8_t)(2 + lane % 2);
    }
    instructions[0].source_instruction_index = 11;
    instructions[1].opcode = USIL_OP_RET;
    instructions[1].source_instruction_index = 12;
    DXBCSignatureElement inputs[2] = {
        {.semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15},
        {.semantic_name = "TEXCOORD",
         .semantic_index = 1,
         .register_id = 1,
         .component_type = 3,
         .mask = 15,
         .rw_mask = 15},
    };
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram program = {.shader_type_model = "ps_5_0",
                           .instructions = instructions,
                           .instruction_count = 2,
                           .instruction_alloc = 2,
                           .inputs = inputs,
                           .input_count = 2,
                           .input_alloc = 2,
                           .outputs = &output,
                           .output_count = 1,
                           .output_alloc = 1,
                           .has_stage_contract = true,
                           .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                           .shader_model_major = 5};
    CHECK(hlsl_unity_uv_lift_matches(&program));
    StringBuilder ordinary, baseline, candidate;
    sb_init(&ordinary);
    sb_init(&baseline);
    sb_init(&candidate);
    CHECK(hlsl_emit(&program, &ordinary, NULL, NULL, NULL));
    CHECK(!strstr(ordinary.buf, "UnityCG") && !strstr(ordinary.buf, HLSL_UNITY_UV_FUNCTION));
    HLSLEmitOptions options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
    options.unity_uv_helper = HLSL_UNITY_UV_INCLUDE;
    CHECK(hlsl_emit_with_options(&program, &baseline, NULL, NULL, NULL, &options));
    CHECK(strstr(baseline.buf, "#include \"UnityCG.cginc\"") &&
          !strstr(baseline.buf, HLSL_UNITY_UV_FUNCTION));
    options.mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    HLSLExpressionSourceMap map;
    options.expression_source_map = &map;
    CHECK(hlsl_emit_with_options(&program, &candidate, NULL, NULL, NULL, &options));
    CHECK(strstr(candidate.buf, "#include \"UnityCG.cginc\""));
    CHECK(strstr(candidate.buf, "o0 = " HLSL_UNITY_UV_FUNCTION "((v0), (v1));"));
    CHECK(map.complete && map.count == 2 && map.origins[0].destination_lanes == 15);
    CHECK(map.origins[0].kind == HLSL_EXPRESSION_ORIGIN_UNITY_UV &&
          map.origins[0].source_instruction_index == 11);
    CHECK(map.origins[1].kind == HLSL_EXPRESSION_ORIGIN_RETURN);
    CHECK(!map.origins[0].definition_begin && !map.origins[0].definition_end);
    CHECK(hlsl_expression_source_map_matches(&map, &program, candidate.buf));
    const HLSLExpressionSourceMap valid = map;
    map.origins[0].definition_end = map.origins[0].source_begin;
    CHECK(!hlsl_expression_source_map_matches(&map, &program, candidate.buf));
    map = valid;
    map.origins[0].kind = HLSL_EXPRESSION_ORIGIN_FUNCTION;
    CHECK(!hlsl_expression_source_map_matches(&map, &program, candidate.buf));
    sb_free(&candidate);
    /* A plain high-level request must not opt itself into a Unity include. */
    options.unity_uv_helper = HLSL_UNITY_UV_DISABLED;
    sb_init(&candidate);
    CHECK(hlsl_emit_with_options(&program, &candidate, NULL, NULL, NULL, &options));
    CHECK(map.complete && !strstr(candidate.buf, "UnityCG"));
    CHECK(map.origins[0].kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION);
    CHECK(hlsl_expression_source_map_matches(&map, &program, candidate.buf));
    sb_free(&candidate);
    options.unity_uv_helper = HLSL_UNITY_UV_INCLUDE;
    const char *collision = HLSL_UNITY_UV_FUNCTION;
    options.reserved_preprocessor_identifiers = &collision;
    options.reserved_preprocessor_identifier_count = 1;
    sb_init(&candidate);
    CHECK(!hlsl_emit_with_options(&program, &candidate, NULL, NULL, NULL, &options));
    CHECK(!map.complete);
    sb_free(&candidate);
    options.reserved_preprocessor_identifiers = NULL;
    options.reserved_preprocessor_identifier_count = 0;
    for (int operand = 1; operand < 4; ++operand) {
        for (int lane = 0; lane < 4; ++lane) {
            instructions[0].operands[operand].swizzle[lane] ^= 1;
            CHECK(!hlsl_unity_uv_lift_matches(&program));
            sb_init(&candidate);
            CHECK(!hlsl_emit_with_options(&program, &candidate, NULL, NULL, NULL, &options));
            CHECK(!map.complete);
            sb_free(&candidate);
            instructions[0].operands[operand].swizzle[lane] ^= 1;
        }
    }
    for (int mutation = 0; mutation < 12; ++mutation) {
        USILProgram changed = program;
        USILInstruction changed_instructions[2];
        memcpy(changed_instructions, instructions, sizeof(instructions));
        changed.instructions = changed_instructions;
        switch (mutation) {
        case 0:
            changed_instructions[0].precise_mask = 1;
            break;
        case 1:
            changed_instructions[0].saturate = true;
            break;
        case 2:
            changed_instructions[0].operands[2].min_precision = 1;
            break;
        case 3:
            changed_instructions[0].operands[3].has_neg = true;
            break;
        case 4:
            changed_instructions[0].operands[3].register_index = 0;
            break;
        case 5:
            changed_instructions[0].operands[0].destination_mask = 0x30;
            break;
        case 6:
            changed_instructions[0].operands[1].register_index_dim = 2;
            break;
        case 7:
            changed_instructions[0].operands[1].index_values[0] = 1;
            break;
        case 8:
            changed.cbuffer_count = 1;
            break;
        case 9:
            changed.has_global_flags = true;
            changed.global_flags = 0;
            break;
        case 10:
            changed.has_stage_contract = false;
            break;
        case 11:
            changed.program_type = DXBC_PROGRAM_TYPE_GEOMETRY;
            break;
        }
        CHECK(!hlsl_unity_uv_lift_matches(&changed));
    }
    CHECK(hlsl_unity_uv_lift_matches(&program));
    options.mode = HLSL_EMIT_MODE_READABLE;
    options.expression_source_map = NULL;
    sb_init(&candidate);
    CHECK(!hlsl_emit_with_options(&program, &candidate, NULL, NULL, NULL, &options));
    sb_free(&candidate);
    sb_free(&baseline);
    sb_free(&ordinary);
    return true;
}

int main(void) {
    if (!check_multiple_results() || !check_modified_moves() ||
        !check_merge_and_undefined_lanes() || !check_loop_phi() || !check_structured_flow_edges() ||
        !check_switch_flow_and_scope() || !check_flow_structure_validation() ||
        !check_control_region_proofs() || !check_exhaustive_postdominance() ||
        !check_long_dominator_chain() || !check_copy_candidates() || !check_generated_copy_lanes() ||
        !check_result_candidates() ||
        !check_effects() || !check_transactions() || !check_result_transactions() ||
        !check_expression_emission() || !check_conditional_emission() ||
        !check_natural_conditional_emission() ||
        !check_counted_loop_emission() || !check_function_emission() || !check_unity_uv_emission())
        return 1;
    puts("HLSL dataflow contracts passed");
    return 0;
}
