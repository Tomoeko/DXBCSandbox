// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_copy_lift.h"
#include "translation/hlsl_lift_transaction.h"
#include "translation/hlsl_matrix_lift.h"
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
    NATURAL_IF_RIGHT_SCALAR_INPUT,
    NATURAL_IF_RIGHT_VECTOR_TEMP
} NaturalIfRightOperand;

typedef enum {
    NATURAL_IF_INPUTS_SEPARATE,
    NATURAL_IF_INPUTS_PACKED_UNION,
    NATURAL_IF_INPUTS_PACKED_SPLIT
} NaturalIfInputLayout;

typedef struct {
    USILOpcode then_opcode, else_opcode, join_opcode;
    NaturalIfRightOperand arm_right, join_right;
    bool custom_literals;
    uint32_t then_bits, else_bits, join_bits;
    /* Only MAD emits these fields, preserving every binary fixture's words. */
    NaturalIfRightOperand arm_third, join_third;
    bool custom_third_literals;
    uint32_t then_third_bits, else_third_bits, join_third_bits;
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
    case USIL_OP_MAD: return 50;
    default: return UINT32_MAX;
    }
}

static bool natural_if_fixture_init_layout(NaturalIfFixture *fixture, unsigned width,
    uint8_t temp_mask, bool nonzero, bool dead_phi, bool vector_literal, bool temp_condition,
    USILOpcode comparison, uint8_t predicate_mask, uint32_t comparison_bits,
    const NaturalIfArithmetic *arithmetic, NaturalIfInputLayout input_layout) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    CHECK(width >= 1 && width <= 3);
    const bool packed_inputs = input_layout != NATURAL_IF_INPUTS_SEPARATE;
    CHECK(input_layout <= NATURAL_IF_INPUTS_PACKED_SPLIT);
    CHECK(!packed_inputs || (width == 3 && temp_mask == 7 && !temp_condition));
    CHECK(!arithmetic || (natural_arithmetic_raw_opcode(arithmetic->then_opcode) != UINT32_MAX &&
        natural_arithmetic_raw_opcode(arithmetic->else_opcode) != UINT32_MAX &&
        natural_arithmetic_raw_opcode(arithmetic->join_opcode) != UINT32_MAX &&
        arithmetic->arm_right <= NATURAL_IF_RIGHT_SCALAR_INPUT && arithmetic->join_right <= NATURAL_IF_RIGHT_SCALAR_INPUT &&
        arithmetic->arm_third <= NATURAL_IF_RIGHT_VECTOR_TEMP && arithmetic->join_third <= NATURAL_IF_RIGHT_VECTOR_TEMP));
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
        NATURAL_WORD(packed_inputs ? UINT32_C(0x0010103a) : UINT32_C(0x0010100a)); NATURAL_WORD(packed_inputs ? 0 : 1); \
    } else if ((mode) == NATURAL_IF_RIGHT_VECTOR_TEMP) { \
        NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, (components))); NATURAL_WORD(0); \
    } else { \
        NATURAL_WORD(UINT32_C(0x00004001)); NATURAL_WORD(bits); \
    } \
} while (0)
    const NaturalIfRightOperand arm_right = arithmetic ? arithmetic->arm_right : NATURAL_IF_RIGHT_SCALAR_LITERAL;
    const NaturalIfRightOperand join_right = arithmetic ? arithmetic->join_right : NATURAL_IF_RIGHT_VECTOR_INPUT;
    const bool then_mad = arithmetic && arithmetic->then_opcode == USIL_OP_MAD;
    const bool else_mad = arithmetic && arithmetic->else_opcode == USIL_OP_MAD;
    const bool join_mad = arithmetic && arithmetic->join_opcode == USIL_OP_MAD;
    const unsigned arm_third_words = arithmetic && arithmetic->arm_third == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 5u : 2u;
    const unsigned join_third_words = arithmetic && arithmetic->join_third == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 5u : 2u;
    NATURAL_WORD(NATURAL_INST(106, 1) | 1u << 11u);
    NATURAL_WORD(NATURAL_INST(98, 3) | 2u << 11u);
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_INPUT,
        input_layout == NATURAL_IF_INPUTS_PACKED_UNION ? 15 : output_mask)); NATURAL_WORD(0);
    if (input_layout != NATURAL_IF_INPUTS_PACKED_UNION) {
        NATURAL_WORD(NATURAL_INST(98, 3) | 2u << 11u);
        NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_INPUT, packed_inputs ? 8 : 1)); NATURAL_WORD(packed_inputs ? 0 : 1);
    }
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
        NATURAL_WORD(packed_inputs ? UINT32_C(0x0010103a) : UINT32_C(0x0010100a)); NATURAL_WORD(packed_inputs ? 0 : 1);
        NATURAL_WORD(UINT32_C(0x00004001)); NATURAL_WORD(comparison_bits);
    }
    NATURAL_WORD(NATURAL_INST(31, 3) | (nonzero ? UINT32_C(0x40000) : 0));
    NATURAL_WORD(compared ? UINT32_C(0x0010000a) | (uint32_t)predicate_lane << 4u
        : temp_condition ? UINT32_C(0x0010000a) |
            (uint32_t)physical[width - 1u] << 4u : packed_inputs ? UINT32_C(0x0010103a) : UINT32_C(0x0010100a));
    NATURAL_WORD(compared || temp_condition ? 2 : packed_inputs ? 0 : 1);
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
        (arm_right == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 10u : 7u) + (then_mad ? arm_third_words : 0u)));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(0);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, identity)); NATURAL_WORD(0);
    NATURAL_RIGHT(arm_right, arithmetic && arithmetic->custom_literals ? arithmetic->then_bits : UINT32_C(0x3fa00000), input_for_temp);
    if (then_mad) NATURAL_RIGHT(arithmetic->arm_third,
        arithmetic->custom_third_literals ? arithmetic->then_third_bits : UINT32_C(0x3f800000),
        arithmetic->arm_third == NATURAL_IF_RIGHT_VECTOR_TEMP ? identity : input_for_temp);
    NATURAL_WORD(NATURAL_INST(18, 1));
    NATURAL_WORD(NATURAL_INST(54, 5));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(0);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, input_for_temp)); NATURAL_WORD(0);
    NATURAL_WORD(NATURAL_INST(arithmetic ? natural_arithmetic_raw_opcode(arithmetic->else_opcode) : 56,
        (arm_right == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 10u : 7u) + (else_mad ? arm_third_words : 0u)));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, temp_mask)); NATURAL_WORD(0);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, identity)); NATURAL_WORD(0);
    NATURAL_RIGHT(arm_right, arithmetic && arithmetic->custom_literals ? arithmetic->else_bits : UINT32_C(0x40000000), input_for_temp);
    if (else_mad) NATURAL_RIGHT(arithmetic->arm_third,
        arithmetic->custom_third_literals ? arithmetic->else_third_bits : UINT32_C(0x40400000),
        arithmetic->arm_third == NATURAL_IF_RIGHT_VECTOR_TEMP ? identity : input_for_temp);
    NATURAL_WORD(NATURAL_INST(21, 1));
    NATURAL_WORD(NATURAL_INST(arithmetic ? natural_arithmetic_raw_opcode(arithmetic->join_opcode) : 0,
        (join_right == NATURAL_IF_RIGHT_VECTOR_LITERAL ? 10u : 7u) + (join_mad ? join_third_words : 0u)));
    NATURAL_WORD(NATURAL_DEST(OPERAND_TYPE_TEMP, output_mask)); NATURAL_WORD(1);
    NATURAL_WORD(natural_if_source_token(OPERAND_TYPE_TEMP, temp_for_output)); NATURAL_WORD(0);
    NATURAL_RIGHT(join_right, arithmetic && arithmetic->custom_literals ? arithmetic->join_bits : UINT32_C(0x3f000000), identity);
    if (join_mad) NATURAL_RIGHT(arithmetic->join_third,
        arithmetic->custom_third_literals ? arithmetic->join_third_bits : UINT32_C(0x3e800000),
        arithmetic->join_third == NATURAL_IF_RIGHT_VECTOR_TEMP ? temp_for_output : identity);
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
        {.semantic_name = "TEXCOORD", .semantic_index = 1, .register_id = packed_inputs ? 0u : 1u,
            .component_type = 3, .mask = packed_inputs ? 8 : 1, .rw_mask = packed_inputs ? 8 : 1}};
    const DXBCSignatureElement output = {.semantic_name = "SV_Target", .system_value = 64,
        .component_type = 3, .mask = output_mask, .rw_mask = (uint8_t)(15u & ~output_mask)};
    CHECK(natural_if_fixture_decode_words(fixture, UINT32_C(0x00000050), words, word_count,
        inputs, 2, &output, 1));
    CHECK(fixture->program.input_count == 2 && fixture->program.output_count == 1);
    CHECK(fixture->program.inputs[0].mask == output_mask &&
        fixture->program.inputs[0].rw_mask == output_mask &&
        fixture->program.inputs[1].mask == (packed_inputs ? 8 : 1) && fixture->program.inputs[1].rw_mask == (packed_inputs ? 8 : 1));
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
        (uint32_t)(fixture->program.signature_declaration_count + 2 + prefix));
    CHECK(fixture->program.instructions[fixture->condition].condition_test == (nonzero ?
        DXBC_INSTRUCTION_TEST_NONZERO : DXBC_INSTRUCTION_TEST_ZERO));
    CHECK(usil_operand_destination_lane_mask(
        &fixture->program.instructions[fixture->then_value].operands[0]) == temp_mask);
    if (arithmetic) CHECK(fixture->program.instructions[fixture->then_value].opcode == arithmetic->then_opcode &&
        fixture->program.instructions[fixture->else_value].opcode == arithmetic->else_opcode &&
        fixture->program.instructions[fixture->join_value].opcode == arithmetic->join_opcode);
    return true;
}

static bool natural_if_fixture_init_arithmetic(NaturalIfFixture *fixture, unsigned width,
    uint8_t temp_mask, bool nonzero, bool dead_phi, bool vector_literal, bool temp_condition,
    USILOpcode comparison, uint8_t predicate_mask, uint32_t comparison_bits,
    const NaturalIfArithmetic *arithmetic) {
    return natural_if_fixture_init_layout(fixture, width, temp_mask, nonzero, dead_phi, vector_literal,
        temp_condition, comparison, predicate_mask, comparison_bits, arithmetic, NATURAL_IF_INPUTS_SEPARATE);
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
    uint8_t mad_binary_events[64];
    size_t reject_at;
    USILProgram *mutable_program;
    StringBuilder *mutable_source;
    HLSLExpressionSourceMap *mutable_map;
    size_t source_offset;
    size_t signature_observation;
    size_t first_header_observation;
    size_t first_preheader_observation;
    size_t mutate_at;
    unsigned mutation;
    int arithmetic_instruction, condition_instruction, modifier_operand;
    int packed_xyz_field, packed_scalar_field, packed_source_instruction;
    bool track_packed;
    uint8_t packed_fields_seen;
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
    if (!ledger->first_preheader_observation && observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION &&
        facts->instruction_index < 0 && ledger->mutable_source && ledger->mutable_source->buf &&
        strstr(ledger->mutable_source->buf, "// Model:") && !strstr(ledger->mutable_source->buf, "float"))
        ledger->first_preheader_observation = ledger->observations;
    if (!ledger->first_header_observation && observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION &&
        facts->instruction_index < 0 && ledger->mutable_source && ledger->mutable_source->buf &&
        strstr(ledger->mutable_source->buf, "float"))
        ledger->first_header_observation = ledger->observations;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION &&
        facts->instruction_index < 0 && ledger->mutable_source && ledger->mutable_source->buf &&
        ledger->mutable_source->len >= sizeof(" output;\n") - 1u &&
        !strcmp(ledger->mutable_source->buf + ledger->mutable_source->len - (sizeof(" output;\n") - 1u), " output;\n"))
        ledger->signature_observation = ledger->observations;
    if (ledger->track_packed && facts->known && facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL) {
        if (facts->logical_value_id == (HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE |
                (uint64_t)(unsigned)ledger->packed_xyz_field)) {
            ledger->packed_fields_seen |= 1;
            if (facts->components != 3 && facts->components != 1) ledger->wrong_owner = true;
        }
        if (facts->logical_value_id == (HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE |
                (uint64_t)(unsigned)ledger->packed_scalar_field)) {
            ledger->packed_fields_seen |= 2;
            if (facts->components != 1) ledger->wrong_owner = true;
        }
    }
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
        if (owner->opcode == USIL_OP_MIN || owner->opcode == USIL_OP_MAX || owner->opcode == USIL_OP_DIV ||
            owner->opcode == USIL_OP_MAD) {
            if (facts->lanes != usil_operand_destination_lane_mask(&owner->operands[0])) ledger->wrong_owner = true;
            else ledger->arithmetic_owners |= UINT64_C(1) << (unsigned)facts->instruction_index;
            if (owner->opcode == USIL_OP_MAD && observation->ast_kind == AST_EXPR_BINARY &&
                ledger->mad_binary_events[facts->instruction_index] < UINT8_MAX)
                ++ledger->mad_binary_events[facts->instruction_index];
        }
    }
    const bool early_header = ledger->mutation == 19 && !ledger->mutated &&
        observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION && facts->instruction_index < 0 &&
        ledger->mutable_source && ledger->mutable_source->buf &&
        strstr(ledger->mutable_source->buf, "float");
    if (ledger->mutable_program && (ledger->observations == ledger->mutate_at || early_header)) {
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
        case 16: {
            DXBCSignatureElement *field = &program->inputs[ledger->packed_scalar_field];
            memset(field->semantic_name, 0, sizeof(field->semantic_name));
            memcpy(field->semantic_name, "COORDGATE", sizeof("COORDGATE"));
            field->semantic_name_length = sizeof("COORDGATE") - 1u;
            break;
        }
        case 17: {
            DXBCOperand *source = &program->instructions[ledger->packed_source_instruction].operands[
                program->instructions[ledger->packed_source_instruction].opcode == USIL_OP_IF ? 0 : 1];
            memset(source->swizzle, 2, sizeof(source->swizzle));
            source->raw_token = (source->raw_token & ~UINT32_C(0x30)) | UINT32_C(0x20);
            break;
        }
        case 18:
            program->inputs[ledger->packed_xyz_field].interpolation_mode = 3;
            program->inputs[ledger->packed_scalar_field].interpolation_mode = 3;
            for (int index = 0; index < program->signature_declaration_count; ++index) {
                USILSignatureDeclaration *declaration = &program->signature_declarations[index];
                if (declaration->operand_type == OPERAND_TYPE_INPUT && declaration->register_id ==
                    program->inputs[ledger->packed_scalar_field].register_id)
                    declaration->interpolation_mode = 3;
            }
            break;
        case 19: {
            char *type = strstr(ledger->mutable_source->buf, "float");
            if (!type) return false;
            *type ^= 1;
            break;
        }
        case 20:
            if (!ledger->mutable_source) return false;
            sb_append(ledger->mutable_source, "// injected header bytes\n");
            break;
        case 21: {
            DXBCOperand *third = &program->instructions[ledger->arithmetic_instruction].operands[3];
            third->imm_values[0] = third->immediate_words[0] = UINT32_C(0x40200000);
            break;
        }
        case 22: {
            USILInstruction *owner = &program->instructions[ledger->arithmetic_instruction];
            const DXBCOperand right = owner->operands[2];
            owner->operands[2] = owner->operands[3]; owner->operands[3] = right;
            break;
        }
        case 23: {
            DXBCOperand *third = &program->instructions[ledger->arithmetic_instruction].operands[3];
            memset(third->swizzle, 1, sizeof(third->swizzle));
            third->raw_token &= ~UINT32_C(0x00000ff0);
            for (unsigned lane = 0; lane < 4; ++lane)
                third->raw_token |= (uint32_t)third->swizzle[lane] << (4u + lane * 2u);
            break;
        }
        case 24: {
            USILInstruction *owner = &program->instructions[ledger->arithmetic_instruction];
            owner->opcode = USIL_OP_MUL; owner->operand_count = 3;
            memset(&owner->operands[3], 0, sizeof(owner->operands[3]));
            break;
        }
        case 25:
            if (!ledger->mutable_source) return false;
            sb_append(ledger->mutable_source, "static const float injectedValue = 1.0f;\n");
            if (!sb_ok(ledger->mutable_source)) return false;
            break;
        case 26: {
            USILInstruction *dot = &program->instructions[ledger->arithmetic_instruction];
            const DXBCOperand left = dot->operands[1];
            dot->operands[1] = dot->operands[2]; dot->operands[2] = left;
            break;
        }
        case 27:
            memset(program->inputs[0].semantic_name, 0, sizeof(program->inputs[0].semantic_name));
            memcpy(program->inputs[0].semantic_name, "COORDVALUE", sizeof("COORDVALUE"));
            program->inputs[0].semantic_name_length = sizeof("COORDVALUE") - 1u;
            break;
        case 28: {
            DXBCOperand *literal = &program->instructions[ledger->arithmetic_instruction].operands[2];
            literal->imm_values[1] = UINT32_C(0x3f000000); literal->immediate_words[1] = UINT32_C(0x3f000000);
            break;
        }
        case 29: {
            USILInstruction *condition = &program->instructions[ledger->condition_instruction];
            condition->condition_test = condition->condition_test == DXBC_INSTRUCTION_TEST_NONZERO
                ? DXBC_INSTRUCTION_TEST_ZERO : DXBC_INSTRUCTION_TEST_NONZERO;
            break;
        }
        case 30: {
            DXBCOperand *source = &program->instructions[ledger->arithmetic_instruction].operands[2];
            const uint8_t selected = source->swizzle[0] == 2 ? 3 : 1;
            memset(source->swizzle, selected, sizeof(source->swizzle));
            source->raw_token = natural_if_source_token(OPERAND_TYPE_INPUT, source->swizzle);
            break;
        }
        case 31: {
            DXBCOperand *source = &program->instructions[ledger->arithmetic_instruction].operands[ledger->modifier_operand];
            if (!source->extended_tokens || source->extended_token_count != 1 || !source->has_abs) return false;
            source->has_neg = !source->has_neg;
            source->extended_tokens[0] = UINT32_C(1) | (source->has_neg ? UINT32_C(0xc0) : UINT32_C(0x80));
            break;
        }
        case 32: {
            DXBCOperand *threshold = &program->instructions[ledger->arithmetic_instruction].operands[2];
            threshold->imm_values[0] = threshold->immediate_words[0] = UINT32_C(0x3f400000);
            break;
        }
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
    /* Two balanced sequential diamonds remain outside the contained-region
     * route. Neither diamond writes an output conditionally. */
    USILInstruction nested[24] = {0};
    memcpy(nested, saved, 7u * sizeof(*saved));
    memcpy(nested + 7, saved, sizeof(saved));
    for (int index = 0; index < 17; ++index)
        nested[index].source_instruction_index = (uint32_t)index + 5u;
    USILProgram extended = *program;
    extended.instructions = nested;
    extended.instruction_count = 17;
    extended.instruction_alloc = 24;
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

static bool natural_if_multiple_outputs_fixture_layout(NaturalIfFixture *fixture, bool vertex,
    USILOpcode comparison, uint8_t predicate_mask, bool nonzero, NaturalIfInputLayout input_layout) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    CHECK(input_layout <= NATURAL_IF_INPUTS_PACKED_SPLIT);
    const bool packed_inputs = input_layout != NATURAL_IF_INPUTS_SEPARATE;
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
        if (input == 2 && input_layout == NATURAL_IF_INPUTS_PACKED_UNION) continue;
        MULTI_WORD(MULTI_INST(vertex ? 95 : 98, 3) | (vertex ? 0 : 2u << 11u));
        const uint8_t mask = input == 1 && input_layout == NATURAL_IF_INPUTS_PACKED_UNION ? 15 :
            input == 2 && packed_inputs ? 8 : masks[input];
        MULTI_WORD(UINT32_C(0x00101002) | (uint32_t)mask << 4u);
        MULTI_WORD(input == 2 && packed_inputs ? 1 : input);
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
        MULTI_WORD(packed_inputs ? UINT32_C(0x0010103a) : UINT32_C(0x0010100a)); MULTI_WORD(packed_inputs ? 1 : 2);
        MULTI_WORD(UINT32_C(0x00004001)); MULTI_WORD(UINT32_C(0x3ec00000));
    }
    MULTI_WORD(MULTI_INST(31, 3) | (nonzero ? UINT32_C(0x40000) : 0));
    MULTI_WORD(compared ? UINT32_C(0x0010000a) | (uint32_t)predicate_lane << 4u
        : packed_inputs ? UINT32_C(0x0010103a) : UINT32_C(0x0010100a)); MULTI_WORD(compared ? 1 : packed_inputs ? 1 : 2);
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
    if (packed_inputs) {
        inputs[2].register_id = 1;
        inputs[2].mask = inputs[2].rw_mask = 8;
    }
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
        fixture->program.signature_declaration_count == (input_layout == NATURAL_IF_INPUTS_PACKED_UNION ? 4 : 5));
    for (unsigned input = 0; input < 3; ++input)
        CHECK(fixture->program.inputs[input].mask == (input == 2 && packed_inputs ? 8 : masks[input]) &&
            fixture->program.inputs[input].rw_mask == (input == 2 && packed_inputs ? 8 : masks[input]));
    CHECK(fixture->program.outputs[0].mask == 15 && !fixture->program.outputs[0].rw_mask &&
        fixture->program.outputs[1].mask == 7 && fixture->program.outputs[1].rw_mask == 8);
    const int prefix = compared ? 1 : 0;
    fixture->condition = prefix;
    fixture->then_value = 1 + prefix;
    fixture->else_value = 3 + prefix;
    fixture->join_value = 5 + prefix;
    fixture->output = 5 + prefix;
    CHECK(fixture->program.instructions[0].source_instruction_index ==
        (uint32_t)(fixture->program.signature_declaration_count + 2));
    return true;
}

static bool natural_if_multiple_outputs_fixture_mode(NaturalIfFixture *fixture, bool vertex,
    USILOpcode comparison, uint8_t predicate_mask, bool nonzero) {
    return natural_if_multiple_outputs_fixture_layout(fixture, vertex, comparison, predicate_mask,
        nonzero, NATURAL_IF_INPUTS_SEPARATE);
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
    bool packed_inputs = false;
    CHECK(hlsl_natural_input_layout_supported(program, &packed_inputs));
    NaturalIfObservations observations = {.program = program, .track_packed = packed_inputs,
        .packed_xyz_field = 0, .packed_scalar_field = 1};
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
    if (packed_inputs) CHECK(observations.packed_fields_seen == 3);
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
            opcodes[owner] == USIL_OP_MIN ? "min(" : opcodes[owner] == USIL_OP_MAX ? "max(" :
            opcodes[owner] == USIL_OP_MAD ? " * " : " / ");
        CHECK(operation && operation < original.buf + origin->source_end);
        if (opcodes[owner] == USIL_OP_MAD) {
            const char *addition = strstr(operation + 3, " + ");
            CHECK(addition && addition < original.buf + origin->source_end &&
                observations.mad_binary_events[index] >= 2 && !strstr(original.buf, "mad("));
        }
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

static NaturalIfObservations natural_packed_observer(const NaturalIfFixture *fixture) {
    const bool vertex = fixture->program.program_type == DXBC_PROGRAM_TYPE_VERTEX;
    return (NaturalIfObservations){.program = &fixture->program, .track_packed = true,
        .packed_xyz_field = vertex ? 1 : 0, .packed_scalar_field = vertex ? 2 : 1,
        .packed_source_instruction = fixture->condition ? 0 : fixture->condition,
        .condition_instruction = fixture->condition};
}

static bool natural_packed_fixture(NaturalIfFixture *fixture, bool vertex, bool compared,
    bool nonzero, NaturalIfInputLayout layout) {
    if (vertex)
        return natural_if_multiple_outputs_fixture_layout(fixture, true,
            compared ? USIL_OP_LT : USIL_OP_NOP, compared ? 8 : 0, nonzero, layout);
    return natural_if_fixture_init_layout(fixture, 3, 7, nonzero, false, false, false,
        compared ? USIL_OP_LT : USIL_OP_NOP, compared ? 2 : 0,
        UINT32_C(0x3ec00000), NULL, layout);
}

static bool check_natural_packed_projection(const NaturalIfFixture *fixture) {
    const USILProgram *program = &fixture->program;
    const bool vertex = program->program_type == DXBC_PROGRAM_TYPE_VERTEX;
    const int xyz_field = vertex ? 1 : 0, scalar_field = vertex ? 2 : 1;
    const int vector_instruction = vertex ? fixture->then_value : fixture->then_value - 1;
    const DXBCOperand *vector = &program->instructions[vector_instruction].operands[1];
    const USILInstruction *predicate = &program->instructions[fixture->condition ? 0 : fixture->condition];
    const DXBCOperand *scalar = &predicate->operands[predicate->opcode == USIL_OP_IF ? 0 : 1];
    const uint8_t scalar_demand = predicate->opcode == USIL_OP_IF ? 1 :
        usil_operand_destination_lane_mask(&predicate->operands[0]);
    bool packed = false;
    CHECK(program->has_parsed_signature_authority && hlsl_natural_input_layout_supported(program, &packed) && packed);
    CHECK(program->inputs[xyz_field].register_id == program->inputs[scalar_field].register_id &&
        program->inputs[xyz_field].mask == 7 && program->inputs[xyz_field].rw_mask == 7 &&
        program->inputs[scalar_field].mask == 8 && program->inputs[scalar_field].rw_mask == 8);
    HLSLNaturalInputProjection xyz, w;
    CHECK(hlsl_natural_input_projection(program, vector, 7, &xyz) &&
        hlsl_natural_input_projection(program, scalar, scalar_demand, &w));
    CHECK(xyz.packed && xyz.field_index == xyz_field && xyz.field_mask == 7 &&
        xyz.natural_components == 3 && xyz.result_components == 3 &&
        xyz.selected_components[0] == 0 && xyz.selected_components[1] == 1 && xyz.selected_components[2] == 2);
    CHECK(w.packed && w.field_index == scalar_field && w.field_mask == 8 &&
        w.natural_components == 1 && w.result_components == 1 && !w.selected_components[0]);
    CHECK(xyz.register_index == w.register_index && xyz.logical_value_id != w.logical_value_id &&
        xyz.logical_value_id == (HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE | (uint64_t)(unsigned)xyz_field) &&
        w.logical_value_id == (HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE | (uint64_t)(unsigned)scalar_field));
    CHECK(usil_operand_source_component(scalar, 0) == 3 && scalar->register_index_dim == 1 &&
        scalar->index_has_immediate[0] && !scalar->index_representations[0]);
    return true;
}

static bool check_natural_packed_positive(bool vertex, bool compared, bool nonzero,
    NaturalIfInputLayout layout) {
    NaturalIfFixture fixture;
    CHECK(natural_packed_fixture(&fixture, vertex, compared, nonzero, layout));
    CHECK(check_natural_packed_projection(&fixture));
    USILProgram *program = &fixture.program;
    StringBuilder source, selected;
    sb_init(&source); sb_init(&selected);
    HLSLExpressionSourceMap map, selected_map;
    HLSLSourceQualityResult quality, selected_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations observations = natural_packed_observer(&fixture);
    const bool emitted = natural_if_emit(program, &source, &map, &quality, &observations, &diagnostic);
    if (!emitted)
        fprintf(stderr, "Packed natural IF %s/%s/layout %d: %s at %d, phase %d\n",
            vertex ? "vertex" : "pixel", compared ? "BOOL" : "raw", (int)layout,
            hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index, (int)diagnostic.phase);
    CHECK(emitted && map.complete && map.count == (size_t)program->instruction_count &&
        hlsl_expression_source_map_matches(&map, program, source.buf));
    CHECK(quality.stage == program->program_type && quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
        !quality.reasons && quality.counts.inspected_units == 1 && !quality.counts.incomplete_units &&
        !quality.counts.residual_total && !quality.counts.unknown_provenance);
    CHECK(observations.packed_fields_seen == 3 && !observations.wrong_owner &&
        observations.return_events == 1 && (compared ? observations.comparison_events > 0 :
            observations.comparison_events == 0));
    CHECK(strstr(source.buf, vertex ? "float3 normal : NORMAL" : "float3 texcoord0 : TEXCOORD0") &&
        strstr(source.buf, vertex ? "float texcoord0 : TEXCOORD0" : "float texcoord1 : TEXCOORD1"));
    CHECK(!strstr(source.buf, vertex ? "texcoord0.w" : "texcoord1.w") &&
        !strstr(source.buf, "u_xlat") && !strstr(source.buf, "float4 r0"));
    if (compared) {
        CHECK(strstr(source.buf, "const bool dxbc_value_i0 = ") &&
            strstr(source.buf, nonzero ? "[branch] if (dxbc_value_i0)" : "[branch] if (!dxbc_value_i0)"));
        CHECK(!strstr(source.buf, "asuint("));
    } else CHECK(strstr(source.buf, nonzero ? "[branch] if (asuint(" : "[branch] if (!asuint("));
    const uint32_t first_raw = program->instructions[0].source_instruction_index;
    CHECK(first_raw == (uint32_t)(program->signature_declaration_count + 2));
    for (size_t index = 0; index < map.count; ++index)
        CHECK(map.origins[index].instruction_index == (int)index &&
            map.origins[index].source_instruction_index == first_raw + (uint32_t)index);
    /* The portable asset-only route need not request public evidence. Its
     * source must still pass the internal header/body ownership checks. */
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = (outputs & 1u) ? &selected_map : NULL;
        options.source_quality = (outputs & 2u) ? &selected_quality : NULL;
        options.source_quality_pass_index = 3;
        options.source_quality_entry_point_index = 4;
        sb_free(&selected); sb_init(&selected);
        CHECK(hlsl_emit_with_options_diagnostic(program, &selected, NULL, NULL, NULL, &options, &diagnostic) &&
            selected.len == source.len && !strcmp(selected.buf, source.buf));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&map, &selected_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &selected_quality));
    }
    const size_t rejection_points[] = {1, observations.observations / 2u, observations.observations};
    for (size_t point = 0; point < sizeof(rejection_points) / sizeof(*rejection_points); ++point) {
        NaturalIfObservations rejected = natural_packed_observer(&fixture);
        rejected.reject_at = rejection_points[point];
        sb_free(&selected); sb_init(&selected);
        CHECK(!natural_if_emit(program, &selected, &selected_map, &selected_quality, &rejected, &diagnostic) &&
            !selected_map.complete && !selected_map.count && selected_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    }
    NaturalIfObservations restored = natural_packed_observer(&fixture);
    sb_free(&selected); sb_init(&selected);
    CHECK(natural_if_emit(program, &selected, &selected_map, &selected_quality, &restored, &diagnostic) &&
        !strcmp(source.buf, selected.buf) && natural_if_maps_equal(&map, &selected_map) &&
        hlsl_source_quality_results_equal(&quality, &selected_quality));
    sb_free(&selected); sb_free(&source); natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_packed_callback_drift(bool vertex) {
    NaturalIfFixture fixture;
    CHECK(natural_packed_fixture(&fixture, vertex, !vertex, true, NATURAL_IF_INPUTS_PACKED_SPLIT));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count <= 11 && program->input_count <= 3 && program->signature_declaration_count <= 5);
    USILInstruction instructions[11];
    DXBCSignatureElement inputs[3];
    USILSignatureDeclaration declarations[5];
    memcpy(instructions, program->instructions, (size_t)program->instruction_count * sizeof(*instructions));
    memcpy(inputs, program->inputs, (size_t)program->input_count * sizeof(*inputs));
    memcpy(declarations, program->signature_declarations, (size_t)program->signature_declaration_count * sizeof(*declarations));
    StringBuilder original, changed;
    sb_init(&original); sb_init(&changed);
    /* A caller prefix is outside the owned header range and remains intact. */
    const char prefix[] = "// caller prefix\n";
    sb_append(&original, prefix);
    HLSLExpressionSourceMap original_map, changed_map;
    HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations baseline = natural_packed_observer(&fixture);
    baseline.mutable_source = &original;
    CHECK(natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic) &&
        baseline.observations > 2 && baseline.signature_observation &&
        baseline.signature_observation < baseline.observations);
    const char *header_type = strstr(original.buf, "float");
    CHECK(header_type && (size_t)(header_type - original.buf) >= sizeof(prefix) - 1u &&
        (size_t)(header_type - original.buf) < original_map.origins[0].source_begin);
    const size_t mutation_points[] = {1, baseline.observations / 2u, baseline.observations};
    const unsigned mutations[] = {16, 17, 18, 19, 20, 4, 15};
    for (size_t action = 0; action < sizeof(mutations) / sizeof(*mutations); ++action) {
        const unsigned mutation = mutations[action];
        if (vertex && mutation >= 18) continue;
        const bool external = mutation >= 19 || mutation == 4 || mutation == 15;
        for (size_t point = external || vertex ? 2u : 0; point < 3; ++point) {
            NaturalIfObservations drift = natural_packed_observer(&fixture);
            drift.mutable_program = program; drift.mutable_source = &changed; drift.mutable_map = &changed_map;
            drift.mutation = mutation;
            drift.mutate_at = mutation == 19 ? 0 : mutation == 20 ? baseline.signature_observation : mutation_points[point];
            drift.source_offset = (size_t)(header_type - original.buf);
            drift.arithmetic_instruction = fixture.then_value;
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(!natural_if_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
                drift.mutated && diagnostic.status != HLSL_EMIT_STATUS_OK && !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && !strncmp(changed.buf, prefix, sizeof(prefix) - 1u));
            if (mutation == 19 || mutation == 20) CHECK(drift.observations < baseline.observations);
            NaturalIfObservations fresh = natural_packed_observer(&fixture);
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed_map.complete && changed_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                hlsl_expression_source_map_matches(&changed_map, program, changed.buf));
            if (external) CHECK(!strcmp(original.buf, changed.buf));
            else {
                CHECK(strcmp(original.buf, changed.buf));
                if (mutation == 16) CHECK(strstr(changed.buf, "COORDGATE"));
                if (mutation == 17) CHECK(strstr(changed.buf, vertex ? "normal.z" : "texcoord0.z"));
                if (mutation == 18) CHECK(strstr(changed.buf, "centroid float3") && strstr(changed.buf, "centroid float "));
            }
            memcpy(program->instructions, instructions, (size_t)program->instruction_count * sizeof(*instructions));
            memcpy(program->inputs, inputs, (size_t)program->input_count * sizeof(*inputs));
            memcpy(program->signature_declarations, declarations, (size_t)program->signature_declaration_count * sizeof(*declarations));
            fresh = natural_packed_observer(&fixture);
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed.len == original.len && !strcmp(changed.buf, original.buf) &&
                natural_if_maps_equal(&original_map, &changed_map) &&
                hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_packed_boundaries(void) {
    NaturalIfFixture fixture;
    CHECK(natural_packed_fixture(&fixture, false, false, true, NATURAL_IF_INPUTS_PACKED_UNION));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 10 && program->input_count == 2 && program->signature_declaration_count == 2);
    USILInstruction instructions[10];
    DXBCSignatureElement inputs[2];
    USILSignatureDeclaration declarations[2];
    memcpy(instructions, program->instructions, sizeof(instructions));
    memcpy(inputs, program->inputs, sizeof(inputs));
    memcpy(declarations, program->signature_declarations, sizeof(declarations));
    for (unsigned mutation = 0; mutation < 19; ++mutation) {
        DXBCOperand *source = &program->instructions[fixture.then_value - 1].operands[1];
        switch (mutation) {
        case 0: program->inputs[1].mask = program->inputs[1].rw_mask = 4; break; /* overlap */
        case 1: program->inputs[0].mask = program->inputs[0].rw_mask = 3; break; /* hole */
        case 2: /* One vector access crosses the two distinct fields. */
            source->swizzle[2] = 3;
            source->raw_token = natural_if_source_token(OPERAND_TYPE_INPUT, source->swizzle); break;
        case 3: source->index_representations[0] = 2; break;
        case 4: source->register_index = 1; source->index_values[0] = 1; break;
        case 5: program->inputs[1].interpolation_mode = 3; break;
        case 6: program->inputs[1].semantic_index = 0; break; /* ambiguous semantic */
        case 7: program->inputs[1].component_type = 1; break;
        case 8: program->inputs[1].system_value = 1; break;
        case 9: program->inputs[1].min_precision = 1; break;
        case 10: program->inputs[1].rw_mask = 9; break;
        case 11: program->signature_declarations[0].mask = 7; break; /* W undeclared */
        case 12: program->signature_declarations[0].interpolation_mode = 1; break;
        case 13: program->inputs[1].stream_index = 1; break;
        case 14: /* No complete natural IF plan may authorize packed straight-line IO. */
            program->instructions[fixture.condition] = (USILInstruction){.opcode = USIL_OP_NOP,
                .source_instruction_index = instructions[fixture.condition].source_instruction_index};
            program->instructions[3] = (USILInstruction){.opcode = USIL_OP_NOP,
                .source_instruction_index = instructions[3].source_instruction_index};
            program->instructions[6] = (USILInstruction){.opcode = USIL_OP_NOP,
                .source_instruction_index = instructions[6].source_instruction_index}; break;
        case 15: program->inputs[0].rw_mask = 6; break;
        case 16: program->inputs[1].rw_mask = 0; break;
        case 17:
        case 18: {
            DXBCSignatureElement *field = &program->inputs[1];
            const char *name = mutation == 17 ? "PACKED1" : "SV_GATE";
            const size_t length = strlen(name);
            memset(field->semantic_name, 0, sizeof(field->semantic_name));
            memcpy(field->semantic_name, name, length + 1u);
            field->semantic_name_length = length;
            break;
        }
        }
        if (mutation <= 4) {
            HLSLNaturalInputProjection rejected;
            memset(&rejected, 0xa5, sizeof(rejected));
            CHECK(!hlsl_natural_input_projection(program, source, 7, &rejected));
            CHECK(!rejected.field_index && !rejected.register_index && !rejected.field_mask &&
                !rejected.natural_components && !rejected.result_components && !rejected.logical_value_id &&
                !rejected.packed && !rejected.selected_components[0] && !rejected.selected_components[1] &&
                !rejected.selected_components[2] && !rejected.selected_components[3]);
        }
        CHECK(natural_if_rejected(program, NULL));
        memcpy(program->instructions, instructions, sizeof(instructions));
        memcpy(program->inputs, inputs, sizeof(inputs));
        memcpy(program->signature_declarations, declarations, sizeof(declarations));
    }
    CHECK(check_natural_packed_projection(&fixture));
    StringBuilder restored;
    sb_init(&restored);
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    NaturalIfObservations observations = natural_packed_observer(&fixture);
    CHECK(natural_if_emit(program, &restored, &map, &quality, &observations, NULL) &&
        quality.classification == HLSL_SOURCE_QUALITY_CLEAN && map.complete);
    sb_free(&restored); natural_if_fixture_dispose(&fixture);
    /* Existing unshared prefix inputs keep the original register identity. */
    CHECK(natural_if_fixture_init(&fixture, 3, 7, true, false, false, false));
    bool packed = true;
    HLSLNaturalInputProjection unshared;
    CHECK(hlsl_natural_input_layout_supported(&fixture.program, &packed) && !packed &&
        hlsl_natural_input_projection(&fixture.program, &fixture.program.instructions[1].operands[1], 7, &unshared) &&
        !unshared.packed && unshared.field_index == 0 && unshared.logical_value_id == (UINT64_C(1) << 63));
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_packed_inputs(void) {
    CHECK(check_natural_packed_positive(false, false, true, NATURAL_IF_INPUTS_PACKED_UNION));
    CHECK(check_natural_packed_positive(false, true, false, NATURAL_IF_INPUTS_PACKED_SPLIT));
    CHECK(check_natural_packed_positive(true, false, false, NATURAL_IF_INPUTS_PACKED_SPLIT));
    CHECK(check_natural_packed_positive(true, true, true, NATURAL_IF_INPUTS_PACKED_UNION));
    CHECK(check_natural_packed_callback_drift(false));
    CHECK(check_natural_packed_callback_drift(true));
    CHECK(check_natural_packed_boundaries());
    return true;
}

static bool check_natural_mad_owners(NaturalIfFixture *fixture) {
    USILProgram *program = &fixture->program;
    const int owners[] = {fixture->then_value, fixture->else_value, fixture->join_value};
    HLSLEmitterContext ctx;
    CHECK(analyze(&ctx, program));
    for (unsigned owner = 0; owner < 3; ++owner) {
        const int index = owners[owner];
        const USILInstruction *instruction = &program->instructions[index];
        const uint8_t mask = usil_operand_destination_lane_mask(&instruction->operands[0]);
        CHECK(instruction->opcode == USIL_OP_MAD && instruction->operand_count == 4 &&
            usil_instruction_shape_valid(program, instruction));
        const DXBCInstruction *raw = NULL;
        for (int token = 0; token < fixture->semantic.instruction_count; ++token) {
            const DXBCInstruction *candidate = &fixture->semantic.instructions[token];
            if (candidate->has_raw_instruction_index &&
                candidate->raw_instruction_index == instruction->source_instruction_index) {
                CHECK(!raw); raw = candidate;
            }
        }
        CHECK(raw && raw->opcode == 50 && raw->operand_count == 4 && !strcmp(dxbc_opcode_name(raw->opcode), "MAD"));
        for (int operand = 1; operand <= 3; ++operand) {
            const DXBCOperand *source = &instruction->operands[operand];
            USILOperandUseInfo use;
            CHECK(usil_instruction_operand_use(program, instruction, operand, &use) &&
                use.use == USIL_OPERAND_USE_SOURCE && use.source_lane_mask == mask);
            if (source->type == OPERAND_TYPE_TEMP) {
                for (int lane = 0; lane < 4; ++lane) {
                    if (!(mask & (1u << lane))) continue;
                    const int value = variable(&ctx, index, operand, lane);
                    CHECK(value >= 0 && value < ctx.ssa.ssa_var_count &&
                        value != variable(&ctx, index, 0, lane));
                    if (owner < 2) CHECK(ctx.ssa.ssa_var_defs[value] == index - 1);
                    else {
                        CHECK(ctx.ssa.ssa_var_defs[value] == HLSL_DEFINITION_AMBIGUOUS);
                        const int block_index = ctx.cfg.instruction_block[index];
                        CHECK(ctx.cfg.blocks[block_index].predecessor_count == 2);
                        const HLSLBlockPhis *block = &ctx.ssa.block_phis[block_index];
                        const HLSLPhiNode *phi = NULL;
                        for (int candidate = 0; candidate < block->phi_count; ++candidate)
                            if (block->phis[candidate].ssa_var == value) phi = &block->phis[candidate];
                        CHECK(phi && phi->register_index == 0 &&
                            phi->component == usil_operand_source_component(source, lane));
                        const int incoming0 = ctx.ssa.ssa_var_defs[phi->incoming_vars[0]];
                        const int incoming1 = ctx.ssa.ssa_var_defs[phi->incoming_vars[1]];
                        CHECK((incoming0 == fixture->then_value && incoming1 == fixture->else_value) ||
                            (incoming1 == fixture->then_value && incoming0 == fixture->else_value));
                    }
                }
            } else if (source->type == OPERAND_TYPE_INPUT) {
                HLSLNaturalInputProjection projection;
                CHECK(hlsl_natural_input_projection(program, source, use.source_lane_mask, &projection) &&
                    projection.field_index >= 0 && projection.field_index < program->input_count &&
                    projection.field_mask == program->inputs[projection.field_index].mask);
                unsigned demanded_components = 0, register_fields = 0;
                for (unsigned lane = 0; lane < 4; ++lane)
                    if (use.source_lane_mask & (1u << lane)) ++demanded_components;
                for (int field = 0; field < program->input_count; ++field)
                    if (program->inputs[field].register_id == projection.register_index) ++register_fields;
                CHECK(projection.result_components == demanded_components &&
                    projection.packed == (register_fields == 2) &&
                    projection.logical_value_id == (projection.packed ?
                        HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE | (uint64_t)(unsigned)projection.field_index :
                        (UINT64_C(1) << 63) | projection.register_index));
                /* The projection retains each demanded physical lane. The
                 * typed source atom later collapses this repeated scalar. */
                if (projection.field_mask == 8) {
                    CHECK(projection.packed && projection.natural_components == 1);
                    for (unsigned component = 0; component < projection.result_components; ++component)
                        CHECK(!projection.selected_components[component]);
                }
            } else {
                CHECK(source->type == OPERAND_TYPE_IMMEDIATE32 &&
                    (source->immediate_word_count == 1 || source->immediate_word_count == 4) &&
                    source->imm_value_count == source->immediate_word_count &&
                    raw->operands[operand].immediate_word_count == source->immediate_word_count);
                for (int word = 0; word < source->immediate_word_count; ++word)
                    CHECK(source->imm_values[word] == source->immediate_words[word] &&
                        raw->operands[operand].immediate_words[word] == source->immediate_words[word]);
            }
        }
    }
    dispose(&ctx);
    return true;
}

static bool check_natural_mad_callback_drift(void) {
    const NaturalIfArithmetic arithmetic = {.then_opcode = USIL_OP_MAD, .else_opcode = USIL_OP_MAD, .join_opcode = USIL_OP_MAD,
        .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_right = NATURAL_IF_RIGHT_VECTOR_INPUT,
        .arm_third = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_third = NATURAL_IF_RIGHT_VECTOR_INPUT};
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init_arithmetic(&fixture, 2, 12, true, false, false, false,
        USIL_OP_LT, 2, UINT32_C(0x3ec00000), &arithmetic));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 11);
    USILInstruction saved[11]; memcpy(saved, program->instructions, sizeof(saved));
    StringBuilder original, changed; sb_init(&original); sb_init(&changed);
    HLSLExpressionSourceMap original_map, changed_map;
    HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations baseline = {.program = program};
    CHECK(natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic) &&
        original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && baseline.observations > 2);
    const size_t points[] = {1, baseline.observations / 2u, baseline.observations};
    const unsigned mutations[] = {21, 22, 23, 24};
    for (size_t action = 0; action < sizeof(mutations) / sizeof(*mutations); ++action) {
        for (size_t point = 0; point < sizeof(points) / sizeof(*points); ++point) {
            NaturalIfObservations drift = {.program = program, .mutable_program = program,
                .mutation = mutations[action], .mutate_at = points[point],
                .arithmetic_instruction = mutations[action] == 23 ? fixture.join_value : fixture.then_value};
            sb_free(&changed); sb_init(&changed);
            CHECK(!natural_if_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
                drift.mutated && !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && diagnostic.status != HLSL_EMIT_STATUS_OK);
            NaturalIfObservations fresh = {.program = program};
            sb_free(&changed); sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && changed_map.complete &&
                hlsl_expression_source_map_matches(&changed_map, program, changed.buf) && strcmp(original.buf, changed.buf));
            memcpy(program->instructions, saved, sizeof(saved));
            fresh = (NaturalIfObservations){.program = program};
            sb_free(&changed); sb_init(&changed);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                !strcmp(original.buf, changed.buf) && natural_if_maps_equal(&original_map, &changed_map) &&
                hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_mad_rejections(void) {
    const NaturalIfArithmetic arithmetic = {.then_opcode = USIL_OP_MAD, .else_opcode = USIL_OP_MAD, .join_opcode = USIL_OP_MAD,
        .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_right = NATURAL_IF_RIGHT_VECTOR_INPUT,
        .arm_third = NATURAL_IF_RIGHT_VECTOR_TEMP, .join_third = NATURAL_IF_RIGHT_VECTOR_TEMP};
    NaturalIfFixture fixture;
    CHECK(natural_if_fixture_init_arithmetic(&fixture, 2, 12, true, false, false, false,
        USIL_OP_LT, 2, UINT32_C(0x3ec00000), &arithmetic));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 11 && check_natural_mad_owners(&fixture));
    USILInstruction saved[11]; memcpy(saved, program->instructions, sizeof(saved));
    /* Borrow one actual decoded scalar TEMP selector only while checking the
     * relative-index rejection; restoring saved[] detaches it before cleanup. */
    DXBCOperand relative = program->instructions[fixture.condition].operands[0];
    for (unsigned mutation = 0; mutation < 13; ++mutation) {
        USILInstruction *owner = &program->instructions[fixture.then_value];
        DXBCOperand *third = &owner->operands[3];
        switch (mutation) {
        case 0: owner->operand_count = 3; break;
        case 1: third->register_index = 1; third->index_values[0] = 1; break; /* Defined only later. */
        case 2: /* Demand the comparison's actually defined Y, isolating BOOL-to-float misuse. */
            third->register_index = 2; third->index_values[0] = 2;
            memset(third->swizzle, 1, sizeof(third->swizzle));
            third->raw_token &= ~UINT32_C(0x00000ff0);
            for (unsigned lane = 0; lane < 4; ++lane)
                third->raw_token |= (uint32_t)third->swizzle[lane] << (4u + lane * 2u);
            break;
        case 3: third->has_neg = true; break;
        case 4: third->has_abs = true; break;
        case 5: third->min_precision = 1; break;
        case 6: third->type = OPERAND_TYPE_RESOURCE; break;
        case 7: third->type = OPERAND_TYPE_CONSTANT_BUFFER; break;
        case 8: /* The physical TEMP X was never written by the ZW definition. */
            third->swizzle[2] = 0;
            third->raw_token &= ~UINT32_C(0x00000300);
            break;
        case 9: owner->precise_mask = 12; break;
        case 10: owner->saturate = true; break;
        case 11: program->instructions[fixture.else_value].operands[0].destination_mask = 0x40; break;
        case 12:
            third->index_representations[0] = 2; third->index_has_immediate[0] = false;
            third->raw_token = (third->raw_token & ~UINT32_C(0x01c00000)) | UINT32_C(0x00800000);
            third->rel_op0 = &relative;
            break;
        }
        const bool rejected = natural_if_rejected(program, NULL);
        if (!rejected) fprintf(stderr, "Natural MAD rejection mutation=%u owner=%d third_type=%d third_register=%d\n",
            mutation, fixture.then_value, (int)third->type, third->register_index);
        memcpy(program->instructions, saved, sizeof(saved));
        CHECK(rejected);
    }
    CHECK(check_natural_arithmetic_fixture(&fixture, 2, 12, true, true, &arithmetic));
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_mad_emission(void) {
    CHECK(natural_arithmetic_raw_opcode(USIL_OP_MAD) == 50 && !strcmp(dxbc_opcode_name(50), "MAD"));
    const struct {unsigned width; uint8_t mask; bool compared, nonzero;
        NaturalIfRightOperand third; NaturalIfInputLayout layout;} shapes[] = {
        {1, 2, false, true, NATURAL_IF_RIGHT_SCALAR_LITERAL, NATURAL_IF_INPUTS_SEPARATE},
        {2, 12, true, false, NATURAL_IF_RIGHT_VECTOR_LITERAL, NATURAL_IF_INPUTS_SEPARATE},
        {3, 7, false, false, NATURAL_IF_RIGHT_SCALAR_INPUT, NATURAL_IF_INPUTS_SEPARATE},
        {2, 12, true, true, NATURAL_IF_RIGHT_VECTOR_TEMP, NATURAL_IF_INPUTS_SEPARATE},
        {3, 7, true, false, NATURAL_IF_RIGHT_SCALAR_INPUT, NATURAL_IF_INPUTS_PACKED_SPLIT}};
    for (size_t shape = 0; shape < sizeof(shapes) / sizeof(*shapes); ++shape) {
        NaturalIfArithmetic arithmetic = {.then_opcode = USIL_OP_MAD, .else_opcode = USIL_OP_MAD, .join_opcode = USIL_OP_MAD,
            .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL, .join_right = NATURAL_IF_RIGHT_VECTOR_INPUT,
            .arm_third = shapes[shape].third, .join_third = shapes[shape].third};
        if (shape == 0) {
            arithmetic.custom_third_literals = true;
            arithmetic.then_third_bits = UINT32_C(0x80000000);
            arithmetic.else_third_bits = 1;
            arithmetic.join_third_bits = UINT32_C(0x7fc12345);
        }
        if (shapes[shape].layout != NATURAL_IF_INPUTS_SEPARATE)
            arithmetic.join_third = NATURAL_IF_RIGHT_VECTOR_INPUT;
        NaturalIfFixture fixture;
        CHECK(natural_if_fixture_init_layout(&fixture, shapes[shape].width, shapes[shape].mask, shapes[shape].nonzero,
            false, false, false, shapes[shape].compared ? USIL_OP_LT : USIL_OP_NOP, shapes[shape].compared ? 8 : 0,
            UINT32_C(0x3ec00000), &arithmetic, shapes[shape].layout) && check_natural_mad_owners(&fixture));
        if (shapes[shape].layout != NATURAL_IF_INPUTS_SEPARATE) CHECK(check_natural_packed_projection(&fixture));
        CHECK(check_natural_arithmetic_fixture(&fixture, shapes[shape].width, shapes[shape].mask,
            shapes[shape].nonzero, shapes[shape].compared, &arithmetic));
        natural_if_fixture_dispose(&fixture);
    }
    CHECK(check_natural_mad_rejections());
    CHECK(check_natural_mad_callback_drift());
    return true;
}

/* Unique-input headers need the same immutable byte ownership as packed
 * headers. These existing parsed fixtures isolate header stability from any
 * new operation admission; callbacks alter only the caller's source or map. */
static bool check_natural_unique_header_callbacks(void) {
    const NaturalIfArithmetic arithmetic = {.then_opcode = USIL_OP_MAD, .else_opcode = USIL_OP_MAD,
        .join_opcode = USIL_OP_MAD, .arm_right = NATURAL_IF_RIGHT_SCALAR_LITERAL,
        .join_right = NATURAL_IF_RIGHT_VECTOR_INPUT, .arm_third = NATURAL_IF_RIGHT_SCALAR_LITERAL,
        .join_third = NATURAL_IF_RIGHT_VECTOR_INPUT};
    bool all_rejected = true;
    for (unsigned vertex = 0; vertex < 2u; ++vertex) {
        NaturalIfFixture fixture;
        CHECK(vertex ? natural_if_multiple_outputs_fixture_mode(&fixture, true, USIL_OP_LT, 8, true)
            : natural_if_fixture_init_arithmetic(&fixture, 2, 12, true, false, false, false,
                USIL_OP_LT, 2, UINT32_C(0x3ec00000), &arithmetic));
        USILProgram *program = &fixture.program;
        bool packed = true;
        CHECK(program->has_parsed_signature_authority && usil_signature_authority_is_valid(program) &&
            hlsl_natural_input_layout_supported(program, &packed) && !packed &&
            program->instruction_count <= 11 && program->input_count <= 3 && program->signature_declaration_count <= 5);
        const USILProgram model = *program;
        USILInstruction instructions[11]; DXBCSignatureElement inputs[3]; USILSignatureDeclaration declarations[5];
        memcpy(instructions, program->instructions, (size_t)program->instruction_count * sizeof(*instructions));
        memcpy(inputs, program->inputs, (size_t)program->input_count * sizeof(*inputs));
        memcpy(declarations, program->signature_declarations,
            (size_t)program->signature_declaration_count * sizeof(*declarations));
        const char prefix[] = "// caller prefix\n";
        StringBuilder original, changed; sb_init(&original); sb_init(&changed); sb_append(&original, prefix);
        HLSLExpressionSourceMap original_map, changed_map;
        HLSLSourceQualityResult original_quality, changed_quality; HLSLEmitDiagnostic diagnostic;
        NaturalIfObservations baseline = {.program = program, .mutable_source = &original};
        CHECK(natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic) &&
            original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !original_quality.reasons &&
            !baseline.wrong_owner && baseline.first_preheader_observation == 1u &&
            baseline.first_preheader_observation < baseline.first_header_observation &&
            baseline.first_header_observation < baseline.signature_observation && baseline.signature_observation < baseline.observations &&
            original_map.complete && hlsl_expression_source_map_matches(&original_map, program, original.buf));
        const char *type = strstr(original.buf, "float");
        CHECK(type && (size_t)(type - original.buf) >= sizeof(prefix) - 1u &&
            (size_t)(type - original.buf) < original_map.origins[0].source_begin);
        const size_t header_offset = (size_t)(type - original.buf);
        const char *comment = strstr(original.buf + sizeof(prefix) - 1u, "// Translated by Codex C-emitter\n");
        CHECK(comment && comment[3] == 'T' && (size_t)(comment - original.buf) + 3u < header_offset);
        const size_t generated_comment_offset = (size_t)(comment - original.buf) + 3u;
        CHECK(fixture.then_value >= 0 && (size_t)fixture.then_value < original_map.count &&
            original_map.origins[fixture.then_value].source_begin < original_map.origins[fixture.then_value].source_end);
        for (unsigned outputs = 0; outputs < 4u; ++outputs) {
            HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
            options.expression_source_map = outputs & 1u ? &changed_map : NULL;
            options.source_quality = outputs & 2u ? &changed_quality : NULL;
            options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(hlsl_emit_with_options_diagnostic(program, &changed, NULL, NULL, NULL, &options, &diagnostic) &&
                changed.len == original.len && !memcmp(changed.buf, original.buf, original.len));
            if (outputs & 1u) CHECK(natural_if_maps_equal(&original_map, &changed_map));
            if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
        /* Complete UNIT_BEGIN itself has no observer event. The first real
         * callback is the preheader configuration emission recorded above.
         * An empty caller builder must use the same guarded header/body path. */
        HLSLEmitOptions empty_options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        empty_options.expression_source_map = &changed_map;
        empty_options.source_quality = &changed_quality;
        empty_options.source_quality_pass_index = 3; empty_options.source_quality_entry_point_index = 4;
        sb_free(&changed); sb_init(&changed);
        CHECK(hlsl_emit_with_options_diagnostic(program, &changed, NULL, NULL, NULL, &empty_options, &diagnostic) &&
            changed.len == original.len - (sizeof(prefix) - 1u) &&
            !memcmp(changed.buf, original.buf + sizeof(prefix) - 1u, changed.len) &&
            hlsl_source_quality_results_equal(&original_quality, &changed_quality) &&
            changed_map.complete && hlsl_expression_source_map_matches(&changed_map, program, changed.buf));
        CHECK(hlsl_expression_source_map_offset(&changed_map, sizeof(prefix) - 1u) &&
            natural_if_maps_equal(&original_map, &changed_map));
        const char *labels[] = {"early-header-type", "last-header-gap", "final-header-type",
            "final-body-byte", "final-map-range", "final-caller-prefix", "early-caller-prefix",
            "first-preheader-caller-prefix", "first-preheader-declaration-insertion", "first-preheader-comment-byte"};
        for (unsigned attack = 0; attack < sizeof(labels) / sizeof(*labels); ++attack) {
            NaturalIfObservations drift = {.program = program, .mutable_program = program, .mutable_source = &changed,
                .mutable_map = &changed_map, .mutation = attack == 0u ? 19u : attack == 1u ? 20u :
                    attack == 4u ? 15u : attack == 8u ? 25u : 4u,
                .mutate_at = attack == 0u ? 0u : attack == 1u ? baseline.signature_observation :
                    attack == 6u ? baseline.first_header_observation :
                    attack >= 7u ? baseline.first_preheader_observation : baseline.observations,
                .source_offset = attack == 3u ? original_map.origins[fixture.then_value].source_begin :
                    attack == 9u ? generated_comment_offset : attack >= 5u ? 0u : header_offset,
                .arithmetic_instruction = fixture.then_value};
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            const bool emitted = natural_if_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic);
            const bool rejected = drift.mutated && !emitted && changed.failed && !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && diagnostic.status != HLSL_EMIT_STATUS_OK;
            if (!rejected) fprintf(stderr,
                "unique header guard stage=%s attack=%s emitted=%d mutated=%d quality=%d map_complete=%d map_count=%zu status=%s\n",
                vertex ? "vertex" : "pixel", labels[attack], emitted, drift.mutated, (int)changed_quality.classification,
                changed_map.complete, changed_map.count, hlsl_emit_status_name(diagnostic.status));
            all_rejected = all_rejected && rejected;
            if (rejected && (attack < 2u || attack >= 6u)) CHECK(drift.observations < baseline.observations);
            if (attack < 5u || attack >= 8u) CHECK(changed.len >= sizeof(prefix) - 1u &&
                !memcmp(changed.buf, prefix, sizeof(prefix) - 1u));
            CHECK(!memcmp(program, &model, sizeof(model)) &&
                !memcmp(program->instructions, instructions, (size_t)program->instruction_count * sizeof(*instructions)) &&
                !memcmp(program->inputs, inputs, (size_t)program->input_count * sizeof(*inputs)) &&
                !memcmp(program->signature_declarations, declarations,
                    (size_t)program->signature_declaration_count * sizeof(*declarations)));
            /* A failed ordinary V/F builder can retain partial source. Fresh
             * emission, rather than an invented truncation contract, proves
             * the original full source and evidence are restored exactly. */
            NaturalIfObservations restored = {.program = program};
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &restored, &diagnostic) &&
                changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
                natural_if_maps_equal(&original_map, &changed_map) &&
                hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
        sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    }
    CHECK(all_rejected);
    return true;
}

typedef struct {
    unsigned width, result_lane;
    NaturalIfInputLayout layout;
    bool compared, nonzero, broadcast;
} NaturalDotShape;

typedef struct {
    uint8_t then_input, add_input, add_literal, else_temp, output_temp;
    uint8_t comparison_input, condition_source, then_destination;
    bool exceptional_literals;
} NaturalDotModifiers;

/* DPn reduces a fixed source tuple into one real physical TEMP lane. Its
 * input width, scalar result and final FLOAT3 output are independent. Keep
 * this author separate so previous arithmetic fixtures retain their bytes. */
static bool natural_dot_fixture_modifiers(NaturalIfFixture *fixture, NaturalDotShape shape,
    const NaturalDotModifiers *modifiers) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document); dxbc_stage_contract_init(&fixture->contract);
    CHECK(shape.width >= 2 && shape.width <= 4 && shape.result_lane < 4 &&
        shape.layout <= NATURAL_IF_INPUTS_PACKED_SPLIT &&
        (shape.layout == NATURAL_IF_INPUTS_SEPARATE || shape.width == 2));
    const bool packed = shape.layout != NATURAL_IF_INPUTS_SEPARATE;
    const uint8_t width_mask = (uint8_t)((1u << shape.width) - 1u);
    const uint8_t result_mask = (uint8_t)(1u << shape.result_lane);
    const uint8_t add_mask = shape.width == 2 ? 6 : shape.width == 3 ? 14 : 15;
    const uint32_t right_register = packed ? 0u : 1u, gate_register = packed ? 1u : 2u;
    const uint32_t add_register = shape.width == 4 ? 1u : 0u;
    const NaturalDotModifiers plain = {0};
    const NaturalDotModifiers *mods = modifiers ? modifiers : &plain;
    CHECK(mods->then_input <= 3 && mods->add_input <= 3 && mods->add_literal <= 3 &&
        mods->else_temp <= 3 && mods->output_temp <= 3 && mods->comparison_input <= 3 &&
        mods->condition_source <= 3 && mods->then_destination <= 3);
    const uint8_t left[] = {0, 1, 2, (uint8_t)(shape.width == 4 ? 3 : 0)};
    const uint8_t right[] = {(uint8_t)(packed ? 2 : 0), (uint8_t)(packed ? 3 : 1),
        (uint8_t)(shape.width >= 3 ? 2 : packed ? 2 : 0), (uint8_t)(shape.width == 4 ? 3 : packed ? 2 : 0)};
    const uint8_t add_input[] = {(uint8_t)(packed ? 2 : 0), (uint8_t)(shape.width == 4 ? 1 : packed ? 2 : 0),
        (uint8_t)(shape.width == 2 ? (packed ? 3 : 1) : shape.width == 3 ? 1 : 2),
        (uint8_t)(shape.width == 4 ? 3 : shape.width == 3 ? 2 : packed ? 2 : 0)};
    const uint8_t add_temp[] = {(uint8_t)(shape.width == 4 ? 0 : 1), (uint8_t)(shape.width == 4 ? 1 : 2),
        (uint8_t)(shape.width == 3 ? 3 : shape.width == 4 ? 2 : 1), (uint8_t)(shape.width == 4 ? 3 : 1)};
    uint8_t right_dot[4], temp_dot[4], result[4];
    memcpy(right_dot, right, sizeof(right)); memcpy(temp_dot, add_temp, sizeof(add_temp));
    for (unsigned lane = 0; lane < 4; ++lane) {
        result[lane] = (uint8_t)shape.result_lane;
        if (shape.broadcast) { right_dot[lane] = right[0]; temp_dot[lane] = add_temp[0]; }
    }
    uint32_t words[128]; size_t count = 0;
#define DOT_WORD(value) do { CHECK(count < sizeof(words) / sizeof(*words)); words[count++] = (value); } while (0)
#define DOT_INST(opcode, length) ((uint32_t)(opcode) | (uint32_t)(length) << 24u)
#define DOT_DEST(type, mask) (UINT32_C(0x00100002) | (uint32_t)(type) << 12u | (uint32_t)(mask) << 4u)
#define DOT_OPERAND(token, modifier) do { \
    DOT_WORD((token) | ((modifier) ? UINT32_C(0x80000000) : 0)); \
    if (modifier) DOT_WORD(UINT32_C(1) | (uint32_t)(modifier) << 6u); \
} while (0)
    DOT_WORD(DOT_INST(106, 1) | 1u << 11u);
    DOT_WORD(DOT_INST(98, 3) | 2u << 11u);
    DOT_WORD(DOT_DEST(OPERAND_TYPE_INPUT, shape.layout == NATURAL_IF_INPUTS_PACKED_UNION ? 15 : width_mask)); DOT_WORD(0);
    if (shape.layout != NATURAL_IF_INPUTS_PACKED_UNION) {
        DOT_WORD(DOT_INST(98, 3) | 2u << 11u);
        DOT_WORD(DOT_DEST(OPERAND_TYPE_INPUT, packed ? 12 : width_mask)); DOT_WORD(right_register);
    }
    DOT_WORD(DOT_INST(98, 3) | 1u << 11u); DOT_WORD(DOT_DEST(OPERAND_TYPE_INPUT, 1)); DOT_WORD(gate_register);
    DOT_WORD(DOT_INST(101, 3)); DOT_WORD(DOT_DEST(OPERAND_TYPE_OUTPUT, 7)); DOT_WORD(0);
    DOT_WORD(DOT_INST(104, 2)); DOT_WORD(shape.width == 4 ? 2 : 1);
    if (shape.compared) {
        DOT_WORD(DOT_INST(49, 7u + (mods->comparison_input != 0))); DOT_WORD(DOT_DEST(OPERAND_TYPE_TEMP, 1)); DOT_WORD(0);
        DOT_OPERAND(UINT32_C(0x0010100a), mods->comparison_input); DOT_WORD(gate_register);
        DOT_WORD(UINT32_C(0x00004001)); DOT_WORD(UINT32_C(0x3ec00000));
    }
    DOT_WORD(DOT_INST(31, 3u + (mods->condition_source != 0)) | (shape.nonzero ? UINT32_C(0x40000) : 0));
    DOT_OPERAND(shape.compared ? UINT32_C(0x0010000a) : UINT32_C(0x0010100a), mods->condition_source);
    DOT_WORD(shape.compared ? 0u : gate_register);
    DOT_WORD(DOT_INST(13u + shape.width, 7u + (mods->then_input != 0) + (mods->then_destination != 0)));
    DOT_OPERAND(DOT_DEST(OPERAND_TYPE_TEMP, result_mask), mods->then_destination); DOT_WORD(0);
    DOT_OPERAND(natural_if_source_token(OPERAND_TYPE_INPUT, left), mods->then_input); DOT_WORD(0);
    DOT_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, right_dot)); DOT_WORD(right_register);
    DOT_WORD(DOT_INST(18, 1));
    DOT_WORD(DOT_INST(0, 10u + (mods->add_input != 0) + (mods->add_literal != 0)));
    DOT_WORD(DOT_DEST(OPERAND_TYPE_TEMP, add_mask)); DOT_WORD(add_register);
    DOT_OPERAND(natural_if_source_token(OPERAND_TYPE_INPUT, add_input), mods->add_input); DOT_WORD(right_register);
    DOT_OPERAND(UINT32_C(0x00004002), mods->add_literal);
    const uint32_t exceptional[] = {UINT32_C(0x7fc12345), UINT32_C(0x80000000), 1, UINT32_C(0xffc12345)};
    for (unsigned lane = 0; lane < 4; ++lane)
        DOT_WORD(mods->exceptional_literals ? exceptional[lane] : add_mask & (1u << lane) ? UINT32_C(0x3e800000) : 0);
    DOT_WORD(DOT_INST(13u + shape.width, 7u + (mods->else_temp != 0)));
    DOT_WORD(DOT_DEST(OPERAND_TYPE_TEMP, result_mask)); DOT_WORD(0);
    DOT_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, left)); DOT_WORD(0);
    DOT_OPERAND(natural_if_source_token(OPERAND_TYPE_TEMP, temp_dot), mods->else_temp); DOT_WORD(add_register);
    DOT_WORD(DOT_INST(21, 1));
    DOT_WORD(DOT_INST(54, 5u + (mods->output_temp != 0))); DOT_WORD(DOT_DEST(OPERAND_TYPE_OUTPUT, 7)); DOT_WORD(0);
    DOT_OPERAND(natural_if_source_token(OPERAND_TYPE_TEMP, result), mods->output_temp); DOT_WORD(0);
    DOT_WORD(DOT_INST(62, 1));
#undef DOT_DEST
#undef DOT_INST
#undef DOT_WORD
#undef DOT_OPERAND
    const DXBCSignatureElement inputs[] = {
        {.semantic_name = "TEXCOORD", .component_type = 3, .mask = width_mask, .rw_mask = width_mask},
        {.semantic_name = "TEXCOORD", .semantic_index = 1, .register_id = right_register,
            .component_type = 3, .mask = (uint8_t)(packed ? 12 : width_mask), .rw_mask = (uint8_t)(packed ? 12 : width_mask)},
        {.semantic_name = "TEXCOORD", .semantic_index = 2, .register_id = gate_register,
            .component_type = 3, .mask = 1, .rw_mask = 1}};
    const DXBCSignatureElement output = {.semantic_name = "SV_Target", .system_value = 64,
        .component_type = 3, .mask = 7, .rw_mask = 8};
    CHECK(natural_if_fixture_decode_words(fixture, UINT32_C(0x00000050), words, count, inputs, 3, &output, 1));
    fixture->condition = shape.compared ? 1 : 0;
    fixture->then_value = fixture->condition + 1; fixture->else_value = fixture->condition + 4;
    fixture->join_value = fixture->output = fixture->condition + 6;
    CHECK(fixture->program.instruction_count == fixture->condition + 8 &&
        fixture->program.signature_declaration_count == (shape.layout == NATURAL_IF_INPUTS_PACKED_UNION ? 3 : 4));
    return true;
}

static bool natural_dot_fixture(NaturalIfFixture *fixture, NaturalDotShape shape) {
    return natural_dot_fixture_modifiers(fixture, shape, NULL);
}

static bool check_natural_dot_owners(NaturalIfFixture *fixture, NaturalDotShape shape) {
    USILProgram *program = &fixture->program;
    HLSLEmitterContext ctx;
    CHECK(analyze(&ctx, program));
    const int dots[] = {fixture->then_value, fixture->else_value};
    const uint8_t source_mask = (uint8_t)((1u << shape.width) - 1u);
    const uint8_t destination_mask = (uint8_t)(1u << shape.result_lane);
    const int add = fixture->else_value - 1;
    const uint8_t add_mask = shape.width == 2 ? 6 : shape.width == 3 ? 14 : 15;
    CHECK(program->instructions[add].opcode == USIL_OP_ADD &&
        usil_operand_destination_lane_mask(&program->instructions[add].operands[0]) == add_mask &&
        program->temp_count == (shape.width == 4 ? 2 : 1));
    for (unsigned item = 0; item < 2; ++item) {
        const int index = dots[item];
        const USILInstruction *instruction = &program->instructions[index];
        const USILOpcode expected = shape.width == 2 ? USIL_OP_DP2 : shape.width == 3 ? USIL_OP_DP3 : USIL_OP_DP4;
        CHECK(instruction->opcode == expected && instruction->operand_count == 3 &&
            usil_instruction_shape_valid(program, instruction) &&
            usil_operand_destination_lane_mask(&instruction->operands[0]) == destination_mask);
        const DXBCInstruction *raw = NULL;
        for (int token = 0; token < fixture->semantic.instruction_count; ++token) {
            const DXBCInstruction *candidate = &fixture->semantic.instructions[token];
            if (candidate->has_raw_instruction_index && candidate->raw_instruction_index == instruction->source_instruction_index) {
                CHECK(!raw); raw = candidate;
            }
        }
        CHECK(raw && raw->opcode == 13u + shape.width && raw->operand_count == 3);
        for (int operand = 1; operand < 3; ++operand) {
            const DXBCOperand *source = &instruction->operands[operand];
            USILOperandUseInfo use;
            CHECK(usil_instruction_operand_use(program, instruction, operand, &use) &&
                use.use == USIL_OPERAND_USE_SOURCE && use.source_lane_mask == source_mask);
            if (source->type == OPERAND_TYPE_TEMP) {
                CHECK(item == 1 && operand == 2);
                for (unsigned lane = 0; lane < shape.width; ++lane) {
                    const int component = usil_operand_source_component(source, (int)lane);
                    const int value = variable(&ctx, index, operand, (int)lane);
                    CHECK(component >= 0 && (add_mask & (1u << (unsigned)component)) && value >= 0 &&
                        value < ctx.ssa.ssa_var_count && ctx.ssa.ssa_var_defs[value] == add);
                }
            } else {
                HLSLNaturalInputProjection projection;
                CHECK(source->type == OPERAND_TYPE_INPUT &&
                    hlsl_natural_input_projection(program, source, source_mask, &projection) &&
                    projection.field_index == operand - 1 && projection.natural_components == shape.width &&
                    projection.result_components == shape.width &&
                    projection.field_mask == program->inputs[operand - 1].mask &&
                    projection.logical_value_id == (projection.packed ?
                        HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE | (uint64_t)(unsigned)(operand - 1) :
                        (UINT64_C(1) << 63) | (uint64_t)(unsigned)source->register_index));
                for (unsigned lane = 0; lane < shape.width; ++lane)
                    CHECK(projection.selected_components[lane] == (shape.broadcast && operand == 2 ? 0u : lane));
                if (shape.layout != NATURAL_IF_INPUTS_SEPARATE && operand == 2)
                    CHECK(projection.packed && projection.field_mask == 12 && source->swizzle[0] == 2);
            }
        }
    }
    const int output = fixture->output;
    USILOperandUseInfo output_use;
    CHECK(program->instructions[output].opcode == USIL_OP_MOV &&
        usil_operand_destination_lane_mask(&program->instructions[output].operands[0]) == 7 &&
        usil_instruction_operand_use(program, &program->instructions[output], 1, &output_use) && output_use.source_lane_mask == 7);
    int phi_variable = -1;
    for (int lane = 0; lane < 3; ++lane) {
        const int value = variable(&ctx, output, 1, lane);
        CHECK(value >= 0 && value < ctx.ssa.ssa_var_count &&
            program->instructions[output].operands[1].swizzle[lane] == shape.result_lane &&
            ctx.ssa.ssa_var_defs[value] == HLSL_DEFINITION_AMBIGUOUS && (phi_variable < 0 || value == phi_variable));
        phi_variable = value;
    }
    const int block_index = ctx.cfg.instruction_block[output];
    CHECK(block_index >= 0 && block_index < ctx.cfg.block_count &&
        ctx.cfg.blocks[block_index].predecessor_count == 2);
    const HLSLBlockPhis *block = &ctx.ssa.block_phis[block_index];
    const HLSLPhiNode *phi = NULL;
    unsigned unused_undefined = 0;
    for (int item = 0; item < block->phi_count; ++item) {
        const HLSLPhiNode *candidate = &block->phis[item];
        CHECK(candidate->incoming_vars && candidate->incoming_blocks);
        if (candidate->ssa_var == phi_variable) phi = candidate;
        if (shape.width < 4 && candidate->register_index == 0 && candidate->component != (int)shape.result_lane) {
            for (int edge = 0; edge < 2; ++edge) {
                const int value = candidate->incoming_vars[edge];
                CHECK(value < ctx.ssa.ssa_var_count);
                if (value < 0 || ctx.ssa.ssa_var_defs[value] == HLSL_DEFINITION_UNKNOWN) ++unused_undefined;
            }
        }
    }
    CHECK(phi && phi->register_index == 0 && phi->component == (int)shape.result_lane &&
        ctx.cfg.blocks[block_index].predecessor_count == 2);
    CHECK(phi->incoming_vars[0] >= 0 && phi->incoming_vars[0] < ctx.ssa.ssa_var_count &&
        phi->incoming_vars[1] >= 0 && phi->incoming_vars[1] < ctx.ssa.ssa_var_count);
    const int incoming0 = ctx.ssa.ssa_var_defs[phi->incoming_vars[0]], incoming1 = ctx.ssa.ssa_var_defs[phi->incoming_vars[1]];
    CHECK((incoming0 == dots[0] && incoming1 == dots[1]) || (incoming0 == dots[1] && incoming1 == dots[0]));
    if (shape.width < 4) CHECK(unused_undefined);
    dispose(&ctx);
    return true;
}

/* This is the existing consuming factory fed by actual decoded INPUT atoms.
 * It checks owned child order/type rather than inferring a tree from spelling. */
static bool natural_dot_owned_tree(NaturalIfFixture *fixture, NaturalDotShape shape, ASTExpr **owned) {
    HLSLEmitterContext ctx; StringBuilder source; HLSLEmitDiagnostic diagnostic;
    CHECK(analyze(&ctx, &fixture->program)); sb_init(&source); hlsl_emit_diagnostic_init(&diagnostic);
    ctx.emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    ctx.sb = &source; ctx.diagnostic = &diagnostic; ctx.entry_point_name = "main";
    ctx.preferred_input_struct_name = "appdata"; ctx.preferred_output_struct_name = "v2f";
    CHECK(hlsl_natural_structured_preflight(&ctx));
    ctx.high_level_interface = true;
    CHECK(hlsl_prepare_high_level_interface(&ctx));
    const int index = fixture->then_value;
    const uint8_t mask = (uint8_t)((1u << shape.width) - 1u);
    ctx.current_instruction_index = index;
    ASTExpr *left = hlsl_natural_source_atom(&ctx, index, 1, mask);
    ASTExpr *right = hlsl_natural_source_atom(&ctx, index, 2, mask);
    CHECK(left && right && left != right && left->kind == AST_EXPR_EMITTER_OPERAND && right->kind == AST_EXPR_EMITTER_OPERAND);
    const ASTOperandProvenance left_origin = left->operand_provenance, right_origin = right->operand_provenance;
    CHECK(left_origin.complete && right_origin.complete && left_origin.natural_components == shape.width &&
        right_origin.natural_components == shape.width && left_origin.result_components == shape.width &&
        right_origin.result_components == (shape.broadcast ? 1u : shape.width) &&
        left_origin.instruction_index == index && right_origin.instruction_index == index &&
        left_origin.operand_index == 1 && right_origin.operand_index == 2 &&
        left_origin.source_instruction_index == fixture->program.instructions[index].source_instruction_index &&
        right_origin.source_instruction_index == left_origin.source_instruction_index &&
        left_origin.destination_lanes == mask && right_origin.destination_lanes == mask);
    ASTExpr *root = hlsl_natural_float_operation(&ctx, index, left, right, NULL);
    CHECK(root && root->kind == AST_EXPR_CALL && !strcmp(root->u.call.name, "dot") && root->u.call.arg_count == 2 &&
        root->u.call.args[0] == left && root->logical_origin.complete &&
        root->logical_origin.scalar_type == AST_SCALAR_FLOAT32 && root->logical_origin.components == 1 &&
        root->logical_origin.instruction_index == index && root->logical_origin.source_instruction_index == left_origin.source_instruction_index &&
        root->logical_origin.destination_lanes == (uint8_t)(1u << shape.result_lane));
    if (shape.broadcast) {
        const ASTExpr *cast = root->u.call.args[1];
        CHECK(cast->kind == AST_EXPR_CAST && cast->u.cast.sub == right && cast->logical_origin.complete &&
            cast->logical_origin.scalar_type == AST_SCALAR_FLOAT32 && cast->logical_origin.components == shape.width &&
            !strcmp(cast->u.cast.type_name, shape.width == 2 ? "float2" : shape.width == 3 ? "float3" : "float4"));
    } else CHECK(root->u.call.args[1] == right);
    if (shape.width == 3 && !shape.broadcast) {
        /* Both children come from the same decoded DP3 sources. A truncated
         * FLOAT2 argument cannot silently select a different dot overload;
         * the consuming factory owns cleanup on this failure. */
        ASTExpr *complete = hlsl_natural_source_atom(&ctx, index, 1, mask);
        ASTExpr *truncated = hlsl_natural_source_atom(&ctx, index, 2, 3);
        CHECK(complete && truncated && truncated->kind == AST_EXPR_EMITTER_OPERAND &&
            truncated->operand_provenance.complete && truncated->operand_provenance.result_components == 2);
        CHECK(!hlsl_natural_float_operation(&ctx, index, complete, truncated, NULL));
    }
    char left_text[128], right_text[128];
    CHECK(strlen(left->u.emitter_operand) < sizeof(left_text) && strlen(right->u.emitter_operand) < sizeof(right_text));
    memcpy(left_text, left->u.emitter_operand, strlen(left->u.emitter_operand) + 1u);
    memcpy(right_text, right->u.emitter_operand, strlen(right->u.emitter_operand) + 1u);
    memset(ctx.high_level_input_names, 0, sizeof(ctx.high_level_input_names));
    CHECK(!strcmp(left->u.emitter_operand, left_text) && !strcmp(right->u.emitter_operand, right_text));
    dispose(&ctx); sb_free(&source); *owned = root;
    return true;
}

typedef struct {
    NaturalIfObservations common;
    uint8_t result_mask, packed_fields;
    uint64_t dot_calls;
    uint64_t scalar_phi_ids;
    size_t scalar_phi_events;
} NaturalDotObservations;

static bool observe_natural_dot(void *context, const HLSLSourceQualityObservation *observation) {
    NaturalDotObservations *ledger = context;
    if (!observe_natural_if(&ledger->common, observation)) return false;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (facts->known && facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL) {
        for (unsigned field = 0; field < 2; ++field)
            if (facts->logical_value_id == (HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE | field)) {
                ledger->packed_fields |= (uint8_t)(1u << field);
                if (facts->components != 2 && facts->components != 1) ledger->common.wrong_owner = true;
            }
        if (observation->ast_kind == AST_EXPR_VAR && facts->instruction_index < 0 &&
            (facts->logical_value_id >> 16u) == (UINT64_C(0x8000000000060000) >> 16u)) {
            ++ledger->scalar_phi_events;
            if (facts->components != 1) ledger->common.wrong_owner = true;
            const uint64_t ordinal = facts->logical_value_id & UINT64_C(0xffff);
            if (ordinal < 64) ledger->scalar_phi_ids |= UINT64_C(1) << (unsigned)ordinal;
            else ledger->common.wrong_owner = true;
        }
        if (observation->ast_kind == AST_EXPR_CALL && facts->instruction_index >= 0 &&
            facts->instruction_index < ledger->common.program->instruction_count) {
            const USILInstruction *owner = &ledger->common.program->instructions[facts->instruction_index];
            if (owner->opcode == USIL_OP_DP2 || owner->opcode == USIL_OP_DP3 || owner->opcode == USIL_OP_DP4) {
                if (facts->instruction_index >= 64 || facts->components != 1 || facts->lanes != ledger->result_mask ||
                    facts->source_instruction_index != owner->source_instruction_index) ledger->common.wrong_owner = true;
                else ledger->dot_calls |= UINT64_C(1) << (unsigned)facts->instruction_index;
            }
        }
    }
    return true;
}

static bool natural_dot_emit(USILProgram *program, StringBuilder *source, HLSLExpressionSourceMap *map,
    HLSLSourceQualityResult *quality, NaturalDotObservations *observations, HLSLEmitDiagnostic *diagnostic) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = map; options.source_quality = quality;
    options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
    options.source_quality_observer = observe_natural_dot; options.source_quality_observer_context = observations;
    return hlsl_emit_with_options_diagnostic(program, source, NULL, NULL, NULL, &options, diagnostic);
}

static NaturalDotObservations natural_dot_observer(USILProgram *program, NaturalDotShape shape) {
    NaturalDotObservations result = {.common = {.program = program},
        .result_mask = (uint8_t)(1u << shape.result_lane)};
    return result;
}

static bool check_natural_dot_positive(NaturalDotShape shape) {
    NaturalIfFixture fixture; ASTExpr *owned = NULL;
    CHECK(natural_dot_fixture(&fixture, shape) && check_natural_dot_owners(&fixture, shape) &&
        natural_dot_owned_tree(&fixture, shape, &owned));
    USILProgram *program = &fixture.program;
    StringBuilder original, changed; sb_init(&original); sb_init(&changed);
    HLSLExpressionSourceMap original_map, changed_map;
    HLSLSourceQualityResult original_quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    NaturalDotObservations baseline = natural_dot_observer(program, shape);
    const bool emitted = natural_dot_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic);
    if (!emitted) fprintf(stderr, "Natural DP%u result_lane=%u layout=%d: status=%s reason=%s instruction=%d raw=%u\n",
        shape.width, shape.result_lane, (int)shape.layout, hlsl_emit_status_name(diagnostic.status),
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index, diagnostic.source_instruction_index);
    CHECK(emitted && original_map.complete && original_map.count == (size_t)program->instruction_count &&
        hlsl_expression_source_map_matches(&original_map, program, original.buf) &&
        original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !original_quality.reasons &&
        original_quality.counts.inspected_units == 1 && !original_quality.counts.incomplete_units &&
        !original_quality.counts.residual_total && !original_quality.counts.unknown_provenance &&
        !baseline.common.wrong_owner && baseline.scalar_phi_events && baseline.common.observations > 2 &&
        baseline.dot_calls == ((UINT64_C(1) << (unsigned)fixture.then_value) | (UINT64_C(1) << (unsigned)fixture.else_value)));
    CHECK((baseline.common.comparison_events != 0) == shape.compared);
    if (shape.layout != NATURAL_IF_INPUTS_SEPARATE) CHECK(baseline.packed_fields == 3);
    if (shape.compared) CHECK(strstr(original.buf, "const bool dxbc_value_i0 = ") && !strstr(original.buf, "asuint("));
    else CHECK(strstr(original.buf, shape.nonzero ? "[branch] if (asuint(" : "[branch] if (!asuint("));
    const HLSLExpressionOrigin *control = &original_map.origins[fixture.condition];
    CHECK(control->kind == HLSL_EXPRESSION_ORIGIN_CONTROL && !control->destination_lanes &&
        (memchr(original.buf + control->source_begin, '!', control->source_end - control->source_begin) != NULL) == !shape.nonzero);
    CHECK(strstr(original.buf, "float dxbc_merge_") && !strstr(original.buf, "float2 dxbc_merge_") &&
        !strstr(original.buf, "float3 dxbc_merge_") && !strstr(original.buf, "float4 dxbc_merge_"));
    const int dots[] = {fixture.then_value, fixture.else_value};
    for (unsigned item = 0; item < 2; ++item) {
        const int index = dots[item]; const HLSLExpressionOrigin *origin = &original_map.origins[index];
        char declaration[80]; snprintf(declaration, sizeof(declaration), "const float dxbc_value_i%d = dot(", index);
        CHECK(strstr(original.buf, declaration) && origin->instruction_index == index &&
            origin->source_instruction_index == program->instructions[index].source_instruction_index &&
            origin->destination_lanes == (uint8_t)(1u << shape.result_lane) && origin->source_begin < origin->source_end);
        const char *call = strstr(original.buf + origin->source_begin, "dot(");
        CHECK(call && call < original.buf + origin->source_end);
    }
    char add_declaration[80]; snprintf(add_declaration, sizeof(add_declaration), "const float%u dxbc_value_i%d = ",
        shape.width, fixture.else_value - 1);
    CHECK(strstr(original.buf, add_declaration)); /* DP4 really needs a full FLOAT4 producer. */
    const HLSLExpressionOrigin *output = &original_map.origins[fixture.output];
    CHECK(output->destination_lanes == 7 && output->source_begin < output->source_end &&
        !memchr(original.buf + output->source_begin, '.', output->source_end - output->source_begin));
    /* The expression is a scalar phi. The actual MOV and interface receipt
     * own its legal assignment/broadcast into the declared FLOAT3 output. */
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = outputs & 1u ? &changed_map : NULL;
        options.source_quality = outputs & 2u ? &changed_quality : NULL;
        options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
        sb_free(&changed); sb_init(&changed);
        CHECK(hlsl_emit_with_options_diagnostic(program, &changed, NULL, NULL, NULL, &options, &diagnostic) &&
            changed.len == original.len && !memcmp(changed.buf, original.buf, original.len));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&original_map, &changed_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&original_quality, &changed_quality));
    }
    const size_t veto_points[] = {1, baseline.common.observations / 2u, baseline.common.observations};
    for (unsigned item = 0; item < sizeof(veto_points) / sizeof(*veto_points); ++item) {
        NaturalDotObservations veto = natural_dot_observer(program, shape); veto.common.reject_at = veto_points[item];
        sb_free(&changed); sb_init(&changed);
        CHECK(!natural_dot_emit(program, &changed, &changed_map, &changed_quality, &veto, &diagnostic) &&
            veto.common.observations == veto_points[item] && !changed_map.complete && !changed_map.count &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    }
    NaturalDotObservations restored = natural_dot_observer(program, shape);
    sb_free(&changed); sb_init(&changed);
    CHECK(natural_dot_emit(program, &changed, &changed_map, &changed_quality, &restored, &diagnostic) &&
        changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
        natural_if_maps_equal(&original_map, &changed_map) && hlsl_source_quality_results_equal(&original_quality, &changed_quality));
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    /* Decoded signatures, program storage and temporary formatting context
     * are gone. Both transferred arguments and their by-value facts remain. */
    const ASTExpr *left = owned->u.call.args[0];
    const ASTExpr *right = shape.broadcast ? owned->u.call.args[1]->u.cast.sub : owned->u.call.args[1];
    CHECK(left->operand_provenance.complete && right->operand_provenance.complete &&
        left->operand_provenance.operand_index == 1 && right->operand_provenance.operand_index == 2 &&
        left->u.emitter_operand[0] && right->u.emitter_operand[0] && owned->logical_origin.components == 1);
    ast_free_expr(owned);
    return true;
}

static bool check_natural_dot_rejections(void) {
    /* Raw IF isolates scalar/unused sibling failures from the BOOL predicate
     * proof; a missing scalar write cannot hide behind a second BOOL use. */
    const NaturalDotShape shape = {.width = 2, .layout = NATURAL_IF_INPUTS_PACKED_SPLIT, .nonzero = true};
    NaturalIfFixture fixture; CHECK(natural_dot_fixture(&fixture, shape));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 8 && program->input_count == 3 && program->signature_declaration_count == 4);
    USILInstruction instructions[8]; DXBCSignatureElement inputs[3]; USILSignatureDeclaration declarations[4];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(inputs, program->inputs, sizeof(inputs));
    memcpy(declarations, program->signature_declarations, sizeof(declarations));
    StringBuilder original, restored_source; sb_init(&original); sb_init(&restored_source);
    HLSLExpressionSourceMap original_map, restored_map;
    HLSLSourceQualityResult original_quality, restored_quality; HLSLEmitDiagnostic diagnostic;
    NaturalDotObservations baseline = natural_dot_observer(program, shape);
    CHECK(natural_dot_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic) &&
        original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    DXBCOperand relative = program->instructions[fixture.condition].operands[0];
    for (unsigned mutation = 0; mutation < 18; ++mutation) {
        USILInstruction *dot = &program->instructions[fixture.then_value];
        DXBCOperand *source = &dot->operands[2];
        switch (mutation) {
        case 0: dot->operands[0].destination_mask = 0x30; dot->operands[0].raw_token |= UINT32_C(0x20); break;
        case 1: dot->operands[0].type = OPERAND_TYPE_OUTPUT;
            dot->operands[0].raw_token = (dot->operands[0].raw_token & ~UINT32_C(0x000ff000)) |
                (uint32_t)OPERAND_TYPE_OUTPUT << 12u; break;
        case 2: dot->operand_count = 2; break;
        case 3: source->min_precision = 1; break;
        case 4: dot->precise_mask = 1; break;
        case 5: dot->saturate = true; break;
        case 6: /* A malformed modifier has no retained extended-token authority. */
            source->extended_token_count = 1; source->extended_tokens = NULL; break;
        case 7: /* A two-product read crosses the two independent FLOAT2 fields. */
            source->swizzle[0] = 1; source->raw_token = natural_if_source_token(OPERAND_TYPE_INPUT, source->swizzle); break;
        case 8: /* ELSE vector producer's Z is needed by the second dot. */
            program->instructions[fixture.else_value - 1].operands[0].destination_mask = 0x20;
            program->instructions[fixture.else_value - 1].operands[0].raw_token =
                (program->instructions[fixture.else_value - 1].operands[0].raw_token & ~UINT32_C(0xf0)) | UINT32_C(0x20); break;
        case 9: /* Demanding an undefined true-edge sibling is not a scalar dot phi. */
            for (unsigned lane = 0; lane < 4; ++lane) program->instructions[fixture.output].operands[1].swizzle[lane] = 1;
            program->instructions[fixture.output].operands[1].raw_token = natural_if_source_token(OPERAND_TYPE_TEMP,
                program->instructions[fixture.output].operands[1].swizzle); break;
        case 10: /* No scalar X definition on the false edge. */
            program->instructions[fixture.else_value].operands[0].destination_mask = 0x80;
            program->instructions[fixture.else_value].operands[0].raw_token =
                (program->instructions[fixture.else_value].operands[0].raw_token & ~UINT32_C(0xf0)) | UINT32_C(0x80); break;
        case 11: source->type = OPERAND_TYPE_RESOURCE; break;
        case 12: source->index_has_immediate[0] = false; source->index_representations[0] = 2;
            source->raw_token = (source->raw_token & ~UINT32_C(0x01c00000)) | UINT32_C(0x00800000);
            source->rel_op0 = &relative; break;
        case 13: program->inputs[1].mask = program->inputs[1].rw_mask = 6; break; /* overlap/hole */
        case 14: program->inputs[1].rw_mask = 4; break;
        case 15: program->inputs[1].interpolation_mode = 3; break;
        case 16: program->inputs[1].semantic_index = 0; break;
        case 17: program->instructions[fixture.output].operands[0].destination_mask = 0x30;
            program->instructions[fixture.output].operands[0].raw_token =
                (program->instructions[fixture.output].operands[0].raw_token & ~UINT32_C(0xf0)) | UINT32_C(0x30); break;
        }
        const bool rejected = natural_if_rejected(program, NULL);
        if (!rejected) fprintf(stderr, "Natural dot rejection mutation=%u dot=%d source_type=%d\n",
            mutation, fixture.then_value, (int)source->type);
        memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->inputs, inputs, sizeof(inputs));
        memcpy(program->signature_declarations, declarations, sizeof(declarations));
        CHECK(rejected); /* Restored borrowed relative node is detached before disposal. */
    }
    NaturalDotObservations restored = natural_dot_observer(program, shape);
    CHECK(natural_dot_emit(program, &restored_source, &restored_map, &restored_quality, &restored, &diagnostic) &&
        restored_source.len == original.len && !memcmp(restored_source.buf, original.buf, original.len) &&
        natural_if_maps_equal(&original_map, &restored_map) &&
        hlsl_source_quality_results_equal(&original_quality, &restored_quality));
    sb_free(&restored_source); sb_free(&original);
    natural_if_fixture_dispose(&fixture);
    /* A real scalar comparison writer is BOOL. Replicating it into every
     * product of DP3 cannot turn its bit-mask value into floating arithmetic. */
    const NaturalDotShape compared = {.width = 3, .compared = true, .nonzero = true};
    CHECK(natural_dot_fixture(&fixture, compared));
    program = &fixture.program;
    USILInstruction saved = program->instructions[fixture.then_value];
    DXBCOperand *predicate = &program->instructions[fixture.then_value].operands[2];
    predicate->type = OPERAND_TYPE_TEMP; predicate->register_index = 0; predicate->index_values[0] = 0;
    memset(predicate->swizzle, 0, sizeof(predicate->swizzle));
    predicate->raw_token = natural_if_source_token(OPERAND_TYPE_TEMP, predicate->swizzle);
    const bool bool_rejected = natural_if_rejected(program, NULL);
    program->instructions[fixture.then_value] = saved;
    CHECK(bool_rejected);
    natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_dot_callback_drift(void) {
    const NaturalDotShape shape = {.width = 3, .compared = true, .nonzero = true};
    NaturalIfFixture fixture; CHECK(natural_dot_fixture(&fixture, shape));
    USILProgram *program = &fixture.program;
    CHECK(program->instruction_count == 9 && program->input_count == 3);
    USILInstruction instructions[9]; DXBCSignatureElement inputs[3];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(inputs, program->inputs, sizeof(inputs));
    const char *prefix = "// retained caller prefix\n";
    StringBuilder original, changed; sb_init(&original); sb_init(&changed); sb_append(&original, prefix);
    HLSLExpressionSourceMap original_map, changed_map;
    HLSLSourceQualityResult original_quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    NaturalDotObservations baseline = natural_dot_observer(program, shape); baseline.common.mutable_source = &original;
    CHECK(natural_dot_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic) &&
        original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && baseline.common.observations > 2 &&
        baseline.common.first_header_observation && baseline.common.signature_observation);
    const size_t points[] = {1, baseline.common.observations / 2u, baseline.common.observations};
    /* Every changed model is independently admitted after rejecting a drift
     * of the frozen original. DOT operand order/width, vector literal bits,
     * a repeated scalar selector and actual IF polarity each change source. */
    const unsigned mutations[] = {26, 27, 28, 29, 30, 10};
    for (unsigned action = 0; action < sizeof(mutations) / sizeof(*mutations); ++action) {
        for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
            NaturalDotObservations drift = natural_dot_observer(program, shape);
            drift.common.mutable_program = program; drift.common.mutation = mutations[action];
            drift.common.mutate_at = points[point]; drift.common.condition_instruction = fixture.condition;
            drift.common.arithmetic_instruction = mutations[action] == 28 ? fixture.else_value - 1 : fixture.then_value;
            drift.common.replacement_opcode = USIL_OP_DP2;
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            const bool rejected = !natural_dot_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic);
            if (!rejected) fprintf(stderr, "Natural dot callback mutation=%u observation=%zu admitted\n",
                mutations[action], points[point]);
            CHECK(rejected && drift.common.mutated && !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && diagnostic.status != HLSL_EMIT_STATUS_OK);
            NaturalDotObservations fresh = natural_dot_observer(program, shape);
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            const bool regenerated = natural_dot_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic);
            if (!regenerated) fprintf(stderr, "Natural dot changed model mutation=%u status=%s reason=%s instruction=%d\n",
                mutations[action], hlsl_emit_status_name(diagnostic.status), hlsl_emit_reason_name(diagnostic.reason),
                diagnostic.instruction_index);
            CHECK(regenerated && changed_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && changed_map.complete &&
                hlsl_expression_source_map_matches(&changed_map, program, changed.buf) && strcmp(original.buf, changed.buf));
            memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->inputs, inputs, sizeof(inputs));
            fresh = natural_dot_observer(program, shape);
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(natural_dot_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
                natural_if_maps_equal(&original_map, &changed_map) &&
                hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    const struct {unsigned mutation; size_t point, source_offset;} source_attacks[] = {
        {4, baseline.common.observations, original_map.origins[fixture.then_value].source_begin},
        {15, baseline.common.observations, 0},
        {19, baseline.common.first_header_observation, 0},
        {20, baseline.common.signature_observation, 0}};
    for (unsigned attack = 0; attack < sizeof(source_attacks) / sizeof(*source_attacks); ++attack) {
        NaturalDotObservations drift = natural_dot_observer(program, shape);
        drift.common.mutable_program = program; drift.common.mutable_source = &changed; drift.common.mutable_map = &changed_map;
        drift.common.mutation = source_attacks[attack].mutation; drift.common.mutate_at = source_attacks[attack].point;
        drift.common.source_offset = source_attacks[attack].source_offset; drift.common.arithmetic_instruction = fixture.then_value;
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(!natural_dot_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
            drift.common.mutated && !changed_map.complete && !changed_map.count &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
            !memcmp(instructions, program->instructions, sizeof(instructions)) &&
            !memcmp(inputs, program->inputs, sizeof(inputs)));
        NaturalDotObservations fresh = natural_dot_observer(program, shape);
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(natural_dot_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
            changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
            natural_if_maps_equal(&original_map, &changed_map) &&
            hlsl_source_quality_results_equal(&original_quality, &changed_quality));
    }
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_dot_emission(void) {
    const NaturalDotShape shapes[] = {
        {.width = 2, .layout = NATURAL_IF_INPUTS_PACKED_SPLIT, .compared = true, .nonzero = true},
        {.width = 2, .layout = NATURAL_IF_INPUTS_PACKED_UNION, .compared = true, .nonzero = true},
        {.width = 3, .compared = true, .nonzero = false},
        {.width = 4, .compared = true, .nonzero = true},
        {.width = 2, .result_lane = 3, .nonzero = false},
        {.width = 3, .compared = true, .nonzero = true, .broadcast = true}};
    for (unsigned index = 0; index < sizeof(shapes) / sizeof(*shapes); ++index)
        CHECK(check_natural_dot_positive(shapes[index]));
    CHECK(check_natural_dot_rejections());
    CHECK(check_natural_dot_callback_drift());
    return true;
}

static bool natural_modifier_chain(const ASTExpr *root, uint8_t modifier, int instruction,
    uint32_t raw_instruction, uint8_t lanes, unsigned width, const ASTExpr **leaf) {
    const ASTExpr *node = root;
    for (unsigned layer = 0; layer < 2; ++layer) {
        const bool present = layer ? (modifier & 2u) != 0 : (modifier & 1u) != 0;
        if (!present) continue;
        CHECK(node && node->logical_origin.complete && node->logical_origin.scalar_type == AST_SCALAR_FLOAT32 &&
            node->logical_origin.components == width && node->logical_origin.instruction_index == instruction &&
            node->logical_origin.source_instruction_index == raw_instruction && node->logical_origin.destination_lanes == lanes);
        if (!layer) {
            CHECK(node->kind == AST_EXPR_UNARY && node->u.unary.op == USIL_OP_INEG);
            node = node->u.unary.sub;
        } else {
            CHECK(node->kind == AST_EXPR_CALL && !strcmp(node->u.call.name, "abs") && node->u.call.arg_count == 1);
            node = node->u.call.args[0];
        }
    }
    CHECK(node); *leaf = node;
    return true;
}

static bool natural_modifier_owned_roots(NaturalIfFixture *fixture, NaturalDotShape shape,
    const NaturalDotModifiers *mods, ASTExpr *roots[3]) {
    HLSLEmitterContext ctx; StringBuilder source; HLSLEmitDiagnostic diagnostic;
    CHECK(analyze(&ctx, &fixture->program)); sb_init(&source); hlsl_emit_diagnostic_init(&diagnostic);
    ctx.emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE; ctx.sb = &source; ctx.diagnostic = &diagnostic;
    ctx.entry_point_name = "main"; ctx.preferred_input_struct_name = "appdata"; ctx.preferred_output_struct_name = "v2f";
    CHECK(hlsl_natural_structured_preflight(&ctx)); ctx.high_level_interface = true;
    CHECK(hlsl_prepare_high_level_interface(&ctx));
    USILProgram *program = &fixture->program;
    const int add = fixture->else_value - 1;
    const int indices[] = {fixture->then_value, add, fixture->else_value};
    const int operands[] = {1, 2, 2};
    const uint8_t modifiers[] = {mods->then_input, mods->add_literal, mods->else_temp};
    for (unsigned item = 0; item < 3; ++item) {
        const int index = indices[item], operand = operands[item];
        const DXBCOperand *original = &program->instructions[index].operands[operand];
        DXBCOperand plain;
        USILOperandUseInfo use;
        CHECK(hlsl_natural_float_source_view(original, &plain) &&
            usil_instruction_operand_use(program, &program->instructions[index], operand, &use) &&
            use.use == USIL_OPERAND_USE_SOURCE && !plain.has_abs && !plain.has_neg && !plain.extended_tokens &&
            !plain.extended_token_count && plain.raw_token == original->raw_token &&
            plain.type == original->type && plain.register_index == original->register_index &&
            !memcmp(plain.swizzle, original->swizzle, sizeof(plain.swizzle)) &&
            !memcmp(plain.imm_values, original->imm_values, sizeof(plain.imm_values)) &&
            original->extended_token_count == 1 && original->extended_tokens &&
            original->extended_tokens[0] == (UINT32_C(1) | (uint32_t)modifiers[item] << 6u) &&
            original->has_abs == ((modifiers[item] & 2u) != 0) && original->has_neg == ((modifiers[item] & 1u) != 0) &&
            (original->raw_token & UINT32_C(0x80000000)));
        const DXBCInstruction *raw = NULL;
        for (int token = 0; token < fixture->semantic.instruction_count; ++token)
            if (fixture->semantic.instructions[token].has_raw_instruction_index &&
                fixture->semantic.instructions[token].raw_instruction_index == program->instructions[index].source_instruction_index)
                raw = &fixture->semantic.instructions[token];
        CHECK(raw && raw->operands[operand].raw_token == original->raw_token &&
            raw->operands[operand].extended_token_count == 1 &&
            raw->operands[operand].extended_tokens != original->extended_tokens &&
            raw->operands[operand].extended_tokens[0] == original->extended_tokens[0]);
        ctx.current_instruction_index = index;
        if (item < 2) roots[item] = hlsl_natural_source_atom(&ctx, index, operand, use.source_lane_mask);
        else {
            const DXBCOperand *destination = &program->instructions[add].operands[0];
            const uint8_t producer_mask = usil_operand_destination_lane_mask(destination);
            for (int lane = 0; lane < 4; ++lane) if (use.source_lane_mask & (1u << lane)) {
                const int value = variable(&ctx, index, operand, lane);
                CHECK(value >= 0 && value < ctx.ssa.ssa_var_count && ctx.ssa.ssa_var_defs[value] == add);
            }
            ASTExpr *value = ast_create_var(add, destination->register_index, OPERAND_TYPE_TEMP, "ownedVector");
            value = hlsl_instruction_logical_expression(&ctx, value, add, producer_mask, shape.width);
            value = hlsl_project_logical_temp(&ctx, value, producer_mask, shape.width, &plain, use.source_lane_mask, index);
            roots[item] = hlsl_natural_float_source_modifiers(&ctx, index, operand, use.source_lane_mask, value);
        }
        const ASTExpr *leaf = NULL;
        CHECK(roots[item] && natural_modifier_chain(roots[item], modifiers[item], index,
            program->instructions[index].source_instruction_index, use.source_lane_mask, shape.width, &leaf));
        if (!item) {
            CHECK(leaf->kind == AST_EXPR_EMITTER_OPERAND && leaf->operand_provenance.complete &&
                leaf->operand_provenance.instruction_index == index && leaf->operand_provenance.operand_index == operand &&
                leaf->operand_provenance.destination_lanes == use.source_lane_mask &&
                leaf->operand_provenance.natural_components == shape.width && leaf->operand_provenance.result_components == shape.width);
        } else if (item == 1) {
            CHECK(leaf->kind == AST_EXPR_LITERAL && leaf->u.literal.scalar_type == AST_SCALAR_FLOAT32 &&
                leaf->u.literal.components == (int)shape.width);
            unsigned component = 0;
            for (unsigned lane = 0; lane < 4; ++lane) if (use.source_lane_mask & (1u << lane))
                CHECK(leaf->u.literal.val[component++] == original->immediate_words[lane]);
            CHECK(component == shape.width); /* ABS/NEG never rewrite -0, subnormal or NaN leaf bits. */
        } else CHECK(leaf->kind == AST_EXPR_VAR && leaf->logical_origin.complete &&
            leaf->logical_origin.instruction_index == add && leaf->logical_origin.components == shape.width &&
            !strcmp(leaf->u.var.name, "ownedVector"));
    }
    dispose(&ctx); sb_free(&source);
    return true;
}

typedef struct {
    NaturalIfObservations common;
    int output_instruction;
    size_t output_scalar_modifiers;
} NaturalModifierObservations;

static bool observe_natural_modifier(void *context, const HLSLSourceQualityObservation *observation) {
    NaturalModifierObservations *ledger = context;
    if (!observe_natural_if(&ledger->common, observation)) return false;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    /* The source-owned ABS CALL at DP3 has width3/lanes7; the dot CALL has
     * result width1/lanes1. They are independent typed operations. */
    if (facts->instruction_index == ledger->output_instruction && facts->known &&
        facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
        (observation->ast_kind == AST_EXPR_UNARY || observation->ast_kind == AST_EXPR_CALL)) {
        ++ledger->output_scalar_modifiers;
        if (facts->components != 1 || facts->lanes != 7) ledger->common.wrong_owner = true;
    }
    return true;
}

static NaturalModifierObservations natural_modifier_observer(NaturalIfFixture *fixture) {
    NaturalModifierObservations result = {.common = {.program = &fixture->program}, .output_instruction = fixture->output};
    return result;
}

static bool natural_modifier_emit(NaturalIfFixture *fixture, StringBuilder *source, HLSLExpressionSourceMap *map,
    HLSLSourceQualityResult *quality, NaturalModifierObservations *observations, HLSLEmitDiagnostic *diagnostic) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = map; options.source_quality = quality;
    options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
    options.source_quality_observer = observe_natural_modifier; options.source_quality_observer_context = observations;
    return hlsl_emit_with_options_diagnostic(&fixture->program, source, NULL, NULL, NULL, &options, diagnostic);
}

static bool check_natural_modifier_positive(NaturalDotShape shape, NaturalDotModifiers mods) {
    NaturalIfFixture fixture; CHECK(natural_dot_fixture_modifiers(&fixture, shape, &mods));
    ASTExpr *roots[3] = {0}; CHECK(natural_modifier_owned_roots(&fixture, shape, &mods, roots));
    StringBuilder original, changed; sb_init(&original); sb_init(&changed);
    HLSLExpressionSourceMap original_map, changed_map; HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic; NaturalModifierObservations baseline = natural_modifier_observer(&fixture);
    const bool emitted = natural_modifier_emit(&fixture, &original, &original_map, &original_quality, &baseline, &diagnostic);
    if (!emitted) fprintf(stderr, "Natural modifier DP%u status=%s reason=%s instruction=%d\n", shape.width,
        hlsl_emit_status_name(diagnostic.status), hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted && original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !original_quality.reasons &&
        !original_quality.counts.incomplete_units && !original_quality.counts.residual_total &&
        !original_quality.counts.unknown_provenance && original_map.complete &&
        hlsl_expression_source_map_matches(&original_map, &fixture.program, original.buf) && !baseline.common.wrong_owner &&
        baseline.output_scalar_modifiers && strstr(original.buf, "abs(") && baseline.common.observations > 2);
    const HLSLExpressionOrigin *output = &original_map.origins[fixture.output];
    CHECK(output->destination_lanes == 7 && output->source_begin < output->source_end &&
        !memchr(original.buf + output->source_begin, '.', output->source_end - output->source_begin));
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = outputs & 1u ? &changed_map : NULL;
        options.source_quality = outputs & 2u ? &changed_quality : NULL;
        options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
        sb_free(&changed); sb_init(&changed);
        CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &changed, NULL, NULL, NULL, &options, &diagnostic) &&
            changed.len == original.len && !memcmp(changed.buf, original.buf, original.len));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&original_map, &changed_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&original_quality, &changed_quality));
    }
    const size_t points[] = {1, baseline.common.observations / 2u, baseline.common.observations};
    for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
        NaturalModifierObservations veto = natural_modifier_observer(&fixture); veto.common.reject_at = points[point];
        sb_free(&changed); sb_init(&changed);
        CHECK(!natural_modifier_emit(&fixture, &changed, &changed_map, &changed_quality, &veto, &diagnostic) &&
            veto.common.observations == points[point] && !changed_map.complete && !changed_map.count &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    }
    NaturalModifierObservations restored = natural_modifier_observer(&fixture); sb_free(&changed); sb_init(&changed);
    CHECK(natural_modifier_emit(&fixture, &changed, &changed_map, &changed_quality, &restored, &diagnostic) &&
        changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
        natural_if_maps_equal(&original_map, &changed_map) && hlsl_source_quality_results_equal(&original_quality, &changed_quality));
    const int indices[] = {fixture.then_value, fixture.else_value - 1, fixture.else_value};
    const uint32_t raw_indices[] = {fixture.program.instructions[indices[0]].source_instruction_index,
        fixture.program.instructions[indices[1]].source_instruction_index, fixture.program.instructions[indices[2]].source_instruction_index};
    const uint8_t demands[] = {(uint8_t)((1u << shape.width) - 1u), (uint8_t)(shape.width == 2 ? 6 : shape.width == 3 ? 14 : 15),
        (uint8_t)((1u << shape.width) - 1u)};
    const uint8_t modifiers[] = {mods.then_input, mods.add_literal, mods.else_temp};
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    for (unsigned item = 0; item < 3; ++item) {
        const ASTExpr *leaf = NULL;
        CHECK(natural_modifier_chain(roots[item], modifiers[item], indices[item], raw_indices[item], demands[item], shape.width, &leaf));
        if (!item) CHECK(leaf->kind == AST_EXPR_EMITTER_OPERAND && leaf->u.emitter_operand[0]);
        else if (item == 1) CHECK(leaf->kind == AST_EXPR_LITERAL && leaf->u.literal.scalar_type == AST_SCALAR_FLOAT32);
        else CHECK(leaf->kind == AST_EXPR_VAR && !strcmp(leaf->u.var.name, "ownedVector"));
        ast_free_expr(roots[item]);
    }
    return true;
}

static bool check_natural_modifier_rejections(void) {
    const NaturalDotShape shape = {.width = 3, .compared = true, .nonzero = true};
    const NaturalDotModifiers mods = {.then_input = 2, .add_input = 2, .add_literal = 2, .else_temp = 2, .output_temp = 2};
    NaturalIfFixture fixture; CHECK(natural_dot_fixture_modifiers(&fixture, shape, &mods));
    USILProgram *program = &fixture.program; CHECK(program->instruction_count == 9);
    USILInstruction saved[9]; memcpy(saved, program->instructions, sizeof(saved));
    DXBCOperand *source = &program->instructions[fixture.then_value].operands[1];
    const uint32_t extension = source->extended_tokens[0];
    DXBCOperand empty; memset(&empty, 0, sizeof(empty));
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        uint32_t extra[] = {UINT32_C(0x80000081), UINT32_C(0)};
        switch (mutation) {
        case 0: source->has_neg = true; break; /* Flags disagree with the retained ABS word. */
        case 1: source->raw_token &= ~UINT32_C(0x80000000); break;
        case 2: source->extended_tokens = NULL; source->extended_token_count = 0; break;
        case 3: source->extended_token_count = 0; break;
        case 4: source->extended_tokens[0] = UINT32_C(0x82); break;
        case 5: source->extended_tokens[0] |= UINT32_C(0x80000000); break;
        case 6: source->extended_tokens = extra; source->extended_token_count = 2; break;
        case 7: source->min_precision = 1; source->extended_tokens[0] |= UINT32_C(0x4000); break;
        case 8: source->extended_tokens[0] |= UINT32_C(0x20000); break;
        case 9: /* A raw continuation bit cannot claim an absent neutral extension. */
            source->has_abs = false; source->extended_tokens = NULL; source->extended_token_count = 0; break;
        case 10: /* MOD_NONE is outside the three measured canonical source forms. */
            source->has_abs = false; source->extended_tokens[0] = UINT32_C(1); break;
        }
        DXBCOperand view; memset(&view, 0xa5, sizeof(view));
        const bool view_rejected = !hlsl_natural_float_source_view(source, &view) && !memcmp(&view, &empty, sizeof(view));
        const bool emission_rejected = natural_if_rejected(program, NULL);
        saved[fixture.then_value].operands[1].extended_tokens[0] = extension;
        memcpy(program->instructions, saved, sizeof(saved));
        source = &program->instructions[fixture.then_value].operands[1];
        if (!view_rejected || !emission_rejected) fprintf(stderr, "Natural modifier malformed mutation=%u view=%d emission=%d\n",
            mutation, (int)view_rejected, (int)emission_rejected);
        CHECK(view_rejected && emission_rejected);
    }
    /* Canonical modifier words do not authorize arithmetic on a comparison's
     * BOOL mask, dynamic reads, or a different resource operand class. */
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        DXBCOperand relative = program->instructions[fixture.condition].operands[0];
        if (!mutation) {
            source->type = OPERAND_TYPE_TEMP; source->register_index = 0; source->index_values[0] = 0;
            memset(source->swizzle, 0, sizeof(source->swizzle));
            source->raw_token = natural_if_source_token(OPERAND_TYPE_TEMP, source->swizzle) | UINT32_C(0x80000000);
        } else if (mutation == 1) {
            source->index_has_immediate[0] = false; source->index_representations[0] = 2; source->rel_op0 = &relative;
            source->raw_token = (source->raw_token & ~UINT32_C(0x01c00000)) | UINT32_C(0x00800000);
        } else {
            source->type = OPERAND_TYPE_RESOURCE;
            source->raw_token = (source->raw_token & ~UINT32_C(0x000ff000)) | (uint32_t)OPERAND_TYPE_RESOURCE << 12u;
        }
        const bool rejected = natural_if_rejected(program, NULL);
        memcpy(program->instructions, saved, sizeof(saved)); source = &program->instructions[fixture.then_value].operands[1];
        CHECK(rejected); /* Borrowed relative nodes are detached before disposal. */
    }
    natural_if_fixture_dispose(&fixture);
    const NaturalDotModifiers prohibited[] = {{.comparison_input = 1}, {.condition_source = 2}, {.then_destination = 1}};
    for (unsigned item = 0; item < sizeof(prohibited) / sizeof(*prohibited); ++item) {
        CHECK(natural_dot_fixture_modifiers(&fixture, shape, &prohibited[item]));
        CHECK(natural_if_rejected(&fixture.program, NULL));
        natural_if_fixture_dispose(&fixture);
    }
    return true;
}

static bool check_natural_modifier_callbacks(void) {
    const NaturalDotShape shape = {.width = 3, .compared = true, .nonzero = true};
    const NaturalDotModifiers mods = {.then_input = 2, .add_input = 2, .add_literal = 2, .else_temp = 2, .output_temp = 2};
    NaturalIfFixture fixture; CHECK(natural_dot_fixture_modifiers(&fixture, shape, &mods));
    USILProgram *program = &fixture.program; CHECK(program->instruction_count == 9);
    USILInstruction saved[9]; memcpy(saved, program->instructions, sizeof(saved));
    const int owners[] = {fixture.then_value, fixture.else_value - 1, fixture.else_value, fixture.output};
    const int operands[] = {1, 2, 2, 1};
    uint32_t extensions[4];
    for (unsigned owner = 0; owner < 4; ++owner) {
        CHECK(saved[owners[owner]].operands[operands[owner]].extended_token_count == 1);
        extensions[owner] = saved[owners[owner]].operands[operands[owner]].extended_tokens[0];
    }
    const char *prefix = "// modifier caller prefix\n";
    StringBuilder original, changed; sb_init(&original); sb_init(&changed); sb_append(&original, prefix);
    HLSLExpressionSourceMap original_map, changed_map; HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic; NaturalModifierObservations baseline = natural_modifier_observer(&fixture);
    baseline.common.mutable_source = &original;
    CHECK(natural_modifier_emit(&fixture, &original, &original_map, &original_quality, &baseline, &diagnostic) &&
        original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && baseline.common.observations > 2 &&
        baseline.common.first_header_observation && baseline.common.signature_observation);
    const size_t points[] = {1, baseline.common.observations / 2u, baseline.common.observations};
    for (unsigned owner = 0; owner < 4; ++owner) {
        for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
            NaturalModifierObservations drift = natural_modifier_observer(&fixture);
            drift.common.mutable_program = program; drift.common.mutation = 31; drift.common.mutate_at = points[point];
            drift.common.arithmetic_instruction = owners[owner]; drift.common.modifier_operand = operands[owner];
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            const bool rejected = !natural_modifier_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic);
            if (!rejected) fprintf(stderr, "Natural modifier drift owner=%d operand=%d observation=%zu admitted\n",
                owners[owner], operands[owner], points[point]);
            CHECK(rejected && drift.common.mutated && !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && diagnostic.status != HLSL_EMIT_STATUS_OK);
            DXBCOperand *current = &program->instructions[owners[owner]].operands[operands[owner]];
            CHECK(current->has_abs && current->has_neg && current->extended_tokens[0] == UINT32_C(0xc1));
            NaturalModifierObservations fresh = natural_modifier_observer(&fixture);
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(natural_modifier_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && changed_map.complete &&
                hlsl_expression_source_map_matches(&changed_map, program, changed.buf) && strcmp(original.buf, changed.buf));
            /* Instruction snapshots borrow extension allocations. Restore
             * their pointed words independently before restoring the model. */
            for (unsigned restored = 0; restored < 4; ++restored)
                saved[owners[restored]].operands[operands[restored]].extended_tokens[0] = extensions[restored];
            memcpy(program->instructions, saved, sizeof(saved));
            fresh = natural_modifier_observer(&fixture); sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(natural_modifier_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
                natural_if_maps_equal(&original_map, &changed_map) && hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    const struct {unsigned mutation; size_t point, offset;} attacks[] = {
        {4, baseline.common.observations, original_map.origins[fixture.else_value].source_begin},
        {15, baseline.common.observations, 0}, {19, baseline.common.first_header_observation, 0},
        {20, baseline.common.signature_observation, 0}};
    for (unsigned attack = 0; attack < sizeof(attacks) / sizeof(*attacks); ++attack) {
        NaturalModifierObservations drift = natural_modifier_observer(&fixture);
        drift.common.mutable_program = program; drift.common.mutable_source = &changed; drift.common.mutable_map = &changed_map;
        drift.common.mutation = attacks[attack].mutation; drift.common.mutate_at = attacks[attack].point;
        drift.common.source_offset = attacks[attack].offset; drift.common.arithmetic_instruction = fixture.else_value;
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(!natural_modifier_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
            drift.common.mutated && !changed_map.complete && !changed_map.count &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && !memcmp(saved, program->instructions, sizeof(saved)));
        for (unsigned owner = 0; owner < 4; ++owner)
            CHECK(saved[owners[owner]].operands[operands[owner]].extended_tokens[0] == extensions[owner]);
        NaturalModifierObservations fresh = natural_modifier_observer(&fixture); sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(natural_modifier_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
            changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
            natural_if_maps_equal(&original_map, &changed_map) && hlsl_source_quality_results_equal(&original_quality, &changed_quality));
    }
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture);
    return true;
}

static bool check_natural_source_modifiers(void) {
    const NaturalDotShape shape = {.width = 3, .compared = true, .nonzero = true};
    NaturalIfFixture original, plain; const NaturalDotModifiers no_modifiers = {0};
    CHECK(natural_dot_fixture(&original, shape) && natural_dot_fixture_modifiers(&plain, shape, &no_modifiers) &&
        original.document.owned_size == plain.document.owned_size &&
        !memcmp(original.document.owned_bytes, plain.document.owned_bytes, original.document.owned_size));
    natural_if_fixture_dispose(&plain); natural_if_fixture_dispose(&original);
    const struct {NaturalDotShape shape; NaturalDotModifiers mods;} cases[] = {
        {{.width = 2, .layout = NATURAL_IF_INPUTS_PACKED_SPLIT, .compared = true, .nonzero = true},
            {.then_input = 1, .add_input = 2, .add_literal = 2, .else_temp = 3, .output_temp = 1}},
        {{.width = 3, .compared = true, .nonzero = false},
            {.then_input = 3, .add_input = 1, .add_literal = 3, .else_temp = 2, .output_temp = 3, .exceptional_literals = true}},
        {{.width = 4, .nonzero = true},
            {.then_input = 2, .add_input = 3, .add_literal = 1, .else_temp = 1, .output_temp = 2}}};
    for (unsigned item = 0; item < sizeof(cases) / sizeof(*cases); ++item)
        CHECK(check_natural_modifier_positive(cases[item].shape, cases[item].mods));
    CHECK(check_natural_modifier_rejections());
    CHECK(check_natural_modifier_callbacks());
    return true;
}

typedef struct {
    NaturalIfFixture decoded;
    int outer_if, inner_if, outer_comparison, inner_comparison;
    int inner_then, inner_else, other_writer, inner_end, outer_end;
    uint8_t writer_mask;
    bool dot, inner_in_else, compared;
    unsigned width;
} NaturalNestedFixture;

typedef struct {
    uint32_t words[128];
    size_t count;
    int operations;
} NaturalNestedWords;

/* Assemble only complete instruction slices from the existing authored
 * documents. The official token reader and a new ordinary decode retain
 * declaration, raw-instruction, signature, CFG and SSA authority. */
static bool natural_nested_copy_instruction(NaturalNestedWords *words,
    const NaturalIfFixture *base, int instruction, bool prefix, bool inner,
    uint8_t writer_mask) {
    const uint32_t raw = base->program.instructions[instruction].source_instruction_index;
    const DXBCDocumentInstruction *record = NULL;
    for (size_t index = 0; index < base->document.instruction_count; ++index)
        if (base->document.instructions[index].instruction_index == raw)
            record = &base->document.instructions[index];
    CHECK(record && words->count + record->token_count <= sizeof(words->words) / sizeof(*words->words));
    const size_t begin = words->count;
    for (uint32_t index = 0; index < record->token_count; ++index)
        CHECK(dxbc_document_instruction_token(record, index, &words->words[words->count++]));
    unsigned lane = 0;
    while (!(writer_mask & (1u << lane))) ++lane;
    if (prefix) {
        CHECK(base->program.instructions[instruction].opcode == USIL_OP_LT && record->token_count == 7);
        words->words[begin + 1] = (words->words[begin + 1] & ~UINT32_C(0xf0)) | (UINT32_C(1) << (lane + 4u));
        words->words[begin + 2] = 0;
        if (inner) words->words[begin + 6] = UINT32_C(0x3f000000);
    } else if (base->program.instructions[instruction].opcode == USIL_OP_IF) {
        CHECK(record->token_count == 3);
        if (base->condition) {
            words->words[begin + 1] = UINT32_C(0x0010000a) | (uint32_t)lane << 4u;
            words->words[begin + 2] = 0;
        }
        if (inner) words->words[begin] ^= UINT32_C(0x40000);
    }
    ++words->operations;
    return true;
}

static bool natural_nested_copy_range(NaturalNestedWords *words,
    const NaturalIfFixture *base, int first, int end, bool prefix, bool inner,
    uint8_t writer_mask) {
    for (int index = first; index < end; ++index)
        CHECK(natural_nested_copy_instruction(words, base, index, prefix, inner, writer_mask));
    return true;
}

static bool natural_nested_inner(NaturalNestedWords *words, const NaturalIfFixture *base,
    int otherwise, int end, NaturalNestedFixture *nested) {
    nested->inner_comparison = base->condition ? words->operations : -1;
    CHECK(natural_nested_copy_range(words, base, 0, base->condition, true, true, nested->writer_mask));
    nested->inner_if = words->operations;
    CHECK(natural_nested_copy_instruction(words, base, base->condition, false, true, nested->writer_mask));
    nested->inner_then = words->operations + base->then_value - base->condition - 1;
    CHECK(natural_nested_copy_range(words, base, base->condition + 1, otherwise, false, false, nested->writer_mask));
    CHECK(natural_nested_copy_instruction(words, base, otherwise, false, false, nested->writer_mask));
    nested->inner_else = words->operations + base->else_value - otherwise - 1;
    CHECK(natural_nested_copy_range(words, base, otherwise + 1, end, false, false, nested->writer_mask));
    nested->inner_end = words->operations;
    CHECK(natural_nested_copy_instruction(words, base, end, false, false, nested->writer_mask));
    return true;
}

static bool natural_nested_fixture(NaturalNestedFixture *nested, unsigned width,
    uint8_t mask, bool dot, bool inner_in_else, bool compared, bool vertex, bool dead_read) {
    memset(nested, 0, sizeof(*nested));
    nested->dot = dot; nested->width = width; nested->writer_mask = mask;
    nested->inner_in_else = inner_in_else; nested->compared = compared;
    NaturalIfFixture base;
    if (dot) {
        const NaturalDotShape shape = {.width = width, .layout = width == 2 ?
            NATURAL_IF_INPUTS_PACKED_SPLIT : NATURAL_IF_INPUTS_SEPARATE,
            .compared = compared, .nonzero = true};
        CHECK(!vertex && mask == 1 && natural_dot_fixture(&base, shape));
    } else if (vertex) {
        CHECK(width == 3 && mask == 7 && natural_if_multiple_outputs_fixture_mode(&base,
            true, compared ? USIL_OP_LT : USIL_OP_NOP, 1, true));
    } else {
        CHECK(natural_if_fixture_init_mode(&base, width, mask, true, false, false, false,
            compared ? USIL_OP_LT : USIL_OP_NOP, 1, UINT32_C(0x3ec00000)));
    }
    NaturalNestedWords words = {0};
    const uint32_t first_raw = base.program.instructions[0].source_instruction_index;
    for (size_t index = 0; index < base.document.instruction_count; ++index) {
        const DXBCDocumentInstruction *record = &base.document.instructions[index];
        if (record->instruction_index >= first_raw) continue;
        CHECK(words.count + record->token_count <= sizeof(words.words) / sizeof(*words.words));
        for (uint32_t token = 0; token < record->token_count; ++token)
            CHECK(dxbc_document_instruction_token(record, token, &words.words[words.count++]));
    }
    int otherwise = -1, end = -1;
    for (int index = 0; index < base.program.instruction_count; ++index) {
        if (base.program.instructions[index].opcode == USIL_OP_ELSE) otherwise = index;
        if (base.program.instructions[index].opcode == USIL_OP_ENDIF) end = index;
    }
    CHECK(otherwise > base.condition && end > otherwise);
    nested->outer_comparison = compared ? words.operations : -1;
    CHECK(natural_nested_copy_range(&words, &base, 0, base.condition, true, false, mask));
    nested->outer_if = words.operations;
    CHECK(natural_nested_copy_instruction(&words, &base, base.condition, false, false, mask));
    if (!inner_in_else) CHECK(natural_nested_inner(&words, &base, otherwise, end, nested));
    else {
        nested->other_writer = words.operations + base.then_value - base.condition - 1;
        CHECK(natural_nested_copy_range(&words, &base, base.condition + 1, otherwise, false, false, mask));
    }
    CHECK(natural_nested_copy_instruction(&words, &base, otherwise, false, false, mask));
    if (inner_in_else) CHECK(natural_nested_inner(&words, &base, otherwise, end, nested));
    else {
        nested->other_writer = words.operations + base.else_value - otherwise - 1;
        CHECK(natural_nested_copy_range(&words, &base, otherwise + 1, end, false, false, mask));
    }
    nested->outer_end = words.operations;
    CHECK(natural_nested_copy_instruction(&words, &base, end, false, false, mask));
    if (dead_read) {
        /* The result W is unused, but this real ADD still reads outer Y. Its
         * incoming inner-Y phi has an undefined true edge in the DP3 grammar. */
        CHECK(dot && width == 3 && words.count + 7u <= sizeof(words.words) / sizeof(*words.words));
        const uint8_t y[] = {1, 1, 1, 1};
        const uint32_t read[] = {UINT32_C(0x07000000), UINT32_C(0x00100082), 0,
            natural_if_source_token(OPERAND_TYPE_TEMP, y), 0, UINT32_C(0x4001), UINT32_C(0x3e800000)};
        memcpy(words.words + words.count, read, sizeof(read)); words.count += 7; ++words.operations;
    }
    const int suffix = words.operations;
    CHECK(natural_nested_copy_range(&words, &base, end + 1, base.program.instruction_count, false, false, mask));
    NaturalIfFixture *decoded = &nested->decoded;
    dxbc_document_init(&decoded->document); dxbc_stage_contract_init(&decoded->contract);
    CHECK(natural_if_fixture_decode_words(decoded, vertex ? UINT32_C(0x00010050) : UINT32_C(0x00000050),
        words.words, words.count, base.program.inputs, (unsigned)base.program.input_count,
        base.program.outputs, (unsigned)base.program.output_count));
    decoded->condition = nested->outer_if; decoded->then_value = nested->inner_then;
    decoded->else_value = nested->inner_else;
    decoded->join_value = suffix + base.join_value - end - 1;
    decoded->output = suffix + base.output - end - 1;
    CHECK(decoded->program.instruction_count == words.operations);
    natural_if_fixture_dispose(&base);
    return true;
}

static const HLSLPhiNode *natural_nested_phi(const HLSLEmitterContext *ctx, int block, int value) {
    const HLSLBlockPhis *phis = &ctx->ssa.block_phis[block];
    for (int index = 0; index < phis->phi_count; ++index)
        if (phis->phis[index].ssa_var == value) return &phis->phis[index];
    return NULL;
}

static bool check_natural_nested_owners(NaturalNestedFixture *fixture) {
    USILProgram *program = &fixture->decoded.program;
    HLSLEmitterContext ctx; CHECK(analyze(&ctx, program));
    HLSLIfRegion outer, inner;
    CHECK(hlsl_cfg_if_region(&ctx, fixture->outer_if, &outer) &&
        hlsl_cfg_if_region(&ctx, fixture->inner_if, &inner) && outer.end_instruction == fixture->outer_end &&
        inner.end_instruction == fixture->inner_end && inner.end_instruction < outer.end_instruction &&
        hlsl_cfg_dominates(&ctx.cfg, outer.header_block, inner.header_block));
    CHECK(fixture->inner_in_else ? fixture->inner_if > outer.else_instruction : fixture->inner_end < outer.else_instruction);
    const HLSLBlockPhis *inner_phis = &ctx.ssa.block_phis[inner.join_block];
    const HLSLBlockPhis *outer_phis = &ctx.ssa.block_phis[outer.join_block];
    CHECK(ctx.cfg.blocks[inner.join_block].predecessor_count == 2 && ctx.cfg.blocks[outer.join_block].predecessor_count == 2);
    unsigned actual_lanes = 0, dead_chain = 0, undefined_inner = 0;
    for (int index = 0; index < inner_phis->phi_count; ++index) {
        const HLSLPhiNode *phi = &inner_phis->phis[index];
        if (phi->register_index != 0) continue;
        CHECK(phi->component >= 0 && phi->component < 4 && phi->incoming_vars && phi->incoming_blocks);
        const HLSLPhiNode *following = NULL;
        for (int item = 0; item < outer_phis->phi_count; ++item) {
            const HLSLPhiNode *candidate = &outer_phis->phis[item];
            if (candidate->register_index == 0 && candidate->component == phi->component &&
                (candidate->incoming_vars[0] == phi->ssa_var || candidate->incoming_vars[1] == phi->ssa_var)) following = candidate;
        }
        CHECK(following);
        if (fixture->writer_mask & (1u << (unsigned)phi->component)) {
            ++actual_lanes;
            const int left = phi->incoming_vars[0], right = phi->incoming_vars[1];
            CHECK(left >= 0 && right >= 0 && left < ctx.ssa.ssa_var_count && right < ctx.ssa.ssa_var_count);
            const int a = ctx.ssa.ssa_var_defs[left], b = ctx.ssa.ssa_var_defs[right];
            CHECK((a == fixture->inner_then && b == fixture->inner_else) ||
                (a == fixture->inner_else && b == fixture->inner_then));
            const int other = following->incoming_vars[following->incoming_vars[0] == phi->ssa_var ? 1 : 0];
            CHECK(other >= 0 && other < ctx.ssa.ssa_var_count && ctx.ssa.ssa_var_defs[other] == fixture->other_writer &&
                ctx.ssa.ssa_var_defs[following->ssa_var] == HLSL_DEFINITION_AMBIGUOUS);
            const int output_lane = fixture->dot ? 0 : (int)actual_lanes - 1;
            CHECK(variable(&ctx, fixture->decoded.join_value, 1, output_lane) == following->ssa_var);
        } else {
            ++dead_chain;
            for (int edge = 0; edge < 2; ++edge)
                if (phi->incoming_vars[edge] < 0 || ctx.ssa.ssa_var_defs[phi->incoming_vars[edge]] == HLSL_DEFINITION_UNKNOWN)
                    ++undefined_inner;
        }
    }
    CHECK(actual_lanes == (fixture->dot ? 1u : fixture->width));
    if (fixture->dot && fixture->width < 4) CHECK(dead_chain && undefined_inner);
    if (fixture->dot) {
        const int writers[] = {fixture->inner_then, fixture->inner_else, fixture->other_writer};
        const uint8_t demand = (uint8_t)((1u << fixture->width) - 1u);
        for (unsigned item = 0; item < 3; ++item) {
            const USILInstruction *writer = &program->instructions[writers[item]];
            CHECK(writer->operand_count == 3 && usil_operand_destination_lane_mask(&writer->operands[0]) == 1);
            for (int operand = 1; operand < 3; ++operand) {
                USILOperandUseInfo use;
                CHECK(usil_instruction_operand_use(program, writer, operand, &use) &&
                    use.use == USIL_OPERAND_USE_SOURCE && use.source_lane_mask == demand);
            }
        }
    }
    if (fixture->compared) {
        const int comparisons[] = {fixture->outer_comparison, fixture->inner_comparison};
        const int conditions[] = {fixture->outer_if, fixture->inner_if};
        unsigned lane = 0; while (!(fixture->writer_mask & (1u << lane))) ++lane;
        int first_ssa = -1;
        for (unsigned index = 0; index < 2; ++index) {
            int consumer = -1;
            const int value = variable(&ctx, comparisons[index], 0, (int)lane);
            CHECK(hlsl_scalar_comparison_predicate_supported(&ctx, comparisons[index], &consumer) && consumer == conditions[index] &&
                value >= 0 && value < ctx.ssa.ssa_var_count && ctx.ssa.ssa_var_defs[value] == comparisons[index] &&
                variable(&ctx, conditions[index], 0, 0) == value && (index == 0 || value != first_ssa));
            first_ssa = value;
        }
        CHECK(program->instructions[comparisons[0]].operands[0].register_index ==
            program->instructions[comparisons[1]].operands[0].register_index);
    }
    dispose(&ctx); return true;
}

static bool check_natural_nested_positive(unsigned width, uint8_t mask, bool dot,
    bool inner_in_else, bool compared, bool vertex) {
    NaturalNestedFixture fixture;
    CHECK(natural_nested_fixture(&fixture, width, mask, dot, inner_in_else, compared, vertex, false) &&
        check_natural_nested_owners(&fixture));
    USILProgram *program = &fixture.decoded.program;
    ASTExpr *owned = NULL;
    if (dot) {
        const NaturalDotShape shape = {.width = width, .layout = width == 2 ?
            NATURAL_IF_INPUTS_PACKED_SPLIT : NATURAL_IF_INPUTS_SEPARATE,
            .compared = compared, .nonzero = true};
        CHECK(natural_dot_owned_tree(&fixture.decoded, shape, &owned));
    }
    StringBuilder original, selected; sb_init(&original); sb_init(&selected);
    HLSLExpressionSourceMap original_map, selected_map;
    HLSLSourceQualityResult original_quality, selected_quality; HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations baseline = {.program = program, .mutable_source = &original};
    const bool emitted = natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic);
    if (!emitted) fprintf(stderr, "Nested natural width=%u mask=%u dot=%d inner_else=%d compared=%d vertex=%d: %s/%s at %d\n",
        width, mask, dot, inner_in_else, compared, vertex, hlsl_emit_status_name(diagnostic.status),
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted && original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !original_quality.reasons &&
        original_quality.counts.inspected_units == 1 && !original_quality.counts.incomplete_units &&
        !original_quality.counts.residual_total && !original_quality.counts.unknown_provenance &&
        original_map.complete && original_map.count == (size_t)program->instruction_count &&
        hlsl_expression_source_map_matches(&original_map, program, original.buf) && !baseline.wrong_owner &&
        baseline.observations > 2 && baseline.comparison_events == (compared ? 2u : 0u));
    CHECK(!strstr(original.buf, "u_xlat") && !strstr(original.buf, "float4 r0"));
    const int controls[] = {fixture.outer_if, fixture.inner_if};
    for (unsigned item = 0; item < 2; ++item) {
        const int index = controls[item]; const HLSLExpressionOrigin *origin = &original_map.origins[index];
        CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_CONTROL && !origin->destination_lanes &&
            origin->instruction_index == index && origin->source_instruction_index == program->instructions[index].source_instruction_index &&
            origin->source_begin < origin->source_end &&
            (memchr(original.buf + origin->source_begin, '!', origin->source_end - origin->source_begin) != NULL) == (item != 0));
    }
    const int writers[] = {fixture.inner_then, fixture.inner_else, fixture.other_writer};
    for (unsigned item = 0; item < 3; ++item) {
        const int index = writers[item]; const HLSLExpressionOrigin *origin = &original_map.origins[index];
        CHECK(origin->instruction_index == index && origin->source_instruction_index == program->instructions[index].source_instruction_index &&
            origin->destination_lanes == mask && origin->source_begin < origin->source_end);
        if (dot) {
            CHECK(program->instructions[index].opcode == (width == 2 ? USIL_OP_DP2 : width == 3 ? USIL_OP_DP3 : USIL_OP_DP4));
            const char *call = strstr(original.buf + origin->source_begin, "dot(");
            CHECK(call && call < original.buf + origin->source_end);
        }
    }
    /* The outer assignment remains owned by its real MOV/MUL/ADD mask; the
     * nested scalar dot phis do not counterfeit a vector result owner. */
    const HLSLExpressionOrigin *output = &original_map.origins[fixture.decoded.output];
    CHECK(output->destination_lanes == (dot || vertex ? 7u : (1u << width) - 1u));
    if (dot) CHECK(!strstr(original.buf, "float2 dxbc_merge_") && !strstr(original.buf, "float3 dxbc_merge_") &&
        !strstr(original.buf, "float4 dxbc_merge_") && !memchr(original.buf + output->source_begin, '.', output->source_end - output->source_begin));
    if (dot) {
        NaturalDotObservations typed = {.common = {.program = program}, .result_mask = 1};
        CHECK(natural_dot_emit(program, &selected, &selected_map, &selected_quality, &typed, &diagnostic) &&
            selected.len == original.len && !memcmp(selected.buf, original.buf, original.len) &&
            natural_if_maps_equal(&original_map, &selected_map) && hlsl_source_quality_results_equal(&original_quality, &selected_quality) &&
            !typed.common.wrong_owner && typed.dot_calls == ((UINT64_C(1) << (unsigned)fixture.inner_then) |
                (UINT64_C(1) << (unsigned)fixture.inner_else) | (UINT64_C(1) << (unsigned)fixture.other_writer)) &&
            typed.scalar_phi_ids && (typed.scalar_phi_ids & (typed.scalar_phi_ids - 1u)));
        if (width == 2) CHECK(typed.packed_fields == 3);
    }
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = outputs & 1u ? &selected_map : NULL;
        options.source_quality = outputs & 2u ? &selected_quality : NULL;
        options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
        sb_free(&selected); sb_init(&selected);
        CHECK(hlsl_emit_with_options_diagnostic(program, &selected, NULL, NULL, NULL, &options, &diagnostic) &&
            selected.len == original.len && !memcmp(selected.buf, original.buf, original.len));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&original_map, &selected_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&original_quality, &selected_quality));
    }
    const size_t points[] = {1, baseline.observations / 2u, baseline.observations};
    for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
        NaturalIfObservations veto = {.program = program, .reject_at = points[point]};
        sb_free(&selected); sb_init(&selected);
        CHECK(!natural_if_emit(program, &selected, &selected_map, &selected_quality, &veto, &diagnostic) &&
            veto.observations == points[point] && !selected_map.complete && !selected_map.count &&
            selected_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    }
    NaturalIfObservations fresh = {.program = program};
    sb_free(&selected); sb_init(&selected);
    CHECK(natural_if_emit(program, &selected, &selected_map, &selected_quality, &fresh, &diagnostic) &&
        selected.len == original.len && !memcmp(selected.buf, original.buf, original.len) &&
        natural_if_maps_equal(&original_map, &selected_map) && hlsl_source_quality_results_equal(&original_quality, &selected_quality));
    sb_free(&selected); sb_free(&original); natural_if_fixture_dispose(&fixture.decoded);
    if (owned) {
        CHECK(owned->kind == AST_EXPR_CALL && owned->logical_origin.components == 1 &&
            owned->u.call.arg_count == 2 && owned->u.call.args[0] != owned->u.call.args[1] &&
            owned->u.call.args[0]->operand_provenance.complete && owned->u.call.args[1]->operand_provenance.complete &&
            owned->u.call.args[0]->operand_provenance.operand_index == 1 &&
            owned->u.call.args[1]->operand_provenance.operand_index == 2 &&
            owned->u.call.args[0]->u.emitter_operand[0] && owned->u.call.args[1]->u.emitter_operand[0]);
        ast_free_expr(owned);
    }
    return true;
}

static bool check_natural_nested_callbacks(void) {
    NaturalNestedFixture fixture;
    CHECK(natural_nested_fixture(&fixture, 3, 1, true, true, true, false, false));
    USILProgram *program = &fixture.decoded.program;
    CHECK(program->instruction_count <= 24 && program->input_count == 3);
    USILInstruction instructions[24]; DXBCSignatureElement inputs[3];
    const size_t instruction_bytes = (size_t)program->instruction_count * sizeof(*instructions);
    memcpy(instructions, program->instructions, instruction_bytes); memcpy(inputs, program->inputs, sizeof(inputs));
    const char *prefix = "// retained nested caller prefix\n";
    StringBuilder original, changed; sb_init(&original); sb_init(&changed); sb_append(&original, prefix);
    HLSLExpressionSourceMap original_map, changed_map; HLSLSourceQualityResult original_quality, changed_quality;
    HLSLEmitDiagnostic diagnostic;
    NaturalIfObservations baseline = {.program = program, .mutable_source = &original};
    CHECK(natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic) &&
        original_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && baseline.first_header_observation && baseline.signature_observation);
    const size_t points[] = {1, baseline.observations / 2u, baseline.observations};
    const struct {unsigned mutation; int instruction;} changes[] = {
        {29, fixture.outer_if}, {29, fixture.inner_if}, {32, fixture.inner_comparison},
        {26, fixture.inner_then}, {28, fixture.inner_else - 1}, {27, fixture.inner_then}};
    for (unsigned action = 0; action < sizeof(changes) / sizeof(*changes); ++action) {
        for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
            NaturalIfObservations drift = {.program = program, .mutable_program = program,
                .mutation = changes[action].mutation, .mutate_at = points[point],
                .condition_instruction = changes[action].instruction, .arithmetic_instruction = changes[action].instruction};
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(!natural_if_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
                drift.mutated && !changed_map.complete && !changed_map.count && changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED);
            NaturalIfObservations fresh = {.program = program};
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            const bool regenerated = natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic);
            if (!regenerated) fprintf(stderr, "Nested changed model mutation=%u: %s/%s at %d\n", changes[action].mutation,
                hlsl_emit_status_name(diagnostic.status), hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
            CHECK(regenerated && changed_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && changed_map.complete &&
                hlsl_expression_source_map_matches(&changed_map, program, changed.buf) && strcmp(original.buf, changed.buf));
            memcpy(program->instructions, instructions, instruction_bytes); memcpy(program->inputs, inputs, sizeof(inputs));
            fresh = (NaturalIfObservations){.program = program};
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
                natural_if_maps_equal(&original_map, &changed_map) && hlsl_source_quality_results_equal(&original_quality, &changed_quality));
        }
    }
    const struct {unsigned mutation; size_t point, offset;} attacks[] = {
        {19, baseline.first_header_observation, 0}, {20, baseline.signature_observation, 0},
        {4, baseline.observations, original_map.origins[fixture.inner_then].source_begin},
        {15, baseline.observations, 0}, {4, baseline.first_preheader_observation, 0}};
    CHECK(baseline.first_preheader_observation);
    for (unsigned attack = 0; attack < sizeof(attacks) / sizeof(*attacks); ++attack) {
        NaturalIfObservations drift = {.program = program, .mutable_program = program, .mutable_source = &changed,
            .mutable_map = &changed_map, .mutation = attacks[attack].mutation, .mutate_at = attacks[attack].point,
            .source_offset = attacks[attack].offset, .arithmetic_instruction = fixture.inner_then};
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(!natural_if_emit(program, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
            drift.mutated && !changed_map.complete && !changed_map.count && changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
            !memcmp(program->instructions, instructions, instruction_bytes) && !memcmp(program->inputs, inputs, sizeof(inputs)));
        NaturalIfObservations fresh = {.program = program};
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(natural_if_emit(program, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
            changed.len == original.len && !memcmp(changed.buf, original.buf, original.len) &&
            natural_if_maps_equal(&original_map, &changed_map) && hlsl_source_quality_results_equal(&original_quality, &changed_quality));
    }
    sb_free(&changed); sb_free(&original); natural_if_fixture_dispose(&fixture.decoded); return true;
}

static bool natural_nested_control_negative(NaturalNestedFixture *source, unsigned edit) {
    const NaturalIfFixture *base = &source->decoded;
    CHECK(!source->compared && edit < 2);
    NaturalNestedWords words = {0};
    const uint32_t first_raw = base->program.instructions[0].source_instruction_index;
    for (size_t index = 0; index < base->document.instruction_count; ++index) {
        const DXBCDocumentInstruction *record = &base->document.instructions[index];
        if (record->instruction_index >= first_raw) continue;
        CHECK(words.count + record->token_count <= sizeof(words.words) / sizeof(*words.words));
        for (uint32_t token = 0; token < record->token_count; ++token)
            CHECK(dxbc_document_instruction_token(record, token, &words.words[words.count++]));
    }
    int inner_else = -1;
    for (int index = source->inner_if + 1; index < source->inner_end; ++index)
        if (base->program.instructions[index].opcode == USIL_OP_ELSE) inner_else = index;
    CHECK(inner_else >= 0);
    for (int index = 0; index < base->program.instruction_count; ++index) {
        if (edit == 0 && index == inner_else) continue; /* Balanced, genuinely decoded IF without ELSE. */
        if (edit == 1 && index == source->inner_then) {
            /* A third contained diamond has a complete scalar write on both
             * edges. Its exclusion does not rely on an undefined output. */
            CHECK(natural_nested_copy_instruction(&words, base, source->inner_if, false, false, 1) &&
                natural_nested_copy_instruction(&words, base, index, false, false, 1) &&
                natural_nested_copy_instruction(&words, base, inner_else, false, false, 1));
        }
        CHECK(natural_nested_copy_instruction(&words, base, index, false, false, 1));
        if (edit == 1 && index == source->inner_then)
            CHECK(natural_nested_copy_instruction(&words, base, source->inner_end, false, false, 1));
    }
    NaturalIfFixture changed = {0};
    dxbc_document_init(&changed.document); dxbc_stage_contract_init(&changed.contract);
    CHECK(natural_if_fixture_decode_words(&changed, UINT32_C(0x00000050), words.words, words.count,
        base->program.inputs, (unsigned)base->program.input_count, base->program.outputs, (unsigned)base->program.output_count));
    const bool rejected = natural_if_rejected(&changed.program, NULL);
    natural_if_fixture_dispose(&changed);
    CHECK(rejected); return true;
}

static bool check_natural_nested_rejections(void) {
    NaturalNestedFixture fixture;
    CHECK(natural_nested_fixture(&fixture, 3, 1, true, false, false, false, false) && check_natural_nested_owners(&fixture));
    CHECK(natural_nested_control_negative(&fixture, 0) && natural_nested_control_negative(&fixture, 1));
    USILProgram *program = &fixture.decoded.program;
    CHECK(program->instruction_count <= 24);
    const size_t instruction_bytes = (size_t)program->instruction_count * sizeof(USILInstruction);
    USILInstruction instructions[24]; memcpy(instructions, program->instructions, instruction_bytes);
    const DXBCSignatureElement input = program->inputs[0];
    StringBuilder original, restored_source; sb_init(&original); sb_init(&restored_source);
    HLSLExpressionSourceMap original_map, restored_map; HLSLSourceQualityResult original_quality, restored_quality;
    HLSLEmitDiagnostic diagnostic; NaturalIfObservations baseline = {.program = program};
    CHECK(natural_if_emit(program, &original, &original_map, &original_quality, &baseline, &diagnostic));
    DXBCOperand relative = program->instructions[fixture.decoded.output].operands[1];
    relative.swizzle_mode = 2;
    memset(relative.swizzle, 1, sizeof(relative.swizzle));
    relative.raw_token = UINT32_C(0x0010001a);
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        bool relative_proved = true;
        USILInstruction *dot = &program->instructions[fixture.inner_then];
        DXBCOperand *output_source = &program->instructions[fixture.decoded.output].operands[1];
        switch (mutation) {
        case 0: /* Inner false edge no longer defines the demanded scalar X. */
            program->instructions[fixture.inner_else].operands[0].destination_mask = 0x80;
            program->instructions[fixture.inner_else].operands[0].raw_token = UINT32_C(0x00100082); break;
        case 1: /* Actual output Y demand propagates outer->inner phi liveness. */
            memset(output_source->swizzle, 1, sizeof(output_source->swizzle));
            output_source->raw_token = natural_if_source_token(OPERAND_TYPE_TEMP, output_source->swizzle); break;
        case 2: /* A vector TEMP is not a scalar reduction writer. */
            dot->operands[0].destination_mask = 0x30; dot->operands[0].raw_token = UINT32_C(0x00100032); break;
        case 3: /* The inner ELSE dot needs all three physical ADD YZW lanes. */
            program->instructions[fixture.inner_else - 1].operands[0].destination_mask = 0x60;
            program->instructions[fixture.inner_else - 1].operands[0].raw_token = UINT32_C(0x00100062); break;
        case 4: /* Output is written only within the inner true arm. */
            dot->operands[0].type = OPERAND_TYPE_OUTPUT; dot->operands[0].destination_mask = 0x70;
            dot->operands[0].raw_token = UINT32_C(0x00102072); break;
        case 5: program->instructions[fixture.decoded.output].operands[0].destination_mask = 0x30;
            program->instructions[fixture.decoded.output].operands[0].raw_token = UINT32_C(0x00102032); break;
        case 6: dot->operands[1].min_precision = 1; break;
        case 7: program->inputs[0].component_type = 2; break;
        case 8: dot->operands[1].type = OPERAND_TYPE_RESOURCE; break;
        case 9: /* A real relative slot reads the unresolved outer Y phi. */
            output_source->index_has_immediate[0] = false; output_source->index_representations[0] = 2;
            output_source->raw_token = (output_source->raw_token & ~UINT32_C(0x01c00000)) | UINT32_C(0x00800000);
            output_source->rel_op0 = &relative;
            {
                HLSLEmitterContext ctx;
                relative_proved = analyze(&ctx, program);
                if (relative_proved) {
                    const int value = ctx.ssa.relative_operand_ssa_vars[(size_t)fixture.decoded.output * DXBC_MAX_OPERANDS * 4u + 4u];
                    relative_proved = value >= 0 && value < ctx.ssa.ssa_var_count &&
                        ctx.ssa.ssa_var_defs[value] == HLSL_DEFINITION_AMBIGUOUS &&
                        hlsl_relative_operand_definition(&ctx, fixture.decoded.output, 1, 0) == HLSL_DEFINITION_AMBIGUOUS;
                }
                dispose(&ctx);
            }
            break;
        case 10: program->instructions[fixture.inner_if].operands[0].has_abs = true; break;
        }
        const bool rejected = natural_if_rejected(program, NULL);
        if (!rejected) fprintf(stderr, "Nested natural rejection mutation=%u admitted\n", mutation);
        memcpy(program->instructions, instructions, instruction_bytes); program->inputs[0] = input;
        CHECK(rejected && relative_proved); /* Borrowed selector is detached before assertions or cleanup. */
        NaturalIfObservations fresh = {.program = program};
        sb_free(&restored_source); sb_init(&restored_source);
        CHECK(natural_if_emit(program, &restored_source, &restored_map, &restored_quality, &fresh, &diagnostic) &&
            restored_source.len == original.len && !memcmp(restored_source.buf, original.buf, original.len) &&
            natural_if_maps_equal(&original_map, &restored_map) && hlsl_source_quality_results_equal(&original_quality, &restored_quality));
    }
    sb_free(&restored_source); sb_free(&original); natural_if_fixture_dispose(&fixture.decoded);

    /* No demand is clipped merely because the resulting W arithmetic is dead.
     * Its Y input must traverse both retained phi edges to a defined writer. */
    CHECK(natural_nested_fixture(&fixture, 3, 1, true, false, false, false, true));
    program = &fixture.decoded.program;
    const int read = fixture.outer_end + 1;
    HLSLEmitterContext ctx; CHECK(analyze(&ctx, program));
    HLSLIfRegion outer, inner;
    CHECK(hlsl_cfg_if_region(&ctx, fixture.outer_if, &outer) && hlsl_cfg_if_region(&ctx, fixture.inner_if, &inner) &&
        program->instructions[read].opcode == USIL_OP_ADD &&
        usil_operand_destination_lane_mask(&program->instructions[read].operands[0]) == 8 &&
        hlsl_definition_use_count(&ctx, read, 3) == 0);
    const int y = variable(&ctx, read, 1, 3);
    const HLSLPhiNode *outer_y = natural_nested_phi(&ctx, outer.join_block, y);
    CHECK(outer_y && outer_y->register_index == 0 && outer_y->component == 1);
    const HLSLPhiNode *inner_y = NULL;
    for (int edge = 0; edge < 2; ++edge)
        if (outer_y->incoming_vars[edge] >= 0)
            inner_y = inner_y ? inner_y : natural_nested_phi(&ctx, inner.join_block, outer_y->incoming_vars[edge]);
    CHECK(inner_y && inner_y->component == 1 &&
        (inner_y->incoming_vars[0] < 0 || inner_y->incoming_vars[1] < 0));
    dispose(&ctx);
    CHECK(natural_if_rejected(program, NULL)); natural_if_fixture_dispose(&fixture.decoded);

    /* A complete scalar/FLOAT2/FLOAT3 group cannot merge differently sized
     * generations, even though both regions and all outputs remain present. */
    CHECK(natural_nested_fixture(&fixture, 2, 12, false, true, true, false, false));
    program = &fixture.decoded.program;
    const USILInstruction final_else = program->instructions[fixture.inner_else];
    program->instructions[fixture.inner_else].operands[0].destination_mask = 0x40;
    program->instructions[fixture.inner_else].operands[0].raw_token = UINT32_C(0x00100042);
    const bool mismatched = natural_if_rejected(program, NULL);
    program->instructions[fixture.inner_else] = final_else;
    CHECK(mismatched);
    /* Numeric consumption of the actual inner BOOL is separately excluded. */
    const USILInstruction first = program->instructions[fixture.inner_then - 1];
    DXBCOperand *numeric = &program->instructions[fixture.inner_then - 1].operands[1];
    numeric->type = OPERAND_TYPE_TEMP; numeric->register_index = 0; numeric->index_values[0] = 0;
    memset(numeric->swizzle, 2, sizeof(numeric->swizzle));
    numeric->raw_token = natural_if_source_token(OPERAND_TYPE_TEMP, numeric->swizzle);
    const bool bool_numeric = natural_if_rejected(program, NULL);
    program->instructions[fixture.inner_then - 1] = first;
    CHECK(bool_numeric); natural_if_fixture_dispose(&fixture.decoded); return true;
}

static bool check_natural_nested_emission(void) {
    const struct {unsigned width; uint8_t mask; bool dot, inner_else, compared, vertex;} cases[] = {
        {1, 2, false, false, false, false}, {2, 12, false, true, true, false}, {3, 7, false, false, true, false},
        {2, 1, true, false, false, false}, {2, 1, true, true, true, false},
        {3, 1, true, true, true, false}, {4, 1, true, false, true, false}, {3, 7, false, true, true, true}};
    for (unsigned index = 0; index < sizeof(cases) / sizeof(*cases); ++index)
        CHECK(check_natural_nested_positive(cases[index].width, cases[index].mask, cases[index].dot,
            cases[index].inner_else, cases[index].compared, cases[index].vertex));
    CHECK(check_natural_nested_rejections() && check_natural_nested_callbacks()); return true;
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
    CHECK(check_natural_packed_inputs());
    CHECK(check_natural_mad_emission());
    CHECK(check_natural_unique_header_callbacks());
    CHECK(check_natural_dot_emission());
    CHECK(check_natural_source_modifiers());
    CHECK(check_natural_nested_emission());
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

typedef struct {
    NaturalIfFixture decoded;
    int xy_field, zw_field, xy_writer, zw_writer;
    const SerializedProgramParameters *parameters, *common_parameters;
} PackedOutputFixture;

/* The independent output rows share a register, but their RW masks describe
 * the complementary unwritten lanes. All authority comes from the normal
 * document/signature/declaration/USIL decoder chain above. */
typedef struct { unsigned row; bool different_rows, second_buffer; } PackedPositionMADConfig;
typedef struct { unsigned placement; } PackedMatrixReceiptConfig;

static bool packed_output_fixture_layout_position_config(PackedOutputFixture *fixture, unsigned arithmetic,
    bool reversed, bool union_declaration, unsigned boundary, unsigned cbuffer, unsigned position_width,
    const PackedPositionMADConfig *mad, const PackedMatrixReceiptConfig *matrix) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->decoded.document);
    dxbc_stage_contract_init(&fixture->decoded.contract);
    CHECK(arithmetic < 4 && boundary < 3 && cbuffer < 3 && (!boundary || cbuffer != 2));
    CHECK(!position_width || (position_width >= 2 && position_width <= 4 && !boundary && !cbuffer));
    CHECK(!mad || (position_width && mad->row < 2 && arithmetic == 1 && !reversed));
    CHECK(!matrix || (matrix->placement < 3 && cbuffer == 2 && arithmetic == 1 && !mad));
    uint32_t words[96]; size_t count = 0;
#define PACKED_WORD(value) do { CHECK(count < sizeof(words) / sizeof(words[0])); words[count++] = (value); } while (0)
#define PACKED_INST(opcode, length) ((uint32_t)(opcode) | (uint32_t)(length) << 24u)
    PACKED_WORD(PACKED_INST(106, 1) | UINT32_C(1) << 11u);
    for (unsigned input = 0; input < 3; ++input) {
        PACKED_WORD(PACKED_INST(95, 3));
        PACKED_WORD(UINT32_C(0x00101002) | (input || position_width ? UINT32_C(3) : UINT32_C(15)) << 4u);
        PACKED_WORD(input);
    }
    PACKED_WORD(PACKED_INST(103, 4)); PACKED_WORD(UINT32_C(0x001020f2)); PACKED_WORD(0); PACKED_WORD(1);
    for (unsigned row = 0; row < (union_declaration ? 1u : 2u); ++row) {
        const uint8_t mask = union_declaration ? 15 : ((row != 0) == reversed ? 3 : 12);
        PACKED_WORD(PACKED_INST(101, 3));
        PACKED_WORD(UINT32_C(0x00102002) | (uint32_t)mask << 4u); PACKED_WORD(1);
    }
    if (cbuffer || mad) {
        PACKED_WORD(PACKED_INST(89, 4)); PACKED_WORD(UINT32_C(0x00208000)); PACKED_WORD(0);
        PACKED_WORD(mad ? mad->different_rows ? 2u : mad->row + 1u :
            cbuffer == 2 ? matrix && matrix->placement ? 5u : 4u : 1u);
    }
    if (mad && mad->second_buffer) {
        PACKED_WORD(PACKED_INST(89, 4)); PACKED_WORD(UINT32_C(0x00208000)); PACKED_WORD(1); PACKED_WORD(1);
    }
    if (cbuffer == 2) {
        PACKED_WORD(PACKED_INST(104, 2)); PACKED_WORD(1);
        const uint8_t components[4] = {1, 0, 2, 3};
        for (unsigned row = 0; row < 4; ++row) {
            const uint8_t selection[4] = {components[row], components[row], components[row], components[row]};
            PACKED_WORD(PACKED_INST(row ? 50 : 56, row ? 10 : 8));
            PACKED_WORD(row == 3 ? UINT32_C(0x001020f2) : UINT32_C(0x001000f2)); PACKED_WORD(0);
            if (row) {
                PACKED_WORD(UINT32_C(0x00208e46)); PACKED_WORD(0);
                PACKED_WORD(components[row] + (matrix && matrix->placement == 1 ? 1u : 0u));
                PACKED_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, selection)); PACKED_WORD(0);
                PACKED_WORD(UINT32_C(0x00100e46)); PACKED_WORD(0);
            } else {
                PACKED_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, selection)); PACKED_WORD(0);
                PACKED_WORD(UINT32_C(0x00208e46)); PACKED_WORD(0);
                PACKED_WORD(components[row] + (matrix && matrix->placement == 1 ? 1u : 0u));
            }
        }
    } else if (mad) {
        const uint8_t xy_selection[4] = {0, 1, 0, 0}, zw_selection[4] = {2, 3, 2, 2};
        PACKED_WORD(PACKED_INST(50, 11)); PACKED_WORD(UINT32_C(0x00102032)); PACKED_WORD(0);
        PACKED_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, xy_selection)); PACKED_WORD(0);
        PACKED_WORD((natural_if_source_token(OPERAND_TYPE_CONSTANT_BUFFER, xy_selection) & ~(UINT32_C(3) << 20u)) |
            UINT32_C(2) << 20u);
        PACKED_WORD(0); PACKED_WORD(mad->row);
        PACKED_WORD((natural_if_source_token(OPERAND_TYPE_CONSTANT_BUFFER, mad->different_rows ? xy_selection : zw_selection) &
            ~(UINT32_C(3) << 20u)) | UINT32_C(2) << 20u); PACKED_WORD(0); PACKED_WORD(mad->different_rows ? 1u : mad->row);
    } else {
        PACKED_WORD(PACKED_INST(54, 5)); PACKED_WORD(position_width ? UINT32_C(0x00102032) :
            boundary == 1 ? UINT32_C(0x00102072) : UINT32_C(0x001020f2)); PACKED_WORD(0);
        PACKED_WORD(UINT32_C(0x00101e46)); PACKED_WORD(0);
    }
    if (boundary == 1) {
        PACKED_WORD(PACKED_INST(54, 5)); PACKED_WORD(UINT32_C(0x00102082)); PACKED_WORD(0);
        PACKED_WORD(UINT32_C(0x0010103a)); PACKED_WORD(0);
    }
    if (position_width) {
        PACKED_WORD(PACKED_INST(54, 8)); PACKED_WORD(UINT32_C(0x001020c2)); PACKED_WORD(0);
        PACKED_WORD(UINT32_C(0x00004002)); PACKED_WORD(0); PACKED_WORD(0); PACKED_WORD(0); PACKED_WORD(UINT32_C(0x3f800000));
    }
    const USILOpcode xy[] = {USIL_OP_MUL, USIL_OP_ADD, USIL_OP_MAD, USIL_OP_MOV};
    const USILOpcode zw[] = {USIL_OP_ADD, USIL_OP_MUL, USIL_OP_MUL, USIL_OP_MAD};
    for (unsigned write = 0; write < 2; ++write) {
        if (boundary == 2) {
            PACKED_WORD(PACKED_INST(56, 7)); PACKED_WORD(UINT32_C(0x001020f2)); PACKED_WORD(1);
            PACKED_WORD(UINT32_C(0x00101e46)); PACKED_WORD(0);
            PACKED_WORD(UINT32_C(0x00004001)); PACKED_WORD(UINT32_C(0x3ec00000));
            break;
        }
        const bool is_xy = (write != 0) == reversed;
        const uint8_t mask = is_xy ? 3 : 12;
        const uint8_t selection[4] = {0, is_xy ? 1 : 0, 0, is_xy ? 0 : 1};
        const USILOpcode opcode = is_xy ? xy[arithmetic] : zw[arithmetic];
        const uint32_t raw_opcode = opcode == USIL_OP_MOV ? 54 : natural_arithmetic_raw_opcode(opcode);
        const bool cbuffer_source = cbuffer == 1 && is_xy;
        const bool uv_cbuffer = mad && (mad->row || mad->second_buffer) && is_xy;
        const bool matrix_vector_source = matrix && matrix->placement && is_xy;
        const unsigned length = mad ? uv_cbuffer ? 8u : 10u :
            (opcode == USIL_OP_MOV ? 5u : opcode == USIL_OP_MAD ? 9u : 7u) +
            (cbuffer_source || matrix_vector_source ? 1u : 0u);
        PACKED_WORD(PACKED_INST(raw_opcode, length));
        PACKED_WORD(UINT32_C(0x00102002) | (uint32_t)mask << 4u); PACKED_WORD(1);
        if (cbuffer_source) {
            PACKED_WORD(UINT32_C(0x00208046)); PACKED_WORD(0); PACKED_WORD(0);
        } else {
            PACKED_WORD(natural_if_source_token(OPERAND_TYPE_INPUT, selection)); PACKED_WORD(is_xy ? 1 : 2);
        }
        if (matrix_vector_source) {
            PACKED_WORD(UINT32_C(0x00208046)); PACKED_WORD(0);
            PACKED_WORD(matrix->placement == 1 ? 0u : 4u);
        } else if (uv_cbuffer) {
            PACKED_WORD(UINT32_C(0x00208046)); PACKED_WORD(mad->second_buffer ? 1u : 0u); PACKED_WORD(0);
        } else if (mad) {
            PACKED_WORD(UINT32_C(0x00004002));
            for (unsigned component = 0; component < 4; ++component)
                PACKED_WORD((mask & (1u << component)) ?
                    is_xy ? UINT32_C(0x3e000000) : UINT32_C(0x3ec00000) : 0u);
        } else if (opcode != USIL_OP_MOV) {
            PACKED_WORD(UINT32_C(0x00004001));
            PACKED_WORD(opcode == USIL_OP_MUL ? UINT32_C(0x3ec00000) : UINT32_C(0x3e800000));
        }
        if (opcode == USIL_OP_MAD) {
            PACKED_WORD(UINT32_C(0x00004001)); PACKED_WORD(UINT32_C(0x3f000000));
        }
        if (is_xy) fixture->xy_writer = (int)write + (cbuffer == 2 ? 4 : 1) + (boundary == 1 || position_width ? 1 : 0);
        else fixture->zw_writer = (int)write + (cbuffer == 2 ? 4 : 1) + (boundary == 1 || position_width ? 1 : 0);
    }
    PACKED_WORD(PACKED_INST(62, 1));
#undef PACKED_INST
#undef PACKED_WORD
    DXBCSignatureElement inputs[3] = {
        {.semantic_name = "POSITION", .component_type = 3, .mask = 15, .rw_mask = 15},
        {.semantic_name = "VALUE", .register_id = 1, .component_type = 3, .mask = 3, .rw_mask = 3},
        {.semantic_name = "DIRECTION", .register_id = 2, .component_type = 3, .mask = 3, .rw_mask = 3}};
    if (position_width) { inputs[0].mask = (uint8_t)((1u << position_width) - 1u); inputs[0].rw_mask = 3; }
    DXBCSignatureElement outputs[3] = {
        {.semantic_name = "SV_POSITION", .component_type = 3, .system_value = 1, .mask = 15},
        {.semantic_name = "TEXCOORD", .register_id = 1, .component_type = 3},
        {.semantic_name = "TEXCOORD", .register_id = 1, .component_type = 3}};
    fixture->xy_field = reversed ? 2 : 1; fixture->zw_field = reversed ? 1 : 2;
    outputs[fixture->xy_field].mask = 3; outputs[fixture->xy_field].rw_mask = 12;
    outputs[fixture->zw_field].mask = 12; outputs[fixture->zw_field].rw_mask = 3;
    outputs[fixture->zw_field].semantic_index = 1;
    CHECK(natural_if_fixture_decode_words(&fixture->decoded, UINT32_C(0x00010050), words, count,
        inputs, 3, outputs, 3));
    USILProgram *program = &fixture->decoded.program;
    CHECK(program->program_type == DXBC_PROGRAM_TYPE_VERTEX && program->instruction_count ==
        (cbuffer == 2 ? 7 : boundary == 1 || position_width ? 5 : boundary == 2 ? 3 : 4) &&
        program->temp_count == (cbuffer == 2 ? 1 : 0) && program->signature_declaration_count == (union_declaration ? 5 : 6) &&
        program->instructions[0].source_instruction_index == (uint32_t)(program->signature_declaration_count +
            (cbuffer == 2 ? 3 : cbuffer || mad ? 2 : 1) + (mad && mad->second_buffer ? 1 : 0)) &&
        usil_signature_authority_is_valid(program));
    CHECK(program->cbuffer_count == (mad && mad->second_buffer ? 2 : cbuffer || mad ? 1 : 0));
    if (cbuffer || mad) CHECK(program->cbuffers[0].reg_idx == 0 && program->cbuffers[0].size ==
        (mad ? (int)(mad->different_rows ? 2u : mad->row + 1u) :
            cbuffer == 2 ? matrix && matrix->placement ? 5 : 4 : 1) && !program->cbuffers[0].dynamic_indexed);
    if (mad && mad->second_buffer) CHECK(program->cbuffers[1].reg_idx == 1 && program->cbuffers[1].size == 1 &&
        !program->cbuffers[1].dynamic_indexed);
    return true;
}

static bool packed_output_fixture_layout_position(PackedOutputFixture *fixture, unsigned arithmetic,
    bool reversed, bool union_declaration, unsigned boundary, unsigned cbuffer, unsigned position_width) {
    return packed_output_fixture_layout_position_config(fixture, arithmetic, reversed, union_declaration,
        boundary, cbuffer, position_width, NULL, NULL);
}

static bool packed_output_fixture_layout(PackedOutputFixture *fixture, unsigned arithmetic,
    bool reversed, bool union_declaration, unsigned boundary, unsigned cbuffer) {
    return packed_output_fixture_layout_position(fixture, arithmetic, reversed, union_declaration, boundary, cbuffer, 0);
}

static bool packed_output_fixture(PackedOutputFixture *fixture, unsigned arithmetic,
    bool reversed, bool union_declaration) {
    return packed_output_fixture_layout(fixture, arithmetic, reversed, union_declaration, 0, false);
}

typedef struct {
    USILProgram *program;
    StringBuilder *source;
    HLSLExpressionSourceMap *map;
    size_t observations, first_config, first_header, last_header, reject_at, mutate_at, source_offset;
    unsigned mutation;
    int field, writer, xy_writer, zw_writer;
    const HLSLEmitNames *names;
    char *mutable_entry_name;
    SerializedProgramParameters *mutable_parameters;
    char *mutable_field_name;
    SerializedVariable *replacement_fields;
    uint8_t writers_seen;
    size_t cbuffer_events;
    uint8_t cbuffer_authority;
    uint32_t cbuffer_field_bytes;
    uint8_t cbuffer_fields_seen;
    unsigned expected_cbuffer_fields;
    size_t position_assemblies;
    uint8_t position_children;
    bool inspect_position;
    bool inspect_position_mad, mixed_cbuffer_authority;
    const HLSLSourceQualityFacts *expected_cbuffer_facts;
    unsigned expected_cbuffer_fact_count;
    bool wrong_owner, mutated;
} PackedOutputObservations;

static bool observe_packed_output(void *context, const HLSLSourceQualityObservation *observation) {
    PackedOutputObservations *ledger = context;
    ++ledger->observations;
    if (ledger->reject_at == ledger->observations) return false;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (observation->stage != DXBC_PROGRAM_TYPE_VERTEX || observation->pass_index != 3 ||
        observation->entry_point_index != 4 || observation->source_unit_id != 0) ledger->wrong_owner = true;
    if (facts->cbuffer_declaration_kind != HLSL_SOURCE_CBUFFER_NONE) {
        if (ledger->expected_cbuffer_facts) {
            if (ledger->cbuffer_events >= ledger->expected_cbuffer_fact_count ||
                !hlsl_source_quality_facts_equal(facts, &ledger->expected_cbuffer_facts[ledger->cbuffer_events]))
                ledger->wrong_owner = true;
        }
        ++ledger->cbuffer_events;
        if (observation->kind != HLSL_SOURCE_OBSERVATION_EMISSION || !facts->known || facts->instruction_index != -1 ||
            facts->source_instruction_index != UINT32_MAX || facts->cbuffer_binding_register != 0 ||
            (ledger->cbuffer_authority && ledger->cbuffer_authority != facts->cbuffer_declaration_authority &&
                !(ledger->mixed_cbuffer_authority && facts->cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_FIELD)))
            ledger->wrong_owner = true;
        if (facts->cbuffer_declaration_kind != HLSL_SOURCE_CBUFFER_FIELD || !ledger->mixed_cbuffer_authority)
            ledger->cbuffer_authority = facts->cbuffer_declaration_authority;
        if (facts->cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_FIELD && !ledger->expected_cbuffer_facts) {
            const unsigned fields = ledger->expected_cbuffer_fields ? ledger->expected_cbuffer_fields : 1u;
            if (facts->cbuffer_field_index >= fields || facts->cbuffer_byte_offset != facts->cbuffer_field_index * 16u ||
                (ledger->cbuffer_fields_seen & (1u << facts->cbuffer_field_index))) ledger->wrong_owner = true;
            else ledger->cbuffer_fields_seen |= (uint8_t)(1u << facts->cbuffer_field_index);
            ledger->cbuffer_field_bytes = facts->cbuffer_byte_size;
        }
    }
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION && facts->instruction_index < 0 &&
        ledger->source && ledger->source->buf) {
        if (!ledger->first_config && strstr(ledger->source->buf, "// Model:") && !strstr(ledger->source->buf, "float"))
            ledger->first_config = ledger->observations;
        if (!ledger->first_header && strstr(ledger->source->buf, "float")) ledger->first_header = ledger->observations;
        const size_t suffix = sizeof(" output;\n") - 1u;
        if (ledger->source->len >= suffix && !strcmp(ledger->source->buf + ledger->source->len - suffix, " output;\n"))
            ledger->last_header = ledger->observations;
    }
    if (facts->instruction_index >= 0) {
        if (facts->instruction_index >= ledger->program->instruction_count || facts->source_instruction_index !=
            ledger->program->instructions[facts->instruction_index].source_instruction_index) ledger->wrong_owner = true;
        else if ((observation->ast_kind == AST_EXPR_BINARY || observation->ast_kind == AST_EXPR_CALL) &&
            facts->known && facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
            (facts->instruction_index == ledger->xy_writer || facts->instruction_index == ledger->zw_writer) &&
            facts->lanes == usil_operand_destination_lane_mask(
                &ledger->program->instructions[facts->instruction_index].operands[0])) {
            if (facts->components != 2 || facts->logical_value_id != (uint64_t)(unsigned)facts->instruction_index)
                ledger->wrong_owner = true;
            ledger->writers_seen |= (uint8_t)(1u << (unsigned)facts->instruction_index);
        }
    }
    if (ledger->inspect_position && observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION) {
        if (facts->logical_value_id == HLSL_POSITION_OUTPUT_LOGICAL_ID) {
            if (!facts->known || facts->value_kind != HLSL_SOURCE_VALUE_LOGICAL || facts->components != 4 ||
                facts->instruction_index != -1 || facts->source_instruction_index != UINT32_MAX || facts->lanes ||
                facts->artifacts || observation->ast_kind != AST_EXPR_CALL) ledger->wrong_owner = true;
            ++ledger->position_assemblies;
        }
        if (facts->known && facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL && facts->instruction_index >= 0 &&
            facts->instruction_index < 2 && facts->components == 2 && facts->lanes == (facts->instruction_index ? 12 : 3)) {
            const bool first_kind = ledger->inspect_position_mad
                ? observation->ast_kind == AST_EXPR_BINARY || observation->ast_kind == AST_EXPR_EMITTER_OPERAND
                : observation->ast_kind == AST_EXPR_EMITTER_OPERAND;
            if (facts->artifacts || (facts->instruction_index ? observation->ast_kind != AST_EXPR_LITERAL : !first_kind))
                ledger->wrong_owner = true;
            ledger->position_children |= (uint8_t)(1u << (unsigned)facts->instruction_index);
        }
    }
    if (!ledger->mutation || ledger->observations != ledger->mutate_at) return true;
    USILProgram *program = ledger->program;
    switch (ledger->mutation) {
    case 1:
        memset(program->outputs[ledger->field].semantic_name, 0, sizeof(program->outputs[ledger->field].semantic_name));
        memcpy(program->outputs[ledger->field].semantic_name, "COORDVALUE", sizeof("COORDVALUE"));
        program->outputs[ledger->field].semantic_name_length = sizeof("COORDVALUE") - 1u;
        break;
    case 2:
        program->instructions[ledger->writer].operands[2].imm_values[0] = UINT32_C(0x3f400000);
        program->instructions[ledger->writer].operands[2].immediate_words[0] = UINT32_C(0x3f400000);
        break;
    case 3: {
        const USILInstruction old = program->instructions[1];
        const uint32_t first_raw = old.source_instruction_index, second_raw = program->instructions[2].source_instruction_index;
        program->instructions[1] = program->instructions[2]; program->instructions[2] = old;
        program->instructions[1].source_instruction_index = first_raw; program->instructions[2].source_instruction_index = second_raw;
        break;
    }
    case 4:
        if (!ledger->source || ledger->source_offset >= ledger->source->len) return false;
        ledger->source->buf[ledger->source_offset] ^= 1;
        break;
    case 5:
        if (!ledger->source) return false;
        sb_append(ledger->source, "static const float injectedValue = 1.0f;\n");
        if (!sb_ok(ledger->source)) return false;
        break;
    case 6: {
        if (!ledger->source) return false;
        char *type = strstr(ledger->source->buf, "float");
        if (!type) return false;
        *type ^= 1;
        break;
    }
    case 7:
        if (!ledger->map || ledger->map->count < 3) return false;
        ++ledger->map->origins[2].source_end;
        break;
    case 8: {
        DXBCOperand *source = &program->instructions[ledger->writer].operands[1];
        source->register_index = source->register_index == 1 ? 2 : 1;
        source->index_values[0] = (uint32_t)source->register_index;
        break;
    }
    case 9:
        if (!ledger->mutable_entry_name || !ledger->mutable_entry_name[0]) return false;
        ledger->mutable_entry_name[0] = ledger->mutable_entry_name[0] == 'e' ? 'E' : 'e';
        break;
    case 10:
        if (!ledger->mutable_field_name) return false;
        memcpy(ledger->mutable_field_name, "_AdjustedScale", sizeof("_AdjustedScale"));
        break;
    case 11:
        if (!ledger->mutable_parameters || ledger->mutable_parameters->cb_count != 1 ||
            ledger->mutable_parameters->constant_buffers[0].var_count != 1) return false;
        ledger->mutable_parameters->constant_buffers[0].variables[0].layout[3] = 4;
        break;
    case 12:
        memset(program->inputs[0].semantic_name, 0, sizeof(program->inputs[0].semantic_name));
        memcpy(program->inputs[0].semantic_name, "POINTVALUE", sizeof("POINTVALUE"));
        program->inputs[0].semantic_name_length = sizeof("POINTVALUE") - 1u;
        break;
    case 13:
        program->instructions[1].operands[1].imm_values[3] = UINT32_C(0x40000000);
        program->instructions[1].operands[1].immediate_words[3] = UINT32_C(0x40000000);
        break;
    case 14:
        program->instructions[0].operands[1].register_index = 1;
        program->instructions[0].operands[1].index_values[0] = 1;
        break;
    case 15:
        if (!ledger->mutable_parameters || ledger->mutable_parameters->cb_count != 1 ||
            !ledger->mutable_parameters->constant_buffers[0].var_count) return false;
        ++ledger->mutable_parameters->constant_buffers[0].variables[0].layout[ledger->mutable_parameters->is_binary ? 1 : 5];
        break; /* Semantically dormant vector word still belongs to the lease. */
    case 16:
        if (!ledger->mutable_parameters || ledger->mutable_parameters->cb_count != 1 || !ledger->replacement_fields) return false;
        ledger->mutable_parameters->constant_buffers[0].variables = ledger->replacement_fields;
        break;
    case 17: {
        DXBCOperand *third = &program->instructions[0].operands[3];
        third->rel_offset0 = 0; third->index_values[1] = 0;
        break;
    }
    case 18:
        program->instructions[ledger->writer].operands[2].imm_values[2] = UINT32_C(0x3f400000);
        program->instructions[ledger->writer].operands[2].immediate_words[2] = UINT32_C(0x3f400000);
        program->instructions[ledger->writer].operands[2].imm_values[3] = UINT32_C(0x3f400000);
        program->instructions[ledger->writer].operands[2].immediate_words[3] = UINT32_C(0x3f400000);
        break;
    }
    ledger->mutated = true;
    return true;
}

static bool packed_output_emit(PackedOutputFixture *fixture, StringBuilder *source,
    HLSLExpressionSourceMap *map, HLSLSourceQualityResult *quality, PackedOutputObservations *observer,
    HLSLEmitDiagnostic *diagnostic) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = map; options.source_quality = quality;
    options.source_quality_pass_index = 3; options.source_quality_entry_point_index = 4;
    if (observer) {
        observer->program = &fixture->decoded.program; observer->source = source; observer->map = map;
        observer->xy_writer = fixture->xy_writer; observer->zw_writer = fixture->zw_writer;
        options.source_quality_observer = observe_packed_output; options.source_quality_observer_context = observer;
    }
    return hlsl_emit_with_options_diagnostic(&fixture->decoded.program, source, fixture->parameters, fixture->common_parameters,
        observer ? observer->names : NULL, &options, diagnostic);
}

static bool packed_output_clean(const HLSLSourceQualityResult *quality) {
    CHECK(quality->classification == HLSL_SOURCE_QUALITY_CLEAN && !quality->reasons &&
        quality->counts.inspected_units == 1 && !quality->counts.incomplete_units &&
        !quality->counts.residual_total && !quality->counts.unknown_provenance);
    return true;
}

static bool packed_output_prepare_context(PackedOutputFixture *fixture, HLSLEmitterContext *ctx,
    StringBuilder *scratch, HLSLEmitDiagnostic *diagnostic) {
    CHECK(analyze(ctx, &fixture->decoded.program)); sb_init(scratch); hlsl_emit_diagnostic_init(diagnostic);
    ctx->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE; ctx->sb = scratch; ctx->diagnostic = diagnostic;
    ctx->entry_point_name = "main"; ctx->preferred_input_struct_name = "appdata"; ctx->preferred_output_struct_name = "v2f";
    ctx->params = fixture->parameters; ctx->common_params = fixture->common_parameters;
    if (ctx->program->cbuffer_count) CHECK(build_cbuffer_register_map(ctx) && build_cbuffer_emission_layouts(ctx));
    const bool candidate = hlsl_packed_output_candidate(ctx->program);
    const bool preflight = candidate && hlsl_packed_output_preflight(ctx);
    if (!candidate || !preflight) fprintf(stderr,
        "Packed output private setup: candidate=%d preflight=%d blocks=%d instructions=%d outputs=%d declarations=%d flags=%u status=%s reason=%s\n",
        candidate, preflight, ctx->cfg.block_count, ctx->program->instruction_count, ctx->program->output_count,
        ctx->program->signature_declaration_count, ctx->program->global_flags, hlsl_emit_status_name(diagnostic->status),
        hlsl_emit_reason_name(diagnostic->reason));
    CHECK(candidate && preflight);
    ctx->high_level_packed_outputs = true; ctx->high_level_interface = true;
    CHECK(hlsl_prepare_high_level_interface(ctx));
    return true;
}

static bool packed_output_owned_roots(PackedOutputFixture *fixture, ASTExpr *roots[2], char names[2][96]) {
    HLSLEmitterContext ctx; StringBuilder scratch; HLSLEmitDiagnostic diagnostic;
    CHECK(packed_output_prepare_context(fixture, &ctx, &scratch, &diagnostic));
    const int writers[2] = {fixture->xy_writer, fixture->zw_writer};
    const int fields[2] = {fixture->xy_field, fixture->zw_field};
    for (unsigned item = 0; item < 2; ++item) {
        const int index = writers[item]; const USILInstruction *instruction = &ctx.program->instructions[index];
        const uint8_t mask = item ? 12 : 3;
        const DXBCInstruction *raw = NULL;
        for (int token = 0; token < fixture->decoded.semantic.instruction_count; ++token) {
            const DXBCInstruction *candidate = &fixture->decoded.semantic.instructions[token];
            if (candidate->has_raw_instruction_index && candidate->raw_instruction_index == instruction->source_instruction_index) {
                CHECK(!raw); raw = candidate;
            }
        }
        CHECK(raw && raw->operand_count == instruction->operand_count && raw->opcode ==
            (instruction->opcode == USIL_OP_MOV ? 54u : natural_arithmetic_raw_opcode(instruction->opcode)) &&
            usil_instruction_shape_valid(ctx.program, instruction));
        HLSLNaturalOutputProjection projection;
        CHECK(hlsl_high_level_output_projection(ctx.program, &instruction->operands[0], &projection) &&
            projection.field_index == fields[item] && projection.register_index == 1 && projection.field_mask == mask &&
            projection.natural_components == 2 && projection.packed);
        const char *name = hlsl_high_level_output_field_name(&ctx, fields[item]);
        CHECK(name && *name && strlen(name) < sizeof(names[item])); strcpy(names[item], name);
        ctx.current_instruction_index = index;
        ASTExpr *left = hlsl_natural_source_atom(&ctx, index, 1, mask);
        ASTExpr *right = instruction->operand_count > 2 ? hlsl_natural_source_atom(&ctx, index, 2, mask) : NULL;
        ASTExpr *third = instruction->operand_count > 3 ? hlsl_natural_source_atom(&ctx, index, 3, mask) : NULL;
        CHECK(left && left->kind == AST_EXPR_EMITTER_OPERAND && left->operand_provenance.complete &&
            left->operand_provenance.instruction_index == index && left->operand_provenance.operand_index == 1 &&
            left->operand_provenance.source_instruction_index == instruction->source_instruction_index &&
            left->operand_provenance.destination_lanes == mask && left->operand_provenance.natural_components == 2 &&
            left->operand_provenance.result_components == 2 && left->operand_provenance.selected_components[0] == 0 &&
            left->operand_provenance.selected_components[1] == 1);
        ASTExpr *literals[2] = {right, third};
        for (int literal = 0; literal < instruction->operand_count - 2; ++literal) {
            const DXBCOperand *owner = &instruction->operands[literal + 2];
            CHECK(owner->type == OPERAND_TYPE_IMMEDIATE32 && owner->imm_value_count == 1 &&
                owner->immediate_word_count == 1 && owner->immediate_words[0] == owner->imm_values[0] &&
                literals[literal] && literals[literal]->kind == AST_EXPR_LITERAL &&
                literals[literal]->u.literal.scalar_type == AST_SCALAR_FLOAT32 && literals[literal]->u.literal.components == 2 &&
                literals[literal]->u.literal.val[0] == owner->imm_values[0] && literals[literal]->u.literal.val[1] == owner->imm_values[0]);
        }
        roots[item] = hlsl_natural_float_operation(&ctx, index, left, right, third);
        CHECK(roots[item]);
        if (instruction->opcode == USIL_OP_MOV) CHECK(roots[item] == left);
        else if (instruction->opcode == USIL_OP_MAD) CHECK(roots[item]->kind == AST_EXPR_BINARY &&
            roots[item]->u.binary.op == USIL_OP_ADD && roots[item]->u.binary.right == third &&
            roots[item]->u.binary.left->kind == AST_EXPR_BINARY && roots[item]->u.binary.left->u.binary.op == USIL_OP_MUL &&
            roots[item]->u.binary.left->u.binary.left == left && roots[item]->u.binary.left->u.binary.right == right);
        else CHECK(roots[item]->kind == AST_EXPR_BINARY && roots[item]->u.binary.op == (int)instruction->opcode &&
            roots[item]->u.binary.left == left && roots[item]->u.binary.right == right);
        if (instruction->opcode != USIL_OP_MOV) CHECK(roots[item]->logical_origin.complete &&
            roots[item]->logical_origin.scalar_type == AST_SCALAR_FLOAT32 && roots[item]->logical_origin.components == 2 &&
            roots[item]->logical_origin.logical_value_id == (uint64_t)(unsigned)index &&
            roots[item]->logical_origin.instruction_index == index && roots[item]->logical_origin.source_instruction_index ==
            instruction->source_instruction_index && roots[item]->logical_origin.destination_lanes == mask);
    }
    CHECK(strcmp(names[0], names[1])); free_cbuffer_emission_layouts(&ctx); free(ctx.cb_reg_map);
    dispose(&ctx); sb_free(&scratch);
    return true;
}

static bool check_packed_output_positive(unsigned arithmetic, bool reversed, bool union_declaration) {
    PackedOutputFixture fixture; CHECK(packed_output_fixture(&fixture, arithmetic, reversed, union_declaration));
    ASTExpr *roots[2] = {0}; char names[2][96]; CHECK(packed_output_owned_roots(&fixture, roots, names));
    StringBuilder baseline, repeated, owned_text; sb_init(&baseline); sb_init(&repeated); sb_init(&owned_text);
    HLSLExpressionSourceMap map, repeated_map; HLSLSourceQualityResult quality, repeated_quality; HLSLEmitDiagnostic diagnostic;
    PackedOutputObservations observer = {0};
    const bool emitted = packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic);
    if (!emitted) fprintf(stderr, "Packed output arithmetic=%u reversed=%d union=%d status=%s phase=%d reason=%s instruction=%d\n",
        arithmetic, reversed, union_declaration, hlsl_emit_status_name(diagnostic.status), (int)diagnostic.phase,
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted && packed_output_clean(&quality) && !observer.wrong_owner && observer.observations > 2 &&
        map.complete && map.count == 4 && hlsl_expression_source_map_matches(&map, &fixture.decoded.program, baseline.buf));
    uint8_t arithmetic_writers = 0;
    for (unsigned index = 1; index < 3; ++index)
        if (fixture.decoded.program.instructions[index].opcode != USIL_OP_MOV) arithmetic_writers |= (uint8_t)(1u << index);
    CHECK(observer.writers_seen == arithmetic_writers);
    CHECK(strstr(baseline.buf, " : TEXCOORD0;") && strstr(baseline.buf, " : TEXCOORD1;") &&
        strstr(baseline.buf, " : SV_POSITION;") && !strstr(baseline.buf, "u_xlat") && !strstr(baseline.buf, "float4 r1"));
    for (unsigned item = 0; item < 2; ++item) {
        const int writer = item ? fixture.zw_writer : fixture.xy_writer;
        char declaration[128], assignment[128];
        snprintf(declaration, sizeof(declaration), "float2 %.95s : TEXCOORD%u;", names[item], item);
        snprintf(assignment, sizeof(assignment), "output.%.95s = ", names[item]);
        CHECK(strstr(baseline.buf, declaration) && strstr(baseline.buf, assignment));
        const HLSLExpressionOrigin *origin = &map.origins[writer];
        CHECK(origin->instruction_index == writer && origin->source_instruction_index ==
            fixture.decoded.program.instructions[writer].source_instruction_index && origin->destination_lanes == (item ? 12 : 3) &&
            origin->source_begin < origin->source_end && origin->source_end <= baseline.len);
        StringBuilder expression; sb_init(&expression); ast_format_expr(roots[item], &expression);
        CHECK(sb_ok(&expression) && expression.len);
        const char *printed = strstr(baseline.buf + origin->source_begin, expression.buf);
        CHECK(printed && printed + expression.len <= baseline.buf + origin->source_end); sb_free(&expression);
        char physical[128]; snprintf(physical, sizeof(physical), "output.%.95s.", names[item]); CHECK(!strstr(baseline.buf, physical));
    }
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        sb_free(&repeated); sb_init(&repeated);
        CHECK(packed_output_emit(&fixture, &repeated, outputs & 1u ? &repeated_map : NULL,
            outputs & 2u ? &repeated_quality : NULL, NULL, &diagnostic) && repeated.len == baseline.len &&
            !memcmp(repeated.buf, baseline.buf, baseline.len));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&map, &repeated_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &repeated_quality));
    }
    const size_t points[] = {1, observer.observations / 2u, observer.observations};
    for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
        PackedOutputObservations veto = {.reject_at = points[point]}; sb_free(&repeated); sb_init(&repeated);
        CHECK(!packed_output_emit(&fixture, &repeated, &repeated_map, &repeated_quality, &veto, &diagnostic) &&
            veto.observations == points[point] && !repeated_map.complete && !repeated_map.count &&
            repeated_quality.classification == HLSL_SOURCE_QUALITY_FAILED && !repeated.len &&
            (!repeated.buf || !repeated.buf[0]));
    }
    sb_free(&repeated); sb_init(&repeated);
    CHECK(packed_output_emit(&fixture, &repeated, &repeated_map, &repeated_quality, NULL, &diagnostic) &&
        repeated.len == baseline.len && !memcmp(repeated.buf, baseline.buf, baseline.len) &&
        natural_if_maps_equal(&map, &repeated_map) && hlsl_source_quality_results_equal(&quality, &repeated_quality));
    for (unsigned item = 0; item < 2; ++item) { ast_format_expr(roots[item], &owned_text); sb_append(&owned_text, "\n"); }
    CHECK(sb_ok(&owned_text) && owned_text.len); const size_t owned_size = owned_text.len;
    char *owned_bytes = malloc(owned_size + 1u); CHECK(owned_bytes); memcpy(owned_bytes, owned_text.buf, owned_size + 1u);
    natural_if_fixture_dispose(&fixture.decoded); sb_free(&owned_text); sb_init(&owned_text);
    for (unsigned item = 0; item < 2; ++item) { ast_format_expr(roots[item], &owned_text); sb_append(&owned_text, "\n"); }
    CHECK(sb_ok(&owned_text) && owned_text.len == owned_size && !memcmp(owned_bytes, owned_text.buf, owned_size + 1u));
    free(owned_bytes); ast_free_expr(roots[0]); ast_free_expr(roots[1]);
    sb_free(&owned_text); sb_free(&baseline); sb_free(&repeated); return true;
}

static bool check_packed_output_rejections(void) {
    PackedOutputFixture fixture; CHECK(packed_output_fixture(&fixture, 0, false, false));
    USILProgram *program = &fixture.decoded.program;
    USILInstruction instructions[4]; DXBCSignatureElement outputs[3]; USILSignatureDeclaration declarations[6];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(outputs, program->outputs, sizeof(outputs));
    CHECK(program->signature_declaration_count == 6); memcpy(declarations, program->signature_declarations, sizeof(declarations));
    for (unsigned mutation = 0; mutation < 17; ++mutation) {
        DXBCOperand relative = {0}; uint32_t extension = UINT32_C(0x41);
        switch (mutation) {
        case 0: program->outputs[2].mask = 6; program->outputs[2].rw_mask = 9; break; /* overlap and hole */
        case 1: program->outputs[2].mask = 8; program->outputs[2].rw_mask = 7; break;
        case 2: program->outputs[1].rw_mask = 8; break;
        case 3: program->outputs[1].component_type = 1; break;
        case 4: program->outputs[2].system_value = 1; break;
        case 5: program->instructions[1].operands[0].destination_mask = 16; break; /* only X */
        case 6: program->instructions[2].operands[0] = program->instructions[1].operands[0]; break;
        case 7: program->instructions[2].opcode = USIL_OP_NOP; program->instructions[2].operand_count = 0; break;
        case 8: {
            DXBCOperand *destination = &program->instructions[1].operands[0];
            relative = program->instructions[0].operands[1]; relative.swizzle_mode = 2;
            memset(relative.swizzle, 0, sizeof(relative.swizzle)); relative.raw_token = UINT32_C(0x0010100a);
            destination->index_representations[0] = 2;
            destination->index_has_immediate[0] = false; destination->rel_op0 = &relative;
            destination->raw_token = (destination->raw_token & ~(UINT32_C(7) << 22u)) | UINT32_C(2) << 22u;
            break;
        }
        case 9: {
            DXBCOperand *source = &program->instructions[1].operands[1]; source->has_neg = true;
            source->raw_token |= UINT32_C(0x80000000); source->extended_tokens = &extension; source->extended_token_count = 1;
            break;
        }
        case 10: program->instructions[2].operands[1].type = OPERAND_TYPE_OUTPUT;
            program->instructions[2].operands[1].register_index = 1;
            program->instructions[2].operands[1].index_values[0] = 1;
            program->instructions[2].operands[1].raw_token = natural_if_source_token(OPERAND_TYPE_OUTPUT,
                program->instructions[2].operands[1].swizzle); break;
        case 11: program->instructions[0].operands[0].destination_mask = 112; break; /* split POSITION not assembled */
        case 12: program->instructions[1].operands[0].destination_mask = 240; break; /* one write crosses both fields */
        case 13: program->outputs[2].semantic_index = 0; break;
        case 14: program->signature_declarations[5].mask = 8; break;
        case 15: program->instructions[1].precise_mask = 1; break;
        case 16: program->instructions[2].operands[1].min_precision = 1; break;
        }
        HLSLEmitDiagnostic diagnostic; const bool rejected = natural_if_rejected(program, &diagnostic);
        /* Borrowed relative/extension storage is removed before decoder cleanup
         * on both the expected and unexpectedly admitted paths. */
        memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->outputs, outputs, sizeof(outputs));
        memcpy(program->signature_declarations, declarations, sizeof(declarations));
        if (!rejected) fprintf(stderr, "Packed output unexpectedly admitted mutation=%u\n", mutation);
        CHECK(rejected);
    }
    CHECK(usil_signature_authority_is_valid(program)); natural_if_fixture_dispose(&fixture.decoded);
    for (unsigned boundary = 1; boundary < 3; ++boundary) {
        CHECK(packed_output_fixture_layout(&fixture, 0, false, false, boundary, false) &&
            usil_signature_authority_is_valid(&fixture.decoded.program));
        CHECK(natural_if_rejected(&fixture.decoded.program, NULL)); natural_if_fixture_dispose(&fixture.decoded);
    }
    return true;
}

static bool check_packed_output_callbacks(void) {
    PackedOutputFixture fixture; CHECK(packed_output_fixture(&fixture, 0, false, false));
    USILProgram *program = &fixture.decoded.program; const char prefix[] = "// caller prefix\n";
    char entry_name[] = "entryName"; const HLSLEmitNames names = {.entry_point = entry_name};
    StringBuilder baseline, changed; sb_init(&baseline); sb_init(&changed); sb_append(&baseline, prefix);
    HLSLExpressionSourceMap map, changed_map; HLSLSourceQualityResult quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    PackedOutputObservations observer = {.names = &names};
    CHECK(packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic) && packed_output_clean(&quality) &&
        observer.first_config && observer.first_header > observer.first_config && observer.last_header > observer.first_header &&
        observer.observations > observer.last_header);
    USILInstruction instructions[4]; DXBCSignatureElement outputs[3];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(outputs, program->outputs, sizeof(outputs));
    const struct {unsigned mutation; size_t point, offset;} attacks[] = {
        {1, 1, 0}, {2, observer.observations / 2u, 0}, {3, observer.observations, 0},
        {8, observer.observations, 0}, {9, observer.observations, 0},
        {6, observer.first_header, 0}, {5, observer.last_header, 0},
        {5, observer.first_config, 0}, {4, observer.first_config, sizeof(prefix)},
        {4, 1, 0}, {4, observer.observations, 0},
        {4, observer.observations, map.origins[fixture.xy_writer].source_begin}, {7, observer.observations, 0}};
    for (unsigned attack = 0; attack < sizeof(attacks) / sizeof(*attacks); ++attack) {
        PackedOutputObservations drift = {.mutation = attacks[attack].mutation, .mutate_at = attacks[attack].point,
            .source_offset = attacks[attack].offset, .field = fixture.zw_field, .writer = fixture.xy_writer,
            .names = &names, .mutable_entry_name = entry_name};
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        const bool rejected = !packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic);
        if (!rejected) fprintf(stderr, "Packed output callback falsely accepted attack=%u action=%u point=%zu\n",
            attack, drift.mutation, drift.mutate_at);
        CHECK(rejected && drift.mutated && !changed_map.complete && !changed_map.count &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && changed.len == sizeof(prefix) - 1u &&
            changed.buf && !strcmp(changed.buf, prefix));
        if (drift.mutation == 1 || drift.mutation == 2 || drift.mutation == 3 || drift.mutation == 8 || drift.mutation == 9) {
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            PackedOutputObservations fresh = {.names = &names};
            CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                packed_output_clean(&changed_quality) && hlsl_expression_source_map_matches(&changed_map, program, changed.buf) &&
                (changed.len != baseline.len || memcmp(changed.buf, baseline.buf, baseline.len)));
        } else CHECK(!memcmp(program->instructions, instructions, sizeof(instructions)) &&
            !memcmp(program->outputs, outputs, sizeof(outputs)));
        memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->outputs, outputs, sizeof(outputs));
        memcpy(entry_name, "entryName", sizeof(entry_name));
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        PackedOutputObservations restored = {.names = &names};
        CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &restored, &diagnostic) &&
            changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) &&
            natural_if_maps_equal(&map, &changed_map) && hlsl_source_quality_results_equal(&quality, &changed_quality));
    }
    sb_free(&baseline); sb_free(&changed); natural_if_fixture_dispose(&fixture.decoded); return true;
}

typedef struct {
    char buffer_names[2][32], field_names[2][32];
    SerializedVariable fields[2];
    SerializedConstantBuffer buffers[2];
    SerializedResourceParam bindings[2];
    SerializedProgramParameters parameters[2];
} PackedOutputCBufferMetadata;

static void packed_output_cbuffer_metadata(PackedOutputCBufferMetadata *metadata) {
    memset(metadata, 0, sizeof(*metadata));
    for (unsigned source = 0; source < 2; ++source) {
        memcpy(metadata->buffer_names[source], "PackedInputs", sizeof("PackedInputs"));
        memcpy(metadata->field_names[source], "_Scale", sizeof("_Scale"));
        metadata->fields[source] = (SerializedVariable){metadata->field_names[source], {0, 0, 0, 2, 0, 0}};
        metadata->buffers[source] = (SerializedConstantBuffer){.name = metadata->buffer_names[source],
            .role = SERIALIZED_CBUFFER_NAMED, .size = 16, .variables = &metadata->fields[source], .var_count = 1};
        metadata->bindings[source] = (SerializedResourceParam){.name = metadata->buffer_names[source],
            .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER};
        metadata->parameters[source] = (SerializedProgramParameters){.constant_buffers = &metadata->buffers[source],
            .cb_count = 1, .resources = &metadata->bindings[source], .res_count = 1};
    }
}

static bool check_packed_output_cbuffer(void) {
    PackedOutputFixture fixture; CHECK(packed_output_fixture_layout(&fixture, 0, false, false, 0, true));
    PackedOutputCBufferMetadata metadata; packed_output_cbuffer_metadata(&metadata);
    fixture.parameters = &metadata.parameters[0];
    ASTExpr *roots[2] = {0}; char names[2][96]; CHECK(packed_output_owned_roots(&fixture, roots, names));
    const ASTExpr *read = roots[0]->u.binary.left;
    CHECK(read->kind == AST_EXPR_EMITTER_OPERAND && read->operand_provenance.complete &&
        read->operand_provenance.value_role == AST_OPERAND_VALUE_LOGICAL &&
        read->operand_provenance.logical_value_id == (UINT64_C(1) << 32) &&
        read->operand_provenance.natural_components == 2 && read->operand_provenance.result_components == 2 &&
        read->operand_provenance.instruction_index == fixture.xy_writer && read->operand_provenance.operand_index == 1 &&
        read->operand_provenance.source_instruction_index == fixture.decoded.program.instructions[fixture.xy_writer].source_instruction_index &&
        read->operand_provenance.destination_lanes == 3);
    StringBuilder held, retained; sb_init(&held); sb_init(&retained); ast_format_expr(roots[0], &held);
    CHECK(sb_ok(&held) && strstr(held.buf, "_Scale"));
    const char prefix[] = "// caller prefix\n";
    StringBuilder baseline, changed; sb_init(&baseline); sb_init(&changed);
    HLSLExpressionSourceMap map, changed_map; HLSLSourceQualityResult quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    for (unsigned authority = 0; authority < 2; ++authority) {
        fixture.parameters = authority ? NULL : &metadata.parameters[0];
        fixture.common_parameters = authority ? &metadata.parameters[1] : NULL;
        sb_free(&baseline); sb_init(&baseline); sb_append(&baseline, prefix);
        PackedOutputObservations observer = {0};
        const bool emitted = packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic);
        if (!emitted) fprintf(stderr, "Packed CB authority=%u status=%s phase=%s reason=%s\n", authority,
            hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase), hlsl_emit_reason_name(diagnostic.reason));
        CHECK(emitted && packed_output_clean(&quality) && !observer.wrong_owner && observer.cbuffer_events == 3 &&
            observer.cbuffer_authority == authority + 1u && observer.cbuffer_field_bytes == 8 &&
            quality.counts.cbuffer_declarations == 1 && quality.counts.cbuffer_fields == 1 &&
            strstr(baseline.buf, "cbuffer PackedInputs : register(b0)") &&
            strstr(baseline.buf, "float2 _Scale : packoffset(c0)") &&
            map.complete && hlsl_expression_source_map_matches(&map, &fixture.decoded.program, baseline.buf));
        for (unsigned outputs = 0; outputs < 4; ++outputs) {
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(packed_output_emit(&fixture, &changed, outputs & 1u ? &changed_map : NULL,
                outputs & 2u ? &changed_quality : NULL, NULL, &diagnostic) &&
                changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len));
            if (outputs & 1u) CHECK(natural_if_maps_equal(&map, &changed_map));
            if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &changed_quality));
        }
        const size_t points[2] = {1, observer.observations};
        for (unsigned change = 0; change < 2; ++change) for (unsigned point = 0; point < 2; ++point) {
            PackedOutputObservations drift = {.mutation = change ? 11 : 10, .mutate_at = points[point],
                .mutable_parameters = &metadata.parameters[authority], .mutable_field_name = metadata.field_names[authority]};
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(!packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic) &&
                drift.mutated && !changed_map.complete && !changed_map.count &&
                changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && changed.len == sizeof(prefix) - 1u &&
                changed.buf && !strcmp(changed.buf, prefix));
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
                packed_output_clean(&changed_quality) && hlsl_expression_source_map_matches(&changed_map, &fixture.decoded.program, changed.buf) &&
                (changed.len != baseline.len || memcmp(changed.buf, baseline.buf, baseline.len)));
            memset(metadata.field_names[authority], 0, sizeof(metadata.field_names[authority]));
            memcpy(metadata.field_names[authority], "_Scale", sizeof("_Scale")); metadata.fields[authority].layout[3] = 2;
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
                changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) &&
                natural_if_maps_equal(&map, &changed_map) && hlsl_source_quality_results_equal(&quality, &changed_quality));
        }
    }
    /* The selected current model remains intact. The same-name common model
     * is dormant read authority, but still belongs to this emission lease. */
    fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = &metadata.parameters[1];
    sb_free(&baseline); sb_init(&baseline); sb_append(&baseline, prefix);
    PackedOutputObservations observer = {0}; CHECK(packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic));
    PackedOutputObservations common_drift = {.mutation = 10, .mutate_at = observer.observations,
        .mutable_field_name = metadata.field_names[1]};
    sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
    CHECK(!packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &common_drift, &diagnostic) && common_drift.mutated &&
        !changed_map.complete && !changed_map.count && changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
        changed.len == sizeof(prefix) - 1u && changed.buf && !strcmp(changed.buf, prefix));
    memset(metadata.field_names[1], 0, sizeof(metadata.field_names[1])); memcpy(metadata.field_names[1], "_Scale", sizeof("_Scale"));
    sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
    CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
        changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) && natural_if_maps_equal(&map, &changed_map) &&
        hlsl_source_quality_results_equal(&quality, &changed_quality));
    for (unsigned failure = 0; failure < 2; ++failure) {
        fixture.parameters = failure ? &metadata.parameters[0] : NULL;
        fixture.common_parameters = failure ? &metadata.parameters[1] : NULL;
        if (failure) metadata.buffers[1].size = 32;
        sb_free(&changed); sb_init(&changed);
        CHECK(!packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
            diagnostic.status != HLSL_EMIT_STATUS_OK && !changed_map.complete && !changed_map.count &&
            changed_quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        metadata.buffers[1].size = 16;
    }
    natural_if_fixture_dispose(&fixture.decoded); memset(&metadata, 0, sizeof(metadata));
    ast_format_expr(roots[0], &retained);
    CHECK(sb_ok(&retained) && retained.len == held.len && !memcmp(retained.buf, held.buf, held.len));
    ast_free_expr(roots[0]); ast_free_expr(roots[1]); sb_free(&held); sb_free(&retained); sb_free(&baseline); sb_free(&changed);
    return true;
}

static bool packed_output_owned_matrix(PackedOutputFixture *fixture, ASTExpr **owned) {
    HLSLEmitterContext ctx; StringBuilder scratch; HLSLEmitDiagnostic diagnostic;
    CHECK(packed_output_prepare_context(fixture, &ctx, &scratch, &diagnostic));
    HLSLMatrixLiftPlan plan;
    CHECK(hlsl_matrix_lift_prepare(&ctx, 0, &plan) && plan.start_instruction == 0 && plan.end_instruction == 3 &&
        plan.claimed_instruction_count == 4 && plan.result_components == 4 && !plan.world_expression &&
        plan.instruction_owners.words[0] == UINT64_C(15));
    const USILProgram *program = ctx.program;
    const uint8_t components[4] = {1, 0, 2, 3};
    for (int instruction = 0; instruction < 4; ++instruction) {
        const USILInstruction *owner = &program->instructions[instruction];
        CHECK(owner->source_instruction_index < (uint32_t)fixture->decoded.semantic.instruction_count);
        const DXBCInstruction *raw = &fixture->decoded.semantic.instructions[owner->source_instruction_index];
        const int input_slot = instruction ? 2 : 1, matrix_slot = instruction ? 1 : 2;
        CHECK(raw->has_raw_instruction_index && raw->raw_instruction_index == owner->source_instruction_index &&
            raw->opcode == (instruction ? 50u : 56u) && raw->operand_count == (instruction ? 4 : 3) &&
            owner->operands[matrix_slot].type == OPERAND_TYPE_CONSTANT_BUFFER &&
            owner->operands[matrix_slot].rel_offset0 == components[instruction] &&
            owner->operands[input_slot].type == OPERAND_TYPE_INPUT && owner->operands[input_slot].register_index == 0 &&
            usil_operand_destination_lane_mask(&owner->operands[0]) == 15 &&
            owner->operands[0].type == (instruction == 3 ? OPERAND_TYPE_OUTPUT : OPERAND_TYPE_TEMP));
        for (int lane = 0; lane < 4; ++lane) {
            CHECK(usil_operand_source_component(&owner->operands[input_slot], lane) == components[instruction]);
            if (instruction) CHECK(hlsl_operand_definition(&ctx, instruction, 3, lane) == instruction - 1);
            if (instruction < 3) CHECK(hlsl_definition_use_count(&ctx, instruction, lane) == 1);
        }
    }
    ASTExpr *root = plan.expression;
    CHECK(root && root->kind == AST_EXPR_CALL && !strcmp(root->u.call.name, "mul") && root->u.call.arg_count == 2 &&
        root->logical_origin.complete && root->logical_origin.scalar_type == AST_SCALAR_FLOAT32 &&
        root->logical_origin.components == 4 && root->logical_origin.destination_lanes == 15 &&
        root->logical_origin.logical_value_id == ((UINT64_C(1) << 62) | UINT64_C(3)) &&
        root->logical_origin.instruction_index == 3 &&
        root->logical_origin.source_instruction_index == program->instructions[3].source_instruction_index);
    const ASTExpr *matrix = root->u.call.args[0], *input = root->u.call.args[1];
    CHECK(matrix != input && matrix->kind == AST_EXPR_EMITTER_OPERAND && matrix->operand_provenance.complete &&
        matrix->operand_provenance.logical_value_id == (UINT64_C(1) << 32) &&
        matrix->operand_provenance.instruction_index == 1 && matrix->operand_provenance.operand_index == 1 &&
        matrix->operand_provenance.source_instruction_index == program->instructions[1].source_instruction_index &&
        matrix->operand_provenance.destination_lanes == 15 && input->kind == AST_EXPR_EMITTER_OPERAND &&
        input->operand_provenance.complete && input->operand_provenance.instruction_index == 0 &&
        input->operand_provenance.source_instruction_index == program->instructions[0].source_instruction_index &&
        input->operand_provenance.operand_index == 1 && input->operand_provenance.natural_components == 4 &&
        input->operand_provenance.result_components == 4 && input->operand_provenance.destination_lanes == 15);
    for (int lane = 0; lane < 4; ++lane) CHECK(input->operand_provenance.selected_components[lane] == lane);
    *owned = root; plan.expression = NULL; hlsl_matrix_lift_plan_free(&plan);
    free_cbuffer_emission_layouts(&ctx); free(ctx.cb_reg_map); dispose(&ctx); sb_free(&scratch);
    return true;
}

/* In matrix-containing buffers the explicit shape keeps the retained variable
 * ordinal separate from the four occupied rows. Vector-only receipts keep
 * their original zero-shape contract. */
static unsigned packed_matrix_receipt_facts(unsigned placement, uint8_t shell_authority,
    uint8_t field_authority, HLSLSourceQualityFacts facts[4]) {
    const unsigned fields = placement ? 2u : 1u, count = fields + 2u;
    for (unsigned event = 0; event < count; ++event) {
        hlsl_source_quality_facts_init(&facts[event]);
        facts[event].known = true; facts[event].cbuffer_binding_register = 0;
        facts[event].cbuffer_declaration_authority = shell_authority;
        facts[event].cbuffer_declaration_kind = event == 0 ? HLSL_SOURCE_CBUFFER_BEGIN :
            event == count - 1u ? HLSL_SOURCE_CBUFFER_END : HLSL_SOURCE_CBUFFER_FIELD;
        facts[event].cbuffer_byte_size = placement ? 80u : 64u;
        if (event && event < count - 1u) {
            const unsigned field = event - 1u;
            const bool matrix = field == (placement == 1 ? 1u : 0u);
            facts[event].cbuffer_declaration_authority = field_authority;
            facts[event].cbuffer_field_index = field;
            facts[event].cbuffer_byte_offset = matrix ? placement == 1 ? 16u : 0u : placement == 1 ? 0u : 64u;
            facts[event].cbuffer_byte_size = matrix ? 64u : 16u;
            facts[event].cbuffer_field_rows = matrix ? 4 : 1;
            facts[event].cbuffer_field_columns = 4;
            facts[event].cbuffer_field_is_matrix = matrix;
        }
    }
    return count;
}

static bool packed_output_matrix_quality(const HLSLSourceQualityResult *quality) {
    CHECK(packed_output_clean(quality) && quality->counts.cbuffer_declarations == 1 &&
        quality->counts.cbuffer_fields == 1 && !quality->has_first_issue);
    return true;
}

static bool check_packed_output_matrix_cbuffer(void) {
    PackedOutputFixture fixture; CHECK(packed_output_fixture_layout(&fixture, 0, false, false, 0, 2));
    PackedOutputCBufferMetadata metadata; packed_output_cbuffer_metadata(&metadata);
    for (unsigned authority = 0; authority < 2; ++authority) {
        memset(metadata.field_names[authority], 0, sizeof(metadata.field_names[authority]));
        memcpy(metadata.field_names[authority], "ObjectTransform", sizeof("ObjectTransform"));
        metadata.fields[authority] = (SerializedVariable){metadata.field_names[authority], {0, 4, 4, 1, 0, 0}};
        metadata.buffers[authority].size = 64; metadata.parameters[authority].is_binary = true;
    }
    fixture.parameters = &metadata.parameters[0];
    ASTExpr *matrix = NULL, *roots[2] = {0}; char names[2][96];
    CHECK(packed_output_owned_matrix(&fixture, &matrix) && packed_output_owned_roots(&fixture, roots, names));
    StringBuilder held, retained; sb_init(&held); sb_init(&retained); ast_format_expr(matrix, &held);
    CHECK(sb_ok(&held) && strstr(held.buf, "mul((ObjectTransform), (position))") && !strstr(held.buf, "get_cb"));
    const char prefix[] = "// caller prefix\n";
    StringBuilder baseline, changed; sb_init(&baseline); sb_init(&changed);
    HLSLExpressionSourceMap map, changed_map; HLSLSourceQualityResult quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    HLSLSourceQualityFacts current_facts[4], common_facts[4];
    const unsigned events = packed_matrix_receipt_facts(0, 1, 1, current_facts);
    CHECK(packed_matrix_receipt_facts(0, 2, 2, common_facts) == events);
    sb_append(&baseline, prefix); PackedOutputObservations observer = {
        .expected_cbuffer_facts = current_facts, .expected_cbuffer_fact_count = events};
    const bool emitted = packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic);
    if (!emitted) fprintf(stderr, "Packed matrix status=%s phase=%s reason=%s instruction=%d\n",
        hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase),
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    if (emitted && (quality.classification != HLSL_SOURCE_QUALITY_CLEAN || quality.reasons)) fprintf(stderr,
        "Packed matrix quality=%s reasons=%u units=%zu incomplete=%zu residual=%zu unknown=%zu first_kind=%d first_owner=%d first_raw=%u first_artifacts=%u first_reasons=%u bytes=%zu\n",
        hlsl_source_quality_class_name(quality.classification), quality.reasons, quality.counts.inspected_units,
        quality.counts.incomplete_units, quality.counts.residual_total, quality.counts.unknown_provenance,
        quality.has_first_issue ? (int)quality.first_issue.kind : -1,
        quality.has_first_issue ? quality.first_issue.facts.instruction_index : -1,
        quality.has_first_issue ? quality.first_issue.facts.source_instruction_index : UINT32_MAX,
        quality.has_first_issue ? quality.first_issue.facts.artifacts : 0,
        quality.has_first_issue ? quality.first_issue.reasons : 0, baseline.len);
    CHECK(emitted && packed_output_matrix_quality(&quality) && !observer.wrong_owner && observer.writers_seen == UINT8_C(48) &&
        observer.cbuffer_events == events && observer.cbuffer_authority == 1 &&
        strstr(baseline.buf, "cbuffer PackedInputs : register(b0)") && strstr(baseline.buf, "float4x4 ObjectTransform") &&
        strstr(baseline.buf, held.buf) && map.complete && map.count == (size_t)fixture.decoded.program.instruction_count &&
        hlsl_expression_source_map_matches(&map, &fixture.decoded.program, baseline.buf));
    for (int instruction = 0; instruction < 4; ++instruction) CHECK(map.origins[instruction].instruction_index == instruction &&
        map.origins[instruction].source_instruction_index == fixture.decoded.program.instructions[instruction].source_instruction_index &&
        map.origins[instruction].destination_lanes == 15 && map.origins[instruction].source_begin >= sizeof(prefix) - 1u &&
        map.origins[instruction].source_end <= baseline.len && map.origins[instruction].source_begin < map.origins[instruction].source_end);
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(packed_output_emit(&fixture, &changed, outputs & 1u ? &changed_map : NULL,
            outputs & 2u ? &changed_quality : NULL, NULL, &diagnostic) &&
            changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&map, &changed_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &changed_quality));
    }
    /* Matrix read authority can come from common metadata; its independent
     * model and callback lease cannot substitute an incomplete current field. */
    fixture.parameters = NULL; fixture.common_parameters = &metadata.parameters[1];
    sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
    PackedOutputObservations common = {.expected_cbuffer_facts = common_facts, .expected_cbuffer_fact_count = events};
    CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &common, &diagnostic) &&
        packed_output_matrix_quality(&changed_quality) && !common.wrong_owner && common.cbuffer_events == events &&
        changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) && natural_if_maps_equal(&map, &changed_map));
    const HLSLSourceQualityResult common_quality = changed_quality;
    for (unsigned authority = 0; authority < 2; ++authority) for (unsigned point = 0; point < 2; ++point) {
        fixture.parameters = authority ? NULL : &metadata.parameters[0];
        fixture.common_parameters = authority ? &metadata.parameters[1] : NULL;
        const size_t at = point ? (authority ? common.observations : observer.observations) : 1;
        PackedOutputObservations drift = {.mutation = 10, .mutate_at = at, .mutable_field_name = metadata.field_names[authority]};
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(!packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic) && drift.mutated &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && !changed_map.complete && !changed_map.count &&
            changed.len == sizeof(prefix) - 1u && changed.buf && !strcmp(changed.buf, prefix));
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
            packed_output_matrix_quality(&changed_quality) && strstr(changed.buf, "_AdjustedScale") &&
            !strstr(changed.buf, "ObjectTransform") &&
            hlsl_expression_source_map_matches(&changed_map, &fixture.decoded.program, changed.buf));
        memset(metadata.field_names[authority], 0, sizeof(metadata.field_names[authority]));
        memcpy(metadata.field_names[authority], "ObjectTransform", sizeof("ObjectTransform"));
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
            changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) && natural_if_maps_equal(&map, &changed_map) &&
            hlsl_source_quality_results_equal(authority ? &common_quality : &quality, &changed_quality));
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        fixture.parameters = failure ? &metadata.parameters[0] : NULL;
        fixture.common_parameters = failure == 2 || failure == 3 ? &metadata.parameters[1] : NULL;
        if (failure == 1 || failure == 3) metadata.fields[0].layout[1] = 3;
        if (failure == 2) metadata.buffers[1].size = 80;
        sb_free(&changed); sb_init(&changed);
        CHECK(!packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
            diagnostic.status != HLSL_EMIT_STATUS_OK && !changed_map.complete && !changed_map.count &&
            changed_quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        metadata.fields[0].layout[1] = 4; metadata.buffers[1].size = 64;
    }
    fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = NULL;
    sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
    CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, NULL, &diagnostic) &&
        changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) && natural_if_maps_equal(&map, &changed_map) &&
        hlsl_source_quality_results_equal(&quality, &changed_quality));
    natural_if_fixture_dispose(&fixture.decoded); memset(&metadata, 0, sizeof(metadata));
    ast_format_expr(matrix, &retained);
    CHECK(sb_ok(&retained) && retained.len == held.len && !memcmp(retained.buf, held.buf, held.len));
    ast_free_expr(matrix); ast_free_expr(roots[0]); ast_free_expr(roots[1]);
    sb_free(&held); sb_free(&retained); sb_free(&baseline); sb_free(&changed);
    return true;
}

typedef struct {
    char field_names[2][3][48];
    SerializedVariable fields[2][3];
    SerializedConstantBuffer buffers[2];
    SerializedResourceParam bindings[2];
    SerializedProgramParameters parameters[2];
} PackedMatrixReceiptMetadata;

static bool packed_matrix_receipt_metadata(PackedMatrixReceiptMetadata *metadata, unsigned placement) {
    memset(metadata, 0, sizeof(*metadata));
    for (unsigned authority = 0; authority < 2; ++authority) {
        const unsigned fields = placement ? 2u : 1u;
        for (unsigned field = 0; field < fields; ++field) {
            const bool matrix = field == (placement == 1 ? 1u : 0u);
            const uint32_t offset = matrix ? placement == 1 ? 16u : 0u : placement == 1 ? 0u : 64u;
            memcpy(metadata->field_names[authority][field], matrix ? "_Transform" : "_UvOffset",
                matrix ? sizeof("_Transform") : sizeof("_UvOffset"));
            metadata->fields[authority][field].name = metadata->field_names[authority][field];
            uint32_t *words = metadata->fields[authority][field].layout;
            if (!authority) { words[1] = matrix ? 4 : 1; words[2] = 4; words[3] = matrix; words[5] = offset; }
            else { words[0] = offset; words[3] = 4; words[4] = matrix; }
            DecodedVariableLayout decoded;
            metadata->parameters[authority].is_binary = !authority;
            CHECK(parameter_layout_decode(&metadata->parameters[authority], &metadata->fields[authority][field], &decoded) &&
                !decoded.scalar_type && !decoded.array_size && decoded.is_matrix == matrix &&
                decoded.rows == (matrix ? 4u : 1u) && decoded.columns == 4 && decoded.byte_offset == offset &&
                parameter_layout_byte_size(&decoded) == (matrix ? 64u : 16u));
        }
        metadata->buffers[authority] = (SerializedConstantBuffer){.name = "PackedInputs",
            .size = placement ? 80u : 64u, .role = SERIALIZED_CBUFFER_NAMED,
            .variables = metadata->fields[authority], .var_count = (int)fields};
        metadata->bindings[authority] = (SerializedResourceParam){.name = "PackedInputs",
            .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER};
        metadata->parameters[authority].constant_buffers = &metadata->buffers[authority];
        metadata->parameters[authority].cb_count = 1;
        metadata->parameters[authority].resources = &metadata->bindings[authority];
        metadata->parameters[authority].res_count = 1;
    }
    return true;
}

static bool packed_matrix_receipt_fixture(PackedOutputFixture *fixture, unsigned placement) {
    const PackedMatrixReceiptConfig config = {.placement = placement};
    CHECK(packed_output_fixture_layout_position_config(fixture, 1, false, false, 0, 2, 0, NULL, &config));
    const USILProgram *program = &fixture->decoded.program;
    CHECK(program->instruction_count == 7 && program->temp_count == 1 &&
        program->instructions[0].source_instruction_index == 9 && fixture->xy_writer == 4 && fixture->zw_writer == 5);
    for (unsigned owner = 0; owner < 4; ++owner) {
        const unsigned slot = owner ? 1u : 2u;
        const unsigned component[4] = {1, 0, 2, 3};
        const DXBCOperand *source = &program->instructions[owner].operands[slot];
        CHECK(source->type == OPERAND_TYPE_CONSTANT_BUFFER && source->register_index == 0 &&
            source->rel_offset0 == (int)(component[owner] + (placement == 1 ? 1u : 0u)) &&
            source->index_values[1] == (uint32_t)source->rel_offset0 &&
            program->instructions[owner].source_instruction_index == 9u + owner &&
            usil_operand_destination_lane_mask(&program->instructions[owner].operands[0]) == 15);
    }
    if (placement) CHECK(program->instructions[4].operands[2].type == OPERAND_TYPE_CONSTANT_BUFFER &&
        program->instructions[4].operands[2].rel_offset0 == (placement == 1 ? 0 : 4));
    return true;
}

static bool check_packed_matrix_receipt_positive(unsigned placement, unsigned authority) {
    PackedOutputFixture fixture; CHECK(packed_matrix_receipt_fixture(&fixture, placement));
    PackedMatrixReceiptMetadata metadata; CHECK(packed_matrix_receipt_metadata(&metadata, placement));
    fixture.parameters = authority == 1 ? NULL : &metadata.parameters[0];
    fixture.common_parameters = authority ? &metadata.parameters[1] : NULL;
    if (authority == 3) {
        metadata.parameters[0].cb_count = 0;
        metadata.parameters[1].res_count = 0; /* Current binding and common complete declaration. */
    }
    const uint8_t expected_authority = authority == 1 || authority == 3 ? 2 : 1;
    HLSLSourceQualityFacts facts[4];
    const unsigned event_count = packed_matrix_receipt_facts(placement, expected_authority, expected_authority, facts);
    PackedOutputObservations observer = {.expected_cbuffer_facts = facts, .expected_cbuffer_fact_count = event_count};
    const char prefix[] = "// held prefix\n";
    StringBuilder baseline, repeated, held, retained; sb_init(&baseline); sb_init(&repeated); sb_init(&held); sb_init(&retained);
    HLSLExpressionSourceMap map, repeated_map; HLSLSourceQualityResult quality, repeated_quality; HLSLEmitDiagnostic diagnostic;
    sb_append(&baseline, prefix);
    const bool emitted = packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic);
    if (!emitted || quality.classification != HLSL_SOURCE_QUALITY_CLEAN) fprintf(stderr,
        "Named matrix placement=%u authority=%u emitted=%d status=%s phase=%s reason=%s quality=%s reasons=%u\n",
        placement, authority, emitted, hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase),
        hlsl_emit_reason_name(diagnostic.reason), hlsl_source_quality_class_name(quality.classification), quality.reasons);
    CHECK(emitted && packed_output_clean(&quality) && !observer.wrong_owner && observer.cbuffer_events == event_count &&
        quality.counts.cbuffer_declarations == 1 && quality.counts.cbuffer_fields == (placement ? 2u : 1u) &&
        map.complete && map.count == 7 && hlsl_expression_source_map_matches(&map, &fixture.decoded.program, baseline.buf) &&
        strstr(baseline.buf, placement == 1 ? "float4x4 _Transform : packoffset(c1)" : "float4x4 _Transform : packoffset(c0)") &&
        (!placement || strstr(baseline.buf, placement == 1 ? "float4 _UvOffset : packoffset(c0)" : "float4 _UvOffset : packoffset(c4)")));
    for (unsigned owner = 0; owner < 7; ++owner) CHECK(map.origins[owner].instruction_index == (int)owner &&
        map.origins[owner].source_instruction_index == 9u + owner && map.origins[owner].source_begin >= sizeof(prefix) - 1u &&
        map.origins[owner].source_end <= baseline.len && map.origins[owner].source_begin < map.origins[owner].source_end);
    /* The matrix declaration covers 64 bytes once; the retained expression
     * still owns the actual four MUL/MAD instructions and independent input. */
    HLSLEmitterContext ctx; StringBuilder scratch; HLSLMatrixLiftPlan plan;
    CHECK(packed_output_prepare_context(&fixture, &ctx, &scratch, &diagnostic) &&
        hlsl_matrix_lift_prepare(&ctx, 0, &plan) && plan.claimed_instruction_count == 4 &&
        plan.start_instruction == 0 && plan.end_instruction == 3 && plan.instruction_owners.words[0] == UINT64_C(15));
    CHECK(!hlsl_source_quality_named_cbuffer_supported(&ctx, 0, NULL)); /* No ready public-emission guard in this private setup. */
    ASTExpr *root = plan.expression; plan.expression = NULL;
    CHECK(root && root->kind == AST_EXPR_CALL && root->u.call.arg_count == 2 && !strcmp(root->u.call.name, "mul") &&
        root->logical_origin.complete && root->logical_origin.components == 4 && root->logical_origin.destination_lanes == 15 &&
        root->logical_origin.instruction_index == 3 && root->logical_origin.source_instruction_index == 12 &&
        root->u.call.args[0] != root->u.call.args[1] && root->u.call.args[0]->operand_provenance.complete &&
        root->u.call.args[0]->operand_provenance.logical_value_id == ((UINT64_C(1) << 32) | (placement == 1 ? 16u : 0u)));
    ast_format_expr(root, &held); CHECK(sb_ok(&held) && strstr(baseline.buf, held.buf));
    hlsl_matrix_lift_plan_free(&plan); free_cbuffer_emission_layouts(&ctx); free(ctx.cb_reg_map); dispose(&ctx); sb_free(&scratch);
    const unsigned combinations = placement == 1 && !authority ? 4u : 1u;
    for (unsigned output = 0; output < combinations; ++output) {
        sb_free(&repeated); sb_init(&repeated); sb_append(&repeated, prefix);
        const unsigned requested = combinations == 1 ? 3u : output;
        CHECK(packed_output_emit(&fixture, &repeated, requested & 1u ? &repeated_map : NULL,
            requested & 2u ? &repeated_quality : NULL, NULL, &diagnostic) && repeated.len == baseline.len &&
            !memcmp(repeated.buf, baseline.buf, baseline.len));
        if (requested & 1u) CHECK(natural_if_maps_equal(&map, &repeated_map));
        if (requested & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &repeated_quality));
    }
    if (placement == 1 && !authority) {
        const size_t points[] = {1, observer.observations};
        for (unsigned point = 0; point < 2; ++point) {
            PackedOutputObservations veto = {.reject_at = points[point]};
            sb_free(&repeated); sb_init(&repeated); sb_append(&repeated, prefix);
            CHECK(!packed_output_emit(&fixture, &repeated, &repeated_map, &repeated_quality, &veto, &diagnostic) &&
                repeated_quality.classification == HLSL_SOURCE_QUALITY_FAILED && !repeated_map.complete && !repeated_map.count &&
                repeated.len == sizeof(prefix) - 1u && !strcmp(repeated.buf, prefix));
        }
    }
    natural_if_fixture_dispose(&fixture.decoded); memset(&metadata, 0, sizeof(metadata));
    ast_format_expr(root, &retained); CHECK(sb_ok(&retained) && retained.len == held.len && !memcmp(retained.buf, held.buf, held.len));
    ast_free_expr(root); sb_free(&held); sb_free(&retained); sb_free(&baseline); sb_free(&repeated);
    return true;
}

static bool check_packed_matrix_receipt_boundaries(void) {
    PackedOutputFixture fixture; CHECK(packed_matrix_receipt_fixture(&fixture, 2));
    PackedMatrixReceiptMetadata metadata; CHECK(packed_matrix_receipt_metadata(&metadata, 2));
    fixture.parameters = &metadata.parameters[0];
    const PackedMatrixReceiptMetadata held = metadata;
    const USILConstantBuffer held_buffer = fixture.decoded.program.cbuffers[0];
    StringBuilder source; sb_init(&source);
    HLSLExpressionSourceMap map; HLSLSourceQualityResult quality; HLSLEmitDiagnostic diagnostic;
    for (unsigned mutation = 0; mutation < 16; ++mutation) {
        metadata = held; fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = NULL;
        fixture.decoded.program.cbuffers[0] = held_buffer;
        switch (mutation) {
        case 0: fixture.parameters = NULL; break;
        case 1: metadata.fields[0][0].layout[1] = 3; break; /* Actual matrix shape. */
        case 2: metadata.fields[0][0].layout[4] = 2; break; /* Matrix array. */
        case 3: metadata.fields[0][0].layout[0] = 1; break; /* Integer matrix. */
        case 4: metadata.fields[0][0].layout[5] = 16; break; /* Overlap with the consumed tail. */
        case 5: metadata.buffers[0].size = 64; break; /* Tail absent from authoritative shell. */
        case 6: metadata.buffers[0].has_is_partial = metadata.buffers[0].is_partial = true; break;
        case 7: metadata.buffers[0].role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS; break;
        case 8: metadata.buffers[0].var_count = 1; break; /* Missing consumed tail metadata. */
        case 9:
            fixture.common_parameters = &metadata.parameters[1]; metadata.fields[1][0].layout[0] = 16; break;
        case 10:
            metadata.fields[0][2] = metadata.fields[0][1]; metadata.buffers[0].var_count = 3; break; /* Duplicate exact member. */
        case 11:
            metadata.fields[0][1].name = metadata.fields[0][0].name; break; /* Matrix/vector identifier ambiguity. */
        case 12: metadata.bindings[0].bind_index = 1; break;
        case 13:
            fixture.decoded.program.cbuffers[0].size = 6; metadata.buffers[0].size = 96; break; /* Anonymous end row. */
        case 14:
            fixture.decoded.program.cbuffers[0].size = 6; metadata.buffers[0].size = 96;
            memcpy(metadata.field_names[0][2], "_Unused", sizeof("_Unused"));
            metadata.fields[0][2] = (SerializedVariable){metadata.field_names[0][2], {0, 1, 4, 0, 0, 80}};
            metadata.buffers[0].var_count = 3; break; /* Serialized field filtered from emitted layout. */
        case 15:
            fixture.common_parameters = &metadata.parameters[1]; metadata.buffers[1].size = 96; break;
        }
        sb_free(&source); sb_init(&source);
        PackedOutputObservations observer = {0};
        const bool emitted = packed_output_emit(&fixture, &source, &map, &quality, &observer, &diagnostic);
        if (emitted && quality.classification == HLSL_SOURCE_QUALITY_CLEAN) fprintf(stderr,
            "Unexpected complete matrix declaration mutation=%u events=%zu\n", mutation, observer.cbuffer_events);
        CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN && !quality.counts.cbuffer_declarations &&
            !quality.counts.cbuffer_fields && !observer.cbuffer_events);
        if (!emitted) CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK && !map.complete && !map.count);
        else CHECK(quality.counts.incomplete_units &&
            (quality.reasons & HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE));
    }
    metadata = held; fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = NULL;
    fixture.decoded.program.cbuffers[0] = held_buffer; sb_free(&source); sb_init(&source);
    CHECK(packed_output_emit(&fixture, &source, &map, &quality, NULL, &diagnostic) && packed_output_clean(&quality));
    sb_free(&source); natural_if_fixture_dispose(&fixture.decoded); return true;
}

static bool check_packed_matrix_receipt_callbacks(void) {
    PackedOutputFixture fixture; CHECK(packed_matrix_receipt_fixture(&fixture, 1));
    PackedMatrixReceiptMetadata metadata; CHECK(packed_matrix_receipt_metadata(&metadata, 1));
    fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = &metadata.parameters[1];
    const PackedMatrixReceiptMetadata held_metadata = metadata;
    USILInstruction held_instructions[7]; memcpy(held_instructions, fixture.decoded.program.instructions, sizeof(held_instructions));
    const USILConstantBuffer held_buffer = fixture.decoded.program.cbuffers[0];
    const char prefix[] = "// matrix caller prefix\n";
    char entry_name[32] = "entryName"; const HLSLEmitNames names = {.entry_point = entry_name};
    StringBuilder baseline, changed; sb_init(&baseline); sb_init(&changed); sb_append(&baseline, prefix);
    HLSLExpressionSourceMap map, changed_map; HLSLSourceQualityResult quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    PackedOutputObservations observer = {.names = &names};
    /* Match the new shape elsewhere; here the observer must not apply the
     * legacy ordinal==row check to a matrix buffer. */
    HLSLSourceQualityFacts expected[4];
    observer.expected_cbuffer_fact_count = packed_matrix_receipt_facts(1, 1, 1, expected);
    observer.expected_cbuffer_facts = expected;
    CHECK(packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic) &&
        packed_output_clean(&quality) && !observer.wrong_owner && observer.first_config && observer.first_header && observer.last_header);
    for (unsigned attack = 0; attack < 12; ++attack) {
        PackedOutputObservations drift = {.names = &names, .mutation = 10,
            .mutate_at = attack & 1u ? observer.observations : 1u,
            .mutable_field_name = metadata.field_names[attack >= 2 && attack < 4 ? 1 : 0][1]};
        SerializedVariable replacement[3]; memcpy(replacement, metadata.fields[0], sizeof(replacement));
        bool fresh_admitted = attack < 7;
        if (attack == 4) { drift.mutation = 15; drift.mutable_parameters = &metadata.parameters[0]; }
        if (attack == 5) {
            drift.mutation = 16; drift.mutable_parameters = &metadata.parameters[0]; drift.replacement_fields = replacement;
        }
        if (attack == 6) { drift.mutation = 2; drift.writer = fixture.zw_writer; }
        if (attack == 7) { drift.mutation = 6; drift.mutate_at = observer.first_header; fresh_admitted = false; }
        if (attack == 8) { drift.mutation = 5; drift.mutate_at = observer.last_header; fresh_admitted = false; }
        if (attack == 9) { drift.mutation = 7; drift.mutate_at = observer.observations; fresh_admitted = false; }
        if (attack == 10) { drift.mutation = 4; drift.source_offset = 0; drift.mutate_at = observer.first_config; fresh_admitted = false; }
        if (attack == 11) { drift.mutation = 4; drift.source_offset = map.origins[0].source_begin; fresh_admitted = false; }
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        const bool emitted = packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic);
        /* The borrowed replacement stays valid through the call, and is
         * detached before any assertion/decoder cleanup path. */
        if (attack == 5) metadata.buffers[0].variables = metadata.fields[0];
        CHECK(!emitted && drift.mutated && changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
            !changed_map.complete && !changed_map.count && changed.len == sizeof(prefix) - 1u &&
            changed.buf && !strcmp(changed.buf, prefix));
        if (fresh_admitted) {
            /* Common-only name drift conflicts with selected read authority;
             * its legal fresh model uses only the changed common model. */
            if (attack == 2 || attack == 3) fixture.parameters = NULL;
            if (attack < 2) fixture.common_parameters = NULL;
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            PackedOutputObservations fresh = {.names = &names};
            if (attack == 5) metadata.buffers[0].variables = replacement;
            const bool fresh_emitted = packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic);
            if (attack == 5) metadata.buffers[0].variables = metadata.fields[0];
            CHECK(fresh_emitted &&
                packed_output_clean(&changed_quality) && hlsl_expression_source_map_matches(&changed_map, &fixture.decoded.program, changed.buf));
            if (attack < 4 || attack == 6) CHECK(changed.len != baseline.len || memcmp(changed.buf, baseline.buf, baseline.len));
            if (attack == 4 || attack == 5) CHECK(changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len));
        }
        metadata = held_metadata; fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = &metadata.parameters[1];
        fixture.decoded.program.cbuffers[0] = held_buffer;
        memcpy(fixture.decoded.program.instructions, held_instructions, sizeof(held_instructions));
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        PackedOutputObservations restored = {.names = &names};
        CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &restored, &diagnostic) &&
            changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) &&
            natural_if_maps_equal(&map, &changed_map) && hlsl_source_quality_results_equal(&quality, &changed_quality));
    }
    sb_free(&baseline); sb_free(&changed); natural_if_fixture_dispose(&fixture.decoded); return true;
}

/* This separate mode reproduces the actual consecutive POSITION.XY and
 * POSITION.ZW MOV grammar. The older XYZ/W fixture remains an excluded route. */
static bool packed_position_fixture(PackedOutputFixture *fixture, unsigned input_width,
    unsigned arithmetic, bool reversed, bool union_declaration) {
    return packed_output_fixture_layout_position(fixture, arithmetic, reversed, union_declaration, 0, 0, input_width);
}

static bool packed_position_owned_root(PackedOutputFixture *fixture, ASTExpr **owned, char name[96]) {
    HLSLEmitterContext ctx; StringBuilder scratch; HLSLEmitDiagnostic diagnostic;
    CHECK(packed_output_prepare_context(fixture, &ctx, &scratch, &diagnostic));
    HLSLPositionOutputPlan plan;
    CHECK(hlsl_position_output_plan_prepare(ctx.program, &plan) && plan.assembly.present &&
        plan.assembly.output_signature_index == 0 && plan.assembly.output.system_value == 1 &&
        plan.assembly.output.mask == 15 && !plan.assembly.output.rw_mask &&
        !plan.assembly.output.semantic_name_extended && plan.assembly.return_instruction_index == 4 &&
        plan.assembly.return_source_instruction_index == ctx.program->instructions[4].source_instruction_index &&
        plan.immediate_raw_token == UINT32_C(0x4002) && !plan.immediate_words[0] && !plan.immediate_words[1] &&
        !plan.immediate_words[2] && plan.immediate_words[3] == UINT32_C(0x3f800000));
    HLSLPositionOutputPlan independent;
    CHECK(hlsl_position_output_plan_prepare(ctx.program, &independent) && hlsl_position_output_plans_equal(&plan, &independent));
    ctx.position_output_plan = plan;
    for (unsigned piece = 0; piece < 2; ++piece) {
        const USILInstruction *instruction = &ctx.program->instructions[piece];
        const HLSLDomainOutputPiece *owner = &plan.assembly.pieces[piece];
        const DXBCInstruction *raw = NULL;
        for (int token = 0; token < fixture->decoded.semantic.instruction_count; ++token) {
            const DXBCInstruction *candidate = &fixture->decoded.semantic.instructions[token];
            if (candidate->has_raw_instruction_index && candidate->raw_instruction_index == instruction->source_instruction_index) {
                CHECK(!raw); raw = candidate;
            }
        }
        CHECK(raw && raw->opcode == 54 && raw->operand_count == 2 && instruction->opcode == USIL_OP_MOV &&
            owner->instruction_index == (int)piece && owner->source_instruction_index == instruction->source_instruction_index &&
            owner->opcode == USIL_OP_MOV && owner->destination_register == 0 && owner->width == 2 &&
            owner->mask == (piece ? 12 : 3) && owner->destination_raw_token == instruction->operands[0].raw_token &&
            usil_operand_destination_lane_mask(&instruction->operands[0]) == owner->mask);
        USILOperandUseInfo use;
        CHECK(usil_instruction_operand_use(ctx.program, instruction, 1, &use) && use.use == USIL_OPERAND_USE_SOURCE &&
            use.source_lane_mask == owner->mask);
        /* Partial pieces cannot masquerade as one complete signature field. */
        HLSLNaturalOutputProjection projection;
        CHECK(!hlsl_high_level_output_projection(ctx.program, &instruction->operands[0], &projection));
    }
    const char *field = hlsl_high_level_output_field_name(&ctx, 0);
    CHECK(field && strlen(field) < 96); strcpy(name, field);
    unsigned input_width = 0;
    for (unsigned lane = 0; lane < 4; ++lane) if (ctx.program->inputs[0].mask & (1u << lane)) ++input_width;
    ASTExpr *first = hlsl_natural_source_atom(&ctx, 0, 1, 3);
    ASTExpr *second = hlsl_natural_source_atom(&ctx, 1, 1, 12);
    CHECK(first && first->kind == AST_EXPR_EMITTER_OPERAND && !first->logical_origin.complete &&
        first->operand_provenance.complete && first->operand_provenance.value_role == AST_OPERAND_VALUE_LOGICAL &&
        first->operand_provenance.instruction_index == 0 && first->operand_provenance.source_instruction_index ==
        ctx.program->instructions[0].source_instruction_index && first->operand_provenance.operand_index == 1 &&
        first->operand_provenance.destination_lanes == 3 && first->operand_provenance.natural_components == input_width &&
        first->operand_provenance.result_components == 2 && first->operand_provenance.selected_components[0] == 0 &&
        first->operand_provenance.selected_components[1] == 1 && !first->operand_provenance.synthetic_interface);
    CHECK(second && second->kind == AST_EXPR_LITERAL && second->u.literal.scalar_type == AST_SCALAR_FLOAT32 &&
        second->u.literal.components == 2 && !second->u.literal.val[0] && second->u.literal.val[1] == UINT32_C(0x3f800000));
    second = hlsl_instruction_logical_expression(&ctx, second, 1, 12, 2);
    CHECK(second && second->logical_origin.complete && second->logical_origin.scalar_type == AST_SCALAR_FLOAT32 &&
        second->logical_origin.components == 2 && second->logical_origin.logical_value_id == 1 &&
        second->logical_origin.instruction_index == 1 && second->logical_origin.source_instruction_index ==
        ctx.program->instructions[1].source_instruction_index && second->logical_origin.destination_lanes == 12);
    ASTExpr *root = hlsl_output_assembly_expression(&plan.assembly, HLSL_POSITION_OUTPUT_LOGICAL_ID, first, second);
    CHECK(root && root->kind == AST_EXPR_CALL && !strcmp(root->u.call.name, "float4") && root->u.call.arg_count == 2 &&
        root->u.call.args[0] == first && root->u.call.args[1] == second && root->logical_origin.complete &&
        root->logical_origin.scalar_type == AST_SCALAR_FLOAT32 && root->logical_origin.components == 4 &&
        root->logical_origin.logical_value_id == HLSL_POSITION_OUTPUT_LOGICAL_ID &&
        root->logical_origin.instruction_index == -1 && root->logical_origin.source_instruction_index == UINT32_MAX &&
        !root->logical_origin.destination_lanes && !root->logical_origin.program_bitcast);
    *owned = root; dispose(&ctx); sb_free(&scratch); return true;
}

static bool check_packed_position_positive(unsigned input_width, unsigned arithmetic, bool reversed, bool union_declaration) {
    PackedOutputFixture fixture; CHECK(packed_position_fixture(&fixture, input_width, arithmetic, reversed, union_declaration));
    CHECK(fixture.decoded.program.inputs[0].mask == (uint8_t)((1u << input_width) - 1u) &&
        fixture.decoded.program.inputs[0].rw_mask == 3 && fixture.decoded.program.signature_declarations[0].mask == 3);
    ASTExpr *root = NULL, *uv[2] = {0}; char position_name[96], uv_names[2][96];
    CHECK(packed_position_owned_root(&fixture, &root, position_name) && packed_output_owned_roots(&fixture, uv, uv_names));
    StringBuilder baseline, repeated, held, retained; sb_init(&baseline); sb_init(&repeated); sb_init(&held); sb_init(&retained);
    ast_format_expr(root, &held); CHECK(sb_ok(&held) && held.len);
    HLSLExpressionSourceMap map, repeated_map; HLSLSourceQualityResult quality, repeated_quality; HLSLEmitDiagnostic diagnostic;
    PackedOutputObservations observer = {.inspect_position = true};
    const bool emitted = packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic);
    if (!emitted) fprintf(stderr, "Packed POSITION width=%u arithmetic=%u reversed=%d union=%d status=%s phase=%d reason=%s instruction=%d\n",
        input_width, arithmetic, reversed, union_declaration, hlsl_emit_status_name(diagnostic.status), (int)diagnostic.phase,
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted && packed_output_clean(&quality) && !observer.wrong_owner && observer.position_assemblies == 1 &&
        observer.position_children == 3 && map.complete && map.count == 5 &&
        hlsl_expression_source_map_matches(&map, &fixture.decoded.program, baseline.buf));
    char assignment[128]; snprintf(assignment, sizeof(assignment), "output.%.95s = ", position_name);
    const char *statement = strstr(baseline.buf, assignment);
    CHECK(statement && !strstr(statement + strlen(assignment), assignment));
    const char *construction = statement + strlen(assignment);
    CHECK((size_t)(construction - baseline.buf) < baseline.len && held.len < baseline.len - (size_t)(construction - baseline.buf) &&
        !memcmp(construction, held.buf, held.len) && construction[held.len] == ';');
    char partial[128]; snprintf(partial, sizeof(partial), "output.%.95s.", position_name); CHECK(!strstr(baseline.buf, partial));
    for (unsigned piece = 0; piece < 2; ++piece) {
        const HLSLExpressionOrigin *origin = &map.origins[piece];
        StringBuilder child; sb_init(&child); ast_format_expr(root->u.call.args[piece], &child);
        CHECK(sb_ok(&child) && child.len && origin->kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION &&
            origin->instruction_index == (int)piece && origin->source_instruction_index ==
            fixture.decoded.program.instructions[piece].source_instruction_index && origin->destination_lanes == (piece ? 12 : 3) &&
            origin->source_begin >= (size_t)(construction - baseline.buf) && origin->source_end <= (size_t)(construction - baseline.buf) + held.len &&
            origin->source_end - origin->source_begin == child.len && !memcmp(baseline.buf + origin->source_begin, child.buf, child.len));
        sb_free(&child);
    }
    CHECK(map.origins[0].source_end < map.origins[1].source_begin);
    for (unsigned item = 0; item < 2; ++item) {
        const int writer = item ? fixture.zw_writer : fixture.xy_writer;
        StringBuilder expression; sb_init(&expression); ast_format_expr(uv[item], &expression);
        const HLSLExpressionOrigin *origin = &map.origins[writer];
        CHECK(origin->instruction_index == writer && origin->destination_lanes == (item ? 12 : 3) && sb_ok(&expression));
        const char *printed = strstr(baseline.buf + origin->source_begin, expression.buf);
        CHECK(printed && printed + expression.len <= baseline.buf + origin->source_end); sb_free(&expression);
    }
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        sb_free(&repeated); sb_init(&repeated);
        CHECK(packed_output_emit(&fixture, &repeated, outputs & 1u ? &repeated_map : NULL,
            outputs & 2u ? &repeated_quality : NULL, NULL, &diagnostic) && repeated.len == baseline.len &&
            !memcmp(repeated.buf, baseline.buf, baseline.len));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&map, &repeated_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &repeated_quality));
    }
    const size_t points[] = {1, observer.observations / 2u, observer.observations};
    for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
        PackedOutputObservations veto = {.reject_at = points[point]}; sb_free(&repeated); sb_init(&repeated);
        CHECK(!packed_output_emit(&fixture, &repeated, &repeated_map, &repeated_quality, &veto, &diagnostic) &&
            veto.observations == points[point] && !repeated_map.complete && !repeated_map.count &&
            repeated_quality.classification == HLSL_SOURCE_QUALITY_FAILED && !repeated.len &&
            (!repeated.buf || !repeated.buf[0]));
    }
    sb_free(&repeated); sb_init(&repeated);
    CHECK(packed_output_emit(&fixture, &repeated, &repeated_map, &repeated_quality, NULL, &diagnostic) &&
        repeated.len == baseline.len && !memcmp(repeated.buf, baseline.buf, baseline.len) &&
        natural_if_maps_equal(&map, &repeated_map) && hlsl_source_quality_results_equal(&quality, &repeated_quality));
    natural_if_fixture_dispose(&fixture.decoded); ast_format_expr(root, &retained);
    CHECK(sb_ok(&retained) && retained.len == held.len && !memcmp(retained.buf, held.buf, held.len));
    ast_free_expr(root); ast_free_expr(uv[0]); ast_free_expr(uv[1]);
    sb_free(&baseline); sb_free(&repeated); sb_free(&held); sb_free(&retained); return true;
}

static bool check_packed_position_rejections(void) {
    PackedOutputFixture fixture; CHECK(packed_position_fixture(&fixture, 3, 0, false, false));
    USILProgram *program = &fixture.decoded.program;
    USILInstruction instructions[5]; DXBCSignatureElement inputs[3], outputs[3]; USILSignatureDeclaration declarations[6];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(inputs, program->inputs, sizeof(inputs));
    memcpy(outputs, program->outputs, sizeof(outputs)); memcpy(declarations, program->signature_declarations, sizeof(declarations));
    for (unsigned mutation = 0; mutation < 19; ++mutation) {
        DXBCOperand relative = {0}; uint32_t extension = UINT32_C(0x41);
        DXBCOperand *literal = &program->instructions[1].operands[1];
        switch (mutation) {
        case 0: program->instructions[1].operands[0].destination_mask = 224; break; /* overlap X/Y suffix */
        case 1: program->instructions[1].operands[0].destination_mask = 128; break; /* missing Z */
        case 2: {
            program->instructions[0] = instructions[1]; program->instructions[1] = instructions[0];
            program->instructions[0].source_instruction_index = instructions[0].source_instruction_index;
            program->instructions[1].source_instruction_index = instructions[1].source_instruction_index; break;
        }
        case 3: program->instructions[1].opcode = USIL_OP_NOP; program->instructions[1].operand_count = 0; break;
        case 4: program->instructions[2].operands[0].register_index = 0; program->instructions[2].operands[0].index_values[0] = 0; break;
        case 5: program->outputs[0].component_type = 1; break;
        case 6: program->outputs[0].system_value = 0; break;
        case 7: program->outputs[0].rw_mask = 8; break;
        case 8: program->signature_declarations[3].mask = 7; break;
        case 9: /* Valid arithmetic arity, but POSITION assembly deliberately admits MOV only. */
            program->instructions[0].opcode = USIL_OP_ADD; program->instructions[0].operand_count = 3;
            program->instructions[0].operands[2] = instructions[2].operands[2]; break;
        case 10: {
            DXBCOperand *source = &program->instructions[0].operands[1]; source->type = OPERAND_TYPE_OUTPUT;
            source->raw_token = natural_if_source_token(OPERAND_TYPE_OUTPUT, source->swizzle); break;
        }
        case 11: {
            DXBCOperand *source = &program->instructions[0].operands[1];
            relative = *source; relative.swizzle_mode = 2; memset(relative.swizzle, 0, sizeof(relative.swizzle));
            relative.raw_token = UINT32_C(0x0010100a);
            source->index_representations[0] = 2; source->index_has_immediate[0] = false; source->rel_op0 = &relative;
            source->raw_token = (source->raw_token & ~(UINT32_C(7) << 22u)) | UINT32_C(2) << 22u; break;
        }
        case 12: {
            DXBCOperand *source = &program->instructions[0].operands[1]; source->has_neg = true;
            source->raw_token |= UINT32_C(0x80000000); source->extended_tokens = &extension; source->extended_token_count = 1; break;
        }
        case 13: literal->imm_values[0] = UINT32_C(0x80000000); literal->immediate_words[0] = UINT32_C(0x80000000); break;
        case 14: literal->imm_values[2] = UINT32_C(0x3f800000); literal->immediate_words[2] = UINT32_C(0x3f800000); break;
        case 15: literal->imm_values[3] = UINT32_C(0x40000000); literal->immediate_words[3] = UINT32_C(0x40000000); break;
        case 16: literal->immediate_words[3] = UINT32_C(0x7fc12345); break; /* independent payload drift */
        case 17: literal->raw_token = UINT32_C(0x4001); literal->imm_value_count = 1; literal->immediate_word_count = 1; break;
        case 18: program->instructions[0].operands[1].swizzle[1] = 0;
            program->instructions[0].operands[1].raw_token = natural_if_source_token(OPERAND_TYPE_INPUT,
                program->instructions[0].operands[1].swizzle); break;
        }
        HLSLEmitDiagnostic diagnostic; const bool rejected = natural_if_rejected(program, &diagnostic);
        /* Detach stack-borrowed nodes/words before CHECK and decoder disposal. */
        memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->inputs, inputs, sizeof(inputs));
        memcpy(program->outputs, outputs, sizeof(outputs)); memcpy(program->signature_declarations, declarations, sizeof(declarations));
        if (!rejected) fprintf(stderr, "Packed POSITION unexpectedly admitted mutation=%u\n", mutation);
        CHECK(rejected);
    }
    CHECK(usil_signature_authority_is_valid(program));
    StringBuilder restored; sb_init(&restored); HLSLExpressionSourceMap map; HLSLSourceQualityResult quality; HLSLEmitDiagnostic diagnostic;
    CHECK(packed_output_emit(&fixture, &restored, &map, &quality, NULL, &diagnostic) && packed_output_clean(&quality) &&
        hlsl_expression_source_map_matches(&map, program, restored.buf));
    sb_free(&restored); natural_if_fixture_dispose(&fixture.decoded); return true;
}

static bool check_packed_position_callbacks(void) {
    PackedOutputFixture fixture; CHECK(packed_position_fixture(&fixture, 3, 0, false, false));
    USILProgram *program = &fixture.decoded.program; const char prefix[] = "// caller prefix\n";
    char entry_name[] = "entryName"; const HLSLEmitNames names = {.entry_point = entry_name};
    StringBuilder baseline, changed; sb_init(&baseline); sb_init(&changed); sb_append(&baseline, prefix);
    HLSLExpressionSourceMap map, changed_map; HLSLSourceQualityResult quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    PackedOutputObservations observer = {.names = &names, .inspect_position = true};
    CHECK(packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic) && packed_output_clean(&quality) &&
        !observer.wrong_owner && observer.position_assemblies == 1 && observer.position_children == 3 &&
        observer.first_config && observer.first_header > observer.first_config && observer.last_header > observer.first_header);
    USILInstruction instructions[5]; DXBCSignatureElement inputs[3], outputs[3];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(inputs, program->inputs, sizeof(inputs));
    memcpy(outputs, program->outputs, sizeof(outputs));
    const struct {unsigned mutation; size_t point, offset; bool fresh;} attacks[] = {
        {12, 1, 0, true}, {2, observer.observations / 2u, 0, true}, {14, observer.observations, 0, true},
        {1, observer.observations, 0, true}, {9, observer.observations, 0, true},
        {13, observer.observations, 0, false},
        {6, observer.first_header, 0, false}, {5, observer.last_header, 0, false},
        {5, observer.first_config, 0, false}, {4, observer.first_config, sizeof(prefix), false},
        {4, 1, 0, false}, {4, observer.observations, map.origins[0].source_begin, false}, {7, observer.observations, 0, false}};
    for (unsigned attack = 0; attack < sizeof(attacks) / sizeof(*attacks); ++attack) {
        PackedOutputObservations drift = {.mutation = attacks[attack].mutation, .mutate_at = attacks[attack].point,
            .source_offset = attacks[attack].offset, .field = fixture.zw_field, .writer = fixture.xy_writer,
            .names = &names, .mutable_entry_name = entry_name};
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        const bool rejected = !packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic);
        if (!rejected) fprintf(stderr, "Packed POSITION callback falsely accepted attack=%u action=%u point=%zu\n",
            attack, drift.mutation, drift.mutate_at);
        CHECK(rejected && drift.mutated && !changed_map.complete && !changed_map.count &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && changed.len == sizeof(prefix) - 1u &&
            changed.buf && !strcmp(changed.buf, prefix));
        if (attacks[attack].fresh) {
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            PackedOutputObservations fresh = {.names = &names, .inspect_position = true};
            CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                packed_output_clean(&changed_quality) && hlsl_expression_source_map_matches(&changed_map, program, changed.buf) &&
                (changed.len != baseline.len || memcmp(changed.buf, baseline.buf, baseline.len)));
        } else if (drift.mutation != 13) CHECK(!memcmp(program->instructions, instructions, sizeof(instructions)) &&
            !memcmp(program->inputs, inputs, sizeof(inputs)) && !memcmp(program->outputs, outputs, sizeof(outputs)));
        else {
            HLSLPositionOutputPlan unsupported;
            CHECK(!hlsl_position_output_plan_prepare(program, &unsupported) && !unsupported.assembly.present);
        }
        memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->inputs, inputs, sizeof(inputs));
        memcpy(program->outputs, outputs, sizeof(outputs)); memcpy(entry_name, "entryName", sizeof(entry_name));
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        PackedOutputObservations restored = {.names = &names, .inspect_position = true};
        const bool restored_emitted = packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &restored, &diagnostic);
        const bool source_equal = restored_emitted && changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len);
        const bool map_equal = restored_emitted && natural_if_maps_equal(&map, &changed_map);
        const bool quality_equal = restored_emitted && hlsl_source_quality_results_equal(&quality, &changed_quality);
        if (!restored_emitted || !source_equal || !map_equal || !quality_equal) fprintf(stderr,
            "Packed POSITION callback restore attack=%u action=%u emitted=%d status=%s source_equal=%d map_equal=%d quality_equal=%d bytes=%zu/%zu\n",
            attack, drift.mutation, restored_emitted, hlsl_emit_status_name(diagnostic.status), source_equal, map_equal, quality_equal,
            changed.len, baseline.len);
        CHECK(restored_emitted && source_equal && map_equal && quality_equal);
    }
    sb_free(&baseline); sb_free(&changed); natural_if_fixture_dispose(&fixture.decoded); return true;
}

typedef struct {
    char buffer_names[2][32], field_names[2][2][32];
    SerializedVariable fields[2][2];
    SerializedConstantBuffer buffers[2];
    SerializedResourceParam bindings[2];
    SerializedProgramParameters parameters[2];
} PackedPositionMADMetadata;

static bool packed_position_mad_metadata(PackedPositionMADMetadata *metadata, unsigned rows) {
    CHECK(rows == 1 || rows == 2);
    memset(metadata, 0, sizeof(*metadata));
    for (unsigned source = 0; source < 2; ++source) {
        const char *buffer_name = rows == 2 ? "OffsetPositionInputs" : "PositionInputs";
        memcpy(metadata->buffer_names[source], buffer_name, strlen(buffer_name) + 1u);
        for (unsigned row = 0; row < rows; ++row) {
            const char *field_name = rows == 2 && !row ? "_UvOffset" : "_PositionScaleOffset";
            memcpy(metadata->field_names[source][row], field_name, strlen(field_name) + 1u);
            metadata->fields[source][row].name = metadata->field_names[source][row];
            const uint32_t binary[6] = {0, 1, 4, 0, 0, row * 16u};
            const uint32_t text[6] = {row * 16u, 0, 0, 4, 0, 0};
            memcpy(metadata->fields[source][row].layout, source ? text : binary, sizeof(binary));
        }
        metadata->buffers[source] = (SerializedConstantBuffer){.name = metadata->buffer_names[source],
            .role = SERIALIZED_CBUFFER_NAMED, .size = rows * 16u, .variables = metadata->fields[source], .var_count = (int)rows};
        metadata->bindings[source] = (SerializedResourceParam){.name = metadata->buffer_names[source],
            .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .array_size = 1};
        metadata->parameters[source] = (SerializedProgramParameters){.is_binary = !source,
            .constant_buffers = &metadata->buffers[source], .cb_count = 1, .resources = &metadata->bindings[source], .res_count = 1};
        for (unsigned row = 0; row < rows; ++row) {
            DecodedVariableLayout layout;
            CHECK(parameter_layout_decode(&metadata->parameters[source], &metadata->fields[source][row], &layout) &&
                layout.byte_offset == row * 16u && !layout.scalar_type && !layout.is_matrix && !layout.array_size &&
                layout.rows == 1 && layout.columns == 4 && parameter_layout_byte_size(&layout) == 16);
        }
    }
    return true;
}

static bool packed_position_mad_fixture(PackedOutputFixture *fixture, unsigned row, bool different_rows) {
    const PackedPositionMADConfig config = {.row = row, .different_rows = different_rows};
    return packed_output_fixture_layout_position_config(fixture, 1, false, false, 0, 0, 3, &config, NULL);
}

static bool packed_position_material_leaf(const ASTExpr *leaf, const USILProgram *program, int operand,
    unsigned row, unsigned first) {
    CHECK(leaf && leaf->kind == AST_EXPR_EMITTER_OPERAND && !leaf->logical_origin.complete &&
        leaf->operand_provenance.complete && leaf->operand_provenance.value_role == AST_OPERAND_VALUE_LOGICAL &&
        leaf->operand_provenance.logical_value_id == ((UINT64_C(1) << 32) | (uint64_t)(row * 16u)) &&
        leaf->operand_provenance.natural_components == 4 && leaf->operand_provenance.result_components == 2 &&
        leaf->operand_provenance.instruction_index == 0 && leaf->operand_provenance.operand_index == operand &&
        leaf->operand_provenance.source_instruction_index == program->instructions[0].source_instruction_index &&
        leaf->operand_provenance.destination_lanes == 3 &&
        leaf->operand_provenance.selection_role == AST_COMPONENT_SELECTION_SEMANTIC &&
        leaf->operand_provenance.selected_components[0] == first &&
        leaf->operand_provenance.selected_components[1] == first + 1u &&
        !leaf->operand_provenance.synthetic_interface && !leaf->operand_provenance.raw_buffer_reconstruction);
    return true;
}

static bool packed_position_mad_owned_root(PackedOutputFixture *fixture, unsigned row, ASTExpr **owned, char name[96]) {
    HLSLEmitterContext ctx; StringBuilder scratch; HLSLEmitDiagnostic diagnostic;
    CHECK(packed_output_prepare_context(fixture, &ctx, &scratch, &diagnostic));
    HLSLPositionOutputPlan plan, independent;
    CHECK(hlsl_position_output_plan_prepare(ctx.program, &plan) && plan.assembly.present &&
        hlsl_position_output_plan_prepare(ctx.program, &independent) && hlsl_position_output_plans_equal(&plan, &independent) &&
        plan.assembly.pieces[0].opcode == USIL_OP_MAD && plan.assembly.pieces[1].opcode == USIL_OP_MOV &&
        plan.assembly.output_signature_index == 0 && plan.assembly.output.system_value == 1 && plan.assembly.output.mask == 15 &&
        !plan.assembly.output.rw_mask && plan.assembly.return_instruction_index == 4 &&
        plan.immediate_raw_token == UINT32_C(0x4002) && !plan.immediate_words[0] && !plan.immediate_words[1] &&
        !plan.immediate_words[2] && plan.immediate_words[3] == UINT32_C(0x3f800000));
    ctx.position_output_plan = plan;
    for (unsigned piece = 0; piece < 2; ++piece) {
        const USILInstruction *instruction = &ctx.program->instructions[piece];
        const HLSLDomainOutputPiece *receipt = &plan.assembly.pieces[piece];
        const DXBCInstruction *raw = NULL;
        for (int token = 0; token < fixture->decoded.semantic.instruction_count; ++token) {
            const DXBCInstruction *candidate = &fixture->decoded.semantic.instructions[token];
            if (candidate->has_raw_instruction_index && candidate->raw_instruction_index == instruction->source_instruction_index) {
                CHECK(!raw); raw = candidate;
            }
        }
        CHECK(raw && raw->opcode == (piece ? 54u : 50u) && raw->operand_count == (piece ? 2 : 4) &&
            receipt->instruction_index == (int)piece && receipt->source_instruction_index == instruction->source_instruction_index &&
            receipt->destination_raw_token == instruction->operands[0].raw_token && receipt->destination_register == 0 &&
            receipt->mask == (piece ? 12 : 3) && receipt->width == 2 &&
            usil_operand_destination_lane_mask(&instruction->operands[0]) == receipt->mask);
        for (int operand = 1; operand < instruction->operand_count; ++operand) {
            USILOperandUseInfo use;
            CHECK(usil_instruction_operand_use(ctx.program, instruction, operand, &use) &&
                use.use == USIL_OPERAND_USE_SOURCE && use.source_lane_mask == receipt->mask);
        }
    }
    for (int operand = 2; operand < 4; ++operand) {
        const DXBCOperand *source = &ctx.program->instructions[0].operands[operand];
        CHECK(source->type == OPERAND_TYPE_CONSTANT_BUFFER && source->register_index == 0 && source->register_index_dim == 2 &&
            source->rel_offset0 == (int)row && source->index_values[0] == 0 && source->index_values[1] == row &&
            !source->index_representations[0] && !source->index_representations[1] &&
            hlsl_material_source_supported(&ctx, source, 3));
    }
    const char *field = hlsl_high_level_output_field_name(&ctx, 0);
    CHECK(field && strlen(field) < 96); strcpy(name, field);
    ASTExpr *input = hlsl_natural_source_atom(&ctx, 0, 1, 3);
    ASTExpr *scale = hlsl_material_source_expression(&ctx, 0, 2, 3);
    ASTExpr *offset = hlsl_material_source_expression(&ctx, 0, 3, 3);
    CHECK(input && input->kind == AST_EXPR_EMITTER_OPERAND && !input->logical_origin.complete &&
        input->operand_provenance.complete && input->operand_provenance.instruction_index == 0 &&
        input->operand_provenance.operand_index == 1 && input->operand_provenance.source_instruction_index ==
        ctx.program->instructions[0].source_instruction_index && input->operand_provenance.natural_components == 3 &&
        input->operand_provenance.result_components == 2 && input->operand_provenance.destination_lanes == 3 &&
        input->operand_provenance.selected_components[0] == 0 && input->operand_provenance.selected_components[1] == 1 &&
        packed_position_material_leaf(scale, ctx.program, 2, row, 0) && packed_position_material_leaf(offset, ctx.program, 3, row, 2));
    ASTExpr *first = hlsl_natural_float_operation(&ctx, 0, input, scale, offset);
    CHECK(first && first->kind == AST_EXPR_BINARY && first->u.binary.op == USIL_OP_ADD &&
        first->u.binary.right == offset && first->u.binary.left->kind == AST_EXPR_BINARY &&
        first->u.binary.left->u.binary.op == USIL_OP_MUL && first->u.binary.left->u.binary.left == input &&
        first->u.binary.left->u.binary.right == scale && first->logical_origin.complete &&
        first->logical_origin.scalar_type == AST_SCALAR_FLOAT32 && first->logical_origin.components == 2 &&
        first->logical_origin.logical_value_id == 0 && first->logical_origin.instruction_index == 0 &&
        first->logical_origin.source_instruction_index == ctx.program->instructions[0].source_instruction_index &&
        first->logical_origin.destination_lanes == 3);
    ASTExpr *second = hlsl_natural_source_atom(&ctx, 1, 1, 12);
    CHECK(second && second->kind == AST_EXPR_LITERAL && second->u.literal.scalar_type == AST_SCALAR_FLOAT32 &&
        second->u.literal.components == 2 && !second->u.literal.val[0] && second->u.literal.val[1] == UINT32_C(0x3f800000));
    second = hlsl_instruction_logical_expression(&ctx, second, 1, 12, 2);
    CHECK(second && second->logical_origin.complete && second->logical_origin.instruction_index == 1 &&
        second->logical_origin.source_instruction_index == ctx.program->instructions[1].source_instruction_index &&
        second->logical_origin.destination_lanes == 12 && second->logical_origin.components == 2);
    ASTExpr *root = hlsl_output_assembly_expression(&plan.assembly, HLSL_POSITION_OUTPUT_LOGICAL_ID, first, second);
    CHECK(root && root->kind == AST_EXPR_CALL && !strcmp(root->u.call.name, "float4") && root->u.call.arg_count == 2 &&
        root->u.call.args[0] == first && root->u.call.args[1] == second && root->logical_origin.complete &&
        root->logical_origin.scalar_type == AST_SCALAR_FLOAT32 && root->logical_origin.components == 4 &&
        root->logical_origin.logical_value_id == HLSL_POSITION_OUTPUT_LOGICAL_ID &&
        root->logical_origin.instruction_index == -1 && root->logical_origin.source_instruction_index == UINT32_MAX &&
        !root->logical_origin.destination_lanes && !root->logical_origin.program_bitcast);
    *owned = root; dispose(&ctx); sb_free(&scratch); return true;
}

static bool check_packed_position_mad_positive(unsigned row, unsigned authority) {
    CHECK(row < 2 && authority < 4);
    PackedOutputFixture fixture; CHECK(packed_position_mad_fixture(&fixture, row, false));
    PackedPositionMADMetadata metadata; CHECK(packed_position_mad_metadata(&metadata, row + 1u));
    fixture.parameters = authority == 1 ? NULL : &metadata.parameters[0];
    fixture.common_parameters = authority == 0 ? NULL : &metadata.parameters[1];
    if (authority == 3) { metadata.parameters[0].cb_count = 0; metadata.parameters[1].res_count = 0; }
    ASTExpr *root = NULL; char name[96]; CHECK(packed_position_mad_owned_root(&fixture, row, &root, name));
    StringBuilder held, retained, baseline, repeated; sb_init(&held); sb_init(&retained); sb_init(&baseline); sb_init(&repeated);
    ast_format_expr(root, &held); CHECK(sb_ok(&held) && held.len && strstr(held.buf, "_PositionScaleOffset"));
    const char prefix[] = "// caller prefix\n"; sb_append(&baseline, prefix);
    HLSLExpressionSourceMap map, repeated_map; HLSLSourceQualityResult quality, repeated_quality; HLSLEmitDiagnostic diagnostic;
    PackedOutputObservations observer = {.inspect_position = true, .inspect_position_mad = true, .expected_cbuffer_fields = row + 1u};
    const bool emitted = packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic);
    if (!emitted) fprintf(stderr, "Packed POSITION MAD row=%u authority=%u status=%s phase=%s reason=%s instruction=%d\n",
        row, authority, hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase),
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted && packed_output_clean(&quality) && !observer.wrong_owner && observer.position_assemblies == 1 &&
        observer.position_children == 3 && observer.cbuffer_events == row + 3u && observer.cbuffer_field_bytes == 16 &&
        observer.cbuffer_fields_seen == (uint8_t)((1u << (row + 1u)) - 1u) &&
        observer.cbuffer_authority == (authority == 1 || authority == 3 ? 2 : 1) &&
        quality.counts.cbuffer_declarations == 1 && quality.counts.cbuffer_fields == row + 1u &&
        map.complete && map.count == 5 && hlsl_expression_source_map_matches(&map, &fixture.decoded.program, baseline.buf));
    char declaration[128];
    snprintf(declaration, sizeof(declaration), "cbuffer %.31s : register(b0)", metadata.buffer_names[0]);
    CHECK(strstr(baseline.buf, declaration));
    for (unsigned field = 0; field <= row; ++field) {
        snprintf(declaration, sizeof(declaration), "float4 %.31s : packoffset(c%u)", metadata.field_names[0][field], field);
        CHECK(strstr(baseline.buf, declaration));
    }
    char assignment[128]; snprintf(assignment, sizeof(assignment), "output.%.95s = ", name);
    const char *statement = strstr(baseline.buf, assignment); CHECK(statement);
    const size_t begin = (size_t)(statement + strlen(assignment) - baseline.buf);
    CHECK(begin < baseline.len && held.len < baseline.len - begin && !memcmp(baseline.buf + begin, held.buf, held.len) &&
        baseline.buf[begin + held.len] == ';' && !strstr(statement + strlen(assignment), assignment));
    for (unsigned piece = 0; piece < 2; ++piece) {
        StringBuilder child; sb_init(&child); ast_format_expr(root->u.call.args[piece], &child);
        const HLSLExpressionOrigin *origin = &map.origins[piece];
        CHECK(sb_ok(&child) && child.len && origin->kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION &&
            origin->instruction_index == (int)piece && origin->source_instruction_index ==
            fixture.decoded.program.instructions[piece].source_instruction_index && origin->destination_lanes == (piece ? 12 : 3) &&
            origin->source_begin >= begin && origin->source_end <= begin + held.len &&
            origin->source_end - origin->source_begin == child.len && !memcmp(baseline.buf + origin->source_begin, child.buf, child.len));
        sb_free(&child);
    }
    CHECK(map.origins[0].source_end < map.origins[1].source_begin);
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        sb_free(&repeated); sb_init(&repeated); sb_append(&repeated, prefix);
        CHECK(packed_output_emit(&fixture, &repeated, outputs & 1u ? &repeated_map : NULL,
            outputs & 2u ? &repeated_quality : NULL, NULL, &diagnostic) && repeated.len == baseline.len &&
            !memcmp(repeated.buf, baseline.buf, baseline.len));
        if (outputs & 1u) CHECK(natural_if_maps_equal(&map, &repeated_map));
        if (outputs & 2u) CHECK(hlsl_source_quality_results_equal(&quality, &repeated_quality));
    }
    const size_t points[] = {1, observer.observations / 2u, observer.observations};
    for (unsigned point = 0; point < sizeof(points) / sizeof(*points); ++point) {
        PackedOutputObservations veto = {.reject_at = points[point]}; sb_free(&repeated); sb_init(&repeated);
        CHECK(!packed_output_emit(&fixture, &repeated, &repeated_map, &repeated_quality, &veto, &diagnostic) &&
            veto.observations == points[point] && !repeated_map.complete && !repeated_map.count &&
            repeated_quality.classification == HLSL_SOURCE_QUALITY_FAILED && !repeated.len &&
            (!repeated.buf || !repeated.buf[0]));
    }
    natural_if_fixture_dispose(&fixture.decoded); memset(&metadata, 0, sizeof(metadata)); ast_format_expr(root, &retained);
    CHECK(sb_ok(&retained) && retained.len == held.len && !memcmp(retained.buf, held.buf, held.len));
    ast_free_expr(root); sb_free(&held); sb_free(&retained); sb_free(&baseline); sb_free(&repeated); return true;
}

static bool check_packed_position_mad_rejections(void) {
    PackedOutputFixture fixture; CHECK(packed_position_mad_fixture(&fixture, 1, false));
    PackedPositionMADMetadata metadata; CHECK(packed_position_mad_metadata(&metadata, 2));
    fixture.parameters = &metadata.parameters[0];
    USILProgram *program = &fixture.decoded.program;
    StringBuilder baseline, rejected; sb_init(&baseline); sb_init(&rejected);
    HLSLExpressionSourceMap map, rejected_map; HLSLSourceQualityResult quality, rejected_quality; HLSLEmitDiagnostic diagnostic;
    CHECK(packed_output_emit(&fixture, &baseline, &map, &quality, NULL, &diagnostic) && packed_output_clean(&quality));
    USILInstruction instructions[5]; DXBCSignatureElement outputs[3]; USILConstantBuffer buffer = program->cbuffers[0];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(outputs, program->outputs, sizeof(outputs));
    const PackedPositionMADMetadata saved = metadata;
    for (unsigned mutation = 0; mutation < 26; ++mutation) {
        DXBCOperand relative = {0}; uint32_t extension = UINT32_C(0x41);
        DXBCOperand *scale = &program->instructions[0].operands[2], *offset = &program->instructions[0].operands[3];
        switch (mutation) {
        case 0: fixture.parameters = NULL; break;
        case 1: metadata.parameters[0].res_count = 0; break;
        case 2: metadata.parameters[0].cb_count = 0; break;
        case 3: metadata.fields[0][1].layout[2] = 2; break;
        case 4: metadata.fields[0][1].layout[3] = 1; break;
        case 5: metadata.fields[0][1].layout[4] = 1; break;
        case 6: metadata.fields[0][1].layout[0] = 1; break;
        case 7: metadata.fields[0][1].layout[5] += 4; break;
        case 8: offset->rel_offset0 = 0; offset->index_values[1] = 0; break;
        case 9: offset->register_index = 1; offset->index_values[0] = 1; break;
        case 10: { const DXBCOperand old = *scale; *scale = *offset; *offset = old; break; }
        case 11: program->instructions[0].operand_count = 3; break;
        case 12: scale->has_neg = true; scale->raw_token |= UINT32_C(0x80000000);
            scale->extended_tokens = &extension; scale->extended_token_count = 1; break;
        case 13: scale->min_precision = 1; break;
        case 14:
            relative = instructions[0].operands[1]; relative.swizzle_mode = 2;
            memset(relative.swizzle, 0, sizeof(relative.swizzle)); relative.raw_token = UINT32_C(0x0010100a);
            scale->index_representations[1] = 2; scale->index_has_immediate[1] = false; scale->rel_op1 = &relative;
            scale->raw_token = (scale->raw_token & ~(UINT32_C(7) << 25u)) | UINT32_C(2) << 25u; break;
        case 15: program->cbuffers[0].size = 1; break;
        case 16: program->cbuffers[0].dynamic_indexed = true; break;
        case 17: program->instructions[1].operands[1].imm_values[3] = UINT32_C(0x40000000);
            program->instructions[1].operands[1].immediate_words[3] = UINT32_C(0x40000000); break;
        case 18: program->instructions[1].opcode = USIL_OP_NOP; program->instructions[1].operand_count = 0; break;
        case 19: program->instructions[0].opcode = USIL_OP_ADD; program->instructions[0].operand_count = 3; break;
        case 20: program->instructions[0].saturate = true; break;
        case 21: program->outputs[0].system_value = 0; break;
        case 22: metadata.fields[0][0].layout[5] = 16; break;
        case 23: fixture.common_parameters = &metadata.parameters[1]; metadata.buffers[1].size = 64; break;
        case 24:
            offset->swizzle[0] = 0; offset->swizzle[1] = 1;
            offset->raw_token = (natural_if_source_token(OPERAND_TYPE_CONSTANT_BUFFER, offset->swizzle) &
                ~(UINT32_C(3) << 20u)) | UINT32_C(2) << 20u; break;
        case 25: program->instructions[0].precise_mask = 1; break;
        }
        HLSLPositionOutputPlan shape;
        if (mutation < 8 || mutation == 22 || mutation == 23)
            CHECK(hlsl_position_output_plan_prepare(program, &shape)); /* Shape is not typed metadata authority. */
        sb_free(&rejected); sb_init(&rejected);
        const bool failed = !packed_output_emit(&fixture, &rejected, &rejected_map, &rejected_quality, NULL, &diagnostic);
        memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->outputs, outputs, sizeof(outputs));
        program->cbuffers[0] = buffer; metadata = saved;
        fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = NULL;
        if (!failed) fprintf(stderr, "Packed POSITION MAD unexpectedly admitted mutation=%u\n", mutation);
        CHECK(failed && diagnostic.status != HLSL_EMIT_STATUS_OK && !rejected_map.complete && !rejected_map.count &&
            rejected_quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        sb_free(&rejected); sb_init(&rejected);
        CHECK(packed_output_emit(&fixture, &rejected, &rejected_map, &rejected_quality, NULL, &diagnostic) &&
            rejected.len == baseline.len && !memcmp(rejected.buf, baseline.buf, baseline.len) &&
            natural_if_maps_equal(&map, &rejected_map) && hlsl_source_quality_results_equal(&quality, &rejected_quality));
    }
    /* Two adjacent FLOAT2 fields cannot authorize one FLOAT4 MAD field. */
    SerializedVariable halves[2] = {{"_PositionScale", {0, 1, 2, 0, 0, 16}},
        {"_PositionOffset", {0, 1, 2, 0, 0, 24}}};
    SerializedVariable split_fields[3] = {metadata.fields[0][0], halves[0], halves[1]};
    metadata.buffers[0].variables = split_fields; metadata.buffers[0].var_count = 3;
    sb_free(&rejected); sb_init(&rejected);
    const bool split_rejected = !packed_output_emit(&fixture, &rejected, &rejected_map, &rejected_quality, NULL, &diagnostic);
    metadata = saved;
    CHECK(split_rejected && !rejected_map.complete && !rejected_map.count && rejected_quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&baseline); sb_free(&rejected); natural_if_fixture_dispose(&fixture.decoded);
    /* Genuine decoded different-row sources match the measured excluded target. */
    CHECK(packed_position_mad_fixture(&fixture, 0, true) && packed_position_mad_metadata(&metadata, 2));
    fixture.parameters = &metadata.parameters[0];
    HLSLPositionOutputPlan excluded;
    CHECK(!hlsl_position_output_plan_prepare(&fixture.decoded.program, &excluded) && !excluded.assembly.present);
    sb_init(&rejected);
    CHECK(!packed_output_emit(&fixture, &rejected, &rejected_map, &rejected_quality, NULL, &diagnostic) &&
        !rejected_map.complete && !rejected_map.count && rejected_quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&rejected); natural_if_fixture_dispose(&fixture.decoded); return true;
}

static bool check_packed_position_mad_duplicate_field(void) {
    PackedOutputFixture fixture; const PackedPositionMADConfig config = {.second_buffer = true};
    CHECK(packed_output_fixture_layout_position_config(&fixture, 1, false, false, 0, 0, 3, &config, NULL));
    PackedPositionMADMetadata metadata; CHECK(packed_position_mad_metadata(&metadata, 1));
    char other_name[32] = "_PositionScaleOffset";
    SerializedVariable other = {.name = other_name, .layout = {0, 1, 4, 0, 0, 0}};
    SerializedConstantBuffer buffers[2] = {metadata.buffers[0],
        {.name = "DuplicatePositionInputs", .role = SERIALIZED_CBUFFER_NAMED, .size = 16, .variables = &other, .var_count = 1}};
    SerializedResourceParam bindings[2] = {metadata.bindings[0],
        {.name = "DuplicatePositionInputs", .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .bind_index = 1, .array_size = 1}};
    SerializedProgramParameters parameters = {.is_binary = true, .constant_buffers = buffers, .cb_count = 2,
        .resources = bindings, .res_count = 2};
    fixture.parameters = &parameters;
    CHECK(fixture.decoded.program.instructions[2].operands[2].type == OPERAND_TYPE_CONSTANT_BUFFER &&
        fixture.decoded.program.instructions[2].operands[2].register_index == 1);
    HLSLPositionOutputPlan plan;
    CHECK(hlsl_position_output_plan_prepare(&fixture.decoded.program, &plan));
    StringBuilder source; sb_init(&source); HLSLExpressionSourceMap map; HLSLSourceQualityResult quality; HLSLEmitDiagnostic diagnostic;
    CHECK(!packed_output_emit(&fixture, &source, &map, &quality, NULL, &diagnostic) &&
        !map.complete && !map.count && quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    memcpy(other_name, "_DifferentUvOffset", sizeof("_DifferentUvOffset")); sb_free(&source); sb_init(&source);
    CHECK(packed_output_emit(&fixture, &source, &map, &quality, NULL, &diagnostic) && packed_output_clean(&quality) &&
        quality.counts.cbuffer_declarations == 2 && quality.counts.cbuffer_fields == 2 &&
        strstr(source.buf, "_DifferentUvOffset") && hlsl_expression_source_map_matches(&map, &fixture.decoded.program, source.buf));
    memcpy(other_name, "_PositionScaleOffset", sizeof("_PositionScaleOffset")); sb_free(&source); sb_init(&source);
    CHECK(!packed_output_emit(&fixture, &source, &map, &quality, NULL, &diagnostic) && !map.complete && !map.count);
    sb_free(&source); natural_if_fixture_dispose(&fixture.decoded); return true;
}

static bool check_packed_position_mad_callbacks(void) {
    PackedOutputFixture fixture; CHECK(packed_position_mad_fixture(&fixture, 1, false));
    PackedPositionMADMetadata metadata; CHECK(packed_position_mad_metadata(&metadata, 2));
    USILProgram *program = &fixture.decoded.program;
    const char prefix[] = "// caller prefix\n"; char entry_name[] = "entryName";
    const HLSLEmitNames names = {.entry_point = entry_name};
    StringBuilder baseline, changed; sb_init(&baseline); sb_init(&changed);
    HLSLExpressionSourceMap map, changed_map; HLSLSourceQualityResult quality, changed_quality; HLSLEmitDiagnostic diagnostic;
    USILInstruction instructions[5]; DXBCSignatureElement inputs[3], outputs[3];
    memcpy(instructions, program->instructions, sizeof(instructions)); memcpy(inputs, program->inputs, sizeof(inputs));
    memcpy(outputs, program->outputs, sizeof(outputs));
    const PackedPositionMADMetadata saved = metadata;
    for (unsigned authority = 0; authority < 2; ++authority) {
        fixture.parameters = authority ? NULL : &metadata.parameters[0];
        fixture.common_parameters = authority ? &metadata.parameters[1] : NULL;
        sb_free(&baseline); sb_init(&baseline); sb_append(&baseline, prefix);
        PackedOutputObservations observer = {.names = &names, .inspect_position = true, .inspect_position_mad = true,
            .expected_cbuffer_fields = 2};
        CHECK(packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic) && packed_output_clean(&quality) &&
            !observer.wrong_owner && observer.position_assemblies == 1 && observer.position_children == 3);
        const size_t points[] = {1, observer.observations};
        for (unsigned point = 0; point < 2; ++point) {
            PackedOutputObservations drift = {.names = &names, .mutation = 10, .mutate_at = points[point],
                .mutable_field_name = metadata.field_names[authority][1]};
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            CHECK(!packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic) && drift.mutated &&
                !changed_map.complete && !changed_map.count && changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
                changed.len == sizeof(prefix) - 1u && changed.buf && !strcmp(changed.buf, prefix));
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            PackedOutputObservations fresh = {.names = &names};
            CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                packed_output_clean(&changed_quality) && hlsl_expression_source_map_matches(&changed_map, program, changed.buf) &&
                (changed.len != baseline.len || memcmp(changed.buf, baseline.buf, baseline.len)));
            metadata = saved; sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            PackedOutputObservations restored = {.names = &names};
            CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &restored, &diagnostic) &&
                changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) &&
                natural_if_maps_equal(&map, &changed_map) && hlsl_source_quality_results_equal(&quality, &changed_quality));
        }
    }
    fixture.parameters = &metadata.parameters[0]; fixture.common_parameters = &metadata.parameters[1];
    sb_free(&baseline); sb_init(&baseline); sb_append(&baseline, prefix);
    PackedOutputObservations observer = {.names = &names, .inspect_position = true, .inspect_position_mad = true,
        .expected_cbuffer_fields = 2};
    CHECK(packed_output_emit(&fixture, &baseline, &map, &quality, &observer, &diagnostic) && packed_output_clean(&quality) &&
        !observer.wrong_owner && observer.first_config && observer.first_header > observer.first_config &&
        observer.last_header > observer.first_header);
    const struct {unsigned mutation; size_t point, offset; unsigned fresh; bool common;} attacks[] = {
        {12, 1, 0, 1, false}, {18, observer.observations / 2u, 0, 1, false}, {1, observer.observations, 0, 1, false},
        {9, observer.observations, 0, 1, false},
        {15, 1, 0, 2, false}, {15, observer.observations, 0, 2, true},
        {16, observer.observations, 0, 2, false},
        {17, observer.observations / 2u, 0, 0, false}, {13, observer.observations, 0, 0, false},
        {6, observer.first_header, 0, 0, false}, {5, observer.last_header, 0, 0, false},
        {5, observer.first_config, 0, 0, false}, {4, observer.first_config, sizeof(prefix), 0, false},
        {4, 1, 0, 0, false}, {4, observer.observations, map.origins[0].source_begin, 0, false},
        {7, observer.observations, 0, 0, false}};
    for (unsigned attack = 0; attack < sizeof(attacks) / sizeof(*attacks); ++attack) {
        SerializedVariable replacement[2]; memcpy(replacement, metadata.fields[0], sizeof(replacement));
        PackedOutputObservations drift = {.names = &names, .mutation = attacks[attack].mutation, .mutate_at = attacks[attack].point,
            .source_offset = attacks[attack].offset, .field = fixture.zw_field, .writer = fixture.zw_writer,
            .mutable_entry_name = entry_name, .mutable_parameters = &metadata.parameters[attacks[attack].common ? 1 : 0],
            .replacement_fields = replacement};
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        const bool rejected = !packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &drift, &diagnostic);
        /* Retain replacement storage until emission completes, then detach it. */
        if (drift.mutation == 16) metadata.buffers[0].variables = metadata.fields[0];
        if (!rejected) fprintf(stderr, "Packed POSITION MAD callback falsely accepted attack=%u action=%u point=%zu\n",
            attack, drift.mutation, drift.mutate_at);
        CHECK(rejected && drift.mutated && !changed_map.complete && !changed_map.count &&
            changed_quality.classification == HLSL_SOURCE_QUALITY_FAILED && changed.len == sizeof(prefix) - 1u &&
            changed.buf && !strcmp(changed.buf, prefix));
        if (attacks[attack].fresh) {
            sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
            PackedOutputObservations fresh = {.names = &names};
            CHECK(packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &fresh, &diagnostic) &&
                packed_output_clean(&changed_quality) && hlsl_expression_source_map_matches(&changed_map, program, changed.buf));
            if (attacks[attack].fresh == 1) CHECK(changed.len != baseline.len || memcmp(changed.buf, baseline.buf, baseline.len));
            else CHECK(changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len) &&
                natural_if_maps_equal(&map, &changed_map) && hlsl_source_quality_results_equal(&quality, &changed_quality));
        }
        memcpy(program->instructions, instructions, sizeof(instructions)); memcpy(program->inputs, inputs, sizeof(inputs));
        memcpy(program->outputs, outputs, sizeof(outputs)); metadata = saved; memcpy(entry_name, "entryName", sizeof(entry_name));
        sb_free(&changed); sb_init(&changed); sb_append(&changed, prefix);
        PackedOutputObservations restored = {.names = &names};
        const bool restored_emitted = packed_output_emit(&fixture, &changed, &changed_map, &changed_quality, &restored, &diagnostic);
        const bool source_equal = restored_emitted && changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len);
        const bool map_equal = restored_emitted && natural_if_maps_equal(&map, &changed_map);
        const bool quality_equal = restored_emitted && hlsl_source_quality_results_equal(&quality, &changed_quality);
        if (!source_equal || !map_equal || !quality_equal) fprintf(stderr,
            "Packed POSITION MAD callback restore attack=%u action=%u emitted=%d status=%s source=%d map=%d quality=%d\n",
            attack, drift.mutation, restored_emitted, hlsl_emit_status_name(diagnostic.status), source_equal, map_equal, quality_equal);
        CHECK(source_equal && map_equal && quality_equal);
    }
    sb_free(&baseline); sb_free(&changed); natural_if_fixture_dispose(&fixture.decoded); return true;
}

static bool check_packed_output_emission(void) {
    for (unsigned arithmetic = 0; arithmetic < 4; ++arithmetic)
        CHECK(check_packed_output_positive(arithmetic, arithmetic == 1, false));
    CHECK(check_packed_output_positive(0, true, true));
    CHECK(check_packed_output_rejections() && check_packed_output_callbacks() && check_packed_output_cbuffer() &&
        check_packed_output_matrix_cbuffer());
    for (unsigned placement = 0; placement < 3; ++placement) CHECK(check_packed_matrix_receipt_positive(placement, 0));
    CHECK(check_packed_matrix_receipt_positive(0, 1) && check_packed_matrix_receipt_positive(2, 2) &&
        check_packed_matrix_receipt_positive(1, 3) && check_packed_matrix_receipt_boundaries() &&
        check_packed_matrix_receipt_callbacks());
    CHECK(check_packed_position_positive(3, 0, false, false) && check_packed_position_positive(2, 1, true, true) &&
        check_packed_position_positive(4, 2, false, false) && check_packed_position_rejections() && check_packed_position_callbacks());
    for (unsigned row = 0; row < 2; ++row) for (unsigned authority = 0; authority < 3; ++authority)
        CHECK(check_packed_position_mad_positive(row, authority));
    CHECK(check_packed_position_mad_positive(1, 3) && check_packed_position_mad_rejections() &&
        check_packed_position_mad_duplicate_field() && check_packed_position_mad_callbacks());
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
        !check_natural_conditional_emission() || !check_packed_output_emission() ||
        !check_counted_loop_emission() || !check_function_emission() || !check_unity_uv_emission())
        return 1;
    puts("HLSL dataflow contracts passed");
    return 0;
}
