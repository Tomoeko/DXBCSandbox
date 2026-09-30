// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <stdio.h>
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

bool hlsl_source_quality_interface_inventory_supported(const HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->program || ctx->emit_mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      !ctx->high_level_interface || !ctx->high_level_interface_prepared || ctx->unity_uv_helper ||
      !ctx->program->has_stage_contract ||
      (ctx->program->program_type != DXBC_PROGRAM_TYPE_VERTEX &&
       ctx->program->program_type != DXBC_PROGRAM_TYPE_PIXEL)) return false;
  const USILProgram *program = ctx->program;
  uint32_t inputs, outputs;
  if (!natural_signature_registers(program->inputs, program->input_count, &inputs) ||
      !natural_signature_registers(program->outputs, program->output_count, &outputs) ||
      !outputs) return false;
  for (int input = 0; input < program->input_count; ++input)
    if (!hlsl_high_level_input_name(ctx, (int)program->inputs[input].register_id)) return false;
  if (ctx->high_level_direct_return) return program->output_count == 1;
  if (!ctx->high_level_output_type[0] || !ctx->high_level_output_variable[0] ||
      !hlsl_high_level_struct_interface_supported(program, ctx->emit_mode)) return false;
  for (int output = 0; output < program->output_count; ++output)
    if (!hlsl_high_level_output_name(ctx, (int)program->outputs[output].register_id)) return false;
  return true;
}

bool hlsl_source_quality_interface_inventory_complete(const HLSLEmitterContext *ctx) {
  if (!hlsl_source_quality_interface_inventory_supported(ctx)) return false;
  uint32_t inputs, outputs;
  if (!natural_signature_registers(ctx->program->inputs, ctx->program->input_count, &inputs) ||
      !natural_signature_registers(ctx->program->outputs, ctx->program->output_count, &outputs))
    return false;
  if (ctx->high_level_input_parameters_emitted != inputs ||
      ctx->high_level_output_statements_emitted != outputs ||
      !ctx->high_level_entry_signature_emitted || !ctx->high_level_return_block_emitted)
    return false;
  if (ctx->high_level_direct_return)
    return !ctx->high_level_output_struct_emitted && !ctx->high_level_result_local_emitted &&
           !ctx->high_level_output_fields_emitted;
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
        usil_operand_destination_lane_mask(&owner->operands[0]) != element->mask) continue;
    ctx->high_level_output_statements_emitted |= UINT32_C(1) << element->register_id;
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

const char *hlsl_high_level_input_name(const HLSLEmitterContext *ctx, int register_index) {
  if (!hlsl_high_level_input_signature(ctx, register_index) ||
      !ctx->high_level_input_names[register_index][0]) return NULL;
  return ctx->high_level_input_names[register_index];
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
      strcmp(ctx->high_level_output_variable, name) == 0) return false;
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

static bool allocate_interface_name(HLSLEmitterContext *ctx, const char *base, char destination[96]) {
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
  for (int input = 0; input < ctx->program->input_count; ++input) {
    const DXBCSignatureElement *element = &ctx->program->inputs[input];
    if (element->component_type != 3 || !element->mask || element->mask > 15 ||
        (element->mask & (element->mask + 1u)) || element->min_precision ||
        element->register_id >= HLSL_SM5_IO_REGISTER_COUNT ||
        ctx->high_level_input_names[element->register_id][0]) goto unsupported;
    char base[80];
    const char *semantic = dxbc_signature_semantic_name(element);
    const char *role = semantic_input_role(semantic);
    if (!role) {
      if (!hlsl_format_checked(ctx, base, sizeof(base), "attribute%d", input)) return false;
    } else if (strcmp(semantic, "TEXCOORD") == 0 || element->semantic_index) {
      if (!hlsl_format_checked(ctx, base, sizeof(base), "%s%u", role,
                                element->semantic_index)) return false;
    } else if (!hlsl_copy_checked(ctx, base, sizeof(base), role)) {
      return false;
    }
    if (!allocate_interface_name(ctx, base, ctx->high_level_input_names[element->register_id])) goto unsupported;
  }
  if (!ctx->high_level_direct_return) {
    if (!allocate_interface_name(ctx, ctx->preferred_output_struct_name,
                                ctx->high_level_output_type) ||
        !allocate_interface_name(ctx, "output", ctx->high_level_output_variable)) goto unsupported;
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
      if (!allocate_interface_name(ctx, base, ctx->high_level_output_names[element->register_id])) goto unsupported;
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
  if (!ctx || !operand || !provenance || operand->type != OPERAND_TYPE_INPUT ||
      !demanded_lanes || (demanded_lanes & ~15u) ||
      !hlsl_lift_operand_is_plain(operand)) return false;
  const DXBCSignatureElement *element = hlsl_high_level_input_signature(ctx, operand->register_index);
  if (!element || !hlsl_high_level_input_name(ctx, operand->register_index)) return false;
  ast_operand_provenance_init(provenance);
  provenance->complete = true;
  provenance->value_role = AST_OPERAND_VALUE_LOGICAL;
  provenance->logical_value_id = (UINT64_C(1) << 63) | (uint32_t)operand->register_index;
  provenance->natural_components = (uint8_t)signature_width(element);
  for (int component = 0; component < 4; ++component) {
    if (!(demanded_lanes & (1u << component))) continue;
    int selected = usil_operand_source_component(operand, component);
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

void emit_io_structs(HLSLEmitterContext* ctx, const char* input_struct, const char* output_struct) {
  if (ctx->high_level_direct_return) return;
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  if (ctx->high_level_interface) {
    size_t struct_begin = sb->len;
    sb_appendf(sb, "struct %s {\n", ctx->high_level_output_type);
    for (int output = 0; output < program->output_count; ++output) {
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
      if (sb_ok(sb))
        ctx->high_level_output_fields_emitted |= UINT32_C(1) << element->register_id;
    }
    sb_append(sb, "};\n\n");
    hlsl_source_quality_emission(ctx, 0, false, -1);
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
      const char *name = hlsl_high_level_input_name(ctx, (int)element->register_id);
      if (!name) { sb->failed = true; return; }
      if (input) sb_append(sb, ", ");
      sb_appendf(sb, "%s%s %s : %s", interpolation_modifier(element->interpolation_mode),
                 get_type_str(element->component_type, (int)signature_width(element)),
                 name, input_semantic);
      if (element->semantic_index || strcmp(input_semantic, "TEXCOORD") == 0)
        sb_appendf(sb, "%u", element->semantic_index);
      hlsl_source_quality_emission(ctx, 0, false, -1);
      if (sb_ok(sb))
        ctx->high_level_input_parameters_emitted |= UINT32_C(1) << element->register_id;
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
