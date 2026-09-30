// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_lift_internal.h"
#include "common/file_io.h"
#include "dxbc/usbd.h"
#include "test_shaderlab_fixture.h"

#define DXBCSANDBOX_CLI_UNITY_COMPILER 1
#include "cli/shaderlab_lift_cli.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #condition); return false; \
} } while (0)

typedef struct {
    CommonFileBytes bytes;
    SerializedShader shader;
    SerializedSubShader subshader;
    SerializedPass pass;
    SerializedSubProgram programs[2];
    SerializedSubProgramIdentity identities[2];
    int platform;
    ShaderBlobArchive archive;
    BlobEntry entries[2];
    uint8_t *segments[2];
    int lengths[2];
} Fixture;

static bool fixture_init(Fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    CHECK(common_file_read_regular(SHADERLAB_INVENTORY_TEST_FIXTURE, 1024 * 1024,
        &fixture->bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, fixture->bytes.data, fixture->bytes.size, NULL));
    CHECK(table.record_count == 2);
    for (int stage = 0; stage < 2; ++stage) {
        DXBCUSBDRecordView record;
        CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &record));
        size_t size = 0;
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
    fixture->subshader = (SerializedSubShader){.pass_count = 1, .passes = &fixture->pass};
    fixture->shader = (SerializedShader){.name = "Fixture/Inventory/PrivacyMarker",
        .subshader_count = 1, .subshaders = &fixture->subshader};
    fixture->archive = (ShaderBlobArchive){.entries = fixture->entries, .entry_count = 2,
        .segments = fixture->segments, .segment_lengths = fixture->lengths, .segment_count = 2};
    return true;
}

static void fixture_dispose(Fixture *fixture) {
    for (int stage = 0; stage < 2; ++stage) free(fixture->segments[stage]);
    common_file_bytes_dispose(&fixture->bytes);
}

static bool observed_inventory(Fixture *fixture, UnityShaderLabLiftArtifact *artifact) {
    const UnityShaderLabLiftInput input = {.shader = &fixture->shader, .archive = &fixture->archive};
    unity_shaderlab_lift_record_inventory(&input, artifact);
    CHECK(artifact->bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_OBSERVED);
    const ShaderLabSourceQualityResult *quality = &artifact->bounded_source_inventory.quality;
    CHECK(quality->classification == HLSL_SOURCE_QUALITY_MIXED && quality->wrapper_complete);
    CHECK(quality->gaps & SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE);
    CHECK(quality->gaps & SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY);
    CHECK(quality->gaps & SHADERLAB_SOURCE_GAP_SCHEMA_AUTHORITY);
    CHECK(!artifact->bounded_source_inventory.has_structural_authority);
    CHECK(quality->required_external_include_root_count == 1 && quality->linked_entry_count == 2);
    CHECK(shaderlab_expression_source_map_matches_source(&artifact->source_map, &artifact->source));
    CHECK(unity_shaderlab_lift_inventory_matches_source(artifact, artifact->source.len,
        artifact->bounded_source_inventory.source_digest));
    return true;
}

static bool test_inventory(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    UnityShaderLabLiftArtifact artifact = {.attempted = true, .high_level = true,
        .emission_attempted = true};
    sb_init(&artifact.source);
    CHECK(shaderlab_emit_high_level_candidate_with_source_map(&fixture.shader,
        fixture.archive.entries, fixture.archive.entry_count, fixture.archive.segments,
        fixture.archive.segment_lengths, fixture.archive.segment_count, &artifact.source,
        &artifact.source_map, NULL));
    char *source_copy = malloc(artifact.source.len);
    CHECK(source_copy); memcpy(source_copy, artifact.source.buf, artifact.source.len);
    const size_t original_count = artifact.source_map.count;
    const HLSLSourceQualityResult stage_quality = artifact.source_map.records[0].source_quality;
    CHECK(observed_inventory(&fixture, &artifact));
    CHECK(!memcmp(source_copy, artifact.source.buf, artifact.source.len));
    CHECK(artifact.source_map.count == original_count);
    CHECK(!memcmp(&stage_quality, &artifact.source_map.records[0].source_quality, sizeof(stage_quality)));
    StringBuilder json; sb_init(&json);
    CHECK(unity_shaderlab_lift_append_inventory_json(&artifact, &json));
    CHECK(strstr(json.buf, "\"status\":\"observed\"") && strstr(json.buf, "\"classification\":\"mixed\""));
    CHECK(strstr(json.buf, "schema-authority") && strstr(json.buf, "dependency-inventory"));
    CHECK(!strstr(json.buf, fixture.shader.name));
    /* Reporting retains a historical producer observation. It has no model
     * pointer or receipts with which to replay a subsequently changed model. */
    uint8_t modeled_digest[32];
    memcpy(modeled_digest, artifact.bounded_source_inventory.modeled_input_digest, 32);
    const char *original_name = fixture.shader.name;
    fixture.shader.name = "Fixture/Inventory/ChangedAfterObservation";
    sb_clear(&json);
    CHECK(unity_shaderlab_lift_append_inventory_json(&artifact, &json));
    CHECK(strstr(json.buf, "\"status\":\"observed\""));
    CHECK(!memcmp(modeled_digest, artifact.bounded_source_inventory.modeled_input_digest, 32));
    CHECK(!strstr(json.buf, fixture.shader.name));
    fixture.shader.name = original_name;
    uint8_t wrong_digest[32]; memcpy(wrong_digest, artifact.bounded_source_inventory.source_digest, 32);
    wrong_digest[0] ^= 1;
    CHECK(!unity_shaderlab_lift_inventory_matches_source(&artifact, artifact.source.len, wrong_digest));
    CHECK(!unity_shaderlab_lift_inventory_matches_source(&artifact, artifact.source.len + 1,
        artifact.bounded_source_inventory.source_digest));
    const char saved_byte = artifact.source.buf[0]; artifact.source.buf[0] ^= 1;
    CHECK(!unity_shaderlab_lift_inventory_matches_source(&artifact, artifact.source.len,
        artifact.bounded_source_inventory.source_digest));
    sb_clear(&json); CHECK(unity_shaderlab_lift_append_inventory_json(&artifact, &json));
    CHECK(strstr(json.buf, "source-binding-mismatch") && strstr(json.buf, "\"quality\":null"));
    artifact.source.buf[0] = saved_byte;
    CHECK(observed_inventory(&fixture, &artifact));

    UnityShaderLabLiftInput input = {.shader = &fixture.shader, .archive = &fixture.archive};
    ShaderObject unpaired = {.shader = fixture.shader, .decoded = true,
        .profile = SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1};
    input.source_object = &unpaired;
    unity_shaderlab_lift_record_inventory(&input, &artifact);
    CHECK(artifact.bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_INVALID_ARGUMENT);
    input.shader = &unpaired.shader;
    unity_shaderlab_lift_record_inventory(&input, &artifact);
    CHECK(artifact.bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_SCHEMA_FAILED);
    CHECK(artifact.bounded_source_inventory.diagnostic.structure.status == SHADERLAB_STRUCTURE_SCHEMA_AUTHORITY_FAILED);
    input.shader = &fixture.shader; input.source_object = NULL;
    fixture.shader.dependency_count = 1;
    unity_shaderlab_lift_record_inventory(&input, &artifact);
    CHECK(artifact.bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_SCOPE_UNAVAILABLE);
    fixture.shader.dependency_count = 0;
    fixture.subshader.pass_count = 2;
    unity_shaderlab_lift_record_inventory(&input, &artifact);
    CHECK(artifact.bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_SCOPE_UNAVAILABLE);
    fixture.subshader.pass_count = 1;
    CHECK(observed_inventory(&fixture, &artifact));
    artifact.high_level = false;
    unity_shaderlab_lift_record_inventory(&input, &artifact);
    CHECK(artifact.bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_UNSUPPORTED_MODE);
    artifact.high_level = true; artifact.unity_uv_helpers = true;
    unity_shaderlab_lift_record_inventory(&input, &artifact);
    CHECK(artifact.bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_UNSUPPORTED_MODE);
    artifact.unity_uv_helpers = false;
    CHECK(observed_inventory(&fixture, &artifact));
    artifact.source.buf[0] ^= 1;
    unity_shaderlab_lift_record_inventory(&input, &artifact);
    CHECK(artifact.bounded_source_inventory.status == UNITY_SHADERLAB_INVENTORY_SOURCE_MISMATCH);
    artifact.source.buf[0] = saved_byte;
    CHECK(observed_inventory(&fixture, &artifact));

    /* Compiler status is a separate dimension; source observations survive a
     * rejected comparison, but selected fallback JSON uses only its own view. */
    ShaderBatchRecordResult publication = {.publication_authorized = true,
        .published_shader_content_recorded = true, .published_shader_size = artifact.source.len};
    memcpy(publication.published_shader_digest, artifact.bounded_source_inventory.source_digest, 32);
    sb_clear(&json);
    CHECK(append_published_artifact_inventory(&artifact, true, &publication, &json));
    CHECK(strstr(json.buf, "\"status\":\"observed\""));
    publication.published_shader_digest[0] ^= 1;
    sb_clear(&json); CHECK(append_published_artifact_inventory(&artifact, true, &publication, &json));
    CHECK(strstr(json.buf, "source-binding-mismatch") && strstr(json.buf, "\"quality\":null"));
    publication.published_shader_digest[0] ^= 1;
    ++publication.published_shader_size;
    sb_clear(&json); CHECK(append_published_artifact_inventory(&artifact, true, &publication, &json));
    CHECK(strstr(json.buf, "source-binding-mismatch")); --publication.published_shader_size;
    publication.publication_authorized = false;
    sb_clear(&json); CHECK(append_published_artifact_inventory(&artifact, true, &publication, &json));
    CHECK(strstr(json.buf, "not-published") && strstr(json.buf, "\"quality\":null"));
    publication.publication_authorized = true; publication.source_identity_close_deferred = true;
    sb_clear(&json); CHECK(append_published_artifact_inventory(&artifact, true, &publication, &json));
    CHECK(strstr(json.buf, "identity-close-deferred")); publication.source_identity_close_deferred = false;
    publication.publication_residue = true;
    sb_clear(&json); CHECK(append_published_artifact_inventory(&artifact, true, &publication, &json));
    CHECK(strstr(json.buf, "publication-residue")); publication.publication_residue = false;
    sb_clear(&json); CHECK(append_published_artifact_inventory(&artifact, false, &publication, &json));
    CHECK(strstr(json.buf, "not-run"));
    sb_clear(&json); CHECK(append_published_artifact_inventory(NULL, true, &publication, &json));
    CHECK(strstr(json.buf, "source-binding-mismatch"));
    artifact.status = HLSL_LIFT_DXBC_MISMATCH;
    UnityShaderLabLiftResult result = {.candidate = artifact};
    result.baseline.attempted = true;
    result.baseline.bounded_source_inventory.status = UNITY_SHADERLAB_INVENTORY_UNSUPPORTED_MODE;
    result.accepted = &result.baseline;
    char *report = unity_shaderlab_lift_format_json(&result); CHECK(report);
    CHECK(strstr(report, "\"whole_source_quality\":\"unavailable\""));
    CHECK(strstr(report, "\"classification\":\"mixed\""));
    char *last = report;
    for (char *next = strstr(last + 1, "\"bounded_source_inventory\""); next;
         next = strstr(last + 1, "\"bounded_source_inventory\"")) last = next;
    CHECK(strstr(last, "unsupported-mode") && !strstr(last, "\"classification\":\"mixed\""));
    free(report);
    result.accepted = &result.candidate;
    report = unity_shaderlab_lift_format_json(&result); CHECK(report);
    CHECK(strstr(report, "\"classification\":\"mixed\"")); free(report);
    sb_free(&json); free(source_copy);
    shaderlab_expression_source_map_free(&artifact.source_map); sb_free(&artifact.source);
    fixture_dispose(&fixture);
    return true;
}

int main(void) {
    if (!test_inventory()) return 1;
    return g_allocated_bytes == 0 && g_allocations_count == 0 ? 0 : 1;
}
