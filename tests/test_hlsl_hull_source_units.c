// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "test_tessellation_fixture.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include "translation/hlsl_global_declarations.h"
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
} HullFixture;

static bool hull_fixture_parse(HullFixture *fixture, uint8_t *bytes, size_t size) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    CHECK(bytes);
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
    return true;
}

static bool hull_fixture_init_counts(HullFixture *fixture, uint32_t points, uint32_t output_points, uint8_t scenario) {
    size_t size = 0;
    uint8_t *bytes = test_tessellation_hull_dxbc(points, output_points, scenario, &size);
    return hull_fixture_parse(fixture, bytes, size);
}

static bool hull_fixture_init(HullFixture *fixture, uint32_t points, uint8_t scenario) {
    return hull_fixture_init_counts(fixture, points, points, scenario);
}

static void hull_fixture_dispose(HullFixture *fixture) {
    usil_free(&fixture->program);
    dxbc_free(&fixture->semantic);
    dxbc_stage_contract_free(&fixture->contract);
    dxbc_document_free(&fixture->document);
}

static bool source_rejected(USILProgram *program) {
    StringBuilder source;
    sb_init(&source);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLEmitDiagnostic diagnostic;
    options.source_quality = &quality;
    const bool emitted = hlsl_emit_with_options_diagnostic(program, &source,
        NULL, NULL, NULL, &options, &diagnostic);
    CHECK(!emitted);
    CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK);
    CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    return true;
}

static bool natural_hull_source(void) {
    HullFixture fixture;
    CHECK(hull_fixture_init(&fixture, 3, 0));
    CHECK(hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    HLSLEmitDiagnostic diagnostic;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    StringBuilder source;
    sb_init(&source);
    bool emitted = hlsl_emit_with_options_diagnostic(&fixture.program, &source, NULL, NULL, NULL, &options, &diagnostic);
    if (!emitted) fprintf(stderr, "hull failed status %s phase %s reason %s instruction %d\n",
        hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase),
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted);
    CHECK(strstr(source.buf, "for (uint factorIndex = 0; factorIndex < 3; ++factorIndex)"));
    CHECK(strstr(source.buf, "factors.outer[factorIndex] = 3.0f;"));
    CHECK(strstr(source.buf, "factors.inner = 4.0f;"));
    CHECK(strstr(source.buf, "return patch[pointIndex];"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(quality.counts.incomplete_units == 0 && quality.counts.inspected_units == 3);
    CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
    map.origins[0].source_instruction_index++;
    CHECK(!hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
    map.origins[0].source_instruction_index--;
    sb_free(&source);
    const uint32_t count = fixture.program.tessellation.phases[0].instance_count;
    fixture.program.tessellation.phases[0].instance_count = 2;
    CHECK(source_rejected(&fixture.program));
    fixture.program.tessellation.phases[0].instance_count = count;
    const DXBCHullPhaseKind kind = fixture.program.tessellation.phases[0].kind;
    fixture.program.tessellation.phases[0].kind = DXBC_HULL_PHASE_CONTROL_POINT;
    CHECK(source_rejected(&fixture.program));
    fixture.program.tessellation.phases[0].kind = kind;
    fixture.program.instructions[0].operands[1].has_neg = true;
    CHECK(source_rejected(&fixture.program));
    fixture.program.instructions[0].operands[1].has_neg = false;
    fixture.program.instructions[1].operands[0].rel_op0->swizzle[0] = 1;
    CHECK(source_rejected(&fixture.program));
    fixture.program.instructions[1].operands[0].rel_op0->swizzle[0] = 0;
    fixture.program.index_ranges[0].hull_phase_index = 1;
    CHECK(source_rejected(&fixture.program));
    fixture.program.index_ranges[0].hull_phase_index = 0;
    fixture.program.tessellation.phases[1].first_instruction_index--;
    CHECK(source_rejected(&fixture.program));
    fixture.program.tessellation.phases[1].first_instruction_index++;
    hull_fixture_dispose(&fixture);
    return true;
}

typedef struct {
    const USILProgram *program;
    unsigned units, expressions;
    bool bad_owner, reject;
} HullLedger;

static bool observe_hull(void *context, const HLSLSourceQualityObservation *observation) {
    HullLedger *ledger = context;
    if (ledger->reject) return false;
    if (observation->stage != DXBC_PROGRAM_TYPE_HULL || observation->pass_index != 4 ||
        observation->entry_point_index != 3 || observation->source_unit_id > 2)
        ledger->bad_owner = true;
    ledger->units |= 1u << observation->source_unit_id;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (facts->instruction_index >= 0 &&
        (facts->instruction_index >= ledger->program->instruction_count ||
         facts->source_instruction_index != ledger->program->instructions[facts->instruction_index].source_instruction_index))
        ledger->bad_owner = true;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION) ++ledger->expressions;
    return true;
}

static bool arithmetic_and_phase_ownership(void) {
    HullFixture fixture;
    CHECK(hull_fixture_init(&fixture, 3, 1));
    HullLedger ledger = {.program = &fixture.program};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    options.source_quality = &quality;
    options.expression_source_map = &map;
    options.source_quality_pass_index = 4;
    options.source_quality_entry_point_index = 3;
    options.source_quality_observer = observe_hull;
    options.source_quality_observer_context = &ledger;
    const char *reserved[] = {"HullPoint", "factors", "pointIndex", "patchConstants"};
    options.reserved_preprocessor_identifiers = reserved;
    options.reserved_preprocessor_identifier_count = 4;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "1.5f * 2.0f"));
    CHECK(strstr(source.buf, "struct HullPoint_1"));
    CHECK(strstr(source.buf, "return patch[pointIndex_1]"));
    CHECK(!ledger.bad_owner && ledger.units == 7 && ledger.expressions >= 6);
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && quality.counts.unknown_provenance == 0);
    CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
    /* The inlined producer owns the same root span without losing its raw index. */
    CHECK(map.origins[1].source_begin >= map.origins[2].source_begin);
    CHECK(map.origins[1].source_end <= map.origins[2].source_end);
    sb_free(&source);
    ledger.reject = true;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    sb_free(&source);
    /* The same temp register is declared in phase0 but undefined in phase1. */
    hull_fixture_dispose(&fixture);
    CHECK(hull_fixture_init(&fixture, 3, 2));
    CHECK(source_rejected(&fixture.program));
    hull_fixture_dispose(&fixture);
    return true;
}

static bool malformed_contracts(void) {
    HullFixture fixture;
    CHECK(hull_fixture_init(&fixture, 3, 0));
    USILProgram *program = &fixture.program;
    program->instructions[0].opcode = USIL_OP_ADD;
    program->instructions[0].operand_count = 3;
    program->instructions[0].operands[2] = program->instructions[0].operands[1];
    CHECK(source_rejected(program));
    program->instructions[0].opcode = USIL_OP_MOV;
    program->instructions[0].operand_count = 2;
    program->instructions[1].has_resource_dimension = true;
    CHECK(source_rejected(program));
    program->instructions[1].has_resource_dimension = false;
    program->signature_declarations[1].source_instruction_index =
        program->tessellation.phases[1].first_source_instruction_index;
    CHECK(source_rejected(program));
    program->signature_declarations[1].source_instruction_index = 11;
    program->patch_constants[2].semantic_index = 1;
    CHECK(source_rejected(program));
    program->patch_constants[2].semantic_index = 2;
    program->inputs[0].mask = 7;
    CHECK(source_rejected(program));
    program->inputs[0].mask = 15;
    program->tessellation.domain = DXBC_TESSELLATOR_DOMAIN_QUAD;
    CHECK(source_rejected(program));
    program->tessellation.domain = DXBC_TESSELLATOR_DOMAIN_TRIANGLE;
    program->tessellation.phases[1].kind = DXBC_HULL_PHASE_JOIN;
    CHECK(source_rejected(program));
    program->tessellation.phases[1].kind = DXBC_HULL_PHASE_FORK;
    program->tessellation.max_tessellation_factor_bits = 0x7fc00000;
    CHECK(source_rejected(program));
    program->tessellation.max_tessellation_factor_bits = 0x42000000;
    program->has_parsed_signature_authority = false;
    CHECK(source_rejected(program));
    program->has_parsed_signature_authority = true;
    /* Declaration order does not supply semantic ownership. */
    DXBCSignatureElement temporary = program->patch_constants[0];
    program->patch_constants[0] = program->patch_constants[3];
    program->patch_constants[3] = temporary;
    CHECK(hlsl_high_level_hull_source_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    /* These actual syntax/formatter tokens cannot be renamed. A caller's
     * legal preprocessor identifier must not rewrite the generated grammar. */
    const char *reserved[] = {"SV_TessFactor", "struct", "for", "return", "const",
        "float2", "float3", "asfloat", "abs", "mad"};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.reserved_preprocessor_identifier_count = 1;
    for (size_t index = 0; index < sizeof(reserved) / sizeof(reserved[0]); ++index) {
        options.reserved_preprocessor_identifiers = &reserved[index];
        StringBuilder source;
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(program, &source, NULL, NULL, NULL, &options));
        CHECK(source.len == 0);
        sb_free(&source);
    }
    hull_fixture_dispose(&fixture);
    return true;
}

static bool reordered_phase_roles(void) {
    HullFixture fixture;
    CHECK(hull_fixture_init(&fixture, 3, 3));
    CHECK(fixture.program.tessellation.phases[0].instance_count == 1);
    CHECK(fixture.program.tessellation.phases[1].instance_count == 3);
    bool inner_first = false;
    CHECK(hlsl_domain_factor_order(&fixture.program, &inner_first) && inner_first);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
    const char *inner = strstr(source.buf, "float inner : SV_InsideTessFactor");
    const char *outer = strstr(source.buf, "float outer[3] : SV_TessFactor");
    CHECK(inner && outer && inner < outer);
    CHECK(strstr(source.buf, "factors.inner = 4.0f") < strstr(source.buf, "factors.outer[factorIndex] = 3.0f"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
    sb_free(&source);
    /* The relative index base is the owned range base, not an arbitrary
     * immediate offset added to the loop variable. */
    fixture.program.instructions[3].operands[0].index_values[0] = 2;
    CHECK(source_rejected(&fixture.program));
    fixture.program.instructions[3].operands[0].index_values[0] = 1;
    fixture.program.index_ranges[0].hull_phase_index = 0;
    CHECK(source_rejected(&fixture.program));
    hull_fixture_dispose(&fixture);
    return true;
}

static bool scoped_cfg_ownership(void) {
    HullFixture fixture;
    CHECK(hull_fixture_init(&fixture, 3, 0));
    HLSLEmitterContext ctx = {.program = &fixture.program};
    CHECK(build_control_flow_graph_range(&ctx, 3, 5));
    CHECK(ctx.cfg.instruction_block[0] == -1 && ctx.cfg.instruction_block[2] == -1);
    CHECK(!instructions_have_unambiguous_path(&ctx, 0, 1));
    CHECK(compute_dominance(&ctx.cfg));
    CHECK(build_hlsl_ssa_graph(&ctx));
    CHECK(hlsl_relative_operand_definition(&ctx, 1, 0, 0) == HLSL_DEFINITION_UNKNOWN);
    free_hlsl_ssa_graph(&ctx);
    free_control_flow_graph(&ctx);
    CHECK(build_control_flow_graph_range(&ctx, 0, 3));
    CHECK(compute_dominance(&ctx.cfg));
    CHECK(build_hlsl_ssa_graph(&ctx));
    CHECK(hlsl_relative_operand_definition(&ctx, 1, 0, 0) == 0);
    free_hlsl_ssa_graph(&ctx);
    free_control_flow_graph(&ctx);
    CHECK(!build_control_flow_graph_range(&ctx, -1, 3));
    CHECK(!build_control_flow_graph_range(&ctx, 3, 3));
    CHECK(!build_control_flow_graph_range(&ctx, 0, 6));
    hull_fixture_dispose(&fixture);
    return true;
}

static bool other_domains_and_control_point_counts(void) {
    for (unsigned shape = 0; shape < 2; ++shape) {
        const bool isoline = shape != 0;
        size_t size = 0;
        uint8_t *bytes = test_tessellation_hull_shape_dxbc(isoline, &size);
        HullFixture fixture;
        CHECK(hull_fixture_parse(&fixture, bytes, size));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        options.source_quality = &quality;
        options.expression_source_map = &map;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        CHECK(strstr(source.buf, isoline ? "[domain(\"isoline\")]" : "[domain(\"quad\")]"));
        CHECK(strstr(source.buf, isoline ? "float outer[2]" : "float outer[4]"));
        CHECK(isoline ? !strstr(source.buf, "SV_InsideTessFactor") : strstr(source.buf, "float inner[2]") != NULL);
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
        sb_free(&source);
        if (!isoline) {
            /* USIL reflection collapses quad edge tags into one enum. The
             * natural producer must still preserve their individual roles. */
            fixture.program.signature_declarations[1].system_value_name = 12;
            CHECK(usil_signature_authority_is_valid(&fixture.program));
            CHECK(source_rejected(&fixture.program));
            fixture.program.signature_declarations[1].system_value_name = 11;
            fixture.program.instructions[4].operands[0].index_values[0] = 5;
            CHECK(source_rejected(&fixture.program));
        }
        hull_fixture_dispose(&fixture);
    }
    const unsigned counts[] = {1, 4, 32};
    for (size_t index = 0; index < 3; ++index) {
        HullFixture fixture;
        CHECK(hull_fixture_init(&fixture, counts[index], 0));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        char expected[64];
        snprintf(expected, sizeof(expected), "[outputcontrolpoints(%u)]", counts[index]);
        CHECK(strstr(source.buf, expected));
        sb_free(&source);
        fixture.program.tessellation.output_control_point_count = counts[index] == 32 ? 31 : counts[index] + 1;
        CHECK(source_rejected(&fixture.program));
        hull_fixture_dispose(&fixture);
    }
    return true;
}

typedef struct {
    HullLedger owners;
    HullFixture *fixture;
    size_t events, inner_value_event, reject_event, mutate_event;
    bool rejected, mutated;
} QuadScopeLedger;

static bool observe_quad_scopes(void *context, const HLSLSourceQualityObservation *observation) {
    QuadScopeLedger *ledger = context;
    if (!observe_hull(&ledger->owners, observation)) return false;
    const size_t event = ++ledger->events;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION &&
        observation->facts.instruction_index == 4 && !ledger->inner_value_event)
        ledger->inner_value_event = event;
    if (ledger->mutate_event == event) {
        DXBCOperand *value = &ledger->fixture->program.instructions[4].operands[1];
        value->imm_values[0] = value->immediate_words[0] = UINT32_C(0x40a00000);
        ledger->mutated = true;
    }
    if (ledger->reject_event == event) { ledger->rejected = true; return false; }
    return true;
}

/* FXC scopes a for-loop variable in its enclosing block. Keep the two quad
 * phase loops in independent lexical blocks while retaining their raw owners. */
static bool quad_factor_lexical_scopes(void) {
    size_t size = 0;
    uint8_t *bytes = test_tessellation_hull_shape_dxbc(false, &size);
    HullFixture fixture;
    CHECK(hull_fixture_parse(&fixture, bytes, size));
    CHECK(fixture.program.instruction_count == 6 && fixture.program.tessellation.phase_count == 2 &&
          fixture.program.instructions[4].opcode == USIL_OP_MOV && fixture.program.instructions[4].operands[1].imm_value_count == 1);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult normal_quality, captured_quality, independent_quality;
    HLSLExpressionSourceMap normal_map = {0}, captured_map = {0};
    options.source_quality = &normal_quality; options.expression_source_map = &normal_map;
    options.source_quality_pass_index = 4; options.source_quality_entry_point_index = 3;
    QuadScopeLedger ledger = {.owners = {.program = &fixture.program}};
    options.source_quality_observer = observe_quad_scopes; options.source_quality_observer_context = &ledger;
    StringBuilder normal, source, independent_source;
    sb_init(&normal); sb_init(&source); sb_init(&independent_source);
    CHECK(hlsl_emit_with_options(&fixture.program, &normal, NULL, NULL, NULL, &options));
    static const char scoped_loops[] =
        "    {\n"
        "        for (uint factorIndex = 0; factorIndex < 4; ++factorIndex) {\n"
        "            factors.outer[factorIndex] = 2.0f;\n"
        "        }\n"
        "    }\n"
        "    {\n"
        "        for (uint factorIndex = 0; factorIndex < 2; ++factorIndex) {\n"
        "            factors.inner[factorIndex] = 3.0f;\n"
        "        }\n"
        "    }\n"
        "    return factors;\n";
    CHECK(strstr(normal.buf, scoped_loops) && !strstr(normal.buf, "\n    for (uint factorIndex") &&
          normal_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !normal_quality.counts.incomplete_units &&
          !ledger.owners.bad_owner && ledger.owners.units == 7 && ledger.inner_value_event && ledger.events > ledger.inner_value_event &&
          hlsl_expression_source_map_matches(&normal_map, &fixture.program, normal.buf));
    HLSLStageCoverage retained = {0}, independent = {0};
    options.source_quality = &captured_quality; options.expression_source_map = &captured_map;
    ledger = (QuadScopeLedger){.owners = {.program = &fixture.program}};
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &retained, NULL));
    CHECK(source.len == normal.len && !memcmp(source.buf, normal.buf, source.len + 1) &&
          hlsl_source_quality_results_equal(&normal_quality, &captured_quality) &&
          normal_map.count == captured_map.count && captured_map.complete && retained.unit_count == 3 &&
          hlsl_stage_coverage_validate(&retained, &source));
    for (size_t index = 0; index < normal_map.count; ++index)
        CHECK(hlsl_expression_origins_equal(&normal_map.origins[index], &captured_map.origins[index]));
    for (size_t phase = 0; phase < fixture.program.tessellation.phase_count; ++phase) {
        const USILHullPhase *scope = &fixture.program.tessellation.phases[phase];
        const HLSLExpressionOrigin *closing = &captured_map.origins[scope->end_instruction_index - 1];
        static const char two_closings[] = "        }\n    }\n";
        CHECK(closing->source_end - closing->source_begin == sizeof(two_closings) - 1 &&
              !memcmp(source.buf + closing->source_begin, two_closings, sizeof(two_closings) - 1));
        const HLSLExpressionOrigin *index = &captured_map.origins[scope->first_instruction_index];
        CHECK(index->source_end - index->source_begin == strlen("factorIndex") &&
              !memcmp(source.buf + index->source_begin, "factorIndex", strlen("factorIndex")) &&
              index->source_begin >= strlen("        for (uint ") &&
              !memcmp(source.buf + index->source_begin - strlen("        for (uint "), "        for (uint ", strlen("        for (uint ")));
    }
    const size_t value_event = ledger.inner_value_event, final_event = ledger.events;
    options.source_quality = &independent_quality; options.expression_source_map = NULL;
    options.source_quality_observer = NULL; options.source_quality_observer_context = NULL;
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &independent_source, NULL, NULL, NULL, &options, &independent, NULL));
    CHECK(source.len == independent_source.len && !memcmp(source.buf, independent_source.buf, source.len + 1) &&
          hlsl_source_quality_results_equal(&captured_quality, &independent_quality) &&
          hlsl_stage_coverage_equal(&retained, &independent));
    const DXBCOperand original_value = fixture.program.instructions[4].operands[1];
    for (unsigned action = 0; action < 5; ++action) {
        QuadScopeLedger failure = {.owners = {.program = &fixture.program}, .fixture = &fixture};
        if (action < 3) failure.reject_event = action == 0 ? 1 : action == 1 ? value_event : final_event;
        else failure.mutate_event = action == 3 ? value_event : final_event;
        options.source_quality = &independent_quality; options.expression_source_map = &captured_map;
        options.source_quality_observer = observe_quad_scopes;
        options.source_quality_observer_context = &failure;
        HLSLStageCoverage rejected = {0};
        HLSLEmitDiagnostic diagnostic;
        StringBuilder rejected_source; sb_init(&rejected_source);
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &rejected_source, NULL, NULL, NULL, &options, &rejected, &diagnostic));
        CHECK((failure.rejected || failure.mutated) && diagnostic.status != HLSL_EMIT_STATUS_OK &&
              independent_quality.classification != HLSL_SOURCE_QUALITY_CLEAN && !captured_map.complete && !captured_map.count &&
              !rejected.began && !rejected.finished && !rejected.source && !rejected.roots && !rejected.unit_count);
        fixture.program.instructions[4].operands[1] = original_value;
        sb_free(&rejected_source); sb_init(&rejected_source);
        options.source_quality_observer = NULL; options.source_quality_observer_context = NULL;
        CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &rejected_source, NULL, NULL, NULL, &options, &rejected, &diagnostic));
        CHECK(rejected_source.len == source.len && !memcmp(rejected_source.buf, source.buf, source.len + 1) &&
              hlsl_source_quality_results_equal(&captured_quality, &independent_quality) &&
              hlsl_stage_coverage_equal(&retained, &rejected));
        hlsl_stage_coverage_dispose(&rejected); sb_free(&rejected_source);
        options.source_quality_observer = observe_quad_scopes;
    }
    hull_fixture_dispose(&fixture);
    CHECK(hlsl_stage_coverage_validate(&retained, &source) && hlsl_stage_coverage_validate(&independent, &independent_source) &&
          hlsl_stage_coverage_equal(&retained, &independent));
    hlsl_stage_coverage_dispose(&retained); hlsl_stage_coverage_dispose(&independent);
    sb_free(&independent_source); sb_free(&source); sb_free(&normal);
    return true;
}

static bool explicit_control_point_phase(void) {
    const unsigned counts[] = {1, 3, 4, 32};
    for (size_t index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
        HullFixture fixture;
        CHECK(hull_fixture_init(&fixture, counts[index], 4));
        CHECK(fixture.program.tessellation.phase_count == 3);
        CHECK(fixture.program.tessellation.phases[0].kind == DXBC_HULL_PHASE_CONTROL_POINT);
        CHECK(usil_signature_authority_is_valid(&fixture.program));
        CHECK(!hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_RECOMPILE));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        HullLedger ledger = {.program = &fixture.program};
        options.source_quality = &quality;
        options.expression_source_map = &map;
        options.source_quality_observer = observe_hull;
        options.source_quality_observer_context = &ledger;
        options.source_quality_pass_index = 4;
        options.source_quality_entry_point_index = 3;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        CHECK(strstr(source.buf, "controlPoint.clipPosition ="));
        CHECK(strstr(source.buf, "patch[pointIndex].clipPosition"));
        CHECK(!strstr(source.buf, "return patch[pointIndex];"));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(!ledger.bad_owner && ledger.units == 7);
        CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
        CHECK(map.origins[0].kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION);
        CHECK(map.origins[0].source_end > map.origins[0].source_begin);
        CHECK(map.origins[2].kind == HLSL_EXPRESSION_ORIGIN_RETURN);
        CHECK(map.origins[2].source_end > map.origins[2].source_begin);
        sb_free(&source);
        hull_fixture_dispose(&fixture);
    }
    HullFixture fixture;
    CHECK(hull_fixture_init(&fixture, 3, 5));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    options.source_quality = &quality;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(map.origins[0].source_begin == map.origins[1].source_begin);
    CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
    sb_free(&source);
    /* The first phase's relative identity is not a global temporary fact. */
    CHECK(fixture.program.temp_count == 2);
    CHECK(fixture.program.tessellation.phases[0].temp_count == 2);
    CHECK(fixture.program.tessellation.phases[1].temp_count == 1);
    CHECK(usil_hull_phase_temp_registers_are_valid(&fixture.program));
    fixture.program.tessellation.phases[0].temp_count = 1;
    fixture.program.tessellation.phases[1].temp_count = 2;
    CHECK(!usil_hull_phase_temp_registers_are_valid(&fixture.program));
    CHECK(source_rejected(&fixture.program));
    fixture.program.tessellation.phases[0].temp_count = 2;
    fixture.program.tessellation.phases[1].temp_count = 1;
    fixture.program.tessellation.phases[0].temp_count_source_instruction_index =
        fixture.program.tessellation.phases[1].first_source_instruction_index;
    CHECK(!usil_hull_phase_temp_registers_are_valid(&fixture.program));
    CHECK(source_rejected(&fixture.program));
    fixture.program.tessellation.phases[0].temp_count_source_instruction_index =
        fixture.program.tessellation.phases[0].first_source_instruction_index + 3;
    fixture.program.temp_count = 3;
    CHECK(!usil_hull_phase_temp_registers_are_valid(&fixture.program));
    CHECK(source_rejected(&fixture.program));
    fixture.program.temp_count = 2;
    CHECK(usil_hull_phase_temp_registers_are_valid(&fixture.program));
    fixture.program.instructions[1].operands[1].register_index = 1;
    fixture.program.instructions[1].operands[1].index_values[0] = 1;
    CHECK(source_rejected(&fixture.program));
    fixture.program.instructions[1].operands[1].register_index = 0;
    fixture.program.instructions[1].operands[1].index_values[0] = 0;
    fixture.program.instructions[1].opcode = USIL_OP_ADD;
    CHECK(source_rejected(&fixture.program));
    fixture.program.instructions[1].opcode = USIL_OP_MOV;
    DXBCOperand *point = &fixture.program.instructions[2].operands[2];
    point->rel_op0->rel_op0 = point->rel_op0;
    CHECK(!usil_hull_phase_temp_registers_are_valid(&fixture.program));
    CHECK(source_rejected(&fixture.program));
    point->rel_op0->rel_op0 = NULL;
    point->index_values[1] = 1;
    point->rel_offset0 = 1;
    CHECK(source_rejected(&fixture.program));
    point->index_values[1] = 0;
    point->rel_offset0 = 0;
    point->swizzle[0] = 1;
    CHECK(source_rejected(&fixture.program));
    point->swizzle[0] = 0;
    fixture.program.instructions[2].operands[0].destination_mask = 3;
    CHECK(source_rejected(&fixture.program));
    fixture.program.instructions[2].operands[0].destination_mask = 240;
    USILSignatureDeclaration *input = &fixture.program.signature_declarations[1];
    input->array_element_count = 4;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    CHECK(source_rejected(&fixture.program));
    input->array_element_count = 3;
    input->source_instruction_index = fixture.program.tessellation.phases[0].marker_source_instruction_index;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    input->source_instruction_index = fixture.program.tessellation.phases[0].first_source_instruction_index + 1;
    fixture.program.inputs[0].system_value = 2;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    fixture.program.inputs[0].system_value = 1;
    fixture.program.outputs[0].system_value = 2;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    fixture.program.outputs[0].system_value = 1;
    fixture.program.tessellation.phases[0].kind = DXBC_HULL_PHASE_FORK;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    CHECK(source_rejected(&fixture.program));
    fixture.program.tessellation.phases[0].kind = DXBC_HULL_PHASE_CONTROL_POINT;
    fixture.program.tessellation.phases[1].kind = DXBC_HULL_PHASE_CONTROL_POINT;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    CHECK(source_rejected(&fixture.program));
    fixture.program.tessellation.phases[1].kind = DXBC_HULL_PHASE_FORK;
    input->mask = 3;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    input->mask = 15;
    fixture.program.inputs[0].component_type = 1;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    fixture.program.inputs[0].component_type = 3;
    fixture.program.program_type = DXBC_PROGRAM_TYPE_DOMAIN;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    fixture.program.program_type = DXBC_PROGRAM_TYPE_GEOMETRY;
    fixture.program.geometry.valid = true;
    fixture.program.geometry.input_vertex_count = 3;
    CHECK(!usil_signature_authority_is_valid(&fixture.program));
    fixture.program.program_type = DXBC_PROGRAM_TYPE_HULL;
    fixture.program.geometry.valid = false;
    CHECK(usil_signature_authority_is_valid(&fixture.program));
    CHECK(hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    /* A later larger declaration must never authorize an earlier phase's
     * register use. Reproject the actual authored semantic token model. */
    unsigned declarations = 0;
    for (int index = 0; index < fixture.semantic.instruction_count; ++index)
        if (fixture.semantic.instructions[index].opcode == 104)
            fixture.semantic.instructions[index].operands[0].register_index = ++declarations == 1 ? 1 : 2;
    USILProgram rejected = {0};
    CHECK(!usil_translate_with_stage_contract(&rejected, &fixture.semantic, &fixture.contract));
    usil_free(&rejected);
    hull_fixture_dispose(&fixture);
    return true;
}

static bool differing_control_point_counts(void) {
    const uint8_t pairs[][2] = {{4, 3}, {32, 3}, {3, 1}, {32, 31}, {2, 1}};
    for (size_t index = 0; index < sizeof(pairs) / sizeof(pairs[0]); ++index) {
        HullFixture fixture;
        const uint8_t inputs = pairs[index][0], outputs = pairs[index][1];
        CHECK(hull_fixture_init_counts(&fixture, inputs, outputs, 5));
        CHECK(fixture.contract.input_control_point_count == inputs);
        CHECK(fixture.contract.output_control_point_count == outputs);
        CHECK(fixture.program.tessellation.input_control_point_count == inputs);
        CHECK(fixture.program.tessellation.output_control_point_count == outputs);
        CHECK(fixture.program.signature_declarations[1].array_element_count == inputs);
        CHECK(usil_signature_authority_is_valid(&fixture.program));
        CHECK(hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        options.source_quality = &quality;
        options.expression_source_map = &map;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
        CHECK(map.origins[0].source_begin == map.origins[1].source_begin);
        char expected[64];
        snprintf(expected, sizeof(expected), "InputPatch<HullPoint, %u>", (unsigned)inputs);
        CHECK(strstr(source.buf, expected));
        snprintf(expected, sizeof(expected), "[outputcontrolpoints(%u)]", (unsigned)outputs);
        CHECK(strstr(source.buf, expected));
        CHECK(strstr(source.buf, "controlPoint.clipPosition ="));
        CHECK(!strstr(source.buf, "return patch[pointIndex];"));
        sb_free(&source);
        fixture.program.tessellation.output_control_point_count = 0;
        CHECK(source_rejected(&fixture.program));
        fixture.program.tessellation.output_control_point_count = 33;
        CHECK(source_rejected(&fixture.program));
        fixture.program.tessellation.output_control_point_count = outputs;
        fixture.program.tessellation.input_control_point_count = 0;
        CHECK(source_rejected(&fixture.program));
        fixture.program.tessellation.input_control_point_count = 33;
        CHECK(source_rejected(&fixture.program));
        fixture.program.tessellation.input_control_point_count = inputs;
        fixture.program.signature_declarations[1].array_element_count = outputs;
        CHECK(!usil_signature_authority_is_valid(&fixture.program));
        CHECK(source_rejected(&fixture.program));
        fixture.program.signature_declarations[1].array_element_count = inputs;
        fixture.program.instructions[0].operands[1].type = OPERAND_TYPE_FORK_INSTANCE_ID;
        CHECK(source_rejected(&fixture.program));
        fixture.program.instructions[0].operands[1].type = OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID;
        fixture.program.instructions[1].opcode = USIL_OP_ADD;
        fixture.program.instructions[1].operand_count = 3;
        fixture.program.instructions[1].operands[2] = fixture.program.instructions[1].operands[1];
        CHECK(usil_instruction_shape_valid(&fixture.program, &fixture.program.instructions[1]));
        CHECK(source_rejected(&fixture.program));
        fixture.program.instructions[1].opcode = USIL_OP_MOV;
        fixture.program.instructions[1].operand_count = 2;
        memset(&fixture.program.instructions[1].operands[2], 0, sizeof(DXBCOperand));
        CHECK(hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
        hull_fixture_dispose(&fixture);
    }
    HullFixture fixture;
    CHECK(hull_fixture_init_counts(&fixture, 3, 4, 4));
    CHECK(usil_signature_authority_is_valid(&fixture.program));
    CHECK(!hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    CHECK(source_rejected(&fixture.program));
    hull_fixture_dispose(&fixture);
    CHECK(hull_fixture_init_counts(&fixture, 4, 3, 0));
    CHECK(!hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    CHECK(source_rejected(&fixture.program));
    hull_fixture_dispose(&fixture);
    return true;
}

static bool owned_empty_hull_metadata(void) {
    for (unsigned scenario = 0; scenario <= 4; scenario += 4) {
        HullFixture fixture;
        CHECK(hull_fixture_init(&fixture, 3, (uint8_t)scenario));
        int platform = 4;
        SerializedSubProgram sub = {.program_type = 21};
        SerializedSubProgramIdentity identity = {.hardware_tier_group = 3};
        SerializedPass pass = {.has_serialized_platforms = true, .platform_count = 1,
            .platforms = &platform, .program_mask = 16};
        pass.subprogram_count[3] = 1; pass.subprograms[3] = &sub;
        pass.subprogram_identities[3] = &identity;
        PlayerSubProgramMetadata player = {.program_type = 21, .has_player_blob_header = true};
        SerializedConstantBuffer shell = {.name = "$Globals", .role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS};
        SerializedProgramParameters parameters = {.cb_count = 1, .constant_buffers = &shell};
        HLSLGlobalDeclarationWitness witness = {0, &player, &parameters};
        HLSLGlobalDeclarationUnion *declarations = NULL;
        CHECK(hlsl_global_declarations_build(&pass, 3, 0, &witness, 1, &declarations, NULL) == HLSL_GLOBAL_DECLARATIONS_OK);
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        HLSLEmitDiagnostic diagnostic;
        options.source_quality = &quality; options.expression_source_map = &map;
        StringBuilder original, owned;
        sb_init(&original); sb_init(&owned);
        CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &original, NULL, NULL, NULL, &options, &diagnostic));
        options.global_declarations = declarations;
        CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &owned, &parameters, NULL, NULL, &options, &diagnostic));
        CHECK(original.len == owned.len && !memcmp(original.buf, owned.buf, original.len));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.counts.unknown_provenance &&
              !quality.counts.incomplete_units && hlsl_expression_source_map_matches(&map, &fixture.program, owned.buf));
        sb_free(&owned); sb_init(&owned);
        shell.size = 16;
        CHECK(!hlsl_emit_with_options_diagnostic(&fixture.program, &owned, &parameters, NULL, NULL, &options, &diagnostic));
        CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        sb_free(&owned); sb_init(&owned); shell.size = 0;
        CHECK(!hlsl_emit_with_options_diagnostic(&fixture.program, &owned, NULL, &parameters, NULL, &options, &diagnostic));
        sb_free(&owned); sb_init(&owned); options.global_declarations = NULL;
        CHECK(!hlsl_emit_with_options_diagnostic(&fixture.program, &owned, &parameters, NULL, NULL, &options, &diagnostic));
        sb_free(&owned); sb_free(&original);
        hlsl_global_declarations_free(declarations); hull_fixture_dispose(&fixture);
    }
    return true;
}

static bool implicit_float3_points(void) {
    static const char *const semantics[] = {"INTERNALTESSPOS", "POINTVALUE", "arbitraryValue"};
    static const uint8_t scenarios[] = {0, 1, 3};
    const unsigned counts[] = {1, 3, 32};
    for (size_t test = 0; test < sizeof(semantics) / sizeof(semantics[0]); ++test) {
        size_t size = 0;
        uint8_t *bytes = test_tessellation_hull_float3_dxbc(counts[test], counts[test],
            scenarios[test], semantics[test], &size);
        HullFixture fixture;
        CHECK(hull_fixture_parse(&fixture, bytes, size));
        CHECK(usil_signature_authority_is_valid(&fixture.program));
        CHECK(fixture.program.inputs[0].mask == 7 && fixture.program.inputs[0].rw_mask == 7 &&
              fixture.program.outputs[0].mask == 7 && fixture.program.outputs[0].rw_mask == 8);
        CHECK(hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
        CHECK(!hlsl_high_level_hull_source_supported(&fixture.program, HLSL_EMIT_MODE_RECOMPILE));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        HullLedger ledger = {.program = &fixture.program};
        const char *reserved[] = {"pointValue", "HullPoint", "patchConstants"};
        options.reserved_preprocessor_identifiers = reserved;
        options.reserved_preprocessor_identifier_count = 3;
        options.source_quality = &quality;
        options.expression_source_map = &map;
        options.source_quality_observer = observe_hull;
        options.source_quality_observer_context = &ledger;
        options.source_quality_pass_index = 4;
        options.source_quality_entry_point_index = 3;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        char expected[256];
        snprintf(expected, sizeof(expected), "float3 pointValue_1 : %s;", semantics[test]);
        CHECK(strstr(source.buf, expected));
        CHECK(strstr(source.buf, "return patch[pointIndex];"));
        CHECK(!strstr(source.buf, "clipPosition") && !strstr(source.buf, "float4"));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.counts.unknown_provenance &&
              !quality.counts.incomplete_units && quality.counts.inspected_units == 3);
        CHECK(!ledger.bad_owner && ledger.units == 7 && ledger.expressions);
        CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
        sb_free(&source);
        const char *semantic_reserved[] = {semantics[test]};
        options.reserved_preprocessor_identifiers = semantic_reserved;
        options.reserved_preprocessor_identifier_count = 1;
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        sb_free(&source);
        fixture.program.tessellation.output_control_point_count = counts[test] == 32 ? 31 : counts[test] + 1;
        CHECK(source_rejected(&fixture.program));
        hull_fixture_dispose(&fixture);
    }
    /* An independently decoded custom FLOAT3 CP arithmetic phase remains
     * outside the existing FLOAT4 expression producer. */
    size_t size = 0;
    uint8_t *bytes = test_tessellation_hull_float3_dxbc(3, 3, 4, "POINTVALUE", &size);
    HullFixture fixture;
    CHECK(hull_fixture_parse(&fixture, bytes, size));
    CHECK(usil_signature_authority_is_valid(&fixture.program));
    CHECK(fixture.program.tessellation.phases[0].kind == DXBC_HULL_PHASE_CONTROL_POINT);
    CHECK(source_rejected(&fixture.program));
    hull_fixture_dispose(&fixture);
    static const char *const rejected[] = {"point", "POINTVALUE1"};
    for (size_t test = 0; test < sizeof(rejected) / sizeof(rejected[0]); ++test) {
        size = 0;
        bytes = test_tessellation_hull_float3_dxbc(3, 3, 0, rejected[test], &size);
        CHECK(hull_fixture_parse(&fixture, bytes, size));
        CHECK(source_rejected(&fixture.program));
        hull_fixture_dispose(&fixture);
    }
    /* Invalid system-semantic encodings and nonidentifier spellings are
     * rejected by the shared signature decoder before source admission. */
    static const char *const invalid[] = {"SV_Position", "sv_Custom", "bad-name"};
    for (size_t test = 0; test < sizeof(invalid) / sizeof(invalid[0]); ++test) {
        size = 0;
        bytes = test_tessellation_hull_float3_dxbc(3, 3, 0, invalid[test], &size);
        CHECK(bytes);
        DXBCDocument document;
        DXBCContainer semantic = {0};
        dxbc_document_init(&document);
        CHECK(dxbc_document_parse(&document, bytes, size, NULL));
        free(bytes);
        CHECK(!dxbc_document_decode_semantic(&document, &semantic));
        dxbc_free(&semantic);
        dxbc_document_free(&document);
    }
    return true;
}

int main(void) {
    return natural_hull_source() && arithmetic_and_phase_ownership() &&
        malformed_contracts() && reordered_phase_roles() && scoped_cfg_ownership() &&
        other_domains_and_control_point_counts() && quad_factor_lexical_scopes() && explicit_control_point_phase() &&
        differing_control_point_counts() && owned_empty_hull_metadata() && implicit_float3_points() ? 0 : 1;
}
