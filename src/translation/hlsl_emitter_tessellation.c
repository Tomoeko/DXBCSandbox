// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * This file contains source-backed inverses for complete tessellation-stage
 * compiler graphs.  A match is deliberately a transaction over the stage
 * contract, signatures, declarations, resources, phases, and every semantic
 * instruction.  A partial match never falls through to the generic
 * vertex/pixel emitter: hull/domain programs outside this surface remain
 * unsupported.
 */

static bool allocation_shape_is_valid(int count, int allocation,
                                      const void *values) {
  return count >= 0 && allocation >= count &&
         ((allocation == 0) == (values == NULL));
}

static bool string_is_exact(const char *value, size_t capacity,
                            const char *expected) {
  if (!value || !expected || capacity == 0) return false;
  const void *end = memchr(value, '\0', capacity);
  return end && strcmp(value, expected) == 0;
}

static bool signature_matches(const DXBCSignatureElement *element,
                              DXBCSignatureRole role,
                              const char *semantic, uint32_t semantic_index,
                              uint32_t system_value, uint32_t register_id,
                              uint8_t mask, uint8_t rw_mask) {
  if (!element) return false;
  const char *actual = dxbc_signature_semantic_name(element);
  return actual && strcmp(actual, semantic) == 0 &&
         element->semantic_index == semantic_index &&
         element->system_value == system_value &&
         element->component_type == 3u &&
         element->register_id == register_id &&
         element->min_precision == 0u && element->interpolation_mode == 0u &&
         element->stream_index == 0u && element->mask == mask &&
         element->rw_mask == rw_mask &&
         dxbc_signature_element_is_valid(element, role);
}

static bool declaration_matches(
    const USILSignatureDeclaration *declaration,
    USILSignatureDeclarationKind kind, DXBCOperandType operand_type,
    bool has_register, uint32_t register_id, uint8_t mask,
    bool has_array_count, uint8_t array_count, bool has_system_value,
    uint32_t system_value, uint32_t source_instruction_index) {
  return declaration && declaration->kind == kind &&
         declaration->operand_type == operand_type &&
         declaration->has_signature_register == has_register &&
         declaration->register_id == register_id && declaration->mask == mask &&
         declaration->stream_index == 0u &&
         declaration->has_array_element_count == has_array_count &&
         declaration->array_element_count == array_count &&
         declaration->has_system_value == has_system_value &&
         declaration->system_value_name == system_value &&
         !declaration->has_interpolation &&
         declaration->interpolation_mode == 0u &&
         declaration->source_instruction_index == source_instruction_index;
}

static bool operand_has_no_immediate(const DXBCOperand *operand) {
  if (!operand || operand->imm_value_count != 0 ||
      operand->immediate_word_count != 0) {
    return false;
  }
  for (int component = 0; component < 4; ++component) {
    if (operand->imm_values[component] != 0u ||
        operand->imm64_values[component] != 0u) {
      return false;
    }
  }
  for (int word = 0; word < 8; ++word)
    if (operand->immediate_words[word] != 0u) return false;
  return true;
}

static bool operand_common_matches(const DXBCOperand *operand,
                                   DXBCOperandType type,
                                   int register_index_dim,
                                   int register_index,
                                   uint8_t destination_mask,
                                   uint8_t swizzle_mode,
                                   const uint8_t swizzle[4]) {
  if (!operand || operand->type != type ||
      operand->register_index_dim != register_index_dim ||
      operand->register_index != register_index ||
      operand->destination_mask != destination_mask ||
      operand->swizzle_mode != swizzle_mode || operand->has_neg ||
      operand->has_abs || operand->min_precision != 0u ||
      operand->extended_token_count != 0u || operand->extended_tokens ||
      operand->rel_op2 || !operand_has_no_immediate(operand)) {
    return false;
  }
  for (int component = 0; component < 4; ++component)
    if (operand->swizzle[component] != swizzle[component]) return false;
  return true;
}

static bool unused_index_dimensions_are_zero(const DXBCOperand *operand,
                                             int first_unused) {
  if (!operand || first_unused < 0 || first_unused > 3) return false;
  for (int dimension = first_unused; dimension < 3; ++dimension) {
    if (operand->index_values[dimension] != 0u ||
        operand->index_representations[dimension] != 0u ||
        operand->index_has_immediate[dimension] ||
        operand->index_value_exceeds_int[dimension]) {
      return false;
    }
  }
  return true;
}

static bool immediate_index_matches(const DXBCOperand *operand,
                                    int dimension, uint32_t value) {
  return operand && dimension >= 0 && dimension < 3 &&
         operand->index_values[dimension] == value &&
         operand->index_representations[dimension] == 0u &&
         operand->index_has_immediate[dimension] &&
         !operand->index_value_exceeds_int[dimension];
}

static bool direct_register_matches(const DXBCOperand *operand,
                                    DXBCOperandType type, int reg,
                                    uint8_t destination_mask,
                                    uint8_t swizzle_mode,
                                    const uint8_t swizzle[4]) {
  return operand_common_matches(operand, type, 1, reg, destination_mask,
                                swizzle_mode, swizzle) &&
         !operand->rel_op0 && !operand->rel_op1 &&
         immediate_index_matches(operand, 0, (uint32_t)reg) &&
         unused_index_dimensions_are_zero(operand, 1);
}

static bool two_index_register_matches(const DXBCOperand *operand,
                                       DXBCOperandType type,
                                       uint32_t first, uint32_t second,
                                       uint8_t swizzle_mode,
                                       const uint8_t swizzle[4]) {
  return operand_common_matches(operand, type, 2, (int)first, 0u,
                                swizzle_mode, swizzle) &&
         !operand->rel_op0 && !operand->rel_op1 &&
         immediate_index_matches(operand, 0, first) &&
         immediate_index_matches(operand, 1, second) &&
         unused_index_dimensions_are_zero(operand, 2);
}

static bool scalar_system_operand_matches(const DXBCOperand *operand,
                                          DXBCOperandType type,
                                          uint8_t component) {
  const uint8_t swizzle[4] = {component, component, component, component};
  return operand_common_matches(operand, type, 0, 0, 0u, 2u, swizzle) &&
         !operand->rel_op0 && !operand->rel_op1 &&
         unused_index_dimensions_are_zero(operand, 0);
}

static bool replicated_system_operand_matches(const DXBCOperand *operand,
                                              DXBCOperandType type,
                                              uint8_t component) {
  const uint8_t swizzle[4] = {component, component, component, component};
  return operand_common_matches(operand, type, 0, 0, 0u, 1u, swizzle) &&
         !operand->rel_op0 && !operand->rel_op1 &&
         unused_index_dimensions_are_zero(operand, 0);
}

static bool temp_dest_matches(const DXBCOperand *operand, int reg,
                              uint8_t mask) {
  const uint8_t identity[4] = {0u, 1u, 2u, 3u};
  return direct_register_matches(operand, OPERAND_TYPE_TEMP, reg, mask, 0u,
                                 identity);
}

static bool output_dest_matches(const DXBCOperand *operand, int reg,
                                uint8_t mask) {
  const uint8_t identity[4] = {0u, 1u, 2u, 3u};
  return direct_register_matches(operand, OPERAND_TYPE_OUTPUT, reg, mask, 0u,
                                 identity);
}

static bool swizzled_register_matches(const DXBCOperand *operand,
                                      DXBCOperandType type, int reg,
                                      uint8_t x, uint8_t y, uint8_t z,
                                      uint8_t w) {
  const uint8_t swizzle[4] = {x, y, z, w};
  return direct_register_matches(operand, type, reg, 0u, 1u, swizzle);
}

static bool scalar_register_matches(const DXBCOperand *operand,
                                    DXBCOperandType type, int reg,
                                    uint8_t component) {
  const uint8_t swizzle[4] = {component, component, component, component};
  return direct_register_matches(operand, type, reg, 0u, 2u, swizzle);
}

static bool replicated_register_matches(const DXBCOperand *operand,
                                        DXBCOperandType type, int reg,
                                        uint8_t component) {
  const uint8_t swizzle[4] = {component, component, component, component};
  return direct_register_matches(operand, type, reg, 0u, 1u, swizzle);
}

static bool cbuffer_operand_matches(const DXBCOperand *operand, uint32_t reg,
                                    uint32_t row, uint8_t swizzle_mode,
                                    uint8_t x, uint8_t y, uint8_t z,
                                    uint8_t w) {
  const uint8_t swizzle[4] = {x, y, z, w};
  return two_index_register_matches(operand, OPERAND_TYPE_CONSTANT_BUFFER,
                                    reg, row, swizzle_mode, swizzle);
}

static bool control_point_operand_matches(const DXBCOperand *operand,
                                          uint32_t point, uint32_t reg,
                                          uint8_t x, uint8_t y, uint8_t z,
                                          uint8_t w) {
  const uint8_t swizzle[4] = {x, y, z, w};
  return two_index_register_matches(operand,
                                    OPERAND_TYPE_INPUT_CONTROL_POINT,
                                    point, reg, 1u, swizzle);
}

static bool immediate_float_matches(const DXBCOperand *operand,
                                    const uint32_t *words, int count) {
  const uint8_t identity[4] = {0u, 1u, 2u, 3u};
  if (!operand || count < 1 || count > 4 ||
      operand->type != OPERAND_TYPE_IMMEDIATE32 ||
      operand->register_index_dim != 0 || operand->register_index != 0 ||
      operand->destination_mask != 0u || operand->swizzle_mode != 0u ||
      operand->has_neg || operand->has_abs || operand->min_precision != 0u ||
      operand->extended_token_count != 0u || operand->extended_tokens ||
      operand->rel_op0 || operand->rel_op1 || operand->rel_op2 ||
      operand->imm_value_count != count ||
      operand->immediate_word_count != count ||
      !unused_index_dimensions_are_zero(operand, 0)) {
    return false;
  }
  for (int component = 0; component < 4; ++component) {
    const uint32_t expected = component < count ? words[component] : 0u;
    if (operand->swizzle[component] != identity[component] ||
        operand->imm_values[component] != expected ||
        operand->imm64_values[component] != 0u) {
      return false;
    }
  }
  for (int word = 0; word < 8; ++word) {
    const uint32_t expected = word < count ? words[word] : 0u;
    if (operand->immediate_words[word] != expected) return false;
  }
  return true;
}

static bool instruction_common_matches(const USILInstruction *instruction,
                                       USILOpcode opcode,
                                       int operand_count,
                                       uint32_t source_instruction_index) {
  return instruction && instruction->opcode == opcode &&
         instruction->operand_count == operand_count &&
         instruction->source_instruction_index == source_instruction_index &&
         !instruction->saturate && instruction->precise_mask == 0u &&
         instruction->condition_test == DXBC_INSTRUCTION_TEST_NONE &&
         instruction->geometry_effect == USIL_GEOMETRY_EFFECT_NONE &&
         instruction->geometry_stream_id == 0u &&
         !instruction->geometry_stream_explicit &&
         !instruction->has_resource_dimension &&
         instruction->resource_dimension[0] == '\0' &&
         instruction->resource_stride == 0u &&
         !instruction->has_texel_offset &&
         instruction->texel_offsets[0] == 0 &&
         instruction->texel_offsets[1] == 0 &&
         instruction->texel_offsets[2] == 0 &&
         !instruction->has_resource_return_types &&
         instruction->resource_return_types[0] == 0u &&
         instruction->resource_return_types[1] == 0u &&
         instruction->resource_return_types[2] == 0u &&
         instruction->resource_return_types[3] == 0u &&
         instruction->resource_info_return_type == 0u &&
         instruction->sample_info_return_type == 0u;
}

static bool sample_instruction_common_matches(
    const USILInstruction *instruction, uint32_t source_instruction_index) {
  return instruction && instruction->opcode == USIL_OP_SAMPLE_L &&
         instruction->operand_count == 5 &&
         instruction->source_instruction_index == source_instruction_index &&
         !instruction->saturate && instruction->precise_mask == 0u &&
         instruction->condition_test == DXBC_INSTRUCTION_TEST_NONE &&
         instruction->geometry_effect == USIL_GEOMETRY_EFFECT_NONE &&
         instruction->geometry_stream_id == 0u &&
         !instruction->geometry_stream_explicit &&
         instruction->has_resource_dimension &&
         string_is_exact(instruction->resource_dimension,
                         sizeof(instruction->resource_dimension), "2d") &&
         instruction->resource_stride == 0u &&
         !instruction->has_texel_offset &&
         instruction->texel_offsets[0] == 0 &&
         instruction->texel_offsets[1] == 0 &&
         instruction->texel_offsets[2] == 0 &&
         instruction->has_resource_return_types &&
         instruction->resource_return_types[0] == 5u &&
         instruction->resource_return_types[1] == 5u &&
         instruction->resource_return_types[2] == 5u &&
         instruction->resource_return_types[3] == 5u &&
         instruction->resource_info_return_type == 0u &&
         instruction->sample_info_return_type == 0u;
}

static bool program_lists_are_valid(const USILProgram *program) {
  if (!program ||
      !allocation_shape_is_valid(program->instruction_count,
                                 program->instruction_alloc,
                                 program->instructions) ||
      !allocation_shape_is_valid(program->cbuffer_count,
                                 program->cbuffer_alloc,
                                 program->cbuffers) ||
      !allocation_shape_is_valid(program->texture_count,
                                 program->texture_alloc,
                                 program->textures) ||
      !allocation_shape_is_valid(program->sampler_count,
                                 program->sampler_alloc,
                                 program->samplers) ||
      !allocation_shape_is_valid(program->indexable_temp_count,
                                 program->indexable_temp_alloc,
                                 program->indexable_temps) ||
      !allocation_shape_is_valid(program->uav_count, program->uav_alloc,
                                 program->uavs) ||
      !allocation_shape_is_valid(program->signature_declaration_count,
                                 program->signature_declaration_alloc,
                                 program->signature_declarations) ||
      !allocation_shape_is_valid(program->index_range_count,
                                 program->index_range_alloc,
                                 program->index_ranges)) {
    return false;
  }
  for (int index = 0; index < program->instruction_count; ++index) {
    if (!usil_instruction_shape_valid(program, &program->instructions[index])) {
      return false;
    }
  }
  return true;
}

static bool common_tessellation_contract_matches(const USILProgram *program,
                                                 DXBCProgramType type,
                                                 const char *model) {
  return program && program->has_parsed_signature_authority &&
         usil_signature_authority_is_valid(program) &&
         program->has_stage_contract && program->program_type == type &&
         string_is_exact(program->shader_type_model,
                         sizeof(program->shader_type_model), model) &&
         program->shader_model_major == 5u &&
         program->shader_model_minor == 0u &&
         program->tessellation.valid &&
         program->tessellation.domain == DXBC_TESSELLATOR_DOMAIN_TRIANGLE &&
         !program->tessellation.has_max_tessellation_factor &&
         program->tessellation.max_tessellation_factor_bits == 0u &&
         program->icb_value_count == 0 && program->icb_value_alloc == 0 &&
         !program->icb_values && program_lists_are_valid(program);
}

static bool hull_signatures_match(const USILProgram *program) {
  return program->input_count == 3 && program->output_count == 3 &&
         program->patch_constant_count == 4 &&
         signature_matches(&program->inputs[0], DXBC_SIGNATURE_ROLE_INPUT,
                           "INTERNALTESSPOS", 0u, 0u, 0u, 0xfu, 0xfu) &&
         signature_matches(&program->inputs[1], DXBC_SIGNATURE_ROLE_INPUT,
                           "NORMAL", 0u, 0u, 1u, 0x7u, 0x7u) &&
         signature_matches(&program->inputs[2], DXBC_SIGNATURE_ROLE_INPUT,
                           "TEXCOORD", 0u, 0u, 2u, 0x3u, 0x3u) &&
         signature_matches(&program->outputs[0], DXBC_SIGNATURE_ROLE_OUTPUT,
                           "INTERNALTESSPOS", 0u, 0u, 0u, 0xfu, 0x0u) &&
         signature_matches(&program->outputs[1], DXBC_SIGNATURE_ROLE_OUTPUT,
                           "NORMAL", 0u, 0u, 1u, 0x7u, 0x8u) &&
         signature_matches(&program->outputs[2], DXBC_SIGNATURE_ROLE_OUTPUT,
                           "TEXCOORD", 0u, 0u, 2u, 0x3u, 0xcu) &&
         signature_matches(&program->patch_constants[0],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_TessFactor", 0u, 13u, 0u, 0x1u, 0xeu) &&
         signature_matches(&program->patch_constants[1],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_TessFactor", 1u, 13u, 1u, 0x1u, 0xeu) &&
         signature_matches(&program->patch_constants[2],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_TessFactor", 2u, 13u, 2u, 0x1u, 0xeu) &&
         signature_matches(&program->patch_constants[3],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_InsideTessFactor", 0u, 14u, 3u, 0x1u,
                           0xeu);
}

static bool hull_declarations_and_phases_match(const USILProgram *program) {
  if (program->signature_declaration_count != 5 ||
      !declaration_matches(&program->signature_declarations[0],
                           USIL_SIGNATURE_DECL_INPUT,
                           OPERAND_TYPE_FORK_INSTANCE_ID, false,
                           UINT32_MAX, 0u, false, 0u, false, 0u, 10u) ||
      !declaration_matches(&program->signature_declarations[1],
                           USIL_SIGNATURE_DECL_OUTPUT_SIV,
                           OPERAND_TYPE_OUTPUT, true, 0u, 0x1u, false, 0u,
                           true, 17u, 11u) ||
      !declaration_matches(&program->signature_declarations[2],
                           USIL_SIGNATURE_DECL_OUTPUT_SIV,
                           OPERAND_TYPE_OUTPUT, true, 1u, 0x1u, false, 0u,
                           true, 18u, 12u) ||
      !declaration_matches(&program->signature_declarations[3],
                           USIL_SIGNATURE_DECL_OUTPUT_SIV,
                           OPERAND_TYPE_OUTPUT, true, 2u, 0x1u, false, 0u,
                           true, 19u, 13u) ||
      !declaration_matches(&program->signature_declarations[4],
                           USIL_SIGNATURE_DECL_OUTPUT_SIV,
                           OPERAND_TYPE_OUTPUT, true, 3u, 0x1u, false, 0u,
                           true, 20u, 20u) ||
      program->tessellation.phase_count != 2u ||
      program->tessellation.phase_capacity < 2u ||
      !program->tessellation.phases) {
    return false;
  }
  const USILHullPhase *edge = &program->tessellation.phases[0];
  const USILHullPhase *inside = &program->tessellation.phases[1];
  if (edge->kind != DXBC_HULL_PHASE_FORK ||
      edge->marker_source_instruction_index != 8u ||
      edge->first_source_instruction_index != 9u ||
      edge->end_source_instruction_index != 19u ||
      edge->first_instruction_index != 0 || edge->end_instruction_index != 3 ||
      !edge->instance_count_declared || edge->instance_count != 3u ||
      inside->kind != DXBC_HULL_PHASE_FORK ||
      inside->marker_source_instruction_index != 19u ||
      inside->first_source_instruction_index != 20u ||
      inside->end_source_instruction_index != 23u ||
      inside->first_instruction_index != 3 ||
      inside->end_instruction_index != 5 ||
      inside->instance_count_declared || inside->instance_count != 1u ||
      program->index_range_count != 1) {
    return false;
  }
  const USILIndexRange *range = &program->index_ranges[0];
  const uint8_t identity[4] = {0u, 1u, 2u, 3u};
  return range->register_count == 3u &&
         range->source_instruction_index == 15u &&
         range->hull_phase_index == 0 &&
         direct_register_matches(&range->operand, OPERAND_TYPE_OUTPUT, 0,
                                 0x10u, 0u, identity);
}

static bool hull_instructions_match(const USILProgram *program) {
  if (program->instruction_count != 5 || program->temp_count != 1) return false;
  const USILInstruction *instructions = program->instructions;
  if (!instruction_common_matches(&instructions[0], USIL_OP_MOV, 2, 16u) ||
      !temp_dest_matches(&instructions[0].operands[0], 0, 0x10u) ||
      !scalar_system_operand_matches(&instructions[0].operands[1],
                                     OPERAND_TYPE_FORK_INSTANCE_ID, 0u) ||
      !instruction_common_matches(&instructions[1], USIL_OP_MOV, 2, 17u)) {
    return false;
  }

  const DXBCOperand *relative_output = &instructions[1].operands[0];
  const uint8_t identity[4] = {0u, 1u, 2u, 3u};
  if (!operand_common_matches(relative_output, OPERAND_TYPE_OUTPUT, 1, 0,
                              0x10u, 0u, identity) ||
      relative_output->index_representations[0] != 2u ||
      relative_output->index_has_immediate[0] ||
      relative_output->index_value_exceeds_int[0] ||
      relative_output->index_values[0] != 0u ||
      !unused_index_dimensions_are_zero(relative_output, 1) ||
      !relative_output->rel_op0 || relative_output->rel_op1 ||
      !scalar_register_matches(relative_output->rel_op0, OPERAND_TYPE_TEMP,
                               0, 0u) ||
      !cbuffer_operand_matches(&instructions[1].operands[1], 0u, 3u, 2u,
                               0u, 0u, 0u, 0u) ||
      !instruction_common_matches(&instructions[2], USIL_OP_RET, 0, 18u) ||
      !instruction_common_matches(&instructions[3], USIL_OP_MOV, 2, 21u) ||
      !output_dest_matches(&instructions[3].operands[0], 3, 0x10u) ||
      !cbuffer_operand_matches(&instructions[3].operands[1], 0u, 3u, 2u,
                               0u, 0u, 0u, 0u) ||
      !instruction_common_matches(&instructions[4], USIL_OP_RET, 0, 22u)) {
    return false;
  }
  return true;
}

static bool hull_program_matches(const USILProgram *program) {
  return common_tessellation_contract_matches(program,
                                              DXBC_PROGRAM_TYPE_HULL,
                                              "hs_5_0") &&
         program->has_global_flags && program->global_flags == 1u &&
         program->tessellation.input_control_point_count == 3u &&
         program->tessellation.output_control_point_count == 3u &&
         program->tessellation.partitioning ==
             DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD &&
         program->tessellation.output_primitive ==
             DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
         program->cbuffer_count == 1 && program->cbuffers[0].reg_idx == 0 &&
         program->cbuffers[0].size == 4 &&
         !program->cbuffers[0].dynamic_indexed &&
         program->texture_count == 0 && program->sampler_count == 0 &&
         program->indexable_temp_count == 0 && program->uav_count == 0 &&
         hull_signatures_match(program) &&
         hull_declarations_and_phases_match(program) &&
         hull_instructions_match(program);
}

static bool domain_signatures_match(const USILProgram *program) {
  return program->input_count == 3 && program->output_count == 2 &&
         program->patch_constant_count == 4 &&
         signature_matches(&program->inputs[0], DXBC_SIGNATURE_ROLE_INPUT,
                           "INTERNALTESSPOS", 0u, 0u, 0u, 0xfu, 0x7u) &&
         signature_matches(&program->inputs[1], DXBC_SIGNATURE_ROLE_INPUT,
                           "NORMAL", 0u, 0u, 1u, 0x7u, 0x7u) &&
         signature_matches(&program->inputs[2], DXBC_SIGNATURE_ROLE_INPUT,
                           "TEXCOORD", 0u, 0u, 2u, 0x3u, 0x3u) &&
         signature_matches(&program->outputs[0], DXBC_SIGNATURE_ROLE_OUTPUT,
                           "SV_POSITION", 0u, 1u, 0u, 0xfu, 0x0u) &&
         signature_matches(&program->outputs[1], DXBC_SIGNATURE_ROLE_OUTPUT,
                           "TEXCOORD", 0u, 0u, 1u, 0x3u, 0xcu) &&
         signature_matches(&program->patch_constants[0],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_TessFactor", 0u, 13u, 0u, 0x1u, 0x0u) &&
         signature_matches(&program->patch_constants[1],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_TessFactor", 1u, 13u, 1u, 0x1u, 0x0u) &&
         signature_matches(&program->patch_constants[2],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_TessFactor", 2u, 13u, 2u, 0x1u, 0x0u) &&
         signature_matches(&program->patch_constants[3],
                           DXBC_SIGNATURE_ROLE_PATCH_CONSTANT,
                           "SV_InsideTessFactor", 0u, 14u, 3u, 0x1u,
                           0x0u);
}

static bool domain_declarations_match(const USILProgram *program) {
  return program->signature_declaration_count == 6 &&
         declaration_matches(&program->signature_declarations[0],
                             USIL_SIGNATURE_DECL_INPUT,
                             OPERAND_TYPE_DOMAIN_LOCATION, false,
                             UINT32_MAX, 0x7u, false, 0u, false, 0u, 7u) &&
         declaration_matches(&program->signature_declarations[1],
                             USIL_SIGNATURE_DECL_INPUT,
                             OPERAND_TYPE_INPUT_CONTROL_POINT, true, 0u,
                             0x7u, true, 3u, false, 0u, 8u) &&
         declaration_matches(&program->signature_declarations[2],
                             USIL_SIGNATURE_DECL_INPUT,
                             OPERAND_TYPE_INPUT_CONTROL_POINT, true, 1u,
                             0x7u, true, 3u, false, 0u, 9u) &&
         declaration_matches(&program->signature_declarations[3],
                             USIL_SIGNATURE_DECL_INPUT,
                             OPERAND_TYPE_INPUT_CONTROL_POINT, true, 2u,
                             0x3u, true, 3u, false, 0u, 10u) &&
         declaration_matches(&program->signature_declarations[4],
                             USIL_SIGNATURE_DECL_OUTPUT_SIV,
                             OPERAND_TYPE_OUTPUT, true, 0u, 0xfu, false, 0u,
                             true, 1u, 11u) &&
         declaration_matches(&program->signature_declarations[5],
                             USIL_SIGNATURE_DECL_OUTPUT,
                             OPERAND_TYPE_OUTPUT, true, 1u, 0x3u, false, 0u,
                             false, 0u, 12u);
}

static bool domain_resources_match(const USILProgram *program) {
  if (program->cbuffer_count != 2 || program->texture_count != 1 ||
      program->sampler_count != 1 || program->indexable_temp_count != 0 ||
      program->uav_count != 0 || program->cbuffers[0].reg_idx != 0 ||
      program->cbuffers[0].size != 4 ||
      program->cbuffers[0].dynamic_indexed ||
      program->cbuffers[1].reg_idx != 1 ||
      program->cbuffers[1].size != 21 ||
      program->cbuffers[1].dynamic_indexed ||
      program->textures[0].reg_idx != 0 ||
      !string_is_exact(program->textures[0].dimension,
                       sizeof(program->textures[0].dimension), "2d") ||
      program->textures[0].sample_count != 0u ||
      program->textures[0].stride != 0 ||
      program->samplers[0].reg_idx != 0 || program->samplers[0].mode != 0u) {
    return false;
  }
  for (int component = 0; component < 4; ++component)
    if (program->textures[0].return_types[component] != 5u) return false;
  return true;
}

static bool domain_interpolation_triplet_matches(
    const USILInstruction *instructions, int first, int temp_reg,
    uint8_t destination_mask, uint32_t control_point_register,
    uint8_t x, uint8_t y, uint8_t z, uint8_t w,
    uint32_t first_source_instruction) {
  return instruction_common_matches(&instructions[first], USIL_OP_MUL, 3,
                                    first_source_instruction) &&
         temp_dest_matches(&instructions[first].operands[0], temp_reg,
                           destination_mask) &&
         replicated_system_operand_matches(&instructions[first].operands[1],
                                           OPERAND_TYPE_DOMAIN_LOCATION,
                                           1u) &&
         control_point_operand_matches(&instructions[first].operands[2], 1u,
                                       control_point_register, x, y, z, w) &&
         instruction_common_matches(&instructions[first + 1], USIL_OP_MAD, 4,
                                    first_source_instruction + 1u) &&
         temp_dest_matches(&instructions[first + 1].operands[0], temp_reg,
                           destination_mask) &&
         control_point_operand_matches(&instructions[first + 1].operands[1],
                                       0u, control_point_register,
                                       x, y, z, w) &&
         replicated_system_operand_matches(
             &instructions[first + 1].operands[2],
             OPERAND_TYPE_DOMAIN_LOCATION, 0u) &&
         swizzled_register_matches(&instructions[first + 1].operands[3],
                                   OPERAND_TYPE_TEMP, temp_reg, x, y, z, w) &&
         instruction_common_matches(&instructions[first + 2], USIL_OP_MAD, 4,
                                    first_source_instruction + 2u) &&
         temp_dest_matches(&instructions[first + 2].operands[0], temp_reg,
                           destination_mask) &&
         control_point_operand_matches(&instructions[first + 2].operands[1],
                                       2u, control_point_register,
                                       x, y, z, w) &&
         replicated_system_operand_matches(
             &instructions[first + 2].operands[2],
             OPERAND_TYPE_DOMAIN_LOCATION, 2u) &&
         swizzled_register_matches(&instructions[first + 2].operands[3],
                                   OPERAND_TYPE_TEMP, temp_reg, x, y, z, w);
}

static bool domain_instructions_match(const USILProgram *program) {
  if (program->instruction_count != 22 || program->temp_count != 2) return false;
  const USILInstruction *i = program->instructions;
  const uint32_t zero = 0u;
  const uint32_t half[4] = {UINT32_C(0x3f000000), UINT32_C(0x3f000000),
                            UINT32_C(0x3f000000), 0u};
  const bool front_a = domain_interpolation_triplet_matches(
      i, 0, 0, 0x70u, 1u, 0u, 1u, 2u, 0u, 14u);
  const bool front_b = domain_interpolation_triplet_matches(
      i, 3, 1, 0x30u, 2u, 0u, 1u, 0u, 0u, 17u);
  const bool front_c =
      sample_instruction_common_matches(&i[6], 20u) &&
      temp_dest_matches(&i[6].operands[0], 0, 0x80u) &&
      swizzled_register_matches(&i[6].operands[1], OPERAND_TYPE_TEMP, 1,
                                 0u, 1u, 0u, 0u);
  const bool front_d =
      swizzled_register_matches(&i[6].operands[2], OPERAND_TYPE_RESOURCE, 0,
                                 1u, 2u, 3u, 0u);
  const bool front_e =
      direct_register_matches(&i[6].operands[3], OPERAND_TYPE_SAMPLER, 0,
                               0u, 0u,
                               (const uint8_t[4]){0u, 1u, 2u, 3u});
  const bool front_f = immediate_float_matches(&i[6].operands[4], &zero, 1);
  const bool front_g =
      instruction_common_matches(&i[7], USIL_OP_MOV, 2, 21u) &&
      output_dest_matches(&i[7].operands[0], 1, 0x30u) &&
      swizzled_register_matches(&i[7].operands[1], OPERAND_TYPE_TEMP, 1,
                                0u, 1u, 0u, 0u);
  const bool front_h =
      instruction_common_matches(&i[8], USIL_OP_MUL, 3, 22u) &&
      temp_dest_matches(&i[8].operands[0], 0, 0x70u) &&
      replicated_register_matches(&i[8].operands[1], OPERAND_TYPE_TEMP, 0,
                                  3u) &&
      swizzled_register_matches(&i[8].operands[2], OPERAND_TYPE_TEMP, 0,
                                0u, 1u, 2u, 0u);
  const bool front_i = domain_interpolation_triplet_matches(
      i, 9, 1, 0x70u, 0u, 0u, 1u, 2u, 0u, 23u);
  const bool front_j =
      instruction_common_matches(&i[12], USIL_OP_MAD, 4, 26u) &&
      temp_dest_matches(&i[12].operands[0], 0, 0x70u) &&
      swizzled_register_matches(&i[12].operands[1], OPERAND_TYPE_TEMP, 0,
                                0u, 1u, 2u, 0u) &&
      immediate_float_matches(&i[12].operands[2], half, 4) &&
      swizzled_register_matches(&i[12].operands[3], OPERAND_TYPE_TEMP, 1,
                                0u, 1u, 2u, 0u);
  if (!(front_a && front_b && front_c && front_d && front_e && front_f &&
        front_g && front_h && front_i && front_j)) {
    return false;
  }

  if (!instruction_common_matches(&i[13], USIL_OP_MUL, 3, 27u) ||
      !temp_dest_matches(&i[13].operands[0], 1, 0xf0u) ||
      !replicated_register_matches(&i[13].operands[1], OPERAND_TYPE_TEMP, 0,
                                   1u) ||
      !cbuffer_operand_matches(&i[13].operands[2], 0u, 1u, 1u,
                               0u, 1u, 2u, 3u) ||
      !instruction_common_matches(&i[14], USIL_OP_MAD, 4, 28u) ||
      !temp_dest_matches(&i[14].operands[0], 1, 0xf0u) ||
      !cbuffer_operand_matches(&i[14].operands[1], 0u, 0u, 1u,
                               0u, 1u, 2u, 3u) ||
      !replicated_register_matches(&i[14].operands[2], OPERAND_TYPE_TEMP, 0,
                                   0u) ||
      !swizzled_register_matches(&i[14].operands[3], OPERAND_TYPE_TEMP, 1,
                                 0u, 1u, 2u, 3u) ||
      !instruction_common_matches(&i[15], USIL_OP_MAD, 4, 29u) ||
      !temp_dest_matches(&i[15].operands[0], 0, 0xf0u) ||
      !cbuffer_operand_matches(&i[15].operands[1], 0u, 2u, 1u,
                               0u, 1u, 2u, 3u) ||
      !replicated_register_matches(&i[15].operands[2], OPERAND_TYPE_TEMP, 0,
                                   2u) ||
      !swizzled_register_matches(&i[15].operands[3], OPERAND_TYPE_TEMP, 1,
                                 0u, 1u, 2u, 3u) ||
      !instruction_common_matches(&i[16], USIL_OP_ADD, 3, 30u) ||
      !temp_dest_matches(&i[16].operands[0], 0, 0xf0u) ||
      !swizzled_register_matches(&i[16].operands[1], OPERAND_TYPE_TEMP, 0,
                                 0u, 1u, 2u, 3u) ||
      !cbuffer_operand_matches(&i[16].operands[2], 0u, 3u, 1u,
                               0u, 1u, 2u, 3u)) {
    return false;
  }

  if (!instruction_common_matches(&i[17], USIL_OP_MUL, 3, 31u) ||
      !temp_dest_matches(&i[17].operands[0], 1, 0xf0u) ||
      !replicated_register_matches(&i[17].operands[1], OPERAND_TYPE_TEMP, 0,
                                   1u) ||
      !cbuffer_operand_matches(&i[17].operands[2], 1u, 18u, 1u,
                               0u, 1u, 2u, 3u) ||
      !instruction_common_matches(&i[18], USIL_OP_MAD, 4, 32u) ||
      !temp_dest_matches(&i[18].operands[0], 1, 0xf0u) ||
      !cbuffer_operand_matches(&i[18].operands[1], 1u, 17u, 1u,
                               0u, 1u, 2u, 3u) ||
      !replicated_register_matches(&i[18].operands[2], OPERAND_TYPE_TEMP, 0,
                                   0u) ||
      !swizzled_register_matches(&i[18].operands[3], OPERAND_TYPE_TEMP, 1,
                                 0u, 1u, 2u, 3u) ||
      !instruction_common_matches(&i[19], USIL_OP_MAD, 4, 33u) ||
      !temp_dest_matches(&i[19].operands[0], 1, 0xf0u) ||
      !cbuffer_operand_matches(&i[19].operands[1], 1u, 19u, 1u,
                               0u, 1u, 2u, 3u) ||
      !replicated_register_matches(&i[19].operands[2], OPERAND_TYPE_TEMP, 0,
                                   2u) ||
      !swizzled_register_matches(&i[19].operands[3], OPERAND_TYPE_TEMP, 1,
                                 0u, 1u, 2u, 3u) ||
      !instruction_common_matches(&i[20], USIL_OP_MAD, 4, 34u) ||
      !output_dest_matches(&i[20].operands[0], 0, 0xf0u) ||
      !cbuffer_operand_matches(&i[20].operands[1], 1u, 20u, 1u,
                               0u, 1u, 2u, 3u) ||
      !replicated_register_matches(&i[20].operands[2], OPERAND_TYPE_TEMP, 0,
                                   3u) ||
      !swizzled_register_matches(&i[20].operands[3], OPERAND_TYPE_TEMP, 1,
                                 0u, 1u, 2u, 3u) ||
      !instruction_common_matches(&i[21], USIL_OP_RET, 0, 35u)) {
    return false;
  }
  return true;
}

static bool domain_program_matches(const USILProgram *program) {
  const bool common = common_tessellation_contract_matches(
      program, DXBC_PROGRAM_TYPE_DOMAIN, "ds_5_0");
  const bool flags = program->has_global_flags && program->global_flags == 1u;
  const bool contract =
         program->tessellation.input_control_point_count == 3u &&
         program->tessellation.output_control_point_count == 0u &&
         program->tessellation.partitioning ==
             DXBC_TESSELLATOR_PARTITIONING_UNDEFINED &&
         program->tessellation.output_primitive ==
             DXBC_TESSELLATOR_OUTPUT_UNDEFINED &&
         program->tessellation.phase_count == 0u &&
         program->tessellation.phase_capacity == 0u &&
         !program->tessellation.phases && program->index_range_count == 0;
  const bool signatures = domain_signatures_match(program);
  const bool declarations = domain_declarations_match(program);
  const bool resources = domain_resources_match(program);
  const bool instructions = domain_instructions_match(program);
  return common && flags && contract && signatures && declarations &&
         resources && instructions;
}

bool hlsl_exact_tessellation_lift_matches(const USILProgram *program) {
  if (!program) return false;
  if (program->program_type == DXBC_PROGRAM_TYPE_HULL)
    return hull_program_matches(program);
  if (program->program_type == DXBC_PROGRAM_TYPE_DOMAIN)
    return domain_program_matches(program);
  return false;
}

static bool texture_name_for_register(const HLSLEmitterContext *ctx, int reg,
                                      const char **out_name) {
  return resolve_srv_name_ctx(ctx, reg, SERIALIZED_RESOURCE_TEXTURE,
                              out_name);
}

static bool sampler_has_serialized_authority(
    const HLSLEmitterContext *ctx, int reg) {
  char *name = NULL;
  if (!resolve_sampler_name(ctx->params, reg, &name)) return false;
  if (name) {
    free(name);
    return true;
  }
  if (!resolve_sampler_name(ctx->common_params, reg, &name)) return false;
  const bool found = name != NULL;
  free(name);
  return found;
}

static void emit_hull_stage(HLSLEmitterContext *ctx,
                            const char *entry_point) {
  StringBuilder *sb = ctx->sb;
  sb_append(sb,
            "cbuffer dxbc_hs_cb0 : register(b0) {\n"
            "    float4 dxbc_hs_cb0_data[4];\n"
            "};\n\n"
            "struct dxbc_hs_control_point {\n"
            "    float4 vertex : INTERNALTESSPOS;\n"
            "    float3 normal : NORMAL;\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n\n"
            "struct dxbc_hs_factors {\n"
            "    float edge[3] : SV_TessFactor;\n"
            "    float inside : SV_InsideTessFactor;\n"
            "};\n\n"
            "dxbc_hs_factors dxbc_patch_constant(\n"
            "    InputPatch<dxbc_hs_control_point, 3> patch) {\n"
            "    dxbc_hs_factors factors;\n"
            "    factors.edge[0] = factors.edge[1] = factors.edge[2] = "
            "dxbc_hs_cb0_data[3].x;\n"
            "    factors.inside = dxbc_hs_cb0_data[3].x;\n"
            "    return factors;\n"
            "}\n\n"
            "[domain(\"tri\")]\n"
            "[partitioning(\"fractional_odd\")]\n"
            "[outputtopology(\"triangle_cw\")]\n"
            "[patchconstantfunc(\"dxbc_patch_constant\")]\n"
            "[outputcontrolpoints(3)]\n");
  sb_appendf(sb,
             "dxbc_hs_control_point %s(\n"
             "    InputPatch<dxbc_hs_control_point, 3> patch,\n"
             "    uint id : SV_OutputControlPointID) {\n"
             "    return patch[id];\n"
             "}\n",
             entry_point);
}

static void emit_domain_stage(HLSLEmitterContext *ctx,
                              const char *entry_point) {
  StringBuilder *sb = ctx->sb;
  const char *texture = NULL;
  if (!texture_name_for_register(ctx, 0, &texture)) {
    hlsl_emit_fail_metadata(
        ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
        HLSL_EMIT_PHASE_TESSELLATION_EMISSION,
        HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
        HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
        0, -1, 0);
    return;
  }
  const char *sampler = ctx->sampler_names[0];
  if (!texture) texture = "t0";
  if (!sampler || !sampler_has_serialized_authority(ctx, 0)) {
    int sampler_index = -1;
    for (int index = 0; index < ctx->program->sampler_count; ++index) {
      if (ctx->program->samplers[index].reg_idx == 0) {
        sampler_index = index;
        break;
      }
    }
    hlsl_emit_fail_metadata(
        ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
        HLSL_EMIT_PHASE_TESSELLATION_EMISSION,
        HLSL_EMIT_REASON_MISSING_METADATA_AUTHORITY,
        HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
        sampler_index, -1, 0);
    return;
  }
  sb_append(sb,
            "cbuffer dxbc_ds_cb0 : register(b0) {\n"
            "    float4x4 dxbc_ObjectToWorld;\n"
            "};\n"
            "cbuffer dxbc_ds_cb1 : register(b1) {\n"
            "    float4 dxbc_ds_cb1_padding[17];\n"
            "    float4x4 dxbc_MatrixVP;\n"
            "};\n\n"
            "struct dxbc_ds_control_point {\n"
            "    float4 vertex : INTERNALTESSPOS;\n"
            "    float3 normal : NORMAL;\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n\n"
            "struct dxbc_ds_factors {\n"
            "    float edge[3] : SV_TessFactor;\n"
            "    float inside : SV_InsideTessFactor;\n"
            "};\n\n"
            "struct dxbc_ds_output {\n"
            "    float4 pos : SV_POSITION;\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n\n"
            "[domain(\"tri\")]\n");
  sb_appendf(sb,
             "dxbc_ds_output %s(\n"
             "    dxbc_ds_factors factors,\n"
             "    OutputPatch<dxbc_ds_control_point, 3> patch,\n"
             "    float3 bary : SV_DomainLocation) {\n"
             "    dxbc_ds_output output;\n"
             "    float4 pos = patch[0].vertex * bary.x\n"
             "               + patch[1].vertex * bary.y\n"
             "               + patch[2].vertex * bary.z;\n"
             "    float3 normal = patch[0].normal * bary.x\n"
             "                  + patch[1].normal * bary.y\n"
             "                  + patch[2].normal * bary.z;\n"
             "    float2 uv = patch[0].uv * bary.x\n"
             "              + patch[1].uv * bary.y\n"
             "              + patch[2].uv * bary.z;\n"
             "    float displacement = %s.SampleLevel(%s, uv, 0.0f).x;\n"
             "    pos.xyz += normal * displacement * 0.5f;\n"
             "    float4 world = mul(dxbc_ObjectToWorld,\n"
             "                       float4(pos.xyz, 1.0f));\n"
             "    output.pos = mul(dxbc_MatrixVP, world);\n"
             "    output.uv = uv;\n"
             "    return output;\n"
             "}\n",
             entry_point, texture, sampler);
}

bool hlsl_emit_exact_tessellation_stage(HLSLEmitterContext *ctx,
                                        const char *entry_point) {
  if (!ctx || !ctx->program || !ctx->sb || !entry_point ||
      entry_point[0] == '\0' ||
      !hlsl_exact_tessellation_lift_matches(ctx->program)) {
    if (ctx) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                     HLSL_EMIT_PHASE_TESSELLATION_EMISSION,
                     HLSL_EMIT_REASON_INTERNAL_INVARIANT);
    }
    return false;
  }
  emit_comments_and_icb(ctx);
  emit_resources(ctx);
  if (!sb_ok(ctx->sb)) return false;
  if (ctx->program->program_type == DXBC_PROGRAM_TYPE_HULL)
    emit_hull_stage(ctx, entry_point);
  else
    emit_domain_stage(ctx, entry_point);
  return sb_ok(ctx->sb);
}
