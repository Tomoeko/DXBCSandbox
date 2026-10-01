// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_identifier.h"
#include "common/sha256.h"
#include "hlsl_geometry_flow.h"
#include "translation/usil_validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* interpolation_modifier(uint8_t mode) {
  switch (mode) {
    case 1: return "nointerpolation ";
    case 3: return "centroid ";
    case 4: return "noperspective ";
    case 5: return "noperspective centroid ";
    case 6: return "sample ";
    case 7: return "noperspective sample ";
    default: return "";
  }
}

static unsigned signature_width(const DXBCSignatureElement *element) {
  unsigned width = 0;
  for (unsigned component = 0; component < 4; ++component)
    if (element->mask & (1u << component)) ++width;
  return width;
}

bool hlsl_high_level_struct_interface_supported(const USILProgram *program, HLSLEmitMode mode) {
  if (!program || mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      (program->program_type != DXBC_PROGRAM_TYPE_VERTEX && program->program_type != DXBC_PROGRAM_TYPE_PIXEL) ||
      program->output_count < 2 || program->output_count > HLSL_SM5_IO_REGISTER_COUNT ||
      program->instruction_count < 2 || !program->instructions || !program->outputs ||
      program->instructions[program->instruction_count - 1].opcode != USIL_OP_RET) return false;
  bool written[HLSL_SM5_IO_REGISTER_COUNT] = {false};
  for (int output = 0; output < program->output_count; ++output) {
    const DXBCSignatureElement *element = &program->outputs[output];
    if (element->component_type != 3 || !element->mask || element->mask > 15 ||
        (element->mask & (element->mask + 1u)) || element->min_precision ||
        element->register_id >= HLSL_SM5_IO_REGISTER_COUNT) return false;
    for (int previous = 0; previous < output; ++previous)
      if (program->outputs[previous].register_id == element->register_id) return false;
  }
  for (int index = 0; index + 1 < program->instruction_count; ++index) {
    const USILInstruction *instruction = &program->instructions[index];
    if (instruction->opcode == USIL_OP_NOP) continue;
    if (!hlsl_expression_effects_supported(program, instruction)) return false;
    for (int operand = 0; operand < instruction->operand_count; ++operand) {
      const DXBCOperand *value = &instruction->operands[operand];
      if (value->type != OPERAND_TYPE_OUTPUT) continue;
      USILOperandUseInfo use;
      if (!usil_instruction_operand_use(program, instruction, operand, &use) ||
          use.use != USIL_OPERAND_USE_DESTINATION) return false;
      int found = -1;
      for (int output = 0; output < program->output_count; ++output)
        if (program->outputs[output].register_id == (uint32_t)value->register_index) found = output;
      if (found < 0 || written[found] ||
          usil_operand_destination_lane_mask(value) != program->outputs[found].mask) return false;
      written[found] = true;
    }
  }
  for (int output = 0; output < program->output_count; ++output)
    if (!written[output]) return false;
  return true;
}

static bool natural_signature_registers(const DXBCSignatureElement *signature, int count,
                                       uint32_t *registers) {
  if (count < 0 || count > HLSL_SM5_IO_REGISTER_COUNT || (count && !signature)) return false;
  *registers = 0;
  for (int index = 0; index < count; ++index) {
    const DXBCSignatureElement *element = &signature[index];
    if (element->component_type != 3 || !element->mask || element->mask > 15 ||
        (element->mask & (element->mask + 1u)) || element->min_precision ||
        element->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
        !dxbc_signature_semantic_name(element)[0]) return false;
    uint32_t bit = UINT32_C(1) << element->register_id;
    if (*registers & bit) return false;
    *registers |= bit;
  }
  return true;
}

static bool same_semantic_base(const char *left, const char *right) {
  while (*left && *right) {
    unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return *left == *right;
}

bool hlsl_natural_input_layout_supported(const USILProgram *program, bool *has_packed) {
  if (has_packed) *has_packed = false;
  if (!program || program->input_count < 0 || program->input_count > HLSL_SM5_IO_REGISTER_COUNT ||
      program->input_alloc < program->input_count || (program->input_count && !program->inputs))
    return false;
  unsigned counts[HLSL_SM5_IO_REGISTER_COUNT] = {0};
  uint8_t masks[HLSL_SM5_IO_REGISTER_COUNT] = {0};
  for (int index = 0; index < program->input_count; ++index) {
    const DXBCSignatureElement *field = &program->inputs[index];
    if (!hlsl_signature_semantic_storage_valid(field) || field->component_type != 3 ||
        !field->mask || field->mask > 15 || field->min_precision ||
        field->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
        (masks[field->register_id] & field->mask)) return false;
    masks[field->register_id] |= field->mask;
    if (++counts[field->register_id] > 2u) return false;
  }
  unsigned pairs = 0;
  for (int index = 0; index < program->input_count; ++index) {
    const DXBCSignatureElement *field = &program->inputs[index];
    if (counts[field->register_id] == 1u) {
      if (field->mask & (field->mask + 1u)) return false;
      continue;
    }
    if (field->mask != 7 && field->mask != 8 && field->mask != 3 && field->mask != 12) return false;
    if (field->system_value || field->stream_index || field->rw_mask != field->mask || field->interpolation_mode > 7 ||
        !hlsl_custom_zero_index_semantic_supported(dxbc_signature_semantic_name(field))) return false;
    for (int other = 0; other < index; ++other) {
      const DXBCSignatureElement *previous = &program->inputs[other];
      if (previous->register_id == field->register_id) {
        if (previous->interpolation_mode != field->interpolation_mode ||
            masks[field->register_id] != 15 || ++pairs > 1u) return false;
      }
    }
  }
  if (!pairs) return true;
  /* The extra layout is authority for one natural IF input interface only.
   * Outputs and every other stage retain their existing layout rules. */
  if ((program->program_type != DXBC_PROGRAM_TYPE_VERTEX && program->program_type != DXBC_PROGRAM_TYPE_PIXEL) ||
      !program->has_stage_contract || !program->has_parsed_signature_authority ||
      program->geometry.valid || program->tessellation.valid || program->compute.valid ||
      program->output_count < 1 || program->output_count > HLSL_SM5_IO_REGISTER_COUNT ||
      program->output_alloc < program->output_count || !program->outputs ||
      program->patch_constant_count || program->signature_declaration_count < 0 ||
      program->signature_declaration_count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT ||
      program->signature_declaration_alloc < program->signature_declaration_count ||
      (program->signature_declaration_count && !program->signature_declarations)) return false;
  for (int index = 0; index < program->output_count; ++index)
    if (!hlsl_signature_semantic_storage_valid(&program->outputs[index])) return false;
  for (int index = 0; index < program->input_count; ++index) {
    const DXBCSignatureElement *field = &program->inputs[index];
    const char *semantic = dxbc_signature_semantic_name(field);
    if (!field->system_value && !hlsl_custom_zero_index_semantic_supported(semantic)) return false;
    for (int other = 0; other < index; ++other)
      if (field->semantic_index == program->inputs[other].semantic_index &&
          same_semantic_base(semantic, dxbc_signature_semantic_name(&program->inputs[other]))) return false;
  }
  if (!usil_signature_authority_is_valid(program)) return false;
  if (has_packed) *has_packed = true;
  return true;
}

bool hlsl_natural_input_projection(const USILProgram *program, const DXBCOperand *operand,
    uint8_t demanded_lanes, HLSLNaturalInputProjection *projection) {
  if (!projection) return false;
  memset(projection, 0, sizeof(*projection));
  bool packed_layout;
  if (!hlsl_natural_input_layout_supported(program, &packed_layout) || !operand ||
      operand->type != OPERAND_TYPE_INPUT || !hlsl_lift_operand_is_plain(operand) ||
      operand->register_index < 0 || operand->register_index >= HLSL_SM5_IO_REGISTER_COUNT ||
      operand->register_index_dim != 1 || !operand->index_has_immediate[0] ||
      operand->index_representations[0] || operand->index_value_exceeds_int[0] ||
      operand->index_values[0] != (uint32_t)operand->register_index ||
      !demanded_lanes || (demanded_lanes & ~15u)) return false;
  int match = -1;
  unsigned register_fields = 0;
  for (int index = 0; index < program->input_count; ++index) {
    const DXBCSignatureElement *field = &program->inputs[index];
    if (field->register_id != (uint32_t)operand->register_index) continue;
    ++register_fields;
    bool member = true;
    for (int lane = 0; lane < 4; ++lane) if (demanded_lanes & (1u << lane)) {
      const int selected = usil_operand_source_component(operand, lane);
      if (selected < 0 || selected >= 4 || !(field->mask & (1u << selected))) member = false;
    }
    if (!member) continue;
    if (match >= 0) return false;
    match = index;
  }
  if (match < 0) return false;
  const DXBCSignatureElement *field = &program->inputs[match];
  HLSLNaturalInputProjection result = {0};
  result.field_index = match;
  result.register_index = field->register_id;
  result.field_mask = field->mask;
  result.natural_components = (uint8_t)signature_width(field);
  result.packed = packed_layout && register_fields == 2u;
  result.logical_value_id = result.packed
      ? HLSL_NATURAL_INPUT_FIELD_LOGICAL_ID_BASE | (uint64_t)(unsigned)match
      : (UINT64_C(1) << 63) | field->register_id;
  for (int lane = 0; lane < 4; ++lane) if (demanded_lanes & (1u << lane)) {
    const int selected = usil_operand_source_component(operand, lane);
    unsigned local = 0;
    for (int physical = 0; physical < selected; ++physical)
      if (field->mask & (1u << physical)) ++local;
    result.selected_components[result.result_components++] = (uint8_t)local;
  }
  *projection = result;
  return true;
}

/* Static vertex indices retain distinct value identity and are checked against
 * the independently decoded input primitive/array extent. Dynamic indexing
 * and primitive/system-value operands remain outside this route. */
static const DXBCSignatureElement *geometry_input_signature(
    const USILProgram *program, const DXBCOperand *operand) {
  if (!program || !operand || operand->type != OPERAND_TYPE_INPUT ||
      operand->register_index_dim != 2 || operand->register_index < 0 ||
      (uint32_t)operand->register_index >= program->geometry.input_vertex_count ||
      !operand->index_has_immediate[0] || !operand->index_has_immediate[1] ||
      operand->index_representations[0] || operand->index_representations[1] ||
      operand->index_value_exceeds_int[0] || operand->index_value_exceeds_int[1] ||
      operand->index_values[0] != (uint32_t)operand->register_index ||
      operand->index_values[0] >= program->geometry.input_vertex_count ||
      operand->index_values[1] >= HLSL_SM5_IO_REGISTER_COUNT ||
      operand->rel_offset0 != (int)operand->index_values[1] ||
      operand->rel_op0 || operand->rel_op1 || operand->rel_op2) return NULL;
  const DXBCSignatureElement *match = NULL;
  for (int input = 0; input < program->input_count; ++input) {
    const DXBCSignatureElement *element = &program->inputs[input];
    if (element->register_id != operand->index_values[1]) continue;
    if (match) return NULL;
    match = element;
  }
  return match;
}

/* Domain coordinates and factor roles are fixed by the tessellator ABI, not
 * by a particular interpolation graph or authored field names. */
bool hlsl_domain_shape(DXBCTessellatorDomain domain, HLSLDomainShape *shape) {
  if (!shape) return false;
  *shape = (HLSLDomainShape){0};
  switch (domain) {
  case DXBC_TESSELLATOR_DOMAIN_TRIANGLE:
    *shape = (HLSLDomainShape){.attribute = "tri", .coordinate_count = 3,
        .outer_count = 3, .inner_count = 1,
        .outer_system_values = {13, 13, 13}, .inner_system_values = {14},
        .raw_outer_siv_names = {17, 18, 19}, .raw_inner_siv_names = {20}};
    return true;
  case DXBC_TESSELLATOR_DOMAIN_QUAD:
    *shape = (HLSLDomainShape){.attribute = "quad", .coordinate_count = 2,
        .outer_count = 4, .inner_count = 2,
        .outer_system_values = {11, 11, 11, 11}, .inner_system_values = {12, 12},
        .raw_outer_siv_names = {11, 12, 13, 14}, .raw_inner_siv_names = {15, 16}};
    return true;
  case DXBC_TESSELLATOR_DOMAIN_ISOLINE:
    *shape = (HLSLDomainShape){.attribute = "isoline", .coordinate_count = 2,
        .outer_count = 2, .outer_system_values = {16, 15},
        .raw_outer_siv_names = {22, 21}};
    return true;
  default:
    return false;
  }
}

/* Preserve the actual register order of complete semantic factor arrays.
 * PCSG record order is immaterial, but inner/outer groups may appear in either
 * field order. Split, repeated or incomplete groups have no source authority. */
bool hlsl_domain_factor_order(const USILProgram *program, bool *inner_first) {
  HLSLDomainShape shape;
  if (!program || !inner_first ||
      (program->program_type != DXBC_PROGRAM_TYPE_HULL && program->program_type != DXBC_PROGRAM_TYPE_DOMAIN) ||
      !hlsl_domain_shape(program->tessellation.domain, &shape)) return false;
  const int count = shape.outer_count + shape.inner_count;
  if (program->patch_constant_count != count || program->patch_constant_alloc < count ||
      !program->patch_constants) return false;
  const DXBCSignatureElement *ordered[6] = {0};
  for (int row = 0; row < count; ++row) {
    const DXBCSignatureElement *field = &program->patch_constants[row];
    if (field->register_id >= (uint32_t)count || ordered[field->register_id]) return false;
    ordered[field->register_id] = field;
  }
  *inner_first = shape.inner_count && ordered[0]->system_value == shape.inner_system_values[0];
  for (int reg = 0; reg < count; ++reg) {
    const bool inner = *inner_first ? reg < shape.inner_count : reg >= shape.outer_count;
    const unsigned first = inner ? (*inner_first ? 0u : shape.outer_count) : (*inner_first ? shape.inner_count : 0u);
    const unsigned semantic = (unsigned)reg - first;
    const uint32_t system = inner ? shape.inner_system_values[semantic] : shape.outer_system_values[semantic];
    const DXBCSignatureElement *field = ordered[reg];
    if (!field || field->component_type != 3 || field->mask != 1 ||
        field->rw_mask != (program->program_type == DXBC_PROGRAM_TYPE_HULL ? 14 : 0) ||
        field->min_precision || field->stream_index || field->system_value != system ||
        field->semantic_index != semantic) return false;
  }
  return true;
}

/* Static patch-point identities and fixed tessellator coordinates retain the
 * independently parsed stage/declaration authority. No phase source synthesis. */
const DXBCSignatureElement *hlsl_high_level_domain_point_signature(
    const USILProgram *program, const DXBCOperand *operand) {
  if (!operand || !hlsl_float_source_modifier_supported(operand)) return NULL;
  DXBCOperand unmodified = *operand;
  unmodified.has_abs = unmodified.has_neg = false;
  unmodified.extended_tokens = NULL;
  unmodified.extended_token_count = 0;
  if (!program || program->program_type != DXBC_PROGRAM_TYPE_DOMAIN ||
      !program->tessellation.valid || operand->type != OPERAND_TYPE_INPUT_CONTROL_POINT ||
      !hlsl_lift_operand_is_plain(&unmodified) || operand->register_index_dim != 2 ||
      operand->register_index < 0 || operand->rel_offset0 < 0 ||
      !operand->index_has_immediate[0] || !operand->index_has_immediate[1] ||
      operand->index_representations[0] || operand->index_representations[1] ||
      operand->index_value_exceeds_int[0] || operand->index_value_exceeds_int[1] ||
      operand->index_values[0] != (uint32_t)operand->register_index ||
      operand->index_values[1] != (uint32_t)operand->rel_offset0 ||
      operand->index_values[0] >= program->tessellation.input_control_point_count) return NULL;
  const DXBCSignatureElement *match = NULL;
  for (int input = 0; input < program->input_count; ++input) {
    const DXBCSignatureElement *element = &program->inputs[input];
    if (element->register_id != operand->index_values[1]) continue;
    if (match) return NULL;
    match = element;
  }
  return match;
}

bool hlsl_custom_zero_index_semantic_supported(const char *semantic) {
  if (!hlsl_source_identifier_valid(semantic) ||
      ((semantic[0] == 'S' || semantic[0] == 's') &&
       (semantic[1] == 'V' || semantic[1] == 'v') && semantic[2] == '_')) return false;
  const size_t length = strlen(semantic);
  /* HLSL splits a decimal suffix into an index. Retain the actual index-zero
   * custom field instead of silently changing its parsed name/index pair. */
  return !(semantic[length - 1] >= '0' && semantic[length - 1] <= '9');
}

/* This is an interface construction, not a fabricated full-width instruction.
 * The two consecutive output writes retain independent decoded owners. */
static bool domain_output_shape(const USILProgram *program, HLSLDomainOutputPlan *plan) {
  if (!plan) return false;
  memset(plan, 0, sizeof(*plan));
  HLSLDomainShape shape;
  if (!program || program->program_type != DXBC_PROGRAM_TYPE_DOMAIN ||
      !hlsl_domain_shape(program->tessellation.domain, &shape) ||
      !program->inputs || program->input_count != 1 || program->input_alloc < 1 ||
      program->inputs[0].mask != 7 || program->inputs[0].rw_mask != 7 ||
      !hlsl_custom_zero_index_semantic_supported(dxbc_signature_semantic_name(&program->inputs[0])) ||
      program->inputs[0].system_value || !program->outputs || program->output_count != 1 ||
      program->output_alloc < 1 || program->outputs[0].mask != 15 ||
      program->outputs[0].rw_mask || program->outputs[0].system_value != 1 ||
      program->outputs[0].semantic_name_extended ||
      !memchr(program->outputs[0].semantic_name, 0, sizeof(program->outputs[0].semantic_name)) ||
      !program->instructions || program->instruction_count < 3 ||
      program->instruction_count > HLSL_DOMAIN_SOURCE_INSTRUCTION_LIMIT ||
      program->instruction_alloc < program->instruction_count) return false;
  const int first = program->instruction_count - 3;
  const USILInstruction *vector = &program->instructions[first];
  const USILInstruction *scalar = &program->instructions[first + 1];
  const USILInstruction *returned = &program->instructions[first + 2];
  if (vector->opcode != USIL_OP_MAD || vector->operand_count != 4 ||
      scalar->opcode != USIL_OP_MOV || scalar->operand_count != 2 ||
      returned->opcode != USIL_OP_RET || returned->operand_count ||
      scalar->operands[1].type != OPERAND_TYPE_IMMEDIATE32 ||
      scalar->operands[1].imm_value_count != 1 ||
      !hlsl_lift_operand_is_plain(&scalar->operands[1])) return false;
  HLSLDomainOutputPlan candidate = {0};
  candidate.present = true;
  candidate.output = program->outputs[0];
  candidate.return_instruction_index = first + 2;
  candidate.return_source_instruction_index = returned->source_instruction_index;
  for (unsigned piece = 0; piece < HLSL_DOMAIN_OUTPUT_PIECE_COUNT; ++piece) {
    const USILInstruction *instruction = &program->instructions[first + (int)piece];
    const DXBCOperand *destination = &instruction->operands[0];
    const uint8_t mask = piece ? 8 : 7;
    if (!usil_instruction_shape_valid(program, instruction) ||
        destination->type != OPERAND_TYPE_OUTPUT ||
        !hlsl_lift_operand_is_plain(destination) ||
        destination->register_index != (int)candidate.output.register_id ||
        usil_operand_destination_lane_mask(destination) != mask) return false;
    candidate.pieces[piece] = (HLSLDomainOutputPiece){
        .instruction_index = first + (int)piece,
        .source_instruction_index = instruction->source_instruction_index,
        .opcode = instruction->opcode,
        .destination_register = candidate.output.register_id,
        .destination_raw_token = destination->raw_token,
        .mask = mask, .width = piece ? 1 : 3,
        .scalar_immediate = piece != 0,
        .immediate_bits = piece ? scalar->operands[1].imm_values[0] : 0};
  }
  for (int index = 0; index < first; ++index) {
    const USILInstruction *instruction = &program->instructions[index];
    if (!usil_instruction_shape_valid(program, instruction)) return false;
    for (int operand = 0; operand < instruction->operand_count; ++operand)
      if (instruction->operands[operand].type == OPERAND_TYPE_OUTPUT) return false;
  }
  *plan = candidate;
  return true;
}

bool hlsl_domain_output_plan_prepare(const USILProgram *program, HLSLDomainOutputPlan *plan) {
  if (!plan) return false;
  memset(plan, 0, sizeof(*plan));
  return hlsl_high_level_domain_interface_supported(program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) &&
      domain_output_shape(program, plan);
}

bool hlsl_domain_output_plans_equal(const HLSLDomainOutputPlan *left,
                                  const HLSLDomainOutputPlan *right) {
  if (!left || !right || left->present != right->present ||
      left->output_signature_index != right->output_signature_index ||
      left->return_instruction_index != right->return_instruction_index ||
      left->return_source_instruction_index != right->return_source_instruction_index) return false;
  const DXBCSignatureElement *a = &left->output, *b = &right->output;
  if (a->semantic_name_extended || b->semantic_name_extended ||
      memcmp(a->semantic_name, b->semantic_name, sizeof(a->semantic_name)) ||
      a->semantic_name_length != b->semantic_name_length ||
      a->semantic_index != b->semantic_index || a->system_value != b->system_value ||
      a->component_type != b->component_type || a->register_id != b->register_id ||
      a->mask != b->mask || a->rw_mask != b->rw_mask ||
      a->stream_index != b->stream_index || a->min_precision != b->min_precision ||
      a->interpolation_mode != b->interpolation_mode) return false;
  for (unsigned piece = 0; piece < HLSL_DOMAIN_OUTPUT_PIECE_COUNT; ++piece) {
    const HLSLDomainOutputPiece *x = &left->pieces[piece], *y = &right->pieces[piece];
    if (x->instruction_index != y->instruction_index ||
        x->source_instruction_index != y->source_instruction_index || x->opcode != y->opcode ||
        x->destination_register != y->destination_register || x->destination_raw_token != y->destination_raw_token ||
        x->mask != y->mask || x->width != y->width || x->scalar_immediate != y->scalar_immediate ||
        x->immediate_bits != y->immediate_bits) return false;
  }
  return true;
}

bool hlsl_high_level_domain_interface_supported(const USILProgram *program, HLSLEmitMode mode) {
  HLSLDomainShape shape;
  if (!program || !hlsl_domain_shape(program->tessellation.domain, &shape)) return false;
  const int factor_count = shape.outer_count + shape.inner_count;
  const uint8_t coordinate_mask = (uint8_t)((1u << shape.coordinate_count) - 1u);
  if (!program || mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      !program->has_stage_contract || !program->has_parsed_signature_authority ||
      program->program_type != DXBC_PROGRAM_TYPE_DOMAIN || !program->tessellation.valid ||
      program->shader_model_major != 5 || program->shader_model_minor ||
      !memchr(program->shader_type_model, 0, sizeof(program->shader_type_model)) ||
      strcmp(program->shader_type_model, "ds_5_0") ||
      !program->tessellation.input_control_point_count ||
      program->tessellation.input_control_point_count > 32 ||
      program->tessellation.output_control_point_count || program->tessellation.phase_count ||
      program->tessellation.partitioning || program->tessellation.output_primitive ||
      program->tessellation.has_max_tessellation_factor || program->tessellation.max_tessellation_factor_bits ||
      program->geometry.valid || program->compute.valid ||
      program->input_count != 1 || program->output_count != 1 || program->patch_constant_count != factor_count ||
      !program->inputs || !program->outputs || !program->patch_constants ||
      program->input_alloc < 1 || program->output_alloc < 1 || program->patch_constant_alloc < factor_count ||
      program->cbuffer_count || program->texture_count || program->sampler_count || program->uav_count ||
      program->icb_value_count || program->indexable_temp_count || program->index_range_count ||
      program->instruction_count < 2 || program->instruction_count > HLSL_DOMAIN_SOURCE_INSTRUCTION_LIMIT ||
      program->instruction_alloc < program->instruction_count || !program->instructions ||
      !usil_signature_authority_is_valid(program)) return false;
  const bool constructed_output = program->inputs[0].mask == 7;
  HLSLDomainOutputPlan output_plan;
  if (constructed_output && !domain_output_shape(program, &output_plan)) return false;
  for (int direction = 0; direction < 2; ++direction) {
    const DXBCSignatureElement *field = direction ? program->outputs : program->inputs;
    if (field->component_type != 3 || field->mask != (!direction && constructed_output ? 7 : 15) ||
        field->system_value != (!direction && constructed_output ? 0u : 1u) ||
        field->register_id >= HLSL_SM5_IO_REGISTER_COUNT || field->min_precision ||
        field->stream_index || field->semantic_index || field->interpolation_mode) return false;
  }
  bool inner_first;
  if (!hlsl_domain_factor_order(program, &inner_first)) return false;
  bool location = false, points = false, output = false;
  uint8_t location_mask = 0;
  for (int index = 0; index < program->signature_declaration_count; ++index) {
    const USILSignatureDeclaration *d = &program->signature_declarations[index];
    if (d->operand_type == OPERAND_TYPE_DOMAIN_LOCATION) {
      if (location || d->kind != USIL_SIGNATURE_DECL_INPUT || d->has_signature_register ||
          !d->mask || (d->mask & (uint8_t)~coordinate_mask)) return false;
      location = true; location_mask = d->mask;
    } else if (d->operand_type == OPERAND_TYPE_INPUT_CONTROL_POINT) {
      if (points || d->kind != USIL_SIGNATURE_DECL_INPUT || !d->has_array_element_count ||
          d->array_element_count != program->tessellation.input_control_point_count || d->register_id != program->inputs[0].register_id || d->mask != program->inputs[0].mask)
        return false;
      points = true;
    } else if (d->operand_type == OPERAND_TYPE_OUTPUT) {
      if (output || d->kind != USIL_SIGNATURE_DECL_OUTPUT_SIV || d->system_value_name != 1 ||
          d->register_id != program->outputs[0].register_id || d->mask != 15) return false;
      output = true;
    } else return false;
  }
  if (!location || !points || !output || program->instructions[program->instruction_count - 1].opcode != USIL_OP_RET)
    return false;
  unsigned writes = 0;
  for (int index = 0; index + 1 < program->instruction_count; ++index) {
    const USILInstruction *inst = &program->instructions[index];
    if ((inst->opcode != USIL_OP_MOV && inst->opcode != USIL_OP_ADD && inst->opcode != USIL_OP_MUL &&
         inst->opcode != USIL_OP_MAD) || !usil_instruction_shape_valid(program, inst) ||
        !hlsl_expression_effects_supported(program, inst) || inst->saturate || inst->precise_mask) return false;
    for (int operand = 0; operand < inst->operand_count; ++operand) {
      const DXBCOperand *value = &inst->operands[operand];
      DXBCOperand unmodified = *value;
      if (operand) {
        if (!hlsl_float_source_modifier_supported(value)) return false;
        unmodified.has_abs = unmodified.has_neg = false;
        unmodified.extended_tokens = NULL;
        unmodified.extended_token_count = 0;
      }
      if (!hlsl_lift_operand_is_plain(&unmodified)) return false;
      if (!operand) {
        if (value->type == OPERAND_TYPE_OUTPUT) {
          ++writes;
          if (!constructed_output && (writes != 1 || index != program->instruction_count - 2 ||
              value->register_index != (int)program->outputs[0].register_id ||
              usil_operand_destination_lane_mask(value) != 15)) return false;
        } else if (value->type != OPERAND_TYPE_TEMP) return false;
      } else if (value->type == OPERAND_TYPE_INPUT_CONTROL_POINT) {
        if (!hlsl_high_level_domain_point_signature(program, value)) return false;
      } else if (value->type == OPERAND_TYPE_DOMAIN_LOCATION) {
        if (value->register_index_dim || value->index_has_immediate[0] || value->index_has_immediate[1]) return false;
        USILOperandUseInfo use;
        if (!usil_instruction_operand_use(program, inst, operand, &use) || use.use != USIL_OPERAND_USE_SOURCE) return false;
        for (int lane = 0; lane < 4; ++lane) if (use.source_lane_mask & (1u << lane)) {
          const int selected = usil_operand_source_component(value, lane);
          if (selected < 0 || selected >= shape.coordinate_count || !(location_mask & (1u << selected))) return false;
        }
      } else if (value->type != OPERAND_TYPE_TEMP && value->type != OPERAND_TYPE_IMMEDIATE32) return false;
    }
  }
  return writes == (constructed_output ? (unsigned)HLSL_DOMAIN_OUTPUT_PIECE_COUNT : 1u);
}

static bool geometry_effect_supported(const USILProgram *program,
                                      const USILInstruction *instruction) {
  USILGeometryEffectKind kind;
  if (instruction->opcode == USIL_OP_GEOMETRY_APPEND)
    kind = USIL_GEOMETRY_EFFECT_APPEND;
  else if (instruction->opcode == USIL_OP_GEOMETRY_RESTART_STRIP)
    kind = USIL_GEOMETRY_EFFECT_RESTART_STRIP;
  else return false;
  USILEffectFlags effects;
  if (!usil_instruction_effects(program, instruction, &effects) ||
      effects != USIL_EFFECT_GEOMETRY_OUTPUT || instruction->geometry_effect != kind ||
      instruction->geometry_stream_id != 0 || instruction->saturate || instruction->precise_mask ||
      instruction->geometry_stream_explicit != (program->geometry.declared_stream_mask == 1u))
    return false;
  if (!instruction->geometry_stream_explicit) return instruction->operand_count == 0;
  if (instruction->operand_count != 1) return false;
  const DXBCOperand *stream = &instruction->operands[0];
  return stream->type == OPERAND_TYPE_STREAM && hlsl_lift_operand_is_plain(stream) &&
      stream->register_index_dim == 1 && stream->register_index == 0 &&
      stream->index_has_immediate[0] && stream->index_representations[0] == 0 &&
      !stream->index_value_exceeds_int[0] && stream->index_values[0] == 0;
}

bool hlsl_high_level_geometry_interface_supported(const USILProgram *program, HLSLEmitMode mode) {
  if (hlsl_geometry_control_flow_admission(program, mode)) return true;
  if (!program || mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      !program->has_stage_contract || !program->has_parsed_signature_authority ||
      program->program_type != DXBC_PROGRAM_TYPE_GEOMETRY ||
      !program->geometry.valid || !program->geometry.output_tuple_state_persists ||
      !dxbc_geometry_input_vertex_count(program->geometry.input_primitive) ||
      program->geometry.input_vertex_count !=
          dxbc_geometry_input_vertex_count(program->geometry.input_primitive) ||
      program->geometry.has_instance_count ||
      program->geometry.instance_count != 1 || program->geometry.referenced_stream_mask != 1 ||
      program->geometry.declared_stream_mask > 1 || !program->geometry.max_output_vertex_count ||
      program->geometry.max_output_vertex_count > 1024 ||
      (program->geometry.output_topology != DXBC_OUTPUT_TOPOLOGY_POINT_LIST &&
       program->geometry.output_topology != DXBC_OUTPUT_TOPOLOGY_LINE_STRIP &&
       program->geometry.output_topology != DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP) ||
      program->input_count < 1 || program->output_count < 1 ||
      program->instruction_count < 2 ||
      /* Geometry statement coverage currently uses one 64-bit word. */
      program->instruction_count > 64 ||
      program->instruction_alloc < program->instruction_count || !program->instructions ||
      program->texture_count || program->sampler_count || program->uav_count ||
      program->icb_value_count || program->indexable_temp_count || program->index_range_count ||
      program->patch_constant_count || !usil_signature_authority_is_valid(program)) return false;
  if (!memchr(program->shader_type_model, '\0', sizeof(program->shader_type_model)) ||
      (program->shader_model_major != 4 && program->shader_model_major != 5) ||
      (program->shader_model_major == 4 && program->shader_model_minor > 1) ||
      (program->shader_model_major == 5 && program->shader_model_minor) ||
      program->compute.valid || program->tessellation.valid) return false;
  char model[32];
  snprintf(model, sizeof(model), "gs_%u_%u", program->shader_model_major, program->shader_model_minor);
  if (strcmp(program->shader_type_model, model) != 0) return false;
  uint32_t inputs, outputs;
  if (!natural_signature_registers(program->inputs, program->input_count, &inputs) ||
      !natural_signature_registers(program->outputs, program->output_count, &outputs)) return false;
  for (int input = 0; input < program->input_count; ++input)
    if (program->inputs[input].stream_index) return false;
  for (int output = 0; output < program->output_count; ++output)
    if (program->outputs[output].stream_index) return false;
  for (int declaration = 0; declaration < program->signature_declaration_count; ++declaration) {
    DXBCOperandType type = program->signature_declarations[declaration].operand_type;
    if (type != OPERAND_TYPE_INPUT && type != OPERAND_TYPE_OUTPUT) return false;
  }
  uint32_t initialized_outputs = 0;
  size_t effects = 0;
  uint32_t appends = 0;
  for (int index = 0; index < program->instruction_count; ++index) {
    const USILInstruction *instruction = &program->instructions[index];
    if (instruction->saturate || instruction->precise_mask ||
        !usil_instruction_shape_valid(program, instruction)) return false;
    if (geometry_effect_supported(program, instruction)) {
      ++effects;
      if (instruction->opcode == USIL_OP_GEOMETRY_APPEND) {
        if (initialized_outputs != outputs) return false;
        ++appends;
      }
      continue;
    }
    if (instruction->geometry_effect != USIL_GEOMETRY_EFFECT_NONE) return false;
    if (instruction->opcode == USIL_OP_RET) {
      if (index + 1 != program->instruction_count) return false;
      continue;
    }
    if (instruction->opcode == USIL_OP_NOP) continue;
    if (instruction->opcode != USIL_OP_MOV && instruction->opcode != USIL_OP_ADD &&
        instruction->opcode != USIL_OP_MUL && instruction->opcode != USIL_OP_MAD &&
        instruction->opcode != USIL_OP_DIV && instruction->opcode != USIL_OP_MIN &&
        instruction->opcode != USIL_OP_MAX) return false;
    if (!hlsl_expression_effects_supported(program, instruction)) return false;
    for (int operand = 0; operand < instruction->operand_count; ++operand) {
      const DXBCOperand *value = &instruction->operands[operand];
      USILOperandUseInfo use;
      if (!usil_instruction_operand_use(program, instruction, operand, &use)) return false;
      if (value->type == OPERAND_TYPE_INPUT && !geometry_input_signature(program, value))
        return false;
      if (value->type != OPERAND_TYPE_OUTPUT) continue;
      if (use.use != USIL_OPERAND_USE_DESTINATION || !hlsl_lift_operand_is_plain(value)) return false;
      const DXBCSignatureElement *field = NULL;
      for (int output = 0; output < program->output_count; ++output)
        if (program->outputs[output].register_id == (uint32_t)value->register_index)
          field = &program->outputs[output];
      if (!field || usil_operand_destination_lane_mask(value) != field->mask) return false;
      initialized_outputs |= UINT32_C(1) << field->register_id;
    }
  }
  /* Append and Cut retain the output tuple. Only an actual field write changes
   * it; in particular, neither effect clears initialized_outputs above. */
  return appends > 0 && appends <= program->geometry.max_output_vertex_count &&
      effects == program->geometry.effect_count &&
      program->instructions[program->instruction_count - 1].opcode == USIL_OP_RET;
}

bool hlsl_high_level_geometry_effect_supported(const HLSLEmitterContext *ctx, int instruction) {
  return ctx && ctx->high_level_geometry && ctx->program && instruction >= 0 &&
      instruction < ctx->program->instruction_count &&
      geometry_effect_supported(ctx->program, &ctx->program->instructions[instruction]);
}

bool hlsl_emit_high_level_geometry_effect(HLSLEmitterContext *ctx, int instruction) {
  if (!hlsl_high_level_geometry_effect_supported(ctx, instruction) ||
      !ctx->high_level_interface_prepared || !ctx->high_level_geometry_stream_variable[0] ||
      !ctx->high_level_output_variable[0]) return false;
  const USILInstruction *owner = &ctx->program->instructions[instruction];
  const size_t begin = ctx->sb->len;
  sb_append_spaces(ctx->sb, ctx->indent);
  if (owner->opcode == USIL_OP_GEOMETRY_APPEND)
    sb_appendf(ctx->sb, "%s.Append(%s);\n", ctx->high_level_geometry_stream_variable,
               ctx->high_level_output_variable);
  else sb_appendf(ctx->sb, "%s.RestartStrip();\n", ctx->high_level_geometry_stream_variable);
  hlsl_source_quality_emission(ctx, 0, true, instruction);
  if (!sb_ok(ctx->sb) || ctx->sb->len <= begin) return false;
  ctx->high_level_geometry_statements_emitted |= UINT64_C(1) << instruction;
  return true;
}

static bool natural_interface_input_registers(const HLSLEmitterContext *ctx, uint32_t *registers) {
  if (!ctx->high_level_packed_inputs)
    return natural_signature_registers(ctx->program->inputs, ctx->program->input_count, registers);
  bool packed;
  if (!ctx->natural_structured_owners_guarded ||
      !hlsl_natural_input_layout_supported(ctx->program, &packed) || !packed) return false;
  *registers = 0;
  for (int input = 0; input < ctx->program->input_count; ++input)
    *registers |= UINT32_C(1) << ctx->program->inputs[input].register_id;
  return true;
}

bool hlsl_source_quality_interface_inventory_supported(const HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->program || ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      !ctx->high_level_interface || !ctx->high_level_interface_prepared || ctx->unity_uv_helper ||
      !ctx->program->has_stage_contract ||
      (ctx->program->program_type != DXBC_PROGRAM_TYPE_VERTEX &&
       ctx->program->program_type != DXBC_PROGRAM_TYPE_PIXEL &&
       !ctx->high_level_geometry && !ctx->high_level_domain)) return false;
  const USILProgram *program = ctx->program;
  uint32_t inputs, outputs;
  if (!natural_interface_input_registers(ctx, &inputs) ||
      !natural_signature_registers(program->outputs, program->output_count, &outputs) ||
      !outputs) return false;
  for (int input = 0; input < program->input_count; ++input)
    if (!(ctx->high_level_packed_inputs ? hlsl_high_level_input_field_name(ctx, input)
        : hlsl_high_level_input_name(ctx, (int)program->inputs[input].register_id))) return false;
  if (ctx->high_level_domain && (!hlsl_high_level_domain_interface_supported(program, ctx->emit_mode) ||
      !ctx->high_level_domain_point_type[0] || !ctx->high_level_domain_patch_variable[0] ||
      !ctx->high_level_domain_location_variable[0] || !ctx->high_level_domain_factors_type[0] ||
      !ctx->high_level_domain_factors_variable[0])) return false;
  if (ctx->high_level_direct_return) return program->output_count == 1;
  if (!ctx->high_level_output_type[0] || !ctx->high_level_output_variable[0]) return false;
  if (ctx->high_level_geometry) {
    if (!hlsl_high_level_geometry_interface_supported(program, ctx->emit_mode) ||
        !ctx->high_level_geometry_input_type[0] || !ctx->high_level_geometry_input_variable[0] ||
        !ctx->high_level_geometry_stream_variable[0]) return false;
  } else {
    /* The new IF plan is frozen before callbacks. Its actual body and return
     * still require independent replay before entry completion. */
    if (!hlsl_high_level_struct_interface_supported(program, ctx->emit_mode) &&
        !ctx->natural_structured_owners_guarded) return false;
  }
  for (int output = 0; output < program->output_count; ++output)
    if (!hlsl_high_level_output_name(ctx, (int)program->outputs[output].register_id)) return false;
  return true;
}

bool hlsl_source_quality_interface_inventory_complete(const HLSLEmitterContext *ctx) {
  if (!hlsl_source_quality_interface_inventory_supported(ctx)) return false;
  uint32_t inputs, outputs;
  if (!natural_interface_input_registers(ctx, &inputs) ||
      !natural_signature_registers(ctx->program->outputs, ctx->program->output_count, &outputs))
    return false;
  if (ctx->high_level_input_parameters_emitted != inputs ||
      ctx->high_level_output_statements_emitted != outputs ||
      !ctx->high_level_entry_signature_emitted || !ctx->high_level_return_block_emitted)
    return false;
  if (ctx->high_level_packed_inputs) {
    const uint32_t fields = ctx->program->input_count == HLSL_SM5_IO_REGISTER_COUNT
        ? UINT32_MAX : (UINT32_C(1) << (unsigned)ctx->program->input_count) - 1u;
    if (ctx->high_level_input_fields_emitted != fields) return false;
  }
  if (ctx->high_level_domain && (!ctx->high_level_domain_point_struct_emitted ||
      ctx->high_level_domain_point_fields_emitted != inputs || !ctx->high_level_domain_attribute_emitted ||
      !ctx->high_level_domain_patch_parameter_emitted || !ctx->high_level_domain_location_parameter_emitted ||
      !ctx->high_level_domain_factors_struct_emitted || ctx->high_level_domain_factor_fields_emitted !=
          ((UINT32_C(1) << ctx->program->patch_constant_count) - 1u) ||
      !ctx->high_level_domain_factors_parameter_emitted))
    return false;
  if (ctx->high_level_direct_return)
    return !ctx->high_level_output_struct_emitted && !ctx->high_level_result_local_emitted &&
           !ctx->high_level_output_fields_emitted;
  if (ctx->high_level_geometry) {
    uint64_t expected_statements = 0;
    for (int instruction = 0; instruction < ctx->program->instruction_count; ++instruction) {
      const USILInstruction *owner = &ctx->program->instructions[instruction];
      if (owner->geometry_effect != USIL_GEOMETRY_EFFECT_NONE ||
          (owner->operand_count && owner->operands[0].type == OPERAND_TYPE_OUTPUT))
        expected_statements |= UINT64_C(1) << instruction;
    }
    if (!ctx->high_level_geometry_input_struct_emitted ||
        ctx->high_level_geometry_input_fields_emitted != inputs ||
        !ctx->high_level_geometry_attribute_emitted ||
        !ctx->high_level_geometry_stream_parameter_emitted ||
        ctx->high_level_geometry_statements_emitted != expected_statements) return false;
  }
  return ctx->high_level_output_struct_emitted && ctx->high_level_result_local_emitted &&
         ctx->high_level_output_fields_emitted == outputs;
}

void hlsl_source_quality_interface_expression_begin(HLSLEmitterContext *ctx, int instruction) {
  if (!ctx || !ctx->high_level_interface || instruction < 0 ||
      instruction >= ctx->program->instruction_count) return;
  const USILInstruction *owner = &ctx->program->instructions[instruction];
  if (!owner->operand_count || owner->operands[0].type != OPERAND_TYPE_OUTPUT) return;
  ctx->high_level_statement_expression_begin = ctx->sb->len;
  ctx->high_level_statement_instruction = instruction;
}

void hlsl_source_quality_interface_statement_emitted(HLSLEmitterContext *ctx, int instruction) {
  if (!ctx || !ctx->high_level_interface || instruction < 0 ||
      instruction >= ctx->program->instruction_count ||
      ctx->high_level_statement_instruction != instruction || !sb_ok(ctx->sb) ||
      ctx->sb->len <= ctx->high_level_statement_expression_begin) return;
  const USILInstruction *owner = &ctx->program->instructions[instruction];
  if (!owner->operand_count || owner->operands[0].type != OPERAND_TYPE_OUTPUT ||
      !hlsl_lift_operand_is_plain(&owner->operands[0])) return;
  for (int output = 0; output < ctx->program->output_count; ++output) {
    const DXBCSignatureElement *element = &ctx->program->outputs[output];
    if (element->register_id != (uint32_t)owner->operands[0].register_index ||
        element->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
        !usil_operand_destination_lane_mask(&owner->operands[0]) ||
        (usil_operand_destination_lane_mask(&owner->operands[0]) & ~element->mask)) continue;
    /* Partial writes need an independently planned complete value: the flow
     * route proves every Append; DOMAIN assembles its exact XYZ/W suffix. */
    if (usil_operand_destination_lane_mask(&owner->operands[0]) != element->mask &&
        !hlsl_geometry_control_flow_admission(ctx->program, ctx->emit_mode)) {
      HLSLDomainOutputPlan current;
      if (!ctx->domain_output_plan.present ||
          instruction != ctx->domain_output_plan.pieces[1].instruction_index ||
          !hlsl_domain_output_plan_prepare(ctx->program, &current) ||
          !hlsl_domain_output_plans_equal(&current, &ctx->domain_output_plan)) continue;
    }
    ctx->high_level_output_statements_emitted |= UINT32_C(1) << element->register_id;
    if (ctx->high_level_geometry)
      ctx->high_level_geometry_statements_emitted |= UINT64_C(1) << instruction;
    ctx->high_level_statement_instruction = -1;
    return;
  }
}

const char *hlsl_high_level_output_name(const HLSLEmitterContext *ctx, int reg) {
  if (!ctx || !ctx->high_level_interface || ctx->high_level_direct_return || reg < 0 ||
      reg >= HLSL_SM5_IO_REGISTER_COUNT || !ctx->high_level_output_names[reg][0]) return NULL;
  return ctx->high_level_output_names[reg];
}

bool hlsl_append_high_level_output(HLSLEmitterContext *ctx, const DXBCOperand *destination) {
  if (!ctx || !destination || destination->type != OPERAND_TYPE_OUTPUT ||
      !hlsl_lift_operand_is_plain(destination)) return false;
  const char *name = hlsl_high_level_output_name(ctx, destination->register_index);
  if (!name) return false;
  for (int output = 0; output < ctx->program->output_count; ++output) {
    const DXBCSignatureElement *element = &ctx->program->outputs[output];
    if (element->register_id != (uint32_t)destination->register_index) continue;
    if (usil_operand_destination_lane_mask(destination) != element->mask) return false;
    sb_appendf(ctx->sb, "%s.%s", ctx->high_level_output_variable, name);
    return sb_ok(ctx->sb);
  }
  return false;
}

const DXBCSignatureElement *hlsl_high_level_input_signature(
    const HLSLEmitterContext *ctx, int register_index) {
  if (!ctx || !ctx->high_level_interface || register_index < 0 ||
      register_index >= HLSL_SM5_IO_REGISTER_COUNT) return NULL;
  const DXBCSignatureElement *match = NULL;
  for (int input = 0; input < ctx->program->input_count; ++input) {
    const DXBCSignatureElement *element = &ctx->program->inputs[input];
    if (element->register_id != (uint32_t)register_index) continue;
    if (match) return NULL;
    match = element;
  }
  return match;
}

const DXBCSignatureElement *hlsl_high_level_input_operand_signature(
    const HLSLEmitterContext *ctx, const DXBCOperand *operand) {
  if (ctx && ctx->high_level_domain && operand && operand->type == OPERAND_TYPE_INPUT_CONTROL_POINT)
    return hlsl_high_level_domain_point_signature(ctx->program, operand);
  if (!ctx || !ctx->high_level_interface || !operand || operand->type != OPERAND_TYPE_INPUT)
    return NULL;
  if (ctx->high_level_geometry) return geometry_input_signature(ctx->program, operand);
  if (operand->register_index_dim != 1 || !operand->index_has_immediate[0] ||
      operand->index_representations[0] || operand->index_value_exceeds_int[0] ||
      operand->index_values[0] != (uint32_t)operand->register_index ||
      operand->rel_op0 || operand->rel_op1 || operand->rel_op2) return NULL;
  return hlsl_high_level_input_signature(ctx, operand->register_index);
}

const char *hlsl_high_level_input_name(const HLSLEmitterContext *ctx, int register_index) {
  const DXBCSignatureElement *field = hlsl_high_level_input_signature(ctx, register_index);
  return field ? hlsl_high_level_input_field_name(ctx, (int)(field - ctx->program->inputs)) : NULL;
}

const char *hlsl_high_level_input_field_name(const HLSLEmitterContext *ctx, int field_index) {
  if (!ctx || !ctx->high_level_interface || field_index < 0 ||
      field_index >= ctx->program->input_count || field_index >= HLSL_SM5_IO_REGISTER_COUNT ||
      !ctx->high_level_input_names[field_index][0]) return NULL;
  return ctx->high_level_input_names[field_index];
}

static const char *semantic_input_role(const char *semantic) {
  if (strcmp(semantic, "POSITION") == 0) return "position";
  if (strcmp(semantic, "NORMAL") == 0) return "normal";
  if (strcmp(semantic, "TANGENT") == 0) return "tangent";
  if (strcmp(semantic, "COLOR") == 0) return "color";
  if (strcmp(semantic, "TEXCOORD") == 0) return "texcoord";
  if (strcmp(semantic, "SV_Position") == 0) return "systemPosition";
  if (strcmp(semantic, "BLENDWEIGHT") == 0) return "blendWeight";
  if (strcmp(semantic, "BLENDINDICES") == 0) return "blendIndices";
  return NULL;
}

bool hlsl_high_level_name_available(const HLSLEmitterContext *ctx, const char *name) {
  if (!ctx || !name || !name[0]) return false;
  if (ctx->entry_point_name && strcmp(ctx->entry_point_name, name) == 0) return false;
  for (size_t identifier = 0; identifier < ctx->reserved_preprocessor_identifier_count; ++identifier)
    if (strcmp(ctx->reserved_preprocessor_identifiers[identifier], name) == 0) return false;
  for (unsigned input = 0; input < HLSL_SM5_IO_REGISTER_COUNT; ++input)
    if (strcmp(ctx->high_level_input_names[input], name) == 0) return false;
  for (unsigned output = 0; output < HLSL_SM5_IO_REGISTER_COUNT; ++output)
    if (strcmp(ctx->high_level_output_names[output], name) == 0) return false;
  if (strcmp(ctx->high_level_output_type, name) == 0 ||
      strcmp(ctx->high_level_output_variable, name) == 0 ||
      strcmp(ctx->high_level_geometry_input_type, name) == 0 ||
      strcmp(ctx->high_level_geometry_input_variable, name) == 0 ||
      strcmp(ctx->high_level_geometry_stream_variable, name) == 0 ||
      strcmp(ctx->high_level_domain_point_type, name) == 0 ||
      strcmp(ctx->high_level_domain_patch_variable, name) == 0 ||
      strcmp(ctx->high_level_domain_location_variable, name) == 0 ||
      strcmp(ctx->high_level_domain_factors_type, name) == 0 ||
      strcmp(ctx->high_level_domain_factors_variable, name) == 0) return false;
  for (int buffer = 0; buffer < ctx->cbuffer_layout_count; ++buffer) {
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[buffer];
    if (layout->declaration_name && strcmp(layout->declaration_name, name) == 0) return false;
    for (int variable = 0; variable < layout->variable_count; ++variable)
      if (layout->variables[variable].name &&
          strcmp(layout->variables[variable].name, name) == 0) return false;
  }
  for (int texture = 0; texture < ctx->program->texture_count; ++texture) {
    const USILTexture *resource = &ctx->program->textures[texture];
    SerializedResourceType kind = strcmp(resource->dimension, "raw") == 0 ||
        strcmp(resource->dimension, "structured") == 0
            ? SERIALIZED_RESOURCE_BUFFER : SERIALIZED_RESOURCE_TEXTURE;
    const char *resource_name = NULL;
    if (!resolve_srv_name_ctx(ctx, resource->reg_idx, kind, &resource_name)) return false;
    if (resource_name && strcmp(resource_name, name) == 0) return false;
  }
  for (int sampler = 0; sampler < ctx->program->sampler_count; ++sampler) {
    int reg = ctx->program->samplers[sampler].reg_idx;
    if (reg >= 0 && reg < HLSL_SM5_SAMPLER_REGISTER_COUNT && ctx->sampler_names[reg] &&
        strcmp(ctx->sampler_names[reg], name) == 0) return false;
  }
  return true;
}

bool hlsl_allocate_interface_name(HLSLEmitterContext *ctx, const char *base, char destination[96]) {
  if (!base || !base[0] || strlen(base) > 80) return false;
  for (size_t index = 0; base[index]; ++index) {
    unsigned char value = (unsigned char)base[index];
    if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
          value == '_' || (index && value >= '0' && value <= '9'))) return false;
  }
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    char candidate[96];
    if (attempt) {
      if (!hlsl_format_checked(ctx, candidate, sizeof(candidate), "%s_%u", base, attempt)) return false;
    } else if (!hlsl_copy_checked(ctx, candidate, sizeof(candidate), base)) return false;
    if (!hlsl_high_level_name_available(ctx, candidate)) continue;
    return hlsl_copy_checked(ctx, destination, 96, candidate);
  }
  return false;
}

bool hlsl_prepare_high_level_interface(HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->high_level_interface) return false;
  ctx->high_level_interface_prepared = false;
  ctx->high_level_statement_instruction = -1;
  if (ctx->program->input_count > HLSL_SM5_IO_REGISTER_COUNT) goto unsupported;
  bool packed = false;
  if ((ctx->program->program_type == DXBC_PROGRAM_TYPE_VERTEX ||
       ctx->program->program_type == DXBC_PROGRAM_TYPE_PIXEL) &&
      hlsl_natural_input_layout_supported(ctx->program, &packed) && packed) {
    /* A candidate hint cannot authorize packed parameter syntax. Repeat the
     * complete shared CFG/SSA plan before any field name or source is emitted. */
    if (!hlsl_natural_structured_preflight(ctx)) goto unsupported;
    ctx->high_level_packed_inputs = true;
  }
  for (int input = 0; input < ctx->program->input_count; ++input) {
    const DXBCSignatureElement *element = &ctx->program->inputs[input];
    if (element->component_type != 3 || !element->mask || element->mask > 15 ||
        (!ctx->high_level_packed_inputs && (element->mask & (element->mask + 1u))) || element->min_precision ||
        element->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
        ctx->high_level_input_names[input][0]) goto unsupported;
    if (!ctx->high_level_packed_inputs)
      for (int previous = 0; previous < input; ++previous)
        if (ctx->program->inputs[previous].register_id == element->register_id) goto unsupported;
    char base[80];
    const char *semantic = dxbc_signature_semantic_name(element);
    const char *role = semantic_input_role(semantic);
    if ((ctx->high_level_geometry || ctx->high_level_domain) && element->system_value == 1) role = "clipPosition";
    if (!role) {
      if (!hlsl_format_checked(ctx, base, sizeof(base), "attribute%d", input)) return false;
    } else if (strcmp(semantic, "TEXCOORD") == 0 || element->semantic_index) {
      if (!hlsl_format_checked(ctx, base, sizeof(base), "%s%u", role,
                                element->semantic_index)) return false;
    } else if (!hlsl_copy_checked(ctx, base, sizeof(base), role)) {
      return false;
    }
    if (!hlsl_allocate_interface_name(ctx, base, ctx->high_level_input_names[input])) goto unsupported;
  }
  if (ctx->high_level_geometry &&
      (!hlsl_allocate_interface_name(ctx, ctx->preferred_input_struct_name,
                                ctx->high_level_geometry_input_type) ||
       !hlsl_allocate_interface_name(ctx, "input", ctx->high_level_geometry_input_variable) ||
       !hlsl_allocate_interface_name(ctx, "stream", ctx->high_level_geometry_stream_variable)))
    goto unsupported;
  if (ctx->high_level_domain &&
      (!hlsl_allocate_interface_name(ctx, ctx->preferred_input_struct_name, ctx->high_level_domain_point_type) ||
       !hlsl_allocate_interface_name(ctx, "patch", ctx->high_level_domain_patch_variable) ||
       !hlsl_allocate_interface_name(ctx, ctx->program->tessellation.domain == DXBC_TESSELLATOR_DOMAIN_TRIANGLE ?
           "barycentric" : "coordinates", ctx->high_level_domain_location_variable) ||
       !hlsl_allocate_interface_name(ctx, "DomainFactors", ctx->high_level_domain_factors_type) ||
       !hlsl_allocate_interface_name(ctx, "factors", ctx->high_level_domain_factors_variable))) goto unsupported;
  if (!ctx->high_level_direct_return) {
    if (!hlsl_allocate_interface_name(ctx, ctx->preferred_output_struct_name,
                                ctx->high_level_output_type) ||
        !hlsl_allocate_interface_name(ctx, "output", ctx->high_level_output_variable)) goto unsupported;
    for (int output = 0; output < ctx->program->output_count; ++output) {
      const DXBCSignatureElement *element = &ctx->program->outputs[output];
      const char *semantic = dxbc_signature_semantic_name(element);
      const char *role = semantic_input_role(semantic);
      if (element->system_value == 1) role = "clipPosition";
      else if (element->system_value == 64) role = "color";
      char base[80];
      if (!role) {
        if (!hlsl_format_checked(ctx, base, sizeof(base), "attribute%d", output)) return false;
      } else if (strcmp(semantic, "TEXCOORD") == 0 || element->semantic_index) {
        if (!hlsl_format_checked(ctx, base, sizeof(base), "%s%u", role, element->semantic_index)) return false;
      } else if (!hlsl_copy_checked(ctx, base, sizeof(base), role)) return false;
      if (!hlsl_allocate_interface_name(ctx, base, ctx->high_level_output_names[element->register_id])) goto unsupported;
    }
  }
  ctx->high_level_interface_prepared = true;
  return true;

unsupported:
  hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_UNSUPPORTED, HLSL_EMIT_PHASE_INTERFACE_EMISSION,
                 HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
  return false;
}

bool hlsl_high_level_input_provenance(HLSLEmitterContext *ctx,
    const DXBCOperand *operand, uint8_t demanded_lanes, ASTOperandProvenance *provenance) {
  if (!ctx || !operand || !provenance || !demanded_lanes ||
      (demanded_lanes & ~15u) || !hlsl_lift_operand_is_plain(operand)) return false;
  const bool location = ctx->high_level_domain &&
      operand->type == OPERAND_TYPE_DOMAIN_LOCATION;
  if (operand->type != OPERAND_TYPE_INPUT &&
      !(ctx->high_level_domain && operand->type == OPERAND_TYPE_INPUT_CONTROL_POINT) &&
      !location) return false;
  HLSLNaturalInputProjection projection = {0};
  const bool packed_input = ctx->high_level_packed_inputs && operand->type == OPERAND_TYPE_INPUT;
  if (packed_input && !hlsl_natural_input_projection(ctx->program, operand, demanded_lanes, &projection))
    return false;
  const DXBCSignatureElement *element = packed_input ? &ctx->program->inputs[projection.field_index]
      : hlsl_high_level_input_operand_signature(ctx, operand);
  if (!location && (!element ||
      !(packed_input ? hlsl_high_level_input_field_name(ctx, projection.field_index)
                     : hlsl_high_level_input_name(ctx, (int)element->register_id)))) return false;
  HLSLDomainShape shape = {0};
  if (location && (operand->register_index_dim ||
      !hlsl_domain_shape(ctx->program->tessellation.domain, &shape))) return false;
  ast_operand_provenance_init(provenance);
  provenance->complete = true;
  provenance->value_role = AST_OPERAND_VALUE_LOGICAL;
  provenance->logical_value_id = packed_input ? projection.logical_value_id : (UINT64_C(1) << 63) |
      (location ? (UINT64_C(1) << 62) : element->register_id);
  if (ctx->high_level_geometry || (ctx->high_level_domain && !location))
    provenance->logical_value_id |= (uint64_t)operand->index_values[0] << 32u;
  provenance->natural_components = location ? shape.coordinate_count : (uint8_t)signature_width(element);
  for (int component = 0; component < 4; ++component) {
    if (!(demanded_lanes & (1u << component))) continue;
    int selected = packed_input ? projection.selected_components[provenance->result_components]
                               : usil_operand_source_component(operand, component);
    if (selected < 0 || selected >= provenance->natural_components) return false;
    provenance->selected_components[provenance->result_components++] = (uint8_t)selected;
  }
  bool same = true, ascending = true, identity = true;
  for (unsigned component = 0; component < provenance->result_components; ++component) {
    unsigned selected = provenance->selected_components[component];
    same = same && selected == provenance->selected_components[0];
    identity = identity && selected == component;
    if (component) ascending = ascending && selected > provenance->selected_components[component - 1];
  }
  if (same && provenance->result_components > 1)
    provenance->result_components = 1;
  identity = true;
  for (unsigned component = 0; component < provenance->result_components; ++component)
    identity = identity && provenance->selected_components[component] == component;
  if (identity && provenance->result_components == provenance->natural_components)
    provenance->selection_role = AST_COMPONENT_SELECTION_NONE;
  else
    provenance->selection_role = (same || ascending) ? AST_COMPONENT_SELECTION_SEMANTIC
                                                     : AST_COMPONENT_SELECTION_TRANSPORT;
  const int instruction = ctx->current_instruction_index;
  if (instruction >= 0 && instruction < ctx->program->instruction_count) {
    provenance->instruction_index = instruction;
    provenance->source_instruction_index = ctx->program->instructions[instruction].source_instruction_index;
    provenance->destination_lanes = demanded_lanes;
    const USILInstruction *owner = &ctx->program->instructions[instruction];
    for (int input = 0; input < owner->operand_count; ++input)
      if (&owner->operands[input] == operand) provenance->operand_index = input;
  }
  return true;
}

static bool is_depth_output(const DXBCSignatureElement *element) {
  const char *semantic = dxbc_signature_semantic_name(element);
  return element->register_id == UINT32_MAX &&
         semantic &&
         (strcmp(semantic, "SV_Depth") == 0 ||
          strcmp(semantic, "SV_DepthGreaterEqual") == 0 ||
          strcmp(semantic, "SV_DepthLessEqual") == 0);
}

static bool output_variable_name(HLSLEmitterContext *ctx,
                                 const DXBCSignatureElement *element,
                                 char *name, size_t name_size) {
  if (element->register_id != UINT32_MAX) {
    return hlsl_format_checked(ctx, name, name_size, "o%u",
                               element->register_id);
  } else if (is_depth_output(element)) {
    return hlsl_copy_checked(ctx, name, name_size, "oDepth");
  }
  return hlsl_format_checked(ctx, name, name_size, "o_%s",
                             dxbc_signature_semantic_name(element));
}

bool hlsl_output_field_name(HLSLEmitterContext *ctx,
                            const DXBCSignatureElement *element,
                            int element_index, char *name,
                            size_t name_size) {
  if (!ctx || !element || element_index < 0 ||
      element_index >= ctx->program->output_count) return false;
  bool duplicate_register = false;
  for (int previous = 0; previous < element_index; ++previous) {
    if (ctx->program->outputs[previous].register_id == element->register_id) {
      duplicate_register = true;
      break;
    }
  }
  if (!duplicate_register)
    return output_variable_name(ctx, element, name, name_size);
  const bool omit_index = element->semantic_index == 0 &&
                          strcmp(dxbc_signature_semantic_name(element),
                                 "TEXCOORD") != 0;
  if (omit_index)
    return hlsl_format_checked(ctx, name, name_size, "o_%s",
                               dxbc_signature_semantic_name(element));
  return hlsl_format_checked(ctx, name, name_size, "o_%s%u",
                             dxbc_signature_semantic_name(element),
                             element->semantic_index);
}

static const char *geometry_input_primitive_name(DXBCInputPrimitive value) {
  switch (value) {
    case DXBC_INPUT_PRIMITIVE_POINT: return "point";
    case DXBC_INPUT_PRIMITIVE_LINE: return "line";
    case DXBC_INPUT_PRIMITIVE_TRIANGLE: return "triangle";
    case DXBC_INPUT_PRIMITIVE_LINE_ADJACENCY: return "lineadj";
    case DXBC_INPUT_PRIMITIVE_TRIANGLE_ADJACENCY: return "triangleadj";
    default: return NULL;
  }
}

static const char *geometry_stream_type_name(DXBCOutputTopology value) {
  switch (value) {
    case DXBC_OUTPUT_TOPOLOGY_POINT_LIST: return "PointStream";
    case DXBC_OUTPUT_TOPOLOGY_LINE_STRIP: return "LineStream";
    case DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP: return "TriangleStream";
    default: return NULL;
  }
}

static void geometry_flow_interface_receipt(HLSLEmitterContext *ctx,
    HLSLGeometryFlowInterfaceKind kind, int field_index, size_t begin) {
  if (!hlsl_geometry_control_flow_record_interface(ctx, kind, field_index, begin))
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
                   HLSL_EMIT_PHASE_INTERFACE_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
}

enum { NATURAL_STRUCTURED_HEADER_BYTE_LIMIT = 16384 };

static bool natural_structured_append_prefix_matches(const HLSLEmitterContext *ctx) {
  const StringBuilder *source = ctx->sb;
  if (!source || !sb_ok(source) || ctx->natural_structured_append_prefix_length > source->len ||
      (source->buf ? source->len >= source->capacity || source->buf[source->len] != '\0'
                   : source->len || source->capacity)) return false;
  uint8_t digest[32];
  common_sha256(source->buf, ctx->natural_structured_append_prefix_length, digest);
  return !memcmp(digest, ctx->natural_structured_append_prefix_digest, sizeof(digest));
}

static bool prepare_natural_structured_header(HLSLEmitterContext *ctx,
    const char *input_struct, const char *output_struct) {
  if (!ctx->natural_structured_owners_guarded || !ctx->high_level_interface_prepared ||
      ctx->natural_structured_header_source.buf || ctx->natural_structured_header_source.len ||
      !ctx->entry_point_name || !natural_structured_append_prefix_matches(ctx)) return false;
  uint8_t digest[32];
  if (!hlsl_natural_structured_owned_contract_digest(ctx->program, digest) ||
      memcmp(digest, ctx->natural_structured_owner_digest, sizeof(digest))) return false;
  HLSLEmitterContext *scratch = malloc(sizeof(*scratch));
  if (!scratch) return false;
  *scratch = *ctx;
  StringBuilder expected;
  sb_init(&expected);
  HLSLEmitDiagnostic diagnostic;
  hlsl_emit_diagnostic_init(&diagnostic);
  scratch->sb = &expected;
  scratch->diagnostic = &diagnostic;
  scratch->source_quality_analysis = NULL;
  scratch->source_quality_root = NULL;
  scratch->source_quality_forward_observer = NULL;
  scratch->source_quality_forward_observer_context = NULL;
  scratch->expression_source_map = NULL;
  scratch->matrix_use_capture = NULL;
  scratch->stage_coverage = NULL;
  scratch->natural_structured_body_inventory = NULL;
  scratch->natural_structured_owners_guarded = false;
  scratch->natural_structured_header_source = (StringBuilder){0};
  scratch->natural_structured_header_replay = true;
  scratch->high_level_input_parameters_emitted = 0;
  scratch->high_level_input_fields_emitted = 0;
  scratch->high_level_output_fields_emitted = 0;
  scratch->high_level_output_struct_emitted = false;
  scratch->high_level_result_local_emitted = false;
  scratch->high_level_entry_signature_emitted = false;
  emit_io_structs(scratch, input_struct, output_struct);
  if (sb_ok(&expected)) emit_entry_point_declarations(scratch, ctx->entry_point_name,
      input_struct, output_struct, NULL, NULL);
  const bool prepared = sb_ok(&expected) && expected.len &&
      expected.len <= (size_t)NATURAL_STRUCTURED_HEADER_BYTE_LIMIT &&
      diagnostic.status == HLSL_EMIT_STATUS_OK && scratch->high_level_entry_signature_emitted;
  free(scratch); /* No borrowed analysis, map or receipt is disposed here. */
  if (!prepared) { sb_free(&expected); return false; }
  ctx->natural_structured_header_begin = ctx->sb->len;
  common_sha256(ctx->sb->buf, ctx->natural_structured_header_begin,
      ctx->natural_structured_header_prefix_digest);
  ctx->natural_structured_header_source = expected;
  return true;
}

bool hlsl_natural_structured_header_matches(const HLSLEmitterContext *ctx) {
  if (!ctx) return false;
  if (!ctx->natural_structured_owners_guarded && !ctx->high_level_packed_inputs) return true;
  const StringBuilder *expected = &ctx->natural_structured_header_source;
  if (!(ctx->natural_structured_owners_guarded && !ctx->natural_structured_header_replay &&
      natural_structured_append_prefix_matches(ctx) &&
      sb_ok(expected) && expected->buf && expected->len &&
      expected->len <= (size_t)NATURAL_STRUCTURED_HEADER_BYTE_LIMIT && expected->len < expected->capacity &&
      expected->buf[expected->len] == '\0' && ctx->sb && sb_ok(ctx->sb) && ctx->sb->buf &&
      ctx->sb->len < ctx->sb->capacity && ctx->natural_structured_header_begin <= ctx->sb->len &&
      expected->len <= ctx->sb->len - ctx->natural_structured_header_begin &&
      !memcmp(expected->buf, ctx->sb->buf + ctx->natural_structured_header_begin, expected->len))) return false;
  uint8_t digest[32];
  common_sha256(ctx->sb->buf, ctx->natural_structured_header_begin, digest);
  return !memcmp(digest, ctx->natural_structured_header_prefix_digest, sizeof(digest));
}

void emit_io_structs(HLSLEmitterContext* ctx, const char* input_struct, const char* output_struct) {
  if ((ctx->natural_structured_owners_guarded || ctx->high_level_packed_inputs) &&
      !ctx->natural_structured_header_replay &&
      !prepare_natural_structured_header(ctx, input_struct, output_struct)) {
    hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_ANALYSIS_FAILED,
        HLSL_EMIT_PHASE_INTERFACE_EMISSION, HLSL_EMIT_REASON_ANALYSIS_CONFLICT);
    return;
  }
  if (ctx->high_level_direct_return && !ctx->high_level_domain) return;
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  if (ctx->high_level_interface) {
    if (ctx->high_level_geometry || ctx->high_level_domain) {
      const size_t input_begin = sb->len;
      sb_appendf(sb, "struct %s {\n", ctx->high_level_domain ? ctx->high_level_domain_point_type : ctx->high_level_geometry_input_type);
      for (int input = 0; input < program->input_count; ++input) {
        const size_t field_begin = input ? sb->len : input_begin;
        const DXBCSignatureElement *element = &program->inputs[input];
        const char *name = hlsl_high_level_input_name(ctx, (int)element->register_id);
        const char *semantic = dxbc_signature_semantic_name(element);
        if (!name) { sb->failed = true; return; }
        sb_appendf(sb, "    %s %s : %s", get_type_str(element->component_type,
            (int)signature_width(element)), name, semantic);
        if (element->semantic_index || strcmp(semantic, "TEXCOORD") == 0)
          sb_appendf(sb, "%u", element->semantic_index);
        sb_append(sb, ";\n");
        hlsl_source_quality_emission(ctx, 0, false, -1);
        geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_FIELD,
                                         input, field_begin);
        if (sb_ok(sb)) {
          if (ctx->high_level_domain) ctx->high_level_domain_point_fields_emitted |= UINT32_C(1) << element->register_id;
          else ctx->high_level_geometry_input_fields_emitted |= UINT32_C(1) << element->register_id;
        }
      }
      const size_t input_end_begin = sb->len;
      sb_append(sb, "};\n\n");
      hlsl_source_quality_emission(ctx, 0, false, -1);
      geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_END,
                                       -1, input_end_begin);
      if (ctx->high_level_domain) ctx->high_level_domain_point_struct_emitted = sb_ok(sb) && sb->len > input_begin;
      else ctx->high_level_geometry_input_struct_emitted = sb_ok(sb) && sb->len > input_begin;
    }
    if (ctx->high_level_domain) {
      size_t factors_begin = sb->len;
      sb_appendf(sb, "struct %s {\n", ctx->high_level_domain_factors_type);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      HLSLDomainShape shape;
      if (!hlsl_domain_shape(program->tessellation.domain, &shape)) { sb->failed = true; return; }
      bool inner_first;
      if (!hlsl_domain_factor_order(program, &inner_first)) { sb->failed = true; return; }
      for (int group = 0; group < (shape.inner_count ? 2 : 1); ++group) {
        const bool inner = inner_first ? group == 0 : group == 1;
        const unsigned count = inner ? shape.inner_count : shape.outer_count;
        const unsigned first = inner ? (inner_first ? 0u : shape.outer_count) : (inner_first ? shape.inner_count : 0u);
        if (inner && count == 1) sb_append(sb, "    float inner : SV_InsideTessFactor;\n");
        else sb_appendf(sb, "    float %s[%u] : %s;\n", inner ? "inner" : "outer", count,
            inner ? "SV_InsideTessFactor" : "SV_TessFactor");
        hlsl_source_quality_emission(ctx, 0, false, -1);
        if (sb_ok(sb)) ctx->high_level_domain_factor_fields_emitted |=
            ((UINT32_C(1) << count) - 1u) << first;
      }
      sb_append(sb, "};\n\n");
      hlsl_source_quality_emission(ctx, 0, false, -1);
      ctx->high_level_domain_factors_struct_emitted = sb_ok(sb) && sb->len > factors_begin;
    }
    if (ctx->high_level_direct_return) return;
    size_t struct_begin = sb->len;
    sb_appendf(sb, "struct %s {\n", ctx->high_level_output_type);
    for (int output = 0; output < program->output_count; ++output) {
      const size_t field_begin = output ? sb->len : struct_begin;
      const DXBCSignatureElement *element = &program->outputs[output];
      const char *name = hlsl_high_level_output_name(ctx, (int)element->register_id);
      const char *semantic = dxbc_signature_semantic_name(element);
      if (!name) { sb->failed = true; return; }
      sb_appendf(sb, "    %s %s : %s", get_type_str(element->component_type,
          (int)signature_width(element)), name, semantic);
      if (element->semantic_index || strcmp(semantic, "TEXCOORD") == 0)
        sb_appendf(sb, "%u", element->semantic_index);
      sb_append(sb, ";\n");
      hlsl_source_quality_emission(ctx, 0, false, -1);
      geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_OUTPUT_FIELD,
                                       output, field_begin);
      if (sb_ok(sb))
        ctx->high_level_output_fields_emitted |= UINT32_C(1) << element->register_id;
    }
    const size_t output_end_begin = sb->len;
    sb_append(sb, "};\n\n");
    hlsl_source_quality_emission(ctx, 0, false, -1);
    geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_OUTPUT_END,
                                     -1, output_end_begin);
    ctx->high_level_output_struct_emitted = sb_ok(sb) && sb->len > struct_begin;
    return;
  }

  // 4. Emit Input/Output structures
  sb_appendf(sb, "struct %s {\n", input_struct);
  for (int i = 0; i < program->input_count; i++) {
    const DXBCSignatureElement *el = &program->inputs[i];
    int comps = 0;
    if (el->mask & 1) comps++;
    if (el->mask & 2) comps++;
    if (el->mask & 4) comps++;
    if (el->mask & 8) comps++;
    if (comps == 0) comps = 4;
    const char *type_str = get_type_str(el->component_type, comps);
    const char *interp = interpolation_modifier(el->interpolation_mode);
    const char *semantic = dxbc_signature_semantic_name(el);
    bool omit_index =
        (el->semantic_index == 0 && strcmp(semantic, "TEXCOORD") != 0);

    // Check if this register_id was already used by a previous input
    bool dup_register = false;
    for (int j = 0; j < i; j++) {
      if (program->inputs[j].register_id == el->register_id) {
        dup_register = true;
        break;
      }
    }

    if (dup_register) {
      if (omit_index) {
        sb_appendf(sb, "    %s%s v_%s : %s;\n", interp, type_str,
                   semantic, semantic);
      } else {
        sb_appendf(sb, "    %s%s v_%s%d : %s%d;\n", interp, type_str,
                   semantic, el->semantic_index,
                   semantic, el->semantic_index);
      }
    } else {
      if (omit_index) {
        sb_appendf(sb, "    %s%s v%u : %s;\n", interp, type_str,
                   el->register_id, semantic);
      } else {
        sb_appendf(sb, "    %s%s v%u : %s%d;\n", interp, type_str,
                   el->register_id, semantic, el->semantic_index);
      }
    }
    hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_SYNTHETIC_INTERFACE, false, -1);
  }
  sb_append(sb, "};\n\n");
  if (program->output_count == 1 && !ctx->is_vertex && !ctx->is_geometry) {
    return;
  }

  sb_appendf(sb, "struct %s {\n", output_struct);
  for (int i = 0; i < program->output_count; i++) {
    const DXBCSignatureElement *el = &program->outputs[i];
    int comps = 0;
    if (el->mask & 1)
      comps++;
    if (el->mask & 2)
      comps++;
    if (el->mask & 4)
      comps++;
    if (el->mask & 8)
      comps++;
    if (comps == 0)
      comps = 4;
    const char *type_str = get_type_str(el->component_type, comps);
    char output_name[128];
    if (!output_variable_name(ctx, el, output_name, sizeof(output_name)))
      return;
    const char *semantic = dxbc_signature_semantic_name(el);
    bool omit_index =
        (el->semantic_index == 0 && strcmp(semantic, "TEXCOORD") != 0);
    // Use semantic-based naming to avoid duplicate member names when multiple
    // outputs share the same register (e.g., o2.xy:TEXCOORD0, o2.zw:TEXCOORD1)
    // Check if this register_id was already used by a previous output
    bool dup_register = false;
    for (int j = 0; j < i; j++) {
      if (program->outputs[j].register_id == el->register_id) {
        dup_register = true;
        break;
      }
    }
    if (dup_register) {
      // Use semantic-based naming to avoid conflict
      if (omit_index) {
        sb_appendf(sb, "    %s o_%s : %s;\n", type_str,
                   semantic, semantic);
      } else {
        sb_appendf(sb, "    %s o_%s%d : %s%d;\n", type_str,
                   semantic, el->semantic_index,
                   semantic, el->semantic_index);
      }
    } else if (omit_index) {
      sb_appendf(sb, "    %s %s : %s;\n", type_str, output_name,
                 semantic);
    } else {
      sb_appendf(sb, "    %s %s : %s%d;\n", type_str, output_name,
                 semantic, el->semantic_index);
    }
    hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_SYNTHETIC_INTERFACE, false, -1);
  }
  sb_append(sb, "};\n\n");
}

void emit_entry_point_declarations(HLSLEmitterContext* ctx,
                                          const char* entry_point, const char* input_struct, const char* output_struct,
                                          const bool inputs_used[HLSL_SM5_IO_REGISTER_COUNT],
                                          const bool outputs_used[HLSL_SM5_IO_REGISTER_COUNT]) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  bool is_vertex = ctx->is_vertex;

  if (ctx->high_level_interface) {
    size_t entry_begin = sb->len;
    if (ctx->high_level_domain) {
      HLSLDomainShape shape;
      if (!hlsl_domain_shape(program->tessellation.domain, &shape)) { sb->failed = true; return; }
      sb_appendf(sb, "[domain(\"%s\")]\n", shape.attribute);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      ctx->high_level_domain_attribute_emitted = sb_ok(sb);
      sb_appendf(sb, "float4 %s(%s %s, ", entry_point,
          ctx->high_level_domain_factors_type, ctx->high_level_domain_factors_variable);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      ctx->high_level_domain_factors_parameter_emitted = sb_ok(sb);
      sb_appendf(sb, "const OutputPatch<%s, %u> %s, ",
          ctx->high_level_domain_point_type, (unsigned)program->tessellation.input_control_point_count,
          ctx->high_level_domain_patch_variable);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      ctx->high_level_domain_patch_parameter_emitted = sb_ok(sb);
      for (int input = 0; input < program->input_count; ++input)
        ctx->high_level_input_parameters_emitted |= UINT32_C(1) << program->inputs[input].register_id;
      sb_appendf(sb, "float%u %s : SV_DomainLocation) : SV_POSITION {\n",
          (unsigned)shape.coordinate_count, ctx->high_level_domain_location_variable);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      ctx->high_level_domain_location_parameter_emitted = sb_ok(sb);
      ctx->high_level_entry_signature_emitted = sb_ok(sb) && sb->len > entry_begin;
      return;
    }
    if (ctx->high_level_geometry) {
      const char *stream = geometry_stream_type_name(program->geometry.output_topology);
      if (!stream) { sb->failed = true; return; }
      const size_t attribute_begin = sb->len;
      sb_appendf(sb, "[maxvertexcount(%u)]\n", program->geometry.max_output_vertex_count);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_ATTRIBUTE,
                                       -1, attribute_begin);
      ctx->high_level_geometry_attribute_emitted = sb_ok(sb);
      const char *primitive = geometry_input_primitive_name(program->geometry.input_primitive);
      if (!primitive) { sb->failed = true; return; }
      const size_t input_parameter_begin = sb->len;
      sb_appendf(sb, "void %s(%s %s %s[%u]", entry_point, primitive,
          ctx->high_level_geometry_input_type, ctx->high_level_geometry_input_variable,
          program->geometry.input_vertex_count);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_PARAMETER,
                                       -1, input_parameter_begin);
      if (sb_ok(sb)) {
        for (int input = 0; input < program->input_count; ++input)
          ctx->high_level_input_parameters_emitted |= UINT32_C(1) << program->inputs[input].register_id;
      }
      const size_t stream_parameter_begin = sb->len;
      sb_appendf(sb, ", inout %s<%s> %s)", stream, ctx->high_level_output_type,
          ctx->high_level_geometry_stream_variable);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_STREAM_PARAMETER,
                                       -1, stream_parameter_begin);
      ctx->high_level_geometry_stream_parameter_emitted = sb_ok(sb);
      const size_t function_open_begin = sb->len;
      sb_append(sb, " {\n");
      hlsl_source_quality_emission(ctx, 0, false, -1);
      geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_FUNCTION_OPEN,
                                       -1, function_open_begin);
      ctx->high_level_entry_signature_emitted = sb_ok(sb) && sb->len > entry_begin;
      const size_t result_local_begin = sb->len;
      sb_appendf(sb, "    %s %s;\n", ctx->high_level_output_type, ctx->high_level_output_variable);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      geometry_flow_interface_receipt(ctx, HLSL_GEOMETRY_FLOW_INTERFACE_RESULT_LOCAL,
                                       -1, result_local_begin);
      ctx->high_level_result_local_emitted = sb_ok(sb);
      return;
    }
    const DXBCSignatureElement *output = &program->outputs[0];
    unsigned components = 0;
    for (unsigned lane = 0; lane < 4; ++lane)
      if (output->mask & (1u << lane)) ++components;
    const char *semantic = dxbc_signature_semantic_name(output);
    const char *type = get_type_str(output->component_type, (int)components);
    sb_appendf(sb, "%s %s(", ctx->high_level_direct_return ? type : ctx->high_level_output_type, entry_point);
    for (int input = 0; input < program->input_count; ++input) {
      const DXBCSignatureElement *element = &program->inputs[input];
      const char *input_semantic = dxbc_signature_semantic_name(element);
      const char *name = ctx->high_level_packed_inputs ? hlsl_high_level_input_field_name(ctx, input)
          : hlsl_high_level_input_name(ctx, (int)element->register_id);
      if (!name) { sb->failed = true; return; }
      if (input) sb_append(sb, ", ");
      sb_appendf(sb, "%s%s %s : %s", interpolation_modifier(element->interpolation_mode),
                 get_type_str(element->component_type, (int)signature_width(element)),
                 name, input_semantic);
      if (element->semantic_index || strcmp(input_semantic, "TEXCOORD") == 0)
        sb_appendf(sb, "%u", element->semantic_index);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      if (sb_ok(sb)) {
        ctx->high_level_input_parameters_emitted |= UINT32_C(1) << element->register_id;
        if (ctx->high_level_packed_inputs)
          ctx->high_level_input_fields_emitted |= UINT32_C(1) << (unsigned)input;
      }
    }
    if (ctx->high_level_direct_return) {
      sb_appendf(sb, ") : %s", semantic);
      if (output->semantic_index || strcmp(semantic, "TEXCOORD") == 0)
        sb_appendf(sb, "%u", output->semantic_index);
      sb_append(sb, " {\n");
    } else {
      sb_appendf(sb, ") {\n    %s %s;\n", ctx->high_level_output_type,
                 ctx->high_level_output_variable);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      ctx->high_level_result_local_emitted = sb_ok(sb);
    }
    hlsl_source_quality_emission(ctx, 0, false, -1);
    ctx->high_level_entry_signature_emitted = sb_ok(sb) && sb->len > entry_begin;
    return;
  }

  // 5. Entry point
  if (is_vertex) {
    sb_appendf(sb, "%s %s(%s input) {\n", output_struct, entry_point,
               input_struct);
    sb_appendf(sb, "    %s output;\n", output_struct);
  } else if (ctx->is_geometry) {
    const char *primitive =
        geometry_input_primitive_name(program->geometry.input_primitive);
    const char *stream =
        geometry_stream_type_name(program->geometry.output_topology);
    if (!primitive || !stream) {
      hlsl_emit_fail(ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                     HLSL_EMIT_PHASE_ENTRY_POINT_EMISSION,
                     HLSL_EMIT_REASON_INTERNAL_INVARIANT);
      return;
    }
    sb_appendf(sb, "[maxvertexcount(%u)]\n",
               program->geometry.max_output_vertex_count);
    if (program->geometry.has_instance_count)
      sb_appendf(sb, "[instance(%u)]\n", program->geometry.instance_count);
    sb_appendf(sb, "void %s(%s %s input[%u]",
               entry_point, primitive, input_struct,
               program->geometry.input_vertex_count);
    if (program->geometry.has_instance_count)
      sb_append(sb, ", uint dxbc_instance_id : SV_GSInstanceID");
    sb_appendf(sb, ", inout %s<%s> dxbc_stream) {\n", stream,
               output_struct);
    sb_appendf(sb, "    %s output;\n", output_struct);
  } else if (program->output_count == 1) {
    const DXBCSignatureElement *el = &program->outputs[0];
    int comps = 0;
    if (el->mask & 1) comps++;
    if (el->mask & 2) comps++;
    if (el->mask & 4) comps++;
    if (el->mask & 8) comps++;
    if (comps == 0) comps = 4;
    const char *type_str = get_type_str(el->component_type, comps);
    const char *semantic = dxbc_signature_semantic_name(el);
    bool omit_index =
        (el->semantic_index == 0 && strcmp(semantic, "TEXCOORD") != 0);
    if (omit_index) {
      sb_appendf(sb, "%s %s(%s input) : %s {\n", type_str, entry_point,
                 input_struct, semantic);
    } else {
      sb_appendf(sb, "%s %s(%s input) : %s%d {\n", type_str, entry_point,
                 input_struct, semantic, el->semantic_index);
    }
  } else {
    sb_appendf(sb, "void %s(%s input", entry_point, input_struct);

    for (int i = 0; i < program->output_count; i++) {
      const DXBCSignatureElement *el = &program->outputs[i];
      bool dup = false;
      for (int j = 0; j < i; j++) {
        if (program->outputs[j].register_id == el->register_id) {
          dup = true;
          break;
        }
      }
      if (dup) continue;

      int comps = 0;
      if (el->mask & 1) comps++;
      if (el->mask & 2) comps++;
      if (el->mask & 4) comps++;
      if (el->mask & 8) comps++;
      if (comps == 0) comps = 4;
      const char *type_str = get_type_str(el->component_type, comps);
      char output_name[128];
      if (!output_variable_name(ctx, el, output_name, sizeof(output_name)))
        return;

      const char *semantic = dxbc_signature_semantic_name(el);
      bool omit_index =
          (el->semantic_index == 0 && strcmp(semantic, "TEXCOORD") != 0);
      if (omit_index) {
        sb_appendf(sb, ", out %s %s : %s", type_str, output_name,
                   semantic);
      } else {
        sb_appendf(sb, ", out %s %s : %s%d", type_str, output_name,
                   semantic, el->semantic_index);
      }
    }
    sb_append(sb, ") {\n");
  }

  // 6. Copy Inputs to Local Variables
  bool input_register_declared[HLSL_SM5_IO_REGISTER_COUNT] = {false};
  if (!ctx->is_geometry) for (int i = 0; i < program->input_count; i++) {
    const DXBCSignatureElement *el = &program->inputs[i];
    if (el->register_id < HLSL_SM5_IO_REGISTER_COUNT &&
        !input_register_declared[el->register_id]) {
      const char *type_str = get_type_str(el->component_type, 4);
      sb_appendf(sb, "    %s v%u = (%s)0;\n", type_str,
                 el->register_id, type_str);
      hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
      input_register_declared[el->register_id] = true;
    }
  }

  if (!ctx->is_geometry) for (int i = 0; i < program->input_count; i++) {
    const DXBCSignatureElement *el = &program->inputs[i];
    const char *semantic = dxbc_signature_semantic_name(el);
    bool omit_index =
        (el->semantic_index == 0 && strcmp(semantic, "TEXCOORD") != 0);
    char member_name[128];

    // Check if this register_id was already used by a previous input
    bool dup_register = false;
    for (int j = 0; j < i; j++) {
      if (program->inputs[j].register_id == el->register_id) {
        dup_register = true;
        break;
      }
    }

    if (dup_register) {
      if (omit_index) {
        if (!hlsl_format_checked(ctx, member_name, sizeof(member_name),
                                 "v_%s", semantic)) return;
      } else {
        if (!hlsl_format_checked(ctx, member_name, sizeof(member_name),
                                 "v_%s%d", semantic,
                                 el->semantic_index)) return;
      }
    } else {
      if (!hlsl_format_checked(ctx, member_name, sizeof(member_name), "v%u",
                               el->register_id)) return;
    }

    char swizzle[8] = "";
    int idx = 0;
    if (el->mask & 1) swizzle[idx++] = 'x';
    if (el->mask & 2) swizzle[idx++] = 'y';
    if (el->mask & 4) swizzle[idx++] = 'z';
    if (el->mask & 8) swizzle[idx++] = 'w';
    swizzle[idx] = '\0';

    if (idx > 0) {
      sb_appendf(sb, "    v%u.%s = input.%s;\n", el->register_id,
                 swizzle, member_name);
    } else {
      sb_appendf(sb, "    v%u = input.%s;\n", el->register_id, member_name);
    }
    hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT, false, -1);
  }

  // Declare input fallbacks across the Shader Model 5 input register file.
  if (!ctx->is_geometry)
  for (unsigned int r = 0; r < HLSL_SM5_IO_REGISTER_COUNT; r++) {
    bool declared = false;
    for (int i = 0; i < program->input_count; i++) {
      if (program->inputs[i].register_id == r) {
        declared = true;
        break;
      }
    }
    if (!declared && inputs_used[r]) {
      sb_appendf(sb, "    float4 v%u = (float4)0;\n", r);
      hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
    }
  }

  // Declare Outputs
  bool output_reg_declared[HLSL_SM5_IO_REGISTER_COUNT] = {false};
  for (int i = 0; i < program->output_count; i++) {
    const DXBCSignatureElement *element = &program->outputs[i];
    uint32_t reg = element->register_id;
    if (reg < HLSL_SM5_IO_REGISTER_COUNT && !output_reg_declared[reg]) {
      // Is it in the signature?
      bool is_in_signature =
          (!is_vertex && !ctx->is_geometry && program->output_count > 1);
      if (!is_in_signature) {
        if (ctx->is_geometry) {
          sb_appendf(sb, "    float4 o%u;\n", reg);
        } else {
          /* A native integer backing local is required for integer vertex
           * system values such as SV_RenderTargetArrayIndex: routing those
           * bits through float changes the eventual MOV.  Pixel outputs and
           * mixed/unauthoritative vertex registers retain the established
           * float backing.  Their per-field return path owns any typed
           * conversion, and a register may legally pack signature elements
           * for which no single native vector type is provable. */
          uint32_t component_type = 3;
          if (ctx->is_vertex) {
            uint32_t proven_type =
                hlsl_output_register_component_type(program, reg);
            if (proven_type == 1 || proven_type == 2)
              component_type = proven_type;
          }
          const char *local_type = get_type_str(component_type, 4);
          sb_appendf(sb, "    %s o%u = (%s)0;\n", local_type, reg,
                     local_type);
        }
        hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
        output_reg_declared[reg] = true;
      }
    } else if (is_depth_output(element) && program->output_count == 1) {
      /* A single pixel output is returned directly, so unlike a multi-output
       * entry point it does not arrive as an out parameter. */
      sb_append(sb, "    float oDepth = 0.0f;\n");
    }
  }
  // Declare output fallbacks across the Shader Model 5 output register file.
  for (unsigned int r = 0; r < HLSL_SM5_IO_REGISTER_COUNT; r++) {
    if (outputs_used[r] && !output_reg_declared[r]) {
      // Is it in the signature?
      bool is_in_signature = false;
      if (!is_vertex && !ctx->is_geometry && program->output_count > 1) {
        for (int i = 0; i < program->output_count; i++) {
          if (program->outputs[i].register_id == r) {
            is_in_signature = true;
            break;
          }
        }
      }
      if (!is_in_signature) {
        sb_appendf(sb, "    float4 o%u = (float4)0;\n", r);
        hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
        output_reg_declared[r] = true;
      }
    }
  }

  if (ctx->emit_mode == HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) return;

  // Declare Temps
  if (program->temp_count > 0) {
    for (int i = 0; i < program->temp_count; i++) {
      if (is_register_decomposed(ctx, i)) continue;
      int max_generation = hlsl_register_max_generation(ctx, i);
      if (max_generation > 0) {
        for (int gen = 0; gen <= max_generation; gen++) {
          if (ctx->use_uint_temps) {
            sb_appendf(sb, "    uint4 r%d_%d = (uint4)0;\n", i, gen);
          } else {
            sb_appendf(sb, "    float4 r%d_%d = (float4)0;\n", i, gen);
          }
          hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
        }
      } else {
        if (ctx->use_uint_temps) {
          sb_appendf(sb, "    uint4 r%d = (uint4)0;\n", i);
        } else {
          sb_appendf(sb, "    float4 r%d = (float4)0;\n", i);
        }
        hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
      }
    }
  }

  for (int instruction = 0; instruction < program->instruction_count;
       instruction++) {
    if (!ctx->saved_mul_is_definition[instruction]) continue;
    int components = get_mask_component_count(
        program->instructions[instruction].operands[0].destination_mask);
    const char *type = get_type_str(0, components);
    sb_appendf(sb, "    %s dxbc_saved_mul%d = (%s)0;\n", type,
               ctx->saved_mul_id[instruction] - 1, type);
  }

  // Declare Decomposed Swizzle Registers
  for (int i = 0; i < ctx->temp_state_count; i++) {
    if (hlsl_readability_transforms_enabled(ctx) &&
        ctx->has_decomposition[i]) {
      sb_appendf(sb, "    float2 %s = float2(0.0, 0.0);\n",
                 ctx->decompositions[i].Float2VarName);
      sb_appendf(sb, "    int %s = 0;\n",
                 ctx->decompositions[i].IntVarName);
      hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
      hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
    }
  }

  // Declare Indexable Temps
  for (int i = 0; i < program->indexable_temp_count; i++) {
    if (ctx->use_uint_temps) {
      sb_appendf(sb, "    uint4 x%d[%d];\n",
                 program->indexable_temps[i].reg_idx,
                 program->indexable_temps[i].size);
    } else {
      sb_appendf(sb, "    float4 x%d[%d];\n",
                 program->indexable_temps[i].reg_idx,
                 program->indexable_temps[i].size);
    }
    hlsl_source_quality_emission(ctx, HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE, false, -1);
  }

  // Component-split helpers hold evaluated HLSL arithmetic, not raw DXBC
  // register bits. Integer register storage is converted before reaching them.
  sb_append(sb, "    float u_xlat_temp_x = 0.0f;\n");
  sb_append(sb, "    float u_xlat_temp_y = 0.0f;\n");
  sb_append(sb, "    float u_xlat_temp_z = 0.0f;\n");
  sb_append(sb, "    float u_xlat_temp_w = 0.0f;\n");

  sb_append(sb, "\n");
}
