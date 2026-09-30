// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_source_quality.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/usil.h"

#include <stdlib.h>
#include <string.h>

enum { SOURCE_QUALITY_DEPTH_LIMIT = 256 };

typedef struct {
    const HLSLSourceQualityRequest *request;
    HLSLSourceQualityResult *result;
    const HLSLSourceQualityUnit *unit;
    const void *ancestors[SOURCE_QUALITY_DEPTH_LIMIT + 1];
    size_t remaining;
} QualityAnalysis;

struct HLSLSourceQualityAnalysis {
    HLSLSourceQualityRequest request;
    QualityAnalysis visitor;
    HLSLSourceQualityUnit unit;
    size_t unit_start_nodes;
    size_t entry_points;
    bool active_unit;
    bool failed;
    bool finished;
};

typedef struct {
    bool logical;
    unsigned components;
} ExpressionValue;

void hlsl_source_quality_facts_init(HLSLSourceQualityFacts *facts) {
    if (!facts)
        return;
    memset(facts, 0, sizeof(*facts));
    facts->logical_value_id = UINT64_MAX;
    facts->instruction_index = -1;
    facts->source_instruction_index = UINT32_MAX;
    facts->declaration_variant_index = UINT32_MAX;
    facts->declaration_field_index = UINT32_MAX;
    facts->declaration_witness_subprogram_index = UINT32_MAX;
    facts->resource_binding_register = UINT32_MAX;
    facts->cbuffer_binding_register = UINT32_MAX;
    facts->cbuffer_field_index = UINT32_MAX;
}

const char *hlsl_source_quality_class_name(HLSLSourceQualityClass classification) {
    switch (classification) {
    case HLSL_SOURCE_QUALITY_CLEAN:
        return "clean";
    case HLSL_SOURCE_QUALITY_MIXED:
        return "mixed";
    case HLSL_SOURCE_QUALITY_LOW_LEVEL:
        return "low-level";
    case HLSL_SOURCE_QUALITY_UNSUPPORTED:
        return "unsupported";
    case HLSL_SOURCE_QUALITY_FAILED:
        return "failed";
    default:
        return "invalid";
    }
}

static bool fail_analysis(QualityAnalysis *analysis, uint32_t reason) {
    analysis->result->classification = HLSL_SOURCE_QUALITY_FAILED;
    analysis->result->reasons |= reason;
    return false;
}

static bool enter_node(QualityAnalysis *analysis, const void *node, unsigned depth) {
    if (!node)
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    if (depth > SOURCE_QUALITY_DEPTH_LIMIT || !analysis->remaining)
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND);
    for (unsigned index = 0; index < depth; ++index)
        if (analysis->ancestors[index] == node)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    analysis->ancestors[depth] = node;
    --analysis->remaining;
    return true;
}

static bool facts_valid(const HLSLSourceQualityFacts *facts) {
    if (facts->value_kind < HLSL_SOURCE_VALUE_UNKNOWN ||
        facts->value_kind > HLSL_SOURCE_VALUE_REGISTER || facts->components > 4 ||
        (facts->artifacts & ~HLSL_SOURCE_ARTIFACT_ALL) || (facts->lanes & ~15u) ||
        facts->instruction_index < -1)
        return false;
    if (facts->resource_declaration_kind < HLSL_SOURCE_RESOURCE_NONE ||
        facts->resource_declaration_kind > HLSL_SOURCE_RESOURCE_UAV ||
        (facts->resource_declaration_kind != HLSL_SOURCE_RESOURCE_NONE &&
         (facts->instruction_index != -1 || facts->logical_operation ||
          facts->resource_binding_register == UINT32_MAX)))
        return false;
    if (facts->cbuffer_declaration_kind < HLSL_SOURCE_CBUFFER_NONE ||
        facts->cbuffer_declaration_kind > HLSL_SOURCE_CBUFFER_END)
        return false;
    if (facts->cbuffer_declaration_kind != HLSL_SOURCE_CBUFFER_NONE) {
        if (!facts->known || facts->instruction_index != -1 || facts->logical_operation ||
            facts->artifacts || facts->resource_declaration_kind != HLSL_SOURCE_RESOURCE_NONE ||
            facts->cbuffer_binding_register >= 15 || !facts->cbuffer_byte_size ||
            facts->cbuffer_byte_size > 65536u || facts->cbuffer_byte_offset > 65536u ||
            facts->cbuffer_byte_offset > UINT32_MAX - facts->cbuffer_byte_size ||
            facts->cbuffer_byte_offset + facts->cbuffer_byte_size > 65536u ||
            (facts->cbuffer_declaration_authority != 1 &&
             facts->cbuffer_declaration_authority != 2))
            return false;
        if (facts->cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_FIELD) {
            if (facts->cbuffer_field_index != facts->cbuffer_byte_offset / 16u ||
                facts->cbuffer_byte_size > 16 ||
                (facts->cbuffer_byte_size & 3u) || (facts->cbuffer_byte_offset & 15u))
                return false;
        } else if (facts->cbuffer_field_index != UINT32_MAX || facts->cbuffer_byte_offset ||
                   (facts->cbuffer_byte_size & 15u))
            return false;
    }
    if (facts->instruction_index == -1 &&
        (facts->source_instruction_index != UINT32_MAX || facts->lanes))
        return false;
    if (facts->instruction_index >= 0 && facts->source_instruction_index == UINT32_MAX)
        return false;
    if (facts->known && facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
        facts->logical_value_id == UINT64_MAX)
        return false;
    if (facts->semantic_projection &&
        (!facts->known || facts->value_kind != HLSL_SOURCE_VALUE_LOGICAL || !facts->components))
        return false;
    if (facts->real_bitcast && !facts->known)
        return false;
    if ((facts->declaration_witness_count || facts->declaration_witness_record) &&
        (!facts->known || facts->instruction_index != -1 || facts->logical_operation))
        return false;
    if (facts->declaration_witness_count && !facts->declaration_witness_record)
        return false;
    if (facts->declaration_witness_record &&
        (facts->declaration_variant_index == UINT32_MAX ||
         facts->declaration_field_index == UINT32_MAX ||
         facts->declaration_witness_subprogram_index == UINT32_MAX))
        return false;
    if ((facts->semantic_projection &&
         (facts->artifacts & HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT)) ||
        (facts->real_bitcast && (facts->artifacts & HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST)))
        return false;
    return true;
}

static bool observe(QualityAnalysis *analysis, HLSLSourceQualityObservationKind kind,
                    int ast_kind, HLSLSourceQualityFacts facts, bool logical_operation) {
    if (!facts_valid(&facts))
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    if (facts.value_kind == HLSL_SOURCE_VALUE_REGISTER)
        facts.artifacts |= HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE;
    uint32_t reasons = 0;
    HLSLSourceQualityCounters *counts = &analysis->result->counts;
    if (!facts.known) {
        ++counts->unknown_provenance;
        reasons |= HLSL_SOURCE_QUALITY_REASON_UNKNOWN_PROVENANCE;
    }
    if (facts.artifacts) {
        reasons |= HLSL_SOURCE_QUALITY_REASON_RESIDUAL;
        const uint32_t flags[] = {
            HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE,
            HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT,
            HLSL_SOURCE_ARTIFACT_SCALARIZED_INTRINSIC,
            HLSL_SOURCE_ARTIFACT_RAW_BUFFER_RECONSTRUCTION,
            HLSL_SOURCE_ARTIFACT_SYNTHETIC_INTERFACE,
            HLSL_SOURCE_ARTIFACT_INSTRUCTION_ASSIGNMENT,
            HLSL_SOURCE_ARTIFACT_UNSTRUCTURED_CONTROL,
            HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST};
        size_t *counters[] = {
            &counts->register_storage, &counts->lane_transport, &counts->scalarized_intrinsics,
            &counts->raw_buffer_reconstruction, &counts->synthetic_interface,
            &counts->instruction_assignments, &counts->unstructured_control,
            &counts->storage_bitcasts};
        for (size_t index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index)
            if (facts.artifacts & flags[index]) {
                ++*counters[index];
                ++counts->residual_total;
            }
    }
    if (facts.known && facts.value_kind == HLSL_SOURCE_VALUE_LOGICAL)
        ++counts->logical_value_references;
    if (logical_operation && facts.known && !facts.artifacts)
        ++counts->logical_operations;
    if (facts.known && facts.semantic_projection)
        ++counts->semantic_projections;
    if (facts.known && facts.real_bitcast)
        ++counts->real_bitcasts;
    if (facts.declaration_witness_count) {
        if (counts->sibling_declaration_witnesses > SIZE_MAX - facts.declaration_witness_count)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND);
        ++counts->sibling_declarations;
        counts->sibling_declaration_witnesses += facts.declaration_witness_count;
    }
    if (facts.resource_declaration_kind != HLSL_SOURCE_RESOURCE_NONE)
        ++counts->resource_declarations;
    if (facts.cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_BEGIN)
        ++counts->cbuffer_declarations;
    if (facts.cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_FIELD)
        ++counts->cbuffer_fields;
    HLSLSourceQualityObservation observation = {
        .stage = analysis->request->stage,
        .pass_index = analysis->request->pass_index,
        .entry_point_index = analysis->request->entry_point_index,
        .source_unit_id = analysis->unit->source_unit_id,
        .unit_kind = analysis->unit->kind,
        .kind = kind,
        .ast_kind = ast_kind,
        .reasons = reasons,
        .facts = facts};
    analysis->result->reasons |= reasons;
    if (reasons && !analysis->result->has_first_issue) {
        analysis->result->has_first_issue = true;
        analysis->result->first_issue = observation;
    }
    if (analysis->request->observer &&
        !analysis->request->observer(analysis->request->observer_context, &observation))
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    return true;
}

static bool unary_valid(int opcode) {
    return opcode == USIL_OP_INEG || opcode == USIL_OP_NOT;
}

static bool binary_valid(int opcode) {
    switch (opcode) {
    case USIL_OP_ADD:
    case USIL_OP_SUB:
    case USIL_OP_MUL:
    case USIL_OP_DIV:
    case USIL_OP_IADD:
    case USIL_OP_AND:
    case USIL_OP_OR:
    case USIL_OP_XOR:
    case USIL_OP_ISHL:
    case USIL_OP_ISHR:
    case USIL_OP_USHR:
        return true;
    default:
        return false;
    }
}

static bool scalar_valid(ASTScalarType type) {
    return type == AST_SCALAR_FLOAT32 || type == AST_SCALAR_SINT32 ||
           type == AST_SCALAR_UINT32;
}

static bool visit_expression(QualityAnalysis *analysis, const ASTExpr *expression,
                             unsigned depth, ExpressionValue *value) {
    if (!enter_node(analysis, expression, depth))
        return false;
    ++analysis->result->counts.ast_expressions;
    HLSLSourceQualityFacts facts;
    hlsl_source_quality_facts_init(&facts);
    if (analysis->request->expression_facts &&
        !analysis->request->expression_facts(analysis->request->facts_context,
                                            analysis->unit->source_unit_id, expression, &facts))
        hlsl_source_quality_facts_init(&facts);
    bool children_logical = true;
    ExpressionValue children[3] = {{0}};
    switch (expression->kind) {
    case AST_EXPR_LITERAL:
        if (expression->u.literal.components < 1 || expression->u.literal.components > 4 ||
            !scalar_valid(expression->u.literal.scalar_type))
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        /* Literal encoding is self-describing and cannot hide register syntax.
         * Preserve any resolver-provided ownership while fixing these facts. */
        facts.known = true;
        facts.value_kind = HLSL_SOURCE_VALUE_LOGICAL;
        facts.logical_value_id = 0;
        facts.components = (unsigned)expression->u.literal.components;
        break;
    case AST_EXPR_VAR:
        if (facts.known && facts.value_kind == HLSL_SOURCE_VALUE_UNKNOWN)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        break;
    case AST_EXPR_EMITTER_OPERAND:
        if (!expression->u.emitter_operand || !expression->u.emitter_operand[0])
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        break;
    case AST_EXPR_UNARY:
        if (!unary_valid(expression->u.unary.op))
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, expression->u.unary.sub, depth + 1, &children[0]))
            return false;
        children_logical = children[0].logical;
        break;
    case AST_EXPR_BINARY:
        if (!binary_valid(expression->u.binary.op) ||
            expression->u.binary.left == expression->u.binary.right)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, expression->u.binary.left, depth + 1, &children[0]) ||
            !visit_expression(analysis, expression->u.binary.right, depth + 1, &children[1]))
            return false;
        children_logical = children[0].logical && children[1].logical;
        break;
    case AST_EXPR_COMPARISON:
        if ((expression->u.binary.op != USIL_OP_GE && expression->u.binary.op != USIL_OP_IGE &&
             expression->u.binary.op != USIL_OP_UGE && expression->u.binary.op != USIL_OP_LT &&
             expression->u.binary.op != USIL_OP_ILT && expression->u.binary.op != USIL_OP_ULT &&
             expression->u.binary.op != USIL_OP_EQ && expression->u.binary.op != USIL_OP_IEQ &&
             expression->u.binary.op != USIL_OP_NE && expression->u.binary.op != USIL_OP_INE) ||
            expression->u.binary.left == expression->u.binary.right)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, expression->u.binary.left, depth + 1, &children[0]) ||
            !visit_expression(analysis, expression->u.binary.right, depth + 1, &children[1]))
            return false;
        children_logical = children[0].logical && children[1].logical;
        if (facts.known && (facts.components != 1 || children[0].components != 1 ||
                            children[1].components != 1))
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        break;
    case AST_EXPR_TERNARY:
        if (expression->u.ternary.cond == expression->u.ternary.true_expr ||
            expression->u.ternary.cond == expression->u.ternary.false_expr ||
            expression->u.ternary.true_expr == expression->u.ternary.false_expr)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, expression->u.ternary.cond, depth + 1, &children[0]) ||
            !visit_expression(analysis, expression->u.ternary.true_expr, depth + 1, &children[1]) ||
            !visit_expression(analysis, expression->u.ternary.false_expr, depth + 1, &children[2]))
            return false;
        children_logical = children[0].logical && children[1].logical && children[2].logical;
        break;
    case AST_EXPR_SWIZZLE:
        if (expression->u.swizzle.swizzle_count < 1 || expression->u.swizzle.swizzle_count > 4)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        for (int index = 0; index < expression->u.swizzle.swizzle_count; ++index)
            if (expression->u.swizzle.swizzle[index] < 0 ||
                expression->u.swizzle.swizzle[index] > 3)
                return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, expression->u.swizzle.sub, depth + 1, &children[0]))
            return false;
        children_logical = children[0].logical;
        if (facts.known && facts.semantic_projection) {
            if (!children[0].logical || !children[0].components ||
                facts.components != (unsigned)expression->u.swizzle.swizzle_count)
                return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
            for (int index = 0; index < expression->u.swizzle.swizzle_count; ++index)
                if ((unsigned)expression->u.swizzle.swizzle[index] >= children[0].components)
                    return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        } else if (facts.known &&
                   !(facts.artifacts & HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT)) {
            /* A resolved swizzle must state its purpose explicitly. */
            facts.known = false;
        }
        break;
    case AST_EXPR_CALL:
        if (!expression->u.call.name || !expression->u.call.name[0] ||
            expression->u.call.arg_count < 0 ||
            (expression->u.call.arg_count && !expression->u.call.args))
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        for (int index = 0; index < expression->u.call.arg_count; ++index) {
            if (!visit_expression(analysis, expression->u.call.args[index], depth + 1, &children[0]))
                return false;
            children_logical = children_logical && children[0].logical;
        }
        break;
    case AST_EXPR_CAST:
        if (!expression->u.cast.type_name || !expression->u.cast.type_name[0])
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, expression->u.cast.sub, depth + 1, &children[0]))
            return false;
        children_logical = children[0].logical;
        break;
    case AST_EXPR_BITCAST:
        if (!scalar_valid(expression->u.bitcast.scalar_type))
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, expression->u.bitcast.sub, depth + 1, &children[0]))
            return false;
        children_logical = children[0].logical;
        if (facts.known && !facts.real_bitcast &&
            !(facts.artifacts & HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST))
            facts.known = false;
        break;
    default:
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    }
    if (facts.semantic_projection && expression->kind != AST_EXPR_SWIZZLE &&
        expression->kind != AST_EXPR_EMITTER_OPERAND)
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    if (facts.real_bitcast && expression->kind != AST_EXPR_BITCAST &&
        expression->kind != AST_EXPR_EMITTER_OPERAND)
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    value->logical = facts.known && facts.value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
                     !facts.artifacts && children_logical;
    value->components = facts.components;
    return observe(analysis, HLSL_SOURCE_OBSERVATION_EXPRESSION, expression->kind, facts,
                   facts.logical_operation && facts.value_kind == HLSL_SOURCE_VALUE_LOGICAL);
}

static bool visit_statement(QualityAnalysis *analysis, const ASTStmt *statement, unsigned depth) {
    if (!enter_node(analysis, statement, depth))
        return false;
    ++analysis->result->counts.ast_statements;
    HLSLSourceQualityFacts facts;
    hlsl_source_quality_facts_init(&facts);
    if (analysis->request->statement_facts &&
        !analysis->request->statement_facts(analysis->request->facts_context,
                                           analysis->unit->source_unit_id, statement, &facts))
        hlsl_source_quality_facts_init(&facts);
    ExpressionValue children[2] = {{0}};
    bool children_logical = true;
    switch (statement->kind) {
    case AST_STMT_BLOCK:
        if (statement->u.block.stmt_count < 0 ||
            (statement->u.block.stmt_count && !statement->u.block.stmts))
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        for (int index = 0; index < statement->u.block.stmt_count; ++index)
            if (!visit_statement(analysis, statement->u.block.stmts[index], depth + 1))
                return false;
        /* Block braces and flow-only syntax carry no hidden value machinery. */
        if (!analysis->request->statement_facts)
            facts.known = true;
        break;
    case AST_STMT_ASSIGN:
        if (statement->u.assign.dest == statement->u.assign.src)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (!visit_expression(analysis, statement->u.assign.dest, depth + 1, &children[0]) ||
            !visit_expression(analysis, statement->u.assign.src, depth + 1, &children[1]))
            return false;
        children_logical = children[0].logical && children[1].logical;
        break;
    case AST_STMT_IF:
        if (statement->u.if_stmt.true_body == statement->u.if_stmt.false_body ||
            !visit_expression(analysis, statement->u.if_stmt.cond, depth + 1, &children[0]) ||
            !visit_statement(analysis, statement->u.if_stmt.true_body, depth + 1))
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (statement->u.if_stmt.false_body &&
            !visit_statement(analysis, statement->u.if_stmt.false_body, depth + 1))
            return false;
        children_logical = children[0].logical;
        break;
    case AST_STMT_LOOP:
        if (statement->u.loop.cond &&
            !visit_expression(analysis, statement->u.loop.cond, depth + 1, &children[0]))
            return false;
        if (!visit_statement(analysis, statement->u.loop.body, depth + 1))
            return false;
        children_logical = !statement->u.loop.cond || children[0].logical;
        break;
    case AST_STMT_BREAK:
    case AST_STMT_CONTINUE:
    case AST_STMT_RETURN:
    case AST_STMT_DISCARD:
        if (!analysis->request->statement_facts)
            facts.known = true;
        break;
    default:
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    }
    if (facts.semantic_projection || facts.real_bitcast ||
        facts.value_kind != HLSL_SOURCE_VALUE_UNKNOWN)
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    return observe(analysis, HLSL_SOURCE_OBSERVATION_STATEMENT, statement->kind, facts,
                   facts.logical_operation && children_logical);
}

static void classify_result(HLSLSourceQualityResult *result) {
    if (result->emission_status == HLSL_EMIT_STATUS_UNSUPPORTED) {
        result->classification = HLSL_SOURCE_QUALITY_UNSUPPORTED;
        result->reasons |= HLSL_SOURCE_QUALITY_REASON_EMISSION_UNSUPPORTED;
    } else if (result->emission_status != HLSL_EMIT_STATUS_OK) {
        result->classification = HLSL_SOURCE_QUALITY_FAILED;
        result->reasons |= HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED;
    } else if (result->counts.residual_total || result->counts.unknown_provenance ||
               result->counts.incomplete_units) {
        if (result->counts.logical_operations)
            result->classification = HLSL_SOURCE_QUALITY_MIXED;
        else if (result->counts.residual_total)
            result->classification = HLSL_SOURCE_QUALITY_LOW_LEVEL;
        else
            result->classification = HLSL_SOURCE_QUALITY_UNSUPPORTED;
    } else {
        result->classification = HLSL_SOURCE_QUALITY_CLEAN;
    }
}

static bool record_incomplete_coverage(QualityAnalysis *analysis,
                                       const HLSLSourceQualityUnit *unit) {
    ++analysis->result->counts.incomplete_units;
    analysis->result->reasons |= HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE;
    HLSLSourceQualityObservation observation = {
        .stage = analysis->request->stage,
        .pass_index = analysis->request->pass_index,
        .entry_point_index = analysis->request->entry_point_index,
        .source_unit_id = unit->source_unit_id,
        .unit_kind = unit->kind,
        .kind = HLSL_SOURCE_OBSERVATION_COVERAGE,
        .ast_kind = -1,
        .reasons = HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE};
    hlsl_source_quality_facts_init(&observation.facts);
    if (!analysis->result->has_first_issue) {
        analysis->result->has_first_issue = true;
        analysis->result->first_issue = observation;
    }
    if (analysis->request->observer &&
        !analysis->request->observer(analysis->request->observer_context, &observation))
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    return true;
}

static bool inspect_unit_header(QualityAnalysis *analysis, const HLSLSourceQualityUnit *unit,
                                 bool allow_empty) {
    analysis->unit = unit;
    ++analysis->result->counts.inspected_units;
    if (unit->kind < HLSL_SOURCE_UNIT_ENTRY_POINT || unit->kind > HLSL_SOURCE_UNIT_REQUIRED_EXTERNAL_DECLARATION ||
        (unit->expression_count && !unit->expressions) ||
        (unit->statement_count && !unit->statements) ||
        (unit->emission_fact_count && !unit->emission_facts))
        return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    if (!unit->coverage_complete ||
        (!allow_empty && !unit->expression_count && !unit->statement_count &&
         !unit->emission_fact_count))
        return record_incomplete_coverage(analysis, unit);
    return true;
}

static bool visit_unit(QualityAnalysis *analysis, const HLSLSourceQualityUnit *unit) {
    if (!inspect_unit_header(analysis, unit, false))
        return false;
    for (size_t index = 0; index < unit->expression_count; ++index) {
        ExpressionValue value;
        if (!visit_expression(analysis, unit->expressions[index], 0, &value))
            return false;
    }
    for (size_t index = 0; index < unit->statement_count; ++index)
        if (!visit_statement(analysis, unit->statements[index], 0))
            return false;
    for (size_t index = 0; index < unit->emission_fact_count; ++index) {
        if (!analysis->remaining)
            return fail_analysis(analysis, HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND);
        --analysis->remaining;
        ++analysis->result->counts.emission_events;
        const HLSLSourceQualityFacts *facts = &unit->emission_facts[index];
        if (!observe(analysis, HLSL_SOURCE_OBSERVATION_EMISSION, -1, *facts,
                     facts->logical_operation))
            return false;
    }
    return true;
}

bool hlsl_source_quality_analyze(const HLSLSourceQualityRequest *request,
                                  HLSLSourceQualityResult *result) {
    if (!result)
        return false;
    memset(result, 0, sizeof(*result));
    result->stage = DXBC_PROGRAM_TYPE_INVALID;
    result->classification = HLSL_SOURCE_QUALITY_FAILED;
    result->emission_status = HLSL_EMIT_STATUS_INVALID_ARGUMENT;
    if (!request) {
        result->reasons = HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT;
        return false;
    }
    result->stage = request->stage;
    result->pass_index = request->pass_index;
    result->entry_point_index = request->entry_point_index;
    result->emission_status = request->emission_status;
    if (request->stage < DXBC_PROGRAM_TYPE_PIXEL || request->stage >= DXBC_PROGRAM_TYPE_COUNT ||
        request->emission_status < HLSL_EMIT_STATUS_OK ||
        request->emission_status > HLSL_EMIT_STATUS_OUTPUT_FAILED ||
        (request->unit_count && !request->units) || request->node_budget > SIZE_MAX / 8u ||
        request->expected_entry_point_count >
            (request->node_budget ? request->node_budget : 1048576u)) {
        result->reasons = HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT;
        return false;
    }
    if (request->emission_status == HLSL_EMIT_STATUS_UNSUPPORTED) {
        result->classification = HLSL_SOURCE_QUALITY_UNSUPPORTED;
        result->reasons = HLSL_SOURCE_QUALITY_REASON_EMISSION_UNSUPPORTED;
        return true;
    }
    if (request->emission_status != HLSL_EMIT_STATUS_OK) {
        result->reasons = HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED;
        return true;
    }
    QualityAnalysis analysis = {
        .request = request,
        .result = result,
        .remaining = request->node_budget ? request->node_budget : 1048576u};
    if (request->unit_count != request->expected_unit_count || !request->unit_count) {
        result->reasons |= HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE;
        ++result->counts.incomplete_units;
    }
    size_t entry_points = 0;
    for (size_t index = 0; index < request->unit_count; ++index) {
        if (!analysis.remaining)
            return fail_analysis(&analysis, HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND);
        --analysis.remaining;
        if (index && request->units[index - 1].source_unit_id >=
                         request->units[index].source_unit_id)
            return fail_analysis(&analysis, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
        if (request->units[index].kind == HLSL_SOURCE_UNIT_ENTRY_POINT)
            ++entry_points;
        if (!visit_unit(&analysis, &request->units[index]))
            return false;
    }
    const size_t expected_entries = request->expected_entry_point_count
                                       ? request->expected_entry_point_count : 1u;
    if (entry_points != expected_entries) {
        result->reasons |= HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE;
        ++result->counts.incomplete_units;
    }
    classify_result(result);
    return true;
}

HLSLSourceQualityAnalysis *hlsl_source_quality_analysis_create(
    const HLSLSourceQualityRequest *request, HLSLSourceQualityResult *result) {
    if (!result)
        return NULL;
    HLSLSourceQualityRequest empty = {0};
    if (request) {
        empty = *request;
        empty.units = NULL;
        empty.unit_count = empty.expected_unit_count = 0;
    }
    if (!hlsl_source_quality_analyze(request ? &empty : NULL, result))
        return NULL;
    HLSLSourceQualityAnalysis *analysis = calloc(1, sizeof(*analysis));
    if (!analysis) {
        result->classification = HLSL_SOURCE_QUALITY_FAILED;
        result->reasons = HLSL_SOURCE_QUALITY_REASON_ALLOCATION_FAILED;
        return NULL;
    }
    /* The empty inventory was only used to validate/init identity. */
    memset(&result->counts, 0, sizeof(result->counts));
    result->reasons = 0;
    result->classification = HLSL_SOURCE_QUALITY_UNSUPPORTED;
    analysis->request = *request;
    analysis->visitor.request = &analysis->request;
    analysis->visitor.result = result;
    analysis->visitor.remaining = request->node_budget ? request->node_budget : 1048576u;
    return analysis;
}

static size_t inspected_nodes(const HLSLSourceQualityResult *result) {
    return result->counts.ast_expressions + result->counts.ast_statements +
           result->counts.emission_events;
}

static bool finish_active_unit(HLSLSourceQualityAnalysis *analysis) {
    if (analysis->active_unit && analysis->unit.coverage_complete &&
        inspected_nodes(analysis->visitor.result) == analysis->unit_start_nodes) {
        return record_incomplete_coverage(&analysis->visitor, &analysis->unit);
    }
    return true;
}

static bool stream_ready(HLSLSourceQualityAnalysis *analysis) {
    if (!analysis || analysis->failed || analysis->finished)
        return false;
    if (!analysis->active_unit) {
        analysis->failed = true;
        return fail_analysis(&analysis->visitor, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    }
    return true;
}

bool hlsl_source_quality_analysis_begin_unit(HLSLSourceQualityAnalysis *analysis,
    uint32_t source_unit_id, HLSLSourceQualityUnitKind kind, bool coverage_complete) {
    if (!analysis || analysis->failed || analysis->finished)
        return false;
    if (analysis->active_unit && source_unit_id <= analysis->unit.source_unit_id) {
        analysis->failed = true;
        return fail_analysis(&analysis->visitor, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    }
    if (!finish_active_unit(analysis)) {
        analysis->failed = true;
        return false;
    }
    analysis->unit = (HLSLSourceQualityUnit){
        .source_unit_id = source_unit_id, .kind = kind, .coverage_complete = coverage_complete};
    analysis->unit_start_nodes = inspected_nodes(analysis->visitor.result);
    analysis->active_unit = true;
    if (kind == HLSL_SOURCE_UNIT_ENTRY_POINT)
        ++analysis->entry_points;
    if (!analysis->visitor.remaining) {
        analysis->failed = true;
        return fail_analysis(&analysis->visitor, HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND);
    }
    --analysis->visitor.remaining;
    bool accepted = inspect_unit_header(&analysis->visitor, &analysis->unit, true);
    analysis->failed = !accepted;
    return accepted;
}

bool hlsl_source_quality_analysis_expression(HLSLSourceQualityAnalysis *analysis,
                                             const ASTExpr *expression) {
    if (!stream_ready(analysis))
        return false;
    ExpressionValue value;
    bool accepted = visit_expression(&analysis->visitor, expression, 0, &value);
    analysis->failed = !accepted;
    return accepted;
}

bool hlsl_source_quality_analysis_statement(HLSLSourceQualityAnalysis *analysis,
                                            const ASTStmt *statement) {
    if (!stream_ready(analysis))
        return false;
    bool accepted = visit_statement(&analysis->visitor, statement, 0);
    analysis->failed = !accepted;
    return accepted;
}

bool hlsl_source_quality_analysis_emission(HLSLSourceQualityAnalysis *analysis,
                                           const HLSLSourceQualityFacts *facts) {
    if (!stream_ready(analysis))
        return false;
    if (!facts || !analysis->visitor.remaining) {
        analysis->failed = true;
        return fail_analysis(&analysis->visitor, facts ? HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND
                                                     : HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    }
    --analysis->visitor.remaining;
    ++analysis->visitor.result->counts.emission_events;
    bool accepted = observe(&analysis->visitor, HLSL_SOURCE_OBSERVATION_EMISSION, -1, *facts,
                            facts->logical_operation);
    analysis->failed = !accepted;
    return accepted;
}

bool hlsl_source_quality_analysis_mark_incomplete_unit(HLSLSourceQualityAnalysis *analysis) {
    if (!stream_ready(analysis))
        return false;
    if (!analysis->unit.coverage_complete)
        return true;
    analysis->unit.coverage_complete = false;
    bool accepted = record_incomplete_coverage(&analysis->visitor, &analysis->unit);
    analysis->failed = !accepted;
    return accepted;
}

bool hlsl_source_quality_analysis_finish(HLSLSourceQualityAnalysis *analysis,
    HLSLEmitStatus emission_status, size_t expected_unit_count) {
    if (!analysis || analysis->finished)
        return false;
    analysis->finished = true;
    HLSLSourceQualityResult *result = analysis->visitor.result;
    result->emission_status = emission_status;
    if (analysis->failed)
        return false;
    if (emission_status < HLSL_EMIT_STATUS_OK || emission_status > HLSL_EMIT_STATUS_OUTPUT_FAILED)
        return fail_analysis(&analysis->visitor, HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    if (!finish_active_unit(analysis)) {
        analysis->failed = true;
        return false;
    }
    const size_t expected_entries = analysis->request.expected_entry_point_count
                                       ? analysis->request.expected_entry_point_count : 1u;
    if (result->counts.inspected_units != expected_unit_count ||
        analysis->entry_points != expected_entries) {
        ++result->counts.incomplete_units;
        result->reasons |= HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE;
    }
    classify_result(result);
    return true;
}

void hlsl_source_quality_analysis_destroy(HLSLSourceQualityAnalysis *analysis) {
    free(analysis);
}

static bool quality_counters_equal(const HLSLSourceQualityCounters *a, const HLSLSourceQualityCounters *b) {
    return a->ast_expressions == b->ast_expressions &&
        a->ast_statements == b->ast_statements &&
        a->emission_events == b->emission_events &&
        a->inspected_units == b->inspected_units &&
        a->incomplete_units == b->incomplete_units &&
        a->unknown_provenance == b->unknown_provenance &&
        a->logical_operations == b->logical_operations &&
        a->logical_value_references == b->logical_value_references &&
        a->semantic_projections == b->semantic_projections &&
        a->real_bitcasts == b->real_bitcasts &&
        a->register_storage == b->register_storage &&
        a->lane_transport == b->lane_transport &&
        a->scalarized_intrinsics == b->scalarized_intrinsics &&
        a->raw_buffer_reconstruction == b->raw_buffer_reconstruction &&
        a->synthetic_interface == b->synthetic_interface &&
        a->instruction_assignments == b->instruction_assignments &&
        a->unstructured_control == b->unstructured_control &&
        a->storage_bitcasts == b->storage_bitcasts &&
        a->sibling_declarations == b->sibling_declarations &&
        a->sibling_declaration_witnesses == b->sibling_declaration_witnesses &&
        a->resource_declarations == b->resource_declarations &&
        a->residual_total == b->residual_total &&
        a->cbuffer_declarations == b->cbuffer_declarations &&
        a->cbuffer_fields == b->cbuffer_fields;
}

bool hlsl_source_quality_facts_equal(const HLSLSourceQualityFacts *a, const HLSLSourceQualityFacts *b) {
    return a->known == b->known &&
        a->value_kind == b->value_kind &&
        a->logical_value_id == b->logical_value_id &&
        a->components == b->components &&
        a->artifacts == b->artifacts &&
        a->semantic_projection == b->semantic_projection &&
        a->real_bitcast == b->real_bitcast &&
        a->logical_operation == b->logical_operation &&
        a->instruction_index == b->instruction_index &&
        a->source_instruction_index == b->source_instruction_index &&
        a->lanes == b->lanes &&
        a->declaration_witness_count == b->declaration_witness_count &&
        a->declaration_variant_index == b->declaration_variant_index &&
        a->declaration_witness_record == b->declaration_witness_record &&
        a->declaration_field_index == b->declaration_field_index &&
        a->declaration_witness_subprogram_index == b->declaration_witness_subprogram_index &&
        a->resource_declaration_kind == b->resource_declaration_kind &&
        a->resource_binding_register == b->resource_binding_register &&
        a->cbuffer_declaration_kind == b->cbuffer_declaration_kind &&
        a->cbuffer_binding_register == b->cbuffer_binding_register &&
        a->cbuffer_field_index == b->cbuffer_field_index &&
        a->cbuffer_byte_offset == b->cbuffer_byte_offset &&
        a->cbuffer_byte_size == b->cbuffer_byte_size &&
        a->cbuffer_declaration_authority == b->cbuffer_declaration_authority;
}

static bool quality_observations_equal(const HLSLSourceQualityObservation *a, const HLSLSourceQualityObservation *b) {
    return a->stage == b->stage &&
        a->pass_index == b->pass_index &&
        a->entry_point_index == b->entry_point_index &&
        a->source_unit_id == b->source_unit_id &&
        a->unit_kind == b->unit_kind &&
        a->kind == b->kind &&
        a->ast_kind == b->ast_kind &&
        a->reasons == b->reasons &&
        hlsl_source_quality_facts_equal(&a->facts, &b->facts);
}

bool hlsl_source_quality_results_equal(const HLSLSourceQualityResult *a, const HLSLSourceQualityResult *b) {
    return a->stage == b->stage &&
        a->pass_index == b->pass_index &&
        a->entry_point_index == b->entry_point_index &&
        a->emission_status == b->emission_status &&
        a->classification == b->classification &&
        a->reasons == b->reasons &&
        a->has_first_issue == b->has_first_issue &&
        quality_counters_equal(&a->counts, &b->counts) &&
        (!a->has_first_issue || quality_observations_equal(&a->first_issue, &b->first_issue));
}
