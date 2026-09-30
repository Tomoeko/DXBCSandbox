// SPDX-License-Identifier: GPL-3.0-only
// Linked control-point, patch-constant and barycentric domain stages.
Shader "Fixture/HighLevel/TessellationTriangle"
{
    Properties
    {
        _Factor ("Tessellation", Float) = 2
        _Displacement ("Displacement", Float) = 0
    }
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma hull hull
            #pragma domain domain
            #pragma fragment frag
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma multi_compile __ REVERSE_TOPOLOGY
            #pragma multi_compile __ ADAPTIVE_FACTORS
            #include "UnityCG.cginc"

            struct ControlPoint { float3 position : INTERNALTESSPOS; };
            struct PatchData
            {
                float edges[3] : SV_TessFactor;
                float inside : SV_InsideTessFactor;
            };
            float _Factor;
            float _Displacement;
            ControlPoint vert(float4 position : POSITION)
            {
                ControlPoint output;
                output.position = position.xyz;
                return output;
            }
            PatchData PatchConstants(InputPatch<ControlPoint, 3> patch)
            {
                PatchData output;
                #if defined(ADAPTIVE_FACTORS)
                    output.edges[0] = _Factor + length(patch[1].position - patch[2].position);
                    output.edges[1] = _Factor + length(patch[2].position - patch[0].position);
                    output.edges[2] = _Factor + length(patch[0].position - patch[1].position);
                    output.inside = max(output.edges[0], max(output.edges[1], output.edges[2]));
                #else
                    output.edges[0] = _Factor;
                    output.edges[1] = _Factor;
                    output.edges[2] = _Factor;
                    output.inside = _Factor;
                #endif
                return output;
            }
            [domain("tri")]
            [partitioning("integer")]
            #if defined(REVERSE_TOPOLOGY)
                [outputtopology("triangle_ccw")]
            #else
                [outputtopology("triangle_cw")]
            #endif
            [outputcontrolpoints(3)]
            [patchconstantfunc("PatchConstants")]
            ControlPoint hull(InputPatch<ControlPoint, 3> patch,
                              uint pointIndex : SV_OutputControlPointID)
            {
                return patch[pointIndex];
            }
            [domain("tri")]
            float4 domain(PatchData factors, const OutputPatch<ControlPoint, 3> patch,
                          float3 barycentric : SV_DomainLocation) : SV_POSITION
            {
                float3 position = patch[0].position * barycentric.x
                                + patch[1].position * barycentric.y
                                + patch[2].position * barycentric.z;
                position.y += _Displacement * barycentric.x * barycentric.y;
                return UnityObjectToClipPos(float4(position, 1));
            }
            float4 frag() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
            ENDHLSL
        }
    }
}
