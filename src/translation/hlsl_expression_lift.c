// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <string.h>

/* A deliberately closed first domain. Existing interface/operand formatting
 * retains ABI authority; this module owns expression structure and SSA uses.
 * There is no source parser, algebraic simplifier, or compiler oracle here. */
enum { EXPRESSION_INSTRUCTION_LIMIT = HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT };

static bool reject(HLSLEmitterContext *ctx, int instruction, HLSLEmitReason reason) {
    hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, reason, instruction, -1);
    return false;
}

static bool value_name(int definition, char name[48]) {
    int length = snprintf(name, 48, "dxbc_value_i%d", definition);
    return length >= 0 && length < 48;
}

static bool identifier_character(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Check the exact emitted spelling, including interface names and intrinsics.
 * This intentionally treats comments conservatively too; it is not a parser
 * or a substitute for the caller's complete include/keyword authority. */
bool hlsl_expression_identifiers_available(HLSLEmitterContext *ctx, size_t source_start) {
    const char *source = ctx->sb->buf + source_start;
    for (size_t index = 0; index < ctx->reserved_preprocessor_identifier_count; ++index) {
        const char *identifier = ctx->reserved_preprocessor_identifiers[index];
        size_t length = strlen(identifier);
        for (const char *found = strstr(source, identifier); found;
             found = strstr(found + length, identifier)) {
            if ((found == source || !identifier_character((unsigned char)found[-1])) &&
                !identifier_character((unsigned char)found[length]))
                return reject(ctx, -1, HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY);
        }
    }
    return true;
}

static int vector_definition(const HLSLEmitterContext *ctx, int instruction, int operand) {
    int definition = hlsl_operand_definition(ctx, instruction, operand, 0);
    if (definition < 0 || definition >= instruction)
        return -1;
    for (int lane = 1; lane < 4; ++lane) {
        if (hlsl_operand_definition(ctx, instruction, operand, lane) != definition)
            return -1;
    }
    return definition;
}

static bool validate_expressions(HLSLEmitterContext *ctx,
                                 unsigned uses[EXPRESSION_INSTRUCTION_LIMIT]) {
    const USILProgram *program = ctx->program;
    if (program->instruction_count < 1 ||
        program->instruction_count > EXPRESSION_INSTRUCTION_LIMIT ||
        (program->shader_model_major != 4 && program->shader_model_major != 5) ||
        program->cbuffer_count || program->texture_count || program->sampler_count ||
        program->uav_count || program->icb_value_count || program->indexable_temp_count ||
        program->index_range_count || program->patch_constant_count || ctx->use_uint_temps ||
        ctx->compiler_model.replacement_count ||
        (program->has_global_flags && program->global_flags != 1u))
        return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    for (int kind = 0; kind < 2; ++kind) {
        const DXBCSignatureElement *signature = kind ? program->outputs : program->inputs;
        int count = kind ? program->output_count : program->input_count;
        for (int index = 0; index < count; ++index) {
            if (signature[index].component_type != 3 || signature[index].mask != 15 ||
                signature[index].min_precision)
                return reject(ctx, -1, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        }
    }
    bool written_outputs[HLSL_SM5_IO_REGISTER_COUNT] = {0};
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *inst = &program->instructions[index];
        if (inst->precise_mask || inst->saturate)
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        if (inst->opcode == USIL_OP_RET) {
            if (index + 1 != program->instruction_count)
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            continue;
        }
        if (inst->opcode == USIL_OP_NOP)
            continue;
        if (inst->opcode != USIL_OP_MOV && inst->opcode != USIL_OP_ADD &&
            inst->opcode != USIL_OP_MUL)
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_OPCODE);
        USILEffectFlags effects;
        if (!usil_instruction_effects(program, inst, &effects) || effects != USIL_EFFECT_NONE)
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        const DXBCOperand *destination = &inst->operands[0];
        if ((destination->type != OPERAND_TYPE_TEMP && destination->type != OPERAND_TYPE_OUTPUT) ||
            usil_operand_destination_lane_mask(destination) != 15)
            return reject(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        if (destination->type == OPERAND_TYPE_OUTPUT) {
            if (destination->register_index < 0 ||
                destination->register_index >= HLSL_SM5_IO_REGISTER_COUNT)
                return reject(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            written_outputs[destination->register_index] = true;
        }
        for (int operand = 0; operand < inst->operand_count; ++operand) {
            const DXBCOperand *value = &inst->operands[operand];
            if (value->min_precision || value->has_abs || value->has_neg || value->rel_op0 ||
                value->rel_op1 || value->rel_op2 || value->extended_token_count)
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            if (!operand)
                continue;
            if (value->type == OPERAND_TYPE_TEMP) {
                int definition = vector_definition(ctx, index, operand);
                if (definition < 0)
                    return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
                ++uses[definition];
            } else if (value->type != OPERAND_TYPE_INPUT &&
                       value->type != OPERAND_TYPE_IMMEDIATE32) {
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            }
        }
    }
    if (program->instructions[program->instruction_count - 1].opcode != USIL_OP_RET)
        return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    for (int index = 0; index < program->output_count; ++index) {
        if (program->outputs[index].register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
            !written_outputs[program->outputs[index].register_id])
            return reject(ctx, -1, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
    return true;
}

static ASTExpr *source_expression(HLSLEmitterContext *ctx, int instruction, int operand,
                                  const unsigned *uses, ASTExpr **pending, uint64_t *pending_owners,
                                  uint64_t *owners) {
    const DXBCOperand *source = &ctx->program->instructions[instruction].operands[operand];
    if (source->type == OPERAND_TYPE_TEMP) {
        int definition = vector_definition(ctx, instruction, operand);
        ASTExpr *value = NULL;
        if (uses[definition] == 1) {
            value = pending[definition];
            pending[definition] = NULL;
            *owners |= pending_owners[definition];
            pending_owners[definition] = 0;
        } else {
            char name[48];
            if (!value_name(definition, name))
                return NULL;
            value = ast_create_var(-1, source->register_index, OPERAND_TYPE_TEMP, name);
        }
        if (!value)
            return NULL;
        int components[4];
        bool identity = true;
        for (int lane = 0; lane < 4; ++lane) {
            components[lane] = usil_operand_source_component(source, lane);
            identity = identity && components[lane] == lane;
        }
        if (identity)
            return value;
        ASTExpr *selected = ast_create_swizzle(value, components, 4);
        if (!selected)
            ast_free_expr(value);
        return selected;
    }
    if (source->type == OPERAND_TYPE_IMMEDIATE32) {
        ASTExpr *value = ast_create_literal_bits(source->imm_values, source->imm_value_count,
                                                 AST_SCALAR_FLOAT32);
        if (!value || source->imm_value_count != 1)
            return value;
        /* Every SSA value is a float4, including a replicated scalar literal. */
        ASTExpr *vector = ast_create_call("float4", &value, 1);
        if (!vector)
            ast_free_expr(value);
        return vector;
    }
    StringBuilder source_text;
    sb_init(&source_text);
    bool formatted = format_operand_hlsl_sb(ctx, source, false, false, 0xf0, true, &source_text);
    ASTExpr *value =
        formatted && sb_ok(&source_text) ? ast_create_emitter_operand(source_text.buf) : NULL;
    sb_free(&source_text);
    return value;
}

typedef struct {
    HLSLExpressionSourceMap *map;
    ASTExpr **roots;
    uint64_t owners;
} ExpressionSpanContext;

static bool record_expression_span(void *context, const ASTExpr *expression, size_t begin,
                                   size_t end) {
    ExpressionSpanContext *trace = context;
    for (size_t index = 0; index < trace->map->count; ++index) {
        if (!(trace->owners & (UINT64_C(1) << index)) || trace->roots[index] != expression)
            continue;
        HLSLExpressionOrigin *origin = &trace->map->origins[index];
        if (origin->kind != HLSL_EXPRESSION_ORIGIN_UNMAPPED || begin >= end)
            return false;
        origin->kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
        origin->source_begin = begin;
        origin->source_end = end;
    }
    return true;
}

static bool finish_expression_origins(ExpressionSpanContext *trace, bool dead) {
    for (int index = 0; index < EXPRESSION_INSTRUCTION_LIMIT; ++index) {
        if (!(trace->owners & (UINT64_C(1) << index)))
            continue;
        if (trace->map) {
            HLSLExpressionOrigin *origin = &trace->map->origins[index];
            if (dead)
                origin->kind = HLSL_EXPRESSION_ORIGIN_DEAD;
            else if (origin->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION)
                return false;
        }
        trace->roots[index] = NULL;
    }
    return true;
}

bool emit_high_level_expressions(HLSLEmitterContext *ctx) {
    unsigned uses[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    ASTExpr *pending[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    ASTExpr *roots[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    uint64_t pending_owners[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    if (!validate_expressions(ctx, uses))
        return false;
    HLSLExpressionSourceMap *map = ctx->expression_source_map;
    if (map) {
        map->count = (size_t)ctx->program->instruction_count;
        for (size_t index = 0; index < map->count; ++index) {
            const USILInstruction *inst = &ctx->program->instructions[index];
            HLSLExpressionOrigin *origin = &map->origins[index];
            origin->instruction_index = (int)index;
            origin->source_instruction_index = inst->source_instruction_index;
            if (inst->opcode == USIL_OP_NOP)
                origin->kind = HLSL_EXPRESSION_ORIGIN_NOP;
            else if (inst->opcode == USIL_OP_RET)
                origin->kind = HLSL_EXPRESSION_ORIGIN_RETURN;
            else
                origin->destination_lanes = usil_operand_destination_lane_mask(&inst->operands[0]);
        }
    }
    bool success = false;
    for (int index = 0; index < ctx->program->instruction_count; ++index) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        ctx->current_instruction_index = index;
        if (inst->opcode == USIL_OP_NOP || inst->opcode == USIL_OP_RET)
            continue;
        uint64_t owners = UINT64_C(1) << index;
        ASTExpr *left = source_expression(ctx, index, 1, uses, pending, pending_owners, &owners);
        ASTExpr *expression = left;
        if (inst->opcode != USIL_OP_MOV) {
            ASTExpr *right =
                source_expression(ctx, index, 2, uses, pending, pending_owners, &owners);
            /* Share the low-level compiler inverse spelling policy. This
             * does not prove equality; acceptance still compares all DXBC. */
            if (compiler_add_uses_mad(inst)) {
                const uint32_t one_bits = UINT32_C(0x3f800000);
                ASTExpr *one = ast_create_literal_bits(&one_bits, 1, AST_SCALAR_FLOAT32);
                ASTExpr *arguments[3] = {left, one, right};
                expression = ast_create_call("mad", arguments, 3);
                if (!expression)
                    ast_free_expr(one);
            } else {
                bool swap = compiler_model_swaps_binary_operands(ctx, index);
                expression =
                    ast_create_binary(inst->opcode, swap ? right : left, swap ? left : right);
            }
            if (!expression) {
                ast_free_expr(left);
                ast_free_expr(right);
            }
        }
        if (!expression)
            goto cleanup;
        roots[index] = expression;
        ExpressionSpanContext trace = {.map = map, .roots = roots, .owners = owners};
        const DXBCOperand *destination = &inst->operands[0];
        if (destination->type == OPERAND_TYPE_TEMP && uses[index] <= 1) {
            if (uses[index]) {
                pending[index] = expression;
                pending_owners[index] = owners;
            } else {
                finish_expression_origins(&trace, true);
                ast_free_expr(expression); /* Proven dead, pure result. */
            }
            continue;
        }
        sb_append_spaces(ctx->sb, ctx->indent);
        if (destination->type == OPERAND_TYPE_TEMP) {
            char name[48];
            if (!value_name(index, name)) {
                ast_free_expr(expression);
                goto cleanup;
            }
            sb_appendf(ctx->sb, "const float4 %s", name);
        } else {
            StringBuilder destination_text;
            sb_init(&destination_text);
            bool formatted = format_dest_operand_hlsl_sb(ctx, destination, false, false, 0xf0, true,
                                                         &destination_text);
            if (formatted)
                sb_append(ctx->sb, destination_text.buf);
            sb_free(&destination_text);
            if (!formatted) {
                ast_free_expr(expression);
                goto cleanup;
            }
        }
        sb_append(ctx->sb, " = ");
        ast_format_expr_traced(expression, ctx->sb, map ? record_expression_span : NULL, &trace);
        if (!finish_expression_origins(&trace, false))
            ctx->sb->failed = true;
        sb_append(ctx->sb, ";\n");
        ast_free_expr(expression);
        if (!sb_ok(ctx->sb))
            goto cleanup;
    }
    success = sb_ok(ctx->sb);
cleanup:
    for (int index = 0; index < EXPRESSION_INSTRUCTION_LIMIT; ++index)
        ast_free_expr(pending[index]);
    return success;
}

bool hlsl_expression_source_map_matches(const HLSLExpressionSourceMap *map,
                                        const USILProgram *program, const char *source) {
    if (!map || !map->complete || !program || !source || !program->instructions ||
        program->instruction_count < 1 ||
        program->instruction_count > EXPRESSION_INSTRUCTION_LIMIT ||
        map->count != (size_t)program->instruction_count)
        return false;
    const size_t source_length = strlen(source);
    for (size_t index = 0; index < map->count; ++index) {
        const HLSLExpressionOrigin *origin = &map->origins[index];
        const USILInstruction *inst = &program->instructions[index];
        if (origin->instruction_index != (int)index ||
            origin->source_instruction_index != inst->source_instruction_index)
            return false;
        const bool expression = inst->opcode == USIL_OP_MOV || inst->opcode == USIL_OP_ADD ||
                                inst->opcode == USIL_OP_MUL;
        const uint8_t lanes =
            expression ? usil_operand_destination_lane_mask(&inst->operands[0]) : 0;
        if (origin->destination_lanes != lanes)
            return false;
        switch (origin->kind) {
        case HLSL_EXPRESSION_ORIGIN_EXPRESSION:
            if (!expression || lanes != 15)
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_RETURN:
            if (inst->opcode != USIL_OP_RET || index + 1 != map->count)
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_DEAD:
            if (!expression || inst->operands[0].type != OPERAND_TYPE_TEMP || lanes != 15)
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_NOP:
            if (inst->opcode != USIL_OP_NOP)
                return false;
            break;
        default:
            return false;
        }
        const bool emitted = origin->kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION ||
                             origin->kind == HLSL_EXPRESSION_ORIGIN_RETURN;
        if (emitted) {
            if (origin->source_begin >= origin->source_end || origin->source_end > source_length)
                return false;
        } else if (origin->source_begin || origin->source_end)
            return false;
    }
    return true;
}

const char *hlsl_expression_origin_kind_name(HLSLExpressionOriginKind kind) {
    switch (kind) {
    case HLSL_EXPRESSION_ORIGIN_EXPRESSION:
        return "expression";
    case HLSL_EXPRESSION_ORIGIN_DEAD:
        return "dead";
    case HLSL_EXPRESSION_ORIGIN_NOP:
        return "nop";
    case HLSL_EXPRESSION_ORIGIN_RETURN:
        return "return";
    default:
        return "unmapped";
    }
}
