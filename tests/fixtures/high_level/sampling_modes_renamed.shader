// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input; positions are already in clip coordinates.
Shader "Fixture/HighLevel/RenamedSamplingModes"
{
    Properties
    {
        _SurfaceImage ("Image", 2D) = "white" {}
        _ColorScale ("Tint", Color) = (1, 1, 1, 1)
        _Level ("Explicit mip", Float) = 0
        _Bias ("Mip bias", Float) = 0
    }
    SubShader
    {
        HLSLINCLUDE
        Texture2D<float4> _SurfaceImage;
        SamplerState sampler_SurfaceImage;
        cbuffer SamplingParameters : register(b0)
        {
            float4 _ColorScale : packoffset(c0);
            float2 _Dy : packoffset(c1);
            float2 _Dx : packoffset(c2);
            float _Bias : packoffset(c3);
            float _Level : packoffset(c4);
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
                precise float4 color = sampled * _ColorScale;
            #else
                float4 color = sampled * _ColorScale;
            #endif
            return color;
        }

        float4 SampleExplicitLevel(float2 uv)
        {
            float level = _Level;
            #if defined(SAMPLING_CHANGED)
                level += 0.375;
            #endif
            #if defined(SAMPLING_OFFSET)
                float4 sampled = _SurfaceImage.SampleLevel(sampler_SurfaceImage, SamplingCoordinates(uv), level, int2(1, -1));
            #else
                float4 sampled = _SurfaceImage.SampleLevel(sampler_SurfaceImage, SamplingCoordinates(uv), level);
            #endif
            return ApplyTint(sampled);
        }

        float4 fragLevel(Interpolants input) : SV_Target
        {
            return SampleExplicitLevel(input.uv);
        }

        float4 fragBias(Interpolants input) : SV_Target
        {
            float bias = _Bias;
            #if defined(SAMPLING_CHANGED)
                bias -= 0.25;
            #endif
            #if defined(SAMPLING_OFFSET)
                float4 sampled = _SurfaceImage.SampleBias(sampler_SurfaceImage, SamplingCoordinates(input.uv), bias, int2(1, -1));
            #else
                float4 sampled = _SurfaceImage.SampleBias(sampler_SurfaceImage, SamplingCoordinates(input.uv), bias);
            #endif
            return ApplyTint(sampled);
        }

        float4 fragGrad(Interpolants input) : SV_Target
        {
            float2 gradientX = _Dx;
            float2 gradientY = _Dy;
            #if defined(SAMPLING_CHANGED)
                gradientX *= 1.5;
                gradientY *= 0.75;
            #endif
            #if defined(SAMPLING_OFFSET)
                float4 sampled = _SurfaceImage.SampleGrad(sampler_SurfaceImage, SamplingCoordinates(input.uv), gradientX, gradientY, int2(1, -1));
            #else
                float4 sampled = _SurfaceImage.SampleGrad(sampler_SurfaceImage, SamplingCoordinates(input.uv), gradientX, gradientY);
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
