// SPDX-License-Identifier: GPL-3.0-only
// Positive-only evaluation domain, separate from offset and precision controls.
Shader "Fixture/HighLevel/CompleteSampling"
{
    SubShader
    {
        HLSLINCLUDE
        Texture2D<float4> _MainTex;
        SamplerState sampler_MainTex;
        cbuffer SamplingInputs : register(b0)
        {
            float _MipLevel : packoffset(c0);
            float _MipBias : packoffset(c1);
            float2 _GradientX : packoffset(c2);
            float2 _GradientY : packoffset(c3);
            float4 _Tint : packoffset(c4);
        };

        struct Interpolants
        {
            float4 position : SV_POSITION;
            float2 uv : TEXCOORD0;
        };

        struct SampledVertex
        {
            float4 position : SV_POSITION;
            float4 sampled : COLOR0;
        };

        float2 SamplingCoordinates(float2 uv)
        {
            #if defined(SAMPLING_CHANGED)
                return uv * 1.25 + float2(0.0625, 0.125);
            #else
                return uv;
            #endif
        }

        float4 SampleExplicitLevel(float2 uv)
        {
            float level = _MipLevel;
            #if defined(SAMPLING_CHANGED)
                level += 0.375;
            #endif
            return _MainTex.SampleLevel(sampler_MainTex, SamplingCoordinates(uv), level) * _Tint;
        }

        float4 SampleExplicitGradient(float2 uv)
        {
            float2 gradientX = _GradientX;
            float2 gradientY = _GradientY;
            #if defined(SAMPLING_CHANGED)
                gradientX *= 1.5;
                gradientY *= 0.75;
            #endif
            return _MainTex.SampleGrad(sampler_MainTex, SamplingCoordinates(uv), gradientX, gradientY) * _Tint;
        }

        Interpolants vert(float4 position : POSITION, float2 uv : TEXCOORD0)
        {
            Interpolants output;
            output.position = position;
            output.uv = uv;
            return output;
        }

        float4 fragLevel(Interpolants input) : SV_Target
        {
            return SampleExplicitLevel(input.uv);
        }

        float4 fragBias(Interpolants input) : SV_Target
        {
            float bias = _MipBias;
            #if defined(SAMPLING_CHANGED)
                bias -= 0.25;
            #endif
            return _MainTex.SampleBias(sampler_MainTex, SamplingCoordinates(input.uv), bias) * _Tint;
        }

        float4 fragGrad(Interpolants input) : SV_Target
        {
            return SampleExplicitGradient(input.uv);
        }

        SampledVertex vertLevel(float4 position : POSITION, float2 uv : TEXCOORD0)
        {
            SampledVertex output;
            output.position = position;
            output.sampled = SampleExplicitLevel(uv);
            return output;
        }

        SampledVertex vertGrad(float4 position : POSITION, float2 uv : TEXCOORD0)
        {
            SampledVertex output;
            output.position = position;
            output.sampled = SampleExplicitGradient(uv);
            return output;
        }

        float4 fragSampled(float4 sampled : COLOR0) : SV_Target
        {
            return sampled;
        }
        ENDHLSL

        Pass
        {
            Name "FRAGMENT_LEVEL"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment fragLevel
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ SAMPLING_CHANGED
            ENDHLSL
        }

        Pass
        {
            Name "FRAGMENT_BIAS"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment fragBias
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ SAMPLING_CHANGED
            ENDHLSL
        }

        Pass
        {
            Name "FRAGMENT_GRAD"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment fragGrad
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ SAMPLING_CHANGED
            ENDHLSL
        }

        Pass
        {
            Name "VERTEX_LEVEL"
            HLSLPROGRAM
            #pragma vertex vertLevel
            #pragma fragment fragSampled
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ SAMPLING_CHANGED
            ENDHLSL
        }

        Pass
        {
            Name "VERTEX_GRADIENT"
            HLSLPROGRAM
            #pragma vertex vertGrad
            #pragma fragment fragSampled
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ SAMPLING_CHANGED
            ENDHLSL
        }
    }
}
