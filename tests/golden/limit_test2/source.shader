Shader "DXBCSandbox/LimitTest2"
{
    SubShader
    {
        Pass
        {
            CGPROGRAM
            #pragma vertex tess_vert
            #pragma fragment frag
            #pragma hull hs
            #pragma domain ds
            #pragma target 5.0
            #include "UnityCG.cginc"

            sampler2D _DispTex;
            float4 _DispTex_ST;
            float _TessFactor;

            struct appdata {
                float4 vertex : POSITION;
                float3 normal : NORMAL;
                float2 uv : TEXCOORD0;
            };
            struct v2f {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
            };
            struct TessVertex {
                float4 vertex : INTERNALTESSPOS;
                float3 normal : NORMAL;
                float2 uv : TEXCOORD0;
            };
            struct TessFactors {
                float edge[3] : SV_TessFactor;
                float inside : SV_InsideTessFactor;
            };

            TessVertex tess_vert(appdata v)
            {
                TessVertex o;
                o.vertex = v.vertex;
                o.normal = v.normal;
                o.uv = v.uv;
                return o;
            }

            TessFactors patchConstantFunc(InputPatch<TessVertex, 3> patch)
            {
                TessFactors f;
                f.edge[0] = f.edge[1] = f.edge[2] = _TessFactor;
                f.inside = _TessFactor;
                return f;
            }

            [domain("tri")]
            [partitioning("fractional_odd")]
            [outputtopology("triangle_cw")]
            [patchconstantfunc("patchConstantFunc")]
            [outputcontrolpoints(3)]
            TessVertex hs(InputPatch<TessVertex, 3> patch,
                          uint id : SV_OutputControlPointID)
            {
                return patch[id];
            }

            [domain("tri")]
            v2f ds(TessFactors factors, OutputPatch<TessVertex, 3> patch,
                   float3 bary : SV_DomainLocation)
            {
                v2f o;
                float4 pos = patch[0].vertex * bary.x
                           + patch[1].vertex * bary.y
                           + patch[2].vertex * bary.z;
                float3 norm = patch[0].normal * bary.x
                            + patch[1].normal * bary.y
                            + patch[2].normal * bary.z;
                float2 uv = patch[0].uv * bary.x
                          + patch[1].uv * bary.y
                          + patch[2].uv * bary.z;
                float d = tex2Dlod(_DispTex, float4(uv, 0, 0)).r;
                pos.xyz += norm * d * 0.5;
                o.pos = UnityObjectToClipPos(pos);
                o.uv = uv;
                return o;
            }

            float4 frag(v2f i) : SV_Target
            {
                return float4(i.uv, 0, 1);
            }
            ENDCG
        }
    }
}
