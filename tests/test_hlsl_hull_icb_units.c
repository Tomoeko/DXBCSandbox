// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "test_tessellation_fixture.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/hlsl_stage_coverage_internal.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static const uint32_t ordinary_values[] = {
    UINT32_C(0x3fa00000), UINT32_C(0x40200000), UINT32_C(0x40700000), UINT32_C(0x40900000)};
static const uint32_t exact_values[] = {
    UINT32_C(0x80000000), UINT32_C(0x00000001), UINT32_C(0x3f800001), UINT32_C(0xbf800001)};

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

static bool fixture_parse(Fixture *fixture, uint8_t *bytes, size_t size, bool scalar) {
    memset(fixture, 0, sizeof(*fixture));
    dxbc_document_init(&fixture->document);
    dxbc_stage_contract_init(&fixture->contract);
    CHECK(bytes);
    const bool parsed = dxbc_document_parse(&fixture->document, bytes, size, NULL);
    free(bytes);
    CHECK(parsed);
    CHECK(dxbc_document_decode_semantic(&fixture->document, &fixture->semantic));
    CHECK(dxbc_stage_contract_decode(&fixture->document, &fixture->semantic, &fixture->contract, NULL));
    CHECK(usil_translate_with_stage_contract(&fixture->program, &fixture->semantic, &fixture->contract));
    CHECK(usil_icb_declaration_is_valid(&fixture->program));
    if (scalar) {
        fixture->field = (SerializedVariable){.name = "_Factor", .layout = {0, 0, 0, 1, 0, 0}};
        fixture->buffer = (SerializedConstantBuffer){.name = "FactorInputs", .size = 16,
            .role = SERIALIZED_CBUFFER_NAMED, .variables = &fixture->field, .var_count = 1};
        fixture->binding = (SerializedResourceParam){.name = fixture->buffer.name,
            .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .array_size = 1};
        fixture->parameters = (SerializedProgramParameters){.constant_buffers = &fixture->buffer,
            .cb_count = 1, .resources = &fixture->binding, .res_count = 1};
    }
    return true;
}

static bool fixture_init(Fixture *fixture, unsigned rows, unsigned column,
                         unsigned chain, unsigned lane, bool zero_base,
                         bool scalar, bool float3, const uint32_t *values) {
    size_t size = 0;
    uint8_t *bytes = test_tessellation_hull_icb_dxbc(rows, column, chain,
        lane, zero_base, scalar, float3, values, &size);
    return fixture_parse(fixture, bytes, size, scalar);
}

static const SerializedProgramParameters *parameters(const Fixture *fixture) {
    return fixture->parameters.cb_count ? &fixture->parameters : NULL;
}

static void fixture_dispose(Fixture *fixture) {
    usil_free(&fixture->program);
    dxbc_stage_contract_free(&fixture->contract);
    dxbc_free(&fixture->semantic);
    dxbc_document_free(&fixture->document);
}

static bool restored(const HLSLStageCoverage *coverage, const HLSLStageCoverage *independent,
                     const StringBuilder *source) {
    CHECK(hlsl_stage_coverage_validate(coverage, source));
    CHECK(hlsl_stage_coverage_equal(coverage, independent));
    return true;
}

static bool ledger_mutations(HLSLStageCoverage *coverage,
                            const HLSLStageCoverage *independent, const StringBuilder *source) {
    HLSLStageHullICB *icb = &coverage->hull_icb;
    HLSLHullICBPlan *plan = &icb->plan;
#define REJECT_RESTORE(change, restore) do { \
    change; CHECK(!hlsl_stage_coverage_validate(coverage, source)); \
    CHECK(!hlsl_stage_coverage_equal(coverage, independent)); \
    restore; CHECK(restored(coverage, independent, source)); \
} while (0)
    REJECT_RESTORE(plan->present = false, plan->present = true);
    REJECT_RESTORE(plan->declaration_token ^= 1, plan->declaration_token ^= 1);
    REJECT_RESTORE(++plan->declaration_source_instruction_index, --plan->declaration_source_instruction_index);
    REJECT_RESTORE(++icb->recorded_plan.declaration_word_count, --icb->recorded_plan.declaration_word_count);
    REJECT_RESTORE(plan->payload[plan->physical_column] ^= 1, plan->payload[plan->physical_column] ^= 1);
    const unsigned unused = (plan->physical_column + 1u) % 4u;
    REJECT_RESTORE(plan->payload[unused] = UINT32_C(0x80000000), plan->payload[unused] = 0);
    REJECT_RESTORE(--plan->row_count, ++plan->row_count);
    REJECT_RESTORE(++plan->phase_index, --plan->phase_index);
    REJECT_RESTORE(++plan->phase.instance_count, --plan->phase.instance_count);
    REJECT_RESTORE(++plan->fork_declaration_index, --plan->fork_declaration_index);
    REJECT_RESTORE(++plan->fork_declaration.source_instruction_index, --plan->fork_declaration.source_instruction_index);
    REJECT_RESTORE(plan->array_name[0] ^= 1, plan->array_name[0] ^= 1);
    REJECT_RESTORE(plan->index_name[0] ^= 1, plan->index_name[0] ^= 1);
    HLSLHullICBConsumer *consumer = &plan->consumers[0];
    REJECT_RESTORE(++consumer->instruction_index, --consumer->instruction_index);
    REJECT_RESTORE(++consumer->source_instruction_index, --consumer->source_instruction_index);
    REJECT_RESTORE(++consumer->operand_index, --consumer->operand_index);
    REJECT_RESTORE(consumer->demanded_lanes ^= 2, consumer->demanded_lanes ^= 2);
    REJECT_RESTORE(consumer->physical_column ^= 1, consumer->physical_column ^= 1);
    REJECT_RESTORE(consumer->operand.raw_token ^= 1, consumer->operand.raw_token ^= 1);
    REJECT_RESTORE(++consumer->operand.index_values[0], --consumer->operand.index_values[0]);
    REJECT_RESTORE(consumer->relative.swizzle[0] ^= 1, consumer->relative.swizzle[0] ^= 1);
    REJECT_RESTORE(consumer->direct_fork_id ^= true, consumer->direct_fork_id ^= true);
    REJECT_RESTORE(++consumer->transport_tail, --consumer->transport_tail);
    if (plan->transport_count) {
        HLSLHullICBTransport *transport = &plan->transports[0];
        REJECT_RESTORE(++transport->instruction_index, --transport->instruction_index);
        REJECT_RESTORE(++transport->source_instruction_index, --transport->source_instruction_index);
        REJECT_RESTORE(transport->destination_lane ^= 1, transport->destination_lane ^= 1);
        REJECT_RESTORE(transport->source_lane ^= 1, transport->source_lane ^= 1);
        REJECT_RESTORE(++transport->destination.register_index, --transport->destination.register_index);
        REJECT_RESTORE(transport->source.raw_token ^= 1, transport->source.raw_token ^= 1);
        REJECT_RESTORE(++transport->predecessor_transport, --transport->predecessor_transport);
    }
    REJECT_RESTORE(++icb->declaration_begin, --icb->declaration_begin);
    REJECT_RESTORE(--icb->declaration_end, ++icb->declaration_end);
    REJECT_RESTORE(++icb->recorded_declaration_begin, --icb->recorded_declaration_begin);
    REJECT_RESTORE(icb->declaration_emitted = false, icb->declaration_emitted = true);
    REJECT_RESTORE(icb->plan_captured = false, icb->plan_captured = true);
    REJECT_RESTORE(--icb->literal_root_count, ++icb->literal_root_count);
    REJECT_RESTORE(--icb->access_root_count, ++icb->access_root_count);
    REJECT_RESTORE(++icb->literal_root_indices[0], --icb->literal_root_indices[0]);
    REJECT_RESTORE(++icb->recorded_access_root_indices[0], --icb->recorded_access_root_indices[0]);
    HLSLStageOwnedRoot *literal = &coverage->roots[icb->literal_root_indices[0]];
    HLSLStageOwnedRoot *access = &coverage->roots[icb->access_root_indices[0]];
    REJECT_RESTORE(literal->tree->u.literal.val[0] ^= 1, literal->tree->u.literal.val[0] ^= 1);
    REJECT_RESTORE(++literal->owner.icb_index, --literal->owner.icb_index);
    REJECT_RESTORE(++access->recorded_owner.icb_index, --access->recorded_owner.icb_index);
    REJECT_RESTORE(++access->begin, --access->begin);
    REJECT_RESTORE(--access->end, ++access->end);
    REJECT_RESTORE(++access->tree->operand_provenance.logical_value_id,
                   --access->tree->operand_provenance.logical_value_id);
    REJECT_RESTORE(access->recorded_tree->operand_provenance.result_components = 2,
                   access->recorded_tree->operand_provenance.result_components = 1);
    /* Changing live and recorded payload together still cannot rewrite the
     * literal roots or the independently emitted bytes. */
    REJECT_RESTORE(plan->payload[plan->physical_column] ^= 1;
                   icb->recorded_plan.payload[plan->physical_column] ^= 1,
                   plan->payload[plan->physical_column] ^= 1;
                   icb->recorded_plan.payload[plan->physical_column] ^= 1);
#undef REJECT_RESTORE
    return true;
}

static bool strict_access_traces(HLSLStageCoverage *coverage,
    const HLSLStageCoverage *independent, const StringBuilder *source) {
    HLSLStageOwnedRoot *access = &coverage->roots[coverage->hull_icb.access_root_indices[0]];
    size_t nodes = 0;
    ASTExpr *registered = hlsl_owned_expression_copy(access->tree, &nodes);
    ASTExpr *clone = hlsl_owned_expression_copy(access->tree, &nodes);
    CHECK(registered && clone);
    /* Reconstruct the producer's pending span state using actual owned access
     * provenance. Matching text/identity on a different pointer is insufficient. */
    coverage->finished = false;
    access->emitted = false;
    access->live_tree = registered;
    CHECK(!hlsl_stage_coverage_span(coverage, clone, access->begin, access->end));
    clone->operand_provenance.logical_value_id = HLSL_HULL_ICB_ACCESS_LOGICAL_ID_BASE | 1;
    CHECK(!hlsl_stage_coverage_span(coverage, clone, access->begin, access->end));
    coverage->finished = true;
    CHECK(!hlsl_stage_coverage_validate(coverage, source));
    CHECK(!hlsl_stage_coverage_equal(coverage, independent));
    coverage->finished = false;
    CHECK(hlsl_stage_coverage_span(coverage, registered, access->begin, access->end));
    CHECK(!hlsl_stage_coverage_span(coverage, registered, access->begin, access->end));
    CHECK(!hlsl_stage_coverage_span(coverage, access->tree, access->begin, access->end));
    coverage->finished = true;
    CHECK(restored(coverage, independent, source));
    ast_free_expr(clone); ast_free_expr(registered);
    return true;
}

static bool positive(unsigned rows, unsigned column, unsigned chain, unsigned lane,
                     bool zero_base, bool scalar, bool float3, const uint32_t *values) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, rows, column, chain, lane, zero_base, scalar, float3, values));
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map = {0}, owned_map = {0};
    HLSLSourceQualityResult quality = {0}, owned_quality = {0};
    HLSLEmitDiagnostic diagnostic;
    StringBuilder normal, source, independent_source;
    sb_init(&normal); sb_init(&source); sb_init(&independent_source);
    options.expression_source_map = &map; options.source_quality = &quality;
    CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &normal,
        parameters(&fixture), NULL, NULL, &options, &diagnostic));
    HLSLStageCoverage coverage = {0}, independent = {0};
    options.expression_source_map = &owned_map; options.source_quality = &owned_quality;
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source,
        parameters(&fixture), NULL, NULL, &options, &coverage, &diagnostic));
    CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &independent_source,
        parameters(&fixture), NULL, NULL, &options, &independent, &diagnostic));
    CHECK(normal.len == source.len && !memcmp(normal.buf, source.buf, source.len + 1));
    CHECK(hlsl_source_quality_results_equal(&quality, &owned_quality));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
          quality.counts.incomplete_units == 1 && !quality.counts.unknown_provenance);
    CHECK(map.complete && owned_map.complete && map.count == owned_map.count);
    for (size_t index = 0; index < map.count; ++index)
        CHECK(hlsl_expression_origins_equal(&map.origins[index], &owned_map.origins[index]));
    CHECK(restored(&coverage, &independent, &source));
    const HLSLStageHullICB *icb = &coverage.hull_icb;
    CHECK(icb->plan.present && icb->plan.row_count == rows && icb->plan.physical_column == column &&
          icb->plan.consumer_count == 1 && icb->plan.transport_count == chain &&
          icb->literal_root_count == rows && icb->access_root_count == 1);
    CHECK(hlsl_hull_icb_plan_matches(&fixture.program, &icb->plan));
    CHECK(icb->declaration_begin < icb->declaration_end &&
          icb->declaration_begin >= coverage.units[0].begin && icb->declaration_end <= coverage.units[0].end);
    CHECK(coverage.obligations & HLSL_STAGE_COVERAGE_BODY);
    CHECK(coverage.units[0].obligations & HLSL_STAGE_COVERAGE_LOCAL_DECLARATION);
    CHECK(strstr(source.buf, "const float ") && !strstr(source.buf, "unk0_arr") && !strstr(source.buf, "r0."));
    for (unsigned row = 0; row < rows; ++row) {
        const HLSLStageOwnedRoot *literal = &coverage.roots[icb->literal_root_indices[row]];
        CHECK(literal->owner.kind == HLSL_STAGE_ROOT_HULL_ICB_LITERAL && literal->owner.icb_index == row &&
              literal->source_unit_id == 0 && literal->tree->kind == AST_EXPR_LITERAL &&
              literal->tree->u.literal.scalar_type == AST_SCALAR_FLOAT32 && literal->tree->u.literal.val[0] == values[row]);
        for (unsigned component = 0; component < 4; ++component)
            CHECK(icb->plan.payload[row * 4 + component] == (component == column ? values[row] : 0));
    }
    const HLSLStageOwnedRoot *access = &coverage.roots[icb->access_root_indices[0]];
    CHECK(access->owner.kind == HLSL_STAGE_ROOT_HULL_ICB_ACCESS && access->owner.icb_index == 0 &&
          access->source_unit_id == 1 && !access->live_tree && access->tree->kind == AST_EXPR_EMITTER_OPERAND &&
          access->tree->operand_provenance.natural_components == 1 &&
          access->tree->operand_provenance.result_components == 1);
    if (rows == 3 && column == 2 && chain == 3 && !scalar) {
        CHECK(ledger_mutations(&coverage, &independent, &source));
        CHECK(strict_access_traces(&coverage, &independent, &source));
    }
    fixture_dispose(&fixture);
    CHECK(restored(&coverage, &independent, &source));
    hlsl_stage_coverage_dispose(&coverage); hlsl_stage_coverage_dispose(&independent);
    sb_free(&normal); sb_free(&source); sb_free(&independent_source);
    return true;
}

static bool rejected_program(Fixture *fixture) {
    StringBuilder normal;
    sb_init(&normal);
    HLSLExpressionSourceMap normal_map = {0};
    HLSLSourceQualityResult normal_quality = {0};
    HLSLEmitDiagnostic normal_diagnostic;
    HLSLEmitOptions normal_options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    normal_options.expression_source_map = &normal_map; normal_options.source_quality = &normal_quality;
    CHECK(!hlsl_emit_with_options_diagnostic(&fixture->program, &normal,
        parameters(fixture), NULL, NULL, &normal_options, &normal_diagnostic));
    CHECK(!normal_map.complete && normal_quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(normal_quality.emission_status == normal_diagnostic.status);
    sb_free(&normal);
    StringBuilder source;
    sb_init(&source);
    HLSLStageCoverage coverage = {0};
    HLSLExpressionSourceMap map = {0};
    HLSLSourceQualityResult quality = {.classification = HLSL_SOURCE_QUALITY_MIXED};
    const HLSLSourceQualityResult retained_quality = quality;
    HLSLEmitDiagnostic diagnostic;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = &map; options.source_quality = &quality;
    CHECK(!hlsl_emit_with_stage_coverage(&fixture->program, &source,
        parameters(fixture), NULL, NULL, &options, &coverage, &diagnostic));
    CHECK(!coverage.finished && !coverage.roots && !coverage.source && !coverage.hull_icb.plan.present);
    CHECK(!map.complete && !map.count);
    if (diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT) {
        /* Decoded-owner rejection precedes capture and preserves caller
         * results; an initialized classification is not an emitted receipt. */
        CHECK(!source.len && sb_ok(&source));
        CHECK(hlsl_source_quality_results_equal(&quality, &retained_quality));
    } else {
        CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(quality.emission_status == diagnostic.status);
    }
    hlsl_stage_coverage_dispose(&coverage);
    sb_free(&source);
    return true;
}

static bool current_mutations(void) {
    Fixture fixture, peer;
    CHECK(fixture_init(&fixture, 3, 1, 3, 2, false, false, false, ordinary_values));
    CHECK(fixture_init(&peer, 2, 1, 0, 0, false, false, false, ordinary_values));
    USILProgram *program = &fixture.program;
    HLSLStageCoverage coverage = {0};
    StringBuilder source;
    sb_init(&source);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    CHECK(hlsl_emit_with_stage_coverage(program, &source, NULL, NULL, NULL, &options, &coverage, NULL));
    const HLSLHullICBPlan *plan = &coverage.hull_icb.plan;
    const int instruction = plan->consumers[0].instruction_index;
    DXBCOperand *operand = &program->instructions[instruction].operands[plan->consumers[0].operand_index];
#define REJECT_CURRENT(change, restore) do { \
    change; CHECK(!hlsl_hull_icb_plan_matches(program, plan)); CHECK(rejected_program(&fixture)); \
    restore; CHECK(hlsl_hull_icb_plan_matches(program, plan)); \
} while (0)
    REJECT_CURRENT(program->has_icb_declaration = false, program->has_icb_declaration = true);
    USILICBDeclarationOwner *owner = program->icb_declaration_owner;
    REJECT_CURRENT(program->icb_declaration_owner = NULL, program->icb_declaration_owner = owner);
    REJECT_CURRENT(program->icb_declaration_owner = peer.program.icb_declaration_owner,
                    program->icb_declaration_owner = owner);
    REJECT_CURRENT(++program->icb_source_instruction_index, --program->icb_source_instruction_index);
    REJECT_CURRENT(program->icb_declaration_token ^= 1, program->icb_declaration_token ^= 1);
    REJECT_CURRENT(--program->icb_declaration_word_count, ++program->icb_declaration_word_count);
    REJECT_CURRENT(program->icb_values[1] ^= 1, program->icb_values[1] ^= 1);
    REJECT_CURRENT(program->icb_values[0] = UINT32_C(0x80000000), program->icb_values[0] = 0);
    REJECT_CURRENT(++program->tessellation.phases[0].instance_count, --program->tessellation.phases[0].instance_count);
    REJECT_CURRENT(++program->signature_declarations[plan->fork_declaration_index].source_instruction_index,
                    --program->signature_declarations[plan->fork_declaration_index].source_instruction_index);
    REJECT_CURRENT(operand->has_abs = true, operand->has_abs = false);
    REJECT_CURRENT(operand->has_neg = true, operand->has_neg = false);
    REJECT_CURRENT(operand->min_precision = 1, operand->min_precision = 0);
    REJECT_CURRENT(operand->swizzle[0] ^= 1, operand->swizzle[0] ^= 1);
    REJECT_CURRENT(++operand->rel_op0->register_index, --operand->rel_op0->register_index);
    REJECT_CURRENT(operand->rel_op0->swizzle[0] ^= 1, operand->rel_op0->swizzle[0] ^= 1);
    const int edge = plan->transports[0].instruction_index;
    REJECT_CURRENT(program->instructions[edge].opcode = USIL_OP_ADD, program->instructions[edge].opcode = USIL_OP_MOV);
    /* Destination ownership uses the decoded lane mask. Adding X to the
     * authored Z-only transport must reject a non-singleton definition. */
    CHECK(usil_operand_destination_lane_mask(&program->instructions[edge].operands[0]) == 4);
    REJECT_CURRENT(program->instructions[edge].operands[0].destination_mask ^= 0x10,
                    program->instructions[edge].operands[0].destination_mask ^= 0x10);
#undef REJECT_CURRENT
    CHECK(hlsl_stage_coverage_validate(&coverage, &source));
    hlsl_stage_coverage_dispose(&coverage); sb_free(&source);
    fixture_dispose(&peer); fixture_dispose(&fixture);
    return true;
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) bytes[byte] = (uint8_t)(value >> (byte * 8));
}

/* Replace one controlled raw instruction using the lossless document's exact
 * boundaries. The executable chunk is last; no signature table is rebuilt. */
static uint8_t *replace_instruction(const DXBCDocument *document, size_t index,
    const uint8_t *replacement, size_t replacement_size, size_t *size) {
    if (!document || index >= document->instruction_count || !replacement || replacement_size % 4 || !size) return NULL;
    const DXBCDocumentInstruction *instruction = &document->instructions[index];
    const DXBCDocumentChunk *chunk = &document->chunks[instruction->chunk_index];
    if (chunk->kind != DXBC_DOCUMENT_CHUNK_EXECUTABLE || chunk->offset + chunk->raw_size != document->owned_size) return NULL;
    *size = document->owned_size - instruction->byte_size + replacement_size;
    uint8_t *bytes = malloc(*size);
    if (!bytes) return NULL;
    memcpy(bytes, document->owned_bytes, instruction->byte_offset);
    memcpy(bytes + instruction->byte_offset, replacement, replacement_size);
    memcpy(bytes + instruction->byte_offset + replacement_size,
        document->owned_bytes + instruction->byte_offset + instruction->byte_size,
        document->owned_size - instruction->byte_offset - instruction->byte_size);
    write_u32(bytes + 24, (uint32_t)*size);
    write_u32(bytes + chunk->offset + 4, (uint32_t)(*size - chunk->offset - 8));
    write_u32(bytes + chunk->offset + 12, (uint32_t)((*size - chunk->offset - 8) / 4));
    if (!dxbc_compute_hash(bytes, *size, bytes + 4)) { free(bytes); return NULL; }
    return bytes;
}

static bool authored_rejections(void) {
    const uint32_t rejected_bits[] = {UINT32_C(0x7f800000), UINT32_C(0xff800000), UINT32_C(0x7fc12345)};
    for (unsigned index = 0; index < 3; ++index) {
        uint32_t values[4]; memcpy(values, ordinary_values, sizeof(values)); values[1] = rejected_bits[index];
        Fixture fixture;
        CHECK(fixture_init(&fixture, 3, 0, 0, 0, false, false, false, values));
        CHECK(rejected_program(&fixture)); fixture_dispose(&fixture);
    }
    /* Preserve a valid raw declaration owner for unused-bit rejection. Merely
     * mutating a live USIL payload would only test its existing integrity gate. */
    for (unsigned index = 0; index < 2; ++index) {
        size_t size = 0;
        uint8_t *bytes = test_tessellation_hull_icb_dxbc(3, 0, 0, 0, false, false, false, ordinary_values, &size);
        CHECK(bytes);
        DXBCDocument document;
        dxbc_document_init(&document);
        CHECK(dxbc_document_parse(&document, bytes, size, NULL));
        CHECK(document.instructions[1].opcode == 53);
        write_u32(bytes + document.instructions[1].byte_offset + 12,
            index ? UINT32_C(0x3f800000) : UINT32_C(0x80000000));
        CHECK(dxbc_compute_hash(bytes, size, bytes + 4));
        dxbc_document_free(&document);
        Fixture fixture;
        CHECK(fixture_parse(&fixture, bytes, size, false));
        CHECK(rejected_program(&fixture)); fixture_dispose(&fixture);
    }
    Fixture baseline;
    CHECK(fixture_init(&baseline, 3, 0, 0, 0, true, false, false, ordinary_values));
    const DXBCDocumentInstruction *declaration = &baseline.document.instructions[1];
    uint8_t duplicate_declaration[112];
    CHECK(declaration->byte_size * 2 == sizeof(duplicate_declaration));
    memcpy(duplicate_declaration, declaration->raw_bytes, declaration->byte_size);
    memcpy(duplicate_declaration + declaration->byte_size, declaration->raw_bytes, declaration->byte_size);
    size_t size = 0;
    uint8_t *bytes = replace_instruction(&baseline.document, 1,
        duplicate_declaration, sizeof(duplicate_declaration), &size);
    CHECK(bytes);
    DXBCDocument duplicate;
    DXBCContainer duplicate_semantic = {0};
    dxbc_document_init(&duplicate);
    CHECK(dxbc_document_parse(&duplicate, bytes, size, NULL)); free(bytes);
    CHECK(duplicate.instruction_count == baseline.document.instruction_count + 1);
    /* Lossless transport retains both declarations, but semantic decoding
     * rejects the duplicate before a stage or access owner can be acquired. */
    CHECK(!dxbc_document_decode_semantic(&duplicate, &duplicate_semantic));
    dxbc_free(&duplicate_semantic); dxbc_document_free(&duplicate);
    uint8_t short_declaration[40];
    memcpy(short_declaration, declaration->raw_bytes, sizeof(short_declaration));
    write_u32(short_declaration + 4, 10); /* Two rows cannot cover three invocations. */
    bytes = replace_instruction(&baseline.document, 1, short_declaration, sizeof(short_declaration), &size);
    Fixture rejected;
    CHECK(fixture_parse(&rejected, bytes, size, false));
    CHECK(rejected_program(&rejected)); fixture_dispose(&rejected);
    size_t consumer_index = SIZE_MAX;
    for (size_t index = 0; index < baseline.document.instruction_count; ++index) {
        const DXBCDocumentInstruction *instruction = &baseline.document.instructions[index];
        if (instruction->opcode == 54 && instruction->token_count > 4) {
            uint32_t token;
            CHECK(dxbc_document_instruction_token(instruction, 4, &token));
            if (token == UINT32_C(0x00d0900a)) consumer_index = index;
        }
    }
    CHECK(consumer_index != SIZE_MAX);
    const DXBCDocumentInstruction *consumer = &baseline.document.instructions[consumer_index];
    uint8_t copy[128];
    CHECK(consumer->byte_size * 2 <= sizeof(copy));
    memcpy(copy, consumer->raw_bytes, consumer->byte_size);
    write_u32(copy + 20, 1); /* Nonzero immediate base is parsed, but unsupported. */
    bytes = replace_instruction(&baseline.document, consumer_index, copy, consumer->byte_size, &size);
    CHECK(fixture_parse(&rejected, bytes, size, false));
    CHECK(rejected_program(&rejected)); fixture_dispose(&rejected);
    /* A dead table load must not acquire an unprinted access receipt. Keep a
     * separate valid output load; enlarge only the actual phase's temp extent. */
    write_u32(copy, 54u | (consumer->token_count - 1) << 24);
    write_u32(copy + 4, UINT32_C(0x00100012)); write_u32(copy + 8, 3);
    memcpy(copy + 12, consumer->raw_bytes + 16, consumer->byte_size - 16);
    const size_t dead_size = consumer->byte_size - 4;
    memcpy(copy + dead_size, consumer->raw_bytes, consumer->byte_size);
    bytes = replace_instruction(&baseline.document, consumer_index, copy, dead_size + consumer->byte_size, &size);
    CHECK(bytes);
    for (size_t index = 0; index < consumer_index; ++index)
        if (baseline.document.instructions[index].opcode == 104)
            write_u32(bytes + baseline.document.instructions[index].byte_offset + 4, 4);
    CHECK(dxbc_compute_hash(bytes, size, bytes + 4));
    CHECK(fixture_parse(&rejected, bytes, size, false));
    CHECK(rejected_program(&rejected)); fixture_dispose(&rejected);
    /* A use by another dead temporary does not grant ownership either. */
    CHECK(dead_size + 20 + consumer->byte_size <= sizeof(copy));
    memmove(copy + dead_size + 20, copy + dead_size, consumer->byte_size);
    write_u32(copy + dead_size, 54u | 5u << 24);
    write_u32(copy + dead_size + 4, UINT32_C(0x00100012));
    write_u32(copy + dead_size + 8, 4);
    write_u32(copy + dead_size + 12, UINT32_C(0x0010000a));
    write_u32(copy + dead_size + 16, 3);
    bytes = replace_instruction(&baseline.document, consumer_index, copy, dead_size + 20 + consumer->byte_size, &size);
    CHECK(bytes);
    for (size_t index = 0; index < consumer_index; ++index)
        if (baseline.document.instructions[index].opcode == 104)
            write_u32(bytes + baseline.document.instructions[index].byte_offset + 4, 5);
    CHECK(dxbc_compute_hash(bytes, size, bytes + 4));
    CHECK(fixture_parse(&rejected, bytes, size, false));
    CHECK(rejected_program(&rejected)); fixture_dispose(&rejected);
    fixture_dispose(&baseline);
    return true;
}

typedef struct { Fixture *fixture; unsigned action; bool acted; } Observer;

static bool observe(void *context, const HLSLSourceQualityObservation *observation) {
    Observer *observer = context;
    if (observer->acted || observation->source_unit_id != 1) return true;
    observer->acted = true;
    if (observer->action == 0) return false;
    if (observer->action == 1) observer->fixture->program.icb_values[0] ^= 1;
    else ++observer->fixture->program.icb_source_instruction_index;
    return true;
}

static bool callback_rollback(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, 3, 0, 1, 3, false, false, false, ordinary_values));
    for (unsigned action = 0; action < 3; ++action) {
        HLSLStageCoverage coverage = {0};
        HLSLSourceQualityResult quality = {0};
        HLSLExpressionSourceMap map = {0};
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        Observer observer = {.fixture = &fixture, .action = action};
        options.source_quality = &quality; options.expression_source_map = &map;
        options.source_quality_observer = observe; options.source_quality_observer_context = &observer;
        StringBuilder source;
        sb_init(&source);
        HLSLEmitDiagnostic diagnostic;
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(observer.acted && !coverage.finished && !coverage.roots && !coverage.source &&
              !coverage.hull_icb.plan.present && !map.complete && !map.count);
        /* An observer veto is analysis failure. Mutating the live parsed
         * owner invalidates the next phase's admission and stays unsupported. */
        CHECK(quality.classification == (action == 0
            ? HLSL_SOURCE_QUALITY_FAILED : HLSL_SOURCE_QUALITY_UNSUPPORTED));
        CHECK(quality.emission_status == diagnostic.status);
        if (action == 1) fixture.program.icb_values[0] ^= 1;
        else if (action == 2) --fixture.program.icb_source_instruction_index;
        sb_free(&source); sb_init(&source);
        options.source_quality_observer = NULL; options.source_quality_observer_context = NULL;
        CHECK(hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &coverage, NULL));
        CHECK(hlsl_stage_coverage_validate(&coverage, &source));
        hlsl_stage_coverage_dispose(&coverage); sb_free(&source);
    }
    fixture_dispose(&fixture);
    return true;
}

static bool initial_ledger_preservation(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, 3, 0, 0, 0, false, false, false, ordinary_values));
    HLSLExpressionSourceMap map = {0};
    HLSLSourceQualityResult quality = {0};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = &map; options.source_quality = &quality;
    StringBuilder baseline;
    sb_init(&baseline);
    CHECK(hlsl_emit_with_options_diagnostic(&fixture.program, &baseline, NULL, NULL, NULL, &options, NULL));
    const HLSLExpressionSourceMap retained_map = map;
    const HLSLSourceQualityResult retained_quality = quality;
    for (unsigned sentinel = 0; sentinel < 8; ++sentinel) {
        HLSLStageCoverage coverage = {0};
        switch (sentinel) {
        case 0: coverage.hull_icb.plan.payload[15] = 1; break;
        case 1: coverage.hull_icb.recorded_plan.consumers[63].operand.raw_token = 1; break;
        case 2: coverage.hull_icb.plan.transports[63].source_lane = 1; break;
        case 3: coverage.hull_icb.declaration_begin = 1; break;
        case 4: coverage.hull_icb.recorded_declaration_end = 1; break;
        case 5: coverage.hull_icb.literal_root_indices[3] = 1; break;
        case 6: coverage.hull_icb.recorded_access_root_indices[63] = 1; break;
        case 7: coverage.hull_icb.recorded_plan_captured = true; break;
        }
        HLSLStageCoverage retained;
        memcpy(&retained, &coverage, sizeof(retained));
        StringBuilder source;
        sb_init(&source);
        HLSLEmitDiagnostic diagnostic;
        CHECK(!hlsl_emit_with_stage_coverage(&fixture.program, &source, NULL, NULL, NULL, &options, &coverage, &diagnostic));
        CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT && sb_ok(&source) && !source.len);
        CHECK(!memcmp(&retained, &coverage, sizeof(retained)));
        CHECK(hlsl_source_quality_results_equal(&quality, &retained_quality));
        CHECK(map.count == retained_map.count && map.complete == retained_map.complete);
        for (size_t index = 0; index < map.count; ++index)
            CHECK(hlsl_expression_origins_equal(&map.origins[index], &retained_map.origins[index]));
        sb_free(&source);
    }
    sb_free(&baseline); fixture_dispose(&fixture);
    return true;
}

int main(void) {
    const size_t allocations = g_allocations_count, bytes = g_allocated_bytes;
    for (unsigned rows = 2; rows <= 4; ++rows) {
        for (unsigned column = 0; column < 4; ++column) {
            for (unsigned zero_base = 0; zero_base < 2; ++zero_base)
                if (!positive(rows, column, 0, 0, zero_base, false, false, ordinary_values)) return 1;
            for (unsigned lane = 0; lane < 4; ++lane)
                for (unsigned zero_base = 0; zero_base < 2; ++zero_base)
                    if (!positive(rows, column, 3, lane, zero_base, false, false, ordinary_values)) return 1;
        }
    }
    for (unsigned column = 0; column < 4; ++column) {
        if (!positive(3, column, 0, 0, false, false, true, exact_values) ||
            !positive(4, column, 1, column, false, false, false, exact_values) ||
            !positive(3, column, 1, column, true, true, false, ordinary_values)) return 1;
    }
    if (!current_mutations() || !authored_rejections() || !callback_rollback() ||
        !initial_ledger_preservation()) return 1;
    if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
        fprintf(stderr, "allocation leak: %zu/%zu -> %zu/%zu\n", allocations, bytes,
            g_allocations_count, g_allocated_bytes); return 1;
    }
    puts("HULL ICB unit tests passed");
    return 0;
}
