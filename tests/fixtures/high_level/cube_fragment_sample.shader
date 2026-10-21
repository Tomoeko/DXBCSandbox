// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/SampledFloatTexture"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 4.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        TextureCube<float4> _Texture;
        SamplerState sampler_Texture;
        float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
        float4 frag(float4 position : SV_POSITION, float3 coordinates : TEXCOORD0) : SV_Target
        {
            return _Texture.Sample(sampler_Texture, coordinates);
        }
        ENDHLSL
    } }
}
