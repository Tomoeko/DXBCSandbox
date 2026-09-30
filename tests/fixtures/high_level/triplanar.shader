// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. Coordinate projections are semantic swizzles.
Shader "Fixture/HighLevel/Triplanar"
{
    Properties
    {
        _MainTex ("Texture", 2D) = "white" {}
        _Tiling ("Tiling", Float) = 1
        _OcclusionMap ("Occlusion", 2D) = "white" {}
    }
    SubShader
    {
        Pass
        {
            Name "TRIPLANAR"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ OCCLUSION_DISABLED
            #include "UnityCG.cginc"

            struct Interpolants
            {
                float3 objectNormal : TEXCOORD0;
                float3 coordinates : TEXCOORD1;
                float2 uv : TEXCOORD2;
                float4 position : SV_POSITION;
            };
            float _Tiling;
            sampler2D _MainTex;
            sampler2D _OcclusionMap;

            Interpolants vert(float4 position : POSITION,
                              float3 normal : NORMAL, float2 uv : TEXCOORD0)
            {
                Interpolants output;
                output.position = UnityObjectToClipPos(position);
                output.coordinates = position.xyz * _Tiling;
                output.objectNormal = normal;
                output.uv = uv;
                return output;
            }

            float4 frag(Interpolants input) : SV_Target
            {
                float3 weights = abs(input.objectNormal);
                weights /= dot(weights, float3(1, 1, 1));
                float4 projectionX = tex2D(_MainTex, input.coordinates.yz);
                float4 projectionY = tex2D(_MainTex, input.coordinates.xz);
                float4 projectionZ = tex2D(_MainTex, input.coordinates.xy);
                float4 color = projectionX * weights.x
                             + projectionY * weights.y
                             + projectionZ * weights.z;
                #if !defined(OCCLUSION_DISABLED)
                    color *= tex2D(_OcclusionMap, input.uv);
                #endif
                return color;
            }
            ENDHLSL
        }
    }
}
