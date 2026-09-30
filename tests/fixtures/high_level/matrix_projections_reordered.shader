// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. Matrix names do not identify coordinate spaces.
Shader "Fixture/HighLevel/ReorderedMatrixProjections"
{
    SubShader
    {
        Pass
        {
            Name "MATRIX_TRANSFORMS"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 4.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ MATRIX_VECTOR_FIRST
            #pragma multi_compile __ MATRIX_ROW_MAJOR
            #pragma multi_compile __ MATRIX_PRECISE
            #pragma multi_compile __ MATRIX_ARRAY
            #pragma multi_compile __ MATRIX_EXTERNAL_USE

            #if defined(MATRIX_ROW_MAJOR)
                #define MATRIX_STORAGE row_major
            #else
                #define MATRIX_STORAGE column_major
            #endif
            cbuffer TransformParameters : register(b0)
            {
            MATRIX_STORAGE float4x4 _ProjectionB : packoffset(c0);
            #if defined(MATRIX_ARRAY)
                MATRIX_STORAGE float4x4 _ProjectionA[2] : packoffset(c4);
            #else
                MATRIX_STORAGE float4x4 _ProjectionA : packoffset(c4);
            #endif
            };

            struct Interpolants
            {
                float3 transformedNormal : TEXCOORD0;
                float4 position : SV_POSITION;
                #if defined(MATRIX_EXTERNAL_USE)
                    float4 partial : TEXCOORD1;
                #endif
            };

            Interpolants vert(float3 normal : NORMAL, float4 position : POSITION)
            {
                #if defined(MATRIX_ARRAY)
                    MATRIX_STORAGE float4x4 transform = _ProjectionA[1];
                #else
                    MATRIX_STORAGE float4x4 transform = _ProjectionA;
                #endif
                Interpolants output;
                #if defined(MATRIX_PRECISE)
                    precise float4 projected;
                    precise float3 transformedNormal;
                #else
                    float4 projected;
                    float3 transformedNormal;
                #endif
                #if defined(MATRIX_VECTOR_FIRST)
                    projected = mul(position, transform);
                    transformedNormal = mul(normal, (float3x3)_ProjectionB);
                #else
                    projected = mul(transform, position);
                    transformedNormal = mul((float3x3)_ProjectionB, normal);
                #endif
                output.position = projected;
                output.transformedNormal = transformedNormal;
                #if defined(MATRIX_EXTERNAL_USE)
                    output.partial = transform[1] * position.y;
                #endif
                return output;
            }

            float4 frag(Interpolants input) : SV_Target
            {
                float4 color = float4(input.transformedNormal, 1);
                #if defined(MATRIX_EXTERNAL_USE)
                    color += input.partial;
                #endif
                return color;
            }
            ENDHLSL
        }
    }
}
