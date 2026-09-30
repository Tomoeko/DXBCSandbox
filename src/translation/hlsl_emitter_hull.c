// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_identifier.h"
#include "translation/usil_validation.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Independent fork phases have independent temporary definitions. This
 * producer uses the same CFG/SSA and pure expression planner as graphics,
 * retaining global semantic/raw owners. The source boundary is independent
 * scalar-factor fork phases and signature-backed implicit
 * control-point passthrough. It is not an inverse of the authored function. */
enum { HULL_SOURCE_INSTRUCTION_LIMIT = 64, HULL_SOURCE_NAME_COUNT = 10 };

typedef struct {
    HLSLDomainShape shape;
    int outer_registers[4], inner_registers[2];
    int phase, outer_phase, inner_phase;
    bool inner_first;
    HLSLInstructionOwners index_transports;
    char names[HULL_SOURCE_NAME_COUNT][96];
} HullSourcePlan;

enum { POINT_TYPE, FACTOR_TYPE, PATCH_FUNCTION, PATCH_VARIABLE, FACTOR_VARIABLE,
       POINT_FIELD, OUTER_FIELD, INNER_FIELD, FACTOR_INDEX, POINT_INDEX };

static const char *partitioning_name(DXBCTessellatorPartitioning partitioning) {
    switch (partitioning) {
    case DXBC_TESSELLATOR_PARTITIONING_INTEGER: return "integer";
    case DXBC_TESSELLATOR_PARTITIONING_POW2: return "pow2";
    case DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD: return "fractional_odd";
    case DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN: return "fractional_even";
    default: return NULL;
    }
}

static bool position_signature(const DXBCSignatureElement *element) {
    return element && element->register_id == 0 && element->semantic_index == 0 &&
        element->system_value == 1 && element->component_type == 3 &&
        element->mask == 15 && !element->stream_index && !element->min_precision &&
        !element->interpolation_mode &&
        !dxbc_ascii_strcasecmp(dxbc_signature_semantic_name(element), "SV_POSITION");
}

bool hlsl_hull_phase_return_owned(const USILProgram *program, int instruction) {
    if (!program || program->program_type != DXBC_PROGRAM_TYPE_HULL ||
        !program->has_stage_contract || !program->tessellation.valid ||
        !program->instructions || instruction < 0 || instruction >= program->instruction_count ||
        program->instructions[instruction].opcode != USIL_OP_RET ||
        !program->tessellation.phases || program->tessellation.phase_count > 64 ||
        program->tessellation.phase_capacity < program->tessellation.phase_count) return false;
    unsigned matches = 0;
    for (size_t phase = 0; phase < program->tessellation.phase_count; ++phase) {
        const USILHullPhase *scope = &program->tessellation.phases[phase];
        if (instruction + 1 == scope->end_instruction_index &&
            instruction >= scope->first_instruction_index &&
            program->instructions[instruction].source_instruction_index >= scope->first_source_instruction_index &&
            program->instructions[instruction].source_instruction_index < scope->end_source_instruction_index)
            ++matches;
    }
    return matches == 1;
}

static bool factor_signature(const USILProgram *program, HullSourcePlan *plan) {
    const HLSLDomainShape *shape = &plan->shape;
    if (!hlsl_domain_shape(program->tessellation.domain, &plan->shape) ||
        !hlsl_domain_factor_order(program, &plan->inner_first)) return false;
    uint8_t coverage = 0;
    for (int field = 0; field < program->patch_constant_count; ++field) {
        const DXBCSignatureElement *element = &program->patch_constants[field];
        if (element->component_type != 3 || element->mask != 1 ||
            element->rw_mask != 14 || element->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
            element->min_precision || element->stream_index || element->interpolation_mode) return false;
        const unsigned semantic = element->semantic_index;
        unsigned slot;
        if (semantic < shape->outer_count && element->system_value == shape->outer_system_values[semantic] &&
            !strcmp(dxbc_signature_semantic_name(element), "SV_TessFactor")) {
            slot = semantic;
            plan->outer_registers[semantic] = (int)element->register_id;
        } else if (semantic < shape->inner_count && element->system_value == shape->inner_system_values[semantic] &&
                   !strcmp(dxbc_signature_semantic_name(element), "SV_InsideTessFactor")) {
            slot = shape->outer_count + semantic;
            plan->inner_registers[semantic] = (int)element->register_id;
        } else return false;
        if (coverage & (1u << slot)) return false;
        coverage |= (uint8_t)(1u << slot);
        for (int previous = 0; previous < field; ++previous)
            if (program->patch_constants[previous].register_id == element->register_id) return false;
    }
    return coverage == (uint8_t)((1u << (shape->outer_count + shape->inner_count)) - 1u);

}

static bool declarations_owned(const USILProgram *program, const HullSourcePlan *plan) {
    uint8_t coverage = 0;
    uint8_t instance_input = 0;
    for (int index = 0; index < program->signature_declaration_count; ++index) {
        const USILSignatureDeclaration *declaration = &program->signature_declarations[index];
        if (declaration->has_array_element_count || declaration->has_interpolation ||
            declaration->stream_index || declaration->interpolation_mode) return false;
        int phase = -1;
        for (size_t scope = 0; scope < program->tessellation.phase_count; ++scope)
            if (declaration->source_instruction_index >= program->tessellation.phases[scope].first_source_instruction_index &&
                declaration->source_instruction_index < program->tessellation.phases[scope].end_source_instruction_index)
                phase = (int)scope; /* Admitted phase count is at most two. */
        if (phase < 0) return false;
        if (declaration->operand_type == OPERAND_TYPE_FORK_INSTANCE_ID) {
            if (program->tessellation.phases[phase].instance_count <= 1 ||
                (instance_input & (1u << phase)) || declaration->kind != USIL_SIGNATURE_DECL_INPUT ||
                declaration->mask || declaration->has_signature_register || declaration->has_system_value)
                return false;
            instance_input |= (uint8_t)(1u << phase);
            continue;
        }
        if (declaration->kind != USIL_SIGNATURE_DECL_OUTPUT_SIV ||
            declaration->operand_type != OPERAND_TYPE_OUTPUT || !declaration->has_signature_register ||
            declaration->mask != 1 || !declaration->has_system_value) return false;
        int slot = -1;
        for (int outer = 0; outer < plan->shape.outer_count; ++outer)
            if (phase == plan->outer_phase && declaration->register_id == (uint32_t)plan->outer_registers[outer] &&
                declaration->system_value_name == plan->shape.raw_outer_siv_names[outer]) slot = outer;
        for (int inner = 0; inner < plan->shape.inner_count; ++inner)
            if (phase == plan->inner_phase && declaration->register_id == (uint32_t)plan->inner_registers[inner] &&
                declaration->system_value_name == plan->shape.raw_inner_siv_names[inner])
                slot = plan->shape.outer_count + inner;
        if (slot < 0 || (coverage & (1u << slot))) return false;
        coverage |= (uint8_t)(1u << slot);
    }
    uint8_t expected_inputs = 0;
    for (size_t phase = 0; phase < program->tessellation.phase_count; ++phase)
        if (program->tessellation.phases[phase].instance_count > 1) expected_inputs |= (uint8_t)(1u << phase);
    return instance_input == expected_inputs &&
        coverage == (uint8_t)((1u << (plan->shape.outer_count + plan->shape.inner_count)) - 1u);
}

static bool hull_contract(const USILProgram *program, HullSourcePlan *plan) {
    HLSLDomainShape shape;
    if (!program || !hlsl_domain_shape(program->tessellation.domain, &shape)) return false;
    const unsigned groups = shape.inner_count ? 2 : 1;
    const unsigned indexed_groups = (shape.outer_count > 1) + (shape.inner_count > 1);
    const unsigned factors = shape.outer_count + shape.inner_count;
    if (!program->has_stage_contract || !program->has_parsed_signature_authority ||
        program->program_type != DXBC_PROGRAM_TYPE_HULL || program->shader_model_major != 5 ||
        program->shader_model_minor || !memchr(program->shader_type_model, 0, sizeof(program->shader_type_model)) ||
        strcmp(program->shader_type_model, "hs_5_0") || !program->tessellation.valid ||
        !program->tessellation.input_control_point_count || program->tessellation.input_control_point_count > 32 ||
        program->tessellation.output_control_point_count != program->tessellation.input_control_point_count ||
        !partitioning_name(program->tessellation.partitioning) ||
        (program->tessellation.domain == DXBC_TESSELLATOR_DOMAIN_ISOLINE
            ? program->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_LINE
            : (program->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
               program->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CCW)) ||
        !program->tessellation.has_max_tessellation_factor ||
        program->tessellation.phase_count != groups || program->tessellation.phase_capacity < groups ||
        !program->tessellation.phases || program->input_count != 1 || program->output_count != 1 ||
        program->patch_constant_count != (int)factors || program->input_alloc < 1 || program->output_alloc < 1 ||
        program->patch_constant_alloc < (int)factors || !program->inputs || !program->outputs || !program->patch_constants ||
        !position_signature(program->inputs) || !position_signature(program->outputs) ||
        strcmp(dxbc_signature_semantic_name(program->inputs), dxbc_signature_semantic_name(program->outputs)) ||
        program->inputs[0].rw_mask != 15 || program->outputs[0].rw_mask != 0 ||
        program->signature_declaration_count != (int)(factors + indexed_groups) ||
        program->signature_declaration_alloc < program->signature_declaration_count ||
        !program->signature_declarations || program->cbuffer_count || program->texture_count ||
        program->sampler_count || program->uav_count || program->indexable_temp_count ||
        program->icb_value_count || program->geometry.valid || program->compute.valid ||
        (program->has_global_flags && program->global_flags != 1) ||
        program->instruction_count < 2 || program->instruction_count > HULL_SOURCE_INSTRUCTION_LIMIT ||
        program->instruction_alloc < program->instruction_count || !program->instructions ||
        program->temp_count < 0 || program->temp_count > HLSL_SM5_TEMP_REGISTER_COUNT ||
        program->index_range_count != (int)indexed_groups || program->index_range_alloc < (int)indexed_groups || !program->index_ranges ||
        !usil_signature_authority_is_valid(program)) return false;
    float max_factor;
    memcpy(&max_factor, &program->tessellation.max_tessellation_factor_bits, sizeof(max_factor));
    if (!isfinite(max_factor) || max_factor < 1.0f || max_factor > 64.0f || !factor_signature(program, plan)) return false;
    plan->outer_phase = plan->inner_phase = -1;
    int next = 0;
    for (int phase = 0; phase < (int)groups; ++phase) {
        const USILHullPhase *scope = &program->tessellation.phases[phase];
        if (scope->kind != DXBC_HULL_PHASE_FORK ||
            (scope->instance_count != shape.outer_count && scope->instance_count != shape.inner_count) ||
            (!scope->instance_count_declared && scope->instance_count != 1) ||
            scope->first_instruction_index != next || scope->end_instruction_index <= next ||
            scope->end_instruction_index > program->instruction_count ||
            scope->marker_source_instruction_index >= scope->first_source_instruction_index ||
            scope->first_source_instruction_index >= scope->end_source_instruction_index ||
            (phase && scope->marker_source_instruction_index != program->tessellation.phases[phase - 1].end_source_instruction_index))
            return false;
        int *role = scope->instance_count == shape.outer_count ? &plan->outer_phase : &plan->inner_phase;
        if (*role >= 0) return false;
        *role = phase;
        for (int index = next; index < scope->end_instruction_index; ++index) {
            const USILInstruction *instruction = &program->instructions[index];
            if (instruction->source_instruction_index < scope->first_source_instruction_index ||
                instruction->source_instruction_index >= scope->end_source_instruction_index ||
                (index > next && instruction->source_instruction_index <= program->instructions[index - 1].source_instruction_index) ||
                instruction->precise_mask || instruction->saturate || instruction->condition_test != DXBC_INSTRUCTION_TEST_NONE ||
                instruction->has_resource_dimension || instruction->resource_dimension[0] || instruction->resource_stride ||
                instruction->has_texel_offset || instruction->has_resource_return_types ||
                instruction->resource_info_return_type || instruction->sample_info_return_type ||
                instruction->geometry_effect || instruction->geometry_stream_id || instruction->geometry_stream_explicit ||
                !usil_instruction_shape_valid(program, instruction) ||
                (instruction->opcode != USIL_OP_MOV && instruction->opcode != USIL_OP_ADD &&
                 instruction->opcode != USIL_OP_MUL && instruction->opcode != USIL_OP_MAD &&
                 instruction->opcode != USIL_OP_RET)) return false;
            if (instruction->opcode == USIL_OP_RET && index + 1 != scope->end_instruction_index) return false;
        }
        if (!hlsl_hull_phase_return_owned(program, scope->end_instruction_index - 1)) return false;
        next = scope->end_instruction_index;
    }
    if (next != program->instruction_count || plan->outer_phase < 0 || (shape.inner_count && plan->inner_phase < 0) ||
        !declarations_owned(program, plan)) return false;
    uint8_t ranges = 0;
    for (int index = 0; index < program->index_range_count; ++index) {
        const USILIndexRange *range = &program->index_ranges[index];
        const int phase = range->hull_phase_index;
        if (phase < 0 || phase >= (int)groups || (ranges & (1u << phase))) return false;
        const bool inner = phase == plan->inner_phase;
        const unsigned count = inner ? shape.inner_count : shape.outer_count;
        const int base = inner ? plan->inner_registers[0] : plan->outer_registers[0];
        const DXBCOperand *operand = &range->operand;
        const USILHullPhase *scope = &program->tessellation.phases[phase];
        if (count <= 1 || range->register_count != count ||
            range->source_instruction_index < scope->first_source_instruction_index ||
            range->source_instruction_index >= scope->end_source_instruction_index ||
            operand->type != OPERAND_TYPE_OUTPUT || !hlsl_lift_operand_is_plain(operand) || operand->extended_tokens ||
            operand->register_index_dim != 1 || operand->register_index != base ||
            !operand->index_has_immediate[0] || operand->index_representations[0] ||
            operand->index_value_exceeds_int[0] || operand->index_values[0] != (uint32_t)base ||
            usil_operand_destination_lane_mask(operand) != 1) return false;
        ranges |= (uint8_t)(1u << phase);
    }
    return true;

}

bool hlsl_high_level_hull_source_supported(const USILProgram *program, HLSLEmitMode mode) {
    HullSourcePlan plan = {0};
    return mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE && hull_contract(program, &plan);
}

static bool instance_operand(const DXBCOperand *operand) {
    return operand->type == OPERAND_TYPE_FORK_INSTANCE_ID && hlsl_lift_operand_is_plain(operand) && !operand->extended_tokens &&
        !operand->register_index_dim && operand->swizzle_mode == 2 && !operand->swizzle[0] &&
        !operand->imm_value_count && !operand->immediate_word_count;
}

static bool scalar_temp(const USILProgram *program, const DXBCOperand *operand, bool destination) {
    return operand->type == OPERAND_TYPE_TEMP && hlsl_lift_operand_is_plain(operand) && !operand->extended_tokens &&
        operand->register_index_dim == 1 && operand->register_index >= 0 && operand->register_index < program->temp_count &&
        operand->index_has_immediate[0] && !operand->index_representations[0] &&
        !operand->index_value_exceeds_int[0] && operand->index_values[0] == (uint32_t)operand->register_index &&
        (destination ? usil_operand_destination_lane_mask(operand) == 1 :
            operand->swizzle_mode == 2 && !operand->swizzle[0]);
}

static bool claim_index_transport(HLSLEmitterContext *ctx, HullSourcePlan *plan, int definition, int before) {
    const USILHullPhase *phase = &ctx->program->tessellation.phases[plan->phase];
    for (unsigned depth = 0; depth < HULL_SOURCE_INSTRUCTION_LIMIT; ++depth) {
        if (definition < phase->first_instruction_index || definition >= before) return false;
        const USILInstruction *copy = &ctx->program->instructions[definition];
        if (copy->opcode != USIL_OP_MOV || copy->operand_count != 2 ||
            !scalar_temp(ctx->program, &copy->operands[0], true) ||
            !hlsl_instruction_owners_add(&plan->index_transports, definition)) return false;
        if (instance_operand(&copy->operands[1])) return true;
        if (!scalar_temp(ctx->program, &copy->operands[1], false)) return false;
        before = definition;
        definition = hlsl_operand_definition(ctx, definition, 1, 0);
    }
    return false;
}

static bool destination_supported(HLSLEmitterContext *ctx, int index, void *context) {
    HullSourcePlan *plan = context;
    const DXBCOperand *destination = &ctx->program->instructions[index].operands[0];
    if (destination->type != OPERAND_TYPE_OUTPUT || destination->register_index_dim != 1 ||
        destination->swizzle_mode || destination->min_precision || destination->has_abs || destination->has_neg ||
        destination->extended_token_count || destination->extended_tokens || destination->rel_op1 || destination->rel_op2 ||
        usil_operand_destination_lane_mask(destination) != 1) return false;
    const bool inner = plan->phase == plan->inner_phase;
    const unsigned count = inner ? plan->shape.inner_count : plan->shape.outer_count;
    const int base = inner ? plan->inner_registers[0] : plan->outer_registers[0];
    if (count == 1) return hlsl_lift_operand_is_plain(destination) &&
        destination->register_index == base && destination->index_has_immediate[0] &&
        !destination->index_representations[0] && !destination->index_value_exceeds_int[0] &&
        destination->index_values[0] == (uint32_t)base;
    const bool relative_only = !base && destination->index_representations[0] == 2 &&
        !destination->index_has_immediate[0] && !destination->index_values[0];
    const bool relative_with_base = destination->index_representations[0] == 3 &&
        destination->index_has_immediate[0] && destination->index_values[0] == (uint32_t)base;
    return destination->register_index == base && destination->rel_op0 &&
        (relative_only || relative_with_base) && !destination->index_value_exceeds_int[0] &&
        scalar_temp(ctx->program, destination->rel_op0, false) &&
        claim_index_transport(ctx, plan, hlsl_relative_operand_definition(ctx, index, 0, 0), index);

}

static bool append_destination(HLSLEmitterContext *ctx, int index, void *context) {
    HullSourcePlan *plan = context;
    if (!destination_supported(ctx, index, context)) return false;
    const bool inner = plan->phase == plan->inner_phase;
    const unsigned count = inner ? plan->shape.inner_count : plan->shape.outer_count;
    sb_appendf(ctx->sb, "%s.%s", plan->names[FACTOR_VARIABLE], plan->names[inner ? INNER_FIELD : OUTER_FIELD]);
    if (count > 1) sb_appendf(ctx->sb, "[%s]", plan->names[FACTOR_INDEX]);
    return sb_ok(ctx->sb);
}

static bool prepare_phase(HLSLEmitterContext *ctx, HullSourcePlan *plan, int phase) {
    free_hlsl_ssa_graph(ctx);
    free_control_flow_graph(ctx);
    const USILHullPhase *scope = &ctx->program->tessellation.phases[phase];
    plan->phase = phase;
    if (!build_control_flow_graph_range(ctx, scope->first_instruction_index, scope->end_instruction_index) ||
        !compute_dominance(&ctx->cfg) || !build_hlsl_ssa_graph(ctx)) return false;
    unsigned output_count = 0;
    for (int index = scope->first_instruction_index; index < scope->end_instruction_index; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        if (instruction->operand_count && instruction->operands[0].type == OPERAND_TYPE_OUTPUT) {
            if (++output_count != 1 || !destination_supported(ctx, index, plan)) return false;
        }
    }
    if (output_count != 1) return false;
    for (int index = scope->first_instruction_index; index < scope->end_instruction_index; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        if (hlsl_instruction_owners_contains(&plan->index_transports, index)) continue;
        for (int operand = 1; operand < instruction->operand_count; ++operand) {
            if (instruction->operands[operand].type == OPERAND_TYPE_TEMP &&
                hlsl_instruction_owners_contains(&plan->index_transports,
                    hlsl_operand_definition(ctx, index, operand, 0))) return false;
        }
    }
    return true;
}

static bool allocate_names(HLSLEmitterContext *ctx, HullSourcePlan *plan) {
    static const char *const bases[HULL_SOURCE_NAME_COUNT] = {
        "HullPoint", "HullFactors", "patchConstants", "patch", "factors",
        "clipPosition", "outer", "inner", "factorIndex", "pointIndex"};
    if (!hlsl_source_identifier_valid(ctx->entry_point_name) ||
        ctx->reserved_preprocessor_identifier_count > 1024) return false;
    static const char *const fixed_tokens[] = {"InputPatch", "struct", "for", "return", "const",
        "float", "float2", "float3", "float4", "uint", "asfloat", "abs", "mad",
        "SV_POSITION", "SV_TessFactor", "SV_InsideTessFactor", "SV_OutputControlPointID",
        "domain", "partitioning", "outputtopology", "outputcontrolpoints", "patchconstantfunc", "maxtessfactor"};
    for (size_t index = 0; index < ctx->reserved_preprocessor_identifier_count; ++index) {
        const char *reserved = ctx->reserved_preprocessor_identifiers[index];
        if (!strcmp(reserved, ctx->entry_point_name) ||
            !strcmp(reserved, dxbc_signature_semantic_name(ctx->program->inputs))) return false;
        for (size_t token = 0; token < sizeof(fixed_tokens) / sizeof(fixed_tokens[0]); ++token)
            if (!strcmp(reserved, fixed_tokens[token])) return false;
    }
    const size_t original_count = ctx->reserved_preprocessor_identifier_count;
    const char *const *original = ctx->reserved_preprocessor_identifiers;
    const char **reserved = calloc(original_count + HULL_SOURCE_NAME_COUNT, sizeof(*reserved));
    if (!reserved) return false;
    for (size_t index = 0; index < original_count; ++index) reserved[index] = original[index];
    ctx->reserved_preprocessor_identifiers = reserved;
    bool success = true;
    for (int name = 0; name < HULL_SOURCE_NAME_COUNT; ++name) {
        if (!hlsl_allocate_interface_name(ctx, bases[name], plan->names[name])) { success = false; break; }
        reserved[ctx->reserved_preprocessor_identifier_count++] = plan->names[name];
    }
    ctx->reserved_preprocessor_identifiers = original;
    ctx->reserved_preprocessor_identifier_count = original_count;
    free(reserved);
    return success;
}

static bool begin_unit(HLSLEmitterContext *ctx, uint32_t id, HLSLSourceQualityUnitKind kind) {
    return !ctx->source_quality_analysis || hlsl_source_quality_analysis_begin_unit(ctx->source_quality_analysis, id, kind, true);
}

static bool observe_owned_atom(HLSLEmitterContext *ctx, const char *text, int instruction,
                                uint64_t logical_id, unsigned width) {
    ASTOperandProvenance provenance;
    ast_operand_provenance_init(&provenance);
    provenance.complete = true;
    provenance.value_role = AST_OPERAND_VALUE_LOGICAL;
    provenance.logical_value_id = logical_id;
    provenance.natural_components = provenance.result_components = (uint8_t)width;
    for (unsigned lane = 0; lane < width; ++lane) provenance.selected_components[lane] = (uint8_t)lane;
    if (instruction >= 0) {
        provenance.instruction_index = instruction;
        provenance.source_instruction_index = ctx->program->instructions[instruction].source_instruction_index;
        provenance.operand_index = 1;
        provenance.destination_lanes = usil_operand_destination_lane_mask(&ctx->program->instructions[instruction].operands[0]);
    }
    ASTExpr *expression = ast_create_emitter_operand_with_provenance(text, &provenance);
    const bool accepted = expression && hlsl_source_quality_observe_expression(ctx, expression, instruction);
    ast_free_expr(expression);
    return accepted;
}

bool hlsl_emit_high_level_hull_stage(HLSLEmitterContext *ctx) {
    HullSourcePlan plan = {0};
    bool emitted = false;
    if (!hull_contract(ctx->program, &plan) || !allocate_names(ctx, &plan) ||
        (ctx->params && (ctx->params->cb_count || ctx->params->res_count)) ||
        (ctx->common_params && (ctx->common_params->cb_count || ctx->common_params->res_count)) ||
        ctx->global_declarations || ctx->unity_uv_helper || ctx->readable_screen_pos_helper) goto finish;
    for (int index = 0; index < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++index) ctx->float4_functions.group[index] = -1;
    /* Analyze both independent phases before appending source. */
    for (size_t phase = 0; phase < ctx->program->tessellation.phase_count; ++phase)
        if (!prepare_phase(ctx, &plan, (int)phase)) goto finish;
    hlsl_expression_source_map_begin(ctx);
    StringBuilder *sb = ctx->sb;
    if (!begin_unit(ctx, 0, HLSL_SOURCE_UNIT_CONFIGURATION)) goto finish;
    sb_appendf(sb, "struct %s {\n    float4 %s : %s;\n};\n\n", plan.names[POINT_TYPE], plan.names[POINT_FIELD],
        dxbc_signature_semantic_name(ctx->program->inputs));
    hlsl_source_quality_emission(ctx, 0, false, -1);
    sb_appendf(sb, "struct %s {\n", plan.names[FACTOR_TYPE]);
    hlsl_source_quality_emission(ctx, 0, false, -1);
    for (int field = 0; field < (plan.shape.inner_count ? 2 : 1); ++field) {
        const bool inner = plan.inner_first ? field == 0 : field == 1;
        const unsigned count = inner ? plan.shape.inner_count : plan.shape.outer_count;
        if (inner && count == 1) sb_appendf(sb, "    float %s : SV_InsideTessFactor;\n", plan.names[INNER_FIELD]);
        else sb_appendf(sb, "    float %s[%u] : %s;\n", plan.names[inner ? INNER_FIELD : OUTER_FIELD], count,
            inner ? "SV_InsideTessFactor" : "SV_TessFactor");
        hlsl_source_quality_emission(ctx, 0, false, -1);
    }
    sb_append(sb, "};\n\n");
    hlsl_source_quality_emission(ctx, 0, false, -1);
    if (!begin_unit(ctx, 1, HLSL_SOURCE_UNIT_HELPER)) goto finish;
    sb_appendf(sb, "%s %s(InputPatch<%s, %u> %s) {\n    %s %s;\n",
        plan.names[FACTOR_TYPE], plan.names[PATCH_FUNCTION], plan.names[POINT_TYPE],
        (unsigned)ctx->program->tessellation.input_control_point_count, plan.names[PATCH_VARIABLE],
        plan.names[FACTOR_TYPE], plan.names[FACTOR_VARIABLE]);
    hlsl_source_quality_emission(ctx, 0, false, -1);
    for (int phase = 0; phase < (int)ctx->program->tessellation.phase_count; ++phase) {
        if (!prepare_phase(ctx, &plan, phase)) goto finish;
        const USILHullPhase *owned = &ctx->program->tessellation.phases[phase];
        const unsigned instances = owned->instance_count;
        if (instances > 1) {
            sb_append(sb, "    for (uint ");
            const size_t index_begin = sb->len;
            sb_appendf(sb, "%s = 0; %s < %u; ++%s) {\n", plan.names[FACTOR_INDEX], plan.names[FACTOR_INDEX], instances, plan.names[FACTOR_INDEX]);
            ctx->indent = 8;
            hlsl_source_quality_emission(ctx, 0, true, -1);
            for (int instruction = owned->first_instruction_index; instruction < owned->end_instruction_index; ++instruction)
                if (hlsl_instruction_owners_contains(&plan.index_transports, instruction)) {
                    if (!observe_owned_atom(ctx, plan.names[FACTOR_INDEX], instruction,
                            UINT64_C(0x8000000000000000) | (uint64_t)phase << 32, 1)) goto finish;
                    hlsl_source_quality_emission(ctx, 0, false, instruction);
                    if (ctx->expression_source_map) {
                        HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[instruction];
                        origin->kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
                        origin->source_begin = index_begin;
                        origin->source_end = index_begin + strlen(plan.names[FACTOR_INDEX]);
                    }
                }
        } else { sb_append(sb, "    {\n"); ctx->indent = 8; hlsl_source_quality_emission(ctx, 0, false, -1); }
        const HLSLPureExpressionScope scope = {.first_instruction = owned->first_instruction_index,
            .end_instruction = owned->end_instruction_index, .omitted_instructions = &plan.index_transports,
            .destination_supported = destination_supported, .append_destination = append_destination, .context = &plan};
        if (!hlsl_emit_pure_expression_scope(ctx, &scope)) goto finish;
        const size_t phase_end_begin = sb->len;
        sb_append(sb, "    }\n");
        hlsl_source_quality_emission(ctx, 0, false, owned->end_instruction_index - 1);
        if (ctx->expression_source_map) {
            HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[owned->end_instruction_index - 1];
            origin->source_begin = phase_end_begin;
            origin->source_end = sb->len;
        }
    }
    ctx->indent = 4;
    sb_appendf(sb, "    return %s;\n}\n\n", plan.names[FACTOR_VARIABLE]);
    if (!observe_owned_atom(ctx, plan.names[FACTOR_VARIABLE], -1, UINT64_C(0x8000000000000001), 0)) goto finish;
    hlsl_source_quality_emission(ctx, 0, false, -1);
    if (!begin_unit(ctx, 2, HLSL_SOURCE_UNIT_ENTRY_POINT)) goto finish;
    sb_appendf(sb, "[domain(\"%s\")]\n[partitioning(\"%s\")]\n[outputtopology(\"%s\")]\n[outputcontrolpoints(%u)]\n[patchconstantfunc(\"%s\")]\n[maxtessfactor(",
        plan.shape.attribute, partitioning_name(ctx->program->tessellation.partitioning),
        ctx->program->tessellation.output_primitive == DXBC_TESSELLATOR_OUTPUT_LINE ? "line" :
            ctx->program->tessellation.output_primitive == DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW ? "triangle_cw" : "triangle_ccw",
        (unsigned)ctx->program->tessellation.output_control_point_count, plan.names[PATCH_FUNCTION]);
    ASTExpr *maximum = ast_create_literal_bits(&ctx->program->tessellation.max_tessellation_factor_bits, 1, AST_SCALAR_FLOAT32);
    if (!maximum || !hlsl_source_quality_observe_expression(ctx, maximum, -1)) { ast_free_expr(maximum); goto finish; }
    ast_format_expr(maximum, sb); ast_free_expr(maximum);
    sb_appendf(sb, ")]\n%s %s(InputPatch<%s, %u> %s, uint %s : SV_OutputControlPointID) {\n    return %s[%s];\n}\n",
        plan.names[POINT_TYPE], ctx->entry_point_name, plan.names[POINT_TYPE],
        (unsigned)ctx->program->tessellation.input_control_point_count, plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX], plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX]);
    char copy[256];
    if (!hlsl_format_checked(ctx, copy, sizeof(copy), "%s[%s]", plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX]) ||
        !observe_owned_atom(ctx, copy, -1, UINT64_C(0x8000000000000002), 0)) goto finish;
    hlsl_source_quality_emission(ctx, 0, false, -1);
    if (ctx->expression_source_map) {
        ctx->expression_source_map->complete = true;
        if (!hlsl_expression_source_map_matches(ctx->expression_source_map, ctx->program, sb->buf)) goto finish;
    }
    emitted = sb_ok(sb);
finish:
    if (!emitted && (!ctx->diagnostic || ctx->diagnostic->status == HLSL_EMIT_STATUS_OK))
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, HLSL_EMIT_PHASE_TESSELLATION_EMISSION, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    if (ctx->source_quality_analysis) {
        HLSLEmitStatus status = emitted ? HLSL_EMIT_STATUS_OK :
            ctx->diagnostic ? ctx->diagnostic->status : HLSL_EMIT_STATUS_UNSUPPORTED;
        if (!hlsl_source_quality_analysis_finish(ctx->source_quality_analysis, status, 3) && emitted) {
            hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            emitted = false;
        }
        hlsl_source_quality_analysis_destroy(ctx->source_quality_analysis);
        ctx->source_quality_analysis = NULL;
    }
    return emitted;
}
