// SPDX-License-Identifier: GPL-3.0-only

#include "translation/usil_validation.h"

#include <limits.h>
#include <string.h>

static bool hull_temp_operand_is_declared(const DXBCOperand *operand, uint32_t count, unsigned *remaining) {
    if (!operand || !remaining || !*remaining) return false;
    --*remaining; /* Each operand node consumes at least one encoded token. */
    if (operand->type == OPERAND_TYPE_TEMP &&
        (operand->register_index < 0 || (uint32_t)operand->register_index >= count)) return false;
    const DXBCOperand *roots[3] = {operand->rel_op0, operand->rel_op1, operand->rel_op2};
    for (unsigned axis = 0; axis < 3; ++axis)
        if (roots[axis] && !hull_temp_operand_is_declared(roots[axis], count, remaining)) return false;
    return true;
}

bool usil_hull_phase_temp_registers_are_valid(const USILProgram *program) {
    if (!program || program->program_type != DXBC_PROGRAM_TYPE_HULL ||
        !program->has_stage_contract || !program->tessellation.valid ||
        program->instruction_count <= 0 || program->instruction_alloc < program->instruction_count ||
        !program->instructions || program->temp_count < 0 || program->temp_count > 4096 ||
        !program->tessellation.phase_count ||
        program->tessellation.phase_count > (size_t)program->instruction_count ||
        program->tessellation.phase_capacity < program->tessellation.phase_count || !program->tessellation.phases)
        return false;
    uint32_t maximum = 0;
    int next = 0;
    for (size_t phase = 0; phase < program->tessellation.phase_count; ++phase) {
        const USILHullPhase *scope = &program->tessellation.phases[phase];
        if (scope->first_instruction_index != next || scope->end_instruction_index <= next ||
            scope->end_instruction_index > program->instruction_count ||
            scope->marker_source_instruction_index >= scope->first_source_instruction_index ||
            scope->first_source_instruction_index >= scope->end_source_instruction_index ||
            (scope->has_temp_count ? scope->temp_count > 4096 ||
                scope->temp_count_source_instruction_index < scope->first_source_instruction_index ||
                scope->temp_count_source_instruction_index >= scope->end_source_instruction_index
                : scope->temp_count || scope->temp_count_source_instruction_index)) return false;
        if (scope->temp_count > maximum) maximum = scope->temp_count;
        for (int index = next; index < scope->end_instruction_index; ++index) {
            const USILInstruction *instruction = &program->instructions[index];
            if (instruction->source_instruction_index < scope->first_source_instruction_index ||
                instruction->source_instruction_index >= scope->end_source_instruction_index ||
                instruction->operand_count < 0 || instruction->operand_count > DXBC_MAX_OPERANDS) return false;
            for (int operand = 0; operand < instruction->operand_count; ++operand) {
                unsigned remaining = DXBC_MAX_NESTED_OPERAND_TOKENS;
                if (!hull_temp_operand_is_declared(&instruction->operands[operand], scope->temp_count, &remaining)) return false;
            }
        }
        next = scope->end_instruction_index;
    }
    return next == program->instruction_count && maximum == (uint32_t)program->temp_count;
}

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

typedef struct {
    int operand_count;
    int destination;
    int binding;
    int address;
    int byte_offset;
    int value;
    int compare;
    USILMemoryKind kind;
    bool flexible_kind;
    bool reads;
    bool writes;
    bool atomic;
} MemoryOpcodeShape;

static bool memory_opcode_shape(USILOpcode opcode, MemoryOpcodeShape *shape) {
    if (!shape) return false;
    switch (opcode) {
        case USIL_OP_LD_UAV_TYPED:
            *shape = (MemoryOpcodeShape){3, 0, 2, 1, -1, -1, -1, USIL_MEMORY_TYPED, false, true, false, false}; break;
        case USIL_OP_STORE_UAV_TYPED:
            *shape = (MemoryOpcodeShape){3, -1, 0, 1, -1, 2, -1, USIL_MEMORY_TYPED, false, false, true, false}; break;
        case USIL_OP_LD_RAW:
            *shape = (MemoryOpcodeShape){3, 0, 2, 1, -1, -1, -1, USIL_MEMORY_RAW, false, true, false, false}; break;
        case USIL_OP_STORE_RAW:
            *shape = (MemoryOpcodeShape){3, -1, 0, 1, -1, 2, -1, USIL_MEMORY_RAW, false, false, true, false}; break;
        case USIL_OP_LD_STRUCTURED:
            *shape = (MemoryOpcodeShape){4, 0, 3, 1, 2, -1, -1, USIL_MEMORY_STRUCTURED, false, true, false, false}; break;
        case USIL_OP_STORE_STRUCTURED:
            *shape = (MemoryOpcodeShape){4, -1, 0, 1, 2, 3, -1, USIL_MEMORY_STRUCTURED, false, false, true, false}; break;
        case USIL_OP_ATOMIC_XOR:
        case USIL_OP_ATOMIC_IADD:
            *shape = (MemoryOpcodeShape){3, -1, 0, 1, -1, 2, -1, USIL_MEMORY_RAW, true, true, true, true}; break;
        case USIL_OP_IMM_ATOMIC_IADD:
            *shape = (MemoryOpcodeShape){4, 0, 1, 2, -1, 3, -1, USIL_MEMORY_RAW, true, true, true, true}; break;
        case USIL_OP_IMM_ATOMIC_CMP_EXCH:
            *shape = (MemoryOpcodeShape){5, 0, 1, 2, -1, 4, 3, USIL_MEMORY_RAW, true, true, true, true}; break;
        case USIL_OP_IMM_ATOMIC_ALLOC:
        case USIL_OP_IMM_ATOMIC_CONSUME:
            *shape = (MemoryOpcodeShape){2, 0, 1, -1, -1, -1, -1, USIL_MEMORY_COUNTER, false, true, true, true}; break;
        default: return false;
    }
    return true;
}

bool usil_opcode_has_memory_access(USILOpcode opcode) {
    MemoryOpcodeShape shape;
    return memory_opcode_shape(opcode, &shape);
}

static bool memory_operand_base_valid(const DXBCOperand *operand) {
    return operand && !operand->has_abs && !operand->has_neg && operand->min_precision == 0u &&
           operand->extended_token_count == 0u && !operand->extended_tokens &&
           !operand->rel_op0 && !operand->rel_op1 && !operand->rel_op2;
}

static bool memory_static_indices_valid(const DXBCOperand *operand, int dimensions) {
    if (!memory_operand_base_valid(operand) || operand->register_index_dim != dimensions)
        return false;
    for (int index = 0; index < dimensions; ++index)
        if (!operand->index_has_immediate[index] || operand->index_value_exceeds_int[index] ||
            operand->index_representations[index] != 0u || operand->index_values[index] > INT_MAX)
            return false;
    return dimensions == 0 || (operand->register_index >= 0 &&
           operand->index_values[0] == (uint32_t)operand->register_index);
}

static bool memory_destination_valid(const USILProgram *program, const DXBCOperand *operand,
                                      bool scalar, uint8_t *lanes) {
    *lanes = usil_operand_destination_lane_mask(operand);
    if (operand->type == OPERAND_TYPE_NULL)
        return scalar && memory_static_indices_valid(operand, 0) && *lanes == 0u;
    return operand->type == OPERAND_TYPE_TEMP && memory_static_indices_valid(operand, 1) &&
           operand->register_index < program->temp_count && operand->swizzle_mode == 0u &&
           (operand->destination_mask & 0x0fu) == 0u && *lanes != 0u &&
           (!scalar || (*lanes & (*lanes - 1u)) == 0u);
}

static bool memory_source_valid(const USILProgram *program, const DXBCOperand *operand,
                                 uint8_t lanes) {
    if (!memory_operand_base_valid(operand) || lanes == 0u || operand->swizzle_mode > 2u)
        return false;
    uint8_t components = 0u;
    for (int lane = 0; lane < 4; ++lane) if ((lanes & (1u << lane)) != 0u) {
        const int component = usil_operand_source_component(operand, lane);
        if (component < 0) return false;
        components |= (uint8_t)(1u << component);
    }
    if (operand->type == OPERAND_TYPE_IMMEDIATE32) {
        if (!memory_static_indices_valid(operand, 0) ||
            operand->swizzle_mode != 0u || operand->destination_mask != 0u ||
            (operand->imm_value_count != 1 && operand->imm_value_count != 4) ||
            operand->immediate_word_count != operand->imm_value_count) return false;
        for (int index = 0; index < operand->imm_value_count; ++index)
            if (operand->immediate_words[index] != operand->imm_values[index]) return false;
        return true;
    }
    if (operand->destination_mask != 0u || operand->swizzle_mode == 0u) return false;
    if (operand->type == OPERAND_TYPE_TEMP)
        return memory_static_indices_valid(operand, 1) && operand->register_index < program->temp_count;
    if (operand->type == OPERAND_TYPE_CONSTANT_BUFFER) {
        if (!memory_static_indices_valid(operand, 2) || program->cbuffer_count < 0 ||
            program->cbuffer_count > 15 || program->cbuffer_count > program->cbuffer_alloc ||
            (program->cbuffer_count != 0 && !program->cbuffers))
            return false;
        for (int index = 0; index < program->cbuffer_count; ++index)
            if (program->cbuffers[index].reg_idx == operand->register_index)
                return program->cbuffers[index].size > 0 &&
                       operand->index_values[1] < (uint32_t)program->cbuffers[index].size;
        return false;
    }
    if (operand->type == OPERAND_TYPE_INPUT_THREAD_ID ||
        operand->type == OPERAND_TYPE_INPUT_THREAD_GROUP_ID ||
        operand->type == OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP ||
        operand->type == OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED) {
        if (!memory_static_indices_valid(operand, 0) || program->signature_declaration_count < 0 ||
            program->signature_declaration_count > 4 ||
            program->signature_declaration_count > program->signature_declaration_alloc ||
            (program->signature_declaration_count != 0 && !program->signature_declarations))
            return false;
        for (int index = 0; index < program->signature_declaration_count; ++index) {
            const USILSignatureDeclaration *declaration = &program->signature_declarations[index];
            if (declaration->operand_type == operand->type) {
                const uint8_t declared = operand->type == OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED
                    ? 1u : declaration->mask;
                return (components & ~declared) == 0u;
            }
        }
    }
    return false;
}

static bool memory_declaration(const USILProgram *program, const DXBCOperand *binding,
                                USILMemoryAccess *access) {
    if (!memory_static_indices_valid(binding, 1)) return false;
    access->register_id = (uint32_t)binding->register_index;
    if (binding->type == OPERAND_TYPE_RESOURCE) {
        if (access->register_id >= 128u || program->texture_count < 0 || program->texture_count > 128 ||
            program->texture_count > program->texture_alloc ||
            (program->texture_count != 0 && !program->textures)) return false;
        bool found = false;
        for (int index = 0; index < program->texture_count; ++index) {
            const USILTexture *texture = &program->textures[index];
            if (texture->reg_idx != binding->register_index) continue;
            if (found) return false;
            found = true;
            access->space = USIL_MEMORY_SHADER_RESOURCE;
            memcpy(access->dimension, texture->dimension, sizeof(access->dimension));
            memcpy(access->return_types, texture->return_types, sizeof(access->return_types));
            if (texture->stride < 0) return false;
            access->byte_stride = (uint32_t)texture->stride;
        }
        if (!found) return false;
    } else if (binding->type == OPERAND_TYPE_UAV) {
        if (access->register_id >= 8u || program->uav_count < 0 || program->uav_count > 8 ||
            program->uav_count > program->uav_alloc ||
            (program->uav_count != 0 && !program->uavs)) return false;
        bool found = false;
        for (int index = 0; index < program->uav_count; ++index) {
            const USILUav *uav = &program->uavs[index];
            if (uav->reg_idx != binding->register_index) continue;
            if (found) return false;
            found = true;
            access->space = USIL_MEMORY_UNORDERED_ACCESS;
            memcpy(access->dimension, uav->dimension, sizeof(access->dimension));
            memcpy(access->return_types, uav->return_types, sizeof(access->return_types));
            access->globally_coherent = uav->globally_coherent;
            access->rasterizer_ordered = uav->rasterizer_ordered;
            access->has_order_preserving_counter = uav->has_order_preserving_counter;
            if (uav->stride < 0) return false;
            access->byte_stride = (uint32_t)uav->stride;
        }
        if (!found) return false;
    } else if (binding->type == OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY) {
        if (access->register_id >= 8192u ||
            program->compute.shared_memory_count > program->compute.shared_memory_capacity ||
            program->compute.shared_memory_count > 8192u ||
            (program->compute.shared_memory_count != 0u && !program->compute.shared_memory))
            return false;
        bool found = false;
        for (size_t index = 0; index < program->compute.shared_memory_count; ++index) {
            const DXBCThreadGroupSharedMemoryContract *memory = &program->compute.shared_memory[index];
            if (memory->register_id != access->register_id) continue;
            if (found) return false;
            if (memory->byte_count == 0u || memory->byte_count > 32768u ||
                (memory->byte_count & 3u) != 0u ||
                (memory->structured &&
                 (memory->element_count == 0u || memory->byte_stride == 0u ||
                  (uint64_t)memory->byte_stride * memory->element_count != memory->byte_count)) ||
                (!memory->structured && (memory->byte_stride != 0u || memory->element_count != 0u)))
                return false;
            found = true;
            access->space = USIL_MEMORY_THREAD_GROUP;
            strcpy(access->dimension, memory->structured ? "structured" : "raw");
            access->byte_stride = memory->byte_stride;
            access->shared_memory_byte_count = memory->byte_count;
        }
        if (!found) return false;
    } else return false;
    if (!memchr(access->dimension, '\0', sizeof(access->dimension))) return false;
    if (strcmp(access->dimension, "raw") == 0) {
        access->kind = USIL_MEMORY_RAW;
        return access->byte_stride == 0u;
    }
    if (strcmp(access->dimension, "structured") == 0) {
        access->kind = USIL_MEMORY_STRUCTURED;
        return access->byte_stride != 0u && (access->byte_stride & 3u) == 0u &&
               (access->space == USIL_MEMORY_THREAD_GROUP || access->byte_stride <= 2048u);
    }
    access->kind = USIL_MEMORY_TYPED;
    return access->space == USIL_MEMORY_UNORDERED_ACCESS && access->byte_stride == 0u &&
           !access->has_order_preserving_counter;
}

static uint8_t typed_memory_address_lanes(const char *dimension) {
    if (strcmp(dimension, "buffer") == 0 || strcmp(dimension, "1d") == 0) return 1u;
    if (strcmp(dimension, "2d") == 0 || strcmp(dimension, "1darray") == 0) return 3u;
    if (strcmp(dimension, "3d") == 0 || strcmp(dimension, "2darray") == 0) return 7u;
    return 0u;
}

bool usil_instruction_memory_access(const USILProgram *program,
                                    const USILInstruction *instruction,
                                    USILMemoryAccess *out_access) {
    if (!out_access) return false;
    memset(out_access, 0, sizeof(*out_access));
    MemoryOpcodeShape shape;
    if (!program || !instruction || !program->has_stage_contract || !program->compute.valid ||
        program->program_type != DXBC_PROGRAM_TYPE_COMPUTE || program->shader_model_major != 5u ||
        program->temp_count < 0 || program->temp_count > 4096 ||
        program->shader_model_minor != 0u || !memory_opcode_shape(instruction->opcode, &shape) ||
        instruction->operand_count != shape.operand_count || instruction->saturate ||
        instruction->precise_mask != 0u || instruction->sync_flags != 0u ||
        instruction->condition_test != DXBC_INSTRUCTION_TEST_NONE || instruction->has_texel_offset ||
        instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE ||
        !memchr(instruction->resource_dimension, '\0', sizeof(instruction->resource_dimension))) return false;
    USILMemoryAccess access = {0};
    access.destination_operand = shape.destination;
    access.binding_operand = shape.binding;
    access.address_operand = shape.address;
    access.byte_offset_operand = shape.byte_offset;
    access.value_operand = shape.value;
    access.compare_operand = shape.compare;
    access.reads = shape.reads;
    access.writes = shape.writes;
    access.atomic = shape.atomic;
    const DXBCOperand *binding = &instruction->operands[shape.binding];
    if (!memory_declaration(program, binding, &access) ||
        (shape.writes && access.space == USIL_MEMORY_SHADER_RESOURCE) ||
        (!shape.flexible_kind && shape.kind != USIL_MEMORY_COUNTER && shape.kind != access.kind))
        return false;
    if (shape.kind == USIL_MEMORY_COUNTER) {
        if (access.kind != USIL_MEMORY_STRUCTURED || access.space != USIL_MEMORY_UNORDERED_ACCESS ||
            binding->swizzle_mode != 0u || binding->destination_mask != 0u)
            return false;
        /* The declaration flag distinguishes permanent COUNTER indices from
         * APPEND indices scoped to an invocation. Both support alloc/consume;
         * the latter's required API flag follows from the opcode itself. */
        access.counter_mode = access.has_order_preserving_counter
            ? USIL_COUNTER_ORDER_PRESERVING : USIL_COUNTER_APPEND_INVOCATION;
        access.kind = USIL_MEMORY_COUNTER;
    }
    if (access.kind == USIL_MEMORY_TYPED) {
        access.address_lanes = typed_memory_address_lanes(access.dimension);
        if (access.address_lanes == 0u) return false;
        for (int component = 0; component < 4; ++component)
            if (access.return_types[component] == 0u || access.return_types[component] > 9u ||
                (shape.atomic && access.return_types[component] != 3u && access.return_types[component] != 4u))
                return false;
    } else if (shape.address >= 0) {
        access.address_lanes = shape.atomic && access.kind == USIL_MEMORY_STRUCTURED ? 3u : 1u;
    }
    if (shape.destination >= 0 &&
        !memory_destination_valid(program, &instruction->operands[shape.destination], shape.atomic,
                                   &access.destination_lanes)) return false;
    if (shape.atomic) {
        if (binding->swizzle_mode != 0u || binding->destination_mask != 0u) return false;
        access.value_lanes = shape.value >= 0 ? 1u : 0u;
        access.memory_component_lanes = 1u;
    } else if (shape.writes) {
        access.value_lanes = usil_operand_destination_lane_mask(binding);
        access.memory_component_lanes = access.value_lanes;
        if (binding->swizzle_mode != 0u || (binding->destination_mask & 0x0fu) != 0u ||
            access.value_lanes == 0u) return false;
    } else {
        if (binding->swizzle_mode == 0u || binding->destination_mask != 0u) return false;
        for (int lane = 0; lane < 4; ++lane) if ((access.destination_lanes & (1u << lane)) != 0u) {
            const int component = usil_operand_source_component(binding, lane);
            if (component < 0) return false;
            access.memory_component_lanes |= (uint8_t)(1u << component);
        }
    }
    if ((shape.address >= 0 && !memory_source_valid(program, &instruction->operands[shape.address], access.address_lanes)) ||
        (shape.byte_offset >= 0 && !memory_source_valid(program, &instruction->operands[shape.byte_offset], 1u)) ||
        (shape.value >= 0 && !memory_source_valid(program, &instruction->operands[shape.value], access.value_lanes)) ||
        (shape.compare >= 0 && !memory_source_valid(program, &instruction->operands[shape.compare], 1u)))
        return false;
    if ((instruction->resource_dimension[0] && strcmp(instruction->resource_dimension, access.dimension) != 0) ||
        (instruction->has_resource_dimension && instruction->resource_stride != access.byte_stride) ||
        (!instruction->has_resource_dimension && instruction->resource_stride != 0u))
        return false;
    if (instruction->has_resource_return_types) {
        for (int component = 0; component < 4; ++component) {
            const uint8_t expected = access.kind == USIL_MEMORY_TYPED ? access.return_types[component] : 6u;
            if (instruction->resource_return_types[component] != expected) return false;
        }
    }
    *out_access = access;
    return true;
}

static bool expected_operand_count(const USILInstruction *instruction,
                                   int *operand_count) {
    if (!instruction || !operand_count) return false;
    MemoryOpcodeShape memory;
    if (memory_opcode_shape(instruction->opcode, &memory)) {
        *operand_count = memory.operand_count;
        return true;
    }
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
        case USIL_OP_SYNC:
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
    if (usil_opcode_has_memory_access(instruction->opcode) &&
        program->program_type == DXBC_PROGRAM_TYPE_COMPUTE) {
        USILMemoryAccess access;
        if (!usil_instruction_memory_access(program, instruction, &access)) return false;
        if (operand_index == access.destination_operand)
            set_use(info, USIL_OPERAND_USE_DESTINATION, 0u);
        else if (operand_index == access.binding_operand)
            set_use(info, USIL_OPERAND_USE_RESOURCE_BINDING, 0u);
        else if (operand_index == access.address_operand)
            set_use(info, USIL_OPERAND_USE_SOURCE, access.address_lanes);
        else if (operand_index == access.value_operand)
            set_use(info, USIL_OPERAND_USE_SOURCE, access.value_lanes);
        else if (operand_index == access.byte_offset_operand || operand_index == access.compare_operand)
            set_use(info, USIL_OPERAND_USE_SOURCE, 1u);
        else return false;
        return true;
    }
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
    if (usil_opcode_has_memory_access(instruction->opcode) &&
        (program->program_type == DXBC_PROGRAM_TYPE_COMPUTE ||
         (instruction->opcode != USIL_OP_LD_STRUCTURED &&
          instruction->opcode != USIL_OP_IMM_ATOMIC_IADD))) {
        USILMemoryAccess access;
        return usil_instruction_memory_access(program, instruction, &access);
    }
    int expected = 0;
    if (!expected_operand_count(instruction, &expected) ||
        instruction->operand_count != expected) {
        return false;
    }
    if (instruction->opcode == USIL_OP_SYNC) {
        if (!program->has_stage_contract ||
            program->program_type != DXBC_PROGRAM_TYPE_COMPUTE ||
            !program->compute.valid || instruction->saturate ||
            instruction->precise_mask != 0u ||
            instruction->condition_test != DXBC_INSTRUCTION_TEST_NONE ||
            instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE ||
            instruction->has_resource_dimension || instruction->resource_dimension[0] ||
            instruction->resource_stride != 0u || instruction->has_texel_offset ||
            instruction->has_resource_return_types ||
            (instruction->sync_flags & ~0xfu) != 0u ||
            (instruction->sync_flags & 0xeu) == 0u ||
            (instruction->sync_flags & 0xcu) == 0xcu) return false;
    } else if (instruction->sync_flags != 0u) {
        return false;
    }
    if (instruction->opcode == USIL_OP_NOP && instruction->saturate) return false;
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

bool usil_instruction_effects(const USILProgram *program,
                               const USILInstruction *instruction,
                               USILEffectFlags *out_effects) {
    if (!out_effects) return false;
    *out_effects = USIL_EFFECT_UNKNOWN;
    if (!usil_instruction_shape_valid(program, instruction)) return false;
    if (usil_opcode_has_memory_access(instruction->opcode) &&
        program->program_type == DXBC_PROGRAM_TYPE_COMPUTE) {
        USILMemoryAccess access;
        if (!usil_instruction_memory_access(program, instruction, &access)) return false;
        USILEffectFlags memory_effects = USIL_EFFECT_NONE;
        if (access.reads)
            memory_effects |= access.space == USIL_MEMORY_THREAD_GROUP
                ? USIL_EFFECT_THREAD_GROUP_READ : USIL_EFFECT_RESOURCE_READ;
        if (access.writes)
            memory_effects |= access.space == USIL_MEMORY_THREAD_GROUP
                ? USIL_EFFECT_THREAD_GROUP_WRITE : USIL_EFFECT_EXTERNAL_WRITE;
        if (access.atomic) memory_effects |= USIL_EFFECT_ATOMIC;
        if (access.kind == USIL_MEMORY_COUNTER) memory_effects |= USIL_EFFECT_COUNTER;
        *out_effects = memory_effects;
        return true;
    }
    USILEffectFlags effects = USIL_EFFECT_NONE;
    switch (instruction->opcode) {
        case USIL_OP_DERIV_RTX: case USIL_OP_DERIV_RTY:
        case USIL_OP_DERIV_RTX_COARSE: case USIL_OP_DERIV_RTY_COARSE:
        case USIL_OP_DERIV_RTX_FINE: case USIL_OP_DERIV_RTY_FINE:
            effects = USIL_EFFECT_QUAD_CONTEXT; break;
        case USIL_OP_SAMPLE: case USIL_OP_SAMPLE_B: case USIL_OP_SAMPLE_C:
            effects = USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_QUAD_CONTEXT; break;
        case USIL_OP_SAMPLE_L: case USIL_OP_SAMPLE_D: case USIL_OP_SAMPLE_C_LZ:
        case USIL_OP_LD: case USIL_OP_LD_STRUCTURED: case USIL_OP_LD_MS:
        case USIL_OP_LDMS: case USIL_OP_RESINFO: case USIL_OP_SAMPLEINFO:
            effects = USIL_EFFECT_RESOURCE_READ; break;
        case USIL_OP_IMM_ATOMIC_IADD:
            effects = USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_EXTERNAL_WRITE; break;
        case USIL_OP_GEOMETRY_APPEND: case USIL_OP_GEOMETRY_RESTART_STRIP:
            effects = USIL_EFFECT_GEOMETRY_OUTPUT; break;
        case USIL_OP_SYNC:
            effects = USIL_EFFECT_MEMORY_BARRIER;
            if ((instruction->sync_flags & DXBC_SYNC_THREADS_IN_GROUP) != 0u)
                effects |= USIL_EFFECT_CONTROL;
            break;
        case USIL_OP_DISCARD:
            effects = USIL_EFFECT_CONTROL | USIL_EFFECT_QUAD_CONTEXT; break;
        case USIL_OP_IF: case USIL_OP_ELSE: case USIL_OP_ENDIF:
        case USIL_OP_LOOP: case USIL_OP_ENDLOOP: case USIL_OP_SWITCH:
        case USIL_OP_CASE: case USIL_OP_DEFAULT: case USIL_OP_ENDSWITCH:
        case USIL_OP_BREAK: case USIL_OP_BREAKC: case USIL_OP_CONTINUE:
        case USIL_OP_CONTINUEC: case USIL_OP_RET:
            effects = USIL_EFFECT_CONTROL; break;
        case USIL_OP_NOP: case USIL_OP_DP2: case USIL_OP_DP3: case USIL_OP_DP4:
        case USIL_OP_IMUL: case USIL_OP_UDIV: case USIL_OP_SINCOS:
            break;
        default:
            if (regular_componentwise_operand_count(instruction->opcode) < 0) return false;
            break;
    }
    *out_effects = effects;
    return true;
}
