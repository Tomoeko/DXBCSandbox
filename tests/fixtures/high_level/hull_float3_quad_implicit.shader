// SPDX-License-Identifier: GPL-3.0-only
// Constant-factor FLOAT3 interface; other stages are authored evaluation stubs.
Shader "Fixture/HighLevel/HullFloat3QuadImplicit"
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
                float edges[4] : SV_TessFactor;
                float inside[2] : SV_InsideTessFactor;
            };
            Point vert(float4 input : POSITION)
            {
                Point result;
                result.value = input.xyz;
                return result;
            }
            Factors PatchFactors(InputPatch<Point, 4> patch)
            {
                Factors result;
                result.edges[0] = 3.0f;
                result.edges[1] = 3.0f;
                result.edges[2] = 3.0f;
                result.edges[3] = 3.0f;
                result.inside[0] = 4.0f;
                result.inside[1] = 4.0f;
                return result;
            }
            [domain("quad")]
            [partitioning("integer")]
            [outputtopology("triangle_cw")]
            [outputcontrolpoints(4)]
            [patchconstantfunc("PatchFactors")]
            [maxtessfactor(32.0f)]
            Point hull(InputPatch<Point, 4> patch, uint index : SV_OutputControlPointID)
            {
                return patch[index];
            }
            [domain("quad")]
            float4 domain(Factors factors, const OutputPatch<Point, 4> patch,
                          float2 weights : SV_DomainLocation) : SV_POSITION
            {
                float3 lower = patch[0].value * (1.0f - weights.x)
                             + patch[1].value * weights.x;
                float3 upper = patch[2].value * (1.0f - weights.x)
                             + patch[3].value * weights.x;
                float3 value = lower * (1.0f - weights.y) + upper * weights.y;
                return float4(value, 1.0f);
            }
            float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
            ENDHLSL
        }
    }
}
