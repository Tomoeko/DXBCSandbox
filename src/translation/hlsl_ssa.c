// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static size_t operand_offset(int instruction, int operand, int component) {
    return (((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u) +
           (size_t)component;
}

static bool instruction_defines_reg_comp(const USILProgram *program,
                                          const USILInstruction *inst,
                                          int reg, int comp) {
    for (int index = 0; index < inst->operand_count; ++index) {
        USILOperandUseInfo use;
        if (!usil_instruction_operand_use(program, inst, index, &use)) return false;
        const DXBCOperand *dest = &inst->operands[index];
        if (use.use == USIL_OPERAND_USE_DESTINATION &&
            dest->type == OPERAND_TYPE_TEMP && dest->register_index == reg &&
            (usil_operand_destination_lane_mask(dest) & (1u << comp))) return true;
    }
    return false;
}

static bool add_phi_node(HLSLBlockPhis *bp, int reg, int comp,
                         int predecessor_count) {
    if (!bp || bp->phi_count < 0 || bp->phi_count == INT_MAX ||
        predecessor_count < 0 ||
        dxbc_size_multiply_overflows((size_t)bp->phi_count + 1u,
                                     sizeof(*bp->phis)) ||
        dxbc_size_multiply_overflows((size_t)predecessor_count,
                                     sizeof(int))) {
        return false;
    }
    HLSLPhiNode *replacement = realloc(
        bp->phis, ((size_t)bp->phi_count + 1u) * sizeof(*bp->phis));
    if (!replacement) return false;
    bp->phis = replacement;
    HLSLPhiNode *phi = &bp->phis[bp->phi_count];
    memset(phi, 0, sizeof(*phi));
    phi->register_index = reg;
    phi->component = comp;
    phi->ssa_var = -1;
    if (predecessor_count > 0) {
        phi->incoming_vars =
            malloc((size_t)predecessor_count * sizeof(int));
        phi->incoming_blocks =
            malloc((size_t)predecessor_count * sizeof(int));
        if (!phi->incoming_vars || !phi->incoming_blocks) {
            free(phi->incoming_vars);
            free(phi->incoming_blocks);
            memset(phi, 0, sizeof(*phi));
            return false;
        }
    }
    for (int i = 0; i < predecessor_count; i++) {
        phi->incoming_vars[i] = -1;
        phi->incoming_blocks[i] = -1;
    }
    bp->phi_count++;
    return true;
}

typedef struct {
    int reg;
    int comp;
    int old_version;
} RenamingUndoEntry;

static bool append_undo_entry(RenamingUndoEntry **undo_log, int *undo_count,
                              int *undo_capacity, int reg, int comp,
                              int old_version) {
    if (!undo_log || !undo_count || !undo_capacity || *undo_count < 0 ||
        *undo_capacity < *undo_count || *undo_count == INT_MAX) {
        return false;
    }
    if (*undo_count == *undo_capacity) {
        int new_capacity = *undo_capacity == 0 ? 64 : *undo_capacity;
        if (new_capacity > INT_MAX / 2) new_capacity = INT_MAX;
        else new_capacity *= 2;
        if (new_capacity <= *undo_count ||
            dxbc_size_multiply_overflows((size_t)new_capacity,
                                         sizeof(**undo_log))) {
            return false;
        }
        RenamingUndoEntry *replacement = realloc(
            *undo_log, (size_t)new_capacity * sizeof(**undo_log));
        if (!replacement) return false;
        *undo_log = replacement;
        *undo_capacity = new_capacity;
    }
    (*undo_log)[*undo_count].reg = reg;
    (*undo_log)[*undo_count].comp = comp;
    (*undo_log)[*undo_count].old_version = old_version;
    (*undo_count)++;
    return true;
}

static bool rename_ssa_block(HLSLEmitterContext *ctx, int block, int (*active_version)[4],
                             int *next_ssa_var, RenamingUndoEntry **undo_log, int *undo_count,
                             int *undo_capacity) {
    HLSLControlFlowGraph *cfg = &ctx->cfg;
    HLSLSSAGraph *ssa = &ctx->ssa;

    // 1. Rename definitions in Phi nodes
    HLSLBlockPhis *bp = &ssa->block_phis[block];
    for (int i = 0; i < bp->phi_count; i++) {
        HLSLPhiNode *phi = &bp->phis[i];
        int reg = phi->register_index;
        int comp = phi->component;

        int new_var = (*next_ssa_var)++;
        phi->ssa_var = new_var;
        /* A merge is not the first executable instruction in its block. */
        ssa->ssa_var_defs[new_var] = HLSL_DEFINITION_AMBIGUOUS;

        if (!append_undo_entry(undo_log, undo_count, undo_capacity, reg, comp,
                               active_version[reg][comp])) {
            return false;
        }

        active_version[reg][comp] = new_var;
    }

    // 2. Rename operands in instructions
    int start = cfg->blocks[block].first_instruction;
    int end = cfg->blocks[block].last_instruction;
    for (int inst_idx = start; inst_idx <= end; inst_idx++) {
        const USILInstruction *inst = &ctx->program->instructions[inst_idx];

        // 2a. Rename uses (sources)
        for (int op_idx = 0; op_idx < inst->operand_count; ++op_idx) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, inst, op_idx, &use)) {
                return false;
            }
            const DXBCOperand *operand = &inst->operands[op_idx];
            if (operand->type != OPERAND_TYPE_TEMP ||
                use.use != USIL_OPERAND_USE_SOURCE) continue;
            int reg = operand->register_index;
            if (reg < 0 || reg >= ctx->program->temp_count)
                return false;
            for (int comp = 0; comp < 4; ++comp) {
                if (!(use.source_lane_mask & (1u << comp))) continue;
                int source = usil_operand_source_component(operand, comp);
                if (source < 0)
                    return false;
                ssa->operand_ssa_vars[operand_offset(inst_idx, op_idx, comp)] =
                    active_version[reg][source];
            }
        }

        /* Read every source before writing either result of IMUL/UDIV/SINCOS.
         * A source may alias one of the destinations. */
        for (int op_idx = 0; op_idx < inst->operand_count; ++op_idx) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, inst, op_idx, &use)) {
                return false;
            }
            const DXBCOperand *operand = &inst->operands[op_idx];
            if (operand->type != OPERAND_TYPE_TEMP ||
                use.use != USIL_OPERAND_USE_DESTINATION) continue;
            int reg = operand->register_index;
            if (reg < 0 || reg >= ctx->program->temp_count)
                return false;
            uint8_t mask = usil_operand_destination_lane_mask(operand);
            for (int comp = 0; comp < 4; ++comp) {
                if (!(mask & (1u << comp))) continue;
                int new_var = (*next_ssa_var)++;
                ssa->operand_ssa_vars[operand_offset(inst_idx, op_idx, comp)] = new_var;
                ssa->ssa_var_defs[new_var] = inst_idx;
                if (!append_undo_entry(undo_log, undo_count, undo_capacity, reg, comp,
                                       active_version[reg][comp])) {
                    return false;
                }
                active_version[reg][comp] = new_var;
            }
        }
    }

    // 3. Fill incoming vars in successors' Phi nodes
    for (int s = 0; s < cfg->blocks[block].successor_count; s++) {
        int succ = cfg->blocks[block].successors[s];

        int pred_slot = -1;
        for (int k = 0; k < cfg->blocks[succ].predecessor_count; k++) {
            if (cfg->blocks[succ].predecessors[k] == block) {
                pred_slot = k;
                break;
            }
        }

        if (pred_slot != -1) {
            HLSLBlockPhis *succ_bp = &ssa->block_phis[succ];
            for (int i = 0; i < succ_bp->phi_count; i++) {
                HLSLPhiNode *phi = &succ_bp->phis[i];
                int reg = phi->register_index;
                int comp = phi->component;
                phi->incoming_vars[pred_slot] = active_version[reg][comp];
                phi->incoming_blocks[pred_slot] = block;
            }
        }
    }

    return true;
}

typedef struct {
    int block;
    int next_child;
    int undo_start;
    bool entered;
} SSARenameFrame;

static bool rename_ssa(HLSLEmitterContext *ctx, int **dom_children, int *dom_child_counts,
                       int (*active_version)[4], int *next_ssa_var) {
    if (dxbc_size_multiply_overflows((size_t)ctx->cfg.block_count, sizeof(SSARenameFrame)))
        return false;
    SSARenameFrame *stack = calloc((size_t)ctx->cfg.block_count, sizeof(*stack));
    if (!stack)
        return false;
    RenamingUndoEntry *undo_log = NULL;
    int undo_count = 0, undo_capacity = 0, depth = 1;
    bool success = false;
    while (depth) {
        SSARenameFrame *frame = &stack[depth - 1];
        if (!frame->entered) {
            frame->undo_start = undo_count;
            if (!rename_ssa_block(ctx, frame->block, active_version, next_ssa_var, &undo_log,
                                  &undo_count, &undo_capacity))
                goto cleanup;
            frame->entered = true;
        }
        if (frame->next_child < dom_child_counts[frame->block]) {
            const int child = dom_children[frame->block][frame->next_child++];
            stack[depth++] = (SSARenameFrame){.block = child};
        } else {
            while (undo_count > frame->undo_start) {
                const RenamingUndoEntry *entry = &undo_log[--undo_count];
                active_version[entry->reg][entry->comp] = entry->old_version;
            }
            --depth;
        }
    }
    success = true;
cleanup:
    free(undo_log);
    free(stack);
    return success;
}

bool build_hlsl_ssa_graph(HLSLEmitterContext *ctx) {
    if (!ctx || !ctx->program || ctx->program->instruction_count < 0 ||
        ctx->cfg.block_count < 0) return false;
    HLSLControlFlowGraph *cfg = &ctx->cfg;
    int count = ctx->program->instruction_count;
    const int register_count = ctx->program->temp_count;
    if (register_count < 0) return false;
    if (cfg->block_count == 0) return true;

    // Allocate flat operand mapping
    const size_t operand_slots = (size_t)DXBC_MAX_OPERANDS * 4U;
    if (dxbc_size_multiply_overflows((size_t)register_count, 4u)) {
        return false;
    }
    const size_t phi_slots_per_block = (size_t)register_count * 4u;
    const size_t block_count_size = (size_t)cfg->block_count;
    if (dxbc_size_multiply_overflows((size_t)count, operand_slots) ||
        dxbc_size_multiply_overflows(
            block_count_size, phi_slots_per_block)) {
        return false;
    }
    size_t operand_count = (size_t)count * operand_slots;
    size_t phi_slots = block_count_size * phi_slots_per_block;
    if (dxbc_size_add_overflows(operand_count, phi_slots) ||
        dxbc_size_multiply_overflows(operand_count, sizeof(int)) ||
        dxbc_size_multiply_overflows(
            operand_count + phi_slots, sizeof(int)) ||
        dxbc_size_multiply_overflows(
            block_count_size, sizeof(HLSLBlockPhis)) ||
        dxbc_size_multiply_overflows(
            block_count_size, sizeof(int *)) ||
        dxbc_size_multiply_overflows(
            block_count_size, sizeof(int))) {
        return false;
    }
    size_t max_ssa_vars = operand_count + phi_slots;
    if (max_ssa_vars > INT_MAX) return false;
    ctx->ssa.operand_ssa_vars = malloc(operand_count * sizeof(int));
    ctx->ssa.ssa_var_defs = malloc(max_ssa_vars * sizeof(int));
    ctx->ssa.block_phis = calloc(block_count_size, sizeof(HLSLBlockPhis));
    if (!ctx->ssa.operand_ssa_vars || !ctx->ssa.ssa_var_defs || !ctx->ssa.block_phis) {
        free_hlsl_ssa_graph(ctx);
        return false;
    }
    ctx->ssa.instruction_count = count;
    ctx->ssa.ssa_var_count = 0;

    for (size_t i = 0; i < operand_count; i++) {
        ctx->ssa.operand_ssa_vars[i] = -1;
    }

    // Phis placement (Iterated Dominance Frontier)
    bool *has_phi = calloc(block_count_size, sizeof(bool));
    bool *visited = calloc(block_count_size, sizeof(bool));
    int *worklist = malloc(block_count_size * sizeof(int));
    if (!has_phi || !visited || !worklist) {
        free(has_phi);
        free(visited);
        free(worklist);
        free_hlsl_ssa_graph(ctx);
        return false;
    }

    for (int reg = 0; reg < register_count; reg++) {
        for (int comp = 0; comp < 4; comp++) {
            int work_count = 0;
            memset(has_phi, 0, block_count_size * sizeof(bool));
            memset(visited, 0, block_count_size * sizeof(bool));

            // Find all blocks defining (reg, comp)
            for (int b = 0; b < cfg->block_count; b++) {
                bool defined = false;
                int start = cfg->blocks[b].first_instruction;
                int end = cfg->blocks[b].last_instruction;
                for (int inst_idx = start; inst_idx <= end; inst_idx++) {
                    if (instruction_defines_reg_comp(
                            ctx->program, &ctx->program->instructions[inst_idx],
                            reg, comp)) {
                        defined = true;
                        break;
                    }
                }
                if (defined) {
                    worklist[work_count++] = b;
                    visited[b] = true;
                }
            }

            // Iterate frontiers to place Phis
            int head = 0;
            while (head < work_count) {
                int n = worklist[head++];
                int df_size = cfg->df_count[n];
                for (int d = 0; d < df_size; d++) {
                    int y = cfg->df[n][d];
                    if (!has_phi[y]) {
                        if (!add_phi_node(&ctx->ssa.block_phis[y], reg, comp,
                                          cfg->blocks[y].predecessor_count)) {
                            free(has_phi);
                            free(visited);
                            free(worklist);
                            free_hlsl_ssa_graph(ctx);
                            return false;
                        }
                        has_phi[y] = true;
                        if (!visited[y]) {
                            visited[y] = true;
                            worklist[work_count++] = y;
                        }
                    }
                }
            }
        }
    }

    free(has_phi);
    free(visited);
    free(worklist);

    // Build dominator tree children array
    int *dom_child_counts = (int *)calloc(block_count_size, sizeof(int));
    int **dom_children = (int **)malloc(block_count_size * sizeof(int *));
    if (!dom_child_counts || !dom_children) {
        free(dom_child_counts);
        free(dom_children);
        free_hlsl_ssa_graph(ctx);
        return false;
    }
    for (int i = 0; i < cfg->block_count; i++) {
        int parent = cfg->idom[i];
        if (parent >= 0 && parent != i) {
            dom_child_counts[parent]++;
        }
    }
    for (int i = 0; i < cfg->block_count; i++) {
        dom_children[i] = dom_child_counts[i] > 0 ? (int *)malloc((size_t)dom_child_counts[i] * sizeof(int)) : NULL;
        if (dom_child_counts[i] > 0 && !dom_children[i]) {
            for (int j = 0; j < i; j++) free(dom_children[j]);
            free(dom_child_counts);
            free(dom_children);
            free_hlsl_ssa_graph(ctx);
            return false;
        }
        dom_child_counts[i] = 0; // Reset to use as index
    }
    for (int i = 0; i < cfg->block_count; i++) {
        int parent = cfg->idom[i];
        if (parent >= 0 && parent != i) {
            dom_children[parent][dom_child_counts[parent]++] = i;
        }
    }

    // Perform dominator tree traversal for variable renaming
    int (*active_version)[4] = NULL;
    if (register_count > 0) {
        if (dxbc_size_multiply_overflows((size_t)register_count,
                                         sizeof(*active_version))) {
            goto rename_cleanup_failure;
        }
        active_version = malloc((size_t)register_count *
                                sizeof(*active_version));
        if (!active_version) goto rename_cleanup_failure;
    }
    for (int i = 0; i < register_count; i++) {
        for (int j = 0; j < 4; j++) {
            active_version[i][j] = -1;
        }
    }
    int next_ssa_var = 0;
    if (!rename_ssa(ctx, dom_children, dom_child_counts, active_version, &next_ssa_var)) {
        free(active_version);
        goto rename_cleanup_failure;
    }
    free(active_version);
    ctx->ssa.ssa_var_count = next_ssa_var;

    // Cleanup local mappings
    for (int i = 0; i < cfg->block_count; i++) free(dom_children[i]);
    free(dom_children);
    free(dom_child_counts);

    return true;

rename_cleanup_failure:
    for (int i = 0; i < cfg->block_count; i++) free(dom_children[i]);
    free(dom_children);
    free(dom_child_counts);
    free_hlsl_ssa_graph(ctx);
    return false;
}

void free_hlsl_ssa_graph(HLSLEmitterContext *ctx) {
    HLSLSSAGraph *ssa = &ctx->ssa;
    if (ssa->block_phis) {
        for (int i = 0; i < ctx->cfg.block_count; i++) {
            HLSLBlockPhis *bp = &ssa->block_phis[i];
            if (bp->phis) {
                for (int j = 0; j < bp->phi_count; j++) {
                    free(bp->phis[j].incoming_vars);
                    free(bp->phis[j].incoming_blocks);
                }
                free(bp->phis);
            }
        }
        free(ssa->block_phis);
    }
    free(ssa->operand_ssa_vars);
    free(ssa->ssa_var_defs);
    memset(ssa, 0, sizeof(*ssa));
}
