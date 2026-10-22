// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "translation/usil_validation.h"

typedef struct HLSLSampledDiscardTree {
    ASTExpr *values[5];
    ASTExpr *recorded[5];
    int value_count;
    int condition_index;
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
    if (!ctx->high_level_sampled_discard || !ctx->program ||
        (tree->value_count != 4 && tree->value_count != 5) ||
        tree->condition_index != tree->value_count - 2 ||
        ctx->program->instruction_count != tree->value_count + 1 || !tree->statement ||
        !tree->body || !tree->discard || tree->statement->kind != AST_STMT_IF ||
        tree->statement->u.if_stmt.cond != tree->values[tree->condition_index] ||
        tree->statement->u.if_stmt.true_body != tree->body ||
        tree->statement->u.if_stmt.false_body || tree->body->kind != AST_STMT_BLOCK ||
        tree->body->u.block.stmt_count != 1 || !tree->body->u.block.stmts ||
        tree->body->u.block.stmts[0] != tree->discard || tree->discard->kind != AST_STMT_DISCARD) {
        return false;
    }
    for (int index = 0; index < tree->value_count; ++index) {
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
    facts->instruction_index = tree->condition_index;
    facts->source_instruction_index =
        ctx->program->instructions[tree->condition_index].source_instruction_index;
    facts->logical_operation = statement == tree->discard;
    return true;
}

static bool discard_cfg_matches(const HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan) {
    const HLSLControlFlowGraph *cfg = &ctx->cfg;
    const int instruction_count = plan->output_index + 2;
    if (cfg->instruction_count != instruction_count || cfg->block_count != 4 || !cfg->blocks ||
        !cfg->instruction_block || !cfg->idom || !cfg->nesting) {
        return false;
    }
    const int first[4] = {0, plan->discard_index, plan->output_index, instruction_count - 1};
    const int last[4] = {
        plan->comparison_index, plan->discard_index, plan->output_index, instruction_count - 1};
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
        ctx->compiler_model.replacement_count || !discard_cfg_matches(ctx, &plan) ||
        ctx->ssa.instruction_count != ctx->program->instruction_count ||
        ctx->ssa.ssa_var_count < 1 || !ctx->ssa.operand_ssa_vars ||
        !ctx->ssa.relative_operand_ssa_vars || !ctx->ssa.ssa_var_defs || !ctx->ssa.block_phis ||
        ctx->use_def.instruction_count != ctx->program->instruction_count ||
        !ctx->use_def.operand_definitions || !ctx->use_def.definition_use_counts) {
        return false;
    }
    if (plan.material) {
        const USILInstruction *arithmetic = &ctx->program->instructions[1];
        const DXBCOperand *result = &ctx->program->instructions[plan.output_index].operands[1];
        const uint8_t mask = usil_operand_destination_lane_mask(&arithmetic->operands[0]);
        if (!ctx->cbuffer_layouts_built ||
            !hlsl_material_source_metadata_matches(ctx, &arithmetic->operands[2], mask) ||
            (plan.arithmetic_opcode == USIL_OP_MAD &&
                !hlsl_material_source_metadata_matches(ctx, &arithmetic->operands[3], mask)) ||
            (result->type == OPERAND_TYPE_CONSTANT_BUFFER &&
                !hlsl_material_source_metadata_matches(ctx, result, 15))) {
            return false;
        }
    }
    for (int block = 0; block < 4; ++block) {
        if (ctx->ssa.block_phis[block].phi_count) {
            return false;
        }
    }
    const int instruction_count = ctx->program->instruction_count;
    unsigned uses[6][4] = {{0}};
    for (int index = 0; index < instruction_count; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        USILEffectFlags expected_effects = USIL_EFFECT_NONE;
        if (index == 0) {
            expected_effects = USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_QUAD_CONTEXT;
        } else if (index == plan.discard_index) {
            expected_effects = USIL_EFFECT_CONTROL | USIL_EFFECT_QUAD_CONTEXT;
        } else if (index == instruction_count - 1) {
            expected_effects = USIL_EFFECT_CONTROL;
        }
        USILEffectFlags observed;
        if (!usil_instruction_effects(ctx->program, instruction, &observed) ||
            observed != expected_effects) {
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
                int source_definition = 0;
                if (index == plan.discard_index) {
                    source_definition = plan.comparison_index;
                } else if (plan.material && index == plan.comparison_index) {
                    source_definition = 1;
                }
                const int expected = destination ? index : source_definition;
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
    for (int instruction = 0; instruction < instruction_count; ++instruction) {
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
    origin.destination_lanes = owner->opcode == USIL_OP_DISCARD
                                   ? 0
                                   : usil_operand_destination_lane_mask(&owner->operands[0]);
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

static ASTExpr *discard_compared_value(
    const HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan) {
    if (plan->material) {
        return discard_named_value(ctx, "coverageValue", 1, 1, AST_SCALAR_FLOAT32, 1);
    }
    ASTExpr *color = discard_named_value(ctx, "sampledValue", 0, 0, AST_SCALAR_FLOAT32, 4);
    const int component = plan->color_component;
    ASTExpr *projection = color ? ast_create_swizzle(color, &component, 1) : NULL;
    if (!projection) {
        ast_free_expr(color);
        return NULL;
    }
    return discard_value_origin(
        ctx, projection, plan->comparison_index, 0, AST_SCALAR_FLOAT32, 1, true);
}

static ASTExpr *discard_comparison(
    const HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan) {
    ASTExpr *projection = discard_compared_value(ctx, plan);
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
    return discard_value_origin(ctx,
        comparison,
        plan->comparison_index,
        (uint64_t)plan->comparison_index,
        AST_SCALAR_BOOL,
        1,
        false);
}

static ASTExpr *discard_condition(
    const HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan) {
    ASTExpr *value = discard_named_value(ctx,
        "discardFragment",
        plan->discard_index,
        (uint64_t)plan->comparison_index,
        AST_SCALAR_BOOL,
        1);
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
    return discard_value_origin(ctx,
        condition,
        plan->discard_index,
        (uint64_t)plan->discard_index,
        AST_SCALAR_BOOL,
        1,
        false);
}

static void discard_tree_dispose(HLSLSampledDiscardTree *tree) {
    if (tree->statement) {
        ast_free_stmt(tree->statement);
    } else {
        ast_free_expr(tree->values[tree->condition_index]);
        if (tree->body) {
            ast_free_stmt(tree->body);
        } else {
            ast_free_stmt(tree->discard);
        }
    }
    for (int index = 0; index < tree->value_count; ++index) {
        if (index != tree->condition_index) {
            ast_free_expr(tree->values[index]);
        }
        ast_free_expr(tree->recorded[index]);
    }
    *tree = (HLSLSampledDiscardTree){0};
}

static bool discard_tree_prepare(
    HLSLEmitterContext *ctx, const HLSLSampledDiscardPlan *plan, HLSLSampledDiscardTree *tree) {
    tree->value_count = plan->output_index + 1;
    tree->condition_index = plan->discard_index;
    ctx->current_instruction_index = 0;
    ASTExpr *coordinates = hlsl_natural_source_atom(ctx, 0, 1, 3);
    tree->values[0] = plan->material
                          ? hlsl_texture_sample_component_expression(ctx, 0, coordinates)
                          : hlsl_texture_sample_expression(ctx, 0, coordinates, NULL, NULL);
    if (plan->material) {
        ctx->current_instruction_index = 1;
        const uint8_t mask =
            usil_operand_destination_lane_mask(&ctx->program->instructions[1].operands[0]);
        ASTExpr *left = discard_named_value(ctx, "sampledValue", 0, 0, AST_SCALAR_FLOAT32, 1);
        ASTExpr *right = hlsl_natural_source_atom(ctx, 1, 2, mask);
        ASTExpr *third = plan->arithmetic_opcode == USIL_OP_MAD
                             ? hlsl_natural_source_atom(ctx, 1, 3, mask)
                             : NULL;
        tree->values[1] = hlsl_natural_float_operation(ctx, 1, left, right, third);
    }
    tree->values[plan->comparison_index] = discard_comparison(ctx, plan);
    tree->values[plan->discard_index] = discard_condition(ctx, plan);
    ctx->current_instruction_index = plan->output_index;
    tree->values[plan->output_index] =
        plan->material ? hlsl_natural_source_atom(ctx, plan->output_index, 1, 15)
                       : discard_named_value(ctx,
                             "sampledValue",
                             plan->output_index,
                             (uint64_t)plan->output_index,
                             AST_SCALAR_FLOAT32,
                             4);
    for (int index = 0; index < tree->value_count; ++index) {
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
    tree->statement = ast_create_if(tree->values[tree->condition_index], tree->body, NULL);
    return tree->statement != NULL;
}

static bool observe_discard_statement(HLSLEmitterContext *ctx, const HLSLSampledDiscardTree *tree) {
    if (!ctx->source_quality_analysis) {
        return true;
    }
    if (!hlsl_stage_coverage_root(
            ctx, tree->values[tree->condition_index], tree->condition_index)) {
        return false;
    }
    ctx->source_quality_root = tree->values[tree->condition_index];
    ctx->source_quality_instruction = tree->condition_index;
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
        !hlsl_high_level_name_available(ctx, "discardFragment") ||
        (plan.material && !hlsl_high_level_name_available(ctx, "coverageValue")) ||
        ctx->indent < 0 || ctx->indent % 2 != 0) {
        return false;
    }
    HLSLSampledDiscardTree tree = {0};
    ASTExpr *roots[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT] = {0};
    bool emitted = false;
    if (!discard_tree_prepare(ctx, &plan, &tree)) {
        goto cleanup;
    }
    ctx->sampled_discard_tree = &tree;
    for (int index = 0; index < tree.value_count; ++index) {
        roots[index] = tree.values[index];
    }
    hlsl_expression_source_map_begin(ctx);
    for (int instruction = 0; instruction < tree.value_count; ++instruction) {
        ctx->current_instruction_index = instruction;
        HLSLInstructionOwners owners = {0};
        if (!hlsl_instruction_owners_add(&owners, instruction)) {
            goto cleanup;
        }
        if (instruction == plan.discard_index) {
            const size_t begin = ctx->sb->len;
            if (!observe_discard_statement(ctx, &tree) ||
                !hlsl_format_owned_statement(
                    ctx, tree.statement, roots, &owners, ctx->indent / 2)) {
                goto cleanup;
            }
            if (ctx->expression_source_map) {
                HLSLExpressionOrigin *origin =
                    &ctx->expression_source_map->origins[plan.discard_index];
                origin->kind = HLSL_EXPRESSION_ORIGIN_EFFECT;
                origin->source_begin = begin;
                origin->source_end = ctx->sb->len;
            }
        } else {
            sb_append_spaces(ctx->sb, ctx->indent);
            const char *declaration = "return ";
            if (instruction == 0) {
                declaration =
                    plan.material ? "const float sampledValue = " : "const float4 sampledValue = ";
            } else if (instruction == plan.comparison_index) {
                declaration = "const bool discardFragment = ";
            } else if (plan.material && instruction == 1) {
                declaration = "const float coverageValue = ";
            }
            sb_append(ctx->sb, declaration);
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
