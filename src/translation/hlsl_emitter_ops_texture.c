// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>

static void format_texture_coordinate(HLSLEmitterContext* ctx,
                                      const DXBCOperand* operand,
                                      int component_count, bool is_uint,
                                      char* output, size_t output_size) {
    if (component_count < 1) component_count = 1;
    if (component_count > 4) component_count = 4;
    const int component_mask = ((1 << component_count) - 1) << 4;
    /* Formatting from the decoded operand and required resource dimension
     * preserves vector casts such as asfloat(r.xy), while select-mode operands
     * remain scalar and follow D3DCompiler's implicit conversion path. */
    format_operand_hlsl(ctx, operand, false, is_uint, component_mask, false,
                        output, output_size);
}

static void format_signed_texture_coordinate(HLSLEmitterContext* ctx,
                                             const DXBCOperand* operand,
                                             int component_count,
                                             char* output,
                                             size_t output_size) {
    if (component_count < 1 || component_count > 4) {
        if (ctx && ctx->sb) ctx->sb->failed = true;
        if (output && output_size) output[0] = '\0';
        return;
    }
    const int component_mask = ((1 << component_count) - 1) << 4;
    format_operand_hlsl(ctx, operand, true, false, component_mask, false,
                        output, output_size);
}

static const USILTexture *find_texture(const USILProgram *program,
                                       int register_index) {
    const USILTexture *result = NULL;
    if (!program) return NULL;
    for (int texture = 0; texture < program->texture_count; ++texture) {
        if (program->textures[texture].reg_idx != register_index) continue;
        /* Duplicate declarations leave both the object type and the return
         * width ambiguous.  Exact emission must not choose one by order. */
        if (result) return NULL;
        result = &program->textures[texture];
    }
    return result;
}

static const USILUav *find_uav(const USILProgram *program,
                               int register_index) {
    const USILUav *result = NULL;
    if (!program) return NULL;
    for (int uav = 0; uav < program->uav_count; ++uav) {
        if (program->uavs[uav].reg_idx != register_index) continue;
        if (result) return NULL;
        result = &program->uavs[uav];
    }
    return result;
}

static const char *find_texture_dimension(const USILProgram *program,
                                          int register_index) {
    const USILTexture *texture = find_texture(program, register_index);
    return texture ? texture->dimension : NULL;
}

static const USILTexture *find_instruction_texture(
    const USILProgram *program, const USILInstruction *instruction,
    int resource_operand_index) {
    if (!program || !instruction || resource_operand_index < 0 ||
        resource_operand_index >= instruction->operand_count)
        return NULL;
    const DXBCOperand *resource =
        &instruction->operands[resource_operand_index];
    if (resource->type != OPERAND_TYPE_RESOURCE) return NULL;
    const USILTexture *texture =
        find_texture(program, resource->register_index);
    if (!texture ||
        (instruction->resource_dimension[0] &&
         strcmp(instruction->resource_dimension, texture->dimension) != 0) ||
        (instruction->has_resource_return_types &&
         memcmp(instruction->resource_return_types, texture->return_types,
                sizeof(texture->return_types)) != 0))
        return NULL;
    return texture;
}

static const char *find_resource_dimension(const USILProgram *program,
                                           const DXBCOperand *operand) {
    if (!program || !operand) return NULL;
    if (operand->type == OPERAND_TYPE_RESOURCE)
        return find_texture_dimension(program, operand->register_index);
    if (operand->type == OPERAND_TYPE_UAV) {
        const USILUav *uav = find_uav(program, operand->register_index);
        return uav ? uav->dimension : NULL;
    }
    return NULL;
}

static int typed_resource_result_component_count(
    const USILTexture *texture) {
    if (!texture) return 0;
    int components = 0;
    while (components < 4 && texture->return_types[components] != 9u)
        ++components;
    if (components == 0) return 0;
    const uint8_t type = texture->return_types[0];
    if (type < 1u || type > 5u) return 0;
    for (int component = 0; component < components; ++component) {
        if (texture->return_types[component] != type) return 0;
    }
    for (int component = components; component < 4; ++component) {
        if (texture->return_types[component] != 9u) return 0;
    }
    return components;
}

static bool resource_result_source_component(const DXBCOperand *operand,
                                             int destination_component,
                                             int *source_component) {
    if (!operand || !source_component || destination_component < 0 ||
        destination_component > 3) return false;
    if (operand->swizzle_mode == 0) {
        *source_component = destination_component;
    } else if (operand->swizzle_mode == 1) {
        *source_component = operand->swizzle[destination_component];
    } else if (operand->swizzle_mode == 2) {
        *source_component = operand->swizzle[0];
    } else {
        return false;
    }
    return *source_component >= 0 && *source_component < 4;
}

/* A DXBC resource operand selects result lanes by the destination lane
 * positions, whereas an HLSL method returns a densely packed value.  Project
 * exactly the lanes written by the instruction so that partial writes never
 * rely on an implicit vector truncation. */
static bool format_resource_result_swizzle(const DXBCOperand *resource,
                                           int destination_mask,
                                           int result_components,
                                           bool scalar_replicated,
                                           char *output,
                                           size_t output_size) {
    if (!resource || !output || output_size == 0 ||
        result_components < 1 || result_components > 4)
        return false;
    output[0] = '\0';
    int mask = destination_mask != 0 ? destination_mask : 0xf0;
    if ((mask & ~0xf0) != 0 || get_mask_component_count(mask) == 0)
        return false;

    char components[5];
    int count = 0;
    for (int destination_component = 0; destination_component < 4;
         ++destination_component) {
        if (!(mask & (16 << destination_component))) continue;
        int source_component = 0;
        if (!resource_result_source_component(resource, destination_component,
                                              &source_component))
            return false;
        if (!scalar_replicated && source_component >= result_components)
            return false;
        components[count++] = "xyzw"[source_component];
    }
    components[count] = '\0';

    /* Comparison sampling is a scalar value replicated to every DXBC result
     * lane.  Applying the resource's .xxxx spelling to the scalar HLSL
     * intrinsic creates a vector and changes the assignment width. */
    if (scalar_replicated || result_components == 1) return true;

    bool full_identity = count == result_components;
    for (int component = 0; full_identity && component < count; ++component)
        full_identity = components[component] == "xyzw"[component];
    if (full_identity) return true;

    const int written = snprintf(output, output_size, ".%s", components);
    return written > 0 && (size_t)written < output_size;
}

static int sample_coordinate_count(const char *dimension) {
    if (!dimension) return 0;
    if (strcmp(dimension, "1d") == 0) return 1;
    if (strcmp(dimension, "1darray") == 0 ||
        strcmp(dimension, "2d") == 0) return 2;
    if (strcmp(dimension, "2darray") == 0 ||
        strcmp(dimension, "3d") == 0 ||
        strcmp(dimension, "cube") == 0) return 3;
    if (strcmp(dimension, "cubearray") == 0) return 4;
    return 0;
}

static int sample_gradient_count(const char *dimension) {
    if (!dimension) return 0;
    if (strcmp(dimension, "1d") == 0 ||
        strcmp(dimension, "1darray") == 0) return 1;
    if (strcmp(dimension, "2d") == 0 ||
        strcmp(dimension, "2darray") == 0) return 2;
    if (strcmp(dimension, "3d") == 0 ||
        strcmp(dimension, "cube") == 0 ||
        strcmp(dimension, "cubearray") == 0) return 3;
    return 0;
}

static int multisample_coordinate_count(const char *dimension) {
    if (!dimension) return 0;
    if (strcmp(dimension, "2dms") == 0) return 2;
    if (strcmp(dimension, "2dmsarray") == 0) return 3;
    return 0;
}

static int load_coordinate_count(const char *dimension) {
    if (!dimension) return 0;
    if (strcmp(dimension, "buffer") == 0) return 1;
    if (strcmp(dimension, "1d") == 0) return 2;
    if (strcmp(dimension, "1darray") == 0 ||
        strcmp(dimension, "2d") == 0) return 3;
    if (strcmp(dimension, "2darray") == 0 ||
        strcmp(dimension, "3d") == 0) return 4;
    return 0;
}

static int texel_offset_component_count(const char *dimension) {
    if (!dimension) return 0;
    if (strcmp(dimension, "1d") == 0 ||
        strcmp(dimension, "1darray") == 0) return 1;
    if (strcmp(dimension, "2d") == 0 ||
        strcmp(dimension, "2darray") == 0 ||
        strcmp(dimension, "2dms") == 0 ||
        strcmp(dimension, "2dmsarray") == 0) return 2;
    if (strcmp(dimension, "3d") == 0 ||
        strcmp(dimension, "cube") == 0 ||
        strcmp(dimension, "cubearray") == 0) return 3;
    return 0;
}

static bool format_texel_offset(HLSLEmitterContext *ctx,
                                const USILInstruction *instruction,
                                const char *dimension, char *output,
                                size_t output_size) {
    if (!instruction || !output || output_size == 0) return false;
    output[0] = '\0';
    if (!instruction->has_texel_offset) return true;
    const int components = texel_offset_component_count(dimension);
    if (components == 1) {
        return hlsl_format_checked(ctx, output, output_size, ", %d",
                                   instruction->texel_offsets[0]);
    } else if (components == 2) {
        return hlsl_format_checked(ctx, output, output_size,
                                   ", int2(%d, %d)",
                                   instruction->texel_offsets[0],
                                   instruction->texel_offsets[1]);
    } else if (components == 3) {
        return hlsl_format_checked(ctx, output, output_size,
                                   ", int3(%d, %d, %d)",
                                   instruction->texel_offsets[0],
                                   instruction->texel_offsets[1],
                                   instruction->texel_offsets[2]);
    }
    return false;
}

void emit_texture_op(HLSLEmitterContext* ctx, const USILInstruction* inst,
                             bool isInt, bool isUint,
                             int comp, int dm, int format_mask, int current_dest_mask,
                             const char* dest,
                             const char* src0, const char* src1, const char* src2,
                             const char* src3, const char* src4,
                             char* line_buf, size_t line_buf_sz,
                             char* rhs_expr, size_t rhs_len, bool* is_custom, bool* wrap_swizzle) {
    const USILProgram* program = ctx->program;
    StringBuilder* sb = ctx->sb;
    (void)comp;
    (void)isInt;
    (void)format_mask;
    (void)current_dest_mask;
    (void)rhs_expr;
    (void)rhs_len;
    (void)wrap_swizzle;
    (void)src3;
    (void)src4;

    switch (inst->opcode) {
      case USIL_OP_SAMPLE:
      case USIL_OP_SAMPLE_C:
      case USIL_OP_SAMPLE_C_LZ:
      case USIL_OP_SAMPLE_L:
      case USIL_OP_SAMPLE_D:
      case USIL_OP_SAMPLE_B:
      {
        if (inst->operand_count < 4 ||
            inst->operands[2].type != OPERAND_TYPE_RESOURCE) {
          sb->failed = true;
          return;
        }
        const USILTexture *texture =
            find_instruction_texture(program, inst, 2);
        const char *dimension = texture ? texture->dimension : NULL;
        int uv_comps = sample_coordinate_count(dimension);
        int result_components =
            typed_resource_result_component_count(texture);
        const bool comparison_sample =
            inst->opcode == USIL_OP_SAMPLE_C ||
            inst->opcode == USIL_OP_SAMPLE_C_LZ;
        char texel_offset[64];
        if (uv_comps == 0 || result_components == 0 ||
            !format_texel_offset(ctx, inst, dimension, texel_offset,
                                 sizeof(texel_offset))) {
          sb->failed = true;
          return;
        }

        char uv[256];
        format_texture_coordinate(ctx, &inst->operands[1], uv_comps, isUint,
                                  uv, sizeof(uv));

        char res_swiz[32] = "";
        if (!format_resource_result_swizzle(
                &inst->operands[2], dm,
                comparison_sample ? 1 : result_components,
                comparison_sample, res_swiz, sizeof(res_swiz))) {
          sb->failed = true;
          return;
        }

        char scalar_argument[256] = "";
        char gradient_x[256] = "";
        char gradient_y[256] = "";
        if (comparison_sample || inst->opcode == USIL_OP_SAMPLE_L ||
            inst->opcode == USIL_OP_SAMPLE_B) {
          if (inst->operand_count < 5) {
            sb->failed = true;
            return;
          }
          format_operand_hlsl(ctx, &inst->operands[4], false, false, 16,
                              false, scalar_argument,
                              sizeof(scalar_argument));
        } else if (inst->opcode == USIL_OP_SAMPLE_D) {
          const int gradient_components = sample_gradient_count(dimension);
          if (inst->operand_count < 6 || gradient_components == 0) {
            sb->failed = true;
            return;
          }
          format_texture_coordinate(ctx, &inst->operands[4],
                                    gradient_components, false, gradient_x,
                                    sizeof(gradient_x));
          format_texture_coordinate(ctx, &inst->operands[5],
                                    gradient_components, false, gradient_y,
                                    sizeof(gradient_y));
        }
        if (sb->failed) return;

        if (inst->opcode == USIL_OP_SAMPLE)
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s.Sample(%s, %s%s)%s;", dest, src1, src2, uv, texel_offset, res_swiz);
        else if (inst->opcode == USIL_OP_SAMPLE_C)
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s.SampleCmp(%s, %s, %s%s)%s;", dest, src1, src2, uv, scalar_argument, texel_offset, res_swiz);
        else if (inst->opcode == USIL_OP_SAMPLE_C_LZ)
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s.SampleCmpLevelZero(%s, %s, %s%s)%s;", dest, src1, src2, uv, scalar_argument, texel_offset, res_swiz);
        else if (inst->opcode == USIL_OP_SAMPLE_L)
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s.SampleLevel(%s, %s, %s%s)%s;", dest, src1, src2, uv, scalar_argument, texel_offset, res_swiz);
        else if (inst->opcode == USIL_OP_SAMPLE_D)
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s.SampleGrad(%s, %s, %s, %s%s)%s;", dest, src1, src2, uv, gradient_x, gradient_y, texel_offset, res_swiz);
        else if (inst->opcode == USIL_OP_SAMPLE_B)
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s.SampleBias(%s, %s, %s%s)%s;", dest, src1, src2, uv, scalar_argument, texel_offset, res_swiz);
        break;
      }
      case USIL_OP_LD:
      {
        if (inst->operand_count < 3 ||
            inst->operands[2].type != OPERAND_TYPE_RESOURCE) {
          sb->failed = true;
          return;
        }
        const USILTexture *texture =
            find_instruction_texture(program, inst, 2);
        const char *dimension = texture ? texture->dimension : NULL;
        const int coordinate_components = load_coordinate_count(dimension);
        const int result_components =
            typed_resource_result_component_count(texture);
        char res_swiz[32] = "";
        if (coordinate_components == 0 || result_components == 0 ||
            !format_resource_result_swizzle(
                &inst->operands[2], dm, result_components, false, res_swiz,
                sizeof(res_swiz))) {
          sb->failed = true;
          return;
        }
        char coordinate[256];
        format_signed_texture_coordinate(ctx, &inst->operands[1],
                                         coordinate_components, coordinate,
                                         sizeof(coordinate));
        char texel_offset[64];
        if (sb->failed ||
            !format_texel_offset(ctx, inst, dimension, texel_offset,
                                 sizeof(texel_offset))) {
          sb->failed = true;
          return;
        }
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s.Load(%s%s)%s;", dest,
                 src1, coordinate, texel_offset, res_swiz);
        break;
      }
      case USIL_OP_LD_MS:
      {
        if (inst->operand_count < 4 ||
            inst->operands[2].type != OPERAND_TYPE_RESOURCE) {
          sb->failed = true;
          return;
        }
        const USILTexture *texture =
            find_instruction_texture(program, inst, 2);
        const char *dimension = texture ? texture->dimension : NULL;
        const int coordinate_components =
            multisample_coordinate_count(dimension);
        const int result_components =
            typed_resource_result_component_count(texture);
        char res_swiz[32] = "";
        if (coordinate_components == 0 || result_components == 0 ||
            !format_resource_result_swizzle(
                &inst->operands[2], dm, result_components, false, res_swiz,
                sizeof(res_swiz))) {
          sb->failed = true;
          return;
        }
        char coordinate[256];
        char sample_index[256];
        format_signed_texture_coordinate(ctx, &inst->operands[1],
                                         coordinate_components, coordinate,
                                         sizeof(coordinate));
        format_operand_hlsl(ctx, &inst->operands[3], true, false, 16, false,
                            sample_index, sizeof(sample_index));
        char texel_offset[64];
        if (sb->failed ||
            !format_texel_offset(ctx, inst, dimension, texel_offset,
                                 sizeof(texel_offset))) {
          sb->failed = true;
          return;
        }
        hlsl_format_checked(ctx, line_buf, line_buf_sz,
                 "%s = %s.Load(%s, %s%s)%s;", dest, src1, coordinate,
                 sample_index, texel_offset, res_swiz);
        break;
      }
      case USIL_OP_LD_STRUCTURED:
      {
        if (inst->operand_count < 4 ||
            inst->operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
            inst->operands[2].imm_value_count != 1) {
          break;
        }
        int offset = (int)inst->operands[2].imm_values[0];
        int width = get_mask_component_count(
            inst->operands[0].destination_mask);
        if (width == 0) width = 4;
        char accesses[4][256];
        for (int component = 0; component < width; component++) {
          int source_component = component;
          if (inst->operands[3].swizzle_mode == 1) {
            source_component = inst->operands[3].swizzle[component];
          } else if (inst->operands[3].swizzle_mode == 2) {
            source_component = inst->operands[3].swizzle[0];
          }
          hlsl_format_checked(ctx, accesses[component], sizeof(accesses[component]),
                   "%s[asuint(%s)].m%d", src2, src0,
                   offset + source_component * 4);
        }
        if (width == 1) {
          hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = %s;", dest, accesses[0]);
        } else {
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                   "%s = float%d(%s, %s%s%s%s%s);", dest, width,
                   accesses[0], accesses[1], width > 2 ? ", " : "",
                   width > 2 ? accesses[2] : "", width > 3 ? ", " : "",
                   width > 3 ? accesses[3] : "");
        }
        break;
      }
      case USIL_OP_RESINFO:
      {
        if (inst->operand_count < 3) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
              HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE,
              ctx->current_instruction_index, -1);
          return;
        }
        *is_custom = true;
        sb_append(sb, "{\n");
        const char *resource_dimension = NULL;
        const DXBCOperand *resource_operand = NULL;
        for (int i = 0; i < inst->operand_count; i++) {
          if (inst->operands[i].type == OPERAND_TYPE_RESOURCE ||
              inst->operands[i].type == OPERAND_TYPE_UAV) {
            if (resource_operand) {
              hlsl_emit_fail_instruction(
                  ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
                  HLSL_EMIT_REASON_RESOURCE_CONTRACT_MISMATCH,
                  ctx->current_instruction_index, i);
              return;
            }
            resource_operand = &inst->operands[i];
          }
        }
        if (!resource_operand || resource_operand != &inst->operands[2] ||
            resource_operand->rel_op0) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
              HLSL_EMIT_REASON_INVALID_OPERAND,
              ctx->current_instruction_index, 2);
          return;
        }
        const char *declared_dimension =
            find_resource_dimension(program, resource_operand);
        if (!declared_dimension ||
            (inst->resource_dimension[0] &&
             strcmp(inst->resource_dimension, declared_dimension) != 0)) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
              HLSL_EMIT_REASON_RESOURCE_CONTRACT_MISMATCH,
              ctx->current_instruction_index, 2);
          return;
        }
        resource_dimension = declared_dimension;
        if (!resource_dimension || strcmp(resource_dimension, "buffer") == 0 ||
            strcmp(resource_dimension, "raw") == 0 ||
            strcmp(resource_dimension, "structured") == 0) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
              HLSL_EMIT_REASON_UNSUPPORTED_FEATURE,
              ctx->current_instruction_index, 2);
          return;
        }

        const bool is_uav = resource_operand->type == OPERAND_TYPE_UAV;
        const bool is_1d = strcmp(resource_dimension, "1d") == 0;
        const bool is_1d_array =
            strcmp(resource_dimension, "1darray") == 0;
        const bool is_2d = strcmp(resource_dimension, "2d") == 0 ||
                           strcmp(resource_dimension, "cube") == 0;
        const bool is_2d_array =
            strcmp(resource_dimension, "2darray") == 0 ||
            strcmp(resource_dimension, "cubearray") == 0;
        const bool is_3d = strcmp(resource_dimension, "3d") == 0;
        const bool is_2dms = strcmp(resource_dimension, "2dms") == 0;
        const bool is_2dms_array =
            strcmp(resource_dimension, "2dmsarray") == 0;
        if (!is_1d && !is_1d_array && !is_2d && !is_2d_array &&
            !is_3d && !is_2dms && !is_2dms_array) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
              HLSL_EMIT_REASON_UNSUPPORTED_FEATURE,
              ctx->current_instruction_index, 2);
          return;
        }

        char res_line[512];
        sb_append_spaces(sb, ctx->indent + 4);
        sb_append(sb, "uint w = 0, h = 0, d = 0, elements = 0, levels = 1, samples = 0;\n");
        sb_append_spaces(sb, ctx->indent + 4);
        if (is_uav) {
          if (is_1d)
            hlsl_format_checked(ctx, res_line, sizeof(res_line), "%s.GetDimensions(w);\n",
                     src1);
          else if (is_1d_array)
            hlsl_format_checked(ctx, res_line, sizeof(res_line),
                     "%s.GetDimensions(w, elements);\n", src1);
          else if (is_2d)
            hlsl_format_checked(ctx, res_line, sizeof(res_line),
                     "%s.GetDimensions(w, h);\n", src1);
          else if (is_2d_array)
            hlsl_format_checked(ctx, res_line, sizeof(res_line),
                     "%s.GetDimensions(w, h, elements);\n", src1);
          else if (is_3d)
            hlsl_format_checked(ctx, res_line, sizeof(res_line),
                     "%s.GetDimensions(w, h, d);\n", src1);
          else {
            hlsl_emit_fail_instruction(
                ctx, HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
                HLSL_EMIT_REASON_INTERNAL_INVARIANT,
                ctx->current_instruction_index, 2);
            return;
          }
        } else if (is_2dms) {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s.GetDimensions(w, h, samples);\n", src1);
        } else if (is_2dms_array) {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s.GetDimensions(w, h, elements, samples);\n", src1);
        } else if (is_1d) {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s.GetDimensions(asuint(%s), w, levels);\n", src1,
                   src0);
        } else if (is_1d_array) {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s.GetDimensions(asuint(%s), w, elements, levels);\n",
                   src1, src0);
        } else if (is_2d) {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s.GetDimensions(asuint(%s), w, h, levels);\n", src1,
                   src0);
        } else if (is_2d_array) {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s.GetDimensions(asuint(%s), w, h, elements, levels);\n",
                   src1, src0);
        } else {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s.GetDimensions(asuint(%s), w, h, d, levels);\n",
                   src1, src0);
        }
        sb_append(sb, res_line);

        const char *y_value = is_1d_array ? "elements"
                              : is_1d ? "0u" : "h";
        const char *z_value = (is_2d_array || is_2dms_array)
                                  ? "elements"
                                  : (is_3d ? "d" : "0u");
        char resource_swizzle[32] = "";
        if (!format_resource_result_swizzle(resource_operand, dm, 4, false,
                                            resource_swizzle,
                                            sizeof(resource_swizzle))) {
          hlsl_emit_fail_instruction(
              ctx, HLSL_EMIT_STATUS_INVALID_PROGRAM,
              HLSL_EMIT_REASON_INVALID_OPERAND,
              ctx->current_instruction_index, 2);
          return;
        }
        sb_append_spaces(sb, ctx->indent + 4);
        if (inst->resource_info_return_type == 2) {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s = uint4(w, %s, %s, levels)%s;\n", dest, y_value,
                   z_value, resource_swizzle);
        } else if (inst->resource_info_return_type == 1) {
          const char *y_rcp = is_1d_array ? "(float)elements"
                              : is_1d ? "0.0f" : "1.0f / (float)h";
          const char *z_rcp = (is_2d_array || is_2dms_array)
                                  ? "(float)elements"
                                  : (is_3d ? "1.0f / (float)d" : "0.0f");
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s = float4(1.0f / (float)w, %s, %s, (float)levels)%s;\n",
                   dest, y_rcp, z_rcp, resource_swizzle);
        } else {
          hlsl_format_checked(ctx, res_line, sizeof(res_line),
                   "%s = float4((float)w, (float)%s, (float)%s, "
                   "(float)levels)%s;\n",
                   dest, y_value, z_value, resource_swizzle);
        }
        sb_append(sb, res_line);
        sb_append_spaces(sb, ctx->indent);
        sb_append(sb, "}\n");
        break;
      }
      case USIL_OP_SAMPLEINFO: {
        *is_custom = true;
        if (inst->operand_count < 2 ||
            inst->operands[1].type != OPERAND_TYPE_RESOURCE) {
          sb->failed = true;
          return;
        }
        const USILTexture *texture =
            find_instruction_texture(program, inst, 1);
        const char *resource_dimension = texture ? texture->dimension : NULL;
        const bool is_2dms =
            resource_dimension && strcmp(resource_dimension, "2dms") == 0;
        const bool is_2dms_array =
            resource_dimension &&
            strcmp(resource_dimension, "2dmsarray") == 0;
        if (!is_2dms && !is_2dms_array) {
          sb->failed = true;
          return;
        }
        sb_append(sb, "{\n");
        sb_append_spaces(sb, ctx->indent + 4);
        sb_append(sb, is_2dms ? "uint w, h, samples;\n"
                              : "uint w, h, elements, samples;\n");
        sb_append_spaces(sb, ctx->indent + 4);
        if (is_2dms) {
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                   "%s.GetDimensions(w, h, samples);\n", src0);
        } else {
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                   "%s.GetDimensions(w, h, elements, samples);\n", src0);
        }
        sb_append(sb, line_buf);
        sb_append_spaces(sb, ctx->indent + 4);
        hlsl_format_checked(
            ctx, line_buf, line_buf_sz,
            inst->sample_info_return_type == 1u
                ? "%s = samples;\n"
                : "%s = (float)samples;\n",
            dest);
        sb_append(sb, line_buf);
        sb_append_spaces(sb, ctx->indent);
        sb_append(sb, "}\n");
        break;
      }
      case USIL_OP_DERIV_RTX:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = ddx(%s);", dest, src0);
        break;
      case USIL_OP_DERIV_RTY:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = ddy(%s);", dest, src0);
        break;
      case USIL_OP_DERIV_RTX_COARSE:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = ddx_coarse(%s);", dest, src0);
        break;
      case USIL_OP_DERIV_RTY_COARSE:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = ddy_coarse(%s);", dest, src0);
        break;
      case USIL_OP_DERIV_RTX_FINE:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = ddx_fine(%s);", dest, src0);
        break;
      case USIL_OP_DERIV_RTY_FINE:
        hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = ddy_fine(%s);", dest, src0);
        break;
      case USIL_OP_IMM_ATOMIC_IADD: {
        *is_custom = true;
        int uav_register = -1;
        int byte_offset = -1;
        if (!hlsl_structured_atomic_address(program, inst, &uav_register,
                                            &byte_offset) ||
            uav_register != inst->operands[1].register_index) {
          sb->failed = true;
          return;
        }
        char element_index[128];
        char increment[128];
        format_operand_hlsl(ctx, &inst->operands[2], false, true, 16, false,
                            element_index, sizeof(element_index));
        format_operand_hlsl(ctx, &inst->operands[3], false, true, 16, false,
                            increment, sizeof(increment));
        if (sb->failed) return;
        if (inst->operands[0].type == OPERAND_TYPE_NULL) {
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                              "InterlockedAdd(%s[%s].m%d, %s);", src0,
                              element_index, byte_offset, increment);
          sb_append_spaces(sb, ctx->indent);
          sb_append(sb, line_buf);
          sb_append(sb, "\n");
        } else {
          sb_append_spaces(sb, ctx->indent);
          sb_append(sb, "{\n");
          sb_append_spaces(sb, ctx->indent + 4);
          sb_append(sb, "uint atomic_temp;\n");
          sb_append_spaces(sb, ctx->indent + 4);
          hlsl_format_checked(ctx, line_buf, line_buf_sz,
                              "InterlockedAdd(%s[%s].m%d, %s, atomic_temp);",
                              src0, element_index, byte_offset, increment);
          sb_append(sb, line_buf);
          sb_append(sb, "\n");
          sb_append_spaces(sb, ctx->indent + 4);
          if (ctx->use_uint_temps) {
            hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = atomic_temp;", dest);
          } else {
            hlsl_format_checked(ctx, line_buf, line_buf_sz, "%s = asfloat(atomic_temp);", dest);
          }
          sb_append(sb, line_buf);
          sb_append(sb, "\n");
          sb_append_spaces(sb, ctx->indent);
          sb_append(sb, "}\n");
        }
        break;
      }
      case USIL_OP_LDMS: {
        if (inst->operand_count < 4 ||
            inst->operands[2].type != OPERAND_TYPE_RESOURCE) {
          sb->failed = true;
          return;
        }
        const USILTexture *texture =
            find_instruction_texture(program, inst, 2);
        const char *dimension = texture ? texture->dimension : NULL;
        const int coordinate_components =
            multisample_coordinate_count(dimension);
        const int result_components =
            typed_resource_result_component_count(texture);
        char result_swizzle[32] = "";
        if (coordinate_components == 0 || result_components == 0 ||
            !format_resource_result_swizzle(
                &inst->operands[2], dm, result_components, false,
                result_swizzle, sizeof(result_swizzle))) {
          sb->failed = true;
          return;
        }
        char coordinate[256];
        char sample_index[256];
        format_signed_texture_coordinate(ctx, &inst->operands[1],
                                         coordinate_components, coordinate,
                                         sizeof(coordinate));
        format_operand_hlsl(ctx, &inst->operands[3], true, false, 16, false,
                            sample_index, sizeof(sample_index));
        if (sb->failed) return;
        hlsl_format_checked(ctx, line_buf, line_buf_sz,
                            "%s = %s.Load(%s, %s)%s;", dest, src1,
                            coordinate, sample_index, result_swizzle);
        break;
      }
      default:
        hlsl_emit_fail_instruction(ctx, HLSL_EMIT_STATUS_UNSUPPORTED,
                                   HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
                                   ctx->current_instruction_index, -1);
        break;
    }
}

