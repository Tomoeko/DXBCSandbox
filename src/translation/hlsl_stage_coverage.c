// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_stage_coverage_internal.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/hlsl_source_identifier.h"

#include <stdlib.h>
#include <string.h>

static HLSLStageCoverage *current_coverage(HLSLEmitterContext *ctx) {
    if (!ctx) return NULL;
    /* Preserve internal matrix test contexts as well as the owning wrapper. */
    return ctx->stage_coverage ? ctx->stage_coverage :
        ctx->matrix_use_capture ? &ctx->matrix_use_capture->coverage : NULL;
}

static bool operand_uses_equal(const HLSLStageOwnedOperandUse *a,
    const HLSLStageOwnedOperandUse *b);

static bool signature_equal(const DXBCSignatureElement *a, const DXBCSignatureElement *b) {
    return a->stream_index == b->stream_index && a->semantic_index == b->semantic_index &&
        a->system_value == b->system_value && a->component_type == b->component_type &&
        a->register_id == b->register_id && a->min_precision == b->min_precision &&
        a->interpolation_mode == b->interpolation_mode && a->mask == b->mask && a->rw_mask == b->rw_mask &&
        !strcmp(dxbc_signature_semantic_name(a), dxbc_signature_semantic_name(b));
}

static bool phase_equal(const USILHullPhase *a, const USILHullPhase *b) {
    return a->kind == b->kind && a->marker_source_instruction_index == b->marker_source_instruction_index &&
        a->first_source_instruction_index == b->first_source_instruction_index &&
        a->end_source_instruction_index == b->end_source_instruction_index &&
        a->first_instruction_index == b->first_instruction_index && a->end_instruction_index == b->end_instruction_index &&
        a->instance_count_declared == b->instance_count_declared && a->instance_count == b->instance_count &&
        a->has_temp_count == b->has_temp_count && a->temp_count == b->temp_count &&
        a->temp_count_source_instruction_index == b->temp_count_source_instruction_index;
}

static bool declaration_equal(const USILSignatureDeclaration *a, const USILSignatureDeclaration *b) {
    return a->kind == b->kind && a->operand_type == b->operand_type &&
        a->has_signature_register == b->has_signature_register && a->register_id == b->register_id &&
        a->mask == b->mask && a->stream_index == b->stream_index &&
        a->has_array_element_count == b->has_array_element_count && a->array_element_count == b->array_element_count &&
        a->has_system_value == b->has_system_value && a->system_value_name == b->system_value_name &&
        a->has_interpolation == b->has_interpolation && a->interpolation_mode == b->interpolation_mode &&
        a->source_instruction_index == b->source_instruction_index;
}

bool hlsl_hull_icb_operands_equal(const HLSLHullICBOperandSnapshot *a, const HLSLHullICBOperandSnapshot *b) {
    if (!a || !b) return false;
    if (a->type != b->type || a->raw_token != b->raw_token || a->register_index != b->register_index ||
        a->register_index_dim != b->register_index_dim || a->swizzle_mode != b->swizzle_mode ||
        a->destination_mask != b->destination_mask) return false;
    for (size_t lane = 0; lane < 4; ++lane)
        if (a->swizzle[lane] != b->swizzle[lane]) return false;
    for (size_t dimension = 0; dimension < 3; ++dimension)
        if (a->rel_offsets[dimension] != b->rel_offsets[dimension] ||
            a->index_values[dimension] != b->index_values[dimension] ||
            a->index_representations[dimension] != b->index_representations[dimension] ||
            a->index_has_immediate[dimension] != b->index_has_immediate[dimension] ||
            a->index_value_exceeds_int[dimension] != b->index_value_exceeds_int[dimension]) return false;
    return true;
}

bool hlsl_hull_icb_consumers_equal(const HLSLHullICBConsumer *a, const HLSLHullICBConsumer *b) {
    return a && b && a->instruction_index == b->instruction_index && a->source_instruction_index == b->source_instruction_index &&
        a->operand_index == b->operand_index && a->demanded_lanes == b->demanded_lanes &&
        a->physical_column == b->physical_column && a->direct_fork_id == b->direct_fork_id &&
        a->transport_tail == b->transport_tail && hlsl_hull_icb_operands_equal(&a->operand, &b->operand) &&
        hlsl_hull_icb_operands_equal(&a->relative, &b->relative);
}

static bool icb_transports_equal(const HLSLHullICBTransport *a, const HLSLHullICBTransport *b) {
    return a->instruction_index == b->instruction_index && a->source_instruction_index == b->source_instruction_index &&
        a->destination_lane == b->destination_lane && a->source_lane == b->source_lane &&
        a->predecessor_transport == b->predecessor_transport &&
        hlsl_hull_icb_operands_equal(&a->destination, &b->destination) && hlsl_hull_icb_operands_equal(&a->source, &b->source);
}

bool hlsl_hull_icb_plans_equal(const HLSLHullICBPlan *a, const HLSLHullICBPlan *b) {
    if (!a || !b || a->payload_count > HLSL_HULL_ICB_WORD_LIMIT || b->payload_count > HLSL_HULL_ICB_WORD_LIMIT ||
        a->row_count > HLSL_HULL_ICB_ROW_LIMIT || b->row_count > HLSL_HULL_ICB_ROW_LIMIT ||
        a->consumer_count > HLSL_HULL_ICB_CONSUMER_LIMIT || b->consumer_count > HLSL_HULL_ICB_CONSUMER_LIMIT ||
        a->transport_count > HLSL_HULL_ICB_TRANSPORT_LIMIT || b->transport_count > HLSL_HULL_ICB_TRANSPORT_LIMIT ||
        a->present != b->present || a->declaration_source_instruction_index != b->declaration_source_instruction_index ||
        a->declaration_token != b->declaration_token || a->declaration_word_count != b->declaration_word_count ||
        a->payload_count != b->payload_count || a->row_count != b->row_count || a->physical_column != b->physical_column ||
        a->phase_index != b->phase_index || !phase_equal(&a->phase, &b->phase) ||
        a->fork_declaration_index != b->fork_declaration_index || !declaration_equal(&a->fork_declaration, &b->fork_declaration) ||
        a->consumer_count != b->consumer_count || a->transport_count != b->transport_count ||
        memcmp(a->array_name, b->array_name, sizeof(a->array_name)) ||
        memcmp(a->index_name, b->index_name, sizeof(a->index_name))) return false;
    for (size_t word = 0; word < HLSL_HULL_ICB_WORD_LIMIT; ++word)
        if (a->payload[word] != b->payload[word]) return false;
    /* Unused slots remain canonical too, so partial or mutated retained plans
     * cannot hide ownership outside their advertised counts. */
    for (size_t index = 0; index < HLSL_HULL_ICB_CONSUMER_LIMIT; ++index)
        if (!hlsl_hull_icb_consumers_equal(&a->consumers[index], &b->consumers[index])) return false;
    for (size_t index = 0; index < HLSL_HULL_ICB_TRANSPORT_LIMIT; ++index)
        if (!icb_transports_equal(&a->transports[index], &b->transports[index])) return false;
    return true;
}

bool hlsl_stage_coverage_hull_icb_empty(const HLSLStageHullICB *icb) {
    const HLSLHullICBPlan empty = {0};
    if (!icb || icb->plan_captured || icb->recorded_plan_captured || icb->declaration_emitted ||
        icb->recorded_declaration_emitted || icb->declaration_begin || icb->declaration_end ||
        icb->recorded_declaration_begin || icb->recorded_declaration_end ||
        icb->literal_root_count || icb->recorded_literal_root_count ||
        icb->access_root_count || icb->recorded_access_root_count ||
        !hlsl_hull_icb_plans_equal(&icb->plan, &empty) || !hlsl_hull_icb_plans_equal(&icb->recorded_plan, &empty)) return false;
    for (size_t row = 0; row < HLSL_HULL_ICB_ROW_LIMIT; ++row)
        if (icb->literal_root_indices[row] || icb->recorded_literal_root_indices[row]) return false;
    for (size_t consumer = 0; consumer < HLSL_HULL_ICB_CONSUMER_LIMIT; ++consumer)
        if (icb->access_root_indices[consumer] || icb->recorded_access_root_indices[consumer]) return false;
    return true;
}

static bool hull_contract_view(HLSLStageHullContract *view, const USILProgram *program) {
    if (!program || program->input_count != 1 || program->output_count != 1 || !program->inputs || !program->outputs ||
        program->patch_constant_count < 1 || program->patch_constant_count > 6 || !program->patch_constants ||
        program->signature_declaration_count < 1 || program->signature_declaration_count > 11 || !program->signature_declarations ||
        !program->tessellation.phase_count || program->tessellation.phase_count > 3 || !program->tessellation.phases ||
        program->cbuffer_count < 0 || program->cbuffer_count > 1 || (program->cbuffer_count && !program->cbuffers)) return false;
    memset(view, 0, sizeof(*view));
    view->tessellation = program->tessellation;
    view->tessellation.phases = NULL;
    view->tessellation.phase_capacity = view->tessellation.phase_count;
    memcpy(view->phases, program->tessellation.phases,
        program->tessellation.phase_count * sizeof(*view->phases));
    view->input = program->inputs[0]; view->output = program->outputs[0];
    view->patch_constant_count = program->patch_constant_count;
    memcpy(view->patch_constants, program->patch_constants,
        (size_t)program->patch_constant_count * sizeof(*view->patch_constants));
    view->signature_declaration_count = program->signature_declaration_count;
    memcpy(view->signature_declarations, program->signature_declarations,
        (size_t)program->signature_declaration_count * sizeof(*view->signature_declarations));
    view->cbuffer_count = program->cbuffer_count;
    if (program->cbuffer_count) view->cbuffer = program->cbuffers[0];
    return true;
}

static void hull_contract_dispose(HLSLStageHullContract *contract) {
    dxbc_signature_element_free(&contract->input);
    dxbc_signature_element_free(&contract->output);
    for (int index = 0; index < contract->patch_constant_count && index < 6; ++index)
        dxbc_signature_element_free(&contract->patch_constants[index]);
    memset(contract, 0, sizeof(*contract));
}

static bool hull_contract_copy(HLSLStageHullContract *copy, const USILProgram *program) {
    HLSLStageHullContract view;
    if (!hull_contract_view(&view, program)) return false;
    *copy = view;
    memset(&copy->input, 0, sizeof(copy->input));
    memset(&copy->output, 0, sizeof(copy->output));
    memset(copy->patch_constants, 0, sizeof(copy->patch_constants));
    if (!dxbc_signature_element_clone(&copy->input, &view.input) ||
        !dxbc_signature_element_clone(&copy->output, &view.output)) goto fail;
    for (int index = 0; index < view.patch_constant_count; ++index)
        if (!dxbc_signature_element_clone(&copy->patch_constants[index], &view.patch_constants[index])) goto fail;
    return true;
fail:
    hull_contract_dispose(copy);
    return false;
}

static bool hull_contract_equal(const HLSLStageHullContract *a, const HLSLStageHullContract *b) {
    const USILTessellationContract *x = &a->tessellation, *y = &b->tessellation;
    if (!x->valid || !y->valid || x->phases || y->phases || !x->phase_count || x->phase_count > 3 ||
        x->phase_count != y->phase_count || x->input_control_point_count != y->input_control_point_count ||
        x->output_control_point_count != y->output_control_point_count || x->domain != y->domain ||
        x->partitioning != y->partitioning || x->output_primitive != y->output_primitive ||
        x->has_max_tessellation_factor != y->has_max_tessellation_factor ||
        x->max_tessellation_factor_bits != y->max_tessellation_factor_bits ||
        x->max_tessellation_factor_source_instruction_index != y->max_tessellation_factor_source_instruction_index ||
        a->patch_constant_count < 1 || a->patch_constant_count > 6 || a->patch_constant_count != b->patch_constant_count ||
        a->signature_declaration_count < 1 || a->signature_declaration_count > 11 ||
        a->signature_declaration_count != b->signature_declaration_count || a->cbuffer_count != b->cbuffer_count ||
        a->cbuffer_count < 0 || a->cbuffer_count > 1 || !signature_equal(&a->input, &b->input) || !signature_equal(&a->output, &b->output)) return false;
    if (a->cbuffer_count && (a->cbuffer.reg_idx != b->cbuffer.reg_idx || a->cbuffer.size != b->cbuffer.size ||
        a->cbuffer.dynamic_indexed != b->cbuffer.dynamic_indexed)) return false;
    for (size_t index = 0; index < x->phase_count; ++index)
        if (!phase_equal(&a->phases[index], &b->phases[index])) return false;
    for (int index = 0; index < a->patch_constant_count; ++index)
        if (!signature_equal(&a->patch_constants[index], &b->patch_constants[index])) return false;
    for (int index = 0; index < a->signature_declaration_count; ++index)
        if (!declaration_equal(&a->signature_declarations[index], &b->signature_declarations[index])) return false;
    return true;
}

static bool opcode_valid(USILOpcode opcode) {
    return (unsigned)opcode <= (unsigned)USIL_OP_IMM_ATOMIC_CMP_EXCH;
}

/* Every demanded lane of an omitted block needs its actual current matrix-read
 * owner. Known scalar/vector fields outside that inventory cannot silently
 * inherit the matrix declaration attachment. */
static bool omitted_matrix_block_supported(const HLSLEmitterContext *ctx, int index) {
    if (!ctx->matrix_use_capture) return false;
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[index];
    const HLSLCurrentMatrixReads *reads = &ctx->matrix_use_capture->reads;
    if (!layout->omit_declaration || !layout->is_unity_builtin || layout->raw_storage ||
        layout->row_struct_storage || layout->reg < 0 || layout->reg >= 15) return false;
    bool saw_read = false;
    for (int instruction = 0; instruction < ctx->program->instruction_count; ++instruction) {
        const USILInstruction *owner = &ctx->program->instructions[instruction];
        for (int operand = 0; operand < owner->operand_count; ++operand) {
            const DXBCOperand *value = &owner->operands[operand];
            if (value->type != OPERAND_TYPE_CONSTANT_BUFFER || value->register_index != layout->reg) continue;
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, owner, operand, &use) ||
                use.use != USIL_OPERAND_USE_SOURCE || !use.source_lane_mask) return false;
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane))) continue;
                size_t matches = 0;
                for (size_t read_index = 0; read_index < reads->read_count; ++read_index) {
                    const HLSLCurrentMatrixRead *read = &reads->reads[read_index];
                    if (read->instruction_index != instruction || read->operand_index != operand ||
                        read->logical_lane != lane || read->field_index >= reads->field_count) continue;
                    const HLSLCurrentMatrixField *field = &reads->fields[read->field_index];
                    if (field->binding_register == (uint32_t)layout->reg && layout->serialized_name &&
                        !strcmp(field->block_name, layout->serialized_name)) ++matches;
                }
                if (matches != 1) return false;
                saw_read = true;
            }
        }
    }
    return saw_read;
}

bool hlsl_stage_coverage_begin(HLSLEmitterContext *ctx) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    if (coverage->began || !hlsl_stage_coverage_hull_icb_empty(&coverage->hull_icb) ||
        !ctx->program || !ctx->program->instructions || ctx->program->instruction_count < 1 ||
        ctx->program->instruction_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT) return false;
    coverage->began = true;
    coverage->stage = ctx->program->program_type;
    coverage->instruction_count = (size_t)ctx->program->instruction_count;
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        if (coverage->stage != DXBC_PROGRAM_TYPE_HULL ||
            !hlsl_hull_owned_contract_digest(ctx->program, coverage->hull_owner_digest) ||
            !hull_contract_copy(&coverage->hull_contract, ctx->program) ||
            !hull_contract_copy(&coverage->recorded_hull_contract, ctx->program)) return false;
        /* Units and owned roots preserve evidence; they do not close HULL body
         * or declaration completeness. The later target factory owns replay. */
        coverage->obligations |= HLSL_STAGE_COVERAGE_BODY | HLSL_STAGE_COVERAGE_LOCAL_DECLARATION;
    } else if (coverage->schema != HLSL_STAGE_COVERAGE_ORDINARY_ENTRY) return false;
    if (!hlsl_source_quality_body_inventory_supported(ctx)) coverage->obligations |= HLSL_STAGE_COVERAGE_BODY;
    for (int index = 0; index < ctx->program->instruction_count; ++index) {
        const USILInstruction *owner = &ctx->program->instructions[index];
        if (!opcode_valid(owner->opcode)) return false;
        coverage->opcodes[index] = owner->opcode;
        coverage->source_instructions[index] = owner->source_instruction_index;
        coverage->destination_lanes[index] = owner->operand_count
            ? usil_operand_destination_lane_mask(&owner->operands[0]) : 0;
        if (owner->operand_count < 0 || owner->operand_count > DXBC_MAX_OPERANDS) return false;
        coverage->operand_counts[index] = (uint8_t)owner->operand_count;
        for (int operand = 0; operand < owner->operand_count; ++operand) {
            const DXBCOperand *value = &owner->operands[operand];
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, owner, operand, &use)) return false;
            coverage->operand_uses[index][operand] = (HLSLStageOwnedOperandUse){
                .type = value->type, .source_lanes = use.source_lane_mask,
                .is_source = use.use == USIL_OPERAND_USE_SOURCE,
                .absolute = value->has_abs, .negative = value->has_neg};
        }
    }
    for (int index = 0; index < ctx->cbuffer_layout_count; ++index) {
        if (hlsl_source_quality_local_cbuffer_supported(ctx, index)) continue;
        if (omitted_matrix_block_supported(ctx, index)) {
            coverage->required_binding_mask |= UINT32_C(1) << ctx->cbuffer_layouts[index].reg;
            coverage->obligations |= HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION;
        } else coverage->obligations |= HLSL_STAGE_COVERAGE_LOCAL_DECLARATION;
    }
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        memcpy(coverage->recorded_opcodes, coverage->opcodes, sizeof(coverage->opcodes));
        memcpy(coverage->recorded_source_instructions, coverage->source_instructions, sizeof(coverage->source_instructions));
        memcpy(coverage->recorded_destination_lanes, coverage->destination_lanes, sizeof(coverage->destination_lanes));
        memcpy(coverage->recorded_operand_counts, coverage->operand_counts, sizeof(coverage->operand_counts));
        memcpy(coverage->recorded_operand_uses, coverage->operand_uses, sizeof(coverage->operand_uses));
    }
    return true;
}

static bool unit_kind_matches(HLSLStageCoverageSchema schema, uint32_t id, HLSLSourceQualityUnitKind kind) {
    if (schema == HLSL_STAGE_COVERAGE_ORDINARY_ENTRY) return id == 0 && kind == HLSL_SOURCE_UNIT_ENTRY_POINT;
    static const HLSLSourceQualityUnitKind kinds[] = {
        HLSL_SOURCE_UNIT_CONFIGURATION, HLSL_SOURCE_UNIT_HELPER, HLSL_SOURCE_UNIT_ENTRY_POINT};
    return schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT && id < 3 && kind == kinds[id];
}

static void close_unit(HLSLStageCoverage *coverage, size_t end) {
    if (!coverage->unit_count) return;
    HLSLStageOwnedUnit *unit = &coverage->units[coverage->unit_count - 1];
    unit->end = end;
    unit->root_end = coverage->root_count;
    unit->syntax_end = coverage->syntax_count;
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        HLSLStageOwnedUnit *recorded = &coverage->recorded_units[coverage->unit_count - 1];
        recorded->end = end;
        recorded->root_end = coverage->recorded_root_count;
        recorded->syntax_end = coverage->recorded_syntax_count;
    }
}

bool hlsl_stage_coverage_begin_unit(HLSLEmitterContext *ctx, uint32_t id, HLSLSourceQualityUnitKind kind) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    if (!coverage->began || coverage->finished || !ctx->sb || id != coverage->unit_count ||
        coverage->unit_count == 3 || !unit_kind_matches(coverage->schema, id, kind)) return false;
    close_unit(coverage, ctx->sb->len);
    HLSLStageOwnedUnit *unit = &coverage->units[coverage->unit_count++];
    *unit = (HLSLStageOwnedUnit){.source_unit_id = id, .kind = kind, .begin = ctx->sb->len,
        .root_begin = coverage->root_count, .syntax_begin = coverage->syntax_count};
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        unit->obligations = id == 0 ? HLSL_STAGE_COVERAGE_LOCAL_DECLARATION : HLSL_STAGE_COVERAGE_BODY;
        coverage->recorded_units[id] = *unit;
        coverage->recorded_unit_count = coverage->unit_count;
    }
    return true;
}

static int instruction_phase(const HLSLStageCoverage *coverage, int instruction) {
    if (coverage->schema != HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) return -1;
    for (size_t phase = 0; phase < coverage->hull_contract.tessellation.phase_count; ++phase) {
        const USILHullPhase *scope = &coverage->hull_contract.phases[phase];
        if (instruction >= scope->first_instruction_index && instruction < scope->end_instruction_index) return (int)phase;
    }
    return -1;
}

static bool icb_plan_valid(const HLSLStageCoverage *coverage) {
    const HLSLStageHullICB *icb = &coverage->hull_icb;
    const HLSLHullICBPlan *plan = &icb->plan;
    if (!icb->plan_captured || !icb->recorded_plan_captured ||
        !hlsl_hull_icb_plans_equal(plan, &icb->recorded_plan)) return false;
    if (!plan->present) {
        const HLSLHullICBPlan empty = {0};
        return hlsl_hull_icb_plans_equal(plan, &empty);
    }
    const HLSLStageHullContract *contract = &coverage->hull_contract;
    if (coverage->schema != HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT ||
        !contract->tessellation.phase_count || contract->tessellation.phase_count > 3 ||
        contract->signature_declaration_count < 1 || contract->signature_declaration_count > 11 ||
        !coverage->instruction_count || coverage->instruction_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
        plan->row_count < 2 || plan->row_count > HLSL_HULL_ICB_ROW_LIMIT ||
        plan->payload_count != plan->row_count * 4 || plan->physical_column >= 4 ||
        plan->declaration_word_count != plan->payload_count + 2 ||
        plan->phase_index < 0 || (size_t)plan->phase_index >= contract->tessellation.phase_count ||
        contract->phases[0].kind == DXBC_HULL_PHASE_CONTROL_POINT ||
        !phase_equal(&plan->phase, &contract->phases[plan->phase_index]) ||
        plan->phase.kind != DXBC_HULL_PHASE_FORK || plan->phase.instance_count != plan->row_count ||
        plan->phase.first_instruction_index < 0 || plan->phase.end_instruction_index <= plan->phase.first_instruction_index ||
        plan->phase.end_instruction_index > (int)coverage->instruction_count ||
        plan->declaration_source_instruction_index >= contract->phases[0].marker_source_instruction_index ||
        plan->fork_declaration_index >= (size_t)contract->signature_declaration_count ||
        !declaration_equal(&plan->fork_declaration, &contract->signature_declarations[plan->fork_declaration_index]) ||
        plan->fork_declaration.operand_type != OPERAND_TYPE_FORK_INSTANCE_ID ||
        !plan->consumer_count || plan->consumer_count > HLSL_HULL_ICB_CONSUMER_LIMIT ||
        plan->transport_count > HLSL_HULL_ICB_TRANSPORT_LIMIT ||
        !memchr(plan->array_name, 0, sizeof(plan->array_name)) ||
        !memchr(plan->index_name, 0, sizeof(plan->index_name)) ||
        !hlsl_source_identifier_valid(plan->array_name) || !hlsl_source_identifier_valid(plan->index_name) ||
        !strcmp(plan->array_name, plan->index_name)) return false;
    for (size_t word = 0; word < HLSL_HULL_ICB_WORD_LIMIT; ++word) {
        const bool selected = word < plan->payload_count && word % 4 == plan->physical_column;
        if (selected ? (plan->payload[word] & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) : plan->payload[word] != 0)
            return false;
    }
    for (size_t index = 0; index < plan->transport_count; ++index) {
        const HLSLHullICBTransport *edge = &plan->transports[index];
        const int instruction = edge->instruction_index;
        if (instruction < plan->phase.first_instruction_index || instruction >= plan->phase.end_instruction_index ||
            instruction < 0 || (size_t)instruction >= coverage->instruction_count ||
            coverage->opcodes[instruction] != USIL_OP_MOV || coverage->operand_counts[instruction] != 2 ||
            edge->source_instruction_index != coverage->source_instructions[instruction] ||
            edge->destination.type != OPERAND_TYPE_TEMP || edge->destination_lane >= 4 || edge->source_lane >= 4 ||
            coverage->destination_lanes[instruction] != (uint8_t)(1u << edge->destination_lane) ||
            !coverage->operand_uses[instruction][1].is_source ||
            edge->source.type != coverage->operand_uses[instruction][1].type) return false;
        for (size_t previous = 0; previous < index; ++previous)
            if (plan->transports[previous].instruction_index == instruction &&
                plan->transports[previous].destination_lane == edge->destination_lane) return false;
        if (edge->predecessor_transport == -1) {
            if (edge->source.type != OPERAND_TYPE_FORK_INSTANCE_ID || edge->source_lane) return false;
        } else {
            if (edge->predecessor_transport < 0 || (size_t)edge->predecessor_transport >= plan->transport_count ||
                edge->source.type != OPERAND_TYPE_TEMP) return false;
            const HLSLHullICBTransport *previous = &plan->transports[edge->predecessor_transport];
            if (previous->instruction_index >= instruction || previous->destination.register_index != edge->source.register_index ||
                previous->destination_lane != edge->source_lane) return false;
        }
    }
    bool used[HLSL_HULL_ICB_TRANSPORT_LIMIT] = {false};
    for (size_t index = 0; index < plan->consumer_count; ++index) {
        const HLSLHullICBConsumer *consumer = &plan->consumers[index];
        const int instruction = consumer->instruction_index, operand = consumer->operand_index;
        if (instruction < plan->phase.first_instruction_index || instruction >= plan->phase.end_instruction_index ||
            instruction < 0 || (size_t)instruction >= coverage->instruction_count || operand < 1 ||
            coverage->operand_counts[instruction] > DXBC_MAX_OPERANDS ||
            operand >= (int)coverage->operand_counts[instruction] ||
            consumer->source_instruction_index != coverage->source_instructions[instruction] ||
            consumer->physical_column != plan->physical_column || !consumer->demanded_lanes ||
            consumer->demanded_lanes & ~15u || consumer->operand.type != OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER ||
            consumer->operand.swizzle_mode != 2 || consumer->operand.swizzle[0] != plan->physical_column ||
            !coverage->operand_uses[instruction][operand].is_source ||
            coverage->operand_uses[instruction][operand].type != OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER ||
            coverage->operand_uses[instruction][operand].source_lanes != consumer->demanded_lanes ||
            (index && (instruction < plan->consumers[index - 1].instruction_index ||
                (instruction == plan->consumers[index - 1].instruction_index && operand <= plan->consumers[index - 1].operand_index))))
            return false;
        if (consumer->direct_fork_id) {
            if (consumer->transport_tail != -1 || consumer->relative.type != OPERAND_TYPE_FORK_INSTANCE_ID) return false;
            continue;
        }
        if (consumer->relative.type != OPERAND_TYPE_TEMP || consumer->relative.swizzle_mode != 2 ||
            consumer->transport_tail < 0 || (size_t)consumer->transport_tail >= plan->transport_count) return false;
        const HLSLHullICBTransport *tail = &plan->transports[consumer->transport_tail];
        if (tail->instruction_index >= instruction || tail->destination.register_index != consumer->relative.register_index ||
            tail->destination_lane != consumer->relative.swizzle[0]) return false;
        int edge = consumer->transport_tail;
        for (size_t depth = 0; edge >= 0; ++depth) {
            if (depth >= HLSL_HULL_ICB_TRANSPORT_LIMIT || (size_t)edge >= plan->transport_count) return false;
            used[edge] = true;
            edge = plan->transports[edge].predecessor_transport;
        }
    }
    for (size_t index = 0; index < plan->transport_count; ++index)
        if (!used[index]) return false;
    return true;
}

bool hlsl_stage_coverage_hull_icb_plan(HLSLEmitterContext *ctx, const HLSLHullICBPlan *plan) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    if (!coverage->began || coverage->finished || coverage->schema != HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT ||
        coverage->unit_count || coverage->root_count || coverage->syntax_count || !plan ||
        !hlsl_stage_coverage_hull_icb_empty(&coverage->hull_icb) || !hlsl_hull_icb_plan_matches(ctx->program, plan)) return false;
    coverage->hull_icb.plan = *plan;
    coverage->hull_icb.recorded_plan = *plan;
    coverage->hull_icb.plan_captured = coverage->hull_icb.recorded_plan_captured = true;
    return icb_plan_valid(coverage);
}

static bool root_owners_equal(const HLSLStageRootOwner *a, const HLSLStageRootOwner *b) {
    return a->kind == b->kind && a->phase_index == b->phase_index &&
        a->source_instruction_index == b->source_instruction_index && a->value_bits == b->value_bits &&
        a->instance_count == b->instance_count && a->input_signature_index == b->input_signature_index &&
        a->output_signature_index == b->output_signature_index && a->icb_index == b->icb_index;
}

static bool root_owner_valid(const HLSLStageCoverage *coverage, const HLSLStageRootOwner *owner,
    int instruction, uint32_t unit) {
    if (!owner || owner->input_signature_index || owner->output_signature_index) return false;
    if (owner->kind == HLSL_STAGE_ROOT_HULL_ICB_LITERAL || owner->kind == HLSL_STAGE_ROOT_HULL_ICB_ACCESS) {
        const HLSLHullICBPlan *plan = &coverage->hull_icb.plan;
        if (!icb_plan_valid(coverage) || !plan->present || owner->phase_index != plan->phase_index || owner->instance_count)
            return false;
        if (owner->kind == HLSL_STAGE_ROOT_HULL_ICB_LITERAL)
            return unit == 0 && instruction == -1 && owner->icb_index < plan->row_count &&
                owner->source_instruction_index == plan->declaration_source_instruction_index &&
                owner->value_bits == plan->payload[owner->icb_index * 4 + plan->physical_column];
        if (unit != 1 || owner->icb_index >= plan->consumer_count || owner->value_bits) return false;
        const HLSLHullICBConsumer *consumer = &plan->consumers[owner->icb_index];
        return instruction == consumer->instruction_index && owner->source_instruction_index == consumer->source_instruction_index;
    }
    if (owner->icb_index) return false;
    if (owner->kind == HLSL_STAGE_ROOT_INSTRUCTION) {
        if (instruction < 0 || (size_t)instruction >= coverage->instruction_count || owner->value_bits || owner->instance_count ||
            owner->source_instruction_index != coverage->source_instructions[instruction] ||
            owner->phase_index != instruction_phase(coverage, instruction)) return false;
        if (coverage->schema != HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) return true;
        return owner->phase_index >= 0 && unit ==
            (coverage->hull_contract.phases[owner->phase_index].kind == DXBC_HULL_PHASE_CONTROL_POINT ? 2u : 1u);
    }
    if (coverage->schema != HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) return false;
    const HLSLStageHullContract *contract = &coverage->hull_contract;
    if (owner->kind == HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE) {
        if (owner->phase_index < 0 || (size_t)owner->phase_index >= contract->tessellation.phase_count || owner->value_bits ||
            instruction < 0 || (size_t)instruction >= coverage->instruction_count || coverage->opcodes[instruction] != USIL_OP_MOV ||
            coverage->operand_counts[instruction] != 2 || instruction_phase(coverage, instruction) != owner->phase_index) return false;
        const USILHullPhase *phase = &contract->phases[owner->phase_index];
        return owner->source_instruction_index == phase->marker_source_instruction_index &&
            owner->instance_count == phase->instance_count && unit == (phase->kind == DXBC_HULL_PHASE_CONTROL_POINT ? 2u : 1u);
    }
    if (instruction != -1 || owner->instance_count) return false;
    if (owner->kind == HLSL_STAGE_ROOT_HULL_MAXIMUM)
        return unit == 2 && owner->phase_index == -1 &&
            owner->source_instruction_index == contract->tessellation.max_tessellation_factor_source_instruction_index &&
            owner->value_bits == contract->tessellation.max_tessellation_factor_bits;
    if (owner->value_bits || owner->source_instruction_index != UINT32_MAX) return false;
    if (owner->kind == HLSL_STAGE_ROOT_HULL_FACTOR_RETURN) return unit == 1 && owner->phase_index == -1;
    if (owner->kind == HLSL_STAGE_ROOT_HULL_IMPLICIT_COPY)
        return unit == 2 && owner->phase_index == -1 && contract->phases[0].kind != DXBC_HULL_PHASE_CONTROL_POINT &&
            contract->tessellation.input_control_point_count == contract->tessellation.output_control_point_count &&
            contract->input.mask == contract->output.mask && contract->input.register_id == contract->output.register_id &&
            contract->input.semantic_index == contract->output.semantic_index && contract->input.system_value == contract->output.system_value &&
            contract->input.component_type == contract->output.component_type &&
            !strcmp(dxbc_signature_semantic_name(&contract->input), dxbc_signature_semantic_name(&contract->output));
    if (owner->kind == HLSL_STAGE_ROOT_HULL_POINT_RETURN)
        return unit == 2 && owner->phase_index == 0 && contract->phases[0].kind == DXBC_HULL_PHASE_CONTROL_POINT;
    return false;
}

bool hlsl_stage_coverage_owned_root(HLSLEmitterContext *ctx, const ASTExpr *root, int instruction,
    const HLSLStageRootOwner *owner) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    const uint32_t unit = coverage->unit_count ? coverage->units[coverage->unit_count - 1].source_unit_id : 0;
    const HLSLSourceQualityUnitKind kind = coverage->unit_count ? coverage->units[coverage->unit_count - 1].kind : HLSL_SOURCE_UNIT_ENTRY_POINT;
    if (!coverage->began || coverage->finished || !root_owner_valid(coverage, owner, instruction, unit) ||
        coverage->root_count == HLSL_STAGE_COVERAGE_ROOT_LIMIT) return false;
    size_t nodes;
    ASTExpr *copy = hlsl_owned_expression_copy(root, &nodes);
    if (!copy || coverage->node_count > HLSL_STAGE_COVERAGE_NODE_LIMIT ||
        nodes > HLSL_STAGE_COVERAGE_NODE_LIMIT - coverage->node_count ||
        (coverage->global_node_count && (*coverage->global_node_count > HLSL_STAGE_COVERAGE_GLOBAL_NODE_LIMIT ||
            nodes > HLSL_STAGE_COVERAGE_GLOBAL_NODE_LIMIT - *coverage->global_node_count))) {
        ast_free_expr(copy);
        return false;
    }
    size_t recorded_nodes;
    ASTExpr *recorded = hlsl_owned_expression_copy(copy, &recorded_nodes);
    if (!recorded || recorded_nodes != nodes) {
        ast_free_expr(copy);
        ast_free_expr(recorded);
        return false;
    }
    HLSLStageOwnedRoot *roots = realloc(coverage->roots, (coverage->root_count + 1) * sizeof(*roots));
    if (!roots) { ast_free_expr(copy); ast_free_expr(recorded); return false; }
    coverage->roots = roots;
    coverage->roots[coverage->root_count++] = (HLSLStageOwnedRoot){
        .tree = copy, .recorded_tree = recorded, .live_tree = root, .instruction = instruction,
        .source_unit_id = unit, .unit_kind = kind, .owner = *owner, .recorded_owner = *owner};
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT)
        ++coverage->recorded_root_count;
    coverage->node_count += nodes;
    if (coverage->global_node_count) *coverage->global_node_count += nodes;
    return true;
}

bool hlsl_stage_coverage_root(HLSLEmitterContext *ctx, const ASTExpr *root, int instruction) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    if (instruction < 0 || (size_t)instruction >= coverage->instruction_count) return false;
    const HLSLStageRootOwner owner = {.kind = HLSL_STAGE_ROOT_INSTRUCTION,
        .phase_index = instruction_phase(coverage, instruction), .source_instruction_index = coverage->source_instructions[instruction]};
    return hlsl_stage_coverage_owned_root(ctx, root, instruction, &owner);
}

bool hlsl_stage_coverage_hull_icb_literal(HLSLEmitterContext *ctx, const ASTExpr *root, size_t row) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    HLSLStageHullICB *icb = &coverage->hull_icb;
    if (!icb_plan_valid(coverage) || !icb->plan.present || coverage->finished || !coverage->unit_count ||
        coverage->unit_count != 1 || icb->declaration_emitted || row != icb->literal_root_count ||
        row >= icb->plan.row_count || !root || root->kind != AST_EXPR_LITERAL ||
        root->u.literal.components != 1 || root->u.literal.scalar_type != AST_SCALAR_FLOAT32 ||
        root->u.literal.val[0] != icb->plan.payload[row * 4 + icb->plan.physical_column]) return false;
    const HLSLStageRootOwner owner = {.kind = HLSL_STAGE_ROOT_HULL_ICB_LITERAL, .phase_index = icb->plan.phase_index,
        .source_instruction_index = icb->plan.declaration_source_instruction_index,
        .value_bits = root->u.literal.val[0], .icb_index = (uint32_t)row};
    const size_t index = coverage->root_count;
    if (!hlsl_stage_coverage_owned_root(ctx, root, -1, &owner)) return false;
    icb->literal_root_indices[row] = icb->recorded_literal_root_indices[row] = index;
    ++icb->literal_root_count;
    ++icb->recorded_literal_root_count;
    return true;
}

static bool icb_access_tree_valid(const HLSLStageCoverage *coverage, const ASTExpr *tree, size_t index) {
    const HLSLHullICBPlan *plan = &coverage->hull_icb.plan;
    if (!plan->present || index >= plan->consumer_count || !tree || tree->kind != AST_EXPR_EMITTER_OPERAND ||
        !tree->u.emitter_operand) return false;
    const HLSLHullICBConsumer *consumer = &plan->consumers[index];
    const ASTOperandProvenance *origin = &tree->operand_provenance;
    if (!origin->complete || origin->value_role != AST_OPERAND_VALUE_LOGICAL ||
        origin->logical_value_id != (HLSL_HULL_ICB_ACCESS_LOGICAL_ID_BASE | index) ||
        origin->natural_components != 1 || origin->result_components != 1 ||
        origin->selection_role != AST_COMPONENT_SELECTION_NONE || origin->bitcast_role != AST_OPERAND_BITCAST_NONE ||
        origin->raw_buffer_reconstruction || origin->synthetic_interface ||
        origin->instruction_index != consumer->instruction_index || origin->source_instruction_index != consumer->source_instruction_index ||
        origin->operand_index != consumer->operand_index || origin->destination_lanes != consumer->demanded_lanes) return false;
    for (size_t lane = 0; lane < 4; ++lane)
        if (origin->selected_components[lane]) return false;
    const size_t array_length = strlen(plan->array_name), index_length = strlen(plan->index_name);
    return strlen(tree->u.emitter_operand) == array_length + index_length + 2 &&
        !memcmp(tree->u.emitter_operand, plan->array_name, array_length) &&
        tree->u.emitter_operand[array_length] == '[' &&
        !memcmp(tree->u.emitter_operand + array_length + 1, plan->index_name, index_length) &&
        tree->u.emitter_operand[array_length + index_length + 1] == ']';
}

bool hlsl_stage_coverage_hull_icb_access(HLSLEmitterContext *ctx, const ASTExpr *root, size_t consumer) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    HLSLStageHullICB *icb = &coverage->hull_icb;
    if (!icb_plan_valid(coverage) || !icb->plan.present || coverage->finished || coverage->unit_count != 2 ||
        !icb->declaration_emitted || consumer >= icb->plan.consumer_count ||
        icb->access_root_count >= icb->plan.consumer_count || !icb_access_tree_valid(coverage, root, consumer)) return false;
    /* Access construction can follow inlining order rather than raw program
     * order. One actual consumer coordinate still acquires exactly one root. */
    for (size_t index = 0; index < coverage->root_count; ++index)
        if (coverage->roots[index].owner.kind == HLSL_STAGE_ROOT_HULL_ICB_ACCESS &&
            coverage->roots[index].owner.icb_index == consumer) return false;
    const HLSLHullICBConsumer *use = &icb->plan.consumers[consumer];
    const HLSLStageRootOwner owner = {.kind = HLSL_STAGE_ROOT_HULL_ICB_ACCESS, .phase_index = icb->plan.phase_index,
        .source_instruction_index = use->source_instruction_index, .icb_index = (uint32_t)consumer};
    const size_t index = coverage->root_count;
    if (!hlsl_stage_coverage_owned_root(ctx, root, use->instruction_index, &owner)) return false;
    icb->access_root_indices[consumer] = icb->recorded_access_root_indices[consumer] = index;
    ++icb->access_root_count;
    ++icb->recorded_access_root_count;
    return true;
}

bool hlsl_stage_coverage_hull_icb_declaration(HLSLEmitterContext *ctx, size_t begin, size_t end) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage) return true;
    HLSLStageHullICB *icb = &coverage->hull_icb;
    if (!icb_plan_valid(coverage) || !icb->plan.present || coverage->finished || coverage->unit_count != 1 ||
        !ctx->sb || !sb_ok(ctx->sb) || icb->declaration_emitted || begin >= end || end > ctx->sb->len ||
        begin < coverage->units[0].begin || icb->literal_root_count != icb->plan.row_count ||
        icb->literal_root_count != icb->recorded_literal_root_count) return false;
    for (size_t row = 0; row < icb->literal_root_count; ++row) {
        const size_t index = icb->literal_root_indices[row];
        if (index != icb->recorded_literal_root_indices[row] || index >= coverage->root_count ||
            !coverage->roots[index].emitted || coverage->roots[index].live_tree ||
            coverage->roots[index].begin < begin || coverage->roots[index].end > end) return false;
    }
    icb->declaration_begin = icb->recorded_declaration_begin = begin;
    icb->declaration_end = icb->recorded_declaration_end = end;
    icb->declaration_emitted = icb->recorded_declaration_emitted = true;
    return true;
}

bool hlsl_stage_coverage_observation(HLSLEmitterContext *ctx, const HLSLSourceQualityObservation *observation) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage || (observation->kind != HLSL_SOURCE_OBSERVATION_EMISSION &&
        !(coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT && observation->kind == HLSL_SOURCE_OBSERVATION_COVERAGE))) return true;
    if (!coverage->began || coverage->finished || !unit_kind_matches(coverage->schema, observation->source_unit_id, observation->unit_kind) ||
        (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT && observation->source_unit_id >= coverage->unit_count) ||
        coverage->syntax_count >= HLSL_STAGE_COVERAGE_EVENT_LIMIT ||
        (coverage->global_event_count && *coverage->global_event_count >= HLSL_STAGE_COVERAGE_GLOBAL_EVENT_LIMIT))
        return false;
    HLSLStageOwnedSyntax *syntax = realloc(coverage->syntax, (coverage->syntax_count + 1) * sizeof(*syntax));
    if (!syntax) return false;
    coverage->syntax = syntax;
    const HLSLStageOwnedSyntax event = {
        .facts = observation->facts, .source_end = ctx->sb->len,
        .source_unit_id = observation->source_unit_id, .unit_kind = observation->unit_kind,
        .kind = observation->kind, .reasons = observation->reasons};
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        if (coverage->recorded_syntax_count != coverage->syntax_count) return false;
        HLSLStageOwnedSyntax *recorded = realloc(coverage->recorded_syntax,
            (coverage->recorded_syntax_count + 1) * sizeof(*recorded));
        if (!recorded) return false;
        coverage->recorded_syntax = recorded;
        coverage->recorded_syntax[coverage->recorded_syntax_count++] = event;
    }
    coverage->syntax[coverage->syntax_count++] = event;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_COVERAGE) {
        coverage->units[observation->source_unit_id].obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
        coverage->recorded_units[observation->source_unit_id].obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
        coverage->obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
    }
    if (coverage->global_event_count) ++*coverage->global_event_count;
    return true;
}

bool hlsl_stage_coverage_span(HLSLStageCoverage *coverage, const ASTExpr *root, size_t begin, size_t end) {
    if (!coverage) return true;
    const bool icb_access = root && root->kind == AST_EXPR_EMITTER_OPERAND &&
        (root->operand_provenance.logical_value_id & ~UINT64_C(63)) == HLSL_HULL_ICB_ACCESS_LOGICAL_ID_BASE;
    if (icb_access) {
        const size_t consumer = (size_t)(root->operand_provenance.logical_value_id & UINT64_C(63));
        if (!coverage->began || coverage->finished || !icb_plan_valid(coverage) ||
            !icb_access_tree_valid(coverage, root, consumer) || begin >= end) return false;
        size_t matches = 0;
        for (size_t index = 0; index < coverage->root_count; ++index) {
            const HLSLStageOwnedRoot *owned = &coverage->roots[index];
            if (owned->owner.kind != HLSL_STAGE_ROOT_HULL_ICB_ACCESS || owned->owner.icb_index != consumer) continue;
            if (owned->live_tree != root || owned->emitted ||
                coverage->hull_icb.access_root_indices[consumer] != index ||
                coverage->hull_icb.recorded_access_root_indices[consumer] != index) return false;
            ++matches;
        }
        /* Clearing the first live pointer must not make a second spelling,
         * copied child or unregistered access invisible to the trace. */
        if (matches != 1) return false;
    }
    for (size_t index = 0; index < coverage->root_count; ++index) {
        HLSLStageOwnedRoot *owned = &coverage->roots[index];
        if (owned->live_tree != root) continue;
        if (owned->emitted || begin >= end) return false;
        owned->begin = begin;
        owned->end = end;
        owned->emitted = true;
        owned->live_tree = NULL;
    }
    return true;
}

static bool icb_emission_valid(const HLSLStageCoverage *coverage) {
    const HLSLStageHullICB *icb = &coverage->hull_icb;
    const HLSLHullICBPlan *plan = &icb->plan;
    if (coverage->root_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT || (coverage->root_count && !coverage->roots) ||
        !icb_plan_valid(coverage) || icb->declaration_emitted != icb->recorded_declaration_emitted ||
        icb->declaration_begin != icb->recorded_declaration_begin || icb->declaration_end != icb->recorded_declaration_end ||
        icb->literal_root_count != icb->recorded_literal_root_count ||
        icb->access_root_count != icb->recorded_access_root_count ||
        icb->literal_root_count > HLSL_HULL_ICB_ROW_LIMIT || icb->access_root_count > HLSL_HULL_ICB_CONSUMER_LIMIT) return false;
    for (size_t row = 0; row < HLSL_HULL_ICB_ROW_LIMIT; ++row)
        if (icb->literal_root_indices[row] != icb->recorded_literal_root_indices[row] ||
            (row >= icb->literal_root_count && icb->literal_root_indices[row])) return false;
    for (size_t index = 0; index < HLSL_HULL_ICB_CONSUMER_LIMIT; ++index)
        if (icb->access_root_indices[index] != icb->recorded_access_root_indices[index] ||
            (index >= plan->consumer_count && icb->access_root_indices[index])) return false;
    if (!plan->present)
        return !icb->declaration_emitted && !icb->declaration_begin && !icb->declaration_end &&
            !icb->literal_root_count && !icb->access_root_count;
    if (!icb->declaration_emitted || icb->literal_root_count != plan->row_count ||
        icb->access_root_count != plan->consumer_count || coverage->unit_count != 3 ||
        icb->declaration_begin < coverage->units[0].begin || icb->declaration_begin >= icb->declaration_end ||
        icb->declaration_end > coverage->units[0].end) return false;
    bool literal_seen[HLSL_HULL_ICB_ROW_LIMIT] = {false}, access_seen[HLSL_HULL_ICB_CONSUMER_LIMIT] = {false};
    size_t previous = icb->declaration_begin;
    for (size_t row = 0; row < plan->row_count; ++row) {
        const size_t index = icb->literal_root_indices[row];
        if (index >= coverage->root_count) return false;
        const HLSLStageOwnedRoot *root = &coverage->roots[index];
        if (root->owner.kind != HLSL_STAGE_ROOT_HULL_ICB_LITERAL || root->owner.icb_index != row ||
            root->source_unit_id != 0 || !root->emitted || root->live_tree ||
            root->begin < previous || root->begin >= root->end || root->end > icb->declaration_end) return false;
        previous = root->end;
    }
    for (size_t consumer = 0; consumer < plan->consumer_count; ++consumer) {
        const size_t index = icb->access_root_indices[consumer];
        if (index >= coverage->root_count) return false;
        const HLSLStageOwnedRoot *root = &coverage->roots[index];
        if (root->owner.kind != HLSL_STAGE_ROOT_HULL_ICB_ACCESS || root->owner.icb_index != consumer ||
            root->source_unit_id != 1 || !root->emitted || root->live_tree ||
            root->begin >= root->end || root->end > coverage->units[1].end ||
            root->begin < coverage->units[1].begin || !icb_access_tree_valid(coverage, root->tree, consumer)) return false;
    }
    for (size_t index = 0; index < coverage->root_count; ++index) {
        const HLSLStageOwnedRoot *root = &coverage->roots[index];
        if (root->owner.kind == HLSL_STAGE_ROOT_HULL_ICB_LITERAL) {
            const size_t row = root->owner.icb_index;
            if (row >= plan->row_count || literal_seen[row] || icb->literal_root_indices[row] != index) return false;
            literal_seen[row] = true;
        } else if (root->owner.kind == HLSL_STAGE_ROOT_HULL_ICB_ACCESS) {
            const size_t consumer = root->owner.icb_index;
            if (consumer >= plan->consumer_count || access_seen[consumer] || icb->access_root_indices[consumer] != index) return false;
            access_seen[consumer] = true;
        }
    }
    return true;
}

static bool icb_ledgers_equal(const HLSLStageHullICB *a, const HLSLStageHullICB *b) {
    if (a->plan_captured != b->plan_captured || a->recorded_plan_captured != b->recorded_plan_captured ||
        a->declaration_emitted != b->declaration_emitted || a->recorded_declaration_emitted != b->recorded_declaration_emitted ||
        a->declaration_begin != b->declaration_begin || a->declaration_end != b->declaration_end ||
        a->recorded_declaration_begin != b->recorded_declaration_begin || a->recorded_declaration_end != b->recorded_declaration_end ||
        a->literal_root_count != b->literal_root_count || a->recorded_literal_root_count != b->recorded_literal_root_count ||
        a->access_root_count != b->access_root_count || a->recorded_access_root_count != b->recorded_access_root_count ||
        !hlsl_hull_icb_plans_equal(&a->plan, &b->plan) || !hlsl_hull_icb_plans_equal(&a->recorded_plan, &b->recorded_plan))
        return false;
    for (size_t row = 0; row < HLSL_HULL_ICB_ROW_LIMIT; ++row)
        if (a->literal_root_indices[row] != b->literal_root_indices[row] ||
            a->recorded_literal_root_indices[row] != b->recorded_literal_root_indices[row]) return false;
    for (size_t index = 0; index < HLSL_HULL_ICB_CONSUMER_LIMIT; ++index)
        if (a->access_root_indices[index] != b->access_root_indices[index] ||
            a->recorded_access_root_indices[index] != b->recorded_access_root_indices[index]) return false;
    return true;
}

void hlsl_stage_coverage_finish(HLSLEmitterContext *ctx) {
    HLSLStageCoverage *coverage = current_coverage(ctx);
    if (!coverage || !coverage->began || coverage->finished) return;
    /* A changed operation owner during emission cannot be frozen as an
     * immutable successful ledger. Leave the capture unfinished on drift. */
    if (!ctx->program || !ctx->program->instructions || ctx->program->instruction_count < 1 ||
        coverage->instruction_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
        (size_t)ctx->program->instruction_count != coverage->instruction_count ||
        ctx->program->program_type != coverage->stage) return;
    for (size_t index = 0; index < coverage->instruction_count; ++index)
        if (!opcode_valid(coverage->opcodes[index]) ||
            ctx->program->instructions[index].opcode != coverage->opcodes[index]) return;
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        uint8_t digest[32];
        HLSLStageHullContract current;
        if (!ctx->sb || !sb_ok(ctx->sb) ||
            (ctx->diagnostic && ctx->diagnostic->status != HLSL_EMIT_STATUS_OK) ||
            coverage->unit_count != 3 || coverage->recorded_unit_count != 3 ||
            coverage->root_count != coverage->recorded_root_count ||
            coverage->syntax_count != coverage->recorded_syntax_count ||
            !hlsl_hull_owned_contract_digest(ctx->program, digest) ||
            memcmp(digest, coverage->hull_owner_digest, sizeof(digest)) ||
            !hull_contract_view(&current, ctx->program) || !hull_contract_equal(&current, &coverage->hull_contract) ||
            !hull_contract_equal(&coverage->hull_contract, &coverage->recorded_hull_contract) ||
            !icb_emission_valid(coverage) || !hlsl_hull_icb_plan_matches(ctx->program, &coverage->hull_icb.plan)) return;
        for (size_t index = 0; index < coverage->instruction_count; ++index) {
            const USILInstruction *owner = &ctx->program->instructions[index];
            if (owner->source_instruction_index != coverage->source_instructions[index] ||
                owner->operand_count != coverage->operand_counts[index] ||
                (owner->operand_count ? usil_operand_destination_lane_mask(&owner->operands[0]) : 0) != coverage->destination_lanes[index] ||
                coverage->opcodes[index] != coverage->recorded_opcodes[index] ||
                coverage->source_instructions[index] != coverage->recorded_source_instructions[index] ||
                coverage->destination_lanes[index] != coverage->recorded_destination_lanes[index] ||
                coverage->operand_counts[index] != coverage->recorded_operand_counts[index]) return;
            for (int operand = 0; operand < owner->operand_count; ++operand) {
                USILOperandUseInfo use;
                const DXBCOperand *value = &owner->operands[operand];
                if (!usil_instruction_operand_use(ctx->program, owner, operand, &use)) return;
                const HLSLStageOwnedOperandUse actual = {.type = value->type, .source_lanes = use.source_lane_mask,
                    .is_source = use.use == USIL_OPERAND_USE_SOURCE, .absolute = value->has_abs, .negative = value->has_neg};
                if (!operand_uses_equal(&actual, &coverage->operand_uses[index][operand]) ||
                    !operand_uses_equal(&actual, &coverage->recorded_operand_uses[index][operand])) return;
            }
        }
    }
    if (!hlsl_source_quality_interface_inventory_complete(ctx) ||
        !hlsl_source_quality_resource_inventory_complete(ctx)) coverage->obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
    for (int index = 0; index < ctx->cbuffer_layout_count; ++index)
        if (!ctx->cbuffer_layouts[index].omit_declaration &&
            !hlsl_source_quality_local_cbuffer_complete(ctx, index))
            coverage->obligations |= HLSL_STAGE_COVERAGE_LOCAL_DECLARATION;
    coverage->source_size = ctx->sb->len;
    coverage->source = malloc(coverage->source_size + 1);
    if (coverage->source) memcpy(coverage->source, ctx->sb->buf, coverage->source_size + 1);
    else coverage->obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
    if (!coverage->syntax_count || coverage->syntax[coverage->syntax_count - 1].source_end != coverage->source_size)
        coverage->obligations |= HLSL_STAGE_COVERAGE_SYNTAX;
    for (size_t index = 0; index < coverage->root_count; ++index)
        if (!coverage->roots[index].emitted || coverage->roots[index].live_tree)
            coverage->obligations |= HLSL_STAGE_COVERAGE_AST;
    if (coverage->schema == HLSL_STAGE_COVERAGE_ORDINARY_ENTRY) {
        memcpy(coverage->recorded_operand_counts, coverage->operand_counts, sizeof(coverage->operand_counts));
        memcpy(coverage->recorded_operand_uses, coverage->operand_uses, sizeof(coverage->operand_uses));
        memcpy(coverage->recorded_opcodes, coverage->opcodes, sizeof(coverage->opcodes));
    }
    close_unit(coverage, coverage->source_size);
    if (coverage->schema == HLSL_STAGE_COVERAGE_ORDINARY_ENTRY) {
        coverage->recorded_unit_count = coverage->unit_count;
        memcpy(coverage->recorded_units, coverage->units, sizeof(coverage->units));
        coverage->recorded_syntax_count = coverage->syntax_count;
        coverage->recorded_root_count = coverage->root_count;
        if (coverage->syntax_count) {
            coverage->recorded_syntax = malloc(coverage->syntax_count * sizeof(*coverage->recorded_syntax));
            if (coverage->recorded_syntax)
                memcpy(coverage->recorded_syntax, coverage->syntax,
                    coverage->syntax_count * sizeof(*coverage->recorded_syntax));
        }
    }
    coverage->finished = true;
}

static bool operand_uses_equal(const HLSLStageOwnedOperandUse *a, const HLSLStageOwnedOperandUse *b) {
    return a->type == b->type && a->source_lanes == b->source_lanes && a->is_source == b->is_source &&
        a->absolute == b->absolute && a->negative == b->negative;
}

static bool owner_valid(const HLSLStageCoverage *coverage, int instruction, uint32_t source, uint8_t lanes) {
    return instruction == -1 ? source == UINT32_MAX && !lanes : instruction >= 0 &&
        (size_t)instruction < coverage->instruction_count && source == coverage->source_instructions[instruction] &&
        !(lanes & ~coverage->destination_lanes[instruction]);
}

/* This alternative applies only to the source AST forms that the existing
 * planner stamps with a demanded source mask. Operation roots and syntax keep
 * destination ownership. An opaque operand carries its exact operand index;
 * modifier nodes without one must select one actual matching source operand.
 * Matching typed immutable trees remains a separate replay requirement. */
static bool source_owner_valid(const HLSLStageCoverage *coverage, int instruction,
    uint32_t source, uint8_t lanes, int operand, const ASTExpr *tree) {
    if (instruction < 0 || (size_t)instruction >= coverage->instruction_count ||
        source != coverage->source_instructions[instruction] || !lanes || !tree ||
        coverage->operand_counts[instruction] > DXBC_MAX_OPERANDS) return false;
    bool absolute = tree->kind == AST_EXPR_CALL && tree->u.call.name &&
        !strcmp(tree->u.call.name, "abs") && tree->u.call.arg_count == 1 && tree->u.call.args;
    bool negative = tree->kind == AST_EXPR_UNARY && tree->u.unary.op == USIL_OP_INEG;
    bool projection = tree->kind == AST_EXPR_SWIZZLE;
    if (tree->kind != AST_EXPR_EMITTER_OPERAND && !absolute && !negative && !projection) return false;
    if (operand >= (int)coverage->operand_counts[instruction] ||
        (tree->kind == AST_EXPR_EMITTER_OPERAND && operand < 0)) return false;
    const ASTExpr *child = absolute ? tree->u.call.args[0] : negative ? tree->u.unary.sub
        : projection ? tree->u.swizzle.sub : NULL;
    /* A formatter atom below a modifier already carries a concrete source
     * coordinate. Do not allow a different same-mask operand to stand in. */
    if (operand < 0 && child && child->kind == AST_EXPR_EMITTER_OPERAND &&
        child->operand_provenance.complete && child->operand_provenance.instruction_index == instruction)
        operand = child->operand_provenance.operand_index;
    size_t matches = 0;
    for (int index = 0; index < (int)coverage->operand_counts[instruction]; ++index) {
        if (operand >= 0 && index != operand) continue;
        const HLSLStageOwnedOperandUse *use = &coverage->operand_uses[instruction][index];
        if (!use->is_source || use->source_lanes != lanes ||
            (absolute && !use->absolute) || (negative && !use->negative) ||
            (projection && use->type != OPERAND_TYPE_TEMP)) continue;
        ++matches;
    }
    return matches == 1;
}

static bool tree_owners_valid(const HLSLStageCoverage *coverage, const ASTExpr *tree, unsigned depth, size_t *nodes) {
    if (!tree || depth > 64 || ++*nodes > HLSL_STAGE_COVERAGE_NODE_LIMIT) return false;
    if (tree->logical_origin.complete && !owner_valid(coverage, tree->logical_origin.instruction_index,
        tree->logical_origin.source_instruction_index, tree->logical_origin.destination_lanes) &&
        !source_owner_valid(coverage, tree->logical_origin.instruction_index,
            tree->logical_origin.source_instruction_index, tree->logical_origin.destination_lanes, -1, tree)) return false;
    if (tree->kind == AST_EXPR_EMITTER_OPERAND)
        return !tree->operand_provenance.complete || owner_valid(coverage,
            tree->operand_provenance.instruction_index, tree->operand_provenance.source_instruction_index,
            tree->operand_provenance.destination_lanes) || source_owner_valid(coverage,
                tree->operand_provenance.instruction_index, tree->operand_provenance.source_instruction_index,
                tree->operand_provenance.destination_lanes, tree->operand_provenance.operand_index, tree);
    switch (tree->kind) {
    case AST_EXPR_LITERAL:
    case AST_EXPR_VAR: return true;
    case AST_EXPR_UNARY: return tree_owners_valid(coverage, tree->u.unary.sub, depth + 1, nodes);
    case AST_EXPR_BINARY:
    case AST_EXPR_COMPARISON:
        return tree_owners_valid(coverage, tree->u.binary.left, depth + 1, nodes) &&
            tree_owners_valid(coverage, tree->u.binary.right, depth + 1, nodes);
    case AST_EXPR_SWIZZLE: return tree_owners_valid(coverage, tree->u.swizzle.sub, depth + 1, nodes);
    case AST_EXPR_CAST: return tree_owners_valid(coverage, tree->u.cast.sub, depth + 1, nodes);
    case AST_EXPR_BITCAST: return tree_owners_valid(coverage, tree->u.bitcast.sub, depth + 1, nodes);
    case AST_EXPR_TERNARY:
        return tree_owners_valid(coverage, tree->u.ternary.cond, depth + 1, nodes) &&
            tree_owners_valid(coverage, tree->u.ternary.true_expr, depth + 1, nodes) &&
            tree_owners_valid(coverage, tree->u.ternary.false_expr, depth + 1, nodes);
    case AST_EXPR_CALL:
        if (!tree->u.call.args || tree->u.call.arg_count < 1 || tree->u.call.arg_count > 4) return false;
        for (int index = 0; index < tree->u.call.arg_count; ++index)
            if (!tree_owners_valid(coverage, tree->u.call.args[index], depth + 1, nodes)) return false;
        return true;
    default: return false;
    }
}

static bool units_equal(const HLSLStageOwnedUnit *a, const HLSLStageOwnedUnit *b) {
    return a->source_unit_id == b->source_unit_id && a->kind == b->kind && a->begin == b->begin && a->end == b->end &&
        a->root_begin == b->root_begin && a->root_end == b->root_end && a->syntax_begin == b->syntax_begin &&
        a->syntax_end == b->syntax_end && a->obligations == b->obligations;
}

static bool syntax_equal(const HLSLStageOwnedSyntax *a, const HLSLStageOwnedSyntax *b) {
    return a->source_end == b->source_end && a->source_unit_id == b->source_unit_id && a->unit_kind == b->unit_kind &&
        a->kind == b->kind && a->reasons == b->reasons && hlsl_source_quality_facts_equal(&a->facts, &b->facts);
}

static bool hull_units_valid(const HLSLStageCoverage *coverage) {
    if (coverage->stage != DXBC_PROGRAM_TYPE_HULL || coverage->unit_count != 3 || coverage->recorded_unit_count != 3 ||
        !(coverage->obligations & HLSL_STAGE_COVERAGE_BODY) || !(coverage->obligations & HLSL_STAGE_COVERAGE_LOCAL_DECLARATION) ||
        !hull_contract_equal(&coverage->hull_contract, &coverage->recorded_hull_contract)) return false;
    int next_instruction = 0;
    for (size_t index = 0; index < coverage->hull_contract.tessellation.phase_count; ++index) {
        const USILHullPhase *phase = &coverage->hull_contract.phases[index];
        if ((phase->kind != DXBC_HULL_PHASE_FORK && !(index == 0 && phase->kind == DXBC_HULL_PHASE_CONTROL_POINT)) ||
            phase->first_instruction_index != next_instruction || phase->end_instruction_index <= next_instruction ||
            phase->end_instruction_index > (int)coverage->instruction_count || !phase->instance_count ||
            phase->marker_source_instruction_index >= phase->first_source_instruction_index ||
            phase->first_source_instruction_index >= phase->end_source_instruction_index) return false;
        next_instruction = phase->end_instruction_index;
    }
    if (next_instruction != (int)coverage->instruction_count) return false;
    size_t source_end = 0, root_end = 0, syntax_end = 0;
    for (size_t index = 0; index < 3; ++index) {
        const HLSLStageOwnedUnit *unit = &coverage->units[index];
        const uint32_t required = index == 0 ? HLSL_STAGE_COVERAGE_LOCAL_DECLARATION : HLSL_STAGE_COVERAGE_BODY;
        if (!units_equal(unit, &coverage->recorded_units[index]) || unit->source_unit_id != index ||
            !unit_kind_matches(coverage->schema, unit->source_unit_id, unit->kind) || unit->begin != source_end ||
            unit->begin >= unit->end || unit->end > coverage->source_size || unit->root_begin != root_end ||
            unit->root_begin > unit->root_end || unit->root_end > coverage->root_count || unit->syntax_begin != syntax_end ||
            unit->syntax_begin >= unit->syntax_end || unit->syntax_end > coverage->syntax_count || !(unit->obligations & required) ||
            (unit->obligations & ~coverage->obligations)) return false;
        for (size_t root = unit->root_begin; root < unit->root_end; ++root) {
            const HLSLStageOwnedRoot *owned = &coverage->roots[root];
            if (owned->source_unit_id != unit->source_unit_id || owned->unit_kind != unit->kind ||
                owned->begin < unit->begin || owned->end > unit->end) return false;
        }
        for (size_t event = unit->syntax_begin; event < unit->syntax_end; ++event) {
            const HLSLStageOwnedSyntax *owned = &coverage->syntax[event];
            if (owned->source_unit_id != unit->source_unit_id || owned->unit_kind != unit->kind ||
                owned->source_end < unit->begin || owned->source_end > unit->end) return false;
        }
        source_end = unit->end; root_end = unit->root_end; syntax_end = unit->syntax_end;
    }
    return source_end == coverage->source_size && root_end == coverage->root_count && syntax_end == coverage->syntax_count &&
        icb_emission_valid(coverage);
}

static bool structural_tree_valid(const HLSLStageCoverage *coverage, const HLSLStageOwnedRoot *root) {
    if (root->owner.kind == HLSL_STAGE_ROOT_INSTRUCTION) return true;
    if (root->owner.kind == HLSL_STAGE_ROOT_HULL_MAXIMUM || root->owner.kind == HLSL_STAGE_ROOT_HULL_ICB_LITERAL)
        return root->tree->kind == AST_EXPR_LITERAL && root->tree->u.literal.components == 1 &&
            root->tree->u.literal.scalar_type == AST_SCALAR_FLOAT32 && root->tree->u.literal.val[0] == root->owner.value_bits;
    if (root->owner.kind == HLSL_STAGE_ROOT_HULL_ICB_ACCESS)
        return icb_access_tree_valid(coverage, root->tree, root->owner.icb_index);
    if (root->tree->kind != AST_EXPR_EMITTER_OPERAND || !root->tree->operand_provenance.complete ||
        root->tree->operand_provenance.value_role != AST_OPERAND_VALUE_LOGICAL ||
        root->tree->operand_provenance.selection_role != AST_COMPONENT_SELECTION_NONE ||
        root->tree->operand_provenance.bitcast_role != AST_OPERAND_BITCAST_NONE ||
        root->tree->operand_provenance.raw_buffer_reconstruction || root->tree->operand_provenance.synthetic_interface ||
        root->tree->operand_provenance.operand_index !=
            (root->owner.kind == HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE ? 1 : -1)) return false;
    if (root->owner.kind == HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE)
        return root->tree->operand_provenance.natural_components == 1 &&
            root->tree->operand_provenance.result_components == 1 &&
            root->tree->operand_provenance.instruction_index == root->instruction;
    return root->tree->operand_provenance.complete &&
        root->tree->operand_provenance.value_role == AST_OPERAND_VALUE_LOGICAL &&
        root->tree->operand_provenance.natural_components == 0 && root->tree->operand_provenance.result_components == 0 &&
        root->tree->operand_provenance.instruction_index == -1 &&
        root->tree->operand_provenance.source_instruction_index == UINT32_MAX && !root->tree->operand_provenance.destination_lanes;
}

bool hlsl_stage_coverage_validate(const HLSLStageCoverage *coverage, const StringBuilder *source) {
    const uint32_t known_obligations = HLSL_STAGE_COVERAGE_BODY | HLSL_STAGE_COVERAGE_LOCAL_DECLARATION |
        HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION | HLSL_STAGE_COVERAGE_SYNTAX | HLSL_STAGE_COVERAGE_AST;
    if (!coverage || !coverage->began || !coverage->finished || !coverage->instruction_count ||
        coverage->instruction_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
        ((coverage->stage == DXBC_PROGRAM_TYPE_HULL) != (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT)) ||
        !source || !sb_ok(source) || !source->buf ||
        source->len != coverage->source_size || !coverage->source ||
        memcmp(source->buf, coverage->source, coverage->source_size + 1) ||
        (coverage->obligations & ~known_obligations) || (coverage->required_binding_mask & ~UINT32_C(0x7fff)) ||
        (!!coverage->required_binding_mask != !!(coverage->obligations & HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION)) ||
        coverage->root_count != coverage->recorded_root_count ||
        coverage->syntax_count != coverage->recorded_syntax_count ||
        coverage->root_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
        coverage->syntax_count > HLSL_STAGE_COVERAGE_EVENT_LIMIT ||
        (coverage->root_count && !coverage->roots) ||
        (coverage->syntax_count && (!coverage->syntax || !coverage->recorded_syntax)) ||
        (!(coverage->obligations & HLSL_STAGE_COVERAGE_SYNTAX) &&
            (!coverage->syntax_count || coverage->syntax[coverage->syntax_count - 1].source_end != source->len)))
        return false;
    if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        if (!hull_units_valid(coverage)) return false;
    } else if (coverage->schema != HLSL_STAGE_COVERAGE_ORDINARY_ENTRY ||
        !hlsl_stage_coverage_hull_icb_empty(&coverage->hull_icb)) return false;
    for (size_t index = 0; index < coverage->instruction_count; ++index) {
        if (!opcode_valid(coverage->opcodes[index]) ||
            coverage->opcodes[index] != coverage->recorded_opcodes[index] ||
            coverage->operand_counts[index] > DXBC_MAX_OPERANDS ||
            coverage->operand_counts[index] != coverage->recorded_operand_counts[index]) return false;
        if (coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT &&
            (coverage->source_instructions[index] != coverage->recorded_source_instructions[index] ||
             coverage->destination_lanes[index] != coverage->recorded_destination_lanes[index])) return false;
        for (unsigned operand = 0; operand < coverage->operand_counts[index]; ++operand)
            if (!operand_uses_equal(&coverage->operand_uses[index][operand],
                    &coverage->recorded_operand_uses[index][operand])) return false;
    }
    size_t previous = 0, nodes = 0;
    for (size_t index = 0; index < coverage->syntax_count; ++index) {
        const HLSLStageOwnedSyntax *syntax = &coverage->syntax[index];
        if (!syntax_equal(syntax, &coverage->recorded_syntax[index]) ||
            syntax->source_end < previous || syntax->source_end > source->len ||
            !unit_kind_matches(coverage->schema, syntax->source_unit_id, syntax->unit_kind) ||
            (syntax->kind != HLSL_SOURCE_OBSERVATION_EMISSION &&
             !(coverage->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT && syntax->kind == HLSL_SOURCE_OBSERVATION_COVERAGE)) ||
            !owner_valid(coverage, syntax->facts.instruction_index,
                syntax->facts.source_instruction_index, syntax->facts.lanes)) return false;
        if (syntax->kind == HLSL_SOURCE_OBSERVATION_COVERAGE &&
            (syntax->reasons != HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE ||
             !(coverage->units[syntax->source_unit_id].obligations & HLSL_STAGE_COVERAGE_SYNTAX))) return false;
        previous = syntax->source_end;
    }
    for (size_t index = 0; index < coverage->root_count; ++index) {
        const HLSLStageOwnedRoot *root = &coverage->roots[index];
        if (!root->emitted || root->live_tree || !root->tree ||
            !root_owners_equal(&root->owner, &root->recorded_owner) ||
            !root_owner_valid(coverage, &root->owner, root->instruction, root->source_unit_id) ||
            !unit_kind_matches(coverage->schema, root->source_unit_id, root->unit_kind) || !structural_tree_valid(coverage, root) || root->begin >= root->end ||
            root->end > source->len || !hlsl_matrix_uses_trees_equal(root->tree, root->recorded_tree) ||
            !tree_owners_valid(coverage, root->tree, 0, &nodes)) return false;
        StringBuilder formatted;
        sb_init(&formatted);
        ast_format_expr(root->tree, &formatted);
        /* The bounded HULL producer spells its typed loop atoms and aggregate
         * returns directly. EMITTER_OPERAND owns their exact payload but its
         * generic expression formatter adds parentheses. Replay that producer's
         * known atom spelling; arithmetic and maximum literals retain the full
         * generic formatting contract. No source text is parsed or rewritten. */
        const bool manual_atom = root->owner.kind == HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE ||
            root->owner.kind == HLSL_STAGE_ROOT_HULL_FACTOR_RETURN ||
            root->owner.kind == HLSL_STAGE_ROOT_HULL_IMPLICIT_COPY ||
            root->owner.kind == HLSL_STAGE_ROOT_HULL_POINT_RETURN;
        const size_t wrapper = manual_atom ? 2 : 0;
        bool valid = sb_ok(&formatted) && formatted.len >= wrapper &&
            (!manual_atom || root->tree->kind == AST_EXPR_EMITTER_OPERAND) &&
            formatted.len - wrapper == root->end - root->begin &&
            (!manual_atom || (formatted.buf[0] == '(' && formatted.buf[formatted.len - 1] == ')')) &&
            !memcmp(formatted.buf + (manual_atom ? 1 : 0), source->buf + root->begin, formatted.len - wrapper);
        sb_free(&formatted);
        if (!valid) return false;
    }
    return nodes == coverage->node_count;
}

bool hlsl_stage_coverage_equal(const HLSLStageCoverage *a, const HLSLStageCoverage *b) {
    if (!a || !b || !a->instruction_count || a->instruction_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
        (a->schema != HLSL_STAGE_COVERAGE_ORDINARY_ENTRY && a->schema != HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) ||
        ((a->stage == DXBC_PROGRAM_TYPE_HULL) != (a->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT)) ||
        a->obligations != b->obligations || a->required_binding_mask != b->required_binding_mask ||
        a->stage != b->stage || a->schema != b->schema || a->instruction_count != b->instruction_count || a->source_size != b->source_size ||
        a->node_count != b->node_count || a->root_count != b->root_count || a->syntax_count != b->syntax_count ||
        a->recorded_root_count != b->recorded_root_count || a->recorded_syntax_count != b->recorded_syntax_count ||
        !icb_ledgers_equal(&a->hull_icb, &b->hull_icb) ||
        !a->finished || !b->finished || !a->source || !b->source ||
        memcmp(a->source, b->source, a->source_size + 1)) return false;
    if (a->unit_count != b->unit_count || a->recorded_unit_count != b->recorded_unit_count || a->unit_count > 3) return false;
    if (a->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT &&
        (!hull_units_valid(a) || !hull_units_valid(b) || !hull_contract_equal(&a->hull_contract, &b->hull_contract) ||
         !hull_contract_equal(&a->recorded_hull_contract, &b->recorded_hull_contract) ||
         memcmp(a->hull_owner_digest, b->hull_owner_digest, sizeof(a->hull_owner_digest)))) return false;
    if (a->schema == HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT) {
        const StringBuilder left_source = {.buf = a->source, .len = a->source_size, .capacity = a->source_size + 1};
        const StringBuilder right_source = {.buf = b->source, .len = b->source_size, .capacity = b->source_size + 1};
        if (!hlsl_stage_coverage_validate(a, &left_source) || !hlsl_stage_coverage_validate(b, &right_source)) return false;
    } else if (!hlsl_stage_coverage_hull_icb_empty(&a->hull_icb) || !hlsl_stage_coverage_hull_icb_empty(&b->hull_icb)) return false;
    for (size_t index = 0; index < a->unit_count; ++index)
        if (!units_equal(&a->units[index], &b->units[index]) || !units_equal(&a->recorded_units[index], &b->recorded_units[index])) return false;
    for (size_t index = 0; index < a->instruction_count; ++index) {
        if (!opcode_valid(a->opcodes[index]) || !opcode_valid(b->opcodes[index]) ||
            a->opcodes[index] != a->recorded_opcodes[index] ||
            b->opcodes[index] != b->recorded_opcodes[index] ||
            a->opcodes[index] != b->opcodes[index] ||
            a->recorded_opcodes[index] != b->recorded_opcodes[index] ||
            a->operand_counts[index] > DXBC_MAX_OPERANDS ||
            a->source_instructions[index] != b->source_instructions[index] ||
            a->destination_lanes[index] != b->destination_lanes[index] ||
            a->operand_counts[index] != b->operand_counts[index] ||
            a->recorded_operand_counts[index] != b->recorded_operand_counts[index]) return false;
        for (unsigned operand = 0; operand < a->operand_counts[index]; ++operand) {
            const HLSLStageOwnedOperandUse *x = &a->operand_uses[index][operand];
            const HLSLStageOwnedOperandUse *y = &b->operand_uses[index][operand];
            if (!operand_uses_equal(x, y) || !operand_uses_equal(
                    &a->recorded_operand_uses[index][operand], &b->recorded_operand_uses[index][operand])) return false;
        }
    }
    for (size_t index = 0; index < a->syntax_count; ++index)
        if (!syntax_equal(&a->syntax[index], &b->syntax[index])) return false;
    for (size_t index = 0; index < a->root_count; ++index) {
        const HLSLStageOwnedRoot *x = &a->roots[index], *y = &b->roots[index];
        if (!x->emitted || !y->emitted || x->live_tree || y->live_tree || x->instruction != y->instruction ||
            x->source_unit_id != y->source_unit_id || x->unit_kind != y->unit_kind ||
            !root_owners_equal(&x->owner, &y->owner) || !root_owners_equal(&x->recorded_owner, &y->recorded_owner) ||
            x->begin != y->begin || x->end != y->end || x->whole_begin != y->whole_begin ||
            x->whole_end != y->whole_end || !hlsl_matrix_uses_trees_equal(x->tree, y->tree) ||
            !hlsl_matrix_uses_trees_equal(x->recorded_tree, y->recorded_tree)) return false;
    }
    return true;
}

void hlsl_stage_coverage_dispose(HLSLStageCoverage *coverage) {
    for (size_t index = 0; index < coverage->root_count; ++index) {
        ast_free_expr(coverage->roots[index].tree);
        ast_free_expr(coverage->roots[index].recorded_tree);
    }
    free(coverage->roots);
    free(coverage->syntax);
    free(coverage->recorded_syntax);
    free(coverage->source);
    hull_contract_dispose(&coverage->hull_contract);
    hull_contract_dispose(&coverage->recorded_hull_contract);
    memset(coverage, 0, sizeof(*coverage));
}
