// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_UNITY_UV_LIFT_H
#define HLSL_UNITY_UV_LIFT_H

#include "translation/usil.h"

#define HLSL_UNITY_UV_LIFT_ID "unity-packed-uv-adjust"
#define HLSL_UNITY_UV_LIFT_VERSION 1U
#define HLSL_UNITY_UV_FUNCTION "UnityStereoScreenSpaceUVAdjust"
#define HLSL_UNITY_UV_INCLUDE_SOURCE "#include \"UnityCG.cginc\"\n\n"

/* Complete two-instruction SM4/5 vertex/pixel pattern: full-float4 output MAD
 * of an identity input and another input's xyxy/zwzw lanes, then RET. Only
 * immediate register indices, plain float32 signatures/operands and the empty
 * cbuffer/resource layout are admitted. This is lane/shape evidence only;
 * actual include definitions, compiler controls, ABI and complete output bytes
 * must still be checked by the caller. No source names or hashes select it. */
bool hlsl_unity_uv_lift_matches(const USILProgram *program);

#endif
