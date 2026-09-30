// SPDX-License-Identifier: GPL-3.0-only
// Controlled unrolled arithmetic evaluation; no runtime dispatch.
Shader "Fixture/HighLevel/LongArithmetic"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ ARITHMETIC_CHANGED

            struct Varyings
            {
                float4 position : SV_POSITION;
                float4 color : COLOR0;
            };

            Varyings vert(float4 position : POSITION, float4 color : COLOR0)
            {
                Varyings output;
                output.position = position;
                output.color = color;
                return output;
            }

            float4 frag(float4 color : COLOR0) : SV_Target
            {
                float4 value = color;
                [unroll]
                for (int step = 0; step < 80; ++step)
                {
                #if defined(ARITHMETIC_CHANGED)
                    value = frac(value * float4(1.1640625 + step * 0.00390625,
                                               1.30859375 + step * 0.001953125,
                                               1.453125 + step * 0.0009765625,
                                               1.59765625 + step * 0.00048828125) + color);
                #else
                    value = frac(value * float4(1.11328125 + step * 0.00390625,
                                               1.2578125 + step * 0.001953125,
                                               1.40234375 + step * 0.0009765625,
                                               1.546875 + step * 0.00048828125) + color);
                #endif
                }
                return value;
            }
            ENDHLSL
        }
    }
}
