// SPDX-License-Identifier: GPL-3.0-only

#include "app/shader_catalog_object.h"
#include "common/file_io.h"
#include "common/sha256.h"
#include "compiler/unity_shaderlab_lift.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_global_declarations.h"
#include "translation/hlsl_source_quality.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct CandidateSource {
    struct CandidateSource *next;
    const ShaderBlobArchive *archive;
    const SerializedPass *pass;
    int subshader_index;
    int pass_index;
    int stage;
    int variant;
    const uint8_t *target;
    size_t target_size;
    StringBuilder source;
} CandidateSource;

/* Evaluation only: input consists of captured released bytes and a captured
 * compile profile. Authored reference sources and fixture manifests are never
 * opened. Quality is measured per captured stage row; the production lift
 * independently checks complete generated pass domains and exact containers. */
typedef struct {
    const char *output_directory;
    const char *compiler_source_directory;
    const UnityCompileProfile *profile;
    UnityCompilerBroker *broker;
    UnityCompilerBroker *candidate_broker;
    CandidateSource *candidates;
    size_t candidate_count;
    size_t current_object;
    FILE *ledger;
    size_t graphics_objects;
    size_t compute_objects;
    size_t requested_graphics_rows;
    size_t requested_compute_variants;
    size_t parsed_rows;
    size_t emitted[2];
    size_t classifications[2][5];
    size_t exact_objects;
    size_t high_level_objects;
    size_t failures;
} Probe;

typedef struct {
    Probe *probe;
    size_t object;
    int subshader;
    int pass;
    int stage;
    int variant;
    int mode;
} QualityObservationContext;

static bool observe_quality(void *opaque, const HLSLSourceQualityObservation *observation) {
    const QualityObservationContext *context = opaque;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (!facts->declaration_witness_record) return true;
    return fprintf(context->probe->ledger,
        "{\"event\":\"declaration_witness\",\"object\":%zu,\"subshader\":%d,"
        "\"pass\":%d,\"serialized_stage\":%d,\"variant\":%d,\"mode\":\"%s\","
        "\"unit\":%u,\"unit_kind\":%u,\"field\":%u,\"current_variant\":%u,"
        "\"witness_subprogram\":%u,\"witness_count\":%u,\"known\":%s,"
        "\"logical_value\":%llu,\"components\":%u,\"artifacts\":%u}\n",
        context->object, context->subshader, context->pass, context->stage, context->variant,
        context->mode ? "high-level-candidate" : "recompile", observation->source_unit_id,
        (unsigned)observation->unit_kind, facts->declaration_field_index,
        facts->declaration_variant_index, facts->declaration_witness_subprogram_index,
        facts->declaration_witness_count, facts->known ? "true" : "false",
        (unsigned long long)facts->logical_value_id, facts->components, facts->artifacts) >= 0;
}

static const HLSLEmitNames stage_names[] = {
    {"vert", "appdata", "v2f"}, {"frag", "ps_input", "fout"},
    {"geom", "gs_input", "gs_output"}, {"hs", "unused_input", "unused_output"},
    {"ds", "unused_input", "unused_output"}
};

static void free_candidates(Probe *probe) {
    while (probe->candidates) {
        CandidateSource *candidate = probe->candidates;
        probe->candidates = candidate->next;
        sb_free(&candidate->source);
        free(candidate);
    }
    probe->candidate_count = 0;
}

static bool retain_candidate(Probe *probe, const ShaderBlobArchive *archive,
                             const SerializedPass *pass, int subshader_index,
                             int pass_index, int stage, int variant,
                             const DXBCContainerView *target, const StringBuilder *source) {
    if (probe->candidate_count >= 16384) return false;
    CandidateSource *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) return false;
    candidate->archive = archive;
    candidate->pass = pass;
    candidate->subshader_index = subshader_index;
    candidate->pass_index = pass_index;
    candidate->stage = stage;
    candidate->variant = variant;
    candidate->target = target->data;
    candidate->target_size = target->size;
    sb_init(&candidate->source);
    /* This generated harness retains the production pass's fixed Unity
     * declaration prelude. Its complete controls are checked independently. */
    sb_append(&candidate->source, "#pragma vertex vert\n#pragma fragment frag\n");
    if (stage > 1)
        sb_appendf(&candidate->source, "#pragma %s %s\n",
                   (const char *[]){"vertex", "fragment", "geometry", "hull", "domain"}[stage],
                   stage_names[stage].entry_point);
    sb_append(&candidate->source, "#define UNIVERSAL_SHADER_VARIABLES_INCLUDED\n"
                                 "#include \"UnityShaderVariables.cginc\"\n");
    sb_append_len(&candidate->source, source->buf, source->len);
    if (!sb_ok(&candidate->source)) {
        sb_free(&candidate->source);
        free(candidate);
        return false;
    }
    candidate->next = probe->candidates;
    probe->candidates = candidate;
    ++probe->candidate_count;
    return true;
}

static bool save_bytes(const char *directory, const char *name,
                       const void *bytes, size_t size) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    return length > 0 && (size_t)length < sizeof(path) &&
           common_file_write_new_atomic(path, bytes, size) == COMMON_FILE_OK;
}

static bool parse_parameters(const ShaderBlobArchive *archive,
                             const SerializedPass *pass, int stage, int variant,
                             SerializedProgramParameters *owned,
                             const SerializedProgramParameters **selected) {
    *selected = &pass->common_parameters[stage];
    int index = pass->subprogram_param_blob_indices[stage]
        ? pass->subprogram_param_blob_indices[stage][variant] : -1;
    if (index == -1) return true;
    if (index < 0) return false;
    const uint8_t *bytes = NULL;
    size_t size = 0;
    if (!shader_blob_archive_get(archive, index, &bytes, &size)) return false;
    ByteStream stream;
    stream_init(&stream, bytes, size);
    stream_set_endian(&stream, false);
    if (!subprogram_metadata_parse_parameters(&stream, owned)) return false;
    *selected = owned;
    return true;
}

static HLSLGlobalDeclarationStatus build_declaration_union(
    const ShaderBlobArchive *archive, const SerializedPass *pass,
    int stage, int current, const SerializedProgramParameters *residual,
    HLSLGlobalDeclarationUnion **output, HLSLGlobalDeclarationDiagnostic *diagnostic) {
    *output = NULL;
    *diagnostic = (HLSLGlobalDeclarationDiagnostic){
        .subprogram_index = current, .conflicting_subprogram_index = -1, .field_index = -1};
    HLSLGlobalDeclarationStatus status = hlsl_global_declarations_scope_status(
        residual, &pass->common_parameters[stage]);
    if (status != HLSL_GLOBAL_DECLARATIONS_OK) return status;
    const int capacity = pass->subprogram_count[stage];
    if (capacity <= 0 || capacity > 4096 || !pass->subprograms[stage])
        return HLSL_GLOBAL_DECLARATIONS_INVALID;
    HLSLGlobalDeclarationWitness *witnesses = calloc((size_t)capacity, sizeof(*witnesses));
    PlayerSubProgramMetadata *players = calloc((size_t)capacity, sizeof(*players));
    SerializedProgramParameters *parameters = calloc((size_t)capacity, sizeof(*parameters));
    if (!witnesses || !players || !parameters) {
        free(witnesses); free(players); free(parameters);
        return HLSL_GLOBAL_DECLARATIONS_ALLOCATION_FAILED;
    }
    size_t count = 0;
    status = HLSL_GLOBAL_DECLARATIONS_INVALID;
    for (int index = 0; index < capacity; ++index) {
        if (!hlsl_global_declarations_same_family(pass, stage, current, index)) continue;
        const size_t slot = count++;
        serialized_program_parameters_init(&parameters[slot]);
        const uint8_t *bytes = NULL;
        size_t size = 0;
        if (!shader_blob_archive_get(archive, pass->subprograms[stage][index].blob_index,
                                     &bytes, &size)) goto cleanup;
        ByteStream stream;
        stream_init(&stream, bytes, size);
        stream_set_endian(&stream, false);
        if (!subprogram_metadata_parse_variant(&stream, &players[slot])) goto cleanup;
        const SerializedProgramParameters *selected = NULL;
        if (!parse_parameters(archive, pass, stage, index, &parameters[slot], &selected))
            goto cleanup;
        const int parameter_index = pass->subprogram_param_blob_indices[stage]
            ? pass->subprogram_param_blob_indices[stage][index] : -1;
        witnesses[slot] = (HLSLGlobalDeclarationWitness){
            index, &players[slot], parameter_index >= 0 ? selected : NULL};
    }
    status = hlsl_global_declarations_build(pass, stage, current, witnesses, count,
                                             output, diagnostic);
cleanup:
    for (size_t index = 0; index < count; ++index) {
        subprogram_metadata_free_variant(&players[index]);
        serialized_program_parameters_free(&parameters[index]);
    }
    free(witnesses); free(players); free(parameters);
    return status;
}

static bool observe_compile(void *context, const UnityCompilerSnippetCompileRequest *request,
                            UnityCompilerBinaryResponse *response) {
    Probe *probe = context;
    bool received = unity_compiler_broker_compile_contract_response(probe->broker, request, response);
    DXBCContainerView baseline = {0};
    if (!received || !unity_compiler_response_status_is_clean_success(&response->status) ||
        !response->has_request_identity ||
        !dxbc_container_view_first(response->data, response->size, &baseline)) return received;
    for (CandidateSource *candidate = probe->candidates; candidate; candidate = candidate->next) {
        int32_t compiler_stage;
        DXBCCompareResult comparison;
        if (!unity_serialized_stage_to_compiler_program(candidate->stage, &compiler_stage) ||
            compiler_stage != request->shader_type ||
            strcmp(candidate->pass->name ? candidate->pass->name : "",
                   request->pass_name ? request->pass_name : "") != 0 ||
            dxbc_compare_exact(candidate->target, candidate->target_size,
                               baseline.data, baseline.size, &comparison) != DXBC_COMPARE_EQUAL)
            continue;
        UnityCompilerSnippetCompileRequest lifted = *request;
        lifted.snippet_source = candidate->source.buf;
        UnityCompilerBinaryResponse actual;
        unity_compiler_binary_response_init(&actual);
        bool actual_received = unity_compiler_broker_compile_contract_response(
            probe->candidate_broker, &lifted, &actual);
        bool clean = actual_received && unity_compiler_response_status_is_clean_success(&actual.status);
        if (!clean) {
            char *diagnostics = unity_compiler_response_status_format(&actual.status, "No candidate response.");
            if (diagnostics) fprintf(stderr, "stage candidate: %s\n", diagnostics);
            free(diagnostics);
        }
        bool same_controls = actual.has_request_identity &&
            memcmp(response->controls_digest, actual.controls_digest, 32) == 0;
        DXBCContainerView generated = {0};
        DXBCCompareStatus status = DXBC_COMPARE_INVALID_ARGUMENT;
        if (clean && dxbc_container_view_first(actual.data, actual.size, &generated))
            status = dxbc_compare_exact(candidate->target, candidate->target_size,
                                        generated.data, generated.size, &comparison);
        SerializedProgramParameters parameters;
        serialized_program_parameters_init(&parameters);
        const SerializedProgramParameters *selected = NULL;
        PlayerSubProgramMetadata player = {0};
        UnityReflectionCertificateReport reflection;
        unity_reflection_certificate_report_init(&reflection);
        reflection.status = UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT;
        bool reflection_attempted = false;
        const uint8_t *bytes = NULL;
        size_t size = 0;
        ByteStream stream;
        if (clean && parse_parameters(candidate->archive, candidate->pass, candidate->stage,
                candidate->variant, &parameters, &selected) &&
            shader_blob_archive_get(candidate->archive,
                candidate->pass->subprograms[candidate->stage][candidate->variant].blob_index,
                &bytes, &size)) {
            stream_init(&stream, bytes, size);
            stream_set_endian(&stream, false);
            if (subprogram_metadata_parse_variant(&stream, &player)) {
                reflection_attempted = true;
                unity_reflection_certify_d3d11_bindings(&player,
                    &candidate->pass->common_parameters[candidate->stage], selected,
                    actual.reflection_records, actual.reflection_record_count, &reflection);
            }
        }
        char request_digest[65] = "", controls_digest[65] = "";
        char target_digest[65], source_digest[65];
        uint8_t digest[32];
        common_sha256(candidate->target, candidate->target_size, digest);
        common_sha256_digest_to_hex(digest, target_digest);
        common_sha256(candidate->source.buf, candidate->source.len, digest);
        common_sha256_digest_to_hex(digest, source_digest);
        if (actual.has_request_identity) {
            common_sha256_digest_to_hex(actual.request_digest, request_digest);
            common_sha256_digest_to_hex(actual.controls_digest, controls_digest);
        }
        fprintf(probe->ledger,
            "{\"event\":\"stage_candidate_compile\",\"object\":%zu,"
            "\"subshader\":%d,\"pass\":%d,\"serialized_stage\":%d,"
            "\"captured_variant\":%d,\"clean_compile\":%s,"
            "\"controls_identical\":%s,\"full_container_comparison\":\"%s\","
            "\"reflection_attempted\":%s,\"reflection_bindings\":\"%s\",\"request_sha256\":\"%s\","
            "\"controls_sha256\":\"%s\",\"target_sha256\":\"%s\","
            "\"source_sha256\":\"%s\",\"scope\":\"stage-observation\"}\n",
            probe->current_object, candidate->subshader_index, candidate->pass_index,
            candidate->stage, candidate->variant,
            clean ? "true" : "false", same_controls ? "true" : "false",
            dxbc_compare_status_name(status), reflection_attempted ? "true" : "false",
            unity_reflection_certificate_status_name(reflection.status),
            request_digest, controls_digest, target_digest, source_digest);
        subprogram_metadata_free_variant(&player);
        serialized_program_parameters_free(&parameters);
        unity_compiler_binary_response_free(&actual);
        /* Equal bytecode aliases select the same body. The production
         * certifier still checks every requested generated state separately. */
        break;
    }
    return received;
}

static void report_emission(Probe *probe, size_t object_index, int subshader,
                            int pass, int stage, int variant, int mode,
                            const char *target_digest, bool emitted,
                            const StringBuilder *source,
                            const HLSLExpressionSourceMap *map,
                            const HLSLEmitDiagnostic *diagnostic,
                            const HLSLSourceQualityResult *quality) {
    char source_digest[65] = "";
    if (emitted) {
        uint8_t digest[32];
        common_sha256(source->buf, source->len, digest);
        common_sha256_digest_to_hex(digest, source_digest);
        char filename[160];
        snprintf(filename, sizeof(filename), "object%zu-sub%d-pass%d-stage%d-row%d-mode%d.hlsl",
                 object_index, subshader, pass, stage, variant, mode);
        if (!save_bytes(probe->output_directory, filename, source->buf, source->len))
            ++probe->failures;
    }
    ++probe->classifications[mode][quality->classification];
    if (emitted) ++probe->emitted[mode];
    fprintf(probe->ledger,
        "{\"event\":\"stage_quality\",\"object\":%zu,\"subshader\":%d,\"pass\":%d,"
        "\"serialized_stage\":%d,\"variant\":%d,\"mode\":\"%s\","
        "\"target_sha256\":\"%s\",\"source_sha256\":\"%s\",\"emitted\":%s,"
        "\"emission_status\":\"%s\",\"emission_phase\":\"%s\","
        "\"emission_reason\":\"%s\",\"source_map_complete\":%s,"
        "\"quality\":\"%s\",\"quality_reasons\":%u,"
        "\"residual_total\":%zu,\"register_storage\":%zu,\"lane_transport\":%zu,"
        "\"scalarized_intrinsics\":%zu,\"raw_buffer_reconstruction\":%zu,"
        "\"synthetic_interface\":%zu,\"instruction_assignments\":%zu,"
        "\"unstructured_control\":%zu,\"storage_bitcasts\":%zu,"
        "\"unknown_provenance\":%zu,\"incomplete_units\":%zu,"
        "\"semantic_projections\":%zu,\"sibling_declarations\":%zu,"
        "\"sibling_declaration_witnesses\":%zu}\n",
        object_index, subshader, pass, stage, variant,
        mode ? "high-level-candidate" : "recompile", target_digest, source_digest,
        emitted ? "true" : "false", hlsl_emit_status_name(diagnostic->status),
        hlsl_emit_phase_name(diagnostic->phase), hlsl_emit_reason_name(diagnostic->reason),
        map->complete ? "true" : "false",
        hlsl_source_quality_class_name(quality->classification), quality->reasons,
        quality->counts.residual_total, quality->counts.register_storage,
        quality->counts.lane_transport, quality->counts.scalarized_intrinsics,
        quality->counts.raw_buffer_reconstruction, quality->counts.synthetic_interface,
        quality->counts.instruction_assignments, quality->counts.unstructured_control,
        quality->counts.storage_bitcasts, quality->counts.unknown_provenance,
        quality->counts.incomplete_units, quality->counts.semantic_projections,
        quality->counts.sibling_declarations, quality->counts.sibling_declaration_witnesses);
}

static void measure_stage(Probe *probe, size_t object_index, const SerializedShader *shader,
                          const ShaderBlobArchive *archive, int subshader_index,
                          int pass_index, int stage, int variant) {
    const SerializedPass *pass = &shader->subshaders[subshader_index].passes[pass_index];
    const SerializedSubProgram *subprogram = &pass->subprograms[stage][variant];
    ++probe->requested_graphics_rows;
    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    const SerializedProgramParameters *selected_parameters = NULL;
    PlayerSubProgramMetadata player = {0};
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    DXBCContainer semantic = {0};
    USILProgram program = {0};
    HLSLGlobalDeclarationUnion *declarations = NULL;
    bool semantic_decoded = false;
    bool translated = false;
    const char *failure = "parameter-metadata";
    const uint8_t *bytes = NULL;
    size_t size = 0;
    DXBCContainerView raw = {0};
    DXBCDocumentDiagnostic document_diagnostic = {0};
    DXBCStageContractDiagnostic contract_diagnostic = {0};
    char target_digest[65] = "";
    if (!parse_parameters(archive, pass, stage, variant, &parameters,
                          &selected_parameters)) goto cleanup;
    failure = "player-wrapper";
    if (!shader_blob_archive_get(archive, subprogram->blob_index, &bytes, &size))
        goto cleanup;
    ByteStream stream;
    stream_init(&stream, bytes, size);
    stream_set_endian(&stream, false);
    if (!subprogram_metadata_parse_variant(&stream, &player) ||
        player.program_type != subprogram->program_type ||
        !subprogram_metadata_local_keyword_set_matches(&player, subprogram)) goto cleanup;
    failure = "dxbc-document";
    if (!dxbc_container_view_first(player.bytecode, player.bytecode_length, &raw) ||
        !dxbc_document_parse(&document, raw.data, raw.size, &document_diagnostic))
        goto cleanup;
    uint8_t digest[32];
    common_sha256(raw.data, raw.size, digest);
    common_sha256_digest_to_hex(digest, target_digest);
    failure = "semantic-decode";
    if (!dxbc_document_decode_semantic(&document, &semantic)) goto cleanup;
    semantic_decoded = true;
    failure = "stage-contract";
    if (!dxbc_stage_contract_decode(&document, &semantic, &contract,
                                    &contract_diagnostic)) goto cleanup;
    UnityCompilerProgramStage compiler_stage;
    ShaderStageTuple tuple = {0};
    tuple.serialized_stage = (UnitySerializedProgramStage)stage;
    if (!shader_stage_serialized_to_compiler(tuple.serialized_stage, &compiler_stage))
        goto cleanup;
    tuple.compiler_program = compiler_stage;
    tuple.serialized_program_mask = pass->program_mask;
    tuple.gpu_program_type = (UnityGPUProgramType)player.program_type;
    tuple.dxbc_program_type = contract.program_type;
    tuple.shader_model_major = contract.shader_model_major;
    tuple.shader_model_minor = contract.shader_model_minor;
    if (shader_stage_validate_d3d11_tuple(&tuple) != SHADER_STAGE_TUPLE_OK) goto cleanup;
    failure = "usil-translation";
    if (!usil_translate_with_stage_contract(&program, &semantic, &contract)) goto cleanup;
    translated = true;
    ++probe->parsed_rows;
    const int parameter_index = pass->subprogram_param_blob_indices[stage]
        ? pass->subprogram_param_blob_indices[stage][variant] : -1;
    HLSLGlobalDeclarationDiagnostic declaration_diagnostic = {0};
    HLSLGlobalDeclarationStatus declaration_status = build_declaration_union(
        archive, pass, stage, variant, parameter_index >= 0 ? selected_parameters : NULL,
        &declarations, &declaration_diagnostic);
    for (int mode = 0; mode < 2; ++mode) {
        if (mode && declaration_status != HLSL_GLOBAL_DECLARATIONS_OK &&
            declaration_status != HLSL_GLOBAL_DECLARATIONS_NOT_APPLICABLE) {
            ++probe->classifications[mode][HLSL_SOURCE_QUALITY_UNSUPPORTED];
            fprintf(probe->ledger,
                "{\"event\":\"stage_unavailable\",\"object\":%zu,\"subshader\":%d,"
                "\"pass\":%d,\"serialized_stage\":%d,\"variant\":%d,"
                "\"mode\":\"high-level-candidate\",\"layer\":\"declaration-union\","
                "\"target_sha256\":\"%s\",\"union_status\":%u,"
                "\"conflicting_subprogram\":%d,\"field\":%d}\n",
                object_index, subshader_index, pass_index, stage, variant, target_digest,
                (unsigned)declaration_status, declaration_diagnostic.conflicting_subprogram_index,
                declaration_diagnostic.field_index);
            continue;
        }
        StringBuilder source;
        sb_init(&source);
        HLSLEmitOptions options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
        options.mode = mode ? HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE : HLSL_EMIT_MODE_RECOMPILE;
        options.omit_unity_builtin_declarations = true;
        options.reserved_preprocessor_identifiers =
            (const char *const *)shader->keyword_names.keywords;
        options.reserved_preprocessor_identifier_count = (size_t)shader->keyword_names.count;
        HLSLExpressionSourceMap map = {0};
        if (mode) options.expression_source_map = &map;
        HLSLSourceQualityResult quality = {0};
        options.source_quality = &quality;
        options.source_quality_pass_index = (uint32_t)pass_index;
        options.source_quality_entry_point_index = (uint32_t)variant;
        QualityObservationContext observation_context = {
            probe, object_index, subshader_index, pass_index, stage, variant, mode};
        options.source_quality_observer = observe_quality;
        options.source_quality_observer_context = &observation_context;
        if (mode) options.global_declarations = declarations;
        HLSLEmitDiagnostic diagnostic;
        bool emitted = hlsl_emit_with_options_diagnostic(&program, &source,
            selected_parameters, &pass->common_parameters[stage], &stage_names[stage], &options, &diagnostic);
        if (mode && emitted && !retain_candidate(probe, archive, pass, subshader_index,
                                                 pass_index, stage, variant, &raw, &source))
            ++probe->failures;
        report_emission(probe, object_index, subshader_index, pass_index, stage, variant,
                        mode, target_digest, emitted, &source, &map, &diagnostic, &quality);
        sb_free(&source);
    }
    failure = NULL;
cleanup:
    if (failure) {
        fprintf(probe->ledger,
            "{\"event\":\"stage_unavailable\",\"object\":%zu,\"subshader\":%d,"
            "\"pass\":%d,\"serialized_stage\":%d,\"variant\":%d,"
            "\"layer\":\"%s\",\"target_sha256\":\"%s\","
            "\"stage_contract\":\"%s\",\"stage_instruction\":%u,\"stage_opcode\":%u}\n",
            object_index, subshader_index, pass_index, stage, variant, failure,
            target_digest, dxbc_stage_contract_status_name(contract_diagnostic.status),
            contract_diagnostic.instruction_index, contract_diagnostic.opcode);
    }
    if (translated) usil_free(&program);
    hlsl_global_declarations_free(declarations);
    if (semantic_decoded) dxbc_free(&semantic);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    subprogram_metadata_free_variant(&player);
    serialized_program_parameters_free(&parameters);
}

static void measure_compilation(Probe *probe, size_t object_index,
                               const ShaderObject *object, const ShaderBlobArchive *archive) {
    UnityShaderLabLiftInput input = {
        .shader = &object->shader, .archive = archive, .profile = probe->profile,
        .broker = probe->broker, .source_path = "captured.shader",
        .source_directory = probe->compiler_source_directory, .source_basename = "captured.shader"
    };
    HLSLLiftLimits limits = {.max_candidates = 2, .max_compiles = 4096, .max_elapsed_ms = 120000};
    UnityShaderLabLiftResult *result = NULL;
    UnityShaderLabLiftServices services;
    unity_shaderlab_lift_default_services(&services);
    services.compile = observe_compile;
    services.context = probe;
    probe->current_object = object_index;
    HLSLLiftStatus status = unity_shaderlab_lift_run(&input, &services, &limits, &result);
    const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(result);
    const UnityShaderLabLiftArtifact *artifacts[] = {
        unity_shaderlab_lift_baseline(result), unity_shaderlab_lift_candidate(result)
    };
    char filename[96];
    for (size_t index = 0; index < 2; ++index) {
        const UnityShaderLabLiftArtifact *artifact = artifacts[index];
        if (!artifact || !artifact->source.len) continue;
        snprintf(filename, sizeof(filename), "object%zu-%s.shader", object_index,
                 index ? "candidate" : "baseline");
        if (!save_bytes(probe->output_directory, filename, artifact->source.buf, artifact->source.len))
            ++probe->failures;
    }
    snprintf(filename, sizeof(filename), "object%zu-lift.json", object_index);
    char *json = unity_shaderlab_lift_format_json(result);
    if (!json || !save_bytes(probe->output_directory, filename, json, strlen(json)))
        ++probe->failures;
    free(json);
    if (accepted) {
        ++probe->exact_objects;
        if (accepted->high_level) ++probe->high_level_objects;
        snprintf(filename, sizeof(filename), "object%zu-accepted.shader", object_index);
        if (!save_bytes(probe->output_directory, filename, accepted->source.buf, accepted->source.len))
            ++probe->failures;
    }
    fprintf(probe->ledger,
        "{\"event\":\"object_compile\",\"object\":%zu,\"status\":\"%s\","
        "\"accepted\":%s,\"accepted_high_level_candidate\":%s,\"native\":\"not-run\"}\n",
        object_index, hlsl_lift_status_name(status), accepted ? "true" : "false",
        accepted && accepted->high_level ? "true" : "false");
    unity_shaderlab_lift_result_free(result);
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Usage: %s RELEASED_INPUT COMPILE_PROFILE PROJECT_ROOT NEW_OUTPUT_DIRECTORY [SHADER_NAME]\n"
               "\nMeasure captured graphics stage quality and full generated-domain recompilation.\n"
               "The output directory must exist and must not contain previous probe outputs.\n"
               "Select the private Unity compiler through the normal compiler environment.\n"
               "Authored reference sources are never read. Compute inventory is retained;\n"
               "compute source reconstruction and physical D3D11 checks remain pending.\n", argv[0]);
        return 0;
    }
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s RELEASED_INPUT COMPILE_PROFILE PROJECT_ROOT NEW_OUTPUT_DIRECTORY [SHADER_NAME]\n", argv[0]);
        return 2;
    }
    struct stat directory;
    if (stat(argv[4], &directory) != 0 || !S_ISDIR(directory.st_mode)) return 2;
    UnityCompileProfile profile;
    if (unity_compile_profile_load(argv[2], &profile) != UNITY_COMPILE_PROFILE_OK) return 2;
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.retain_source_snapshots = true;
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    const char *inputs[] = {argv[1]};
    if (shader_catalog_build(inputs, 1, &options, &catalog) != SHADER_CATALOG_OK ||
        !shader_catalog_is_complete(&catalog)) {
        shader_catalog_dispose(&catalog);
        return 1;
    }
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(argv[3], NULL);
    if (!broker) { shader_catalog_dispose(&catalog); return 1; }
    UnityCompilerBroker *candidate_broker = unity_compiler_broker_create_lazy(argv[3], NULL);
    if (!candidate_broker) {
        unity_compiler_broker_destroy(broker);
        shader_catalog_dispose(&catalog);
        return 1;
    }
    char ledger_path[4096];
    int length = snprintf(ledger_path, sizeof(ledger_path), "%s/stage-quality.jsonl", argv[4]);
    FILE *ledger = length > 0 && (size_t)length < sizeof(ledger_path) ? fopen(ledger_path, "wx") : NULL;
    if (!ledger) {
        unity_compiler_broker_destroy(candidate_broker);
        unity_compiler_broker_destroy(broker);
        shader_catalog_dispose(&catalog);
        return 1;
    }
    Probe probe = {.output_directory = argv[4], .compiler_source_directory = argv[3],
                   .profile = &profile, .broker = broker,
                   .candidate_broker = candidate_broker, .ledger = ledger};
    for (size_t record_index = 0; record_index < catalog.record_count; ++record_index) {
        const ShaderCatalogRecord *record = &catalog.records[record_index];
        if (argc == 6 && (!record->name || strcmp(record->name, argv[5]) != 0)) continue;
        if (record->class_id == 72) {
            ++probe.compute_objects;
            probe.requested_compute_variants += record->compute_summary.kernel_variant_count;
            fprintf(ledger, "{\"event\":\"compute_inventory\",\"object\":%zu,"
                    "\"variants\":%zu,\"source_status\":\"declaration-inverse-unavailable\"}\n",
                    record_index, record->compute_summary.kernel_variant_count);
            continue;
        }
        ++probe.graphics_objects;
        ShaderObject object;
        shader_object_init(&object);
        ShaderCatalogObjectReport report;
        ShaderBlobArchive archive = {0};
        if (shader_catalog_decode_object(&catalog, record, NULL, &object, &report) != SHADER_CATALOG_OBJECT_OK ||
            shader_object_open_d3d11_archive(&object, &archive) != SHADER_OBJECT_OK) {
            ++probe.failures;
            shader_object_dispose(&object);
            continue;
        }
        for (int subshader = 0; subshader < object.shader.subshader_count; ++subshader)
            for (int pass = 0; pass < object.shader.subshaders[subshader].pass_count; ++pass)
                for (int stage = 0; stage < 5; ++stage) {
                    const SerializedPass *selected = &object.shader.subshaders[subshader].passes[pass];
                    for (int variant = 0; variant < selected->subprogram_count[stage]; ++variant)
                        if (serialized_pass_subprogram_is_platform(selected, stage, variant, 4))
                            measure_stage(&probe, record_index, &object.shader, &archive,
                                          subshader, pass, stage, variant);
                }
        measure_compilation(&probe, record_index, &object, &archive);
        free_candidates(&probe);
        shader_blob_archive_close(&archive);
        shader_object_dispose(&object);
        fflush(ledger);
    }
    fprintf(ledger, "{\"event\":\"summary\",\"graphics_objects\":%zu,\"compute_objects\":%zu,"
            "\"requested_graphics_rows\":%zu,\"requested_compute_variants\":%zu,"
            "\"parsed_rows\":%zu,\"low_level_emitted\":%zu,\"high_level_emitted\":%zu,"
            "\"exact_accepted_objects\":%zu,\"high_level_accepted_objects\":%zu,"
            "\"probe_failures\":%zu,\"native\":\"not-run\"}\n",
            probe.graphics_objects, probe.compute_objects, probe.requested_graphics_rows,
            probe.requested_compute_variants, probe.parsed_rows, probe.emitted[0], probe.emitted[1],
            probe.exact_objects, probe.high_level_objects, probe.failures);
    bool output_ok = !ferror(ledger);
    if (fclose(ledger) != 0) output_ok = false;
    unity_compiler_broker_destroy(broker);
    unity_compiler_broker_destroy(candidate_broker);
    shader_catalog_dispose(&catalog);
    return output_ok && !probe.failures && (probe.graphics_objects || probe.compute_objects) ? 0 : 1;
}
