// SPDX-License-Identifier: GPL-3.0-only

#include "translation/usil.h"
#include "dxbc/dxbc_decoder.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_usil_operand(DXBCOperand* op);

static void free_signature_elements(DXBCSignatureElement* elements,
                                    int count) {
    if (!elements || count <= 0) return;
    for (int index = 0; index < count; ++index) {
        dxbc_signature_element_free(&elements[index]);
    }
}

static bool clone_signature_elements(DXBCSignatureElement* destination,
                                     const DXBCSignatureElement* source,
                                     int count) {
    if (count < 0 || (count > 0 && (!destination || !source))) return false;
    for (int index = 0; index < count; ++index) {
        if (!dxbc_signature_element_clone(&destination[index],
                                          &source[index])) {
            free_signature_elements(destination, index);
            return false;
        }
    }
    return true;
}

static bool reserve_usil_array(void** array, int* allocation, int count,
                               int required, size_t element_size) {
    if (!array || !allocation || count < 0 || *allocation < count ||
        required < count || ((*allocation == 0) != (*array == NULL))) {
        return false;
    }
    if (required <= *allocation) return true;

    int new_allocation = *allocation == 0 ? 8 : *allocation;
    while (new_allocation < required) {
        if (new_allocation > INT_MAX / 2) {
            new_allocation = required;
            break;
        }
        new_allocation *= 2;
    }
    if (dxbc_size_multiply_overflows((size_t)*allocation, element_size) ||
        dxbc_size_multiply_overflows((size_t)new_allocation, element_size)) {
        return false;
    }
    const size_t old_size = (size_t)*allocation * element_size;
    const size_t new_size = (size_t)new_allocation * element_size;
    void* replacement = mem_realloc(*array, old_size, new_size);
    if (!replacement) return false;
    *array = replacement;
    *allocation = new_allocation;
    return true;
}

static bool clone_operand(DXBCOperand* dest, const DXBCOperand* src) {
    memcpy(dest, src, sizeof(DXBCOperand));
    dest->extended_tokens = NULL;
    dest->rel_op0 = NULL;
    dest->rel_op1 = NULL;
    dest->rel_op2 = NULL;
    if (src->extended_tokens && src->extended_token_count) {
        size_t size = src->extended_token_count * sizeof(uint32_t);
        dest->extended_tokens = (uint32_t*)mem_alloc(size);
        if (dest->extended_tokens) {
            memcpy(dest->extended_tokens, src->extended_tokens, size);
        } else {
            dest->extended_token_count = 0;
            return false;
        }
    }
    if (src->rel_op0) {
        dest->rel_op0 = (DXBCOperand*)mem_alloc(sizeof(DXBCOperand));
        if (!dest->rel_op0 || !clone_operand(dest->rel_op0, src->rel_op0)) {
            goto fail;
        }
    }
    if (src->rel_op1) {
        dest->rel_op1 = (DXBCOperand*)mem_alloc(sizeof(DXBCOperand));
        if (!dest->rel_op1 || !clone_operand(dest->rel_op1, src->rel_op1)) {
            goto fail;
        }
    }
    if (src->rel_op2) {
        dest->rel_op2 = (DXBCOperand*)mem_alloc(sizeof(DXBCOperand));
        if (!dest->rel_op2 || !clone_operand(dest->rel_op2, src->rel_op2)) {
            goto fail;
        }
    }
    return true;

fail:
    free_usil_operand(dest);
    return false;
}

static void free_usil_operand(DXBCOperand* op) {
    if (op->rel_op0) {
        free_usil_operand(op->rel_op0);
        mem_free(op->rel_op0, sizeof(DXBCOperand));
        op->rel_op0 = NULL;
    }
    if (op->rel_op1) {
        free_usil_operand(op->rel_op1);
        mem_free(op->rel_op1, sizeof(DXBCOperand));
        op->rel_op1 = NULL;
    }
    if (op->rel_op2) {
        free_usil_operand(op->rel_op2);
        mem_free(op->rel_op2, sizeof(DXBCOperand));
        op->rel_op2 = NULL;
    }
    if (op->extended_tokens) {
        mem_free(op->extended_tokens,
                 op->extended_token_count * sizeof(uint32_t));
        op->extended_tokens = NULL;
        op->extended_token_count = 0;
    }
}

static bool declaration_scalar_u32(const DXBCInstruction* instruction,
                                   int operand_index,
                                   uint32_t* value) {
    if (!instruction || !value || operand_index < 0 ||
        operand_index >= instruction->operand_count) {
        return false;
    }
    const DXBCOperand* operand = &instruction->operands[operand_index];
    if (operand->type != OPERAND_TYPE_IMMEDIATE32 ||
        operand->imm_value_count != 1 || operand->immediate_word_count != 1 ||
        operand->imm_values[0] != operand->immediate_words[0] ||
        operand->extended_token_count != 0 || operand->extended_tokens ||
        operand->rel_op0 || operand->rel_op1 || operand->rel_op2) {
        return false;
    }
    *value = operand->immediate_words[0];
    return true;
}

static bool signature_declaration_kind_is_input(
    USILSignatureDeclarationKind kind) {
    return kind >= USIL_SIGNATURE_DECL_INPUT &&
           kind <= USIL_SIGNATURE_DECL_INPUT_PS_SIV;
}

static bool signature_declaration_kind_is_output(
    USILSignatureDeclarationKind kind) {
    return kind >= USIL_SIGNATURE_DECL_OUTPUT &&
           kind <= USIL_SIGNATURE_DECL_OUTPUT_SIV;
}

static bool signature_declaration_kind_has_system_value(
    USILSignatureDeclarationKind kind) {
    return kind == USIL_SIGNATURE_DECL_INPUT_SGV ||
           kind == USIL_SIGNATURE_DECL_INPUT_SIV ||
           kind == USIL_SIGNATURE_DECL_INPUT_PS_SGV ||
           kind == USIL_SIGNATURE_DECL_INPUT_PS_SIV ||
           kind == USIL_SIGNATURE_DECL_OUTPUT_SGV ||
           kind == USIL_SIGNATURE_DECL_OUTPUT_SIV;
}

static bool signature_declaration_kind_has_interpolation(
    USILSignatureDeclarationKind kind) {
    return kind == USIL_SIGNATURE_DECL_INPUT_PS ||
           kind == USIL_SIGNATURE_DECL_INPUT_PS_SGV ||
           kind == USIL_SIGNATURE_DECL_INPUT_PS_SIV;
}

static uint32_t signature_system_value_from_name_token(uint32_t value) {
    if (value >= 1u && value <= 10u) return value;
    if (value >= 11u && value <= 14u) return 11u;
    if (value >= 15u && value <= 16u) return 12u;
    if (value >= 17u && value <= 19u) return 13u;
    if (value == 20u) return 14u;
    if (value == 21u) return 15u;
    if (value == 22u) return 16u;
    if (value >= 23u && value <= 25u) return value;
    return UINT32_MAX;
}

static uint32_t signature_system_value_from_special_output_operand(
    DXBCOperandType type) {
    switch (type) {
        case OPERAND_TYPE_OUTPUT_DEPTH: return 65u;
        case OPERAND_TYPE_OUTPUT_COVERAGE_MASK: return 66u;
        case OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL: return 67u;
        case OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL: return 68u;
        case OPERAND_TYPE_OUTPUT_STENCIL_REF: return 69u;
        default: return UINT32_MAX;
    }
}

static const char* signature_semantic_from_special_output_operand(
    DXBCOperandType type) {
    switch (type) {
        case OPERAND_TYPE_OUTPUT_DEPTH: return "SV_Depth";
        case OPERAND_TYPE_OUTPUT_COVERAGE_MASK: return "SV_Coverage";
        case OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
            return "SV_DepthGreaterEqual";
        case OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL:
            return "SV_DepthLessEqual";
        case OPERAND_TYPE_OUTPUT_STENCIL_REF: return "SV_StencilRef";
        default: return NULL;
    }
}

static bool signature_semantic_equals(const char* left,
                                      const char* right) {
    if (!left || !right) return false;
    while (*left != '\0' && *right != '\0') {
        unsigned char left_value = (unsigned char)*left++;
        unsigned char right_value = (unsigned char)*right++;
        if (left_value >= (unsigned char)'A' &&
            left_value <= (unsigned char)'Z') {
            left_value = (unsigned char)(left_value + ('a' - 'A'));
        }
        if (right_value >= (unsigned char)'A' &&
            right_value <= (unsigned char)'Z') {
            right_value = (unsigned char)(right_value + ('a' - 'A'));
        }
        if (left_value != right_value) return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool signature_input_builtin_operand(DXBCOperandType type) {
    switch (type) {
        case OPERAND_TYPE_INPUT_PRIMITIVE_ID:
        case OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID:
        case OPERAND_TYPE_FORK_INSTANCE_ID:
        case OPERAND_TYPE_JOIN_INSTANCE_ID:
        case OPERAND_TYPE_DOMAIN_LOCATION:
        case OPERAND_TYPE_INPUT_THREAD_ID:
        case OPERAND_TYPE_INPUT_THREAD_GROUP_ID:
        case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP:
        case OPERAND_TYPE_INPUT_COVERAGE_MASK:
        case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED:
        case OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
        case OPERAND_TYPE_INNER_COVERAGE:
            return true;
        default:
            return false;
    }
}

static bool signature_input_builtin_mask_is_valid(DXBCOperandType type,
                                                  uint8_t mask) {
    switch (type) {
        case OPERAND_TYPE_DOMAIN_LOCATION:
            return mask == 0x03u || mask == 0x07u;
        case OPERAND_TYPE_INPUT_THREAD_ID:
        case OPERAND_TYPE_INPUT_THREAD_GROUP_ID:
        case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP:
            return mask == 0x07u;
        case OPERAND_TYPE_INPUT_PRIMITIVE_ID:
        case OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID:
        case OPERAND_TYPE_FORK_INSTANCE_ID:
        case OPERAND_TYPE_JOIN_INSTANCE_ID:
        case OPERAND_TYPE_INPUT_COVERAGE_MASK:
        case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED:
        case OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
        case OPERAND_TYPE_INNER_COVERAGE:
            return mask == 0u;
        default:
            return false;
    }
}

static bool signature_declaration_operand(
    const DXBCInstruction* instruction, uint32_t* register_id,
    uint8_t* mask, DXBCOperandType* operand_type,
    bool* has_signature_register) {
    if (!instruction || !register_id || !mask || !operand_type ||
        !has_signature_register ||
        instruction->operand_count !=
            (instruction->has_declaration_system_value ? 2 : 1)) {
        return false;
    }
    const DXBCOperand* operand = &instruction->operands[0];
    if (operand->has_abs || operand->has_neg ||
        operand->extended_token_count != 0 || operand->extended_tokens ||
        operand->rel_op0 || operand->rel_op1 || operand->rel_op2 ||
        operand->min_precision != 0u || operand->swizzle_mode != 0u) {
        return false;
    }
    const uint8_t declaration_mask =
        (uint8_t)(operand->destination_mask >> 4u);
    if ((operand->destination_mask & 0x0fu) != 0u) {
        return false;
    }

    const bool special_builtin =
        signature_input_builtin_operand(operand->type);
    if (special_builtin) {
        if (operand->register_index_dim != 0 ||
            !signature_input_builtin_mask_is_valid(
                operand->type, declaration_mask)) {
            return false;
        }
        *register_id = UINT32_MAX;
        *mask = declaration_mask;
        *operand_type = operand->type;
        *has_signature_register = false;
        return true;
    }
    const bool special_output =
        operand->type == OPERAND_TYPE_OUTPUT_DEPTH ||
        operand->type == OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL ||
        operand->type == OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL ||
        operand->type == OPERAND_TYPE_OUTPUT_COVERAGE_MASK ||
        operand->type == OPERAND_TYPE_OUTPUT_STENCIL_REF;
    if (special_output) {
        if (operand->register_index_dim != 0 || declaration_mask != 0u) {
            return false;
        }
        *register_id = UINT32_MAX;
        *mask = 0u;
        *operand_type = operand->type;
        *has_signature_register = false;
        return true;
    }
    if (declaration_mask == 0u) return false;

    if ((operand->type == OPERAND_TYPE_INPUT ||
         operand->type == OPERAND_TYPE_INPUT_CONTROL_POINT ||
         operand->type == OPERAND_TYPE_OUTPUT_CONTROL_POINT) &&
               operand->register_index_dim == 2) {
        /* Arrayed stage inputs/outputs encode
         * value[array-element-count][attribute-register].  The signature
         * register is the second axis; the first is validated against the
         * lossless stage contract after projection. */
        if (!operand->index_has_immediate[0] ||
            !operand->index_has_immediate[1] ||
            operand->index_representations[0] != 0u ||
            operand->index_representations[1] != 0u ||
            operand->index_value_exceeds_int[0] ||
            operand->index_value_exceeds_int[1] ||
            operand->index_values[0] == 0u ||
            operand->index_values[0] > 32u ||
            operand->index_values[1] > UINT32_MAX ||
            operand->register_index < 0 || operand->rel_offset0 < 0 ||
            (uint64_t)(uint32_t)operand->register_index !=
                operand->index_values[0] ||
            (uint64_t)(uint32_t)operand->rel_offset0 !=
                operand->index_values[1]) {
            return false;
        }
        *register_id = (uint32_t)operand->index_values[1];
    } else {
        if (operand->register_index_dim != 1 ||
            !operand->index_has_immediate[0] ||
            operand->index_representations[0] != 0u ||
            operand->index_value_exceeds_int[0] ||
            operand->index_values[0] > UINT32_MAX ||
            operand->register_index < 0 ||
            (uint64_t)(uint32_t)operand->register_index !=
                operand->index_values[0]) {
            return false;
        }
        *register_id = (uint32_t)operand->index_values[0];
    }
    *mask = declaration_mask;
    *operand_type = operand->type;
    *has_signature_register = true;
    return true;
}

static bool signature_declaration_matches_element(
    const USILSignatureDeclaration* declaration,
    const DXBCSignatureElement* element, DXBCSignatureRole role) {
    if (!declaration || !element) {
        return false;
    }
    if (!declaration->has_signature_register) {
        const uint32_t special_system_value =
            signature_system_value_from_special_output_operand(
                declaration->operand_type);
        const char* special_semantic =
            signature_semantic_from_special_output_operand(
                declaration->operand_type);
        return role == DXBC_SIGNATURE_ROLE_OUTPUT &&
               signature_declaration_kind_is_output(declaration->kind) &&
               declaration->stream_index == element->stream_index &&
               special_system_value != UINT32_MAX &&
               (element->system_value == special_system_value ||
                (element->system_value == 0u && special_semantic &&
                 signature_semantic_equals(
                     dxbc_signature_semantic_name(element),
                     special_semantic))) &&
               element->register_id == UINT32_MAX;
    }
    if (declaration->register_id != element->register_id ||
        (element->mask & declaration->mask) == 0u) {
        return false;
    }
    if (role == DXBC_SIGNATURE_ROLE_INPUT) {
        if (!signature_declaration_kind_is_input(declaration->kind) ||
            declaration->stream_index != 0u) return false;
    } else if (role == DXBC_SIGNATURE_ROLE_OUTPUT) {
        if (!signature_declaration_kind_is_output(declaration->kind) ||
            declaration->stream_index != element->stream_index) return false;
    } else {
        /* Hull fork/join phase outputs and domain patch inputs share PCSG.
         * Operand type, rather than a guessed stage phase, identifies them. */
        if (declaration->operand_type !=
                OPERAND_TYPE_INPUT_PATCH_CONSTANT &&
            !signature_declaration_kind_is_output(declaration->kind)) {
            return false;
        }
    }

    if (declaration->has_system_value) {
        return signature_system_value_from_name_token(
                   declaration->system_value_name) ==
                   element->system_value &&
               (!declaration->has_interpolation ||
                declaration->interpolation_mode ==
                    element->interpolation_mode);
    }
    const bool plain_special_output =
        role != DXBC_SIGNATURE_ROLE_INPUT && element->system_value >= 64u &&
        element->system_value <= 70u;
    return (element->system_value == 0u || plain_special_output) &&
           (!declaration->has_interpolation ||
            declaration->interpolation_mode ==
                element->interpolation_mode);
}

static bool signature_declaration_is_valid(
    const USILSignatureDeclaration* declaration) {
    if (!declaration ||
        (!signature_declaration_kind_is_input(declaration->kind) &&
         !signature_declaration_kind_is_output(declaration->kind)) ||
        (declaration->mask & 0xf0u) != 0u ||
        (declaration->has_signature_register && declaration->mask == 0u) ||
        (!declaration->has_signature_register &&
         declaration->register_id != UINT32_MAX) ||
        declaration->stream_index > 3u ||
        declaration->has_system_value !=
            signature_declaration_kind_has_system_value(declaration->kind) ||
        declaration->has_interpolation !=
            signature_declaration_kind_has_interpolation(declaration->kind) ||
        (declaration->has_system_value &&
         signature_system_value_from_name_token(
             declaration->system_value_name) == UINT32_MAX) ||
        (!declaration->has_system_value &&
         declaration->system_value_name != 0u) ||
        (declaration->has_interpolation &&
         declaration->interpolation_mode > 7u) ||
        (!declaration->has_interpolation &&
         declaration->interpolation_mode != 0u) ||
        (declaration->has_array_element_count &&
         (declaration->array_element_count == 0u ||
          declaration->array_element_count > 32u ||
          (declaration->operand_type != OPERAND_TYPE_INPUT &&
           declaration->operand_type != OPERAND_TYPE_INPUT_CONTROL_POINT &&
           declaration->operand_type !=
               OPERAND_TYPE_OUTPUT_CONTROL_POINT))) ||
        (!declaration->has_array_element_count &&
         declaration->array_element_count != 0u)) {
        return false;
    }
    if (!declaration->has_signature_register) {
        if (declaration->has_system_value ||
            declaration->has_interpolation) {
            return false;
        }
        if (signature_declaration_kind_is_output(declaration->kind)) {
            return signature_system_value_from_special_output_operand(
                       declaration->operand_type) != UINT32_MAX;
        }
        return signature_declaration_kind_is_input(declaration->kind) &&
               signature_input_builtin_mask_is_valid(
                   declaration->operand_type, declaration->mask);
    }
    if (signature_declaration_kind_is_input(declaration->kind)) {
        return declaration->operand_type == OPERAND_TYPE_INPUT ||
               declaration->operand_type ==
                   OPERAND_TYPE_INPUT_CONTROL_POINT ||
               declaration->operand_type ==
                   OPERAND_TYPE_INPUT_PATCH_CONSTANT;
    }
    return declaration->operand_type == OPERAND_TYPE_OUTPUT ||
           declaration->operand_type ==
               OPERAND_TYPE_OUTPUT_CONTROL_POINT ||
           declaration->operand_type == OPERAND_TYPE_OUTPUT_DEPTH ||
           declaration->operand_type ==
               OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL ||
           declaration->operand_type ==
               OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL ||
           declaration->operand_type ==
               OPERAND_TYPE_OUTPUT_COVERAGE_MASK ||
           declaration->operand_type == OPERAND_TYPE_OUTPUT_STENCIL_REF;
}

static bool append_signature_declaration(
    USILProgram* program, const DXBCInstruction* instruction,
    uint32_t source_instruction_index, uint8_t stream_index) {
    if (!program || !instruction || instruction->opcode < 95u ||
        instruction->opcode > 103u ||
        program->signature_declaration_count == INT_MAX ||
        !reserve_usil_array(
            (void**)&program->signature_declarations,
            &program->signature_declaration_alloc,
            program->signature_declaration_count,
            program->signature_declaration_count + 1,
            sizeof(*program->signature_declarations))) {
        return false;
    }
    USILSignatureDeclaration* declaration =
        &program->signature_declarations[
            program->signature_declaration_count];
    memset(declaration, 0, sizeof(*declaration));
    declaration->kind =
        (USILSignatureDeclarationKind)instruction->opcode;
    declaration->has_system_value =
        instruction->has_declaration_system_value;
    declaration->system_value_name =
        instruction->declaration_system_value;
    declaration->has_interpolation =
        instruction->has_declaration_interpolation;
    declaration->interpolation_mode =
        instruction->declaration_interpolation;
    declaration->source_instruction_index = source_instruction_index;
    declaration->stream_index =
        signature_declaration_kind_is_output(declaration->kind)
            ? stream_index
            : 0u;
    if (!signature_declaration_operand(
            instruction, &declaration->register_id, &declaration->mask,
            &declaration->operand_type,
            &declaration->has_signature_register) ||
        !signature_declaration_is_valid(declaration)) {
        if (getenv("DXBC_DEBUG_STAGE")) {
            const DXBCOperand* operand = &instruction->operands[0];
            fprintf(stderr,
                    "[usil] signature dcl op=%u operands=%d type=%d "
                    "reg=%d dim=%d idx=%llu/%llu mask=0x%02x "
                    "sys=%d/%u interp=%d/%u\n",
                    instruction->opcode, instruction->operand_count,
                    operand->type, operand->register_index,
                    operand->register_index_dim,
                    (unsigned long long)operand->index_values[0],
                    (unsigned long long)operand->index_values[1],
                    operand->destination_mask,
                    instruction->has_declaration_system_value,
                    instruction->declaration_system_value,
                    instruction->has_declaration_interpolation,
                    instruction->declaration_interpolation);
        }
        memset(declaration, 0, sizeof(*declaration));
        return false;
    }
    if ((instruction->operands[0].type == OPERAND_TYPE_INPUT ||
         instruction->operands[0].type ==
             OPERAND_TYPE_INPUT_CONTROL_POINT ||
         instruction->operands[0].type ==
             OPERAND_TYPE_OUTPUT_CONTROL_POINT) &&
        instruction->operands[0].register_index_dim == 2) {
        declaration->has_array_element_count = true;
        declaration->array_element_count =
            (uint8_t)instruction->operands[0].index_values[0];
    }
    if (!signature_declaration_is_valid(declaration)) {
        memset(declaration, 0, sizeof(*declaration));
        return false;
    }

    /* Interpolation is declaration authority, absent from ISGN/ISG1. Bind it
     * only to signature elements completely covered by this declaration. */
    if (declaration->has_interpolation) {
        for (int index = 0; index < program->input_count; ++index) {
            DXBCSignatureElement* element = &program->inputs[index];
            if (declaration->register_id == element->register_id &&
                (element->mask & declaration->mask) != 0u &&
                (!declaration->has_system_value ||
                 signature_system_value_from_name_token(
                     declaration->system_value_name) ==
                     element->system_value)) {
                element->interpolation_mode =
                    declaration->interpolation_mode;
            }
        }
    }
    ++program->signature_declaration_count;
    return true;
}

static bool signature_stream_declaration_id(
    const DXBCInstruction* instruction, uint8_t* stream_id) {
    if (!instruction || !stream_id || instruction->opcode != 143u ||
        instruction->operand_count != 1) return false;
    const DXBCOperand* operand = &instruction->operands[0];
    if (operand->type != OPERAND_TYPE_STREAM ||
        operand->register_index_dim != 1 ||
        !operand->index_has_immediate[0] ||
        operand->index_representations[0] != 0u ||
        operand->index_value_exceeds_int[0] ||
        operand->index_values[0] > 3u || operand->rel_op0 ||
        operand->rel_op1 || operand->rel_op2 ||
        operand->extended_token_count != 0 || operand->extended_tokens ||
        operand->has_abs || operand->has_neg || operand->min_precision != 0u) {
        return false;
    }
    *stream_id = (uint8_t)operand->index_values[0];
    return true;
}

bool usil_signature_authority_is_valid(const USILProgram* program) {
    if (!program || program->input_count < 0 ||
        program->input_alloc < program->input_count ||
        (program->input_alloc == 0) != (program->inputs == NULL) ||
        program->output_count < 0 ||
        program->output_alloc < program->output_count ||
        (program->output_alloc == 0) != (program->outputs == NULL) ||
        program->patch_constant_count < 0 ||
        program->patch_constant_alloc < program->patch_constant_count ||
        (program->patch_constant_alloc == 0) !=
            (program->patch_constants == NULL) ||
        program->signature_declaration_count < 0 ||
        program->signature_declaration_alloc <
            program->signature_declaration_count ||
        (program->signature_declaration_alloc == 0) !=
            (program->signature_declarations == NULL)) {
        return false;
    }
    for (int index = 0; index < program->input_count; ++index) {
        if (!dxbc_signature_element_is_valid(
                &program->inputs[index], DXBC_SIGNATURE_ROLE_INPUT)) {
            return false;
        }
    }
    for (int index = 0; index < program->output_count; ++index) {
        if (!dxbc_signature_element_is_valid(
                &program->outputs[index], DXBC_SIGNATURE_ROLE_OUTPUT)) {
            return false;
        }
    }
    for (int index = 0; index < program->patch_constant_count; ++index) {
        if (!dxbc_signature_element_is_valid(
                &program->patch_constants[index],
                DXBC_SIGNATURE_ROLE_PATCH_CONSTANT)) {
            return false;
        }
    }
    for (int index = 0; index < program->signature_declaration_count;
         ++index) {
        const USILSignatureDeclaration* declaration =
            &program->signature_declarations[index];
        if (!signature_declaration_is_valid(declaration) ||
            (index > 0 && declaration->source_instruction_index <=
                              program->signature_declarations[index - 1]
                                  .source_instruction_index) ||
            (declaration->has_array_element_count &&
             ((declaration->operand_type == OPERAND_TYPE_INPUT &&
               (!program->geometry.valid ||
                declaration->array_element_count !=
                    program->geometry.input_vertex_count)) ||
              (declaration->operand_type ==
                   OPERAND_TYPE_INPUT_CONTROL_POINT &&
               (!program->tessellation.valid ||
                declaration->array_element_count !=
                    program->tessellation.input_control_point_count)) ||
              (declaration->operand_type ==
                   OPERAND_TYPE_OUTPUT_CONTROL_POINT &&
               (!program->tessellation.valid ||
                declaration->array_element_count !=
                    program->tessellation.output_control_point_count))))) {
            return false;
        }
        if (!declaration->has_signature_register) {
            bool stage_matches = false;
            const uint32_t special_output_system_value =
                signature_system_value_from_special_output_operand(
                    declaration->operand_type);
            if (special_output_system_value != UINT32_MAX) {
                stage_matches =
                    program->program_type == DXBC_PROGRAM_TYPE_PIXEL;
            }
            switch (declaration->operand_type) {
                case OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID:
                case OPERAND_TYPE_FORK_INSTANCE_ID:
                case OPERAND_TYPE_JOIN_INSTANCE_ID:
                    stage_matches =
                        program->program_type == DXBC_PROGRAM_TYPE_HULL;
                    break;
                case OPERAND_TYPE_DOMAIN_LOCATION:
                    stage_matches =
                        program->program_type == DXBC_PROGRAM_TYPE_DOMAIN &&
                        program->tessellation.valid &&
                        declaration->mask ==
                            (program->tessellation.domain ==
                                     DXBC_TESSELLATOR_DOMAIN_TRIANGLE
                                 ? 0x07u
                                 : 0x03u);
                    break;
                case OPERAND_TYPE_INPUT_PRIMITIVE_ID:
                case OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
                    stage_matches =
                        program->program_type == DXBC_PROGRAM_TYPE_GEOMETRY;
                    break;
                case OPERAND_TYPE_INPUT_COVERAGE_MASK:
                case OPERAND_TYPE_INNER_COVERAGE:
                    stage_matches =
                        program->program_type == DXBC_PROGRAM_TYPE_PIXEL;
                    break;
                case OPERAND_TYPE_INPUT_THREAD_ID:
                case OPERAND_TYPE_INPUT_THREAD_GROUP_ID:
                case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP:
                case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED:
                    stage_matches =
                        program->program_type == DXBC_PROGRAM_TYPE_COMPUTE;
                    break;
                default:
                    break;
            }
            if (!stage_matches) return false;
            if (special_output_system_value == UINT32_MAX) continue;
        }
        bool matched = false;
        uint8_t matched_signature_mask = 0u;
        for (int signature = 0; signature < program->input_count; ++signature)
            if (signature_declaration_matches_element(
                    declaration, &program->inputs[signature],
                    DXBC_SIGNATURE_ROLE_INPUT)) {
                matched = true;
                matched_signature_mask |= program->inputs[signature].mask;
            }
        for (int signature = 0; signature < program->output_count;
             ++signature)
            if (signature_declaration_matches_element(
                    declaration, &program->outputs[signature],
                    DXBC_SIGNATURE_ROLE_OUTPUT)) {
                matched = true;
                matched_signature_mask |= program->outputs[signature].mask;
            }
        for (int signature = 0; signature < program->patch_constant_count;
             ++signature)
            if (signature_declaration_matches_element(
                    declaration, &program->patch_constants[signature],
                    DXBC_SIGNATURE_ROLE_PATCH_CONSTANT)) {
                matched = true;
                matched_signature_mask |=
                    program->patch_constants[signature].mask;
            }
        if (program->has_parsed_signature_authority &&
            (!matched ||
             (declaration->mask & (uint8_t)~matched_signature_mask) != 0u)) {
            return false;
        }
    }

    if (!program->has_parsed_signature_authority) return true;
#define REQUIRE_SIGNATURE_COVERAGE(list, count, role, required_expression)     \
    do {                                                                       \
        for (int signature = 0; signature < (count); ++signature) {            \
            uint8_t covered_mask = 0u;                                          \
            for (int declaration = 0;                                          \
                 declaration < program->signature_declaration_count;           \
                 ++declaration) {                                               \
                if (signature_declaration_matches_element(                     \
                        &program->signature_declarations[declaration],          \
                        &(list)[signature], (role))) {                          \
                    covered_mask |=                                             \
                        program->signature_declarations[declaration]            \
                                .has_signature_register                         \
                            ? program->signature_declarations[declaration].mask  \
                            : (list)[signature].mask;                            \
                }                                                               \
            }                                                                   \
            const uint8_t required_mask = (required_expression);                \
            if ((required_mask & (uint8_t)~covered_mask) != 0u) return false;   \
        }                                                                       \
    } while (0)
    const bool tessellation_stage =
        program->program_type == DXBC_PROGRAM_TYPE_HULL ||
        program->program_type == DXBC_PROGRAM_TYPE_DOMAIN;
    if (!tessellation_stage) {
        REQUIRE_SIGNATURE_COVERAGE(program->inputs, program->input_count,
                                   DXBC_SIGNATURE_ROLE_INPUT,
                                   program->inputs[signature].rw_mask);
        REQUIRE_SIGNATURE_COVERAGE(
            program->outputs, program->output_count,
            DXBC_SIGNATURE_ROLE_OUTPUT,
            (uint8_t)(program->outputs[signature].mask &
                      (uint8_t)~program->outputs[signature].rw_mask));
    }
    /* A domain shader's PCSG describes the fixed-function tessellator link,
     * including factors the domain bytecode need not read or declare.  Hull
     * output declarations do provide direct patch-signature authority. */
    if (program->program_type != DXBC_PROGRAM_TYPE_DOMAIN) {
        REQUIRE_SIGNATURE_COVERAGE(
            program->patch_constants, program->patch_constant_count,
            DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
            (uint8_t)(program->patch_constants[signature].mask &
                      (uint8_t)~program->patch_constants[signature].rw_mask));
    }
#undef REQUIRE_SIGNATURE_COVERAGE
    return true;
}

static bool map_dxbc_opcode(uint32_t opcode, USILOpcode* mapped) {
    if (!mapped) return false;
    switch (opcode) {
        case 0: *mapped = USIL_OP_ADD; break;
        case 1: *mapped = USIL_OP_AND; break;
        case 2: *mapped = USIL_OP_BREAK; break;
        case 3: *mapped = USIL_OP_BREAKC; break;
        case 6: *mapped = USIL_OP_CASE; break;
        case 7: *mapped = USIL_OP_CONTINUE; break;
        case 8: *mapped = USIL_OP_CONTINUEC; break;
        case 10: *mapped = USIL_OP_DEFAULT; break;
        case 11: *mapped = USIL_OP_DERIV_RTX; break;
        case 12: *mapped = USIL_OP_DERIV_RTY; break;
        case 13: *mapped = USIL_OP_DISCARD; break;
        case 14: *mapped = USIL_OP_DIV; break;
        case 15: *mapped = USIL_OP_DP2; break;
        case 16: *mapped = USIL_OP_DP3; break;
        case 17: *mapped = USIL_OP_DP4; break;
        case 18: *mapped = USIL_OP_ELSE; break;
        case 21: *mapped = USIL_OP_ENDIF; break;
        case 22: *mapped = USIL_OP_ENDLOOP; break;
        case 23: *mapped = USIL_OP_ENDSWITCH; break;
        case 24: *mapped = USIL_OP_EQ; break;
        case 25: *mapped = USIL_OP_EXP; break;
        case 26: *mapped = USIL_OP_FRC; break;
        case 27: *mapped = USIL_OP_FTOI; break;
        case 28: *mapped = USIL_OP_FTOU; break;
        case 29: *mapped = USIL_OP_GE; break;
        case 30: *mapped = USIL_OP_IADD; break;
        case 31: *mapped = USIL_OP_IF; break;
        case 32: *mapped = USIL_OP_IEQ; break;
        case 33: *mapped = USIL_OP_IGE; break;
        case 34: *mapped = USIL_OP_ILT; break;
        case 35: *mapped = USIL_OP_IMAD; break;
        case 36: *mapped = USIL_OP_IMAX; break;
        case 37: *mapped = USIL_OP_IMIN; break;
        case 38: *mapped = USIL_OP_IMUL; break;
        case 39: *mapped = USIL_OP_INE; break;
        case 40: *mapped = USIL_OP_INEG; break;
        case 41: *mapped = USIL_OP_ISHL; break;
        case 42: *mapped = USIL_OP_ISHR; break;
        case 43: *mapped = USIL_OP_ITOF; break;
        case 45: *mapped = USIL_OP_LD; break;
        case 46: *mapped = USIL_OP_LD_MS; break;
        case 47: *mapped = USIL_OP_LOG; break;
        case 48: *mapped = USIL_OP_LOOP; break;
        case 49: *mapped = USIL_OP_LT; break;
        case 50: *mapped = USIL_OP_MAD; break;
        case 51: *mapped = USIL_OP_MIN; break;
        case 52: *mapped = USIL_OP_MAX; break;
        case 54: *mapped = USIL_OP_MOV; break;
        case 55: *mapped = USIL_OP_MOVC; break;
        case 56: *mapped = USIL_OP_MUL; break;
        case 57: *mapped = USIL_OP_NE; break;
        case 58: *mapped = USIL_OP_NOP; break;
        case 59: *mapped = USIL_OP_NOT; break;
        case 60: *mapped = USIL_OP_OR; break;
        case 61: *mapped = USIL_OP_RESINFO; break;
        case 62: *mapped = USIL_OP_RET; break;
        case 64: *mapped = USIL_OP_ROUND_NE; break;
        case 65: *mapped = USIL_OP_ROUND_NI; break;
        case 66: *mapped = USIL_OP_ROUND_PI; break;
        case 67: *mapped = USIL_OP_ROUND_Z; break;
        case 68: *mapped = USIL_OP_RSQ; break;
        case 69: *mapped = USIL_OP_SAMPLE; break;
        case 70: *mapped = USIL_OP_SAMPLE_C; break;
        case 71: *mapped = USIL_OP_SAMPLE_C_LZ; break;
        case 72: *mapped = USIL_OP_SAMPLE_L; break;
        case 73: *mapped = USIL_OP_SAMPLE_D; break;
        case 74: *mapped = USIL_OP_SAMPLE_B; break;
        case 75: *mapped = USIL_OP_SQRT; break;
        case 76: *mapped = USIL_OP_SWITCH; break;
        case 77: *mapped = USIL_OP_SINCOS; break;
        case 78: *mapped = USIL_OP_UDIV; break;
        case 79: *mapped = USIL_OP_ULT; break;
        case 80: *mapped = USIL_OP_UGE; break;
        case 83: *mapped = USIL_OP_UMAX; break;
        case 84: *mapped = USIL_OP_UMIN; break;
        case 85: *mapped = USIL_OP_USHR; break;
        case 86: *mapped = USIL_OP_UTOF; break;
        case 87: *mapped = USIL_OP_XOR; break;
        case 111: *mapped = USIL_OP_SAMPLEINFO; break;
        case 122: *mapped = USIL_OP_DERIV_RTX_COARSE; break;
        case 123: *mapped = USIL_OP_DERIV_RTX_FINE; break;
        case 124: *mapped = USIL_OP_DERIV_RTY_COARSE; break;
        case 125: *mapped = USIL_OP_DERIV_RTY_FINE; break;
        case 129: *mapped = USIL_OP_RCP; break;
        case 138: *mapped = USIL_OP_UBFE; break;
        case 167: *mapped = USIL_OP_LD_STRUCTURED; break;
        case 180: *mapped = USIL_OP_IMM_ATOMIC_IADD; break;
        case 9: *mapped = USIL_OP_GEOMETRY_RESTART_STRIP; break;
        case 19: *mapped = USIL_OP_GEOMETRY_APPEND; break;
        case 117: *mapped = USIL_OP_GEOMETRY_APPEND; break;
        case 118: *mapped = USIL_OP_GEOMETRY_RESTART_STRIP; break;
        default: return false;
    }
    return true;
}

static const char *resource_dimension_from_code(uint32_t dimension) {
    switch (dimension) {
        case 1: return "buffer";
        case 2: return "1d";
        case 3: return "2d";
        case 4: return "2dms";
        case 5: return "3d";
        case 6: return "cube";
        case 7: return "1darray";
        case 8: return "2darray";
        case 9: return "2dmsarray";
        case 10: return "cubearray";
        case 11: return "raw";
        case 12: return "structured";
        default: return NULL;
    }
}

static const char *resource_dimension_from_declaration(
    const DXBCResourceDecl *resource) {
    if (!resource || !resource->declared) return NULL;
    if (resource->is_structured && resource->dimension != 12) return NULL;
    if (resource->dimension == 12 && !resource->is_structured) return NULL;
    return resource_dimension_from_code(resource->dimension);
}

static const char *instruction_resource_dimension_from_binding(
    const DXBCContainer *container, const DXBCInstruction *instruction) {
    const DXBCResourceDecl *declaration = NULL;
    for (int index = 0; index < instruction->operand_count; ++index) {
        const DXBCOperand *operand = &instruction->operands[index];
        const DXBCResourceDecl *candidate = NULL;
        if (operand->type == OPERAND_TYPE_RESOURCE) {
            candidate = dxbc_find_resource_declaration(
                container, operand->register_index, false);
        } else if (operand->type == OPERAND_TYPE_UAV) {
            candidate = dxbc_find_resource_declaration(
                container, operand->register_index, true);
        } else {
            continue;
        }
        if (!candidate) return NULL;
        if (declaration && declaration != candidate) return NULL;
        declaration = candidate;
    }
    if (!declaration) return "";
    return resource_dimension_from_declaration(declaration);
}

static void check_operand_temp_usage(const DXBCOperand* op, int* max_temp_idx) {
    if (!op) return;
    if (op->type == OPERAND_TYPE_TEMP) {
        if (op->register_index > *max_temp_idx) {
            *max_temp_idx = op->register_index;
        }
    }
    check_operand_temp_usage(op->rel_op0, max_temp_idx);
    check_operand_temp_usage(op->rel_op1, max_temp_idx);
    check_operand_temp_usage(op->rel_op2, max_temp_idx);
}

static bool resource_declaration_list_valid(
    const DXBCResourceDecl* declarations, int count) {
    for (int index = 0; index < count; ++index) {
        if (!declarations[index].declared ||
            declarations[index].register_index < 0 ||
            (index > 0 && declarations[index - 1].register_index >=
                              declarations[index].register_index)) {
            return false;
        }
    }
    return true;
}

static uint32_t geometry_input_vertex_count(DXBCInputPrimitive primitive) {
    switch (primitive) {
        case DXBC_INPUT_PRIMITIVE_POINT: return 1u;
        case DXBC_INPUT_PRIMITIVE_LINE: return 2u;
        case DXBC_INPUT_PRIMITIVE_TRIANGLE: return 3u;
        case DXBC_INPUT_PRIMITIVE_LINE_ADJACENCY: return 4u;
        case DXBC_INPUT_PRIMITIVE_TRIANGLE_ADJACENCY: return 6u;
        default: return 0u;
    }
}

static bool initialize_stage_contract(
    USILProgram* program, const DXBCContainer* container,
    const DXBCStageContract* contract) {
    program->program_type = DXBC_PROGRAM_TYPE_INVALID;
    if (!contract) return true;

    DXBCStageContractDiagnostic diagnostic;
    if (!dxbc_stage_contract_validate_container(contract, container,
                                                &diagnostic)) {
        LOG_ERROR("DXBC stage contract and semantic projection disagree: %s",
                  dxbc_stage_contract_status_name(diagnostic.status));
        return false;
    }
    program->has_stage_contract = true;
    program->program_type = contract->program_type;
    program->shader_model_major = contract->shader_model_major;
    program->shader_model_minor = contract->shader_model_minor;
    if (contract->program_type == DXBC_PROGRAM_TYPE_HULL ||
        contract->program_type == DXBC_PROGRAM_TYPE_DOMAIN) {
        if (!contract->has_input_control_point_count ||
            !contract->has_tessellator_domain ||
            contract->input_control_point_count == 0u ||
            contract->input_control_point_count > 32u) {
            return false;
        }
        if (contract->program_type == DXBC_PROGRAM_TYPE_HULL &&
            (!contract->has_output_control_point_count ||
             !contract->has_tessellator_partitioning ||
             !contract->has_tessellator_output_primitive ||
             contract->output_control_point_count == 0u ||
             contract->output_control_point_count > 32u ||
             contract->hull_phase_count == 0u)) {
            return false;
        }
        program->tessellation.valid = true;
        program->tessellation.input_control_point_count =
            contract->input_control_point_count;
        program->tessellation.output_control_point_count =
            contract->has_output_control_point_count
                ? contract->output_control_point_count
                : 0u;
        program->tessellation.domain = contract->tessellator_domain;
        program->tessellation.partitioning =
            contract->has_tessellator_partitioning
                ? contract->tessellator_partitioning
                : DXBC_TESSELLATOR_PARTITIONING_UNDEFINED;
        program->tessellation.output_primitive =
            contract->has_tessellator_output_primitive
                ? contract->tessellator_output_primitive
                : DXBC_TESSELLATOR_OUTPUT_UNDEFINED;
        program->tessellation.has_max_tessellation_factor =
            contract->has_max_tessellation_factor;
        program->tessellation.max_tessellation_factor_bits =
            contract->max_tessellation_factor_bits;
        if (contract->hull_phase_count > 0u) {
            if (contract->hull_phase_count >
                SIZE_MAX / sizeof(*program->tessellation.phases)) {
                return false;
            }
            const size_t bytes = contract->hull_phase_count *
                                 sizeof(*program->tessellation.phases);
            program->tessellation.phases =
                (USILHullPhase*)mem_alloc(bytes);
            if (!program->tessellation.phases) return false;
            memset(program->tessellation.phases, 0, bytes);
            program->tessellation.phase_count = contract->hull_phase_count;
            program->tessellation.phase_capacity =
                contract->hull_phase_count;
            for (size_t index = 0; index < contract->hull_phase_count;
                 ++index) {
                const DXBCHullPhaseContract* source =
                    &contract->hull_phases[index];
                USILHullPhase* destination =
                    &program->tessellation.phases[index];
                destination->kind = source->kind;
                destination->marker_source_instruction_index =
                    source->marker_instruction_index;
                destination->first_source_instruction_index =
                    source->first_instruction_index;
                destination->end_source_instruction_index =
                    source->end_instruction_index;
                destination->first_instruction_index = -1;
                destination->end_instruction_index = -1;
                destination->instance_count_declared =
                    source->instance_count_declared;
                destination->instance_count = source->instance_count;
            }
        }
        return true;
    }
    if (contract->program_type != DXBC_PROGRAM_TYPE_GEOMETRY) return true;

    const uint32_t vertex_count =
        geometry_input_vertex_count(contract->input_primitive);
    if (!contract->has_input_primitive || !contract->has_output_topology ||
        !contract->has_max_output_vertex_count || vertex_count == 0u ||
        contract->max_output_vertex_count == 0u ||
        contract->max_output_vertex_count > 1024u ||
        contract->geometry_effect_count > (size_t)INT_MAX) {
        return false;
    }
    program->geometry.valid = true;
    program->geometry.input_primitive = contract->input_primitive;
    program->geometry.output_topology = contract->output_topology;
    program->geometry.input_vertex_count = vertex_count;
    program->geometry.max_output_vertex_count =
        contract->max_output_vertex_count;
    program->geometry.has_instance_count =
        contract->has_geometry_instance_count;
    program->geometry.instance_count =
        contract->has_geometry_instance_count
            ? contract->geometry_instance_count
            : 1u;
    program->geometry.declared_stream_mask =
        contract->declared_stream_mask;
    program->geometry.referenced_stream_mask =
        contract->referenced_stream_mask;
    program->geometry.effect_count = contract->geometry_effect_count;
    program->geometry.output_tuple_state_persists = true;
    return true;
}

static bool geometry_effect_matches(
    const DXBCInstruction* source, size_t semantic_instruction_index,
    const DXBCStageContract* contract, size_t* effect_index,
    USILInstruction* destination) {
    const bool is_effect = source->opcode == 9u || source->opcode == 19u ||
                           source->opcode == 117u || source->opcode == 118u;
    if (!is_effect) return true;
    if (!contract || contract->program_type != DXBC_PROGRAM_TYPE_GEOMETRY ||
        !effect_index || *effect_index >= contract->geometry_effect_count) {
        return false;
    }
    const DXBCGeometryEffectContract* effect =
        &contract->geometry_effects[*effect_index];
    const USILGeometryEffectKind expected_kind =
        source->opcode == 19u || source->opcode == 117u
            ? USIL_GEOMETRY_EFFECT_APPEND
            : USIL_GEOMETRY_EFFECT_RESTART_STRIP;
    const bool expected_explicit =
        source->opcode == 117u || source->opcode == 118u;
    if (effect->instruction_index != semantic_instruction_index ||
        effect->kind !=
            (expected_kind == USIL_GEOMETRY_EFFECT_APPEND
                 ? DXBC_GEOMETRY_EFFECT_APPEND
                 : DXBC_GEOMETRY_EFFECT_RESTART_STRIP) ||
        effect->explicit_stream != expected_explicit) {
        return false;
    }
    uint8_t semantic_stream = 0u;
    if (expected_explicit) {
        if (source->operand_count != 1 ||
            source->operands[0].type != OPERAND_TYPE_STREAM ||
            source->operands[0].register_index_dim != 1 ||
            !source->operands[0].index_has_immediate[0] ||
            source->operands[0].rel_op0 ||
            source->operands[0].index_values[0] > 3u) {
            return false;
        }
        semantic_stream = (uint8_t)source->operands[0].index_values[0];
    } else if (source->operand_count != 0) {
        return false;
    }
    if (effect->stream_id != semantic_stream) return false;
    destination->geometry_effect = expected_kind;
    destination->geometry_stream_id = semantic_stream;
    destination->geometry_stream_explicit = expected_explicit;
    ++*effect_index;
    return true;
}

static bool usil_translate_internal(
    USILProgram* program, const DXBCContainer* container,
    const DXBCStageContract* stage_contract) {
    if (!program) return false;
    memset(program, 0, sizeof(USILProgram));
    if (!container || container->input_signature_count < 0 ||
        container->input_signature_alloc < container->input_signature_count ||
        (container->input_signature_alloc == 0) !=
            (container->input_signature == NULL) ||
        container->output_signature_count < 0 ||
        container->output_signature_alloc < container->output_signature_count ||
        (container->output_signature_alloc == 0) !=
            (container->output_signature == NULL) ||
        container->patch_constant_signature_count < 0 ||
        container->patch_constant_signature_alloc <
            container->patch_constant_signature_count ||
        (container->patch_constant_signature_alloc == 0) !=
            (container->patch_constant_signature == NULL) ||
        container->resource_count < 0 ||
        container->resource_alloc < container->resource_count ||
        (container->resource_alloc == 0) != (container->resources == NULL) ||
        container->uav_count < 0 ||
        container->uav_alloc < container->uav_count ||
        (container->uav_alloc == 0) != (container->uavs == NULL) ||
        container->instruction_count < 0 ||
        container->instruction_alloc < container->instruction_count ||
        (container->instruction_alloc == 0) !=
            (container->instructions == NULL) ||
        (container->instruction_count > 0 && !container->instructions) ||
        container->icb_value_count < 0 ||
        container->icb_value_alloc < container->icb_value_count ||
        (container->icb_value_alloc == 0) !=
            (container->icb_values == NULL)) {
        LOG_ERROR("Invalid DXBC container passed to USIL translation");
        return false;
    }
    if (!resource_declaration_list_valid(container->resources,
                                         container->resource_count) ||
        !resource_declaration_list_valid(container->uavs,
                                         container->uav_count)) {
        LOG_ERROR("Invalid DXBC resource declaration list");
        return false;
    }
    memcpy(program->shader_type_model, container->shader_type_model,
           sizeof(program->shader_type_model));
    program->has_parsed_signature_authority =
        container->parsed_signature_authority;
    if (!initialize_stage_contract(program, container, stage_contract)) {
        goto fail;
    }
    
    // Copy signatures
    if (!reserve_usil_array((void**)&program->inputs, &program->input_alloc,
                            program->input_count,
                            container->input_signature_count,
                            sizeof(*program->inputs)) ||
        !reserve_usil_array((void**)&program->outputs, &program->output_alloc,
                            program->output_count,
                            container->output_signature_count,
                            sizeof(*program->outputs)) ||
        !reserve_usil_array(
            (void**)&program->patch_constants,
            &program->patch_constant_alloc, program->patch_constant_count,
            container->patch_constant_signature_count,
            sizeof(*program->patch_constants))) {
        goto fail;
    }
    if (container->input_signature_count > 0) {
        if (!clone_signature_elements(program->inputs,
                                      container->input_signature,
                                      container->input_signature_count)) {
            goto fail;
        }
    }
    program->input_count = container->input_signature_count;
    if (container->output_signature_count > 0) {
        if (!clone_signature_elements(program->outputs,
                                      container->output_signature,
                                      container->output_signature_count)) {
            goto fail;
        }
    }
    program->output_count = container->output_signature_count;
    if (container->patch_constant_signature_count > 0) {
        if (!clone_signature_elements(
                program->patch_constants,
                container->patch_constant_signature,
                container->patch_constant_signature_count)) {
            goto fail;
        }
    }
    program->patch_constant_count =
        container->patch_constant_signature_count;

    // Copy textures/resources
    for (int i = 0; i < container->resource_count; i++) {
        if (container->resources[i].declared) {
            if (program->texture_count == INT_MAX ||
                !reserve_usil_array(
                    (void**)&program->textures, &program->texture_alloc,
                    program->texture_count, program->texture_count + 1,
                    sizeof(*program->textures))) goto fail;
            program->textures[program->texture_count].reg_idx =
                container->resources[i].register_index;
            
            const char* dim = resource_dimension_from_declaration(
                &container->resources[i]);
            if (!dim) {
                LOG_ERROR("Unsupported DXBC resource dimension '%s' at t%d",
                          container->resources[i].dim_name,
                          container->resources[i].register_index);
                goto fail;
            }
            
            strncpy(program->textures[program->texture_count].dimension, dim, 15);
            program->textures[program->texture_count].dimension[15] = '\0';
            program->textures[program->texture_count].stride =
                container->resources[i].stride;
            memcpy(program->textures[program->texture_count].return_types,
                   container->resources[i].return_types,
                   sizeof(program->textures[program->texture_count].return_types));
            program->textures[program->texture_count].sample_count =
                container->resources[i].sample_count;
            program->texture_count++;
        }
    }

    // Copy UAVs
    for (int i = 0; i < container->uav_count; i++) {
        if (container->uavs[i].declared) {
            if (program->uav_count == INT_MAX ||
                !reserve_usil_array(
                    (void**)&program->uavs, &program->uav_alloc,
                    program->uav_count, program->uav_count + 1,
                    sizeof(*program->uavs))) goto fail;
            program->uavs[program->uav_count].reg_idx =
                container->uavs[i].register_index;
            const char* dim = resource_dimension_from_declaration(
                &container->uavs[i]);
            if (!dim) {
                LOG_ERROR("Unsupported DXBC UAV dimension '%s' at u%d",
                          container->uavs[i].dim_name,
                          container->uavs[i].register_index);
                goto fail;
            }
            strncpy(program->uavs[program->uav_count].dimension, dim, 15);
            program->uavs[program->uav_count].dimension[15] = '\0';
            memcpy(program->uavs[program->uav_count].return_types,
                   container->uavs[i].return_types,
                   sizeof(program->uavs[program->uav_count].return_types));
            program->uavs[program->uav_count].sample_count =
                container->uavs[i].sample_count;
            program->uavs[program->uav_count].stride = container->uavs[i].stride;
            program->uavs[program->uav_count].globally_coherent =
                container->uavs[i].globally_coherent;
            program->uavs[program->uav_count].rasterizer_ordered =
                container->uavs[i].rasterizer_ordered;
            program->uavs[program->uav_count].has_order_preserving_counter =
                container->uavs[i].has_order_preserving_counter;
            program->uav_count++;
        }
    }
    
    int max_temp_idx = -1;
    
    size_t geometry_effect_index = 0u;
    size_t hull_phase_index = 0u;
    uint8_t active_signature_stream = 0u;

    // Parse declarations and instructions
    for (int i = 0; i < container->instruction_count; i++) {
        const DXBCInstruction* src_inst = &container->instructions[i];
        if (src_inst->operand_count < 0 ||
            src_inst->operand_count > DXBC_MAX_OPERANDS) {
            LOG_ERROR("DXBC opcode %u has invalid operand count %d",
                      src_inst->opcode, src_inst->operand_count);
            goto fail;
        }
        
        if (src_inst->opcode >= 114u && src_inst->opcode <= 116u) {
            if (!stage_contract ||
                stage_contract->program_type != DXBC_PROGRAM_TYPE_HULL ||
                hull_phase_index >= program->tessellation.phase_count ||
                src_inst->operand_count != 0) {
                LOG_ERROR("Hull phase marker disagrees with raw stage contract");
                goto fail;
            }
            const DXBCHullPhaseKind kind =
                src_inst->opcode == 114u
                    ? DXBC_HULL_PHASE_CONTROL_POINT
                    : src_inst->opcode == 115u
                          ? DXBC_HULL_PHASE_FORK
                          : DXBC_HULL_PHASE_JOIN;
            USILHullPhase* phase =
                &program->tessellation.phases[hull_phase_index];
            if (phase->kind != kind ||
                phase->first_instruction_index != -1) {
                LOG_ERROR("Hull phase kind/order disagrees with raw contract");
                goto fail;
            }
            if (hull_phase_index > 0u) {
                program->tessellation.phases[hull_phase_index - 1u]
                    .end_instruction_index = program->instruction_count;
            }
            phase->first_instruction_index = program->instruction_count;
            ++hull_phase_index;
            continue;
        }

        /* Opcode labels and the cached is_decl flag are diagnostic projections.
         * Classify the instruction from the raw opcode so a stale or mutated
         * label cannot turn executable code into metadata (or vice versa). */
        if (dxbc_opcode_is_declaration(src_inst->opcode)) {
            switch (src_inst->opcode) {
            case 53: /* CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER */
                /* The decoder copies the exact DWORD payload into
                 * container->icb_values; the rows themselves carry no
                 * additional semantic authority. */
                if (src_inst->operand_count != 0) goto declaration_fail;
                break;
            case 88:  /* DCL_RESOURCE */
            case 156: /* DCL_UAV_TYPED */
            case 157: /* DCL_UAV_RAW */
            case 158: /* DCL_UAV_STRUCTURED */
            case 161: /* DCL_RESOURCE_RAW */
            case 162: /* DCL_RESOURCE_STRUCTURED */
                /* Resource declaration payloads were already projected into
                 * the validated resource/UAV tables above. */
                if (src_inst->operand_count < 1) goto declaration_fail;
                break;
            case 89: /* DCL_CONSTANT_BUFFER */
                if (src_inst->operand_count == 1) {
                    const DXBCOperand* op = &src_inst->operands[0];
                    if (op->type != OPERAND_TYPE_CONSTANT_BUFFER)
                        goto declaration_fail;
                    if (program->cbuffer_count == INT_MAX ||
                        !reserve_usil_array(
                            (void**)&program->cbuffers,
                            &program->cbuffer_alloc,
                            program->cbuffer_count,
                            program->cbuffer_count + 1,
                            sizeof(*program->cbuffers))) goto fail;
                    program->cbuffers[program->cbuffer_count].reg_idx = op->register_index;
                    // cbN[size] has size in rel_offset0
                    program->cbuffers[program->cbuffer_count].size = op->rel_offset0;
                    program->cbuffers[program->cbuffer_count]
                        .dynamic_indexed =
                        ((src_inst->token >> 11u) & 1u) != 0u;
                    program->cbuffer_count++;
                } else goto declaration_fail;
                break;
            case 90: /* DCL_SAMPLER */
                if (src_inst->operand_count == 1) {
                    const DXBCOperand* op = &src_inst->operands[0];
                    if (op->type != OPERAND_TYPE_SAMPLER)
                        goto declaration_fail;
                    if (program->sampler_count == INT_MAX ||
                        !reserve_usil_array(
                            (void**)&program->samplers,
                            &program->sampler_alloc,
                            program->sampler_count,
                            program->sampler_count + 1,
                            sizeof(*program->samplers))) goto fail;
                    program->samplers[program->sampler_count].reg_idx = op->register_index;
                    program->samplers[program->sampler_count].mode =
                        (uint8_t)((src_inst->token >> 11u) & 0x0fu);
                    program->sampler_count++;
                } else goto declaration_fail;
                break;
            case 91: { /* DCL_INDEX_RANGE */
                uint32_t register_count = 0;
                if (!stage_contract ||
                    stage_contract->program_type != DXBC_PROGRAM_TYPE_HULL ||
                    hull_phase_index == 0u ||
                    hull_phase_index > program->tessellation.phase_count ||
                    src_inst->operand_count != 2 ||
                    !declaration_scalar_u32(src_inst, 1, &register_count) ||
                    register_count == 0 || register_count > INT_MAX) {
                    goto declaration_fail;
                }
                const USILHullPhase* phase =
                    &program->tessellation.phases[hull_phase_index - 1u];
                if (phase->first_instruction_index < 0 ||
                    program->index_range_count == INT_MAX ||
                    !reserve_usil_array(
                        (void**)&program->index_ranges,
                        &program->index_range_alloc,
                        program->index_range_count,
                        program->index_range_count + 1,
                        sizeof(*program->index_ranges))) {
                    goto declaration_fail;
                }
                USILIndexRange* range =
                    &program->index_ranges[program->index_range_count];
                memset(range, 0, sizeof(*range));
                if (!clone_operand(&range->operand,
                                   &src_inst->operands[0])) {
                    goto fail;
                }
                range->register_count = register_count;
                range->source_instruction_index = (uint32_t)i;
                range->hull_phase_index = (int)(hull_phase_index - 1u);
                program->index_range_count++;
                break;
            }
            case 92:  /* DCL_OUTPUT_TOPOLOGY */
            case 93:  /* DCL_INPUT_PRIMITIVE */
            case 94:  /* DCL_MAX_OUTPUT_VERTEX_COUNT */
            case 206: /* DCL_GS_INSTANCE_COUNT */
                if (!stage_contract ||
                    stage_contract->program_type !=
                        DXBC_PROGRAM_TYPE_GEOMETRY) {
                    goto declaration_fail;
                }
                break;
            case 143: /* DCL_STREAM */
                if (!stage_contract ||
                    stage_contract->program_type !=
                        DXBC_PROGRAM_TYPE_GEOMETRY ||
                    !signature_stream_declaration_id(
                        src_inst, &active_signature_stream)) {
                    goto declaration_fail;
                }
                break;
            case 95:  /* DCL_INPUT */
            case 96:  /* DCL_INPUT_SGV */
            case 97:  /* DCL_INPUT_SIV */
            case 98:  /* DCL_INPUT_PS */
            case 99:  /* DCL_INPUT_PS_SGV */
            case 100: /* DCL_INPUT_PS_SIV */
            case 101: /* DCL_OUTPUT */
            case 102: /* DCL_OUTPUT_SGV */
            case 103: /* DCL_OUTPUT_SIV */
                if (!append_signature_declaration(
                        program, src_inst, (uint32_t)i,
                        active_signature_stream)) {
                    goto declaration_fail;
                }
                break;
            case 105: /* DCL_INDEXABLE_TEMP */
                if (src_inst->operand_count == 2) {
                    const DXBCOperand* op = &src_inst->operands[0];
                    if (op->type != OPERAND_TYPE_INDEXABLE_TEMP)
                        goto declaration_fail;
                    if (program->indexable_temp_count == INT_MAX ||
                        !reserve_usil_array(
                            (void**)&program->indexable_temps,
                            &program->indexable_temp_alloc,
                            program->indexable_temp_count,
                            program->indexable_temp_count + 1,
                            sizeof(*program->indexable_temps))) goto fail;
                    program->indexable_temps[program->indexable_temp_count].reg_idx = op->register_index;
                    program->indexable_temps[program->indexable_temp_count].size = op->rel_offset1; // size is stored in rel_offset1
                    program->indexable_temp_count++;
                } else goto declaration_fail;
                break;
            case 104: /* DCL_TEMPS */
                if (src_inst->operand_count == 1 &&
                    src_inst->operands[0].register_index >= 0) {
                    program->temp_count = src_inst->operands[0].register_index;
                } else goto declaration_fail;
                break;
            case 106: /* DCL_GLOBAL_FLAGS */
                if (program->has_global_flags) goto declaration_fail;
                program->has_global_flags = true;
                program->global_flags = (src_inst->token >> 11u) & 0x1fffu;
                break;
            case 113: /* HS_DECLS */
            case 148: /* DCL_OUTPUT_CONTROL_POINT_COUNT */
            case 150: /* DCL_TESS_PARTITIONING */
            case 151: /* DCL_TESS_OUTPUT_PRIMITIVE */
            case 152: /* DCL_HS_MAX_TESSFACTOR */
            case 153: /* DCL_HS_FORK_PHASE_INSTANCE_COUNT */
            case 154: /* DCL_HS_JOIN_PHASE_INSTANCE_COUNT */
                if (!stage_contract ||
                    stage_contract->program_type != DXBC_PROGRAM_TYPE_HULL) {
                    goto declaration_fail;
                }
                break;
            case 147: /* DCL_INPUT_CONTROL_POINT_COUNT */
            case 149: /* DCL_TESS_DOMAIN */
                if (!stage_contract ||
                    (stage_contract->program_type != DXBC_PROGRAM_TYPE_HULL &&
                     stage_contract->program_type !=
                         DXBC_PROGRAM_TYPE_DOMAIN)) {
                    goto declaration_fail;
                }
                break;
            default:
                /* Function/interface linkage, compute thread-group and TGSM
                 * declarations have no faithful USIL model yet. Never erase
                 * them by treating them as inert metadata. */
                LOG_ERROR("Cannot translate DXBC declaration opcode %u: no "
                          "faithful USIL representation",
                          src_inst->opcode);
                goto fail;
            }
            continue; // Skip declaration in final USIL instruction list

declaration_fail:
            LOG_ERROR("Malformed or stage-inconsistent DXBC declaration "
                      "opcode %u", src_inst->opcode);
            goto fail;
        }
        
        // standard instruction
        if (program->instruction_count == INT_MAX ||
            !reserve_usil_array(
                (void**)&program->instructions,
                &program->instruction_alloc, program->instruction_count,
                program->instruction_count + 1,
                sizeof(*program->instructions))) goto fail;
        
        USILInstruction* dest_inst = &program->instructions[program->instruction_count];
        memset(dest_inst, 0, sizeof(USILInstruction));
        
        if (!map_dxbc_opcode(src_inst->opcode, &dest_inst->opcode)) {
            LOG_ERROR("Cannot translate executable DXBC opcode %u (%s): no "
                      "faithful USIL representation",
                      src_inst->opcode, src_inst->opcode_str);
            goto fail;
        }
        dest_inst->saturate = src_inst->saturate;
        dest_inst->condition_test = src_inst->condition_test;
        dest_inst->source_instruction_index = (uint32_t)i;
        if (!geometry_effect_matches(src_inst, (size_t)i, stage_contract,
                                     &geometry_effect_index, dest_inst)) {
            LOG_ERROR("Geometry stream effect disagrees with raw DXBC contract "
                      "at semantic instruction %d", i);
            goto fail;
        }
        const char *instruction_dimension =
            src_inst->has_resource_dimension
                ? resource_dimension_from_code(src_inst->resource_dimension)
                : instruction_resource_dimension_from_binding(container,
                                                              src_inst);
        if (!instruction_dimension) {
            LOG_ERROR("Missing or ambiguous resource-dimension authority on "
                      "DXBC opcode %u", src_inst->opcode);
            goto fail;
        }
        snprintf(dest_inst->resource_dimension,
                 sizeof(dest_inst->resource_dimension), "%s",
                 instruction_dimension ? instruction_dimension : "");
        dest_inst->has_resource_dimension =
            src_inst->has_resource_dimension;
        dest_inst->resource_stride =
            src_inst->structured_stride >= 0
                ? (uint32_t)src_inst->structured_stride
                : 0;
        dest_inst->has_texel_offset = src_inst->has_texel_offset;
        memcpy(dest_inst->texel_offsets, src_inst->texel_offsets,
               sizeof(dest_inst->texel_offsets));
        dest_inst->has_resource_return_types =
            src_inst->has_resource_return_types;
        memcpy(dest_inst->resource_return_types,
               src_inst->resource_return_types,
               sizeof(dest_inst->resource_return_types));
        dest_inst->resource_info_return_type =
            src_inst->resource_info_return_type;
        dest_inst->sample_info_return_type =
            src_inst->sample_info_return_type;
        dest_inst->precise_mask = src_inst->precise_mask;
        dest_inst->operand_count = src_inst->operand_count;
        memcpy(dest_inst->original_asm, src_inst->formatted_asm,
               sizeof(dest_inst->original_asm));
        
        for (int k = 0; k < src_inst->operand_count; k++) {
            if (!clone_operand(&dest_inst->operands[k],
                               &src_inst->operands[k])) {
                for (int cloned = 0; cloned <= k; cloned++) {
                    free_usil_operand(&dest_inst->operands[cloned]);
                }
                goto fail;
            }
            check_operand_temp_usage(&dest_inst->operands[k], &max_temp_idx);
        }
        
        program->instruction_count++;
    }

    if (stage_contract &&
        stage_contract->program_type == DXBC_PROGRAM_TYPE_GEOMETRY &&
        geometry_effect_index != stage_contract->geometry_effect_count) {
        LOG_ERROR("Semantic projection omitted a geometry stream effect");
        goto fail;
    }
    if (stage_contract &&
        stage_contract->program_type == DXBC_PROGRAM_TYPE_HULL) {
        if (hull_phase_index != program->tessellation.phase_count ||
            hull_phase_index == 0u) {
            LOG_ERROR("Semantic projection omitted a hull phase");
            goto fail;
        }
        program->tessellation.phases[hull_phase_index - 1u]
            .end_instruction_index = program->instruction_count;
        for (size_t phase_index = 0;
             phase_index < program->tessellation.phase_count;
             ++phase_index) {
            const USILHullPhase* phase =
                &program->tessellation.phases[phase_index];
            if (phase->first_instruction_index < 0 ||
                phase->end_instruction_index <=
                    phase->first_instruction_index) {
                LOG_ERROR("Hull phase has no faithfully projected body");
                goto fail;
            }
        }
    }
    
    if (program->temp_count == 0 && max_temp_idx >= 0) {
        if (max_temp_idx == INT_MAX) goto fail;
        program->temp_count = max_temp_idx + 1;
    }
    
    if (!reserve_usil_array((void**)&program->icb_values,
                            &program->icb_value_alloc,
                            program->icb_value_count,
                            container->icb_value_count,
                            sizeof(*program->icb_values))) {
        goto fail;
    }
    program->icb_value_count = container->icb_value_count;
    if (program->icb_value_count > 0) {
        memcpy(program->icb_values, container->icb_values,
               (size_t)program->icb_value_count *
                   sizeof(*program->icb_values));
    }

    if (!usil_signature_authority_is_valid(program)) {
        if (getenv("DXBC_DEBUG_STAGE")) {
            for (int index = 0; index < program->input_count; ++index) {
                const DXBCSignatureElement* element =
                    &program->inputs[index];
                fprintf(stderr,
                        "[usil] ISG %s%u sv=%u reg=%u mask=%x rw=%x "
                        "interp=%u\n",
                        dxbc_signature_semantic_name(element),
                        element->semantic_index, element->system_value,
                        element->register_id, element->mask,
                        element->rw_mask, element->interpolation_mode);
            }
            for (int index = 0; index < program->output_count; ++index) {
                const DXBCSignatureElement* element =
                    &program->outputs[index];
                fprintf(stderr,
                        "[usil] OSG %s%u sv=%u reg=%u mask=%x rw=%x "
                        "stream=%u\n",
                        dxbc_signature_semantic_name(element),
                        element->semantic_index, element->system_value,
                        element->register_id, element->mask,
                        element->rw_mask, element->stream_index);
            }
            for (int index = 0; index < program->patch_constant_count;
                 ++index) {
                const DXBCSignatureElement* element =
                    &program->patch_constants[index];
                fprintf(stderr,
                        "[usil] PSG %s%u sv=%u reg=%u mask=%x rw=%x\n",
                        dxbc_signature_semantic_name(element),
                        element->semantic_index, element->system_value,
                        element->register_id, element->mask,
                        element->rw_mask);
            }
            for (int index = 0;
                 index < program->signature_declaration_count; ++index) {
                const USILSignatureDeclaration* declaration =
                    &program->signature_declarations[index];
                fprintf(stderr,
                        "[usil] DCL op=%u type=%u hasreg=%d reg=%u mask=%x "
                        "stream=%u sys=%d/%u interp=%d/%u\n",
                        (unsigned)declaration->kind,
                        (unsigned)declaration->operand_type,
                        declaration->has_signature_register,
                        declaration->register_id, declaration->mask,
                        declaration->stream_index,
                        declaration->has_system_value,
                        declaration->system_value_name,
                        declaration->has_interpolation,
                        declaration->interpolation_mode);
            }
        }
        LOG_ERROR("DXBC signature declarations disagree with signature "
                  "reflection authority");
        goto fail;
    }
    
    return true;

fail:
    usil_free(program);
    return false;
}

void usil_free(USILProgram* program) {
    if (!program) return;
    if (program->instructions) {
        for (int i = 0; i < program->instruction_count; i++) {
            USILInstruction* inst = &program->instructions[i];
            for (int k = 0; k < inst->operand_count; k++) {
                free_usil_operand(&inst->operands[k]);
            }
        }
        mem_free(program->instructions, program->instruction_alloc * sizeof(USILInstruction));
        program->instructions = NULL;
    }
    if (program->cbuffers) {
        mem_free(program->cbuffers, program->cbuffer_alloc * sizeof(USILConstantBuffer));
        program->cbuffers = NULL;
    }
    if (program->textures) {
        mem_free(program->textures, program->texture_alloc * sizeof(USILTexture));
        program->textures = NULL;
    }
    if (program->samplers) {
        mem_free(program->samplers, program->sampler_alloc * sizeof(USILSampler));
        program->samplers = NULL;
    }
    if (program->indexable_temps) {
        mem_free(program->indexable_temps, program->indexable_temp_alloc * sizeof(USILIndexableTemp));
        program->indexable_temps = NULL;
    }
    if (program->index_ranges) {
        for (int i = 0; i < program->index_range_count; ++i) {
            free_usil_operand(&program->index_ranges[i].operand);
        }
        mem_free(program->index_ranges,
                 (size_t)program->index_range_alloc *
                     sizeof(*program->index_ranges));
        program->index_ranges = NULL;
    }
    if (program->uavs) {
        mem_free(program->uavs, program->uav_alloc * sizeof(USILUav));
        program->uavs = NULL;
    }
    mem_free(program->signature_declarations,
             (size_t)program->signature_declaration_alloc *
                 sizeof(*program->signature_declarations));
    free_signature_elements(program->inputs, program->input_count);
    free_signature_elements(program->outputs, program->output_count);
    free_signature_elements(program->patch_constants,
                            program->patch_constant_count);
    mem_free(program->inputs,
             (size_t)program->input_alloc * sizeof(*program->inputs));
    mem_free(program->outputs,
             (size_t)program->output_alloc * sizeof(*program->outputs));
    mem_free(program->patch_constants,
             (size_t)program->patch_constant_alloc *
                 sizeof(*program->patch_constants));
    mem_free(program->tessellation.phases,
             program->tessellation.phase_capacity *
                 sizeof(*program->tessellation.phases));
    mem_free(program->icb_values,
             (size_t)program->icb_value_alloc * sizeof(*program->icb_values));
    memset(program, 0, sizeof(USILProgram));
}

bool usil_translate(USILProgram* program, const DXBCContainer* container) {
    return usil_translate_internal(program, container, NULL);
}

bool usil_translate_with_stage_contract(
    USILProgram* program, const DXBCContainer* container,
    const DXBCStageContract* stage_contract) {
    if (!stage_contract) {
        if (program) memset(program, 0, sizeof(*program));
        return false;
    }
    return usil_translate_internal(program, container, stage_contract);
}
