// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input for linked tessellation stage contracts.
Shader "Fixture/HighLevel/TessellationQuad" {
    Properties { _Factor ("Factor", Float) = 2 _Displacement ("Displacement", Float) = 0 }
    SubShader { Pass { HLSLPROGRAM
            #pragma vertex vert
            #pragma hull hull
            #pragma domain domain
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            #include "UnityCG.cginc"
            struct ControlPoint { float3 position : INTERNALTESSPOS; };
            float _Factor;
            float _Displacement;
            ControlPoint vert(float4 position : POSITION) {
                ControlPoint result; result.position = position.xyz; return result;
            }
            float4 frag() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
            struct PatchData {
                float edges[4] : SV_TessFactor;
                float inside[2] : SV_InsideTessFactor;
            };
            PatchData PatchConstants(InputPatch<ControlPoint, 4> patch) {
                PatchData result;
                for (uint index = 0; index < 4; ++index) result.edges[index] = _Factor;
                result.inside[0] = _Factor; result.inside[1] = _Factor;
                return result;
            }
            [domain("quad")]
            [partitioning("fractional_even")]
            [outputtopology("triangle_cw")]
            [outputcontrolpoints(4)]
            [patchconstantfunc("PatchConstants")]
            ControlPoint hull(InputPatch<ControlPoint, 4> patch,
                              uint index : SV_OutputControlPointID) {
                return patch[index];
            }
            [domain("quad")]
            float4 domain(PatchData factors, const OutputPatch<ControlPoint, 4> patch,
                          float2 uv : SV_DomainLocation) : SV_POSITION {
                float3 bottom = lerp(patch[0].position, patch[1].position, uv.x);
                float3 top = lerp(patch[2].position, patch[3].position, uv.x);
                float3 position = lerp(bottom, top, uv.y);
                position.y += _Displacement * uv.x * uv.y;
                return UnityObjectToClipPos(float4(position, 1));
            }
    ENDHLSL } }
}
