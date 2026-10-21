// SPDX-License-Identifier: GPL-3.0-only
Shader "Fixture/HighLevel/SparseGlobals" { SubShader { Pass {
        HLSLPROGRAM
        #pragma target 4.0
        #pragma only_renderers d3d11
        #pragma vertex vert
        #pragma fragment frag
        float _Scale : register(c26);
        float4 _Transform : register(c27);
        float4 _UnusedLayoutEnd : register(c42);
        struct v2f { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float2 coordinates : TEXCOORD1; };
        v2f vert(float3 position : POSITION) {
            v2f output;
            output.position = float4(position.xy, 0.0f, 1.0f);
            float2 coordinates = (position.xy + float2(1.0f, 1.0f)) * _Transform.xy;
            coordinates = coordinates * float2(0.5f, 0.5f) + _Transform.zw;
            output.coordinates = coordinates * _Scale;
            output.uv = coordinates;
            return output;
        }
        float4 frag() : SV_Target { return 0.0f; }
        ENDHLSL
} } }
