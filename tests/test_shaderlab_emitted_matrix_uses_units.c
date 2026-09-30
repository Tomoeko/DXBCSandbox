// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitted_matrix_uses.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "common/file_io.h"
#include "dxbc/usbd.h"
#include "test_shaderlab_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

typedef struct {
    SerializedShader shader;
    SerializedSubShader subshader;
    SerializedPass pass;
    SerializedSubProgram programs[2];
    SerializedSubProgramIdentity identities[2];
    SerializedVariable matrix;
    SerializedConstantBuffer buffer;
    SerializedResourceParam binding;
    ShaderBlobArchive archive;
    BlobEntry entries[2];
    uint8_t *segments[2];
    int lengths[2], platform;
} Fixture;

static bool fixture_init(Fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    size_t size;
    uint8_t *vertex = test_shaderlab_matrix_vertex_dxbc(&size);
    CHECK(vertex);
    size_t payload_size;
    fixture->segments[0] = test_shaderlab_variant_blob(vertex, size, 16, NULL, &payload_size);
    free(vertex);
    CHECK(fixture->segments[0] && payload_size <= INT32_MAX);
    fixture->lengths[0] = (int)payload_size;
    CommonFileBytes bytes = {0};
    CHECK(common_file_read_regular(MATRIX_USES_PIXEL_FIXTURE, 1024 * 1024, &bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    DXBCUSBDRecordView pixel;
    CHECK(dxbc_usbd_table_open(&table, bytes.data, bytes.size, NULL));
    CHECK(dxbc_usbd_table_record(&table, 1, &pixel));
    fixture->segments[1] = test_shaderlab_variant_blob(pixel.dxbc, pixel.dxbc_size, 17, NULL, &payload_size);
    common_file_bytes_dispose(&bytes);
    CHECK(fixture->segments[1] && payload_size <= INT32_MAX);
    fixture->lengths[1] = (int)payload_size;
    for (int stage = 0; stage < 2; ++stage) {
        fixture->entries[stage] = (BlobEntry){0, fixture->lengths[stage], stage};
        fixture->programs[stage] = (SerializedSubProgram){.blob_index = stage,
            .program_type = stage ? 17 : 16, .shader_requirements = 0xe3};
        fixture->identities[stage].hardware_tier_group = 3;
        fixture->pass.subprogram_count[stage] = 1;
        fixture->pass.subprograms[stage] = &fixture->programs[stage];
        fixture->pass.subprogram_identities[stage] = &fixture->identities[stage];
    }
    fixture->matrix = (SerializedVariable){.name = "ObjectTransform", .layout = {0, 4, 4, 1, 0, 0}};
    fixture->buffer = (SerializedConstantBuffer){.name = "Matrices", .size = 64,
        .var_count = 1, .variables = &fixture->matrix};
    fixture->binding = (SerializedResourceParam){.name = "Matrices",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .bind_index = 0};
    fixture->pass.common_parameters[0] = (SerializedProgramParameters){.is_binary = true,
        .cb_count = 1, .constant_buffers = &fixture->buffer, .res_count = 1, .resources = &fixture->binding};
    fixture->platform = 4;
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = &fixture->platform;
    fixture->pass.program_mask = 6;
    fixture->subshader = (SerializedSubShader){.pass_count = 1, .passes = &fixture->pass};
    fixture->shader = (SerializedShader){.name = "Fixture/Quality/MatrixUses",
        .subshader_count = 1, .subshaders = &fixture->subshader};
    fixture->archive = (ShaderBlobArchive){.entries = fixture->entries, .entry_count = 2,
        .segments = fixture->segments, .segment_lengths = fixture->lengths, .segment_count = 2};
    return true;
}

static void fixture_dispose(Fixture *fixture) {
    free(fixture->segments[0]);
    free(fixture->segments[1]);
}

static bool reject_entry(void *context, const ShaderLabSourceSyntaxReceipt *receipt) {
    (void)context;
    return receipt->kind != SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY;
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
