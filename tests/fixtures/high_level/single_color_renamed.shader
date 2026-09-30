// SPDX-License-Identifier: GPL-3.0-only
// Same algorithm with renamed bindings, functions and reordered declarations.
Shader "Fixture/HighLevel/RenamedColor"
{
    Properties
    {
        _Gain ("Gain", Float) = 1
        _Tint ("Tint", Color) = (1, 1, 1, 1)
    }
    SubShader
    {
        Pass
        {
            Name "RENAMED_COLOR"
            HLSLPROGRAM
            #pragma vertex projectPosition
            #pragma fragment shadeTint
            #pragma target 3.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ COLOR_SCALE
            #include "UnityCG.cginc"

            float _Gain;
            float4 _Tint;

            float4 shadeTint() : SV_Target
            {
                #if defined(COLOR_SCALE)
                    return _Tint * _Gain;
                #else
                    return _Tint;
                #endif
            }

            float4 projectPosition(float4 objectPosition : POSITION) : SV_POSITION
            {
                return UnityObjectToClipPos(objectPosition);
            }
            ENDHLSL
        }
    }
}
