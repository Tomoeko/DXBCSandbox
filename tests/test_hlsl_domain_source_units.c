// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "test_tessellation_fixture.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/hlsl_stage_coverage_internal.h"
#include "translation/usil_validation.h"

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
} DomainFixture;

static bool domain_fixture_parse(DomainFixture *fixture, uint8_t *bytes, size_t size) {
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

static bool domain_fixture_init_shape(DomainFixture *fixture, unsigned domain, uint32_t points,
                                     uint8_t location_mask) {
    size_t size = 0;
    uint8_t *bytes = test_tessellation_domain_dxbc(domain, points, location_mask, &size);
    return domain_fixture_parse(fixture, bytes, size);
}

static bool domain_fixture_float3(DomainFixture *fixture, const char *semantic, unsigned scenario) {
    size_t size = 0;
    uint8_t *bytes = test_tessellation_domain_float3_dxbc(3, semantic, scenario, &size);
    return domain_fixture_parse(fixture, bytes, size);
}

static bool domain_fixture_init(DomainFixture *fixture, uint32_t points, uint8_t location_mask) {
    return domain_fixture_init_shape(fixture, 2, points, location_mask);
}

static void domain_fixture_dispose(DomainFixture *fixture) {
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

typedef struct {
    const USILProgram *program;
    uint8_t points;
    bool location;
    bool bad_owner;
    bool reject;
} DomainLedger;

static bool observe_domain(void *context,
                            const HLSLSourceQualityObservation *observation) {
    DomainLedger *ledger = context;
    if (ledger->reject) return false;
    if (observation->stage != DXBC_PROGRAM_TYPE_DOMAIN ||
        observation->pass_index != 6 || observation->entry_point_index != 2 ||
        observation->source_unit_id != 0)
        ledger->bad_owner = true;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (facts->instruction_index >= 0 &&
        (facts->instruction_index >= ledger->program->instruction_count ||
         facts->source_instruction_index !=
            ledger->program->instructions[facts->instruction_index].source_instruction_index))
        ledger->bad_owner = true;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION &&
        facts->value_kind == HLSL_SOURCE_VALUE_LOGICAL &&
        (facts->logical_value_id & (UINT64_C(1) << 63))) {
        if (facts->logical_value_id & (UINT64_C(1) << 62))
            ledger->location = true;
        else
            ledger->points |= (uint8_t)(1u << ((facts->logical_value_id >> 32) & 3u));
    }
    return true;
}

static bool natural_domain_source(void) {
    DomainFixture fixture;
    CHECK(domain_fixture_init(&fixture, 3, 7));
    CHECK(hlsl_high_level_domain_interface_supported(&fixture.program,
                                                     HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    DomainLedger ledger = {.program = &fixture.program};
    options.source_quality = &quality;
    options.expression_source_map = &map;
    options.source_quality_pass_index = 6;
    options.source_quality_entry_point_index = 2;
    options.source_quality_observer = observe_domain;
    options.source_quality_observer_context = &ledger;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&fixture.program, &source,
                                 NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "DomainFactors factors"));
    CHECK(strstr(source.buf, "float outer[3] : SV_TessFactor;"));
    CHECK(strstr(source.buf, "float inner : SV_InsideTessFactor;"));
    CHECK(strstr(source.buf, "OutputPatch<appdata, 3> patch"));
    CHECK(strstr(source.buf, "barycentric : SV_DomainLocation"));
    CHECK(strstr(source.buf, "[domain(\"tri\")]"));
    CHECK(strstr(source.buf, "float4 clipPosition : SV_POSITION;"));
    CHECK(strstr(source.buf, "patch[0].clipPosition"));
    CHECK(strstr(source.buf, "patch[1].clipPosition"));
    CHECK(strstr(source.buf, "patch[2].clipPosition"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(!quality.counts.residual_total && !quality.counts.unknown_provenance &&
          !quality.counts.incomplete_units);
    CHECK(!ledger.bad_owner && ledger.points == 7 && ledger.location);
    CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
    sb_free(&source);

    ledger.reject = true;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&fixture.program, &source,
                                  NULL, NULL, NULL, &options));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    sb_free(&source);
    domain_fixture_dispose(&fixture);
    return true;
}

static bool domain_authority_negatives(void) {
    DomainFixture fixture;
    CHECK(domain_fixture_init(&fixture, 3, 7));
    USILProgram *program = &fixture.program;

    program->program_type = DXBC_PROGRAM_TYPE_VERTEX;
    CHECK(!usil_signature_authority_is_valid(program));
    CHECK(source_rejected(program));
    program->program_type = DXBC_PROGRAM_TYPE_DOMAIN;

    program->inputs[0].system_value = 2;
    CHECK(!usil_signature_authority_is_valid(program));
    CHECK(source_rejected(program));
    program->inputs[0].system_value = 1;

    program->has_parsed_signature_authority = false;
    CHECK(source_rejected(program));
    program->has_parsed_signature_authority = true;

    DXBCOperand *point = &program->instructions[0].operands[2];
    point->index_values[0] = 3;
    point->register_index = 3;
    CHECK(source_rejected(program));
    point->index_values[0] = 1;
    point->register_index = 1;

    program->patch_constants[0].system_value = 11;
    CHECK(source_rejected(program));
    program->patch_constants[0].system_value = 13;

    program->instructions[0].precise_mask = 15;
    CHECK(source_rejected(program));
    program->instructions[0].precise_mask = 0;

    /* Valid builtin declaration syntax does not authorize undeclared lanes. */
    program->signature_declarations[0].mask = 1;
    CHECK(usil_signature_authority_is_valid(program));
    CHECK(source_rejected(program));
    program->signature_declarations[0].mask = 7;

    program->tessellation.domain = DXBC_TESSELLATOR_DOMAIN_QUAD;
    CHECK(source_rejected(program));
    program->tessellation.domain = DXBC_TESSELLATOR_DOMAIN_TRIANGLE;

    program->signature_declarations[1].array_element_count = 4;
    CHECK(!usil_signature_authority_is_valid(program));
    CHECK(source_rejected(program));
    program->signature_declarations[1].array_element_count = 3;

    const int instruction_count = program->instruction_count;
    program->instruction_count = HLSL_DOMAIN_SOURCE_INSTRUCTION_LIMIT + 1;
    CHECK(!hlsl_high_level_domain_interface_supported(program,
                                                      HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    program->instruction_count = instruction_count;
    CHECK(hlsl_high_level_domain_interface_supported(program,
                                                     HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    domain_fixture_dispose(&fixture);

    CHECK(domain_fixture_init(&fixture, 4, 7));
    CHECK(hlsl_high_level_domain_interface_supported(&fixture.program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    domain_fixture_dispose(&fixture);
    return true;
}

/* Authored domain-specific ABI cases exercise natural coordinate widths and
 * factor roles independently of interpolation structure and source spelling. */
static bool other_domain_shapes(void) {
    const struct { unsigned domain; uint8_t points, mask; const char *attribute; } cases[] = {
        {3, 4, 3, "quad"}, {1, 2, 1, "isoline"}, {2, 32, 7, "tri"}
    };
    for (unsigned row = 0; row < sizeof(cases) / sizeof(cases[0]); ++row) {
        DomainFixture fixture;
        CHECK(domain_fixture_init_shape(&fixture, cases[row].domain, cases[row].points, cases[row].mask));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        options.source_quality = &quality;
        options.expression_source_map = &map;
        StringBuilder source; sb_init(&source);
        CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.counts.unknown_provenance &&
              !quality.counts.residual_total && !quality.counts.incomplete_units);
        CHECK(hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
        char expected[96];
        snprintf(expected, sizeof(expected), "[domain(\"%s\")]", cases[row].attribute);
        CHECK(strstr(source.buf, expected));
        snprintf(expected, sizeof(expected), "OutputPatch<appdata, %u>", cases[row].points);
        CHECK(strstr(source.buf, expected));
        if (cases[row].domain == 3) {
            CHECK(strstr(source.buf, "float outer[4] : SV_TessFactor;"));
            CHECK(strstr(source.buf, "float inner[2] : SV_InsideTessFactor;"));
            CHECK(strstr(source.buf, "float2 coordinates : SV_DomainLocation"));
        } else if (cases[row].domain == 1) {
            CHECK(strstr(source.buf, "float outer[2] : SV_TessFactor;"));
            CHECK(!strstr(source.buf, "SV_InsideTessFactor"));
            CHECK(strstr(source.buf, "float2 coordinates : SV_DomainLocation"));
        }
        sb_free(&source);
        /* A valid domain value cannot repair the wrong factor role, extent,
         * or a use of an undeclared coordinate component. */
        const uint32_t saved = fixture.program.patch_constants[0].system_value;
        fixture.program.patch_constants[0].system_value = 999;
        CHECK(source_rejected(&fixture.program));
        fixture.program.patch_constants[0].system_value = saved;
        fixture.program.signature_declarations[1].array_element_count = (uint8_t)(cases[row].points + 1u);
        CHECK(source_rejected(&fixture.program));
        fixture.program.signature_declarations[1].array_element_count = cases[row].points;
        if (cases[row].domain != 2) {
            fixture.program.instructions[0].operands[1].swizzle[0] = 2;
            CHECK(source_rejected(&fixture.program));
        }
        domain_fixture_dispose(&fixture);
    }
    return true;
}

/* The factors' retained ABI order comes from registers and semantic indices,
 * including when the decoded record list itself uses another order. */
static bool factor_group_order(void) {
    for (unsigned domain = 2; domain <= 3; ++domain) {
        DomainFixture fixture;
        CHECK(domain_fixture_init_shape(&fixture, domain, domain == 2 ? 3 : 4, domain == 2 ? 7 : 3));
        const unsigned outer = domain == 2 ? 3 : 4, inner = domain == 2 ? 1 : 2;
        for (unsigned row = 0; row < outer + inner; ++row) {
            DXBCSignatureElement *field = &fixture.program.patch_constants[row];
            field->register_id = row < outer ? row + inner : row - outer;
        }
        bool inner_first;
        CHECK(usil_signature_authority_is_valid(&fixture.program));
        CHECK(hlsl_domain_factor_order(&fixture.program, &inner_first) && inner_first);
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality; HLSLExpressionSourceMap map;
        options.source_quality = &quality; options.expression_source_map = &map;
        StringBuilder source; sb_init(&source);
        CHECK(hlsl_emit_with_options(&fixture.program, &source, NULL, NULL, NULL, &options));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              hlsl_expression_source_map_matches(&map, &fixture.program, source.buf));
        const char *outer_field = strstr(source.buf, "float outer["), *inner_field = strstr(source.buf, "float inner");
        CHECK(inner_field && outer_field && inner_field < outer_field);
        sb_free(&source);
        /* Duplicate and split groups must fail rather than get sorted into an
         * apparently valid source ABI. */
        fixture.program.patch_constants[0].register_id = fixture.program.patch_constants[1].register_id;
        CHECK(source_rejected(&fixture.program));
        fixture.program.patch_constants[0].register_id = inner;
        fixture.program.patch_constants[0].semantic_index = 1;
        CHECK(source_rejected(&fixture.program));
        domain_fixture_dispose(&fixture);
    }
    return true;
}

/* Point4 must not alias the domain coordinate logical value. */
static bool separated_point_and_location_identity(void) {
    DomainFixture fixture;
    CHECK(domain_fixture_init(&fixture, 5, 7));
    DXBCOperand *point = &fixture.program.instructions[0].operands[2];
    point->register_index = 4; point->index_values[0] = 4;
    HLSLEmitterContext context = {.program = &fixture.program, .high_level_interface = true,
                                 .high_level_domain = true};
    strcpy(context.high_level_input_names[0], "clipPosition");
    ASTOperandProvenance point_origin, location_origin;
    CHECK(hlsl_high_level_input_provenance(&context, point, 15, &point_origin));
    CHECK(hlsl_high_level_input_provenance(&context, &fixture.program.instructions[0].operands[1],
                                          15, &location_origin));
    CHECK(point_origin.logical_value_id != location_origin.logical_value_id);
    CHECK(point_origin.logical_value_id == ((UINT64_C(1) << 63) | (UINT64_C(4) << 32)));
    CHECK(location_origin.logical_value_id == ((UINT64_C(1) << 63) | (UINT64_C(1) << 62)));
    domain_fixture_dispose(&fixture);
    return true;
}

static bool logical_coordinate_source(DomainFixture *fixture, const char *spelling) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality; HLSLExpressionSourceMap map;
    options.source_quality = &quality; options.expression_source_map = &map;
    DomainLedger ledger = {.program = &fixture->program};
    options.source_quality_pass_index = 6; options.source_quality_entry_point_index = 2;
    options.source_quality_observer = observe_domain;
    options.source_quality_observer_context = &ledger;
    StringBuilder source; sb_init(&source);
    HLSLEmitDiagnostic diagnostic;
    if (!hlsl_emit_with_options_diagnostic(&fixture->program, &source, NULL, NULL, NULL, &options, &diagnostic)) {
        fprintf(stderr, "Composition diagnostic: status=%s phase=%s reason=%s instruction=%d domain=%u\n", hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase), hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index, fixture->program.tessellation.domain);
        sb_free(&source);
        return false;
    }
    CHECK(strstr(source.buf, spelling));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(!quality.counts.unknown_provenance && !quality.counts.residual_total &&
          !quality.counts.incomplete_units && !ledger.bad_owner && ledger.location);
    CHECK(hlsl_expression_source_map_matches(&map, &fixture->program, source.buf));
    sb_free(&source);
    return true;
}

static bool logical_coordinate_composition(void) {
    for (unsigned domain = 1; domain <= 3; ++domain) {
        DomainFixture fixture;
        CHECK(domain_fixture_init_shape(&fixture, domain, domain == 1 ? 2u : domain == 2 ? 3u : 4u,
                                       (uint8_t)(domain == 2 ? 7u : 3u)));
        USILInstruction *producer = &fixture.program.instructions[0];
        DXBCOperand *location = &producer->operands[1];
        producer->opcode = USIL_OP_MUL;
        producer->operands[0].destination_mask = 0x70;
        const DXBCOperand saved_factor = producer->operands[2];
        producer->operands[2] = (DXBCOperand){.type = OPERAND_TYPE_IMMEDIATE32,
            .swizzle_mode = 1, .swizzle = {0,1,2,3}, .imm_values = {0x3f800000, 0x40000000, 0x40400000, 0x40800000}, .imm_value_count = 4};
        fixture.program.instructions[1].operands[3].swizzle_mode = 1;
        memset(fixture.program.instructions[1].operands[3].swizzle, 0,
               sizeof(fixture.program.instructions[1].operands[3].swizzle));
        location->swizzle_mode = 1;
        location->swizzle[0] = location->swizzle[1] = 0;
        location->swizzle[2] = location->swizzle[3] = 1;
        CHECK(logical_coordinate_source(&fixture, domain == 2
            ? "float3((barycentric.x), (barycentric.x), (barycentric.y))"
            : "float3((coordinates.x), (coordinates.x), (coordinates.y))"));
        producer->operands[0].destination_mask = 0xf0;
        CHECK(logical_coordinate_source(&fixture, "float4("));
        producer->operands[0].destination_mask = 0x30;
        location->swizzle[0] = 1; location->swizzle[1] = 0;
        CHECK(logical_coordinate_source(&fixture, "float2("));
        producer->operands[0].destination_mask = 0x70;
        location->swizzle[0] = location->swizzle[1] = 0;
        location->swizzle[2] = 1;
        location->has_abs = location->has_neg = true;
        uint32_t modifier = 0xc1;
        location->extended_tokens = &modifier; location->extended_token_count = 1;
        CHECK(logical_coordinate_source(&fixture, "abs(float3("));
        modifier = 0x800000c1;
        CHECK(source_rejected(&fixture.program));
        location->extended_tokens = NULL; location->extended_token_count = 0;
        location->has_abs = location->has_neg = false;
        location->min_precision = 1;
        CHECK(source_rejected(&fixture.program));
        location->min_precision = 0;
        producer->precise_mask = 15;
        CHECK(source_rejected(&fixture.program));
        producer->precise_mask = 0;
        producer->saturate = true;
        CHECK(source_rejected(&fixture.program));
        producer->saturate = false;
        fixture.program.signature_declarations[0].mask = 1;
        CHECK(source_rejected(&fixture.program));
        fixture.program.signature_declarations[0].mask = (uint8_t)(domain == 2 ? 7u : 3u);
        if (domain != 2) {
            location->swizzle[2] = 2;
            CHECK(source_rejected(&fixture.program));
        }
        producer->operands[2] = saved_factor;
        domain_fixture_dispose(&fixture);
    }
    return true;
}

typedef struct {
    DomainLedger owners;
    size_t events, mul_event, xyz_event, w_event, constructor_event;
    size_t reject_event, mutate_event;
    DomainFixture *fixture;
    unsigned mutation;
    bool rejected, mutated, valid_mutation;
} Float3Ledger;

static bool observe_float3(void *context, const HLSLSourceQualityObservation *observation) {
    Float3Ledger *ledger = context;
    if (!observe_domain(&ledger->owners, observation)) return false;
    const size_t event = ++ledger->events;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION) {
        if (observation->facts.instruction_index == 0 && !ledger->mul_event) ledger->mul_event = event;
        if (observation->facts.instruction_index == 2 && !ledger->xyz_event) ledger->xyz_event = event;
        if (observation->facts.instruction_index == 3 && !ledger->w_event) ledger->w_event = event;
        if (observation->facts.logical_value_id == HLSL_DOMAIN_OUTPUT_LOGICAL_ID) {
            if (observation->facts.instruction_index != -1 ||
                observation->facts.source_instruction_index != UINT32_MAX ||
                observation->facts.lanes || observation->facts.components != 4)
                ledger->owners.bad_owner = true;
            if (!ledger->constructor_event) ledger->constructor_event = event;
        }
    }
    if (ledger->mutate_event == event) {
        USILProgram *program = &ledger->fixture->program;
        if (ledger->mutation == 0) program->instructions[3].operands[1].imm_values[0] = UINT32_C(0x40000000);
        if (ledger->mutation == 1) ++program->instructions[3].source_instruction_index;
        if (ledger->mutation == 2) program->outputs[0].rw_mask = 1;
        if (ledger->mutation == 3) {
            program->instructions[0].operands[2].register_index = 2;
            program->instructions[0].operands[2].index_values[0] = 2;
        }
        if (ledger->mutation == 4) {
            memset(program->instructions[0].operands[1].swizzle, 0,
                   sizeof(program->instructions[0].operands[1].swizzle));
            program->instructions[0].operands[1].raw_token = 0x0001c006;
        }
        if (ledger->mutation == 5) {
            memset(program->inputs[0].semantic_name, 0, sizeof(program->inputs[0].semantic_name));
            memcpy(program->inputs[0].semantic_name, "OBJECTCOORD", sizeof("OBJECTCOORD"));
            program->inputs[0].semantic_name_length = strlen("OBJECTCOORD");
        }
        if (ledger->mutation == 0 || ledger->mutation >= 3)
            ledger->valid_mutation = hlsl_high_level_domain_interface_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE);
        ledger->mutated = true;
    }
    if (ledger->reject_event == event) { ledger->rejected = true; return false; }
    return true;
}

static bool float3_capture_matches(const HLSLStageCoverage *coverage,
                                  const HLSLStageCoverage *independent,
                                  const StringBuilder *source, const StringBuilder *other_source) {
    CHECK(hlsl_stage_coverage_validate(coverage, source) &&
          hlsl_stage_coverage_validate(independent, other_source) &&
          hlsl_stage_coverage_equal(coverage, independent));
    return true;
}

static bool float3_owned_mutations(HLSLStageCoverage *coverage,
                                  const HLSLStageCoverage *independent,
                                  StringBuilder *source, const StringBuilder *other_source) {
    HLSLStageDomainOutput *output = &coverage->domain_output;
    HLSLStageOwnedRoot *constructor = &coverage->roots[output->root_index];
#define REJECT_RESTORE(change, restore) do { \
    change; \
    CHECK(!hlsl_stage_coverage_validate(coverage, source) && \
          !hlsl_stage_coverage_equal(coverage, independent)); \
    restore; \
    CHECK(float3_capture_matches(coverage, independent, source, other_source)); \
} while (0)
    REJECT_RESTORE(coverage->domain_owner_digest[0] ^= 1, coverage->domain_owner_digest[0] ^= 1);
    REJECT_RESTORE(coverage->recorded_domain_owner_digest[0] ^= 1,
                   coverage->recorded_domain_owner_digest[0] ^= 1);
    REJECT_RESTORE(output->plan.present = false, output->plan.present = true);
    REJECT_RESTORE(++output->plan.pieces[0].source_instruction_index,
                   --output->plan.pieces[0].source_instruction_index);
    REJECT_RESTORE(output->recorded_plan.pieces[1].mask ^= 1,
                   output->recorded_plan.pieces[1].mask ^= 1);
    REJECT_RESTORE(output->plan.pieces[1].immediate_bits ^= 1,
                   output->plan.pieces[1].immediate_bits ^= 1);
    REJECT_RESTORE(output->plan.pieces[1].immediate_bits ^= 1; output->recorded_plan.pieces[1].immediate_bits ^= 1,
                   output->plan.pieces[1].immediate_bits ^= 1; output->recorded_plan.pieces[1].immediate_bits ^= 1);
    REJECT_RESTORE(output->plan.output.mask ^= 1; output->recorded_plan.output.mask ^= 1,
                   output->plan.output.mask ^= 1; output->recorded_plan.output.mask ^= 1);
    REJECT_RESTORE(++output->root_index, --output->root_index);
    REJECT_RESTORE(output->recorded_root_captured = false, output->recorded_root_captured = true);
    REJECT_RESTORE(++output->return_begin, --output->return_begin);
    REJECT_RESTORE(--output->recorded_return_end, ++output->recorded_return_end);
    REJECT_RESTORE(output->return_emitted = false, output->return_emitted = true);
    const size_t terminator = output->return_end - strlen(";\n}\n");
    REJECT_RESTORE(source->buf[terminator] = ':'; coverage->source[terminator] = ':',
                   source->buf[terminator] = ';'; coverage->source[terminator] = ';');
    REJECT_RESTORE(constructor->tree->logical_origin.instruction_index = 3,
                   constructor->tree->logical_origin.instruction_index = -1);
    REJECT_RESTORE(constructor->tree->logical_origin.destination_lanes = 15,
                   constructor->tree->logical_origin.destination_lanes = 0);
    REJECT_RESTORE(constructor->tree->u.call.name[5] = '3', constructor->tree->u.call.name[5] = '4');
    REJECT_RESTORE(++constructor->begin, --constructor->begin);
    REJECT_RESTORE(--constructor->end, ++constructor->end);
    source->buf[constructor->begin] ^= 1;
    CHECK(!hlsl_stage_coverage_validate(coverage, source) &&
          hlsl_stage_coverage_equal(coverage, independent));
    source->buf[constructor->begin] ^= 1;
    CHECK(float3_capture_matches(coverage, independent, source, other_source));
#undef REJECT_RESTORE
    return true;
}

static bool float3_domain_source(const char *semantic, bool colliding_name) {
    DomainFixture fixture;
    CHECK(domain_fixture_float3(&fixture, semantic, TEST_DOMAIN_FLOAT3_VALID));
    const USILProgram *program = &fixture.program;
    CHECK(usil_signature_authority_is_valid(program) && program->instruction_count == 5 &&
          program->inputs[0].mask == 7 && program->inputs[0].rw_mask == 7 && !program->inputs[0].system_value &&
          program->outputs[0].mask == 15 && !program->outputs[0].rw_mask && program->outputs[0].system_value == 1);
    CHECK(!hlsl_high_level_patch_domain_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE));
    CHECK(program->instructions[0].opcode == USIL_OP_MUL &&
          program->instructions[1].opcode == USIL_OP_MAD && program->instructions[2].opcode == USIL_OP_MAD &&
          program->instructions[3].opcode == USIL_OP_MOV && program->instructions[4].opcode == USIL_OP_RET);
    for (int index = 0; index < program->instruction_count; ++index)
        CHECK(program->instructions[index].source_instruction_index == (uint32_t)(7 + index));
    CHECK(usil_operand_destination_lane_mask(&program->instructions[2].operands[0]) == 7 &&
          usil_operand_destination_lane_mask(&program->instructions[3].operands[0]) == 8);
    HLSLEmitNames names = {.entry_point = colliding_name ? "attribute0" : "domain", .input_struct = "DomainPoint"};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult normal_quality, captured_quality, independent_quality;
    HLSLExpressionSourceMap normal_map = {0}, captured_map = {0};
    HLSLEmitDiagnostic diagnostic;
    options.source_quality_pass_index = 6; options.source_quality_entry_point_index = 2;
    options.source_quality = &normal_quality; options.expression_source_map = &normal_map;
    Float3Ledger ledger = {.owners = {.program = program}};
    options.source_quality_observer = observe_float3; options.source_quality_observer_context = &ledger;
    StringBuilder normal, source, other_source;
    sb_init(&normal); sb_init(&source); sb_init(&other_source);
    CHECK(hlsl_emit_with_options_diagnostic(program, &normal, NULL, NULL, &names, &options, &diagnostic));
    CHECK(normal_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !normal_quality.counts.residual_total &&
          !normal_quality.counts.unknown_provenance && !normal_quality.counts.incomplete_units &&
          normal_quality.counts.inspected_units == 1);
    CHECK(!ledger.owners.bad_owner && ledger.owners.points == 7 && ledger.owners.location &&
          ledger.xyz_event && ledger.w_event && ledger.constructor_event);
    char input_semantic[192];
    const int length = snprintf(input_semantic, sizeof(input_semantic), " : %s;", semantic);
    CHECK(length > 0 && (size_t)length < sizeof(input_semantic) && strstr(normal.buf, input_semantic));
    CHECK(strstr(normal.buf, "float3 ") && strstr(normal.buf, "OutputPatch<DomainPoint, 3>") &&
          strstr(normal.buf, "return float4(") && !strstr(normal.buf, "output.") &&
          !strstr(normal.buf, ".xyzx") && !strstr(normal.buf, ".yyyy") &&
          !strstr(normal.buf, ".zzzz") && !strstr(normal.buf, "asfloat("));
    CHECK(hlsl_expression_source_map_matches(&normal_map, program, normal.buf));
    HLSLStageCoverage coverage = {0}, independent = {0};
    options.source_quality = &captured_quality; options.expression_source_map = &captured_map;
    ledger = (Float3Ledger){.owners = {.program = program}};
    const bool captured = hlsl_emit_with_stage_coverage(program, &source, NULL, NULL,
        &names, &options, &coverage, &diagnostic);
    if (!captured) fprintf(stderr,
        "FLOAT3 DOMAIN capture failed: status=%s phase=%s reason=%s instruction=%d raw=%u "
        "bytes=%zu plan=%d roots=%zu began=%d finished=%d quality=%s\n",
        hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase),
        hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index,
        diagnostic.source_instruction_index, source.len, coverage.domain_output.plan.present,
        coverage.root_count, coverage.began, coverage.finished,
        hlsl_source_quality_class_name(captured_quality.classification));
    CHECK(captured);
    CHECK(normal.len == source.len && !memcmp(normal.buf, source.buf, source.len + 1) &&
          hlsl_source_quality_results_equal(&normal_quality, &captured_quality) &&
          normal_map.complete && captured_map.complete && normal_map.count == captured_map.count);
    for (size_t index = 0; index < normal_map.count; ++index)
        CHECK(hlsl_expression_origins_equal(&normal_map.origins[index], &captured_map.origins[index]));
    CHECK(coverage.domain_output.plan.present && coverage.domain_output.plan_captured &&
          coverage.domain_output.recorded_plan_captured && coverage.domain_output.root_captured &&
          coverage.domain_output.recorded_root_captured && coverage.domain_output.root_index < coverage.root_count);
    const HLSLDomainOutputPlan *plan = &coverage.domain_output.plan;
    CHECK(plan->output.mask == 15 && !plan->output.rw_mask &&
          plan->pieces[0].instruction_index == 2 && plan->pieces[0].source_instruction_index == 9 &&
          plan->pieces[0].mask == 7 && plan->pieces[0].width == 3 && !plan->pieces[0].scalar_immediate &&
          plan->pieces[1].instruction_index == 3 && plan->pieces[1].source_instruction_index == 10 &&
          plan->pieces[1].mask == 8 && plan->pieces[1].width == 1 && plan->pieces[1].scalar_immediate &&
          plan->pieces[1].immediate_bits == UINT32_C(0x3f800000) &&
          plan->return_instruction_index == 4 && plan->return_source_instruction_index == 11);
    const HLSLStageOwnedRoot *constructor = &coverage.roots[coverage.domain_output.root_index];
    CHECK(coverage.domain_output.return_emitted && coverage.domain_output.recorded_return_emitted &&
          constructor->begin == coverage.domain_output.return_begin + strlen("    return ") &&
          constructor->end == coverage.domain_output.return_end - strlen(";\n}\n"));
    CHECK(constructor->owner.kind == HLSL_STAGE_ROOT_DOMAIN_OUTPUT_CONSTRUCTION && constructor->instruction == -1 &&
          constructor->tree->kind == AST_EXPR_CALL && !strcmp(constructor->tree->u.call.name, "float4") &&
          constructor->tree->u.call.arg_count == 2 && constructor->tree->logical_origin.complete &&
          constructor->tree->logical_origin.logical_value_id == HLSL_DOMAIN_OUTPUT_LOGICAL_ID &&
          constructor->tree->logical_origin.instruction_index == -1 &&
          constructor->tree->logical_origin.source_instruction_index == UINT32_MAX &&
          !constructor->tree->logical_origin.destination_lanes && constructor->tree->logical_origin.components == 4);
    unsigned actual_pieces = 0;
    for (size_t root = 0; root < coverage.root_count; ++root) {
        const HLSLStageOwnedRoot *child = &coverage.roots[root];
        if (child->owner.kind != HLSL_STAGE_ROOT_INSTRUCTION || child->instruction < 2 || child->instruction > 3) continue;
        const unsigned piece = (unsigned)(child->instruction - 2);
        CHECK(!(actual_pieces & (1u << piece)) && child->tree->logical_origin.complete &&
              child->tree->logical_origin.instruction_index == child->instruction &&
              child->tree->logical_origin.source_instruction_index == (uint32_t)(9 + piece) &&
              child->tree->logical_origin.destination_lanes == (piece ? 8 : 7) &&
              child->tree->logical_origin.components == (piece ? 1 : 3) &&
              constructor->begin < child->begin && child->end < constructor->end);
        if (piece) CHECK(child->tree->kind == AST_EXPR_LITERAL && child->tree->u.literal.components == 1 &&
                         child->tree->u.literal.val[0] == UINT32_C(0x3f800000));
        actual_pieces |= 1u << piece;
    }
    CHECK(actual_pieces == 3 && captured_map.origins[2].destination_lanes == 7 &&
          captured_map.origins[3].destination_lanes == 8 && captured_map.origins[4].kind == HLSL_EXPRESSION_ORIGIN_RETURN &&
          captured_map.origins[4].source_begin < constructor->begin &&
          constructor->end < captured_map.origins[4].source_end &&
          !memcmp(source.buf + captured_map.origins[4].source_begin, "return ", strlen("return ")));
    options.source_quality = &independent_quality; options.expression_source_map = NULL;
    CHECK(hlsl_emit_with_stage_coverage(program, &other_source, NULL, NULL, &names, &options, &independent, &diagnostic));
    CHECK(source.len == other_source.len && !memcmp(source.buf, other_source.buf, source.len + 1) &&
          hlsl_source_quality_results_equal(&captured_quality, &independent_quality));
    CHECK(float3_capture_matches(&coverage, &independent, &source, &other_source));
    domain_fixture_dispose(&fixture);
    CHECK(float3_capture_matches(&coverage, &independent, &source, &other_source));
    CHECK(float3_owned_mutations(&coverage, &independent, &source, &other_source));
    hlsl_stage_coverage_dispose(&independent); hlsl_stage_coverage_dispose(&coverage);
    sb_free(&other_source); sb_free(&source); sb_free(&normal);
    return true;
}

static bool float3_rejected(const USILProgram *program) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map = {0};
    HLSLEmitDiagnostic diagnostic;
    options.source_quality = &quality; options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options_diagnostic(program, &source, NULL, NULL, NULL, &options, &diagnostic));
    CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK && quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
          !map.complete);
    sb_free(&source); sb_init(&source);
    HLSLStageCoverage coverage = {0};
    memset(&map, 0, sizeof(map));
    memset(&quality, 0, sizeof(quality)); quality.classification = HLSL_SOURCE_QUALITY_FAILED;
    CHECK(!hlsl_emit_with_stage_coverage(program, &source, NULL, NULL, NULL, &options, &coverage, &diagnostic));
    CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK && !map.complete && !map.count &&
          quality.classification != HLSL_SOURCE_QUALITY_CLEAN && !coverage.began && !coverage.finished &&
          !coverage.source && !coverage.roots && !coverage.syntax && !coverage.recorded_syntax &&
          !coverage.root_count && !coverage.syntax_count && hlsl_stage_coverage_domain_output_empty(&coverage.domain_output));
    hlsl_stage_coverage_dispose(&coverage); sb_free(&source);
    return true;
}

static bool float3_program_rejections(void) {
    for (unsigned scenario = TEST_DOMAIN_FLOAT3_MISSING_W; scenario <= TEST_DOMAIN_FLOAT3_REORDERED_WRITES; ++scenario) {
        DomainFixture fixture;
        CHECK(domain_fixture_float3(&fixture, "POINTVALUE", scenario));
        CHECK(float3_rejected(&fixture.program));
        domain_fixture_dispose(&fixture);
    }
    for (unsigned mutation = 0; mutation < 22; ++mutation) {
        DomainFixture fixture;
        CHECK(domain_fixture_float3(&fixture, "POINTVALUE", TEST_DOMAIN_FLOAT3_VALID));
        USILProgram *program = &fixture.program;
        const USILProgram saved_program = *program;
        const DXBCSignatureElement input = program->inputs[0], output = program->outputs[0], patch = program->patch_constants[0];
        USILInstruction instructions[5];
        USILSignatureDeclaration declarations[3];
        CHECK(program->instruction_count == 5 && program->signature_declaration_count == 3);
        memcpy(instructions, program->instructions, sizeof(instructions));
        memcpy(declarations, program->signature_declarations, sizeof(declarations));
        DXBCOperand relative = {.type = OPERAND_TYPE_TEMP, .register_index_dim = 1,
            .index_has_immediate = {true}, .swizzle_mode = 2};
        switch (mutation) {
        case 0: program->has_parsed_signature_authority = false; break;
        case 1: program->inputs[0].mask = 15; break;
        case 2: program->inputs[0].rw_mask = 3; break;
        case 3: program->inputs[0].system_value = 1; break;
        case 4: program->outputs[0].mask = 7; break;
        case 5: program->outputs[0].rw_mask = 1; break;
        case 6: program->signature_declarations[1].mask = 3; break;
        case 7: program->signature_declarations[1].array_element_count = 4; break;
        case 8: program->instructions[0].operands[2].register_index = 3;
                program->instructions[0].operands[2].index_values[0] = 3; break;
        case 9: program->instructions[0].operands[2].rel_op0 = &relative;
                program->instructions[0].operands[2].index_representations[0] = 2;
                program->instructions[0].operands[2].index_has_immediate[0] = false; break;
        case 10: program->instructions[0].operands[2].swizzle[2] = 3; break;
        case 11: program->instructions[2].operands[0].destination_mask = 0x30; break;
        case 12: program->instructions[3].operands[0].destination_mask = 0x10; break;
        case 13: program->instructions[3].operands[0].register_index = 1;
                 program->instructions[3].operands[0].index_values[0] = 1; break;
        case 14: program->instructions[0].opcode = USIL_OP_DP3; break;
        case 15: program->instructions[1].precise_mask = 7; break;
        case 16: program->instructions[2].saturate = true; break;
        case 17: program->instructions[3].operands[1].has_neg = true; break;
        case 18: program->signature_declarations[0].mask = 3; break;
        case 19: program->patch_constants[0].system_value = 0;
                 memcpy(program->patch_constants[0].semantic_name, "PATCHVALUE", sizeof("PATCHVALUE")); break;
        case 20: program->inputs[0].semantic_index = 1; break;
        case 21: program->signature_declarations[2].mask = 7; break;
        }
        CHECK(float3_rejected(program));
        *program = saved_program;
        program->inputs[0] = input; program->outputs[0] = output; program->patch_constants[0] = patch;
        memcpy(program->instructions, instructions, sizeof(instructions));
        memcpy(program->signature_declarations, declarations, sizeof(declarations));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        options.source_quality = &quality;
        HLSLStageCoverage restored = {0};
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_stage_coverage(program, &source, NULL, NULL, NULL, &options, &restored, NULL) &&
              quality.classification == HLSL_SOURCE_QUALITY_CLEAN && hlsl_stage_coverage_validate(&restored, &source));
        hlsl_stage_coverage_dispose(&restored); sb_free(&source); domain_fixture_dispose(&fixture);
    }
    static const char *const rejected_semantics[] = {"point", "POINTVALUE1"};
    for (size_t index = 0; index < sizeof(rejected_semantics) / sizeof(rejected_semantics[0]); ++index) {
        DomainFixture fixture;
        CHECK(domain_fixture_float3(&fixture, rejected_semantics[index], TEST_DOMAIN_FLOAT3_VALID));
        CHECK(float3_rejected(&fixture.program)); domain_fixture_dispose(&fixture);
    }
    return true;
}

static bool float3_observer_rollback(void) {
    DomainFixture fixture;
    CHECK(domain_fixture_float3(&fixture, "POINTVALUE", TEST_DOMAIN_FLOAT3_VALID));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map = {0};
    options.source_quality = &quality; options.expression_source_map = &map;
    options.source_quality_pass_index = 6; options.source_quality_entry_point_index = 2;
    Float3Ledger baseline = {.owners = {.program = &fixture.program}};
    options.source_quality_observer = observe_float3; options.source_quality_observer_context = &baseline;
    HLSLStageCoverage retained = {0};
    StringBuilder reference;
    sb_init(&reference);
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &reference, NULL, NULL, NULL, &options, &retained, NULL));
    CHECK(baseline.mul_event && baseline.xyz_event && baseline.w_event && baseline.constructor_event && baseline.events > baseline.w_event);
    const size_t rejected_events[] = {1, baseline.xyz_event, baseline.w_event, baseline.constructor_event, baseline.events};
    for (size_t index = 0; index < sizeof(rejected_events) / sizeof(rejected_events[0]) + 6; ++index) {
      for (unsigned captured = 0; captured < 2; ++captured) {
        Float3Ledger ledger = {.owners = {.program = &fixture.program}, .fixture = &fixture};
        if (index < sizeof(rejected_events) / sizeof(rejected_events[0])) ledger.reject_event = rejected_events[index];
        else {
            ledger.mutation = (unsigned)(index - sizeof(rejected_events) / sizeof(rejected_events[0]));
            ledger.mutate_event = ledger.mutation == 0 ? baseline.w_event : ledger.mutation >= 3 ? baseline.mul_event : 1;
        }
        const DXBCOperand saved_location = fixture.program.instructions[0].operands[1];
        const DXBCSignatureElement saved_input = fixture.program.inputs[0];
        options.source_quality_observer_context = &ledger;
        HLSLStageCoverage rejected = {0};
        HLSLEmitDiagnostic diagnostic;
        StringBuilder source;
        sb_init(&source);
        const bool emitted = captured
            ? hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &rejected, &diagnostic)
            : hlsl_emit_with_options_diagnostic(&fixture.program, &source, NULL, NULL, NULL, &options, &diagnostic);
        CHECK(!emitted);
        CHECK((ledger.rejected || ledger.mutated) && diagnostic.status != HLSL_EMIT_STATUS_OK &&
              quality.classification != HLSL_SOURCE_QUALITY_CLEAN && !map.complete && (!captured || !map.count) &&
              !rejected.began && !rejected.finished && !rejected.source && !rejected.roots && !rejected.syntax &&
              hlsl_stage_coverage_domain_output_empty(&rejected.domain_output));
        if (ledger.mutated) {
            if (ledger.mutation == 0 || ledger.mutation >= 3) CHECK(ledger.valid_mutation);
            if (ledger.mutation == 0) fixture.program.instructions[3].operands[1].imm_values[0] = UINT32_C(0x3f800000);
            if (ledger.mutation == 1) --fixture.program.instructions[3].source_instruction_index;
            if (ledger.mutation == 2) fixture.program.outputs[0].rw_mask = 0;
            if (ledger.mutation == 3) {
                fixture.program.instructions[0].operands[2].register_index = 1;
                fixture.program.instructions[0].operands[2].index_values[0] = 1;
            }
            if (ledger.mutation == 4) fixture.program.instructions[0].operands[1] = saved_location;
            if (ledger.mutation == 5) fixture.program.inputs[0] = saved_input;
        }
        sb_free(&source); sb_init(&source);
        options.source_quality_observer = NULL; options.source_quality_observer_context = NULL;
        CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &rejected, &diagnostic));
        CHECK(source.len == reference.len && !memcmp(source.buf, reference.buf, source.len + 1) &&
              quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              float3_capture_matches(&rejected, &retained, &source, &reference));
        hlsl_stage_coverage_dispose(&rejected); sb_free(&source);
        options.source_quality_observer = observe_float3;
      }
    }
    hlsl_stage_coverage_dispose(&retained); sb_free(&reference); domain_fixture_dispose(&fixture);
    return true;
}

static bool unchanged_float4_capture(void) {
    DomainFixture fixture;
    CHECK(domain_fixture_init(&fixture, 3, 7));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult normal_quality, captured_quality;
    HLSLExpressionSourceMap normal_map = {0}, captured_map = {0};
    StringBuilder normal, captured;
    sb_init(&normal); sb_init(&captured);
    options.source_quality = &normal_quality; options.expression_source_map = &normal_map;
    CHECK(hlsl_emit_with_options(&fixture.program, &normal, NULL, NULL, NULL, &options));
    options.source_quality = &captured_quality; options.expression_source_map = &captured_map;
    HLSLStageCoverage coverage = {0};
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &captured, NULL, NULL, NULL, &options, &coverage, NULL));
    CHECK(normal.len == captured.len && !memcmp(normal.buf, captured.buf, normal.len + 1) &&
          normal_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          hlsl_source_quality_results_equal(&normal_quality, &captured_quality) &&
          normal_map.complete && captured_map.complete && normal_map.count == captured_map.count &&
          hlsl_stage_coverage_domain_output_empty(&coverage.domain_output));
    for (size_t index = 0; index < normal_map.count; ++index)
        CHECK(hlsl_expression_origins_equal(&normal_map.origins[index], &captured_map.origins[index]));
    domain_fixture_dispose(&fixture);
    CHECK(hlsl_stage_coverage_validate(&coverage, &captured));
    hlsl_stage_coverage_dispose(&coverage); sb_free(&captured); sb_free(&normal);
    return true;
}

static bool domain_capture_argument_preservation(void) {
    DomainFixture fixture;
    CHECK(domain_fixture_float3(&fixture, "POINTVALUE", TEST_DOMAIN_FLOAT3_VALID));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map = {0};
    options.source_quality = &quality; options.expression_source_map = &map;
    StringBuilder reference;
    sb_init(&reference);
    CHECK(hlsl_emit_with_options(&fixture.program, &reference, NULL, NULL, NULL, &options));
    const HLSLSourceQualityResult previous_quality = quality;
    const HLSLExpressionSourceMap previous_map = map;
    for (unsigned sentinel = 0; sentinel < 3; ++sentinel) {
        HLSLStageCoverage coverage = {0};
        if (sentinel == 0) coverage.domain_owner_digest[0] = 1;
        if (sentinel == 1) coverage.recorded_domain_owner_digest[0] = 1;
        if (sentinel == 2) coverage.domain_output.return_begin = 1;
        HLSLEmitDiagnostic diagnostic;
        StringBuilder source;
        sb_init(&source);
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT && !source.len &&
              !coverage.began && !coverage.finished && !coverage.source && !coverage.roots &&
              coverage.domain_owner_digest[0] == (sentinel == 0) &&
              coverage.recorded_domain_owner_digest[0] == (sentinel == 1) &&
              coverage.domain_output.return_begin == (sentinel == 2) &&
              hlsl_source_quality_results_equal(&quality, &previous_quality) &&
              map.complete == previous_map.complete && map.count == previous_map.count);
        for (size_t index = 0; index < map.count; ++index)
            CHECK(hlsl_expression_origins_equal(&map.origins[index], &previous_map.origins[index]));
        memset(&coverage, 0, sizeof(coverage));
        CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &coverage, &diagnostic) &&
              source.len == reference.len && !memcmp(source.buf, reference.buf, source.len + 1) &&
              hlsl_source_quality_results_equal(&quality, &previous_quality));
        hlsl_stage_coverage_dispose(&coverage); sb_free(&source);
    }
    sb_free(&reference); domain_fixture_dispose(&fixture);
    return true;
}

int main(void) {
    if (!natural_domain_source() || !domain_authority_negatives() ||
        !other_domain_shapes() || !factor_group_order() || !separated_point_and_location_identity() || !logical_coordinate_composition() ||
        !float3_domain_source("POINTVALUE", false) || !float3_domain_source("OBJECTCOORD", false) ||
        !float3_domain_source("COLOR", false) || !float3_domain_source("POINTVALUE", true) ||
        !float3_program_rejections() || !float3_observer_rollback() || !unchanged_float4_capture() ||
        !domain_capture_argument_preservation()) return 1;
    puts("Domain source units passed");
    return 0;
}
