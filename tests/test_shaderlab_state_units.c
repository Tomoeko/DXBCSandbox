#include "translation/shaderlab_emitter_internal.h"
#include "fixtures/unity_2021_3_shaderlab_state_reference.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,      \
              #condition);                                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

static void set_value(SerializedShaderFloatValue *value, float number,
                      const char *property) {
  memset(value, 0, sizeof(*value));
  value->present = true;
  value->val = number;
  value->name = property;
}

static void set_native_stencil_defaults(SerializedStencilOp *op) {
  set_value(&op->pass, (float)kUnity2021StateDefaults.stencil_op,
            "<noninit>");
  set_value(&op->fail, (float)kUnity2021StateDefaults.stencil_op,
            "<noninit>");
  set_value(&op->zFail, (float)kUnity2021StateDefaults.stencil_op,
            "<noninit>");
  set_value(&op->comp, (float)kUnity2021StateDefaults.stencil_compare,
            "<noninit>");
}

static void set_native_default_state(SerializedShaderState *state) {
  memset(state, 0, sizeof(*state));
  set_value(&state->zClip, (float)kUnity2021StateDefaults.z_clip,
            "<noninit>");
  set_value(&state->zTest, (float)kUnity2021StateDefaults.z_test,
            "<noninit>");
  set_value(&state->zWrite, (float)kUnity2021StateDefaults.z_write,
            "<noninit>");
  set_value(&state->culling, (float)kUnity2021StateDefaults.cull,
            "<noninit>");
  set_value(&state->conservative,
            (float)kUnity2021StateDefaults.conservative, "<noninit>");
  set_value(&state->offsetFactor, 0.0f, "<noninit>");
  set_value(&state->offsetUnits, 0.0f, "<noninit>");
  set_value(&state->alphaToMask,
            (float)kUnity2021StateDefaults.alpha_to_mask, "<noninit>");
  for (int target = 0; target < 8; ++target) {
    SerializedShaderRTBlendState *rt = &state->rtBlend[target];
    set_value(&rt->srcBlend,
              (float)kUnity2021StateDefaults.src_blend, "<noninit>");
    set_value(&rt->destBlend,
              (float)kUnity2021StateDefaults.dst_blend, "<noninit>");
    set_value(&rt->srcBlendAlpha,
              (float)kUnity2021StateDefaults.src_blend, "<noninit>");
    set_value(&rt->destBlendAlpha,
              (float)kUnity2021StateDefaults.dst_blend, "<noninit>");
    set_value(&rt->blendOp,
              (float)kUnity2021StateDefaults.blend_op, "<noninit>");
    set_value(&rt->blendOpAlpha,
              (float)kUnity2021StateDefaults.blend_op, "<noninit>");
    set_value(&rt->colMask,
              (float)kUnity2021StateDefaults.color_mask, "<noninit>");
  }
  set_native_stencil_defaults(&state->stencilOp);
  set_native_stencil_defaults(&state->stencilOpFront);
  set_native_stencil_defaults(&state->stencilOpBack);
  set_value(&state->stencilReadMask,
            (float)kUnity2021StateDefaults.stencil_read_mask, "<noninit>");
  set_value(&state->stencilWriteMask,
            (float)kUnity2021StateDefaults.stencil_write_mask, "<noninit>");
  set_value(&state->stencilRef,
            (float)kUnity2021StateDefaults.stencil_reference, "<noninit>");
  state->fogMode = kUnity2021StateDefaults.fog_mode;
  state->fogColor.name = kUnity2021FogDefaultNames[0];
  set_value(&state->fogColor.x, 0.0f, "<noninit>");
  set_value(&state->fogColor.y, 0.0f, "<noninit>");
  set_value(&state->fogColor.z, 0.0f, "<noninit>");
  set_value(&state->fogColor.w, 0.0f, "<noninit>");
  set_value(&state->fogStart, 0.0f, kUnity2021FogDefaultNames[1]);
  set_value(&state->fogEnd, 0.0f, kUnity2021FogDefaultNames[2]);
  set_value(&state->fogDensity, 0.0f, kUnity2021FogDefaultNames[3]);
  state->lighting = kUnity2021StateDefaults.lighting != 0;
}

static int test_absent_state_emits_nothing(void) {
  SerializedShaderState state;
  memset(&state, 0, sizeof(state));
  StringBuilder output;
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 2));
  CHECK(sb_ok(&output));
  CHECK(output.len == 0u);
  sb_free(&output);
  return 0;
}

static int test_tags_preserve_serialized_spelling(void) {
  SerializedTag tags_storage[] = {
      {"LIGHTMODE", "UNIVERSALFORWARD"},
      {"CustomTag", "UserDefinedValue"},
  };
  SerializedTagMap tags;
  memset(&tags, 0, sizeof(tags));
  tags.tag_count = (int)ARRAY_COUNT(tags_storage);
  tags.tags = tags_storage;

  StringBuilder output;
  sb_init(&output);
  CHECK(emit_tags(&output, &tags, 1));
  CHECK(output.buf != NULL);
  CHECK(strcmp(output.buf,
               "    Tags { \"LIGHTMODE\"=\"UNIVERSALFORWARD\" "
               "\"CustomTag\"=\"UserDefinedValue\" }\n") == 0);
  sb_free(&output);
  return 0;
}

static int test_native_default_state_emits_nothing(void) {
  SerializedShaderState state;
  set_native_default_state(&state);
  StringBuilder output;
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 0));
  CHECK(sb_ok(&output));
  CHECK(output.len == 0u);
  sb_free(&output);
  return 0;
}

static int test_complete_state(void) {
  SerializedShaderState state;
  memset(&state, 0, sizeof(state));
  set_value(&state.culling, 1.0f, NULL);
  set_value(&state.zClip, 0.0f, NULL);
  set_value(&state.zWrite, 0.0f, NULL);
  set_value(&state.zTest, 4.0f, "_ZTest");
  set_value(&state.conservative, 1.0f, NULL);
  set_value(&state.alphaToMask, 1.0f, NULL);
  set_value(&state.offsetFactor, -1.25f, NULL);
  set_value(&state.offsetUnits, 0.0f, "_OffsetUnits");
  state.lighting = true;

  state.rtSeparateBlend = true;
  SerializedShaderRTBlendState *rt = &state.rtBlend[2];
  set_value(&rt->srcBlend, 5.0f, NULL);
  set_value(&rt->destBlend, 10.0f, NULL);
  set_value(&rt->srcBlendAlpha, 1.0f, NULL);
  set_value(&rt->destBlendAlpha, 0.0f, NULL);
  set_value(&rt->blendOp, 2.0f, NULL);
  set_value(&rt->blendOpAlpha, 4.0f, NULL);
  set_value(&rt->colMask, 10.0f, NULL);

  set_value(&state.stencilRef, 7.0f, NULL);
  set_value(&state.stencilReadMask, 127.0f, NULL);
  set_value(&state.stencilWriteMask, 255.0f, "_StencilWriteMask");
  set_value(&state.stencilOp.comp, 3.0f, NULL);
  set_value(&state.stencilOp.pass, 2.0f, NULL);
  set_value(&state.stencilOpFront.comp, 5.0f, NULL);

  static const char expected[] =
      "        Lighting On\n"
      "        AlphaToMask On\n"
      "        ZTest [_ZTest]\n"
      "        ZWrite Off\n"
      "        Cull Front\n"
      "        Stencil\n"
      "        {\n"
      "            Ref 7\n"
      "            ReadMask 127\n"
      "            WriteMask [_StencilWriteMask]\n"
      "            Comp Equal\n"
      "            Pass Replace\n"
      "            CompFront Greater\n"
      "        }\n"
      "        Blend 2 SrcAlpha OneMinusSrcAlpha, One Zero\n"
      "        BlendOp 2 RevSub, Max\n"
      "        ColorMask RB 2\n"
      "        Offset -1.25, [_OffsetUnits]\n"
      "        ZClip False\n"
      "        Conservative True\n";

  StringBuilder output;
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 2));
  CHECK(sb_ok(&output));
  CHECK(output.buf != NULL);
  CHECK(strcmp(output.buf, expected) == 0);
  const char *ordered = output.buf;
  for (size_t index = 0;
       index < ARRAY_COUNT(kUnity2021EmitPassStateOrder); ++index) {
    ordered = strstr(ordered, kUnity2021EmitPassStateOrder[index]);
    CHECK(ordered != NULL);
    ordered += strlen(kUnity2021EmitPassStateOrder[index]);
  }
  sb_free(&output);
  return 0;
}

static int test_native_compare_and_cull_spellings(void) {
  for (size_t index = 0;
       index < ARRAY_COUNT(kUnity2021CompareFunctionSpellings); ++index) {
    const Unity2021ShaderLabEnumSpelling *entry =
        &kUnity2021CompareFunctionSpellings[index];
    SerializedShaderState state;
    set_native_default_state(&state);
    char expected[128];
    if (entry->value == kUnity2021StateDefaults.z_test) {
      set_value(&state.stencilOp.comp, (float)entry->value, "<noninit>");
      CHECK(snprintf(expected, sizeof(expected),
                     "Stencil\n{\n    Comp %s\n}\n", entry->spelling) > 0);
    } else {
      set_value(&state.zTest, (float)entry->value, "<noninit>");
      CHECK(snprintf(expected, sizeof(expected), "ZTest %s\n",
                     entry->spelling) > 0);
    }
    StringBuilder output;
    sb_init(&output);
    CHECK(emit_render_state(&output, &state, 0));
    CHECK(output.buf != NULL && strcmp(output.buf, expected) == 0);
    sb_free(&output);
  }

  for (size_t index = 0; index < ARRAY_COUNT(kUnity2021CullSpellings);
       ++index) {
    const Unity2021ShaderLabEnumSpelling *entry =
        &kUnity2021CullSpellings[index];
    SerializedShaderState state;
    set_native_default_state(&state);
    set_value(&state.culling, (float)entry->value, "<noninit>");
    StringBuilder output;
    sb_init(&output);
    CHECK(emit_render_state(&output, &state, 0));
    if (entry->value == kUnity2021StateDefaults.cull) {
      CHECK(output.len == 0u);
    } else {
      char expected[64];
      CHECK(snprintf(expected, sizeof(expected), "Cull %s\n",
                     entry->spelling) > 0);
      CHECK(output.buf != NULL && strcmp(output.buf, expected) == 0);
    }
    sb_free(&output);
  }
  return 0;
}

static int test_native_blend_and_stencil_spellings(void) {
  for (size_t index = 0; index < ARRAY_COUNT(kUnity2021BlendSpellings);
       ++index) {
    const Unity2021ShaderLabEnumSpelling *entry =
        &kUnity2021BlendSpellings[index];
    SerializedShaderState state;
    set_native_default_state(&state);
    SerializedShaderRTBlendState *rt = &state.rtBlend[0];
    set_value(&rt->srcBlend, (float)entry->value, "<noninit>");
    set_value(&rt->destBlend, (float)entry->value, "<noninit>");
    set_value(&rt->srcBlendAlpha, (float)entry->value, "<noninit>");
    set_value(&rt->destBlendAlpha, (float)entry->value, "<noninit>");
    char expected[128];
    CHECK(snprintf(expected, sizeof(expected), "Blend %s %s\n",
                   entry->spelling, entry->spelling) > 0);
    StringBuilder output;
    sb_init(&output);
    CHECK(emit_render_state(&output, &state, 0));
    CHECK(output.buf != NULL && strcmp(output.buf, expected) == 0);
    sb_free(&output);
  }

  for (size_t index = 0; index < ARRAY_COUNT(kUnity2021BlendOpSpellings);
       ++index) {
    const Unity2021ShaderLabEnumSpelling *entry =
        &kUnity2021BlendOpSpellings[index];
    SerializedShaderState state;
    set_native_default_state(&state);
    set_value(&state.rtBlend[0].blendOp, (float)entry->value, "<noninit>");
    const int alpha_value = entry->value == 0 ? 1 : 0;
    set_value(&state.rtBlend[0].blendOpAlpha, (float)alpha_value,
              "<noninit>");
    const char *alpha_spelling =
        kUnity2021BlendOpSpellings[(size_t)alpha_value].spelling;
    char expected[160];
    CHECK(snprintf(expected, sizeof(expected), "BlendOp %s, %s\n",
                   entry->spelling, alpha_spelling) > 0);
    StringBuilder output;
    sb_init(&output);
    CHECK(emit_render_state(&output, &state, 0));
    CHECK(output.buf != NULL && strcmp(output.buf, expected) == 0);
    sb_free(&output);
  }

  for (size_t index = 0; index < ARRAY_COUNT(kUnity2021StencilOpSpellings);
       ++index) {
    const Unity2021ShaderLabEnumSpelling *entry =
        &kUnity2021StencilOpSpellings[index];
    SerializedShaderState state;
    set_native_default_state(&state);
    set_value(&state.stencilOp.pass, (float)entry->value, "<noninit>");
    StringBuilder output;
    sb_init(&output);
    CHECK(emit_render_state(&output, &state, 0));
    if (entry->value == kUnity2021StateDefaults.stencil_op) {
      CHECK(output.len == 0u);
    } else {
      char expected[96];
      CHECK(snprintf(expected, sizeof(expected),
                     "Stencil\n{\n    Pass %s\n}\n", entry->spelling) > 0);
      CHECK(output.buf != NULL && strcmp(output.buf, expected) == 0);
    }
    sb_free(&output);
  }
  return 0;
}

static int test_native_property_and_bool_variable_behavior(void) {
  SerializedShaderState state;
  set_native_default_state(&state);
  set_value(&state.alphaToMask, -0.25f, "<noninit>");
  set_value(&state.zTest, 12345.0f, "_ZTest");
  StringBuilder output;
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 0));
  CHECK(output.buf != NULL);
  CHECK(strcmp(output.buf,
               "AlphaToMask On\n"
               "ZTest [_ZTest]\n") == 0);
  sb_free(&output);
  return 0;
}

static void set_default_fog(SerializedShaderState *state) {
  state->fogMode = -1;
  state->fogColor.name = "unity_FogColor";
  set_value(&state->fogColor.x, 0.0f, "<noninit>");
  set_value(&state->fogColor.y, 0.0f, "<noninit>");
  set_value(&state->fogColor.z, 0.0f, "<noninit>");
  set_value(&state->fogColor.w, 0.0f, "<noninit>");
  set_value(&state->fogStart, 0.0f, "unity_FogStart");
  set_value(&state->fogEnd, 0.0f, "unity_FogEnd");
  set_value(&state->fogDensity, 0.0f, "unity_FogDensity");
}

static int test_fog_and_lighting_projection(void) {
  SerializedShaderState state;
  StringBuilder output;

  memset(&state, 0, sizeof(state));
  set_default_fog(&state);
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 0));
  CHECK(output.len == 0u);
  sb_free(&output);

  state.fogMode = 0;
  state.lighting = true;
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 1));
  CHECK(strcmp(output.buf,
               "    Lighting On\n"
               "    Fog\n"
               "    {\n"
               "        Mode Off\n"
               "    }\n") == 0);
  sb_free(&output);

  memset(&state, 0, sizeof(state));
  set_default_fog(&state);
  state.fogColor.name = "<noninit>";
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 0));
  CHECK(strcmp(output.buf,
               "Fog\n"
               "{\n"
               "    Color (0,0,0,0)\n"
               "}\n") == 0);
  sb_free(&output);

  state.fogMode = 4;
  sb_init(&output);
  sb_append(&output, "unchanged");
  CHECK(!emit_render_state(&output, &state, 0));
  CHECK(strcmp(output.buf, "unchanged") == 0);
  sb_free(&output);
  return 0;
}

static int test_invalid_values_fail_closed(void) {
  SerializedShaderState state;
  StringBuilder output;

  memset(&state, 0, sizeof(state));
  set_value(&state.rtBlend[0].blendOp, 36.0f, NULL);
  sb_init(&output);
  CHECK(!emit_render_state(&output, &state, 0));
  sb_free(&output);

  memset(&state, 0, sizeof(state));
  set_value(&state.rtBlend[0].colMask, 3.5f, NULL);
  sb_init(&output);
  CHECK(!emit_render_state(&output, &state, 0));
  sb_free(&output);

  memset(&state, 0, sizeof(state));
  set_value(&state.zWrite, 2.0f, NULL);
  sb_init(&output);
  CHECK(!emit_render_state(&output, &state, 0));
  sb_free(&output);

  memset(&state, 0, sizeof(state));
  set_value(&state.stencilRef, 256.0f, NULL);
  sb_init(&output);
  CHECK(!emit_render_state(&output, &state, 0));
  sb_free(&output);
  return 0;
}

static int test_state_emission_is_transactional_and_no_state_is_hidden(void) {
  SerializedShaderState state;
  StringBuilder output;

  memset(&state, 0, sizeof(state));
  set_value(&state.culling, 1.0f, NULL);
  set_value(&state.zWrite, 2.0f, NULL);
  sb_init(&output);
  sb_append(&output, "unchanged");
  CHECK(!emit_render_state(&output, &state, 0));
  CHECK(strcmp(output.buf, "unchanged") == 0);
  sb_free(&output);

  /* 255 and -1 are values, not universal omission sentinels.  Treating them
   * as absent silently changed invalid enum authority into defaults. */
  memset(&state, 0, sizeof(state));
  set_value(&state.zWrite, 255.0f, NULL);
  sb_init(&output);
  sb_append(&output, "unchanged");
  CHECK(!emit_render_state(&output, &state, 0));
  CHECK(strcmp(output.buf, "unchanged") == 0);
  sb_free(&output);

  /* A numeric alpha factor with the same cached float as a color property is
   * still different authority. Omitting the alpha tuple would silently make
   * it inherit the property at ShaderLab compile time. */
  memset(&state, 0, sizeof(state));
  set_value(&state.rtBlend[0].srcBlend, 1.0f, "_SrcBlend");
  set_value(&state.rtBlend[0].srcBlendAlpha, 1.0f, NULL);
  set_value(&state.rtBlend[0].blendOp, 0.0f, "_BlendOp");
  set_value(&state.rtBlend[0].blendOpAlpha, 0.0f, NULL);
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 0));
  CHECK(strstr(output.buf, "Blend [_SrcBlend] Zero, One Zero\n") != NULL);
  CHECK(strstr(output.buf, "BlendOp [_BlendOp], Add\n") != NULL);
  sb_free(&output);

  memset(&state, 0, sizeof(state));
  set_value(&state.culling, 1.0f, "9invalid");
  sb_init(&output);
  CHECK(!emit_render_state(&output, &state, 0));
  CHECK(output.len == 0u);
  sb_free(&output);

  memset(&state, 0, sizeof(state));
  set_value(&state.rtBlend[1].srcBlend, 5.0f, NULL);
  sb_init(&output);
  CHECK(!emit_render_state(&output, &state, 0));
  CHECK(output.len == 0u);
  sb_free(&output);

  state.rtSeparateBlend = true;
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 0));
  CHECK(strstr(output.buf, "Blend 1 SrcAlpha Zero") != NULL);
  sb_free(&output);

  memset(&state, 0, sizeof(state));
  set_value(&state.stencilReadMask, 255.0f, "<noninit>");
  sb_init(&output);
  CHECK(emit_render_state(&output, &state, 0));
  CHECK(output.len == 0u);
  CHECK(!emit_render_state(&output, &state, -1));
  CHECK(output.len == 0u);
  sb_free(&output);
  return 0;
}

static int test_properties_are_unbounded_and_exact(void) {
  SerializedShader shader;
  ParsedShaderProperty properties[4];
  char long_attribute[700];
  char *attributes[1] = {long_attribute};
  memset(&shader, 0, sizeof(shader));
  memset(properties, 0, sizeof(properties));
  shader.name = "Escaped \\\" name";
  shader.property_count = 4;
  shader.properties = properties;

  memset(long_attribute, 'A', sizeof(long_attribute) - 1u);
  long_attribute[sizeof(long_attribute) - 1u] = '\0';
  properties[0].name = "_Color";
  properties[0].description = "quoted \\\" description";
  properties[0].type = 0;
  properties[0].flags = 1u | 64u | 128u | 256u;
  properties[0].attribute_count = 1;
  properties[0].attributes = attributes;
  properties[0].def_value[0] = 1.23456776f;
  properties[0].def_value[1] = -0.0f;
  properties[0].def_value[2] = 0.25f;
  properties[0].def_value[3] = 1.0f;

  properties[1].name = "_Texture";
  properties[1].description = "texture";
  properties[1].type = 4;
  properties[1].def_texture_dim = 2;
  properties[1].def_texture_name = "";

  properties[3].name = "_TextureAny";
  properties[3].description = "any texture";
  properties[3].type = 4;
  properties[3].def_texture_dim = 1;
  properties[3].def_texture_name = "white";

  properties[2].name = "_Count";
  properties[2].description = "count";
  properties[2].type = 5;
  properties[2].def_value[0] = 17.0f;

  StringBuilder output;
  sb_init(&output);
  CHECK(shaderlab_emit_raw(&shader, NULL, NULL, &output));
  CHECK(sb_ok(&output));
  CHECK(strstr(output.buf, "Shader \"Escaped \\\\\\\" name\"") != NULL);
  CHECK(strstr(output.buf, long_attribute) != NULL);
  CHECK(strstr(output.buf,
               "[HideInInspector] [NonModifiableTextureData] "
               "[MainTexture] [MainColor]") != NULL);
  CHECK(strstr(output.buf, "1.23456776,-0,0.25,1") != NULL);
  CHECK(strstr(output.buf, "_Texture (\"texture\", 2D) = \"\" {}") != NULL);
  CHECK(strstr(output.buf,
               "_TextureAny (\"any texture\", Any) = \"white\" {}") !=
        NULL);
  CHECK(strstr(output.buf, "_Count (\"count\", Int) = 17") != NULL);
  sb_free(&output);
  return 0;
}

static int test_invalid_properties_fail_closed(void) {
  SerializedShader shader;
  ParsedShaderProperty property;
  StringBuilder output;
  memset(&shader, 0, sizeof(shader));
  memset(&property, 0, sizeof(property));
  shader.name = "PropertyTest";
  shader.property_count = 1;
  shader.properties = &property;
  property.name = "_Value";
  property.type = 2;

  property.flags = 1u << 31;
  sb_init(&output);
  CHECK(!shaderlab_emit_raw(&shader, NULL, NULL, &output));
  sb_free(&output);

  property.flags = 0;
  property.name = "bad name";
  sb_init(&output);
  CHECK(!shaderlab_emit_raw(&shader, NULL, NULL, &output));
  sb_free(&output);

  property.name = "_Texture";
  property.type = 4;
  property.def_texture_dim = 99;
  sb_init(&output);
  CHECK(!shaderlab_emit_raw(&shader, NULL, NULL, &output));
  sb_free(&output);

  property.name = "_Integer";
  property.description = "integer";
  property.type = 5;
  property.def_value[0] = 17.5f;
  sb_init(&output);
  CHECK(!shaderlab_emit_raw(&shader, NULL, NULL, &output));
  sb_free(&output);

  /* binary32 rounds (float)INT_MAX to 2147483648.  Reject that value before
   * converting it to int, while accepting the largest in-range binary32
   * integer. */
  property.def_value[0] = 2147483648.0f;
  sb_init(&output);
  CHECK(!shaderlab_emit_raw(&shader, NULL, NULL, &output));
  sb_free(&output);

  property.def_value[0] = 2147483520.0f;
  sb_init(&output);
  CHECK(shaderlab_emit_raw(&shader, NULL, NULL, &output));
  CHECK(strstr(output.buf,
               "_Integer (\"integer\", Int) = 2147483520") != NULL);
  sb_free(&output);
  return 0;
}

static int test_pass_lod_is_independent_of_subshader_lod(void) {
  SerializedShader shader;
  SerializedSubShader subshader;
  SerializedPass pass;
  StringBuilder output;
  memset(&shader, 0, sizeof(shader));
  memset(&subshader, 0, sizeof(subshader));
  memset(&pass, 0, sizeof(pass));
  shader.name = "PassLOD";
  shader.subshader_count = 1;
  shader.subshaders = &subshader;
  subshader.lod = 100;
  subshader.pass_count = 1;
  subshader.passes = &pass;
  pass.pass_type = 0;
  pass.state.lod = 200;

  sb_init(&output);
  CHECK(shaderlab_emit_raw(&shader, "", "", &output));
  CHECK(strstr(output.buf, "        LOD 100\n") != NULL);
  CHECK(strstr(output.buf, "            LOD 200\n") != NULL);
  sb_free(&output);
  return 0;
}

static int test_fallback_suppression_projection(void) {
  SerializedShader shader;
  StringBuilder output;
  memset(&shader, 0, sizeof(shader));
  shader.name = "FallbackProjection";
  shader.fallback_name = "";

  sb_init(&output);
  CHECK(shaderlab_emit_raw(&shader, NULL, NULL, &output));
  CHECK(strstr(output.buf, "\n    Fallback ") == NULL);
  sb_free(&output);

  shader.disable_no_subshaders_message = true;
  sb_init(&output);
  CHECK(shaderlab_emit_raw(&shader, NULL, NULL, &output));
  CHECK(strstr(output.buf, "    Fallback Off\n") != NULL);
  sb_free(&output);

  shader.disable_no_subshaders_message = false;
  shader.fallback_name = "Legacy/VertexLit";
  sb_init(&output);
  CHECK(shaderlab_emit_raw(&shader, NULL, NULL, &output));
  CHECK(strstr(output.buf,
               "    Fallback \"Legacy/VertexLit\"\n") != NULL);
  sb_free(&output);

  shader.disable_no_subshaders_message = true;
  sb_init(&output);
  CHECK(shaderlab_emit_raw(&shader, NULL, NULL, &output));
  const char* named = strstr(
      output.buf, "    Fallback \"Legacy/VertexLit\"\n");
  const char* off = strstr(output.buf, "    Fallback Off\n");
  CHECK(named != NULL && off != NULL && named < off);
  sb_free(&output);
  return 0;
}

int main(void) {
  CHECK(test_tags_preserve_serialized_spelling() == 0);
  CHECK(test_absent_state_emits_nothing() == 0);
  CHECK(test_native_default_state_emits_nothing() == 0);
  CHECK(test_complete_state() == 0);
  CHECK(test_native_compare_and_cull_spellings() == 0);
  CHECK(test_native_blend_and_stencil_spellings() == 0);
  CHECK(test_native_property_and_bool_variable_behavior() == 0);
  CHECK(test_fog_and_lighting_projection() == 0);
  CHECK(test_invalid_values_fail_closed() == 0);
  CHECK(test_state_emission_is_transactional_and_no_state_is_hidden() == 0);
  CHECK(test_properties_are_unbounded_and_exact() == 0);
  CHECK(test_invalid_properties_fail_closed() == 0);
  CHECK(test_pass_lod_is_independent_of_subshader_lod() == 0);
  CHECK(test_fallback_suppression_projection() == 0);
  CHECK(g_allocations_count == 0u);
  CHECK(g_allocated_bytes == 0u);
  printf("ShaderLab state unit tests passed.\n");
  return 0;
}
