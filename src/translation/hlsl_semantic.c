// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <stdlib.h>
#include <string.h>

static const HLSLSemanticContract k_static_uv_contract = {
    HLSL_SEMANTIC_STATIC_UV_SELECTION, "unity.static_uv_selection", 300, true};
static const HLSLSemanticContract k_surface_clip_contract = {
    HLSL_SEMANTIC_SURFACE_CLIP_POSITION, "unity.surface_clip_position", 400,
    true};
static const HLSLSemanticContract k_surface_tangent_contract = {
    HLSL_SEMANTIC_SURFACE_TANGENT_FRAME, "unity.surface_tangent_frame", 400,
    true};
static const HLSLSemanticContract k_face_basis_contract = {
    HLSL_SEMANTIC_INDEXED_FACE_BASIS, "unity.indexed_face_basis", 450, true};
static const HLSLSemanticContract k_shadow_position_contract = {
    HLSL_SEMANTIC_SHADOW_WORLD_POSITION, "unity.shadow_world_position", 350,
    true};
static const HLSLSemanticContract k_alpha_clip_contract = {
    HLSL_SEMANTIC_DEFERRED_ALPHA_CLIP, "source.deferred_alpha_clip", 500,
    false};
static const HLSLSemanticContract k_speedtree_wind_contract = {
    HLSL_SEMANTIC_SPEEDTREE_GLOBAL_WIND, "unity.speedtree_global_wind", 600,
    true};
static const HLSLSemanticContract k_readable_screen_position_contract = {
    HLSL_SEMANTIC_SCREEN_POSITION, "unity.compute_screen_position", 300,
    true};
static const HLSLSemanticContract k_truthiness_contract = {
    HLSL_SEMANTIC_TRUTHINESS_CONDITION, "source.truthiness_condition", 200,
    false};

static bool same_cb_register(const DXBCOperand *left,
                             const DXBCOperand *right) {
    return left->type == OPERAND_TYPE_CONSTANT_BUFFER &&
           right->type == OPERAND_TYPE_CONSTANT_BUFFER &&
           left->register_index == right->register_index &&
           left->rel_offset0 == right->rel_offset0 &&
           left->rel_offset1 == right->rel_offset1;
}

static const DXBCOperand *find_operand_type(const USILInstruction *inst,
                                            DXBCOperandType type) {
    for (int operand = 1; operand < inst->operand_count; operand++) {
        if (inst->operands[operand].type == type)
            return &inst->operands[operand];
    }
    return NULL;
}

static bool operand_is_cb_variable(const HLSLEmitterContext *ctx,
                                   const DXBCOperand *operand,
                                   const char *expected) {
    if (!operand || operand->type != OPERAND_TYPE_CONSTANT_BUFFER) return false;
    int source_component =
        operand->swizzle_mode == 2 ? operand->swizzle[0] : 0;
    int component_offset = 0;
    const char *variable = resolve_cb_variable_ctx(
        ctx, operand->register_index, operand->rel_offset0, source_component,
        &component_offset);
    return variable && strcmp(variable, expected) == 0 &&
           component_offset == 0;
}

static bool register_lift(HLSLEmitterContext *ctx,
                          const HLSLSemanticLift *candidate) {
    HLSLSemanticProgram *semantic = &ctx->semantic_program;
    int instruction_count = ctx->program->instruction_count;
    if (candidate->trigger_instruction >= 0 &&
        (candidate->trigger_instruction >= instruction_count ||
         semantic->before_lift[candidate->trigger_instruction] >= 0)) {
        semantic->conflict_count++;
        return false;
    }
    if (candidate->after_instruction >= 0 &&
        (candidate->after_instruction >= instruction_count ||
         semantic->after_lift[candidate->after_instruction] >= 0)) {
        semantic->conflict_count++;
        return false;
    }
    for (int claim = 0; claim < candidate->claimed_count; claim++) {
        int instruction = candidate->claimed[claim];
        if (instruction < 0 || instruction >= instruction_count ||
            semantic->claim_owner[instruction] >= 0) {
            semantic->conflict_count++;
            return false;
        }
    }
    if (semantic->lift_count >= semantic->lift_capacity) return false;
    int lift_id = semantic->lift_count++;
    semantic->lifts[lift_id] = *candidate;
    if (candidate->trigger_instruction >= 0)
        semantic->before_lift[candidate->trigger_instruction] = lift_id;
    if (candidate->after_instruction >= 0)
        semantic->after_lift[candidate->after_instruction] = lift_id;
    for (int claim = 0; claim < candidate->claimed_count; claim++) {
        int instruction = candidate->claimed[claim];
        semantic->claim_owner[instruction] = lift_id;
        if (instruction != candidate->trigger_instruction)
            ctx->skip_instruction[instruction] = true;
    }
    return true;
}

static void register_high_priority_structural_lifts(HLSLEmitterContext *ctx) {
    const IndexedFaceBasisInfo *face = &ctx->indexed_face_basis;
    if (face->valid) {
        HLSLSemanticLift lift;
        memset(&lift, 0, sizeof(lift));
        lift.contract = &k_face_basis_contract;
        lift.trigger_instruction = face->start;
        lift.after_instruction = -1;
        for (int instruction = face->start; instruction <= face->end;
             instruction++)
            lift.claimed[lift.claimed_count++] = instruction;
        lift.data.face_basis.output_register = face->output_reg;
        lift.data.face_basis.uv_input_register = face->uv_input_reg;
        register_lift(ctx, &lift);
    }
    const SurfaceTangentFrameInfo *surface = &ctx->surface_tangent_frame;
    if (surface->valid) {
        HLSLSemanticLift clip;
        memset(&clip, 0, sizeof(clip));
        clip.contract = &k_surface_clip_contract;
        clip.trigger_instruction = surface->transform_start;
        clip.after_instruction = -1;
        clip.data.surface.phase = 0;
        for (int instruction = surface->transform_start;
             instruction <= surface->transform_start + 8; instruction++)
            clip.claimed[clip.claimed_count++] = instruction;
        if (surface->clip_temp_reg >= 0)
            clip.claimed[clip.claimed_count++] = surface->transform_start + 9;
        register_lift(ctx, &clip);

        HLSLSemanticLift tangent;
        memset(&tangent, 0, sizeof(tangent));
        tangent.contract = &k_surface_tangent_contract;
        tangent.trigger_instruction = surface->tangent_start;
        tangent.after_instruction = -1;
        tangent.data.surface.phase = 1;
        for (int instruction = surface->tangent_start - 1;
             instruction <= surface->tangent_end; instruction++)
            tangent.claimed[tangent.claimed_count++] = instruction;
        register_lift(ctx, &tangent);
    }
}

static void register_readable_screen_position_lift(HLSLEmitterContext *ctx) {
    int trigger = ctx->readable_screen_pos_mul_y_idx >= 0
                      ? ctx->readable_screen_pos_mul_y_idx
                      : ctx->readable_screen_pos_mul_xzw_idx;
    if (trigger < 0) return;
    HLSLSemanticLift lift;
    memset(&lift, 0, sizeof(lift));
    lift.contract = &k_readable_screen_position_contract;
    lift.trigger_instruction = trigger;
    lift.after_instruction = -1;
    int indices[] = {ctx->readable_screen_pos_mul_y_idx,
                     ctx->readable_screen_pos_mul_xzw_idx,
                     ctx->readable_screen_pos_mov_idx,
                     ctx->readable_screen_pos_add_idx};
    for (unsigned int item = 0; item < sizeof(indices) / sizeof(indices[0]);
         item++) {
        int instruction = indices[item];
        bool duplicate = false;
        for (int claim = 0; claim < lift.claimed_count; claim++)
            duplicate |= lift.claimed[claim] == instruction;
        if (instruction >= 0 && !duplicate)
            lift.claimed[lift.claimed_count++] = instruction;
    }
    if (ctx->readable_screen_pos_mul_y_idx >= 0 &&
        ctx->readable_screen_pos_mul_xzw_idx == ctx->readable_screen_pos_mul_y_idx + 2)
        lift.claimed[lift.claimed_count++] = ctx->readable_screen_pos_mul_y_idx + 1;
    register_lift(ctx, &lift);
}

static void detect_static_uv_lifts(HLSLEmitterContext *ctx) {
    const USILProgram *program = ctx->program;
    for (int index = 0; index + 3 < program->instruction_count; index++) {
        const USILInstruction *condition_inst = &program->instructions[index];
        const USILInstruction *lightmap = &program->instructions[index + 1];
        const USILInstruction *dynamic = &program->instructions[index + 2];
        const USILInstruction *select = &program->instructions[index + 3];
        if (condition_inst->opcode != USIL_OP_NE ||
            condition_inst->operand_count < 3) continue;
        const DXBCOperand *condition = find_operand_type(
            condition_inst, OPERAND_TYPE_CONSTANT_BUFFER);
        const DXBCOperand *zero = find_operand_type(
            condition_inst, OPERAND_TYPE_IMMEDIATE32);
        const DXBCOperand *lightmap_input = find_operand_type(
            lightmap, OPERAND_TYPE_INPUT);
        const DXBCOperand *dynamic_input = find_operand_type(
            dynamic, OPERAND_TYPE_INPUT);
        const DXBCOperand *lightmap_st = find_operand_type(
            lightmap, OPERAND_TYPE_CONSTANT_BUFFER);
        const DXBCOperand *dynamic_st = find_operand_type(
            dynamic, OPERAND_TYPE_CONSTANT_BUFFER);
        if (!condition || !zero || zero->imm_value_count < 1 ||
            zero->imm_values[0] != 0 ||
            !operand_is_cb_variable(ctx, condition, "_StaticUV1") ||
            lightmap->opcode != USIL_OP_MAD ||
            dynamic->opcode != USIL_OP_MAD ||
            select->opcode != USIL_OP_MOVC || select->operand_count < 4 ||
            !lightmap_input || !dynamic_input ||
            !operand_is_cb_variable(ctx, lightmap_st, "unity_LightmapST") ||
            !operand_is_cb_variable(ctx, dynamic_st,
                                    "unity_DynamicLightmapST") ||
            select->operands[1].type != OPERAND_TYPE_TEMP ||
            select->operands[1].register_index !=
                condition_inst->operands[0].register_index) {
            continue;
        }
        HLSLSemanticLift lift;
        memset(&lift, 0, sizeof(lift));
        lift.contract = &k_static_uv_contract;
        lift.trigger_instruction = index;
        lift.after_instruction = -1;
        lift.claimed_count = 4;
        for (int claim = 0; claim < 4; claim++)
            lift.claimed[claim] = index + claim;
        lift.data.static_uv.select = index + 3;
        lift.data.static_uv.lightmap_input = lightmap_input->register_index;
        lift.data.static_uv.dynamic_input = dynamic_input->register_index;
        register_lift(ctx, &lift);
    }
}

static void detect_shadow_position_lifts(HLSLEmitterContext *ctx) {
    const USILProgram *program = ctx->program;
    for (int index = 4; index + 3 < program->instruction_count; index++) {
        const USILInstruction *mul = &program->instructions[index];
        const USILInstruction *mad_x = &program->instructions[index + 1];
        const USILInstruction *mad_z = &program->instructions[index + 2];
        const USILInstruction *mad_w = &program->instructions[index + 3];
        if (mul->opcode != USIL_OP_MUL || mad_x->opcode != USIL_OP_MAD ||
            mad_z->opcode != USIL_OP_MAD || mad_w->opcode != USIL_OP_MAD ||
            mul->operand_count < 3 || mad_x->operand_count < 4 ||
            mad_z->operand_count < 4 || mad_w->operand_count < 4 ||
            mul->operands[0].type != OPERAND_TYPE_TEMP ||
            mul->operands[0].destination_mask != (16 | 32 | 64) ||
            mad_w->operands[0].type != OPERAND_TYPE_OUTPUT ||
            mad_w->operands[0].destination_mask != (16 | 32 | 64) ||
            mul->operands[1].type != OPERAND_TYPE_INPUT ||
            mul->operands[2].type != OPERAND_TYPE_CONSTANT_BUFFER) continue;
        int temporary = mul->operands[0].register_index;
        int cb = mul->operands[2].register_index;
        if (mad_x->operands[0].type != OPERAND_TYPE_TEMP ||
            mad_z->operands[0].type != OPERAND_TYPE_TEMP ||
            mad_x->operands[0].register_index != temporary ||
            mad_z->operands[0].register_index != temporary ||
            mad_x->operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
            mad_z->operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
            mad_w->operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
            mad_x->operands[1].register_index != cb ||
            mad_z->operands[1].register_index != cb ||
            mad_w->operands[1].register_index != cb ||
            mul->operands[2].rel_offset0 != 1 ||
            mad_x->operands[1].rel_offset0 != 0 ||
            mad_z->operands[1].rel_offset0 != 2 ||
            mad_w->operands[1].rel_offset0 != 3) continue;
        int component_offset = 0;
        const char *matrix = resolve_cb_variable_ctx(ctx, cb, 0, 0,
                                                     &component_offset);
        if (!matrix || strcmp(matrix, "unity_ObjectToWorld") != 0 ||
            component_offset != 0) continue;
        bool has_bias_min = false;
        bool has_output_mad = false;
        bool has_output_mov = false;
        for (int previous = index - 1;
             previous >= 0 && previous >= index - 12; previous--) {
            const USILInstruction *bias = &program->instructions[previous];
            has_bias_min |= bias->opcode == USIL_OP_MIN;
            if (bias->operand_count > 0 &&
                bias->operands[0].type == OPERAND_TYPE_OUTPUT) {
                has_output_mad |= bias->opcode == USIL_OP_MAD;
                has_output_mov |= bias->opcode == USIL_OP_MOV;
            }
        }
        if (!has_bias_min || !has_output_mad || !has_output_mov) continue;
        HLSLSemanticLift lift;
        memset(&lift, 0, sizeof(lift));
        lift.contract = &k_shadow_position_contract;
        lift.trigger_instruction = -1;
        lift.after_instruction = -1;
        lift.claimed_count = 4;
        for (int claim = 0; claim < 4; claim++)
            lift.claimed[claim] = index + claim;
        lift.data.shadow_position.output_register =
            mad_w->operands[0].register_index;
        lift.data.shadow_position.input_register =
            mul->operands[1].register_index;
        lift.data.shadow_position.matrix = matrix;
        register_lift(ctx, &lift);
        return;
    }
}

static bool find_alpha_clip_for_move(const HLSLEmitterContext *ctx,
                                     int move_index,
                                     HLSLSemanticLift *lift) {
    const USILProgram *program = ctx->program;
    const USILInstruction *move = &program->instructions[move_index];
    if (move->opcode != USIL_OP_MOV || move->operand_count < 2 ||
        move->operands[0].type != OPERAND_TYPE_OUTPUT ||
        move->operands[1].type != OPERAND_TYPE_TEMP || move_index < 3) {
        return false;
    }
    int color_register = move->operands[1].register_index;
    int multiply_index = move_index - 1;
    const USILInstruction *multiply = &program->instructions[multiply_index];
    if (multiply->opcode != USIL_OP_MUL || multiply->operand_count < 3 ||
        multiply->operands[0].type != OPERAND_TYPE_TEMP ||
        multiply->operands[0].register_index != color_register ||
        multiply->operands[0].destination_mask != (16 | 32 | 64 | 128)) {
        return false;
    }
    int color_temp_operand = -1;
    int color_cb_operand = -1;
    for (int operand = 1; operand <= 2; operand++) {
        if (multiply->operands[operand].type == OPERAND_TYPE_TEMP &&
            multiply->operands[operand].register_index == color_register)
            color_temp_operand = operand;
        else if (multiply->operands[operand].type ==
                 OPERAND_TYPE_CONSTANT_BUFFER)
            color_cb_operand = operand;
    }
    if (color_temp_operand < 0 || color_cb_operand < 0) return false;

    for (int mad_index = multiply_index - 1;
         mad_index >= 0 && mad_index >= multiply_index - 24; mad_index--) {
        const USILInstruction *mad = &program->instructions[mad_index];
        if (mad->opcode != USIL_OP_MAD || mad->operand_count < 4 ||
            mad->operands[0].type != OPERAND_TYPE_TEMP ||
            get_mask_component_count(mad->operands[0].destination_mask) != 1)
            continue;
        int alpha_temp_operand = -1;
        int alpha_cb_operand = -1;
        for (int operand = 1; operand <= 2; operand++) {
            if (mad->operands[operand].type == OPERAND_TYPE_TEMP &&
                mad->operands[operand].register_index == color_register)
                alpha_temp_operand = operand;
            else if (same_cb_register(&mad->operands[operand],
                                      &multiply->operands[color_cb_operand]))
                alpha_cb_operand = operand;
        }
        if (alpha_temp_operand < 0 || alpha_cb_operand < 0) continue;
        int compare_index = -1;
        for (int index = mad_index + 1; index <= move_index + 1; index++) {
            const USILInstruction *candidate = &program->instructions[index];
            if (candidate->opcode == USIL_OP_LT &&
                candidate->operand_count >= 3 &&
                candidate->operands[1].type == OPERAND_TYPE_TEMP &&
                candidate->operands[1].register_index ==
                    mad->operands[0].register_index) {
                compare_index = index;
                break;
            }
        }
        if (compare_index < 0 ||
            compare_index + 1 >= program->instruction_count) continue;
        const USILInstruction *compare = &program->instructions[compare_index];
        const USILInstruction *discard =
            &program->instructions[compare_index + 1];
        if (discard->opcode != USIL_OP_DISCARD ||
            discard->operand_count < 1 ||
            discard->operands[0].type != OPERAND_TYPE_TEMP ||
            discard->operands[0].register_index !=
                compare->operands[0].register_index) continue;
        int alpha_component = mad->operands[alpha_temp_operand].swizzle[0];
        if (!component_value_unchanged(ctx, color_register, alpha_component,
                                       mad_index + 1, multiply_index)) {
            continue;
        }
        memset(lift, 0, sizeof(*lift));
        lift->contract = &k_alpha_clip_contract;
        lift->trigger_instruction = -1;
        lift->after_instruction = move_index;
        lift->claimed[lift->claimed_count++] = mad_index;
        if (compare_index < multiply_index) {
            lift->claimed[lift->claimed_count++] = compare_index;
            lift->claimed[lift->claimed_count++] = compare_index + 1;
        }
        lift->data.alpha_clip.mad = mad_index;
        lift->data.alpha_clip.compare = compare_index;
        lift->data.alpha_clip.discard = compare_index + 1;
        lift->data.alpha_clip.multiply = multiply_index;
        lift->data.alpha_clip.move = move_index;
        lift->data.alpha_clip.color_temp_operand = color_temp_operand;
        lift->data.alpha_clip.first_operand_is_cb =
            mad->operands[1].type == OPERAND_TYPE_CONSTANT_BUFFER;
        return true;
    }
    return false;
}

static void detect_alpha_clip_lifts(HLSLEmitterContext *ctx) {
    for (int index = 0; index < ctx->program->instruction_count; index++) {
        HLSLSemanticLift lift;
        if (find_alpha_clip_for_move(ctx, index, &lift))
            register_lift(ctx, &lift);
    }
}

static bool speedtree_shape_matches(const HLSLEmitterContext *ctx, int index) {
    const USILProgram *program = ctx->program;
    if (index + 10 >= program->instruction_count) return false;
    const USILInstruction *inst = &program->instructions[index];
    static const USILOpcode tail[] = {
        USIL_OP_MOV, USIL_OP_DP3, USIL_OP_RSQ, USIL_OP_MUL, USIL_OP_DP3,
        USIL_OP_SQRT, USIL_OP_MUL, USIL_OP_MUL, USIL_OP_LT, USIL_OP_MOVC};
    if (inst->opcode != USIL_OP_MAD || inst->operand_count < 4 ||
        inst->operands[0].type != OPERAND_TYPE_TEMP ||
        inst->operands[0].destination_mask != (16 | 64) ||
        inst->operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
        inst->operands[2].type != OPERAND_TYPE_TEMP ||
        inst->operands[3].type != OPERAND_TYPE_TEMP) return false;
    for (int offset = 0; offset < 10; offset++) {
        if (program->instructions[index + 1 + offset].opcode != tail[offset])
            return false;
    }
    const USILInstruction *move_y = &program->instructions[index + 1];
    const USILInstruction *select = &program->instructions[index + 10];
    if (move_y->operand_count < 2 || select->operand_count < 4 ||
        move_y->operands[0].type != OPERAND_TYPE_TEMP ||
        move_y->operands[0].register_index !=
            inst->operands[0].register_index ||
        move_y->operands[0].destination_mask != 32 ||
        move_y->operands[1].type != OPERAND_TYPE_TEMP ||
        move_y->operands[1].register_index !=
            inst->operands[3].register_index ||
        move_y->operands[1].swizzle[0] != 3 ||
        select->operands[0].type != OPERAND_TYPE_TEMP ||
        select->operands[0].register_index !=
            inst->operands[0].register_index ||
        select->operands[0].destination_mask != (16 | 32 | 64)) return false;
    return operand_is_cb_variable(ctx, &inst->operands[1],
                                  "_ST_WindVector");
}

static void detect_speedtree_wind_lifts(HLSLEmitterContext *ctx) {
    for (int index = 0; index < ctx->program->instruction_count; index++) {
        if (!speedtree_shape_matches(ctx, index)) continue;
        HLSLSemanticLift lift;
        memset(&lift, 0, sizeof(lift));
        lift.contract = &k_speedtree_wind_contract;
        lift.trigger_instruction = index;
        lift.after_instruction = -1;
        lift.claimed_count = 11;
        for (int claim = 0; claim < 11; claim++)
            lift.claimed[claim] = index + claim;
        lift.data.speedtree_wind.start = index;
        register_lift(ctx, &lift);
    }
}

static bool is_sh_l2_output(const HLSLEmitterContext *ctx, int index) {
    const USILInstruction *inst = &ctx->program->instructions[index];
    if (inst->opcode != USIL_OP_MAD || inst->operand_count < 4 ||
        inst->operands[0].type != OPERAND_TYPE_OUTPUT ||
        inst->operands[0].destination_mask != (16 | 32 | 64) ||
        inst->operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
        inst->operands[2].type != OPERAND_TYPE_TEMP ||
        inst->operands[3].type != OPERAND_TYPE_TEMP ||
        inst->operands[2].register_index !=
            inst->operands[3].register_index ||
        !is_replicate_swizzle(&inst->operands[2]) ||
        inst->operands[2].swizzle[0] != 3 ||
        inst->operands[3].swizzle_mode != 1 ||
        inst->operands[3].swizzle[0] != 0 ||
        inst->operands[3].swizzle[1] != 1 ||
        inst->operands[3].swizzle[2] != 2) return false;
    return operand_is_cb_variable(ctx, &inst->operands[1], "unity_SHC");
}

static bool is_sh_linear_output(const HLSLEmitterContext *ctx, int index) {
    const USILInstruction *inst = &ctx->program->instructions[index];
    if (index < 3 || inst->opcode != USIL_OP_ADD ||
        inst->operand_count < 3 ||
        inst->operands[0].type != OPERAND_TYPE_OUTPUT ||
        inst->operands[0].destination_mask != (16 | 32 | 64) ||
        inst->operands[1].type != OPERAND_TYPE_TEMP ||
        inst->operands[2].type != OPERAND_TYPE_TEMP) return false;
    int sh_register = inst->operands[2].register_index;
    static const char *const names[3] = {
        "unity_SHAr", "unity_SHAg", "unity_SHAb"};
    for (int component = 0; component < 3; component++) {
        const USILInstruction *dp4 =
            &ctx->program->instructions[index - 3 + component];
        if (dp4->opcode != USIL_OP_DP4 || dp4->operand_count < 3 ||
            dp4->operands[0].type != OPERAND_TYPE_TEMP ||
            dp4->operands[0].register_index != sh_register ||
            dp4->operands[0].destination_mask != (16 << component) ||
            !operand_is_cb_variable(ctx, &dp4->operands[1],
                                    names[component])) return false;
    }
    return true;
}

static bool is_vertex_light_accumulation(const HLSLEmitterContext *ctx,
                                         int index) {
    const USILProgram *program = ctx->program;
    const USILInstruction *inst = &program->instructions[index];
    if (index < 1 || strncmp(program->shader_type_model, "vs", 2) != 0 ||
        inst->opcode != USIL_OP_MUL || inst->operand_count < 3 ||
        inst->operands[0].type != OPERAND_TYPE_TEMP ||
        inst->operands[1].type != OPERAND_TYPE_TEMP ||
        inst->operands[2].type != OPERAND_TYPE_TEMP ||
        inst->operands[2].register_index !=
            inst->operands[0].register_index) return false;
    const USILInstruction *color = &program->instructions[index - 1];
    if (color->opcode != USIL_OP_MUL || color->operand_count < 3 ||
        color->operands[0].type != OPERAND_TYPE_TEMP ||
        color->operands[0].register_index !=
            inst->operands[0].register_index) return false;
    for (int operand = 1; operand < color->operand_count; operand++) {
        if (operand_is_cb_variable(ctx, &color->operands[operand],
                                   "_LightColor0")) return true;
    }
    return false;
}

static void detect_instruction_annotations(HLSLEmitterContext *ctx) {
    for (int index = 0; index < ctx->program->instruction_count; index++) {
        if (is_sh_l2_output(ctx, index) || is_sh_linear_output(ctx, index)) {
            ctx->semantic_program.instruction_flags[index] |=
                HLSL_SEMANTIC_FLAG_UNITY_SH_VECTOR_OUTPUT;
        }
        if (is_vertex_light_accumulation(ctx, index))
            ctx->semantic_program.binary_operand_order[index] = 1;
    }
}

static bool comparison_lane_has_only_movc_predicate_uses(
    const HLSLEmitterContext *ctx, int definition, int lane) {
    const USILInstruction *comparison =
        &ctx->program->instructions[definition];
    int register_index = comparison->operands[0].register_index;
    bool found_use = false;
    for (int index = definition + 1;
         index < ctx->program->instruction_count; index++) {
        const USILInstruction *use = &ctx->program->instructions[index];
        int first_source = inst_writes_to_dest(use) ? 1 : 0;
        for (int operand = first_source; operand < use->operand_count;
             operand++) {
            const DXBCOperand *source = &use->operands[operand];
            if (source->type != OPERAND_TYPE_TEMP ||
                source->register_index != register_index) {
                continue;
            }
            for (int component = 0; component < 4; component++) {
                if (usil_operand_source_component(source, component) != lane ||
                    hlsl_operand_definition(ctx, index, operand, component) !=
                        definition) {
                    continue;
                }
                found_use = true;
                if (use->opcode != USIL_OP_MOVC || operand != 1 ||
                    ctx->semantic_program.claim_owner[index] >= 0) {
                    return false;
                }
            }
        }
    }
    return found_use;
}

static void detect_truthiness_condition_lifts(HLSLEmitterContext *ctx) {
    /*
     * Pixel-stage scalar conditionals are lowered through Boolean temporaries.
     * Vertex-stage predicates participate in different packing and scheduling
     * rules and are handled by their structural semantic contracts instead.
     */
    if (strncmp(ctx->program->shader_type_model, "ps", 2) != 0) return;
    for (int index = 0; index < ctx->program->instruction_count; index++) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        if (ctx->semantic_program.claim_owner[index] >= 0 ||
            inst->opcode != USIL_OP_NE || inst->operand_count < 3 ||
            inst->operands[0].type != OPERAND_TYPE_TEMP) {
            continue;
        }
        int immediate_operand =
            inst->operands[1].type == OPERAND_TYPE_IMMEDIATE32 ? 1 :
            inst->operands[2].type == OPERAND_TYPE_IMMEDIATE32 ? 2 : -1;
        if (immediate_operand < 0) continue;
        const DXBCOperand *immediate = &inst->operands[immediate_operand];
        bool all_zero = immediate->imm_value_count > 0;
        for (int component = 0; component < immediate->imm_value_count;
             component++) {
            all_zero &= immediate->imm_values[component] == 0;
        }
        if (!all_zero) continue;

        unsigned char mask = 0;
        for (int lane = 0; lane < 4; lane++) {
            if (!(inst->operands[0].destination_mask & (16 << lane)))
                continue;
            if (!comparison_lane_has_only_movc_predicate_uses(ctx, index,
                                                               lane)) {
                mask = 0;
                break;
            }
            mask |= (unsigned char)(1u << lane);
        }
        if (mask == 0) continue;

        HLSLSemanticLift lift;
        memset(&lift, 0, sizeof(lift));
        lift.contract = &k_truthiness_contract;
        lift.trigger_instruction = index;
        lift.after_instruction = -1;
        lift.claimed[0] = index;
        lift.claimed_count = 1;
        lift.data.truthiness.value_operand =
            immediate_operand == 1 ? 2 : 1;
        lift.data.truthiness.component_mask = mask;
        register_lift(ctx, &lift);
    }
}

bool analyze_semantic_lifts(HLSLEmitterContext *ctx) {
    HLSLSemanticProgram *semantic = &ctx->semantic_program;
    int instruction_count = ctx->program->instruction_count;
    detect_readable_screen_pos_pattern(ctx);
    detect_surface_tangent_frame(ctx);
    detect_indexed_face_basis(ctx);
    semantic->lift_capacity = instruction_count * 2 + 8;
    semantic->lifts = (HLSLSemanticLift *)calloc(
        (size_t)semantic->lift_capacity, sizeof(HLSLSemanticLift));
    semantic->before_lift = (int *)malloc(
        (size_t)instruction_count * sizeof(int));
    semantic->after_lift = (int *)malloc(
        (size_t)instruction_count * sizeof(int));
    semantic->claim_owner = (int *)malloc(
        (size_t)instruction_count * sizeof(int));
    semantic->instruction_flags = (unsigned int *)calloc(
        (size_t)instruction_count, sizeof(unsigned int));
    semantic->binary_operand_order = (signed char *)malloc(
        (size_t)instruction_count * sizeof(signed char));
    if (!semantic->lifts || !semantic->before_lift ||
        !semantic->after_lift || !semantic->claim_owner ||
        !semantic->instruction_flags || !semantic->binary_operand_order) {
        free_semantic_lifts(ctx);
        return false;
    }
    for (int index = 0; index < instruction_count; index++) {
        semantic->before_lift[index] = -1;
        semantic->after_lift[index] = -1;
        semantic->claim_owner[index] = -1;
        semantic->binary_operand_order[index] = -1;
    }
    /* Registration is intentionally highest-priority first. A lower-priority
     * contract that claims an already-owned instruction is rejected, making
     * overlap resolution independent of emitter call order. */
    detect_speedtree_wind_lifts(ctx);
    detect_alpha_clip_lifts(ctx);
    register_high_priority_structural_lifts(ctx);
    detect_shadow_position_lifts(ctx);
    detect_static_uv_lifts(ctx);
    register_readable_screen_position_lift(ctx);
    detect_instruction_annotations(ctx);
    detect_truthiness_condition_lifts(ctx);
    return true;
}

void free_semantic_lifts(HLSLEmitterContext *ctx) {
    HLSLSemanticProgram *semantic = &ctx->semantic_program;
    free(semantic->lifts);
    free(semantic->before_lift);
    free(semantic->after_lift);
    free(semantic->claim_owner);
    free(semantic->instruction_flags);
    free(semantic->binary_operand_order);
    memset(semantic, 0, sizeof(*semantic));
}

int semantic_alpha_clip_first_operand(const HLSLEmitterContext *ctx,
                                      int multiply_instruction) {
    const HLSLSemanticProgram *semantic = &ctx->semantic_program;
    for (int lift_id = 0; lift_id < semantic->lift_count; lift_id++) {
        const HLSLSemanticLift *lift = &semantic->lifts[lift_id];
        if (lift->contract->kind == HLSL_SEMANTIC_DEFERRED_ALPHA_CLIP &&
            lift->data.alpha_clip.multiply == multiply_instruction) {
            return lift->data.alpha_clip.first_operand_is_cb;
        }
    }
    return -1;
}

bool semantic_instruction_has_flag(const HLSLEmitterContext *ctx,
                                   int instruction, unsigned int flag) {
    return ctx->semantic_program.instruction_flags && instruction >= 0 &&
           instruction < ctx->program->instruction_count &&
           (ctx->semantic_program.instruction_flags[instruction] & flag) != 0;
}

int semantic_binary_operand_order(const HLSLEmitterContext *ctx,
                                  int instruction) {
    if (!ctx->semantic_program.binary_operand_order || instruction < 0 ||
        instruction >= ctx->program->instruction_count) return -1;
    return ctx->semantic_program.binary_operand_order[instruction];
}

bool semantic_truthiness_condition_alias(HLSLEmitterContext *ctx,
                                         int instruction, int operand,
                                         int component, char *alias,
                                         size_t alias_size) {
    if (!ctx || !alias || alias_size == 0 || instruction < 0 ||
        instruction >= ctx->program->instruction_count || operand < 0 ||
        operand >= ctx->program->instructions[instruction].operand_count) {
        return false;
    }
    const DXBCOperand *source =
        &ctx->program->instructions[instruction].operands[operand];
    int lane = usil_operand_source_component(source, component);
    int definition =
        hlsl_operand_definition(ctx, instruction, operand, component);
    if (definition < 0) return false;
    for (int lift_id = 0; lift_id < ctx->semantic_program.lift_count;
         lift_id++) {
        const HLSLSemanticLift *lift =
            &ctx->semantic_program.lifts[lift_id];
        if (lift->contract->kind != HLSL_SEMANTIC_TRUTHINESS_CONDITION ||
            lift->trigger_instruction != definition ||
            !(lift->data.truthiness.component_mask & (1u << lane))) {
            continue;
        }
        return hlsl_format_checked(ctx, alias, alias_size,
                                   "dxbc_truth_%d_%c", definition,
                                   "xyzw"[lane]);
    }
    return false;
}

static void emit_readable_screen_position(HLSLEmitterContext *ctx) {
    int mul_y_idx = ctx->readable_screen_pos_mul_y_idx;
    int mul_xzw_idx = ctx->readable_screen_pos_mul_xzw_idx;
    int add_idx = ctx->readable_screen_pos_add_idx;
    int mov_idx = ctx->readable_screen_pos_mov_idx;
    const USILInstruction *mul_xzw =
        &ctx->program->instructions[mul_xzw_idx];
    const USILInstruction *add = &ctx->program->instructions[add_idx];
    char work[128];
    char clip[128];
    char output[128];
    format_dest_operand_hlsl(ctx, &mul_xzw->operands[0], false, false, 240,
                             false, work, sizeof(work));
    char *dot = strchr(work, '.');
    if (dot) *dot = '\0';
    format_operand_hlsl(ctx, &mul_xzw->operands[1], false, false, 240, false,
                        clip, sizeof(clip));
    dot = strchr(clip, '.');
    if (dot) *dot = '\0';
    format_dest_operand_hlsl(ctx, &add->operands[0], false, false, 240, false,
                             output, sizeof(output));
    dot = strchr(output, '.');
    if (dot) *dot = '\0';
    char position_output[32] = "";
    if (mul_y_idx < 0) {
        for (int index = 0; index < ctx->program->output_count; index++) {
            const DXBCSignatureElement *element =
                &ctx->program->outputs[index];
            const char *semantic = dxbc_signature_semantic_name(element);
            if (dxbc_ascii_strcasecmp(semantic, "position") == 0 ||
                dxbc_ascii_strcasecmp(semantic, "sv_position") == 0) {
                hlsl_format_checked(ctx, position_output, sizeof(position_output), "o%d",
                         element->register_id);
                break;
            }
        }
    }
    const char *source = position_output[0] ? position_output : clip;
    char projection[128] = "1.0f";
    if (mul_y_idx >= 0) {
        const USILInstruction *mul_y =
            &ctx->program->instructions[mul_y_idx];
        format_operand_hlsl(ctx, &mul_y->operands[2], false, false, 16,
                            false, projection, sizeof(projection));
    }
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb, "// Optimized ComputeScreenPos sequence\n");
    int output_mask = add->operands[0].destination_mask;
    if (output_mask == 48) {
        bool copies_only_w = mov_idx >= 0 &&
            ctx->program->instructions[mov_idx].operands[0].destination_mask ==
                128;
        if (copies_only_w) {
            sb_appendf(ctx->sb, "%s.xy = %s(%s, %s).xy;\n", output,
                       ctx->readable_screen_pos_helper, source, projection);
        } else if (mov_idx >= 0) {
            sb_appendf(ctx->sb, "%s = %s(%s, %s);\n", output,
                       ctx->readable_screen_pos_helper, source, projection);
        } else {
            sb_appendf(ctx->sb, "%s.xy = %s(%s, %s).xy;\n", output,
                       ctx->readable_screen_pos_helper, source, projection);
        }
        if (copies_only_w) {
            sb_append_spaces(ctx->sb, ctx->indent);
            sb_appendf(ctx->sb, "%s.w = %s.w;\n", output, source);
        }
    } else {
        sb_appendf(ctx->sb, "%s.zw = %s(%s, %s).xy;\n", output,
                   ctx->readable_screen_pos_helper, source, projection);
    }
    if (mul_y_idx >= 0) {
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb, "%s.y = %s.y * %s;\n", clip, clip,
                   projection);
    }
}

static void emit_static_uv(HLSLEmitterContext *ctx,
                           const HLSLSemanticLift *lift) {
    const USILInstruction *select = &ctx->program->instructions[
        lift->data.static_uv.select];
    const USILInstruction *lightmap = &ctx->program->instructions[
        lift->data.static_uv.select - 2];
    const USILInstruction *dynamic = &ctx->program->instructions[
        lift->data.static_uv.select - 1];
    char destination[128];
    char lightmap_destination[128];
    char dynamic_destination[128];
    char true_value[128];
    char false_value[128];
    format_dest_operand_hlsl(ctx, &select->operands[0], false, false,
                             select->operands[0].destination_mask, false,
                             destination, sizeof(destination));
    format_dest_operand_hlsl(ctx, &lightmap->operands[0], false, false,
                             lightmap->operands[0].destination_mask, false,
                             lightmap_destination,
                             sizeof(lightmap_destination));
    format_dest_operand_hlsl(ctx, &dynamic->operands[0], false, false,
                             dynamic->operands[0].destination_mask, false,
                             dynamic_destination,
                             sizeof(dynamic_destination));
    format_operand_hlsl(ctx, &select->operands[2], false, false,
                        select->operands[0].destination_mask, false,
                        true_value, sizeof(true_value));
    format_operand_hlsl(ctx, &select->operands[3], false, false,
                        select->operands[0].destination_mask, false,
                        false_value, sizeof(false_value));
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb, "bool dxbc_static_uv_condition = _StaticUV1;\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "%s = v%d.xy * unity_LightmapST.xy + "
               "unity_LightmapST.zw;\n",
               lightmap_destination, lift->data.static_uv.lightmap_input);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "%s = v%d.xy * unity_DynamicLightmapST.xy + "
               "unity_DynamicLightmapST.zw;\n",
               dynamic_destination, lift->data.static_uv.dynamic_input);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb, "%s = dxbc_static_uv_condition ? %s : %s;\n",
               destination, true_value, false_value);
}

static void emit_truthiness_condition(HLSLEmitterContext *ctx,
                                      const HLSLSemanticLift *lift) {
    const USILInstruction *inst = &ctx->program->instructions[
        lift->trigger_instruction];
    const DXBCOperand *value =
        &inst->operands[lift->data.truthiness.value_operand];
    for (int lane = 0; lane < 4; lane++) {
        if (!(lift->data.truthiness.component_mask & (1u << lane))) continue;
        DXBCOperand scalar = *value;
        scalar.swizzle_mode = 2;
        scalar.swizzle[0] = (uint8_t)usil_operand_source_component(value, lane);
        char source[128];
        format_operand_hlsl(ctx, &scalar, false, false, 16, false, source,
                            sizeof(source));
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb, "bool dxbc_truth_%d_%c = %s;\n",
                   lift->trigger_instruction, "xyzw"[lane], source);
    }
}

static void emit_surface(HLSLEmitterContext *ctx,
                         const HLSLSemanticLift *lift) {
    const SurfaceTangentFrameInfo *info = &ctx->surface_tangent_frame;
    if (lift->data.surface.phase == 0) {
        sb_append_spaces(ctx->sb, ctx->indent);
        if (info->clip_temp_reg >= 0) {
            sb_appendf(ctx->sb,
                       "r%d = mul(unity_MatrixVP, mul(unity_ObjectToWorld, "
                       "float4(v%d.xyz, 1.0f)));\n",
                       info->clip_temp_reg, info->position_input_reg);
        } else {
            sb_appendf(ctx->sb,
                       "o%d = mul(unity_MatrixVP, mul(unity_ObjectToWorld, "
                       "float4(v%d.xyz, 1.0f)));\n",
                       info->clip_output_reg, info->position_input_reg);
        }
        if (info->clip_temp_reg >= 0) {
            sb_append_spaces(ctx->sb, ctx->indent);
            sb_appendf(ctx->sb, "o%d = r%d;\n", info->clip_output_reg,
                       info->clip_temp_reg);
        }
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "float3 dxbc_surface_world_position = "
                   "mul(unity_ObjectToWorld, v%d).xyz;\n",
                   info->position_input_reg);
        return;
    }
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float3 dxbc_surface_world_normal = normalize(mul(v%d.xyz, "
               "(float3x3)unity_WorldToObject));\n",
               info->normal_input_reg);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float3 dxbc_surface_world_tangent = normalize(mul("
               "(float3x3)unity_ObjectToWorld, v%d.xyz));\n",
               info->tangent_input_reg);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float dxbc_surface_tangent_sign = v%d.w * "
               "unity_WorldTransformParams.w;\n",
               info->tangent_input_reg);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb,
              "float3 dxbc_surface_world_binormal = "
              "cross(dxbc_surface_world_normal, dxbc_surface_world_tangent) "
              "* dxbc_surface_tangent_sign;\n");
    for (int component = 0; component < 3; component++) {
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb,
                   "o%d.xyz = float3(dxbc_surface_world_tangent.%c, "
                   "dxbc_surface_world_binormal.%c, "
                   "dxbc_surface_world_normal.%c);\n",
                   info->tangent_output_regs[component], "xyz"[component],
                   "xyz"[component], "xyz"[component]);
    }
    for (int component = 0; component < 3; component++) {
        sb_append_spaces(ctx->sb, ctx->indent);
        sb_appendf(ctx->sb, "o%d.w = dxbc_surface_world_position.%c;\n",
                   info->tangent_output_regs[component], "xyz"[component]);
    }
}

static void emit_face_basis(HLSLEmitterContext *ctx,
                            const HLSLSemanticLift *lift) {
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb, "int dxbc_face_index = (int)_faceIndex;\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb,
              "float3 dxbc_transform_u = dxbc_face_u[dxbc_face_index];\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb,
              "float3 dxbc_transform_v = dxbc_face_v[dxbc_face_index];\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb,
              "float3 dxbc_face_normal = cross(dxbc_transform_v, "
              "dxbc_transform_u);\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float2 dxbc_face_uv = v%d.xy * _MainTex_ST.xy + "
               "_MainTex_ST.zw;\n",
               lift->data.face_basis.uv_input_register);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb, "dxbc_face_uv = dxbc_face_uv * 2.0f - 1.0f;\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "o%d.xyz = dxbc_face_normal + dxbc_face_uv.x * "
               "dxbc_transform_u + dxbc_face_uv.y * dxbc_transform_v;\n",
               lift->data.face_basis.output_register);
}

static void emit_shadow_position(HLSLEmitterContext *ctx,
                                 const HLSLSemanticLift *lift) {
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb, "o%d.xyz = mul(%s, v%d).xyz;\n",
               lift->data.shadow_position.output_register,
               lift->data.shadow_position.matrix,
               lift->data.shadow_position.input_register);
}

static void emit_speedtree_wind(HLSLEmitterContext *ctx,
                                const HLSLSemanticLift *lift) {
    int index = lift->data.speedtree_wind.start;
    const USILProgram *program = ctx->program;
    const USILInstruction *inst = &program->instructions[index];
    const USILInstruction *move_y = &program->instructions[index + 1];
    const USILInstruction *wind_enabled = &program->instructions[index + 8];
    const USILInstruction *select = &program->instructions[index + 10];
    char wind[128];
    char amount[128];
    char original_xz[128];
    char original_y[128];
    char enabled_left[128];
    char enabled_right[128];
    char destination[128];
    format_operand_hlsl(ctx, &inst->operands[1], false, false, 16 | 64,
                        false, wind, sizeof(wind));
    format_operand_hlsl(ctx, &inst->operands[2], false, false, 16, false,
                        amount, sizeof(amount));
    format_operand_hlsl(ctx, &inst->operands[3], false, false, 16 | 64,
                        false, original_xz, sizeof(original_xz));
    format_operand_hlsl(ctx, &move_y->operands[1], false, false, 16, false,
                        original_y, sizeof(original_y));
    format_operand_hlsl(ctx, &wind_enabled->operands[1], false, false, 16,
                        false, enabled_left, sizeof(enabled_left));
    format_operand_hlsl(ctx, &wind_enabled->operands[2], false, false, 16,
                        false, enabled_right, sizeof(enabled_right));
    format_dest_operand_hlsl(ctx, &select->operands[0], false, false,
                             16 | 32 | 64, false, destination,
                             sizeof(destination));
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "float3 dxbc_global_wind_original = float3(%s.x, %s, %s.y);\n",
               original_xz, original_y, original_xz);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb,
              "float dxbc_global_wind_length = "
              "length(dxbc_global_wind_original);\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb,
              "float3 dxbc_global_wind_position = "
              "dxbc_global_wind_original;\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb, "dxbc_global_wind_position.xz += %s * %s;\n", wind,
               amount);
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb,
              "dxbc_global_wind_position = "
              "normalize(dxbc_global_wind_position) * "
              "dxbc_global_wind_length;\n");
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb,
               "%s = ((%s * %s) > 0.0f) ? dxbc_global_wind_position : "
               "dxbc_global_wind_original;\n",
               destination, enabled_right, enabled_left);
}

static void emit_alpha_clip_after(HLSLEmitterContext *ctx,
                                  const HLSLSemanticLift *lift) {
    const USILInstruction *mad = &ctx->program->instructions[
        lift->data.alpha_clip.mad];
    const USILInstruction *multiply = &ctx->program->instructions[
        lift->data.alpha_clip.multiply];
    int alpha_temp_operand =
        mad->operands[1].type == OPERAND_TYPE_TEMP ? 1 : 2;
    char destination[128];
    char alpha[128];
    char offset[128];
    format_dest_operand_hlsl(ctx, &mad->operands[0], false, false,
                             mad->operands[0].destination_mask, false,
                             destination, sizeof(destination));
    DXBCOperand post_multiply_alpha = mad->operands[alpha_temp_operand];
    post_multiply_alpha.register_index =
        multiply->operands[lift->data.alpha_clip.color_temp_operand]
            .register_index;
    format_operand_hlsl(ctx, &post_multiply_alpha, false, false,
                        mad->operands[0].destination_mask, true, alpha,
                        sizeof(alpha));
    format_operand_hlsl(ctx, &mad->operands[3], false, false,
                        mad->operands[0].destination_mask, true, offset,
                        sizeof(offset));
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_appendf(ctx->sb, "%s = %s + %s;\n", destination, alpha, offset);
    if (lift->data.alpha_clip.compare <
        lift->data.alpha_clip.multiply) {
        int saved_index = ctx->current_instruction_index;
        ctx->current_instruction_index = lift->data.alpha_clip.compare;
        hlsl_emit_instruction(
            ctx, &ctx->program->instructions[lift->data.alpha_clip.compare]);
        ctx->current_instruction_index = lift->data.alpha_clip.discard;
        hlsl_emit_instruction(
            ctx, &ctx->program->instructions[lift->data.alpha_clip.discard]);
        ctx->current_instruction_index = saved_index;
    }
}

bool emit_semantic_lift_before(HLSLEmitterContext *ctx, int instruction) {
    HLSLSemanticProgram *semantic = &ctx->semantic_program;
    if (!semantic->before_lift || instruction < 0 ||
        instruction >= ctx->program->instruction_count) return false;
    int lift_id = semantic->before_lift[instruction];
    if (lift_id < 0) return false;
    const HLSLSemanticLift *lift = &semantic->lifts[lift_id];
    switch (lift->contract->kind) {
        case HLSL_SEMANTIC_SCREEN_POSITION:
            emit_readable_screen_position(ctx);
            break;
        case HLSL_SEMANTIC_STATIC_UV_SELECTION:
            emit_static_uv(ctx, lift);
            break;
        case HLSL_SEMANTIC_SURFACE_CLIP_POSITION:
        case HLSL_SEMANTIC_SURFACE_TANGENT_FRAME:
            emit_surface(ctx, lift);
            break;
        case HLSL_SEMANTIC_INDEXED_FACE_BASIS:
            emit_face_basis(ctx, lift);
            break;
        case HLSL_SEMANTIC_SHADOW_WORLD_POSITION:
            emit_shadow_position(ctx, lift);
            break;
        case HLSL_SEMANTIC_SPEEDTREE_GLOBAL_WIND:
            emit_speedtree_wind(ctx, lift);
            break;
        case HLSL_SEMANTIC_TRUTHINESS_CONDITION:
            emit_truthiness_condition(ctx, lift);
            break;
        default:
            return false;
    }
    return true;
}

void emit_semantic_prelude(HLSLEmitterContext *ctx) {
    const HLSLSemanticProgram *semantic = &ctx->semantic_program;
    for (int lift_id = 0; lift_id < semantic->lift_count; lift_id++) {
        const HLSLSemanticLift *lift = &semantic->lifts[lift_id];
        if (lift->trigger_instruction < 0 && lift->after_instruction < 0 &&
            lift->contract->kind == HLSL_SEMANTIC_SHADOW_WORLD_POSITION) {
            emit_shadow_position(ctx, lift);
        }
    }
}

void emit_semantic_lift_after(HLSLEmitterContext *ctx, int instruction) {
    HLSLSemanticProgram *semantic = &ctx->semantic_program;
    if (!semantic->after_lift || instruction < 0 ||
        instruction >= ctx->program->instruction_count) return;
    int lift_id = semantic->after_lift[instruction];
    if (lift_id < 0) return;
    const HLSLSemanticLift *lift = &semantic->lifts[lift_id];
    if (lift->contract->kind == HLSL_SEMANTIC_DEFERRED_ALPHA_CLIP)
        emit_alpha_clip_after(ctx, lift);
}
