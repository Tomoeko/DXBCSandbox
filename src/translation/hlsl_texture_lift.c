// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <string.h>

/* Sampling remains at its original instruction site. Implicit derivatives and
 * bias require a pixel stage; explicit levels/gradients carry their own inputs.
 * No comparison, offset, integer format or result-lane transport is inferred. */
static const char *sample_method(USILOpcode opcode) {
    switch (opcode) {
    case USIL_OP_SAMPLE: return "Sample";
    case USIL_OP_SAMPLE_L: return "SampleLevel";
    case USIL_OP_SAMPLE_B: return "SampleBias";
    case USIL_OP_SAMPLE_D: return "SampleGrad";
    default: return NULL;
    }
}

bool hlsl_texture_sample_opcode(USILOpcode opcode) {
    return sample_method(opcode) != NULL;
}

static bool sample_binding(const HLSLEmitterContext *ctx, int index,
                            const char **texture_name, const char **sampler_name) {
    if (!ctx || !ctx->program || index < 0 || index >= ctx->program->instruction_count ||
        (ctx->program->program_type != DXBC_PROGRAM_TYPE_PIXEL &&
         ctx->program->program_type != DXBC_PROGRAM_TYPE_VERTEX)) return false;
    const USILInstruction *instruction = &ctx->program->instructions[index];
    const bool implicit = instruction->opcode == USIL_OP_SAMPLE || instruction->opcode == USIL_OP_SAMPLE_B;
    const int operand_count = instruction->opcode == USIL_OP_SAMPLE ? 4 :
                              instruction->opcode == USIL_OP_SAMPLE_D ? 6 : 5;
    if (!sample_method(instruction->opcode) || instruction->operand_count != operand_count ||
        (implicit && ctx->program->program_type != DXBC_PROGRAM_TYPE_PIXEL) ||
        !usil_instruction_shape_valid(ctx->program, instruction) || instruction->saturate ||
        instruction->precise_mask || instruction->has_texel_offset ||
        usil_operand_destination_lane_mask(&instruction->operands[0]) != 15)
        return false;
    const USILTexture *texture = hlsl_instruction_texture(ctx->program, instruction, 2);
    if (!texture || strcmp(texture->dimension, "2d") || texture->stride || texture->sample_count)
        return false;
    for (unsigned component = 0; component < 4; ++component)
        if (texture->return_types[component] != 5u) return false;
    for (int operand_index = 2; operand_index <= 3; ++operand_index) {
        const DXBCOperand *operand = &instruction->operands[operand_index];
        if (!hlsl_lift_operand_is_plain(operand) || operand->destination_mask ||
            operand->register_index_dim != 1 || !operand->index_has_immediate[0] ||
            operand->index_representations[0] || operand->index_value_exceeds_int[0] ||
            operand->register_index < 0 ||
            operand->index_values[0] != (uint32_t)operand->register_index)
            return false;
        if (operand_index == 2)
            for (int component = 0; component < 4; ++component)
                if (usil_operand_source_component(operand, component) != component) return false;
    }
    const int sampler = instruction->operands[3].register_index;
    if (sampler >= HLSL_SM5_SAMPLER_REGISTER_COUNT || !ctx->sampler_names[sampler]) return false;
    int declarations = 0;
    for (int declaration = 0; declaration < ctx->program->sampler_count; ++declaration) {
        const USILSampler *value = &ctx->program->samplers[declaration];
        if (value->reg_idx != sampler) continue;
        if (value->mode) return false;
        ++declarations;
    }
    if (declarations != 1 || !resolve_srv_name_ctx(ctx, texture->reg_idx,
            SERIALIZED_RESOURCE_TEXTURE, texture_name) || !*texture_name)
        return false;
    *sampler_name = ctx->sampler_names[sampler];
    return true;
}

bool hlsl_texture_sample_supported(HLSLEmitterContext *ctx, int instruction) {
    const char *texture = NULL, *sampler = NULL;
    return sample_binding(ctx, instruction, &texture, &sampler);
}

ASTExpr *hlsl_texture_sample_expression(HLSLEmitterContext *ctx, int index,
                                       ASTExpr *coordinates, ASTExpr *parameter,
                                       ASTExpr *second_parameter) {
    const char *texture_name = NULL, *sampler_name = NULL;
    if (!coordinates || !sample_binding(ctx, index, &texture_name, &sampler_name)) {
        ast_free_expr(coordinates);
        ast_free_expr(parameter);
        ast_free_expr(second_parameter);
        return NULL;
    }
    const USILInstruction *instruction = &ctx->program->instructions[index];
    if ((instruction->operand_count >= 5) != (parameter != NULL) ||
        (instruction->operand_count == 6) != (second_parameter != NULL)) {
        ast_free_expr(coordinates);
        ast_free_expr(parameter);
        ast_free_expr(second_parameter);
        return NULL;
    }
    char method[256];
    const int length = snprintf(method, sizeof(method), "%s.%s", texture_name,
                                sample_method(instruction->opcode));
    if (length < 0 || (size_t)length >= sizeof(method)) {
        ast_free_expr(coordinates);
        ast_free_expr(parameter);
        ast_free_expr(second_parameter);
        return NULL;
    }
    ASTOperandProvenance origin;
    ast_operand_provenance_init(&origin);
    origin.complete = true;
    origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    origin.logical_value_id = (UINT64_C(1) << 62) |
        (uint32_t)instruction->operands[3].register_index;
    origin.instruction_index = index;
    origin.source_instruction_index = instruction->source_instruction_index;
    origin.operand_index = 3;
    origin.destination_lanes = 15;
    ASTExpr *sampler = ast_create_emitter_operand_with_provenance(sampler_name, &origin);
    ASTExpr *arguments[4] = {sampler, coordinates, parameter, second_parameter};
    ASTExpr *expression = sampler ? ast_create_call(method, arguments,
                                                    instruction->operand_count - 2) : NULL;
    if (!expression) {
        ast_free_expr(sampler);
        ast_free_expr(coordinates);
        ast_free_expr(parameter);
        ast_free_expr(second_parameter);
        return NULL;
    }
    ASTLogicalValueOrigin value;
    ast_logical_value_origin_init(&value);
    value.complete = true;
    value.scalar_type = AST_SCALAR_FLOAT32;
    value.components = 4;
    value.logical_value_id = (uint64_t)index;
    value.instruction_index = index;
    value.source_instruction_index = instruction->source_instruction_index;
    value.destination_lanes = 15;
    if (!ast_set_logical_value_origin(expression, &value)) {
        ast_free_expr(expression);
        return NULL;
    }
    return expression;
}
