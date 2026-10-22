// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

static bool discard_cfg_matches(const HLSLEmitterContext *ctx) {
    const HLSLControlFlowGraph *cfg = &ctx->cfg;
    if (cfg->instruction_count != 5 || cfg->block_count != 4 || !cfg->blocks ||
        !cfg->instruction_block || !cfg->idom || !cfg->nesting) {
        return false;
    }
    static const int first[4] = {0, 2, 3, 4};
    static const int last[4] = {1, 2, 3, 4};
    for (int index = 0; index < 4; ++index) {
        const HLSLBasicBlock *block = &cfg->blocks[index];
        if (block->first_instruction != first[index] || block->last_instruction != last[index] ||
            block->has_ambiguous_flow || block->may_exit != (index == 1 || index == 3) ||
            cfg->idom[index] != (index ? index - 1 : 0) || cfg->nesting[index].parent_block != -1 ||
            cfg->nesting[index].is_loop || cfg->nesting[index].is_if ||
            block->predecessor_count != (index ? 1 : 0) ||
            (index && (!block->predecessors || block->predecessors[0] != index - 1)) ||
            block->successor_count != (index == 3 ? 0 : 1) ||
            (index != 3 && (!block->successors || block->successors[0] != index + 1))) {
            return false;
        }
        for (int instruction = first[index]; instruction <= last[index]; ++instruction) {
            if (cfg->instruction_block[instruction] != index) {
                return false;
            }
        }
    }
    return true;
}

/* LT supplies a canonical Boolean mask, consumed only by the conditional kill.
 * The sample remains one reaching float4 value on the surviving path. */
bool hlsl_sampled_discard_preflight(const HLSLEmitterContext *ctx) {
    HLSLSampledDiscardPlan plan;
    if (!ctx || ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
        !hlsl_sampled_discard_plan_prepare(ctx->program, &plan) ||
        ctx->compiler_model.replacement_count || !discard_cfg_matches(ctx) ||
        ctx->ssa.instruction_count != 5 || ctx->ssa.ssa_var_count < 1 ||
        !ctx->ssa.operand_ssa_vars || !ctx->ssa.relative_operand_ssa_vars ||
        !ctx->ssa.ssa_var_defs || !ctx->ssa.block_phis || ctx->use_def.instruction_count != 5 ||
        !ctx->use_def.operand_definitions || !ctx->use_def.definition_use_counts) {
        return false;
    }
    for (int block = 0; block < 4; ++block) {
        if (ctx->ssa.block_phis[block].phi_count) {
            return false;
        }
    }
    static const USILEffectFlags effects[5] = {USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_QUAD_CONTEXT,
        USIL_EFFECT_NONE,
        USIL_EFFECT_CONTROL | USIL_EFFECT_QUAD_CONTEXT,
        USIL_EFFECT_NONE,
        USIL_EFFECT_CONTROL};
    static const int source_definitions[5] = {-1, 0, 1, 0, -1};
    unsigned uses[5][4] = {{0}};
    for (int index = 0; index < 5; ++index) {
        const USILInstruction *instruction = &ctx->program->instructions[index];
        USILEffectFlags observed;
        if (!usil_instruction_effects(ctx->program, instruction, &observed) ||
            observed != effects[index]) {
            return false;
        }
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            USILOperandUseInfo use;
            if (!usil_instruction_operand_use(ctx->program, instruction, operand, &use)) {
                return false;
            }
            for (int lane = 0; lane < 4; ++lane) {
                const size_t offset =
                    ((size_t)index * DXBC_MAX_OPERANDS + (unsigned)operand) * 4u + (unsigned)lane;
                if (ctx->ssa.relative_operand_ssa_vars[offset] >= 0) {
                    return false;
                }
                if (instruction->operands[operand].type != OPERAND_TYPE_TEMP) {
                    continue;
                }
                const bool destination = use.use == USIL_OPERAND_USE_DESTINATION;
                const uint8_t lanes = destination ? usil_operand_destination_lane_mask(
                                                        &instruction->operands[operand])
                                                  : use.source_lane_mask;
                if (!(lanes & (1u << lane))) {
                    continue;
                }
                const int variable = ctx->ssa.operand_ssa_vars[offset];
                const int expected = destination ? index : source_definitions[index];
                if (expected < 0 || variable < 0 || variable >= ctx->ssa.ssa_var_count ||
                    ctx->ssa.ssa_var_defs[variable] != expected ||
                    hlsl_operand_definition(ctx, index, operand, lane) != expected) {
                    return false;
                }
                if (!destination) {
                    const unsigned component =
                        usil_operand_source_component(&instruction->operands[operand], lane);
                    if (component > 3 || ctx->use_def.operand_definitions[offset] != expected) {
                        return false;
                    }
                    ++uses[expected][component];
                }
            }
        }
    }
    for (int instruction = 0; instruction < 5; ++instruction) {
        for (int lane = 0; lane < 4; ++lane) {
            if (hlsl_definition_use_count(ctx, instruction, lane) != uses[instruction][lane]) {
                return false;
            }
        }
    }
    return true;
}
