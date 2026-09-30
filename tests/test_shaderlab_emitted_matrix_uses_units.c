// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitted_matrix_uses.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "translation/hlsl_emitter_internal.h"
#include "common/file_io.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "dxbc/usbd.h"
#include "test_shaderlab_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

typedef TestShaderLabMatrixFixture Fixture;

static bool fixture_init(Fixture *fixture) {
    return test_shaderlab_matrix_fixture_init(fixture, MATRIX_USES_PIXEL_FIXTURE, false);
}

static void fixture_dispose(Fixture *fixture) {
    test_shaderlab_matrix_fixture_dispose(fixture);
}

static bool reject_entry(void *context, const ShaderLabSourceSyntaxReceipt *receipt) {
    (void)context;
    return receipt->kind != SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY;
}

static StringBuilder coverage_source(const HLSLStageCoverage *coverage) {
    return (StringBuilder){.buf = coverage->source, .len = coverage->source_size,
        .capacity = coverage->source_size + 1};
}

static bool opcode_ledger_mutations(const ShaderLabSourceQualityRequest *request,
    ShaderLabEmittedMatrixUses *owned, const ShaderLabEmittedMatrixUses *independent) {
    HLSLStageCoverage *coverage = &owned->entries[0].coverage;
    const HLSLStageCoverage *other = &independent->entries[0].coverage;
    StringBuilder source = coverage_source(coverage);
    CHECK(coverage->instruction_count == 5 && coverage->opcodes[0] == USIL_OP_MUL);
    CHECK(coverage->opcodes[1] == USIL_OP_MAD && coverage->opcodes[4] == USIL_OP_RET);
    CHECK(hlsl_stage_coverage_validate(coverage, &source));
    CHECK(hlsl_stage_coverage_equal(coverage, other));
    const USILOpcode original = coverage->opcodes[0];
    const uint32_t obligations = coverage->obligations;

    coverage->opcodes[0] = USIL_OP_ADD;
    CHECK(!hlsl_stage_coverage_validate(coverage, &source));
    CHECK(!hlsl_stage_coverage_equal(coverage, other));
    CHECK(!shaderlab_emitted_matrix_uses_replay(request, owned));
    coverage->opcodes[0] = original;
    CHECK(shaderlab_emitted_matrix_uses_replay(request, owned));

    coverage->recorded_opcodes[0] = USIL_OP_ADD;
    CHECK(!hlsl_stage_coverage_validate(coverage, &source));
    CHECK(!hlsl_stage_coverage_equal(coverage, other));
    CHECK(!shaderlab_emitted_matrix_uses_replay(request, owned));
    coverage->recorded_opcodes[0] = original;
    CHECK(shaderlab_emitted_matrix_uses_replay(request, owned));

    /* Coordinated edits still differ from the independent capture of the
     * retained target. Matching two edited private arrays is not authority. */
    coverage->opcodes[0] = coverage->recorded_opcodes[0] = USIL_OP_ADD;
    CHECK(hlsl_stage_coverage_validate(coverage, &source));
    CHECK(!hlsl_stage_coverage_equal(coverage, other));
    CHECK(!shaderlab_emitted_matrix_uses_replay(request, owned));
    coverage->opcodes[0] = coverage->recorded_opcodes[0] = (USILOpcode)-1;
    CHECK(!hlsl_stage_coverage_validate(coverage, &source));
    CHECK(!hlsl_stage_coverage_equal(coverage, other));
    CHECK(!shaderlab_emitted_matrix_uses_replay(request, owned));
    coverage->opcodes[0] = coverage->recorded_opcodes[0] = original;
    CHECK(hlsl_stage_coverage_validate(coverage, &source));
    CHECK(hlsl_stage_coverage_equal(coverage, other));

    const size_t instruction_count = coverage->instruction_count;
    coverage->instruction_count = HLSL_STAGE_COVERAGE_ROOT_LIMIT + 1;
    CHECK(!hlsl_stage_coverage_validate(coverage, &source));
    CHECK(!hlsl_stage_coverage_equal(coverage, coverage));
    coverage->instruction_count = instruction_count;
    CHECK(!hlsl_stage_coverage_equal(NULL, coverage));
    CHECK(coverage->obligations == obligations);
    CHECK(shaderlab_emitted_matrix_uses_replay(request, owned));
    return true;
}

static bool opcode_owner_drift(const HLSLMatrixUseCapture *entry) {
    DXBCDocument document;
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    USILProgram program = {0};
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContractDiagnostic contract_diagnostic;
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    CHECK(dxbc_document_parse(&document, entry->target, entry->target_size, &document_diagnostic));
    CHECK(dxbc_document_decode_semantic(&document, &semantic));
    CHECK(dxbc_stage_contract_decode(&document, &semantic, &contract, &contract_diagnostic));
    CHECK(usil_translate_with_stage_contract(&program, &semantic, &contract));
    CHECK(program.instruction_count == 5 && program.instructions[0].opcode == USIL_OP_MUL);

    /* Exercise sealing independently of the source-quality producer. This
     * deliberately incomplete capture cannot clear its existing obligations. */
    HLSLMatrixUseCapture capture = {0}, changed = {0};
    StringBuilder source = coverage_source(&entry->coverage);
    HLSLEmitterContext context = {.program = &program, .sb = &source,
        .matrix_use_capture = &capture, .emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE};
    CHECK(hlsl_stage_coverage_begin(&context));
    CHECK(capture.coverage.opcodes[0] == USIL_OP_MUL);
    program.instructions[0].opcode = USIL_OP_ADD;
    hlsl_stage_coverage_finish(&context);
    CHECK(!capture.coverage.finished && !hlsl_stage_coverage_validate(&capture.coverage, &source));
    program.instructions[0].opcode = USIL_OP_MUL;
    hlsl_stage_coverage_finish(&context);
    CHECK(capture.coverage.finished && hlsl_stage_coverage_validate(&capture.coverage, &source));
    CHECK(capture.coverage.obligations & HLSL_STAGE_COVERAGE_BODY);
    CHECK(capture.coverage.obligations & HLSL_STAGE_COVERAGE_SYNTAX);

    /* An owner changed after sealing is visible in a new actual-program
     * capture even when the supplied source text is identical. */
    context.matrix_use_capture = &changed;
    program.instructions[0].opcode = USIL_OP_ADD;
    CHECK(hlsl_stage_coverage_begin(&context));
    hlsl_stage_coverage_finish(&context);
    CHECK(hlsl_stage_coverage_validate(&changed.coverage, &source));
    CHECK(!hlsl_stage_coverage_equal(&capture.coverage, &changed.coverage));
    hlsl_stage_coverage_dispose(&changed.coverage);
    program.instructions[0].opcode = USIL_OP_MUL;
    CHECK(hlsl_stage_coverage_begin(&context));
    hlsl_stage_coverage_finish(&context);
    CHECK(hlsl_stage_coverage_validate(&changed.coverage, &source));
    CHECK(hlsl_stage_coverage_equal(&capture.coverage, &changed.coverage));
    hlsl_stage_coverage_dispose(&changed.coverage);
    hlsl_stage_coverage_dispose(&capture.coverage);
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    return true;
}

static bool positive_and_mutations(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    ShaderLabEmittedMatrixUses *owned = NULL;
    ShaderLabMatrixUsesStatus status = shaderlab_emitted_matrix_uses_capture(&request, &owned);
    if (status != SHADERLAB_MATRIX_USES_OK) fprintf(stderr, "capture status %d\n", status);
    CHECK(status == SHADERLAB_MATRIX_USES_OK && owned);
    CHECK(shaderlab_emitted_matrix_uses_replay(&request, owned));
    ShaderLabEmittedMatrixSummary summary;
    CHECK(shaderlab_emitted_matrix_uses_describe(owned, &summary));
    CHECK(summary.entry_count == 2 && summary.use_count == 1 && summary.field_count == 1 && summary.read_count == 16);
    CHECK(summary.base_quality.classification == HLSL_SOURCE_QUALITY_MIXED);
    CHECK(summary.base_quality.gaps & SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE);
    ShaderLabEmittedMatrixUse use;
    CHECK(shaderlab_emitted_matrix_uses_use(owned, 0, 0, &use));
    CHECK(use.read_count == 16 && use.instruction_count == 4 && use.final_instruction == 3);
    const char *source;
    size_t source_size;
    CHECK(shaderlab_emitted_matrix_uses_source(owned, &source, &source_size));
    CHECK(source_size == summary.source_size && strstr(source, "ObjectTransform") && strstr(source, "mul("));
    StringBuilder normal;
    sb_init(&normal);
    ShaderLabSourceQualityInventory normal_inventory = {0};
    CHECK(shaderlab_source_quality_emit(&request, &normal, &normal_inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(normal.len == source_size && !memcmp(normal.buf, source, source_size));
    CHECK(normal_inventory.quality.gaps == summary.base_quality.gaps);
    CHECK(normal_inventory.entries.records[0].source_quality.classification == owned->inventory.entries.records[0].source_quality.classification);
    shaderlab_source_quality_inventory_dispose(&normal_inventory);
    sb_free(&normal);

    ShaderLabEmittedMatrixUses *independent = NULL;
    CHECK(shaderlab_emitted_matrix_uses_capture(&request, &independent) == SHADERLAB_MATRIX_USES_OK);
    CHECK(opcode_ledger_mutations(&request, owned, independent));
    CHECK(opcode_owner_drift(&owned->entries[0]));
    ASTExpr *left_tree = owned->entries[0].uses[0].tree;
    ASTExpr *right_tree = independent->entries[0].uses[0].tree;
    CHECK(hlsl_matrix_uses_trees_equal(left_tree, right_tree));
    ++right_tree->logical_origin.destination_lanes;
    CHECK(!hlsl_matrix_uses_trees_equal(left_tree, right_tree));
    --right_tree->logical_origin.destination_lanes;
    CHECK(right_tree->kind == AST_EXPR_CALL);
    ASTExpr *aggregate = right_tree->u.call.args[1];
    if (aggregate->kind != AST_EXPR_EMITTER_OPERAND || aggregate->operand_provenance.natural_components)
        aggregate = right_tree->u.call.args[0];
    CHECK(aggregate->kind == AST_EXPR_EMITTER_OPERAND && !aggregate->operand_provenance.natural_components);
    ++aggregate->operand_provenance.logical_value_id;
    CHECK(!hlsl_matrix_uses_trees_equal(left_tree, right_tree));
    --aggregate->operand_provenance.logical_value_id;
    ++aggregate->operand_provenance.selected_components[0];
    CHECK(!hlsl_matrix_uses_trees_equal(left_tree, right_tree));
    --aggregate->operand_provenance.selected_components[0];
    char *saved_name = right_tree->u.call.name;
    right_tree->u.call.name = "AnotherConstructor";
    CHECK(!hlsl_matrix_uses_trees_equal(left_tree, right_tree));
    right_tree->u.call.name = saved_name;
    ASTExpr *saved_child = right_tree->u.call.args[0];
    right_tree->u.call.args[0] = NULL;
    CHECK(!hlsl_matrix_uses_trees_equal(left_tree, right_tree));
    right_tree->u.call.args[0] = saved_child;
    CHECK(hlsl_matrix_uses_trees_equal(left_tree, right_tree));
    shaderlab_emitted_matrix_uses_free(independent);

    HLSLMatrixUseCapture *entry = &owned->entries[0];
    --entry->reads.read_count;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    ++entry->reads.read_count;
    ++entry->reads.read_count;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->reads.read_count;
    ++entry->reads.reads[0].operand_index;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->reads.reads[0].operand_index;
    ++entry->reads.reads[0].physical_lane;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->reads.reads[0].physical_lane;
    ++entry->reads.fields[0].declared_byte_size;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->reads.fields[0].declared_byte_size;
    entry->target[4] ^= 1;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    entry->target[4] ^= 1;
    entry->player_payload[8] ^= 1;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    entry->player_payload[8] ^= 1;
    ++entry->current.constant_buffers[0].size;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->current.constant_buffers[0].size;
    --entry->use_count;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    ++entry->use_count;
    ++entry->uses[0].observation.instructions[0];
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->uses[0].observation.instructions[0];
    ++entry->uses[0].observation.source_begin;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->uses[0].observation.source_begin;
    entry->uses[0].tree->logical_origin.logical_value_id ^= 1;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    entry->uses[0].tree->logical_origin.logical_value_id ^= 1;
    const char saved = owned->source.buf[use.source_begin];
    owned->source.buf[use.source_begin] ^= 1;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    owned->source.buf[use.source_begin] = saved;
    ++entry->raw_map.origins[0].source_instruction_index;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->raw_map.origins[0].source_instruction_index;
    ++entry->player.source_map;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->player.source_map;
    fixture.segments[0][8] ^= 1;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    fixture.segments[0][8] ^= 1;
    fixture.programs[0].shader_requirements ^= UINT64_C(1) << 30;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    fixture.programs[0].shader_requirements ^= UINT64_C(1) << 30;
    fixture.programs[0].has_hardware_tier = true;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    fixture.programs[0].has_hardware_tier = false;
    fixture.buffer.has_is_partial = true;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    fixture.buffer.has_is_partial = false;
    ++entry->common.constant_buffers[0].size;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --entry->common.constant_buffers[0].size;
    ++fixture.identities[0].hardware_tier_group;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    --fixture.identities[0].hardware_tier_group;
    fixture.matrix.name = "RenamedCurrentMatrix";
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    fixture.matrix.name = "ObjectTransform";
    CHECK(shaderlab_emitted_matrix_uses_replay(&request, owned));
    /* Factory rejects a nonempty destination and preserves its exact owner. */
    ShaderLabEmittedMatrixUses *saved_owner = owned;
    CHECK(shaderlab_emitted_matrix_uses_capture(&request, &owned) == SHADERLAB_MATRIX_USES_INVALID_ARGUMENT);
    CHECK(owned == saved_owner);
    ShaderLabEmittedMatrixUses *rejected = NULL;
    request.observer = reject_entry;
    CHECK(shaderlab_emitted_matrix_uses_capture(&request, &rejected) != SHADERLAB_MATRIX_USES_OK && !rejected);
    request.observer = NULL;
    fixture.subshader.pass_count = 2;
    CHECK(shaderlab_emitted_matrix_uses_capture(&request, &rejected) == SHADERLAB_MATRIX_USES_SCOPE_UNAVAILABLE && !rejected);
    fixture.subshader.pass_count = 1;
    /* Owned observations remain valid after every borrowed input is released. */
    fixture_dispose(&fixture);
    CHECK(shaderlab_emitted_matrix_uses_describe(owned, &summary));
    CHECK(shaderlab_emitted_matrix_uses_source(owned, &source, &source_size));
    HLSLCurrentMatrixField field;
    CHECK(shaderlab_emitted_matrix_uses_field(owned, 0, 0, &field));
    CHECK(!strcmp(field.field_name, "ObjectTransform"));
    shaderlab_emitted_matrix_uses_free(owned);
    return true;
}

int main(void) {
    return positive_and_mutations() ? 0 : 1;
}
