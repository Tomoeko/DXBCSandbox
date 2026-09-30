// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. Emission and topology are observable contracts.
Shader "Fixture/HighLevel/GeometryStreams" {
    SubShader {
        Pass {
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
            [instance(2)]
            [maxvertexcount(2)]
            void geom(point Vertex input[1], uint instanceId : SV_GSInstanceID,
                      inout PointStream<Vertex> visibleStream,
                      inout PointStream<Vertex> secondaryStream) {
                Vertex result = input[0];
                result.position.x += instanceId * 0.25;
                visibleStream.Append(result);
                result.position.y += 0.25;
                secondaryStream.Append(result);
            }
            ENDHLSL
        }
    }
}
