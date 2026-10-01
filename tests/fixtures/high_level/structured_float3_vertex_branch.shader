// SPDX-License-Identifier: GPL-3.0-only
// Vertex reconstruction fixture; fragment is an authored compilation stub.
Shader "Fixture/HighLevel/StructuredFloat3VertexBranch"
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

            struct EvaluatedOutput { float4 position : SV_POSITION; float3 value : TEXCOORD0; };
            EvaluatedOutput vert(float4 position : POSITION, float3 value : NORMAL, float gate : TEXCOORD0)
            {
                float3 result;
                [branch] if (asuint(gate)) result = value * 2.0f;
                else result = value + 1.0f;
                EvaluatedOutput output;
                output.position = position;
                output.value = result * value;
                return output;
            }
            float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
            ENDHLSL
        }
    }
}
