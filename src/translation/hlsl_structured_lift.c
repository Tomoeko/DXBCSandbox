// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include "common/sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reuse the expression contracts, CFG and lane SSA. The established full
 * vector route retains its spelling and loop boundary. A separate bounded
 * natural-width mode proves one complete two-arm join and the physical lanes
 * belonging to each scalar/vector value before assigning it a source type. */
enum { STRUCTURED_VALUE_LIMIT = HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT * 2 };
#define STRUCTURED_PHI_LOGICAL_ID_BASE UINT64_C(0x8000000000060000)

typedef struct {
    int instruction; /* -1 for a phi vector. */
    int block;
    int register_index;
    uint8_t mask, width;
    const HLSLPhiNode *lanes[4];
    int incoming[2];
    bool predicate;
    int predicate_if;
    unsigned resolution; /* 0 unseen, 1 visiting, 2 proven/live. */
    char name[48];
} StructuredValue;

typedef struct {
    int instruction, end, comparison, test, increment, initialization;
    int body_block, latch_block, exit_block, initial_edge, latch_edge;
    int counter_phi, predicate_phi, initial_variable, increment_variable, predicate_variable;
    uint32_t initial_bits;
} CountedLoop;

typedef struct {
    StructuredValue values[STRUCTURED_VALUE_LIMIT];
    int value_count;
    int instruction_value[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT];
    int join_if[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT];
    HLSLIfRegion regions[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT];
    int *ssa_value;
    int *ssa_lane;
    CountedLoop loop;
    bool natural_width;
} StructuredPlan;

/* This receipt is producer-private. Expected syntax owners are independently
 * enumerated from the proved plan before any body callback. Actual ranges are
 * recorded only after their syntax was appended, then replayed through the
 * same planner/emitter without observers. No source names or text are parsed. */
typedef enum {
    NATURAL_BODY_PHI_DECLARATION,
    NATURAL_BODY_PHI_EDGE,
    NATURAL_BODY_IF,
    NATURAL_BODY_ELSE,
    NATURAL_BODY_ENDIF,
    NATURAL_BODY_TEMP_ASSIGNMENT,
    NATURAL_BODY_OUTPUT_ASSIGNMENT,
    NATURAL_BODY_RETURN
} NaturalBodySyntaxKind;

enum {
    NATURAL_BODY_RECEIPT_LIMIT = STRUCTURED_VALUE_LIMIT * 3 + HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT + 1,
    NATURAL_BODY_BYTE_LIMIT = 1024 * 1024
};

typedef struct {
    NaturalBodySyntaxKind kind;
    int instruction, value, incoming, edge;
    uint32_t source_instruction;
    uint8_t mask, width;
    ASTScalarType scalar_type;
    size_t begin, end, expression_begin, expression_end;
} NaturalBodySyntax;

typedef struct HLSLNaturalStructuredBodyInventory {
    NaturalBodySyntax expected[NATURAL_BODY_RECEIPT_LIMIT];
    NaturalBodySyntax emitted[NATURAL_BODY_RECEIPT_LIMIT];
    size_t expected_count, emitted_count;
    size_t source_begin, body_end, source_end;
    int indent;
    char *source;
    uint8_t prefix_digest[COMMON_SHA256_DIGEST_SIZE];
    HLSLExpressionSourceMap internal_map, map;
    HLSLExpressionSourceMap *caller_map;
    bool sealed;
} HLSLNaturalStructuredBodyInventory;

static bool reject(HLSLEmitterContext *ctx, int instruction, HLSLEmitReason reason) {
    hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, reason, instruction, -1);
    return false;
}

static bool natural_model_stable(HLSLEmitterContext *ctx) {
    if (!ctx->natural_structured_owners_guarded) return true;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    if (hlsl_natural_structured_owned_contract_digest(ctx->program, digest) &&
        !memcmp(digest, ctx->natural_structured_owner_digest, sizeof(digest))) return true;
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
        HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    return false;
}

static unsigned mask_width(uint8_t mask) {
    unsigned width = 0;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (mask & (1u << lane)) ++width;
    return width;
}

static bool scalar_comparison_opcode(USILOpcode opcode) {
    return opcode == USIL_OP_LT || opcode == USIL_OP_GE ||
        opcode == USIL_OP_EQ || opcode == USIL_OP_NE;
}

static bool natural_arithmetic_opcode(USILOpcode opcode) {
    return opcode == USIL_OP_MOV || opcode == USIL_OP_ADD || opcode == USIL_OP_MUL ||
        opcode == USIL_OP_MIN || opcode == USIL_OP_MAX || opcode == USIL_OP_DIV;
}

/* A loop's scalar induction values belong to the old FLOAT4 route. Select
 * natural mode only for a new width in a loop-free instruction stream; the
 * subsequent closed gate still rejects every unsupported opcode or shape. */
static bool natural_width_requested(const USILProgram *program) {
    for (int index = 0; index < program->instruction_count; ++index)
        if (program->instructions[index].opcode == USIL_OP_LOOP) return false;
    for (int direction = 0; direction < 2; ++direction) {
        const DXBCSignatureElement *fields = direction ? program->outputs : program->inputs;
        const int count = direction ? program->output_count : program->input_count;
        for (int index = 0; index < count; ++index)
            if (fields[index].mask != 15) return true;
    }
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        if ((natural_arithmetic_opcode(instruction->opcode) || scalar_comparison_opcode(instruction->opcode)) && instruction->operand_count &&
            usil_operand_destination_lane_mask(&instruction->operands[0]) != 15) return true;
    }
    return false;
}

static bool natural_program_features_supported(const USILProgram *program) {
    return (program->program_type == DXBC_PROGRAM_TYPE_VERTEX ||
        program->program_type == DXBC_PROGRAM_TYPE_PIXEL) &&
        !program->cbuffer_count && !program->texture_count && !program->sampler_count &&
        !program->patch_constant_count;
}

static bool natural_signature_layout_supported(const USILProgram *program) {
    if (!hlsl_natural_input_layout_supported(program, NULL)) return false;
    const DXBCSignatureElement *fields = program->outputs;
    for (int index = 0; index < program->output_count; ++index) {
        if (!hlsl_signature_semantic_storage_valid(&fields[index]) ||
            !fields[index].mask || fields[index].mask > 15 ||
            (fields[index].mask & (fields[index].mask + 1u)) ||
            fields[index].register_id >= HLSL_SM5_IO_REGISTER_COUNT)
            return false;
        for (int previous = 0; previous < index; ++previous)
            if (fields[previous].register_id == fields[index].register_id)
                return false;
    }
    return true;
}

static bool natural_program_supported(HLSLEmitterContext *ctx) {
    if (!hlsl_natural_float_program_supported(ctx)) return false;
    if (!natural_program_features_supported(ctx->program))
        return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    return natural_signature_layout_supported(ctx->program) ||
        reject(ctx, -1, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
}

/* Only defer the early interface rejection long enough to prepare CFG/SSA.
 * This hint shares the program/layout gates and width classifier with the
 * planner, but proves no joins, reaching definitions, output completeness or
 * source syntax. Only the later complete preflight may enable an interface. */
bool hlsl_natural_structured_candidate(const USILProgram *program) {
    if (!program || !program->instructions || program->instruction_count < 1 ||
        program->instruction_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        program->instruction_alloc < program->instruction_count ||
        program->input_count < 0 || program->input_count > HLSL_SM5_IO_REGISTER_COUNT ||
        program->input_alloc < program->input_count ||
        (program->input_count && !program->inputs) ||
        program->output_count < 1 || program->output_count > HLSL_SM5_IO_REGISTER_COUNT ||
        program->output_alloc < program->output_count ||
        !program->outputs || !program->has_stage_contract || !program->has_parsed_signature_authority ||
        program->signature_declaration_count < 0 ||
        program->signature_declaration_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        !natural_program_features_supported(program) ||
        !natural_signature_layout_supported(program) ||
        hlsl_natural_float_program_contract(program) != HLSL_EMIT_REASON_NONE ||
        !usil_signature_authority_is_valid(program) || !natural_width_requested(program))
        return false;
    unsigned conditions = 0, alternatives = 0, joins = 0;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILOpcode opcode = program->instructions[index].opcode;
        if (opcode == USIL_OP_IF) ++conditions;
        else if (opcode == USIL_OP_ELSE) ++alternatives;
        else if (opcode == USIL_OP_ENDIF) ++joins;
    }
    return conditions == 1u && alternatives == 1u && joins == 1u;
}

static uint8_t demanded_lanes(const HLSLEmitterContext *ctx, int instruction, int operand) {
    USILOperandUseInfo use;
    return usil_instruction_operand_use(ctx->program, &ctx->program->instructions[instruction],
                operand, &use) && use.use == USIL_OPERAND_USE_SOURCE ? use.source_lane_mask : 0;
}

static int operand_variable(const HLSLEmitterContext *ctx, int instruction, int operand, int lane) {
    return ctx->ssa
        .operand_ssa_vars[((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u +
                          (size_t)lane];
}

static bool map_variable(const HLSLEmitterContext *ctx, StructuredPlan *plan, int variable,
                         int value, int lane) {
    if (variable < 0 || variable >= ctx->ssa.ssa_var_count || plan->ssa_value[variable] != -1)
        return false;
    plan->ssa_value[variable] = value;
    plan->ssa_lane[variable] = lane;
    return true;
}

static bool resolve_value(HLSLEmitterContext *ctx, StructuredPlan *plan, int index, int depth) {
    if (index < 0 || index >= plan->value_count || depth > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT)
        return false;
    StructuredValue *value = &plan->values[index];
    if (value->resolution == 2)
        return true;
    if (value->resolution == 1)
        return value->instruction < 0 && plan->loop.instruction >= 0 &&
               value->block == plan->loop.body_block;
    value->resolution = 1;
    if (value->instruction < 0) {
        const HLSLBasicBlock *block = &ctx->cfg.blocks[value->block];
        if (block->predecessor_count != 2 ||
            (plan->join_if[value->block] < 0 &&
             !(plan->loop.instruction >= 0 && value->block == plan->loop.body_block)))
            return false;
        for (int edge = 0; edge < 2; ++edge) {
            int incoming = -1;
            for (int lane = 0; lane < 4; ++lane) {
                if (plan->natural_width && !(value->mask & (1u << lane))) continue;
                const HLSLPhiNode *phi = value->lanes[lane];
                if (!phi || phi->incoming_blocks[edge] != block->predecessors[edge])
                    return false;
                int variable = phi->incoming_vars[edge];
                if (variable < 0 || variable >= ctx->ssa.ssa_var_count ||
                    plan->ssa_lane[variable] != lane || plan->ssa_value[variable] < 0)
                    return false;
                int source = plan->ssa_value[variable];
                if ((incoming >= 0 && incoming != source) ||
                    !hlsl_cfg_dominates(&ctx->cfg, plan->values[source].block,
                                        block->predecessors[edge]))
                    return false;
                incoming = source;
            }
            if (!resolve_value(ctx, plan, incoming, depth + 1))
                return false;
            /* One natural value must own every lane on both edges. A
             * collection of independently updated register fragments cannot
             * supply a complete typed phi, even if its consumed lane exists. */
            if (plan->natural_width && (!value->mask || !value->width ||
                plan->values[incoming].mask != value->mask ||
                plan->values[incoming].width != value->width || plan->values[incoming].predicate))
                return false;
            value->incoming[edge] = incoming;
        }
    }
    value->resolution = 2;
    return true;
}

static int source_value(HLSLEmitterContext *ctx, StructuredPlan *plan, int instruction, int operand,
                        int lanes) {
    const DXBCOperand *source = &ctx->program->instructions[instruction].operands[operand];
    int result = -1;
    const uint8_t mask = plan->natural_width ? demanded_lanes(ctx, instruction, operand)
        : (uint8_t)((1u << lanes) - 1u);
    if (!mask) return -1;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(mask & (1u << lane))) continue;
        int variable = operand_variable(ctx, instruction, operand, lane);
        if (variable < 0 || variable >= ctx->ssa.ssa_var_count || plan->ssa_value[variable] < 0 ||
            plan->ssa_lane[variable] != usil_operand_source_component(source, lane))
            return -1;
        int value = plan->ssa_value[variable];
        if (plan->natural_width && plan->values[value].predicate &&
            (ctx->program->instructions[instruction].opcode != USIL_OP_IF || operand != 0 ||
             instruction != plan->values[value].predicate_if)) return -1;
        if ((result >= 0 && result != value) ||
            !hlsl_cfg_dominates(&ctx->cfg, plan->values[value].block,
                                ctx->cfg.instruction_block[instruction]))
            return -1;
        result = value;
    }
    return resolve_value(ctx, plan, result, 0) ? result : -1;
}

static int destination_component(const DXBCOperand *operand) {
    const unsigned mask = usil_operand_destination_lane_mask(operand);
    for (int lane = 0; lane < 4; ++lane)
        if (mask == (1u << lane))
            return lane;
    return -1;
}

static bool plain_instruction(const USILInstruction *instruction) {
    if (instruction->precise_mask || instruction->saturate)
        return false;
    for (int operand = 0; operand < instruction->operand_count; ++operand)
        if (!hlsl_lift_operand_is_plain(&instruction->operands[operand]))
            return false;
    return true;
}

/* An unsigned unit-step induction with an immutable input-bit bound. UGE
 * returns a mask; only its exact BREAKC_NZ consumes it. While counter < bound,
 * counter <= UINT_MAX-1, so the increment cannot wrap. No float comparison or
 * signed-overflow inference is involved. A zero-iteration loop stays possible. */
static bool match_counted_loop(HLSLEmitterContext *ctx, int index, CountedLoop *result) {
    const USILProgram *program = ctx->program;
    const HLSLControlFlowGraph *cfg = &ctx->cfg;
    if (!ctx->loop_info || !ctx->loop_info[index].is_optimized || !cfg->instruction_flow ||
        !ctx->ssa.operand_ssa_vars || !ctx->ssa.block_phis)
        return false;
    CountedLoop loop = {.instruction = index, .predicate_phi = -1};
    loop.end = cfg->instruction_flow[index].end;
    loop.comparison = ctx->loop_info[index].comparison_inst_idx;
    loop.test = ctx->loop_info[index].breakc_inst_idx;
    loop.increment = loop.end - 1;
    if (loop.comparison != index + 1 || loop.test != index + 2 || loop.increment <= loop.test ||
        loop.end >= program->instruction_count - 1)
        return false;
    const USILInstruction *comparison = &program->instructions[loop.comparison];
    const USILInstruction *test = &program->instructions[loop.test];
    const USILInstruction *increment = &program->instructions[loop.increment];
    if (comparison->opcode != USIL_OP_UGE || increment->opcode != USIL_OP_IADD ||
        test->condition_test != DXBC_INSTRUCTION_TEST_NONZERO || !plain_instruction(comparison) ||
        !plain_instruction(test) || !plain_instruction(increment))
        return false;
    int predicate_lane = destination_component(&comparison->operands[0]);
    int counter_lane = destination_component(&increment->operands[0]);
    if (predicate_lane < 0 || counter_lane < 0 ||
        comparison->operands[1].type != OPERAND_TYPE_TEMP ||
        comparison->operands[2].type != OPERAND_TYPE_INPUT ||
        increment->operands[0].type != OPERAND_TYPE_TEMP ||
        increment->operands[1].type != OPERAND_TYPE_TEMP ||
        increment->operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
        increment->operands[2].imm_value_count != 1 || increment->operands[2].imm_values[0] != 1u)
        return false;
    loop.counter_phi = operand_variable(ctx, loop.comparison, 1, predicate_lane);
    loop.predicate_variable = operand_variable(ctx, loop.comparison, 0, predicate_lane);
    loop.increment_variable = operand_variable(ctx, loop.increment, 0, counter_lane);
    if (loop.counter_phi < 0 || loop.predicate_variable < 0 || loop.increment_variable < 0 ||
        operand_variable(ctx, loop.increment, 1, counter_lane) != loop.counter_phi ||
        operand_variable(ctx, loop.test, 0, 0) != loop.predicate_variable)
        return false;
    loop.body_block = cfg->instruction_block[loop.comparison];
    loop.latch_block = cfg->instruction_block[loop.end];
    loop.exit_block = cfg->instruction_block[loop.end + 1];
    const int preheader = cfg->instruction_block[index];
    const HLSLBasicBlock *body = &cfg->blocks[loop.body_block];
    if (body->predecessor_count != 2 || !hlsl_cfg_dominates(cfg, preheader, loop.exit_block) ||
        !hlsl_cfg_must_reach(cfg, cfg->instruction_block[loop.test + 1], loop.latch_block))
        return false;
    loop.initial_edge = body->predecessors[0] == preheader ? 0 : 1;
    loop.latch_edge = 1 - loop.initial_edge;
    if (body->predecessors[loop.initial_edge] != preheader ||
        body->predecessors[loop.latch_edge] != loop.latch_block ||
        cfg->blocks[preheader].successor_count != 1 ||
        cfg->blocks[preheader].successors[0] != loop.body_block ||
        cfg->blocks[loop.latch_block].successor_count != 1 ||
        cfg->blocks[loop.latch_block].successors[0] != loop.body_block)
        return false;
    const HLSLBlockPhis *phis = &ctx->ssa.block_phis[loop.body_block];
    const HLSLPhiNode *counter = NULL;
    for (int item = 0; item < phis->phi_count; ++item) {
        const HLSLPhiNode *phi = &phis->phis[item];
        if (phi->ssa_var == loop.counter_phi)
            counter = phi;
        if (phi->register_index == comparison->operands[0].register_index &&
            phi->component == predicate_lane)
            loop.predicate_phi = phi->ssa_var;
    }
    if (!counter || counter->component != counter_lane ||
        counter->register_index != increment->operands[0].register_index ||
        counter->incoming_vars[loop.latch_edge] != loop.increment_variable)
        return false;
    loop.initial_variable = counter->incoming_vars[loop.initial_edge];
    if (loop.initial_variable < 0 || loop.initial_variable >= ctx->ssa.ssa_var_count)
        return false;
    loop.initialization = ctx->ssa.ssa_var_defs[loop.initial_variable];
    if (loop.initialization < 0 || loop.initialization >= index ||
        !hlsl_cfg_dominates(cfg, cfg->instruction_block[loop.initialization], preheader))
        return false;
    const USILInstruction *initial = &program->instructions[loop.initialization];
    if (initial->opcode != USIL_OP_MOV || !plain_instruction(initial) ||
        initial->operands[0].type != OPERAND_TYPE_TEMP ||
        initial->operands[0].register_index != counter->register_index ||
        destination_component(&initial->operands[0]) != counter_lane ||
        initial->operands[1].type != OPERAND_TYPE_IMMEDIATE32 ||
        initial->operands[1].imm_value_count != 1)
        return false;
    loop.initial_bits = initial->operands[1].imm_values[0];
    /* Ignore a dead unpruned predicate phi, but never any real use of it or
     * of a scalar control value outside this exact induction/predicate. */
    for (int i = 0; i < program->instruction_count; ++i) {
        const USILInstruction *inst = &program->instructions[i];
        for (int operand = 0; operand < inst->operand_count; ++operand) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(program, inst, operand, &use))
                return false;
            if (use.use != USIL_OPERAND_USE_SOURCE ||
                inst->operands[operand].type != OPERAND_TYPE_TEMP)
                continue;
            for (int lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane)))
                    continue;
                int variable = operand_variable(ctx, i, operand, lane);
                if (variable < 0)
                    continue;
                if (variable == loop.predicate_phi || variable == loop.initial_variable ||
                    variable == loop.increment_variable)
                    return false;
                if (variable == loop.counter_phi &&
                    !((i == loop.comparison && operand == 1 && lane == predicate_lane) ||
                      (i == loop.increment && operand == 1 && lane == counter_lane)))
                    return false;
                if (variable == loop.predicate_variable &&
                    !(i == loop.test && operand == 0 && lane == 0))
                    return false;
            }
        }
    }
    *result = loop;
    return true;
}

static bool is_loop_control(const CountedLoop *loop, int index) {
    return loop->instruction >= 0 &&
           (index == loop->instruction || index == loop->end || index == loop->comparison ||
            index == loop->test || index == loop->increment || index == loop->initialization);
}

static bool build_plan(HLSLEmitterContext *ctx, StructuredPlan *plan) {
    plan->natural_width = natural_width_requested(ctx->program);
    if (!(plan->natural_width ? natural_program_supported(ctx) : hlsl_float4_program_supported(ctx)))
        return false;
    const USILProgram *program = ctx->program;
    bool packed_inputs = false;
    if (plan->natural_width && !hlsl_natural_input_layout_supported(program, &packed_inputs))
        return reject(ctx, -1, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    if (!ctx->ssa.operand_ssa_vars || !ctx->ssa.block_phis || ctx->ssa.ssa_var_count < 0 ||
        ctx->cfg.block_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        dxbc_size_multiply_overflows((size_t)ctx->ssa.ssa_var_count, sizeof(int)))
        return reject(ctx, -1, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    const size_t variables = ctx->ssa.ssa_var_count ? (size_t)ctx->ssa.ssa_var_count : 1u;
    plan->ssa_value = malloc(variables * sizeof(int));
    plan->ssa_lane = malloc(variables * sizeof(int));
    if (!plan->ssa_value || !plan->ssa_lane)
        return false;
    for (size_t index = 0; index < variables; ++index)
        plan->ssa_value[index] = plan->ssa_lane[index] = -1;
    for (int index = 0; index < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++index)
        plan->instruction_value[index] = plan->join_if[index] = -1;
    if (program->instructions[program->instruction_count - 1].opcode != USIL_OP_RET)
        return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    plan->loop.instruction = -1;
    for (int index = 0; index < program->instruction_count; ++index)
        if (program->instructions[index].opcode == USIL_OP_LOOP) {
            if (plan->loop.instruction >= 0 || !match_counted_loop(ctx, index, &plan->loop))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        }
    /* Compose only the established LOOP/compare/BREAKC prefix claim. Other
     * compiler inverses must not silently disappear in this emission path. */
    if (plan->loop.instruction >= 0) {
        if (ctx->compiler_model.replacement_count != 1 || !ctx->compiler_model.claim_owner ||
            ctx->loop_info[plan->loop.instruction].inc_inst_idx >= 0)
            return reject(ctx, plan->loop.instruction, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        for (int index = 0; index < program->instruction_count; ++index) {
            const bool claimed = index == plan->loop.instruction ||
                                 index == plan->loop.comparison || index == plan->loop.test;
            if (ctx->compiler_model.claim_owner[index] != (claimed ? 0 : -1))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        }
    } else if (ctx->compiler_model.replacement_count) {
        return reject(ctx, -1, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
    const int return_block = ctx->cfg.instruction_block[program->instruction_count - 1];
    bool outputs[HLSL_SM5_IO_REGISTER_COUNT] = {0};
    unsigned natural_if_count = 0, natural_else_count = 0, natural_end_count = 0;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *inst = &program->instructions[index];
        if (inst->precise_mask || inst->saturate)
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        if (inst->opcode == USIL_OP_RET) {
            if (index + 1 != program->instruction_count)
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            continue;
        }
        if (is_loop_control(&plan->loop, index))
            continue;
        if (inst->opcode == USIL_OP_NOP || inst->opcode == USIL_OP_ELSE ||
            inst->opcode == USIL_OP_ENDIF) {
            if (plan->natural_width) {
                if (inst->opcode == USIL_OP_ELSE) ++natural_else_count;
                if (inst->opcode == USIL_OP_ENDIF) ++natural_end_count;
            }
            continue;
        }
        if (inst->opcode == USIL_OP_IF) {
            if (plan->loop.instruction >= 0)
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            if (!hlsl_cfg_if_region(ctx, index, &plan->regions[index]))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            if (plan->natural_width && (++natural_if_count != 1 ||
                plan->regions[index].else_instruction < 0 || inst->operand_count != 1 ||
                (inst->condition_test != DXBC_INSTRUCTION_TEST_ZERO &&
                 inst->condition_test != DXBC_INSTRUCTION_TEST_NONZERO)))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            int join = plan->regions[index].join_block;
            if (plan->join_if[join] >= 0)
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            plan->join_if[join] = index;
            const DXBCOperand *condition = &inst->operands[0];
            if ((condition->type != OPERAND_TYPE_INPUT && condition->type != OPERAND_TYPE_TEMP) ||
                !hlsl_lift_operand_is_plain(condition))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            if (packed_inputs && condition->type == OPERAND_TYPE_INPUT) {
                HLSLNaturalInputProjection projection;
                if (!hlsl_natural_input_projection(program, condition,
                    demanded_lanes(ctx, index, 0), &projection))
                    return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            }
            continue;
        }
        const bool comparison = plan->natural_width && scalar_comparison_opcode(inst->opcode);
        if (plan->natural_width &&
            ((!natural_arithmetic_opcode(inst->opcode) && !comparison) ||
             !plain_instruction(inst)))
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        int predicate_if = -1;
        if (comparison) {
            /* A mask-valued comparison is represented as BOOL only after its
             * exact SSA writer is proved to have one direct control use. No
             * numeric mask, transport or predicate phi receives this type. */
            if (!hlsl_scalar_comparison_predicate_supported(ctx, index, &predicate_if))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        } else if (!(plan->natural_width ? hlsl_natural_float_instruction_supported(ctx, index)
                                        : hlsl_float4_instruction_supported(ctx, index)))
            return false;
        if (packed_inputs) for (int operand = 1; operand < inst->operand_count; ++operand) {
            if (inst->operands[operand].type != OPERAND_TYPE_INPUT) continue;
            HLSLNaturalInputProjection projection;
            if (!hlsl_natural_input_projection(program, &inst->operands[operand],
                demanded_lanes(ctx, index, operand), &projection))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        }
        const DXBCOperand *dest = &inst->operands[0];
        const int block = ctx->cfg.instruction_block[index];
        if (dest->type == OPERAND_TYPE_OUTPUT) {
            /* Until output SSA is modeled, require every output write to be
             * unconditional. No initialized placeholder can stand in for a
             * branch that leaves an original output lane undefined. */
            if (dest->register_index < 0 || dest->register_index >= HLSL_SM5_IO_REGISTER_COUNT ||
                !hlsl_cfg_dominates(&ctx->cfg, block, return_block))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            if (plan->natural_width) {
                const DXBCSignatureElement *field = NULL;
                for (int output = 0; output < program->output_count; ++output)
                    if (program->outputs[output].register_id == (uint32_t)dest->register_index)
                        field = &program->outputs[output];
                if (!field || outputs[dest->register_index] ||
                    usil_operand_destination_lane_mask(dest) != field->mask)
                    return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            }
            outputs[dest->register_index] = true;
            continue;
        }
        int value_index = plan->value_count++;
        StructuredValue *value = &plan->values[value_index];
        value->instruction = index;
        value->block = block;
        value->register_index = dest->register_index;
        value->mask = usil_operand_destination_lane_mask(dest);
        value->width = (uint8_t)mask_width(value->mask);
        value->predicate = comparison;
        value->predicate_if = predicate_if;
        snprintf(value->name, sizeof(value->name), "dxbc_value_i%d", index);
        plan->instruction_value[index] = value_index;
        for (int lane = 0; lane < 4; ++lane) {
            if (plan->natural_width && !(value->mask & (1u << lane))) continue;
            if (!map_variable(ctx, plan, operand_variable(ctx, index, 0, lane), value_index, lane))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        }
    }
    if (plan->natural_width && (natural_if_count != 1 || natural_else_count != 1 || natural_end_count != 1))
        return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    for (int index = 0; index < program->output_count; ++index)
        if (program->outputs[index].register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
            !outputs[program->outputs[index].register_id])
            return reject(ctx, -1, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    for (int block = 0; block < ctx->cfg.block_count; ++block) {
        const HLSLBlockPhis *phis = &ctx->ssa.block_phis[block];
        const int first_value = plan->value_count;
        for (int item = 0; item < phis->phi_count; ++item) {
            const HLSLPhiNode *phi = &phis->phis[item];
            if (plan->loop.instruction >= 0 && (phi->ssa_var == plan->loop.counter_phi ||
                                                phi->ssa_var == plan->loop.predicate_phi))
                continue;
            int value_index = first_value;
            while (value_index < plan->value_count &&
                   plan->values[value_index].register_index != phi->register_index)
                ++value_index;
            if (value_index == plan->value_count) {
                if (value_index == STRUCTURED_VALUE_LIMIT)
                    return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
                ++plan->value_count;
                StructuredValue *value = &plan->values[value_index];
                value->instruction = -1;
                value->block = block;
                value->register_index = phi->register_index;
                snprintf(value->name, sizeof(value->name), "dxbc_merge_i%d_r%d",
                         ctx->cfg.blocks[block].first_instruction, phi->register_index);
            }
            StructuredValue *value = &plan->values[value_index];
            if (phi->component < 0 || phi->component >= 4 || value->lanes[phi->component] ||
                !map_variable(ctx, plan, phi->ssa_var, value_index, phi->component))
                return reject(ctx, -1, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            value->lanes[phi->component] = phi;
            value->mask |= (uint8_t)(1u << phi->component);
            value->width = (uint8_t)mask_width(value->mask);
        }
    }
    /* Resolve only actually read phi vectors. Unpruned SSA can contain a dead
     * merge with an undefined arm; it must not invent a declaration or value. */
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *inst = &program->instructions[index];
        if (is_loop_control(&plan->loop, index))
            continue;
        const int first = inst->opcode == USIL_OP_IF ? 0 : 1;
        for (int operand = first; operand < inst->operand_count; ++operand)
            if (inst->operands[operand].type == OPERAND_TYPE_TEMP &&
                source_value(ctx, plan, index, operand, first ? 4 : 1) < 0)
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
    return true;
}

/* The entry interface is chosen before source emission. Borrow the already
 * prepared CFG/SSA through a separate diagnostic/builder context, so a
 * rejected new shape cannot poison the established emission route. No
 * borrowed analysis state is freed or modified by the planner. */
bool hlsl_natural_structured_preflight(HLSLEmitterContext *ctx) {
    if (!ctx || !ctx->program || !ctx->program->instructions ||
        ctx->program->instruction_count < 1 ||
        ctx->program->instruction_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        !ctx->cfg.blocks || !ctx->cfg.instruction_block ||
        !ctx->ssa.operand_ssa_vars || !ctx->ssa.block_phis)
        return false;
    HLSLEmitterContext *scratch = malloc(sizeof(*scratch));
    if (!scratch) return false;
    *scratch = *ctx;
    bool packed_inputs = false;
    if (hlsl_natural_input_layout_supported(ctx->program, &packed_inputs) && packed_inputs)
        scratch->high_level_interface = false; /* The pure plan must not depend on prepared names. */
    HLSLEmitDiagnostic diagnostic;
    hlsl_emit_diagnostic_init(&diagnostic);
    StringBuilder output;
    sb_init(&output);
    scratch->diagnostic = &diagnostic;
    scratch->sb = &output;
    StructuredPlan plan = {0};
    const bool supported = build_plan(scratch, &plan) && plan.natural_width;
    free(plan.ssa_value);
    free(plan.ssa_lane);
    sb_free(&output);
    free(scratch);
    return supported;
}

static ASTExpr *input_expression(HLSLEmitterContext *ctx, const DXBCOperand *source, int lanes) {
    if (lanes == 4)
        return hlsl_float4_source_atom(ctx, source);
    StringBuilder text;
    sb_init(&text);
    bool formatted = format_operand_hlsl_sb(ctx, source, false, false, 0x10, true, &text);
    ASTExpr *value = formatted && sb_ok(&text) ? ast_create_emitter_operand(text.buf) : NULL;
    sb_free(&text);
    return value;
}

static ASTExpr *planned_value_expression(HLSLEmitterContext *ctx,
    const StructuredPlan *plan, int value_index) {
    if (value_index < 0 || value_index >= plan->value_count) return NULL;
    const StructuredValue *planned = &plan->values[value_index];
    ASTExpr *value = ast_create_var(planned->instruction, planned->register_index,
        OPERAND_TYPE_TEMP, planned->name);
    if (!value) return NULL;
    if (planned->predicate) {
        if (planned->instruction < 0 || planned->instruction >= ctx->program->instruction_count ||
            planned->width != 1 || mask_width(planned->mask) != 1 ||
            planned->predicate_if < 0 || planned->predicate_if >= ctx->program->instruction_count ||
            !scalar_comparison_opcode(ctx->program->instructions[planned->instruction].opcode)) {
            ast_free_expr(value);
            return NULL;
        }
        ASTLogicalValueOrigin origin;
        ast_logical_value_origin_init(&origin);
        origin.complete = true;
        origin.scalar_type = AST_SCALAR_BOOL;
        origin.components = 1;
        origin.logical_value_id = (uint64_t)planned->instruction;
        origin.instruction_index = planned->instruction;
        origin.source_instruction_index = ctx->program->instructions[planned->instruction].source_instruction_index;
        origin.destination_lanes = planned->mask;
        if (!ast_set_logical_value_origin(value, &origin)) {
            ast_free_expr(value);
            return NULL;
        }
        return value;
    }
    if (planned->instruction >= 0)
        return hlsl_instruction_logical_expression(ctx, value, planned->instruction,
            planned->mask, planned->width);
    if (planned->resolution != 2 || !planned->mask || !planned->width) {
        ast_free_expr(value);
        return NULL;
    }
    /* The closed FLOAT32 operation/source contract and both complete incoming
     * tuples prove this value. Its identity is the planned phi, rather than
     * its physical register or an invented source instruction. */
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = AST_SCALAR_FLOAT32;
    origin.components = planned->width;
    origin.logical_value_id = STRUCTURED_PHI_LOGICAL_ID_BASE | (uint64_t)value_index;
    if (!ast_set_logical_value_origin(value, &origin)) {
        ast_free_expr(value);
        return NULL;
    }
    return value;
}

static bool structured_expression_span(void *context, const ASTExpr *expression,
    size_t begin, size_t end) {
    HLSLEmitterContext *ctx = context;
    return hlsl_stage_coverage_span(ctx->stage_coverage, expression, begin, end);
}

static void format_structured_expression(HLSLEmitterContext *ctx, const StructuredPlan *plan,
    const ASTExpr *expression) {
    if (plan->natural_width)
        ast_format_expr_traced(expression, ctx->sb, structured_expression_span, ctx);
    else
        ast_format_expr(expression, ctx->sb);
}

static const char *natural_type(unsigned width) {
    static const char *const types[] = {NULL, "float", "float2", "float3", "float4"};
    return width > 0 && width < sizeof(types) / sizeof(types[0]) ? types[width] : NULL;
}

static NaturalBodySyntax body_syntax(const HLSLEmitterContext *ctx,
    NaturalBodySyntaxKind kind, int instruction, int value, int incoming,
    int edge, uint8_t mask, uint8_t width) {
    NaturalBodySyntax result = {0};
    result.kind = kind;
    result.instruction = instruction;
    result.source_instruction = ctx->program->instructions[instruction].source_instruction_index;
    result.value = value;
    result.incoming = incoming;
    result.edge = edge;
    result.mask = mask;
    result.width = width;
    result.scalar_type = kind == NATURAL_BODY_TEMP_ASSIGNMENT &&
        scalar_comparison_opcode(ctx->program->instructions[instruction].opcode)
        ? AST_SCALAR_BOOL : AST_SCALAR_FLOAT32;
    return result;
}

static bool body_syntax_owners_equal(const NaturalBodySyntax *a, const NaturalBodySyntax *b) {
    return a->kind == b->kind && a->instruction == b->instruction &&
        a->source_instruction == b->source_instruction && a->value == b->value &&
        a->incoming == b->incoming && a->edge == b->edge &&
        a->mask == b->mask && a->width == b->width && a->scalar_type == b->scalar_type;
}

static bool body_expect(HLSLNaturalStructuredBodyInventory *inventory, NaturalBodySyntax syntax) {
    if (inventory->expected_count >= (size_t)NATURAL_BODY_RECEIPT_LIMIT) return false;
    inventory->expected[inventory->expected_count++] = syntax;
    return true;
}

static int phi_predecessor_edge(const HLSLEmitterContext *ctx, int join, int predecessor) {
    const HLSLBasicBlock *block = &ctx->cfg.blocks[join];
    for (int edge = 0; edge < block->predecessor_count; ++edge)
        if (block->predecessors[edge] == predecessor) return edge;
    return -1;
}

static bool body_expect_phi(HLSLEmitterContext *ctx, const StructuredPlan *plan,
    HLSLNaturalStructuredBodyInventory *inventory, int instruction, int join, int edge) {
    if (edge < -1 || edge > 1) return false;
    for (int index = 0; index < plan->value_count; ++index) {
        const StructuredValue *value = &plan->values[index];
        if (value->instruction >= 0 || value->block != join || value->resolution != 2) continue;
        if (!value->mask || value->width != mask_width(value->mask) ||
            !body_expect(inventory, body_syntax(ctx,
                edge < 0 ? NATURAL_BODY_PHI_DECLARATION : NATURAL_BODY_PHI_EDGE,
                instruction, index, edge < 0 ? -1 : value->incoming[edge], edge,
                value->mask, value->width))) return false;
    }
    return true;
}

/* This enumeration uses only proved CFG/SSA owners. It neither observes the
 * output builder nor shares the emitter's append counters/ranges. */
static bool body_expected_inventory(HLSLEmitterContext *ctx, const StructuredPlan *plan,
    HLSLNaturalStructuredBodyInventory *inventory) {
    if (!plan->natural_width || inventory->expected_count || inventory->emitted_count) return false;
    for (int index = 0; index < ctx->program->instruction_count; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        if (instruction->opcode == USIL_OP_NOP) continue;
        NaturalBodySyntaxKind kind;
        int value = -1;
        uint8_t mask = 0, width = 0;
        if (instruction->opcode == USIL_OP_IF) {
            if (!body_expect_phi(ctx, plan, inventory, index, plan->regions[index].join_block, -1))
                return false;
            kind = NATURAL_BODY_IF;
        } else if (instruction->opcode == USIL_OP_ELSE || instruction->opcode == USIL_OP_ENDIF) {
            const HLSLInstructionFlow *flow = &ctx->cfg.instruction_flow[index];
            const int header = instruction->opcode == USIL_OP_ENDIF ? flow->jump_scope
                : ctx->cfg.instruction_flow[flow->end].jump_scope;
            const int join = plan->regions[header].join_block;
            const int edge = phi_predecessor_edge(ctx, join, ctx->cfg.instruction_block[index]);
            if (edge < 0 || !body_expect_phi(ctx, plan, inventory, index, join, edge)) return false;
            kind = instruction->opcode == USIL_OP_ELSE ? NATURAL_BODY_ELSE : NATURAL_BODY_ENDIF;
        } else if (instruction->opcode == USIL_OP_RET) {
            kind = NATURAL_BODY_RETURN;
        } else {
            const DXBCOperand *destination = &instruction->operands[0];
            kind = destination->type == OPERAND_TYPE_TEMP ? NATURAL_BODY_TEMP_ASSIGNMENT
                : NATURAL_BODY_OUTPUT_ASSIGNMENT;
            value = plan->instruction_value[index];
            mask = usil_operand_destination_lane_mask(destination);
            width = (uint8_t)mask_width(mask);
        }
        if (!body_expect(inventory, body_syntax(ctx, kind, index, value, -1, -1, mask, width)))
            return false;
    }
    return inventory->expected_count > 0 &&
        inventory->expected[inventory->expected_count - 1].kind == NATURAL_BODY_RETURN;
}

static bool body_record(HLSLNaturalStructuredBodyInventory *inventory,
    NaturalBodySyntax syntax, size_t begin, size_t end, size_t expression_begin,
    size_t expression_end) {
    if (!inventory) return true; /* Established FLOAT4/loop spelling. */
    if (inventory->sealed || inventory->emitted_count >= inventory->expected_count ||
        !body_syntax_owners_equal(&syntax, &inventory->expected[inventory->emitted_count]) ||
        begin >= end || end < inventory->source_begin ||
        end - inventory->source_begin > (size_t)NATURAL_BODY_BYTE_LIMIT ||
        begin != (inventory->emitted_count
            ? inventory->emitted[inventory->emitted_count - 1].end : inventory->source_begin) ||
        ((expression_begin || expression_end) &&
            (expression_begin < begin || expression_begin >= expression_end || expression_end > end)))
        return false;
    syntax.begin = begin;
    syntax.end = end;
    syntax.expression_begin = expression_begin;
    syntax.expression_end = expression_end;
    inventory->emitted[inventory->emitted_count++] = syntax;
    return true;
}

static bool body_map_equal(const HLSLExpressionSourceMap *a, const HLSLExpressionSourceMap *b) {
    if (!a || !b || a->count != b->count || a->complete != b->complete) return false;
    for (size_t index = 0; index < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++index)
        if (!hlsl_expression_origins_equal(&a->origins[index], &b->origins[index])) return false;
    return true;
}

static ASTExpr *source_expression(HLSLEmitterContext *ctx, StructuredPlan *plan, int index,
                                  int operand, int lanes) {
    const DXBCOperand *source = &ctx->program->instructions[index].operands[operand];
    if (source->type != OPERAND_TYPE_TEMP) {
        if (plan->natural_width)
            return hlsl_natural_source_atom(ctx, index, operand, demanded_lanes(ctx, index, operand));
        return input_expression(ctx, source, lanes);
    }
    int value_index = source_value(ctx, plan, index, operand, lanes);
    if (value_index < 0)
        return NULL;
    if (plan->natural_width) {
        ASTExpr *value = planned_value_expression(ctx, plan, value_index);
        if (plan->values[value_index].predicate) return value;
        return hlsl_project_logical_temp(ctx, value, plan->values[value_index].mask,
            plan->values[value_index].width, source, demanded_lanes(ctx, index, operand), index);
    }
    ASTExpr *value = ast_create_var(-1, source->register_index, OPERAND_TYPE_TEMP,
                                    plan->values[value_index].name);
    int components[4];
    bool identity = lanes == 4;
    for (int lane = 0; lane < lanes; ++lane) {
        components[lane] = usil_operand_source_component(source, lane);
        identity = identity && components[lane] == lane;
    }
    if (identity)
        return value;
    ASTExpr *selected = ast_create_swizzle(value, components, lanes);
    if (!selected)
        ast_free_expr(value);
    return selected;
}

static bool emit_phi_edge(HLSLEmitterContext *ctx, const StructuredPlan *plan, int join,
                          int predecessor, HLSLNaturalStructuredBodyInventory *inventory) {
    const HLSLBasicBlock *block = &ctx->cfg.blocks[join];
    int edge = 0;
    while (edge < block->predecessor_count && block->predecessors[edge] != predecessor)
        ++edge;
    if (edge == block->predecessor_count)
        return false;
    for (int index = 0; index < plan->value_count; ++index) {
        const StructuredValue *value = &plan->values[index];
        if (value->instruction >= 0 || value->block != join || value->resolution != 2)
            continue;
        if (edge >= 2)
            return false;
        const size_t statement_begin = ctx->sb->len;
        sb_append_spaces(ctx->sb, ctx->indent);
        if (plan->natural_width) {
            const int instruction = ctx->current_instruction_index;
            ASTExpr *incoming = planned_value_expression(ctx, plan, value->incoming[edge]);
            if (!incoming || !hlsl_source_quality_observe_expression(ctx, incoming, instruction)) {
                ast_free_expr(incoming);
                return false;
            }
            sb_appendf(ctx->sb, "%s = ", value->name);
            const size_t expression_begin = ctx->sb->len;
            format_structured_expression(ctx, plan, incoming);
            const size_t expression_end = ctx->sb->len;
            ast_free_expr(incoming);
            sb_append(ctx->sb, ";\n");
            hlsl_source_quality_emission(ctx, 0, false, instruction);
            if (!natural_model_stable(ctx) || !sb_ok(ctx->sb) ||
                !body_record(inventory, body_syntax(ctx, NATURAL_BODY_PHI_EDGE,
                    instruction, index, value->incoming[edge], edge, value->mask, value->width),
                    statement_begin, ctx->sb->len, expression_begin, expression_end)) return false;
            continue;
        }
        sb_appendf(ctx->sb, "%s = %s;\n", value->name, plan->values[value->incoming[edge]].name);
    }
    return sb_ok(ctx->sb);
}

static bool emit_counted_loop_begin(HLSLEmitterContext *ctx, StructuredPlan *plan) {
    const CountedLoop *loop = &plan->loop;
    for (int item = 0; item < plan->value_count; ++item) {
        const StructuredValue *value = &plan->values[item];
        if (value->instruction < 0 && value->block == loop->body_block && value->resolution == 2) {
            sb_append_spaces(ctx->sb, ctx->indent);
            sb_appendf(ctx->sb, "float4 %s = %s;\n", value->name,
                       plan->values[value->incoming[loop->initial_edge]].name);
        }
    }
    ctx->current_instruction_index = loop->comparison;
    const USILInstruction *comparison = &ctx->program->instructions[loop->comparison];
    DXBCOperand bound_operand = comparison->operands[2];
    bound_operand.swizzle[0] = (uint8_t)usil_operand_source_component(
        &comparison->operands[2], destination_component(&comparison->operands[0]));
    bound_operand.swizzle_mode = 2;
    ASTExpr *bound = input_expression(ctx, &bound_operand, 1);
    ctx->current_instruction_index = loop->instruction;
    ASTExpr *bits = ast_create_bitcast(AST_SCALAR_UINT32, bound);
    if (!bits) {
        ast_free_expr(bound);
        return false;
    }
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb, "[loop] for (uint dxbc_index_i%d = dxbc_initial_i%d; dxbc_index_i%d < ",
               loop->instruction, loop->initialization, loop->instruction);
    if (!hlsl_source_quality_observe_expression(ctx, bits, loop->comparison)) {
        ast_free_expr(bits);
        return false;
    }
    ast_format_expr(bits, ctx->sb);
    ast_free_expr(bits);
    sb_appendf(ctx->sb, "; ++dxbc_index_i%d) {\n", loop->instruction);
    ctx->indent += 4;
    return sb_ok(ctx->sb);
}

static bool emit_structured_plan(HLSLEmitterContext *ctx, StructuredPlan *plan,
    HLSLNaturalStructuredBodyInventory *inventory) {
    bool success = false;
    const int initial_indent = ctx->indent;
    hlsl_expression_source_map_begin(ctx);
    for (int index = 0; index < ctx->program->instruction_count; ++index) {
        if (!natural_model_stable(ctx)) goto cleanup;
        const USILInstruction *inst = &ctx->program->instructions[index];
        ctx->current_instruction_index = index;
        if (inst->opcode == USIL_OP_NOP || inst->opcode == USIL_OP_RET)
            continue;
        size_t begin = ctx->sb->len, end = begin;
        const CountedLoop *loop = &plan->loop;
        if (is_loop_control(loop, index)) {
            if (index == loop->comparison || index == loop->test || index == loop->increment)
                continue; /* Their operation is represented in the for header. */
            if (index == loop->initialization) {
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_appendf(ctx->sb, "const uint dxbc_initial_i%d = %" PRIu32 "u;\n", index,
                           loop->initial_bits);
            } else if (index == loop->instruction) {
                if (!emit_counted_loop_begin(ctx, plan))
                    goto cleanup;
                if (ctx->expression_source_map) {
                    const int folded[] = {loop->comparison, loop->test, loop->increment};
                    for (size_t item = 0; item < sizeof(folded) / sizeof(folded[0]); ++item) {
                        HLSLExpressionOrigin *origin =
                            &ctx->expression_source_map->origins[folded[item]];
                        origin->kind = HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL;
                        origin->source_begin = begin;
                        origin->source_end = ctx->sb->len;
                    }
                }
            } else {
                if (!emit_phi_edge(ctx, plan, loop->body_block, loop->latch_block, inventory))
                    goto cleanup;
                ctx->indent -= 4;
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_append(ctx->sb, "}\n");
            }
        } else if (inst->opcode == USIL_OP_IF) {
            const HLSLIfRegion *region = &plan->regions[index];
            for (int item = 0; item < plan->value_count; ++item) {
                const StructuredValue *value = &plan->values[item];
                if (value->instruction < 0 && value->block == region->join_block &&
                    value->resolution == 2) {
                    const size_t declaration_begin = ctx->sb->len;
                    sb_append_spaces(ctx->sb, ctx->indent);
                    sb_appendf(ctx->sb, "%s %s;\n",
                        plan->natural_width ? natural_type(value->width) : "float4", value->name);
                    if (plan->natural_width) hlsl_source_quality_emission(ctx, 0, false, index);
                    if (!natural_model_stable(ctx) ||
                        !body_record(inventory, body_syntax(ctx, NATURAL_BODY_PHI_DECLARATION,
                            index, item, -1, -1, value->mask, value->width),
                            declaration_begin, ctx->sb->len, 0, 0)) goto cleanup;
                }
            }
            const size_t control_begin = ctx->sb->len;
            bool predicate = false;
            if (plan->natural_width && inst->operands[0].type == OPERAND_TYPE_TEMP) {
                const int value = source_value(ctx, plan, index, 0, 1);
                if (value < 0) goto cleanup;
                predicate = plan->values[value].predicate;
            }
            ASTExpr *condition = source_expression(ctx, plan, index, 0, 1);
            /* IF_Z negates the BOOL itself. Replacing LT with GE would change
             * unordered float/NaN behavior. Raw float
             * conditions retain the existing bit-test spelling. */
            ASTExpr *bits = predicate ? condition : ast_create_bitcast(AST_SCALAR_UINT32, condition);
            if (!bits) {
                ast_free_expr(condition);
                goto cleanup;
            }
            if (plan->natural_width && !predicate) {
                ASTLogicalValueOrigin origin;
                ast_logical_value_origin_init(&origin);
                origin.complete = true;
                origin.scalar_type = AST_SCALAR_UINT32;
                origin.components = 1;
                origin.logical_value_id = (uint64_t)index;
                origin.instruction_index = index;
                origin.source_instruction_index = inst->source_instruction_index;
                origin.program_bitcast = true;
                if (!ast_set_logical_value_origin(bits, &origin)) {
                    ast_free_expr(bits);
                    goto cleanup;
                }
            }
            sb_append_spaces(ctx->sb, ctx->indent);
            sb_append(ctx->sb, inst->condition_test == DXBC_INSTRUCTION_TEST_NONZERO
                                   ? "[branch] if ("
                                   : "[branch] if (!");
            if (!hlsl_source_quality_observe_expression(ctx, bits, index)) {
                ast_free_expr(bits);
                goto cleanup;
            }
            if (!natural_model_stable(ctx)) {
                ast_free_expr(bits);
                goto cleanup;
            }
            const size_t expression_begin = ctx->sb->len;
            format_structured_expression(ctx, plan, bits);
            const size_t expression_end = ctx->sb->len;
            ast_free_expr(bits);
            sb_append(ctx->sb, ") {\n");
            if (plan->natural_width) hlsl_source_quality_emission(ctx, 0, false, index);
            if (!natural_model_stable(ctx) ||
                !body_record(inventory, body_syntax(ctx, NATURAL_BODY_IF,
                    index, -1, -1, -1, 0, 0), control_begin, ctx->sb->len,
                    expression_begin, expression_end)) goto cleanup;
            ctx->indent += 4;
        } else if (inst->opcode == USIL_OP_ELSE || inst->opcode == USIL_OP_ENDIF) {
            const HLSLInstructionFlow *flow = &ctx->cfg.instruction_flow[index];
            const int header = inst->opcode == USIL_OP_ENDIF
                                   ? flow->jump_scope
                                   : ctx->cfg.instruction_flow[flow->end].jump_scope;
            const HLSLIfRegion *region = &plan->regions[header];
            if (!emit_phi_edge(ctx, plan, region->join_block, ctx->cfg.instruction_block[index], inventory))
                goto cleanup;
            const size_t control_begin = ctx->sb->len;
            ctx->indent -= 4;
            sb_append_spaces(ctx->sb, ctx->indent);
            if (inst->opcode == USIL_OP_ELSE) {
                sb_append(ctx->sb, "} else {\n");
                ctx->indent += 4;
            } else if (region->else_instruction < 0) {
                sb_append(ctx->sb, "} else {\n");
                ctx->indent += 4;
                if (!emit_phi_edge(ctx, plan, region->join_block, region->header_block, inventory))
                    goto cleanup;
                ctx->indent -= 4;
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_append(ctx->sb, "}\n");
            } else {
                sb_append(ctx->sb, "}\n");
            }
            if (plan->natural_width) hlsl_source_quality_emission(ctx, 0, false, index);
            if (!natural_model_stable(ctx) ||
                !body_record(inventory, body_syntax(ctx,
                    inst->opcode == USIL_OP_ELSE ? NATURAL_BODY_ELSE : NATURAL_BODY_ENDIF,
                    index, -1, -1, -1, 0, 0), control_begin, ctx->sb->len, 0, 0)) goto cleanup;
        } else {
            const size_t statement_begin = ctx->sb->len;
            ASTExpr *left = source_expression(ctx, plan, index, 1, 4);
            ASTExpr *right =
                inst->opcode == USIL_OP_MOV ? NULL : source_expression(ctx, plan, index, 2, 4);
            const bool comparison = plan->natural_width && scalar_comparison_opcode(inst->opcode);
            ASTExpr *expression = comparison
                ? hlsl_scalar_comparison_expression(ctx, index, left, right)
                : plan->natural_width ? hlsl_natural_float_binary_operation(ctx, index, left, right)
                    : hlsl_float4_operation(ctx, index, left, right);
            if (!expression)
                goto cleanup;
            sb_append_spaces(ctx->sb, ctx->indent);
            const DXBCOperand *destination = &inst->operands[0];
            if (destination->type == OPERAND_TYPE_TEMP) {
                sb_appendf(ctx->sb, "const %s %s",
                           comparison ? "bool" : plan->natural_width
                               ? natural_type(plan->values[plan->instruction_value[index]].width) : "float4",
                           plan->values[plan->instruction_value[index]].name);
            } else if (!hlsl_float4_append_output(ctx, destination)) {
                ast_free_expr(expression);
                goto cleanup;
            }
            sb_append(ctx->sb, " = ");
            begin = ctx->sb->len;
            if (!hlsl_source_quality_observe_expression(ctx, expression, index)) {
                ast_free_expr(expression);
                goto cleanup;
            }
            if (!natural_model_stable(ctx)) {
                ast_free_expr(expression);
                goto cleanup;
            }
            format_structured_expression(ctx, plan, expression);
            end = ctx->sb->len;
            ast_free_expr(expression);
            sb_append(ctx->sb, ";\n");
            if (plan->natural_width) hlsl_source_quality_emission(ctx,
                destination->type == OPERAND_TYPE_OUTPUT && !ctx->high_level_interface
                    ? HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE : 0, false, index);
            if (!natural_model_stable(ctx) ||
                !body_record(inventory, body_syntax(ctx,
                    destination->type == OPERAND_TYPE_TEMP ? NATURAL_BODY_TEMP_ASSIGNMENT
                        : NATURAL_BODY_OUTPUT_ASSIGNMENT,
                    index, plan->instruction_value[index], -1, -1,
                    usil_operand_destination_lane_mask(destination),
                    (uint8_t)mask_width(usil_operand_destination_lane_mask(destination))),
                    statement_begin, ctx->sb->len, begin, end)) goto cleanup;
        }
        if (ctx->expression_source_map) {
            HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[index];
            origin->kind = inst->opcode == USIL_OP_IF || inst->opcode == USIL_OP_ELSE ||
                                   inst->opcode == USIL_OP_ENDIF
                               ? HLSL_EXPRESSION_ORIGIN_CONTROL
                               : HLSL_EXPRESSION_ORIGIN_EXPRESSION;
            if (is_loop_control(loop, index))
                origin->kind = HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL;
            origin->source_begin = begin;
            origin->source_end = end > begin ? end : ctx->sb->len;
        }
        if (!sb_ok(ctx->sb))
            goto cleanup;
    }
    success = ctx->indent == initial_indent && sb_ok(ctx->sb);
cleanup:
    ctx->indent = initial_indent;
    return success;
}

static bool packed_header_ends_at(const HLSLEmitterContext *ctx, size_t offset) {
    return !ctx->high_level_packed_inputs ||
        (hlsl_natural_packed_header_matches(ctx) && offset >= ctx->natural_packed_header_begin &&
         ctx->natural_packed_header_source.len == offset - ctx->natural_packed_header_begin);
}

bool emit_high_level_structured(HLSLEmitterContext *ctx) {
    StructuredPlan plan = {0};
    HLSLNaturalStructuredBodyInventory *inventory = NULL;
    bool success = false;
    if (!natural_model_stable(ctx) || !packed_header_ends_at(ctx, ctx->sb->len) ||
        !build_plan(ctx, &plan)) goto cleanup;
    if (plan.natural_width) {
        if (!ctx->natural_structured_owners_guarded || ctx->natural_structured_body_inventory)
            goto cleanup;
        inventory = calloc(1, sizeof(*inventory));
        if (!inventory) goto cleanup;
        inventory->source_begin = ctx->sb->len;
        inventory->indent = ctx->indent;
        inventory->caller_map = ctx->expression_source_map;
        common_sha256(ctx->sb->buf, inventory->source_begin, inventory->prefix_digest);
        if (!body_expected_inventory(ctx, &plan, inventory)) goto cleanup;
        ctx->natural_structured_body_inventory = inventory;
        if (!ctx->expression_source_map) ctx->expression_source_map = &inventory->internal_map;
    }
    if (!emit_structured_plan(ctx, &plan, inventory)) goto cleanup;
    if (inventory) {
        inventory->body_end = ctx->sb->len;
        const size_t length = inventory->body_end - inventory->source_begin;
        if (!length || length > (size_t)NATURAL_BODY_BYTE_LIMIT ||
            inventory->emitted_count + 1u != inventory->expected_count) goto cleanup;
        inventory->source = malloc(length);
        if (!inventory->source) goto cleanup;
        memcpy(inventory->source, ctx->sb->buf + inventory->source_begin, length);
        inventory->map = *ctx->expression_source_map;
    }
    success = sb_ok(ctx->sb);
cleanup:
    free(plan.ssa_value);
    free(plan.ssa_lane);
    if (!success && ctx->natural_structured_owners_guarded) {
        /* The new route must never return a partly owned body. Model drift is
         * detected before borrowing analysis or reading the next instruction. */
        natural_model_stable(ctx);
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
            HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
    if (inventory && ctx->natural_structured_body_inventory != inventory) free(inventory);
    return success;
}

static bool body_prefix_matches(const HLSLEmitterContext *ctx,
    const HLSLNaturalStructuredBodyInventory *inventory) {
    if (!ctx->sb || !sb_ok(ctx->sb) || !ctx->sb->buf || ctx->sb->len < inventory->source_begin)
        return false;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(ctx->sb->buf, inventory->source_begin, digest);
    return !memcmp(digest, inventory->prefix_digest, sizeof(digest));
}

static bool body_receipts_equal(const HLSLNaturalStructuredBodyInventory *a,
    const HLSLNaturalStructuredBodyInventory *b, size_t offset) {
    if (a->expected_count != b->expected_count || a->emitted_count != a->expected_count ||
        b->emitted_count != b->expected_count) return false;
    for (size_t index = 0; index < a->expected_count; ++index) {
        const NaturalBodySyntax *left = &a->emitted[index], *right = &b->emitted[index];
        if (!body_syntax_owners_equal(&a->expected[index], &b->expected[index]) ||
            !body_syntax_owners_equal(left, right) ||
            left->begin != right->begin + offset || left->end != right->end + offset ||
            left->expression_begin != (right->expression_begin ? right->expression_begin + offset : 0) ||
            left->expression_end != (right->expression_end ? right->expression_end + offset : 0))
            return false;
    }
    return true;
}

/* Complete the same ordinary return path and its final RET raw map owner.
 * The outer emitter uses these exact offsets; no source token is searched. */
static bool body_record_return(HLSLEmitterContext *ctx,
    HLSLNaturalStructuredBodyInventory *inventory, size_t begin) {
    const int instruction = ctx->program->instruction_count - 1;
    if (!ctx->expression_source_map || !ctx->expression_source_map->count ||
        ctx->program->instructions[instruction].opcode != USIL_OP_RET ||
        !body_record(inventory, body_syntax(ctx, NATURAL_BODY_RETURN,
            instruction, -1, -1, -1, 0, 0), begin, ctx->sb->len, 0, 0)) return false;
    HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[instruction];
    origin->source_begin = begin;
    origin->source_end = ctx->sb->len;
    ctx->expression_source_map->complete = sb_ok(ctx->sb);
    return true;
}

static bool body_replay(HLSLEmitterContext *ctx,
    const HLSLNaturalStructuredBodyInventory *inventory) {
    HLSLEmitterContext *scratch = malloc(sizeof(*scratch));
    HLSLNaturalStructuredBodyInventory *replay = calloc(1, sizeof(*replay));
    if (!scratch || !replay) {
        free(scratch);
        free(replay);
        return false;
    }
    *scratch = *ctx;
    HLSLEmitDiagnostic diagnostic;
    hlsl_emit_diagnostic_init(&diagnostic);
    StringBuilder output;
    sb_init(&output);
    scratch->diagnostic = &diagnostic;
    scratch->sb = &output;
    scratch->indent = inventory->indent;
    scratch->expression_source_map = &replay->internal_map;
    scratch->natural_structured_body_inventory = NULL;
    scratch->natural_structured_owners_guarded = false;
    scratch->source_quality_analysis = NULL;
    scratch->source_quality_root = NULL;
    scratch->source_quality_forward_observer = NULL;
    scratch->source_quality_forward_observer_context = NULL;
    scratch->matrix_use_capture = NULL;
    scratch->stage_coverage = NULL;
    StructuredPlan plan = {0};
    bool success = build_plan(scratch, &plan) && plan.natural_width &&
        body_expected_inventory(scratch, &plan, replay) &&
        emit_structured_plan(scratch, &plan, replay);
    if (success) {
        replay->body_end = output.len;
        emit_return_block(scratch);
        success = sb_ok(&output) && body_record_return(scratch, replay, replay->body_end) &&
            output.len == inventory->source_end - inventory->source_begin &&
            replay->body_end == inventory->body_end - inventory->source_begin &&
            !memcmp(output.buf, inventory->source, output.len) &&
            body_receipts_equal(inventory, replay, inventory->source_begin) &&
            hlsl_expression_source_map_offset(scratch->expression_source_map, inventory->source_begin) &&
            body_map_equal(&inventory->map, scratch->expression_source_map);
    }
    free(plan.ssa_value);
    free(plan.ssa_lane);
    sb_free(&output);
    free(replay);
    free(scratch);
    return success;
}

bool hlsl_natural_structured_body_inventory_complete(HLSLEmitterContext *ctx) {
    if (!ctx || !ctx->natural_structured_owners_guarded || !natural_model_stable(ctx) ||
        !hlsl_natural_packed_header_matches(ctx)) return false;
    HLSLNaturalStructuredBodyInventory *inventory = ctx->natural_structured_body_inventory;
    if (!inventory || !inventory->source || !ctx->expression_source_map ||
        !body_prefix_matches(ctx, inventory) || !packed_header_ends_at(ctx, inventory->source_begin) ||
        inventory->body_end < inventory->source_begin ||
        ctx->sb->len <= inventory->body_end ||
        ctx->sb->len - inventory->source_begin > (size_t)NATURAL_BODY_BYTE_LIMIT) return false;
    if (inventory->sealed) return hlsl_natural_structured_body_inventory_matches(ctx);
    const size_t body_length = inventory->body_end - inventory->source_begin;
    if (memcmp(inventory->source, ctx->sb->buf + inventory->source_begin, body_length) ||
        inventory->emitted_count + 1u != inventory->expected_count) return false;
    /* The body's map is held before return callbacks. Only the final RET
     * range/complete flag is legitimately added by the outer return path. */
    for (size_t index = 0; index + 1u < inventory->map.count; ++index)
        if (!hlsl_expression_origins_equal(&inventory->map.origins[index],
            &ctx->expression_source_map->origins[index])) return false;
    const size_t length = ctx->sb->len - inventory->source_begin;
    char *source = realloc(inventory->source, length);
    if (!source) return false;
    inventory->source = source;
    memcpy(source + body_length, ctx->sb->buf + inventory->body_end, length - body_length);
    inventory->source_end = ctx->sb->len;
    /* Do not repair the caller's map: the outer path already appended this
     * RET owner, and an observer changing it must fail independent replay. */
    const int instruction = ctx->program->instruction_count - 1;
    if (!body_record(inventory, body_syntax(ctx, NATURAL_BODY_RETURN,
        instruction, -1, -1, -1, 0, 0), inventory->body_end, inventory->source_end, 0, 0)) return false;
    inventory->map = *ctx->expression_source_map;
    if (!inventory->map.complete || !hlsl_expression_source_map_matches(&inventory->map,
        ctx->program, ctx->sb->buf) || !body_replay(ctx, inventory)) return false;
    inventory->sealed = true;
    return true;
}

bool hlsl_natural_structured_body_inventory_matches(HLSLEmitterContext *ctx) {
    const HLSLNaturalStructuredBodyInventory *inventory = ctx ? ctx->natural_structured_body_inventory : NULL;
    return inventory && inventory->sealed && inventory->source &&
        inventory->source_end > inventory->source_begin &&
        ctx->sb->len == inventory->source_end && body_prefix_matches(ctx, inventory) &&
        !memcmp(ctx->sb->buf + inventory->source_begin, inventory->source,
            inventory->source_end - inventory->source_begin) &&
        body_map_equal(ctx->expression_source_map, &inventory->map) && natural_model_stable(ctx) &&
        packed_header_ends_at(ctx, inventory->source_begin);
}

void hlsl_natural_structured_body_inventory_dispose(HLSLEmitterContext *ctx) {
    if (!ctx) return;
    sb_free(&ctx->natural_packed_header_source);
    ctx->natural_packed_header_begin = 0;
    ctx->natural_packed_header_replay = false;
    if (!ctx->natural_structured_body_inventory) return;
    HLSLNaturalStructuredBodyInventory *inventory = ctx->natural_structured_body_inventory;
    if (ctx->expression_source_map == &inventory->internal_map)
        ctx->expression_source_map = inventory->caller_map;
    free(inventory->source);
    free(inventory);
    ctx->natural_structured_body_inventory = NULL;
}
