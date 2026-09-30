// SPDX-License-Identifier: GPL-3.0-only
// Point expansion has an observable emission order and strip restart.
Shader "Fixture/HighLevel/PointExpansion"
{
    Properties { _Radius ("Radius", Float) = 0.1 }
    SubShader
    {
        Pass
        {
            Cull Off
            HLSLPROGRAM
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ REVERSE_EMISSION

            struct Vertex { float4 position : SV_POSITION; };
            struct Fragment
            {
                float4 position : SV_POSITION;
                float2 uv : TEXCOORD0;
            };
            float _Radius;
            Vertex vert(float4 position : POSITION)
            {
                Vertex output;
                output.position = position;
                return output;
            }
            [maxvertexcount(4)]
            void geom(point Vertex input[1], inout TriangleStream<Fragment> stream)
            {
                const float2 corners[4] = {
                    float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1)
                };
                for (uint index = 0; index < 4; ++index)
                {
                    #if defined(REVERSE_EMISSION)
                        uint cornerIndex = 3 - index;
                    #else
                        uint cornerIndex = index;
                    #endif
                    Fragment output;
                    output.position = input[0].position;
                    output.position.xy += corners[cornerIndex] * _Radius;
                    output.uv = corners[cornerIndex] * 0.5 + 0.5;
                    stream.Append(output);
                }
                stream.RestartStrip();
            }
            float4 frag(Fragment input) : SV_Target
            {
                return float4(input.uv, 0, 1);
            }
            ENDHLSL
        }
    }
}
