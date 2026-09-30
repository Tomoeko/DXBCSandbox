// SPDX-License-Identifier: GPL-3.0-only
#include "hlsl_patch_constants.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_identifier.h"
#include <string.h>

bool hlsl_patch_scalar_layout(const USILProgram *program, HLSLPatchLayout *layout) {
    HLSLDomainShape shape;
    if (!program || !layout || !program->tessellation.valid ||
        (program->program_type != DXBC_PROGRAM_TYPE_HULL && program->program_type != DXBC_PROGRAM_TYPE_DOMAIN) ||
        !hlsl_domain_shape(program->tessellation.domain, &shape) ||
        program->patch_constant_count <= 0 || program->patch_constant_count > HLSL_PATCH_CONSTANT_LIMIT ||
        program->patch_constant_alloc < program->patch_constant_count || !program->patch_constants) return false;
    memset(layout, 0, sizeof(*layout));
    memset(layout->register_field, -1, sizeof(layout->register_field));
    const DXBCSignatureElement *rows[HLSL_PATCH_CONSTANT_LIMIT] = {0};
    uint8_t outer = 0, inner = 0;
    for (int index = 0; index < program->patch_constant_count; ++index) {
        const DXBCSignatureElement *element = &program->patch_constants[index];
        if (element->register_id >= (uint32_t)program->patch_constant_count || rows[element->register_id] ||
            element->mask != 1 || (program->program_type == DXBC_PROGRAM_TYPE_HULL ? element->rw_mask != 14 :
                (element->rw_mask & ~element->mask) != 0) ||
            element->component_type != 3 || element->stream_index || element->min_precision || element->interpolation_mode ||
            (element->system_value == 0 && !hlsl_source_identifier_valid(dxbc_signature_semantic_name(element)))) return false;
        rows[element->register_id] = element;
        if (!element->system_value) {
            const char *semantic = dxbc_signature_semantic_name(element);
            const size_t length = strlen(semantic);
            if (!length || (semantic[length - 1] >= '0' && semantic[length - 1] <= '9')) return false;
            for (int previous = 0; previous < index; ++previous) {
                const DXBCSignatureElement *other = &program->patch_constants[previous];
                if (!other->system_value && !dxbc_ascii_strcasecmp(semantic, dxbc_signature_semantic_name(other)) &&
                    (strcmp(semantic, dxbc_signature_semantic_name(other)) || other->semantic_index == element->semantic_index)) return false;
            }
        }
    }
    for (unsigned reg = 0; reg < (unsigned)program->patch_constant_count;) {
        const DXBCSignatureElement *first = rows[reg];
        if (!first) return false;
        HLSLPatchField *field = &layout->fields[layout->field_count];
        field->semantic = dxbc_signature_semantic_name(first);
        field->semantic_index = first->semantic_index;
        field->first_register = (uint8_t)reg;
        field->kind = first->system_value == 0 ? HLSL_PATCH_CUSTOM :
            !strcmp(field->semantic, "SV_TessFactor") ? HLSL_PATCH_OUTER : HLSL_PATCH_INNER;
        const unsigned expected = field->kind == HLSL_PATCH_OUTER ? shape.outer_count :
            field->kind == HLSL_PATCH_INNER ? shape.inner_count : HLSL_PATCH_CONSTANT_LIMIT;
        if (field->kind != HLSL_PATCH_CUSTOM && (!expected || first->semantic_index)) return false;
        unsigned count = 0;
        while (reg + count < (unsigned)program->patch_constant_count && count < expected) {
            const DXBCSignatureElement *element = rows[reg + count];
            if (!element || strcmp(dxbc_signature_semantic_name(element), field->semantic) ||
                element->semantic_index != field->semantic_index + count) break;
            if (field->kind == HLSL_PATCH_OUTER) {
                if (element->system_value != shape.outer_system_values[count] || (outer & (1u << count))) return false;
                outer |= (uint8_t)(1u << count);
            } else if (field->kind == HLSL_PATCH_INNER) {
                if (strcmp(field->semantic, "SV_InsideTessFactor") ||
                    element->system_value != shape.inner_system_values[count] || (inner & (1u << count))) return false;
                inner |= (uint8_t)(1u << count);
            } else if (element->system_value || element->semantic_index > 31) return false;
            layout->register_field[reg + count] = (int8_t)layout->field_count;
            layout->register_element[reg + count] = (uint8_t)count;
            ++count;
        }
        if (!count || (field->kind != HLSL_PATCH_CUSTOM && count != expected)) return false;
        field->count = (uint8_t)count;
        layout->has_custom |= field->kind == HLSL_PATCH_CUSTOM;
        ++layout->field_count;
        reg += count;
    }
    layout->row_count = (uint8_t)program->patch_constant_count;
    return outer == (uint8_t)((1u << shape.outer_count) - 1u) &&
        inner == (uint8_t)((1u << shape.inner_count) - 1u);
}

bool hlsl_patch_static_scalar_operand(const HLSLPatchLayout *layout, const DXBCOperand *operand) {
    return layout && operand && operand->type == OPERAND_TYPE_INPUT_PATCH_CONSTANT &&
        operand->register_index_dim == 1 && operand->register_index >= 0 &&
        operand->register_index < layout->row_count && layout->register_field[operand->register_index] >= 0 &&
        operand->index_has_immediate[0] && !operand->index_representations[0] &&
        !operand->index_value_exceeds_int[0] && operand->index_values[0] == (uint32_t)operand->register_index &&
        !operand->rel_op0 && !operand->rel_op1 && !operand->rel_op2 && !operand->min_precision &&
        operand->swizzle_mode == 2 && !operand->swizzle[0];
}
