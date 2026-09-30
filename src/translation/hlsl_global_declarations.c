// SPDX-License-Identifier: GPL-3.0-only
#include "translation/hlsl_global_declarations.h"
#include "translation/dxbc_cbuffer_projection.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct HLSLGlobalDeclarationUnion {
    int stage, current;
    DXBCProgramType dxbc_stage;
    uint32_t shell_size;
    HLSLGlobalDeclarationField *fields;
    size_t field_count;
};

static HLSLGlobalDeclarationStatus failure(HLSLGlobalDeclarationDiagnostic *diagnostic,
                                           HLSLGlobalDeclarationStatus status, int index,
                                           int conflict, int field) {
    if (diagnostic)
        *diagnostic = (HLSLGlobalDeclarationDiagnostic){status, index, conflict, field};
    return status;
}

void hlsl_global_declarations_free(HLSLGlobalDeclarationUnion *value) {
    if (!value)
        return;
    for (size_t i = 0; i < value->field_count; ++i) {
        free((void *)value->fields[i].name);
        free((void *)value->fields[i].witness_subprogram_indices);
    }
    free(value->fields);
    free(value);
}

static bool same_layout(const HLSLGlobalDeclarationField *a, const HLSLGlobalDeclarationField *b) {
    return strcmp(a->name, b->name) == 0 && a->byte_size == b->byte_size &&
           a->layout.byte_offset == b->layout.byte_offset &&
           a->layout.array_size == b->layout.array_size &&
           a->layout.scalar_type == b->layout.scalar_type && a->layout.rows == b->layout.rows &&
           a->layout.columns == b->layout.columns && a->layout.is_matrix == b->layout.is_matrix;
}

static HLSLGlobalDeclarationStatus add_field(HLSLGlobalDeclarationUnion *value,
                                             const HLSLGlobalDeclarationField *field,
                                             uint32_t witness, bool current) {
    HLSLGlobalDeclarationField *slot = NULL;
    for (size_t i = 0; i < value->field_count; ++i) {
        HLSLGlobalDeclarationField *prior = &value->fields[i];
        uint64_t end = (uint64_t)field->layout.byte_offset + field->byte_size;
        uint64_t prior_end = (uint64_t)prior->layout.byte_offset + prior->byte_size;
        if (strcmp(prior->name, field->name) != 0 &&
            (end <= prior->layout.byte_offset || prior_end <= field->layout.byte_offset))
            continue;
        if (!same_layout(prior, field))
            return HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT;
        slot = prior;
        break;
    }
    if (!slot) {
        if (value->field_count >= 4096 ||
            value->field_count >= SIZE_MAX / sizeof(*value->fields) - 1)
            return HLSL_GLOBAL_DECLARATIONS_INVALID;
        HLSLGlobalDeclarationField *fields =
            realloc(value->fields, (value->field_count + 1) * sizeof(*fields));
        if (!fields)
            return HLSL_GLOBAL_DECLARATIONS_ALLOCATION_FAILED;
        value->fields = fields;
        slot = &fields[value->field_count];
        *slot = *field;
        slot->name = NULL;
        slot->witness_count = 0;
        slot->witness_subprogram_indices = NULL;
        size_t length = strlen(field->name);
        char *name = malloc(length + 1);
        if (!name)
            return HLSL_GLOBAL_DECLARATIONS_ALLOCATION_FAILED;
        memcpy(name, field->name, length + 1);
        slot->name = name;
        ++value->field_count;
    }
    slot->current_authority |= current;
    for (size_t i = 0; i < slot->witness_count; ++i)
        if (slot->witness_subprogram_indices[i] == witness)
            return HLSL_GLOBAL_DECLARATIONS_OK;
    uint32_t *ids =
        realloc((void *)slot->witness_subprogram_indices, (slot->witness_count + 1) * sizeof(*ids));
    if (!ids)
        return HLSL_GLOBAL_DECLARATIONS_ALLOCATION_FAILED;
    size_t at = slot->witness_count;
    while (at && ids[at - 1] > witness) {
        ids[at] = ids[at - 1];
        --at;
    }
    ids[at] = witness;
    slot->witness_subprogram_indices = ids;
    ++slot->witness_count;
    return HLSL_GLOBAL_DECLARATIONS_OK;
}

static bool metadata_shape(const SerializedProgramParameters *parameters) {
    return !parameters || (parameters->cb_count >= 0 && parameters->res_count >= 0 &&
                           parameters->cb_count <= 4096 && parameters->res_count <= 4096 &&
                           (!parameters->cb_count || parameters->constant_buffers) &&
                           (!parameters->res_count || parameters->resources));
}

static bool identifier(const char *name) {
    if (!name || !name[0])
        return false;
    for (size_t i = 0; name[i]; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (i >= 255 || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                          (i && c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

static bool keyword_shape(const SerializedSubProgram *sub) {
    if (sub->local_keyword_count < 0 || sub->global_keyword_count < 0 ||
        sub->local_keyword_count > 4096 || sub->global_keyword_count > 4096 ||
        (sub->local_keyword_count && !sub->local_keywords) ||
        (sub->global_keyword_count && !sub->global_keywords))
        return false;
    int count = sub->local_keyword_count + sub->global_keyword_count;
    for (int i = 0; i < count; ++i) {
        const char *name = i < sub->local_keyword_count
                               ? sub->local_keywords[i]
                               : sub->global_keywords[i - sub->local_keyword_count];
        if (!identifier(name))
            return false;
        for (int j = 0; j < i; ++j) {
            const char *prior = j < sub->local_keyword_count
                                    ? sub->local_keywords[j]
                                    : sub->global_keywords[j - sub->local_keyword_count];
            if (strcmp(name, prior) == 0)
                return false;
        }
    }
    return true;
}

static HLSLGlobalDeclarationStatus shell(const SerializedProgramParameters *residual,
                                         const SerializedProgramParameters *common,
                                         uint32_t *size) {
    if (!metadata_shape(residual) || !metadata_shape(common))
        return HLSL_GLOBAL_DECLARATIONS_INVALID;
    *size = 0;
    bool bound = false;
    const SerializedProgramParameters *sources[2] = {residual, common};
    for (unsigned source = 0; source < 2; ++source) {
        const SerializedProgramParameters *parameters = sources[source];
        if (!parameters)
            continue;
        for (int i = 0; i < parameters->res_count; ++i) {
            const SerializedResourceParam *resource = &parameters->resources[i];
            if (resource->bind_type != SERIALIZED_RESOURCE_CONSTANT_BUFFER || !resource->name ||
                strcmp(resource->name, "$Globals") != 0)
                continue;
            if (resource->bind_index != 0)
                return HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            bound = true;
        }
        for (int i = 0; i < parameters->cb_count; ++i) {
            const SerializedConstantBuffer *buffer = &parameters->constant_buffers[i];
            if (!buffer->name)
                return HLSL_GLOBAL_DECLARATIONS_INVALID;
            if (strcmp(buffer->name, "$Globals") != 0 || buffer->role != SERIALIZED_CBUFFER_NAMED ||
                (buffer->has_is_partial && buffer->is_partial))
                continue;
            if (!buffer->size || buffer->size > 65536 || (buffer->size & 15u))
                return HLSL_GLOBAL_DECLARATIONS_INVALID;
            if (*size && *size != buffer->size)
                return HLSL_GLOBAL_DECLARATIONS_SHELL_CONFLICT;
            *size = buffer->size;
        }
    }
    return bound && *size ? HLSL_GLOBAL_DECLARATIONS_OK : HLSL_GLOBAL_DECLARATIONS_NOT_APPLICABLE;
}

static HLSLGlobalDeclarationStatus append_parameters(HLSLGlobalDeclarationUnion *value,
                                                     const SerializedProgramParameters *parameters,
                                                     uint32_t witness, bool current) {
    if (!parameters)
        return HLSL_GLOBAL_DECLARATIONS_OK;
    for (int i = 0; i < parameters->cb_count; ++i) {
        const SerializedConstantBuffer *buffer = &parameters->constant_buffers[i];
        if (!buffer->name || buffer->var_count < 0 || buffer->var_count > 4096 ||
            buffer->struct_count < 0 || (buffer->var_count && !buffer->variables))
            return HLSL_GLOBAL_DECLARATIONS_INVALID;
        if (strcmp(buffer->name, "$Globals") != 0)
            continue;
        if (buffer->role != SERIALIZED_CBUFFER_NAMED &&
            buffer->role != SERIALIZED_CBUFFER_LOOSE_PARAMETERS)
            return HLSL_GLOBAL_DECLARATIONS_INVALID;
        if (buffer->struct_count)
            return HLSL_GLOBAL_DECLARATIONS_NOT_APPLICABLE;
        bool project = buffer->role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS ||
                       (buffer->has_is_partial && buffer->is_partial);
        for (int variable = 0; variable < buffer->var_count; ++variable) {
            const SerializedVariable *source = &buffer->variables[variable];
            HLSLGlobalDeclarationField field = {0};
            field.name = source->name;
            if (!identifier(source->name) ||
                !parameter_layout_decode(parameters, source, &field.layout))
                return HLSL_GLOBAL_DECLARATIONS_INVALID;
            if (project && field.layout.byte_offset >= value->shell_size)
                continue;
            field.byte_size = parameter_layout_byte_size(&field.layout);
            if (field.layout.scalar_type != 0 || field.layout.is_matrix ||
                field.layout.array_size || field.layout.rows != 1 || !field.layout.columns ||
                field.layout.columns > 4 || (field.layout.byte_offset & 15u) ||
                field.byte_size != field.layout.columns * 4u ||
                (uint64_t)field.layout.byte_offset + field.byte_size > value->shell_size)
                return HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT;
            HLSLGlobalDeclarationStatus status = add_field(value, &field, witness, current);
            if (status != HLSL_GLOBAL_DECLARATIONS_OK)
                return status;
        }
    }
    return HLSL_GLOBAL_DECLARATIONS_OK;
}

HLSLGlobalDeclarationStatus
hlsl_global_declarations_scope_status(const SerializedProgramParameters *residual,
                                      const SerializedProgramParameters *common) {
    uint32_t size;
    return shell(residual, common, &size);
}

bool hlsl_global_declarations_same_family(const SerializedPass *pass, int stage, int current,
                                          int candidate) {
    if (!pass || stage < 0 || stage >= 5 || current < 0 || candidate < 0 ||
        pass->subprogram_count[stage] > 4096 || current >= pass->subprogram_count[stage] ||
        candidate >= pass->subprogram_count[stage] || !pass->subprograms[stage] ||
        !pass->subprogram_identities[stage] || !pass->has_serialized_platforms ||
        pass->platform_count < 0 || pass->platform_count > 4096 ||
        (pass->platform_count && !pass->platforms) ||
        !serialized_pass_subprogram_is_platform(pass, stage, current, 4) ||
        !serialized_pass_subprogram_is_platform(pass, stage, candidate, 4))
        return false;
    const SerializedSubProgram *target = &pass->subprograms[stage][current];
    const SerializedSubProgram *sub = &pass->subprograms[stage][candidate];
    const SerializedSubProgramIdentity *target_id = &pass->subprogram_identities[stage][current];
    const SerializedSubProgramIdentity *id = &pass->subprogram_identities[stage][candidate];
    return sub->program_type == target->program_type &&
           sub->shader_requirements == target->shader_requirements &&
           id->hardware_tier_group == target_id->hardware_tier_group &&
           sub->has_hardware_tier == target->has_hardware_tier &&
           (!sub->has_hardware_tier || sub->hardware_tier == target->hardware_tier);
}

HLSLGlobalDeclarationStatus
hlsl_global_declarations_build(const SerializedPass *pass, int stage, int current,
                               const HLSLGlobalDeclarationWitness *witnesses, size_t count,
                               HLSLGlobalDeclarationUnion **output,
                               HLSLGlobalDeclarationDiagnostic *diagnostic) {
    if (output)
        *output = NULL;
    if (!output || !pass || stage < 0 || stage >= 5 || current < 0 ||
        current >= pass->subprogram_count[stage] || !pass->subprograms[stage] || !witnesses ||
        !count || count > 4096 || count > (size_t)pass->subprogram_count[stage] ||
        pass->subprogram_count[stage] > 4096)
        return failure(diagnostic, HLSL_GLOBAL_DECLARATIONS_INVALID, current, -1, -1);
    if (pass->platform_count < 0 || pass->platform_count > 4096 ||
        (pass->platform_count && !pass->platforms))
        return failure(diagnostic, HLSL_GLOBAL_DECLARATIONS_INVALID, current, -1, -1);
    const HLSLGlobalDeclarationWitness *selected = NULL;
    for (size_t i = 0; i < count; ++i)
        if (witnesses[i].subprogram_index == current)
            selected = &witnesses[i];
    if (!selected)
        return failure(diagnostic, HLSL_GLOBAL_DECLARATIONS_INVALID, current, -1, -1);
    uint32_t selected_shell;
    HLSLGlobalDeclarationStatus status =
        shell(selected->residual, &pass->common_parameters[stage], &selected_shell);
    if (status != HLSL_GLOBAL_DECLARATIONS_OK)
        return failure(diagnostic, status, current, -1, -1);
    if (!pass->has_serialized_platforms || !pass->subprogram_identities[stage])
        return failure(diagnostic, HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT, current, -1, -1);
    HLSLGlobalDeclarationUnion *value = calloc(1, sizeof(*value));
    if (!value)
        return failure(diagnostic, HLSL_GLOBAL_DECLARATIONS_ALLOCATION_FAILED, current, -1, -1);
    value->stage = stage;
    value->current = current;
    value->shell_size = selected_shell;
    int conflict = -1;
    static const DXBCProgramType stages[5] = {DXBC_PROGRAM_TYPE_VERTEX, DXBC_PROGRAM_TYPE_PIXEL,
                                              DXBC_PROGRAM_TYPE_GEOMETRY, DXBC_PROGRAM_TYPE_HULL,
                                              DXBC_PROGRAM_TYPE_DOMAIN};
    value->dxbc_stage = stages[stage];
    const SerializedSubProgram *target = &pass->subprograms[stage][current];
    const SerializedSubProgramIdentity *target_id = &pass->subprogram_identities[stage][current];
    size_t eligible = 0;
    for (int i = 0; i < pass->subprogram_count[stage]; ++i) {
        if (hlsl_global_declarations_same_family(pass, stage, current, i))
            ++eligible;
    }
    for (size_t i = 0; i < count; ++i) {
        int index = witnesses[i].subprogram_index;
        conflict = index;
        if (index < 0 || index >= pass->subprogram_count[stage]) {
            status = HLSL_GLOBAL_DECLARATIONS_INVALID;
            goto fail;
        }
        for (size_t j = 0; j < i; ++j)
            if (witnesses[j].subprogram_index == index) {
                status = HLSL_GLOBAL_DECLARATIONS_INVALID;
                goto fail;
            }
        const SerializedSubProgram *sub = &pass->subprograms[stage][index];
        const SerializedSubProgramIdentity *id = &pass->subprogram_identities[stage][index];
        if (!serialized_pass_subprogram_is_platform(pass, stage, index, 4) ||
            sub->program_type != target->program_type ||
            sub->shader_requirements != target->shader_requirements ||
            id->hardware_tier_group != target_id->hardware_tier_group ||
            sub->has_hardware_tier != target->has_hardware_tier ||
            (sub->has_hardware_tier && sub->hardware_tier != target->hardware_tier) ||
            !keyword_shape(sub) || !witnesses[i].player ||
            witnesses[i].player->program_type != sub->program_type ||
            !witnesses[i].player->has_player_blob_header ||
            !subprogram_metadata_local_keyword_set_matches(witnesses[i].player, sub)) {
            status = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            goto fail;
        }
        uint32_t size;
        status = shell(witnesses[i].residual, &pass->common_parameters[stage], &size);
        if (status != HLSL_GLOBAL_DECLARATIONS_OK || size != selected_shell) {
            status = HLSL_GLOBAL_DECLARATIONS_SHELL_CONFLICT;
            goto fail;
        }
        status = append_parameters(value, witnesses[i].residual, (uint32_t)index, index == current);
        if (status == HLSL_GLOBAL_DECLARATIONS_OK)
            status = append_parameters(value, &pass->common_parameters[stage], (uint32_t)index,
                                       index == current);
        if (status != HLSL_GLOBAL_DECLARATIONS_OK)
            goto fail;
    }
    if (eligible != count) {
        conflict = -1;
        status = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
        goto fail;
    }
    *output = value;
    return failure(diagnostic, HLSL_GLOBAL_DECLARATIONS_OK, current, -1, -1);
fail:
    hlsl_global_declarations_free(value);
    return failure(diagnostic, status, current, conflict, -1);
}

const HLSLGlobalDeclarationField *
hlsl_global_declarations_fields(const HLSLGlobalDeclarationUnion *value, size_t *count) {
    if (count)
        *count = value ? value->field_count : 0;
    return value ? value->fields : NULL;
}
uint32_t hlsl_global_declarations_shell_size(const HLSLGlobalDeclarationUnion *value) {
    return value ? value->shell_size : 0;
}
int hlsl_global_declarations_current_variant(const HLSLGlobalDeclarationUnion *value) {
    return value ? value->current : -1;
}

HLSLGlobalDeclarationStatus hlsl_global_declarations_validate_target(
    const HLSLGlobalDeclarationUnion *value, const USILProgram *program,
    const SerializedProgramParameters *residual, const SerializedProgramParameters *common) {
    if (!value || !program || program->program_type != value->dxbc_stage ||
        program->cbuffer_count < 0 || program->cbuffer_count > 4096 ||
        (program->cbuffer_count && !program->cbuffers) || program->instruction_count < 0 ||
        (program->instruction_count && !program->instructions))
        return HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
    uint32_t size;
    HLSLGlobalDeclarationStatus status = shell(residual, common, &size);
    if (status != HLSL_GLOBAL_DECLARATIONS_OK || size != value->shell_size)
        return HLSL_GLOBAL_DECLARATIONS_SHELL_CONFLICT;
    HLSLGlobalDeclarationUnion own = {.shell_size = size};
    status = append_parameters(&own, residual, (uint32_t)value->current, true);
    if (status == HLSL_GLOBAL_DECLARATIONS_OK)
        status = append_parameters(&own, common, (uint32_t)value->current, true);
    size_t expected = 0;
    for (size_t i = 0; i < value->field_count; ++i)
        if (value->fields[i].current_authority)
            ++expected;
    if (status == HLSL_GLOBAL_DECLARATIONS_OK && own.field_count != expected)
        status = HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT;
    for (size_t i = 0; status == HLSL_GLOBAL_DECLARATIONS_OK && i < own.field_count; ++i) {
        bool found = false;
        for (size_t j = 0; j < value->field_count; ++j)
            if (value->fields[j].current_authority &&
                same_layout(&own.fields[i], &value->fields[j]))
                found = true;
        if (!found)
            status = HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT;
    }
    DXBCCBufferVariableRange *ranges =
        own.field_count ? calloc(own.field_count, sizeof(*ranges)) : NULL;
    DXBCCBufferVariableUse *uses = own.field_count ? calloc(own.field_count, sizeof(*uses)) : NULL;
    if (own.field_count && (!ranges || !uses))
        status = HLSL_GLOBAL_DECLARATIONS_ALLOCATION_FAILED;
    for (size_t i = 0; ranges && i < own.field_count; ++i)
        ranges[i] =
            (DXBCCBufferVariableRange){own.fields[i].layout.byte_offset, own.fields[i].byte_size};
    bool found_buffer = false;
    for (int i = 0; status == HLSL_GLOBAL_DECLARATIONS_OK && i < program->cbuffer_count; ++i) {
        if (program->cbuffers[i].reg_idx != 0)
            continue;
        if (found_buffer) {
            status = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        }
        found_buffer = true;
        DXBCCBufferProjection projection = {0};
        if (program->cbuffers[i].size <= 0 || (uint64_t)program->cbuffers[i].size * 16u > size ||
            dxbc_cbuffer_project_program(program, 0, (uint32_t)program->cbuffers[i].size * 16u,
                                         ranges, own.field_count, uses,
                                         &projection) != DXBC_CBUFFER_PROJECTION_EXACT ||
            projection.saw_static_padding_access || projection.saw_dynamic_access)
            status = HLSL_GLOBAL_DECLARATIONS_CURRENT_READ_UNAUTHORIZED;
    }
    if (!found_buffer && status == HLSL_GLOBAL_DECLARATIONS_OK)
        status = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
    free(ranges);
    free(uses);
    for (size_t i = 0; i < own.field_count; ++i) {
        free((void *)own.fields[i].name);
        free((void *)own.fields[i].witness_subprogram_indices);
    }
    free(own.fields);
    return status;
}
