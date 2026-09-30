// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input. Reconstruction must consume the released artifact.
Shader "Fixture/HighLevel/SingleColor"
{
    Properties
    {
        _Color ("Main Color", Color) = (1, 1, 1, 1)
        _Scale ("Color Scale", Float) = 1
    }
    SubShader
    {
        Pass
        {
            Name "COLOR"
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ COLOR_SCALE
            #include "UnityCG.cginc"

            float4 _Color;
            float _Scale;

            float4 vert(float4 vertex : POSITION) : SV_POSITION
            {
                return UnityObjectToClipPos(vertex);
            }

            float4 frag() : SV_Target
            {
                #if defined(COLOR_SCALE)
                    return _Color * _Scale;
                #else
                    return _Color;
                #endif
            }
            ENDHLSL
        }
    }
}
