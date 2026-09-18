#include "translation/dxbc_cbuffer_projection.h"
#include "dxbc/dxbc_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
        return 1; \
    } \
} while (0)

static void init_operand(DXBCOperand *operand, uint32_t cbuffer_register,
                         uint32_t row) {
    memset(operand, 0, sizeof(*operand));
    operand->type = OPERAND_TYPE_CONSTANT_BUFFER;
    operand->register_index_dim = 2;
    operand->register_index = (int)cbuffer_register;
    operand->rel_offset0 = (int)row;
    operand->index_has_immediate[0] = true;
    operand->index_has_immediate[1] = true;
    operand->index_values[0] = cbuffer_register;
    operand->index_values[1] = row;
    operand->swizzle_mode = 1;
    operand->swizzle[0] = 0;
    operand->swizzle[1] = 1;
    operand->swizzle[2] = 2;
    operand->swizzle[3] = 3;
}

static int test_static_projection(void) {
    const DXBCCBufferVariableRange variables[] = {
        {0, 4}, {4, 8}, {12, 4}, {16, 64}, {80, 16}
    };
    DXBCCBufferVariableUse uses[5];
    DXBCCBufferProjection projection;
    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 5));

    DXBCOperand operand;
    init_operand(&operand, 2, 0);
    operand.swizzle[0] = 3;
    operand.swizzle[1] = 2;
    operand.swizzle[2] = 1;
    operand.swizzle[3] = 0;
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 0x6, 2, 96, variables, 5, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(!uses[0].referenced);
    CHECK(uses[1].referenced && uses[1].component_mask == 0x6);
    CHECK(!uses[2].referenced && !uses[3].referenced && !uses[4].referenced);

    init_operand(&operand, 2, 3);
    operand.swizzle_mode = 2;
    operand.swizzle[0] = 3;
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 1, 2, 96, variables, 5, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(uses[3].referenced && (uses[3].component_mask & 0x8) != 0);
    CHECK(projection.saw_access && !projection.saw_dynamic_access);

    init_operand(&operand, 7, 0);
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 0xf, 2, 96, variables, 5, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    return 0;
}

static int test_padding_and_invalid_authority(void) {
    const DXBCCBufferVariableRange variables[] = {{0, 4}, {16, 16}};
    DXBCCBufferVariableUse uses[2];
    DXBCCBufferProjection projection;
    DXBCOperand operand;
    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 2));
    init_operand(&operand, 0, 0);
    operand.swizzle_mode = 2;
    operand.swizzle[0] = 3;
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 1, 0, 32, variables, 2, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(projection.saw_padding_access);

    init_operand(&operand, 0, 2);
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 1, 0, 32, variables, 2, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_INVALID);

    const DXBCCBufferVariableRange overlaps[] = {{0, 8}, {4, 4}};
    init_operand(&operand, 0, 0);
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 1, 0, 32, overlaps, 2, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_INVALID);

    const DXBCCBufferVariableRange optimized_out[] = {{0, 16}, {32, 16}};
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 1, 0, 32, optimized_out, 2, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    /* The DXBC declaration may end in the middle of a serialized variable
     * after the compiler prunes unused trailing rows. Only the declared
     * intersection participates in projection. */
    const DXBCCBufferVariableRange pruned_tail[] = {{16, 32}};
    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 1));
    init_operand(&operand, 0, 1);
    CHECK(dxbc_cbuffer_project_operand(
              &operand, 1, 0, 32, pruned_tail, 1, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(uses[0].referenced && !projection.saw_padding_access);
    return 0;
}

static int test_dynamic_projection_is_explicit(void) {
    const DXBCCBufferVariableRange variables[] = {
        {0, 4}, {4, 4}, {16, 4}, {20, 4}, {32, 16}
    };
    DXBCCBufferVariableUse uses[5];
    DXBCCBufferProjection projection;
    DXBCOperand operand;
    DXBCOperand relative;
    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 5));
    init_operand(&operand, 1, 0);
    memset(&relative, 0, sizeof(relative));
    relative.type = OPERAND_TYPE_TEMP;
    relative.swizzle_mode = 2;
    relative.swizzle[0] = 0;
    operand.rel_op1 = &relative;
    operand.index_representations[1] = 3;
    operand.swizzle_mode = 2;
    operand.swizzle[0] = 0;

    CHECK(dxbc_cbuffer_project_operand(
              &operand, 1, 1, 48, variables, 5, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_REQUIRES_INDEX_AUTHORITY);
    CHECK(projection.saw_dynamic_access);
    CHECK(uses[0].referenced && uses[0].dynamically_addressed);
    CHECK(!uses[1].referenced);
    CHECK(uses[2].referenced && uses[2].dynamically_addressed);
    CHECK(!uses[3].referenced);
    CHECK(uses[4].referenced && uses[4].dynamically_addressed);
    return 0;
}

static int test_program_lane_semantics(void) {
    const DXBCCBufferVariableRange variables[] = {
        {0, 4}, {4, 8}, {12, 4}, {16, 16}
    };
    DXBCCBufferVariableUse uses[4];
    DXBCCBufferProjection projection;
    USILInstruction instructions[2];
    USILProgram program;
    memset(&program, 0, sizeof(program));
    memset(instructions, 0, sizeof(instructions));
    program.instructions = instructions;
    program.instruction_count = 2;

    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0].type = OPERAND_TYPE_TEMP;
    instructions[0].operands[0].destination_mask = 0x60;
    init_operand(&instructions[0].operands[1], 0, 0);
    instructions[0].operands[1].swizzle[0] = 3;
    instructions[0].operands[1].swizzle[1] = 2;
    instructions[0].operands[1].swizzle[2] = 1;
    instructions[0].operands[1].swizzle[3] = 0;

    instructions[1].opcode = USIL_OP_DP3;
    instructions[1].operand_count = 3;
    instructions[1].operands[0].type = OPERAND_TYPE_TEMP;
    instructions[1].operands[0].destination_mask = 0x10;
    init_operand(&instructions[1].operands[1], 0, 1);
    instructions[1].operands[2].type = OPERAND_TYPE_TEMP;

    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 4));
    CHECK(dxbc_cbuffer_project_program(
              &program, 0, 32, variables, 4, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(!uses[0].referenced);
    CHECK(uses[1].referenced && uses[1].component_mask == 0x6);
    CHECK(!uses[2].referenced);
    CHECK(uses[3].referenced && uses[3].component_mask == 0x7);
    return 0;
}

static int test_special_instruction_lane_semantics(void) {
    const DXBCCBufferVariableRange variables[] = {
        {0, 4}, {4, 4}, {8, 4}, {12, 4}
    };
    DXBCCBufferVariableUse uses[4];
    DXBCCBufferProjection projection;
    USILInstruction instruction;
    USILProgram program;
    memset(&program, 0, sizeof(program));
    memset(&instruction, 0, sizeof(instruction));
    program.instructions = &instruction;
    program.instruction_count = 1;

    /* sincos has two destinations.  The source lanes are the union of both
     * write masks, not merely the first destination's mask. */
    instruction.opcode = USIL_OP_SINCOS;
    instruction.operand_count = 3;
    instruction.operands[0].type = OPERAND_TYPE_TEMP;
    instruction.operands[0].destination_mask = 0x10;
    instruction.operands[1].type = OPERAND_TYPE_TEMP;
    instruction.operands[1].destination_mask = 0x80;
    init_operand(&instruction.operands[2], 0, 0);
    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 4));
    CHECK(dxbc_cbuffer_project_program(
              &program, 0, 16, variables, 4, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(uses[0].referenced && !uses[1].referenced &&
          !uses[2].referenced && uses[3].referenced);

    /* A 2D-array sample consumes xyz from the address operand. */
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_SAMPLE;
    instruction.operand_count = 4;
    instruction.operands[0].type = OPERAND_TYPE_TEMP;
    instruction.operands[0].destination_mask = 0xf0;
    init_operand(&instruction.operands[1], 0, 0);
    instruction.operands[1].swizzle[0] = 3;
    instruction.operands[1].swizzle[1] = 2;
    instruction.operands[1].swizzle[2] = 1;
    instruction.operands[1].swizzle[3] = 0;
    instruction.operands[2].type = OPERAND_TYPE_RESOURCE;
    instruction.operands[2].register_index = 7;
    instruction.operands[3].type = OPERAND_TYPE_SAMPLER;
    snprintf(instruction.resource_dimension,
             sizeof(instruction.resource_dimension), "2darray");
    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 4));
    CHECK(dxbc_cbuffer_project_program(
              &program, 0, 16, variables, 4, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(!uses[0].referenced && uses[1].referenced &&
          uses[2].referenced && uses[3].referenced);

    /* A typed-buffer ld has one address component and no mip component. */
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_LD;
    instruction.operand_count = 3;
    instruction.operands[0].type = OPERAND_TYPE_TEMP;
    instruction.operands[0].destination_mask = 0xf0;
    init_operand(&instruction.operands[1], 0, 0);
    instruction.operands[2].type = OPERAND_TYPE_RESOURCE;
    instruction.operands[2].register_index = 9;
    snprintf(instruction.resource_dimension,
             sizeof(instruction.resource_dimension), "buffer");
    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 4));
    CHECK(dxbc_cbuffer_project_program(
              &program, 0, 16, variables, 4, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(uses[0].referenced && !uses[1].referenced &&
          !uses[2].referenced && !uses[3].referenced);
    return 0;
}

static int test_nested_relative_access(void) {
    const DXBCCBufferVariableRange variables[] = {{0, 4}};
    DXBCCBufferVariableUse uses[1];
    DXBCCBufferProjection projection;
    USILInstruction instruction;
    USILProgram program;
    memset(&instruction, 0, sizeof(instruction));
    memset(&program, 0, sizeof(program));
    program.instructions = &instruction;
    program.instruction_count = 1;
    instruction.opcode = USIL_OP_MOV;
    instruction.operand_count = 2;
    instruction.operands[0].type = OPERAND_TYPE_TEMP;
    instruction.operands[0].destination_mask = 0x10;
    instruction.operands[1].type = OPERAND_TYPE_INDEXABLE_TEMP;
    instruction.operands[1].swizzle_mode = 2;
    instruction.operands[1].swizzle[0] = 0;

    DXBCOperand relative;
    init_operand(&relative, 3, 0);
    relative.swizzle_mode = 2;
    relative.swizzle[0] = 0;
    instruction.operands[1].rel_op0 = &relative;

    CHECK(dxbc_cbuffer_projection_reset(&projection, uses, 1));
    CHECK(dxbc_cbuffer_project_program(
              &program, 3, 16, variables, 1, uses, &projection) ==
          DXBC_CBUFFER_PROJECTION_EXACT);
    CHECK(uses[0].referenced && uses[0].component_mask == 1);
    return 0;
}

static int test_fixture_payload(const uint8_t *bytes, size_t length) {
    DXBCContainer container;
    CHECK(dxbc_parse(&container, bytes, length));
    USILProgram program;
    CHECK(usil_translate(&program, &container));
    for (int index = 0; index < program.cbuffer_count; ++index) {
        CHECK(program.cbuffers[index].reg_idx >= 0);
        CHECK(program.cbuffers[index].size > 0);
        DXBCCBufferVariableRange variable = {
            0, (uint32_t)program.cbuffers[index].size * 16u
        };
        DXBCCBufferVariableUse use;
        DXBCCBufferProjection projection;
        CHECK(dxbc_cbuffer_projection_reset(&projection, &use, 1));
        CHECK(dxbc_cbuffer_project_program(
                  &program, (uint32_t)program.cbuffers[index].reg_idx,
                  variable.byte_size, &variable, 1, &use, &projection) ==
              DXBC_CBUFFER_PROJECTION_EXACT);
    }
    usil_free(&program);
    dxbc_free(&container);
    return 0;
}

static uint32_t read_le_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int test_fixture(const char *path) {
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = (uint8_t *)malloc((size_t)length);
    CHECK(bytes != NULL);
    CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);

    if ((size_t)length >= 8 && memcmp(bytes, "USBD", 4) == 0) {
        uint32_t record_count = read_le_u32(bytes + 4);
        size_t position = 8;
        for (uint32_t record = 0; record < record_count; ++record) {
            CHECK(position <= (size_t)length &&
                  (size_t)length - position >= 4);
            uint32_t name_size = read_le_u32(bytes + position);
            position += 4;
            CHECK(name_size <= (size_t)length - position);
            position += name_size;
            CHECK((size_t)length - position >= 4);
            uint32_t payload_size = read_le_u32(bytes + position);
            position += 4;
            CHECK(payload_size <= (size_t)length - position);
            CHECK(test_fixture_payload(bytes + position, payload_size) == 0);
            position += payload_size;
        }
        CHECK(position == (size_t)length);
    } else {
        CHECK(test_fixture_payload(bytes, (size_t)length) == 0);
    }
    free(bytes);
    return 0;
}

int main(int argc, char **argv) {
    CHECK(test_static_projection() == 0);
    CHECK(test_padding_and_invalid_authority() == 0);
    CHECK(test_dynamic_projection_is_explicit() == 0);
    CHECK(test_program_lane_semantics() == 0);
    CHECK(test_special_instruction_lane_semantics() == 0);
    CHECK(test_nested_relative_access() == 0);
    for (int index = 1; index < argc; ++index) {
        CHECK(test_fixture(argv[index]) == 0);
    }
    printf("cbuffer projection unit tests passed\n");
    return 0;
}
