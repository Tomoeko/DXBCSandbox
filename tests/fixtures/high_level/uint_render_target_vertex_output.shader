// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/DirectFloatToUIntVertex"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 4.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        cbuffer ConvertedOutputInputs : register(b0)
        {
            float _UVScale;
            float _LayerIndex;
        };
        struct EvaluatedOutput
        {
            float4 position : SV_POSITION;
            float2 uv0 : TEXCOORD0;
            float2 uv1 : TEXCOORD1;
            uint layer : SV_RenderTargetArrayIndex;
        };
        EvaluatedOutput vert(float3 position : POSITION)
        {
            EvaluatedOutput output;
            output.position.xy = position.xy;
            output.position.zw = float2(0.0f, 1.0f);
            output.uv0 = position.xy * _UVScale + float2(0.125f, -0.25f);
            output.uv1 = position.yx * 0.375f;
            output.layer = (uint)_LayerIndex;
            return output;
        }
        float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
        ENDHLSL
    } }
}
