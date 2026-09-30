// SPDX-License-Identifier: GPL-3.0-only
// Natural scalar/vector widths exercise partial signature masks and writes.
Shader "Fixture/HighLevel/ValueWidths" {
    SubShader {
        Pass {
            Name "WIDTH_1"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            struct Interpolants {
                float4 position : SV_POSITION;
                float value : TEXCOORD0;
            };
            Interpolants vert(float4 position : POSITION) {
                Interpolants result;
                result.position = position;
                result.value = position.x;
                return result;
            }
            float frag(Interpolants input) : SV_Target {
                float squared = input.value * input.value;
                return squared + input.value;
            }
            ENDHLSL
        }
        Pass {
            Name "WIDTH_2"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            struct Interpolants {
                float4 position : SV_POSITION;
                float2 value : TEXCOORD0;
            };
            Interpolants vert(float4 position : POSITION) {
                Interpolants result;
                result.position = position;
                result.value = position.xy;
                return result;
            }
            float2 frag(Interpolants input) : SV_Target {
                float2 squared = input.value * input.value;
                return squared + input.value;
            }
            ENDHLSL
        }
        Pass {
            Name "WIDTH_3"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            struct Interpolants {
                float4 position : SV_POSITION;
                float3 value : TEXCOORD0;
            };
            Interpolants vert(float4 position : POSITION) {
                Interpolants result;
                result.position = position;
                result.value = position.xyz;
                return result;
            }
            float3 frag(Interpolants input) : SV_Target {
                float3 squared = input.value * input.value;
                return squared + input.value;
            }
            ENDHLSL
        }
        Pass {
            Name "WIDTH_4"
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
                Interpolants result;
                result.position = position;
                result.value = position.xyzw;
                return result;
            }
            float4 frag(Interpolants input) : SV_Target {
                float4 squared = input.value * input.value;
                return squared + input.value;
            }
            ENDHLSL
        }
        Pass {
            Name "PARTIAL_WRITES"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            struct Interpolants { float4 position : SV_POSITION; float4 value : TEXCOORD0; };
            Interpolants vert(float4 position : POSITION) {
                Interpolants result; result.position = position; result.value = position; return result;
            }
            float4 frag(Interpolants input) : SV_Target {
                float4 color;
                color.xy = input.value.xy * input.value.zw;
                color.zw = input.value.zw + input.value.xy;
                return color;
            }
            ENDHLSL
        }
    }
}
