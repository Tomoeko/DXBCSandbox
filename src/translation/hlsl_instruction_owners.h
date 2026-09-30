// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_INSTRUCTION_OWNERS_H
#define HLSL_INSTRUCTION_OWNERS_H

#include "translation/hlsl_emitter.h"
#include <string.h>

/* Each bit names a validated source instruction, independently of AST node
 * count. Wider programs must not wrap a scalar mask or lose root ownership. */
enum { HLSL_INSTRUCTION_OWNER_WORDS = (HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT + 63) / 64 };
typedef struct { uint64_t words[HLSL_INSTRUCTION_OWNER_WORDS]; } HLSLInstructionOwners;

static inline bool hlsl_instruction_owners_add(HLSLInstructionOwners *owners, int instruction) {
    if (!owners || instruction < 0 || instruction >= HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT) return false;
    owners->words[(unsigned)instruction / 64] |= UINT64_C(1) << ((unsigned)instruction % 64);
    return true;
}
static inline bool hlsl_instruction_owners_contains(const HLSLInstructionOwners *owners, int instruction) {
    return owners && instruction >= 0 && instruction < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT &&
        (owners->words[(unsigned)instruction / 64] & (UINT64_C(1) << ((unsigned)instruction % 64))) != 0;
}
static inline void hlsl_instruction_owners_union(HLSLInstructionOwners *owners, const HLSLInstructionOwners *other) {
    for (unsigned word = 0; word < HLSL_INSTRUCTION_OWNER_WORDS; ++word) owners->words[word] |= other->words[word];
}
static inline void hlsl_instruction_owners_clear(HLSLInstructionOwners *owners) { memset(owners, 0, sizeof(*owners)); }
static inline bool hlsl_instruction_owners_empty(const HLSLInstructionOwners *owners) {
    for (unsigned word = 0; word < HLSL_INSTRUCTION_OWNER_WORDS; ++word) if (owners->words[word]) return false;
    return true;
}
#endif
