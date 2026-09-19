// SPDX-License-Identifier: GPL-3.0-only
// Controlled source for target.bin; it is not recovered original source.
Shader "Experiment/ConditionalFixture" {
    Properties { }
    SubShader {
        Pass {
            Name "CONDITIONAL"
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
                float4 result;
                [branch] if (asuint(input.flags.x)) {
                    [branch] if (asuint(input.flags.y))
                        result = input.value * input.value.yzwx;
                    else
                        result = input.value * input.value.wxyz;
                } else {
                    result = input.value * input.value.zwxy;
                }
                return result * input.value;
            }
            ENDHLSL
        }
    }
}
