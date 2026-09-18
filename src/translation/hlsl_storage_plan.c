// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdlib.h>

static bool instruction_requires_raw_storage(
    const USILInstruction *instruction) {
    switch (instruction->opcode) {
        case USIL_OP_AND:
        case USIL_OP_OR:
        case USIL_OP_XOR:
        case USIL_OP_NOT:
        case USIL_OP_ISHL:
        case USIL_OP_ISHR:
        case USIL_OP_USHR:
        case USIL_OP_UBFE:
            return true;
        case USIL_OP_SAMPLEINFO:
            return instruction->sample_info_return_type == 1u;
        default:
            return false;
    }
}

bool build_hlsl_storage_plan(HLSLEmitterContext *ctx) {
    int register_count = ctx->program->temp_count > 0
                             ? ctx->program->temp_count
                             : 1;
    HLSLStoragePlan *plan = &ctx->storage_plan;
    plan->register_count = register_count;
    plan->register_storage =
        (unsigned char *)calloc((size_t)register_count,
                                sizeof(unsigned char));
    if (!plan->register_storage) return false;
    bool requires_raw_default = false;
    for (int index = 0; index < ctx->program->instruction_count; index++) {
        const USILInstruction *inst = &ctx->program->instructions[index];
        if (!instruction_requires_raw_storage(inst)) continue;
        requires_raw_default = true;
        for (int operand = 0; operand < inst->operand_count; operand++) {
            const DXBCOperand *value = &inst->operands[operand];
            if (value->type == OPERAND_TYPE_TEMP &&
                value->register_index >= 0 &&
                value->register_index < register_count)
                plan->register_storage[value->register_index] =
                    HLSL_TEMP_STORAGE_RAW_UINT;
        }
    }
    plan->default_storage = requires_raw_default
                                ? HLSL_TEMP_STORAGE_RAW_UINT
                                : HLSL_TEMP_STORAGE_FLOAT;
    /* Preserve the established whole-program storage ABI while emitters are
     * migrated to the per-register plan. This is a storage decision, not a
     * value-type inference. */
    ctx->use_uint_temps =
        plan->default_storage == HLSL_TEMP_STORAGE_RAW_UINT;
    return true;
}

bool temp_register_requires_raw_storage(const HLSLEmitterContext *ctx,
                                        int reg) {
    return ctx->storage_plan.register_storage && reg >= 0 &&
           reg < ctx->storage_plan.register_count &&
           ctx->storage_plan.register_storage[reg] ==
               HLSL_TEMP_STORAGE_RAW_UINT;
}

void free_hlsl_storage_plan(HLSLEmitterContext *ctx) {
    free(ctx->storage_plan.register_storage);
    ctx->storage_plan.register_storage = NULL;
    ctx->storage_plan.register_count = 0;
    ctx->storage_plan.default_storage = HLSL_TEMP_STORAGE_FLOAT;
}
