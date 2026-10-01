// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_identifier.h"
#include "translation/usil_validation.h"
#include "hlsl_compute_source_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A bounded source projection, not the Class72 declaration inverse. The
 * existing lossless decoder and USIL execution contract supply all metadata;
 * existing CFG/SSA supplies definition ownership. A private candidate route
 * admits one UINT4 texture or structured bits expression, or one FLOAT4 UAV
 * read/add/store with original metadata. A separate complete typed atomic use
 * proves a scalar UINT UAV view. Signed data, other memory families and
 * control flow remain unavailable. */
/* This stage retains its own bound; generic V/F capacity grants no wider
 * compute admission or effect-planning authority. */
enum { COMPUTE_SOURCE_INSTRUCTION_LIMIT = 64 };

typedef struct {
    DXBCOperandType type;
    uint8_t flag;
    uint8_t width;
    const char *identifier;
    const char *semantic;
} ComputeBuiltin;

static const ComputeBuiltin compute_builtins[] = {
    {OPERAND_TYPE_INPUT_THREAD_ID, USIL_COMPUTE_DISPATCH_THREAD_ID, 3,
     "dispatchThreadId", "SV_DispatchThreadID"},
    {OPERAND_TYPE_INPUT_THREAD_GROUP_ID, USIL_COMPUTE_GROUP_ID, 3,
     "groupId", "SV_GroupID"},
    {OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP, USIL_COMPUTE_GROUP_THREAD_ID, 3,
     "groupThreadId", "SV_GroupThreadID"},
    {OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED, USIL_COMPUTE_GROUP_INDEX, 1,
     "groupIndex", "SV_GroupIndex"}
};

static const ComputeBuiltin *compute_builtin(DXBCOperandType type) {
    for (size_t index = 0; index < sizeof(compute_builtins) / sizeof(compute_builtins[0]); ++index)
        if (compute_builtins[index].type == type) return &compute_builtins[index];
    return NULL;
}

static unsigned component_count(uint8_t lanes) {
    unsigned count = 0;
    for (unsigned lane = 0; lane < 4; ++lane) count += (lanes >> lane) & 1u;
    return count;
}

static const char *uint_type(unsigned width) {
    static const char *const names[] = {NULL, "uint", "uint2", "uint3", "uint4"};
    return width >= 1 && width <= 4 ? names[width] : NULL;
}


/* These operators preserve 32-bit unsigned words at each natural width.
 * The shared AST retains their actual opcode; no signed shift, conversion or
 * floating-point operation is admitted through this vocabulary. */
static bool uint_operation(USILOpcode opcode) {
    return opcode == USIL_OP_IADD || opcode == USIL_OP_AND || opcode == USIL_OP_OR ||
           opcode == USIL_OP_XOR || opcode == USIL_OP_NOT ||
           opcode == USIL_OP_ISHL || opcode == USIL_OP_USHR;
}

static bool reject_stage(HLSLEmitterContext *ctx) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                   HLSL_EMIT_REASON_UNSUPPORTED_STAGE);
    return false;
}

static bool reject_instruction(HLSLEmitterContext *ctx, int index, HLSLEmitReason reason) {
    hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, reason, index, -1);
    return false;
}

static const char *barrier_intrinsic(uint8_t flags) {
    switch (flags) {
    case DXBC_SYNC_THREAD_GROUP_SHARED_MEMORY:
        return "GroupMemoryBarrier";
    case DXBC_SYNC_THREAD_GROUP_SHARED_MEMORY | DXBC_SYNC_THREADS_IN_GROUP:
        return "GroupMemoryBarrierWithGroupSync";
    case DXBC_SYNC_UAV_MEMORY_GLOBAL:
        return "DeviceMemoryBarrier";
    case DXBC_SYNC_UAV_MEMORY_GLOBAL | DXBC_SYNC_THREADS_IN_GROUP:
        return "DeviceMemoryBarrierWithGroupSync";
    case DXBC_SYNC_THREAD_GROUP_SHARED_MEMORY | DXBC_SYNC_UAV_MEMORY_GLOBAL:
        return "AllMemoryBarrier";
    case DXBC_SYNC_THREAD_GROUP_SHARED_MEMORY | DXBC_SYNC_UAV_MEMORY_GLOBAL |
         DXBC_SYNC_THREADS_IN_GROUP:
        return "AllMemoryBarrierWithGroupSync";
    default:
        /* UAV group visibility is a distinct contract. Without its exact
         * source declaration context, do not strengthen it to device scope
         * or split one ordered barrier into several different operations. */
        return NULL;
    }
}

static bool typed_resources_valid(HLSLEmitterContext *ctx, const HLSLComputeTypedSource *typed) {
    const USILProgram *program = ctx->program;
    const bool scalar_atomic = typed && typed->resource_count == 1u && typed->resources &&
        typed->resources[0].scalar_atomic;
    const bool system_values_valid = scalar_atomic
        ? program->compute.system_value_mask == 0u ||
          program->compute.system_value_mask == USIL_COMPUTE_DISPATCH_THREAD_ID
        : program->compute.system_value_mask == USIL_COMPUTE_DISPATCH_THREAD_ID;
    if (!typed || typed->resource_count < 1 || typed->resource_count > 2 || !typed->resources ||
        program->shader_model_major != 5 || program->shader_model_minor != 0 ||
        program->compute.shared_memory_count || program->compute.barrier_count ||
        !system_values_valid ||
        program->texture_count < 0 || program->texture_count > 1 || program->uav_count != 1 ||
        program->texture_alloc < program->texture_count || program->uav_alloc < program->uav_count ||
        !program->uavs || (program->texture_count && !program->textures) ||
        typed->resource_count != (size_t)(program->texture_count + program->uav_count))
        return reject_stage(ctx);
    for (size_t index = 0; index < typed->resource_count; ++index) {
        const HLSLComputeTypedResource *resource = &typed->resources[index];
        if (resource->scalar_atomic != scalar_atomic ||
            (scalar_atomic && (!resource->writable || resource->structured ||
                resource->scalar_type != AST_SCALAR_UINT32))) return reject_stage(ctx);
        if (!hlsl_source_identifier_valid(resource->name) || !strcmp(resource->name, "dispatchThreadId")) return reject_stage(ctx);
        for (size_t previous = 0; previous < index; ++previous)
            if (strcmp(resource->name, typed->resources[previous].name) == 0 ||
                (resource->writable == typed->resources[previous].writable &&
                 resource->binding_register == typed->resources[previous].binding_register))
                return reject_stage(ctx);
        const uint8_t *formats;
        const char *dimension = resource->structured ? "structured" : "2d";
        const int stride = resource->structured ? 16 : 0;
        if (resource->structured != typed->resources[0].structured) return reject_stage(ctx);
        if (resource->writable) {
            const USILUav *uav = &program->uavs[0];
            if (resource->binding_register != (uint32_t)uav->reg_idx ||
                uav->reg_idx < 0 || uav->reg_idx >= HLSL_SM5_UAV_REGISTER_COUNT ||
                !memchr(uav->dimension, 0, sizeof(uav->dimension)) || strcmp(uav->dimension, dimension) ||
                uav->stride != stride || uav->sample_count || uav->globally_coherent ||
                uav->rasterizer_ordered || uav->has_order_preserving_counter)
                return reject_stage(ctx);
            formats = uav->return_types;
        } else {
            if (!program->texture_count) return reject_stage(ctx);
            const USILTexture *texture = &program->textures[0];
            if (resource->binding_register != (uint32_t)texture->reg_idx ||
                texture->reg_idx < 0 || texture->reg_idx >= HLSL_SM5_RESOURCE_REGISTER_COUNT ||
                !memchr(texture->dimension, 0, sizeof(texture->dimension)) || strcmp(texture->dimension, dimension) ||
                texture->stride != stride || texture->sample_count) return reject_stage(ctx);
            formats = texture->return_types;
        }
        if ((resource->scalar_type != AST_SCALAR_UINT32 && resource->scalar_type != AST_SCALAR_FLOAT32) ||
            (resource->scalar_type == AST_SCALAR_FLOAT32 &&
             (resource->structured || !resource->writable || typed->resource_count != 1u || program->texture_count)))
            return reject_stage(ctx);
        const uint8_t format = resource->structured ? 0u :
            (resource->scalar_type == AST_SCALAR_FLOAT32 ? 5u : 4u);
        for (unsigned component = 0; component < 4; ++component)
            if (formats[component] != format) return reject_stage(ctx);
    }
    return true;
}

static bool source_program_valid(HLSLEmitterContext *ctx, const HLSLComputeTypedSource *typed) {
    const USILProgram *program = ctx->program;
    const USILComputeContract *compute = &program->compute;
    char model[32];
    snprintf(model, sizeof(model), "cs_%u_%u", program->shader_model_major, program->shader_model_minor);
    if (!program->has_stage_contract || !compute->valid ||
        !memchr(program->shader_type_model, '\0', sizeof(program->shader_type_model)) ||
        strcmp(program->shader_type_model, model) != 0 ||
        program->program_type != DXBC_PROGRAM_TYPE_COMPUTE ||
        (program->shader_model_major != 4 && program->shader_model_major != 5) ||
        (program->shader_model_major == 5 && program->shader_model_minor != 0) ||
        (program->shader_model_major == 4 && program->shader_model_minor > 1) ||
        program->instruction_count < 1 ||
        program->instruction_count > COMPUTE_SOURCE_INSTRUCTION_LIMIT ||
        program->instruction_alloc < program->instruction_count || !program->instructions ||
        program->temp_count < 0 || program->temp_count > HLSL_SM5_TEMP_REGISTER_COUNT ||
        program->signature_declaration_count < 0 || program->signature_declaration_count > 4 ||
        program->signature_declaration_alloc < program->signature_declaration_count ||
        (program->signature_declaration_count && !program->signature_declarations) ||
        !usil_signature_authority_is_valid(program)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM, HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                       HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE);
        return false;
    }
    if (program->input_count || program->output_count || program->patch_constant_count ||
        program->cbuffer_count || program->sampler_count ||
        (!typed && (program->texture_count || program->uav_count)) || program->indexable_temp_count || program->index_range_count ||
        program->icb_value_count || program->geometry.valid || program->tessellation.valid ||
        compute->barrier_count > (size_t)program->instruction_count ||
        (program->has_global_flags && program->global_flags != 1u))
        return reject_stage(ctx);

    if (typed && !typed_resources_valid(ctx, typed)) return false;
    uint64_t thread_count = 1;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const uint32_t value = compute->thread_group_size[axis];
        if (!value || value > (axis == 2 ? (program->shader_model_major == 5 ? 64u : 1u) : 1024u))
            return reject_stage(ctx);
        thread_count *= value;
    }
    if (thread_count > (program->shader_model_major == 5 ? 1024u : 768u))
        return reject_stage(ctx);
    uint8_t system_values = 0;
    for (int index = 0; index < program->signature_declaration_count; ++index) {
        const ComputeBuiltin *builtin = compute_builtin(program->signature_declarations[index].operand_type);
        if (!builtin || (system_values & builtin->flag)) return reject_stage(ctx);
        system_values |= builtin->flag;
    }
    if (system_values != compute->system_value_mask ||
        compute->shared_memory_count > compute->shared_memory_capacity ||
        compute->shared_memory_capacity > 8192u ||
        (compute->shared_memory_capacity == 0) != (compute->shared_memory == NULL) ||
        (program->shader_model_major != 5 && compute->shared_memory_count))
        return reject_stage(ctx);
    uint64_t memory_bytes = 0;
    for (size_t index = 0; index < compute->shared_memory_count; ++index) {
        const DXBCThreadGroupSharedMemoryContract *memory = &compute->shared_memory[index];
        if (memory->register_id >= 8192u || !memory->byte_count || memory->byte_count > 32768u ||
            (memory->byte_count & 3u)) return reject_stage(ctx);
        if (memory->structured) {
            if (!memory->byte_stride || (memory->byte_stride & 3u) || !memory->element_count ||
                (uint64_t)memory->byte_stride * memory->element_count != memory->byte_count)
                return reject_stage(ctx);
        } else if (memory->byte_stride || memory->element_count) {
            return reject_stage(ctx);
        }
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (compute->shared_memory[earlier].register_id == memory->register_id)
                return reject_stage(ctx);
        memory_bytes += memory->byte_count;
    }
    if (memory_bytes > 32768u || memory_bytes != compute->shared_memory_bytes)
        return reject_stage(ctx);
    return true;
}

static bool operand_plain(const DXBCOperand *operand) {
    return operand && !operand->has_abs && !operand->has_neg && !operand->min_precision &&
           !operand->rel_op0 && !operand->rel_op1 && !operand->rel_op2 &&
           !operand->extended_token_count && !operand->extended_tokens && operand->destination_mask == 0;
}

static bool static_indices(const DXBCOperand *operand, unsigned dimensions) {
    if (operand->register_index_dim != (int)dimensions || (dimensions && operand->register_index < 0))
        return false;
    for (unsigned index = 0; index < dimensions; ++index)
        if (operand->index_representations[index] != 0u || !operand->index_has_immediate[index] ||
            operand->index_value_exceeds_int[index] ||
            operand->index_values[index] != (uint32_t)operand->register_index) return false;
    return true;
}

static bool instruction_valid(HLSLEmitterContext *ctx, int index, bool natural_projection, bool float_add) {
    const USILProgram *program = ctx->program;
    const USILInstruction *instruction = &program->instructions[index];
    if (!usil_instruction_shape_valid(program, instruction) || instruction->saturate ||
        instruction->precise_mask || instruction->has_resource_dimension ||
        instruction->has_resource_return_types || instruction->has_texel_offset ||
        instruction->condition_test != DXBC_INSTRUCTION_TEST_NONE ||
        instruction->resource_dimension[0] || instruction->resource_stride ||
        instruction->resource_info_return_type || instruction->sample_info_return_type ||
        instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE ||
        instruction->geometry_stream_id || instruction->geometry_stream_explicit)
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (unsigned lane = 0; lane < 4; ++lane)
        if (instruction->resource_return_types[lane])
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (unsigned axis = 0; axis < 3; ++axis)
        if (instruction->texel_offsets[axis])
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    if (instruction->opcode == USIL_OP_SYNC) {
        if (program->shader_model_major != 5 || !barrier_intrinsic(instruction->sync_flags))
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        USILEffectFlags effects;
        const USILEffectFlags expected = USIL_EFFECT_MEMORY_BARRIER |
            ((instruction->sync_flags & DXBC_SYNC_THREADS_IN_GROUP) ? USIL_EFFECT_CONTROL : 0u);
        if (!usil_instruction_effects(program, instruction, &effects) || effects != expected)
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
        return true;
    }
    if (instruction->opcode == USIL_OP_RET)
        return index == program->instruction_count - 1 ||
               reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    if (instruction->opcode != USIL_OP_MOV && !uint_operation(instruction->opcode) &&
        !(float_add && instruction->opcode == USIL_OP_ADD))
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_OPCODE);
    USILEffectFlags effects;
    if (!usil_instruction_effects(program, instruction, &effects) || effects != USIL_EFFECT_NONE)
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    const DXBCOperand *destination = &instruction->operands[0];
    if (destination->type != OPERAND_TYPE_TEMP || !static_indices(destination, 1) ||
        destination->register_index >= program->temp_count ||
        !usil_operand_destination_lane_mask(destination) || destination->swizzle_mode ||
        destination->has_abs || destination->has_neg || destination->min_precision ||
        destination->rel_op0 || destination->rel_op1 || destination->rel_op2 ||
        destination->extended_token_count || destination->extended_tokens)
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
    const uint8_t lanes = usil_operand_destination_lane_mask(destination);
    for (int operand_index = 1; operand_index < instruction->operand_count; ++operand_index) {
        const DXBCOperand *source = &instruction->operands[operand_index];
        if (!operand_plain(source))
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        if (source->type == OPERAND_TYPE_TEMP) {
            if (!static_indices(source, 1) || source->register_index >= program->temp_count)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        } else if (source->type == OPERAND_TYPE_IMMEDIATE32) {
            if (source->register_index_dim || (source->imm_value_count != 1 && source->imm_value_count != 4) ||
                source->immediate_word_count != source->imm_value_count)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            for (int component = 0; component < source->imm_value_count; ++component)
                if (source->imm_values[component] != source->immediate_words[component])
                    return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        } else {
            const ComputeBuiltin *builtin = compute_builtin(source->type);
            if (!builtin || !static_indices(source, 0) ||
                !(program->compute.system_value_mask & builtin->flag))
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            uint8_t declared = 0;
            for (int declaration = 0; declaration < program->signature_declaration_count; ++declaration)
                if (program->signature_declarations[declaration].operand_type == source->type)
                    declared = builtin->width == 1 ? 1u : program->signature_declarations[declaration].mask;
            int previous = -1;
            for (int lane = 0; lane < 4; ++lane) {
                if (!(lanes & (1u << lane))) continue;
                const int component = usil_operand_source_component(source, lane);
                if (component < 0 || component >= builtin->width || !(declared & (1u << component)) ||
                    (natural_projection && component <= previous))
                    return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
                previous = component;
            }
        }
    }
    return true;
}

static ASTExpr *logical_value(HLSLEmitterContext *ctx, ASTExpr *expression, int instruction,
                              uint8_t lanes) {
    if (!expression) return NULL;
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = AST_SCALAR_UINT32;
    origin.components = (uint8_t)component_count(lanes);
    origin.logical_value_id = (uint64_t)instruction;
    origin.instruction_index = instruction;
    origin.source_instruction_index = ctx->program->instructions[instruction].source_instruction_index;
    origin.destination_lanes = lanes;
    if (!ast_set_logical_value_origin(expression, &origin)) {
        ast_free_expr(expression);
        return NULL;
    }
    return expression;
}

static ASTExpr *source_expression(HLSLEmitterContext *ctx, int index, int operand_index) {
    const USILInstruction *instruction = &ctx->program->instructions[index];
    const DXBCOperand *source = &instruction->operands[operand_index];
    const uint8_t lanes = usil_operand_destination_lane_mask(&instruction->operands[0]);
    const unsigned width = component_count(lanes);
    if (source->type == OPERAND_TYPE_TEMP) {
        int definition = -1;
        for (int lane = 0; lane < 4; ++lane) {
            if (!(lanes & (1u << lane))) continue;
            const int current = hlsl_operand_definition(ctx, index, operand_index, lane);
            if (current < 0 || current >= index || (definition >= 0 && definition != current)) return NULL;
            definition = current;
        }
        const uint8_t producer_lanes = usil_operand_destination_lane_mask(
            &ctx->program->instructions[definition].operands[0]);
        if (component_count(producer_lanes) != width) return NULL;
        unsigned component = 0;
        for (int lane = 0; lane < 4; ++lane) {
            if (!(lanes & (1u << lane))) continue;
            const int selected = usil_operand_source_component(source, lane);
            if (selected < 0 || selected > 3 || !(producer_lanes & (1u << selected)) ||
                component_count((uint8_t)(producer_lanes & ((1u << selected) - 1u))) != component++)
                return NULL;
        }
        char name[48];
        snprintf(name, sizeof(name), "value_%d", definition);
        return logical_value(ctx, ast_create_var(definition, source->register_index, source->type, name),
                             definition, producer_lanes);
    }
    if (source->type == OPERAND_TYPE_IMMEDIATE32) {
        uint32_t bits[4];
        unsigned component = 0;
        for (int lane = 0; lane < 4; ++lane) {
            if (!(lanes & (1u << lane))) continue;
            const int selected = source->imm_value_count == 1 ? 0 : usil_operand_source_component(source, lane);
            if (selected < 0 || selected >= source->imm_value_count) return NULL;
            bits[component++] = source->imm_values[selected];
        }
        return ast_create_literal_bits(bits, (int)width, AST_SCALAR_UINT32);
    }
    const ComputeBuiltin *builtin = compute_builtin(source->type);
    if (!builtin) return NULL;
    ASTOperandProvenance origin;
    ast_operand_provenance_init(&origin);
    origin.complete = true;
    origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    origin.logical_value_id = UINT64_C(0x100000000) | builtin->flag;
    origin.natural_components = builtin->width;
    origin.result_components = (uint8_t)width;
    origin.instruction_index = index;
    origin.source_instruction_index = instruction->source_instruction_index;
    origin.operand_index = operand_index;
    origin.destination_lanes = lanes;
    char text[64];
    size_t length = strlen(builtin->identifier);
    memcpy(text, builtin->identifier, length + 1u);
    bool identity = width == builtin->width;
    unsigned component = 0;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(lanes & (1u << lane))) continue;
        const int selected = usil_operand_source_component(source, lane);
        origin.selected_components[component] = (uint8_t)selected;
        identity = identity && selected == (int)component;
        ++component;
    }
    if (!identity) {
        text[length++] = '.';
        for (unsigned selected = 0; selected < width; ++selected)
            text[length++] = "xyz"[origin.selected_components[selected]];
        text[length] = '\0';
        origin.selection_role = AST_COMPONENT_SELECTION_SEMANTIC;
    }
    return ast_create_emitter_operand_with_provenance(text, &origin);
}

/* Bounded typed memory expression projection. Registers are only SSA
 * evidence: the emitted source consists of resource elements, logical vector
 * addresses and typed values. A load is consumed exactly once and never moved
 * across another effect. Each source node retains its original owner. */
typedef struct {
    HLSLEmitterContext *ctx;
    const HLSLComputeTypedSource *typed;
    uint8_t consumed[COMPUTE_SOURCE_INSTRUCTION_LIMIT];
    int store;
    int load;
    unsigned expanded_nodes;
    unsigned load_expansions;
} ComputeMemoryPlan;

static const HLSLComputeTypedResource *typed_binding(const HLSLComputeTypedSource *typed,
                                                    bool writable, uint32_t binding) {
    for (size_t index = 0; index < typed->resource_count; ++index)
        if (typed->resources[index].writable == writable &&
            typed->resources[index].binding_register == binding) return &typed->resources[index];
    return NULL;
}

static bool atomic_dispatch_address_valid(const USILProgram *program, const DXBCOperand *address,
                                           uint8_t demanded_lanes, bool store) {
    if (program->compute.system_value_mask != USIL_COMPUTE_DISPATCH_THREAD_ID ||
        program->signature_declaration_count != 1 || program->signature_declaration_alloc < 1 ||
        !program->signature_declarations || address->type != OPERAND_TYPE_INPUT_THREAD_ID ||
        !static_indices(address, 0) ||
        address->raw_token != (store ? UINT32_C(0x00020546) : UINT32_C(0x00020046)) ||
        address->swizzle_mode != 1u || address->imm_value_count || address->immediate_word_count ||
        demanded_lanes != 3u) return false;
    const USILSignatureDeclaration *declaration = &program->signature_declarations[0];
    if (declaration->kind != USIL_SIGNATURE_DECL_INPUT ||
        declaration->operand_type != OPERAND_TYPE_INPUT_THREAD_ID || declaration->has_signature_register ||
        declaration->register_id != UINT32_MAX || declaration->mask != 3u || declaration->stream_index ||
        declaration->has_array_element_count || declaration->array_element_count ||
        declaration->has_system_value || declaration->system_value_name ||
        declaration->has_interpolation || declaration->interpolation_mode ||
        declaration->source_instruction_index >= program->compute.declaration_source_instruction_index)
        return false;
    /* The raw selector retains unused Z/W components too. Demanded XY must
     * select exactly the independently declared builtin lanes, in order. */
    const uint8_t components[4] = {0u, 1u, store ? 1u : 0u, store ? 1u : 0u};
    for (unsigned lane = 0; lane < 4; ++lane)
        if (address->swizzle[lane] != components[lane]) return false;
    for (int lane = 0; lane < 4; ++lane) if (demanded_lanes & (1u << lane)) {
        const int component = usil_operand_source_component(address, lane);
        if (component != lane || !(declaration->mask & (1u << component))) return false;
    }
    return true;
}

static bool atomic_returns_value(const USILProgram *program) {
    return program->instruction_count == 3 &&
        program->instructions[0].opcode == USIL_OP_IMM_ATOMIC_IADD;
}

static bool atomic_masked_operand_plain(const DXBCOperand *operand, uint8_t mask) {
    DXBCOperand unmasked = *operand;
    unmasked.destination_mask = 0;
    return operand->destination_mask == ((unsigned)mask << 4u) && operand_plain(&unmasked);
}

static bool typed_atomic_store_valid(HLSLEmitterContext *ctx, int index) {
    const USILProgram *program = ctx->program;
    const USILInstruction *instruction = &program->instructions[index];
    USILMemoryAccess memory;
    USILEffectFlags effects;
    if (index != 1 || instruction->opcode != USIL_OP_STORE_UAV_TYPED || instruction->operand_count != 3 ||
        instruction->has_resource_dimension || instruction->has_resource_return_types ||
        instruction->has_texel_offset || instruction->resource_stride ||
        instruction->resource_info_return_type || instruction->sample_info_return_type ||
        instruction->geometry_stream_id || instruction->geometry_stream_explicit ||
        !usil_instruction_memory_access(program, instruction, &memory) ||
        memory.kind != USIL_MEMORY_TYPED || memory.space != USIL_MEMORY_UNORDERED_ACCESS ||
        strcmp(memory.dimension, "2d") || memory.reads || !memory.writes || memory.atomic ||
        memory.globally_coherent || memory.rasterizer_ordered || memory.has_order_preserving_counter ||
        memory.counter_mode != USIL_COUNTER_NONE || memory.byte_stride || memory.shared_memory_byte_count ||
        memory.destination_operand != -1 || memory.binding_operand != 0 || memory.address_operand != 1 ||
        memory.value_operand != 2 || memory.byte_offset_operand != -1 || memory.compare_operand != -1 ||
        memory.destination_lanes || memory.address_lanes != 3u || memory.value_lanes != 15u ||
        memory.memory_component_lanes != 15u ||
        !usil_instruction_effects(program, instruction, &effects) || effects != USIL_EFFECT_EXTERNAL_WRITE)
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    for (unsigned component = 0; component < 4; ++component)
        if (memory.return_types[component] != 4u || instruction->resource_return_types[component])
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (unsigned axis = 0; axis < 3; ++axis)
        if (instruction->texel_offsets[axis])
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (int operand_index = 0; operand_index < 3; ++operand_index) {
        const DXBCOperand *operand = &instruction->operands[operand_index];
        USILOperandUseInfo use;
        if (!usil_instruction_operand_use(program, instruction, operand_index, &use) ||
            use.use != (operand_index ? USIL_OPERAND_USE_SOURCE : USIL_OPERAND_USE_RESOURCE_BINDING) ||
            use.source_lane_mask != (operand_index == 1 ? 3u : operand_index == 2 ? 15u : 0u) ||
            operand->imm_value_count || operand->immediate_word_count)
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        if (operand_index == 1) {
            if (!operand_plain(operand) || !atomic_dispatch_address_valid(program, operand, use.source_lane_mask, true))
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        } else if (!operand_index) {
            if (!atomic_masked_operand_plain(operand, 15u) || operand->type != OPERAND_TYPE_UAV ||
                !static_indices(operand, 1) || operand->swizzle_mode ||
                operand->raw_token != UINT32_C(0x0011e0f2) ||
                operand->register_index != program->instructions[0].operands[1].register_index)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            for (unsigned lane = 0; lane < 4; ++lane)
                if (operand->swizzle[lane] != lane)
                    return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        } else {
            if (!operand_plain(operand) || operand->type != OPERAND_TYPE_TEMP || !static_indices(operand, 1) ||
                operand->register_index != 0 || operand->swizzle_mode != 1u ||
                operand->raw_token != UINT32_C(0x00100006))
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            for (int lane = 0; lane < 4; ++lane)
                if (operand->swizzle[lane] || usil_operand_source_component(operand, lane) != 0)
                    return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        }
    }
    return true;
}

/* This complete SM5 use, rather than four repeated declaration formats,
 * establishes the legal scalar R32_UINT view required by typed atomics. */
static bool typed_atomic_instruction_valid(HLSLEmitterContext *ctx, int index) {
    const USILProgram *program = ctx->program;
    const USILInstruction *instruction = &program->instructions[index];
    const bool returning = atomic_returns_value(program);
    USILEffectFlags effects;
    if (index == program->instruction_count - 1) {
        if (instruction->opcode != USIL_OP_RET ||
            !usil_instruction_effects(program, instruction, &effects) || effects != USIL_EFFECT_CONTROL)
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
        return instruction_valid(ctx, index, true, false);
    }
    if (returning && index == 1) return typed_atomic_store_valid(ctx, index);
    USILMemoryAccess memory;
    const int binding_operand = returning ? 1 : 0;
    const int address_operand = returning ? 2 : 1;
    const int value_operand = returning ? 3 : 2;
    if (index != 0 || instruction->opcode != (returning ? USIL_OP_IMM_ATOMIC_IADD : USIL_OP_ATOMIC_IADD) ||
        instruction->operand_count != (returning ? 4 : 3) ||
        instruction->has_resource_dimension || instruction->has_resource_return_types ||
        instruction->has_texel_offset || instruction->resource_stride ||
        instruction->resource_info_return_type || instruction->sample_info_return_type ||
        instruction->geometry_stream_id || instruction->geometry_stream_explicit ||
        !usil_instruction_memory_access(program, instruction, &memory) ||
        memory.kind != USIL_MEMORY_TYPED || memory.space != USIL_MEMORY_UNORDERED_ACCESS ||
        strcmp(memory.dimension, "2d") || !memory.reads || !memory.writes || !memory.atomic ||
        memory.globally_coherent || memory.rasterizer_ordered || memory.has_order_preserving_counter ||
        memory.counter_mode != USIL_COUNTER_NONE || memory.byte_stride || memory.shared_memory_byte_count ||
        memory.destination_operand != (returning ? 0 : -1) || memory.binding_operand != binding_operand ||
        memory.address_operand != address_operand || memory.value_operand != value_operand ||
        memory.byte_offset_operand != -1 || memory.compare_operand != -1 ||
        memory.destination_lanes != (returning ? 1u : 0u) || memory.address_lanes != 3u || memory.value_lanes != 1u ||
        memory.memory_component_lanes != 1u ||
        !usil_instruction_effects(program, instruction, &effects) ||
        effects != (USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_EXTERNAL_WRITE | USIL_EFFECT_ATOMIC))
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    for (unsigned component = 0; component < 4; ++component)
        if (memory.return_types[component] != 4u || instruction->resource_return_types[component])
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (unsigned axis = 0; axis < 3; ++axis)
        if (instruction->texel_offsets[axis])
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (int operand_index = 0; operand_index < instruction->operand_count; ++operand_index) {
        const DXBCOperand *operand = &instruction->operands[operand_index];
        USILOperandUseInfo use;
        if (returning && !operand_index) {
            if (!atomic_masked_operand_plain(operand, 1u) || operand->type != OPERAND_TYPE_TEMP ||
                !static_indices(operand, 1) || operand->register_index || operand->swizzle_mode ||
                operand->raw_token != UINT32_C(0x00100012) || operand->imm_value_count || operand->immediate_word_count ||
                !usil_instruction_operand_use(program, instruction, operand_index, &use) ||
                use.use != USIL_OPERAND_USE_DESTINATION || use.source_lane_mask)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            for (unsigned lane = 0; lane < 4; ++lane)
                if (operand->swizzle[lane] != lane)
                    return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            continue;
        }
        if (!operand_plain(operand) ||
            !usil_instruction_operand_use(program, instruction, operand_index, &use) ||
            use.use != (operand_index == binding_operand ? USIL_OPERAND_USE_RESOURCE_BINDING : USIL_OPERAND_USE_SOURCE) ||
            use.source_lane_mask != (operand_index == address_operand ? 3u : operand_index == value_operand ? 1u : 0u))
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        if (operand_index == address_operand && operand->type == OPERAND_TYPE_INPUT_THREAD_ID) {
            if (!atomic_dispatch_address_valid(program, operand, use.source_lane_mask, false))
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            continue;
        }
        if (operand->swizzle_mode || (operand_index == address_operand &&
            (program->signature_declaration_count || program->compute.system_value_mask)))
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        for (unsigned component = 0; component < 4; ++component)
            if (operand->swizzle[component] != component)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        if (operand_index == binding_operand) {
            if (operand->type != OPERAND_TYPE_UAV || !static_indices(operand, 1) ||
                operand->raw_token != UINT32_C(0x0011e000) || operand->imm_value_count ||
                operand->immediate_word_count)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        } else {
            const int width = operand_index == address_operand ? 4 : 1;
            if (operand->type != OPERAND_TYPE_IMMEDIATE32 || !static_indices(operand, 0) ||
                operand->raw_token != (operand_index == address_operand ? UINT32_C(0x4002) : UINT32_C(0x4001)) ||
                operand->imm_value_count != width || operand->immediate_word_count != width)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
            for (int component = 0; component < width; ++component)
                if (operand->imm_values[component] != operand->immediate_words[component] ||
                    (operand_index == address_operand && component >= 2 && operand->imm_values[component]))
                    return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        }
    }
    return true;
}

static bool typed_instruction_valid(HLSLEmitterContext *ctx, const HLSLComputeTypedSource *typed, int index) {
    if (typed->resources[0].scalar_atomic) return typed_atomic_instruction_valid(ctx, index);
    const USILInstruction *instruction = &ctx->program->instructions[index];
    const bool float_data = typed->resources[0].scalar_type == AST_SCALAR_FLOAT32;
    if (instruction->opcode == USIL_OP_LD_STRUCTURED || instruction->opcode == USIL_OP_STORE_STRUCTURED) {
        USILMemoryAccess memory;
        USILEffectFlags effects;
        const bool load = instruction->opcode == USIL_OP_LD_STRUCTURED;
        if (!usil_instruction_memory_access(ctx->program, instruction, &memory) ||
            memory.kind != USIL_MEMORY_STRUCTURED || memory.byte_stride != 16u ||
            memory.space != (load ? USIL_MEMORY_SHADER_RESOURCE : USIL_MEMORY_UNORDERED_ACCESS) ||
            memory.address_lanes != 1u || !memory.memory_component_lanes ||
            (load ? !memory.destination_lanes :
                (memory.memory_component_lanes != 15u || memory.value_lanes != 15u)) ||
            memory.atomic || memory.globally_coherent || memory.rasterizer_ordered ||
            memory.has_order_preserving_counter || memory.reads != load || memory.writes == load ||
            instruction->geometry_stream_id || instruction->geometry_stream_explicit ||
            instruction->resource_info_return_type || instruction->sample_info_return_type ||
            !usil_instruction_effects(ctx->program, instruction, &effects) ||
            effects != (load ? USIL_EFFECT_RESOURCE_READ : USIL_EFFECT_EXTERNAL_WRITE))
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        const DXBCOperand *binding = &instruction->operands[memory.binding_operand];
        if (load) for (int lane = 0; lane < 4; ++lane)
            if ((memory.destination_lanes & (1u << lane)) &&
                (usil_operand_source_component(binding, lane) < 0 ||
                 usil_operand_source_component(binding, lane) > 3))
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        for (int operand = 1; operand < instruction->operand_count; ++operand)
            if (operand != memory.binding_operand && !operand_plain(&instruction->operands[operand]))
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
        return true;
    }
    const bool uav_read = instruction->opcode == USIL_OP_LD_UAV_TYPED;
    if (instruction->opcode != USIL_OP_LD && !uav_read && instruction->opcode != USIL_OP_STORE_UAV_TYPED) {
        if (instruction->opcode != USIL_OP_MOV && !uint_operation(instruction->opcode) &&
            !(float_data && instruction->opcode == USIL_OP_ADD) && instruction->opcode != USIL_OP_RET)
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_OPCODE);
        if (instruction->opcode == USIL_OP_ADD &&
            (!usil_instruction_shape_valid(ctx->program, instruction) ||
             usil_operand_destination_lane_mask(&instruction->operands[0]) != 15u))
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        return instruction_valid(ctx, index, false, float_data);
    }
    if (!usil_instruction_shape_valid(ctx->program, instruction) || instruction->operand_count != 3 ||
        instruction->saturate || instruction->precise_mask || instruction->has_texel_offset ||
        instruction->condition_test != DXBC_INSTRUCTION_TEST_NONE || instruction->sync_flags ||
        instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE || instruction->geometry_stream_id ||
        instruction->geometry_stream_explicit || instruction->resource_stride ||
        instruction->resource_info_return_type || instruction->sample_info_return_type ||
        !memchr(instruction->resource_dimension, 0, sizeof(instruction->resource_dimension)) ||
        (instruction->resource_dimension[0] && strcmp(instruction->resource_dimension, "2d")))
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (unsigned axis = 0; axis < 3; ++axis)
        if (instruction->texel_offsets[axis])
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    for (unsigned component = 0; component < 4; ++component)
        if (instruction->resource_return_types[component] !=
            (instruction->has_resource_return_types ? (float_data ? 5u : 4u) : 0u))
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    USILEffectFlags effects;
    if (!usil_instruction_effects(ctx->program, instruction, &effects) ||
        effects != (instruction->opcode == USIL_OP_LD || uav_read ? USIL_EFFECT_RESOURCE_READ : USIL_EFFECT_EXTERNAL_WRITE))
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE);
    if (instruction->opcode == USIL_OP_STORE_UAV_TYPED) {
        USILMemoryAccess memory;
        if (!usil_instruction_memory_access(ctx->program, instruction, &memory) ||
            memory.kind != USIL_MEMORY_TYPED || memory.space != USIL_MEMORY_UNORDERED_ACCESS ||
            memory.address_lanes != 3u || memory.value_lanes != 15u || memory.atomic || memory.reads ||
            !memory.writes || memory.globally_coherent || memory.rasterizer_ordered ||
            memory.has_order_preserving_counter)
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    } else {
        const DXBCOperand *destination = &instruction->operands[0];
        const DXBCOperand *resource = &instruction->operands[2];
        const uint8_t destination_lanes = usil_operand_destination_lane_mask(destination);
        if (destination->type != OPERAND_TYPE_TEMP || !static_indices(destination, 1) ||
            !destination_lanes || ((!uav_read || float_data) && destination_lanes != 15u) || destination->swizzle_mode ||
            destination->has_abs || destination->has_neg || destination->min_precision ||
            destination->rel_op0 || destination->rel_op1 || destination->rel_op2 ||
            destination->extended_token_count || destination->extended_tokens ||
            !operand_plain(resource) || resource->type != (uav_read ? OPERAND_TYPE_UAV : OPERAND_TYPE_RESOURCE) ||
            !static_indices(resource, 1) || resource->swizzle_mode != 1u)
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        for (unsigned lane = 0; lane < 4; ++lane)
            if (usil_operand_source_component(resource, (int)lane) != (int)lane)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        if (uav_read) {
            USILMemoryAccess memory;
            if (!usil_instruction_memory_access(ctx->program, instruction, &memory) ||
                memory.kind != USIL_MEMORY_TYPED || memory.space != USIL_MEMORY_UNORDERED_ACCESS ||
                memory.address_lanes != 3u || memory.destination_lanes != destination_lanes ||
                memory.memory_component_lanes != destination_lanes || !memory.reads || memory.writes ||
                memory.atomic || memory.globally_coherent || memory.rasterizer_ordered ||
                memory.has_order_preserving_counter)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        }
    }
    const DXBCOperand *address = &instruction->operands[1];
    if (!operand_plain(address))
        return reject_instruction(ctx, index, HLSL_EMIT_REASON_INVALID_OPERAND);
    return true;
}

static ASTExpr *memory_origin(ComputeMemoryPlan *plan, ASTExpr *expression, int owner,
                              uint8_t lanes, ASTScalarType scalar, unsigned width) {
    if (!expression) return NULL;
    ASTLogicalValueOrigin origin;
    ast_logical_value_origin_init(&origin);
    origin.complete = true;
    origin.scalar_type = scalar;
    origin.components = (uint8_t)width;
    origin.logical_value_id = (uint64_t)owner;
    origin.instruction_index = owner;
    origin.source_instruction_index = plan->ctx->program->instructions[owner].source_instruction_index;
    origin.destination_lanes = lanes;
    if (!ast_set_logical_value_origin(expression, &origin)) {
        ast_free_expr(expression);
        return NULL;
    }
    return expression;
}

static ASTExpr *memory_operand(ComputeMemoryPlan *plan, int consumer, int operand_index,
                               uint8_t lanes, ASTScalarType scalar, unsigned depth);

static bool memory_literal_lane(ComputeMemoryPlan *plan, int consumer, int operand_index,
    int lane, unsigned depth, uint32_t *bits, int *literal_owner, uint8_t *literal_lanes) {
    if (depth > COMPUTE_SOURCE_INSTRUCTION_LIMIT) return false;
    const DXBCOperand *operand = &plan->ctx->program->instructions[consumer].operands[operand_index];
    if (!operand_plain(operand)) return false;
    int selected = usil_operand_source_component(operand, lane);
    if (operand->type == OPERAND_TYPE_IMMEDIATE32) {
        if (operand->imm_value_count == 1) selected = 0;
        if (selected < 0 || selected >= operand->imm_value_count ||
            operand->imm_values[selected] != operand->immediate_words[selected]) return false;
        *bits = operand->imm_values[selected];
        *literal_owner = consumer; *literal_lanes = (uint8_t)(1u << lane);
        return true;
    }
    if (operand->type != OPERAND_TYPE_TEMP || selected < 0) return false;
    int definition = hlsl_operand_definition(plan->ctx, consumer, operand_index, lane);
    if (definition < 0 || definition >= consumer ||
        plan->ctx->program->instructions[definition].opcode != USIL_OP_MOV) return false;
    plan->consumed[definition] |= (uint8_t)(1u << selected);
    return memory_literal_lane(plan, definition, 1, selected, depth + 1,
        bits, literal_owner, literal_lanes);
}

static bool memory_zero_lane(ComputeMemoryPlan *plan, int consumer, int operand_index,
    int lane, unsigned depth, int *literal_owner, uint8_t *literal_lanes) {
    uint32_t bits;
    return memory_literal_lane(plan, consumer, operand_index, lane, depth,
        &bits, literal_owner, literal_lanes) && !bits;
}

/* Both bits-view structured reads and typed UINT4 reads select components of
 * one four-word value. Preserve their actual ascending selection and width;
 * a result-register mask is never a new resource element type. */
static bool memory_load_projection(const DXBCOperand *resource, uint8_t lanes,
                                   unsigned word_offset, int projection[4], bool *project) {
    unsigned component = 0;
    int previous = -1;
    for (int lane = 0; lane < 4; ++lane) if (lanes & (1u << lane)) {
        const int selected = usil_operand_source_component(resource, lane);
        if (selected < 0 || selected > 3) return false;
        const unsigned physical = word_offset + (unsigned)selected;
        if (physical >= 4u || (int)physical <= previous) return false;
        projection[component] = (int)physical;
        *project |= physical != component;
        previous = (int)physical;
        ++component;
    }
    *project |= component != 4u;
    return component != 0;
}

static ASTExpr *memory_definition(ComputeMemoryPlan *plan, int definition, uint8_t lanes,
                                   ASTScalarType scalar, unsigned depth) {
    if (depth > COMPUTE_SOURCE_INSTRUCTION_LIMIT || ++plan->expanded_nodes > 512u) return NULL;
    const USILInstruction *instruction = &plan->ctx->program->instructions[definition];
    const unsigned width = component_count(lanes);
    plan->consumed[definition] |= lanes;
    if (instruction->opcode == USIL_OP_MOV)
        return memory_operand(plan, definition, 1, lanes, scalar, depth + 1);
    if (uint_operation(instruction->opcode) || instruction->opcode == USIL_OP_ADD) {
        if (width < 1u || width > 4u ||
            (uint_operation(instruction->opcode) ? scalar != AST_SCALAR_UINT32 :
                (scalar != AST_SCALAR_FLOAT32 || lanes != 15u))) return NULL;
        ASTExpr *left = memory_operand(plan, definition, 1, lanes, scalar, depth + 1);
        const bool unary = instruction->opcode == USIL_OP_NOT;
        ASTExpr *right = unary ? NULL : memory_operand(plan, definition, 2, lanes, scalar, depth + 1);
        ASTExpr *expression = NULL;
        if (left && (unary || right))
            expression = unary ? ast_create_unary(instruction->opcode, left)
                               : ast_create_binary(instruction->opcode, left, right);
        if (!expression) { ast_free_expr(left); ast_free_expr(right); return NULL; }
        return memory_origin(plan, expression, definition, lanes, scalar, width);
    }
    if ((instruction->opcode != USIL_OP_LD && instruction->opcode != USIL_OP_LD_STRUCTURED &&
         instruction->opcode != USIL_OP_LD_UAV_TYPED) ||
        lanes != usil_operand_destination_lane_mask(&instruction->operands[0]) ||
        definition != plan->load) return NULL;
    /* Direct SSA use counts do not expose duplication through a shared pure
     * intermediate. Count the actual expanded effect node independently. */
    if (++plan->load_expansions != 1u) return NULL;
    for (unsigned component = 0; component < 4; ++component)
        if ((lanes & (1u << component)) &&
            hlsl_definition_use_count(plan->ctx, definition, (int)component) != 1u) return NULL;
    /* LOD is a proven zero source lane. The fourth transport lane is unused by
     * this 2D load; SSA liveness below accounts for its absence explicitly. */
    int literal_owner; uint8_t literal_lanes;
    ASTExpr *location = NULL;
    int binding_operand = 2;
    int projection[4] = {0, 1, 2, 3};
    bool project = false;
    if (instruction->opcode == USIL_OP_LD_STRUCTURED) {
        USILMemoryAccess memory;
        uint32_t byte_offset;
        if (!usil_instruction_memory_access(plan->ctx->program, instruction, &memory) ||
            !memory_literal_lane(plan, definition, memory.byte_offset_operand, 0, depth + 1,
                &byte_offset, &literal_owner, &literal_lanes) ||
            byte_offset >= 16u || (byte_offset & 3u)) return NULL;
        const DXBCOperand *resource = &instruction->operands[memory.binding_operand];
        if (!memory_load_projection(resource, lanes, byte_offset / 4u, projection, &project)) return NULL;
        location = memory_operand(plan, definition, memory.address_operand, memory.address_lanes, AST_SCALAR_UINT32, depth + 1);
        binding_operand = memory.binding_operand;
    } else if (instruction->opcode == USIL_OP_LD_UAV_TYPED) {
        USILMemoryAccess memory;
        if (!usil_instruction_memory_access(plan->ctx->program, instruction, &memory) ||
            !memory_load_projection(&instruction->operands[memory.binding_operand],
                lanes, 0u, projection, &project)) return NULL;
        ASTExpr *coordinates = memory_operand(plan, definition, memory.address_operand, memory.address_lanes, AST_SCALAR_UINT32, depth + 1);
        ASTExpr *arguments[] = {coordinates};
        location = coordinates ? ast_create_call("int2", arguments, 1) : NULL;
        if (!location) { ast_free_expr(coordinates); return NULL; }
        location = memory_origin(plan, location, definition, memory.address_lanes, AST_SCALAR_SINT32, 2);
        binding_operand = memory.binding_operand;
    } else {
        if (!memory_zero_lane(plan, definition, 1, 2, depth + 1, &literal_owner, &literal_lanes)) return NULL;
        ASTExpr *coordinates = memory_operand(plan, definition, 1, 3u, AST_SCALAR_UINT32, depth + 1);
        const uint32_t zero = 0u;
        ASTExpr *lod = memory_origin(plan, ast_create_literal_bits(&zero, 1, AST_SCALAR_SINT32),
                                     literal_owner, literal_lanes, AST_SCALAR_SINT32, 1);
        ASTExpr *arguments[2] = {coordinates, lod};
        location = coordinates && lod ? ast_create_call("int3", arguments, 2) : NULL;
        if (!location) { ast_free_expr(coordinates); ast_free_expr(lod); return NULL; }
        location = memory_origin(plan, location, definition, 7u, AST_SCALAR_SINT32, 3);
    }
    if (!location) return NULL;
    const DXBCOperand *resource = &instruction->operands[binding_operand];
    const HLSLComputeTypedResource *binding = typed_binding(plan->typed,
        instruction->opcode == USIL_OP_LD_UAV_TYPED, (uint32_t)resource->register_index);
    if (!binding || binding->scalar_type != scalar) { ast_free_expr(location); return NULL; }
    char method[264];
    int size = snprintf(method, sizeof(method), "%s.Load", binding->name);
    if (size < 0 || (size_t)size >= sizeof(method)) { ast_free_expr(location); return NULL; }
    ASTExpr *load_arguments[] = {location};
    ASTExpr *load = ast_create_call(method, load_arguments, 1);
    if (!load) { ast_free_expr(location); return NULL; }
    /* The declared value has four words. A typed read retains its scalar type;
     * a structured bits view grants no unavailable original element type.
     * Any following selection retains this single load's instruction owner. */
    load = memory_origin(plan, load, definition, lanes, scalar, 4);
    if (!load || !project) return load;
    ASTExpr *selection = ast_create_swizzle(load, projection, (int)width);
    if (!selection) { ast_free_expr(load); return NULL; }
    selection = memory_origin(plan, selection, definition, lanes, scalar, width);
    if (selection) selection->logical_origin.semantic_projection = true;
    return selection;
}

static ASTExpr *memory_operand(ComputeMemoryPlan *plan, int consumer, int operand_index,
                               uint8_t lanes, ASTScalarType scalar, unsigned depth) {
    if (depth > COMPUTE_SOURCE_INSTRUCTION_LIMIT || ++plan->expanded_nodes > 512u) return NULL;
    const USILInstruction *instruction = &plan->ctx->program->instructions[consumer];
    const DXBCOperand *operand = &instruction->operands[operand_index];
    if (!operand_plain(operand) || !lanes || (lanes & ~15u)) return NULL;
    const unsigned width = component_count(lanes);
    if (operand->type == OPERAND_TYPE_IMMEDIATE32) {
        if (operand->imm_value_count != 1 && operand->imm_value_count != 4) return NULL;
        if (operand->immediate_word_count != operand->imm_value_count) return NULL;
        uint32_t bits[4]; unsigned component = 0;
        for (int lane = 0; lane < 4; ++lane) if (lanes & (1u << lane)) {
            const int selected = operand->imm_value_count == 1 ? 0 : usil_operand_source_component(operand, lane);
            if (selected < 0 || selected >= operand->imm_value_count ||
                operand->imm_values[selected] != operand->immediate_words[selected]) return NULL;
            bits[component++] = operand->imm_values[selected];
        }
        if (width == 4 && bits[0] == bits[1] && bits[1] == bits[2] && bits[2] == bits[3]) return NULL;
        return memory_origin(plan, ast_create_literal_bits(bits, (int)width, scalar),
                             consumer, lanes, scalar, width);
    }
    if (operand->type == OPERAND_TYPE_TEMP) {
        if (!static_indices(operand, 1)) return NULL;
        int definition = -1;
        int definitions[4], selections[4]; unsigned count = 0;
        uint8_t selected_lanes = 0; bool mixed = false;
        for (int lane = 0; lane < 4; ++lane) if (lanes & (1u << lane)) {
            const int current = hlsl_operand_definition(plan->ctx, consumer, operand_index, lane);
            const int selected = usil_operand_source_component(operand, lane);
            if (current < 0 || current >= consumer || selected < 0 || selected > 3 ||
                plan->ctx->program->instructions[current].operands[0].type != OPERAND_TYPE_TEMP ||
                plan->ctx->program->instructions[current].operands[0].register_index != operand->register_index ||
                !(usil_operand_destination_lane_mask(&plan->ctx->program->instructions[current].operands[0]) &
                  (1u << selected))) return NULL;
            mixed |= definition >= 0 && definition != current;
            definitions[count] = current; selections[count++] = selected;
            definition = current; selected_lanes |= (uint8_t)(1u << selected);
        }
        if (!mixed) {
            int previous = -1;
            for (unsigned component = 0; component < count; ++component) {
                if (selections[component] <= previous) return NULL;
                previous = selections[component];
            }
            return memory_definition(plan, definition, selected_lanes, scalar, depth + 1);
        }
        if (scalar != AST_SCALAR_UINT32) return NULL;
        ASTExpr *arguments[4] = {0}; unsigned argument_count = 0;
        /* Compose distinct naturally ordered producers as vector arguments.
         * A resource read cannot recur through a second group or expansion. */
        for (unsigned component = 0; component < count;) {
            const int owner = definitions[component];
            for (unsigned prior = 0; prior < component; ++prior)
                if (definitions[prior] == owner) goto composition_failed;
            uint8_t producer_lanes = (uint8_t)(1u << selections[component]);
            unsigned end = component + 1;
            while (end < count && definitions[end] == owner) {
                if (selections[end] != selections[end - 1] + 1) goto composition_failed;
                producer_lanes |= (uint8_t)(1u << selections[end++]);
            }
            arguments[argument_count] = memory_definition(plan, owner, producer_lanes, scalar, depth + 1);
            if (!arguments[argument_count]) goto composition_failed;
            ++argument_count;
            component = end;
        }
        ASTExpr *composition = ast_create_call(uint_type(width), arguments, (int)argument_count);
        if (!composition) goto composition_failed;
        return memory_origin(plan, composition, consumer, lanes, AST_SCALAR_UINT32, width);
composition_failed:
        for (unsigned component = 0; component < argument_count; ++component) ast_free_expr(arguments[component]);
        return NULL;
    }
    const ComputeBuiltin *builtin = compute_builtin(operand->type);
    if (scalar != AST_SCALAR_UINT32 || !builtin || builtin->flag != USIL_COMPUTE_DISPATCH_THREAD_ID || !static_indices(operand, 0) ||
        (width != 1 && width != 2) || !(plan->ctx->program->compute.system_value_mask & builtin->flag)) return NULL;
    uint8_t selected_components[2] = {0, 0};
    unsigned component = 0;
    for (int lane = 0; lane < 4; ++lane) if (lanes & (1u << lane)) {
        const int selected = usil_operand_source_component(operand, lane);
        if (selected < 0 || selected >= 3 || (width == 2 && selected != (int)component)) return NULL;
        selected_components[component++] = (uint8_t)selected;
    }
    ASTOperandProvenance origin;
    ast_operand_provenance_init(&origin);
    origin.complete = true;
    origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    origin.logical_value_id = UINT64_C(0x100000000) | builtin->flag;
    origin.natural_components = 3;
    origin.result_components = (uint8_t)width;
    origin.selection_role = AST_COMPONENT_SELECTION_SEMANTIC;
    origin.selected_components[0] = selected_components[0];
    origin.selected_components[1] = selected_components[1];
    origin.instruction_index = consumer;
    origin.source_instruction_index = instruction->source_instruction_index;
    origin.operand_index = operand_index;
    origin.destination_lanes = lanes;
    char name[32];
    snprintf(name, sizeof(name), "dispatchThreadId.%s", width == 2 ? "xy" :
             (selected_components[0] == 0 ? "x" : selected_components[0] == 1 ? "y" : "z"));
    return ast_create_emitter_operand_with_provenance(name, &origin);
}

static bool observe_memory_expression(ComputeMemoryPlan *plan, ASTExpr *expression, int owner) {
    HLSLEmitterContext *ctx = plan->ctx;
    if (!expression || !hlsl_source_quality_observe_expression(ctx, expression, owner)) {
        ast_free_expr(expression); return false;
    }
    ast_format_expr(expression, ctx->sb);
    if (!sb_ok(ctx->sb)) { ast_free_expr(expression); return false; }
    if (plan->typed->retain_expression) {
        if (plan->typed->retain_expression(plan->typed->expression_context, expression)) return true;
        ast_free_expr(expression);
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED, HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                       HLSL_EMIT_REASON_ALLOCATION_FAILED);
        return false;
    }
    ast_free_expr(expression);
    return true;
}

static void emit_typed_declarations(HLSLEmitterContext *ctx, const HLSLComputeTypedSource *typed) {
    for (size_t index = 0; index < typed->resource_count; ++index) {
        const HLSLComputeTypedResource *resource = &typed->resources[index];
        const char *type = resource->structured ? (resource->writable ? "RWStructuredBuffer" : "StructuredBuffer")
                                                : (resource->writable ? "RWTexture2D" : "Texture2D");
        const char *element = resource->scalar_atomic ? "uint" :
            resource->scalar_type == AST_SCALAR_FLOAT32 ? "float4" : "uint4";
        if (resource->scalar_atomic)
            sb_appendf(ctx->sb, "%s<%s> %s : register(u%u);\n", type, element,
                       resource->name, resource->binding_register);
        else
            sb_appendf(ctx->sb, "%s<%s> %s;\n", type, element, resource->name);
        HLSLSourceQualityFacts fact;
        hlsl_source_quality_facts_init(&fact);
        fact.known = true;
        fact.resource_declaration_kind = resource->writable ? HLSL_SOURCE_RESOURCE_UAV : HLSL_SOURCE_RESOURCE_TEXTURE;
        fact.resource_binding_register = resource->binding_register;
        if (ctx->source_quality_analysis && !hlsl_source_quality_analysis_emission(ctx->source_quality_analysis, &fact))
            hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT, HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                           HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
    sb_append(ctx->sb, "\n");
}

/* Replicated result lanes are equal only when this same pure UINT32 producer
 * consumes identical values for both lanes. This is one instruction, not an
 * alias search or a comparison of independently computed expressions. */
static bool uint_result_lanes_equal(HLSLEmitterContext *ctx, int definition,
                                    int first, int second) {
    if (definition < 0 || definition >= ctx->program->instruction_count ||
        first < 0 || first >= 4 || second < 0 || second >= 4) return false;
    const USILInstruction *instruction = &ctx->program->instructions[definition];
    USILEffectFlags effects;
    if (!uint_operation(instruction->opcode) ||
        !usil_instruction_effects(ctx->program, instruction, &effects) || effects != USIL_EFFECT_NONE)
        return false;
    const uint8_t mask = usil_operand_destination_lane_mask(&instruction->operands[0]);
    if (!(mask & (1u << first)) || !(mask & (1u << second))) return false;
    for (int operand_index = 1; operand_index < instruction->operand_count; ++operand_index) {
        const DXBCOperand *source = &instruction->operands[operand_index];
        if (!operand_plain(source)) return false;
        const int left = usil_operand_source_component(source, first);
        const int right = usil_operand_source_component(source, second);
        if (source->type == OPERAND_TYPE_IMMEDIATE32) {
            const int a = source->imm_value_count == 1 ? 0 : left;
            const int b = source->imm_value_count == 1 ? 0 : right;
            if (a < 0 || b < 0 || a >= source->immediate_word_count || b >= source->immediate_word_count ||
                source->immediate_words[a] != source->immediate_words[b]) return false;
        } else {
            if (left < 0 || left != right) return false;
            if (source->type == OPERAND_TYPE_TEMP) {
                const int owner = hlsl_operand_definition(ctx, definition, operand_index, first);
                if (owner < 0 || owner != hlsl_operand_definition(ctx, definition, operand_index, second))
                    return false;
            } else if (source->type != OPERAND_TYPE_INPUT_THREAD_ID) return false;
        }
    }
    return true;
}

/* One unchanged physical address producer owns both accesses. Distinct lanes
 * may refer to a proven replicated UINT32 result of that same instruction;
 * the emitted operands retain their original selected-lane provenance. */
static bool same_typed_uav_address(HLSLEmitterContext *ctx, int load, int store) {
    const USILInstruction *read = &ctx->program->instructions[load];
    const USILInstruction *write = &ctx->program->instructions[store];
    USILMemoryAccess read_access, write_access;
    if (!usil_instruction_memory_access(ctx->program, read, &read_access) ||
        !usil_instruction_memory_access(ctx->program, write, &write_access) ||
        read_access.register_id != write_access.register_id ||
        read_access.address_lanes != write_access.address_lanes) return false;
    const DXBCOperand *left = &read->operands[read_access.address_operand];
    const DXBCOperand *right = &write->operands[write_access.address_operand];
    if (left->type != right->type) return false;
    for (int lane = 0; lane < 4; ++lane) {
        if (!(read_access.address_lanes & (1u << lane))) continue;
        const int first = usil_operand_source_component(left, lane);
        const int second = usil_operand_source_component(right, lane);
        if (left->type == OPERAND_TYPE_IMMEDIATE32) {
            const int a = left->imm_value_count == 1 ? 0 : first;
            const int b = right->imm_value_count == 1 ? 0 : second;
            if (a < 0 || b < 0 || a >= left->imm_value_count || b >= right->imm_value_count ||
                left->imm_values[a] != right->imm_values[b]) return false;
        } else {
            if (first < 0 || second < 0) return false;
            if (left->type == OPERAND_TYPE_TEMP) {
                const int owner = hlsl_operand_definition(ctx, load, read_access.address_operand, lane);
                if (left->register_index != right->register_index || owner < 0 ||
                    owner != hlsl_operand_definition(ctx, store, write_access.address_operand, lane)) return false;
                if (first != second && !uint_result_lanes_equal(ctx, owner, first, second)) return false;
            } else if (left->type != OPERAND_TYPE_INPUT_THREAD_ID || first != second) return false;
        }
    }
    return true;
}

static bool emit_typed_memory_body(HLSLEmitterContext *ctx, const HLSLComputeTypedSource *typed) {
    ComputeMemoryPlan plan = {.ctx = ctx, .typed = typed, .store = -1, .load = -1};
    const USILProgram *program = ctx->program;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        if (instruction->opcode == USIL_OP_STORE_UAV_TYPED || instruction->opcode == USIL_OP_STORE_STRUCTURED) {
            if (plan.store >= 0 || index != program->instruction_count - 2)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            plan.store = index;
        } else if (instruction->opcode == USIL_OP_LD || instruction->opcode == USIL_OP_LD_STRUCTURED ||
                   instruction->opcode == USIL_OP_LD_UAV_TYPED) {
            if (plan.load >= 0) return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
            plan.load = index;
        }
    }
    const bool uav_read = plan.load >= 0 && program->instructions[plan.load].opcode == USIL_OP_LD_UAV_TYPED;
    if (plan.store < 0 || (program->texture_count != (plan.load >= 0 && !uav_read))) return reject_stage(ctx);
    if (uav_read && (program->texture_count || typed->resource_count != 1u || plan.load >= plan.store ||
        program->instructions[plan.store].opcode != USIL_OP_STORE_UAV_TYPED ||
        !same_typed_uav_address(ctx, plan.load, plan.store))) return reject_stage(ctx);
    if (typed->resources[0].scalar_type == AST_SCALAR_FLOAT32) {
        unsigned additions = 0;
        for (int index = 0; index < program->instruction_count; ++index)
            additions += program->instructions[index].opcode == USIL_OP_ADD;
        if (!uav_read || additions != 1u) return reject_stage(ctx);
    }
    if (plan.load >= 0) {
        for (int index = plan.load + 1; index < plan.store; ++index) {
            USILEffectFlags effects;
            if (!usil_instruction_effects(program, &program->instructions[index], &effects) || effects != USIL_EFFECT_NONE)
                return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        }
    }
    const USILInstruction *store = &program->instructions[plan.store];
    const HLSLComputeTypedResource *destination = typed_binding(typed, true,
        (uint32_t)store->operands[0].register_index);
    if (!destination) return reject_stage(ctx);
    ASTOperandProvenance resource_origin;
    ast_operand_provenance_init(&resource_origin);
    resource_origin.complete = true;
    resource_origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    resource_origin.logical_value_id = UINT64_C(0x200000000) | destination->binding_register;
    resource_origin.instruction_index = plan.store;
    resource_origin.source_instruction_index = store->source_instruction_index;
    resource_origin.operand_index = 0;
    ASTExpr *resource = ast_create_emitter_operand_with_provenance(destination->name, &resource_origin);
    int address_operand = 1, value_operand = 2;
    uint8_t address_lanes = 3u;
    if (store->opcode == USIL_OP_STORE_STRUCTURED) {
        USILMemoryAccess memory;
        int literal_owner; uint8_t literal_lanes;
        if (!usil_instruction_memory_access(program, store, &memory) ||
            !memory_zero_lane(&plan, plan.store, memory.byte_offset_operand, 0, 0, &literal_owner, &literal_lanes)) {
            ast_free_expr(resource);
            return reject_instruction(ctx, plan.store, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        }
        address_operand = memory.address_operand;
        value_operand = memory.value_operand;
        address_lanes = memory.address_lanes;
    }
    ASTExpr *coordinates = memory_operand(&plan, plan.store, address_operand, address_lanes, AST_SCALAR_UINT32, 0);
    ASTExpr *value = memory_operand(&plan, plan.store, value_operand, 15u, destination->scalar_type, 0);
    if (!resource || !coordinates || !value) {
        ast_free_expr(resource); ast_free_expr(coordinates); ast_free_expr(value);
        return reject_instruction(ctx, plan.store, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    }
    /* A retained resource read is an effect even when SSA liveness finds no
     * consumer. It cannot disappear behind a later pure redefinition. */
    if (plan.load >= 0 &&
        (plan.consumed[plan.load] != usil_operand_destination_lane_mask(&program->instructions[plan.load].operands[0]) ||
         plan.load_expansions != 1u)) {
        ast_free_expr(resource); ast_free_expr(coordinates); ast_free_expr(value);
        return reject_instruction(ctx, plan.load, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    }
    for (int index = 0; index < plan.store; ++index) {
        if (program->instructions[index].opcode == USIL_OP_ADD && plan.consumed[index] != 15u) {
            ast_free_expr(resource); ast_free_expr(coordinates); ast_free_expr(value);
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        }
        const uint8_t written = usil_operand_destination_lane_mask(&program->instructions[index].operands[0]);
        for (int lane = 0; lane < 4; ++lane) if ((written & (1u << lane)) && !(plan.consumed[index] & (1u << lane)) &&
            hlsl_definition_use_count(ctx, index, lane) != 0u) {
            ast_free_expr(resource); ast_free_expr(coordinates); ast_free_expr(value);
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        }
    }
    sb_append(ctx->sb, "    ");
    if (!observe_memory_expression(&plan, resource, plan.store)) {
        ast_free_expr(coordinates); ast_free_expr(value); return false;
    }
    sb_append(ctx->sb, "[");
    if (!observe_memory_expression(&plan, coordinates, plan.store)) { ast_free_expr(value); return false; }
    sb_append(ctx->sb, "] = ");
    if (!observe_memory_expression(&plan, value, plan.store)) return false;
    sb_append(ctx->sb, ";\n");
    hlsl_source_quality_emission(ctx, 0, true, plan.store);
    sb_append(ctx->sb, "    return;\n");
    hlsl_source_quality_emission(ctx, 0, true, program->instruction_count - 1);
    return sb_ok(ctx->sb);
}

static bool atomic_return_analysis_prepare(HLSLEmitterContext *ctx) {
    if (!atomic_returns_value(ctx->program)) return true;
    if (!build_control_flow_graph(ctx) || !compute_dominance(&ctx->cfg) ||
        !build_hlsl_ssa_graph(ctx) || !build_hlsl_use_def_graph(ctx)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_SSA_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        return false;
    }
    if (ctx->cfg.block_count < 1 || ctx->cfg.block_count > 3 || !ctx->cfg.instruction_block ||
        ctx->ssa.instruction_count != 3 || ctx->ssa.ssa_var_count != 1 ||
        !ctx->ssa.operand_ssa_vars || !ctx->ssa.ssa_var_defs || !ctx->ssa.block_phis ||
        ctx->ssa.ssa_var_defs[0] != 0 || ctx->ssa.operand_ssa_vars[0] != 0 ||
        !hlsl_cfg_dominates(&ctx->cfg, ctx->cfg.instruction_block[0], ctx->cfg.instruction_block[1]) ||
        !instructions_have_unambiguous_path(ctx, 0, 1) || hlsl_definition_use_count(ctx, 0, 0) != 4u)
        return reject_instruction(ctx, 0, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    for (int block = 0; block < ctx->cfg.block_count; ++block)
        if (ctx->ssa.block_phis[block].phi_count)
            return reject_instruction(ctx, 0, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    /* STORE demands four transport lanes, all of which must consume this
     * single actual X definition. They do not make the result a uint4. */
    for (int lane = 0; lane < 4; ++lane) {
        if (hlsl_operand_definition(ctx, 1, 2, lane) != 0 ||
            usil_operand_source_component(&ctx->program->instructions[1].operands[2], lane) != 0 ||
            (lane && hlsl_definition_use_count(ctx, 0, lane)))
            return reject_instruction(ctx, 1, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    }
    return true;
}

static ASTExpr *atomic_resource_expression(ComputeMemoryPlan *plan, int owner, int operand_index) {
    const USILInstruction *instruction = &plan->ctx->program->instructions[owner];
    ASTOperandProvenance origin;
    ast_operand_provenance_init(&origin);
    origin.complete = true;
    origin.value_role = AST_OPERAND_VALUE_LOGICAL;
    origin.logical_value_id = UINT64_C(0x200000000) | plan->typed->resources[0].binding_register;
    origin.instruction_index = owner;
    origin.source_instruction_index = instruction->source_instruction_index;
    origin.operand_index = operand_index;
    return ast_create_emitter_operand_with_provenance(plan->typed->resources[0].name, &origin);
}

static ASTExpr *atomic_coordinate_expression(ComputeMemoryPlan *plan, int owner, int operand_index) {
    const USILInstruction *instruction = &plan->ctx->program->instructions[owner];
    ASTExpr *coordinates = NULL;
    if (instruction->operands[operand_index].type == OPERAND_TYPE_INPUT_THREAD_ID) {
        ASTExpr *dispatch = memory_operand(plan, owner, operand_index, 3u, AST_SCALAR_UINT32, 0);
        ASTExpr *arguments[] = {dispatch};
        coordinates = dispatch ? ast_create_call("int2", arguments, 1) : NULL;
        if (!coordinates) ast_free_expr(dispatch);
        coordinates = memory_origin(plan, coordinates, owner, 3u, AST_SCALAR_SINT32, 2);
    } else {
        coordinates = memory_operand(plan, owner, operand_index, 3u, AST_SCALAR_SINT32, 0);
    }
    return coordinates;
}

static ASTExpr *atomic_result_expression(ComputeMemoryPlan *plan) {
    const int variable = plan->ctx->ssa.operand_ssa_vars[0];
    return memory_origin(plan, ast_create_var(variable, 0, OPERAND_TYPE_TEMP, "atomicResult"),
                         0, 1u, AST_SCALAR_UINT32, 1);
}

static bool emit_returning_atomic_body(HLSLEmitterContext *ctx, const HLSLComputeTypedSource *typed) {
    ComputeMemoryPlan plan = {.ctx = ctx, .typed = typed, .load = -1, .store = 1};
    ASTExpr *roots[7] = {
        atomic_resource_expression(&plan, 0, 1), atomic_coordinate_expression(&plan, 0, 2),
        memory_operand(&plan, 0, 3, 1u, AST_SCALAR_UINT32, 0), atomic_result_expression(&plan),
        atomic_resource_expression(&plan, 1, 0), atomic_coordinate_expression(&plan, 1, 1),
        atomic_result_expression(&plan)
    };
    bool emitted = false;
    for (size_t index = 0; index < 7u; ++index)
        if (!roots[index]) { reject_instruction(ctx, 0, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT); goto cleanup; }
    sb_append(ctx->sb, "    uint atomicResult;\n");
    hlsl_source_quality_emission(ctx, 0, false, 0);
    sb_append(ctx->sb, "    InterlockedAdd(");
    static const char *const suffixes[7] = {"[", "], ", ", ", ");\n", "[", "] = ", ";\n"};
    for (size_t index = 0; index < 7u; ++index) {
        ASTExpr *root = roots[index];
        roots[index] = NULL;
        if (!sb_ok(ctx->sb)) { ast_free_expr(root); goto cleanup; }
        if (!observe_memory_expression(&plan, root, index < 4u ? 0 : 1)) goto cleanup;
        sb_append(ctx->sb, suffixes[index]);
        if (index == 3u) {
            /* The intrinsic is an ordered void effect with an out variable,
             * never a numeric CALL expression that could be duplicated. */
            hlsl_source_quality_emission(ctx, 0, true, 0);
            sb_append(ctx->sb, "    ");
        } else if (index == 6u) {
            hlsl_source_quality_emission(ctx, 0, true, 1);
        }
    }
    sb_append(ctx->sb, "    return;\n");
    hlsl_source_quality_emission(ctx, 0, true, 2);
    emitted = sb_ok(ctx->sb);
cleanup:
    for (size_t index = 0; index < 7u; ++index) ast_free_expr(roots[index]);
    return emitted;
}

static bool emit_typed_atomic_body(HLSLEmitterContext *ctx, const HLSLComputeTypedSource *typed) {
    if (atomic_returns_value(ctx->program)) return emit_returning_atomic_body(ctx, typed);
    ComputeMemoryPlan plan = {.ctx = ctx, .typed = typed, .store = 0, .load = -1};
    ASTExpr *resource = atomic_resource_expression(&plan, 0, 0);
    ASTExpr *coordinates = atomic_coordinate_expression(&plan, 0, 1);
    ASTExpr *value = memory_operand(&plan, 0, 2, 1u, AST_SCALAR_UINT32, 0);
    if (!resource || !coordinates || !value) {
        ast_free_expr(resource); ast_free_expr(coordinates); ast_free_expr(value);
        return reject_instruction(ctx, 0, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    }
    /* A void atomic effect owns three actual operand roots. It is never a
     * numeric-result call masquerading as a destination definition. */
    sb_append(ctx->sb, "    InterlockedAdd(");
    if (!observe_memory_expression(&plan, resource, 0)) {
        ast_free_expr(coordinates); ast_free_expr(value); return false;
    }
    sb_append(ctx->sb, "[");
    if (!observe_memory_expression(&plan, coordinates, 0)) { ast_free_expr(value); return false; }
    sb_append(ctx->sb, "], ");
    if (!observe_memory_expression(&plan, value, 0)) return false;
    sb_append(ctx->sb, ");\n");
    hlsl_source_quality_emission(ctx, 0, true, 0);
    sb_append(ctx->sb, "    return;\n");
    hlsl_source_quality_emission(ctx, 0, true, 1);
    return sb_ok(ctx->sb);
}

static void emit_shared_memory(HLSLEmitterContext *ctx) {
    for (size_t index = 0; index < ctx->program->compute.shared_memory_count; ++index) {
        const DXBCThreadGroupSharedMemoryContract *memory = &ctx->program->compute.shared_memory[index];
        if (memory->structured) {
            sb_appendf(ctx->sb, "struct SharedMemoryElement_%u\n{\n    uint words[%u];\n};\n"
                       "groupshared SharedMemoryElement_%u sharedMemory_%u[%u];\n\n",
                       memory->register_id, memory->byte_stride / 4u, memory->register_id,
                       memory->register_id, memory->element_count);
        } else {
            sb_appendf(ctx->sb, "groupshared uint sharedMemory_%u[%u];\n\n",
                       memory->register_id, memory->byte_count / 4u);
        }
        /* DXBC retains byte layout, not the original element scalar types. */
        hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_RAW_BUFFER_RECONSTRUCTION, false, -1);
    }
}

static bool emit_source(HLSLEmitterContext *ctx, const char *entry_point,
                        const HLSLComputeTypedSource *typed) {
    const USILProgram *program = ctx->program;
    emit_shared_memory(ctx);
    if (typed && typed->emit_declarations) emit_typed_declarations(ctx, typed);
    sb_appendf(ctx->sb, "[numthreads(%u, %u, %u)]\nvoid %s(",
               program->compute.thread_group_size[0], program->compute.thread_group_size[1],
               program->compute.thread_group_size[2], entry_point);
    hlsl_source_quality_emission(ctx, 0, false, -1);
    bool first = true;
    for (size_t index = 0; index < sizeof(compute_builtins) / sizeof(compute_builtins[0]); ++index) {
        const ComputeBuiltin *builtin = &compute_builtins[index];
        if (!(program->compute.system_value_mask & builtin->flag)) continue;
        sb_appendf(ctx->sb, "%s%s %s : %s", first ? "" : ", ", uint_type(builtin->width),
                   builtin->identifier, builtin->semantic);
        hlsl_source_quality_emission(ctx, 0, false, -1);
        first = false;
    }
    sb_append(ctx->sb, ")\n{\n");
    hlsl_source_quality_emission(ctx, 0, false, -1);
    if (typed) {
        if (!(typed->resources[0].scalar_atomic ? emit_typed_atomic_body(ctx, typed)
                                               : emit_typed_memory_body(ctx, typed))) return false;
        sb_append(ctx->sb, "}\n");
        hlsl_source_quality_emission(ctx, 0, false, -1);
        return sb_ok(ctx->sb);
    }
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *instruction = &program->instructions[index];
        if (instruction->opcode == USIL_OP_SYNC) {
            sb_appendf(ctx->sb, "    %s();\n", barrier_intrinsic(instruction->sync_flags));
            /* A direct intrinsic effect, owned by this retained SYNC. The
             * validated route has no divergent branch, early RET or loop. */
            hlsl_source_quality_emission(ctx, 0, true, index);
            continue;
        }
        if (instruction->opcode == USIL_OP_RET) {
            sb_append(ctx->sb, "    return;\n");
            hlsl_source_quality_emission(ctx, 0, true, index);
            continue;
        }
        ASTExpr *left = source_expression(ctx, index, 1);
        ASTExpr *right = instruction->operand_count == 3 ? source_expression(ctx, index, 2) : NULL;
        ASTExpr *expression = NULL;
        if (left && (instruction->operand_count == 2 || right)) {
            if (instruction->opcode == USIL_OP_MOV) {
                expression = left;
            } else if (instruction->opcode == USIL_OP_NOT) {
                expression = ast_create_unary(instruction->opcode, left);
            } else {
                expression = ast_create_binary(instruction->opcode, left, right);
            }
        }
        if (!expression) {
            ast_free_expr(left);
            ast_free_expr(right);
            return reject_instruction(ctx, index, HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
        }
        const uint8_t lanes = usil_operand_destination_lane_mask(&instruction->operands[0]);
        /* MOV retains its source's logical identity. The assignment event
         * below owns the copy instruction without impersonating its producer. */
        if (instruction->opcode != USIL_OP_MOV &&
            expression->kind != AST_EXPR_EMITTER_OPERAND && expression->kind != AST_EXPR_LITERAL)
            expression = logical_value(ctx, expression, index, lanes);
        if (!expression) {
            hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED, HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                           HLSL_EMIT_REASON_ALLOCATION_FAILED);
            return false;
        }
        const bool observed = hlsl_source_quality_observe_expression(ctx, expression, index);
        if (observed) {
            sb_appendf(ctx->sb, "    %s value_%d = ", uint_type(component_count(lanes)), index);
            ast_format_expr(expression, ctx->sb);
            sb_append(ctx->sb, ";\n");
            hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_INSTRUCTION_ASSIGNMENT, true, index);
        }
        ast_free_expr(expression);
        if (!observed || !sb_ok(ctx->sb)) return false;
    }
    sb_append(ctx->sb, "}\n");
    hlsl_source_quality_emission(ctx, 0, false, -1);
    return sb_ok(ctx->sb);
}

enum { COMPUTE_ATOMIC_NAME_LIMIT = 255, COMPUTE_ATOMIC_KEYWORD_LIMIT = 8,
       COMPUTE_ATOMIC_SOURCE_LIMIT = 4096 };

typedef struct {
    const USILProgram *program;
    USILProgram program_owner, owned_program;
    USILInstruction instructions[3];
    USILUav uav;
    USILSignatureDeclaration declaration;
    const HLSLComputeTypedSource *typed;
    HLSLComputeTypedSource typed_owner, owned_typed;
    HLSLComputeTypedResource resource_owner, owned_resource;
    const HLSLEmitOptions *options;
    HLSLEmitOptions options_owner, owned_options;
    const HLSLEmitNames *names;
    HLSLEmitNames names_owner;
    const char *entry_owner;
    size_t entry_owner_length, resource_length;
    char entry[COMPUTE_ATOMIC_NAME_LIMIT + 1], resource[COMPUTE_ATOMIC_NAME_LIMIT + 1];
    const char *keyword_owners[COMPUTE_ATOMIC_KEYWORD_LIMIT];
    const char *owned_keywords[COMPUTE_ATOMIC_KEYWORD_LIMIT];
    char keywords[COMPUTE_ATOMIC_KEYWORD_LIMIT][COMPUTE_ATOMIC_NAME_LIMIT + 1];
    size_t keyword_lengths[COMPUTE_ATOMIC_KEYWORD_LIMIT];
    StringBuilder expected, prefix;
    StringBuilder *output;
    StringBuilder output_owner;
    HLSLEmitterContext *ctx;
    bool published, ready, rejected;
} ComputeAtomicGuard;

static bool atomic_copy_identifier(char destination[COMPUTE_ATOMIC_NAME_LIMIT + 1],
                                    const char *source, size_t *length) {
    if (!hlsl_source_identifier_valid(source)) return false;
    *length = strlen(source);
    memcpy(destination, source, *length + 1u);
    return true;
}

static bool atomic_program_top_supported(const USILProgram *program) {
    return program->has_stage_contract && program->has_parsed_signature_authority &&
        program->program_type == DXBC_PROGRAM_TYPE_COMPUTE && program->shader_model_major == 5u &&
        program->shader_model_minor == 0u && program->compute.valid &&
        (program->instruction_count == 2 || program->instruction_count == 3) &&
        program->instruction_alloc >= program->instruction_count && program->instructions &&
        program->uav_count == 1 && program->uav_alloc >= 1 && program->uavs &&
        program->temp_count == (program->instruction_count == 3 ? 1 : 0) &&
        !program->input_count && !program->output_count &&
        !program->patch_constant_count && program->signature_declaration_count >= 0 &&
        program->signature_declaration_count <= 1 &&
        (program->instruction_count != 3 || program->signature_declaration_count == 1) &&
        program->signature_declaration_alloc >= program->signature_declaration_count &&
        ((program->signature_declaration_alloc == 0) == (program->signature_declarations == NULL)) &&
        !program->cbuffer_count && !program->texture_count && !program->sampler_count &&
        !program->indexable_temp_count && !program->index_range_count && !program->icb_value_count &&
        !program->has_icb_declaration && !program->icb_declaration_owner &&
        !program->geometry.valid && !program->tessellation.valid &&
        program->compute.system_value_mask == (program->signature_declaration_count
            ? (unsigned)USIL_COMPUTE_DISPATCH_THREAD_ID : 0u) && !program->compute.shared_memory_count &&
        !program->compute.shared_memory_capacity && !program->compute.shared_memory &&
        !program->compute.shared_memory_bytes && !program->compute.barrier_count;
}

static bool atomic_guard_model_matches(const ComputeAtomicGuard *guard) {
    /* Top objects retain the original pointer/count lease. Compare them first,
     * before any borrowed array or name can be followed after a callback. */
    if (memcmp(guard->program, &guard->program_owner, sizeof(guard->program_owner)) ||
        memcmp(guard->typed, &guard->typed_owner, sizeof(guard->typed_owner)) ||
        memcmp(guard->options, &guard->options_owner, sizeof(guard->options_owner)) ||
        (guard->names && memcmp(guard->names, &guard->names_owner, sizeof(guard->names_owner)))) return false;
    if (memcmp(guard->program_owner.instructions, guard->instructions,
            (size_t)guard->program_owner.instruction_count * sizeof(*guard->instructions)) ||
        memcmp(guard->program_owner.uavs, &guard->uav, sizeof(guard->uav)) ||
        (guard->program_owner.signature_declaration_count &&
         memcmp(guard->program_owner.signature_declarations, &guard->declaration, sizeof(guard->declaration))) ||
        memcmp(guard->typed_owner.resources, &guard->resource_owner, sizeof(guard->resource_owner)) ||
        memcmp(guard->resource_owner.name, guard->resource, guard->resource_length + 1u) ||
        (guard->entry_owner && memcmp(guard->entry_owner,
            guard->entry_owner_length ? guard->entry : "", guard->entry_owner_length + 1u))) return false;
    const size_t count = guard->options_owner.reserved_preprocessor_identifier_count;
    if (count && memcmp(guard->options_owner.reserved_preprocessor_identifiers,
            guard->keyword_owners, count * sizeof(*guard->keyword_owners))) return false;
    for (size_t index = 0; index < count; ++index)
        if (memcmp(guard->keyword_owners[index], guard->keywords[index], guard->keyword_lengths[index] + 1u))
            return false;
    return true;
}

static bool atomic_builder_valid(const StringBuilder *builder) {
    return sb_ok(builder) && (builder->buf
        ? builder->len < builder->capacity && builder->buf[builder->len] == '\0'
        : !builder->len && !builder->capacity);
}

static bool atomic_guard_source_matches(const ComputeAtomicGuard *guard, bool complete) {
    const StringBuilder *source = guard->ctx->sb;
    if (!guard->ready || !atomic_builder_valid(source) || source->len > guard->expected.len ||
        (complete && source->len != guard->expected.len) ||
        (source->len && memcmp(source->buf, guard->expected.buf, source->len)) ||
        memcmp(guard->output, &guard->output_owner, sizeof(guard->output_owner))) return false;
    const StringBuilder *output = guard->output;
    const size_t prefix_length = guard->prefix.len;
    const size_t suffix_length = guard->published ? guard->expected.len : 0u;
    return atomic_builder_valid(output) && output->len >= prefix_length &&
        output->len - prefix_length == suffix_length &&
        (!prefix_length || memcmp(output->buf, guard->prefix.buf, prefix_length) == 0) &&
        (!suffix_length || memcmp(output->buf + prefix_length, guard->expected.buf, suffix_length) == 0);
}

static bool atomic_guard_matches(const ComputeAtomicGuard *guard, bool complete) {
    return atomic_guard_model_matches(guard) && atomic_guard_source_matches(guard, complete);
}

static void atomic_guard_fail(ComputeAtomicGuard *guard) {
    guard->rejected = true;
    hlsl_emit_fail(guard->ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_OUTPUT,
                   HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
}

static bool atomic_guard_observer(void *context, const HLSLSourceQualityObservation *observation) {
    ComputeAtomicGuard *guard = context;
    if (!atomic_guard_matches(guard, false)) { atomic_guard_fail(guard); return false; }
    const StringBuilder staged = *guard->ctx->sb;
    const bool accepted = !guard->options_owner.source_quality_observer ||
        guard->options_owner.source_quality_observer(
            guard->options_owner.source_quality_observer_context, observation);
    if (!accepted || memcmp(guard->ctx->sb, &staged, sizeof(staged)) ||
        !atomic_guard_matches(guard, false)) { atomic_guard_fail(guard); return false; }
    return true;
}

static bool atomic_guard_retain(void *context, ASTExpr *expression) {
    ComputeAtomicGuard *guard = context;
    if (!atomic_guard_matches(guard, false)) { atomic_guard_fail(guard); return false; }
    const StringBuilder staged = *guard->ctx->sb;
    const bool accepted = guard->typed_owner.retain_expression(guard->typed_owner.expression_context, expression);
    if (memcmp(guard->ctx->sb, &staged, sizeof(staged)) || !atomic_guard_matches(guard, false))
        atomic_guard_fail(guard);
    /* A successful callback owns the root even when it broke another lease.
     * Let the failed builder reject publication without freeing it twice. */
    return accepted;
}

static bool atomic_guard_prepare(ComputeAtomicGuard *guard, const USILProgram *program,
    StringBuilder *output, const HLSLEmitNames *names, const HLSLEmitOptions *options,
    const HLSLComputeTypedSource *typed, HLSLEmitterContext *ctx) {
    guard->program = program; guard->program_owner = *program;
    guard->typed = typed; guard->typed_owner = *typed;
    guard->options = options; guard->options_owner = *options;
    guard->names = names;
    if (names) guard->names_owner = *names;
    guard->output = output; guard->output_owner = *output;
    guard->ctx = ctx;
    if (!atomic_program_top_supported(&guard->program_owner) || typed->resource_count != 1u || !typed->resources ||
        !typed->resources[0].scalar_atomic || output->len > COMPUTE_ATOMIC_SOURCE_LIMIT ||
        !atomic_builder_valid(output) ||
        options->reserved_preprocessor_identifier_count > COMPUTE_ATOMIC_KEYWORD_LIMIT ||
        ((options->reserved_preprocessor_identifier_count != 0u) != (options->reserved_preprocessor_identifiers != NULL)) ||
        (options->source_quality_observer && !options->source_quality) ||
        options->expression_source_map || options->unity_uv_helper) return false;
    memcpy(guard->instructions, program->instructions,
           (size_t)program->instruction_count * sizeof(*guard->instructions));
    guard->uav = program->uavs[0];
    if (program->signature_declaration_count) guard->declaration = program->signature_declarations[0];
    guard->resource_owner = typed->resources[0];
    if (!atomic_copy_identifier(guard->resource, guard->resource_owner.name, &guard->resource_length)) return false;
    guard->entry_owner = names ? names->entry_point : NULL;
    const char *entry = guard->entry_owner && guard->entry_owner[0] ? guard->entry_owner : "main";
    size_t entry_length;
    if (!atomic_copy_identifier(guard->entry, entry, &entry_length) ||
        !strcmp(guard->entry, guard->resource) || !strcmp(guard->entry, "InterlockedAdd") ||
        !strcmp(guard->resource, "InterlockedAdd")) return false;
    if (program->instruction_count == 3 &&
        (!strcmp(guard->entry, "atomicResult") || !strcmp(guard->resource, "atomicResult"))) return false;
    guard->entry_owner_length = guard->entry_owner && guard->entry_owner[0] ? entry_length : 0u;
    for (size_t index = 0; index < options->reserved_preprocessor_identifier_count; ++index) {
        guard->keyword_owners[index] = options->reserved_preprocessor_identifiers[index];
        if (!atomic_copy_identifier(guard->keywords[index], guard->keyword_owners[index], &guard->keyword_lengths[index]))
            return false;
        if (program->instruction_count == 3 && !strcmp(guard->keywords[index], "atomicResult")) return false;
        guard->owned_keywords[index] = guard->keywords[index];
    }
    guard->owned_program = guard->program_owner;
    guard->owned_program.instructions = guard->instructions;
    guard->owned_program.instruction_alloc = guard->owned_program.instruction_count;
    guard->owned_program.uavs = &guard->uav;
    guard->owned_program.uav_alloc = 1;
    if (guard->owned_program.signature_declaration_count) {
        guard->owned_program.signature_declarations = &guard->declaration;
        guard->owned_program.signature_declaration_alloc = 1;
    }
    guard->owned_resource = guard->resource_owner;
    guard->owned_resource.name = guard->resource;
    guard->owned_typed = guard->typed_owner;
    guard->owned_typed.resources = &guard->owned_resource;
    if (typed->retain_expression) {
        guard->owned_typed.retain_expression = atomic_guard_retain;
        guard->owned_typed.expression_context = guard;
    }
    guard->owned_options = guard->options_owner;
    guard->owned_options.source_quality_observer = atomic_guard_observer;
    guard->owned_options.source_quality_observer_context = guard;
    guard->owned_options.reserved_preprocessor_identifiers =
        options->reserved_preprocessor_identifier_count ? guard->owned_keywords : NULL;
    ctx->program = &guard->owned_program;
    ctx->reserved_preprocessor_identifiers = guard->owned_options.reserved_preprocessor_identifiers;
    ctx->reserved_preprocessor_identifier_count = guard->owned_options.reserved_preprocessor_identifier_count;
    if (!source_program_valid(ctx, &guard->owned_typed)) return false;
    for (int index = 0; index < guard->owned_program.instruction_count; ++index) {
        const uint32_t source = guard->instructions[index].source_instruction_index;
        if (source == UINT32_MAX || (index ? source != guard->instructions[index - 1].source_instruction_index + 1u
            : source <= guard->owned_program.compute.declaration_source_instruction_index) ||
            !typed_atomic_instruction_valid(ctx, index)) return false;
    }
    if (!atomic_return_analysis_prepare(ctx)) return false;
    sb_append_len(&guard->prefix, output->buf, output->len);
    if (!sb_ok(&guard->prefix)) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED, HLSL_EMIT_PHASE_CONTEXT_ALLOCATION,
                       HLSL_EMIT_REASON_ALLOCATION_FAILED);
        return false;
    }
    HLSLEmitterContext *scratch = calloc(1u, sizeof(*scratch));
    if (!scratch) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED, HLSL_EMIT_PHASE_CONTEXT_ALLOCATION,
                       HLSL_EMIT_REASON_ALLOCATION_FAILED);
        return false;
    }
    HLSLEmitDiagnostic scratch_diagnostic;
    hlsl_emit_diagnostic_init(&scratch_diagnostic);
    scratch->program = &guard->owned_program;
    scratch->sb = &guard->expected;
    scratch->diagnostic = &scratch_diagnostic;
    scratch->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    scratch->reserved_preprocessor_identifiers = ctx->reserved_preprocessor_identifiers;
    scratch->reserved_preprocessor_identifier_count = ctx->reserved_preprocessor_identifier_count;
    HLSLComputeTypedSource expected_typed = guard->owned_typed;
    expected_typed.retain_expression = NULL;
    expected_typed.expression_context = NULL;
    const bool expected = atomic_return_analysis_prepare(scratch) && sb_ok(&guard->prefix) &&
        emit_source(scratch, guard->entry, &expected_typed) &&
        hlsl_expression_identifiers_available(scratch, 0u) && guard->expected.len <= COMPUTE_ATOMIC_SOURCE_LIMIT &&
        scratch_diagnostic.status == HLSL_EMIT_STATUS_OK;
    free_hlsl_use_def_graph(scratch);
    free_hlsl_ssa_graph(scratch);
    free_control_flow_graph(scratch);
    free(scratch);
    if (!expected && scratch_diagnostic.status != HLSL_EMIT_STATUS_OK && ctx->diagnostic)
        *ctx->diagnostic = scratch_diagnostic;
    if (!sb_ok(&guard->expected))
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_OUTPUT_FAILED, HLSL_EMIT_PHASE_OUTPUT,
                       HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED);
    guard->ready = expected;
    return expected;
}

static bool emit_atomic_compute_stage(const USILProgram *program, StringBuilder *output,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    const HLSLComputeTypedSource *typed, HLSLEmitDiagnostic *diagnostic) {
    StringBuilder staged;
    sb_init(&staged);
    HLSLEmitterContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program = program; ctx.sb = &staged; ctx.diagnostic = diagnostic;
    ctx.emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    ComputeAtomicGuard guard = {0};
    sb_init(&guard.expected); sb_init(&guard.prefix);
    bool emitted = false;
    if (!atomic_guard_prepare(&guard, program, output, names, options, typed, &ctx)) {
        if (!diagnostic || diagnostic->status == HLSL_EMIT_STATUS_OK) reject_stage(&ctx);
        HLSLEmitOptions failed_options = *options;
        failed_options.source_quality_observer = NULL;
        failed_options.source_quality_observer_context = NULL;
        hlsl_source_quality_initialize(&ctx, &failed_options);
        goto cleanup;
    }
    if (!hlsl_source_quality_initialize(&ctx, &guard.owned_options) ||
        !hlsl_source_quality_begin_entry(&ctx, true)) goto cleanup;
    emitted = emit_source(&ctx, guard.entry, &guard.owned_typed) &&
        hlsl_expression_identifiers_available(&ctx, 0u);
    if (emitted && !atomic_guard_matches(&guard, true)) { atomic_guard_fail(&guard); emitted = false; }
    if (emitted) {
        sb_append_len(output, staged.buf, staged.len);
        emitted = sb_ok(output);
        if (emitted) {
            guard.output_owner = *output;
            guard.published = true;
        } else {
            hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_OUTPUT_FAILED, HLSL_EMIT_PHASE_OUTPUT,
                           HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED);
        }
    }
cleanup:
    hlsl_source_quality_finish_emission(&ctx);
    if (emitted && !atomic_guard_matches(&guard, true)) { atomic_guard_fail(&guard); emitted = false; }
    free_hlsl_use_def_graph(&ctx);
    free_hlsl_ssa_graph(&ctx);
    free_control_flow_graph(&ctx);
    emitted = emitted && sb_ok(&staged) && (!diagnostic || diagnostic->status == HLSL_EMIT_STATUS_OK);
    if (!emitted) {
        if (guard.ready) {
            output->len = 0u;
            output->failed = false;
            if (output->buf) output->buf[0] = '\0';
            sb_append_len(output, guard.prefix.buf, guard.prefix.len);
        }
        output->failed = true;
        if (guard.rejected && guard.options_owner.source_quality) {
            HLSLSourceQualityResult *quality = guard.options_owner.source_quality;
            quality->emission_status = diagnostic ? diagnostic->status : HLSL_EMIT_STATUS_ANALYSIS_FAILED;
            quality->classification = HLSL_SOURCE_QUALITY_FAILED;
            quality->reasons |= HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED;
        }
    }
    sb_free(&guard.expected); sb_free(&guard.prefix); sb_free(&staged);
    return emitted;
}

static bool emit_compute_stage(const USILProgram *program, StringBuilder *output,
                             const HLSLEmitNames *names, const HLSLEmitOptions *options,
                             const HLSLComputeTypedSource *typed, HLSLEmitDiagnostic *diagnostic) {
    StringBuilder staged;
    sb_init(&staged);
    HLSLEmitterContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program = program;
    ctx.sb = &staged;
    ctx.diagnostic = diagnostic;
    ctx.emit_mode = options ? options->mode : HLSL_EMIT_MODE_RECOMPILE;
    ctx.reserved_preprocessor_identifiers = options ? options->reserved_preprocessor_identifiers : NULL;
    ctx.reserved_preprocessor_identifier_count = options ? options->reserved_preprocessor_identifier_count : 0;
    const char *entry_point = names && names->entry_point && names->entry_point[0] ? names->entry_point : "main";
    const size_t source_start = output->len;
    bool emitted = false;
    if (!hlsl_source_identifier_valid(entry_point)) {
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_INVALID_ARGUMENT, HLSL_EMIT_PHASE_ARGUMENT_VALIDATION,
                       HLSL_EMIT_REASON_INVALID_ARGUMENT);
        goto cleanup;
    }
    if ((ctx.emit_mode != HLSL_EMIT_MODE_RECOMPILE && ctx.emit_mode != HLSL_EMIT_MODE_READABLE &&
         ctx.emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) ||
        (options && (options->expression_source_map || options->unity_uv_helper))) {
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_UNSUPPORTED, HLSL_EMIT_PHASE_ARGUMENT_VALIDATION,
                       HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
        goto cleanup;
    }
    if (!hlsl_source_quality_initialize(&ctx, options) || !source_program_valid(&ctx, typed)) goto cleanup;
    size_t barrier_count = 0;
    for (int index = 0; index < program->instruction_count; ++index) {
        if (!(typed ? typed_instruction_valid(&ctx, typed, index) : instruction_valid(&ctx, index, true, false))) goto cleanup;
        barrier_count += program->instructions[index].opcode == USIL_OP_SYNC;
    }
    if (barrier_count != program->compute.barrier_count) {
        reject_stage(&ctx);
        goto cleanup;
    }
    if (program->instructions[program->instruction_count - 1].opcode != USIL_OP_RET) {
        reject_stage(&ctx);
        goto cleanup;
    }
    /* This route emits one entry unit with no helper/include syntax. Typed
     * resources require the private complete-candidate inventory; declarations
     * are either emitted here or covered by its enclosing configuration unit.
     * Direct uint system values and all body expressions retain authority.
     * Shared memory still preserves only byte layout and remains incomplete.
     * Complete coverage does not erase instruction-assignment residuals. */
    const bool structured_declarations = typed && typed->emit_declarations && typed->resource_count && typed->resources[0].structured;
    if (!hlsl_source_quality_begin_entry(&ctx, program->compute.shared_memory_count == 0 && !structured_declarations))
        goto cleanup;
    if (!build_control_flow_graph(&ctx) || !compute_dominance(&ctx.cfg) ||
        !build_hlsl_ssa_graph(&ctx)) {
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_SSA_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        goto cleanup;
    }
    if (typed && !build_hlsl_use_def_graph(&ctx)) {
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED, HLSL_EMIT_PHASE_SSA_ANALYSIS,
                       HLSL_EMIT_REASON_ALLOCATION_FAILED);
        goto cleanup;
    }
    emitted = emit_source(&ctx, entry_point, typed) && hlsl_expression_identifiers_available(&ctx, 0);
cleanup:
    if (!emitted && diagnostic && diagnostic->status == HLSL_EMIT_STATUS_OK)
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_OUTPUT_FAILED, HLSL_EMIT_PHASE_OUTPUT,
                       HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED);
    if (emitted && sb_ok(&staged)) {
        sb_append_len(output, staged.buf, staged.len);
        emitted = sb_ok(output);
        if (!emitted)
            hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_OUTPUT_FAILED, HLSL_EMIT_PHASE_OUTPUT,
                           HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED);
    }
    hlsl_source_quality_finish_emission(&ctx);
    emitted = emitted && sb_ok(&staged) && (!diagnostic || diagnostic->status == HLSL_EMIT_STATUS_OK);
    free_hlsl_use_def_graph(&ctx);
    free_hlsl_ssa_graph(&ctx);
    free_control_flow_graph(&ctx);
    if (!emitted) {
        output->len = source_start;
        if (output->buf) output->buf[source_start] = '\0';
        output->failed = true;
    }
    sb_free(&staged);
    return emitted;
}


bool hlsl_emit_compute_stage(const USILProgram *program, StringBuilder *output,
    const HLSLEmitNames *names, const HLSLEmitOptions *options, HLSLEmitDiagnostic *diagnostic) {
    return emit_compute_stage(program, output, names, options, NULL, diagnostic);
}

bool hlsl_emit_compute_typed_stage(const USILProgram *program, StringBuilder *output,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    const HLSLComputeTypedSource *typed, HLSLEmitDiagnostic *diagnostic) {
    hlsl_emit_diagnostic_init(diagnostic);
    if (!program || !output || !typed || !options || options->mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
        options->global_declarations || !sb_ok(output)) {
        if (output) output->failed = true;
        HLSLEmitterContext invalid = {.diagnostic = diagnostic};
        hlsl_emit_fail(&invalid, HLSL_EMIT_STATUS_INVALID_ARGUMENT,
                       HLSL_EMIT_PHASE_ARGUMENT_VALIDATION, HLSL_EMIT_REASON_INVALID_ARGUMENT);
        return false;
    }
    if (typed->resource_count == 1u && typed->resources && typed->resources[0].scalar_atomic)
        return emit_atomic_compute_stage(program, output, names, options, typed, diagnostic);
    return emit_compute_stage(program, output, names, options, typed, diagnostic);
}
