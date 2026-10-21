// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/SparseGlobals" { SubShader { Pass {
        HLSLPROGRAM
        #pragma target 5.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        float _Scale : register(c26);
        float4 _UnusedLayoutEnd : register(c27);
        struct v2f { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float2 coordinates : TEXCOORD1; };
        v2f vert(float3 position : POSITION) {
            v2f output;
            output.position = float4(position.xy, 0.0f, 1.0f);
            output.coordinates = _Scale * ((position.xy + float2(1.0f, 1.0f)) * float2(0.5f, -0.5f) + float2(0.0f, 1.0f));
            output.uv = position.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
            return output;
        }
        float4 frag() : SV_Target { return 0.0f; }
        ENDHLSL
} } }
