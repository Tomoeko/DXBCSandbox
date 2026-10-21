// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/SampledFloatTexture"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 4.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        Texture2D<float4> _Texture;
        SamplerState sampler_Texture;
        float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
        bool4 valueIsNaN(float4 value)
        {
            return ((value < 0.0f) || (0.0f < value) || (value == 0.0f)) == false;
        }
        float4 frag(float4 position : SV_POSITION, float2 unusedCoordinates : TEXCOORD0, float2 coordinates : TEXCOORD1) : SV_Target
        {
            float4 color = _Texture.Sample(sampler_Texture, coordinates);
            bool4 invalid = valueIsNaN(color);
            return (invalid.r || invalid.g || invalid.b || invalid.a) ? float4(0.0f, 0.0f, 0.0f, 0.0f) : color;
        }
        ENDHLSL
    } }
}
