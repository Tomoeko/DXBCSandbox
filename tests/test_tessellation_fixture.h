// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_TESSELLATION_FIXTURE_H
#define TEST_TESSELLATION_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Synthetic token grammar shared by stage and whole-source ownership tests.
 * Returned DXBC buffers are malloc-owned. These are authored tests, not copies
 * of captured compiler inputs. Scenario values retain the hull unit contract. */
uint8_t *test_tessellation_hull_dxbc(uint32_t input_points, uint32_t output_points,
    uint8_t scenario, size_t *size);
/* Custom FLOAT3 signature grammar for implicit control-point copies. */
uint8_t *test_tessellation_hull_float3_dxbc(uint32_t input_points, uint32_t output_points,
    uint8_t scenario, const char *semantic, size_t *size);
/* Existing implicit triangle grammar with one scalar b0 input and two final
 * factor clamps. The custom semantic applies to the FLOAT3 point signature;
 * FLOAT4 retains the existing SV_POSITION signature grammar. */
uint8_t *test_tessellation_hull_scalar_cbuffer_dxbc(uint32_t input_points,
    uint32_t output_points, bool float3, const char *semantic, size_t *size);
/* Exact existing authored quad/isoline grammar, shared without token parsing. */
uint8_t *test_tessellation_hull_shape_dxbc(bool isoline, size_t *size);
/* One parsed ICB declaration and one outer factor phase consumer. Rows select
 * isoline2/triangle3/quad4. Chain length0 uses actual ForkID; 1..3 authors
 * strictly earlier singleton MOVs with rotating physical lanes. Values supply
 * the selected column's exact raw bits; all unused DWORDs are positive zero.
 * FLOAT3 and the existing scalar-CB calibration are triangle-only. */
uint8_t *test_tessellation_hull_icb_dxbc(unsigned rows, unsigned column,
    unsigned chain_length, unsigned transport_lane, bool zero_base,
    bool scalar, bool float3, const uint32_t *values, size_t *size);
uint8_t *test_tessellation_domain_dxbc(unsigned domain, uint32_t points,
    uint8_t location_mask, size_t *size);
enum {
    TEST_DOMAIN_FLOAT3_VALID = 0,
    TEST_DOMAIN_FLOAT3_MISSING_W,
    TEST_DOMAIN_FLOAT3_OVERLAPPING_W,
    TEST_DOMAIN_FLOAT3_MISSING_Z,
    TEST_DOMAIN_FLOAT3_FOREIGN_OUTPUT,
    TEST_DOMAIN_FLOAT3_UNDECLARED_POINT_W,
    TEST_DOMAIN_FLOAT3_REORDERED_WRITES
};
/* Triangle custom FLOAT3 points with the measured MUL/MAD/MAD XYZ and scalar
 * MOV W/RET grammar. Scenarios author independently decoded boundary cases;
 * no compiler target bytes or serialized parameter authority are copied. */
uint8_t *test_tessellation_domain_float3_dxbc(uint32_t points,
    const char *semantic, unsigned scenario, size_t *size);
/* Shape-specific authored interpolation: triangle barycentric, quad bilinear,
 * or isoline linear. All retain independent MAD XYZ, scalar MOV W and RET
 * owners. The triangle wrapper above preserves its existing token bytes. */
uint8_t *test_tessellation_domain_float3_shape_dxbc(unsigned domain, uint32_t points,
    const char *semantic, unsigned scenario, size_t *size);
/* Writes the existing fixed signature grammar into a sufficiently sized test
 * buffer; this helper is also used by hull quad/isoline token authors. */
size_t test_tessellation_hull_signature(uint8_t *bytes, unsigned role, bool inner_first);

#endif
