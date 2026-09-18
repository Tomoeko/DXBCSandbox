#ifndef UNITY_2021_3_SHADERLAB_STATE_REFERENCE_H
#define UNITY_2021_3_SHADERLAB_STATE_REFERENCE_H

/*
 * Portable read-only observations from the macOS arm64 Unity 2021.3.35f1
 * Editor (157b46ce122a). These values were recovered from:
 *
 *   CreateDefaultShaderState                              0x101a77290
 *   EmitPass                                             0x101a82aa4
 *   EmitBoolVariable                                     0x101a86544
 *   kCmpFuncNames                                        0x103c103f0
 *   kCullNames                                           0x103c10438
 *   kStencilOpNames                                      0x103c10450
 *   kBlendNames                                          0x103c10490
 *   kBlendOpNames                                        0x103c104e8
 *
 * This is evidence for deterministic formatting/default-elision tests, not a
 * claim that SerializedShaderState is an injective representation of source.
 * In the collected 2021.3 corpus, the same all-plain-zero fog tuple with mode
 * 0 is produced by distinct source intent (including Color black, Color white,
 * and Mode Off). A compiler re-import plus compiled-program/render gate is
 * therefore still required for that tuple.
 */

typedef struct {
  int value;
  const char *spelling;
} Unity2021ShaderLabEnumSpelling;

static const Unity2021ShaderLabEnumSpelling
    kUnity2021CompareFunctionSpellings[] = {
        {0, "False"},   {1, "Never"},    {2, "Less"},
        {3, "Equal"},   {4, "LEqual"},   {5, "Greater"},
        {6, "NotEqual"}, {7, "GEqual"},  {8, "Always"},
};

static const Unity2021ShaderLabEnumSpelling kUnity2021CullSpellings[] = {
    {0, "Off"}, {1, "Front"}, {2, "Back"},
};

static const Unity2021ShaderLabEnumSpelling
    kUnity2021StencilOpSpellings[] = {
        {0, "Keep"},    {1, "Zero"},     {2, "Replace"},
        {3, "IncrSat"}, {4, "DecrSat"},  {5, "Invert"},
        {6, "IncrWrap"}, {7, "DecrWrap"},
};

static const Unity2021ShaderLabEnumSpelling kUnity2021BlendSpellings[] = {
    {0, "Zero"},
    {1, "One"},
    {2, "DstColor"},
    {3, "SrcColor"},
    {4, "OneMinusDstColor"},
    {5, "SrcAlpha"},
    {6, "OneMinusSrcColor"},
    {7, "DstAlpha"},
    {8, "OneMinusDstAlpha"},
    {9, "SrcAlphaSaturate"},
    {10, "OneMinusSrcAlpha"},
};

static const Unity2021ShaderLabEnumSpelling kUnity2021BlendOpSpellings[] = {
    {0, "Add"},
    {1, "Sub"},
    {2, "RevSub"},
    {3, "Min"},
    {4, "Max"},
    {5, "LogicalClear"},
    {6, "LogicalSet"},
    {7, "LogicalCopy"},
    {8, "LogicalCopyInverted"},
    {9, "LogicalNoop"},
    {10, "LogicalInvert"},
    {11, "LogicalAnd"},
    {12, "LogicalNand"},
    {13, "LogicalOr"},
    {14, "LogicalNor"},
    {15, "LogicalXor"},
    {16, "LogicalEquivalence"},
    {17, "LogicalAndReverse"},
    {18, "LogicalAndInverted"},
    {19, "LogicalOrReverse"},
    {20, "LogicalOrInverted"},
    {21, "Multiply"},
    {22, "Screen"},
    {23, "Overlay"},
    {24, "Darken"},
    {25, "Lighten"},
    {26, "ColorDodge"},
    {27, "ColorBurn"},
    {28, "HardLight"},
    {29, "SoftLight"},
    {30, "Difference"},
    {31, "Exclusion"},
    {32, "HSLHue"},
    {33, "HSLSaturation"},
    {34, "HSLColor"},
    {35, "HSLLuminosity"},
};

typedef struct {
  int z_clip;
  int z_test;
  int z_write;
  int cull;
  int conservative;
  int alpha_to_mask;
  int src_blend;
  int dst_blend;
  int blend_op;
  int color_mask;
  int stencil_op;
  int stencil_compare;
  int stencil_read_mask;
  int stencil_write_mask;
  int stencil_reference;
  int fog_mode;
  int lighting;
} Unity2021ShaderLabStateDefaults;

static const Unity2021ShaderLabStateDefaults kUnity2021StateDefaults = {
    1, 4, 1, 2, 0, 0, 1, 0, 0, 15, 0, 8, 255, 255, 0, -1, 0,
};

static const char *const kUnity2021FogDefaultNames[] = {
    "unity_FogColor", "unity_FogStart", "unity_FogEnd",
    "unity_FogDensity",
};

static const char *const kUnity2021EmitPassStateOrder[] = {
    "Lighting ", "AlphaToMask ", "ZTest ", "ZWrite ", "Cull ",
    "Stencil\n", "Blend ", "BlendOp ", "ColorMask ", "Offset ",
};

#endif /* UNITY_2021_3_SHADERLAB_STATE_REFERENCE_H */
