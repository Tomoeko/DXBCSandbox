// SPDX-License-Identifier: GPL-3.0-only
#include "test_geometry_fixture.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
            return false;                                                                          \
        }                                                                                          \
    } while (0)


typedef struct {
    DXBCDocument document;
    DXBCContainer semantic;
    DXBCStageContract contract;
    USILProgram program;
    HLSLEmitOptions options;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
} Fixture;

static bool fixture_init_primitive(Fixture *fixture, bool explicit_stream, bool arithmetic,
    DXBCInputPrimitive primitive, uint32_t vertices, uint32_t selected_vertex, uint8_t color_mask) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    size_t size = 0;
    uint8_t *bytes = test_geometry_dxbc_primitive(explicit_stream, arithmetic,
        primitive, vertices, selected_vertex, color_mask, &size);
    CHECK(bytes);
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContractDiagnostic contract_diagnostic;
    CHECK(dxbc_document_parse(&fixture->document, bytes, size, &document_diagnostic));
    free(bytes);
    CHECK(dxbc_document_decode_semantic(&fixture->document, &fixture->semantic));
    CHECK(dxbc_stage_contract_decode(&fixture->document, &fixture->semantic,
                                     &fixture->contract, &contract_diagnostic));
    CHECK(usil_translate_with_stage_contract(&fixture->program, &fixture->semantic,
                                             &fixture->contract));
    CHECK(fixture->program.instruction_count == 8 &&
          fixture->program.has_parsed_signature_authority && fixture->program.geometry.valid);
    fixture->options = (HLSLEmitOptions)HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    fixture->options.expression_source_map = &fixture->map;
    fixture->options.source_quality = &fixture->quality;
    fixture->options.source_quality_pass_index = 4;
    fixture->options.source_quality_entry_point_index = 2;
    return true;
}

static bool fixture_init(Fixture *fixture, bool explicit_stream, bool arithmetic) {
    return fixture_init_primitive(fixture, explicit_stream, arithmetic,
        DXBC_INPUT_PRIMITIVE_POINT, 1, 0, 15);
}

static void fixture_free(Fixture *fixture) {
    usil_free(&fixture->program);
    dxbc_stage_contract_free(&fixture->contract);
    dxbc_free(&fixture->semantic);
    dxbc_document_free(&fixture->document);
}

static bool emit(Fixture *fixture, StringBuilder *source) {
    sb_init(source);
    return hlsl_emit_with_options(&fixture->program, source, NULL, NULL, NULL, &fixture->options);
}

static unsigned occurrences(const char *source, const char *needle) {
    unsigned count = 0;
    for (const char *found = strstr(source, needle); found; found = strstr(found + 1, needle))
        ++count;
    return count;
}

typedef struct {
    const USILProgram *program;
    uint64_t effects;
    uint64_t logical_input_ids;
    bool reject_first_effect;
    bool invalid_owner;
} Ledger;

static bool observe(void *context, const HLSLSourceQualityObservation *observation) {
    Ledger *ledger = context;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (observation->stage != DXBC_PROGRAM_TYPE_GEOMETRY || observation->pass_index != 4 ||
        observation->entry_point_index != 2 || observation->source_unit_id != 0)
        ledger->invalid_owner = true;
    if (facts->instruction_index >= 0) {
        if (facts->instruction_index >= ledger->program->instruction_count ||
            facts->source_instruction_index !=
                ledger->program->instructions[facts->instruction_index].source_instruction_index)
            ledger->invalid_owner = true;
        else if (observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION &&
                 ledger->program->instructions[facts->instruction_index].geometry_effect !=
                     USIL_GEOMETRY_EFFECT_NONE) {
            if (ledger->reject_first_effect) return false;
            ledger->effects |= UINT64_C(1) << facts->instruction_index;
            if (facts->lanes || !facts->known || !facts->logical_operation)
                ledger->invalid_owner = true;
        }
    }
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION &&
        facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
        (facts->logical_value_id & (UINT64_C(1) << 63)))
        ledger->logical_input_ids |= UINT64_C(1) << (facts->logical_value_id & 31u);
    return true;
}

static bool named_point_and_persistent_tuple(void) {
    for (unsigned explicit_stream = 0; explicit_stream < 2; ++explicit_stream) {
        Fixture fixture;
        CHECK(fixture_init(&fixture, explicit_stream != 0, true));
        Ledger ledger = {.program = &fixture.program};
        fixture.options.source_quality_observer = observe;
        fixture.options.source_quality_observer_context = &ledger;
        StringBuilder source;
        CHECK(emit(&fixture, &source));
        CHECK(strstr(source.buf, "struct appdata {\n") &&
              strstr(source.buf, "float4 clipPosition : SV_POSITION;") &&
              strstr(source.buf, "float4 color : COLOR;") &&
              strstr(source.buf, "void main(point appdata input[1], inout TriangleStream<v2f> stream)") &&
              strstr(source.buf, "[maxvertexcount(2)]"));
        CHECK(strstr(source.buf, "output.clipPosition_1 = (input[0].clipPosition);") &&
              strstr(source.buf, "output.color_1 = (input[0].color);") &&
              occurrences(source.buf, "output.color_1 =") == 1 &&
              occurrences(source.buf, "stream.Append(output);") == 2 &&
              occurrences(source.buf, "stream.RestartStrip();") == 2);
        CHECK(!strstr(source.buf, " o0") && !strstr(source.buf, " v0") &&
              !strstr(source.buf, "return output"));
        CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              fixture.quality.emission_status == HLSL_EMIT_STATUS_OK &&
              fixture.quality.counts.incomplete_units == 0 &&
              fixture.quality.counts.unknown_provenance == 0 &&
              fixture.quality.counts.residual_total == 0);
        CHECK(!ledger.invalid_owner && ledger.logical_input_ids == 3 &&
              ledger.effects == ((UINT64_C(1) << 2) | (UINT64_C(1) << 3) |
                                 (UINT64_C(1) << 5) | (UINT64_C(1) << 6)));
        CHECK(hlsl_expression_source_map_matches(&fixture.map, &fixture.program, source.buf));
        for (int instruction = 0; instruction < fixture.program.instruction_count; ++instruction) {
            if (!fixture.program.instructions[instruction].geometry_effect) continue;
            HLSLExpressionOrigin *origin = &fixture.map.origins[instruction];
            CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_EFFECT && !origin->destination_lanes &&
                  origin->source_end > origin->source_begin);
        }
        HLSLExpressionSourceMap changed = fixture.map;
        changed.origins[2].kind = HLSL_EXPRESSION_ORIGIN_DEAD;
        CHECK(!hlsl_expression_source_map_matches(&changed, &fixture.program, source.buf));
        changed = fixture.map;
        changed.origins[2].source_instruction_index++;
        CHECK(!hlsl_expression_source_map_matches(&changed, &fixture.program, source.buf));
        changed = fixture.map;
        changed.origins[2].destination_lanes = 1;
        CHECK(!hlsl_expression_source_map_matches(&changed, &fixture.program, source.buf));
        sb_free(&source);
        const char *reserved[] = {"appdata", "input", "stream", "v2f", "output", "color", "clipPosition"};
        fixture.options.reserved_preprocessor_identifiers = reserved;
        fixture.options.reserved_preprocessor_identifier_count = sizeof(reserved) / sizeof(reserved[0]);
        CHECK(emit(&fixture, &source));
        CHECK(strstr(source.buf, "point appdata_1 input_1[1]") &&
              strstr(source.buf, "TriangleStream<v2f_1> stream_1") &&
              strstr(source.buf, "stream_1.Append(output_1);"));
        sb_free(&source);
        fixture_free(&fixture);
    }
    return true;
}

static bool raw_bytes_and_observer_failure(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, false, false));
    StringBuilder original, measured;
    sb_init(&original);
    CHECK(hlsl_emit(&fixture.program, &original, NULL, NULL, NULL));
    HLSLEmitOptions raw = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
    raw.source_quality = &fixture.quality;
    sb_init(&measured);
    CHECK(hlsl_emit_with_options(&fixture.program, &measured, NULL, NULL, NULL, &raw));
    CHECK(original.len == measured.len && memcmp(original.buf, measured.buf, original.len + 1) == 0);
    CHECK(strstr(original.buf, "dxbc_stream.Append(output);") && strstr(original.buf, "o0"));
    sb_free(&original);
    sb_free(&measured);
    Ledger ledger = {.program = &fixture.program, .reject_first_effect = true};
    fixture.options.source_quality_observer = observe;
    fixture.options.source_quality_observer_context = &ledger;
    CHECK(!emit(&fixture, &measured));
    CHECK(measured.failed && !fixture.map.complete && fixture.map.count == 0 &&
          fixture.quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
          fixture.quality.emission_status == HLSL_EMIT_STATUS_ANALYSIS_FAILED);
    sb_free(&measured);
    fixture_free(&fixture);
    return true;
}

static bool unsupported_boundaries(void) {
    for (unsigned mutation = 0; mutation < 19; ++mutation) {
        Fixture fixture;
        CHECK(fixture_init(&fixture, false, true));
        USILProgram *program = &fixture.program;
        switch (mutation) {
        case 0: program->has_parsed_signature_authority = false; break;
        case 1: program->has_stage_contract = false; break;
        case 2: program->geometry.output_tuple_state_persists = false; break;
        case 3: program->geometry.effect_count++; break;
        case 4: program->instructions[1].operands[0].register_index = 0; break;
        case 5: program->instructions[0].operands[0].destination_mask = 0x70; break;
        case 6: program->instructions[0].operands[1].index_values[0] = 1; break;
        case 7: program->instructions[1].operands[1].index_values[1] = 2; break;
        case 8: program->instructions[2].geometry_stream_id = 1; break;
        case 9: program->instructions[2].geometry_effect = USIL_GEOMETRY_EFFECT_RESTART_STRIP; break;
        case 10: program->instructions[2].geometry_stream_explicit = true; break;
        case 11: program->geometry.max_output_vertex_count = 1; break;
        case 12: program->geometry.has_instance_count = true; break;
        case 13: program->outputs[1].mask = 5; break;
        case 14: program->shader_model_major = 5; break;
        case 15: program->instructions[0].precise_mask = 1; break;
        case 16: program->instructions[0].opcode = USIL_OP_IF; break;
        case 17: program->instructions[4].operands[1].type = OPERAND_TYPE_OUTPUT; break;
        case 18: program->instructions[0].operands[1].register_index_dim = 1; break;
        }
        StringBuilder source;
        CHECK(!emit(&fixture, &source));
        CHECK(source.failed && !fixture.map.complete &&
              fixture.quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
              fixture.quality.emission_status != HLSL_EMIT_STATUS_OK);
        sb_free(&source);
        fixture_free(&fixture);
    }
    return true;
}

static bool natural_heterogeneous_field_widths(void) {
    const uint8_t masks[] = {1, 3, 7, 15};
    const char *types[] = {"float", "float2", "float3", "float4"};
    for (unsigned width = 0; width < 4; ++width) {
        Fixture fixture;
        CHECK(fixture_init_primitive(&fixture, true, true, DXBC_INPUT_PRIMITIVE_POINT,
                                     1, 0, masks[width]));
        StringBuilder source;
        CHECK(emit(&fixture, &source));
        char expected[96];
        snprintf(expected, sizeof(expected), "%s color : COLOR;", types[width]);
        CHECK(strstr(source.buf, expected));
        CHECK(strstr(source.buf, "output.color_1 = (input[0].color);") &&
              !strstr(source.buf, "input[0].color."));
        CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              fixture.quality.counts.unknown_provenance == 0 &&
              fixture.quality.counts.residual_total == 0 &&
              fixture.quality.counts.incomplete_units == 0);
        CHECK(hlsl_expression_source_map_matches(&fixture.map,&fixture.program,source.buf));
        sb_free(&source);
        DXBCOperand *color = &fixture.program.instructions[1].operands[1];
        color->swizzle[0] = 3;
        if (width < 3) {
            CHECK(!emit(&fixture, &source)); sb_free(&source);
        }
        fixture_free(&fixture);
    }
    return true;
}

static bool static_primitive_array_boundaries(void) {
    const struct { DXBCInputPrimitive primitive; uint32_t vertices; const char *name; } shapes[] = {
        {DXBC_INPUT_PRIMITIVE_POINT, 1, "point"},
        {DXBC_INPUT_PRIMITIVE_LINE, 2, "line"},
        {DXBC_INPUT_PRIMITIVE_TRIANGLE, 3, "triangle"},
        {DXBC_INPUT_PRIMITIVE_LINE_ADJACENCY, 4, "lineadj"},
        {DXBC_INPUT_PRIMITIVE_TRIANGLE_ADJACENCY, 6, "triangleadj"}
    };
    for (size_t shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); ++shape) {
        for (uint32_t vertex = 0; vertex < shapes[shape].vertices; ++vertex) {
            Fixture fixture;
            CHECK(fixture_init_primitive(&fixture, true, true, shapes[shape].primitive,
                                         shapes[shape].vertices, vertex, 15));
            StringBuilder source;
            if (!emit(&fixture, &source)) {
                fprintf(stderr, "Failed shape%zu vertex%u predicate%d quality-status%d\n", shape, vertex,
                        hlsl_high_level_geometry_interface_supported(&fixture.program, fixture.options.mode),
                        fixture.quality.emission_status);
                sb_free(&source);
                HLSLEmitDiagnostic diag; sb_init(&source);
                hlsl_emit_with_options_diagnostic(&fixture.program,&source,NULL,NULL,NULL,&fixture.options,&diag);
                fprintf(stderr,"Diagnostic status%d phase%d reason%d instruction%d operand%d\n",
                        diag.status,diag.phase,diag.reason,diag.instruction_index,diag.operand_index);
                return false;
            }
            char expected[128];
            snprintf(expected, sizeof(expected), "void main(%s appdata input[%u]", shapes[shape].name,
                     shapes[shape].vertices);
            CHECK(strstr(source.buf, expected));
            snprintf(expected, sizeof(expected), "output.color_1 = (input[%u].color);", vertex);
            CHECK(strstr(source.buf, expected));
            CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                  fixture.quality.counts.unknown_provenance == 0 &&
                  fixture.quality.counts.residual_total == 0 &&
                  fixture.quality.counts.incomplete_units == 0);
            CHECK(hlsl_expression_source_map_matches(&fixture.map, &fixture.program, source.buf));
            HLSLEmitterContext context = {.program=&fixture.program,
                .emit_mode=HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE, .high_level_geometry=true,
                .high_level_interface=true, .preferred_input_struct_name="appdata",
                .preferred_output_struct_name="v2f", .entry_point_name="main"};
            CHECK(hlsl_prepare_high_level_interface(&context));
            ASTOperandProvenance position, color;
            CHECK(hlsl_high_level_input_provenance(&context, &fixture.program.instructions[0].operands[1],
                                                   15, &position));
            CHECK(hlsl_high_level_input_provenance(&context, &fixture.program.instructions[1].operands[1],
                                                   15, &color));
            CHECK(position.logical_value_id == ((UINT64_C(1) << 63) | (uint64_t)vertex << 32u) &&
                  color.logical_value_id == (position.logical_value_id | 1u));
            sb_free(&source);
            DXBCOperand *input = &fixture.program.instructions[0].operands[1];
            const DXBCOperand original = *input;
            input->register_index = (int)shapes[shape].vertices;
            input->index_values[0] = shapes[shape].vertices;
            CHECK(!emit(&fixture, &source)); sb_free(&source); *input = original;
            input->index_values[0] = vertex + 1;
            CHECK(!emit(&fixture, &source)); sb_free(&source); *input = original;
            input->index_representations[0] = 2;
            CHECK(!emit(&fixture, &source)); sb_free(&source); *input = original;
            fixture.program.geometry.input_vertex_count++;
            CHECK(!emit(&fixture, &source)); sb_free(&source);
            fixture_free(&fixture);
        }
    }
    return true;
}

int main(void) {
    const size_t allocations = g_allocations_count;
    const size_t bytes = g_allocated_bytes;
    if (!natural_heterogeneous_field_widths() || !static_primitive_array_boundaries() ||
        !named_point_and_persistent_tuple() || !raw_bytes_and_observer_failure() ||
        !unsupported_boundaries()) return 1;
    if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
        fprintf(stderr, "allocation imbalance\n");
        return 1;
    }
    puts("Natural geometry source unit tests passed");
    return 0;
}
