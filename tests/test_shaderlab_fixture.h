// SPDX-License-Identifier: GPL-3.0-only

#ifndef TEST_SHADERLAB_FIXTURE_H
#define TEST_SHADERLAB_FIXTURE_H

#include <stdint.h>
#include <stddef.h>

/* Synthetic Unity player wrapper; caller owns the returned malloc allocation. */
uint8_t *test_shaderlab_variant_blob(const uint8_t *dxbc, size_t dxbc_size, int32_t program_type,
                                     const char *keyword, size_t *out_size);
/* Same wrapper with an ordered keyword list for exhaustive bounded domains. */
uint8_t *test_shaderlab_variant_blob_keywords(const uint8_t *dxbc, size_t dxbc_size,
    int32_t program_type, const char *const *keywords, size_t keyword_count, size_t *out_size);

/* Authored single-matrix vertex tokens; no captured private byte array. */
uint8_t *test_shaderlab_matrix_vertex_dxbc(size_t *out_size);

#endif
