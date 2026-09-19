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
    /* RET, conditional DISCARD, or falling off the program. Termination is
     * separate from successors because it carries no reaching definitions. */
    bool may_exit;
} HLSLBasicBlock;

typedef struct {
    int parent_block;
    bool is_loop;
    bool is_if;
} HLSLBlockNesting;

/* Instruction indices from the single validated structured-flow matcher.
 * Missing links are -1. ELSE's parent is the enclosing construct's parent;
 * instructions in its arm name ELSE as their parent. */
typedef struct {
    int end;
    int alternate;
    int parent;
    int jump_scope;
    int first_case;
    int next_case;
    bool has_default;
} HLSLInstructionFlow;

typedef struct HLSLControlFlowGraph {
    HLSLBasicBlock *blocks;
    int *successor_storage;
    int *predecessor_storage;
    int block_count;
    int instruction_count;
    int *instruction_block;
    HLSLInstructionFlow *instruction_flow;
    int *idom;
    int **df;
    int *df_count;
    HLSLBlockNesting *nesting;
} HLSLControlFlowGraph;

typedef struct {
    int header_block;
    int true_block;
    int false_block;
    int join_block;
    int else_instruction;
    int end_instruction;
} HLSLIfRegion;

bool compute_dominance(HLSLControlFlowGraph *cfg);
bool analyze_block_nesting(struct HLSLEmitterContext *ctx);
/* Reachable blocks only; dominance must already have been computed. */
bool hlsl_cfg_dominates(const HLSLControlFlowGraph *cfg, int dominator, int block);
/* Strong post-dominance: every graph path from start reaches target, including
 * paths that could terminate or loop forever. A cycle avoiding target fails;
 * this deliberately makes no inference about loop bounds or branch values.
 * Uses O(blocks + edges) work and O(blocks) temporary storage. Allocation or
 * analysis unavailability returns false, never an unproved success. */
bool hlsl_cfg_must_reach(const HLSLControlFlowGraph *cfg, int start, int target);
/* A reachable single-entry IF region whose arms must reach its lexical join.
 * No outside predecessor may enter an arm. Loops and early exits are admitted
 * only when the same graph proof succeeds. Output is unchanged on failure. */
bool hlsl_cfg_if_region(const struct HLSLEmitterContext *ctx, int instruction,
                        HLSLIfRegion *region);

#endif
