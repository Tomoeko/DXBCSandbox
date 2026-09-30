// SPDX-License-Identifier: GPL-3.0-only
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                   \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    USILProgram program;
    USILInstruction instructions[8];
    DXBCSignatureElement inputs[2], outputs[2];
    HLSLEmitOptions options;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
} Fixture;

static DXBCOperand operand(DXBCOperandType type, int reg, uint8_t lanes) {
    DXBCOperand value = {0};
    value.type = type;
    value.register_index = reg;
    value.register_index_dim = 1;
    value.index_has_immediate[0] = true;
    value.index_values[0] = (uint32_t)reg;
    value.destination_mask = lanes << 4;
    value.swizzle_mode = 1;
    for (unsigned lane = 0; lane < 4; ++lane)
        value.swizzle[lane] = (uint8_t)lane;
    return value;
}

static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof(*f));
    f->inputs[0] = (DXBCSignatureElement){
        .semantic_name = "POSITION", .component_type = 3, .mask = 15, .rw_mask = 15};
    f->inputs[1] = (DXBCSignatureElement){.semantic_name = "TEXCOORD",
                                          .register_id = 1,
                                          .component_type = 3,
                                          .mask = 3,
                                          .rw_mask = 3};
    f->outputs[0] = (DXBCSignatureElement){
        .semantic_name = "TEXCOORD", .component_type = 3, .mask = 3, .rw_mask = 3};
    f->outputs[1] = (DXBCSignatureElement){.semantic_name = "SV_POSITION",
                                           .register_id = 1,
                                           .component_type = 3,
                                           .mask = 15,
                                           .rw_mask = 15,
                                           .system_value = 1};
    for (unsigned i = 0; i < 2; ++i) {
        f->instructions[i].opcode = USIL_OP_MOV;
        f->instructions[i].operand_count = 2;
        f->instructions[i].operands[0] = operand(OPERAND_TYPE_OUTPUT, (int)i, i ? 15 : 3);
        f->instructions[i].operands[1] = operand(OPERAND_TYPE_INPUT, i ? 0 : 1, 0);
        f->instructions[i].source_instruction_index = i + 10;
    }
    f->instructions[2].opcode = USIL_OP_RET;
    f->instructions[2].source_instruction_index = 12;
    f->program = (USILProgram){.instructions = f->instructions,
                               .instruction_count = 3,
                               .instruction_alloc = 4,
                               .inputs = f->inputs,
                               .input_count = 2,
                               .input_alloc = 2,
                               .outputs = f->outputs,
                               .output_count = 2,
                               .output_alloc = 2,
                               .has_stage_contract = true,
                               .program_type = DXBC_PROGRAM_TYPE_VERTEX,
                               .shader_model_major = 5};
    memcpy(f->program.shader_type_model, "vs_5_0", sizeof("vs_5_0"));
    f->options = (HLSLEmitOptions)HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    f->options.source_quality = &f->quality;
    f->options.expression_source_map = &f->map;
}

static bool emit(Fixture *f, StringBuilder *source) {
    sb_init(source);
    return hlsl_emit_with_options(&f->program, source, NULL, NULL, NULL, &f->options);
}

static bool check_named_fields_and_inputs(void) {
    Fixture f;
    fixture_init(&f);
    StringBuilder source;
    CHECK(emit(&f, &source));
    CHECK(strstr(source.buf, "struct v2f {") &&
          strstr(source.buf, "float2 texcoord0_1 : TEXCOORD0;") &&
          strstr(source.buf, "float4 clipPosition : SV_POSITION;"));
    CHECK(
        strstr(source.buf, "v2f main(float4 position : POSITION, float2 texcoord0 : TEXCOORD0)") &&
        strstr(source.buf, "output.texcoord0_1 = (texcoord0);") &&
        strstr(source.buf, "output.clipPosition = (position);") &&
        strstr(source.buf, "return output;"));
    CHECK(!strstr(source.buf, " v0") && !strstr(source.buf, " o0") &&
          !strstr(source.buf, "appdata"));
    CHECK(hlsl_expression_source_map_matches(&f.map, &f.program, source.buf));
    CHECK(f.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          f.quality.emission_status == HLSL_EMIT_STATUS_OK &&
          f.quality.counts.incomplete_units == 0 && !f.quality.counts.unknown_provenance &&
          !f.quality.counts.residual_total);
    sb_free(&source);
    const char *reserved[] = {"v2f", "output", "clipPosition", "texcoord0_1"};
    f.options.reserved_preprocessor_identifiers = reserved;
    f.options.reserved_preprocessor_identifier_count = 4;
    CHECK(emit(&f, &source));
    CHECK(strstr(source.buf, "struct v2f_1 {") && strstr(source.buf, "v2f_1 output_1;") &&
          strstr(source.buf, "output_1.clipPosition_1") &&
          strstr(source.buf, "output_1.texcoord0_2"));
    sb_free(&source);
    return true;
}

static bool check_mrt_fields(void) {
    Fixture f;
    fixture_init(&f);
    f.program.program_type = DXBC_PROGRAM_TYPE_PIXEL;
    memcpy(f.program.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(f.outputs[i].semantic_name, "SV_Target", sizeof("SV_Target"));
        f.outputs[i].system_value = 64;
        f.outputs[i].semantic_index = i;
        f.outputs[i].mask = f.outputs[i].rw_mask = 15;
        f.instructions[i].operands[0] = operand(OPERAND_TYPE_OUTPUT, (int)i, 15);
        f.instructions[i].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
    }
    StringBuilder source;
    CHECK(emit(&f, &source));
    CHECK(strstr(source.buf, "float4 color : SV_Target;") &&
          strstr(source.buf, "float4 color1 : SV_Target1;") &&
          strstr(source.buf, "output.color = (position);") &&
          strstr(source.buf, "output.color1 = (position);"));
    CHECK(f.quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    return true;
}

static bool check_unsupported_transport(void) {
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        Fixture f;
        fixture_init(&f);
        if (mutation == 0)
            f.instructions[0].operands[0].destination_mask = 0x10;
        if (mutation == 1) {
            f.outputs[1].register_id = 0;
            f.outputs[1].mask = f.outputs[1].rw_mask = 12;
            f.outputs[1].system_value = 0;
            f.outputs[1].semantic_index = 1;
            memcpy(f.outputs[1].semantic_name, "TEXCOORD", sizeof("TEXCOORD"));
            f.instructions[1].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 12);
        }
        if (mutation == 2) {
            f.instructions[2] = f.instructions[0];
            f.instructions[2].source_instruction_index = 12;
            f.instructions[3].opcode = USIL_OP_RET;
            f.instructions[3].source_instruction_index = 13;
            f.program.instruction_count = 4;
        }
        if (mutation == 3)
            f.instructions[1].operands[1] = operand(OPERAND_TYPE_OUTPUT, 0, 0);
        if (mutation == 4)
            f.inputs[1].mask = f.inputs[1].rw_mask = 6;
        StringBuilder source;
        CHECK(!emit(&f, &source));
        CHECK(!f.map.complete && f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
        sb_free(&source);
    }
    return true;
}

typedef struct {
    const USILProgram *program;
    uint32_t expression_owners;
    uint32_t statement_owners;
    uint32_t input_owners;
    size_t syntax_records;
    size_t observations;
    bool reject_struct_field;
    HLSLSourceQualityObservation copied;
} InterfaceLedger;

static bool observe_interface(void *context, const HLSLSourceQualityObservation *value) {
    InterfaceLedger *ledger = context;
    ++ledger->observations;
    if (value->stage != ledger->program->program_type || value->pass_index != 6 ||
        value->entry_point_index != 2 || value->source_unit_id != 0 ||
        value->unit_kind != HLSL_SOURCE_UNIT_ENTRY_POINT)
        return false;
    int owner = value->facts.instruction_index;
    if (owner >= 0) {
        if (owner >= ledger->program->instruction_count ||
            value->facts.source_instruction_index !=
                ledger->program->instructions[owner].source_instruction_index)
            return false;
        if (value->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION) {
            ledger->expression_owners |= UINT32_C(1) << owner;
            if (value->ast_kind == AST_EXPR_EMITTER_OPERAND && value->facts.known &&
                value->facts.value_kind == HLSL_SOURCE_VALUE_LOGICAL)
                ledger->input_owners |= UINT32_C(1) << owner;
        } else if (value->kind == HLSL_SOURCE_OBSERVATION_EMISSION) {
            ledger->statement_owners |= UINT32_C(1) << owner;
        }
    } else if (value->kind == HLSL_SOURCE_OBSERVATION_EMISSION && value->facts.known) {
        ledger->copied = *value;
        ++ledger->syntax_records;
        /* In this resource-free fixture the comment is followed by the first
         * actual typed result field. Rejecting that record must fail emission. */
        if (ledger->reject_struct_field && ledger->syntax_records == 2)
            return false;
    }
    return true;
}

static bool check_observer_ownership_and_transport(void) {
    Fixture f;
    fixture_init(&f);
    InterfaceLedger ledger = {.program = &f.program};
    f.options.source_quality_pass_index = 6;
    f.options.source_quality_entry_point_index = 2;
    f.options.source_quality_observer = observe_interface;
    f.options.source_quality_observer_context = &ledger;
    StringBuilder source;
    CHECK(emit(&f, &source));
    CHECK(f.quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !f.quality.reasons);
    CHECK(ledger.expression_owners == 3 && ledger.input_owners == 3 &&
          ledger.statement_owners == 7 && ledger.syntax_records == 8);
    CHECK(ledger.observations == f.quality.counts.ast_expressions +
                                     f.quality.counts.emission_events +
                                     f.quality.counts.incomplete_units);
    CHECK(ledger.copied.facts.instruction_index == -1 &&
          ledger.copied.facts.source_instruction_index == UINT32_MAX && ledger.copied.facts.known &&
          !ledger.copied.facts.artifacts);
    sb_free(&source);

    /* Actual declaration-omission policy alone creates no hidden dependency. */
    f.options.omit_unity_builtin_declarations = true;
    memset(&ledger, 0, sizeof(ledger));
    ledger.program = &f.program;
    CHECK(emit(&f, &source));
    CHECK(f.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          !f.quality.counts.incomplete_units);
    sb_free(&source);

    f.instructions[0].operands[1].swizzle[0] = 1;
    f.instructions[0].operands[1].swizzle[1] = 0;
    memset(&ledger, 0, sizeof(ledger));
    ledger.program = &f.program;
    CHECK(emit(&f, &source));
    CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
          f.quality.counts.lane_transport == 1 && !f.quality.counts.incomplete_units &&
          !f.quality.counts.unknown_provenance);
    CHECK(hlsl_expression_source_map_matches(&f.map, &f.program, source.buf));
    sb_free(&source);

    f.instructions[0].operands[1].swizzle[0] = 0;
    f.instructions[0].operands[1].swizzle[1] = 1;
    memset(&ledger, 0, sizeof(ledger));
    ledger.program = &f.program;
    ledger.reject_struct_field = true;
    CHECK(!emit(&f, &source));
    CHECK(f.quality.classification == HLSL_SOURCE_QUALITY_FAILED && !f.map.complete);
    sb_free(&source);
    CHECK(ledger.copied.facts.known && ledger.copied.source_unit_id == 0);
    return true;
}

static bool emit_inventory_statement(HLSLEmitterContext *ctx, int index) {
    const USILInstruction *instruction = &ctx->program->instructions[index];
    ctx->current_instruction_index = index;
    sb_append(ctx->sb, "    ");
    CHECK(hlsl_append_high_level_output(ctx, &instruction->operands[0]));
    sb_append(ctx->sb, " = ");
    StringBuilder operand_source;
    sb_init(&operand_source);
    uint8_t lanes = usil_operand_destination_lane_mask(&instruction->operands[0]);
    CHECK(format_operand_hlsl_sb(ctx, &instruction->operands[1], false, false, lanes << 4, false,
                                 &operand_source));
    ASTOperandProvenance provenance;
    CHECK(hlsl_high_level_input_provenance(ctx, &instruction->operands[1], lanes, &provenance));
    ASTExpr *expression =
        ast_create_emitter_operand_with_provenance(operand_source.buf, &provenance);
    sb_free(&operand_source);
    CHECK(expression && hlsl_source_quality_observe_expression(ctx, expression, index));
    ast_format_expr(expression, ctx->sb);
    ast_free_expr(expression);
    sb_append(ctx->sb, ";\n");
    hlsl_source_quality_emission(ctx, 0, false, index);
    return sb_ok(ctx->sb);
}

static bool check_missing_actual_interface_spans(void) {
    /* Exercise the independent inventory through the actual syntax emitters.
     * Planning names alone cannot attest a struct, parameter, write or return
     * whose required span/event was omitted. No fake include unit is added. */
    for (int omission = -1; omission < 8; ++omission) {
        Fixture f;
        fixture_init(&f);
        StringBuilder source;
        sb_init(&source);
        HLSLEmitterContext *ctx = calloc(1, sizeof(*ctx));
        CHECK(ctx);
        ctx->program = &f.program;
        ctx->sb = &source;
        ctx->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
        ctx->high_level_interface = true;
        ctx->is_vertex = true;
        ctx->entry_point_name = "main";
        ctx->preferred_output_struct_name = "v2f";
        CHECK(hlsl_prepare_high_level_interface(ctx));
        CHECK(hlsl_source_quality_interface_inventory_supported(ctx));
        CHECK(!hlsl_source_quality_interface_inventory_complete(ctx));
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.source_quality = &f.quality;
        CHECK(hlsl_source_quality_initialize(ctx, &options));
        CHECK(hlsl_source_quality_begin_entry(ctx, true));
        bool used[HLSL_SM5_IO_REGISTER_COUNT] = {false};
        if (omission != 0)
            emit_io_structs(ctx, "appdata", "v2f");
        if (omission != 1)
            emit_entry_point_declarations(ctx, "main", "appdata", "v2f", used, used);
        if (omission != 2)
            CHECK(emit_inventory_statement(ctx, 0));
        CHECK(emit_inventory_statement(ctx, 1));
        if (omission != 3)
            emit_return_block(ctx);
        if (omission == 4)
            ctx->high_level_input_parameters_emitted &= ~UINT32_C(2);
        if (omission == 5)
            ctx->high_level_output_fields_emitted &= ~UINT32_C(2);
        if (omission == 6)
            ctx->high_level_result_local_emitted = false;
        if (omission == 7)
            ctx->high_level_interface_prepared = false;
        CHECK(hlsl_source_quality_interface_inventory_complete(ctx) == (omission == -1));
        hlsl_source_quality_finish_emission(ctx);
        CHECK(f.quality.emission_status == HLSL_EMIT_STATUS_OK &&
              f.quality.counts.inspected_units == 1 && !f.quality.counts.unknown_provenance &&
              !f.quality.counts.residual_total);
        if (omission == -1) {
            CHECK(f.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                  !f.quality.counts.incomplete_units);
        } else {
            CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
                  f.quality.counts.incomplete_units == 1 && f.quality.has_first_issue &&
                  f.quality.first_issue.kind == HLSL_SOURCE_OBSERVATION_COVERAGE &&
                  f.quality.first_issue.unit_kind == HLSL_SOURCE_UNIT_ENTRY_POINT &&
                  f.quality.first_issue.source_unit_id == 0);
        }
        sb_free(&source);
        free(ctx);
    }
    return true;
}

static bool check_builtin_and_helper_dependencies(void) {
    Fixture f;
    fixture_init(&f);
    USILConstantBuffer cbuffer = {.reg_idx = 0, .size = 9};
    SerializedVariable variable = {"unity_LODFade", {128, 0, 0, 4, 0, 0}};
    SerializedConstantBuffer buffer = {
        .name = "UnityPerDraw", .size = 144, .variables = &variable, .var_count = 1};
    SerializedResourceParam binding = {.name = "UnityPerDraw",
                                       .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER};
    SerializedProgramParameters parameters = {
        .constant_buffers = &buffer, .cb_count = 1, .resources = &binding, .res_count = 1};
    f.program.cbuffers = &cbuffer;
    f.program.cbuffer_count = f.program.cbuffer_alloc = 1;
    f.instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 0, 0);
    f.instructions[0].operands[1].register_index_dim = 2;
    f.instructions[0].operands[1].index_has_immediate[1] = true;
    f.instructions[0].operands[1].index_values[1] = 8;
    f.instructions[0].operands[1].rel_offset0 = 8;
    f.options.omit_unity_builtin_declarations = true;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&f.program, &source, &parameters, NULL, NULL, &f.options));
    CHECK(strstr(source.buf, "unity_LODFade.xy") && !strstr(source.buf, "cbuffer UnityPerDraw"));
    CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
          f.quality.emission_status == HLSL_EMIT_STATUS_OK &&
          f.quality.counts.incomplete_units == 1);
    sb_free(&source);

    fixture_init(&f);
    f.inputs[1].mask = f.inputs[1].rw_mask = 15;
    f.outputs[0].mask = f.outputs[0].rw_mask = 15;
    memset(f.instructions, 0, sizeof(f.instructions));
    for (int chain = 0; chain < 2; ++chain) {
        USILInstruction *first = &f.instructions[chain * 2], *second = first + 1;
        first->opcode = second->opcode = USIL_OP_MUL;
        first->operand_count = second->operand_count = 3;
        first->operands[0] = second->operands[0] = operand(OPERAND_TYPE_TEMP, chain, 15);
        first->operands[1] = operand(OPERAND_TYPE_INPUT, chain, 0);
        first->operands[2] = operand(OPERAND_TYPE_INPUT, 1 - chain, 0);
        second->operands[1] = operand(OPERAND_TYPE_TEMP, chain, 0);
        second->operands[2] = operand(OPERAND_TYPE_INPUT, chain, 0);
        f.instructions[4 + chain].opcode = USIL_OP_MOV;
        f.instructions[4 + chain].operand_count = 2;
        f.instructions[4 + chain].operands[0] = operand(OPERAND_TYPE_OUTPUT, chain, 15);
        f.instructions[4 + chain].operands[1] = operand(OPERAND_TYPE_TEMP, chain, 0);
    }
    f.instructions[6].opcode = USIL_OP_RET;
    f.program.instruction_count = 7;
    f.program.instruction_alloc = 8;
    f.program.temp_count = 2;
    CHECK(emit(&f, &source));
    CHECK(strstr(source.buf, "float4 dxbc_mul_chain_") && strstr(source.buf, "struct v2f"));
    CHECK(f.quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
          f.quality.emission_status == HLSL_EMIT_STATUS_OK &&
          f.quality.counts.incomplete_units == 1 && f.quality.counts.unknown_provenance);
    CHECK(hlsl_expression_source_map_matches(&f.map, &f.program, source.buf));
    sb_free(&source);
    return true;
}

int main(void) {
    if (!check_named_fields_and_inputs() || !check_mrt_fields() || !check_unsupported_transport() ||
        !check_observer_ownership_and_transport() || !check_missing_actual_interface_spans() ||
        !check_builtin_and_helper_dependencies())
        return 1;
    puts("Natural multiple-output stage interfaces passed");
    return 0;
}
