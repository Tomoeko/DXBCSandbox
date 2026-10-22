// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_global_declarations.h"
#include "io/parameter_layout.h"
#include "hlsl_source_identifier.h"

#include <string.h>

enum { CBUFFER_INVENTORY_ROW_LIMIT = 4096 };

static bool name_available(const HLSLEmitterContext *ctx, const HLSLCBufferLayout *owner,
                           const TempVariable *field, const char *name) {
    if (!hlsl_source_identifier_valid(name) ||
        (ctx->entry_point_name && strcmp(ctx->entry_point_name, name) == 0)) return false;
    for (size_t macro = 0; macro < ctx->reserved_preprocessor_identifier_count; ++macro)
        if (strcmp(ctx->reserved_preprocessor_identifiers[macro], name) == 0) return false;
    for (int buffer = 0; buffer < ctx->cbuffer_layout_count; ++buffer) {
        const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[buffer];
        if ((layout != owner || field) && layout->declaration_name &&
            strcmp(layout->declaration_name, name) == 0) return false;
        for (int variable = 0; variable < layout->variable_count; ++variable)
            if (&layout->variables[variable] != field && layout->variables[variable].name &&
                strcmp(layout->variables[variable].name, name) == 0) return false;
    }
    for (int texture = 0; texture < ctx->program->texture_count; ++texture) {
        const USILTexture *resource = &ctx->program->textures[texture];
        if (!memchr(resource->dimension, '\0', sizeof(resource->dimension))) return false;
        SerializedResourceType kind = strcmp(resource->dimension, "raw") == 0 ||
            strcmp(resource->dimension, "structured") == 0
                ? SERIALIZED_RESOURCE_BUFFER : SERIALIZED_RESOURCE_TEXTURE;
        const char *resource_name = NULL;
        if (!resolve_srv_name_ctx(ctx, resource->reg_idx, kind, &resource_name) ||
            (resource_name && strcmp(resource_name, name) == 0)) return false;
    }
    for (int sampler = 0; sampler < ctx->program->sampler_count; ++sampler) {
        int reg = ctx->program->samplers[sampler].reg_idx;
        if (reg < 0 || reg >= HLSL_SM5_SAMPLER_REGISTER_COUNT ||
            (ctx->sampler_names[reg] && strcmp(ctx->sampler_names[reg], name) == 0)) return false;
    }
    return true;
}

bool hlsl_cbuffer_field_matches(const TempVariable *field, const SerializedVariable *variable,
                          const DecodedVariableLayout *decoded) {
    return variable->name && strcmp(variable->name, field->name) == 0 &&
        decoded->scalar_type == field->type && decoded->byte_offset == field->byte_offset &&
        decoded->rows == field->rows && decoded->columns == field->dim &&
        decoded->is_matrix == (field->is_matrix != 0) &&
        decoded->array_size == field->matrix_array_size &&
        parameter_layout_byte_size(decoded) == field->byte_size;
}

/* Order declarations without moving fields or their parallel use records.
 * Stage-specific fields may precede common fields at lower byte offsets. */
bool hlsl_global_field_order(const HLSLCBufferLayout *layout, int order[HLSL_GLOBAL_FIELD_LIMIT]) {
    if (!layout || !order || layout->variable_count < 1 || layout->variable_count > HLSL_GLOBAL_FIELD_LIMIT ||
        layout->variable_alloc < layout->variable_count || !layout->variables) return false;
    for (int field = 0; field < layout->variable_count; ++field) {
        const uint32_t offset = layout->variables[field].byte_offset;
        int place = field;
        while (place && layout->variables[order[place - 1]].byte_offset > offset) {
            order[place] = order[place - 1]; --place;
        }
        if (place && layout->variables[order[place - 1]].byte_offset == offset) return false;
        order[place] = field;
    }
    return true;
}

static bool global_metadata_complete(
    const HLSLEmitterContext *ctx, const HLSLCBufferLayout *layout) {
    bool shell = false;
    const SerializedProgramParameters *sources[2] = {ctx->params, ctx->common_params};
    for (unsigned source = 0; source < 2; ++source) {
        const SerializedProgramParameters *parameters = sources[source];
        if (!parameters) {
            continue;
        }
        for (int buffer = 0; buffer < parameters->cb_count; ++buffer) {
            const SerializedConstantBuffer *metadata = &parameters->constant_buffers[buffer];
            if (strcmp(metadata->name, "$Globals")) {
                continue;
            }
            if (metadata->struct_count) {
                return false;
            }
            if (metadata->role == SERIALIZED_CBUFFER_NAMED &&
                (!metadata->has_is_partial || !metadata->is_partial)) {
                if (metadata->size != layout->reflection_size_bytes) {
                    return false;
                }
                shell = true;
            }
            for (int variable = 0; variable < metadata->var_count; ++variable) {
                DecodedVariableLayout decoded;
                if (!parameter_layout_decode(
                        parameters, &metadata->variables[variable], &decoded)) {
                    return false;
                }
                bool found = false;
                for (int field = 0; field < layout->variable_count; ++field) {
                    if (hlsl_cbuffer_field_matches(
                            &layout->variables[field], &metadata->variables[variable], &decoded)) {
                        if (found) {
                            return false;
                        }
                        found = true;
                    }
                }
                if (!found) {
                    return false;
                }
            }
        }
    }
    return shell;
}

static bool global_declarations_complete(
    const HLSLEmitterContext *ctx, const HLSLCBufferLayout *layout) {
    if (!ctx->global_declarations) {
        return true;
    }
    size_t count = 0;
    const HLSLGlobalDeclarationField *fields =
        hlsl_global_declarations_fields(ctx->global_declarations, &count);
    if (count != (size_t)layout->variable_count || !fields) {
        return false;
    }
    for (size_t declaration = 0; declaration < count; ++declaration) {
        if (!fields[declaration].current_authority) {
            return false;
        }
        bool found = false;
        for (int field = 0; field < layout->variable_count; ++field) {
            if (!strcmp(fields[declaration].name, layout->variables[field].name)) {
                if (found) {
                    return false;
                }
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool global_fields_supported(const HLSLEmitterContext *ctx,
    int index,
    bool packed,
    uint32_t *prefix_rows,
    uint32_t *suffix_rows) {
    if (!ctx || !ctx->program || index != 0 || ctx->program->cbuffer_count != 1 ||
        ctx->cbuffer_layout_count != 1 || !ctx->high_level_packed_outputs ||
        ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
        ctx->program->program_type != DXBC_PROGRAM_TYPE_VERTEX ||
        !hlsl_packed_output_candidate(ctx->program)) return false;
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[0];
    if (!ctx->program->cbuffers || ctx->program->cbuffers[0].reg_idx ||
        ctx->program->cbuffers[0].size != layout->row_count || ctx->program->cbuffers[0].dynamic_indexed ||
        !layout->is_globals || layout->is_unity_builtin || layout->reg ||
        !layout->serialized_name || strcmp(layout->serialized_name, "$Globals") ||
        !layout->declaration_name || strcmp(layout->declaration_name, "_Globals") ||
        !layout->has_serialized_authority || !layout->has_reflection_size_authority ||
        layout->raw_storage || layout->row_struct_storage || layout->omit_declaration ||
        layout->row_count < 1 || layout->row_count >= CBUFFER_INVENTORY_ROW_LIMIT ||
        (layout->reflection_size_bytes & 15u) ||
        (packed ? layout->reflection_size_bytes < (uint32_t)layout->row_count * 16u :
            layout->reflection_size_bytes <= (uint32_t)layout->row_count * 16u) ||
        layout->reflection_size_bytes > CBUFFER_INVENTORY_ROW_LIMIT * 16u ||
        layout->variable_count < 1 || layout->variable_count > 128 ||
        layout->variable_alloc < layout->variable_count || !layout->variables ||
        layout->use_alloc < layout->variable_count || !layout->uses ||
        layout->projection_status != DXBC_CBUFFER_PROJECTION_EXACT ||
        !layout->projection.saw_access || layout->projection.saw_dynamic_access ||
        layout->projection.saw_padding_access ||
        (packed ? !name_available(ctx, layout, NULL, HLSL_GLOBAL_PREFIX_IDENTIFIER) ||
            !name_available(ctx, layout, NULL, HLSL_GLOBAL_SUFFIX_IDENTIFIER) :
            !name_available(ctx, layout, NULL, HLSL_GLOBAL_TAIL_IDENTIFIER))) return false;
    int order[HLSL_GLOBAL_FIELD_LIMIT];
    if (packed && !hlsl_global_field_order(layout, order)) return false;
    uint32_t last_named_row = 0, cursor = layout->variables[packed ? order[0] : 0].byte_offset;
    const uint32_t first_offset = cursor;
    if (packed && (first_offset & 15u)) return false;
    bool saw_packed = false;
    for (int variable = 0; variable < layout->variable_count; ++variable) {
        const int field_index = packed ? order[variable] : variable;
        const TempVariable *field = &layout->variables[field_index];
        if ((field->authority != 1 && field->authority != 2) || !layout->uses[field_index].referenced ||
            field->type || field->is_matrix || field->matrix_array_size || field->row_major ||
            field->rows != 1 || (field->dim != 1 && field->dim != 4) || field->byte_size != field->dim * 4u ||
            (field->byte_offset & (packed ? 3u : 15u)) ||
            field->byte_offset >= (uint32_t)layout->row_count * 16u ||
            field->reg_offset != field->byte_offset / 16u ||
            !name_available(ctx, layout, field, field->name) ||
            !hlsl_material_metadata_unique(ctx, layout, field)) return false;
        if (packed) {
            if (ctx->omit_unity_builtin_declarations &&
                (hlsl_unity_include_global_alias_status(field) != 0 ||
                 is_unity_builtin_variable_ex(field->name, ctx->common_params))) return false;
            const uint32_t aligned = field->dim == 4 ? (cursor + 15u) / 16u * 16u : cursor;
            if (field->byte_offset != aligned) return false;
            saw_packed |= (field->byte_offset & 15u) != 0;
            cursor = aligned + field->byte_size;
        }
        if (field->reg_offset > last_named_row) last_named_row = field->reg_offset;
    }
    if (last_named_row != (uint32_t)layout->row_count - 1u ||
        (packed && (!saw_packed || (cursor + 15u) / 16u != (uint32_t)layout->row_count))) return false;
    if (!global_metadata_complete(ctx, layout) || !global_declarations_complete(ctx, layout)) {
        return false;
    }
    if (prefix_rows) *prefix_rows = first_offset / 16u;
    if (suffix_rows) *suffix_rows = layout->reflection_size_bytes / 16u - (uint32_t)layout->row_count;
    return true;
}

bool hlsl_global_tail_supported(const HLSLEmitterContext *ctx, int index) {
    return global_fields_supported(ctx, index, false, NULL, NULL);
}

bool hlsl_global_packing_supported(const HLSLEmitterContext *ctx, int index,
    uint32_t *prefix_rows, uint32_t *suffix_rows) {
    return global_fields_supported(ctx, index, true, prefix_rows, suffix_rows);
}

static bool layout_has_matrix_field(const HLSLCBufferLayout *layout) {
    if (!layout || layout->variable_count < 0 ||
        layout->variable_count > CBUFFER_INVENTORY_ROW_LIMIT ||
        (layout->variable_count && !layout->variables)) return false;
    for (int field = 0; field < layout->variable_count; ++field)
        if (layout->variables[field].is_matrix) return true;
    return false;
}

/* Require every serialized field to survive in the actual emitted inventory,
 * including unused fields. A filtered field or anonymous tail remains a gap.
 * Scalar/vector row alignment is supplied by actual packoffset qualifiers,
 * except the separately proved HULL b0 scalar at offset zero, whose one
 * 16-byte row uses implicit cbuffer rounding. Generated dummy variables do
 * not supply coverage. A complete FLOAT4x4 occupies four rows but one actual
 * declaration, only within the independently replayed packed-output unit.
 * Sibling declaration authority is excluded. */
bool hlsl_source_quality_named_cbuffer_supported(const HLSLEmitterContext *ctx,
    int index, uint8_t *shell_authority) {
    if (!ctx || !ctx->program || !ctx->program->cbuffers || index < 0 ||
        index >= ctx->cbuffer_layout_count || index >= HLSL_MAX_CBUFFER_LAYOUTS ||
        index >= ctx->program->cbuffer_count) return false;
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[index];
    if (ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE || layout->is_globals ||
        layout->is_unity_builtin || layout->omit_declaration || layout->raw_storage ||
        layout->row_struct_storage || !layout->serialized_name || !layout->declaration_name ||
        strcmp(layout->serialized_name, layout->declaration_name) ||
        !name_available(ctx, layout, NULL, layout->declaration_name) ||
        !layout->has_serialized_authority || !layout->has_reflection_size_authority ||
        layout->reg < 0 || layout->reg >= HLSL_SM5_CBUFFER_REGISTER_COUNT ||
        layout->reg != ctx->program->cbuffers[index].reg_idx || layout->row_count <= 0 ||
        layout->row_count > CBUFFER_INVENTORY_ROW_LIMIT ||
        ctx->program->cbuffers[index].dynamic_indexed ||
        layout->reflection_size_bytes != (uint32_t)layout->row_count * 16u ||
        layout->variable_count <= 0 || layout->variable_count > layout->row_count || !layout->variables ||
        layout->projection_status != DXBC_CBUFFER_PROJECTION_EXACT ||
        layout->projection.saw_dynamic_access || layout->projection.saw_padding_access) return false;
    const bool matrix_inventory = layout_has_matrix_field(layout);
    if (matrix_inventory ? !hlsl_source_quality_packed_output_guard_active(ctx)
                         : layout->variable_count != layout->row_count) return false;
    uint32_t cursor = 0;
    for (int variable = 0; variable < layout->variable_count; ++variable) {
        const TempVariable *field = &layout->variables[variable];
        if ((field->authority != 1 && field->authority != 2) ||
            !name_available(ctx, layout, field, field->name) || field->type ||
            field->matrix_array_size || field->row_major || field->byte_offset != cursor ||
            field->reg_offset != cursor / 16u) return false;
        const uint32_t extent = field->is_matrix ? 64u : 16u;
        if (field->is_matrix ? field->rows != 4 || field->dim != 4 || field->byte_size != 64
                             : field->rows != 1 || !field->dim || field->dim > 4 ||
                                   field->byte_size != field->dim * 4u) return false;
        if (cursor > layout->reflection_size_bytes ||
            extent > layout->reflection_size_bytes - cursor) return false;
        cursor += extent;
    }
    if (cursor != layout->reflection_size_bytes) return false;
    uint8_t authority = 0;
    uint8_t field_authorities[CBUFFER_INVENTORY_ROW_LIMIT] = {0};
    const SerializedProgramParameters *sources[] = {ctx->params, ctx->common_params};
    for (unsigned source = 0; source < 2; ++source) {
        const SerializedProgramParameters *parameters = sources[source];
        if (!parameters) continue;
        if (parameters->cb_count < 0 ||
            (parameters->cb_count && !parameters->constant_buffers)) return false;
        for (int buffer = 0; buffer < parameters->cb_count; ++buffer) {
            const SerializedConstantBuffer *metadata = &parameters->constant_buffers[buffer];
            if (!metadata->name || strcmp(metadata->name, layout->serialized_name)) continue;
            if (metadata->struct_count || metadata->var_count < 0 ||
                metadata->var_count > CBUFFER_INVENTORY_ROW_LIMIT ||
                (metadata->var_count && !metadata->variables)) return false;
            if (metadata->role == SERIALIZED_CBUFFER_NAMED &&
                (!metadata->has_is_partial || !metadata->is_partial) &&
                metadata->size == layout->reflection_size_bytes && !authority)
                authority = (uint8_t)(source + 1u);
            for (int member = 0; member < metadata->var_count; ++member) {
                DecodedVariableLayout decoded;
                const SerializedVariable *variable = &metadata->variables[member];
                if (!parameter_layout_decode(parameters, variable, &decoded) ||
                    (decoded.byte_offset & 15u)) return false;
                uint32_t field = decoded.byte_offset / 16u;
                if (matrix_inventory) {
                    field = UINT32_MAX;
                    for (int candidate = 0; candidate < layout->variable_count; ++candidate) {
                        if (!hlsl_cbuffer_field_matches(&layout->variables[candidate], variable, &decoded)) continue;
                        if (field != UINT32_MAX) return false;
                        field = (uint32_t)candidate;
                    }
                }
                if (field >= (uint32_t)layout->variable_count ||
                    !hlsl_cbuffer_field_matches(&layout->variables[field], variable, &decoded)) return false;
                if (field_authorities[field] & (1u << source)) return false;
                field_authorities[field] |= (uint8_t)(1u << source);
            }
        }
    }
    if (!authority) return false;
    for (int variable = 0; variable < layout->variable_count; ++variable)
        if (!(field_authorities[variable] & (1u << (layout->variables[variable].authority - 1u))))
            return false;
    if (shell_authority) *shell_authority = authority;
    return true;
}

bool hlsl_source_quality_local_cbuffer_supported(const HLSLEmitterContext *ctx, int index) {
    if (!ctx || !ctx->program || !ctx->cbuffer_layouts_built || index < 0 ||
        index >= ctx->cbuffer_layout_count || ctx->cbuffer_layout_count != ctx->program->cbuffer_count)
        return false;
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[index];
    return (layout->compact_global_layout && !layout->omit_declaration && !layout->raw_storage &&
            !layout->row_struct_storage && !layout->is_unity_builtin &&
            (!layout->compact_global_tail || (hlsl_source_quality_packed_output_guard_active(ctx) &&
                hlsl_global_tail_supported(ctx, index))) &&
            (!layout->compact_global_packing || (hlsl_source_quality_packed_output_guard_active(ctx) &&
                hlsl_global_packing_supported(ctx, index, NULL, NULL)))) ||
        hlsl_source_quality_named_cbuffer_supported(ctx, index, NULL);
}

bool hlsl_source_quality_local_cbuffer_complete(const HLSLEmitterContext *ctx, int index) {
    if (!hlsl_source_quality_local_cbuffer_supported(ctx, index)) return false;
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[index];
    return layout->compact_global_layout || (layout->source_quality_begin_emitted &&
        layout->source_quality_end_emitted &&
        layout->source_quality_fields_emitted == layout->variable_count);
}

bool hlsl_source_quality_cbuffer_inventory_supported(const HLSLEmitterContext *ctx) {
    if (!ctx || !ctx->program || !ctx->cbuffer_layouts_built ||
        ctx->cbuffer_layout_count != ctx->program->cbuffer_count) return false;
    for (int index = 0; index < ctx->cbuffer_layout_count; ++index)
        if (!hlsl_source_quality_local_cbuffer_supported(ctx, index)) return false;
    return true;
}

bool hlsl_source_quality_cbuffer_inventory_complete(const HLSLEmitterContext *ctx) {
    if (!hlsl_source_quality_cbuffer_inventory_supported(ctx)) return false;
    for (int index = 0; index < ctx->cbuffer_layout_count; ++index)
        if (!hlsl_source_quality_local_cbuffer_complete(ctx, index)) return false;
    return true;
}

bool hlsl_source_quality_cbuffer_syntax(HLSLEmitterContext *ctx, int index,
    HLSLSourceQualityCBufferDeclarationKind kind, int field_index, uint8_t authority) {
    if (!ctx || index < 0 || index >= ctx->cbuffer_layout_count ||
        index >= HLSL_MAX_CBUFFER_LAYOUTS || kind < HLSL_SOURCE_CBUFFER_BEGIN ||
        kind > HLSL_SOURCE_CBUFFER_END || (authority != 1 && authority != 2) ||
        (kind == HLSL_SOURCE_CBUFFER_FIELD
             ? field_index < 0 || field_index >= ctx->cbuffer_layouts[index].variable_count ||
                   !ctx->cbuffer_layouts[index].variables
             : field_index != -1)) {
        if (ctx)
            hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                           HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (!ctx->source_quality_analysis) return true;
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[index];
    HLSLSourceQualityFacts facts;
    hlsl_source_quality_facts_init(&facts);
    facts.known = true;
    facts.cbuffer_declaration_kind = kind;
    facts.cbuffer_binding_register = (uint32_t)layout->reg;
    facts.cbuffer_declaration_authority = authority;
    facts.cbuffer_byte_size = layout->reflection_size_bytes;
    if (kind == HLSL_SOURCE_CBUFFER_FIELD) {
        const TempVariable *field = &layout->variables[field_index];
        facts.cbuffer_field_index = (uint32_t)field_index;
        facts.cbuffer_byte_offset = field->byte_offset;
        facts.cbuffer_byte_size = field->byte_size;
        if (layout_has_matrix_field(layout)) {
            if (!hlsl_source_quality_named_cbuffer_supported(ctx, index, NULL)) {
                hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                               HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
                return false;
            }
            facts.cbuffer_field_rows = (uint8_t)field->rows;
            facts.cbuffer_field_columns = (uint8_t)field->dim;
            facts.cbuffer_field_is_matrix = field->is_matrix != 0;
        }
    }
    if (hlsl_source_quality_analysis_emission(ctx->source_quality_analysis, &facts)) return true;
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                   HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    return false;
}
