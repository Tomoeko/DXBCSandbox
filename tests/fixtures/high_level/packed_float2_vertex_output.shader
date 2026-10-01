// SPDX-License-Identifier: GPL-3.0-only
// Vertex reconstruction recipe; fragment is an authored compilation stub.
Shader "Fixture/HighLevel/PackedOutputAddMul"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 5.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        struct EvaluatedOutput
        {
            float4 position : SV_POSITION;
            float2 uv0 : TEXCOORD0;
            float2 uv1 : TEXCOORD1;
        };
        EvaluatedOutput vert(float4 position : POSITION)
        {
            EvaluatedOutput output;
            output.position = position;
            output.uv0 = position.xy + 0.125f;
            output.uv1 = position.zw * 0.375f;
            return output;
        }
        float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
        ENDHLSL
    } }
}
