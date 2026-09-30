// SPDX-License-Identifier: GPL-3.0-only
#include "hlsl_patch_constants.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_identifier.h"
#include <string.h>

static unsigned width(uint8_t mask) {
    unsigned count = 0;
    for (unsigned component = 0; component < 4; ++component) count += (mask >> component) & 1u;
    return count;
}
static bool contiguous(uint8_t mask) {
    if (!mask || (mask & ~15u)) return false;
    while (!(mask & 1u)) mask >>= 1;
    return (mask & (uint8_t)(mask + 1u)) == 0;
}
static bool add_field(HLSLPatchLayout *layout, const DXBCSignatureElement *const *rows,
                      unsigned count, HLSLPatchFieldKind kind) {
    if (!count || layout->field_count >= HLSL_PATCH_CONSTANT_LIMIT) return false;
    const DXBCSignatureElement *first = rows[0];
    HLSLPatchField *field = &layout->fields[layout->field_count];
    field->kind = kind; field->semantic = dxbc_signature_semantic_name(first);
    field->semantic_index = first->semantic_index; field->first_register = (uint8_t)first->register_id;
    field->count = (uint8_t)count; field->width = (uint8_t)width(first->mask); field->mask = first->mask;
    for (unsigned element = 0; element < count; ++element) {
        const DXBCSignatureElement *row = rows[element];
        if (row->mask != field->mask || row->register_id != first->register_id + element ||
            row->semantic_index != first->semantic_index + element || strcmp(dxbc_signature_semantic_name(row), field->semantic)) return false;
        unsigned component = 0;
        for (unsigned lane = 0; lane < 4; ++lane) if (row->mask & (1u << lane)) {
            if (layout->lane_field[row->register_id][lane] >= 0) return false;
            layout->lane_field[row->register_id][lane] = (int8_t)layout->field_count;
            layout->lane_element[row->register_id][lane] = (uint8_t)element;
            layout->lane_component[row->register_id][lane] = (uint8_t)component++;
        }
    }
    layout->has_custom |= kind == HLSL_PATCH_CUSTOM;
    ++layout->field_count;
    return true;
}
bool hlsl_patch_layout(const USILProgram *program, HLSLPatchLayout *layout) {
    HLSLDomainShape shape;
    if (!program || !layout || !program->tessellation.valid ||
        (program->program_type != DXBC_PROGRAM_TYPE_HULL && program->program_type != DXBC_PROGRAM_TYPE_DOMAIN) ||
        !hlsl_domain_shape(program->tessellation.domain, &shape) ||
        program->patch_constant_count <= 0 || program->patch_constant_count > HLSL_PATCH_CONSTANT_LIMIT ||
        program->patch_constant_alloc < program->patch_constant_count || !program->patch_constants) return false;
    memset(layout, 0, sizeof(*layout));
    memset(layout->lane_field, -1, sizeof(layout->lane_field));
    const DXBCSignatureElement *outer[4] = {0}, *inner[2] = {0};
    bool assigned[HLSL_PATCH_CONSTANT_LIMIT] = {0};
    for (int index = 0; index < program->patch_constant_count; ++index) {
        const DXBCSignatureElement *row = &program->patch_constants[index];
        if (row->register_id >= HLSL_PATCH_CONSTANT_LIMIT || !contiguous(row->mask) ||
            (program->program_type == DXBC_PROGRAM_TYPE_HULL ? row->rw_mask != (row->mask ^ 15u) :
                (row->rw_mask & ~row->mask) != 0) ||
            row->component_type != 3 || row->stream_index || row->min_precision || row->interpolation_mode ||
            (layout->register_mask[row->register_id] & row->mask)) return false;
        layout->register_mask[row->register_id] |= row->mask;
        if (row->register_id + 1u > layout->row_count) layout->row_count = (uint8_t)(row->register_id + 1u);
        const char *semantic = dxbc_signature_semantic_name(row);
        if (!row->system_value) {
            if (!hlsl_source_identifier_valid(semantic) || row->semantic_index > 31 ||
                width(row->mask) > 3 || (width(row->mask) == 1 && row->mask != 1)) return false;
            const size_t length = strlen(semantic);
            if (!length || (semantic[length - 1] >= '0' && semantic[length - 1] <= '9') ||
                (length >= 3 && (semantic[0] == 's' || semantic[0] == 'S') &&
                 (semantic[1] == 'v' || semantic[1] == 'V') && semantic[2] == '_')) return false;
            for (int previous = 0; previous < index; ++previous) {
                const DXBCSignatureElement *other = &program->patch_constants[previous];
                if (!other->system_value && !dxbc_ascii_strcasecmp(semantic, dxbc_signature_semantic_name(other)) &&
                    (strcmp(semantic, dxbc_signature_semantic_name(other)) || other->semantic_index == row->semantic_index ||
                     width(row->mask) > 1 || width(other->mask) > 1)) return false;
            }
        } else {
            if (row->mask != 1) return false;
            if (!strcmp(semantic, "SV_TessFactor")) {
                if (row->semantic_index >= shape.outer_count || outer[row->semantic_index] ||
                    row->system_value != shape.outer_system_values[row->semantic_index]) return false;
                outer[row->semantic_index] = row;
            } else if (!strcmp(semantic, "SV_InsideTessFactor")) {
                if (row->semantic_index >= shape.inner_count || inner[row->semantic_index] ||
                    row->system_value != shape.inner_system_values[row->semantic_index]) return false;
                inner[row->semantic_index] = row;
            } else return false;
            assigned[index] = true;
        }
    }
    for (unsigned index = 0; index < shape.outer_count; ++index) if (!outer[index]) return false;
    for (unsigned index = 0; index < shape.inner_count; ++index) if (!inner[index]) return false;
    /* System factor order comes from actual registers, not PCSG record order.
     * Custom fields follow the factor roles, preserving scalar-array sources. */
    if (shape.inner_count && inner[0]->register_id < outer[0]->register_id) {
        if (!add_field(layout, inner, shape.inner_count, HLSL_PATCH_INNER) ||
            !add_field(layout, outer, shape.outer_count, HLSL_PATCH_OUTER)) return false;
    } else if (!add_field(layout, outer, shape.outer_count, HLSL_PATCH_OUTER) ||
               (shape.inner_count && !add_field(layout, inner, shape.inner_count, HLSL_PATCH_INNER))) return false;
    for (unsigned reg = 0; reg < layout->row_count; ++reg) {
        if (!layout->register_mask[reg]) return false;
        for (int index = 0; index < program->patch_constant_count; ++index) {
            const DXBCSignatureElement *row = &program->patch_constants[index];
            if (assigned[index] || row->register_id != reg) continue;
            const DXBCSignatureElement *custom[HLSL_PATCH_CONSTANT_LIMIT] = {row};
            assigned[index] = true;
            unsigned count = 1;
            if (row->mask == 1) {
                for (;;) {
                    int next = -1;
                    for (int candidate = 0; candidate < program->patch_constant_count; ++candidate) {
                        const DXBCSignatureElement *following = &program->patch_constants[candidate];
                        if (!assigned[candidate] && !following->system_value && following->mask == 1 &&
                            following->register_id == reg + count && following->semantic_index == row->semantic_index + count &&
                            !strcmp(dxbc_signature_semantic_name(following), dxbc_signature_semantic_name(row))) { next = candidate; break; }
                    }
                    if (next < 0) break;
                    if (count >= HLSL_PATCH_CONSTANT_LIMIT) return false;
                    custom[count++] = &program->patch_constants[next]; assigned[next] = true;
                }
            }
            if (!add_field(layout, custom, count, HLSL_PATCH_CUSTOM)) return false;
        }
    }
    layout->signature_count = (uint8_t)program->patch_constant_count;
    return true;
}

bool hlsl_patch_static_operand(const HLSLPatchLayout *layout, const DXBCOperand *operand) {
    return layout && operand && operand->type == OPERAND_TYPE_INPUT_PATCH_CONSTANT &&
        operand->register_index_dim == 1 && operand->register_index >= 0 &&
        operand->register_index < layout->row_count && hlsl_lift_operand_is_plain(operand) &&
        !operand->extended_tokens && operand->index_has_immediate[0] &&
        !operand->index_representations[0] && !operand->index_value_exceeds_int[0] &&
        operand->index_values[0] == (uint32_t)operand->register_index &&
        !operand->rel_op0 && !operand->rel_op1 && !operand->rel_op2;
}
