// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_stage_coverage_internal.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include "translation/hlsl_source_quality_internal.h"

#include <stdlib.h>
#include <string.h>

static HLSLStageCoverage *current_coverage(HLSLEmitterContext *ctx) {
    return ctx && ctx->matrix_use_capture ? &ctx->matrix_use_capture->coverage : NULL;
}

/* Every demanded lane of an omitted block needs its actual current matrix-read
 * owner. Known scalar/vector fields outside that inventory cannot silently
 * inherit the matrix declaration attachment. */
static bool omitted_matrix_block_supported(const HLSLEmitterContext *ctx, int index) {
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[index];
    const HLSLCurrentMatrixReads *reads = &ctx->matrix_use_capture->reads;
    if (!layout->omit_declaration || !layout->is_unity_builtin || layout->raw_storage ||
        layout->row_struct_storage || layout->reg < 0 || layout->reg >= 15) return false;
    bool saw_read = false;
    for (int instruction = 0; instruction < ctx->program->instruction_count; ++instruction) {
        const USILInstruction *owner = &ctx->program->instructions[instruction];
        for (int operand = 0; operand < owner->operand_count; ++operand) {
            const DXBCOperand *value = &owner->operands[operand];
            if (value->type != OPERAND_TYPE_CONSTANT_BUFFER || value->register_index != layout->reg) continue;
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, owner, operand, &use) ||
                use.use != USIL_OPERAND_USE_SOURCE || !use.source_lane_mask) return false;
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane))) continue;
                size_t matches = 0;
                for (size_t read_index = 0; read_index < reads->read_count; ++read_index) {
                    const HLSLCurrentMatrixRead *read = &reads->reads[read_index];
                    if (read->instruction_index != instruction || read->operand_index != operand ||
                        read->logical_lane != lane || read->field_index >= reads->field_count) continue;
                    const HLSLCurrentMatrixField *field = &reads->fields[read->field_index];
                    if (field->binding_register == (uint32_t)layout->reg && layout->serialized_name &&
                        !strcmp(field->block_name, layout->serialized_name)) ++matches;
                }
                if (matches != 1) return false;
                saw_read = true;
            }
        }
    }
    return saw_read;
}

bool hlsl_stage_coverage_begin(HLSLEmitterContext *ctx) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    if (coverage->began || !ctx->program || ctx->program->instruction_count < 1 ||
        ctx->program->instruction_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT) return false;
    coverage->began = true;
    coverage->stage = ctx->program->program_type;
    coverage->instruction_count = (size_t)ctx->program->instruction_count;
    if (!hlsl_source_quality_body_inventory_supported(ctx)) coverage->obligations |= HLSL_STAGE_COVERAGE_BODY;
    for (int index = 0; index < ctx->program->instruction_count; ++index) {
        const USILInstruction *owner = &ctx->program->instructions[index];
        coverage->source_instructions[index] = owner->source_instruction_index;
        coverage->destination_lanes[index] = owner->operand_count
            ? usil_operand_destination_lane_mask(&owner->operands[0]) : 0;
        if (owner->operand_count < 0 || owner->operand_count > DXBC_MAX_OPERANDS) return false;
        coverage->operand_counts[index] = (uint8_t)owner->operand_count;
        for (int operand = 0; operand < owner->operand_count; ++operand) {
            const DXBCOperand *value = &owner->operands[operand];
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, owner, operand, &use)) return false;
            coverage->operand_uses[index][operand] = (HLSLStageOwnedOperandUse){
                .type = value->type, .source_lanes = use.source_lane_mask,
                .is_source = use.use == USIL_OPERAND_USE_SOURCE,
                .absolute = value->has_abs, .negative = value->has_neg};
        }
    }
    for (int index = 0; index < ctx->cbuffer_layout_count; ++index) {
        if (hlsl_source_quality_local_cbuffer_supported(ctx, index)) continue;
        if (omitted_matrix_block_supported(ctx, index)) {
            coverage->required_binding_mask |= UINT32_C(1) << ctx->cbuffer_layouts[index].reg;
            coverage->obligations |= HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION;
        } else coverage->obligations |= HLSL_STAGE_COVERAGE_LOCAL_DECLARATION;
    }
    return true;
}

bool hlsl_stage_coverage_root(HLSLEmitterContext *ctx, const ASTExpr *root, int instruction) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    if (!coverage->began || coverage->finished || instruction < 0 ||
        (size_t)instruction >= coverage->instruction_count ||
        coverage->root_count == HLSL_STAGE_COVERAGE_ROOT_LIMIT) return false;
    size_t nodes;
    ASTExpr *copy = hlsl_owned_expression_copy(root, &nodes);
    if (!copy || coverage->node_count > HLSL_STAGE_COVERAGE_NODE_LIMIT ||
        nodes > HLSL_STAGE_COVERAGE_NODE_LIMIT - coverage->node_count ||
        (coverage->global_node_count && (*coverage->global_node_count > HLSL_STAGE_COVERAGE_GLOBAL_NODE_LIMIT ||
            nodes > HLSL_STAGE_COVERAGE_GLOBAL_NODE_LIMIT - *coverage->global_node_count))) {
        ast_free_expr(copy);
        return false;
    }
    size_t recorded_nodes;
    ASTExpr *recorded = hlsl_owned_expression_copy(copy, &recorded_nodes);
    if (!recorded || recorded_nodes != nodes) {
        ast_free_expr(copy);
        ast_free_expr(recorded);
        return false;
    }
    HLSLStageOwnedRoot *roots = realloc(coverage->roots, (coverage->root_count + 1) * sizeof(*roots));
    if (!roots) { ast_free_expr(copy); ast_free_expr(recorded); return false; }
    coverage->roots = roots;
    coverage->roots[coverage->root_count++] = (HLSLStageOwnedRoot){
        .tree = copy, .recorded_tree = recorded, .live_tree = root, .instruction = instruction};
    coverage->node_count += nodes;
    if (coverage->global_node_count) *coverage->global_node_count += nodes;
    return true;
}

bool hlsl_stage_coverage_observation(HLSLEmitterContext *ctx, const HLSLSourceQualityObservation *observation) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage || observation->kind != HLSL_SOURCE_OBSERVATION_EMISSION) return true;
    if (!coverage->began || coverage->finished || observation->unit_kind != HLSL_SOURCE_UNIT_ENTRY_POINT ||
        observation->source_unit_id || coverage->syntax_count >= HLSL_STAGE_COVERAGE_EVENT_LIMIT ||
        (coverage->global_event_count && *coverage->global_event_count >= HLSL_STAGE_COVERAGE_GLOBAL_EVENT_LIMIT))
        return false;
    HLSLStageOwnedSyntax *syntax = realloc(coverage->syntax, (coverage->syntax_count + 1) * sizeof(*syntax));
    if (!syntax) return false;
    coverage->syntax = syntax;
    coverage->syntax[coverage->syntax_count++] = (HLSLStageOwnedSyntax){
        .facts = observation->facts, .source_end = ctx->sb->len};
    if (coverage->global_event_count) ++*coverage->global_event_count;
    return true;
}

bool hlsl_stage_coverage_span(HLSLStageCoverage *coverage, const ASTExpr *root, size_t begin, size_t end) {
    if (!coverage) return true;
    for (size_t index = 0; index < coverage->root_count; ++index) {
        HLSLStageOwnedRoot *owned = &coverage->roots[index];
        if (owned->live_tree != root) continue;
        if (owned->emitted || begin >= end) return false;
        owned->begin = begin;
        owned->end = end;
        owned->emitted = true;
        owned->live_tree = NULL;
    }
    return true;
}

void hlsl_stage_coverage_finish(HLSLEmitterContext *ctx) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage || !coverage->began || coverage->finished) return;
    if (!hlsl_source_quality_interface_inventory_complete(ctx) ||
        !hlsl_source_quality_resource_inventory_complete(ctx)) coverage->obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
    for (int index = 0; index < ctx->cbuffer_layout_count; ++index)
        if (!ctx->cbuffer_layouts[index].omit_declaration &&
            !hlsl_source_quality_local_cbuffer_complete(ctx, index))
            coverage->obligations |= HLSL_STAGE_COVERAGE_LOCAL_DECLARATION;
    coverage->source_size = ctx->sb->len;
    coverage->source = malloc(coverage->source_size + 1);
    if (coverage->source) memcpy(coverage->source, ctx->sb->buf, coverage->source_size + 1);
    else coverage->obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
    if (!coverage->syntax_count || coverage->syntax[coverage->syntax_count - 1].source_end != coverage->source_size)
        coverage->obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
    for (size_t index = 0; index < coverage->root_count; ++index)
        if (!coverage->roots[index].emitted || coverage->roots[index].live_tree)
            coverage->obligations |= HLSL_STAGE_COVERAGE_AST;
    memcpy(coverage->recorded_operand_counts, coverage->operand_counts, sizeof(coverage->operand_counts));
    memcpy(coverage->recorded_operand_uses, coverage->operand_uses, sizeof(coverage->operand_uses));
    coverage->recorded_syntax_count = coverage->syntax_count;
    coverage->recorded_root_count = coverage->root_count;
    if (coverage->syntax_count) {
        coverage->recorded_syntax = malloc(coverage->syntax_count * sizeof(*coverage->recorded_syntax));
        if (coverage->recorded_syntax)
            memcpy(coverage->recorded_syntax, coverage->syntax,
                coverage->syntax_count * sizeof(*coverage->recorded_syntax));
    }
    coverage->finished = true;
}

static bool operand_uses_equal(const HLSLStageOwnedOperandUse *a, const HLSLStageOwnedOperandUse *b) {
    return a->type == b->type && a->source_lanes == b->source_lanes && a->is_source == b->is_source &&
        a->absolute == b->absolute && a->negative == b->negative;
}

static bool owner_valid(const HLSLStageCoverage *coverage, int instruction, uint32_t source, uint8_t lanes) {
    return instruction == -1 ? source == UINT32_MAX && !lanes : instruction >= 0 &&
        (size_t)instruction < coverage->instruction_count && source == coverage->source_instructions[instruction] &&
        !(lanes & ~coverage->destination_lanes[instruction]);
}

/* This alternative applies only to the source AST forms that the existing
 * planner stamps with a demanded source mask. Operation roots and syntax keep
 * destination ownership. An opaque operand carries its exact operand index;
 * modifier nodes without one must select one actual matching source operand.
 * Matching typed immutable trees remains a separate replay requirement. */
static bool source_owner_valid(const HLSLStageCoverage *coverage, int instruction,
    uint32_t source, uint8_t lanes, int operand, const ASTExpr *tree) {
    if (instruction < 0 || (size_t)instruction >= coverage->instruction_count ||
        source != coverage->source_instructions[instruction] || !lanes || !tree ||
        coverage->operand_counts[instruction] > DXBC_MAX_OPERANDS) return false;
    bool absolute = tree->kind == AST_EXPR_CALL && tree->u.call.name &&
        !strcmp(tree->u.call.name, "abs") && tree->u.call.arg_count == 1 && tree->u.call.args;
    bool negative = tree->kind == AST_EXPR_UNARY && tree->u.unary.op == USIL_OP_INEG;
    bool projection = tree->kind == AST_EXPR_SWIZZLE;
    if (tree->kind != AST_EXPR_EMITTER_OPERAND && !absolute && !negative && !projection) return false;
    if (operand >= (int)coverage->operand_counts[instruction] ||
        (tree->kind == AST_EXPR_EMITTER_OPERAND && operand < 0)) return false;
    const ASTExpr *child = absolute ? tree->u.call.args[0] : negative ? tree->u.unary.sub
        : projection ? tree->u.swizzle.sub : NULL;
    /* A formatter atom below a modifier already carries a concrete source
     * coordinate. Do not allow a different same-mask operand to stand in. */
    if (operand < 0 && child && child->kind == AST_EXPR_EMITTER_OPERAND &&
        child->operand_provenance.complete && child->operand_provenance.instruction_index == instruction)
        operand = child->operand_provenance.operand_index;
    size_t matches = 0;
    for (int index = 0; index < (int)coverage->operand_counts[instruction]; ++index) {
        if (operand >= 0 && index != operand) continue;
        const HLSLStageOwnedOperandUse *use = &coverage->operand_uses[instruction][index];
        if (!use->is_source || use->source_lanes != lanes ||
            (absolute && !use->absolute) || (negative && !use->negative) ||
            (projection && use->type != OPERAND_TYPE_TEMP)) continue;
        ++matches;
    }
    return matches == 1;
}

static bool tree_owners_valid(const HLSLStageCoverage *coverage, const ASTExpr *tree, unsigned depth, size_t *nodes) {
    if (!tree || depth > 64 || ++*nodes > HLSL_STAGE_COVERAGE_NODE_LIMIT) return false;
    if (tree->logical_origin.complete && !owner_valid(coverage, tree->logical_origin.instruction_index,
        tree->logical_origin.source_instruction_index, tree->logical_origin.destination_lanes) &&
        !source_owner_valid(coverage, tree->logical_origin.instruction_index,
            tree->logical_origin.source_instruction_index, tree->logical_origin.destination_lanes, -1, tree)) return false;
    if (tree->kind == AST_EXPR_EMITTER_OPERAND)
        return !tree->operand_provenance.complete || owner_valid(coverage,
            tree->operand_provenance.instruction_index, tree->operand_provenance.source_instruction_index,
            tree->operand_provenance.destination_lanes) || source_owner_valid(coverage,
                tree->operand_provenance.instruction_index, tree->operand_provenance.source_instruction_index,
                tree->operand_provenance.destination_lanes, tree->operand_provenance.operand_index, tree);
    switch (tree->kind) {
    case AST_EXPR_LITERAL:
    case AST_EXPR_VAR: return true;
    case AST_EXPR_UNARY: return tree_owners_valid(coverage, tree->u.unary.sub, depth + 1, nodes);
    case AST_EXPR_BINARY:
    case AST_EXPR_COMPARISON:
        return tree_owners_valid(coverage, tree->u.binary.left, depth + 1, nodes) &&
            tree_owners_valid(coverage, tree->u.binary.right, depth + 1, nodes);
    case AST_EXPR_SWIZZLE: return tree_owners_valid(coverage, tree->u.swizzle.sub, depth + 1, nodes);
    case AST_EXPR_CAST: return tree_owners_valid(coverage, tree->u.cast.sub, depth + 1, nodes);
    case AST_EXPR_BITCAST: return tree_owners_valid(coverage, tree->u.bitcast.sub, depth + 1, nodes);
    case AST_EXPR_TERNARY:
        return tree_owners_valid(coverage, tree->u.ternary.cond, depth + 1, nodes) &&
            tree_owners_valid(coverage, tree->u.ternary.true_expr, depth + 1, nodes) &&
            tree_owners_valid(coverage, tree->u.ternary.false_expr, depth + 1, nodes);
    case AST_EXPR_CALL:
        if (!tree->u.call.args || tree->u.call.arg_count < 1 || tree->u.call.arg_count > 4) return false;
        for (int index = 0; index < tree->u.call.arg_count; ++index)
            if (!tree_owners_valid(coverage, tree->u.call.args[index], depth + 1, nodes)) return false;
        return true;
    default: return false;
    }
}

bool hlsl_stage_coverage_validate(const HLSLStageCoverage *coverage, const StringBuilder *source) {
    const uint32_t known_obligations = HLSL_STAGE_COVERAGE_BODY | HLSL_STAGE_COVERAGE_LOCAL_DECLARATION |
        HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION | HLSL_STAGE_COVERAGE_SYNTAX | HLSL_STAGE_COVERAGE_AST;
    if (!coverage || !coverage->began || !coverage->finished || !coverage->instruction_count ||
        coverage->instruction_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT || !source || !sb_ok(source) || !source->buf ||
        source->len != coverage->source_size || !coverage->source ||
        memcmp(source->buf, coverage->source, coverage->source_size + 1) ||
        (coverage->obligations & ~known_obligations) || (coverage->required_binding_mask & ~UINT32_C(0x7fff)) ||
        (!!coverage->required_binding_mask != !!(coverage->obligations & HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION)) ||
        coverage->root_count != coverage->recorded_root_count ||
        coverage->syntax_count != coverage->recorded_syntax_count ||
        coverage->root_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
        coverage->syntax_count > HLSL_STAGE_COVERAGE_EVENT_LIMIT ||
        (coverage->root_count && !coverage->roots) ||
        (coverage->syntax_count && (!coverage->syntax || !coverage->recorded_syntax)) ||
        (!(coverage->obligations & HLSL_STAGE_COVERAGE_SYNTAX) &&
            (!coverage->syntax_count || coverage->syntax[coverage->syntax_count - 1].source_end != source->len)))
        return false;
    for (size_t index = 0; index < coverage->instruction_count; ++index) {
        if (coverage->operand_counts[index] > DXBC_MAX_OPERANDS ||
            coverage->operand_counts[index] != coverage->recorded_operand_counts[index]) return false;
        for (unsigned operand = 0; operand < coverage->operand_counts[index]; ++operand)
            if (!operand_uses_equal(&coverage->operand_uses[index][operand],
                    &coverage->recorded_operand_uses[index][operand])) return false;
    }
    size_t previous = 0, nodes = 0;
    for (size_t index = 0; index < coverage->syntax_count; ++index) {
        const HLSLStageOwnedSyntax *syntax = &coverage->syntax[index];
        if (syntax->source_end != coverage->recorded_syntax[index].source_end ||
            !hlsl_source_quality_facts_equal(&syntax->facts, &coverage->recorded_syntax[index].facts) ||
            syntax->source_end < previous || syntax->source_end > source->len ||
            !owner_valid(coverage, syntax->facts.instruction_index,
                syntax->facts.source_instruction_index, syntax->facts.lanes)) return false;
        previous = syntax->source_end;
    }
    for (size_t index = 0; index < coverage->root_count; ++index) {
        const HLSLStageOwnedRoot *root = &coverage->roots[index];
        if (!root->emitted || root->live_tree || root->instruction < 0 ||
            (size_t)root->instruction >= coverage->instruction_count || root->begin >= root->end ||
            root->end > source->len || !hlsl_matrix_uses_trees_equal(root->tree, root->recorded_tree) ||
            !tree_owners_valid(coverage, root->tree, 0, &nodes)) return false;
        StringBuilder formatted;
        sb_init(&formatted);
        ast_format_expr(root->tree, &formatted);
        bool valid = sb_ok(&formatted) && formatted.len == root->end - root->begin &&
            !memcmp(formatted.buf, source->buf + root->begin, formatted.len);
        sb_free(&formatted);
        if (!valid) return false;
    }
    return nodes == coverage->node_count;
}

bool hlsl_stage_coverage_equal(const HLSLStageCoverage *a, const HLSLStageCoverage *b) {
    if (a->obligations != b->obligations || a->required_binding_mask != b->required_binding_mask ||
        a->stage != b->stage || a->instruction_count != b->instruction_count || a->source_size != b->source_size ||
        a->node_count != b->node_count || a->root_count != b->root_count || a->syntax_count != b->syntax_count ||
        a->recorded_root_count != b->recorded_root_count || a->recorded_syntax_count != b->recorded_syntax_count ||
        !a->finished || !b->finished || !a->source || !b->source ||
        memcmp(a->source, b->source, a->source_size + 1)) return false;
    for (size_t index = 0; index < a->instruction_count; ++index) {
        if (a->operand_counts[index] > DXBC_MAX_OPERANDS ||
            a->source_instructions[index] != b->source_instructions[index] ||
            a->destination_lanes[index] != b->destination_lanes[index] ||
            a->operand_counts[index] != b->operand_counts[index] ||
            a->recorded_operand_counts[index] != b->recorded_operand_counts[index]) return false;
        for (unsigned operand = 0; operand < a->operand_counts[index]; ++operand) {
            const HLSLStageOwnedOperandUse *x = &a->operand_uses[index][operand];
            const HLSLStageOwnedOperandUse *y = &b->operand_uses[index][operand];
            if (!operand_uses_equal(x, y) || !operand_uses_equal(
                    &a->recorded_operand_uses[index][operand], &b->recorded_operand_uses[index][operand])) return false;
        }
    }
    for (size_t index = 0; index < a->syntax_count; ++index)
        if (a->syntax[index].source_end != b->syntax[index].source_end ||
            !hlsl_source_quality_facts_equal(&a->syntax[index].facts, &b->syntax[index].facts)) return false;
    for (size_t index = 0; index < a->root_count; ++index) {
        const HLSLStageOwnedRoot *x = &a->roots[index], *y = &b->roots[index];
        if (!x->emitted || !y->emitted || x->live_tree || y->live_tree || x->instruction != y->instruction ||
            x->begin != y->begin || x->end != y->end || x->whole_begin != y->whole_begin ||
            x->whole_end != y->whole_end || !hlsl_matrix_uses_trees_equal(x->tree, y->tree) ||
            !hlsl_matrix_uses_trees_equal(x->recorded_tree, y->recorded_tree)) return false;
    }
    return true;
}

void hlsl_stage_coverage_dispose(HLSLStageCoverage *coverage) {
    for (size_t index = 0; index < coverage->root_count; ++index) {
        ast_free_expr(coverage->roots[index].tree);
        ast_free_expr(coverage->roots[index].recorded_tree);
    }
    free(coverage->roots);
    free(coverage->syntax);
    free(coverage->recorded_syntax);
    free(coverage->source);
    memset(coverage, 0, sizeof(*coverage));
}
