// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_document.h"

#include "dxbc/dxbc_hash.h"

#include <limits.h>
#include <string.h>

#define DXBC_DOCUMENT_HEADER_SIZE 32u

static uint32_t read_u32_le(const uint8_t* bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return read_le32(value);
}

static void clear_diagnostic(DXBCDocumentDiagnostic* diagnostic) {
    if (!diagnostic) return;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->code = DXBC_DOCUMENT_OK;
    diagnostic->chunk_index = DXBC_DOCUMENT_NO_INDEX;
    diagnostic->instruction_index = DXBC_DOCUMENT_NO_INDEX;
}

static DXBCDocumentDiagnosticKind diagnostic_kind(
    DXBCDocumentDiagnosticCode code) {
    switch (code) {
        case DXBC_DOCUMENT_UNSUPPORTED_CONTAINER_VERSION:
        case DXBC_DOCUMENT_UNSUPPORTED_SHADER_STAGE:
            return DXBC_DOCUMENT_DIAGNOSTIC_UNSUPPORTED;
        default:
            return DXBC_DOCUMENT_DIAGNOSTIC_VALIDATION;
    }
}

static bool report_failure(DXBCDocumentDiagnostic* diagnostic,
                           DXBCDocumentDiagnosticCode code,
                           size_t byte_offset, uint32_t chunk_index,
                           uint32_t instruction_index, uint64_t expected,
                           uint64_t actual) {
    if (diagnostic) {
        diagnostic->kind = diagnostic_kind(code);
        diagnostic->code = code;
        diagnostic->byte_offset = byte_offset;
        diagnostic->chunk_index = chunk_index;
        diagnostic->instruction_index = instruction_index;
        diagnostic->expected = expected;
        diagnostic->actual = actual;
    }
    return false;
}

static bool fourcc_equal(const uint8_t fourcc[4], const char* name) {
    return memcmp(fourcc, name, 4) == 0;
}

static DXBCDocumentChunkKind classify_chunk(const uint8_t fourcc[4]) {
    if (fourcc_equal(fourcc, "SHDR") || fourcc_equal(fourcc, "SHEX")) {
        return DXBC_DOCUMENT_CHUNK_EXECUTABLE;
    }
    if (fourcc_equal(fourcc, "ISGN") || fourcc_equal(fourcc, "ISG1") ||
        fourcc_equal(fourcc, "OSGN") || fourcc_equal(fourcc, "OSG1") ||
        fourcc_equal(fourcc, "OSG5") || fourcc_equal(fourcc, "PCSG") ||
        fourcc_equal(fourcc, "PSG1")) {
        return DXBC_DOCUMENT_CHUNK_SIGNATURE;
    }
    if (fourcc_equal(fourcc, "RDEF") || fourcc_equal(fourcc, "RD11")) {
        return DXBC_DOCUMENT_CHUNK_REFLECTION;
    }
    if (fourcc_equal(fourcc, "STAT")) {
        return DXBC_DOCUMENT_CHUNK_STATISTICS;
    }
    if (fourcc_equal(fourcc, "SDBG") || fourcc_equal(fourcc, "SPDB") ||
        fourcc_equal(fourcc, "ILDB") || fourcc_equal(fourcc, "ILDN")) {
        return DXBC_DOCUMENT_CHUNK_DEBUG;
    }
    return DXBC_DOCUMENT_CHUNK_UNKNOWN;
}

static bool append_diagnostic(DXBCDocument* document,
                              DXBCDocumentDiagnosticCode code,
                              size_t byte_offset, uint32_t chunk_index,
                              uint32_t instruction_index, uint64_t expected,
                              uint64_t actual,
                              DXBCDocumentDiagnostic* out_diagnostic) {
    if (document->diagnostic_count == document->diagnostic_capacity) {
        size_t old_capacity = document->diagnostic_capacity;
        size_t new_capacity = old_capacity == 0 ? 4 : old_capacity * 2;
        if (new_capacity < old_capacity ||
            new_capacity > SIZE_MAX / sizeof(*document->diagnostics)) {
            return report_failure(out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY,
                                  byte_offset, chunk_index,
                                  instruction_index, 0, 0);
        }
        DXBCDocumentDiagnostic* resized =
            (DXBCDocumentDiagnostic*)mem_realloc(
                document->diagnostics,
                old_capacity * sizeof(*document->diagnostics),
                new_capacity * sizeof(*document->diagnostics));
        if (!resized) {
            return report_failure(out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY,
                                  byte_offset, chunk_index,
                                  instruction_index, 0, 0);
        }
        document->diagnostics = resized;
        document->diagnostic_capacity = new_capacity;
    }

    DXBCDocumentDiagnostic* diagnostic =
        &document->diagnostics[document->diagnostic_count++];
    clear_diagnostic(diagnostic);
    diagnostic->kind = diagnostic_kind(code);
    diagnostic->code = code;
    diagnostic->byte_offset = byte_offset;
    diagnostic->chunk_index = chunk_index;
    diagnostic->instruction_index = instruction_index;
    diagnostic->expected = expected;
    diagnostic->actual = actual;
    return true;
}

static bool append_instruction(DXBCDocument* document,
                               const DXBCDocumentInstruction* instruction,
                               DXBCDocumentDiagnostic* out_diagnostic) {
    if (document->instruction_count == document->instruction_capacity) {
        size_t old_capacity = document->instruction_capacity;
        size_t new_capacity = old_capacity == 0 ? 64 : old_capacity * 2;
        if (new_capacity < old_capacity ||
            new_capacity > SIZE_MAX / sizeof(*document->instructions)) {
            return report_failure(
                out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY,
                instruction->byte_offset, instruction->chunk_index,
                instruction->instruction_index, 0, 0);
        }
        DXBCDocumentInstruction* resized =
            (DXBCDocumentInstruction*)mem_realloc(
                document->instructions,
                old_capacity * sizeof(*document->instructions),
                new_capacity * sizeof(*document->instructions));
        if (!resized) {
            return report_failure(
                out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY,
                instruction->byte_offset, instruction->chunk_index,
                instruction->instruction_index, 0, 0);
        }
        document->instructions = resized;
        document->instruction_capacity = new_capacity;
    }
    document->instructions[document->instruction_count++] = *instruction;
    return true;
}

static bool parse_executable_chunk(DXBCDocument* document,
                                   const DXBCDocumentChunk* chunk,
                                   DXBCDocumentDiagnostic* out_diagnostic) {
    const uint32_t chunk_index = chunk->table_index;
    const size_t payload_offset = (size_t)chunk->offset + 8u;
    if ((chunk->payload_size & 3u) != 0) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_EXECUTABLE_SIZE_UNALIGNED,
            payload_offset, chunk_index, DXBC_DOCUMENT_NO_INDEX, 4,
            chunk->payload_size & 3u);
    }
    if (chunk->payload_size < 8u) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_TRUNCATED_EXECUTABLE_HEADER,
            payload_offset, chunk_index, DXBC_DOCUMENT_NO_INDEX, 8,
            chunk->payload_size);
    }

    const uint32_t version_token = read_u32_le(chunk->payload_bytes);
    const uint32_t declared_tokens = read_u32_le(chunk->payload_bytes + 4u);
    const uint32_t available_tokens = chunk->payload_size / 4u;
    if (declared_tokens != available_tokens || declared_tokens < 2u) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_EXECUTABLE_LENGTH_MISMATCH,
            payload_offset + 4u, chunk_index, DXBC_DOCUMENT_NO_INDEX,
            available_tokens, declared_tokens);
    }

    const uint32_t shader_stage = (version_token >> 16) & 0xffffu;
    if (shader_stage > 5u &&
        !append_diagnostic(document, DXBC_DOCUMENT_UNSUPPORTED_SHADER_STAGE,
                           payload_offset, chunk_index,
                           DXBC_DOCUMENT_NO_INDEX, 5, shader_stage,
                           out_diagnostic)) {
        return false;
    }

    uint32_t word_offset = 2u;
    uint32_t instruction_index = 0u;
    while (word_offset < declared_tokens) {
        const size_t instruction_offset =
            payload_offset + (size_t)word_offset * 4u;
        const uint32_t first_token =
            read_u32_le(document->owned_bytes + instruction_offset);
        const uint32_t opcode = first_token & 0x7ffu;
        const uint32_t encoded_length = (first_token >> 24) & 0x7fu;
        const bool uses_customdata_length = opcode == 53u;
        uint32_t token_count = encoded_length;

        if (uses_customdata_length) {
            if (declared_tokens - word_offset < 2u) {
                return report_failure(
                    out_diagnostic, DXBC_DOCUMENT_TRUNCATED_INSTRUCTION,
                    instruction_offset, chunk_index, instruction_index, 2,
                    declared_tokens - word_offset);
            }
            token_count = read_u32_le(document->owned_bytes +
                                      instruction_offset + 4u);
        } else if (encoded_length == 0u) {
            return report_failure(
                out_diagnostic, DXBC_DOCUMENT_INSTRUCTION_LENGTH_INVALID,
                instruction_offset, chunk_index, instruction_index, 1, 0);
        }

        const uint32_t minimum_tokens = uses_customdata_length ? 2u : 1u;
        if (token_count < minimum_tokens) {
            return report_failure(
                out_diagnostic, DXBC_DOCUMENT_INSTRUCTION_LENGTH_INVALID,
                instruction_offset, chunk_index, instruction_index,
                minimum_tokens, token_count);
        }
        if (token_count > declared_tokens - word_offset) {
            return report_failure(
                out_diagnostic, DXBC_DOCUMENT_TRUNCATED_INSTRUCTION,
                instruction_offset, chunk_index, instruction_index,
                token_count, declared_tokens - word_offset);
        }

        uint32_t extension_count = 0u;
        if ((first_token & 0x80000000u) != 0) {
            uint32_t extension_word = uses_customdata_length ? 2u : 1u;
            bool continuation = true;
            while (continuation) {
                if (extension_word >= token_count) {
                    return report_failure(
                        out_diagnostic,
                        DXBC_DOCUMENT_TRUNCATED_OPCODE_EXTENSION,
                        instruction_offset + (size_t)extension_word * 4u,
                        chunk_index, instruction_index, extension_word + 1u,
                        token_count);
                }
                const uint32_t extension_token = read_u32_le(
                    document->owned_bytes + instruction_offset +
                    (size_t)extension_word * 4u);
                continuation = (extension_token & 0x80000000u) != 0;
                ++extension_count;
                ++extension_word;
            }
        }

        DXBCDocumentInstruction instruction;
        memset(&instruction, 0, sizeof(instruction));
        instruction.chunk_index = chunk_index;
        instruction.instruction_index = instruction_index;
        instruction.byte_offset = instruction_offset;
        instruction.byte_size = (size_t)token_count * 4u;
        instruction.token_count = token_count;
        instruction.opcode = opcode;
        instruction.encoded_length = encoded_length;
        instruction.extended_opcode_token_count = extension_count;
        instruction.uses_customdata_length = uses_customdata_length;
        instruction.raw_bytes = document->owned_bytes + instruction_offset;
        if (!append_instruction(document, &instruction, out_diagnostic)) {
            return false;
        }

        word_offset += token_count;
        ++instruction_index;
    }
    return true;
}

void dxbc_document_init(DXBCDocument* document) {
    if (document) memset(document, 0, sizeof(*document));
}

void dxbc_document_free(DXBCDocument* document) {
    if (!document) return;
    if (document->diagnostics) {
        mem_free(document->diagnostics,
                 document->diagnostic_capacity *
                     sizeof(*document->diagnostics));
    }
    if (document->instructions) {
        mem_free(document->instructions,
                 document->instruction_capacity *
                     sizeof(*document->instructions));
    }
    if (document->chunks) {
        mem_free(document->chunks,
                 (size_t)document->chunk_count * sizeof(*document->chunks));
    }
    if (document->owned_bytes) {
        mem_free(document->owned_bytes, document->owned_size);
    }
    memset(document, 0, sizeof(*document));
}

bool dxbc_document_parse(DXBCDocument* document, const uint8_t* bytes,
                         size_t size,
                         DXBCDocumentDiagnostic* out_diagnostic) {
    DXBCDocument parsed;
    dxbc_document_init(&parsed);
    clear_diagnostic(out_diagnostic);

    if (!document || !bytes) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_INVALID_ARGUMENT,
                              0, DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX, 0, 0);
    }
    if (size < 4u) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_TRUNCATED_HEADER,
                              0, DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_HEADER_SIZE, size);
    }
    if (memcmp(bytes, "DXBC", 4) != 0) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_NOT_DXBC, 0,
                              DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX, 0, 0);
    }
    if (size < DXBC_DOCUMENT_HEADER_SIZE) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_TRUNCATED_HEADER,
                              size, DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_HEADER_SIZE, size);
    }

    const uint32_t container_version = read_u32_le(bytes + 20u);
    const uint32_t declared_size = read_u32_le(bytes + 24u);
    const uint32_t chunk_count = read_u32_le(bytes + 28u);
    if (container_version != 1u) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_UNSUPPORTED_CONTAINER_VERSION, 20,
            DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX, 1,
            container_version);
    }
    if (declared_size < DXBC_DOCUMENT_HEADER_SIZE) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_DECLARED_SIZE_INVALID, 24,
            DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
            DXBC_DOCUMENT_HEADER_SIZE, declared_size);
    }
    if ((size_t)declared_size != size) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_DECLARED_SIZE_MISMATCH, 24,
            DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX, declared_size,
            size);
    }
    if (chunk_count > (UINT32_MAX - DXBC_DOCUMENT_HEADER_SIZE) / 4u ||
        dxbc_size_multiply_overflows(
            (size_t)chunk_count, sizeof(uint32_t)) ||
        dxbc_size_add_overflows(
            DXBC_DOCUMENT_HEADER_SIZE,
            (size_t)chunk_count * sizeof(uint32_t))) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_CHUNK_TABLE_OVERFLOW, 28,
            DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
            (declared_size - DXBC_DOCUMENT_HEADER_SIZE) / 4u, chunk_count);
    }
    const size_t chunk_table_end =
        DXBC_DOCUMENT_HEADER_SIZE + (size_t)chunk_count * sizeof(uint32_t);
    if (chunk_table_end > declared_size) {
        return report_failure(
            out_diagnostic, DXBC_DOCUMENT_CHUNK_TABLE_OVERFLOW, 28,
            DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
            (declared_size - DXBC_DOCUMENT_HEADER_SIZE) / 4u, chunk_count);
    }
    if (dxbc_size_multiply_overflows(
            (size_t)chunk_count, sizeof(*parsed.chunks))) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY, 28,
                              DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX, 0, chunk_count);
    }

    parsed.owned_bytes = (uint8_t*)mem_alloc(size);
    if (!parsed.owned_bytes) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY, 0,
                              DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX, size, 0);
    }
    memcpy(parsed.owned_bytes, bytes, size);
    parsed.owned_size = size;
    memcpy(parsed.magic, bytes, sizeof(parsed.magic));
    memcpy(parsed.hash, bytes + 4u, sizeof(parsed.hash));
    parsed.container_version = container_version;
    parsed.declared_size = declared_size;
    parsed.chunk_count = chunk_count;

    if (chunk_count != 0u) {
        parsed.chunks = (DXBCDocumentChunk*)mem_alloc(
            (size_t)chunk_count * sizeof(*parsed.chunks));
        if (!parsed.chunks) {
            report_failure(out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY, 28,
                           DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX,
                           chunk_count, 0);
            goto fail;
        }
        memset(parsed.chunks, 0,
               (size_t)chunk_count * sizeof(*parsed.chunks));
    }

    for (uint32_t i = 0; i < chunk_count; ++i) {
        const size_t table_offset =
            DXBC_DOCUMENT_HEADER_SIZE + (size_t)i * sizeof(uint32_t);
        const uint32_t offset = read_u32_le(bytes + table_offset);
        if ((offset & 3u) != 0u) {
            report_failure(out_diagnostic,
                           DXBC_DOCUMENT_CHUNK_OFFSET_UNALIGNED, table_offset,
                           i, DXBC_DOCUMENT_NO_INDEX, 4, offset & 3u);
            goto fail;
        }
        if ((size_t)offset < chunk_table_end) {
            report_failure(out_diagnostic,
                           DXBC_DOCUMENT_CHUNK_OFFSET_IN_HEADER, table_offset,
                           i, DXBC_DOCUMENT_NO_INDEX, chunk_table_end, offset);
            goto fail;
        }
        for (uint32_t previous = 0; previous < i; ++previous) {
            if (parsed.chunks[previous].offset == offset) {
                report_failure(
                    out_diagnostic, DXBC_DOCUMENT_DUPLICATE_CHUNK_OFFSET,
                    table_offset, i, DXBC_DOCUMENT_NO_INDEX, previous,
                    offset);
                goto fail;
            }
        }
        if ((size_t)offset > size || size - (size_t)offset < 8u) {
            report_failure(
                out_diagnostic, DXBC_DOCUMENT_TRUNCATED_CHUNK_HEADER, offset,
                i, DXBC_DOCUMENT_NO_INDEX, 8,
                (size_t)offset <= size ? size - (size_t)offset : 0);
            goto fail;
        }

        DXBCDocumentChunk* chunk = &parsed.chunks[i];
        memcpy(chunk->fourcc, parsed.owned_bytes + offset, 4u);
        chunk->kind = classify_chunk(chunk->fourcc);
        chunk->table_index = i;
        chunk->offset = offset;
        chunk->payload_size = read_u32_le(parsed.owned_bytes + offset + 4u);
        if (chunk->payload_size > UINT32_MAX - offset - 8u) {
            report_failure(
                out_diagnostic, DXBC_DOCUMENT_CHUNK_RANGE_OVERFLOW,
                (size_t)offset + 4u, i, DXBC_DOCUMENT_NO_INDEX,
                UINT32_MAX - offset - 8u, chunk->payload_size);
            goto fail;
        }
        const size_t payload_offset = (size_t)offset + 8u;
        if ((size_t)chunk->payload_size > size - payload_offset) {
            report_failure(
                out_diagnostic, DXBC_DOCUMENT_TRUNCATED_CHUNK_PAYLOAD,
                payload_offset, i, DXBC_DOCUMENT_NO_INDEX,
                chunk->payload_size, size - payload_offset);
            goto fail;
        }
        chunk->raw_size = 8u + (size_t)chunk->payload_size;
        chunk->raw_bytes = parsed.owned_bytes + offset;
        chunk->payload_bytes = parsed.owned_bytes + payload_offset;

        const size_t this_start = offset;
        const size_t this_end = this_start + chunk->raw_size;
        for (uint32_t previous = 0; previous < i; ++previous) {
            const size_t previous_start = parsed.chunks[previous].offset;
            const size_t previous_end =
                previous_start + parsed.chunks[previous].raw_size;
            if (this_start < previous_end && previous_start < this_end) {
                report_failure(out_diagnostic,
                               DXBC_DOCUMENT_OVERLAPPING_CHUNKS, offset, i,
                               DXBC_DOCUMENT_NO_INDEX, previous, offset);
                goto fail;
            }
        }
    }

    parsed.hash_valid =
        dxbc_verify_hash(parsed.owned_bytes, parsed.owned_size);
    if (!parsed.hash_valid) {
        report_failure(out_diagnostic, DXBC_DOCUMENT_HASH_MISMATCH, 4,
                       DXBC_DOCUMENT_NO_INDEX, DXBC_DOCUMENT_NO_INDEX, 0, 0);
        goto fail;
    }

    for (uint32_t i = 0; i < chunk_count; ++i) {
        if (parsed.chunks[i].kind == DXBC_DOCUMENT_CHUNK_EXECUTABLE &&
            !parse_executable_chunk(&parsed, &parsed.chunks[i],
                                    out_diagnostic)) {
            goto fail;
        }
    }

    dxbc_document_free(document);
    *document = parsed;
    return true;

fail:
    dxbc_document_free(&parsed);
    return false;
}

bool dxbc_document_serialize_exact(
    const DXBCDocument* document, uint8_t** out_bytes, size_t* out_size,
    DXBCDocumentDiagnostic* out_diagnostic) {
    clear_diagnostic(out_diagnostic);
    if (!out_bytes || !out_size) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_INVALID_ARGUMENT,
                              0, DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX, 0, 0);
    }
    *out_bytes = NULL;
    *out_size = 0;
    if (!document || !document->owned_bytes || document->owned_size == 0 ||
        document->declared_size != document->owned_size ||
        memcmp(document->magic, "DXBC", 4) != 0) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_CORRUPT_DOCUMENT,
                              0, DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX, 0,
                              document ? document->owned_size : 0);
    }

    uint8_t* serialized = (uint8_t*)mem_alloc(document->owned_size);
    if (!serialized) {
        return report_failure(out_diagnostic, DXBC_DOCUMENT_OUT_OF_MEMORY, 0,
                              DXBC_DOCUMENT_NO_INDEX,
                              DXBC_DOCUMENT_NO_INDEX, document->owned_size,
                              0);
    }
    memcpy(serialized, document->owned_bytes, document->owned_size);
    *out_bytes = serialized;
    *out_size = document->owned_size;
    return true;
}

bool dxbc_document_instruction_token(
    const DXBCDocumentInstruction* instruction, size_t token_index,
    uint32_t* out_token) {
    if (!instruction || !instruction->raw_bytes || !out_token ||
        token_index >= instruction->token_count) {
        return false;
    }
    *out_token = read_u32_le(instruction->raw_bytes + token_index * 4u);
    return true;
}

bool dxbc_document_decode_semantic(const DXBCDocument* document,
                                   DXBCContainer* out_container) {
    if (!document || !document->owned_bytes || document->owned_size == 0 ||
        !out_container) {
        return false;
    }
    return dxbc_parse(out_container, document->owned_bytes,
                      document->owned_size);
}

const char* dxbc_document_diagnostic_code_name(
    DXBCDocumentDiagnosticCode code) {
    switch (code) {
        case DXBC_DOCUMENT_OK: return "ok";
        case DXBC_DOCUMENT_INVALID_ARGUMENT: return "invalid_argument";
        case DXBC_DOCUMENT_OUT_OF_MEMORY: return "out_of_memory";
        case DXBC_DOCUMENT_NOT_DXBC: return "not_dxbc";
        case DXBC_DOCUMENT_TRUNCATED_HEADER: return "truncated_header";
        case DXBC_DOCUMENT_UNSUPPORTED_CONTAINER_VERSION:
            return "unsupported_container_version";
        case DXBC_DOCUMENT_DECLARED_SIZE_INVALID:
            return "declared_size_invalid";
        case DXBC_DOCUMENT_DECLARED_SIZE_MISMATCH:
            return "declared_size_mismatch";
        case DXBC_DOCUMENT_CHUNK_TABLE_OVERFLOW:
            return "chunk_table_overflow";
        case DXBC_DOCUMENT_CHUNK_OFFSET_UNALIGNED:
            return "chunk_offset_unaligned";
        case DXBC_DOCUMENT_CHUNK_OFFSET_IN_HEADER:
            return "chunk_offset_in_header";
        case DXBC_DOCUMENT_DUPLICATE_CHUNK_OFFSET:
            return "duplicate_chunk_offset";
        case DXBC_DOCUMENT_TRUNCATED_CHUNK_HEADER:
            return "truncated_chunk_header";
        case DXBC_DOCUMENT_CHUNK_RANGE_OVERFLOW:
            return "chunk_range_overflow";
        case DXBC_DOCUMENT_TRUNCATED_CHUNK_PAYLOAD:
            return "truncated_chunk_payload";
        case DXBC_DOCUMENT_OVERLAPPING_CHUNKS:
            return "overlapping_chunks";
        case DXBC_DOCUMENT_HASH_MISMATCH: return "hash_mismatch";
        case DXBC_DOCUMENT_EXECUTABLE_SIZE_UNALIGNED:
            return "executable_size_unaligned";
        case DXBC_DOCUMENT_TRUNCATED_EXECUTABLE_HEADER:
            return "truncated_executable_header";
        case DXBC_DOCUMENT_EXECUTABLE_LENGTH_MISMATCH:
            return "executable_length_mismatch";
        case DXBC_DOCUMENT_TRUNCATED_INSTRUCTION:
            return "truncated_instruction";
        case DXBC_DOCUMENT_INSTRUCTION_LENGTH_INVALID:
            return "instruction_length_invalid";
        case DXBC_DOCUMENT_TRUNCATED_OPCODE_EXTENSION:
            return "truncated_opcode_extension";
        case DXBC_DOCUMENT_UNSUPPORTED_SHADER_STAGE:
            return "unsupported_shader_stage";
        case DXBC_DOCUMENT_CORRUPT_DOCUMENT: return "corrupt_document";
    }
    return "unknown_diagnostic";
}
