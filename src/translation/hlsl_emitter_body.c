// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <limits.h>
#include <string.h>

static bool lift_operand_base(const DXBCOperand* operand,
                              DXBCOperandType type, int register_index,
                              int index_dimension, uint8_t destination_mask,
                              uint8_t swizzle_mode, uint8_t x, uint8_t y,
                              uint8_t z, uint8_t w, bool negate,
                              bool absolute) {
  if (!operand || operand->type != type ||
      operand->register_index != register_index ||
      operand->register_index_dim != index_dimension ||
      operand->destination_mask != destination_mask ||
      operand->swizzle_mode != swizzle_mode || operand->swizzle[0] != x ||
      operand->swizzle[1] != y || operand->swizzle[2] != z ||
      operand->swizzle[3] != w || operand->has_neg != negate ||
      operand->has_abs != absolute || operand->min_precision != 0 ||
      operand->rel_op0 || operand->rel_op1 || operand->rel_op2 ||
      operand->imm_value_count != 0 || operand->immediate_word_count != 0) {
    return false;
  }
  const uint32_t expected_modifier =
      negate && absolute ? UINT32_C(0x000000c1)
      : absolute        ? UINT32_C(0x00000081)
      : negate          ? UINT32_C(0x00000041)
                        : 0;
  if (expected_modifier == 0) {
    if (operand->extended_token_count != 0 || operand->extended_tokens)
      return false;
  } else if (operand->extended_token_count != 1 ||
             !operand->extended_tokens ||
             operand->extended_tokens[0] != expected_modifier) {
    return false;
  }
  for (int index = 0; index < 3; ++index) {
    if (index < index_dimension) {
      if (!operand->index_has_immediate[index] ||
          operand->index_representations[index] != 0 ||
          operand->index_value_exceeds_int[index]) {
        return false;
      }
    } else if (operand->index_has_immediate[index] ||
               operand->index_representations[index] != 0 ||
               operand->index_value_exceeds_int[index] ||
               operand->index_values[index] != 0) {
      return false;
    }
  }
  return true;
}

static bool lift_register_destination(const DXBCOperand* operand,
                                      DXBCOperandType type,
                                      int register_index, uint8_t mask) {
  return lift_operand_base(operand, type, register_index, 1, mask, 0,
                           0, 1, 2, 3, false, false) &&
         operand->index_values[0] == (uint64_t)register_index;
}

static bool lift_register_source(const DXBCOperand* operand,
                                 DXBCOperandType type, int register_index,
                                 uint8_t swizzle_mode, uint8_t x, uint8_t y,
                                 uint8_t z, uint8_t w, bool negate,
                                 bool absolute) {
  return lift_operand_base(operand, type, register_index, 1, 0,
                           swizzle_mode, x, y, z, w, negate, absolute) &&
         operand->index_values[0] == (uint64_t)register_index;
}

static bool lift_geometry_input(const DXBCOperand* operand,
                                uint32_t vertex, uint32_t register_index,
                                uint8_t swizzle_mode, uint8_t x, uint8_t y,
                                uint8_t z, uint8_t w) {
  return vertex <= INT_MAX && register_index <= INT_MAX &&
         lift_operand_base(operand, OPERAND_TYPE_INPUT, (int)vertex, 2, 0,
                           swizzle_mode, x, y, z, w, false, false) &&
         operand->index_values[0] == vertex &&
         operand->index_values[1] == register_index;
}

static bool lift_negated_geometry_input(const DXBCOperand* operand,
                                        uint32_t vertex,
                                        uint32_t register_index,
                                        uint8_t swizzle_mode, uint8_t x,
                                        uint8_t y, uint8_t z, uint8_t w) {
  return vertex <= INT_MAX && register_index <= INT_MAX &&
         lift_operand_base(operand, OPERAND_TYPE_INPUT, (int)vertex, 2, 0,
                           swizzle_mode, x, y, z, w, true, false) &&
         operand->index_values[0] == vertex &&
         operand->index_values[1] == register_index;
}

static bool lift_constant_buffer_source(const DXBCOperand* operand,
                                        uint32_t buffer,
                                        uint32_t element,
                                        uint8_t swizzle_mode, uint8_t x,
                                        uint8_t y, uint8_t z, uint8_t w,
                                        bool negate) {
  return buffer <= INT_MAX &&
         lift_operand_base(operand, OPERAND_TYPE_CONSTANT_BUFFER,
                           (int)buffer, 2, 0, swizzle_mode, x, y, z, w,
                           negate, false) &&
         operand->index_values[0] == buffer &&
         operand->index_values[1] == element;
}

static bool lift_immediate32(const DXBCOperand* operand,
                             const uint32_t* words, int word_count) {
  if (!operand || !words || (word_count != 1 && word_count != 4) ||
      operand->type != OPERAND_TYPE_IMMEDIATE32 ||
      operand->register_index_dim != 0 || operand->register_index != 0 ||
      operand->destination_mask != 0 || operand->swizzle_mode != 0 ||
      operand->swizzle[0] != 0 || operand->swizzle[1] != 1 ||
      operand->swizzle[2] != 2 || operand->swizzle[3] != 3 ||
      operand->has_neg || operand->has_abs || operand->min_precision != 0 ||
      operand->extended_token_count != 0 || operand->extended_tokens ||
      operand->rel_op0 || operand->rel_op1 || operand->rel_op2 ||
      operand->imm_value_count != word_count ||
      operand->immediate_word_count != word_count) {
    return false;
  }
  for (int index = 0; index < 3; ++index) {
    if (operand->index_has_immediate[index] ||
        operand->index_representations[index] != 0 ||
        operand->index_value_exceeds_int[index] ||
        operand->index_values[index] != 0) {
      return false;
    }
  }
  for (int word = 0; word < word_count; ++word) {
    if (operand->immediate_words[word] != words[word] ||
        operand->imm_values[word] != words[word]) {
      return false;
    }
  }
  return true;
}

static bool lift_instruction(const USILInstruction* instruction,
                             USILOpcode opcode, int operand_count) {
  return instruction && instruction->opcode == opcode &&
         instruction->operand_count == operand_count &&
         !instruction->saturate &&
         instruction->condition_test == DXBC_INSTRUCTION_TEST_NONE &&
         instruction->geometry_effect == USIL_GEOMETRY_EFFECT_NONE &&
         instruction->geometry_stream_id == 0 &&
         !instruction->geometry_stream_explicit &&
         !instruction->has_resource_dimension &&
         !instruction->has_texel_offset &&
         !instruction->has_resource_return_types &&
         instruction->resource_stride == 0;
}

static bool lift_geometry_append_instruction(
    const USILInstruction* instruction) {
  return instruction && instruction->opcode == USIL_OP_GEOMETRY_APPEND &&
         instruction->operand_count == 0 && !instruction->saturate &&
         instruction->condition_test == DXBC_INSTRUCTION_TEST_NONE &&
         instruction->geometry_effect == USIL_GEOMETRY_EFFECT_APPEND &&
         instruction->geometry_stream_id == 0 &&
         !instruction->geometry_stream_explicit;
}

static bool lift_dynamic_geometry_input(const DXBCOperand* operand,
                                        uint8_t counter_component,
                                        uint32_t register_index,
                                        uint8_t swizzle_mode, uint8_t x,
                                        uint8_t y, uint8_t z, uint8_t w) {
  if (!operand || register_index > INT_MAX || counter_component > 3 ||
      operand->type != OPERAND_TYPE_INPUT || operand->register_index != 0 ||
      operand->register_index_dim != 2 || operand->destination_mask != 0 ||
      operand->swizzle_mode != swizzle_mode || operand->swizzle[0] != x ||
      operand->swizzle[1] != y || operand->swizzle[2] != z ||
      operand->swizzle[3] != w || operand->has_neg || operand->has_abs ||
      operand->min_precision != 0 || operand->extended_token_count != 0 ||
      operand->extended_tokens || !operand->rel_op0 || operand->rel_op1 ||
      operand->rel_op2 || operand->imm_value_count != 0 ||
      operand->immediate_word_count != 0 ||
      operand->index_representations[0] != 2 ||
      operand->index_representations[1] != 0 ||
      operand->index_representations[2] != 0 ||
      operand->index_has_immediate[0] ||
      !operand->index_has_immediate[1] ||
      operand->index_has_immediate[2] ||
      operand->index_value_exceeds_int[0] ||
      operand->index_value_exceeds_int[1] ||
      operand->index_value_exceeds_int[2] ||
      operand->index_values[0] != 0 ||
      operand->index_values[1] != register_index ||
      operand->index_values[2] != 0) {
    return false;
  }
  return lift_register_source(operand->rel_op0, OPERAND_TYPE_TEMP, 0, 2,
                              counter_component, counter_component,
                              counter_component, counter_component, false,
                              false);
}

static bool lift_geometry_stream_effect(const USILInstruction* instruction,
                                        USILOpcode opcode,
                                        USILGeometryEffectKind effect) {
  return instruction && instruction->opcode == opcode &&
         instruction->operand_count == 1 && !instruction->saturate &&
         instruction->precise_mask == 0 &&
         instruction->condition_test == DXBC_INSTRUCTION_TEST_NONE &&
         instruction->geometry_effect == effect &&
         instruction->geometry_stream_id == 0 &&
         instruction->geometry_stream_explicit &&
         !instruction->has_resource_dimension &&
         !instruction->has_texel_offset &&
         !instruction->has_resource_return_types &&
         instruction->resource_stride == 0 &&
         lift_register_source(&instruction->operands[0],
                              OPERAND_TYPE_STREAM, 0, 0, 0, 1, 2, 3,
                              false, false);
}

static bool lift_condition_instruction(const USILInstruction* instruction,
                                       USILOpcode opcode, int operand_count,
                                       DXBCInstructionTest test) {
  return instruction && instruction->opcode == opcode &&
         instruction->operand_count == operand_count &&
         !instruction->saturate && instruction->precise_mask == 0 &&
         instruction->condition_test == test &&
         instruction->geometry_effect == USIL_GEOMETRY_EFFECT_NONE &&
         instruction->geometry_stream_id == 0 &&
         !instruction->geometry_stream_explicit &&
         !instruction->has_resource_dimension &&
         !instruction->has_texel_offset &&
         !instruction->has_resource_return_types &&
         instruction->resource_stride == 0;
}

#define LIFT_D(index, operand, type, reg, mask)                                 \
  lift_register_destination(&(index)->operands[(operand)], (type), (reg),      \
                            (mask))
#define LIFT_S(index, operand, type, reg, mode, x, y, z, w, neg, abs)           \
  lift_register_source(&(index)->operands[(operand)], (type), (reg), (mode),   \
                       (x), (y), (z), (w), (neg), (abs))
#define LIFT_I(index, operand, vertex, reg, mode, x, y, z, w)                   \
  lift_geometry_input(&(index)->operands[(operand)], (vertex), (reg), (mode),  \
                      (x), (y), (z), (w))
#define LIFT_CB(index, operand, buffer, element, mode, x, y, z, w, neg)         \
  lift_constant_buffer_source(&(index)->operands[(operand)], (buffer),         \
                              (element), (mode), (x), (y), (z), (w), (neg))
#define LIFT_OP(index, opcode, count)                                           \
  lift_instruction(&(program)->instructions[(index)], (opcode), (count))

bool hlsl_triangle_edge_distance_lift_matches(const USILProgram* program) {
  static const uint32_t zero4[4] = {0, 0, 0, 0};
  static const uint32_t one4[4] = {UINT32_C(0x3f800000),
                                    UINT32_C(0x3f800000),
                                    UINT32_C(0x3f800000),
                                    UINT32_C(0x3f800000)};
  static const uint32_t eight_hundred[1] = {UINT32_C(0x44480000)};
  if (!program || program->program_type != DXBC_PROGRAM_TYPE_GEOMETRY ||
      !program->has_stage_contract || !program->geometry.valid ||
      program->geometry.input_primitive != DXBC_INPUT_PRIMITIVE_TRIANGLE ||
      program->geometry.output_topology !=
          DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP ||
      program->geometry.input_vertex_count != 3 ||
      program->geometry.max_output_vertex_count != 3 ||
      program->geometry.has_instance_count ||
      program->geometry.declared_stream_mask != 0 ||
      program->geometry.referenced_stream_mask != 1 ||
      program->geometry.effect_count != 3 ||
      !program->geometry.output_tuple_state_persists ||
      program->shader_model_major != 4 || program->shader_model_minor != 0 ||
      program->temp_count != 2 || program->input_count != 2 ||
      program->input_alloc < 2 || !program->inputs ||
      program->output_count != 3 || program->output_alloc < 3 ||
      !program->outputs || program->cbuffer_count != 1 ||
      program->cbuffer_alloc < 1 || !program->cbuffers ||
      program->cbuffers[0].reg_idx != 0 || program->cbuffers[0].size != 3 ||
      program->texture_count != 0 || program->sampler_count != 0 ||
      program->uav_count != 0 || program->indexable_temp_count != 0 ||
      program->icb_value_count != 0 || program->instruction_count != 45 ||
      program->instruction_alloc < 45 || !program->instructions) {
    return false;
  }
  for (int input = 0; input < 2; ++input) {
    if (program->inputs[input].register_id != (uint32_t)input ||
        program->inputs[input].component_type != 3 ||
        program->inputs[input].mask != 0x0fu ||
        program->inputs[input].stream_index != 0) return false;
  }
  for (int output = 0; output < 3; ++output) {
    if (program->outputs[output].register_id != (uint32_t)output ||
        program->outputs[output].component_type != 3 ||
        program->outputs[output].mask != 0x0fu ||
        program->outputs[output].stream_index != 0) return false;
  }

  const USILInstruction* i = program->instructions;
#define MATCH2(n, op, a, b)                                                     \
  do {                                                                          \
    if (!LIFT_OP((n), (op), 2) || !(a) || !(b)) return false;                   \
  } while (0)
#define MATCH3(n, op, a, b, c)                                                  \
  do {                                                                          \
    if (!LIFT_OP((n), (op), 3) || !(a) || !(b) || !(c)) return false;           \
  } while (0)
#define MATCH4(n, op, a, b, c, d)                                               \
  do {                                                                          \
    if (!LIFT_OP((n), (op), 4) || !(a) || !(b) || !(c) || !(d)) return false;   \
  } while (0)

  MATCH2(0, USIL_OP_MOV, LIFT_D(&i[0], 0, OPERAND_TYPE_OUTPUT, 0, 0xf0),
         LIFT_I(&i[0], 1, 0, 0, 1, 0, 1, 2, 3));
  MATCH2(1, USIL_OP_MOV, LIFT_D(&i[1], 0, OPERAND_TYPE_OUTPUT, 1, 0xf0),
         LIFT_I(&i[1], 1, 0, 1, 1, 0, 1, 2, 3));
  MATCH3(2, USIL_OP_DIV, LIFT_D(&i[2], 0, OPERAND_TYPE_TEMP, 0, 0x30),
         LIFT_I(&i[2], 1, 1, 0, 1, 0, 1, 0, 0),
         LIFT_I(&i[2], 2, 1, 0, 1, 3, 3, 3, 3));
  MATCH3(3, USIL_OP_DIV, LIFT_D(&i[3], 0, OPERAND_TYPE_TEMP, 0, 0xc0),
         LIFT_I(&i[3], 1, 2, 0, 1, 0, 0, 0, 1),
         LIFT_I(&i[3], 2, 2, 0, 1, 3, 3, 3, 3));
  MATCH3(4, USIL_OP_ADD, LIFT_D(&i[4], 0, OPERAND_TYPE_TEMP, 1, 0x30),
         LIFT_S(&i[4], 1, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 0, 0, true, false),
         LIFT_S(&i[4], 2, OPERAND_TYPE_TEMP, 0, 1, 2, 3, 2, 2, false, false));
  MATCH3(5, USIL_OP_DP2, LIFT_D(&i[5], 0, OPERAND_TYPE_TEMP, 1, 0x10),
         LIFT_S(&i[5], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 0, 0, false, false),
         LIFT_S(&i[5], 2, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 0, 0, false, false));
  MATCH2(6, USIL_OP_SQRT, LIFT_D(&i[6], 0, OPERAND_TYPE_TEMP, 1, 0x10),
         LIFT_S(&i[6], 1, OPERAND_TYPE_TEMP, 1, 2, 0, 0, 0, 0, false, false));
  MATCH3(7, USIL_OP_DIV, LIFT_D(&i[7], 0, OPERAND_TYPE_TEMP, 1, 0x60),
         LIFT_I(&i[7], 1, 0, 0, 1, 0, 0, 1, 0),
         LIFT_I(&i[7], 2, 0, 0, 1, 3, 3, 3, 3));
  MATCH3(8, USIL_OP_ADD, LIFT_D(&i[8], 0, OPERAND_TYPE_TEMP, 0, 0xf0),
         LIFT_S(&i[8], 1, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 2, 3, false, false),
         LIFT_S(&i[8], 2, OPERAND_TYPE_TEMP, 1, 1, 1, 2, 1, 2, true, false));
  MATCH3(9, USIL_OP_MUL, LIFT_D(&i[9], 0, OPERAND_TYPE_TEMP, 1, 0x20),
         LIFT_S(&i[9], 1, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0, false, false),
         LIFT_S(&i[9], 2, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3, false, false));
  MATCH4(10, USIL_OP_MAD, LIFT_D(&i[10], 0, OPERAND_TYPE_TEMP, 1, 0x20),
         LIFT_S(&i[10], 1, OPERAND_TYPE_TEMP, 0, 2, 2, 2, 2, 2, false, false),
         LIFT_S(&i[10], 2, OPERAND_TYPE_TEMP, 0, 2, 1, 1, 1, 1, false, false),
         LIFT_S(&i[10], 3, OPERAND_TYPE_TEMP, 1, 2, 1, 1, 1, 1, true, false));
  MATCH3(11, USIL_OP_DP2, LIFT_D(&i[11], 0, OPERAND_TYPE_TEMP, 0, 0x40),
         LIFT_S(&i[11], 1, OPERAND_TYPE_TEMP, 0, 1, 2, 3, 2, 2, false, false),
         LIFT_S(&i[11], 2, OPERAND_TYPE_TEMP, 0, 1, 2, 3, 2, 2, false, false));
  MATCH2(12, USIL_OP_SQRT, LIFT_D(&i[12], 0, OPERAND_TYPE_TEMP, 0, 0x40),
         LIFT_S(&i[12], 1, OPERAND_TYPE_TEMP, 0, 2, 2, 2, 2, 2, false, false));
  MATCH3(13, USIL_OP_DIV, LIFT_D(&i[13], 0, OPERAND_TYPE_TEMP, 0, 0x40),
         LIFT_S(&i[13], 1, OPERAND_TYPE_TEMP, 1, 2, 1, 1, 1, 1, false, true),
         LIFT_S(&i[13], 2, OPERAND_TYPE_TEMP, 0, 2, 2, 2, 2, 2, false, false));
  MATCH3(14, USIL_OP_MUL, LIFT_D(&i[14], 0, OPERAND_TYPE_TEMP, 0, 0x40),
         LIFT_S(&i[14], 1, OPERAND_TYPE_TEMP, 0, 2, 2, 2, 2, 2, false, false),
         LIFT_I(&i[14], 2, 1, 0, 2, 3, 3, 3, 3));
  MATCH3(15, USIL_OP_DP2, LIFT_D(&i[15], 0, OPERAND_TYPE_TEMP, 0, 0x10),
         LIFT_S(&i[15], 1, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 0, 0, false, false),
         LIFT_S(&i[15], 2, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 0, 0, false, false));
  MATCH2(16, USIL_OP_SQRT, LIFT_D(&i[16], 0, OPERAND_TYPE_TEMP, 0, 0x10),
         LIFT_S(&i[16], 1, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0, false, false));
  MATCH3(17, USIL_OP_DIV, LIFT_D(&i[17], 0, OPERAND_TYPE_TEMP, 0, 0x10),
         LIFT_S(&i[17], 1, OPERAND_TYPE_TEMP, 1, 2, 1, 1, 1, 1, false, true),
         LIFT_S(&i[17], 2, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0, false, false));
  MATCH3(18, USIL_OP_DIV, LIFT_D(&i[18], 0, OPERAND_TYPE_TEMP, 0, 0x20),
         LIFT_S(&i[18], 1, OPERAND_TYPE_TEMP, 1, 2, 1, 1, 1, 1, false, true),
         LIFT_S(&i[18], 2, OPERAND_TYPE_TEMP, 1, 2, 0, 0, 0, 0, false, false));
  MATCH3(19, USIL_OP_MUL, LIFT_D(&i[19], 0, OPERAND_TYPE_TEMP, 0, 0x20),
         LIFT_S(&i[19], 1, OPERAND_TYPE_TEMP, 0, 2, 1, 1, 1, 1, false, false),
         LIFT_I(&i[19], 2, 0, 0, 2, 3, 3, 3, 3));
  MATCH3(20, USIL_OP_MUL, LIFT_D(&i[20], 0, OPERAND_TYPE_TEMP, 0, 0x10),
         LIFT_S(&i[20], 1, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0, false, false),
         LIFT_I(&i[20], 2, 2, 0, 2, 3, 3, 3, 3));
  MATCH3(21, USIL_OP_ADD, LIFT_D(&i[21], 0, OPERAND_TYPE_TEMP, 0, 0x80),
         LIFT_CB(&i[21], 1, 0, 2, 2, 0, 0, 0, 0, true),
         lift_immediate32(&i[21].operands[2], eight_hundred, 1));
  MATCH3(22, USIL_OP_MUL, LIFT_D(&i[22], 0, OPERAND_TYPE_TEMP, 1, 0x10),
         LIFT_S(&i[22], 1, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3, false, false),
         LIFT_S(&i[22], 2, OPERAND_TYPE_TEMP, 0, 2, 1, 1, 1, 1, false, false));
  MATCH2(23, USIL_OP_MOV, LIFT_D(&i[23], 0, OPERAND_TYPE_TEMP, 1, 0x60),
         lift_immediate32(&i[23].operands[1], zero4, 4));
  MATCH2(24, USIL_OP_MOV, LIFT_D(&i[24], 0, OPERAND_TYPE_OUTPUT, 2, 0x70),
         LIFT_S(&i[24], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0, false, false));
  MATCH3(25, USIL_OP_DIV, LIFT_D(&i[25], 0, OPERAND_TYPE_TEMP, 0, 0x20),
         lift_immediate32(&i[25].operands[1], one4, 4),
         LIFT_I(&i[25], 2, 0, 0, 2, 3, 3, 3, 3));
  MATCH2(26, USIL_OP_MOV, LIFT_D(&i[26], 0, OPERAND_TYPE_OUTPUT, 2, 0x80),
         LIFT_S(&i[26], 1, OPERAND_TYPE_TEMP, 0, 2, 1, 1, 1, 1, false, false));
  if (!lift_geometry_append_instruction(&i[27])) return false;
  MATCH2(28, USIL_OP_MOV, LIFT_D(&i[28], 0, OPERAND_TYPE_OUTPUT, 0, 0xf0),
         LIFT_I(&i[28], 1, 1, 0, 1, 0, 1, 2, 3));
  MATCH2(29, USIL_OP_MOV, LIFT_D(&i[29], 0, OPERAND_TYPE_OUTPUT, 1, 0xf0),
         LIFT_I(&i[29], 1, 1, 1, 1, 0, 1, 2, 3));
  MATCH3(30, USIL_OP_MUL, LIFT_D(&i[30], 0, OPERAND_TYPE_TEMP, 1, 0x20),
         LIFT_S(&i[30], 1, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3, false, false),
         LIFT_S(&i[30], 2, OPERAND_TYPE_TEMP, 0, 2, 2, 2, 2, 2, false, false));
  MATCH3(31, USIL_OP_MUL, LIFT_D(&i[31], 0, OPERAND_TYPE_TEMP, 0, 0x40),
         LIFT_S(&i[31], 1, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3, false, false),
         LIFT_S(&i[31], 2, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0, false, false));
  MATCH2(32, USIL_OP_MOV, LIFT_D(&i[32], 0, OPERAND_TYPE_TEMP, 1, 0x50),
         lift_immediate32(&i[32].operands[1], zero4, 4));
  MATCH2(33, USIL_OP_MOV, LIFT_D(&i[33], 0, OPERAND_TYPE_OUTPUT, 2, 0x70),
         LIFT_S(&i[33], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0, false, false));
  MATCH3(34, USIL_OP_DIV, LIFT_D(&i[34], 0, OPERAND_TYPE_TEMP, 0, 0x80),
         lift_immediate32(&i[34].operands[1], one4, 4),
         LIFT_I(&i[34], 2, 1, 0, 2, 3, 3, 3, 3));
  MATCH2(35, USIL_OP_MOV, LIFT_D(&i[35], 0, OPERAND_TYPE_OUTPUT, 2, 0x80),
         LIFT_S(&i[35], 1, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3, false, false));
  if (!lift_geometry_append_instruction(&i[36])) return false;
  MATCH2(37, USIL_OP_MOV, LIFT_D(&i[37], 0, OPERAND_TYPE_OUTPUT, 0, 0xf0),
         LIFT_I(&i[37], 1, 2, 0, 1, 0, 1, 2, 3));
  MATCH2(38, USIL_OP_MOV, LIFT_D(&i[38], 0, OPERAND_TYPE_OUTPUT, 1, 0xf0),
         LIFT_I(&i[38], 1, 2, 1, 1, 0, 1, 2, 3));
  MATCH2(39, USIL_OP_MOV, LIFT_D(&i[39], 0, OPERAND_TYPE_TEMP, 0, 0x30),
         lift_immediate32(&i[39].operands[1], zero4, 4));
  MATCH2(40, USIL_OP_MOV, LIFT_D(&i[40], 0, OPERAND_TYPE_OUTPUT, 2, 0x70),
         LIFT_S(&i[40], 1, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 2, 0, false, false));
  MATCH3(41, USIL_OP_DIV, LIFT_D(&i[41], 0, OPERAND_TYPE_TEMP, 0, 0x10),
         lift_immediate32(&i[41].operands[1], one4, 4),
         LIFT_I(&i[41], 2, 2, 0, 2, 3, 3, 3, 3));
  MATCH2(42, USIL_OP_MOV, LIFT_D(&i[42], 0, OPERAND_TYPE_OUTPUT, 2, 0x80),
         LIFT_S(&i[42], 1, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0, false, false));
  if (!lift_geometry_append_instruction(&i[43]) ||
      !LIFT_OP(44, USIL_OP_RET, 0)) return false;

#undef MATCH2
#undef MATCH3
#undef MATCH4
  return true;
}

static bool extrusion_signature_declaration_matches(
    const USILSignatureDeclaration* declaration,
    USILSignatureDeclarationKind kind, DXBCOperandType operand_type,
    uint32_t register_id, uint8_t mask, bool is_array,
    bool has_system_value) {
  return declaration && declaration->kind == kind &&
         declaration->operand_type == operand_type &&
         declaration->has_signature_register &&
         declaration->register_id == register_id &&
         declaration->mask == mask && declaration->stream_index == 0 &&
         declaration->has_array_element_count == is_array &&
         declaration->array_element_count == (is_array ? 3u : 0u) &&
         declaration->has_system_value == has_system_value &&
         declaration->system_value_name == (has_system_value ? 1u : 0u) &&
         !declaration->has_interpolation &&
         declaration->interpolation_mode == 0;
}

static bool extrusion_signature_element_matches(
    const DXBCSignatureElement* element, const char* semantic,
    uint32_t register_id, uint8_t mask, uint32_t system_value) {
  if (!element || !semantic) return false;
  const char* actual_semantic = dxbc_signature_semantic_name(element);
  return actual_semantic && strcmp(actual_semantic, semantic) == 0 &&
         element->semantic_index == 0 &&
         element->system_value == system_value &&
         element->component_type == 3 && element->register_id == register_id &&
         element->min_precision == 0 && element->stream_index == 0 &&
         element->mask == mask;
}

bool hlsl_extruded_triangle_lift_matches(const USILProgram* program) {
  static const uint32_t zero[1] = {0};
  static const uint32_t one[1] = {1};
  static const uint32_t three[1] = {3};
  static const uint32_t white[4] = {
      UINT32_C(0x3f800000), UINT32_C(0x3f800000),
      UINT32_C(0x3f800000), UINT32_C(0x3f800000)};
  static const uint32_t red[4] = {
      UINT32_C(0x3f800000), 0, 0, UINT32_C(0x3f800000)};
  static const uint32_t one_third[4] = {
      UINT32_C(0x3eaaaaab), UINT32_C(0x3eaaaaab),
      UINT32_C(0x3eaaaaab), 0};
  static const uint32_t one_half[4] = {
      UINT32_C(0x3f000000), UINT32_C(0x3f000000),
      UINT32_C(0x3f000000), 0};

  if (!program || program->program_type != DXBC_PROGRAM_TYPE_GEOMETRY ||
      !program->has_stage_contract || !program->geometry.valid ||
      program->geometry.input_primitive != DXBC_INPUT_PRIMITIVE_TRIANGLE ||
      program->geometry.output_topology !=
          DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP ||
      program->geometry.input_vertex_count != 3 ||
      program->geometry.max_output_vertex_count != 6 ||
      program->geometry.has_instance_count ||
      program->geometry.declared_stream_mask != 1 ||
      program->geometry.referenced_stream_mask != 1 ||
      program->geometry.effect_count != 4 ||
      !program->geometry.output_tuple_state_persists ||
      program->shader_model_major != 5 || program->shader_model_minor != 0 ||
      !program->has_global_flags || program->global_flags != 1 ||
      !program->has_parsed_signature_authority ||
      program->temp_count != 4 || program->input_count != 2 ||
      program->input_alloc < 2 || !program->inputs ||
      program->output_count != 2 || program->output_alloc < 2 ||
      !program->outputs || program->patch_constant_count != 0 ||
      program->signature_declaration_count != 4 ||
      program->signature_declaration_alloc < 4 ||
      !program->signature_declarations || program->cbuffer_count != 3 ||
      program->cbuffer_alloc < 3 || !program->cbuffers ||
      program->cbuffers[0].reg_idx != 0 || program->cbuffers[0].size != 3 ||
      program->cbuffers[0].dynamic_indexed ||
      program->cbuffers[1].reg_idx != 1 || program->cbuffers[1].size != 4 ||
      program->cbuffers[1].dynamic_indexed ||
      program->cbuffers[2].reg_idx != 2 || program->cbuffers[2].size != 21 ||
      program->cbuffers[2].dynamic_indexed ||
      program->texture_count != 0 || program->sampler_count != 0 ||
      program->uav_count != 0 || program->indexable_temp_count != 0 ||
      program->index_range_count != 0 || program->icb_value_count != 0 ||
      program->instruction_count != 50 ||
      program->instruction_alloc < 50 || !program->instructions) {
    return false;
  }
  if (!extrusion_signature_element_matches(&program->inputs[0],
                                            "SV_POSITION", 0, 0x0f, 1) ||
      !extrusion_signature_element_matches(&program->inputs[1], "NORMAL", 1,
                                            0x07, 0) ||
      !extrusion_signature_element_matches(&program->outputs[0],
                                            "SV_POSITION", 0, 0x0f, 1) ||
      !extrusion_signature_element_matches(&program->outputs[1], "COLOR", 1,
                                            0x0f, 0) ||
      !extrusion_signature_declaration_matches(
          &program->signature_declarations[0], USIL_SIGNATURE_DECL_INPUT_SIV,
          OPERAND_TYPE_INPUT, 0, 0x0f, true, true) ||
      !extrusion_signature_declaration_matches(
          &program->signature_declarations[1], USIL_SIGNATURE_DECL_INPUT,
          OPERAND_TYPE_INPUT, 1, 0x07, true, false) ||
      !extrusion_signature_declaration_matches(
          &program->signature_declarations[2], USIL_SIGNATURE_DECL_OUTPUT_SIV,
          OPERAND_TYPE_OUTPUT, 0, 0x0f, false, true) ||
      !extrusion_signature_declaration_matches(
          &program->signature_declarations[3], USIL_SIGNATURE_DECL_OUTPUT,
          OPERAND_TYPE_OUTPUT, 1, 0x0f, false, false)) {
    return false;
  }

  const USILInstruction* i = program->instructions;
#define EX_MATCH2(n, op, a, b)                                                  \
  do {                                                                          \
    if (!LIFT_OP((n), (op), 2) || !(a) || !(b)) return false;                   \
  } while (0)
#define EX_MATCH3(n, op, a, b, c)                                               \
  do {                                                                          \
    if (!LIFT_OP((n), (op), 3) || !(a) || !(b) || !(c)) return false;           \
  } while (0)
#define EX_MATCH4(n, op, a, b, c, d)                                            \
  do {                                                                          \
    if (!LIFT_OP((n), (op), 4) || !(a) || !(b) || !(c) || !(d)) return false;   \
  } while (0)

  EX_MATCH2(0, USIL_OP_MOV,
            LIFT_D(&i[0], 0, OPERAND_TYPE_TEMP, 0, 0x10),
            lift_immediate32(&i[0].operands[1], zero, 1));
  if (!LIFT_OP(1, USIL_OP_LOOP, 0)) return false;
  EX_MATCH3(2, USIL_OP_IGE,
            LIFT_D(&i[2], 0, OPERAND_TYPE_TEMP, 0, 0x20),
            LIFT_S(&i[2], 1, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0,
                   false, false),
            lift_immediate32(&i[2].operands[2], three, 1));
  if (!lift_condition_instruction(&i[3], USIL_OP_BREAKC, 1,
                                  DXBC_INSTRUCTION_TEST_NONZERO) ||
      !LIFT_S(&i[3], 0, OPERAND_TYPE_TEMP, 0, 2, 1, 1, 1, 1,
              false, false)) return false;
  EX_MATCH3(4, USIL_OP_MUL,
            LIFT_D(&i[4], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_CB(&i[4], 1, 1, 1, 1, 0, 1, 2, 3, false),
            lift_dynamic_geometry_input(&i[4].operands[2], 0, 0, 1,
                                        1, 1, 1, 1));
  EX_MATCH4(5, USIL_OP_MAD,
            LIFT_D(&i[5], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_CB(&i[5], 1, 1, 0, 1, 0, 1, 2, 3, false),
            lift_dynamic_geometry_input(&i[5].operands[2], 0, 0, 1,
                                        0, 0, 0, 0),
            LIFT_S(&i[5], 3, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH4(6, USIL_OP_MAD,
            LIFT_D(&i[6], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_CB(&i[6], 1, 1, 2, 1, 0, 1, 2, 3, false),
            lift_dynamic_geometry_input(&i[6].operands[2], 0, 0, 1,
                                        2, 2, 2, 2),
            LIFT_S(&i[6], 3, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH3(7, USIL_OP_ADD,
            LIFT_D(&i[7], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_S(&i[7], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 3,
                   false, false),
            LIFT_CB(&i[7], 2, 1, 3, 1, 0, 1, 2, 3, false));
  EX_MATCH3(8, USIL_OP_MUL,
            LIFT_D(&i[8], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_S(&i[8], 1, OPERAND_TYPE_TEMP, 1, 1, 1, 1, 1, 1,
                   false, false),
            LIFT_CB(&i[8], 2, 2, 18, 1, 0, 1, 2, 3, false));
  EX_MATCH4(9, USIL_OP_MAD,
            LIFT_D(&i[9], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_CB(&i[9], 1, 2, 17, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[9], 2, OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0, 0,
                   false, false),
            LIFT_S(&i[9], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH4(10, USIL_OP_MAD,
            LIFT_D(&i[10], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_CB(&i[10], 1, 2, 19, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[10], 2, OPERAND_TYPE_TEMP, 1, 1, 2, 2, 2, 2,
                   false, false),
            LIFT_S(&i[10], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH4(11, USIL_OP_MAD,
            LIFT_D(&i[11], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_CB(&i[11], 1, 2, 20, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[11], 2, OPERAND_TYPE_TEMP, 1, 1, 3, 3, 3, 3,
                   false, false),
            LIFT_S(&i[11], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH2(12, USIL_OP_MOV,
            LIFT_D(&i[12], 0, OPERAND_TYPE_OUTPUT, 0, 0xf0),
            LIFT_S(&i[12], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH2(13, USIL_OP_MOV,
            LIFT_D(&i[13], 0, OPERAND_TYPE_OUTPUT, 1, 0xf0),
            lift_immediate32(&i[13].operands[1], white, 4));
  if (!lift_geometry_stream_effect(&i[14], USIL_OP_GEOMETRY_APPEND,
                                   USIL_GEOMETRY_EFFECT_APPEND)) return false;
  EX_MATCH3(15, USIL_OP_IADD,
            LIFT_D(&i[15], 0, OPERAND_TYPE_TEMP, 0, 0x10),
            LIFT_S(&i[15], 1, OPERAND_TYPE_TEMP, 0, 2, 0, 0, 0, 0,
                   false, false),
            lift_immediate32(&i[15].operands[2], one, 1));
  if (!LIFT_OP(16, USIL_OP_ENDLOOP, 0) ||
      !lift_geometry_stream_effect(&i[17],
                                   USIL_OP_GEOMETRY_RESTART_STRIP,
                                   USIL_GEOMETRY_EFFECT_RESTART_STRIP)) {
    return false;
  }
  EX_MATCH3(18, USIL_OP_ADD,
            LIFT_D(&i[18], 0, OPERAND_TYPE_TEMP, 0, 0x70),
            LIFT_I(&i[18], 1, 1, 0, 1, 0, 1, 2, 0),
            LIFT_I(&i[18], 2, 0, 0, 1, 0, 1, 2, 0));
  EX_MATCH3(19, USIL_OP_ADD,
            LIFT_D(&i[19], 0, OPERAND_TYPE_TEMP, 0, 0x70),
            LIFT_S(&i[19], 1, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 2, 0,
                   false, false),
            LIFT_I(&i[19], 2, 2, 0, 1, 0, 1, 2, 0));
  EX_MATCH3(20, USIL_OP_ADD,
            LIFT_D(&i[20], 0, OPERAND_TYPE_TEMP, 1, 0x70),
            lift_negated_geometry_input(&i[20].operands[1], 0, 0, 1,
                                        2, 0, 1, 2),
            LIFT_I(&i[20], 2, 1, 0, 1, 2, 0, 1, 2));
  EX_MATCH3(21, USIL_OP_ADD,
            LIFT_D(&i[21], 0, OPERAND_TYPE_TEMP, 2, 0x70),
            lift_negated_geometry_input(&i[21].operands[1], 0, 0, 1,
                                        1, 2, 0, 1),
            LIFT_I(&i[21], 2, 2, 0, 1, 1, 2, 0, 1));
  EX_MATCH3(22, USIL_OP_MUL,
            LIFT_D(&i[22], 0, OPERAND_TYPE_TEMP, 3, 0x70),
            LIFT_S(&i[22], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0,
                   false, false),
            LIFT_S(&i[22], 2, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 0,
                   false, false));
  EX_MATCH4(23, USIL_OP_MAD,
            LIFT_D(&i[23], 0, OPERAND_TYPE_TEMP, 1, 0x70),
            LIFT_S(&i[23], 1, OPERAND_TYPE_TEMP, 1, 1, 2, 0, 1, 2,
                   false, false),
            LIFT_S(&i[23], 2, OPERAND_TYPE_TEMP, 2, 1, 1, 2, 0, 1,
                   false, false),
            LIFT_S(&i[23], 3, OPERAND_TYPE_TEMP, 3, 1, 0, 1, 2, 0,
                   true, false));
  EX_MATCH3(24, USIL_OP_DP3,
            LIFT_D(&i[24], 0, OPERAND_TYPE_TEMP, 0, 0x80),
            LIFT_S(&i[24], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0,
                   false, false),
            LIFT_S(&i[24], 2, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0,
                   false, false));
  EX_MATCH2(25, USIL_OP_RSQ,
            LIFT_D(&i[25], 0, OPERAND_TYPE_TEMP, 0, 0x80),
            LIFT_S(&i[25], 1, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3,
                   false, false));
  EX_MATCH3(26, USIL_OP_MUL,
            LIFT_D(&i[26], 0, OPERAND_TYPE_TEMP, 1, 0x70),
            LIFT_S(&i[26], 1, OPERAND_TYPE_TEMP, 0, 1, 3, 3, 3, 3,
                   false, false),
            LIFT_S(&i[26], 2, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0,
                   false, false));
  EX_MATCH3(27, USIL_OP_MUL,
            LIFT_D(&i[27], 0, OPERAND_TYPE_TEMP, 1, 0x70),
            LIFT_S(&i[27], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0,
                   false, false),
            LIFT_CB(&i[27], 2, 0, 2, 1, 0, 0, 0, 0, false));
  EX_MATCH4(28, USIL_OP_MAD,
            LIFT_D(&i[28], 0, OPERAND_TYPE_TEMP, 0, 0x70),
            LIFT_S(&i[28], 1, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 2, 0,
                   false, false),
            lift_immediate32(&i[28].operands[2], one_third, 4),
            LIFT_S(&i[28], 3, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0,
                   false, false));
  EX_MATCH2(29, USIL_OP_MOV,
            LIFT_D(&i[29], 0, OPERAND_TYPE_TEMP, 0, 0x80),
            lift_immediate32(&i[29].operands[1], zero, 1));
  if (!LIFT_OP(30, USIL_OP_LOOP, 0)) return false;
  EX_MATCH3(31, USIL_OP_IGE,
            LIFT_D(&i[31], 0, OPERAND_TYPE_TEMP, 1, 0x10),
            LIFT_S(&i[31], 1, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3,
                   false, false),
            lift_immediate32(&i[31].operands[2], three, 1));
  if (!lift_condition_instruction(&i[32], USIL_OP_BREAKC, 1,
                                  DXBC_INSTRUCTION_TEST_NONZERO) ||
      !LIFT_S(&i[32], 0, OPERAND_TYPE_TEMP, 1, 2, 0, 0, 0, 0,
              false, false)) return false;
  EX_MATCH3(33, USIL_OP_ADD,
            LIFT_D(&i[33], 0, OPERAND_TYPE_TEMP, 1, 0x70),
            LIFT_S(&i[33], 1, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 2, 0,
                   true, false),
            lift_dynamic_geometry_input(&i[33].operands[2], 3, 0, 1,
                                        0, 1, 2, 0));
  EX_MATCH4(34, USIL_OP_MAD,
            LIFT_D(&i[34], 0, OPERAND_TYPE_TEMP, 1, 0x70),
            LIFT_S(&i[34], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 0,
                   false, false),
            lift_immediate32(&i[34].operands[2], one_half, 4),
            LIFT_S(&i[34], 3, OPERAND_TYPE_TEMP, 0, 1, 0, 1, 2, 0,
                   false, false));
  EX_MATCH3(35, USIL_OP_MUL,
            LIFT_D(&i[35], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_S(&i[35], 1, OPERAND_TYPE_TEMP, 1, 1, 1, 1, 1, 1,
                   false, false),
            LIFT_CB(&i[35], 2, 1, 1, 1, 0, 1, 2, 3, false));
  EX_MATCH4(36, USIL_OP_MAD,
            LIFT_D(&i[36], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_CB(&i[36], 1, 1, 0, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[36], 2, OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0, 0,
                   false, false),
            LIFT_S(&i[36], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH4(37, USIL_OP_MAD,
            LIFT_D(&i[37], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_CB(&i[37], 1, 1, 2, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[37], 2, OPERAND_TYPE_TEMP, 1, 1, 2, 2, 2, 2,
                   false, false),
            LIFT_S(&i[37], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH3(38, USIL_OP_ADD,
            LIFT_D(&i[38], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_S(&i[38], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 3,
                   false, false),
            LIFT_CB(&i[38], 2, 1, 3, 1, 0, 1, 2, 3, false));
  EX_MATCH3(39, USIL_OP_MUL,
            LIFT_D(&i[39], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_S(&i[39], 1, OPERAND_TYPE_TEMP, 1, 1, 1, 1, 1, 1,
                   false, false),
            LIFT_CB(&i[39], 2, 2, 18, 1, 0, 1, 2, 3, false));
  EX_MATCH4(40, USIL_OP_MAD,
            LIFT_D(&i[40], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_CB(&i[40], 1, 2, 17, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[40], 2, OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0, 0,
                   false, false),
            LIFT_S(&i[40], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH4(41, USIL_OP_MAD,
            LIFT_D(&i[41], 0, OPERAND_TYPE_TEMP, 2, 0xf0),
            LIFT_CB(&i[41], 1, 2, 19, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[41], 2, OPERAND_TYPE_TEMP, 1, 1, 2, 2, 2, 2,
                   false, false),
            LIFT_S(&i[41], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH4(42, USIL_OP_MAD,
            LIFT_D(&i[42], 0, OPERAND_TYPE_TEMP, 1, 0xf0),
            LIFT_CB(&i[42], 1, 2, 20, 1, 0, 1, 2, 3, false),
            LIFT_S(&i[42], 2, OPERAND_TYPE_TEMP, 1, 1, 3, 3, 3, 3,
                   false, false),
            LIFT_S(&i[42], 3, OPERAND_TYPE_TEMP, 2, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH2(43, USIL_OP_MOV,
            LIFT_D(&i[43], 0, OPERAND_TYPE_OUTPUT, 0, 0xf0),
            LIFT_S(&i[43], 1, OPERAND_TYPE_TEMP, 1, 1, 0, 1, 2, 3,
                   false, false));
  EX_MATCH2(44, USIL_OP_MOV,
            LIFT_D(&i[44], 0, OPERAND_TYPE_OUTPUT, 1, 0xf0),
            lift_immediate32(&i[44].operands[1], red, 4));
  if (!lift_geometry_stream_effect(&i[45], USIL_OP_GEOMETRY_APPEND,
                                   USIL_GEOMETRY_EFFECT_APPEND)) return false;
  EX_MATCH3(46, USIL_OP_IADD,
            LIFT_D(&i[46], 0, OPERAND_TYPE_TEMP, 0, 0x80),
            LIFT_S(&i[46], 1, OPERAND_TYPE_TEMP, 0, 2, 3, 3, 3, 3,
                   false, false),
            lift_immediate32(&i[46].operands[2], one, 1));
  if (!LIFT_OP(47, USIL_OP_ENDLOOP, 0) ||
      !lift_geometry_stream_effect(&i[48],
                                   USIL_OP_GEOMETRY_RESTART_STRIP,
                                   USIL_GEOMETRY_EFFECT_RESTART_STRIP) ||
      !LIFT_OP(49, USIL_OP_RET, 0)) return false;

  for (int instruction = 0; instruction < 50; ++instruction) {
    if (i[instruction].source_instruction_index !=
        (uint32_t)(instruction + 13)) return false;
  }
#undef EX_MATCH2
#undef EX_MATCH3
#undef EX_MATCH4
  return true;
}

#undef LIFT_D
#undef LIFT_S
#undef LIFT_I
#undef LIFT_CB
#undef LIFT_OP

static bool emit_triangle_edge_distance_lift(HLSLEmitterContext* ctx) {
  if (!ctx || !hlsl_triangle_edge_distance_lift_matches(ctx->program))
    return false;
  DXBCOperand thickness = ctx->program->instructions[21].operands[1];
  thickness.has_neg = false;
  thickness.has_abs = false;
  char thickness_expression[512];
  ctx->current_instruction_index = 21;
  format_operand_hlsl(ctx, &thickness, false, false, 0x10, false,
                      thickness_expression, sizeof(thickness_expression));
  if (!sb_ok(ctx->sb) || thickness_expression[0] == '\0') return false;

  StringBuilder* sb = ctx->sb;
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "float2 dxbc_p0 = input[0].v0.xy / input[0].v0.w;\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "float2 dxbc_p1 = input[1].v0.xy / input[1].v0.w;\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "float2 dxbc_p2 = input[2].v0.xy / input[2].v0.w;\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "float2 dxbc_edge0 = dxbc_p2 - dxbc_p1;\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "float2 dxbc_edge1 = dxbc_p2 - dxbc_p0;\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "float2 dxbc_edge2 = dxbc_p1 - dxbc_p0;\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "float dxbc_area = abs(dxbc_edge1.x * dxbc_edge2.y - "
                "dxbc_edge1.y * dxbc_edge2.x);\n");
  sb_append_spaces(sb, ctx->indent);
  sb_appendf(sb, "float dxbc_thickness = 800.0f - %s;\n\n",
             thickness_expression);

  static const char* const distances[3] = {
      "float3(dxbc_area / length(dxbc_edge0), 0.0f, 0.0f)",
      "float3(0.0f, dxbc_area / length(dxbc_edge1), 0.0f)",
      "float3(0.0f, 0.0f, dxbc_area / length(dxbc_edge2))",
  };
  for (int vertex = 0; vertex < 3; ++vertex) {
    sb_append_spaces(sb, ctx->indent);
    sb_appendf(sb, "output.o1 = input[%d].v1;\n", vertex);
    sb_append_spaces(sb, ctx->indent);
    sb_appendf(sb, "output.o0 = input[%d].v0;\n", vertex);
    sb_append_spaces(sb, ctx->indent);
    sb_appendf(sb, "output.o2.xyz = %s * output.o0.w * "
                   "dxbc_thickness;\n", distances[vertex]);
    sb_append_spaces(sb, ctx->indent);
    sb_append(sb, "output.o2.w = 1.0f / output.o0.w;\n");
    sb_append_spaces(sb, ctx->indent);
    sb_append(sb, "dxbc_stream.Append(output);\n");
    if (vertex != 2) sb_append_char(sb, '\n');
  }
  return sb_ok(sb);
}

static bool emit_extruded_triangle_lift(HLSLEmitterContext* ctx) {
  if (!ctx || !hlsl_extruded_triangle_lift_matches(ctx->program))
    return false;

  DXBCOperand extrusion = ctx->program->instructions[27].operands[2];
  extrusion.has_neg = false;
  extrusion.has_abs = false;
  char extrusion_expression[512];
  ctx->current_instruction_index = 27;
  format_operand_hlsl(ctx, &extrusion, false, false, 0x10, false,
                      extrusion_expression, sizeof(extrusion_expression));
  if (!sb_ok(ctx->sb) || extrusion_expression[0] == '\0') return false;

  StringBuilder* sb = ctx->sb;
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "for (int dxbc_i = 0; dxbc_i < 3; ++dxbc_i) {\n");
  sb_append_spaces(sb, ctx->indent + 4);
  sb_append(sb,
            "output.o0 = mul(UNITY_MATRIX_VP, "
            "mul(unity_ObjectToWorld, "
            "float4(input[dxbc_i].v0.xyz, 1.0f)));\n");
  sb_append_spaces(sb, ctx->indent + 4);
  sb_append(sb, "output.o1 = float4(1.0f, 1.0f, 1.0f, 1.0f);\n");
  sb_append_spaces(sb, ctx->indent + 4);
  sb_append(sb, "dxbc_stream.Append(output);\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "}\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "dxbc_stream.RestartStrip();\n\n");

  sb_append_spaces(sb, ctx->indent);
  sb_append(sb,
            "float4 dxbc_center = (input[0].v0 + input[1].v0 + "
            "input[2].v0) / 3.0f;\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb,
            "float3 dxbc_face_normal = normalize(cross("
            "input[1].v0 - input[0].v0, "
            "input[2].v0 - input[0].v0));\n");
  sb_append_spaces(sb, ctx->indent);
  sb_appendf(sb, "dxbc_center.xyz += dxbc_face_normal * %s;\n\n",
             extrusion_expression);

  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "for (int dxbc_j = 0; dxbc_j < 3; ++dxbc_j) {\n");
  sb_append_spaces(sb, ctx->indent + 4);
  sb_append(sb,
            "float4 dxbc_position = dxbc_center + "
            "(input[dxbc_j].v0 - dxbc_center) * 0.5f;\n");
  sb_append_spaces(sb, ctx->indent + 4);
  sb_append(sb,
            "output.o0 = mul(UNITY_MATRIX_VP, "
            "mul(unity_ObjectToWorld, "
            "float4(dxbc_position.xyz, 1.0f)));\n");
  sb_append_spaces(sb, ctx->indent + 4);
  sb_append(sb, "output.o1 = float4(1.0f, 0.0f, 0.0f, 1.0f);\n");
  sb_append_spaces(sb, ctx->indent + 4);
  sb_append(sb, "dxbc_stream.Append(output);\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "}\n");
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "dxbc_stream.RestartStrip();\n");
  return sb_ok(sb);
}

typedef struct ExactRayBoxIntersectionLift {
  bool valid;
  int reciprocal_start;
  int reciprocal_end;
  int repeated_start[3];
  int repeated_end[3];
  int discard_start;
  int discard_end;
  int direction_register;
  int reciprocal_register;
  int initial_origin_register;
  int initial_box_register;
  int repeated_origin_register[3];
  bool volume_color_valid;
  int sample_position_mul[3];
  int sample_position_register[3];
  int sample_position_source_register[3];
  int sample_scale_cbuffer;
  int sample_scale_row;
  int volume_color_start;
  int volume_color_end;
  int volume_texture_register;
  int volume_sampler_register[3];
  int volume_color_register[3];
} ExactRayBoxIntersectionLift;

static bool ray_box_instruction(const USILProgram* program, int index,
                                USILOpcode opcode, int operand_count) {
  return program && index >= 0 && index < program->instruction_count &&
         lift_instruction(&program->instructions[index], opcode,
                          operand_count);
}

static bool ray_box_temp_destination(const DXBCOperand* operand, int reg,
                                     uint8_t mask) {
  return lift_register_destination(operand, OPERAND_TYPE_TEMP, reg, mask);
}

static bool ray_box_temp_source(const DXBCOperand* operand, int reg,
                                uint8_t mode, uint8_t x, uint8_t y,
                                uint8_t z, uint8_t w, bool negate,
                                bool absolute) {
  return lift_register_source(operand, OPERAND_TYPE_TEMP, reg, mode,
                              x, y, z, w, negate, absolute);
}

static bool ray_box_vector_source(const DXBCOperand* operand, int reg,
                                  bool negate, bool absolute) {
  return ray_box_temp_source(operand, reg, 1, 0, 1, 2, 0,
                             negate, absolute);
}

static bool ray_box_scalar_source(const DXBCOperand* operand, int reg,
                                  int lane, bool negate) {
  return lane >= 0 && lane < 4 &&
         ray_box_temp_source(operand, reg, 2, (uint8_t)lane,
                             (uint8_t)lane, (uint8_t)lane,
                             (uint8_t)lane, negate, false);
}

static bool ray_box_reduction(const USILProgram* program, int start,
                              int near_register, int far_register,
                              int result_register, int scalar_register,
                              int predicate_register) {
  static const uint32_t zero[1] = {0};
  const USILInstruction* i = program->instructions;
  return
      ray_box_instruction(program, start + 0, USIL_OP_MAX, 3) &&
      ray_box_temp_destination(&i[start + 0].operands[0], scalar_register,
                               0x80) &&
      ray_box_scalar_source(&i[start + 0].operands[1], near_register, 1,
                            false) &&
      ray_box_scalar_source(&i[start + 0].operands[2], near_register, 0,
                            false) &&
      ray_box_instruction(program, start + 1, USIL_OP_MAX, 3) &&
      ray_box_temp_destination(&i[start + 1].operands[0], result_register,
                               0x10) &&
      ray_box_scalar_source(&i[start + 1].operands[1], near_register, 2,
                            false) &&
      ray_box_scalar_source(&i[start + 1].operands[2], scalar_register, 3,
                            false) &&
      ray_box_instruction(program, start + 2, USIL_OP_MIN, 3) &&
      ray_box_temp_destination(&i[start + 2].operands[0], scalar_register,
                               0x80) &&
      ray_box_scalar_source(&i[start + 2].operands[1], far_register, 1,
                            false) &&
      ray_box_scalar_source(&i[start + 2].operands[2], far_register, 0,
                            false) &&
      ray_box_instruction(program, start + 3, USIL_OP_MIN, 3) &&
      ray_box_temp_destination(&i[start + 3].operands[0], result_register,
                               0x20) &&
      ray_box_scalar_source(&i[start + 3].operands[1], far_register, 2,
                            false) &&
      ray_box_scalar_source(&i[start + 3].operands[2], scalar_register, 3,
                            false) &&
      ray_box_instruction(program, start + 4, USIL_OP_LT, 3) &&
      ray_box_temp_destination(&i[start + 4].operands[0], scalar_register,
                               0x80) &&
      ray_box_scalar_source(&i[start + 4].operands[1], result_register, 1,
                            false) &&
      ray_box_scalar_source(&i[start + 4].operands[2], result_register, 0,
                            false) &&
      ray_box_instruction(program, start + 5, USIL_OP_LT, 3) &&
      ray_box_temp_destination(&i[start + 5].operands[0], predicate_register,
                               0x80) &&
      ray_box_scalar_source(&i[start + 5].operands[1], result_register, 1,
                            false) &&
      lift_immediate32(&i[start + 5].operands[2], zero, 1) &&
      ray_box_instruction(program, start + 6, USIL_OP_OR, 3) &&
      ray_box_temp_destination(&i[start + 6].operands[0], scalar_register,
                               0x80) &&
      ray_box_scalar_source(&i[start + 6].operands[1], scalar_register, 3,
                            false) &&
      ray_box_scalar_source(&i[start + 6].operands[2], predicate_register, 3,
                            false);
}

static bool ray_box_movc_pair(const USILProgram* program, int index,
                              int destination_register,
                              uint8_t destination_mask,
                              int scalar_register, int near_register,
                              const uint32_t immediate[4]) {
  const USILInstruction* inst = &program->instructions[index];
  const uint8_t y = destination_mask == 0xc0 ? 0 : 1;
  const uint8_t w = destination_mask == 0xc0 ? 1 : 0;
  return ray_box_instruction(program, index, USIL_OP_MOVC, 4) &&
         ray_box_temp_destination(&inst->operands[0], destination_register,
                                  destination_mask) &&
         ray_box_temp_source(&inst->operands[1], scalar_register, 1,
                             3, 3, 3, 3, false, false) &&
         lift_immediate32(&inst->operands[2], immediate, 4) &&
         ray_box_temp_source(&inst->operands[3], near_register, 1,
                             0, y, 0, w, false, false);
}

static bool ray_box_match_initial(const USILProgram* program, int start,
                                  ExactRayBoxIntersectionLift* lift) {
  static const uint32_t one4[4] = {
      UINT32_C(0x3f800000), UINT32_C(0x3f800000),
      UINT32_C(0x3f800000), UINT32_C(0x3f800000)};
  static const uint32_t minus_one_xy[4] = {
      UINT32_C(0xbf800000), UINT32_C(0xbf800000), 0, 0};
  if (!program || !lift || start < 0 ||
      start + 12 >= program->instruction_count) return false;
  const USILInstruction* i = program->instructions;
  if (!ray_box_instruction(program, start, USIL_OP_DIV, 3) ||
      i[start].operands[0].type != OPERAND_TYPE_TEMP ||
      i[start].operands[2].type != OPERAND_TYPE_TEMP) return false;
  const int reciprocal = i[start].operands[0].register_index;
  const int direction = i[start].operands[2].register_index;
  if (!ray_box_temp_destination(&i[start].operands[0], reciprocal, 0x70) ||
      !lift_immediate32(&i[start].operands[1], one4, 4) ||
      !ray_box_vector_source(&i[start].operands[2], direction, false,
                             false)) return false;

  if (!ray_box_instruction(program, start + 1, USIL_OP_MUL, 3) ||
      i[start + 1].operands[0].type != OPERAND_TYPE_TEMP ||
      i[start + 1].operands[1].type != OPERAND_TYPE_TEMP) return false;
  const int product = i[start + 1].operands[0].register_index;
  const int origin = i[start + 1].operands[1].register_index;
  if (!ray_box_temp_destination(&i[start + 1].operands[0], product, 0x70) ||
      !ray_box_vector_source(&i[start + 1].operands[1], origin, false,
                             false) ||
      !ray_box_vector_source(&i[start + 1].operands[2], reciprocal, false,
                             false)) return false;

  if (!ray_box_instruction(program, start + 2, USIL_OP_MUL, 3) ||
      i[start + 2].operands[0].type != OPERAND_TYPE_TEMP ||
      i[start + 2].operands[1].type != OPERAND_TYPE_TEMP) return false;
  const int near_register = i[start + 2].operands[0].register_index;
  const int box = i[start + 2].operands[1].register_index;
  if (!ray_box_temp_destination(&i[start + 2].operands[0], near_register,
                                0x70) ||
      !ray_box_vector_source(&i[start + 2].operands[1], box, false, false) ||
      !ray_box_vector_source(&i[start + 2].operands[2], reciprocal, false,
                             true)) return false;

  if (!ray_box_instruction(program, start + 3, USIL_OP_MAD, 4) ||
      !ray_box_temp_destination(&i[start + 3].operands[0], near_register,
                                0x70) ||
      !ray_box_vector_source(&i[start + 3].operands[1], reciprocal, true,
                             false) ||
      !ray_box_vector_source(&i[start + 3].operands[2], origin, false,
                             false) ||
      !ray_box_vector_source(&i[start + 3].operands[3], near_register, true,
                             false) ||
      !ray_box_instruction(program, start + 4, USIL_OP_MAD, 4) ||
      !ray_box_temp_destination(&i[start + 4].operands[0], box, 0x70) ||
      !ray_box_vector_source(&i[start + 4].operands[1], reciprocal, false,
                             true) ||
      !ray_box_vector_source(&i[start + 4].operands[2], box, false, false) ||
      !ray_box_vector_source(&i[start + 4].operands[3], product, true,
                             false)) return false;

  const int scalar = i[start + 5].operands[0].register_index;
  const int predicate = i[start + 10].operands[0].register_index;
  if (!ray_box_reduction(program, start + 5, near_register, box, product,
                         scalar, predicate) ||
      !ray_box_movc_pair(program, start + 12, box, 0x30, scalar, product,
                         minus_one_xy)) return false;
  lift->reciprocal_start = start;
  lift->reciprocal_end = start + 12;
  lift->direction_register = direction;
  lift->reciprocal_register = reciprocal;
  lift->initial_origin_register = origin;
  lift->initial_box_register = box;
  return true;
}

static bool ray_box_match_repeated(const USILProgram* program, int start,
                                   ExactRayBoxIntersectionLift* lift) {
  static const uint32_t box4[4] = {
      UINT32_C(0x3f004189), UINT32_C(0x3f004189),
      UINT32_C(0x3f004189), 0};
  static const uint32_t minus_one_xy[4] = {
      UINT32_C(0xbf800000), UINT32_C(0xbf800000), 0, 0};
  static const uint32_t minus_one_zw[4] = {
      0, 0, UINT32_C(0xbf800000), UINT32_C(0xbf800000)};
  if (!program || !lift || start < 0 || start + 42 >=
      program->instruction_count) return false;
  const USILInstruction* i = program->instructions;
  const int reciprocal = lift->reciprocal_register;

  if (!ray_box_instruction(program, start, USIL_OP_MUL, 3) ||
      i[start].operands[0].type != OPERAND_TYPE_TEMP ||
      i[start].operands[2].type != OPERAND_TYPE_TEMP) return false;
  const int packed = i[start].operands[0].register_index;
  const int origin0 = i[start].operands[2].register_index;
  if (!ray_box_temp_destination(&i[start].operands[0], packed, 0x70) ||
      !ray_box_vector_source(&i[start].operands[1], reciprocal, false,
                             false) ||
      !ray_box_vector_source(&i[start].operands[2], origin0, false, false) ||
      !ray_box_instruction(program, start + 1, USIL_OP_MUL, 3) ||
      i[start + 1].operands[0].type != OPERAND_TYPE_TEMP) return false;
  const int shared_box = i[start + 1].operands[0].register_index;
  if (!ray_box_temp_destination(&i[start + 1].operands[0], shared_box,
                                0x70) ||
      !ray_box_vector_source(&i[start + 1].operands[1], reciprocal, false,
                             true) ||
      !lift_immediate32(&i[start + 1].operands[2], box4, 4) ||
      !ray_box_instruction(program, start + 2, USIL_OP_MAD, 4) ||
      i[start + 2].operands[0].type != OPERAND_TYPE_TEMP) return false;
  const int near0 = i[start + 2].operands[0].register_index;
  if (!ray_box_temp_destination(&i[start + 2].operands[0], near0, 0x70) ||
      !ray_box_vector_source(&i[start + 2].operands[1], reciprocal, true,
                             false) ||
      !ray_box_vector_source(&i[start + 2].operands[2], origin0, false,
                             false) ||
      !ray_box_vector_source(&i[start + 2].operands[3], shared_box, true,
                             false) ||
      !ray_box_instruction(program, start + 3, USIL_OP_MAD, 4) ||
      !ray_box_temp_destination(&i[start + 3].operands[0], packed, 0x70) ||
      !ray_box_vector_source(&i[start + 3].operands[1], reciprocal, false,
                             true) ||
      !lift_immediate32(&i[start + 3].operands[2], box4, 4) ||
      !ray_box_vector_source(&i[start + 3].operands[3], packed, true,
                             false)) return false;
  const int scalar = i[start + 4].operands[0].register_index;
  const int predicate = i[start + 9].operands[0].register_index;
  if (!ray_box_reduction(program, start + 4, near0, packed, near0, scalar,
                         predicate) ||
      !ray_box_movc_pair(program, start + 11, packed, 0x30, scalar, near0,
                         minus_one_xy)) return false;

  const int start1 = start + 12;
  if (!ray_box_instruction(program, start1, USIL_OP_MUL, 3) ||
      i[start1].operands[0].type != OPERAND_TYPE_TEMP ||
      i[start1].operands[2].type != OPERAND_TYPE_TEMP) return false;
  const int far1 = i[start1].operands[0].register_index;
  const int origin1 = i[start1].operands[2].register_index;
  if (!ray_box_temp_destination(&i[start1].operands[0], far1, 0x70) ||
      !ray_box_vector_source(&i[start1].operands[1], reciprocal, false,
                             false) ||
      !ray_box_vector_source(&i[start1].operands[2], origin1, false, false) ||
      !ray_box_instruction(program, start1 + 1, USIL_OP_MAD, 4) ||
      i[start1 + 1].operands[0].type != OPERAND_TYPE_TEMP) return false;
  const int near1 = i[start1 + 1].operands[0].register_index;
  if (!ray_box_temp_destination(&i[start1 + 1].operands[0], near1, 0x70) ||
      !ray_box_vector_source(&i[start1 + 1].operands[1], reciprocal, true,
                             false) ||
      !ray_box_vector_source(&i[start1 + 1].operands[2], origin1, false,
                             false) ||
      !ray_box_vector_source(&i[start1 + 1].operands[3], shared_box, true,
                             false) ||
      !ray_box_instruction(program, start1 + 2, USIL_OP_MAD, 4) ||
      !ray_box_temp_destination(&i[start1 + 2].operands[0], far1, 0x70) ||
      !ray_box_vector_source(&i[start1 + 2].operands[1], reciprocal, false,
                             true) ||
      !lift_immediate32(&i[start1 + 2].operands[2], box4, 4) ||
      !ray_box_vector_source(&i[start1 + 2].operands[3], far1, true,
                             false) ||
      !ray_box_reduction(program, start1 + 3, near1, far1, near1, scalar,
                         predicate) ||
      !ray_box_movc_pair(program, start1 + 10, packed, 0xc0, scalar, near1,
                         minus_one_zw)) return false;

  const int start2 = start1 + 11;
  if (!ray_box_instruction(program, start2, USIL_OP_MUL, 3) ||
      i[start2].operands[0].type != OPERAND_TYPE_TEMP ||
      i[start2].operands[1].type != OPERAND_TYPE_TEMP) return false;
  const int far2 = i[start2].operands[0].register_index;
  const int origin2 = i[start2].operands[1].register_index;
  if (far2 != far1 ||
      !ray_box_temp_destination(&i[start2].operands[0], far2, 0x70) ||
      !ray_box_vector_source(&i[start2].operands[1], origin2, false, false) ||
      !ray_box_vector_source(&i[start2].operands[2], reciprocal, false,
                             false) ||
      !ray_box_instruction(program, start2 + 1, USIL_OP_MAD, 4) ||
      !ray_box_temp_destination(&i[start2 + 1].operands[0], shared_box,
                                0x70) ||
      !ray_box_vector_source(&i[start2 + 1].operands[1], reciprocal, true,
                             false) ||
      !ray_box_vector_source(&i[start2 + 1].operands[2], origin2, false,
                             false) ||
      !ray_box_vector_source(&i[start2 + 1].operands[3], shared_box, true,
                             false) ||
      !ray_box_instruction(program, start2 + 2, USIL_OP_MAD, 4) ||
      !ray_box_temp_destination(&i[start2 + 2].operands[0], reciprocal,
                                0x70) ||
      !ray_box_vector_source(&i[start2 + 2].operands[1], reciprocal, false,
                             true) ||
      !lift_immediate32(&i[start2 + 2].operands[2], box4, 4) ||
      !ray_box_vector_source(&i[start2 + 2].operands[3], far2, true,
                             false) ||
      !ray_box_reduction(program, start2 + 3, shared_box, reciprocal,
                         shared_box, scalar, predicate) ||
      !ray_box_movc_pair(program, start2 + 10, reciprocal, 0x30, scalar,
                         shared_box, minus_one_xy)) return false;

  const int discard = start2 + 11;
  static const uint32_t zero4[4] = {0, 0, 0, 0};
  if (!ray_box_instruction(program, discard + 0, USIL_OP_LT, 3) ||
      !ray_box_temp_destination(&i[discard + 0].operands[0], reciprocal,
                                0xc0) ||
      !ray_box_temp_source(&i[discard + 0].operands[1], packed, 1,
                           0, 0, 0, 1, false, false) ||
      !lift_immediate32(&i[discard + 0].operands[2], zero4, 4) ||
      !ray_box_instruction(program, discard + 1, USIL_OP_AND, 3) ||
      !ray_box_temp_destination(&i[discard + 1].operands[0], scalar, 0x80) ||
      !ray_box_scalar_source(&i[discard + 1].operands[1], reciprocal, 3,
                             false) ||
      !ray_box_scalar_source(&i[discard + 1].operands[2], reciprocal, 2,
                             false) ||
      !ray_box_instruction(program, discard + 2, USIL_OP_LT, 3) ||
      !ray_box_temp_destination(&i[discard + 2].operands[0], reciprocal,
                                0xc0) ||
      !ray_box_temp_source(&i[discard + 2].operands[1], packed, 1,
                           2, 2, 2, 3, false, false) ||
      !lift_immediate32(&i[discard + 2].operands[2], zero4, 4) ||
      !ray_box_instruction(program, discard + 3, USIL_OP_AND, 3) ||
      !ray_box_temp_destination(&i[discard + 3].operands[0], scalar, 0x80) ||
      !ray_box_scalar_source(&i[discard + 3].operands[1], scalar, 3, false) ||
      !ray_box_scalar_source(&i[discard + 3].operands[2], reciprocal, 2,
                             false) ||
      !ray_box_instruction(program, discard + 4, USIL_OP_AND, 3) ||
      !ray_box_temp_destination(&i[discard + 4].operands[0], scalar, 0x80) ||
      !ray_box_scalar_source(&i[discard + 4].operands[1], reciprocal, 3,
                             false) ||
      !ray_box_scalar_source(&i[discard + 4].operands[2], scalar, 3, false) ||
      !ray_box_instruction(program, discard + 5, USIL_OP_LT, 3) ||
      !ray_box_temp_destination(&i[discard + 5].operands[0], reciprocal,
                                0x30) ||
      !ray_box_temp_source(&i[discard + 5].operands[1], reciprocal, 1,
                           0, 1, 0, 0, false, false) ||
      !lift_immediate32(&i[discard + 5].operands[2], zero4, 4) ||
      !ray_box_instruction(program, discard + 6, USIL_OP_AND, 3) ||
      !ray_box_temp_destination(&i[discard + 6].operands[0], scalar, 0x80) ||
      !ray_box_scalar_source(&i[discard + 6].operands[1], scalar, 3, false) ||
      !ray_box_scalar_source(&i[discard + 6].operands[2], reciprocal, 0,
                             false) ||
      !ray_box_instruction(program, discard + 7, USIL_OP_AND, 3) ||
      !ray_box_temp_destination(&i[discard + 7].operands[0], scalar, 0x80) ||
      !ray_box_scalar_source(&i[discard + 7].operands[1], reciprocal, 1,
                             false) ||
      !ray_box_scalar_source(&i[discard + 7].operands[2], scalar, 3, false)) {
    return false;
  }
  const USILInstruction* discard_instruction = &i[discard + 8];
  if (!discard_instruction || discard_instruction->opcode != USIL_OP_DISCARD ||
      discard_instruction->operand_count != 1 || discard_instruction->saturate ||
      discard_instruction->condition_test != DXBC_INSTRUCTION_TEST_NONZERO ||
      discard_instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE ||
      !ray_box_scalar_source(&discard_instruction->operands[0], scalar, 3,
                             false)) return false;

  lift->repeated_start[0] = start;
  lift->repeated_end[0] = start + 11;
  lift->repeated_start[1] = start1;
  lift->repeated_end[1] = start1 + 10;
  lift->repeated_start[2] = start2;
  lift->repeated_end[2] = start2 + 10;
  lift->discard_start = discard;
  lift->discard_end = discard + 8;
  lift->repeated_origin_register[0] = origin0;
  lift->repeated_origin_register[1] = origin1;
  lift->repeated_origin_register[2] = origin2;
  return true;
}

static bool ray_box_intersection_lift(const USILProgram* program,
                                      ExactRayBoxIntersectionLift* output) {
  ExactRayBoxIntersectionLift found;
  bool has_found = false;
  if (!program || !output || program->program_type != DXBC_PROGRAM_TYPE_PIXEL ||
      program->shader_model_major != 4 || program->shader_model_minor != 0 ||
      !program->instructions || program->instruction_count < 56) return false;
  memset(&found, 0, sizeof(found));
  for (int initial = 0; initial + 12 < program->instruction_count; ++initial) {
    ExactRayBoxIntersectionLift candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (!ray_box_match_initial(program, initial, &candidate)) continue;
    for (int repeated = initial + 13;
         repeated + 42 < program->instruction_count; ++repeated) {
      ExactRayBoxIntersectionLift completed = candidate;
      if (!ray_box_match_repeated(program, repeated, &completed)) continue;
      bool reciprocal_overwritten = false;
      for (int index = candidate.reciprocal_end + 1; index < repeated;
           ++index) {
        const USILInstruction* instruction = &program->instructions[index];
        if (instruction->operand_count < 1 ||
            instruction->operands[0].type != OPERAND_TYPE_TEMP ||
            instruction->operands[0].register_index !=
                candidate.reciprocal_register) continue;
        if ((instruction->operands[0].destination_mask & 0x70) != 0) {
          reciprocal_overwritten = true;
          break;
        }
      }
      if (reciprocal_overwritten || has_found) return false;
      completed.valid = true;
      found = completed;
      has_found = true;
    }
  }
  if (!has_found) return false;
  *output = found;
  return true;
}

static bool volume_flow_instruction(const USILProgram* program, int index,
                                    USILOpcode opcode, int operand_count,
                                    DXBCInstructionTest test) {
  if (!program || index < 0 || index >= program->instruction_count)
    return false;
  const USILInstruction* instruction = &program->instructions[index];
  return instruction->opcode == opcode &&
         instruction->operand_count == operand_count &&
         !instruction->saturate && instruction->condition_test == test &&
         instruction->geometry_effect == USIL_GEOMETRY_EFFECT_NONE &&
         instruction->geometry_stream_id == 0 &&
         !instruction->geometry_stream_explicit &&
         !instruction->has_resource_dimension &&
         !instruction->has_texel_offset &&
         !instruction->has_resource_return_types &&
         instruction->resource_stride == 0;
}

static bool volume_sample_instruction(const USILProgram* program, int index) {
  if (!program || index < 0 || index >= program->instruction_count)
    return false;
  const USILInstruction* instruction = &program->instructions[index];
  return instruction->opcode == USIL_OP_SAMPLE &&
         instruction->operand_count == 4 && !instruction->saturate &&
         instruction->condition_test == DXBC_INSTRUCTION_TEST_NONE &&
         instruction->geometry_effect == USIL_GEOMETRY_EFFECT_NONE &&
         instruction->geometry_stream_id == 0 &&
         !instruction->geometry_stream_explicit &&
         !instruction->has_texel_offset && instruction->resource_stride == 0 &&
         (!instruction->has_resource_dimension ||
          strcmp(instruction->resource_dimension, "3d") == 0);
}

static bool volume_binding_source(const DXBCOperand* operand,
                                  DXBCOperandType type, int reg,
                                  uint8_t x, uint8_t y, uint8_t z,
                                  uint8_t w) {
  return lift_register_source(operand, type, reg, 1, x, y, z, w,
                              false, false);
}

static bool volume_constant_scalar(const DXBCOperand* operand,
                                   int buffer, int row) {
  return lift_constant_buffer_source(operand, (uint32_t)buffer,
                                     (uint32_t)row, 2, 0, 0, 0, 0,
                                     false);
}

static bool volume_constant_vector(const DXBCOperand* operand,
                                   int buffer, int row) {
  return lift_constant_buffer_source(operand, (uint32_t)buffer,
                                     (uint32_t)row, 1, 0, 1, 2, 0,
                                     false);
}

static bool volume_match_eq(const USILProgram* program, int index,
                            int predicate_register, int buffer, int row,
                            uint32_t literal) {
  const USILInstruction* instruction = &program->instructions[index];
  return ray_box_instruction(program, index, USIL_OP_EQ, 3) &&
         ray_box_temp_destination(&instruction->operands[0],
                                  predicate_register, 0x80) &&
         volume_constant_scalar(&instruction->operands[1], buffer, row) &&
         lift_immediate32(&instruction->operands[2], &literal, 1);
}

static bool volume_match_position_multiply(
    const USILProgram* program, int before, int scaled_register,
    int* out_index, int* out_source_register, int* inout_buffer,
    int* inout_row) {
  if (!program || !out_index || !out_source_register || !inout_buffer ||
      !inout_row || before <= 0) return false;
  int definition = -1;
  for (int index = 0; index < before; ++index) {
    const USILInstruction* instruction = &program->instructions[index];
    if (instruction->operand_count < 1 ||
        instruction->operands[0].type != OPERAND_TYPE_TEMP ||
        instruction->operands[0].register_index != scaled_register ||
        (instruction->operands[0].destination_mask & 0x70) == 0) continue;
    definition = index;
  }
  if (definition < 0 ||
      !ray_box_instruction(program, definition, USIL_OP_MUL, 3))
    return false;
  const USILInstruction* instruction = &program->instructions[definition];
  if (!ray_box_temp_destination(&instruction->operands[0], scaled_register,
                                0x70) ||
      instruction->operands[1].type != OPERAND_TYPE_TEMP ||
      instruction->operands[2].type != OPERAND_TYPE_CONSTANT_BUFFER)
    return false;
  const int source_register = instruction->operands[1].register_index;
  const int buffer = instruction->operands[2].register_index;
  if (instruction->operands[2].index_values[1] > INT_MAX) return false;
  const int row = (int)instruction->operands[2].index_values[1];
  if (!ray_box_vector_source(&instruction->operands[1], source_register,
                             false, false) ||
      !volume_constant_vector(&instruction->operands[2], buffer, row) ||
      (*inout_buffer >= 0 &&
       (*inout_buffer != buffer || *inout_row != row))) return false;
  *inout_buffer = buffer;
  *inout_row = row;
  *out_index = definition;
  *out_source_register = source_register;
  return true;
}

static const USILTexture* volume_texture(const USILProgram* program,
                                         int reg) {
  const USILTexture* result = NULL;
  if (!program) return NULL;
  for (int index = 0; index < program->texture_count; ++index) {
    if (program->textures[index].reg_idx != reg) continue;
    if (result) return NULL;
    result = &program->textures[index];
  }
  return result;
}

static const USILSampler* volume_sampler(const USILProgram* program,
                                         int reg) {
  const USILSampler* result = NULL;
  if (!program) return NULL;
  for (int index = 0; index < program->sampler_count; ++index) {
    if (program->samplers[index].reg_idx != reg) continue;
    if (result) return NULL;
    result = &program->samplers[index];
  }
  return result;
}

static bool volume_match_sample(const USILProgram* program, int index,
                                int destination_register,
                                int coordinate_register,
                                int* inout_texture_register,
                                int* inout_sampler_register) {
  if (!volume_sample_instruction(program, index)) return false;
  const USILInstruction* instruction = &program->instructions[index];
  if (!ray_box_temp_destination(&instruction->operands[0],
                                destination_register, 0xf0) ||
      !ray_box_vector_source(&instruction->operands[1], coordinate_register,
                             false, false) ||
      instruction->operands[2].type != OPERAND_TYPE_RESOURCE ||
      instruction->operands[3].type != OPERAND_TYPE_SAMPLER)
    return false;
  const int texture_register = instruction->operands[2].register_index;
  const int sampler_register = instruction->operands[3].register_index;
  if (!volume_binding_source(&instruction->operands[2],
                             OPERAND_TYPE_RESOURCE, texture_register,
                             2, 0, 1, 3) ||
      !lift_register_source(&instruction->operands[3],
                            OPERAND_TYPE_SAMPLER, sampler_register,
                            0, 0, 1, 2, 3, false, false) ||
      (*inout_texture_register >= 0 &&
       *inout_texture_register != texture_register) ||
      (*inout_sampler_register >= 0 &&
       *inout_sampler_register != sampler_register)) return false;
  const USILTexture* texture = volume_texture(program, texture_register);
  const USILSampler* sampler = volume_sampler(program, sampler_register);
  if (!texture || strcmp(texture->dimension, "3d") != 0 ||
      texture->return_types[0] != 5 || texture->return_types[1] != 5 ||
      texture->return_types[2] != 5 || texture->return_types[3] != 5 ||
      texture->sample_count != 0 || texture->stride != 0 || !sampler ||
      sampler->mode != 0) return false;
  *inout_texture_register = texture_register;
  *inout_sampler_register = sampler_register;
  return true;
}

static bool volume_match_pack_move(const USILProgram* program, int index,
                                   int pack_register,
                                   int sample_register) {
  const USILInstruction* instruction = &program->instructions[index];
  return ray_box_instruction(program, index, USIL_OP_MOV, 2) &&
         ray_box_temp_destination(&instruction->operands[0], pack_register,
                                  0xe0) &&
         ray_box_temp_source(&instruction->operands[1], sample_register, 1,
                             1, 1, 2, 3, false, false);
}

static bool volume_match_unpack_sequence(const USILProgram* program,
                                         int start, int pack_register,
                                         int normal_register,
                                         int scalar_register) {
  static const uint32_t two_xy[4] = {
      UINT32_C(0x40000000), UINT32_C(0x40000000), 0, 0};
  static const uint32_t minus_one_xy[4] = {
      UINT32_C(0xbf800000), UINT32_C(0xbf800000), 0, 0};
  static const uint32_t one[1] = {UINT32_C(0x3f800000)};
  static const uint32_t half_xyz[4] = {
      UINT32_C(0x3f000000), UINT32_C(0x3f000000),
      UINT32_C(0x3f000000), 0};
  const USILInstruction* i = program->instructions;
  return
      ray_box_instruction(program, start + 0, USIL_OP_MUL, 3) &&
      ray_box_temp_destination(&i[start + 0].operands[0], pack_register,
                               0x10) &&
      ray_box_scalar_source(&i[start + 0].operands[1], pack_register, 3,
                            false) &&
      ray_box_scalar_source(&i[start + 0].operands[2], pack_register, 1,
                            false) &&
      ray_box_instruction(program, start + 1, USIL_OP_MAD, 4) &&
      ray_box_temp_destination(&i[start + 1].operands[0], normal_register,
                               0x30) &&
      ray_box_temp_source(&i[start + 1].operands[1], pack_register, 1,
                          0, 2, 0, 0, false, false) &&
      lift_immediate32(&i[start + 1].operands[2], two_xy, 4) &&
      lift_immediate32(&i[start + 1].operands[3], minus_one_xy, 4) &&
      ray_box_instruction(program, start + 2, USIL_OP_DP2, 3) &&
      ray_box_temp_destination(&i[start + 2].operands[0], scalar_register,
                               0x80) &&
      ray_box_temp_source(&i[start + 2].operands[1], normal_register, 1,
                          0, 1, 0, 0, false, false) &&
      ray_box_temp_source(&i[start + 2].operands[2], normal_register, 1,
                          0, 1, 0, 0, false, false) &&
      ray_box_instruction(program, start + 3, USIL_OP_MIN, 3) &&
      ray_box_temp_destination(&i[start + 3].operands[0], scalar_register,
                               0x80) &&
      ray_box_scalar_source(&i[start + 3].operands[1], scalar_register, 3,
                            false) &&
      lift_immediate32(&i[start + 3].operands[2], one, 1) &&
      ray_box_instruction(program, start + 4, USIL_OP_ADD, 3) &&
      ray_box_temp_destination(&i[start + 4].operands[0], scalar_register,
                               0x80) &&
      ray_box_scalar_source(&i[start + 4].operands[1], scalar_register, 3,
                            true) &&
      lift_immediate32(&i[start + 4].operands[2], one, 1) &&
      ray_box_instruction(program, start + 5, USIL_OP_SQRT, 2) &&
      ray_box_temp_destination(&i[start + 5].operands[0], normal_register,
                               0x40) &&
      ray_box_scalar_source(&i[start + 5].operands[1], scalar_register, 3,
                            false) &&
      ray_box_instruction(program, start + 6, USIL_OP_MAD, 4) &&
      ray_box_temp_destination(&i[start + 6].operands[0], normal_register,
                               0x70) &&
      ray_box_vector_source(&i[start + 6].operands[1], normal_register,
                            false, false) &&
      lift_immediate32(&i[start + 6].operands[2], half_xyz, 4) &&
      lift_immediate32(&i[start + 6].operands[3], half_xyz, 4);
}

static bool volume_match_color_join(const USILProgram* program, int start,
                                    int normal_register,
                                    int sample_register, int pack_register,
                                    int color_register, int normal_buffer,
                                    int normal_row) {
  static const uint32_t one[1] = {UINT32_C(0x3f800000)};
  const USILInstruction* i = program->instructions;
  const USILInstruction* movc = &i[start + 2];
  return
      ray_box_instruction(program, start + 0, USIL_OP_MOV, 2) &&
      ray_box_temp_destination(&i[start + 0].operands[0], normal_register,
                               0x80) &&
      lift_immediate32(&i[start + 0].operands[1], one, 1) &&
      ray_box_instruction(program, start + 1, USIL_OP_MOV, 2) &&
      ray_box_temp_destination(&i[start + 1].operands[0], sample_register,
                               0xe0) &&
      ray_box_temp_source(&i[start + 1].operands[1], pack_register, 1,
                          1, 1, 2, 3, false, false) &&
      movc->opcode == USIL_OP_MOVC && movc->operand_count == 4 &&
      movc->saturate &&
      movc->condition_test == DXBC_INSTRUCTION_TEST_NONE &&
      movc->geometry_effect == USIL_GEOMETRY_EFFECT_NONE &&
      !movc->has_resource_dimension && !movc->has_texel_offset &&
      !movc->has_resource_return_types && movc->resource_stride == 0 &&
      ray_box_temp_destination(&movc->operands[0], color_register, 0xf0) &&
      lift_constant_buffer_source(&movc->operands[1],
                                  (uint32_t)normal_buffer,
                                  (uint32_t)normal_row,
                                  1, 0, 0, 0, 0, false) &&
      ray_box_temp_source(&movc->operands[2], normal_register, 1,
                          0, 1, 2, 3, false, false) &&
      ray_box_temp_source(&movc->operands[3], sample_register, 1,
                          1, 2, 0, 3, false, false);
}

static bool volume_slice_sampling_lift(
    const USILProgram* program, ExactRayBoxIntersectionLift* lift) {
  static const uint32_t sample_offset[4] = {
      UINT32_C(0x3f000011), UINT32_C(0x3f000011),
      UINT32_C(0x3f000011), 0};
  if (!program || !lift || !lift->valid ||
      lift->discard_end + 68 >= program->instruction_count) return false;

  int scale_buffer = -1;
  int scale_row = -1;
  for (int sample = 0; sample < 3; ++sample) {
    const int scaled_register = lift->repeated_origin_register[sample];
    if (!volume_match_position_multiply(
            program, lift->repeated_start[sample], scaled_register,
            &lift->sample_position_mul[sample],
            &lift->sample_position_source_register[sample],
            &scale_buffer, &scale_row)) {
      return false;
    }
    lift->sample_position_register[sample] = scaled_register;
  }
  if (!(lift->sample_position_mul[0] < lift->sample_position_mul[1] &&
        lift->sample_position_mul[1] < lift->sample_position_mul[2]))
    return false;

  const int start = lift->discard_end + 1;
  const USILInstruction* i = program->instructions;
  if (!ray_box_instruction(program, start, USIL_OP_EQ, 3) ||
      i[start].operands[0].type != OPERAND_TYPE_TEMP ||
      i[start].operands[1].type != OPERAND_TYPE_CONSTANT_BUFFER ||
      i[start].operands[1].index_values[1] > INT_MAX) return false;
  const int predicate_register = i[start].operands[0].register_index;
  const int filter_buffer = i[start].operands[1].register_index;
  const int filter_row = (int)i[start].operands[1].index_values[1];
  if (!volume_match_eq(program, start, predicate_register,
                       filter_buffer, filter_row, 0) ||
      !volume_flow_instruction(program, start + 1, USIL_OP_IF, 1,
                               DXBC_INSTRUCTION_TEST_NONZERO) ||
      !ray_box_scalar_source(&i[start + 1].operands[0],
                             predicate_register, 3, false) ||
      !volume_flow_instruction(program, start + 11, USIL_OP_ELSE, 0,
                               DXBC_INSTRUCTION_TEST_NONE) ||
      !volume_match_eq(program, start + 12, predicate_register,
                       filter_buffer, filter_row,
                       UINT32_C(0x3f800000)) ||
      !volume_flow_instruction(program, start + 13, USIL_OP_IF, 1,
                               DXBC_INSTRUCTION_TEST_NONZERO) ||
      !ray_box_scalar_source(&i[start + 13].operands[0],
                             predicate_register, 3, false) ||
      !volume_flow_instruction(program, start + 23, USIL_OP_ELSE, 0,
                               DXBC_INSTRUCTION_TEST_NONE) ||
      !volume_match_eq(program, start + 24, predicate_register,
                       filter_buffer, filter_row,
                       UINT32_C(0x40000000)) ||
      !volume_flow_instruction(program, start + 25, USIL_OP_IF, 1,
                               DXBC_INSTRUCTION_TEST_NONZERO) ||
      !ray_box_scalar_source(&i[start + 25].operands[0],
                             predicate_register, 3, false) ||
      !volume_flow_instruction(program, start + 35, USIL_OP_ENDIF, 0,
                               DXBC_INSTRUCTION_TEST_NONE) ||
      !volume_flow_instruction(program, start + 36, USIL_OP_ENDIF, 0,
                               DXBC_INSTRUCTION_TEST_NONE) ||
      !volume_flow_instruction(program, start + 37, USIL_OP_ENDIF, 0,
                               DXBC_INSTRUCTION_TEST_NONE)) {
    return false;
  }

  int texture_register = -1;
  int sampler_register[3] = {-1, -1, -1};
  int color_register[3] = {-1, -1, -1};
  int pack_register[3] = {-1, -1, -1};
  int shared_coordinate = -1;
  static const int branch_starts[3] = {2, 14, 26};
  static const int pack_starts[3] = {8, 20, 32};
  for (int branch = 0; branch < 3; ++branch) {
    for (int sample = 0; sample < 3; ++sample) {
      const int mad_index = start + branch_starts[branch] + sample * 2;
      const int sample_index = mad_index + 1;
      if (!ray_box_instruction(program, mad_index, USIL_OP_MAD, 4) ||
          i[mad_index].operands[0].type != OPERAND_TYPE_TEMP)
        return false;
      const int coordinate_register =
          i[mad_index].operands[0].register_index;
      if (!ray_box_temp_destination(&i[mad_index].operands[0],
                                    coordinate_register, 0x70) ||
          !ray_box_vector_source(&i[mad_index].operands[1],
                                 lift->sample_position_source_register[sample],
                                 false, false) ||
          !volume_constant_vector(&i[mad_index].operands[2], scale_buffer,
                                  scale_row) ||
          !lift_immediate32(&i[mad_index].operands[3], sample_offset, 4)) {
        return false;
      }
      if (branch == 0) {
        color_register[sample] = coordinate_register;
      } else if (branch == 1) {
        if (sample == 0) shared_coordinate = coordinate_register;
        if (coordinate_register != shared_coordinate) return false;
      } else {
        const int expected = sample < 2
            ? lift->sample_position_source_register[0]
            : lift->sample_position_source_register[2];
        if (coordinate_register != expected) return false;
      }
      if (!volume_match_sample(program, sample_index,
                               color_register[sample], coordinate_register,
                               &texture_register,
                               &sampler_register[branch])) {
        return false;
      }
    }
    for (int sample = 0; sample < 3; ++sample) {
      const int move_index = start + pack_starts[branch] + sample;
      if (branch == 0) {
        if (i[move_index].operands[0].type != OPERAND_TYPE_TEMP)
          return false;
        pack_register[sample] =
            i[move_index].operands[0].register_index;
      }
      if (!volume_match_pack_move(program, move_index,
                                  pack_register[sample],
                                  color_register[sample])) {
        return false;
      }
    }
  }
  if (sampler_register[0] == sampler_register[1] ||
      sampler_register[0] == sampler_register[2] ||
      sampler_register[1] == sampler_register[2]) return false;

  const int normal_start = start + 38;
  int normal_register[3] = {-1, -1, -1};
  int scalar_register[3] = {-1, -1, -1};
  for (int sample = 0; sample < 3; ++sample) {
    const int unpack = normal_start + sample * 7;
    if (i[unpack + 1].operands[0].type != OPERAND_TYPE_TEMP ||
        i[unpack + 2].operands[0].type != OPERAND_TYPE_TEMP)
      return false;
    normal_register[sample] = i[unpack + 1].operands[0].register_index;
    scalar_register[sample] = i[unpack + 2].operands[0].register_index;
    if (!volume_match_unpack_sequence(program, unpack,
                                      pack_register[sample],
                                      normal_register[sample],
                                      scalar_register[sample])) {
      return false;
    }
  }

  const int join_start = normal_start + 21;
  if (i[join_start + 2].operands[1].type !=
          OPERAND_TYPE_CONSTANT_BUFFER ||
      i[join_start + 2].operands[1].index_values[1] > INT_MAX)
    return false;
  const int normal_buffer = i[join_start + 2].operands[1].register_index;
  const int normal_row =
      (int)i[join_start + 2].operands[1].index_values[1];
  for (int sample = 0; sample < 3; ++sample) {
    const int join = join_start + sample * 3;
    if (i[join + 2].operands[0].type != OPERAND_TYPE_TEMP) return false;
    const int output_register = i[join + 2].operands[0].register_index;
    if (!volume_match_color_join(program, join, normal_register[sample],
                                 color_register[sample],
                                 pack_register[sample], output_register,
                                 normal_buffer, normal_row)) {
      return false;
    }
    lift->volume_color_register[sample] = output_register;
  }

  lift->sample_scale_cbuffer = scale_buffer;
  lift->sample_scale_row = scale_row;
  lift->volume_color_start = start;
  lift->volume_color_end = start + 67;
  lift->volume_texture_register = texture_register;
  for (int branch = 0; branch < 3; ++branch)
    lift->volume_sampler_register[branch] = sampler_register[branch];
  lift->volume_color_valid = true;
  return true;
}

bool hlsl_ray_box_intersection_lift_matches(const USILProgram* program) {
  ExactRayBoxIntersectionLift lift;
  memset(&lift, 0, sizeof(lift));
  return ray_box_intersection_lift(program, &lift);
}

bool hlsl_volume_slice_sampling_lift_matches(const USILProgram* program) {
  ExactRayBoxIntersectionLift lift;
  memset(&lift, 0, sizeof(lift));
  return ray_box_intersection_lift(program, &lift) &&
         volume_slice_sampling_lift(program, &lift);
}

typedef struct VolumeSliceEmission {
  char scale_expression[512];
  char filter_expression[512];
  char normal_map_expression[512];
  char texture_fallback[32];
  char sample_offset[64];
  const char* texture_name;
  const char* sampler_name[3];
} VolumeSliceEmission;

static bool prepare_volume_slice_emission(
    HLSLEmitterContext* ctx, const ExactRayBoxIntersectionLift* lift,
    VolumeSliceEmission* emission) {
  if (!ctx || !lift || !emission || !lift->volume_color_valid)
    return false;
  memset(emission, 0, sizeof(*emission));

  const USILInstruction* instructions = ctx->program->instructions;
  const USILInstruction* scale =
      &instructions[lift->sample_position_mul[0]];
  format_operand_hlsl(ctx, &scale->operands[2], false, false, 0x70, false,
                      emission->scale_expression,
                      sizeof(emission->scale_expression));

  const USILInstruction* filter =
      &instructions[lift->volume_color_start];
  format_operand_hlsl(ctx, &filter->operands[1], false, false, 0x10, false,
                      emission->filter_expression,
                      sizeof(emission->filter_expression));
  if (!sb_ok(ctx->sb) || emission->scale_expression[0] == '\0' ||
      emission->filter_expression[0] == '\0') return false;

  const USILInstruction* normal_join =
      &instructions[lift->volume_color_start + 61];
  const DXBCOperand* normal_condition = &normal_join->operands[1];
  format_operand_hlsl(ctx, normal_condition, true, false, 0x10, false,
                      emission->normal_map_expression,
                      sizeof(emission->normal_map_expression));
  if (!sb_ok(ctx->sb) || emission->normal_map_expression[0] == '\0')
    return false;

  if (!resolve_srv_name_ctx(ctx, lift->volume_texture_register,
                            SERIALIZED_RESOURCE_TEXTURE,
                            &emission->texture_name)) {
    return false;
  }
  if (!emission->texture_name) {
    if (!hlsl_format_checked(ctx, emission->texture_fallback,
                             sizeof(emission->texture_fallback), "t%d",
                             lift->volume_texture_register)) return false;
    emission->texture_name = emission->texture_fallback;
  }

  for (int branch = 0; branch < 3; ++branch) {
    const int sampler_register = lift->volume_sampler_register[branch];
    if (sampler_register < 0 ||
        sampler_register >= HLSL_SM5_SAMPLER_REGISTER_COUNT ||
        !ctx->sampler_names[sampler_register] ||
        ctx->sampler_names[sampler_register][0] == '\0') return false;
    emission->sampler_name[branch] =
        ctx->sampler_names[sampler_register];
  }

  const uint32_t offset_bits =
      instructions[lift->volume_color_start + 2]
          .operands[3].immediate_words[0];
  if (!format_float_bits_hlsl(offset_bits, emission->sample_offset,
                              sizeof(emission->sample_offset))) return false;
  return sb_ok(ctx->sb);
}

void emit_exact_structural_helpers(HLSLEmitterContext* ctx) {
  ExactRayBoxIntersectionLift lift;
  memset(&lift, 0, sizeof(lift));
  if (!ctx || !ctx->use_uint_temps ||
      !ray_box_intersection_lift(ctx->program, &lift)) return;
  sb_append(ctx->sb,
      "float2 dxbc_ray_box_intersection(float3 ro, float3 rd, "
      "float3 boxSize) {\n"
      "    float3 m = 1.0f / rd;\n"
      "    float3 n = m * ro;\n"
      "    float3 k = abs(m) * boxSize;\n"
      "    float3 t1 = -n - k;\n"
      "    float3 t2 = -n + k;\n"
      "    float tN = max(max(t1.x, t1.y), t1.z);\n"
      "    float tF = min(min(t2.x, t2.y), t2.z);\n"
      "    if (tN > tF || tF < 0.0f) return -1.0f;\n"
      "    return float2(tN, tF);\n"
      "}\n\n");
  if (!volume_slice_sampling_lift(ctx->program, &lift)) return;
  sb_append(ctx->sb,
      "float3 dxbc_unpack_normal(float4 packednormal) {\n"
      "    packednormal.x *= packednormal.w;\n"
      "    float3 normal;\n"
      "    normal.xy = packednormal.xy * 2.0f - 1.0f;\n"
      "    normal.z = sqrt(1.0f - saturate(dot(normal.xy, normal.xy)));\n"
      "    return normal;\n"
      "}\n\n");
}

static void emit_ray_box_comment_range(HLSLEmitterContext* ctx, int first,
                                       int last) {
  for (int index = first; index <= last; ++index) {
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb, "// ");
    sb_append(ctx->sb, ctx->program->instructions[index].original_asm);
    sb_append_char(ctx->sb, '\n');
  }
}

static bool emit_volume_sample_position(
    HLSLEmitterContext* ctx, const ExactRayBoxIntersectionLift* lift,
    const VolumeSliceEmission* emission, int sample) {
  if (!ctx || !lift || !emission || sample < 0 || sample >= 3)
    return false;
  const int index = lift->sample_position_mul[sample];
  sb_append_spaces(ctx->sb, ctx->indent);
  sb_append(ctx->sb, "// ");
  sb_append(ctx->sb, ctx->program->instructions[index].original_asm);
  sb_append_char(ctx->sb, '\n');
  sb_append_spaces(ctx->sb, ctx->indent);
  sb_appendf(ctx->sb,
             "float3 dxbc_sample_position%d = asfloat(r%d.xyz) * %s;\n",
             sample + 1, lift->sample_position_source_register[sample],
             emission->scale_expression);
  sb_append_spaces(ctx->sb, ctx->indent);
  sb_appendf(ctx->sb, "r%d.xyz = asuint(dxbc_sample_position%d);\n",
             lift->sample_position_register[sample], sample + 1);
  return sb_ok(ctx->sb);
}

static bool emit_volume_slice_sampling(
    HLSLEmitterContext* ctx, const ExactRayBoxIntersectionLift* lift,
    const VolumeSliceEmission* emission) {
  if (!ctx || !lift || !emission) return false;
  StringBuilder* sb = ctx->sb;
  for (int sample = 0; sample < 3; ++sample) {
    sb_append_spaces(sb, ctx->indent);
    sb_appendf(sb, "float4 color%d;\n", sample + 1);
  }
  for (int branch = 0; branch < 3; ++branch) {
    sb_append_spaces(sb, ctx->indent);
    if (branch == 0) {
      sb_appendf(sb, "if (%s == 0.0f) {\n",
                 emission->filter_expression);
    } else {
      sb_appendf(sb, "} else if (%s == %d.0f) {\n",
                 emission->filter_expression, branch);
    }
    for (int sample = 0; sample < 3; ++sample) {
      sb_append_spaces(sb, ctx->indent + 4);
      sb_appendf(
          sb,
          "color%d = %s.Sample(%s, dxbc_sample_position%d + "
          "float3(%s, %s, %s));\n",
          sample + 1, emission->texture_name,
          emission->sampler_name[branch], sample + 1,
          emission->sample_offset, emission->sample_offset,
          emission->sample_offset);
    }
  }
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "}\n");

  sb_append_spaces(sb, ctx->indent);
  sb_appendf(sb, "if (%s) {\n", emission->normal_map_expression);
  for (int sample = 0; sample < 3; ++sample) {
    sb_append_spaces(sb, ctx->indent + 4);
    sb_appendf(sb,
               "color%d.rgb = 0.5f + 0.5f * "
               "dxbc_unpack_normal(color%d);\n",
               sample + 1, sample + 1);
  }
  for (int sample = 0; sample < 3; ++sample) {
    sb_append_spaces(sb, ctx->indent + 4);
    sb_appendf(sb, "color%d.a = 1.0f;\n", sample + 1);
  }
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "}\n");
  for (int sample = 0; sample < 3; ++sample) {
    sb_append_spaces(sb, ctx->indent);
    sb_appendf(sb, "r%d = asuint(saturate(color%d));\n",
               lift->volume_color_register[sample], sample + 1);
  }
  return sb_ok(sb);
}

static bool emit_ray_box_intersection_lift(HLSLEmitterContext* ctx) {
  ExactRayBoxIntersectionLift lift;
  memset(&lift, 0, sizeof(lift));
  if (!ctx || !ctx->use_uint_temps ||
      !ray_box_intersection_lift(ctx->program, &lift)) return false;
  VolumeSliceEmission volume_emission;
  bool emit_volume = volume_slice_sampling_lift(ctx->program, &lift) &&
                     prepare_volume_slice_emission(ctx, &lift,
                                                   &volume_emission);
  if (!emit_volume) lift.volume_color_valid = false;
  StringBuilder* sb = ctx->sb;
  for (int index = 0; index < ctx->program->instruction_count; ++index) {
    ctx->current_instruction_index = index;
    if (lift.volume_color_valid) {
      bool emitted_position = false;
      for (int sample = 0; sample < 3; ++sample) {
        if (index != lift.sample_position_mul[sample]) continue;
        if (!emit_volume_sample_position(ctx, &lift, &volume_emission,
                                         sample)) return false;
        emitted_position = true;
        break;
      }
      if (emitted_position) continue;
      if (index == lift.volume_color_start) {
        if (!emit_volume_slice_sampling(ctx, &lift, &volume_emission))
          return false;
        index = lift.volume_color_end;
        continue;
      }
    }
    if (index == lift.reciprocal_start) {
      sb_append_spaces(sb, ctx->indent);
      sb_append(sb, "// ");
      sb_append(sb, ctx->program->instructions[index].original_asm);
      sb_append_char(sb, '\n');
      sb_append_spaces(sb, ctx->indent);
      sb_appendf(sb, "float3 dxbc_ray_direction = asfloat(r%d.xyz);\n",
                 lift.direction_register);
      sb_append_spaces(sb, ctx->indent);
      sb_appendf(sb,
                 "r%d.xy = asuint(dxbc_ray_box_intersection(asfloat(r%d.xyz), "
                 "dxbc_ray_direction, asfloat(r%d.xyz)));\n",
                 lift.initial_box_register, lift.initial_origin_register,
                 lift.initial_box_register);
      emit_ray_box_comment_range(ctx, index + 1, lift.reciprocal_end);
      index = lift.reciprocal_end;
      continue;
    }
    bool emitted_repeated = false;
    for (int repeat = 0; repeat < 3; ++repeat) {
      if (index != lift.repeated_start[repeat]) continue;
      sb_append_spaces(sb, ctx->indent);
      sb_append(sb, "// ");
      sb_append(sb, ctx->program->instructions[index].original_asm);
      sb_append_char(sb, '\n');
      sb_append_spaces(sb, ctx->indent);
      sb_appendf(sb,
                 "float2 dxbc_intersection%d = dxbc_ray_box_intersection("
                 "asfloat(r%d.xyz), dxbc_ray_direction, 0.501f);\n",
                 repeat + 1, lift.repeated_origin_register[repeat]);
      emit_ray_box_comment_range(ctx, index + 1,
                                 lift.repeated_end[repeat]);
      index = lift.repeated_end[repeat];
      emitted_repeated = true;
      break;
    }
    if (emitted_repeated) continue;
    if (index == lift.discard_start) {
      emit_ray_box_comment_range(ctx, index, lift.discard_end);
      sb_append_spaces(sb, ctx->indent);
      sb_append(sb,
          "if (dxbc_intersection1.x < 0.0f &&\n");
      sb_append_spaces(sb, ctx->indent + 4);
      sb_append(sb, "dxbc_intersection1.y < 0.0f &&\n");
      sb_append_spaces(sb, ctx->indent + 4);
      sb_append(sb, "dxbc_intersection2.x < 0.0f &&\n");
      sb_append_spaces(sb, ctx->indent + 4);
      sb_append(sb, "dxbc_intersection2.y < 0.0f &&\n");
      sb_append_spaces(sb, ctx->indent + 4);
      sb_append(sb, "dxbc_intersection3.x < 0.0f &&\n");
      sb_append_spaces(sb, ctx->indent + 4);
      sb_append(sb, "dxbc_intersection3.y < 0.0f) {\n");
      sb_append_spaces(sb, ctx->indent + 4);
      sb_append(sb, "discard;\n");
      sb_append_spaces(sb, ctx->indent);
      sb_append(sb, "}\n");
      index = lift.discard_end;
      continue;
    }
    hlsl_emit_instruction(ctx, &ctx->program->instructions[index]);
  }
  return sb_ok(sb);
}

static void emit_geometry_append(HLSLEmitterContext* ctx) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  for (int i = 0; i < program->output_count; ++i) {
    const DXBCSignatureElement* element = &program->outputs[i];
    const uint8_t written_mask = hlsl_output_written_mask(program, element);
    if (written_mask == 0u) continue;
    char field_name[128];
    char swizzle[16];
    if (!hlsl_output_field_name(ctx, element, i, field_name,
                                sizeof(field_name))) return;
    DXBCSignatureElement written_element = *element;
    written_element.mask = written_mask;
    get_signature_swizzle(&written_element, swizzle, sizeof(swizzle));
    sb_append_spaces(sb, ctx->indent);
    if (element->component_type == 1) {
      sb_appendf(sb, "output.%s = asuint(o%u%s);\n", field_name,
                 element->register_id, swizzle);
    } else if (element->component_type == 2) {
      sb_appendf(sb, "output.%s = asint(o%u%s);\n", field_name,
                 element->register_id, swizzle);
    } else {
      sb_appendf(sb, "output.%s = o%u%s;\n", field_name,
                 element->register_id, swizzle);
    }
  }
  sb_append_spaces(sb, ctx->indent);
  sb_append(sb, "dxbc_stream.Append(output);\n");
}

static bool emit_geometry_effect(HLSLEmitterContext* ctx,
                                 const USILInstruction* instruction) {
  if (instruction->opcode == USIL_OP_GEOMETRY_APPEND) {
    emit_geometry_append(ctx);
    return true;
  }
  if (instruction->opcode == USIL_OP_GEOMETRY_RESTART_STRIP) {
    sb_append_spaces(ctx->sb, ctx->indent);
    sb_append(ctx->sb, "dxbc_stream.RestartStrip();\n");
    return true;
  }
  return false;
}

/* Return 1 after replacing a proven flattened row scale, 0 when this is an
 * ordinary instruction, and -1 on an internal proof/emission mismatch.  The
 * captured value is the pre-scale element index.  Direct struct indexing
 * lets D3DCompiler recreate ISHL or low-result IMUL from the serialized
 * stride; retaining the scale and applying an inverse division would add
 * arithmetic that was absent from the source program. */
static int emit_row_struct_index_capture(HLSLEmitterContext *ctx,
                                         int instruction_index) {
  if (!ctx || instruction_index < 0 ||
      instruction_index >= ctx->program->instruction_count) {
    return -1;
  }
  const DXBCOperand *index_source = NULL;
  bool matched = false;
  for (int layout_index = 0;
       layout_index < ctx->cbuffer_layout_count; ++layout_index) {
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[layout_index];
    if (!layout->row_struct_storage ||
        layout->row_struct_scale_instruction != instruction_index) {
      continue;
    }
    if (!layout->row_struct_index_source ||
        (matched && index_source != layout->row_struct_index_source)) {
      return -1;
    }
    index_source = layout->row_struct_index_source;
    matched = true;
  }
  if (!matched) return 0;

  char source[256];
  format_operand_hlsl(ctx, index_source, true, false, 0, false, source,
                      sizeof(source));
  if (!sb_ok(ctx->sb)) return -1;
  sb_append_spaces(ctx->sb, ctx->indent);
  sb_append(ctx->sb, "// ");
  sb_append(ctx->sb,
            ctx->program->instructions[instruction_index].original_asm);
  sb_append_char(ctx->sb, '\n');
  sb_append_spaces(ctx->sb, ctx->indent);
  sb_appendf(ctx->sb, "int dxbc_row_index_i%d = %s;\n",
             instruction_index, source);
  return sb_ok(ctx->sb) ? 1 : -1;
}

void emit_instructions(HLSLEmitterContext* ctx) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;

  if (ctx->emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) {
    if (!emit_high_level_expressions(ctx)) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                     HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                     HLSL_EMIT_REASON_LOWERING_FAILED);
    }
    return;
  }

  /* This lift consumes the complete proven instruction graph. Falling back
   * after a partial write would violate transactional emission, so the match
   * is performed before the first lifted byte is appended. */
  if (ctx->is_geometry &&
      hlsl_triangle_edge_distance_lift_matches(program)) {
    if (!emit_triangle_edge_distance_lift(ctx) || !sb_ok(sb)) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                     HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                     HLSL_EMIT_REASON_LOWERING_FAILED);
    }
    return;
  }
  if (ctx->is_geometry && hlsl_extruded_triangle_lift_matches(program)) {
    if (!emit_extruded_triangle_lift(ctx) || !sb_ok(sb)) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                     HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                     HLSL_EMIT_REASON_LOWERING_FAILED);
    }
    return;
  }

  if (ctx->use_uint_temps &&
      hlsl_ray_box_intersection_lift_matches(program)) {
    if (!emit_ray_box_intersection_lift(ctx) || !sb_ok(sb)) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                     HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                     HLSL_EMIT_REASON_LOWERING_FAILED);
    }
    return;
  }

  if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE) {
    emit_semantic_prelude(ctx);
    if (!sb_ok(sb)) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                     HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
                     HLSL_EMIT_REASON_LOWERING_FAILED);
      return;
    }
  }

  for (int i = 0; i < program->instruction_count; i++) {
    ctx->current_instruction_index = i;

    const bool split_transform_lowered =
        emit_compiler_split_matrix_transform(ctx, i);
    if (!sb_ok(sb)) {
      hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                 HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
      return;
    }
    if (split_transform_lowered) continue;
    const bool tangent_lowered =
        emit_compiler_tangent_frame_lowering(ctx, i);
    if (!sb_ok(sb)) {
      hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                 HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
      return;
    }
    if (tangent_lowered) continue;
    const bool screen_lowered =
        emit_compiler_screen_position_lowering(ctx, i);
    if (!sb_ok(sb)) {
      hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                 HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
      return;
    }
    if (screen_lowered) continue;

    const int row_index_capture = emit_row_struct_index_capture(ctx, i);
    if (row_index_capture < 0 || !sb_ok(sb)) {
      hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                                 HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
      return;
    }
    if (row_index_capture > 0) continue;

    if (ctx->is_geometry &&
        emit_geometry_effect(ctx, &program->instructions[i])) {
      if (!sb_ok(sb)) {
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                   HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
        return;
      }
      continue;
    }

    if (ctx->skip_instruction[i]) {
      sb_append_spaces(sb, ctx->indent);
      sb_append(sb, "// ");
      sb_append(sb, program->instructions[i].original_asm);
      sb_append(sb, "\n");
      if (!sb_ok(sb)) {
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_OUTPUT_FAILED,
                                   HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED,
                                   i, -1);
        return;
      }
      continue;
    }

    const bool semantic_lowered =
        ctx->emit_mode == HLSL_EMIT_MODE_READABLE &&
        emit_semantic_lift_before(ctx, i);
    if (!sb_ok(sb)) {
      hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                 HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
      return;
    }
    if (!semantic_lowered)
      hlsl_emit_instruction(ctx, &program->instructions[i]);
    if (!sb_ok(sb)) {
      hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                 HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
      return;
    }
    if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE) {
      emit_semantic_lift_after(ctx, i);
      if (!sb_ok(sb)) {
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                   HLSL_EMIT_REASON_LOWERING_FAILED, i, -1);
        return;
      }
    }
  }
}

void emit_copy_back_outputs(HLSLEmitterContext* ctx, bool first_line_already_indented) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  int emitted_output = 0;

  for (int i = 0; i < program->output_count; i++) {
    const DXBCSignatureElement *el = &program->outputs[i];
    const uint8_t written_mask = hlsl_output_written_mask(program, el);
    if (written_mask == 0u) continue;
    bool dup = false;
    for (int j = 0; j < i; j++) {
      if (program->outputs[j].register_id == el->register_id) {
        dup = true;
        break;
      }
    }

    int comps = 0;
    if (el->mask & 1) comps++;
    if (el->mask & 2) comps++;
    if (el->mask & 4) comps++;
    if (el->mask & 8) comps++;
    if (comps == 0) comps = 4;

    const char *semantic = dxbc_signature_semantic_name(el);
    bool omit_index =
        (el->semantic_index == 0 && strcmp(semantic, "TEXCOORD") != 0);
    char param_name[128];
    if (dup) {
      if (omit_index) {
        if (!hlsl_format_checked(ctx, param_name, sizeof(param_name),
                                 "out_o_%s", semantic)) return;
      } else {
        if (!hlsl_format_checked(ctx, param_name, sizeof(param_name),
                                 "out_o_%s%d", semantic,
                                 el->semantic_index)) return;
      }
    } else {
      if (!hlsl_format_checked(ctx, param_name, sizeof(param_name), "out_o%u",
                               el->register_id)) return;
    }

    char swizzle[16];
    DXBCSignatureElement written_element = *el;
    written_element.mask = written_mask;
    get_signature_swizzle(&written_element, swizzle, sizeof(swizzle));

    if (emitted_output == 0 && first_line_already_indented) {
      // First line already indented
    } else {
      sb_append_spaces(sb, ctx->indent);
    }
    sb_appendf(sb, "%s = o%u%s;\n", param_name, el->register_id, swizzle);
    ++emitted_output;
  }
}

void emit_return_block(HLSLEmitterContext* ctx) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  bool is_vertex = (strncmp(program->shader_type_model, "vs", 2) == 0);

  if (ctx->is_geometry) {
    sb_append(sb, "}\n");
    return;
  }
  if (is_vertex) {
    for (int i = 0; i < program->output_count; i++) {
      const DXBCSignatureElement *el = &program->outputs[i];
      const uint8_t written_mask = hlsl_output_written_mask(program, el);
      if (written_mask == 0u) continue;
      bool dup = false;
      for (int j = 0; j < i; j++) {
        if (program->outputs[j].register_id == el->register_id) {
          dup = true;
          break;
        }
      }
      const char *semantic = dxbc_signature_semantic_name(el);
      bool omit_index =
          (el->semantic_index == 0 && strcmp(semantic, "TEXCOORD") != 0);
      char field_name[128];
      if (dup) {
        if (omit_index) {
          if (!hlsl_format_checked(ctx, field_name, sizeof(field_name),
                                   "o_%s", semantic)) return;
        } else {
          if (!hlsl_format_checked(ctx, field_name, sizeof(field_name),
                                   "o_%s%d", semantic,
                                   el->semantic_index)) return;
        }
      } else {
        if (!hlsl_format_checked(ctx, field_name, sizeof(field_name), "o%u",
                                 el->register_id)) return;
      }
      char swizzle[16];
      DXBCSignatureElement written_element = *el;
      written_element.mask = written_mask;
      get_signature_swizzle(&written_element, swizzle, sizeof(swizzle));
      sb_appendf(sb, "    output.%s = o%u%s;\n", field_name,
                 el->register_id, swizzle);
    }
    sb_append(sb, "    return output;\n");
  } else if (program->output_count == 1) {
    const DXBCSignatureElement *el = &program->outputs[0];
    if (el->register_id == UINT32_MAX &&
        (strcmp(dxbc_signature_semantic_name(el), "SV_Depth") == 0 ||
         strcmp(dxbc_signature_semantic_name(el),
                "SV_DepthGreaterEqual") == 0 ||
         strcmp(dxbc_signature_semantic_name(el),
                "SV_DepthLessEqual") == 0)) {
      sb_append(sb, "    return oDepth;\n");
    } else {
      char swizzle[16];
      get_signature_swizzle(el, swizzle, sizeof(swizzle));
      sb_appendf(sb, "    return o%u%s;\n", el->register_id, swizzle);
    }
  }
  sb_append(sb, "}\n");
}
