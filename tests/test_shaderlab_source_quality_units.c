// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_source_quality.h"
#include "translation/shaderlab_source_quality_internal.h"
#include "common/file_io.h"
#include "dxbc/usbd.h"
#include "test_shaderlab_fixture.h"
#include "test_geometry_fixture.h"
#include "test_tessellation_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

typedef struct {
    CommonFileBytes bytes;
    SerializedShader shader;
    SerializedSubShader subshader;
    SerializedPass pass;
    SerializedSubProgram programs[5];
    SerializedSubProgramIdentity identities[5];
    ParsedShaderProperty properties[2];
    int platform;
    ShaderBlobArchive archive;
    BlobEntry entries[5];
    uint8_t *segments[5];
    int lengths[5];
    size_t expected_entries;
} Fixture;

static bool fixture_init(Fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    CHECK(common_file_read_regular(SHADERLAB_QUALITY_TEST_FIXTURE, 1024 * 1024, &fixture->bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, fixture->bytes.data, fixture->bytes.size, NULL));
    CHECK(table.record_count == 2);
    for (int stage = 0; stage < 2; ++stage) {
        DXBCUSBDRecordView record;
        CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &record));
        size_t size;
        fixture->segments[stage] = test_shaderlab_variant_blob(record.dxbc, record.dxbc_size,
            stage ? 17 : 15, NULL, &size);
        CHECK(fixture->segments[stage] && size <= INT32_MAX);
        fixture->lengths[stage] = (int)size;
        fixture->entries[stage] = (BlobEntry){0, (int32_t)size, stage};
        fixture->programs[stage] = (SerializedSubProgram){.blob_index = stage,
            .program_type = stage ? 17 : 15, .shader_requirements = 0xe3};
        fixture->identities[stage].hardware_tier_group = 3;
        fixture->pass.subprogram_count[stage] = 1;
        fixture->pass.subprograms[stage] = &fixture->programs[stage];
        fixture->pass.subprogram_identities[stage] = &fixture->identities[stage];
    }
    fixture->expected_entries = 2;
    fixture->platform = 4;
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = &fixture->platform;
    fixture->pass.program_mask = 6;
    fixture->pass.state.name = "QUALITY";
    fixture->subshader = (SerializedSubShader){.pass_count = 1, .passes = &fixture->pass};
    fixture->properties[0] = (ParsedShaderProperty){.name = "_Color", .description = "Color",
        .type = 0, .def_value = {1, 1, 1, 1}};
    fixture->properties[1] = (ParsedShaderProperty){.name = "_Scale", .description = "Scale",
        .type = 2, .def_value = {1}};
    fixture->shader = (SerializedShader){.name = "Fixture/Quality/Wrapper",
        .property_count = 2, .properties = fixture->properties,
        .subshader_count = 1, .subshaders = &fixture->subshader};
    fixture->archive = (ShaderBlobArchive){.entries = fixture->entries, .entry_count = 2,
        .segments = fixture->segments, .segment_lengths = fixture->lengths, .segment_count = 2};
    return true;
}

static void fixture_dispose(Fixture *fixture) {
    for (int stage = 0; stage < 5; ++stage) free(fixture->segments[stage]);
    common_file_bytes_dispose(&fixture->bytes);
}

static bool check_current_inventory(Fixture *fixture, StringBuilder *source,
                                    ShaderLabSourceQualityInventory *inventory) {
    ShaderLabSourceQualityRequest request = {.shader = &fixture->shader, .archive = &fixture->archive};
    ShaderLabSourceQualityResult result;
    CHECK(shaderlab_source_quality_inventory_analyze(&request, source, inventory, &result, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(result.classification == HLSL_SOURCE_QUALITY_MIXED && result.wrapper_complete);
    CHECK((result.gaps & (SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE | SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY |
        SHADERLAB_SOURCE_GAP_SCHEMA_AUTHORITY)) == (SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE |
        SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY | SHADERLAB_SOURCE_GAP_SCHEMA_AUTHORITY));
    CHECK(result.linked_entry_count == fixture->expected_entries && result.wrapper_receipt_count > 12);
    CHECK(result.required_external_include_root_count == 1);
    return true;
}

static bool check_bad_inventory(Fixture *fixture, const StringBuilder *source,
                                ShaderLabSourceQualityInventory *inventory) {
    ShaderLabSourceQualityRequest request = {.shader = &fixture->shader, .archive = &fixture->archive};
    ShaderLabSourceQualityResult result = {.classification = HLSL_SOURCE_QUALITY_UNSUPPORTED};
    const ShaderLabSourceQualityResult before = result;
    CHECK(shaderlab_source_quality_inventory_analyze(&request, source, inventory, &result, NULL) != SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(!memcmp(&before, &result, sizeof(before)));
    return true;
}

static bool check_inventory_mutations(Fixture *fixture, StringBuilder *source,
                                      ShaderLabSourceQualityInventory *inventory) {
    for (size_t index = 0; index < inventory->receipt_count; ++index) {
        ShaderLabSourceSyntaxReceipt *receipt = &inventory->receipts[index];
        const ShaderLabSourceSyntaxReceipt saved = *receipt;
        receipt->kind = SHADERLAB_SOURCE_SYNTAX_SHADER_END;
        if (saved.kind == SHADERLAB_SOURCE_SYNTAX_SHADER_END) receipt->kind = SHADERLAB_SOURCE_SYNTAX_HEADER;
        CHECK(check_bad_inventory(fixture, source, inventory)); *receipt = saved;
        ++receipt->source_begin;
        CHECK(check_bad_inventory(fixture, source, inventory)); *receipt = saved;
        receipt->source_digest[0] ^= 1;
        CHECK(check_bad_inventory(fixture, source, inventory)); *receipt = saved;
        ++receipt->stage_index;
        CHECK(check_bad_inventory(fixture, source, inventory)); *receipt = saved;
        ++receipt->pass_index;
        CHECK(check_bad_inventory(fixture, source, inventory)); *receipt = saved;
        if (receipt->kind == SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY) {
            receipt->entry_record_index = SIZE_MAX;
            CHECK(check_bad_inventory(fixture, source, inventory)); *receipt = saved;
        }
    }
    --inventory->receipt_count;
    CHECK(check_bad_inventory(fixture, source, inventory)); ++inventory->receipt_count;
    const ShaderLabSourceQualityResult saved_quality = inventory->quality;
    inventory->quality.classification = HLSL_SOURCE_QUALITY_CLEAN;
    CHECK(check_bad_inventory(fixture, source, inventory)); inventory->quality = saved_quality;
    inventory->quality.gaps = 0;
    CHECK(check_bad_inventory(fixture, source, inventory)); inventory->quality = saved_quality;
    inventory->structure.visual_output_certified = true;
    CHECK(check_bad_inventory(fixture, source, inventory)); inventory->structure.visual_output_certified = false;
    inventory->modeled_input_digest[0] ^= 1;
    CHECK(check_bad_inventory(fixture, source, inventory)); inventory->modeled_input_digest[0] ^= 1;
    for (size_t index = 0; index < inventory->entries.count; ++index) {
        ShaderLabExpressionSourceRecord *entry = &inventory->entries.records[index];
        const bool present = entry->has_source_quality;
        entry->has_source_quality = false;
        CHECK(check_bad_inventory(fixture, source, inventory)); entry->has_source_quality = present;
        ++entry->stage_index;
        CHECK(check_bad_inventory(fixture, source, inventory)); --entry->stage_index;
        ++entry->hardware_tier_group;
        CHECK(check_bad_inventory(fixture, source, inventory)); --entry->hardware_tier_group;
        ++entry->blob_index;
        CHECK(check_bad_inventory(fixture, source, inventory)); --entry->blob_index;
        ++entry->serialized_state;
        CHECK(check_bad_inventory(fixture, source, inventory)); --entry->serialized_state;
        entry->target_digest[0] ^= 1;
        CHECK(check_bad_inventory(fixture, source, inventory)); entry->target_digest[0] ^= 1;
        CHECK(entry->instructions.count != 0);
        ++entry->instructions.origins[0].source_instruction_index;
        CHECK(check_bad_inventory(fixture, source, inventory));
        --entry->instructions.origins[0].source_instruction_index;
        ++entry->source_quality.counts.unknown_provenance;
        CHECK(check_bad_inventory(fixture, source, inventory)); --entry->source_quality.counts.unknown_provenance;
    }
    /* Updating public source hashes cannot bless changed include/route text. */
    const ShaderLabExpressionSourceMap saved_map = inventory->entries;
    const uint8_t changed = (uint8_t)source->buf[0] ^ 1u;
    const char saved = source->buf[0]; source->buf[0] = (char)changed;
    common_sha256(source->buf, source->len, inventory->entries.source_digest);
    CHECK(check_bad_inventory(fixture, source, inventory)); source->buf[0] = saved;
    inventory->entries = saved_map;
    const char *tokens[] = {"UnityShaderVariables.cginc", "#if defined(VERTEX)", "#pragma vertex"};
    for (size_t index = 0; index < sizeof(tokens) / sizeof(tokens[0]); ++index) {
        char *token = strstr(source->buf, tokens[index]); CHECK(token);
        const char original = token[0]; token[0] ^= 1;
        CHECK(check_bad_inventory(fixture, source, inventory)); token[0] = original;
    }
    CHECK(check_current_inventory(fixture, source, inventory));
    return true;
}

static bool reject_receipt(void *context, const ShaderLabSourceSyntaxReceipt *receipt) {
    const ShaderLabSourceSyntaxKind *kind = context;
    return receipt->kind != *kind;
}

static bool check_transaction_and_model(Fixture *fixture, StringBuilder *source,
                                        ShaderLabSourceQualityInventory *inventory) {
    ShaderLabSourceQualityRequest request = {.shader = &fixture->shader, .archive = &fixture->archive};
    const SerializedShaderFloatValue saved_state = fixture->pass.state.culling;
    fixture->pass.state.culling = (SerializedShaderFloatValue){.present = true, .val = 99};
    StringBuilder rejected; sb_init(&rejected);
    const ShaderLabSourceQualityInventory before = *inventory;
    CHECK(shaderlab_source_quality_emit(&request, &rejected, inventory, NULL) == SHADERLAB_SOURCE_QUALITY_EMISSION_FAILED);
    CHECK(!rejected.len && !memcmp(&before, inventory, sizeof(before)));
    fixture->pass.state.culling = saved_state;
    fixture->pass.state.culling = (SerializedShaderFloatValue){.present = true, .val = 1};
    CHECK(check_bad_inventory(fixture, source, inventory)); fixture->pass.state.culling = saved_state;
    /* A dormant state's raw value is still selected-model identity, even when
     * absence prevents emission of that value as a command. */
    fixture->pass.state.culling.val = 2;
    CHECK(check_bad_inventory(fixture, source, inventory)); fixture->pass.state.culling = saved_state;
    fixture->properties[0].def_value[0] = 0.5f;
    CHECK(check_bad_inventory(fixture, source, inventory)); fixture->properties[0].def_value[0] = 1;
    fixture->shader.dependency_count = 1;
    CHECK(shaderlab_source_quality_emit(&request, &rejected, inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
    CHECK(!memcmp(&before, inventory, sizeof(before))); fixture->shader.dependency_count = 0;
    const ShaderLabSourceSyntaxKind rejections[] = {SHADERLAB_SOURCE_SYNTAX_PROPERTY,
        SHADERLAB_SOURCE_SYNTAX_RENDER_STATE, SHADERLAB_SOURCE_SYNTAX_INCLUDE_POLICY,
        SHADERLAB_SOURCE_SYNTAX_ROUTING, SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY};
    for (size_t index = 0; index < sizeof(rejections) / sizeof(rejections[0]); ++index) {
        request.observer = reject_receipt; request.observer_context = (void *)&rejections[index];
        if (rejections[index] == SHADERLAB_SOURCE_SYNTAX_RENDER_STATE)
            fixture->pass.state.culling = (SerializedShaderFloatValue){.present = true, .val = 1};
        CHECK(shaderlab_source_quality_emit(&request, &rejected, inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OBSERVER_REJECTED);
        CHECK(!rejected.len && !memcmp(&before, inventory, sizeof(before)));
        fixture->pass.state.culling = saved_state;
    }
    sb_free(&rejected);
    return true;
}


static bool check_named_wrapper_fields(Fixture *fixture) {
    SerializedTag sub_tag = {"RenderType", "Opaque"};
    SerializedTag pass_tag = {"LightMode", "Always"};
    fixture->subshader.tags = (SerializedTagMap){1, &sub_tag};
    fixture->pass.tags = (SerializedTagMap){1, &pass_tag};
    fixture->subshader.lod = 120;
    fixture->pass.state.lod = 80;
    fixture->pass.state.culling = (SerializedShaderFloatValue){.present = true, .val = 1};
    fixture->shader.disable_no_subshaders_message = true;
    ShaderLabSourceQualityRequest request = {.shader = &fixture->shader, .archive = &fixture->archive};
    ShaderLabSourceQualityInventory inventory = {0};
    StringBuilder source; sb_init(&source);
    CHECK(shaderlab_source_quality_emit(&request, &source, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    const ShaderLabSourceSyntaxKind required[] = {SHADERLAB_SOURCE_SYNTAX_SUBSHADER_TAGS,
        SHADERLAB_SOURCE_SYNTAX_SUBSHADER_LOD, SHADERLAB_SOURCE_SYNTAX_PASS_TAGS,
        SHADERLAB_SOURCE_SYNTAX_PASS_LOD, SHADERLAB_SOURCE_SYNTAX_RENDER_STATE, SHADERLAB_SOURCE_SYNTAX_TRAILER};
    for (size_t kind = 0; kind < sizeof(required) / sizeof(required[0]); ++kind) {
        bool found = false;
        for (size_t receipt = 0; receipt < inventory.receipt_count; ++receipt)
            found |= inventory.receipts[receipt].kind == required[kind];
        CHECK(found);
    }
    CHECK(check_current_inventory(fixture, &source, &inventory));
    pass_tag.value = "ForwardBase";
    CHECK(check_bad_inventory(fixture, &source, &inventory)); pass_tag.value = "Always";
    ++fixture->subshader.lod;
    CHECK(check_bad_inventory(fixture, &source, &inventory)); --fixture->subshader.lod;
    ShaderObject unrelated = {0}; request.object = &unrelated;
    ShaderLabSourceQualityResult result;
    CHECK(shaderlab_source_quality_inventory_analyze(&request, &source, &inventory, &result, NULL) == SHADERLAB_SOURCE_QUALITY_INVALID_ARGUMENT);
    shaderlab_source_quality_inventory_dispose(&inventory); sb_free(&source);
    fixture->subshader.tags = (SerializedTagMap){0}; fixture->pass.tags = (SerializedTagMap){0};
    fixture->subshader.lod = 0; fixture->pass.state.lod = 0;
    fixture->pass.state.culling = (SerializedShaderFloatValue){0};
    fixture->shader.disable_no_subshaders_message = false;
    return true;
}

static bool check_keyword_routing(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    char *names[] = {"FEATURE"};
    uint8_t flags[] = {1};
    uint16_t mask[] = {0};
    int raw_keyword = 0;
    SerializedSubProgram programs[2][2];
    SerializedSubProgramIdentity identities[2][2];
    uint8_t *segments[4] = {fixture.segments[0], fixture.segments[1], NULL, NULL};
    int lengths[4] = {fixture.lengths[0], fixture.lengths[1], 0, 0};
    BlobEntry entries[4] = {fixture.entries[0], fixture.entries[1], {0}, {0}};
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, fixture.bytes.data, fixture.bytes.size, NULL));
    for (int stage = 0; stage < 2; ++stage) {
        DXBCUSBDRecordView record;
        CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &record));
        programs[stage][0] = programs[stage][1] = fixture.programs[stage];
        identities[stage][0] = identities[stage][1] = fixture.identities[stage];
        programs[stage][1].blob_index = stage + 2;
        programs[stage][1].local_keyword_count = 1;
        programs[stage][1].local_keywords = names;
        identities[stage][1].local_keyword_index_count = 1;
        identities[stage][1].local_keyword_indices = &raw_keyword;
        size_t size;
        segments[stage + 2] = test_shaderlab_variant_blob(record.dxbc, record.dxbc_size,
            stage ? 17 : 15, names[0], &size);
        CHECK(segments[stage + 2] && size <= INT32_MAX);
        lengths[stage + 2] = (int)size;
        entries[stage + 2] = (BlobEntry){0, (int32_t)size, stage + 2};
        fixture.pass.subprogram_count[stage] = 2;
        fixture.pass.subprograms[stage] = programs[stage];
        fixture.pass.subprogram_identities[stage] = identities[stage];
    }
    fixture.shader.keyword_names = (SerializedKeywordList){1, names};
    fixture.shader.keyword_flags = flags;
    fixture.pass.serialized_keyword_state_mask_count = 1;
    fixture.pass.serialized_keyword_state_mask = mask;
    fixture.archive = (ShaderBlobArchive){.entries = entries, .entry_count = 4,
        .segments = segments, .segment_lengths = lengths, .segment_count = 4};
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    StringBuilder source, baseline; sb_init(&source); sb_init(&baseline);
    ShaderLabSourceQualityInventory inventory = {0};
    CHECK(shaderlab_source_quality_emit(&request, &source, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(inventory.quality.wrapper_complete && inventory.quality.linked_entry_count == 4);
    CHECK(strstr(source.buf, "#pragma multi_compile_local __ FEATURE"));
    CHECK(shaderlab_emit_high_level_candidate(&fixture.shader, entries, 4, segments, lengths, 4, &baseline, NULL));
    CHECK(source.len == baseline.len && !memcmp(source.buf, baseline.buf, source.len));
    ShaderLabSourceQualityResult result;
    CHECK(shaderlab_source_quality_inventory_analyze(&request, &source, &inventory, &result, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    flags[0] = 5;
    CHECK(check_bad_inventory(&fixture, &source, &inventory)); flags[0] = 1;
    identities[0][1].hardware_tier_group = 1;
    CHECK(check_bad_inventory(&fixture, &source, &inventory)); identities[0][1].hardware_tier_group = 3;
    raw_keyword = 1;
    CHECK(check_bad_inventory(&fixture, &source, &inventory)); raw_keyword = 0;
    char *route = strstr(source.buf, "#elif"); CHECK(route);
    route[1] ^= 1;
    CHECK(check_bad_inventory(&fixture, &source, &inventory)); route[1] ^= 1;
    CHECK(shaderlab_source_quality_inventory_analyze(&request, &source, &inventory, &result, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    shaderlab_source_quality_inventory_dispose(&inventory); sb_free(&source); sb_free(&baseline);
    free(segments[2]); free(segments[3]); fixture_dispose(&fixture);
    return true;
}

static bool check_texture_properties_and_schema_guards(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    fixture.properties[0] = (ParsedShaderProperty){.name = "_MainTex",
        .description = "Texture", .type = 4, .def_texture_name = "white",
        .def_texture_dim = 2};
    ShaderLabSourceQualityRequest request = {.shader = &fixture.shader,
        .archive = &fixture.archive};
    StringBuilder source, baseline;
    sb_init(&source); sb_init(&baseline);
    ShaderLabSourceQualityInventory inventory = {0};
    CHECK(shaderlab_source_quality_emit(&request, &source, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(strstr(source.buf, "_MainTex (\"Texture\", 2D) = \"white\" {}"));
    CHECK(shaderlab_emit_high_level_candidate(&fixture.shader, fixture.archive.entries,
        fixture.archive.entry_count, fixture.archive.segments, fixture.archive.segment_lengths,
        fixture.archive.segment_count, &baseline, NULL));
    CHECK(source.len == baseline.len && !memcmp(source.buf, baseline.buf, source.len));
    CHECK(check_current_inventory(&fixture, &source, &inventory));
    fixture.properties[0].def_texture_name = "black";
    CHECK(check_bad_inventory(&fixture, &source, &inventory));
    fixture.properties[0].def_texture_name = "white";
    fixture.properties[0].def_texture_dim = 3;
    CHECK(check_bad_inventory(&fixture, &source, &inventory));
    fixture.properties[0].def_texture_dim = 2;
    /* Dormant serialized defaults are still selected-model identity. */
    fixture.properties[0].def_value[3] = 1;
    CHECK(check_bad_inventory(&fixture, &source, &inventory));
    fixture.properties[0].def_value[3] = 0;
    StringBuilder rejected; sb_init(&rejected);
    const ShaderLabSourceQualityInventory before = inventory;
    const int dimensions[] = {0, 7};
    for (size_t index = 0; index < sizeof(dimensions) / sizeof(dimensions[0]); ++index) {
        fixture.properties[0].def_texture_dim = dimensions[index];
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        CHECK(!rejected.len && !memcmp(&before, &inventory, sizeof(before)));
    }
    fixture.properties[0].def_texture_dim = 2;
    fixture.properties[0].def_texture_name = NULL;
    CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
    fixture.properties[0].def_texture_name = "white";
    /* A caller cannot manufacture optional schema authority by setting decoded
     * on a hand-built model without the complete schema-derived value shape. */
    ShaderObject object = {.shader = fixture.shader, .decoded = true,
        .profile = SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1};
    request.object = &object; request.shader = &object.shader;
    ShaderLabSourceQualityDiagnostic diagnostic;
    CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, &diagnostic) == SHADERLAB_SOURCE_QUALITY_STRUCTURE_FAILED);
    CHECK(diagnostic.structure.status == SHADERLAB_STRUCTURE_SCHEMA_AUTHORITY_FAILED);
    CHECK(!rejected.len && !memcmp(&before, &inventory, sizeof(before)));
    object.profile = (SerializedShaderSchemaProfile)999;
    CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, &diagnostic) == SHADERLAB_SOURCE_QUALITY_STRUCTURE_FAILED);
    CHECK(diagnostic.structure.status == SHADERLAB_STRUCTURE_SCHEMA_AUTHORITY_FAILED);
    object.decoded = false;
    CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, &diagnostic) == SHADERLAB_SOURCE_QUALITY_STRUCTURE_FAILED);
    CHECK(diagnostic.structure.status == SHADERLAB_STRUCTURE_INVALID_ARGUMENT);
    CHECK(!rejected.len && !memcmp(&before, &inventory, sizeof(before)));
    shaderlab_source_quality_inventory_dispose(&inventory);
    sb_free(&source); sb_free(&baseline); sb_free(&rejected);
    fixture_dispose(&fixture);
    return true;
}

static bool fixture_add_stage(Fixture *fixture, int stage) {
    size_t dxbc_size = 0;
    uint8_t *dxbc = stage == 2 ? test_geometry_dxbc(false, true, &dxbc_size) :
        stage == 3 ? test_tessellation_hull_dxbc(3, 3, 0, &dxbc_size) :
        test_tessellation_domain_dxbc(2, 3, 7, &dxbc_size);
    CHECK(dxbc);
    const int types[] = {15, 17, 19, 21, 22};
    size_t blob_size = 0;
    fixture->segments[stage] = test_shaderlab_variant_blob(dxbc, dxbc_size,
        types[stage], NULL, &blob_size);
    free(dxbc);
    CHECK(fixture->segments[stage] && blob_size <= INT32_MAX);
    fixture->lengths[stage] = (int)blob_size;
    fixture->entries[stage] = (BlobEntry){0, (int32_t)blob_size, stage};
    fixture->programs[stage] = (SerializedSubProgram){.blob_index = stage,
        .program_type = types[stage], .shader_requirements = 0xe3};
    fixture->identities[stage].hardware_tier_group = 3;
    fixture->pass.subprogram_count[stage] = 1;
    fixture->pass.subprograms[stage] = &fixture->programs[stage];
    fixture->pass.subprogram_identities[stage] = &fixture->identities[stage];
    fixture->pass.program_mask |= 2u << stage;
    if (fixture->archive.entry_count <= stage)
        fixture->archive.entry_count = fixture->archive.segment_count = stage + 1;
    ++fixture->expected_entries;
    return true;
}

static bool check_stage_receipts(Fixture *fixture, StringBuilder *source,
                                 ShaderLabSourceQualityInventory *inventory, int stage) {
    size_t guards = 0, bodies = 0, routes = 0;
    size_t linked_index = SIZE_MAX;
    for (size_t index = 0; index < inventory->receipt_count; ++index) {
        const ShaderLabSourceSyntaxReceipt *receipt = &inventory->receipts[index];
        if (receipt->stage_index != stage) continue;
        if (receipt->kind == SHADERLAB_SOURCE_SYNTAX_STAGE_GUARD) ++guards;
        if (receipt->kind == SHADERLAB_SOURCE_SYNTAX_ROUTING) ++routes;
        if (receipt->kind == SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY) {
            ++bodies;
            linked_index = receipt->entry_record_index;
        }
    }
    CHECK(guards == 2 && bodies == 1 && routes != 0);
    CHECK(linked_index < inventory->entries.count);
    const ShaderLabExpressionSourceRecord *entry = &inventory->entries.records[linked_index];
    CHECK(entry->stage_index == stage && entry->has_source_quality && entry->instructions.complete);
    const HLSLSourceQualityResult original = entry->source_quality;
    ShaderLabSourceQualityRequest request = {.shader = &fixture->shader, .archive = &fixture->archive};
    ShaderLabSourceQualityResult result;
    CHECK(shaderlab_source_quality_inventory_analyze(&request, source, inventory, &result, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(!memcmp(&original, &entry->source_quality, sizeof(original)));
    CHECK(result.classification != HLSL_SOURCE_QUALITY_CLEAN);

    const char *guards_text[] = {NULL, NULL, "#if defined(GEOMETRY)",
        "#if defined(HULL)", "#if defined(DOMAIN)"};
    char *guard = strstr(source->buf, guards_text[stage]);
    CHECK(guard); const char saved = guard[0]; guard[0] ^= 1;
    CHECK(check_bad_inventory(fixture, source, inventory));
    /* Public digest fields cannot grant coverage to changed routing bytes. */
    uint8_t source_digest[32], map_digest[32];
    memcpy(source_digest, inventory->source_digest, 32);
    memcpy(map_digest, inventory->entries.source_digest, 32);
    common_sha256(source->buf, source->len, inventory->source_digest);
    memcpy(inventory->entries.source_digest, inventory->source_digest, 32);
    size_t guard_receipt = SIZE_MAX;
    for (size_t index = 0; index < inventory->receipt_count; ++index) {
        ShaderLabSourceSyntaxReceipt *receipt = &inventory->receipts[index];
        if (receipt->kind == SHADERLAB_SOURCE_SYNTAX_STAGE_GUARD && receipt->stage_index == stage) {
            guard_receipt = index;
            break;
        }
    }
    CHECK(guard_receipt != SIZE_MAX);
    ShaderLabSourceSyntaxReceipt *receipt = &inventory->receipts[guard_receipt];
    const ShaderLabSourceSyntaxReceipt original_receipt = *receipt;
    common_sha256(source->buf + receipt->source_begin, receipt->source_end - receipt->source_begin,
        receipt->source_digest);
    CHECK(check_bad_inventory(fixture, source, inventory));
    *receipt = original_receipt; guard[0] = saved;
    memcpy(inventory->source_digest, source_digest, 32);
    memcpy(inventory->entries.source_digest, map_digest, 32);
    /* Removing a specific stage guard cannot be hidden by retaining the rest
     * of its body and contiguous source map. */
    memmove(receipt, receipt + 1, (inventory->receipt_count - guard_receipt - 1) * sizeof(*receipt));
    --inventory->receipt_count;
    CHECK(check_bad_inventory(fixture, source, inventory));
    memmove(receipt + 1, receipt, (inventory->receipt_count - guard_receipt) * sizeof(*receipt));
    ++inventory->receipt_count; *receipt = original_receipt;
    const uint64_t requirements = fixture->programs[stage].shader_requirements;
    fixture->programs[stage].shader_requirements ^= 1u;
    CHECK(check_bad_inventory(fixture, source, inventory));
    fixture->programs[stage].shader_requirements = requirements;
    fixture->identities[stage].hardware_tier_group = 1;
    CHECK(check_bad_inventory(fixture, source, inventory));
    fixture->identities[stage].hardware_tier_group = 3;
    SerializedSubProgramIdentity *identities = fixture->pass.subprogram_identities[stage];
    fixture->pass.subprogram_identities[stage] = NULL;
    CHECK(check_bad_inventory(fixture, source, inventory));
    fixture->pass.subprogram_identities[stage] = identities;
    const int type = fixture->programs[stage].program_type;
    fixture->programs[stage].program_type = 15;
    CHECK(check_bad_inventory(fixture, source, inventory)); fixture->programs[stage].program_type = type;
    return true;
}

static bool check_linked_graphics_stages(void) {
    for (unsigned tessellation = 0; tessellation < 2; ++tessellation) {
        Fixture fixture; CHECK(fixture_init(&fixture));
        CHECK(fixture_add_stage(&fixture, tessellation ? 3 : 2));
        if (tessellation) CHECK(fixture_add_stage(&fixture, 4));
        ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
        StringBuilder source, baseline, rejected;
        sb_init(&source); sb_init(&baseline); sb_init(&rejected);
        ShaderLabSourceQualityInventory inventory = {0};
        ShaderLabSourceQualityDiagnostic diagnostic;
        const ShaderLabSourceQualityStatus emitted = shaderlab_source_quality_emit(&request, &source, &inventory, &diagnostic);
        if (emitted != SHADERLAB_SOURCE_QUALITY_OK)
            fprintf(stderr, "linked emission failed: tessellation=%u quality=%u candidate=%u stage=%d status=%u emitter=%u reason=%u target=%u\n",
                tessellation, emitted, diagnostic.emission.status, diagnostic.emission.stage.stage_index,
                diagnostic.emission.stage.status, diagnostic.emission.stage.hlsl.status,
                diagnostic.emission.stage.hlsl.reason, diagnostic.emission.target.status);
        CHECK(emitted == SHADERLAB_SOURCE_QUALITY_OK);
        CHECK(shaderlab_emit_high_level_candidate(&fixture.shader, fixture.archive.entries,
            fixture.archive.entry_count, fixture.archive.segments, fixture.archive.segment_lengths,
            fixture.archive.segment_count, &baseline, NULL));
        CHECK(source.len == baseline.len && !memcmp(source.buf, baseline.buf, source.len));
        CHECK(check_current_inventory(&fixture, &source, &inventory));
        CHECK(check_inventory_mutations(&fixture, &source, &inventory));
        for (int stage = tessellation ? 3 : 2; stage <= (tessellation ? 4 : 2); ++stage)
            CHECK(check_stage_receipts(&fixture, &source, &inventory, stage));
        const ShaderLabSourceQualityInventory saved = inventory;
        const int stage = tessellation ? 3 : 2;
        fixture.pass.subprogram_count[stage] = 0;
        CHECK(shaderlab_source_quality_inventory_analyze(&request, &source, &inventory,
            &(ShaderLabSourceQualityResult){0}, NULL) != SHADERLAB_SOURCE_QUALITY_OK);
        fixture.pass.subprogram_count[stage] = 1;
        if (tessellation) {
            fixture.pass.subprogram_count[4] = 0;
            CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
            CHECK(!rejected.len && !memcmp(&saved, &inventory, sizeof(saved)));
            fixture.pass.subprogram_count[4] = 1;
        }
        SerializedPass two_passes[2] = {fixture.pass, fixture.pass};
        two_passes[1].pass_type = 1; /* A later UsePass is still outside scope. */
        fixture.subshader.pass_count = 2; fixture.subshader.passes = two_passes;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        CHECK(!rejected.len && !memcmp(&saved, &inventory, sizeof(saved)));
        fixture.subshader.pass_count = 1; fixture.subshader.passes = &fixture.pass;
        const ShaderLabSourceSyntaxKind kinds[] = {SHADERLAB_SOURCE_SYNTAX_STAGE_GUARD,
            SHADERLAB_SOURCE_SYNTAX_ROUTING, SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY};
        for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
            request.observer = reject_receipt; request.observer_context = (void *)&kinds[index];
            CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OBSERVER_REJECTED);
            CHECK(!rejected.len && !memcmp(&saved, &inventory, sizeof(saved)));
        }
        shaderlab_source_quality_inventory_dispose(&inventory);
        sb_free(&source); sb_free(&baseline); sb_free(&rejected); fixture_dispose(&fixture);
    }
    return true;
}

static bool check_inventory_limits(void) {
    Fixture fixture; CHECK(fixture_init(&fixture));
    char *names[] = {"FIRST", "SECOND", "THIRD", "FOURTH"};
    uint8_t flags[] = {1, 1, 1, 1};
    uint16_t mask[] = {0, 1, 2, 3};
    int keyword_indices[16][4] = {{0}};
    char *selected_names[16][4] = {{0}};
    SerializedSubProgram programs[2][16] = {{{0}}};
    SerializedSubProgramIdentity identities[2][16] = {{{0}}};
    uint8_t *segments[33] = {0};
    int lengths[33] = {0};
    BlobEntry entries[33] = {{0}};
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, fixture.bytes.data, fixture.bytes.size, NULL));
    for (int state = 0; state < 16; ++state) {
        int keyword_count = 0;
        const char *blob_keywords[4] = {0};
        for (int keyword = 0; keyword < 4; ++keyword) {
            if (!(state & (1 << keyword))) continue;
            keyword_indices[state][keyword_count] = keyword;
            selected_names[state][keyword_count] = names[keyword];
            blob_keywords[keyword_count++] = names[keyword];
        }
        for (int stage = 0; stage < 2; ++stage) {
            DXBCUSBDRecordView record;
            CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &record));
            const int index = stage * 16 + state;
            size_t size = 0;
            segments[index] = test_shaderlab_variant_blob_keywords(record.dxbc,
                record.dxbc_size, stage ? 17 : 15, blob_keywords, (size_t)keyword_count, &size);
            CHECK(segments[index] && size <= INT32_MAX);
            lengths[index] = (int)size;
            entries[index] = (BlobEntry){0, (int32_t)size, index};
            programs[stage][state] = fixture.programs[stage];
            programs[stage][state].blob_index = index;
            programs[stage][state].local_keyword_count = keyword_count;
            programs[stage][state].local_keywords = selected_names[state];
            identities[stage][state] = fixture.identities[stage];
            identities[stage][state].local_keyword_index_count = keyword_count;
            identities[stage][state].local_keyword_indices = keyword_indices[state];
        }
    }
    fixture.shader.keyword_names = (SerializedKeywordList){4, names};
    fixture.shader.keyword_flags = flags;
    fixture.pass.serialized_keyword_state_mask_count = 4;
    fixture.pass.serialized_keyword_state_mask = mask;
    for (int stage = 0; stage < 2; ++stage) {
        fixture.pass.subprogram_count[stage] = 16;
        fixture.pass.subprograms[stage] = programs[stage];
        fixture.pass.subprogram_identities[stage] = identities[stage];
    }
    fixture.expected_entries = 32;
    fixture.archive = (ShaderBlobArchive){.entries = entries, .entry_count = 32,
        .segments = segments, .segment_lengths = lengths, .segment_count = 32};
    const ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    StringBuilder source, rejected; sb_init(&source); sb_init(&rejected);
    ShaderLabSourceQualityInventory inventory = {0};
    CHECK(shaderlab_source_quality_emit(&request, &source, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(inventory.entries.count == 32 && inventory.receipt_count <= 256);
    CHECK(check_current_inventory(&fixture, &source, &inventory));
    const size_t entry_count = inventory.entries.count;
    inventory.entries.count = 33;
    CHECK(check_bad_inventory(&fixture, &source, &inventory)); inventory.entries.count = entry_count;
    const size_t receipt_count = inventory.receipt_count;
    inventory.receipt_count = 257;
    CHECK(check_bad_inventory(&fixture, &source, &inventory)); inventory.receipt_count = receipt_count;
    const ShaderLabSourceQualityInventory saved = inventory;
    /* Four exhaustive axes create 16 V and 16 F bodies. One independent G
     * body raises the actual linked record inventory to exactly 33. */
    CHECK(fixture_add_stage(&fixture, 2));
    fixture.programs[2].blob_index = 32;
    segments[32] = fixture.segments[2]; lengths[32] = fixture.lengths[2];
    entries[32] = (BlobEntry){0, lengths[32], 32};
    fixture.archive.entry_count = fixture.archive.segment_count = 33;
    CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) != SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(!rejected.len && !memcmp(&saved, &inventory, sizeof(saved)));
    CHECK(check_bad_inventory(&fixture, &source, &inventory));
    shaderlab_source_quality_inventory_dispose(&inventory);
    sb_free(&source); sb_free(&rejected);
    for (int index = 0; index < 32; ++index) free(segments[index]);
    fixture_dispose(&fixture);

    /* The streaming receipt boundary is independently bounded even when an
     * internal producer attempts more syntax than admitted wrapper models. */
    StringBuilder bounded; sb_init(&bounded);
    ShaderLabSourceQualityInventory receipts = {0};
    ShaderLabSourceQualityCapture capture = {.inventory = &receipts};
    for (size_t index = 0; index < 256; ++index) {
        sb_append_char(&bounded, 'x');
        CHECK(shaderlab_source_quality_capture_receipt(&capture, &bounded,
            SHADERLAB_SOURCE_SYNTAX_ROUTING, -1, 0, 0, 2, SIZE_MAX));
    }
    CHECK(receipts.receipt_count == 256 && !capture.failed);
    sb_append_char(&bounded, 'x');
    CHECK(!shaderlab_source_quality_capture_receipt(&capture, &bounded,
        SHADERLAB_SOURCE_SYNTAX_ROUTING, -1, 0, 0, 2, SIZE_MAX));
    CHECK(capture.failed && receipts.receipt_count == 256 && capture.cursor == 256);
    shaderlab_source_quality_inventory_dispose(&receipts); sb_free(&bounded);
    return true;
}

typedef struct {
    int subshader;
    int pass;
    ShaderLabSourceSyntaxKind kind;
} ReceiptRejection;

static bool reject_selected_receipt(void *context, const ShaderLabSourceSyntaxReceipt *receipt) {
    const ReceiptRejection *rejected = context;
    return receipt->subshader_index != rejected->subshader ||
        receipt->pass_index != rejected->pass || receipt->kind != rejected->kind;
}

static bool check_same_source_changed_model(Fixture *fixture, const StringBuilder *source,
                                            ShaderLabSourceQualityInventory *inventory) {
    CHECK(check_bad_inventory(fixture, source, inventory));
    const ShaderLabSourceQualityRequest request = {.shader = &fixture->shader, .archive = &fixture->archive};
    StringBuilder current; sb_init(&current);
    ShaderLabSourceQualityInventory rebuilt = {0};
    CHECK(shaderlab_source_quality_emit(&request, &current, &rebuilt, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
    CHECK(current.len == source->len && !memcmp(current.buf, source->buf, source->len));
    CHECK(!memcmp(rebuilt.source_digest, inventory->source_digest, sizeof(rebuilt.source_digest)));
    CHECK(memcmp(rebuilt.modeled_input_digest, inventory->modeled_input_digest,
        sizeof(rebuilt.modeled_input_digest)));
    CHECK(rebuilt.quality.classification == HLSL_SOURCE_QUALITY_MIXED);
    shaderlab_source_quality_inventory_dispose(&rebuilt); sb_free(&current);
    return true;
}

static bool check_multiple_passes_and_subshaders(void) {
    for (unsigned stage_family = 0; stage_family < 3; ++stage_family) {
        Fixture fixture; CHECK(fixture_init(&fixture));
        if (stage_family == 1) CHECK(fixture_add_stage(&fixture, 2));
        if (stage_family == 2) {
            CHECK(fixture_add_stage(&fixture, 3));
            CHECK(fixture_add_stage(&fixture, 4));
        }
        const size_t entries_per_pass = fixture.expected_entries;
        SerializedPass passes[8];
        SerializedSubShader subshaders[4];
        SerializedSubProgram programs[8][5];
        SerializedSubProgramIdentity identities[8][5];
        for (int index = 0; index < 8; ++index) {
            passes[index] = fixture.pass;
            for (int stage = 0; stage < 5; ++stage) {
                programs[index][stage] = fixture.programs[stage];
                identities[index][stage] = fixture.identities[stage];
                if (!passes[index].subprogram_count[stage]) continue;
                passes[index].subprograms[stage] = &programs[index][stage];
                passes[index].subprogram_identities[stage] = &identities[index][stage];
            }
            /* These distinct absent values deliberately generate identical
             * pass text. Their order remains selected-model authority. */
            passes[index].state.culling.val = (float)index;
        }
        for (int index = 0; index < 4; ++index)
            subshaders[index] = (SerializedSubShader){.pass_count = 2, .passes = &passes[index * 2]};
        fixture.shader.subshader_count = 4;
        fixture.shader.subshaders = subshaders;
        fixture.expected_entries *= 8;
        const ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
        StringBuilder source, baseline, rejected;
        sb_init(&source); sb_init(&baseline); sb_init(&rejected);
        ShaderLabSourceQualityInventory inventory = {0};
        CHECK(shaderlab_source_quality_emit(&request, &source, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
        CHECK(shaderlab_emit_high_level_candidate(&fixture.shader, fixture.archive.entries,
            fixture.archive.entry_count, fixture.archive.segments, fixture.archive.segment_lengths,
            fixture.archive.segment_count, &baseline, NULL));
        CHECK(source.len == baseline.len && !memcmp(source.buf, baseline.buf, source.len));
        CHECK(inventory.entries.count == entries_per_pass * 8 && inventory.entries.count <= 32);
        CHECK(inventory.receipt_count <= 256 && inventory.quality.wrapper_complete);
        CHECK(inventory.quality.classification == HLSL_SOURCE_QUALITY_MIXED);
        CHECK(inventory.quality.required_external_include_root_count == 8);
        CHECK(inventory.quality.gaps & SHADERLAB_SOURCE_GAP_SCHEMA_AUTHORITY);
        ShaderLabSourceQualityResult quality;
        CHECK(shaderlab_source_quality_inventory_analyze(&request, &source, &inventory,
            &quality, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
        inventory.quality.required_external_include_root_count = 1;
        CHECK(check_bad_inventory(&fixture, &source, &inventory));
        inventory.quality.required_external_include_root_count = 8;
        unsigned pass_begins[4][2] = {{0}}, pass_ends[4][2] = {{0}}, includes[4][2] = {{0}};
        size_t linked[4][2] = {{0}};
        for (size_t index = 0; index < inventory.receipt_count; ++index) {
            const ShaderLabSourceSyntaxReceipt *receipt = &inventory.receipts[index];
            if (receipt->pass_index < 0) continue;
            CHECK(receipt->subshader_index >= 0 && receipt->subshader_index < 4);
            CHECK(receipt->pass_index < 2);
            const int sub = receipt->subshader_index, pass = receipt->pass_index;
            pass_begins[sub][pass] += receipt->kind == SHADERLAB_SOURCE_SYNTAX_PASS_BEGIN;
            pass_ends[sub][pass] += receipt->kind == SHADERLAB_SOURCE_SYNTAX_PASS_END;
            includes[sub][pass] += receipt->kind == SHADERLAB_SOURCE_SYNTAX_INCLUDE_POLICY;
            linked[sub][pass] += receipt->kind == SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY;
        }
        for (int sub = 0; sub < 4; ++sub)
            for (int pass = 0; pass < 2; ++pass)
                CHECK(pass_begins[sub][pass] == 1 && pass_ends[sub][pass] == 1 &&
                    includes[sub][pass] == 1 && linked[sub][pass] == entries_per_pass);
        /* An absent later-pass command still changes the modeled identity. */
        passes[7].state.culling.val += 10;
        CHECK(check_same_source_changed_model(&fixture, &source, &inventory));
        passes[7].state.culling.val -= 10;
        /* The digest must inspect the actual later pass, not pass zero's
         * matching target/program. Absent tier and inner-row values leave
         * emitted text unchanged but still belong to their current owner. */
        ++programs[7][1].hardware_tier;
        CHECK(check_same_source_changed_model(&fixture, &source, &inventory));
        --programs[7][1].hardware_tier;
        ++identities[7][1].inner_subprogram_index;
        CHECK(check_same_source_changed_model(&fixture, &source, &inventory));
        --identities[7][1].inner_subprogram_index;
        programs[7][1].shader_requirements ^= 1;
        CHECK(check_bad_inventory(&fixture, &source, &inventory));
        programs[7][1].shader_requirements ^= 1;
        ++programs[7][1].blob_index;
        CHECK(check_bad_inventory(&fixture, &source, &inventory));
        --programs[7][1].blob_index;
        ++identities[7][1].hardware_tier_group;
        CHECK(check_bad_inventory(&fixture, &source, &inventory));
        --identities[7][1].hardware_tier_group;
        /* Complete pass/subshader moves cannot reuse receipts even when all
         * generated syntax is identical and source hashes remain unchanged. */
        SerializedPass moved_pass = passes[6]; passes[6] = passes[7]; passes[7] = moved_pass;
        CHECK(check_same_source_changed_model(&fixture, &source, &inventory));
        moved_pass = passes[6]; passes[6] = passes[7]; passes[7] = moved_pass;
        SerializedSubShader moved_subshader = subshaders[2];
        subshaders[2] = subshaders[3]; subshaders[3] = moved_subshader;
        CHECK(check_same_source_changed_model(&fixture, &source, &inventory));
        moved_subshader = subshaders[2]; subshaders[2] = subshaders[3]; subshaders[3] = moved_subshader;
        for (size_t index = 0; index < inventory.receipt_count; ++index) {
            ShaderLabSourceSyntaxReceipt *receipt = &inventory.receipts[index];
            if (receipt->subshader_index != 3 || receipt->pass_index != 1) continue;
            const ShaderLabSourceSyntaxReceipt saved = *receipt;
            receipt->pass_index = 0;
            CHECK(check_bad_inventory(&fixture, &source, &inventory)); *receipt = saved;
            receipt->subshader_index = 2;
            CHECK(check_bad_inventory(&fixture, &source, &inventory)); *receipt = saved;
        }
        ShaderLabExpressionSourceRecord *entry = &inventory.entries.records[inventory.entries.count - 1];
        --entry->pass_index;
        CHECK(check_bad_inventory(&fixture, &source, &inventory)); ++entry->pass_index;
        --entry->subshader_index;
        CHECK(check_bad_inventory(&fixture, &source, &inventory)); ++entry->subshader_index;
        const ShaderLabSourceQualityInventory before = inventory;
        const ShaderLabSourceSyntaxKind observed_kinds[] = {SHADERLAB_SOURCE_SYNTAX_PASS_BEGIN,
            SHADERLAB_SOURCE_SYNTAX_RENDER_STATE, SHADERLAB_SOURCE_SYNTAX_INCLUDE_POLICY,
            SHADERLAB_SOURCE_SYNTAX_STAGE_GUARD, SHADERLAB_SOURCE_SYNTAX_ROUTING,
            SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY, SHADERLAB_SOURCE_SYNTAX_PASS_END};
        for (size_t index = 0; index < sizeof(observed_kinds) / sizeof(observed_kinds[0]); ++index) {
            ReceiptRejection rejection = {3, 1, observed_kinds[index]};
            ShaderLabSourceQualityRequest observed = request;
            observed.observer = reject_selected_receipt; observed.observer_context = &rejection;
            passes[7].state.culling.present = observed_kinds[index] == SHADERLAB_SOURCE_SYNTAX_RENDER_STATE;
            if (passes[7].state.culling.present) passes[7].state.culling.val = 1;
            CHECK(shaderlab_source_quality_emit(&observed, &rejected, &inventory, NULL) ==
                SHADERLAB_SOURCE_QUALITY_OBSERVER_REJECTED);
            CHECK(!rejected.len && !memcmp(&before, &inventory, sizeof(before)));
            passes[7].state.culling.present = false; passes[7].state.culling.val = 7;
        }
        const SerializedPass ordinary = passes[7];
        passes[7].pass_type = 1;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary; passes[7].pass_type = 2;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary; passes[7].use_name = "External/Shader/PASS";
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary; passes[7].has_instancing_variant = true;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary; passes[7].has_procedural_instancing_variant = true;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary;
        int other_platform = 5;
        passes[7].platforms = &other_platform;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary;
        uint16_t invalid_keyword = 0;
        passes[7].serialized_keyword_state_mask_count = 1;
        passes[7].serialized_keyword_state_mask = &invalid_keyword;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_EMISSION_FAILED);
        passes[7] = ordinary;
        for (int stage = 0; stage < 6; ++stage) passes[7].subprogram_count[stage] = 0;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary; passes[7].subprogram_count[5] = 1; passes[7].subprograms[5] = &fixture.programs[0];
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        passes[7] = ordinary;
        if (stage_family == 2) {
            passes[7].subprogram_count[4] = 0;
            CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
            passes[7] = ordinary;
        }
        fixture.shader.subshader_count = 5;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        fixture.shader.subshader_count = 4;
        subshaders[3].pass_count = 3;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        subshaders[3].pass_count = 2;
        subshaders[3].pass_count = 0;
        CHECK(shaderlab_source_quality_emit(&request, &rejected, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE);
        subshaders[3].pass_count = 2;
        CHECK(!rejected.len && !memcmp(&before, &inventory, sizeof(before)));
        CHECK(shaderlab_source_quality_inventory_analyze(&request, &source, &inventory,
            &quality, NULL) == SHADERLAB_SOURCE_QUALITY_OK);
        shaderlab_source_quality_inventory_dispose(&inventory);
        sb_free(&source); sb_free(&baseline); sb_free(&rejected); fixture_dispose(&fixture);
    }
    return true;
}

int main(void) {
    Fixture fixture;
    if (!fixture_init(&fixture)) return 1;
    StringBuilder source, baseline; sb_init(&source); sb_init(&baseline);
    ShaderLabSourceQualityInventory inventory = {0};
    const ShaderLabSourceQualityRequest request = {.shader = &fixture.shader, .archive = &fixture.archive};
    bool passed = shaderlab_source_quality_emit(&request, &source, &inventory, NULL) == SHADERLAB_SOURCE_QUALITY_OK;
    passed = passed && shaderlab_emit_high_level_candidate(&fixture.shader, fixture.archive.entries,
        fixture.archive.entry_count, fixture.archive.segments, fixture.archive.segment_lengths,
        fixture.archive.segment_count, &baseline, NULL);
    passed = passed && source.len == baseline.len && !memcmp(source.buf, baseline.buf, source.len);
    passed = passed && check_current_inventory(&fixture, &source, &inventory) &&
        check_inventory_mutations(&fixture, &source, &inventory) &&
        check_transaction_and_model(&fixture, &source, &inventory) &&
        check_named_wrapper_fields(&fixture) && check_keyword_routing() &&
        check_texture_properties_and_schema_guards() && check_linked_graphics_stages() &&
        check_inventory_limits() && check_multiple_passes_and_subshaders();
    shaderlab_source_quality_inventory_dispose(&inventory);
    sb_free(&source); sb_free(&baseline); fixture_dispose(&fixture);
    passed = passed && g_allocated_bytes == 0 && g_allocations_count == 0;
    return passed ? 0 : 1;
}
