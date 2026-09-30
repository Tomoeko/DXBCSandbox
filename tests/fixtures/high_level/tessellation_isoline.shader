// SPDX-License-Identifier: GPL-3.0-only
// Authored evaluation input for linked tessellation stage contracts.
Shader "Fixture/HighLevel/TessellationIsoline" {
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
            struct PatchData { float edges[2] : SV_TessFactor; };
            PatchData PatchConstants(InputPatch<ControlPoint, 2> patch) {
                PatchData result; result.edges[0] = 1; result.edges[1] = _Factor; return result;
            }
            [domain("isoline")]
            [partitioning("integer")]
            [outputtopology("line")]
            [outputcontrolpoints(2)]
            [patchconstantfunc("PatchConstants")]
            ControlPoint hull(InputPatch<ControlPoint, 2> patch,
                              uint index : SV_OutputControlPointID) {
                return patch[index];
            }
            [domain("isoline")]
            float4 domain(PatchData factors, const OutputPatch<ControlPoint, 2> patch,
                          float2 uv : SV_DomainLocation) : SV_POSITION {
                float3 position = lerp(patch[0].position, patch[1].position, uv.x);
                position.y += _Displacement * uv.x * (1 - uv.x);
                return UnityObjectToClipPos(float4(position, 1));
            }
    ENDHLSL } }
}
