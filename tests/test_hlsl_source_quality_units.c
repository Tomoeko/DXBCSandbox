// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_source_quality.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/usil.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #value);            \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    const ASTExpr *register_value;
    const ASTExpr *unknown_value;
    const ASTExpr *artifact_value;
    uint32_t extra_artifacts;
    bool projections_are_semantic;
    bool bitcast_is_real;
    bool instruction_assignments;
    bool reject_observer;
    unsigned variable_components;
    size_t observations;
    HLSLSourceQualityObservation issue;
} QualityFixture;

/* Test authority comes from node identity and explicit provenance roles; the
 * resolver does not inspect identifiers, call names or formatted source. */
static bool expression_facts(void *context, uint32_t unit_id, const ASTExpr *expression,
                             HLSLSourceQualityFacts *facts) {
    QualityFixture *fixture = context;
    (void)unit_id;
    if (expression == fixture->unknown_value)
        return false;
    facts->known = true;
    facts->value_kind = expression == fixture->register_value ? HLSL_SOURCE_VALUE_REGISTER
                                                               : HLSL_SOURCE_VALUE_LOGICAL;
    facts->logical_value_id = (uint64_t)(uintptr_t)expression;
    facts->components = fixture->variable_components ? fixture->variable_components : 4;
    facts->instruction_index = 7;
    facts->source_instruction_index = 19;
    facts->lanes = 15;
    facts->logical_operation = expression->kind != AST_EXPR_VAR &&
                               expression->kind != AST_EXPR_LITERAL;
    if (expression->kind == AST_EXPR_SWIZZLE) {
        facts->components = (unsigned)expression->u.swizzle.swizzle_count;
        if (fixture->projections_are_semantic)
            facts->semantic_projection = true;
        else
            facts->artifacts |= HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT;
    }
    if (expression->kind == AST_EXPR_BITCAST) {
        if (fixture->bitcast_is_real)
            facts->real_bitcast = true;
        else
            facts->artifacts |= HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST;
    }
    if (expression == fixture->artifact_value)
        facts->artifacts |= fixture->extra_artifacts;
    return true;
}

static bool statement_facts(void *context, uint32_t unit_id, const ASTStmt *statement,
                            HLSLSourceQualityFacts *facts) {
    QualityFixture *fixture = context;
    (void)unit_id;
    facts->known = true;
    facts->instruction_index = 8;
    facts->source_instruction_index = 20;
    if (statement->kind == AST_STMT_ASSIGN) {
        if (fixture->instruction_assignments)
            facts->artifacts = HLSL_SOURCE_ARTIFACT_INSTRUCTION_ASSIGNMENT;
        else
            facts->logical_operation = true;
    } else if (statement->kind == AST_STMT_IF || statement->kind == AST_STMT_LOOP) {
        facts->logical_operation = true;
    }
    return true;
}

static bool observation(void *context, const HLSLSourceQualityObservation *value) {
    QualityFixture *fixture = context;
    ++fixture->observations;
    if (value->reasons)
        fixture->issue = *value;
    return !fixture->reject_observer;
}

static HLSLSourceQualityRequest make_request(HLSLSourceQualityUnit *unit,
                                             QualityFixture *fixture) {
    HLSLSourceQualityRequest request = {
        .stage = DXBC_PROGRAM_TYPE_PIXEL,
        .pass_index = 12,
        .entry_point_index = 3,
        .emission_status = HLSL_EMIT_STATUS_OK,
        .units = unit,
        .unit_count = 1,
        .expected_unit_count = 1,
        .expression_facts = expression_facts,
        .statement_facts = statement_facts,
        .facts_context = fixture,
        .observer = observation,
        .observer_context = fixture};
    return request;
}

static bool check_semantic_projection_and_names(void) {
    const int projection[] = {1, 2};
    ASTExpr *base = ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "r0");
    ASTExpr *value = ast_create_swizzle(base, projection, 2);
    CHECK(value);
    const ASTExpr *expressions[] = {value};
    HLSLSourceQualityUnit unit = {
        .source_unit_id = 1,
        .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
        .coverage_complete = true,
        .expressions = expressions,
        .expression_count = 1};
    QualityFixture fixture = {.projections_are_semantic = true, .variable_components = 3};
    HLSLSourceQualityRequest request = make_request(&unit, &fixture);
    HLSLSourceQualityResult result;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_CLEAN && !result.reasons);
    CHECK(result.counts.semantic_projections == 1 && result.counts.lane_transport == 0 &&
          result.counts.residual_total == 0 && result.counts.ast_expressions == 2 &&
          result.counts.logical_operations == 1);
    CHECK(result.stage == DXBC_PROGRAM_TYPE_PIXEL && result.pass_index == 12 &&
          result.entry_point_index == 3 && result.emission_status == HLSL_EMIT_STATUS_OK);
    CHECK(fixture.observations == 2);

    fixture.projections_are_semantic = false;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_LOW_LEVEL &&
          result.counts.semantic_projections == 0 && result.counts.lane_transport == 1);
    CHECK(result.has_first_issue && result.first_issue.facts.instruction_index == 7 &&
          result.first_issue.facts.source_instruction_index == 19 &&
          result.first_issue.facts.lanes == 15 && result.first_issue.source_unit_id == 1 &&
          result.first_issue.stage == DXBC_PROGRAM_TYPE_PIXEL &&
          result.first_issue.pass_index == 12 && result.first_issue.entry_point_index == 3);

    fixture.register_value = base;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.counts.register_storage == 1 && result.counts.residual_total == 2);
    ast_free_expr(value);

    /* A flattering identifier cannot remove its emitted storage provenance. */
    value = ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "objectPosition");
    CHECK(value);
    expressions[0] = value;
    fixture.register_value = value;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_LOW_LEVEL &&
          result.counts.register_storage == 1);
    ast_free_expr(value);
    return true;
}

static bool check_bitcast_and_opaque_boundary(void) {
    ASTExpr *base = ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "bits");
    ASTExpr *value = ast_create_bitcast(AST_SCALAR_FLOAT32, base);
    CHECK(value);
    const ASTExpr *expressions[] = {value};
    HLSLSourceQualityUnit unit = {
        .source_unit_id = 4,
        .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
        .coverage_complete = true,
        .expressions = expressions,
        .expression_count = 1};
    QualityFixture fixture = {.bitcast_is_real = true};
    HLSLSourceQualityRequest request = make_request(&unit, &fixture);
    HLSLSourceQualityResult result;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_CLEAN && result.counts.real_bitcasts == 1);
    fixture.bitcast_is_real = false;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_LOW_LEVEL &&
          result.counts.real_bitcasts == 0 && result.counts.storage_bitcasts == 1);
    ast_free_expr(value);

    value = ast_create_emitter_operand("input.coords.yz");
    CHECK(value);
    expressions[0] = value;
    fixture.unknown_value = value;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED &&
          result.counts.unknown_provenance == 1 && result.counts.semantic_projections == 0);
    CHECK(result.reasons & HLSL_SOURCE_QUALITY_REASON_UNKNOWN_PROVENANCE);
    fixture.unknown_value = NULL;
    fixture.artifact_value = value;
    fixture.extra_artifacts = HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT |
                              HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_LOW_LEVEL &&
          result.counts.residual_total == 2);
    ast_free_expr(value);
    return true;
}

static bool check_hidden_helpers_and_categories(void) {
    ASTExpr *argument = ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "coordinate");
    ASTExpr *call = ast_create_call("project", &argument, 1);
    ASTExpr *helper = ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "prettyHelperLocal");
    CHECK(call && helper);
    const ASTExpr *entry_roots[] = {call}, *helper_roots[] = {helper};
    HLSLSourceQualityFacts facts[7];
    const uint32_t categories[] = {
        HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT,
        HLSL_SOURCE_ARTIFACT_SCALARIZED_INTRINSIC,
        HLSL_SOURCE_ARTIFACT_RAW_BUFFER_RECONSTRUCTION,
        HLSL_SOURCE_ARTIFACT_SYNTHETIC_INTERFACE,
        HLSL_SOURCE_ARTIFACT_INSTRUCTION_ASSIGNMENT,
        HLSL_SOURCE_ARTIFACT_UNSTRUCTURED_CONTROL,
        HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST};
    for (size_t index = 0; index < 7; ++index) {
        hlsl_source_quality_facts_init(&facts[index]);
        facts[index].known = true;
        facts[index].artifacts = categories[index];
    }
    HLSLSourceQualityUnit units[] = {
        {.source_unit_id = 1,
         .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
         .coverage_complete = true,
         .expressions = entry_roots,
         .expression_count = 1},
        {.source_unit_id = 2,
         .kind = HLSL_SOURCE_UNIT_HELPER,
         .coverage_complete = true,
         .expressions = helper_roots,
         .expression_count = 1,
         .emission_facts = facts,
         .emission_fact_count = 7}};
    QualityFixture fixture = {.register_value = helper};
    HLSLSourceQualityRequest request = make_request(units, &fixture);
    request.unit_count = request.expected_unit_count = 2;
    HLSLSourceQualityResult result;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_MIXED && result.counts.residual_total == 8 &&
          result.counts.logical_operations == 1 && result.counts.inspected_units == 2 &&
          result.counts.emission_events == 7);
    CHECK(result.counts.register_storage == 1 && result.counts.lane_transport == 1 &&
          result.counts.scalarized_intrinsics == 1 && result.counts.raw_buffer_reconstruction == 1 &&
          result.counts.synthetic_interface == 1 && result.counts.instruction_assignments == 1 &&
          result.counts.unstructured_control == 1 && result.counts.storage_bitcasts == 1);

    /* Omitting an inventoried include cannot hide its machinery. */
    request.expected_unit_count = 3;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_MIXED && result.counts.incomplete_units == 1);
    request.expected_unit_count = 2;
    units[1].kind = HLSL_SOURCE_UNIT_EXTERNAL_INCLUDE;
    units[1].coverage_complete = false;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.counts.incomplete_units == 1 &&
          (result.reasons & HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE));
    units[1].kind = HLSL_SOURCE_UNIT_GENERATED_INCLUDE;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.counts.residual_total == 8);
    ast_free_expr(call);
    ast_free_expr(helper);
    return true;
}

static bool check_statements_and_emission_outcomes(void) {
    ASTStmt *assign = ast_create_assign(ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "result"),
                                        ast_create_literal_int(1));
    ASTStmt *body = ast_create_block();
    CHECK(assign && body && ast_block_add(body, assign));
    CHECK(ast_block_add(body, ast_create_flow(AST_STMT_RETURN)));
    const ASTStmt *statements[] = {body};
    HLSLSourceQualityUnit unit = {
        .source_unit_id = 1,
        .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
        .coverage_complete = true,
        .statements = statements,
        .statement_count = 1};
    QualityFixture fixture = {0};
    HLSLSourceQualityRequest request = make_request(&unit, &fixture);
    HLSLSourceQualityResult result;
    for (int stage = DXBC_PROGRAM_TYPE_PIXEL; stage < DXBC_PROGRAM_TYPE_COUNT; ++stage) {
        request.stage = (DXBCProgramType)stage;
        CHECK(hlsl_source_quality_analyze(&request, &result));
        CHECK(result.classification == HLSL_SOURCE_QUALITY_CLEAN && result.stage == (DXBCProgramType)stage);
        CHECK(result.counts.ast_statements == 3 && result.counts.ast_expressions == 2 &&
              result.counts.logical_operations == 1);
    }
    fixture.instruction_assignments = true;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_LOW_LEVEL &&
          result.counts.instruction_assignments == 1);

    request.emission_status = HLSL_EMIT_STATUS_UNSUPPORTED;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED &&
          result.emission_status == HLSL_EMIT_STATUS_UNSUPPORTED && !result.counts.ast_expressions &&
          result.reasons == HLSL_SOURCE_QUALITY_REASON_EMISSION_UNSUPPORTED);
    request.emission_status = HLSL_EMIT_STATUS_ALLOCATION_FAILED;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_FAILED &&
          result.reasons == HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED);
    ast_free_stmt(body);
    return true;
}

static bool check_invalid_and_bounded_analysis(void) {
    ASTExpr *value = ast_create_literal_int(1);
    CHECK(value);
    const ASTExpr *expressions[] = {value};
    HLSLSourceQualityUnit unit = {
        .source_unit_id = 1,
        .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
        .coverage_complete = true,
        .expressions = expressions,
        .expression_count = 1};
    QualityFixture fixture = {0};
    HLSLSourceQualityRequest request = make_request(&unit, &fixture);
    HLSLSourceQualityResult result;
    CHECK(!hlsl_source_quality_analyze(NULL, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_FAILED &&
          result.reasons == HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    CHECK(!hlsl_source_quality_analyze(&request, NULL));
    request.node_budget = 1;
    CHECK(!hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_FAILED &&
          (result.reasons & HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND));
    request.node_budget = 0;
    fixture.reject_observer = true;
    CHECK(!hlsl_source_quality_analyze(&request, &result));
    fixture.reject_observer = false;

    ASTExpr malformed = {.kind = AST_EXPR_SWIZZLE};
    malformed.u.swizzle.sub = &malformed;
    malformed.u.swizzle.swizzle_count = 1;
    expressions[0] = &malformed;
    CHECK(!hlsl_source_quality_analyze(&request, &result));
    CHECK(result.reasons & HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    malformed.kind = (ASTExprKind)99;
    CHECK(!hlsl_source_quality_analyze(&request, &result));
    expressions[0] = value;
    unit.expression_count = 0;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED &&
          result.counts.incomplete_units == 1);
    unit.expression_count = 1;

    const int invalid_projection[] = {3};
    ASTExpr *projected = ast_create_swizzle(
        ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "normal"), invalid_projection, 1);
    CHECK(projected);
    expressions[0] = projected;
    fixture.projections_are_semantic = true;
    fixture.variable_components = 3;
    CHECK(!hlsl_source_quality_analyze(&request, &result));
    CHECK(result.reasons & HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    ast_free_expr(projected);

    ASTExpr *deep = ast_create_literal_int(1);
    CHECK(deep);
    for (int index = 0; index < 258; ++index) {
        deep = ast_create_unary(USIL_OP_INEG, deep);
        CHECK(deep);
    }
    expressions[0] = deep;
    CHECK(!hlsl_source_quality_analyze(&request, &result));
    CHECK(result.reasons & HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND);
    ast_free_expr(deep);
    ast_free_expr(value);
    return true;
}

static bool check_streaming_matches_batch(void) {
    ASTExpr *argument = ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "value");
    ASTExpr *call = ast_create_call("operation", &argument, 1);
    CHECK(call);
    const ASTExpr *expressions[] = {call};
    HLSLSourceQualityUnit unit = {
        .source_unit_id = 1, .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
        .coverage_complete = true, .expressions = expressions, .expression_count = 1};
    QualityFixture fixture = {0};
    HLSLSourceQualityRequest request = make_request(&unit, &fixture);
    HLSLSourceQualityResult batch, streaming;
    CHECK(hlsl_source_quality_analyze(&request, &batch));
    size_t batch_observations = fixture.observations;
    fixture.observations = 0;
    HLSLSourceQualityAnalysis *analysis = hlsl_source_quality_analysis_create(&request, &streaming);
    CHECK(analysis);
    CHECK(hlsl_source_quality_analysis_begin_unit(analysis, 1, HLSL_SOURCE_UNIT_ENTRY_POINT, true));
    CHECK(hlsl_source_quality_analysis_expression(analysis, call));
    CHECK(hlsl_source_quality_analysis_finish(analysis, HLSL_EMIT_STATUS_OK, 1));
    CHECK(!hlsl_source_quality_analysis_expression(analysis, call));
    CHECK(memcmp(&batch.counts, &streaming.counts, sizeof(batch.counts)) == 0);
    CHECK(batch.classification == streaming.classification && batch.reasons == streaming.reasons &&
          fixture.observations == batch_observations);
    hlsl_source_quality_analysis_destroy(analysis);

    analysis = hlsl_source_quality_analysis_create(&request, &streaming);
    CHECK(analysis);
    CHECK(!hlsl_source_quality_analysis_expression(analysis, call));
    CHECK(!hlsl_source_quality_analysis_begin_unit(analysis, 1, HLSL_SOURCE_UNIT_ENTRY_POINT, true));
    CHECK(!hlsl_source_quality_analysis_finish(analysis, HLSL_EMIT_STATUS_OK, 1));
    CHECK(streaming.classification == HLSL_SOURCE_QUALITY_FAILED &&
          streaming.reasons == HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    hlsl_source_quality_analysis_destroy(analysis);

    analysis = hlsl_source_quality_analysis_create(&request, &streaming);
    CHECK(analysis);
    CHECK(hlsl_source_quality_analysis_begin_unit(analysis, 1, HLSL_SOURCE_UNIT_ENTRY_POINT, true));
    CHECK(hlsl_source_quality_analysis_finish(analysis, HLSL_EMIT_STATUS_OK, 1));
    CHECK(streaming.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED &&
          streaming.counts.incomplete_units == 1 && streaming.has_first_issue &&
          streaming.first_issue.kind == HLSL_SOURCE_OBSERVATION_COVERAGE &&
          streaming.first_issue.source_unit_id == 1);
    hlsl_source_quality_analysis_destroy(analysis);

    /* A planned complete unit may discover a missing actual syntax span.
     * Downgrade that unit once, retaining its identity and observed facts. */
    analysis = hlsl_source_quality_analysis_create(&request, &streaming);
    CHECK(analysis);
    CHECK(hlsl_source_quality_analysis_begin_unit(analysis, 1, HLSL_SOURCE_UNIT_ENTRY_POINT, true));
    CHECK(hlsl_source_quality_analysis_expression(analysis, call));
    CHECK(hlsl_source_quality_analysis_mark_incomplete_unit(analysis));
    CHECK(hlsl_source_quality_analysis_mark_incomplete_unit(analysis));
    CHECK(hlsl_source_quality_analysis_finish(analysis, HLSL_EMIT_STATUS_OK, 1));
    CHECK(streaming.classification == HLSL_SOURCE_QUALITY_MIXED &&
          streaming.counts.inspected_units == 1 && streaming.counts.incomplete_units == 1 &&
          streaming.counts.ast_expressions == batch.counts.ast_expressions &&
          streaming.first_issue.kind == HLSL_SOURCE_OBSERVATION_COVERAGE &&
          streaming.first_issue.unit_kind == HLSL_SOURCE_UNIT_ENTRY_POINT &&
          streaming.first_issue.source_unit_id == 1);
    CHECK(!hlsl_source_quality_analysis_mark_incomplete_unit(analysis));
    hlsl_source_quality_analysis_destroy(analysis);

    analysis = hlsl_source_quality_analysis_create(&request, &streaming);
    CHECK(analysis);
    CHECK(!hlsl_source_quality_analysis_mark_incomplete_unit(analysis));
    CHECK(!hlsl_source_quality_analysis_finish(analysis, HLSL_EMIT_STATUS_OK, 1));
    CHECK(streaming.classification == HLSL_SOURCE_QUALITY_FAILED &&
          streaming.reasons == HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT);
    hlsl_source_quality_analysis_destroy(analysis);
    ast_free_expr(call);
    return true;
}

static DXBCOperand emitted_operand(DXBCOperandType type, int index, uint8_t lanes) {
    DXBCOperand value = {0};
    value.type = type;
    value.register_index = index;
    value.register_index_dim = 1;
    value.index_has_immediate[0] = true;
    value.index_values[0] = (uint32_t)index;
    value.destination_mask = lanes << 4;
    value.swizzle_mode = 1;
    for (uint8_t lane = 0; lane < 4; ++lane)
        value.swizzle[lane] = lane;
    return value;
}

static bool check_real_emitter_quality_is_independent(void) {
    USILInstruction instructions[4] = {0};
    instructions[0].opcode = USIL_OP_MUL;
    instructions[0].operand_count = 3;
    instructions[0].operands[0] = emitted_operand(OPERAND_TYPE_TEMP, 0, 15);
    instructions[0].operands[1] = emitted_operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[0].operands[2] = emitted_operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[1].opcode = USIL_OP_ADD;
    instructions[1].operand_count = 3;
    instructions[1].operands[0] = emitted_operand(OPERAND_TYPE_TEMP, 1, 15);
    instructions[1].operands[1] = emitted_operand(OPERAND_TYPE_TEMP, 0, 0);
    instructions[1].operands[2] = emitted_operand(OPERAND_TYPE_TEMP, 0, 0);
    instructions[2].opcode = USIL_OP_MOV;
    instructions[2].operand_count = 2;
    instructions[2].operands[0] = emitted_operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[2].operands[1] = emitted_operand(OPERAND_TYPE_TEMP, 1, 0);
    instructions[3].opcode = USIL_OP_RET;
    for (int index = 0; index < 4; ++index)
        instructions[index].source_instruction_index = (uint32_t)index + 100;
    DXBCSignatureElement input = {
        .semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15};
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram program = {
        .instructions = instructions, .instruction_count = 4, .instruction_alloc = 4,
        .temp_count = 2, .inputs = &input, .input_count = 1, .input_alloc = 1,
        .outputs = &output, .output_count = 1, .output_alloc = 1,
        .has_stage_contract = true, .program_type = DXBC_PROGRAM_TYPE_PIXEL,
        .shader_model_major = 5};
    memcpy(program.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
    for (int mode = HLSL_EMIT_MODE_RECOMPILE; mode <= HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE; ++mode) {
        HLSLEmitOptions options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
        options.mode = (HLSLEmitMode)mode;
        StringBuilder baseline, with_quality;
        sb_init(&baseline);
        sb_init(&with_quality);
        CHECK(hlsl_emit_with_options(&program, &baseline, NULL, NULL, NULL, &options));
        HLSLSourceQualityResult quality;
        QualityFixture ledger = {0};
        options.source_quality = &quality;
        options.source_quality_observer = observation;
        options.source_quality_observer_context = &ledger;
        options.source_quality_pass_index = 8;
        options.source_quality_entry_point_index = 2;
        CHECK(hlsl_emit_with_options(&program, &with_quality, NULL, NULL, NULL, &options));
        CHECK(strcmp(baseline.buf, with_quality.buf) == 0);
        CHECK(quality.stage == DXBC_PROGRAM_TYPE_PIXEL && quality.pass_index == 8 &&
              quality.entry_point_index == 2 && quality.emission_status == HLSL_EMIT_STATUS_OK);
        CHECK(ledger.observations == quality.counts.ast_expressions + quality.counts.ast_statements +
              quality.counts.emission_events + quality.counts.incomplete_units);
        CHECK(quality.counts.incomplete_units ==
              (mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ? 0u : 1u));
        CHECK(quality.classification == (mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE
                                           ? HLSL_SOURCE_QUALITY_CLEAN
                                           : HLSL_SOURCE_QUALITY_LOW_LEVEL));
        if (mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) {
            CHECK(quality.counts.ast_expressions > 0 && quality.counts.logical_operations > 0 &&
                  quality.counts.register_storage == 0 && quality.counts.synthetic_interface == 0 &&
                  quality.counts.lane_transport == 0 && quality.counts.unknown_provenance == 0 &&
                  quality.counts.residual_total == 0);
        } else {
            CHECK(quality.counts.register_storage >= 2 && quality.counts.synthetic_interface == 1 &&
                  quality.counts.lane_transport >= 1);
            CHECK(quality.counts.instruction_assignments == 3);
        }
        sb_free(&baseline);
        sb_free(&with_quality);
    }
    HLSLSourceQualityResult quality;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.source_quality = &quality;
    options.source_quality_pass_index = 9;
    instructions[0].precise_mask = 1;
    StringBuilder failed;
    sb_init(&failed);
    CHECK(!hlsl_emit_with_options(&program, &failed, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED &&
          quality.emission_status == HLSL_EMIT_STATUS_UNSUPPORTED && quality.pass_index == 9);
    sb_free(&failed);
    CHECK(!hlsl_emit_with_options(NULL, NULL, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
          quality.emission_status == HLSL_EMIT_STATUS_INVALID_ARGUMENT &&
          quality.stage == DXBC_PROGRAM_TYPE_INVALID);
    instructions[0].precise_mask = 0;
    QualityFixture rejected = {.reject_observer = true};
    options.source_quality_observer = observation;
    options.source_quality_observer_context = &rejected;
    sb_init(&failed);
    CHECK(!hlsl_emit_with_options(&program, &failed, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
          quality.emission_status == HLSL_EMIT_STATUS_ANALYSIS_FAILED &&
          (quality.reasons & HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT) && rejected.observations == 1);
    sb_free(&failed);
    options.source_quality = NULL;
    HLSLEmitDiagnostic diagnostic;
    sb_init(&failed);
    CHECK(!hlsl_emit_with_options_diagnostic(&program, &failed, NULL, NULL, NULL, &options,
                                              &diagnostic));
    CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
    sb_free(&failed);
    return true;
}

static bool owned_logical_facts(void *context, uint32_t unit_id,
                                const ASTExpr *expression, HLSLSourceQualityFacts *facts) {
    (void)context;
    (void)unit_id;
    const ASTLogicalValueOrigin *origin = &expression->logical_origin;
    if (!origin->complete) return false;
    facts->known = true;
    facts->value_kind = HLSL_SOURCE_VALUE_LOGICAL;
    facts->logical_value_id = origin->logical_value_id;
    facts->components = origin->components;
    facts->instruction_index = origin->instruction_index;
    facts->source_instruction_index = origin->source_instruction_index;
    facts->lanes = origin->destination_lanes;
    facts->semantic_projection = origin->semantic_projection;
    facts->real_bitcast = origin->program_bitcast;
    facts->logical_operation = expression->kind != AST_EXPR_VAR &&
                               expression->kind != AST_EXPR_LITERAL;
    return true;
}

static bool check_owned_ast_logical_origins(void) {
    ASTExpr *base = ast_create_var(-1, 7, OPERAND_TYPE_TEMP, "r7");
    ASTExpr *nested = ast_create_binary(USIL_OP_MUL, base, ast_create_literal_float(2.0f));
    ASTExpr *root = ast_create_binary(USIL_OP_ADD, nested, ast_create_literal_float(1.0f));
    CHECK(root);
    CHECK(!base->logical_origin.complete && base->logical_origin.logical_value_id == UINT64_MAX &&
          base->logical_origin.instruction_index == -1 &&
          base->logical_origin.source_instruction_index == UINT32_MAX);
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = AST_SCALAR_FLOAT32;
    origin.components = 4;
    origin.logical_value_id = 7;
    origin.instruction_index = 0;
    origin.source_instruction_index = 10;
    origin.destination_lanes = 15;
    CHECK(ast_set_logical_value_origin(base, &origin));
    origin.logical_value_id = 8;
    origin.instruction_index = 1;
    origin.source_instruction_index = 11;
    CHECK(ast_set_logical_value_origin(nested, &origin));
    origin.logical_value_id = 9;
    origin.instruction_index = 2;
    origin.source_instruction_index = 12;
    CHECK(ast_set_logical_value_origin(root, &origin));
    origin.components = 2;
    CHECK(root->logical_origin.components == 4); /* Owned, no borrowed facts. */
    ASTLogicalValueOrigin previous = root->logical_origin;
    origin = previous;
    origin.components = 0;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    origin.components = 5;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    origin = previous;
    origin.scalar_type = (ASTScalarType)99;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    origin = previous;
    origin.source_instruction_index = UINT32_MAX;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    origin = previous;
    origin.instruction_index = -1;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    origin = previous;
    origin.destination_lanes = 16;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    origin = previous;
    origin.semantic_projection = true;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    origin = previous;
    origin.program_bitcast = true;
    CHECK(!ast_set_logical_value_origin(root, &origin));
    CHECK(memcmp(&previous, &root->logical_origin, sizeof(previous)) == 0);
    const ASTExpr *expressions[] = {root};
    HLSLSourceQualityUnit unit = {
        .source_unit_id = 1, .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
        .coverage_complete = true, .expressions = expressions, .expression_count = 1};
    HLSLSourceQualityRequest request = {
        .stage = DXBC_PROGRAM_TYPE_VERTEX, .emission_status = HLSL_EMIT_STATUS_OK,
        .units = &unit, .unit_count = 1, .expected_unit_count = 1,
        .expression_facts = owned_logical_facts};
    HLSLSourceQualityResult quality;
    CHECK(hlsl_source_quality_analyze(&request, &quality));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          quality.counts.logical_operations == 2 && !quality.counts.unknown_provenance);
    nested->logical_origin.complete = false;
    CHECK(hlsl_source_quality_analyze(&request, &quality));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
          quality.counts.unknown_provenance == 1 && quality.counts.logical_operations == 1 &&
          quality.first_issue.ast_kind == AST_EXPR_BINARY);
    ast_free_expr(root);

    ASTOperandProvenance operand_origin;
    ast_operand_provenance_init(&operand_origin);
    operand_origin.complete = true;
    operand_origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    operand_origin.logical_value_id = 17;
    operand_origin.natural_components = 4;
    operand_origin.result_components = 1;
    operand_origin.selection_role = AST_COMPONENT_SELECTION_SEMANTIC;
    operand_origin.selected_components[0] = 1;
    ASTExpr *opaque = ast_create_emitter_operand_with_provenance("color.y", &operand_origin);
    CHECK(opaque);
    operand_origin.selected_components[0] = 4;
    CHECK(opaque->operand_provenance.selected_components[0] == 1);
    CHECK(!ast_create_emitter_operand_with_provenance("color.y", &operand_origin));
    operand_origin.selection_role = AST_COMPONENT_SELECTION_TRANSPORT;
    CHECK(!ast_create_emitter_operand_with_provenance("color.y", &operand_origin));
    operand_origin.selected_components[0] = 1;
    operand_origin.natural_components = 0;
    CHECK(!ast_create_emitter_operand_with_provenance("color.y", &operand_origin));
    origin = previous;
    CHECK(!ast_set_logical_value_origin(opaque, &origin));
    ast_free_expr(opaque);
    return true;
}

static bool check_natural_interfaces_and_residual_projections(void) {
    USILInstruction instructions[2] = {0};
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].source_instruction_index = 101;
    instructions[0].operands[0] = emitted_operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[0].operands[1] = emitted_operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[1].opcode = USIL_OP_RET;
    instructions[1].source_instruction_index = 102;
    DXBCSignatureElement inputs[2] = {
        {.semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15},
        {.semantic_name = "TEXCOORD", .semantic_index = 1, .register_id = 1,
         .component_type = 3, .mask = 15, .rw_mask = 15}};
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram program = {
        .instructions = instructions, .instruction_count = 2, .instruction_alloc = 2,
        .inputs = inputs, .input_count = 1, .input_alloc = 2,
        .outputs = &output, .output_count = 1, .output_alloc = 1,
        .has_stage_contract = true, .program_type = DXBC_PROGRAM_TYPE_PIXEL,
        .shader_model_major = 5};
    memcpy(program.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
    HLSLSourceQualityResult quality;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.source_quality = &quality;
    StringBuilder source;
    /* Prefix signatures become natural scalar/vector parameters and returns. */
    for (unsigned width = 1; width <= 4; ++width) {
        uint8_t mask = (uint8_t)((1u << width) - 1u);
        inputs[0].mask = inputs[0].rw_mask = mask;
        output.mask = mask;
        instructions[0].operands[0] = emitted_operand(OPERAND_TYPE_OUTPUT, 0, mask);
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
        const char *type = width == 1 ? "float" : width == 2 ? "float2" :
                           width == 3 ? "float3" : "float4";
        char declaration[96];
        snprintf(declaration, sizeof(declaration), "%s main(%s texcoord0 : TEXCOORD0)", type, type);
        CHECK(strstr(source.buf, declaration) && strstr(source.buf, "return (texcoord0);"));
        CHECK(!strstr(source.buf, "v0") && !strstr(source.buf, "o0"));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !quality.counts.residual_total && !quality.counts.incomplete_units);
        sb_free(&source);
    }
    /* Ascending coordinate projection is distinct from reversed transport. */
    output.mask = 3;
    instructions[0].operands[0] = emitted_operand(OPERAND_TYPE_OUTPUT, 0, 3);
    instructions[0].operands[1].swizzle[0] = 1;
    instructions[0].operands[1].swizzle[1] = 2;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "return (texcoord0.yz);"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          quality.counts.semantic_projections == 1 && !quality.counts.lane_transport);
    sb_free(&source);
    output.mask = 15;
    instructions[0].operands[0] = emitted_operand(OPERAND_TYPE_OUTPUT, 0, 15);
    for (unsigned component = 0; component < 4; ++component)
        instructions[0].operands[1].swizzle[component] = (uint8_t)(3 - component);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "return (texcoord0.wzyx);"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_LOW_LEVEL &&
          quality.counts.lane_transport == 1 && !quality.counts.semantic_projections &&
          quality.first_issue.facts.instruction_index == 0 &&
          quality.first_issue.facts.source_instruction_index == 101);
    sb_free(&source);
    instructions[0].operands[1] = emitted_operand(OPERAND_TYPE_INPUT, 0, 0);
    const char *reserved[] = {"texcoord0"};
    options.reserved_preprocessor_identifiers = reserved;
    options.reserved_preprocessor_identifier_count = 1;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "float4 texcoord0_1 : TEXCOORD0") &&
          strstr(source.buf, "return (texcoord0_1);"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    options.reserved_preprocessor_identifier_count = 0;
    options.reserved_preprocessor_identifiers = NULL;
    const HLSLEmitNames names = {.entry_point = "texcoord0"};
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, &names, &options));
    CHECK(strstr(source.buf, "float4 texcoord0(float4 texcoord0_1 : TEXCOORD0)") &&
          strstr(source.buf, "return (texcoord0_1);"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    strcpy(inputs[0].semantic_name, "CUSTOM_ATTRIBUTE");
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "float4 attribute0 : CUSTOM_ATTRIBUTE") &&
          strstr(source.buf, "return (attribute0);"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    strcpy(inputs[0].semantic_name, "TEXCOORD");
    /* Unsupported static data cannot inherit a clean entry classification. */
    uint32_t icb[4] = {0};
    program.icb_values = icb;
    program.icb_value_count = program.icb_value_alloc = 4;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED);
    sb_free(&source);
    program.icb_values = NULL;
    program.icb_value_count = program.icb_value_alloc = 0;
    options.omit_unity_builtin_declarations = true;
    /* An unused omission policy does not invent an external dependency. */
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          quality.counts.incomplete_units == 0);
    sb_free(&source);
    options.omit_unity_builtin_declarations = false;
    /* Malformed signature metadata retains the original failure outcome. */
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        inputs[0].mask = mutation == 0 ? 6 : 15;
        inputs[0].min_precision = mutation == 1 ? 1 : 0;
        program.input_count = mutation == 2 ? 2 : 1;
        inputs[1].register_id = mutation == 2 ? 0 : 1;
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
        CHECK(quality.classification == (mutation == 2 ? HLSL_SOURCE_QUALITY_UNSUPPORTED
                                                        : HLSL_SOURCE_QUALITY_FAILED) &&
              quality.emission_status == (mutation == 2 ? HLSL_EMIT_STATUS_UNSUPPORTED
                                                        : HLSL_EMIT_STATUS_INVALID_PROGRAM));
        sb_free(&source);
    }
    /* Valid disjoint packed fields remain unsupported by the natural ABI. */
    inputs[0].mask = inputs[0].rw_mask = 6;
    inputs[0].min_precision = 0;
    inputs[1].mask = inputs[1].rw_mask = 9;
    inputs[1].register_id = 0;
    output.mask = 3;
    instructions[0].operands[0] = emitted_operand(OPERAND_TYPE_OUTPUT, 0, 3);
    instructions[0].operands[1].swizzle[0] = 1;
    instructions[0].operands[1].swizzle[1] = 2;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&program, &source, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED &&
          quality.emission_status == HLSL_EMIT_STATUS_UNSUPPORTED);
    sb_free(&source);
    return true;
}

static bool check_cbuffer_shape_facts(void) {
    HLSLSourceQualityFacts base;
    memset(&base, 0xff, sizeof(base)); hlsl_source_quality_facts_init(&base);
    CHECK(!base.cbuffer_field_rows && !base.cbuffer_field_columns && !base.cbuffer_field_is_matrix);
    base.known = true; base.cbuffer_declaration_kind = HLSL_SOURCE_CBUFFER_FIELD;
    base.cbuffer_binding_register = 0; base.cbuffer_declaration_authority = 1;
    base.cbuffer_field_index = 0; base.cbuffer_byte_size = 64;
    base.cbuffer_field_rows = 4; base.cbuffer_field_columns = 4; base.cbuffer_field_is_matrix = true;
    for (unsigned variant = 0; variant < 5; ++variant) {
        HLSLSourceQualityFacts fact = base;
        if (variant == 1) { fact.cbuffer_field_index = 1; fact.cbuffer_byte_offset = 16; }
        if (variant >= 2) {
            fact.cbuffer_field_index = 1; fact.cbuffer_byte_offset = 64; fact.cbuffer_byte_size = 16;
            fact.cbuffer_field_rows = 1; fact.cbuffer_field_is_matrix = false;
        }
        if (variant == 3) { fact.cbuffer_field_rows = fact.cbuffer_field_columns = 0; fact.cbuffer_field_index = 4; }
        if (variant == 4) {
            fact.cbuffer_declaration_kind = HLSL_SOURCE_CBUFFER_END; fact.cbuffer_field_index = UINT32_MAX;
            fact.cbuffer_byte_offset = 0; fact.cbuffer_byte_size = 80;
            fact.cbuffer_field_rows = fact.cbuffer_field_columns = 0;
        }
        HLSLSourceQualityUnit unit = {.kind = HLSL_SOURCE_UNIT_ENTRY_POINT, .coverage_complete = true,
            .emission_facts = &fact, .emission_fact_count = 1};
        HLSLSourceQualityRequest request = {.stage = DXBC_PROGRAM_TYPE_VERTEX, .emission_status = HLSL_EMIT_STATUS_OK,
            .units = &unit, .unit_count = 1, .expected_unit_count = 1};
        HLSLSourceQualityResult result;
        /* This checks the analyzer's fact grammar, not a producer receipt or
         * proof that a standalone declaration covers an actual program. */
        CHECK(hlsl_source_quality_analyze(&request, &result) && result.classification == HLSL_SOURCE_QUALITY_CLEAN &&
            result.counts.cbuffer_fields == (variant == 4 ? 0u : 1u));
    }
    for (unsigned mutation = 0; mutation < 14; ++mutation) {
        HLSLSourceQualityFacts fact = base;
        if (mutation == 0) {
            fact.cbuffer_field_rows = fact.cbuffer_field_columns = 0; fact.cbuffer_field_is_matrix = false;
        }
        if (mutation == 1) fact.cbuffer_field_index = 1; /* Ordinal one cannot start at byte zero. */
        if (mutation == 2) fact.cbuffer_field_rows = 3;
        if (mutation == 3) fact.cbuffer_field_columns = 3;
        if (mutation == 4) fact.cbuffer_byte_size = 16;
        if (mutation == 5) fact.cbuffer_field_is_matrix = false;
        if (mutation == 6) { fact.cbuffer_field_rows = 1; fact.cbuffer_field_is_matrix = false; }
        if (mutation == 7) fact.cbuffer_byte_size = 128; /* Array extents have no shape authority here. */
        if (mutation == 8) fact.cbuffer_byte_offset = 4;
        if (mutation == 9) fact.cbuffer_byte_offset = 65520;
        if (mutation >= 10) {
            fact.cbuffer_declaration_kind = mutation == 10 ? HLSL_SOURCE_CBUFFER_NONE :
                mutation == 11 ? HLSL_SOURCE_CBUFFER_BEGIN : HLSL_SOURCE_CBUFFER_END;
            fact.cbuffer_field_index = UINT32_MAX;
            if (mutation == 13) { fact.cbuffer_field_rows = fact.cbuffer_field_columns = 0; }
        }
        HLSLSourceQualityUnit unit = {.kind = HLSL_SOURCE_UNIT_ENTRY_POINT, .coverage_complete = true,
            .emission_facts = &fact, .emission_fact_count = 1};
        HLSLSourceQualityRequest request = {.stage = DXBC_PROGRAM_TYPE_VERTEX, .emission_status = HLSL_EMIT_STATUS_OK,
            .units = &unit, .unit_count = 1, .expected_unit_count = 1};
        HLSLSourceQualityResult result;
        CHECK(!hlsl_source_quality_analyze(&request, &result) && result.classification == HLSL_SOURCE_QUALITY_FAILED);
    }
    CHECK(hlsl_source_quality_facts_equal(&base, &base));
    for (unsigned member = 0; member < 3; ++member) {
        HLSLSourceQualityFacts other = base;
        if (!member) --other.cbuffer_field_rows;
        if (member == 1) --other.cbuffer_field_columns;
        if (member == 2) other.cbuffer_field_is_matrix = false;
        CHECK(!hlsl_source_quality_facts_equal(&base, &other));
        HLSLSourceQualityResult a = {.has_first_issue = true}, b = a;
        a.first_issue.facts = base; b.first_issue.facts = other;
        CHECK(!hlsl_source_quality_results_equal(&a, &b));
    }
    return true;
}

static bool check_multiple_entry_inventory(void) {
    HLSLSourceQualityFacts fact;
    hlsl_source_quality_facts_init(&fact);
    fact.known = true;
    HLSLSourceQualityUnit units[] = {
        {.source_unit_id = 0, .kind = HLSL_SOURCE_UNIT_CONFIGURATION, .coverage_complete = true,
         .emission_facts = &fact, .emission_fact_count = 1},
        {.source_unit_id = 1, .kind = HLSL_SOURCE_UNIT_ENTRY_POINT, .coverage_complete = true,
         .emission_facts = &fact, .emission_fact_count = 1},
        {.source_unit_id = 2, .kind = HLSL_SOURCE_UNIT_ENTRY_POINT, .coverage_complete = true,
         .emission_facts = &fact, .emission_fact_count = 1}
    };
    HLSLSourceQualityRequest request = {
        .stage = DXBC_PROGRAM_TYPE_COMPUTE, .emission_status = HLSL_EMIT_STATUS_OK,
        .units = units, .unit_count = 3, .expected_unit_count = 3, .expected_entry_point_count = 2
    };
    HLSLSourceQualityResult batch, stream;
    CHECK(hlsl_source_quality_analyze(&request, &batch));
    CHECK(batch.classification == HLSL_SOURCE_QUALITY_CLEAN && batch.counts.inspected_units == 3);
    HLSLSourceQualityAnalysis *analysis = hlsl_source_quality_analysis_create(&request, &stream);
    CHECK(analysis != NULL);
    for (size_t index = 0; index < 3; ++index) {
        CHECK(hlsl_source_quality_analysis_begin_unit(analysis, units[index].source_unit_id,
                                                      units[index].kind, true));
        CHECK(hlsl_source_quality_analysis_emission(analysis, &fact));
    }
    CHECK(hlsl_source_quality_analysis_finish(analysis, HLSL_EMIT_STATUS_OK, 3));
    CHECK(stream.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          stream.counts.inspected_units == batch.counts.inspected_units &&
          stream.counts.emission_events == batch.counts.emission_events);
    hlsl_source_quality_analysis_destroy(analysis);
    request.expected_entry_point_count = 0; /* Existing one-entry default remains strict. */
    CHECK(hlsl_source_quality_analyze(&request, &batch));
    CHECK(batch.classification != HLSL_SOURCE_QUALITY_CLEAN && batch.counts.incomplete_units == 1);
    request.expected_entry_point_count = 2;
    request.unit_count = 2; /* Dropping a required entry cannot become clean. */
    CHECK(hlsl_source_quality_analyze(&request, &batch));
    CHECK(batch.classification != HLSL_SOURCE_QUALITY_CLEAN && batch.counts.incomplete_units == 2);
    request.unit_count = 3;
    units[2].source_unit_id = 1; /* Duplicate entry identity stays invalid. */
    CHECK(!hlsl_source_quality_analyze(&request, &batch));
    units[2].source_unit_id = 2;
    request.expected_entry_point_count = 1048577;
    CHECK(!hlsl_source_quality_analyze(&request, &batch));
    return true;
}

int main(void) {
    if (!check_semantic_projection_and_names() || !check_bitcast_and_opaque_boundary() ||
        !check_hidden_helpers_and_categories() || !check_statements_and_emission_outcomes() ||
        !check_invalid_and_bounded_analysis() || !check_streaming_matches_batch() ||
        !check_real_emitter_quality_is_independent() || !check_owned_ast_logical_origins() ||
        !check_natural_interfaces_and_residual_projections() || !check_multiple_entry_inventory() ||
        !check_cbuffer_shape_facts())
        return 1;
    puts("HLSL semantic source-quality classifications passed");
    return 0;
}
