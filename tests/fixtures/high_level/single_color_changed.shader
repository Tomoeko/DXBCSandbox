// SPDX-License-Identifier: GPL-3.0-only
// Semantic negative: the fragment color contains a real scale operation.
Shader "Fixture/HighLevel/ChangedColor"
{
    Properties { _Color ("Main Color", Color) = (1, 1, 1, 1) }
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma only_renderers d3d11
            #include "UnityCG.cginc"
            float4 _Color;
            float4 vert(float4 vertex : POSITION) : SV_POSITION
            {
                return UnityObjectToClipPos(vertex);
            }
            float4 frag() : SV_Target { return _Color * 0.5; }
            ENDHLSL
        }
    }
}
