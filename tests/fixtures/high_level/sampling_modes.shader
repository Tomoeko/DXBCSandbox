// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input; positions are already in clip coordinates.
Shader "Fixture/HighLevel/SamplingModes"
{
    Properties
    {
        _MainTex ("Image", 2D) = "white" {}
        _Tint ("Tint", Color) = (1, 1, 1, 1)
        _MipLevel ("Explicit mip", Float) = 0
        _MipBias ("Mip bias", Float) = 0
    }
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

        Interpolants vert(float4 position : POSITION, float2 uv : TEXCOORD0)
        {
            Interpolants output;
            output.position = position;
            output.uv = uv;
            return output;
        }

        float2 SamplingCoordinates(float2 uv)
        {
            #if defined(SAMPLING_CHANGED)
                return uv * 1.25 + float2(0.0625, 0.125);
            #else
                return uv;
            #endif
        }

        float4 ApplyTint(float4 sampled)
        {
            #if defined(SAMPLING_PRECISE)
                precise float4 color = sampled * _Tint;
            #else
                float4 color = sampled * _Tint;
            #endif
            return color;
        }

        float4 SampleExplicitLevel(float2 uv)
        {
            float level = _MipLevel;
            #if defined(SAMPLING_CHANGED)
                level += 0.375;
            #endif
            #if defined(SAMPLING_OFFSET)
                float4 sampled = _MainTex.SampleLevel(sampler_MainTex, SamplingCoordinates(uv), level, int2(1, -1));
            #else
                float4 sampled = _MainTex.SampleLevel(sampler_MainTex, SamplingCoordinates(uv), level);
            #endif
            return ApplyTint(sampled);
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
            #if defined(SAMPLING_OFFSET)
                float4 sampled = _MainTex.SampleBias(sampler_MainTex, SamplingCoordinates(input.uv), bias, int2(1, -1));
            #else
                float4 sampled = _MainTex.SampleBias(sampler_MainTex, SamplingCoordinates(input.uv), bias);
            #endif
            return ApplyTint(sampled);
        }

        float4 fragGrad(Interpolants input) : SV_Target
        {
            float2 gradientX = _GradientX;
            float2 gradientY = _GradientY;
            #if defined(SAMPLING_CHANGED)
                gradientX *= 1.5;
                gradientY *= 0.75;
            #endif
            #if defined(SAMPLING_OFFSET)
                float4 sampled = _MainTex.SampleGrad(sampler_MainTex, SamplingCoordinates(input.uv), gradientX, gradientY, int2(1, -1));
            #else
                float4 sampled = _MainTex.SampleGrad(sampler_MainTex, SamplingCoordinates(input.uv), gradientX, gradientY);
            #endif
            return ApplyTint(sampled);
        }

        struct SampledVertex
        {
            float4 position : SV_POSITION;
            float4 sampled : COLOR0;
        };

        SampledVertex vertLevel(float4 position : POSITION, float2 uv : TEXCOORD0)
        {
            SampledVertex output;
            output.position = position;
            output.sampled = SampleExplicitLevel(uv);
            return output;
        }

        float4 fragVertexLevel(SampledVertex input) : SV_Target
        {
            return input.sampled;
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
            #pragma multi_compile __ SAMPLING_OFFSET
            #pragma multi_compile __ SAMPLING_PRECISE
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
            #pragma multi_compile __ SAMPLING_OFFSET
            #pragma multi_compile __ SAMPLING_PRECISE
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
            #pragma multi_compile __ SAMPLING_OFFSET
            #pragma multi_compile __ SAMPLING_PRECISE
            ENDHLSL
        }

        Pass
        {
            Name "VERTEX_LEVEL"
            HLSLPROGRAM
            #pragma vertex vertLevel
            #pragma fragment fragVertexLevel
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ SAMPLING_CHANGED
            #pragma multi_compile __ SAMPLING_OFFSET
            #pragma multi_compile __ SAMPLING_PRECISE
            ENDHLSL
        }
    }
}
