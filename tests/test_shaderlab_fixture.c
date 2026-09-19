// SPDX-License-Identifier: GPL-3.0-only

#include "test_shaderlab_fixture.h"
#include "io/subprogram_metadata.h"
#include <stdlib.h>
#include <string.h>

static void write_u32_le(uint8_t *bytes, size_t *cursor, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[(*cursor)++] = (uint8_t)(value >> (i * 8));
}

uint8_t *test_shaderlab_variant_blob(const uint8_t *dxbc, size_t dxbc_size, int32_t program_type,
                                     const char *keyword, size_t *out_size) {
    if (!dxbc || !out_size || dxbc_size > UINT32_MAX)
        return NULL;
    const size_t keyword_size = keyword ? strlen(keyword) : 0u;
    if (keyword_size > UINT32_MAX)
        return NULL;
    const size_t padded_keyword_size = (keyword_size + 3u) & ~(size_t)3u;
    const size_t padded_dxbc_size = (dxbc_size + 3u) & ~(size_t)3u;
    if (padded_keyword_size < keyword_size || padded_dxbc_size < dxbc_size ||
        padded_keyword_size > SIZE_MAX - padded_dxbc_size - 44u) {
        return NULL;
    }
    const size_t size = 40u + (keyword ? 4u + padded_keyword_size : 0u) + padded_dxbc_size;
    uint8_t *blob = (uint8_t *)calloc(size, 1u);
    if (!blob)
        return NULL;
    size_t cursor = 0;
    write_u32_le(blob, &cursor, UNITY_2021_3_PLAYER_BLOB_VERSION);
    write_u32_le(blob, &cursor, (uint32_t)program_type);
    write_u32_le(blob, &cursor, 0u);
    write_u32_le(blob, &cursor, 0u);
    write_u32_le(blob, &cursor, 2u);
    write_u32_le(blob, &cursor, 0u);
    write_u32_le(blob, &cursor, keyword ? 1u : 0u);
    if (keyword) {
        write_u32_le(blob, &cursor, (uint32_t)keyword_size);
        memcpy(blob + cursor, keyword, keyword_size);
        cursor += padded_keyword_size;
    }
    write_u32_le(blob, &cursor, (uint32_t)dxbc_size);
    memcpy(blob + cursor, dxbc, dxbc_size);
    cursor += padded_dxbc_size;
    write_u32_le(blob, &cursor, 0u);
    write_u32_le(blob, &cursor, 0u);
    if (cursor != size) {
        free(blob);
        return NULL;
    }
    *out_size = size;
    return blob;
}
