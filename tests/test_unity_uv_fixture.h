// SPDX-License-Identifier: GPL-3.0-only

#ifndef TEST_UNITY_UV_FIXTURE_H
#define TEST_UNITY_UV_FIXTURE_H

#include "common/string_builder.h"

/* Deliberately small authored token fixtures; installed Unity includes are
 * read only by the optional live compiler check, never redistributed here. */
static const char test_uv_scalar[] =
    "inline float2 UnityStereoScreenSpaceUVAdjustInternal(float2 uv, float4 scaleAndOffset) {"
    "return uv.xy * scaleAndOffset.xy + scaleAndOffset.zw;}\n";
static const char test_uv_vector[] =
    "inline float4 UnityStereoScreenSpaceUVAdjustInternal(float4 uv, float4 scaleAndOffset) {"
    "return float4(UnityStereoScreenSpaceUVAdjustInternal(uv.xy, scaleAndOffset),"
    "UnityStereoScreenSpaceUVAdjustInternal(uv.zw, scaleAndOffset));}\n";
static const char test_uv_probe[] =
    "float4 dxbc_unity_uv_contract_probe(float4 dxbc_uv, float4 dxbc_scale_offset) {"
    "return UnityStereoScreenSpaceUVAdjustInternal(dxbc_uv, dxbc_scale_offset);}\n";

static void test_uv_expansion(StringBuilder *source) {
    sb_init(source);
    sb_append(source, test_uv_scalar);
    sb_append(source, test_uv_vector);
    sb_append(source, test_uv_probe);
}

#endif
