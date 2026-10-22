// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/SampledAlphaDiscard"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 5.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        Texture2D<float4> _CoverageTexture;
        SamplerState sampler_CoverageTexture;
        float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
        float4 frag(float2 coordinates : TEXCOORD0) : SV_Target
        {
            float4 color = _CoverageTexture.Sample(sampler_CoverageTexture, coordinates);
            if (color.a < 0.375f)
            {
                discard;
            }
            return color;
        }
        ENDHLSL
    } }
}
