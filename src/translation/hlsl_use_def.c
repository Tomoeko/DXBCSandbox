// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <stdlib.h>
#include <string.h>

static size_t operand_offset(int instruction, int operand, int component) {
    return (((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u) +
           (size_t)component;
}

bool build_hlsl_use_def_graph(HLSLEmitterContext *ctx) {
    HLSLUseDefGraph *graph = &ctx->use_def;
    int count = ctx->program->instruction_count;
    size_t operand_count =
        (size_t)count * DXBC_MAX_OPERANDS * 4u;
    graph->operand_definitions =
        (int *)malloc(operand_count * sizeof(int));
    graph->definition_use_counts =
        (unsigned int *)calloc((size_t)count * 4u, sizeof(unsigned int));
    if (!graph->operand_definitions || !graph->definition_use_counts) {
        free_hlsl_use_def_graph(ctx);
        return false;
    }
    graph->instruction_count = count;
    for (size_t item = 0; item < operand_count; item++)
        graph->operand_definitions[item] = HLSL_DEFINITION_UNKNOWN;

    for (int index = 0; index < count; index++) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        for (int operand_index = 0; operand_index < inst->operand_count;
             ++operand_index) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, inst, operand_index, &use)) {
                free_hlsl_use_def_graph(ctx);
                return false;
            }
            const DXBCOperand *operand = &inst->operands[operand_index];
            if (use.use != USIL_OPERAND_USE_SOURCE ||
                operand->type != OPERAND_TYPE_TEMP) continue;
            for (int component = 0; component < 4; component++) {
                if (!(use.source_lane_mask & (1u << component))) continue;
                int source = usil_operand_source_component(operand, component);
                if (source < 0) {
                    free_hlsl_use_def_graph(ctx);
                    return false;
                }
                int definition = HLSL_DEFINITION_UNKNOWN;
                if (ctx->ssa.operand_ssa_vars) {
                    definition = hlsl_operand_definition(ctx, index,
                                                          operand_index, component);
                } else {
                    const HLSLComponentProvenance *provenance =
                        get_component_provenance(ctx, index,
                                                 operand->register_index, source);
                    if (provenance && provenance->definition_instruction >= 0) {
                        definition = provenance->definition_instruction;
                        if (!instructions_have_unambiguous_path(ctx, definition, index))
                            definition = HLSL_DEFINITION_AMBIGUOUS;
                    }
                }
                graph->operand_definitions[operand_offset(
                    index, operand_index, component)] = definition;
                if (definition >= 0)
                    graph->definition_use_counts[
                        (size_t)definition * 4u + (size_t)source]++;
            }
        }
    }
    /* Phi edges consume their incoming definitions too. Ignoring those
     * edges would make a branch-local value look dead or single-use. */
    if (ctx->ssa.block_phis) {
        for (int block = 0; block < ctx->cfg.block_count; ++block) {
            const HLSLBlockPhis *phis = &ctx->ssa.block_phis[block];
            for (int index = 0; index < phis->phi_count; ++index) {
                const HLSLPhiNode *phi = &phis->phis[index];
                for (int edge = 0; edge < ctx->cfg.blocks[block].predecessor_count;
                     ++edge) {
                    int variable = phi->incoming_vars[edge];
                    if (variable < 0 || variable >= ctx->ssa.ssa_var_count) continue;
                    int definition = ctx->ssa.ssa_var_defs[variable];
                    if (definition >= 0 && definition < count) {
                        graph->definition_use_counts[(size_t)definition * 4u +
                                                      (size_t)phi->component]++;
                    }
                }
            }
        }
    }
    return true;
}

int hlsl_operand_definition(const HLSLEmitterContext *ctx, int instruction,
                            int operand, int component) {
    if (!ctx || instruction < 0 || operand < 0 ||
        operand >= DXBC_MAX_OPERANDS || component < 0 || component >= 4)
        return HLSL_DEFINITION_UNKNOWN;
    if (ctx->ssa.operand_ssa_vars && instruction < ctx->ssa.instruction_count) {
        int ssa_var = ctx->ssa.operand_ssa_vars[
            (((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u) +
            (size_t)component];
        if (ssa_var >= 0 && ssa_var < ctx->ssa.ssa_var_count)
            return ctx->ssa.ssa_var_defs[ssa_var];
        /* Undefined or unused SSA lanes cannot inherit a linear-scan value. */
        return HLSL_DEFINITION_UNKNOWN;
    }
    if (ctx->use_def.operand_definitions && instruction < ctx->use_def.instruction_count) {
        return ctx->use_def.operand_definitions[
            (((size_t)instruction * DXBC_MAX_OPERANDS + (size_t)operand) * 4u) +
            (size_t)component];
    }
    return HLSL_DEFINITION_UNKNOWN;
}

unsigned int hlsl_definition_use_count(const HLSLEmitterContext *ctx,
                                       int instruction, int component) {
    if (!ctx || !ctx->use_def.definition_use_counts || instruction < 0 ||
        instruction >= ctx->use_def.instruction_count || component < 0 ||
        component >= 4)
        return 0;
    return ctx->use_def.definition_use_counts[
        (size_t)instruction * 4u + (size_t)component];
}

void free_hlsl_use_def_graph(HLSLEmitterContext *ctx) {
    free(ctx->use_def.operand_definitions);
    free(ctx->use_def.definition_use_counts);
    memset(&ctx->use_def, 0, sizeof(ctx->use_def));
}
