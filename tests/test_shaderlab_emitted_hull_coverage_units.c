// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitted_hull_coverage.h"
#include "translation/shaderlab_emitted_hull_coverage_internal.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/usil_validation.h"
#include "translation/shaderlab_emitter_internal.h"
#include "common/file_io.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "dxbc/usbd.h"
#include "test_geometry_fixture.h"
#include "test_shaderlab_fixture.h"
#include "test_tessellation_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

enum { HULL_STAGE = 3, DOMAIN_STAGE = 4, FIXTURE_VARIANT_COUNT = 9, FIXTURE_BLOB_LIMIT = 11 };

typedef enum {
    EMPTY_ROUTE_ABSENT,
    EMPTY_ROUTE_COMMON,
    EMPTY_ROUTE_EXPLICIT_ABSENCE,
    EMPTY_ROUTE_PARSED_ZERO,
    EMPTY_ROUTE_PARSED_GLOBALS,
    EMPTY_ROUTE_MIXED_PARSED
} EmptyRoute;

typedef struct {
    SerializedShader shader;
    SerializedSubShader subshader;
    SerializedPass pass;
    SerializedSubProgram programs[5][2];
    SerializedSubProgramIdentity identities[5][2];
    SerializedStringPool strings;
    ShaderBlobArchive archive;
    BlobEntry entries[FIXTURE_BLOB_LIMIT];
    uint8_t *segments[FIXTURE_BLOB_LIMIT];
    int lengths[FIXTURE_BLOB_LIMIT], platform, keyword_index;
    int parameter_indices[2];
    char *keyword_name;
    uint8_t keyword_flag;
    uint16_t keyword_mask;
} Fixture;

static bool fixture_blob(Fixture *fixture, int index, const uint8_t *dxbc,
                         size_t dxbc_size, int type, const char *keyword) {
    size_t size = 0;
    fixture->segments[index] = test_shaderlab_variant_blob(dxbc, dxbc_size,
        type, keyword, &size);
    CHECK(fixture->segments[index] && size <= INT32_MAX);
    fixture->lengths[index] = (int)size;
    fixture->entries[index] = (BlobEntry){0, (int32_t)size, index};
    return true;
}

static bool fixture_stage(Fixture *fixture, int stage, uint8_t *dxbc, size_t size) {
    const int types[] = {15, 17, 19, 21, 22};
    CHECK(dxbc);
    const bool wrapped = fixture_blob(fixture, stage, dxbc, size, types[stage], NULL);
    free(dxbc);
    CHECK(wrapped);
    fixture->programs[stage][0] = (SerializedSubProgram){.blob_index = stage,
        .program_type = types[stage], .shader_requirements = 0xe3};
    fixture->identities[stage][0].hardware_tier_group = 3;
    fixture->pass.subprogram_count[stage] = 1;
    fixture->pass.subprograms[stage] = fixture->programs[stage];
    fixture->pass.subprogram_identities[stage] = fixture->identities[stage];
    fixture->pass.program_mask |= 2u << stage;
    return true;
}

static bool fixture_init(Fixture *fixture, bool control_point, bool scalar) {
    memset(fixture, 0, sizeof(*fixture));
    CommonFileBytes bytes = {0};
    CHECK(common_file_read_regular(SHADERLAB_HULL_COVERAGE_FIXTURE, 1024 * 1024,
                                   &bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, bytes.data, bytes.size, NULL));
    CHECK(table.record_count == 2);
    for (int stage = 0; stage < 2; ++stage) {
        DXBCUSBDRecordView record;
        CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &record));
        CHECK(fixture_blob(fixture, stage, record.dxbc, record.dxbc_size,
                           stage ? 17 : 15, NULL));
        fixture->programs[stage][0] = (SerializedSubProgram){.blob_index = stage,
            .program_type = stage ? 17 : 15, .shader_requirements = 0xe3};
        fixture->identities[stage][0].hardware_tier_group = 3;
        fixture->pass.subprogram_count[stage] = 1;
        fixture->pass.subprograms[stage] = fixture->programs[stage];
        fixture->pass.subprogram_identities[stage] = fixture->identities[stage];
    }
    common_file_bytes_dispose(&bytes);
    fixture->pass.program_mask = 6;
    size_t size = 0;
    uint8_t *hull = scalar
        ? test_tessellation_hull_scalar_cbuffer_dxbc(3, 3, false, NULL, &size)
        : test_tessellation_hull_dxbc(3, 3, control_point ? 4 : 0, &size);
    CHECK(fixture_stage(fixture, HULL_STAGE, hull, size));
    uint8_t *domain = test_tessellation_domain_dxbc(2, 3, 7, &size);
    CHECK(fixture_stage(fixture, DOMAIN_STAGE, domain, size));
    fixture->platform = 4;
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = &fixture->platform;
    fixture->pass.state.name = "HULL_COVERAGE";
    fixture->subshader = (SerializedSubShader){.pass_count = 1, .passes = &fixture->pass};
    const char *name = NULL;
    CHECK(serialized_string_pool_copy(&fixture->strings, "Fixture/Quality/HullCoverage", &name));
    fixture->shader = (SerializedShader){.name = name,
        .subshader_count = 1, .subshaders = &fixture->subshader};
    fixture->archive = (ShaderBlobArchive){.entries = fixture->entries,
        .entry_count = 5, .segments = fixture->segments,
        .segment_lengths = fixture->lengths, .segment_count = 5};

    /* Only the scalar target declares and reads FactorInputs. Its controlled
     * API projection serves both the ordinary current fallback and common
     * metadata, which each receipt must copy independently. Constant targets
     * use the existing truly empty metadata route. This does not claim a
     * separate parameter-blob producer or player reflection. */
    SerializedVariable field = {.name = "_Factor", .layout = {0, 0, 0, 1, 0, 0}};
    SerializedConstantBuffer buffer = {.name = "FactorInputs", .size = 16,
        .role = SERIALIZED_CBUFFER_NAMED, .variables = &field, .var_count = 1};
    SerializedResourceParam binding = {.name = buffer.name,
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .array_size = 1};
    SerializedProgramParameters parameters = {.constant_buffers = &buffer, .cb_count = 1,
        .resources = &binding, .res_count = 1};
    if (scalar)
        CHECK(serialized_program_parameters_copy(&fixture->pass.common_parameters[HULL_STAGE],
                                                 &parameters));
    return true;
}

static void fixture_dispose(Fixture *fixture) {
    for (int index = 0; index < FIXTURE_BLOB_LIMIT; ++index) free(fixture->segments[index]);
    serialized_program_parameters_free(&fixture->pass.common_parameters[HULL_STAGE]);
    serialized_string_pool_dispose(&fixture->strings);
    memset(fixture, 0, sizeof(*fixture));
}

static bool fixture_scalar_names(Fixture *fixture, bool renamed) {
    if (!renamed) return true;
    SerializedProgramParameters *parameters = &fixture->pass.common_parameters[HULL_STAGE];
    CHECK(parameters->cb_count == 1 && parameters->res_count == 1 &&
          parameters->constant_buffers[0].var_count == 1);
    SerializedVariable field = parameters->constant_buffers[0].variables[0];
    SerializedConstantBuffer buffer = parameters->constant_buffers[0];
    SerializedResourceParam binding = parameters->resources[0];
    field.name = "PatchScale";
    buffer.name = binding.name = "PatchInputs";
    buffer.variables = &field;
    SerializedProgramParameters source = *parameters, replacement = {0};
    source.constant_buffers = &buffer;
    source.resources = &binding;
    CHECK(serialized_program_parameters_copy(&replacement, &source));
    serialized_program_parameters_free(parameters);
    *parameters = replacement;
    return true;
}

static bool fixture_init_icb(Fixture *fixture, bool scalar) {
    CHECK(fixture_init(fixture, false, scalar));
    const uint32_t values[] = {UINT32_C(0x3fa00000), UINT32_C(0x40200000), UINT32_C(0x40700000)};
    size_t size = 0;
    uint8_t *hull = test_tessellation_hull_icb_dxbc(3, 2, 3, 3, true,
        scalar, false, values, &size);
    CHECK(hull);
    free(fixture->segments[HULL_STAGE]); fixture->segments[HULL_STAGE] = NULL;
    CHECK(fixture_stage(fixture, HULL_STAGE, hull, size));
    return true;
}

/* Pair two independently parsed stage targets. The whole-source receipt does
 * not infer a linked-stage or runtime certificate from matching semantics. */
static bool fixture_init_float3_shape(Fixture *fixture, bool scalar, bool icb,
                                     const char *semantic, bool control_point) {
    CHECK(!control_point || (!scalar && !icb));
    CHECK(fixture_init(fixture, false, scalar));
    size_t size = 0;
    const uint32_t values[] = {UINT32_C(0x3fa00000), UINT32_C(0x40200000), UINT32_C(0x40700000)};
    CHECK(!icb || !strcmp(semantic, "POINTVALUE"));
    uint8_t *hull = icb
        ? test_tessellation_hull_icb_dxbc(3, 2, 3, 3, true, scalar, true, values, &size)
        : scalar ? test_tessellation_hull_scalar_cbuffer_dxbc(3, 3, true, semantic, &size)
        : test_tessellation_hull_float3_dxbc(3, 3,
            control_point ? TEST_HULL_FLOAT3_EXPLICIT_ADD : 0, semantic, &size);
    free(fixture->segments[HULL_STAGE]); fixture->segments[HULL_STAGE] = NULL;
    CHECK(fixture_stage(fixture, HULL_STAGE, hull, size));
    uint8_t *domain = test_tessellation_domain_float3_dxbc(3, semantic,
        TEST_DOMAIN_FLOAT3_VALID, &size);
    free(fixture->segments[DOMAIN_STAGE]); fixture->segments[DOMAIN_STAGE] = NULL;
    CHECK(fixture_stage(fixture, DOMAIN_STAGE, domain, size));
    return true;
}

static bool fixture_init_float3(Fixture *fixture, bool scalar, bool icb,
                               const char *semantic) {
    return fixture_init_float3_shape(fixture, scalar, icb, semantic, false);
}

static bool fixture_second_state(Fixture *fixture, bool unsupported_hull) {
    const char *name = NULL;
    CHECK(serialized_string_pool_copy(&fixture->strings, "FEATURE", &name));
    fixture->keyword_name = (char *)name;
    fixture->keyword_flag = 1;
    fixture->shader.keyword_names = (SerializedKeywordList){1, &fixture->keyword_name};
    fixture->shader.keyword_flags = &fixture->keyword_flag;
    fixture->pass.serialized_keyword_state_mask_count = 1;
    fixture->pass.serialized_keyword_state_mask = &fixture->keyword_mask;
    const int stages[] = {0, 1, HULL_STAGE, DOMAIN_STAGE};
    for (unsigned ordinal = 0; ordinal < 4; ++ordinal) {
        const int stage = stages[ordinal], index = 5 + (int)ordinal;
        ByteStream stream;
        stream_init(&stream, fixture->segments[stage], (size_t)fixture->lengths[stage]);
        stream_set_endian(&stream, false);
        PlayerSubProgramMetadata player = {0};
        CHECK(subprogram_metadata_parse_variant(&stream, &player));
        uint8_t *different = NULL;
        size_t different_size = 0;
        if (unsupported_hull && stage == HULL_STAGE) {
            different = test_tessellation_hull_dxbc(3, 3, 2, &different_size);
            CHECK(different);
        }
        const bool wrapped = fixture_blob(fixture, index,
            different ? different : player.bytecode,
            different ? different_size : player.bytecode_length,
            fixture->programs[stage][0].program_type, name);
        free(different);
        subprogram_metadata_free_variant(&player);
        CHECK(wrapped);
        fixture->programs[stage][1] = fixture->programs[stage][0];
        fixture->programs[stage][1].blob_index = index;
        fixture->programs[stage][1].local_keyword_count = 1;
        fixture->programs[stage][1].local_keywords = &fixture->keyword_name;
        fixture->identities[stage][1] = fixture->identities[stage][0];
        fixture->identities[stage][1].local_keyword_index_count = 1;
        fixture->identities[stage][1].local_keyword_indices = &fixture->keyword_index;
        fixture->pass.subprogram_count[stage] = 2;
    }
    fixture->archive.entry_count = fixture->archive.segment_count = FIXTURE_VARIANT_COUNT;
    return true;
}

static bool fixture_empty_route(Fixture *fixture, EmptyRoute route) {
    if (route == EMPTY_ROUTE_ABSENT) return true;
    SerializedConstantBuffer shell = {.name = "$Globals", .role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS};
    SerializedProgramParameters common = {.cb_count = 1, .constant_buffers = &shell};
    CHECK(serialized_program_parameters_copy(&fixture->pass.common_parameters[HULL_STAGE], &common));
    if (route == EMPTY_ROUTE_COMMON) return true;
    fixture->parameter_indices[0] = fixture->parameter_indices[1] = -1;
    fixture->pass.subprogram_param_blob_indices[HULL_STAGE] = fixture->parameter_indices;
    if (route == EMPTY_ROUTE_EXPLICIT_ABSENCE) return true;
    for (int state = 0; state < fixture->pass.subprogram_count[HULL_STAGE]; ++state) {
        const int index = FIXTURE_VARIANT_COUNT + state;
        const bool globals = route == EMPTY_ROUTE_PARSED_GLOBALS ||
            (route == EMPTY_ROUTE_MIXED_PARSED && state == 1);
        size_t size = 0;
        fixture->segments[index] = test_shaderlab_empty_parameters_blob(globals, &size);
        CHECK(fixture->segments[index] && size <= INT32_MAX);
        fixture->lengths[index] = (int)size;
        fixture->entries[index] = (BlobEntry){0, (int32_t)size, index};
        fixture->parameter_indices[state] = index;
        fixture->archive.entry_count = fixture->archive.segment_count = index + 1;
        ByteStream stream;
        stream_init(&stream, fixture->segments[index], size); stream_set_endian(&stream, false);
        SerializedProgramParameters parsed = {0};
        CHECK(subprogram_metadata_parse_parameters(&stream, &parsed));
        CHECK(!stream_remaining(&stream) && parsed.is_binary &&
              parsed.version == UNITY_2021_3_PLAYER_BLOB_VERSION &&
              parsed.dialect == PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1 &&
              parsed.cb_count == (globals ? 1 : 0) && !parsed.res_count);
        if (globals) CHECK(parsed.constant_buffers[0].role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS &&
                           !strcmp(parsed.constant_buffers[0].name, "$Globals"));
        serialized_program_parameters_free(&parsed);
    }
    return true;
}

static StringBuilder local_source(const HLSLStageCoverage *coverage) {
    return (StringBuilder){.buf = coverage->source, .len = coverage->source_size,
        .capacity = coverage->source_size + 1};
}

static bool replay_restored(const ShaderLabSourceQualityRequest *request,
                            const ShaderLabEmittedHullCoverage *owned) {
    CHECK(shaderlab_emitted_hull_coverage_replay(request, owned));
    return true;
}

/* Independent line-copy oracle for this existing producer's twelve-space
 * indentation. Start boundaries follow indentation; newline ends precede the
 * next indentation. No parser or production placement helper is reused. */
static size_t whole_coordinate(const HLSLStageCoverage *coverage, size_t body_begin,
                               size_t coordinate, bool end) {
    size_t lines = 0;
    for (size_t byte = 0; byte < coordinate; ++byte)
        if (coverage->source[byte] == '\n') ++lines;
    const bool at_line_end = end && coordinate && coverage->source[coordinate - 1] == '\n';
    return body_begin + coordinate + 12 * (lines + (at_line_end ? 0 : 1));
}

static bool check_placements(const ShaderLabEmittedHullCoverage *owned,
                             const HLSLHullCoverageCapture *entry) {
    const HLSLStageCoverage *coverage = &entry->coverage;
    const HLSLHullWholePlacement *placement = &entry->placement;
    CHECK(placement->rebased && placement->offset && placement->unit_count == coverage->unit_count &&
          placement->unit_count == (entry->observation.stage_index == HULL_STAGE ? 3u : 1u));
    CHECK(placement->root_count == coverage->root_count &&
          placement->syntax_count == coverage->syntax_count);
    const size_t body_begin = entry->observation.source_begin;
    CHECK(coverage->source_size && coverage->source[coverage->source_size - 1] == '\n');
    StringBuilder indented;
    sb_init(&indented);
    size_t position = 0;
    while (position < coverage->source_size) {
        sb_append(&indented, "            ");
        do sb_append_char(&indented, coverage->source[position++]);
        while (position < coverage->source_size && coverage->source[position - 1] != '\n');
    }
    CHECK(sb_ok(&indented) && body_begin < entry->observation.source_end &&
          entry->observation.source_end <= owned->source.len &&
          indented.len == entry->observation.source_end - body_begin &&
          !memcmp(indented.buf, owned->source.buf + body_begin, indented.len));
    sb_free(&indented);
    for (size_t index = 0; index < coverage->root_count; ++index) {
        const HLSLStageOwnedRoot *root = &coverage->roots[index];
        CHECK(placement->roots[index].begin == whole_coordinate(coverage, body_begin, root->begin, false));
        CHECK(placement->roots[index].end == whole_coordinate(coverage, body_begin, root->end, true));
        CHECK(placement->roots[index].begin >= body_begin &&
              placement->roots[index].end <= entry->observation.source_end);
    }
    for (size_t index = 0; index < placement->unit_count; ++index) {
        CHECK(placement->units[index].begin == whole_coordinate(coverage, body_begin, coverage->units[index].begin, false));
        CHECK(placement->units[index].end == whole_coordinate(coverage, body_begin, coverage->units[index].end, true));
    }
    for (size_t index = 0; index < coverage->syntax_count; ++index) {
        const HLSLStageOwnedSyntax *syntax = &coverage->syntax[index];
        CHECK(syntax->source_unit_id < coverage->unit_count);
        const bool at_unit_begin = syntax->source_end == coverage->units[syntax->source_unit_id].begin;
        CHECK(placement->syntax_ends[index] == whole_coordinate(coverage, body_begin,
            syntax->source_end, !at_unit_begin));
    }
    CHECK(placement->has_icb_declaration == coverage->hull_icb.plan.present);
    if (coverage->hull_icb.plan.present) {
        CHECK(placement->icb_declaration.begin == whole_coordinate(coverage, body_begin,
              coverage->hull_icb.declaration_begin, false));
        CHECK(placement->icb_declaration.end == whole_coordinate(coverage, body_begin,
              coverage->hull_icb.declaration_end, true));
        CHECK(placement->icb_declaration.begin >= placement->units[0].begin &&
              placement->icb_declaration.begin < placement->icb_declaration.end &&
              placement->icb_declaration.end <= placement->units[0].end);
    } else CHECK(!placement->icb_declaration.begin && !placement->icb_declaration.end);
    CHECK(placement->has_domain_return == coverage->domain_output.plan.present);
    if (placement->has_domain_return) {
        CHECK(placement->domain_return.begin == whole_coordinate(coverage, body_begin,
              coverage->domain_output.return_begin, false));
        CHECK(placement->domain_return.end == whole_coordinate(coverage, body_begin,
              coverage->domain_output.return_end, true));
        CHECK(placement->domain_return.begin >= placement->units[0].begin &&
              placement->domain_return.begin < placement->domain_return.end &&
              placement->domain_return.end <= placement->units[0].end);
    } else CHECK(!placement->domain_return.begin && !placement->domain_return.end);
    return true;
}

static bool owned_mutations(const ShaderLabSourceQualityRequest *request,
                            ShaderLabEmittedHullCoverage *owned) {
    HLSLHullCoverageCapture *entry = &owned->entries[0];
    HLSLStageCoverage *coverage = &entry->coverage;
    HLSLHullWholePlacement *placement = &entry->placement;
#define REJECT_RESTORE(change, restore) do { \
    change; CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned)); \
    restore; CHECK(replay_restored(request, owned)); \
} while (0)
    REJECT_RESTORE(owned->source.buf[0] ^= 1, owned->source.buf[0] ^= 1);
    REJECT_RESTORE(owned->sealed = false, owned->sealed = true);
    REJECT_RESTORE(entry->inputs.target[4] ^= 1, entry->inputs.target[4] ^= 1);
    REJECT_RESTORE(entry->inputs.player_payload[8] ^= 1, entry->inputs.player_payload[8] ^= 1);
    REJECT_RESTORE(entry->inputs.player.source_map ^= 1, entry->inputs.player.source_map ^= 1);
    REJECT_RESTORE(entry->inputs.current.version ^= 1, entry->inputs.current.version ^= 1);
    REJECT_RESTORE(entry->inputs.common.version ^= 1, entry->inputs.common.version ^= 1);
    if (entry->inputs.current.cb_count) {
        CHECK(entry->inputs.current.cb_count == 1);
        REJECT_RESTORE(entry->inputs.current.constant_buffers[0].size += 16,
                       entry->inputs.current.constant_buffers[0].size -= 16);
    }
    if (entry->inputs.common.cb_count) {
        CHECK(entry->inputs.common.cb_count == 1);
        if (entry->inputs.common.constant_buffers[0].var_count) {
            CHECK(entry->inputs.common.constant_buffers[0].var_count == 1);
            REJECT_RESTORE(entry->inputs.common.constant_buffers[0].variables[0].layout[0] += 4,
                           entry->inputs.common.constant_buffers[0].variables[0].layout[0] -= 4);
        } else {
            REJECT_RESTORE(entry->inputs.common.constant_buffers[0].size = 16,
                           entry->inputs.common.constant_buffers[0].size = 0);
        }
    }
    const size_t inventory_ordinal = entry->observation.entry_record_index;
    REJECT_RESTORE(entry->observation.entry_record_index = 0,
                   entry->observation.entry_record_index = inventory_ordinal);
    REJECT_RESTORE(++entry->observation.source_begin, --entry->observation.source_begin);
    REJECT_RESTORE(--entry->observation.source_end, ++entry->observation.source_end);
    REJECT_RESTORE(entry->observation.target_digest[0] ^= 1, entry->observation.target_digest[0] ^= 1);
    const HLSLSourceQualityResult base_quality = entry->observation.base_quality;
    CHECK(base_quality.classification != HLSL_SOURCE_QUALITY_FAILED);
    REJECT_RESTORE(entry->observation.base_quality.classification = HLSL_SOURCE_QUALITY_FAILED,
                   entry->observation.base_quality = base_quality);
    REJECT_RESTORE(owned->inventory.quality.gaps ^= SHADERLAB_SOURCE_GAP_STAGE_COVERAGE,
                   owned->inventory.quality.gaps ^= SHADERLAB_SOURCE_GAP_STAGE_COVERAGE);
    REJECT_RESTORE(owned->inventory.modeled_input_digest[0] ^= 1,
                   owned->inventory.modeled_input_digest[0] ^= 1);
    REJECT_RESTORE(++owned->inventory.entries.records[entry->observation.entry_record_index].subprogram_index,
                   --owned->inventory.entries.records[entry->observation.entry_record_index].subprogram_index);
    const HLSLStageCoverageSchema schema = coverage->schema;
    REJECT_RESTORE(coverage->schema = HLSL_STAGE_COVERAGE_ORDINARY_ENTRY, coverage->schema = schema);
    REJECT_RESTORE(coverage->obligations ^= HLSL_STAGE_COVERAGE_BODY,
                   coverage->obligations ^= HLSL_STAGE_COVERAGE_BODY);
    REJECT_RESTORE(coverage->units[1].obligations ^= HLSL_STAGE_COVERAGE_BODY,
                   coverage->units[1].obligations ^= HLSL_STAGE_COVERAGE_BODY);
    REJECT_RESTORE(coverage->source[0] ^= 1, coverage->source[0] ^= 1);
    REJECT_RESTORE(++coverage->hull_contract.tessellation.output_control_point_count,
                   --coverage->hull_contract.tessellation.output_control_point_count);
    REJECT_RESTORE(++coverage->recorded_hull_contract.phases[0].instance_count,
                   --coverage->recorded_hull_contract.phases[0].instance_count);
    const USILOpcode opcode = coverage->opcodes[0];
    REJECT_RESTORE(coverage->opcodes[0] = coverage->recorded_opcodes[0] = USIL_OP_ADD,
                   coverage->opcodes[0] = coverage->recorded_opcodes[0] = opcode);
    REJECT_RESTORE(++coverage->roots[0].owner.phase_index, --coverage->roots[0].owner.phase_index);
    REJECT_RESTORE(++coverage->roots[0].tree->logical_origin.logical_value_id,
                   --coverage->roots[0].tree->logical_origin.logical_value_id);
    REJECT_RESTORE(++entry->raw_map.origins[0].source_instruction_index,
                   --entry->raw_map.origins[0].source_instruction_index);
    REJECT_RESTORE(++placement->roots[0].begin, --placement->roots[0].begin);
    REJECT_RESTORE(++placement->roots[0].end, --placement->roots[0].end);
    const HLSLHullWholeRange root = placement->roots[0];
    REJECT_RESTORE(placement->roots[0] = ((HLSLHullWholeRange){0, 1}), placement->roots[0] = root);
    const size_t unit_begin = placement->units[0].begin;
    REJECT_RESTORE(placement->units[0].begin = entry->observation.source_begin,
                   placement->units[0].begin = unit_begin);
    REJECT_RESTORE(++placement->units[1].end, --placement->units[1].end);
    REJECT_RESTORE(++placement->syntax_ends[0], --placement->syntax_ends[0]);
    REJECT_RESTORE(++placement->line_cursor, --placement->line_cursor);
    REJECT_RESTORE(placement->rebased = false, placement->rebased = true);
    REJECT_RESTORE(placement->offset = false, placement->offset = true);
    const bool has_declaration = placement->has_icb_declaration;
    REJECT_RESTORE(placement->has_icb_declaration = !has_declaration,
                   placement->has_icb_declaration = has_declaration);
    if (has_declaration) {
        REJECT_RESTORE(++placement->icb_declaration.begin, --placement->icb_declaration.begin);
        REJECT_RESTORE(--placement->icb_declaration.end, ++placement->icb_declaration.end);
        const HLSLHullWholeRange declaration = placement->icb_declaration;
        REJECT_RESTORE(placement->icb_declaration = ((HLSLHullWholeRange){0, 1}),
                       placement->icb_declaration = declaration);
        REJECT_RESTORE(entry->coverage.hull_icb.plan.payload[2] ^= 1,
                       entry->coverage.hull_icb.plan.payload[2] ^= 1);
        REJECT_RESTORE(++entry->coverage.hull_icb.recorded_declaration_begin,
                       --entry->coverage.hull_icb.recorded_declaration_begin);
    } else {
        REJECT_RESTORE(placement->icb_declaration.begin = 1, placement->icb_declaration.begin = 0);
        REJECT_RESTORE(placement->icb_declaration.end = 1, placement->icb_declaration.end = 0);
    }
#undef REJECT_RESTORE
    return true;
}

static bool current_mutations(Fixture *fixture, const ShaderLabSourceQualityRequest *request,
                              ShaderLabEmittedHullCoverage *owned) {
    const uint64_t requirements = fixture->programs[HULL_STAGE][0].shader_requirements;
    fixture->programs[HULL_STAGE][0].shader_requirements ^= 1;
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    fixture->programs[HULL_STAGE][0].shader_requirements = requirements;
    CHECK(replay_restored(request, owned));
    SerializedProgramParameters *parameters = &fixture->pass.common_parameters[HULL_STAGE];
    ++parameters->version; /* Dormant typed metadata is still owned and replayed. */
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    --parameters->version;
    CHECK(replay_restored(request, owned));
    if (parameters->cb_count) {
        CHECK(parameters->cb_count == 1);
        const bool has_field = parameters->constant_buffers[0].var_count != 0;
        if (has_field) {
            CHECK(parameters->constant_buffers[0].var_count == 1);
            ++parameters->constant_buffers[0].variables[0].layout[0];
        } else parameters->constant_buffers[0].size = 16;
        CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
        if (has_field) --parameters->constant_buffers[0].variables[0].layout[0];
        else parameters->constant_buffers[0].size = 0;
        CHECK(replay_restored(request, owned));
    }
    fixture->segments[HULL_STAGE][8] ^= 1;
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    fixture->segments[HULL_STAGE][8] ^= 1;
    CHECK(replay_restored(request, owned));
    const SerializedShaderFloatValue culling = fixture->pass.state.culling;
    fixture->pass.state.culling.val = 2; /* Dormant wrapper identity still matters. */
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    fixture->pass.state.culling = culling;
    CHECK(replay_restored(request, owned));
    return true;
}

static bool check_selected_stage(const Fixture *fixture, const ShaderLabEmittedHullCoverage *owned) {
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&fixture->shader, &fixture->pass, &plan, NULL) == SHADERLAB_VARIANT_PLAN_OK);
    StringBuilder source;
    sb_init(&source);
    ShaderLabExpressionSourceMap map = {0};
    ShaderLabExpressionMapContext trace = {.map = &map};
    ShaderLabStageDiagnostic diagnostic;
    CHECK(emit_stage_hlsl_with_variant_plan_mode(&plan, HULL_STAGE,
        fixture->entries, fixture->archive.entry_count, (uint8_t **)fixture->segments,
        fixture->lengths, fixture->archive.segment_count, true, &trace, &source, &diagnostic));
    CHECK(diagnostic.status == SHADERLAB_STAGE_OK && sb_ok(&source) && source.len && map.count == owned->entry_count);
    for (size_t index = 0; index < map.count; ++index) {
        const ShaderLabExpressionSourceRecord *record = &map.records[index];
        const ShaderLabEmittedHullEntry *entry = &owned->entries[index].observation;
        CHECK(record->stage_index == HULL_STAGE && record->subprogram_index == entry->subprogram_index &&
              record->serialized_state == entry->serialized_state && record->has_source_quality &&
              !memcmp(record->target_digest, entry->target_digest, sizeof(entry->target_digest)) &&
              hlsl_source_quality_results_equal(&record->source_quality, &entry->base_quality));
        CHECK(record->instructions.complete && record->instructions.count == owned->entries[index].raw_map.count);
        for (size_t instruction = 0; instruction < record->instructions.count; ++instruction)
            CHECK(hlsl_expression_origin_ranges_valid(&record->instructions.origins[instruction], source.len));
    }
    shaderlab_expression_source_map_free(&map); sb_free(&source); shaderlab_variant_plan_free(&plan);
    return true;
}

static bool check_paired_domain(const Fixture *fixture, const ShaderLabEmittedHullCoverage *owned,
                                const ShaderLabSourceQualityInventory *normal, const char *semantic) {
    size_t domains = 0;
    CHECK(normal->entries.count == owned->inventory.entries.count);
    for (size_t index = 0; index < owned->inventory.entries.count; ++index) {
        const ShaderLabExpressionSourceRecord *record = &owned->inventory.entries.records[index];
        if (record->stage_index != DOMAIN_STAGE) continue;
        ++domains;
        const ShaderLabExpressionSourceRecord *ordinary = &normal->entries.records[index];
        CHECK(ordinary->stage_index == DOMAIN_STAGE && ordinary->subprogram_index == record->subprogram_index &&
              ordinary->serialized_state == record->serialized_state && ordinary->blob_index == record->blob_index &&
              !memcmp(ordinary->target_digest, record->target_digest, sizeof(record->target_digest)) &&
              record->has_source_quality && ordinary->has_source_quality &&
              hlsl_source_quality_results_equal(&ordinary->source_quality, &record->source_quality) &&
              record->source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !record->source_quality.counts.residual_total && !record->source_quality.counts.incomplete_units &&
              !record->source_quality.counts.unknown_provenance && record->source_quality.counts.inspected_units == 1 &&
              record->instructions.complete && ordinary->instructions.complete &&
              record->instructions.count == 5 && ordinary->instructions.count == 5);
        const ShaderLabSourceSyntaxReceipt *body = NULL;
        for (size_t receipt = 0; receipt < owned->inventory.receipt_count; ++receipt) {
            const ShaderLabSourceSyntaxReceipt *candidate = &owned->inventory.receipts[receipt];
            if (candidate->kind != SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY || candidate->entry_record_index != index) continue;
            CHECK(!body); body = candidate;
        }
        CHECK(body && body->stage_index == DOMAIN_STAGE && body->source_begin < body->source_end &&
              body->source_end <= owned->source.len && record->blob_index >= 0 &&
              record->blob_index < fixture->archive.segment_count);

        ByteStream stream;
        stream_init(&stream, fixture->segments[record->blob_index], (size_t)fixture->lengths[record->blob_index]);
        stream_set_endian(&stream, false);
        PlayerSubProgramMetadata player = {0};
        CHECK(subprogram_metadata_parse_variant(&stream, &player));
        DXBCDocument document;
        DXBCContainer decoded = {0};
        DXBCStageContract contract;
        USILProgram program;
        dxbc_document_init(&document); dxbc_stage_contract_init(&contract);
        CHECK(dxbc_document_parse(&document, player.bytecode, player.bytecode_length, NULL) &&
              dxbc_document_decode_semantic(&document, &decoded) &&
              dxbc_stage_contract_decode(&document, &decoded, &contract, NULL) &&
              usil_translate_with_stage_contract(&program, &decoded, &contract));
        CHECK(usil_signature_authority_is_valid(&program) && program.instruction_count == 5 &&
              program.tessellation.domain == DXBC_TESSELLATOR_DOMAIN_TRIANGLE &&
              program.tessellation.input_control_point_count == 3 &&
              program.input_count == 1 && program.inputs[0].mask == 7 && program.inputs[0].rw_mask == 7 &&
              !program.inputs[0].system_value && !program.inputs[0].semantic_index &&
              !strcmp(dxbc_signature_semantic_name(&program.inputs[0]), semantic) &&
              program.output_count == 1 && program.outputs[0].mask == 15 && !program.outputs[0].rw_mask &&
              program.outputs[0].system_value == 1);
        uint8_t target_digest[32];
        common_sha256(player.bytecode, player.bytecode_length, target_digest);
        CHECK(!memcmp(target_digest, record->target_digest, sizeof(target_digest)));
        const HLSLEmitNames names = {.entry_point = "ds", .input_struct = "unused_input", .output_struct = "unused_output"};
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLSourceQualityResult quality;
        HLSLExpressionSourceMap map = {0};
        options.source_quality = &quality; options.expression_source_map = &map;
        options.source_quality_pass_index = (uint32_t)record->pass_index;
        options.source_quality_entry_point_index = (uint32_t)record->subprogram_index;
        options.omit_unity_builtin_declarations = true;
        options.reserved_preprocessor_identifiers = (const char *const *)fixture->shader.keyword_names.keywords;
        options.reserved_preprocessor_identifier_count = (size_t)fixture->shader.keyword_names.count;
        const SerializedProgramParameters *parameters = &fixture->pass.common_parameters[DOMAIN_STAGE];
        HLSLStageCoverage coverage = {0};
        StringBuilder local, indented;
        sb_init(&local); sb_init(&indented);
        CHECK(hlsl_emit_with_stage_coverage(&program, &local, parameters, parameters, &names, &options, &coverage, NULL) &&
              hlsl_stage_coverage_validate(&coverage, &local) &&
              hlsl_source_quality_results_equal(&quality, &record->source_quality) &&
              map.complete && map.count == 5 && coverage.domain_output.plan.present &&
              coverage.domain_output.return_emitted && coverage.domain_output.root_captured);
        size_t position = 0;
        while (position < local.len) {
            sb_append(&indented, "            ");
            do sb_append_char(&indented, local.buf[position++]);
            while (position < local.len && local.buf[position - 1] != '\n');
        }
        CHECK(sb_ok(&indented) && indented.len == body->source_end - body->source_begin &&
              !memcmp(indented.buf, owned->source.buf + body->source_begin, indented.len) &&
              strstr(indented.buf, "return float4(") && strstr(indented.buf, semantic) &&
              !strstr(indented.buf, ".xyzx") && !strstr(indented.buf, ".yyyy") && !strstr(indented.buf, ".zzzz"));
        for (size_t instruction = 0; instruction < map.count; ++instruction) {
            HLSLExpressionOrigin expected = map.origins[instruction];
            CHECK(!expected.definition_begin && !expected.definition_end &&
                  expected.source_instruction_index == 7 + instruction);
            expected.source_begin = whole_coordinate(&coverage, body->source_begin, expected.source_begin, false);
            expected.source_end = whole_coordinate(&coverage, body->source_begin, expected.source_end, true);
            CHECK(hlsl_expression_origins_equal(&expected, &record->instructions.origins[instruction]) &&
                  hlsl_expression_origins_equal(&ordinary->instructions.origins[instruction],
                                                &record->instructions.origins[instruction]) &&
                  hlsl_expression_origin_ranges_valid(&expected, owned->source.len));
        }
        const HLSLStageOwnedRoot *constructor = &coverage.roots[coverage.domain_output.root_index];
        const size_t constructor_begin = whole_coordinate(&coverage, body->source_begin, constructor->begin, false);
        const size_t constructor_end = whole_coordinate(&coverage, body->source_begin, constructor->end, true);
        const HLSLExpressionOrigin *ret = &record->instructions.origins[4];
        CHECK(constructor->owner.kind == HLSL_STAGE_ROOT_DOMAIN_OUTPUT_CONSTRUCTION && constructor->instruction == -1 &&
              ret->kind == HLSL_EXPRESSION_ORIGIN_RETURN && ret->source_begin < constructor_begin &&
              constructor_begin < constructor_end && constructor_end < ret->source_end && ret->source_end <= body->source_end &&
              record->instructions.origins[2].destination_lanes == 7 && record->instructions.origins[3].destination_lanes == 8 &&
              !memcmp(owned->source.buf + ret->source_begin, "return ", strlen("return ")) &&
              !memcmp(owned->source.buf + constructor_begin, "float4(", strlen("float4(")));
        usil_free(&program); dxbc_stage_contract_free(&contract); dxbc_free(&decoded);
        dxbc_document_free(&document); subprogram_metadata_free_variant(&player);
        CHECK(hlsl_stage_coverage_validate(&coverage, &local));
        hlsl_stage_coverage_dispose(&coverage); sb_free(&local); sb_free(&indented);
    }
    CHECK(domains == owned->entry_count);
    return true;
}

static bool paired_domain_mutations(Fixture *fixture, const ShaderLabSourceQualityRequest *request,
                                   ShaderLabEmittedHullCoverage *owned, const char *semantic) {
    size_t ordinal = SIZE_MAX;
    for (size_t index = 0; index < owned->inventory.entries.count; ++index)
        if (owned->inventory.entries.records[index].stage_index == DOMAIN_STAGE) { ordinal = index; break; }
    CHECK(ordinal != SIZE_MAX);
    ShaderLabExpressionSourceRecord *record = &owned->inventory.entries.records[ordinal];
    const HLSLExpressionOrigin *literal = &record->instructions.origins[3];
    CHECK(literal->source_begin < literal->source_end && literal->source_end <= owned->source.len);
    owned->source.buf[literal->source_begin] ^= 1;
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    owned->source.buf[literal->source_begin] ^= 1;
    CHECK(replay_restored(request, owned));
    ++record->instructions.origins[3].source_instruction_index;
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    --record->instructions.origins[3].source_instruction_index;
    CHECK(replay_restored(request, owned));
    fixture->programs[DOMAIN_STAGE][0].shader_requirements ^= 1;
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    fixture->programs[DOMAIN_STAGE][0].shader_requirements ^= 1;
    CHECK(replay_restored(request, owned));
    /* Empty DOMAIN metadata is a complete owned input only in paired mode;
     * ordinary HULL capture retains the sibling's modeled source facts. */
    if (owned->capture_domain) {
        ++fixture->pass.common_parameters[DOMAIN_STAGE].version;
        CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
        --fixture->pass.common_parameters[DOMAIN_STAGE].version;
        CHECK(replay_restored(request, owned));
    }

    /* The changed sibling is independently supported, but differs from this
     * receipt. No new cross-stage semantic mismatch gate is inferred here. */
    const int blob = fixture->programs[DOMAIN_STAGE][0].blob_index;
    uint8_t *saved_segment = fixture->segments[blob];
    const int saved_length = fixture->lengths[blob];
    const BlobEntry saved_entry = fixture->entries[blob];
    size_t size = 0;
    uint8_t *different = test_tessellation_domain_float3_dxbc(3,
        !strcmp(semantic, "OBJECTCOORD") ? "POINTVALUE" : "OBJECTCOORD", TEST_DOMAIN_FLOAT3_VALID, &size);
    CHECK(different && fixture_blob(fixture, blob, different, size, 22, NULL));
    free(different);
    CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned));
    free(fixture->segments[blob]); fixture->segments[blob] = saved_segment;
    fixture->lengths[blob] = saved_length; fixture->entries[blob] = saved_entry;
    CHECK(replay_restored(request, owned));
    return true;
}

static bool positive_capture_shape(bool control_point, bool scalar, bool two_states, bool icb,
                                   EmptyRoute route, bool renamed, const char *semantic) {
    Fixture fixture, replacement;
    CHECK(!semantic || route == EMPTY_ROUTE_ABSENT);
    CHECK(semantic ? fixture_init_float3_shape(&fixture, scalar, icb, semantic, control_point) :
          icb ? fixture_init_icb(&fixture, scalar) : fixture_init(&fixture, control_point, scalar));
    CHECK(!renamed || scalar);
    CHECK(fixture_scalar_names(&fixture, renamed));
    if (two_states) CHECK(fixture_second_state(&fixture, false));
    CHECK(route == EMPTY_ROUTE_ABSENT || (!scalar && two_states));
    CHECK(fixture_empty_route(&fixture, route));
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    ShaderLabEmittedHullCoverage *owned = NULL, *independent = NULL;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &owned) == SHADERLAB_HULL_COVERAGE_OK && owned);
    CHECK(shaderlab_emitted_hull_coverage_replay(&request, owned));
    ShaderLabEmittedHullSummary summary;
    CHECK(shaderlab_emitted_hull_coverage_describe(owned, &summary));
    CHECK(summary.entry_count == (two_states ? 2u : 1u) &&
          summary.linked_entry_count == (two_states ? 8u : 4u) &&
          summary.unit_count == summary.entry_count * 3 &&
          summary.base_quality.classification == HLSL_SOURCE_QUALITY_MIXED);
    CHECK(summary.base_quality.gaps & SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE);
    CHECK(summary.base_quality.gaps & SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY);
    const char *source = NULL;
    size_t source_size = 0;
    CHECK(shaderlab_emitted_hull_coverage_source(owned, &source, &source_size));
    CHECK(source_size == summary.source_size && source_size == owned->source.len);
    StringBuilder normal;
    sb_init(&normal);
    ShaderLabSourceQualityInventory normal_inventory = {0};
    CHECK(shaderlab_source_quality_emit(&request, &normal, &normal_inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(normal.len == source_size && !memcmp(normal.buf, source, source_size + 1));
    ShaderLabSourceQualityResult normal_result, owned_result;
    CHECK(shaderlab_source_quality_inventory_analyze(&request, &normal, &normal_inventory,
        &normal_result, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(shaderlab_source_quality_inventory_analyze(&request, &owned->source, &owned->inventory,
        &owned_result, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(normal_result.gaps == owned_result.gaps &&
          normal_result.classification == owned_result.classification &&
          normal_result.observed_stage_incomplete_units == owned_result.observed_stage_incomplete_units);
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &independent) == SHADERLAB_HULL_COVERAGE_OK);
    if (semantic) CHECK(check_paired_domain(&fixture, owned, &normal_inventory, semantic));
    if (control_point || scalar || route != EMPTY_ROUTE_ABSENT) CHECK(check_selected_stage(&fixture, owned));
    for (size_t index = 0; index < owned->entry_count; ++index) {
        HLSLHullCoverageCapture *entry = &owned->entries[index];
        ShaderLabEmittedHullEntry observation;
        CHECK(shaderlab_emitted_hull_coverage_entry(owned, index, &observation));
        CHECK(observation.stage_index == HULL_STAGE && observation.entry_record_index != index &&
              observation.entry_record_index < owned->inventory.entries.count && observation.unit_count == 3);
        CHECK((observation.obligations & (HLSL_STAGE_COVERAGE_BODY | HLSL_STAGE_COVERAGE_LOCAL_DECLARATION)) ==
              (HLSL_STAGE_COVERAGE_BODY | HLSL_STAGE_COVERAGE_LOCAL_DECLARATION));
        CHECK(hlsl_source_quality_results_equal(&observation.base_quality,
              &normal_inventory.entries.records[observation.entry_record_index].source_quality));
        CHECK(hlsl_stage_coverage_equal(&entry->coverage, &independent->entries[index].coverage));
        if (semantic) {
            const HLSLStageHullContract *contract = &entry->coverage.hull_contract;
            CHECK(contract->input.mask == 7 && contract->input.rw_mask == 7 &&
                  contract->output.mask == 7 && contract->output.rw_mask == 8 &&
                  !contract->input.system_value && !contract->output.system_value &&
                  !strcmp(dxbc_signature_semantic_name(&contract->input), semantic) &&
                  !strcmp(dxbc_signature_semantic_name(&contract->output), semantic) &&
                  contract->tessellation.input_control_point_count == 3 &&
                  contract->tessellation.output_control_point_count == 3 &&
                  strstr(entry->coverage.source, "float3 "));
            if (control_point) {
                CHECK(contract->tessellation.phase_count == 3 &&
                      contract->phases[0].kind == DXBC_HULL_PHASE_CONTROL_POINT &&
                      observation.base_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                      !observation.base_quality.reasons &&
                      observation.base_quality.counts.inspected_units == 3 &&
                      !observation.base_quality.counts.incomplete_units &&
                      !observation.base_quality.counts.residual_total &&
                      !observation.base_quality.counts.unknown_provenance &&
                      strstr(entry->coverage.source, "controlPoint.pointValue =") &&
                      strstr(entry->coverage.source, "patch[pointIndex].pointValue") &&
                      !strstr(entry->coverage.source, "return patch[pointIndex];"));
                const int output = contract->phases[0].end_instruction_index - 2;
                CHECK(output >= 0 && (size_t)output < entry->raw_map.count &&
                      entry->raw_map.origins[output].destination_lanes == 7 &&
                      entry->raw_map.origins[output].source_instruction_index ==
                          entry->coverage.source_instructions[output]);
            }
        }
        CHECK(entry->inputs.target != independent->entries[index].inputs.target &&
              entry->inputs.player_payload != independent->entries[index].inputs.player_payload);
        const bool parsed = route >= EMPTY_ROUTE_PARSED_ZERO;
        const bool current_shell = route == EMPTY_ROUTE_COMMON || route == EMPTY_ROUTE_EXPLICIT_ABSENCE ||
            route == EMPTY_ROUTE_PARSED_GLOBALS ||
            (route == EMPTY_ROUTE_MIXED_PARSED && observation.subprogram_index == 1);
        CHECK(entry->inputs.current.cb_count == ((scalar || current_shell) ? 1 : 0) &&
              entry->inputs.common.cb_count == ((scalar || route != EMPTY_ROUTE_ABSENT) ? 1 : 0));
        if (entry->inputs.current.cb_count)
            CHECK(entry->inputs.current.constant_buffers != entry->inputs.common.constant_buffers);
        if (scalar) {
            const SerializedConstantBuffer *current = entry->inputs.current.constant_buffers;
            const SerializedConstantBuffer *common = entry->inputs.common.constant_buffers;
            CHECK(current->name != common->name && current->variables != common->variables &&
                  current->variables[0].name != common->variables[0].name &&
                  common != fixture.pass.common_parameters[HULL_STAGE].constant_buffers &&
                  common->name != fixture.pass.common_parameters[HULL_STAGE].constant_buffers[0].name);
            CHECK(!strcmp(current->name, renamed ? "PatchInputs" : "FactorInputs") &&
                  !strcmp(current->variables[0].name, renamed ? "PatchScale" : "_Factor") &&
                  !strcmp(current->name, common->name) &&
                  !strcmp(current->variables[0].name, common->variables[0].name) &&
                  current->size == 16 && common->size == 16 &&
                  current->variables[0].layout[0] == 0 && common->variables[0].layout[0] == 0);
            char declaration[256];
            const int length = snprintf(declaration, sizeof(declaration),
                "cbuffer %s : register(b0) {\n    float %s;\n};\n", current->name, current->variables[0].name);
            CHECK(length > 0 && (size_t)length < sizeof(declaration) &&
                  strstr(entry->coverage.source, declaration) &&
                  !strstr(entry->coverage.source, "packoffset(") &&
                  !strstr(entry->coverage.source, "_pad") && !strstr(entry->coverage.source, "cb0_"));
            CHECK(observation.base_quality.counts.cbuffer_declarations == 1 &&
                  observation.base_quality.counts.cbuffer_fields == 1 &&
                  !observation.base_quality.counts.unknown_provenance);
            if (!icb) CHECK(observation.base_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                            !observation.base_quality.counts.residual_total &&
                            !observation.base_quality.counts.incomplete_units &&
                            observation.base_quality.counts.inspected_units == 3);
        }
        if (route != EMPTY_ROUTE_ABSENT) {
            CHECK(entry->inputs.current.is_binary == parsed && !entry->inputs.common.is_binary &&
                  entry->inputs.current.version == (parsed ? UNITY_2021_3_PLAYER_BLOB_VERSION : 0) &&
                  entry->inputs.current.dialect == (parsed ? PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1 : PLAYER_BLOB_DIALECT_INVALID));
            const SerializedConstantBuffer *common = entry->inputs.common.constant_buffers;
            CHECK(common != fixture.pass.common_parameters[HULL_STAGE].constant_buffers &&
                  common->name != fixture.pass.common_parameters[HULL_STAGE].constant_buffers[0].name &&
                  common->role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS && !strcmp(common->name, "$Globals") &&
                  !common->size && !common->var_count && !common->struct_count);
            if (current_shell) CHECK(entry->inputs.current.constant_buffers[0].name != common->name &&
                                    entry->inputs.current.constant_buffers[0].role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS);
        }
        CHECK(entry->coverage.hull_icb.plan.present == icb);
        if (icb) {
            CHECK(entry->coverage.hull_icb.plan.row_count == 3 &&
                  entry->coverage.hull_icb.literal_root_count == 3 && entry->coverage.hull_icb.access_root_count == 1);
            CHECK(entry->observation.base_quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
                  entry->observation.base_quality.counts.incomplete_units == 1);
        }
        CHECK(check_placements(owned, entry));
        unsigned clamps = 0;
        for (size_t instruction = 0; instruction < entry->raw_map.count; ++instruction)
            if (entry->raw_map.origins[instruction].kind == HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP) ++clamps;
        /* The ICB-fed outer MIN stays a readable expression. Existing clamp
         * lowering still owns the scalar-buffer inner MIN through the stage
         * attribute, without extending that lowering to ICB operands. */
        CHECK(clamps == (scalar ? (icb ? 1u : 2u) : 0u));
        if (scalar && icb) CHECK(strstr(entry->coverage.source, "min("));
    }
    /* The mutation helper deliberately targets the first ordinary selected
     * row; the multi-state fixture separately exercises complete denominators. */
    if (!two_states || route != EMPTY_ROUTE_ABSENT) CHECK(owned_mutations(&request, owned));
    CHECK(current_mutations(&fixture, &request, owned));
    if (semantic) CHECK(paired_domain_mutations(&fixture, &request, owned, semantic));
    ShaderLabEmittedHullCoverage *same = owned;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &same) == SHADERLAB_HULL_COVERAGE_INVALID_ARGUMENT && same == owned);
    CHECK(!shaderlab_emitted_hull_coverage_entry(owned, owned->entry_count, &(ShaderLabEmittedHullEntry){0}));

    CHECK(semantic ? fixture_init_float3_shape(&replacement, scalar, icb, semantic, control_point) :
          icb ? fixture_init_icb(&replacement, scalar) : fixture_init(&replacement, control_point, scalar));
    CHECK(fixture_scalar_names(&replacement, renamed));
    if (two_states) CHECK(fixture_second_state(&replacement, false));
    CHECK(fixture_empty_route(&replacement, route));
    fixture_dispose(&fixture);
    request.shader = &replacement.shader;
    request.archive = &replacement.archive;
    CHECK(shaderlab_emitted_hull_coverage_replay(&request, owned));
    CHECK(shaderlab_emitted_hull_coverage_describe(owned, &summary));
    CHECK(shaderlab_emitted_hull_coverage_source(owned, &source, &source_size));
    CHECK(source_size == independent->source.len && !memcmp(source, independent->source.buf, source_size + 1));
    for (size_t index = 0; index < owned->entry_count; ++index) {
        StringBuilder local = local_source(&owned->entries[index].coverage);
        CHECK(hlsl_stage_coverage_validate(&owned->entries[index].coverage, &local));
        CHECK(hlsl_stage_coverage_equal(&owned->entries[index].coverage, &independent->entries[index].coverage));
    }
    shaderlab_source_quality_inventory_dispose(&normal_inventory);
    sb_free(&normal);
    shaderlab_emitted_hull_coverage_free(independent);
    shaderlab_emitted_hull_coverage_free(owned);
    fixture_dispose(&replacement);
    return true;
}

static bool positive_capture_names(bool control_point, bool scalar, bool two_states, bool icb,
                                   EmptyRoute route, bool renamed) {
    return positive_capture_shape(control_point, scalar, two_states, icb, route, renamed, NULL);
}

static bool positive_capture(bool control_point, bool scalar, bool two_states, bool icb, EmptyRoute route) {
    return positive_capture_names(control_point, scalar, two_states, icb, route, false);
}

static bool paired_fixture(Fixture *fixture, bool float3, bool scalar, bool icb, bool asymmetric) {
    CHECK(float3 ? fixture_init_float3(fixture, scalar, icb, "POINTVALUE") : fixture_init(fixture, false, false));
    CHECK(fixture_second_state(fixture, false));
    /* Keep an actual stage-only FEATURE axis in HULL. Partial sharing across
     * V/F/HULL without DOMAIN would fail the existing pragma-coherence gate. */
    if (asymmetric) {
        fixture->pass.subprogram_count[0] = fixture->pass.subprogram_count[1] = 1;
        fixture->pass.subprogram_count[DOMAIN_STAGE] = 1;
        ShaderLabVariantPlan plan;
        shaderlab_variant_plan_init(&plan);
        CHECK(shaderlab_variant_plan_build(&fixture->shader, &fixture->pass, &plan, NULL) == SHADERLAB_VARIANT_PLAN_OK &&
              plan.stages[HULL_STAGE].state_count == 2 && plan.stages[HULL_STAGE].generated_state_count == 2 &&
              plan.stages[HULL_STAGE].axis_count == 1 && plan.stages[DOMAIN_STAGE].state_count == 1 &&
              plan.stages[DOMAIN_STAGE].generated_state_count == 1 && !plan.stages[DOMAIN_STAGE].axis_count);
        shaderlab_variant_plan_free(&plan);
    }
    return true;
}

static bool paired_owned_domain_mutations(const ShaderLabSourceQualityRequest *request,
                                         ShaderLabEmittedHullCoverage *owned, HLSLHullCoverageCapture *entry) {
    HLSLStageCoverage *coverage = &entry->coverage;
    HLSLHullWholePlacement *placement = &entry->placement;
#define REJECT_RESTORE(change, restore) do { \
    change; CHECK(!shaderlab_emitted_hull_coverage_replay(request, owned)); \
    restore; CHECK(replay_restored(request, owned)); \
} while (0)
    REJECT_RESTORE(owned->capture_domain = false, owned->capture_domain = true);
    REJECT_RESTORE(entry->inputs.target[4] ^= 1, entry->inputs.target[4] ^= 1);
    REJECT_RESTORE(entry->inputs.player_payload[8] ^= 1, entry->inputs.player_payload[8] ^= 1);
    REJECT_RESTORE(entry->inputs.current.version ^= 1, entry->inputs.current.version ^= 1);
    REJECT_RESTORE(entry->inputs.common.version ^= 1, entry->inputs.common.version ^= 1);
    REJECT_RESTORE(coverage->domain_owner_digest[0] ^= 1, coverage->domain_owner_digest[0] ^= 1);
    REJECT_RESTORE(coverage->recorded_domain_owner_digest[0] ^= 1, coverage->recorded_domain_owner_digest[0] ^= 1);
    REJECT_RESTORE(++entry->raw_map.origins[0].source_instruction_index,
                   --entry->raw_map.origins[0].source_instruction_index);
    REJECT_RESTORE(++placement->roots[0].begin, --placement->roots[0].begin);
    REJECT_RESTORE(--placement->units[0].end, ++placement->units[0].end);
    REJECT_RESTORE(++placement->syntax_ends[0], --placement->syntax_ends[0]);
    const size_t actual_ordinal = entry->observation.entry_record_index;
    REJECT_RESTORE(entry->observation.entry_record_index = owned->entries[0].observation.entry_record_index,
                   entry->observation.entry_record_index = actual_ordinal);
    const bool has_return = placement->has_domain_return;
    REJECT_RESTORE(placement->has_domain_return = !has_return, placement->has_domain_return = has_return);
    if (has_return) {
        REJECT_RESTORE(++placement->domain_return.begin, --placement->domain_return.begin);
        REJECT_RESTORE(--placement->domain_return.end, ++placement->domain_return.end);
        const HLSLHullWholeRange range = placement->domain_return;
        REJECT_RESTORE(placement->domain_return = ((HLSLHullWholeRange){0, 1}), placement->domain_return = range);
        REJECT_RESTORE(++coverage->domain_output.recorded_return_begin, --coverage->domain_output.recorded_return_begin);
        REJECT_RESTORE(coverage->domain_output.plan.pieces[1].immediate_bits ^= 1,
                       coverage->domain_output.plan.pieces[1].immediate_bits ^= 1);
        HLSLStageOwnedRoot *constructor = &coverage->roots[coverage->domain_output.root_index];
        REJECT_RESTORE(constructor->tree->logical_origin.instruction_index = 3,
                       constructor->tree->logical_origin.instruction_index = -1);
        REJECT_RESTORE(++placement->roots[coverage->domain_output.root_index].end,
                       --placement->roots[coverage->domain_output.root_index].end);
    } else {
        REJECT_RESTORE(placement->domain_return.begin = 1, placement->domain_return.begin = 0);
        REJECT_RESTORE(placement->domain_return.end = 1, placement->domain_return.end = 0);
    }
#undef REJECT_RESTORE
    return true;
}

typedef struct {
    Fixture *fixture;
    bool mutate, called;
} DomainObserver;

static bool observe_paired_domain(void *context, const ShaderLabSourceSyntaxReceipt *receipt) {
    DomainObserver *observer = context;
    if (receipt->stage_index != DOMAIN_STAGE || receipt->kind != SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY || observer->called)
        return true;
    observer->called = true;
    if (!observer->mutate) return false;
    observer->fixture->programs[DOMAIN_STAGE][0].shader_requirements ^= 1;
    return true;
}

static bool paired_capture(bool float3, bool scalar, bool icb, bool asymmetric) {
    Fixture fixture, replacement;
    CHECK(paired_fixture(&fixture, float3, scalar, icb, asymmetric));
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    ShaderLabEmittedHullCoverage *ordinary = NULL, *owned = NULL, *independent = NULL;
    const ShaderLabHullCoverageStatus ordinary_status = shaderlab_emitted_hull_coverage_capture(&request, &ordinary);
    const ShaderLabHullCoverageStatus paired_status = shaderlab_emitted_hull_coverage_capture_paired(&request, &owned);
    const ShaderLabHullCoverageStatus independent_status = shaderlab_emitted_hull_coverage_capture_paired(&request, &independent);
    const bool ordinary_replay = ordinary && shaderlab_emitted_hull_coverage_replay(&request, ordinary);
    const bool paired_replay = owned && shaderlab_emitted_hull_coverage_replay(&request, owned);
    if (ordinary_status != SHADERLAB_HULL_COVERAGE_OK || paired_status != SHADERLAB_HULL_COVERAGE_OK ||
        independent_status != SHADERLAB_HULL_COVERAGE_OK || !ordinary_replay || !paired_replay)
        fprintf(stderr, "Paired factory failed: float3=%d scalar=%d icb=%d asymmetric=%d "
            "ordinary_status=%d paired_status=%d independent_status=%d ordinary_replay=%d paired_replay=%d\n",
            float3, scalar, icb, asymmetric, ordinary_status, paired_status, independent_status, ordinary_replay, paired_replay);
    CHECK(ordinary_status == SHADERLAB_HULL_COVERAGE_OK && paired_status == SHADERLAB_HULL_COVERAGE_OK &&
          independent_status == SHADERLAB_HULL_COVERAGE_OK && ordinary_replay && paired_replay);
    ShaderLabEmittedHullSummary normal_summary, summary;
    CHECK(shaderlab_emitted_hull_coverage_describe(ordinary, &normal_summary) &&
          shaderlab_emitted_hull_coverage_describe(owned, &summary));
    const size_t domains = asymmetric ? 1u : 2u;
    CHECK(normal_summary.entry_count == 2 && !normal_summary.domain_entry_count && !normal_summary.domain_unit_count &&
          !normal_summary.domain_root_count && !normal_summary.domain_syntax_count &&
          summary.entry_count == 2 && summary.unit_count == 6 && summary.domain_entry_count == domains &&
          summary.domain_unit_count == domains && summary.domain_root_count && summary.domain_syntax_count &&
          summary.linked_entry_count == (asymmetric ? 5u : 8u) && summary.root_count == normal_summary.root_count &&
          summary.syntax_count == normal_summary.syntax_count && summary.base_quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
          summary.base_quality.gaps == normal_summary.base_quality.gaps &&
          (summary.base_quality.gaps & (SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE | SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY)) ==
              (SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE | SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY) &&
          !memcmp(summary.source_digest, normal_summary.source_digest, sizeof(summary.source_digest)) &&
          !memcmp(summary.modeled_input_digest, normal_summary.modeled_input_digest, sizeof(summary.modeled_input_digest)) &&
          owned->capture_domain && owned->entry_count == 2 + domains &&
          owned->source.len == ordinary->source.len && owned->source.len == independent->source.len &&
          !memcmp(owned->source.buf, ordinary->source.buf, owned->source.len + 1) &&
          !memcmp(owned->source.buf, independent->source.buf, owned->source.len + 1));
    ShaderLabEmittedHullEntry description = {.stage_index = -1};
    CHECK(!shaderlab_emitted_hull_coverage_domain_entry(ordinary, 0, &description) && description.stage_index == -1);
    size_t hull_ordinal = 0, domain_ordinal = 0, domain_roots = 0, domain_syntax = 0;
    HLSLHullCoverageCapture *first_domain = NULL;
    for (size_t index = 0; index < owned->entry_count; ++index) {
        HLSLHullCoverageCapture *entry = &owned->entries[index];
        HLSLHullCoverageCapture *other = &independent->entries[index];
        const bool domain = entry->observation.stage_index == DOMAIN_STAGE;
        CHECK(domain || entry->observation.stage_index == HULL_STAGE);
        CHECK(domain ? shaderlab_emitted_hull_coverage_domain_entry(owned, domain_ordinal++, &description) :
              shaderlab_emitted_hull_coverage_entry(owned, hull_ordinal++, &description));
        CHECK(description.stage_index == entry->observation.stage_index &&
              description.entry_record_index == entry->observation.entry_record_index &&
              description.entry_record_index != index && description.entry_record_index < owned->inventory.entries.count &&
              entry->inputs.target != other->inputs.target && entry->inputs.player_payload != other->inputs.player_payload &&
              hlsl_owned_stage_inputs_equal(&entry->inputs, &other->inputs) &&
              hlsl_stage_coverage_equal(&entry->coverage, &other->coverage) && check_placements(owned, entry));
        if (!domain) continue;
        if (!first_domain) first_domain = entry;
        domain_roots += description.root_count; domain_syntax += description.syntax_count;
        CHECK(description.unit_count == 1 && description.base_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !description.base_quality.counts.incomplete_units && !description.base_quality.counts.residual_total &&
              !description.base_quality.counts.unknown_provenance &&
              hlsl_source_quality_results_equal(&description.base_quality,
                  &ordinary->inventory.entries.records[description.entry_record_index].source_quality) &&
              entry->coverage.stage == DXBC_PROGRAM_TYPE_DOMAIN &&
              entry->coverage.schema == HLSL_STAGE_COVERAGE_ORDINARY_ENTRY &&
              entry->coverage.domain_output.plan.present == float3 && entry->placement.has_domain_return == float3);
        if (float3) {
            const HLSLStageOwnedRoot *constructor = &entry->coverage.roots[entry->coverage.domain_output.root_index];
            const HLSLHullWholeRange whole = entry->placement.roots[entry->coverage.domain_output.root_index];
            CHECK(constructor->owner.kind == HLSL_STAGE_ROOT_DOMAIN_OUTPUT_CONSTRUCTION && constructor->instruction == -1 &&
                  entry->coverage.domain_output.plan.pieces[0].mask == 7 &&
                  entry->coverage.domain_output.plan.pieces[1].mask == 8 &&
                  entry->coverage.domain_output.plan.pieces[1].immediate_bits == UINT32_C(0x3f800000) &&
                  whole.begin < whole.end && whole.begin > entry->placement.domain_return.begin &&
                  whole.end < entry->placement.domain_return.end &&
                  !memcmp(owned->source.buf + whole.begin, "float4(", strlen("float4(")));
        } else CHECK(hlsl_stage_coverage_domain_output_empty(&entry->coverage.domain_output));
    }
    CHECK(hull_ordinal == 2 && domain_ordinal == domains && domain_roots == summary.domain_root_count &&
          domain_syntax == summary.domain_syntax_count && first_domain);
    CHECK(!shaderlab_emitted_hull_coverage_entry(owned, summary.entry_count, &description) &&
          !shaderlab_emitted_hull_coverage_domain_entry(owned, domains, &description));
    CHECK(paired_owned_domain_mutations(&request, owned, first_domain));
    if (float3) CHECK(paired_domain_mutations(&fixture, &request, owned, "POINTVALUE"));
    for (unsigned mutation = 0; mutation < 2; ++mutation) {
        DomainObserver observer = {.fixture = &fixture, .mutate = mutation != 0};
        request.observer = observe_paired_domain; request.observer_context = &observer;
        ShaderLabEmittedHullCoverage *rejected = NULL;
        CHECK(shaderlab_emitted_hull_coverage_capture_paired(&request, &rejected) != SHADERLAB_HULL_COVERAGE_OK &&
              !rejected && observer.called);
        if (observer.mutate) fixture.programs[DOMAIN_STAGE][0].shader_requirements ^= 1;
        request.observer = NULL; request.observer_context = NULL;
        CHECK(replay_restored(&request, owned));
    }
    ShaderLabEmittedHullCoverage *same = owned;
    CHECK(shaderlab_emitted_hull_coverage_capture_paired(&request, &same) == SHADERLAB_HULL_COVERAGE_INVALID_ARGUMENT && same == owned);
    CHECK(paired_fixture(&replacement, float3, scalar, icb, asymmetric));
    fixture_dispose(&fixture);
    request.shader = &replacement.shader; request.archive = &replacement.archive;
    CHECK(shaderlab_emitted_hull_coverage_replay(&request, owned) &&
          shaderlab_emitted_hull_coverage_replay(&request, ordinary));
    for (size_t index = 0; index < owned->entry_count; ++index) {
        StringBuilder local = local_source(&owned->entries[index].coverage);
        CHECK(hlsl_stage_coverage_validate(&owned->entries[index].coverage, &local) &&
              hlsl_stage_coverage_equal(&owned->entries[index].coverage, &independent->entries[index].coverage));
    }
    shaderlab_emitted_hull_coverage_free(independent); shaderlab_emitted_hull_coverage_free(owned);
    shaderlab_emitted_hull_coverage_free(ordinary); fixture_dispose(&replacement);
    return true;
}

typedef struct {
    Fixture *fixture;
    ShaderLabSourceSyntaxKind rejected_kind;
    bool mutate, mutate_parameters, changed;
} Observer;

static bool observe_receipt(void *context, const ShaderLabSourceSyntaxReceipt *receipt) {
    Observer *observer = context;
    if (receipt->stage_index != HULL_STAGE || receipt->kind != observer->rejected_kind) return true;
    if (!observer->mutate) return false;
    if (observer->mutate_parameters)
        observer->fixture->pass.common_parameters[HULL_STAGE].constant_buffers[0].size += 16;
    else observer->fixture->pass.state.culling.val += 1;
    observer->changed = true;
    return true;
}

static bool route_rejected(Fixture *fixture, ShaderLabStageStatus expected) {
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&fixture->shader, &fixture->pass, &plan, NULL) == SHADERLAB_VARIANT_PLAN_OK);
    StringBuilder stage;
    sb_init(&stage);
    ShaderLabStageDiagnostic diagnostic;
    CHECK(!emit_stage_hlsl_with_variant_plan_mode(&plan, HULL_STAGE,
        fixture->entries, fixture->archive.entry_count, fixture->segments,
        fixture->lengths, fixture->archive.segment_count, true, NULL, &stage, &diagnostic));
    CHECK(diagnostic.status == expected && !stage.len);
    sb_free(&stage); shaderlab_variant_plan_free(&plan);
    ShaderLabSourceQualityRequest request = {.shader = &fixture->shader, .archive = &fixture->archive};
    StringBuilder source;
    sb_init(&source);
    ShaderLabSourceQualityInventory inventory = {0};
    CHECK(shaderlab_source_quality_emit(&request, &source, &inventory, NULL) != SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(!inventory.complete && !source.len);
    shaderlab_source_quality_inventory_dispose(&inventory); sb_free(&source);
    ShaderLabEmittedHullCoverage *rejected = NULL;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) != SHADERLAB_HULL_COVERAGE_OK && !rejected);
    return true;
}

static bool empty_route_rejections(bool icb) {
    Fixture fixture;
    CHECK(icb ? fixture_init_icb(&fixture, false) : fixture_init(&fixture, false, false));
    CHECK(fixture_second_state(&fixture, false));
    CHECK(fixture_empty_route(&fixture, EMPTY_ROUTE_MIXED_PARSED));
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    ShaderLabEmittedHullCoverage *owned = NULL;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &owned) == SHADERLAB_HULL_COVERAGE_OK);
    for (unsigned state = 0; state < 2; ++state) {
        const int index = fixture.parameter_indices[state];
        fixture.parameter_indices[state] = -2;
        CHECK(route_rejected(&fixture, SHADERLAB_STAGE_INVALID_PARAMETER_BLOB));
        fixture.parameter_indices[state] = index;
        CHECK(replay_restored(&request, owned));
        fixture.segments[index][0] ^= 1; /* A present malformed blob cannot select common metadata. */
        CHECK(route_rejected(&fixture, SHADERLAB_STAGE_INVALID_PARAMETER_BLOB));
        fixture.segments[index][0] ^= 1;
        CHECK(replay_restored(&request, owned));
        const int32_t length = fixture.entries[index].length;
        fixture.entries[index].length = 8; /* Valid version and count, missing collection tail. */
        CHECK(route_rejected(&fixture, SHADERLAB_STAGE_INVALID_PARAMETER_BLOB));
        fixture.entries[index].length = length;
        CHECK(replay_restored(&request, owned));
    }
    const int sibling = fixture.parameter_indices[1];
    CHECK(fixture.lengths[sibling] == 28);
    fixture.segments[sibling][12] = 16; /* Well-formed but nonempty sibling loose shell. */
    CHECK(route_rejected(&fixture, SHADERLAB_STAGE_VARIANT_METADATA_MISMATCH));
    fixture.segments[sibling][12] = 0;
    CHECK(replay_restored(&request, owned));
    /* The no-blob selected row and its explicit parsed sibling still share
     * authority; the sibling conflict must not disappear behind a NULL residual. */
    fixture.parameter_indices[0] = -1;
    CHECK(shaderlab_emitted_hull_coverage_replay(&request, owned) == false);
    ShaderLabEmittedHullCoverage *common_route = NULL;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &common_route) == SHADERLAB_HULL_COVERAGE_OK);
    CHECK(!common_route->entries[0].inputs.current.is_binary && common_route->entries[0].inputs.current.cb_count == 1);
    fixture.segments[sibling][12] = 16;
    CHECK(route_rejected(&fixture, SHADERLAB_STAGE_VARIANT_METADATA_MISMATCH));
    fixture.segments[sibling][12] = 0;
    CHECK(replay_restored(&request, common_route));
    shaderlab_emitted_hull_coverage_free(common_route);
    fixture.parameter_indices[0] = FIXTURE_VARIANT_COUNT;
    CHECK(replay_restored(&request, owned));
    Observer observer = {.fixture = &fixture, .rejected_kind = SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY,
        .mutate = true, .mutate_parameters = true};
    request.observer = observe_receipt; request.observer_context = &observer;
    ShaderLabEmittedHullCoverage *rejected = NULL;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) != SHADERLAB_HULL_COVERAGE_OK &&
          !rejected && observer.changed);
    fixture.pass.common_parameters[HULL_STAGE].constant_buffers[0].size = 0;
    request.observer = NULL; request.observer_context = NULL;
    CHECK(replay_restored(&request, owned));
    shaderlab_emitted_hull_coverage_free(owned); fixture_dispose(&fixture);
    return true;
}

static bool rejection_and_restore(bool icb) {
    Fixture fixture;
    CHECK(icb ? fixture_init_icb(&fixture, false) : fixture_init(&fixture, false, false));
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    ShaderLabEmittedHullCoverage *owned = NULL;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &owned) == SHADERLAB_HULL_COVERAGE_OK);
    const ShaderLabSourceSyntaxKind kinds[] = {SHADERLAB_SOURCE_SYNTAX_ROUTING,
        SHADERLAB_SOURCE_SYNTAX_STAGE_GUARD, SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY};
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
        Observer observer = {.fixture = &fixture, .rejected_kind = kinds[index]};
        request.observer = observe_receipt;
        request.observer_context = &observer;
        ShaderLabEmittedHullCoverage *rejected = NULL;
        CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) != SHADERLAB_HULL_COVERAGE_OK && !rejected);
        request.observer = NULL; request.observer_context = NULL;
        CHECK(replay_restored(&request, owned));
    }
    Observer observer = {.fixture = &fixture, .rejected_kind = SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY, .mutate = true};
    request.observer = observe_receipt; request.observer_context = &observer;
    ShaderLabEmittedHullCoverage *rejected = NULL;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) != SHADERLAB_HULL_COVERAGE_OK && !rejected && observer.changed);
    fixture.pass.state.culling.val = 0;
    request.observer = NULL; request.observer_context = NULL;
    CHECK(replay_restored(&request, owned));
    fixture.pass.subprogram_count[DOMAIN_STAGE] = 0;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) == SHADERLAB_HULL_COVERAGE_SCOPE_UNAVAILABLE && !rejected);
    fixture.pass.subprogram_count[DOMAIN_STAGE] = 1;
    fixture.pass.subprogram_count[HULL_STAGE] = 0;
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) == SHADERLAB_HULL_COVERAGE_SCOPE_UNAVAILABLE && !rejected);
    fixture.pass.subprogram_count[HULL_STAGE] = 1;
    CHECK(replay_restored(&request, owned));
    shaderlab_emitted_hull_coverage_free(owned);
    fixture_dispose(&fixture);

    CHECK(fixture_init(&fixture, false, false));
    CHECK(fixture_second_state(&fixture, true));
    request = (ShaderLabSourceQualityRequest){.shader = &fixture.shader, .archive = &fixture.archive};
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) != SHADERLAB_HULL_COVERAGE_OK && !rejected);
    fixture_dispose(&fixture);

    /* An optional selected geometry row must retain ordinary stage admission.
     * This malformed triangle input declares only two vertices; it is an
     * explicit unavailable request, not a claimed paired-stage interface. */
    CHECK(fixture_init(&fixture, false, false));
    size_t geometry_size = 0;
    uint8_t *geometry = test_geometry_dxbc_primitive(false, true,
        DXBC_INPUT_PRIMITIVE_TRIANGLE, 2, 0, 15, &geometry_size);
    CHECK(fixture_stage(&fixture, 2, geometry, geometry_size));
    request = (ShaderLabSourceQualityRequest){.shader = &fixture.shader, .archive = &fixture.archive};
    CHECK(shaderlab_emitted_hull_coverage_capture(&request, &rejected) != SHADERLAB_HULL_COVERAGE_OK && !rejected);
    fixture_dispose(&fixture);
    return true;
}

int main(void) {
    const size_t allocations = g_allocations_count, bytes = g_allocated_bytes;
    if (!positive_capture(false, false, false, false, EMPTY_ROUTE_ABSENT) ||
        !positive_capture(true, false, false, false, EMPTY_ROUTE_ABSENT) ||
        !positive_capture(false, true, false, false, EMPTY_ROUTE_ABSENT) ||
        !positive_capture(false, false, true, false, EMPTY_ROUTE_ABSENT) ||
        !positive_capture(false, false, false, true, EMPTY_ROUTE_ABSENT) ||
        !positive_capture(false, true, false, true, EMPTY_ROUTE_ABSENT) ||
        !positive_capture(false, false, true, true, EMPTY_ROUTE_ABSENT) ||
        !rejection_and_restore(false) || !rejection_and_restore(true)) return 1;
    for (unsigned icb = 0; icb < 2; ++icb) {
        if (!positive_capture(false, true, true, icb, EMPTY_ROUTE_ABSENT) ||
            !positive_capture_names(false, true, true, icb, EMPTY_ROUTE_ABSENT, true)) return 1;
        for (EmptyRoute route = EMPTY_ROUTE_COMMON; route <= EMPTY_ROUTE_MIXED_PARSED; ++route)
            if (!positive_capture(false, false, true, icb, route)) return 1;
        if (!empty_route_rejections(icb)) return 1;
    }
    if (!positive_capture_shape(false, false, false, false, EMPTY_ROUTE_ABSENT, false, "POINTVALUE") ||
        !positive_capture_shape(true, false, true, false, EMPTY_ROUTE_ABSENT, false, "POINTVALUE") ||
        !positive_capture_shape(false, false, true, false, EMPTY_ROUTE_ABSENT, false, "OBJECTCOORD") ||
        !positive_capture_shape(false, true, true, false, EMPTY_ROUTE_ABSENT, false, "POINTVALUE") ||
        !positive_capture_shape(false, true, true, false, EMPTY_ROUTE_ABSENT, true, "OBJECTCOORD") ||
        !positive_capture_shape(false, false, false, true, EMPTY_ROUTE_ABSENT, false, "POINTVALUE") ||
        !positive_capture_shape(false, true, true, true, EMPTY_ROUTE_ABSENT, false, "POINTVALUE")) return 1;
    if (!paired_capture(false, false, false, false) || !paired_capture(true, false, false, false) ||
        !paired_capture(true, true, false, false) || !paired_capture(true, true, true, false) ||
        !paired_capture(true, false, false, true)) return 1;
    if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
        fprintf(stderr, "allocation leak: %zu/%zu -> %zu/%zu\n",
            allocations, bytes, g_allocations_count, g_allocated_bytes);
        return 1;
    }
    puts("ShaderLab emitted HULL coverage unit tests passed");
    return 0;
}
