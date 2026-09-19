// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

static bool immediate_register(const DXBCOperand *operand) {
    return hlsl_lift_operand_is_plain(operand) && operand->register_index >= 0 &&
           operand->register_index < HLSL_SM5_IO_REGISTER_COUNT &&
           operand->register_index_dim == 1 && operand->index_representations[0] == 0 &&
           operand->index_has_immediate[0] && !operand->index_value_exceeds_int[0] &&
           operand->index_values[0] == (uint32_t)operand->register_index;
}

static bool input_register(const USILProgram *program, const DXBCOperand *operand) {
    if (operand->type != OPERAND_TYPE_INPUT)
        return false;
    for (int i = 0; i < program->input_count; ++i)
        if (program->inputs[i].register_id == (uint32_t)operand->register_index)
            return true;
    return false;
}

bool hlsl_unity_uv_lift_matches(const USILProgram *program) {
    if (!program || !program->instructions || program->instruction_count != 2 ||
        program->instruction_alloc < 2 || program->temp_count || program->input_count != 2 ||
        program->output_count != 1 || !program->has_stage_contract ||
        (program->program_type != DXBC_PROGRAM_TYPE_VERTEX &&
         program->program_type != DXBC_PROGRAM_TYPE_PIXEL) ||
        !usil_signature_authority_is_valid(program) ||
        hlsl_float4_program_contract(program) != HLSL_EMIT_REASON_NONE ||
        program->inputs[0].register_id == program->inputs[1].register_id)
        return false;
    const USILInstruction *mad = &program->instructions[0];
    const USILInstruction *ret = &program->instructions[1];
    USILEffectFlags effects;
    if (mad->opcode != USIL_OP_MAD || ret->opcode != USIL_OP_RET || mad->precise_mask ||
        mad->saturate || ret->precise_mask || ret->saturate ||
        !usil_instruction_shape_valid(program, mad) ||
        !usil_instruction_shape_valid(program, ret) ||
        !usil_instruction_effects(program, mad, &effects) || effects != USIL_EFFECT_NONE)
        return false;
    for (int i = 0; i < 4; ++i)
        if (!immediate_register(&mad->operands[i]))
            return false;
    const DXBCOperand *output = &mad->operands[0], *uv = &mad->operands[1];
    const DXBCOperand *scale = &mad->operands[2], *offset = &mad->operands[3];
    if (output->type != OPERAND_TYPE_OUTPUT || usil_operand_destination_lane_mask(output) != 15 ||
        program->outputs[0].register_id != (uint32_t)output->register_index ||
        !input_register(program, uv) || !input_register(program, scale) ||
        !input_register(program, offset) || scale->register_index != offset->register_index ||
        uv->register_index == scale->register_index)
        return false;
    for (int lane = 0; lane < 4; ++lane)
        if (usil_operand_source_component(uv, lane) != lane ||
            usil_operand_source_component(scale, lane) != lane % 2 ||
            usil_operand_source_component(offset, lane) != 2 + lane % 2)
            return false;
    return true;
}

bool emit_unity_uv_lift(HLSLEmitterContext *ctx) {
    if (!hlsl_unity_uv_lift_matches(ctx->program) || ctx->compiler_model.replacement_count ||
        !hlsl_float4_program_supported(ctx))
        return false;
    const USILInstruction *mad = &ctx->program->instructions[0];
    DXBCOperand scale_offset = mad->operands[2];
    scale_offset.swizzle_mode = 1;
    for (int lane = 0; lane < 4; ++lane)
        scale_offset.swizzle[lane] = (uint8_t)lane;
    ctx->current_instruction_index = 0;
    ASTExpr *arguments[2] = {hlsl_float4_source_atom(ctx, &mad->operands[1]),
                             hlsl_float4_source_atom(ctx, &scale_offset)};
    ASTExpr *call =
        arguments[0] && arguments[1] ? ast_create_call(HLSL_UNITY_UV_FUNCTION, arguments, 2) : NULL;
    if (!call) {
        ast_free_expr(arguments[0]);
        ast_free_expr(arguments[1]);
        return false;
    }
    hlsl_expression_source_map_begin(ctx);
    sb_append_spaces(ctx->sb, ctx->indent);
    if (!hlsl_float4_append_output(ctx, &mad->operands[0])) {
        ast_free_expr(call);
        return false;
    }
    sb_append(ctx->sb, " = ");
    const size_t begin = ctx->sb->len;
    ast_format_expr(call, ctx->sb);
    if (ctx->expression_source_map) {
        HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[0];
        origin->kind = HLSL_EXPRESSION_ORIGIN_UNITY_UV;
        origin->source_begin = begin;
        origin->source_end = ctx->sb->len;
    }
    sb_append(ctx->sb, ";\n");
    ast_free_expr(call);
    return sb_ok(ctx->sb);
}
