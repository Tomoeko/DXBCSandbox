// SPDX-License-Identifier: GPL-3.0-only

#include "test_shaderlab_fixture.h"
#include "io/subprogram_metadata.h"
#include "dxbc/dxbc_hash.h"
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

static void matrix_u32(uint8_t *bytes, uint32_t value) {
    size_t cursor = 0;
    write_u32_le(bytes, &cursor, value);
}

static size_t matrix_signature(uint8_t *bytes, bool output) {
    const char *semantic = output ? "SV_POSITION" : "POSITION";
    const size_t payload_size = 32 + strlen(semantic) + 1;
    memcpy(bytes, output ? "OSGN" : "ISGN", 4);
    matrix_u32(bytes + 4, (uint32_t)payload_size);
    uint8_t *payload = bytes + 8;
    matrix_u32(payload, 1);
    matrix_u32(payload + 4, 8);
    matrix_u32(payload + 8, 32);
    matrix_u32(payload + 12, 0);
    matrix_u32(payload + 16, output ? 1 : 0);
    matrix_u32(payload + 20, 3);
    matrix_u32(payload + 24, 0);
    matrix_u32(payload + 28, output ? 15 : 0x0f0f);
    memcpy(payload + 32, semantic, strlen(semantic) + 1);
    return 8 + payload_size;
}

uint8_t *test_shaderlab_matrix_vertex_dxbc(size_t *out_size) {
    if (!out_size) return NULL;
    uint32_t words[80];
    size_t count = 0;
#define MATRIX_WORD(value) words[count++] = (value)
#define MATRIX_INSTRUCTION(opcode, length) ((uint32_t)(opcode) | (uint32_t)(length) << 24u)
    MATRIX_WORD(MATRIX_INSTRUCTION(95, 3)); MATRIX_WORD(UINT32_C(0x001010f2)); MATRIX_WORD(0);
    MATRIX_WORD(MATRIX_INSTRUCTION(103, 4)); MATRIX_WORD(UINT32_C(0x001020f2)); MATRIX_WORD(0); MATRIX_WORD(1);
    MATRIX_WORD(MATRIX_INSTRUCTION(89, 4)); MATRIX_WORD(UINT32_C(0x00208000)); MATRIX_WORD(0); MATRIX_WORD(4);
    MATRIX_WORD(MATRIX_INSTRUCTION(104, 2)); MATRIX_WORD(1);
    const unsigned components[4] = {1, 0, 2, 3};
    for (unsigned index = 0; index < 4; ++index) {
        MATRIX_WORD(MATRIX_INSTRUCTION(index ? 50 : 56, index ? 10 : 8));
        MATRIX_WORD(index == 3 ? UINT32_C(0x001020f2) : UINT32_C(0x001000f2));
        MATRIX_WORD(0);
        if (!index) {
            MATRIX_WORD(UINT32_C(0x0010100a) | components[index] << 4u); MATRIX_WORD(0);
            MATRIX_WORD(UINT32_C(0x00208e46)); MATRIX_WORD(0); MATRIX_WORD(components[index]);
        } else {
            MATRIX_WORD(UINT32_C(0x00208e46)); MATRIX_WORD(0); MATRIX_WORD(components[index]);
            MATRIX_WORD(UINT32_C(0x0010100a) | components[index] << 4u); MATRIX_WORD(0);
            MATRIX_WORD(UINT32_C(0x00100e46)); MATRIX_WORD(0);
        }
    }
    MATRIX_WORD(MATRIX_INSTRUCTION(62, 1));
#undef MATRIX_WORD
#undef MATRIX_INSTRUCTION
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    matrix_u32(bytes + 20, 1);
    matrix_u32(bytes + 28, 3);
    size_t offset = 44;
    for (unsigned signature = 0; signature < 2; ++signature) {
        matrix_u32(bytes + 32 + signature * 4, (uint32_t)offset);
        offset += matrix_signature(bytes + offset, signature != 0);
        offset = (offset + 3u) & ~(size_t)3u;
    }
    matrix_u32(bytes + 40, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    matrix_u32(bytes + offset + 4, (uint32_t)(count + 2) * 4);
    matrix_u32(bytes + offset + 8, UINT32_C(0x00010050));
    matrix_u32(bytes + offset + 12, (uint32_t)count + 2);
    for (size_t index = 0; index < count; ++index) matrix_u32(bytes + offset + 16 + index * 4, words[index]);
    size_t size = offset + 16 + count * 4;
    matrix_u32(bytes + 24, (uint32_t)size);
    if (!dxbc_compute_hash(bytes, size, bytes + 4)) return NULL;
    uint8_t *result = malloc(size);
    if (!result) return NULL;
    memcpy(result, bytes, size);
    *out_size = size;
    return result;
}
