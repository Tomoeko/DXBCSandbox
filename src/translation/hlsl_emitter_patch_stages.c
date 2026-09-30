// SPDX-License-Identifier: GPL-3.0-only
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_global_declarations.h"
#include "translation/hlsl_source_identifier.h"
#include "translation/usil_validation.h"
#include "hlsl_patch_constants.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { PATCH_PHASE_LIMIT = 32, PATCH_INSTRUCTION_LIMIT = 64, PATCH_NAME_COUNT = 10 };
enum { POINT_TYPE, FACTOR_TYPE, PATCH_FUNCTION, PATCH_VARIABLE, FACTOR_VARIABLE,
       POINT_FIELD, INSTANCE_INDEX, POINT_INDEX, LOCATION, OUTPUT_VALUE };
typedef struct {
    HLSLPatchLayout layout;
    HLSLDomainShape shape;
    int phase, phase_field[PATCH_PHASE_LIMIT], producer[HLSL_PATCH_CONSTANT_LIMIT];
    uint32_t declared_reads[PATCH_PHASE_LIMIT];
    uint8_t point_read_masks[PATCH_PHASE_LIMIT];
    HLSLInstructionOwners index_transports;
    char names[PATCH_NAME_COUNT][96], field_names[HLSL_PATCH_CONSTANT_LIMIT][96];
} PatchPlan;

static const char *partition_name(DXBCTessellatorPartitioning value) {
    switch (value) {
    case DXBC_TESSELLATOR_PARTITIONING_INTEGER: return "integer";
    case DXBC_TESSELLATOR_PARTITIONING_POW2: return "pow2";
    case DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD: return "fractional_odd";
    case DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN: return "fractional_even";
    default: return NULL;
    }
}
static bool position(const DXBCSignatureElement *e) {
    return e && !e->register_id && !e->semantic_index && e->system_value == 1 &&
        e->component_type == 3 && e->mask == 15 && !e->min_precision && !e->stream_index &&
        !e->interpolation_mode && !dxbc_ascii_strcasecmp(dxbc_signature_semantic_name(e), "SV_Position");
}
static bool ordinary_contract(const USILProgram *p, PatchPlan *plan, bool hull) {
    if (!p || !p->has_stage_contract || !p->has_parsed_signature_authority || !p->tessellation.valid ||
        p->program_type != (hull ? DXBC_PROGRAM_TYPE_HULL : DXBC_PROGRAM_TYPE_DOMAIN) ||
        p->shader_model_major != 5 || p->shader_model_minor ||
        !memchr(p->shader_type_model, 0, sizeof(p->shader_type_model)) ||
        strcmp(p->shader_type_model, hull ? "hs_5_0" : "ds_5_0") ||
        !hlsl_domain_shape(p->tessellation.domain, &plan->shape) ||
        !hlsl_patch_scalar_layout(p, &plan->layout) || !plan->layout.has_custom ||
        !p->tessellation.input_control_point_count || p->tessellation.input_control_point_count > 32 ||
        p->input_count != 1 || p->output_count != 1 || p->input_alloc < 1 || p->output_alloc < 1 ||
        !p->inputs || !p->outputs || !position(p->inputs) || !position(p->outputs) ||
        p->outputs[0].rw_mask || p->cbuffer_count || p->texture_count || p->sampler_count || p->uav_count ||
        p->indexable_temp_count || p->icb_value_count || p->geometry.valid || p->compute.valid ||
        (p->has_global_flags && p->global_flags != 1) || p->temp_count < 0 ||
        p->temp_count > HLSL_SM5_TEMP_REGISTER_COUNT || p->instruction_count < 2 ||
        p->instruction_count > PATCH_INSTRUCTION_LIMIT || p->instruction_alloc < p->instruction_count || !p->instructions ||
        p->signature_declaration_count < 1 || p->signature_declaration_count > 1024 ||
        p->signature_declaration_alloc < p->signature_declaration_count ||
        !p->signature_declarations || !usil_signature_authority_is_valid(p)) return false;
    for (int index = 0; index < p->instruction_count; ++index) {
        const USILInstruction *i = &p->instructions[index];
        if (i->precise_mask || i->saturate || i->condition_test != DXBC_INSTRUCTION_TEST_NONE ||
            i->has_resource_dimension || i->resource_dimension[0] || i->resource_stride || i->has_texel_offset ||
            i->has_resource_return_types || i->resource_info_return_type || i->sample_info_return_type ||
            i->geometry_effect || i->geometry_stream_id || i->geometry_stream_explicit ||
            !usil_instruction_shape_valid(p, i) || (i->opcode != USIL_OP_RET && !hlsl_expression_effects_supported(p, i)) ||
            (i->opcode != USIL_OP_MOV && i->opcode != USIL_OP_ADD && i->opcode != USIL_OP_MUL &&
             i->opcode != USIL_OP_MAD && i->opcode != USIL_OP_MIN && i->opcode != USIL_OP_MAX && i->opcode != USIL_OP_RET)) return false;
        for (int operand = 1; operand < i->operand_count; ++operand) {
            const DXBCOperand *source = &i->operands[operand];
            if ((source->has_abs || source->has_neg) && !source->extended_token_count) return false;
        }
    }
    return true;
}
static int phase_owner(const USILProgram *p, uint32_t raw) {
    int owner = -1;
    for (size_t phase = 0; phase < p->tessellation.phase_count; ++phase) {
        const USILHullPhase *s = &p->tessellation.phases[phase];
        if (raw >= s->first_source_instruction_index && raw < s->end_source_instruction_index) {
            if (owner >= 0) return -1;
            owner = (int)phase;
        }
    }
    return owner;
}
static bool hull_contract(const USILProgram *p, PatchPlan *plan) {
    if (!ordinary_contract(p, plan, true) || p->inputs[0].rw_mask != 15 ||
        p->tessellation.output_control_point_count != p->tessellation.input_control_point_count ||
        !partition_name(p->tessellation.partitioning) ||
        (p->tessellation.domain == DXBC_TESSELLATOR_DOMAIN_ISOLINE ?
            p->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_LINE :
            (p->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
             p->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CCW)) ||
        strcmp(dxbc_signature_semantic_name(p->inputs), dxbc_signature_semantic_name(p->outputs)) ||
        !p->tessellation.has_max_tessellation_factor || !p->tessellation.phases ||
        !p->tessellation.phase_count || p->tessellation.phase_count > PATCH_PHASE_LIMIT ||
        p->tessellation.phase_capacity < p->tessellation.phase_count ||
        p->index_range_count < 0 || p->index_range_count > PATCH_PHASE_LIMIT || p->index_range_alloc < p->index_range_count ||
        (p->index_range_count && !p->index_ranges) || !usil_hull_phase_temp_registers_are_valid(p)) return false;
    float maximum;
    memcpy(&maximum, &p->tessellation.max_tessellation_factor_bits, 4);
    if (!isfinite(maximum) || maximum < 1 || maximum > 64) return false;
    for (unsigned reg = 0; reg < HLSL_PATCH_CONSTANT_LIMIT; ++reg) plan->producer[reg] = -1;
    for (unsigned phase = 0; phase < PATCH_PHASE_LIMIT; ++phase) plan->phase_field[phase] = -1;
    bool join = false;
    int next = 0;
    for (size_t phase = 0; phase < p->tessellation.phase_count; ++phase) {
        const USILHullPhase *s = &p->tessellation.phases[phase];
        if ((s->kind != DXBC_HULL_PHASE_FORK && s->kind != DXBC_HULL_PHASE_JOIN) ||
            (join && s->kind != DXBC_HULL_PHASE_JOIN) || !s->instance_count || s->instance_count > 32 ||
            (!s->instance_count_declared && s->instance_count != 1) ||
            (s->kind == DXBC_HULL_PHASE_JOIN && s->instance_count != 1) ||
            s->first_instruction_index != next || s->end_instruction_index <= next ||
            s->end_instruction_index > p->instruction_count ||
            s->marker_source_instruction_index >= s->first_source_instruction_index ||
            s->first_source_instruction_index != s->marker_source_instruction_index + 1u ||
            s->first_source_instruction_index >= s->end_source_instruction_index ||
            (phase && s->marker_source_instruction_index != p->tessellation.phases[phase - 1].end_source_instruction_index)) return false;
        join |= s->kind == DXBC_HULL_PHASE_JOIN;
        for (int i = next; i < s->end_instruction_index; ++i)
            if (p->instructions[i].source_instruction_index < s->first_source_instruction_index ||
                p->instructions[i].source_instruction_index >= s->end_source_instruction_index ||
                (i > next && p->instructions[i].source_instruction_index <= p->instructions[i - 1].source_instruction_index) ||
                (p->instructions[i].opcode == USIL_OP_RET && i + 1 != s->end_instruction_index)) return false;
        if (!hlsl_hull_phase_return_owned(p, s->end_instruction_index - 1)) return false;
        next = s->end_instruction_index;
    }
    if (next != p->instruction_count) return false;
    uint32_t outputs = 0, instances = 0, point_inputs = 0;
    for (int index = 0; index < p->signature_declaration_count; ++index) {
        const USILSignatureDeclaration *d = &p->signature_declarations[index];
        int phase = phase_owner(p, d->source_instruction_index);
        if (phase < 0 || d->has_interpolation || d->stream_index || d->interpolation_mode) return false;
        const USILHullPhase *s = &p->tessellation.phases[phase];
        if (d->operand_type == OPERAND_TYPE_FORK_INSTANCE_ID) {
            if (s->kind != DXBC_HULL_PHASE_FORK || s->instance_count <= 1 || (instances & (UINT32_C(1) << phase)) ||
                d->kind != USIL_SIGNATURE_DECL_INPUT || d->has_signature_register || d->mask ||
                d->has_array_element_count || d->has_system_value) return false;
            instances |= UINT32_C(1) << phase;
        } else if (d->operand_type == OPERAND_TYPE_INPUT_CONTROL_POINT) {
            if ((point_inputs & (UINT32_C(1) << phase)) || d->kind != USIL_SIGNATURE_DECL_INPUT ||
                d->has_system_value || !d->has_array_element_count ||
                d->array_element_count != p->tessellation.input_control_point_count || !d->has_signature_register ||
                d->register_id || !d->mask || (d->mask & ~15u)) return false;
            point_inputs |= UINT32_C(1) << phase;
            plan->point_read_masks[phase] = d->mask;
        } else if (d->operand_type == OPERAND_TYPE_INPUT_PATCH_CONSTANT) {
            if (s->kind != DXBC_HULL_PHASE_JOIN || d->kind != USIL_SIGNATURE_DECL_INPUT ||
                d->has_system_value || d->has_array_element_count || !d->has_signature_register ||
                d->mask != 1 || d->register_id >= plan->layout.row_count ||
                (plan->declared_reads[phase] & (UINT32_C(1) << d->register_id))) return false;
            plan->declared_reads[phase] |= UINT32_C(1) << d->register_id;
        } else if (d->operand_type == OPERAND_TYPE_OUTPUT) {
            if (!d->has_signature_register || d->has_array_element_count || d->mask != 1 ||
                d->register_id >= plan->layout.row_count || (outputs & (UINT32_C(1) << d->register_id))) return false;
            int field = plan->layout.register_field[d->register_id];
            const HLSLPatchField *f = &plan->layout.fields[field];
            if (f->kind == HLSL_PATCH_CUSTOM ? (d->kind != USIL_SIGNATURE_DECL_OUTPUT || d->has_system_value) :
                (d->kind != USIL_SIGNATURE_DECL_OUTPUT_SIV || !d->has_system_value ||
                 d->system_value_name != (f->kind == HLSL_PATCH_OUTER ? plan->shape.raw_outer_siv_names[plan->layout.register_element[d->register_id]] :
                    plan->shape.raw_inner_siv_names[plan->layout.register_element[d->register_id]]))) return false;
            if ((plan->phase_field[phase] >= 0 && plan->phase_field[phase] != field) || s->instance_count != f->count) return false;
            plan->phase_field[phase] = field;
            plan->producer[d->register_id] = phase;
            outputs |= UINT32_C(1) << d->register_id;
        } else return false;
    }
    if (outputs != (plan->layout.row_count == 32 ? UINT32_MAX : (UINT32_C(1) << plan->layout.row_count) - 1u)) return false;
    uint32_t ranges = 0;
    for (int index = 0; index < p->index_range_count; ++index) {
        const USILIndexRange *r = &p->index_ranges[index];
        if (r->hull_phase_index < 0 || r->hull_phase_index >= (int)p->tessellation.phase_count ||
            (ranges & (UINT32_C(1) << r->hull_phase_index))) return false;
        const int phase = r->hull_phase_index, field = plan->phase_field[phase];
        if (field < 0) return false;
        const HLSLPatchField *f = &plan->layout.fields[field];
        const USILHullPhase *s = &p->tessellation.phases[phase];
        const DXBCOperand *o = &r->operand;
        if (s->kind != DXBC_HULL_PHASE_FORK || f->count <= 1 || r->register_count != f->count ||
            r->source_instruction_index < s->first_source_instruction_index || r->source_instruction_index >= s->end_source_instruction_index ||
            o->type != OPERAND_TYPE_OUTPUT || !hlsl_lift_operand_is_plain(o) || o->extended_tokens ||
            o->register_index_dim != 1 || o->register_index != f->first_register ||
            !o->index_has_immediate[0] || o->index_representations[0] || o->index_value_exceeds_int[0] ||
            o->index_values[0] != f->first_register || usil_operand_destination_lane_mask(o) != 1) return false;
        ranges |= UINT32_C(1) << phase;
    }
    for (size_t phase = 0; phase < p->tessellation.phase_count; ++phase) {
        int field = plan->phase_field[phase];
        if (field < 0) return false;
        const HLSLPatchField *f = &plan->layout.fields[field];
        if (!!(ranges & (UINT32_C(1) << phase)) != (f->count > 1) ||
            !!(instances & (UINT32_C(1) << phase)) != (f->count > 1)) return false;
        for (unsigned reg = 0; reg < plan->layout.row_count; ++reg) {
            if (reg >= f->first_register && reg < f->first_register + f->count && plan->producer[reg] != (int)phase) return false;
            if (plan->declared_reads[phase] & (UINT32_C(1) << reg)) {
                const int producer = plan->producer[reg];
                if (producer < 0 || producer >= (int)phase || p->tessellation.phases[producer].kind != DXBC_HULL_PHASE_FORK) return false;
            }
        }
    }
    return true;
}
static bool domain_contract(const USILProgram *p, PatchPlan *plan) {
    if (!ordinary_contract(p, plan, false) || p->tessellation.output_control_point_count || p->tessellation.phase_count ||
        p->tessellation.partitioning || p->tessellation.output_primitive || p->tessellation.has_max_tessellation_factor ||
        p->tessellation.max_tessellation_factor_bits || p->index_range_count) return false;
    unsigned location = 0, points = 0, output = 0;
    uint32_t reads = 0;
    for (int index = 0; index < p->signature_declaration_count; ++index) {
        const USILSignatureDeclaration *d = &p->signature_declarations[index];
        if (d->has_interpolation || d->has_system_value || d->stream_index || d->interpolation_mode) {
            if (d->operand_type != OPERAND_TYPE_OUTPUT || d->kind != USIL_SIGNATURE_DECL_OUTPUT_SIV ||
                !d->has_system_value || d->system_value_name != 1 || d->has_interpolation || d->stream_index || d->interpolation_mode) return false;
        }
        if (d->operand_type == OPERAND_TYPE_DOMAIN_LOCATION) {
            if (location || d->kind != USIL_SIGNATURE_DECL_INPUT || d->has_signature_register || d->has_array_element_count ||
                !d->mask || (d->mask & ~((1u << plan->shape.coordinate_count) - 1u))) return false;
            location = d->mask;
        } else if (d->operand_type == OPERAND_TYPE_INPUT_CONTROL_POINT) {
            if (points++ || d->kind != USIL_SIGNATURE_DECL_INPUT || !d->has_signature_register || d->register_id ||
                !d->has_array_element_count || d->array_element_count != p->tessellation.input_control_point_count || d->mask != 15) return false;
        } else if (d->operand_type == OPERAND_TYPE_INPUT_PATCH_CONSTANT) {
            if (d->kind != USIL_SIGNATURE_DECL_INPUT || !d->has_signature_register || d->has_array_element_count ||
                d->mask != 1 || d->register_id >= plan->layout.row_count || (reads & (UINT32_C(1) << d->register_id))) return false;
            reads |= UINT32_C(1) << d->register_id;
        } else if (d->operand_type == OPERAND_TYPE_OUTPUT) {
            if (output++ || d->kind != USIL_SIGNATURE_DECL_OUTPUT_SIV || !d->has_signature_register || d->register_id ||
                d->has_array_element_count || d->mask != 15) return false;
        } else return false;
    }
    plan->declared_reads[0] = reads;
    plan->point_read_masks[0] = (uint8_t)location;
    return location && points && output && p->instructions[p->instruction_count - 1].opcode == USIL_OP_RET;
}
bool hlsl_high_level_hull_join_supported(const USILProgram *p, HLSLEmitMode mode) {
    PatchPlan plan = {0};
    return mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE && hull_contract(p, &plan);
}
bool hlsl_high_level_patch_domain_supported(const USILProgram *p, HLSLEmitMode mode) {
    PatchPlan plan = {0};
    return mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE && domain_contract(p, &plan);
}

static bool scalar_temp(const USILProgram *p, const DXBCOperand *o, bool dest) {
    return o->type == OPERAND_TYPE_TEMP && hlsl_lift_operand_is_plain(o) && !o->extended_tokens &&
        o->register_index_dim == 1 && o->register_index >= 0 && o->register_index < p->temp_count &&
        o->index_has_immediate[0] && !o->index_representations[0] && !o->index_value_exceeds_int[0] &&
        o->index_values[0] == (uint32_t)o->register_index &&
        (dest ? usil_operand_destination_lane_mask(o) == 1 : o->swizzle_mode == 2 && !o->swizzle[0]);
}
static bool claim_index(HLSLEmitterContext *ctx, PatchPlan *plan, int definition, int before) {
    const USILHullPhase *s = &ctx->program->tessellation.phases[plan->phase];
    for (unsigned depth = 0; depth < PATCH_INSTRUCTION_LIMIT; ++depth) {
        if (definition < s->first_instruction_index || definition >= before) return false;
        const USILInstruction *i = &ctx->program->instructions[definition];
        if (i->opcode != USIL_OP_MOV || i->operand_count != 2 || !scalar_temp(ctx->program, &i->operands[0], true) ||
            !hlsl_instruction_owners_add(&plan->index_transports, definition)) return false;
        const DXBCOperand *source = &i->operands[1];
        if (s->kind == DXBC_HULL_PHASE_FORK && source->type == OPERAND_TYPE_FORK_INSTANCE_ID &&
            hlsl_lift_operand_is_plain(source) && !source->extended_tokens && !source->register_index_dim &&
            source->swizzle_mode == 2 && !source->swizzle[0]) return true;
        if (!scalar_temp(ctx->program, source, false)) return false;
        before = definition;
        definition = hlsl_operand_definition(ctx, definition, 1, 0);
    }
    return false;
}
static bool destination_supported(HLSLEmitterContext *ctx, int index, void *opaque) {
    PatchPlan *plan = opaque;
    const DXBCOperand *o = &ctx->program->instructions[index].operands[0];
    if (ctx->program->program_type == DXBC_PROGRAM_TYPE_DOMAIN)
        return o->type == OPERAND_TYPE_OUTPUT && hlsl_lift_operand_is_plain(o) && !o->extended_tokens &&
            o->register_index_dim == 1 && !o->register_index && o->index_has_immediate[0] &&
            !o->index_representations[0] && !o->index_values[0] && !o->index_value_exceeds_int[0] &&
            usil_operand_destination_lane_mask(o) == 15;
    const HLSLPatchField *f = &plan->layout.fields[plan->phase_field[plan->phase]];
    if (o->type != OPERAND_TYPE_OUTPUT || o->register_index_dim != 1 || o->swizzle_mode || o->min_precision ||
        o->has_abs || o->has_neg || o->extended_token_count || o->extended_tokens || o->rel_op1 || o->rel_op2 ||
        usil_operand_destination_lane_mask(o) != 1 || o->register_index != f->first_register) return false;
    if (f->count == 1) return hlsl_lift_operand_is_plain(o) && o->index_has_immediate[0] &&
        !o->index_representations[0] && !o->index_value_exceeds_int[0] && o->index_values[0] == f->first_register;
    return o->rel_op0 && !o->index_value_exceeds_int[0] &&
        ((!f->first_register && o->index_representations[0] == 2 && !o->index_has_immediate[0] && !o->index_values[0]) ||
         (o->index_representations[0] == 3 && o->index_has_immediate[0] && o->index_values[0] == f->first_register)) &&
        scalar_temp(ctx->program, o->rel_op0, false) &&
        claim_index(ctx, plan, hlsl_relative_operand_definition(ctx, index, 0, 0), index);
}
static bool source_supported(HLSLEmitterContext *ctx, int index, int operand, void *opaque) {
    PatchPlan *plan = opaque;
    const USILProgram *p = ctx->program;
    const DXBCOperand *original = &p->instructions[index].operands[operand];
    DXBCOperand o = *original;
    if (!hlsl_float_source_modifier_supported(&o)) return false;
    o.has_abs = o.has_neg = false; o.extended_tokens = NULL; o.extended_token_count = 0;
    if (o.type == OPERAND_TYPE_INPUT_PATCH_CONSTANT) {
        if (!hlsl_patch_static_scalar_operand(&plan->layout, &o) || !hlsl_lift_operand_is_plain(&o) ||
            !(plan->declared_reads[plan->phase] & (UINT32_C(1) << o.register_index))) return false;
        return p->program_type == DXBC_PROGRAM_TYPE_DOMAIN ||
            (p->tessellation.phases[plan->phase].kind == DXBC_HULL_PHASE_JOIN &&
             plan->producer[o.register_index] < plan->phase && plan->producer[o.register_index] >= 0 &&
             p->tessellation.phases[plan->producer[o.register_index]].kind == DXBC_HULL_PHASE_FORK);
    }
    USILOperandUseInfo use;
    if (!usil_instruction_operand_use(p, &p->instructions[index], operand, &use) || use.use != USIL_OPERAND_USE_SOURCE) return false;
    if (o.type == OPERAND_TYPE_DOMAIN_LOCATION) {
        if (p->program_type != DXBC_PROGRAM_TYPE_DOMAIN || !hlsl_lift_operand_is_plain(&o) || o.register_index_dim || o.extended_tokens) return false;
        for (unsigned lane = 0; lane < 4; ++lane) if (use.source_lane_mask & (1u << lane)) {
            int selected = usil_operand_source_component(&o, (int)lane);
            if (selected < 0 || selected >= plan->shape.coordinate_count || !(plan->point_read_masks[0] & (1u << selected))) return false;
        }
        return true;
    }
    if (o.type != OPERAND_TYPE_INPUT_CONTROL_POINT || o.register_index_dim != 2 || o.min_precision ||
        o.rel_op1 || o.rel_op2 || o.index_representations[1] || !o.index_has_immediate[1] || o.index_values[1] || o.index_value_exceeds_int[1]) return false;
    for (unsigned lane = 0; lane < 4; ++lane) if (use.source_lane_mask & (1u << lane)) {
        int selected = usil_operand_source_component(&o, (int)lane);
        if (selected < 0 || selected >= 4 || (p->program_type == DXBC_PROGRAM_TYPE_HULL && !(plan->point_read_masks[plan->phase] & (1u << selected)))) return false;
    }
    if (p->program_type == DXBC_PROGRAM_TYPE_DOMAIN)
        return hlsl_lift_operand_is_plain(&o) && o.register_index == (int)o.index_values[0] &&
            o.index_has_immediate[0] && !o.index_representations[0] &&
            !o.index_value_exceeds_int[0] && o.index_values[0] < p->tessellation.input_control_point_count;
    return !o.register_index && p->tessellation.phases[plan->phase].kind == DXBC_HULL_PHASE_FORK &&
        p->tessellation.phases[plan->phase].instance_count <= p->tessellation.input_control_point_count &&
        o.index_representations[0] == 2 && !o.index_has_immediate[0] && !o.index_values[0] &&
        !o.index_value_exceeds_int[0] && o.rel_op0 && scalar_temp(p, o.rel_op0, false) &&
        claim_index(ctx, plan, hlsl_relative_operand_definition(ctx, index, operand, 0), index);
}
static ASTExpr *owned_atom(HLSLEmitterContext *ctx, const char *text, int index, int operand,
                           uint8_t lanes, unsigned natural, unsigned selected, uint64_t id) {
    ASTOperandProvenance origin;
    ast_operand_provenance_init(&origin);
    origin.complete = true; origin.value_role = AST_OPERAND_VALUE_LOGICAL; origin.logical_value_id = id;
    origin.natural_components = (uint8_t)natural; origin.result_components = natural ? 1 : 0;
    origin.selected_components[0] = (uint8_t)selected;
    origin.selection_role = natural <= 1 ? AST_COMPONENT_SELECTION_NONE : AST_COMPONENT_SELECTION_SEMANTIC;
    if (index >= 0) {
        origin.instruction_index = index; origin.source_instruction_index = ctx->program->instructions[index].source_instruction_index;
        origin.operand_index = operand; origin.destination_lanes = lanes;
    }
    return ast_create_emitter_operand_with_provenance(text, &origin);
}
static ASTExpr *source_expression(HLSLEmitterContext *ctx, int index, int operand, uint8_t lanes, void *opaque) {
    PatchPlan *plan = opaque;
    if (!source_supported(ctx, index, operand, opaque)) return NULL;
    const DXBCOperand *o = &ctx->program->instructions[index].operands[operand];
    char text[400];
    if (o->type == OPERAND_TYPE_INPUT_PATCH_CONSTANT) {
        int field = plan->layout.register_field[o->register_index];
        if (plan->layout.fields[field].count > 1) {
            if (!hlsl_format_checked(ctx, text, sizeof(text), "%s.%s[%u]", plan->names[FACTOR_VARIABLE], plan->field_names[field],
                                     (unsigned)plan->layout.register_element[o->register_index])) return NULL;
        } else if (!hlsl_format_checked(ctx, text, sizeof(text), "%s.%s", plan->names[FACTOR_VARIABLE], plan->field_names[field])) return NULL;
        return owned_atom(ctx, text, index, operand, lanes, 1, 0, UINT64_C(0x9000000000000000) | (unsigned)o->register_index);
    }
    const bool location = o->type == OPERAND_TYPE_DOMAIN_LOCATION;
    const unsigned natural = location ? plan->shape.coordinate_count : 4;
    char base[320];
    if (location) {
        if (!hlsl_copy_checked(ctx, base, sizeof(base), plan->names[LOCATION])) return NULL;
    } else if (ctx->program->program_type == DXBC_PROGRAM_TYPE_HULL) {
        if (!hlsl_format_checked(ctx, base, sizeof(base), "%s[%s].%s", plan->names[PATCH_VARIABLE], plan->names[INSTANCE_INDEX], plan->names[POINT_FIELD])) return NULL;
    } else if (!hlsl_format_checked(ctx, base, sizeof(base), "%s[%u].%s", plan->names[PATCH_VARIABLE],
                                  (unsigned)o->index_values[0], plan->names[POINT_FIELD])) return NULL;
    int selected[4], width = 0;
    bool same = true, identity = true;
    for (unsigned lane = 0; lane < 4; ++lane) if (lanes & (1u << lane)) {
        selected[width] = usil_operand_source_component(o, (int)lane);
        same &= !width || selected[width] == selected[0]; identity &= selected[width] == width;
        ++width;
    }
    const uint64_t id = (location ? UINT64_C(0xb000000000000000) : UINT64_C(0xa000000000000000)) |
        (ctx->program->program_type == DXBC_PROGRAM_TYPE_HULL ? (uint64_t)plan->phase << 32 : o->index_values[0]);
    if (identity && width == (int)natural) {
        ASTOperandProvenance origin;
        ast_operand_provenance_init(&origin);
        origin.complete = true; origin.value_role = AST_OPERAND_VALUE_LOGICAL; origin.logical_value_id = id;
        origin.natural_components = origin.result_components = (uint8_t)natural;
        for (unsigned c = 0; c < natural; ++c) origin.selected_components[c] = (uint8_t)c;
        origin.instruction_index = index; origin.source_instruction_index = ctx->program->instructions[index].source_instruction_index;
        origin.operand_index = operand; origin.destination_lanes = lanes;
        return ast_create_emitter_operand_with_provenance(base, &origin);
    }
    if (same) {
        if (!hlsl_format_checked(ctx, text, sizeof(text), "%s.%c", base, "xyzw"[selected[0]])) return NULL;
        return owned_atom(ctx, text, index, operand, lanes, natural, (unsigned)selected[0], id);
    }
    ASTExpr *arguments[4] = {0};
    for (int c = 0; c < width; ++c) {
        if (!hlsl_format_checked(ctx, text, sizeof(text), "%s.%c", base, "xyzw"[selected[c]])) goto bad;
        arguments[c] = owned_atom(ctx, text, index, operand, lanes, natural, (unsigned)selected[c], id);
        if (!arguments[c]) goto bad;
    }
    if (!hlsl_format_checked(ctx, text, sizeof(text), "float%d", width)) goto bad;
    ASTExpr *composition = ast_create_call(text, arguments, width);
    if (!composition) goto bad;
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true; origin.scalar_type = AST_SCALAR_FLOAT32;
    origin.components = (uint8_t)width; origin.logical_value_id = (uint64_t)index;
    origin.instruction_index = index; origin.source_instruction_index = ctx->program->instructions[index].source_instruction_index;
    origin.destination_lanes = lanes;
    if (!ast_set_logical_value_origin(composition, &origin)) { ast_free_expr(composition); return NULL; }
    return composition;
bad:
    for (int c = 0; c < width; ++c) ast_free_expr(arguments[c]);
    return NULL;
}
static bool append_destination(HLSLEmitterContext *ctx, int index, void *opaque) {
    PatchPlan *plan = opaque;
    if (!destination_supported(ctx, index, opaque)) return false;
    if (ctx->program->program_type == DXBC_PROGRAM_TYPE_DOMAIN) sb_append(ctx->sb, plan->names[OUTPUT_VALUE]);
    else {
        const int field = plan->phase_field[plan->phase];
        sb_appendf(ctx->sb, "%s.%s", plan->names[FACTOR_VARIABLE], plan->field_names[field]);
        if (plan->layout.fields[field].count > 1) sb_appendf(ctx->sb, "[%s]", plan->names[INSTANCE_INDEX]);
    }
    return sb_ok(ctx->sb);
}
static bool prepare(HLSLEmitterContext *ctx, PatchPlan *plan, int phase) {
    free_hlsl_ssa_graph(ctx); free_control_flow_graph(ctx); plan->phase = phase;
    const bool hull = ctx->program->program_type == DXBC_PROGRAM_TYPE_HULL;
    const int first = hull ? ctx->program->tessellation.phases[phase].first_instruction_index : 0;
    const int end = hull ? ctx->program->tessellation.phases[phase].end_instruction_index : ctx->program->instruction_count;
    if (!build_control_flow_graph_range(ctx, first, end) || !compute_dominance(&ctx->cfg) || !build_hlsl_ssa_graph(ctx)) return false;
    unsigned writes = 0;
    for (int index = first; index < end; ++index) {
        const USILInstruction *i = &ctx->program->instructions[index];
        if (i->operand_count && i->operands[0].type == OPERAND_TYPE_OUTPUT &&
            (++writes != 1 || !destination_supported(ctx, index, plan))) return false;
    }
    if (writes != 1) return false;
    /* Claim transport definitions from their typed consumers before checking
     * the omitted builtin MOVs. Builtin integer values are never float atoms. */
    for (int index = first; index < end; ++index) {
        const USILInstruction *i = &ctx->program->instructions[index];
        for (int operand = 1; operand < i->operand_count; ++operand) {
            DXBCOperandType type = i->operands[operand].type;
            if (type != OPERAND_TYPE_TEMP && type != OPERAND_TYPE_IMMEDIATE32 &&
                type != OPERAND_TYPE_FORK_INSTANCE_ID && !source_supported(ctx, index, operand, plan)) return false;
        }
    }
    for (int index = first; index < end; ++index) {
        if (hlsl_instruction_owners_contains(&plan->index_transports, index)) continue;
        const USILInstruction *i = &ctx->program->instructions[index];
        for (int operand = 1; operand < i->operand_count; ++operand)
            if (i->operands[operand].type == OPERAND_TYPE_FORK_INSTANCE_ID ||
                (i->operands[operand].type == OPERAND_TYPE_TEMP && hlsl_instruction_owners_contains(&plan->index_transports,
                hlsl_operand_definition(ctx, index, operand, 0)))) return false;
    }
    return true;
}

static bool allocate_names(HLSLEmitterContext *ctx, PatchPlan *plan) {
    static const char *const bases[PATCH_NAME_COUNT] = {"HullPoint", "HullFactors", "patchConstants", "patch", "factors",
        "clipPosition", "factorIndex", "pointIndex", "coordinates", "clipPositionResult"};
    static const char *const fixed[] = {"InputPatch", "OutputPatch", "struct", "for", "return", "const", "float", "float2", "float3", "float4", "uint",
        "asfloat", "abs", "mad", "min", "max", "SV_OutputControlPointID", "SV_DomainLocation", "domain", "partitioning", "outputtopology", "outputcontrolpoints", "patchconstantfunc", "maxtessfactor"};
    if (!hlsl_source_identifier_valid(ctx->entry_point_name) || ctx->reserved_preprocessor_identifier_count > 1024) return false;
    for (size_t i = 0; i < ctx->reserved_preprocessor_identifier_count; ++i) {
        const char *reserved = ctx->reserved_preprocessor_identifiers[i];
        if (!strcmp(reserved, ctx->entry_point_name) || !strcmp(reserved, dxbc_signature_semantic_name(ctx->program->inputs))) return false;
        for (size_t token = 0; token < sizeof(fixed) / sizeof(fixed[0]); ++token) if (!strcmp(reserved, fixed[token])) return false;
        for (unsigned field = 0; field < plan->layout.field_count; ++field)
            if (!strcmp(reserved, plan->layout.fields[field].semantic)) return false;
        for (unsigned field = 0; field < plan->layout.field_count; ++field) {
            const HLSLPatchField *f = &plan->layout.fields[field];
            char semantic[300];
            if (f->kind == HLSL_PATCH_CUSTOM &&
                (!hlsl_format_checked(ctx, semantic, sizeof(semantic), "%s%u", f->semantic, f->semantic_index) ||
                 !strcmp(reserved, semantic))) return false;
        }
    }
    const size_t original_count = ctx->reserved_preprocessor_identifier_count;
    const char *const *original = ctx->reserved_preprocessor_identifiers;
    const char **reserved = calloc(original_count + PATCH_NAME_COUNT + plan->layout.field_count, sizeof(*reserved));
    if (!reserved) return false;
    for (size_t i = 0; i < original_count; ++i) reserved[i] = original[i];
    ctx->reserved_preprocessor_identifiers = reserved;
    bool ok = true;
    for (int i = 0; i < PATCH_NAME_COUNT; ++i) {
        const bool domain = ctx->program->program_type == DXBC_PROGRAM_TYPE_DOMAIN;
        const char *base = domain && i == POINT_TYPE ? "DomainPoint" :
            domain && i == FACTOR_TYPE ? "DomainFactors" : bases[i];
        if (!hlsl_allocate_interface_name(ctx, base, plan->names[i])) { ok = false; break; }
        reserved[ctx->reserved_preprocessor_identifier_count++] = plan->names[i];
    }
    for (unsigned field = 0; ok && field < plan->layout.field_count; ++field) {
        const HLSLPatchField *f = &plan->layout.fields[field];
        char base[80];
        if (f->kind == HLSL_PATCH_CUSTOM) {
            if (!hlsl_format_checked(ctx, base, sizeof(base), "patchValues%u", field)) { ok = false; break; }
        } else if (!hlsl_copy_checked(ctx, base, sizeof(base), f->kind == HLSL_PATCH_OUTER ? "outer" : "inner")) { ok = false; break; }
        if (!hlsl_allocate_interface_name(ctx, base, plan->field_names[field])) { ok = false; break; }
        reserved[ctx->reserved_preprocessor_identifier_count++] = plan->field_names[field];
    }
    ctx->reserved_preprocessor_identifiers = original;
    ctx->reserved_preprocessor_identifier_count = original_count;
    free(reserved);
    return ok;
}
static bool begin_unit(HLSLEmitterContext *ctx, uint32_t id, HLSLSourceQualityUnitKind kind) {
    return !ctx->source_quality_analysis || hlsl_source_quality_analysis_begin_unit(ctx->source_quality_analysis, id, kind, true);
}
static bool observe_value(HLSLEmitterContext *ctx, const char *text, int instruction, uint64_t id, unsigned width) {
    ASTExpr *value = owned_atom(ctx, text, instruction, instruction >= 0 ? 1 : -1,
        instruction >= 0 ? usil_operand_destination_lane_mask(&ctx->program->instructions[instruction].operands[0]) : 0,
        width, 0, id);
    bool ok = value && hlsl_source_quality_observe_expression(ctx, value, instruction);
    ast_free_expr(value);
    return ok;
}
static void emit_structures(HLSLEmitterContext *ctx, const PatchPlan *plan) {
    sb_appendf(ctx->sb, "struct %s {\n    float4 %s : %s;\n};\n\nstruct %s {\n", plan->names[POINT_TYPE],
        plan->names[POINT_FIELD], dxbc_signature_semantic_name(ctx->program->inputs), plan->names[FACTOR_TYPE]);
    hlsl_source_quality_emission(ctx, 0, false, -1);
    for (unsigned field = 0; field < plan->layout.field_count; ++field) {
        const HLSLPatchField *f = &plan->layout.fields[field];
        sb_appendf(ctx->sb, "    float %s", plan->field_names[field]);
        if (f->count > 1) sb_appendf(ctx->sb, "[%u]", (unsigned)f->count);
        sb_appendf(ctx->sb, " : %s", f->semantic);
        if (f->kind == HLSL_PATCH_CUSTOM) sb_appendf(ctx->sb, "%u", f->semantic_index);
        sb_append(ctx->sb, ";\n");
        hlsl_source_quality_emission(ctx, 0, false, -1);
    }
    sb_append(ctx->sb, "};\n\n");
    hlsl_source_quality_emission(ctx, 0, false, -1);
}
static bool finish(HLSLEmitterContext *ctx, bool emitted, unsigned units) {
    if (emitted && ctx->expression_source_map) {
        ctx->expression_source_map->complete = true;
        emitted = hlsl_expression_source_map_matches(ctx->expression_source_map, ctx->program, ctx->sb->buf);
    }
    if (!emitted && (!ctx->diagnostic || ctx->diagnostic->status == HLSL_EMIT_STATUS_OK))
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, HLSL_EMIT_PHASE_TESSELLATION_EMISSION, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    if (ctx->source_quality_analysis) {
        HLSLEmitStatus status = emitted ? HLSL_EMIT_STATUS_OK : ctx->diagnostic ? ctx->diagnostic->status : HLSL_EMIT_STATUS_UNSUPPORTED;
        if (!hlsl_source_quality_analysis_finish(ctx->source_quality_analysis, status, units) && emitted) {
            hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            emitted = false;
        }
        hlsl_source_quality_analysis_destroy(ctx->source_quality_analysis); ctx->source_quality_analysis = NULL;
    }
    return emitted && sb_ok(ctx->sb);
}
bool hlsl_emit_high_level_hull_join(HLSLEmitterContext *ctx) {
    PatchPlan plan = {0};
    if (!hull_contract(ctx->program, &plan) || !allocate_names(ctx, &plan) || ctx->unity_uv_helper || ctx->readable_screen_pos_helper ||
        hlsl_global_declarations_validate_empty_target(ctx->global_declarations, ctx->program, ctx->params, ctx->common_params) != HLSL_GLOBAL_DECLARATIONS_OK)
        return finish(ctx, false, 3);
    for (int i = 0; i < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++i) ctx->float4_functions.group[i] = -1;
    for (size_t phase = 0; phase < ctx->program->tessellation.phase_count; ++phase)
        if (!prepare(ctx, &plan, (int)phase)) return finish(ctx, false, 3);
    hlsl_expression_source_map_begin(ctx);
    if (!begin_unit(ctx, 0, HLSL_SOURCE_UNIT_CONFIGURATION)) return finish(ctx, false, 3);
    emit_structures(ctx, &plan);
    if (!begin_unit(ctx, 1, HLSL_SOURCE_UNIT_HELPER)) return finish(ctx, false, 3);
    sb_appendf(ctx->sb, "%s %s(InputPatch<%s, %u> %s) {\n    %s %s;\n", plan.names[FACTOR_TYPE], plan.names[PATCH_FUNCTION],
        plan.names[POINT_TYPE], (unsigned)ctx->program->tessellation.input_control_point_count, plan.names[PATCH_VARIABLE],
        plan.names[FACTOR_TYPE], plan.names[FACTOR_VARIABLE]);
    hlsl_source_quality_emission(ctx, 0, false, -1);
    for (int phase = 0; phase < (int)ctx->program->tessellation.phase_count; ++phase) {
        if (!prepare(ctx, &plan, phase)) return finish(ctx, false, 3);
        const USILHullPhase *s = &ctx->program->tessellation.phases[phase];
        if (s->instance_count > 1) {
            sb_append(ctx->sb, "    for (uint ");
            const size_t index_begin = ctx->sb->len;
            sb_appendf(ctx->sb, "%s = 0; %s < %u; ++%s) {\n", plan.names[INSTANCE_INDEX], plan.names[INSTANCE_INDEX],
                (unsigned)s->instance_count, plan.names[INSTANCE_INDEX]);
            hlsl_source_quality_emission(ctx, 0, true, -1);
            for (int index = s->first_instruction_index; index < s->end_instruction_index; ++index)
                if (hlsl_instruction_owners_contains(&plan.index_transports, index)) {
                    if (!observe_value(ctx, plan.names[INSTANCE_INDEX], index, UINT64_C(0xc000000000000000) | (uint64_t)phase << 32, 1)) return finish(ctx, false, 3);
                    hlsl_source_quality_emission(ctx, 0, false, index);
                    if (ctx->expression_source_map) {
                        HLSLExpressionOrigin *o = &ctx->expression_source_map->origins[index];
                        o->kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION; o->source_begin = index_begin;
                        o->source_end = index_begin + strlen(plan.names[INSTANCE_INDEX]);
                    }
                }
        } else {
            sb_append(ctx->sb, "    {\n"); hlsl_source_quality_emission(ctx, 0, false, -1);
        }
        ctx->indent = 8;
        const HLSLPureExpressionScope scope = {.first_instruction = s->first_instruction_index, .end_instruction = s->end_instruction_index,
            .omitted_instructions = &plan.index_transports, .destination_supported = destination_supported, .append_destination = append_destination,
            .source_supported = source_supported, .source_expression = source_expression, .context = &plan};
        if (!hlsl_emit_pure_expression_scope(ctx, &scope)) return finish(ctx, false, 3);
        const size_t end_begin = ctx->sb->len;
        sb_append(ctx->sb, "    }\n"); hlsl_source_quality_emission(ctx, 0, false, s->end_instruction_index - 1);
        if (ctx->expression_source_map) {
            HLSLExpressionOrigin *o = &ctx->expression_source_map->origins[s->end_instruction_index - 1];
            o->source_begin = end_begin; o->source_end = ctx->sb->len;
        }
    }
    sb_appendf(ctx->sb, "    return %s;\n}\n\n", plan.names[FACTOR_VARIABLE]);
    if (!observe_value(ctx, plan.names[FACTOR_VARIABLE], -1, UINT64_C(0xd000000000000000), 0)) return finish(ctx, false, 3);
    hlsl_source_quality_emission(ctx, 0, false, -1);
    if (!begin_unit(ctx, 2, HLSL_SOURCE_UNIT_ENTRY_POINT)) return finish(ctx, false, 3);
    sb_appendf(ctx->sb, "[domain(\"%s\")]\n[partitioning(\"%s\")]\n[outputtopology(\"%s\")]\n[outputcontrolpoints(%u)]\n[patchconstantfunc(\"%s\")]\n[maxtessfactor(",
        plan.shape.attribute, partition_name(ctx->program->tessellation.partitioning),
        ctx->program->tessellation.output_primitive == DXBC_TESSELLATOR_OUTPUT_LINE ? "line" :
            ctx->program->tessellation.output_primitive == DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW ? "triangle_cw" : "triangle_ccw",
        (unsigned)ctx->program->tessellation.output_control_point_count, plan.names[PATCH_FUNCTION]);
    ASTExpr *maximum = ast_create_literal_bits(&ctx->program->tessellation.max_tessellation_factor_bits, 1, AST_SCALAR_FLOAT32);
    if (!maximum || !hlsl_source_quality_observe_expression(ctx, maximum, -1)) { ast_free_expr(maximum); return finish(ctx, false, 3); }
    ast_format_expr(maximum, ctx->sb); ast_free_expr(maximum);
    sb_appendf(ctx->sb, ")]\n%s %s(InputPatch<%s, %u> %s, uint %s : SV_OutputControlPointID) {\n    return %s[%s];\n}\n",
        plan.names[POINT_TYPE], ctx->entry_point_name, plan.names[POINT_TYPE], (unsigned)ctx->program->tessellation.input_control_point_count,
        plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX], plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX]);
    char text[300];
    if (!hlsl_format_checked(ctx, text, sizeof(text), "%s[%s]", plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX]) ||
        !observe_value(ctx, text, -1, UINT64_C(0xe000000000000000), 0)) return finish(ctx, false, 3);
    hlsl_source_quality_emission(ctx, 0, false, -1);
    return finish(ctx, true, 3);
}
bool hlsl_emit_high_level_patch_domain(HLSLEmitterContext *ctx) {
    /* This complete stage owns its output local and RET. The ordinary direct
     * signature route must not rewrite scope writes into early returns. */
    ctx->high_level_direct_return = false;
    ctx->high_level_interface = false;
    PatchPlan plan = {0};
    if (!domain_contract(ctx->program, &plan) || !allocate_names(ctx, &plan) || ctx->unity_uv_helper || ctx->readable_screen_pos_helper ||
        hlsl_global_declarations_validate_empty_target(ctx->global_declarations, ctx->program, ctx->params, ctx->common_params) != HLSL_GLOBAL_DECLARATIONS_OK ||
        !prepare(ctx, &plan, 0)) return finish(ctx, false, 2);
    for (int i = 0; i < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++i) ctx->float4_functions.group[i] = -1;
    hlsl_expression_source_map_begin(ctx);
    if (!begin_unit(ctx, 0, HLSL_SOURCE_UNIT_CONFIGURATION)) return finish(ctx, false, 2);
    emit_structures(ctx, &plan);
    if (!begin_unit(ctx, 1, HLSL_SOURCE_UNIT_ENTRY_POINT)) return finish(ctx, false, 2);
    sb_appendf(ctx->sb, "[domain(\"%s\")]\nfloat4 %s(%s %s, float%u %s : SV_DomainLocation, const OutputPatch<%s, %u> %s) : %s {\n    float4 %s;\n",
        plan.shape.attribute, ctx->entry_point_name, plan.names[FACTOR_TYPE], plan.names[FACTOR_VARIABLE],
        (unsigned)plan.shape.coordinate_count, plan.names[LOCATION], plan.names[POINT_TYPE],
        (unsigned)ctx->program->tessellation.input_control_point_count, plan.names[PATCH_VARIABLE],
        dxbc_signature_semantic_name(ctx->program->outputs), plan.names[OUTPUT_VALUE]);
    hlsl_source_quality_emission(ctx, 0, false, -1); ctx->indent = 4;
    const HLSLPureExpressionScope scope = {.first_instruction = 0, .end_instruction = ctx->program->instruction_count,
        .compose_disjoint_temp_lanes = true,
        .destination_supported = destination_supported, .append_destination = append_destination,
        .source_supported = source_supported, .source_expression = source_expression, .context = &plan};
    if (!hlsl_emit_pure_expression_scope(ctx, &scope)) return finish(ctx, false, 2);
    const size_t return_begin = ctx->sb->len;
    sb_appendf(ctx->sb, "    return %s;\n}\n", plan.names[OUTPUT_VALUE]);
    if (!observe_value(ctx, plan.names[OUTPUT_VALUE], -1, UINT64_C(0xf000000000000000), 0)) return finish(ctx, false, 2);
    const int ret = ctx->program->instruction_count - 1;
    hlsl_source_quality_emission(ctx, 0, false, ret);
    if (ctx->expression_source_map) {
        ctx->expression_source_map->origins[ret].source_begin = return_begin;
        ctx->expression_source_map->origins[ret].source_end = ctx->sb->len;
    }
    return finish(ctx, true, 2);
}
