// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static bool operand_resolves_to(const HLSLEmitterContext *ctx,
                                const DXBCOperand *operand,
                                const char *expected_name);

bool RunAnalysisPasses(HLSLEmitterContext* ctx) {
    // 1. Core structural & metadata maps
    if (!build_cbuffer_register_map(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                       HLSL_EMIT_PHASE_CBUFFER_BINDING_MAP,
                       HLSL_EMIT_REASON_INVALID_METADATA_SHAPE);
        return false;
    }
    if (!build_sampler_name_map(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                       HLSL_EMIT_PHASE_SAMPLER_BINDING_MAP,
                       HLSL_EMIT_REASON_SAMPLER_CONTRACT_MISMATCH);
        return false;
    }
    
    // 2. Control flow & live range analysis
    if (!analyze_live_ranges(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,
                       HLSL_EMIT_PHASE_LIVE_RANGE_ANALYSIS,
                       HLSL_EMIT_REASON_ALLOCATION_FAILED);
        return false;
    }
    
    // 3. Select register-layout behavior.  The prescans initialize their
    // state in both modes, but only READABLE is permitted to infer a virtual
    // register layout or decompose a decoded register move.  Recompile mode
    // must preserve the physical DXBC register/lane model.
    ctx->current_instruction_index = 0;

    // 4. Register simulation & layout analysis
    // ORDER SENSITIVE: Swizzle decomposition must run before scrambled register layout prescan
    PreScanSwizzleDecomposition(ctx);
    PreScanScrambledRegisters(ctx);
    if (!build_control_flow_graph(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_CONTROL_FLOW_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!compute_dominance(&ctx->cfg)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_DOMINANCE_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    /* Relative cbuffer topology is executable dataflow, not just metadata.
     * Build it only after CFG/dominance exist so every access can be proven
     * downstream of its scale and through branch-local kills and merges. */
    if (!build_cbuffer_emission_layouts(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                       HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
                       HLSL_EMIT_REASON_INVALID_PARAMETER_LAYOUT);
        return false;
    }
    if (!analyze_block_nesting(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_BLOCK_NESTING_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!build_hlsl_ssa_graph(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_SSA_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!build_component_provenance(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_PROVENANCE_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!build_hlsl_use_def_graph(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_USE_DEF_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!analyze_lane_value_types(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_VALUE_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!build_hlsl_storage_plan(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_STORAGE_PLANNING,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE &&
        !analyze_semantic_lifts(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_SEMANTIC_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE &&
        ctx->semantic_program.conflict_count != 0) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_SEMANTIC_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!build_d3dcompiler_model(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                       HLSL_EMIT_PHASE_COMPILER_MODEL_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    return true;
}

void detect_indexed_face_basis(HLSLEmitterContext *ctx) {
    static const USILOpcode opcodes[] = {
        USIL_OP_MOV, USIL_OP_MOV, USIL_OP_FTOI, USIL_OP_MUL,
        USIL_OP_MOV, USIL_OP_MOV, USIL_OP_MOV, USIL_OP_MAD,
        USIL_OP_MOV, USIL_OP_MAD, USIL_OP_MAD, USIL_OP_MOV,
        USIL_OP_MOV, USIL_OP_MAD, USIL_OP_MOV, USIL_OP_MAD};
    static const uint32_t expected_icb[] = {
        0, 0, 0xbf800000u, 0, 0, 0, 0x3f800000u, 0,
        0x3f800000u, 0, 0, 0, 0x3f800000u, 0, 0, 0,
        0x3f800000u, 0, 0, 0xbf800000u,
        0xbf800000u, 0, 0, 0xbf800000u,
        0, 0, 0x3f800000u, 0, 0, 0, 0xbf800000u, 0,
        0, 0, 0, 0xbf800000u, 0, 0, 0, 0xbf800000u};
    const int count = (int)(sizeof(opcodes) / sizeof(opcodes[0]));
    const USILProgram *program = ctx->program;
    memset(&ctx->indexed_face_basis, 0, sizeof(ctx->indexed_face_basis));
    if (program->icb_value_count !=
        (int)(sizeof(expected_icb) / sizeof(expected_icb[0])) ||
        memcmp(program->icb_values, expected_icb, sizeof(expected_icb)) != 0) {
        return;
    }
    for (int start = 0; start + count <= program->instruction_count; start++) {
        bool matches = true;
        for (int offset = 0; offset < count; offset++) {
            if (program->instructions[start + offset].opcode !=
                opcodes[offset]) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        const USILInstruction *index = &program->instructions[start + 2];
        const USILInstruction *uv = &program->instructions[start + 9];
        const USILInstruction *output = &program->instructions[start + 15];
        if (index->operand_count < 2 || uv->operand_count < 4 ||
            output->operand_count < 4 ||
            !operand_resolves_to(ctx, &index->operands[1], "_faceIndex") ||
            uv->operands[1].type != OPERAND_TYPE_INPUT ||
            !operand_resolves_to(ctx, &uv->operands[2], "_MainTex_ST") ||
            output->operands[0].type != OPERAND_TYPE_OUTPUT) {
            continue;
        }
        ctx->indexed_face_basis.valid = true;
        ctx->indexed_face_basis.start = start;
        ctx->indexed_face_basis.end = start + count - 1;
        ctx->indexed_face_basis.uv_input_reg = uv->operands[1].register_index;
        ctx->indexed_face_basis.output_reg =
            output->operands[0].register_index;
        return;
    }
}

static bool operands_identical(const DXBCOperand *left,
                               const DXBCOperand *right) {
    if (left->type != right->type ||
        left->raw_token != right->raw_token ||
        left->register_index != right->register_index ||
        left->swizzle_mode != right->swizzle_mode ||
        left->has_neg != right->has_neg || left->has_abs != right->has_abs ||
        left->imm_value_count != right->imm_value_count ||
        left->immediate_word_count != right->immediate_word_count ||
        left->min_precision != right->min_precision ||
        left->extended_token_count != right->extended_token_count ||
        left->register_index_dim != right->register_index_dim ||
        left->rel_offset0 != right->rel_offset0 ||
        left->rel_offset1 != right->rel_offset1 ||
        left->rel_offset2 != right->rel_offset2 || left->rel_op0 ||
        right->rel_op0 || left->rel_op1 || right->rel_op1 || left->rel_op2 ||
        right->rel_op2) {
        return false;
    }
    for (int component = 0; component < 4; component++) {
        if (left->swizzle[component] != right->swizzle[component] ||
            left->imm_values[component] != right->imm_values[component] ||
            left->imm64_values[component] != right->imm64_values[component]) {
            return false;
        }
    }
    for (int word = 0; word < 8; word++) {
        if (left->immediate_words[word] != right->immediate_words[word])
            return false;
    }
    for (int dimension = 0; dimension < 3; dimension++) {
        if (left->index_values[dimension] != right->index_values[dimension] ||
            left->index_representations[dimension] !=
                right->index_representations[dimension] ||
            left->index_has_immediate[dimension] !=
                right->index_has_immediate[dimension] ||
            left->index_value_exceeds_int[dimension] !=
                right->index_value_exceeds_int[dimension]) {
            return false;
        }
    }
    if (left->extended_token_count > 0) {
        if (!left->extended_tokens || !right->extended_tokens ||
            memcmp(left->extended_tokens, right->extended_tokens,
                   left->extended_token_count *
                       sizeof(*left->extended_tokens)) != 0) {
            return false;
        }
    }
    return true;
}

static bool is_loop_invariant_multiply_operand(const DXBCOperand *operand) {
    return operand->type == OPERAND_TYPE_CONSTANT_BUFFER ||
           operand->type == OPERAND_TYPE_IMMEDIATE32 ||
           operand->type == OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER ||
           operand->type == OPERAND_TYPE_INPUT;
}

static bool is_fixed_cbuffer_multiply_operand(const DXBCOperand *operand) {
    if (!operand || operand->type != OPERAND_TYPE_CONSTANT_BUFFER ||
        operand->register_index_dim != 2 || operand->rel_op0 ||
        operand->rel_op1 || operand->rel_op2) {
        return false;
    }
    for (int dimension = 0; dimension < 2; dimension++) {
        if (!operand->index_has_immediate[dimension] ||
            operand->index_value_exceeds_int[dimension] ||
            operand->index_representations[dimension] > 1) {
            return false;
        }
    }
    return true;
}

static bool saved_multiply_destination_is_emittable(
    HLSLEmitterContext *ctx, const USILInstruction *instruction) {
    if (!ctx || !instruction || instruction->operand_count < 1 ||
        instruction->operands[0].type != OPERAND_TYPE_TEMP ||
        instruction->operands[0].register_index < 0 ||
        instruction->operands[0].register_index >= ctx->temp_state_count ||
        instruction->operands[0].destination_mask == 0 ||
        is_mask_non_contiguous(
            instruction->operands[0].destination_mask)) {
        return false;
    }

    const int reg = instruction->operands[0].register_index;
    if (is_register_decomposed(ctx, reg)) return false;
    for (int lane = 0; lane < 4; lane++) {
        if ((instruction->operands[0].destination_mask & (16 << lane)) == 0)
            continue;
        if (ctx->has_deferred_float[reg][lane] ||
            ctx->has_write_redirect[reg][lane]) {
            return false;
        }
    }
    return true;
}

static bool mad_accumulates_its_destination(const HLSLEmitterContext *ctx,
                                            int mul_index, int mad_index) {
    const USILInstruction *mul = &ctx->program->instructions[mul_index];
    const USILInstruction *mad = &ctx->program->instructions[mad_index];
    const DXBCOperand *destination = &mad->operands[0];
    const DXBCOperand *accumulator = &mad->operands[3];
    if (destination->type != OPERAND_TYPE_TEMP ||
        accumulator->type != OPERAND_TYPE_TEMP ||
        destination->register_index != mul->operands[0].register_index ||
        accumulator->register_index != destination->register_index ||
        destination->destination_mask !=
            mul->operands[0].destination_mask ||
        accumulator->has_neg || accumulator->has_abs ||
        accumulator->rel_op0 || accumulator->rel_op1 ||
        accumulator->rel_op2) {
        return false;
    }

    for (int lane = 0; lane < 4; lane++) {
        if ((destination->destination_mask & (16 << lane)) == 0) continue;
        if (usil_operand_source_component(accumulator, lane) != lane) return false;
        const int definition =
            hlsl_operand_definition(ctx, mad_index, 3, lane);
        if (definition < mul_index || definition >= mad_index ||
            !instructions_have_unambiguous_path(ctx, definition, mad_index)) {
            return false;
        }
        const USILInstruction *definition_instruction =
            &ctx->program->instructions[definition];
        if (definition_instruction->operand_count < 1 ||
            definition_instruction->operands[0].type != OPERAND_TYPE_TEMP ||
            definition_instruction->operands[0].register_index !=
                destination->register_index ||
            (definition_instruction->operands[0].destination_mask &
             (16 << lane)) == 0) {
            return false;
        }
    }
    return true;
}

static bool multiply_sources_match(const USILInstruction *mul,
                                   const USILInstruction *candidate,
                                   bool *same_order, bool *reverse_order) {
    if (!mul || !candidate || candidate->operand_count < 3) return false;
    *same_order =
        operands_identical(&mul->operands[1], &candidate->operands[1]) &&
        operands_identical(&mul->operands[2], &candidate->operands[2]);
    *reverse_order =
        operands_identical(&mul->operands[1], &candidate->operands[2]) &&
        operands_identical(&mul->operands[2], &candidate->operands[1]);
    return *same_order || *reverse_order;
}

/* D3DCompiler preserves the decoded reverse order for a recurrent MAD family
 * when the shared product is named once in reverse source order.  Certify only
 * the observed, dataflow-complete form: two fixed-address cbuffer operands,
 * an unsplit float destination, and at least two straight-line MAD updates of
 * that same destination.  Any competing product spelling rejects the entire
 * family instead of guessing which source expression the compiler recovered. */
static bool detect_exact_recurrent_multiply(HLSLEmitterContext *ctx,
                                            int mul_index) {
    const USILProgram *program = ctx->program;
    const USILInstruction *mul = &program->instructions[mul_index];
    if (ctx->use_uint_temps || mul->opcode != USIL_OP_MUL ||
        mul->operand_count < 3 || mul->saturate ||
        !saved_multiply_destination_is_emittable(ctx, mul) ||
        mul->operands[1].has_neg || mul->operands[1].has_abs ||
        mul->operands[2].has_neg || mul->operands[2].has_abs ||
        !is_fixed_cbuffer_multiply_operand(&mul->operands[1]) ||
        !is_fixed_cbuffer_multiply_operand(&mul->operands[2]) ||
        operands_identical(&mul->operands[1], &mul->operands[2])) {
        return false;
    }

    int reuse_count = 0;
    for (int index = mul_index + 1; index < program->instruction_count;
         index++) {
        const USILInstruction *candidate = &program->instructions[index];
        if ((candidate->opcode != USIL_OP_MAD &&
             candidate->opcode != USIL_OP_MUL) ||
            candidate->operand_count < 3) {
            continue;
        }
        bool same_order = false;
        bool reverse_order = false;
        if (!multiply_sources_match(mul, candidate, &same_order,
                                    &reverse_order)) {
            continue;
        }

        if (candidate->opcode != USIL_OP_MAD || same_order ||
            !reverse_order || candidate->operand_count < 4 ||
            candidate->saturate || ctx->saved_mul_id[index] != 0 ||
            !saved_multiply_destination_is_emittable(ctx, candidate) ||
            !instructions_have_unambiguous_path(ctx, mul_index, index) ||
            !mad_accumulates_its_destination(ctx, mul_index, index)) {
            return false;
        }
        reuse_count++;
    }
    if (reuse_count < 2) return false;

    const int saved_id = ++ctx->saved_mul_count;
    ctx->saved_mul_id[mul_index] = saved_id;
    ctx->saved_mul_is_definition[mul_index] = true;
    ctx->saved_mul_reverse_definition[mul_index] = true;
    for (int index = mul_index + 1; index < program->instruction_count;
         index++) {
        const USILInstruction *candidate = &program->instructions[index];
        bool same_order = false;
        bool reverse_order = false;
        if (candidate->opcode == USIL_OP_MAD &&
            multiply_sources_match(mul, candidate, &same_order,
                                   &reverse_order) &&
            !same_order && reverse_order) {
            ctx->saved_mul_id[index] = saved_id;
        }
    }
    return true;
}

static void detect_readable_reused_multiply(HLSLEmitterContext *ctx,
                                            int mul_index) {
    const USILProgram *program = ctx->program;
    const USILInstruction *mul = &program->instructions[mul_index];
    if (ctx->use_uint_temps || mul->opcode != USIL_OP_MUL ||
        mul->operand_count < 3 || mul->saturate ||
        !saved_multiply_destination_is_emittable(ctx, mul) ||
        mul->operands[1].has_neg || mul->operands[1].has_abs ||
        mul->operands[2].has_neg || mul->operands[2].has_abs ||
        !is_loop_invariant_multiply_operand(&mul->operands[1]) ||
        !is_loop_invariant_multiply_operand(&mul->operands[2])) {
        return;
    }

    int saved_id = 0;
    for (int mad_index = mul_index + 1;
         mad_index < program->instruction_count; mad_index++) {
        const USILInstruction *mad = &program->instructions[mad_index];
        if (ctx->saved_mul_id[mad_index] != 0 ||
            mad->opcode != USIL_OP_MAD || mad->operand_count < 4 ||
            mad->saturate || mad->operands[1].has_neg ||
            mad->operands[1].has_abs || mad->operands[2].has_neg ||
            mad->operands[2].has_abs ||
            mad->operands[0].destination_mask !=
                mul->operands[0].destination_mask ||
            !saved_multiply_destination_is_emittable(ctx, mad) ||
            !instructions_have_unambiguous_path(ctx, mul_index, mad_index)) {
            continue;
        }
        bool same_order = false;
        bool reverse_order = false;
        if (!multiply_sources_match(mul, mad, &same_order, &reverse_order))
            continue;

        if (saved_id == 0) {
            saved_id = ++ctx->saved_mul_count;
            ctx->saved_mul_id[mul_index] = saved_id;
            ctx->saved_mul_is_definition[mul_index] = true;
            ctx->saved_mul_reverse_definition[mul_index] = reverse_order;
        }
        ctx->saved_mul_id[mad_index] = saved_id;
    }
}

void detect_reused_multiply_expressions(HLSLEmitterContext *ctx) {
    const USILProgram *program = ctx->program;
    ctx->saved_mul_count = 0;
    if (program->instruction_count > 0) {
        memset(ctx->saved_mul_id, 0,
               (size_t)program->instruction_count *
                   sizeof(*ctx->saved_mul_id));
        memset(ctx->saved_mul_is_definition, 0,
               (size_t)program->instruction_count *
                   sizeof(*ctx->saved_mul_is_definition));
        memset(ctx->saved_mul_reverse_definition, 0,
               (size_t)program->instruction_count *
                   sizeof(*ctx->saved_mul_reverse_definition));
    }
    for (int mul_index = 0; mul_index < program->instruction_count;
         mul_index++) {
        if (ctx->saved_mul_id[mul_index] != 0) continue;
        if (ctx->emit_mode == HLSL_EMIT_MODE_RECOMPILE)
            (void)detect_exact_recurrent_multiply(ctx, mul_index);
        else
            detect_readable_reused_multiply(ctx, mul_index);
    }
}

static bool instruction_has_opcode(const USILProgram *program, int index,
                                   USILOpcode opcode) {
    return index >= 0 && index < program->instruction_count &&
           program->instructions[index].opcode == opcode;
}

static bool operand_resolves_to(const HLSLEmitterContext *ctx,
                                const DXBCOperand *operand,
                                const char *expected_name) {
    if (operand->type != OPERAND_TYPE_CONSTANT_BUFFER) return false;
    int component_offset = 0;
    const char *name = resolve_cb_variable_ctx(
        ctx, operand->register_index, operand->rel_offset0, 0,
        &component_offset);
    return name && strcmp(name, expected_name) == 0;
}

void detect_surface_tangent_frame(HLSLEmitterContext *ctx) {
    static const USILOpcode tangent_opcodes[] = {
        USIL_OP_DP3, USIL_OP_DP3, USIL_OP_DP3, USIL_OP_DP3, USIL_OP_RSQ,
        USIL_OP_MUL, USIL_OP_MUL, USIL_OP_MAD, USIL_OP_MAD, USIL_OP_DP3,
        USIL_OP_RSQ, USIL_OP_MUL, USIL_OP_MUL, USIL_OP_MAD, USIL_OP_MUL,
        USIL_OP_MUL, USIL_OP_MOV, USIL_OP_MOV, USIL_OP_MOV, USIL_OP_MOV,
        USIL_OP_MOV, USIL_OP_MOV, USIL_OP_MOV, USIL_OP_MOV, USIL_OP_MOV,
        USIL_OP_MOV, USIL_OP_MOV};
    static const USILOpcode transform_opcodes[] = {
        USIL_OP_MUL, USIL_OP_MAD, USIL_OP_MAD, USIL_OP_ADD, USIL_OP_MAD,
        USIL_OP_MUL, USIL_OP_MAD, USIL_OP_MAD, USIL_OP_MAD};
    const int tangent_count =
        (int)(sizeof(tangent_opcodes) / sizeof(tangent_opcodes[0]));
    const int transform_count =
        (int)(sizeof(transform_opcodes) / sizeof(transform_opcodes[0]));
    const USILProgram *program = ctx->program;
    memset(&ctx->surface_tangent_frame, 0,
           sizeof(ctx->surface_tangent_frame));

    for (int tangent_start = 1;
         tangent_start + tangent_count <= program->instruction_count;
         tangent_start++) {
        bool matches = true;
        for (int offset = 0; offset < tangent_count; offset++) {
            if (!instruction_has_opcode(program, tangent_start + offset,
                                        tangent_opcodes[offset])) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;

        const USILInstruction *normal0 =
            &program->instructions[tangent_start];
        const USILInstruction *tangent_mul =
            &program->instructions[tangent_start + 6];
        const USILInstruction *sign_mul =
            &program->instructions[tangent_start + 14];
        if (normal0->operand_count < 3 || tangent_mul->operand_count < 3 ||
            sign_mul->operand_count < 3 ||
            normal0->operands[1].type != OPERAND_TYPE_INPUT ||
            tangent_mul->operands[1].type != OPERAND_TYPE_INPUT ||
            sign_mul->operands[1].type != OPERAND_TYPE_INPUT ||
            !operand_resolves_to(ctx, &normal0->operands[2],
                                 "unity_WorldToObject") ||
            !operand_resolves_to(ctx, &tangent_mul->operands[2],
                                 "unity_ObjectToWorld") ||
            !operand_resolves_to(ctx, &sign_mul->operands[2],
                                 "unity_WorldTransformParams")) {
            continue;
        }

        int transform_start = -1;
        int search_begin = tangent_start - 32;
        if (search_begin < 0) search_begin = 0;
        for (int candidate = search_begin;
             candidate + transform_count <= tangent_start; candidate++) {
            bool transform_matches = true;
            for (int offset = 0; offset < transform_count; offset++) {
                if (!instruction_has_opcode(program, candidate + offset,
                                            transform_opcodes[offset])) {
                    transform_matches = false;
                    break;
                }
            }
            if (!transform_matches) continue;
            const USILInstruction *object_mul =
                &program->instructions[candidate];
            const USILInstruction *clip_mul =
                &program->instructions[candidate + 5];
            if (object_mul->operand_count >= 3 &&
                clip_mul->operand_count >= 3 &&
                object_mul->operands[1].type == OPERAND_TYPE_INPUT &&
                operand_resolves_to(ctx, &object_mul->operands[2],
                                    "unity_ObjectToWorld") &&
                operand_resolves_to(ctx, &clip_mul->operands[2],
                                    "unity_MatrixVP")) {
                transform_start = candidate;
                break;
            }
        }
        if (transform_start < 0) continue;

        const USILInstruction *world_x =
            &program->instructions[tangent_start - 1];
        if (world_x->opcode != USIL_OP_MOV || world_x->operand_count < 2 ||
            world_x->operands[0].type != OPERAND_TYPE_OUTPUT) {
            continue;
        }

        SurfaceTangentFrameInfo *info = &ctx->surface_tangent_frame;
        info->valid = true;
        info->transform_start = transform_start;
        info->tangent_start = tangent_start;
        info->tangent_end = tangent_start + tangent_count - 1;
        info->clip_temp_reg = -1;
        info->position_input_reg =
            program->instructions[transform_start].operands[1].register_index;
        info->normal_input_reg = normal0->operands[1].register_index;
        info->tangent_input_reg = tangent_mul->operands[1].register_index;
        const DXBCOperand *clip_transform_dest =
            &program->instructions[transform_start + 8].operands[0];
        if (clip_transform_dest->type == OPERAND_TYPE_OUTPUT) {
            info->clip_output_reg = clip_transform_dest->register_index;
        } else if (clip_transform_dest->type == OPERAND_TYPE_TEMP &&
                   transform_start + 9 < tangent_start) {
            const USILInstruction *clip_move =
                &program->instructions[transform_start + 9];
            if (clip_move->opcode != USIL_OP_MOV ||
                clip_move->operand_count < 2 ||
                clip_move->operands[0].type != OPERAND_TYPE_OUTPUT ||
                clip_move->operands[1].type != OPERAND_TYPE_TEMP ||
                clip_move->operands[1].register_index !=
                    clip_transform_dest->register_index) {
                memset(info, 0, sizeof(*info));
                continue;
            }
            info->clip_output_reg = clip_move->operands[0].register_index;
            info->clip_temp_reg = clip_transform_dest->register_index;
        } else {
            memset(info, 0, sizeof(*info));
            continue;
        }
        info->tangent_output_regs[0] = world_x->operands[0].register_index;
        info->tangent_output_regs[1] =
            program->instructions[tangent_start + 19].operands[0].register_index;
        info->tangent_output_regs[2] =
            program->instructions[tangent_start + 20].operands[0].register_index;

        return;
    }
}


