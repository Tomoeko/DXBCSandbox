// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_compare.h"

#include <string.h>

static uint32_t read_u32_le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

void dxbc_compare_result_init(DXBCCompareResult* result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->status = DXBC_COMPARE_EQUAL;
    result->first_differing_byte = SIZE_MAX;
    result->chunk_index = DXBC_DOCUMENT_NO_INDEX;
    result->instruction_index = DXBC_DOCUMENT_NO_INDEX;
    result->token_index = DXBC_DOCUMENT_NO_INDEX;
    result->expected_diagnostic.code = DXBC_DOCUMENT_OK;
    result->expected_diagnostic.chunk_index = DXBC_DOCUMENT_NO_INDEX;
    result->expected_diagnostic.instruction_index = DXBC_DOCUMENT_NO_INDEX;
    result->actual_diagnostic.code = DXBC_DOCUMENT_OK;
    result->actual_diagnostic.chunk_index = DXBC_DOCUMENT_NO_INDEX;
    result->actual_diagnostic.instruction_index = DXBC_DOCUMENT_NO_INDEX;
}

static DXBCCompareStatus report(DXBCCompareResult* result,
                                DXBCCompareStatus status,
                                size_t byte_offset, uint32_t chunk_index,
                                uint32_t instruction_index,
                                uint32_t token_index,
                                uint64_t expected_value,
                                uint64_t actual_value) {
    result->status = status;
    result->first_differing_byte = byte_offset;
    result->chunk_index = chunk_index;
    result->instruction_index = instruction_index;
    result->token_index = token_index;
    result->expected_value = expected_value;
    result->actual_value = actual_value;
    return status;
}

static size_t instruction_count_for_chunk(const DXBCDocument* document,
                                          uint32_t chunk_index) {
    size_t count = 0U;
    for (size_t index = 0U; index < document->instruction_count; ++index) {
        if (document->instructions[index].chunk_index == chunk_index) ++count;
    }
    return count;
}

static const DXBCDocumentInstruction* instruction_for_chunk(
    const DXBCDocument* document, uint32_t chunk_index,
    size_t local_instruction_index) {
    size_t current = 0U;
    for (size_t index = 0U; index < document->instruction_count; ++index) {
        const DXBCDocumentInstruction* instruction =
            &document->instructions[index];
        if (instruction->chunk_index != chunk_index) continue;
        if (current++ == local_instruction_index) return instruction;
    }
    return NULL;
}

static size_t first_byte_difference(const uint8_t* expected,
                                    const uint8_t* actual, size_t size,
                                    size_t begin) {
    for (size_t offset = begin; offset < size; ++offset) {
        if (expected[offset] != actual[offset]) return offset;
    }
    return SIZE_MAX;
}

static uint32_t fourcc_value(const uint8_t fourcc[4]) {
    return read_u32_le(fourcc);
}

DXBCCompareStatus dxbc_compare_exact(const uint8_t* expected,
                                     size_t expected_size,
                                     const uint8_t* actual,
                                     size_t actual_size,
                                     DXBCCompareResult* result) {
    if (!result) return DXBC_COMPARE_INVALID_ARGUMENT;
    dxbc_compare_result_init(result);
    result->expected_size = expected_size;
    result->actual_size = actual_size;
    if ((!expected && expected_size != 0U) ||
        (!actual && actual_size != 0U) || !expected || !actual) {
        result->status = DXBC_COMPARE_INVALID_ARGUMENT;
        return result->status;
    }

    DXBCDocument expected_document;
    DXBCDocument actual_document;
    dxbc_document_init(&expected_document);
    dxbc_document_init(&actual_document);
    if (!dxbc_document_parse(&expected_document, expected, expected_size,
                             &result->expected_diagnostic)) {
        result->status = DXBC_COMPARE_EXPECTED_INVALID;
        dxbc_document_free(&expected_document);
        dxbc_document_free(&actual_document);
        return result->status;
    }
    if (!dxbc_document_parse(&actual_document, actual, actual_size,
                             &result->actual_diagnostic)) {
        result->status = DXBC_COMPARE_ACTUAL_INVALID;
        dxbc_document_free(&expected_document);
        dxbc_document_free(&actual_document);
        return result->status;
    }

#define RETURN_DIFFERENCE(...)                                                 \
    do {                                                                       \
        DXBCCompareStatus return_status = report(result, __VA_ARGS__);         \
        dxbc_document_free(&expected_document);                                \
        dxbc_document_free(&actual_document);                                  \
        return return_status;                                                  \
    } while (0)

    if (expected_size == actual_size &&
        memcmp(expected, actual, expected_size) == 0) {
        dxbc_document_free(&expected_document);
        dxbc_document_free(&actual_document);
        return DXBC_COMPARE_EQUAL;
    }

    if (expected_document.container_version !=
        actual_document.container_version) {
        RETURN_DIFFERENCE(DXBC_COMPARE_CONTAINER_VERSION, 20U,
                          DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                          DXBC_DOCUMENT_NO_INDEX,
                          expected_document.container_version,
                          actual_document.container_version);
    }
    if (expected_document.chunk_count != actual_document.chunk_count) {
        RETURN_DIFFERENCE(DXBC_COMPARE_CHUNK_COUNT, 28U,
                          DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                          DXBC_DOCUMENT_NO_INDEX,
                          expected_document.chunk_count,
                          actual_document.chunk_count);
    }

    for (uint32_t chunk_index = 0U;
         chunk_index < expected_document.chunk_count; ++chunk_index) {
        const DXBCDocumentChunk* expected_chunk =
            &expected_document.chunks[chunk_index];
        const DXBCDocumentChunk* actual_chunk =
            &actual_document.chunks[chunk_index];
        if (memcmp(expected_chunk->fourcc, actual_chunk->fourcc, 4U) != 0) {
            RETURN_DIFFERENCE(DXBC_COMPARE_CHUNK_FOURCC,
                              expected_chunk->offset, chunk_index,
                              DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                              fourcc_value(expected_chunk->fourcc),
                              fourcc_value(actual_chunk->fourcc));
        }

        if (expected_chunk->kind == DXBC_DOCUMENT_CHUNK_EXECUTABLE &&
            actual_chunk->kind == DXBC_DOCUMENT_CHUNK_EXECUTABLE) {
            const uint32_t expected_version =
                read_u32_le(expected_chunk->payload_bytes);
            const uint32_t actual_version =
                read_u32_le(actual_chunk->payload_bytes);
            if (expected_version != actual_version) {
                RETURN_DIFFERENCE(DXBC_COMPARE_PROGRAM_VERSION,
                                  (size_t)expected_chunk->offset + 8U,
                                  chunk_index, DXBC_DOCUMENT_NO_INDEX, 0U,
                                  expected_version, actual_version);
            }

            const size_t expected_instruction_count =
                instruction_count_for_chunk(&expected_document, chunk_index);
            const size_t actual_instruction_count =
                instruction_count_for_chunk(&actual_document, chunk_index);
            if (expected_instruction_count != actual_instruction_count) {
                RETURN_DIFFERENCE(
                    DXBC_COMPARE_INSTRUCTION_COUNT,
                    (size_t)expected_chunk->offset + 12U, chunk_index,
                    DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                    expected_instruction_count, actual_instruction_count);
            }

            for (size_t instruction_index = 0U;
                 instruction_index < expected_instruction_count;
                 ++instruction_index) {
                const DXBCDocumentInstruction* expected_instruction =
                    instruction_for_chunk(&expected_document, chunk_index,
                                          instruction_index);
                const DXBCDocumentInstruction* actual_instruction =
                    instruction_for_chunk(&actual_document, chunk_index,
                                          instruction_index);
                if (!expected_instruction || !actual_instruction) {
                    RETURN_DIFFERENCE(
                        DXBC_COMPARE_RAW_BYTE,
                        (size_t)expected_chunk->offset + 8U, chunk_index,
                        (uint32_t)instruction_index, DXBC_DOCUMENT_NO_INDEX,
                        expected_instruction != NULL,
                        actual_instruction != NULL);
                }
                if (expected_instruction->opcode != actual_instruction->opcode) {
                    RETURN_DIFFERENCE(
                        DXBC_COMPARE_INSTRUCTION_OPCODE,
                        expected_instruction->byte_offset, chunk_index,
                        (uint32_t)instruction_index, 0U,
                        expected_instruction->opcode,
                        actual_instruction->opcode);
                }
                if (expected_instruction->token_count !=
                    actual_instruction->token_count) {
                    RETURN_DIFFERENCE(
                        DXBC_COMPARE_INSTRUCTION_LENGTH,
                        expected_instruction->byte_offset, chunk_index,
                        (uint32_t)instruction_index, 0U,
                        expected_instruction->token_count,
                        actual_instruction->token_count);
                }
                for (uint32_t token_index = 0U;
                     token_index < expected_instruction->token_count;
                     ++token_index) {
                    uint32_t expected_token = 0U;
                    uint32_t actual_token = 0U;
                    if (!dxbc_document_instruction_token(
                            expected_instruction, token_index,
                            &expected_token) ||
                        !dxbc_document_instruction_token(
                            actual_instruction, token_index, &actual_token)) {
                        RETURN_DIFFERENCE(
                            DXBC_COMPARE_RAW_BYTE,
                            expected_instruction->byte_offset, chunk_index,
                            (uint32_t)instruction_index, token_index, 0U, 0U);
                    }
                    if (expected_token != actual_token) {
                        RETURN_DIFFERENCE(
                            DXBC_COMPARE_INSTRUCTION_TOKEN,
                            expected_instruction->byte_offset +
                                (size_t)token_index * 4U,
                            chunk_index, (uint32_t)instruction_index,
                            token_index, expected_token, actual_token);
                    }
                }
            }
        }
    }

    for (uint32_t chunk_index = 0U;
         chunk_index < expected_document.chunk_count; ++chunk_index) {
        const DXBCDocumentChunk* expected_chunk =
            &expected_document.chunks[chunk_index];
        const DXBCDocumentChunk* actual_chunk =
            &actual_document.chunks[chunk_index];
        if (expected_chunk->payload_size != actual_chunk->payload_size) {
            RETURN_DIFFERENCE(DXBC_COMPARE_CHUNK_PAYLOAD_SIZE,
                              (size_t)expected_chunk->offset + 4U, chunk_index,
                              DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                              expected_chunk->payload_size,
                              actual_chunk->payload_size);
        }
        if (expected_chunk->offset != actual_chunk->offset) {
            RETURN_DIFFERENCE(DXBC_COMPARE_CHUNK_OFFSET,
                              32U + (size_t)chunk_index * 4U, chunk_index,
                              DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                              expected_chunk->offset, actual_chunk->offset);
        }
    }
    if (expected_size != actual_size) {
        RETURN_DIFFERENCE(DXBC_COMPARE_CONTAINER_SIZE, 24U,
                          DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                          DXBC_DOCUMENT_NO_INDEX, expected_size, actual_size);
    }

    for (uint32_t chunk_index = 0U;
         chunk_index < expected_document.chunk_count; ++chunk_index) {
        const DXBCDocumentChunk* expected_chunk =
            &expected_document.chunks[chunk_index];
        const DXBCDocumentChunk* actual_chunk =
            &actual_document.chunks[chunk_index];
        size_t difference = first_byte_difference(
            expected_chunk->payload_bytes, actual_chunk->payload_bytes,
            expected_chunk->payload_size, 0U);
        if (difference != SIZE_MAX) {
            RETURN_DIFFERENCE(
                DXBC_COMPARE_CHUNK_PAYLOAD,
                (size_t)expected_chunk->offset + 8U + difference,
                chunk_index, DXBC_DOCUMENT_NO_INDEX,
                DXBC_DOCUMENT_NO_INDEX,
                expected_chunk->payload_bytes[difference],
                actual_chunk->payload_bytes[difference]);
        }
    }

    /* Ignore the checksum while looking for the causative raw-byte mismatch. */
    size_t difference = first_byte_difference(expected, actual, expected_size,
                                              0U);
    if (difference >= 4U && difference < 20U) {
        difference = first_byte_difference(expected, actual, expected_size,
                                           20U);
        if (difference == SIZE_MAX) {
            difference = first_byte_difference(expected, actual, 20U, 4U);
        }
    }
    if (difference == SIZE_MAX) difference = 0U;
    RETURN_DIFFERENCE(DXBC_COMPARE_RAW_BYTE, difference,
                      DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                      DXBC_DOCUMENT_NO_INDEX, expected[difference],
                      actual[difference]);

#undef RETURN_DIFFERENCE
}

const char* dxbc_compare_status_name(DXBCCompareStatus status) {
    switch (status) {
        case DXBC_COMPARE_EQUAL: return "equal";
        case DXBC_COMPARE_INVALID_ARGUMENT: return "invalid_argument";
        case DXBC_COMPARE_EXPECTED_INVALID: return "expected_invalid";
        case DXBC_COMPARE_ACTUAL_INVALID: return "actual_invalid";
        case DXBC_COMPARE_CONTAINER_VERSION: return "container_version";
        case DXBC_COMPARE_CHUNK_COUNT: return "chunk_count";
        case DXBC_COMPARE_CHUNK_FOURCC: return "chunk_fourcc";
        case DXBC_COMPARE_PROGRAM_VERSION: return "program_version";
        case DXBC_COMPARE_INSTRUCTION_COUNT: return "instruction_count";
        case DXBC_COMPARE_INSTRUCTION_OPCODE: return "instruction_opcode";
        case DXBC_COMPARE_INSTRUCTION_LENGTH: return "instruction_length";
        case DXBC_COMPARE_INSTRUCTION_TOKEN: return "instruction_token";
        case DXBC_COMPARE_CHUNK_PAYLOAD_SIZE: return "chunk_payload_size";
        case DXBC_COMPARE_CHUNK_OFFSET: return "chunk_offset";
        case DXBC_COMPARE_CONTAINER_SIZE: return "container_size";
        case DXBC_COMPARE_CHUNK_PAYLOAD: return "chunk_payload";
        case DXBC_COMPARE_RAW_BYTE: return "raw_byte";
    }
    return "unknown";
}
