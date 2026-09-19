// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_USE_DEF_H
#define HLSL_USE_DEF_H

#define HLSL_DEFINITION_UNKNOWN (-1)
#define HLSL_DEFINITION_AMBIGUOUS (-2)

typedef struct HLSLUseDefGraph {
    int *operand_definitions;
    /* Conservative count per instruction/lane, including incoming phi edges.
     * Multiple destination operands on one instruction share this count. */
    unsigned int *definition_use_counts;
    int instruction_count;
} HLSLUseDefGraph;

#endif
