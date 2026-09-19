// SPDX-License-Identifier: GPL-3.0-only
// Controlled source for target.bin; it is not recovered original source.
Shader "Experiment/FunctionFixture" {
    Properties { }
    SubShader {
        Pass {
            Name "FUNCTION"
            Cull Off ZWrite Off ZTest Always Blend Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            struct Interpolants {
                float4 position : SV_POSITION;
                float4 value : TEXCOORD0;
                float4 flags : TEXCOORD1;
            };
            Interpolants vert(float4 position : POSITION) {
                Interpolants output;
                output.position = position;
                output.value = position;
                output.flags = position;
                return output;
            }
            float4 frag(Interpolants input) : SV_Target {
                float4 p = input.value * input.value.yzwx;
                float4 q = p * input.flags;
                float4 r = input.flags.zwxy * input.value.wzyx;
                float4 s = r * input.value;
                return q * s;
            }
            ENDHLSL
        }
    }
}
