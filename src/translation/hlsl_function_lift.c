// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

static const char *function_name(int group) {
    return group == 0 ? "dxbc_mul_chain_left" : "dxbc_mul_chain_right";
}

static bool argument_is_atom(const DXBCOperand *operand) {
    return operand->type == OPERAND_TYPE_INPUT || operand->type == OPERAND_TYPE_IMMEDIATE32;
}

/* The complete float4 validator already excludes effects, partial writes and
 * modifiers. A helper candidate further needs exactly one identity use of its
 * first result and immutable arguments. No operation crosses another one. */
static int chain_scale_operand(const HLSLEmitterContext *ctx, int producer, const unsigned *uses) {
    const USILInstruction *first = &ctx->program->instructions[producer];
    const USILInstruction *second = &ctx->program->instructions[producer + 1];
    if (first->opcode != USIL_OP_MUL || second->opcode != USIL_OP_MUL ||
        first->operands[0].type != OPERAND_TYPE_TEMP ||
        second->operands[0].type != OPERAND_TYPE_TEMP || uses[producer] != 1 ||
        !uses[producer + 1] || !argument_is_atom(&first->operands[1]) ||
        !argument_is_atom(&first->operands[2]))
        return 0;
    for (int product = 1; product <= 2; ++product) {
        const DXBCOperand *value = &second->operands[product];
        if (value->type != OPERAND_TYPE_TEMP || !argument_is_atom(&second->operands[3 - product]))
            continue;
        bool identity = true;
        for (int lane = 0; lane < 4; ++lane)
            identity = identity && usil_operand_source_component(value, lane) == lane &&
                       hlsl_operand_definition(ctx, producer + 1, product, lane) == producer;
        if (identity)
            return 3 - product;
    }
    return 0;
}

static bool prepare_functions(HLSLEmitterContext *ctx) {
    HLSLFloat4FunctionPlan *plan = &ctx->float4_functions;
    for (int i = 0; i < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++i)
        plan->group[i] = -1;
    for (int i = 0; i < ctx->program->instruction_count; ++i)
        if (ctx->program->instructions[i].opcode == USIL_OP_IF ||
            ctx->program->instructions[i].opcode == USIL_OP_LOOP)
            return true; /* Structured values retain their existing planner. */
    unsigned uses[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT] = {0};
    if (!hlsl_float4_validate_expressions(ctx, uses))
        return false;
    for (int i = 0; i + 1 < ctx->program->instruction_count; ++i) {
        const int scale = chain_scale_operand(ctx, i, uses);
        if (!scale)
            continue;
        const bool swap = compiler_model_swaps_binary_operands(ctx, i + 1);
        const int group = ((scale == 2) != swap) ? 0 : 1;
        plan->group[i + 1] = group;
        plan->scale_operand[i + 1] = scale;
        ++plan->use_count[group];
        ++i;
    }
    for (int i = 0; i < ctx->program->instruction_count; ++i)
        if (plan->group[i] >= 0 && plan->use_count[plan->group[i]] < 2)
            plan->group[i] = -1;
    return true;
}

static ASTExpr *parameter(const char *name) {
    return ast_create_var(-1, -1, OPERAND_TYPE_TEMP, name);
}

static bool emit_product(HLSLEmitterContext *ctx, int group, int operation, const char *left,
                         const char *right) {
    ASTExpr *a = parameter(left), *b = parameter(right);
    ASTExpr *expression = a && b ? ast_create_binary(USIL_OP_MUL, a, b) : NULL;
    if (!expression) {
        ast_free_expr(a);
        ast_free_expr(b);
        return false;
    }
    ctx->float4_functions.definition_begin[group][operation] = ctx->sb->len;
    ast_format_expr(expression, ctx->sb);
    ctx->float4_functions.definition_end[group][operation] = ctx->sb->len;
    ast_free_expr(expression);
    return sb_ok(ctx->sb);
}

bool emit_high_level_functions(HLSLEmitterContext *ctx) {
    if (ctx->unity_uv_helper)
        return true;
    if (!prepare_functions(ctx))
        return false;
    for (int group = 0; group < 2; ++group) {
        if (ctx->float4_functions.use_count[group] < 2)
            continue;
        sb_appendf(ctx->sb,
                   "float4 %s(float4 left, float4 right, float4 scale) {\n"
                   "    const float4 product = ",
                   function_name(group));
        if (!emit_product(ctx, group, 0, "left", "right"))
            return false;
        sb_append(ctx->sb, ";\n    return ");
        if (!emit_product(ctx, group, 1, group == 0 ? "product" : "scale",
                          group == 0 ? "scale" : "product"))
            return false;
        sb_append(ctx->sb, ";\n}\n\n");
    }
    return sb_ok(ctx->sb);
}

ASTExpr *hlsl_float4_function_call(HLSLEmitterContext *ctx, int index) {
    const int group = ctx->float4_functions.group[index];
    const int scale = ctx->float4_functions.scale_operand[index];
    const USILInstruction *first = &ctx->program->instructions[index - 1];
    const USILInstruction *second = &ctx->program->instructions[index];
    const bool swap = compiler_model_swaps_binary_operands(ctx, index - 1);
    ctx->current_instruction_index = index - 1;
    ASTExpr *arguments[3] = {hlsl_float4_source_atom(ctx, &first->operands[swap ? 2 : 1]),
                             hlsl_float4_source_atom(ctx, &first->operands[swap ? 1 : 2]), NULL};
    ctx->current_instruction_index = index;
    arguments[2] = hlsl_float4_source_atom(ctx, &second->operands[scale]);
    ASTExpr *call = arguments[0] && arguments[1] && arguments[2]
                        ? ast_create_call(function_name(group), arguments, 3)
                        : NULL;
    if (!call)
        for (int i = 0; i < 3; ++i)
            ast_free_expr(arguments[i]);
    return call;
}
