// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. Emission and topology are observable contracts.
Shader "Fixture/HighLevel/GeometryPrimitives" {
    SubShader {
        Pass {
            Name "LINE_AMPLIFICATION"
            Cull Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            struct Vertex { float4 position : SV_POSITION; };
            Vertex vert(float4 position : POSITION) {
                Vertex result; result.position = position; return result;
            }
            float4 frag() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
            [maxvertexcount(4)]
            void geom(line Vertex input[2], inout TriangleStream<Vertex> stream) {
                for (uint index = 0; index < 2; ++index) {
                    Vertex result = input[index];
                    result.position.y -= 0.125; stream.Append(result);
                    result.position.y += 0.25; stream.Append(result);
                }
                stream.RestartStrip();
            }
            ENDHLSL
        }
        Pass {
            Name "TRIANGLE_AMPLIFICATION"
            Cull Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            struct Vertex { float4 position : SV_POSITION; };
            Vertex vert(float4 position : POSITION) {
                Vertex result; result.position = position; return result;
            }
            float4 frag() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
            [maxvertexcount(6)]
            void geom(triangle Vertex input[3], inout TriangleStream<Vertex> stream) {
                for (uint triangleIndex = 0; triangleIndex < 2; ++triangleIndex) {
                    for (uint index = 0; index < 3; ++index) {
                        Vertex result = input[index];
                        result.position.x += triangleIndex * 0.125;
                        stream.Append(result);
                    }
                    stream.RestartStrip();
                }
            }
            ENDHLSL
        }
        Pass {
            Name "LINE_ADJACENCY"
            Cull Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            struct Vertex { float4 position : SV_POSITION; };
            Vertex vert(float4 position : POSITION) {
                Vertex result; result.position = position; return result;
            }
            float4 frag() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
            [maxvertexcount(2)]
            void geom(lineadj Vertex input[4], inout LineStream<Vertex> stream) {
                Vertex first = input[1];
                Vertex second = input[2];
                first.position.y += (input[0].position.y - first.position.y) * 0.125;
                second.position.y += (input[3].position.y - second.position.y) * 0.125;
                stream.Append(first); stream.Append(second); stream.RestartStrip();
            }
            ENDHLSL
        }
        Pass {
            Name "TRIANGLE_ADJACENCY"
            Cull Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            struct Vertex { float4 position : SV_POSITION; };
            Vertex vert(float4 position : POSITION) {
                Vertex result; result.position = position; return result;
            }
            float4 frag() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
            [maxvertexcount(3)]
            void geom(triangleadj Vertex input[6], inout TriangleStream<Vertex> stream) {
                for (uint index = 0; index < 3; ++index) {
                    Vertex result = input[index * 2];
                    result.position.y += input[index * 2 + 1].position.y * 0.125;
                    stream.Append(result);
                }
                stream.RestartStrip();
            }
            ENDHLSL
        }
    }
}
