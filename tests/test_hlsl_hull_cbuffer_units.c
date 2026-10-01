// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "test_tessellation_fixture.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_global_declarations.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/hlsl_stage_coverage_internal.h"
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

/* Every positive owns a fresh complete authored document, rather than editing
 * an already accepted USIL model. Metadata is separately controlled API input. */
static bool fixture_decode(Fixture *f, uint8_t *bytes, size_t size, bool named) {
    CHECK(dxbc_compute_hash(bytes, size, bytes + 4));
    dxbc_document_init(&f->document);
    dxbc_stage_contract_init(&f->contract);
    DXBCDocumentDiagnostic dd;
    DXBCStageContractDiagnostic sd;
    const bool parsed = dxbc_document_parse(&f->document, bytes, size, &dd);
    free(bytes);
    CHECK(parsed);
    CHECK(dxbc_document_decode_semantic(&f->document, &f->semantic));
    CHECK(dxbc_stage_contract_decode(&f->document, &f->semantic, &f->contract,
                                     &sd));
    CHECK(usil_translate_with_stage_contract(&f->program, &f->semantic,
                                            &f->contract));
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
    return fixture_decode(f, bytes, new_size, named);
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

typedef enum {
    CLAMP_VALID = 0,
    CLAMP_WRONG_LITERAL,
    CLAMP_NEGATIVE_ZERO_LITERAL,
    CLAMP_SWAPPED_OPERANDS,
    CLAMP_MAX_OPCODE,
    CLAMP_EXTRA_CONSUMER,
    CLAMP_ARITHMETIC_CONSUMER,
    CLAMP_FOREIGN_OUTPUT,
    CLAMP_CROSS_PHASE,
    CLAMP_PRECISE,
    CLAMP_SATURATE,
    CLAMP_MODIFIED_VALUE
} ClampCase;

typedef struct {
    uint32_t maximum_bits;
    bool scalar_cbuffer;
    bool temporary_result;
    bool temporary_value;
    unsigned result_lane, value_lane;
    ClampCase scenario;
} ClampGrammar;

/* The clamp is authored into the complete token grammar. The target operations
 * and output consumer are independent inputs to the inverse emitter. */
static bool fixture_init_clamp(Fixture *f, bool inner_first, bool float3,
                               const ClampGrammar *grammar) {
    memset(f, 0, sizeof(*f));
    size_t size = 0;
    uint8_t *base = float3 ? test_tessellation_hull_float3_dxbc(
                                 3, 3, inner_first ? 3 : 0, "POINTVALUE", &size)
                           : test_tessellation_hull_dxbc(
                                 3, 3, inner_first ? 3 : 0, &size);
    CHECK(base);
    const uint32_t offset = read_u32(base + 44);
    CHECK(offset + 16 <= size && !memcmp(base + offset, "SHEX", 4));
    uint32_t words[256];
    size_t written = 0;
    unsigned factors = 0;
    bool declared = false;
    for (size_t byte = offset + 16; byte < size;) {
        const uint32_t token = read_u32(base + byte);
        const unsigned length = token >> 24;
        const unsigned opcode = token & 0x7ffu;
        CHECK(length && byte + length * 4 <= size && written + 64 < 256);
        if (opcode == 115 && !declared && grammar->scalar_cbuffer) {
            words[written++] = WORD(89, 4);
            words[written++] = 0x00208000;
            words[written++] = 0;
            words[written++] = 1;
            declared = true;
        }
        if (opcode == 152) {
            CHECK(length == 2);
            words[written++] = token;
            words[written++] = grammar->maximum_bits;
        } else if (opcode == 104) {
            words[written++] = token;
            words[written++] = 4;
        } else if (opcode == 54 && length >= 5 &&
                   ((read_u32(base + byte + 4) & 0x000ff000u) == 0x00002000u)) {
            const bool inner = read_u32(base + byte + 4) == 0x00102012;
            const bool temporary = grammar->temporary_result ||
                grammar->scenario == CLAMP_EXTRA_CONSUMER ||
                grammar->scenario == CLAMP_ARITHMETIC_CONSUMER ||
                grammar->scenario == CLAMP_CROSS_PHASE;
            if (inner && (temporary || grammar->temporary_value)) {
                words[written++] = WORD(104, 2);
                words[written++] = 4;
            }
            uint32_t value[4] = {grammar->scalar_cbuffer ? 0x0020800a : 0x00004001,
                                 grammar->scalar_cbuffer ? 0 : 0x40400000, 0, 0};
            size_t value_count = grammar->scalar_cbuffer ? 3 : 2;
            if (grammar->temporary_value) {
                words[written++] = WORD(54, 3 + value_count);
                words[written++] = 0x00100002u | (1u << grammar->value_lane) << 4;
                words[written++] = 3;
                memcpy(words + written, value, value_count * sizeof(*value));
                written += value_count;
                value[0] = 0x0010000au | grammar->value_lane << 4;
                value[1] = 3;
                value_count = 2;
            }
            if (grammar->scenario == CLAMP_MODIFIED_VALUE) {
                memmove(value + 2, value + 1, (value_count - 1) * sizeof(*value));
                value[0] |= 0x80000000u;
                value[1] = 1u | 1u << 6; /* Extended NEG source modifier. */
                ++value_count;
            }
            uint32_t maximum[] = {
                0x00004001,
                grammar->scenario == CLAMP_WRONG_LITERAL ? grammar->maximum_bits + 1
                : grammar->scenario == CLAMP_NEGATIVE_ZERO_LITERAL ? 0x80000000u
                                                                  : grammar->maximum_bits};
            if (!(inner && grammar->scenario == CLAMP_CROSS_PHASE)) {
                const unsigned operation = grammar->scenario == CLAMP_MAX_OPCODE ? 52 : 51;
                const unsigned destination_count = temporary ? 2 : length - 3;
                uint32_t instruction = WORD(operation, 1 + destination_count + value_count + 2);
                if (grammar->scenario == CLAMP_PRECISE) instruction |= 1u << 19;
                if (grammar->scenario == CLAMP_SATURATE) instruction |= 1u << 13;
                words[written++] = instruction;
                if (temporary) {
                    words[written++] = 0x00100002u | (1u << grammar->result_lane) << 4;
                    words[written++] = 1;
                } else {
                    for (unsigned word = 1; word < length - 2; ++word)
                        words[written++] = read_u32(base + byte + word * 4);
                }
                const bool swapped = grammar->scenario == CLAMP_SWAPPED_OPERANDS;
                memcpy(words + written, swapped ? maximum : value,
                       (swapped ? 2 : value_count) * sizeof(*words));
                written += swapped ? 2 : value_count;
                memcpy(words + written, swapped ? value : maximum,
                       (swapped ? value_count : 2) * sizeof(*words));
                written += swapped ? value_count : 2;
            }
            if (grammar->scenario == CLAMP_EXTRA_CONSUMER) {
                const uint32_t consumer[] = {WORD(54, 5), 0x00100012, 2,
                                             0x0010000a, 1};
                memcpy(words + written, consumer, sizeof(consumer));
                written += sizeof(consumer) / sizeof(*consumer);
            }
            if (!inner && grammar->scenario == CLAMP_FOREIGN_OUTPUT) {
                const uint32_t foreign[] = {WORD(54, 5), 0x00102012,
                    inner_first ? 0 : 3, 0x00004001, 0x3f800000};
                memcpy(words + written, foreign, sizeof(foreign));
                written += sizeof(foreign) / sizeof(*foreign);
            }
            if (temporary) {
                const bool arithmetic = grammar->scenario == CLAMP_ARITHMETIC_CONSUMER;
                words[written++] = WORD(arithmetic ? 0 : 54, length + (arithmetic ? 2 : 0));
                for (unsigned word = 1; word < length - 2; ++word)
                    words[written++] = read_u32(base + byte + word * 4);
                words[written++] = 0x0010000au | grammar->result_lane << 4;
                words[written++] = 1;
                if (arithmetic) {
                    words[written++] = 0x00004001;
                    words[written++] = 0x3f800000;
                }
            }
            ++factors;
        } else {
            for (unsigned word = 0; word < length; ++word)
                words[written++] = read_u32(base + byte + word * 4);
        }
        byte += length * 4;
    }
    CHECK(factors == 2 && (declared || !grammar->scalar_cbuffer));
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
    return fixture_decode(f, bytes, new_size, true);
}

static void fixture_free(Fixture *f) {
    usil_free(&f->program);
    dxbc_free(&f->semantic);
    dxbc_stage_contract_free(&f->contract);
    dxbc_document_free(&f->document);
}

static bool scalar_declaration_matches(const Fixture *f, const char *source) {
    char expected[256];
    const bool named = strcmp(f->buffer.name, "$Globals") != 0;
    const int length = named
        ? snprintf(expected, sizeof(expected),
                   "cbuffer %s : register(b0) {\n    float %s;\n};\n",
                   f->buffer.name, f->field.name)
        : snprintf(expected, sizeof(expected), "float %s : register(c0);",
                   f->field.name);
    CHECK(length > 0 && (size_t)length < sizeof(expected));
    CHECK(strstr(source, expected) && !strstr(source, "packoffset(") &&
          !strstr(source, "_pad") && !strstr(source, "cb0_"));
    return true;
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
              quality.counts.unknown_provenance == 0 &&
              quality.counts.residual_total == 0);
        CHECK(scalar_declaration_matches(f, source.buf));
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

static bool check_clamp_origins(const Fixture *f, const ClampGrammar *grammar,
                                const HLSLExpressionSourceMap *map,
                                const char *source, bool lowered) {
    unsigned minima = 0, lowering_owners = 0;
    uint32_t declaration_owner = UINT32_MAX;
    for (int index = 0; index < f->semantic.instruction_count; ++index) {
        const DXBCInstruction *instruction = &f->semantic.instructions[index];
        if (instruction->opcode != 152) continue;
        CHECK(declaration_owner == UINT32_MAX &&
              instruction->has_raw_instruction_index);
        declaration_owner = instruction->raw_instruction_index;
    }
    CHECK(declaration_owner != UINT32_MAX &&
          f->program.tessellation.max_tessellation_factor_bits == grammar->maximum_bits &&
          f->program.tessellation.max_tessellation_factor_source_instruction_index ==
              declaration_owner);
    for (int index = 0; index < f->program.instruction_count; ++index) {
        const USILInstruction *instruction = &f->program.instructions[index];
        const HLSLExpressionOrigin *origin = &map->origins[index];
        if (origin->kind == HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP)
            ++lowering_owners;
        if (instruction->opcode != USIL_OP_MIN) continue;
        ++minima;
        if (!lowered) {
            CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION);
            continue;
        }
        CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP &&
              origin->instruction_index == index &&
              origin->source_instruction_index == instruction->source_instruction_index &&
              origin->destination_lanes == (grammar->temporary_result ? 1u << grammar->result_lane : 1u) &&
              hlsl_expression_origin_has_span(origin->kind) &&
              hlsl_expression_origin_ranges_valid(origin, strlen(source)));
        CHECK(origin->hull_factor_clamp.maximum_bits == grammar->maximum_bits &&
              origin->hull_factor_clamp.maximum_source_instruction_index == declaration_owner &&
              origin->hull_factor_clamp.value_operand_index == 1 &&
              origin->hull_factor_clamp.maximum_operand_index == 2 &&
              origin->hull_factor_clamp.value_source_component ==
                  (grammar->temporary_value ? grammar->value_lane : 0));
        const int phase = origin->hull_factor_clamp.phase_index;
        CHECK(phase >= 0 && (size_t)phase < f->program.tessellation.phase_count);
        const USILHullPhase *scope = &f->program.tessellation.phases[phase];
        CHECK(scope->kind == DXBC_HULL_PHASE_FORK && index >= scope->first_instruction_index &&
              index < scope->end_instruction_index &&
              origin->hull_factor_clamp.phase_marker_source_instruction_index ==
                  scope->marker_source_instruction_index);
        const int output = origin->hull_factor_clamp.output_instruction_index;
        CHECK(output == index + (grammar->temporary_result ? 1 : 0) &&
              output < scope->end_instruction_index &&
              f->program.instructions[output].operands[0].type == OPERAND_TYPE_OUTPUT &&
              origin->hull_factor_clamp.output_source_instruction_index ==
                  f->program.instructions[output].source_instruction_index);
        if (grammar->temporary_value) {
            const int definition = origin->hull_factor_clamp.value_definition_instruction_index;
            CHECK(definition == index - 1 && definition >= scope->first_instruction_index &&
                  f->program.instructions[definition].opcode == USIL_OP_MOV &&
                  f->program.instructions[definition].operands[0].register_index == 3);
        } else
            CHECK(origin->hull_factor_clamp.value_definition_instruction_index == -1);
        static const uint8_t zero_digest[32] = {0};
        CHECK(memcmp(origin->hull_factor_clamp.decoded_owner_digest,
                     zero_digest, sizeof(zero_digest)));
        const char *assignment = source + origin->source_begin;
        CHECK(origin->source_end - origin->source_begin >= strlen("factors.") &&
              !memcmp(assignment, "factors.", strlen("factors.")) &&
              origin->definition_begin > origin->source_end &&
              origin->definition_end - origin->definition_begin >= strlen("[maxtessfactor(") &&
              !memcmp(source + origin->definition_begin, "[maxtessfactor(",
                      strlen("[maxtessfactor(")) &&
              source[origin->definition_end - 1] == ']');
    }
    CHECK(lowering_owners == (lowered ? 2u : 0u));
    CHECK(minima == (grammar->scenario == CLAMP_MAX_OPCODE ? 0u : 2u));
    return true;
}

static bool emit_clamp_fixture(Fixture *f, const ClampGrammar *grammar,
                               bool expected, bool lowered,
                               HLSLExpressionSourceMap *retained_map,
                               StringBuilder *retained_source) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap local_map;
    HLSLExpressionSourceMap *map = retained_map ? retained_map : &local_map;
    HLSLEmitDiagnostic diagnostic;
    options.source_quality = &quality;
    options.expression_source_map = map;
    StringBuilder local_source;
    StringBuilder *source = retained_source ? retained_source : &local_source;
    sb_init(source);
    const bool emitted = hlsl_emit_with_options_diagnostic(
        &f->program, source, grammar->scalar_cbuffer ? &f->parameters : NULL,
        NULL, NULL, &options, &diagnostic);
    if (emitted != expected)
        fprintf(stderr, "HULL clamp case=%u cb=%u result=%u value=%u max=%08x "
                        "emitted=%u reason=%s instruction=%d\n",
                grammar->scenario, grammar->scalar_cbuffer,
                grammar->temporary_result, grammar->temporary_value,
                grammar->maximum_bits, emitted,
                hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index);
    CHECK(emitted == expected);
    if (expected) {
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !quality.counts.unknown_provenance && !quality.counts.incomplete_units &&
              hlsl_expression_source_map_matches(map, &f->program, source->buf));
        CHECK(check_clamp_origins(f, grammar, map, source->buf, lowered));
        if (lowered)
            CHECK(!strstr(source->buf, "min(") && !strstr(source->buf, "max(") &&
                  strstr(source->buf, "factors.outer[factorIndex]") &&
                  strstr(source->buf, "factors.inner"));
        else {
            const char *operation = grammar->scenario == CLAMP_MAX_OPCODE ? "max(" : "min(";
            const char *first = strstr(source->buf, operation);
            CHECK(first && strstr(first + strlen(operation), operation));
        }
    } else
        CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK &&
              quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
              !map->complete && !map->count);
    if (!retained_source) sb_free(source);
    return true;
}

static bool final_clamp_parsed_matrix(void) {
    const struct {
        uint32_t bits;
        const char *attribute;
    } maxima[] = {
        {0x3f800000, "[maxtessfactor(1.0f)]"},
        {0x3fc00000, "[maxtessfactor(1.5f)]"},
        {0x41800000, "[maxtessfactor(16.0f)]"},
        {0x42000000, "[maxtessfactor(32.0f)]"},
        {0x42800000, "[maxtessfactor(64.0f)]"}};
    /* Case zero is direct. Cases one through four select each physical TEMP
     * lane independently for the value and the final clamp result. */
    for (unsigned maximum = 0; maximum < sizeof(maxima) / sizeof(maxima[0]); ++maximum)
        for (unsigned cbuffer = 0; cbuffer < 2; ++cbuffer)
            for (unsigned width = 0; width < 2; ++width)
                for (unsigned order = 0; order < 2; ++order)
                    for (unsigned result = 0; result < 5; ++result)
                        for (unsigned value = 0; value < 5; ++value) {
                            const ClampGrammar grammar = {
                                .maximum_bits = maxima[maximum].bits,
                                .scalar_cbuffer = cbuffer != 0,
                                .temporary_result = result != 0,
                                .temporary_value = value != 0,
                                .result_lane = result ? result - 1 : 0,
                                .value_lane = value ? value - 1 : 0};
                            Fixture f;
                            StringBuilder source;
                            CHECK(fixture_init_clamp(&f, order != 0, width != 0, &grammar));
                            CHECK(emit_clamp_fixture(&f, &grammar, true, true, NULL, &source));
                            CHECK(strstr(source.buf, maxima[maximum].attribute));
                            CHECK(!strstr(source.buf, "cb0") && !strstr(source.buf, "r0") &&
                                  !strstr(source.buf, "r1") && !strstr(source.buf, ".xxxx"));
                            sb_free(&source);
                            fixture_free(&f);
                        }
    return true;
}

static bool final_clamp_parsed_rejections(void) {
    for (unsigned cbuffer = 0; cbuffer < 2; ++cbuffer)
        for (unsigned width = 0; width < 2; ++width)
            for (unsigned order = 0; order < 2; ++order)
                for (ClampCase scenario = CLAMP_WRONG_LITERAL; scenario <= CLAMP_MODIFIED_VALUE; ++scenario) {
                    const ClampGrammar grammar = {.maximum_bits = 0x42000000,
                        .scalar_cbuffer = cbuffer != 0, .scenario = scenario};
                    Fixture f;
                    CHECK(fixture_init_clamp(&f, order != 0, width != 0, &grammar));
                    const bool expected = scenario <= CLAMP_ARITHMETIC_CONSUMER ||
                        (scenario == CLAMP_MODIFIED_VALUE && !grammar.scalar_cbuffer);
                    CHECK(emit_clamp_fixture(&f, &grammar, expected, false, NULL, NULL));
                    fixture_free(&f);
                }
    return true;
}

static bool invalid_clamp_maximum_declarations(void) {
    const uint32_t invalid[] = {0, 0x80000000u, 0x3f000000, 0x42800001,
                                0x7f800000, 0x7fc00001};
    for (unsigned value = 0; value < sizeof(invalid) / sizeof(invalid[0]); ++value) {
        size_t size = 0;
        uint8_t *bytes = test_tessellation_hull_dxbc(3, 3, 0, &size);
        CHECK(bytes);
        const uint32_t offset = read_u32(bytes + 44);
        bool replaced = false;
        for (size_t byte = offset + 16; byte < size;) {
            const uint32_t token = read_u32(bytes + byte);
            const unsigned length = token >> 24;
            CHECK(length && byte + length * 4 <= size);
            if ((token & 0x7ffu) == 152) {
                CHECK(length == 2 && !replaced);
                write_u32(bytes + byte + 4, invalid[value]);
                replaced = true;
            }
            byte += length * 4;
        }
        CHECK(replaced && dxbc_compute_hash(bytes, size, bytes + 4));
        Fixture f = {0};
        dxbc_document_init(&f.document);
        dxbc_stage_contract_init(&f.contract);
        DXBCDocumentDiagnostic dd;
        DXBCStageContractDiagnostic sd;
        CHECK(dxbc_document_parse(&f.document, bytes, size, &dd));
        free(bytes);
        CHECK(dxbc_document_decode_semantic(&f.document, &f.semantic));
        CHECK(!dxbc_stage_contract_decode(&f.document, &f.semantic, &f.contract, &sd));
        CHECK(sd.status == DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE);
        fixture_free(&f);
    }
    return true;
}

static bool clamp_map_replay_and_rebasing(void) {
    const ClampGrammar grammar = {.maximum_bits = 0x42000000,
        .scalar_cbuffer = true, .temporary_result = true};
    Fixture f;
    HLSLExpressionSourceMap original, changed;
    StringBuilder source;
    CHECK(fixture_init_clamp(&f, false, true, &grammar));
    CHECK(emit_clamp_fixture(&f, &grammar, true, true, &original, &source));
    int minimum = -1;
    for (int index = 0; index < f.program.instruction_count; ++index)
        if (f.program.instructions[index].opcode == USIL_OP_MIN) {
            minimum = index;
            break;
        }
    CHECK(minimum >= 0);
    int neighboring_minimum = -1;
    for (int index = minimum + 1; index < f.program.instruction_count; ++index)
        if (f.program.instructions[index].opcode == USIL_OP_MIN) {
            neighboring_minimum = index;
            break;
        }
    CHECK(neighboring_minimum >= 0);
    for (unsigned mutation = 0; mutation < 19; ++mutation) {
        changed = original;
        HLSLExpressionOrigin *origin = &changed.origins[minimum];
        switch (mutation) {
        case 0: origin->kind = HLSL_EXPRESSION_ORIGIN_DEAD; break;
        case 1: --origin->instruction_index; break;
        case 2: ++origin->source_instruction_index; break;
        case 3: origin->destination_lanes = 2; break;
        case 4: origin->hull_factor_clamp.maximum_bits ^= 1; break;
        case 5: ++origin->hull_factor_clamp.maximum_source_instruction_index; break;
        case 6: origin->hull_factor_clamp.phase_index = 1; break;
        case 7: ++origin->hull_factor_clamp.phase_marker_source_instruction_index; break;
        case 8: ++origin->hull_factor_clamp.output_instruction_index; break;
        case 9: ++origin->hull_factor_clamp.output_source_instruction_index; break;
        case 10: origin->hull_factor_clamp.value_operand_index = 2; break;
        case 11: origin->hull_factor_clamp.maximum_operand_index = 1; break;
        case 12: origin->hull_factor_clamp.value_source_component = 1; break;
        case 13: origin->hull_factor_clamp.value_definition_instruction_index = 0; break;
        case 14: origin->hull_factor_clamp.decoded_owner_digest[0] ^= 1; break;
        case 15: origin->definition_end = origin->definition_begin; break;
        case 16: origin->source_end = strlen(source.buf) + 1; break;
        case 17:
            origin->kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
            origin->definition_begin = origin->definition_end = 0;
            memset(&origin->hull_factor_clamp, 0, sizeof(origin->hull_factor_clamp));
            break;
        case 18:
            origin->source_begin = original.origins[neighboring_minimum].source_begin;
            origin->source_end = original.origins[neighboring_minimum].source_end;
            break;
        }
        CHECK(!hlsl_expression_origins_equal(origin, &original.origins[minimum]));
        CHECK(!hlsl_expression_source_map_matches(&changed, &f.program, source.buf));
    }
    changed = original;
    CHECK(hlsl_expression_origins_equal(&changed.origins[minimum],
                                        &original.origins[minimum]));
    /* Retain the source and ledger while independently changing decoded owners.
     * Changes that leave ordinary syntax valid still invalidate this lowering. */
    USILInstruction *producer = &f.program.instructions[minimum];
    const uint32_t bits = producer->operands[2].imm_values[0];
    producer->operands[2].imm_values[0] ^= 1;
    CHECK(!hlsl_expression_source_map_matches(&original, &f.program, source.buf));
    producer->operands[2].imm_values[0] = bits;
    const uint32_t raw = producer->operands[2].immediate_words[0];
    producer->operands[2].immediate_words[0] ^= 1;
    CHECK(!hlsl_expression_source_map_matches(&original, &f.program, source.buf));
    producer->operands[2].immediate_words[0] = raw;
    const uint32_t declaration = f.program.tessellation.max_tessellation_factor_source_instruction_index;
    ++f.program.tessellation.max_tessellation_factor_source_instruction_index;
    CHECK(!hlsl_expression_source_map_matches(&original, &f.program, source.buf));
    f.program.tessellation.max_tessellation_factor_source_instruction_index = declaration;
    const int output = original.origins[minimum].hull_factor_clamp.output_instruction_index;
    USILInstruction *consumer = &f.program.instructions[output];
    const USILOpcode opcode = consumer->opcode;
    consumer->opcode = USIL_OP_ADD;
    CHECK(!hlsl_expression_source_map_matches(&original, &f.program, source.buf));
    consumer->opcode = opcode;
    const uint32_t source_register = consumer->operands[1].register_index;
    consumer->operands[1].register_index = 2;
    CHECK(!hlsl_expression_source_map_matches(&original, &f.program, source.buf));
    consumer->operands[1].register_index = (int)source_register;
    CHECK(hlsl_expression_source_map_matches(&original, &f.program, source.buf));
    const size_t literal = original.origins[minimum].definition_begin + strlen("[maxtessfactor(");
    CHECK(source.buf[literal] == '3');
    source.buf[literal] = '1';
    CHECK(!hlsl_expression_source_map_matches(&original, &f.program, source.buf));
    source.buf[literal] = '3';
    CHECK(hlsl_expression_source_map_matches(&original, &f.program, source.buf));

    StringBuilder prefixed;
    sb_init(&prefixed);
    sb_append(&prefixed, "prefix\n");
    const size_t prefix_length = prefixed.len;
    sb_append(&prefixed, source.buf);
    changed = original;
    CHECK(hlsl_expression_source_map_offset(&changed, prefix_length));
    CHECK(hlsl_expression_source_map_matches(&changed, &f.program, prefixed.buf));
    CHECK(changed.origins[minimum].source_begin == original.origins[minimum].source_begin + prefix_length &&
          changed.origins[minimum].definition_begin == original.origins[minimum].definition_begin + prefix_length &&
          changed.origins[minimum].definition_end == original.origins[minimum].definition_end + prefix_length);
    sb_free(&prefixed);

    StringBuilder indented;
    sb_init(&indented);
    changed = original;
    for (size_t begin = 0; begin < source.len;) {
        const char *newline = strchr(source.buf + begin, '\n');
        const size_t end = newline ? (size_t)(newline - source.buf) + 1 : source.len;
        sb_append(&indented, "  ");
        const size_t output_begin = indented.len;
        sb_append_len(&indented, source.buf + begin, end - begin);
        CHECK(hlsl_expression_source_map_rebase_line(&changed, &original,
                                                      begin, end, output_begin));
        begin = end;
    }
    CHECK(hlsl_expression_source_map_matches(&changed, &f.program, indented.buf));
    CHECK(changed.origins[minimum].source_begin > original.origins[minimum].source_begin &&
          changed.origins[minimum].definition_begin > original.origins[minimum].definition_begin &&
          changed.origins[minimum].definition_end > original.origins[minimum].definition_end);
    sb_free(&indented);
    sb_free(&source);
    fixture_free(&f);
    return true;
}

typedef struct {
    Fixture *fixture;
    SerializedConstantBuffer *current_shell;
    SerializedResourceParam *current_binding;
    int minimum, output, phase;
    unsigned mutation;
    bool fired;
} ClampObserver;

static bool observe_clamp_mutation(void *opaque,
                                   const HLSLSourceQualityObservation *observation) {
    ClampObserver *observer = opaque;
    if (observer->fired || observation->unit_kind != HLSL_SOURCE_UNIT_HELPER ||
        observation->facts.instruction_index != observer->minimum)
        return true;
    observer->fired = true;
    Fixture *f = observer->fixture;
    USILInstruction *minimum = &f->program.instructions[observer->minimum];
    USILInstruction *output = &f->program.instructions[observer->output];
    switch (observer->mutation) {
    case 0: return false;
    case 1: f->program.tessellation.max_tessellation_factor_bits ^= 1; break;
    case 2: ++f->program.tessellation.max_tessellation_factor_source_instruction_index; break;
    case 3: minimum->operands[2].imm_values[0] ^= 1; break;
    case 4: minimum->operands[2].immediate_words[0] ^= 1; break;
    case 5: output->opcode = USIL_OP_MAX; break;
    case 6:
        /* Keep the two decoded index projections mutually consistent while
         * changing the consumer to an independently undefined register. */
        output->operands[1].register_index = 2;
        output->operands[1].index_values[0] = 2;
        break;
    case 7: ++minimum->source_instruction_index; break;
    case 8: ++f->program.tessellation.phases[observer->phase].instance_count; break;
    case 9: f->field.layout[0] = 4; break;
    case 10: f->field.name = "_ChangedFactor"; break;
    case 11:
        if (observer->current_binding) observer->current_binding->bind_index = 1;
        else f->binding.bind_index = 1;
        break;
    case 12:
        if (observer->current_shell) observer->current_shell->size = 32;
        else f->buffer.size = 32;
        break;
    }
    return true;
}

static bool clamp_observer_rollback(void) {
    /* Observers promise not to mutate the emitter. These deliberate violations
     * exercise the frozen typed owner and failure transaction defensively. */
    for (unsigned authority = 0; authority < 3; ++authority) {
        for (unsigned width = 0; width < 2; ++width) {
            for (unsigned mutation = 0; mutation < 13; ++mutation) {
                const ClampGrammar grammar = {.maximum_bits = 0x42000000,
                    .scalar_cbuffer = true, .temporary_result = true};
                Fixture f;
                CHECK(fixture_init_clamp(&f, false, width != 0, &grammar));
                SerializedConstantBuffer shell = {.name = f.buffer.name,
                    .role = SERIALIZED_CBUFFER_NAMED, .size = 16};
                SerializedResourceParam shell_binding = f.binding;
                SerializedProgramParameters current = {.constant_buffers = &shell,
                    .cb_count = 1, .resources = &shell_binding, .res_count = 1};
                if (authority == 2) f.buffer.has_is_partial = f.buffer.is_partial = true;
                const SerializedProgramParameters *current_parameters =
                    authority == 2 ? &current : authority == 0 ? &f.parameters : NULL;
                const SerializedProgramParameters *common_parameters =
                    authority ? &f.parameters : NULL;
                int minimum = -1;
                for (int index = 0; index < f.program.instruction_count; ++index)
                    if (f.program.instructions[index].opcode == USIL_OP_MIN) {
                        minimum = index;
                        break;
                    }
                CHECK(minimum >= 0 && minimum + 1 < f.program.instruction_count &&
                      f.program.instructions[minimum + 1].opcode == USIL_OP_MOV);
                const USILInstruction saved_minimum = f.program.instructions[minimum];
                const USILInstruction saved_output = f.program.instructions[minimum + 1];
                const USILTessellationContract saved_tessellation = f.program.tessellation;
                const USILHullPhase saved_phase = f.program.tessellation.phases[0];
                const SerializedVariable saved_field = f.field;
                const SerializedResourceParam saved_binding = f.binding;
                const SerializedConstantBuffer saved_buffer = f.buffer;
                const SerializedConstantBuffer saved_shell = shell;
                const SerializedResourceParam saved_shell_binding = shell_binding;
                ClampObserver observer = {.fixture = &f, .minimum = minimum,
                    .output = minimum + 1, .phase = 0, .mutation = mutation,
                    .current_shell = authority == 2 ? &shell : NULL,
                    .current_binding = authority == 2 ? &shell_binding : NULL};
                HLSLSourceQualityResult quality;
                HLSLExpressionSourceMap map;
                HLSLEmitDiagnostic diagnostic;
                HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
                options.source_quality = &quality;
                options.expression_source_map = &map;
                options.source_quality_observer = observe_clamp_mutation;
                options.source_quality_observer_context = &observer;
                StringBuilder source;
                sb_init(&source);
                CHECK(!hlsl_emit_with_options_diagnostic(&f.program, &source,
                                                         current_parameters, common_parameters, NULL,
                                                         &options, &diagnostic));
                CHECK(observer.fired && source.failed && !map.complete && !map.count &&
                      diagnostic.status != HLSL_EMIT_STATUS_OK &&
                      quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
                if (!mutation)
                    CHECK(diagnostic.status == HLSL_EMIT_STATUS_ANALYSIS_FAILED &&
                          quality.classification == HLSL_SOURCE_QUALITY_FAILED);
                sb_free(&source);
                f.program.instructions[minimum] = saved_minimum;
                f.program.instructions[minimum + 1] = saved_output;
                f.program.tessellation = saved_tessellation;
                f.program.tessellation.phases[0] = saved_phase;
                f.field = saved_field;
                f.binding = saved_binding;
                f.buffer = saved_buffer;
                shell = saved_shell;
                shell_binding = saved_shell_binding;
                options.source_quality_observer = NULL;
                options.source_quality_observer_context = NULL;
                sb_init(&source);
                CHECK(hlsl_emit_with_options_diagnostic(&f.program, &source,
                                                        current_parameters, common_parameters, NULL,
                                                        &options, &diagnostic));
                CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                      map.complete && hlsl_expression_source_map_matches(
                                          &map, &f.program, source.buf) &&
                      check_clamp_origins(&f, &grammar, &map, source.buf, true));
                sb_free(&source);
                if (authority == 2 && mutation == 0)
                    CHECK(emit(&f, false, NULL, &f.parameters));
                fixture_free(&f);
            }
        }
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
        /* The sole byte-zero scalar retains its named b0 shell. The 16-byte
         * cbuffer rounding needs no qualifier or anonymous tail. */
        length = snprintf(expected, sizeof(expected),
                          "    float %s;\n",
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
static bool scalar_packing_capture_parity(void) {
    /* Current-only, common-only and split shell/partial-field authority all
     * traverse the normal producer before the independent owned captures. */
    for (unsigned float3 = 0; float3 < 2; ++float3) {
        for (unsigned authority = 0; authority < 3; ++authority) {
            for (unsigned renamed = 0; renamed < 2; ++renamed) {
                Fixture f;
                CHECK(fixture_init_shape(&f, authority % 2 != 0, true, false,
                                         float3 != 0));
                if (renamed) {
                    f.field.name = "PatchScale";
                    f.buffer.name = f.binding.name = "PatchInputs";
                }
                SerializedConstantBuffer shells[2] = {
                    {.name = "$Globals", .role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS},
                    {.name = f.buffer.name, .role = SERIALIZED_CBUFFER_NAMED, .size = 16}};
                SerializedProgramParameters current_shell = {
                    .constant_buffers = shells, .cb_count = 2};
                const SerializedProgramParameters *current = authority == 1 ? NULL : &f.parameters;
                const SerializedProgramParameters *common = authority == 0 ? NULL : &f.parameters;
                if (authority == 2) {
                    f.buffer.has_is_partial = f.buffer.is_partial = true;
                    current = &current_shell;
                }
                HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
                HLSLSourceQualityResult normal_quality, captured_quality, independent_quality;
                HLSLExpressionSourceMap normal_map = {0}, captured_map = {0};
                HLSLEmitDiagnostic diagnostic;
                StringBuilder normal, captured, independent;
                sb_init(&normal); sb_init(&captured); sb_init(&independent);
                options.source_quality = &normal_quality;
                options.expression_source_map = &normal_map;
                CHECK(hlsl_emit_with_options_diagnostic(&f.program, &normal, current,
                    common, NULL, &options, &diagnostic));
                CHECK(scalar_declaration_matches(&f, normal.buf));
                CHECK(normal_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                      !normal_quality.counts.residual_total && !normal_quality.counts.unknown_provenance &&
                      !normal_quality.counts.incomplete_units && normal_quality.counts.inspected_units == 3 &&
                      normal_quality.counts.cbuffer_declarations == 1 && normal_quality.counts.cbuffer_fields == 1);
                HLSLStageCoverage coverage = {0}, other = {0};
                options.source_quality = &captured_quality;
                options.expression_source_map = &captured_map;
                CHECK(hlsl_emit_with_stage_coverage(&f.program, &captured, current,
                    common, NULL, &options, &coverage, &diagnostic));
                CHECK(normal.len == captured.len && !memcmp(normal.buf, captured.buf, normal.len + 1));
                CHECK(hlsl_source_quality_results_equal(&normal_quality, &captured_quality));
                CHECK(normal_map.complete && captured_map.complete && normal_map.count == captured_map.count);
                for (size_t origin = 0; origin < normal_map.count; ++origin)
                    CHECK(hlsl_expression_origins_equal(&normal_map.origins[origin], &captured_map.origins[origin]));
                unsigned declarations = 0;
                for (size_t event = 0; event < coverage.syntax_count; ++event) {
                    const HLSLSourceQualityFacts *facts = &coverage.syntax[event].facts;
                    if (facts->cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_NONE) continue;
                    CHECK(facts->known && !facts->artifacts && !facts->cbuffer_binding_register &&
                          !facts->cbuffer_byte_offset && facts->cbuffer_declaration_authority ==
                              (facts->cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_FIELD ?
                               (authority == 0 ? 1 : 2) : (authority == 1 ? 2 : 1)));
                    CHECK(facts->cbuffer_byte_size ==
                        (facts->cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_FIELD ? 4u : 16u));
                    ++declarations;
                }
                CHECK(declarations == 3 && coverage.unit_count == 3 &&
                      (coverage.obligations & HLSL_STAGE_COVERAGE_BODY) &&
                      (coverage.obligations & HLSL_STAGE_COVERAGE_LOCAL_DECLARATION));
                options.source_quality = &independent_quality;
                options.expression_source_map = NULL;
                CHECK(hlsl_emit_with_stage_coverage(&f.program, &independent, current,
                    common, NULL, &options, &other, &diagnostic));
                CHECK(independent.len == normal.len && !memcmp(independent.buf, normal.buf, normal.len + 1) &&
                      hlsl_source_quality_results_equal(&captured_quality, &independent_quality) &&
                      hlsl_stage_coverage_equal(&coverage, &other));
                fixture_free(&f);
                CHECK(hlsl_stage_coverage_validate(&coverage, &captured) &&
                      hlsl_stage_coverage_validate(&other, &independent));
                hlsl_stage_coverage_dispose(&other); hlsl_stage_coverage_dispose(&coverage);
                sb_free(&independent); sb_free(&captured); sb_free(&normal);
            }
        }
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
        !float3_scalar_name_collisions() || !scalar_packing_capture_parity() || !negatives() ||
        !factor_minmax_and_lane_matrix() || !factor_index_rejections() ||
        !final_clamp_parsed_matrix() || !final_clamp_parsed_rejections() ||
        !invalid_clamp_maximum_declarations() ||
        !clamp_map_replay_and_rebasing() ||
        !clamp_observer_rollback() ||
        !factor_modifiers_and_macro_rejections() ||
        !declaration_union_authority() || !split_current_common_authority() ||
        !declaration_observer_rollback() || !control_point_reads_reject())
        return 1;
    puts("HULL scalar cbuffer units passed");
    return 0;
}
