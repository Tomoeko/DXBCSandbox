// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_matrix_lift.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

typedef struct {
    USILProgram program;
    USILInstruction instructions[10];
    USILConstantBuffer buffers[2];
    DXBCSignatureElement input;
    DXBCSignatureElement output;
    TempVariable matrices[2];
    StringBuilder source;
    HLSLEmitterContext context;
} MatrixFixture;

static DXBCOperand register_operand(DXBCOperandType type, int reg) {
    DXBCOperand value = {0};
    value.type = type;
    value.register_index = reg;
    value.register_index_dim = 1;
    value.index_has_immediate[0] = true;
    value.index_values[0] = (uint32_t)reg;
    value.swizzle_mode = 1;
    for (int lane = 0; lane < 4; ++lane) value.swizzle[lane] = (uint8_t)lane;
    return value;
}

static DXBCOperand destination(DXBCOperandType type, int reg) {
    DXBCOperand value = register_operand(type, reg);
    value.destination_mask = 0xf0;
    return value;
}

static DXBCOperand scalar(DXBCOperandType type, int reg, int lane) {
    assert(lane >= 0 && lane < 4);
    DXBCOperand value = register_operand(type, reg);
    for (int component = 0; component < 4; ++component)
        value.swizzle[component] = (uint8_t)lane;
    return value;
}

static DXBCOperand row(int buffer, int index) {
    DXBCOperand value = register_operand(OPERAND_TYPE_CONSTANT_BUFFER, buffer);
    value.register_index_dim = 2;
    value.rel_offset0 = index;
    value.index_has_immediate[1] = true;
    value.index_values[1] = (uint32_t)index;
    return value;
}

static void initialize(MatrixFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const int rows[4] = {1, 0, 2, 3};
    const int components[4] = {1, 0, 2, 3};
    for (int index = 0; index < 8; ++index) {
        USILInstruction *instruction = &fixture->instructions[index];
        const int offset = index & 3;
        const bool clip = index >= 4;
        instruction->source_instruction_index = (uint32_t)(100 + index);
        instruction->opcode = offset == 0 ? USIL_OP_MUL :
            (!clip && offset == 3 ? USIL_OP_ADD : USIL_OP_MAD);
        instruction->operand_count = instruction->opcode == USIL_OP_MAD ? 4 : 3;
        instruction->operands[0] = destination(index == 7 ? OPERAND_TYPE_OUTPUT : OPERAND_TYPE_TEMP,
                                              index == 7 ? 0 : clip ? 1 : 0);
        if (offset == 0) {
            instruction->operands[1] = scalar(clip ? OPERAND_TYPE_TEMP : OPERAND_TYPE_INPUT,
                                               0, components[offset]);
            instruction->operands[2] = row(clip ? 3 : 2, (clip ? 17 : 4) + rows[offset]);
        } else if (!clip && offset == 3) {
            instruction->operands[1] = register_operand(OPERAND_TYPE_TEMP, 0);
            instruction->operands[2] = row(2, 7);
        } else {
            instruction->operands[1] = row(clip ? 3 : 2, (clip ? 17 : 4) + rows[offset]);
            instruction->operands[2] = scalar(clip ? OPERAND_TYPE_TEMP : OPERAND_TYPE_INPUT,
                                               0, components[offset]);
            instruction->operands[3] = register_operand(OPERAND_TYPE_TEMP, clip ? 1 : 0);
        }
    }
    fixture->instructions[8].opcode = USIL_OP_RET;
    fixture->instructions[8].source_instruction_index = 108;
    fixture->input = (DXBCSignatureElement){.semantic_name = "POSITION", .mask = 15,
        .rw_mask = 15, .component_type = 3};
    fixture->output = (DXBCSignatureElement){.semantic_name = "SV_Position", .mask = 15,
        .component_type = 3, .system_value = 1};
    fixture->buffers[0] = (USILConstantBuffer){.reg_idx = 2, .size = 8};
    fixture->buffers[1] = (USILConstantBuffer){.reg_idx = 3, .size = 21};
    fixture->program = (USILProgram){.instructions = fixture->instructions,
        .instruction_count = 9, .instruction_alloc = 10, .temp_count = 2,
        .inputs = &fixture->input, .input_count = 1, .input_alloc = 1,
        .outputs = &fixture->output, .output_count = 1, .output_alloc = 1,
        .cbuffers = fixture->buffers, .cbuffer_count = 2, .cbuffer_alloc = 2,
        .has_stage_contract = true, .program_type = DXBC_PROGRAM_TYPE_VERTEX,
        .shader_model_major = 5};
    memcpy(fixture->program.shader_type_model, "vs_5_0", sizeof("vs_5_0"));
    fixture->context.program = &fixture->program;
    sb_init(&fixture->source);
    fixture->context.sb = &fixture->source;
    fixture->context.high_level_direct_return = true;
    fixture->context.high_level_interface = true;
    fixture->context.current_instruction_index = -1;
    fixture->context.entry_point_name = "vert";
    fixture->context.cbuffer_layouts_built = true;
    fixture->context.cbuffer_layout_count = 2;
    for (int index = 0; index < 2; ++index) {
        fixture->matrices[index] = (TempVariable){.name = index ? "ClipTransform" : "ObjectTransform",
            .is_matrix = 1, .rows = 4, .dim = 4, .reg_offset = index ? 17 : 4,
            .byte_offset = index ? 272 : 64, .byte_size = 64, .authority = 1};
        fixture->context.cbuffer_layouts[index] = (HLSLCBufferLayout){
            .reg = index ? 3 : 2, .row_count = index ? 21 : 8,
            .has_serialized_authority = true, .variable_count = 1,
            .variables = &fixture->matrices[index]};
    }
}

static bool analyze(MatrixFixture *fixture) {
    CHECK(build_control_flow_graph(&fixture->context));
    CHECK(compute_dominance(&fixture->context.cfg));
    CHECK(build_hlsl_ssa_graph(&fixture->context));
    CHECK(build_hlsl_use_def_graph(&fixture->context));
    CHECK(hlsl_prepare_high_level_interface(&fixture->context));
    return true;
}

static void release(MatrixFixture *fixture) {
    free_hlsl_use_def_graph(&fixture->context);
    free_hlsl_ssa_graph(&fixture->context);
    free_control_flow_graph(&fixture->context);
    sb_free(&fixture->source);
}

static bool check_graph_and_metadata(void) {
    MatrixFixture fixture;
    initialize(&fixture);
    CHECK(analyze(&fixture));
    HLSLMatrixVectorChain chain;
    CHECK(hlsl_compiler_matrix_vector_chain_matches(&fixture.program, 0, &chain));
    CHECK(chain.world_matrix_buffer == 2 && chain.world_matrix_first_row == 4);
    CHECK(chain.clip_matrix_buffer == 3 && chain.clip_matrix_first_row == 17);
    HLSLMatrixLiftPlan plan;
    CHECK(hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    CHECK(plan.start_instruction == 0 && plan.end_instruction == 7);
    CHECK(plan.instruction_owners == UINT64_C(255));
    CHECK(plan.expression->logical_origin.complete && plan.world_expression->logical_origin.complete);
    CHECK(plan.world_expression->logical_origin.instruction_index == 3);
    CHECK(plan.expression->logical_origin.instruction_index == 7);
    StringBuilder source;
    sb_init(&source);
    ast_format_expr(plan.expression, &source);
    CHECK(sb_ok(&source));
    CHECK(strstr(source.buf, "mul((ClipTransform), mul((ObjectTransform), float4((position.xyz), 1"));
    CHECK(!strstr(source.buf, "get_cb") && !strstr(source.buf, "transpose") && !strstr(source.buf, "r0"));
    sb_free(&source);
    hlsl_matrix_lift_plan_free(&plan);
    CHECK(!plan.expression && !plan.world_expression && !plan.instruction_owners);
    /* Row-major storage represents a row vector multiplied by that matrix;
     * it cannot silently be relabeled as the column-major expression. */
    fixture.matrices[0].row_major = true;
    CHECK(hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    sb_init(&source);
    ast_format_expr(plan.expression, &source);
    CHECK(strstr(source.buf, "mul(float4((position.xyz), 1") && strstr(source.buf, "), (ObjectTransform))"));
    sb_free(&source);
    hlsl_matrix_lift_plan_free(&plan);
    release(&fixture);
    return true;
}

static bool check_fail_closed_mutations(void) {
    for (int mutation = 0; mutation < 17; ++mutation) {
        MatrixFixture fixture;
        initialize(&fixture);
        CHECK(analyze(&fixture));
        if (mutation == 0) {
            fixture.instructions[2].operands[1].rel_offset0 = 7;
            fixture.instructions[2].operands[1].index_values[1] = 7;
        }
        if (mutation == 1) fixture.instructions[5].operands[2].swizzle[0] = 1;
        if (mutation == 2) fixture.instructions[4].operands[1].has_neg = true;
        if (mutation == 3) fixture.instructions[1].precise_mask = 1;
        if (mutation == 4) fixture.instructions[3].saturate = true;
        if (mutation == 5) fixture.instructions[6].operands[3].extended_token_count = 1;
        if (mutation == 6) fixture.instructions[7].operands[0].destination_mask = 0x70;
        if (mutation == 7) fixture.matrices[0].type = 1;
        if (mutation == 8) fixture.matrices[0].matrix_array_size = 2;
        if (mutation == 9) fixture.matrices[0].rows = 3;
        if (mutation == 10) fixture.matrices[0].byte_size = 80;
        if (mutation == 11) fixture.matrices[0].authority = 3;
        if (mutation == 12) fixture.context.cbuffer_layouts[0].raw_storage = true;
        if (mutation == 13) fixture.context.cbuffer_layouts[0].has_serialized_authority = false;
        if (mutation == 14) fixture.matrices[1].name = fixture.matrices[0].name;
        if (mutation == 15) fixture.buffers[0].dynamic_indexed = true;
        if (mutation == 16) fixture.instructions[0].opcode = USIL_OP_NOP;
        HLSLMatrixLiftPlan plan;
        memset(&plan, 0xff, sizeof(plan));
        CHECK(!hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
        CHECK(!plan.expression && !plan.world_expression && !plan.instruction_owners);
        release(&fixture);
    }
    return true;
}

static bool check_dataflow_ownership(void) {
    MatrixFixture fixture;
    initialize(&fixture);
    CHECK(analyze(&fixture));
    HLSLMatrixLiftPlan plan;
    /* More uses and conflicting SSA edges cannot be hidden inside mul. */
    ++fixture.context.use_def.definition_use_counts[3 * 4];
    CHECK(!hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    --fixture.context.use_def.definition_use_counts[3 * 4];
    const size_t offset = ((size_t)5 * DXBC_MAX_OPERANDS + 2) * 4;
    const int variable = fixture.context.ssa.operand_ssa_vars[offset];
    CHECK(variable >= 0);
    fixture.context.ssa.ssa_var_defs[variable] = 2;
    CHECK(!hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    release(&fixture);
    initialize(&fixture);
    fixture.instructions[9] = fixture.instructions[8];
    fixture.instructions[8] = (USILInstruction){.opcode = USIL_OP_MOV, .operand_count = 2};
    fixture.instructions[8].operands[0] = destination(OPERAND_TYPE_TEMP, 1);
    fixture.instructions[8].operands[1] = register_operand(OPERAND_TYPE_TEMP, 0);
    fixture.program.instruction_count = 10;
    HLSLMatrixVectorChain chain;
    CHECK(!hlsl_compiler_matrix_vector_chain_matches(&fixture.program, 0, &chain));
    initialize(&fixture);
    CHECK(analyze(&fixture));
    int owners[9];
    for (int index = 0; index < 9; ++index) owners[index] = -1;
    owners[2] = 0;
    fixture.context.compiler_model.claim_owner = owners;
    CHECK(!hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    fixture.context.compiler_model.claim_owner = NULL;
    release(&fixture);
    return true;
}

static bool check_invalid_metadata_context(void) {
    MatrixFixture fixture;
    initialize(&fixture);
    bool row_major = false;
    CHECK(!hlsl_matrix_lift_identifier(NULL, 2, 4, &row_major));
    fixture.context.program = NULL;
    CHECK(!hlsl_matrix_lift_identifier(&fixture.context, 2, 4, &row_major));
    fixture.context.program = &fixture.program;
    fixture.context.cbuffer_layout_count = HLSL_MAX_CBUFFER_LAYOUTS + 1;
    CHECK(!hlsl_matrix_lift_identifier(&fixture.context, 2, 4, &row_major));
    fixture.context.cbuffer_layout_count = 2;
    fixture.context.cbuffer_layouts[1].variables = NULL;
    CHECK(!hlsl_matrix_lift_identifier(&fixture.context, 2, 4, &row_major));
    fixture.context.cbuffer_layouts[1].variables = &fixture.matrices[1];
    fixture.program.cbuffers = NULL;
    CHECK(!hlsl_matrix_lift_identifier(&fixture.context, 2, 4, &row_major));
    fixture.program.cbuffers = fixture.buffers;
    CHECK(hlsl_matrix_lift_identifier(&fixture.context, 2, 4, &row_major));
    release(&fixture);
    return true;
}

static void initialize_single(MatrixFixture *fixture) {
    initialize(fixture);
    fixture->instructions[3].opcode = USIL_OP_MAD;
    fixture->instructions[3].operand_count = 4;
    fixture->instructions[3].operands[0] = destination(OPERAND_TYPE_OUTPUT, 0);
    fixture->instructions[3].operands[1] = row(2, 7);
    fixture->instructions[3].operands[2] = scalar(OPERAND_TYPE_INPUT, 0, 3);
    fixture->instructions[3].operands[3] = register_operand(OPERAND_TYPE_TEMP, 0);
    fixture->instructions[4] = (USILInstruction){.opcode = USIL_OP_RET,
        .source_instruction_index = 104};
    fixture->program.instruction_count = 5;
    for (int index = 0; index < 4; ++index)
        fixture->instructions[index].operands[0].swizzle_mode = 0;
}

static bool check_single_matrix_graph(void) {
    MatrixFixture fixture;
    initialize_single(&fixture);
    CHECK(analyze(&fixture));
    /* The natural multi-output interface guard suffices; the direct-return
     * specialization cannot decide whether a generic matrix graph is owned. */
    fixture.context.high_level_direct_return = false;
    HLSLMatrixLiftPlan plan;
    CHECK(hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    CHECK(plan.end_instruction == 3 && plan.instruction_owners == UINT64_C(15));
    CHECK(plan.result_components == 4 && plan.claimed_instruction_count == 4);
    CHECK(!plan.world_expression && plan.expression->logical_origin.complete);
    CHECK(plan.expression->logical_origin.instruction_index == 3 &&
          plan.expression->logical_origin.source_instruction_index == 103);
    CHECK(fixture.context.current_instruction_index == -1);
    StringBuilder source;
    sb_init(&source);
    ast_format_expr(plan.expression, &source);
    CHECK(sb_ok(&source) && strstr(source.buf, "mul((ObjectTransform), (position))"));
    CHECK(!strstr(source.buf, "float4(") && !strstr(source.buf, "get_cb"));
    sb_free(&source);
    hlsl_matrix_lift_plan_free(&plan);
    fixture.matrices[0].row_major = true;
    CHECK(hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    sb_init(&source); ast_format_expr(plan.expression, &source);
    CHECK(sb_ok(&source) && strstr(source.buf, "mul((position), (ObjectTransform))"));
    sb_free(&source); hlsl_matrix_lift_plan_free(&plan);
    release(&fixture);
    return true;
}

static bool check_single_matrix_negatives(void) {
    for (int mutation = 0; mutation < 23; ++mutation) {
        MatrixFixture fixture;
        initialize_single(&fixture);
        CHECK(analyze(&fixture));
        if (mutation == 0) fixture.instructions[1].precise_mask = 1;
        if (mutation == 1) fixture.instructions[2].saturate = true;
        if (mutation == 2) fixture.instructions[0].operands[1].has_abs = true;
        if (mutation == 3) fixture.instructions[2].operands[1].has_neg = true;
        if (mutation == 4) fixture.instructions[2].operands[2].min_precision = 1;
        if (mutation == 5) fixture.instructions[2].operands[3].swizzle[0] = 1;
        if (mutation == 6) fixture.instructions[2].operands[1].index_values[1] = 7;
        if (mutation == 7) fixture.instructions[3].operands[0].destination_mask = 0x70;
        if (mutation == 8) fixture.instructions[2].operands[1].extended_token_count = 1;
        if (mutation == 9) fixture.instructions[0].operands[0].register_index = 1;
        if (mutation == 10) fixture.instructions[0].operands[1].type = OPERAND_TYPE_TEMP;
        if (mutation == 11) fixture.instructions[0].opcode = USIL_OP_ADD;
        if (mutation == 12) fixture.matrices[0].matrix_array_size = 2;
        if (mutation == 13) fixture.matrices[0].rows = 3;
        if (mutation == 14) fixture.matrices[0].dim = 3;
        if (mutation == 15) fixture.matrices[0].authority = 3;
        if (mutation == 16) fixture.context.high_level_interface = false;
        if (mutation == 17) ++fixture.context.use_def.definition_use_counts[0];
        if (mutation == 18) fixture.instructions[1].operands[1].rel_offset0 = 7;
        if (mutation == 19) fixture.context.cbuffer_layouts[0].raw_storage = true;
        if (mutation == 20) fixture.instructions[1].resource_stride = 16;
        if (mutation == 21) fixture.instructions[2].texel_offsets[0] = 1;
        if (mutation == 22) fixture.instructions[0].geometry_stream_explicit = true;
        HLSLMatrixLiftPlan plan;
        memset(&plan, 0xff, sizeof(plan));
        CHECK(!hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
        CHECK(!plan.expression && !plan.world_expression && !plan.instruction_owners &&
              !plan.result_components && !plan.claimed_instruction_count);
        release(&fixture);
    }
    MatrixFixture fixture;
    initialize_single(&fixture);
    fixture.instructions[4] = (USILInstruction){.opcode = USIL_OP_MOV, .operand_count = 2,
        .source_instruction_index = 104};
    fixture.instructions[4].operands[0] = destination(OPERAND_TYPE_TEMP, 1);
    fixture.instructions[4].operands[1] = register_operand(OPERAND_TYPE_TEMP, 0);
    fixture.instructions[5] = (USILInstruction){.opcode = USIL_OP_RET,
        .source_instruction_index = 105};
    fixture.program.instruction_count = 6;
    CHECK(analyze(&fixture));
    HLSLMatrixLiftPlan plan;
    CHECK(!hlsl_matrix_lift_prepare(&fixture.context, 0, &plan));
    release(&fixture);
    return true;
}

int main(void) {
    return check_graph_and_metadata() && check_fail_closed_mutations() &&
           check_dataflow_ownership() && check_invalid_metadata_context() &&
           check_single_matrix_graph() && check_single_matrix_negatives() ? 0 : 1;
}
