// SPDX-License-Identifier: GPL-3.0-only

#ifndef TEST_SHADERLAB_FIXTURE_H
#define TEST_SHADERLAB_FIXTURE_H

#include <stdint.h>
#include <stddef.h>

/* Synthetic Unity player wrapper; caller owns the returned malloc allocation. */
uint8_t *test_shaderlab_variant_blob(const uint8_t *dxbc, size_t dxbc_size, int32_t program_type,
                                     const char *keyword, size_t *out_size);

#endif
