// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "common/sha256.h"
#include "test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "Failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    const char *file;
    const char *digest;
    USILOpcode arithmetic;
    unsigned model;
} MaterialDiscardCase;

typedef struct {
    SerializedVariable fields[2];
    SerializedConstantBuffer buffer;
    SerializedResourceParam resources[2];
    SerializedProgramParameters parameters;
} MaterialMetadata;

/* Used-field tuples actually observed from the generic native controls;
 * reflected counts of dormant source declarations remain unavailable. */
static void material_metadata_init(MaterialMetadata *metadata, bool mad) {
    *metadata = (MaterialMetadata){0};
    metadata->fields[0] = (SerializedVariable){.name = mad ? "_Cutoff" : "_ResultColor",
        .layout = {mad ? 48u : 32u, 0, 0, mad ? 1u : 4u, 0, 0}};
    metadata->fields[1] = (SerializedVariable){.name = mad ? "_CoverageScale" : "_Cutoff",
        .layout = {mad ? 64u : 48u, 0, 0, mad ? 4u : 1u, 0, 0}};
    metadata->buffer = (SerializedConstantBuffer){.role = SERIALIZED_CBUFFER_NAMED,
        .name = "MaterialValues",
        .size = mad ? 80 : 64,
        .var_count = 2,
        .variables = metadata->fields,
        .has_is_partial = false,
        .is_partial = false};
    metadata->resources[0] = (SerializedResourceParam){.name = "MaterialValues",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER,
        .bind_index = 0,
        .array_size = 1,
        .extra = {1}};
    metadata->resources[1] = (SerializedResourceParam){.name = "_CoverageTexture",
        .bind_type = SERIALIZED_RESOURCE_TEXTURE,
        .bind_index = 0,
        .sampler_index = 0,
        .dimension = 2,
        .extra = {0, 0}};
    metadata->parameters = (SerializedProgramParameters){.cb_count = 1,
        .constant_buffers = &metadata->buffer,
        .res_count = 2,
        .resources = metadata->resources};
}

static bool check_analysis(USILProgram *program);

static void set_temp(DXBCOperand *operand, int index) {
    operand->register_index = index;
    operand->rel_offset1 = index;
    operand->index_values[0] = (uint32_t)index;
}

static bool check_register_aliases(USILProgram *program) {
    const USILProgram parent = *program;
    USILInstruction original[6];
    memcpy(original, program->instructions, sizeof(original));
    bool passed = false;
    program->temp_count = 2;
    for (int color = 0; color < 2; ++color) {
        for (int value = 0; value < 2; ++value) {
            for (int predicate = 0; predicate < 2; ++predicate) {
                memcpy(program->instructions, original, sizeof(original));
                set_temp(&program->instructions[0].operands[0], color);
                set_temp(&program->instructions[1].operands[0], value);
                set_temp(&program->instructions[1].operands[1], color);
                set_temp(&program->instructions[2].operands[0], predicate);
                set_temp(&program->instructions[2].operands[1], value);
                set_temp(&program->instructions[3].operands[0], predicate);
                HLSLSampledDiscardPlan plan;
                REQUIRE(hlsl_sampled_discard_plan_prepare(program, &plan) && plan.material &&
                        plan.color_register == color && plan.predicate_register == predicate);
                REQUIRE(check_analysis(program));
                set_temp(&program->instructions[2].operands[1], 1 - value);
                REQUIRE(!hlsl_sampled_discard_plan_prepare(program, &plan) && !plan.present);
            }
        }
    }
    passed = true;

cleanup:
    *program = parent;
    memcpy(program->instructions, original, sizeof(original));
    return passed;
}

static bool check_analysis(USILProgram *program) {
    MaterialMetadata metadata;
    material_metadata_init(&metadata, program->instructions[1].opcode == USIL_OP_MAD);
    HLSLEmitterContext *ctx = calloc(1, sizeof(*ctx));
    StringBuilder scratch;
    sb_init(&scratch);
    bool passed = false;
    REQUIRE(ctx);
    ctx->program = program;
    ctx->params = &metadata.parameters;
    ctx->sb = &scratch;
    ctx->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    REQUIRE(build_cbuffer_register_map(ctx) && build_control_flow_graph(ctx) &&
            compute_dominance(&ctx->cfg) && build_cbuffer_emission_layouts(ctx) &&
            build_hlsl_ssa_graph(ctx) && build_component_provenance(ctx) &&
            build_hlsl_use_def_graph(ctx));
    REQUIRE(hlsl_sampled_discard_preflight(ctx));
    ctx->cfg.blocks[1].may_exit = false;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->cfg.blocks[1].may_exit = true;
    ctx->use_def.definition_use_counts[0] += 1;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->use_def.definition_use_counts[0] -= 1;
    ctx->compiler_model.replacement_count = 1;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->compiler_model.replacement_count = 0;
    for (unsigned index = 0; index < 2; ++index) {
        metadata.fields[index].layout[1] = 1;
        REQUIRE(!hlsl_sampled_discard_preflight(ctx));
        metadata.fields[index].layout[1] = 0;
    }
    REQUIRE(hlsl_sampled_discard_preflight(ctx));
    passed = true;

cleanup:
    if (ctx) {
        free_hlsl_use_def_graph(ctx);
        free_component_provenance(ctx);
        free_hlsl_ssa_graph(ctx);
        free_control_flow_graph(ctx);
        free_cbuffer_emission_layouts(ctx);
        free(ctx->cb_reg_map);
        free(ctx);
    }
    sb_free(&scratch);
    return passed;
}

static bool check_boundaries(USILProgram *program) {
    const USILProgram parent = *program;
    const USILConstantBuffer buffer = program->cbuffers[0];
    USILInstruction original[6];
    memcpy(original, program->instructions, sizeof(original));
    bool passed = false;
    for (unsigned mutation = 0; mutation < 15; ++mutation) {
        memcpy(program->instructions, original, sizeof(original));
        *program = parent;
        program->cbuffers[0] = buffer;
        switch (mutation) {
            case 0:
                program->instructions[1].opcode = USIL_OP_MUL;
                break;
            case 1:
                program->instructions[3].condition_test = 0;
                break;
            case 2:
                program->cbuffers[0].dynamic_indexed = true;
                break;
            case 3:
                program->cbuffers[0].size = 0;
                break;
            case 4:
                program->cbuffer_count = 0;
                break;
            case 5:
                program->sampler_count = 2;
                break;
            case 6:
                program->instructions[0].opcode = USIL_OP_SAMPLE_L;
                break;
            case 7:
                program->instructions[1].operands[0].destination_mask = 0xf0;
                break;
            case 8:
                program->instructions[2].operands[1].swizzle[0] = 1;
                break;
            case 9:
                program->instructions[1].operands[2].index_representations[1] = 2;
                break;
            case 10:
                program->instructions[1].operands[2].rel_offset0 = buffer.size;
                break;
            case 11:
                program->instructions[2].saturate = true;
                break;
            case 12:
                program->instructions[4].source_instruction_index += 1;
                break;
            case 13:
                program->instructions[4].operands[1].has_neg = true;
                break;
            case 14:
                program->cbuffers = NULL;
                break;
        }
        HLSLSampledDiscardPlan plan = {.present = true, .material = true};
        REQUIRE(!hlsl_sampled_discard_plan_prepare(program, &plan) && !plan.present &&
                !plan.material && !plan.comparison_index && !plan.discard_index &&
                !plan.output_index && plan.arithmetic_opcode == USIL_OP_NOP);
    }
    passed = true;

cleanup:
    *program = parent;
    memcpy(program->instructions, original, sizeof(original));
    program->cbuffers[0] = buffer;
    return passed;
}

typedef struct {
    USILProgram *program;
    MaterialMetadata *metadata;
    StringBuilder *source;
    HLSLExpressionSourceMap *map;
    HLSLEmitOptions *options;
    HLSLEmitNames *names;
    unsigned observations;
    unsigned mutate_at;
    unsigned mutation;
    bool mutated;
} MaterialObserver;

static bool observe_material(void *context, const HLSLSourceQualityObservation *observation) {
    MaterialObserver *state = context;
    (void)observation;
    if (++state->observations != state->mutate_at) {
        return true;
    }
    state->mutated = true;
    switch (state->mutation) {
        case 0:
            return false;
        case 1:
            state->metadata->fields[0].layout[1] = 1;
            break;
        case 2:
            state->metadata->fields[1].layout[0] += 16;
            break;
        case 3:
            state->metadata->buffer.size += 16;
            break;
        case 4:
            state->metadata->resources[1].dimension = 3;
            break;
        case 5:
            state->program->instructions[1].opcode = USIL_OP_MUL;
            break;
        case 6:
            state->program->instructions[3].condition_test ^= 3;
            break;
        case 7:
            state->source->buf[0] = '!';
            break;
        case 8:
            state->map->count = 0;
            state->map->complete = true;
            break;
        case 9:
            state->options->mode = HLSL_EMIT_MODE_RECOMPILE;
            break;
        case 10:
            state->names->entry_point = "changed";
            break;
        case 11:
            state->metadata->buffer.has_is_partial = true;
            state->metadata->buffer.is_partial = true;
            break;
        case 12:
            state->metadata->buffer.variables = NULL;
            break;
        case 13:
            state->program->cbuffer_count = 0;
            break;
        case 14:
            state->program->cbuffers[0].size += 1;
            break;
        default:
            return false;
    }
    return true;
}

static bool emit_observed_material(USILProgram *program,
    MaterialMetadata *metadata,
    StringBuilder *source,
    HLSLExpressionSourceMap *map,
    HLSLSourceQualityResult *quality,
    MaterialObserver *observer,
    const char *reserved) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLEmitNames names = {.entry_point = "frag"};
    HLSLEmitDiagnostic diagnostic;
    options.expression_source_map = map;
    options.source_quality = quality;
    options.reserved_preprocessor_identifiers = reserved ? &reserved : NULL;
    options.reserved_preprocessor_identifier_count = reserved ? 1 : 0;
    if (observer) {
        observer->program = program;
        observer->metadata = metadata;
        observer->source = source;
        observer->map = map;
        observer->options = &options;
        observer->names = &names;
        options.source_quality_observer = observe_material;
        options.source_quality_observer_context = observer;
    }
    return hlsl_emit_with_options_diagnostic(
        program, source, &metadata->parameters, NULL, &names, &options, &diagnostic);
}

static bool check_callbacks(USILProgram *program) {
    const USILProgram parent = *program;
    const USILConstantBuffer buffer = program->cbuffers[0];
    USILInstruction instructions[6];
    memcpy(instructions, program->instructions, sizeof(instructions));
    MaterialMetadata metadata;
    material_metadata_init(&metadata, instructions[1].opcode == USIL_OP_MAD);
    const MaterialMetadata original = metadata;
    StringBuilder baseline;
    sb_init(&baseline);
    StringBuilder source;
    sb_init(&source);
    const char *prefix = "// caller prefix\n";
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    MaterialObserver observer = {0};
    bool passed = false;
    sb_append(&baseline, prefix);
    REQUIRE(emit_observed_material(program, &metadata, &baseline, &map, &quality, &observer, NULL));
    REQUIRE(observer.observations && quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        sb_append(&source, prefix);
        REQUIRE(emit_observed_material(program,
            &metadata,
            &source,
            outputs & 1 ? &map : NULL,
            outputs & 2 ? &quality : NULL,
            NULL,
            NULL));
        REQUIRE(source.len == baseline.len && !memcmp(source.buf, baseline.buf, baseline.len));
        sb_free(&source);
        sb_init(&source);
    }
    static const char *const reserved[] = {"sampledValue", "coverageValue", "discardFragment"};
    for (size_t index = 0; index < sizeof(reserved) / sizeof(*reserved); ++index) {
        sb_append(&source, prefix);
        REQUIRE(!emit_observed_material(
            program, &metadata, &source, &map, &quality, NULL, reserved[index]));
        REQUIRE(source.failed && !map.complete && !map.count && !strcmp(source.buf, prefix));
        sb_free(&source);
        sb_init(&source);
    }
    for (unsigned mutation = 0; mutation < 15; ++mutation) {
        for (unsigned event = 1; event <= observer.observations; ++event) {
            MaterialObserver drift = {.mutate_at = event, .mutation = mutation};
            sb_append(&source, prefix);
            const bool emitted =
                emit_observed_material(program, &metadata, &source, &map, &quality, &drift, NULL);
            *program = parent;
            memcpy(program->instructions, instructions, sizeof(instructions));
            program->cbuffers[0] = buffer;
            metadata = original;
            REQUIRE(!emitted && drift.mutated && source.failed && !map.complete && !map.count &&
                    quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
                    !strcmp(source.buf, prefix));
            sb_free(&source);
            sb_init(&source);
        }
    }
    sb_append(&source, prefix);
    REQUIRE(emit_observed_material(program, &metadata, &source, &map, &quality, NULL, NULL));
    REQUIRE(source.len == baseline.len && !memcmp(source.buf, baseline.buf, baseline.len));
    passed = true;

cleanup:
    *program = parent;
    memcpy(program->instructions, instructions, sizeof(instructions));
    program->cbuffers[0] = buffer;
    sb_free(&source);
    sb_free(&baseline);
    return passed;
}

static bool check_emission(USILProgram *program) {
    MaterialMetadata metadata;
    material_metadata_init(&metadata, program->instructions[1].opcode == USIL_OP_MAD);
    StringBuilder source;
    sb_init(&source);
    StringBuilder repeated;
    sb_init(&repeated);
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    HLSLEmitDiagnostic diagnostic;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    const HLSLEmitNames names = {.entry_point = "frag"};
    options.expression_source_map = &map;
    options.source_quality = &quality;
    bool passed = false;
    const bool emitted = hlsl_emit_with_options_diagnostic(
        program, &source, &metadata.parameters, NULL, &names, &options, &diagnostic);
    if (!emitted || quality.classification != HLSL_SOURCE_QUALITY_CLEAN) {
        fprintf(stderr,
            "Material emission=%d status=%s phase=%s reason=%s quality=%s reasons=%u bytes=%zu\n",
            emitted,
            hlsl_emit_status_name(diagnostic.status),
            hlsl_emit_phase_name(diagnostic.phase),
            hlsl_emit_reason_name(diagnostic.reason),
            hlsl_source_quality_class_name(quality.classification),
            quality.reasons,
            source.len);
        if (emitted && source.len < 4096) {
            fprintf(stderr, "Material source:\n%s\n", source.buf);
        }
    }
    REQUIRE(emitted && map.complete && map.count == 6 &&
            hlsl_expression_source_map_matches(&map, program, source.buf));
    REQUIRE(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.reasons &&
            quality.counts.ast_statements == 3);
    REQUIRE(strstr(source.buf, "const float sampledValue = _CoverageTexture.Sample(") &&
            strstr(source.buf, "const float coverageValue = ") &&
            strstr(source.buf, "const bool discardFragment = ") &&
            strstr(source.buf, "if (discardFragment)\n") && strstr(source.buf, "discard;\n"));
    REQUIRE(
        map.origins[3].kind == HLSL_EXPRESSION_ORIGIN_EFFECT && !map.origins[3].destination_lanes);
    metadata.buffer.has_is_partial = true;
    metadata.buffer.is_partial = true;
    REQUIRE(hlsl_emit_with_options_diagnostic(
        program, &repeated, &metadata.parameters, NULL, &names, &options, &diagnostic));
    REQUIRE(quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
            quality.reasons == HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE &&
            repeated.len == source.len && !memcmp(repeated.buf, source.buf, source.len));
    sb_free(&repeated);
    sb_init(&repeated);

    SerializedConstantBuffer shell = metadata.buffer;
    shell.has_is_partial = false;
    shell.is_partial = false;
    shell.var_count = 0;
    shell.variables = NULL;
    const SerializedProgramParameters current = {
        .is_binary = true, .cb_count = 1, .constant_buffers = &shell};
    REQUIRE(hlsl_emit_with_options_diagnostic(
        program, &repeated, &current, &metadata.parameters, &names, &options, &diagnostic));
    REQUIRE(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.reasons &&
            repeated.len == source.len && !memcmp(repeated.buf, source.buf, source.len) &&
            hlsl_expression_source_map_matches(&map, program, repeated.buf));
    passed = true;

cleanup:
    sb_free(&repeated);
    sb_free(&source);
    return passed;
}

static bool check_case(const MaterialDiscardCase *fixture) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    USILProgram program = {0};
    bool passed = false;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hex[COMMON_SHA256_HEX_SIZE];
    HLSLSampledDiscardPlan plan;
    HLSLSampledFragmentPlan admitted;
    REQUIRE(test_fixture_decode_base64(fixture->file, &bytes, &size));
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, hex);
    REQUIRE(!strcmp(hex, fixture->digest));
    REQUIRE(dxbc_document_parse(&document, bytes, size, NULL) &&
            dxbc_document_decode_semantic(&document, &semantic) &&
            dxbc_stage_contract_decode(&document, &semantic, &contract, NULL) &&
            usil_translate_with_stage_contract(&program, &semantic, &contract));
    REQUIRE(program.instruction_count == 6 && program.shader_model_major == fixture->model &&
            program.cbuffer_count == 1 && program.temp_count == 1);
    REQUIRE(
        hlsl_decoded_graphics_owner_storage_valid(&program, HLSL_GRAPHICS_OWNER_SAMPLED_MATERIAL));
    const bool prepared = hlsl_sampled_discard_plan_prepare(&program, &plan);
    if (!prepared) {
        fprintf(stderr,
            "Material fixture=%s model=%u flags=%d/%u inputs=%d declarations=%d\n",
            fixture->file,
            program.shader_model_major,
            program.has_global_flags,
            program.global_flags,
            program.input_count,
            program.signature_declaration_count);
    }
    REQUIRE(prepared);
    REQUIRE(plan.present && plan.material && plan.arithmetic_opcode == fixture->arithmetic &&
            !plan.arithmetic_component && plan.comparison_opcode == USIL_OP_LT &&
            plan.comparison_index == 2 && plan.discard_index == 3 && plan.output_index == 4 &&
            plan.color_register == 0 && plan.predicate_register == 0 &&
            plan.sampled_component == 3 && plan.scalar_sample == (fixture->model == 5) &&
            plan.color_component == (fixture->model == 5 ? 0 : 3) && !plan.predicate_component &&
            !plan.literal_first && !plan.discard_on_zero && !plan.threshold_bits);
    /* This decoded hint supplies no prepared graph or source-quality claim. */
    REQUIRE(hlsl_sampled_fragment_plan_prepare(&program, &admitted) && admitted.present &&
            admitted.discard && admitted.material_discard);
    REQUIRE(check_register_aliases(&program) && check_boundaries(&program) &&
            check_emission(&program) && check_callbacks(&program));
    passed = true;

cleanup:
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    mem_free(bytes, size);
    return passed;
}

int main(void) {
    static const MaterialDiscardCase cases[] = {
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/add_sm4.dxbc.b64",
            "6752dbdbb5fc8a4e7abd865b0299e5a41fcb2b2c15e49d309d26bff3c39f87df",
            USIL_OP_ADD,
            4},
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/add_sm5.dxbc.b64",
            "5e43e12a964022a52969d072134cb3f1375b99d0eff56be716c2472eefcbc961",
            USIL_OP_ADD,
            5},
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/mad_sm4.dxbc.b64",
            "1e1aab1fa651783ef0bb89ec5a72908b5d8760de854897bebefb72951fbe2888",
            USIL_OP_MAD,
            4},
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/mad_sm5.dxbc.b64",
            "0a0a854f267387e7ce7d1bfdcd0b0ac0389c138a3543fbc885ab4efadde89d8b",
            USIL_OP_MAD,
            5},
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/add_position_sm4.dxbc.b64",
            "22c944f0a9a017bea0b84ef6ecc8882b209d6b81d53c62c5000e393620765d7b",
            USIL_OP_ADD,
            4},
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/add_position_sm5.dxbc.b64",
            "3d65ba234f8723a2525d986dd4972ad640f5c8d47f75974e2ff3c572f07ca961",
            USIL_OP_ADD,
            5},
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/mad_position_sm4.dxbc.b64",
            "7916e629f76ef8ac35f240404e48409112dfc42cbfeca4e6346ddb4a030002ae",
            USIL_OP_MAD,
            4},
        {DXBC_MATERIAL_DISCARD_FIXTURE_ROOT "/mad_position_sm5.dxbc.b64",
            "734c93797d9b36234d7f5f681c8f97791231eaa642473e389f5eeaa23783875a",
            USIL_OP_MAD,
            5}};
    for (size_t index = 0; index < sizeof(cases) / sizeof(*cases); ++index) {
        if (!check_case(&cases[index])) {
            return 1;
        }
    }
    return 0;
}
