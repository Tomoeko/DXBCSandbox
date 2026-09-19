// SPDX-License-Identifier: GPL-3.0-only

#include "translation/usil_validation.h"

#include <string.h>

uint8_t usil_operand_destination_lane_mask(const DXBCOperand *destination) {
    if (!destination || destination->type == OPERAND_TYPE_NULL) return 0;
    uint8_t mask = (uint8_t)(destination->destination_mask >> 4);
    if (mask == 0 && (destination->type == OPERAND_TYPE_OUTPUT_DEPTH ||
                      destination->type ==
                          OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL ||
                      destination->type ==
                          OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL)) {
        mask = 1;
    }
    return (uint8_t)(mask & 0x0fu);
}

int usil_operand_source_component(const DXBCOperand *operand, int lane) {
    if (!operand || lane < 0 || lane >= 4 || operand->swizzle_mode > 2) {
        return -1;
    }
    int component = lane;
    if (operand->swizzle_mode == 2) component = operand->swizzle[0];
    else if (operand->swizzle_mode == 1) component = operand->swizzle[lane];
    return component < 4 ? component : -1;
}

static bool texture_dimension_lanes(const USILProgram *program,
                                    const USILInstruction *instruction,
                                    int resource_operand,
                                    bool include_lod,
                                    uint8_t *lane_mask) {
    if (!program || !instruction || !lane_mask || resource_operand < 0 ||
        resource_operand >= instruction->operand_count) {
        return false;
    }
    const DXBCOperand *resource = &instruction->operands[resource_operand];
    if (resource->type != OPERAND_TYPE_RESOURCE) return false;
    const char *dimension = instruction->resource_dimension;
    if (!dimension[0]) {
        for (int index = 0; index < program->texture_count; ++index) {
            if (program->textures[index].reg_idx == resource->register_index) {
                dimension = program->textures[index].dimension;
                break;
            }
        }
    }

    unsigned int lanes = 0;
    bool has_mip_coordinate = true;
    if (strcmp(dimension, "buffer") == 0) {
        lanes = 1;
        has_mip_coordinate = false;
    } else if (strcmp(dimension, "1d") == 0) {
        lanes = 1;
    } else if (strcmp(dimension, "1darray") == 0) {
        lanes = 2;
    } else if (strcmp(dimension, "2d") == 0 ||
               strcmp(dimension, "2dms") == 0) {
        lanes = 2;
    } else if (strcmp(dimension, "2darray") == 0 ||
               strcmp(dimension, "2dmsarray") == 0 ||
               strcmp(dimension, "3d") == 0 ||
               strcmp(dimension, "cube") == 0) {
        lanes = 3;
    } else if (strcmp(dimension, "cubearray") == 0) {
        lanes = 4;
    } else {
        return false;
    }
    if (include_lod && has_mip_coordinate && lanes < 4) ++lanes;
    *lane_mask = (uint8_t)((1u << lanes) - 1u);
    return true;
}

static int regular_componentwise_operand_count(USILOpcode opcode) {
    switch (opcode) {
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
            return 3;
        case USIL_OP_MAD:
        case USIL_OP_MOVC:
        case USIL_OP_IMAD:
        case USIL_OP_UBFE:
            return 4;
        default:
            return -1;
    }
}

static bool geometry_shape_valid(const USILInstruction *instruction,
                                 int *operand_count) {
    const USILGeometryEffectKind expected =
        instruction->opcode == USIL_OP_GEOMETRY_APPEND
            ? USIL_GEOMETRY_EFFECT_APPEND
            : USIL_GEOMETRY_EFFECT_RESTART_STRIP;
    if (instruction->geometry_effect != expected ||
        instruction->geometry_stream_id > 3u) {
        return false;
    }
    if (!instruction->geometry_stream_explicit) {
        if (instruction->geometry_stream_id != 0u) return false;
        *operand_count = 0;
        return true;
    }
    *operand_count = 1;
    if (instruction->operand_count != 1) return true;
    const DXBCOperand *stream = &instruction->operands[0];
    return stream->type == OPERAND_TYPE_STREAM &&
           stream->register_index_dim == 1 &&
           stream->index_has_immediate[0] && !stream->rel_op0 &&
           !stream->rel_op1 && !stream->rel_op2 &&
           !stream->index_value_exceeds_int[0] &&
           stream->register_index >= 0 &&
           (uint32_t)stream->register_index == stream->index_values[0] &&
           stream->index_values[0] == instruction->geometry_stream_id;
}

static bool expected_operand_count(const USILInstruction *instruction,
                                   int *operand_count) {
    if (!instruction || !operand_count) return false;
    const int componentwise =
        regular_componentwise_operand_count(instruction->opcode);
    if (componentwise >= 0) {
        *operand_count = componentwise;
        return true;
    }
    switch (instruction->opcode) {
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
            *operand_count = 0;
            return true;
        case USIL_OP_IF:
        case USIL_OP_SWITCH:
        case USIL_OP_CASE:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUEC:
        case USIL_OP_DISCARD:
            *operand_count = 1;
            return true;
        case USIL_OP_SAMPLEINFO:
            *operand_count = 2;
            return true;
        case USIL_OP_DP2:
        case USIL_OP_DP3:
        case USIL_OP_DP4:
        case USIL_OP_LD:
        case USIL_OP_RESINFO:
        case USIL_OP_SINCOS:
            *operand_count = 3;
            return true;
        case USIL_OP_IMUL:
        case USIL_OP_UDIV:
        case USIL_OP_LD_STRUCTURED:
        case USIL_OP_LD_MS:
        case USIL_OP_IMM_ATOMIC_IADD:
        case USIL_OP_LDMS:
        case USIL_OP_SAMPLE:
            *operand_count = 4;
            return true;
        case USIL_OP_SAMPLE_C:
        case USIL_OP_SAMPLE_C_LZ:
        case USIL_OP_SAMPLE_L:
        case USIL_OP_SAMPLE_B:
            *operand_count = 5;
            return true;
        case USIL_OP_SAMPLE_D:
            *operand_count = 6;
            return true;
        case USIL_OP_GEOMETRY_APPEND:
        case USIL_OP_GEOMETRY_RESTART_STRIP:
            return geometry_shape_valid(instruction, operand_count);
        default:
            return false;
    }
}

static void set_use(USILOperandUseInfo *info, USILOperandUse use,
                    uint8_t source_lane_mask) {
    info->use = use;
    info->source_lane_mask = source_lane_mask;
}

static bool operand_use_unchecked(const USILProgram *program,
                                  const USILInstruction *instruction,
                                  int operand_index,
                                  USILOperandUseInfo *info) {
    const int componentwise =
        regular_componentwise_operand_count(instruction->opcode);
    if (componentwise >= 0) {
        if (operand_index == 0) {
            set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
            return true;
        }
        const uint8_t mask = usil_operand_destination_lane_mask(&instruction->operands[0]);
        if (mask == 0) return false;
        set_use(info, USIL_OPERAND_USE_SOURCE, mask);
        return true;
    }

    switch (instruction->opcode) {
        case USIL_OP_DP2:
        case USIL_OP_DP3:
        case USIL_OP_DP4:
            if (operand_index == 0) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
            } else {
                const uint8_t mask = instruction->opcode == USIL_OP_DP2
                                         ? 0x3u
                                         : instruction->opcode == USIL_OP_DP3
                                               ? 0x7u
                                               : 0xfu;
                set_use(info, USIL_OPERAND_USE_SOURCE, mask);
            }
            return true;

        case USIL_OP_IF:
        case USIL_OP_SWITCH:
        case USIL_OP_CASE:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUEC:
        case USIL_OP_DISCARD:
            set_use(info, USIL_OPERAND_USE_SOURCE, 1);
            return true;

        case USIL_OP_IMUL:
        case USIL_OP_UDIV:
        case USIL_OP_SINCOS:
            if (operand_index < 2) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
                return true;
            } else {
                const uint8_t mask =
                    usil_operand_destination_lane_mask(&instruction->operands[0]) |
                    usil_operand_destination_lane_mask(&instruction->operands[1]);
                if (mask == 0) return false;
                set_use(info, USIL_OPERAND_USE_SOURCE, mask);
                return true;
            }

        case USIL_OP_SAMPLE:
        case USIL_OP_SAMPLE_C:
        case USIL_OP_SAMPLE_C_LZ:
        case USIL_OP_SAMPLE_L:
        case USIL_OP_SAMPLE_D:
        case USIL_OP_SAMPLE_B:
            if (operand_index == 0) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
                return true;
            }
            if (operand_index == 1 ||
                (instruction->opcode == USIL_OP_SAMPLE_D &&
                 (operand_index == 4 || operand_index == 5))) {
                uint8_t mask = 0;
                if (!texture_dimension_lanes(program, instruction, 2, false,
                                             &mask)) {
                    return false;
                }
                if (operand_index >= 4 && mask == 0xfu) mask = 0x7u;
                set_use(info, USIL_OPERAND_USE_SOURCE, mask);
                return true;
            }
            if (operand_index == 2) {
                set_use(info, USIL_OPERAND_USE_RESOURCE_BINDING, 0);
                return true;
            }
            if (operand_index == 3) {
                set_use(info, USIL_OPERAND_USE_SAMPLER_BINDING, 0);
                return true;
            }
            set_use(info, USIL_OPERAND_USE_SOURCE, 1);
            return true;

        case USIL_OP_LD:
            if (operand_index == 0) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
                return true;
            }
            if (operand_index == 2) {
                set_use(info, USIL_OPERAND_USE_RESOURCE_BINDING, 0);
                return true;
            } else {
                uint8_t mask = 0;
                if (!texture_dimension_lanes(program, instruction, 2, true,
                                             &mask)) {
                    return false;
                }
                set_use(info, USIL_OPERAND_USE_SOURCE, mask);
                return true;
            }

        case USIL_OP_LD_MS:
        case USIL_OP_LDMS:
            if (operand_index == 0) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
                return true;
            }
            if (operand_index == 2) {
                set_use(info, USIL_OPERAND_USE_RESOURCE_BINDING, 0);
                return true;
            }
            if (operand_index == 3) {
                set_use(info, USIL_OPERAND_USE_SOURCE, 1);
                return true;
            } else {
                uint8_t mask = 0;
                if (!texture_dimension_lanes(program, instruction, 2, false,
                                             &mask)) {
                    return false;
                }
                set_use(info, USIL_OPERAND_USE_SOURCE, mask);
                return true;
            }

        case USIL_OP_LD_STRUCTURED:
            if (operand_index == 0) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
            } else if (operand_index == 3) {
                set_use(info, USIL_OPERAND_USE_RESOURCE_BINDING, 0);
            } else {
                set_use(info, USIL_OPERAND_USE_SOURCE, 1);
            }
            return true;

        case USIL_OP_RESINFO:
            if (operand_index == 0) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
            } else if (operand_index == 1) {
                set_use(info, USIL_OPERAND_USE_SOURCE, 1);
            } else {
                set_use(info, USIL_OPERAND_USE_RESOURCE_BINDING, 0);
            }
            return true;

        case USIL_OP_SAMPLEINFO:
            set_use(info, operand_index == 0
                              ? USIL_OPERAND_USE_DESTINATION
                              : USIL_OPERAND_USE_RESOURCE_BINDING,
                    0);
            return true;

        case USIL_OP_IMM_ATOMIC_IADD:
            if (operand_index == 0) {
                set_use(info, USIL_OPERAND_USE_DESTINATION, 0);
            } else if (operand_index == 1) {
                set_use(info, USIL_OPERAND_USE_RESOURCE_BINDING, 0);
            } else {
                set_use(info, USIL_OPERAND_USE_SOURCE, 1);
            }
            return true;

        case USIL_OP_GEOMETRY_APPEND:
        case USIL_OP_GEOMETRY_RESTART_STRIP:
            set_use(info, USIL_OPERAND_USE_STREAM_SELECTOR, 0);
            return true;

        default:
            return false;
    }
}

bool usil_instruction_shape_valid(const USILProgram *program,
                                  const USILInstruction *instruction) {
    if (!program || !instruction || instruction->operand_count < 0 ||
        instruction->operand_count > DXBC_MAX_OPERANDS) {
        return false;
    }
    int expected = 0;
    if (!expected_operand_count(instruction, &expected) ||
        instruction->operand_count != expected) {
        return false;
    }
    /* The SM4/5 integer multi-output encodings have no saturate form. IMUL
     * permits two's-complement negate on its sources but not floating-point
     * absolute value; UDIV permits neither modifier. Reject synthetic USIL
     * that HLSL could only approximate with a different instruction. */
    if (instruction->opcode == USIL_OP_IMUL &&
        (instruction->saturate || instruction->operands[2].has_abs ||
         instruction->operands[3].has_abs)) {
        return false;
    }
    if (instruction->opcode == USIL_OP_UDIV &&
        (instruction->saturate || instruction->operands[2].has_abs ||
         instruction->operands[2].has_neg ||
         instruction->operands[3].has_abs ||
         instruction->operands[3].has_neg)) {
        return false;
    }
    for (int operand = 0; operand < expected; ++operand) {
        USILOperandUseInfo info;
        if (!operand_use_unchecked(program, instruction, operand, &info) ||
            info.use == USIL_OPERAND_USE_INVALID ||
            (info.use == USIL_OPERAND_USE_SOURCE) !=
                (info.source_lane_mask != 0)) {
            return false;
        }
    }
    return true;
}

bool usil_instruction_operand_use(const USILProgram *program,
                                  const USILInstruction *instruction,
                                  int operand_index,
                                  USILOperandUseInfo *info) {
    if (info) {
        info->use = USIL_OPERAND_USE_INVALID;
        info->source_lane_mask = 0;
    }
    if (!info || !usil_instruction_shape_valid(program, instruction) ||
        operand_index < 0 || operand_index >= instruction->operand_count) {
        return false;
    }
    return operand_use_unchecked(program, instruction, operand_index, info);
}
