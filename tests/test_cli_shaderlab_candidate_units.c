// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define main dxbc_sandbox_cli_embedded_main
#endif
#include "cli/dxbc_sandbox_cli.c"
#ifndef _WIN32
#undef main
#endif
#include "common/file_io.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/usbd.h"
#include "test_shaderlab_fixture.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

enum { STAGE_COUNT = 2, STATE_COUNT = 2, TIER_COUNT = 3,
       ROW_COUNT = STATE_COUNT * TIER_COUNT, BLOB_COUNT = STAGE_COUNT * STATE_COUNT };

typedef struct {
    CommonFileBytes bytes;
    ShaderObject object;
    SerializedSubShader subshaders[5];
    SerializedPass pass;
    SerializedSubProgram programs[STAGE_COUNT][ROW_COUNT];
    SerializedSubProgramIdentity identities[STAGE_COUNT][ROW_COUNT];
    char *keyword;
    uint8_t keyword_flag;
    uint16_t keyword_mask;
    int keyword_index, platform;
    ShaderBlobArchive archive;
    BlobEntry entries[BLOB_COUNT];
    uint8_t *segments[BLOB_COUNT];
    int lengths[BLOB_COUNT];
} Fixture;

static bool fixture_init(Fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    CHECK(common_file_read_regular(CLI_CANDIDATE_TEST_FIXTURE, 1024 * 1024,
                                  &fixture->bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, fixture->bytes.data, fixture->bytes.size, NULL));
    CHECK(table.record_count == STAGE_COUNT);
    fixture->keyword = (char *)"FEATURE";
    fixture->keyword_flag = 1;
    fixture->platform = 4;
    for (int stage = 0; stage < STAGE_COUNT; ++stage) {
        DXBCUSBDRecordView target;
        CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &target));
        for (int state = 0; state < STATE_COUNT; ++state) {
            const int blob = stage * STATE_COUNT + state;
            size_t size = 0;
            fixture->segments[blob] = test_shaderlab_variant_blob(target.dxbc,
                target.dxbc_size, stage ? 17 : 15, state ? fixture->keyword : NULL, &size);
            CHECK(fixture->segments[blob] && size <= INT32_MAX);
            fixture->lengths[blob] = (int)size;
            fixture->entries[blob] = (BlobEntry){0, (int32_t)size, blob};
            for (int tier = 0; tier < TIER_COUNT; ++tier) {
                const int row = state * TIER_COUNT + tier;
                fixture->programs[stage][row] = (SerializedSubProgram){
                    .blob_index = blob, .program_type = stage ? 17 : 15,
                    .shader_requirements = 0xe3, .local_keyword_count = state,
                    .local_keywords = state ? &fixture->keyword : NULL};
                fixture->identities[stage][row] = (SerializedSubProgramIdentity){
                    .hardware_tier_group = tier, .local_keyword_index_count = state,
                    .local_keyword_indices = state ? &fixture->keyword_index : NULL};
            }
        }
        fixture->pass.subprogram_count[stage] = ROW_COUNT;
        fixture->pass.subprograms[stage] = fixture->programs[stage];
        fixture->pass.subprogram_identities[stage] = fixture->identities[stage];
    }
    fixture->pass.name = "Candidate";
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = &fixture->platform;
    fixture->pass.program_mask = 6;
    fixture->pass.serialized_keyword_state_mask_count = 1;
    fixture->pass.serialized_keyword_state_mask = &fixture->keyword_mask;
    for (size_t index = 0; index < 5; ++index)
        fixture->subshaders[index] = (SerializedSubShader){.pass_count = 1, .passes = &fixture->pass};
    fixture->object.shader = (SerializedShader){.name = "Fixture/PortableCandidate",
        .subshader_count = 1, .subshaders = fixture->subshaders,
        .keyword_names = {1, &fixture->keyword}, .keyword_flags = &fixture->keyword_flag};
    /* This is a controlled projected model, with no fabricated schema tree.
     * A separate structural inventory must report that authority unavailable. */
    fixture->object.decoded = true;
    fixture->object.profile = SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1;
    fixture->archive = (ShaderBlobArchive){.entries = fixture->entries,
        .entry_count = BLOB_COUNT, .segments = fixture->segments,
        .segment_lengths = fixture->lengths, .segment_count = BLOB_COUNT};
    return true;
}

static void fixture_dispose(Fixture *fixture) {
    for (int blob = 0; blob < BLOB_COUNT; ++blob) free(fixture->segments[blob]);
    common_file_bytes_dispose(&fixture->bytes);
    memset(fixture, 0, sizeof(*fixture));
}

static ShaderBatchCandidateInput candidate_input(Fixture *fixture, size_t record) {
    return (ShaderBatchCandidateInput){.catalog_record_index = record,
        .object = &fixture->object, .archive = &fixture->archive,
        .source_path = "output/PortableCandidate.shader", .source_directory = "output",
        .source_basename = "PortableCandidate.shader"};
}

static bool ordinary_high_level(Fixture *fixture, StringBuilder *source,
                               ShaderLabExpressionSourceMap *map) {
    CHECK(shaderlab_emit_high_level_candidate_with_source_map(&fixture->object.shader,
        fixture->archive.entries, fixture->archive.entry_count, fixture->archive.segments,
        fixture->archive.segment_lengths, fixture->archive.segment_count, source, map, NULL));
    CHECK(shaderlab_expression_source_map_matches_source(map, source));
    CHECK(map->count == (size_t)fixture->object.shader.subshader_count * STAGE_COUNT * ROW_COUNT);
    for (size_t index = 0; index < map->count; ++index) {
        const ShaderLabExpressionSourceRecord *entry = &map->records[index];
        const int body = (int)(index % (STAGE_COUNT * ROW_COUNT));
        const int stage_body = body % ROW_COUNT;
        const int state = stage_body % STATE_COUNT;
        const int tier = stage_body / STATE_COUNT;
        CHECK(entry->subshader_index == (int)(index / (STAGE_COUNT * ROW_COUNT)));
        CHECK(entry->pass_index == 0 && entry->stage_index == body / ROW_COUNT);
        CHECK(entry->subprogram_index == state * TIER_COUNT + tier);
        CHECK(entry->hardware_tier_group == tier);
        CHECK(entry->serialized_state == (size_t)state);
        CHECK(entry->blob_index == entry->stage_index * STATE_COUNT + (int)entry->serialized_state);
        CHECK(entry->has_source_quality && entry->instructions.complete && entry->instructions.count);
        if (entry->stage_index == 1) {
            CHECK(entry->source_quality.classification == HLSL_SOURCE_QUALITY_MIXED);
            CHECK(entry->source_quality.reasons & HLSL_SOURCE_QUALITY_REASON_RESIDUAL);
            CHECK(entry->source_quality.counts.lane_transport && entry->source_quality.counts.residual_total);
        }
    }
    CHECK(!strstr(source->buf, "float4 r0") && !strstr(source->buf, "unsupported stage"));
    return true;
}

static ShaderBatchRecordResult publication_for_source(const StringBuilder *source) {
    ShaderBatchRecordResult publication = {.status = SHADER_BATCH_EMITTED,
        .publication_authorized = true, .published_shader_content_recorded = true,
        .published_shader_size = source->len};
    common_sha256(source->buf, source->len, publication.published_shader_digest);
    return publication;
}

static bool append_record(const CliShaderLabLift *lift, size_t record, StringBuilder *json) {
    sb_clear(json);
    CHECK(cli_shaderlab_lift_append_json(lift, record, json));
    CHECK(sb_ok(json));
    return true;
}

static size_t count_text(const char *source, const char *needle) {
    size_t count = 0;
    for (const char *found = source; (found = strstr(found, needle)) != NULL; found += strlen(needle))
        ++count;
    return count;
}

static bool published_status(const CliShaderLabLift *lift, size_t record,
                             const ShaderBatchRecordResult *publication,
                             const char *status) {
    StringBuilder json; sb_init(&json);
    CHECK(cli_shaderlab_lift_append_published_inventory_json(lift, record, publication, &json));
    char expected[96];
    const int count = snprintf(expected, sizeof(expected), "\"status\":\"%s\"", status);
    CHECK(count > 0 && (size_t)count < sizeof(expected) && strstr(json.buf, expected));
    sb_free(&json);
    return true;
}

static bool check_source_only_reports(const CliShaderLabLift *lift,
                                     const ShaderBatchRecordResult *publication,
                                     bool generated) {
    ShaderCatalogRecord records[2];
    memset(records, 0, sizeof(records));
    for (size_t index = 0; index < 2; ++index) {
        records[index].class_id = 48;
        records[index].path_id = (int64_t)index + 1;
        records[index].status = SHADER_CATALOG_RECORD_READY;
        records[index].name = (char *)(index ? "Fixture/NotAttempted" : "Fixture/Published");
        records[index].outer_path = (char *)"fixture.assets";
    }
    ShaderCatalog catalog; shader_catalog_init(&catalog);
    catalog.records = records; catalog.record_count = 2;
    catalog.stats.shader_objects = catalog.stats.ready_shaders = 2;
    ShaderBatchRecordResult outputs[2] = {*publication, {0}};
    outputs[0].output_path = (char *)"output/PortableCandidate.shader";
    outputs[1].status = SHADER_BATCH_FAILED;
    outputs[1].failure = SHADER_BATCH_FAILURE_CATALOG_NOT_READY;
    ShaderBatchResult batch = {.catalog_authority = &catalog, .records = outputs,
        .record_count = 2, .stats = {.selected = 2, .emitted = generated ? 1 : 0,
                                   .failed = generated ? 1 : 2}};
    bool selected[2] = {true, true}, texture_complete = false;
    StringBuilder report; sb_init(&report);
    CHECK(render_extract_json(&catalog, selected, &batch, NULL, NULL,
        CLI_SHADER_KIND_GRAPHICS, NULL, NULL, &report, &texture_complete, lift));
    CHECK(texture_complete && strstr(report.buf, "\"mode\":\"source-only\""));
    CHECK(strstr(report.buf, generated ? "\"requested\":2,\"high_level_candidates\":1" :
                                        "\"requested\":2,\"high_level_candidates\":0"));
    CHECK(strstr(report.buf, generated ? "\"unavailable\":0,\"not_run\":1,\"published_generated\":1" :
                                        "\"unavailable\":1,\"not_run\":1,\"published_generated\":0"));
    CHECK(strstr(report.buf, "\"published_local_domain_verified\":0") &&
          strstr(report.buf, "\"published_high_level_source\":false"));
    CHECK((strstr(report.buf, "\"published_high_level_source\":true") != NULL) == generated);
    CHECK(strstr(report.buf, "\"d3d11\":\"not-run\"") &&
          strstr(report.buf, "\"variant_selection\":\"not-run\""));
    CHECK(strstr(report.buf, "\"complete\":false") &&
          !strstr(report.buf, "structural-with-local-domain-reports") &&
          !strstr(report.buf, "see-local-domain-records") &&
          !strstr(report.buf, "\"published_local_domain_verified\":true"));
    sb_clear(&report);
    CHECK(render_extract_table(&catalog, selected, &batch, NULL, NULL,
        CLI_SHADER_KIND_GRAPHICS, false, "bundled.registry", "digest", &report, &texture_complete, lift));
    CHECK(strstr(report.buf, generated ? "high-level (unverified)" : "unavailable") &&
          strstr(report.buf, generated ? "requested=2 high-level=1" : "requested=2 high-level=0"));
    CHECK(strstr(report.buf, generated ? "unavailable=0 not-run=1 published-generated=1 published-local-domain-verified=0" :
                                        "unavailable=1 not-run=1 published-generated=0 published-local-domain-verified=0"));
    CHECK(strstr(report.buf, "checks were not run") &&
          !strstr(report.buf, "complete local D3D11 program domains"));
    outputs[0].published_shader_digest[0] ^= 1;
    sb_clear(&report);
    CHECK(render_extract_json(&catalog, selected, &batch, NULL, NULL,
        CLI_SHADER_KIND_GRAPHICS, NULL, NULL, &report, &texture_complete, lift));
    CHECK(strstr(report.buf, "\"published_generated\":0") &&
          !strstr(report.buf, "\"published_high_level_source\":true"));
    sb_free(&report);
    return true;
}

static bool test_source_only_generation(void) {
    Fixture fixture; CHECK(fixture_init(&fixture));
    CHECK(cli_shaderlab_lift_supported() && !cli_shaderlab_lift_verifier_supported());
    const CliShaderLabLiftOptions options = {.enabled = true};
    CliShaderLabLift *lift = cli_shaderlab_lift_create(&options, 2);
    CHECK(lift && !cli_shaderlab_lift_verification_requested(lift));
    CHECK(!strcmp(cli_shaderlab_lift_selection(lift, 0), "not-run"));
    ShaderBatchOptions batch_options; shader_batch_options_default(&batch_options);
    cli_shaderlab_lift_attach(lift, &batch_options);
    CHECK(batch_options.select_candidate && batch_options.candidate_context);
    StringBuilder canonical, selected, json;
    sb_init(&canonical); sb_init(&selected); sb_init(&json);
    ShaderLabExpressionSourceMap map = {0};
    CHECK(ordinary_high_level(&fixture, &canonical, &map));
    ShaderBatchCandidateInput input = candidate_input(&fixture, 0);
    ShaderLabCandidateDiagnostic diagnostic;
    sb_append(&selected, "caller-owned destination");
    CHECK(!batch_options.select_candidate(batch_options.candidate_context, &input, &selected, &diagnostic));
    CHECK(!strcmp(selected.buf, "caller-owned destination") &&
          !strcmp(cli_shaderlab_lift_selection(lift, 0), "not-run"));
    sb_clear(&selected);
    input.catalog_record_index = 2;
    CHECK(!batch_options.select_candidate(batch_options.candidate_context, &input, &selected, &diagnostic));
    CHECK(!selected.len && !strcmp(cli_shaderlab_lift_selection(lift, 0), "not-run"));
    input.catalog_record_index = 0;
    CHECK(batch_options.select_candidate(batch_options.candidate_context, &input, &selected, &diagnostic));
    CHECK(diagnostic.status == SHADERLAB_CANDIDATE_OK && selected.len == canonical.len &&
          !memcmp(selected.buf, canonical.buf, selected.len));
    CHECK(!strcmp(cli_shaderlab_lift_selection(lift, 0), "high-level"));
    ShaderBatchRecordResult publication = publication_for_source(&selected);
    CHECK(cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(!cli_shaderlab_lift_output_verified(lift, 0, &publication));
    CHECK(check_source_only_reports(lift, &publication, true));
    CHECK(append_record(lift, 0, &json));
    CHECK(strstr(json.buf, "\"mode\":\"source-only\"") &&
          strstr(json.buf, "\"attempted\":true") && strstr(json.buf, "\"generated\":true"));
    CHECK(strstr(json.buf, "\"compiler\":\"not-run\"") &&
          strstr(json.buf, "\"exact_dxbc\":\"not-run\"") &&
          strstr(json.buf, "\"unity_import\":\"not-run\"") &&
          strstr(json.buf, "\"native_d3d11\":\"not-run\""));
    CHECK(strstr(json.buf, "\"requested_rows_complete\":true"));
    CHECK(strstr(json.buf, "\"requested_variants\":12") && strstr(json.buf, "\"emitted_bodies\":12"));
    CHECK(count_text(json.buf, "\"source_quality_scope\":\"emitted-stage-entry\"") == map.count);
    StringBuilder quality_json; sb_init(&quality_json);
    for (size_t index = 0; index < map.count; ++index) {
        const ShaderLabExpressionSourceRecord *entry = &map.records[index];
        sb_clear(&quality_json);
        CHECK(hlsl_source_quality_append_json(&entry->source_quality, &quality_json));
        CHECK(strstr(json.buf, quality_json.buf));
        char coordinates[144];
        const int count = snprintf(coordinates, sizeof(coordinates),
            "\"subshader\":%d,\"pass\":%d,\"stage\":%d,\"subprogram\":%d,\"blob\":%d,\"tier\":%d",
            entry->subshader_index, entry->pass_index, entry->stage_index,
            entry->subprogram_index, entry->blob_index, entry->hardware_tier_group);
        CHECK(count > 0 && (size_t)count < sizeof(coordinates) && count_text(json.buf, coordinates) == 2);
    }
    sb_free(&quality_json);
    char digest[65]; common_sha256_digest_to_hex(publication.published_shader_digest, digest);
    CHECK(strstr(json.buf, digest) && !strstr(json.buf, fixture.object.shader.name) &&
          !strstr(json.buf, input.source_path));
    CHECK(published_status(lift, 0, &publication, "schema-failed"));
    char *frozen_json = malloc(json.len + 1);
    CHECK(frozen_json); memcpy(frozen_json, json.buf, json.len + 1);
    fixture_dispose(&fixture);
    CHECK(append_record(lift, 0, &json) && !strcmp(json.buf, frozen_json));
    CHECK(cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(!cli_shaderlab_lift_output_verified(lift, 0, &publication));

    const ShaderBatchRecordResult saved = publication;
    publication.published_shader_digest[0] ^= 1;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(published_status(lift, 0, &publication, "source-binding-mismatch"));
    publication = saved; ++publication.published_shader_size;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(published_status(lift, 0, &publication, "source-binding-mismatch"));
    publication = saved; publication.source_identity_close_deferred = true;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(published_status(lift, 0, &publication, "identity-close-deferred"));
    publication = saved; publication.publication_residue = true;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(published_status(lift, 0, &publication, "publication-residue"));
    publication = saved; publication.shader_publication_residue = true;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    publication = saved; publication.meta_publication_residue = true;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    publication = saved; publication.publication_authorized = false;
    publication.status = SHADER_BATCH_FAILED;
    publication.failure = SHADER_BATCH_FAILURE_OUTPUT_COLLISION;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(published_status(lift, 0, &publication, "not-published"));
    publication.publication_authorized = true;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(published_status(lift, 0, &publication, "not-published"));
    publication = saved; publication.published_shader_content_recorded = false;
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication));
    publication = saved;
    CHECK(cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(!cli_shaderlab_lift_output_generated(lift, 1, &publication));
    CHECK(!cli_shaderlab_lift_output_generated(lift, 2, &publication));
    free(frozen_json);
    shaderlab_expression_source_map_free(&map);
    sb_free(&canonical); sb_free(&selected); sb_free(&json);
    cli_shaderlab_lift_free(lift);
    return true;
}

static bool replace_with_saturated_pixel(Fixture *fixture) {
    DXBCUSBDTableView table;
    DXBCUSBDRecordView target;
    CHECK(dxbc_usbd_table_open(&table, fixture->bytes.data, fixture->bytes.size, NULL));
    CHECK(dxbc_usbd_table_record(&table, 1, &target));
    uint8_t *copy = malloc(target.dxbc_size);
    CHECK(copy); memcpy(copy, target.dxbc, target.dxbc_size);
    DXBCDocument document; dxbc_document_init(&document);
    CHECK(dxbc_document_parse(&document, copy, target.dxbc_size, NULL));
    bool changed = false;
    for (size_t index = 0; index < document.instruction_count; ++index) {
        const DXBCDocumentInstruction *instruction = &document.instructions[index];
        if (instruction->opcode != 56) continue;
        CHECK(instruction->byte_offset + 2 <= target.dxbc_size);
        copy[instruction->byte_offset + 1] |= 0x20;
        changed = true;
        break;
    }
    CHECK(changed && dxbc_compute_hash(copy, target.dxbc_size, copy + 4));
    dxbc_document_free(&document);
    size_t size = 0;
    uint8_t *replacement = test_shaderlab_variant_blob(copy, target.dxbc_size, 17,
                                                     fixture->keyword, &size);
    free(copy);
    CHECK(replacement && size <= INT32_MAX);
    const int blob = BLOB_COUNT - 1;
    free(fixture->segments[blob]); fixture->segments[blob] = replacement;
    fixture->lengths[blob] = (int)size;
    fixture->entries[blob].length = (int32_t)size;
    return true;
}

static bool test_selected_row_failure(void) {
    Fixture fixture; CHECK(fixture_init(&fixture));
    CHECK(replace_with_saturated_pixel(&fixture));
    StringBuilder low, source, json; sb_init(&low); sb_init(&source); sb_init(&json);
    CHECK(shaderlab_emit_candidate_with_diagnostic(&fixture.object.shader,
        fixture.archive.entries, fixture.archive.entry_count, fixture.archive.segments,
        fixture.archive.segment_lengths, fixture.archive.segment_count, &low, NULL));
    CHECK(strstr(low.buf, "saturate("));
    const CliShaderLabLiftOptions options = {.enabled = true};
    CliShaderLabLift *lift = cli_shaderlab_lift_create(&options, 2); CHECK(lift);
    ShaderBatchOptions batch_options; shader_batch_options_default(&batch_options);
    cli_shaderlab_lift_attach(lift, &batch_options);
    ShaderBatchCandidateInput input = candidate_input(&fixture, 0);
    ShaderLabCandidateDiagnostic diagnostic;
    CHECK(!batch_options.select_candidate(batch_options.candidate_context, &input, &source, &diagnostic));
    CHECK(!source.len && diagnostic.status == SHADERLAB_CANDIDATE_STAGE_FAILED &&
          diagnostic.stage.stage_index == 1 && diagnostic.stage.subprogram_index == TIER_COUNT);
    CHECK(!strcmp(cli_shaderlab_lift_selection(lift, 0), "unavailable"));
    CHECK(append_record(lift, 0, &json));
    CHECK(strstr(json.buf, "\"attempted\":true") && strstr(json.buf, "\"generated\":false") &&
          strstr(json.buf, "\"requested_rows_complete\":true") &&
          strstr(json.buf, "\"requested_variants\":12") && strstr(json.buf, "\"emitted_bodies\":0"));
    CHECK(strstr(json.buf, "\"source_sha256\":null") && !strstr(json.buf, "low-level-fallback"));
    ShaderBatchRecordResult publication = publication_for_source(&low);
    CHECK(!cli_shaderlab_lift_output_generated(lift, 0, &publication) &&
          !cli_shaderlab_lift_output_verified(lift, 0, &publication));
    publication.status = SHADER_BATCH_FAILED;
    publication.failure = SHADER_BATCH_FAILURE_CANDIDATE_SELECTION;
    publication.publication_authorized = false;
    CHECK(check_source_only_reports(lift, &publication, false));
    sb_append(&source, "caller-owned destination");
    CHECK(!batch_options.select_candidate(batch_options.candidate_context, &input, &source, &diagnostic));
    CHECK(!strcmp(source.buf, "caller-owned destination"));
    CHECK(append_record(lift, 0, &json) && strstr(json.buf, "\"generated\":false"));
    fixture_dispose(&fixture); cli_shaderlab_lift_free(lift);
    sb_free(&low); sb_free(&source); sb_free(&json);
    return true;
}

static bool test_inventory_scope_is_optional(void) {
    Fixture fixture; CHECK(fixture_init(&fixture));
    fixture.object.shader.subshader_count = 5;
    StringBuilder canonical, selected; sb_init(&canonical); sb_init(&selected);
    ShaderLabExpressionSourceMap map = {0};
    CHECK(ordinary_high_level(&fixture, &canonical, &map));
    const CliShaderLabLiftOptions options = {.enabled = true};
    CliShaderLabLift *lift = cli_shaderlab_lift_create(&options, 1); CHECK(lift);
    ShaderBatchOptions batch_options; shader_batch_options_default(&batch_options);
    cli_shaderlab_lift_attach(lift, &batch_options);
    ShaderBatchCandidateInput input = candidate_input(&fixture, 0);
    ShaderLabCandidateDiagnostic diagnostic;
    CHECK(batch_options.select_candidate(batch_options.candidate_context, &input, &selected, &diagnostic));
    CHECK(selected.len == canonical.len && !memcmp(selected.buf, canonical.buf, selected.len));
    const ShaderBatchRecordResult publication = publication_for_source(&selected);
    CHECK(cli_shaderlab_lift_output_generated(lift, 0, &publication));
    CHECK(!cli_shaderlab_lift_output_verified(lift, 0, &publication));
    CHECK(published_status(lift, 0, &publication, "scope-unavailable"));
    fixture_dispose(&fixture); cli_shaderlab_lift_free(lift);
    shaderlab_expression_source_map_free(&map); sb_free(&canonical); sb_free(&selected);
    return true;
}

static bool test_argument_boundaries(void) {
    const CliShaderLabLiftOptions disabled = {0};
    CHECK(!cli_shaderlab_lift_create(NULL, 1) && !cli_shaderlab_lift_create(&disabled, 1));
    const CliShaderLabLiftOptions profile = {.enabled = true, .profile_path = "absent.profile"};
    CHECK(!cli_shaderlab_lift_create(&profile, 1));
    const CliShaderLabLiftOptions options = {.enabled = true};
    CliShaderLabLift *lift = cli_shaderlab_lift_create(&options, 0); CHECK(lift);
    ShaderBatchOptions batch_options; shader_batch_options_default(&batch_options);
    cli_shaderlab_lift_attach(lift, &batch_options);
    StringBuilder source; sb_init(&source);
    ShaderBatchCandidateInput invalid = {.catalog_record_index = 0};
    ShaderLabCandidateDiagnostic diagnostic;
    CHECK(!batch_options.select_candidate(batch_options.candidate_context, &invalid, &source, &diagnostic));
    CHECK(!source.len && !strcmp(cli_shaderlab_lift_selection(lift, 0), "not-run"));
    CHECK(!cli_shaderlab_lift_output_verified(NULL, 0, NULL) &&
          !cli_shaderlab_lift_output_generated(NULL, 0, NULL));
    cli_shaderlab_lift_free(lift); sb_free(&source);
    return true;
}

int main(void) {
    if (!test_argument_boundaries() || !test_source_only_generation() ||
        !test_selected_row_failure() || !test_inventory_scope_is_optional()) return 1;
    puts("Profile-free ShaderLab candidates retain complete source rows and unverified evidence.");
    return 0;
}
