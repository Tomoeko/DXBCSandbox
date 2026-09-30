// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. Matrix names do not identify coordinate spaces.
Shader "Fixture/HighLevel/MatrixVector4"
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
            cbuffer MatrixInputs : register(b0)
            {
            #if defined(MATRIX_ARRAY)
                MATRIX_STORAGE float4x4 _Transform[2];
            #else
                MATRIX_STORAGE float4x4 _Transform;
            #endif
            };

            struct Interpolants
            {
                float4 position : SV_POSITION;
                #if defined(MATRIX_EXTERNAL_USE)
                    float4 partial : TEXCOORD1;
                #endif
            };

            Interpolants vert(float4 position : POSITION)
            {
                #if defined(MATRIX_ARRAY)
                    MATRIX_STORAGE float4x4 transform = _Transform[1];
                #else
                    MATRIX_STORAGE float4x4 transform = _Transform;
                #endif
                Interpolants output;
                #if defined(MATRIX_PRECISE)
                    precise float4 projected;
                #else
                    float4 projected;
                #endif
                #if defined(MATRIX_VECTOR_FIRST)
                    projected = mul(position, transform);
                #else
                    projected = mul(transform, position);
                #endif
                output.position = projected;
                #if defined(MATRIX_EXTERNAL_USE)
                    output.partial = transform[1] * position.y;
                #endif
                return output;
            }

            float4 frag(Interpolants input) : SV_Target
            {
                float4 color = float4(1, 1, 1, 1);
                #if defined(MATRIX_EXTERNAL_USE)
                    color += input.partial;
                #endif
                return color;
            }
            ENDHLSL
        }
    }
}
