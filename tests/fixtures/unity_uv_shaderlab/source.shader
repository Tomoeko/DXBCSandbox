// SPDX-License-Identifier: GPL-3.0-only
// Controlled source for target.bin; it is not recovered original source.
Shader "Experiment/PackedUVFixture" {
    Properties { }
    SubShader {
        Pass {
            Name "PACKED_UV"
            Cull Off ZWrite Off ZTest Always Blend Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ UNITY_SINGLE_PASS_STEREO
            #pragma multi_compile __ UV_VARIANT
            struct Interpolants {
                float4 position : SV_POSITION;
                float4 uv : TEXCOORD0;
                float4 scaleOffset : TEXCOORD1;
            };
            Interpolants vert(float4 position : POSITION) {
                Interpolants output;
                output.position = position;
                output.uv = position;
                output.scaleOffset = position;
                return output;
            }
            float4 frag(float4 uv : TEXCOORD0, float4 scaleOffset : TEXCOORD1) : SV_Target {
                return uv * scaleOffset.xyxy + scaleOffset.zwzw;
            }
            ENDHLSL
        }
    }
}
