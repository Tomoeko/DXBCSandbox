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
    return test_shaderlab_variant_blob_keywords(dxbc, dxbc_size, program_type,
        keyword ? &keyword : NULL, keyword ? 1 : 0, out_size);
}

uint8_t *test_shaderlab_variant_blob_keywords(const uint8_t *dxbc, size_t dxbc_size,
    int32_t program_type, const char *const *keywords, size_t keyword_count, size_t *out_size) {
    if (!dxbc || !out_size || dxbc_size > UINT32_MAX || keyword_count > UINT32_MAX ||
        (keyword_count && !keywords))
        return NULL;
    const size_t padded_dxbc_size = (dxbc_size + 3u) & ~(size_t)3u;
    if (padded_dxbc_size < dxbc_size || padded_dxbc_size > SIZE_MAX - 40u)
        return NULL;
    size_t size = 40u + padded_dxbc_size;
    for (size_t index = 0; index < keyword_count; ++index) {
        if (!keywords[index]) return NULL;
        const size_t length = strlen(keywords[index]);
        if (length > UINT32_MAX || length > SIZE_MAX - 3u) return NULL;
        const size_t padded = (length + 3u) & ~(size_t)3u;
        if (padded > SIZE_MAX - 4u || size > SIZE_MAX - 4u - padded) return NULL;
        size += 4u + padded;
    }
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
    write_u32_le(blob, &cursor, (uint32_t)keyword_count);
    for (size_t index = 0; index < keyword_count; ++index) {
        const size_t length = strlen(keywords[index]);
        write_u32_le(blob, &cursor, (uint32_t)length);
        memcpy(blob + cursor, keywords[index], length);
        cursor += (length + 3u) & ~(size_t)3u;
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
