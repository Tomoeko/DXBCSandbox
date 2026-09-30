// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_identifier.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <string.h>

/* A bounded source projection, not the Class72 declaration inverse. The
 * existing lossless decoder and USIL execution contract supply all metadata;
 * existing CFG/SSA supplies definition ownership. Resource effects,
 * signed/float domains, and control flow remain explicitly unavailable here. */
enum { COMPUTE_SOURCE_INSTRUCTION_LIMIT = HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT };

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

static bool source_program_valid(HLSLEmitterContext *ctx) {
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
        program->cbuffer_count || program->texture_count || program->sampler_count ||
        program->uav_count || program->indexable_temp_count || program->index_range_count ||
        program->icb_value_count || program->geometry.valid || program->tessellation.valid ||
        compute->barrier_count > (size_t)program->instruction_count ||
        (program->has_global_flags && program->global_flags != 1u))
        return reject_stage(ctx);

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

static bool instruction_valid(HLSLEmitterContext *ctx, int index) {
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
    if (instruction->opcode != USIL_OP_MOV && instruction->opcode != USIL_OP_IADD &&
        instruction->opcode != USIL_OP_AND && instruction->opcode != USIL_OP_OR &&
        instruction->opcode != USIL_OP_XOR && instruction->opcode != USIL_OP_NOT &&
        instruction->opcode != USIL_OP_ISHL && instruction->opcode != USIL_OP_USHR)
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
                    component <= previous)
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

static bool emit_source(HLSLEmitterContext *ctx, const char *entry_point) {
    const USILProgram *program = ctx->program;
    emit_shared_memory(ctx);
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

bool hlsl_emit_compute_stage(const USILProgram *program, StringBuilder *output,
                             const HLSLEmitNames *names, const HLSLEmitOptions *options,
                             HLSLEmitDiagnostic *diagnostic) {
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
    if (!hlsl_source_quality_initialize(&ctx, options) || !source_program_valid(&ctx)) goto cleanup;
    size_t barrier_count = 0;
    for (int index = 0; index < program->instruction_count; ++index) {
        if (!instruction_valid(&ctx, index)) goto cleanup;
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
    /* This route emits one entry unit and no helper/include/resource syntax.
     * Direct uint system values have signature authority and every admitted
     * body expression/event is observed below. Shared memory still preserves
     * only byte layout, so its source inventory remains explicitly incomplete.
     * Complete coverage does not erase instruction-assignment residuals. */
    if (!hlsl_source_quality_begin_entry(&ctx, program->compute.shared_memory_count == 0))
        goto cleanup;
    if (!build_control_flow_graph(&ctx) || !compute_dominance(&ctx.cfg) ||
        !build_hlsl_ssa_graph(&ctx)) {
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED, HLSL_EMIT_PHASE_SSA_ANALYSIS,
                       HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
        goto cleanup;
    }
    emitted = emit_source(&ctx, entry_point) && hlsl_expression_identifiers_available(&ctx, 0);
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
