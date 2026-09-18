#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_hash.h"

#include "common/common.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static void write_u32_le(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t instruction_token(uint32_t opcode, uint32_t length) {
    return opcode | (length << 24U);
}

static size_t build_container(uint8_t* bytes, size_t capacity,
                              const char fourcc[4], uint32_t program_version,
                              const uint32_t* instructions,
                              size_t instruction_word_count,
                              uint32_t chunk_offset, uint8_t gap_fill) {
    if (!bytes || !fourcc || !instructions || instruction_word_count == 0U ||
        instruction_word_count > UINT32_MAX - 2U || chunk_offset < 36U ||
        (chunk_offset & 3U) != 0U) {
        return 0U;
    }
    const size_t payload_size = (2U + instruction_word_count) * 4U;
    const size_t total_size = (size_t)chunk_offset + 8U + payload_size;
    if (total_size > capacity || total_size > UINT32_MAX) return 0U;

    memset(bytes, gap_fill, total_size);
    memcpy(bytes, "DXBC", 4U);
    memset(bytes + 4U, 0, 16U);
    write_u32_le(bytes + 20U, 1U);
    write_u32_le(bytes + 24U, (uint32_t)total_size);
    write_u32_le(bytes + 28U, 1U);
    write_u32_le(bytes + 32U, chunk_offset);
    memcpy(bytes + chunk_offset, fourcc, 4U);
    write_u32_le(bytes + chunk_offset + 4U, (uint32_t)payload_size);
    write_u32_le(bytes + chunk_offset + 8U, program_version);
    write_u32_le(bytes + chunk_offset + 12U,
                 (uint32_t)(2U + instruction_word_count));
    for (size_t index = 0U; index < instruction_word_count; ++index) {
        write_u32_le(bytes + chunk_offset + 16U + index * 4U,
                     instructions[index]);
    }
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, total_size, hash)) return 0U;
    memcpy(bytes + 4U, hash, sizeof(hash));
    return total_size;
}

static int verify_exact_and_instruction_localization(void) {
    uint8_t expected[128];
    uint8_t actual[128];
    const uint32_t ret[] = {instruction_token(62U, 1U)};
    size_t expected_size = build_container(expected, sizeof(expected), "SHDR",
                                           UINT32_C(0x00000050), ret, 1U,
                                           36U, 0U);
    CHECK(expected_size != 0U);
    memcpy(actual, expected, expected_size);

    DXBCCompareResult result;
    CHECK(dxbc_compare_exact(expected, expected_size, actual, expected_size,
                             &result) == DXBC_COMPARE_EQUAL);
    CHECK(result.first_differing_byte == SIZE_MAX);

    const uint32_t nop[] = {instruction_token(58U, 1U)};
    size_t actual_size = build_container(actual, sizeof(actual), "SHDR",
                                         UINT32_C(0x00000050), nop, 1U, 36U,
                                         0U);
    CHECK(actual_size == expected_size);
    CHECK(dxbc_compare_exact(expected, expected_size, actual, actual_size,
                             &result) ==
          DXBC_COMPARE_INSTRUCTION_OPCODE);
    CHECK(result.first_differing_byte == 52U);
    CHECK(result.chunk_index == 0U);
    CHECK(result.instruction_index == 0U);
    CHECK(result.token_index == 0U);
    CHECK(result.expected_value == 62U);
    CHECK(result.actual_value == 58U);

    const uint32_t flagged_ret[] = {
        instruction_token(62U, 1U) | UINT32_C(0x00002000)};
    actual_size = build_container(actual, sizeof(actual), "SHDR",
                                  UINT32_C(0x00000050), flagged_ret, 1U, 36U,
                                  0U);
    CHECK(actual_size == expected_size);
    CHECK(dxbc_compare_exact(expected, expected_size, actual, actual_size,
                             &result) == DXBC_COMPARE_INSTRUCTION_TOKEN);
    CHECK(result.first_differing_byte == 52U);
    CHECK(result.expected_value == ret[0]);
    CHECK(result.actual_value == flagged_ret[0]);
    CHECK(strcmp(dxbc_compare_status_name(result.status),
                 "instruction_token") == 0);
    return 0;
}

static int verify_structural_localization(void) {
    uint8_t expected[160];
    uint8_t actual[160];
    const uint32_t ret[] = {instruction_token(62U, 1U)};
    size_t expected_size = build_container(expected, sizeof(expected), "SHDR",
                                           UINT32_C(0x00000050), ret, 1U,
                                           36U, 0U);
    CHECK(expected_size != 0U);
    DXBCCompareResult result;

    size_t actual_size = build_container(actual, sizeof(actual), "SHDR",
                                         UINT32_C(0x00010050), ret, 1U, 36U,
                                         0U);
    CHECK(dxbc_compare_exact(expected, expected_size, actual, actual_size,
                             &result) == DXBC_COMPARE_PROGRAM_VERSION);
    CHECK(result.first_differing_byte == 44U);

    actual_size = build_container(actual, sizeof(actual), "SHEX",
                                  UINT32_C(0x00000050), ret, 1U, 36U, 0U);
    CHECK(dxbc_compare_exact(expected, expected_size, actual, actual_size,
                             &result) == DXBC_COMPARE_CHUNK_FOURCC);
    CHECK(result.first_differing_byte == 36U);

    const uint32_t two_instructions[] = {
        instruction_token(58U, 1U), instruction_token(62U, 1U)};
    actual_size = build_container(actual, sizeof(actual), "SHDR",
                                  UINT32_C(0x00000050), two_instructions, 2U,
                                  36U, 0U);
    CHECK(dxbc_compare_exact(expected, expected_size, actual, actual_size,
                             &result) == DXBC_COMPARE_INSTRUCTION_COUNT);
    CHECK(result.expected_value == 1U);
    CHECK(result.actual_value == 2U);

    memcpy(actual, expected, expected_size);
    actual[52U] ^= 1U;
    CHECK(dxbc_compare_exact(expected, expected_size, actual, expected_size,
                             &result) == DXBC_COMPARE_ACTUAL_INVALID);
    CHECK(result.actual_diagnostic.code == DXBC_DOCUMENT_HASH_MISMATCH);
    return 0;
}

static int verify_payload_and_padding_localization(void) {
    uint8_t expected[160];
    uint8_t actual[160];
    const uint32_t payload[] = {UINT32_C(0x01020304)};
    size_t expected_size = build_container(expected, sizeof(expected), "ISGN",
                                           0U, payload, 1U, 36U, 0U);
    const uint32_t changed_payload[] = {UINT32_C(0x01020305)};
    size_t actual_size = build_container(actual, sizeof(actual), "ISGN", 0U,
                                         changed_payload, 1U, 36U, 0U);
    CHECK(expected_size != 0U && actual_size == expected_size);

    DXBCCompareResult result;
    CHECK(dxbc_compare_exact(expected, expected_size, actual, actual_size,
                             &result) == DXBC_COMPARE_CHUNK_PAYLOAD);
    CHECK(result.chunk_index == 0U);
    CHECK(result.first_differing_byte == 52U);

    expected_size = build_container(expected, sizeof(expected), "SHDR",
                                    UINT32_C(0x00000050), payload, 1U, 40U,
                                    0x11U);
    actual_size = build_container(actual, sizeof(actual), "SHDR",
                                  UINT32_C(0x00000050), payload, 1U, 40U,
                                  0x22U);
    CHECK(expected_size != 0U && actual_size == expected_size);
    CHECK(dxbc_compare_exact(expected, expected_size, actual, actual_size,
                             &result) == DXBC_COMPARE_RAW_BYTE);
    CHECK(result.first_differing_byte == 36U);
    CHECK(result.expected_value == 0x11U);
    CHECK(result.actual_value == 0x22U);
    return 0;
}

int main(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    CHECK(verify_exact_and_instruction_localization() == 0);
    CHECK(verify_structural_localization() == 0);
    CHECK(verify_payload_and_padding_localization() == 0);
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    puts("DXBC exact-comparison unit tests passed");
    return 0;
}
