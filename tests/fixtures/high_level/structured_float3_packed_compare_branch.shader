// SPDX-License-Identifier: GPL-3.0-only
// Fragment reconstruction fixture; vertex is an authored compilation stub.
Shader "Fixture/HighLevel/StructuredPackedFragmentCompare"
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
            float3 frag(float3 value : TEXCOORD0, float gate : TEXCOORD1) : SV_Target
            {
                float3 result;
                [branch] if (gate < 0.375f) result = value * 2.0f;
                else result = value + 1.0f;
                return result * value;
            }
            ENDHLSL
        }
    }
}
