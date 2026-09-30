// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_required_declaration_quality_internal.h"
#include "compiler/unity_hlsl_cbuffer_layout_internal.h"
#include "test_shaderlab_fixture.h"
#include "translation/usil.h"
#include "translation/hlsl_emitter_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

/* Controlled complete syntax, not a captured private byte array. The target
 * declares 64 bytes; metadata and the full declaration retain the distinct 176-byte shell. */
static const char complete_block[] =
    "cbuffer UnityPerDraw {\n"
    " float4x4 unity_ObjectToWorld;\n"
    " float4x4 unity_WorldToObject;\n"
    " float4 unity_LODFade;\n"
    " half4 unity_WorldTransformParams;\n"
    " float4 unity_RenderingLayer;\n"
    "};\n";

static bool parsed_declarations(UnityRequiredDeclarationQuality *out) {
    UnityHlslCBufferInventory inventory;
    unity_hlsl_cbuffer_inventory_init(&inventory);
    const char *names[] = {"UnityPerDraw"};
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)complete_block,
        sizeof(complete_block) - 1, names, 1,
        (UnityHlslCBufferStoragePolicy){.legacy_half_is_float32 = true}, &inventory) == UNITY_HLSL_CBUFFER_OK);
    CHECK(inventory.block_count == 1 && inventory.field_count == 5);
    unity_required_declaration_quality_init(out);
    CHECK(unity_required_declaration_quality_block(out, 0, &inventory.blocks[0], inventory.fields, 5));
    CHECK(unity_required_declaration_quality_seal(out));
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    return true;
}

static bool observe_fragments(void *context, const HLSLSourceQualityObservation *observation) {
    if (observation->unit_kind != HLSL_SOURCE_UNIT_REQUIRED_EXTERNAL_DECLARATION) return true;
    CHECK(observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION);
    CHECK(observation->facts.known && !observation->facts.artifacts);
    CHECK(observation->facts.cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_NONE);
    CHECK(!observation->facts.cbuffer_declaration_authority);
    ++*(size_t *)context;
    return true;
}

static bool reject_fragment(void *context, const HLSLSourceQualityObservation *observation) {
    (void)context;
    return observation->unit_kind != HLSL_SOURCE_UNIT_REQUIRED_EXTERNAL_DECLARATION;
}

static bool positive_and_mutations(void) {
    TestShaderLabMatrixFixture fixture;
    CHECK(test_shaderlab_matrix_fixture_init(&fixture, REQUIRED_DECLARATION_PIXEL_FIXTURE, true));
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    ShaderLabEmittedMatrixUses *owned = NULL;
    CHECK(shaderlab_emitted_matrix_uses_capture(&request, &owned) == SHADERLAB_MATRIX_USES_OK);
    CHECK(shaderlab_emitted_matrix_uses_replay(&request, owned));
    HLSLMatrixUseCapture *entry = &owned->entries[0];
    CHECK(entry->coverage.obligations == HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION);
    CHECK(entry->coverage.required_binding_mask == 1 && entry->coverage.root_count == 1);
    CHECK(entry->coverage.syntax_count && entry->reads.fields[0].declared_byte_size == 64);
    CHECK(entry->reads.fields[0].reflected_byte_size == 176);
    CHECK(entry->observation.base_quality.classification == HLSL_SOURCE_QUALITY_MIXED);
    CHECK(entry->observation.base_quality.counts.incomplete_units == 1);
    UnityRequiredDeclarationQuality declarations;
    CHECK(parsed_declarations(&declarations));
    HLSLSourceQualityResult quality;
    uint32_t unresolved = UINT32_MAX;
    size_t fragment_events = 0;
    CHECK(unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved,
        observe_fragments, &fragment_events));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.reasons && !unresolved);
    CHECK(quality.counts.inspected_units == 2 && !quality.counts.incomplete_units);
    CHECK(!quality.counts.unknown_provenance && !quality.counts.residual_total && fragment_events == 7);
    CHECK(owned->inventory.quality.classification == HLSL_SOURCE_QUALITY_MIXED);
    CHECK(owned->inventory.quality.gaps & SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE);
    CHECK(owned->inventory.quality.gaps & SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY);
    CHECK(!unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved,
        reject_fragment, NULL));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED && unresolved == UINT32_MAX);
    UnityRequiredDeclarationQuality empty;
    unity_required_declaration_quality_init(&empty);
    CHECK(unity_required_declaration_quality_seal(&empty));
    CHECK(!unity_required_declaration_quality_analyze(entry, &empty, &quality, &unresolved, NULL, NULL));
    unity_required_declaration_quality_dispose(&empty);

#define ANALYSIS_REJECTS_INCREMENT(field) do { ++(field); \
    CHECK(!unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved, NULL, NULL)); \
    --(field); } while (0)
    ANALYSIS_REJECTS_INCREMENT(declarations.inventory.blocks[0].field_count);
    ANALYSIS_REJECTS_INCREMENT(declarations.inventory.fields[4].name[0]);
    ANALYSIS_REJECTS_INCREMENT(declarations.inventory.fields[4].source_end);
    ANALYSIS_REJECTS_INCREMENT(declarations.inventory.fields[0].scalar);
    ANALYSIS_REJECTS_INCREMENT(entry->reads.fields[0].reflected_byte_size);
    ANALYSIS_REJECTS_INCREMENT(entry->coverage.required_binding_mask);
    ANALYSIS_REJECTS_INCREMENT(entry->coverage.roots[0].tree->logical_origin.source_instruction_index);
    ANALYSIS_REJECTS_INCREMENT(entry->coverage.node_count);
    ANALYSIS_REJECTS_INCREMENT(entry->coverage.syntax[0].facts.source_instruction_index);
    ANALYSIS_REJECTS_INCREMENT(entry->coverage.syntax[0].source_end);
#undef ANALYSIS_REJECTS_INCREMENT
    /* Complete external blocks cannot cover missing local syntax or bodies. */
    const uint32_t missing[] = {HLSL_STAGE_COVERAGE_SYNTAX, HLSL_STAGE_COVERAGE_BODY,
        HLSL_STAGE_COVERAGE_LOCAL_DECLARATION};
    for (size_t index = 0; index < sizeof(missing) / sizeof(missing[0]); ++index) {
        entry->coverage.obligations |= missing[index];
        CHECK(unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved, NULL, NULL));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_MIXED && quality.counts.incomplete_units == 1);
        CHECK(unresolved == missing[index]);
        entry->coverage.obligations &= ~missing[index];
    }
#define REPLAY_REJECTS_INCREMENT(field) do { ++(field); \
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned)); --(field); } while (0)
    REPLAY_REJECTS_INCREMENT(entry->coverage.syntax_count);
    REPLAY_REJECTS_INCREMENT(entry->coverage.syntax[0].source_end);
    REPLAY_REJECTS_INCREMENT(entry->coverage.roots[0].whole_begin);
#undef REPLAY_REJECTS_INCREMENT
    --entry->coverage.syntax_count;
    CHECK(!unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved, NULL, NULL));
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    ++entry->coverage.syntax_count;
    entry->coverage.syntax[0].facts.known = false;
    CHECK(!unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved, NULL, NULL));
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    entry->coverage.syntax[0].facts.known = true;
    --entry->coverage.root_count;
    CHECK(!unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved, NULL, NULL));
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    ++entry->coverage.root_count;
    entry->coverage.source[0] ^= 1;
    CHECK(!shaderlab_emitted_matrix_uses_replay(&request, owned));
    entry->coverage.source[0] ^= 1;
    CHECK(shaderlab_emitted_matrix_uses_replay(&request, owned));
    test_shaderlab_matrix_fixture_dispose(&fixture);
    CHECK(unity_required_declaration_quality_analyze(entry, &declarations, &quality, &unresolved, NULL, NULL));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    unity_required_declaration_quality_dispose(&declarations);
    shaderlab_emitted_matrix_uses_free(owned);
    return true;
}

/* DP3 consumes three lanes even when its destination is one scalar. The
 * source modifier owns the actual operand coordinate; neither a different
 * operation nor a syntax statement may borrow that source demand. */
static bool source_demand_ownership(void) {
    USILInstruction instruction = {.opcode = USIL_OP_DP3, .operand_count = 3,
        .source_instruction_index = 11};
    instruction.operands[0] = (DXBCOperand){.type = OPERAND_TYPE_TEMP, .destination_mask = 0x10};
    instruction.operands[1] = (DXBCOperand){.type = OPERAND_TYPE_INPUT, .has_abs = true,
        .swizzle_mode = 1, .swizzle = {0, 1, 2, 3}};
    instruction.operands[2] = (DXBCOperand){.type = OPERAND_TYPE_IMMEDIATE32,
        .imm_value_count = 4, .swizzle_mode = 1, .swizzle = {0, 1, 2, 3}};
    USILProgram program = {.program_type = DXBC_PROGRAM_TYPE_PIXEL,
        .instructions = &instruction, .instruction_count = 1};
    HLSLMatrixUseCapture capture = {0};
    StringBuilder source;
    sb_init(&source);
    HLSLEmitterContext ctx = {.program = &program, .sb = &source, .matrix_use_capture = &capture};
    CHECK(hlsl_stage_coverage_begin(&ctx));
    CHECK(capture.coverage.destination_lanes[0] == 1);
    CHECK(capture.coverage.operand_counts[0] == 3);
    CHECK(capture.coverage.operand_uses[0][1].is_source &&
        capture.coverage.operand_uses[0][1].source_lanes == 7 &&
        capture.coverage.operand_uses[0][1].type == OPERAND_TYPE_INPUT &&
        capture.coverage.operand_uses[0][1].absolute);
    ASTOperandProvenance operand;
    ast_operand_provenance_init(&operand);
    operand.complete = true;
    operand.value_role = AST_OPERAND_VALUE_LOGICAL;
    operand.logical_value_id = 1;
    operand.natural_components = operand.result_components = 3;
    operand.instruction_index = 0;
    operand.source_instruction_index = 11;
    operand.operand_index = 1;
    operand.destination_lanes = 7;
    ASTExpr *normal = ast_create_emitter_operand_with_provenance("normal", &operand);
    ASTExpr *absolute = normal ? ast_create_call("abs", &normal, 1) : NULL;
    CHECK(absolute);
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.components = 3;
    origin.logical_value_id = 0;
    origin.instruction_index = 0;
    origin.source_instruction_index = 11;
    origin.destination_lanes = 7;
    CHECK(ast_set_logical_value_origin(absolute, &origin));
    const uint32_t bits[] = {0x3f800000, 0x3f800000, 0x3f800000};
    ASTExpr *arguments[] = {absolute, ast_create_literal_bits(bits, 3, AST_SCALAR_FLOAT32)};
    ASTExpr *tree = arguments[1] ? ast_create_call("dot", arguments, 2) : NULL;
    CHECK(tree);
    origin.components = 1;
    origin.destination_lanes = 1;
    CHECK(ast_set_logical_value_origin(tree, &origin));
    CHECK(hlsl_stage_coverage_root(&ctx, tree, 0));
    ast_format_expr(tree, &source);
    CHECK(hlsl_stage_coverage_span(&capture.coverage, tree, 0, source.len));
    HLSLSourceQualityObservation observation = {.kind = HLSL_SOURCE_OBSERVATION_EMISSION,
        .unit_kind = HLSL_SOURCE_UNIT_ENTRY_POINT};
    hlsl_source_quality_facts_init(&observation.facts);
    observation.facts.known = true;
    observation.facts.instruction_index = 0;
    observation.facts.source_instruction_index = 11;
    observation.facts.lanes = 1;
    CHECK(hlsl_stage_coverage_observation(&ctx, &observation));
    hlsl_stage_coverage_finish(&ctx);
    CHECK(hlsl_stage_coverage_validate(&capture.coverage, &source));
    ASTExpr *owned = capture.coverage.roots[0].tree;
    ASTExpr *recorded = capture.coverage.roots[0].recorded_tree;
    /* Alter both typed tree copies so these negatives exercise owner checks,
     * rather than merely the independent immutable-tree equality guard. */
    owned->logical_origin.destination_lanes = recorded->logical_origin.destination_lanes = 7;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    owned->logical_origin.destination_lanes = recorded->logical_origin.destination_lanes = 1;
    owned->u.call.args[0]->logical_origin.destination_lanes =
        recorded->u.call.args[0]->logical_origin.destination_lanes = 3;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    owned->u.call.args[0]->logical_origin.destination_lanes =
        recorded->u.call.args[0]->logical_origin.destination_lanes = 7;
    ASTExpr *owned_atom = owned->u.call.args[0]->u.call.args[0];
    ASTExpr *recorded_atom = recorded->u.call.args[0]->u.call.args[0];
    owned_atom->operand_provenance.operand_index = recorded_atom->operand_provenance.operand_index = 2;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    owned_atom->operand_provenance.operand_index = recorded_atom->operand_provenance.operand_index = 1;
    capture.coverage.operand_uses[0][1].absolute = false;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    capture.coverage.recorded_operand_uses[0][1].absolute = false;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    capture.coverage.operand_uses[0][1].absolute = capture.coverage.recorded_operand_uses[0][1].absolute = true;
    capture.coverage.syntax[0].facts.lanes = capture.coverage.recorded_syntax[0].facts.lanes = 7;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    capture.coverage.syntax[0].facts.lanes = capture.coverage.recorded_syntax[0].facts.lanes = 1;
    capture.coverage.operand_uses[0][1].type = OPERAND_TYPE_TEMP;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    capture.coverage.operand_uses[0][1].type = OPERAND_TYPE_INPUT;
    capture.coverage.operand_counts[0] = DXBC_MAX_OPERANDS + 1;
    CHECK(!hlsl_stage_coverage_validate(&capture.coverage, &source));
    capture.coverage.operand_counts[0] = 3;
    CHECK(hlsl_stage_coverage_validate(&capture.coverage, &source));
    ast_free_expr(tree);
    hlsl_stage_coverage_dispose(&capture.coverage);
    sb_free(&source);
    return true;
}

static bool shared_layout_boundaries(void) {
    UnityRequiredDeclarationQuality declarations;
    CHECK(parsed_declarations(&declarations));
    uint32_t cursor = 0;
    for (size_t index = 0; index < declarations.inventory.field_count; ++index) {
        const UnityHlslCBufferField *actual = &declarations.inventory.fields[index];
        UnityHlslCBufferField copied = *actual;
        copied.byte_offset = copied.byte_size = UINT32_MAX;
        CHECK(unity_hlsl_cbuffer_layout_field(&copied, &cursor) == UNITY_HLSL_CBUFFER_OK);
        CHECK(copied.byte_offset == actual->byte_offset && copied.byte_size == actual->byte_size);
    }
    uint32_t extent;
    CHECK(unity_hlsl_cbuffer_layout_extent(cursor, &extent) == UNITY_HLSL_CBUFFER_OK);
    CHECK(extent == declarations.inventory.blocks[0].byte_size);
    unity_required_declaration_quality_dispose(&declarations);

    UnityHlslCBufferField vector = {.scalar = UNITY_HLSL_CBUFFER_UINT, .rows = 1, .columns = 2};
    cursor = 12;
    CHECK(unity_hlsl_cbuffer_layout_field(&vector, &cursor) == UNITY_HLSL_CBUFFER_OK);
    CHECK(vector.byte_offset == 16 && vector.byte_size == 8 && cursor == 24);
    UnityHlslCBufferField invalid = vector;
    invalid.is_matrix = true;
    invalid.rows = invalid.columns = 4;
    CHECK(unity_hlsl_cbuffer_layout_field(&invalid, &cursor) == UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION);
    CHECK(cursor == 24 && invalid.byte_offset == 16 && invalid.byte_size == 8);
    invalid.scalar = UNITY_HLSL_CBUFFER_FLOAT;
    invalid.rows = 3;
    CHECK(unity_hlsl_cbuffer_layout_field(&invalid, &cursor) == UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION);
    CHECK(cursor == 24);
    cursor = 65536;
    const uint32_t saved_offset = vector.byte_offset;
    CHECK(unity_hlsl_cbuffer_layout_field(&vector, &cursor) == UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT);
    CHECK(cursor == 65536 && vector.byte_offset == saved_offset);
    CHECK(unity_hlsl_cbuffer_layout_extent(cursor, &extent) == UNITY_HLSL_CBUFFER_OK && extent == 65536);
    ++cursor;
    CHECK(unity_hlsl_cbuffer_layout_extent(cursor, &extent) == UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT);
    CHECK(extent == 65536);
    return true;
}

static bool mechanical_copy_boundaries(void) {
    const uint32_t bits[4] = {1, 2, 3, 4};
    ASTExpr *condition = ast_create_comparison(USIL_OP_LT,
        ast_create_var(0, 0, 0, "amount"), ast_create_literal_bits(bits, 1, AST_SCALAR_UINT32));
    ASTExpr *yes = ast_create_cast("float", ast_create_unary(USIL_OP_INEG,
        ast_create_binary(USIL_OP_ADD, ast_create_var(1, 1, 0, "left"),
            ast_create_var(2, 2, 0, "right"))));
    ASTExpr *no = ast_create_bitcast(AST_SCALAR_FLOAT32,
        ast_create_literal_bits(bits, 1, AST_SCALAR_UINT32));
    ASTExpr *tree = ast_create_ternary(condition, yes, no);
    CHECK(tree && condition && yes && no);
    tree->logical_origin = (ASTLogicalValueOrigin){.complete = true, .scalar_type = AST_SCALAR_FLOAT32,
        .components = 1, .logical_value_id = 3, .instruction_index = 2,
        .source_instruction_index = 7, .destination_lanes = 1};
    size_t nodes;
    ASTExpr *copy = hlsl_owned_expression_copy(tree, &nodes);
    CHECK(copy && copy != tree && nodes > 8 && hlsl_matrix_uses_trees_equal(copy, tree));
    ++copy->u.ternary.false_expr->u.bitcast.sub->u.literal.val[0];
    CHECK(!hlsl_matrix_uses_trees_equal(copy, tree));
    --copy->u.ternary.false_expr->u.bitcast.sub->u.literal.val[0];
    copy->logical_origin.semantic_projection = true;
    CHECK(!hlsl_matrix_uses_trees_equal(copy, tree));
    ast_free_expr(copy);
    ast_free_expr(tree);
    ASTExpr *deep = ast_create_literal_bits(bits, 1, AST_SCALAR_UINT32);
    CHECK(deep);
    for (size_t index = 0; index < 66; ++index) {
        ASTExpr *next = ast_create_unary(USIL_OP_INEG, deep);
        CHECK(next);
        deep = next;
    }
    CHECK(!hlsl_owned_expression_copy(deep, &nodes));
    ast_free_expr(deep);
    ASTExpr cycle = {.kind = AST_EXPR_UNARY, .u.unary = {.op = USIL_OP_INEG}};
    cycle.u.unary.sub = &cycle;
    CHECK(!hlsl_owned_expression_copy(&cycle, &nodes));
    return true;
}

static bool declaration_boundaries(void) {
    UnityHlslCBufferInventory inventory;
    unity_hlsl_cbuffer_inventory_init(&inventory);
    const char *names[] = {"UnityPerDraw"};
    const char *bad[] = {
        "cbuffer UnityPerDraw { row_major float4x4 unity_ObjectToWorld; };",
        "cbuffer UnityPerDraw { float4x4 unity_ObjectToWorld[2]; };",
        "cbuffer UnityPerDraw { float4x4 unity_ObjectToWorld : packoffset(c0); };",
        "cbuffer UnityPerDraw { float4x4 unity_ObjectToWorld; float4x4 unity_ObjectToWorld; };"
    };
    for (size_t index = 0; index < sizeof(bad) / sizeof(bad[0]); ++index) {
        CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)bad[index], strlen(bad[index]), names, 1,
            (UnityHlslCBufferStoragePolicy){0}, &inventory) != UNITY_HLSL_CBUFFER_OK);
        CHECK(!inventory.fields && !inventory.block_count);
    }
    UnityRequiredDeclarationQuality owned;
    CHECK(parsed_declarations(&owned));
    CHECK(!unity_required_declaration_quality_block(&owned, 0, &owned.inventory.blocks[0], owned.inventory.fields, 5));
    unity_required_declaration_quality_dispose(&owned);
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    return true;
}

int main(void) {
    return positive_and_mutations() && source_demand_ownership() && shared_layout_boundaries() &&
        mechanical_copy_boundaries() && declaration_boundaries() ? 0 : 1;
}
