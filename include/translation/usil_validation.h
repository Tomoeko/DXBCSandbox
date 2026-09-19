// SPDX-License-Identifier: GPL-3.0-only

#ifndef USIL_VALIDATION_H
#define USIL_VALIDATION_H

#include "translation/usil.h"

typedef enum {
    USIL_OPERAND_USE_INVALID = 0,
    USIL_OPERAND_USE_DESTINATION,
    USIL_OPERAND_USE_SOURCE,
    USIL_OPERAND_USE_RESOURCE_BINDING,
    USIL_OPERAND_USE_SAMPLER_BINDING,
    USIL_OPERAND_USE_STREAM_SELECTOR
} USILOperandUse;

typedef struct {
    USILOperandUse use;
    /* Logical x/y/z/w lanes consumed by a source operand. This is zero for
     * destinations and binding/select operands. */
    uint8_t source_lane_mask;
} USILOperandUseInfo;

/* Conservative observable/context effects. NONE permits value-only local
 * rewriting; it does not permit floating-point algebraic reassociation. */
typedef enum {
    USIL_EFFECT_NONE = 0,
    USIL_EFFECT_CONTROL = 1u << 0,
    USIL_EFFECT_RESOURCE_READ = 1u << 1,
    USIL_EFFECT_EXTERNAL_WRITE = 1u << 2,
    USIL_EFFECT_QUAD_CONTEXT = 1u << 3,
    USIL_EFFECT_GEOMETRY_OUTPUT = 1u << 4,
    USIL_EFFECT_UNKNOWN = 1u << 5
} USILEffectFlags;
bool usil_instruction_effects(const USILProgram *program,
                               const USILInstruction *instruction,
                               USILEffectFlags *out_effects);

/* Resolve a logical source lane to x/y/z/w, or -1 for an invalid swizzle. */
int usil_operand_source_component(const DXBCOperand *operand, int lane);

/* Exact written lanes, including scalar depth outputs; null writes are zero. */
uint8_t usil_operand_destination_lane_mask(const DXBCOperand *operand);

/* Validates the exact operand arity for every opcode represented by USIL and
 * proves every source lane from the destination/resource shape. Unknown or
 * underspecified instructions fail closed. */
bool usil_instruction_shape_valid(const USILProgram *program,
                                  const USILInstruction *instruction);

/* Returns the typed use and exact source-lane demand for one operand. The
 * complete instruction shape is validated before any use is returned. */
bool usil_instruction_operand_use(const USILProgram *program,
                                  const USILInstruction *instruction,
                                  int operand_index,
                                  USILOperandUseInfo *info);

#endif
