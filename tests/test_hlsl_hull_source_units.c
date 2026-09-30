// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"
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
#define INSTRUCTION(opcode, length) \
    ((uint32_t)(opcode) | (uint32_t)(length) << 24)

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte)
        bytes[byte] = (uint8_t)(value >> (8 * byte));
}

/* Authored token grammar and signatures, with no captured byte array. */
static size_t write_signature(uint8_t *bytes, unsigned role, bool inner_first) {
    const bool patch = role == 2;
    const unsigned count = patch ? 4 : 1;
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
        const unsigned logical = patch && inner_first ? (field ? field - 1 : 3) : field;
        uint8_t *element = bytes + 8 + 24 * field;
        write_u32(element, (uint32_t)(name_offset +
            (patch && logical == 3 ? strlen(semantic) + 1 : 0)));
        write_u32(element + 4, patch && logical < 3 ? logical : 0);
        write_u32(element + 8, patch ? (logical < 3 ? 13 : 14) : 1);
        write_u32(element + 12, 3);
        write_u32(element + 16, patch ? field : 0);
        write_u32(element + 20, patch ? 0x0e01 : role ? 15 : 0x0f0f);
    }
    memcpy(bytes + name_offset, semantic, strlen(semantic) + 1);
    if (patch)
        memcpy(bytes + name_offset + strlen(semantic) + 1,
               "SV_InsideTessFactor", sizeof("SV_InsideTessFactor"));
    return size + 8;
}

static uint8_t *make_controlled_dxbc(uint32_t points, uint8_t scenario, size_t *size) {
    const uint32_t base_words[] = {
        INSTRUCTION(113, 1), INSTRUCTION(147, 1) | points << 11,
        INSTRUCTION(148, 1) | points << 11,
        INSTRUCTION(149, 1) | 2u << 11,
        INSTRUCTION(150, 1) | 1u << 11,
        INSTRUCTION(151, 1) | 3u << 11,
        INSTRUCTION(152, 2), 0x42000000,
        INSTRUCTION(106, 1) | 1u << 11,
        INSTRUCTION(115, 1), INSTRUCTION(153, 2), 3,
        INSTRUCTION(95, 2), 0x00017000,
        INSTRUCTION(103, 4), 0x00102012, 0, 17,
        INSTRUCTION(103, 4), 0x00102012, 1, 18,
        INSTRUCTION(103, 4), 0x00102012, 2, 19,
        INSTRUCTION(104, 2), 1,
        INSTRUCTION(91, 4), 0x00102012, 0, 3,
        INSTRUCTION(54, 4), 0x00100012, 0, 0x0001700a,
        INSTRUCTION(54, 6), 0x00902012, 0x0010000a, 0, 0x00004001, 0x40400000,
        INSTRUCTION(62, 1),
        INSTRUCTION(115, 1),
        INSTRUCTION(103, 4), 0x00102012, 3, 20,
        INSTRUCTION(54, 5), 0x00102012, 3, 0x00004001, 0x40800000,
        INSTRUCTION(62, 1)
    };
    uint32_t words[128];
    size_t word_count = 0;
    for (size_t word = 0; word < sizeof(base_words) / 4; ++word) {
        if ((scenario == 4 || scenario == 5) && word == 9) {
            const uint32_t cp_header[] = {INSTRUCTION(114, 1), INSTRUCTION(95, 2), 0x00016000,
                INSTRUCTION(95, 4), 0x002010f2, points, 0,
                INSTRUCTION(101, 3), 0x001020f2, 0,
                INSTRUCTION(104, 2), scenario == 5 ? 2 : 1,
                INSTRUCTION(54, 4), 0x00100012, 0, 0x00016001};
            memcpy(words + word_count, cp_header, sizeof(cp_header));
            word_count += sizeof(cp_header) / 4;
            if (scenario == 5) {
                const uint32_t chained[] = {INSTRUCTION(54, 5), 0x00100012, 1, 0x0010000a, 0};
                memcpy(words + word_count, chained, sizeof(chained));
                word_count += sizeof(chained) / 4;
            }
            const uint32_t cp_body[] = {INSTRUCTION(56, 12), 0x001020f2, 0,
                0x00004002, 0x3fa00000, 0x3fa00000, 0x3fa00000, 0x3fa00000,
                0x00a01e46, 0x0010000a, scenario == 5 ? 1 : 0, 0, INSTRUCTION(62, 1)};
            memcpy(words + word_count, cp_body, sizeof(cp_body));
            word_count += sizeof(cp_body) / 4;
        }
        /* New authored arithmetic follows the instance-index transport.
         * The inner read control deliberately has no phase-local definition. */
        if ((scenario == 1 || scenario == 2) && word == 36) {
            const uint32_t product[] = {INSTRUCTION(56, 7), 0x00100012, 1,
                0x00004001, 0x3fc00000, 0x00004001, 0x40000000};
            memcpy(words + word_count, product, sizeof(product));
            word_count += sizeof(product) / 4;
        }
        if (scenario == 2 && base_words[word] == INSTRUCTION(54, 5) && base_words[word + 1] == 0x00102012) {
            words[word_count++] = INSTRUCTION(104, 2);
            words[word_count++] = 2;
        }
        words[word_count++] = base_words[word];
    }
    if (scenario == 1 || scenario == 2) {
        /* Locate authored DCL_TEMPS instead of depending on byte offsets. */
        for (size_t word = 0; word + 1 < word_count; ++word)
            if (words[word] == INSTRUCTION(104, 2)) words[word + 1] = 2;
        if (scenario == 1) {
            /* Replace the outer literal with the full scalar product use. */
            for (size_t word = 0; word + 5 < word_count; ++word)
                if (words[word] == INSTRUCTION(54, 6) && words[word + 1] == 0x00902012) {
                    words[word + 4] = 0x0010000a;
                    words[word + 5] = 1;
                }
        } else if (scenario == 2) {
            for (size_t word = 0; word + 4 < word_count; ++word)
                if (words[word] == INSTRUCTION(54, 5) && words[word + 1] == 0x00102012) {
                    words[word + 3] = 0x0010000a;
                    words[word + 4] = 1;
                }
        }
    }
    if (scenario == 3) {
        /* Author the same two independent phase shapes in inner/outer order,
         * with the indexed outer group beginning at register1. */
        size_t markers[2] = {0};
        unsigned marker_count = 0;
        for (size_t word = 0; word < word_count;) {
            const unsigned opcode = words[word] & 0x7ffu;
            const size_t length = words[word] >> 24;
            if (opcode == 115 && marker_count < 2) markers[marker_count++] = word;
            word += length;
        }
        if (marker_count != 2) return NULL;
        uint32_t reordered[128];
        size_t written = 0;
        const size_t first[] = {0, markers[1], markers[0]};
        const size_t end[] = {markers[0], word_count, markers[1]};
        for (unsigned section = 0; section < 3; ++section) {
            for (size_t word = first[section]; word < end[section];) {
                const unsigned opcode = words[word] & 0x7ffu;
                const size_t length = words[word] >> 24;
                if (opcode == 54 && words[word + 1] == 0x00902012) {
                    reordered[written++] = INSTRUCTION(54, 7);
                    reordered[written++] = 0x00d02012;
                    reordered[written++] = 1;
                    memcpy(reordered + written, words + word + 2, 4 * (length - 2));
                    written += length - 2;
                } else {
                    memcpy(reordered + written, words + word, 4 * length);
                    if (opcode == 103) reordered[written + 2] = words[word + 3] == 20 ? 0 : words[word + 2] + 1;
                    if (opcode == 91) reordered[written + 2] = 1;
                    if (opcode == 54 && words[word + 1] == 0x00102012) reordered[written + 2] = 0;
                    written += length;
                }
                word += length;
            }
        }
        memcpy(words, reordered, 4 * written);
        word_count = written;
    }
    const size_t word_bytes = word_count * 4;
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    write_u32(bytes + 20, 1);
    write_u32(bytes + 28, 4);
    size_t offset = 48;
    for (unsigned role = 0; role < 3; ++role) {
        write_u32(bytes + 32 + 4 * role, (uint32_t)offset);
        offset += write_signature(bytes + offset, role, scenario == 3);
        offset = (offset + 3) & ~(size_t)3;
    }
    write_u32(bytes + 44, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    write_u32(bytes + offset + 4, (uint32_t)word_bytes + 8);
    write_u32(bytes + offset + 8, 0x00030050);
    write_u32(bytes + offset + 12, (uint32_t)word_count + 2);
    for (unsigned word = 0; word < word_count; ++word)
        write_u32(bytes + offset + 16 + 4 * word, words[word]);
    *size = offset + 16 + word_bytes;
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

static bool hull_fixture_init(HullFixture *fixture, uint32_t points, uint8_t scenario) {
    size_t size = 0;
    uint8_t *bytes = make_controlled_dxbc(points, scenario, &size);
    return hull_fixture_parse(fixture, bytes, size);
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

/* Independently authored quad/isoline token grammars exercise the generic
 * descriptor producer, including distinct raw per-edge names. */
static size_t shape_signature(uint8_t *bytes, unsigned role, bool isoline) {
    if (role != 2) return write_signature(bytes, role, false);
    const unsigned count = isoline ? 2 : 6;
    const size_t names = 8 + 24 * count;
    const size_t size = names + sizeof("SV_TessFactor") + (isoline ? 0 : sizeof("SV_InsideTessFactor"));
    memcpy(bytes, "PCSG", 4);
    write_u32(bytes + 4, (uint32_t)size);
    bytes += 8;
    write_u32(bytes, count);
    write_u32(bytes + 4, 8);
    for (unsigned field = 0; field < count; ++field) {
        const bool inner = !isoline && field >= 4;
        uint8_t *element = bytes + 8 + 24 * field;
        write_u32(element, (uint32_t)(names + (inner ? sizeof("SV_TessFactor") : 0)));
        write_u32(element + 4, inner ? field - 4 : field);
        write_u32(element + 8, isoline ? (field ? 15 : 16) : inner ? 12 : 11);
        write_u32(element + 12, 3);
        write_u32(element + 16, field);
        write_u32(element + 20, 0x0e01);
    }
    memcpy(bytes + names, "SV_TessFactor", sizeof("SV_TessFactor"));
    if (!isoline) memcpy(bytes + names + sizeof("SV_TessFactor"),
        "SV_InsideTessFactor", sizeof("SV_InsideTessFactor"));
    return size + 8;
}

static uint8_t *make_shape_dxbc(bool isoline, size_t *size) {
    uint32_t words[128];
    size_t count = 0;
#define WORD(value) words[count++] = (uint32_t)(value)
    const unsigned points = isoline ? 2 : 4;
    WORD(INSTRUCTION(113, 1));
    WORD(INSTRUCTION(147, 1) | points << 11);
    WORD(INSTRUCTION(148, 1) | points << 11);
    WORD(INSTRUCTION(149, 1) | (isoline ? 1u : 3u) << 11);
    WORD(INSTRUCTION(150, 1) | 1u << 11);
    WORD(INSTRUCTION(151, 1) | (isoline ? 2u : 3u) << 11);
    WORD(INSTRUCTION(152, 2)); WORD(0x42000000);
    WORD(INSTRUCTION(106, 1) | 1u << 11);
    for (unsigned phase = 0; phase < (isoline ? 1u : 2u); ++phase) {
        const unsigned factors = isoline ? 2 : phase ? 2 : 4;
        const unsigned base = phase ? 4 : 0;
        WORD(INSTRUCTION(115, 1));
        WORD(INSTRUCTION(153, 2)); WORD(factors);
        WORD(INSTRUCTION(95, 2)); WORD(0x00017000);
        for (unsigned factor = 0; factor < factors; ++factor) {
            const unsigned raw_name = isoline ? (factor ? 21 : 22) : phase ? 15 + factor : 11 + factor;
            WORD(INSTRUCTION(103, 4)); WORD(0x00102012); WORD(base + factor); WORD(raw_name);
        }
        WORD(INSTRUCTION(104, 2)); WORD(1);
        WORD(INSTRUCTION(91, 4)); WORD(0x00102012); WORD(base); WORD(factors);
        WORD(INSTRUCTION(54, 4)); WORD(0x00100012); WORD(0); WORD(0x0001700a);
        WORD(INSTRUCTION(54, base ? 7 : 6)); WORD(base ? 0x00d02012 : 0x00902012);
        if (base) WORD(base);
        WORD(0x0010000a); WORD(0); WORD(0x00004001); WORD(phase ? 0x40400000 : 0x40000000);
        WORD(INSTRUCTION(62, 1));
    }
#undef WORD
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    write_u32(bytes + 20, 1);
    write_u32(bytes + 28, 4);
    size_t offset = 48;
    for (unsigned role = 0; role < 3; ++role) {
        write_u32(bytes + 32 + 4 * role, (uint32_t)offset);
        offset += shape_signature(bytes + offset, role, isoline);
        offset = (offset + 3) & ~(size_t)3;
    }
    write_u32(bytes + 44, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    write_u32(bytes + offset + 4, (uint32_t)(4 * count + 8));
    write_u32(bytes + offset + 8, 0x00030050);
    write_u32(bytes + offset + 12, (uint32_t)(count + 2));
    for (size_t word = 0; word < count; ++word) write_u32(bytes + offset + 16 + 4 * word, words[word]);
    *size = offset + 16 + 4 * count;
    write_u32(bytes + 24, (uint32_t)*size);
    if (!dxbc_compute_hash(bytes, *size, bytes + 4)) return NULL;
    uint8_t *result = malloc(*size);
    if (result) memcpy(result, bytes, *size);
    return result;
}

static bool other_domains_and_control_point_counts(void) {
    for (unsigned shape = 0; shape < 2; ++shape) {
        const bool isoline = shape != 0;
        size_t size = 0;
        uint8_t *bytes = make_shape_dxbc(isoline, &size);
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

int main(void) {
    return natural_hull_source() && arithmetic_and_phase_ownership() &&
        malformed_contracts() && reordered_phase_roles() && scoped_cfg_ownership() &&
        other_domains_and_control_point_counts() && explicit_control_point_phase() ? 0 : 1;
}
