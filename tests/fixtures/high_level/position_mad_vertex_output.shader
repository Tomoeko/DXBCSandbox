// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/PositionMadRow1WithUvRow0"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 5.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        cbuffer OffsetPositionInputs : register(b0)
        {
            float4 _UvOffset;
            float4 _PositionScaleOffset;
        };
        struct EvaluatedOutput
        {
            float4 position : SV_POSITION;
            float2 uv0 : TEXCOORD0;
            float2 uv1 : TEXCOORD1;
        };
        EvaluatedOutput vert(float3 position : POSITION,
                             float2 uv0 : TEXCOORD0,
                             float2 uv1 : TEXCOORD1)
        {
            EvaluatedOutput output;
            output.position.xy = position.xy * _PositionScaleOffset.xy + _PositionScaleOffset.zw;
            output.position.zw = float2(0.0f, 1.0f);
            output.uv0 = uv0 + _UvOffset.xy;
            output.uv1 = uv1 * 0.375f;
            return output;
        }
        float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
        ENDHLSL
    } }
}
