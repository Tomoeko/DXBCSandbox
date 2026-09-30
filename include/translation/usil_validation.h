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
    USIL_EFFECT_UNKNOWN = 1u << 5,
    USIL_EFFECT_MEMORY_BARRIER = 1u << 6,
    USIL_EFFECT_ATOMIC = 1u << 7,
    USIL_EFFECT_COUNTER = 1u << 8,
    USIL_EFFECT_THREAD_GROUP_READ = 1u << 9,
    USIL_EFFECT_THREAD_GROUP_WRITE = 1u << 10
} USILEffectFlags;
bool usil_instruction_effects(const USILProgram *program,
                               const USILInstruction *instruction,
                               USILEffectFlags *out_effects);

typedef enum {
    USIL_MEMORY_RAW = 0,
    USIL_MEMORY_STRUCTURED,
    USIL_MEMORY_TYPED,
    USIL_MEMORY_COUNTER
} USILMemoryKind;

typedef enum {
    USIL_MEMORY_SHADER_RESOURCE = 0,
    USIL_MEMORY_UNORDERED_ACCESS,
    USIL_MEMORY_THREAD_GROUP
} USILMemorySpace;

typedef enum {
    USIL_COUNTER_NONE = 0,
    USIL_COUNTER_APPEND_INVOCATION,
    USIL_COUNTER_ORDER_PRESERVING
} USILCounterMode;

/* Static declaration and exact operand/lane authority for the admitted SM5
 * compute memory operations. Values remain raw register bits; this records
 * memory format and effects, not source types, address bounds, race freedom,
 * atomic ordering proofs, or runtime resource-format certification. */
typedef struct {
    USILMemoryKind kind;
    USILMemorySpace space;
    uint32_t register_id;
    uint32_t byte_stride;
    uint32_t shared_memory_byte_count;
    char dimension[16];
    uint8_t return_types[4];
    bool globally_coherent;
    bool rasterizer_ordered;
    bool has_order_preserving_counter;
    USILCounterMode counter_mode;
    bool reads;
    bool writes;
    bool atomic;
    int destination_operand;
    int binding_operand;
    int address_operand;
    int byte_offset_operand;
    int value_operand;
    int compare_operand;
    uint8_t destination_lanes;
    uint8_t address_lanes;
    uint8_t value_lanes;
    uint8_t memory_component_lanes;
} USILMemoryAccess;

bool usil_opcode_has_memory_access(USILOpcode opcode);
bool usil_instruction_memory_access(const USILProgram *program,
                                    const USILInstruction *instruction,
                                    USILMemoryAccess *out_access);

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
