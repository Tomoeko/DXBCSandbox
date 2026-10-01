// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "translation/hlsl_global_declarations.h"
#include "common/sha256.h"
#include "hlsl_geometry_flow.h"
#include "translation/usil_validation.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool hlsl_emit_with_options_impl(
    const USILProgram *program, StringBuilder *sb,
    const SerializedProgramParameters *params,
    const SerializedProgramParameters *common_params,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    HLSLMatrixUseCapture *matrix_capture, HLSLStageCoverage *stage_coverage,
    HLSLEmitDiagnostic *diagnostic, bool packed_replay);

static void hlsl_emit_metadata_location_init(
    HLSLEmitMetadataLocation *location) {
  if (!location) return;
  location->source = HLSL_EMIT_METADATA_SOURCE_NONE;
  location->kind = HLSL_EMIT_METADATA_NONE;
  location->record_index = -1;
  location->member_index = -1;
  location->register_index = -1;
}

void hlsl_emit_diagnostic_init(HLSLEmitDiagnostic *diagnostic) {
  if (!diagnostic) return;
  memset(diagnostic, 0, sizeof(*diagnostic));
  diagnostic->status = HLSL_EMIT_STATUS_OK;
  diagnostic->phase = HLSL_EMIT_PHASE_NONE;
  diagnostic->reason = HLSL_EMIT_REASON_NONE;
  diagnostic->instruction_index = -1;
  diagnostic->source_instruction_index = UINT32_MAX;
  diagnostic->opcode = -1;
  diagnostic->operand_index = -1;
  hlsl_emit_metadata_location_init(&diagnostic->metadata);
  hlsl_emit_metadata_location_init(&diagnostic->related_metadata);
}

const char *hlsl_emit_status_name(HLSLEmitStatus status) {
  switch (status) {
    case HLSL_EMIT_STATUS_OK: return "ok";
    case HLSL_EMIT_STATUS_INVALID_ARGUMENT: return "invalid-argument";
    case HLSL_EMIT_STATUS_INVALID_PROGRAM: return "invalid-program";
    case HLSL_EMIT_STATUS_UNSUPPORTED: return "unsupported";
    case HLSL_EMIT_STATUS_INVALID_METADATA: return "invalid-metadata";
    case HLSL_EMIT_STATUS_ALLOCATION_FAILED: return "allocation-failed";
    case HLSL_EMIT_STATUS_ANALYSIS_FAILED: return "analysis-failed";
    case HLSL_EMIT_STATUS_INTERNAL_INVARIANT: return "internal-invariant";
    case HLSL_EMIT_STATUS_OUTPUT_FAILED: return "output-failed";
  }
  return "unknown";
}

const char *hlsl_emit_phase_name(HLSLEmitPhase phase) {
  switch (phase) {
    case HLSL_EMIT_PHASE_NONE: return "none";
    case HLSL_EMIT_PHASE_ARGUMENT_VALIDATION: return "argument-validation";
    case HLSL_EMIT_PHASE_PROGRAM_VALIDATION: return "program-validation";
    case HLSL_EMIT_PHASE_CONTEXT_ALLOCATION: return "context-allocation";
    case HLSL_EMIT_PHASE_STATE_ALLOCATION: return "state-allocation";
    case HLSL_EMIT_PHASE_CBUFFER_BINDING_MAP: return "cbuffer-binding-map";
    case HLSL_EMIT_PHASE_CBUFFER_LAYOUT: return "cbuffer-layout";
    case HLSL_EMIT_PHASE_SAMPLER_BINDING_MAP: return "sampler-binding-map";
    case HLSL_EMIT_PHASE_LIVE_RANGE_ANALYSIS: return "live-range-analysis";
    case HLSL_EMIT_PHASE_CONTROL_FLOW_ANALYSIS: return "control-flow-analysis";
    case HLSL_EMIT_PHASE_DOMINANCE_ANALYSIS: return "dominance-analysis";
    case HLSL_EMIT_PHASE_BLOCK_NESTING_ANALYSIS: return "block-nesting-analysis";
    case HLSL_EMIT_PHASE_SSA_ANALYSIS: return "ssa-analysis";
    case HLSL_EMIT_PHASE_PROVENANCE_ANALYSIS: return "provenance-analysis";
    case HLSL_EMIT_PHASE_USE_DEF_ANALYSIS: return "use-def-analysis";
    case HLSL_EMIT_PHASE_VALUE_ANALYSIS: return "value-analysis";
    case HLSL_EMIT_PHASE_STORAGE_PLANNING: return "storage-planning";
    case HLSL_EMIT_PHASE_SEMANTIC_ANALYSIS: return "semantic-analysis";
    case HLSL_EMIT_PHASE_COMPILER_MODEL_ANALYSIS: return "compiler-model-analysis";
    case HLSL_EMIT_PHASE_TESSELLATION_EMISSION: return "tessellation-emission";
    case HLSL_EMIT_PHASE_ICB_EMISSION: return "icb-emission";
    case HLSL_EMIT_PHASE_CBUFFER_EMISSION: return "cbuffer-emission";
    case HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION: return "cbuffer-helper-emission";
    case HLSL_EMIT_PHASE_RESOURCE_EMISSION: return "resource-emission";
    case HLSL_EMIT_PHASE_STRUCTURAL_HELPER_EMISSION: return "structural-helper-emission";
    case HLSL_EMIT_PHASE_INTERFACE_EMISSION: return "interface-emission";
    case HLSL_EMIT_PHASE_ENTRY_POINT_EMISSION: return "entry-point-emission";
    case HLSL_EMIT_PHASE_INSTRUCTION_EMISSION: return "instruction-emission";
    case HLSL_EMIT_PHASE_RETURN_EMISSION: return "return-emission";
    case HLSL_EMIT_PHASE_OUTPUT: return "output";
  }
  return "unknown";
}

const char *hlsl_emit_reason_name(HLSLEmitReason reason) {
  switch (reason) {
    case HLSL_EMIT_REASON_NONE: return "none";
    case HLSL_EMIT_REASON_INVALID_ARGUMENT: return "invalid-argument";
    case HLSL_EMIT_REASON_OUTPUT_ALREADY_FAILED: return "output-already-failed";
    case HLSL_EMIT_REASON_INVALID_MODE: return "invalid-mode";
    case HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE: return "invalid-program-shape";
    case HLSL_EMIT_REASON_INVALID_SIGNATURE: return "invalid-signature";
    case HLSL_EMIT_REASON_UNSUPPORTED_STAGE: return "unsupported-stage";
    case HLSL_EMIT_REASON_UNSUPPORTED_FEATURE: return "unsupported-feature";
    case HLSL_EMIT_REASON_UNSUPPORTED_OPCODE: return "unsupported-opcode";
    case HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE: return "invalid-instruction-shape";
    case HLSL_EMIT_REASON_INVALID_OPERAND: return "invalid-operand";
    case HLSL_EMIT_REASON_MISSING_BINDING: return "missing-binding";
    case HLSL_EMIT_REASON_RESOURCE_CONTRACT_MISMATCH: return "resource-contract-mismatch";
    case HLSL_EMIT_REASON_INVALID_METADATA_SHAPE: return "invalid-metadata-shape";
    case HLSL_EMIT_REASON_MISSING_METADATA_AUTHORITY: return "missing-metadata-authority";
    case HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY: return "conflicting-metadata-authority";
    case HLSL_EMIT_REASON_INVALID_PARAMETER_LAYOUT: return "invalid-parameter-layout";
    case HLSL_EMIT_REASON_BUILTIN_CONTRACT_MISMATCH: return "builtin-contract-mismatch";
    case HLSL_EMIT_REASON_SAMPLER_CONTRACT_MISMATCH: return "sampler-contract-mismatch";
    case HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT: return "unrepresentable-layout";
    case HLSL_EMIT_REASON_ANALYSIS_CONFLICT: return "analysis-conflict";
    case HLSL_EMIT_REASON_SIZE_OVERFLOW: return "size-overflow";
    case HLSL_EMIT_REASON_ALLOCATION_FAILED: return "allocation-failed";
    case HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW: return "fixed-buffer-overflow";
    case HLSL_EMIT_REASON_LOWERING_FAILED: return "lowering-failed";
    case HLSL_EMIT_REASON_INTERNAL_INVARIANT: return "internal-invariant";
    case HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED: return "output-builder-failed";
  }
  return "unknown";
}

const char *hlsl_emit_metadata_source_name(HLSLEmitMetadataSource source) {
  switch (source) {
    case HLSL_EMIT_METADATA_SOURCE_NONE: return "none";
    case HLSL_EMIT_METADATA_SOURCE_PROGRAM: return "program";
    case HLSL_EMIT_METADATA_SOURCE_STAGE_PARAMETERS: return "stage-parameters";
    case HLSL_EMIT_METADATA_SOURCE_COMMON_PARAMETERS: return "common-parameters";
  }
  return "unknown";
}

const char *hlsl_emit_metadata_kind_name(HLSLEmitMetadataKind kind) {
  switch (kind) {
    case HLSL_EMIT_METADATA_NONE: return "none";
    case HLSL_EMIT_METADATA_INPUT_SIGNATURE: return "input-signature";
    case HLSL_EMIT_METADATA_OUTPUT_SIGNATURE: return "output-signature";
    case HLSL_EMIT_METADATA_PATCH_SIGNATURE: return "patch-signature";
    case HLSL_EMIT_METADATA_CBUFFER: return "constant-buffer";
    case HLSL_EMIT_METADATA_CBUFFER_VARIABLE: return "constant-buffer-variable";
    case HLSL_EMIT_METADATA_RESOURCE: return "resource";
    case HLSL_EMIT_METADATA_TEXTURE: return "texture";
    case HLSL_EMIT_METADATA_SAMPLER: return "sampler";
    case HLSL_EMIT_METADATA_UAV: return "uav";
    case HLSL_EMIT_METADATA_INDEXABLE_TEMP: return "indexable-temp";
  }
  return "unknown";
}

const char *hlsl_emit_opcode_name(int opcode) {
  static const char *const names[] = {
      "nop", "add", "sub", "mul", "div", "mad", "mov", "movc",
      "dp2", "dp3", "dp4", "rcp", "rsq", "sqrt", "min", "max",
      "lt", "ge", "eq", "ne", "ilt", "ige", "ieq", "ine", "ult",
      "uge", "and", "or", "xor", "not", "ishl", "ishr", "ushr",
      "ftoi", "ftou", "itof", "utof", "sample", "sample-c",
      "sample-c-lz", "sample-l", "sample-d", "sample-b", "ld",
      "ld-structured", "ld-ms", "resinfo", "sampleinfo", "log", "exp",
      "sin", "cos", "frc", "round-ne", "round-ni", "round-pi",
      "round-z", "if", "else", "endif", "loop", "endloop", "switch",
      "case", "default", "endswitch", "break", "breakc", "continue",
      "continuec", "ret", "discard", "deriv-rtx", "deriv-rty",
      "deriv-rtx-coarse", "deriv-rty-coarse", "deriv-rtx-fine",
      "deriv-rty-fine", "iadd", "imul", "imad", "imax", "imin", "umax",
      "umin", "udiv", "ineg", "imm-atomic-iadd", "ldms", "sincos",
      "ubfe", "geometry-append", "geometry-restart-strip", "sync",
      "ld-uav-typed", "store-uav-typed", "ld-raw", "store-raw", "store-structured",
      "atomic-xor", "atomic-iadd", "imm-atomic-alloc", "imm-atomic-consume",
      "imm-atomic-cmp-exch"};
  _Static_assert(sizeof(names) / sizeof(names[0]) ==
                     (size_t)USIL_OP_IMM_ATOMIC_CMP_EXCH + 1u,
                 "HLSL diagnostic opcode names must cover every USIL opcode");
  const size_t count = sizeof(names) / sizeof(names[0]);
  return opcode >= 0 && (size_t)opcode < count ? names[opcode] : "unknown";
}

static bool hlsl_emit_set_failure(HLSLEmitDiagnostic *diagnostic,
                                  HLSLEmitStatus status,
                                  HLSLEmitPhase phase,
                                  HLSLEmitReason reason) {
  if (!diagnostic || diagnostic->status != HLSL_EMIT_STATUS_OK) return false;
  diagnostic->status = status;
  diagnostic->phase = phase;
  diagnostic->reason = reason;
  return true;
}

static bool hlsl_emit_set_instruction_failure(
    HLSLEmitDiagnostic *diagnostic, const USILProgram *program,
    HLSLEmitStatus status, HLSLEmitPhase phase, HLSLEmitReason reason,
    int instruction_index, int operand_index) {
  if (!hlsl_emit_set_failure(diagnostic, status, phase, reason)) return false;
  if (!program ||
      instruction_index < 0 ||
      instruction_index >= program->instruction_count) return true;
  const USILInstruction *instruction =
      &program->instructions[instruction_index];
  diagnostic->instruction_index = instruction_index;
  diagnostic->source_instruction_index = instruction->source_instruction_index;
  diagnostic->opcode = (int)instruction->opcode;
  diagnostic->operand_index = operand_index;
  return true;
}

static bool hlsl_emit_set_metadata_failure(
    HLSLEmitDiagnostic *diagnostic, HLSLEmitStatus status,
    HLSLEmitPhase phase, HLSLEmitReason reason,
    HLSLEmitMetadataSource source, HLSLEmitMetadataKind kind,
    int record_index, int member_index, int register_index) {
  if (!hlsl_emit_set_failure(diagnostic, status, phase, reason)) return false;
  diagnostic->metadata.source = source;
  diagnostic->metadata.kind = kind;
  diagnostic->metadata.record_index = record_index;
  diagnostic->metadata.member_index = member_index;
  diagnostic->metadata.register_index = register_index;
  return true;
}

static void hlsl_emit_set_related_metadata(
    HLSLEmitDiagnostic *diagnostic, HLSLEmitMetadataSource source,
    HLSLEmitMetadataKind kind, int record_index, int member_index,
    int register_index) {
  if (!diagnostic || diagnostic->status == HLSL_EMIT_STATUS_OK ||
      diagnostic->related_metadata.kind != HLSL_EMIT_METADATA_NONE) return;
  diagnostic->related_metadata.source = source;
  diagnostic->related_metadata.kind = kind;
  diagnostic->related_metadata.record_index = record_index;
  diagnostic->related_metadata.member_index = member_index;
  diagnostic->related_metadata.register_index = register_index;
}

static void hlsl_emit_set_operand_metadata(
    HLSLEmitDiagnostic *diagnostic, const USILProgram *program,
    const DXBCOperand *operand);

void hlsl_emit_fail(HLSLEmitterContext *ctx, HLSLEmitStatus status,
                    HLSLEmitPhase phase, HLSLEmitReason reason) {
  if (!ctx) return;
  hlsl_emit_set_failure(ctx->diagnostic, status, phase, reason);
  if (ctx->sb) ctx->sb->failed = true;
}

void hlsl_emit_fail_instruction(HLSLEmitterContext *ctx,
                                HLSLEmitStatus status,
                                HLSLEmitReason reason,
                                int instruction_index,
                                int operand_index) {
  if (!ctx) return;
  const bool first_failure = hlsl_emit_set_instruction_failure(
      ctx->diagnostic, ctx->program, status,
      HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, reason, instruction_index,
      operand_index);
  if (first_failure && ctx->diagnostic && ctx->program &&
      instruction_index >= 0 &&
      instruction_index < ctx->program->instruction_count) {
    const USILInstruction *instruction =
        &ctx->program->instructions[instruction_index];
    if (operand_index >= 0 && operand_index < instruction->operand_count) {
      hlsl_emit_set_operand_metadata(ctx->diagnostic, ctx->program,
                                     &instruction->operands[operand_index]);
    } else {
      /* Resource authority is usually the actionable context for a lowering
       * failure; destination I/O is only the fallback when no binding-bearing
       * operand exists. */
      for (int index = 0; index < instruction->operand_count; ++index) {
        const DXBCOperandType type = instruction->operands[index].type;
        if (type != OPERAND_TYPE_CONSTANT_BUFFER &&
            type != OPERAND_TYPE_RESOURCE && type != OPERAND_TYPE_SAMPLER &&
            type != OPERAND_TYPE_UAV &&
            type != OPERAND_TYPE_INDEXABLE_TEMP) continue;
        hlsl_emit_set_operand_metadata(ctx->diagnostic, ctx->program,
                                       &instruction->operands[index]);
        if (ctx->diagnostic->metadata.kind != HLSL_EMIT_METADATA_NONE) break;
      }
      for (int index = 0;
           index < instruction->operand_count &&
           ctx->diagnostic->metadata.kind == HLSL_EMIT_METADATA_NONE;
           ++index) {
        hlsl_emit_set_operand_metadata(ctx->diagnostic, ctx->program,
                                       &instruction->operands[index]);
      }
    }
  }
  if (ctx->sb) ctx->sb->failed = true;
}

void hlsl_emit_fail_metadata(HLSLEmitterContext *ctx,
                             HLSLEmitStatus status,
                             HLSLEmitPhase phase,
                             HLSLEmitReason reason,
                             HLSLEmitMetadataSource source,
                             HLSLEmitMetadataKind kind,
                             int record_index, int member_index,
                             int register_index) {
  if (!ctx) return;
  hlsl_emit_set_metadata_failure(ctx->diagnostic, status, phase, reason,
                                 source, kind, record_index, member_index,
                                 register_index);
  if (ctx->sb) ctx->sb->failed = true;
}

bool hlsl_emit_check_output(HLSLEmitterContext *ctx, HLSLEmitPhase phase) {
  if (ctx && sb_ok(ctx->sb)) return true;
  if (ctx) {
    hlsl_emit_set_failure(ctx->diagnostic, HLSL_EMIT_STATUS_OUTPUT_FAILED,
                          phase, HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED);
  }
  return false;
}

static bool has_terminated_text(const char *text, size_t size) {
  return text && memchr(text, '\0', size) != NULL;
}

static bool is_depth_semantic(const DXBCSignatureElement *element) {
  const char *semantic = dxbc_signature_semantic_name(element);
  return element->register_id == UINT32_MAX &&
         semantic &&
         (strcmp(semantic, "SV_Depth") == 0 ||
          strcmp(semantic, "SV_DepthGreaterEqual") == 0 ||
          strcmp(semantic, "SV_DepthLessEqual") == 0);
}

static bool signature_semantic_is_valid(
    const DXBCSignatureElement *element) {
  const char *semantic = dxbc_signature_semantic_name(element);
  if (!element || !semantic) return false;
  if (!element->semantic_name_extended) {
    return has_terminated_text(element->semantic_name,
                               sizeof(element->semantic_name));
  }
  return element->semantic_name_length != SIZE_MAX &&
         semantic[element->semantic_name_length] == '\0' &&
         memchr(semantic, '\0', element->semantic_name_length) == NULL;
}

bool hlsl_signature_semantic_storage_valid(const DXBCSignatureElement *element) {
  return signature_semantic_is_valid(element);
}

static bool signature_has_register(const DXBCSignatureElement *elements,
                                   int count, int register_index) {
  for (int i = 0; i < count; i++) {
    if (elements[i].register_id == (uint32_t)register_index) return true;
  }
  return false;
}

static bool geometry_input_operand_supported(const USILProgram *program,
                                             const DXBCOperand *operand) {
  if (!program || !operand || !program->geometry.valid ||
      operand->register_index_dim != 2 || operand->rel_op1 ||
      operand->rel_op2 || !operand->index_has_immediate[1] ||
      operand->index_representations[1] != 0 ||
      operand->index_value_exceeds_int[1] ||
      operand->index_values[1] >= HLSL_SM5_IO_REGISTER_COUNT ||
      !signature_has_register(program->inputs, program->input_count,
                              (int)operand->index_values[1])) {
    return false;
  }
  if (!operand->rel_op0) {
    return operand->index_has_immediate[0] &&
           operand->index_representations[0] == 0 &&
           !operand->index_value_exceeds_int[0] &&
           operand->index_values[0] <
               program->geometry.input_vertex_count &&
           operand->register_index == (int)operand->index_values[0] &&
           operand->rel_offset0 == (int)operand->index_values[1];
  }

  /* Geometry input arrays are the one SM5 input class for which D3DCompiler
   * routinely emits a relative first dimension (`v[rN.c][register]`).  The
   * generic operand formatter does not yet expose that array expression, so
   * admit it only when the complete extrusion inverse consumes the graph. */
  const DXBCOperand *relative = operand->rel_op0;
  return hlsl_extruded_triangle_lift_matches(program) &&
         !operand->index_has_immediate[0] &&
         operand->index_representations[0] == 2 &&
         !operand->index_value_exceeds_int[0] &&
         operand->index_values[0] == 0 && operand->register_index == 0 &&
         operand->rel_offset0 == 0 &&
         relative->type == OPERAND_TYPE_TEMP &&
         relative->swizzle_mode == 2 && relative->swizzle[0] <= 3;
}

static bool program_has_binding(const USILProgram *program,
                                DXBCOperandType type, int register_index) {
  int count = 0;
  switch (type) {
    case OPERAND_TYPE_CONSTANT_BUFFER:
      count = program->cbuffer_count;
      for (int i = 0; i < count; i++)
        if (program->cbuffers[i].reg_idx == register_index) return true;
      break;
    case OPERAND_TYPE_SAMPLER:
      count = program->sampler_count;
      for (int i = 0; i < count; i++)
        if (program->samplers[i].reg_idx == register_index) return true;
      break;
    case OPERAND_TYPE_RESOURCE:
      count = program->texture_count;
      for (int i = 0; i < count; i++)
        if (program->textures[i].reg_idx == register_index) return true;
      break;
    case OPERAND_TYPE_INDEXABLE_TEMP:
      count = program->indexable_temp_count;
      for (int i = 0; i < count; i++)
        if (program->indexable_temps[i].reg_idx == register_index) return true;
      break;
    case OPERAND_TYPE_UAV:
      count = program->uav_count;
      for (int i = 0; i < count; i++)
        if (program->uavs[i].reg_idx == register_index) return true;
      break;
    default:
      break;
  }
  return false;
}

static HLSLEmitMetadataKind metadata_kind_for_operand(
    DXBCOperandType type) {
  switch (type) {
    case OPERAND_TYPE_INPUT: return HLSL_EMIT_METADATA_INPUT_SIGNATURE;
    case OPERAND_TYPE_OUTPUT:
    case OPERAND_TYPE_OUTPUT_DEPTH:
    case OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
    case OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL:
      return HLSL_EMIT_METADATA_OUTPUT_SIGNATURE;
    case OPERAND_TYPE_CONSTANT_BUFFER: return HLSL_EMIT_METADATA_CBUFFER;
    case OPERAND_TYPE_RESOURCE: return HLSL_EMIT_METADATA_TEXTURE;
    case OPERAND_TYPE_SAMPLER: return HLSL_EMIT_METADATA_SAMPLER;
    case OPERAND_TYPE_UAV: return HLSL_EMIT_METADATA_UAV;
    case OPERAND_TYPE_INDEXABLE_TEMP:
      return HLSL_EMIT_METADATA_INDEXABLE_TEMP;
    default: return HLSL_EMIT_METADATA_NONE;
  }
}

static int program_metadata_index_for_operand(const USILProgram *program,
                                              const DXBCOperand *operand) {
  if (!program || !operand) return -1;
#define FIND_REGISTER_INDEX(values, count)                                      \
  do {                                                                          \
    for (int index = 0; index < (count); ++index)                               \
      if ((values)[index].reg_idx == operand->register_index) return index;      \
  } while (0)
  switch (operand->type) {
    case OPERAND_TYPE_INPUT:
      for (int index = 0; index < program->input_count; ++index)
        if (program->inputs[index].register_id ==
            (uint32_t)operand->register_index) return index;
      break;
    case OPERAND_TYPE_OUTPUT:
    case OPERAND_TYPE_OUTPUT_DEPTH:
    case OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
    case OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL:
      for (int index = 0; index < program->output_count; ++index)
        if (program->outputs[index].register_id ==
            (uint32_t)operand->register_index ||
            ((operand->type == OPERAND_TYPE_OUTPUT_DEPTH ||
              operand->type == OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL ||
              operand->type == OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL) &&
             is_depth_semantic(&program->outputs[index]))) return index;
      break;
    case OPERAND_TYPE_CONSTANT_BUFFER:
      FIND_REGISTER_INDEX(program->cbuffers, program->cbuffer_count);
      break;
    case OPERAND_TYPE_RESOURCE:
      FIND_REGISTER_INDEX(program->textures, program->texture_count);
      break;
    case OPERAND_TYPE_SAMPLER:
      FIND_REGISTER_INDEX(program->samplers, program->sampler_count);
      break;
    case OPERAND_TYPE_UAV:
      FIND_REGISTER_INDEX(program->uavs, program->uav_count);
      break;
    case OPERAND_TYPE_INDEXABLE_TEMP:
      FIND_REGISTER_INDEX(program->indexable_temps,
                          program->indexable_temp_count);
      break;
    default:
      break;
  }
#undef FIND_REGISTER_INDEX
  return -1;
}

static void hlsl_emit_set_operand_metadata(
    HLSLEmitDiagnostic *diagnostic, const USILProgram *program,
    const DXBCOperand *operand) {
  if (!diagnostic || !operand ||
      diagnostic->metadata.kind != HLSL_EMIT_METADATA_NONE) return;
  HLSLEmitMetadataKind kind = metadata_kind_for_operand(operand->type);
  if (kind == HLSL_EMIT_METADATA_NONE) return;
  diagnostic->metadata.source = HLSL_EMIT_METADATA_SOURCE_PROGRAM;
  diagnostic->metadata.kind = kind;
  diagnostic->metadata.record_index =
      program_metadata_index_for_operand(program, operand);
  diagnostic->metadata.member_index = -1;
  diagnostic->metadata.register_index = operand->register_index;
}

static bool operand_has_dynamic_cbuffer_access(
    const DXBCOperand *operand, int register_index, unsigned int depth) {
  if (!operand || depth >= DXBC_MAX_NESTED_OPERAND_TOKENS) return false;
  if (operand->type == OPERAND_TYPE_CONSTANT_BUFFER &&
      operand->register_index == register_index &&
      operand->register_index_dim == 2 &&
      (operand->index_representations[1] == 2u ||
       operand->index_representations[1] == 3u ||
       operand->index_representations[1] == 4u)) {
    return true;
  }
  return operand_has_dynamic_cbuffer_access(operand->rel_op0,
                                            register_index, depth + 1) ||
         operand_has_dynamic_cbuffer_access(operand->rel_op1,
                                            register_index, depth + 1) ||
         operand_has_dynamic_cbuffer_access(operand->rel_op2,
                                            register_index, depth + 1);
}

static bool cbuffer_has_dynamic_access(const USILProgram *program,
                                       int register_index) {
  for (int instruction = 0; instruction < program->instruction_count;
       ++instruction) {
    const USILInstruction *value = &program->instructions[instruction];
    for (int operand = 0; operand < value->operand_count; ++operand) {
      if (operand_has_dynamic_cbuffer_access(&value->operands[operand],
                                             register_index, 0)) {
        return true;
      }
    }
  }
  return false;
}

static bool validate_operand_for_hlsl(const USILProgram *program,
                                      const DXBCOperand *operand,
                                      unsigned int depth) {
  if (!operand || depth >= DXBC_MAX_NESTED_OPERAND_TOKENS ||
      operand->swizzle_mode > 2 || operand->register_index_dim < 0 ||
      operand->register_index_dim > 2 || operand->imm_value_count < 0 ||
      operand->imm_value_count > 4 || operand->immediate_word_count < 0 ||
      operand->immediate_word_count > 8 || operand->rel_op2 ||
      operand->min_precision != 0u) {
    return false;
  }
  for (int component = 0; component < 4; component++) {
    if (operand->swizzle[component] > 3) return false;
  }
  for (int dimension = 0; dimension < operand->register_index_dim;
       dimension++) {
    uint8_t representation = operand->index_representations[dimension];
    /* The current HLSL formatter stores indices in int. Preserve the decoded
     * 64-bit value, but do not silently truncate it while emitting HLSL. */
    if (representation > 4 || representation == 1 || representation == 4 ||
        operand->index_value_exceeds_int[dimension]) {
      return false;
    }
  }
  if ((operand->rel_op0 &&
       !validate_operand_for_hlsl(program, operand->rel_op0, depth + 1)) ||
      (operand->rel_op1 &&
       !validate_operand_for_hlsl(program, operand->rel_op1, depth + 1))) {
    return false;
  }

  int reg = operand->register_index;
  switch (operand->type) {
    case OPERAND_TYPE_TEMP:
      return operand->register_index_dim == 1 && reg >= 0 &&
             reg < program->temp_count;
    case OPERAND_TYPE_INPUT:
      if (program->program_type == DXBC_PROGRAM_TYPE_GEOMETRY)
        return geometry_input_operand_supported(program, operand);
      return operand->register_index_dim == 1 && reg >= 0 &&
             reg < HLSL_SM5_IO_REGISTER_COUNT &&
             signature_has_register(program->inputs, program->input_count,
                                    reg);
    case OPERAND_TYPE_OUTPUT:
      return operand->register_index_dim == 1 && reg >= 0 &&
             reg < (strncmp(program->shader_type_model, "ps", 2) == 0
                        ? HLSL_SM5_PIXEL_OUTPUT_REGISTER_COUNT
                        : HLSL_SM5_IO_REGISTER_COUNT) &&
             signature_has_register(program->outputs, program->output_count,
                                    reg);
    case OPERAND_TYPE_INDEXABLE_TEMP:
      return operand->register_index_dim == 2 && reg >= 0 &&
             reg < HLSL_SM5_TEMP_REGISTER_COUNT &&
             program_has_binding(program, operand->type, reg);
    case OPERAND_TYPE_IMMEDIATE32:
      return operand->register_index_dim == 0 &&
             (operand->imm_value_count == 1 ||
              operand->imm_value_count == 4);
    case OPERAND_TYPE_SAMPLER:
      return operand->register_index_dim == 1 && reg >= 0 &&
             reg < HLSL_SM5_SAMPLER_REGISTER_COUNT &&
             program_has_binding(program, operand->type, reg);
    case OPERAND_TYPE_RESOURCE:
      return operand->register_index_dim == 1 && reg >= 0 &&
             reg < HLSL_SM5_RESOURCE_REGISTER_COUNT &&
             program_has_binding(program, operand->type, reg);
    case OPERAND_TYPE_UAV:
      return operand->register_index_dim == 1 && reg >= 0 &&
             reg < HLSL_SM5_UAV_REGISTER_COUNT &&
             program_has_binding(program, operand->type, reg);
    case OPERAND_TYPE_CONSTANT_BUFFER:
      return operand->register_index_dim == 2 && reg >= 0 &&
             reg < HLSL_SM5_CBUFFER_REGISTER_COUNT &&
             program_has_binding(program, operand->type, reg);
    case OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER:
      return operand->register_index_dim == 1 &&
             program->icb_value_count > 0;
    case OPERAND_TYPE_OUTPUT_DEPTH:
      if (operand->register_index_dim != 0) return false;
      for (int i = 0; i < program->output_count; i++) {
        if (is_depth_semantic(&program->outputs[i])) return true;
      }
      return false;
    case OPERAND_TYPE_NULL:
      /* Optional destinations use the null operand in imul, udiv, sincos and
       * atomics. The specialized opcode emitters consume it without indexing
       * a register file. */
      return operand->register_index_dim == 0;
    case OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
      return program->program_type == DXBC_PROGRAM_TYPE_GEOMETRY &&
             program->geometry.valid &&
             program->geometry.has_instance_count &&
             operand->register_index_dim == 0;
    case OPERAND_TYPE_STREAM:
      /* Stream operands are consumed by the geometry-effect emitter; the
       * USIL shape validator has already tied this register to the enclosing
       * emit/cut instruction and its explicit stream id. */
      return program->program_type == DXBC_PROGRAM_TYPE_GEOMETRY &&
             program->geometry.valid && operand->register_index_dim == 1 &&
             reg >= 0 && reg <= 3 && operand->index_has_immediate[0] &&
             operand->index_values[0] == (uint32_t)reg &&
             !operand->rel_op0 && !operand->rel_op1 && !operand->rel_op2;
    case OPERAND_TYPE_INPUT_CONTROL_POINT:
      return hlsl_high_level_domain_point_signature(program, operand) != NULL;
    case OPERAND_TYPE_DOMAIN_LOCATION:
      return hlsl_high_level_domain_interface_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) &&
          operand->register_index_dim == 0;
    case OPERAND_TYPE_IMMEDIATE64:
    case OPERAND_TYPE_LABEL:
    case OPERAND_TYPE_INPUT_COVERAGE_MASK:
    case OPERAND_TYPE_RASTERIZER:
    case OPERAND_TYPE_INNER_COVERAGE:
    case OPERAND_TYPE_FORK_INSTANCE_ID:
    case OPERAND_TYPE_OUTPUT_CONTROL_POINT:
    case OPERAND_TYPE_UNKNOWN:
    default:
      /* These classes currently fall through to synthetic `unk` variables in
       * the formatter. That is not a faithful representation, so reject the
       * program instead of emitting compilable but semantically false HLSL. */
      return false;
  }
}

static const USILTexture *find_texture_binding(const USILProgram *program,
                                               int register_index) {
  for (int index = 0; index < program->texture_count; ++index) {
    if (program->textures[index].reg_idx == register_index)
      return &program->textures[index];
  }
  return NULL;
}

static const USILUav *find_uav_binding(const USILProgram *program,
                                      int register_index) {
  for (int index = 0; index < program->uav_count; ++index) {
    if (program->uavs[index].reg_idx == register_index)
      return &program->uavs[index];
  }
  return NULL;
}

static bool opcode_supports_texel_offset(USILOpcode opcode) {
  switch (opcode) {
    case USIL_OP_SAMPLE:
    case USIL_OP_SAMPLE_C:
    case USIL_OP_SAMPLE_C_LZ:
    case USIL_OP_SAMPLE_L:
    case USIL_OP_SAMPLE_D:
    case USIL_OP_SAMPLE_B:
    case USIL_OP_LD:
    case USIL_OP_LD_MS:
      return true;
    default:
      return false;
  }
}

static bool opcode_has_condition_test(USILOpcode opcode) {
  return opcode == USIL_OP_IF || opcode == USIL_OP_BREAKC ||
         opcode == USIL_OP_CONTINUEC || opcode == USIL_OP_DISCARD;
}

static bool hlsl_opcode_supported(USILOpcode opcode) {
  switch (opcode) {
    case USIL_OP_NOP:
    case USIL_OP_ADD:
    case USIL_OP_SUB:
    case USIL_OP_MUL:
    case USIL_OP_DIV:
    case USIL_OP_MAD:
    case USIL_OP_MOV:
    case USIL_OP_MOVC:
    case USIL_OP_DP2:
    case USIL_OP_DP3:
    case USIL_OP_DP4:
    case USIL_OP_RCP:
    case USIL_OP_RSQ:
    case USIL_OP_SQRT:
    case USIL_OP_MIN:
    case USIL_OP_MAX:
    case USIL_OP_LT:
    case USIL_OP_GE:
    case USIL_OP_EQ:
    case USIL_OP_NE:
    case USIL_OP_ILT:
    case USIL_OP_IGE:
    case USIL_OP_IEQ:
    case USIL_OP_INE:
    case USIL_OP_ULT:
    case USIL_OP_UGE:
    case USIL_OP_AND:
    case USIL_OP_OR:
    case USIL_OP_XOR:
    case USIL_OP_NOT:
    case USIL_OP_ISHL:
    case USIL_OP_ISHR:
    case USIL_OP_USHR:
    case USIL_OP_FTOI:
    case USIL_OP_FTOU:
    case USIL_OP_ITOF:
    case USIL_OP_UTOF:
    case USIL_OP_SAMPLE:
    case USIL_OP_SAMPLE_C:
    case USIL_OP_SAMPLE_C_LZ:
    case USIL_OP_SAMPLE_L:
    case USIL_OP_SAMPLE_D:
    case USIL_OP_SAMPLE_B:
    case USIL_OP_LD:
    case USIL_OP_LD_STRUCTURED:
    case USIL_OP_LD_MS:
    case USIL_OP_RESINFO:
    case USIL_OP_SAMPLEINFO:
    case USIL_OP_LOG:
    case USIL_OP_EXP:
    case USIL_OP_SIN:
    case USIL_OP_COS:
    case USIL_OP_FRC:
    case USIL_OP_ROUND_NE:
    case USIL_OP_ROUND_NI:
    case USIL_OP_ROUND_PI:
    case USIL_OP_ROUND_Z:
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
    case USIL_OP_DERIV_RTX:
    case USIL_OP_DERIV_RTY:
    case USIL_OP_DERIV_RTX_COARSE:
    case USIL_OP_DERIV_RTY_COARSE:
    case USIL_OP_DERIV_RTX_FINE:
    case USIL_OP_DERIV_RTY_FINE:
    case USIL_OP_IADD:
    case USIL_OP_IMUL:
    case USIL_OP_IMAD:
    case USIL_OP_IMAX:
    case USIL_OP_IMIN:
    case USIL_OP_UMAX:
    case USIL_OP_UMIN:
    case USIL_OP_UDIV:
    case USIL_OP_INEG:
    case USIL_OP_IMM_ATOMIC_IADD:
    case USIL_OP_LDMS:
    case USIL_OP_SINCOS:
    case USIL_OP_UBFE:
    case USIL_OP_GEOMETRY_APPEND:
    case USIL_OP_GEOMETRY_RESTART_STRIP:
      return true;
    default:
      return false;
  }
}

static bool validate_instruction_resource_authority(
    const USILProgram *program, const USILInstruction *instruction) {
  if (!has_terminated_text(instruction->resource_dimension,
                           sizeof(instruction->resource_dimension))) {
    return false;
  }
  if (instruction->has_texel_offset) {
    if (!opcode_supports_texel_offset(instruction->opcode)) return false;
    for (int component = 0; component < 3; ++component) {
      if (instruction->texel_offsets[component] < -8 ||
          instruction->texel_offsets[component] > 7) return false;
    }
  }

  if (!instruction->has_resource_dimension &&
      !instruction->has_resource_return_types) return true;

  const USILTexture *texture = NULL;
  const USILUav *uav = NULL;
  for (int operand = 0; operand < instruction->operand_count; ++operand) {
    if (instruction->operands[operand].type == OPERAND_TYPE_RESOURCE) {
      if (texture) return false;
      texture = find_texture_binding(
          program, instruction->operands[operand].register_index);
      if (!texture) return false;
    } else if (instruction->operands[operand].type == OPERAND_TYPE_UAV) {
      if (uav) return false;
      uav = find_uav_binding(
          program, instruction->operands[operand].register_index);
      if (!uav) return false;
    }
  }
  if ((texture != NULL) == (uav != NULL)) return false;

  const char *dimension = texture ? texture->dimension : uav->dimension;
  const uint8_t *return_types =
      texture ? texture->return_types : uav->return_types;
  const uint32_t stride =
      (uint32_t)(texture ? texture->stride : uav->stride);
  if (instruction->has_resource_dimension &&
      (strcmp(instruction->resource_dimension, dimension) != 0 ||
       instruction->resource_stride != stride)) {
    return false;
  }
  const bool has_typed_return = strcmp(dimension, "structured") != 0 &&
                                strcmp(dimension, "raw") != 0;
  if (has_typed_return && instruction->has_resource_return_types &&
      memcmp(instruction->resource_return_types, return_types, 4) != 0) {
    return false;
  }
  return true;
}

static bool geometry_flow_opcode_supported(USILOpcode opcode) {
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
    case USIL_OP_DISCARD:
      return false;
    default:
      return true;
  }
}

static bool geometry_exact_opcode_supported(USILOpcode opcode) {
  /* This is the currently byte-verified geometry recompile surface.  The
   * USIL contract still retains every arithmetic instruction, but emitting
  * optimized arithmetic as one HLSL statement per DXBC instruction is not
  * an inverse of D3DCompiler and is therefore kept behind the fail-closed
  * boundary until a proof-driven expression IR is available. */
  switch (opcode) {
    case USIL_OP_MOV:
    case USIL_OP_FTOU:
    case USIL_OP_RET:
    case USIL_OP_GEOMETRY_APPEND:
    case USIL_OP_GEOMETRY_RESTART_STRIP:
      return true;
    default:
      return false;
  }
}

static bool validate_geometry_program_for_hlsl(const USILProgram *program) {
  if (program->program_type != DXBC_PROGRAM_TYPE_GEOMETRY)
    return strncmp(program->shader_type_model, "gs", 2) != 0;
  const bool extrusion_lift =
      hlsl_extruded_triangle_lift_matches(program);
  if (!program->has_stage_contract || !program->geometry.valid ||
      strncmp(program->shader_type_model, "gs", 2) != 0 ||
      !program->geometry.output_tuple_state_persists ||
      program->geometry.input_vertex_count == 0 ||
      program->input_count == 0 || program->output_count == 0 ||
      program->geometry.max_output_vertex_count == 0 ||
      program->geometry.max_output_vertex_count > 1024 ||
      (program->geometry.declared_stream_mask != 0 && !extrusion_lift) ||
      program->geometry.referenced_stream_mask != 1u) {
    return false;
  }
  switch (program->geometry.input_primitive) {
    case DXBC_INPUT_PRIMITIVE_POINT:
    case DXBC_INPUT_PRIMITIVE_LINE:
    case DXBC_INPUT_PRIMITIVE_TRIANGLE:
    case DXBC_INPUT_PRIMITIVE_LINE_ADJACENCY:
    case DXBC_INPUT_PRIMITIVE_TRIANGLE_ADJACENCY:
      break;
    default:
      return false;
  }
  switch (program->geometry.output_topology) {
    case DXBC_OUTPUT_TOPOLOGY_POINT_LIST:
    case DXBC_OUTPUT_TOPOLOGY_LINE_STRIP:
    case DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP:
      break;
    default:
      return false;
  }

  /* The bounded geometry interface names one field per input register.
   * Reject packed duplicate declarations until a register-lane projection is
   * represented explicitly rather than guessing which semantic owns a read. */
  for (int input = 0; input < program->input_count; ++input) {
    const DXBCSignatureElement *element = &program->inputs[input];
    if (element->component_type != 3 || element->mask == 0 ||
        element->stream_index != 0) return false;
    for (int previous = 0; previous < input; ++previous)
      if (program->inputs[previous].register_id == element->register_id)
        return false;
  }
  for (int output = 0; output < program->output_count; ++output) {
    const DXBCSignatureElement *element = &program->outputs[output];
    if (element->mask == 0 || element->component_type < 1 ||
        element->component_type > 3 || element->stream_index != 0)
      return false;
  }

  uint8_t written_masks[HLSL_SM5_IO_REGISTER_COUNT] = {0};
  size_t effect_count = 0;
  uint8_t referenced_streams = 0;
  const bool exact_structural_lift =
      hlsl_triangle_edge_distance_lift_matches(program) ||
      extrusion_lift;
  for (int index = 0; index < program->instruction_count; ++index) {
    const USILInstruction *instruction = &program->instructions[index];
    if (!exact_structural_lift &&
        (!geometry_flow_opcode_supported(instruction->opcode) ||
         !geometry_exact_opcode_supported(instruction->opcode))) return false;
    if (instruction->opcode == USIL_OP_GEOMETRY_APPEND ||
        instruction->opcode == USIL_OP_GEOMETRY_RESTART_STRIP) {
      const USILGeometryEffectKind expected =
          instruction->opcode == USIL_OP_GEOMETRY_APPEND
              ? USIL_GEOMETRY_EFFECT_APPEND
              : USIL_GEOMETRY_EFFECT_RESTART_STRIP;
      if (instruction->geometry_effect != expected ||
          instruction->geometry_stream_id != 0) return false;
      if (instruction->geometry_stream_explicit) {
        const DXBCOperand *stream = &instruction->operands[0];
        if (instruction->operand_count != 1 ||
            stream->type != OPERAND_TYPE_STREAM ||
            stream->register_index_dim != 1 ||
            stream->register_index != 0 ||
            !stream->index_has_immediate[0] ||
            stream->index_values[0] != 0 || stream->rel_op0 ||
            stream->rel_op1 || stream->rel_op2) return false;
      } else if (instruction->operand_count != 0) {
        return false;
      }
      ++effect_count;
      referenced_streams |= 1u;
      if (expected == USIL_GEOMETRY_EFFECT_APPEND) {
        for (int output = 0; output < program->output_count; ++output) {
          const DXBCSignatureElement *element = &program->outputs[output];
          if (element->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
              (written_masks[element->register_id] & element->mask) !=
                  element->mask) return false;
        }
      }
      continue;
    }
    if (instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE)
      return false;
    if (inst_writes_to_dest(instruction) && instruction->operand_count > 0 &&
        instruction->operands[0].type == OPERAND_TYPE_OUTPUT) {
      const DXBCOperand *destination = &instruction->operands[0];
      if (destination->register_index < 0 ||
          destination->register_index >= HLSL_SM5_IO_REGISTER_COUNT)
        return false;
      written_masks[destination->register_index] |=
          (uint8_t)(destination->destination_mask >> 4);
    }
  }
  return effect_count == program->geometry.effect_count &&
         referenced_streams == program->geometry.referenced_stream_mask;
}

static bool validate_program_for_hlsl(const USILProgram *program,
                                      HLSLEmitMode mode,
                                      HLSLEmitDiagnostic *diagnostic) {
  if (!program || !has_terminated_text(program->shader_type_model,
                                       sizeof(program->shader_type_model)) ||
      program->input_count < 0 ||
      program->input_alloc < program->input_count ||
      (program->input_alloc == 0) != (program->inputs == NULL) ||
      program->output_count < 0 ||
      program->output_alloc < program->output_count ||
      (program->output_alloc == 0) != (program->outputs == NULL) ||
      program->patch_constant_count < 0 ||
      program->patch_constant_alloc < program->patch_constant_count ||
      (program->patch_constant_alloc == 0) !=
          (program->patch_constants == NULL) ||
      program->instruction_count < 0 ||
      program->instruction_alloc < program->instruction_count ||
      (program->instruction_alloc == 0) !=
          (program->instructions == NULL) ||
      program->temp_count < 0 ||
      program->temp_count > HLSL_SM5_TEMP_REGISTER_COUNT ||
      program->icb_value_count < 0 ||
      program->icb_value_alloc < program->icb_value_count ||
      (program->icb_value_alloc == 0) != (program->icb_values == NULL) ||
      (program->icb_value_count % 4) != 0 ||
      program->index_range_count < 0 ||
      program->index_range_alloc < program->index_range_count ||
      (program->index_range_alloc == 0) !=
          (program->index_ranges == NULL) ||
      program->tessellation.phase_capacity <
          program->tessellation.phase_count ||
      (program->tessellation.phase_capacity == 0) !=
          (program->tessellation.phases == NULL)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
                          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                          HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE);
    return false;
  }
  if (!usil_signature_authority_is_valid(program)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
                          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                          HLSL_EMIT_REASON_INVALID_SIGNATURE);
    return false;
  }
  /* Recompile mode currently corresponds to the default optimized compiler
   * contract: either no global-flags declaration or only
   * REFACTORING_ALLOWED. Other flags change validation/code generation and
   * require an explicit compiler/source projection; never discard them. */
  if (program->has_global_flags && program->global_flags != 1u) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                          HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    return false;
  }
  /* Ordinary hull/domain emission retains its complete source-backed inverse.
   * Separate candidates admit typed domain interfaces and independent pure
   * hull fork phases; admission supplies no linked compiler authority. */
  if (program->program_type == DXBC_PROGRAM_TYPE_HULL ||
      program->program_type == DXBC_PROGRAM_TYPE_DOMAIN) {
    if (hlsl_high_level_hull_source_supported(program, mode) ||
        hlsl_high_level_patch_domain_supported(program, mode) ||
        hlsl_exact_tessellation_lift_matches(program)) return true;
    if (!hlsl_high_level_domain_interface_supported(program, mode)) {
      hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                            HLSL_EMIT_REASON_UNSUPPORTED_STAGE);
      return false;
    }
  }
  if (program->patch_constant_count != 0 && !hlsl_high_level_domain_interface_supported(program, mode)) {
    hlsl_emit_set_metadata_failure(
        diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
        HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
        HLSL_EMIT_REASON_UNSUPPORTED_FEATURE,
        HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_PATCH_SIGNATURE,
        0, -1, -1);
    return false;
  }
  if (!hlsl_high_level_domain_interface_supported(program, mode) &&
      !hlsl_high_level_geometry_interface_supported(program, mode) &&
      !validate_geometry_program_for_hlsl(program)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                          HLSL_EMIT_REASON_UNSUPPORTED_STAGE);
    return false;
  }

  const uint32_t output_register_limit =
      strncmp(program->shader_type_model, "ps", 2) == 0
          ? HLSL_SM5_PIXEL_OUTPUT_REGISTER_COUNT
          : HLSL_SM5_IO_REGISTER_COUNT;
  for (int i = 0; i < program->input_count; i++) {
    const DXBCSignatureElement *element = &program->inputs[i];
    if (!signature_semantic_is_valid(element) ||
        element->min_precision != 0u ||
        element->stream_index != 0u ||
        element->register_id >= HLSL_SM5_IO_REGISTER_COUNT) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_SIGNATURE,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM,
          HLSL_EMIT_METADATA_INPUT_SIGNATURE, i, -1,
          element->register_id <= INT_MAX ? (int)element->register_id : -1);
      return false;
    }
  }
  for (int i = 0; i < program->output_count; i++) {
    const DXBCSignatureElement *element = &program->outputs[i];
    if (!signature_semantic_is_valid(element) ||
        element->min_precision != 0u ||
        element->stream_index != 0u ||
        (element->register_id >= output_register_limit &&
         !is_depth_semantic(element))) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_SIGNATURE,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM,
          HLSL_EMIT_METADATA_OUTPUT_SIGNATURE, i, -1,
          element->register_id <= INT_MAX ? (int)element->register_id : -1);
      return false;
    }
  }

#define VALIDATE_LIST(count, allocation, pointer, maximum, metadata_kind)       \
  do {                                                                          \
    if ((count) < 0 || (count) > (maximum) || (allocation) < (count) ||         \
        (((allocation) == 0) != ((pointer) == NULL))) {                         \
      hlsl_emit_set_metadata_failure(                                            \
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,                         \
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,                                   \
          HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE,                               \
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, (metadata_kind), -1, -1, -1);      \
      return false;                                                              \
    }                                                                            \
  } while (0)
  VALIDATE_LIST(program->cbuffer_count, program->cbuffer_alloc,
                program->cbuffers,
                HLSL_SM5_CBUFFER_REGISTER_COUNT,
                HLSL_EMIT_METADATA_CBUFFER);
  VALIDATE_LIST(program->texture_count, program->texture_alloc,
                program->textures,
                HLSL_SM5_RESOURCE_REGISTER_COUNT,
                HLSL_EMIT_METADATA_TEXTURE);
  VALIDATE_LIST(program->sampler_count, program->sampler_alloc,
                program->samplers,
                HLSL_SM5_SAMPLER_REGISTER_COUNT,
                HLSL_EMIT_METADATA_SAMPLER);
  VALIDATE_LIST(program->indexable_temp_count, program->indexable_temp_alloc,
                program->indexable_temps,
                HLSL_SM5_TEMP_REGISTER_COUNT,
                HLSL_EMIT_METADATA_INDEXABLE_TEMP);
  VALIDATE_LIST(program->uav_count, program->uav_alloc, program->uavs,
                HLSL_SM5_UAV_REGISTER_COUNT, HLSL_EMIT_METADATA_UAV);
#undef VALIDATE_LIST

  for (int i = 0; i < program->cbuffer_count; i++) {
    if (program->cbuffers[i].reg_idx < 0 ||
        program->cbuffers[i].reg_idx >= HLSL_SM5_CBUFFER_REGISTER_COUNT ||
        program->cbuffers[i].size <= 0 ||
        program->cbuffers[i].size > 4096 ||
        program->cbuffers[i].dynamic_indexed !=
            cbuffer_has_dynamic_access(program,
                                       program->cbuffers[i].reg_idx)) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_CBUFFER,
          i, -1, program->cbuffers[i].reg_idx);
      return false;
    }
    for (int previous = 0; previous < i; ++previous) {
      if (program->cbuffers[previous].reg_idx ==
          program->cbuffers[i].reg_idx) {
        hlsl_emit_set_metadata_failure(
            diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
            HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_CBUFFER,
            i, -1, program->cbuffers[i].reg_idx);
        hlsl_emit_set_related_metadata(
            diagnostic, HLSL_EMIT_METADATA_SOURCE_PROGRAM,
            HLSL_EMIT_METADATA_CBUFFER, previous, -1,
            program->cbuffers[previous].reg_idx);
        return false;
      }
    }
  }
  for (int i = 0; i < program->texture_count; i++) {
    if (program->textures[i].reg_idx < 0 ||
        program->textures[i].reg_idx >= HLSL_SM5_RESOURCE_REGISTER_COUNT ||
        !has_terminated_text(program->textures[i].dimension,
                             sizeof(program->textures[i].dimension)) ||
        !hlsl_texture_declaration_supported(&program->textures[i])) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
          i, -1, program->textures[i].reg_idx);
      return false;
    }
    for (int previous = 0; previous < i; ++previous) {
      if (program->textures[previous].reg_idx ==
          program->textures[i].reg_idx) {
        hlsl_emit_set_metadata_failure(
            diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
            HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
            i, -1, program->textures[i].reg_idx);
        hlsl_emit_set_related_metadata(
            diagnostic, HLSL_EMIT_METADATA_SOURCE_PROGRAM,
            HLSL_EMIT_METADATA_TEXTURE, previous, -1,
            program->textures[previous].reg_idx);
        return false;
      }
    }
  }
  for (int i = 0; i < program->sampler_count; i++) {
    if (program->samplers[i].reg_idx < 0 ||
        program->samplers[i].reg_idx >= HLSL_SM5_SAMPLER_REGISTER_COUNT ||
        program->samplers[i].mode > 1u) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_SAMPLER_CONTRACT_MISMATCH,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
          i, -1, program->samplers[i].reg_idx);
      return false;
    }
    bool uses_regular = false;
    bool uses_comparison = false;
    get_sampler_usage(program, program->samplers[i].reg_idx,
                      &uses_regular, &uses_comparison);
    if ((program->samplers[i].mode == 0u && uses_comparison) ||
        (program->samplers[i].mode == 1u &&
         (!uses_comparison || uses_regular))) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_SAMPLER_CONTRACT_MISMATCH,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
          i, -1, program->samplers[i].reg_idx);
      return false;
    }
    for (int previous = 0; previous < i; ++previous) {
      if (program->samplers[previous].reg_idx ==
          program->samplers[i].reg_idx) {
        hlsl_emit_set_metadata_failure(
            diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
            HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
            i, -1, program->samplers[i].reg_idx);
        hlsl_emit_set_related_metadata(
            diagnostic, HLSL_EMIT_METADATA_SOURCE_PROGRAM,
            HLSL_EMIT_METADATA_SAMPLER, previous, -1,
            program->samplers[previous].reg_idx);
        return false;
      }
    }
  }
  uint64_t combined_temp_registers = (uint32_t)program->temp_count;
  for (int i = 0; i < program->indexable_temp_count; i++) {
    if (program->indexable_temps[i].reg_idx < 0 ||
        program->indexable_temps[i].reg_idx >=
            HLSL_SM5_TEMP_REGISTER_COUNT ||
        program->indexable_temps[i].size <= 0) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM,
          HLSL_EMIT_METADATA_INDEXABLE_TEMP, i, -1,
          program->indexable_temps[i].reg_idx);
      return false;
    }
    combined_temp_registers +=
        (uint32_t)program->indexable_temps[i].size;
    if (combined_temp_registers > HLSL_SM5_TEMP_REGISTER_COUNT) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM,
          HLSL_EMIT_METADATA_INDEXABLE_TEMP, i, -1,
          program->indexable_temps[i].reg_idx);
      return false;
    }
    for (int previous = 0; previous < i; ++previous) {
      if (program->indexable_temps[previous].reg_idx ==
          program->indexable_temps[i].reg_idx) {
        hlsl_emit_set_metadata_failure(
            diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
            HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM,
            HLSL_EMIT_METADATA_INDEXABLE_TEMP, i, -1,
            program->indexable_temps[i].reg_idx);
        hlsl_emit_set_related_metadata(
            diagnostic, HLSL_EMIT_METADATA_SOURCE_PROGRAM,
            HLSL_EMIT_METADATA_INDEXABLE_TEMP, previous, -1,
            program->indexable_temps[previous].reg_idx);
        return false;
      }
    }
  }
  for (int i = 0; i < program->uav_count; i++) {
    if (program->uavs[i].reg_idx < 0 ||
        program->uavs[i].reg_idx >= HLSL_SM5_UAV_REGISTER_COUNT ||
        !has_terminated_text(program->uavs[i].dimension,
                             sizeof(program->uavs[i].dimension)) ||
        !hlsl_uav_declaration_supported(&program->uavs[i])) {
      hlsl_emit_set_metadata_failure(
          diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_UAV,
          i, -1, program->uavs[i].reg_idx);
      return false;
    }
    for (int previous = 0; previous < i; ++previous) {
      if (program->uavs[previous].reg_idx == program->uavs[i].reg_idx) {
        hlsl_emit_set_metadata_failure(
            diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
            HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_UAV,
            i, -1, program->uavs[i].reg_idx);
        hlsl_emit_set_related_metadata(
            diagnostic, HLSL_EMIT_METADATA_SOURCE_PROGRAM,
            HLSL_EMIT_METADATA_UAV, previous, -1,
            program->uavs[previous].reg_idx);
        return false;
      }
    }
  }

  for (int instruction = 0; instruction < program->instruction_count;
       instruction++) {
    const USILInstruction *value = &program->instructions[instruction];
    if (!usil_instruction_shape_valid(program, value)) {
      hlsl_emit_set_instruction_failure(
          diagnostic, program, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE, instruction, -1);
      return false;
    }
    if (value->precise_mask != 0u) {
      hlsl_emit_set_instruction_failure(
          diagnostic, program, HLSL_EMIT_STATUS_UNSUPPORTED,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_UNSUPPORTED_FEATURE, instruction, 0);
      return false;
    }
    if (!hlsl_opcode_supported(value->opcode)) {
      hlsl_emit_set_instruction_failure(
          diagnostic, program, HLSL_EMIT_STATUS_UNSUPPORTED,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_UNSUPPORTED_OPCODE, instruction, -1);
      return false;
    }
    if ((value->opcode == USIL_OP_SAMPLEINFO
             ? value->sample_info_return_type > 1u
             : value->sample_info_return_type != 0u) ||
        (opcode_has_condition_test(value->opcode) !=
         (value->condition_test == DXBC_INSTRUCTION_TEST_ZERO ||
          value->condition_test == DXBC_INSTRUCTION_TEST_NONZERO))) {
      hlsl_emit_set_instruction_failure(
          diagnostic, program, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE, instruction, -1);
      return false;
    }
    if (!validate_instruction_resource_authority(program, value)) {
      hlsl_emit_set_instruction_failure(
          diagnostic, program, HLSL_EMIT_STATUS_INVALID_PROGRAM,
          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
          HLSL_EMIT_REASON_RESOURCE_CONTRACT_MISMATCH, instruction, -1);
      for (int operand = 0; operand < value->operand_count; ++operand) {
        hlsl_emit_set_operand_metadata(diagnostic, program,
                                       &value->operands[operand]);
        if (diagnostic &&
            diagnostic->metadata.kind != HLSL_EMIT_METADATA_NONE) break;
      }
      return false;
    }
    for (int operand = 0; operand < value->operand_count; operand++) {
      if (!validate_operand_for_hlsl(program, &value->operands[operand], 0)) {
        hlsl_emit_set_instruction_failure(
            diagnostic, program, HLSL_EMIT_STATUS_INVALID_PROGRAM,
            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
            HLSL_EMIT_REASON_INVALID_OPERAND, instruction, operand);
        hlsl_emit_set_operand_metadata(diagnostic, program,
                                       &value->operands[operand]);
        return false;
      }
    }
  }
  return true;
}

static unsigned source_quality_width(const DXBCOperand *destination) {
  unsigned count = 0;
  unsigned mask = usil_operand_destination_lane_mask(destination);
  for (unsigned lane = 0; lane < 4; ++lane)
    if (mask & (1u << lane)) ++count;
  return count;
}

bool hlsl_source_quality_owned_expression_facts(
    void *context, uint32_t unit_id, const ASTExpr *expression,
    HLSLSourceQualityFacts *facts) {
  (void)context;
  (void)unit_id;
  if (!expression || !facts) return false;
  if (expression->kind == AST_EXPR_EMITTER_OPERAND) {
    const ASTOperandProvenance *origin = &expression->operand_provenance;
    if (!origin->complete || origin->value_role == AST_OPERAND_VALUE_UNKNOWN) return false;
    facts->known = true;
    facts->value_kind = origin->value_role == AST_OPERAND_VALUE_LOGICAL
                            ? HLSL_SOURCE_VALUE_LOGICAL
                            : origin->value_role == AST_OPERAND_VALUE_REGISTER
                                  ? HLSL_SOURCE_VALUE_REGISTER : HLSL_SOURCE_VALUE_UNKNOWN;
    facts->logical_value_id = origin->logical_value_id;
    facts->components = origin->result_components;
    facts->semantic_projection = origin->selection_role == AST_COMPONENT_SELECTION_SEMANTIC;
    facts->real_bitcast = origin->bitcast_role == AST_OPERAND_BITCAST_PROGRAM;
    facts->instruction_index = origin->instruction_index;
    facts->source_instruction_index = origin->source_instruction_index;
    facts->lanes = origin->destination_lanes;
    if (origin->selection_role == AST_COMPONENT_SELECTION_TRANSPORT)
      facts->artifacts |= HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT;
    if (origin->bitcast_role == AST_OPERAND_BITCAST_STORAGE)
      facts->artifacts |= HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST;
    if (origin->raw_buffer_reconstruction)
      facts->artifacts |= HLSL_SOURCE_ARTIFACT_RAW_BUFFER_RECONSTRUCTION;
    if (origin->synthetic_interface)
      facts->artifacts |= HLSL_SOURCE_ARTIFACT_SYNTHETIC_INTERFACE;
    return true;
  }
  const ASTLogicalValueOrigin *logical = &expression->logical_origin;
  if (logical->complete) {
    facts->known = true;
    facts->value_kind = HLSL_SOURCE_VALUE_LOGICAL;
    facts->logical_value_id = logical->logical_value_id;
    facts->components = logical->components;
    facts->instruction_index = logical->instruction_index;
    facts->source_instruction_index = logical->source_instruction_index;
    facts->lanes = logical->destination_lanes;
    facts->semantic_projection = logical->semantic_projection;
    facts->real_bitcast = logical->program_bitcast;
    facts->logical_operation = expression->kind != AST_EXPR_VAR &&
                               expression->kind != AST_EXPR_LITERAL;
    if (expression->kind == AST_EXPR_SWIZZLE && !logical->semantic_projection)
      facts->artifacts = HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT;
    if (expression->kind == AST_EXPR_BITCAST && !logical->program_bitcast)
      facts->artifacts = HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST;
    return true;
  }
  return false;
}

static bool emitter_source_quality_expression_facts(
    void *context, uint32_t unit_id, const ASTExpr *expression,
    HLSLSourceQualityFacts *facts) {
  HLSLEmitterContext *ctx = context;
  (void)unit_id;
  if (hlsl_source_quality_owned_expression_facts(NULL, unit_id, expression, facts))
    return true;
  if (expression->kind == AST_EXPR_SWIZZLE) {
    facts->known = true;
    facts->artifacts = HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT;
    return true;
  }
  int instruction = ctx->source_quality_instruction;
  if (expression->kind == AST_EXPR_VAR)
    instruction = expression->u.var.ssa_var;
  else if (expression != ctx->source_quality_root ||
           expression->kind == AST_EXPR_EMITTER_OPERAND)
    return false;
  if (instruction < 0 || instruction >= ctx->program->instruction_count ||
      ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE)
    return false;
  const USILInstruction *owner = &ctx->program->instructions[instruction];
  if (owner->operand_count < 1) return false;
  facts->known = true;
  facts->value_kind = HLSL_SOURCE_VALUE_LOGICAL;
  facts->logical_value_id = (uint64_t)instruction;
  facts->components = source_quality_width(&owner->operands[0]);
  facts->instruction_index = instruction;
  facts->source_instruction_index = owner->source_instruction_index;
  facts->lanes = usil_operand_destination_lane_mask(&owner->operands[0]);
  facts->logical_operation = expression->kind != AST_EXPR_VAR &&
                             expression->kind != AST_EXPR_LITERAL;
  return true;
}

static bool high_level_direct_return_supported(const USILProgram *program,
                                                HLSLEmitMode mode) {
  if (mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      program->output_count != 1 || program->instruction_count < 2 ||
      program->instructions[program->instruction_count - 1].opcode != USIL_OP_RET)
    return false;
  const DXBCSignatureElement *output = &program->outputs[0];
  if (output->component_type != 3 || output->register_id == UINT32_MAX ||
      !output->mask || (output->mask & (output->mask + 1u)))
    return false;
  int output_write = -1;
  int final_value_instruction = -1;
  for (int instruction = 0; instruction + 1 < program->instruction_count; ++instruction) {
    const USILInstruction *value = &program->instructions[instruction];
    if (value->opcode == USIL_OP_NOP) continue;
    final_value_instruction = instruction;
    if (!hlsl_expression_effects_supported(program, value)) return false;
    for (int operand = 0; operand < value->operand_count; ++operand) {
      USILOperandUseInfo use;
      if (!usil_instruction_operand_use(program, value, operand, &use)) return false;
      if (value->operands[operand].type != OPERAND_TYPE_OUTPUT) continue;
      if (use.use != USIL_OPERAND_USE_DESTINATION || output_write >= 0 ||
          value->operands[operand].register_index != (int)output->register_id ||
          usil_operand_destination_lane_mask(&value->operands[operand]) != output->mask)
        return false;
      output_write = instruction;
    }
  }
  return output_write >= 0 && output_write == final_value_instruction;
}

bool hlsl_source_quality_observe_owned_expression(HLSLEmitterContext *ctx,
    const ASTExpr *expression, int instruction, const HLSLStageRootOwner *owner) {
  if (!ctx->source_quality_analysis) return true;
  if (!(owner ? hlsl_stage_coverage_owned_root(ctx, expression, instruction, owner) :
        hlsl_stage_coverage_root(ctx, expression, instruction))) return false;
  hlsl_source_quality_interface_expression_begin(ctx, instruction);
  ctx->source_quality_root = expression;
  ctx->source_quality_instruction = instruction;
  bool accepted = hlsl_source_quality_analysis_expression(ctx->source_quality_analysis, expression);
  ctx->source_quality_root = NULL;
  if (!accepted)
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
  return accepted;
}

bool hlsl_source_quality_observe_expression(HLSLEmitterContext *ctx,
                                            const ASTExpr *expression, int instruction) {
  return hlsl_source_quality_observe_owned_expression(ctx, expression, instruction, NULL);
}

/* Eligibility for independently inventoried natural stage-entry units. The
 * bounded geometry flow route additionally proves its CFG/SSA/type/range
 * contract and later replays receipts for every actual body/control byte.
 * Interface and declaration syntax have separate coverage checks. Includes,
 * uncovered storage layouts and helpers retain explicit coverage gaps. */
bool hlsl_source_quality_body_inventory_supported(HLSLEmitterContext *ctx) {
  const USILProgram *program = ctx->program;
  /* The bounded flow route must independently prove CFG/SSA, types, loop
   * bounds and persistent output initialization before promising coverage. */
  const bool bounded_flow = hlsl_geometry_control_flow_admission(program, ctx->emit_mode);
  if (bounded_flow &&
      !hlsl_geometry_control_flow_inventory_supported(ctx)) return false;
  if (ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      !hlsl_source_quality_interface_inventory_supported(ctx) ||
      !ctx->high_level_functions_prepared ||
      (program->program_type != DXBC_PROGRAM_TYPE_VERTEX &&
       program->program_type != DXBC_PROGRAM_TYPE_PIXEL &&
       !ctx->high_level_geometry && !ctx->high_level_domain) ||
      ctx->unity_uv_helper ||
      ctx->readable_screen_pos_helper ||
      /* This body consumes only the proved flow plan, never legacy compiler
       * replacement plans discovered by the common analysis passes. */
      (ctx->compiler_model.replacement_count && !bounded_flow) ||
      ctx->use_uint_temps ||
      ctx->indexed_face_basis.valid || ctx->surface_tangent_frame.valid ||
      !hlsl_source_quality_resource_inventory_complete(ctx) || program->uav_count ||
      program->icb_value_count || program->indexable_temp_count ||
      program->index_range_count ||
      (program->patch_constant_count && !ctx->high_level_domain))
    return false;
  for (int group = 0; group < 2; ++group)
    if (ctx->float4_functions.use_count[group] >= 2)
      return false;
  return true;
}

bool hlsl_source_quality_inventory_supported(HLSLEmitterContext *ctx) {
  return hlsl_source_quality_body_inventory_supported(ctx) &&
      hlsl_source_quality_cbuffer_inventory_supported(ctx);
}

bool hlsl_source_quality_begin_entry(HLSLEmitterContext *ctx, bool complete) {
  if (!hlsl_stage_coverage_begin(ctx) || (ctx->domain_output_plan.present &&
      !hlsl_stage_coverage_domain_output_plan(ctx, &ctx->domain_output_plan))) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_CONTEXT_ALLOCATION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    return false;
  }
  if (!ctx->source_quality_analysis) return true;
  ctx->source_quality_interface_required = complete && ctx->high_level_interface;
  ctx->source_quality_cbuffer_required = complete && ctx->program->cbuffer_count > 0;
  /* Replay actual flow receipts even when another unit dependency is already
   * incomplete. This check can only retain/downgrade coverage, never grant it. */
  ctx->source_quality_geometry_flow_required =
      hlsl_geometry_control_flow_admission(ctx->program, ctx->emit_mode);
  if (hlsl_source_quality_analysis_begin_unit(ctx->source_quality_analysis, 0,
                                             HLSL_SOURCE_UNIT_ENTRY_POINT, complete) &&
      hlsl_stage_coverage_begin_unit(ctx, 0, HLSL_SOURCE_UNIT_ENTRY_POINT))
    return true;
  hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                 HLSL_EMIT_PHASE_CONTEXT_ALLOCATION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
  return false;
}

static bool source_builder_storage_valid(const StringBuilder *source) {
  return source && sb_ok(source) && (source->buf
      ? source->len < source->capacity && source->buf[source->len] == '\0'
      : !source->len && !source->capacity);
}

enum { PACKED_OUTPUT_LEASE_LIMIT = 256, PACKED_OUTPUT_BYTE_LIMIT = 1024 * 1024 };

typedef struct {
  const void *original;
  void *owned;
  size_t size;
} PackedOutputLease;

/* Parent objects precede every pointed collection. A changed pointer or count
 * rejects before typed replay follows that collection. Rendered source and
 * maps come from a callback-free invocation of this same complete emitter. */
typedef struct HLSLPackedOutputGuard {
  PackedOutputLease leases[PACKED_OUTPUT_LEASE_LIMIT];
  size_t lease_count, owned_bytes, prefix_length;
  uint8_t model_digest[32];
  SerializedProgramParameters current, common;
  StringBuilder expected;
  HLSLExpressionSourceMap expected_map, actual_map, callback_map;
  HLSLExpressionSourceMap *caller_map;
  const HLSLGlobalDeclarationUnion *globals;
  const HLSLGlobalDeclarationField *global_fields;
  size_t global_field_count;
  uint32_t global_shell_size;
  int global_current_variant;
  bool ready;
} HLSLPackedOutputGuard;

bool hlsl_source_quality_packed_output_guard_active(const HLSLEmitterContext *ctx) {
  return ctx && ctx->high_level_packed_outputs && ctx->packed_output_guard &&
      ctx->packed_output_guard->ready;
}

static bool packed_output_lease(HLSLPackedOutputGuard *guard, const void *source, size_t size) {
  if (!size) return true;
  if (!source || guard->lease_count == (size_t)PACKED_OUTPUT_LEASE_LIMIT ||
      size > (size_t)PACKED_OUTPUT_BYTE_LIMIT - guard->owned_bytes) return false;
  void *owned = malloc(size);
  if (!owned) return false;
  memcpy(owned, source, size);
  guard->leases[guard->lease_count++] = (PackedOutputLease){source, owned, size};
  guard->owned_bytes += size;
  return true;
}

static bool packed_output_string_lease(HLSLPackedOutputGuard *guard, const char *text) {
  if (!text) return true;
  size_t length = 0;
  while (length < 256 && text[length]) ++length;
  return length < 256 && packed_output_lease(guard, text, length + 1);
}

static bool packed_output_parameters_lease(HLSLPackedOutputGuard *guard,
    const SerializedProgramParameters *parameters, SerializedProgramParameters *owned) {
  if (!parameters) return true;
  if (parameters->cb_count < 0 || parameters->cb_count > 8 ||
      parameters->res_count < 0 || parameters->res_count > 128 ||
      !packed_output_lease(guard, parameters, sizeof(*parameters)) ||
      !packed_output_lease(guard, parameters->constant_buffers,
          (size_t)parameters->cb_count * sizeof(*parameters->constant_buffers)) ||
      !packed_output_lease(guard, parameters->resources,
          (size_t)parameters->res_count * sizeof(*parameters->resources))) return false;
  for (int buffer = 0; buffer < parameters->cb_count; ++buffer) {
    const SerializedConstantBuffer *cbuffer = &parameters->constant_buffers[buffer];
    if (cbuffer->var_count < 0 || cbuffer->var_count > 128 ||
        cbuffer->struct_count < 0 || cbuffer->struct_count > 16 ||
        !packed_output_string_lease(guard, cbuffer->name) ||
        !packed_output_lease(guard, cbuffer->variables,
            (size_t)cbuffer->var_count * sizeof(*cbuffer->variables)) ||
        !packed_output_lease(guard, cbuffer->struct_params,
            (size_t)cbuffer->struct_count * sizeof(*cbuffer->struct_params))) return false;
    for (int variable = 0; variable < cbuffer->var_count; ++variable)
      if (!packed_output_string_lease(guard, cbuffer->variables[variable].name)) return false;
    for (int structure = 0; structure < cbuffer->struct_count; ++structure) {
      const SerializedStructParam *entry = &cbuffer->struct_params[structure];
      if (entry->member_count < 0 || entry->member_count > 128 ||
          !packed_output_string_lease(guard, entry->name) ||
          !packed_output_lease(guard, entry->members,
              (size_t)entry->member_count * sizeof(*entry->members))) return false;
      for (int member = 0; member < entry->member_count; ++member)
        if (!packed_output_string_lease(guard, entry->members[member].name)) return false;
    }
  }
  for (int resource = 0; resource < parameters->res_count; ++resource)
    if (!packed_output_string_lease(guard, parameters->resources[resource].name)) return false;
  return serialized_program_parameters_copy(owned, parameters);
}

static bool packed_output_maps_equal(const HLSLExpressionSourceMap *left,
    const HLSLExpressionSourceMap *right) {
  if (!left || !right || left->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
      left->count != right->count || left->complete != right->complete) return false;
  for (size_t origin = 0; origin < left->count; ++origin)
    if (!hlsl_expression_origins_equal(&left->origins[origin], &right->origins[origin])) return false;
  return true;
}

static bool packed_output_model_matches(const HLSLEmitterContext *ctx) {
  const HLSLPackedOutputGuard *guard = ctx->packed_output_guard;
  if (!guard || !guard->ready) return false;
  for (size_t lease = 0; lease < guard->lease_count; ++lease)
    if (memcmp(guard->leases[lease].original, guard->leases[lease].owned,
        guard->leases[lease].size)) return false;
  uint8_t current[32];
  if (!hlsl_packed_output_owned_contract_digest(ctx->program, current) ||
      memcmp(current, guard->model_digest, sizeof(current)) ||
      (ctx->params && !serialized_program_parameters_equal(ctx->params, &guard->current)) ||
      (ctx->common_params && !serialized_program_parameters_equal(ctx->common_params, &guard->common))) return false;
  if (guard->globals) {
    size_t count = 0;
    if (hlsl_global_declarations_fields(guard->globals, &count) != guard->global_fields ||
        count != guard->global_field_count ||
        hlsl_global_declarations_shell_size(guard->globals) != guard->global_shell_size ||
        hlsl_global_declarations_current_variant(guard->globals) != guard->global_current_variant) return false;
  }
  return true;
}

static bool packed_output_source_matches(const HLSLEmitterContext *ctx, bool complete) {
  const HLSLPackedOutputGuard *guard = ctx->packed_output_guard;
  return guard && source_builder_storage_valid(ctx->sb) &&
      ctx->sb->len >= guard->prefix_length && ctx->sb->len <= guard->expected.len &&
      (!complete || ctx->sb->len == guard->expected.len) &&
      (!ctx->sb->len || !memcmp(ctx->sb->buf, guard->expected.buf, ctx->sb->len));
}

static void packed_output_guard_dispose(HLSLEmitterContext *ctx) {
  HLSLPackedOutputGuard *guard = ctx->packed_output_guard;
  if (!guard) return;
  ctx->expression_source_map = guard->caller_map;
  for (size_t lease = 0; lease < guard->lease_count; ++lease) free(guard->leases[lease].owned);
  serialized_program_parameters_free(&guard->current);
  serialized_program_parameters_free(&guard->common);
  sb_free(&guard->expected);
  free(guard);
  ctx->packed_output_guard = NULL;
}

static void packed_output_guard_rollback(HLSLEmitterContext *ctx) {
  HLSLPackedOutputGuard *guard = ctx->packed_output_guard;
  if (!guard) return;
  if (ctx->expression_source_map) memset(ctx->expression_source_map, 0, sizeof(*ctx->expression_source_map));
  /* The initial prefix is owned independently of all observer mutations. */
  sb_clear(ctx->sb);
  ctx->sb->failed = false;
  sb_append_len(ctx->sb, guard->expected.buf, guard->prefix_length);
  ctx->sb->failed = true;
}

static bool packed_output_guard_prepare(HLSLEmitterContext *ctx, size_t prefix_length,
    const HLSLEmitNames *names, const HLSLEmitOptions *options) {
  if (!source_builder_storage_valid(ctx->sb) || ctx->sb->len != prefix_length ||
      prefix_length > (size_t)PACKED_OUTPUT_BYTE_LIMIT) return false;
  HLSLPackedOutputGuard *guard = calloc(1, sizeof(*guard));
  if (!guard) return false;
  guard->caller_map = ctx->expression_source_map;
  guard->prefix_length = prefix_length;
  sb_init(&guard->expected);
  sb_append_len(&guard->expected, ctx->sb->buf, prefix_length);
  ctx->packed_output_guard = guard;
  const USILProgram *program = ctx->program;
  if (!sb_ok(&guard->expected) ||
      !packed_output_lease(guard, program, sizeof(*program)) ||
      !packed_output_lease(guard, program->instructions,
          (size_t)program->instruction_count * sizeof(*program->instructions)) ||
      !packed_output_lease(guard, program->inputs, (size_t)program->input_count * sizeof(*program->inputs)) ||
      !packed_output_lease(guard, program->outputs, (size_t)program->output_count * sizeof(*program->outputs)) ||
      !packed_output_lease(guard, program->signature_declarations,
          (size_t)program->signature_declaration_count * sizeof(*program->signature_declarations)) ||
      !packed_output_lease(guard, program->cbuffers, (size_t)program->cbuffer_count * sizeof(*program->cbuffers)) ||
      !packed_output_parameters_lease(guard, ctx->params, &guard->current) ||
      !packed_output_parameters_lease(guard, ctx->common_params, &guard->common) ||
      !packed_output_lease(guard, names, names ? sizeof(*names) : 0) ||
      !packed_output_string_lease(guard, ctx->entry_point_name) ||
      !packed_output_string_lease(guard, ctx->preferred_input_struct_name) ||
      !packed_output_string_lease(guard, ctx->preferred_output_struct_name) ||
      !packed_output_lease(guard, options, options ? sizeof(*options) : 0)) goto fail;
  if (ctx->reserved_preprocessor_identifier_count > 128 ||
      !packed_output_lease(guard, ctx->reserved_preprocessor_identifiers,
          ctx->reserved_preprocessor_identifier_count * sizeof(*ctx->reserved_preprocessor_identifiers))) goto fail;
  for (size_t index = 0; index < ctx->reserved_preprocessor_identifier_count; ++index)
    if (!packed_output_string_lease(guard, ctx->reserved_preprocessor_identifiers[index])) goto fail;
  for (int role = 0; role < 2; ++role) {
    const DXBCSignatureElement *elements = role ? program->outputs : program->inputs;
    const int count = role ? program->output_count : program->input_count;
    for (int field = 0; field < count; ++field)
      if (!packed_output_string_lease(guard, elements[field].semantic_name_extended)) goto fail;
  }
  guard->globals = ctx->global_declarations;
  if (guard->globals) {
    guard->global_fields = hlsl_global_declarations_fields(guard->globals, &guard->global_field_count);
    guard->global_shell_size = hlsl_global_declarations_shell_size(guard->globals);
    guard->global_current_variant = hlsl_global_declarations_current_variant(guard->globals);
    if (guard->global_field_count > 128 || !packed_output_lease(guard, guard->global_fields,
        guard->global_field_count * sizeof(*guard->global_fields))) goto fail;
    for (size_t field = 0; field < guard->global_field_count; ++field) {
      const HLSLGlobalDeclarationField *entry = &guard->global_fields[field];
      if (entry->witness_count > 128 || !packed_output_string_lease(guard, entry->name) ||
          !packed_output_lease(guard, entry->witness_subprogram_indices,
              entry->witness_count * sizeof(*entry->witness_subprogram_indices))) goto fail;
    }
  }
  if (!hlsl_packed_output_owned_contract_digest(program, guard->model_digest)) goto fail;
  HLSLEmitOptions replay = options ? *options : (HLSLEmitOptions){0};
  replay.source_quality = NULL;
  replay.source_quality_observer = NULL;
  replay.source_quality_observer_context = NULL;
  replay.expression_source_map = &guard->expected_map;
  HLSLEmitDiagnostic diagnostic;
  if (!hlsl_emit_with_options_impl(program, &guard->expected, ctx->params, ctx->common_params,
      names, &replay, NULL, NULL, &diagnostic, true) ||
      guard->expected.len > (size_t)PACKED_OUTPUT_BYTE_LIMIT) goto fail;
  guard->ready = true;
  if (!packed_output_model_matches(ctx)) goto fail;
  if (!ctx->expression_source_map) ctx->expression_source_map = &guard->actual_map;
  return true;
fail:
  packed_output_guard_dispose(ctx);
  return false;
}

static bool packed_output_quality_observer(HLSLEmitterContext *ctx,
    const HLSLSourceQualityObservation *observation) {
  HLSLPackedOutputGuard *guard = ctx->packed_output_guard;
  if (!guard || !guard->ready || !packed_output_model_matches(ctx) ||
      !packed_output_source_matches(ctx, false) || !ctx->expression_source_map) return false;
  const size_t length = ctx->sb->len;
  guard->callback_map = *ctx->expression_source_map;
  const bool observed = hlsl_stage_coverage_observation(ctx, observation) &&
      (!ctx->source_quality_forward_observer ||
       ctx->source_quality_forward_observer(ctx->source_quality_forward_observer_context, observation));
  return observed && packed_output_model_matches(ctx) && ctx->sb->len == length &&
      packed_output_source_matches(ctx, false) &&
      packed_output_maps_equal(ctx->expression_source_map, &guard->callback_map);
}

void hlsl_source_quality_emission(HLSLEmitterContext *ctx, uint32_t artifacts,
                                  bool logical_operation, int instruction) {
  if (!ctx->source_quality_analysis) return;
  HLSLSourceQualityFacts facts;
  hlsl_source_quality_facts_init(&facts);
  facts.known = true;
  facts.artifacts = artifacts;
  facts.logical_operation = logical_operation;
  if (instruction >= 0 && instruction < ctx->program->instruction_count) {
    const USILInstruction *owner = &ctx->program->instructions[instruction];
    facts.instruction_index = instruction;
    facts.source_instruction_index = owner->source_instruction_index;
    if (owner->operand_count > 0) {
      USILOperandUseInfo use;
      if (usil_instruction_operand_use(ctx->program, owner, 0, &use) &&
          use.use == USIL_OPERAND_USE_DESTINATION)
        facts.lanes = usil_operand_destination_lane_mask(&owner->operands[0]);
    }
  }
  /* Before independent header replay begins, keep each actual preheader
   * emission stable across its synchronous callback. This owns byte stability,
   * without granting independent syntax authority to a generated preamble. */
  const bool guard_preheader = ctx->natural_structured_owners_guarded &&
      !ctx->natural_structured_header_source.buf;
  if (guard_preheader && !source_builder_storage_valid(ctx->sb)) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    return;
  }
  const size_t source_length = guard_preheader ? ctx->sb->len : 0;
  uint8_t source_digest[32];
  if (guard_preheader) common_sha256(ctx->sb->buf, source_length, source_digest);
  const bool observed = hlsl_source_quality_analysis_emission(ctx->source_quality_analysis, &facts);
  if (guard_preheader) {
    uint8_t current_digest[32];
    const bool same_storage = source_builder_storage_valid(ctx->sb) && ctx->sb->len == source_length;
    if (same_storage) common_sha256(ctx->sb->buf, source_length, current_digest);
    if (!same_storage || memcmp(source_digest, current_digest, sizeof(source_digest))) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                     HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
      return;
    }
  }
  if (!observed)
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_INSTRUCTION_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
  else
    hlsl_source_quality_interface_statement_emitted(ctx, instruction);
}

static bool owned_stage_quality_observer(void *context, const HLSLSourceQualityObservation *observation) {
  HLSLEmitterContext *ctx = context;
  if (ctx->high_level_packed_outputs) return packed_output_quality_observer(ctx, observation);
  return hlsl_stage_coverage_observation(ctx, observation) &&
      (!ctx->source_quality_forward_observer ||
       ctx->source_quality_forward_observer(ctx->source_quality_forward_observer_context, observation));
}

bool hlsl_source_quality_initialize(HLSLEmitterContext *ctx, const HLSLEmitOptions *options) {
  if (!options || !options->source_quality) return true;
  ctx->source_quality_forward_observer = options->source_quality_observer;
  ctx->source_quality_forward_observer_context = options->source_quality_observer_context;
  const HLSLSourceQualityRequest quality_request = {
      .stage = ctx->program->program_type,
      .pass_index = options->source_quality_pass_index,
      .entry_point_index = options->source_quality_entry_point_index,
      .emission_status = HLSL_EMIT_STATUS_OK,
      .expression_facts = emitter_source_quality_expression_facts,
      .facts_context = ctx,
      .observer = ctx->stage_coverage || ctx->matrix_use_capture || ctx->high_level_packed_outputs
          ? owned_stage_quality_observer : options->source_quality_observer,
      .observer_context = ctx->stage_coverage || ctx->matrix_use_capture || ctx->high_level_packed_outputs
          ? ctx : options->source_quality_observer_context};
  ctx->source_quality_analysis =
      hlsl_source_quality_analysis_create(&quality_request, options->source_quality);
  if (ctx->source_quality_analysis) return true;
  hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                 HLSL_EMIT_PHASE_CONTEXT_ALLOCATION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
  return false;
}

void hlsl_source_quality_finish_emission(HLSLEmitterContext *ctx) {
  const bool natural_body_complete = !ctx->natural_structured_owners_guarded ||
      !sb_ok(ctx->sb) || hlsl_natural_structured_body_inventory_complete(ctx);
  hlsl_stage_coverage_finish(ctx);
  if (ctx->source_quality_analysis) {
    HLSLEmitStatus status = ctx->diagnostic ? ctx->diagnostic->status : HLSL_EMIT_STATUS_OK;
    if (status == HLSL_EMIT_STATUS_OK && !sb_ok(ctx->sb)) status = HLSL_EMIT_STATUS_OUTPUT_FAILED;
    if (status == HLSL_EMIT_STATUS_OK &&
        ((ctx->source_quality_interface_required &&
          !hlsl_source_quality_interface_inventory_complete(ctx)) ||
         (ctx->source_quality_cbuffer_required &&
          !hlsl_source_quality_cbuffer_inventory_complete(ctx)) ||
         (ctx->source_quality_geometry_flow_required &&
          !hlsl_geometry_control_flow_inventory_complete(ctx)) ||
         !natural_body_complete) &&
        !hlsl_source_quality_analysis_mark_incomplete_unit(ctx->source_quality_analysis)) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                     HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
      status = HLSL_EMIT_STATUS_ANALYSIS_FAILED;
    }
    if (!hlsl_source_quality_analysis_finish(ctx->source_quality_analysis, status, 1) &&
        status == HLSL_EMIT_STATUS_OK)
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                     HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    hlsl_source_quality_analysis_destroy(ctx->source_quality_analysis);
  }
  ctx->source_quality_analysis = NULL;
}

static void free_emitter_context(HLSLEmitterContext *ctx) {
  hlsl_source_quality_finish_emission(ctx);
  if (ctx->packed_output_guard) {
    if (sb_ok(ctx->sb) && (!packed_output_model_matches(ctx) ||
        !packed_output_source_matches(ctx, true) ||
        !packed_output_maps_equal(ctx->expression_source_map, &ctx->packed_output_guard->expected_map) ||
        !hlsl_expression_source_map_matches(ctx->expression_source_map, ctx->program, ctx->sb->buf)))
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                     HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    if (!sb_ok(ctx->sb)) packed_output_guard_rollback(ctx);
    packed_output_guard_dispose(ctx);
  }
  if (ctx->natural_structured_owners_guarded && !sb_ok(ctx->sb) && ctx->expression_source_map)
    memset(ctx->expression_source_map, 0, sizeof(*ctx->expression_source_map));
  if (ctx->natural_structured_owners_guarded && sb_ok(ctx->sb) &&
      !hlsl_natural_structured_body_inventory_matches(ctx)) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    if (ctx->expression_source_map) memset(ctx->expression_source_map, 0, sizeof(*ctx->expression_source_map));
  }
  hlsl_natural_structured_body_inventory_dispose(ctx);
  hlsl_geometry_control_flow_inventory_free(ctx);
  free(ctx->cb_reg_map);
  free_sampler_name_map(ctx);
  free_cbuffer_emission_layouts(ctx);
  free_hlsl_generation_state(ctx);
  free(ctx->skip_instruction);
  free(ctx->modulo_divisor);
  free(ctx->inst_reg_permutation);
  free(ctx->inst_reg_is_scrambled);
  free(ctx->loop_info);
  free(ctx->typed_loop_bound_instructions);
  free(ctx->cross_info);
  free(ctx->saved_mul_id);
  free(ctx->saved_mul_is_definition);
  free(ctx->saved_mul_reverse_definition);
  free(ctx->decompositions);
  free(ctx->has_decomposition);
  free(ctx->ftoi_temps);
  free(ctx->has_ftoi_temp);
  free(ctx->int_temps);
  free(ctx->has_int_temp);
  free(ctx->write_redirects);
  free(ctx->has_write_redirect);
  free(ctx->write_redirect_storage);
  free(ctx->deferred_floats);
  free(ctx->has_deferred_float);
  free_d3dcompiler_model(ctx);
  free_semantic_lifts(ctx);
  free_hlsl_storage_plan(ctx);
  free_lane_value_types(ctx);
  free_hlsl_use_def_graph(ctx);
  free_component_provenance(ctx);
  free_hlsl_ssa_graph(ctx);
  free_control_flow_graph(ctx);
}

static bool allocate_emitter_temp_state(HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->program || ctx->program->temp_count < 0 ||
      ctx->program->instruction_count < 0) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                   HLSL_EMIT_PHASE_STATE_ALLOCATION,
                   HLSL_EMIT_REASON_INTERNAL_INVARIANT);
    return false;
  }
  const size_t register_count = (size_t)ctx->program->temp_count;
  ctx->temp_state_count = ctx->program->temp_count;
  if (register_count == 0) return true;

#define ALLOCATE_TEMP_STATE(member)                                              \
  do {                                                                           \
    if (dxbc_size_multiply_overflows(register_count, sizeof(*ctx->member))) {    \
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,                    \
                     HLSL_EMIT_PHASE_STATE_ALLOCATION,                           \
                     HLSL_EMIT_REASON_SIZE_OVERFLOW);                            \
      return false;                                                               \
    }                                                                             \
    ctx->member = calloc(register_count, sizeof(*ctx->member));                  \
    if (!ctx->member) {                                                           \
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,                    \
                     HLSL_EMIT_PHASE_STATE_ALLOCATION,                           \
                     HLSL_EMIT_REASON_ALLOCATION_FAILED);                        \
      return false;                                                               \
    }                                                                             \
  } while (0)
  ALLOCATE_TEMP_STATE(decompositions);
  ALLOCATE_TEMP_STATE(has_decomposition);
  ALLOCATE_TEMP_STATE(ftoi_temps);
  ALLOCATE_TEMP_STATE(has_ftoi_temp);
  ALLOCATE_TEMP_STATE(int_temps);
  ALLOCATE_TEMP_STATE(has_int_temp);
  ALLOCATE_TEMP_STATE(write_redirects);
  ALLOCATE_TEMP_STATE(has_write_redirect);
  ALLOCATE_TEMP_STATE(write_redirect_storage);
  ALLOCATE_TEMP_STATE(deferred_floats);
  ALLOCATE_TEMP_STATE(has_deferred_float);
#undef ALLOCATE_TEMP_STATE

  if (ctx->program->instruction_count == INT_MAX) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,
                   HLSL_EMIT_PHASE_STATE_ALLOCATION,
                   HLSL_EMIT_REASON_SIZE_OVERFLOW);
    return false;
  }
  const size_t snapshots =
      (size_t)ctx->program->instruction_count + 1u;
  if (dxbc_size_multiply_overflows(snapshots, register_count)) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,
                   HLSL_EMIT_PHASE_STATE_ALLOCATION,
                   HLSL_EMIT_REASON_SIZE_OVERFLOW);
    return false;
  }
  const size_t state_count = snapshots * register_count;
  if (dxbc_size_multiply_overflows(
          state_count, sizeof(*ctx->inst_reg_permutation)) ||
      dxbc_size_multiply_overflows(
          state_count, sizeof(*ctx->inst_reg_is_scrambled))) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,
                   HLSL_EMIT_PHASE_STATE_ALLOCATION,
                   HLSL_EMIT_REASON_SIZE_OVERFLOW);
    return false;
  }
  ctx->inst_reg_permutation =
      calloc(state_count, sizeof(*ctx->inst_reg_permutation));
  ctx->inst_reg_is_scrambled =
      calloc(state_count, sizeof(*ctx->inst_reg_is_scrambled));
  if (!ctx->inst_reg_permutation || !ctx->inst_reg_is_scrambled) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,
                   HLSL_EMIT_PHASE_STATE_ALLOCATION,
                   HLSL_EMIT_REASON_ALLOCATION_FAILED);
    return false;
  }
  return true;
}

static void mark_operand_ref(const DXBCOperand *op, bool *inputs_used,
                             bool *outputs_used) {
  if (!op)
    return;
  if (op->type == OPERAND_TYPE_INPUT) {
    if (op->register_index >= 0 &&
        op->register_index < HLSL_SM5_IO_REGISTER_COUNT) {
      inputs_used[op->register_index] = true;
    }
  } else if (op->type == OPERAND_TYPE_OUTPUT) {
    if (op->register_index >= 0 &&
        op->register_index < HLSL_SM5_IO_REGISTER_COUNT) {
      outputs_used[op->register_index] = true;
    }
  }
  if (op->rel_op0)
    mark_operand_ref(op->rel_op0, inputs_used, outputs_used);
  if (op->rel_op1)
    mark_operand_ref(op->rel_op1, inputs_used, outputs_used);
}

static bool hlsl_reserved_identifier_options_valid(
    const HLSLEmitOptions *options) {
  if (!options) return true;
  if (options->global_declarations && options->mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE)
    return false;
  if ((options->source_quality_observer && !options->source_quality) ||
      (options->source_quality_observer_context && !options->source_quality_observer))
    return false;
  const bool has_pointer =
      options->reserved_preprocessor_identifiers != NULL;
  const bool has_count =
      options->reserved_preprocessor_identifier_count != 0;
  if (has_pointer != has_count) return false;
  for (size_t index = 0;
       index < options->reserved_preprocessor_identifier_count; ++index) {
    const char *identifier =
        options->reserved_preprocessor_identifiers[index];
    if (!identifier || !identifier[0] || strcmp(identifier, "defined") == 0)
      return false;
    const unsigned char first = (unsigned char)identifier[0];
    if (!((first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z') || first == '_')) {
      return false;
    }
    for (size_t character = 1; identifier[character]; ++character) {
      const unsigned char value = (unsigned char)identifier[character];
      if (!((value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '_')) {
        return false;
      }
    }
  }
  return true;
}

static bool hlsl_emit_with_options_impl(
    const USILProgram *program, StringBuilder *sb,
    const SerializedProgramParameters *params,
    const SerializedProgramParameters *common_params,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    HLSLMatrixUseCapture *matrix_capture, HLSLStageCoverage *stage_coverage,
    HLSLEmitDiagnostic *diagnostic, bool packed_replay) {
  hlsl_emit_diagnostic_init(diagnostic);
  if (options && options->expression_source_map)
    memset(options->expression_source_map, 0, sizeof(*options->expression_source_map));
  if (!sb || !program) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_ARGUMENT,
                          HLSL_EMIT_PHASE_ARGUMENT_VALIDATION,
                          HLSL_EMIT_REASON_INVALID_ARGUMENT);
    if (sb) sb->failed = true;
    return false;
  }
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_OUTPUT_FAILED,
                          HLSL_EMIT_PHASE_ARGUMENT_VALIDATION,
                          HLSL_EMIT_REASON_OUTPUT_ALREADY_FAILED);
    return false;
  }
  if (!hlsl_reserved_identifier_options_valid(options)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_ARGUMENT,
                          HLSL_EMIT_PHASE_ARGUMENT_VALIDATION,
                          HLSL_EMIT_REASON_INVALID_ARGUMENT);
    sb->failed = true;
    return false;
  }
  if (program->program_type == DXBC_PROGRAM_TYPE_COMPUTE) {
    return hlsl_emit_compute_stage(program, sb, names, options, diagnostic);
  }
  HLSLEmitMode emit_mode = options ? options->mode : HLSL_EMIT_MODE_RECOMPILE;
  if (!validate_program_for_hlsl(program, emit_mode, diagnostic)) {
    sb->failed = true;
    return false;
  }
  if ((emit_mode != HLSL_EMIT_MODE_RECOMPILE &&
       emit_mode != HLSL_EMIT_MODE_READABLE &&
       emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) ||
      (options && options->expression_source_map &&
       emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_ARGUMENT,
                          HLSL_EMIT_PHASE_ARGUMENT_VALIDATION,
                          HLSL_EMIT_REASON_INVALID_MODE);
    sb->failed = true;
    return false;
  }
  if (emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE &&
      (program->instruction_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT || !program->has_stage_contract ||
       (program->program_type != DXBC_PROGRAM_TYPE_VERTEX &&
        program->program_type != DXBC_PROGRAM_TYPE_PIXEL &&
        !hlsl_high_level_geometry_interface_supported(program, emit_mode) &&
        !hlsl_high_level_domain_interface_supported(program, emit_mode) &&
        !hlsl_high_level_patch_domain_supported(program, emit_mode) &&
        !hlsl_high_level_hull_source_supported(program, emit_mode)))) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                          HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                          HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
    sb->failed = true;
    return false;
  }
  if (options && options->unity_uv_helper &&
      (emit_mode == HLSL_EMIT_MODE_READABLE ||
       (options->unity_uv_helper != HLSL_UNITY_UV_INCLUDE &&
        options->unity_uv_helper != HLSL_UNITY_UV_EXTERNAL_INCLUDE) ||
       !hlsl_unity_uv_lift_matches(program))) {
      hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                            HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
                            HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
      sb->failed = true;
      return false;
  }
  const char* entry_point = (names && names->entry_point && names->entry_point[0]) ? names->entry_point : "main";
  const char* input_struct = (names && names->input_struct && names->input_struct[0]) ? names->input_struct : "appdata";
  const char* output_struct = (names && names->output_struct && names->output_struct[0]) ? names->output_struct : "v2f";
  const char* readable_screen_pos_helper = NULL;
  if (emit_mode == HLSL_EMIT_MODE_READABLE) {
    readable_screen_pos_helper =
        (options && options->readable_screen_pos_helper &&
         options->readable_screen_pos_helper[0])
            ? options->readable_screen_pos_helper
            : "ComputeNonStereoScreenPos";
  }

  HLSLEmitterContext* ctx_ptr = (HLSLEmitterContext*)calloc(1, sizeof(HLSLEmitterContext));
  if (!ctx_ptr) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_ALLOCATION_FAILED,
                          HLSL_EMIT_PHASE_CONTEXT_ALLOCATION,
                          HLSL_EMIT_REASON_ALLOCATION_FAILED);
    sb->failed = true;
    return false;
  }
#define ctx (*ctx_ptr)

  const size_t source_start = sb->len;
  ctx.program = program;
  ctx.diagnostic = diagnostic;
  ctx.emit_mode = emit_mode;
  ctx.omit_unity_builtin_declarations =
      options && options->omit_unity_builtin_declarations;
  ctx.is_vertex = (strncmp(program->shader_type_model, "vs", 2) == 0);
  ctx.is_geometry =
      program->program_type == DXBC_PROGRAM_TYPE_GEOMETRY;
  ctx.params = params;
  ctx.common_params = common_params;
  ctx.sb = sb;
  ctx.reserved_preprocessor_identifiers =
      options ? options->reserved_preprocessor_identifiers : NULL;
  ctx.reserved_preprocessor_identifier_count =
      options ? options->reserved_preprocessor_identifier_count : 0;
  ctx.expression_source_map = options ? options->expression_source_map : NULL;
  ctx.matrix_use_capture = matrix_capture;
  ctx.stage_coverage = stage_coverage ? stage_coverage : matrix_capture ? &matrix_capture->coverage : NULL;
  ctx.unity_uv_helper = options && options->unity_uv_helper;
  ctx.readable_screen_pos_helper = readable_screen_pos_helper;
  ctx.readable_screen_pos_mul_y_idx = -1;
  ctx.readable_screen_pos_mul_xzw_idx = -1;
  ctx.readable_screen_pos_add_idx = -1;
  ctx.readable_screen_pos_mov_idx = -1;
  ctx.indent = 4;
  ctx.high_level_direct_return = high_level_direct_return_supported(program, emit_mode) &&
      !(options && options->unity_uv_helper);
  ctx.high_level_geometry = hlsl_high_level_geometry_interface_supported(program, emit_mode);
  ctx.high_level_domain = hlsl_high_level_domain_interface_supported(program, emit_mode);
  if (ctx.high_level_domain && !hlsl_domain_owned_contract_digest(program, ctx.domain_owner_digest)) {
    hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_PROGRAM_VALIDATION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }
  if (ctx.high_level_domain && hlsl_domain_output_plan_prepare(program, &ctx.domain_output_plan))
    ctx.high_level_direct_return = true;
  ctx.high_level_interface = ctx.high_level_direct_return ||
      ctx.high_level_geometry || ctx.high_level_domain ||
      (hlsl_high_level_struct_interface_supported(program, emit_mode) &&
       !(options && options->unity_uv_helper));
  ctx.high_level_packed_outputs = emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE &&
      !ctx.unity_uv_helper && hlsl_packed_output_candidate(program);
  ctx.preferred_output_struct_name = output_struct;
  ctx.preferred_input_struct_name = input_struct;
  ctx.entry_point_name = entry_point;
  ctx.global_declarations = options ? options->global_declarations : NULL;
  if (!hlsl_source_quality_initialize(&ctx, options)) {
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }
  const bool defer_multi_output_interface = emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE &&
      !ctx.high_level_interface && !ctx.unity_uv_helper && program->output_count > 1 &&
      (program->program_type == DXBC_PROGRAM_TYPE_VERTEX || program->program_type == DXBC_PROGRAM_TYPE_PIXEL) &&
      (ctx.high_level_packed_outputs || hlsl_natural_structured_candidate(program));
  if (emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE && program->output_count > 1 &&
      (program->program_type == DXBC_PROGRAM_TYPE_VERTEX || program->program_type == DXBC_PROGRAM_TYPE_PIXEL) &&
      !ctx.high_level_interface && !defer_multi_output_interface) {
    hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_UNSUPPORTED, HLSL_EMIT_PHASE_INTERFACE_EMISSION,
                   HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }

  if (hlsl_high_level_hull_source_supported(program, emit_mode)) {
    const bool emitted = hlsl_emit_high_level_hull_stage(&ctx);
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return emitted && sb_ok(sb);
  }

  if (hlsl_high_level_patch_domain_supported(program, emit_mode)) {
    const bool emitted = hlsl_emit_high_level_patch_domain(&ctx);
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return emitted && sb_ok(sb);
  }

  if (!ctx.high_level_domain && (program->program_type == DXBC_PROGRAM_TYPE_HULL ||
      program->program_type == DXBC_PROGRAM_TYPE_DOMAIN)) {
    if (!hlsl_source_quality_begin_entry(&ctx, false)) {
      free_emitter_context(&ctx);
      free(ctx_ptr);
      return false;
    }
    const bool maps_ready = build_sampler_name_map(&ctx);
    if (!maps_ready) {
      hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                     HLSL_EMIT_PHASE_SAMPLER_BINDING_MAP,
                     HLSL_EMIT_REASON_SAMPLER_CONTRACT_MISMATCH);
    }
    const bool emitted = maps_ready &&
        hlsl_emit_exact_tessellation_stage(&ctx, entry_point);
    if (!emitted && maps_ready) {
      hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                     HLSL_EMIT_PHASE_TESSELLATION_EMISSION,
                     HLSL_EMIT_REASON_LOWERING_FAILED);
    }
    free_emitter_context(&ctx);
    const bool success = emitted && sb_ok(sb);
    if (!success && diagnostic &&
        diagnostic->status == HLSL_EMIT_STATUS_OK) {
      hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_OUTPUT_FAILED,
                            HLSL_EMIT_PHASE_TESSELLATION_EMISSION,
                            HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED);
    }
    free(ctx_ptr);
    return success;
  }

  if (program->instruction_count > 0) {
    const size_t instruction_count = (size_t)program->instruction_count;
#define ALLOCATE_INSTRUCTION_STATE(member)                                      \
    do {                                                                         \
      if (dxbc_size_multiply_overflows(instruction_count,                       \
                                       sizeof(*ctx.member))) {                   \
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,                 \
                       HLSL_EMIT_PHASE_STATE_ALLOCATION,                         \
                       HLSL_EMIT_REASON_SIZE_OVERFLOW);                          \
        free_emitter_context(&ctx);                                              \
        free(ctx_ptr);                                                           \
        return false;                                                            \
      }                                                                          \
      ctx.member = calloc(instruction_count, sizeof(*ctx.member));               \
      if (!ctx.member) {                                                         \
        hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,                 \
                       HLSL_EMIT_PHASE_STATE_ALLOCATION,                         \
                       HLSL_EMIT_REASON_ALLOCATION_FAILED);                      \
        free_emitter_context(&ctx);                                              \
        free(ctx_ptr);                                                           \
        return false;                                                            \
      }                                                                          \
    } while (0)
    ALLOCATE_INSTRUCTION_STATE(skip_instruction);
    ALLOCATE_INSTRUCTION_STATE(modulo_divisor);
    ALLOCATE_INSTRUCTION_STATE(loop_info);
    ALLOCATE_INSTRUCTION_STATE(typed_loop_bound_instructions);
    ALLOCATE_INSTRUCTION_STATE(cross_info);
    ALLOCATE_INSTRUCTION_STATE(saved_mul_id);
    ALLOCATE_INSTRUCTION_STATE(saved_mul_is_definition);
    ALLOCATE_INSTRUCTION_STATE(saved_mul_reverse_definition);
#undef ALLOCATE_INSTRUCTION_STATE
    for (size_t instruction = 0; instruction < instruction_count;
         ++instruction) {
      ctx.typed_loop_bound_instructions[instruction].loop_instruction = -1;
    }
  }
  if (!allocate_emitter_temp_state(&ctx)) {
    if (diagnostic && diagnostic->status == HLSL_EMIT_STATUS_OK) {
      hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ALLOCATION_FAILED,
                     HLSL_EMIT_PHASE_STATE_ALLOCATION,
                     HLSL_EMIT_REASON_ALLOCATION_FAILED);
    }
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }

  if (!RunAnalysisPasses(&ctx)) {
    if (diagnostic && diagnostic->status == HLSL_EMIT_STATUS_OK) {
      hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                     HLSL_EMIT_PHASE_COMPILER_MODEL_ANALYSIS,
                     HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    }
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }
  /* A complete natural-width IF plan supplies the natural struct route.
   * Keep its proof after CFG/SSA analysis and before naming or quality begins;
   * the established direct-return and full-width interfaces retain their path. */
  const bool natural_structured = emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE &&
      !ctx.unity_uv_helper && hlsl_natural_structured_preflight(&ctx);
  const bool packed_output = ctx.high_level_packed_outputs && hlsl_packed_output_preflight(&ctx);
  if ((ctx.high_level_packed_outputs && !packed_output) ||
      (defer_multi_output_interface && !natural_structured && !packed_output)) {
    hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_UNSUPPORTED, HLSL_EMIT_PHASE_INTERFACE_EMISSION,
                   HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }
  if (packed_output) (void)hlsl_position_output_plan_prepare(program, &ctx.position_output_plan);
  if (emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE &&
      !ctx.high_level_interface && !ctx.unity_uv_helper && program->output_count >= 1 &&
      program->output_count <= HLSL_SM5_IO_REGISTER_COUNT &&
      (program->program_type == DXBC_PROGRAM_TYPE_VERTEX || program->program_type == DXBC_PROGRAM_TYPE_PIXEL) &&
      (natural_structured || packed_output)) {
    ctx.high_level_interface = true;
  }
  if (ctx.high_level_interface && !hlsl_prepare_high_level_interface(&ctx)) {
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }
  if (emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE &&
      !hlsl_prepare_high_level_functions(&ctx)) {
    hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                   HLSL_EMIT_PHASE_STRUCTURAL_HELPER_EMISSION,
                   HLSL_EMIT_REASON_LOWERING_FAILED);
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }
  if (natural_structured) {
    if (sb->len != source_start || !source_builder_storage_valid(sb) ||
        !hlsl_natural_structured_owned_contract_digest(program, ctx.natural_structured_owner_digest)) {
      hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                     HLSL_EMIT_PHASE_COMPILER_MODEL_ANALYSIS, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
      free_emitter_context(&ctx);
      free(ctx_ptr);
      return false;
    }
    ctx.natural_structured_append_prefix_length = source_start;
    common_sha256(sb->buf, source_start, ctx.natural_structured_append_prefix_digest);
    ctx.natural_structured_owners_guarded = true;
  }
  if (packed_output && !packed_replay &&
      !packed_output_guard_prepare(&ctx, source_start, names, options)) {
    hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_COMPILER_MODEL_ANALYSIS, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }
  if (!hlsl_source_quality_begin_entry(&ctx, hlsl_source_quality_inventory_supported(&ctx))) {
    free_emitter_context(&ctx);
    free(ctx_ptr);
    return false;
  }

  bool inputs_used[HLSL_SM5_IO_REGISTER_COUNT] = {false};
  bool outputs_used[HLSL_SM5_IO_REGISTER_COUNT] = {false};
  for (int i = 0; i < program->instruction_count; i++) {
    const USILInstruction *inst = &program->instructions[i];
    for (int j = 0; j < inst->operand_count; j++) {
      mark_operand_ref(&inst->operands[j], inputs_used, outputs_used);
    }
  }

  if (ctx.unity_uv_helper) {
      if (ctx.compiler_model.replacement_count || !hlsl_float4_program_supported(&ctx)) {
          hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                         HLSL_EMIT_PHASE_COMPILER_MODEL_ANALYSIS,
                         HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
          goto cleanup;
      }
      if (options->unity_uv_helper == HLSL_UNITY_UV_INCLUDE)
          sb_append(sb, HLSL_UNITY_UV_INCLUDE_SOURCE);
  }
  emit_comments_and_icb(&ctx);
  if (sb_ok(sb) && hlsl_source_quality_inventory_supported(&ctx))
    hlsl_source_quality_emission(&ctx, 0, false, -1);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_PROGRAM,
                          HLSL_EMIT_PHASE_ICB_EMISSION,
                          HLSL_EMIT_REASON_INVALID_METADATA_SHAPE);
    goto cleanup;
  }

  // 2. Emit Constant Buffers
  emit_cbuffers(&ctx);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_METADATA,
                          HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                          HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    goto cleanup;
  }

  // Emit Helper Functions for Constant Buffer indexing
  if (emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE)
    emit_cbuffer_helpers(&ctx);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_METADATA,
                          HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION,
                          HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    goto cleanup;
  }

  // 3. Emit Textures & Samplers
  emit_resources(&ctx);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_METADATA,
                          HLSL_EMIT_PHASE_RESOURCE_EMISSION,
                          HLSL_EMIT_REASON_RESOURCE_CONTRACT_MISMATCH);
    goto cleanup;
  }

  // Complete, fail-closed compiler inverses may need source-level helpers.
  emit_exact_structural_helpers(&ctx);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                          HLSL_EMIT_PHASE_STRUCTURAL_HELPER_EMISSION,
                          HLSL_EMIT_REASON_LOWERING_FAILED);
    goto cleanup;
  }

  if (emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE && !emit_high_level_functions(&ctx)) {
    hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                   HLSL_EMIT_PHASE_STRUCTURAL_HELPER_EMISSION,
                   HLSL_EMIT_REASON_LOWERING_FAILED);
    goto cleanup;
  }

  // 4. Emit Input/Output structures
  emit_io_structs(&ctx, input_struct, output_struct);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                          HLSL_EMIT_PHASE_INTERFACE_EMISSION,
                          HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    goto cleanup;
  }

  // 5. Entry point and local variables declaration
  emit_entry_point_declarations(&ctx, entry_point, input_struct, output_struct,
                                inputs_used, outputs_used);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                          HLSL_EMIT_PHASE_ENTRY_POINT_EMISSION,
                          HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
    goto cleanup;
  }

  // 7. Emit instructions
  emit_instructions(&ctx);
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                          HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                          HLSL_EMIT_REASON_LOWERING_FAILED);
    goto cleanup;
  }

  // 8. Return output struct
  const size_t return_begin = sb->len;
  emit_return_block(&ctx);
  if (ctx.expression_source_map && ctx.expression_source_map->count) {
    HLSLExpressionOrigin* origin = &ctx.expression_source_map->origins[
        ctx.expression_source_map->count - 1];
    origin->source_begin = ctx.domain_output_plan.present
        ? ctx.domain_output_return_begin + (size_t)ctx.indent : return_begin;
    origin->source_end = sb->len;
  }
  if (!sb_ok(sb)) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_UNSUPPORTED,
                          HLSL_EMIT_PHASE_RETURN_EMISSION,
                          HLSL_EMIT_REASON_LOWERING_FAILED);
  }

cleanup:
  if (sb_ok(sb) && emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE)
    hlsl_expression_identifiers_available(&ctx, source_start);
  if (ctx.expression_source_map) {
    ctx.expression_source_map->complete = sb_ok(sb);
    if (sb_ok(sb) && !hlsl_expression_source_map_matches(ctx.expression_source_map, program, sb->buf))
      hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                     HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    if (!sb_ok(sb)) memset(ctx.expression_source_map, 0, sizeof(*ctx.expression_source_map));
  }
  free_emitter_context(&ctx);
  /* The final quality callbacks can still touch caller-owned input. Keep the
   * complete admitted decoded model stable through every callback, including
   * unit completion, before returning source or an owned capture. */
  if (ctx.high_level_domain || ctx.natural_structured_owners_guarded) {
    uint8_t current[32];
    const bool stable = ctx.high_level_domain
        ? hlsl_domain_owned_contract_digest(program, current) &&
          !memcmp(current, ctx.domain_owner_digest, sizeof(current))
        : hlsl_natural_structured_owned_contract_digest(program, current) &&
          !memcmp(current, ctx.natural_structured_owner_digest, sizeof(current));
    if (!stable) {
      hlsl_emit_fail(&ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                     HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
      if (ctx.expression_source_map) memset(ctx.expression_source_map, 0, sizeof(*ctx.expression_source_map));
      if (options && options->source_quality) {
        options->source_quality->classification = HLSL_SOURCE_QUALITY_FAILED;
        options->source_quality->emission_status = HLSL_EMIT_STATUS_ANALYSIS_FAILED;
        options->source_quality->reasons |= HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED;
      }
    }
  }
  bool success = sb_ok(sb);
  if (!success && diagnostic && diagnostic->status == HLSL_EMIT_STATUS_OK) {
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_OUTPUT_FAILED,
                          HLSL_EMIT_PHASE_OUTPUT,
                          HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED);
  }
  free(ctx_ptr);
#undef ctx
  return success;
}

static bool emit_with_diagnostic_and_capture(
    const USILProgram *program, StringBuilder *sb,
    const SerializedProgramParameters *params,
    const SerializedProgramParameters *common_params,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    HLSLMatrixUseCapture *matrix_capture, HLSLStageCoverage *stage_coverage, HLSLEmitDiagnostic *diagnostic) {
  HLSLEmitDiagnostic local_diagnostic;
  HLSLEmitDiagnostic *failure = diagnostic;
  HLSLSourceQualityResult *quality = options ? options->source_quality : NULL;
  if (quality) {
    if (!failure) failure = &local_diagnostic;
    memset(quality, 0, sizeof(*quality));
    quality->stage = program ? program->program_type : DXBC_PROGRAM_TYPE_INVALID;
    quality->pass_index = options->source_quality_pass_index;
    quality->entry_point_index = options->source_quality_entry_point_index;
    quality->classification = HLSL_SOURCE_QUALITY_FAILED;
  }
  bool success = hlsl_emit_with_options_impl(program, sb, params, common_params,
                                             names, options, matrix_capture, stage_coverage, failure, false);
  if (quality) {
    quality->emission_status = failure->status;
    if (!success) {
      bool unsupported = failure->status == HLSL_EMIT_STATUS_UNSUPPORTED;
      quality->classification = unsupported ? HLSL_SOURCE_QUALITY_UNSUPPORTED
                                            : HLSL_SOURCE_QUALITY_FAILED;
      quality->reasons |= unsupported ? HLSL_SOURCE_QUALITY_REASON_EMISSION_UNSUPPORTED
                                      : HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED;
    }
  }
  return success;
}

bool hlsl_emit_with_options_diagnostic(
    const USILProgram *program, StringBuilder *output,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    const HLSLEmitNames *names, const HLSLEmitOptions *options, HLSLEmitDiagnostic *diagnostic) {
  return emit_with_diagnostic_and_capture(program, output, current, common, names, options, NULL, NULL, diagnostic);
}

bool hlsl_emit_with_matrix_capture(
    const USILProgram *program, StringBuilder *output,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    HLSLMatrixUseCapture *capture, HLSLEmitDiagnostic *diagnostic) {
  return capture && emit_with_diagnostic_and_capture(program, output, current, common, names, options, capture, NULL, diagnostic);
}

bool hlsl_emit_with_stage_coverage(const USILProgram *program, StringBuilder *output,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    HLSLStageCoverage *coverage, HLSLEmitDiagnostic *diagnostic) {
  uint8_t digest[32];
  if (!coverage || coverage->began || coverage->finished || coverage->source || coverage->roots || coverage->syntax ||
      coverage->recorded_syntax || coverage->root_count || coverage->syntax_count || coverage->unit_count ||
      coverage->recorded_root_count || coverage->recorded_syntax_count || coverage->recorded_unit_count ||
      coverage->instruction_count || coverage->source_size || coverage->node_count ||
      coverage->obligations || coverage->required_binding_mask || coverage->schema != HLSL_STAGE_COVERAGE_ORDINARY_ENTRY ||
      coverage->hull_contract.tessellation.valid || coverage->recorded_hull_contract.tessellation.valid ||
      coverage->hull_contract.tessellation.phase_count || coverage->recorded_hull_contract.tessellation.phase_count ||
      coverage->hull_contract.patch_constant_count || coverage->recorded_hull_contract.patch_constant_count ||
      coverage->hull_contract.signature_declaration_count || coverage->recorded_hull_contract.signature_declaration_count ||
      coverage->hull_contract.cbuffer_count || coverage->recorded_hull_contract.cbuffer_count ||
      !hlsl_stage_coverage_hull_icb_empty(&coverage->hull_icb) ||
      !hlsl_stage_coverage_domain_output_empty(&coverage->domain_output) ||
      !hlsl_stage_coverage_domain_owners_empty(coverage) ||
      !output || !sb_ok(output) || output->len || !options || options->mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      (!hlsl_high_level_domain_interface_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) &&
       !hlsl_hull_owned_contract_digest(program, digest))) {
    hlsl_emit_diagnostic_init(diagnostic);
    hlsl_emit_set_failure(diagnostic, HLSL_EMIT_STATUS_INVALID_ARGUMENT,
        HLSL_EMIT_PHASE_ARGUMENT_VALIDATION, HLSL_EMIT_REASON_INVALID_ARGUMENT);
    return false;
  }
  HLSLSourceQualityResult local_quality;
  HLSLEmitDiagnostic local_diagnostic;
  HLSLEmitDiagnostic *failure = diagnostic ? diagnostic : &local_diagnostic;
  HLSLEmitOptions owned_options = *options;
  if (!owned_options.source_quality) owned_options.source_quality = &local_quality;
  if (program->program_type == DXBC_PROGRAM_TYPE_HULL)
    coverage->schema = HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT;
  bool emitted = emit_with_diagnostic_and_capture(program, output, current, common,
      names, &owned_options, NULL, coverage, failure);
  if (!emitted || !hlsl_stage_coverage_validate(coverage, output)) {
    if (emitted) {
      hlsl_emit_set_failure(failure, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
          HLSL_EMIT_PHASE_OUTPUT, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
      output->failed = true;
      owned_options.source_quality->classification = HLSL_SOURCE_QUALITY_FAILED;
      owned_options.source_quality->emission_status = failure->status;
      owned_options.source_quality->reasons |= HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED;
    }
    if (options->expression_source_map)
      memset(options->expression_source_map, 0, sizeof(*options->expression_source_map));
    /* A rejected capture never retains a successful receipt or a partial
     * signature/tree lease. Argument rejection above preserves old results. */
    hlsl_stage_coverage_dispose(coverage);
    return false;
  }
  return true;
}

bool hlsl_emit_with_options(const USILProgram *program, StringBuilder *sb,
                            const SerializedProgramParameters *params,
                            const SerializedProgramParameters *common_params,
                            const HLSLEmitNames *names,
                            const HLSLEmitOptions *options) {
  return hlsl_emit_with_options_diagnostic(
      program, sb, params, common_params, names, options, NULL);
}

bool hlsl_emit(const USILProgram *program, StringBuilder *sb,
               const SerializedProgramParameters *params,
               const SerializedProgramParameters *common_params,
               const HLSLEmitNames *names) {
  const HLSLEmitOptions recompile_options =
      HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  return hlsl_emit_with_options(program, sb, params, common_params, names,
                                &recompile_options);
}
