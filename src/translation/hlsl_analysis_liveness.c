// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"

#include <stdint.h>
#include <stdlib.h>

void free_hlsl_generation_state(HLSLEmitterContext* ctx) {
  if (!ctx) return;
  free(ctx->inst_src_gen);
  free(ctx->inst_dest_gen);
  free(ctx->reg_max_gen);
  ctx->inst_src_gen = NULL;
  ctx->inst_dest_gen = NULL;
  ctx->reg_max_gen = NULL;
  ctx->generation_instruction_count = 0;
  ctx->generation_register_count = 0;
}

bool allocate_hlsl_generation_state(HLSLEmitterContext* ctx,
                                    HLSLCallocFunction allocate_zeroed) {
  if (!ctx || !ctx->program || !allocate_zeroed) return false;
  free_hlsl_generation_state(ctx);

  int instruction_count = ctx->program->instruction_count;
  int register_count = ctx->program->temp_count;
  if (instruction_count < 0 || register_count < 0) return false;
  if (instruction_count > 0 && register_count > 0 &&
      (size_t)instruction_count > SIZE_MAX / (size_t)register_count)
    return false;
  size_t cell_count = (size_t)instruction_count * (size_t)register_count;
  if (cell_count > SIZE_MAX / sizeof(int)) return false;

  int* source = NULL;
  int* destination = NULL;
  int* maximum = NULL;
  if (cell_count > 0) {
    source = (int*)allocate_zeroed(cell_count, sizeof(*source));
    if (!source) goto fail;
    destination =
        (int*)allocate_zeroed(cell_count, sizeof(*destination));
    if (!destination) goto fail;
  }
  if (register_count > 0) {
    maximum =
        (int*)allocate_zeroed((size_t)register_count, sizeof(*maximum));
    if (!maximum) goto fail;
  }

  ctx->inst_src_gen = source;
  ctx->inst_dest_gen = destination;
  ctx->reg_max_gen = maximum;
  ctx->generation_instruction_count = instruction_count;
  ctx->generation_register_count = register_count;
  return true;

fail:
  free(source);
  free(destination);
  free(maximum);
  return false;
}

bool analyze_live_ranges(HLSLEmitterContext* ctx) {
  // Preserve the decoded DXBC register file exactly. Synthetic linear
  // generations are not SSA phi nodes and change D3DCompiler's deterministic
  // component packing, register allocation, and control-flow semantics.
  return allocate_hlsl_generation_state(ctx, calloc);
}

int hlsl_instruction_generation(HLSLEmitterContext* ctx, bool destination,
                                int instruction, int reg) {
  if (!ctx || instruction < 0 || reg < 0 ||
      instruction >= ctx->generation_instruction_count ||
      reg >= ctx->generation_register_count) {
    if (ctx && ctx->sb) ctx->sb->failed = true;
    return 0;
  }
  size_t index = (size_t)instruction *
                     (size_t)ctx->generation_register_count +
                 (size_t)reg;
  return destination ? ctx->inst_dest_gen[index] : ctx->inst_src_gen[index];
}

int hlsl_register_max_generation(HLSLEmitterContext* ctx, int reg) {
  if (!ctx || reg < 0 || reg >= ctx->generation_register_count ||
      !ctx->reg_max_gen) {
    if (ctx && ctx->sb) ctx->sb->failed = true;
    return 0;
  }
  return ctx->reg_max_gen[reg];
}
