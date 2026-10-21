// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include "translation/hlsl_source_identifier.h"

#include <string.h>

/* Closed decoded forms used by the one-sample fragment proof. The unused
 * coordinate padding is retained in the owner digest, never made into a
 * fourth logical coordinate. */
static bool sample_register(const DXBCOperand *operand, DXBCOperandType type,
    uint32_t token, const uint8_t selectors[4]) {
    if (!hlsl_lift_operand_is_plain(operand) || operand->extended_tokens || operand->type != type ||
        operand->raw_token != token || operand->register_index_dim != 1 || operand->register_index < 0 ||
        operand->swizzle_mode != ((token >> 2u) & 3u) ||
        operand->destination_mask != ((token & 12u) ? 0 : (token & 0xf0u)) ||
        operand->rel_offset0 || operand->rel_offset2 || operand->rel_offset1 != operand->register_index ||
        operand->imm_value_count || operand->immediate_word_count ||
        !operand->index_has_immediate[0] || operand->index_representations[0] ||
        operand->index_value_exceeds_int[0] || operand->index_values[0] != (uint32_t)operand->register_index)
        return false;
    for (unsigned index = 1; index < 3; ++index)
        if (operand->index_has_immediate[index] || operand->index_representations[index] ||
            operand->index_value_exceeds_int[index] || operand->index_values[index]) return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (operand->swizzle[lane] != selectors[lane] || operand->imm_values[lane] || operand->imm64_values[lane])
            return false;
    for (unsigned word = 0; word < 8; ++word)
        if (operand->immediate_words[word]) return false;
    return true;
}

static bool sample_level(const DXBCOperand *operand) {
    if (!hlsl_lift_operand_is_plain(operand) || operand->extended_tokens ||
        operand->type != OPERAND_TYPE_IMMEDIATE32 || operand->raw_token != UINT32_C(0x00004001) ||
        operand->swizzle_mode || operand->destination_mask || operand->register_index_dim || operand->register_index ||
        operand->rel_offset0 || operand->rel_offset1 || operand->rel_offset2 ||
        operand->imm_value_count != 1 || operand->immediate_word_count != 1 ||
        operand->imm_values[0] != operand->immediate_words[0]) return false;
    for (unsigned index = 0; index < 3; ++index)
        if (operand->index_has_immediate[index] || operand->index_representations[index] ||
            operand->index_value_exceeds_int[index] || operand->index_values[index]) return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (operand->swizzle[lane] != lane || operand->imm64_values[lane] ||
            operand->imm_values[lane] != operand->immediate_words[0]) return false;
    for (unsigned word = 1; word < 8; ++word)
        if (operand->immediate_words[word]) return false;
    return true;
}

/* The layout belongs to the actual sample and released signatures. Result
 * consumers have separate complete instruction-graph proofs below. */
static bool sample_layout_prepare(const USILProgram *program, HLSLSampledFragmentPlan *plan) {
    if (!plan) return false;
    memset(plan, 0, sizeof(*plan));
    if (!hlsl_decoded_graphics_owner_storage_valid(program, HLSL_GRAPHICS_OWNER_SAMPLED_FRAGMENT) ||
        program->program_type != DXBC_PROGRAM_TYPE_PIXEL || program->shader_model_minor ||
        !program->input_count || program->output_count != 1 ||
        (program->has_global_flags && program->global_flags != 1u)) return false;
    const USILTexture *texture = &program->textures[0];
    const USILSampler *sampler = &program->samplers[0];
    HLSLSampledTextureKind kind;
    if (!strcmp(texture->dimension, "2d")) kind = HLSL_SAMPLED_TEXTURE_2D;
    else if (!strcmp(texture->dimension, "cube")) kind = HLSL_SAMPLED_TEXTURE_CUBE;
    else if (!strcmp(texture->dimension, "2darray")) kind = HLSL_SAMPLED_TEXTURE_2D_ARRAY;
    else return false;
    const bool three_coordinates = kind != HLSL_SAMPLED_TEXTURE_2D;
    if (texture->stride || texture->sample_count ||
        texture->reg_idx < 0 || texture->reg_idx >= HLSL_SM5_RESOURCE_REGISTER_COUNT ||
        sampler->reg_idx < 0 || sampler->reg_idx >= HLSL_SM5_SAMPLER_REGISTER_COUNT) return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (texture->return_types[lane] != 5) return false;
    const DXBCSignatureElement *output = &program->outputs[0];
    if (output->register_id || output->component_type != 3 || output->mask != 15 || output->rw_mask ||
        output->system_value || output->semantic_index || output->stream_index || output->min_precision ||
        output->interpolation_mode || dxbc_ascii_strcasecmp(dxbc_signature_semantic_name(output), "SV_Target"))
        return false;
    const USILInstruction *sample = &program->instructions[0];
    const bool level = sample->opcode == USIL_OP_SAMPLE_L;
    if ((!level && sample->opcode != USIL_OP_SAMPLE) || sample->operand_count != (level ? 5 : 4)) return false;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        if (instruction->saturate || instruction->precise_mask || instruction->condition_test ||
            instruction->has_texel_offset || instruction->resource_stride || instruction->geometry_effect || instruction->geometry_stream_id ||
            instruction->geometry_stream_explicit || instruction->sync_flags ||
            (index ? instruction->resource_dimension[0] != 0 :
                strcmp(instruction->resource_dimension, texture->dimension) != 0)) return false;
    }
    static const uint8_t identity[4] = {0, 1, 2, 3}, xy[4] = {0, 1, 0, 0},
        xyz[4] = {0, 1, 2, 0}, zw[4] = {2, 3, 2, 2};
    const bool tail_coordinates = kind == HLSL_SAMPLED_TEXTURE_2D &&
        sample->operands[1].raw_token == UINT32_C(0x00101ae6);
    if (!sample_register(&sample->operands[1], OPERAND_TYPE_INPUT,
            three_coordinates ? UINT32_C(0x00101246) : tail_coordinates ? UINT32_C(0x00101ae6) : UINT32_C(0x00101046),
            three_coordinates ? xyz : tail_coordinates ? zw : xy) ||
        sample->operands[1].register_index >= HLSL_SM5_IO_REGISTER_COUNT ||
        !sample_register(&sample->operands[2], OPERAND_TYPE_RESOURCE, UINT32_C(0x00107e46), identity) ||
        sample->operands[2].register_index != texture->reg_idx ||
        !sample_register(&sample->operands[3], OPERAND_TYPE_SAMPLER, UINT32_C(0x00106000), identity) ||
        sample->operands[3].register_index != sampler->reg_idx || (level && !sample_level(&sample->operands[4])))
        return false;
    const uint8_t coordinate_mask = three_coordinates ? 7 : tail_coordinates ? 12 : 3;
    int coordinate = -1;
    bool inactive_typed_inputs = false;
    uint8_t occupied[HLSL_SM5_IO_REGISTER_COUNT] = {0};
    unsigned register_fields[HLSL_SM5_IO_REGISTER_COUNT] = {0};
    uint32_t registers = 0;
    for (int index = 0; index < program->input_count; ++index) {
        const DXBCSignatureElement *field = &program->inputs[index];
        const char *semantic = dxbc_signature_semantic_name(field);
        if (field->component_type < 1 || field->component_type > 3 ||
            !field->mask || field->mask > 15 ||
            field->min_precision || field->stream_index || field->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
            (occupied[field->register_id] & field->mask) || ++register_fields[field->register_id] > 2u) return false;
        if (!hlsl_inactive_input_semantic_supported(field)) return false;
        for (int previous = 0; previous < index; ++previous)
            if (program->inputs[previous].semantic_index == field->semantic_index &&
                !dxbc_ascii_strcasecmp(dxbc_signature_semantic_name(&program->inputs[previous]), semantic)) return false;
        if (field->register_id == (uint32_t)sample->operands[1].register_index && field->mask == coordinate_mask) {
            if (field->system_value || field->component_type != 3 ||
                field->mask != coordinate_mask || field->rw_mask != coordinate_mask ||
                field->interpolation_mode < 2 || field->interpolation_mode > 7) return false;
            coordinate = index;
        } else {
            if (field->rw_mask || field->interpolation_mode) return false;
            inactive_typed_inputs |= field->component_type != 3;
        }
        occupied[field->register_id] |= field->mask;
        registers |= UINT32_C(1) << field->register_id;
    }
    uint32_t packed_registers = 0;
    for (int index = 0; index < program->input_count; ++index) {
        const DXBCSignatureElement *field = &program->inputs[index];
        if (register_fields[field->register_id] == 1u) {
            if (field->mask & (field->mask + 1u)) return false;
        } else {
            if (kind != HLSL_SAMPLED_TEXTURE_2D || field->component_type != 3 || field->system_value ||
                (field->mask != 3 && field->mask != 12) || occupied[field->register_id] != 15) return false;
            packed_registers |= UINT32_C(1) << field->register_id;
        }
    }
    if (packed_registers && (inactive_typed_inputs || (packed_registers & (packed_registers - 1u)) ||
        packed_registers != (UINT32_C(1) << (unsigned)sample->operands[1].register_index))) return false;
    if (coordinate < 0 || program->signature_declaration_count != 2) return false;
    bool input_seen = false, output_seen = false;
    for (int index = 0; index < 2; ++index) {
        const USILSignatureDeclaration *declaration = &program->signature_declarations[index];
        if (!declaration->has_signature_register || declaration->stream_index ||
            declaration->has_array_element_count || declaration->array_element_count ||
            declaration->has_system_value || declaration->system_value_name ||
            declaration->source_instruction_index >= sample->source_instruction_index) return false;
        if (declaration->kind == USIL_SIGNATURE_DECL_INPUT_PS && declaration->operand_type == OPERAND_TYPE_INPUT &&
            declaration->register_id == program->inputs[coordinate].register_id && declaration->mask == coordinate_mask &&
            declaration->has_interpolation && declaration->interpolation_mode == program->inputs[coordinate].interpolation_mode &&
            !input_seen) input_seen = true;
        else if (declaration->kind == USIL_SIGNATURE_DECL_OUTPUT && declaration->operand_type == OPERAND_TYPE_OUTPUT &&
            !declaration->register_id && declaration->mask == 15 && !declaration->has_interpolation &&
            !declaration->interpolation_mode && !output_seen) output_seen = true;
        else return false;
    }
    if (!input_seen || !output_seen) return false;
    *plan = (HLSLSampledFragmentPlan){.present = true, .texture_kind = kind, .explicit_level = level,
        .inactive_typed_inputs = inactive_typed_inputs, .packed_inputs = packed_registers != 0,
        .coordinate_field = coordinate, .input_registers = registers,
        .texture_register = (uint32_t)texture->reg_idx,
        .sampler_register = (uint32_t)sampler->reg_idx};
    return true;
}

static bool vector_literal(const DXBCOperand *operand, bool zero) {
    if (!hlsl_lift_operand_is_plain(operand) || operand->extended_tokens ||
        operand->type != OPERAND_TYPE_IMMEDIATE32 || operand->raw_token != UINT32_C(0x00004002) ||
        operand->register_index_dim || operand->register_index || operand->swizzle_mode || operand->destination_mask ||
        operand->rel_offset0 || operand->rel_offset1 || operand->rel_offset2 ||
        operand->imm_value_count != 4 || operand->immediate_word_count != 4) return false;
    for (unsigned index = 0; index < 3; ++index)
        if (operand->index_has_immediate[index] || operand->index_representations[index] ||
            operand->index_value_exceeds_int[index] || operand->index_values[index]) return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (operand->swizzle[lane] != lane || operand->imm64_values[lane] ||
            operand->imm_values[lane] != operand->immediate_words[lane] ||
            (zero && operand->imm_values[lane])) return false;
    for (unsigned word = 4; word < 8; ++word)
        if (operand->immediate_words[word]) return false;
    return true;
}

static bool guard_temp(const DXBCOperand *operand, unsigned reg, uint32_t token) {
    uint8_t selectors[4] = {0, 1, 2, 3};
    if (((token >> 2u) & 3u) == 2u)
        for (unsigned lane = 0; lane < 4; ++lane) selectors[lane] = (uint8_t)((token >> 4u) & 3u);
    else if (((token >> 2u) & 3u) == 1u)
        for (unsigned lane = 0; lane < 4; ++lane) selectors[lane] = (uint8_t)((token >> (4u + lane * 2u)) & 3u);
    return sample_register(operand, OPERAND_TYPE_TEMP, token, selectors) &&
        operand->register_index == (int)reg;
}

/* Canonical masks come from these actual comparisons, then OR and IEQ.
 * Arbitrary nonzero MOVC conditions and Boolean usage facts supply no proof. */
static bool sampled_nan_guard_shape(const USILProgram *program) {
    static const USILOpcode operations[12] = {USIL_OP_SAMPLE, USIL_OP_LT, USIL_OP_LT,
        USIL_OP_OR, USIL_OP_EQ, USIL_OP_OR, USIL_OP_IEQ, USIL_OP_OR, USIL_OP_OR,
        USIL_OP_OR, USIL_OP_MOVC, USIL_OP_RET};
    static const uint8_t identity[4] = {0, 1, 2, 3};
    if (program->instruction_count != 12 || program->temp_count != 3) return false;
    const USILInstruction *instructions = program->instructions;
    for (unsigned index = 0; index < 12; ++index)
        if (instructions[index].opcode != operations[index] ||
            instructions[index].operand_count != (index == 11 ? 0 : index == 0 || index == 10 ? 4 : 3) ||
            instructions[index].source_instruction_index != instructions[0].source_instruction_index + index)
            return false;
    if (!guard_temp(&instructions[0].operands[0], 0, UINT32_C(0x001000f2))) return false;
    for (unsigned index = 1; index <= 6; ++index) {
        const DXBCOperand *operands = instructions[index].operands;
        if (!guard_temp(&operands[0], index == 2 || index == 4 ? 2 : 1, UINT32_C(0x001000f2))) return false;
        if (index == 3 || index == 5) {
            if (!guard_temp(&operands[1], 1, UINT32_C(0x00100e46)) ||
                !guard_temp(&operands[2], 2, UINT32_C(0x00100e46))) return false;
        } else {
            const unsigned literal = index == 2 ? 1 : 2;
            if (!vector_literal(&operands[literal], true) ||
                !guard_temp(&operands[3 - literal], index == 6 ? 1 : 0, UINT32_C(0x00100e46))) return false;
        }
    }
    for (unsigned index = 7; index <= 9; ++index) {
        const DXBCOperand *operands = instructions[index].operands;
        if (!guard_temp(&operands[0], 1, UINT32_C(0x00100012)) ||
            !guard_temp(&operands[1], 1, UINT32_C(0x0010000a) | ((index - 6u) << 4u)) ||
            !guard_temp(&operands[2], 1, UINT32_C(0x0010000a))) return false;
    }
    const DXBCOperand *result = instructions[10].operands;
    return sample_register(&result[0], OPERAND_TYPE_OUTPUT, UINT32_C(0x001020f2), identity) &&
        !result[0].register_index && guard_temp(&result[1], 1, UINT32_C(0x00100006)) &&
        vector_literal(&result[2], false) && guard_temp(&result[3], 0, UINT32_C(0x00100e46));
}

bool hlsl_sampled_fragment_plan_prepare(const USILProgram *program, HLSLSampledFragmentPlan *plan) {
    if (!plan) return false;
    memset(plan, 0, sizeof(*plan));
    HLSLSampledFragmentPlan layout = {0};
    if (!sample_layout_prepare(program, &layout)) return false;
    static const uint8_t identity[4] = {0, 1, 2, 3};
    if (program->instruction_count == 2 && !program->temp_count &&
        program->instructions[1].opcode == USIL_OP_RET && !program->instructions[1].operand_count &&
        program->instructions[1].source_instruction_index == program->instructions[0].source_instruction_index + 1u &&
        sample_register(&program->instructions[0].operands[0], OPERAND_TYPE_OUTPUT,
            UINT32_C(0x001020f2), identity) && !program->instructions[0].operands[0].register_index) {
        *plan = layout;
        return true;
    }
    if (layout.texture_kind != HLSL_SAMPLED_TEXTURE_2D || layout.explicit_level ||
        layout.inactive_typed_inputs || !sampled_nan_guard_shape(program)) return false;
    layout.nan_guard = true;
    *plan = layout;
    return true;
}

bool hlsl_sampled_fragment_admission(const HLSLEmitterContext *ctx) {
    HLSLSampledFragmentPlan plan;
    const char *name = NULL;
    if (!ctx || !hlsl_sampled_fragment_plan_prepare(ctx->program, &plan) ||
        (plan.texture_kind == HLSL_SAMPLED_TEXTURE_2D && !plan.inactive_typed_inputs && !plan.packed_inputs && !plan.nan_guard)) return false;
    const uint32_t metadata_dimension = plan.texture_kind == HLSL_SAMPLED_TEXTURE_2D ? 2 :
        plan.texture_kind == HLSL_SAMPLED_TEXTURE_CUBE ? 4 : 5;
    const SerializedProgramParameters *parameters[2] = {ctx->params, ctx->common_params};
    for (unsigned authority = 0; authority < 2; ++authority) {
        const SerializedProgramParameters *source = parameters[authority];
        if (source && (source->cb_count < 0 || source->cb_count > 8 || source->res_count < 0 ||
            source->res_count > 128 || (source->res_count && !source->resources))) return false;
    }
    if (!resolve_srv_name_ctx(ctx, (int)plan.texture_register, SERIALIZED_RESOURCE_TEXTURE, &name) ||
        !hlsl_source_identifier_valid(name)) return false;
    bool found = false;
    for (unsigned authority = 0; authority < 2; ++authority) {
        const SerializedProgramParameters *source = parameters[authority];
        if (!source) continue;
        for (int index = 0; index < source->res_count; ++index) {
            const SerializedResourceParam *resource = &source->resources[index];
            if (resource->bind_type == SERIALIZED_RESOURCE_SAMPLER && resource->bind_index == plan.sampler_register)
                return false;
            if (resource->bind_type != SERIALIZED_RESOURCE_TEXTURE || resource->bind_index != plan.texture_register) continue;
            if (!resource->name || strcmp(resource->name, name) || resource->dimension != metadata_dimension || resource->multisampled ||
                resource->sampler_index != plan.sampler_register) return false;
            found = true;
        }
    }
    return found;
}
