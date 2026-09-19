// SPDX-License-Identifier: GPL-3.0-only
// Controlled source for target.bin; it is not recovered original source.
Shader "Experiment/ExpressionFixture" {
    Properties { }
    SubShader {
        Pass {
            Name "EXPRESSION"
            Cull Off ZWrite Off ZTest Always Blend Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            struct Interpolants {
                float4 position : SV_POSITION;
                float4 value : TEXCOORD0;
            };
            Interpolants vert(float4 position : POSITION) {
                Interpolants output;
                output.position = position;
                output.value = position;
                return output;
            }
            float4 frag(Interpolants input) : SV_Target {
                float4 product = input.value * input.value.yzwx;
                return product * input.value.zwxy;
            }
            ENDHLSL
        }
    }
}
