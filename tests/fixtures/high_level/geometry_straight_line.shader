// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input; positions are already in clip coordinates.
Shader "Fixture/HighLevel/GeometryStraightLine"
{
    SubShader
    {
        Pass
        {
            Name "POINT_TRIANGLE"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma target 4.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ GS_REVERSE_ORDER

            cbuffer GeometryInputs : register(b0)
            {
                float4 _Offset : packoffset(c0);
                float4 _Tint : packoffset(c1);
                float4 _Scale : packoffset(c2);
            };

            struct Vertex
            {
                float4 position : SV_POSITION;
                float4 color : COLOR0;
            };

            Vertex vert(float4 position : POSITION, float4 color : COLOR0)
            {
                Vertex output;
                output.position = position;
                output.color = color;
                return output;
            }

            [maxvertexcount(3)]
            void geom(point Vertex input[1], inout TriangleStream<Vertex> stream)
            {
                Vertex output;
                #if defined(GS_REVERSE_ORDER)
                    output.position = input[0].position - _Offset;
                #else
                    output.position = input[0].position + _Offset;
                #endif
                output.color = input[0].color * _Tint;
                stream.Append(output);

                #if defined(GS_REVERSE_ORDER)
                    output.position = input[0].position + _Offset;
                #else
                    output.position = input[0].position - _Offset;
                #endif
                output.color = input[0].color + _Tint;
                stream.Append(output);

                output.position = input[0].position * _Scale + _Offset;
                output.color = input[0].color * _Scale;
                stream.Append(output);
                stream.RestartStrip();
            }

            float4 frag(Vertex input) : SV_Target
            {
                return input.color;
            }
            ENDHLSL
        }
    }
}
