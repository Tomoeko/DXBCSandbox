// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_identifier.h"
#include "translation/usil_validation.h"
#include "translation/hlsl_global_declarations.h"
#include "translation/hlsl_hull_icb_internal.h"
#include "common/sha256.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Independent fork phases have independent temporary definitions. This
 * producer uses the same CFG/SSA and pure expression planner as graphics,
 * retaining global semantic/raw owners. The source boundary is independent
 * scalar-factor fork phases and either signature-backed implicit copy or
 * a separately owned pure float4 control-point phase. It is not an inverse of the authored function. */
enum { HULL_SOURCE_INSTRUCTION_LIMIT = 64, HULL_SOURCE_NAME_COUNT = 12 };

typedef struct {
    HLSLDomainShape shape;
    int outer_registers[4], inner_registers[2];
    int phase, control_point_phase, outer_phase, inner_phase;
    bool inner_first;
    unsigned point_width;
    HLSLInstructionOwners index_transports;
    HLSLInstructionOwners factor_clamps;
    HLSLHullFactorClampOrigin clamp_origins[HULL_SOURCE_INSTRUCTION_LIMIT];
    HLSLHullICBPlan icb;
    size_t assignment_begin[HULL_SOURCE_INSTRUCTION_LIMIT];
    size_t assignment_end[HULL_SOURCE_INSTRUCTION_LIMIT];
    size_t maximum_attribute_begin, maximum_attribute_end;
    uint8_t decoded_owner_digest[COMMON_SHA256_DIGEST_SIZE];
    SerializedProgramParameters material_parameters[2];
    bool has_material_parameters[2];
    bool owners_frozen;
    char names[HULL_SOURCE_NAME_COUNT][96];
} HullSourcePlan;

enum { POINT_TYPE, FACTOR_TYPE, PATCH_FUNCTION, PATCH_VARIABLE, FACTOR_VARIABLE,
       POINT_FIELD, OUTER_FIELD, INNER_FIELD, FACTOR_INDEX, POINT_INDEX, POINT_VARIABLE, FACTOR_OFFSETS };

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

static bool point_signatures(const USILProgram *program, HullSourcePlan *plan,
                             bool explicit_phase) {
    const DXBCSignatureElement *input = program->inputs, *output = program->outputs;
    if (position_signature(input) && position_signature(output) &&
        !strcmp(dxbc_signature_semantic_name(input), dxbc_signature_semantic_name(output)) &&
        input->rw_mask == 15 && !output->rw_mask) {
        plan->point_width = 4;
        return true;
    }
    /* An absent control-point phase copies the complete current signature.
     * A custom FLOAT3 field has no inferred position or coordinate space.
     * Explicit phase expressions retain their separate FLOAT4 contract. */
    if (explicit_phase) return false;
    const char *semantic = dxbc_signature_semantic_name(input);
    if (!hlsl_custom_zero_index_semantic_supported(semantic) ||
        strcmp(semantic, dxbc_signature_semantic_name(output))) return false;
    const DXBCSignatureElement *elements[] = {input, output};
    for (unsigned index = 0; index < 2; ++index) {
        const DXBCSignatureElement *element = elements[index];
        if (element->register_id || element->semantic_index || element->system_value ||
            element->component_type != 3 || element->mask != 7 ||
            element->stream_index || element->min_precision || element->interpolation_mode ||
            element->rw_mask != (index ? 8 : 7)) return false;
    }
    plan->point_width = 3;
    return true;
}

/* Only a single static row is admitted here. Actual names, FLOAT scalar type,
 * current read authority and declaration inventory are checked after the normal
 * cbuffer layout builder runs; this shape check supplies no metadata authority. */
static bool scalar_cbuffer_shape(const USILProgram *program) {
    return program->cbuffer_count == 0 ||
        (program->cbuffer_count == 1 && program->cbuffer_alloc >= 1 && program->cbuffers &&
         program->cbuffers[0].reg_idx == 0 && program->cbuffers[0].size == 1 &&
         !program->cbuffers[0].dynamic_indexed);
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
    uint8_t instance_input = 0, control_point_declarations = 0;
    for (int index = 0; index < program->signature_declaration_count; ++index) {
        const USILSignatureDeclaration *declaration = &program->signature_declarations[index];
        if (declaration->has_interpolation || declaration->stream_index ||
            declaration->interpolation_mode) return false;
        int phase = -1;
        for (size_t scope = 0; scope < program->tessellation.phase_count; ++scope)
            if (declaration->source_instruction_index >= program->tessellation.phases[scope].first_source_instruction_index &&
                declaration->source_instruction_index < program->tessellation.phases[scope].end_source_instruction_index)
                phase = (int)scope; /* At most one CP phase and two factor phases. */
        if (phase < 0) return false;
        if (phase == plan->control_point_phase) {
            unsigned role = 0;
            if (!declaration->has_system_value && declaration->kind == USIL_SIGNATURE_DECL_INPUT &&
                declaration->operand_type == OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID &&
                !declaration->mask && !declaration->has_signature_register && !declaration->has_array_element_count)
                role = 1;
            else if (!declaration->has_system_value && declaration->has_signature_register &&
                     !declaration->register_id && declaration->mask == 15) {
                if (declaration->kind == USIL_SIGNATURE_DECL_INPUT && declaration->operand_type == OPERAND_TYPE_INPUT &&
                    declaration->has_array_element_count &&
                    declaration->array_element_count == program->tessellation.input_control_point_count) role = 2;
                else if (declaration->kind == USIL_SIGNATURE_DECL_OUTPUT && declaration->operand_type == OPERAND_TYPE_OUTPUT &&
                         !declaration->has_array_element_count) role = 4;
            }
            if (!role || (control_point_declarations & role)) return false;
            control_point_declarations |= (uint8_t)role;
            continue;
        }
        if (declaration->has_array_element_count) return false;
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
    return control_point_declarations == (plan->control_point_phase >= 0 ? 7 : 0) &&
        instance_input == expected_inputs &&
        coverage == (uint8_t)((1u << (plan->shape.outer_count + plan->shape.inner_count)) - 1u);
}

static bool control_point_counts_supported(const USILProgram *program, bool explicit_phase) {
    const uint32_t inputs = program->tessellation.input_control_point_count;
    const uint32_t outputs = program->tessellation.output_control_point_count;
    if (!inputs || inputs > 32 || !outputs || outputs > 32) return false;
    /* An actual CP invocation's unsigned ID is in [0, outputs). The scoped
     * source proof below only admits unmodified ID copies, so outputs <= inputs
     * proves each dynamic patch read is in range. An absent CP phase copies
     * all input points and must preserve the count. */
    return explicit_phase ? outputs <= inputs : outputs == inputs;
}

static bool hull_contract(const USILProgram *program, HullSourcePlan *plan) {
    HLSLDomainShape shape;
    if (!program || !hlsl_domain_shape(program->tessellation.domain, &shape)) return false;
    const unsigned groups = shape.inner_count ? 2 : 1;
    const bool control_point = program->tessellation.phases && program->tessellation.phase_count &&
        program->tessellation.phase_capacity >= program->tessellation.phase_count &&
        program->tessellation.phases[0].kind == DXBC_HULL_PHASE_CONTROL_POINT;
    const unsigned phase_count = groups + (control_point ? 1 : 0);
    plan->control_point_phase = control_point ? 0 : -1;
    const unsigned indexed_groups = (shape.outer_count > 1) + (shape.inner_count > 1);
    const unsigned factors = shape.outer_count + shape.inner_count;
    if (!program->has_stage_contract || !program->has_parsed_signature_authority ||
        program->program_type != DXBC_PROGRAM_TYPE_HULL || program->shader_model_major != 5 ||
        program->shader_model_minor || !memchr(program->shader_type_model, 0, sizeof(program->shader_type_model)) ||
        strcmp(program->shader_type_model, "hs_5_0") || !program->tessellation.valid ||
        !control_point_counts_supported(program, control_point) ||
        !partitioning_name(program->tessellation.partitioning) ||
        (program->tessellation.domain == DXBC_TESSELLATOR_DOMAIN_ISOLINE
            ? program->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_LINE
            : (program->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
               program->tessellation.output_primitive != DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CCW)) ||
        !program->tessellation.has_max_tessellation_factor ||
        program->tessellation.phase_count != phase_count || program->tessellation.phase_capacity < phase_count ||
        !program->tessellation.phases || program->input_count != 1 || program->output_count != 1 ||
        program->patch_constant_count != (int)factors || program->input_alloc < 1 || program->output_alloc < 1 ||
        program->patch_constant_alloc < (int)factors || !program->inputs || !program->outputs || !program->patch_constants ||
        !point_signatures(program, plan, control_point) ||
        program->signature_declaration_count != (int)(factors + indexed_groups + (control_point ? 3 : 0)) ||
        program->signature_declaration_alloc < program->signature_declaration_count ||
        !program->signature_declarations || !scalar_cbuffer_shape(program) || program->texture_count ||
        program->sampler_count || program->uav_count || program->indexable_temp_count ||
        !usil_icb_declaration_is_valid(program) ||
        (program->icb_value_count && (control_point || program->icb_value_count < 8 ||
            program->icb_value_count > HLSL_HULL_ICB_WORD_LIMIT ||
            program->icb_source_instruction_index >= program->tessellation.phases[0].marker_source_instruction_index)) ||
        program->geometry.valid || program->compute.valid ||
        (program->has_global_flags && program->global_flags != 1) ||
        program->instruction_count < 2 || program->instruction_count > HULL_SOURCE_INSTRUCTION_LIMIT ||
        program->instruction_alloc < program->instruction_count || !program->instructions ||
        program->temp_count < 0 || program->temp_count > HLSL_SM5_TEMP_REGISTER_COUNT ||
        program->index_range_count != (int)indexed_groups || program->index_range_alloc < (int)indexed_groups || !program->index_ranges ||
        !usil_signature_authority_is_valid(program) || !usil_hull_phase_temp_registers_are_valid(program)) return false;
    if (program->tessellation.max_tessellation_factor_source_instruction_index >=
        program->tessellation.phases[0].marker_source_instruction_index) return false;
    float max_factor;
    memcpy(&max_factor, &program->tessellation.max_tessellation_factor_bits, sizeof(max_factor));
    if (!isfinite(max_factor) || max_factor < 1.0f || max_factor > 64.0f || !factor_signature(program, plan)) return false;
    plan->outer_phase = plan->inner_phase = -1;
    int next = 0;
    for (int phase = 0; phase < (int)phase_count; ++phase) {
        const USILHullPhase *scope = &program->tessellation.phases[phase];
        const bool cp = phase == plan->control_point_phase;
        if ((cp ? scope->kind != DXBC_HULL_PHASE_CONTROL_POINT || scope->instance_count_declared || scope->instance_count != 1
                : scope->kind != DXBC_HULL_PHASE_FORK ||
                  (scope->instance_count != shape.outer_count && scope->instance_count != shape.inner_count) ||
                  (!scope->instance_count_declared && scope->instance_count != 1)) ||
            scope->first_instruction_index != next || scope->end_instruction_index <= next ||
            scope->end_instruction_index > program->instruction_count ||
            scope->marker_source_instruction_index >= scope->first_source_instruction_index ||
            scope->first_source_instruction_index >= scope->end_source_instruction_index ||
            (phase && scope->marker_source_instruction_index != program->tessellation.phases[phase - 1].end_source_instruction_index))
            return false;
        if (!cp) {
            int *role = scope->instance_count == shape.outer_count ? &plan->outer_phase : &plan->inner_phase;
            if (*role >= 0) return false;
            *role = phase;
        }
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
                 (cp || (instruction->opcode != USIL_OP_MIN && instruction->opcode != USIL_OP_MAX)) &&
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
        if (phase < 0 || phase >= (int)phase_count || phase == plan->control_point_phase || (ranges & (1u << phase))) return false;
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
    return mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE &&
        (hull_contract(program, &plan) || hlsl_high_level_hull_join_supported(program, mode));
}

static void hash_owner_number(CommonSha256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8; ++index) bytes[index] = (uint8_t)(value >> (index * 8));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

/* Freeze decoded owners, excluding diagnostic strings and allocation pointers.
 * This is a callback/replay guard over the already admitted stage, never a
 * substitute for retaining or comparing the original complete DXBC container. */
static bool hash_owner_operand(CommonSha256Context *hash, const DXBCOperand *operand,
                               unsigned *remaining) {
    if (!operand || !remaining || !*remaining ||
        operand->extended_token_count > DXBC_MAX_NESTED_OPERAND_TOKENS ||
        (operand->extended_token_count && !operand->extended_tokens)) return false;
    --*remaining;
    const uint64_t fields[] = {operand->type, operand->raw_token, (uint64_t)operand->register_index,
        operand->swizzle_mode, operand->destination_mask, operand->has_neg, operand->has_abs,
        operand->min_precision, (uint64_t)operand->imm_value_count,
        (uint64_t)operand->immediate_word_count, (uint64_t)operand->register_index_dim,
        (uint64_t)operand->rel_offset0, (uint64_t)operand->rel_offset1, (uint64_t)operand->rel_offset2,
        operand->extended_token_count};
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index)
        hash_owner_number(hash, fields[index]);
    for (unsigned index = 0; index < 4; ++index) {
        hash_owner_number(hash, operand->swizzle[index]);
        hash_owner_number(hash, operand->imm_values[index]);
        hash_owner_number(hash, operand->imm64_values[index]);
    }
    for (unsigned index = 0; index < 8; ++index) hash_owner_number(hash, operand->immediate_words[index]);
    for (size_t index = 0; index < operand->extended_token_count; ++index)
        hash_owner_number(hash, operand->extended_tokens[index]);
    const DXBCOperand *relative[] = {operand->rel_op0, operand->rel_op1, operand->rel_op2};
    for (unsigned index = 0; index < 3; ++index) {
        hash_owner_number(hash, operand->index_values[index]);
        hash_owner_number(hash, operand->index_representations[index]);
        hash_owner_number(hash, operand->index_has_immediate[index]);
        hash_owner_number(hash, operand->index_value_exceeds_int[index]);
        hash_owner_number(hash, relative[index] != NULL);
        if (relative[index] && !hash_owner_operand(hash, relative[index], remaining)) return false;
    }
    return true;
}

typedef enum {
    DECODED_OWNER_HULL,
    DECODED_OWNER_DOMAIN,
    DECODED_OWNER_NATURAL_STRUCTURED
} DecodedOwnerStage;

/* Storage and decoded-shape safety only. The structured producer separately
 * proves the complete IF/phi/value contract over the prepared CFG and SSA. */
static bool natural_structured_owner_storage_valid(const USILProgram *program) {
    if (!program || !program->has_stage_contract || !program->has_parsed_signature_authority ||
        (program->program_type != DXBC_PROGRAM_TYPE_VERTEX && program->program_type != DXBC_PROGRAM_TYPE_PIXEL) ||
        (program->shader_model_major != 4 && program->shader_model_major != 5) ||
        !memchr(program->shader_type_model, 0, sizeof(program->shader_type_model)) ||
        program->instruction_count < 1 || program->instruction_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        program->instruction_alloc < program->instruction_count || !program->instructions ||
        program->input_count < 0 || program->input_count > HLSL_SM5_IO_REGISTER_COUNT ||
        program->input_alloc < program->input_count || (program->input_count && !program->inputs) ||
        program->output_count < 1 || program->output_count > HLSL_SM5_IO_REGISTER_COUNT ||
        program->output_alloc < program->output_count || !program->outputs ||
        program->signature_declaration_count < 0 ||
        program->signature_declaration_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
        program->signature_declaration_alloc < program->signature_declaration_count ||
        (program->signature_declaration_count && !program->signature_declarations) ||
        program->temp_count < 0 || program->temp_count > HLSL_SM5_TEMP_REGISTER_COUNT ||
        program->cbuffer_count || program->texture_count || program->sampler_count || program->uav_count ||
        program->indexable_temp_count || program->index_range_count || program->patch_constant_count ||
        program->icb_value_count || program->has_icb_declaration || !usil_icb_declaration_is_valid(program) ||
        program->tessellation.valid || program->tessellation.phase_count || program->geometry.valid ||
        program->compute.valid || program->compute.shared_memory_count || program->compute.barrier_count)
        return false;
    const DXBCSignatureElement *signatures[] = {program->inputs, program->outputs};
    const int counts[] = {program->input_count, program->output_count};
    for (unsigned role = 0; role < 2; ++role)
        for (int index = 0; index < counts[role]; ++index)
            if (!hlsl_signature_semantic_storage_valid(&signatures[role][index])) return false;
    if (!usil_signature_authority_is_valid(program)) return false;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        USILEffectFlags effects;
        if (instruction->source_instruction_index == UINT32_MAX ||
            (index && instruction->source_instruction_index <= program->instructions[index - 1].source_instruction_index) ||
            instruction->operand_count < 0 || instruction->operand_count > DXBC_MAX_OPERANDS ||
            !memchr(instruction->resource_dimension, 0, sizeof(instruction->resource_dimension)) ||
            !usil_instruction_shape_valid(program, instruction) ||
            !usil_instruction_effects(program, instruction, &effects)) return false;
    }
    return true;
}

static bool decoded_stage_owner_digest(const USILProgram *program,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE], DecodedOwnerStage stage) {
    HullSourcePlan checked = {0};
    if (stage == DECODED_OWNER_HULL ? !hull_contract(program, &checked)
        : stage == DECODED_OWNER_DOMAIN
            ? !hlsl_high_level_domain_interface_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE)
            : stage != DECODED_OWNER_NATURAL_STRUCTURED || !natural_structured_owner_storage_valid(program)) return false;
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char domain[] = "dxbc-hull-final-factor-owner-v1";
    static const char domain_output[] = "dxbc-domain-output-owner-v1";
    static const char natural_structured[] = "dxbc-natural-structured-owner-v1";
    if (stage == DECODED_OWNER_NATURAL_STRUCTURED)
        common_sha256_update(&hash, natural_structured, sizeof(natural_structured));
    else common_sha256_update(&hash, stage == DECODED_OWNER_DOMAIN ? domain_output : domain,
        stage == DECODED_OWNER_DOMAIN ? sizeof(domain_output) : sizeof(domain));
    const USILTessellationContract *tessellation = &program->tessellation;
    const uint64_t fields[] = {program->program_type, program->shader_model_major,
        program->shader_model_minor, program->has_parsed_signature_authority,
        program->has_global_flags, program->global_flags, (uint64_t)program->temp_count,
        (uint64_t)program->cbuffer_count, tessellation->input_control_point_count,
        tessellation->output_control_point_count, tessellation->domain, tessellation->partitioning,
        tessellation->output_primitive, tessellation->max_tessellation_factor_bits,
        tessellation->max_tessellation_factor_source_instruction_index, tessellation->phase_count,
        (uint64_t)program->instruction_count, (uint64_t)program->signature_declaration_count,
        (uint64_t)program->index_range_count};
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index)
        hash_owner_number(&hash, fields[index]);
    if (stage == DECODED_OWNER_NATURAL_STRUCTURED) {
        const uint64_t authority[] = {program->has_stage_contract,
            (uint64_t)program->texture_count, (uint64_t)program->sampler_count, (uint64_t)program->uav_count,
            (uint64_t)program->indexable_temp_count, (uint64_t)program->icb_value_count,
            program->has_icb_declaration, program->icb_source_instruction_index,
            program->icb_declaration_token, program->icb_declaration_word_count,
            tessellation->valid, tessellation->has_max_tessellation_factor,
            program->geometry.valid, program->geometry.input_primitive, program->geometry.output_topology,
            program->geometry.input_vertex_count, program->geometry.max_output_vertex_count,
            program->geometry.has_instance_count, program->geometry.instance_count,
            program->geometry.declared_stream_mask, program->geometry.referenced_stream_mask,
            program->geometry.effect_count, program->geometry.output_tuple_state_persists,
            program->compute.valid, program->compute.declaration_source_instruction_index,
            program->compute.system_value_mask, program->compute.shared_memory_bytes,
            program->compute.shared_memory_count, program->compute.barrier_count};
        for (size_t index = 0; index < sizeof(authority) / sizeof(authority[0]); ++index)
            hash_owner_number(&hash, authority[index]);
        common_sha256_update(&hash, program->shader_type_model, sizeof(program->shader_type_model));
        for (unsigned index = 0; index < 3; ++index)
            hash_owner_number(&hash, program->compute.thread_group_size[index]);
    }
    /* Preserve old non-ICB observations while extending the actual parsed
     * declaration guard with every admitted raw payload word. */
    if (program->has_icb_declaration) {
        static const char icb_domain[] = "dxbc-hull-icb-owner-v1";
        common_sha256_update(&hash, icb_domain, sizeof(icb_domain));
        hash_owner_number(&hash, program->icb_source_instruction_index);
        hash_owner_number(&hash, program->icb_declaration_token);
        hash_owner_number(&hash, program->icb_declaration_word_count);
        hash_owner_number(&hash, (uint64_t)program->icb_value_count);
        for (int word = 0; word < program->icb_value_count; ++word)
            hash_owner_number(&hash, program->icb_values[word]);
    }
    for (int index = 0; index < program->cbuffer_count; ++index) {
        hash_owner_number(&hash, (uint64_t)program->cbuffers[index].reg_idx);
        hash_owner_number(&hash, (uint64_t)program->cbuffers[index].size);
        hash_owner_number(&hash, program->cbuffers[index].dynamic_indexed);
    }
    const DXBCSignatureElement *signatures[] = {program->inputs, program->outputs, program->patch_constants};
    const int counts[] = {program->input_count, program->output_count, program->patch_constant_count};
    for (unsigned role = 0; role < 3; ++role) {
        hash_owner_number(&hash, (uint64_t)counts[role]);
        for (int index = 0; index < counts[role]; ++index) {
            const DXBCSignatureElement *element = &signatures[role][index];
            const char *semantic = dxbc_signature_semantic_name(element);
            const size_t length = strlen(semantic);
            hash_owner_number(&hash, length);
            common_sha256_update(&hash, semantic, length);
            if (stage == DECODED_OWNER_NATURAL_STRUCTURED) {
                hash_owner_number(&hash, element->semantic_name_length);
                hash_owner_number(&hash, element->semantic_name_extended != NULL);
                common_sha256_update(&hash, element->semantic_name, sizeof(element->semantic_name));
            }
            const uint64_t signature[] = {element->register_id, element->semantic_index, element->system_value,
                element->component_type, element->mask, element->rw_mask, element->stream_index,
                element->min_precision, element->interpolation_mode};
            for (size_t field = 0; field < sizeof(signature) / sizeof(signature[0]); ++field)
                hash_owner_number(&hash, signature[field]);
        }
    }
    for (size_t index = 0; index < tessellation->phase_count; ++index) {
        const USILHullPhase *phase = &tessellation->phases[index];
        const uint64_t scope[] = {phase->kind, phase->marker_source_instruction_index,
            phase->first_source_instruction_index, phase->end_source_instruction_index,
            (uint64_t)phase->first_instruction_index, (uint64_t)phase->end_instruction_index,
            phase->instance_count_declared, phase->instance_count, phase->has_temp_count,
            phase->temp_count, phase->temp_count_source_instruction_index};
        for (size_t field = 0; field < sizeof(scope) / sizeof(scope[0]); ++field)
            hash_owner_number(&hash, scope[field]);
    }
    for (int index = 0; index < program->signature_declaration_count; ++index) {
        const USILSignatureDeclaration *declaration = &program->signature_declarations[index];
        const uint64_t owner[] = {declaration->kind, declaration->operand_type,
            declaration->has_signature_register, declaration->register_id, declaration->mask,
            declaration->stream_index, declaration->has_array_element_count, declaration->array_element_count,
            declaration->has_system_value, declaration->system_value_name, declaration->has_interpolation,
            declaration->interpolation_mode, declaration->source_instruction_index};
        for (size_t field = 0; field < sizeof(owner) / sizeof(owner[0]); ++field)
            hash_owner_number(&hash, owner[field]);
    }
    for (int index = 0; index < program->index_range_count; ++index) {
        const USILIndexRange *range = &program->index_ranges[index];
        hash_owner_number(&hash, range->register_count);
        hash_owner_number(&hash, range->source_instruction_index);
        hash_owner_number(&hash, (uint64_t)range->hull_phase_index);
        unsigned remaining = DXBC_MAX_NESTED_OPERAND_TOKENS;
        if (!hash_owner_operand(&hash, &range->operand, &remaining)) return false;
    }
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        hash_owner_number(&hash, instruction->opcode);
        hash_owner_number(&hash, instruction->source_instruction_index);
        hash_owner_number(&hash, (uint64_t)instruction->operand_count);
        if (stage == DECODED_OWNER_NATURAL_STRUCTURED) {
            USILEffectFlags effects;
            if (!usil_instruction_effects(program, instruction, &effects)) return false;
            const uint64_t controls[] = {instruction->saturate, instruction->precise_mask,
                instruction->condition_test, instruction->has_resource_dimension, instruction->resource_stride,
                instruction->has_texel_offset, instruction->has_resource_return_types,
                instruction->resource_info_return_type, instruction->sample_info_return_type,
                instruction->geometry_effect, instruction->geometry_stream_id,
                instruction->geometry_stream_explicit, instruction->sync_flags, effects};
            for (size_t field = 0; field < sizeof(controls) / sizeof(controls[0]); ++field)
                hash_owner_number(&hash, controls[field]);
            common_sha256_update(&hash, instruction->resource_dimension, sizeof(instruction->resource_dimension));
            for (unsigned field = 0; field < 3; ++field)
                hash_owner_number(&hash, (uint64_t)instruction->texel_offsets[field]);
            for (unsigned field = 0; field < 4; ++field)
                hash_owner_number(&hash, instruction->resource_return_types[field]);
        }
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            unsigned remaining = DXBC_MAX_NESTED_OPERAND_TOKENS;
            if (!hash_owner_operand(&hash, &instruction->operands[operand], &remaining)) return false;
        }
    }
    common_sha256_final(&hash, digest);
    return true;
}

static bool decoded_owner_digest(const USILProgram *program,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    return decoded_stage_owner_digest(program, digest, DECODED_OWNER_HULL);
}

bool hlsl_hull_owned_contract_digest(const USILProgram *program, uint8_t digest[32]) {
    return digest && decoded_owner_digest(program, digest);
}

bool hlsl_domain_owned_contract_digest(const USILProgram *program, uint8_t digest[32]) {
    return digest && decoded_stage_owner_digest(program, digest, DECODED_OWNER_DOMAIN);
}

bool hlsl_natural_structured_owned_contract_digest(const USILProgram *program, uint8_t digest[32]) {
    return digest && decoded_stage_owner_digest(program, digest, DECODED_OWNER_NATURAL_STRUCTURED);
}

static bool instance_operand(const DXBCOperand *operand, bool control_point) {
    return operand->type == (control_point ? OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID : OPERAND_TYPE_FORK_INSTANCE_ID) &&
        hlsl_lift_operand_is_plain(operand) && !operand->extended_tokens && !operand->register_index_dim &&
        !operand->swizzle[0] && (operand->swizzle_mode == 2 || (control_point && operand->swizzle_mode == 0)) &&
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

/* Factor indices retain the physical singleton lane selected by their actual
 * SSA read. The existing control-point route still requires X-only copies. */
static bool index_temp(const USILProgram *program, const DXBCOperand *operand,
                       bool destination, bool control_point) {
    if (control_point) return scalar_temp(program, operand, destination);
    if (operand->type != OPERAND_TYPE_TEMP || !hlsl_lift_operand_is_plain(operand) ||
        operand->extended_tokens || operand->register_index_dim != 1 ||
        operand->register_index < 0 || operand->register_index >= program->temp_count ||
        !operand->index_has_immediate[0] || operand->index_representations[0] ||
        operand->index_value_exceeds_int[0] ||
        operand->index_values[0] != (uint32_t)operand->register_index) return false;
    const uint8_t lanes = usil_operand_destination_lane_mask(operand);
    const int component = usil_operand_source_component(operand, 0);
    return destination ? lanes && !(lanes & (uint8_t)(lanes - 1u))
                       : operand->swizzle_mode == 2 && component >= 0 && component < 4;
}

static bool claim_index_transport(HLSLEmitterContext *ctx, HullSourcePlan *plan,
                                  int definition, int before, const DXBCOperand *read) {
    const bool control_point = plan->phase == plan->control_point_phase;
    const USILHullPhase *phase = &ctx->program->tessellation.phases[plan->phase];
    if (!index_temp(ctx->program, read, false, control_point)) return false;
    int register_id = read->register_index;
    unsigned lane = (unsigned)usil_operand_source_component(read, 0);
    for (unsigned depth = 0; depth < HULL_SOURCE_INSTRUCTION_LIMIT; ++depth) {
        if (definition < phase->first_instruction_index || definition >= before) return false;
        const USILInstruction *copy = &ctx->program->instructions[definition];
        if (copy->opcode != USIL_OP_MOV || copy->operand_count != 2 ||
            !index_temp(ctx->program, &copy->operands[0], true, control_point) ||
            copy->operands[0].register_index != register_id ||
            usil_operand_destination_lane_mask(&copy->operands[0]) != (1u << lane) ||
            !hlsl_instruction_owners_add(&plan->index_transports, definition)) return false;
        if (instance_operand(&copy->operands[1], control_point)) return true;
        if (!index_temp(ctx->program, &copy->operands[1], false, control_point)) return false;
        before = definition;
        definition = hlsl_operand_definition(ctx, definition, 1, (int)lane);
        register_id = copy->operands[1].register_index;
        lane = (unsigned)usil_operand_source_component(&copy->operands[1], 0);
    }
    return false;
}

static bool destination_supported(HLSLEmitterContext *ctx, int index, void *context) {
    HullSourcePlan *plan = context;
    const DXBCOperand *destination = &ctx->program->instructions[index].operands[0];
    if (plan->phase == plan->control_point_phase)
        return destination->type == OPERAND_TYPE_OUTPUT && hlsl_lift_operand_is_plain(destination) &&
            !destination->extended_tokens && destination->register_index_dim == 1 &&
            !destination->register_index && destination->index_has_immediate[0] &&
            !destination->index_representations[0] && !destination->index_values[0] &&
            !destination->index_value_exceeds_int[0] && usil_operand_destination_lane_mask(destination) == 15;
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
        index_temp(ctx->program, destination->rel_op0, false, false) &&
        claim_index_transport(ctx, plan, hlsl_relative_operand_definition(ctx, index, 0, 0), index, destination->rel_op0);

}

static bool control_point_source_supported(HLSLEmitterContext *ctx, int index, int operand, void *context) {
    HullSourcePlan *plan = context;
    const USILInstruction *instruction = &ctx->program->instructions[index];
    if (operand < 1 || operand >= instruction->operand_count) return false;
    const DXBCOperand *source = &instruction->operands[operand];
    if (plan->phase != plan->control_point_phase || !control_point_counts_supported(ctx->program, true) ||
        source->type != OPERAND_TYPE_INPUT ||
        source->register_index_dim != 2 ||
        source->register_index || source->rel_offset0 || source->swizzle_mode != 1 || source->min_precision ||
        source->has_abs || source->has_neg || source->extended_tokens || source->extended_token_count ||
        source->index_representations[0] != 2 || source->index_has_immediate[0] || source->index_values[0] ||
        source->index_value_exceeds_int[0] || !source->rel_op0 || source->rel_op1 || source->rel_op2 ||
        source->index_representations[1] || !source->index_has_immediate[1] || source->index_values[1] ||
        source->index_value_exceeds_int[1] || usil_operand_destination_lane_mask(&instruction->operands[0]) != 15 ||
        !scalar_temp(ctx->program, source->rel_op0, false)) return false;
    for (unsigned lane = 0; lane < 4; ++lane) if (source->swizzle[lane] != lane) return false;
    return claim_index_transport(ctx, plan, hlsl_relative_operand_definition(ctx, index, operand, 0), index, source->rel_op0);
}

static bool factor_material_source_supported(HLSLEmitterContext *ctx, int index,
                                              int operand, void *context) {
    HullSourcePlan *plan = context;
    if (!ctx || !ctx->program || !plan || plan->phase < 0 ||
        (size_t)plan->phase >= ctx->program->tessellation.phase_count ||
        plan->phase == plan->control_point_phase || index < 0 ||
        index >= ctx->program->instruction_count || operand < 1 ||
        operand >= ctx->program->instructions[index].operand_count ||
        !ctx->cbuffer_layouts_built || ctx->cbuffer_layout_count != 1) return false;
    const USILHullPhase *phase = &ctx->program->tessellation.phases[plan->phase];
    const USILInstruction *instruction = &ctx->program->instructions[index];
    const DXBCOperand *source = &instruction->operands[operand];
    USILOperandUseInfo use;
    if (index < phase->first_instruction_index || index >= phase->end_instruction_index ||
        source->type != OPERAND_TYPE_CONSTANT_BUFFER || !hlsl_lift_operand_is_plain(source) ||
        source->extended_token_count || source->extended_tokens || source->register_index_dim != 2 ||
        source->register_index || source->rel_offset0 ||
        !source->index_has_immediate[0] || !source->index_has_immediate[1] ||
        source->index_representations[0] || source->index_representations[1] ||
        source->index_values[0] || source->index_values[1] ||
        source->index_value_exceeds_int[0] || source->index_value_exceeds_int[1] ||
        !usil_instruction_operand_use(ctx->program, instruction, operand, &use) ||
        use.use != USIL_OPERAND_USE_SOURCE || !use.source_lane_mask ||
        (use.source_lane_mask & (uint8_t)(use.source_lane_mask - 1u)) ||
        !hlsl_material_source_supported(ctx, source, use.source_lane_mask)) return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if ((use.source_lane_mask & (1u << lane)) && usil_operand_source_component(source, (int)lane) != 0)
            return false;
    return true;
}

static bool icb_operand_snapshot(const DXBCOperand *operand, bool relative,
                                 HLSLHullICBOperandSnapshot *snapshot) {
    if (!operand || !snapshot || operand->has_abs || operand->has_neg || operand->min_precision ||
        operand->extended_token_count || operand->extended_tokens || operand->imm_value_count ||
        operand->immediate_word_count || operand->register_index_dim < 0 || operand->register_index_dim > 1 ||
        operand->rel_op1 || operand->rel_op2 || (relative ? !operand->rel_op0 : operand->rel_op0 != NULL)) return false;
    for (unsigned index = 0; index < 4; ++index)
        if (operand->imm_values[index] || operand->imm64_values[index]) return false;
    for (unsigned index = 0; index < 8; ++index)
        if (operand->immediate_words[index]) return false;
    for (int dimension = operand->register_index_dim; dimension < 3; ++dimension)
        if (operand->index_values[dimension] || operand->index_representations[dimension] ||
            operand->index_has_immediate[dimension] || operand->index_value_exceeds_int[dimension]) return false;
    *snapshot = (HLSLHullICBOperandSnapshot){.type = operand->type, .raw_token = operand->raw_token,
        .register_index = operand->register_index, .register_index_dim = operand->register_index_dim,
        .rel_offsets = {operand->rel_offset0, operand->rel_offset1, operand->rel_offset2},
        .swizzle_mode = operand->swizzle_mode, .destination_mask = operand->destination_mask};
    memcpy(snapshot->swizzle, operand->swizzle, sizeof(snapshot->swizzle));
    memcpy(snapshot->index_values, operand->index_values, sizeof(snapshot->index_values));
    memcpy(snapshot->index_representations, operand->index_representations, sizeof(snapshot->index_representations));
    memcpy(snapshot->index_has_immediate, operand->index_has_immediate, sizeof(snapshot->index_has_immediate));
    memcpy(snapshot->index_value_exceeds_int, operand->index_value_exceeds_int, sizeof(snapshot->index_value_exceeds_int));
    return true;
}

/* Build predecessor edges root first. Shared MOV definitions retain one exact
 * register/lane edge rather than acquiring authority from a membership bit. */
static bool icb_index_transport(HLSLEmitterContext *ctx, HullSourcePlan *plan,
    int definition, int before, const DXBCOperand *read, unsigned depth, int *tail) {
    const USILHullPhase *phase = &ctx->program->tessellation.phases[plan->phase];
    if (depth >= HLSL_HULL_ICB_TRANSPORT_LIMIT || !index_temp(ctx->program, read, false, false) ||
        definition < phase->first_instruction_index || definition >= before) return false;
    const unsigned lane = (unsigned)usil_operand_source_component(read, 0);
    const USILInstruction *copy = &ctx->program->instructions[definition];
    if (copy->opcode != USIL_OP_MOV || copy->operand_count != 2 ||
        !index_temp(ctx->program, &copy->operands[0], true, false) ||
        copy->operands[0].register_index != read->register_index ||
        usil_operand_destination_lane_mask(&copy->operands[0]) != (1u << lane)) return false;
    HLSLHullICBTransport edge = {.instruction_index = definition,
        .source_instruction_index = copy->source_instruction_index,
        .destination_lane = (uint8_t)lane, .predecessor_transport = -1};
    if (!icb_operand_snapshot(&copy->operands[0], false, &edge.destination) ||
        !icb_operand_snapshot(&copy->operands[1], false, &edge.source)) return false;
    if (!instance_operand(&copy->operands[1], false)) {
        if (!index_temp(ctx->program, &copy->operands[1], false, false) ||
            !icb_index_transport(ctx, plan, hlsl_operand_definition(ctx, definition, 1, (int)lane),
                definition, &copy->operands[1], depth + 1, &edge.predecessor_transport)) return false;
        edge.source_lane = (uint8_t)usil_operand_source_component(&copy->operands[1], 0);
    }
    for (size_t index = 0; index < plan->icb.transport_count; ++index) {
        const HLSLHullICBTransport *retained = &plan->icb.transports[index];
        if (retained->instruction_index != definition) continue;
        if (retained->source_instruction_index != edge.source_instruction_index ||
            retained->destination_lane != edge.destination_lane || retained->source_lane != edge.source_lane ||
            retained->predecessor_transport != edge.predecessor_transport ||
            !hlsl_hull_icb_operands_equal(&retained->destination, &edge.destination) ||
            !hlsl_hull_icb_operands_equal(&retained->source, &edge.source)) return false;
        *tail = (int)index;
        return true;
    }
    if (plan->owners_frozen || plan->icb.transport_count == HLSL_HULL_ICB_TRANSPORT_LIMIT) return false;
    *tail = (int)plan->icb.transport_count;
    plan->icb.transports[plan->icb.transport_count++] = edge;
    return true;
}

static bool begin_icb_plan(HLSLEmitterContext *ctx, HullSourcePlan *plan, unsigned column) {
    const USILProgram *program = ctx->program;
    const USILHullPhase *phase = &program->tessellation.phases[plan->phase];
    if (plan->icb.present) return plan->icb.phase_index == plan->phase &&
        plan->icb.physical_column == column && plan->icb.row_count == phase->instance_count;
    if (plan->owners_frozen) return false;
    size_t fork = SIZE_MAX;
    for (int index = 0; index < program->signature_declaration_count; ++index) {
        const USILSignatureDeclaration *declaration = &program->signature_declarations[index];
        if (declaration->operand_type != OPERAND_TYPE_FORK_INSTANCE_ID ||
            declaration->source_instruction_index < phase->first_source_instruction_index ||
            declaration->source_instruction_index >= phase->end_source_instruction_index) continue;
        if (fork != SIZE_MAX) return false;
        fork = (size_t)index;
    }
    if (fork == SIZE_MAX) return false;
    HLSLHullICBPlan *icb = &plan->icb;
    icb->present = true;
    icb->declaration_source_instruction_index = program->icb_source_instruction_index;
    icb->declaration_token = program->icb_declaration_token;
    icb->declaration_word_count = program->icb_declaration_word_count;
    icb->payload_count = (size_t)program->icb_value_count;
    memcpy(icb->payload, program->icb_values, icb->payload_count * sizeof(*icb->payload));
    icb->row_count = phase->instance_count;
    icb->physical_column = (uint8_t)column;
    icb->phase_index = plan->phase;
    icb->phase = *phase;
    icb->fork_declaration_index = fork;
    icb->fork_declaration = program->signature_declarations[fork];
    return hlsl_copy_checked(ctx, icb->array_name, sizeof(icb->array_name), plan->names[FACTOR_OFFSETS]) &&
        hlsl_copy_checked(ctx, icb->index_name, sizeof(icb->index_name), plan->names[FACTOR_INDEX]);
}

static bool factor_constant_source_supported(HLSLEmitterContext *ctx, int index,
                                             int operand, void *context) {
    HullSourcePlan *plan = context;
    if (!ctx || !ctx->program || !plan || plan->phase < 0 ||
        (size_t)plan->phase >= ctx->program->tessellation.phase_count ||
        plan->phase == plan->control_point_phase || index < 0 || index >= ctx->program->instruction_count ||
        operand < 1 || operand >= ctx->program->instructions[index].operand_count ||
        !ctx->program->icb_value_count || !usil_icb_declaration_is_valid(ctx->program)) return false;
    const USILHullPhase *phase = &ctx->program->tessellation.phases[plan->phase];
    const USILInstruction *instruction = &ctx->program->instructions[index];
    const DXBCOperand *source = &instruction->operands[operand];
    USILOperandUseInfo use;
    HLSLHullICBConsumer consumer = {.instruction_index = index, .operand_index = operand,
        .source_instruction_index = instruction->source_instruction_index, .transport_tail = -1};
    if (index < phase->first_instruction_index || index >= phase->end_instruction_index ||
        phase->kind != DXBC_HULL_PHASE_FORK || phase->instance_count < 2 || phase->instance_count > 4 ||
        ctx->program->icb_value_count != (int)phase->instance_count * 4 ||
        source->type != OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER || source->register_index_dim != 1 ||
        source->register_index || source->rel_offset0 || source->rel_offset1 || source->rel_offset2 ||
        source->swizzle_mode != 2 || source->index_values[0] || source->index_value_exceeds_int[0] ||
        !((source->index_representations[0] == 2 && !source->index_has_immediate[0]) ||
          (source->index_representations[0] == 3 && source->index_has_immediate[0])) ||
        !icb_operand_snapshot(source, true, &consumer.operand) ||
        !icb_operand_snapshot(source->rel_op0, false, &consumer.relative) ||
        !usil_instruction_operand_use(ctx->program, instruction, operand, &use) ||
        use.use != USIL_OPERAND_USE_SOURCE || !use.source_lane_mask ||
        (use.source_lane_mask & (uint8_t)(use.source_lane_mask - 1u))) return false;
    unsigned lane = 0;
    while (!(use.source_lane_mask & (1u << lane))) ++lane;
    const int column = usil_operand_source_component(source, (int)lane);
    if (column < 0 || column >= 4) return false;
    for (unsigned row = 0; row < phase->instance_count; ++row) {
        const uint32_t bits = ctx->program->icb_values[row * 4 + (unsigned)column];
        float value;
        memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value)) return false;
        for (unsigned physical = 0; physical < 4; ++physical)
            if (physical != (unsigned)column && ctx->program->icb_values[row * 4 + physical]) return false;
    }
    if (!begin_icb_plan(ctx, plan, (unsigned)column)) return false;
    consumer.demanded_lanes = use.source_lane_mask;
    consumer.physical_column = (uint8_t)column;
    consumer.direct_fork_id = instance_operand(source->rel_op0, false);
    if (!consumer.direct_fork_id &&
        (!claim_index_transport(ctx, plan, hlsl_relative_operand_definition(ctx, index, operand, 0), index, source->rel_op0) ||
         !icb_index_transport(ctx, plan, hlsl_relative_operand_definition(ctx, index, operand, 0),
             index, source->rel_op0, 0, &consumer.transport_tail))) return false;
    for (size_t retained = 0; retained < plan->icb.consumer_count; ++retained) {
        const HLSLHullICBConsumer *current = &plan->icb.consumers[retained];
        if (current->instruction_index == index && current->operand_index == operand)
            return hlsl_hull_icb_consumers_equal(current, &consumer);
    }
    if (plan->owners_frozen || plan->icb.consumer_count == HLSL_HULL_ICB_CONSUMER_LIMIT) return false;
    plan->icb.consumers[plan->icb.consumer_count++] = consumer;
    return true;
}

static bool factor_source_supported(HLSLEmitterContext *ctx, int index, int operand, void *context) {
    return factor_material_source_supported(ctx, index, operand, context) ||
        factor_constant_source_supported(ctx, index, operand, context);
}

static ASTExpr *factor_source_expression(HLSLEmitterContext *ctx, int index,
                                        int operand, uint8_t lanes, void *context) {
    if (factor_material_source_supported(ctx, index, operand, context))
        return hlsl_material_source_expression(ctx, index, operand, lanes);
    if (!factor_constant_source_supported(ctx, index, operand, context)) return NULL;
    const HullSourcePlan *plan = context;
    for (size_t consumer = 0; consumer < plan->icb.consumer_count; ++consumer) {
        const HLSLHullICBConsumer *owner = &plan->icb.consumers[consumer];
        if (owner->instruction_index != index || owner->operand_index != operand) continue;
        if (owner->demanded_lanes != lanes) return NULL;
        char text[2 * HLSL_HULL_ICB_NAME_LIMIT + 4];
        if (!hlsl_format_checked(ctx, text, sizeof(text), "%s[%s]", plan->icb.array_name, plan->icb.index_name)) return NULL;
        ASTOperandProvenance provenance;
        ast_operand_provenance_init(&provenance);
        provenance.complete = true;
        provenance.value_role = AST_OPERAND_VALUE_LOGICAL;
        provenance.logical_value_id = HLSL_HULL_ICB_ACCESS_LOGICAL_ID_BASE | (uint64_t)consumer;
        provenance.natural_components = provenance.result_components = 1;
        provenance.instruction_index = index;
        provenance.source_instruction_index = owner->source_instruction_index;
        provenance.operand_index = operand;
        provenance.destination_lanes = lanes;
        ASTExpr *access = ast_create_emitter_operand_with_provenance(text, &provenance);
        if (access && !hlsl_stage_coverage_hull_icb_access(ctx, access, consumer)) {
            ast_free_expr(access);
            access = NULL;
        }
        return access;
    }
    return NULL;
}

static bool material_parameter_bounds(const SerializedProgramParameters *parameters) {
    if (!parameters) return true;
    if (parameters->cb_count < 0 || parameters->cb_count > 2 ||
        parameters->res_count < 0 || parameters->res_count > 1 ||
        (parameters->cb_count && !parameters->constant_buffers) ||
        (parameters->res_count && !parameters->resources)) return false;
    for (int index = 0; index < parameters->cb_count; ++index) {
        const SerializedConstantBuffer *buffer = &parameters->constant_buffers[index];
        if (buffer->var_count < 0 || buffer->var_count > 1 || buffer->struct_count ||
            (buffer->var_count && !buffer->variables)) return false;
    }
    return true;
}

static bool retain_material_parameters(HLSLEmitterContext *ctx, HullSourcePlan *plan) {
    const SerializedProgramParameters *parameters[] = {ctx->params, ctx->common_params};
    for (unsigned source = 0; source < 2; ++source) {
        if (!material_parameter_bounds(parameters[source])) return false;
        plan->has_material_parameters[source] = parameters[source] != NULL;
        if (parameters[source] &&
            !serialized_program_parameters_copy(&plan->material_parameters[source], parameters[source])) return false;
    }
    return true;
}

static bool material_parameters_match(HLSLEmitterContext *ctx, const HullSourcePlan *plan) {
    const SerializedProgramParameters *parameters[] = {ctx->params, ctx->common_params};
    for (unsigned source = 0; source < 2; ++source) {
        if (plan->has_material_parameters[source] != (parameters[source] != NULL) ||
            !material_parameter_bounds(parameters[source]) ||
            (parameters[source] && !serialized_program_parameters_equal(
                &plan->material_parameters[source], parameters[source]))) return false;
    }
    return true;
}

static bool prepare_scalar_cbuffer(HLSLEmitterContext *ctx) {
    if (!ctx->program->cbuffer_count)
        return hlsl_global_declarations_validate_empty_target(ctx->global_declarations, ctx->program,
            ctx->params, ctx->common_params) == HLSL_GLOBAL_DECLARATIONS_OK;
    /* The shared layout builder resolves fields and authority. Bound its public
     * metadata collections before entering that parser on this early route. */
    const SerializedProgramParameters *sources[] = {ctx->params, ctx->common_params};
    for (unsigned source = 0; source < 2; ++source) {
        const SerializedProgramParameters *parameters = sources[source];
        if (!parameters) continue;
        if (parameters->cb_count < 0 || parameters->cb_count > 2 ||
            parameters->res_count < 0 || parameters->res_count > 1 ||
            (parameters->cb_count && !parameters->constant_buffers) ||
            (parameters->res_count && !parameters->resources)) return false;
        for (int buffer = 0; buffer < parameters->cb_count; ++buffer) {
            const SerializedConstantBuffer *metadata = &parameters->constant_buffers[buffer];
            if (metadata->var_count < 0 || metadata->var_count > 1 ||
                (metadata->var_count && !metadata->variables) || metadata->struct_count)
                return false;
        }
        for (int resource = 0; resource < parameters->res_count; ++resource) {
            const SerializedResourceParam *binding = &parameters->resources[resource];
            if (binding->bind_type != SERIALIZED_RESOURCE_CONSTANT_BUFFER ||
                binding->bind_index || binding->array_size != 1) return false;
        }
    }
    if (!scalar_cbuffer_shape(ctx->program) || !build_cbuffer_register_map(ctx) ||
        !build_cbuffer_emission_layouts(ctx) || ctx->cbuffer_layout_count != 1 ||
        !hlsl_source_quality_cbuffer_inventory_supported(ctx)) return false;
    HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[0];
    if (layout->reg || layout->row_count != 1 || layout->reflection_size_bytes != 16 ||
        !layout->has_serialized_authority || !layout->has_reflection_size_authority ||
        layout->variable_count != 1 || !layout->variables || layout->raw_storage ||
        layout->row_struct_storage || layout->is_unity_builtin || layout->omit_declaration ||
        !layout->uses || layout->use_alloc < 1 || !layout->uses[0].referenced ||
        layout->projection_status != DXBC_CBUFFER_PROJECTION_EXACT ||
        layout->projection.saw_dynamic_access || layout->projection.saw_padding_access) return false;
    const TempVariable *field = &layout->variables[0];
    const bool prepared = field->name && hlsl_source_identifier_valid(field->name) &&
        (!ctx->entry_point_name || (strcmp(field->name, ctx->entry_point_name) &&
            (!layout->declaration_name || strcmp(layout->declaration_name, ctx->entry_point_name)))) &&
        (field->authority == 1 || field->authority == 2) && !field->type &&
        !field->is_matrix && !field->matrix_array_size && !field->row_major && field->rows == 1 &&
        field->dim == 1 && !field->byte_offset && !field->reg_offset && field->byte_size == 4;
    if (!prepared) return false;
    /* Only the complete named scalar layout acquires this source policy.
     * Compact Globals declarations retain their existing explicit binding. */
    layout->natural_hull_scalar_packing =
        hlsl_source_quality_named_cbuffer_supported(ctx, 0, NULL);
    return true;
}

static ASTExpr *control_point_source_expression(HLSLEmitterContext *ctx, int index, int operand,
                                                uint8_t mask, void *context) {
    HullSourcePlan *plan = context;
    if (mask != 15 || !control_point_source_supported(ctx, index, operand, context)) return NULL;
    char text[320];
    if (!hlsl_format_checked(ctx, text, sizeof(text), "%s[%s].%s", plan->names[PATCH_VARIABLE],
                             plan->names[POINT_INDEX], plan->names[POINT_FIELD])) return NULL;
    ASTOperandProvenance origin;
    ast_operand_provenance_init(&origin);
    origin.complete = true;
    origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    origin.logical_value_id = UINT64_C(0x8000000000000100);
    origin.natural_components = origin.result_components = 4;
    for (unsigned lane = 0; lane < 4; ++lane) origin.selected_components[lane] = (uint8_t)lane;
    origin.instruction_index = index;
    origin.source_instruction_index = ctx->program->instructions[index].source_instruction_index;
    origin.operand_index = operand;
    origin.destination_lanes = mask;
    return ast_create_emitter_operand_with_provenance(text, &origin);
}

static bool append_destination(HLSLEmitterContext *ctx, int index, void *context) {
    HullSourcePlan *plan = context;
    if (!destination_supported(ctx, index, context)) return false;
    if (plan->phase == plan->control_point_phase) {
        sb_appendf(ctx->sb, "%s.%s", plan->names[POINT_VARIABLE], plan->names[POINT_FIELD]);
        return sb_ok(ctx->sb);
    }
    const bool inner = plan->phase == plan->inner_phase;
    const unsigned count = inner ? plan->shape.inner_count : plan->shape.outer_count;
    sb_appendf(ctx->sb, "%s.%s", plan->names[FACTOR_VARIABLE], plan->names[inner ? INNER_FIELD : OUTER_FIELD]);
    if (count > 1) sb_appendf(ctx->sb, "[%s]", plan->names[FACTOR_INDEX]);
    return sb_ok(ctx->sb);
}

static bool factor_clamp_origin(HLSLEmitterContext *ctx, HullSourcePlan *plan, int index,
                                HLSLHullFactorClampOrigin *origin) {
    if (!ctx || !plan || !origin || plan->phase == plan->control_point_phase) return false;
    const USILProgram *program = ctx->program;
    const USILHullPhase *phase = &program->tessellation.phases[plan->phase];
    if (index < phase->first_instruction_index || index >= phase->end_instruction_index) return false;
    const USILInstruction *instruction = &program->instructions[index];
    if (instruction->opcode != USIL_OP_MIN || instruction->operand_count != 3 ||
        instruction->precise_mask || instruction->saturate) return false;
    const DXBCOperand *destination = &instruction->operands[0];
    const DXBCOperand *value = &instruction->operands[1], *maximum = &instruction->operands[2];
    const uint8_t lanes = usil_operand_destination_lane_mask(destination);
    if (!lanes || (lanes & (uint8_t)(lanes - 1u)) ||
        !hlsl_lift_operand_is_plain(value) || value->extended_tokens ||
        !hlsl_lift_operand_is_plain(maximum) || maximum->extended_tokens ||
        maximum->type != OPERAND_TYPE_IMMEDIATE32 || maximum->register_index_dim ||
        maximum->imm_value_count != 1 || maximum->immediate_word_count != 1 ||
        maximum->imm_values[0] != maximum->immediate_words[0] ||
        maximum->imm_values[0] != program->tessellation.max_tessellation_factor_bits) return false;
    unsigned lane = 0;
    while (!(lanes & (1u << lane))) ++lane;
    int component = 0, definition = HLSL_DEFINITION_UNKNOWN;
    if (value->type == OPERAND_TYPE_TEMP) {
        if (!index_temp(program, value, false, false)) return false;
        component = usil_operand_source_component(value, (int)lane);
        definition = hlsl_operand_definition(ctx, index, 1, (int)lane);
        if (definition < phase->first_instruction_index || definition >= index) return false;
    } else if (value->type == OPERAND_TYPE_CONSTANT_BUFFER) {
        component = usil_operand_source_component(value, (int)lane);
        if (value->swizzle_mode != 2 || component != 0 || value->register_index_dim != 2 ||
            value->register_index || value->rel_offset0 ||
            !value->index_has_immediate[0] || !value->index_has_immediate[1] ||
            value->index_representations[0] || value->index_representations[1] ||
            value->index_values[0] || value->index_values[1] ||
            value->index_value_exceeds_int[0] || value->index_value_exceeds_int[1]) return false;
    } else if (value->type != OPERAND_TYPE_IMMEDIATE32 || value->register_index_dim ||
               value->imm_value_count != 1 || value->immediate_word_count != 1 ||
               value->imm_values[0] != value->immediate_words[0]) return false;
    int output = index;
    if (destination->type == OPERAND_TYPE_OUTPUT) {
        if (!destination_supported(ctx, index, plan)) return false;
    } else {
        if (!index_temp(program, destination, true, false) ||
            hlsl_definition_use_count(ctx, index, (int)lane) != 1) return false;
        output = -1;
        for (int consumer = index + 1; consumer < phase->end_instruction_index; ++consumer) {
            const USILInstruction *copy = &program->instructions[consumer];
            if (copy->opcode == USIL_OP_MOV && copy->operand_count == 2 &&
                copy->operands[0].type == OPERAND_TYPE_OUTPUT &&
                index_temp(program, &copy->operands[1], false, false) &&
                usil_operand_source_component(&copy->operands[1], 0) == (int)lane &&
                hlsl_operand_definition(ctx, consumer, 1, 0) == index &&
                destination_supported(ctx, consumer, plan)) output = consumer;
        }
        if (output < 0) return false;
    }
    USILEffectFlags effects;
    if (!usil_instruction_effects(program, instruction, &effects) || effects != USIL_EFFECT_NONE) return false;
    *origin = (HLSLHullFactorClampOrigin){
        .maximum_bits = maximum->imm_values[0],
        .maximum_source_instruction_index = program->tessellation.max_tessellation_factor_source_instruction_index,
        .phase_index = plan->phase,
        .phase_marker_source_instruction_index = phase->marker_source_instruction_index,
        .output_instruction_index = output,
        .output_source_instruction_index = program->instructions[output].source_instruction_index,
        .value_operand_index = 1, .maximum_operand_index = 2,
        .value_source_component = (uint8_t)component,
        .value_definition_instruction_index = definition};
    memcpy(origin->decoded_owner_digest, plan->decoded_owner_digest, sizeof(origin->decoded_owner_digest));
    return true;
}

static bool clamp_owners_equal(const HLSLHullFactorClampOrigin *a, const HLSLHullFactorClampOrigin *b) {
    if (!a || !b) return false;
    return a->maximum_bits == b->maximum_bits &&
        a->maximum_source_instruction_index == b->maximum_source_instruction_index &&
        a->phase_index == b->phase_index &&
        a->phase_marker_source_instruction_index == b->phase_marker_source_instruction_index &&
        a->output_instruction_index == b->output_instruction_index &&
        a->output_source_instruction_index == b->output_source_instruction_index &&
        a->value_operand_index == b->value_operand_index && a->maximum_operand_index == b->maximum_operand_index &&
        a->value_source_component == b->value_source_component &&
        a->value_definition_instruction_index == b->value_definition_instruction_index &&
        !memcmp(a->decoded_owner_digest, b->decoded_owner_digest, sizeof(a->decoded_owner_digest));
}

bool hlsl_hull_factor_clamp_origins_equal(const HLSLHullFactorClampOrigin *a, const HLSLHullFactorClampOrigin *b) {
    return clamp_owners_equal(a, b) &&
        a->assignment_source_begin == b->assignment_source_begin && a->assignment_source_end == b->assignment_source_end &&
        a->maximum_attribute_source_begin == b->maximum_attribute_source_begin &&
        a->maximum_attribute_source_end == b->maximum_attribute_source_end;
}

static bool factor_clamp_supported(HLSLEmitterContext *ctx, int index, void *context) {
    HullSourcePlan *plan = context;
    HLSLHullFactorClampOrigin current;
    return hlsl_instruction_owners_contains(&plan->factor_clamps, index) &&
        factor_clamp_origin(ctx, plan, index, &current) &&
        clamp_owners_equal(&current, &plan->clamp_origins[index]);
}

static void record_factor_assignment(HLSLEmitterContext *ctx, int instruction, size_t begin,
                                     size_t end, void *context) {
    HullSourcePlan *plan = context;
    if (!ctx || instruction < 0 || instruction >= HULL_SOURCE_INSTRUCTION_LIMIT ||
        ctx->program->instructions[instruction].operands[0].type != OPERAND_TYPE_OUTPUT) return;
    while (begin < end && ctx->sb->buf[begin] == ' ') ++begin;
    plan->assignment_begin[instruction] = begin;
    plan->assignment_end[instruction] = end;
}

static bool retained_clamp_source_origin(HLSLEmitterContext *ctx, const HullSourcePlan *plan,
                                        int instruction, HLSLExpressionOrigin *origin) {
    const int output = plan->clamp_origins[instruction].output_instruction_index;
    if (output < 0 || output >= HULL_SOURCE_INSTRUCTION_LIMIT || !plan->assignment_begin[output] ||
        plan->assignment_begin[output] >= plan->assignment_end[output]) return false;
    *origin = (HLSLExpressionOrigin){.kind = HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP,
        .instruction_index = instruction,
        .source_instruction_index = ctx->program->instructions[instruction].source_instruction_index,
        .destination_lanes = usil_operand_destination_lane_mask(&ctx->program->instructions[instruction].operands[0]),
        .source_begin = plan->assignment_begin[output], .source_end = plan->assignment_end[output],
        .definition_begin = plan->maximum_attribute_begin, .definition_end = plan->maximum_attribute_end,
        .hull_factor_clamp = plan->clamp_origins[instruction]};
    origin->hull_factor_clamp.assignment_source_begin = origin->source_begin;
    origin->hull_factor_clamp.assignment_source_end = origin->source_end;
    origin->hull_factor_clamp.maximum_attribute_source_begin = origin->definition_begin;
    origin->hull_factor_clamp.maximum_attribute_source_end = origin->definition_end;
    return true;
}

static bool prepare_phase_graph(HLSLEmitterContext *ctx, HullSourcePlan *plan, int phase) {
    free_hlsl_use_def_graph(ctx);
    free_hlsl_ssa_graph(ctx);
    free_control_flow_graph(ctx);
    const USILHullPhase *scope = &ctx->program->tessellation.phases[phase];
    plan->phase = phase;
    if (!build_control_flow_graph_range(ctx, scope->first_instruction_index, scope->end_instruction_index) ||
        !compute_dominance(&ctx->cfg) || !build_hlsl_ssa_graph(ctx) || !build_hlsl_use_def_graph(ctx)) return false;
    unsigned output_count = 0;
    for (int index = scope->first_instruction_index; index < scope->end_instruction_index; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        if (instruction->operand_count && instruction->operands[0].type == OPERAND_TYPE_OUTPUT) {
            if (++output_count != 1 || !destination_supported(ctx, index, plan)) return false;
        }
    }
    if (output_count != 1) return false;
    return true;
}

/* Every table consumer must reach a printed factor expression. The
 * ordinary pure planner may otherwise discard a dead pending subtree before
 * printing it. Follow demanded SSA lanes backward within this one straight
 * phase, so capture and ordinary emission share the same admission boundary.
 * This follows whole producer expressions conservatively; the existing pure
 * planner retains their children when it projects a selected vector lane. */
static bool icb_consumers_live(HLSLEmitterContext *ctx, const HullSourcePlan *plan) {
    if (!plan->icb.present || plan->phase != plan->icb.phase_index) return true;
    const USILProgram *program = ctx->program;
    const USILHullPhase *phase = &program->tessellation.phases[plan->phase];
    if (!plan->icb.consumer_count || plan->icb.consumer_count > HLSL_HULL_ICB_CONSUMER_LIMIT ||
        phase->kind != DXBC_HULL_PHASE_FORK || phase->first_instruction_index < 0 ||
        phase->end_instruction_index > HULL_SOURCE_INSTRUCTION_LIMIT) return false;
    bool live[HULL_SOURCE_INSTRUCTION_LIMIT] = {false};
    int output = -1;
    for (int index = phase->first_instruction_index; index < phase->end_instruction_index; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        if (!instruction->operand_count || instruction->operands[0].type != OPERAND_TYPE_OUTPUT) continue;
        if (output >= 0 || usil_operand_destination_lane_mask(&instruction->operands[0]) != 1) return false;
        output = index;
    }
    if (output < 0) return false;
    live[output] = true;
    for (int index = output; index >= phase->first_instruction_index; --index) {
        if (!live[index]) continue;
        const USILInstruction *instruction = &program->instructions[index];
        for (int operand = 1; operand < instruction->operand_count; ++operand) {
            const DXBCOperand *source = &instruction->operands[operand];
            if (source->type != OPERAND_TYPE_TEMP) continue;
            USILOperandUseInfo use;
            if (source->rel_op0 || source->rel_op1 || source->rel_op2 ||
                !usil_instruction_operand_use(program, instruction, operand, &use) ||
                use.use != USIL_OPERAND_USE_SOURCE || !use.source_lane_mask || (use.source_lane_mask & ~15u))
                return false;
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane))) continue;
                const int definition = hlsl_operand_definition(ctx, index, operand, (int)lane);
                const int selected = usil_operand_source_component(source, (int)lane);
                if (definition < phase->first_instruction_index || definition >= index || selected < 0 || selected >= 4 ||
                    hlsl_instruction_owners_contains(&plan->index_transports, definition)) return false;
                const USILInstruction *producer = &program->instructions[definition];
                if (!producer->operand_count || producer->operands[0].type != OPERAND_TYPE_TEMP ||
                    producer->operands[0].register_index != source->register_index ||
                    !(usil_operand_destination_lane_mask(&producer->operands[0]) & (1u << (unsigned)selected))) return false;
                live[definition] = true;
            }
        }
    }
    for (size_t index = 0; index < plan->icb.consumer_count; ++index) {
        const int instruction = plan->icb.consumers[index].instruction_index;
        if (instruction < phase->first_instruction_index || instruction >= phase->end_instruction_index || !live[instruction])
            return false;
    }
    return true;
}

static bool prepare_phase(HLSLEmitterContext *ctx, HullSourcePlan *plan, int phase) {
    if (plan->owners_frozen) {
        uint8_t current[COMMON_SHA256_DIGEST_SIZE];
        if (!material_parameters_match(ctx, plan) || !decoded_owner_digest(ctx->program, current) ||
            memcmp(current, plan->decoded_owner_digest, sizeof(current))) return false;
    }
    if (!prepare_phase_graph(ctx, plan, phase)) return false;
    const USILHullPhase *scope = &ctx->program->tessellation.phases[phase];
    if (phase == plan->control_point_phase) {
        unsigned point_reads = 0;
        for (int index = scope->first_instruction_index; index < scope->end_instruction_index; ++index) {
            const USILInstruction *instruction = &ctx->program->instructions[index];
            for (int operand = 1; operand < instruction->operand_count; ++operand)
                if (instruction->operands[operand].type == OPERAND_TYPE_INPUT) {
                    if (!control_point_source_supported(ctx, index, operand, plan)) return false;
                    ++point_reads;
                }
        }
        if (!point_reads) return false;
    }
    for (int index = scope->first_instruction_index; index < scope->end_instruction_index; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        if (hlsl_instruction_owners_contains(&plan->index_transports, index)) continue;
        if (phase == plan->control_point_phase && instruction->operand_count &&
            usil_operand_destination_lane_mask(&instruction->operands[0]) != 15) return false;
        for (int operand = 1; operand < instruction->operand_count; ++operand) {
            if (instruction->operands[operand].type == OPERAND_TYPE_CONSTANT_BUFFER &&
                !factor_material_source_supported(ctx, index, operand, plan)) return false;
            if (instruction->operands[operand].type == OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER &&
                !factor_constant_source_supported(ctx, index, operand, plan)) return false;
            if (instruction->operands[operand].type == OPERAND_TYPE_TEMP) {
                USILOperandUseInfo use;
                if (!usil_instruction_operand_use(ctx->program, instruction, operand, &use) ||
                    use.use != USIL_OPERAND_USE_SOURCE) return false;
                for (unsigned lane = 0; lane < 4; ++lane)
                    if ((use.source_lane_mask & (1u << lane)) &&
                        hlsl_instruction_owners_contains(&plan->index_transports,
                            hlsl_operand_definition(ctx, index, operand, (int)lane))) return false;
            }
        }
    }
    if (!icb_consumers_live(ctx, plan)) return false;
    if (phase != plan->control_point_phase) {
        for (int index = scope->first_instruction_index; index < scope->end_instruction_index; ++index) {
            HLSLHullFactorClampOrigin origin;
            const bool recognized = factor_clamp_origin(ctx, plan, index, &origin);
            if (plan->owners_frozen) {
                const bool retained = hlsl_instruction_owners_contains(&plan->factor_clamps, index);
                if (recognized != retained || (retained && !clamp_owners_equal(&origin, &plan->clamp_origins[index])))
                    return false;
            } else if (recognized) {
                if (!hlsl_instruction_owners_add(&plan->factor_clamps, index)) return false;
                plan->clamp_origins[index] = origin;
            }
        }
    }
    return true;
}

bool hlsl_hull_icb_plan_matches(const USILProgram *program, const HLSLHullICBPlan *expected) {
    HullSourcePlan plan = {0};
    if (!expected || !hull_contract(program, &plan)) return false;
    if (!program->icb_value_count) return hlsl_hull_icb_plans_equal(&plan.icb, expected);
    if (!expected->present || !memchr(expected->array_name, 0, sizeof(expected->array_name)) ||
        !memchr(expected->index_name, 0, sizeof(expected->index_name)) ||
        !hlsl_source_identifier_valid(expected->array_name) || !hlsl_source_identifier_valid(expected->index_name) ||
        !strcmp(expected->array_name, expected->index_name)) return false;
    memcpy(plan.names[FACTOR_OFFSETS], expected->array_name, sizeof(plan.names[FACTOR_OFFSETS]));
    memcpy(plan.names[FACTOR_INDEX], expected->index_name, sizeof(plan.names[FACTOR_INDEX]));
    HLSLEmitterContext replay = {.program = program};
    bool accepted = true;
    for (size_t phase = 0; accepted && phase < program->tessellation.phase_count; ++phase) {
        if (!prepare_phase_graph(&replay, &plan, (int)phase)) { accepted = false; break; }
        const USILHullPhase *scope = &program->tessellation.phases[phase];
        for (int index = scope->first_instruction_index; accepted && index < scope->end_instruction_index; ++index) {
            const USILInstruction *instruction = &program->instructions[index];
            for (int operand = 1; accepted && operand < instruction->operand_count; ++operand)
                if (instruction->operands[operand].type == OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER &&
                    !factor_constant_source_supported(&replay, index, operand, &plan)) accepted = false;
        }
        if (accepted && !icb_consumers_live(&replay, &plan)) accepted = false;
    }
    accepted = accepted && plan.icb.present && hlsl_hull_icb_plans_equal(&plan.icb, expected);
    free_hlsl_use_def_graph(&replay);
    free_hlsl_ssa_graph(&replay);
    free_control_flow_graph(&replay);
    return accepted;
}

bool hlsl_hull_factor_clamp_origin_matches(const HLSLExpressionOrigin *origin,
    const USILProgram *program, const char *source) {
    if (!origin || !source || origin->kind != HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP ||
        !hlsl_expression_origin_ranges_valid(origin, strlen(source))) return false;
    HullSourcePlan plan = {0};
    if (!hull_contract(program, &plan) ||
        !decoded_owner_digest(program, plan.decoded_owner_digest) ||
        origin->hull_factor_clamp.phase_index < 0 ||
        (size_t)origin->hull_factor_clamp.phase_index >= program->tessellation.phase_count ||
        origin->instruction_index < 0 || origin->instruction_index >= program->instruction_count ||
        origin->source_instruction_index != program->instructions[origin->instruction_index].source_instruction_index ||
        origin->destination_lanes != usil_operand_destination_lane_mask(
            &program->instructions[origin->instruction_index].operands[0])) return false;
    HLSLEmitterContext replay = {.program = program};
    HLSLHullFactorClampOrigin current;
    bool accepted = prepare_phase_graph(&replay, &plan, origin->hull_factor_clamp.phase_index) &&
        factor_clamp_origin(&replay, &plan, origin->instruction_index, &current) &&
        clamp_owners_equal(&current, &origin->hull_factor_clamp);
    free_hlsl_use_def_graph(&replay);
    free_hlsl_ssa_graph(&replay);
    free_control_flow_graph(&replay);
    if (!accepted) return false;
    StringBuilder attribute;
    sb_init(&attribute);
    sb_append(&attribute, "[maxtessfactor(");
    ASTExpr *maximum = ast_create_literal_bits(&current.maximum_bits, 1, AST_SCALAR_FLOAT32);
    const bool has_maximum = maximum != NULL;
    if (maximum) ast_format_expr(maximum, &attribute);
    ast_free_expr(maximum);
    sb_append(&attribute, ")]");
    accepted = has_maximum && sb_ok(&attribute) &&
        origin->definition_end - origin->definition_begin == attribute.len &&
        !memcmp(source + origin->definition_begin, attribute.buf, attribute.len);
    sb_free(&attribute);
    return accepted;
}

bool hlsl_hull_factor_clamp_map_kinds_match(const HLSLExpressionSourceMap *map, const USILProgram *program) {
    HullSourcePlan plan = {0};
    if (!hull_contract(program, &plan)) return true; /* Other HULL producers retain their own scope. */
    if (!map || map->count != (size_t)program->instruction_count ||
        !decoded_owner_digest(program, plan.decoded_owner_digest)) return false;
    HLSLEmitterContext replay = {.program = program};
    bool accepted = true;
    for (size_t phase = 0; accepted && phase < program->tessellation.phase_count; ++phase) {
        if ((int)phase == plan.control_point_phase) continue;
        if (!prepare_phase_graph(&replay, &plan, (int)phase)) { accepted = false; break; }
        const USILHullPhase *scope = &program->tessellation.phases[phase];
        for (int instruction = scope->first_instruction_index; instruction < scope->end_instruction_index; ++instruction) {
            HLSLHullFactorClampOrigin origin;
            const bool lowered = factor_clamp_origin(&replay, &plan, instruction, &origin);
            if (lowered != (map->origins[instruction].kind == HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP)) {
                accepted = false;
                break;
            }
        }
    }
    free_hlsl_use_def_graph(&replay);
    free_hlsl_ssa_graph(&replay);
    free_control_flow_graph(&replay);
    return accepted;
}

static bool allocate_names(HLSLEmitterContext *ctx, HullSourcePlan *plan) {
    static const char *const bases[HULL_SOURCE_NAME_COUNT] = {
        "HullPoint", "HullFactors", "patchConstants", "patch", "factors",
        "clipPosition", "outer", "inner", "factorIndex", "pointIndex", "controlPoint", "factorOffsets"};
    if (!hlsl_source_identifier_valid(ctx->entry_point_name) ||
        ctx->reserved_preprocessor_identifier_count > 1024) return false;
    static const char *const fixed_tokens[] = {"InputPatch", "struct", "for", "return", "const",
        "float", "float2", "float3", "float4", "uint", "asfloat", "abs", "mad", "min", "max",
        "SV_POSITION", "SV_TessFactor", "SV_InsideTessFactor", "SV_OutputControlPointID",
        "domain", "partitioning", "outputtopology", "outputcontrolpoints", "patchconstantfunc", "maxtessfactor"};
    for (size_t index = 0; index < ctx->reserved_preprocessor_identifier_count; ++index) {
        const char *reserved = ctx->reserved_preprocessor_identifiers[index];
        if (!strcmp(reserved, ctx->entry_point_name) ||
            !strcmp(reserved, dxbc_signature_semantic_name(ctx->program->inputs)) ||
            (ctx->program->icb_value_count && !strcmp(reserved, "static"))) return false;
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
    const int name_count = ctx->program->icb_value_count ? HULL_SOURCE_NAME_COUNT : FACTOR_OFFSETS;
    for (int name = 0; name < name_count; ++name) {
        const char *base = name == POINT_FIELD && plan->point_width == 3 ? "pointValue" : bases[name];
        if (!hlsl_allocate_interface_name(ctx, base, plan->names[name])) { success = false; break; }
        reserved[ctx->reserved_preprocessor_identifier_count++] = plan->names[name];
    }
    ctx->reserved_preprocessor_identifiers = original;
    ctx->reserved_preprocessor_identifier_count = original_count;
    free(reserved);
    return success;
}

static bool begin_unit(HLSLEmitterContext *ctx, uint32_t id, HLSLSourceQualityUnitKind kind) {
    /* Closing the previous quality unit may report its incomplete coverage.
     * Keep that event attached to its explicit old ID before recording the new
     * unit's contiguous event range. */
    return (!ctx->source_quality_analysis || hlsl_source_quality_analysis_begin_unit(ctx->source_quality_analysis, id, kind, true)) &&
        hlsl_stage_coverage_begin_unit(ctx, id, kind);
}

static bool observe_owned_atom(HLSLEmitterContext *ctx, const char *text, int instruction,
    uint64_t logical_id, unsigned width, const HLSLStageRootOwner *owner, size_t begin) {
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
    const bool accepted = expression && hlsl_source_quality_observe_owned_expression(ctx, expression, instruction, owner) &&
        hlsl_stage_coverage_span(ctx->stage_coverage, expression, begin, begin + strlen(text));
    ast_free_expr(expression);
    return accepted;
}

/* The declaration hook already acquired this exact literal root. Observe its
 * quality through the ordinary expression lifecycle without acquiring a
 * second root or inventing an instruction for a declaration payload. */
static bool observe_registered_icb_literal(HLSLEmitterContext *ctx, const ASTExpr *literal) {
    if (!ctx->source_quality_analysis) return true;
    hlsl_source_quality_interface_expression_begin(ctx, -1);
    ctx->source_quality_root = literal;
    ctx->source_quality_instruction = -1;
    const bool accepted = hlsl_source_quality_analysis_expression(ctx->source_quality_analysis, literal);
    ctx->source_quality_root = NULL;
    if (!accepted)
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
            HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    return accepted;
}

bool hlsl_emit_high_level_hull_stage(HLSLEmitterContext *ctx) {
    if (hlsl_high_level_hull_join_supported(ctx->program, ctx->emit_mode))
        return hlsl_emit_high_level_hull_join(ctx);
    HullSourcePlan plan = {0};
    bool emitted = false;
    if (!hull_contract(ctx->program, &plan) || !prepare_scalar_cbuffer(ctx) || !allocate_names(ctx, &plan) ||
        ctx->unity_uv_helper || ctx->readable_screen_pos_helper ||
        !decoded_owner_digest(ctx->program, plan.decoded_owner_digest) ||
        !retain_material_parameters(ctx, &plan)) goto finish;
    for (int index = 0; index < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++index) ctx->float4_functions.group[index] = -1;
    /* Analyze both independent phases before appending source. */
    for (size_t phase = 0; phase < ctx->program->tessellation.phase_count; ++phase)
        if (!prepare_phase(ctx, &plan, (int)phase)) goto finish;
    if ((ctx->program->icb_value_count && !plan.icb.present) ||
        !hlsl_hull_icb_plan_matches(ctx->program, &plan.icb)) goto finish;
    plan.owners_frozen = true;
    if (!hlsl_stage_coverage_begin(ctx) || !hlsl_stage_coverage_hull_icb_plan(ctx, &plan.icb)) goto finish;
    hlsl_expression_source_map_begin(ctx);
    StringBuilder *sb = ctx->sb;
    if (!begin_unit(ctx, 0, HLSL_SOURCE_UNIT_CONFIGURATION)) goto finish;
    if (ctx->program->cbuffer_count) {
        emit_cbuffers(ctx);
        if (!sb_ok(sb) || !hlsl_source_quality_cbuffer_inventory_complete(ctx) ||
            (ctx->diagnostic && ctx->diagnostic->status != HLSL_EMIT_STATUS_OK)) goto finish;
    }
    if (plan.icb.present) {
        const size_t declaration_begin = sb->len;
        sb_appendf(sb, "static const float %s[%zu] = {", plan.icb.array_name, plan.icb.row_count);
        hlsl_source_quality_emission(ctx, 0, false, -1);
        for (size_t row = 0; row < plan.icb.row_count; ++row) {
            if (row) sb_append(sb, ", ");
            const uint32_t bits = plan.icb.payload[row * 4 + plan.icb.physical_column];
            ASTExpr *literal = ast_create_literal_bits(&bits, 1, AST_SCALAR_FLOAT32);
            if (!literal || !hlsl_stage_coverage_hull_icb_literal(ctx, literal, row) ||
                !observe_registered_icb_literal(ctx, literal)) {
                ast_free_expr(literal);
                goto finish;
            }
            const size_t literal_begin = sb->len;
            ast_format_expr(literal, sb);
            const bool captured = hlsl_stage_coverage_span(ctx->stage_coverage, literal, literal_begin, sb->len);
            ast_free_expr(literal);
            if (!captured || !sb_ok(sb)) goto finish;
        }
        sb_append(sb, "};\n\n");
        if (!sb_ok(sb) || !hlsl_stage_coverage_hull_icb_declaration(ctx, declaration_begin, sb->len)) goto finish;
        hlsl_source_quality_emission(ctx, 0, false, -1);
        /* Exact declaration ownership supplies source integrity; logical local
         * declaration completeness remains a separate future obligation. */
        if (ctx->source_quality_analysis &&
            !hlsl_source_quality_analysis_mark_incomplete_unit(ctx->source_quality_analysis)) goto finish;
    }
    sb_appendf(sb, "struct %s {\n    float%u %s : %s;\n};\n\n", plan.names[POINT_TYPE], plan.point_width, plan.names[POINT_FIELD],
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
    /* FXC retains a for-loop variable in its enclosing scope. Quad's two
     * indexed factor phases therefore need separate lexical phase scopes. */
    const bool separated_indexed_groups = plan.shape.outer_count > 1 && plan.shape.inner_count > 1;
    for (int phase = 0; phase < (int)ctx->program->tessellation.phase_count; ++phase) {
        if (phase == plan.control_point_phase) continue;
        if (!prepare_phase(ctx, &plan, phase)) goto finish;
        const USILHullPhase *owned = &ctx->program->tessellation.phases[phase];
        const unsigned instances = owned->instance_count;
        if (instances > 1) {
            if (separated_indexed_groups) {
                sb_append(sb, "    {\n");
                hlsl_source_quality_emission(ctx, 0, false, -1);
            }
            sb_append(sb, separated_indexed_groups ? "        for (uint " : "    for (uint ");
            const size_t index_begin = sb->len;
            sb_appendf(sb, "%s = 0; %s < %u; ++%s) {\n", plan.names[FACTOR_INDEX], plan.names[FACTOR_INDEX], instances, plan.names[FACTOR_INDEX]);
            ctx->indent = separated_indexed_groups ? 12 : 8;
            hlsl_source_quality_emission(ctx, 0, true, -1);
            for (int instruction = owned->first_instruction_index; instruction < owned->end_instruction_index; ++instruction)
                if (hlsl_instruction_owners_contains(&plan.index_transports, instruction)) {
                    const HLSLStageRootOwner owner = {.kind = HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE,
                        .phase_index = phase, .source_instruction_index = owned->marker_source_instruction_index,
                        .instance_count = owned->instance_count};
                    if (!observe_owned_atom(ctx, plan.names[FACTOR_INDEX], instruction,
                            UINT64_C(0x8000000000000000) | (uint64_t)phase << 32, 1, &owner, index_begin)) goto finish;
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
            .destination_supported = destination_supported, .append_destination = append_destination,
            .source_supported = factor_source_supported,
            .source_expression = factor_source_expression,
            .hull_factor_clamp_supported = factor_clamp_supported,
            .assignment_span = record_factor_assignment, .context = &plan};
        if (!hlsl_emit_pure_expression_scope(ctx, &scope)) goto finish;
        const size_t phase_end_begin = sb->len;
        sb_append(sb, separated_indexed_groups && instances > 1 ? "        }\n    }\n" : "    }\n");
        hlsl_source_quality_emission(ctx, 0, false, owned->end_instruction_index - 1);
        if (ctx->expression_source_map) {
            HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[owned->end_instruction_index - 1];
            origin->source_begin = phase_end_begin;
            origin->source_end = sb->len;
        }
    }
    ctx->indent = 4;
    const size_t factor_return_begin = sb->len + strlen("    return ");
    sb_appendf(sb, "    return %s;\n}\n\n", plan.names[FACTOR_VARIABLE]);
    const HLSLStageRootOwner factor_return = {.kind = HLSL_STAGE_ROOT_HULL_FACTOR_RETURN,
        .phase_index = -1, .source_instruction_index = UINT32_MAX};
    if (!observe_owned_atom(ctx, plan.names[FACTOR_VARIABLE], -1, UINT64_C(0x8000000000000001), 0,
            &factor_return, factor_return_begin)) goto finish;
    hlsl_source_quality_emission(ctx, 0, false, -1);
    if (!begin_unit(ctx, 2, HLSL_SOURCE_UNIT_ENTRY_POINT)) goto finish;
    sb_appendf(sb, "[domain(\"%s\")]\n[partitioning(\"%s\")]\n[outputtopology(\"%s\")]\n[outputcontrolpoints(%u)]\n[patchconstantfunc(\"%s\")]\n[maxtessfactor(",
        plan.shape.attribute, partitioning_name(ctx->program->tessellation.partitioning),
        ctx->program->tessellation.output_primitive == DXBC_TESSELLATOR_OUTPUT_LINE ? "line" :
            ctx->program->tessellation.output_primitive == DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW ? "triangle_cw" : "triangle_ccw",
        (unsigned)ctx->program->tessellation.output_control_point_count, plan.names[PATCH_FUNCTION]);
    plan.maximum_attribute_begin = sb->len - strlen("[maxtessfactor(");
    const size_t maximum_begin = sb->len;
    ASTExpr *maximum = ast_create_literal_bits(&ctx->program->tessellation.max_tessellation_factor_bits, 1, AST_SCALAR_FLOAT32);
    const HLSLStageRootOwner maximum_owner = {.kind = HLSL_STAGE_ROOT_HULL_MAXIMUM, .phase_index = -1,
        .source_instruction_index = ctx->program->tessellation.max_tessellation_factor_source_instruction_index,
        .value_bits = ctx->program->tessellation.max_tessellation_factor_bits};
    if (!maximum || !hlsl_source_quality_observe_owned_expression(ctx, maximum, -1, &maximum_owner)) { ast_free_expr(maximum); goto finish; }
    ast_format_expr(maximum, sb);
    const bool maximum_span = hlsl_stage_coverage_span(ctx->stage_coverage, maximum, maximum_begin, sb->len);
    ast_free_expr(maximum);
    if (!maximum_span) goto finish;
    plan.maximum_attribute_end = sb->len + 2; /* Closing )] is emitted with the entry signature. */
    if (plan.control_point_phase < 0) {
        sb_appendf(sb, ")]\n%s %s(InputPatch<%s, %u> %s, uint %s : SV_OutputControlPointID) {\n    return %s[%s];\n}\n",
            plan.names[POINT_TYPE], ctx->entry_point_name, plan.names[POINT_TYPE],
            (unsigned)ctx->program->tessellation.input_control_point_count, plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX], plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX]);
        char copy[256];
        const HLSLStageRootOwner copy_owner = {.kind = HLSL_STAGE_ROOT_HULL_IMPLICIT_COPY,
            .phase_index = -1, .source_instruction_index = UINT32_MAX};
        if (!hlsl_format_checked(ctx, copy, sizeof(copy), "%s[%s]", plan.names[PATCH_VARIABLE], plan.names[POINT_INDEX]) ||
            !observe_owned_atom(ctx, copy, -1, UINT64_C(0x8000000000000002), 0, &copy_owner,
                sb->len - strlen(";\n}\n") - strlen(copy))) goto finish;
        hlsl_source_quality_emission(ctx, 0, false, -1);
    } else {
        if (!prepare_phase(ctx, &plan, plan.control_point_phase)) goto finish;
        const USILHullPhase *phase = &ctx->program->tessellation.phases[plan.control_point_phase];
        sb_appendf(sb, ")]\n%s %s(InputPatch<%s, %u> %s, uint ", plan.names[POINT_TYPE], ctx->entry_point_name,
            plan.names[POINT_TYPE], (unsigned)ctx->program->tessellation.input_control_point_count, plan.names[PATCH_VARIABLE]);
        const size_t point_index_begin = sb->len;
        sb_appendf(sb, "%s : SV_OutputControlPointID) {\n    %s %s;\n", plan.names[POINT_INDEX],
            plan.names[POINT_TYPE], plan.names[POINT_VARIABLE]);
        hlsl_source_quality_emission(ctx, 0, false, -1);
        for (int instruction = phase->first_instruction_index; instruction < phase->end_instruction_index; ++instruction)
            if (hlsl_instruction_owners_contains(&plan.index_transports, instruction)) {
                const HLSLStageRootOwner owner = {.kind = HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE,
                    .phase_index = plan.control_point_phase, .source_instruction_index = phase->marker_source_instruction_index,
                    .instance_count = phase->instance_count};
                if (!observe_owned_atom(ctx, plan.names[POINT_INDEX], instruction, UINT64_C(0x8000000000000200), 1,
                        &owner, point_index_begin)) goto finish;
                hlsl_source_quality_emission(ctx, 0, false, instruction);
                if (ctx->expression_source_map) {
                    HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[instruction];
                    origin->kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
                    origin->source_begin = point_index_begin;
                    origin->source_end = point_index_begin + strlen(plan.names[POINT_INDEX]);
                }
            }
        ctx->indent = 4;
        const HLSLPureExpressionScope scope = {.first_instruction = phase->first_instruction_index,
            .end_instruction = phase->end_instruction_index, .omitted_instructions = &plan.index_transports,
            .destination_supported = destination_supported, .append_destination = append_destination,
            .source_supported = control_point_source_supported, .source_expression = control_point_source_expression, .context = &plan};
        if (!hlsl_emit_pure_expression_scope(ctx, &scope)) goto finish;
        const size_t return_begin = sb->len;
        sb_appendf(sb, "    return %s;\n}\n", plan.names[POINT_VARIABLE]);
        const HLSLStageRootOwner point_return = {.kind = HLSL_STAGE_ROOT_HULL_POINT_RETURN,
            .phase_index = plan.control_point_phase, .source_instruction_index = UINT32_MAX};
        if (!observe_owned_atom(ctx, plan.names[POINT_VARIABLE], -1, UINT64_C(0x8000000000000300), 0,
                &point_return, return_begin + strlen("    return "))) goto finish;
        hlsl_source_quality_emission(ctx, 0, false, phase->end_instruction_index - 1);
        if (ctx->expression_source_map) {
            HLSLExpressionOrigin *origin = &ctx->expression_source_map->origins[phase->end_instruction_index - 1];
            origin->source_begin = return_begin;
            origin->source_end = sb->len;
        }
    }
    for (size_t phase = 0; phase < ctx->program->tessellation.phase_count; ++phase)
        if (!prepare_phase(ctx, &plan, (int)phase)) goto finish;
    if (ctx->expression_source_map) {
        for (int instruction = 0; instruction < ctx->program->instruction_count; ++instruction) {
            if (!hlsl_instruction_owners_contains(&plan.factor_clamps, instruction)) continue;
            if (!retained_clamp_source_origin(ctx, &plan, instruction,
                    &ctx->expression_source_map->origins[instruction])) goto finish;
        }
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
        hlsl_stage_coverage_finish(ctx);
        if (ctx->stage_coverage && !ctx->stage_coverage->finished && emitted) {
            hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            emitted = false;
        }
        hlsl_source_quality_analysis_destroy(ctx->source_quality_analysis);
        ctx->source_quality_analysis = NULL;
    }
    if (emitted) {
        uint8_t current[COMMON_SHA256_DIGEST_SIZE];
        if (!material_parameters_match(ctx, &plan) || !decoded_owner_digest(ctx->program, current) ||
            memcmp(current, plan.decoded_owner_digest, sizeof(current)) ||
            !hlsl_hull_icb_plan_matches(ctx->program, &plan.icb) ||
            (ctx->expression_source_map &&
             !hlsl_expression_source_map_matches(ctx->expression_source_map, ctx->program, ctx->sb->buf))) {
            hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
            emitted = false;
        }
        for (int instruction = 0; emitted && ctx->expression_source_map && instruction < ctx->program->instruction_count; ++instruction) {
            if (!hlsl_instruction_owners_contains(&plan.factor_clamps, instruction)) continue;
            HLSLExpressionOrigin retained;
            if (!retained_clamp_source_origin(ctx, &plan, instruction, &retained) ||
                !hlsl_expression_origins_equal(&retained, &ctx->expression_source_map->origins[instruction])) {
                hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
                emitted = false;
            }
        }
    }
    if (!emitted && ctx->expression_source_map)
        memset(ctx->expression_source_map, 0, sizeof(*ctx->expression_source_map));
    for (unsigned source = 0; source < 2; ++source)
        serialized_program_parameters_free(&plan.material_parameters[source]);
    return emitted;
}
