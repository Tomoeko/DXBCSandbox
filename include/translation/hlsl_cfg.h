// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_CFG_H
#define HLSL_CFG_H

#include <stdbool.h>

struct HLSLEmitterContext;

typedef struct HLSLBasicBlock {
    int first_instruction;
    int last_instruction;
    /* Borrowed slices of the enclosing graph's owned edge storage. */
    int *successors;
    int *predecessors;
    int successor_count;
    int predecessor_count;
    bool has_ambiguous_flow;
} HLSLBasicBlock;

typedef struct {
    int parent_block;
    bool is_loop;
    bool is_if;
} HLSLBlockNesting;

typedef struct HLSLControlFlowGraph {
    HLSLBasicBlock *blocks;
    int *successor_storage;
    int *predecessor_storage;
    int block_count;
    int *instruction_block;
    int *idom;
    int **df;
    int *df_count;
    HLSLBlockNesting *nesting;
} HLSLControlFlowGraph;

bool compute_dominance(HLSLControlFlowGraph *cfg);
bool analyze_block_nesting(struct HLSLEmitterContext *ctx);

#endif
