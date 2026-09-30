// SPDX-License-Identifier: GPL-3.0-only
#include "test_geometry_fixture.h"
#include "dxbc/dxbc_hash.h"
#include <stdlib.h>
#include <string.h>

#define INSTRUCTION(opcode, length) ((uint32_t)(opcode) | (uint32_t)(length) << 24u)

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = (uint8_t)(value >> (8 * index));
}

static size_t write_signature(uint8_t *bytes, bool output, uint8_t color_mask) {
    memcpy(bytes, output ? "OSGN" : "ISGN", 4);
    const size_t payload_size = 8 + 2 * 24 + sizeof("SV_POSITION") + sizeof("COLOR");
    write_u32(bytes + 4, (uint32_t)payload_size);
    uint8_t *payload = bytes + 8;
    write_u32(payload, 2);
    write_u32(payload + 4, 8);
    for (unsigned index = 0; index < 2; ++index) {
        uint8_t *element = payload + 8 + index * 24;
        write_u32(element, index ? 56 + (uint32_t)sizeof("SV_POSITION") : 56);
        write_u32(element + 4, 0);
        write_u32(element + 8, index ? 0 : 1);
        write_u32(element + 12, 3);
        write_u32(element + 16, index);
        const uint32_t mask = index ? color_mask : 15u;
        write_u32(element + 20, output ? mask | ((15u & ~mask) << 8u) : mask | mask << 8u);
    }
    memcpy(payload + 56, "SV_POSITION", sizeof("SV_POSITION"));
    memcpy(payload + 56 + sizeof("SV_POSITION"), "COLOR", sizeof("COLOR"));
    return 8 + payload_size;
}

/* Controlled raw tokens exercise the same document/signature/contract/USIL
 * path as retained compiler targets. Different signature registers share
 * vertex index0, catching accidental v0[1] -> position remapping. */
uint8_t *test_geometry_dxbc_primitive(bool explicit_stream, bool arithmetic,
    DXBCInputPrimitive primitive, uint32_t vertices, uint32_t selected_vertex,
    uint8_t color_mask, size_t *out_size) {
    if (!out_size) return NULL;
    *out_size = 0;
    uint32_t words[96];
    size_t count = 0;
#define WORD(value) words[count++] = (value)
    WORD(INSTRUCTION(97, 5)); WORD(UINT32_C(0x002010f2)); WORD(vertices); WORD(0); WORD(1);
    WORD(INSTRUCTION(95, 4)); WORD(UINT32_C(0x00201002) | (uint32_t)color_mask << 4u); WORD(vertices); WORD(1);
    WORD(INSTRUCTION(93, 1) | (uint32_t)primitive << 11u);
    if (explicit_stream) {
        WORD(INSTRUCTION(143, 3)); WORD(UINT32_C(0x00110000)); WORD(0);
    }
    WORD(INSTRUCTION(92, 1) | 5u << 11u);
    WORD(INSTRUCTION(103, 4)); WORD(UINT32_C(0x001020f2)); WORD(0); WORD(1);
    WORD(INSTRUCTION(101, 3)); WORD(UINT32_C(0x00102002) | (uint32_t)color_mask << 4u); WORD(1);
    WORD(INSTRUCTION(94, 2)); WORD(2);
    WORD(INSTRUCTION(54, 6)); WORD(UINT32_C(0x001020f2)); WORD(0);
    WORD(UINT32_C(0x00201e46)); WORD(selected_vertex); WORD(0);
    WORD(INSTRUCTION(54, 6)); WORD(UINT32_C(0x00102002) | (uint32_t)color_mask << 4u); WORD(1);
    WORD(UINT32_C(0x00201e46)); WORD(selected_vertex); WORD(1);
    if (explicit_stream) {
        WORD(INSTRUCTION(117, 3)); WORD(UINT32_C(0x00110000)); WORD(0);
        WORD(INSTRUCTION(118, 3)); WORD(UINT32_C(0x00110000)); WORD(0);
    } else {
        WORD(INSTRUCTION(19, 1)); WORD(INSTRUCTION(9, 1));
    }
    WORD(INSTRUCTION(arithmetic ? 56 : 54, arithmetic ? 8 : 6));
    WORD(UINT32_C(0x001020f2)); WORD(0);
    WORD(UINT32_C(0x00201e46)); WORD(selected_vertex); WORD(0);
    if (arithmetic) { WORD(UINT32_C(0x00004001)); WORD(UINT32_C(0x3fc00000)); }
    if (explicit_stream) {
        WORD(INSTRUCTION(117, 3)); WORD(UINT32_C(0x00110000)); WORD(0);
        WORD(INSTRUCTION(118, 3)); WORD(UINT32_C(0x00110000)); WORD(0);
    } else {
        WORD(INSTRUCTION(19, 1)); WORD(INSTRUCTION(9, 1));
    }
    WORD(INSTRUCTION(62, 1));
#undef WORD
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    write_u32(bytes + 20, 1);
    write_u32(bytes + 28, 3);
    size_t offset = 44;
    for (unsigned signature = 0; signature < 2; ++signature) {
        write_u32(bytes + 32 + signature * 4, (uint32_t)offset);
        offset += write_signature(bytes + offset, signature != 0, color_mask);
        offset = (offset + 3u) & ~(size_t)3u;
    }
    write_u32(bytes + 40, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    write_u32(bytes + offset + 4, (uint32_t)(count + 2) * 4);
    write_u32(bytes + offset + 8, explicit_stream ? UINT32_C(0x00020050) : UINT32_C(0x00020040));
    write_u32(bytes + offset + 12, (uint32_t)count + 2);
    for (size_t index = 0; index < count; ++index)
        write_u32(bytes + offset + 16 + index * 4, words[index]);
    const size_t size = offset + 16 + count * 4;
    write_u32(bytes + 24, (uint32_t)size);
    if (!dxbc_compute_hash(bytes, size, bytes + 4)) return NULL;
    uint8_t *result = malloc(size);
    if (!result) return NULL;
    memcpy(result, bytes, size);
    *out_size = size;
    return result;
}

uint8_t *test_geometry_dxbc(bool explicit_stream, bool arithmetic, size_t *out_size) {
    return test_geometry_dxbc_primitive(explicit_stream, arithmetic,
        DXBC_INPUT_PRIMITIVE_POINT, 1, 0, 15, out_size);
}
