// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_GEOMETRY_FIXTURE_H
#define TEST_GEOMETRY_FIXTURE_H
#include "dxbc/dxbc_stage_contract.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Owned controlled DXBC with parsed point input, two natural float4 fields,
 * two ordered stream appends/restarts, and a final return. */
uint8_t *test_geometry_dxbc(bool explicit_stream, bool arithmetic, size_t *out_size);
/* Test-only explicit primitive/field authority, including malformed extents. */
uint8_t *test_geometry_dxbc_primitive(bool explicit_stream, bool arithmetic,
    DXBCInputPrimitive primitive, uint32_t vertices, uint32_t selected_vertex,
    uint8_t color_mask, size_t *out_size);
#endif
