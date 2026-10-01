// SPDX-License-Identifier: GPL-3.0-only
// Fragment reconstruction fixture; vertex is an authored compilation stub.
Shader "Fixture/HighLevel/StructuredFloat3MadBranch"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma vertex vert
            #pragma fragment frag

            float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
            float3 frag(float3 value : TEXCOORD0, nointerpolation float gate : TEXCOORD1) : SV_Target
            {
                float3 result;
                [branch] if (gate < 0.375f) result = value * 1.75f + gate;
                else result = value * 3.5f + 0.25f;
                return result * value + 1.25f;
            }
            ENDHLSL
        }
    }
}
