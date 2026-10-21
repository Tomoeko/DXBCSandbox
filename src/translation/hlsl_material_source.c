// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <string.h>

/* Material-field authority is shared by the expression and packed-output
 * planners. Formatting and AST construction remain with their consumers. */
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

static bool material_field_name_unique(const HLSLEmitterContext *ctx, const TempVariable *field) {
    for (int buffer_index = 0; buffer_index < ctx->cbuffer_layout_count; ++buffer_index) {
        const HLSLCBufferLayout *other = &ctx->cbuffer_layouts[buffer_index];
        if (other->variable_count < 0 || other->variable_alloc < other->variable_count ||
            (other->variable_count && !other->variables)) return false;
        for (int variable = 0; variable < other->variable_count; ++variable) {
            const TempVariable *candidate = &other->variables[variable];
            if (!candidate->name || (candidate != field && !strcmp(candidate->name, field->name)))
                return false;
        }
    }
    return true;
}

static bool bounded_material_name(const char *name) {
    if (!name || !name[0]) return false;
    for (size_t index = 1; index < 256u; ++index) if (!name[index]) return true;
    return false;
}

/* Include dormant serialized buffers: the ordinary name resolver traverses
 * both inventories, so another buffer's matching identifier cannot own this
 * scalar even when its layout coincides. The two authorities may agree on one
 * exact field; role and buffer identity remain part of that agreement. */
static bool packed_uint_metadata_unique(const HLSLEmitterContext *ctx,
    const HLSLCBufferLayout *buffer, const TempVariable *field) {
    const SerializedProgramParameters *sources[] = {ctx->params, ctx->common_params};
    bool found = false;
    SerializedConstantBufferRole role = SERIALIZED_CBUFFER_NAMED;
    for (size_t source = 0; source < 2u; ++source) {
        const SerializedProgramParameters *parameters = sources[source];
        if (!parameters) continue;
        if (parameters->cb_count < 0 || parameters->cb_count > 8 ||
            (parameters->cb_count && !parameters->constant_buffers) || parameters->res_count < 0 ||
            parameters->res_count > 128 || (parameters->res_count && !parameters->resources)) return false;
        unsigned matches = 0;
        for (int cb = 0; cb < parameters->cb_count; ++cb) {
            const SerializedConstantBuffer *owner = &parameters->constant_buffers[cb];
            if (!bounded_material_name(owner->name) || owner->var_count < 0 || owner->var_count > 128 ||
                (owner->var_count && !owner->variables) || owner->struct_count < 0 ||
                owner->struct_count > 16 || (owner->struct_count && !owner->struct_params)) return false;
            for (int variable = 0; variable < owner->var_count; ++variable) {
                const SerializedVariable *candidate = &owner->variables[variable];
                if (!bounded_material_name(candidate->name)) return false;
                if (strcmp(candidate->name, field->name)) continue;
                DecodedVariableLayout layout;
                if (++matches > 1u || strcmp(owner->name, buffer->serialized_name) ||
                    (owner->role != SERIALIZED_CBUFFER_NAMED &&
                     owner->role != SERIALIZED_CBUFFER_LOOSE_PARAMETERS) ||
                    (found && owner->role != role) ||
                    !parameter_layout_decode(parameters, candidate, &layout) ||
                    layout.scalar_type || layout.rows != 1 || layout.columns != 1 ||
                    layout.array_size || layout.is_matrix || layout.byte_offset != field->byte_offset ||
                    parameter_layout_byte_size(&layout) != 4) return false;
                role = owner->role;
                found = true;
            }
            for (int structure = 0; structure < owner->struct_count; ++structure) {
                const SerializedStructParam *aggregate = &owner->struct_params[structure];
                if (!bounded_material_name(aggregate->name) || aggregate->member_count < 0 ||
                    aggregate->member_count > 128 || (aggregate->member_count && !aggregate->members) ||
                    !strcmp(aggregate->name, field->name)) return false;
                for (int member = 0; member < aggregate->member_count; ++member)
                    if (!bounded_material_name(aggregate->members[member].name) ||
                        !strcmp(aggregate->members[member].name, field->name)) return false;
            }
        }
        for (int resource = 0; resource < parameters->res_count; ++resource) {
            const SerializedResourceParam *binding = &parameters->resources[resource];
            if (binding->bind_type != SERIALIZED_RESOURCE_CONSTANT_BUFFER) continue;
            if (!bounded_material_name(binding->name) ||
                (!strcmp(binding->name, buffer->serialized_name) && binding->bind_index != (uint32_t)buffer->reg))
                return false;
        }
    }
    return found;
}

bool hlsl_packed_uint_material_supported(HLSLEmitterContext *ctx,
    const HLSLPackedUIntOutputPlan *plan) {
    HLSLPackedUIntOutputPlan current;
    if (!ctx || !ctx->high_level_packed_outputs || !plan || !plan->present ||
        !hlsl_packed_output_candidate(ctx->program) ||
        !hlsl_packed_uint_output_plan_prepare(ctx->program, &current) ||
        current.instruction_index != plan->instruction_index ||
        current.output_signature_index != plan->output_signature_index ||
        current.source_instruction_index != plan->source_instruction_index ||
        current.destination_lanes != plan->destination_lanes || current.cbuffer_binding != plan->cbuffer_binding ||
        current.row != plan->row || current.physical_component != plan->physical_component ||
        current.byte_offset != plan->byte_offset || !ctx->cbuffer_layouts_built ||
        ctx->cbuffer_layout_count < 1 || ctx->cbuffer_layout_count > HLSL_MAX_CBUFFER_LAYOUTS) return false;
    const HLSLCBufferLayout *buffer = get_cbuffer_emission_layout(ctx, current.cbuffer_binding);
    const char *binding = get_cbuffer_name_from_map(ctx, current.cbuffer_binding);
    if (!buffer || buffer->raw_storage || buffer->row_struct_storage || !buffer->has_serialized_authority ||
        !bounded_material_name(buffer->serialized_name) || !bounded_material_name(binding) ||
        strcmp(buffer->serialized_name, binding) || buffer->variable_count < 1 ||
        buffer->variable_count > 128 || buffer->variable_alloc < buffer->variable_count ||
        !buffer->variables || buffer->row_count <= (int)current.row) return false;
    const TempVariable *field = NULL;
    for (int variable = 0; variable < buffer->variable_count; ++variable) {
        const TempVariable *candidate = &buffer->variables[variable];
        if (candidate->byte_offset != current.byte_offset) continue;
        if (field) return false;
        field = candidate;
    }
    if (!field || !bounded_material_name(field->name) || field->type || field->rows != 1 || field->dim != 1 ||
        field->is_matrix || field->matrix_array_size || field->byte_size != 4 ||
        (field->authority != 1 && field->authority != 2)) return false;
    for (int cb = 0; cb < ctx->cbuffer_layout_count; ++cb) {
        const HLSLCBufferLayout *owner = &ctx->cbuffer_layouts[cb];
        if (owner->variable_count < 0 || owner->variable_count > 128 ||
            owner->variable_alloc < owner->variable_count || (owner->variable_count && !owner->variables)) return false;
        for (int variable = 0; variable < owner->variable_count; ++variable)
            if (!bounded_material_name(owner->variables[variable].name)) return false;
    }
    if (!material_field_name_unique(ctx, field) || !packed_uint_metadata_unique(ctx, buffer, field)) return false;
    int field_offset = -1;
    const char *name = resolve_cb_variable_ctx(ctx, current.cbuffer_binding, (int)current.row,
        current.physical_component, &field_offset);
    DecodedVariableLayout layout;
    return name && !strcmp(name, field->name) && !field_offset &&
        resolve_variable_layout_ctx(ctx, field->name, &layout) && !layout.scalar_type && layout.rows == 1 &&
        layout.columns == 1 && !layout.is_matrix && !layout.array_size &&
        layout.byte_offset == current.byte_offset && parameter_layout_byte_size(&layout) == 4;
}

/* The program-only position plan identifies the two physical slices. MAD
 * additionally needs one serialized float4 field; two adjacent float2 fields
 * cannot supply that source identity. Reuse the merged, scoped layout rather
 * than treating a variable name or a DXBC row as declaration authority. */
bool hlsl_position_output_material_supported(HLSLEmitterContext *ctx,
    const HLSLPositionOutputPlan *plan) {
    HLSLPositionOutputPlan current;
    if (!ctx || !plan || !hlsl_position_output_plan_prepare(ctx->program, &current) ||
        !hlsl_position_output_plans_equal(&current, plan)) return false;
    if (plan->assembly.pieces[0].opcode == USIL_OP_MOV) return true;
    if (plan->assembly.pieces[0].opcode != USIL_OP_MAD || !ctx->cbuffer_layouts_built ||
        ctx->cbuffer_layout_count < 1 || ctx->cbuffer_layout_count > HLSL_MAX_CBUFFER_LAYOUTS)
        return false;
    const USILInstruction *owner = &ctx->program->instructions[plan->assembly.pieces[0].instruction_index];
    const DXBCOperand *scale = &owner->operands[2], *offset = &owner->operands[3];
    if (!hlsl_material_source_supported(ctx, scale, 3) ||
        !hlsl_material_source_supported(ctx, offset, 3)) return false;
    const HLSLCBufferLayout *buffer = get_cbuffer_emission_layout(ctx, scale->register_index);
    const char *binding = get_cbuffer_name_from_map(ctx, scale->register_index);
    if (!buffer || buffer->raw_storage || buffer->row_struct_storage ||
        !buffer->has_serialized_authority || !buffer->serialized_name || !binding ||
        strcmp(buffer->serialized_name, binding) || buffer->variable_count < 1 ||
        buffer->variable_alloc < buffer->variable_count || !buffer->variables ||
        scale->rel_offset0 >= buffer->row_count) return false;
    const uint64_t byte_offset = (uint64_t)(uint32_t)scale->rel_offset0 * 16u;
    if (byte_offset > UINT32_MAX) return false;
    const TempVariable *field = NULL;
    for (int variable = 0; variable < buffer->variable_count; ++variable) {
        const TempVariable *candidate = &buffer->variables[variable];
        if (candidate->byte_offset != (uint32_t)byte_offset) continue;
        if (field) return false;
        field = candidate;
    }
    if (!field || !field->name || field->type != 0 || field->rows != 1 || field->dim != 4 ||
        field->is_matrix || field->matrix_array_size || field->byte_size != 16 ||
        (field->authority != 1 && field->authority != 2)) return false;
    /* The shared name-to-layout resolver is intentionally unscoped. A same
     * spelling in another buffer cannot prove this field's source identity,
     * even when both physical layouts happen to be identical. */
    if (!material_field_name_unique(ctx, field)) return false;
    for (int component = 0; component < 4; ++component) {
        int field_offset = -1;
        const char *name = resolve_cb_variable_ctx(ctx, scale->register_index,
            scale->rel_offset0, component, &field_offset);
        if (!name || strcmp(name, field->name) || field_offset != component * 4) return false;
    }
    DecodedVariableLayout layout;
    return resolve_variable_layout_ctx(ctx, field->name, &layout) &&
        layout.scalar_type == field->type && layout.rows == field->rows && layout.columns == field->dim &&
        !layout.is_matrix && !layout.array_size && layout.byte_offset == field->byte_offset &&
        parameter_layout_byte_size(&layout) == field->byte_size;
}
