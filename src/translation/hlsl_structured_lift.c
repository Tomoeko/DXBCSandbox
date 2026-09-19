// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reuse the full-vector expression contract, CFG and lane SSA. This pass
 * gives each instruction result an immutable value and each live phi one
 * explicitly assigned float4. It does not speculate expressions across an
 * arm, duplicate work, or assume an undefined incoming lane has a value. */
enum { STRUCTURED_VALUE_LIMIT = HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT * 2 };

typedef struct {
    int instruction; /* -1 for a phi vector. */
    int block;
    int register_index;
    const HLSLPhiNode *lanes[4];
    int incoming[2];
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
} StructuredPlan;

static bool reject(HLSLEmitterContext *ctx, int instruction, HLSLEmitReason reason) {
    hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, reason, instruction, -1);
    return false;
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
                const HLSLPhiNode *phi = value->lanes[lane];
                if (!phi || phi->incoming_blocks[edge] != block->predecessors[edge])
                    return false;
                int variable = phi->incoming_vars[edge];
                if (variable < 0 || variable >= ctx->ssa.ssa_var_count ||
                    plan->ssa_lane[variable] != lane || plan->ssa_value[variable] < 0)
                    return false;
                int source = plan->ssa_value[variable];
                if ((lane && incoming != source) ||
                    !hlsl_cfg_dominates(&ctx->cfg, plan->values[source].block,
                                        block->predecessors[edge]))
                    return false;
                incoming = source;
            }
            if (!resolve_value(ctx, plan, incoming, depth + 1))
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
    for (int lane = 0; lane < lanes; ++lane) {
        int variable = operand_variable(ctx, instruction, operand, lane);
        if (variable < 0 || variable >= ctx->ssa.ssa_var_count || plan->ssa_value[variable] < 0 ||
            plan->ssa_lane[variable] != usil_operand_source_component(source, lane))
            return -1;
        int value = plan->ssa_value[variable];
        if ((lane && result != value) ||
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
    if (!hlsl_float4_program_supported(ctx))
        return false;
    const USILProgram *program = ctx->program;
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
            inst->opcode == USIL_OP_ENDIF)
            continue;
        if (inst->opcode == USIL_OP_IF) {
            if (plan->loop.instruction >= 0)
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            if (!hlsl_cfg_if_region(ctx, index, &plan->regions[index]))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            int join = plan->regions[index].join_block;
            if (plan->join_if[join] >= 0)
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            plan->join_if[join] = index;
            const DXBCOperand *condition = &inst->operands[0];
            if ((condition->type != OPERAND_TYPE_INPUT && condition->type != OPERAND_TYPE_TEMP) ||
                !hlsl_lift_operand_is_plain(condition))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            continue;
        }
        if (!hlsl_float4_instruction_supported(ctx, index))
            return false;
        const DXBCOperand *dest = &inst->operands[0];
        const int block = ctx->cfg.instruction_block[index];
        if (dest->type == OPERAND_TYPE_OUTPUT) {
            /* Until output SSA is modeled, require every output write to be
             * unconditional. No initialized placeholder can stand in for a
             * branch that leaves an original output lane undefined. */
            if (dest->register_index < 0 || dest->register_index >= HLSL_SM5_IO_REGISTER_COUNT ||
                !hlsl_cfg_dominates(&ctx->cfg, block, return_block))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            outputs[dest->register_index] = true;
            continue;
        }
        int value_index = plan->value_count++;
        StructuredValue *value = &plan->values[value_index];
        value->instruction = index;
        value->block = block;
        value->register_index = dest->register_index;
        snprintf(value->name, sizeof(value->name), "dxbc_value_i%d", index);
        plan->instruction_value[index] = value_index;
        for (int lane = 0; lane < 4; ++lane)
            if (!map_variable(ctx, plan, operand_variable(ctx, index, 0, lane), value_index, lane))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
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

static ASTExpr *source_expression(HLSLEmitterContext *ctx, StructuredPlan *plan, int index,
                                  int operand, int lanes) {
    const DXBCOperand *source = &ctx->program->instructions[index].operands[operand];
    if (source->type != OPERAND_TYPE_TEMP)
        return input_expression(ctx, source, lanes);
    int value_index = source_value(ctx, plan, index, operand, lanes);
    if (value_index < 0)
        return NULL;
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
                          int predecessor) {
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
        sb_append_spaces(ctx->sb, ctx->indent);
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
    ast_format_expr(bits, ctx->sb);
    ast_free_expr(bits);
    sb_appendf(ctx->sb, "; ++dxbc_index_i%d) {\n", loop->instruction);
    ctx->indent += 4;
    return sb_ok(ctx->sb);
}

bool emit_high_level_structured(HLSLEmitterContext *ctx) {
    StructuredPlan plan = {0};
    bool success = false;
    const int initial_indent = ctx->indent;
    if (!build_plan(ctx, &plan))
        goto cleanup;
    hlsl_expression_source_map_begin(ctx);
    for (int index = 0; index < ctx->program->instruction_count; ++index) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        ctx->current_instruction_index = index;
        if (inst->opcode == USIL_OP_NOP || inst->opcode == USIL_OP_RET)
            continue;
        size_t begin = ctx->sb->len, end = begin;
        const CountedLoop *loop = &plan.loop;
        if (is_loop_control(loop, index)) {
            if (index == loop->comparison || index == loop->test || index == loop->increment)
                continue; /* Their operation is represented in the for header. */
            if (index == loop->initialization) {
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_appendf(ctx->sb, "const uint dxbc_initial_i%d = %" PRIu32 "u;\n", index,
                           loop->initial_bits);
            } else if (index == loop->instruction) {
                if (!emit_counted_loop_begin(ctx, &plan))
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
                if (!emit_phi_edge(ctx, &plan, loop->body_block, loop->latch_block))
                    goto cleanup;
                ctx->indent -= 4;
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_append(ctx->sb, "}\n");
            }
        } else if (inst->opcode == USIL_OP_IF) {
            const HLSLIfRegion *region = &plan.regions[index];
            for (int item = 0; item < plan.value_count; ++item) {
                const StructuredValue *value = &plan.values[item];
                if (value->instruction < 0 && value->block == region->join_block &&
                    value->resolution == 2) {
                    sb_append_spaces(ctx->sb, ctx->indent);
                    sb_appendf(ctx->sb, "float4 %s;\n", value->name);
                }
            }
            ASTExpr *condition = source_expression(ctx, &plan, index, 0, 1);
            ASTExpr *bits = ast_create_bitcast(AST_SCALAR_UINT32, condition);
            if (!bits) {
                ast_free_expr(condition);
                goto cleanup;
            }
            sb_append_spaces(ctx->sb, ctx->indent);
            sb_append(ctx->sb, inst->condition_test == DXBC_INSTRUCTION_TEST_NONZERO
                                   ? "[branch] if ("
                                   : "[branch] if (!");
            ast_format_expr(bits, ctx->sb);
            ast_free_expr(bits);
            sb_append(ctx->sb, ") {\n");
            ctx->indent += 4;
        } else if (inst->opcode == USIL_OP_ELSE || inst->opcode == USIL_OP_ENDIF) {
            const HLSLInstructionFlow *flow = &ctx->cfg.instruction_flow[index];
            const int header = inst->opcode == USIL_OP_ENDIF
                                   ? flow->jump_scope
                                   : ctx->cfg.instruction_flow[flow->end].jump_scope;
            const HLSLIfRegion *region = &plan.regions[header];
            if (!emit_phi_edge(ctx, &plan, region->join_block, ctx->cfg.instruction_block[index]))
                goto cleanup;
            ctx->indent -= 4;
            sb_append_spaces(ctx->sb, ctx->indent);
            if (inst->opcode == USIL_OP_ELSE) {
                sb_append(ctx->sb, "} else {\n");
                ctx->indent += 4;
            } else if (region->else_instruction < 0) {
                sb_append(ctx->sb, "} else {\n");
                ctx->indent += 4;
                if (!emit_phi_edge(ctx, &plan, region->join_block, region->header_block))
                    goto cleanup;
                ctx->indent -= 4;
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_append(ctx->sb, "}\n");
            } else {
                sb_append(ctx->sb, "}\n");
            }
        } else {
            ASTExpr *left = source_expression(ctx, &plan, index, 1, 4);
            ASTExpr *right =
                inst->opcode == USIL_OP_MOV ? NULL : source_expression(ctx, &plan, index, 2, 4);
            ASTExpr *expression = hlsl_float4_operation(ctx, index, left, right);
            if (!expression)
                goto cleanup;
            sb_append_spaces(ctx->sb, ctx->indent);
            const DXBCOperand *destination = &inst->operands[0];
            if (destination->type == OPERAND_TYPE_TEMP) {
                sb_appendf(ctx->sb, "const float4 %s",
                           plan.values[plan.instruction_value[index]].name);
            } else if (!hlsl_float4_append_output(ctx, destination)) {
                ast_free_expr(expression);
                goto cleanup;
            }
            sb_append(ctx->sb, " = ");
            begin = ctx->sb->len;
            ast_format_expr(expression, ctx->sb);
            end = ctx->sb->len;
            ast_free_expr(expression);
            sb_append(ctx->sb, ";\n");
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
    free(plan.ssa_value);
    free(plan.ssa_lane);
    return success;
}
