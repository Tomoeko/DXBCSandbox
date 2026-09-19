// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <limits.h>
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

/* Structured DXBC has two edges per conditional and one edge per switch
 * label. A single owned array keeps arbitrary switch fan-out bounded by the
 * instruction count without allocating a separate list for every block. */
static bool add_successor(HLSLControlFlowGraph *cfg, int from, int instruction,
                          int instruction_count, size_t *edge_count) {
    if (instruction == instruction_count)
        return true;
    if (from < 0 || from >= cfg->block_count || instruction < 0 || instruction >= instruction_count)
        return false;
    int to = cfg->instruction_block[instruction];
    if (to < 0 || to >= cfg->block_count)
        return false;
    HLSLBasicBlock *block = &cfg->blocks[from];
    for (int item = 0; item < block->successor_count; ++item)
        if (block->successors[item] == to)
            return true;
    if (*edge_count >= (size_t)instruction_count * 3u || block->successor_count == INT_MAX ||
        cfg->blocks[to].predecessor_count == INT_MAX)
        return false;
    block->successors[block->successor_count++] = to;
    ++*edge_count;
    ++cfg->blocks[to].predecessor_count;
    return true;
}

typedef struct {
    int end;
    int alternate;
    int parent;
    int jump_scope;
    int first_case;
    int next_case;
    bool has_default;
} FlowStructure;

/* Match once, preserving the nearest breakable scope independently from the
 * nearest loop. SWITCH inside LOOP must not turn its BREAK into a loop exit.
 * Shader Model 4/5 defines at most 64 nested flow-control constructs. */
static bool match_flow_structure(const USILProgram *program, FlowStructure *flow) {
    enum { MAX_FLOW_DEPTH = 64 };
    int stack[MAX_FLOW_DEPTH];
    int last_case[MAX_FLOW_DEPTH];
    int depth = 0;
    for (int index = 0; index < program->instruction_count; ++index) {
        flow[index].end = flow[index].alternate = flow[index].parent = -1;
        flow[index].jump_scope = flow[index].first_case = flow[index].next_case = -1;
    }
    for (int index = 0; index < program->instruction_count; ++index) {
        const USILOpcode opcode = program->instructions[index].opcode;
        const int scope = depth ? stack[depth - 1] : -1;
        const USILOpcode enclosing = scope >= 0 ? program->instructions[scope].opcode : USIL_OP_NOP;
        flow[index].parent =
            scope >= 0 && flow[scope].alternate >= 0 ? flow[scope].alternate : scope;
        if (opcode == USIL_OP_IF || opcode == USIL_OP_LOOP || opcode == USIL_OP_SWITCH) {
            if (depth == MAX_FLOW_DEPTH)
                return false;
            stack[depth] = index;
            last_case[depth++] = -1;
        } else if (opcode == USIL_OP_ELSE) {
            if (enclosing != USIL_OP_IF || flow[scope].alternate >= 0)
                return false;
            flow[scope].alternate = index;
            flow[index].parent = flow[scope].parent;
        } else if (opcode == USIL_OP_ENDIF || opcode == USIL_OP_ENDLOOP ||
                   opcode == USIL_OP_ENDSWITCH) {
            const USILOpcode required = opcode == USIL_OP_ENDIF     ? USIL_OP_IF
                                        : opcode == USIL_OP_ENDLOOP ? USIL_OP_LOOP
                                                                    : USIL_OP_SWITCH;
            if (enclosing != required)
                return false;
            flow[scope].end = index;
            if (flow[scope].alternate >= 0)
                flow[flow[scope].alternate].end = index;
            flow[index].jump_scope = scope;
            flow[index].parent = flow[scope].parent;
            --depth;
        } else if (opcode == USIL_OP_CASE || opcode == USIL_OP_DEFAULT) {
            if (enclosing != USIL_OP_SWITCH ||
                (opcode == USIL_OP_DEFAULT && flow[scope].has_default))
                return false;
            if (opcode == USIL_OP_DEFAULT)
                flow[scope].has_default = true;
            if (last_case[depth - 1] >= 0)
                flow[last_case[depth - 1]].next_case = index;
            else
                flow[scope].first_case = index;
            last_case[depth - 1] = index;
        } else if (opcode == USIL_OP_BREAK || opcode == USIL_OP_BREAKC ||
                   opcode == USIL_OP_CONTINUE || opcode == USIL_OP_CONTINUEC) {
            const bool loop_only = opcode == USIL_OP_CONTINUE || opcode == USIL_OP_CONTINUEC;
            for (int item = depth - 1; item >= 0; --item) {
                const USILOpcode kind = program->instructions[stack[item]].opcode;
                if (kind == USIL_OP_LOOP || (!loop_only && kind == USIL_OP_SWITCH)) {
                    flow[index].jump_scope = stack[item];
                    break;
                }
            }
            if (flow[index].jump_scope < 0)
                return false;
        }
    }
    return depth == 0;
}

bool build_control_flow_graph(HLSLEmitterContext *ctx) {
    if (!ctx || !ctx->program || ctx->program->instruction_count < 0)
        return false;
    const USILProgram *program = ctx->program;
    const int count = program->instruction_count;
    HLSLControlFlowGraph *cfg = &ctx->cfg;
    if (count == 0)
        return true;
    if (!program->instructions || cfg->blocks || cfg->instruction_block || cfg->successor_storage ||
        cfg->predecessor_storage ||
        dxbc_size_multiply_overflows((size_t)count, sizeof(FlowStructure)) ||
        dxbc_size_multiply_overflows((size_t)count, sizeof(HLSLBasicBlock)) ||
        dxbc_size_multiply_overflows((size_t)count, 3u * sizeof(int)))
        return false;
    bool *leader = calloc((size_t)count, sizeof(*leader));
    FlowStructure *flow = calloc((size_t)count, sizeof(*flow));
    cfg->instruction_block = malloc((size_t)count * sizeof(*cfg->instruction_block));
    cfg->successor_storage = malloc((size_t)count * 3u * sizeof(*cfg->successor_storage));
    cfg->blocks = calloc((size_t)count, sizeof(*cfg->blocks));
    if (!leader || !flow || !cfg->instruction_block || !cfg->successor_storage || !cfg->blocks ||
        !match_flow_structure(program, flow))
        goto fail;
    leader[0] = true;
    for (int index = 0; index < count; ++index) {
        if (!is_boundary(program->instructions[index].opcode))
            continue;
        leader[index] = true;
        if (index + 1 < count)
            leader[index + 1] = true;
    }
    for (int index = 0; index < count; ++index) {
        if (!leader[index])
            continue;
        int block = cfg->block_count++;
        cfg->blocks[block].first_instruction = index;
        if (block > 0)
            cfg->blocks[block - 1].last_instruction = index - 1;
    }
    cfg->blocks[cfg->block_count - 1].last_instruction = count - 1;
    for (int block = 0; block < cfg->block_count; ++block)
        for (int index = cfg->blocks[block].first_instruction;
             index <= cfg->blocks[block].last_instruction; ++index)
            cfg->instruction_block[index] = block;
    cfg->nesting = calloc((size_t)cfg->block_count, sizeof(*cfg->nesting));
    if (!cfg->nesting)
        goto fail;

    size_t edge_count = 0;
    for (int block = 0; block < cfg->block_count; ++block) {
        const int first = cfg->blocks[block].first_instruction;
        const int parent = flow[first].parent;
        cfg->nesting[block].parent_block = parent >= 0 ? cfg->instruction_block[parent] : -1;
        if (parent >= 0) {
            const USILOpcode parent_opcode = program->instructions[parent].opcode;
            cfg->nesting[block].is_loop = parent_opcode == USIL_OP_LOOP;
            cfg->nesting[block].is_if =
                parent_opcode == USIL_OP_IF || parent_opcode == USIL_OP_ELSE;
        }
        cfg->blocks[block].successors = cfg->successor_storage + edge_count;
        const int last = cfg->blocks[block].last_instruction;
        const USILOpcode opcode = program->instructions[last].opcode;
#define EDGE(target)                                                                               \
    do {                                                                                           \
        if (!add_successor(cfg, block, (target), count, &edge_count))                              \
            goto fail;                                                                             \
    } while (0)
        if (opcode == USIL_OP_RET)
            continue;
        if (opcode == USIL_OP_SWITCH) {
            /* Keep the existing conservative single-path recognizer boundary,
             * even though SSA now receives every real dispatch edge. */
            cfg->blocks[block].has_ambiguous_flow = true;
            for (int label = flow[last].first_case; label >= 0; label = flow[label].next_case)
                EDGE(label);
            if (!flow[last].has_default)
                EDGE(flow[last].end + 1);
        } else if (opcode == USIL_OP_IF) {
            EDGE(last + 1);
            EDGE((flow[last].alternate >= 0 ? flow[last].alternate : flow[last].end) + 1);
        } else if (opcode == USIL_OP_ELSE) {
            EDGE(flow[last].end + 1);
        } else if (opcode == USIL_OP_BREAK || opcode == USIL_OP_BREAKC ||
                   opcode == USIL_OP_CONTINUE || opcode == USIL_OP_CONTINUEC) {
            if (opcode == USIL_OP_BREAKC || opcode == USIL_OP_CONTINUEC)
                EDGE(last + 1);
            const int end = flow[flow[last].jump_scope].end;
            EDGE((opcode == USIL_OP_CONTINUE || opcode == USIL_OP_CONTINUEC) ? end : end + 1);
        } else if (opcode == USIL_OP_ENDLOOP) {
            /* ENDLOOP repeats unconditionally. An empty body is a self-edge;
             * only an explicit break can reach the following instruction. */
            EDGE(flow[last].jump_scope + 1);
        } else {
            if (opcode == USIL_OP_CASE || opcode == USIL_OP_DEFAULT || opcode == USIL_OP_ENDSWITCH)
                cfg->blocks[block].has_ambiguous_flow = true;
            /* DISCARD's taken edge terminates; only fallthrough contributes
             * a reaching definition to a later instruction. */
            EDGE(last + 1);
        }
#undef EDGE
    }
    cfg->predecessor_storage = malloc((edge_count ? edge_count : 1u) * sizeof(int));
    if (!cfg->predecessor_storage)
        goto fail;
    size_t predecessor_offset = 0;
    for (int block = 0; block < cfg->block_count; ++block) {
        HLSLBasicBlock *value = &cfg->blocks[block];
        value->predecessors = cfg->predecessor_storage + predecessor_offset;
        predecessor_offset += (size_t)value->predecessor_count;
        value->predecessor_count = 0;
    }
    for (int block = 0; block < cfg->block_count; ++block) {
        const HLSLBasicBlock *value = &cfg->blocks[block];
        for (int edge = 0; edge < value->successor_count; ++edge) {
            HLSLBasicBlock *target = &cfg->blocks[value->successors[edge]];
            target->predecessors[target->predecessor_count++] = block;
        }
    }
    free(leader);
    free(flow);
    return true;
fail:
    free(leader);
    free(flow);
    free_control_flow_graph(ctx);
    return false;
}

bool instructions_have_unambiguous_path(const HLSLEmitterContext *ctx, int first, int second) {
    if (!ctx->cfg.instruction_block || first < 0 || second < first ||
        second >= ctx->program->instruction_count)
        return false;
    int first_block = ctx->cfg.instruction_block[first];
    int second_block = ctx->cfg.instruction_block[second];
    if (first_block == second_block)
        return true;
    for (int block = first_block; block < second_block; block++) {
        const HLSLBasicBlock *current = &ctx->cfg.blocks[block];
        const HLSLBasicBlock *next = &ctx->cfg.blocks[block + 1];
        if (current->has_ambiguous_flow || next->has_ambiguous_flow ||
            current->successor_count != 1 || current->successors[0] != block + 1 ||
            next->predecessor_count != 1)
            return false;
    }
    return true;
}

void free_control_flow_graph(HLSLEmitterContext *ctx) {
    free(ctx->cfg.blocks);
    free(ctx->cfg.successor_storage);
    free(ctx->cfg.predecessor_storage);
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

typedef struct {
    int block;
    int next_successor;
} DFSFrame;

static bool build_postorder(const HLSLControlFlowGraph *cfg, int *postorder, int *postorder_count) {
    if (dxbc_size_multiply_overflows((size_t)cfg->block_count, sizeof(DFSFrame)))
        return false;
    DFSFrame *stack = calloc((size_t)cfg->block_count, sizeof(*stack));
    bool *visited = calloc((size_t)cfg->block_count, sizeof(*visited));
    if (!stack || !visited) {
        free(stack);
        free(visited);
        return false;
    }
    int depth = 1;
    visited[0] = true;
    while (depth) {
        DFSFrame *frame = &stack[depth - 1];
        const HLSLBasicBlock *block = &cfg->blocks[frame->block];
        if (frame->next_successor == block->successor_count) {
            postorder[(*postorder_count)++] = frame->block;
            --depth;
        } else {
            const int successor = block->successors[frame->next_successor++];
            if (!visited[successor]) {
                visited[successor] = true;
                stack[depth++] = (DFSFrame){.block = successor};
            }
        }
    }
    free(stack);
    free(visited);
    return true;
}

static int intersect(int b1, int b2, const int *postorder_numbers, const int *idom) {
    int finger1 = b1;
    int finger2 = b2;
    while (finger1 != finger2) {
        while (finger1 >= 0 && postorder_numbers[finger1] < postorder_numbers[finger2]) {
            finger1 = idom[finger1];
        }
        if (finger1 < 0)
            return finger2;
        while (finger2 >= 0 && postorder_numbers[finger2] < postorder_numbers[finger1]) {
            finger2 = idom[finger2];
        }
        if (finger2 < 0)
            return finger1;
    }
    return finger1;
}

static bool add_frontier(int **values, int *count, int block) {
    for (int index = 0; index < *count; ++index)
        if ((*values)[index] == block)
            return true;
    if (*count == INT_MAX || dxbc_size_multiply_overflows((size_t)*count + 1u, sizeof(int)))
        return false;
    int *replacement = realloc(*values, ((size_t)*count + 1u) * sizeof(int));
    if (!replacement)
        return false;
    *values = replacement;
    (*values)[(*count)++] = block;
    return true;
}

bool compute_dominance(HLSLControlFlowGraph *cfg) {
    if (!cfg || cfg->block_count < 0 || cfg->idom || cfg->df || cfg->df_count ||
        dxbc_size_multiply_overflows((size_t)cfg->block_count, sizeof(int)) ||
        dxbc_size_multiply_overflows((size_t)cfg->block_count, sizeof(int *)))
        return false;
    if (cfg->block_count == 0)
        return true;
    if (!cfg->blocks || !cfg->successor_storage || !cfg->predecessor_storage)
        return false;
    const size_t count = (size_t)cfg->block_count;
    int *postorder = malloc(count * sizeof(int));
    int *numbers = malloc(count * sizeof(int));
    int *idom = malloc(count * sizeof(int));
    int **frontiers = calloc(count, sizeof(*frontiers));
    int *frontier_counts = calloc(count, sizeof(*frontier_counts));
    bool success = false;
    if (!postorder || !numbers || !idom || !frontiers || !frontier_counts)
        goto cleanup;
    for (int block = 0; block < cfg->block_count; ++block) {
        numbers[block] = -1;
        idom[block] = -1;
    }
    int postorder_count = 0;
    if (!build_postorder(cfg, postorder, &postorder_count))
        goto cleanup;
    for (int index = 0; index < postorder_count; ++index)
        numbers[postorder[index]] = index;
    idom[0] = 0;

    /* Cooper/Harvey/Kennedy immediate dominators in reverse postorder.
     * Unreachable blocks retain -1 and cannot establish a definition. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int index = postorder_count - 2; index >= 0; --index) {
            const int block_index = postorder[index];
            const HLSLBasicBlock *block = &cfg->blocks[block_index];
            int parent = -1;
            for (int edge = 0; edge < block->predecessor_count; ++edge) {
                const int predecessor = block->predecessors[edge];
                if (idom[predecessor] < 0)
                    continue;
                parent = parent < 0 ? predecessor : intersect(parent, predecessor, numbers, idom);
            }
            if (parent != idom[block_index]) {
                idom[block_index] = parent;
                changed = true;
            }
        }
    }
    for (int block_index = 0; block_index < cfg->block_count; ++block_index) {
        const HLSLBasicBlock *block = &cfg->blocks[block_index];
        if (idom[block_index] < 0 || block->predecessor_count < 2)
            continue;
        for (int edge = 0; edge < block->predecessor_count; ++edge) {
            int runner = block->predecessors[edge];
            if (idom[runner] < 0)
                continue;
            while (runner != idom[block_index]) {
                if (!add_frontier(&frontiers[runner], &frontier_counts[runner], block_index))
                    goto cleanup;
                runner = idom[runner];
            }
        }
    }
    cfg->idom = idom;
    cfg->df = frontiers;
    cfg->df_count = frontier_counts;
    success = true;
cleanup:
    free(postorder);
    free(numbers);
    if (!success) {
        if (frontiers)
            for (size_t block = 0; block < count; ++block)
                free(frontiers[block]);
        free(frontiers);
        free(frontier_counts);
        free(idom);
    }
    return success;
}

bool analyze_block_nesting(HLSLEmitterContext *ctx) {
    /* The CFG matcher owns the single structured-flow interpretation. ELSE
     * changes the active arm; it does not introduce another nested scope. */
    return ctx && ctx->cfg.block_count >= 0 &&
           (ctx->cfg.block_count == 0 || ctx->cfg.nesting != NULL);
}
