#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define SYNTHETIC_SIZE 96u
#define UNKNOWN_OFFSET 40u
#define SHDR_OFFSET 52u

static void write_u32_le(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static bool refresh_hash(uint8_t* bytes, size_t size) {
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, size, hash)) return false;
    memcpy(bytes + 4, hash, sizeof(hash));
    return true;
}

static bool make_synthetic(uint8_t bytes[SYNTHETIC_SIZE]) {
    memset(bytes, 0, SYNTHETIC_SIZE);
    memcpy(bytes, "DXBC", 4);
    write_u32_le(bytes + 20, 1);
    write_u32_le(bytes + 24, SYNTHETIC_SIZE);
    write_u32_le(bytes + 28, 2);

    /* Offset-table order intentionally differs from physical chunk order. */
    write_u32_le(bytes + 32, SHDR_OFFSET);
    write_u32_le(bytes + 36, UNKNOWN_OFFSET);

    const uint8_t unknown_fourcc[4] = {'A', 'B', 0, 0xff};
    memcpy(bytes + UNKNOWN_OFFSET, unknown_fourcc, sizeof(unknown_fourcc));
    write_u32_le(bytes + UNKNOWN_OFFSET + 4, 3);
    bytes[UNKNOWN_OFFSET + 8] = 0xde;
    bytes[UNKNOWN_OFFSET + 9] = 0xad;
    bytes[UNKNOWN_OFFSET + 10] = 0xbe;
    bytes[UNKNOWN_OFFSET + 11] = 0x7e; /* preserved inter-chunk padding */

    memcpy(bytes + SHDR_OFFSET, "SHDR", 4);
    write_u32_le(bytes + SHDR_OFFSET + 4, 36);
    uint8_t* program = bytes + SHDR_OFFSET + 8;
    write_u32_le(program + 0, 0x00010050u); /* vs_5_0 */
    write_u32_le(program + 4, 9);           /* complete program DWORDs */

    /* One four-token instruction.  It has an extended-opcode token and an
     * extended operand token; the lossless slice must retain all four. */
    write_u32_le(program + 8, 0x84000036u);
    write_u32_le(program + 12, 0x00000001u);
    write_u32_le(program + 16, 0x80001001u);
    write_u32_le(program + 20, 0x12345678u);

    /* CUSTOMDATA carries its block length in the following DWORD. */
    write_u32_le(program + 24, 0x00000035u);
    write_u32_le(program + 28, 3);
    write_u32_le(program + 32, 0xcafebabeu);
    return refresh_hash(bytes, SYNTHETIC_SIZE);
}

static bool expect_parse_error(const uint8_t* bytes, size_t size,
                               DXBCDocumentDiagnosticCode expected_code) {
    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    CHECK(!dxbc_document_parse(&document, bytes, size, &diagnostic));
    CHECK(diagnostic.code == expected_code);
    CHECK(diagnostic.kind == DXBC_DOCUMENT_DIAGNOSTIC_VALIDATION ||
          diagnostic.kind == DXBC_DOCUMENT_DIAGNOSTIC_UNSUPPORTED);
    CHECK(strcmp(dxbc_document_diagnostic_code_name(diagnostic.code),
                 "unknown_diagnostic") != 0);
    CHECK(document.owned_bytes == NULL);
    dxbc_document_free(&document);
    return true;
}

static bool verify_lossless_synthetic(void) {
    uint8_t bytes[SYNTHETIC_SIZE];
    CHECK(make_synthetic(bytes));

    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    CHECK(dxbc_document_parse(&document, bytes, sizeof(bytes), &diagnostic));
    CHECK(diagnostic.code == DXBC_DOCUMENT_OK);
    CHECK(document.hash_valid);
    CHECK(document.owned_size == sizeof(bytes));
    CHECK(memcmp(document.owned_bytes, bytes, sizeof(bytes)) == 0);
    CHECK(document.container_version == 1);
    CHECK(document.declared_size == sizeof(bytes));
    CHECK(document.chunk_count == 2);

    const DXBCDocumentChunk* executable = &document.chunks[0];
    const DXBCDocumentChunk* unknown = &document.chunks[1];
    CHECK(executable->table_index == 0 && executable->offset == SHDR_OFFSET);
    CHECK(executable->kind == DXBC_DOCUMENT_CHUNK_EXECUTABLE);
    CHECK(memcmp(executable->fourcc, "SHDR", 4) == 0);
    CHECK(executable->raw_bytes == document.owned_bytes + SHDR_OFFSET);
    CHECK(executable->payload_bytes == executable->raw_bytes + 8);
    CHECK(executable->raw_size == 44 && executable->payload_size == 36);

    const uint8_t expected_fourcc[4] = {'A', 'B', 0, 0xff};
    const uint8_t expected_payload[3] = {0xde, 0xad, 0xbe};
    CHECK(unknown->table_index == 1 && unknown->offset == UNKNOWN_OFFSET);
    CHECK(unknown->kind == DXBC_DOCUMENT_CHUNK_UNKNOWN);
    CHECK(memcmp(unknown->fourcc, expected_fourcc, 4) == 0);
    CHECK(unknown->payload_size == sizeof(expected_payload));
    CHECK(memcmp(unknown->payload_bytes, expected_payload,
                 sizeof(expected_payload)) == 0);
    CHECK(document.owned_bytes[UNKNOWN_OFFSET + 11] == 0x7e);

    CHECK(document.instruction_count == 2);
    const DXBCDocumentInstruction* first = &document.instructions[0];
    CHECK(first->chunk_index == 0 && first->instruction_index == 0);
    CHECK(first->byte_offset == SHDR_OFFSET + 16);
    CHECK(first->byte_size == 16 && first->token_count == 4);
    CHECK(first->opcode == 54 && first->encoded_length == 4);
    CHECK(first->extended_opcode_token_count == 1);
    CHECK(!first->uses_customdata_length);
    const uint32_t first_tokens[4] = {
        0x84000036u, 0x00000001u, 0x80001001u, 0x12345678u
    };
    CHECK(memcmp(first->raw_bytes, bytes + first->byte_offset,
                 first->byte_size) == 0);
    for (size_t i = 0; i < 4; ++i) {
        uint32_t token = 0;
        CHECK(dxbc_document_instruction_token(first, i, &token));
        CHECK(token == first_tokens[i]);
    }
    uint32_t token = 0;
    CHECK(!dxbc_document_instruction_token(first, 4, &token));

    const DXBCDocumentInstruction* second = &document.instructions[1];
    CHECK(second->instruction_index == 1 && second->token_count == 3);
    CHECK(second->opcode == 53 && second->encoded_length == 0 &&
          second->uses_customdata_length);
    CHECK(second->extended_opcode_token_count == 0);
    CHECK(dxbc_document_instruction_token(second, 1, &token));
    CHECK(token == 3);
    CHECK(dxbc_document_instruction_token(second, 2, &token));
    CHECK(token == 0xcafebabeu);

    uint8_t* serialized = NULL;
    size_t serialized_size = 0;
    CHECK(dxbc_document_serialize_exact(&document, &serialized,
                                        &serialized_size, &diagnostic));
    CHECK(serialized != document.owned_bytes);
    CHECK(serialized_size == sizeof(bytes));
    CHECK(memcmp(serialized, bytes, sizeof(bytes)) == 0);
    mem_free(serialized, serialized_size);

    dxbc_document_free(&document);
    return true;
}

static bool verify_structured_failures(void) {
    uint8_t bytes[SYNTHETIC_SIZE];
    CHECK(make_synthetic(bytes));
    CHECK(expect_parse_error(bytes, sizeof(bytes) - 1,
                             DXBC_DOCUMENT_DECLARED_SIZE_MISMATCH));

    uint8_t malformed[SYNTHETIC_SIZE];
    memcpy(malformed, bytes, sizeof(malformed));
    write_u32_le(malformed + 24, SYNTHETIC_SIZE - 1);
    CHECK(refresh_hash(malformed, SYNTHETIC_SIZE - 1));
    CHECK(expect_parse_error(malformed, SYNTHETIC_SIZE - 1,
                             DXBC_DOCUMENT_TRUNCATED_CHUNK_PAYLOAD));

    memcpy(malformed, bytes, sizeof(malformed));
    write_u32_le(malformed + 36, SHDR_OFFSET);
    CHECK(expect_parse_error(malformed, sizeof(malformed),
                             DXBC_DOCUMENT_DUPLICATE_CHUNK_OFFSET));

    memcpy(malformed, bytes, sizeof(malformed));
    write_u32_le(malformed + UNKNOWN_OFFSET + 4, UINT32_MAX);
    CHECK(expect_parse_error(malformed, sizeof(malformed),
                             DXBC_DOCUMENT_CHUNK_RANGE_OVERFLOW));

    memcpy(malformed, bytes, sizeof(malformed));
    write_u32_le(malformed + 28, UINT32_MAX);
    CHECK(expect_parse_error(malformed, sizeof(malformed),
                             DXBC_DOCUMENT_CHUNK_TABLE_OVERFLOW));

    memcpy(malformed, bytes, sizeof(malformed));
    /* First instruction claims one token but also demands an opcode
     * extension.  Refresh the hash so the token diagnostic remains primary. */
    write_u32_le(malformed + SHDR_OFFSET + 16, 0x81000036u);
    CHECK(refresh_hash(malformed, sizeof(malformed)));
    CHECK(expect_parse_error(malformed, sizeof(malformed),
                             DXBC_DOCUMENT_TRUNCATED_OPCODE_EXTENSION));

    memcpy(malformed, bytes, sizeof(malformed));
    /* Only CUSTOMDATA may use DWORD 1 as a length. Ordinary opcode 54 with
     * a zero encoded length is malformed even when a following DWORD exists. */
    write_u32_le(malformed + SHDR_OFFSET + 16, 0x00000036u);
    CHECK(refresh_hash(malformed, sizeof(malformed)));
    CHECK(expect_parse_error(malformed, sizeof(malformed),
                             DXBC_DOCUMENT_INSTRUCTION_LENGTH_INVALID));

    memcpy(malformed, bytes, sizeof(malformed));
    malformed[4] ^= 0x80;
    CHECK(expect_parse_error(malformed, sizeof(malformed),
                             DXBC_DOCUMENT_HASH_MISMATCH));
    return true;
}

static bool verify_unsupported_reason(void) {
    uint8_t bytes[SYNTHETIC_SIZE];
    CHECK(make_synthetic(bytes));
    write_u32_le(bytes + SHDR_OFFSET + 8, 0x00070050u);
    CHECK(refresh_hash(bytes, sizeof(bytes)));

    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    CHECK(dxbc_document_parse(&document, bytes, sizeof(bytes), &diagnostic));
    CHECK(document.diagnostic_count == 1);
    const DXBCDocumentDiagnostic* unsupported = &document.diagnostics[0];
    CHECK(unsupported->kind == DXBC_DOCUMENT_DIAGNOSTIC_UNSUPPORTED);
    CHECK(unsupported->code == DXBC_DOCUMENT_UNSUPPORTED_SHADER_STAGE);
    CHECK(unsupported->chunk_index == 0);
    CHECK(unsupported->actual == 7 && unsupported->expected == 5);
    dxbc_document_free(&document);
    return true;
}

static uint8_t* read_file(const char* path, size_t* out_size) {
    FILE* file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) return NULL;
    const long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t* bytes = (uint8_t*)malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return bytes;
}

static bool verify_real_container_roundtrip(void) {
    size_t fixture_size = 0;
    uint8_t* fixture = read_file(DXBC_DOCUMENT_TEST_FIXTURE, &fixture_size);
    CHECK(fixture != NULL);

    DXBCContainerView view;
    CHECK(dxbc_container_view_first(fixture, fixture_size, &view));
    CHECK(view.data >= fixture && view.data + view.size <= fixture + fixture_size);

    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    CHECK(dxbc_document_parse(&document, view.data, view.size, &diagnostic));
    CHECK(document.chunk_count > 0);
    CHECK(document.instruction_count > 0);

    DXBCContainer semantic_projection;
    CHECK(dxbc_document_decode_semantic(&document, &semantic_projection));
    CHECK(semantic_projection.instruction_count > 0);
    dxbc_free(&semantic_projection);

    uint8_t* serialized = NULL;
    size_t serialized_size = 0;
    CHECK(dxbc_document_serialize_exact(&document, &serialized,
                                        &serialized_size, &diagnostic));
    CHECK(serialized_size == view.size);
    CHECK(memcmp(serialized, view.data, view.size) == 0);
    mem_free(serialized, serialized_size);
    dxbc_document_free(&document);
    free(fixture);
    return true;
}

static bool run_all_tests(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    CHECK(verify_lossless_synthetic());
    CHECK(verify_structured_failures());
    CHECK(verify_unsupported_reason());
    CHECK(verify_real_container_roundtrip());
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return true;
}

int main(void) {
    return run_all_tests() ? 0 : 1;
}
