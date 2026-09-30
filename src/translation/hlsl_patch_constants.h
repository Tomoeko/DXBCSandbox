// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_PATCH_CONSTANTS_H
#define HLSL_PATCH_CONSTANTS_H

#include "translation/usil.h"

enum { HLSL_PATCH_CONSTANT_LIMIT = 32 };
typedef enum { HLSL_PATCH_OUTER, HLSL_PATCH_INNER, HLSL_PATCH_CUSTOM } HLSLPatchFieldKind;
typedef struct {
    HLSLPatchFieldKind kind;
    const char *semantic;
    uint32_t semantic_index;
    uint8_t first_register, count;
} HLSLPatchField;
/* A canonical scalar/array representation of actual PCSG rows. Packed vectors
 * remain outside this boundary: no register-to-field aliasing is guessed. */
typedef struct {
    HLSLPatchField fields[HLSL_PATCH_CONSTANT_LIMIT];
    uint8_t field_count, row_count;
    int8_t register_field[HLSL_PATCH_CONSTANT_LIMIT];
    uint8_t register_element[HLSL_PATCH_CONSTANT_LIMIT];
    bool has_custom;
} HLSLPatchLayout;
bool hlsl_patch_scalar_layout(const USILProgram *program, HLSLPatchLayout *layout);
bool hlsl_patch_static_scalar_operand(const HLSLPatchLayout *layout,
                                     const DXBCOperand *operand);
#endif
