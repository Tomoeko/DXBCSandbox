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
    uint8_t first_register, count, width, mask;
} HLSLPatchField;
/* Actual PCSG fields own disjoint physical lanes. Custom packed vectors are
 * one float2/float3 field; vector arrays remain outside this boundary. */
typedef struct {
    HLSLPatchField fields[HLSL_PATCH_CONSTANT_LIMIT];
    uint8_t field_count, row_count, signature_count;
    int8_t lane_field[HLSL_PATCH_CONSTANT_LIMIT][4];
    uint8_t lane_element[HLSL_PATCH_CONSTANT_LIMIT][4];
    uint8_t lane_component[HLSL_PATCH_CONSTANT_LIMIT][4];
    uint8_t register_mask[HLSL_PATCH_CONSTANT_LIMIT];
    bool has_custom;
} HLSLPatchLayout;
bool hlsl_patch_layout(const USILProgram *program, HLSLPatchLayout *layout);
bool hlsl_patch_static_operand(const HLSLPatchLayout *layout,
                              const DXBCOperand *operand);
#endif
