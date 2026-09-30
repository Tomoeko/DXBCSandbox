// SPDX-License-Identifier: GPL-3.0-only
// Same algorithm with renamed metadata and reordered resource declarations.
Shader "Fixture/HighLevel/RenamedProjections"
{
    Properties
    {
        _Albedo ("Texture", 2D) = "white" {}
        _Frequency ("Tiling", Float) = 1
        _Visibility ("Occlusion", 2D) = "white" {}
    }
    SubShader
    {
        Pass
        {
            Name "TRIPLANAR"
            HLSLPROGRAM
            #pragma vertex projectSurface
            #pragma fragment shadeSurface
            #pragma target 3.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ OCCLUSION_DISABLED
            #include "UnityCG.cginc"

            struct SurfaceValues
            {
                float3 surfaceNormal : TEXCOORD0;
                float3 objectCoordinates : TEXCOORD1;
                float2 uv : TEXCOORD2;
                float4 position : SV_POSITION;
            };
            sampler2D _Visibility;
            sampler2D _Albedo;
            float _Frequency;

            SurfaceValues projectSurface(float4 position : POSITION,
                              float3 normal : NORMAL, float2 uv : TEXCOORD0)
            {
                SurfaceValues output;
                output.position = UnityObjectToClipPos(position);
                output.objectCoordinates = position.xyz * _Frequency;
                output.surfaceNormal = normal;
                output.uv = uv;
                return output;
            }

            float4 shadeSurface(SurfaceValues input) : SV_Target
            {
                float3 contribution = abs(input.surfaceNormal);
                contribution /= dot(contribution, float3(1, 1, 1));
                float4 alongX = tex2D(_Albedo, input.objectCoordinates.yz);
                float4 alongY = tex2D(_Albedo, input.objectCoordinates.xz);
                float4 alongZ = tex2D(_Albedo, input.objectCoordinates.xy);
                float4 color = alongX * contribution.x
                             + alongY * contribution.y
                             + alongZ * contribution.z;
                #if !defined(OCCLUSION_DISABLED)
                    color *= tex2D(_Visibility, input.uv);
                #endif
                return color;
            }
            ENDHLSL
        }
    }
}
