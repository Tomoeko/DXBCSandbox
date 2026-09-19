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
    const HLSLLiftArtifact *accepted = hlsl_lift_transaction_artifact(transaction);
    const char *baseline_source = accepted->source;
    const uint8_t *baseline_bytes = accepted->dxbc;
    fixture.mutate = true;
    CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) == HLSL_LIFT_DXBC_MISMATCH);
    CHECK(result.compared && result.comparison.status == DXBC_COMPARE_INSTRUCTION_OPCODE);
    CHECK(hlsl_lift_transaction_program(transaction) == &program);
    CHECK(accepted->source == baseline_source && accepted->dxbc == baseline_bytes);
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
    return true;
}

int main(void) {
    if (!check_multiple_results() || !check_modified_moves() ||
        !check_merge_and_undefined_lanes() || !check_loop_phi() || !check_copy_candidates() ||
        !check_effects() || !check_transactions())
        return 1;
    puts("HLSL dataflow contracts passed");
    return 0;
}
