#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_parser.h"
#include "translation/usil.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int expected_operand_count(USILOpcode opcode) {
    switch (opcode) {
        case USIL_OP_NOP:
        case USIL_OP_ELSE:
        case USIL_OP_ENDIF:
        case USIL_OP_LOOP:
        case USIL_OP_ENDLOOP:
        case USIL_OP_DEFAULT:
        case USIL_OP_ENDSWITCH:
        case USIL_OP_BREAK:
        case USIL_OP_CONTINUE:
        case USIL_OP_RET:
        case USIL_OP_GEOMETRY_APPEND:
        case USIL_OP_GEOMETRY_RESTART_STRIP:
            return 0;
        case USIL_OP_IF:
        case USIL_OP_SWITCH:
        case USIL_OP_CASE:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUEC:
        case USIL_OP_DISCARD:
            return 1;
        case USIL_OP_MOV:
        case USIL_OP_RCP:
        case USIL_OP_RSQ:
        case USIL_OP_SQRT:
        case USIL_OP_NOT:
        case USIL_OP_FTOI:
        case USIL_OP_FTOU:
        case USIL_OP_ITOF:
        case USIL_OP_UTOF:
        case USIL_OP_LOG:
        case USIL_OP_EXP:
        case USIL_OP_SIN:
        case USIL_OP_COS:
        case USIL_OP_FRC:
        case USIL_OP_ROUND_NE:
        case USIL_OP_ROUND_NI:
        case USIL_OP_ROUND_PI:
        case USIL_OP_ROUND_Z:
        case USIL_OP_DERIV_RTX:
        case USIL_OP_DERIV_RTY:
        case USIL_OP_DERIV_RTX_COARSE:
        case USIL_OP_DERIV_RTY_COARSE:
        case USIL_OP_DERIV_RTX_FINE:
        case USIL_OP_DERIV_RTY_FINE:
        case USIL_OP_INEG:
        case USIL_OP_SAMPLEINFO:
            return 2;
        case USIL_OP_ADD:
        case USIL_OP_SUB:
        case USIL_OP_MUL:
        case USIL_OP_DIV:
        case USIL_OP_MIN:
        case USIL_OP_MAX:
        case USIL_OP_LT:
        case USIL_OP_GE:
        case USIL_OP_EQ:
        case USIL_OP_NE:
        case USIL_OP_ILT:
        case USIL_OP_IGE:
        case USIL_OP_IEQ:
        case USIL_OP_INE:
        case USIL_OP_ULT:
        case USIL_OP_UGE:
        case USIL_OP_AND:
        case USIL_OP_OR:
        case USIL_OP_XOR:
        case USIL_OP_ISHL:
        case USIL_OP_ISHR:
        case USIL_OP_USHR:
        case USIL_OP_IADD:
        case USIL_OP_IMAX:
        case USIL_OP_IMIN:
        case USIL_OP_UMAX:
        case USIL_OP_UMIN:
        case USIL_OP_DP2:
        case USIL_OP_DP3:
        case USIL_OP_DP4:
        case USIL_OP_LD:
        case USIL_OP_RESINFO:
        case USIL_OP_SINCOS:
            return 3;
        case USIL_OP_MAD:
        case USIL_OP_MOVC:
        case USIL_OP_IMAD:
        case USIL_OP_UBFE:
        case USIL_OP_IMUL:
        case USIL_OP_UDIV:
        case USIL_OP_LD_STRUCTURED:
        case USIL_OP_LD_MS:
        case USIL_OP_IMM_ATOMIC_IADD:
        case USIL_OP_LDMS:
        case USIL_OP_SAMPLE:
            return 4;
        case USIL_OP_SAMPLE_C:
        case USIL_OP_SAMPLE_C_LZ:
        case USIL_OP_SAMPLE_L:
        case USIL_OP_SAMPLE_B:
            return 5;
        case USIL_OP_SAMPLE_D:
            return 6;
        default:
            return -1;
    }
}

static void init_operand(DXBCOperand *operand, DXBCOperandType type,
                         int register_index, uint8_t destination_mask) {
    memset(operand, 0, sizeof(*operand));
    operand->type = type;
    operand->register_index = register_index;
    operand->register_index_dim = 1;
    operand->index_has_immediate[0] = true;
    operand->index_values[0] = (uint32_t)register_index;
    operand->destination_mask = destination_mask;
    operand->swizzle_mode = 1;
    operand->swizzle[0] = 0;
    operand->swizzle[1] = 1;
    operand->swizzle[2] = 2;
    operand->swizzle[3] = 3;
}

static void init_valid_instruction(USILInstruction *instruction,
                                   USILOpcode opcode) {
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = opcode;
    instruction->operand_count = expected_operand_count(opcode);
    for (int operand = 0; operand < instruction->operand_count; ++operand) {
        init_operand(&instruction->operands[operand], OPERAND_TYPE_TEMP,
                     operand, operand == 0 ? 0xf0u : 0u);
    }
    snprintf(instruction->resource_dimension,
             sizeof(instruction->resource_dimension), "2d");

    switch (opcode) {
        case USIL_OP_IMUL:
        case USIL_OP_UDIV:
        case USIL_OP_SINCOS:
            instruction->operands[1].destination_mask = 0xf0u;
            break;
        case USIL_OP_SAMPLE:
        case USIL_OP_SAMPLE_C:
        case USIL_OP_SAMPLE_C_LZ:
        case USIL_OP_SAMPLE_L:
        case USIL_OP_SAMPLE_D:
        case USIL_OP_SAMPLE_B:
            init_operand(&instruction->operands[2], OPERAND_TYPE_RESOURCE, 2,
                         0);
            init_operand(&instruction->operands[3], OPERAND_TYPE_SAMPLER, 3,
                         0);
            break;
        case USIL_OP_LD:
        case USIL_OP_LD_MS:
        case USIL_OP_LDMS:
            init_operand(&instruction->operands[2], OPERAND_TYPE_RESOURCE, 2,
                         0);
            if (opcode != USIL_OP_LD) {
                snprintf(instruction->resource_dimension,
                         sizeof(instruction->resource_dimension), "2dms");
            }
            break;
        case USIL_OP_LD_STRUCTURED:
            init_operand(&instruction->operands[3], OPERAND_TYPE_RESOURCE, 3,
                         0);
            break;
        case USIL_OP_RESINFO:
            init_operand(&instruction->operands[2], OPERAND_TYPE_RESOURCE, 2,
                         0);
            break;
        case USIL_OP_SAMPLEINFO:
            init_operand(&instruction->operands[1], OPERAND_TYPE_RESOURCE, 1,
                         0);
            break;
        case USIL_OP_IMM_ATOMIC_IADD:
            init_operand(&instruction->operands[1], OPERAND_TYPE_UAV, 1, 0);
            break;
        case USIL_OP_GEOMETRY_APPEND:
            instruction->geometry_effect = USIL_GEOMETRY_EFFECT_APPEND;
            break;
        case USIL_OP_GEOMETRY_RESTART_STRIP:
            instruction->geometry_effect =
                USIL_GEOMETRY_EFFECT_RESTART_STRIP;
            break;
        default:
            break;
    }
}

static int test_all_modeled_opcode_arities(void) {
    USILProgram program;
    memset(&program, 0, sizeof(program));

    for (int raw_opcode = USIL_OP_NOP;
         raw_opcode <= USIL_OP_GEOMETRY_RESTART_STRIP; ++raw_opcode) {
        const USILOpcode opcode = (USILOpcode)raw_opcode;
        const int count = expected_operand_count(opcode);
        CHECK(count >= 0 && count <= DXBC_MAX_OPERANDS);

        USILInstruction instruction;
        init_valid_instruction(&instruction, opcode);
        CHECK(usil_instruction_shape_valid(&program, &instruction));

        for (int operand = 0; operand < count; ++operand) {
            USILOperandUseInfo info = {USIL_OPERAND_USE_INVALID, 0};
            CHECK(usil_instruction_operand_use(&program, &instruction,
                                               operand, &info));
            CHECK(info.use != USIL_OPERAND_USE_INVALID);
            CHECK((info.use == USIL_OPERAND_USE_SOURCE) ==
                  (info.source_lane_mask != 0));
        }

        instruction.operand_count = count == 0 ? 1 : count - 1;
        CHECK(!usil_instruction_shape_valid(&program, &instruction));
        instruction.operand_count = count + 1;
        CHECK(!usil_instruction_shape_valid(&program, &instruction));
    }

    USILInstruction invalid;
    memset(&invalid, 0, sizeof(invalid));
    invalid.opcode = (USILOpcode)(USIL_OP_GEOMETRY_RESTART_STRIP + 1);
    CHECK(!usil_instruction_shape_valid(&program, &invalid));
    invalid.opcode = USIL_OP_MOV;
    invalid.operand_count = -1;
    CHECK(!usil_instruction_shape_valid(&program, &invalid));
    invalid.operand_count = DXBC_MAX_OPERANDS + 1;
    CHECK(!usil_instruction_shape_valid(&program, &invalid));
    return 0;
}

static int test_typed_uses_and_lane_authority(void) {
    USILProgram program;
    USILInstruction instruction;
    USILOperandUseInfo info;
    memset(&program, 0, sizeof(program));

    init_valid_instruction(&instruction, USIL_OP_MOV);
    instruction.operands[0].destination_mask = 0x50u;
    CHECK(usil_instruction_operand_use(&program, &instruction, 0, &info));
    CHECK(info.use == USIL_OPERAND_USE_DESTINATION &&
          info.source_lane_mask == 0);
    CHECK(usil_instruction_operand_use(&program, &instruction, 1, &info));
    CHECK(info.use == USIL_OPERAND_USE_SOURCE &&
          info.source_lane_mask == 0x5u);

    init_valid_instruction(&instruction, USIL_OP_DP3);
    CHECK(usil_instruction_operand_use(&program, &instruction, 1, &info));
    CHECK(info.use == USIL_OPERAND_USE_SOURCE &&
          info.source_lane_mask == 0x7u);

    init_valid_instruction(&instruction, USIL_OP_SINCOS);
    instruction.operands[0].destination_mask = 0x10u;
    instruction.operands[1].destination_mask = 0x80u;
    CHECK(usil_instruction_operand_use(&program, &instruction, 2, &info));
    CHECK(info.use == USIL_OPERAND_USE_SOURCE &&
          info.source_lane_mask == 0x9u);

    init_valid_instruction(&instruction, USIL_OP_SAMPLE_D);
    snprintf(instruction.resource_dimension,
             sizeof(instruction.resource_dimension), "cubearray");
    CHECK(usil_instruction_operand_use(&program, &instruction, 1, &info));
    CHECK(info.use == USIL_OPERAND_USE_SOURCE &&
          info.source_lane_mask == 0xfu);
    CHECK(usil_instruction_operand_use(&program, &instruction, 2, &info));
    CHECK(info.use == USIL_OPERAND_USE_RESOURCE_BINDING);
    CHECK(usil_instruction_operand_use(&program, &instruction, 3, &info));
    CHECK(info.use == USIL_OPERAND_USE_SAMPLER_BINDING);
    CHECK(usil_instruction_operand_use(&program, &instruction, 4, &info));
    CHECK(info.use == USIL_OPERAND_USE_SOURCE &&
          info.source_lane_mask == 0x7u);

    instruction.resource_dimension[0] = '\0';
    CHECK(!usil_instruction_shape_valid(&program, &instruction));
    USILTexture texture;
    memset(&texture, 0, sizeof(texture));
    texture.reg_idx = 2;
    snprintf(texture.dimension, sizeof(texture.dimension), "2darray");
    program.textures = &texture;
    program.texture_count = 1;
    CHECK(usil_instruction_shape_valid(&program, &instruction));
    CHECK(usil_instruction_operand_use(&program, &instruction, 1, &info));
    CHECK(info.source_lane_mask == 0x7u);

    init_valid_instruction(&instruction, USIL_OP_MOV);
    instruction.operands[0].type = OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL;
    instruction.operands[0].destination_mask = 0;
    CHECK(usil_instruction_operand_use(&program, &instruction, 1, &info));
    CHECK(info.source_lane_mask == 1u);

    info.use = USIL_OPERAND_USE_SOURCE;
    info.source_lane_mask = 0xfu;
    CHECK(!usil_instruction_operand_use(&program, &instruction, 2, &info));
    CHECK(info.use == USIL_OPERAND_USE_INVALID &&
          info.source_lane_mask == 0);
    return 0;
}

static int test_integer_multi_output_modifier_authority(void) {
    USILProgram program;
    USILInstruction instruction;
    memset(&program, 0, sizeof(program));

    init_valid_instruction(&instruction, USIL_OP_IMUL);
    CHECK(usil_instruction_shape_valid(&program, &instruction));
    instruction.operands[2].has_neg = true;
    CHECK(usil_instruction_shape_valid(&program, &instruction));
    instruction.operands[2].has_abs = true;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));
    instruction.operands[2].has_abs = false;
    instruction.saturate = true;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));

    init_valid_instruction(&instruction, USIL_OP_UDIV);
    CHECK(usil_instruction_shape_valid(&program, &instruction));
    instruction.operands[2].has_neg = true;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));
    instruction.operands[2].has_neg = false;
    instruction.operands[3].has_abs = true;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));
    instruction.operands[3].has_abs = false;
    instruction.saturate = true;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));
    return 0;
}

static int test_geometry_effect_shapes(void) {
    USILProgram program;
    USILInstruction instruction;
    USILOperandUseInfo info;
    memset(&program, 0, sizeof(program));

    init_valid_instruction(&instruction, USIL_OP_GEOMETRY_APPEND);
    CHECK(usil_instruction_shape_valid(&program, &instruction));
    instruction.geometry_stream_id = 1;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));

    instruction.geometry_stream_explicit = true;
    instruction.geometry_stream_id = 3;
    instruction.operand_count = 1;
    init_operand(&instruction.operands[0], OPERAND_TYPE_STREAM, 3, 0);
    CHECK(usil_instruction_shape_valid(&program, &instruction));
    CHECK(usil_instruction_operand_use(&program, &instruction, 0, &info));
    CHECK(info.use == USIL_OPERAND_USE_STREAM_SELECTOR &&
          info.source_lane_mask == 0);

    instruction.operands[0].index_values[0] = 2;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));
    instruction.operands[0].index_values[0] = 3;
    instruction.geometry_effect = USIL_GEOMETRY_EFFECT_RESTART_STRIP;
    CHECK(!usil_instruction_shape_valid(&program, &instruction));
    return 0;
}

static void write_le_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static size_t build_shader(const uint32_t *words, size_t word_count,
                           uint8_t *bytes, size_t capacity) {
    const size_t payload_size = (2u + word_count) * sizeof(uint32_t);
    const size_t total_size = 44u + payload_size;
    if (!words || !bytes || total_size > capacity || total_size > UINT32_MAX) {
        return 0;
    }
    memset(bytes, 0, total_size);
    memcpy(bytes, "DXBC", 4);
    write_le_u32(bytes + 20, 1);
    write_le_u32(bytes + 24, (uint32_t)total_size);
    write_le_u32(bytes + 28, 1);
    write_le_u32(bytes + 32, 36);
    memcpy(bytes + 36, "SHDR", 4);
    write_le_u32(bytes + 40, (uint32_t)payload_size);
    write_le_u32(bytes + 44, UINT32_C(0x00000050));
    write_le_u32(bytes + 48, (uint32_t)(2u + word_count));
    for (size_t word = 0; word < word_count; ++word) {
        write_le_u32(bytes + 52u + word * sizeof(uint32_t), words[word]);
    }
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, total_size, hash)) return 0;
    memcpy(bytes + 4, hash, sizeof(hash));
    return total_size;
}

static int test_eight_operand_parser_capacity(void) {
    CHECK(DXBC_MAX_OPERANDS == 8);
    const uint32_t immediate_token =
        1u | ((uint32_t)OPERAND_TYPE_IMMEDIATE32 << 12);
    uint32_t words[1u + 2u * 9u];
    uint8_t bytes[256];

    words[0] = 232u | (17u << 24);
    for (size_t operand = 0; operand < 8u; ++operand) {
        words[1u + operand * 2u] = immediate_token;
        words[2u + operand * 2u] = (uint32_t)operand;
    }
    size_t size = build_shader(words, 17, bytes, sizeof(bytes));
    CHECK(size > 0);
    DXBCContainer container;
    CHECK(dxbc_parse(&container, bytes, size));
    CHECK(container.instruction_count == 1);
    CHECK(container.instructions[0].opcode == 232u);
    CHECK(container.instructions[0].operand_count == DXBC_MAX_OPERANDS);
    USILProgram program;
    CHECK(!usil_translate(&program, &container));
    dxbc_free(&container);

    words[0] = 232u | (19u << 24);
    words[17] = immediate_token;
    words[18] = 8u;
    size = build_shader(words, 19, bytes, sizeof(bytes));
    CHECK(size > 0);
    CHECK(!dxbc_parse(&container, bytes, size));
    return 0;
}

int main(void) {
    CHECK(test_all_modeled_opcode_arities() == 0);
    CHECK(test_typed_uses_and_lane_authority() == 0);
    CHECK(test_integer_multi_output_modifier_authority() == 0);
    CHECK(test_geometry_effect_shapes() == 0);
    CHECK(test_eight_operand_parser_capacity() == 0);
    printf("USIL validation unit tests passed\n");
    return 0;
}
