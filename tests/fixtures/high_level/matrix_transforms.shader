// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. Matrix names do not identify coordinate spaces.
Shader "Fixture/HighLevel/MatrixTransforms"
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
            #if defined(MATRIX_ARRAY)
                MATRIX_STORAGE float4x4 _Transform[2];
            #else
                MATRIX_STORAGE float4x4 _Transform;
            #endif
            MATRIX_STORAGE float3x3 _NormalTransform;

            struct Interpolants
            {
                float4 position : SV_POSITION;
                float3 transformedNormal : TEXCOORD0;
                #if defined(MATRIX_EXTERNAL_USE)
                    float4 partial : TEXCOORD1;
                #endif
            };

            Interpolants vert(float4 position : POSITION, float3 normal : NORMAL)
            {
                #if defined(MATRIX_ARRAY)
                    MATRIX_STORAGE float4x4 transform = _Transform[1];
                #else
                    MATRIX_STORAGE float4x4 transform = _Transform;
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
                    transformedNormal = mul(normal, _NormalTransform);
                #else
                    projected = mul(transform, position);
                    transformedNormal = mul(_NormalTransform, normal);
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
