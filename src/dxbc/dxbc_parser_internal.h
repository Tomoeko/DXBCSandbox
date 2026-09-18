// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_PARSER_INTERNAL_H
#define DXBC_PARSER_INTERNAL_H

#include "dxbc/dxbc_parser.h"

/* Bits 30:24 of an SM4/5 ordinary opcode token.  Zero is invalid for an
 * ordinary instruction; CUSTOMDATA (opcode 53) uses DWORD 1 for its block
 * length because these bits belong to its custom-data class. */
static inline uint32_t dxbc_tokenized_instruction_length(uint32_t token) {
    return (token >> 24) & 0x7fu;
}

typedef enum {
    DXBC_OPERAND_CONTEXT_EXECUTABLE = 0,
    DXBC_OPERAND_CONTEXT_DECLARATION,
    DXBC_OPERAND_CONTEXT_CONSTANT_BUFFER_DECLARATION,
    DXBC_OPERAND_CONTEXT_RELATIVE_INDEX
} DXBCOperandParseContext;

/* Internal operand parsing/freeing/diagnostic-formatting functions.  Opcode
 * and context are typed so mutable presentation strings can never influence
 * token grammar or semantic state. */
DXBCOperand* parse_operand_recursive(ByteStream* stream, size_t limit,
                                     DXBCOperandParseContext context,
                                     uint32_t opcode,
                                     DXBCOperand* destOp);
void free_operand(DXBCOperand* op);
void dxbc_format_immediate_diagnostic(uint32_t value, uint32_t opcode,
                                      bool is_int, char* output,
                                      size_t output_size);

// Internal instruction decoding logic
bool parse_shader_logic(ByteStream* stream, size_t next_pos, DXBCContainer* container);
DXBCResourceDecl* dxbc_get_or_add_resource_declaration(
    DXBCContainer* container, int register_index, bool uav);

#endif // DXBC_PARSER_INTERNAL_H
