// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

/* Only the complete sampled predicate admitted by the decoded layout planner
 * reaches this producer. Prove its reaching values independently through SSA;
 * BOOLEAN_BITS usage alone does not distinguish masks from arbitrary bits. */
bool hlsl_sampled_nan_guard_preflight(const HLSLEmitterContext *ctx) {
    HLSLSampledFragmentPlan plan = {0};
    if (!ctx || !ctx->high_level_sampled_fragment || !ctx->high_level_sampled_nan_guard ||
        !hlsl_sampled_fragment_plan_prepare(ctx->program, &plan) || !plan.nan_guard ||
        ctx->compiler_model.replacement_count || ctx->cfg.instruction_count != 12 ||
        ctx->cfg.block_count != 2 || !ctx->cfg.blocks || !ctx->cfg.instruction_block ||
        ctx->cfg.blocks[0].first_instruction != 0 || ctx->cfg.blocks[0].last_instruction != 10 ||
        ctx->cfg.blocks[0].predecessor_count || ctx->cfg.blocks[0].successor_count != 1 ||
        !ctx->cfg.blocks[0].successors || ctx->cfg.blocks[0].successors[0] != 1 ||
        ctx->cfg.blocks[1].first_instruction != 11 || ctx->cfg.blocks[1].last_instruction != 11 ||
        ctx->cfg.blocks[1].predecessor_count != 1 || !ctx->cfg.blocks[1].predecessors ||
        ctx->cfg.blocks[1].predecessors[0] || ctx->cfg.blocks[1].successor_count ||
        ctx->ssa.instruction_count != 12 || ctx->ssa.ssa_var_count < 1 ||
        !ctx->ssa.operand_ssa_vars || !ctx->ssa.relative_operand_ssa_vars ||
        !ctx->ssa.ssa_var_defs || !ctx->ssa.block_phis || ctx->ssa.block_phis[0].phi_count ||
        ctx->ssa.block_phis[1].phi_count ||
        ctx->use_def.instruction_count != 12 || !ctx->use_def.definition_use_counts) return false;
    static const int definitions[12][4] = {
        {-1, -1, -1, -1}, {-1, 0, -1, -1}, {-1, -1, 0, -1},
        {-1, 1, 2, -1}, {-1, 0, -1, -1}, {-1, 3, 4, -1},
        {-1, 5, -1, -1}, {-1, 6, 6, -1}, {-1, 6, 7, -1},
        {-1, 6, 8, -1}, {-1, 9, -1, 0}, {-1, -1, -1, -1}};
    unsigned uses[11][4] = {{0}};
    for (int index = 0; index < 12; ++index) {
        if (ctx->cfg.instruction_block[index] != (index == 11 ? 1 : 0)) return false;
        const USILInstruction *instruction = &ctx->program->instructions[index];
        USILEffectFlags effects;
        if (!usil_instruction_effects(ctx->program, instruction, &effects) ||
            (index != 0 && index != 11 && effects != USIL_EFFECT_NONE)) return false;
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, instruction, operand, &use)) return false;
            for (int lane = 0; lane < 4; ++lane) {
                const size_t offset = ((size_t)index * DXBC_MAX_OPERANDS + (unsigned)operand) * 4u + (unsigned)lane;
                if (ctx->ssa.relative_operand_ssa_vars[offset] >= 0) return false;
                if (instruction->operands[operand].type != OPERAND_TYPE_TEMP) continue;
                const bool destination = use.use == USIL_OPERAND_USE_DESTINATION;
                const uint8_t lanes = destination
                    ? usil_operand_destination_lane_mask(&instruction->operands[operand]) : use.source_lane_mask;
                if (!(lanes & (1u << lane))) continue;
                const int variable = ctx->ssa.operand_ssa_vars[offset];
                const int expected = destination ? index : definitions[index][operand];
                if (expected < 0 || variable < 0 || variable >= ctx->ssa.ssa_var_count ||
                    ctx->ssa.ssa_var_defs[variable] != expected ||
                    hlsl_operand_definition(ctx, index, operand, lane) != expected) return false;
                if (!destination) {
                    const unsigned selected = usil_operand_source_component(&instruction->operands[operand], lane);
                    if (selected > 3) return false;
                    ++uses[expected][selected];
                }
            }
        }
    }
    for (int index = 0; index < 11; ++index)
        for (int lane = 0; lane < 4; ++lane)
            if (hlsl_definition_use_count(ctx, index, lane) != uses[index][lane]) return false;
    return true;
}

static ASTExpr *typed_origin(const HLSLEmitterContext *ctx, ASTExpr *expression,
    int instruction, ASTScalarType type, unsigned width, bool projection) {
    if (!expression) return NULL;
    const USILInstruction *owner = &ctx->program->instructions[instruction];
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = type;
    origin.components = (uint8_t)width;
    origin.logical_value_id = (uint64_t)instruction;
    origin.instruction_index = instruction;
    origin.source_instruction_index = owner->source_instruction_index;
    origin.destination_lanes = usil_operand_destination_lane_mask(&owner->operands[0]);
    origin.semantic_projection = projection;
    if (!ast_set_logical_value_origin(expression, &origin)) {
        ast_free_expr(expression);
        return NULL;
    }
    return expression;
}

static ASTExpr *named_value(const HLSLEmitterContext *ctx, const char *name,
    int instruction, ASTScalarType type, unsigned width) {
    return typed_origin(ctx, ast_create_var(-1, -1, OPERAND_TYPE_TEMP, name),
        instruction, type, width, false);
}

/* Both constructors consume their children on every outcome. */
static ASTExpr *boolean_binary(const HLSLEmitterContext *ctx, int instruction,
    ASTExpr *left, ASTExpr *right, unsigned width) {
    const USILOpcode operation = ctx->program->instructions[instruction].opcode;
    ASTExpr *expression = operation == USIL_OP_OR
        ? ast_create_logical_binary(operation, left, right)
        : ast_create_vector_comparison(operation, left, right);
    if (!expression) { ast_free_expr(left); ast_free_expr(right); return NULL; }
    return typed_origin(ctx, expression, instruction, AST_SCALAR_BOOL, width, false);
}

static ASTExpr *nan_predicate(const HLSLEmitterContext *ctx, ASTExpr **roots) {
    ASTExpr *negative = boolean_binary(ctx, 1,
        named_value(ctx, "sampledColor", 0, AST_SCALAR_FLOAT32, 4), ast_create_literal_float(0.0f), 4);
    roots[1] = negative;
    ASTExpr *positive = boolean_binary(ctx, 2, ast_create_literal_float(0.0f),
        named_value(ctx, "sampledColor", 0, AST_SCALAR_FLOAT32, 4), 4);
    roots[2] = positive;
    ASTExpr *nonzero = boolean_binary(ctx, 3, negative, positive, 4);
    roots[3] = nonzero;
    ASTExpr *zero = boolean_binary(ctx, 4,
        named_value(ctx, "sampledColor", 0, AST_SCALAR_FLOAT32, 4), ast_create_literal_float(0.0f), 4);
    roots[4] = zero;
    ASTExpr *ordered = boolean_binary(ctx, 5, nonzero, zero, 4);
    roots[5] = ordered;
    ASTExpr *invalid = boolean_binary(ctx, 6, ordered, ast_create_literal_bool(false), 4);
    roots[6] = invalid;
    return invalid;
}

static ASTExpr *color_flag(const HLSLEmitterContext *ctx, unsigned channel, int consumer) {
    ASTExpr *value = named_value(ctx, "invalidColor", 6, AST_SCALAR_BOOL, 4);
    const int component = (int)channel;
    ASTExpr *projection = value ? ast_create_swizzle(value, &component, 1) : NULL;
    if (!projection) { ast_free_expr(value); return NULL; }
    return typed_origin(ctx, projection, consumer, AST_SCALAR_BOOL, 1, true);
}

static ASTExpr *selected_color(const HLSLEmitterContext *ctx, ASTExpr **roots) {
    /* The first two canonical Boolean flags commute. The selected compiler
     * emits Y|X from source X||Y; subsequent reductions retain the left tree.
     * Full-container comparison separately decides exact compiler inversion. */
    ASTExpr *condition = boolean_binary(ctx, 7, color_flag(ctx, 0, 7), color_flag(ctx, 1, 7), 1);
    roots[7] = condition;
    for (int instruction = 8; instruction <= 9; ++instruction) {
        condition = boolean_binary(ctx, instruction, condition,
            color_flag(ctx, (unsigned)instruction - 6u, instruction), 1);
        roots[instruction] = condition;
    }
    ASTExpr *fallback = ast_create_literal_bits(ctx->program->instructions[10].operands[2].imm_values,
        4, AST_SCALAR_FLOAT32);
    ASTExpr *color = named_value(ctx, "sampledColor", 0, AST_SCALAR_FLOAT32, 4);
    ASTExpr *selection = ast_create_ternary(condition, fallback, color);
    if (!selection) { ast_free_expr(condition); ast_free_expr(fallback); ast_free_expr(color); return NULL; }
    selection = typed_origin(ctx, selection, 10, AST_SCALAR_FLOAT32, 4, false);
    roots[10] = selection;
    return selection;
}

bool hlsl_emit_sampled_nan_guard(HLSLEmitterContext *ctx) {
    if (!hlsl_sampled_nan_guard_preflight(ctx) ||
        !hlsl_high_level_name_available(ctx, "sampledColor") ||
        !hlsl_high_level_name_available(ctx, "invalidColor")) return false;
    ASTExpr *roots[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT] = {0};
    ASTExpr *expressions[3] = {0};
    ctx->current_instruction_index = 0;
    expressions[0] = hlsl_texture_sample_expression(ctx,
        0, hlsl_natural_source_atom(ctx, 0, 1, 3), NULL, NULL);
    roots[0] = expressions[0];
    expressions[1] = nan_predicate(ctx, roots);
    expressions[2] = selected_color(ctx, roots);
    bool emitted = false;
    if (!expressions[0] || !expressions[1] || !expressions[2]) goto done;
    hlsl_expression_source_map_begin(ctx);
    static const int first[3] = {0, 1, 7}, last[3] = {0, 6, 10};
    static const char *const declarations[3] = {"const float4 sampledColor = ",
        "const bool4 invalidColor = ", "return "};
    for (unsigned expression = 0; expression < 3; ++expression) {
        const int instruction = last[expression];
        ctx->current_instruction_index = instruction;
        HLSLInstructionOwners owners = {0};
        for (int owner = first[expression]; owner <= instruction; ++owner)
            if (!hlsl_instruction_owners_add(&owners, owner)) goto done;
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_append(ctx->sb, declarations[expression]);
        if (!hlsl_source_quality_observe_expression(ctx, expressions[expression], instruction) ||
            !hlsl_format_owned_expression(ctx, expressions[expression], roots, &owners, false)) goto done;
        sb_append(ctx->sb, ";\n");
        hlsl_source_quality_emission(ctx, 0, false, instruction);
        if (!sb_ok(ctx->sb)) goto done;
    }
    emitted = sb_ok(ctx->sb);
done:
    for (unsigned index = 0; index < 3; ++index) ast_free_expr(expressions[index]);
    return emitted;
}
