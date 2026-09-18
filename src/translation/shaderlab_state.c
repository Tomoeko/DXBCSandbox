// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitter.h"
#include "dxbc/dxbc_parser.h"
#include "translation/hlsl_emitter.h"
#include "translation/usil.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "translation/shaderlab_emitter_internal.h"

void append_indent(StringBuilder *sb, int level) {
  for (int i = 0; i < level; i++) {
    sb_append(sb, "    ");
  }
}

static const char *format_cull(float val) {
  int v = (int)val;
  if (v == 0)
    return "Off";
  if (v == 1)
    return "Front";
  if (v == 2)
    return "Back";
  return NULL;
}

static const char *format_zwrite(float val) {
  int v = (int)val;
  if (v == 0)
    return "Off";
  if (v == 1)
    return "On";
  return NULL;
}

static const char *format_ztest(float val) {
  int v = (int)val;
  if (v == 0)
    return "False";
  if (v == 1)
    return "Never";
  if (v == 2)
    return "Less";
  if (v == 3)
    return "Equal";
  if (v == 4)
    return "LEqual";
  if (v == 5)
    return "Greater";
  if (v == 6)
    return "NotEqual";
  if (v == 7)
    return "GEqual";
  if (v == 8)
    return "Always";
  return NULL;
}

static const char *format_blend(float val) {
  int v = (int)val;
  if (v == 0)
    return "Zero";
  if (v == 1)
    return "One";
  if (v == 2)
    return "DstColor";
  if (v == 3)
    return "SrcColor";
  if (v == 4)
    return "OneMinusDstColor";
  if (v == 5)
    return "SrcAlpha";
  if (v == 6)
    return "OneMinusSrcColor";
  if (v == 7)
    return "DstAlpha";
  if (v == 8)
    return "OneMinusDstAlpha";
  if (v == 9)
    return "SrcAlphaSaturate";
  if (v == 10)
    return "OneMinusSrcAlpha";
  return NULL;
}

static const char *format_blend_op(float val) {
  switch ((int)val) {
  case 0: return "Add";
  case 1: return "Sub";
  case 2: return "RevSub";
  case 3: return "Min";
  case 4: return "Max";
  case 5: return "LogicalClear";
  case 6: return "LogicalSet";
  case 7: return "LogicalCopy";
  case 8: return "LogicalCopyInverted";
  case 9: return "LogicalNoop";
  case 10: return "LogicalInvert";
  case 11: return "LogicalAnd";
  case 12: return "LogicalNand";
  case 13: return "LogicalOr";
  case 14: return "LogicalNor";
  case 15: return "LogicalXor";
  case 16: return "LogicalEquivalence";
  case 17: return "LogicalAndReverse";
  case 18: return "LogicalAndInverted";
  case 19: return "LogicalOrReverse";
  case 20: return "LogicalOrInverted";
  case 21: return "Multiply";
  case 22: return "Screen";
  case 23: return "Overlay";
  case 24: return "Darken";
  case 25: return "Lighten";
  case 26: return "ColorDodge";
  case 27: return "ColorBurn";
  case 28: return "HardLight";
  case 29: return "SoftLight";
  case 30: return "Difference";
  case 31: return "Exclusion";
  case 32: return "HSLHue";
  case 33: return "HSLSaturation";
  case 34: return "HSLColor";
  case 35: return "HSLLuminosity";
  default: return NULL;
  }
}

static const char *format_stencil_op(float val) {
  switch ((int)val) {
  case 0: return "Keep";
  case 1: return "Zero";
  case 2: return "Replace";
  case 3: return "IncrSat";
  case 4: return "DecrSat";
  case 5: return "Invert";
  case 6: return "IncrWrap";
  case 7: return "DecrWrap";
  default: return NULL;
  }
}

static const char *format_on_off(float val) {
  return val == 0.0f ? "Off" : "On";
}

static const char *format_true_false(float val) {
  if (val == 0.0f) return "False";
  if (val == 1.0f) return "True";
  return NULL;
}

static bool state_value_has_property(const SerializedShaderFloatValue *value) {
  return value && value->present && value->name && value->name[0] != '\0' &&
         strcmp(value->name, "<noninit>") != 0;
}

static bool state_name_is_noninit(const char *name) {
  return !name || name[0] == '\0' || strcmp(name, "<noninit>") == 0;
}

static bool state_value_is_builtin_default(
    const SerializedShaderFloatValue *value, const char *builtin_name) {
  return value && value->present && value->val == 0.0f && value->name &&
         strcmp(value->name, builtin_name) == 0;
}

static bool state_value_is_plain(const SerializedShaderFloatValue *value) {
  return value && value->present && state_name_is_noninit(value->name) &&
         isfinite(value->val);
}

static bool state_property_name_is_valid(const char *name) {
  if (!name || name[0] == '\0') return false;
  const unsigned char first = (unsigned char)name[0];
  if (!((first >= 'A' && first <= 'Z') ||
        (first >= 'a' && first <= 'z') || first == '_')) {
    return false;
  }
  for (size_t i = 1; name[i] != '\0'; i++) {
    unsigned char c = (unsigned char)name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

static bool state_value_is_unspecified(
    const SerializedShaderFloatValue *value) {
  return !value || !value->present;
}

static bool state_value_is_default(const SerializedShaderFloatValue *value,
                                   float default_value) {
  return state_value_is_unspecified(value) ||
         (!state_value_has_property(value) && value->val == default_value);
}

static bool state_value_is_integral(const SerializedShaderFloatValue *value) {
  return value && isfinite(value->val) &&
         (double)value->val >= (double)INT_MIN &&
         (double)value->val <= (double)INT_MAX &&
         (float)(int)value->val == value->val;
}

static bool state_values_are_semantically_equal(
    const SerializedShaderFloatValue *left,
    const SerializedShaderFloatValue *right) {
  if (!left || !right) return false;
  const bool left_property = state_value_has_property(left);
  const bool right_property = state_value_has_property(right);
  if (left_property || right_property) {
    return left_property && right_property &&
           strcmp(left->name, right->name) == 0;
  }
  return left->val == right->val;
}

static bool append_state_value(
    StringBuilder *sb, const SerializedShaderFloatValue *value,
    const char *(*formatter)(float)) {
  if (!sb || !value || state_value_is_unspecified(value)) return false;
  if (state_value_has_property(value)) {
    if (!state_property_name_is_valid(value->name)) return false;
    sb_append_char(sb, '[');
    sb_append(sb, value->name);
    sb_append_char(sb, ']');
    return sb_ok(sb);
  }
  if (!isfinite(value->val)) return false;
  if (formatter) {
    if (!state_value_is_integral(value)) return false;
    const char *formatted = formatter(value->val);
    if (!formatted) return false;
    sb_append(sb, formatted);
  } else {
    sb_appendf(sb, "%.9g", value->val);
  }
  return sb_ok(sb);
}

static bool emit_state_command(
    StringBuilder *sb, const char *label,
    const SerializedShaderFloatValue *value,
    const char *(*formatter)(float), float default_value, int indent) {
  if (state_value_is_default(value, default_value)) return true;
  append_indent(sb, indent);
  sb_append(sb, label);
  sb_append_char(sb, ' ');
  if (!append_state_value(sb, value, formatter)) return false;
  sb_append_char(sb, '\n');
  return sb_ok(sb);
}

static bool emit_bool_state_command(
    StringBuilder *sb, const char *label,
    const SerializedShaderFloatValue *value, float default_value,
    int indent) {
  if (state_value_is_default(value, default_value)) return true;
  append_indent(sb, indent);
  sb_append(sb, label);
  sb_append_char(sb, ' ');
  if (state_value_has_property(value)) {
    if (!append_state_value(sb, value, NULL)) return false;
  } else {
    if (!value || !value->present || !isfinite(value->val)) return false;
    sb_append(sb, format_on_off(value->val));
  }
  sb_append_char(sb, '\n');
  return sb_ok(sb);
}

static bool emit_blend_target(StringBuilder *sb,
                              const SerializedShaderRTBlendState *rt,
                              int target, bool include_target, int indent) {
  bool blend_default = state_value_is_default(&rt->srcBlend, 1.0f) &&
                       state_value_is_default(&rt->destBlend, 0.0f) &&
                       state_value_is_default(&rt->srcBlendAlpha, 1.0f) &&
                       state_value_is_default(&rt->destBlendAlpha, 0.0f);
  bool blend_op_default = state_value_is_default(&rt->blendOp, 0.0f) &&
                          state_value_is_default(&rt->blendOpAlpha, 0.0f);
  bool color_mask_default = state_value_is_default(&rt->colMask, 15.0f);

  if (!blend_default) {
    append_indent(sb, indent);
    sb_append(sb, "Blend ");
    if (include_target) sb_appendf(sb, "%d ", target);
    SerializedShaderFloatValue one = {1.0f, "", true};
    SerializedShaderFloatValue zero = {0.0f, "", true};
    const SerializedShaderFloatValue *src =
        state_value_is_unspecified(&rt->srcBlend) ? &one : &rt->srcBlend;
    const SerializedShaderFloatValue *dst =
        state_value_is_unspecified(&rt->destBlend) ? &zero : &rt->destBlend;
    const SerializedShaderFloatValue *src_alpha =
        state_value_is_unspecified(&rt->srcBlendAlpha) ? &one
                                                       : &rt->srcBlendAlpha;
    const SerializedShaderFloatValue *dst_alpha =
        state_value_is_unspecified(&rt->destBlendAlpha) ? &zero
                                                        : &rt->destBlendAlpha;
    if (!append_state_value(sb, src, format_blend) ||
        (sb_append_char(sb, ' '),
         !append_state_value(sb, dst, format_blend))) {
      return false;
    }
    bool alpha_differs =
        !state_values_are_semantically_equal(src_alpha, src) ||
        !state_values_are_semantically_equal(dst_alpha, dst);
    if (alpha_differs) {
      sb_append(sb, ", ");
      if (!append_state_value(sb, src_alpha, format_blend) ||
          (sb_append_char(sb, ' '),
           !append_state_value(sb, dst_alpha, format_blend))) {
        return false;
      }
    }
    sb_append_char(sb, '\n');
  }

  if (!blend_op_default) {
    append_indent(sb, indent);
    sb_append(sb, "BlendOp ");
    if (include_target) sb_appendf(sb, "%d ", target);
    SerializedShaderFloatValue add = {0.0f, "", true};
    const SerializedShaderFloatValue *color =
        state_value_is_unspecified(&rt->blendOp) ? &add : &rt->blendOp;
    const SerializedShaderFloatValue *alpha =
        state_value_is_unspecified(&rt->blendOpAlpha) ? &add
                                                      : &rt->blendOpAlpha;
    if (!append_state_value(sb, color, format_blend_op)) return false;
    if (!state_values_are_semantically_equal(alpha, color)) {
      sb_append(sb, ", ");
      if (!append_state_value(sb, alpha, format_blend_op)) return false;
    }
    sb_append_char(sb, '\n');
  }

  if (!color_mask_default) {
    append_indent(sb, indent);
    sb_append(sb, "ColorMask ");
    if (state_value_has_property(&rt->colMask)) {
      if (!append_state_value(sb, &rt->colMask, NULL)) return false;
    } else {
      if (!state_value_is_integral(&rt->colMask) || rt->colMask.val < 0.0f ||
          rt->colMask.val > 15.0f) {
        return false;
      }
      unsigned mask = (unsigned)rt->colMask.val;
      if (mask == 0) {
        sb_append_char(sb, '0');
      } else {
        if (mask & 8u) sb_append_char(sb, 'R');
        if (mask & 4u) sb_append_char(sb, 'G');
        if (mask & 2u) sb_append_char(sb, 'B');
        if (mask & 1u) sb_append_char(sb, 'A');
      }
    }
    if (include_target) sb_appendf(sb, " %d", target);
    sb_append_char(sb, '\n');
  }
  return sb_ok(sb);
}

static bool stencil_op_is_default(const SerializedStencilOp *op) {
  return state_value_is_default(&op->pass, 0.0f) &&
         state_value_is_default(&op->fail, 0.0f) &&
         state_value_is_default(&op->zFail, 0.0f) &&
         state_value_is_default(&op->comp, 8.0f);
}

static bool emit_stencil_value(StringBuilder *sb, const char *label,
                               const SerializedShaderFloatValue *value,
                               const char *(*formatter)(float),
                               float default_value, int indent) {
  if (state_value_is_default(value, default_value)) return true;
  append_indent(sb, indent);
  sb_append(sb, label);
  sb_append_char(sb, ' ');
  if (!append_state_value(sb, value, formatter)) return false;
  sb_append_char(sb, '\n');
  return sb_ok(sb);
}

static bool emit_stencil_op(StringBuilder *sb, const SerializedStencilOp *op,
                            const char *suffix, int indent) {
  char label[32];
  int length = snprintf(label, sizeof(label), "Comp%s", suffix);
  if (length < 0 || (size_t)length >= sizeof(label)) return false;
  if (!emit_stencil_value(sb, label, &op->comp, format_ztest, 8.0f,
                          indent)) return false;
  length = snprintf(label, sizeof(label), "Pass%s", suffix);
  if (length < 0 || (size_t)length >= sizeof(label)) return false;
  if (!emit_stencil_value(sb, label, &op->pass, format_stencil_op, 0.0f,
                          indent)) return false;
  length = snprintf(label, sizeof(label), "Fail%s", suffix);
  if (length < 0 || (size_t)length >= sizeof(label)) return false;
  if (!emit_stencil_value(sb, label, &op->fail, format_stencil_op, 0.0f,
                          indent)) return false;
  length = snprintf(label, sizeof(label), "ZFail%s", suffix);
  if (length < 0 || (size_t)length >= sizeof(label)) return false;
  return emit_stencil_value(sb, label, &op->zFail, format_stencil_op, 0.0f,
                            indent);
}

static bool stencil_scalar_is_valid(
    const SerializedShaderFloatValue *value) {
  return state_value_is_unspecified(value) || state_value_has_property(value) ||
         (state_value_is_integral(value) && value->val >= 0.0f &&
          value->val <= 255.0f);
}

static bool rt_blend_is_default(const SerializedShaderRTBlendState *rt) {
  return rt && state_value_is_default(&rt->srcBlend, 1.0f) &&
         state_value_is_default(&rt->destBlend, 0.0f) &&
         state_value_is_default(&rt->srcBlendAlpha, 1.0f) &&
         state_value_is_default(&rt->destBlendAlpha, 0.0f) &&
         state_value_is_default(&rt->blendOp, 0.0f) &&
         state_value_is_default(&rt->blendOpAlpha, 0.0f) &&
         state_value_is_default(&rt->colMask, 15.0f);
}

static bool emit_stencil(StringBuilder *sb,
                         const SerializedShaderState *state, int indent) {
  const bool stencil_default =
      stencil_op_is_default(&state->stencilOp) &&
      stencil_op_is_default(&state->stencilOpFront) &&
      stencil_op_is_default(&state->stencilOpBack) &&
      state_value_is_default(&state->stencilReadMask, 255.0f) &&
      state_value_is_default(&state->stencilWriteMask, 255.0f) &&
      state_value_is_default(&state->stencilRef, 0.0f);
  if (stencil_default) return true;
  if (!stencil_scalar_is_valid(&state->stencilRef) ||
      !stencil_scalar_is_valid(&state->stencilReadMask) ||
      !stencil_scalar_is_valid(&state->stencilWriteMask)) {
    return false;
  }
  append_indent(sb, indent);
  sb_append(sb, "Stencil\n");
  append_indent(sb, indent);
  sb_append(sb, "{\n");
  if (!emit_stencil_value(sb, "Ref", &state->stencilRef, NULL, 0.0f,
                          indent + 1) ||
      !emit_stencil_value(sb, "ReadMask", &state->stencilReadMask, NULL,
                          255.0f, indent + 1) ||
      !emit_stencil_value(sb, "WriteMask", &state->stencilWriteMask, NULL,
                          255.0f, indent + 1) ||
      !emit_stencil_op(sb, &state->stencilOp, "", indent + 1) ||
      !emit_stencil_op(sb, &state->stencilOpFront, "Front", indent + 1) ||
      !emit_stencil_op(sb, &state->stencilOpBack, "Back", indent + 1)) {
    return false;
  }
  append_indent(sb, indent);
  sb_append(sb, "}\n");
  return sb_ok(sb);
}

static bool emit_offset(StringBuilder *sb,
                        const SerializedShaderState *state, int indent) {
  const bool offset_default =
      state_value_is_default(&state->offsetFactor, 0.0f) &&
      state_value_is_default(&state->offsetUnits, 0.0f);
  if (offset_default) return true;
  const SerializedShaderFloatValue zero = {0.0f, "", true};
  const SerializedShaderFloatValue *factor =
      state_value_is_unspecified(&state->offsetFactor)
          ? &zero : &state->offsetFactor;
  const SerializedShaderFloatValue *units =
      state_value_is_unspecified(&state->offsetUnits)
          ? &zero : &state->offsetUnits;
  append_indent(sb, indent);
  sb_append(sb, "Offset ");
  if (!append_state_value(sb, factor, NULL)) return false;
  sb_append(sb, ", ");
  if (!append_state_value(sb, units, NULL)) return false;
  sb_append_char(sb, '\n');
  return sb_ok(sb);
}

typedef enum {
  FOG_VALUE_INVALID = 0,
  FOG_VALUE_DEFAULT,
  FOG_VALUE_EXPLICIT,
} FogValueKind;

static FogValueKind fog_scalar_kind(
    const SerializedShaderFloatValue *value, const char *builtin_name) {
  if (state_value_is_builtin_default(value, builtin_name)) {
    return FOG_VALUE_DEFAULT;
  }
  if (state_value_is_plain(value)) return FOG_VALUE_EXPLICIT;
  if (value && value->present && state_value_has_property(value) &&
      state_property_name_is_valid(value->name) && isfinite(value->val)) {
    return FOG_VALUE_EXPLICIT;
  }
  return FOG_VALUE_INVALID;
}

static bool fog_color_components_are_plain(
    const SerializedShaderState *state) {
  return state_value_is_plain(&state->fogColor.x) &&
         state_value_is_plain(&state->fogColor.y) &&
         state_value_is_plain(&state->fogColor.z) &&
         state_value_is_plain(&state->fogColor.w);
}

static bool fog_color_components_are_zero(
    const SerializedShaderState *state) {
  return state->fogColor.x.val == 0.0f &&
         state->fogColor.y.val == 0.0f &&
         state->fogColor.z.val == 0.0f &&
         state->fogColor.w.val == 0.0f;
}

static FogValueKind fog_color_kind(const SerializedShaderState *state) {
  if (!state || !fog_color_components_are_plain(state)) {
    return FOG_VALUE_INVALID;
  }
  if (state->fogColor.name &&
      strcmp(state->fogColor.name, "unity_FogColor") == 0) {
    return fog_color_components_are_zero(state) ? FOG_VALUE_DEFAULT
                                                 : FOG_VALUE_INVALID;
  }
  if (state_name_is_noninit(state->fogColor.name)) {
    return FOG_VALUE_EXPLICIT;
  }
  return state_property_name_is_valid(state->fogColor.name) &&
                 fog_color_components_are_zero(state)
             ? FOG_VALUE_EXPLICIT
             : FOG_VALUE_INVALID;
}

static bool fog_authority_is_absent(
    const SerializedShaderState *state) {
  return state && !state->fogColor.x.present &&
         !state->fogColor.y.present && !state->fogColor.z.present &&
         !state->fogColor.w.present && !state->fogStart.present &&
         !state->fogEnd.present && !state->fogDensity.present &&
         state_name_is_noninit(state->fogColor.name);
}

static const char *format_fog_mode(int mode) {
  switch (mode) {
  case 0: return "Off";
  case 1: return "Linear";
  case 2: return "Exp";
  case 3: return "Exp2";
  default: return NULL;
  }
}

static bool emit_fog_color(StringBuilder *sb,
                           const SerializedShaderState *state,
                           int indent) {
  append_indent(sb, indent);
  sb_append(sb, "Color ");
  if (!state_name_is_noninit(state->fogColor.name)) {
    if (!state_property_name_is_valid(state->fogColor.name)) return false;
    sb_append_char(sb, '[');
    sb_append(sb, state->fogColor.name);
    sb_append_char(sb, ']');
  } else {
    sb_append_char(sb, '(');
    if (!append_state_value(sb, &state->fogColor.x, NULL)) return false;
    sb_append_char(sb, ',');
    if (!append_state_value(sb, &state->fogColor.y, NULL)) return false;
    sb_append_char(sb, ',');
    if (!append_state_value(sb, &state->fogColor.z, NULL)) return false;
    sb_append_char(sb, ',');
    if (!append_state_value(sb, &state->fogColor.w, NULL)) return false;
    sb_append_char(sb, ')');
  }
  sb_append_char(sb, '\n');
  return sb_ok(sb);
}

static bool emit_fog(StringBuilder *sb,
                     const SerializedShaderState *state, int indent) {
  /* A zero-initialized synthetic state has no SerializedShader fog
   * authority at all.  Keep this compatibility path distinct from a parsed
   * Fog { Mode Off } value, whose seven scalar/vector fields are present. */
  if (fog_authority_is_absent(state)) return true;
  const FogValueKind color = fog_color_kind(state);
  const FogValueKind start = fog_scalar_kind(&state->fogStart,
                                              "unity_FogStart");
  const FogValueKind end = fog_scalar_kind(&state->fogEnd,
                                            "unity_FogEnd");
  const FogValueKind density = fog_scalar_kind(&state->fogDensity,
                                                "unity_FogDensity");
  if (color == FOG_VALUE_INVALID || start == FOG_VALUE_INVALID ||
      end == FOG_VALUE_INVALID || density == FOG_VALUE_INVALID ||
      state->fogMode < -1 || state->fogMode > 3) {
    return false;
  }

  const bool all_plain_zero = color == FOG_VALUE_EXPLICIT &&
      state_name_is_noninit(state->fogColor.name) &&
      fog_color_components_are_zero(state) &&
      start == FOG_VALUE_EXPLICIT && state->fogStart.val == 0.0f &&
      density == FOG_VALUE_EXPLICIT && state->fogDensity.val == 0.0f &&
      end == FOG_VALUE_EXPLICIT && state->fogEnd.val == 0.0f;
  const bool emit_color = color == FOG_VALUE_EXPLICIT &&
      !(all_plain_zero && state->fogMode >= 0);
  const bool emit_range = start == FOG_VALUE_EXPLICIT ||
                          end == FOG_VALUE_EXPLICIT;
  const bool emit_density = density == FOG_VALUE_EXPLICIT;
  const bool has_fog = state->fogMode >= 0 || emit_color || emit_range ||
                       emit_density;
  if (!has_fog) return true;

  append_indent(sb, indent);
  sb_append(sb, "Fog\n");
  append_indent(sb, indent);
  sb_append(sb, "{\n");
  if (state->fogMode >= 0) {
    const char *mode = format_fog_mode(state->fogMode);
    if (!mode) return false;
    append_indent(sb, indent + 1);
    sb_append(sb, "Mode ");
    sb_append(sb, mode);
    sb_append_char(sb, '\n');
  }
  if (emit_color && !emit_fog_color(sb, state, indent + 1)) return false;
  if (emit_density) {
    append_indent(sb, indent + 1);
    sb_append(sb, "Density ");
    if (!append_state_value(sb, &state->fogDensity, NULL)) return false;
    sb_append_char(sb, '\n');
  }
  if (emit_range) {
    append_indent(sb, indent + 1);
    sb_append(sb, "Range ");
    if (!append_state_value(sb, &state->fogStart, NULL)) return false;
    sb_append(sb, ", ");
    if (!append_state_value(sb, &state->fogEnd, NULL)) return false;
    sb_append_char(sb, '\n');
  }
  append_indent(sb, indent);
  sb_append(sb, "}\n");
  return sb_ok(sb);
}

static bool emit_render_state_into(StringBuilder *sb,
                                   const SerializedShaderState *state,
                                   int indent) {
  /* Unity 2021.3.35f1's EmitPass writes this common subset in exactly this
   * order. Keep the subset contiguous so our canonical source is stable. */
  if (state->lighting) {
    append_indent(sb, indent);
    sb_append(sb, "Lighting On\n");
  }
  if (!emit_bool_state_command(sb, "AlphaToMask", &state->alphaToMask,
                               0.0f, indent) ||
      !emit_state_command(sb, "ZTest", &state->zTest, format_ztest, 4.0f,
                          indent) ||
      !emit_state_command(sb, "ZWrite", &state->zWrite, format_zwrite, 1.0f,
                          indent) ||
      !emit_state_command(sb, "Cull", &state->culling, format_cull, 2.0f,
                          indent) ||
      !emit_stencil(sb, state, indent)) {
    return false;
  }

  if (!state->rtSeparateBlend) {
    for (int target = 1; target < 8; ++target) {
      if (!rt_blend_is_default(&state->rtBlend[target])) return false;
    }
  }
  int render_target_count = state->rtSeparateBlend ? 8 : 1;
  for (int i = 0; i < render_target_count; i++) {
    if (!emit_blend_target(sb, &state->rtBlend[i], i,
                           state->rtSeparateBlend, indent)) return false;
  }
  if (!emit_offset(sb, state, indent)) return false;

  /* EmitPass does not project these SerializedShaderState fields. They stay
   * outside the native-observed subset rather than perturbing its order. */
  if (!emit_state_command(sb, "ZClip", &state->zClip, format_true_false,
                          1.0f, indent) ||
      !emit_state_command(sb, "Conservative", &state->conservative,
                          format_true_false, 0.0f, indent) ||
      !emit_fog(sb, state, indent)) {
    return false;
  }
  return sb_ok(sb);
}

bool emit_render_state(StringBuilder *sb, const SerializedShaderState *state,
                       int indent) {
  if (!sb || !state || indent < 0 || !sb_ok(sb)) return false;
  StringBuilder generated;
  sb_init_with_capacity(&generated, 512);
  const bool generated_ok =
      emit_render_state_into(&generated, state, indent) && sb_ok(&generated);
  if (generated_ok) sb_append_len(sb, generated.buf, generated.len);
  const bool success = generated_ok && sb_ok(sb);
  sb_free(&generated);
  return success;
}

bool append_shaderlab_quoted(StringBuilder *sb, const char *value) {
  if (!sb || !value) return false;
  sb_append_char(sb, '"');
  for (const unsigned char *cursor = (const unsigned char *)value; *cursor;
       cursor++) {
    switch (*cursor) {
    case '"': sb_append(sb, "\\\""); break;
    case '\\': sb_append(sb, "\\\\"); break;
    case '\n': sb_append(sb, "\\n"); break;
    case '\r': sb_append(sb, "\\r"); break;
    case '\t': sb_append(sb, "\\t"); break;
    default:
      if (*cursor < 0x20u || *cursor == 0x7fu) return false;
      sb_append_char(sb, (char)*cursor);
      break;
    }
  }
  sb_append_char(sb, '"');
  return sb_ok(sb);
}

bool emit_tags(StringBuilder *sb, const SerializedTagMap *tags,
               int indent) {
  if (!sb || !tags || tags->tag_count < 0 ||
      (tags->tag_count > 0 && !tags->tags)) {
    return false;
  }
  if (tags->tag_count == 0)
    return true;
  append_indent(sb, indent);
  sb_append(sb, "Tags { ");
  for (int i = 0; i < tags->tag_count; i++) {
    /* Preserve the serialized spelling exactly. Unity 2021.3's
     * shadertag::GetShaderTagID stores keys and values in a map whose
     * comparator is StrICmp, so spelling is not runtime-selection authority.
     * A spelling whitelist would discard evidence and inevitably miss
     * user-defined LightMode values. */
    if (!append_shaderlab_quoted(sb, tags->tags[i].key)) return false;
    sb_append_char(sb, '=');
    if (!append_shaderlab_quoted(sb, tags->tags[i].value)) return false;
    sb_append_char(sb, ' ');
  }
  sb_append(sb, "}\n");
  return sb_ok(sb);
}

const char *shaderlab_target_status_name(ShaderLabTargetStatus status) {
  switch (status) {
  case SHADERLAB_TARGET_OK: return "ok";
  case SHADERLAB_TARGET_INVALID_ARGUMENT: return "invalid-argument";
  case SHADERLAB_TARGET_MISSING_PLATFORM_AUTHORITY:
    return "missing-platform-authority";
  case SHADERLAB_TARGET_PLATFORM_CONFLICT: return "platform-conflict";
  case SHADERLAB_TARGET_NO_D3D_VARIANTS: return "no-d3d-variants";
  case SHADERLAB_TARGET_INVALID_BLOB: return "invalid-blob";
  case SHADERLAB_TARGET_VARIANT_PARSE_FAILED: return "variant-parse-failed";
  case SHADERLAB_TARGET_VARIANT_METADATA_MISMATCH:
    return "variant-metadata-mismatch";
  case SHADERLAB_TARGET_DXBC_DOCUMENT_FAILED: return "dxbc-document-failed";
  case SHADERLAB_TARGET_STAGE_CONTRACT_FAILED:
    return "stage-contract-failed";
  case SHADERLAB_TARGET_STAGE_TUPLE_CONFLICT:
    return "stage-tuple-conflict";
  case SHADERLAB_TARGET_REQUIREMENT_MODEL_CONFLICT:
    return "requirement-model-conflict";
  case SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS:
    return "unrepresentable-requirements";
  }
  return "unknown";
}

const char *shaderlab_target_version_name(ShaderLabTargetVersion version) {
  static const char *const names[] = {
      "2.0", "2.5", "3.0", "3.5", "4.0", "4.5", "4.6", "5.0"};
  return (unsigned)version < sizeof(names) / sizeof(names[0])
             ? names[(unsigned)version]
             : NULL;
}

ShaderLabTargetVersion shaderlab_target_from_requirements(
    uint64_t requirements) {
  /* This is Unity 2021.3's
   * GetApproximateShaderTargetLevelFromRequirementsMask.  The exact masks
   * are produced by GetShaderRequirementsFromPragmaTargetString; the later
   * single-feature elevations are intentional for inexact masks. */
  int target = (requirements & UINT64_C(1)) != 0 ? 0 : -1;
  if ((~requirements & UINT64_C(0x21)) == 0) target = 1;
  if ((~requirements & UINT64_C(0xe3)) == 0) target = 2;
  if ((~requirements & UINT64_C(0xfeb)) == 0) target = 3;
  if ((~requirements & UINT64_C(0x1feb)) == 0) target = 4;
  if ((~requirements & UINT64_C(0x13dfeb)) == 0) target = 7;
  if (target <= 3 &&
      (~requirements & UINT64_C(0x10cfeb)) == 0) target = 5;
  if (target <= 6 &&
      (~requirements & UINT64_C(0x131feb)) == 0) target = 6;
  if ((~requirements & UINT64_C(0x24000)) == 0) target = 7;
  if (target < 2 && (requirements & UINT64_C(0x8)) != 0) target = 2;
  if (target < 3 && (requirements & UINT64_C(0xc00)) != 0) target = 3;
  if (target < 4 && (requirements & UINT64_C(0x1000)) != 0) target = 4;
  if (target < 5 &&
      (requirements & UINT64_C(0x104000)) != 0) target = 5;
  if (target < 6 && (requirements & UINT64_C(0x20000)) != 0) target = 6;
  if ((requirements & UINT64_C(0x40000)) != 0) target = 7;
  if (target < 0) target = 2;
  return (ShaderLabTargetVersion)target;
}

static uint64_t target_implied_requirements(ShaderLabTargetVersion target) {
  switch (target) {
  case SHADERLAB_TARGET_2_0: return UINT64_C(0x1);
  case SHADERLAB_TARGET_2_5: return UINT64_C(0x21);
  case SHADERLAB_TARGET_3_0: return UINT64_C(0xe3);
  case SHADERLAB_TARGET_3_5: return UINT64_C(0xfeb);
  case SHADERLAB_TARGET_4_0: return UINT64_C(0x1feb);
  case SHADERLAB_TARGET_4_5: return UINT64_C(0x10cfeb);
  case SHADERLAB_TARGET_4_6: return UINT64_C(0x131feb);
  case SHADERLAB_TARGET_5_0: return UINT64_C(0x13dfeb);
  }
  return UINT64_MAX;
}

static bool source_target_from_requirements(
    uint64_t requirements, ShaderLabTargetVersion *out_target) {
  if (!out_target) return false;
  /* The approximate runtime helper intentionally elevates a mask for any
   * single high-end feature.  ShaderLab source inversion has a stricter
   * requirement: #pragma target must not imply bits absent from the
   * serialized mask.  Pick the highest exact subset and spell the remaining
   * expressible features with #pragma require. */
  for (int target = SHADERLAB_TARGET_5_0;
       target >= SHADERLAB_TARGET_2_0; --target) {
    const uint64_t implied =
        target_implied_requirements((ShaderLabTargetVersion)target);
    if ((implied & ~requirements) == 0U) {
      *out_target = (ShaderLabTargetVersion)target;
      return true;
    }
  }
  return false;
}

typedef struct {
  uint64_t bit;
  const char *name;
} ShaderRequirementFeature;

static const ShaderRequirementFeature k_shader_requirement_features[] = {
    {UINT64_C(0x2), "interpolators10"},
    {UINT64_C(0x4), "interpolators32"},
    {UINT64_C(0x8), "mrt4"},
    {UINT64_C(0x10), "mrt8"},
    {UINT64_C(0x20), "derivatives"},
    {UINT64_C(0x40), "samplelod"},
    {UINT64_C(0x80), "fragcoord"},
    /* 0x100 (FragClipDepth) has no Unity 2021.3 #pragma require token. */
    {UINT64_C(0x200), "interpolators15"},
    {UINT64_C(0x400), "2darray"},
    {UINT64_C(0x800), "instancing"},
    {UINT64_C(0x1000), "geometry"},
    {UINT64_C(0x2000), "cubearray"},
    {UINT64_C(0x4000), "compute"},
    {UINT64_C(0x8000), "randomwrite"},
    {UINT64_C(0x10000), "tesshw"},
    {UINT64_C(0x20000), "tessellation"},
    {UINT64_C(0x40000), "sparsetex"},
    {UINT64_C(0x80000), "framebufferfetch"},
    {UINT64_C(0x100000), "msaatex"},
    {UINT64_C(0x200000), "setrtarrayindexfromanyshader"},
};

static uint64_t source_expressible_requirement_bits(void) {
  uint64_t bits = 0;
  for (size_t i = 0;
       i < sizeof(k_shader_requirement_features) /
               sizeof(k_shader_requirement_features[0]);
       ++i) {
    bits |= k_shader_requirement_features[i].bit;
  }
  return bits;
}

bool shaderlab_requirements_emit_pragmas(
    StringBuilder *output, ShaderLabTargetVersion target,
    uint64_t shader_requirements, int indent) {
  if (!output || !sb_ok(output) || indent < 0) return false;
  const uint64_t implied = target_implied_requirements(target);
  if (implied == UINT64_MAX) return false;
  if ((implied & ~shader_requirements) != 0U) return false;
  const uint64_t delta = shader_requirements & ~implied;
  if ((delta & ~source_expressible_requirement_bits()) != 0) return false;
  if (delta == 0) return true;

  StringBuilder generated;
  sb_init(&generated);
  append_indent(&generated, indent);
  sb_append(&generated, "#pragma require");
  for (size_t i = 0;
       i < sizeof(k_shader_requirement_features) /
               sizeof(k_shader_requirement_features[0]);
       ++i) {
    if ((delta & k_shader_requirement_features[i].bit) == 0) continue;
    sb_append_char(&generated, ' ');
    sb_append(&generated, k_shader_requirement_features[i].name);
  }
  sb_append_char(&generated, '\n');
  const bool generated_ok = sb_ok(&generated);
  if (generated_ok) sb_append_len(output, generated.buf, generated.len);
  const bool success = generated_ok && sb_ok(output);
  sb_free(&generated);
  return success;
}

static void set_target_diagnostic(ShaderLabTargetDiagnostic *diagnostic,
                                  ShaderLabTargetStatus status, int stage,
                                  int subprogram, uint64_t requirements,
                                  uint8_t major, uint8_t minor) {
  if (!diagnostic) return;
  memset(diagnostic, 0, sizeof(*diagnostic));
  diagnostic->status = status;
  diagnostic->stage_index = stage;
  diagnostic->subprogram_index = subprogram;
  diagnostic->shader_requirements = requirements;
  diagnostic->shader_model_major = major;
  diagnostic->shader_model_minor = minor;
  diagnostic->document_status = DXBC_DOCUMENT_OK;
  diagnostic->stage_contract_status = DXBC_STAGE_CONTRACT_OK;
  diagnostic->stage_tuple_status = SHADER_STAGE_TUPLE_OK;
}

static bool target_blob_view(const BlobEntry *entries, int entry_count,
                             uint8_t **segments,
                             const int *segment_lengths, int segment_count,
                             int index, const uint8_t **out_payload,
                             size_t *out_length) {
  if (out_payload) *out_payload = NULL;
  if (out_length) *out_length = 0;
  if (!entries || !segments || !segment_lengths || !out_payload ||
      !out_length || entry_count <= 0 || segment_count <= 0 || index < 0 ||
      index >= entry_count) {
    return false;
  }
  const BlobEntry *entry = &entries[index];
  if (entry->offset < 0 || entry->length <= 0 || entry->segment < 0 ||
      entry->segment >= segment_count || !segments[entry->segment] ||
      segment_lengths[entry->segment] < 0) {
    return false;
  }
  const size_t segment_length = (size_t)segment_lengths[entry->segment];
  const size_t offset = (size_t)entry->offset;
  const size_t length = (size_t)entry->length;
  if (offset > segment_length || length > segment_length - offset) {
    return false;
  }
  *out_payload = segments[entry->segment] + offset;
  *out_length = length;
  return true;
}

ShaderLabTargetStatus shaderlab_pass_target_resolve(
    const SerializedPass *pass, const BlobEntry *blob_entries,
    int entry_count, uint8_t **segments, const int *segment_lengths,
    int segment_count, ShaderLabPassTarget *out_target,
    ShaderLabTargetDiagnostic *diagnostic) {
  ShaderLabPassTarget resolved;
  memset(&resolved, 0, sizeof(resolved));
  set_target_diagnostic(diagnostic, SHADERLAB_TARGET_INVALID_ARGUMENT, -1,
                        -1, 0, 0, 0);
  if (out_target) memset(out_target, 0, sizeof(*out_target));
  if (!pass || !out_target || entry_count < 0 || segment_count < 0 ||
      (entry_count > 0 && !blob_entries) ||
      (segment_count > 0 && (!segments || !segment_lengths))) {
    return SHADERLAB_TARGET_INVALID_ARGUMENT;
  }
  if (!pass->has_serialized_platforms || pass->platform_count <= 0 ||
      !pass->platforms) {
    set_target_diagnostic(diagnostic,
                          SHADERLAB_TARGET_MISSING_PLATFORM_AUTHORITY, -1,
                          -1, 0, 0, 0);
    return SHADERLAB_TARGET_MISSING_PLATFORM_AUTHORITY;
  }
  int d3d_platform_count = 0;
  for (int platform_index = 0; platform_index < pass->platform_count;
       ++platform_index) {
    if (pass->platforms[platform_index] == 4) ++d3d_platform_count;
  }
  if (d3d_platform_count == 0) {
    set_target_diagnostic(diagnostic,
                          SHADERLAB_TARGET_MISSING_PLATFORM_AUTHORITY, -1,
                          -1, 0, 0, 0);
    return SHADERLAB_TARGET_MISSING_PLATFORM_AUTHORITY;
  }
  if (d3d_platform_count != 1) {
    set_target_diagnostic(diagnostic, SHADERLAB_TARGET_PLATFORM_CONFLICT,
                          -1, -1, 0, 0, 0);
    return SHADERLAB_TARGET_PLATFORM_CONFLICT;
  }

  bool found_target = false;
  for (int stage = 0; stage < UNITY_SERIALIZED_STAGE_COUNT; ++stage) {
    if (pass->subprogram_count[stage] < 0 ||
        (pass->subprogram_count[stage] > 0 && !pass->subprograms[stage])) {
      set_target_diagnostic(diagnostic, SHADERLAB_TARGET_INVALID_ARGUMENT,
                            stage, -1, 0, 0, 0);
      return SHADERLAB_TARGET_INVALID_ARGUMENT;
    }
    for (int subprogram_index = 0;
         subprogram_index < pass->subprogram_count[stage];
         ++subprogram_index) {
      if (!serialized_pass_subprogram_is_platform(
              pass, stage, subprogram_index, 4)) {
        continue;
      }
      const SerializedSubProgram *serialized_subprogram =
          &pass->subprograms[stage][subprogram_index];
      const uint64_t requirements =
          serialized_subprogram->shader_requirements;
      if (resolved.d3d_variant_count == 0U) {
        resolved.common_requirements = requirements;
      } else {
        resolved.common_requirements &= requirements;
      }
      resolved.union_requirements |= requirements;
      const uint8_t *payload = NULL;
      size_t payload_length = 0;
      if (!target_blob_view(blob_entries, entry_count, segments,
                            segment_lengths, segment_count,
                            serialized_subprogram->blob_index, &payload,
                            &payload_length)) {
        set_target_diagnostic(diagnostic, SHADERLAB_TARGET_INVALID_BLOB,
                              stage, subprogram_index, requirements, 0, 0);
        return SHADERLAB_TARGET_INVALID_BLOB;
      }

      ByteStream stream;
      stream_init(&stream, payload, payload_length);
      stream_set_endian(&stream, false);
      PlayerSubProgramMetadata binary_subprogram;
      memset(&binary_subprogram, 0, sizeof(binary_subprogram));
      if (!subprogram_metadata_parse_variant(&stream, &binary_subprogram)) {
        set_target_diagnostic(diagnostic,
                              SHADERLAB_TARGET_VARIANT_PARSE_FAILED, stage,
                              subprogram_index, requirements, 0, 0);
        return SHADERLAB_TARGET_VARIANT_PARSE_FAILED;
      }
      if (binary_subprogram.program_type !=
          serialized_subprogram->program_type) {
        subprogram_metadata_free_variant(&binary_subprogram);
        set_target_diagnostic(
            diagnostic, SHADERLAB_TARGET_VARIANT_METADATA_MISMATCH, stage,
            subprogram_index, requirements, 0, 0);
        return SHADERLAB_TARGET_VARIANT_METADATA_MISMATCH;
      }

      DXBCContainerView raw_view;
      DXBCDocument document;
      DXBCDocumentDiagnostic document_diagnostic;
      DXBCStageContract contract;
      DXBCStageContractDiagnostic contract_diagnostic;
      dxbc_document_init(&document);
      dxbc_stage_contract_init(&contract);
      memset(&document_diagnostic, 0, sizeof(document_diagnostic));
      memset(&contract_diagnostic, 0, sizeof(contract_diagnostic));
      ShaderLabTargetStatus failure = SHADERLAB_TARGET_OK;
      ShaderStageTupleStatus tuple_status = SHADER_STAGE_TUPLE_OK;

      if (!binary_subprogram.bytecode ||
          binary_subprogram.bytecode_length == 0u ||
          !dxbc_container_view_first(binary_subprogram.bytecode,
                                     binary_subprogram.bytecode_length,
                                     &raw_view)) {
        failure = SHADERLAB_TARGET_DXBC_DOCUMENT_FAILED;
        document_diagnostic.code = DXBC_DOCUMENT_NOT_DXBC;
      } else if (binary_subprogram.bytecode_length >= 4u &&
                 memcmp(binary_subprogram.bytecode, "DXBC", 4u) == 0 &&
                 raw_view.size != binary_subprogram.bytecode_length) {
        failure = SHADERLAB_TARGET_DXBC_DOCUMENT_FAILED;
        document_diagnostic.code = DXBC_DOCUMENT_DECLARED_SIZE_MISMATCH;
      } else if (!dxbc_document_parse(&document, raw_view.data, raw_view.size,
                                      &document_diagnostic)) {
        failure = SHADERLAB_TARGET_DXBC_DOCUMENT_FAILED;
      } else if (!dxbc_stage_contract_decode_document(
                     &document, &contract, &contract_diagnostic)) {
        failure = SHADERLAB_TARGET_STAGE_CONTRACT_FAILED;
      } else {
        UnityCompilerProgramStage compiler_stage;
        ShaderStageTuple tuple;
        memset(&tuple, 0, sizeof(tuple));
        tuple.serialized_stage = (UnitySerializedProgramStage)stage;
        if (!shader_stage_serialized_to_compiler(tuple.serialized_stage,
                                                 &compiler_stage)) {
          tuple_status = SHADER_STAGE_TUPLE_INVALID_SERIALIZED_STAGE;
        } else {
          tuple.compiler_program = compiler_stage;
          tuple.serialized_program_mask = pass->program_mask;
          tuple.gpu_program_type =
              (UnityGPUProgramType)binary_subprogram.program_type;
          tuple.dxbc_program_type = contract.program_type;
          tuple.shader_model_major = contract.shader_model_major;
          tuple.shader_model_minor = contract.shader_model_minor;
          tuple_status = shader_stage_validate_d3d11_tuple(&tuple);
        }
        if (tuple_status != SHADER_STAGE_TUPLE_OK) {
          failure = SHADERLAB_TARGET_STAGE_TUPLE_CONFLICT;
        }
      }

      const uint8_t major = contract.shader_model_major;
      const uint8_t minor = contract.shader_model_minor;
      const ShaderLabTargetVersion requirement_target =
          shaderlab_target_from_requirements(requirements);
      if (failure == SHADERLAB_TARGET_OK &&
          requirement_target > SHADERLAB_TARGET_4_0 && major < 5u) {
        /* A higher-model backend may legally carry a low-requirement
         * variant (for example, Unity's corpus contains SM5 DXBC with a
         * target-3.x requirements mask).  The inverse is impossible: an
         * SM4 program cannot satisfy a requirements mask whose approximate
         * target is 4.5 or newer. */
        failure = SHADERLAB_TARGET_REQUIREMENT_MODEL_CONFLICT;
      }

      if (failure != SHADERLAB_TARGET_OK) {
        dxbc_stage_contract_free(&contract);
        dxbc_document_free(&document);
        subprogram_metadata_free_variant(&binary_subprogram);
        set_target_diagnostic(diagnostic, failure, stage, subprogram_index,
                              requirements, major, minor);
        if (diagnostic) {
          diagnostic->document_status = document_diagnostic.code;
          diagnostic->stage_contract_status = contract_diagnostic.status;
          diagnostic->stage_tuple_status = tuple_status;
        }
        return failure;
      }

      found_target = true;
      if (resolved.d3d_variant_count == SIZE_MAX) {
        dxbc_stage_contract_free(&contract);
        dxbc_document_free(&document);
        subprogram_metadata_free_variant(&binary_subprogram);
        return SHADERLAB_TARGET_INVALID_ARGUMENT;
      }
      ++resolved.d3d_variant_count;
      dxbc_stage_contract_free(&contract);
      dxbc_document_free(&document);
      subprogram_metadata_free_variant(&binary_subprogram);
    }
  }

  if (!found_target) {
    set_target_diagnostic(diagnostic, SHADERLAB_TARGET_NO_D3D_VARIANTS, -1,
                          -1, 0, 0, 0);
    return SHADERLAB_TARGET_NO_D3D_VARIANTS;
  }
  if (!source_target_from_requirements(
          resolved.common_requirements, &resolved.version)) {
    set_target_diagnostic(
        diagnostic, SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS,
        -1, -1, resolved.common_requirements, 0, 0);
    return SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS;
  }
  {
    StringBuilder requirement_probe;
    sb_init(&requirement_probe);
    const bool representable = shaderlab_requirements_emit_pragmas(
        &requirement_probe, resolved.version,
        resolved.common_requirements, 0);
    sb_free(&requirement_probe);
    if (!representable) {
      set_target_diagnostic(
          diagnostic, SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS,
          -1, -1, resolved.common_requirements, 0, 0);
      return SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS;
    }
  }
  *out_target = resolved;
  set_target_diagnostic(diagnostic, SHADERLAB_TARGET_OK, -1, -1,
                        resolved.common_requirements, 0, 0);
  return SHADERLAB_TARGET_OK;
}
