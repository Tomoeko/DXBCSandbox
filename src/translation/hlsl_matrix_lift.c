// SPDX-License-Identifier: GPL-3.0-only

#include "hlsl_matrix_lift.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <string.h>
#include <limits.h>

const char *hlsl_matrix_lift_identifier(const HLSLEmitterContext *ctx, int buffer,
                                       int first_row, bool *row_major) {
    if (!ctx || !ctx->program || !row_major || !ctx->cbuffer_layouts_built ||
        ctx->program->cbuffer_count < 0 ||
        ctx->program->cbuffer_count > HLSL_MAX_CBUFFER_LAYOUTS ||
        (ctx->program->cbuffer_count && !ctx->program->cbuffers) ||
        ctx->cbuffer_layout_count < 0 ||
        ctx->cbuffer_layout_count > HLSL_MAX_CBUFFER_LAYOUTS || buffer < 0 ||
        first_row < 0 || (uint32_t)first_row > UINT32_MAX / 16u)
        return NULL;
    for (int index = 0; index < ctx->program->cbuffer_count; ++index)
        if (ctx->program->cbuffers[index].reg_idx == buffer &&
            ctx->program->cbuffers[index].dynamic_indexed)
            return NULL;
    const HLSLCBufferLayout *layout = get_cbuffer_emission_layout(ctx, buffer);
    if (!layout || layout->raw_storage || !layout->has_serialized_authority ||
        layout->variable_count <= 0 || !layout->variables)
        return NULL;
    const TempVariable *match = NULL;
    const uint32_t offset = (uint32_t)first_row * 16u;
    for (int index = 0; index < layout->variable_count; ++index) {
        const TempVariable *variable = &layout->variables[index];
        if (variable->byte_offset != offset) continue;
        if (match) return NULL;
        match = variable;
    }
    if (!match || !match->name || !match->name[0] || match->type != 0 ||
        !match->is_matrix || match->rows != 4 || match->dim != 4 ||
        match->matrix_array_size || match->byte_size != 64 ||
        (match->authority != 1 && match->authority != 2))
        return NULL;
    /* A second projected object with the same spelling cannot supply an
     * independent type/orientation authority to a named HLSL expression. */
    for (int cb = 0; cb < ctx->cbuffer_layout_count; ++cb) {
        const HLSLCBufferLayout *candidate = &ctx->cbuffer_layouts[cb];
        if (candidate->variable_count < 0 ||
            (candidate->variable_count && !candidate->variables))
            return NULL;
        for (int index = 0; index < candidate->variable_count; ++index) {
            const TempVariable *variable = &candidate->variables[index];
            if (variable != match && variable->name &&
                strcmp(variable->name, match->name) == 0)
                return NULL;
        }
    }
    for (int row = 0; row < 4; ++row) {
        for (int component = 0; component < 4; ++component) {
            int relative = -1;
            const char *name = resolve_cb_variable_ctx(
                ctx, buffer, first_row + row, component, &relative);
            if (!name || strcmp(name, match->name) != 0 ||
                relative != row * 16 + component * 4)
                return NULL;
        }
    }
    *row_major = match->row_major;
    return match->name;
}

static bool chain_dataflow_is_owned(const HLSLEmitterContext *ctx,
                                    const HLSLMatrixVectorChain *chain) {
    if (!ctx->ssa.operand_ssa_vars || !ctx->use_def.definition_use_counts ||
        ctx->ssa.instruction_count != ctx->program->instruction_count ||
        ctx->use_def.instruction_count != ctx->program->instruction_count)
        return false;
    unsigned uses[8][4] = {{0}};
    for (int offset = 0; offset < 8; ++offset) {
        const int index = chain->start_instruction + offset;
        const USILInstruction *instruction = &ctx->program->instructions[index];
        if ((ctx->compiler_model.claim_owner && ctx->compiler_model.claim_owner[index] >= 0) ||
            (ctx->semantic_program.claim_owner && ctx->semantic_program.claim_owner[index] >= 0))
            return false;
        USILEffectFlags effects;
        if (!usil_instruction_effects(ctx->program, instruction, &effects) ||
            effects != USIL_EFFECT_NONE)
            return false;
        for (int operand = 1; operand < instruction->operand_count; ++operand) {
            const DXBCOperand *source = &instruction->operands[operand];
            if (source->type != OPERAND_TYPE_TEMP) continue;
            const int expected = offset <= 3 ? index - 1 :
                ((offset == 4 || operand == 2) ? chain->start_instruction + 3 : index - 1);
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, instruction, operand, &use) ||
                use.use != USIL_OPERAND_USE_SOURCE)
                return false;
            for (int lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane))) continue;
                if (hlsl_operand_definition(ctx, index, operand, lane) != expected)
                    return false;
                const int selected = usil_operand_source_component(source, lane);
                if (selected < 0 || selected > 3 || expected < chain->start_instruction ||
                    expected >= index)
                    return false;
                ++uses[expected - chain->start_instruction][selected];
            }
        }
    }
    /* Includes phi edges and all later consumers. No world/clip intermediate
     * can disappear into the nested expression if it has another use. */
    for (int offset = 0; offset < 7; ++offset)
        for (int lane = 0; lane < 4; ++lane)
            if (hlsl_definition_use_count(ctx, chain->start_instruction + offset, lane) !=
                uses[offset][lane])
                return false;
    return true;
}

static bool set_result_origin(const HLSLEmitterContext *ctx, ASTExpr *expression,
                              int instruction, uint64_t identity) {
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = AST_SCALAR_FLOAT32;
    origin.components = 4;
    origin.logical_value_id = identity;
    origin.instruction_index = instruction;
    origin.source_instruction_index = ctx->program->instructions[instruction].source_instruction_index;
    origin.destination_lanes = 15;
    return ast_set_logical_value_origin(expression, &origin);
}

static ASTExpr *matrix_atom(const HLSLEmitterContext *ctx, const char *name, int buffer,
                            int row, int instruction, int operand) {
    ASTOperandProvenance origin;
    ast_operand_provenance_init(&origin);
    origin.complete = true;
    origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    origin.logical_value_id = (((uint64_t)(uint32_t)buffer + 1) << 32) | ((uint32_t)row * 16u);
    origin.instruction_index = instruction;
    origin.source_instruction_index = ctx->program->instructions[instruction].source_instruction_index;
    origin.operand_index = operand;
    origin.destination_lanes = 15;
    return ast_create_emitter_operand_with_provenance(name, &origin);
}

/* Always consumes matrix/vector, including constructor failure. */
static ASTExpr *matrix_product(ASTExpr *matrix, ASTExpr *vector, bool row_major) {
    ASTExpr *arguments[2] = {row_major ? vector : matrix, row_major ? matrix : vector};
    ASTExpr *result = matrix && vector ? ast_create_call("mul", arguments, 2) : NULL;
    if (!result) {
        ast_free_expr(matrix);
        ast_free_expr(vector);
    }
    return result;
}

static bool prepare_nested_matrix(HLSLEmitterContext *ctx, int start,
                              HLSLMatrixLiftPlan *out_plan) {
    if (!out_plan) return false;
    memset(out_plan, 0, sizeof(*out_plan));
    HLSLMatrixVectorChain chain;
    if (!ctx || !ctx->program || !ctx->high_level_interface ||
        !hlsl_compiler_matrix_vector_chain_matches(ctx->program, start, &chain) ||
        start < 0 || start > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT - 8 ||
        !chain_dataflow_is_owned(ctx, &chain))
        return false;
    bool world_row_major, clip_row_major;
    const char *world_name = hlsl_matrix_lift_identifier(
        ctx, chain.world_matrix_buffer, chain.world_matrix_first_row, &world_row_major);
    const char *clip_name = hlsl_matrix_lift_identifier(
        ctx, chain.clip_matrix_buffer, chain.clip_matrix_first_row, &clip_row_major);
    if (!world_name || !clip_name) return false;
    /* The graph consumes xyz and proves the fourth input is a constant one.
     * This semantic projection is derived from those edges, not from a helper
     * name or an assumed UnityObjectToClipPos implementation. */
    DXBCOperand position = ctx->program->instructions[start].operands[1];
    position.swizzle_mode = 1;
    for (int lane = 0; lane < 4; ++lane) position.swizzle[lane] = lane;
    ASTOperandProvenance position_origin;
    if (!hlsl_high_level_input_provenance(ctx, &position, 7, &position_origin)) return false;
    position_origin.instruction_index = start;
    position_origin.source_instruction_index = ctx->program->instructions[start].source_instruction_index;
    position_origin.operand_index = 1;
    position_origin.destination_lanes = 15;
    StringBuilder spelling;
    sb_init(&spelling);
    const int previous_instruction = ctx->current_instruction_index;
    ctx->current_instruction_index = start;
    const bool formatted = format_operand_hlsl_sb(ctx, &position, false, false, 0x70, false, &spelling);
    ctx->current_instruction_index = previous_instruction;
    ASTExpr *xyz = formatted && sb_ok(&spelling)
        ? ast_create_emitter_operand_with_provenance(spelling.buf, &position_origin) : NULL;
    sb_free(&spelling);
    const uint32_t one_bits = UINT32_C(0x3f800000);
    ASTExpr *one = ast_create_literal_bits(&one_bits, 1, AST_SCALAR_FLOAT32);
    ASTExpr *arguments[2] = {xyz, one};
    ASTExpr *homogeneous = xyz && one ? ast_create_call("float4", arguments, 2) : NULL;
    if (!homogeneous) {
        ast_free_expr(xyz);
        ast_free_expr(one);
        return false;
    }
    const uint64_t world_identity = (UINT64_C(1) << 62) | (uint32_t)(start + 3);
    if (!set_result_origin(ctx, homogeneous, start + 3,
                           (UINT64_C(1) << 61) | (uint32_t)(start + 3))) {
        ast_free_expr(homogeneous);
        return false;
    }
    ASTExpr *world = matrix_product(
        matrix_atom(ctx, world_name, chain.world_matrix_buffer, chain.world_matrix_first_row,
                    start + 1, 1), homogeneous, world_row_major);
    if (!world || !set_result_origin(ctx, world, start + 3, world_identity)) {
        ast_free_expr(world);
        return false;
    }
    ASTExpr *clip = matrix_product(
        matrix_atom(ctx, clip_name, chain.clip_matrix_buffer, chain.clip_matrix_first_row,
                    start + 5, 1), world, clip_row_major);
    if (!clip || !set_result_origin(ctx, clip, start + 7,
                                   (UINT64_C(1) << 62) | (uint32_t)(start + 7))) {
        ast_free_expr(clip);
        return false;
    }
    *out_plan = (HLSLMatrixLiftPlan){.start_instruction = start, .end_instruction = start + 7,
        .instruction_owners = UINT64_C(255) << start,
        .result_components = 4, .claimed_instruction_count = 8, .expression = clip,
        .world_expression = world};
    return true;
}

static bool single_operand_plain(const DXBCOperand *operand) {
    return operand && !operand->has_abs && !operand->has_neg && !operand->min_precision &&
           !operand->extended_token_count && !operand->extended_tokens &&
           !operand->rel_op0 && !operand->rel_op1 && !operand->rel_op2;
}

static bool single_static_operand(const DXBCOperand *operand, unsigned dimensions) {
    if (!single_operand_plain(operand) || operand->register_index < 0 ||
        operand->register_index_dim != (int)dimensions) return false;
    for (unsigned dimension = 0; dimension < dimensions; ++dimension) {
        const int expected = dimension == 0 ? operand->register_index : operand->rel_offset0;
        if (expected < 0 || operand->index_representations[dimension] != 0 ||
            !operand->index_has_immediate[dimension] || operand->index_value_exceeds_int[dimension] ||
            operand->index_values[dimension] != (uint32_t)expected) return false;
    }
    return true;
}

static bool single_vector_source(const DXBCOperand *operand, DXBCOperandType type,
                                  int reg, unsigned dimensions, int row) {
    if (operand->type != type || operand->register_index != reg ||
        !single_static_operand(operand, dimensions) || operand->destination_mask ||
        (dimensions == 2 && operand->rel_offset0 != row)) return false;
    for (int lane = 0; lane < 4; ++lane)
        if (usil_operand_source_component(operand, lane) != lane) return false;
    return true;
}

static bool single_scalar_input(const DXBCOperand *operand, int reg, int component) {
    if (operand->type != OPERAND_TYPE_INPUT || operand->register_index != reg ||
        !single_static_operand(operand, 1) || operand->destination_mask) return false;
    for (int lane = 0; lane < 4; ++lane)
        if (usil_operand_source_component(operand, lane) != component) return false;
    return true;
}

/* The admitted compiler order is y, x, z, w. Do not commute rows or reassociate
 * accumulation; the full compiler container gate remains independent. */
static bool prepare_single_matrix(HLSLEmitterContext *ctx, int start,
                                   HLSLMatrixLiftPlan *out_plan) {
    if (!ctx || !ctx->program || !ctx->high_level_interface || !ctx->sb ||
        start < 0 || start > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT - 4 ||
        ctx->program->instruction_count < 4 || start > ctx->program->instruction_count - 4 ||
        !ctx->program->instructions || !ctx->ssa.operand_ssa_vars ||
        !ctx->use_def.definition_use_counts ||
        ctx->ssa.instruction_count != ctx->program->instruction_count ||
        ctx->use_def.instruction_count != ctx->program->instruction_count) return false;
    const USILInstruction *chain = ctx->program->instructions + start;
    if (chain[0].operand_count != 3 || chain[0].opcode != USIL_OP_MUL) return false;
    const int accumulator = chain[0].operands[0].register_index;
    const int input = chain[0].operands[1].register_index;
    const int buffer = chain[0].operands[2].register_index;
    const int row_one = chain[0].operands[2].rel_offset0;
    if (accumulator < 0 || accumulator >= ctx->program->temp_count || input < 0 ||
        buffer < 0 || row_one < 1 || row_one > INT_MAX - 2) return false;
    const int first_row = row_one - 1;
    const int components[4] = {1, 0, 2, 3};
    for (int offset = 0; offset < 4; ++offset) {
        const int index = start + offset;
        const USILInstruction *instruction = &chain[offset];
        if (instruction->opcode != (offset ? USIL_OP_MAD : USIL_OP_MUL) ||
            instruction->operand_count != (offset ? 4 : 3) ||
            !usil_instruction_shape_valid(ctx->program, instruction) ||
            instruction->saturate || instruction->precise_mask ||
            instruction->condition_test != DXBC_INSTRUCTION_TEST_NONE ||
            instruction->has_resource_dimension || instruction->has_resource_return_types ||
            instruction->has_texel_offset || instruction->sync_flags ||
            (ctx->compiler_model.claim_owner && ctx->compiler_model.claim_owner[index] >= 0) ||
            (ctx->semantic_program.claim_owner && ctx->semantic_program.claim_owner[index] >= 0))
            return false;
        if (instruction->resource_dimension[0] || instruction->resource_stride ||
            instruction->resource_info_return_type || instruction->sample_info_return_type ||
            instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE ||
            instruction->geometry_stream_id || instruction->geometry_stream_explicit) return false;
        for (int lane = 0; lane < 4; ++lane)
            if (instruction->resource_return_types[lane]) return false;
        for (int axis = 0; axis < 3; ++axis)
            if (instruction->texel_offsets[axis]) return false;
        USILEffectFlags effects;
        if (!usil_instruction_effects(ctx->program, instruction, &effects) ||
            effects != USIL_EFFECT_NONE) return false;
        const DXBCOperand *destination = &instruction->operands[0];
        if (!single_static_operand(destination, 1) || destination->swizzle_mode ||
            usil_operand_destination_lane_mask(destination) != 15 ||
            destination->type != (offset == 3 ? OPERAND_TYPE_OUTPUT : OPERAND_TYPE_TEMP) ||
            (offset != 3 && destination->register_index != accumulator) ||
            (offset == 3 && destination->register_index >= HLSL_SM5_IO_REGISTER_COUNT)) return false;
        const int input_operand = offset ? 2 : 1;
        const int matrix_operand = offset ? 1 : 2;
        if (!single_scalar_input(&instruction->operands[input_operand], input, components[offset]) ||
            !single_vector_source(&instruction->operands[matrix_operand],
                                  OPERAND_TYPE_CONSTANT_BUFFER, buffer, 2,
                                  first_row + components[offset])) return false;
        if (!offset) continue;
        if (!single_vector_source(&instruction->operands[3], OPERAND_TYPE_TEMP,
                                  accumulator, 1, 0)) return false;
        for (int lane = 0; lane < 4; ++lane)
            if (hlsl_operand_definition(ctx, index, 3, lane) != index - 1) return false;
    }
    for (int offset = 0; offset < 3; ++offset)
        for (int lane = 0; lane < 4; ++lane)
            if (hlsl_definition_use_count(ctx, start + offset, lane) != 1) return false;
    bool row_major;
    const char *matrix_name = hlsl_matrix_lift_identifier(ctx, buffer, first_row, &row_major);
    if (!matrix_name) return false;
    DXBCOperand vector = chain[0].operands[1];
    vector.swizzle_mode = 1;
    for (int lane = 0; lane < 4; ++lane) vector.swizzle[lane] = lane;
    ASTOperandProvenance provenance;
    if (!hlsl_high_level_input_provenance(ctx, &vector, 15, &provenance)) return false;
    provenance.instruction_index = start;
    provenance.source_instruction_index = chain[0].source_instruction_index;
    provenance.operand_index = 1;
    provenance.destination_lanes = 15;
    StringBuilder spelling;
    sb_init(&spelling);
    const int previous_instruction = ctx->current_instruction_index;
    ctx->current_instruction_index = start;
    const bool formatted = format_operand_hlsl_sb(ctx, &vector, false, false, 0xf0, false, &spelling);
    ctx->current_instruction_index = previous_instruction;
    ASTExpr *input_expression = formatted && sb_ok(&spelling)
        ? ast_create_emitter_operand_with_provenance(spelling.buf, &provenance) : NULL;
    sb_free(&spelling);
    ASTExpr *product = matrix_product(
        matrix_atom(ctx, matrix_name, buffer, first_row, start + 1, 1), input_expression, row_major);
    if (!product || !set_result_origin(ctx, product, start + 3,
                                     (UINT64_C(1) << 62) | (uint32_t)(start + 3))) {
        ast_free_expr(product); return false;
    }
    *out_plan = (HLSLMatrixLiftPlan){.start_instruction = start, .end_instruction = start + 3,
        .instruction_owners = UINT64_C(15) << start, .result_components = 4,
        .claimed_instruction_count = 4, .expression = product};
    return true;
}

bool hlsl_matrix_lift_prepare(HLSLEmitterContext *ctx, int start,
                              HLSLMatrixLiftPlan *out_plan) {
    if (!out_plan) return false;
    memset(out_plan, 0, sizeof(*out_plan));
    if (prepare_nested_matrix(ctx, start, out_plan)) return true;
    return prepare_single_matrix(ctx, start, out_plan);
}

void hlsl_matrix_lift_plan_free(HLSLMatrixLiftPlan *plan) {
    if (!plan) return;
    ast_free_expr(plan->expression);
    memset(plan, 0, sizeof(*plan));
}
