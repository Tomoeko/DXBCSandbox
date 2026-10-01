// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_quality.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    USILProgram program;
    USILInstruction instructions[2];
    USILConstantBuffer cbuffer;
    DXBCSignatureElement output;
    SerializedVariable fields[2];
    SerializedConstantBuffer buffers[2];
    SerializedResourceParam bindings[2];
    SerializedProgramParameters parameters[2];
    HLSLEmitOptions options;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
} Fixture;

static DXBCOperand operand(DXBCOperandType type, unsigned row, uint8_t lanes) {
    DXBCOperand value = {0};
    value.type = type;
    value.register_index_dim = 1;
    value.index_has_immediate[0] = true;
    value.destination_mask = lanes << 4;
    value.swizzle_mode = 1;
    for (unsigned lane = 0; lane < 4; ++lane)
        value.swizzle[lane] = (uint8_t)lane;
    if (type == OPERAND_TYPE_CONSTANT_BUFFER) {
        value.register_index_dim = 2;
        value.index_has_immediate[1] = true;
        value.index_values[1] = row;
        value.rel_offset0 = (int)row;
    }
    return value;
}

static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof(*f));
    f->instructions[0].opcode = USIL_OP_ADD;
    f->instructions[0].operand_count = 3;
    f->instructions[0].source_instruction_index = 10;
    f->instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    f->instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 0, 0);
    memset(f->instructions[0].operands[1].swizzle, 0,
           sizeof(f->instructions[0].operands[1].swizzle));
    f->instructions[0].operands[2] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 1, 0);
    f->instructions[1].opcode = USIL_OP_RET;
    f->instructions[1].source_instruction_index = 11;
    f->cbuffer = (USILConstantBuffer){.reg_idx = 0, .size = 2};
    f->output = (DXBCSignatureElement){.semantic_name = "SV_Target",
                                       .component_type = 3,
                                       .system_value = 64,
                                       .mask = 15,
                                       .rw_mask = 15};
    f->program = (USILProgram){.instructions = f->instructions,
                               .instruction_count = 2,
                               .instruction_alloc = 2,
                               .outputs = &f->output,
                               .output_count = 1,
                               .output_alloc = 1,
                               .cbuffers = &f->cbuffer,
                               .cbuffer_count = 1,
                               .cbuffer_alloc = 1,
                               .has_stage_contract = true,
                               .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                               .shader_model_major = 5};
    memcpy(f->program.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
    f->fields[0] = (SerializedVariable){"_Amount", {0, 0, 0, 1, 0, 0}};
    f->fields[1] = (SerializedVariable){"_Color", {16, 0, 0, 4, 0, 0}};
    for (unsigned source = 0; source < 2; ++source) {
        f->buffers[source] = (SerializedConstantBuffer){.name = "MaterialInputs",
                                                       .size = 32,
                                                       .role = SERIALIZED_CBUFFER_NAMED,
                                                       .variables = f->fields,
                                                       .var_count = 2};
        f->bindings[source] = (SerializedResourceParam){
            .name = "MaterialInputs", .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER};
        f->parameters[source] = (SerializedProgramParameters){
            .constant_buffers = &f->buffers[source], .cb_count = 1,
            .resources = &f->bindings[source], .res_count = 1};
    }
    f->options = (HLSLEmitOptions)HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    f->options.source_quality = &f->quality;
    f->options.expression_source_map = &f->map;
    f->options.source_quality_pass_index = 3;
    f->options.source_quality_entry_point_index = 2;
}

static bool emit(Fixture *f, StringBuilder *source,
                 const SerializedProgramParameters *stage,
                 const SerializedProgramParameters *common) {
    sb_init(source);
    return hlsl_emit_with_options(&f->program, source, stage, common, NULL, &f->options);
}

typedef struct {
    unsigned sequence;
    unsigned reject_sequence;
    uint8_t authorities[4];
    HLSLSourceQualityFacts fields[2];
} Ledger;

static bool observe_cbuffer(void *context, const HLSLSourceQualityObservation *value) {
    Ledger *ledger = context;
    const HLSLSourceQualityFacts *facts = &value->facts;
    if (facts->cbuffer_field_rows || facts->cbuffer_field_columns || facts->cbuffer_field_is_matrix)
        return false; /* Existing scalar/vector receipts retain zero shape. */
    if (facts->cbuffer_declaration_kind == HLSL_SOURCE_CBUFFER_NONE)
        return true;
    const unsigned sequence = ledger->sequence++;
    if (sequence >= 4 || value->kind != HLSL_SOURCE_OBSERVATION_EMISSION ||
        value->stage != DXBC_PROGRAM_TYPE_PIXEL || value->pass_index != 3 ||
        value->entry_point_index != 2 || value->source_unit_id != 0 ||
        value->unit_kind != HLSL_SOURCE_UNIT_ENTRY_POINT || !facts->known ||
        facts->instruction_index != -1 || facts->source_instruction_index != UINT32_MAX ||
        facts->lanes || facts->artifacts || facts->cbuffer_binding_register != 0)
        return false;
    const HLSLSourceQualityCBufferDeclarationKind expected[] = {
        HLSL_SOURCE_CBUFFER_BEGIN, HLSL_SOURCE_CBUFFER_FIELD,
        HLSL_SOURCE_CBUFFER_FIELD, HLSL_SOURCE_CBUFFER_END};
    if (facts->cbuffer_declaration_kind != expected[sequence])
        return false;
    ledger->authorities[sequence] = facts->cbuffer_declaration_authority;
    if (sequence == 1 || sequence == 2) {
        const unsigned field = sequence - 1;
        if (facts->cbuffer_field_index != field || facts->cbuffer_byte_offset != field * 16u ||
            facts->cbuffer_byte_size != (field ? 16u : 4u))
            return false;
        ledger->fields[field] = *facts;
    } else if (facts->cbuffer_field_index != UINT32_MAX || facts->cbuffer_byte_offset ||
               facts->cbuffer_byte_size != 32u) {
        return false;
    }
    return !ledger->reject_sequence || ledger->reject_sequence != sequence + 1u;
}

static bool check_stage_and_common_authority(void) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        Fixture f;
        fixture_init(&f);
        Ledger ledger = {0};
        f.options.source_quality_observer = observe_cbuffer;
        f.options.source_quality_observer_context = &ledger;
        const SerializedProgramParameters *stage = variant == 1 ? NULL : &f.parameters[0];
        const SerializedProgramParameters *common = variant == 0 ? NULL : &f.parameters[1];
        if (variant == 2) {
            f.buffers[0].var_count = 1;
            f.buffers[1].variables = &f.fields[1];
            f.buffers[1].var_count = 1;
        }
        StringBuilder source;
        CHECK(emit(&f, &source, stage, common));
        CHECK(strstr(source.buf, "cbuffer MaterialInputs : register(b0)") &&
              strstr(source.buf, "float _Amount : packoffset(c0)") &&
              strstr(source.buf, "float4 _Color : packoffset(c1)") &&
              !strstr(source.buf, "_pad") && !strstr(source.buf, "cb0_"));
        CHECK(f.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !f.quality.counts.incomplete_units && !f.quality.counts.unknown_provenance &&
              !f.quality.counts.residual_total && f.quality.counts.cbuffer_declarations == 1 &&
              f.quality.counts.cbuffer_fields == 2 && ledger.sequence == 4);
        CHECK(ledger.authorities[0] == (variant == 1 ? 2 : 1) &&
              ledger.authorities[1] == (variant == 1 ? 2 : 1) &&
              ledger.authorities[2] == (variant == 0 ? 1 : 2) &&
              ledger.authorities[3] == ledger.authorities[0]);
        CHECK(hlsl_expression_source_map_matches(&f.map, &f.program, source.buf));
        CHECK(ledger.fields[0].cbuffer_byte_size == 4 &&
              ledger.fields[1].cbuffer_byte_offset == 16);
        StringBuilder without_quality;
        f.options.source_quality = NULL;
        f.options.source_quality_observer = NULL;
        f.options.source_quality_observer_context = NULL;
        CHECK(emit(&f, &without_quality, stage, common));
        CHECK(source.len == without_quality.len && !memcmp(source.buf, without_quality.buf,
                                                           source.len));
        sb_free(&without_quality);
        sb_free(&source);
    }
    return true;
}

static bool check_observer_rejection(void) {
    for (unsigned reject = 1; reject <= 4; ++reject) {
        Fixture f;
        fixture_init(&f);
        Ledger ledger = {.reject_sequence = reject};
        f.options.source_quality_observer = observe_cbuffer;
        f.options.source_quality_observer_context = &ledger;
        StringBuilder source;
        CHECK(!emit(&f, &source, &f.parameters[0], NULL));
        CHECK(ledger.sequence == reject && f.quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
              f.quality.emission_status == HLSL_EMIT_STATUS_ANALYSIS_FAILED &&
              !f.map.complete && !f.map.count);
        sb_free(&source);
    }
    return true;
}

static bool check_other_stage_scalar_layouts(void) {
    /* The identical byte-zero scalar/16-byte shell in ordinary pixel and
     * vertex emission keeps the existing explicit layout in both modes. */
    for (unsigned vertex = 0; vertex < 2; ++vertex) {
        for (unsigned raw = 0; raw < 2; ++raw) {
            Fixture f;
            fixture_init(&f);
            f.cbuffer.size = 1;
            f.instructions[0].opcode = USIL_OP_MOV;
            f.instructions[0].operand_count = 2;
            for (unsigned authority = 0; authority < 2; ++authority) {
                f.buffers[authority].size = 16;
                f.buffers[authority].var_count = 1;
            }
            if (vertex) {
                f.program.program_type = DXBC_PROGRAM_TYPE_VERTEX;
                memcpy(f.program.shader_type_model, "vs_5_0", sizeof("vs_5_0"));
                memcpy(f.output.semantic_name, "SV_Position", sizeof("SV_Position"));
                f.output.system_value = 1;
            }
            if (raw) {
                f.options.mode = HLSL_EMIT_MODE_RECOMPILE;
                f.options.expression_source_map = NULL;
            }
            StringBuilder source;
            CHECK(emit(&f, &source, &f.parameters[0], NULL));
            CHECK(strstr(source.buf, "cbuffer MaterialInputs : register(b0)") &&
                  strstr(source.buf, "float _Amount : packoffset(c0);") &&
                  !strstr(source.buf, "float _Amount;"));
            if (raw) CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
            else CHECK(hlsl_expression_source_map_matches(&f.map, &f.program, source.buf));
            sb_free(&source);
        }
    }
    return true;
}

static HLSLEmitterContext *layout_context(Fixture *f, StringBuilder *source) {
    HLSLEmitterContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    sb_init(source);
    ctx->program = &f->program;
    ctx->params = &f->parameters[0];
    ctx->sb = source;
    ctx->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    ctx->entry_point_name = "main";
    if (!build_cbuffer_register_map(ctx) || !build_cbuffer_emission_layouts(ctx)) {
        free_cbuffer_emission_layouts(ctx);
        free(ctx->cb_reg_map);
        free(ctx);
        return NULL;
    }
    return ctx;
}

static void layout_context_free(HLSLEmitterContext *ctx, StringBuilder *source) {
    free_cbuffer_emission_layouts(ctx);
    free(ctx->cb_reg_map);
    free(ctx);
    sb_free(source);
}

static bool check_invalid_natural_marker(void) {
    Fixture f;
    fixture_init(&f);
    StringBuilder source;
    HLSLEmitterContext *ctx = layout_context(&f, &source);
    CHECK(ctx && !ctx->cbuffer_layouts[0].natural_hull_scalar_packing);
    /* The normal omitted-declaration route is still a no-op. A malformed
     * natural-layout marker must fail before that skip can hide it. */
    ctx->cbuffer_layouts[0].omit_declaration = true;
    HLSLEmitDiagnostic diagnostic = {0};
    ctx->diagnostic = &diagnostic;
    emit_cbuffers(ctx);
    CHECK(!source.len && diagnostic.status == HLSL_EMIT_STATUS_OK);
    ctx->cbuffer_layouts[0].natural_hull_scalar_packing = true;
    emit_cbuffers(ctx);
    CHECK(!source.len && diagnostic.status == HLSL_EMIT_STATUS_INVALID_METADATA &&
          diagnostic.phase == HLSL_EMIT_PHASE_CBUFFER_EMISSION &&
          diagnostic.reason == HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    layout_context_free(ctx, &source);
    return true;
}

static bool check_actual_syntax_coverage(void) {
    Fixture f;
    fixture_init(&f);
    StringBuilder source;
    HLSLEmitterContext *ctx = layout_context(&f, &source);
    CHECK(ctx && hlsl_source_quality_cbuffer_inventory_supported(ctx) &&
          !hlsl_source_quality_cbuffer_inventory_complete(ctx));
    emit_cbuffers(ctx);
    CHECK(hlsl_source_quality_cbuffer_inventory_complete(ctx));
    HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[0];
    layout->source_quality_begin_emitted = false;
    CHECK(!hlsl_source_quality_cbuffer_inventory_complete(ctx));
    layout->source_quality_begin_emitted = true;
    layout->source_quality_end_emitted = false;
    CHECK(!hlsl_source_quality_cbuffer_inventory_complete(ctx));
    layout->source_quality_end_emitted = true;
    --layout->source_quality_fields_emitted;
    CHECK(!hlsl_source_quality_cbuffer_inventory_complete(ctx));
    ++layout->source_quality_fields_emitted;
    layout->variables[1].authority = 4;
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    layout->variables[1].authority = 1;
    layout->omit_declaration = true;
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    layout->omit_declaration = false;
    layout->is_unity_builtin = true;
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    layout->is_unity_builtin = false;
    layout->raw_storage = true;
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    layout->raw_storage = false;
    f.cbuffer.dynamic_indexed = true;
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    layout_context_free(ctx, &source);
    return true;
}

static bool check_metadata_gaps(void) {
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
        Fixture f;
        fixture_init(&f);
        if (mutation == 0) f.buffers[0].size = 48; /* Unused retained shell tail. */
        if (mutation == 1) f.fields[1].layout[0] = 32; /* Missing middle row. */
        if (mutation == 2) {
            f.instructions[0].opcode = USIL_OP_MOV;
            f.instructions[0].operand_count = 2; /* Unused serialized _Color is filtered. */
        }
        if (mutation == 3) f.buffers[0].size = 0;
        if (mutation == 4) {
            f.buffers[0].has_is_partial = true;
            f.buffers[0].is_partial = true;
        }
        if (mutation == 5) f.fields[0].name = "bad-name";
        if (mutation == 6) f.fields[0].name = "float4";
        if (mutation == 7) f.buffers[0].name = f.bindings[0].name = "bad-buffer";
        if (mutation == 8) f.fields[1].layout[2] = 1; /* Integer field. */
        if (mutation == 9) f.fields[1].layout[1] = 2; /* Array. */
        if (mutation == 10) f.fields[1].layout[4] = 1; /* Matrix. */
        StringBuilder source;
        (void)emit(&f, &source, &f.parameters[0], NULL);
        CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        sb_free(&source);
    }
    Fixture f;
    fixture_init(&f);
    const char *reserved[] = {"_Color"};
    f.options.reserved_preprocessor_identifiers = reserved;
    f.options.reserved_preprocessor_identifier_count = 1;
    StringBuilder source;
    (void)emit(&f, &source, &f.parameters[0], NULL);
    CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    return true;
}

static bool check_resource_namespace_and_invalid_syntax(void) {
    Fixture f;
    fixture_init(&f);
    StringBuilder source;
    HLSLEmitterContext *ctx = layout_context(&f, &source);
    CHECK(ctx && hlsl_source_quality_cbuffer_inventory_supported(ctx));
    SerializedResourceParam resources[2] = {f.bindings[0],
        {.name = "_Color", .bind_type = SERIALIZED_RESOURCE_TEXTURE}};
    USILTexture texture = {.reg_idx = 0, .dimension = "2d", .return_types = {5, 5, 5, 5}};
    f.parameters[0].resources = resources;
    f.parameters[0].res_count = 2;
    f.program.textures = &texture;
    f.program.texture_count = f.program.texture_alloc = 1;
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    resources[1].name = "MaterialInputs";
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    f.program.texture_count = 0;
    USILSampler sampler = {.reg_idx = 0};
    f.program.samplers = &sampler;
    f.program.sampler_count = f.program.sampler_alloc = 1;
    ctx->sampler_names[0] = "_Amount";
    CHECK(!hlsl_source_quality_cbuffer_inventory_supported(ctx));
    f.program.sampler_count = 0;
    CHECK(hlsl_source_quality_cbuffer_inventory_supported(ctx));
    CHECK(!hlsl_source_quality_cbuffer_syntax(NULL, 0, HLSL_SOURCE_CBUFFER_BEGIN, -1, 1));
    CHECK(!hlsl_source_quality_cbuffer_syntax(ctx, -1, HLSL_SOURCE_CBUFFER_BEGIN, -1, 1));
    CHECK(!hlsl_source_quality_cbuffer_syntax(ctx, 1, HLSL_SOURCE_CBUFFER_BEGIN, -1, 1));
    CHECK(!hlsl_source_quality_cbuffer_syntax(ctx, 0, HLSL_SOURCE_CBUFFER_NONE, -1, 1));
    CHECK(!hlsl_source_quality_cbuffer_syntax(ctx, 0, HLSL_SOURCE_CBUFFER_BEGIN, 0, 1));
    CHECK(!hlsl_source_quality_cbuffer_syntax(ctx, 0, HLSL_SOURCE_CBUFFER_FIELD, 2, 1));
    CHECK(!hlsl_source_quality_cbuffer_syntax(ctx, 0, HLSL_SOURCE_CBUFFER_FIELD, 0, 4));
    layout_context_free(ctx, &source);
    return true;
}

static bool check_invalid_observer_facts(void) {
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
        HLSLSourceQualityResult quality;
        HLSLSourceQualityRequest request = {.stage = DXBC_PROGRAM_TYPE_PIXEL};
        HLSLSourceQualityAnalysis *analysis = hlsl_source_quality_analysis_create(&request, &quality);
        CHECK(analysis && hlsl_source_quality_analysis_begin_unit(analysis, 0,
                                                                 HLSL_SOURCE_UNIT_CONFIGURATION, true));
        HLSLSourceQualityFacts facts;
        hlsl_source_quality_facts_init(&facts);
        facts.known = true;
        facts.cbuffer_declaration_kind = HLSL_SOURCE_CBUFFER_FIELD;
        facts.cbuffer_binding_register = 0;
        facts.cbuffer_declaration_authority = 1;
        facts.cbuffer_field_index = 1;
        facts.cbuffer_byte_offset = 16;
        facts.cbuffer_byte_size = 16;
        if (mutation == 0) facts.cbuffer_field_index = 0;
        if (mutation == 1) facts.cbuffer_byte_offset = 17;
        if (mutation == 2) facts.cbuffer_byte_size = 17;
        if (mutation == 3) facts.cbuffer_byte_size = 0;
        if (mutation == 4) facts.cbuffer_binding_register = 15;
        if (mutation == 5) facts.cbuffer_declaration_authority = 4;
        if (mutation == 6) facts.instruction_index = 0;
        if (mutation == 7) facts.source_instruction_index = 0;
        if (mutation == 8) facts.resource_declaration_kind = HLSL_SOURCE_RESOURCE_TEXTURE;
        if (mutation == 9) facts.cbuffer_byte_offset = UINT32_MAX;
        if (mutation == 10) facts.cbuffer_declaration_kind = HLSL_SOURCE_CBUFFER_END;
        if (mutation == 11) facts.known = false;
        CHECK(!hlsl_source_quality_analysis_emission(analysis, &facts));
        CHECK(!hlsl_source_quality_analysis_finish(analysis, HLSL_EMIT_STATUS_OK, 1));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
              !quality.counts.cbuffer_declarations && !quality.counts.cbuffer_fields);
        hlsl_source_quality_analysis_destroy(analysis);
    }
    return true;
}

static bool check_matrix_receipt_requires_guard(void) {
    Fixture f; fixture_init(&f);
    f.cbuffer.size = 4;
    f.fields[0] = (SerializedVariable){"ObjectTransform", {0, 0, 0, 4, 1, 0}};
    f.buffers[0].size = 64; f.buffers[0].var_count = 1;
    /* This test owns only the local layout predicate. Its hand-authored
     * program does not acquire parsed authority or a ready whole-source guard. */
    StringBuilder source; HLSLEmitterContext *ctx = layout_context(&f, &source);
    CHECK(ctx && ctx->cbuffer_layouts[0].variable_count == 1 &&
        ctx->cbuffer_layouts[0].variables[0].is_matrix &&
        ctx->cbuffer_layouts[0].variables[0].byte_size == 64 &&
        !hlsl_source_quality_packed_output_guard_active(ctx) &&
        !hlsl_source_quality_named_cbuffer_supported(ctx, 0, NULL) &&
        !hlsl_source_quality_cbuffer_inventory_supported(ctx));
    ctx->high_level_packed_outputs = true; /* A nominal flag cannot supply the private lease. */
    CHECK(!hlsl_source_quality_packed_output_guard_active(ctx) &&
        !hlsl_source_quality_named_cbuffer_supported(ctx, 0, NULL));
    layout_context_free(ctx, &source);
    return true;
}

static bool check_raw_mode_remains_incomplete(void) {
    Fixture f;
    fixture_init(&f);
    f.options.mode = HLSL_EMIT_MODE_RECOMPILE;
    f.options.expression_source_map = NULL;
    StringBuilder source;
    CHECK(emit(&f, &source, &f.parameters[0], NULL));
    CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
          !f.quality.counts.cbuffer_declarations && !f.quality.counts.cbuffer_fields);
    StringBuilder default_source;
    sb_init(&default_source);
    CHECK(hlsl_emit(&f.program, &default_source, &f.parameters[0], NULL, NULL));
    CHECK(source.len == default_source.len && !memcmp(source.buf, default_source.buf, source.len));
    sb_free(&default_source);
    sb_free(&source);
    return true;
}

int main(void) {
    const size_t allocations = g_allocations_count;
    const size_t bytes = g_allocated_bytes;
    if (!check_stage_and_common_authority() || !check_observer_rejection() ||
        !check_other_stage_scalar_layouts() || !check_invalid_natural_marker() ||
        !check_actual_syntax_coverage() || !check_metadata_gaps() ||
        !check_resource_namespace_and_invalid_syntax() || !check_invalid_observer_facts() ||
        !check_matrix_receipt_requires_guard() ||
        !check_raw_mode_remains_incomplete())
        return 1;
    if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
        fprintf(stderr, "Named cbuffer source-quality tests leaked tracked allocations.\n");
        return 1;
    }
    puts("Named cbuffer source-quality tests passed.");
    return 0;
}
