// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "translation/usil_validation.h"

typedef struct HLSLSampledDiscardTree {
    ASTExpr *values[4];
    ASTExpr *recorded[4];
    ASTStmt *statement;
    ASTStmt *body;
    ASTStmt *discard;
} HLSLSampledDiscardTree;

bool hlsl_sampled_discard_tree_matches(const HLSLEmitterContext *ctx) {
    if (!ctx) {
        return false;
    }
    const HLSLSampledDiscardTree *tree = ctx->sampled_discard_tree;
    if (!tree) {
        return true;
    }
    if (!ctx->high_level_sampled_discard || !tree->statement || !tree->body || !tree->discard ||
        tree->statement->kind != AST_STMT_IF ||
        tree->statement->u.if_stmt.cond != tree->values[2] ||
        tree->statement->u.if_stmt.true_body != tree->body ||
        tree->statement->u.if_stmt.false_body || tree->body->kind != AST_STMT_BLOCK ||
        tree->body->u.block.stmt_count != 1 || !tree->body->u.block.stmts ||
        tree->body->u.block.stmts[0] != tree->discard || tree->discard->kind != AST_STMT_DISCARD) {
        return false;
    }
    for (unsigned index = 0; index < 4; ++index) {
        if (!tree->values[index] || !tree->recorded[index] ||
            !hlsl_matrix_uses_trees_equal(tree->values[index], tree->recorded[index])) {
            return false;
        }
    }
    return true;
}

bool hlsl_sampled_discard_statement_facts(
    void *context, uint32_t unit_id, const ASTStmt *statement, HLSLSourceQualityFacts *facts) {
    HLSLEmitterContext *ctx = context;
    if (!ctx || !facts || unit_id != 0 || !ctx->sampled_discard_tree ||
        !hlsl_sampled_discard_tree_matches(ctx)) {
        return false;
    }
    const HLSLSampledDiscardTree *tree = ctx->sampled_discard_tree;
    if (statement != tree->statement && statement != tree->body && statement != tree->discard) {
        return false;
    }
    hlsl_source_quality_facts_init(facts);
    facts->known = true;
    facts->instruction_index = 2;
    facts->source_instruction_index = ctx->program->instructions[2].source_instruction_index;
    facts->logical_operation = statement == tree->discard;
    return true;
}

static bool discard_cfg_matches(const HLSLEmitterContext *ctx) {
    const HLSLControlFlowGraph *cfg = &ctx->cfg;
    if (cfg->instruction_count != 5 || cfg->block_count != 4 || !cfg->blocks ||
        !cfg->instruction_block || !cfg->idom || !cfg->nesting) {
        return false;
    }
    static const int first[4] = {0, 2, 3, 4};
    static const int last[4] = {1, 2, 3, 4};
    for (int index = 0; index < 4; ++index) {
        const HLSLBasicBlock *block = &cfg->blocks[index];
        if (block->first_instruction != first[index] || block->last_instruction != last[index] ||
            block->has_ambiguous_flow || block->may_exit != (index == 1 || index == 3) ||
            cfg->idom[index] != (index ? index - 1 : 0) || cfg->nesting[index].parent_block != -1 ||
            cfg->nesting[index].is_loop || cfg->nesting[index].is_if ||
            block->predecessor_count != (index ? 1 : 0) ||
            (index && (!block->predecessors || block->predecessors[0] != index - 1)) ||
            block->successor_count != (index == 3 ? 0 : 1) ||
            (index != 3 && (!block->successors || block->successors[0] != index + 1))) {
            return false;
        }
        for (int instruction = first[index]; instruction <= last[index]; ++instruction) {
            if (cfg->instruction_block[instruction] != index) {
                return false;
            }
        }
    }
    return true;
}

/* LT and GE produce canonical Boolean masks, consumed only by the conditional kill.
 * The sample remains one reaching float4 value on the surviving path. */
bool hlsl_sampled_discard_preflight(const HLSLEmitterContext *ctx) {
    HLSLSampledDiscardPlan plan;
    if (!ctx || ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
        !hlsl_sampled_discard_plan_prepare(ctx->program, &plan) ||
        ctx->compiler_model.replacement_count || !discard_cfg_matches(ctx) ||
        ctx->ssa.instruction_count != 5 || ctx->ssa.ssa_var_count < 1 ||
        !ctx->ssa.operand_ssa_vars || !ctx->ssa.relative_operand_ssa_vars ||
        !ctx->ssa.ssa_var_defs || !ctx->ssa.block_phis || ctx->use_def.instruction_count != 5 ||
        !ctx->use_def.operand_definitions || !ctx->use_def.definition_use_counts) {
        return false;
    }
    for (int block = 0; block < 4; ++block) {
        if (ctx->ssa.block_phis[block].phi_count) {
            return false;
        }
    }
    static const USILEffectFlags effects[5] = {USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_QUAD_CONTEXT,
        USIL_EFFECT_NONE,
        USIL_EFFECT_CONTROL | USIL_EFFECT_QUAD_CONTEXT,
        USIL_EFFECT_NONE,
        USIL_EFFECT_CONTROL};
    static const int source_definitions[5] = {-1, 0, 1, 0, -1};
    unsigned uses[5][4] = {{0}};
    for (int index = 0; index < 5; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        USILEffectFlags observed;
        if (!usil_instruction_effects(ctx->program, instruction, &observed) ||
            observed != effects[index]) {
            return false;
        }
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, instruction, operand, &use)) {
                return false;
            }
            for (int lane = 0; lane < 4; ++lane) {
                const size_t offset =
                    ((size_t)index * DXBC_MAX_OPERANDS + (unsigned)operand) * 4u + (unsigned)lane;
                if (ctx->ssa.relative_operand_ssa_vars[offset] >= 0) {
                    return false;
                }
                if (instruction->operands[operand].type != OPERAND_TYPE_TEMP) {
                    continue;
                }
                const bool destination = use.use == USIL_OPERAND_USE_DESTINATION;
                const uint8_t lanes = destination ? usil_operand_destination_lane_mask(
                                                        &instruction->operands[operand])
                                                  : use.source_lane_mask;
                if (!(lanes & (1u << lane))) {
                    continue;
                }
                const int variable = ctx->ssa.operand_ssa_vars[offset];
                const int expected = destination ? index : source_definitions[index];
                if (expected < 0 || variable < 0 || variable >= ctx->ssa.ssa_var_count ||
                    ctx->ssa.ssa_var_defs[variable] != expected ||
                    hlsl_operand_definition(ctx, index, operand, lane) != expected) {
                    return false;
                }
                if (!destination) {
                    const unsigned component =
                        usil_operand_source_component(&instruction->operands[operand], lane);
                    if (component > 3 || ctx->use_def.operand_definitions[offset] != expected) {
                        return false;
                    }
                    ++uses[expected][component];
                }
            }
        }
    }
    for (int instruction = 0; instruction < 5; ++instruction) {
        for (int lane = 0; lane < 4; ++lane) {
            if (hlsl_definition_use_count(ctx, instruction, lane) != uses[instruction][lane]) {
                return false;
            }
        }
    }
    return true;
}

/* Consumes expression on every outcome. The decoded owner determines the
 * destination lanes; DISCARD has a source operand but no destination. */
static ASTExpr *discard_value_origin(const HLSLEmitterContext *ctx,
    ASTExpr *expression,
    int instruction,
    uint64_t value_id,
    ASTScalarType type,
    unsigned width,
    bool projection) {
    if (!expression) {
        return NULL;
    }
    const USILInstruction *owner = &ctx->program->instructions[instruction];
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = type;
    origin.components = (uint8_t)width;
    origin.logical_value_id = value_id;
    origin.instruction_index = instruction;
    origin.source_instruction_index = owner->source_instruction_index;
    origin.destination_lanes =
        instruction == 2 ? 0 : usil_operand_destination_lane_mask(&owner->operands[0]);
    origin.semantic_projection = projection;
    if (!ast_set_logical_value_origin(expression, &origin)) {
        ast_free_expr(expression);
        return NULL;
    }
    return expression;
}

static ASTExpr *discard_named_value(const HLSLEmitterContext *ctx,
    const char *name,
    int instruction,
    uint64_t value_id,
    ASTScalarType type,
    unsigned width) {
    return discard_value_origin(ctx,
        ast_create_var(-1, -1, OPERAND_TYPE_TEMP, name),
        instruction,
        value_id,
        type,
        width,
        false);
}

static ASTExpr *discard_comparison(
    const HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan) {
    ASTExpr *color = discard_named_value(ctx, "sampledValue", 0, 0, AST_SCALAR_FLOAT32, 4);
    const int component = plan->color_component;
    ASTExpr *projection = color ? ast_create_swizzle(color, &component, 1) : NULL;
    if (!projection) {
        ast_free_expr(color);
        return NULL;
    }
    projection = discard_value_origin(ctx, projection, 1, 0, AST_SCALAR_FLOAT32, 1, true);
    ASTExpr *literal = ast_create_literal_bits(&plan->threshold_bits, 1, AST_SCALAR_FLOAT32);
    ASTExpr *comparison = projection && literal ? ast_create_comparison(plan->comparison_opcode,
                                                      plan->literal_first ? literal : projection,
                                                      plan->literal_first ? projection : literal)
                                                : NULL;
    if (!comparison) {
        ast_free_expr(literal);
        ast_free_expr(projection);
        return NULL;
    }
    return discard_value_origin(ctx, comparison, 1, 1, AST_SCALAR_BOOL, 1, false);
}

static ASTExpr *discard_condition(
    const HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan) {
    ASTExpr *value = discard_named_value(ctx, "discardFragment", 2, 1, AST_SCALAR_BOOL, 1);
    if (!value || !plan->discard_on_zero) {
        return value;
    }
    /* Inverting the Boolean result preserves unordered comparisons. Replacing
     * LT with GE would change the decoded behavior for NaN operands. */
    ASTExpr *condition = ast_create_logical_not(value);
    if (!condition) {
        ast_free_expr(value);
        return NULL;
    }
    return discard_value_origin(ctx, condition, 2, 2, AST_SCALAR_BOOL, 1, false);
}

static void discard_tree_dispose(HLSLSampledDiscardTree *tree) {
    if (tree->statement) {
        ast_free_stmt(tree->statement);
    } else {
        ast_free_expr(tree->values[2]);
        if (tree->body) {
            ast_free_stmt(tree->body);
        } else {
            ast_free_stmt(tree->discard);
        }
    }
    for (unsigned index = 0; index < 4; ++index) {
        if (index != 2) {
            ast_free_expr(tree->values[index]);
        }
        ast_free_expr(tree->recorded[index]);
    }
    *tree = (HLSLSampledDiscardTree){0};
}

static bool discard_tree_prepare(
    HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan, HLSLSampledDiscardTree *tree) {
    ctx->current_instruction_index = 0;
    tree->values[0] =
        hlsl_texture_sample_expression(ctx, 0, hlsl_natural_source_atom(ctx, 0, 1, 3), NULL, NULL);
    tree->values[1] = discard_comparison(ctx, plan);
    tree->values[2] = discard_condition(ctx, plan);
    tree->values[3] = discard_named_value(ctx, "sampledValue", 3, 3, AST_SCALAR_FLOAT32, 4);
    for (unsigned index = 0; index < 4; ++index) {
        if (!tree->values[index]) {
            return false;
        }
        size_t nodes = 0;
        tree->recorded[index] = hlsl_owned_expression_copy(tree->values[index], &nodes);
        if (!tree->recorded[index]) {
            return false;
        }
    }
    tree->discard = ast_create_flow(AST_STMT_DISCARD);
    if (!tree->discard) {
        return false;
    }
    tree->body = ast_create_block();
    if (!tree->body) {
        return false;
    }
    if (!ast_block_add(tree->body, tree->discard)) {
        ast_free_stmt(tree->discard);
        tree->discard = NULL;
        return false;
    }
    tree->statement = ast_create_if(tree->values[2], tree->body, NULL);
    return tree->statement != NULL;
}

static bool observe_discard_statement(HLSLEmitterContext *ctx, const HLSLSampledDiscardTree *tree) {
    if (!ctx->source_quality_analysis) {
        return true;
    }
    if (!hlsl_stage_coverage_root(ctx, tree->values[2], 2)) {
        return false;
    }
    ctx->source_quality_root = tree->values[2];
    ctx->source_quality_instruction = 2;
    const bool observed =
        hlsl_source_quality_analysis_statement(ctx->source_quality_analysis, tree->statement);
    ctx->source_quality_root = NULL;
    if (!observed || !hlsl_sampled_discard_tree_matches(ctx)) {
        hlsl_emit_fail(ctx,
            HLSL_EMIT_STATUS_ANALYSIS_FAILED,
            HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
            HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    return true;
}

bool hlsl_emit_sampled_discard(HLSLEmitterContext *ctx) {
    HLSLSampledDiscardPlan plan;
    if (!hlsl_sampled_discard_preflight(ctx) ||
        !hlsl_sampled_discard_plan_prepare(ctx->program, &plan) || ctx->sampled_discard_tree ||
        !hlsl_high_level_name_available(ctx, "sampledValue") ||
        !hlsl_high_level_name_available(ctx, "discardFragment") || ctx->indent < 0 ||
        ctx->indent % 2 != 0) {
        return false;
    }
    HLSLSampledDiscardTree tree = {0};
    ASTExpr *roots[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT] = {0};
    bool emitted = false;
    if (!discard_tree_prepare(ctx, &plan, &tree)) {
        goto cleanup;
    }
    ctx->sampled_discard_tree = &tree;
    for (unsigned index = 0; index < 4; ++index) {
        roots[index] = tree.values[index];
    }
    hlsl_expression_source_map_begin(ctx);
    static const char *const declarations[4] = {
        "const float4 sampledValue = ", "const bool discardFragment = ", NULL, "return "};
    for (int instruction = 0; instruction < 4; ++instruction) {
        ctx->current_instruction_index = instruction;
        HLSLInstructionOwners owners = {0};
        if (!hlsl_instruction_owners_add(&owners, instruction)) {
            goto cleanup;
        }
        if (instruction == 2) {
            const size_t begin = ctx->sb->len;
            if (!observe_discard_statement(ctx, &tree) ||
                !hlsl_format_owned_statement(
                    ctx, tree.statement, roots, &owners, ctx->indent / 2)) {
                goto cleanup;
            }
            if (ctx->expression_source_map) {
                HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[2];
                origin->kind = HLSL_EXPRESSION_ORIGIN_EFFECT;
                origin->source_begin = begin;
                origin->source_end = ctx->sb->len;
            }
        } else {
            sb_append_spaces(ctx->sb, ctx->indent);
            sb_append(ctx->sb, declarations[instruction]);
            if (!hlsl_source_quality_observe_expression(
                    ctx, tree.values[instruction], instruction) ||
                !hlsl_format_owned_expression(
                    ctx, tree.values[instruction], roots, &owners, false)) {
                goto cleanup;
            }
            sb_append(ctx->sb, ";\n");
        }
        hlsl_source_quality_emission(ctx, 0, false, instruction);
        if (!sb_ok(ctx->sb) || !hlsl_sampled_discard_tree_matches(ctx)) {
            goto cleanup;
        }
    }
    emitted = true;

cleanup:
    ctx->sampled_discard_tree = NULL;
    discard_tree_dispose(&tree);
    return emitted;
}
