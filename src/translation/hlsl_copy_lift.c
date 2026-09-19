// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_copy_lift.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <stdlib.h>
#include <string.h>

struct HLSLCopyLift {
    USILProgram view;
    USILInstruction *instructions;
    HLSLCopyLiftEdit *edits;
    size_t edit_count;
    size_t edit_capacity;
    int copy_instruction;
    int producer_instruction;
};

static bool plain_operand(const DXBCOperand *operand) {
    return operand && !operand->has_abs && !operand->has_neg && operand->min_precision == 0 &&
           !operand->rel_op0 && !operand->rel_op1 && !operand->rel_op2 &&
           operand->extended_token_count == 0 && !operand->extended_tokens;
}

static bool immutable_source(const DXBCOperand *operand) {
    return operand->type == OPERAND_TYPE_INPUT || operand->type == OPERAND_TYPE_CONSTANT_BUFFER;
}

static bool append_edit(HLSLCopyLift *candidate, HLSLCopyLiftEdit edit) {
    if (candidate->edit_count == candidate->edit_capacity) {
        size_t capacity = candidate->edit_capacity ? candidate->edit_capacity * 2u : 8u;
        if (capacity < candidate->edit_capacity ||
            dxbc_size_multiply_overflows(capacity, sizeof(*candidate->edits)))
            return false;
        HLSLCopyLiftEdit *replacement =
            realloc(candidate->edits, capacity * sizeof(*candidate->edits));
        if (!replacement)
            return false;
        candidate->edits = replacement;
        candidate->edit_capacity = capacity;
    }
    candidate->edits[candidate->edit_count++] = edit;
    return true;
}

static HLSLCopyLiftStatus validate_region(const USILProgram *program) {
    if (!program || program->instruction_count <= 0 || !program->instructions ||
        program->temp_count < 0 || program->texture_count < 0 ||
        (program->texture_count && !program->textures))
        return HLSL_COPY_LIFT_INVALID_PROGRAM;
    if (!program->has_stage_contract ||
        (program->program_type != DXBC_PROGRAM_TYPE_VERTEX &&
         program->program_type != DXBC_PROGRAM_TYPE_PIXEL) ||
        (program->shader_model_major != 4 && program->shader_model_major != 5))
        return HLSL_COPY_LIFT_UNSUPPORTED_STAGE;
    if (program->has_global_flags && program->global_flags != 1u)
        return HLSL_COPY_LIFT_PRECISION_CONTROL;
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *inst = &program->instructions[index];
        USILEffectFlags effects;
        if (!usil_instruction_effects(program, inst, &effects))
            return HLSL_COPY_LIFT_INVALID_PROGRAM;
        if (inst->precise_mask)
            return HLSL_COPY_LIFT_PRECISION_CONTROL;
        if (inst->opcode == USIL_OP_RET) {
            if (index + 1 != program->instruction_count)
                return HLSL_COPY_LIFT_EFFECTFUL_REGION;
        } else if (effects != USIL_EFFECT_NONE) {
            return HLSL_COPY_LIFT_EFFECTFUL_REGION;
        }
        for (int operand = 0; operand < inst->operand_count; ++operand) {
            const DXBCOperand *value = &inst->operands[operand];
            if (value->rel_op0 || value->rel_op1 || value->rel_op2)
                return HLSL_COPY_LIFT_RELATIVE_ADDRESSING;
            if (value->min_precision)
                return HLSL_COPY_LIFT_PRECISION_CONTROL;
            if (value->type == OPERAND_TYPE_TEMP &&
                (value->register_index < 0 || value->register_index >= program->temp_count))
                return HLSL_COPY_LIFT_INVALID_PROGRAM;
        }
    }
    return HLSL_COPY_LIFT_OK;
}

static HLSLCopyLiftStatus validate_defined_sources(const USILProgram *program,
                                                   const HLSLEmitterContext *ctx) {
    /* Undefined lanes are outside this proof, even when the candidate would
     * leave that particular use unchanged. No convenient value is invented. */
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILInstruction *inst = &program->instructions[index];
        for (int operand = 0; operand < inst->operand_count; ++operand) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(program, inst, operand, &use)) {
                return HLSL_COPY_LIFT_INVALID_PROGRAM;
            }
            if (use.use != USIL_OPERAND_USE_SOURCE ||
                inst->operands[operand].type != OPERAND_TYPE_TEMP)
                continue;
            for (int lane = 0; lane < 4; ++lane) {
                if ((use.source_lane_mask & (1u << lane)) &&
                    hlsl_operand_definition(ctx, index, operand, lane) < 0) {
                    return HLSL_COPY_LIFT_UNDEFINED_SOURCE;
                }
            }
        }
    }
    return HLSL_COPY_LIFT_OK;
}

static bool clone_instructions(HLSLCopyLift *candidate, const USILProgram *baseline) {
    if (dxbc_size_multiply_overflows((size_t)baseline->instruction_count,
                                     sizeof(*candidate->instructions)))
        return false;
    size_t bytes = (size_t)baseline->instruction_count * sizeof(*candidate->instructions);
    candidate->instructions = malloc(bytes);
    if (!candidate->instructions)
        return false;
    memcpy(candidate->instructions, baseline->instructions, bytes);
    candidate->view = *baseline;
    candidate->view.instructions = candidate->instructions;
    candidate->view.instruction_alloc = baseline->instruction_count;
    return true;
}

static void rewrite_selection(DXBCOperand *operand, const uint8_t components[4]) {
    operand->text[0] = '\0';
    if (operand->type == OPERAND_TYPE_IMMEDIATE32) {
        if (operand->imm_value_count == 4) {
            uint32_t original[4];
            memcpy(original, operand->imm_values, sizeof(original));
            for (int lane = 0; lane < 4; ++lane) {
                operand->imm_values[lane] = original[components[lane]];
                if (operand->immediate_word_count == 4)
                    operand->immediate_words[lane] = operand->imm_values[lane];
            }
        }
        return;
    }
    operand->swizzle_mode = 1;
    operand->destination_mask = 0;
    /* Only semantic selection changes; the lossless target stays immutable. */
    operand->raw_token = (operand->raw_token & ~UINT32_C(0xfff)) | 6u;
    for (int lane = 0; lane < 4; ++lane) {
        operand->swizzle[lane] = components[lane];
        operand->raw_token |= (uint32_t)components[lane] << (4u + 2u * (unsigned)lane);
    }
}

HLSLCopyLiftStatus hlsl_copy_lift_create(const USILProgram *baseline, int copy_instruction,
                                         HLSLCopyLift **out_candidate) {
    if (!out_candidate)
        return HLSL_COPY_LIFT_INVALID_PROGRAM;
    *out_candidate = NULL;
    HLSLCopyLiftStatus status = validate_region(baseline);
    if (status != HLSL_COPY_LIFT_OK)
        return status;
    if (copy_instruction < 0 || copy_instruction >= baseline->instruction_count)
        return HLSL_COPY_LIFT_INVALID_PROGRAM;
    const USILInstruction *copy = &baseline->instructions[copy_instruction];
    const DXBCOperand *destination = &copy->operands[0];
    const DXBCOperand *source = &copy->operands[1];
    if (copy->opcode != USIL_OP_MOV || copy->operand_count != 2 || copy->saturate ||
        destination->type != OPERAND_TYPE_TEMP || !plain_operand(destination) ||
        !plain_operand(source) || (source->type != OPERAND_TYPE_TEMP && !immutable_source(source)))
        return HLSL_COPY_LIFT_NOT_PLAIN_COPY;
    if (source->type == OPERAND_TYPE_TEMP && source->register_index == destination->register_index)
        return HLSL_COPY_LIFT_NOT_PLAIN_COPY;

    HLSLEmitterContext ctx = {0};
    ctx.program = baseline;
    HLSLCopyLift *candidate = NULL;
    if (!build_control_flow_graph(&ctx) || !compute_dominance(&ctx.cfg) ||
        !build_hlsl_ssa_graph(&ctx) || !build_component_provenance(&ctx)) {
        status = HLSL_COPY_LIFT_INVALID_PROGRAM;
        goto cleanup;
    }
    status = validate_defined_sources(baseline, &ctx);
    if (status != HLSL_COPY_LIFT_OK)
        goto cleanup;
    candidate = calloc(1u, sizeof(*candidate));
    if (!candidate) {
        status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
        goto cleanup;
    }
    candidate->copy_instruction = copy_instruction;
    candidate->producer_instruction = copy_instruction;
    const uint8_t written = usil_operand_destination_lane_mask(destination);
    for (int index = copy_instruction + 1; index < baseline->instruction_count; ++index) {
        const USILInstruction *inst = &baseline->instructions[index];
        for (int operand_index = 0; operand_index < inst->operand_count; ++operand_index) {
            const DXBCOperand *operand = &inst->operands[operand_index];
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(baseline, inst, operand_index, &use)) {
                status = HLSL_COPY_LIFT_INVALID_PROGRAM;
                goto cleanup;
            }
            if (use.use != USIL_OPERAND_USE_SOURCE || operand->type != OPERAND_TYPE_TEMP ||
                operand->register_index != destination->register_index)
                continue;
            uint8_t copied = 0;
            HLSLCopyLiftEdit edit = {.instruction_index = index,
                                     .operand_index = operand_index,
                                     .logical_lane_mask = use.source_lane_mask};
            for (int lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane)))
                    continue;
                int component = usil_operand_source_component(operand, lane);
                if (component < 0) {
                    status = HLSL_COPY_LIFT_INVALID_PROGRAM;
                    goto cleanup;
                }
                if (hlsl_operand_definition(&ctx, index, operand_index, lane) != copy_instruction)
                    continue;
                if (!(written & (1u << component))) {
                    status = HLSL_COPY_LIFT_INVALID_PROGRAM;
                    goto cleanup;
                }
                int original_component = usil_operand_source_component(source, component);
                if (original_component < 0) {
                    status = HLSL_COPY_LIFT_INVALID_PROGRAM;
                    goto cleanup;
                }
                edit.source_components[lane] = (uint8_t)original_component;
                copied |= (uint8_t)(1u << lane);
                if (!immutable_source(source)) {
                    const HLSLComponentProvenance *before = get_component_provenance(
                        &ctx, copy_instruction, source->register_index, original_component);
                    const HLSLComponentProvenance *after = get_component_provenance(
                        &ctx, index, source->register_index, original_component);
                    if (!before || before->definition_instruction < 0) {
                        status = HLSL_COPY_LIFT_UNDEFINED_SOURCE;
                        goto cleanup;
                    }
                    if (!after || before->definition_instruction != after->definition_instruction) {
                        status = HLSL_COPY_LIFT_SOURCE_CHANGED;
                        goto cleanup;
                    }
                }
            }
            if (!copied)
                continue;
            if (copied != use.source_lane_mask) {
                status = HLSL_COPY_LIFT_PARTIAL_USE;
                goto cleanup;
            }
            if (!plain_operand(operand)) {
                status = HLSL_COPY_LIFT_NOT_PLAIN_COPY;
                goto cleanup;
            }
            if (!append_edit(candidate, edit)) {
                status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
                goto cleanup;
            }
        }
    }
    if (!candidate->edit_count) {
        status = HLSL_COPY_LIFT_NO_USES;
        goto cleanup;
    }
    if (!clone_instructions(candidate, baseline)) {
        status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t index = 0; index < candidate->edit_count; ++index) {
        const HLSLCopyLiftEdit *edit = &candidate->edits[index];
        DXBCOperand *operand =
            &candidate->instructions[edit->instruction_index].operands[edit->operand_index];
        *operand = *source;
        rewrite_selection(operand, edit->source_components);
    }
    candidate->instructions[copy_instruction].opcode = USIL_OP_NOP;
    candidate->instructions[copy_instruction].operand_count = 0;
    *out_candidate = candidate;
    candidate = NULL;
cleanup:
    hlsl_copy_lift_destroy(candidate);
    free_component_provenance(&ctx);
    free_hlsl_ssa_graph(&ctx);
    free_control_flow_graph(&ctx);
    return status;
}

static bool result_opcode(USILOpcode opcode) {
    return opcode == USIL_OP_ADD || opcode == USIL_OP_MUL || opcode == USIL_OP_AND ||
           opcode == USIL_OP_OR || opcode == USIL_OP_XOR;
}

HLSLCopyLiftStatus hlsl_result_lift_create(const USILProgram *baseline, int copy_instruction,
                                           HLSLCopyLift **out_candidate) {
    if (!out_candidate)
        return HLSL_COPY_LIFT_INVALID_PROGRAM;
    *out_candidate = NULL;
    HLSLCopyLiftStatus status = validate_region(baseline);
    if (status != HLSL_COPY_LIFT_OK)
        return status;
    if (copy_instruction < 1 || copy_instruction >= baseline->instruction_count)
        return HLSL_COPY_LIFT_NOT_PLAIN_RESULT;
    const USILInstruction *copy = &baseline->instructions[copy_instruction];
    const DXBCOperand *destination = &copy->operands[0], *source = &copy->operands[1];
    if (copy->opcode != USIL_OP_MOV || copy->operand_count != 2 || copy->saturate ||
        source->type != OPERAND_TYPE_TEMP || !plain_operand(source) ||
        !plain_operand(destination) ||
        (destination->type != OPERAND_TYPE_TEMP && destination->type != OPERAND_TYPE_OUTPUT))
        return HLSL_COPY_LIFT_NOT_PLAIN_COPY;
    int producer_index = copy_instruction - 1;
    while (producer_index >= 0 && baseline->instructions[producer_index].opcode == USIL_OP_NOP)
        --producer_index;
    if (producer_index < 0)
        return HLSL_COPY_LIFT_NOT_PLAIN_RESULT;
    const USILInstruction *producer = &baseline->instructions[producer_index];
    if (!result_opcode(producer->opcode) || producer->operand_count != 3 || producer->saturate ||
        producer->operands[0].type != OPERAND_TYPE_TEMP ||
        producer->operands[0].register_index != source->register_index)
        return HLSL_COPY_LIFT_NOT_PLAIN_RESULT;
    for (int operand = 0; operand < 3; ++operand) {
        const DXBCOperand *value = &producer->operands[operand];
        if (!plain_operand(value) ||
            (value->type != OPERAND_TYPE_TEMP && !immutable_source(value) &&
             value->type != OPERAND_TYPE_IMMEDIATE32) ||
            (value->type == OPERAND_TYPE_IMMEDIATE32 && value->imm_value_count != 1 &&
             value->imm_value_count != 4))
            return HLSL_COPY_LIFT_NOT_PLAIN_RESULT;
    }
    const uint8_t written = usil_operand_destination_lane_mask(&producer->operands[0]);
    const uint8_t consumed = usil_operand_destination_lane_mask(destination);
    uint8_t selected = 0, lanes[4] = {0};
    for (int lane = 0; lane < 4; ++lane) {
        if (!(consumed & (1u << lane)))
            continue;
        int component = usil_operand_source_component(source, lane);
        if (component < 0 || !(written & (1u << component)) || (selected & (1u << component)))
            return HLSL_COPY_LIFT_NONBIJECTIVE_LANES;
        lanes[lane] = (uint8_t)component;
        selected |= (uint8_t)(1u << component);
    }
    if (!written || selected != written)
        return HLSL_COPY_LIFT_NONBIJECTIVE_LANES;
    HLSLEmitterContext ctx = {0};
    ctx.program = baseline;
    HLSLCopyLift *candidate = NULL;
    if (!build_control_flow_graph(&ctx) || !compute_dominance(&ctx.cfg) ||
        !build_hlsl_ssa_graph(&ctx) || !build_component_provenance(&ctx)) {
        status = HLSL_COPY_LIFT_INVALID_PROGRAM;
        goto cleanup;
    }
    status = validate_defined_sources(baseline, &ctx);
    if (status != HLSL_COPY_LIFT_OK)
        goto cleanup;
    /* Count actual reaching uses, including later reads through a partial
     * overwrite. Merely counting appearances of the register is insufficient. */
    for (int index = 0; index < baseline->instruction_count; ++index) {
        const USILInstruction *inst = &baseline->instructions[index];
        for (int operand = 0; operand < inst->operand_count; ++operand) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(baseline, inst, operand, &use)) {
                status = HLSL_COPY_LIFT_INVALID_PROGRAM;
                goto cleanup;
            }
            if (use.use != USIL_OPERAND_USE_SOURCE ||
                inst->operands[operand].type != OPERAND_TYPE_TEMP)
                continue;
            for (int lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane)))
                    continue;
                int definition = hlsl_operand_definition(&ctx, index, operand, lane);
                if (definition == producer_index && (index != copy_instruction || operand != 1)) {
                    status = HLSL_COPY_LIFT_MULTIPLE_USES;
                    goto cleanup;
                }
                if (index == copy_instruction && operand == 1 && definition != producer_index) {
                    status = HLSL_COPY_LIFT_PARTIAL_USE;
                    goto cleanup;
                }
            }
        }
    }
    candidate = calloc(1u, sizeof(*candidate));
    if (!candidate) {
        status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
        goto cleanup;
    }
    candidate->copy_instruction = copy_instruction;
    candidate->producer_instruction = producer_index;
    if (!clone_instructions(candidate, baseline)) {
        status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
        goto cleanup;
    }
    USILInstruction *forwarded = &candidate->instructions[copy_instruction];
    *forwarded = *producer;
    forwarded->operands[0] = *destination;
    for (int operand = 1; operand < 3; ++operand) {
        HLSLCopyLiftEdit edit = {.instruction_index = copy_instruction,
                                 .operand_index = operand,
                                 .logical_lane_mask = consumed};
        for (int lane = 0; lane < 4; ++lane) {
            if (!(consumed & (1u << lane)))
                continue;
            const DXBCOperand *input = &producer->operands[operand];
            int component = input->type == OPERAND_TYPE_IMMEDIATE32
                                ? lanes[lane]
                                : usil_operand_source_component(input, lanes[lane]);
            if (component < 0) {
                status = HLSL_COPY_LIFT_INVALID_PROGRAM;
                goto cleanup;
            }
            edit.source_components[lane] = (uint8_t)component;
        }
        if (!append_edit(candidate, edit)) {
            status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
            goto cleanup;
        }
        rewrite_selection(&forwarded->operands[operand], edit.source_components);
    }
    candidate->instructions[producer_index].opcode = USIL_OP_NOP;
    candidate->instructions[producer_index].operand_count = 0;
    *out_candidate = candidate;
    candidate = NULL;
cleanup:
    hlsl_copy_lift_destroy(candidate);
    free_component_provenance(&ctx);
    free_hlsl_ssa_graph(&ctx);
    free_control_flow_graph(&ctx);
    return status;
}

int hlsl_copy_lift_producer_instruction(const HLSLCopyLift *candidate) {
    return candidate ? candidate->producer_instruction : -1;
}

const USILProgram *hlsl_copy_lift_program(const HLSLCopyLift *candidate) {
    return candidate ? &candidate->view : NULL;
}
const HLSLCopyLiftEdit *hlsl_copy_lift_edits(const HLSLCopyLift *candidate, size_t *out_count) {
    if (out_count)
        *out_count = candidate ? candidate->edit_count : 0u;
    return candidate ? candidate->edits : NULL;
}
int hlsl_copy_lift_instruction(const HLSLCopyLift *candidate) {
    return candidate ? candidate->copy_instruction : -1;
}
void hlsl_copy_lift_destroy(HLSLCopyLift *candidate) {
    if (!candidate)
        return;
    free(candidate->edits);
    free(candidate->instructions);
    free(candidate);
}
const char *hlsl_copy_lift_status_name(HLSLCopyLiftStatus status) {
    switch (status) {
#define STATUS(value, name)                                                                        \
    case HLSL_COPY_LIFT_##value:                                                                   \
        return name
        STATUS(OK, "ok");
        STATUS(INVALID_PROGRAM, "invalid-program");
        STATUS(UNSUPPORTED_STAGE, "unsupported-stage");
        STATUS(NOT_PLAIN_COPY, "not-plain-copy");
        STATUS(PRECISION_CONTROL, "precision-control");
        STATUS(EFFECTFUL_REGION, "effectful-region");
        STATUS(RELATIVE_ADDRESSING, "relative-addressing");
        STATUS(PARTIAL_USE, "partial-use");
        STATUS(UNDEFINED_SOURCE, "undefined-source");
        STATUS(SOURCE_CHANGED, "source-changed");
        STATUS(NO_USES, "no-uses");
        STATUS(NOT_PLAIN_RESULT, "not-plain-result");
        STATUS(MULTIPLE_USES, "multiple-uses");
        STATUS(NONBIJECTIVE_LANES, "nonbijective-lanes");
        STATUS(OUT_OF_MEMORY, "out-of-memory");
#undef STATUS
    }
    return "unknown";
}
