// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/InactiveFragmentInputs"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 5.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        struct FragmentInput
        {
            float4 position : SV_POSITION;
            float3 normal : TEXCOORD0;
            float weight : TEXCOORD4;
            float3 direction : TEXCOORD1;
            float3 tangent : TEXCOORD2;
            float4 coordinates : TEXCOORD3;
            nointerpolation uint identifier : TEXCOORD5;
        };
        float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
        float4 frag(FragmentInput input) : SV_Target { return float4(0.0f, 0.0f, 0.0f, 1.0f); }
        ENDHLSL
    } }
}
