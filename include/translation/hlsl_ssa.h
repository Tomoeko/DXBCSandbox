// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_SSA_H
#define HLSL_SSA_H

#include <stdbool.h>

typedef struct {
    int register_index;
    int component;
    int ssa_var;           // Unique SSA variable ID defined by this Phi node
    int *incoming_vars;    // Array of incoming SSA variable IDs (size = predecessor_count)
    int *incoming_blocks;  // Array of predecessor block indices (size = predecessor_count)
} HLSLPhiNode;

typedef struct {
    HLSLPhiNode *phis;
    int phi_count;
} HLSLBlockPhis;

typedef struct {
    HLSLBlockPhis *block_phis; // Array of Phi lists per basic block (size = block_count)
    int *operand_ssa_vars;     // Flat array mapping (inst, operand, comp) -> ssa_var ID
    int *ssa_var_defs;         // Array mapping ssa_var ID -> definition instruction index (size = ssa_var_count)
    int instruction_count;
    int ssa_var_count;
} HLSLSSAGraph;

struct HLSLEmitterContext;

bool build_hlsl_ssa_graph(struct HLSLEmitterContext *ctx);
void free_hlsl_ssa_graph(struct HLSLEmitterContext *ctx);

#endif // HLSL_SSA_H
