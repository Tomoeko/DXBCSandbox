// SPDX-License-Identifier: GPL-3.0-only

#include "translation/dxbc_cbuffer_projection.h"
#include "translation/usil_validation.h"

#include <limits.h>
#include <string.h>

static DXBCCBufferProjectionStatus combine_status(
    DXBCCBufferProjectionStatus left, DXBCCBufferProjectionStatus right) {
    return left > right ? left : right;
}

static bool ranges_overlap(uint32_t left_offset, uint32_t left_size,
                           uint32_t right_offset, uint32_t right_size) {
    uint64_t left_end = (uint64_t)left_offset + left_size;
    uint64_t right_end = (uint64_t)right_offset + right_size;
    return (uint64_t)left_offset < right_end &&
           (uint64_t)right_offset < left_end;
}

static bool validate_ranges(uint32_t cbuffer_size_bytes,
                            const DXBCCBufferVariableRange *variables,
                            size_t variable_count) {
    if (cbuffer_size_bytes == 0 || (cbuffer_size_bytes & 15u) != 0 ||
        (variable_count > 0 && !variables)) {
        return false;
    }
    for (size_t index = 0; index < variable_count; ++index) {
        const DXBCCBufferVariableRange *range = &variables[index];
        uint64_t end = (uint64_t)range->byte_offset + range->byte_size;
        if (range->byte_size == 0 || (range->byte_offset & 3u) != 0 ||
            (range->byte_size & 3u) != 0 || end > UINT32_MAX) {
            return false;
        }
        /* Release DXBC declares only through its highest referenced register.
         * A serialized variable may begin before that end and extend beyond
         * it. Projection visits only declared rows, so the intersection is
         * exact while the full range remains available to HLSL emission. */
        for (size_t other = 0; other < index; ++other) {
            if (ranges_overlap(range->byte_offset, range->byte_size,
                               variables[other].byte_offset,
                               variables[other].byte_size)) {
                return false;
            }
        }
    }
    return true;
}

bool dxbc_cbuffer_projection_reset(DXBCCBufferProjection *projection,
                                   DXBCCBufferVariableUse *uses,
                                   size_t variable_count) {
    if (!projection || (variable_count > 0 && !uses) ||
        variable_count > SIZE_MAX / sizeof(*uses)) {
        return false;
    }
    memset(projection, 0, sizeof(*projection));
    if (variable_count > 0) {
        memset(uses, 0, variable_count * sizeof(*uses));
    }
    return true;
}

static bool operand_physical_lane_mask(const DXBCOperand *operand,
                                       uint8_t logical_lane_mask,
                                       uint8_t *physical_lane_mask) {
    if (!operand || !physical_lane_mask || logical_lane_mask == 0 ||
        (logical_lane_mask & 0xf0u) != 0) {
        return false;
    }
    uint8_t result = 0;
    switch (operand->swizzle_mode) {
        /* Mask selection is a destination form.  A cbuffer is read-only, so
         * accepting it here would require inventing a source-lane ordering. */
        case 0:
            return false;
        case 1:
            for (unsigned int lane = 0; lane < 4; ++lane) {
                if ((logical_lane_mask & (1u << lane)) == 0) continue;
                if (operand->swizzle[lane] > 3) return false;
                result |= (uint8_t)(1u << operand->swizzle[lane]);
            }
            break;
        case 2:
            if (operand->swizzle[0] > 3) return false;
            result = (uint8_t)(1u << operand->swizzle[0]);
            break;
        default:
            return false;
    }
    *physical_lane_mask = result;
    return result != 0;
}

static DXBCCBufferProjectionStatus project_byte(
    uint32_t byte_offset, uint8_t physical_lane,
    const DXBCCBufferVariableRange *variables, size_t variable_count,
    DXBCCBufferVariableUse *uses, DXBCCBufferProjection *projection,
    bool dynamic) {
    size_t match = SIZE_MAX;
    for (size_t variable = 0; variable < variable_count; ++variable) {
        uint64_t end = (uint64_t)variables[variable].byte_offset +
                       variables[variable].byte_size;
        if ((uint64_t)byte_offset >= variables[variable].byte_offset &&
            (uint64_t)byte_offset < end) {
            if (match != SIZE_MAX) return DXBC_CBUFFER_PROJECTION_INVALID;
            match = variable;
        }
    }
    if (match == SIZE_MAX) {
        projection->saw_padding_access = true;
        if (dynamic) {
            projection->saw_dynamic_padding_access = true;
        } else {
            projection->saw_static_padding_access = true;
        }
        return DXBC_CBUFFER_PROJECTION_EXACT;
    }
    uses[match].referenced = true;
    uses[match].dynamically_addressed |= dynamic;
    uses[match].component_mask |= (uint8_t)(1u << physical_lane);
    return DXBC_CBUFFER_PROJECTION_EXACT;
}

static DXBCCBufferProjectionStatus project_row(
    uint32_t row, uint8_t physical_lane_mask, uint32_t cbuffer_size_bytes,
    const DXBCCBufferVariableRange *variables, size_t variable_count,
    DXBCCBufferVariableUse *uses, DXBCCBufferProjection *projection,
    bool dynamic) {
    uint64_t row_offset = (uint64_t)row * 16u;
    if (row_offset >= cbuffer_size_bytes) {
        return DXBC_CBUFFER_PROJECTION_INVALID;
    }
    DXBCCBufferProjectionStatus status = DXBC_CBUFFER_PROJECTION_EXACT;
    for (unsigned int lane = 0; lane < 4; ++lane) {
        if ((physical_lane_mask & (1u << lane)) == 0) continue;
        status = combine_status(
            status,
            project_byte((uint32_t)row_offset + lane * 4u,
                         (uint8_t)lane, variables, variable_count, uses,
                         projection, dynamic));
    }
    return status;
}

DXBCCBufferProjectionStatus dxbc_cbuffer_project_operand(
    const DXBCOperand *operand, uint8_t logical_lane_mask,
    uint32_t cbuffer_register, uint32_t cbuffer_size_bytes,
    const DXBCCBufferVariableRange *variables, size_t variable_count,
    DXBCCBufferVariableUse *uses, DXBCCBufferProjection *projection) {
    if (!operand || !projection || (variable_count > 0 && !uses) ||
        !validate_ranges(cbuffer_size_bytes, variables, variable_count) ||
        operand->type != OPERAND_TYPE_CONSTANT_BUFFER ||
        operand->register_index_dim != 2 || operand->rel_op2) {
        return DXBC_CBUFFER_PROJECTION_INVALID;
    }

    /* SM4/5 cbuffer operands identify the buffer in dimension zero.  A
     * relative buffer slot cannot be mapped to one metadata block. */
    if (!operand->index_has_immediate[0] || operand->rel_op0 ||
        operand->index_representations[0] > 1 ||
        operand->index_value_exceeds_int[0] ||
        operand->index_values[0] > UINT32_MAX) {
        return DXBC_CBUFFER_PROJECTION_INVALID;
    }
    if (operand->register_index < 0 ||
        (uint32_t)operand->register_index != operand->index_values[0]) {
        return DXBC_CBUFFER_PROJECTION_INVALID;
    }
    if ((uint32_t)operand->index_values[0] != cbuffer_register) {
        return DXBC_CBUFFER_PROJECTION_EXACT;
    }

    uint8_t physical_lane_mask = 0;
    if (!operand_physical_lane_mask(operand, logical_lane_mask,
                                    &physical_lane_mask)) {
        return DXBC_CBUFFER_PROJECTION_INVALID;
    }
    projection->saw_access = true;

    const bool dynamic = operand->rel_op1 != NULL;
    if (!dynamic) {
        if (!operand->index_has_immediate[1] ||
            operand->index_representations[1] > 1 ||
            operand->index_value_exceeds_int[1] ||
            operand->index_values[1] > UINT32_MAX) {
            return DXBC_CBUFFER_PROJECTION_INVALID;
        }
        if (operand->rel_offset0 < 0 ||
            (uint32_t)operand->rel_offset0 != operand->index_values[1]) {
            return DXBC_CBUFFER_PROJECTION_INVALID;
        }
        return project_row((uint32_t)operand->index_values[1],
                           physical_lane_mask, cbuffer_size_bytes, variables,
                           variable_count, uses, projection, false);
    }

    if (operand->index_representations[1] < 2 ||
        operand->index_representations[1] > 4 ||
        (operand->index_representations[1] == 2 &&
         operand->index_has_immediate[1]) ||
        (operand->index_representations[1] >= 3 &&
         !operand->index_has_immediate[1]) ||
        operand->index_value_exceeds_int[1] ||
        operand->index_values[1] > INT_MAX || operand->rel_offset0 < 0 ||
        (uint32_t)operand->rel_offset0 != operand->index_values[1]) {
        return DXBC_CBUFFER_PROJECTION_INVALID;
    }

    /* The relative operand token contains an expression, not its value range.
     * Mark every byte lane that can be named in this cbuffer, then make the
     * missing authority explicit in the status. */
    projection->saw_dynamic_access = true;
    uint32_t row_count = cbuffer_size_bytes / 16u;
    DXBCCBufferProjectionStatus status =
        DXBC_CBUFFER_PROJECTION_REQUIRES_INDEX_AUTHORITY;
    for (uint32_t row = 0; row < row_count; ++row) {
        status = combine_status(
            status,
            project_row(row, physical_lane_mask, cbuffer_size_bytes,
                        variables, variable_count, uses, projection, true));
    }
    return status;
}

static DXBCCBufferProjectionStatus project_operand_tree(
    const DXBCOperand *operand, bool project_root, uint8_t root_lane_mask,
    uint32_t cbuffer_register, uint32_t cbuffer_size_bytes,
    const DXBCCBufferVariableRange *variables, size_t variable_count,
    DXBCCBufferVariableUse *uses, DXBCCBufferProjection *projection,
    unsigned int depth) {
    if (!operand || depth >= DXBC_MAX_NESTED_OPERAND_TOKENS)
        return DXBC_CBUFFER_PROJECTION_INVALID;
    DXBCCBufferProjectionStatus status = DXBC_CBUFFER_PROJECTION_EXACT;
    if (project_root && operand->type == OPERAND_TYPE_CONSTANT_BUFFER) {
        status = dxbc_cbuffer_project_operand(
            operand, root_lane_mask, cbuffer_register, cbuffer_size_bytes,
            variables, variable_count, uses, projection);
    }

    const DXBCOperand *relatives[3] = {
        operand->rel_op0, operand->rel_op1, operand->rel_op2
    };
    for (size_t index = 0; index < 3; ++index) {
        if (!relatives[index]) continue;
        status = combine_status(
            status,
            project_operand_tree(relatives[index], true, 1,
                                 cbuffer_register, cbuffer_size_bytes,
                                 variables, variable_count, uses, projection,
                                 depth + 1));
    }
    return status;
}

DXBCCBufferProjectionStatus dxbc_cbuffer_project_program(
    const USILProgram *program, uint32_t cbuffer_register,
    uint32_t cbuffer_size_bytes,
    const DXBCCBufferVariableRange *variables, size_t variable_count,
    DXBCCBufferVariableUse *uses, DXBCCBufferProjection *projection) {
    if (!program || !projection || program->instruction_count < 0 ||
        (program->instruction_count > 0 && !program->instructions) ||
        (variable_count > 0 && !uses) ||
        !validate_ranges(cbuffer_size_bytes, variables, variable_count)) {
        return DXBC_CBUFFER_PROJECTION_INVALID;
    }

    DXBCCBufferProjectionStatus status = DXBC_CBUFFER_PROJECTION_EXACT;
    for (int instruction_index = 0;
         instruction_index < program->instruction_count;
         ++instruction_index) {
        const USILInstruction *instruction =
            &program->instructions[instruction_index];
        if (!usil_instruction_shape_valid(program, instruction)) {
            return DXBC_CBUFFER_PROJECTION_INVALID;
        }
        for (int operand_index = 0;
             operand_index < instruction->operand_count;
             ++operand_index) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(program, instruction,
                                              operand_index, &use)) {
                return DXBC_CBUFFER_PROJECTION_INVALID;
            }
            const bool is_source = use.use == USIL_OPERAND_USE_SOURCE;
            const DXBCOperand *operand = &instruction->operands[operand_index];
            if (!is_source && operand->type == OPERAND_TYPE_CONSTANT_BUFFER) {
                return DXBC_CBUFFER_PROJECTION_INVALID;
            }
            status = combine_status(
                status, project_operand_tree(operand, is_source,
                                             use.source_lane_mask,
                                             cbuffer_register,
                                             cbuffer_size_bytes, variables,
                                             variable_count, uses, projection,
                                             0));
            if (status == DXBC_CBUFFER_PROJECTION_INVALID) return status;
        }
    }
    return status;
}
