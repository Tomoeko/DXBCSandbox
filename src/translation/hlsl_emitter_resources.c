// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool return_types_are_zero(const uint8_t return_types[4]) {
  return return_types[0] == 0 && return_types[1] == 0 &&
         return_types[2] == 0 && return_types[3] == 0;
}

static bool format_typed_resource_element_type(
    const uint8_t return_types[4], char *output, size_t output_size) {
  if (!return_types || !output || output_size == 0) return false;
  int components = 0;
  while (components < 4 && return_types[components] != 9) ++components;
  if (components == 0) return false;
  for (int component = 0; component < components; ++component) {
    if (return_types[component] != return_types[0]) return false;
  }
  for (int component = components; component < 4; ++component) {
    if (return_types[component] != 9) return false;
  }
  const char *prefix = "";
  const char *base = NULL;
  switch (return_types[0]) {
    case 1: prefix = "unorm "; base = "float"; break;
    case 2: prefix = "snorm "; base = "float"; break;
    case 3: base = "int"; break;
    case 4: base = "uint"; break;
    case 5: base = "float"; break;
    default: return false;
  }
  int written = components == 1
                    ? snprintf(output, output_size, "%s%s", prefix, base)
                    : snprintf(output, output_size, "%s%s%d", prefix, base,
                               components);
  return written > 0 && (size_t)written < output_size;
}

static bool is_multisampled_dimension(const char *dimension) {
  return strcmp(dimension, "2dms") == 0 ||
         strcmp(dimension, "2dmsarray") == 0;
}

static SerializedResourceType srv_kind_for_texture(
    const USILTexture *texture) {
  return texture &&
                 (strcmp(texture->dimension, "structured") == 0 ||
                  strcmp(texture->dimension, "raw") == 0)
             ? SERIALIZED_RESOURCE_BUFFER
             : SERIALIZED_RESOURCE_TEXTURE;
}

bool hlsl_texture_declaration_supported(const USILTexture* texture) {
  if (!texture || !memchr(texture->dimension, '\0',
                          sizeof(texture->dimension))) return false;
  if (strcmp(texture->dimension, "structured") == 0) {
    return texture->sample_count == 0 && return_types_are_zero(
               texture->return_types) && texture->stride > 0 &&
           texture->stride <= HLSL_D3D11_MAX_STRUCTURED_STRIDE &&
           (texture->stride & 3) == 0;
  }
  if (strcmp(texture->dimension, "raw") == 0) {
    return texture->sample_count == 0 && texture->stride == 0 &&
           return_types_are_zero(texture->return_types);
  }
  static const char *const typed_dimensions[] = {
      "buffer", "1d", "1darray", "2d", "2darray", "2dms",
      "2dmsarray", "3d", "cube", "cubearray"};
  bool known = false;
  for (size_t index = 0;
       index < sizeof(typed_dimensions) / sizeof(typed_dimensions[0]);
       ++index) {
    if (strcmp(texture->dimension, typed_dimensions[index]) == 0) {
      known = true;
      break;
    }
  }
  char element_type[32];
  if (!known || texture->stride != 0 ||
      !format_typed_resource_element_type(texture->return_types,
                                          element_type,
                                          sizeof(element_type))) return false;
  if (is_multisampled_dimension(texture->dimension)) {
    /* Zero is the legal unspecialized Texture2DMS form; an explicit sample
     * template argument is present only when the declaration encodes one. */
    return texture->sample_count <=
           HLSL_D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT;
  }
  return texture->sample_count == 0;
}

bool hlsl_uav_declaration_supported(const USILUav* uav) {
  if (!uav || !memchr(uav->dimension, '\0', sizeof(uav->dimension)) ||
      uav->sample_count != 0 || uav->has_order_preserving_counter) return false;
  if (strcmp(uav->dimension, "structured") == 0) {
    return return_types_are_zero(uav->return_types) && uav->stride > 0 &&
           uav->stride <= HLSL_D3D11_MAX_STRUCTURED_STRIDE &&
           (uav->stride & 3) == 0;
  }
  if (strcmp(uav->dimension, "raw") == 0) {
    return uav->stride == 0 && return_types_are_zero(uav->return_types);
  }
  char element_type[32];
  if (uav->stride != 0 ||
      !format_typed_resource_element_type(uav->return_types, element_type,
                                          sizeof(element_type))) return false;
  return strcmp(uav->dimension, "buffer") == 0 ||
         strcmp(uav->dimension, "1d") == 0 ||
         strcmp(uav->dimension, "1darray") == 0 ||
         strcmp(uav->dimension, "2d") == 0 ||
         strcmp(uav->dimension, "2darray") == 0 ||
         strcmp(uav->dimension, "3d") == 0;
}

bool hlsl_structured_atomic_address(const USILProgram* program,
                                    const USILInstruction* instruction,
                                    int* uav_register, int* byte_offset) {
  if (uav_register) *uav_register = -1;
  if (byte_offset) *byte_offset = -1;
  if (!program || !instruction || !uav_register || !byte_offset ||
      instruction->opcode != USIL_OP_IMM_ATOMIC_IADD ||
      instruction->operand_count != 4 ||
      instruction->operands[1].type != OPERAND_TYPE_UAV ||
      instruction->operands[1].rel_op0 ||
      instruction->operands[1].rel_op1 ||
      instruction->operands[1].rel_op2) return false;

  const int reg = instruction->operands[1].register_index;
  const USILUav *uav = NULL;
  for (int index = 0; index < program->uav_count; ++index) {
    if (program->uavs[index].reg_idx != reg) continue;
    if (uav) return false;
    uav = &program->uavs[index];
  }
  if (!uav || strcmp(uav->dimension, "structured") != 0 ||
      !hlsl_uav_declaration_supported(uav)) return false;

  /* A structured UAV's atomic address is (element index, byte offset).
   * HLSL needs the byte offset as a static member name; a dynamic or
   * modified value has no exact typed-struct spelling and must fail closed. */
  const DXBCOperand *address = &instruction->operands[2];
  if (address->type != OPERAND_TYPE_IMMEDIATE32 ||
      address->imm_value_count != 4 || address->has_abs || address->has_neg ||
      address->rel_op0 || address->rel_op1 || address->rel_op2 ||
      address->imm_values[1] > (uint32_t)INT_MAX) return false;
  const int offset = (int)address->imm_values[1];
  if (offset < 0 || (offset & 3) != 0 || offset > uav->stride - 4)
    return false;
  *uav_register = reg;
  *byte_offset = offset;
  return true;
}

static bool append_structured_type(StringBuilder *sb, const char *type_name,
                                   int stride, const USILProgram *program,
                                   int uav_register) {
  if (!sb || !type_name || stride <= 0 || (stride & 3) != 0 ||
      (uav_register >= 0 && !program)) return false;
  sb_appendf(sb, "struct %s {\n", type_name);
  for (int offset = 0; offset < stride; offset += 4) {
    bool atomic_field = false;
    if (uav_register >= 0) {
      for (int instruction = 0; instruction < program->instruction_count;
           ++instruction) {
        const USILInstruction *candidate =
            &program->instructions[instruction];
        if (candidate->opcode != USIL_OP_IMM_ATOMIC_IADD ||
            candidate->operand_count < 2 ||
            candidate->operands[1].type != OPERAND_TYPE_UAV ||
            candidate->operands[1].register_index != uav_register)
          continue;
        int target_register = -1;
        int target_offset = -1;
        if (!hlsl_structured_atomic_address(program, candidate,
                                            &target_register,
                                            &target_offset))
          return false;
        if (target_register == uav_register && target_offset == offset)
          atomic_field = true;
      }
    }
    sb_appendf(sb, "    %s m%d;\n", atomic_field ? "uint" : "float",
               offset);
  }
  sb_append(sb, "};\n");
  return !sb->failed;
}

static bool format_texture_type(const USILTexture *texture, char *output,
                                size_t output_size) {
  if (!hlsl_texture_declaration_supported(texture) || !output ||
      output_size == 0) return false;
  if (strcmp(texture->dimension, "raw") == 0) {
    int written = snprintf(output, output_size, "ByteAddressBuffer");
    return written > 0 && (size_t)written < output_size;
  }
  if (strcmp(texture->dimension, "structured") == 0) return false;
  const char *object = NULL;
  if (strcmp(texture->dimension, "buffer") == 0) object = "Buffer";
  else if (strcmp(texture->dimension, "1d") == 0) object = "Texture1D";
  else if (strcmp(texture->dimension, "1darray") == 0)
    object = "Texture1DArray";
  else if (strcmp(texture->dimension, "2d") == 0) object = "Texture2D";
  else if (strcmp(texture->dimension, "2darray") == 0)
    object = "Texture2DArray";
  else if (strcmp(texture->dimension, "2dms") == 0)
    object = "Texture2DMS";
  else if (strcmp(texture->dimension, "2dmsarray") == 0)
    object = "Texture2DMSArray";
  else if (strcmp(texture->dimension, "3d") == 0) object = "Texture3D";
  else if (strcmp(texture->dimension, "cube") == 0)
    object = "TextureCube";
  else if (strcmp(texture->dimension, "cubearray") == 0)
    object = "TextureCubeArray";
  if (!object) return false;
  char element[32];
  if (!format_typed_resource_element_type(texture->return_types, element,
                                          sizeof(element))) return false;
  int written = is_multisampled_dimension(texture->dimension) &&
                        texture->sample_count != 0
                    ? snprintf(output, output_size, "%s<%s, %u>", object,
                               element, texture->sample_count)
                    : snprintf(output, output_size, "%s<%s>", object,
                               element);
  return written > 0 && (size_t)written < output_size;
}

static bool format_uav_type(const USILUav *uav, char *output,
                            size_t output_size) {
  if (!hlsl_uav_declaration_supported(uav) || !output || output_size == 0)
    return false;
  if (strcmp(uav->dimension, "raw") == 0) {
    int written = snprintf(output, output_size, "%s",
                           uav->rasterizer_ordered
                               ? "RasterizerOrderedByteAddressBuffer"
                               : "RWByteAddressBuffer");
    return written > 0 && (size_t)written < output_size;
  }
  if (strcmp(uav->dimension, "structured") == 0) return false;
  const char *object = NULL;
  if (strcmp(uav->dimension, "buffer") == 0)
    object = uav->rasterizer_ordered ? "RasterizerOrderedBuffer"
                                     : "RWBuffer";
  else if (strcmp(uav->dimension, "1d") == 0)
    object = uav->rasterizer_ordered ? "RasterizerOrderedTexture1D"
                                     : "RWTexture1D";
  else if (strcmp(uav->dimension, "1darray") == 0)
    object = uav->rasterizer_ordered ? "RasterizerOrderedTexture1DArray"
                                     : "RWTexture1DArray";
  else if (strcmp(uav->dimension, "2d") == 0)
    object = uav->rasterizer_ordered ? "RasterizerOrderedTexture2D"
                                     : "RWTexture2D";
  else if (strcmp(uav->dimension, "2darray") == 0)
    object = uav->rasterizer_ordered ? "RasterizerOrderedTexture2DArray"
                                     : "RWTexture2DArray";
  else if (strcmp(uav->dimension, "3d") == 0)
    object = uav->rasterizer_ordered ? "RasterizerOrderedTexture3D"
                                     : "RWTexture3D";
  if (!object) return false;
  char element[32];
  if (!format_typed_resource_element_type(uav->return_types, element,
                                          sizeof(element))) return false;
  int written = snprintf(output, output_size, "%s<%s>", object,
                         element);
  return written > 0 && (size_t)written < output_size;
}

typedef struct UnityBuiltinTexture {
  const char *name;
  const char *dimension;
} UnityBuiltinTexture;

static const UnityBuiltinTexture g_unity_builtin_textures[] = {
    {"unity_Lightmap", "2d"},
    {"unity_LightmapInd", "2d"},
    {"unity_ShadowMask", "2d"},
    {"unity_DynamicLightmap", "2d"},
    {"unity_DynamicDirectionality", "2d"},
    {"unity_DynamicNormal", "2d"},
    {"unity_SpecCube0", "cube"},
    {"unity_SpecCube1", "cube"},
    {"unity_ProbeVolumeSH", "3d"},
};

static int unity_builtin_texture_contract(const char *name,
                                          const USILTexture *texture) {
  if (!name || !texture) return 0;
  for (size_t index = 0;
       index < sizeof(g_unity_builtin_textures) /
                   sizeof(g_unity_builtin_textures[0]);
       ++index) {
    const UnityBuiltinTexture *builtin = &g_unity_builtin_textures[index];
    if (strcmp(name, builtin->name) != 0) continue;
    if (strcmp(texture->dimension, builtin->dimension) != 0 ||
        texture->sample_count != 0 || texture->stride != 0) {
      return -1;
    }
    for (size_t component = 0; component < 4; ++component) {
      if (texture->return_types[component] != 5) return -1;
    }
    return 1;
  }
  return 0;
}

static bool unity_builtin_sampler_name(const char *name) {
  static const char *const names[] = {
      "samplerunity_Lightmap",
      "samplerunity_ShadowMask",
      "samplerunity_DynamicLightmap",
      "samplerunity_SpecCube0",
      "samplerunity_ProbeVolumeSH",
  };
  if (!name) return false;
  for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
    if (strcmp(name, names[index]) == 0) return true;
  }
  return false;
}

bool get_structured_resource_layout(const USILProgram* program, int reg,
                                    StructuredResourceLayout* layout) {
  if (!program || !layout) return false;
  memset(layout, 0, sizeof(*layout));
  for (int i = 0; i < program->texture_count; i++) {
    if (program->textures[i].reg_idx == reg &&
        strcmp(program->textures[i].dimension, "structured") == 0) {
      layout->stride = program->textures[i].stride;
      break;
    }
  }
  if (layout->stride <= 0 ||
      layout->stride > HLSL_D3D11_MAX_STRUCTURED_STRIDE ||
      (layout->stride & 3) != 0) return false;

  for (int i = 0; i < program->instruction_count; i++) {
    const USILInstruction* inst = &program->instructions[i];
    if (inst->opcode != USIL_OP_LD_STRUCTURED || inst->operand_count < 4 ||
        inst->operands[3].type != OPERAND_TYPE_RESOURCE ||
        inst->operands[3].register_index != reg ||
        inst->operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
        inst->operands[2].imm_value_count != 1) {
      continue;
    }
    if (inst->operands[2].imm_values[0] > INT_MAX) {
      free_structured_resource_layout(layout);
      return false;
    }
    int offset = (int)inst->operands[2].imm_values[0];
    int width = get_mask_component_count(inst->operands[0].destination_mask);
    if (width == 0) width = 4;
    if (offset < 0 || (offset & 3) != 0 || width < 1 || width > 4 ||
        offset > layout->stride - width * 4) {
      free_structured_resource_layout(layout);
      return false;
    }
    int insert = 0;
    while (insert < layout->field_count &&
           layout->offsets[insert] < offset) insert++;
    if (insert < layout->field_count && layout->offsets[insert] == offset) {
      if (width > layout->widths[insert]) layout->widths[insert] = width;
      continue;
    }
    if (layout->field_count == INT_MAX) {
      free_structured_resource_layout(layout);
      return false;
    }
    if (layout->field_count == layout->field_alloc) {
      int new_allocation = layout->field_alloc == 0 ? 16
                                                     : layout->field_alloc;
      if (new_allocation > INT_MAX / 2) new_allocation = INT_MAX;
      else new_allocation *= 2;
      if (new_allocation <= layout->field_count ||
          dxbc_size_multiply_overflows((size_t)new_allocation,
                                       sizeof(*layout->offsets)) ||
          dxbc_size_multiply_overflows((size_t)new_allocation,
                                       sizeof(*layout->widths))) {
        free_structured_resource_layout(layout);
        return false;
      }
      int *new_offsets = malloc((size_t)new_allocation *
                                sizeof(*layout->offsets));
      int *new_widths = malloc((size_t)new_allocation *
                               sizeof(*layout->widths));
      if (!new_offsets || !new_widths) {
        free(new_offsets);
        free(new_widths);
        free_structured_resource_layout(layout);
        return false;
      }
      if (layout->field_count > 0) {
        memcpy(new_offsets, layout->offsets,
               (size_t)layout->field_count * sizeof(*layout->offsets));
        memcpy(new_widths, layout->widths,
               (size_t)layout->field_count * sizeof(*layout->widths));
      }
      free(layout->offsets);
      free(layout->widths);
      layout->offsets = new_offsets;
      layout->widths = new_widths;
      layout->field_alloc = new_allocation;
    }
    memmove(&layout->offsets[insert + 1], &layout->offsets[insert],
            (size_t)(layout->field_count - insert) * sizeof(layout->offsets[0]));
    memmove(&layout->widths[insert + 1], &layout->widths[insert],
            (size_t)(layout->field_count - insert) * sizeof(layout->widths[0]));
    layout->offsets[insert] = offset;
    layout->widths[insert] = width;
    layout->field_count++;
  }
  if (layout->field_count == 0) {
    free_structured_resource_layout(layout);
    return false;
  }
  int end = 0;
  for (int i = 0; i < layout->field_count; i++) {
    if (layout->offsets[i] < end) {
      free_structured_resource_layout(layout);
      return false;
    }
    end = layout->offsets[i] + layout->widths[i] * 4;
  }
  if (end > layout->stride) {
    free_structured_resource_layout(layout);
    return false;
  }
  return true;
}

void free_structured_resource_layout(StructuredResourceLayout* layout) {
  if (!layout) return;
  free(layout->offsets);
  free(layout->widths);
  memset(layout, 0, sizeof(*layout));
}

void emit_resources(HLSLEmitterContext* ctx) {
  const USILProgram* program = ctx->program;
  StringBuilder* sb = ctx->sb;
  const SerializedProgramParameters* params = ctx->params;

  // 3. Emit Textures & Samplers
  for (int i = 0; i < program->texture_count; i++) {
    int reg = program->textures[i].reg_idx;
    const SerializedResourceType srv_kind =
        srv_kind_for_texture(&program->textures[i]);
    const char *name = NULL;
    if (!resolve_srv_name_ctx(ctx, reg, srv_kind, &name)) {
      hlsl_emit_fail_metadata(
          ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
          HLSL_EMIT_PHASE_RESOURCE_EMISSION,
          HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
          i, -1, reg);
      return;
    }
    if (ctx->omit_unity_builtin_declarations) {
      const int builtin =
          unity_builtin_texture_contract(name, &program->textures[i]);
      if (builtin < 0) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_BUILTIN_CONTRACT_MISMATCH,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
            i, -1, reg);
        return;
      }
      if (builtin > 0) {
        for (int previous = 0; previous < i; ++previous) {
          const int previous_reg = program->textures[previous].reg_idx;
          const SerializedResourceType previous_kind =
              srv_kind_for_texture(&program->textures[previous]);
          const char *previous_name = NULL;
          if (!resolve_srv_name_ctx(ctx, previous_reg, previous_kind,
                                    &previous_name)) {
            hlsl_emit_fail_metadata(
                ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                HLSL_EMIT_PHASE_RESOURCE_EMISSION,
                HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
                HLSL_EMIT_METADATA_SOURCE_PROGRAM,
                HLSL_EMIT_METADATA_TEXTURE, previous, -1, previous_reg);
            return;
          }
          if (previous_name && strcmp(previous_name, name) == 0) {
            hlsl_emit_fail_metadata(
                ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
                HLSL_EMIT_PHASE_RESOURCE_EMISSION,
                HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
                HLSL_EMIT_METADATA_SOURCE_PROGRAM,
                HLSL_EMIT_METADATA_TEXTURE, i, -1, reg);
            return;
          }
        }
        continue;
      }
    }
    char tex_type[96];
    if (strcmp(program->textures[i].dimension, "structured") == 0) {
      char type_name[64];
      if (!hlsl_format_checked(ctx, type_name, sizeof(type_name),
                               "dxbc_struct_t%d", reg)) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
            i, -1, reg);
        return;
      }
      if (!append_structured_type(sb, type_name,
                                  program->textures[i].stride, NULL, -1)) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
            i, -1, reg);
        return;
      }
      if (!hlsl_format_checked(ctx, tex_type, sizeof(tex_type),
                               "StructuredBuffer<%s>", type_name)) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
            i, -1, reg);
        return;
      }
    } else if (!format_texture_type(&program->textures[i], tex_type,
                                    sizeof(tex_type))) {
      hlsl_emit_fail_metadata(
          ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
          HLSL_EMIT_PHASE_RESOURCE_EMISSION,
          HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_TEXTURE,
          i, -1, reg);
      return;
    }

    if (name) {
      sb_appendf(sb, "%s %s : register(t%d);\n", tex_type, name, reg);
    } else {
      sb_appendf(sb, "%s t%d : register(t%d);\n", tex_type, reg, reg);
    }
  }
  const char *emitted_sampler_names[HLSL_SM5_SAMPLER_REGISTER_COUNT] = {0};
  int emitted_sampler_count = 0;
  for (int i = 0; i < program->sampler_count; i++) {
    int reg = program->samplers[i].reg_idx;
    const char *name =
        (reg >= 0 && reg < HLSL_SM5_SAMPLER_REGISTER_COUNT &&
         ctx->sampler_names[reg] && ctx->sampler_names[reg][0])
        ? ctx->sampler_names[reg]
        : NULL;
    if (!name) {
      hlsl_emit_fail_metadata(
          ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
          HLSL_EMIT_PHASE_RESOURCE_EMISSION,
          HLSL_EMIT_REASON_MISSING_METADATA_AUTHORITY,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
          i, -1, reg);
      return;
    }
    if (ctx->omit_unity_builtin_declarations &&
        unity_builtin_sampler_name(name)) {
      if (program->samplers[i].mode != 0u) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_BUILTIN_CONTRACT_MISMATCH,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
            i, -1, reg);
        return;
      }
      bool duplicate_builtin = false;
      for (int previous = 0; previous < i; ++previous) {
        const int previous_reg = program->samplers[previous].reg_idx;
        const char *previous_name =
            previous_reg >= 0 &&
                    previous_reg < HLSL_SM5_SAMPLER_REGISTER_COUNT
                ? ctx->sampler_names[previous_reg]
                : NULL;
        if (previous_name && strcmp(previous_name, name) == 0) {
          duplicate_builtin = true;
          break;
        }
      }
      if (duplicate_builtin) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
            i, -1, reg);
        return;
      }
      continue;
    }
    bool dup = false;
    for (int j = 0; j < emitted_sampler_count; j++) {
      if (strcmp(emitted_sampler_names[j], name) == 0) {
        dup = true;
        break;
      }
    }
    /* Two distinct binding registers cannot share one HLSL identifier.  The
     * previous de-duplication silently kept the first declaration, causing
     * every later s# use to bind the wrong sampler. */
    if (dup || emitted_sampler_count >= HLSL_SM5_SAMPLER_REGISTER_COUNT) {
      hlsl_emit_fail_metadata(
          ctx, HLSL_EMIT_STATUS_INVALID_METADATA,
          HLSL_EMIT_PHASE_RESOURCE_EMISSION,
          HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_SAMPLER,
          i, -1, reg);
      return;
    }
    emitted_sampler_names[emitted_sampler_count++] = name;
    const char *sampler_type = program->samplers[i].mode == 1u
                                   ? "SamplerComparisonState"
                                   : "SamplerState";
    sb_appendf(sb, "%s %s : register(s%d);\n", sampler_type, name,
               reg);
  }
  for (int i = 0; i < program->uav_count; i++) {
    int reg = program->uavs[i].reg_idx;
    const char *name = resolve_uav_name(params, reg);
    char uav_type[96];
    if (strcmp(program->uavs[i].dimension, "structured") == 0) {
      char type_name[64];
      if (!hlsl_format_checked(ctx, type_name, sizeof(type_name),
                               "dxbc_struct_u%d", reg)) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_UAV,
            i, -1, reg);
        return;
      }
      if (!append_structured_type(sb, type_name, program->uavs[i].stride,
                                  program, reg)) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_UAV,
            i, -1, reg);
        return;
      }
      if (!hlsl_format_checked(
              ctx, uav_type, sizeof(uav_type), "%s<%s>",
              program->uavs[i].rasterizer_ordered
                  ? "RasterizerOrderedStructuredBuffer"
                  : "RWStructuredBuffer",
              type_name)) {
        hlsl_emit_fail_metadata(
            ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
            HLSL_EMIT_PHASE_RESOURCE_EMISSION,
            HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW,
            HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_UAV,
            i, -1, reg);
        return;
      }
    } else if (!format_uav_type(&program->uavs[i], uav_type,
                                sizeof(uav_type))) {
      hlsl_emit_fail_metadata(
          ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
          HLSL_EMIT_PHASE_RESOURCE_EMISSION,
          HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT,
          HLSL_EMIT_METADATA_SOURCE_PROGRAM, HLSL_EMIT_METADATA_UAV,
          i, -1, reg);
      return;
    }

    if (name) {
      sb_appendf(sb, "%s%s %s : register(u%d);\n",
                 program->uavs[i].globally_coherent ? "globallycoherent " : "",
                 uav_type, name, reg);
    } else {
      sb_appendf(sb, "%s%s u%d : register(u%d);\n",
                 program->uavs[i].globally_coherent ? "globallycoherent " : "",
                 uav_type, reg, reg);
    }
  }
  if (program->texture_count > 0 || program->sampler_count > 0 ||
      program->uav_count > 0) {
    sb_append(sb, "\n");
  }

  if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE &&
      ctx->readable_screen_pos_mul_xzw_idx != -1) {
    sb_appendf(sb, "float4 %s(float4 pos, float proj_param) {\n",
               ctx->readable_screen_pos_helper);
    sb_append(sb, "    float4 o = pos * 0.5f;\n");
    sb_append(sb, "    o.xy = float2(o.x, o.y * proj_param) + o.w;\n");
    sb_append(sb, "    o.zw = pos.zw;\n");
    sb_append(sb, "    return o;\n");
    sb_append(sb, "}\n\n");
  }
}

