// SPDX-License-Identifier: GPL-3.0-only
#include "translation/hlsl_current_matrix_reads.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_matrix_lift.h"
#include "translation/usil_validation.h"
#include <stdlib.h>
#include <string.h>

void hlsl_current_matrix_reads_init(HLSLCurrentMatrixReads *inventory) {
    if (inventory) memset(inventory, 0, sizeof(*inventory));
}
void hlsl_current_matrix_reads_dispose(HLSLCurrentMatrixReads *inventory) {
    if (!inventory) return;
    free(inventory->fields);
    free(inventory->reads);
    hlsl_current_matrix_reads_init(inventory);
}

static bool metadata_shape(const SerializedProgramParameters *parameters) {
    if (!parameters) return true;
    if (parameters->cb_count < 0 || parameters->cb_count > 4096 ||
        parameters->res_count < 0 || parameters->res_count > 4096 ||
        (parameters->cb_count && !parameters->constant_buffers) ||
        (parameters->res_count && !parameters->resources)) return false;
    size_t walked_fields = 0;
    for (int index = 0; index < parameters->cb_count; ++index) {
        const SerializedConstantBuffer *buffer = &parameters->constant_buffers[index];
        if (!buffer->name || buffer->var_count < 0 ||
            buffer->var_count > HLSL_CURRENT_MATRIX_FIELD_LIMIT ||
            (buffer->var_count && !buffer->variables) || buffer->struct_count < 0 ||
            buffer->struct_count > HLSL_CURRENT_MATRIX_FIELD_LIMIT ||
            (buffer->struct_count && !buffer->struct_params)) return false;
        walked_fields += (size_t)buffer->var_count;
        if (walked_fields > HLSL_CURRENT_MATRIX_READ_LIMIT) return false;
        for (int field = 0; field < buffer->var_count; ++field)
            if (!buffer->variables[field].name) return false;
        for (int structure = 0; structure < buffer->struct_count; ++structure) {
            const SerializedStructParam *parameter = &buffer->struct_params[structure];
            if (!parameter->name || parameter->member_count < 0 ||
                parameter->member_count > HLSL_CURRENT_MATRIX_FIELD_LIMIT ||
                (parameter->member_count && !parameter->members)) return false;
            walked_fields += (size_t)parameter->member_count;
            if (walked_fields > HLSL_CURRENT_MATRIX_READ_LIMIT) return false;
            for (int member = 0; member < parameter->member_count; ++member)
                if (!parameter->members[member].name) return false;
        }
    }
    for (int index = 0; index < parameters->res_count; ++index)
        if (!parameters->resources[index].name) return false;
    return true;
}

static bool copy_name(char destination[HLSL_CURRENT_MATRIX_NAME_LIMIT], const char *source) {
    if (!source || !source[0]) return false;
    size_t size = strlen(source);
    if (size >= HLSL_CURRENT_MATRIX_NAME_LIMIT) return false;
    memcpy(destination, source, size + 1);
    return true;
}

static bool append_field(HLSLCurrentMatrixReads *inventory,
                         const HLSLCurrentMatrixField *field, uint32_t *index) {
    for (size_t prior = 0; prior < inventory->field_count; ++prior) {
        if (inventory->fields[prior].binding_register == field->binding_register &&
            inventory->fields[prior].field_byte_offset == field->field_byte_offset) {
            *index = (uint32_t)prior;
            return true;
        }
    }
    if (inventory->field_count == HLSL_CURRENT_MATRIX_FIELD_LIMIT) return false;
    HLSLCurrentMatrixField *fields = realloc(inventory->fields,
        (inventory->field_count + 1) * sizeof(*fields));
    if (!fields) return false;
    inventory->fields = fields;
    *index = (uint32_t)inventory->field_count;
    inventory->fields[inventory->field_count++] = *field;
    return true;
}

static bool append_read(HLSLCurrentMatrixReads *inventory, const HLSLCurrentMatrixRead *read) {
    if (inventory->read_count == HLSL_CURRENT_MATRIX_READ_LIMIT) return false;
    HLSLCurrentMatrixRead *reads = realloc(inventory->reads,
        (inventory->read_count + 1) * sizeof(*reads));
    if (!reads) return false;
    inventory->reads = reads;
    inventory->reads[inventory->read_count++] = *read;
    return true;
}

static bool field_metadata(const HLSLEmitterContext *context,
                           const HLSLCBufferLayout *layout, const TempVariable *variable,
                           HLSLCurrentMatrixField *field) {
    const SerializedProgramParameters *sets[] = {context->params, context->common_params};
    if (variable->authority != 1 && variable->authority != 2) return false;
    const SerializedProgramParameters *owner = sets[variable->authority - 1];
    if (!owner || !layout->serialized_name || layout->is_globals ||
        !layout->has_reflection_size_authority || !layout->has_serialized_authority ||
        layout->raw_storage || layout->row_struct_storage || variable->row_major ||
        !copy_name(field->block_name, layout->serialized_name) ||
        !copy_name(field->field_name, variable->name)) return false;
    unsigned matches = 0;
    for (int buffer_index = 0; buffer_index < owner->cb_count; ++buffer_index) {
        const SerializedConstantBuffer *buffer = &owner->constant_buffers[buffer_index];
        if (strcmp(buffer->name, layout->serialized_name) != 0) continue;
        if (buffer->role != SERIALIZED_CBUFFER_NAMED || buffer->struct_count) return false;
        for (int variable_index = 0; variable_index < buffer->var_count; ++variable_index) {
            const SerializedVariable *candidate = &buffer->variables[variable_index];
            if (!candidate->name || strcmp(candidate->name, variable->name) != 0) continue;
            DecodedVariableLayout decoded;
            if (!parameter_layout_decode(owner, candidate, &decoded) ||
                decoded.scalar_type != 0 || !decoded.is_matrix || decoded.rows != 4 ||
                decoded.columns != 4 || decoded.array_size ||
                decoded.byte_offset != variable->byte_offset ||
                parameter_layout_byte_size(&decoded) != 64) return false;
            field->metadata_buffer_index = (uint32_t)buffer_index;
            field->metadata_field_index = (uint32_t)variable_index;
            ++matches;
        }
    }
    if (matches != 1) return false;
    for (unsigned set = 0; set < 2; ++set) {
        if (!sets[set]) continue;
        for (int index = 0; index < sets[set]->cb_count; ++index) {
            const SerializedConstantBuffer *buffer = &sets[set]->constant_buffers[index];
            if (strcmp(buffer->name, layout->serialized_name) == 0 &&
                buffer->role == SERIALIZED_CBUFFER_NAMED &&
                !(buffer->has_is_partial && buffer->is_partial) &&
                buffer->size == layout->reflection_size_bytes)
                field->full_shell_authorities |= (uint8_t)(1u << set);
        }
    }
    if (!field->full_shell_authorities) return false;
    const CBufferRegMapEntry *binding = NULL;
    for (int index = 0; index < context->cb_reg_map_count; ++index) {
        const CBufferRegMapEntry *candidate = &context->cb_reg_map[index];
        if (candidate->dxbc_reg == layout->reg && candidate->resolved) binding = candidate;
    }
    if (!binding || binding->authority < 1 || binding->authority > 2 ||
        strcmp(binding->cb_name, layout->serialized_name) != 0) return false;
    owner = sets[binding->authority - 1];
    matches = 0;
    for (int index = 0; owner && index < owner->res_count; ++index) {
        const SerializedResourceParam *resource = &owner->resources[index];
        if (resource->bind_type == SERIALIZED_RESOURCE_CONSTANT_BUFFER &&
            resource->bind_index == (uint32_t)layout->reg && resource->name &&
            strcmp(resource->name, layout->serialized_name) == 0) {
            field->metadata_binding_index = (uint32_t)index;
            ++matches;
        }
    }
    if (matches != 1) return false;
    field->binding_register = (uint32_t)layout->reg;
    field->declared_byte_size = (uint32_t)layout->row_count * 16u;
    field->reflected_byte_size = layout->reflection_size_bytes;
    field->field_byte_offset = variable->byte_offset;
    field->field_byte_size = 64;
    field->field_authority = variable->authority;
    field->binding_authority = binding->authority;
    field->logical_aggregate_id = (((uint64_t)field->binding_register + 1) << 32) |
                                  field->field_byte_offset;
    return true;
}

static HLSLCurrentMatrixStatus observe_operand(HLSLEmitterContext *context,
    int instruction_index, int operand_index, uint8_t demand, HLSLCurrentMatrixReads *out) {
    const USILInstruction *instruction = &context->program->instructions[instruction_index];
    const DXBCOperand *operand = &instruction->operands[operand_index];
    const HLSLCBufferLayout *layout = get_cbuffer_emission_layout(context, operand->register_index);
    if (!layout || layout->raw_storage || layout->projection_status != DXBC_CBUFFER_PROJECTION_EXACT ||
        layout->projection.saw_padding_access || layout->projection.saw_dynamic_access ||
        operand->rel_offset0 < 0) return HLSL_CURRENT_MATRIX_MISSING_AUTHORITY;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(demand & (1u << lane))) continue;
        const int component = usil_operand_source_component(operand, lane);
        if (component < 0 || component > 3) return HLSL_CURRENT_MATRIX_INVALID_PROGRAM;
        const uint64_t byte = (uint64_t)(uint32_t)operand->rel_offset0 * 16u + (unsigned)component * 4u;
        const TempVariable *variable = NULL;
        for (int index = 0; index < layout->variable_count; ++index) {
            const TempVariable *candidate = &layout->variables[index];
            if (byte >= candidate->byte_offset && byte < (uint64_t)candidate->byte_offset + candidate->byte_size) {
                if (variable) return HLSL_CURRENT_MATRIX_MISSING_AUTHORITY;
                variable = candidate;
            }
        }
        if (!variable) return HLSL_CURRENT_MATRIX_MISSING_AUTHORITY;
        if (!variable->is_matrix) continue;
        if (operand->has_abs || operand->has_neg || operand->min_precision ||
            operand->extended_token_count || operand->extended_tokens || variable->matrix_array_size)
            return HLSL_CURRENT_MATRIX_UNSUPPORTED;
        bool row_major;
        if (!hlsl_matrix_lift_identifier(context, layout->reg,
                (int)(variable->byte_offset / 16u), &row_major) || row_major)
            return HLSL_CURRENT_MATRIX_MISSING_AUTHORITY;
        HLSLCurrentMatrixField field = {0};
        if (!field_metadata(context, layout, variable, &field))
            return HLSL_CURRENT_MATRIX_MISSING_AUTHORITY;
        DXBCCBufferVariableRange range = {field.field_byte_offset, 64};
        DXBCCBufferVariableUse use = {0};
        DXBCCBufferProjection projection = {0};
        if (dxbc_cbuffer_project_operand(operand, (uint8_t)(1u << lane),
                field.binding_register, field.declared_byte_size, &range, 1,
                &use, &projection) != DXBC_CBUFFER_PROJECTION_EXACT ||
            !use.referenced || projection.saw_padding_access || projection.saw_dynamic_access)
            return HLSL_CURRENT_MATRIX_INVALID_PROGRAM;
        uint32_t field_index;
        if (!append_field(out, &field, &field_index)) return HLSL_CURRENT_MATRIX_ALLOCATION_FAILED;
        HLSLCurrentMatrixRead read = {.field_index = field_index,
            .instruction_index = instruction_index, .operand_index = operand_index,
            .source_instruction_index = instruction->source_instruction_index,
            .destination_lanes = (uint8_t)(instruction->operand_count
                ? usil_operand_destination_lane_mask(&instruction->operands[0]) : 0u),
            .logical_lane = (uint8_t)lane, .physical_lane = (uint8_t)component,
            .physical_row = (uint32_t)operand->rel_offset0, .byte_offset = (uint32_t)byte,
            .field_relative_byte_offset = (uint32_t)byte - field.field_byte_offset};
        if (!append_read(out, &read)) return HLSL_CURRENT_MATRIX_ALLOCATION_FAILED;
    }
    return HLSL_CURRENT_MATRIX_OK;
}

HLSLCurrentMatrixStatus hlsl_current_matrix_reads_build(const USILProgram *program,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    HLSLCurrentMatrixReads *out) {
    if (!out || out->fields || out->reads || out->field_count || out->read_count || !program ||
        !metadata_shape(current) || !metadata_shape(common)) return HLSL_CURRENT_MATRIX_INVALID_ARGUMENT;
    if (program->instruction_count < 1 || program->instruction_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        !program->instructions || program->instruction_alloc < program->instruction_count ||
        program->cbuffer_count < 0 || program->cbuffer_alloc < program->cbuffer_count ||
        program->cbuffer_count > HLSL_MAX_CBUFFER_LAYOUTS ||
        (program->cbuffer_count && !program->cbuffers)) return HLSL_CURRENT_MATRIX_INVALID_PROGRAM;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        if (!usil_instruction_shape_valid(program, instruction)) return HLSL_CURRENT_MATRIX_INVALID_PROGRAM;
        /* Relative trees have independently consumed operands. This first API
         * does not enumerate or invent their index/range authority. */
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            const DXBCOperand *value = &instruction->operands[operand];
            if (value->rel_op0 || value->rel_op1 || value->rel_op2)
                return HLSL_CURRENT_MATRIX_UNSUPPORTED;
        }
    }
    for (int index = 0; index < program->cbuffer_count; ++index)
        if (program->cbuffers[index].dynamic_indexed) return HLSL_CURRENT_MATRIX_UNSUPPORTED;
    HLSLEmitterContext *context = calloc(1, sizeof(*context));
    if (!context) return HLSL_CURRENT_MATRIX_ALLOCATION_FAILED;
    HLSLEmitDiagnostic diagnostic;
    hlsl_emit_diagnostic_init(&diagnostic);
    context->program = program;
    context->params = current;
    context->common_params = common;
    context->diagnostic = &diagnostic;
    context->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    context->current_instruction_index = -1;
    HLSLCurrentMatrixReads temporary = {0};
    HLSLCurrentMatrixStatus status = HLSL_CURRENT_MATRIX_MISSING_AUTHORITY;
    if (!build_cbuffer_register_map(context) || !build_cbuffer_emission_layouts(context)) goto done;
    status = HLSL_CURRENT_MATRIX_OK;
    for (int index = 0; index < program->instruction_count && status == HLSL_CURRENT_MATRIX_OK; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            if (instruction->operands[operand].type != OPERAND_TYPE_CONSTANT_BUFFER) continue;
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(program, instruction, operand, &use) ||
                use.use != USIL_OPERAND_USE_SOURCE || !use.source_lane_mask) {
                status = HLSL_CURRENT_MATRIX_INVALID_PROGRAM; break;
            }
            status = observe_operand(context, index, operand, use.source_lane_mask, &temporary);
            if (status != HLSL_CURRENT_MATRIX_OK) break;
        }
    }
    if (status == HLSL_CURRENT_MATRIX_OK && !temporary.read_count)
        status = HLSL_CURRENT_MATRIX_NOT_APPLICABLE;
    if (status == HLSL_CURRENT_MATRIX_OK) {
        *out = temporary;
        hlsl_current_matrix_reads_init(&temporary);
    }
done:
    hlsl_current_matrix_reads_dispose(&temporary);
    free_cbuffer_emission_layouts(context);
    free(context->cb_reg_map);
    free(context);
    return status;
}
