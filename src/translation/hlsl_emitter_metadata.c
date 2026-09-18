// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "io/parameter_layout.h"
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const BuiltinVariable g_builtins[] = {
    // =========================================================================
    // All Unity builtin cbuffer variables from UnityShaderVariables.cginc
    // Format: {cb_name, var_name, byte_offset, type, rows, dim, is_matrix, array_size}
    // =========================================================================

    // UnityPerDraw (register varies per shader)
    {  "UnityPerDraw", "unity_ObjectToWorld",       0,   0, 4, 4, 1, 0},
    {  "UnityPerDraw", "unity_WorldToObject",       64,  0, 4, 4, 1, 0},
    {  "UnityPerDraw", "unity_LODFade",             128, 0, 0, 4, 0, 0},
    {  "UnityPerDraw", "unity_WorldTransformParams",144, 0, 0, 4, 0, 0},
    {  "UnityPerDraw", "unity_RenderingLayer",      160, 0, 0, 4, 0, 0},

    /* UnityShaderVariables.cginc owns this one-row block in the
     * UNITY_SINGLE_PASS_STEREO source domain.  It is a distinct binding from
     * the legacy UnityPerFrame field with the same variable spelling. */
    {"UnityStereoEyeIndex", "unity_StereoEyeIndex", 0, 1, 0, 1, 0, 0},

    /* UnityShaderVariables.cginc declares this block when
     * USING_STEREO_MATRICES is active.  Array elements retain one complete
     * four-row matrix each; the offsets below are the exact declaration
     * order in Unity 2021.3.  These records are compile-environment
     * contracts, not guesses from a particular shader's access pattern. */
    {"UnityStereoGlobals", "unity_StereoMatrixP",             0, 0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoMatrixV",           128, 0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoMatrixInvV",        256, 0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoMatrixVP",          384, 0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoCameraProjection",  512, 0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoCameraInvProjection",640,0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoWorldToCamera",     768, 0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoCameraToWorld",     896, 0, 4, 4, 1, 2},
    {"UnityStereoGlobals", "unity_StereoWorldSpaceCameraPos",1024,0,0, 3, 0, 2},
    {"UnityStereoGlobals", "unity_StereoScaleOffset",      1056, 0, 0, 4, 0, 2},

    // UnityPerFrame
    {"UnityPerFrame", "glstate_lightmodel_ambient", 0,   0, 0, 4, 0, 0},
    {"UnityPerFrame", "unity_AmbientSky",           16,  0, 0, 4, 0, 0},
    {"UnityPerFrame", "unity_AmbientEquator",       32,  0, 0, 4, 0, 0},
    {"UnityPerFrame", "unity_AmbientGround",        48,  0, 0, 4, 0, 0},
    {"UnityPerFrame", "unity_IndirectSpecColor",    64,  0, 0, 4, 0, 0},
    {"UnityPerFrame", "glstate_matrix_projection",  80,  0, 4, 4, 1, 0},
    {"UnityPerFrame", "unity_MatrixV",              144, 0, 4, 4, 1, 0},
    {"UnityPerFrame", "unity_MatrixInvV",           208, 0, 4, 4, 1, 0},
    {"UnityPerFrame", "unity_MatrixVP",             272, 0, 4, 4, 1, 0},
    {"UnityPerFrame", "unity_StereoEyeIndex",       336, 1, 0, 1, 0, 0},
    {"UnityPerFrame", "unity_ShadowColor",          352, 0, 0, 4, 0, 0},

    // UnityPerCamera
    {"UnityPerCamera", "_Time",                     0,   0, 0, 4, 0, 0},
    {"UnityPerCamera", "_SinTime",                  16,  0, 0, 4, 0, 0},
    {"UnityPerCamera", "_CosTime",                  32,  0, 0, 4, 0, 0},
    {"UnityPerCamera", "unity_DeltaTime",           48,  0, 0, 4, 0, 0},
    {"UnityPerCamera", "_WorldSpaceCameraPos",      64,  0, 0, 3, 0, 0},
    {"UnityPerCamera", "_ProjectionParams",         80,  0, 0, 4, 0, 0},
    {"UnityPerCamera", "_ScreenParams",             96,  0, 0, 4, 0, 0},
    {"UnityPerCamera", "_ZBufferParams",            112, 0, 0, 4, 0, 0},
    {"UnityPerCamera", "unity_OrthoParams",         128, 0, 0, 4, 0, 0},
    {"UnityPerCamera", "unity_HalfStereoSeparation", 144, 0, 0, 4, 0, 0},

    // UnityPerCameraRare
    {"UnityPerCameraRare", "unity_CameraWorldClipPlanes", 0,  0, 0, 4, 0, 6},
    {"UnityPerCameraRare", "unity_CameraProjection",      96, 0, 4, 4, 1, 0},
    {"UnityPerCameraRare", "unity_CameraInvProjection",   160,0, 4, 4, 1, 0},
    {"UnityPerCameraRare", "unity_WorldToCamera",         224,0, 4, 4, 1, 0},
    {"UnityPerCameraRare", "unity_CameraToWorld",         288,0, 4, 4, 1, 0},

    // UnityLighting
    {"UnityLighting", "_WorldSpaceLightPos0",       0,   0, 0, 4, 0, 0},
    {"UnityLighting", "_LightPositionRange",        16,  0, 0, 4, 0, 0},
    {"UnityLighting", "_LightProjectionParams",     32,  0, 0, 4, 0, 0},
    {"UnityLighting", "unity_4LightPosX0",          48,  0, 0, 4, 0, 0},
    {"UnityLighting", "unity_4LightPosY0",          64,  0, 0, 4, 0, 0},
    {"UnityLighting", "unity_4LightPosZ0",          80,  0, 0, 4, 0, 0},
    {"UnityLighting", "unity_4LightAtten0",         96,  0, 0, 4, 0, 0},
    {"UnityLighting", "unity_LightColor",           112, 0, 0, 4, 0, 8},
    {"UnityLighting", "unity_LightPosition",        240, 0, 0, 4, 0, 8},
    {"UnityLighting", "unity_LightAtten",           368, 0, 0, 4, 0, 8},
    {"UnityLighting", "unity_SpotDirection",        496, 0, 0, 4, 0, 8},
    {"UnityLighting", "unity_SHAr",                 624, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_SHAg",                 640, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_SHAb",                 656, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_SHBr",                 672, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_SHBg",                 688, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_SHBb",                 704, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_SHC",                  720, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_OcclusionMaskSelector",736, 0, 0, 4, 0, 0},
    {"UnityLighting", "unity_ProbesOcclusion",      752, 0, 0, 4, 0, 0},

    // UnityLightingOld
    {"UnityLightingOld", "unity_LightColor0",       0,   0, 0, 3, 0, 0},
    {"UnityLightingOld", "unity_LightColor1",       16,  0, 0, 3, 0, 0},
    {"UnityLightingOld", "unity_LightColor2",       32,  0, 0, 3, 0, 0},
    {"UnityLightingOld", "unity_LightColor3",       48,  0, 0, 3, 0, 0},

    // UnityShadows
    {"UnityShadows", "unity_ShadowSplitSpheres",    0,   0, 0, 4, 0, 4},
    {"UnityShadows", "unity_ShadowSplitSqRadii",    64,  0, 0, 4, 0, 0},
    {"UnityShadows", "unity_LightShadowBias",       80,  0, 0, 4, 0, 0},
    {"UnityShadows", "_LightSplitsNear",            96,  0, 0, 4, 0, 0},
    {"UnityShadows", "_LightSplitsFar",             112, 0, 0, 4, 0, 0},
    {"UnityShadows", "unity_WorldToShadow",         128, 0, 4, 4, 1, 4},
    {"UnityShadows", "_LightShadowData",            384, 0, 0, 4, 0, 0},
    {"UnityShadows", "unity_ShadowFadeCenterAndType",400,0, 0, 4, 0, 0},

    // UnityFog
    {"UnityFog", "unity_FogColor",                  0,   0, 0, 4, 0, 0},
    {"UnityFog", "unity_FogParams",                 16,  0, 0, 4, 0, 0},

    // UnityLightmaps
    {"UnityLightmaps", "unity_LightmapST",          0,   0, 0, 4, 0, 0},
    {"UnityLightmaps", "unity_DynamicLightmapST",   16,  0, 0, 4, 0, 0},

    // UnityReflectionProbes
    {"UnityReflectionProbes", "unity_SpecCube0_BoxMax",      0,  0, 0, 4, 0, 0},
    {"UnityReflectionProbes", "unity_SpecCube0_BoxMin",      16, 0, 0, 4, 0, 0},
    {"UnityReflectionProbes", "unity_SpecCube0_ProbePosition",32,0, 0, 4, 0, 0},
    {"UnityReflectionProbes", "unity_SpecCube0_HDR",         48, 0, 0, 4, 0, 0},
    {"UnityReflectionProbes", "unity_SpecCube1_BoxMax",      64, 0, 0, 4, 0, 0},
    {"UnityReflectionProbes", "unity_SpecCube1_BoxMin",      80, 0, 0, 4, 0, 0},
    {"UnityReflectionProbes", "unity_SpecCube1_ProbePosition",96,0, 0, 4, 0, 0},
    {"UnityReflectionProbes", "unity_SpecCube1_HDR",         112,0, 0, 4, 0, 0},

    // UnityProbeVolume
    {"UnityProbeVolume", "unity_ProbeVolumeParams",          0,  0, 0, 4, 0, 0},
    {"UnityProbeVolume", "unity_ProbeVolumeWorldToObject",   16, 0, 4, 4, 1, 0},
    {"UnityProbeVolume", "unity_ProbeVolumeSizeInv",         80, 0, 0, 3, 0, 0},
    {"UnityProbeVolume", "unity_ProbeVolumeMin",             96, 0, 0, 3, 0, 0},

    // UnityPerDrawRare
    {"UnityPerDrawRare", "glstate_matrix_transpose_modelview0", 0, 0, 4, 4, 1, 0},
};
const size_t g_builtins_count = sizeof(g_builtins) / sizeof(g_builtins[0]);

uint32_t get_var_occupied_regs(uint32_t is_matrix, uint32_t rows,
                               uint32_t matrix_array_size) {
  uint32_t array_size = (matrix_array_size > 0) ? matrix_array_size : 1;
  if (is_matrix) {
    return array_size * rows;
  } else {
    return array_size * ((rows > 1) ? rows : 1);
  }
}

bool resolve_variable_info(const SerializedProgramParameters *params,
                           const char *var_name, uint32_t *out_dim,
                           uint32_t *out_byte_offset,
                           bool *out_is_matrix_or_array) {
  /* This context-free API has no emission mode, so it is authority-only.
   * Hardcoded builtins are a readable presentation fallback, never metadata. */
  if (params) {
    for (int i = 0; i < params->cb_count; i++) {
      for (int j = 0; j < params->constant_buffers[i].var_count; j++) {
        const char* candidate_name =
            params->constant_buffers[i].variables[j].name;
        if (candidate_name && var_name &&
            strcmp(candidate_name, var_name) == 0) {
          const SerializedVariable *v =
              &params->constant_buffers[i].variables[j];
          DecodedVariableLayout layout;
          if (!parameter_layout_decode(params, v, &layout)) return false;
          
          if (out_dim)
            *out_dim = layout.columns;
          if (out_byte_offset)
            *out_byte_offset = layout.byte_offset;
          if (out_is_matrix_or_array) {
            *out_is_matrix_or_array =
                (layout.is_matrix || layout.array_size > 1);
          }
          return true;
        }
      }
    }
  }
  return false;
}

int resolve_variable_type(const HLSLEmitterContext* ctx, const char* var_name) {
  DecodedVariableLayout layout;
  return resolve_variable_layout_ctx(ctx, var_name, &layout)
             ? (int)layout.scalar_type
             : 0;
}

bool resolve_variable_is_matrix(const HLSLEmitterContext* ctx,
                                const char *var_name) {
  DecodedVariableLayout layout;
  return resolve_variable_layout_ctx(ctx, var_name, &layout) &&
         layout.is_matrix;
}

bool resolve_variable_is_row_major(const HLSLEmitterContext* ctx,
                                   const char *var_name) {
  if (!ctx || !var_name) return false;
  if (ctx->cbuffer_layouts_built) {
    for (int buffer = 0; buffer < ctx->cbuffer_layout_count; ++buffer) {
      const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[buffer];
      for (int variable = 0; variable < layout->variable_count; ++variable) {
        if (strcmp(layout->variables[variable].name, var_name) == 0) {
          return layout->variables[variable].row_major;
        }
      }
    }
  }

  const SerializedProgramParameters *sources[2] = {
      ctx->params, ctx->common_params};
  for (size_t source = 0; source < 2; ++source) {
    const SerializedProgramParameters *parameters = sources[source];
    if (!parameters) continue;
    for (int buffer = 0; buffer < parameters->cb_count; ++buffer) {
      for (int variable = 0;
           variable < parameters->constant_buffers[buffer].var_count;
           ++variable) {
        const SerializedVariable *value =
            &parameters->constant_buffers[buffer].variables[variable];
        if (!value->name || strcmp(value->name, var_name) != 0) continue;
        DecodedVariableLayout decoded;
        return parameter_layout_decode(parameters, value, &decoded) &&
               decoded.is_matrix;
      }
    }
  }
  if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE) {
    for (size_t index = 0; index < g_builtins_count; ++index) {
      if (strcmp(g_builtins[index].var_name, var_name) == 0) return false;
    }
  }
  return false;
}

// ============================================================================
// Deterministic cbuffer register mapping
// ============================================================================
//
// The constant-buffer resource bindings from BindCB provide the definitive
// mapping from cbuffer name to DXBC register index (bind_index). This is the
// same BindPoint from D3D shader reflection, serialized via the IPC protocol
// ("cbbind: name bind_index") into the binary blob.
//
// After the parser off-by-one fix, constant_buffers[i] matches bind_index=i
// exactly, so Phase 1 alone is sufficient for deterministic mapping.
// ============================================================================

bool build_cbuffer_register_map(HLSLEmitterContext *ctx) {
  if (!ctx) return false;
  const SerializedProgramParameters *params = ctx->params;
  free(ctx->cb_reg_map);
  ctx->cb_reg_map = NULL;
  ctx->cb_reg_map_count = 0;
  ctx->cb_reg_map_alloc = 0;

  // Use the definitive resource bindings (SerializedResourceParam)
  // Scan BOTH program params AND common_params
  const SerializedProgramParameters *sources[] = { params, ctx->common_params };
  size_t map_capacity = 0;
  for (size_t src = 0; src < sizeof(sources) / sizeof(sources[0]); src++) {
    const SerializedProgramParameters *p = sources[src];
    if (!p) continue;
    if (p->res_count < 0 || (p->res_count > 0 && !p->resources)) {
      return false;
    }
    for (int i = 0; i < p->res_count; i++) {
      if (p->resources[i].bind_type !=
          SERIALIZED_RESOURCE_CONSTANT_BUFFER) {
        continue;
      }
      if (p->resources[i].bind_index > INT_MAX || map_capacity == INT_MAX) {
        return false;
      }
      map_capacity++;
    }
  }
  if (map_capacity > SIZE_MAX / sizeof(*ctx->cb_reg_map)) return false;
  if (map_capacity > 0) {
    ctx->cb_reg_map = (CBufferRegMapEntry *)calloc(
        map_capacity, sizeof(*ctx->cb_reg_map));
    if (!ctx->cb_reg_map) return false;
    ctx->cb_reg_map_alloc = (int)map_capacity;
  }
  
  for (size_t src = 0; src < sizeof(sources) / sizeof(sources[0]); src++) {
    const SerializedProgramParameters *p = sources[src];
    if (!p) continue;
    
    for (int i = 0; i < p->res_count; i++) {
      if (p->resources[i].bind_type !=
          SERIALIZED_RESOURCE_CONSTANT_BUFFER)
        continue;
      
      int bind_idx = (int)p->resources[i].bind_index;
      const char *res_name = p->resources[i].name;
      if (!res_name) return false;
      
      // Find the matching constant buffer in EITHER params or common_params
      int meta_idx = -1;
      if (params) {
        for (int m = 0; m < params->cb_count; m++) {
          if (params->constant_buffers[m].name &&
              strcmp(params->constant_buffers[m].name, res_name) == 0) {
            meta_idx = m;
            break;
          }
        }
      }
      if (meta_idx < 0 && ctx->common_params) {
        for (int m = 0; m < ctx->common_params->cb_count; m++) {
          if (ctx->common_params->constant_buffers[m].name &&
              strcmp(ctx->common_params->constant_buffers[m].name,
                     res_name) == 0) {
            meta_idx = (params ? params->cb_count : 0) + m;
            break;
          }
        }
      }
      
      // A conflicting binding is ambiguous and must remain unresolved.
      CBufferRegMapEntry *existing = NULL;
      for (int k = 0; k < ctx->cb_reg_map_count; k++) {
        if (ctx->cb_reg_map[k].dxbc_reg == bind_idx) {
          existing = &ctx->cb_reg_map[k];
          break;
        }
      }

      if (existing) {
        if (strcmp(existing->cb_name, res_name) != 0) {
          if (existing->authority == (uint8_t)(src + 1)) {
            existing->resolved = false;
            existing->metadata_cb_idx = -1;
          }
          /* A conflicting lower-priority common binding cannot replace the
           * stage-specific compiled binding. */
        }
      } else {
        if (ctx->cb_reg_map_count >= ctx->cb_reg_map_alloc) {
          free(ctx->cb_reg_map);
          ctx->cb_reg_map = NULL;
          ctx->cb_reg_map_count = 0;
          ctx->cb_reg_map_alloc = 0;
          return false;
        }
        CBufferRegMapEntry *entry = &ctx->cb_reg_map[ctx->cb_reg_map_count++];
        entry->dxbc_reg = bind_idx;
        entry->cb_name = res_name;
        entry->metadata_cb_idx = meta_idx;
        entry->resolved = true;
        entry->authority = (uint8_t)(src + 1);
      }
    }
  }
  return true;
}

// Look up cbuffer name from the pre-built deterministic map
const char *get_cbuffer_name_from_map(const HLSLEmitterContext *ctx,
                                      int cb_reg) {
  for (int i = 0; i < ctx->cb_reg_map_count; i++) {
    if (ctx->cb_reg_map[i].dxbc_reg == cb_reg && ctx->cb_reg_map[i].resolved) {
      return ctx->cb_reg_map[i].cb_name;
    }
  }
  return NULL;
}

// Legacy get_cbuffer_name (for callers without context)
const char *get_cbuffer_name(const SerializedProgramParameters *params,
                             int cb_reg) {
  if (!params)
    return NULL;
  // Only check resource bindings (the only reliable source without DXBC
  // context)
  for (int i = 0; i < params->res_count; i++) {
    if (params->resources[i].bind_index == (uint32_t)cb_reg &&
        params->resources[i].bind_type ==
            SERIALIZED_RESOURCE_CONSTANT_BUFFER) {
      return params->resources[i].name;
    }
  }
  return NULL;
}

static const char *resolve_cb_variable_from_parameters(
    const SerializedProgramParameters *parameters, const char *cb_name,
    uint32_t byte_offset, int *out_var_offset) {
  if (!parameters || !cb_name) return NULL;
  for (int buffer = 0; buffer < parameters->cb_count; ++buffer) {
    if (!parameters->constant_buffers[buffer].name ||
        strcmp(parameters->constant_buffers[buffer].name, cb_name) != 0) {
      continue;
    }
    const SerializedConstantBuffer *cb =
        &parameters->constant_buffers[buffer];
    for (int variable = 0; variable < cb->var_count; ++variable) {
      DecodedVariableLayout layout;
      if (!parameter_layout_decode(parameters, &cb->variables[variable],
                                   &layout)) {
        continue;
      }
      uint64_t end = (uint64_t)layout.byte_offset +
                     parameter_layout_byte_size(&layout);
      if ((uint64_t)byte_offset >= layout.byte_offset &&
          (uint64_t)byte_offset < end) {
        if (out_var_offset) {
          *out_var_offset = (int)(byte_offset - layout.byte_offset);
        }
        return cb->variables[variable].name;
      }
    }
  }
  return NULL;
}

// Context-aware resolve: uses the one projected declaration/helper layout.
const char *resolve_cb_variable_ctx(const HLSLEmitterContext *ctx, int cb_reg,
                                    int float4_offset, int component_hint, int *out_var_offset) {
  if (!ctx || float4_offset < 0 || component_hint < -1 ||
      component_hint > 3) return NULL;
  uint64_t wide_offset = (uint64_t)(uint32_t)float4_offset * 16u +
                         (component_hint >= 0
                              ? (uint32_t)component_hint * 4u
                              : 0u);
  if (wide_offset > UINT32_MAX) return NULL;
  uint32_t byte_offset = (uint32_t)wide_offset;

  if (ctx->cbuffer_layouts_built) {
    const HLSLCBufferLayout *layout =
        get_cbuffer_emission_layout(ctx, cb_reg);
    if (!layout || layout->raw_storage) return NULL;
    const TempVariable *match = NULL;
    for (int variable = 0; variable < layout->variable_count; ++variable) {
      const TempVariable *candidate = &layout->variables[variable];
      uint64_t end = (uint64_t)candidate->byte_offset +
                     candidate->byte_size;
      if ((uint64_t)byte_offset < candidate->byte_offset ||
          (uint64_t)byte_offset >= end) {
        continue;
      }
      if (match) return NULL;
      match = candidate;
    }
    if (!match) return NULL;
    if (out_var_offset) {
      *out_var_offset = (int)(byte_offset - match->byte_offset);
    }
    return match->name;
  }

  const char *cb_name = get_cbuffer_name_from_map(ctx, cb_reg);
  if (!cb_name) return NULL;

  const char *name = resolve_cb_variable_from_parameters(
      ctx->params, cb_name, byte_offset, out_var_offset);
  if (name) return name;
  name = resolve_cb_variable_from_parameters(
      ctx->common_params, cb_name, byte_offset, out_var_offset);
  if (name) return name;
  if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE) {
    const char *normalized = cb_name[0] == '$' ? cb_name + 1 : cb_name;
    for (size_t index = 0; index < g_builtins_count; ++index) {
      const BuiltinVariable *builtin = &g_builtins[index];
      uint32_t size = builtin->is_matrix || builtin->matrix_array_size
                          ? get_var_occupied_regs(
                                builtin->is_matrix, builtin->rows,
                                builtin->matrix_array_size) * 16u
                          : builtin->dim * 4u;
      if (strcmp(builtin->cb_name, normalized) == 0 &&
          byte_offset >= builtin->offset &&
          (uint64_t)byte_offset < (uint64_t)builtin->offset + size) {
        if (out_var_offset) {
          *out_var_offset = (int)(byte_offset - builtin->offset);
        }
        return builtin->var_name;
      }
    }
  }
  return NULL;
}


// Legacy resolve (for callers without context) - kept for compatibility
const char *resolve_cb_variable(const SerializedProgramParameters *params,
                                int cb_reg, int float4_offset,
                                int component_hint, int *out_var_offset) {
  if (!params)
    return NULL;

  const char *cb_name = get_cbuffer_name(params, cb_reg);
  if (!cb_name)
    return NULL;

  if (float4_offset < 0 || component_hint < -1 || component_hint > 3) {
    return NULL;
  }
  uint64_t byte_offset = (uint64_t)(uint32_t)float4_offset * 16u +
                         (component_hint >= 0
                              ? (uint32_t)component_hint * 4u
                              : 0u);
  if (byte_offset > UINT32_MAX) return NULL;
  return resolve_cb_variable_from_parameters(
      params, cb_name, (uint32_t)byte_offset, out_var_offset);
}

bool resolve_srv_name(const SerializedProgramParameters *params, int reg_idx,
                      SerializedResourceType bind_type,
                      const char **out_name) {
  if (!out_name) return false;
  *out_name = NULL;
  if (reg_idx < 0 || reg_idx >= HLSL_SM5_RESOURCE_REGISTER_COUNT ||
      (bind_type != SERIALIZED_RESOURCE_TEXTURE &&
       bind_type != SERIALIZED_RESOURCE_BUFFER)) {
    return false;
  }
  if (!params) return true;
  if (params->res_count < 0 ||
      (params->res_count > 0 && !params->resources)) {
    return false;
  }

  bool matched = false;
  for (int i = 0; i < params->res_count; ++i) {
    const SerializedResourceParam *resource = &params->resources[i];
    if (resource->bind_index != (uint32_t)reg_idx ||
        resource->bind_type != bind_type) {
      continue;
    }
    /* One architectural slot has one name for each exact Unity reflection
     * kind.  Even byte-identical duplicate records are conflicting authority,
     * while a TEXTURE/BUFFER pair is intentionally disambiguated by kind. */
    if (matched || !resource->name || resource->name[0] == '\0') {
      *out_name = NULL;
      return false;
    }
    matched = true;
    *out_name = resource->name;
  }
  return true;
}

bool resolve_srv_name_ctx(const HLSLEmitterContext *ctx, int reg_idx,
                          SerializedResourceType bind_type,
                          const char **out_name) {
  if (!out_name) return false;
  *out_name = NULL;
  if (!ctx) return false;
  if (!resolve_srv_name(ctx->params, reg_idx, bind_type, out_name))
    return false;
  if (*out_name) return true;
  return resolve_srv_name(ctx->common_params, reg_idx, bind_type, out_name);
}

const char *resolve_texture_name(const SerializedProgramParameters *params,
                                 int reg_idx) {
  const char *name = NULL;
  return resolve_srv_name(params, reg_idx, SERIALIZED_RESOURCE_TEXTURE,
                          &name)
             ? name
             : NULL;
}

static bool allocate_sampler_identifier(const char *texture_name,
                                        char **out_name) {
  static const char prefix[] = "sampler";
  if (!texture_name || !out_name) return false;
  *out_name = NULL;
  size_t texture_length = strlen(texture_name);
  if (sizeof(prefix) > SIZE_MAX - texture_length) return false;
  size_t allocation_size = sizeof(prefix) + texture_length;
  char *name = malloc(allocation_size);
  if (!name) return false;
  memcpy(name, prefix, sizeof(prefix) - 1u);
  memcpy(name + sizeof(prefix) - 1u, texture_name, texture_length + 1u);
  *out_name = name;
  return true;
}

static bool find_inline_sampler_state(
    const SerializedProgramParameters *params, int reg_idx,
    bool *out_found, uint32_t *out_state) {
  if (!out_found || !out_state) return false;
  *out_found = false;
  *out_state = 0;
  if (reg_idx < 0 || reg_idx >= HLSL_SM5_SAMPLER_REGISTER_COUNT)
    return false;
  if (!params) return true;
  if (params->res_count < 0 ||
      (params->res_count > 0 && !params->resources)) return false;

  for (int index = 0; index < params->res_count; ++index) {
    const SerializedResourceParam *resource = &params->resources[index];
    if (resource->bind_type != SERIALIZED_RESOURCE_SAMPLER ||
        resource->bind_index != (uint32_t)reg_idx) continue;
    /* One architectural binding has one serialized state.  Duplicate
     * records are not a second source of authority, even when their current
     * values happen to agree. */
    if (*out_found) return false;
    *out_found = true;
    *out_state = resource->sampler_state;
  }
  return true;
}

static bool inline_sampler_state_is_valid(uint32_t state) {
  const uint32_t known_mask = 0x0fffu;
  const uint32_t filter = state & 0x3u;
  const uint32_t aniso_log2 = (state >> 9u) & 0x7u;
  return (state & ~known_mask) == 0u && filter <= 2u &&
         aniso_log2 <= 4u;
}

static bool validate_inline_sampler_records(
    const SerializedProgramParameters *params) {
  if (!params) return true;
  if (params->res_count < 0 ||
      (params->res_count > 0 && !params->resources)) return false;
  bool seen[HLSL_SM5_SAMPLER_REGISTER_COUNT] = {false};
  for (int index = 0; index < params->res_count; ++index) {
    const SerializedResourceParam *resource = &params->resources[index];
    if (resource->bind_type != SERIALIZED_RESOURCE_SAMPLER) continue;
    if (resource->bind_index >= HLSL_SM5_SAMPLER_REGISTER_COUNT ||
        seen[resource->bind_index] ||
        !inline_sampler_state_is_valid(resource->sampler_state)) {
      return false;
    }
    seen[resource->bind_index] = true;
  }
  return true;
}

static bool allocate_inline_sampler_identifier(uint32_t state, int reg_idx,
                                               char **out_name) {
  static const char *const filters[] = {
      "point", "linear", "trilinear"};
  static const char *const wraps[] = {
      "repeat", "clamp", "mirror", "mirroronce"};
  if (!out_name) return false;
  *out_name = NULL;
  if (reg_idx < 0 || reg_idx >= HLSL_SM5_SAMPLER_REGISTER_COUNT ||
      !inline_sampler_state_is_valid(state)) return false;

  const uint32_t filter = state & 0x3u;
  const uint32_t wrap_u = (state >> 2u) & 0x3u;
  const uint32_t wrap_v = (state >> 4u) & 0x3u;
  const uint32_t wrap_w = (state >> 6u) & 0x3u;
  const bool comparison = (state & 0x100u) != 0u;
  const uint32_t aniso_log2 = (state >> 9u) & 0x7u;
  /* ParseInlineSamplerName recognizes exactly point/linear/trilinear and
   * anisotropy 2/4/8/16.  Zero is the serialized no-anisotropy value (one
   * sample); every other bit pattern is outside that accepted language. */
  if (filter >= sizeof(filters) / sizeof(filters[0])) return false;

  char aniso_suffix[16] = "";
  if (aniso_log2 != 0u) {
    unsigned int samples = 1u << aniso_log2;
    int written = snprintf(aniso_suffix, sizeof(aniso_suffix),
                           "_aniso%u", samples);
    if (written < 0 || (size_t)written >= sizeof(aniso_suffix)) return false;
  }
  const char *compare_suffix = comparison ? "_compare" : "";
  int length = snprintf(NULL, 0,
                        "sampler_dxbc_s%d_%s_%su_%sv_%sw%s%s",
                        reg_idx, filters[filter], wraps[wrap_u],
                        wraps[wrap_v], wraps[wrap_w], compare_suffix,
                        aniso_suffix);
  if (length < 0) return false;
  size_t allocation_size = (size_t)length + 1u;
  if (allocation_size == 0u) return false;
  char *name = malloc(allocation_size);
  if (!name) return false;
  int written = snprintf(name, allocation_size,
                         "sampler_dxbc_s%d_%s_%su_%sv_%sw%s%s",
                         reg_idx, filters[filter], wraps[wrap_u],
                         wraps[wrap_v], wraps[wrap_w], compare_suffix,
                         aniso_suffix);
  if (written != length) {
    free(name);
    return false;
  }
  *out_name = name;
  return true;
}

static bool resolve_texture_sampler_name(
    const SerializedProgramParameters *params, int reg_idx,
    char **out_name) {
  if (!out_name) return false;
  *out_name = NULL;
  if (reg_idx < 0 || reg_idx >= HLSL_SM5_SAMPLER_REGISTER_COUNT)
    return false;
  if (!params) return true;
  if (params->res_count < 0 ||
      (params->res_count > 0 && !params->resources)) return false;

  const char *texture_name = NULL;
  bool matched = false;
  for (int i = 0; i < params->res_count; i++) {
    if (params->resources[i].bind_type != SERIALIZED_RESOURCE_TEXTURE ||
        params->resources[i].sampler_index != (uint32_t)reg_idx) {
      continue;
    }
    if (matched) return true;
    if (!params->resources[i].name) return false;
    matched = true;
    texture_name = params->resources[i].name;
  }
  return !matched || allocate_sampler_identifier(texture_name, out_name);
}

static bool sampler_register_is_declared(const USILProgram *program,
                                         int register_index) {
  for (int index = 0; index < program->sampler_count; ++index) {
    if (program->samplers[index].reg_idx == register_index) return true;
  }
  return false;
}

static bool identifier_is_comparison_alias(const char *base,
                                           const char *candidate) {
  static const char suffix[] = "_cmp";
  size_t base_length = strlen(base);
  size_t candidate_length = strlen(candidate);
  return base_length <= SIZE_MAX - (sizeof(suffix) - 1u) &&
         candidate_length == base_length + sizeof(suffix) - 1u &&
         memcmp(candidate, base, base_length) == 0 &&
         memcmp(candidate + base_length, suffix, sizeof(suffix)) == 0;
}

static bool sampler_uses_two_identifiers(const HLSLEmitterContext *ctx,
                                         int register_index) {
  bool uses_regular = false;
  bool uses_comparison = false;
  get_sampler_usage(ctx->program, register_index, &uses_regular,
                    &uses_comparison);
  return uses_regular && uses_comparison;
}

static bool sampler_identifier_conflicts_with_lower(
    const HLSLEmitterContext *ctx, int register_index,
    const char *candidate) {
  const bool candidate_has_comparison_alias =
      sampler_uses_two_identifiers(ctx, register_index);
  for (int lower = 0; lower < register_index; ++lower) {
    const char *accepted = ctx->sampler_names[lower];
    if (!accepted || !sampler_register_is_declared(ctx->program, lower))
      continue;
    const bool accepted_has_comparison_alias =
        sampler_uses_two_identifiers(ctx, lower);
    if (strcmp(accepted, candidate) == 0 ||
        (accepted_has_comparison_alias &&
         identifier_is_comparison_alias(accepted, candidate)) ||
        (candidate_has_comparison_alias &&
         identifier_is_comparison_alias(candidate, accepted))) {
      return true;
    }
  }
  return false;
}

static char *allocate_disambiguated_sampler_identifier(
    const char *base, int register_index, unsigned int attempt) {
  char suffix[64];
  int written = attempt == 0
                    ? snprintf(suffix, sizeof(suffix), "_dxbc_s%d",
                               register_index)
                    : snprintf(suffix, sizeof(suffix), "_dxbc_s%d_%u",
                               register_index, attempt);
  if (written < 0 || (size_t)written >= sizeof(suffix)) return NULL;
  size_t base_length = strlen(base);
  size_t suffix_length = (size_t)written;
  if (base_length > SIZE_MAX - suffix_length - 1u) return NULL;
  char *result = malloc(base_length + suffix_length + 1u);
  if (!result) return NULL;
  memcpy(result, base, base_length);
  memcpy(result + base_length, suffix, suffix_length + 1u);
  return result;
}

static bool disambiguate_declared_sampler_identifiers(
    HLSLEmitterContext *ctx) {
  /* A serialized texture name identifies a texture/sampler association, not
   * a globally unique HLSL variable.  Distinct architectural s# registers
   * must nevertheless have distinct declarations.  Preserve the candidate
   * for the lowest register and derive every collision from the binding
   * number; no source-name or corpus heuristic participates. */
  for (int reg = 0; reg < HLSL_SM5_SAMPLER_REGISTER_COUNT; ++reg) {
    if (!ctx->sampler_names[reg] ||
        !sampler_register_is_declared(ctx->program, reg)) continue;
    if (!sampler_identifier_conflicts_with_lower(
            ctx, reg, ctx->sampler_names[reg])) continue;

    char *replacement = NULL;
    /* Each lower s# contributes at most a base and `_cmp` symbol, so one more
     * candidate than twice the architectural register count guarantees a
     * free deterministic spelling unless allocation itself fails. */
    for (unsigned int attempt = 0;
         attempt < HLSL_SM5_SAMPLER_REGISTER_COUNT * 2u + 1u; ++attempt) {
      replacement = allocate_disambiguated_sampler_identifier(
          ctx->sampler_names[reg], reg, attempt);
      if (!replacement) return false;
      if (!sampler_identifier_conflicts_with_lower(ctx, reg, replacement))
        break;
      free(replacement);
      replacement = NULL;
    }
    if (!replacement) return false;
    free(ctx->sampler_names[reg]);
    ctx->sampler_names[reg] = replacement;
  }
  return true;
}

bool resolve_sampler_name(const SerializedProgramParameters *params,
                          int reg_idx, char **out_name) {
  if (!out_name) return false;
  *out_name = NULL;
  if (reg_idx < 0 || reg_idx >= HLSL_SM5_SAMPLER_REGISTER_COUNT)
    return false;
  if (!params) return true;

  /* Serialized sampler records are the compiler's inline-state authority and
   * therefore outrank texture/sampler associations.  Their name field is not
   * a source identifier: Unity loads exactly (state, bindPoint), so invert the
   * accepted inline-name grammar from those bits. */
  bool found_inline = false;
  uint32_t inline_state = 0;
  if (!find_inline_sampler_state(params, reg_idx, &found_inline,
                                 &inline_state)) return false;
  if (found_inline) {
    return allocate_inline_sampler_identifier(inline_state, reg_idx,
                                              out_name);
  }
  return resolve_texture_sampler_name(params, reg_idx, out_name);
}

bool resolve_variable_layout_ctx(const HLSLEmitterContext* ctx,
                                 const char* var_name,
                                 DecodedVariableLayout* out_layout) {
  if (!ctx || !var_name || !out_layout) return false;

  if (ctx->cbuffer_layouts_built) {
    for (int buffer = 0; buffer < ctx->cbuffer_layout_count; ++buffer) {
      const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[buffer];
      for (int variable = 0; variable < layout->variable_count; ++variable) {
        const TempVariable *value = &layout->variables[variable];
        if (strcmp(value->name, var_name) != 0) continue;
        memset(out_layout, 0, sizeof(*out_layout));
        out_layout->byte_offset = value->byte_offset;
        out_layout->array_size = value->matrix_array_size;
        out_layout->scalar_type = value->type;
        out_layout->is_matrix = value->is_matrix != 0;
        out_layout->rows = value->rows;
        out_layout->columns = value->dim;
        return out_layout->rows >= 1 && out_layout->rows <= 4 &&
               out_layout->columns >= 1 && out_layout->columns <= 4;
      }
    }
  }

  /* Stage-specific compiled parameters are the first authority.  Common
   * TypeTree parameters fill only what the stage block does not provide. */
  const SerializedProgramParameters* sources[2] = {
      ctx->params, ctx->common_params
  };
  for (int source = 0; source < 2; source++) {
    const SerializedProgramParameters* parameters = sources[source];
    if (!parameters) continue;
    for (int i = 0; i < parameters->cb_count; i++) {
      for (int j = 0; j < parameters->constant_buffers[i].var_count; j++) {
        const SerializedVariable* variable =
            &parameters->constant_buffers[i].variables[j];
        if (variable->name && strcmp(variable->name, var_name) == 0) {
          return parameter_layout_decode(parameters, variable, out_layout);
        }
      }
    }
  }
  if (ctx->emit_mode == HLSL_EMIT_MODE_READABLE) {
    for (size_t i = 0; i < g_builtins_count; i++) {
      if (strcmp(g_builtins[i].var_name, var_name) != 0) continue;
      memset(out_layout, 0, sizeof(*out_layout));
      out_layout->byte_offset = g_builtins[i].offset;
      out_layout->array_size = g_builtins[i].matrix_array_size;
      out_layout->scalar_type = g_builtins[i].type;
      out_layout->is_matrix = g_builtins[i].is_matrix != 0;
      out_layout->rows = out_layout->is_matrix ? g_builtins[i].rows : 1;
      out_layout->columns = g_builtins[i].dim;
      return out_layout->rows >= 1 && out_layout->rows <= 4 &&
             out_layout->columns >= 1 && out_layout->columns <= 4;
    }
  }
  return false;
}

static bool is_sample_opcode(USILOpcode opcode) {
  return opcode == USIL_OP_SAMPLE || opcode == USIL_OP_SAMPLE_C ||
         opcode == USIL_OP_SAMPLE_C_LZ ||
         opcode == USIL_OP_SAMPLE_L || opcode == USIL_OP_SAMPLE_D ||
         opcode == USIL_OP_SAMPLE_B;
}

void get_sampler_usage(const USILProgram *program, int sampler_reg,
                       bool *uses_regular, bool *uses_comparison) {
  *uses_regular = false;
  *uses_comparison = false;
  for (int i = 0; i < program->instruction_count; i++) {
    const USILInstruction *inst = &program->instructions[i];
    if (!is_sample_opcode(inst->opcode)) continue;
    for (int j = 0; j < inst->operand_count; j++) {
      const DXBCOperand *operand = &inst->operands[j];
      if (operand->type != OPERAND_TYPE_SAMPLER ||
          operand->register_index != sampler_reg) {
        continue;
      }
      if (inst->opcode == USIL_OP_SAMPLE_C ||
          inst->opcode == USIL_OP_SAMPLE_C_LZ) {
        *uses_comparison = true;
      } else {
        *uses_regular = true;
      }
      break;
    }
  }
}

void free_sampler_name_map(HLSLEmitterContext *ctx) {
  if (!ctx) return;
  for (int i = 0; i < HLSL_SM5_SAMPLER_REGISTER_COUNT; ++i) {
    free(ctx->sampler_names[i]);
    ctx->sampler_names[i] = NULL;
  }
}

bool build_sampler_name_map(HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->program) return false;
  free_sampler_name_map(ctx);
  if (!validate_inline_sampler_records(ctx->params) ||
      !validate_inline_sampler_records(ctx->common_params)) goto fail;
  int paired_texture[HLSL_SM5_SAMPLER_REGISTER_COUNT];
  bool inline_sampler[HLSL_SM5_SAMPLER_REGISTER_COUNT] = {false};
  uint32_t inline_state[HLSL_SM5_SAMPLER_REGISTER_COUNT] = {0};
  for (int i = 0; i < HLSL_SM5_SAMPLER_REGISTER_COUNT; i++) {
    paired_texture[i] = -1;
    if (!find_inline_sampler_state(ctx->params, i, &inline_sampler[i],
                                   &inline_state[i])) goto fail;
    if (!inline_sampler[i] &&
        !find_inline_sampler_state(ctx->common_params, i,
                                   &inline_sampler[i],
                                   &inline_state[i])) goto fail;
    if (inline_sampler[i]) {
      if (!allocate_inline_sampler_identifier(inline_state[i], i,
                                              &ctx->sampler_names[i])) {
        goto fail;
      }
      continue;
    }
    if (!resolve_texture_sampler_name(ctx->params, i,
                                     &ctx->sampler_names[i])) goto fail;
    if (!ctx->sampler_names[i] &&
        !resolve_texture_sampler_name(ctx->common_params, i,
                                      &ctx->sampler_names[i])) goto fail;
  }

  /* The state word's comparison bit and the DXBC dcl_sampler mode describe
   * the same binding.  A mismatch cannot be repaired by choosing a different
   * spelling, so reject it before emitting HLSL. */
  for (int sampler = 0; sampler < ctx->program->sampler_count; ++sampler) {
    int reg = ctx->program->samplers[sampler].reg_idx;
    if (reg < 0 || reg >= HLSL_SM5_SAMPLER_REGISTER_COUNT ||
        ctx->program->samplers[sampler].mode > 1u) goto fail;
    if (inline_sampler[reg] &&
        (((inline_state[reg] & 0x100u) != 0u) !=
         (ctx->program->samplers[sampler].mode == 1u))) goto fail;
  }

  for (int i = 0; i < ctx->program->instruction_count; i++) {
    const USILInstruction *inst = &ctx->program->instructions[i];
    if (!is_sample_opcode(inst->opcode)) {
      continue;
    }

    int sampler_reg = -1;
    int texture_reg = -1;
    for (int j = 0; j < inst->operand_count; j++) {
      if (inst->operands[j].type == OPERAND_TYPE_SAMPLER) {
        sampler_reg = inst->operands[j].register_index;
      } else if (inst->operands[j].type == OPERAND_TYPE_RESOURCE) {
        texture_reg = inst->operands[j].register_index;
      }
    }
    if (sampler_reg < 0 ||
        sampler_reg >= HLSL_SM5_SAMPLER_REGISTER_COUNT || texture_reg < 0) {
      continue;
    }
    if (inline_sampler[sampler_reg]) continue;
    /* A sampler register may be shared by several texture bindings.  Any
     * paired texture gives Unity the same serialized sampler register; use
     * the lowest binding as the canonical, source-independent spelling. */
    if (paired_texture[sampler_reg] == -1 ||
        texture_reg < paired_texture[sampler_reg]) {
      paired_texture[sampler_reg] = texture_reg;
    }
  }

  for (int sampler_reg = 0;
       sampler_reg < HLSL_SM5_SAMPLER_REGISTER_COUNT; sampler_reg++) {
    if (paired_texture[sampler_reg] < 0) {
      continue;
    }
    const char *texture_name = NULL;
    if (!resolve_srv_name_ctx(ctx, paired_texture[sampler_reg],
                              SERIALIZED_RESOURCE_TEXTURE,
                              &texture_name)) goto fail;
    if (texture_name) {
      char *canonical_name = NULL;
      if (!allocate_sampler_identifier(texture_name, &canonical_name))
        goto fail;
      free(ctx->sampler_names[sampler_reg]);
      ctx->sampler_names[sampler_reg] = canonical_name;
    }
  }

  /* Give every declared sampler an owned spelling so later formatting has no
   * fixed-capacity fallback scratch.  Register bounds are validated before
   * this analysis runs. */
  for (int i = 0; i < ctx->program->sampler_count; ++i) {
    int reg = ctx->program->samplers[i].reg_idx;
    if (reg < 0 || reg >= HLSL_SM5_SAMPLER_REGISTER_COUNT) goto fail;
    if (ctx->sampler_names[reg]) continue;
    int length = snprintf(NULL, 0, "s%d", reg);
    if (length < 0) goto fail;
    ctx->sampler_names[reg] = malloc((size_t)length + 1u);
    if (!ctx->sampler_names[reg]) goto fail;
    int written = snprintf(ctx->sampler_names[reg], (size_t)length + 1u,
                           "s%d", reg);
    if (written != length) goto fail;
  }
  if (!disambiguate_declared_sampler_identifiers(ctx)) goto fail;
  return true;

fail:
  free_sampler_name_map(ctx);
  if (ctx->sb) ctx->sb->failed = true;
  return false;
}

const char *resolve_uav_name(const SerializedProgramParameters *params,
                             int reg_idx) {
  if (!params)
    return NULL;
  for (int i = 0; i < params->res_count; i++) {
    if (params->resources[i].bind_index == (uint32_t)reg_idx &&
        params->resources[i].bind_type == SERIALIZED_RESOURCE_UAV) {
      return params->resources[i].name;
    }
  }
  return NULL;
}

bool is_unity_builtin_cbuffer(const char *name) {
  if (!name)
    return false;
  const char *check = name;
  if (check[0] == '$')
    check++;
  return (strcmp(check, "UnityPerDraw") == 0 ||
          strcmp(check, "UnityPerFrame") == 0 ||
          strcmp(check, "UnityPerCamera") == 0 ||
          strcmp(check, "UnityPerCameraRare") == 0 ||
          strcmp(check, "UnityLighting") == 0 ||
          strcmp(check, "UnityLightingOld") == 0 ||
          strcmp(check, "UnityShadows") == 0 ||
          strcmp(check, "UnityStereoEyeIndex") == 0 ||
          strcmp(check, "UnityStereoGlobals") == 0 ||
          strcmp(check, "UnityPerDrawRare") == 0 ||
          strcmp(check, "UnityFog") == 0 ||
          strcmp(check, "UnityLightmaps") == 0 ||
          strcmp(check, "UnityReflectionProbes") == 0 ||
          strcmp(check, "UnityProbeVolume") == 0);
}
// Check if a variable is a known Unity builtin that belongs to a dedicated
// cbuffer (not $Globals). Uses ONLY common_params (variant-specific TypeTree data)
// to check if the variable exists in a non-$Globals cbuffer for THIS shader.
// This is authoritative: if common_params says unity_WorldToLight is in UnityLighting,
// then Unity includes will provide it and we must NOT redeclare it in $Globals.
// The g_builtins table is NOT used here — it's too broad and would incorrectly
// filter variables that legitimately live in $Globals for some shader variants.
bool is_unity_builtin_variable_ex(const char *var_name,
                                   const SerializedProgramParameters *common_params) {
  if (!var_name) return false;
  // Check common_params: if this variable exists in any non-$Globals cbuffer,
  // it's a Unity builtin provided by includes (e.g. unity_WorldToLight in UnityLighting)
  if (common_params) {
    for (int ci = 0; ci < common_params->cb_count; ci++) {
      const char *cp_cb_name = common_params->constant_buffers[ci].name;
      // Skip $Globals — we're checking if it exists in OTHER cbuffers
      if (!cp_cb_name || strcmp(cp_cb_name, "$Globals") == 0) continue;
      const SerializedConstantBuffer *ccb = &common_params->constant_buffers[ci];
      for (int v = 0; v < ccb->var_count; v++) {
        if (ccb->variables[v].name &&
            strcmp(ccb->variables[v].name, var_name) == 0) {
          return true;
        }
      }
    }
  }
  return false;
}


const char *
resolve_builtin_cb_name_for_reg(const SerializedProgramParameters *params,
                                int cb_reg) {
  const char *cb_name = get_cbuffer_name(params, cb_reg);
  if (!cb_name)
    return NULL;
  if (is_unity_builtin_cbuffer(cb_name))
    return cb_name;
  return NULL;
}
