// SPDX-License-Identifier: GPL-3.0-only

#include "app/shader_catalog.h"
#include "app/shader_catalog_object.h"
#include "common/sha256.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"
#include "translation/usil.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An argument-driven measurement tool, not a verifier. Every original D3D11
 * row is retained, including aliases and failures. The JSON contains numeric
 * shapes, digests and typed diagnostics, never source, asset names or paths.
 * Compilation, clean source and runtime equivalence require separate gates. */
typedef struct {
    uint64_t requested_rows;
    uint64_t decoded_rows;
    uint64_t projected_rows;
    uint64_t emitted_rows[2];
    uint64_t quality_classes[2][5];
    uint64_t invalid_quality_measurements;
    uint64_t failed_objects;
    uint64_t compute_objects;
} InventoryTotals;

static const char *stage_name(int stage) {
    static const char *const names[] = {"vertex", "fragment", "geometry", "hull", "domain"};
    return stage >= 0 && stage < 5 ? names[stage] : "unsupported-stage";
}

static void print_digest(FILE *output, const uint8_t digest[32]) {
    char text[65];
    common_sha256_digest_to_hex(digest, text);
    fprintf(output, "\"%s\"", text);
}

static void print_signature(FILE *output, const DXBCSignatureElement *elements, int count) {
    fputc('[', output);
    for (int index = 0; index < count; ++index) {
        const DXBCSignatureElement *element = &elements[index];
        if (index) fputc(',', output);
        fprintf(output, "{\"register\":%u,\"semantic_index\":%u,\"system_value\":%u,"
                        "\"component_type\":%u,\"mask\":%u,\"rw_mask\":%u,"
                        "\"min_precision\":%u,\"interpolation\":%u,\"stream\":%u}",
                element->register_id, element->semantic_index, element->system_value,
                element->component_type, element->mask, element->rw_mask,
                element->min_precision, element->interpolation_mode, element->stream_index);
    }
    fputc(']', output);
}

static void print_program_shape(FILE *output, const USILProgram *program) {
    uint64_t opcodes[(size_t)USIL_OP_IMM_ATOMIC_CMP_EXCH + 1U] = {0};
    unsigned precise = 0, saturated = 0, operand_precision = 0, relative = 0;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        if ((unsigned)instruction->opcode <= (unsigned)USIL_OP_IMM_ATOMIC_CMP_EXCH)
            ++opcodes[instruction->opcode];
        precise += instruction->precise_mask != 0;
        saturated += instruction->saturate;
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            const DXBCOperand *value = &instruction->operands[operand];
            operand_precision += value->min_precision != 0;
            relative += value->rel_op0 != NULL || value->rel_op1 != NULL || value->rel_op2 != NULL;
        }
    }
    fprintf(output, ",\"shape\":{\"shader_model\":[%u,%u],\"instructions\":%d,"
                    "\"temps\":%d,\"global_flags_present\":%s,\"global_flags\":%u,"
                    "\"precise_instructions\":%u,\"saturated_instructions\":%u,"
                    "\"minimum_precision_operands\":%u,\"relative_operands\":%u,"
                    "\"indexable_temps\":%d,\"index_ranges\":%d,\"inputs\":",
            program->shader_model_major, program->shader_model_minor, program->instruction_count,
            program->temp_count, program->has_global_flags ? "true" : "false", program->global_flags,
            precise, saturated, operand_precision, relative, program->indexable_temp_count,
            program->index_range_count);
    print_signature(output, program->inputs, program->input_count);
    fputs(",\"outputs\":", output);
    print_signature(output, program->outputs, program->output_count);
    fputs(",\"patch_constants\":", output);
    print_signature(output, program->patch_constants, program->patch_constant_count);
    fputs(",\"cbuffers\":[", output);
    for (int index = 0; index < program->cbuffer_count; ++index) {
        const USILConstantBuffer *buffer = &program->cbuffers[index];
        fprintf(output, "%s{\"register\":%d,\"rows\":%d,\"dynamic\":%s}",
                index ? "," : "", buffer->reg_idx, buffer->size,
                buffer->dynamic_indexed ? "true" : "false");
    }
    fputs("],\"textures\":[", output);
    for (int index = 0; index < program->texture_count; ++index) {
        const USILTexture *texture = &program->textures[index];
        fprintf(output, "%s{\"register\":%d,\"dimension\":\"%s\",\"stride\":%d,"
                        "\"samples\":%u,\"return_types\":[%u,%u,%u,%u]}",
                index ? "," : "", texture->reg_idx, texture->dimension, texture->stride,
                texture->sample_count, texture->return_types[0], texture->return_types[1],
                texture->return_types[2], texture->return_types[3]);
    }
    fputs("],\"samplers\":[", output);
    for (int index = 0; index < program->sampler_count; ++index) {
        const USILSampler *sampler = &program->samplers[index];
        fprintf(output, "%s{\"register\":%d,\"mode\":%u}", index ? "," : "",
                sampler->reg_idx, sampler->mode);
    }
    fputs("],\"uavs\":[", output);
    for (int index = 0; index < program->uav_count; ++index) {
        const USILUav *uav = &program->uavs[index];
        fprintf(output, "%s{\"register\":%d,\"dimension\":\"%s\",\"stride\":%d,"
                        "\"counter\":%s,\"coherent\":%s}", index ? "," : "",
                uav->reg_idx, uav->dimension, uav->stride,
                uav->has_order_preserving_counter ? "true" : "false",
                uav->globally_coherent ? "true" : "false");
    }
    fputs("],\"opcodes\":{", output);
    bool separator = false;
    for (size_t index = 0; index < sizeof(opcodes) / sizeof(opcodes[0]); ++index) {
        if (!opcodes[index]) continue;
        fprintf(output, "%s\"%s\":%" PRIu64, separator ? "," : "",
                hlsl_emit_opcode_name((int)index), opcodes[index]);
        separator = true;
    }
    fprintf(output, "},\"geometry\":{\"valid\":%s,\"input_primitive\":%u,"
                    "\"output_topology\":%u,\"max_vertices\":%u,\"instances\":%u,"
                    "\"declared_streams\":%u,\"referenced_streams\":%u},"
                    "\"tessellation\":{\"valid\":%s,\"domain\":%u,\"partitioning\":%u,"
                    "\"output_primitive\":%u,\"input_points\":%u,\"output_points\":%u,"
                    "\"phase_count\":%zu}}",
            program->geometry.valid ? "true" : "false", program->geometry.input_primitive,
            program->geometry.output_topology, program->geometry.max_output_vertex_count,
            program->geometry.instance_count, program->geometry.declared_stream_mask,
            program->geometry.referenced_stream_mask, program->tessellation.valid ? "true" : "false",
            program->tessellation.domain, program->tessellation.partitioning,
            program->tessellation.output_primitive, program->tessellation.input_control_point_count,
            program->tessellation.output_control_point_count, program->tessellation.phase_count);
}

static void print_parameters(FILE *output, const char *label,
                             const SerializedProgramParameters *parameters) {
    fprintf(output, ",\"%s\":{\"buffers\":[", label);
    for (int index = 0; index < parameters->cb_count; ++index) {
        const SerializedConstantBuffer *buffer = &parameters->constant_buffers[index];
        fprintf(output, "%s{\"role\":%u,\"size\":%u,\"partial_present\":%s,"
                        "\"partial\":%s,\"variables\":[", index ? "," : "", buffer->role,
                buffer->size, buffer->has_is_partial ? "true" : "false",
                buffer->is_partial ? "true" : "false");
        for (int variable = 0; variable < buffer->var_count; ++variable) {
            if (variable) fputc(',', output);
            fputc('[', output);
            for (int word = 0; word < 6; ++word)
                fprintf(output, "%s%u", word ? "," : "", buffer->variables[variable].layout[word]);
            fputc(']', output);
        }
        fputs("],\"structures\":[", output);
        for (int structure = 0; structure < buffer->struct_count; ++structure) {
            const SerializedStructParam *value = &buffer->struct_params[structure];
            fprintf(output, "%s{\"layout\":[%u,%u,%u],\"members\":[", structure ? "," : "",
                    value->layout[0], value->layout[1], value->layout[2]);
            for (int member = 0; member < value->member_count; ++member) {
                if (member) fputc(',', output);
                fputc('[', output);
                for (int word = 0; word < 6; ++word)
                    fprintf(output, "%s%u", word ? "," : "", value->members[member].layout[word]);
                fputc(']', output);
            }
            fputs("]}", output);
        }
        fputs("]}", output);
    }
    fputs("],\"resources\":[", output);
    for (int index = 0; index < parameters->res_count; ++index) {
        const SerializedResourceParam *resource = &parameters->resources[index];
        fprintf(output, "%s{\"kind\":%u,\"bind\":%u,\"array\":%u,\"dimension\":%u,"
                        "\"sampler\":%u,\"multisampled\":%s,\"sampler_state\":%u}",
                index ? "," : "", resource->bind_type, resource->bind_index, resource->array_size,
                resource->dimension, resource->sampler_index,
                resource->multisampled ? "true" : "false", resource->sampler_state);
    }
    fputs("]}", output);
}

static bool quality_counts_valid(const HLSLSourceQualityResult *quality) {
    const HLSLSourceQualityCounters *counts = &quality->counts;
    const size_t residuals[] = {
        counts->register_storage, counts->lane_transport, counts->scalarized_intrinsics,
        counts->raw_buffer_reconstruction, counts->synthetic_interface,
        counts->instruction_assignments, counts->unstructured_control, counts->storage_bitcasts
    };
    size_t total = 0;
    for (size_t index = 0; index < sizeof(residuals) / sizeof(residuals[0]); ++index) {
        if (residuals[index] > SIZE_MAX - total) return false;
        total += residuals[index];
    }
    return total == counts->residual_total &&
           (quality->classification != HLSL_SOURCE_QUALITY_CLEAN ||
            (!quality->reasons && !counts->residual_total && !counts->unknown_provenance &&
             !counts->incomplete_units));
}

static void print_quality(FILE *output, const HLSLSourceQualityResult *quality) {
    fprintf(output, ",\"source_quality\":{\"measured\":true,\"counter_consistency_valid\":%s,\"classification\":\"%s\","
                    "\"reasons\":%u,\"stage\":%u,\"pass\":%u,\"entry_point\":%u,\"counts\":{",
            quality_counts_valid(quality) ? "true" : "false",
            hlsl_source_quality_class_name(quality->classification), quality->reasons,
            quality->stage, quality->pass_index, quality->entry_point_index);
#define PRINT_COUNT(field, separator) \
    fprintf(output, separator "\"" #field "\":%zu", quality->counts.field)
    PRINT_COUNT(ast_expressions, "");
    PRINT_COUNT(ast_statements, ",");
    PRINT_COUNT(emission_events, ",");
    PRINT_COUNT(inspected_units, ",");
    PRINT_COUNT(incomplete_units, ",");
    PRINT_COUNT(unknown_provenance, ",");
    PRINT_COUNT(logical_operations, ",");
    PRINT_COUNT(logical_value_references, ",");
    PRINT_COUNT(semantic_projections, ",");
    PRINT_COUNT(real_bitcasts, ",");
    PRINT_COUNT(register_storage, ",");
    PRINT_COUNT(lane_transport, ",");
    PRINT_COUNT(scalarized_intrinsics, ",");
    PRINT_COUNT(raw_buffer_reconstruction, ",");
    PRINT_COUNT(synthetic_interface, ",");
    PRINT_COUNT(instruction_assignments, ",");
    PRINT_COUNT(unstructured_control, ",");
    PRINT_COUNT(storage_bitcasts, ",");
    PRINT_COUNT(sibling_declarations, ",");
    PRINT_COUNT(sibling_declaration_witnesses, ",");
    PRINT_COUNT(residual_total, ",");
#undef PRINT_COUNT
    fprintf(output, "},\"first_issue\":");
    if (quality->has_first_issue) {
        const HLSLSourceQualityObservation *issue = &quality->first_issue;
        fprintf(output, "{\"unit\":%u,\"unit_kind\":%u,\"observation_kind\":%u,"
                        "\"ast_kind\":%d,\"reasons\":%u,\"known\":%s,\"value_kind\":%u,"
                        "\"logical_value\":%" PRIu64 ",\"components\":%u,\"artifacts\":%u,"
                        "\"instruction\":%d,\"source_instruction\":%u,\"lanes\":%u}",
                issue->source_unit_id, issue->unit_kind, issue->kind, issue->ast_kind,
                issue->reasons, issue->facts.known ? "true" : "false", issue->facts.value_kind,
                issue->facts.logical_value_id, issue->facts.components, issue->facts.artifacts,
                issue->facts.instruction_index, issue->facts.source_instruction_index, issue->facts.lanes);
    } else fputs("null", output);
    fputc('}', output);
}

static void print_emission(FILE *output, const char *label, bool success,
                           const HLSLEmitDiagnostic *diagnostic, const StringBuilder *source,
                           const HLSLSourceQualityResult *quality) {
    fprintf(output, ",\"%s\":{\"emitted\":%s,\"status\":\"%s\",\"phase\":\"%s\","
                    "\"reason\":\"%s\",\"instruction\":%d,\"opcode\":%d,"
                    "\"metadata_kind\":\"%s\",\"source_bytes\":%zu,\"source_sha256\":",
            label, success ? "true" : "false", hlsl_emit_status_name(diagnostic->status),
            hlsl_emit_phase_name(diagnostic->phase), hlsl_emit_reason_name(diagnostic->reason),
            diagnostic->instruction_index, diagnostic->opcode,
            hlsl_emit_metadata_kind_name(diagnostic->metadata.kind), success ? source->len : 0U);
    if (success) {
        uint8_t digest[32];
        common_sha256(source->buf, source->len, digest);
        print_digest(output, digest);
    } else fputs("null", output);
    print_quality(output, quality);
    fputc('}', output);
}

static bool inventory_row(FILE *output, size_t object_index, int subshader_index,
                           int pass_index, int stage, int row, const SerializedShader *shader,
                           const SerializedPass *pass, const ShaderBlobArchive *archive,
                           bool unique_blob, InventoryTotals *totals) {
    ++totals->requested_rows;
    const SerializedSubProgram *subprogram = &pass->subprograms[stage][row];
    fprintf(output, "{\"schema\":\"dxbc-hlsl-inventory-row-v2\",\"object\":%zu,"
                    "\"subshader\":%d,\"pass\":%d,\"stage\":\"%s\",\"row\":%d,"
                    "\"blob\":%d,\"unique_blob\":%s", object_index, subshader_index,
            pass_index, stage_name(stage), row, subprogram->blob_index,
            unique_blob ? "true" : "false");
    bool success = false, semantic_ready = false, projected = false, variant_ready = false;
    const char *failure = NULL;
    const uint8_t *bytes = NULL;
    size_t size = 0;
    PlayerSubProgramMetadata variant = {0};
    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    const SerializedProgramParameters *selected = &pass->common_parameters[stage];
    DXBCDocument document;
    DXBCStageContract contract;
    DXBCContainer semantic = {0};
    USILProgram program = {0};
    DXBCDocumentDiagnostic document_diagnostic = {0};
    DXBCStageContractDiagnostic stage_diagnostic = {0};
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    int parameter_blob = pass->subprogram_param_blob_indices[stage]
                             ? pass->subprogram_param_blob_indices[stage][row] : -1;
    if (parameter_blob < -1) { failure = "invalid-parameter-coordinate"; goto done; }
    if (parameter_blob >= 0) {
        if (!shader_blob_archive_get(archive, parameter_blob, &bytes, &size)) {
            failure = "parameter-blob-unavailable"; goto done;
        }
        ByteStream stream;
        stream_init(&stream, bytes, size);
        stream_set_endian(&stream, false);
        if (!subprogram_metadata_parse_parameters(&stream, &parameters)) {
            failure = "parameter-decode-failed"; goto done;
        }
        selected = &parameters;
    }
    if (!shader_blob_archive_get(archive, subprogram->blob_index, &bytes, &size)) {
        failure = "variant-blob-unavailable"; goto done;
    }
    ByteStream stream;
    stream_init(&stream, bytes, size);
    stream_set_endian(&stream, false);
    if (!subprogram_metadata_parse_variant(&stream, &variant)) {
        failure = "variant-decode-failed"; goto done;
    }
    variant_ready = true;
    if (variant.program_type != subprogram->program_type ||
        !subprogram_metadata_local_keyword_set_matches(&variant, subprogram)) {
        failure = "variant-metadata-mismatch"; goto done;
    }
    DXBCContainerView view;
    if (!dxbc_container_view_first(variant.bytecode, variant.bytecode_length, &view) ||
        !dxbc_document_parse(&document, view.data, view.size, &document_diagnostic)) {
        failure = "dxbc-document-failed"; goto done;
    }
    ++totals->decoded_rows;
    uint8_t target_digest[32];
    common_sha256(view.data, view.size, target_digest);
    fputs(",\"target_sha256\":", output);
    print_digest(output, target_digest);
    if (!dxbc_stage_contract_decode_document(&document, &contract, &stage_diagnostic)) {
        failure = "stage-contract-failed"; goto done;
    }
    UnityCompilerProgramStage compiler_stage;
    ShaderStageTuple tuple = {0};
    if (!shader_stage_serialized_to_compiler((UnitySerializedProgramStage)stage, &compiler_stage)) {
        failure = "stage-tuple-unmapped"; goto done;
    }
    tuple.serialized_stage = (UnitySerializedProgramStage)stage;
    tuple.compiler_program = compiler_stage;
    tuple.serialized_program_mask = pass->program_mask;
    tuple.gpu_program_type = (UnityGPUProgramType)variant.program_type;
    tuple.dxbc_program_type = contract.program_type;
    tuple.shader_model_major = contract.shader_model_major;
    tuple.shader_model_minor = contract.shader_model_minor;
    if (shader_stage_validate_d3d11_tuple(&tuple) != SHADER_STAGE_TUPLE_OK) {
        failure = "stage-tuple-mismatch"; goto done;
    }
    if (!dxbc_document_decode_semantic(&document, &semantic)) {
        failure = "semantic-decode-failed"; goto done;
    }
    semantic_ready = true;
    if (!dxbc_stage_contract_validate_container(&contract, &semantic, &stage_diagnostic) ||
        !usil_translate_with_stage_contract(&program, &semantic, &contract)) {
        failure = "usil-projection-failed"; goto done;
    }
    projected = true;
    ++totals->projected_rows;
    print_program_shape(output, &program);
    print_parameters(output, "selected_parameters", selected);
    print_parameters(output, "common_parameters", &pass->common_parameters[stage]);
    const HLSLEmitNames names = {"main", "StageInput", "StageOutput"};
    for (int mode = 0; mode < 2; ++mode) {
        HLSLEmitOptions options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
        if (mode) options.mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
        options.omit_unity_builtin_declarations = true;
        options.reserved_preprocessor_identifiers = (const char *const *)shader->keyword_names.keywords;
        options.reserved_preprocessor_identifier_count = (size_t)shader->keyword_names.count;
        HLSLSourceQualityResult quality;
        options.source_quality = &quality;
        options.source_quality_pass_index = (uint32_t)pass_index;
        options.source_quality_entry_point_index = (uint32_t)row;
        StringBuilder source;
        sb_init(&source);
        HLSLEmitDiagnostic diagnostic;
        bool emitted = hlsl_emit_with_options_diagnostic(&program, &source, selected,
                       &pass->common_parameters[stage], &names, &options, &diagnostic);
        if (emitted) ++totals->emitted_rows[mode];
        if (!quality_counts_valid(&quality)) ++totals->invalid_quality_measurements;
        else if ((unsigned)quality.classification < 5u)
            ++totals->quality_classes[mode][quality.classification];
        print_emission(output, mode ? "candidate" : "recompile", emitted, &diagnostic, &source, &quality);
        sb_free(&source);
    }
    success = true;
done:
    if (failure) {
        fprintf(output, ",\"document_diagnostic\":{\"code\":\"%s\",\"byte\":%zu,"
                        "\"chunk\":%u,\"instruction\":%u},"
                        "\"stage_diagnostic\":{\"status\":\"%s\",\"chunk\":%u,"
                        "\"instruction\":%u,\"opcode\":%u}",
                dxbc_document_diagnostic_code_name(document_diagnostic.code),
                document_diagnostic.byte_offset, document_diagnostic.chunk_index,
                document_diagnostic.instruction_index,
                dxbc_stage_contract_status_name(stage_diagnostic.status),
                stage_diagnostic.chunk_index, stage_diagnostic.instruction_index,
                stage_diagnostic.opcode);
    }
    fprintf(output, ",\"pipeline_status\":\"%s\",\"quality\":\"%s\","
                    "\"compile_exactness\":\"not-run\",\"native\":\"not-run\"}\n",
            failure ? failure : "ok", projected ? "measured-by-emission" : "not-measured");
    if (projected) usil_free(&program);
    if (semantic_ready) dxbc_free(&semantic);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    if (variant_ready) subprogram_metadata_free_variant(&variant);
    serialized_program_parameters_free(&parameters);
    return success;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s SCHEMA_REGISTRY OUTPUT_JSONL INPUT...\n", argv[0]);
        return 0;
    }
    if (argc < 4) {
        fprintf(stderr, "Usage: %s SCHEMA_REGISTRY OUTPUT_JSONL INPUT...\n", argv[0]);
        return 2;
    }
    FILE *output = fopen(argv[2], "wbx");
    if (!output) return 2;
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    int exit_status = 1;
    if (typetree_schema_registry_import_file_replace(&registry, argv[1]) != TYPETREE_SCHEMA_OK)
        goto done;
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = &registry;
    options.retain_source_snapshots = true;
    if (shader_catalog_build((const char *const *)&argv[3], (size_t)(argc - 3), &options,
                             &catalog) != SHADER_CATALOG_OK) goto done;
    InventoryTotals totals = {0};
    for (size_t index = 0; index < catalog.record_count; ++index) {
        const ShaderCatalogRecord *record = &catalog.records[index];
        if (record->class_id == 72) {
            ++totals.compute_objects;
            fprintf(output, "{\"schema\":\"dxbc-hlsl-inventory-object-v1\",\"object\":%zu,"
                            "\"class_id\":72,\"source_authority\":\"%s\",\"kernels\":%zu,"
                            "\"variants\":%zu,\"status\":\"compute-source-not-measured\"}\n", index,
                    compute_shader_source_authority_status_name(record->compute_source_authority_status),
                    record->compute_summary.kernel_parent_count, record->compute_summary.kernel_variant_count);
            continue;
        }
        ShaderObject object;
        shader_object_init(&object);
        ShaderCatalogObjectReport report;
        ShaderBlobArchive archive = {0};
        if (shader_catalog_decode_object(&catalog, record, &registry, &object, &report) !=
            SHADER_CATALOG_OBJECT_OK ||
            shader_object_open_d3d11_archive(&object, &archive) != SHADER_OBJECT_OK) {
            ++totals.failed_objects;
            fprintf(output, "{\"schema\":\"dxbc-hlsl-inventory-object-v1\",\"object\":%zu,"
                            "\"status\":\"object-or-archive-unavailable\"}\n", index);
            shader_object_dispose(&object);
            continue;
        }
        bool *seen = calloc((size_t)archive.entry_count, sizeof(*seen));
        if (archive.entry_count && !seen) {
            shader_blob_archive_close(&archive); shader_object_dispose(&object); goto done;
        }
        const SerializedShader *shader = &object.shader;
        for (int subshader = 0; subshader < shader->subshader_count; ++subshader) {
            const SerializedSubShader *sub = &shader->subshaders[subshader];
            for (int pass = 0; pass < sub->pass_count; ++pass) {
                const SerializedPass *program_pass = &sub->passes[pass];
                for (int stage = 0; stage < 5; ++stage) {
                    for (int row = 0; row < program_pass->subprogram_count[stage]; ++row) {
                        if (!serialized_pass_subprogram_is_platform(program_pass, stage, row, 4))
                            continue;
                        int blob = program_pass->subprograms[stage][row].blob_index;
                        bool unique = blob >= 0 && blob < archive.entry_count && !seen[blob];
                        if (unique) seen[blob] = true;
                        inventory_row(output, index, subshader, pass, stage, row, shader,
                                      program_pass, &archive, unique, &totals);
                    }
                }
            }
        }
        free(seen);
        shader_blob_archive_close(&archive);
        shader_object_dispose(&object);
    }
    fprintf(output, "{\"schema\":\"dxbc-hlsl-inventory-summary-v2\",\"objects\":%zu,"
                    "\"requested_rows\":%" PRIu64 ",\"decoded_rows\":%" PRIu64 ","
                    "\"projected_rows\":%" PRIu64 ",\"recompile_emitted_rows\":%" PRIu64 ","
                    "\"candidate_emitted_rows\":%" PRIu64 ",\"failed_objects\":%" PRIu64 ","
                    "\"compute_objects_source_not_measured\":%" PRIu64, catalog.record_count,
            totals.requested_rows, totals.decoded_rows, totals.projected_rows,
            totals.emitted_rows[0], totals.emitted_rows[1], totals.failed_objects, totals.compute_objects);
    fprintf(output, ",\"invalid_quality_measurements\":%" PRIu64, totals.invalid_quality_measurements);
    fputs(",\"source_quality_classes\":{", output);
    for (int mode = 0; mode < 2; ++mode) {
        fprintf(output, "%s\"%s\":{", mode ? "," : "", mode ? "candidate" : "recompile");
        for (int classification = 0; classification < 5; ++classification)
            fprintf(output, "%s\"%s\":%" PRIu64, classification ? "," : "",
                    hlsl_source_quality_class_name((HLSLSourceQualityClass)classification),
                    totals.quality_classes[mode][classification]);
        fputc('}', output);
    }
    fputs("}}\n", output);
    exit_status = shader_catalog_is_complete(&catalog) && !totals.failed_objects && !totals.invalid_quality_measurements && totals.projected_rows == totals.requested_rows && !ferror(output)
                      ? 0 : 1;
done:
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    if (fclose(output) != 0) exit_status = 1;
    return exit_status;
}
