// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include "io/parameter_layout.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *allocate_cbuffer_declaration_name(const char *serialized_name,
                                               int register_index) {
  const char *prefix = "";
  const char *name = serialized_name;
  char fallback[32];
  if (!name) {
    int written = snprintf(fallback, sizeof(fallback), "cb%d", register_index);
    if (written < 0 || (size_t)written >= sizeof(fallback)) return NULL;
    name = fallback;
  } else if (name[0] == '$') {
    prefix = "_";
    name++;
  }

  size_t prefix_length = strlen(prefix);
  size_t name_length = strlen(name);
  if (prefix_length > SIZE_MAX - name_length - 1u) return NULL;
  size_t allocation_size = prefix_length + name_length + 1u;
  char *result = malloc(allocation_size);
  if (!result) return NULL;
  memcpy(result, prefix, prefix_length);
  memcpy(result + prefix_length, name, name_length + 1u);
  return result;
}

static uint32_t temp_variable_byte_size(const TempVariable *variable) {
  if (variable->byte_size != 0) return variable->byte_size;
  uint32_t elements = variable->matrix_array_size
                          ? variable->matrix_array_size
                          : 1;
  if (variable->is_matrix) return variable->rows * elements * 16;
  if (variable->matrix_array_size) return elements * 16;
  return variable->dim * 4;
}

static bool byte_ranges_overlap(uint32_t lhs_offset, uint32_t lhs_size,
                                uint32_t rhs_offset, uint32_t rhs_size) {
  return (uint64_t)lhs_offset < (uint64_t)rhs_offset + rhs_size &&
         (uint64_t)rhs_offset < (uint64_t)lhs_offset + lhs_size;
}

enum {
  CBUFFER_AUTHORITY_STAGE = 1,
  CBUFFER_AUTHORITY_COMMON = 2,
  CBUFFER_AUTHORITY_READABLE_BUILTIN = 3
};

static void fail_cbuffer_location(
    HLSLEmitterContext *ctx, HLSLEmitStatus status, HLSLEmitPhase phase,
    HLSLEmitReason reason, int layout_index, int variable_index) {
  if (!ctx || !ctx->program || layout_index < 0 ||
      layout_index >= ctx->program->cbuffer_count ||
      layout_index >= ctx->cbuffer_layout_count) {
    hlsl_emit_fail(ctx, status, phase, reason);
    return;
  }
  const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[layout_index];
  HLSLEmitMetadataSource source = HLSL_EMIT_METADATA_SOURCE_PROGRAM;
  HLSLEmitMetadataKind kind = HLSL_EMIT_METADATA_CBUFFER;
  int record_index = layout_index;
  int member_index = -1;
  if (layout->serialized_name) {
    const SerializedProgramParameters *sources[2] = {
        ctx->params, ctx->common_params};
    const HLSLEmitMetadataSource source_kinds[2] = {
        HLSL_EMIT_METADATA_SOURCE_STAGE_PARAMETERS,
        HLSL_EMIT_METADATA_SOURCE_COMMON_PARAMETERS};
    bool found_serialized_buffer = false;
    for (size_t source_index = 0;
         source_index < 2 && !found_serialized_buffer; ++source_index) {
      const SerializedProgramParameters *parameters = sources[source_index];
      if (!parameters || parameters->cb_count < 0 ||
          (parameters->cb_count > 0 && !parameters->constant_buffers)) {
        continue;
      }
      for (int buffer = 0; buffer < parameters->cb_count; ++buffer) {
        const SerializedConstantBuffer *candidate =
            &parameters->constant_buffers[buffer];
        if (!candidate->name ||
            strcmp(candidate->name, layout->serialized_name) != 0) {
          continue;
        }
        source = source_kinds[source_index];
        record_index = buffer;
        found_serialized_buffer = true;
        break;
      }
    }
  }
  if (variable_index >= 0 && variable_index < layout->variable_count) {
    const TempVariable *variable = &layout->variables[variable_index];
    kind = HLSL_EMIT_METADATA_CBUFFER_VARIABLE;
    member_index = variable_index;
    const SerializedProgramParameters *parameters = NULL;
    if (variable->authority == CBUFFER_AUTHORITY_STAGE) {
      source = HLSL_EMIT_METADATA_SOURCE_STAGE_PARAMETERS;
      parameters = ctx->params;
    } else if (variable->authority == CBUFFER_AUTHORITY_COMMON) {
      source = HLSL_EMIT_METADATA_SOURCE_COMMON_PARAMETERS;
      parameters = ctx->common_params;
    }
    if (parameters) {
      bool found = false;
      for (int buffer = 0; buffer < parameters->cb_count && !found; ++buffer) {
        const SerializedConstantBuffer *candidate =
            &parameters->constant_buffers[buffer];
        for (int member = 0; member < candidate->var_count; ++member) {
          if (candidate->variables[member].name != variable->name) continue;
          record_index = buffer;
          member_index = member;
          found = true;
          break;
        }
      }
    }
  }
  hlsl_emit_fail_metadata(ctx, status, phase, reason, source, kind,
                          record_index, member_index, layout->reg);
}

static int cbuffer_layout_variable_index(
    const HLSLCBufferLayout *layout, const char *name) {
  if (!layout || !name) return -1;
  for (int index = 0; index < layout->variable_count; ++index) {
    if (layout->variables[index].name == name ||
        strcmp(layout->variables[index].name, name) == 0) return index;
  }
  return -1;
}

static bool temp_variables_equal(const TempVariable *left,
                                 const TempVariable *right) {
  return strcmp(left->name, right->name) == 0 &&
         left->type == right->type && left->rows == right->rows &&
         left->dim == right->dim && left->is_matrix == right->is_matrix &&
         left->matrix_array_size == right->matrix_array_size &&
         left->byte_offset == right->byte_offset &&
         left->byte_size == right->byte_size &&
         left->row_major == right->row_major;
}

static bool append_cbuffer_variable(HLSLCBufferLayout *layout,
                                    const TempVariable *candidate) {
  for (int index = 0; index < layout->variable_count; ++index) {
    const TempVariable *existing = &layout->variables[index];
    bool same_name = strcmp(existing->name, candidate->name) == 0;
    bool overlap = byte_ranges_overlap(
        existing->byte_offset, temp_variable_byte_size(existing),
        candidate->byte_offset, temp_variable_byte_size(candidate));
    if (!same_name && !overlap) continue;

    if (existing->authority < candidate->authority) {
      /* Sources are appended in authority order.  A stage-specific record is
       * definitive at its name/range; common TypeTree data only fills holes. */
      return true;
    }
    if (existing->authority == candidate->authority &&
        temp_variables_equal(existing, candidate)) {
      return true;
    }
    return false;
  }
  if (layout->variable_count == INT_MAX) return false;
  if (layout->variable_count == layout->variable_alloc) {
    int new_allocation = layout->variable_alloc == 0
                             ? 16
                             : layout->variable_alloc;
    if (new_allocation > INT_MAX / 2) new_allocation = INT_MAX;
    else new_allocation *= 2;
    if (new_allocation <= layout->variable_count ||
        dxbc_size_multiply_overflows((size_t)new_allocation,
                                     sizeof(*layout->variables))) {
      return false;
    }
    TempVariable *replacement = realloc(
        layout->variables,
        (size_t)new_allocation * sizeof(*layout->variables));
    if (!replacement) return false;
    layout->variables = replacement;
    layout->variable_alloc = new_allocation;
  }
  layout->variables[layout->variable_count++] = *candidate;
  return true;
}

static bool append_serialized_cbuffer_source(
    HLSLCBufferLayout *layout, const SerializedProgramParameters *parameters,
    const char *cb_name, uint8_t authority) {
  if (!parameters) return true;
  if (!cb_name || parameters->cb_count < 0 ||
      (parameters->cb_count > 0 && !parameters->constant_buffers)) {
    return false;
  }
  const uint32_t cbuffer_size = (uint32_t)layout->row_count * 16u;
  for (int cb_index = 0; cb_index < parameters->cb_count; ++cb_index) {
    const SerializedConstantBuffer *buffer =
        &parameters->constant_buffers[cb_index];
    if (!buffer->name) return false;
    if (strcmp(buffer->name, cb_name) != 0) continue;
    if (buffer->size != 0U) {
      if ((buffer->size & 15U) != 0U || buffer->size > UINT16_MAX + 1U) {
        return false;
      }
      const bool partial = buffer->has_is_partial && buffer->is_partial;
      const bool loose =
          buffer->role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
      /* A partial common record describes fields shared across a wider
       * family; its shell can legitimately be larger than this compiled
       * variant.  Likewise, the loose-parameter area is not a named shell.
       * Both contribute variables but cannot replace variant-specific named
       * reflection-size authority. */
      if (!partial && !loose) {
        if (buffer->size < cbuffer_size ||
            (layout->has_reflection_size_authority &&
             layout->reflection_size_bytes != buffer->size)) {
          return false;
        }
        layout->reflection_size_bytes = buffer->size;
        layout->has_reflection_size_authority = true;
      }
    }
    if (buffer->var_count < 0 ||
        (buffer->var_count > 0 && !buffer->variables)) {
      return false;
    }
    for (int variable_index = 0; variable_index < buffer->var_count;
         ++variable_index) {
      const SerializedVariable *variable = &buffer->variables[variable_index];
      DecodedVariableLayout decoded;
      if (!variable->name ||
          !parameter_layout_decode(parameters, variable, &decoded)) {
        return false;
      }
      uint32_t byte_size = parameter_layout_byte_size(&decoded);
      uint64_t wide_size = byte_size;
      uint64_t end = (uint64_t)decoded.byte_offset + wide_size;
      if (wide_size == 0 || wide_size > UINT32_MAX ||
          decoded.scalar_type > 2 ||
          (decoded.is_matrix && decoded.scalar_type != 0) ||
          (decoded.byte_offset & 3u) != 0 || (wide_size & 3u) != 0 ||
          end > UINT32_MAX) {
        return false;
      }
      /* Release DXBC sizes a cbuffer through its highest referenced register.
       * Serialized metadata remains authoritative for a variable whose tail
       * was optimized away, so retain its complete declaration while the
       * projection intersects only bytes present in this DXBC declaration. */
      if (decoded.byte_offset >= cbuffer_size) continue;

      TempVariable candidate;
      memset(&candidate, 0, sizeof(candidate));
      candidate.name = variable->name;
      candidate.type = decoded.scalar_type;
      candidate.rows = decoded.rows;
      candidate.dim = decoded.columns;
      candidate.is_matrix = decoded.is_matrix;
      candidate.matrix_array_size = decoded.array_size;
      candidate.reg_offset = decoded.byte_offset / 16u;
      candidate.byte_offset = decoded.byte_offset;
      candidate.byte_size = byte_size;
      /* Unity's D3D11 reporter accepts D3D_SVC_MATRIX_COLUMNS but its
       * ReportConstantVarOfBasicTypes path rejects D3D_SVC_MATRIX_ROWS.
       * Serialized matrix parameters therefore need the source-default
       * column-major declaration to survive the independent reflection
       * callback. Access formatting transposes the logical value so the
       * physical row reads and stripped DXBC remain unchanged. */
      candidate.row_major = false;
      candidate.authority = authority;
      if (!append_cbuffer_variable(layout, &candidate)) {
        return false;
      }
      layout->has_serialized_authority = true;
    }
  }
  return true;
}

static bool append_readable_builtin_fallback(HLSLCBufferLayout *layout,
                                              const char *cb_name) {
  if (!cb_name) return true;
  const char *normalized = cb_name[0] == '$' ? cb_name + 1 : cb_name;
  const uint32_t cbuffer_size = (uint32_t)layout->row_count * 16u;
  for (size_t index = 0; index < G_BUILTINS_COUNT; ++index) {
    if (strcmp(g_builtins[index].cb_name, normalized) != 0) continue;
    TempVariable candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.name = g_builtins[index].var_name;
    candidate.type = g_builtins[index].type;
    candidate.rows = g_builtins[index].is_matrix
                         ? g_builtins[index].rows
                         : 1;
    candidate.dim = g_builtins[index].dim;
    candidate.is_matrix = g_builtins[index].is_matrix;
    candidate.matrix_array_size = g_builtins[index].matrix_array_size;
    candidate.reg_offset = g_builtins[index].offset / 16u;
    candidate.byte_offset = g_builtins[index].offset;
    candidate.byte_size = temp_variable_byte_size(&candidate);
    candidate.row_major = false;
    candidate.authority = CBUFFER_AUTHORITY_READABLE_BUILTIN;
    if (candidate.byte_offset >= cbuffer_size) continue;
    if ((uint64_t)candidate.byte_offset + candidate.byte_size > cbuffer_size ||
        !append_cbuffer_variable(layout, &candidate)) {
      return false;
    }
  }
  return true;
}

static const BuiltinVariable *find_unity_include_builtin(
    const char *cbuffer_name, const char *variable_name) {
  if (!cbuffer_name || !variable_name) return NULL;
  const char *normalized = cbuffer_name[0] == '$'
                               ? cbuffer_name + 1
                               : cbuffer_name;
  const BuiltinVariable *match = NULL;
  for (size_t index = 0; index < G_BUILTINS_COUNT; ++index) {
    const BuiltinVariable *candidate = &g_builtins[index];
    if (strcmp(candidate->cb_name, normalized) != 0 ||
        strcmp(candidate->var_name, variable_name) != 0) {
      continue;
    }
    if (match) return NULL;
    match = candidate;
  }
  return match;
}

/* Unity's source compiler can flatten variables supplied by
 * UnityShaderVariables.cginc into a serialized $Globals parameter table.
 * The serialized table then retains the exact target byte offset but no
 * longer retains the source cbuffer which owned the declaration.  Treating
 * that flattened record as a user declaration produces an unconditional HLSL
 * redefinition error.
 *
 * Name alone is not sufficient authority.  This inverse accepts a flattened
 * alias only when every pinned include record with that name has one identical
 * source type/shape and the serialized variable has that exact shape.  The
 * target byte offset is deliberately not compared: flattening is the operation
 * which changed it.  A known name with a conflicting shape is malformed for
 * this compile environment and must fail closed instead of being redeclared. */
static int unity_include_global_alias_status(const TempVariable *variable) {
  if (!variable || !variable->name) return 0;
  const BuiltinVariable *contract = NULL;
  for (size_t index = 0; index < G_BUILTINS_COUNT; ++index) {
    const BuiltinVariable *candidate = &g_builtins[index];
    if (strcmp(candidate->var_name, variable->name) != 0) continue;
    if (contract &&
        (contract->type != candidate->type ||
         contract->rows != candidate->rows ||
         contract->dim != candidate->dim ||
         contract->is_matrix != candidate->is_matrix ||
         contract->matrix_array_size != candidate->matrix_array_size)) {
      return -1;
    }
    contract = candidate;
  }
  if (!contract) return 0;

  TempVariable expected;
  memset(&expected, 0, sizeof(expected));
  expected.type = contract->type;
  expected.rows = contract->is_matrix ? contract->rows : 1U;
  expected.dim = contract->dim;
  expected.is_matrix = contract->is_matrix != 0U;
  expected.matrix_array_size = contract->matrix_array_size;
  if (variable->type != expected.type || variable->rows != expected.rows ||
      variable->dim != expected.dim ||
      variable->is_matrix != expected.is_matrix ||
      variable->matrix_array_size != expected.matrix_array_size ||
      variable->byte_size != temp_variable_byte_size(&expected) ||
      (variable->is_matrix && variable->row_major)) {
    return -1;
  }
  return 1;
}

/* UnityShaderVariables.cginc is a byte-affecting part of Unity's source
 * compiler contract.  An omitted declaration is sound only when every
 * referenced serialized field is exactly the field supplied by that include.
 * Serialized matrices describe physical DXBC rows; the include declares
 * ordinary (column-major) HLSL matrices, so formatting must transpose the
 * logical value when selecting one physical row. */
static bool adopt_unity_include_cbuffer_layout(HLSLCBufferLayout *layout) {
  if (!layout || !layout->is_unity_builtin || layout->raw_storage ||
      layout->row_struct_storage || layout->variable_count <= 0) {
    return false;
  }
  for (int index = 0; index < layout->variable_count; ++index) {
    TempVariable *variable = &layout->variables[index];
    const BuiltinVariable *builtin = find_unity_include_builtin(
        layout->declaration_name, variable->name);
    if (!builtin || variable->type != builtin->type ||
        variable->dim != builtin->dim ||
        variable->is_matrix != builtin->is_matrix ||
        variable->matrix_array_size != builtin->matrix_array_size ||
        variable->byte_offset != builtin->offset ||
        (variable->is_matrix && variable->rows != builtin->rows)) {
      return false;
    }
    TempVariable expected = *variable;
    expected.rows = builtin->is_matrix ? builtin->rows : variable->rows;
    expected.dim = builtin->dim;
    expected.is_matrix = builtin->is_matrix;
    expected.matrix_array_size = builtin->matrix_array_size;
    expected.byte_offset = builtin->offset;
    expected.byte_size = 0;
    if (variable->byte_size != temp_variable_byte_size(&expected)) {
      return false;
    }
    variable->row_major = false;
  }
  return true;
}

static const TempVariable *find_variable_at_byte(
    const HLSLCBufferLayout *layout, uint32_t byte_offset) {
  const TempVariable *result = NULL;
  for (int index = 0; index < layout->variable_count; ++index) {
    const TempVariable *variable = &layout->variables[index];
    uint64_t end = (uint64_t)variable->byte_offset + variable->byte_size;
    if ((uint64_t)byte_offset < variable->byte_offset ||
        (uint64_t)byte_offset >= end) {
      continue;
    }
    if (result) return NULL;
    result = variable;
  }
  return result;
}

static bool validate_dynamic_cbuffer_operand(const DXBCOperand *operand,
                                             const HLSLCBufferLayout *layout,
                                             unsigned int depth) {
  if (!operand || !layout ||
      depth >= DXBC_MAX_NESTED_OPERAND_TOKENS) return false;
  if (operand->type == OPERAND_TYPE_CONSTANT_BUFFER &&
      operand->register_index == layout->reg && operand->rel_op1) {
    uint64_t byte_offset = (uint64_t)(uint32_t)operand->rel_offset0 * 16u;
    if (byte_offset > UINT32_MAX) return false;
    const TempVariable *variable =
        find_variable_at_byte(layout, (uint32_t)byte_offset);
    if (!variable || variable->matrix_array_size == 0 ||
        (!variable->is_matrix &&
         variable->byte_offset != (uint32_t)byte_offset)) {
      return false;
    }
  }
  const DXBCOperand *relative_operands[3] = {
      operand->rel_op0, operand->rel_op1, operand->rel_op2};
  for (size_t index = 0; index < 3; ++index) {
    if (relative_operands[index] &&
        !validate_dynamic_cbuffer_operand(relative_operands[index], layout,
                                          depth + 1)) {
      return false;
    }
  }
  return true;
}

static bool validate_dynamic_cbuffer_accesses(
    const USILProgram *program, const HLSLCBufferLayout *layout) {
  for (int instruction = 0; instruction < program->instruction_count;
       ++instruction) {
    for (int operand = 0;
         operand < program->instructions[instruction].operand_count;
         ++operand) {
      if (!validate_dynamic_cbuffer_operand(
              &program->instructions[instruction].operands[operand], layout,
              0)) {
        return false;
      }
    }
  }
  return true;
}

typedef struct {
  bool saw_access;
  int relative_register;
  int relative_component;
  int first_access_instruction;
  int last_access_instruction;
  int largest_row_offset;
} RowStructAccessProof;

static bool operand_is_plain_temp_scalar(const DXBCOperand *operand,
                                         int *register_index,
                                         int *component) {
  if (!operand || operand->type != OPERAND_TYPE_TEMP ||
      operand->register_index_dim != 1 ||
      !operand->index_has_immediate[0] ||
      operand->index_value_exceeds_int[0] ||
      operand->index_values[0] != (uint32_t)operand->register_index ||
      operand->index_representations[0] != 0 ||
      operand->swizzle_mode != 2 || operand->swizzle[0] > 3 ||
      operand->has_abs || operand->has_neg || operand->extended_tokens ||
      operand->extended_token_count != 0 || operand->imm_value_count != 0 ||
      operand->immediate_word_count != 0 || operand->rel_op0 ||
      operand->rel_op1 || operand->rel_op2) {
    return false;
  }
  if (register_index) *register_index = operand->register_index;
  if (component) *component = operand->swizzle[0];
  return true;
}

static bool operand_is_plain_temp_destination_lane(
    const DXBCOperand *operand, int register_index, int component) {
  return operand && operand->type == OPERAND_TYPE_TEMP &&
         operand->register_index == register_index && component >= 0 &&
         component < 4 && operand->register_index_dim == 1 &&
         operand->index_representations[0] == 0 &&
         operand->index_has_immediate[0] &&
         !operand->index_value_exceeds_int[0] &&
         operand->index_values[0] == (uint32_t)register_index &&
         operand->swizzle_mode == 0 &&
         operand->destination_mask == (uint8_t)(16 << component) &&
         !operand->has_abs && !operand->has_neg &&
         !operand->extended_tokens && operand->extended_token_count == 0 &&
         operand->imm_value_count == 0 &&
         operand->immediate_word_count == 0 && !operand->rel_op0 &&
         !operand->rel_op1 && !operand->rel_op2;
}

static bool decode_row_struct_access(const DXBCOperand *operand,
                                     int cbuffer_register,
                                     int *relative_register,
                                     int *relative_component,
                                     int *row_offset) {
  if (!operand || operand->type != OPERAND_TYPE_CONSTANT_BUFFER ||
      operand->register_index != cbuffer_register) {
    return false;
  }
  bool relative_only_row =
      operand->index_representations[1] == 2 &&
      !operand->index_has_immediate[1] &&
      !operand->index_value_exceeds_int[1] &&
      operand->index_values[1] == 0 && operand->rel_offset0 == 0;
  bool immediate_plus_relative_row =
      operand->index_representations[1] == 3 &&
      operand->index_has_immediate[1] &&
      !operand->index_value_exceeds_int[1] &&
      operand->index_values[1] == (uint32_t)operand->rel_offset0;
  int decoded_register = -1;
  int decoded_component = -1;
  if (operand->register_index_dim != 2 || operand->rel_op0 ||
      !operand->rel_op1 || operand->rel_op2 ||
      operand->index_representations[0] != 0 ||
      !operand->index_has_immediate[0] ||
      operand->index_value_exceeds_int[0] ||
      operand->index_values[0] != (uint32_t)cbuffer_register ||
      (!relative_only_row && !immediate_plus_relative_row) ||
      operand->rel_offset0 < 0 ||
      !operand_is_plain_temp_scalar(operand->rel_op1, &decoded_register,
                                    &decoded_component)) {
    return false;
  }
  if (relative_register) *relative_register = decoded_register;
  if (relative_component) *relative_component = decoded_component;
  if (row_offset) *row_offset = operand->rel_offset0;
  return true;
}

static bool collect_row_struct_access_operand(
    const DXBCOperand *operand, int cbuffer_register, int instruction_index,
    RowStructAccessProof *proof, unsigned int depth) {
  if (!operand || !proof || depth >= DXBC_MAX_NESTED_OPERAND_TOKENS)
    return false;

  if (operand->type == OPERAND_TYPE_CONSTANT_BUFFER &&
      operand->register_index == cbuffer_register) {
    int relative_register = -1;
    int relative_component = -1;
    int row_offset = -1;
    if (!decode_row_struct_access(operand, cbuffer_register,
                                  &relative_register, &relative_component,
                                  &row_offset)) {
      return false;
    }
    if (proof->saw_access &&
        (proof->relative_register != relative_register ||
         proof->relative_component != relative_component)) {
      return false;
    }
    if (!proof->saw_access) {
      proof->saw_access = true;
      proof->relative_register = relative_register;
      proof->relative_component = relative_component;
      proof->first_access_instruction = instruction_index;
      proof->last_access_instruction = instruction_index;
      proof->largest_row_offset = row_offset;
    } else {
      proof->last_access_instruction = instruction_index;
      if (row_offset > proof->largest_row_offset)
        proof->largest_row_offset = row_offset;
    }
  }

  const DXBCOperand *relative_operands[3] = {
      operand->rel_op0, operand->rel_op1, operand->rel_op2};
  for (size_t index = 0; index < 3; ++index) {
    if (relative_operands[index] &&
        !collect_row_struct_access_operand(relative_operands[index],
                                           cbuffer_register,
                                           instruction_index, proof,
                                           depth + 1)) {
      return false;
    }
  }
  return true;
}

static bool instruction_writes_temp_lane(const USILProgram *program,
                                         const USILInstruction *instruction,
                                         int register_index, int component,
                                         bool *writes) {
  if (writes) *writes = false;
  if (!program || !instruction || !writes || component < 0 ||
      component >= 4 ||
      !usil_instruction_shape_valid(program, instruction)) {
    return false;
  }
  for (int operand_index = 0;
       operand_index < instruction->operand_count; ++operand_index) {
    USILOperandUseInfo use;
    if (!usil_instruction_operand_use(program, instruction, operand_index,
                                      &use)) {
      return false;
    }
    if (use.use != USIL_OPERAND_USE_DESTINATION) continue;
    const DXBCOperand *destination =
        &instruction->operands[operand_index];
    if (destination->type == OPERAND_TYPE_TEMP &&
        destination->register_index == register_index &&
        (destination->destination_mask & (16 << component)) != 0) {
      *writes = true;
    }
  }
  return true;
}

static bool source_operand_reads_temp_lane(const DXBCOperand *operand,
                                           uint8_t logical_lane_mask,
                                           int register_index,
                                           int component) {
  if (!operand || operand->type != OPERAND_TYPE_TEMP ||
      operand->register_index != register_index ||
      logical_lane_mask == 0 || component < 0 || component >= 4) {
    return false;
  }
  for (int logical_lane = 0; logical_lane < 4; ++logical_lane) {
    if ((logical_lane_mask & (uint8_t)(1u << logical_lane)) == 0) continue;
    int physical_lane = logical_lane;
    if (operand->swizzle_mode == 2) {
      physical_lane = operand->swizzle[0];
    } else if (operand->swizzle_mode == 1) {
      physical_lane = operand->swizzle[logical_lane];
    }
    if (physical_lane == component) return true;
  }
  return false;
}

/* The row scale is removable only if its result has no value use other than
 * the exact relative-index child of this cbuffer.  Merely finding a scale
 * before the first access is insufficient: an independent arithmetic read
 * would make replacing the scale change executable semantics. */
static bool operand_tree_has_only_row_index_use(
    const DXBCOperand *operand, bool operand_is_source,
    uint8_t logical_lane_mask, int cbuffer_register, int register_index,
    int component, bool allow_exact_index_scalar, unsigned int depth) {
  if (!operand || depth >= DXBC_MAX_NESTED_OPERAND_TOKENS) return false;
  if (operand_is_source &&
      source_operand_reads_temp_lane(operand, logical_lane_mask,
                                     register_index, component)) {
    int decoded_register = -1;
    int decoded_component = -1;
    if (!allow_exact_index_scalar ||
        !operand_is_plain_temp_scalar(operand, &decoded_register,
                                      &decoded_component) ||
        decoded_register != register_index ||
        decoded_component != component) {
      return false;
    }
  }

  int access_register = -1;
  int access_component = -1;
  const bool matching_access =
      operand->type == OPERAND_TYPE_CONSTANT_BUFFER &&
      operand->register_index == cbuffer_register &&
      decode_row_struct_access(operand, cbuffer_register, &access_register,
                               &access_component, NULL) &&
      access_register == register_index && access_component == component;
  const DXBCOperand *relative_operands[3] = {
      operand->rel_op0, operand->rel_op1, operand->rel_op2};
  for (size_t relative_index = 0; relative_index < 3; ++relative_index) {
    const DXBCOperand *relative = relative_operands[relative_index];
    if (!relative) continue;
    const bool allow_child = matching_access && relative_index == 1;
    if (!operand_tree_has_only_row_index_use(
            relative, true, 1u, cbuffer_register, register_index, component,
            allow_child, depth + 1)) {
      return false;
    }
  }
  return true;
}

static bool instruction_has_only_row_index_use(
    const USILProgram *program, const USILInstruction *instruction,
    int cbuffer_register, int register_index, int component) {
  if (!program || !instruction ||
      !usil_instruction_shape_valid(program, instruction)) {
    return false;
  }
  for (int operand_index = 0;
       operand_index < instruction->operand_count; ++operand_index) {
    USILOperandUseInfo use;
    if (!usil_instruction_operand_use(program, instruction, operand_index,
                                      &use)) {
      return false;
    }
    if (!operand_tree_has_only_row_index_use(
            &instruction->operands[operand_index],
            use.use == USIL_OPERAND_USE_SOURCE, use.source_lane_mask,
            cbuffer_register, register_index, component, false, 0)) {
      return false;
    }
  }
  return true;
}

enum {
  ROW_STRUCT_REACHES_SCALED_VALUE = 1u,
  ROW_STRUCT_REACHES_OTHER_VALUE = 2u
};

static bool instruction_has_row_struct_access(
    const USILInstruction *instruction, int cbuffer_register,
    int register_index, int component, bool *has_access) {
  if (has_access) *has_access = false;
  if (!instruction || !has_access) return false;
  RowStructAccessProof proof;
  memset(&proof, 0, sizeof(proof));
  proof.relative_register = -1;
  proof.relative_component = -1;
  proof.first_access_instruction = -1;
  proof.last_access_instruction = -1;
  proof.largest_row_offset = -1;
  for (int operand_index = 0;
       operand_index < instruction->operand_count; ++operand_index) {
    if (!collect_row_struct_access_operand(
            &instruction->operands[operand_index], cbuffer_register, 0,
            &proof, 0)) {
      return false;
    }
  }
  if (!proof.saw_access) return true;
  if (proof.relative_register != register_index ||
      proof.relative_component != component) {
    return false;
  }
  *has_access = true;
  return true;
}

static bool row_struct_block_dominates(
    const HLSLControlFlowGraph *cfg, int dominator, int block) {
  if (!cfg || !cfg->idom || dominator < 0 || block < 0 ||
      dominator >= cfg->block_count || block >= cfg->block_count) {
    return false;
  }
  for (int steps = 0; steps <= cfg->block_count; ++steps) {
    if (block == dominator) return true;
    const int parent = cfg->idom[block];
    if (parent < 0 || parent >= cfg->block_count || parent == block) {
      return false;
    }
    block = parent;
  }
  return false;
}

/* Replacing the scale is semantics-preserving only while its old flattened
 * row value reaches cbuffer indexing and no other read.  Track both possible
 * reaching classes through the already-validated CFG: a write changes every
 * incoming path to OTHER, and merges union the two bits.  Thus a read after a
 * branch-local kill is accepted inside that branch, while a post-merge read
 * or cbuffer access fails whenever even one predecessor still carries the
 * old scaled value or one carries a replacement. */
static bool prove_row_struct_scale_liveness(
    const HLSLEmitterContext *ctx, int cbuffer_register,
    int register_index, int component, int scale_instruction_index) {
  if (!ctx || !ctx->program || !ctx->cfg.blocks ||
      !ctx->cfg.instruction_block || ctx->cfg.block_count <= 0 ||
      scale_instruction_index < 0 ||
      scale_instruction_index >= ctx->program->instruction_count) {
    return false;
  }
  const USILProgram *program = ctx->program;
  const int scale_block =
      ctx->cfg.instruction_block[scale_instruction_index];
  if (scale_block < 0 || scale_block >= ctx->cfg.block_count ||
      ctx->cfg.block_count > program->instruction_count) {
    return false;
  }
  /* Every executable row access must be downstream of this exact scale.
   * This admits a scale inside one branch while rejecting a syntactically
   * nearby definition that does not dominate all uses. */
  for (int instruction_index = 0;
       instruction_index < program->instruction_count;
       ++instruction_index) {
    bool has_access = false;
    if (!instruction_has_row_struct_access(
            &program->instructions[instruction_index], cbuffer_register,
            register_index, component, &has_access)) {
      return false;
    }
    if (!has_access) continue;
    const int access_block =
        ctx->cfg.instruction_block[instruction_index];
    if (access_block < 0 || access_block >= ctx->cfg.block_count ||
        (access_block == scale_block &&
         instruction_index <= scale_instruction_index) ||
        (access_block != scale_block &&
         !row_struct_block_dominates(&ctx->cfg, scale_block,
                                     access_block))) {
      return false;
    }
  }
  uint8_t *entry_state =
      (uint8_t *)calloc((size_t)ctx->cfg.block_count, sizeof(uint8_t));
  if (!entry_state) return false;
  entry_state[scale_block] = ROW_STRUCT_REACHES_SCALED_VALUE;

  bool valid = true;
  bool changed = true;
  int64_t iterations = 0;
  const int64_t iteration_limit =
      (int64_t)ctx->cfg.block_count * 2 + 1;
  while (changed && valid) {
    changed = false;
    if (++iterations > iteration_limit) {
      valid = false;
      break;
    }
    for (int block_index = 0;
         block_index < ctx->cfg.block_count && valid; ++block_index) {
      uint8_t state = entry_state[block_index];
      if (state == 0U) continue;
      const HLSLBasicBlock *block = &ctx->cfg.blocks[block_index];
      if (block->has_ambiguous_flow || block->first_instruction < 0 ||
          block->last_instruction < block->first_instruction ||
          block->last_instruction >= program->instruction_count) {
        valid = false;
        break;
      }
      int first_instruction = block->first_instruction;
      if (block_index == scale_block) {
        if (scale_instruction_index < first_instruction ||
            scale_instruction_index > block->last_instruction) {
          valid = false;
          break;
        }
        first_instruction = scale_instruction_index + 1;
      } else if (block->last_instruction <= scale_instruction_index) {
        valid = false;
        break;
      }

      for (int instruction_index = first_instruction;
           instruction_index <= block->last_instruction; ++instruction_index) {
        const USILInstruction *instruction =
            &program->instructions[instruction_index];
        bool has_access = false;
        if (!instruction_has_row_struct_access(
                instruction, cbuffer_register, register_index, component,
                &has_access)) {
          valid = false;
          break;
        }
        if ((state & ROW_STRUCT_REACHES_OTHER_VALUE) != 0U && has_access) {
          valid = false;
          break;
        }
        if ((state & ROW_STRUCT_REACHES_SCALED_VALUE) != 0U &&
            !instruction_has_only_row_index_use(
                program, instruction, cbuffer_register, register_index,
                component)) {
          valid = false;
          break;
        }
        bool writes = false;
        if (!instruction_writes_temp_lane(
                program, instruction, register_index, component, &writes)) {
          valid = false;
          break;
        }
        if (writes) state = ROW_STRUCT_REACHES_OTHER_VALUE;
      }
      if (!valid) break;

      for (int successor_index = 0;
           successor_index < block->successor_count; ++successor_index) {
        const int successor = block->successors[successor_index];
        if (successor < 0 || successor >= ctx->cfg.block_count ||
            successor == scale_block) {
          valid = false;
          break;
        }
        const uint8_t merged =
            (uint8_t)(entry_state[successor] | state);
        if (merged != entry_state[successor]) {
          entry_state[successor] = merged;
          changed = true;
        }
      }
    }
  }
  free(entry_state);
  return valid;
}

static bool immediate_scalar_u32(const DXBCOperand *operand,
                                 uint32_t *value) {
  if (!operand || !value || operand->type != OPERAND_TYPE_IMMEDIATE32 ||
      operand->imm_value_count != 1 || operand->immediate_word_count != 1 ||
      operand->imm_values[0] != operand->immediate_words[0] ||
      operand->register_index_dim != 0 ||
      operand->has_abs || operand->has_neg || operand->extended_tokens ||
      operand->extended_token_count != 0 || operand->rel_op0 ||
      operand->rel_op1 || operand->rel_op2) {
    return false;
  }
  *value = operand->immediate_words[0];
  return true;
}

static bool decode_row_struct_scale(const USILInstruction *instruction,
                                    int destination_register,
                                    int destination_component,
                                    const DXBCOperand **index_source,
                                    uint32_t *stride) {
  if (index_source) *index_source = NULL;
  if (stride) *stride = 0;
  if (!instruction || !index_source || !stride || instruction->saturate ||
      instruction->precise_mask != 0) {
    return false;
  }

  if (instruction->opcode == USIL_OP_ISHL &&
      instruction->operand_count == 3) {
    int source_register = -1;
    int source_component = -1;
    uint32_t shift_amount = 0;
    if (!operand_is_plain_temp_destination_lane(
            &instruction->operands[0], destination_register,
            destination_component) ||
        !operand_is_plain_temp_scalar(&instruction->operands[1],
                                      &source_register,
                                      &source_component) ||
        !immediate_scalar_u32(&instruction->operands[2], &shift_amount) ||
        shift_amount == 0 || shift_amount >= 31) {
      return false;
    }
    *index_source = &instruction->operands[1];
    *stride = UINT32_C(1) << shift_amount;
    return true;
  }

  if (instruction->opcode == USIL_OP_IMUL &&
      instruction->operand_count == 4 &&
      instruction->operands[0].type == OPERAND_TYPE_NULL &&
      operand_is_plain_temp_destination_lane(
          &instruction->operands[1], destination_register,
          destination_component)) {
    const DXBCOperand *source = NULL;
    uint32_t multiplier = 0;
    int source_register = -1;
    int source_component = -1;
    if (operand_is_plain_temp_scalar(&instruction->operands[2],
                                     &source_register, &source_component) &&
        immediate_scalar_u32(&instruction->operands[3], &multiplier)) {
      source = &instruction->operands[2];
    } else if (operand_is_plain_temp_scalar(&instruction->operands[3],
                                            &source_register,
                                            &source_component) &&
               immediate_scalar_u32(&instruction->operands[2],
                                    &multiplier)) {
      source = &instruction->operands[3];
    } else {
      return false;
    }
    if (multiplier <= 1 || multiplier > INT_MAX) return false;
    *index_source = source;
    *stride = multiplier;
    return true;
  }
  return false;
}

static bool prove_row_struct_storage(const HLSLEmitterContext *ctx,
                                     HLSLCBufferLayout *layout,
                                     const SerializedStructParam *parameter) {
  if (!ctx || !ctx->program || !layout || layout->row_count <= 0 ||
      layout->variable_count != 0) {
    return false;
  }
  const USILProgram *program = ctx->program;

  RowStructAccessProof proof;
  memset(&proof, 0, sizeof(proof));
  proof.relative_register = -1;
  proof.relative_component = -1;
  proof.first_access_instruction = -1;
  proof.last_access_instruction = -1;
  proof.largest_row_offset = -1;
  for (int instruction_index = 0;
       instruction_index < program->instruction_count;
       ++instruction_index) {
    const USILInstruction *instruction =
        &program->instructions[instruction_index];
    for (int operand_index = 0;
         operand_index < instruction->operand_count; ++operand_index) {
      if (!collect_row_struct_access_operand(
              &instruction->operands[operand_index], layout->reg,
              instruction_index, &proof, 0)) {
        return false;
      }
    }
  }
  if (!proof.saw_access || proof.first_access_instruction < 0 ||
      proof.last_access_instruction < proof.first_access_instruction) {
    return false;
  }

  /* A one-row serialized element needs no arithmetic inverse: the DXBC row
   * index is already the HLSL array index.  The full serialized shell and
   * the trimmed-final-element identity still have to agree exactly; member
   * packing is validated by adopt_serialized_row_struct_topology below. */
  if (parameter && layout->has_reflection_size_authority &&
      parameter->layout[0] == 0U && parameter->layout[1] >= 2U &&
      parameter->layout[1] <= INT_MAX && parameter->layout[2] == 16U &&
      parameter->layout[1] <= UINT32_MAX / parameter->layout[2] &&
      layout->reflection_size_bytes ==
          parameter->layout[1] * parameter->layout[2] &&
      proof.largest_row_offset == 0 &&
      (uint32_t)layout->row_count == parameter->layout[1]) {
    layout->row_struct_storage = true;
    layout->row_struct_stride = 1;
    layout->row_struct_elements = (int)parameter->layout[1];
    layout->row_struct_scale_instruction = -1;
    layout->row_struct_index_source = NULL;
    return true;
  }
  if (proof.first_access_instruction == 0) return false;

  int scale_instruction_index = -1;
  for (int instruction_index = proof.first_access_instruction - 1;
       instruction_index >= 0; --instruction_index) {
    bool writes = false;
    if (!instruction_writes_temp_lane(
            program, &program->instructions[instruction_index],
            proof.relative_register, proof.relative_component, &writes)) {
      return false;
    }
    if (writes) {
      scale_instruction_index = instruction_index;
      break;
    }
  }
  if (scale_instruction_index < 0) return false;
  const DXBCOperand *index_source = NULL;
  uint32_t stride = 0;
  if (!decode_row_struct_scale(
          &program->instructions[scale_instruction_index],
          proof.relative_register, proof.relative_component, &index_source,
          &stride)) {
    return false;
  }
  if (stride > (uint32_t)layout->row_count ||
      proof.largest_row_offset >= (int)stride) {
    return false;
  }
  /* Depending on the source/reflection path, D3DCompiler either retains the
   * complete serialized array shell or trims only the unused tail of its
   * final element.  Serialized authority makes both extents exact: accept
   * precisely count*stride rows or
   * (count-1)*stride+highest-referenced-row+1, never an intermediate shell.
   * Without that authority, only the trimmed identity determines a unique
   * element count from executable evidence. */
  uint32_t elements = 0U;
  if (parameter && layout->has_reflection_size_authority &&
      parameter->layout[0] == 0U && parameter->layout[1] >= 2U &&
      parameter->layout[1] <= INT_MAX &&
      stride <= UINT32_MAX / 16U &&
      parameter->layout[2] == stride * 16U &&
      parameter->layout[1] <= UINT32_MAX / stride) {
    elements = parameter->layout[1];
    const uint32_t full_rows = elements * stride;
    const uint32_t trimmed_rows =
        (elements - 1U) * stride +
        (uint32_t)proof.largest_row_offset + 1U;
    if (full_rows > UINT32_MAX / 16U ||
        layout->reflection_size_bytes != full_rows * 16U ||
        ((uint32_t)layout->row_count != full_rows &&
         (uint32_t)layout->row_count != trimmed_rows)) {
      return false;
    }
  } else {
    elements = ((uint32_t)layout->row_count - 1U) / stride + 1U;
    const uint32_t trimmed_rows =
        (elements - 1U) * stride +
        (uint32_t)proof.largest_row_offset + 1U;
    if (elements < 2U || elements > INT_MAX ||
        trimmed_rows != (uint32_t)layout->row_count) {
      return false;
    }
  }

  if (!prove_row_struct_scale_liveness(
          ctx, layout->reg, proof.relative_register,
          proof.relative_component, scale_instruction_index)) {
    return false;
  }

  layout->row_struct_storage = true;
  layout->row_struct_stride = (int)stride;
  layout->row_struct_elements = (int)elements;
  layout->row_struct_scale_instruction = scale_instruction_index;
  layout->row_struct_index_source = index_source;
  return true;
}

static bool collect_serialized_row_struct_candidate(
    const SerializedProgramParameters *parameters, const char *cbuffer_name,
    const SerializedProgramParameters **candidate_parameters,
    const SerializedStructParam **candidate_parameter,
    size_t *candidate_count) {
  if (!candidate_parameters || !candidate_parameter || !candidate_count)
    return false;
  if (!parameters) return true;
  if (parameters->cb_count < 0 ||
      (parameters->cb_count > 0 && !parameters->constant_buffers)) {
    return false;
  }
  if (!cbuffer_name) return true;
  for (int buffer_index = 0; buffer_index < parameters->cb_count;
       ++buffer_index) {
    const SerializedConstantBuffer *buffer =
        &parameters->constant_buffers[buffer_index];
    if (!buffer->name || buffer->struct_count < 0 ||
        (buffer->struct_count > 0 && !buffer->struct_params)) {
      return false;
    }
    if (strcmp(buffer->name, cbuffer_name) != 0) continue;
    for (int struct_index = 0; struct_index < buffer->struct_count;
         ++struct_index) {
      if (*candidate_count != 0U) return false;
      *candidate_parameters = parameters;
      *candidate_parameter = &buffer->struct_params[struct_index];
      ++*candidate_count;
    }
  }
  return true;
}

/* The stripped declaration preserves only flattened 16-byte rows.  A
 * serialized top-level struct can replace the anonymous row inverse only
 * when its complete natural HLSL layout is independently identical: one
 * zero-based array fills the reflection shell, its element count/stride are
 * the values proven from the executable row-address graph, and its members
 * tile that stride in serialized callback order.  Restrict members to the
 * two source-default shapes whose D3D11 packing and physical row selection
 * are unambiguous here: float4 and float4x4.  Unknown shapes fail closed
 * rather than changing reflection or executable addressing. */
static bool adopt_serialized_row_struct_topology(
    HLSLCBufferLayout *layout,
    const SerializedProgramParameters *parameters,
    const SerializedStructParam *parameter) {
  if (!layout || !parameters || !parameter || !parameter->name ||
      parameter->name[0] == '\0' || parameter->member_count <= 0 ||
      !parameter->members || layout->row_struct_stride <= 0 ||
      layout->row_struct_elements <= 0 || !layout->has_reflection_size_authority ||
      parameter->layout[0] != 0U ||
      parameter->layout[1] != (uint32_t)layout->row_struct_elements ||
      parameter->layout[2] !=
          (uint32_t)layout->row_struct_stride * 16U ||
      parameter->layout[1] > UINT32_MAX / parameter->layout[2] ||
      layout->reflection_size_bytes !=
          parameter->layout[1] * parameter->layout[2]) {
    return false;
  }

  uint32_t cursor = 0U;
  for (int member_index = 0; member_index < parameter->member_count;
       ++member_index) {
    const SerializedVariable *member = &parameter->members[member_index];
    DecodedVariableLayout decoded;
    if (!member->name || member->name[0] == '\0' ||
        !parameter_layout_decode(parameters, member, &decoded) ||
        decoded.byte_offset != cursor || decoded.scalar_type != 0U ||
        decoded.array_size != 0U ||
        !((!decoded.is_matrix && decoded.columns == 4U) ||
          (decoded.is_matrix && decoded.rows == 4U &&
           decoded.columns == 4U))) {
      return false;
    }
    for (int prior = 0; prior < member_index; ++prior) {
      if (!parameter->members[prior].name ||
          strcmp(parameter->members[prior].name, member->name) == 0) {
        return false;
      }
    }
    const uint32_t byte_size = parameter_layout_byte_size(&decoded);
    if (byte_size == 0U || byte_size > parameter->layout[2] ||
        cursor > parameter->layout[2] - byte_size) {
      return false;
    }
    cursor += byte_size;
  }
  if (cursor != parameter->layout[2]) return false;

  layout->row_struct_parameters = parameters;
  layout->row_struct_parameter = parameter;
  return true;
}

const HLSLCBufferLayout *get_cbuffer_emission_layout(
    const HLSLEmitterContext *ctx, int reg) {
  if (!ctx || !ctx->cbuffer_layouts_built) return NULL;
  for (int index = 0; index < ctx->cbuffer_layout_count; ++index) {
    if (ctx->cbuffer_layouts[index].reg == reg) {
      return &ctx->cbuffer_layouts[index];
    }
  }
  return NULL;
}

void free_cbuffer_emission_layouts(HLSLEmitterContext *ctx) {
  if (!ctx) return;
  for (int index = 0; index < HLSL_MAX_CBUFFER_LAYOUTS; ++index) {
    free(ctx->cbuffer_layouts[index].declaration_name);
    free(ctx->cbuffer_layouts[index].variables);
    free(ctx->cbuffer_layouts[index].uses);
    memset(&ctx->cbuffer_layouts[index], 0,
           sizeof(ctx->cbuffer_layouts[index]));
  }
  ctx->cbuffer_layout_count = 0;
  ctx->cbuffer_layouts_built = false;
}

bool build_cbuffer_emission_layouts(HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->program || ctx->program->cbuffer_count < 0 ||
      ctx->program->cbuffer_count > HLSL_MAX_CBUFFER_LAYOUTS ||
      (ctx->program->cbuffer_count > 0 && !ctx->program->cbuffers)) {
    return false;
  }
  free_cbuffer_emission_layouts(ctx);
  ctx->cbuffer_layout_count = ctx->program->cbuffer_count;
  ctx->cbuffer_layouts_built = false;

  for (int index = 0; index < ctx->program->cbuffer_count; ++index) {
    HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[index];
    layout->row_struct_scale_instruction = -1;
    layout->reg = ctx->program->cbuffers[index].reg_idx;
    layout->row_count = ctx->program->cbuffers[index].size;
    if (layout->row_count <= 0 || layout->row_count > 4096) return false;
    layout->reflection_size_bytes = (uint32_t)layout->row_count * 16U;
    for (int previous = 0; previous < index; ++previous) {
      if (ctx->cbuffer_layouts[previous].reg == layout->reg) return false;
    }
    layout->serialized_name = get_cbuffer_name_from_map(ctx, layout->reg);
    layout->declaration_name = allocate_cbuffer_declaration_name(
        layout->serialized_name, layout->reg);
    if (!layout->declaration_name) return false;
    layout->is_globals = layout->serialized_name &&
                         strcmp(layout->declaration_name, "_Globals") == 0;
    layout->is_unity_builtin =
        is_unity_builtin_cbuffer(layout->declaration_name);

    if (layout->serialized_name &&
        (!append_serialized_cbuffer_source(
             layout, ctx->params, layout->serialized_name,
             CBUFFER_AUTHORITY_STAGE) ||
         !append_serialized_cbuffer_source(
             layout, ctx->common_params, layout->serialized_name,
             CBUFFER_AUTHORITY_COMMON))) {
      fail_cbuffer_location(
          ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
          HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
          HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY, index, -1);
      return false;
    }
    if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE &&
        layout->variable_count == 0 &&
        !append_readable_builtin_fallback(layout,
                                          layout->serialized_name)) {
      return false;
    }

    DXBCCBufferVariableRange *ranges = NULL;
    if (layout->variable_count > 0) {
      if (dxbc_size_multiply_overflows(
              (size_t)layout->variable_count, sizeof(*ranges)) ||
          dxbc_size_multiply_overflows(
              (size_t)layout->variable_count, sizeof(*layout->uses))) {
        return false;
      }
      ranges = malloc((size_t)layout->variable_count * sizeof(*ranges));
      layout->uses = calloc((size_t)layout->variable_count,
                            sizeof(*layout->uses));
      if (!ranges || !layout->uses) {
        free(ranges);
        return false;
      }
      layout->use_alloc = layout->variable_count;
    }
    for (int variable = 0; variable < layout->variable_count; ++variable) {
      ranges[variable].byte_offset = layout->variables[variable].byte_offset;
      ranges[variable].byte_size = layout->variables[variable].byte_size;
    }
    if (!dxbc_cbuffer_projection_reset(&layout->projection, layout->uses,
                                       (size_t)layout->variable_count)) {
      free(ranges);
      return false;
    }
    layout->projection_status = dxbc_cbuffer_project_program(
        ctx->program, (uint32_t)layout->reg,
        (uint32_t)layout->row_count * 16u, ranges,
        (size_t)layout->variable_count, layout->uses,
        &layout->projection);
    free(ranges);
    if (layout->projection_status == DXBC_CBUFFER_PROJECTION_INVALID) {
      fail_cbuffer_location(
          ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
          HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
          HLSL_EMIT_REASON_INVALID_PARAMETER_LAYOUT, index, -1);
      return false;
    }

    if (layout->projection_status ==
        DXBC_CBUFFER_PROJECTION_REQUIRES_INDEX_AUTHORITY) {
      if (ctx->emit_mode == HLSL_EMIT_MODE_RECOMPILE &&
          !layout->has_serialized_authority) {
        /* A stripped program can retain a named cbuffer resource while its
         * serialized variable table is empty (Unity's instancing buffers are
         * a common example).  In that case the DXBC declaration still gives
         * exact physical authority: N float4 rows at the declared bind slot,
         * and the operand supplies the original relative row expression.
         * Preserve that representation directly instead of inventing a
         * source-level array or rejecting an otherwise lossless lowering. */
        if (layout->variable_count != 0) {
          fail_cbuffer_location(
              ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
              HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
              HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY, index, -1);
          return false;
        }
        const SerializedProgramParameters *struct_parameters = NULL;
        const SerializedStructParam *struct_parameter = NULL;
        size_t struct_candidate_count = 0U;
        if (!collect_serialized_row_struct_candidate(
                ctx->params, layout->serialized_name, &struct_parameters,
                &struct_parameter, &struct_candidate_count) ||
            !collect_serialized_row_struct_candidate(
                ctx->common_params, layout->serialized_name,
                &struct_parameters, &struct_parameter,
                &struct_candidate_count)) {
          fail_cbuffer_location(
              ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
              HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
              HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY, index, -1);
          return false;
        }
        if (!prove_row_struct_storage(ctx, layout, struct_parameter)) {
          /* Serialized struct reflection is identity-bearing authority.  A
           * flat float4 fallback could preserve executable rows while
           * necessarily reporting different constant names/types. */
          if (struct_candidate_count != 0U) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
                HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, index, -1);
            return false;
          }
          layout->raw_storage = true;
        } else {
          if (struct_candidate_count != 0U &&
              !adopt_serialized_row_struct_topology(
                  layout, struct_parameters, struct_parameter)) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
                HLSL_EMIT_REASON_INVALID_PARAMETER_LAYOUT, index, -1);
            return false;
          }
          /* The flattened DXBC declaration may trim the unused tail of its
           * final source struct element. Reflection still reports the full
           * source declaration shell. Infer that shell only when serialized
           * metadata did not already provide an authoritative size. */
          const uint32_t source_struct_size =
              (uint32_t)layout->row_struct_stride *
              (uint32_t)layout->row_struct_elements * 16U;
          if (layout->has_reflection_size_authority &&
              layout->reflection_size_bytes < source_struct_size) {
            /* The inferred struct would change an exact serialized source
             * shell. Keep the physically authoritative flat DXBC rows. */
            layout->row_struct_storage = false;
            layout->row_struct_stride = 0;
            layout->row_struct_elements = 0;
            layout->row_struct_scale_instruction = -1;
            layout->row_struct_index_source = NULL;
            layout->raw_storage = true;
          } else {
            if (!layout->has_reflection_size_authority) {
              layout->reflection_size_bytes = source_struct_size;
            }
          }
        }
      } else {
        /* Without a serialized value range, keep the full authoritative
         * layout and require the relative operand to name a real array/matrix
         * base.  This preserves declaration and helper indexing together. */
        if (!validate_dynamic_cbuffer_accesses(ctx->program, layout)) {
          fail_cbuffer_location(
              ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
              HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
              HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, index, -1);
          return false;
        }
        if (ctx->emit_mode == HLSL_EMIT_MODE_RECOMPILE &&
            layout->projection.saw_static_padding_access) {
          return false;
        }
      }
    } else if (layout->projection.saw_static_padding_access) {
      /* A statically addressed byte not covered by metadata has only one
       * exact representation: the flat DXBC register array. */
      layout->raw_storage = true;
      layout->variable_count = 0;
      if (layout->use_alloc > 0) {
        memset(layout->uses, 0,
               (size_t)layout->use_alloc * sizeof(*layout->uses));
      }
    } else if (layout->projection.saw_access) {
      int output = 0;
      for (int variable = 0; variable < layout->variable_count; ++variable) {
        if (!layout->uses[variable].referenced) continue;
        if (output != variable) {
          layout->variables[output] = layout->variables[variable];
          layout->uses[output] = layout->uses[variable];
        }
        output++;
      }
      layout->variable_count = output;
    }

    layout->omit_declaration =
        layout->is_unity_builtin && ctx->omit_unity_builtin_declarations;
    if (layout->omit_declaration &&
        !adopt_unity_include_cbuffer_layout(layout)) {
      fail_cbuffer_location(
          ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
          HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
          HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY, index, -1);
      return false;
    }

    for (int left = 0; left + 1 < layout->variable_count; ++left) {
      for (int right = left + 1; right < layout->variable_count; ++right) {
        if (layout->variables[left].byte_offset >
            layout->variables[right].byte_offset) {
          TempVariable variable = layout->variables[left];
          layout->variables[left] = layout->variables[right];
          layout->variables[right] = variable;
          DXBCCBufferVariableUse use = layout->uses[left];
          layout->uses[left] = layout->uses[right];
          layout->uses[right] = use;
        }
      }
    }
  }
  ctx->cbuffer_layouts_built = true;
  return true;
}

void emit_comments_and_icb(HLSLEmitterContext* ctx) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;

  // 1. Emit Comments
  sb_append(sb, "// Translated by Codex C-emitter\n");
  sb_append(sb, "// Model: ");
  sb_append(sb, program->shader_type_model);
  sb_append(sb, "\n\n");

  if (program->icb_value_count > 0) {
    sb_append(sb, "static const float4 unk0_arr[] = {\n");
    int vec_count = program->icb_value_count / 4;
    for (int v = 0; v < vec_count; v++) {
      char v0[64], v1[64], v2[64], v3[64];
      if (!format_float_bits_hlsl(program->icb_values[v * 4 + 0], v0,
                                  sizeof(v0)) ||
          !format_float_bits_hlsl(program->icb_values[v * 4 + 1], v1,
                                  sizeof(v1)) ||
          !format_float_bits_hlsl(program->icb_values[v * 4 + 2], v2,
                                  sizeof(v2)) ||
          !format_float_bits_hlsl(program->icb_values[v * 4 + 3], v3,
                                  sizeof(v3))) {
        hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                       HLSL_EMIT_PHASE_ICB_EMISSION,
                       HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW);
        return;
      }
      sb_appendf(sb, "    float4(%s, %s, %s, %s)%s\n", v0, v1, v2, v3,
                 (v == vec_count - 1) ? "" : ",");
    }
    sb_append(sb, "};\n\n");
  }
  if (ctx->indexed_face_basis.valid) {
    sb_append(sb,
              "static const float3 dxbc_face_u[6] = {\n"
              "    float3(0, 0, -1), float3(0, 0, 1),\n"
              "    float3(1, 0, 0), float3(1, 0, 0),\n"
              "    float3(1, 0, 0), float3(-1, 0, 0)\n"
              "};\n"
              "static const float3 dxbc_face_v[6] = {\n"
              "    float3(0, -1, 0), float3(0, -1, 0),\n"
              "    float3(0, 0, 1), float3(0, 0, -1),\n"
              "    float3(0, -1, 0), float3(0, -1, 0)\n"
              "};\n\n");
  }
}

void emit_cbuffers(HLSLEmitterContext* ctx) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  if (!ctx->cbuffer_layouts_built) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                   HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                   HLSL_EMIT_REASON_INTERNAL_INVARIANT);
    return;
  }

  for (int i = 0; i < program->cbuffer_count; i++) {
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[i];
    int reg = layout->reg;
    if (layout->omit_declaration) continue;

    bool use_packoffset = !layout->is_unity_builtin &&
                          layout->variable_count > 0 &&
                          !layout->is_globals;
    if (!layout->is_globals) {
      sb_appendf(sb, "cbuffer %s : register(b%d) {\n",
                 layout->declaration_name, reg);
    }
    if (layout->row_struct_storage) {
      sb_append(sb, "    struct {\n");
      if (layout->row_struct_parameter) {
        if (!layout->row_struct_parameters ||
            layout->row_struct_parameter->member_count <= 0 ||
            !layout->row_struct_parameter->members) {
          fail_cbuffer_location(
              ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
              HLSL_EMIT_PHASE_CBUFFER_EMISSION,
              HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, i, -1);
          return;
        }
        for (int member_index = 0;
             member_index < layout->row_struct_parameter->member_count;
             ++member_index) {
          const SerializedVariable *member =
              &layout->row_struct_parameter->members[member_index];
          DecodedVariableLayout decoded;
          if (!member->name ||
              !parameter_layout_decode(layout->row_struct_parameters,
                                       member, &decoded)) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, i, -1);
            return;
          }
          if (decoded.is_matrix) {
            sb_appendf(sb, "        float%ux%u %s;\n", decoded.rows,
                       decoded.columns, member->name);
          } else {
            sb_appendf(sb, "        float%u %s;\n", decoded.columns,
                       member->name);
          }
        }
        sb_appendf(sb, "    } %s[%d];\n",
                   layout->row_struct_parameter->name,
                   layout->row_struct_elements);
      } else {
        for (int row = 0; row < layout->row_struct_stride; ++row)
          sb_appendf(sb, "        float4 row%d;\n", row);
        sb_appendf(sb, "    } cb%d_rows[%d];\n", reg,
                   layout->row_struct_elements);
      }
      const uint32_t declared_size =
          (uint32_t)layout->row_struct_stride *
          (uint32_t)layout->row_struct_elements * 16U;
      if (layout->reflection_size_bytes < declared_size) {
        fail_cbuffer_location(
            ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
            HLSL_EMIT_PHASE_CBUFFER_EMISSION,
            HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, i, -1);
        return;
      }
      if (layout->reflection_size_bytes > declared_size) {
        sb_appendf(sb, "    float4 dxbc_reflection_tail_cb%d[%u];\n", reg,
                   (layout->reflection_size_bytes - declared_size) / 16U);
      }
      if (!layout->is_globals) sb_append(sb, "};\n\n");
      else sb_append(sb, "\n");
      continue;
    }
    if (layout->raw_storage) {
      sb_appendf(sb, "    float4 cb%d_data[%d];\n", reg, layout->row_count);
      const uint32_t active_size = (uint32_t)layout->row_count * 16U;
      if (layout->reflection_size_bytes > active_size) {
        sb_appendf(sb, "    float4 dxbc_reflection_tail_cb%d[%u];\n", reg,
                   (layout->reflection_size_bytes - active_size) / 16U);
      }
      if (!layout->is_globals) sb_append(sb, "};\n\n");
      else sb_append(sb, "\n");
      continue;
    }

    uint32_t current_byte_offset = 0;
    for (int k = 0; k < layout->variable_count; k++) {
        const TempVariable *var = &layout->variables[k];
        uint32_t target_byte_offset = var->byte_offset;

        if (ctx->omit_unity_builtin_declarations && layout->is_globals) {
          const int global_alias =
              unity_include_global_alias_status(var);
          if (global_alias < 0) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                HLSL_EMIT_REASON_BUILTIN_CONTRACT_MISMATCH, i, k);
            return;
          }
          if (global_alias > 0 ||
              is_unity_builtin_variable_ex(var->name,
                                           ctx->common_params)) {
            continue;
          }
        }

        if (!use_packoffset) {
          if (target_byte_offset > current_byte_offset) {
            uint32_t diff = target_byte_offset - current_byte_offset;
            uint32_t pad_comps = diff / 4;

            while (pad_comps > 0) {
              if (pad_comps >= 4 && (current_byte_offset % 16 == 0)) {
                sb_appendf(sb, "    float4 cb%d_%d;\n", reg,
                           current_byte_offset / 16);
                current_byte_offset += 16;
                pad_comps -= 4;
              } else if (pad_comps >= 3 && (current_byte_offset % 16 <= 4)) {
                sb_appendf(sb, "    float3 cb%d_pad_%d;\n", reg,
                           current_byte_offset);
                current_byte_offset += 12;
                pad_comps -= 3;
              } else if (pad_comps >= 2 && (current_byte_offset % 16 <= 8)) {
                sb_appendf(sb, "    float2 cb%d_pad_%d;\n", reg,
                           current_byte_offset);
                current_byte_offset += 8;
                pad_comps -= 2;
              } else {
                sb_appendf(sb, "    float cb%d_pad_%d;\n", reg,
                           current_byte_offset);
                current_byte_offset += 4;
                pad_comps -= 1;
              }
            }
          }
        }

        // Compute packoffset string
        char packoffset_str[32] = "";
        if (use_packoffset) {
          uint32_t reg_num = target_byte_offset / 16;
          uint32_t comp = (target_byte_offset % 16) / 4;
          const char* comp_suffix[] = {"", ".y", ".z", ".w"};
          if (!hlsl_format_checked(ctx, packoffset_str,
                                   sizeof(packoffset_str),
                                   " : packoffset(c%u%s)", reg_num,
                                   comp_suffix[comp])) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                HLSL_EMIT_PHASE_CBUFFER_EMISSION,
                HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW, i, k);
            return;
          }
        }

        if (var->is_matrix != 0) {
          const char* major_modifier = var->row_major ? "row_major " : "";
          if (var->matrix_array_size > 0) {
            sb_appendf(sb, "    %sfloat%ux%u %s[%u]%s;\n", major_modifier,
                       var->rows, var->dim, var->name,
                       var->matrix_array_size, packoffset_str);
          } else {
            sb_appendf(sb, "    %sfloat%ux%u %s%s;\n", major_modifier,
                       var->rows, var->dim, var->name, packoffset_str);
          }
        } else {
          const char *type_name = var->type == 0 ? "float" :
                                  var->type == 1 ? "int" : "bool";
          bool is_array = (var->matrix_array_size > 0);
          uint32_t array_len = is_array ? var->matrix_array_size : var->rows;
          if (is_array || var->rows > 1) {
            if (var->dim == 1) {
              sb_appendf(sb, "    %s %s[%u]%s;\n", type_name, var->name,
                         array_len, packoffset_str);
            } else {
              sb_appendf(sb, "    %s%u %s[%u]%s;\n", type_name, var->dim,
                         var->name, array_len, packoffset_str);
            }
          } else {
            if (var->dim == 1) {
              sb_appendf(sb, "    %s %s%s;\n", type_name, var->name,
                         packoffset_str);
            } else {
              sb_appendf(sb, "    %s%u %s%s;\n", type_name, var->dim,
                         var->name, packoffset_str);
            }
          }
        }
        current_byte_offset = target_byte_offset + var->byte_size;
    }

      /* Extend only the source/reflection shell. The executable access model
       * continues to use row_count from stripped DXBC. D3DCompiler retains
       * this unused tail in reflection while trimming it from dcl_constantbuffer. */
      uint32_t total_size_bytes = layout->reflection_size_bytes;
      if (current_byte_offset < total_size_bytes) {
        if (use_packoffset) {
          uint32_t last_reg = (total_size_bytes / 16) - 1;
          if (current_byte_offset <= last_reg * 16) {
            sb_appendf(sb, "    float4 _cb%d_pad_end : packoffset(c%u);\n",
                       reg, last_reg);
          }
        } else {
          uint32_t diff = total_size_bytes - current_byte_offset;
          uint32_t pad_comps = diff / 4;
          while (pad_comps > 0) {
              if (pad_comps >= 4 && (current_byte_offset % 16 == 0)) {
              sb_appendf(sb, "    float4 cb%d_%d;\n", reg,
                         current_byte_offset / 16);
              current_byte_offset += 16;
              pad_comps -= 4;
            } else if (pad_comps >= 3 && (current_byte_offset % 16 <= 4)) {
              sb_appendf(sb, "    float3 cb%d_pad_%d;\n", reg,
                         current_byte_offset);
              current_byte_offset += 12;
              pad_comps -= 3;
            } else if (pad_comps >= 2 && (current_byte_offset % 16 <= 8)) {
              sb_appendf(sb, "    float2 cb%d_pad_%d;\n", reg,
                         current_byte_offset);
              current_byte_offset += 8;
              pad_comps -= 2;
            } else {
              sb_appendf(sb, "    float cb%d_pad_%d;\n", reg,
                         current_byte_offset);
              current_byte_offset += 4;
              pad_comps -= 1;
            }
          }
        }
      }

      if (!layout->is_globals) {
        sb_append(sb, "};\n\n");
      } else {
        sb_append(sb, "\n");
      }
  }
}

typedef struct {
  const char *name;
  uint32_t type;
  uint32_t dim;
  uint32_t comp_offset;
  int is_matrix;
  uint32_t rows;
  uint32_t matrix_array_size;
  uint32_t rel_idx;
  bool row_major;
} CBufferRegisterVariable;

static bool append_cbuffer_register_access(
    StringBuilder *sb, const CBufferRegisterVariable *variable) {
  if (!sb || !variable || !variable->name) return false;
  if (variable->is_matrix != 0 && variable->matrix_array_size > 0) {
    if (variable->rows == 0) return false;
    uint32_t matrix_index = variable->rel_idx / variable->rows;
    uint32_t row_index = variable->rel_idx % variable->rows;
    if (variable->row_major) {
      sb_appendf(sb, "%s[%u][%u]", variable->name, matrix_index, row_index);
    } else {
      sb_appendf(sb, "transpose(%s[%u])[%u]", variable->name, matrix_index,
                 row_index);
    }
  } else if (variable->is_matrix != 0) {
    if (variable->row_major) {
      sb_appendf(sb, "%s[%u]", variable->name, variable->rel_idx);
    } else {
      sb_appendf(sb, "transpose(%s)[%u]", variable->name,
                 variable->rel_idx);
    }
  } else if (variable->matrix_array_size > 0 || variable->rows > 1) {
    sb_appendf(sb, "%s[%u]", variable->name, variable->rel_idx);
  } else {
    sb_append(sb, variable->name);
  }
  return sb_ok(sb);
}

static bool append_cbuffer_register_raw_value(
    StringBuilder *sb, const CBufferRegisterVariable *variable) {
  if (!sb || !variable || variable->type > 2 || variable->dim < 1 ||
      variable->dim > 4) {
    return false;
  }
  if (variable->type == 0) {
    return append_cbuffer_register_access(sb, variable);
  }
  if (variable->type == 1) {
    sb_append(sb, "asfloat(");
    if (!append_cbuffer_register_access(sb, variable)) return false;
    sb_append_char(sb, ')');
    return sb_ok(sb);
  }

  if (variable->dim > 1) sb_appendf(sb, "float%u(", variable->dim);
  for (uint32_t component = 0; component < variable->dim; ++component) {
    if (component > 0) sb_append(sb, ", ");
    sb_append(sb, "asfloat((");
    if (!append_cbuffer_register_access(sb, variable)) return false;
    if (variable->dim > 1) sb_appendf(sb, "[%u]", component);
    sb_append(sb, ") ? 1u : 0u)");
  }
  if (variable->dim > 1) sb_append_char(sb, ')');
  return sb_ok(sb);
}

static bool append_cbuffer_register_as_float4(
    StringBuilder *sb, const CBufferRegisterVariable *variable) {
  if (!variable || variable->dim < 1 || variable->dim > 4 ||
      variable->comp_offset >= 4 ||
      variable->dim > 4 - variable->comp_offset) {
    return false;
  }
  if (variable->dim == 4) {
    return append_cbuffer_register_raw_value(sb, variable);
  }

  sb_append(sb, "float4(");
  bool first = true;
  for (uint32_t component = 0; component < 4;) {
    if (!first) sb_append(sb, ", ");
    if (component == variable->comp_offset) {
      if (!append_cbuffer_register_raw_value(sb, variable)) return false;
      component += variable->dim;
    } else {
      sb_append(sb, "0.0f");
      component++;
    }
    first = false;
  }
  sb_append_char(sb, ')');
  return sb_ok(sb);
}

void emit_cbuffer_helpers(HLSLEmitterContext* ctx) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;

  for (int i = 0; i < program->cbuffer_count; i++) {
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[i];
    int reg = layout->reg;

    if (layout->row_struct_storage) continue;
    if (layout->raw_storage) {
      sb_appendf(sb,
                 "float4 get_cb%d(int idx) {\n"
                 "    return cb%d_data[idx];\n"
                 "}\n\n",
                 reg, reg);
      continue;
    }
    sb_appendf(sb, "float4 get_cb%d(int idx) {\n    switch(idx) {\n",
               reg);

    for (int o = 0; o < program->cbuffers[i].size; o++) {
      CBufferRegisterVariable reg_vars[8];
      int reg_var_count = 0;

      for (int k = 0; k < layout->variable_count; k++) {
        const TempVariable *var = &layout->variables[k];
        uint32_t occupied_regs = get_var_occupied_regs(
            var->is_matrix, var->rows, var->matrix_array_size);

        if (o >= (int)var->reg_offset &&
            o < (int)(var->reg_offset + occupied_regs)) {
          uint32_t rel_idx = o - var->reg_offset;
          uint32_t comp_offset = (var->byte_offset % 16) / 4;
          if (reg_var_count >=
              (int)(sizeof(reg_vars) / sizeof(reg_vars[0]))) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION,
                HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, i, k);
            return;
          }
          reg_vars[reg_var_count].name = var->name;
          reg_vars[reg_var_count].type = var->type;
          reg_vars[reg_var_count].dim = var->dim;
          reg_vars[reg_var_count].comp_offset = comp_offset;
          reg_vars[reg_var_count].is_matrix = var->is_matrix;
          reg_vars[reg_var_count].rows = var->rows;
          reg_vars[reg_var_count].matrix_array_size = var->matrix_array_size;
          reg_vars[reg_var_count].rel_idx = rel_idx;
          reg_vars[reg_var_count].row_major = var->row_major;
          reg_var_count++;
        }
      }

      sb_appendf(sb, "        case %d: return ", o);
      if (reg_var_count == 1) {
        if (!append_cbuffer_register_as_float4(sb, &reg_vars[0])) {
          fail_cbuffer_location(
              ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
              HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION,
              HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, i,
              cbuffer_layout_variable_index(layout, reg_vars[0].name));
          return;
        }
      } else if (reg_var_count > 1) {
        for (int a = 0; a < reg_var_count - 1; a++) {
          for (int b = a + 1; b < reg_var_count; b++) {
            if (reg_vars[a].comp_offset > reg_vars[b].comp_offset) {
              CBufferRegisterVariable tmp = reg_vars[a];
              reg_vars[a] = reg_vars[b];
              reg_vars[b] = tmp;
            }
          }
        }
        int component_owner[4] = {-1, -1, -1, -1};
        for (int rv_idx = 0; rv_idx < reg_var_count; rv_idx++) {
          CBufferRegisterVariable *variable = &reg_vars[rv_idx];
          if (variable->dim < 1 || variable->dim > 4 ||
              variable->comp_offset >= 4 ||
              variable->dim > 4 - variable->comp_offset) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION,
                HLSL_EMIT_REASON_INVALID_PARAMETER_LAYOUT, i,
                cbuffer_layout_variable_index(layout, variable->name));
            return;
          }
          for (uint32_t component = variable->comp_offset;
               component < variable->comp_offset + variable->dim;
               ++component) {
            if (component_owner[component] != -1) {
              fail_cbuffer_location(
                  ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                  HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION,
                  HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY, i,
                  cbuffer_layout_variable_index(layout, variable->name));
              return;
            }
            component_owner[component] =
                component == variable->comp_offset ? rv_idx : -2;
          }
        }

        sb_append(sb, "float4(");
        bool first = true;
        for (int component = 0; component < 4; ++component) {
          if (component_owner[component] == -2) continue;
          if (!first) sb_append(sb, ", ");
          if (component_owner[component] < 0) {
            sb_append(sb, "0.0f");
          } else if (!append_cbuffer_register_raw_value(
                         sb, &reg_vars[component_owner[component]])) {
            fail_cbuffer_location(
                ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION,
                HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT, i,
                cbuffer_layout_variable_index(
                    layout,
                    reg_vars[component_owner[component]].name));
            return;
          }
          first = false;
        }
        sb_append_char(sb, ')');
      } else {
        sb_append(sb, "(float4)0");
      }
      sb_append(sb, ";\n");
    }
    sb_append(sb, "        default: return (float4)0;\n    }\n}\n\n");
  }
}
