#include "translation/hlsl_emitter_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

enum {
    MASK_X = 0x10,
    MASK_Y = 0x20,
    MASK_Z = 0x40,
    MASK_W = 0x80,
    MASK_XY = 0x30,
    MASK_XYZ = 0x70,
    MASK_XYZW = 0xf0,
};

static void init_register_operand(DXBCOperand *operand,
                                  DXBCOperandType type,
                                  int register_index) {
    memset(operand, 0, sizeof(*operand));
    operand->type = type;
    operand->register_index = register_index;
    operand->swizzle_mode = 1;
    operand->swizzle[0] = 0;
    operand->swizzle[1] = 1;
    operand->swizzle[2] = 2;
    operand->swizzle[3] = 3;
    operand->register_index_dim = 1;
    operand->index_has_immediate[0] = true;
    operand->index_values[0] = (uint32_t)register_index;
}

static void init_cbuffer_operand(DXBCOperand *operand, int buffer, int row) {
    init_register_operand(operand, OPERAND_TYPE_CONSTANT_BUFFER, buffer);
    operand->register_index_dim = 2;
    operand->index_has_immediate[1] = true;
    operand->index_values[1] = (uint32_t)row;
    operand->rel_offset0 = row;
}

static void init_half_scalar(DXBCOperand *operand) {
    memset(operand, 0, sizeof(*operand));
    operand->type = OPERAND_TYPE_IMMEDIATE32;
    operand->swizzle_mode = 2;
    operand->imm_value_count = 1;
    operand->immediate_word_count = 1;
    operand->imm_values[0] = 0x3f000000u;
    operand->immediate_words[0] = 0x3f000000u;
}

static void init_half_vector(DXBCOperand *operand, bool include_w) {
    memset(operand, 0, sizeof(*operand));
    operand->type = OPERAND_TYPE_IMMEDIATE32;
    operand->imm_value_count = 4;
    operand->immediate_word_count = 4;
    operand->imm_values[0] = 0x3f000000u;
    operand->imm_values[2] = 0x3f000000u;
    operand->imm_values[3] = include_w ? 0x3f000000u : 0u;
    memcpy(operand->immediate_words, operand->imm_values,
           sizeof(operand->imm_values));
}

static void set_swizzle(DXBCOperand *operand, int x, int y, int z, int w) {
    operand->swizzle_mode = 1;
    operand->swizzle[0] = x;
    operand->swizzle[1] = y;
    operand->swizzle[2] = z;
    operand->swizzle[3] = w;
}

static void set_replicate(DXBCOperand *operand, int component) {
    operand->swizzle_mode = 2;
    operand->swizzle[0] = component;
    operand->swizzle[1] = component;
    operand->swizzle[2] = component;
    operand->swizzle[3] = component;
}

static void init_instruction(USILInstruction *instruction,
                             USILOpcode opcode, int operand_count) {
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = opcode;
    instruction->operand_count = operand_count;
}

static void init_program(USILProgram *program,
                         USILInstruction *instructions, int count,
                         int temp_count) {
    memset(program, 0, sizeof(*program));
    snprintf(program->shader_type_model, sizeof(program->shader_type_model),
             "vs_4_0");
    program->program_type = DXBC_PROGRAM_TYPE_VERTEX;
    program->temp_count = temp_count;
    program->instructions = instructions;
    program->instruction_count = count;
    program->instruction_alloc = count;
}

static int model_swaps_binary(const USILProgram *program, int instruction,
                              bool expected);

static void init_tangent_frame_program(USILProgram *program,
                                       USILInstruction instructions[20]) {
    memset(instructions, 0, sizeof(*instructions) * 20u);

    init_instruction(&instructions[0], USIL_OP_MUL, 3);
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[0].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_INPUT, 1);
    set_replicate(&instructions[0].operands[1], 1);
    init_cbuffer_operand(&instructions[0].operands[2], 1, 1);
    set_swizzle(&instructions[0].operands[2], 1, 2, 0, 0);

    init_instruction(&instructions[1], USIL_OP_MAD, 4);
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[1].operands[0].destination_mask = MASK_XYZ;
    init_cbuffer_operand(&instructions[1].operands[1], 1, 0);
    set_swizzle(&instructions[1].operands[1], 1, 2, 0, 0);
    init_register_operand(&instructions[1].operands[2], OPERAND_TYPE_INPUT, 1);
    set_replicate(&instructions[1].operands[2], 0);
    init_register_operand(&instructions[1].operands[3], OPERAND_TYPE_TEMP, 1);

    init_instruction(&instructions[2], USIL_OP_MAD, 4);
    init_register_operand(&instructions[2].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[2].operands[0].destination_mask = MASK_XYZ;
    init_cbuffer_operand(&instructions[2].operands[1], 1, 2);
    set_swizzle(&instructions[2].operands[1], 1, 2, 0, 0);
    init_register_operand(&instructions[2].operands[2], OPERAND_TYPE_INPUT, 1);
    set_replicate(&instructions[2].operands[2], 2);
    init_register_operand(&instructions[2].operands[3], OPERAND_TYPE_TEMP, 1);

    init_instruction(&instructions[3], USIL_OP_DP3, 3);
    init_register_operand(&instructions[3].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[3].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[3].operands[1], OPERAND_TYPE_TEMP, 1);
    init_register_operand(&instructions[3].operands[2], OPERAND_TYPE_TEMP, 1);

    init_instruction(&instructions[4], USIL_OP_RSQ, 2);
    init_register_operand(&instructions[4].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[4].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[4].operands[1], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[4].operands[1], 3);

    init_instruction(&instructions[5], USIL_OP_MUL, 3);
    init_register_operand(&instructions[5].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[5].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[5].operands[1], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[5].operands[1], 3);
    init_register_operand(&instructions[5].operands[2], OPERAND_TYPE_TEMP, 1);

    init_instruction(&instructions[6], USIL_OP_MUL, 3);
    init_register_operand(&instructions[6].operands[0], OPERAND_TYPE_TEMP, 2);
    instructions[6].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[6].operands[1], OPERAND_TYPE_TEMP, 0);
    set_swizzle(&instructions[6].operands[1], 2, 0, 1, 0);
    init_register_operand(&instructions[6].operands[2], OPERAND_TYPE_TEMP, 1);

    init_instruction(&instructions[7], USIL_OP_MAD, 4);
    init_register_operand(&instructions[7].operands[0], OPERAND_TYPE_TEMP, 2);
    instructions[7].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[7].operands[1], OPERAND_TYPE_TEMP, 0);
    set_swizzle(&instructions[7].operands[1], 1, 2, 0, 0);
    init_register_operand(&instructions[7].operands[2], OPERAND_TYPE_TEMP, 1);
    set_swizzle(&instructions[7].operands[2], 1, 2, 0, 0);
    init_register_operand(&instructions[7].operands[3], OPERAND_TYPE_TEMP, 2);
    instructions[7].operands[3].has_neg = true;

    init_instruction(&instructions[8], USIL_OP_MUL, 3);
    init_register_operand(&instructions[8].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[8].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[8].operands[1], OPERAND_TYPE_INPUT, 1);
    set_replicate(&instructions[8].operands[1], 3);
    init_cbuffer_operand(&instructions[8].operands[2], 1, 9);
    set_replicate(&instructions[8].operands[2], 3);

    init_instruction(&instructions[9], USIL_OP_MUL, 3);
    init_register_operand(&instructions[9].operands[0], OPERAND_TYPE_TEMP, 2);
    instructions[9].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[9].operands[1], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[9].operands[1], 3);
    init_register_operand(&instructions[9].operands[2], OPERAND_TYPE_TEMP, 2);

    static const int output_registers[9] = {6, 6, 6, 7, 8, 7, 8, 7, 8};
    static const int output_lanes[9] = {1, 2, 0, 0, 0, 2, 2, 1, 1};
    static const int source_registers[9] = {2, 0, 1, 1, 1, 0, 0, 2, 2};
    static const int source_lanes[9] = {0, 0, 2, 0, 1, 1, 2, 1, 2};
    for (int item = 0; item < 9; item++) {
        USILInstruction *move = &instructions[10 + item];
        init_instruction(move, USIL_OP_MOV, 2);
        init_register_operand(&move->operands[0], OPERAND_TYPE_OUTPUT,
                              output_registers[item]);
        move->operands[0].destination_mask = 16 << output_lanes[item];
        init_register_operand(&move->operands[1], OPERAND_TYPE_TEMP,
                              source_registers[item]);
        set_replicate(&move->operands[1], source_lanes[item]);
    }
    init_instruction(&instructions[19], USIL_OP_RET, 0);
    init_program(program, instructions, 20, 3);
}

static int verify_tangent_frame_inverse_is_mutation_safe(void) {
    USILInstruction instructions[20];
    USILProgram program;
    init_tangent_frame_program(&program, instructions);
    CHECK(hlsl_compiler_tangent_frame_matches(&program, 0));

    instructions[1].operands[1].rel_offset0 = 2;
    instructions[1].operands[1].index_values[1] = 2u;
    CHECK(!hlsl_compiler_tangent_frame_matches(&program, 0));
    instructions[1].operands[1].rel_offset0 = 0;
    instructions[1].operands[1].index_values[1] = 0u;
    CHECK(hlsl_compiler_tangent_frame_matches(&program, 0));

    instructions[4].opcode = USIL_OP_RCP;
    CHECK(!hlsl_compiler_tangent_frame_matches(&program, 0));
    instructions[4].opcode = USIL_OP_RSQ;
    CHECK(hlsl_compiler_tangent_frame_matches(&program, 0));

    instructions[7].operands[3].has_neg = false;
    CHECK(!hlsl_compiler_tangent_frame_matches(&program, 0));
    instructions[7].operands[3].has_neg = true;
    instructions[6].operands[1].swizzle[0] = 1;
    CHECK(!hlsl_compiler_tangent_frame_matches(&program, 0));
    instructions[6].operands[1].swizzle[0] = 2;
    CHECK(hlsl_compiler_tangent_frame_matches(&program, 0));

    instructions[18].operands[0].destination_mask = MASK_Z;
    CHECK(!hlsl_compiler_tangent_frame_matches(&program, 0));
    instructions[18].operands[0].destination_mask = MASK_Y;
    CHECK(hlsl_compiler_tangent_frame_matches(&program, 0));
    return 0;
}

typedef enum ScreenPattern {
    SCREEN_COMBINED,
    SCREEN_SPLIT,
    SCREEN_INPLACE,
    SCREEN_INPLACE_SCRATCH,
} ScreenPattern;

static int init_screen_program(USILProgram *program,
                               USILInstruction instructions[10],
                               ScreenPattern pattern, bool intermediate_copy,
                               bool post_read) {
    memset(instructions, 0, sizeof(*instructions) * 10u);
    init_instruction(&instructions[0], USIL_OP_MAD, 4);
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[0].destination_mask = MASK_XYZW;
    init_cbuffer_operand(&instructions[0].operands[1], 3, 20);
    init_register_operand(&instructions[0].operands[2], OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[0].operands[2], 3);
    init_register_operand(&instructions[0].operands[3], OPERAND_TYPE_TEMP, 2);

    init_instruction(&instructions[1], USIL_OP_MOV, 2);
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_OUTPUT, 0);
    instructions[1].operands[0].destination_mask = MASK_XYZW;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 0);

    int start = 2;
    if (intermediate_copy) {
        init_instruction(&instructions[start], USIL_OP_MOV, 2);
        init_register_operand(&instructions[start].operands[0],
                              OPERAND_TYPE_OUTPUT, 7);
        instructions[start].operands[0].destination_mask = MASK_X;
        init_register_operand(&instructions[start].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_replicate(&instructions[start].operands[1], 0);
        start++;
    }

    int cursor = start;
    if (pattern == SCREEN_COMBINED) {
        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[cursor].operands[0].destination_mask = MASK_Y;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_replicate(&instructions[cursor].operands[1], 1);
        init_cbuffer_operand(&instructions[cursor].operands[2], 4, 0);
        set_replicate(&instructions[cursor].operands[2], 0);
        cursor++;

        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[cursor].operands[0].destination_mask =
            MASK_X | MASK_Z | MASK_W;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_swizzle(&instructions[cursor].operands[1], 0, 0, 3, 1);
        init_half_vector(&instructions[cursor].operands[2], true);
        cursor++;
    } else if (pattern == SCREEN_SPLIT) {
        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[cursor].operands[0].destination_mask = MASK_X;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_replicate(&instructions[cursor].operands[1], 1);
        init_cbuffer_operand(&instructions[cursor].operands[2], 4, 0);
        set_replicate(&instructions[cursor].operands[2], 0);
        cursor++;

        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[cursor].operands[0].destination_mask = MASK_W;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 3);
        set_replicate(&instructions[cursor].operands[1], 0);
        init_half_scalar(&instructions[cursor].operands[2]);
        cursor++;

        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[cursor].operands[0].destination_mask = MASK_X | MASK_Z;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_swizzle(&instructions[cursor].operands[1], 0, 0, 3, 0);
        init_half_vector(&instructions[cursor].operands[2], false);
        cursor++;
    } else {
        const bool scratch = pattern == SCREEN_INPLACE_SCRATCH;
        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, scratch ? 3 : 0);
        instructions[cursor].operands[0].destination_mask =
            scratch ? MASK_X : MASK_Y;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_replicate(&instructions[cursor].operands[1], 1);
        init_cbuffer_operand(&instructions[cursor].operands[2], 4, 0);
        set_replicate(&instructions[cursor].operands[2], 0);
        cursor++;

        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[cursor].operands[0].destination_mask = MASK_X | MASK_Z;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_swizzle(&instructions[cursor].operands[1], 0, 0, 3, 0);
        init_half_vector(&instructions[cursor].operands[2], false);
        cursor++;

        init_instruction(&instructions[cursor], USIL_OP_MUL, 3);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[cursor].operands[0].destination_mask = MASK_W;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, scratch ? 3 : 0);
        set_replicate(&instructions[cursor].operands[1], scratch ? 0 : 1);
        init_half_scalar(&instructions[cursor].operands[2]);
        cursor++;
    }

    if (pattern == SCREEN_COMBINED || pattern == SCREEN_SPLIT) {
        init_instruction(&instructions[cursor], USIL_OP_MOV, 2);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_OUTPUT, 4);
        instructions[cursor].operands[0].destination_mask = MASK_Z | MASK_W;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        cursor++;
    }

    init_instruction(&instructions[cursor], USIL_OP_ADD, 3);
    init_register_operand(&instructions[cursor].operands[0],
                          OPERAND_TYPE_OUTPUT, 4);
    instructions[cursor].operands[0].destination_mask = MASK_XY;
    init_register_operand(&instructions[cursor].operands[1],
                          OPERAND_TYPE_TEMP,
                          (pattern == SCREEN_COMBINED ||
                           pattern == SCREEN_SPLIT) ? 3 : 0);
    set_replicate(&instructions[cursor].operands[1], 2);
    init_register_operand(&instructions[cursor].operands[2],
                          OPERAND_TYPE_TEMP,
                          (pattern == SCREEN_COMBINED ||
                           pattern == SCREEN_SPLIT) ? 3 : 0);
    set_swizzle(&instructions[cursor].operands[2], 0, 3, 0, 0);
    cursor++;

    if (post_read) {
        init_instruction(&instructions[cursor], USIL_OP_MOV, 2);
        init_register_operand(&instructions[cursor].operands[0],
                              OPERAND_TYPE_TEMP, 5);
        instructions[cursor].operands[0].destination_mask = MASK_X;
        init_register_operand(&instructions[cursor].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        set_replicate(&instructions[cursor].operands[1], 0);
        cursor++;
    }
    init_instruction(&instructions[cursor++], USIL_OP_RET, 0);
    init_program(program, instructions, cursor, 6);
    return start;
}

static int verify_screen_position_inverses_are_mutation_safe(void) {
    USILInstruction instructions[10];
    USILProgram program;
    int screen = -1;
    for (int pattern = SCREEN_COMBINED;
         pattern <= SCREEN_INPLACE_SCRATCH; pattern++) {
        int expected = init_screen_program(&program, instructions,
                                           (ScreenPattern)pattern,
                                           true, false);
        CHECK(hlsl_compiler_screen_position_matches(&program, 0, &screen));
        CHECK(screen == expected);
    }

    int start = init_screen_program(&program, instructions,
                                    SCREEN_COMBINED, true, false);
    instructions[start + 1].operands[2].immediate_words[0] ^= 1u;
    CHECK(!hlsl_compiler_screen_position_matches(&program, 0, &screen));
    instructions[start + 1].operands[2].immediate_words[0] ^= 1u;
    CHECK(hlsl_compiler_screen_position_matches(&program, 0, &screen));

    instructions[start + 3].operands[0].destination_mask = MASK_X;
    CHECK(!hlsl_compiler_screen_position_matches(&program, 0, &screen));
    instructions[start + 3].operands[0].destination_mask = MASK_XY;
    CHECK(hlsl_compiler_screen_position_matches(&program, 0, &screen));

    /* The intervening copy is admitted only while it is a pure consumer of
     * the still-live clip value.  Turning it into arithmetic or overwriting
     * a required clip lane invalidates the proof. */
    instructions[2].opcode = USIL_OP_ADD;
    instructions[2].operand_count = 3;
    init_register_operand(&instructions[2].operands[2], OPERAND_TYPE_TEMP, 5);
    CHECK(!hlsl_compiler_screen_position_matches(&program, 0, &screen));
    init_screen_program(&program, instructions, SCREEN_COMBINED, true, false);
    instructions[2].operands[0].type = OPERAND_TYPE_TEMP;
    instructions[2].operands[0].register_index = 0;
    instructions[2].operands[0].index_values[0] = 0u;
    init_register_operand(&instructions[2].operands[1], OPERAND_TYPE_TEMP, 5);
    CHECK(!hlsl_compiler_screen_position_matches(&program, 0, &screen));

    init_screen_program(&program, instructions, SCREEN_INPLACE_SCRATCH,
                        false, true);
    CHECK(!hlsl_compiler_screen_position_matches(&program, 0, &screen));
    return 0;
}

static void init_interleaved_projection_pack_program(
    USILProgram *program, USILInstruction instructions[8]) {
    memset(instructions, 0, sizeof(*instructions) * 8u);

    /* Exact retained ScreenSpace normal form, renumbered to start at zero. */
    init_instruction(&instructions[0], USIL_OP_MUL, 3);
    init_register_operand(&instructions[0].operands[0],
                          OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[0].destination_mask = MASK_Y;
    init_register_operand(&instructions[0].operands[1],
                          OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[0].operands[1], 1);
    init_cbuffer_operand(&instructions[0].operands[2], 0, 5);
    set_replicate(&instructions[0].operands[2], 0);

    init_instruction(&instructions[1], USIL_OP_MUL, 3);
    init_register_operand(&instructions[1].operands[0],
                          OPERAND_TYPE_TEMP, 1);
    instructions[1].operands[0].destination_mask =
        MASK_X | MASK_Z | MASK_W;
    init_register_operand(&instructions[1].operands[1],
                          OPERAND_TYPE_TEMP, 0);
    set_swizzle(&instructions[1].operands[1], 0, 0, 3, 1);
    init_half_vector(&instructions[1].operands[2], true);

    init_instruction(&instructions[2], USIL_OP_MUL, 3);
    init_register_operand(&instructions[2].operands[0],
                          OPERAND_TYPE_TEMP, 0);
    instructions[2].operands[0].destination_mask =
        MASK_Y | MASK_Z | MASK_W;
    init_register_operand(&instructions[2].operands[1],
                          OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[2].operands[1], 1);
    init_cbuffer_operand(&instructions[2].operands[2], 1, 11);
    set_swizzle(&instructions[2].operands[2], 0, 0, 1, 2);

    init_instruction(&instructions[3], USIL_OP_MAD, 4);
    init_register_operand(&instructions[3].operands[0],
                          OPERAND_TYPE_TEMP, 0);
    instructions[3].operands[0].destination_mask = MASK_XYZ;
    init_cbuffer_operand(&instructions[3].operands[1], 1, 10);
    set_swizzle(&instructions[3].operands[1], 0, 1, 2, 0);
    init_register_operand(&instructions[3].operands[2],
                          OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[3].operands[2], 0);
    init_register_operand(&instructions[3].operands[3],
                          OPERAND_TYPE_TEMP, 0);
    set_swizzle(&instructions[3].operands[3], 1, 2, 3, 1);

    init_instruction(&instructions[4], USIL_OP_ADD, 3);
    init_register_operand(&instructions[4].operands[0],
                          OPERAND_TYPE_OUTPUT, 1);
    instructions[4].operands[0].destination_mask = MASK_Z | MASK_W;
    init_register_operand(&instructions[4].operands[1],
                          OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[4].operands[1], 2);
    init_register_operand(&instructions[4].operands[2],
                          OPERAND_TYPE_TEMP, 1);
    set_swizzle(&instructions[4].operands[2], 0, 0, 0, 3);

    /* The packed physical lanes are dead only after these independent
     * overwrites.  The read at instruction six observes the new z lane. */
    init_instruction(&instructions[5], USIL_OP_ADD, 3);
    init_register_operand(&instructions[5].operands[0],
                          OPERAND_TYPE_TEMP, 1);
    instructions[5].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[5].operands[1],
                          OPERAND_TYPE_TEMP, 0);
    init_cbuffer_operand(&instructions[5].operands[2], 1, 12);

    init_instruction(&instructions[6], USIL_OP_MOV, 2);
    init_register_operand(&instructions[6].operands[0],
                          OPERAND_TYPE_TEMP, 1);
    instructions[6].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[6].operands[1],
                          OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[6].operands[1], 2);
    instructions[6].operands[1].has_neg = true;

    init_instruction(&instructions[7], USIL_OP_RET, 0);
    init_program(program, instructions, 8, 2);
}

static bool model_has_interleaved_projection_pack(
    const USILProgram *program, int start) {
    HLSLEmitterContext context;
    bool skip_instruction[8] = {false};
    memset(&context, 0, sizeof(context));
    context.program = program;
    context.skip_instruction = skip_instruction;
    if (!analyze_d3dcompiler_model(&context)) return false;
    bool result = context.compiler_model.interleaved_projection_packs &&
        context.compiler_model.interleaved_projection_packs[start].valid &&
        context.compiler_model.interleaved_projection_packs[start]
                .scale_instruction == start &&
        context.compiler_model.interleaved_projection_packs[start]
                .pack_instruction == start + 1 &&
        context.compiler_model.interleaved_projection_packs[start]
                .add_instruction == start + 4 &&
        !skip_instruction[start] && skip_instruction[start + 1] &&
        skip_instruction[start + 4];
    free_d3dcompiler_model(&context);
    return result;
}

static int verify_interleaved_projection_pack_is_mutation_safe(void) {
    USILInstruction instructions[8];
    USILProgram program;
    init_interleaved_projection_pack_program(&program, instructions);
    CHECK(model_has_interleaved_projection_pack(&program, 0));

    instructions[1].operands[2].immediate_words[3] ^= 1u;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[0].operands[2].has_neg = true;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[1].operands[1].swizzle[2] = 2;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    init_register_operand(&instructions[2].operands[1],
                          OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[2].operands[1], 0);
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[2].operands[0].register_index = 1;
    instructions[2].operands[0].index_values[0] = 1u;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[4].operands[2].swizzle[3] = 2;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[5].operands[0].destination_mask = MASK_XY;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[5].opcode = USIL_OP_IF;
    instructions[5].operand_count = 1;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[2].opcode = USIL_OP_IF;
    instructions[2].operand_count = 1;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    init_interleaved_projection_pack_program(&program, instructions);
    instructions[4].operands[0].destination_mask = MASK_XY;
    CHECK(!model_has_interleaved_projection_pack(&program, 0));
    return 0;
}

static void init_split_matrix_transform_program(
    USILProgram *program, USILInstruction instructions[17]) {
    memset(instructions, 0, sizeof(*instructions) * 17u);

    init_instruction(&instructions[0], USIL_OP_MUL, 3);
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[0].destination_mask = MASK_XYZW;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_INPUT, 0);
    set_replicate(&instructions[0].operands[1], 1);
    init_cbuffer_operand(&instructions[0].operands[2], 2, 5);

    init_instruction(&instructions[1], USIL_OP_MAD, 4);
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[1].operands[0].destination_mask = MASK_XYZW;
    init_cbuffer_operand(&instructions[1].operands[1], 2, 4);
    init_register_operand(&instructions[1].operands[2], OPERAND_TYPE_INPUT, 0);
    set_replicate(&instructions[1].operands[2], 0);
    init_register_operand(&instructions[1].operands[3], OPERAND_TYPE_TEMP, 0);

    init_instruction(&instructions[2], USIL_OP_MAD, 4);
    init_register_operand(&instructions[2].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[2].operands[0].destination_mask = MASK_XYZW;
    init_cbuffer_operand(&instructions[2].operands[1], 2, 6);
    init_register_operand(&instructions[2].operands[2], OPERAND_TYPE_INPUT, 0);
    set_replicate(&instructions[2].operands[2], 2);
    init_register_operand(&instructions[2].operands[3], OPERAND_TYPE_TEMP, 0);

    init_instruction(&instructions[3], USIL_OP_ADD, 3);
    init_register_operand(&instructions[3].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[3].operands[0].destination_mask = MASK_XYZW;
    init_register_operand(&instructions[3].operands[1], OPERAND_TYPE_TEMP, 0);
    init_cbuffer_operand(&instructions[3].operands[2], 2, 7);

    init_instruction(&instructions[4], USIL_OP_MUL, 3);
    init_register_operand(&instructions[4].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[4].operands[0].destination_mask = MASK_XYZW;
    init_register_operand(&instructions[4].operands[1], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[4].operands[1], 1);
    init_cbuffer_operand(&instructions[4].operands[2], 3, 18);

    static const int clip_rows[3] = {17, 19, 20};
    static const int clip_components[3] = {0, 2, 3};
    for (int item = 0; item < 3; item++) {
        USILInstruction *mad = &instructions[5 + item];
        init_instruction(mad, USIL_OP_MAD, 4);
        init_register_operand(&mad->operands[0],
                              item == 2 ? OPERAND_TYPE_OUTPUT
                                        : OPERAND_TYPE_TEMP,
                              item == 2 ? 0 : 1);
        mad->operands[0].destination_mask = MASK_XYZW;
        init_cbuffer_operand(&mad->operands[1], 3, clip_rows[item]);
        init_register_operand(&mad->operands[2], OPERAND_TYPE_TEMP, 0);
        set_replicate(&mad->operands[2], clip_components[item]);
        init_register_operand(&mad->operands[3], OPERAND_TYPE_TEMP, 1);
    }

    /* Kill the old clip temporary one lane at a time.  These unrelated moves
     * make the positive case prove liveness, not mere adjacency. */
    for (int component = 0; component < 4; component++) {
        USILInstruction *move = &instructions[8 + component];
        init_instruction(move, USIL_OP_MOV, 2);
        init_register_operand(&move->operands[0], OPERAND_TYPE_TEMP, 0);
        move->operands[0].destination_mask = 16 << component;
        init_register_operand(&move->operands[1], OPERAND_TYPE_INPUT, 1);
        set_replicate(&move->operands[1], component);
    }

    init_instruction(&instructions[12], USIL_OP_MUL, 3);
    init_register_operand(&instructions[12].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[12].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[12].operands[1], OPERAND_TYPE_INPUT, 0);
    set_replicate(&instructions[12].operands[1], 1);
    init_cbuffer_operand(&instructions[12].operands[2], 2, 5);

    static const int world_rows[3] = {4, 6, 7};
    static const int world_components[3] = {0, 2, 3};
    for (int item = 0; item < 3; item++) {
        USILInstruction *mad = &instructions[13 + item];
        init_instruction(mad, USIL_OP_MAD, 4);
        init_register_operand(&mad->operands[0], OPERAND_TYPE_TEMP, 0);
        mad->operands[0].destination_mask = MASK_XYZ;
        init_cbuffer_operand(&mad->operands[1], 2, world_rows[item]);
        init_register_operand(&mad->operands[2], OPERAND_TYPE_INPUT, 0);
        set_replicate(&mad->operands[2], world_components[item]);
        init_register_operand(&mad->operands[3], OPERAND_TYPE_TEMP, 0);
    }
    init_instruction(&instructions[16], USIL_OP_RET, 0);
    init_program(program, instructions, 17, 2);
}

static int verify_split_matrix_transform_inverse_is_mutation_safe(void) {
    USILInstruction instructions[17];
    USILProgram program;
    int world = -1;
    init_split_matrix_transform_program(&program, instructions);
    CHECK(hlsl_compiler_split_matrix_transform_matches(&program, 0, &world));
    CHECK(world == 12);

    instructions[1].operands[1].rel_offset0 = 5;
    instructions[1].operands[1].index_values[1] = 5u;
    CHECK(!hlsl_compiler_split_matrix_transform_matches(&program, 0, &world));
    instructions[1].operands[1].rel_offset0 = 4;
    instructions[1].operands[1].index_values[1] = 4u;

    set_replicate(&instructions[15].operands[2], 0);
    CHECK(!hlsl_compiler_split_matrix_transform_matches(&program, 0, &world));
    set_replicate(&instructions[15].operands[2], 3);

    instructions[14].operands[1].rel_offset0 = 7;
    instructions[14].operands[1].index_values[1] = 7u;
    CHECK(!hlsl_compiler_split_matrix_transform_matches(&program, 0, &world));
    instructions[14].operands[1].rel_offset0 = 6;
    instructions[14].operands[1].index_values[1] = 6u;

    init_register_operand(&instructions[8].operands[1], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[8].operands[1], 1);
    CHECK(!hlsl_compiler_split_matrix_transform_matches(&program, 0, &world));
    init_register_operand(&instructions[8].operands[1], OPERAND_TYPE_INPUT, 1);
    set_replicate(&instructions[8].operands[1], 0);

    instructions[6].operands[3].register_index = 0;
    instructions[6].operands[3].index_values[0] = 0u;
    CHECK(!hlsl_compiler_split_matrix_transform_matches(&program, 0, &world));
    instructions[6].operands[3].register_index = 1;
    instructions[6].operands[3].index_values[0] = 1u;
    CHECK(hlsl_compiler_split_matrix_transform_matches(&program, 0, &world));
    return 0;
}

static void init_cross_product_order_program(
    USILProgram *program, USILInstruction instructions[4]) {
    memset(instructions, 0, sizeof(*instructions) * 4u);
    init_instruction(&instructions[0], USIL_OP_MUL, 3);
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[0].operands[1], 3);
    init_register_operand(&instructions[0].operands[2], OPERAND_TYPE_TEMP, 0);

    init_instruction(&instructions[1], USIL_OP_MUL, 3);
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 2);
    instructions[1].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 1);
    set_swizzle(&instructions[1].operands[1], 1, 2, 0, 1);
    init_register_operand(&instructions[1].operands[2], OPERAND_TYPE_TEMP, 0);
    set_swizzle(&instructions[1].operands[2], 2, 0, 1, 2);

    init_instruction(&instructions[2], USIL_OP_MAD, 4);
    init_register_operand(&instructions[2].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[2].operands[0].destination_mask = MASK_XYZ;
    init_register_operand(&instructions[2].operands[1], OPERAND_TYPE_TEMP, 0);
    set_swizzle(&instructions[2].operands[1], 1, 2, 0, 1);
    init_register_operand(&instructions[2].operands[2], OPERAND_TYPE_TEMP, 1);
    set_swizzle(&instructions[2].operands[2], 2, 0, 1, 2);
    init_register_operand(&instructions[2].operands[3], OPERAND_TYPE_TEMP, 2);
    instructions[2].operands[3].has_neg = true;
    init_instruction(&instructions[3], USIL_OP_RET, 0);
    init_program(program, instructions, 4, 3);
}

static int verify_cross_product_order_inverse_is_mutation_safe(void) {
    USILInstruction instructions[4];
    USILProgram program;
    init_cross_product_order_program(&program, instructions);
    CHECK(hlsl_compiler_cross_product_mul_reverses_source(&program, 1));
    CHECK(model_swaps_binary(&program, 1, true) == 0);

    instructions[2].operands[1].swizzle[0] = 0;
    CHECK(!hlsl_compiler_cross_product_mul_reverses_source(&program, 1));
    instructions[2].operands[1].swizzle[0] = 1;

    instructions[2].operands[3].has_neg = false;
    CHECK(!hlsl_compiler_cross_product_mul_reverses_source(&program, 1));
    instructions[2].operands[3].has_neg = true;

    instructions[0].operands[0].register_index = 3;
    instructions[0].operands[0].index_values[0] = 3u;
    CHECK(!hlsl_compiler_cross_product_mul_reverses_source(&program, 1));
    instructions[0].operands[0].register_index = 0;
    instructions[0].operands[0].index_values[0] = 0u;

    instructions[0].operands[0].destination_mask = MASK_XY;
    CHECK(!hlsl_compiler_cross_product_mul_reverses_source(&program, 1));
    instructions[0].operands[0].destination_mask = MASK_XYZ;

    instructions[1].saturate = true;
    CHECK(!hlsl_compiler_cross_product_mul_reverses_source(&program, 1));
    instructions[1].saturate = false;
    CHECK(hlsl_compiler_cross_product_mul_reverses_source(&program, 1));
    return 0;
}

static void init_quadratic_sh_program(USILProgram *program,
                                      USILInstruction instructions[8]) {
    memset(instructions, 0, sizeof(*instructions) * 8u);
    init_instruction(&instructions[0], USIL_OP_MUL, 3);
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[0].operands[1], 1);
    init_register_operand(&instructions[0].operands[2], OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[0].operands[2], 1);

    init_instruction(&instructions[1], USIL_OP_MAD, 4);
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[1].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[1].operands[1], 0);
    init_register_operand(&instructions[1].operands[2], OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[1].operands[2], 0);
    init_register_operand(&instructions[1].operands[3], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[1].operands[3], 3);
    instructions[1].operands[3].has_neg = true;

    init_instruction(&instructions[2], USIL_OP_MUL, 3);
    init_register_operand(&instructions[2].operands[0], OPERAND_TYPE_TEMP, 2);
    instructions[2].operands[0].destination_mask = MASK_XYZW;
    init_register_operand(&instructions[2].operands[1], OPERAND_TYPE_TEMP, 1);
    set_swizzle(&instructions[2].operands[1], 1, 2, 2, 0);
    init_register_operand(&instructions[2].operands[2], OPERAND_TYPE_TEMP, 1);
    set_swizzle(&instructions[2].operands[2], 0, 1, 2, 2);

    for (int component = 0; component < 3; component++) {
        USILInstruction *dot = &instructions[3 + component];
        init_instruction(dot, USIL_OP_DP4, 3);
        init_register_operand(&dot->operands[0], OPERAND_TYPE_TEMP, 3);
        dot->operands[0].destination_mask = 16 << component;
        init_cbuffer_operand(&dot->operands[1], 0, 42 + component);
        init_register_operand(&dot->operands[2], OPERAND_TYPE_TEMP, 2);
    }

    init_instruction(&instructions[6], USIL_OP_MAD, 4);
    init_register_operand(&instructions[6].operands[0], OPERAND_TYPE_OUTPUT, 0);
    instructions[6].operands[0].destination_mask = MASK_XYZ;
    init_cbuffer_operand(&instructions[6].operands[1], 0, 45);
    set_swizzle(&instructions[6].operands[1], 0, 1, 2, 0);
    init_register_operand(&instructions[6].operands[2], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[6].operands[2], 3);
    init_register_operand(&instructions[6].operands[3], OPERAND_TYPE_TEMP, 3);
    init_instruction(&instructions[7], USIL_OP_RET, 0);
    init_program(program, instructions, 8, 4);
}

static int model_preserves_vector_output(const USILProgram *program,
                                         int instruction,
                                         bool expected) {
    HLSLEmitterContext context;
    memset(&context, 0, sizeof(context));
    context.program = program;
    CHECK(analyze_d3dcompiler_model(&context));
    CHECK(compiler_model_preserves_vector_output(&context, instruction) ==
          expected);
    free_d3dcompiler_model(&context);
    return 0;
}

static int verify_quadratic_sh_inverse_is_mutation_safe(void) {
    USILInstruction instructions[8];
    USILProgram program;
    init_quadratic_sh_program(&program, instructions);
    CHECK(model_preserves_vector_output(&program, 6, true) == 0);

    instructions[4].operands[1].rel_offset0 = 44;
    instructions[4].operands[1].index_values[1] = 44u;
    CHECK(model_preserves_vector_output(&program, 6, false) == 0);
    instructions[4].operands[1].rel_offset0 = 43;
    instructions[4].operands[1].index_values[1] = 43u;

    instructions[1].operands[3].has_neg = false;
    CHECK(model_preserves_vector_output(&program, 6, false) == 0);
    instructions[1].operands[3].has_neg = true;

    instructions[2].operands[1].swizzle[0] = 0;
    CHECK(model_preserves_vector_output(&program, 6, false) == 0);
    instructions[2].operands[1].swizzle[0] = 1;

    instructions[6].operands[0].destination_mask = MASK_XY;
    CHECK(model_preserves_vector_output(&program, 6, false) == 0);
    return 0;
}

static int model_swaps_binary(const USILProgram *program, int instruction,
                              bool expected) {
    HLSLEmitterContext context;
    memset(&context, 0, sizeof(context));
    context.program = program;
    CHECK(analyze_d3dcompiler_model(&context));
    CHECK(compiler_model_swaps_binary_operands(&context, instruction) ==
          expected);
    free_d3dcompiler_model(&context);
    return 0;
}

static int verify_fresh_destination_order_is_mutation_safe(void) {
    USILInstruction instructions[3];
    USILProgram program;
    memset(instructions, 0, sizeof(instructions));
    init_instruction(&instructions[0], USIL_OP_SQRT, 2);
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[0].destination_mask = MASK_Y;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[0].operands[1], 1);
    init_instruction(&instructions[1], USIL_OP_MUL, 3);
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[1].operands[0].destination_mask = MASK_Y;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[1].operands[1], 0);
    init_register_operand(&instructions[1].operands[2], OPERAND_TYPE_TEMP, 0);
    set_replicate(&instructions[1].operands[2], 1);
    init_instruction(&instructions[2], USIL_OP_RET, 0);
    init_program(&program, instructions, 3, 2);
    CHECK(model_swaps_binary(&program, 1, true) == 0);

    instructions[0].operands[0].register_index = 2;
    instructions[0].operands[0].index_values[0] = 2u;
    program.temp_count = 3;
    CHECK(model_swaps_binary(&program, 1, false) == 0);

    init_instruction(&instructions[0], USIL_OP_MIN, 3);
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[0].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 6);
    set_replicate(&instructions[0].operands[1], 1);
    init_register_operand(&instructions[0].operands[2], OPERAND_TYPE_TEMP, 6);
    set_replicate(&instructions[0].operands[2], 0);
    init_instruction(&instructions[1], USIL_OP_MIN, 3);
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[1].operands[0].destination_mask = MASK_W;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 6);
    set_replicate(&instructions[1].operands[1], 2);
    init_register_operand(&instructions[1].operands[2], OPERAND_TYPE_TEMP, 1);
    set_replicate(&instructions[1].operands[2], 3);
    program.temp_count = 7;
    CHECK(model_swaps_binary(&program, 1, true) == 0);

    instructions[0].operands[0].register_index = 2;
    instructions[0].operands[0].index_values[0] = 2u;
    CHECK(model_swaps_binary(&program, 1, false) == 0);
    return 0;
}

int main(void) {
    CHECK(verify_tangent_frame_inverse_is_mutation_safe() == 0);
    CHECK(verify_screen_position_inverses_are_mutation_safe() == 0);
    CHECK(verify_interleaved_projection_pack_is_mutation_safe() == 0);
    CHECK(verify_split_matrix_transform_inverse_is_mutation_safe() == 0);
    CHECK(verify_cross_product_order_inverse_is_mutation_safe() == 0);
    CHECK(verify_quadratic_sh_inverse_is_mutation_safe() == 0);
    CHECK(verify_fresh_destination_order_is_mutation_safe() == 0);
    return 0;
}
