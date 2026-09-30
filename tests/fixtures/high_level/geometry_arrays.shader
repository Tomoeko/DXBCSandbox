// SPDX-License-Identifier: GPL-3.0-only
// Controlled evaluation source: static primitive arrays and stream emission.
Shader "Fixture/HighLevel/GeometryArrays"
{
    SubShader
    {
        HLSLINCLUDE
        struct VertexData
        {
            float4 position : SV_POSITION;
            float4 color : COLOR0;
        };

        VertexData vert(float4 position : POSITION, float4 color : COLOR0)
        {
            VertexData output;
            output.position = position;
            output.color = color;
            return output;
        }

        float4 frag(float4 color : COLOR0) : SV_Target
        {
            return color;
        }
        ENDHLSL

        Pass
        {
            Name "LINE"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma geometry geom
            #pragma target 4.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ GS_ARRAY_REVERSE

            [maxvertexcount(2)]
            void geom(line VertexData input[2], inout LineStream<VertexData> output)
            {
                VertexData vertex0;
                vertex0.position = input[0].position;
                vertex0.color = input[0].color;
                VertexData vertex1;
                vertex1.position = input[1].position;
                vertex1.color = input[1].color;

                #if defined(GS_ARRAY_REVERSE)
                    output.Append(vertex1);
                    output.Append(vertex0);
                #else
                    output.Append(vertex0);
                    output.Append(vertex1);
                #endif
                output.RestartStrip();
            }
            ENDHLSL
        }

        Pass
        {
            Name "TRIANGLE"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma geometry geom
            #pragma target 4.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ GS_ARRAY_REVERSE

            [maxvertexcount(3)]
            void geom(triangle VertexData input[3], inout TriangleStream<VertexData> output)
            {
                VertexData vertex0;
                vertex0.position = input[0].position;
                vertex0.color = input[0].color;
                VertexData vertex1;
                vertex1.position = input[1].position;
                vertex1.color = input[1].color;
                VertexData vertex2;
                vertex2.position = input[2].position;
                vertex2.color = input[2].color;

                #if defined(GS_ARRAY_REVERSE)
                    output.Append(vertex2);
                    output.Append(vertex1);
                    output.Append(vertex0);
                #else
                    output.Append(vertex0);
                    output.Append(vertex1);
                    output.Append(vertex2);
                #endif
                output.RestartStrip();
            }
            ENDHLSL
        }

        Pass
        {
            Name "LINE_ADJACENCY"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma geometry geom
            #pragma target 4.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ GS_ARRAY_REVERSE

            [maxvertexcount(2)]
            void geom(lineadj VertexData input[4], inout LineStream<VertexData> output)
            {
                VertexData vertex0;
                vertex0.position = input[1].position;
                vertex0.color = input[0].color + input[1].color;
                VertexData vertex1;
                vertex1.position = input[2].position;
                vertex1.color = input[2].color + input[3].color;

                #if defined(GS_ARRAY_REVERSE)
                    output.Append(vertex1);
                    output.Append(vertex0);
                #else
                    output.Append(vertex0);
                    output.Append(vertex1);
                #endif
                output.RestartStrip();
            }
            ENDHLSL
        }

        Pass
        {
            Name "TRIANGLE_ADJACENCY"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma geometry geom
            #pragma target 4.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ GS_ARRAY_REVERSE

            [maxvertexcount(3)]
            void geom(triangleadj VertexData input[6], inout TriangleStream<VertexData> output)
            {
                VertexData vertex0;
                vertex0.position = input[0].position;
                vertex0.color = input[0].color + input[1].color;
                VertexData vertex1;
                vertex1.position = input[2].position;
                vertex1.color = input[2].color + input[3].color;
                VertexData vertex2;
                vertex2.position = input[4].position;
                vertex2.color = input[4].color + input[5].color;

                #if defined(GS_ARRAY_REVERSE)
                    output.Append(vertex2);
                    output.Append(vertex1);
                    output.Append(vertex0);
                #else
                    output.Append(vertex0);
                    output.Append(vertex1);
                    output.Append(vertex2);
                #endif
                output.RestartStrip();
            }
            ENDHLSL
        }
    }
}
