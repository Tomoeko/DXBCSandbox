// SPDX-License-Identifier: GPL-3.0-only
// Controlled unrolled arithmetic evaluation; no runtime dispatch.
Shader "Fixture/HighLevel/RenamedLongArithmetic"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma vertex projectPoint
            #pragma fragment evaluateTint
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ ARITHMETIC_CHANGED

            struct SurfaceData
            {
                float4 clipPoint : SV_POSITION;
                float4 surfaceTint : COLOR0;
            };

            SurfaceData projectPoint(float4 clipPoint : POSITION, float4 surfaceTint : COLOR0)
            {
                SurfaceData result;
                result.clipPoint = clipPoint;
                result.surfaceTint = surfaceTint;
                return result;
            }

            float4 evaluateTint(float4 surfaceTint : COLOR0) : SV_Target
            {
                float4 accumulatedTint = surfaceTint;
                [unroll]
                for (int iteration = 0; iteration < 80; ++iteration)
                {
                #if defined(ARITHMETIC_CHANGED)
                    accumulatedTint = frac(accumulatedTint * float4(1.1640625 + iteration * 0.00390625,
                                               1.30859375 + iteration * 0.001953125,
                                               1.453125 + iteration * 0.0009765625,
                                               1.59765625 + iteration * 0.00048828125) + surfaceTint);
                #else
                    accumulatedTint = frac(accumulatedTint * float4(1.11328125 + iteration * 0.00390625,
                                               1.2578125 + iteration * 0.001953125,
                                               1.40234375 + iteration * 0.0009765625,
                                               1.546875 + iteration * 0.00048828125) + surfaceTint);
                #endif
                }
                return accumulatedTint;
            }
            ENDHLSL
        }
    }
}
