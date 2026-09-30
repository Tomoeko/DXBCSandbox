// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "hlsl_geometry_flow.h"
#include "translation/usil_validation.h"
#include "hlsl_matrix_lift.h"

#include <stdio.h>
#include <string.h>

/* A bounded expression domain. Existing interface/operand formatting
 * retains ABI authority; this module owns expression structure and SSA uses.
 * There is no source parser, algebraic simplifier, or compiler oracle here. */
enum { EXPRESSION_INSTRUCTION_LIMIT = HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT };

enum { INLINE_EXPRESSION_DEPTH_LIMIT = 12, INLINE_EXPRESSION_NODE_LIMIT = 32 };

/* Keep an owned logical value at its original evaluation site when inlining
 * would hide it in a very deep expression. This changes no operation tree,
 * type, order or effects; the existing compiler transaction still decides
 * whether the additional typed local reproduces the target container. */
static bool expression_exceeds_inline_limit(const ASTExpr *expression,
                                            unsigned depth, unsigned *nodes) {
    if (!expression || depth > INLINE_EXPRESSION_DEPTH_LIMIT ||
        ++*nodes > INLINE_EXPRESSION_NODE_LIMIT) return true;
    switch (expression->kind) {
    case AST_EXPR_LITERAL:
    case AST_EXPR_VAR:
    case AST_EXPR_EMITTER_OPERAND:
        return false;
    case AST_EXPR_UNARY:
        return expression_exceeds_inline_limit(expression->u.unary.sub, depth + 1, nodes);
    case AST_EXPR_BINARY:
        return expression_exceeds_inline_limit(expression->u.binary.left, depth + 1, nodes) ||
               expression_exceeds_inline_limit(expression->u.binary.right, depth + 1, nodes);
    case AST_EXPR_TERNARY:
        return expression_exceeds_inline_limit(expression->u.ternary.cond, depth + 1, nodes) ||
               expression_exceeds_inline_limit(expression->u.ternary.true_expr, depth + 1, nodes) ||
               expression_exceeds_inline_limit(expression->u.ternary.false_expr, depth + 1, nodes);
    case AST_EXPR_SWIZZLE:
        return expression_exceeds_inline_limit(expression->u.swizzle.sub, depth + 1, nodes);
    case AST_EXPR_CALL:
        for (int argument = 0; argument < expression->u.call.arg_count; ++argument)
            if (expression_exceeds_inline_limit(expression->u.call.args[argument], depth + 1, nodes))
                return true;
        return false;
    case AST_EXPR_CAST:
        return expression_exceeds_inline_limit(expression->u.cast.sub, depth + 1, nodes);
    case AST_EXPR_BITCAST:
        return expression_exceeds_inline_limit(expression->u.bitcast.sub, depth + 1, nodes);
    default:
        return true;
    }
}

static bool reject(HLSLEmitterContext *ctx, int instruction, HLSLEmitReason reason) {
    hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, reason, instruction, -1);
    return false;
}

static bool value_name(HLSLEmitterContext *ctx, int definition, char name[48]) {
    int length = snprintf(name, 48, "dxbc_value_i%d", definition);
    if (length < 0 || length >= 48) return false;
    return hlsl_high_level_name_available(ctx, name) ||
        reject(ctx, definition, HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY);
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

static int lane_count(uint8_t mask) {
    int count = 0;
    for (int lane = 0; lane < 4; ++lane)
        count += (mask >> lane) & 1;
    return count;
}

static unsigned expression_width(const ASTExpr *expression) {
    if (!expression) return 0;
    if (expression->logical_origin.complete)
        return expression->logical_origin.components;
    if (expression->kind == AST_EXPR_LITERAL)
        return (unsigned)expression->u.literal.components;
    if (expression->kind == AST_EXPR_EMITTER_OPERAND &&
        expression->operand_provenance.complete)
        return expression->operand_provenance.result_components;
    return 0;
}

/* Scalar broadcast is an HLSL value operation. Recover it only from the owned
 * natural widths of all children, never from a register name or source text. */
static unsigned operation_width(uint8_t lanes, const ASTExpr *left,
                                 const ASTExpr *right, const ASTExpr *third) {
    unsigned width = expression_width(left);
    if (!width) return (unsigned)lane_count(lanes);
    const ASTExpr *others[] = {right, third};
    for (unsigned child = 0; child < 2; ++child) {
        if (!others[child]) continue;
        unsigned next = expression_width(others[child]);
        if (!next || (next != width && next != 1 && width != 1))
            return (unsigned)lane_count(lanes);
        if (next > width) width = next;
    }
    return width;
}

static ASTExpr *logical_expression(HLSLEmitterContext *ctx, ASTExpr *expression,
                                    int instruction, uint8_t lanes, unsigned width) {
    if (!expression || expression->kind == AST_EXPR_EMITTER_OPERAND)
        return expression;
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = AST_SCALAR_FLOAT32;
    origin.components = (uint8_t)width;
    origin.logical_value_id = (uint64_t)instruction;
    origin.instruction_index = instruction;
    origin.source_instruction_index =
        ctx->program->instructions[instruction].source_instruction_index;
    origin.destination_lanes = lanes;
    if (!ast_set_logical_value_origin(expression, &origin)) {
        ast_free_expr(expression);
        return NULL;
    }
    return expression;
}

static uint8_t source_lanes(const HLSLEmitterContext *ctx, int instruction, int operand) {
    USILOperandUseInfo use;
    if (!usil_instruction_operand_use(ctx->program,
            &ctx->program->instructions[instruction], operand, &use) ||
        use.use != USIL_OPERAND_USE_SOURCE)
        return 0;
    return use.source_lane_mask;
}

static int vector_definition(const HLSLEmitterContext *ctx, int instruction, int operand) {
    const uint8_t mask = source_lanes(ctx, instruction, operand);
    if (!mask) return -1;
    int definition = -1;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(mask & (1u << lane)))
            continue;
        const int current = hlsl_operand_definition(ctx, instruction, operand, lane);
        if (current < 0 || current >= instruction || (definition >= 0 && current != definition))
            return -1;
        definition = current;
    }
    return definition;
}

static HLSLEmitReason float_program_contract(const USILProgram *program, bool full_width) {
    if (program->instruction_count < 1 ||
        program->instruction_count > EXPRESSION_INSTRUCTION_LIMIT ||
        (program->shader_model_major != 4 && program->shader_model_major != 5) ||
        (full_width && (program->cbuffer_count || program->texture_count || program->sampler_count)) ||
        program->uav_count || program->icb_value_count || program->indexable_temp_count ||
        program->index_range_count || (program->patch_constant_count &&
        !hlsl_high_level_domain_interface_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE)) ||
        (program->has_global_flags && program->global_flags != 1u))
        return HLSL_EMIT_REASON_UNSUPPORTED_FEATURE;
    for (int kind = 0; kind < 2; ++kind) {
        const DXBCSignatureElement *signature = kind ? program->outputs : program->inputs;
        int count = kind ? program->output_count : program->input_count;
        for (int index = 0; index < count; ++index) {
            if (signature[index].component_type != 3 || !signature[index].mask ||
                (full_width && signature[index].mask != 15) ||
                signature[index].min_precision)
                return HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT;
        }
    }
    return HLSL_EMIT_REASON_NONE;
}

HLSLEmitReason hlsl_float4_program_contract(const USILProgram *program) {
    return float_program_contract(program, true);
}

bool hlsl_float4_program_supported(HLSLEmitterContext *ctx) {
    const HLSLEmitReason reason = ctx->use_uint_temps ? HLSL_EMIT_REASON_UNSUPPORTED_FEATURE
                                                      : hlsl_float4_program_contract(ctx->program);
    return reason == HLSL_EMIT_REASON_NONE || reject(ctx, -1, reason);
}

bool hlsl_lift_operand_is_plain(const DXBCOperand *value) {
    return value && !value->min_precision && !value->has_abs && !value->has_neg &&
           !value->rel_op0 && !value->rel_op1 && !value->rel_op2 && !value->extended_token_count;
}

static const char *float_intrinsic(USILOpcode opcode) {
    switch (opcode) {
    case USIL_OP_DP2: case USIL_OP_DP3: case USIL_OP_DP4: return "dot";
    case USIL_OP_RCP: return "rcp";
    case USIL_OP_RSQ: return "rsqrt";
    case USIL_OP_SQRT: return "sqrt";
    case USIL_OP_LOG: return "log2";
    case USIL_OP_EXP: return "exp2";
    case USIL_OP_FRC: return "frac";
    case USIL_OP_ROUND_NE: return "round";
    case USIL_OP_ROUND_NI: return "floor";
    case USIL_OP_ROUND_PI: return "ceil";
    case USIL_OP_ROUND_Z: return "trunc";
    case USIL_OP_DERIV_RTX: return "ddx";
    case USIL_OP_DERIV_RTY: return "ddy";
    case USIL_OP_DERIV_RTX_COARSE: return "ddx_coarse";
    case USIL_OP_DERIV_RTY_COARSE: return "ddy_coarse";
    case USIL_OP_DERIV_RTX_FINE: return "ddx_fine";
    case USIL_OP_DERIV_RTY_FINE: return "ddy_fine";
    default: return NULL;
    }
}

static bool float_derivative(USILOpcode opcode) {
    return opcode == USIL_OP_DERIV_RTX || opcode == USIL_OP_DERIV_RTY ||
        opcode == USIL_OP_DERIV_RTX_COARSE || opcode == USIL_OP_DERIV_RTY_COARSE ||
        opcode == USIL_OP_DERIV_RTX_FINE || opcode == USIL_OP_DERIV_RTY_FINE;
}

bool hlsl_expression_effects_supported(const USILProgram *program,
                                        const USILInstruction *instruction) {
    USILEffectFlags effects;
    if (!program || !instruction ||
        !usil_instruction_effects(program, instruction, &effects)) return false;
    if (effects == USIL_EFFECT_NONE) return true;
    if (instruction->opcode == USIL_OP_SAMPLE_L || instruction->opcode == USIL_OP_SAMPLE_D)
        return (program->program_type == DXBC_PROGRAM_TYPE_PIXEL ||
                program->program_type == DXBC_PROGRAM_TYPE_VERTEX) &&
               effects == USIL_EFFECT_RESOURCE_READ;
    if (program->program_type != DXBC_PROGRAM_TYPE_PIXEL) return false;
    if (instruction->opcode == USIL_OP_SAMPLE || instruction->opcode == USIL_OP_SAMPLE_B)
        return effects == (USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_QUAD_CONTEXT);
    if (!float_derivative(instruction->opcode) || effects != USIL_EFFECT_QUAD_CONTEXT)
        return false;
    return program->shader_model_major == 5 ||
        instruction->opcode == USIL_OP_DERIV_RTX || instruction->opcode == USIL_OP_DERIV_RTY;
}

static bool float_dot(USILOpcode opcode) {
    return opcode == USIL_OP_DP2 || opcode == USIL_OP_DP3 || opcode == USIL_OP_DP4;
}

bool hlsl_float_source_modifier_supported(const DXBCOperand *operand) {
    if (!operand) return false;
    if (!operand->extended_token_count) return operand->extended_tokens == NULL;
    if (operand->extended_token_count != 1 || !operand->extended_tokens || operand->min_precision)
        return false;
    const uint32_t modifier = (operand->has_neg ? 1u : 0u) | (operand->has_abs ? 2u : 0u);
    return operand->extended_tokens[0] == (1u | (modifier << 6));
}

bool hlsl_material_source_supported(HLSLEmitterContext *ctx, const DXBCOperand *source,
                                    uint8_t mask) {
    if (!ctx || !ctx->program || !source || source->type != OPERAND_TYPE_CONSTANT_BUFFER ||
        !mask || (mask & ~15u)) return false;
    for (int index = 0; index < ctx->program->cbuffer_count; ++index) {
        const USILConstantBuffer *buffer = &ctx->program->cbuffers[index];
        if (buffer->reg_idx == source->register_index && buffer->dynamic_indexed)
            return false;
    }
    const char *value_name = NULL;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(mask & (1u << lane)))
            continue;
        const int component = usil_operand_source_component(source, lane);
        int offset = 0;
        const char *name = resolve_cb_variable_ctx(ctx, source->register_index,
                                                   source->rel_offset0, component, &offset);
        DecodedVariableLayout layout;
        if (!name || !resolve_variable_layout_ctx(ctx, name, &layout) ||
            layout.scalar_type != 0 || layout.is_matrix || layout.array_size || layout.rows != 1 ||
            (value_name && strcmp(value_name, name) != 0))
            return false;
        const int first = (int)((layout.byte_offset % 16) / 4);
        if (component < first || component >= first + (int)layout.columns)
            return false;
        value_name = name;
    }
    return value_name != NULL;
}

static bool float_instruction_supported(HLSLEmitterContext *ctx, int index, bool full_width,
                                        const HLSLPureExpressionScope *scope) {
    const USILProgram *program = ctx->program;
    const USILInstruction *inst = &program->instructions[index];
    if (inst->precise_mask || inst->saturate)
        return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    const bool base_opcode = inst->opcode == USIL_OP_MOV || inst->opcode == USIL_OP_ADD ||
                             inst->opcode == USIL_OP_MUL;
    const bool vector_opcode = inst->opcode == USIL_OP_MAD || inst->opcode == USIL_OP_DIV ||
                               inst->opcode == USIL_OP_MIN || inst->opcode == USIL_OP_MAX;
    const bool sample = !full_width && hlsl_texture_sample_opcode(inst->opcode);
    if (!base_opcode && (full_width || (!vector_opcode && !float_intrinsic(inst->opcode) && !sample)))
        return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_OPCODE);
    if (!hlsl_expression_effects_supported(program, inst))
        return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    if (sample && !hlsl_texture_sample_supported(ctx, index))
        return reject(ctx, index, HLSL_EMIT_REASON_MISSING_METADATA_AUTHORITY);
    const DXBCOperand *destination = &inst->operands[0];
    if ((destination->type != OPERAND_TYPE_TEMP && destination->type != OPERAND_TYPE_OUTPUT) ||
        !usil_operand_destination_lane_mask(destination) ||
        (full_width && usil_operand_destination_lane_mask(destination) != 15))
        return reject(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    for (int operand = 0; operand < inst->operand_count; ++operand) {
        if (sample && (operand == 2 || operand == 3))
            continue; /* Extra level/bias/gradient operands retain ordinary value authority. */
        const DXBCOperand *value = &inst->operands[operand];
        DXBCOperand unmodified = *value;
        if (!full_width && operand) {
            if (!hlsl_float_source_modifier_supported(value))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            unmodified.has_abs = unmodified.has_neg = false;
            unmodified.extended_tokens = NULL;
            unmodified.extended_token_count = 0;
        }
        const bool stage_destination = !operand && scope &&
            value->type == OPERAND_TYPE_OUTPUT && scope->destination_supported &&
            scope->destination_supported(ctx, index, scope->context);
        const bool stage_source = operand && scope && scope->source_supported &&
            scope->source_supported(ctx, index, operand, scope->context);
        if (scope && scope->compose_disjoint_temp_lanes && value->type == OPERAND_TYPE_TEMP &&
            (value->register_index_dim != 1 || value->register_index < 0 ||
             !value->index_has_immediate[0] || value->index_representations[0] ||
             value->index_value_exceeds_int[0] || value->index_values[0] != (uint32_t)value->register_index ||
             value->rel_op0 || value->rel_op1 || value->rel_op2))
            return reject(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        if (!stage_destination && !stage_source && !hlsl_lift_operand_is_plain(&unmodified))
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        const bool domain_input = ctx->high_level_domain &&
            (value->type == OPERAND_TYPE_INPUT_CONTROL_POINT || value->type == OPERAND_TYPE_DOMAIN_LOCATION);
        if (operand && !domain_input && !stage_source && value->type != OPERAND_TYPE_TEMP && value->type != OPERAND_TYPE_INPUT &&
            value->type != OPERAND_TYPE_IMMEDIATE32 &&
            (full_width || value->type != OPERAND_TYPE_CONSTANT_BUFFER))
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        if (operand && value->type == OPERAND_TYPE_CONSTANT_BUFFER &&
            !hlsl_material_source_supported(ctx, value, source_lanes(ctx, index, operand)))
            return reject(ctx, index, HLSL_EMIT_REASON_MISSING_METADATA_AUTHORITY);
        if (operand && (value->type == OPERAND_TYPE_INPUT || domain_input) && ctx->high_level_interface) {
            ASTOperandProvenance origin;
            if (!hlsl_high_level_input_provenance(ctx, &unmodified,
                    source_lanes(ctx, index, operand), &origin))
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        }
    }
    return true;
}

bool hlsl_float4_instruction_supported(HLSLEmitterContext *ctx, int index) {
    return float_instruction_supported(ctx, index, true, NULL);
}

/* The straight-line planner can represent a partial register definition as a
 * logical scalar/vector. It does not widen the structured float4 contract or
 * pretend that lanes from different definitions form one source value. */
static bool vector_program_supported(HLSLEmitterContext *ctx) {
    const HLSLEmitReason reason = ctx->use_uint_temps ? HLSL_EMIT_REASON_UNSUPPORTED_FEATURE
                                                    : float_program_contract(ctx->program, false);
    return reason == HLSL_EMIT_REASON_NONE || reject(ctx, -1, reason);
}

static bool validate_float_expressions(HLSLEmitterContext *ctx, unsigned *uses, bool full_width,
                                       HLSLMatrixLiftPlan *matrix_plans,
                                       const HLSLPureExpressionScope *scope) {
    if (!scope && !(full_width ? hlsl_float4_program_supported(ctx) : vector_program_supported(ctx)))
        return false;
    if (ctx->compiler_model.replacement_count)
        return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    uint8_t written_outputs[HLSL_SM5_IO_REGISTER_COUNT] = {0};
    const USILProgram *program = ctx->program;
    const int first = scope ? scope->first_instruction : 0;
    const int end = scope ? scope->end_instruction : program->instruction_count;
    for (int index = first; index < end; ++index) {
        const USILInstruction *inst = &program->instructions[index];
        if (scope && scope->omitted_instructions &&
            hlsl_instruction_owners_contains(scope->omitted_instructions, index)) continue;
        if (scope) {
            if (!usil_instruction_shape_valid(program, inst) ||
                (inst->opcode != USIL_OP_MOV && inst->opcode != USIL_OP_ADD &&
                 inst->opcode != USIL_OP_MUL && inst->opcode != USIL_OP_MAD &&
                 inst->opcode != USIL_OP_MIN && inst->opcode != USIL_OP_MAX &&
                 inst->opcode != USIL_OP_RET))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            for (int operand = 1; operand < inst->operand_count; ++operand)
                if (inst->operands[operand].type != OPERAND_TYPE_TEMP &&
                    inst->operands[operand].type != OPERAND_TYPE_IMMEDIATE32 &&
                    !(scope->source_supported && scope->source_supported(ctx, index, operand, scope->context)))
                    return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        }
        if (inst->precise_mask || inst->saturate)
            return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        if (inst->opcode == USIL_OP_NOP)
            continue;
        if (inst->opcode == USIL_OP_RET) {
            if (index + 1 != end)
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            continue;
        }
        if (ctx->high_level_geometry &&
            (inst->opcode == USIL_OP_GEOMETRY_APPEND ||
             inst->opcode == USIL_OP_GEOMETRY_RESTART_STRIP)) {
            if (!hlsl_high_level_geometry_effect_supported(ctx, index))
                return reject(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            continue;
        }
        if (matrix_plans && hlsl_matrix_lift_prepare(ctx, index, &matrix_plans[index])) {
            const int final = matrix_plans[index].end_instruction;
            const DXBCOperand *destination = &program->instructions[final].operands[0];
            if (destination->type == OPERAND_TYPE_OUTPUT) {
                if (destination->register_index < 0 ||
                    destination->register_index >= HLSL_SM5_IO_REGISTER_COUNT)
                    return reject(ctx, final, HLSL_EMIT_REASON_INVALID_OPERAND);
                written_outputs[destination->register_index] |=
                    usil_operand_destination_lane_mask(destination);
            }
            index = final;
            continue;
        }
        if (!float_instruction_supported(ctx, index, full_width, scope))
            return false;
        if (hlsl_texture_sample_opcode(inst->opcode) || float_derivative(inst->opcode))
            uses[index] = 2; /* Keep quad/resource operations at their original site. */
        const DXBCOperand *destination = &inst->operands[0];
        if (destination->type == OPERAND_TYPE_OUTPUT) {
            if (scope && (!scope->destination_supported ||
                !scope->destination_supported(ctx, index, scope->context)))
                return reject(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
            if (destination->register_index < 0 ||
                destination->register_index >= HLSL_SM5_IO_REGISTER_COUNT)
                return reject(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            written_outputs[destination->register_index] |=
                usil_operand_destination_lane_mask(destination);
        }
        for (int operand = 1; operand < inst->operand_count; ++operand) {
            if (inst->operands[operand].type != OPERAND_TYPE_TEMP)
                continue;
            const int definition = vector_definition(ctx, index, operand);
            if (definition < 0 && scope && scope->compose_disjoint_temp_lanes) {
                const uint8_t demanded = source_lanes(ctx, index, operand);
                HLSLInstructionOwners definitions = {0};
                if (!demanded) return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
                for (int lane = 0; lane < 4; ++lane) {
                    if (!(demanded & (1u << lane))) continue;
                    const int owner = hlsl_operand_definition(ctx, index, operand, lane);
                    const int selected = usil_operand_source_component(&inst->operands[operand], lane);
                    if (owner < first || owner >= index || selected < 0 || selected > 3 ||
                        (scope->omitted_instructions && hlsl_instruction_owners_contains(scope->omitted_instructions, owner)) ||
                        program->instructions[owner].operands[0].type != OPERAND_TYPE_TEMP ||
                        program->instructions[owner].operands[0].register_index != inst->operands[operand].register_index ||
                        !(usil_operand_destination_lane_mask(&program->instructions[owner].operands[0]) & (1u << selected)) ||
                        !hlsl_instruction_owners_add(&definitions, owner))
                        return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
                }
                for (int owner = first; owner < index; ++owner)
                    if (hlsl_instruction_owners_contains(&definitions, owner))
                        uses[owner] = uses[owner] < 2 ? 2 : uses[owner] + 1;
                continue;
            }
            if (definition < 0)
                return reject(ctx, index, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            ++uses[definition];
        }
    }
    if (program->instructions[end - 1].opcode != USIL_OP_RET)
        return reject(ctx, -1, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    if (scope) return true; /* The stage producer owns full per-phase field coverage. */
    for (int index = 0; index < program->output_count; ++index) {
        const DXBCSignatureElement *output = &program->outputs[index];
        if (output->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
            (written_outputs[output->register_id] & output->mask) != output->mask)
            return reject(ctx, -1, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
    return true;
}

bool hlsl_float4_validate_expressions(HLSLEmitterContext *ctx,
                                      unsigned uses[EXPRESSION_INSTRUCTION_LIMIT]) {
    return validate_float_expressions(ctx, uses, true, NULL, NULL);
}

static ASTExpr *formatted_source_atom(HLSLEmitterContext *ctx, const DXBCOperand *source,
                                       uint8_t mask, bool preserve_vector,
                                       int instruction, int operand) {
    StringBuilder text;
    sb_init(&text);
    const bool formatted =
        format_operand_hlsl_sb(ctx, source, false, false, mask << 4, preserve_vector, &text);
    ASTExpr *value = NULL;
    if (formatted && sb_ok(&text)) {
        ASTOperandProvenance origin;
        ast_operand_provenance_init(&origin);
        if (source->type == OPERAND_TYPE_CONSTANT_BUFFER &&
            hlsl_material_source_supported(ctx, source, mask)) {
            int lane = 0;
            while (!(mask & (1u << lane))) ++lane;
            int offset = 0;
            const char *name = resolve_cb_variable_ctx(
                ctx, source->register_index, source->rel_offset0,
                usil_operand_source_component(source, lane), &offset);
            DecodedVariableLayout layout;
            if (name && resolve_variable_layout_ctx(ctx, name, &layout)) {
                origin.complete = true;
                origin.value_role = AST_OPERAND_VALUE_LOGICAL;
                origin.logical_value_id =
                    ((uint64_t)(source->register_index + 1) << 32) | layout.byte_offset;
                origin.natural_components = (uint8_t)layout.columns;
                origin.result_components = (uint8_t)lane_count(mask);
                const int first = (int)(layout.byte_offset % 16u) / 4;
                bool identity = origin.natural_components == origin.result_components;
                int component = 0;
                for (int selected_lane = 0; selected_lane < 4; ++selected_lane) {
                    if (!(mask & (1u << selected_lane))) continue;
                    const int selected = usil_operand_source_component(source, selected_lane) - first;
                    origin.selected_components[component] = (uint8_t)selected;
                    identity = identity && selected == component;
                    ++component;
                }
                bool same = true, ascending = true;
                for (int selected = 1; selected < component; ++selected) {
                    same = same && origin.selected_components[selected] ==
                                     origin.selected_components[0];
                    ascending = ascending && origin.selected_components[selected] >
                                              origin.selected_components[selected - 1];
                }
                /* The formatter collapses repeated components of this actual
                 * metadata field to one scalar. Prefix/ascending selections
                 * are logical component projections, not raw row transport. */
                if (same) origin.result_components = 1;
                origin.selection_role = identity || layout.columns == 1
                    ? AST_COMPONENT_SELECTION_NONE : (same || ascending)
                        ? AST_COMPONENT_SELECTION_SEMANTIC : AST_COMPONENT_SELECTION_TRANSPORT;
                if (instruction >= 0) {
                    origin.instruction_index = instruction;
                    origin.source_instruction_index =
                        ctx->program->instructions[instruction].source_instruction_index;
                    origin.operand_index = operand;
                    origin.destination_lanes = mask;
                }
                value = ast_create_emitter_operand_with_provenance(text.buf, &origin);
            }
        } else if ((source->type == OPERAND_TYPE_INPUT || (ctx->high_level_domain &&
                    (source->type == OPERAND_TYPE_INPUT_CONTROL_POINT || source->type == OPERAND_TYPE_DOMAIN_LOCATION))) &&
                   hlsl_high_level_input_provenance(ctx, source, mask, &origin)) {
            if (instruction >= 0) {
                origin.instruction_index = instruction;
                origin.source_instruction_index =
                    ctx->program->instructions[instruction].source_instruction_index;
                origin.operand_index = operand;
                origin.destination_lanes = mask;
            }
            value = ast_create_emitter_operand_with_provenance(text.buf, &origin);
        } else {
            value = ast_create_emitter_operand(text.buf);
        }
    }
    sb_free(&text);
    return value;
}

ASTExpr *hlsl_material_source_expression(HLSLEmitterContext *ctx, int instruction,
                                          int operand, uint8_t lanes) {
    if (!ctx || !ctx->program || instruction < 0 || instruction >= ctx->program->instruction_count ||
        operand < 1 || operand >= ctx->program->instructions[instruction].operand_count ||
        lanes != source_lanes(ctx, instruction, operand)) return NULL;
    const DXBCOperand *source = &ctx->program->instructions[instruction].operands[operand];
    if (!hlsl_material_source_supported(ctx, source, lanes)) return NULL;
    return formatted_source_atom(ctx, source, lanes, false, instruction, operand);
}

/* An actual domain coordinate is a pure logical parameter. Compose a vector
 * from individually owned scalar component reads instead of treating a mixed
 * repeated selection as one semantic swizzle. No register/temp/effectful source
 * is admitted here, and existing scalar broadcast/ascending selections remain. */
static ASTExpr *domain_coordinate_composition(HLSLEmitterContext *ctx,
                                              const DXBCOperand *source,
                                              uint8_t mask, int instruction,
                                              int operand,
                                              const ASTOperandProvenance *selection) {
    const int width = lane_count(mask);
    if (!selection || width < 2 || width > 4 || instruction < 0 ||
        instruction >= ctx->program->instruction_count || operand < 1 ||
        operand >= ctx->program->instructions[instruction].operand_count)
        return NULL;
    ASTExpr *arguments[4] = {0};
    int count = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        if (!(mask & (1u << lane))) continue;
        ASTExpr *component = formatted_source_atom(ctx, source,
            (uint8_t)(1u << lane), false, instruction, operand);
        if (!component || component->kind != AST_EXPR_EMITTER_OPERAND ||
            !component->operand_provenance.complete ||
            component->operand_provenance.value_role != AST_OPERAND_VALUE_LOGICAL ||
            component->operand_provenance.result_components != 1 ||
            component->operand_provenance.natural_components != selection->natural_components ||
            component->operand_provenance.logical_value_id != selection->logical_value_id ||
            component->operand_provenance.selected_components[0] != selection->selected_components[count] ||
            component->operand_provenance.selection_role != AST_COMPONENT_SELECTION_SEMANTIC ||
            component->operand_provenance.bitcast_role != AST_OPERAND_BITCAST_NONE ||
            component->operand_provenance.raw_buffer_reconstruction ||
            component->operand_provenance.synthetic_interface ||
            component->operand_provenance.instruction_index != instruction ||
            component->operand_provenance.source_instruction_index !=
                ctx->program->instructions[instruction].source_instruction_index ||
            component->operand_provenance.operand_index != operand ||
            component->operand_provenance.destination_lanes != (uint8_t)(1u << lane)) {
            ast_free_expr(component);
            for (int previous = 0; previous < count; ++previous)
                ast_free_expr(arguments[previous]);
            return NULL;
        }
        arguments[count++] = component;
    }
    static const char *const constructors[] = {NULL, NULL, "float2", "float3", "float4"};
    ASTExpr *composition = ast_create_call(constructors[width], arguments, count);
    if (!composition)
        for (int previous = 0; previous < count; ++previous)
            ast_free_expr(arguments[previous]);
    return logical_expression(ctx, composition, instruction, mask, (unsigned)width);
}

static ASTExpr *vector_source_atom(HLSLEmitterContext *ctx, const DXBCOperand *source,
                                    uint8_t mask, int instruction, int operand) {
    const int width = lane_count(mask);
    if (source->type == OPERAND_TYPE_IMMEDIATE32) {
        uint32_t bits[4];
        int component = 0;
        for (int lane = 0; lane < 4; ++lane) {
            if (!(mask & (1u << lane)))
                continue;
            const int selected = source->imm_value_count == 1
                                     ? 0 : usil_operand_source_component(source, lane);
            if (selected < 0 || selected >= source->imm_value_count)
                return NULL;
            bits[component++] = source->imm_values[selected];
        }
        return ast_create_literal_bits(bits, width, AST_SCALAR_FLOAT32);
    }
    if (ctx->high_level_domain && source->type == OPERAND_TYPE_DOMAIN_LOCATION) {
        ASTOperandProvenance origin;
        if (!hlsl_high_level_input_provenance(ctx, source, mask, &origin)) return NULL;
        if (origin.complete && origin.value_role == AST_OPERAND_VALUE_LOGICAL &&
            origin.selection_role == AST_COMPONENT_SELECTION_TRANSPORT &&
            origin.natural_components >= 2 && origin.natural_components <= 3 &&
            origin.result_components == width && origin.bitcast_role == AST_OPERAND_BITCAST_NONE &&
            !origin.raw_buffer_reconstruction && !origin.synthetic_interface)
            return domain_coordinate_composition(ctx, source, mask, instruction, operand, &origin);
    }
    return formatted_source_atom(ctx, source, mask, false, instruction, operand);
}

static ASTExpr *disjoint_temp_composition(HLSLEmitterContext *ctx, int instruction, int operand,
                                        uint8_t mask, const DXBCOperand *source,
                                        const uint8_t *logical_widths) {
    const int width = lane_count(mask);
    if (width < 2 || width > 4) return NULL;
    int owners[4], selections[4], lanes[4], leaf_count = 0;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(mask & (1u << lane))) continue;
        owners[leaf_count] = hlsl_operand_definition(ctx, instruction, operand, lane);
        selections[leaf_count] = usil_operand_source_component(source, lane);
        lanes[leaf_count++] = lane;
    }
    ASTExpr *arguments[4] = {0};
    int count = 0;
    for (int leaf = 0; leaf < leaf_count;) {
        const int owner = owners[leaf];
        if (owner < 0 || owner >= instruction || selections[leaf] < 0 || selections[leaf] > 3) goto failed;
        const DXBCOperand *destination = &ctx->program->instructions[owner].operands[0];
        const uint8_t producer_mask = usil_operand_destination_lane_mask(destination);
        const unsigned natural = logical_widths[owner];
        if (!natural || natural > 4 || !(producer_mask & (1u << selections[leaf]))) goto failed;
        char name[48];
        if (!value_name(ctx, owner, name)) goto failed;
        ASTExpr *value = ast_create_var(owner, source->register_index, OPERAND_TYPE_TEMP, name);
        value = logical_expression(ctx, value, owner, producer_mask, natural);
        if (!value) goto failed;
        /* Join a contiguous, ascending selection from one owned natural value.
         * A complete float3 zero producer remains one float3 argument rather
         * than three component reads spelling out physical register packing. */
        int group = 1;
        while (leaf + group < leaf_count && owners[leaf + group] == owner &&
               selections[leaf + group] == selections[leaf] + group &&
               (producer_mask & (1u << selections[leaf + group])) && natural > 1) ++group;
        const int first_component = lane_count((uint8_t)(producer_mask & ((1u << selections[leaf]) - 1u)));
        if (natural > 1 && (first_component || group != (int)natural)) {
            int components[4];
            for (int component = 0; component < group; ++component) {
                components[component] = first_component + component;
                if (components[component] >= (int)natural) { ast_free_expr(value); goto failed; }
            }
            ASTExpr *projection = ast_create_swizzle(value, components, group);
            if (!projection) { ast_free_expr(value); goto failed; }
            uint8_t projection_lanes = 0;
            for (int component = 0; component < group; ++component)
                projection_lanes |= (uint8_t)(1u << lanes[leaf + component]);
            value = logical_expression(ctx, projection, instruction, projection_lanes, (unsigned)group);
            if (!value) goto failed;
            value->logical_origin.semantic_projection = true;
        }
        arguments[count++] = value;
        leaf += group;
    }
    static const char *const constructors[] = {NULL, NULL, "float2", "float3", "float4"};
    ASTExpr *composition = ast_create_call(constructors[width], arguments, count);
    if (!composition) goto failed;
    return logical_expression(ctx, composition, instruction, mask, (unsigned)width);
failed:
    for (int component = 0; component < count; ++component) ast_free_expr(arguments[component]);
    return NULL;
}

static ASTExpr *source_expression_unmodified(HLSLEmitterContext *ctx, int instruction, int operand,
                                  const unsigned *uses, ASTExpr **pending, HLSLInstructionOwners *pending_owners,
                                  HLSLInstructionOwners *owners, const DXBCOperand *source,
                                  const uint8_t *logical_widths, bool compose_disjoint) {
    const uint8_t mask = source_lanes(ctx, instruction, operand);
    if (!mask) return NULL;
    if (source->type == OPERAND_TYPE_TEMP) {
        int definition = vector_definition(ctx, instruction, operand);
        if (definition < 0 && compose_disjoint)
            return disjoint_temp_composition(ctx, instruction, operand, mask, source, logical_widths);
        if (definition < 0)
            return NULL;
        ASTExpr *value = NULL;
        if (uses[definition] == 1) {
            value = pending[definition];
            pending[definition] = NULL;
            hlsl_instruction_owners_union(owners, &pending_owners[definition]);
            hlsl_instruction_owners_clear(&pending_owners[definition]);
        } else {
            char name[48];
            if (!value_name(ctx, definition, name))
                return NULL;
            value = ast_create_var(definition, source->register_index, OPERAND_TYPE_TEMP, name);
        }
        if (!value)
            return NULL;
        const uint8_t producer_mask = usil_operand_destination_lane_mask(
            &ctx->program->instructions[definition].operands[0]);
        const bool scalar_producer = logical_widths[definition] == 1;
        const int producer_width = scalar_producer ? 1 : lane_count(producer_mask);
        if (uses[definition] != 1) {
            value = logical_expression(ctx, value, definition, producer_mask,
                                       (unsigned)producer_width);
            if (!value) return NULL;
        }
        /* A dot result is one scalar even when DXBC replicates it into several
         * physical destination lanes. HLSL assignment/arithmetic broadcasts
         * that scalar; selecting those identical copies adds no operation. */
        if (scalar_producer)
            return value;
        int components[4], width = 0;
        bool identity = lane_count(mask) == producer_width;
        for (int lane = 0; lane < 4; ++lane) {
            if (!(mask & (1u << lane)))
                continue;
            const int selected = usil_operand_source_component(source, lane);
            if (selected < 0 || selected > 3 || !(producer_mask & (1u << selected))) {
                ast_free_expr(value);
                return NULL;
            }
            components[width] = lane_count((uint8_t)(producer_mask & ((1u << selected) - 1u)));
            identity = identity && components[width] == width;
            ++width;
        }
        if (identity)
            return value;
        bool same = true, ascending = true;
        for (int component = 1; component < width; ++component) {
            same = same && components[component] == components[0];
            ascending = ascending && components[component] > components[component - 1];
        }
        /* A repeated component of a proved logical vector is one scalar.
         * HLSL broadcasts it at its consumer; no .xxxx reconstruction remains. */
        if (same) width = 1;
        ASTExpr *selected = ast_create_swizzle(value, components, width);
        if (!selected)
            ast_free_expr(value);
        selected = logical_expression(ctx, selected, instruction, mask, (unsigned)width);
        if (selected) selected->logical_origin.semantic_projection = same || ascending;
        return selected;
    }
    if (mask == 15 && source->type != OPERAND_TYPE_CONSTANT_BUFFER &&
        source->type != OPERAND_TYPE_INPUT &&
        !(ctx->high_level_domain && (source->type == OPERAND_TYPE_INPUT_CONTROL_POINT ||
                                    source->type == OPERAND_TYPE_DOMAIN_LOCATION)))
        return hlsl_float4_source_atom(ctx, source);
    return vector_source_atom(ctx, source, mask, instruction, operand);
}

static ASTExpr *source_expression(HLSLEmitterContext *ctx, int instruction, int operand,
                                  const unsigned *uses, ASTExpr **pending, HLSLInstructionOwners *pending_owners,
                                  HLSLInstructionOwners *owners, const uint8_t *logical_widths,
                                  const HLSLPureExpressionScope *scope) {
    const DXBCOperand *original = &ctx->program->instructions[instruction].operands[operand];
    DXBCOperand unmodified = *original;
    unmodified.has_abs = unmodified.has_neg = false;
    unmodified.extended_tokens = NULL;
    unmodified.extended_token_count = 0;
    const uint8_t lanes = source_lanes(ctx, instruction, operand);
    ASTExpr *expression = scope && scope->source_supported &&
        scope->source_supported(ctx, instruction, operand, scope->context)
        ? scope->source_expression(ctx, instruction, operand, lanes, scope->context)
        : source_expression_unmodified(ctx, instruction, operand, uses,
            pending, pending_owners, owners, &unmodified, logical_widths,
            scope && scope->compose_disjoint_temp_lanes);
    if (expression && original->has_abs) {
        const unsigned width = expression_width(expression);
        ASTExpr *call = ast_create_call("abs", &expression, 1);
        if (!call) ast_free_expr(expression);
        expression = logical_expression(ctx, call, instruction, lanes,
                                         width ? width : (unsigned)lane_count(lanes));
    }
    if (expression && original->has_neg) {
        const unsigned width = expression_width(expression);
        /* The AST unary spelling is shared with integer negation; its owned
         * float origin preserves the actual source arithmetic domain. */
        ASTExpr *negative = ast_create_unary(USIL_OP_INEG, expression);
        if (!negative) ast_free_expr(expression);
        expression = logical_expression(ctx, negative, instruction, lanes,
                                         width ? width : (unsigned)lane_count(lanes));
    }
    return expression;
}

ASTExpr *hlsl_float4_source_atom(HLSLEmitterContext *ctx, const DXBCOperand *source) {
    if (source->type == OPERAND_TYPE_IMMEDIATE32) {
        ASTExpr *value = ast_create_literal_bits(source->imm_values, source->imm_value_count,
                                                 AST_SCALAR_FLOAT32);
        if (!value || source->imm_value_count != 1)
            return value;
        /* Every SSA value is a float4, including a replicated scalar literal. */
        ASTExpr *vector = ast_create_call("float4", &value, 1);
        if (!vector)
            ast_free_expr(value);
        /* The scalar's exact bits and this constructor's fixed numeric type
         * prove a four-component broadcast. Retain its consuming instruction
         * owner rather than leaving an opaque call inside a clean expression. */
        if (vector && ctx && ctx->program && ctx->program->instructions &&
            ctx->current_instruction_index >= 0 &&
            ctx->current_instruction_index < ctx->program->instruction_count)
            vector = logical_expression(ctx, vector, ctx->current_instruction_index, 15, 4);
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
    HLSLInstructionOwners owners;
    bool function;
    HLSLMatrixUseCapture *matrix_capture;
} ExpressionSpanContext;

static bool record_expression_span(void *context, const ASTExpr *expression, size_t begin,
                                   size_t end) {
    ExpressionSpanContext *trace = context;
    if (!hlsl_matrix_uses_span(trace->matrix_capture, expression, begin, end)) return false;
    for (size_t index = 0; trace->map && index < trace->map->count; ++index) {
        if (!hlsl_instruction_owners_contains(&trace->owners, (int)index) || trace->roots[index] != expression)
            continue;
        HLSLExpressionOrigin *origin = &trace->map->origins[index];
        if (origin->kind != HLSL_EXPRESSION_ORIGIN_UNMAPPED || begin >= end)
            return false;
        origin->kind =
            trace->function ? HLSL_EXPRESSION_ORIGIN_FUNCTION : HLSL_EXPRESSION_ORIGIN_EXPRESSION;
        origin->source_begin = begin;
        origin->source_end = end;
    }
    return true;
}

static bool finish_expression_origins(ExpressionSpanContext *trace, bool dead) {
    for (int index = 0; index < EXPRESSION_INSTRUCTION_LIMIT; ++index) {
        if (!hlsl_instruction_owners_contains(&trace->owners, (int)index))
            continue;
        if (trace->map) {
            HLSLExpressionOrigin *origin = &trace->map->origins[index];
            if (dead)
                origin->kind = HLSL_EXPRESSION_ORIGIN_DEAD;
            else if (origin->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION &&
                     origin->kind != HLSL_EXPRESSION_ORIGIN_FUNCTION)
                return false;
        }
        trace->roots[index] = NULL;
    }
    return true;
}

/* The selected compiler's identity-multiply spelling retains an ADD's
 * decoded child order. This constructs a candidate inverse, not an independent
 * floating-point equivalence proof. Complete compiler equality is required. */
static ASTExpr *ordered_add_expression(ASTExpr *left, ASTExpr *right) {
    const uint32_t one_bits = UINT32_C(0x3f800000);
    ASTExpr *one = ast_create_literal_bits(&one_bits, 1, AST_SCALAR_FLOAT32);
    ASTExpr *arguments[3] = {left, one, right};
    ASTExpr *expression = one ? ast_create_call("mad", arguments, 3) : NULL;
    if (!expression) ast_free_expr(one);
    return expression;
}

/* Always consumes both children, including on allocation failure. */
ASTExpr *hlsl_float4_operation(HLSLEmitterContext *ctx, int index, ASTExpr *left, ASTExpr *right) {
    const USILInstruction *inst = &ctx->program->instructions[index];
    if (inst->opcode == USIL_OP_MOV)
        return left;
    const uint8_t lanes = usil_operand_destination_lane_mask(&inst->operands[0]);
    const unsigned width = operation_width(lanes, left, right, NULL);
    ASTExpr *expression = NULL;
    if (compiler_add_uses_mad(inst)) {
        expression = ordered_add_expression(left, right);
    } else {
        bool swap = compiler_model_swaps_binary_operands(ctx, index);
        expression = ast_create_binary(inst->opcode, swap ? right : left, swap ? left : right);
    }
    if (!expression) {
        ast_free_expr(left);
        ast_free_expr(right);
    }
    return logical_expression(ctx, expression, index, lanes, width);
}

static ASTExpr *vector_operation(HLSLEmitterContext *ctx, int index, ASTExpr *left,
                                 ASTExpr *right, ASTExpr *third,
                                 const HLSLPureExpressionScope *scope) {
    const USILOpcode opcode = ctx->program->instructions[index].opcode;
    if (opcode == USIL_OP_ADD && scope && scope->ordered_add_supported &&
        scope->ordered_add_supported(ctx, index, scope->context)) {
        const uint8_t lanes = usil_operand_destination_lane_mask(
            &ctx->program->instructions[index].operands[0]);
        ASTExpr *expression = left && right && expression_width(left) == 1 &&
            expression_width(right) == 1 ? ordered_add_expression(left, right) : NULL;
        if (!expression) {
            ast_free_expr(left);
            ast_free_expr(right);
        }
        return logical_expression(ctx, expression, index, lanes, 1);
    }
    const char *intrinsic = float_intrinsic(opcode);
    if (intrinsic) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        const uint8_t lanes = usil_operand_destination_lane_mask(&instruction->operands[0]);
        const unsigned width = operation_width(lanes, left, NULL, NULL);
        const bool dot = float_dot(opcode);
        if (dot) {
            const unsigned input_width = opcode == USIL_OP_DP2 ? 2 : opcode == USIL_OP_DP3 ? 3 : 4;
            ASTExpr **inputs[] = {&left, &right};
            for (unsigned operand = 0; operand < 2; ++operand) {
                const unsigned actual_width = expression_width(*inputs[operand]);
                if (actual_width == input_width) continue;
                if (actual_width != 1) {
                    ast_free_expr(left);
                    ast_free_expr(right);
                    return NULL;
                }
                /* DPn still reduces n products when a logical scalar was
                 * replicated into its physical input lanes. An explicit
                 * numeric vector cast selects that overload without changing
                 * the scalar bits, evaluating twice, or reassociating the sum. */
                const char *type = input_width == 2 ? "float2" : input_width == 3 ? "float3" : "float4";
                ASTExpr *cast = ast_create_cast(type, *inputs[operand]);
                if (!cast) {
                    ast_free_expr(left);
                    ast_free_expr(right);
                    return NULL;
                }
                *inputs[operand] = logical_expression(ctx, cast, index, lanes, input_width);
                if (!*inputs[operand]) {
                    ast_free_expr(left);
                    ast_free_expr(right);
                    return NULL;
                }
            }
        }
        ASTExpr *arguments[2] = {left, right};
        ASTExpr *expression = left && (!dot || right)
            ? ast_create_call(intrinsic, arguments, dot ? 2 : 1) : NULL;
        if (!expression) {
            ast_free_expr(left);
            ast_free_expr(right);
        }
        expression = logical_expression(ctx, expression, index, lanes, dot ? 1u : width);
        return expression;
    }
    if (opcode == USIL_OP_MAD) {
        const uint8_t lanes = usil_operand_destination_lane_mask(
            &ctx->program->instructions[index].operands[0]);
        const unsigned product_width = operation_width(lanes, left, right, NULL);
        const unsigned width = operation_width(lanes, left, right, third);
        /* Use the same multiply/add spelling as the existing compiler inverse.
         * Exact full-container comparison, not this spelling, accepts it. */
        ASTExpr *product = left && right ? ast_create_binary(USIL_OP_MUL, left, right) : NULL;
        if (!product) {
            ast_free_expr(left);
            ast_free_expr(right);
            ast_free_expr(third);
            return NULL;
        }
        product = logical_expression(ctx, product, index, lanes, product_width);
        if (!product) {
            ast_free_expr(third);
            return NULL;
        }
        ASTExpr *expression = third ? ast_create_binary(USIL_OP_ADD, product, third) : NULL;
        if (!expression) {
            ast_free_expr(product);
            ast_free_expr(third);
        }
        return logical_expression(ctx, expression, index, lanes, width);
    }
    if (opcode == USIL_OP_MIN || opcode == USIL_OP_MAX) {
        const uint8_t lanes = usil_operand_destination_lane_mask(
            &ctx->program->instructions[index].operands[0]);
        const unsigned width = operation_width(lanes, left, right, NULL);
        const bool swap = compiler_model_swaps_binary_operands(ctx, index);
        ASTExpr *arguments[2] = {swap ? right : left, swap ? left : right};
        ASTExpr *expression = left && right
                                  ? ast_create_call(opcode == USIL_OP_MIN ? "min" : "max", arguments, 2)
                                  : NULL;
        if (!expression) {
            ast_free_expr(left);
            ast_free_expr(right);
        }
        return logical_expression(ctx, expression, index, lanes, width);
    }
    return hlsl_float4_operation(ctx, index, left, right);
}

bool hlsl_float4_append_output(HLSLEmitterContext *ctx, const DXBCOperand *destination) {
    StringBuilder text;
    sb_init(&text);
    const int mask = usil_operand_destination_lane_mask(destination) << 4;
    bool formatted = format_dest_operand_hlsl_sb(ctx, destination, false, false, mask, false, &text);
    if (formatted)
        sb_append(ctx->sb, text.buf);
    sb_free(&text);
    return formatted && sb_ok(ctx->sb);
}

static uint8_t instruction_destination_lanes(const USILProgram *program,
                                             const USILInstruction *instruction) {
    USILOperandUseInfo use;
    if (instruction->operand_count < 1 ||
        !usil_instruction_operand_use(program, instruction, 0, &use) ||
        use.use != USIL_OPERAND_USE_DESTINATION)
        return 0;
    return usil_operand_destination_lane_mask(&instruction->operands[0]);
}

void hlsl_expression_source_map_begin(HLSLEmitterContext *ctx) {
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
                origin->destination_lanes = instruction_destination_lanes(ctx->program, inst);
        }
    }
}

static bool emit_straightline_expressions(HLSLEmitterContext *ctx,
                                          const HLSLPureExpressionScope *scope) {
    const int first = scope ? scope->first_instruction : 0;
    const int end = scope ? scope->end_instruction : ctx->program->instruction_count;
    unsigned uses[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    ASTExpr *pending[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    ASTExpr *roots[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    uint8_t logical_widths[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    HLSLInstructionOwners pending_owners[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    HLSLMatrixLiftPlan matrix_plans[EXPRESSION_INSTRUCTION_LIMIT] = {0};
    int matrix_starts[EXPRESSION_INSTRUCTION_LIMIT];
    for (int index = 0; index < EXPRESSION_INSTRUCTION_LIMIT; ++index)
        matrix_starts[index] = -1;
    bool success = false;
    if (!validate_float_expressions(ctx, uses, false, scope ? NULL : matrix_plans, scope))
        goto cleanup;
    for (int index = first; index < end; ++index) {
        if (!matrix_plans[index].expression) continue;
        if (!hlsl_matrix_uses_plan(ctx, &matrix_plans[index])) goto cleanup;
        for (int claimed = index; claimed <= matrix_plans[index].end_instruction; ++claimed)
            matrix_starts[claimed] = index;
    }
    for (int index = first; index < end; ++index)
        if (ctx->float4_functions.group[index] >= 0)
            uses[index] = 2; /* Keep each call result at its original instruction site. */
    /* Method arguments are emitted in coordinate/LOD/gradient order. Distinct
     * pure SSA values can have been computed in another order in DXBC. Preserve
     * those evaluation sites with natural typed locals instead of moving their
     * operation graphs into a differently ordered argument list. */
    for (int index = first; index < end; ++index) {
        const USILInstruction *sample = &ctx->program->instructions[index];
        if (!hlsl_texture_sample_opcode(sample->opcode)) continue;
        const int source_operands[] = {1, 4, 5};
        int definitions[3], count = 0, previous = -1;
        bool reordered = false;
        for (size_t argument = 0; argument < 3; ++argument) {
            const int operand = source_operands[argument];
            if (operand >= sample->operand_count ||
                sample->operands[operand].type != OPERAND_TYPE_TEMP) continue;
            const int definition = vector_definition(ctx, index, operand);
            if (definition < 0) goto cleanup;
            definitions[count++] = definition;
            if (previous > definition) reordered = true;
            previous = definition;
        }
        if (reordered)
            for (int argument = 0; argument < count; ++argument)
                if (uses[definitions[argument]] < 2) uses[definitions[argument]] = 2;
    }
    if (!scope) hlsl_expression_source_map_begin(ctx);
    HLSLExpressionSourceMap *map = ctx->expression_source_map;
    for (int index = first; index < end; ++index) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        ctx->current_instruction_index = index;
        if (scope && scope->omitted_instructions &&
            hlsl_instruction_owners_contains(scope->omitted_instructions, index)) continue;
        if (inst->opcode == USIL_OP_NOP || inst->opcode == USIL_OP_RET)
            continue;
        if (ctx->high_level_geometry &&
            (inst->opcode == USIL_OP_GEOMETRY_APPEND ||
             inst->opcode == USIL_OP_GEOMETRY_RESTART_STRIP)) {
            const size_t begin = ctx->sb->len;
            if (!hlsl_emit_high_level_geometry_effect(ctx, index))
                goto cleanup;
            if (map) {
                map->origins[index].kind = HLSL_EXPRESSION_ORIGIN_EFFECT;
                map->origins[index].source_begin = begin;
                map->origins[index].source_end = ctx->sb->len;
            }
            continue;
        }
        HLSLMatrixLiftPlan *matrix = matrix_starts[index] >= 0
                                        ? &matrix_plans[matrix_starts[index]] : NULL;
        if (matrix && index != matrix->end_instruction) continue;
        if (index + 1 < end &&
            ctx->float4_functions.group[index + 1] >= 0)
            continue; /* The following call owns this single-use producer. */
        HLSLInstructionOwners owners = {0};
        if (!hlsl_instruction_owners_add(&owners, index)) goto cleanup;
        const int group = ctx->float4_functions.group[index];
        ASTExpr *expression = NULL;
        if (matrix) {
            expression = matrix->expression;
            matrix->expression = NULL; /* Ownership moves to this existing AST root. */
            owners = matrix->instruction_owners;
            for (int claimed = matrix->start_instruction; claimed <= index; ++claimed)
                roots[claimed] = matrix->world_expression && claimed < matrix->start_instruction + 4
                                     ? matrix->world_expression : expression;
        } else if (group >= 0) {
            if (!hlsl_instruction_owners_add(&owners, index - 1)) goto cleanup;
            expression = hlsl_float4_function_call(ctx, index);
            roots[index - 1] = expression;
            for (int operation = 0; map && operation < 2; ++operation) {
                HLSLExpressionOrigin *origin = &map->origins[index - 1 + operation];
                origin->definition_begin = ctx->float4_functions.definition_begin[group][operation];
                origin->definition_end = ctx->float4_functions.definition_end[group][operation];
            }
        } else {
            ASTExpr *left =
                source_expression(ctx, index, 1, uses, pending, pending_owners, &owners,
                                  logical_widths, scope);
            if (hlsl_texture_sample_opcode(inst->opcode)) {
                ASTExpr *parameter = inst->operand_count < 5 ? NULL
                    : source_expression(ctx, index, 4, uses, pending, pending_owners, &owners,
                                        logical_widths, scope);
                ASTExpr *second_parameter = inst->operand_count < 6 ? NULL
                    : source_expression(ctx, index, 5, uses, pending, pending_owners, &owners,
                                        logical_widths, scope);
                expression = hlsl_texture_sample_expression(ctx, index, left, parameter, second_parameter);
            } else {
                ASTExpr *right = inst->operand_count < 3 ? NULL
                    : source_expression(ctx, index, 2, uses, pending, pending_owners, &owners,
                                        logical_widths, scope);
                ASTExpr *third = inst->opcode == USIL_OP_MAD
                    ? source_expression(ctx, index, 3, uses, pending, pending_owners, &owners,
                                        logical_widths, scope) : NULL;
                expression = vector_operation(ctx, index, left, right, third, scope);
            }
        }
        if (!expression)
            goto cleanup;
        logical_widths[index] = (uint8_t)expression_width(expression);
        if (!logical_widths[index])
            logical_widths[index] = (uint8_t)lane_count(
                usil_operand_destination_lane_mask(&inst->operands[0]));
        roots[index] = expression;
        ExpressionSpanContext trace = {
            .map = map, .roots = roots, .owners = owners, .function = group >= 0,
            .matrix_capture = ctx->matrix_use_capture};
        const DXBCOperand *destination = &inst->operands[0];
        unsigned inline_nodes = 0;
        if (destination->type == OPERAND_TYPE_TEMP && uses[index] == 1 &&
            expression_exceeds_inline_limit(expression, 1, &inline_nodes))
            uses[index] = 2; /* Later consumers use this emitted logical value. */
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
            if (!value_name(ctx, index, name)) {
                ast_free_expr(expression);
                goto cleanup;
            }
            const int width = logical_widths[index];
            const char *type = width == 1 ? "float" : width == 2 ? "float2" :
                               width == 3 ? "float3" : "float4";
            sb_appendf(ctx->sb, "const %s %s", type, name);
            sb_append(ctx->sb, " = ");
        } else if (ctx->high_level_direct_return) {
            sb_append(ctx->sb, "return ");
        } else {
            if (!(scope ? scope->append_destination(ctx, index, scope->context)
                    : ctx->high_level_interface
                        ? hlsl_append_high_level_output(ctx, destination)
                        : hlsl_float4_append_output(ctx, destination))) {
                ast_free_expr(expression);
                goto cleanup;
            }
            sb_append(ctx->sb, " = ");
        }
        if (!hlsl_source_quality_observe_expression(ctx, expression, index))
            ctx->sb->failed = true;
        ast_format_expr_traced(expression, ctx->sb, map || ctx->matrix_use_capture ? record_expression_span : NULL, &trace);
        if (!finish_expression_origins(&trace, false))
            ctx->sb->failed = true;
        sb_append(ctx->sb, ";\n");
        hlsl_source_quality_emission(ctx,
            destination->type == OPERAND_TYPE_OUTPUT && !scope && !ctx->high_level_interface
                ? HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE : 0, false, index);
        ast_free_expr(expression);
        if (!sb_ok(ctx->sb))
            goto cleanup;
    }
    success = sb_ok(ctx->sb);
cleanup:
    for (int index = 0; index < EXPRESSION_INSTRUCTION_LIMIT; ++index) {
        ast_free_expr(pending[index]);
        hlsl_matrix_lift_plan_free(&matrix_plans[index]);
    }
    return success;
}

bool hlsl_emit_pure_expression_scope(HLSLEmitterContext *ctx,
                                      const HLSLPureExpressionScope *scope) {
    if (!ctx || !ctx->program || !scope || !scope->destination_supported ||
        !scope->append_destination || (!!scope->source_supported != !!scope->source_expression) ||
        scope->first_instruction < 0 ||
        scope->end_instruction <= scope->first_instruction ||
        scope->end_instruction > ctx->program->instruction_count ||
        ctx->program->instruction_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        !ctx->cfg.blocks || ctx->cfg.block_count < 1 ||
        !ctx->cfg.instruction_block || !ctx->ssa.operand_ssa_vars ||
        ctx->cfg.blocks[0].first_instruction != scope->first_instruction ||
        ctx->cfg.blocks[ctx->cfg.block_count - 1].last_instruction != scope->end_instruction - 1)
        return false;
    return emit_straightline_expressions(ctx, scope);
}

bool emit_high_level_expressions(HLSLEmitterContext *ctx) {
    if (ctx->unity_uv_helper) return emit_unity_uv_lift(ctx);
    for (int index = 0; index < ctx->program->instruction_count; ++index)
        if (ctx->program->instructions[index].opcode == USIL_OP_IF ||
            ctx->program->instructions[index].opcode == USIL_OP_LOOP)
            return emit_high_level_structured(ctx);
    return emit_straightline_expressions(ctx, NULL);
}

bool hlsl_expression_source_map_matches(const HLSLExpressionSourceMap *map,
                                        const USILProgram *program, const char *source) {
    if (hlsl_geometry_control_flow_admission(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE))
        return hlsl_geometry_control_flow_source_map_matches(map, program, source);
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
                                inst->opcode == USIL_OP_MUL || inst->opcode == USIL_OP_MAD ||
                                inst->opcode == USIL_OP_DIV || inst->opcode == USIL_OP_MIN ||
                                inst->opcode == USIL_OP_MAX || float_intrinsic(inst->opcode) ||
                                hlsl_texture_sample_opcode(inst->opcode);
        const uint8_t lanes = instruction_destination_lanes(program, inst);
        if (origin->destination_lanes != lanes)
            return false;
        switch (origin->kind) {
        case HLSL_EXPRESSION_ORIGIN_UNITY_UV:
            if (index != 0 || !hlsl_unity_uv_lift_matches(program))
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_FUNCTION:
            if (inst->opcode != USIL_OP_MUL || lanes != 15 ||
                inst->operands[0].type != OPERAND_TYPE_TEMP)
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_EXPRESSION:
            if (!expression || !lanes)
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_EFFECT:
            if (lanes || !hlsl_high_level_geometry_interface_supported(
                    program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) ||
                (inst->opcode != USIL_OP_GEOMETRY_APPEND &&
                 inst->opcode != USIL_OP_GEOMETRY_RESTART_STRIP))
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_CONTROL:
            if (inst->opcode != USIL_OP_IF && inst->opcode != USIL_OP_ELSE &&
                inst->opcode != USIL_OP_ENDIF)
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL:
            if (inst->opcode == USIL_OP_MOV || inst->opcode == USIL_OP_UGE ||
                inst->opcode == USIL_OP_IADD) {
                if (!lanes || (lanes & (lanes - 1)) || inst->operands[0].type != OPERAND_TYPE_TEMP)
                    return false;
            } else if (inst->opcode != USIL_OP_LOOP && inst->opcode != USIL_OP_BREAKC &&
                       inst->opcode != USIL_OP_ENDLOOP) {
                return false;
            }
            break;
        case HLSL_EXPRESSION_ORIGIN_RETURN:
            if (inst->opcode != USIL_OP_RET ||
                (index + 1 != map->count && !hlsl_hull_phase_return_owned(program, (int)index)))
                return false;
            break;
        case HLSL_EXPRESSION_ORIGIN_DEAD:
            if (!expression || inst->operands[0].type != OPERAND_TYPE_TEMP || !lanes)
                return false;
            {
                USILEffectFlags effects;
                if (!usil_instruction_effects(program, inst, &effects) || effects != USIL_EFFECT_NONE)
                    return false;
            }
            break;
        case HLSL_EXPRESSION_ORIGIN_NOP:
            if (inst->opcode != USIL_OP_NOP)
                return false;
            break;
        default:
            return false;
        }
        if (!hlsl_expression_origin_ranges_valid(origin, source_length))
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
    case HLSL_EXPRESSION_ORIGIN_CONTROL:
        return "control";
    case HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL:
        return "loop-control";
    case HLSL_EXPRESSION_ORIGIN_FUNCTION:
        return "function";
    case HLSL_EXPRESSION_ORIGIN_UNITY_UV:
        return HLSL_UNITY_UV_LIFT_ID;
    case HLSL_EXPRESSION_ORIGIN_EFFECT:
        return "effect";
    default:
        return "unmapped";
    }
}

bool hlsl_expression_origin_has_span(HLSLExpressionOriginKind kind) {
    return kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION || kind == HLSL_EXPRESSION_ORIGIN_RETURN ||
           kind == HLSL_EXPRESSION_ORIGIN_CONTROL || kind == HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL ||
           kind == HLSL_EXPRESSION_ORIGIN_FUNCTION || kind == HLSL_EXPRESSION_ORIGIN_UNITY_UV ||
           kind == HLSL_EXPRESSION_ORIGIN_EFFECT;
}

bool hlsl_expression_origin_ranges_valid(const HLSLExpressionOrigin *origin, size_t source_length) {
    if (!origin)
        return false;
    if (hlsl_expression_origin_has_span(origin->kind)) {
        if (origin->source_begin >= origin->source_end || origin->source_end > source_length)
            return false;
    } else if ((origin->kind != HLSL_EXPRESSION_ORIGIN_NOP &&
                origin->kind != HLSL_EXPRESSION_ORIGIN_DEAD) ||
               origin->source_begin || origin->source_end) {
        return false;
    }
    if (origin->kind == HLSL_EXPRESSION_ORIGIN_FUNCTION)
        return origin->definition_begin < origin->definition_end &&
               origin->definition_end <= origin->source_begin;
    return !origin->definition_begin && !origin->definition_end;
}

bool hlsl_expression_source_map_offset(HLSLExpressionSourceMap *map, size_t offset) {
    if (!map || !map->complete || map->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT)
        return false;
    for (size_t i = 0; i < map->count; ++i) {
        const HLSLExpressionOrigin *origin = &map->origins[i];
        if (!hlsl_expression_origin_ranges_valid(origin, SIZE_MAX - offset))
            return false;
    }
    for (size_t i = 0; i < map->count; ++i) {
        HLSLExpressionOrigin *origin = &map->origins[i];
        if (!hlsl_expression_origin_has_span(origin->kind))
            continue;
        origin->source_begin += offset;
        origin->source_end += offset;
        if (origin->kind == HLSL_EXPRESSION_ORIGIN_FUNCTION) {
            origin->definition_begin += offset;
            origin->definition_end += offset;
        }
    }
    return true;
}

bool hlsl_expression_source_map_rebase_line(HLSLExpressionSourceMap *map,
                                            const HLSLExpressionSourceMap *original,
                                            size_t line_begin, size_t line_end,
                                            size_t output_begin) {
    if (!map || !original || !original->complete || map->count != original->count ||
        original->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT || line_begin > line_end ||
        output_begin > SIZE_MAX - (line_end - line_begin))
        return false;
    for (size_t i = 0; i < original->count; ++i) {
        const HLSLExpressionOrigin *from = &original->origins[i];
        HLSLExpressionOrigin *to = &map->origins[i];
        if (!hlsl_expression_origin_has_span(from->kind))
            continue;
        const size_t starts[] = {from->source_begin, from->definition_begin};
        const size_t ends[] = {from->source_end, from->definition_end};
        size_t *to_starts[] = {&to->source_begin, &to->definition_begin};
        size_t *to_ends[] = {&to->source_end, &to->definition_end};
        const int ranges = from->kind == HLSL_EXPRESSION_ORIGIN_FUNCTION ? 2 : 1;
        for (int range = 0; range < ranges; ++range) {
            /* A newline end precedes new indentation; a start follows it. */
            if (starts[range] >= line_begin && starts[range] < line_end)
                *to_starts[range] = output_begin + starts[range] - line_begin;
            if (ends[range] > line_begin && ends[range] <= line_end)
                *to_ends[range] = output_begin + ends[range] - line_begin;
        }
    }
    return true;
}
