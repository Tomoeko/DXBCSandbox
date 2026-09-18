// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_DOCUMENT_H
#define DXBC_DOCUMENT_H

#include "common/common.h"
#include "dxbc/dxbc_parser.h"

/*
 * Lossless representation of one raw DXBC container.
 *
 * DXBCDocument deliberately does not use DXBCContainer/USIL as its storage
 * authority.  The owned_bytes allocation is the source of truth, including
 * the header, offset table, inter-chunk gaps, unknown chunks, and every token
 * in an executable program.  The other records are immutable views into that
 * allocation and can therefore be used without throwing bytes away.
 *
 * A document must be initialized before its first parse and freed when no
 * longer needed.  Do not copy a live document by value: its views point into
 * owned_bytes.
 */

typedef enum {
    DXBC_DOCUMENT_DIAGNOSTIC_VALIDATION = 0,
    DXBC_DOCUMENT_DIAGNOSTIC_UNSUPPORTED = 1
} DXBCDocumentDiagnosticKind;

typedef enum {
    DXBC_DOCUMENT_OK = 0,
    DXBC_DOCUMENT_INVALID_ARGUMENT,
    DXBC_DOCUMENT_OUT_OF_MEMORY,
    DXBC_DOCUMENT_NOT_DXBC,
    DXBC_DOCUMENT_TRUNCATED_HEADER,
    DXBC_DOCUMENT_UNSUPPORTED_CONTAINER_VERSION,
    DXBC_DOCUMENT_DECLARED_SIZE_INVALID,
    DXBC_DOCUMENT_DECLARED_SIZE_MISMATCH,
    DXBC_DOCUMENT_CHUNK_TABLE_OVERFLOW,
    DXBC_DOCUMENT_CHUNK_OFFSET_UNALIGNED,
    DXBC_DOCUMENT_CHUNK_OFFSET_IN_HEADER,
    DXBC_DOCUMENT_DUPLICATE_CHUNK_OFFSET,
    DXBC_DOCUMENT_TRUNCATED_CHUNK_HEADER,
    DXBC_DOCUMENT_CHUNK_RANGE_OVERFLOW,
    DXBC_DOCUMENT_TRUNCATED_CHUNK_PAYLOAD,
    DXBC_DOCUMENT_OVERLAPPING_CHUNKS,
    DXBC_DOCUMENT_HASH_MISMATCH,
    DXBC_DOCUMENT_EXECUTABLE_SIZE_UNALIGNED,
    DXBC_DOCUMENT_TRUNCATED_EXECUTABLE_HEADER,
    DXBC_DOCUMENT_EXECUTABLE_LENGTH_MISMATCH,
    DXBC_DOCUMENT_TRUNCATED_INSTRUCTION,
    DXBC_DOCUMENT_INSTRUCTION_LENGTH_INVALID,
    DXBC_DOCUMENT_TRUNCATED_OPCODE_EXTENSION,
    DXBC_DOCUMENT_UNSUPPORTED_SHADER_STAGE,
    DXBC_DOCUMENT_CORRUPT_DOCUMENT
} DXBCDocumentDiagnosticCode;

#define DXBC_DOCUMENT_NO_INDEX UINT32_MAX

typedef struct {
    DXBCDocumentDiagnosticKind kind;
    DXBCDocumentDiagnosticCode code;
    size_t byte_offset;
    uint32_t chunk_index;
    uint32_t instruction_index;
    uint64_t expected;
    uint64_t actual;
} DXBCDocumentDiagnostic;

typedef enum {
    DXBC_DOCUMENT_CHUNK_UNKNOWN = 0,
    DXBC_DOCUMENT_CHUNK_EXECUTABLE,
    DXBC_DOCUMENT_CHUNK_SIGNATURE,
    DXBC_DOCUMENT_CHUNK_REFLECTION,
    DXBC_DOCUMENT_CHUNK_STATISTICS,
    DXBC_DOCUMENT_CHUNK_DEBUG
} DXBCDocumentChunkKind;

typedef struct {
    /* FourCC bytes are kept as bytes because arbitrary/unknown values are
     * legal preservation inputs and need not be printable or NUL-free. */
    uint8_t fourcc[4];
    DXBCDocumentChunkKind kind;
    uint32_t table_index;
    uint32_t offset;
    uint32_t payload_size;
    size_t raw_size;
    const uint8_t* raw_bytes;
    const uint8_t* payload_bytes;
} DXBCDocumentChunk;

typedef struct {
    uint32_t chunk_index;
    uint32_t instruction_index;
    size_t byte_offset;
    size_t byte_size;
    uint32_t token_count;
    uint32_t opcode;
    uint32_t encoded_length;
    uint32_t extended_opcode_token_count;
    /* CUSTOMDATA (opcode 53) stores its complete block length in DWORD 1;
     * ordinary opcodes must encode a nonzero length in the opcode token. */
    bool uses_customdata_length;
    const uint8_t* raw_bytes;
} DXBCDocumentInstruction;

typedef struct {
    uint8_t magic[4];
    uint8_t hash[16];
    uint32_t container_version;
    uint32_t declared_size;
    uint32_t chunk_count;
    bool hash_valid;

    uint8_t* owned_bytes;
    size_t owned_size;

    DXBCDocumentChunk* chunks;
    DXBCDocumentInstruction* instructions;
    size_t instruction_count;
    size_t instruction_capacity;

    /* Non-fatal, structured unsupported-feature reports.  Validation errors
     * make parsing fail and are returned through out_diagnostic instead. */
    DXBCDocumentDiagnostic* diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
} DXBCDocument;

void dxbc_document_init(DXBCDocument* document);
void dxbc_document_free(DXBCDocument* document);

/* Parse exactly one raw DXBC container.  Wrappers (such as USBD or Unity
 * compiled-program records) must first be resolved with
 * dxbc_container_view_first(); keeping that boundary explicit avoids
 * pretending an embedded container is byte-identical to its wrapper. */
bool dxbc_document_parse(DXBCDocument* document, const uint8_t* bytes,
                         size_t size,
                         DXBCDocumentDiagnostic* out_diagnostic);

/* Allocates an exact copy of owned_bytes.  This is deliberately a lossless
 * serializer, not a normalizing/rebuilding serializer.  Free *out_bytes with
 * mem_free(*out_bytes, *out_size). */
bool dxbc_document_serialize_exact(
    const DXBCDocument* document, uint8_t** out_bytes, size_t* out_size,
    DXBCDocumentDiagnostic* out_diagnostic);

/* Reads one little-endian token from the complete raw instruction slice.
 * Both opcode- and operand-extension tokens are present because slices are
 * never reconstructed from semantic operands. */
bool dxbc_document_instruction_token(
    const DXBCDocumentInstruction* instruction, size_t token_index,
    uint32_t* out_token);

/* Optional lossy projection for existing USIL consumers.  This always reads
 * from owned_bytes; it never updates or replaces the lossless document. */
bool dxbc_document_decode_semantic(const DXBCDocument* document,
                                   DXBCContainer* out_container);

const char* dxbc_document_diagnostic_code_name(
    DXBCDocumentDiagnosticCode code);

#endif /* DXBC_DOCUMENT_H */
