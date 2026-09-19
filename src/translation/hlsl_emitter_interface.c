// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
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
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;

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
        }
      } else {
        if (ctx->use_uint_temps) {
          sb_appendf(sb, "    uint4 r%d = (uint4)0;\n", i);
        } else {
          sb_appendf(sb, "    float4 r%d = (float4)0;\n", i);
        }
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
  }

  // Component-split helpers hold evaluated HLSL arithmetic, not raw DXBC
  // register bits. Integer register storage is converted before reaching them.
  sb_append(sb, "    float u_xlat_temp_x = 0.0f;\n");
  sb_append(sb, "    float u_xlat_temp_y = 0.0f;\n");
  sb_append(sb, "    float u_xlat_temp_z = 0.0f;\n");
  sb_append(sb, "    float u_xlat_temp_w = 0.0f;\n");

  sb_append(sb, "\n");
}
