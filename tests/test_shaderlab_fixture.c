// SPDX-License-Identifier: GPL-3.0-only

#include "test_shaderlab_fixture.h"
#include "io/subprogram_metadata.h"
#include "dxbc/dxbc_hash.h"
#include "common/file_io.h"
#include "dxbc/usbd.h"
#include <stdlib.h>
#include <string.h>

static void write_u32_le(uint8_t *bytes, size_t *cursor, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[(*cursor)++] = (uint8_t)(value >> (i * 8));
}

uint8_t *test_shaderlab_empty_parameters_blob(bool globals_shell, size_t *out_size) {
    if (!out_size) return NULL;
    *out_size = 0;
    const size_t size = globals_shell ? 28u : 12u;
    uint8_t *bytes = calloc(size, 1);
    if (!bytes) return NULL;
    size_t cursor = 0;
    write_u32_le(bytes, &cursor, UNITY_2021_3_PLAYER_BLOB_VERSION);
    write_u32_le(bytes, &cursor, globals_shell ? 1u : 0u);
    if (globals_shell) {
        write_u32_le(bytes, &cursor, 0u); /* Empty loose name projects to $Globals. */
        write_u32_le(bytes, &cursor, 0u); /* Shell size. */
        write_u32_le(bytes, &cursor, 0u); /* Variables. */
        write_u32_le(bytes, &cursor, 0u); /* Structures. */
    }
    write_u32_le(bytes, &cursor, 0u); /* Resources. */
    if (cursor != size) { free(bytes); return NULL; }
    *out_size = size;
    return bytes;
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

#define FIXTURE_REQUIRE(condition) do { if (!(condition)) { \
    goto failure; } } while (0)

bool test_shaderlab_matrix_fixture_init(TestShaderLabMatrixFixture *fixture,
    const char *pixel_path, bool builtin) {
    memset(fixture, 0, sizeof(*fixture));
    CommonFileBytes bytes = {0};
    size_t size;
    uint8_t *vertex = test_shaderlab_matrix_vertex_dxbc(&size);
    FIXTURE_REQUIRE(vertex);
    size_t payload_size = 0;
    fixture->segments[0] = test_shaderlab_variant_blob(vertex, size, 16, NULL, &payload_size);
    free(vertex);
    FIXTURE_REQUIRE(fixture->segments[0] && payload_size <= INT32_MAX);
    fixture->lengths[0] = (int)payload_size;
    FIXTURE_REQUIRE(common_file_read_regular(pixel_path, 1024 * 1024, &bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    DXBCUSBDRecordView pixel;
    FIXTURE_REQUIRE(dxbc_usbd_table_open(&table, bytes.data, bytes.size, NULL));
    FIXTURE_REQUIRE(dxbc_usbd_table_record(&table, 1, &pixel));
    fixture->segments[1] = test_shaderlab_variant_blob(pixel.dxbc, pixel.dxbc_size, 17, NULL, &payload_size);
    common_file_bytes_dispose(&bytes);
    FIXTURE_REQUIRE(fixture->segments[1] && payload_size <= INT32_MAX);
    fixture->lengths[1] = (int)payload_size;
    for (int stage = 0; stage < 2; ++stage) {
        fixture->entries[stage] = (BlobEntry){0, fixture->lengths[stage], stage};
        fixture->programs[stage] = (SerializedSubProgram){.blob_index = stage,
            .program_type = stage ? 17 : 16, .shader_requirements = 0xe3};
        fixture->identities[stage].hardware_tier_group = 3;
        fixture->pass.subprogram_count[stage] = 1;
        fixture->pass.subprograms[stage] = &fixture->programs[stage];
        fixture->pass.subprogram_identities[stage] = &fixture->identities[stage];
    }
    fixture->matrix = (SerializedVariable){.name = builtin ? "unity_ObjectToWorld" : "ObjectTransform", .layout = {0, 4, 4, 1, 0, 0}};
    fixture->buffer = (SerializedConstantBuffer){.name = builtin ? "UnityPerDraw" : "Matrices", .size = builtin ? 176 : 64,
        .var_count = 1, .variables = &fixture->matrix};
    fixture->binding = (SerializedResourceParam){.name = builtin ? "UnityPerDraw" : "Matrices",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .bind_index = 0};
    fixture->pass.common_parameters[0] = (SerializedProgramParameters){.is_binary = true,
        .cb_count = 1, .constant_buffers = &fixture->buffer, .res_count = 1, .resources = &fixture->binding};
    fixture->platform = 4;
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = &fixture->platform;
    fixture->pass.program_mask = 6;
    fixture->subshader = (SerializedSubShader){.pass_count = 1, .passes = &fixture->pass};
    fixture->shader = (SerializedShader){.name = "Fixture/Quality/MatrixUses",
        .subshader_count = 1, .subshaders = &fixture->subshader};
    fixture->archive = (ShaderBlobArchive){.entries = fixture->entries, .entry_count = 2,
        .segments = fixture->segments, .segment_lengths = fixture->lengths, .segment_count = 2};
    return true;
failure:
    common_file_bytes_dispose(&bytes);
    test_shaderlab_matrix_fixture_dispose(fixture);
    return false;
}

void test_shaderlab_matrix_fixture_dispose(TestShaderLabMatrixFixture *fixture) {
    free(fixture->segments[0]);
    free(fixture->segments[1]);
    memset(fixture, 0, sizeof(*fixture));
}


#undef FIXTURE_REQUIRE
