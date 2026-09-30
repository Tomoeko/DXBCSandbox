// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "test_tessellation_fixture.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_global_declarations.h"
#include "translation/usil_validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "CHECK %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            return false;                                                      \
        }                                                                      \
    } while (0)
#define WORD(op, n) ((uint32_t)(op) | (uint32_t)(n) << 24)
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void write_u32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (i * 8));
}

typedef struct {
    DXBCDocument document;
    DXBCContainer semantic;
    DXBCStageContract contract;
    USILProgram program;
    SerializedVariable field;
    SerializedConstantBuffer buffer;
    SerializedResourceParam binding;
    SerializedProgramParameters parameters;
} Fixture;

typedef struct {
    unsigned factor_opcode;
    unsigned first_lane, second_lane;
    bool scalar_cbuffer, two_hops;
    unsigned control_point_opcode;
    enum {
        INDEX_VALID = 0,
        INDEX_UNDEFINED,
        INDEX_MISMATCHED_LANE,
        INDEX_MULTIBIT,
        INDEX_WRONG_TEMP,
        INDEX_JOIN_ID,
        INDEX_CONTROL_POINT_ID,
        INDEX_TRANSFORMED,
        INDEX_FLOAT_USE,
        INDEX_CROSS_PHASE_USE
    } index_case;
} FactorGrammar;

/* Compose an independent declaration/source grammar into the shared authored
 * tessellation fixture. No compiler target bytes or source patterns are copied.
 */
static bool fixture_init_grammar(Fixture *f, bool inner_first, bool named,
                                 bool control_point, bool float3,
                                 const FactorGrammar *grammar) {
    memset(f, 0, sizeof(*f));
    size_t size = 0;
    const uint8_t scenario = control_point ? 4 : inner_first ? 3 : 0;
    uint8_t *base = float3 ? test_tessellation_hull_float3_dxbc(
                                 3, 3, scenario, "POINTVALUE", &size)
                           : test_tessellation_hull_dxbc(3, 3, scenario, &size);
    CHECK(base);
    const uint32_t offset = read_u32(base + 44);
    CHECK(offset + 16 <= size && !memcmp(base + offset, "SHEX", 4));
    uint32_t words[160];
    size_t written = 0;
    unsigned replaced = 0;
    bool declared = false;
    for (size_t byte = offset + 16; byte < size;) {
        const uint32_t token = read_u32(base + byte);
        const unsigned length = token >> 24;
        const unsigned opcode = token & 0x7ffu;
        CHECK(length && byte + length * 4 <= size &&
              written + length + 12 < 160);
        const bool scalar_cbuffer = !grammar || grammar->scalar_cbuffer;
        if ((opcode == 114 || opcode == 115) && !declared && scalar_cbuffer) {
            words[written++] = WORD(89, 4);
            words[written++] = 0x00208000;
            words[written++] = 0;
            words[written++] = 1;
            declared = true;
        }
        if (grammar && opcode == 104) {
            words[written++] = token;
            words[written++] = grammar->two_hops ? 2 : 1;
        } else if (grammar && opcode == 54 && length == 4 &&
                   read_u32(base + byte + 4) == 0x00100012 &&
                   read_u32(base + byte + 12) == 0x0001700a) {
            if (grammar->index_case == INDEX_UNDEFINED) {
                byte += length * 4;
                continue;
            }
            const bool transformed = grammar->index_case == INDEX_TRANSFORMED;
            unsigned lanes = 1u << grammar->first_lane;
            if (grammar->index_case == INDEX_MULTIBIT)
                lanes |= 1u << ((grammar->first_lane + 1) % 4);
            words[written++] = transformed ? WORD(0, 6) : token;
            words[written++] = 0x00100002u | lanes << 4;
            words[written++] = grammar->index_case == INDEX_WRONG_TEMP ? 1 : 0;
            words[written++] = grammar->index_case == INDEX_JOIN_ID
                                    ? 0x0001800a
                                : grammar->index_case == INDEX_CONTROL_POINT_ID
                                    ? 0x0001600a
                                    : 0x0001700a;
            if (transformed) {
                words[written++] = 0x00004001;
                words[written++] = 0;
            }
            if (grammar->two_hops) {
                words[written++] = WORD(54, 5);
                words[written++] =
                    0x00100002u | (1u << grammar->second_lane) << 4;
                words[written++] = 1;
                const unsigned read_lane =
                    grammar->index_case == INDEX_MISMATCHED_LANE
                        ? (grammar->first_lane + 1) % 4
                        : grammar->first_lane;
                words[written++] = 0x0010000au | read_lane << 4;
                words[written++] = 0;
            }
        } else if (grammar && opcode == 54 &&
                   ((length == 6 || length == 7) &&
                    (read_u32(base + byte + 4) == 0x00902012 ||
                     read_u32(base + byte + 4) == 0x00d02012))) {
            const bool index_float = grammar->index_case == INDEX_FLOAT_USE;
            const unsigned source_words = scalar_cbuffer && !index_float ? 3 : 2;
            words[written++] = WORD(grammar->factor_opcode,
                                      length - 2 + source_words + 2);
            for (unsigned word = 1; word < length - 2; ++word) {
                uint32_t operand = read_u32(base + byte + word * 4);
                if (operand == 0x0010000a)
                    operand |= (grammar->two_hops ? grammar->second_lane
                                                   : grammar->first_lane)
                               << 4;
                else if (word == length - 3)
                    operand = grammar->two_hops ? 1 : 0;
                words[written++] = operand;
            }
            words[written++] = index_float
                                   ? 0x0010000au |
                                         (grammar->two_hops ? grammar->second_lane
                                                             : grammar->first_lane)
                                             << 4
                                   : scalar_cbuffer ? 0x0020800a : 0x00004001;
            words[written++] = index_float ? (grammar->two_hops ? 1 : 0)
                                          : scalar_cbuffer ? 0 : 0x40400000;
            if (scalar_cbuffer && !index_float) words[written++] = 0;
            words[written++] = 0x00004001;
            words[written++] = 0x41000000;
            ++replaced;
        } else if (grammar && opcode == 54 && length == 5 &&
                   read_u32(base + byte + 4) == 0x00102012) {
            const bool cross_phase =
                grammar->index_case == INDEX_CROSS_PHASE_USE;
            const unsigned source_words = scalar_cbuffer && !cross_phase ? 3 : 2;
            if (cross_phase) {
                /* Declare a valid inner-phase temporary bank while leaving its
                 * register undefined in that phase. The rejection then belongs
                 * to the independent SSA owner, not declaration validity. */
                words[written++] = WORD(104, 2);
                words[written++] = 2;
            }
            words[written++] = WORD(grammar->factor_opcode,
                                      3 + source_words + 2);
            words[written++] = 0x00102012;
            words[written++] = read_u32(base + byte + 8);
            words[written++] = cross_phase
                                   ? 0x0010000au | grammar->first_lane << 4
                                   : scalar_cbuffer ? 0x0020800a : 0x00004001;
            words[written++] = scalar_cbuffer && !cross_phase ? 0
                                      : cross_phase ? 0 : 0x40800000;
            if (scalar_cbuffer && !cross_phase) words[written++] = 0;
            words[written++] = 0x00004001;
            words[written++] = 0x41000000;
            ++replaced;
        } else if (grammar && grammar->control_point_opcode && opcode == 56) {
            words[written++] = WORD(grammar->control_point_opcode, length);
            for (unsigned word = 1; word < length; ++word)
                words[written++] = read_u32(base + byte + word * 4);
        } else if (opcode == 54 && (length == 6 || length == 7) &&
            (read_u32(base + byte + 4) == 0x00902012 ||
             read_u32(base + byte + 4) == 0x00d02012)) {
            words[written++] = WORD(54, length + 1);
            for (unsigned word = 1; word < length - 2; ++word)
                words[written++] = read_u32(base + byte + word * 4);
            words[written++] = 0x0020800a;
            words[written++] = 0;
            words[written++] = 0;
            ++replaced;
        } else if (opcode == 54 && length == 5 &&
                   read_u32(base + byte + 4) == 0x00102012) {
            words[written++] = WORD(54, 6);
            words[written++] = 0x00102012;
            words[written++] = read_u32(base + byte + 8);
            words[written++] = 0x0020800a;
            words[written++] = 0;
            words[written++] = 0;
            ++replaced;
        } else
            for (unsigned word = 0; word < length; ++word)
                words[written++] = read_u32(base + byte + word * 4);
        byte += length * 4;
    }
    CHECK((declared || (grammar && !grammar->scalar_cbuffer)) && replaced == 2);
    const size_t new_size = offset + 16 + written * 4;
    uint8_t *bytes = calloc(new_size, 1);
    CHECK(bytes);
    memcpy(bytes, base, offset + 16);
    free(base);
    write_u32(bytes + 24, (uint32_t)new_size);
    write_u32(bytes + offset + 4, (uint32_t)written * 4 + 8);
    write_u32(bytes + offset + 12, (uint32_t)written + 2);
    for (size_t word = 0; word < written; ++word)
        write_u32(bytes + offset + 16 + word * 4, words[word]);
    CHECK(dxbc_compute_hash(bytes, new_size, bytes + 4));
    dxbc_document_init(&f->document);
    dxbc_stage_contract_init(&f->contract);
    DXBCDocumentDiagnostic dd;
    DXBCStageContractDiagnostic sd;
    const bool parsed = dxbc_document_parse(&f->document, bytes, new_size, &dd);
    free(bytes);
    CHECK(parsed);
    CHECK(dxbc_document_decode_semantic(&f->document, &f->semantic));
    CHECK(dxbc_stage_contract_decode(&f->document, &f->semantic, &f->contract,
                                     &sd));
    const bool translated = usil_translate_with_stage_contract(
        &f->program, &f->semantic, &f->contract);
    if (!translated && grammar)
        fprintf(stderr, "HULL authored grammar translation failed: case=%u "
                        "order=%u lanes=%u/%u cb=%u cp=%u\n",
                (unsigned)grammar->index_case, (unsigned)inner_first,
                grammar->first_lane, grammar->second_lane,
                (unsigned)grammar->scalar_cbuffer,
                grammar->control_point_opcode);
    CHECK(translated);
    f->field =
        (SerializedVariable){.name = "_Factor", .layout = {0, 0, 0, 1, 0, 0}};
    f->buffer =
        (SerializedConstantBuffer){.name = named ? "FactorInputs" : "$Globals",
                                   .size = 16,
                                   .role = SERIALIZED_CBUFFER_NAMED,
                                   .variables = &f->field,
                                   .var_count = 1};
    f->binding = (SerializedResourceParam){
        .name = f->buffer.name,
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER,
        .array_size = 1};
    f->parameters =
        (SerializedProgramParameters){.constant_buffers = &f->buffer,
                                      .cb_count = 1,
                                      .resources = &f->binding,
                                      .res_count = 1};
    return true;
}
static bool fixture_init_shape(Fixture *f, bool inner_first, bool named,
                               bool control_point, bool float3) {
    return fixture_init_grammar(f, inner_first, named, control_point, float3,
                                NULL);
}
static bool fixture_init_scenario(Fixture *f, bool inner_first, bool named,
                                  bool control_point) {
    return fixture_init_shape(f, inner_first, named, control_point, false);
}
static bool fixture_init(Fixture *f, bool inner_first, bool named) {
    return fixture_init_scenario(f, inner_first, named, false);
}
static void fixture_free(Fixture *f) {
    usil_free(&f->program);
    dxbc_free(&f->semantic);
    dxbc_stage_contract_free(&f->contract);
    dxbc_document_free(&f->document);
}

static bool emit_with_union(Fixture *f, bool expected,
                            const SerializedProgramParameters *current,
                            const SerializedProgramParameters *common,
                            const HLSLGlobalDeclarationUnion *declarations) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    HLSLEmitDiagnostic diagnostic;
    options.global_declarations = declarations;
    options.source_quality = &quality;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    const bool emitted = hlsl_emit_with_options_diagnostic(
        &f->program, &source, current, common, NULL, &options, &diagnostic);
    if (emitted != expected)
        fprintf(stderr,
                "hull scalar result=%d status=%s phase=%s reason=%s "
                "instruction=%d\n",
                emitted, hlsl_emit_status_name(diagnostic.status),
                hlsl_emit_phase_name(diagnostic.phase),
                hlsl_emit_reason_name(diagnostic.reason),
                diagnostic.instruction_index);
    CHECK(emitted == expected);
    if (expected) {
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(quality.counts.incomplete_units == 0 &&
              quality.counts.unknown_provenance == 0);
        CHECK(
            hlsl_expression_source_map_matches(&map, &f->program, source.buf));
        char outer[256], inner[256];
        const int outer_length =
            snprintf(outer, sizeof(outer), "factors.outer[factorIndex] = (%s);",
                     f->field.name);
        const int inner_length = snprintf(
            inner, sizeof(inner), "factors.inner = (%s);", f->field.name);
        CHECK(outer_length > 0 && (size_t)outer_length < sizeof(outer));
        CHECK(inner_length > 0 && (size_t)inner_length < sizeof(inner));
        CHECK(strstr(source.buf, outer) && strstr(source.buf, inner));
        CHECK(!strstr(source.buf, "cb0_pad") &&
              !strstr(source.buf, "asfloat("));
        CHECK(!strstr(source.buf, "_Factor.x"));
        if (strcmp(f->buffer.name, "$Globals"))
            CHECK(quality.counts.cbuffer_declarations == 1 &&
                  quality.counts.cbuffer_fields == 1);
    } else {
        CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK);
        CHECK(!map.complete && map.count == 0);
    }
    sb_free(&source);
    return true;
}

static bool emit(Fixture *f, bool expected,
                 const SerializedProgramParameters *current,
                 const SerializedProgramParameters *common) {
    return emit_with_union(f, expected, current, common, NULL);
}

static bool emit_factor_grammar(Fixture *f, const FactorGrammar *grammar,
                                bool expected, const char *reserved) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    HLSLEmitDiagnostic diagnostic;
    options.source_quality = &quality;
    options.expression_source_map = &map;
    if (reserved) {
        options.reserved_preprocessor_identifiers = &reserved;
        options.reserved_preprocessor_identifier_count = 1;
    }
    StringBuilder source;
    sb_init(&source);
    const bool emitted = hlsl_emit_with_options_diagnostic(
        &f->program, &source,
        grammar->scalar_cbuffer ? &f->parameters : NULL, NULL, NULL, &options,
        &diagnostic);
    if (emitted != expected)
        fprintf(stderr,
                "HULL factor opcode=%u lanes=%u/%u hops=%u cb=%u case=%u "
                "result=%d reason=%s instruction=%d\n",
                grammar->factor_opcode, grammar->first_lane,
                grammar->second_lane, grammar->two_hops,
                grammar->scalar_cbuffer, grammar->index_case, emitted,
                hlsl_emit_reason_name(diagnostic.reason),
                diagnostic.instruction_index);
    CHECK(emitted == expected);
    if (expected) {
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !quality.counts.unknown_provenance &&
              !quality.counts.incomplete_units);
        CHECK(hlsl_expression_source_map_matches(&map, &f->program, source.buf));
        const char *operation = grammar->factor_opcode == 51 ? "min(" : "max(";
        const char *first = strstr(source.buf, operation);
        CHECK(first && strstr(first + strlen(operation), operation));
        CHECK(strstr(source.buf, "factors.outer[factorIndex]") &&
              strstr(source.buf, "factors.inner"));
        CHECK(!strstr(source.buf, "asfloat(") && !strstr(source.buf, "cb0") &&
              !strstr(source.buf, "icb") && !strstr(source.buf, "r0") &&
              !strstr(source.buf, "r1") && !strstr(source.buf, ".xxxx") &&
              !strstr(source.buf, ".yyyy") && !strstr(source.buf, ".zzzz") &&
              !strstr(source.buf, ".wwww") && !strstr(source.buf, ".x") &&
              !strstr(source.buf, ".y") && !strstr(source.buf, ".z") &&
              !strstr(source.buf, ".w"));
        CHECK(quality.counts.cbuffer_declarations ==
                  (grammar->scalar_cbuffer ? 1u : 0u) &&
              quality.counts.cbuffer_fields ==
                  (grammar->scalar_cbuffer ? 1u : 0u));
        if (grammar->scalar_cbuffer) CHECK(strstr(source.buf, "_Factor"));
        unsigned index_owners = 0;
        size_t shared_begin = 0, shared_end = 0;
        for (int index = 0; index < f->program.instruction_count; ++index) {
            const USILInstruction *instruction = &f->program.instructions[index];
            if (instruction->opcode != USIL_OP_MOV) continue;
            ++index_owners;
            const HLSLExpressionOrigin *origin = &map.origins[index];
            CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION &&
                  origin->instruction_index == index &&
                  origin->source_instruction_index ==
                      instruction->source_instruction_index &&
                  origin->destination_lanes == usil_operand_destination_lane_mask(
                                                   &instruction->operands[0]) &&
                  origin->source_end - origin->source_begin ==
                      strlen("factorIndex") &&
                  !memcmp(source.buf + origin->source_begin, "factorIndex",
                          strlen("factorIndex")));
            if (index_owners == 1) {
                shared_begin = origin->source_begin;
                shared_end = origin->source_end;
            } else
                CHECK(origin->source_begin == shared_begin &&
                      origin->source_end == shared_end);
        }
        CHECK(index_owners == (grammar->two_hops ? 2u : 1u));
    } else {
        CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
              diagnostic.status != HLSL_EMIT_STATUS_OK && !map.complete &&
              !map.count);
    }
    sb_free(&source);
    return true;
}

static bool factor_minmax_and_lane_matrix(void) {
    /* Every accepted lane and cross-lane copy is authored in the token stream
     * before the document, stage contract and USIL are decoded. The resulting
     * map must own both elided physical copies at the natural loop index. */
    for (unsigned cbuffer = 0; cbuffer < 2; ++cbuffer)
        for (unsigned float3 = 0; float3 < 2; ++float3)
            for (unsigned order = 0; order < 2; ++order)
                for (unsigned opcode = 51; opcode <= 52; ++opcode)
                    for (unsigned hops = 1; hops <= 2; ++hops)
                        for (unsigned first = 0; first < 4; ++first)
                            for (unsigned second = 0;
                                 second < (hops == 2 ? 4u : 1u); ++second) {
                                const FactorGrammar grammar = {
                                    .factor_opcode = opcode,
                                    .first_lane = first,
                                    .second_lane = second,
                                    .scalar_cbuffer = cbuffer != 0,
                                    .two_hops = hops == 2};
                                Fixture f;
                                CHECK(fixture_init_grammar(
                                    &f, order != 0, true, false, float3 != 0,
                                    &grammar));
                                CHECK(emit_factor_grammar(&f, &grammar, true,
                                                          NULL));
                                fixture_free(&f);
                            }
    return true;
}

static bool factor_index_rejections(void) {
    /* A defined integer index is never interchangeable with an ordinary
     * floating expression. Exercise non-X lanes as well as independent phase
     * ownership with authored sources, not only mutated model fields. */
    for (unsigned cbuffer = 0; cbuffer < 2; ++cbuffer)
        for (unsigned float3 = 0; float3 < 2; ++float3)
            for (unsigned order = 0; order < 2; ++order)
                for (unsigned scenario = INDEX_UNDEFINED;
                     scenario <= INDEX_CROSS_PHASE_USE; ++scenario) {
                    FactorGrammar grammar = {.factor_opcode = 51,
                                             .first_lane = 2,
                                             .second_lane = 3,
                                             .scalar_cbuffer = cbuffer != 0,
                                             .two_hops = true};
                    grammar.index_case = scenario;
                    Fixture f;
                    CHECK(fixture_init_grammar(&f, order != 0, true, false,
                                               float3 != 0, &grammar));
                    CHECK(emit_factor_grammar(&f, &grammar, false, NULL));
                    fixture_free(&f);
                }
    return true;
}

static bool factor_modifiers_and_macro_rejections(void) {
    const FactorGrammar grammar = {.factor_opcode = 52,
                                   .first_lane = 1,
                                   .second_lane = 3,
                                   .scalar_cbuffer = true,
                                   .two_hops = true};
    for (unsigned scenario = 0; scenario < 13; ++scenario) {
        Fixture f;
        CHECK(fixture_init_grammar(&f, false, true, false, true, &grammar));
        USILInstruction *copy = &f.program.instructions[0];
        USILInstruction *factor = &f.program.instructions[2];
        CHECK(copy->opcode == USIL_OP_MOV && factor->opcode == USIL_OP_MAX);
        switch (scenario) {
        case 0: copy->precise_mask = 2; break;
        case 1: copy->saturate = true; break;
        case 2: copy->operands[0].min_precision = 1; break;
        case 3: copy->operands[1].has_abs = true; break;
        case 4: copy->operands[1].has_neg = true; break;
        case 5: factor->precise_mask = 1; break;
        case 6: factor->saturate = true; break;
        case 7: factor->operands[1].min_precision = 1; break;
        case 8: factor->operands[1].has_abs = true; break;
        case 9: factor->operands[1].has_neg = true; break;
        case 10: factor->operands[0].rel_op0->has_neg = true; break;
        case 11: factor->operands[0].rel_op0->has_abs = true; break;
        case 12: factor->operands[0].rel_op0->min_precision = 1; break;
        }
        CHECK(emit_factor_grammar(&f, &grammar, false, NULL));
        fixture_free(&f);
    }
    const char *reserved[] = {"min", "max"};
    for (size_t index = 0; index < sizeof(reserved) / sizeof(reserved[0]);
         ++index) {
        Fixture f;
        CHECK(fixture_init_grammar(&f, false, true, false, true, &grammar));
        CHECK(emit_factor_grammar(&f, &grammar, false, reserved[index]));
        CHECK(emit_factor_grammar(&f, &grammar, true, NULL));
        fixture_free(&f);
    }
    /* Scope the opcode extension to scalar factor phases. A separately parsed
     * FLOAT4 control-point expression still accepts MUL and rejects MIN/MAX. */
    for (unsigned opcode = 51; opcode <= 52; ++opcode) {
        FactorGrammar cp_grammar = grammar;
        cp_grammar.two_hops = false;
        cp_grammar.control_point_opcode = opcode;
        Fixture f;
        CHECK(fixture_init_grammar(&f, false, true, true, false, &cp_grammar));
        CHECK(emit_factor_grammar(&f, &cp_grammar, false, NULL));
        fixture_free(&f);
    }
    return true;
}

static bool positive(void) {
    for (unsigned named = 0; named < 2; ++named)
        for (unsigned order = 0; order < 2; ++order) {
            Fixture f;
            CHECK(fixture_init(&f, order != 0, named != 0));
            CHECK(emit(&f, true, &f.parameters, NULL));
            CHECK(emit(&f, true, NULL, &f.parameters));
            fixture_free(&f);
        }
    return true;
}

static bool float3_scalar_authority(void) {
    /* Parsed custom FLOAT3 signatures combine with each independently supplied
     * declaration shape. These are controlled API fixtures, not player
     * captures. */
    for (unsigned named = 0; named < 2; ++named) {
        for (unsigned order = 0; order < 2; ++order) {
            for (unsigned authority = 0; authority < 3; ++authority) {
                Fixture f;
                CHECK(fixture_init_shape(&f, order != 0, named != 0, false,
                                         true));
                CHECK(f.program.inputs[0].mask == 7 &&
                      f.program.inputs[0].rw_mask == 7 &&
                      f.program.outputs[0].mask == 7 &&
                      f.program.outputs[0].rw_mask == 8);
                SerializedConstantBuffer current_buffers[2] = {
                    {.name = "$Globals",
                     .role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS},
                    {.name = f.buffer.name,
                     .role = SERIALIZED_CBUFFER_NAMED,
                     .size = 16}};
                SerializedProgramParameters current = {
                    .constant_buffers = current_buffers, .cb_count = 2};
                if (authority == 0)
                    CHECK(emit(&f, true, &f.parameters, NULL));
                if (authority == 1)
                    CHECK(emit(&f, true, NULL, &f.parameters));
                if (authority == 2) {
                    f.buffer.has_is_partial = f.buffer.is_partial = true;
                    CHECK(emit(&f, true, &current, &f.parameters));
                    /* A partial field never authorizes a missing or larger
                     * current shell just because the interface was lifted. */
                    CHECK(emit(&f, false, NULL, &f.parameters));
                    current_buffers[1].size = 32;
                    CHECK(emit(&f, false, &current, &f.parameters));
                }
                fixture_free(&f);
            }
        }
    }
    Fixture explicit_cp;
    CHECK(fixture_init_shape(&explicit_cp, false, true, true, true));
    CHECK(emit(&explicit_cp, false, &explicit_cp.parameters, NULL));
    fixture_free(&explicit_cp);
    return true;
}

static bool float3_scalar_name_collisions(void) {
    static const struct {
        const char *field, *buffer, *point_type, *point_field;
    } cases[] = {{"_Factor", "FactorInputs", "HullPoint", "pointValue"},
                 {"pointValue", "FactorInputs", "HullPoint", "pointValue_1"},
                 {"_Factor", "HullPoint", "HullPoint_1", "pointValue"},
                 {"pointValue", "HullPoint", "HullPoint_1", "pointValue_1"},
                 {"_Factor", "pointValue", "HullPoint", "pointValue_1"},
                 {"HullPoint", "FactorInputs", "HullPoint_1", "pointValue"}};
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        Fixture f;
        CHECK(fixture_init_shape(&f, index % 2 != 0, true, false, true));
        f.field.name = (char *)cases[index].field;
        f.buffer.name = f.binding.name = (char *)cases[index].buffer;
        CHECK(emit(&f, true, &f.parameters, NULL));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map;
        options.source_quality = &quality;
        options.expression_source_map = &map;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&f.program, &source, &f.parameters, NULL,
                                     NULL, &options));
        char expected[256];
        int length = snprintf(expected, sizeof(expected), "struct %s {",
                              cases[index].point_type);
        CHECK(length > 0 && (size_t)length < sizeof(expected) &&
              strstr(source.buf, expected));
        length = snprintf(expected, sizeof(expected), "float3 %s : POINTVALUE;",
                          cases[index].point_field);
        CHECK(length > 0 && (size_t)length < sizeof(expected) &&
              strstr(source.buf, expected));
        length = snprintf(expected, sizeof(expected),
                          "cbuffer %s : register(b0) {\n",
                          cases[index].buffer);
        CHECK(length > 0 && (size_t)length < sizeof(expected) &&
              strstr(source.buf, expected));
        /* Named buffer fields retain their actual byte-zero location through
         * the shared declarator, rather than an unqualified global form. */
        length = snprintf(expected, sizeof(expected),
                          "    float %s : packoffset(c0);\n",
                          cases[index].field);
        CHECK(length > 0 && (size_t)length < sizeof(expected) &&
              strstr(source.buf, expected));
        CHECK(!strstr(source.buf, "clipPosition") &&
              !strstr(source.buf, "float4"));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !quality.counts.unknown_provenance &&
              !quality.counts.incomplete_units &&
              quality.counts.cbuffer_declarations == 1 &&
              quality.counts.cbuffer_fields == 1);
        CHECK(hlsl_expression_source_map_matches(&map, &f.program, source.buf));
        sb_free(&source);
        fixture_free(&f);
    }
    return true;
}
static bool negatives(void) {
    for (unsigned scenario = 0; scenario < 33; ++scenario) {
        Fixture f;
        CHECK(fixture_init(&f, false, false));
        const SerializedProgramParameters *parameters = &f.parameters;
        switch (scenario) {
        case 0:
            parameters = NULL;
            break;
        case 1:
            f.program.cbuffers[0].dynamic_indexed = true;
            break;
        case 2:
            f.program.cbuffers[0].size = 2;
            break;
        case 3:
            f.program.cbuffers[0].reg_idx = 1;
            break;
        case 4:
            f.buffer.size = 32;
            break;
        case 5:
            f.buffer.var_count = 0;
            break;
        case 6:
            f.field.layout[0] = 4;
            break;
        case 7:
            f.field.layout[1] = 2;
            break;
        case 8:
            f.field.layout[2] = 1;
            break;
        case 9:
            f.field.layout[3] = 2;
            break;
        case 10:
            f.field.name = "for";
            break;
        case 11:
            f.binding.bind_index = 1;
            break;
        case 12:
            f.program.instructions[1].precise_mask = 1;
            break;
        case 13:
            f.program.instructions[1].saturate = true;
            break;
        case 14:
            f.program.instructions[1].operands[1].has_abs = true;
            break;
        case 15:
            f.program.instructions[1].operands[1].index_values[1] = 1;
            f.program.instructions[1].operands[1].rel_offset0 = 1;
            break;
        case 16:
            f.program.instructions[1].operands[1].swizzle[0] = 1;
            break;
        case 17:
            f.buffer.role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
            break;
        case 18:
            f.buffer.has_is_partial = f.buffer.is_partial = true;
            break;
        case 19:
            f.parameters.res_count = 0;
            break;
        case 20:
            f.parameters.cb_count = -1;
            break;
        case 21:
            f.parameters.cb_count = 33;
            break;
        case 22:
            f.parameters.res_count = 33;
            break;
        case 23:
            f.buffer.var_count = 33;
            break;
        case 24:
            f.binding.name = "_Foreign";
            break;
        case 25:
            f.buffer.name = "_Foreign";
            break;
        case 26:
            f.program.instructions[1].operands[1].min_precision = 1;
            break;
        case 27:
            f.binding.array_size = 2;
            break;
        case 28:
            f.binding.bind_type = SERIALIZED_RESOURCE_TEXTURE;
            break;
        case 29:
            f.parameters.res_count = 2;
            break;
        case 30:
            f.buffer.var_count = 2;
            break;
        case 31:
            f.field.name = "main";
            break;
        case 32:
            f.buffer.name = f.binding.name = "main";
            break;
        }
        CHECK(emit(&f, false, parameters, NULL));
        fixture_free(&f);
    }
    return true;
}
static bool declaration_union_authority(void) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Fixture f;
        CHECK(fixture_init(&f, false, false));
        const int stage = scenario == 1 ? 1 : 3;
        int platform = 4;
        SerializedSubProgram subprograms[2] = {{0}};
        SerializedSubProgramIdentity identities[2] = {{0}};
        PlayerSubProgramMetadata players[2] = {{0}};
        SerializedPass pass = {0};
        SerializedProgramParameters sibling = f.parameters;
        SerializedConstantBuffer sibling_buffer = f.buffer;
        SerializedVariable sibling_field = f.field;
        sibling.constant_buffers = &sibling_buffer;
        sibling_buffer.variables = &sibling_field;
        pass.has_serialized_platforms = true;
        pass.platforms = &platform;
        pass.platform_count = 1;
        pass.program_mask = 27;
        pass.subprograms[stage] = subprograms;
        pass.subprogram_count[stage] = 2;
        pass.subprogram_identities[stage] = identities;
        char *keyword = "SCALED";
        for (unsigned index = 0; index < 2; ++index) {
            subprograms[index].program_type = stage == 3 ? 21 : 18;
            subprograms[index].shader_requirements = 4;
            identities[index].inner_subprogram_index = (int)index;
            players[index].program_type = subprograms[index].program_type;
            players[index].has_player_blob_header = true;
        }
        subprograms[1].local_keywords = &keyword;
        subprograms[1].local_keyword_count = 1;
        players[1].local_keywords = &keyword;
        players[1].local_keyword_count = 1;
        if (scenario == 2) {
            f.buffer.var_count = 0;
            f.buffer.variables = NULL;
        }
        if (scenario == 4) {
            sibling_field = (SerializedVariable){.name = "_Sibling",
                                                 .layout = {4, 0, 0, 1, 0, 0}};
        }
        HLSLGlobalDeclarationWitness witnesses[] = {
            {0, &players[0], &f.parameters}, {1, &players[1], &sibling}};
        HLSLGlobalDeclarationUnion *declarations = NULL;
        HLSLGlobalDeclarationDiagnostic diagnostic;
        CHECK(hlsl_global_declarations_build(&pass, stage, 0, witnesses, 2,
                                             &declarations, &diagnostic) ==
              HLSL_GLOBAL_DECLARATIONS_OK);
        size_t field_count = 0;
        const HLSLGlobalDeclarationField *fields =
            hlsl_global_declarations_fields(declarations, &field_count);
        CHECK(field_count && fields[0].name != f.field.name);
        if (scenario == 3)
            f.field.name = "_ChangedAfterConstruction";
        CHECK(emit_with_union(&f, scenario == 0, &f.parameters, NULL,
                              declarations));
        hlsl_global_declarations_free(declarations);
        fixture_free(&f);
    }
    return true;
}

/* Release metadata can retain an empty loose shell and a complete named size
 * witness in the current variant, while the actual field lives in the common
 * partial declaration. Both inputs are required for this paired authority. */
static bool split_current_common_authority(void) {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        Fixture f;
        CHECK(fixture_init(&f, false, false));
        SerializedConstantBuffer current_buffers[2] = {
            {.name = "$Globals", .role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS},
            {.name = "$Globals", .role = SERIALIZED_CBUFFER_NAMED, .size = 16}};
        SerializedProgramParameters current = {
            .constant_buffers = current_buffers, .cb_count = 2};
        f.buffer.has_is_partial = f.buffer.is_partial = true;
        switch (scenario) {
        case 1:
            current_buffers[1].size = 32;
            break;
        case 2:
            current_buffers[1].role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
            break;
        case 3:
            current_buffers[1].name = "ForeignInputs";
            break;
        case 4:
            f.field.layout[0] = 4;
            break;
        case 5:
            current_buffers[1].has_is_partial = current_buffers[1].is_partial =
                true;
            break;
        default:
            break;
        }
        CHECK(emit(&f, scenario == 0, &current, &f.parameters));
        fixture_free(&f);
    }
    return true;
}

typedef struct {
    HLSLSourceQualityCBufferDeclarationKind rejected_kind;
    bool observed_rejection;
} RejectedDeclaration;

static bool
reject_declaration(void *opaque,
                   const HLSLSourceQualityObservation *observation) {
    RejectedDeclaration *rejection = opaque;
    if (observation->facts.cbuffer_declaration_kind != rejection->rejected_kind)
        return true;
    rejection->observed_rejection = true;
    return false;
}

static bool declaration_observer_rollback(void) {
    const HLSLSourceQualityCBufferDeclarationKind kinds[] = {
        HLSL_SOURCE_CBUFFER_BEGIN, HLSL_SOURCE_CBUFFER_FIELD,
        HLSL_SOURCE_CBUFFER_END};
    for (unsigned width = 0; width < 2; ++width) {
        for (unsigned index = 0; index < sizeof(kinds) / sizeof(kinds[0]);
             ++index) {
            Fixture f;
            CHECK(fixture_init_shape(&f, false, true, false, width != 0));
            RejectedDeclaration rejection = {.rejected_kind = kinds[index]};
            HLSLSourceQualityResult quality;
            HLSLExpressionSourceMap map;
            HLSLEmitDiagnostic diagnostic;
            HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
            options.source_quality = &quality;
            options.expression_source_map = &map;
            options.source_quality_observer = reject_declaration;
            options.source_quality_observer_context = &rejection;
            StringBuilder source;
            sb_init(&source);
            CHECK(!hlsl_emit_with_options_diagnostic(&f.program, &source,
                                                     &f.parameters, NULL, NULL,
                                                     &options, &diagnostic));
            CHECK(rejection.observed_rejection && source.failed &&
                  !map.complete && !map.count);
            CHECK(diagnostic.status == HLSL_EMIT_STATUS_ANALYSIS_FAILED &&
                  quality.classification == HLSL_SOURCE_QUALITY_FAILED);
            sb_free(&source);
            /* Reuse the caller's result/map storage after the failed
             * transaction; no rejected declaration ledger may poison the next
             * clean emission. */
            sb_init(&source);
            options.source_quality_observer = NULL;
            options.source_quality_observer_context = NULL;
            CHECK(hlsl_emit_with_options_diagnostic(&f.program, &source,
                                                    &f.parameters, NULL, NULL,
                                                    &options, &diagnostic));
            CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                  !quality.counts.unknown_provenance &&
                  !quality.counts.incomplete_units);
            CHECK(map.complete && hlsl_expression_source_map_matches(
                                      &map, &f.program, source.buf));
            sb_free(&source);
            fixture_free(&f);
        }
    }
    return true;
}

static bool control_point_reads_reject(void) {
    Fixture f;
    CHECK(fixture_init_scenario(&f, false, false, true));
    CHECK(emit(&f, true, &f.parameters, NULL));
    const DXBCOperand constant = f.program.instructions[4].operands[1];
    CHECK(constant.type == OPERAND_TYPE_CONSTANT_BUFFER);
    /* Same declared buffer and valid full-vector broadcast, but the actual
     * read belongs to the separately scoped control-point arithmetic phase. */
    f.program.instructions[1].operands[1] = constant;
    CHECK(emit(&f, false, &f.parameters, NULL));
    fixture_free(&f);
    return true;
}

int main(void) {
    if (!positive() || !float3_scalar_authority() ||
        !float3_scalar_name_collisions() || !negatives() ||
        !factor_minmax_and_lane_matrix() || !factor_index_rejections() ||
        !factor_modifiers_and_macro_rejections() ||
        !declaration_union_authority() || !split_current_common_authority() ||
        !declaration_observer_rollback() || !control_point_reads_reject())
        return 1;
    puts("HULL scalar cbuffer units passed");
    return 0;
}
