// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_source_quality.h"
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
    CommonFileBytes bytes;
    SerializedShader shader;
    SerializedSubShader subshader;
    SerializedPass pass;
    SerializedSubProgram programs[2];
    SerializedSubProgramIdentity identities[2];
    ParsedShaderProperty properties[2];
    int platform;
    ShaderBlobArchive archive;
    BlobEntry entries[2];
    uint8_t *segments[2];
    int lengths[2];
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
    for (int stage = 0; stage < 2; ++stage) free(fixture->segments[stage]);
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
    CHECK(result.linked_entry_count == 2 && result.wrapper_receipt_count > 12);
    CHECK(result.required_external_include_root_count == 1);
    return true;
}

static bool check_bad_inventory(Fixture *fixture, StringBuilder *source,
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
        ++entry->serialized_state;
        CHECK(check_bad_inventory(fixture, source, inventory)); --entry->serialized_state;
        entry->target_digest[0] ^= 1;
        CHECK(check_bad_inventory(fixture, source, inventory)); entry->target_digest[0] ^= 1;
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
        check_texture_properties_and_schema_guards();
    shaderlab_source_quality_inventory_dispose(&inventory);
    sb_free(&source); sb_free(&baseline); fixture_dispose(&fixture);
    passed = passed && g_allocated_bytes == 0 && g_allocations_count == 0;
    return passed ? 0 : 1;
}
