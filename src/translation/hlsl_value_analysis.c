// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdlib.h>
#include <string.h>

static size_t fact_offset(const HLSLEmitterContext *ctx, int state, int reg,
                          int component) {
    return (((size_t)state * (size_t)ctx->value_analysis.register_count +
             (size_t)reg) * 4u) + (size_t)component;
}

static size_t operand_fact_offset(int instruction, int operand,
                                  int component) {
    return (((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u) +
           (size_t)component;
}

unsigned int get_lane_value_facts(const HLSLEmitterContext *ctx, int state,
                                  int reg, int component) {
    if (!ctx || !ctx->value_analysis.lane_facts || state < 0 ||
        state > ctx->program->instruction_count || reg < 0 ||
        reg >= ctx->value_analysis.register_count || component < 0 ||
        component >= 4)
        return HLSL_VALUE_UNKNOWN;
    return ctx->value_analysis.lane_facts[
        fact_offset(ctx, state, reg, component)];
}

unsigned int get_operand_value_facts(const HLSLEmitterContext *ctx,
                                     int instruction, int operand,
                                     int component) {
    if (!ctx || !ctx->value_analysis.operand_facts || instruction < 0 ||
        instruction >= ctx->program->instruction_count || operand < 0 ||
        operand >= DXBC_MAX_OPERANDS || component < 0 || component >= 4)
        return HLSL_VALUE_UNKNOWN;
    return ctx->value_analysis.operand_facts[
        operand_fact_offset(instruction, operand, component)];
}

static int source_component(const DXBCOperand *operand, int destination) {
    if (operand->swizzle_mode == 2) return operand->swizzle[0];
    if (operand->swizzle_mode == 1) return operand->swizzle[destination];
    return destination;
}

static unsigned int source_facts(const HLSLEmitterContext *ctx, int state,
                                 const DXBCOperand *operand,
                                 int destination_component) {
    if (operand->type != OPERAND_TYPE_TEMP) return HLSL_VALUE_UNKNOWN;
    return get_lane_value_facts(ctx, state, operand->register_index,
                                source_component(operand,
                                                 destination_component));
}

static unsigned int opcode_facts(const USILInstruction *inst) {
    switch (inst->opcode) {
        case USIL_OP_ADD:
        case USIL_OP_SUB:
        case USIL_OP_MUL:
        case USIL_OP_DIV:
        case USIL_OP_MAD:
        case USIL_OP_DP2:
        case USIL_OP_DP3:
        case USIL_OP_DP4:
        case USIL_OP_RCP:
        case USIL_OP_RSQ:
        case USIL_OP_SQRT:
        case USIL_OP_MIN:
        case USIL_OP_MAX:
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
        case USIL_OP_ITOF:
        case USIL_OP_UTOF:
        case USIL_OP_SINCOS:
            return HLSL_VALUE_FLOAT;
        case USIL_OP_IADD:
        case USIL_OP_IMUL:
        case USIL_OP_IMAD:
        case USIL_OP_IMAX:
        case USIL_OP_IMIN:
        case USIL_OP_INEG:
        case USIL_OP_FTOI:
            return HLSL_VALUE_SINT;
        case USIL_OP_UMAX:
        case USIL_OP_UMIN:
        case USIL_OP_UDIV:
        case USIL_OP_FTOU:
        case USIL_OP_UBFE:
            return HLSL_VALUE_UINT;
        case USIL_OP_SAMPLEINFO:
            return inst->sample_info_return_type == 1u
                       ? HLSL_VALUE_UINT
                       : HLSL_VALUE_FLOAT;
        case USIL_OP_RESINFO:
            return inst->resource_info_return_type == 2
                       ? HLSL_VALUE_UINT
                       : HLSL_VALUE_FLOAT;
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
            return HLSL_VALUE_BOOLEAN_BITS;
        case USIL_OP_AND:
        case USIL_OP_OR:
        case USIL_OP_XOR:
        case USIL_OP_NOT:
        case USIL_OP_ISHL:
        case USIL_OP_ISHR:
        case USIL_OP_USHR:
            return HLSL_VALUE_RAW_BITS;
        default:
            return HLSL_VALUE_UNKNOWN;
    }
}

static unsigned int expected_operand_facts(const USILInstruction *inst,
                                           int operand) {
    if (operand >= inst->operand_count) return HLSL_VALUE_UNKNOWN;
    switch (inst->opcode) {
        case USIL_OP_ADD:
        case USIL_OP_SUB:
        case USIL_OP_MUL:
        case USIL_OP_DIV:
        case USIL_OP_MAD:
        case USIL_OP_DP2:
        case USIL_OP_DP3:
        case USIL_OP_DP4:
        case USIL_OP_RCP:
        case USIL_OP_RSQ:
        case USIL_OP_SQRT:
        case USIL_OP_MIN:
        case USIL_OP_MAX:
        case USIL_OP_LT:
        case USIL_OP_GE:
        case USIL_OP_EQ:
        case USIL_OP_NE:
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
        case USIL_OP_FTOI:
        case USIL_OP_FTOU:
            return operand > 0 ? HLSL_VALUE_FLOAT : HLSL_VALUE_UNKNOWN;
        case USIL_OP_IADD:
        case USIL_OP_IMUL:
        case USIL_OP_IMAD:
        case USIL_OP_IMAX:
        case USIL_OP_IMIN:
        case USIL_OP_INEG:
        case USIL_OP_ILT:
        case USIL_OP_IGE:
        case USIL_OP_IEQ:
        case USIL_OP_INE:
        case USIL_OP_ITOF:
            return operand > 0 ? HLSL_VALUE_SINT : HLSL_VALUE_UNKNOWN;
        case USIL_OP_UMAX:
        case USIL_OP_UMIN:
        case USIL_OP_UDIV:
        case USIL_OP_ULT:
        case USIL_OP_UGE:
        case USIL_OP_UTOF:
        case USIL_OP_UBFE:
            return operand > 0 ? HLSL_VALUE_UINT : HLSL_VALUE_UNKNOWN;
        case USIL_OP_AND:
        case USIL_OP_OR:
        case USIL_OP_XOR:
        case USIL_OP_NOT:
        case USIL_OP_ISHL:
        case USIL_OP_ISHR:
        case USIL_OP_USHR:
            return operand > 0 ? HLSL_VALUE_RAW_BITS : HLSL_VALUE_UNKNOWN;
        case USIL_OP_MOVC:
            return operand == 1 ? HLSL_VALUE_BOOLEAN_BITS :
                   HLSL_VALUE_UNKNOWN;
        case USIL_OP_IF:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUEC:
        case USIL_OP_DISCARD:
            return operand == 0 ? HLSL_VALUE_BOOLEAN_BITS :
                   HLSL_VALUE_UNKNOWN;
        case USIL_OP_SAMPLE:
        case USIL_OP_SAMPLE_C:
        case USIL_OP_SAMPLE_C_LZ:
        case USIL_OP_SAMPLE_L:
        case USIL_OP_SAMPLE_D:
        case USIL_OP_SAMPLE_B:
            return operand == 1 ? HLSL_VALUE_FLOAT : HLSL_VALUE_UNKNOWN;
        case USIL_OP_SINCOS:
            /* The first two operands are destinations. The angle is the sole
             * float-valued source even when the sine destination is null. */
            return operand == 2 ? HLSL_VALUE_FLOAT : HLSL_VALUE_UNKNOWN;
        default:
            return HLSL_VALUE_UNKNOWN;
    }
}

static bool write_definition_facts(HLSLEmitterContext *ctx, int definition,
                                   int reg, int component,
                                   unsigned int facts) {
    bool changed = false;
    for (int state = definition + 1;
         state <= ctx->program->instruction_count; state++) {
        const HLSLComponentProvenance *provenance =
            get_component_provenance(ctx, state, reg, component);
        if (!provenance ||
            provenance->definition_instruction != definition)
            break;
        unsigned char *value = &ctx->value_analysis.lane_facts[
            fact_offset(ctx, state, reg, component)];
        unsigned char combined = (unsigned char)(*value | facts);
        if (*value != combined) {
            *value = combined;
            changed = true;
        }
    }
    return changed;
}

static bool propagate_operand_requirements(HLSLEmitterContext *ctx) {
    bool changed = false;
    for (int index = 0; index < ctx->program->instruction_count; index++) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        for (int operand_index = 0; operand_index < inst->operand_count;
             operand_index++) {
            const DXBCOperand *operand = &inst->operands[operand_index];
            unsigned int required =
                expected_operand_facts(inst, operand_index);
            for (int component = 0; component < 4; component++) {
                int source = source_component(operand, component);
                unsigned int actual = source_facts(ctx, index, operand,
                                                   component);
                unsigned char *operand_value =
                    &ctx->value_analysis.operand_facts[operand_fact_offset(
                        index, operand_index, component)];
                unsigned char combined =
                    (unsigned char)(*operand_value | actual | required);
                if (*operand_value != combined) {
                    *operand_value = combined;
                    changed = true;
                }
                if (required == HLSL_VALUE_UNKNOWN ||
                    operand->type != OPERAND_TYPE_TEMP)
                    continue;
                int definition = hlsl_operand_definition(
                    ctx, index, operand_index, component);
                if (definition >= 0)
                    changed |= write_definition_facts(
                        ctx, definition,
                        operand->register_index, source, required);
            }
        }
        if ((inst->opcode == USIL_OP_MOV || inst->opcode == USIL_OP_MOVC) &&
            inst->operand_count >= 2 &&
            inst->operands[0].type == OPERAND_TYPE_TEMP) {
            int mask = inst->operands[0].destination_mask;
            if (mask == 0) mask = 16 | 32 | 64 | 128;
            for (int component = 0; component < 4; component++) {
                if (!(mask & (16 << component))) continue;
                unsigned int facts = source_facts(
                    ctx, index, &inst->operands[inst->opcode == USIL_OP_MOV
                                                    ? 1
                                                    : 2],
                    component);
                if (inst->opcode == USIL_OP_MOVC && inst->operand_count >= 4)
                    facts |= source_facts(ctx, index, &inst->operands[3],
                                          component);
                changed |= write_definition_facts(
                    ctx, index, inst->operands[0].register_index, component,
                    facts);
            }
        }
    }
    return changed;
}

bool analyze_lane_value_types(HLSLEmitterContext *ctx) {
    const USILProgram *program = ctx->program;
    ctx->value_analysis.register_count = program->temp_count > 0
                                             ? program->temp_count
                                             : 1;
    size_t width = (size_t)ctx->value_analysis.register_count * 4u;
    size_t count = ((size_t)program->instruction_count + 1u) * width;
    ctx->value_analysis.lane_facts =
        (unsigned char *)calloc(count, sizeof(unsigned char));
    ctx->value_analysis.operand_facts = (unsigned char *)calloc(
        (size_t)program->instruction_count * DXBC_MAX_OPERANDS * 4u,
        sizeof(unsigned char));
    if (!ctx->value_analysis.lane_facts ||
        !ctx->value_analysis.operand_facts) {
        free_lane_value_types(ctx);
        return false;
    }
    for (int index = 0; index < program->instruction_count; index++) {
        unsigned char *before = &ctx->value_analysis.lane_facts[
            fact_offset(ctx, index, 0, 0)];
        unsigned char *after = &ctx->value_analysis.lane_facts[
            fact_offset(ctx, index + 1, 0, 0)];
        memcpy(after, before, width);
        const USILInstruction *inst = &program->instructions[index];
        if (!inst_writes_to_dest(inst) || inst->operand_count < 1 ||
            inst->operands[0].type != OPERAND_TYPE_TEMP ||
            inst->operands[0].register_index < 0 ||
            inst->operands[0].register_index >=
                ctx->value_analysis.register_count)
            continue;
        int mask = inst->operands[0].destination_mask;
        if (mask == 0) mask = 16 | 32 | 64 | 128;
        for (int component = 0; component < 4; component++) {
            if (!(mask & (16 << component))) continue;
            unsigned int facts = opcode_facts(inst);
            if (inst->opcode == USIL_OP_MOV && inst->operand_count >= 2)
                facts = source_facts(ctx, index, &inst->operands[1],
                                     component);
            else if (inst->opcode == USIL_OP_MOVC && inst->operand_count >= 4)
                facts = source_facts(ctx, index, &inst->operands[2],
                                     component) |
                        source_facts(ctx, index, &inst->operands[3],
                                     component);
            after[inst->operands[0].register_index * 4 + component] =
                (unsigned char)facts;
        }
    }
    int iteration_limit = program->instruction_count + 1;
    for (int iteration = 0; iteration < iteration_limit; iteration++)
        if (!propagate_operand_requirements(ctx)) break;
    return true;
}

void free_lane_value_types(HLSLEmitterContext *ctx) {
    free(ctx->value_analysis.lane_facts);
    free(ctx->value_analysis.operand_facts);
    ctx->value_analysis.lane_facts = NULL;
    ctx->value_analysis.operand_facts = NULL;
    ctx->value_analysis.register_count = 0;
}
