// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLSL_XYZ_MASK (16 | 32 | 64)
#define HLSL_XYZW_MASK (16 | 32 | 64 | 128)

enum {
    HLSL_COMPILER_SCREEN_COMBINED = 0,
    HLSL_COMPILER_SCREEN_SPLIT = 1,
    HLSL_COMPILER_SCREEN_INPLACE = 2,
    HLSL_COMPILER_SCREEN_INPLACE_SCRATCH = 3,
};

static bool compiler_screen_pattern_is_inplace(unsigned char pattern) {
    return pattern == HLSL_COMPILER_SCREEN_INPLACE ||
           pattern == HLSL_COMPILER_SCREEN_INPLACE_SCRATCH;
}

static bool compiler_plain_operand(const DXBCOperand *operand) {
    return operand && !operand->has_abs && !operand->has_neg &&
           operand->min_precision == 0 && !operand->rel_op0 &&
           !operand->rel_op1 && !operand->rel_op2;
}

static bool compiler_instruction_shape(const USILInstruction *instruction,
                                       USILOpcode opcode,
                                       int operand_count) {
    return instruction && instruction->opcode == opcode &&
           instruction->operand_count == operand_count &&
           !instruction->saturate && instruction->precise_mask == 0;
}

static bool compiler_destination(const DXBCOperand *operand,
                                 DXBCOperandType type, int reg, int mask) {
    return compiler_plain_operand(operand) && operand->type == type &&
           operand->register_index == reg &&
           operand->destination_mask == mask;
}

static bool compiler_scalar_source(const DXBCOperand *operand,
                                   DXBCOperandType type, int reg,
                                   int component) {
    return compiler_plain_operand(operand) && operand->type == type &&
           operand->register_index == reg &&
           is_replicate_swizzle(operand) &&
           usil_operand_source_component(operand, 0) == component;
}

static bool compiler_vector_source(const DXBCOperand *operand,
                                   DXBCOperandType type, int reg,
                                   const int components[3]) {
    if (!compiler_plain_operand(operand) || operand->type != type ||
        operand->register_index != reg || operand->swizzle_mode != 1)
        return false;
    for (int lane = 0; lane < 3; lane++) {
        if (operand->swizzle[lane] != components[lane]) return false;
    }
    return true;
}

static bool compiler_negated_vector_source(const DXBCOperand *operand,
                                           DXBCOperandType type, int reg,
                                           const int components[3]) {
    if (!operand || !operand->has_neg || operand->has_abs) return false;
    DXBCOperand positive = *operand;
    positive.has_neg = false;
    return compiler_vector_source(&positive, type, reg, components);
}

static bool compiler_vector4_source(const DXBCOperand *operand,
                                    DXBCOperandType type, int reg,
                                    const int components[4]) {
    if (!compiler_plain_operand(operand) || operand->type != type ||
        operand->register_index != reg || operand->swizzle_mode != 1)
        return false;
    for (int lane = 0; lane < 4; lane++) {
        if (operand->swizzle[lane] != components[lane]) return false;
    }
    return true;
}

static bool compiler_static_cbuffer_row(const DXBCOperand *operand,
                                        int buffer, int row,
                                        const int components[3]) {
    return compiler_vector_source(operand, OPERAND_TYPE_CONSTANT_BUFFER,
                                  buffer, components) &&
           operand->register_index_dim == 2 &&
           operand->index_has_immediate[0] &&
           operand->index_has_immediate[1] &&
           !operand->index_value_exceeds_int[0] &&
           !operand->index_value_exceeds_int[1] &&
           operand->rel_offset0 == row;
}

static bool compiler_static_cbuffer_row4(const DXBCOperand *operand,
                                         int buffer, int row,
                                         const int components[4]) {
    return compiler_vector4_source(operand, OPERAND_TYPE_CONSTANT_BUFFER,
                                   buffer, components) &&
           operand->register_index_dim == 2 &&
           operand->index_has_immediate[0] &&
           operand->index_has_immediate[1] &&
           !operand->index_value_exceeds_int[0] &&
           !operand->index_value_exceeds_int[1] &&
           operand->rel_offset0 == row;
}

static bool compiler_mov_scalar(const USILInstruction *instruction,
                                int output_reg, int output_lane,
                                int source_reg, int source_lane) {
    return compiler_instruction_shape(instruction, USIL_OP_MOV, 2) &&
           compiler_destination(&instruction->operands[0],
                                OPERAND_TYPE_OUTPUT, output_reg,
                                16 << output_lane) &&
           compiler_scalar_source(&instruction->operands[1],
                                  OPERAND_TYPE_TEMP, source_reg,
                                  source_lane);
}

/* Exact inverse of D3DCompiler's packed tangent-frame lowering.  This is not
 * a source semantic or a Unity-name heuristic: every arithmetic edge, lane
 * permutation, sign dependency, and output lane is proven from the decoded
 * instruction stream.  Grouping the physical operations into logical
 * float3 values is required for D3DCompiler to choose the serialized lane
 * allocation again. */
static bool compiler_tangent_frame_matches(
    const USILProgram *program, int start,
    HLSLCompilerTangentFrame *out_frame) {
    static const int identity[3] = {0, 1, 2};
    if (!program || !out_frame || start < 0 ||
        start + 19 >= program->instruction_count)
        return false;
    const USILInstruction *instruction = program->instructions + start;
    if (!compiler_instruction_shape(&instruction[0], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[1], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&instruction[2], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&instruction[3], USIL_OP_DP3, 3) ||
        !compiler_instruction_shape(&instruction[4], USIL_OP_RSQ, 2) ||
        !compiler_instruction_shape(&instruction[5], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[6], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[7], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&instruction[8], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[9], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[19], USIL_OP_RET, 0))
        return false;

    const int tangent_reg = instruction[0].operands[0].register_index;
    const int input_reg = instruction[0].operands[1].register_index;
    const int matrix_buffer = instruction[0].operands[2].register_index;
    if (tangent_reg < 0 || input_reg < 0 || matrix_buffer < 0 ||
        !compiler_destination(&instruction[0].operands[0],
                              OPERAND_TYPE_TEMP, tangent_reg,
                              HLSL_XYZ_MASK) ||
        instruction[0].operands[1].type != OPERAND_TYPE_INPUT ||
        instruction[0].operands[2].type !=
            OPERAND_TYPE_CONSTANT_BUFFER)
        return false;

    int permutation[3];
    if (!compiler_plain_operand(&instruction[0].operands[2]) ||
        instruction[0].operands[2].swizzle_mode != 1)
        return false;
    for (int lane = 0; lane < 3; lane++) {
        permutation[lane] = instruction[0].operands[2].swizzle[lane];
        if (permutation[lane] < 0 || permutation[lane] > 2) return false;
        for (int previous = 0; previous < lane; previous++) {
            if (permutation[previous] == permutation[lane]) return false;
        }
    }

    int input_components[3];
    const DXBCOperand *matrix_rows[3] = {
        &instruction[0].operands[2], &instruction[1].operands[1],
        &instruction[2].operands[1]};
    const DXBCOperand *input_scalars[3] = {
        &instruction[0].operands[1], &instruction[1].operands[2],
        &instruction[2].operands[2]};
    for (int item = 0; item < 3; item++) {
        if (!compiler_plain_operand(input_scalars[item]) ||
            input_scalars[item]->type != OPERAND_TYPE_INPUT ||
            input_scalars[item]->register_index != input_reg ||
            !is_replicate_swizzle(input_scalars[item]))
            return false;
        input_components[item] =
            usil_operand_source_component(input_scalars[item], 0);
        if (input_components[item] < 0 || input_components[item] > 2 ||
            !compiler_static_cbuffer_row(matrix_rows[item], matrix_buffer,
                                         input_components[item],
                                         permutation))
            return false;
        for (int previous = 0; previous < item; previous++) {
            if (input_components[previous] == input_components[item])
                return false;
        }
        if (!compiler_destination(&instruction[item].operands[0],
                                  OPERAND_TYPE_TEMP, tangent_reg,
                                  HLSL_XYZ_MASK))
            return false;
        if (item > 0 &&
            (!compiler_vector_source(&instruction[item].operands[3],
                                     OPERAND_TYPE_TEMP, tangent_reg,
                                     identity)))
            return false;
    }

    const int scalar_reg = instruction[3].operands[0].register_index;
    const int scalar_lane = 3;
    if (scalar_reg < 0 ||
        !compiler_destination(&instruction[3].operands[0],
                              OPERAND_TYPE_TEMP, scalar_reg,
                              16 << scalar_lane) ||
        !compiler_vector_source(&instruction[3].operands[1],
                                OPERAND_TYPE_TEMP, tangent_reg, identity) ||
        !compiler_vector_source(&instruction[3].operands[2],
                                OPERAND_TYPE_TEMP, tangent_reg, identity) ||
        !compiler_destination(&instruction[4].operands[0],
                              OPERAND_TYPE_TEMP, scalar_reg,
                              16 << scalar_lane) ||
        !compiler_scalar_source(&instruction[4].operands[1],
                                OPERAND_TYPE_TEMP, scalar_reg, scalar_lane) ||
        !compiler_destination(&instruction[5].operands[0],
                              OPERAND_TYPE_TEMP, tangent_reg,
                              HLSL_XYZ_MASK) ||
        !compiler_scalar_source(&instruction[5].operands[1],
                                OPERAND_TYPE_TEMP, scalar_reg, scalar_lane) ||
        !compiler_vector_source(&instruction[5].operands[2],
                                OPERAND_TYPE_TEMP, tangent_reg, identity))
        return false;

    const DXBCOperand *cross_mul_left = &instruction[6].operands[1];
    const DXBCOperand *cross_mul_right = &instruction[6].operands[2];
    if (!compiler_plain_operand(cross_mul_left) ||
        !compiler_plain_operand(cross_mul_right) ||
        cross_mul_left->type != OPERAND_TYPE_TEMP ||
        cross_mul_right->type != OPERAND_TYPE_TEMP)
        return false;
    const DXBCOperand *cross_mul_tangent = NULL;
    const DXBCOperand *cross_mul_normal = NULL;
    if (cross_mul_left->register_index == tangent_reg &&
        cross_mul_right->register_index != tangent_reg) {
        cross_mul_tangent = cross_mul_left;
        cross_mul_normal = cross_mul_right;
    } else if (cross_mul_right->register_index == tangent_reg &&
               cross_mul_left->register_index != tangent_reg) {
        cross_mul_tangent = cross_mul_right;
        cross_mul_normal = cross_mul_left;
    } else {
        return false;
    }
    const int normal_reg = cross_mul_normal->register_index;
    const int binormal_reg = instruction[6].operands[0].register_index;
    if (normal_reg < 0 || normal_reg == tangent_reg ||
        binormal_reg < 0 || binormal_reg == normal_reg ||
        binormal_reg == tangent_reg ||
        !compiler_destination(&instruction[6].operands[0],
                              OPERAND_TYPE_TEMP, binormal_reg,
                              HLSL_XYZ_MASK) ||
        !compiler_destination(&instruction[7].operands[0],
                              OPERAND_TYPE_TEMP, binormal_reg,
                              HLSL_XYZ_MASK) ||
        !compiler_negated_vector_source(&instruction[7].operands[3],
                                        OPERAND_TYPE_TEMP, binormal_reg,
                                        identity))
        return false;
    for (int lane = 0; lane < 3; lane++) {
        const int previous_normal = (lane + 2) % 3;
        const int previous_tangent = (lane + 1) % 3;
        const int add_normal = (lane + 1) % 3;
        const int add_tangent = (lane + 2) % 3;
        const int mul_normal =
            usil_operand_source_component(cross_mul_normal, lane);
        const int mul_tangent_physical =
            usil_operand_source_component(cross_mul_tangent, lane);
        const int mad_normal =
            usil_operand_source_component(&instruction[7].operands[1], lane);
        const int mad_tangent_physical =
            usil_operand_source_component(&instruction[7].operands[2], lane);
        if (mul_normal != previous_normal || mad_normal != add_normal ||
            mul_tangent_physical < 0 || mul_tangent_physical > 2 ||
            mad_tangent_physical < 0 || mad_tangent_physical > 2 ||
            permutation[mul_tangent_physical] != previous_tangent ||
            permutation[mad_tangent_physical] != add_tangent)
            return false;
    }
    if (!compiler_plain_operand(&instruction[7].operands[1]) ||
        instruction[7].operands[1].type != OPERAND_TYPE_TEMP ||
        instruction[7].operands[1].register_index != normal_reg ||
        !compiler_plain_operand(&instruction[7].operands[2]) ||
        instruction[7].operands[2].type != OPERAND_TYPE_TEMP ||
        instruction[7].operands[2].register_index != tangent_reg)
        return false;

    if (!compiler_destination(&instruction[8].operands[0],
                              OPERAND_TYPE_TEMP, scalar_reg,
                              16 << scalar_lane) ||
        !compiler_scalar_source(&instruction[8].operands[1],
                                OPERAND_TYPE_INPUT, input_reg, 3) ||
        !compiler_plain_operand(&instruction[8].operands[2]) ||
        instruction[8].operands[2].type != OPERAND_TYPE_CONSTANT_BUFFER ||
        instruction[8].operands[2].register_index != matrix_buffer ||
        !is_replicate_swizzle(&instruction[8].operands[2]) ||
        !compiler_destination(&instruction[9].operands[0],
                              OPERAND_TYPE_TEMP, binormal_reg,
                              HLSL_XYZ_MASK) ||
        !compiler_scalar_source(&instruction[9].operands[1],
                                OPERAND_TYPE_TEMP, scalar_reg, scalar_lane) ||
        !compiler_vector_source(&instruction[9].operands[2],
                                OPERAND_TYPE_TEMP, binormal_reg, identity))
        return false;

    const int output0 = instruction[10].operands[0].register_index;
    const int output1 = instruction[13].operands[0].register_index;
    const int output2 = instruction[14].operands[0].register_index;
    int inverse_permutation[3] = {-1, -1, -1};
    for (int lane = 0; lane < 3; lane++)
        inverse_permutation[permutation[lane]] = lane;
    if (output0 < 0 || output1 < 0 || output2 < 0 ||
        output0 == output1 || output0 == output2 || output1 == output2 ||
        !compiler_mov_scalar(&instruction[10], output0, 1,
                             binormal_reg, 0) ||
        !compiler_mov_scalar(&instruction[11], output0, 2,
                             normal_reg, 0) ||
        !compiler_mov_scalar(&instruction[12], output0, 0,
                             tangent_reg, inverse_permutation[0]) ||
        !compiler_mov_scalar(&instruction[13], output1, 0,
                             tangent_reg, inverse_permutation[1]) ||
        !compiler_mov_scalar(&instruction[14], output2, 0,
                             tangent_reg, inverse_permutation[2]) ||
        !compiler_mov_scalar(&instruction[15], output1, 2,
                             normal_reg, 1) ||
        !compiler_mov_scalar(&instruction[16], output2, 2,
                             normal_reg, 2) ||
        !compiler_mov_scalar(&instruction[17], output1, 1,
                             binormal_reg, 1) ||
        !compiler_mov_scalar(&instruction[18], output2, 1,
                             binormal_reg, 2))
        return false;

    *out_frame = (HLSLCompilerTangentFrame){
        .valid = true,
        .end_instruction = start + 18,
        .normal_register = normal_reg,
        .tangent_register = tangent_reg,
        .binormal_register = binormal_reg,
        .tangent_input_register = input_reg,
        .output_registers = {output0, output1, output2}};
    return true;
}

bool hlsl_compiler_tangent_frame_matches(const USILProgram *program,
                                         int instruction) {
    HLSLCompilerTangentFrame frame;
    return compiler_tangent_frame_matches(program, instruction, &frame);
}

static bool compiler_operand_temp_read_mask(
    const DXBCOperand *operand, bool operand_is_source,
    uint8_t logical_lane_mask, int reg, unsigned int depth,
    unsigned int *read_mask) {
    if (!read_mask) return false;
    if (!operand) return true;
    if (depth >= DXBC_MAX_NESTED_OPERAND_TOKENS) return false;

    if (operand_is_source &&
        (operand->type == OPERAND_TYPE_TEMP ||
         operand->type == OPERAND_TYPE_INDEXABLE_TEMP) &&
        operand->register_index == reg) {
        if (operand->swizzle_mode == 0) {
            /* Mask-selection sources retain their physical lane in the
             * operand mask.  In particular IF r0.z is operand zero with
             * destination_mask == 0x40, not a destination write. */
            *read_mask |=
                ((unsigned int)operand->destination_mask >> 4) & 0x0fu;
        } else if (operand->swizzle_mode == 1) {
            for (int lane = 0; lane < 4; ++lane) {
                if ((logical_lane_mask & (uint8_t)(1u << lane)) == 0)
                    continue;
                const int component = operand->swizzle[lane];
                if (component < 0 || component > 3) return false;
                *read_mask |= 1u << component;
            }
        } else if (operand->swizzle_mode == 2) {
            const int component = operand->swizzle[0];
            if (logical_lane_mask == 0 || component < 0 || component > 3)
                return false;
            *read_mask |= 1u << component;
        } else {
            return false;
        }
    }

    const DXBCOperand *relative_operands[3] = {
        operand->rel_op0, operand->rel_op1, operand->rel_op2};
    for (size_t relative = 0; relative < 3; ++relative) {
        if (!compiler_operand_temp_read_mask(
                relative_operands[relative], true, 1u, reg, depth + 1u,
                read_mask)) {
            return false;
        }
    }
    return true;
}

/* Derive both sides of the use/def relation from the validated opcode table.
 * No operand-position or destination-mask heuristic is authoritative here:
 * control-flow sources occupy operand zero and SM4/5 IMUL/UDIV/SINCOS each
 * have two destination operands.  Relative indices are executable sources
 * even when attached to a destination or binding operand. */
static bool compiler_instruction_temp_access_masks(
    const USILProgram *program, const USILInstruction *instruction, int reg,
    unsigned int *read_mask, unsigned int *write_mask) {
    if (read_mask) *read_mask = 0u;
    if (write_mask) *write_mask = 0u;
    if (!program || !instruction || reg < 0 || !read_mask || !write_mask ||
        !usil_instruction_shape_valid(program, instruction)) {
        return false;
    }

    for (int operand_index = 0;
         operand_index < instruction->operand_count; ++operand_index) {
        USILOperandUseInfo use;
        if (!usil_instruction_operand_use(program, instruction,
                                          operand_index, &use) ||
            use.use == USIL_OPERAND_USE_INVALID) {
            return false;
        }
        const DXBCOperand *operand = &instruction->operands[operand_index];
        if (!compiler_operand_temp_read_mask(
                operand, use.use == USIL_OPERAND_USE_SOURCE,
                use.source_lane_mask, reg, 0u, read_mask)) {
            return false;
        }
        if (use.use != USIL_OPERAND_USE_DESTINATION ||
            (operand->type != OPERAND_TYPE_TEMP &&
             operand->type != OPERAND_TYPE_INDEXABLE_TEMP) ||
            operand->register_index != reg) {
            continue;
        }
        *write_mask |=
            ((unsigned int)operand->destination_mask >> 4) & 0x0fu;
    }
    return true;
}

/* A compiler-model lift may stop materializing a physical temporary after
 * its logical value has been copied to a private source variable.  This
 * proof admits later register reuse only when every read is dominated by a
 * write to the same lane.  It therefore distinguishes harmless allocation
 * reuse from an actual downstream dependency on a removed assignment. */
static bool compiler_temp_value_is_dead_after(
    const USILProgram *program, int start, int reg,
    unsigned int live_mask) {
    if (!program || start < 0 || reg < 0) return false;
    live_mask &= 0x0fu;
    for (int index = start; index < program->instruction_count; index++) {
        const USILInstruction *instruction = &program->instructions[index];
        unsigned int reads = 0u;
        unsigned int writes = 0u;
        if (!compiler_instruction_temp_access_masks(
                program, instruction, reg, &reads, &writes)) {
            return false;
        }
        if ((reads & live_mask) != 0u) return false;
        live_mask &= ~writes;
        if (instruction->opcode == USIL_OP_RET) return true;
    }
    return false;
}

static bool compiler_opcode_is_control_flow(USILOpcode opcode) {
    switch (opcode) {
        case USIL_OP_IF:
        case USIL_OP_ELSE:
        case USIL_OP_ENDIF:
        case USIL_OP_LOOP:
        case USIL_OP_ENDLOOP:
        case USIL_OP_SWITCH:
        case USIL_OP_CASE:
        case USIL_OP_DEFAULT:
        case USIL_OP_ENDSWITCH:
        case USIL_OP_BREAK:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUE:
        case USIL_OP_CONTINUEC:
        case USIL_OP_RET:
        case USIL_OP_DISCARD:
            return true;
        default:
            return false;
    }
}

static bool compiler_signed_scalar_immediate(const DXBCOperand *operand,
                                             int *out_value) {
    if (!out_value || !compiler_plain_operand(operand) ||
        operand->type != OPERAND_TYPE_IMMEDIATE32 ||
        operand->imm_value_count != 1) {
        return false;
    }
    *out_value = (int32_t)operand->imm_values[0];
    return true;
}

static bool compiler_temp_value_is_dead_in_linear_tail(
    const USILProgram *program, int start, int reg,
    unsigned int live_mask);

static bool compiler_temp_lane_is_never_read_after(
    const USILProgram *program, int start, int reg,
    unsigned int lane_mask) {
    if (!program || start < 0 || reg < 0) return false;
    lane_mask &= 0x0fu;
    for (int index = start; index < program->instruction_count; index++) {
        unsigned int reads = 0u;
        unsigned int writes = 0u;
        if (!compiler_instruction_temp_access_masks(
                program, &program->instructions[index], reg, &reads,
                &writes) ||
            (reads & lane_mask) != 0u) {
            return false;
        }
    }
    return true;
}

static bool compiler_identifier_matches_indexed(const char *candidate,
                                                const char *prefix,
                                                int index,
                                                const char *suffix) {
    char generated[128];
    if (!candidate || !prefix || !suffix) return true;
    const int written = snprintf(generated, sizeof(generated), "%s%d%s",
                                 prefix, index, suffix);
    return written < 0 || (size_t)written >= sizeof(generated) ||
           strcmp(candidate, generated) == 0;
}

static bool compiler_identifier_matches_two_indices(
    const char *candidate, const char *prefix, int first, int second,
    const char *suffix) {
    char generated[128];
    if (!candidate || !prefix || !suffix) return true;
    const int written = snprintf(generated, sizeof(generated), "%s%d_%d%s",
                                 prefix, first, second, suffix);
    return written < 0 || (size_t)written >= sizeof(generated) ||
           strcmp(candidate, generated) == 0;
}

static bool compiler_identifier_conflicts_with_cbuffer(
    const HLSLEmitterContext *ctx, const char *candidate) {
    if (!ctx || !candidate || !ctx->cbuffer_layouts_built) return true;
    for (int buffer = 0; buffer < ctx->cbuffer_layout_count; ++buffer) {
        const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[buffer];
        if ((layout->declaration_name &&
             strcmp(candidate, layout->declaration_name) == 0) ||
            (layout->serialized_name &&
             strcmp(candidate, layout->serialized_name) == 0) ||
            compiler_identifier_matches_indexed(candidate, "cb", layout->reg,
                                                "") ||
            compiler_identifier_matches_indexed(candidate, "cb", layout->reg,
                                                "_rows") ||
            compiler_identifier_matches_indexed(candidate, "cb", layout->reg,
                                                "_data") ||
            compiler_identifier_matches_indexed(
                candidate, "dxbc_reflection_tail_cb", layout->reg, "") ||
            compiler_identifier_matches_indexed(candidate, "get_cb",
                                                layout->reg, "")) {
            return true;
        }
        for (int variable = 0; variable < layout->variable_count;
             ++variable) {
            if (layout->variables[variable].name &&
                strcmp(candidate, layout->variables[variable].name) == 0) {
                return true;
            }
        }
        if (layout->row_struct_parameter) {
            const SerializedStructParam *structure =
                layout->row_struct_parameter;
            if (structure->name && strcmp(candidate, structure->name) == 0)
                return true;
            for (int member = 0; member < structure->member_count; ++member) {
                if (structure->members && structure->members[member].name &&
                    strcmp(candidate, structure->members[member].name) == 0) {
                    return true;
                }
            }
        }
    }
    for (size_t builtin = 0; builtin < g_builtins_count; ++builtin) {
        if ((g_builtins[builtin].cb_name &&
             strcmp(candidate, g_builtins[builtin].cb_name) == 0) ||
            (g_builtins[builtin].var_name &&
             strcmp(candidate, g_builtins[builtin].var_name) == 0)) {
            return true;
        }
    }
    return false;
}

static bool compiler_identifier_conflicts_with_resources(
    const HLSLEmitterContext *ctx, const char *candidate) {
    if (!ctx || !ctx->program || !candidate) return true;
    const USILProgram *program = ctx->program;
    for (int texture = 0; texture < program->texture_count; ++texture) {
        const USILTexture *declaration = &program->textures[texture];
        const SerializedResourceType kind =
            strcmp(declaration->dimension, "structured") == 0 ||
                    strcmp(declaration->dimension, "raw") == 0
                ? SERIALIZED_RESOURCE_BUFFER
                : SERIALIZED_RESOURCE_TEXTURE;
        const char *name = NULL;
        if (!resolve_srv_name_ctx(ctx, declaration->reg_idx, kind, &name))
            return true;
        if ((name && strcmp(candidate, name) == 0) ||
            (!name && compiler_identifier_matches_indexed(
                          candidate, "t", declaration->reg_idx, "")) ||
            (strcmp(declaration->dimension, "structured") == 0 &&
             compiler_identifier_matches_indexed(
                 candidate, "dxbc_struct_t", declaration->reg_idx, ""))) {
            return true;
        }
    }
    for (int sampler = 0; sampler < program->sampler_count; ++sampler) {
        const int reg = program->samplers[sampler].reg_idx;
        if (reg < 0 || reg >= HLSL_SM5_SAMPLER_REGISTER_COUNT ||
            !ctx->sampler_names[reg] || !ctx->sampler_names[reg][0]) {
            return true;
        }
        const char *name = ctx->sampler_names[reg];
        if (strcmp(candidate, name) == 0) return true;
        bool uses_regular = false;
        bool uses_comparison = false;
        get_sampler_usage(program, reg, &uses_regular, &uses_comparison);
        if (uses_regular && uses_comparison) {
            const size_t name_length = strlen(name);
            static const char suffix[] = "_cmp";
            const size_t candidate_length = strlen(candidate);
            if (name_length <= SIZE_MAX - sizeof(suffix) &&
                candidate_length == name_length + sizeof(suffix) - 1u &&
                memcmp(candidate, name, name_length) == 0 &&
                memcmp(candidate + name_length, suffix, sizeof(suffix)) == 0) {
                return true;
            }
        }
    }
    for (int uav = 0; uav < program->uav_count; ++uav) {
        const USILUav *declaration = &program->uavs[uav];
        const char *name = resolve_uav_name(ctx->params,
                                            declaration->reg_idx);
        if ((name && strcmp(candidate, name) == 0) ||
            (!name && compiler_identifier_matches_indexed(
                          candidate, "u", declaration->reg_idx, "")) ||
            (strcmp(declaration->dimension, "structured") == 0 &&
             compiler_identifier_matches_indexed(
                 candidate, "dxbc_struct_u", declaration->reg_idx, ""))) {
            return true;
        }
    }
    return false;
}

static bool compiler_identifier_conflicts_with_generated_locals(
    const HLSLEmitterContext *ctx, const char *candidate) {
    static const char *const fixed[] = {
        "input", "output", "oDepth", "u_xlat_temp_x", "u_xlat_temp_y",
        "u_xlat_temp_z", "u_xlat_temp_w", "atomic_temp", "w", "h",
        "d", "elements", "levels", "samples", "unk0_arr",
        "dxbc_static_uv_condition", "dxbc_surface_world_position",
        "dxbc_surface_world_normal", "dxbc_surface_world_tangent",
        "dxbc_surface_tangent_sign", "dxbc_surface_world_binormal",
        "dxbc_face_index", "dxbc_transform_u", "dxbc_transform_v",
        "dxbc_face_normal", "dxbc_face_uv", "dxbc_global_wind_original",
        "dxbc_global_wind_length", "dxbc_global_wind_position",
        "dxbc_ray_direction", "dxbc_center", "dxbc_position", "dxbc_i",
        "dxbc_j", "dxbc_stream", "dxbc_instance_id",
        "dxbc_ray_box_intersection", "dxbc_unpack_normal"};
    if (!ctx || !ctx->program || !candidate) return true;
    for (size_t index = 0; index < sizeof(fixed) / sizeof(fixed[0]); ++index) {
        if (strcmp(candidate, fixed[index]) == 0) return true;
    }
    if (ctx->readable_screen_pos_helper &&
        strcmp(candidate, ctx->readable_screen_pos_helper) == 0) {
        return true;
    }
    for (int reg = 0; reg < ctx->program->temp_count; ++reg) {
        if (compiler_identifier_matches_indexed(candidate, "r", reg, ""))
            return true;
        const int max_generation = hlsl_register_max_generation(
            (HLSLEmitterContext *)ctx, reg);
        for (int generation = 0; generation <= max_generation; ++generation) {
            if (compiler_identifier_matches_two_indices(
                    candidate, "r", reg, generation, "")) {
                return true;
            }
        }
        if (reg < ctx->temp_state_count) {
            if (ctx->has_decomposition && ctx->has_decomposition[reg] &&
                (strcmp(candidate, ctx->decompositions[reg].IntVarName) == 0 ||
                 strcmp(candidate,
                        ctx->decompositions[reg].Float2VarName) == 0)) {
                return true;
            }
            for (int lane = 0; lane < 4; ++lane) {
                if ((ctx->has_ftoi_temp[reg][lane] &&
                     strcmp(candidate, ctx->ftoi_temps[reg][lane]) == 0) ||
                    (ctx->has_int_temp[reg][lane] &&
                     strcmp(candidate, ctx->int_temps[reg][lane]) == 0) ||
                    (ctx->has_write_redirect[reg][lane] &&
                     strcmp(candidate, ctx->write_redirects[reg][lane]) ==
                         0) ||
                    (ctx->has_deferred_float[reg][lane] &&
                     strcmp(candidate, ctx->deferred_floats[reg][lane]) ==
                         0)) {
                    return true;
                }
            }
        }
    }
    for (int input = 0; input < HLSL_SM5_IO_REGISTER_COUNT; ++input) {
        if (compiler_identifier_matches_indexed(candidate, "v", input, "") ||
            compiler_identifier_matches_indexed(candidate, "o", input, ""))
            return true;
    }
    for (int indexable = 0; indexable < ctx->program->indexable_temp_count;
         ++indexable) {
        if (compiler_identifier_matches_indexed(
                candidate, "x",
                ctx->program->indexable_temps[indexable].reg_idx, "")) {
            return true;
        }
    }
    for (int instruction = 0;
         instruction < ctx->program->instruction_count; ++instruction) {
        static const char *const indexed_prefixes[] = {
            "dxbc_saved_mul", "dxbc_compiler_world_",
            "dxbc_compiler_projection_pack_", "dxbc_compiler_clip_",
            "dxbc_compiler_screen_", "dxbc_row_index_i",
            "dxbc_sample_position", "dxbc_intersection", "color"};
        for (size_t prefix = 0;
             prefix < sizeof(indexed_prefixes) / sizeof(indexed_prefixes[0]);
             ++prefix) {
            if (compiler_identifier_matches_indexed(
                    candidate, indexed_prefixes[prefix], instruction, "")) {
                return true;
            }
        }
        static const char *const frame_suffixes[] = {
            "_normal", "_tangent", "_sign", "_binormal"};
        for (size_t suffix = 0;
             suffix < sizeof(frame_suffixes) / sizeof(frame_suffixes[0]);
             ++suffix) {
            if (compiler_identifier_matches_indexed(
                    candidate, "dxbc_compiler_frame_", instruction,
                    frame_suffixes[suffix])) {
                return true;
            }
        }
        for (int lane = 0; lane < 4; ++lane) {
            char truth[64];
            const int written = snprintf(truth, sizeof(truth),
                                         "dxbc_truth_%d_%c", instruction,
                                         "xyzw"[lane]);
            if (written < 0 || (size_t)written >= sizeof(truth) ||
                strcmp(candidate, truth) == 0) {
                return true;
            }
        }
    }
    for (int loop = 0; loop < ctx->program->instruction_count; ++loop) {
        const char *reserved = ctx->loop_info[loop].typed_bound_alias;
        if (reserved[0] && strcmp(candidate, reserved) == 0) return true;
    }
    return false;
}

static bool compiler_typed_loop_alias_conflicts(
    const HLSLEmitterContext *ctx, const char *candidate) {
    static const char *const hlsl_keywords[] = {
        "asm", "bool", "break", "buffer", "cbuffer", "case", "catch",
        "class", "const", "continue", "default", "discard", "do",
        "double", "else", "false", "float", "for", "half", "if", "in",
        "inline", "inout", "int", "interface", "matrix", "namespace",
        "out", "return", "sampler", "static", "struct", "switch", "true",
        "typedef", "uint", "uniform", "vector", "void", "volatile", "while"};
    if (!ctx || !candidate || !candidate[0]) return true;
    for (size_t keyword = 0;
         keyword < sizeof(hlsl_keywords) / sizeof(hlsl_keywords[0]);
         ++keyword) {
        if (strcmp(candidate, hlsl_keywords[keyword]) == 0) return true;
    }
    for (size_t identifier = 0;
         identifier < ctx->reserved_preprocessor_identifier_count;
         ++identifier) {
        if (strcmp(candidate,
                   ctx->reserved_preprocessor_identifiers[identifier]) == 0) {
            return true;
        }
    }
    return compiler_identifier_conflicts_with_cbuffer(ctx, candidate) ||
           compiler_identifier_conflicts_with_resources(ctx, candidate) ||
           compiler_identifier_conflicts_with_generated_locals(ctx,
                                                               candidate);
}

static bool compiler_allocate_typed_loop_alias(
    const HLSLEmitterContext *ctx, int ftoi_idx, char *alias,
    size_t alias_size) {
    if (!ctx || ftoi_idx < 0 || !alias || alias_size == 0) return false;
    alias[0] = '\0';
    enum { HLSL_TYPED_LOOP_ALIAS_ATTEMPTS = 1024 };
    for (unsigned int attempt = 0;
         attempt < HLSL_TYPED_LOOP_ALIAS_ATTEMPTS; ++attempt) {
        const int written =
            attempt == 0
                ? snprintf(alias, alias_size, "dxbc_loop_bound_%d", ftoi_idx)
                : snprintf(alias, alias_size,
                           "dxbc_loop_bound_%d_dxbc_%u", ftoi_idx, attempt);
        if (written < 0 || (size_t)written >= alias_size) {
            alias[0] = '\0';
            return false;
        }
        if (!compiler_typed_loop_alias_conflicts(ctx, alias)) return true;
    }
    alias[0] = '\0';
    return false;
}

static void compiler_reject_typed_loop_owner(HLSLEmitterContext *ctx,
                                             int loop_idx) {
    if (!ctx || !ctx->program || !ctx->loop_info ||
        !ctx->typed_loop_bound_instructions || loop_idx < 0 ||
        loop_idx >= ctx->program->instruction_count) {
        return;
    }
    LoopOptimizationInfo *info = &ctx->loop_info[loop_idx];
    const int claimed[4] = {
        info->typed_bound_ftoi_idx, info->typed_bound_imax_idx,
        info->typed_bound_imin_idx, info->typed_bound_itof_idx};
    for (size_t phase = 0; phase < 4; ++phase) {
        const int instruction = claimed[phase];
        if (instruction < 0 ||
            instruction >= ctx->program->instruction_count) {
            continue;
        }
        HLSLTypedLoopBoundInstruction *association =
            &ctx->typed_loop_bound_instructions[instruction];
        if (association->loop_instruction == loop_idx) {
            association->loop_instruction = -2;
            association->phase = HLSL_TYPED_LOOP_BOUND_NONE;
        }
    }
    info->typed_bound_ftoi_idx = -1;
    info->typed_bound_imax_idx = -1;
    info->typed_bound_imin_idx = -1;
    info->typed_bound_itof_idx = -1;
    info->typed_bound_alias[0] = '\0';
}

static bool compiler_register_typed_loop_bound(
    HLSLEmitterContext *ctx, int loop_idx, int ftoi_idx, int imax_idx,
    int imin_idx, int itof_idx, const char *alias) {
    if (!ctx || !ctx->program || !ctx->loop_info ||
        !ctx->typed_loop_bound_instructions || !alias || !alias[0] ||
        loop_idx < 0 || loop_idx >= ctx->program->instruction_count) {
        return false;
    }
    const int claimed[4] = {ftoi_idx, imax_idx, imin_idx, itof_idx};
    const HLSLTypedLoopBoundPhase phases[4] = {
        HLSL_TYPED_LOOP_BOUND_FTOI, HLSL_TYPED_LOOP_BOUND_IMAX,
        HLSL_TYPED_LOOP_BOUND_IMIN, HLSL_TYPED_LOOP_BOUND_ITOF};
    bool conflict = false;
    for (size_t phase = 0; phase < 4; ++phase) {
        if (claimed[phase] < 0 ||
            claimed[phase] >= ctx->program->instruction_count) {
            return false;
        }
        for (size_t previous = 0; previous < phase; ++previous) {
            if (claimed[phase] == claimed[previous]) return false;
        }
        if (ctx->typed_loop_bound_instructions[claimed[phase]]
                .loop_instruction != -1) {
            conflict = true;
        }
    }
    if (conflict) {
        for (size_t phase = 0; phase < 4; ++phase) {
            HLSLTypedLoopBoundInstruction *association =
                &ctx->typed_loop_bound_instructions[claimed[phase]];
            if (association->loop_instruction >= 0) {
                compiler_reject_typed_loop_owner(
                    ctx, association->loop_instruction);
            }
            association->loop_instruction = -2;
            association->phase = HLSL_TYPED_LOOP_BOUND_NONE;
        }
        return false;
    }

    LoopOptimizationInfo *info = &ctx->loop_info[loop_idx];
    const size_t alias_length = strlen(alias);
    if (alias_length >= sizeof(info->typed_bound_alias)) return false;
    memcpy(info->typed_bound_alias, alias, alias_length + 1u);
    info->typed_bound_ftoi_idx = ftoi_idx;
    info->typed_bound_imax_idx = imax_idx;
    info->typed_bound_imin_idx = imin_idx;
    info->typed_bound_itof_idx = itof_idx;
    for (size_t phase = 0; phase < 4; ++phase) {
        ctx->typed_loop_bound_instructions[claimed[phase]] =
            (HLSLTypedLoopBoundInstruction){loop_idx, phases[phase]};
    }
    return true;
}

/* FXC's loop simulator can lose the integer range when an exact DXBC scalar
 * is carried through float-backed temporary bits.  For the resulting source
 * spelling it then diagnoses a zero-iteration loop and may remove the loop
 * before HLSLcc sees it.  Admit a typed compiler inverse only for the complete
 * physical form observed in Unity's post-processing shaders:
 *
 *   ftoi bound, value
 *   imax bound, bound, positive-lower
 *   imin bound, bound, upper>=lower
 *   itof denominator, bound
 *   ... straight-line code that does not rewrite bound ...
 *   loop; ige predicate, counter, bound; breakc_nz predicate
 *   itof predicate, counter
 *
 * The first body instruction proves the comparison result is unconditionally
 * overwritten before a read, and the post-loop liveness proofs reject any
 * observable exit value.  Keeping the typed alias live through the loop
 * condition is therefore an exact source inverse, while every near-match
 * retains the assignment-preserving generic loop emission. */
static void detect_typed_integer_loop_bounds(HLSLEmitterContext *ctx) {
    const USILProgram *program = ctx ? ctx->program : NULL;
    if (!program || ctx->emit_mode != HLSL_EMIT_MODE_RECOMPILE ||
        ctx->use_uint_temps || !ctx->loop_info) {
        return;
    }

    for (int loop_idx = 0; loop_idx < program->instruction_count;
         loop_idx++) {
        LoopOptimizationInfo *info = &ctx->loop_info[loop_idx];
        if (!info->is_optimized || info->comparison_inst_idx != loop_idx + 1 ||
            info->breakc_inst_idx != loop_idx + 2 ||
            loop_idx + 3 >= program->instruction_count) {
            continue;
        }

        const USILInstruction *comparison =
            &program->instructions[info->comparison_inst_idx];
        const USILInstruction *breakc =
            &program->instructions[info->breakc_inst_idx];
        const USILInstruction *first_body =
            &program->instructions[loop_idx + 3];
        if (!compiler_instruction_shape(comparison, USIL_OP_IGE, 3) ||
            !compiler_instruction_shape(breakc, USIL_OP_BREAKC, 1) ||
            breakc->condition_test != DXBC_INSTRUCTION_TEST_NONZERO ||
            !compiler_instruction_shape(first_body, USIL_OP_ITOF, 2)) {
            continue;
        }

        const DXBCOperand *predicate = &comparison->operands[0];
        if (!compiler_plain_operand(predicate) ||
            predicate->type != OPERAND_TYPE_TEMP) {
            continue;
        }
        int predicate_lane = -1;
        for (int lane = 0; lane < 4; lane++) {
            if (predicate->destination_mask == (16 << lane)) {
                predicate_lane = lane;
                break;
            }
        }
        if (predicate_lane < 0 ||
            !compiler_scalar_source(&breakc->operands[0],
                                    OPERAND_TYPE_TEMP,
                                    predicate->register_index,
                                    predicate_lane) ||
            !compiler_destination(&first_body->operands[0],
                                  OPERAND_TYPE_TEMP,
                                  predicate->register_index,
                                  16 << predicate_lane)) {
            continue;
        }

        int loop_depth = 1;
        int endloop_idx = -1;
        for (int index = loop_idx + 3;
             index < program->instruction_count; index++) {
            const USILOpcode opcode = program->instructions[index].opcode;
            if (opcode == USIL_OP_LOOP) {
                loop_depth++;
            } else if (opcode == USIL_OP_ENDLOOP && --loop_depth == 0) {
                endloop_idx = index;
                break;
            }
        }
        const unsigned int predicate_mask = 1u << predicate_lane;
        if (endloop_idx < 0 ||
            (!compiler_temp_lane_is_never_read_after(
                 program, endloop_idx + 1, predicate->register_index,
                 predicate_mask) &&
             !compiler_temp_value_is_dead_in_linear_tail(
                 program, endloop_idx + 1, predicate->register_index,
                 predicate_mask))) {
            continue;
        }

        const DXBCOperand *counter = &comparison->operands[1];
        const DXBCOperand *bound = &comparison->operands[2];
        if (!compiler_plain_operand(counter) ||
            counter->type != OPERAND_TYPE_TEMP ||
            !is_replicate_swizzle(counter) ||
            !compiler_plain_operand(bound) ||
            bound->type != OPERAND_TYPE_TEMP ||
            !is_replicate_swizzle(bound)) {
            continue;
        }
        const int counter_lane = usil_operand_source_component(counter, 0);
        const int bound_lane = usil_operand_source_component(bound, 0);
        const int counter_reg = counter->register_index;
        const int bound_reg = bound->register_index;
        if (counter_lane < 0 || counter_lane > 3 || bound_lane < 0 ||
            bound_lane > 3 || counter_reg < 0 || bound_reg < 0 ||
            (predicate->register_index == counter_reg &&
             predicate_lane == counter_lane) ||
            (predicate->register_index == bound_reg &&
             predicate_lane == bound_lane) ||
            !compiler_scalar_source(&first_body->operands[1],
                                    OPERAND_TYPE_TEMP, counter_reg,
                                    counter_lane)) {
            continue;
        }

        int imin_idx = -1;
        bool bound_access_valid = true;
        for (int index = loop_idx - 1; index >= 0; index--) {
            const USILInstruction *instruction =
                &program->instructions[index];
            if (compiler_opcode_is_control_flow(instruction->opcode)) break;
            unsigned int reads = 0u;
            unsigned int writes = 0u;
            if (!compiler_instruction_temp_access_masks(
                    program, instruction, bound_reg, &reads, &writes)) {
                bound_access_valid = false;
                break;
            }
            if ((writes & (1u << bound_lane)) != 0u) {
                imin_idx = index;
                break;
            }
        }
        if (!bound_access_valid || imin_idx < 2 || imin_idx + 1 >= loop_idx)
            continue;
        const int ftoi_idx = imin_idx - 2;
        const int imax_idx = imin_idx - 1;
        const int itof_idx = imin_idx + 1;
        const USILInstruction *ftoi = &program->instructions[ftoi_idx];
        const USILInstruction *imax = &program->instructions[imax_idx];
        const USILInstruction *imin = &program->instructions[imin_idx];
        const USILInstruction *itof = &program->instructions[itof_idx];
        const int bound_mask = 16 << bound_lane;
        if (!compiler_instruction_shape(ftoi, USIL_OP_FTOI, 2) ||
            !compiler_instruction_shape(imax, USIL_OP_IMAX, 3) ||
            !compiler_instruction_shape(imin, USIL_OP_IMIN, 3) ||
            !compiler_instruction_shape(itof, USIL_OP_ITOF, 2) ||
            !compiler_destination(&ftoi->operands[0], OPERAND_TYPE_TEMP,
                                  bound_reg, bound_mask) ||
            !compiler_scalar_source(&ftoi->operands[1], OPERAND_TYPE_TEMP,
                                    bound_reg, bound_lane) ||
            !compiler_destination(&imax->operands[0], OPERAND_TYPE_TEMP,
                                  bound_reg, bound_mask) ||
            !compiler_scalar_source(&imax->operands[1], OPERAND_TYPE_TEMP,
                                    bound_reg, bound_lane) ||
            !compiler_destination(&imin->operands[0], OPERAND_TYPE_TEMP,
                                  bound_reg, bound_mask) ||
            !compiler_scalar_source(&imin->operands[1], OPERAND_TYPE_TEMP,
                                    bound_reg, bound_lane) ||
            !compiler_scalar_source(&itof->operands[1], OPERAND_TYPE_TEMP,
                                    bound_reg, bound_lane)) {
            continue;
        }

        int lower = 0;
        int upper = 0;
        if (!compiler_signed_scalar_immediate(&imax->operands[2], &lower) ||
            !compiler_signed_scalar_immediate(&imin->operands[2], &upper) ||
            lower <= 0 || upper < lower) {
            continue;
        }

        if (bound_reg >= ctx->temp_state_count ||
            is_register_decomposed(ctx, bound_reg) ||
            ctx->has_ftoi_temp[bound_reg][bound_lane] ||
            ctx->has_int_temp[bound_reg][bound_lane] ||
            ctx->has_deferred_float[bound_reg][bound_lane] ||
            ctx->has_write_redirect[bound_reg][bound_lane] ||
            hlsl_operand_backing_storage(ctx, &ftoi->operands[0],
                                         bound_mask, true) !=
                HLSL_BACKING_STORAGE_FLOAT) {
            continue;
        }

        bool straight_line = true;
        for (int index = ftoi_idx; index < loop_idx; index++) {
            const USILInstruction *instruction =
                &program->instructions[index];
            if (compiler_opcode_is_control_flow(instruction->opcode)) {
                straight_line = false;
                break;
            }
            unsigned int reads = 0u;
            unsigned int writes = 0u;
            if (!compiler_instruction_temp_access_masks(
                    program, instruction, bound_reg, &reads, &writes)) {
                straight_line = false;
                break;
            }
            if (index > imin_idx &&
                (writes & (1u << bound_lane)) != 0u) {
                straight_line = false;
                break;
            }
        }
        for (int index = loop_idx + 3;
             straight_line && index < endloop_idx; index++) {
            unsigned int reads = 0u;
            unsigned int writes = 0u;
            if (!compiler_instruction_temp_access_masks(
                    program, &program->instructions[index], bound_reg,
                    &reads, &writes) ||
                (writes & (1u << bound_lane)) != 0u) {
                straight_line = false;
            }
        }
        if (!straight_line) continue;

        char alias[HLSL_TYPED_LOOP_BOUND_ALIAS_SIZE];
        if (!compiler_allocate_typed_loop_alias(ctx, ftoi_idx, alias,
                                                sizeof(alias))) {
            continue;
        }
        (void)compiler_register_typed_loop_bound(
            ctx, loop_idx, ftoi_idx, imax_idx, imin_idx, itof_idx, alias);
    }
}

const LoopOptimizationInfo *hlsl_typed_loop_bound_for_instruction(
    const HLSLEmitterContext *ctx, int instruction,
    HLSLTypedLoopBoundPhase *out_phase) {
    if (out_phase) *out_phase = HLSL_TYPED_LOOP_BOUND_NONE;
    if (!ctx || !ctx->program || !ctx->loop_info ||
        !ctx->typed_loop_bound_instructions || instruction < 0 ||
        instruction >= ctx->program->instruction_count) {
        return NULL;
    }
    const HLSLTypedLoopBoundInstruction *association =
        &ctx->typed_loop_bound_instructions[instruction];
    if (association->loop_instruction < 0 ||
        association->loop_instruction >= ctx->program->instruction_count ||
        association->phase < HLSL_TYPED_LOOP_BOUND_FTOI ||
        association->phase > HLSL_TYPED_LOOP_BOUND_ITOF) {
        return NULL;
    }
    const LoopOptimizationInfo *info =
        &ctx->loop_info[association->loop_instruction];
    const int expected_instruction =
        association->phase == HLSL_TYPED_LOOP_BOUND_FTOI
            ? info->typed_bound_ftoi_idx
            : association->phase == HLSL_TYPED_LOOP_BOUND_IMAX
                  ? info->typed_bound_imax_idx
                  : association->phase == HLSL_TYPED_LOOP_BOUND_IMIN
                        ? info->typed_bound_imin_idx
                        : info->typed_bound_itof_idx;
    if (!info->typed_bound_alias[0] || expected_instruction != instruction)
        return NULL;
    if (out_phase) *out_phase = association->phase;
    return info;
}

/* This is a deliberately stricter liveness proof than the general helper
 * above.  A split source expression is allowed to span only one straight-line
 * region: all lanes of the physical temporary must be overwritten, or the
 * program must return, before a branch boundary can make a linear scan
 * ambiguous. */
static bool compiler_temp_value_is_dead_in_linear_tail(
    const USILProgram *program, int start, int reg,
    unsigned int live_mask) {
    if (!program || start < 0 || reg < 0) return false;
    live_mask &= 0x0fu;
    for (int index = start; index < program->instruction_count; index++) {
        const USILInstruction *instruction = &program->instructions[index];
        if (instruction->opcode == USIL_OP_RET) return true;
        if (compiler_opcode_is_control_flow(instruction->opcode)) return false;
        unsigned int reads = 0u;
        unsigned int writes = 0u;
        if (!compiler_instruction_temp_access_masks(
                program, instruction, reg, &reads, &writes)) {
            return false;
        }
        if ((reads & live_mask) != 0u) return false;
        live_mask &= ~writes;
        if (live_mask == 0u) return true;
    }
    return false;
}

static bool compiler_split_object_chain_matches(
    const USILInstruction *instruction, int destination_reg,
    int input_reg, int matrix_buffer, int first_row,
    bool homogeneous_input) {
    static const int identity3[3] = {0, 1, 2};
    static const int identity4[4] = {0, 1, 2, 3};
    if (!instruction || destination_reg < 0 || input_reg < 0 ||
        matrix_buffer < 0 || first_row < 0 ||
        !compiler_instruction_shape(&instruction[0], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[1], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&instruction[2], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&instruction[3],
                                    homogeneous_input ? USIL_OP_MAD
                                                      : USIL_OP_ADD,
                                    homogeneous_input ? 4 : 3)) {
        return false;
    }
    const int mask = homogeneous_input ? HLSL_XYZ_MASK : HLSL_XYZW_MASK;
    for (int item = 0; item < 4; item++) {
        if (!compiler_destination(&instruction[item].operands[0],
                                  OPERAND_TYPE_TEMP, destination_reg,
                                  mask)) {
            return false;
        }
    }
    bool row1_matches = homogeneous_input
                            ? compiler_static_cbuffer_row(
                                  &instruction[0].operands[2], matrix_buffer,
                                  first_row + 1, identity3)
                            : compiler_static_cbuffer_row4(
                                  &instruction[0].operands[2], matrix_buffer,
                                  first_row + 1, identity4);
    bool row0_matches = homogeneous_input
                            ? compiler_static_cbuffer_row(
                                  &instruction[1].operands[1], matrix_buffer,
                                  first_row, identity3)
                            : compiler_static_cbuffer_row4(
                                  &instruction[1].operands[1], matrix_buffer,
                                  first_row, identity4);
    bool accumulator1_matches = homogeneous_input
                                    ? compiler_vector_source(
                                          &instruction[1].operands[3],
                                          OPERAND_TYPE_TEMP,
                                          destination_reg, identity3)
                                    : compiler_vector4_source(
                                          &instruction[1].operands[3],
                                          OPERAND_TYPE_TEMP,
                                          destination_reg, identity4);
    bool row2_matches = homogeneous_input
                            ? compiler_static_cbuffer_row(
                                  &instruction[2].operands[1], matrix_buffer,
                                  first_row + 2, identity3)
                            : compiler_static_cbuffer_row4(
                                  &instruction[2].operands[1], matrix_buffer,
                                  first_row + 2, identity4);
    bool accumulator2_matches = homogeneous_input
                                    ? compiler_vector_source(
                                          &instruction[2].operands[3],
                                          OPERAND_TYPE_TEMP,
                                          destination_reg, identity3)
                                    : compiler_vector4_source(
                                          &instruction[2].operands[3],
                                          OPERAND_TYPE_TEMP,
                                          destination_reg, identity4);
    if (!compiler_scalar_source(&instruction[0].operands[1],
                                OPERAND_TYPE_INPUT, input_reg, 1) ||
        !row1_matches || !row0_matches ||
        !compiler_scalar_source(&instruction[1].operands[2],
                                OPERAND_TYPE_INPUT, input_reg, 0) ||
        !accumulator1_matches || !row2_matches ||
        !compiler_scalar_source(&instruction[2].operands[2],
                                OPERAND_TYPE_INPUT, input_reg, 2) ||
        !accumulator2_matches) {
        return false;
    }
    if (homogeneous_input) {
        return compiler_static_cbuffer_row(
                   &instruction[3].operands[1], matrix_buffer,
                   first_row + 3, identity3) &&
               compiler_scalar_source(&instruction[3].operands[2],
                                      OPERAND_TYPE_INPUT, input_reg, 3) &&
               compiler_vector_source(&instruction[3].operands[3],
                                      OPERAND_TYPE_TEMP, destination_reg,
                                      identity3);
    }
    return compiler_vector4_source(&instruction[3].operands[1],
                                   OPERAND_TYPE_TEMP, destination_reg,
                                   identity4) &&
           compiler_static_cbuffer_row4(
               &instruction[3].operands[2], matrix_buffer,
               first_row + 3, identity4);
}

/* D3DCompiler does not recover Unity's two object-space expressions from a
 * flat register program.  If the clip expression uses float4(position.xyz,1)
 * while a later world expression uses position.w, flattening exposes their
 * common three-instruction prefix and CSE merges it.  The original source
 * shape, proved entirely by this graph, keeps the two matrix multiplies as
 * independent expressions and reproduces the serialized instruction stream. */
static bool compiler_split_matrix_transform_matches(
    const USILProgram *program, int clip_start,
    HLSLCompilerSplitMatrixTransform *out_transform) {
    static const int identity4[4] = {0, 1, 2, 3};
    if (!program || !out_transform || clip_start < 0 ||
        clip_start + 11 >= program->instruction_count) {
        return false;
    }
    const USILInstruction *clip = program->instructions + clip_start;
    const int clip_temp = clip[0].operands[0].register_index;
    const int input_reg = clip[0].operands[1].register_index;
    const int object_buffer = clip[0].operands[2].register_index;
    const int object_first_row = clip[0].operands[2].rel_offset0 - 1;
    if (clip_temp < 0 || input_reg < 0 || object_buffer < 0 ||
        object_first_row < 0 ||
        !compiler_split_object_chain_matches(
            clip, clip_temp, input_reg, object_buffer, object_first_row,
            false)) {
        return false;
    }

    const int clip_accumulator = clip[4].operands[0].register_index;
    const int clip_buffer = clip[4].operands[2].register_index;
    const int clip_first_row = clip[4].operands[2].rel_offset0 - 1;
    if (clip_accumulator < 0 || clip_accumulator == clip_temp ||
        clip_buffer < 0 || clip_first_row < 0 ||
        !compiler_instruction_shape(&clip[4], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&clip[5], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&clip[6], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&clip[7], USIL_OP_MAD, 4) ||
        !compiler_destination(&clip[4].operands[0], OPERAND_TYPE_TEMP,
                              clip_accumulator, HLSL_XYZW_MASK) ||
        !compiler_scalar_source(&clip[4].operands[1], OPERAND_TYPE_TEMP,
                                clip_temp, 1) ||
        !compiler_static_cbuffer_row4(&clip[4].operands[2], clip_buffer,
                                     clip_first_row + 1, identity4)) {
        return false;
    }
    const int clip_components[3] = {0, 2, 3};
    const int clip_rows[3] = {0, 2, 3};
    for (int item = 0; item < 3; item++) {
        const USILInstruction *mad = &clip[5 + item];
        DXBCOperandType destination_type = item == 2
                                               ? OPERAND_TYPE_OUTPUT
                                               : OPERAND_TYPE_TEMP;
        int destination_reg = item == 2
                                  ? mad->operands[0].register_index
                                  : clip_accumulator;
        if (destination_reg < 0 ||
            !compiler_destination(&mad->operands[0], destination_type,
                                  destination_reg, HLSL_XYZW_MASK) ||
            !compiler_static_cbuffer_row4(
                &mad->operands[1], clip_buffer,
                clip_first_row + clip_rows[item], identity4) ||
            !compiler_scalar_source(&mad->operands[2], OPERAND_TYPE_TEMP,
                                    clip_temp, clip_components[item]) ||
            !compiler_vector4_source(&mad->operands[3], OPERAND_TYPE_TEMP,
                                     clip_accumulator, identity4)) {
            return false;
        }
    }
    if (object_buffer == clip_buffer &&
        object_first_row == clip_first_row) {
        return false;
    }

    int world_start = -1;
    int search_end = program->instruction_count - 4;
    for (int candidate = clip_start + 8; candidate <= search_end;
         candidate++) {
        if (compiler_opcode_is_control_flow(
                program->instructions[candidate].opcode)) {
            break;
        }
        int world_reg =
            program->instructions[candidate].operands[0].register_index;
        if (!compiler_split_object_chain_matches(
                program->instructions + candidate, world_reg, input_reg,
                object_buffer, object_first_row, true)) {
            continue;
        }
        if (world_start >= 0) return false;
        world_start = candidate;
    }
    if (world_start < 0 ||
        !compiler_temp_value_is_dead_in_linear_tail(
            program, clip_start + 8, clip_temp, 0x0fu) ||
        !compiler_temp_value_is_dead_in_linear_tail(
            program, clip_start + 8, clip_accumulator, 0x0fu)) {
        return false;
    }

    *out_transform = (HLSLCompilerSplitMatrixTransform){
        .valid = true,
        .phase = 0,
        .clip_instruction = clip_start,
        .world_instruction = world_start,
        .position_input_register = input_reg,
        .object_matrix_buffer = object_buffer,
        .object_matrix_first_row = object_first_row,
        .clip_matrix_buffer = clip_buffer,
        .clip_matrix_first_row = clip_first_row};
    return true;
}

bool hlsl_compiler_split_matrix_transform_matches(
    const USILProgram *program, int clip_instruction,
    int *out_world_instruction) {
    HLSLCompilerSplitMatrixTransform transform;
    bool matches = compiler_split_matrix_transform_matches(
        program, clip_instruction, &transform);
    if (matches && out_world_instruction)
        *out_world_instruction = transform.world_instruction;
    return matches;
}

static bool compiler_half_scalar(const DXBCOperand *operand) {
    return compiler_plain_operand(operand) &&
           operand->type == OPERAND_TYPE_IMMEDIATE32 &&
           operand->imm_value_count == 1 &&
           operand->immediate_word_count == 1 &&
           operand->immediate_words[0] == 0x3f000000u;
}

static bool compiler_projection_scalar(const DXBCOperand *operand) {
    return compiler_plain_operand(operand) &&
           operand->type == OPERAND_TYPE_CONSTANT_BUFFER &&
           operand->register_index_dim == 2 &&
           operand->index_has_immediate[0] &&
           operand->index_has_immediate[1] &&
           !operand->index_value_exceeds_int[0] &&
           !operand->index_value_exceeds_int[1] &&
           is_replicate_swizzle(operand);
}

static bool compiler_screen_sequence_combined_matches(
    const USILProgram *program, int start, int clip_reg,
    int *out_work_reg) {
    if (!program || !out_work_reg || start < 0 ||
        start + 3 >= program->instruction_count)
        return false;
    const USILInstruction *instruction = program->instructions + start;
    if (!compiler_instruction_shape(&instruction[0], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[1], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[2], USIL_OP_MOV, 2) ||
        !compiler_instruction_shape(&instruction[3], USIL_OP_ADD, 3) ||
        !compiler_destination(&instruction[0].operands[0],
                              OPERAND_TYPE_TEMP, clip_reg, 32) ||
        !compiler_scalar_source(&instruction[0].operands[1],
                                OPERAND_TYPE_TEMP, clip_reg, 1) ||
        !compiler_projection_scalar(&instruction[0].operands[2]))
        return false;

    const int work_reg = instruction[1].operands[0].register_index;
    const DXBCOperand *work_destination = &instruction[1].operands[0];
    const DXBCOperand *clip_xxwy = &instruction[1].operands[1];
    const DXBCOperand *half = &instruction[1].operands[2];
    if (work_reg < 0 || work_reg == clip_reg ||
        !compiler_destination(work_destination, OPERAND_TYPE_TEMP, work_reg,
                              16 | 64 | 128) ||
        !compiler_plain_operand(clip_xxwy) ||
        clip_xxwy->type != OPERAND_TYPE_TEMP ||
        clip_xxwy->register_index != clip_reg ||
        clip_xxwy->swizzle_mode != 1 || clip_xxwy->swizzle[0] != 0 ||
        clip_xxwy->swizzle[2] != 3 || clip_xxwy->swizzle[3] != 1 ||
        !compiler_plain_operand(half) ||
        half->type != OPERAND_TYPE_IMMEDIATE32 ||
        half->imm_value_count != 4 || half->immediate_word_count != 4 ||
        half->immediate_words[0] != 0x3f000000u ||
        half->immediate_words[1] != 0u ||
        half->immediate_words[2] != 0x3f000000u ||
        half->immediate_words[3] != 0x3f000000u)
        return false;

    const int screen_output = instruction[2].operands[0].register_index;
    const DXBCOperand *copy_source = &instruction[2].operands[1];
    if (screen_output < 0 ||
        !compiler_destination(&instruction[2].operands[0],
                              OPERAND_TYPE_OUTPUT, screen_output,
                              64 | 128) ||
        !compiler_plain_operand(copy_source) ||
        copy_source->type != OPERAND_TYPE_TEMP ||
        copy_source->register_index != clip_reg ||
        copy_source->swizzle_mode != 1 ||
        usil_operand_source_component(copy_source, 2) != 2 ||
        usil_operand_source_component(copy_source, 3) != 3 ||
        !compiler_destination(&instruction[3].operands[0],
                              OPERAND_TYPE_OUTPUT, screen_output,
                              16 | 32) ||
        !compiler_scalar_source(&instruction[3].operands[1],
                                OPERAND_TYPE_TEMP, work_reg, 2) ||
        !compiler_plain_operand(&instruction[3].operands[2]) ||
        instruction[3].operands[2].type != OPERAND_TYPE_TEMP ||
        instruction[3].operands[2].register_index != work_reg ||
        instruction[3].operands[2].swizzle_mode != 1 ||
        usil_operand_source_component(&instruction[3].operands[2], 0) != 0 ||
        usil_operand_source_component(&instruction[3].operands[2], 1) != 3)
        return false;

    *out_work_reg = work_reg;
    return true;
}

static bool compiler_screen_sequence_split_matches(
    const USILProgram *program, int start, int clip_reg,
    int *out_work_reg) {
    if (!program || !out_work_reg || start < 0 ||
        start + 4 >= program->instruction_count)
        return false;
    const USILInstruction *instruction = program->instructions + start;
    if (!compiler_instruction_shape(&instruction[0], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[1], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[2], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[3], USIL_OP_MOV, 2) ||
        !compiler_instruction_shape(&instruction[4], USIL_OP_ADD, 3))
        return false;

    const int work_reg = instruction[0].operands[0].register_index;
    if (work_reg < 0 || work_reg == clip_reg ||
        !compiler_destination(&instruction[0].operands[0],
                              OPERAND_TYPE_TEMP, work_reg, 16) ||
        !compiler_scalar_source(&instruction[0].operands[1],
                                OPERAND_TYPE_TEMP, clip_reg, 1) ||
        !compiler_projection_scalar(&instruction[0].operands[2]) ||
        !compiler_destination(&instruction[1].operands[0],
                              OPERAND_TYPE_TEMP, work_reg, 128) ||
        !compiler_scalar_source(&instruction[1].operands[1],
                                OPERAND_TYPE_TEMP, work_reg, 0) ||
        !compiler_half_scalar(&instruction[1].operands[2]) ||
        !compiler_destination(&instruction[2].operands[0],
                              OPERAND_TYPE_TEMP, work_reg, 16 | 64) ||
        !compiler_plain_operand(&instruction[2].operands[1]) ||
        instruction[2].operands[1].type != OPERAND_TYPE_TEMP ||
        instruction[2].operands[1].register_index != clip_reg ||
        instruction[2].operands[1].swizzle_mode != 1 ||
        usil_operand_source_component(&instruction[2].operands[1], 0) != 0 ||
        usil_operand_source_component(&instruction[2].operands[1], 2) != 3 ||
        !compiler_plain_operand(&instruction[2].operands[2]) ||
        instruction[2].operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
        instruction[2].operands[2].imm_value_count != 4 ||
        instruction[2].operands[2].immediate_word_count != 4 ||
        instruction[2].operands[2].immediate_words[0] != 0x3f000000u ||
        instruction[2].operands[2].immediate_words[1] != 0u ||
        instruction[2].operands[2].immediate_words[2] != 0x3f000000u ||
        instruction[2].operands[2].immediate_words[3] != 0u)
        return false;

    const int screen_output = instruction[3].operands[0].register_index;
    const DXBCOperand *copy_source = &instruction[3].operands[1];
    if (screen_output < 0 ||
        !compiler_destination(&instruction[3].operands[0],
                              OPERAND_TYPE_OUTPUT, screen_output,
                              64 | 128) ||
        !compiler_plain_operand(copy_source) ||
        copy_source->type != OPERAND_TYPE_TEMP ||
        copy_source->register_index != clip_reg ||
        copy_source->swizzle_mode != 1 ||
        usil_operand_source_component(copy_source, 2) != 2 ||
        usil_operand_source_component(copy_source, 3) != 3 ||
        !compiler_destination(&instruction[4].operands[0],
                              OPERAND_TYPE_OUTPUT, screen_output,
                              16 | 32) ||
        !compiler_scalar_source(&instruction[4].operands[1],
                                OPERAND_TYPE_TEMP, work_reg, 2) ||
        !compiler_plain_operand(&instruction[4].operands[2]) ||
        instruction[4].operands[2].type != OPERAND_TYPE_TEMP ||
        instruction[4].operands[2].register_index != work_reg ||
        instruction[4].operands[2].swizzle_mode != 1 ||
        usil_operand_source_component(&instruction[4].operands[2], 0) != 0 ||
        usil_operand_source_component(&instruction[4].operands[2], 1) != 3)
        return false;

    *out_work_reg = work_reg;
    return true;
}

static bool compiler_screen_sequence_inplace_matches(
    const USILProgram *program, int start, int clip_reg,
    int *out_work_reg) {
    if (!program || !out_work_reg || start < 0 ||
        start + 3 >= program->instruction_count)
        return false;
    const USILInstruction *instruction = program->instructions + start;
    if (!compiler_instruction_shape(&instruction[0], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[1], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[2], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[3], USIL_OP_ADD, 3) ||
        !compiler_destination(&instruction[0].operands[0],
                              OPERAND_TYPE_TEMP, clip_reg, 32) ||
        !compiler_scalar_source(&instruction[0].operands[1],
                                OPERAND_TYPE_TEMP, clip_reg, 1) ||
        !compiler_projection_scalar(&instruction[0].operands[2]) ||
        !compiler_destination(&instruction[1].operands[0],
                              OPERAND_TYPE_TEMP, clip_reg, 16 | 64) ||
        !compiler_plain_operand(&instruction[1].operands[1]) ||
        instruction[1].operands[1].type != OPERAND_TYPE_TEMP ||
        instruction[1].operands[1].register_index != clip_reg ||
        instruction[1].operands[1].swizzle_mode != 1 ||
        usil_operand_source_component(&instruction[1].operands[1], 0) != 0 ||
        usil_operand_source_component(&instruction[1].operands[1], 2) != 3 ||
        !compiler_plain_operand(&instruction[1].operands[2]) ||
        instruction[1].operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
        instruction[1].operands[2].imm_value_count != 4 ||
        instruction[1].operands[2].immediate_word_count != 4 ||
        instruction[1].operands[2].immediate_words[0] != 0x3f000000u ||
        instruction[1].operands[2].immediate_words[1] != 0u ||
        instruction[1].operands[2].immediate_words[2] != 0x3f000000u ||
        instruction[1].operands[2].immediate_words[3] != 0u ||
        !compiler_destination(&instruction[2].operands[0],
                              OPERAND_TYPE_TEMP, clip_reg, 128) ||
        !compiler_scalar_source(&instruction[2].operands[1],
                                OPERAND_TYPE_TEMP, clip_reg, 1) ||
        !compiler_half_scalar(&instruction[2].operands[2]))
        return false;

    const int screen_output = instruction[3].operands[0].register_index;
    if (screen_output < 0 ||
        !compiler_destination(&instruction[3].operands[0],
                              OPERAND_TYPE_OUTPUT, screen_output,
                              16 | 32) ||
        !compiler_scalar_source(&instruction[3].operands[1],
                                OPERAND_TYPE_TEMP, clip_reg, 2) ||
        !compiler_plain_operand(&instruction[3].operands[2]) ||
        instruction[3].operands[2].type != OPERAND_TYPE_TEMP ||
        instruction[3].operands[2].register_index != clip_reg ||
        instruction[3].operands[2].swizzle_mode != 1 ||
        usil_operand_source_component(&instruction[3].operands[2], 0) != 0 ||
        usil_operand_source_component(&instruction[3].operands[2], 1) != 3)
        return false;

    *out_work_reg = clip_reg;
    return true;
}

static bool compiler_screen_sequence_inplace_scratch_matches(
    const USILProgram *program, int start, int clip_reg,
    int *out_work_reg) {
    if (!program || !out_work_reg || start < 0 ||
        start + 3 >= program->instruction_count)
        return false;
    const USILInstruction *instruction = program->instructions + start;
    if (!compiler_instruction_shape(&instruction[0], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[1], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[2], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[3], USIL_OP_ADD, 3))
        return false;

    const int scratch_reg = instruction[0].operands[0].register_index;
    int scratch_lane = -1;
    for (int lane = 0; lane < 4; lane++) {
        if (instruction[0].operands[0].destination_mask == (16 << lane)) {
            scratch_lane = lane;
            break;
        }
    }
    if (scratch_reg < 0 || scratch_reg == clip_reg || scratch_lane < 0 ||
        !compiler_destination(&instruction[0].operands[0],
                              OPERAND_TYPE_TEMP, scratch_reg,
                              16 << scratch_lane) ||
        !compiler_scalar_source(&instruction[0].operands[1],
                                OPERAND_TYPE_TEMP, clip_reg, 1) ||
        !compiler_projection_scalar(&instruction[0].operands[2]) ||
        !compiler_destination(&instruction[1].operands[0],
                              OPERAND_TYPE_TEMP, clip_reg, 16 | 64) ||
        !compiler_plain_operand(&instruction[1].operands[1]) ||
        instruction[1].operands[1].type != OPERAND_TYPE_TEMP ||
        instruction[1].operands[1].register_index != clip_reg ||
        instruction[1].operands[1].swizzle_mode != 1 ||
        usil_operand_source_component(&instruction[1].operands[1], 0) != 0 ||
        usil_operand_source_component(&instruction[1].operands[1], 2) != 3 ||
        !compiler_plain_operand(&instruction[1].operands[2]) ||
        instruction[1].operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
        instruction[1].operands[2].imm_value_count != 4 ||
        instruction[1].operands[2].immediate_word_count != 4 ||
        instruction[1].operands[2].immediate_words[0] != 0x3f000000u ||
        instruction[1].operands[2].immediate_words[1] != 0u ||
        instruction[1].operands[2].immediate_words[2] != 0x3f000000u ||
        instruction[1].operands[2].immediate_words[3] != 0u ||
        !compiler_destination(&instruction[2].operands[0],
                              OPERAND_TYPE_TEMP, clip_reg, 128) ||
        !compiler_scalar_source(&instruction[2].operands[1],
                                OPERAND_TYPE_TEMP, scratch_reg,
                                scratch_lane) ||
        !compiler_half_scalar(&instruction[2].operands[2]))
        return false;

    const int screen_output = instruction[3].operands[0].register_index;
    if (screen_output < 0 ||
        !compiler_destination(&instruction[3].operands[0],
                              OPERAND_TYPE_OUTPUT, screen_output,
                              16 | 32) ||
        !compiler_scalar_source(&instruction[3].operands[1],
                                OPERAND_TYPE_TEMP, clip_reg, 2) ||
        !compiler_plain_operand(&instruction[3].operands[2]) ||
        instruction[3].operands[2].type != OPERAND_TYPE_TEMP ||
        instruction[3].operands[2].register_index != clip_reg ||
        instruction[3].operands[2].swizzle_mode != 1 ||
        usil_operand_source_component(&instruction[3].operands[2], 0) != 0 ||
        usil_operand_source_component(&instruction[3].operands[2], 1) != 3 ||
        !compiler_temp_value_is_dead_after(program, start + 4, scratch_reg,
                                           1u << scratch_lane))
        return false;

    *out_work_reg = clip_reg;
    return true;
}

static bool compiler_clip_output_copy_matches(
    const USILInstruction *instruction, int clip_reg) {
    if (!compiler_instruction_shape(instruction, USIL_OP_MOV, 2) ||
        instruction->operands[0].type != OPERAND_TYPE_OUTPUT ||
        instruction->operands[0].destination_mask == 0 ||
        !compiler_plain_operand(&instruction->operands[0]) ||
        !compiler_plain_operand(&instruction->operands[1]) ||
        instruction->operands[1].type != OPERAND_TYPE_TEMP ||
        instruction->operands[1].register_index != clip_reg)
        return false;
    unsigned int destination_lanes = 0u;
    for (int lane = 0; lane < 4; lane++) {
        if ((instruction->operands[0].destination_mask & (16 << lane)) != 0)
            destination_lanes++;
    }
    unsigned int source_lanes = 0u;
    if (instruction->operands[1].swizzle_mode == 2) {
        source_lanes = 1u;
    } else if (instruction->operands[1].swizzle_mode == 1) {
        source_lanes = destination_lanes;
    }
    return source_lanes == destination_lanes;
}

static bool compiler_screen_sequence_matches(const USILProgram *program,
                                             int start, int clip_reg,
                                             int *out_work_reg,
                                             int *out_instruction_count,
                                             unsigned char *out_pattern) {
    if (!out_instruction_count || !out_pattern) return false;
    if (compiler_screen_sequence_combined_matches(program, start, clip_reg,
                                                  out_work_reg)) {
        *out_instruction_count = 4;
        *out_pattern = HLSL_COMPILER_SCREEN_COMBINED;
        return true;
    }
    if (compiler_screen_sequence_split_matches(program, start, clip_reg,
                                               out_work_reg)) {
        *out_instruction_count = 5;
        *out_pattern = HLSL_COMPILER_SCREEN_SPLIT;
        return true;
    }
    if (compiler_screen_sequence_inplace_matches(program, start, clip_reg,
                                                 out_work_reg)) {
        *out_instruction_count = 4;
        *out_pattern = HLSL_COMPILER_SCREEN_INPLACE;
        return true;
    }
    if (compiler_screen_sequence_inplace_scratch_matches(
            program, start, clip_reg, out_work_reg)) {
        *out_instruction_count = 4;
        *out_pattern = HLSL_COMPILER_SCREEN_INPLACE_SCRATCH;
        return true;
    }
    return false;
}

static bool compiler_screen_position_matches(
    const USILProgram *program, int clip_instruction,
    int *out_screen_instruction, int *out_screen_instruction_count,
    unsigned char *out_pattern) {
    static const int identity[3] = {0, 1, 2};
    if (!program || !out_screen_instruction ||
        !out_screen_instruction_count || clip_instruction < 0 ||
        !out_pattern ||
        clip_instruction + 5 >= program->instruction_count)
        return false;
    const USILInstruction *clip =
        &program->instructions[clip_instruction];
    const USILInstruction *copy = clip + 1;
    if (!compiler_instruction_shape(clip, USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(copy, USIL_OP_MOV, 2))
        return false;
    const int clip_reg = clip->operands[0].register_index;
    const int position_output = copy->operands[0].register_index;
    if (clip_reg < 0 || position_output < 0 ||
        !compiler_destination(&clip->operands[0], OPERAND_TYPE_TEMP,
                              clip_reg, 16 | 32 | 64 | 128) ||
        !compiler_plain_operand(&clip->operands[1]) ||
        clip->operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
        clip->operands[1].register_index_dim != 2 ||
        !clip->operands[1].index_has_immediate[0] ||
        !clip->operands[1].index_has_immediate[1] ||
        clip->operands[1].index_value_exceeds_int[0] ||
        clip->operands[1].index_value_exceeds_int[1] ||
        clip->operands[1].swizzle_mode != 1 ||
        clip->operands[1].swizzle[0] != 0 ||
        clip->operands[1].swizzle[1] != 1 ||
        clip->operands[1].swizzle[2] != 2 ||
        clip->operands[1].swizzle[3] != 3 ||
        !compiler_plain_operand(&clip->operands[2]) ||
        clip->operands[2].type != OPERAND_TYPE_TEMP ||
        !is_replicate_swizzle(&clip->operands[2]) ||
        !compiler_plain_operand(&clip->operands[3]) ||
        clip->operands[3].type != OPERAND_TYPE_TEMP ||
        clip->operands[3].swizzle_mode != 1 ||
        clip->operands[3].swizzle[0] != identity[0] ||
        clip->operands[3].swizzle[1] != identity[1] ||
        clip->operands[3].swizzle[2] != identity[2] ||
        clip->operands[3].swizzle[3] != 3 ||
        !compiler_destination(&copy->operands[0], OPERAND_TYPE_OUTPUT,
                              position_output, 16 | 32 | 64 | 128) ||
        !compiler_plain_operand(&copy->operands[1]) ||
        copy->operands[1].type != OPERAND_TYPE_TEMP ||
        copy->operands[1].register_index != clip_reg ||
        copy->operands[1].swizzle_mode != 1 ||
        copy->operands[1].swizzle[0] != 0 ||
        copy->operands[1].swizzle[1] != 1 ||
        copy->operands[1].swizzle[2] != 2 ||
        copy->operands[1].swizzle[3] != 3)
        return false;

    int match = -1;
    int work_reg = -1;
    int match_count = 0;
    unsigned char match_pattern = HLSL_COMPILER_SCREEN_COMBINED;
    for (int candidate = clip_instruction + 2;
         candidate + 3 < program->instruction_count; candidate++) {
        int candidate_work = -1;
        int candidate_count = 0;
        unsigned char candidate_pattern = HLSL_COMPILER_SCREEN_COMBINED;
        if (!compiler_screen_sequence_matches(program, candidate, clip_reg,
                                              &candidate_work,
                                              &candidate_count,
                                              &candidate_pattern))
            continue;
        if (match >= 0) return false;
        match = candidate;
        work_reg = candidate_work;
        match_count = candidate_count;
        match_pattern = candidate_pattern;
    }
    if (match < 0) return false;
    unsigned int original_lanes = 0x0fu;
    unsigned int screen_required_lanes =
        compiler_screen_pattern_is_inplace(match_pattern) ? 0x0bu : 0x0fu;
    for (int index = clip_instruction + 2; index < match; index++) {
        const USILInstruction *instruction = &program->instructions[index];
        unsigned int reads = 0u;
        unsigned int writes = 0u;
        if (!compiler_instruction_temp_access_masks(
                program, instruction, clip_reg, &reads, &writes)) {
            return false;
        }
        if (compiler_clip_output_copy_matches(instruction, clip_reg)) {
            if ((reads & ~original_lanes) != 0u) return false;
        } else if ((reads & original_lanes) != 0u) {
            return false;
        }
        original_lanes &= ~writes;
    }
    if ((original_lanes & screen_required_lanes) != screen_required_lanes)
        return false;
    if (!compiler_temp_value_is_dead_after(program, match + match_count,
                                           clip_reg,
                                           0x0fu) ||
        !compiler_temp_value_is_dead_after(program, match + match_count,
                                           work_reg,
                                           0x0du))
        return false;
    *out_screen_instruction = match;
    *out_screen_instruction_count = match_count;
    *out_pattern = match_pattern;
    return true;
}

bool hlsl_compiler_screen_position_matches(const USILProgram *program,
                                           int clip_instruction,
                                           int *out_screen_instruction) {
    int screen_instruction = -1;
    int screen_instruction_count = 0;
    unsigned char pattern = HLSL_COMPILER_SCREEN_COMBINED;
    bool matches = compiler_screen_position_matches(
        program, clip_instruction, &screen_instruction,
        &screen_instruction_count, &pattern);
    if (matches && out_screen_instruction)
        *out_screen_instruction = screen_instruction;
    return matches;
}

/* D3DCompiler lowers a two-component homogeneous projection into this exact
 * physical graph when the projection-scaled source lane is also consumed by
 * independent arithmetic:
 *
 *   mul clip.y, clip.y, projection
 *   mul packed.xzw, clip.xxwy, { .5, 0, .5, .5 }
 *   ... straight-line instructions independent of packed.x/z/w ...
 *   add output.zw, packed.z, packed.xw
 *
 * This is a compiler scheduling/lane-allocation normal form, not a shader or
 * helper-name heuristic.  Every token shape and every removed temporary lane
 * is proven before the inverse is admitted. */
static bool compiler_interleaved_projection_pack_matches(
    const USILProgram *program, int start,
    HLSLCompilerInterleavedProjectionPack *out_pack) {
    if (!program || !out_pack || start < 0 ||
        start + 3 >= program->instruction_count) {
        return false;
    }
    const USILInstruction *scale = &program->instructions[start];
    const USILInstruction *pack = scale + 1;
    if (!compiler_instruction_shape(scale, USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(pack, USIL_OP_MUL, 3)) {
        return false;
    }
    const int clip_reg = scale->operands[0].register_index;
    const int packed_reg = pack->operands[0].register_index;
    const DXBCOperand *packed_source = &pack->operands[1];
    const DXBCOperand *half = &pack->operands[2];
    if (clip_reg < 0 || packed_reg < 0 || packed_reg == clip_reg ||
        !compiler_destination(&scale->operands[0], OPERAND_TYPE_TEMP,
                              clip_reg, 32) ||
        !compiler_scalar_source(&scale->operands[1], OPERAND_TYPE_TEMP,
                                clip_reg, 1) ||
        scale->operands[2].register_index < 0 ||
        !compiler_projection_scalar(&scale->operands[2]) ||
        !compiler_destination(&pack->operands[0], OPERAND_TYPE_TEMP,
                              packed_reg, 16 | 64 | 128) ||
        !compiler_plain_operand(packed_source) ||
        packed_source->type != OPERAND_TYPE_TEMP ||
        packed_source->register_index != clip_reg ||
        packed_source->swizzle_mode != 1 ||
        packed_source->swizzle[0] != 0 ||
        packed_source->swizzle[1] != 0 ||
        packed_source->swizzle[2] != 3 ||
        packed_source->swizzle[3] != 1 ||
        !compiler_plain_operand(half) ||
        half->type != OPERAND_TYPE_IMMEDIATE32 ||
        half->imm_value_count != 4 || half->immediate_word_count != 4 ||
        half->immediate_words[0] != 0x3f000000u ||
        half->immediate_words[1] != 0u ||
        half->immediate_words[2] != 0x3f000000u ||
        half->immediate_words[3] != 0x3f000000u) {
        return false;
    }

    const unsigned int packed_lanes = 0x0du;
    int add_instruction = -1;
    for (int candidate = start + 2;
         candidate < program->instruction_count; ++candidate) {
        const USILInstruction *instruction =
            &program->instructions[candidate];
        if (compiler_opcode_is_control_flow(instruction->opcode)) break;
        const DXBCOperand *offset =
            instruction->operand_count >= 3 ? &instruction->operands[2]
                                             : NULL;
        const int output_reg = instruction->operand_count >= 1
            ? instruction->operands[0].register_index : -1;
        if (candidate > start + 2 &&
            compiler_instruction_shape(instruction, USIL_OP_ADD, 3) &&
            output_reg >= 0 &&
            compiler_destination(&instruction->operands[0],
                                 OPERAND_TYPE_OUTPUT,
                                 output_reg,
                                 64 | 128) &&
            compiler_scalar_source(&instruction->operands[1],
                                   OPERAND_TYPE_TEMP, packed_reg, 2) &&
            compiler_plain_operand(offset) &&
            offset->type == OPERAND_TYPE_TEMP &&
            offset->register_index == packed_reg &&
            offset->swizzle_mode == 1 && offset->swizzle[0] == 0 &&
            offset->swizzle[1] == 0 && offset->swizzle[2] == 0 &&
            offset->swizzle[3] == 3) {
            add_instruction = candidate;
            break;
        }
        unsigned int reads = 0u;
        unsigned int writes = 0u;
        if (!compiler_instruction_temp_access_masks(
                program, instruction, packed_reg, &reads, &writes) ||
            ((reads | writes) & packed_lanes) != 0u) {
            return false;
        }
    }
    if (add_instruction < 0 ||
        !compiler_temp_value_is_dead_in_linear_tail(
            program, add_instruction + 1, packed_reg, packed_lanes)) {
        return false;
    }
    *out_pack = (HLSLCompilerInterleavedProjectionPack){
        .valid = true,
        .phase = 0,
        .scale_instruction = start,
        .pack_instruction = start + 1,
        .add_instruction = add_instruction};
    return true;
}

static bool replacement_instructions_available(
    const HLSLEmitterContext *ctx, const int *instructions, int count) {
    const HLSLCompilerModelProgram *model = &ctx->compiler_model;
    int instruction_count = ctx->program->instruction_count;
    for (int item = 0; item < count; item++) {
        int instruction = instructions[item];
        if (instruction < 0 || instruction >= instruction_count ||
            model->claim_owner[instruction] >= 0 ||
            (ctx->semantic_program.claim_owner &&
             ctx->semantic_program.claim_owner[instruction] >= 0)) {
            return false;
        }
    }
    return true;
}

static bool claim_replacement(HLSLEmitterContext *ctx, int trigger,
                              const int *instructions, int count,
                              bool suppress_claimed) {
    HLSLCompilerModelProgram *model = &ctx->compiler_model;
    if (!replacement_instructions_available(ctx, instructions, count)) {
        model->conflict_count++;
        return false;
    }
    int owner = model->replacement_count++;
    for (int item = 0; item < count; item++) {
        int instruction = instructions[item];
        model->claim_owner[instruction] = owner;
        if (suppress_claimed && instruction != trigger)
            ctx->skip_instruction[instruction] = true;
    }
    return true;
}

/* High-level mul(matrix, vector) is an exact inverse only for the ordinary
 * column-major float4x4 declaration that generated this physical row chain.
 * Serialized row-major shells and arrays have different source semantics and
 * must continue through the flat emitter. */
static const char *compiler_column_major_float4x4_identifier(
    const HLSLEmitterContext *ctx, int buffer, int first_row) {
    if (!ctx || buffer < 0 || first_row < 0) return NULL;
    int offset = -1;
    const char *name = resolve_cb_variable_ctx(
        ctx, buffer, first_row, -1, &offset);
    if (!name || offset != 0) return NULL;
    for (int row = 1; row < 4; row++) {
        int row_offset = -1;
        const char *row_name = resolve_cb_variable_ctx(
            ctx, buffer, first_row + row, -1, &row_offset);
        if (!row_name || strcmp(row_name, name) != 0 ||
            row_offset != row * 16) {
            return NULL;
        }
    }
    DecodedVariableLayout layout;
    if (!resolve_variable_layout_ctx(ctx, name, &layout) ||
        !layout.is_matrix || layout.rows != 4 || layout.columns != 4 ||
        layout.scalar_type != 0 || layout.array_size != 0 ||
        layout.byte_offset != (uint32_t)first_row * 16u ||
        resolve_variable_is_row_major(ctx, name)) {
        return NULL;
    }
    return name;
}

static void register_split_matrix_transform_replacements(
    HLSLEmitterContext *ctx) {
    const int count = ctx->program->instruction_count;
    for (int clip = 0; clip + 11 < count; clip++) {
        HLSLCompilerSplitMatrixTransform transform;
        if (!compiler_split_matrix_transform_matches(
                ctx->program, clip, &transform)) {
            continue;
        }
        const char *object_matrix =
            compiler_column_major_float4x4_identifier(
                ctx, transform.object_matrix_buffer,
                transform.object_matrix_first_row);
        const char *clip_matrix =
            compiler_column_major_float4x4_identifier(
                ctx, transform.clip_matrix_buffer,
                transform.clip_matrix_first_row);
        if (!object_matrix || !clip_matrix ||
            strcmp(object_matrix, clip_matrix) == 0) {
            continue;
        }

        int clip_claimed[8];
        int world_claimed[4];
        for (int item = 0; item < 8; item++)
            clip_claimed[item] = clip + item;
        for (int item = 0; item < 4; item++)
            world_claimed[item] = transform.world_instruction + item;
        if (!replacement_instructions_available(ctx, clip_claimed, 8) ||
            !replacement_instructions_available(ctx, world_claimed, 4)) {
            continue;
        }
        if (!claim_replacement(ctx, clip, clip_claimed, 8, true) ||
            !claim_replacement(ctx, transform.world_instruction,
                               world_claimed, 4, true)) {
            continue;
        }
        ctx->compiler_model.split_matrix_transforms[clip] = transform;
        transform.phase = 1;
        ctx->compiler_model.split_matrix_transforms[
            transform.world_instruction] = transform;
        clip = transform.world_instruction + 3;
    }
}

static void register_tangent_frame_replacements(HLSLEmitterContext *ctx) {
    const int count = ctx->program->instruction_count;
    for (int start = 0; start + 19 < count; start++) {
        HLSLCompilerTangentFrame frame;
        if (!compiler_tangent_frame_matches(ctx->program, start, &frame))
            continue;
        int claimed[19];
        for (int item = 0; item < 19; item++) claimed[item] = start + item;
        if (!claim_replacement(ctx, start, claimed, 19, true)) continue;
        ctx->compiler_model.tangent_frames[start] = frame;
        start = frame.end_instruction;
    }
}

static void register_screen_position_replacements(HLSLEmitterContext *ctx) {
    const int count = ctx->program->instruction_count;
    for (int clip = 0; clip + 5 < count; clip++) {
        int screen = -1;
        int screen_count = 0;
        unsigned char pattern = HLSL_COMPILER_SCREEN_COMBINED;
        if (!compiler_screen_position_matches(ctx->program, clip, &screen,
                                              &screen_count, &pattern))
            continue;
        int clip_claimed[2] = {clip, clip + 1};
        if (!replacement_instructions_available(ctx, clip_claimed, 2))
            continue;
        int screen_claimed[5];
        for (int item = 0; item < screen_count; item++)
            screen_claimed[item] = screen + item;
        if (!replacement_instructions_available(ctx, screen_claimed,
                                                screen_count))
            continue;
        bool consumers_available = true;
        const int clip_reg =
            ctx->program->instructions[clip].operands[0].register_index;
        for (int index = clip + 2; index < screen; index++) {
            if (!compiler_clip_output_copy_matches(
                    &ctx->program->instructions[index], clip_reg))
                continue;
            if (!replacement_instructions_available(ctx, &index, 1)) {
                consumers_available = false;
                break;
            }
        }
        if (!consumers_available) continue;
        if (!claim_replacement(ctx, clip, clip_claimed, 2, true) ||
            !claim_replacement(ctx, screen, screen_claimed, screen_count,
                               true))
            continue;
        ctx->compiler_model.screen_positions[clip] =
            (HLSLCompilerScreenPosition){
                .valid = true, .phase = 0, .pattern = pattern,
                .clip_instruction = clip,
                .screen_instruction = screen,
                .screen_instruction_count = screen_count};
        ctx->compiler_model.screen_positions[screen] =
            (HLSLCompilerScreenPosition){
                .valid = true, .phase = 1, .pattern = pattern,
                .clip_instruction = clip,
                .screen_instruction = screen,
                .screen_instruction_count = screen_count};
        for (int index = clip + 2; index < screen; index++) {
            if (!compiler_clip_output_copy_matches(
                    &ctx->program->instructions[index], clip_reg))
                continue;
            if (!claim_replacement(ctx, index, &index, 1, true)) break;
            ctx->compiler_model.screen_positions[index] =
                (HLSLCompilerScreenPosition){
                    .valid = true, .phase = 2, .pattern = pattern,
                    .clip_instruction = clip,
                    .screen_instruction = screen,
                    .screen_instruction_count = screen_count};
        }
        clip = screen + screen_count - 1;
    }
}

static void register_interleaved_projection_pack_replacements(
    HLSLEmitterContext *ctx) {
    const int count = ctx->program->instruction_count;
    for (int start = 0; start + 3 < count; ++start) {
        HLSLCompilerInterleavedProjectionPack pack;
        if (!compiler_interleaved_projection_pack_matches(
                ctx->program, start, &pack)) {
            continue;
        }
        int claimed[3] = {pack.scale_instruction, pack.pack_instruction,
                          pack.add_instruction};
        if (!claim_replacement(ctx, start, claimed, 3, true)) continue;
        ctx->compiler_model.interleaved_projection_packs[start] = pack;
        pack.phase = 1;
        ctx->compiler_model.interleaved_projection_packs[
            pack.pack_instruction] = pack;
        ctx->compiler_model.interleaved_projection_packs[
            pack.add_instruction] = pack;
    }
}

static void register_instruction_replacements(HLSLEmitterContext *ctx) {
    int count = ctx->program->instruction_count;
    register_split_matrix_transform_replacements(ctx);
    register_tangent_frame_replacements(ctx);
    register_screen_position_replacements(ctx);
    register_interleaved_projection_pack_replacements(ctx);
    for (int index = 0; index < count; index++) {
        if (ctx->compiler_model.claim_owner[index] >= 0) continue;
        if (ctx->modulo_divisor && ctx->modulo_divisor[index] > 0) {
            int claimed[] = {index, index + 1, index + 2, index + 3,
                             index + 4};
            claim_replacement(ctx, index, claimed, 5, true);
        }
        const LoopOptimizationInfo *loop =
            ctx->loop_info ? &ctx->loop_info[index] : NULL;
        if (loop && loop->is_optimized) {
            int claimed[4] = {index, loop->comparison_inst_idx,
                              loop->breakc_inst_idx, loop->inc_inst_idx};
            int claim_count = loop->inc_inst_idx >= 0 ? 4 : 3;
            claim_replacement(ctx, index, claimed, claim_count, true);
        }
        if (ctx->cross_info && ctx->cross_info[index].is_cross_mad) {
            int claimed[] = {ctx->cross_info[index].source_instruction,
                             index};
            claim_replacement(ctx, index, claimed, 2, false);
        }
    }
}

static bool is_written_by_previous(const USILInstruction *previous,
                                   const DXBCOperand *operand) {
    if (!previous || !operand || previous->operand_count < 1 ||
        (previous->operands[0].type != OPERAND_TYPE_TEMP &&
         previous->operands[0].type != OPERAND_TYPE_INDEXABLE_TEMP) ||
        previous->operands[0].type != operand->type ||
        previous->operands[0].register_index != operand->register_index) {
        return false;
    }
    int write_mask = previous->operands[0].destination_mask;
    int read_mask = 0;
    if (operand->swizzle_mode == 2) {
        read_mask = 1 << operand->swizzle[0];
    } else if (operand->swizzle_mode == 1) {
        for (int component = 0; component < 4; component++)
            read_mask |= 1 << operand->swizzle[component];
    } else {
        read_mask = 15;
    }
    int write_bits = 0;
    for (int component = 0; component < 4; component++)
        if (write_mask & (16 << component)) write_bits |= 1 << component;
    return (write_bits & read_mask) != 0;
}

/* Exact two-instruction cross-product lowering for the operand-order corner
 * where the immediately scheduled definition is the second MUL source.
 * D3DCompiler performs commutative canonicalization after scheduling that
 * dependent subtree, so spelling the dependent source first is required to
 * serialize the decoded source order. */
static bool compiler_cross_product_mul_reverses_source(
    const USILProgram *program, int index) {
    static const int yzx[3] = {1, 2, 0};
    static const int zxy[3] = {2, 0, 1};
    static const int identity[3] = {0, 1, 2};
    if (!program || index < 1 || index + 1 >= program->instruction_count)
        return false;
    const USILInstruction *previous = &program->instructions[index - 1];
    const USILInstruction *mul = &program->instructions[index];
    const USILInstruction *mad = &program->instructions[index + 1];
    if (!compiler_instruction_shape(mul, USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(mad, USIL_OP_MAD, 4)) {
        return false;
    }
    const int scratch_reg = mul->operands[0].register_index;
    const int left_reg = mul->operands[1].register_index;
    const int dependent_reg = mul->operands[2].register_index;
    if (scratch_reg < 0 || left_reg < 0 || dependent_reg < 0 ||
        scratch_reg == left_reg || scratch_reg == dependent_reg ||
        left_reg == dependent_reg ||
        !compiler_destination(&mul->operands[0], OPERAND_TYPE_TEMP,
                              scratch_reg, HLSL_XYZ_MASK) ||
        !compiler_vector_source(&mul->operands[1], OPERAND_TYPE_TEMP,
                                left_reg, yzx) ||
        !compiler_vector_source(&mul->operands[2], OPERAND_TYPE_TEMP,
                                dependent_reg, zxy) ||
        !compiler_destination(&mad->operands[0], OPERAND_TYPE_TEMP,
                              left_reg, HLSL_XYZ_MASK) ||
        !compiler_vector_source(&mad->operands[1], OPERAND_TYPE_TEMP,
                                dependent_reg, yzx) ||
        !compiler_vector_source(&mad->operands[2], OPERAND_TYPE_TEMP,
                                left_reg, zxy) ||
        !compiler_negated_vector_source(&mad->operands[3],
                                        OPERAND_TYPE_TEMP, scratch_reg,
                                        identity)) {
        return false;
    }
    unsigned int dependent_reads = 0u;
    unsigned int dependent_writes = 0u;
    unsigned int left_reads = 0u;
    unsigned int left_writes = 0u;
    return compiler_instruction_temp_access_masks(
               program, previous, dependent_reg, &dependent_reads,
               &dependent_writes) &&
           compiler_instruction_temp_access_masks(
               program, previous, left_reg, &left_reads, &left_writes) &&
           dependent_writes == 0x07u && left_writes == 0u &&
           is_written_by_previous(previous, &mul->operands[2]) &&
           !is_written_by_previous(previous, &mul->operands[1]);
}

bool hlsl_compiler_cross_product_mul_reverses_source(
    const USILProgram *program, int instruction) {
    return compiler_cross_product_mul_reverses_source(program, instruction);
}

static bool is_commutative(USILOpcode opcode) {
    switch (opcode) {
        case USIL_OP_ADD:
        case USIL_OP_IADD:
        case USIL_OP_MUL:
        case USIL_OP_IMUL:
        case USIL_OP_MAX:
        case USIL_OP_MIN:
        case USIL_OP_IMAX:
        case USIL_OP_IMIN:
        case USIL_OP_UMAX:
        case USIL_OP_UMIN:
        case USIL_OP_AND:
        case USIL_OP_OR:
        case USIL_OP_XOR:
        case USIL_OP_EQ:
        case USIL_OP_NE:
        case USIL_OP_IEQ:
        case USIL_OP_INE:
            return true;
        default:
            return false;
    }
}

static bool base_binary_order(const USILInstruction *inst,
                              const USILInstruction *previous) {
    if (!is_commutative(inst->opcode) || inst->operand_count < 3) return false;
    bool literal0 = inst->operands[1].type == OPERAND_TYPE_IMMEDIATE32;
    bool literal1 = inst->operands[2].type == OPERAND_TYPE_IMMEDIATE32;
    if (literal0 || literal1) {
        if (inst->opcode == USIL_OP_EQ || inst->opcode == USIL_OP_IEQ ||
            inst->opcode == USIL_OP_NE || inst->opcode == USIL_OP_INE)
            return literal0;
        return false;
    }
    bool scalar0 = is_replicate_swizzle(&inst->operands[1]);
    bool scalar1 = is_replicate_swizzle(&inst->operands[2]);
    if (scalar0 != scalar1) return true;
    bool temp0 = inst->operands[1].type == OPERAND_TYPE_TEMP ||
                 inst->operands[1].type == OPERAND_TYPE_INDEXABLE_TEMP;
    bool temp1 = inst->operands[2].type == OPERAND_TYPE_TEMP ||
                 inst->operands[2].type == OPERAND_TYPE_INDEXABLE_TEMP;
    if (inst->opcode == USIL_OP_MUL && temp0 && temp1 &&
        inst->operands[1].type == inst->operands[2].type &&
        inst->operands[1].register_index == inst->operands[2].register_index) {
        /* Component order alone is not authoritative when one lane was
         * produced by the immediately preceding instruction.  D3DCompiler's
         * expression scheduler treats that lane as the dependent subtree and
         * applies its commutative-source canonicalization after scheduling.
         * Preserve that exact local dependency before falling back to the
         * stable component-order rule. */
        bool dependency0 =
            is_written_by_previous(previous, &inst->operands[1]);
        bool dependency1 =
            is_written_by_previous(previous, &inst->operands[2]);
        if (dependency0 != dependency1) return true;
        return is_non_canonical_add(&inst->operands[1],
                                    &inst->operands[2]);
    }
    int write_mask = inst->operands[0].destination_mask;
    if (has_non_contiguous_swizzle(&inst->operands[1], write_mask) ||
        has_non_contiguous_swizzle(&inst->operands[2], write_mask))
        return false;
    bool destination_temp = inst->operands[0].type == OPERAND_TYPE_TEMP ||
                            inst->operands[0].type == OPERAND_TYPE_INDEXABLE_TEMP;
    if (destination_temp && temp0 && temp1) {
        if (inst->operands[1].register_index ==
            inst->operands[0].register_index)
            return true;
        if (inst->operands[2].register_index ==
            inst->operands[0].register_index) {
            bool dependency0 =
                is_written_by_previous(previous, &inst->operands[1]);
            bool dependency1 =
                is_written_by_previous(previous, &inst->operands[2]);
            if (dependency0 != dependency1) return true;
            return inst->opcode == USIL_OP_ADD;
        }
    }
    bool dependency0 = is_written_by_previous(previous, &inst->operands[1]);
    bool dependency1 = is_written_by_previous(previous, &inst->operands[2]);
    if (dependency0 != dependency1) return true;
    return is_non_canonical_add(&inst->operands[1], &inst->operands[2]);
}

static bool is_saturated_cubic_mul(const USILProgram *program, int index) {
    const USILInstruction *inst = &program->instructions[index];
    if (!inst->saturate || index < 1 || inst->operand_count < 3 ||
        inst->operands[0].type != OPERAND_TYPE_TEMP ||
        inst->operands[1].type != OPERAND_TYPE_TEMP ||
        inst->operands[2].type != OPERAND_TYPE_TEMP ||
        inst->operands[0].register_index != inst->operands[1].register_index ||
        inst->operands[1].register_index != inst->operands[2].register_index ||
        inst->operands[1].swizzle_mode != 2 ||
        inst->operands[2].swizzle_mode != 2)
        return false;
    const USILInstruction *square = &program->instructions[index - 1];
    if (square->opcode != USIL_OP_MUL || square->operand_count < 3 ||
        square->operands[0].type != OPERAND_TYPE_TEMP ||
        square->operands[1].type != OPERAND_TYPE_TEMP ||
        square->operands[2].type != OPERAND_TYPE_TEMP ||
        square->operands[0].register_index != inst->operands[0].register_index ||
        square->operands[1].register_index != inst->operands[0].register_index ||
        square->operands[2].register_index != inst->operands[0].register_index ||
        square->operands[1].swizzle_mode != 2 ||
        square->operands[2].swizzle_mode != 2)
        return false;
    int component = 0;
    while (component < 4 &&
           !(square->operands[0].destination_mask & (16 << component)))
        component++;
    return component == inst->operands[2].swizzle[0] &&
           square->operands[1].swizzle[0] == inst->operands[1].swizzle[0] &&
           square->operands[2].swizzle[0] == inst->operands[1].swizzle[0];
}

/* D3DCompiler keeps this decoded three-lane MAD as one vector expression.
 * Splitting it into scalar HLSL assignments prevents the compiler from
 * recreating the original vector MAD.  This is a compiler-lowering contract,
 * not a Unity variable-name semantic: the complete decision is carried by
 * the decoded destination mask, operand classes, and source swizzles. */
static bool is_vector_output_mad_lowering(const USILInstruction *inst) {
    if (!inst || inst->opcode != USIL_OP_MAD || inst->operand_count != 4 ||
        inst->operands[0].type != OPERAND_TYPE_OUTPUT ||
        inst->operands[0].destination_mask != (16 | 32 | 64) ||
        inst->operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
        inst->operands[2].type != OPERAND_TYPE_TEMP ||
        inst->operands[3].type != OPERAND_TYPE_TEMP ||
        inst->operands[2].register_index !=
            inst->operands[3].register_index ||
        !is_replicate_swizzle(&inst->operands[2]) ||
        inst->operands[2].swizzle[0] != 3 ||
        inst->operands[3].swizzle_mode != 1) {
        return false;
    }
    return inst->operands[1].swizzle_mode == 1 &&
           inst->operands[1].swizzle[0] == 0 &&
           inst->operands[1].swizzle[1] == 1 &&
           inst->operands[1].swizzle[2] == 2 &&
           inst->operands[3].swizzle[0] == 0 &&
           inst->operands[3].swizzle[1] == 1 &&
           inst->operands[3].swizzle[2] == 2;
}

/* Exact inverse of the seven-instruction quadratic SH tail emitted by
 * D3DCompiler.  The final three-lane MAD must remain one HLSL vector
 * expression or the compiler schedules its three components independently.
 * This proof does not use cbuffer or output names: it requires the complete
 * x^2-y^2 / xy,yz,zz,zx construction, three consecutive coefficient rows,
 * accumulator lanes, and final coefficient row/data-flow topology. */
static bool is_quadratic_sh_vector_output_mad_lowering(
    const USILProgram *program, int index) {
    static const int identity3[3] = {0, 1, 2};
    static const int identity4[4] = {0, 1, 2, 3};
    static const int yzzx[4] = {1, 2, 2, 0};
    static const int xyzz[4] = {0, 1, 2, 2};
    static const int xyzx[4] = {0, 1, 2, 0};
    if (!program || index < 6 || index >= program->instruction_count)
        return false;
    const USILInstruction *instruction = &program->instructions[index - 6];
    if (!compiler_instruction_shape(&instruction[0], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[1], USIL_OP_MAD, 4) ||
        !compiler_instruction_shape(&instruction[2], USIL_OP_MUL, 3) ||
        !compiler_instruction_shape(&instruction[3], USIL_OP_DP4, 3) ||
        !compiler_instruction_shape(&instruction[4], USIL_OP_DP4, 3) ||
        !compiler_instruction_shape(&instruction[5], USIL_OP_DP4, 3) ||
        !compiler_instruction_shape(&instruction[6], USIL_OP_MAD, 4))
        return false;

    const int scalar_reg = instruction[0].operands[0].register_index;
    int scalar_lane = -1;
    for (int lane = 0; lane < 4; lane++) {
        if (instruction[0].operands[0].destination_mask == (16 << lane)) {
            scalar_lane = lane;
            break;
        }
    }
    const int normal_reg = instruction[0].operands[1].register_index;
    if (scalar_reg < 0 || scalar_lane < 0 || normal_reg < 0 ||
        !compiler_destination(&instruction[0].operands[0],
                              OPERAND_TYPE_TEMP, scalar_reg,
                              16 << scalar_lane) ||
        !compiler_scalar_source(&instruction[0].operands[1],
                                OPERAND_TYPE_TEMP, normal_reg, 1) ||
        !compiler_scalar_source(&instruction[0].operands[2],
                                OPERAND_TYPE_TEMP, normal_reg, 1) ||
        !compiler_destination(&instruction[1].operands[0],
                              OPERAND_TYPE_TEMP, scalar_reg,
                              16 << scalar_lane) ||
        !compiler_scalar_source(&instruction[1].operands[1],
                                OPERAND_TYPE_TEMP, normal_reg, 0) ||
        !compiler_scalar_source(&instruction[1].operands[2],
                                OPERAND_TYPE_TEMP, normal_reg, 0))
        return false;
    DXBCOperand positive_scalar = instruction[1].operands[3];
    if (!positive_scalar.has_neg || positive_scalar.has_abs) return false;
    positive_scalar.has_neg = false;
    if (!compiler_scalar_source(&positive_scalar, OPERAND_TYPE_TEMP,
                                scalar_reg, scalar_lane) ||
        (scalar_reg == normal_reg && scalar_lane != 3))
        return false;

    const int product_reg = instruction[2].operands[0].register_index;
    if (product_reg < 0 ||
        !compiler_destination(&instruction[2].operands[0],
                              OPERAND_TYPE_TEMP, product_reg,
                              16 | 32 | 64 | 128) ||
        !compiler_vector4_source(&instruction[2].operands[1],
                                 OPERAND_TYPE_TEMP, normal_reg, yzzx) ||
        !compiler_vector4_source(&instruction[2].operands[2],
                                 OPERAND_TYPE_TEMP, normal_reg, xyzz))
        return false;

    const int accumulator_reg = instruction[3].operands[0].register_index;
    const int coefficient_buffer =
        instruction[3].operands[1].register_index;
    const int first_row = instruction[3].operands[1].rel_offset0;
    if (accumulator_reg < 0 || coefficient_buffer < 0 || first_row < 0)
        return false;
    for (int component = 0; component < 3; component++) {
        const USILInstruction *dot = &instruction[3 + component];
        if (!compiler_destination(&dot->operands[0], OPERAND_TYPE_TEMP,
                                  accumulator_reg, 16 << component) ||
            !compiler_static_cbuffer_row4(&dot->operands[1],
                                          coefficient_buffer,
                                          first_row + component,
                                          identity4) ||
            !compiler_vector4_source(&dot->operands[2], OPERAND_TYPE_TEMP,
                                     product_reg, identity4))
            return false;
    }

    const USILInstruction *output = &instruction[6];
    if (!compiler_destination(&output->operands[0], OPERAND_TYPE_OUTPUT,
                              output->operands[0].register_index,
                              HLSL_XYZ_MASK) ||
        !compiler_static_cbuffer_row4(&output->operands[1],
                                     coefficient_buffer, first_row + 3,
                                     xyzx) ||
        !compiler_scalar_source(&output->operands[2], OPERAND_TYPE_TEMP,
                                scalar_reg, scalar_lane) ||
        !compiler_vector_source(&output->operands[3], OPERAND_TYPE_TEMP,
                                accumulator_reg, identity3))
        return false;
    return true;
}

bool analyze_d3dcompiler_model(HLSLEmitterContext *ctx) {
    int count = ctx->program->instruction_count;
    ctx->compiler_model.swap_binary_operands =
        (signed char *)malloc((size_t)count * sizeof(signed char));
    ctx->compiler_model.preserve_vector_output =
        (unsigned char *)calloc((size_t)count, sizeof(unsigned char));
    ctx->compiler_model.tangent_frames =
        (HLSLCompilerTangentFrame *)calloc(
            (size_t)count, sizeof(HLSLCompilerTangentFrame));
    ctx->compiler_model.screen_positions =
        (HLSLCompilerScreenPosition *)calloc(
            (size_t)count, sizeof(HLSLCompilerScreenPosition));
    ctx->compiler_model.interleaved_projection_packs =
        (HLSLCompilerInterleavedProjectionPack *)calloc(
            (size_t)count,
            sizeof(HLSLCompilerInterleavedProjectionPack));
    ctx->compiler_model.split_matrix_transforms =
        (HLSLCompilerSplitMatrixTransform *)calloc(
            (size_t)count, sizeof(HLSLCompilerSplitMatrixTransform));
    ctx->compiler_model.claim_owner =
        (int *)malloc((size_t)count * sizeof(int));
    if (!ctx->compiler_model.swap_binary_operands ||
        !ctx->compiler_model.preserve_vector_output ||
        !ctx->compiler_model.tangent_frames ||
        !ctx->compiler_model.screen_positions ||
        !ctx->compiler_model.interleaved_projection_packs ||
        !ctx->compiler_model.split_matrix_transforms ||
        !ctx->compiler_model.claim_owner) {
        free_d3dcompiler_model(ctx);
        return false;
    }
    for (int index = 0; index < count; index++)
        ctx->compiler_model.claim_owner[index] = -1;
    register_instruction_replacements(ctx);
    for (int index = 0; index < count; index++) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        const USILInstruction *previous =
            index > 0 ? &ctx->program->instructions[index - 1] : NULL;
        bool swap = base_binary_order(inst, previous);
        if (compiler_cross_product_mul_reverses_source(ctx->program,
                                                       index) ||
            is_saturated_cubic_mul(ctx->program, index) ||
            semantic_binary_operand_order(ctx, index) == 1)
            swap = true;
        int alpha_order = semantic_alpha_clip_first_operand(ctx, index);
        if (alpha_order >= 0) {
            bool operand1_is_cb =
                inst->operand_count >= 2 &&
                inst->operands[1].type == OPERAND_TYPE_CONSTANT_BUFFER;
            swap = alpha_order ? !operand1_is_cb : operand1_is_cb;
        }
        ctx->compiler_model.swap_binary_operands[index] = swap ? 1 : 0;
        ctx->compiler_model.preserve_vector_output[index] =
            (is_vector_output_mad_lowering(inst) ||
             is_quadratic_sh_vector_output_mad_lowering(ctx->program,
                                                        index))
                ? 1u
                : 0u;
    }
    return ctx->compiler_model.conflict_count == 0;
}

bool build_d3dcompiler_model(HLSLEmitterContext *ctx) {
    /* Recompile mode permits two complete compiler inverses: the one-lane
     * comparison/BREAKC loop prefix and the fixed-cbuffer recurrent product
     * family.  Each recognizer proves its control/data-flow contract before
     * emission.  The remaining fragment-derived recognizers stay
     * readability-only. */
    if (ctx->modulo_divisor) detect_signed_modulo_pattern(ctx);
    if (ctx->loop_info) {
        detect_loop_patterns(ctx);
        detect_typed_integer_loop_bounds(ctx);
    }
    if (ctx->cross_info) detect_cross_product_patterns(ctx);
    if (ctx->saved_mul_id && ctx->saved_mul_is_definition &&
        ctx->saved_mul_reverse_definition)
        detect_reused_multiply_expressions(ctx);
    return analyze_d3dcompiler_model(ctx);
}

bool compiler_model_swaps_binary_operands(const HLSLEmitterContext *ctx,
                                          int instruction) {
    return ctx->compiler_model.swap_binary_operands && instruction >= 0 &&
           instruction < ctx->program->instruction_count &&
           ctx->compiler_model.swap_binary_operands[instruction] == 1;
}

bool compiler_model_preserves_vector_output(const HLSLEmitterContext *ctx,
                                            int instruction) {
    return ctx->compiler_model.preserve_vector_output && instruction >= 0 &&
           instruction < ctx->program->instruction_count &&
           ctx->compiler_model.preserve_vector_output[instruction] != 0u;
}

static DXBCOperand compiler_canonical_xyz_operand(
    const DXBCOperand *operand) {
    DXBCOperand canonical = *operand;
    canonical.swizzle_mode = 1;
    canonical.swizzle[0] = 0;
    canonical.swizzle[1] = 1;
    canonical.swizzle[2] = 2;
    canonical.swizzle[3] = 3;
    return canonical;
}

bool emit_compiler_split_matrix_transform(HLSLEmitterContext *ctx,
                                          int instruction_index) {
    if (!ctx || !ctx->compiler_model.split_matrix_transforms ||
        instruction_index < 0 ||
        instruction_index >= ctx->program->instruction_count) {
        return false;
    }
    const HLSLCompilerSplitMatrixTransform *transform =
        &ctx->compiler_model.split_matrix_transforms[instruction_index];
    if (!transform->valid) return false;

    const char *object_matrix =
        compiler_column_major_float4x4_identifier(
            ctx, transform->object_matrix_buffer,
            transform->object_matrix_first_row);
    const char *clip_matrix =
        compiler_column_major_float4x4_identifier(
            ctx, transform->clip_matrix_buffer,
            transform->clip_matrix_first_row);
    if (!object_matrix || !clip_matrix) {
        ctx->sb->failed = true;
        return true;
    }

    if (transform->phase == 0) {
        const USILInstruction *clip =
            ctx->program->instructions + transform->clip_instruction;
        DXBCOperand input = clip[0].operands[1];
        input.swizzle_mode = 1;
        for (int component = 0; component < 4; component++)
            input.swizzle[component] = (uint8_t)component;
        char input_name[128];
        char clip_output[128];
        format_operand_hlsl(ctx, &input, false, false, HLSL_XYZW_MASK,
                            false, input_name, sizeof(input_name));
        format_dest_operand_hlsl(ctx, &clip[7].operands[0], false, false,
                                 HLSL_XYZW_MASK, false, clip_output,
                                 sizeof(clip_output));
        if (ctx->sb->failed) return true;

        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "float4 dxbc_compiler_world_%d = mul(%s, %s);\n",
                   transform->clip_instruction, object_matrix, input_name);
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "%s = mul(%s, mul(%s, float4(%s.xyz, 1.0f)));\n",
                   clip_output, clip_matrix, object_matrix, input_name);
        return true;
    }

    if (transform->phase == 1) {
        const USILInstruction *world =
            &ctx->program->instructions[transform->world_instruction];
        char world_output[128];
        format_dest_operand_hlsl(ctx, &world->operands[0], false, false,
                                 HLSL_XYZ_MASK, false, world_output,
                                 sizeof(world_output));
        if (ctx->sb->failed) return true;
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb, "%s = dxbc_compiler_world_%d.xyz;\n",
                   world_output, transform->clip_instruction);
        return true;
    }

    ctx->sb->failed = true;
    return true;
}

bool emit_compiler_tangent_frame_lowering(HLSLEmitterContext *ctx,
                                          int instruction_index) {
    if (!ctx || !ctx->compiler_model.tangent_frames ||
        instruction_index < 0 ||
        instruction_index >= ctx->program->instruction_count)
        return false;
    const HLSLCompilerTangentFrame *frame =
        &ctx->compiler_model.tangent_frames[instruction_index];
    if (!frame->valid) return false;

    const USILInstruction *instruction =
        ctx->program->instructions + instruction_index;
    DXBCOperand normal_operand =
        compiler_canonical_xyz_operand(&instruction[11].operands[1]);
    DXBCOperand matrix_operands[3] = {
        compiler_canonical_xyz_operand(&instruction[0].operands[2]),
        compiler_canonical_xyz_operand(&instruction[1].operands[1]),
        compiler_canonical_xyz_operand(&instruction[2].operands[1])};
    DXBCOperand output_operands[3] = {
        instruction[10].operands[0], instruction[13].operands[0],
        instruction[14].operands[0]};
    for (int output = 0; output < 3; output++)
        output_operands[output].destination_mask = HLSL_XYZ_MASK;

    char normal[128];
    char matrix_rows[3][256];
    char input_components[3][128];
    char sign_input[128];
    char sign_factor[256];
    char outputs[3][128];
    format_operand_hlsl(ctx, &normal_operand, false, false,
                        HLSL_XYZ_MASK, false, normal, sizeof(normal));
    const int input_operands[3] = {1, 2, 2};
    for (int item = 0; item < 3; item++) {
        format_operand_hlsl(ctx, &matrix_operands[item], false, false,
                            HLSL_XYZ_MASK, false, matrix_rows[item],
                            sizeof(matrix_rows[item]));
        format_operand_hlsl(ctx,
                            &instruction[item].operands[input_operands[item]],
                            false, false, 16, false,
                            input_components[item],
                            sizeof(input_components[item]));
        format_dest_operand_hlsl(ctx, &output_operands[item], false, false,
                                 HLSL_XYZ_MASK, false, outputs[item],
                                 sizeof(outputs[item]));
    }
    format_operand_hlsl(ctx, &instruction[8].operands[1], false, false, 16,
                        false, sign_input, sizeof(sign_input));
    format_operand_hlsl(ctx, &instruction[8].operands[2], false, false, 16,
                        false, sign_factor, sizeof(sign_factor));
    if (ctx->sb->failed) return true;

    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float3 dxbc_compiler_frame_%d_normal = %s;\n",
               instruction_index, normal);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float3 dxbc_compiler_frame_%d_tangent = %s * %s;\n",
               instruction_index, matrix_rows[0], input_components[0]);
    for (int item = 1; item < 3; item++) {
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "dxbc_compiler_frame_%d_tangent = %s * %s + "
                   "dxbc_compiler_frame_%d_tangent;\n",
                   instruction_index, matrix_rows[item],
                   input_components[item], instruction_index);
    }
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "dxbc_compiler_frame_%d_tangent = "
               "normalize(dxbc_compiler_frame_%d_tangent);\n",
               instruction_index, instruction_index);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float dxbc_compiler_frame_%d_sign = %s * %s;\n",
               instruction_index, sign_input, sign_factor);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float3 dxbc_compiler_frame_%d_binormal = "
               "cross(dxbc_compiler_frame_%d_normal, "
               "dxbc_compiler_frame_%d_tangent) * "
               "dxbc_compiler_frame_%d_sign;\n",
               instruction_index, instruction_index, instruction_index,
               instruction_index);
    for (int component = 0; component < 3; component++) {
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "%s = float3(dxbc_compiler_frame_%d_tangent.%c, "
                   "dxbc_compiler_frame_%d_binormal.%c, "
                   "dxbc_compiler_frame_%d_normal.%c);\n",
                   outputs[component], instruction_index,
                   "xyz"[component], instruction_index,
                   "xyz"[component], instruction_index,
                   "xyz"[component]);
    }
    return true;
}

bool emit_compiler_screen_position_lowering(HLSLEmitterContext *ctx,
                                            int instruction_index) {
    if (!ctx ||
        instruction_index < 0 ||
        instruction_index >= ctx->program->instruction_count)
        return false;
    if (ctx->compiler_model.interleaved_projection_packs) {
        const HLSLCompilerInterleavedProjectionPack *pack =
            &ctx->compiler_model.interleaved_projection_packs[
                instruction_index];
        if (pack->valid) {
            if (pack->phase == 0) {
                const USILInstruction *scale =
                    &ctx->program->instructions[pack->scale_instruction];
                const USILInstruction *add =
                    &ctx->program->instructions[pack->add_instruction];
                const int full_mask = 16 | 32 | 64 | 128;
                char clip[128];
                char projection[256];
                char output[128];
                format_dest_operand_hlsl(
                    ctx, &scale->operands[0], false, false, full_mask,
                    false, clip, sizeof(clip));
                format_operand_hlsl(
                    ctx, &scale->operands[2], false, false, 16, false,
                    projection, sizeof(projection));
                format_dest_operand_hlsl(
                    ctx, &add->operands[0], false, false, 64 | 128,
                    false, output, sizeof(output));
                if (ctx->sb->failed) return true;
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_appendf(ctx->sb,
                           "float4 dxbc_compiler_projection_pack_%d = "
                           "%s * 0.5f;\n",
                           pack->scale_instruction, clip);
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_appendf(ctx->sb,
                           "dxbc_compiler_projection_pack_%d.y *= %s;\n",
                           pack->scale_instruction, projection);
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_appendf(ctx->sb,
                           "dxbc_compiler_projection_pack_%d.xy += "
                           "dxbc_compiler_projection_pack_%d.w;\n",
                           pack->scale_instruction,
                           pack->scale_instruction);
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_appendf(ctx->sb,
                           "%s = dxbc_compiler_projection_pack_%d.xy;\n",
                           output, pack->scale_instruction);
                sb_append_spaces(ctx->sb, ctx->indent);
                sb_appendf(ctx->sb, "%s.y *= %s;\n", clip,
                           projection);
            }
            return true;
        }
    }
    if (!ctx->compiler_model.screen_positions) return false;
    const HLSLCompilerScreenPosition *position =
        &ctx->compiler_model.screen_positions[instruction_index];
    if (!position->valid) return false;

    const USILInstruction *clip =
        &ctx->program->instructions[position->clip_instruction];
    const USILInstruction *copy = clip + 1;
    const USILInstruction *screen =
        &ctx->program->instructions[position->screen_instruction];
    const int full_mask = 16 | 32 | 64 | 128;

    if (position->phase == 0) {
        char matrix_row[256];
        char homogeneous_weight[128];
        char accumulator[128];
        char output[128];
        format_operand_hlsl(ctx, &clip->operands[1], false, false,
                            full_mask, false, matrix_row,
                            sizeof(matrix_row));
        format_operand_hlsl(ctx, &clip->operands[2], false, false, 16,
                            false, homogeneous_weight,
                            sizeof(homogeneous_weight));
        format_operand_hlsl(ctx, &clip->operands[3], false, false,
                            full_mask, false, accumulator,
                            sizeof(accumulator));
        format_dest_operand_hlsl(ctx, &copy->operands[0], false, false,
                                 full_mask, false, output,
                                 sizeof(output));
        if (ctx->sb->failed) return true;

        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "float4 dxbc_compiler_clip_%d = %s * %s + %s;\n",
                   position->clip_instruction, matrix_row,
                   homogeneous_weight, accumulator);
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb, "%s = dxbc_compiler_clip_%d;\n", output,
                   position->clip_instruction);
        return true;
    }

    if (position->phase == 2) {
        const USILInstruction *copy_instruction =
            &ctx->program->instructions[instruction_index];
        char output[128];
        char swizzle[8];
        int output_mask = copy_instruction->operands[0].destination_mask;
        format_dest_operand_hlsl(ctx, &copy_instruction->operands[0],
                                 false, false, output_mask, false,
                                 output, sizeof(output));
        if (!format_swizzle_hlsl(&copy_instruction->operands[1],
                                 output_mask, false, swizzle,
                                 sizeof(swizzle))) {
            ctx->sb->failed = true;
            return true;
        }
        if (ctx->sb->failed) return true;
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb, "%s = dxbc_compiler_clip_%d%s;\n", output,
                   position->clip_instruction, swizzle);
        return true;
    }

    if (position->phase == 1) {
        char projection_sign[256];
        int output_offset =
            compiler_screen_pattern_is_inplace(position->pattern)
                ? position->screen_instruction_count - 1
                : position->screen_instruction_count - 2;
        DXBCOperand output_operand = screen[output_offset].operands[0];
        int output_mask = output_operand.destination_mask;
        if (!compiler_screen_pattern_is_inplace(position->pattern)) {
            output_operand.destination_mask = (uint8_t)full_mask;
            output_mask = full_mask;
        }
        char output[128];
        format_operand_hlsl(ctx, &screen[0].operands[2], false, false,
                            16, false, projection_sign,
                            sizeof(projection_sign));
        format_dest_operand_hlsl(ctx, &output_operand, false, false,
                                 output_mask, false, output,
                                 sizeof(output));
        if (ctx->sb->failed) return true;

        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "float4 dxbc_compiler_screen_%d = "
                   "dxbc_compiler_clip_%d * 0.5f;\n",
                   position->clip_instruction,
                   position->clip_instruction);
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "dxbc_compiler_screen_%d.y *= %s;\n",
                   position->clip_instruction, projection_sign);
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "dxbc_compiler_screen_%d.xy += "
                   "dxbc_compiler_screen_%d.w;\n",
                   position->clip_instruction,
                   position->clip_instruction);
        if (!compiler_screen_pattern_is_inplace(position->pattern)) {
            sb_append_spaces(ctx->sb, ctx->indent);
            sb_appendf(ctx->sb,
                       "dxbc_compiler_screen_%d.zw = "
                       "dxbc_compiler_clip_%d.zw;\n",
                       position->clip_instruction,
                       position->clip_instruction);
        }
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb, "%s = dxbc_compiler_screen_%d%s;\n", output,
                   position->clip_instruction,
                   compiler_screen_pattern_is_inplace(position->pattern)
                       ? ".xy"
                       : "");
        return true;
    }

    return false;
}

void free_d3dcompiler_model(HLSLEmitterContext *ctx) {
    free(ctx->compiler_model.swap_binary_operands);
    free(ctx->compiler_model.preserve_vector_output);
    free(ctx->compiler_model.tangent_frames);
    free(ctx->compiler_model.screen_positions);
    free(ctx->compiler_model.interleaved_projection_packs);
    free(ctx->compiler_model.split_matrix_transforms);
    free(ctx->compiler_model.claim_owner);
    ctx->compiler_model.swap_binary_operands = NULL;
    ctx->compiler_model.preserve_vector_output = NULL;
    ctx->compiler_model.tangent_frames = NULL;
    ctx->compiler_model.screen_positions = NULL;
    ctx->compiler_model.interleaved_projection_packs = NULL;
    ctx->compiler_model.split_matrix_transforms = NULL;
    ctx->compiler_model.claim_owner = NULL;
    ctx->compiler_model.replacement_count = 0;
    ctx->compiler_model.conflict_count = 0;
}
