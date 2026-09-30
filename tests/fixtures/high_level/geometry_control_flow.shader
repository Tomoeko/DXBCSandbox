// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. The runtime loop and emission order are observable.
Shader "Fixture/HighLevel/GeometryControlFlow"
{
    Properties
    {
        _RepeatCount ("Repeat Count", Int) = 2
        _ClipThreshold ("Clip Threshold", Float) = 0
    }
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
            #pragma multi_compile __ REVERSE_WINDING

            struct Vertex { float4 position : SV_POSITION; };
            struct Fragment
            {
                float4 position : SV_POSITION;
                float2 uv : TEXCOORD0;
            };
            int _RepeatCount;
            float _ClipThreshold;

            Vertex vert(float4 position : POSITION)
            {
                Vertex result;
                result.position = position;
                return result;
            }

            [maxvertexcount(9)]
            void geom(point Vertex input[1], inout TriangleStream<Fragment> stream)
            {
                int repeatCount = min(max(_RepeatCount, 0), 3);
                [branch]
                if (input[0].position.w >= _ClipThreshold)
                {
                    [loop]
                    for (int emissionIndex = 0; emissionIndex < repeatCount; ++emissionIndex)
                    {
                        Fragment first;
                        first.position = input[0].position;
                        first.position.xy += float2(emissionIndex * 0.25, -0.125);
                        first.uv = float2(0, 0);
                        Fragment second = first;
                        second.position.x += 0.125;
                        second.uv = float2(1, 0);
                        Fragment third = first;
                        third.position.y += 0.25;
                        third.uv = float2(0, 1);
                        stream.Append(first);
                        #if defined(REVERSE_WINDING)
                            stream.Append(third);
                            stream.Append(second);
                        #else
                            stream.Append(second);
                            stream.Append(third);
                        #endif
                        stream.RestartStrip();
                    }
                }
            }

            float4 frag(Fragment input) : SV_Target
            {
                return float4(input.uv, 0.5, 1);
            }
            ENDHLSL
        }
    }
}
