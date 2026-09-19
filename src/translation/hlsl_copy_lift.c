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
    /* Undefined lanes are outside this proof, even when the candidate would
     * leave that particular use unchanged. No convenient value is invented. */
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
                if ((use.source_lane_mask & (1u << lane)) &&
                    hlsl_operand_definition(&ctx, index, operand, lane) < 0) {
                    status = HLSL_COPY_LIFT_UNDEFINED_SOURCE;
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
    if (dxbc_size_multiply_overflows((size_t)baseline->instruction_count,
                                     sizeof(*candidate->instructions))) {
        status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
        goto cleanup;
    }
    size_t bytes = (size_t)baseline->instruction_count * sizeof(*candidate->instructions);
    candidate->instructions = malloc(bytes);
    if (!candidate->instructions) {
        status = HLSL_COPY_LIFT_OUT_OF_MEMORY;
        goto cleanup;
    }
    memcpy(candidate->instructions, baseline->instructions, bytes);
    candidate->view = *baseline;
    candidate->view.instructions = candidate->instructions;
    candidate->view.instruction_alloc = baseline->instruction_count;
    for (size_t index = 0; index < candidate->edit_count; ++index) {
        const HLSLCopyLiftEdit *edit = &candidate->edits[index];
        DXBCOperand *operand =
            &candidate->instructions[edit->instruction_index].operands[edit->operand_index];
        *operand = *source;
        operand->swizzle_mode = 1;
        operand->destination_mask = 0;
        operand->text[0] = '\0';
        /* Rebuild only the component-selection part of this semantic operand.
         * The original lossless DXBC document is untouched. Unconsumed lanes
         * are explicitly absent from the edit's demand mask. */
        operand->raw_token = (operand->raw_token & ~UINT32_C(0xfff)) | 6u;
        for (int lane = 0; lane < 4; ++lane) {
            operand->swizzle[lane] = edit->source_components[lane];
            operand->raw_token |= (uint32_t)operand->swizzle[lane] << (4u + 2u * (unsigned)lane);
        }
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
        STATUS(OUT_OF_MEMORY, "out-of-memory");
#undef STATUS
    }
    return "unknown";
}
