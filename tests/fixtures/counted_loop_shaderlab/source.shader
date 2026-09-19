// SPDX-License-Identifier: GPL-3.0-only
// Controlled source for target.bin; it is not recovered original source.
Shader "Experiment/CountedLoopFixture" {
    Properties { }
    SubShader {
        Pass {
            Name "COUNTED_LOOP"
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
                float4 result = input.value, second = input.value.yzwx;
                [loop] for (uint index = 0u; index < asuint(input.flags.w); ++index) {
                    result = result * second;
                    second = second * input.value;
                }
                return result * second;
            }
            ENDHLSL
        }
    }
}
