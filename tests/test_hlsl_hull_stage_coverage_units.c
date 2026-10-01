// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "test_tessellation_fixture.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/hlsl_stage_coverage_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

typedef struct {
    DXBCDocument document;
    DXBCContainer semantic;
    DXBCStageContract contract;
    USILProgram program;
    SerializedVariable field;
    SerializedConstantBuffer buffer;
    SerializedResourceParam binding;
    SerializedProgramParameters parameters;
} HullFixture;

static bool fixture_init(HullFixture *fixture, bool float3,
                          bool control_point, bool clamps) {
    memset(fixture, 0, sizeof(*fixture));
    size_t size = 0;
    uint8_t *bytes = clamps
        ? test_tessellation_hull_scalar_cbuffer_dxbc(3, 3, float3, "POINTVALUE", &size)
        : float3 ? test_tessellation_hull_float3_dxbc(3, 3, 0, "POINTVALUE", &size)
                 : test_tessellation_hull_dxbc(3, 3, control_point ? 4 : 0, &size);
    CHECK(bytes);
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContractDiagnostic contract_diagnostic;
    const bool parsed = dxbc_document_parse(&fixture->document, bytes, size,
                                            &document_diagnostic);
    free(bytes);
    CHECK(parsed);
    CHECK(dxbc_document_decode_semantic(&fixture->document, &fixture->semantic));
    CHECK(dxbc_stage_contract_decode(&fixture->document, &fixture->semantic,
                                     &fixture->contract, &contract_diagnostic));
    CHECK(usil_translate_with_stage_contract(&fixture->program,
        &fixture->semantic, &fixture->contract));
    if (clamps) {
        fixture->field = (SerializedVariable){
            .name = "_Factor", .layout = {0, 0, 0, 1, 0, 0}};
        fixture->buffer = (SerializedConstantBuffer){
            .name = "FactorInputs", .size = 16,
            .role = SERIALIZED_CBUFFER_NAMED,
            .variables = &fixture->field, .var_count = 1};
        fixture->binding = (SerializedResourceParam){
            .name = fixture->buffer.name,
            .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .array_size = 1};
        fixture->parameters = (SerializedProgramParameters){
            .constant_buffers = &fixture->buffer, .cb_count = 1,
            .resources = &fixture->binding, .res_count = 1};
    }
    return true;
}

static void fixture_dispose(HullFixture *fixture) {
    usil_free(&fixture->program);
    dxbc_stage_contract_free(&fixture->contract);
    dxbc_free(&fixture->semantic);
    dxbc_document_free(&fixture->document);
}

static const SerializedProgramParameters *fixture_parameters(const HullFixture *fixture) {
    return fixture->parameters.cb_count ? &fixture->parameters : NULL;
}

typedef struct {
    USILProgram *program;
    unsigned units, expressions;
    int reject_unit;
    bool bad_owner;
} ObservationLedger;

static bool observe_units(void *context, const HLSLSourceQualityObservation *observation) {
    ObservationLedger *ledger = context;
    if (ledger->reject_unit >= 0 &&
        observation->source_unit_id == (uint32_t)ledger->reject_unit) return false;
    if (observation->stage != DXBC_PROGRAM_TYPE_HULL ||
        observation->pass_index != 4 || observation->entry_point_index != 3 ||
        observation->source_unit_id > 2) ledger->bad_owner = true;
    else ledger->units |= 1u << observation->source_unit_id;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (facts->instruction_index >= 0 &&
        (facts->instruction_index >= ledger->program->instruction_count ||
         facts->source_instruction_index !=
            ledger->program->instructions[facts->instruction_index].source_instruction_index))
        ledger->bad_owner = true;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION) ++ledger->expressions;
    return true;
}

static bool coverage_matches(HLSLStageCoverage *coverage,
                              const HLSLStageCoverage *independent,
                              const StringBuilder *source) {
    CHECK(hlsl_stage_coverage_validate(coverage, source));
    CHECK(hlsl_stage_coverage_equal(coverage, independent));
    return true;
}

static bool coverage_rejects(HLSLStageCoverage *coverage,
                              const HLSLStageCoverage *independent,
                              const StringBuilder *source) {
    CHECK(!hlsl_stage_coverage_validate(coverage, source));
    CHECK(!hlsl_stage_coverage_equal(coverage, independent));
    return true;
}

static bool check_unit_inventory(const HLSLStageCoverage *coverage,
                                  const StringBuilder *source) {
    static const HLSLSourceQualityUnitKind kinds[] = {
        HLSL_SOURCE_UNIT_CONFIGURATION,
        HLSL_SOURCE_UNIT_HELPER,
        HLSL_SOURCE_UNIT_ENTRY_POINT
    };
    CHECK(coverage->stage == DXBC_PROGRAM_TYPE_HULL &&
          coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT &&
          coverage->unit_count == 3 && coverage->recorded_unit_count == 3);
    CHECK(coverage->obligations & HLSL_STAGE_COVERAGE_BODY);
    CHECK(coverage->obligations & HLSL_STAGE_COVERAGE_LOCAL_DECLARATION);
    CHECK(coverage->units[0].obligations & HLSL_STAGE_COVERAGE_LOCAL_DECLARATION);
    CHECK(coverage->units[1].obligations & HLSL_STAGE_COVERAGE_BODY);
    CHECK(coverage->units[2].obligations & HLSL_STAGE_COVERAGE_BODY);
    CHECK(coverage->required_binding_mask == 0);
    size_t previous_end = 0, previous_root = 0, previous_syntax = 0;
    for (size_t unit_index = 0; unit_index < 3; ++unit_index) {
        const HLSLStageOwnedUnit *unit = &coverage->units[unit_index];
        CHECK(unit->source_unit_id == unit_index && unit->kind == kinds[unit_index]);
        CHECK(unit->begin == previous_end && unit->begin < unit->end &&
              unit->end <= source->len);
        CHECK(unit->root_begin == previous_root && unit->root_begin <= unit->root_end &&
              unit->root_end <= coverage->root_count);
        CHECK(unit->syntax_begin == previous_syntax && unit->syntax_begin < unit->syntax_end &&
              unit->syntax_end <= coverage->syntax_count);
        for (size_t root_index = unit->root_begin; root_index < unit->root_end; ++root_index) {
            const HLSLStageOwnedRoot *root = &coverage->roots[root_index];
            CHECK(root->source_unit_id == unit_index && root->unit_kind == unit->kind);
            CHECK(root->emitted && !root->live_tree && root->tree && root->recorded_tree);
            CHECK(root->begin >= unit->begin && root->begin < root->end && root->end <= unit->end);
        }
        for (size_t syntax_index = unit->syntax_begin; syntax_index < unit->syntax_end; ++syntax_index) {
            const HLSLStageOwnedSyntax *syntax = &coverage->syntax[syntax_index];
            CHECK(syntax->source_unit_id == unit_index && syntax->unit_kind == unit->kind);
            CHECK(syntax->source_end >= unit->begin && syntax->source_end <= unit->end);
        }
        previous_end = unit->end;
        previous_root = unit->root_end;
        previous_syntax = unit->syntax_end;
    }
    CHECK(previous_end == source->len && previous_root == coverage->root_count &&
          previous_syntax == coverage->syntax_count);
    return true;
}

static bool unit_mutations(HLSLStageCoverage *coverage,
                            const HLSLStageCoverage *independent,
                            const StringBuilder *source) {
    const HLSLStageCoverageSchema schema = coverage->schema;
    coverage->schema = (HLSLStageCoverageSchema)99;
    CHECK(!hlsl_stage_coverage_equal(coverage, coverage));
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->schema = HLSL_STAGE_COVERAGE_ORDINARY_ENTRY;
    CHECK(!hlsl_stage_coverage_equal(coverage, coverage));
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->schema = schema;
    const uint32_t obligations = coverage->obligations;
    coverage->obligations &= ~HLSL_STAGE_COVERAGE_BODY;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->obligations = obligations & ~HLSL_STAGE_COVERAGE_LOCAL_DECLARATION;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->obligations = obligations | UINT32_C(0x80000000);
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->obligations = obligations;
    --coverage->unit_count;
    CHECK(coverage_rejects(coverage, independent, source));
    ++coverage->unit_count;
    --coverage->recorded_unit_count;
    CHECK(coverage_rejects(coverage, independent, source));
    ++coverage->recorded_unit_count;
    const HLSLStageOwnedUnit unit = coverage->units[0];
    const HLSLStageOwnedUnit recorded = coverage->recorded_units[0];
    coverage->units[0].kind = HLSL_SOURCE_UNIT_ENTRY_POINT;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->units[0] = unit;
    coverage->recorded_units[0].kind = HLSL_SOURCE_UNIT_ENTRY_POINT;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->recorded_units[0] = recorded;
    coverage->units[0].kind = coverage->recorded_units[0].kind = HLSL_SOURCE_UNIT_ENTRY_POINT;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->units[0] = unit;
    coverage->recorded_units[0] = recorded;
    ++coverage->units[0].begin;
    ++coverage->recorded_units[0].begin;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->units[0] = unit;
    coverage->recorded_units[0] = recorded;
    ++coverage->units[0].source_unit_id;
    ++coverage->recorded_units[0].source_unit_id;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->units[0] = unit;
    coverage->recorded_units[0] = recorded;
    ++coverage->units[0].root_end;
    ++coverage->recorded_units[0].root_end;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->units[0] = unit;
    coverage->recorded_units[0] = recorded;
    --coverage->units[0].syntax_end;
    --coverage->recorded_units[0].syntax_end;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->units[0] = unit;
    coverage->recorded_units[0] = recorded;
    coverage->units[1].obligations ^= HLSL_STAGE_COVERAGE_BODY;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->units[1].obligations ^= HLSL_STAGE_COVERAGE_BODY;
    for (size_t index = 0; index < 3; ++index) {
        const uint32_t required = index == 0
            ? HLSL_STAGE_COVERAGE_LOCAL_DECLARATION : HLSL_STAGE_COVERAGE_BODY;
        coverage->units[index].obligations ^= required;
        coverage->recorded_units[index].obligations ^= required;
        CHECK(coverage_rejects(coverage, independent, source));
        coverage->units[index].obligations ^= required;
        coverage->recorded_units[index].obligations ^= required;
    }
    const size_t instruction_count = coverage->instruction_count;
    coverage->instruction_count = HLSL_STAGE_COVERAGE_ROOT_LIMIT + 1;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->instruction_count = instruction_count;
    const size_t root_count = coverage->root_count;
    coverage->root_count = coverage->recorded_root_count = HLSL_STAGE_COVERAGE_ROOT_LIMIT + 1;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->root_count = coverage->recorded_root_count = root_count;
    const size_t syntax_count = coverage->syntax_count;
    coverage->syntax_count = coverage->recorded_syntax_count = HLSL_STAGE_COVERAGE_EVENT_LIMIT + 1;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->syntax_count = coverage->recorded_syntax_count = syntax_count;
    CHECK(coverage_matches(coverage, independent, source));
    return true;
}

static bool owner_mutations(HLSLStageCoverage *coverage,
                             const HLSLStageCoverage *independent,
                             const StringBuilder *source) {
    CHECK(coverage->instruction_count > 0 && coverage->root_count > 0);
    ++coverage->source_instructions[0];
    CHECK(coverage_rejects(coverage, independent, source));
    --coverage->source_instructions[0];
    ++coverage->recorded_source_instructions[0];
    CHECK(coverage_rejects(coverage, independent, source));
    --coverage->recorded_source_instructions[0];
    const USILOpcode opcode = coverage->opcodes[0];
    coverage->opcodes[0] = USIL_OP_ADD;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->opcodes[0] = opcode;
    coverage->recorded_opcodes[0] = USIL_OP_ADD;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->recorded_opcodes[0] = opcode;
    coverage->destination_lanes[0] ^= 2u;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->destination_lanes[0] ^= 2u;
    coverage->recorded_destination_lanes[0] ^= 2u;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->recorded_destination_lanes[0] ^= 2u;
    coverage->operand_counts[0] = DXBC_MAX_OPERANDS + 1;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->operand_counts[0] = coverage->recorded_operand_counts[0];
    const HLSLStageOwnedOperandUse use = coverage->operand_uses[0][1];
    coverage->operand_uses[0][1].type = OPERAND_TYPE_CONSTANT_BUFFER;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->operand_uses[0][1] = use;
    coverage->recorded_operand_uses[0][1].source_lanes ^= 2u;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->recorded_operand_uses[0][1].source_lanes ^= 2u;
    ++coverage->hull_contract.tessellation.output_control_point_count;
    CHECK(coverage_rejects(coverage, independent, source));
    --coverage->hull_contract.tessellation.output_control_point_count;
    ++coverage->recorded_hull_contract.phases[0].instance_count;
    CHECK(coverage_rejects(coverage, independent, source));
    --coverage->recorded_hull_contract.phases[0].instance_count;
    coverage->hull_contract.input.mask ^= 1u;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->hull_contract.input.mask ^= 1u;
    coverage->recorded_hull_contract.tessellation.max_tessellation_factor_bits ^= 1u;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->recorded_hull_contract.tessellation.max_tessellation_factor_bits ^= 1u;
    CHECK(coverage_matches(coverage, independent, source));
    return true;
}

static HLSLStageOwnedRoot *find_root(HLSLStageCoverage *coverage,
                                     HLSLStageRootOwnerKind kind) {
    for (size_t index = 0; index < coverage->root_count; ++index)
        if (coverage->roots[index].owner.kind == kind) return &coverage->roots[index];
    return NULL;
}

static bool root_mutations(HLSLStageCoverage *coverage,
                            const HLSLStageCoverage *independent,
                            const StringBuilder *source, bool control_point) {
    HLSLStageOwnedRoot *maximum = find_root(coverage, HLSL_STAGE_ROOT_HULL_MAXIMUM);
    HLSLStageOwnedRoot *aggregate = find_root(coverage, control_point
        ? HLSL_STAGE_ROOT_HULL_POINT_RETURN : HLSL_STAGE_ROOT_HULL_IMPLICIT_COPY);
    HLSLStageOwnedRoot *instance = find_root(coverage, HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE);
    CHECK(maximum && aggregate && instance && maximum->tree->kind == AST_EXPR_LITERAL);
    CHECK(maximum->owner.value_bits == UINT32_C(0x42000000) &&
          maximum->owner.source_instruction_index ==
            coverage->hull_contract.tessellation.max_tessellation_factor_source_instruction_index);
    CHECK(aggregate->tree->kind == AST_EXPR_EMITTER_OPERAND &&
          aggregate->tree->operand_provenance.complete &&
          aggregate->tree->operand_provenance.natural_components == 0 &&
          aggregate->tree->operand_provenance.result_components == 0);
    const HLSLStageRootOwner owner = maximum->owner;
    ++maximum->owner.source_instruction_index;
    CHECK(coverage_rejects(coverage, independent, source));
    maximum->owner = owner;
    maximum->recorded_owner.value_bits ^= 1u;
    CHECK(coverage_rejects(coverage, independent, source));
    maximum->recorded_owner.value_bits ^= 1u;
    maximum->owner.kind = maximum->recorded_owner.kind = HLSL_STAGE_ROOT_INSTRUCTION;
    CHECK(coverage_rejects(coverage, independent, source));
    maximum->owner = maximum->recorded_owner = owner;
    ++aggregate->owner.output_signature_index;
    CHECK(coverage_rejects(coverage, independent, source));
    --aggregate->owner.output_signature_index;
    ++aggregate->recorded_owner.output_signature_index;
    CHECK(coverage_rejects(coverage, independent, source));
    --aggregate->recorded_owner.output_signature_index;
    ++instance->owner.instance_count;
    ++instance->recorded_owner.instance_count;
    CHECK(coverage_rejects(coverage, independent, source));
    --instance->owner.instance_count;
    --instance->recorded_owner.instance_count;
    CHECK(instance->tree->kind == AST_EXPR_EMITTER_OPERAND);
    const uint8_t lanes = instance->tree->operand_provenance.destination_lanes;
    instance->tree->operand_provenance.destination_lanes ^= 2u;
    instance->recorded_tree->operand_provenance.destination_lanes ^= 2u;
    CHECK(coverage_rejects(coverage, independent, source));
    instance->tree->operand_provenance.destination_lanes = lanes;
    instance->recorded_tree->operand_provenance.destination_lanes = lanes;
    maximum->tree->u.literal.scalar_type = AST_SCALAR_UINT32;
    CHECK(coverage_rejects(coverage, independent, source));
    maximum->tree->u.literal.scalar_type = AST_SCALAR_FLOAT32;
    maximum->recorded_tree->u.literal.scalar_type = AST_SCALAR_UINT32;
    CHECK(coverage_rejects(coverage, independent, source));
    maximum->recorded_tree->u.literal.scalar_type = AST_SCALAR_FLOAT32;
    maximum->tree->u.literal.val[0] ^= 1u;
    maximum->recorded_tree->u.literal.val[0] ^= 1u;
    CHECK(coverage_rejects(coverage, independent, source));
    maximum->tree->u.literal.val[0] ^= 1u;
    maximum->recorded_tree->u.literal.val[0] ^= 1u;
    aggregate->tree->operand_provenance.natural_components = 1;
    CHECK(coverage_rejects(coverage, independent, source));
    aggregate->tree->operand_provenance.natural_components = 0;
    aggregate->recorded_tree->operand_provenance.result_components = 1;
    CHECK(coverage_rejects(coverage, independent, source));
    aggregate->recorded_tree->operand_provenance.result_components = 0;
    const size_t begin = maximum->begin;
    ++maximum->begin;
    CHECK(coverage_rejects(coverage, independent, source));
    maximum->begin = begin;
    coverage->source[begin] ^= 1;
    CHECK(coverage_rejects(coverage, independent, source));
    coverage->source[begin] ^= 1;
    const int instruction = aggregate->instruction;
    aggregate->instruction = (int)coverage->instruction_count;
    CHECK(coverage_rejects(coverage, independent, source));
    aggregate->instruction = instruction;
    const uint32_t unit_id = aggregate->source_unit_id;
    aggregate->source_unit_id = 0;
    CHECK(coverage_rejects(coverage, independent, source));
    aggregate->source_unit_id = unit_id;
    HLSLStageOwnedSyntax *syntax = &coverage->syntax[coverage->units[1].syntax_begin];
    const uint32_t syntax_unit = syntax->source_unit_id;
    syntax->source_unit_id = 0;
    CHECK(coverage_rejects(coverage, independent, source));
    syntax->source_unit_id = syntax_unit;
    CHECK(coverage_matches(coverage, independent, source));
    return true;
}

static bool positive_capture(bool float3, bool control_point, bool clamps) {
    HullFixture fixture;
    CHECK(fixture_init(&fixture, float3, control_point, clamps));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult normal_quality, captured_quality, independent_quality;
    HLSLExpressionSourceMap normal_map, captured_map;
    HLSLEmitDiagnostic diagnostic;
    StringBuilder normal, source, other_source;
    sb_init(&normal);
    sb_init(&source);
    sb_init(&other_source);
    ObservationLedger ledger = {.program = &fixture.program, .reject_unit = -1};
    options.source_quality = &normal_quality;
    options.expression_source_map = &normal_map;
    options.source_quality_pass_index = 4;
    options.source_quality_entry_point_index = 3;
    options.source_quality_observer = observe_units;
    options.source_quality_observer_context = &ledger;
    CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &normal,
        fixture_parameters(&fixture), NULL, NULL, &options, &diagnostic));
    CHECK(!ledger.bad_owner && ledger.units == 7 && ledger.expressions > 0);
    ledger.units = ledger.expressions = 0;
    options.source_quality = &captured_quality;
    options.expression_source_map = &captured_map;
    HLSLStageCoverage coverage = {0}, independent = {0};
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source,
        fixture_parameters(&fixture), NULL, NULL, &options, &coverage, &diagnostic));
    CHECK(!ledger.bad_owner && ledger.units == 7 && ledger.expressions > 0);
    CHECK(normal.len == source.len && !memcmp(normal.buf, source.buf, source.len + 1));
    CHECK(hlsl_source_quality_results_equal(&normal_quality, &captured_quality));
    CHECK(normal_map.complete == captured_map.complete && normal_map.count == captured_map.count);
    for (size_t index = 0; index < normal_map.count; ++index)
        CHECK(hlsl_expression_origins_equal(&normal_map.origins[index], &captured_map.origins[index]));
    CHECK(hlsl_expression_source_map_matches(&captured_map, &fixture.program, source.buf));
    CHECK(check_unit_inventory(&coverage, &source));
    CHECK(coverage.instruction_count == (size_t)fixture.program.instruction_count);
    unsigned min_count = 0, mul_count = 0;
    for (size_t instruction = 0; instruction < coverage.instruction_count; ++instruction) {
        CHECK(coverage.opcodes[instruction] == fixture.program.instructions[instruction].opcode &&
              coverage.source_instructions[instruction] ==
                fixture.program.instructions[instruction].source_instruction_index);
        if (coverage.opcodes[instruction] == USIL_OP_MUL) ++mul_count;
        if (coverage.opcodes[instruction] != USIL_OP_MIN) continue;
        ++min_count;
        CHECK(captured_map.origins[instruction].kind == HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP);
        bool saw_root = false;
        for (size_t root = 0; root < coverage.root_count; ++root)
            if (coverage.roots[root].instruction == (int)instruction &&
                coverage.roots[root].owner.kind == HLSL_STAGE_ROOT_INSTRUCTION)
                saw_root = true;
        CHECK(saw_root);
    }
    CHECK(min_count == (clamps ? 2u : 0u) && mul_count == (control_point ? 1u : 0u));
    if (clamps) CHECK(strstr(source.buf, "_Factor") && !strstr(source.buf, "min("));
    options.source_quality = &independent_quality;
    options.expression_source_map = NULL;
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &other_source,
        fixture_parameters(&fixture), NULL, NULL, &options, &independent, &diagnostic));
    CHECK(hlsl_source_quality_results_equal(&captured_quality, &independent_quality));
    CHECK(coverage_matches(&coverage, &independent, &source));
    StringBuilder reused_output;
    sb_init(&reused_output);
    CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &reused_output,
        fixture_parameters(&fixture), NULL, NULL, &options, &coverage, &diagnostic));
    CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
    CHECK(coverage_matches(&coverage, &independent, &source));
    sb_free(&reused_output);

    /* Every tree, signature and contract coordinate in these private captures
     * must outlive the producer's parsed source and semantic projection. */
    fixture_dispose(&fixture);
    CHECK(coverage_matches(&coverage, &independent, &source));
    CHECK(unit_mutations(&coverage, &independent, &source));
    CHECK(owner_mutations(&coverage, &independent, &source));
    CHECK(root_mutations(&coverage, &independent, &source, control_point));
    hlsl_stage_coverage_dispose(&independent);
    hlsl_stage_coverage_dispose(&coverage);
    sb_free(&other_source);
    sb_free(&source);
    sb_free(&normal);
    return true;
}

static bool capture_is_empty(const HLSLStageCoverage *coverage) {
    CHECK(!coverage->began && !coverage->finished && !coverage->source &&
          !coverage->roots && !coverage->syntax && !coverage->recorded_syntax &&
          !coverage->instruction_count && !coverage->source_size && !coverage->node_count &&
          !coverage->root_count && !coverage->recorded_root_count &&
          !coverage->syntax_count && !coverage->recorded_syntax_count &&
          !coverage->unit_count && !coverage->recorded_unit_count &&
          !coverage->obligations && !coverage->required_binding_mask &&
          !coverage->hull_contract.tessellation.valid &&
          !coverage->recorded_hull_contract.tessellation.valid);
    return true;
}

static bool argument_rejection(void) {
    HullFixture fixture;
    CHECK(fixture_init(&fixture, true, false, false));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    HLSLEmitDiagnostic diagnostic;
    StringBuilder reference;
    sb_init(&reference);
    options.source_quality = &quality;
    options.expression_source_map = &map;
    CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &reference,
        NULL, NULL, NULL, &options, &diagnostic));
    const HLSLSourceQualityResult retained_quality = quality;
    const HLSLExpressionSourceMap retained_map = map;
    /* Early argument rejection cannot invalidate an earlier independent
     * receipt or append to a caller's already populated source buffer. */
    for (unsigned invalid_argument = 0; invalid_argument < 9; ++invalid_argument) {
        HLSLStageCoverage coverage, retained_coverage;
        memset(&coverage, 0, sizeof(coverage));
        StringBuilder source;
        sb_init(&source);
        switch (invalid_argument) {
        case 0: sb_append(&source, "retained output"); break;
        case 1: coverage.instruction_count = 1; break;
        case 2: coverage.node_count = 1; break;
        case 3: coverage.obligations = HLSL_STAGE_COVERAGE_BODY; break;
        case 4: coverage.required_binding_mask = 1; break;
        case 5: coverage.schema = (HLSLStageCoverageSchema)99; break;
        case 6: coverage.recorded_unit_count = 1; break;
        case 7: coverage.recorded_root_count = 1; break;
        case 8: coverage.recorded_syntax_count = 1; break;
        }
        memcpy(&retained_coverage, &coverage, sizeof(coverage));
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &source,
            NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT && sb_ok(&source));
        CHECK(source.len == (invalid_argument == 0 ? strlen("retained output") : 0));
        if (invalid_argument == 0) CHECK(!strcmp(source.buf, "retained output"));
        CHECK(!memcmp(&coverage, &retained_coverage, sizeof(coverage)));
        CHECK(hlsl_source_quality_results_equal(&quality, &retained_quality));
        CHECK(map.complete == retained_map.complete && map.count == retained_map.count);
        for (size_t index = 0; index < map.count; ++index)
            CHECK(hlsl_expression_origins_equal(&map.origins[index], &retained_map.origins[index]));
        sb_free(&source);
    }
    sb_free(&reference);
    fixture_dispose(&fixture);
    return true;
}

static bool callback_rejection(void) {
    HullFixture fixture;
    CHECK(fixture_init(&fixture, true, false, false));
    for (int rejected_unit = 0; rejected_unit < 3; ++rejected_unit) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        HLSLEmitDiagnostic diagnostic;
        HLSLStageCoverage coverage = {0};
        StringBuilder source;
        sb_init(&source);
        ObservationLedger ledger = {
            .program = &fixture.program, .reject_unit = rejected_unit};
        options.source_quality = &quality;
        options.expression_source_map = &map;
        options.source_quality_pass_index = 4;
        options.source_quality_entry_point_index = 3;
        options.source_quality_observer = observe_units;
        options.source_quality_observer_context = &ledger;
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &source,
            NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(!coverage.finished && !hlsl_stage_coverage_validate(&coverage, &source));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED);
        CHECK(!map.complete && !map.count && capture_is_empty(&coverage));
        hlsl_stage_coverage_dispose(&coverage);
        sb_free(&source);
    }
    fixture_dispose(&fixture);
    return true;
}

typedef enum {
    CAPTURE_UNIT_KIND,
    CAPTURE_OPCODE,
    CAPTURE_OBLIGATION,
    CAPTURE_NODE_COUNT,
    CAPTURE_SYNTAX_UNIT,
    CAPTURE_SYNTAX_FACTS,
    CAPTURE_DRIFT_COUNT
} CaptureDrift;

typedef struct {
    HLSLStageCoverage *coverage;
    CaptureDrift drift;
    bool changed;
} CaptureDriftObserver;

static bool mutate_capture(void *context, const HLSLSourceQualityObservation *observation) {
    CaptureDriftObserver *observer = context;
    const bool first_event = observer->drift == CAPTURE_SYNTAX_UNIT ||
        observer->drift == CAPTURE_SYNTAX_FACTS;
    if (observer->changed || (!first_event && observation->source_unit_id != 2) ||
        observation->kind != HLSL_SOURCE_OBSERVATION_EMISSION) return true;
    HLSLStageCoverage *coverage = observer->coverage;
    switch (observer->drift) {
    case CAPTURE_UNIT_KIND: coverage->units[0].kind = HLSL_SOURCE_UNIT_ENTRY_POINT; break;
    case CAPTURE_OPCODE: coverage->opcodes[0] = USIL_OP_ADD; break;
    case CAPTURE_OBLIGATION: coverage->obligations &= ~HLSL_STAGE_COVERAGE_LOCAL_DECLARATION; break;
    case CAPTURE_NODE_COUNT: ++coverage->node_count; break;
    case CAPTURE_SYNTAX_UNIT:
        if (!coverage->syntax_count) return false;
        coverage->syntax[coverage->syntax_count - 1].source_unit_id = 2;
        break;
    case CAPTURE_SYNTAX_FACTS:
        if (!coverage->syntax_count) return false;
        coverage->syntax[coverage->syntax_count - 1].facts.lanes ^= 1u;
        break;
    case CAPTURE_DRIFT_COUNT: return false;
    }
    observer->changed = true;
    return true;
}

static bool capture_drift_rejection(void) {
    HullFixture fixture;
    CHECK(fixture_init(&fixture, true, false, false));
    for (CaptureDrift drift = CAPTURE_UNIT_KIND; drift < CAPTURE_DRIFT_COUNT; ++drift) {
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        HLSLEmitDiagnostic diagnostic;
        HLSLStageCoverage coverage = {0};
        StringBuilder source;
        sb_init(&source);
        CaptureDriftObserver observer = {.coverage = &coverage, .drift = drift};
        options.source_quality = &quality;
        options.expression_source_map = &map;
        options.source_quality_observer = mutate_capture;
        options.source_quality_observer_context = &observer;
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &source,
            NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(observer.changed && diagnostic.status == HLSL_EMIT_STATUS_ANALYSIS_FAILED);
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
              (quality.reasons & HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED));
        CHECK(!map.complete && !map.count && capture_is_empty(&coverage));
        CHECK(!hlsl_stage_coverage_validate(&coverage, &source));
        hlsl_stage_coverage_dispose(&coverage);
        sb_free(&source);
        sb_init(&source);
        options.source_quality_observer = NULL;
        options.source_quality_observer_context = NULL;
        CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source,
            NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(hlsl_stage_coverage_validate(&coverage, &source) && map.complete);
        hlsl_stage_coverage_dispose(&coverage);
        sb_free(&source);
    }
    fixture_dispose(&fixture);
    return true;
}

typedef enum {
    DRIFT_RAW_INDEX,
    DRIFT_OPCODE,
    DRIFT_OPERAND_TYPE,
    DRIFT_DESTINATION_LANE,
    DRIFT_PHASE_MARKER,
    DRIFT_PHASE_COUNT,
    DRIFT_SIGNATURE,
    DRIFT_MAXIMUM,
    DRIFT_MAXIMUM_OWNER,
    DRIFT_COUNT
} ProducerDrift;

typedef struct {
    USILProgram *program;
    ProducerDrift drift;
    bool changed;
} DriftObserver;

static bool mutate_producer(void *context, const HLSLSourceQualityObservation *observation) {
    DriftObserver *observer = context;
    if (observer->changed || observation->kind != HLSL_SOURCE_OBSERVATION_EMISSION) return true;
    USILProgram *program = observer->program;
    switch (observer->drift) {
    case DRIFT_RAW_INDEX: ++program->instructions[0].source_instruction_index; break;
    case DRIFT_OPCODE: program->instructions[0].opcode = USIL_OP_ADD; break;
    case DRIFT_OPERAND_TYPE: program->instructions[0].operands[1].type = OPERAND_TYPE_INPUT_THREAD_ID; break;
    case DRIFT_DESTINATION_LANE: program->instructions[0].operands[0].destination_mask ^= 0x20u; break;
    case DRIFT_PHASE_MARKER: ++program->tessellation.phases[0].marker_source_instruction_index; break;
    case DRIFT_PHASE_COUNT: ++program->tessellation.phases[0].instance_count; break;
    case DRIFT_SIGNATURE: program->inputs[0].mask ^= 1u; break;
    case DRIFT_MAXIMUM: program->tessellation.max_tessellation_factor_bits ^= 1u; break;
    case DRIFT_MAXIMUM_OWNER: ++program->tessellation.max_tessellation_factor_source_instruction_index; break;
    case DRIFT_COUNT: return false;
    }
    observer->changed = true;
    return true;
}

static bool producer_drift_rejection(void) {
    for (ProducerDrift drift = DRIFT_RAW_INDEX; drift < DRIFT_COUNT; ++drift) {
        HullFixture fixture;
        CHECK(fixture_init(&fixture, true, false, false));
        const USILInstruction instruction = fixture.program.instructions[0];
        const USILHullPhase phase = fixture.program.tessellation.phases[0];
        const uint8_t mask = fixture.program.inputs[0].mask;
        const uint32_t maximum = fixture.program.tessellation.max_tessellation_factor_bits;
        const uint32_t maximum_owner = fixture.program.tessellation.max_tessellation_factor_source_instruction_index;
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLEmitDiagnostic diagnostic;
        HLSLStageCoverage coverage = {0};
        StringBuilder source;
        sb_init(&source);
        DriftObserver observer = {.program = &fixture.program, .drift = drift};
        options.source_quality = &quality;
        options.source_quality_observer = mutate_producer;
        options.source_quality_observer_context = &observer;
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &source,
            NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(observer.changed && !coverage.finished &&
              !hlsl_stage_coverage_validate(&coverage, &source));
        CHECK(capture_is_empty(&coverage));
        fixture.program.instructions[0] = instruction;
        fixture.program.tessellation.phases[0] = phase;
        fixture.program.inputs[0].mask = mask;
        fixture.program.tessellation.max_tessellation_factor_bits = maximum;
        fixture.program.tessellation.max_tessellation_factor_source_instruction_index = maximum_owner;
        hlsl_stage_coverage_dispose(&coverage);
        sb_free(&source);
        sb_init(&source);
        options.source_quality_observer = NULL;
        options.source_quality_observer_context = NULL;
        CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source,
            NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(hlsl_stage_coverage_validate(&coverage, &source));
        hlsl_stage_coverage_dispose(&coverage);
        sb_free(&source);
        fixture_dispose(&fixture);
    }
    return true;
}

int main(void) {
    const size_t allocations = g_allocations_count;
    const size_t bytes = g_allocated_bytes;
    if (!positive_capture(true, false, false) ||
        !positive_capture(false, true, false) ||
        !positive_capture(true, false, true) ||
        !argument_rejection() || !callback_rejection() ||
        !capture_drift_rejection() || !producer_drift_rejection()) return 1;
    if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
        fprintf(stderr, "allocation leak: %zu/%zu -> %zu/%zu\n",
            allocations, bytes, g_allocations_count, g_allocated_bytes);
        return 1;
    }
    puts("HLSL HULL stage coverage unit tests passed");
    return 0;
}
