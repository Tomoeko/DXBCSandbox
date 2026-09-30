// SPDX-License-Identifier: GPL-3.0-only
// Authored explicit-gradient vertex sampling evaluation input.
Shader "Fixture/HighLevel/VertexGradientSampling"
{
    SubShader
    {
        Pass
        {
            Name "VERTEX_GRADIENT"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ SAMPLING_CHANGED
            #pragma multi_compile __ SAMPLING_OFFSET
            #pragma multi_compile __ SAMPLING_PRECISE

            Texture2D<float4> _MainTex;
            SamplerState sampler_MainTex;
            cbuffer GradientInputs : register(b0)
            {
                float2 _GradientX : packoffset(c0);
                float2 _GradientY : packoffset(c1);
                float4 _Tint : packoffset(c2);
            };

            struct Interpolants
            {
                float4 position : SV_POSITION;
                float4 sampled : COLOR0;
            };

            Interpolants vert(float4 position : POSITION, float2 uv : TEXCOORD0)
            {
                float2 gradientX = _GradientX;
                float2 gradientY = _GradientY;
                #if defined(SAMPLING_CHANGED)
                    uv = uv * 1.25 + float2(0.0625, 0.125);
                    gradientX *= 1.5;
                    gradientY *= 0.75;
                #endif
                #if defined(SAMPLING_OFFSET)
                    float4 sampled = _MainTex.SampleGrad(sampler_MainTex, uv, gradientX, gradientY, int2(1, -1));
                #else
                    float4 sampled = _MainTex.SampleGrad(sampler_MainTex, uv, gradientX, gradientY);
                #endif
                #if defined(SAMPLING_PRECISE)
                    precise float4 color = sampled * _Tint;
                #else
                    float4 color = sampled * _Tint;
                #endif
                Interpolants output;
                output.position = position;
                output.sampled = color;
                return output;
            }

            float4 frag(float4 sampled : COLOR0) : SV_Target
            {
                return sampled;
            }
            ENDHLSL
        }
    }
}
