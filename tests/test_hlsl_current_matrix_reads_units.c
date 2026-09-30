// SPDX-License-Identifier: GPL-3.0-only
#include "translation/hlsl_current_matrix_reads.h"
#include <stdio.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); return 1; } } while (0)

typedef struct {
    USILInstruction instructions[5];
    USILConstantBuffer cbuffer;
    USILProgram program;
    SerializedVariable variable;
    SerializedConstantBuffer buffer;
    SerializedResourceParam resources[2];
    SerializedProgramParameters parameters;
} Fixture;

static void initialize(Fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->variable = (SerializedVariable){.name = "ObjectTransform", .layout = {0,4,4,1,0,0}};
    fixture->buffer = (SerializedConstantBuffer){.name = "Matrices", .size = 80,
        .var_count = 1, .variables = &fixture->variable};
    fixture->resources[0] = (SerializedResourceParam){.name = "Matrices",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .bind_index = 2};
    fixture->parameters = (SerializedProgramParameters){.is_binary = true,
        .cb_count = 1, .constant_buffers = &fixture->buffer, .res_count = 1,
        .resources = fixture->resources};
    fixture->cbuffer = (USILConstantBuffer){.reg_idx = 2, .size = 4};
    fixture->program = (USILProgram){.instruction_count = 5, .instruction_alloc = 5, .instructions = fixture->instructions,
        .temp_count = 1, .cbuffers = &fixture->cbuffer, .cbuffer_count = 1, .cbuffer_alloc = 1,
        .program_type = DXBC_PROGRAM_TYPE_VERTEX, .shader_model_major = 5};
    for (int index = 0; index < 4; ++index) {
        USILInstruction *instruction = &fixture->instructions[index];
        instruction->opcode = USIL_OP_MOV;
        instruction->operand_count = 2;
        instruction->source_instruction_index = (uint32_t)(10 + index);
        instruction->operands[0] = (DXBCOperand){.type = OPERAND_TYPE_TEMP,
            .register_index_dim = 1, .destination_mask = 0xf0,
            .index_has_immediate = {true}, .index_values = {0}};
        instruction->operands[1] = (DXBCOperand){.type = OPERAND_TYPE_CONSTANT_BUFFER,
            .register_index = 2, .register_index_dim = 2, .rel_offset0 = index,
            .index_has_immediate = {true,true}, .index_values = {2,(uint32_t)index},
            .swizzle_mode = 1, .swizzle = {0,1,2,3}};
    }
    fixture->instructions[4].opcode = USIL_OP_RET;
    fixture->instructions[4].source_instruction_index = 14;
}

static int positive_and_ownership(void) {
    Fixture fixture;
    initialize(&fixture);
    HLSLCurrentMatrixReads reads = {0};
    CHECK(hlsl_current_matrix_reads_build(&fixture.program, &fixture.parameters, NULL, &reads) == HLSL_CURRENT_MATRIX_OK);
    CHECK(reads.field_count == 1 && reads.read_count == 16);
    CHECK(reads.fields[0].field_authority == 1 && reads.fields[0].binding_authority == 1);
    CHECK(reads.fields[0].full_shell_authorities == 1 && reads.fields[0].reflected_byte_size == 80);
    CHECK(reads.fields[0].declared_byte_size == 64 && reads.fields[0].binding_register == 2);
    CHECK(reads.fields[0].logical_aggregate_id == (UINT64_C(3) << 32));
    for (int index = 0; index < 16; ++index) {
        CHECK(reads.reads[index].instruction_index == index / 4);
        CHECK(reads.reads[index].source_instruction_index == (uint32_t)(10 + index / 4));
        CHECK(reads.reads[index].logical_lane == index % 4 && reads.reads[index].physical_lane == index % 4);
        CHECK(reads.reads[index].byte_offset == (uint32_t)index * 4);
    }
    fixture.variable.name = "RenamedAfterProduction";
    fixture.buffer.name = "RenamedBlock";
    CHECK(strcmp(reads.fields[0].field_name, "ObjectTransform") == 0);
    CHECK(strcmp(reads.fields[0].block_name, "Matrices") == 0);
    hlsl_current_matrix_reads_dispose(&reads);
    CHECK(!reads.reads && !reads.fields && !reads.read_count && !reads.field_count);
    initialize(&fixture);
    CHECK(hlsl_current_matrix_reads_build(&fixture.program, NULL, &fixture.parameters, &reads) == HLSL_CURRENT_MATRIX_OK);
    CHECK(reads.fields[0].field_authority == 2 && reads.fields[0].binding_authority == 2);
    CHECK(reads.fields[0].full_shell_authorities == 2);
    hlsl_current_matrix_reads_dispose(&reads);
    return 0;
}

static int swizzled_demands(void) {
    Fixture fixture;
    initialize(&fixture);
    fixture.instructions[0].operands[0].destination_mask = 0x60;
    fixture.instructions[0].operands[1].swizzle[1] = 3;
    fixture.instructions[0].operands[1].swizzle[2] = 3;
    HLSLCurrentMatrixReads reads = {0};
    CHECK(hlsl_current_matrix_reads_build(&fixture.program, &fixture.parameters, NULL, &reads) == HLSL_CURRENT_MATRIX_OK);
    CHECK(reads.read_count == 14);
    CHECK(reads.reads[0].logical_lane == 1 && reads.reads[0].physical_lane == 3 && reads.reads[0].byte_offset == 12);
    CHECK(reads.reads[1].logical_lane == 2 && reads.reads[1].physical_lane == 3 && reads.reads[1].byte_offset == 12);
    CHECK(reads.reads[0].destination_lanes == 6);
    hlsl_current_matrix_reads_dispose(&reads);
    return 0;
}

static int rejects_and_empty_scope(void) {
    for (int change = 0; change < 18; ++change) {
        Fixture fixture;
        initialize(&fixture);
        DXBCOperand relative = {.type = OPERAND_TYPE_TEMP};
        if (change == 0) fixture.cbuffer.dynamic_indexed = true;
        if (change == 1) fixture.instructions[0].operands[1].rel_op1 = &relative;
        if (change == 2) fixture.instructions[0].operands[1].has_abs = true;
        if (change == 3) fixture.instructions[0].operands[1].min_precision = 1;
        if (change == 4) fixture.variable.layout[4] = 2;
        if (change == 5) fixture.variable.layout[1] = 3;
        if (change == 6) fixture.buffer.role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
        if (change == 7) fixture.resources[0].bind_index = 3;
        if (change == 8) fixture.buffer.size = 48;
        if (change == 9) { fixture.buffer.name = "$Globals"; fixture.resources[0].name = "$Globals"; }
        if (change == 10) { fixture.resources[1] = fixture.resources[0]; fixture.resources[1].name = "Conflicting"; fixture.parameters.res_count = 2; }
        if (change == 11) fixture.variable.layout[5] = 4;
        if (change == 12) fixture.program.instruction_alloc = 4;
        if (change == 13) fixture.program.cbuffer_alloc = 0;
        if (change == 14) fixture.program.instruction_count = -1;
        if (change == 15) fixture.program.cbuffer_count = -1;
        if (change == 16) fixture.buffer.struct_count = HLSL_CURRENT_MATRIX_FIELD_LIMIT + 1;
        if (change == 17) fixture.buffer.struct_count = -1;
        HLSLCurrentMatrixReads reads = {0};
        CHECK(hlsl_current_matrix_reads_build(&fixture.program, &fixture.parameters, NULL, &reads) != HLSL_CURRENT_MATRIX_OK);
        CHECK(!reads.fields && !reads.reads && !reads.field_count && !reads.read_count);
    }
    Fixture fixture;
    initialize(&fixture);
    fixture.program.instruction_count = 1;
    fixture.program.instructions = &fixture.instructions[4];
    HLSLCurrentMatrixReads reads = {0};
    CHECK(hlsl_current_matrix_reads_build(&fixture.program, &fixture.parameters, NULL, &reads) == HLSL_CURRENT_MATRIX_NOT_APPLICABLE);
    CHECK(!reads.fields && !reads.reads && !reads.field_count && !reads.read_count);
    fixture.program.cbuffer_count = 0;
    CHECK(hlsl_current_matrix_reads_build(&fixture.program, NULL, NULL, &reads) == HLSL_CURRENT_MATRIX_NOT_APPLICABLE);
    CHECK(!reads.fields && !reads.reads);
    return 0;
}

int main(void) {
    if (positive_and_ownership() || swizzled_demands() || rejects_and_empty_scope()) return 1;
    puts("current matrix-read ownership/demand/negative units passed");
    return 0;
}
