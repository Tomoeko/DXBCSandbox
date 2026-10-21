// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <string.h>

/* Unused fields still belong to the released interface. Retain their types,
 * semantics and order; only the actual MOV source may supply an expression.
 * This proof deliberately grants no integer or packed-input dataflow. */
bool hlsl_inactive_input_semantic_supported(const DXBCSignatureElement *field) {
    const char *semantic = dxbc_signature_semantic_name(field);
    switch (field->system_value) {
    case 0:
        /* The compiler retains an unread vertex instance semantic in a pixel
         * signature as an undefined system value. It has no executable input
         * declaration here; do not reinterpret it as an active system value. */
        return hlsl_custom_zero_index_semantic_supported(semantic) ||
            (!field->semantic_index && field->component_type == 1 && field->mask == 1 && !field->rw_mask &&
             !dxbc_ascii_strcasecmp(semantic, "SV_InstanceID"));
    case 1:
        return !field->semantic_index && field->component_type == 3 && field->mask == 15 &&
            !dxbc_ascii_strcasecmp(semantic, "SV_Position");
    case 4:
        return !field->semantic_index && field->component_type == 1 && field->mask == 1 &&
            !dxbc_ascii_strcasecmp(semantic, "SV_RenderTargetArrayIndex");
    case 9:
        return !field->semantic_index && field->component_type == 1 && field->mask == 1 &&
            !dxbc_ascii_strcasecmp(semantic, "SV_IsFrontFace");
    default:
        return false;
    }
}

static bool plain_operand(const DXBCOperand *operand) {
    if (operand->has_neg || operand->has_abs || operand->min_precision ||
        operand->extended_token_count || operand->extended_tokens ||
        operand->rel_op0 || operand->rel_op1 || operand->rel_op2 || operand->rel_offset0 || operand->rel_offset2) return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (operand->imm64_values[lane] || operand->swizzle[lane] != lane) return false;
    for (unsigned index = 0; index < 3; ++index)
        if (operand->index_representations[index] || operand->index_value_exceeds_int[index] ||
            (index >= (unsigned)operand->register_index_dim &&
             (operand->index_has_immediate[index] || operand->index_values[index]))) return false;
    return true;
}

static bool register_operand(const DXBCOperand *operand, DXBCOperandType type) {
    return plain_operand(operand) && operand->type == type && operand->register_index_dim == 1 &&
        operand->swizzle_mode == (type == OPERAND_TYPE_INPUT ? 1 : 0) &&
        operand->destination_mask == (type == OPERAND_TYPE_INPUT ? 0 : 0xf0) &&
        operand->register_index >= 0 && operand->register_index < HLSL_SM5_IO_REGISTER_COUNT &&
        operand->rel_offset1 == operand->register_index &&
        operand->index_has_immediate[0] && operand->index_values[0] == (uint32_t)operand->register_index &&
        !operand->imm_value_count && !operand->immediate_word_count;
}

static bool plain_instruction(const USILInstruction *instruction) {
    return !instruction->saturate && !instruction->precise_mask && !instruction->condition_test &&
        !instruction->has_resource_dimension && !instruction->resource_stride &&
        !instruction->has_texel_offset && !instruction->has_resource_return_types &&
        !instruction->geometry_effect && !instruction->geometry_stream_id &&
        !instruction->geometry_stream_explicit && !instruction->sync_flags;
}

bool hlsl_inactive_input_plan_prepare(const USILProgram *program, HLSLInactiveInputPlan *plan) {
    if (!plan) return false;
    memset(plan, 0, sizeof(*plan));
    if (!hlsl_decoded_graphics_owner_storage_valid(program, HLSL_GRAPHICS_OWNER_PURE) ||
        program->program_type != DXBC_PROGRAM_TYPE_PIXEL || program->shader_model_minor ||
        program->instruction_count != 2 || program->temp_count ||
        program->input_count < 1 || program->output_count != 1 ||
        (program->has_global_flags && program->global_flags != 1u)) return false;
    const DXBCSignatureElement *output = &program->outputs[0];
    if (output->register_id || output->semantic_index || output->system_value ||
        output->component_type != 3 || output->mask != 15 || output->rw_mask ||
        output->stream_index || output->min_precision || output->interpolation_mode ||
        dxbc_ascii_strcasecmp(dxbc_signature_semantic_name(output), "SV_Target")) return false;
    const USILInstruction *move = &program->instructions[0], *ret = &program->instructions[1];
    if (move->opcode != USIL_OP_MOV || move->operand_count != 2 || ret->opcode != USIL_OP_RET ||
        ret->operand_count || !plain_instruction(move) || !plain_instruction(ret) ||
        ret->source_instruction_index != move->source_instruction_index + 1u) return false;
    const DXBCOperand *destination = &move->operands[0], *source = &move->operands[1];
    if (!register_operand(destination, OPERAND_TYPE_OUTPUT) || destination->register_index ||
        destination->raw_token != UINT32_C(0x001020f2) ||
        usil_operand_destination_lane_mask(destination) != 15) return false;
    int active = -1;
    if (source->type == OPERAND_TYPE_INPUT) {
        if (!register_operand(source, OPERAND_TYPE_INPUT) || source->raw_token != UINT32_C(0x00101e46))
            return false;
        for (int lane = 0; lane < 4; ++lane)
            if (usil_operand_source_component(source, lane) != lane) return false;
        for (int field = 0; field < program->input_count; ++field)
            if (program->inputs[field].register_id == (uint32_t)source->register_index) {
                if (active >= 0) return false;
                active = field;
            }
        if (active < 0) return false;
    } else {
        if (!plain_operand(source) || source->type != OPERAND_TYPE_IMMEDIATE32 ||
            source->register_index_dim || source->register_index || source->rel_offset1 ||
            source->swizzle_mode || source->destination_mask || source->raw_token != UINT32_C(0x00004002) ||
            source->imm_value_count != 4 || source->immediate_word_count != 4 ||
            memcmp(source->imm_values, source->immediate_words, sizeof(source->imm_values))) return false;
    }
    bool new_layout = false, inactive = false;
    uint8_t occupied[HLSL_SM5_IO_REGISTER_COUNT] = {0};
    uint32_t registers = 0;
    for (int field = 0; field < program->input_count; ++field) {
        const DXBCSignatureElement *input = &program->inputs[field];
        if (input->component_type < 1 || input->component_type > 3 || !input->mask || input->mask > 15 ||
            input->register_id >= HLSL_SM5_IO_REGISTER_COUNT || input->stream_index || input->min_precision ||
            !hlsl_inactive_input_semantic_supported(input) || (occupied[input->register_id] & input->mask)) return false;
        for (int previous = 0; previous < field; ++previous)
            if (program->inputs[previous].semantic_index == input->semantic_index &&
                !dxbc_ascii_strcasecmp(dxbc_signature_semantic_name(&program->inputs[previous]),
                    dxbc_signature_semantic_name(input))) return false;
        if (field == active) {
            if (input->system_value || input->component_type != 3 || input->mask != 15 || input->rw_mask != 15 ||
                input->interpolation_mode < 2 || input->interpolation_mode > 7) return false;
        } else {
            if (input->rw_mask || input->interpolation_mode) return false;
            inactive = true;
        }
        new_layout |= input->component_type != 3 || (input->mask & (input->mask + 1u)) ||
            occupied[input->register_id];
        occupied[input->register_id] |= input->mask;
        registers |= UINT32_C(1) << input->register_id;
    }
    if (!inactive || !new_layout || program->signature_declaration_count != (active < 0 ? 1 : 2)) return false;
    bool declared_input = false, declared_output = false;
    for (int index = 0; index < program->signature_declaration_count; ++index) {
        const USILSignatureDeclaration *declaration = &program->signature_declarations[index];
        if (!declaration->has_signature_register || declaration->mask != 15 || declaration->stream_index ||
            declaration->has_array_element_count || declaration->array_element_count ||
            declaration->has_system_value || declaration->system_value_name ||
            declaration->source_instruction_index >= move->source_instruction_index ||
            (index && declaration->source_instruction_index <=
                program->signature_declarations[index - 1].source_instruction_index)) return false;
        if (declaration->kind == USIL_SIGNATURE_DECL_OUTPUT &&
            declaration->operand_type == OPERAND_TYPE_OUTPUT && !declaration->register_id &&
            !declaration->has_interpolation && !declaration->interpolation_mode && !declared_output)
            declared_output = true;
        else if (active >= 0 && declaration->kind == USIL_SIGNATURE_DECL_INPUT_PS &&
            declaration->operand_type == OPERAND_TYPE_INPUT &&
            declaration->register_id == program->inputs[active].register_id && declaration->has_interpolation &&
            declaration->interpolation_mode == program->inputs[active].interpolation_mode && !declared_input)
            declared_input = true;
        else return false;
    }
    if (!declared_output || declared_input != (active >= 0)) return false;
    *plan = (HLSLInactiveInputPlan){.present = true, .active_input_field = active, .input_registers = registers};
    return true;
}
