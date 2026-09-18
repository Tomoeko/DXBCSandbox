// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdlib.h>
#include <string.h>

static HLSLProvenanceKind classify_instruction(const USILInstruction *inst) {
    switch (inst->opcode) {
        case USIL_OP_SAMPLE:
        case USIL_OP_SAMPLE_L:
        case USIL_OP_SAMPLE_D:
        case USIL_OP_SAMPLE_B:
        case USIL_OP_SAMPLE_C:
        case USIL_OP_SAMPLE_C_LZ:
        case USIL_OP_LD:
            return HLSL_PROVENANCE_TEXTURE;
        case USIL_OP_MOV:
        case USIL_OP_MOVC:
            return HLSL_PROVENANCE_COPY;
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
            return HLSL_PROVENANCE_COMPARISON;
        case USIL_OP_IF:
        case USIL_OP_ELSE:
        case USIL_OP_ENDIF:
        case USIL_OP_LOOP:
        case USIL_OP_ENDLOOP:
        case USIL_OP_BREAK:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUE:
        case USIL_OP_CONTINUEC:
        case USIL_OP_DISCARD:
            return HLSL_PROVENANCE_CONTROL;
        default:
            return HLSL_PROVENANCE_ARITHMETIC;
    }
}

static int source_component(const DXBCOperand *operand, int destination_component) {
    if (operand->swizzle_mode == 2) return operand->swizzle[0];
    if (operand->swizzle_mode == 1)
        return operand->swizzle[destination_component];
    return destination_component;
}

static size_t provenance_offset(const HLSLEmitterContext *ctx, int state,
                                int reg, int component) {
    return (((size_t)state * (size_t)ctx->provenance_register_count +
             (size_t)reg) * 4u) + (size_t)component;
}

const HLSLComponentProvenance *get_component_provenance(
    const HLSLEmitterContext *ctx, int state, int reg, int component) {
    if (!ctx || !ctx->component_provenance || state < 0 ||
        state > ctx->program->instruction_count || reg < 0 ||
        reg >= ctx->provenance_register_count || component < 0 ||
        component >= 4) {
        return NULL;
    }
    return &ctx->component_provenance[
        provenance_offset(ctx, state, reg, component)];
}

bool component_value_unchanged(const HLSLEmitterContext *ctx, int reg,
                               int component, int first_state,
                               int second_state) {
    const HLSLComponentProvenance *first = get_component_provenance(
        ctx, first_state, reg, component);
    const HLSLComponentProvenance *second = get_component_provenance(
        ctx, second_state, reg, component);
    int first_instruction = first_state < ctx->program->instruction_count
                                ? first_state
                                : ctx->program->instruction_count - 1;
    int second_instruction = second_state < ctx->program->instruction_count
                                 ? second_state
                                 : ctx->program->instruction_count - 1;
    return first && second &&
           instructions_have_unambiguous_path(ctx, first_instruction,
                                              second_instruction) &&
           first->definition_instruction == second->definition_instruction &&
           first->root_instruction == second->root_instruction &&
           first->root_register == second->root_register &&
           first->root_component == second->root_component;
}

bool build_component_provenance(HLSLEmitterContext *ctx) {
    const USILProgram *program = ctx->program;
    ctx->provenance_register_count = program->temp_count;
    if (ctx->provenance_register_count < 1)
        ctx->provenance_register_count = 1;
    size_t state_width =
        (size_t)ctx->provenance_register_count * 4u;
    size_t entry_count =
        ((size_t)program->instruction_count + 1u) * state_width;
    ctx->component_provenance = (HLSLComponentProvenance *)calloc(
        entry_count, sizeof(HLSLComponentProvenance));
    if (!ctx->component_provenance) return false;

    for (int reg = 0; reg < ctx->provenance_register_count; reg++) {
        for (int component = 0; component < 4; component++) {
            HLSLComponentProvenance *initial = &ctx->component_provenance[
                provenance_offset(ctx, 0, reg, component)];
            initial->definition_instruction = -1;
            initial->root_instruction = -1;
            initial->root_register = reg;
            initial->root_component = component;
            initial->vector_definition = -1;
        }
    }

    for (int index = 0; index < program->instruction_count; index++) {
        HLSLComponentProvenance *before = &ctx->component_provenance[
            provenance_offset(ctx, index, 0, 0)];
        HLSLComponentProvenance *after = &ctx->component_provenance[
            provenance_offset(ctx, index + 1, 0, 0)];
        memcpy(after, before,
               state_width * sizeof(HLSLComponentProvenance));
        const USILInstruction *inst = &program->instructions[index];
        if (!inst_writes_to_dest(inst) || inst->operand_count < 1 ||
            inst->operands[0].type != OPERAND_TYPE_TEMP) {
            continue;
        }
        const DXBCOperand *destination = &inst->operands[0];
        if (destination->register_index < 0 ||
            destination->register_index >= ctx->provenance_register_count) {
            continue;
        }
        int mask = destination->destination_mask;
        if (mask == 0) mask = 16 | 32 | 64 | 128;
        for (int component = 0; component < 4; component++) {
            if (!(mask & (16 << component))) continue;
            HLSLComponentProvenance *value = &after[
                destination->register_index * 4 + component];
            value->kind = classify_instruction(inst);
            value->definition_instruction = index;
            value->root_instruction = index;
            value->root_register = destination->register_index;
            value->root_component = component;
            value->vector_definition = index;
            value->vector_mask = (unsigned char)mask;
            if (inst->opcode == USIL_OP_MOV && inst->operand_count >= 2 &&
                inst->operands[1].type == OPERAND_TYPE_TEMP) {
                const DXBCOperand *source = &inst->operands[1];
                int source_comp = source_component(source, component);
                const HLSLComponentProvenance *source_value =
                    get_component_provenance(ctx, index,
                                             source->register_index,
                                             source_comp);
                if (source_value) {
                    value->root_instruction = source_value->root_instruction;
                    value->root_register = source_value->root_register;
                    value->root_component = source_value->root_component;
                }
            }
        }
    }
    return true;
}

void free_component_provenance(HLSLEmitterContext *ctx) {
    free(ctx->component_provenance);
    ctx->component_provenance = NULL;
    ctx->provenance_register_count = 0;
}
