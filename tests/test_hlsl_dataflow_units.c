// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static DXBCOperand reg(DXBCOperandType type, int index, uint8_t mask) {
    DXBCOperand result = {0};
    result.type = type;
    result.register_index = index;
    result.destination_mask = mask;
    result.swizzle_mode = 1;
    for (uint8_t lane = 0; lane < 4; ++lane) result.swizzle[lane] = lane;
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

static int variable(const HLSLEmitterContext *ctx, int instruction,
                    int operand, int lane) {
    return ctx->ssa.operand_ssa_vars[
        ((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u +
        (size_t)lane];
}

static bool check_multiple_results(void) {
    const USILOpcode opcodes[] = {USIL_OP_IMUL, USIL_OP_UDIV, USIL_OP_SINCOS};
    for (size_t kind = 0; kind < sizeof(opcodes) / sizeof(opcodes[0]); ++kind) {
        USILInstruction instructions[5] = {0};
        instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0xf0),
                               reg(OPERAND_TYPE_INPUT, 0, 0));
        instructions[1].opcode = opcodes[kind];
        instructions[1].operand_count = opcodes[kind] == USIL_OP_SINCOS ? 3 : 4;
        instructions[1].operands[0] = reg(OPERAND_TYPE_TEMP, 1, 0x10);
        instructions[1].operands[1] = reg(OPERAND_TYPE_TEMP, 2, 0x10);
        instructions[1].operands[2] = reg(OPERAND_TYPE_TEMP, 0, 0);
        instructions[1].operands[2].swizzle[0] = 3;
        instructions[1].operands[3] = reg(OPERAND_TYPE_TEMP, 0, 0);
        instructions[2] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10),
                               reg(OPERAND_TYPE_TEMP, 2, 0));
        instructions[3] = move(reg(OPERAND_TYPE_OUTPUT, 1, 0x10),
                               reg(OPERAND_TYPE_TEMP, 0, 0));
        instructions[4].opcode = USIL_OP_RET;
        USILProgram program = {.instructions = instructions,
                               .instruction_count = 5, .temp_count = 3};
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
        instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10),
                               reg(OPERAND_TYPE_INPUT, 0, 0));
        instructions[1] = move(reg(OPERAND_TYPE_TEMP, 1, 0x10),
                               reg(OPERAND_TYPE_TEMP, 0, 0));
        instructions[1].operands[1].has_neg = modifier == 1;
        instructions[1].operands[1].has_abs = modifier == 2;
        instructions[1].saturate = modifier == 3;
        instructions[1].operands[1].min_precision = modifier == 4 ? 1 : 0;
        instructions[2].opcode = USIL_OP_RET;
        USILProgram program = {.instructions = instructions,
                               .instruction_count = 3, .temp_count = 2};
        HLSLEmitterContext ctx;
        CHECK(analyze(&ctx, &program));
        CHECK(get_component_provenance(&ctx, 2, 1, 0)->root_instruction ==
               (modifier == 0 ? 0 : 1));
        dispose(&ctx);
    }
    return true;
}

static bool check_merge_and_undefined_lanes(void) {
    USILInstruction instructions[5] = {0};
    instructions[0].opcode = USIL_OP_IF;
    instructions[0].operand_count = 1;
    instructions[0].operands[0] = reg(OPERAND_TYPE_INPUT, 0, 0);
    instructions[1] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10),
                           reg(OPERAND_TYPE_INPUT, 0, 0));
    instructions[2].opcode = USIL_OP_ENDIF;
    instructions[3] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10),
                           reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[4].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions,
                           .instruction_count = 5, .temp_count = 1};
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
        if (value < 0) ++undefined;
        else if (ctx.ssa.ssa_var_defs[value] == 1) ++defined;
    }
    CHECK(undefined == 1 && defined == 1);
    CHECK(hlsl_definition_use_count(&ctx, 1, 0) == 1);
    dispose(&ctx);
    return true;
}

static bool check_loop_phi(void) {
    USILInstruction instructions[7] = {0};
    instructions[0] = move(reg(OPERAND_TYPE_TEMP, 0, 0x10),
                           reg(OPERAND_TYPE_INPUT, 0, 0));
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
    instructions[5] = move(reg(OPERAND_TYPE_OUTPUT, 0, 0x10),
                           reg(OPERAND_TYPE_TEMP, 0, 0));
    instructions[6].opcode = USIL_OP_RET;
    USILProgram program = {.instructions = instructions,
                           .instruction_count = 7, .temp_count = 1};
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

int main(void) {
    if (!check_multiple_results() || !check_modified_moves() ||
        !check_merge_and_undefined_lanes() || !check_loop_phi()) return 1;
    puts("HLSL dataflow contracts passed");
    return 0;
}
