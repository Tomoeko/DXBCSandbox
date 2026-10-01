// SPDX-License-Identifier: GPL-3.0-only

#include "shaderlab_lift_cli.h"
#include "translation/shaderlab_source_quality.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
#include "compiler/unity_shaderlab_lift_batch.h"
#endif

typedef struct {
    int subshader_index, pass_index, stage_index, subprogram_index;
    int blob_index, hardware_tier_group;
} CliSourceRow;

typedef struct {
    CliSourceRow row;
    size_t serialized_state;
    uint8_t target_digest[COMMON_SHA256_DIGEST_SIZE];
    bool has_quality;
    HLSLSourceQualityResult quality;
} CliSourceBody;

/* Historical observations contain no borrowed model, AST or source pointers.
 * Typed receipt replay occurs while the batch's immutable inputs are alive. */
typedef struct {
    ShaderLabSourceQualityStatus status;
    ShaderLabSourceQualityDiagnostic diagnostic;
    ShaderLabSourceQualityResult quality;
    size_t receipt_count, source_size;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t modeled_input_digest[COMMON_SHA256_DIGEST_SIZE];
    bool has_structural_authority;
    ShaderLabStructuralDiagnostic structure;
} CliSourceInventory;

typedef struct {
    bool attempted, generated, rows_complete;
    ShaderLabCandidateDiagnostic diagnostic;
    CliSourceRow *rows;
    size_t row_count;
    CliSourceBody *bodies;
    size_t body_count, source_size;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    CliSourceInventory inventory;
} CliSourceResult;

struct CliShaderLabLift {
    bool verification_requested;
    size_t record_count;
    CliSourceResult *sources;
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    UnityCompilerBroker *broker;
    UnityShaderLabLiftBatch *batch;
#endif
};

bool cli_shaderlab_lift_supported(void) { return true; }

bool cli_shaderlab_lift_verifier_supported(void) {
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    return true;
#else
    return false;
#endif
}

bool cli_shaderlab_lift_verification_requested(const CliShaderLabLift *lift) {
    return lift && lift->verification_requested;
}

static void source_result_dispose(CliSourceResult *result) {
    if (!result) return;
    free(result->rows);
    free(result->bodies);
    memset(result, 0, sizeof(*result));
}

/* Enumerate the serialized denominator before emission. Body counts differ:
 * the proven plan may map serialized rows to an emitted state/tier body. */
static bool capture_rows(const SerializedShader *shader, CliSourceResult *result) {
    if (!shader || shader->subshader_count < 0 ||
        (shader->subshader_count && !shader->subshaders)) return false;
    size_t count = 0;
    for (int s = 0; s < shader->subshader_count; ++s) {
        const SerializedSubShader *subshader = &shader->subshaders[s];
        if (subshader->pass_count < 0 ||
            (subshader->pass_count && !subshader->passes)) return false;
        for (int p = 0; p < subshader->pass_count; ++p) {
            const SerializedPass *pass = &subshader->passes[p];
            if (pass->platform_count < 0 ||
                (pass->platform_count && !pass->platforms)) return false;
            for (int stage = 0; stage < 6; ++stage) {
                if (pass->subprogram_count[stage] < 0 ||
                    (pass->subprogram_count[stage] && !pass->subprograms[stage])) return false;
                for (int row = 0; row < pass->subprogram_count[stage]; ++row) {
                    if (!serialized_pass_subprogram_is_platform(pass, stage, row, 4)) continue;
                    if (count == SIZE_MAX / sizeof(*result->rows)) return false;
                    ++count;
                }
            }
        }
    }
    CliSourceRow *rows = count ? calloc(count, sizeof(*rows)) : NULL;
    if (count && !rows) return false;
    size_t next = 0;
    for (int s = 0; s < shader->subshader_count; ++s) {
        const SerializedSubShader *subshader = &shader->subshaders[s];
        for (int p = 0; p < subshader->pass_count; ++p) {
            const SerializedPass *pass = &subshader->passes[p];
            for (int stage = 0; stage < 6; ++stage) {
                for (int row = 0; row < pass->subprogram_count[stage]; ++row) {
                    if (!serialized_pass_subprogram_is_platform(pass, stage, row, 4)) continue;
                    const SerializedSubProgram *program = &pass->subprograms[stage][row];
                    rows[next++] = (CliSourceRow){s, p, stage, row, program->blob_index,
                        pass->subprogram_identities[stage]
                            ? pass->subprogram_identities[stage][row].hardware_tier_group : -1};
                }
            }
        }
    }
    result->rows = rows;
    result->row_count = count;
    result->rows_complete = true;
    return true;
}

static bool capture_bodies(const ShaderLabExpressionSourceMap *map, CliSourceResult *result) {
    if (!map->complete || (map->count && !map->records) ||
        map->count > SIZE_MAX / sizeof(*result->bodies)) return false;
    CliSourceBody *bodies = map->count ? calloc(map->count, sizeof(*bodies)) : NULL;
    if (map->count && !bodies) return false;
    for (size_t i = 0; i < map->count; ++i) {
        const ShaderLabExpressionSourceRecord *record = &map->records[i];
        bodies[i].row = (CliSourceRow){record->subshader_index, record->pass_index,
            record->stage_index, record->subprogram_index, record->blob_index,
            record->hardware_tier_group};
        bodies[i].serialized_state = record->serialized_state;
        memcpy(bodies[i].target_digest, record->target_digest, sizeof(bodies[i].target_digest));
        bodies[i].has_quality = record->has_source_quality;
        if (record->has_source_quality) bodies[i].quality = record->source_quality;
    }
    result->bodies = bodies;
    result->body_count = map->count;
    return true;
}

static void observe_inventory(const ShaderBatchCandidateInput *input,
                              const StringBuilder *source, CliSourceInventory *snapshot) {
    const ShaderLabSourceQualityRequest request = {
        .shader = &input->object->shader, .archive = input->archive, .object = input->object};
    StringBuilder canonical;
    sb_init(&canonical);
    ShaderLabSourceQualityInventory inventory = {0};
    snapshot->status = shaderlab_source_quality_emit(
        &request, &canonical, &inventory, &snapshot->diagnostic);
    if (snapshot->status != SHADERLAB_SOURCE_QUALITY_OK) goto cleanup;
    if (canonical.len != source->len || memcmp(canonical.buf, source->buf, source->len)) {
        snapshot->status = SHADERLAB_SOURCE_QUALITY_INVENTORY_MISMATCH;
        goto cleanup;
    }
    snapshot->status = shaderlab_source_quality_inventory_analyze(
        &request, source, &inventory, &snapshot->quality, &snapshot->diagnostic);
    if (snapshot->status != SHADERLAB_SOURCE_QUALITY_OK) goto cleanup;
    snapshot->receipt_count = inventory.receipt_count;
    snapshot->source_size = inventory.source_size;
    memcpy(snapshot->source_digest, inventory.source_digest, sizeof(snapshot->source_digest));
    memcpy(snapshot->modeled_input_digest, inventory.modeled_input_digest,
           sizeof(snapshot->modeled_input_digest));
    snapshot->has_structural_authority = inventory.has_structural_authority;
    snapshot->structure = inventory.structure;
cleanup:
    shaderlab_source_quality_inventory_dispose(&inventory);
    sb_free(&canonical);
}

static bool source_candidate(void *context, const ShaderBatchCandidateInput *input,
                             StringBuilder *output, ShaderLabCandidateDiagnostic *diagnostic) {
    CliShaderLabLift *lift = context;
    if (!lift || lift->verification_requested || !input || !input->object || !input->archive ||
        !output || !sb_ok(output) || output->len || input->catalog_record_index >= lift->record_count)
        return false;
    CliSourceResult *retained = &lift->sources[input->catalog_record_index];
    if (retained->attempted) return false;
    CliSourceResult result = {0};
    result.attempted = true;
    result.diagnostic.status = SHADERLAB_CANDIDATE_INVALID_ARGUMENT;
    result.diagnostic.property_index = result.diagnostic.subshader_index = result.diagnostic.pass_index = -1;
    result.diagnostic.unsupported_stage_index = -1;
    StringBuilder source;
    sb_init(&source);
    ShaderLabExpressionSourceMap map = {0};
    if (!capture_rows(&input->object->shader, &result)) goto cleanup;
    if (!shaderlab_emit_high_level_candidate_with_source_map(
            &input->object->shader, input->archive->entries, input->archive->entry_count,
            input->archive->segments, input->archive->segment_lengths, input->archive->segment_count,
            &source, &map, &result.diagnostic)) goto cleanup;
    if (!source.len || !shaderlab_expression_source_map_matches_source(&map, &source) ||
        !capture_bodies(&map, &result)) {
        result.diagnostic.status = SHADERLAB_CANDIDATE_OUTPUT_FAILED;
        goto cleanup;
    }
    /* Inventory limits and schema observation never reduce source admission. */
    observe_inventory(input, &source, &result.inventory);
    result.source_size = source.len;
    common_sha256(source.buf, source.len, result.source_digest);
    /* Transfer the complete producer output without another source bank. */
    sb_free(output);
    *output = source;
    memset(&source, 0, sizeof(source));
    result.generated = true;
cleanup:
    if (diagnostic) *diagnostic = result.diagnostic;
    *retained = result;
    shaderlab_expression_source_map_free(&map);
    sb_free(&source);
    return result.generated;
}

CliShaderLabLift *cli_shaderlab_lift_create(const CliShaderLabLiftOptions *options, size_t records) {
    if (!options || !options->enabled) return NULL;
    CliShaderLabLift *lift = calloc(1, sizeof(*lift));
    if (!lift) return NULL;
    lift->record_count = records;
    lift->verification_requested = options->profile_path != NULL;
    if (!lift->verification_requested) {
        if (records > SIZE_MAX / sizeof(*lift->sources)) goto failure;
        lift->sources = records ? calloc(records, sizeof(*lift->sources)) : NULL;
        if (records && !lift->sources) goto failure;
        return lift;
    }
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    UnityCompileProfile profile;
    UnityCompileProfileStatus status = unity_compile_profile_load(options->profile_path, &profile);
    if (status != UNITY_COMPILE_PROFILE_OK) {
        fprintf(stderr, "Error: could not load compile profile: %s.\n",
                unity_compile_profile_status_string(status));
        goto failure;
    }
    lift->broker = unity_compiler_broker_create_lazy(
        options->project_root ? options->project_root : ".", options->includes ? options->includes : "");
    const HLSLLiftLimits limits = {2, options->max_compiles, options->max_elapsed_ms};
    if (lift->broker && unity_compiler_broker_set_expected_valid_apis(lift->broker, profile.valid_apis))
        lift->batch = unity_shaderlab_lift_batch_create(lift->broker, &profile, &limits, records);
    if (lift->batch) return lift;
    fputs("Error: could not initialize the ShaderLab compiler verifier.\n", stderr);
#else
    fputs("Error: compile-profile verification requires the optional Unity compiler build.\n", stderr);
#endif
failure:
    cli_shaderlab_lift_free(lift);
    return NULL;
}

void cli_shaderlab_lift_attach(CliShaderLabLift *lift, ShaderBatchOptions *options) {
    if (!lift || !options) return;
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    if (lift->verification_requested) {
        unity_shaderlab_lift_batch_attach(lift->batch, options);
        return;
    }
#endif
    options->select_candidate = source_candidate;
    options->candidate_context = lift;
}

static const CliSourceResult *source_result(const CliShaderLabLift *lift, size_t record) {
    return lift && !lift->verification_requested && record < lift->record_count &&
        lift->sources[record].attempted ? &lift->sources[record] : NULL;
}

static void digest_json(StringBuilder *out, const char *name, const uint8_t *digest, bool present) {
    sb_appendf(out, "\"%s\":", name);
    if (!present) { sb_append(out, "null"); return; }
    char hex[65];
    common_sha256_digest_to_hex(digest, hex);
    sb_appendf(out, "\"%s\"", hex);
}

static const char *inventory_status_name(ShaderLabSourceQualityStatus status) {
    switch (status) {
    case SHADERLAB_SOURCE_QUALITY_OK: return "observed";
    case SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE: return "scope-unavailable";
    case SHADERLAB_SOURCE_QUALITY_STRUCTURE_FAILED: return "schema-failed";
    case SHADERLAB_SOURCE_QUALITY_EMISSION_FAILED: return "emission-failed";
    case SHADERLAB_SOURCE_QUALITY_INVENTORY_MISMATCH: return "source-binding-mismatch";
    case SHADERLAB_SOURCE_QUALITY_OBSERVER_REJECTED: return "observer-rejected";
    case SHADERLAB_SOURCE_QUALITY_ALLOCATION_FAILED: return "allocation-failed";
    default: return "invalid-argument";
    }
}

static bool append_source_inventory(const CliSourceResult *result, StringBuilder *out) {
    const CliSourceInventory *snapshot = &result->inventory;
    const char *status = result->generated ? inventory_status_name(snapshot->status) : "not-run";
    bool observed = result->generated && snapshot->status == SHADERLAB_SOURCE_QUALITY_OK;
    if (observed && (snapshot->source_size != result->source_size ||
        memcmp(snapshot->source_digest, result->source_digest, sizeof(result->source_digest)))) {
        observed = false;
        status = "source-binding-mismatch";
    }
    sb_appendf(out, "{\"scope\":\"emitted-shaderlab-with-explicit-gaps\",\"status\":\"%s\",", status);
    digest_json(out, "source_sha256", snapshot->source_digest, observed);
    sb_append_char(out, ',');
    digest_json(out, "modeled_input_sha256", snapshot->modeled_input_digest, observed);
    if (!observed) {
        sb_append(out, ",\"quality\":null}");
        return sb_ok(out);
    }
    const ShaderLabSourceQualityResult *quality = &snapshot->quality;
    sb_appendf(out, ",\"source_bytes\":%zu,\"receipts\":%zu,\"quality\":{"
        "\"classification\":\"%s\",\"reasons\":%u,\"gaps\":%u,\"wrapper_complete\":%s,"
        "\"wrapper_receipts\":%zu,\"linked_entries\":%zu,\"stage_class_counts\":{",
        snapshot->source_size, snapshot->receipt_count, hlsl_source_quality_class_name(quality->classification),
        quality->reasons, quality->gaps, quality->wrapper_complete ? "true" : "false",
        quality->wrapper_receipt_count, quality->linked_entry_count);
    for (unsigned i = 0; i < 5; ++i)
        sb_appendf(out, "%s\"%s\":%zu", i ? "," : "",
            hlsl_source_quality_class_name((HLSLSourceQualityClass)i), quality->stage_class_counts[i]);
    sb_appendf(out, "},\"observed_stage_residual_lower_bound\":%zu,"
        "\"observed_stage_unknown_lower_bound\":%zu,\"observed_stage_incomplete_units\":%zu,"
        "\"literal_include_roots\":%zu,\"gap_names\":[",
        quality->observed_stage_residual_total, quality->observed_stage_unknown_provenance,
        quality->observed_stage_incomplete_units, quality->required_external_include_root_count);
    const struct { uint32_t bit; const char *name; } gaps[] = {
        {SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE, "external-include-semantics"},
        {SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY, "dependency-inventory"},
        {SHADERLAB_SOURCE_GAP_SCHEMA_AUTHORITY, "schema-authority"},
        {SHADERLAB_SOURCE_GAP_STAGE_COVERAGE, "stage-coverage"}};
    bool first = true;
    for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); ++i) {
        if (!(quality->gaps & gaps[i].bit)) continue;
        sb_appendf(out, "%s\"%s\"", first ? "" : ",", gaps[i].name);
        first = false;
    }
    sb_appendf(out, "]},\"schema_structure\":{\"present\":%s,"
        "\"scope\":\"serialized-d3d11-shaderlab-structure\",\"status\":\"%s\","
        "\"runtime_selection_certified\":false,\"visual_output_certified\":false}}",
        snapshot->has_structural_authority ? "true" : "false",
        snapshot->has_structural_authority ? shaderlab_structural_status_name(snapshot->structure.status) : "not-run");
    return sb_ok(out);
}

static void append_row_json(const CliSourceRow *row, StringBuilder *out) {
    sb_appendf(out, "\"subshader\":%d,\"pass\":%d,\"stage\":%d,\"subprogram\":%d,"
        "\"blob\":%d,\"tier\":%d", row->subshader_index, row->pass_index,
        row->stage_index, row->subprogram_index, row->blob_index, row->hardware_tier_group);
}

bool cli_shaderlab_lift_append_json(const CliShaderLabLift *lift, size_t record, StringBuilder *out) {
    if (!out || !sb_ok(out)) return false;
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    if (lift && lift->verification_requested) {
        const UnityShaderLabLiftResult *result = unity_shaderlab_lift_batch_result(lift->batch, record);
        if (!result) { sb_append(out, "null"); return sb_ok(out); }
        char *json = unity_shaderlab_lift_format_json(result);
        if (!json) return false;
        sb_append(out, json);
        free(json);
        return sb_ok(out);
    }
#endif
    const CliSourceResult *result = source_result(lift, record);
    if (!result) { sb_append(out, "null"); return sb_ok(out); }
    sb_appendf(out, "{\"schema\":\"dxbc-shaderlab-source-v1\",\"mode\":\"source-only\","
        "\"scope\":\"complete-serialized-d3d11-source-candidate\",\"attempted\":true,\"generated\":%s,"
        "\"selection\":\"%s\",\"emission_status\":\"%s\",\"emission_reason\":\"%s\","
        "\"source_bytes\":%zu,", result->generated ? "true" : "false",
        result->generated ? "high-level" : "unavailable",
        shaderlab_candidate_status_name(result->diagnostic.status),
        shaderlab_candidate_reason_name(&result->diagnostic), result->source_size);
    digest_json(out, "source_sha256", result->source_digest, result->generated);
    sb_appendf(out, ",\"requested_rows_complete\":%s,\"requested_variants\":%zu,\"requested_rows\":[",
        result->rows_complete ? "true" : "false", result->row_count);
    for (size_t i = 0; i < result->row_count; ++i) {
        sb_append(out, i ? ",{" : "{");
        append_row_json(&result->rows[i], out);
        sb_append_char(out, '}');
    }
    sb_appendf(out, "],\"emitted_bodies\":%zu,\"bodies\":[", result->body_count);
    for (size_t i = 0; i < result->body_count; ++i) {
        const CliSourceBody *body = &result->bodies[i];
        sb_append(out, i ? ",{" : "{");
        append_row_json(&body->row, out);
        sb_appendf(out, ",\"serialized_state\":%zu,", body->serialized_state);
        digest_json(out, "target_sha256", body->target_digest, true);
        sb_append(out, ",\"source_quality_scope\":\"emitted-stage-entry\",\"source_quality\":");
        if (body->has_quality) {
            if (!hlsl_source_quality_append_json(&body->quality, out)) return false;
        } else sb_append(out, "null");
        sb_append_char(out, '}');
    }
    sb_append(out, "],\"bounded_source_inventory\":");
    if (!append_source_inventory(result, out)) return false;
    sb_append(out, ",\"verification\":{\"requested\":false,\"compiler\":\"not-run\","
        "\"exact_dxbc\":\"not-run\",\"unity_import\":\"not-run\",\"native_d3d11\":\"not-run\"}}");
    return sb_ok(out);
}

const char *cli_shaderlab_lift_selection(const CliShaderLabLift *lift, size_t record) {
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    if (lift && lift->verification_requested) {
        const UnityShaderLabLiftResult *result = unity_shaderlab_lift_batch_result(lift->batch, record);
        if (!result) return "not-run";
        const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(result);
        if (!accepted) return "unverified";
        return accepted->high_level ? "high-level" : "low-level-fallback";
    }
#endif
    const CliSourceResult *result = source_result(lift, record);
    return result ? (result->generated ? "high-level" : "unavailable") : "not-run";
}

static const char *publication_status(const ShaderBatchRecordResult *publication) {
    if (!publication || !publication->publication_authorized ||
        !publication->published_shader_content_recorded ||
        (publication->status != SHADER_BATCH_EMITTED &&
         publication->status != SHADER_BATCH_UNCHANGED)) return "not-published";
    if (publication->source_identity_close_deferred) return "identity-close-deferred";
    if (publication->publication_residue || publication->shader_publication_residue ||
        publication->meta_publication_residue) return "publication-residue";
    return NULL;
}

#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
static bool accepted_artifact_matches_publication(const UnityShaderLabLiftArtifact *accepted,
    const ShaderBatchRecordResult *publication) {
    if (!accepted || !publication || !accepted->source.buf || !sb_ok(&accepted->source) ||
        !accepted->source.len || accepted->source.len != publication->published_shader_size) return false;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(accepted->source.buf, accepted->source.len, digest);
    return !memcmp(digest, publication->published_shader_digest, sizeof(digest));
}

static bool append_published_artifact_inventory(const UnityShaderLabLiftArtifact *accepted,
    bool lift_available, const ShaderBatchRecordResult *publication, StringBuilder *out) {
    if (!out || !sb_ok(out)) return false;
    const char *unavailable = !lift_available ? "not-run" : publication_status(publication);
    if (!unavailable) {
        if (accepted_artifact_matches_publication(accepted, publication))
            return unity_shaderlab_lift_append_inventory_json(accepted, out);
        unavailable = "source-binding-mismatch";
    }
    sb_appendf(out, "{\"status\":\"%s\",\"quality\":null}", unavailable);
    return sb_ok(out);
}
#endif

bool cli_shaderlab_lift_output_generated(const CliShaderLabLift *lift, size_t record,
                                         const ShaderBatchRecordResult *publication) {
    if (!lift || publication_status(publication)) return false;
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    if (lift->verification_requested) {
        const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(
            unity_shaderlab_lift_batch_result(lift->batch, record));
        return accepted_artifact_matches_publication(accepted, publication);
    }
#endif
    const CliSourceResult *result = source_result(lift, record);
    return result && result->generated && result->source_size == publication->published_shader_size &&
        !memcmp(result->source_digest, publication->published_shader_digest, sizeof(result->source_digest));
}

bool cli_shaderlab_lift_output_verified(const CliShaderLabLift *lift, size_t record,
                                        const ShaderBatchRecordResult *publication) {
    return cli_shaderlab_lift_verification_requested(lift) &&
        cli_shaderlab_lift_output_generated(lift, record, publication);
}

bool cli_shaderlab_lift_append_published_inventory_json(const CliShaderLabLift *lift, size_t record,
    const ShaderBatchRecordResult *publication, StringBuilder *out) {
    if (!out || !sb_ok(out)) return false;
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    if (lift && lift->verification_requested) {
        const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(
            unity_shaderlab_lift_batch_result(lift->batch, record));
        return append_published_artifact_inventory(accepted, true, publication, out);
    }
#endif
    const char *unavailable = !lift ? "not-run" : publication_status(publication);
    if (!unavailable && cli_shaderlab_lift_output_generated(lift, record, publication)) {
        return append_source_inventory(source_result(lift, record), out);
    }
    if (!unavailable) unavailable = "source-binding-mismatch";
    sb_appendf(out, "{\"status\":\"%s\",\"quality\":null}", unavailable);
    return sb_ok(out);
}

void cli_shaderlab_lift_free(CliShaderLabLift *lift) {
    if (!lift) return;
    if (lift->sources) {
        for (size_t i = 0; i < lift->record_count; ++i) source_result_dispose(&lift->sources[i]);
        free(lift->sources);
    }
#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
    unity_shaderlab_lift_batch_free(lift->batch);
    unity_compiler_broker_destroy(lift->broker);
#endif
    free(lift);
}
