Shader "DXBCSandbox/ImportGateInvalidSincos"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma target 3.0
            #pragma vertex vert
            #pragma fragment frag

            float4 vert(float4 position : POSITION) : SV_POSITION
            {
                float2 sine_value;
                float cosine_value;
                sincos(position.xy, sine_value, cosine_value);
                return position;
            }

            float4 frag() : SV_Target
            {
                return 1.0;
            }
            ENDHLSL
        }
    }
}
