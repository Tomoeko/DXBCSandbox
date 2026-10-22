// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/SampledMaterialDiscard"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 5.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        cbuffer MaterialValues
        {
            float4 _ResultColor : packoffset(c2);
            float _Cutoff : packoffset(c3);
        };
        Texture2D<float4> _CoverageTexture;
        SamplerState sampler_CoverageTexture;
        float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
        float4 frag(float2 coordinates : TEXCOORD0) : SV_Target
        {
            float alpha = _CoverageTexture.Sample(sampler_CoverageTexture, coordinates).a;
            clip(alpha - _Cutoff);
            return _ResultColor;
        }
        ENDHLSL
    } }
}
