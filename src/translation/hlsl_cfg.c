// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdlib.h>
#include <string.h>

static bool is_boundary(USILOpcode opcode) {
    switch (opcode) {
        case USIL_OP_IF:
        case USIL_OP_ELSE:
        case USIL_OP_ENDIF:
        case USIL_OP_LOOP:
        case USIL_OP_ENDLOOP:
        case USIL_OP_SWITCH:
        case USIL_OP_CASE:
        case USIL_OP_DEFAULT:
        case USIL_OP_ENDSWITCH:
        case USIL_OP_BREAK:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUE:
        case USIL_OP_CONTINUEC:
        case USIL_OP_RET:
        case USIL_OP_DISCARD:
        case USIL_OP_GEOMETRY_APPEND:
        case USIL_OP_GEOMETRY_RESTART_STRIP:
            return true;
        default:
            return false;
    }
}

static void add_successor(HLSLControlFlowGraph *cfg, int from,
                          int instruction) {
    if (from < 0 || from >= cfg->block_count || instruction < 0) return;
    int to = cfg->instruction_block[instruction];
    if (to < 0 || to >= cfg->block_count || to == from) return;
    HLSLBasicBlock *block = &cfg->blocks[from];
    for (int item = 0; item < block->successor_count; item++)
        if (block->successors[item] == to) return;
    if (block->successor_count >= 3) return;
    block->successors[block->successor_count++] = to;
    cfg->blocks[to].predecessor_count++;
}

static int matching_forward(const USILProgram *program, int start,
                            USILOpcode open, int alternate,
                            USILOpcode close) {
    int depth = 0;
    for (int index = start + 1; index < program->instruction_count; index++) {
        USILOpcode opcode = program->instructions[index].opcode;
        if (opcode == open) depth++;
        else if (opcode == close) {
            if (depth == 0) return index;
            depth--;
        } else if (alternate >= 0 && opcode == (USILOpcode)alternate &&
                   depth == 0) {
            return index;
        }
    }
    return -1;
}

static int containing_loop(const USILProgram *program, int instruction) {
    int depth = 0;
    for (int index = instruction - 1; index >= 0; index--) {
        USILOpcode opcode = program->instructions[index].opcode;
        if (opcode == USIL_OP_ENDLOOP) depth++;
        else if (opcode == USIL_OP_LOOP) {
            if (depth == 0) return index;
            depth--;
        }
    }
    return -1;
}

bool build_control_flow_graph(HLSLEmitterContext *ctx) {
    if (!ctx || !ctx->program || ctx->program->instruction_count < 0)
        return false;
    const USILProgram *program = ctx->program;
    int count = program->instruction_count;
    HLSLControlFlowGraph *cfg = &ctx->cfg;
    if (count == 0) return true;
    bool *leader = (bool *)calloc((size_t)count, sizeof(bool));
    cfg->instruction_block = (int *)malloc((size_t)count * sizeof(int));
    cfg->blocks = (HLSLBasicBlock *)calloc((size_t)count,
                                           sizeof(HLSLBasicBlock));
    if (!leader || !cfg->instruction_block || !cfg->blocks) {
        free(leader);
        free_control_flow_graph(ctx);
        return false;
    }
    leader[0] = true;
    for (int index = 0; index < count; index++) {
        if (!is_boundary(program->instructions[index].opcode)) continue;
        leader[index] = true;
        if (index + 1 < count) leader[index + 1] = true;
    }
    for (int index = 0; index < count; index++) {
        if (!leader[index]) continue;
        int block = cfg->block_count++;
        cfg->blocks[block].first_instruction = index;
        if (block > 0) cfg->blocks[block - 1].last_instruction = index - 1;
    }
    cfg->blocks[cfg->block_count - 1].last_instruction = count - 1;
    for (int block = 0; block < cfg->block_count; block++)
        for (int index = cfg->blocks[block].first_instruction;
             index <= cfg->blocks[block].last_instruction; index++)
            cfg->instruction_block[index] = block;
    free(leader);

    for (int block = 0; block < cfg->block_count; block++) {
        int last = cfg->blocks[block].last_instruction;
        USILOpcode opcode = program->instructions[last].opcode;
        if (opcode == USIL_OP_RET) continue;
        if (opcode == USIL_OP_SWITCH || opcode == USIL_OP_CASE ||
            opcode == USIL_OP_DEFAULT || opcode == USIL_OP_ENDSWITCH) {
            /* Switch dispatch has an arbitrary number of targets. Until the
             * graph stores variable-length edge lists, never certify a
             * single-path proof across any part of a switch. */
            cfg->blocks[block].has_ambiguous_flow = true;
            if (last + 1 < count) add_successor(cfg, block, last + 1);
            continue;
        }
        if (opcode == USIL_OP_DISCARD) {
            /* The taken edge terminates and therefore cannot contribute a
             * reaching definition at a later instruction. The only
             * continuing edge is fallthrough. */
            if (last + 1 < count) add_successor(cfg, block, last + 1);
            continue;
        }
        if (opcode == USIL_OP_IF) {
            if (last + 1 < count) add_successor(cfg, block, last + 1);
            int target = matching_forward(program, last, USIL_OP_IF,
                                          USIL_OP_ELSE, USIL_OP_ENDIF);
            if (target >= 0 && target + 1 < count)
                add_successor(cfg, block, target + 1);
            continue;
        }
        if (opcode == USIL_OP_ELSE) {
            int target = matching_forward(program, last, USIL_OP_IF,
                                          -1, USIL_OP_ENDIF);
            if (target >= 0 && target + 1 < count)
                add_successor(cfg, block, target + 1);
            continue;
        }
        if (opcode == USIL_OP_BREAK || opcode == USIL_OP_BREAKC ||
            opcode == USIL_OP_CONTINUE || opcode == USIL_OP_CONTINUEC) {
            int loop = containing_loop(program, last);
            int end = loop >= 0 ? matching_forward(
                                      program, loop, USIL_OP_LOOP,
                                      -1, USIL_OP_ENDLOOP)
                                : -1;
            if (opcode == USIL_OP_BREAKC || opcode == USIL_OP_CONTINUEC)
                if (last + 1 < count) add_successor(cfg, block, last + 1);
            int target = (opcode == USIL_OP_CONTINUE ||
                          opcode == USIL_OP_CONTINUEC)
                             ? end
                             : end + 1;
            if (target >= 0 && target < count)
                add_successor(cfg, block, target);
            continue;
        }
        if (opcode == USIL_OP_ENDLOOP) {
            int loop = containing_loop(program, last);
            if (loop >= 0 && loop + 1 < count)
                add_successor(cfg, block, loop + 1);
            if (last + 1 < count) add_successor(cfg, block, last + 1);
            continue;
        }
        if (last + 1 < count) add_successor(cfg, block, last + 1);
    }
    return true;
}

bool instructions_have_unambiguous_path(const HLSLEmitterContext *ctx,
                                        int first, int second) {
    if (!ctx->cfg.instruction_block || first < 0 || second < first ||
        second >= ctx->program->instruction_count)
        return false;
    int first_block = ctx->cfg.instruction_block[first];
    int second_block = ctx->cfg.instruction_block[second];
    if (first_block == second_block) return true;
    for (int block = first_block; block < second_block; block++) {
        const HLSLBasicBlock *current = &ctx->cfg.blocks[block];
        const HLSLBasicBlock *next = &ctx->cfg.blocks[block + 1];
        if (current->has_ambiguous_flow || next->has_ambiguous_flow ||
            current->successor_count != 1 ||
            current->successors[0] != block + 1 ||
            next->predecessor_count != 1)
            return false;
    }
    return true;
}

void free_control_flow_graph(HLSLEmitterContext *ctx) {
    free(ctx->cfg.blocks);
    free(ctx->cfg.instruction_block);
    free(ctx->cfg.idom);
    if (ctx->cfg.df) {
        for (int i = 0; i < ctx->cfg.block_count; i++) {
            free(ctx->cfg.df[i]);
        }
        free(ctx->cfg.df);
    }
    free(ctx->cfg.df_count);
    free(ctx->cfg.nesting);
    memset(&ctx->cfg, 0, sizeof(ctx->cfg));
}

static void dfs_postorder(const HLSLControlFlowGraph *cfg, int block, bool *visited, int *postorder, int *postorder_count) {
    visited[block] = true;
    for (int s = 0; s < cfg->blocks[block].successor_count; s++) {
        int succ = cfg->blocks[block].successors[s];
        if (!visited[succ]) {
            dfs_postorder(cfg, succ, visited, postorder, postorder_count);
        }
    }
    postorder[(*postorder_count)++] = block;
}

static int intersect(int b1, int b2, const int *postorder_numbers, const int *idom) {
    int finger1 = b1;
    int finger2 = b2;
    while (finger1 != finger2) {
        while (finger1 >= 0 && postorder_numbers[finger1] < postorder_numbers[finger2]) {
            finger1 = idom[finger1];
        }
        if (finger1 < 0) return finger2;
        while (finger2 >= 0 && postorder_numbers[finger2] < postorder_numbers[finger1]) {
            finger2 = idom[finger2];
        }
        if (finger2 < 0) return finger1;
    }
    return finger1;
}

bool compute_dominance(HLSLControlFlowGraph *cfg) {
    if (!cfg || cfg->block_count < 0 ||
        dxbc_size_multiply_overflows(
            (size_t)cfg->block_count, sizeof(int)) ||
        dxbc_size_multiply_overflows(
            (size_t)cfg->block_count, sizeof(int *)) ||
        dxbc_size_multiply_overflows(
            (size_t)cfg->block_count, sizeof(bool))) {
        return false;
    }
    if (cfg->block_count == 0) return true;

    // 1. Build predecessor lists
    int **preds = (int **)malloc((size_t)cfg->block_count * sizeof(int *));
    int *pred_idx = (int *)calloc((size_t)cfg->block_count, sizeof(int));
    if (!preds || !pred_idx) {
        free(preds);
        free(pred_idx);
        return false;
    }
    for (int i = 0; i < cfg->block_count; i++) {
        int p_count = cfg->blocks[i].predecessor_count;
        preds[i] = p_count > 0 ? (int *)malloc((size_t)p_count * sizeof(int)) : NULL;
        if (p_count > 0 && !preds[i]) {
            for (int j = 0; j < i; j++) free(preds[j]);
            free(preds);
            free(pred_idx);
            return false;
        }
    }
    for (int i = 0; i < cfg->block_count; i++) {
        for (int s = 0; s < cfg->blocks[i].successor_count; s++) {
            int succ = cfg->blocks[i].successors[s];
            preds[succ][pred_idx[succ]++] = i;
        }
    }

    // 2. Perform DFS to get postorder numbering
    bool *visited = (bool *)calloc((size_t)cfg->block_count, sizeof(bool));
    int *postorder = (int *)malloc((size_t)cfg->block_count * sizeof(int));
    int *postorder_numbers = (int *)malloc((size_t)cfg->block_count * sizeof(int));
    if (!visited || !postorder || !postorder_numbers) {
        free(visited);
        free(postorder);
        free(postorder_numbers);
        for (int i = 0; i < cfg->block_count; i++) free(preds[i]);
        free(preds);
        free(pred_idx);
        return false;
    }
    for (int i = 0; i < cfg->block_count; i++) {
        postorder_numbers[i] = -1;
    }
    int postorder_count = 0;
    dfs_postorder(cfg, 0, visited, postorder, &postorder_count);
    for (int i = 0; i < postorder_count; i++) {
        postorder_numbers[postorder[i]] = i;
    }

    // 3. Compute immediate dominators (Cooper, Harvey, Kennedy algorithm)
    int *idom = (int *)malloc((size_t)cfg->block_count * sizeof(int));
    if (!idom) {
        free(visited);
        free(postorder);
        free(postorder_numbers);
        for (int i = 0; i < cfg->block_count; i++) free(preds[i]);
        free(preds);
        free(pred_idx);
        return false;
    }
    for (int i = 0; i < cfg->block_count; i++) {
        idom[i] = -1;
    }
    idom[0] = 0; // Entry dominates itself

    bool changed = true;
    while (changed) {
        changed = false;
        // Iterate in reverse postorder (starting after the entry block)
        for (int i = postorder_count - 2; i >= 0; i--) {
            int b = postorder[i];
            int new_idom = -1;
            for (int p_idx = 0; p_idx < cfg->blocks[b].predecessor_count; p_idx++) {
                int p = preds[b][p_idx];
                if (idom[p] != -1) {
                    new_idom = p;
                    break;
                }
            }
            if (new_idom != -1) {
                for (int p_idx = 0; p_idx < cfg->blocks[b].predecessor_count; p_idx++) {
                    int p = preds[b][p_idx];
                    if (p != new_idom && idom[p] != -1) {
                        new_idom = intersect(new_idom, p, postorder_numbers, idom);
                    }
                }
                if (idom[b] != new_idom) {
                    idom[b] = new_idom;
                    changed = true;
                }
            }
        }
    }

    // 4. Compute Dominance Frontiers
    int **df = (int **)calloc((size_t)cfg->block_count, sizeof(int *));
    int *df_count = (int *)calloc((size_t)cfg->block_count, sizeof(int));
    if (!df || !df_count) {
        free(df);
        free(df_count);
        free(idom);
        free(visited);
        free(postorder);
        free(postorder_numbers);
        for (int i = 0; i < cfg->block_count; i++) free(preds[i]);
        free(preds);
        free(pred_idx);
        return false;
    }
    for (int i = 0; i < cfg->block_count; i++) {
        df[i] = (int *)malloc((size_t)cfg->block_count * sizeof(int));
        if (!df[i]) {
            for (int j = 0; j < i; j++) free(df[j]);
            free(df);
            free(df_count);
            free(idom);
            free(visited);
            free(postorder);
            free(postorder_numbers);
            for (int k = 0; k < cfg->block_count; k++) free(preds[k]);
            free(preds);
            free(pred_idx);
            return false;
        }
    }

    for (int b = 0; b < cfg->block_count; b++) {
        if (cfg->blocks[b].predecessor_count >= 2) {
            for (int p_idx = 0; p_idx < cfg->blocks[b].predecessor_count; p_idx++) {
                int runner = preds[b][p_idx];
                while (runner >= 0 && runner != idom[b]) {
                    bool already_exists = false;
                    for (int k = 0; k < df_count[runner]; k++) {
                        if (df[runner][k] == b) {
                            already_exists = true;
                            break;
                        }
                    }
                    if (!already_exists) {
                        df[runner][df_count[runner]++] = b;
                    }
                    runner = idom[runner];
                }
            }
        }
    }

    // 5. Store in CFG
    cfg->idom = idom;
    cfg->df = df;
    cfg->df_count = df_count;

    // 6. Cleanup local structures
    free(visited);
    free(postorder);
    free(postorder_numbers);
    for (int i = 0; i < cfg->block_count; i++) free(preds[i]);
    free(preds);
    free(pred_idx);
    return true;
}

bool analyze_block_nesting(HLSLEmitterContext *ctx) {
    HLSLControlFlowGraph *cfg = &ctx->cfg;
    if (cfg->block_count == 0) return true;
    cfg->nesting = calloc((size_t)cfg->block_count, sizeof(HLSLBlockNesting));
    if (!cfg->nesting) return false;

    int *stack = malloc((size_t)cfg->block_count * sizeof(int));
    if (!stack) {
        free(cfg->nesting);
        cfg->nesting = NULL;
        return false;
    }
    int stack_depth = 0;

    for (int block = 0; block < cfg->block_count; block++) {
        int first_inst = cfg->blocks[block].first_instruction;
        USILOpcode first_op = ctx->program->instructions[first_inst].opcode;

        if (first_op == USIL_OP_ENDIF || first_op == USIL_OP_ENDLOOP) {
            if (stack_depth > 0) stack_depth--;
        }

        if (stack_depth > 0) {
            int parent = stack[stack_depth - 1];
            USILOpcode parent_op = ctx->program->instructions[cfg->blocks[parent].first_instruction].opcode;
            cfg->nesting[block].parent_block = parent;
            cfg->nesting[block].is_loop = (parent_op == USIL_OP_LOOP);
            cfg->nesting[block].is_if = (parent_op == USIL_OP_IF || parent_op == USIL_OP_ELSE);
        } else {
            cfg->nesting[block].parent_block = -1;
            cfg->nesting[block].is_loop = false;
            cfg->nesting[block].is_if = false;
        }

        int last_inst = cfg->blocks[block].last_instruction;
        USILOpcode last_op = ctx->program->instructions[last_inst].opcode;
        if (last_op == USIL_OP_LOOP || last_op == USIL_OP_IF || last_op == USIL_OP_ELSE) {
            stack[stack_depth++] = block;
        }
    }
    free(stack);
    return true;
}
