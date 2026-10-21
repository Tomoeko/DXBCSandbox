// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/InactiveFragmentInputs"
{
    SubShader { Pass {
        HLSLPROGRAM
        #pragma target 4.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        struct FragmentInput
        {
            float4 position : SV_POSITION;
            float4 color : COLOR0;
            uint layer : SV_RenderTargetArrayIndex;
        };
        float4 vert(float4 position : POSITION) : SV_POSITION { return position; }
        float4 frag(FragmentInput input) : SV_Target { return input.color; }
        ENDHLSL
    } }
}
