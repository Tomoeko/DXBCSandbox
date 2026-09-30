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
uint8_t *test_tessellation_domain_dxbc(unsigned domain, uint32_t points,
    uint8_t location_mask, size_t *size);
/* Writes the existing fixed signature grammar into a sufficiently sized test
 * buffer; this helper is also used by hull quad/isoline token authors. */
size_t test_tessellation_hull_signature(uint8_t *bytes, unsigned role, bool inner_first);

#endif
