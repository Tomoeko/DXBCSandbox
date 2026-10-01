// SPDX-License-Identifier: GPL-3.0-only
// Constant-factor FLOAT3 interface; other stages are authored evaluation stubs.
Shader "Fixture/HighLevel/HullFloat3ExplicitControlPoint"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma target 5.0
            #pragma only_renderers d3d11
            #pragma vertex vert
            #pragma hull hull
            #pragma domain domain
            #pragma fragment frag

            struct Point { float3 value : POINTVALUE; };
            struct Factors
            {
                float edges[3] : SV_TessFactor;
                float inside : SV_InsideTessFactor;
            };
            Point vert(float4 input : POSITION)
            {
                Point result;
                result.value = input.xyz;
                return result;
            }
            Factors PatchFactors(InputPatch<Point, 3> patch)
            {
                Factors result;
                result.edges[0] = 3.0f;
                result.edges[1] = 3.0f;
                result.edges[2] = 3.0f;
                result.inside = 4.0f;
                return result;
            }
            [domain("tri")]
            [partitioning("integer")]
            [outputtopology("triangle_cw")]
            [outputcontrolpoints(3)]
            [patchconstantfunc("PatchFactors")]
            [maxtessfactor(32.0f)]
            Point hull(InputPatch<Point, 3> patch, uint index : SV_OutputControlPointID)
            {
                Point result;
                result.value = patch[index].value * 2.0f;
                return result;
            }
            [domain("tri")]
            float4 domain(Factors factors, const OutputPatch<Point, 3> patch,
                          float3 weights : SV_DomainLocation) : SV_POSITION
            {
                float3 value = patch[0].value * weights.x
                             + patch[1].value * weights.y
                             + patch[2].value * weights.z;
                return float4(value, 1.0f);
            }
            float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
            ENDHLSL
        }
    }
}
