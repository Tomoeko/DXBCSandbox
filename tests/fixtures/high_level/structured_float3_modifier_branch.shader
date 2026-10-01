// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/StructuredDot3NegAbs"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 5.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
        float3 frag(float3 value : TEXCOORD0, float3 direction : TEXCOORD1,
                    nointerpolation float gate : TEXCOORD2) : SV_Target
        {
            float result;
            [branch] if (gate < 0.375f) result = dot(-abs(value), direction);
            else result = dot(value, direction + 0.25f);
            return float3(result, result, result);
        }
        ENDHLSL
    } }
}
