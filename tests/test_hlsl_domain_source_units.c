// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"

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
#define INSTRUCTION(opcode, length) \
    ((uint32_t)(opcode) | (uint32_t)(length) << 24)

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte)
        bytes[byte] = (uint8_t)(value >> (8 * byte));
}

/* Authored token grammar and signatures, with no captured byte array. */
static size_t write_signature(uint8_t *bytes, unsigned role, unsigned domain) {
    const bool patch = role == 2;
    const unsigned outer = domain == 2 ? 3 : domain == 3 ? 4 : 2;
    const unsigned inner = domain == 2 ? 1 : domain == 3 ? 2 : 0;
    const unsigned count = patch ? outer + inner : 1;
    const char *semantic = patch ? "SV_TessFactor" : "SV_POSITION";
    const size_t name_offset = 8 + count * 24;
    const size_t size = name_offset + strlen(semantic) + 1 +
        (patch ? sizeof("SV_InsideTessFactor") : 0);
    memcpy(bytes, patch ? "PCSG" : role ? "OSGN" : "ISGN", 4);
    write_u32(bytes + 4, (uint32_t)size);
    bytes += 8;
    write_u32(bytes, count);
    write_u32(bytes + 4, 8);
    for (unsigned field = 0; field < count; ++field) {
        uint8_t *element = bytes + 8 + 24 * field;
        write_u32(element, (uint32_t)(name_offset +
            (patch && field >= outer ? strlen(semantic) + 1 : 0)));
        write_u32(element + 4, patch ? (field < outer ? field : field - outer) : 0);
        const unsigned system = domain == 2 ? (field < outer ? 13 : 14) :
            domain == 3 ? (field < outer ? 11 : 12) : (field ? 15 : 16);
        write_u32(element + 8, patch ? system : 1);
        write_u32(element + 12, 3);
        write_u32(element + 16, patch ? field : 0);
        write_u32(element + 20, patch ? 1 : role ? 15 : 0x0f0f);
    }
    memcpy(bytes + name_offset, semantic, strlen(semantic) + 1);
    if (patch)
        memcpy(bytes + name_offset + strlen(semantic) + 1,
               "SV_InsideTessFactor", sizeof("SV_InsideTessFactor"));
    return size + 8;
}

static uint8_t *make_controlled_dxbc(unsigned domain, uint32_t points, uint8_t location_mask,
                                     size_t *size) {
    const unsigned coordinate_y = domain == 1 ? 0 : 1;
    const unsigned coordinate_z = domain == 2 ? 2 : 0;
    const uint32_t words[] = {
        INSTRUCTION(147, 1) | (points << 11),
        INSTRUCTION(149, 1) | ((uint32_t)domain << 11),
        INSTRUCTION(106, 1) | (1u << 11),
        INSTRUCTION(95, 2), 0x0001c002u | (uint32_t)location_mask << 4,
        INSTRUCTION(95, 4), 0x002190f2, points, 0,
        INSTRUCTION(103, 4), 0x001020f2, 0, 1,
        INSTRUCTION(104, 2), 1,
        INSTRUCTION(56, 7), 0x001000f2, 0, 0x0001c006u | ((uint32_t)coordinate_y * 0x55u << 4),
            0x00219e46, 1, 0,
        INSTRUCTION(50, 9), 0x001000f2, 0, 0x00219e46, 0, 0,
            0x0001c006, 0x00100e46, 0,
        INSTRUCTION(50, 9), 0x001020f2, 0, 0x00219e46, points == 2 ? 1u : 2u, 0,
            0x0001c006u | ((uint32_t)coordinate_z * 0x55u << 4), 0x00100e46, 0,
        INSTRUCTION(62, 1)
    };
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    write_u32(bytes + 20, 1);
    write_u32(bytes + 28, 4);
    size_t offset = 48;
    for (unsigned role = 0; role < 3; ++role) {
        write_u32(bytes + 32 + 4 * role, (uint32_t)offset);
        offset += write_signature(bytes + offset, role, domain);
        offset = (offset + 3) & ~(size_t)3;
    }
    write_u32(bytes + 44, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    write_u32(bytes + offset + 4, sizeof(words) + 8);
    write_u32(bytes + offset + 8, 0x00040050);
    write_u32(bytes + offset + 12, sizeof(words) / 4 + 2);
    for (unsigned word = 0; word < sizeof(words) / 4; ++word)
        write_u32(bytes + offset + 16 + 4 * word, words[word]);
    *size = offset + 16 + sizeof(words);
    write_u32(bytes + 24, (uint32_t)*size);
    if (!dxbc_compute_hash(bytes, *size, bytes + 4)) return NULL;
    uint8_t *result = malloc(*size);
    if (result) memcpy(result, bytes, *size);
    return result;
}

typedef struct {
    DXBCDocument document;
    DXBCContainer semantic;
    DXBCStageContract contract;
    USILProgram program;
} DomainFixture;

static bool domain_fixture_init_shape(DomainFixture *fixture, unsigned domain, uint32_t points,
                                 uint8_t location_mask) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    size_t size = 0;
    uint8_t *bytes = make_controlled_dxbc(domain, points, location_mask, &size);
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

int main(void) {
    if (!natural_domain_source() || !domain_authority_negatives() ||
        !other_domain_shapes() || !factor_group_order() || !separated_point_and_location_identity() || !logical_coordinate_composition()) return 1;
    puts("Domain source units passed");
    return 0;
}
