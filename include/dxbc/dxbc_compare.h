// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_COMPARE_H
#define DXBC_COMPARE_H

#include "dxbc/dxbc_document.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Exact DXBC comparison with structural localization.
 *
 * A DXBC checksum changes whenever the container body changes, so a raw
 * first-byte comparison almost always points at header bytes 4..19.  This API
 * still defines equality as complete byte equality, but reports the first
 * authoritative structural difference before falling back to padding/header
 * bytes.  It never normalizes either input and it rejects malformed inputs.
 */
typedef enum {
    DXBC_COMPARE_EQUAL = 0,
    DXBC_COMPARE_INVALID_ARGUMENT,
    DXBC_COMPARE_EXPECTED_INVALID,
    DXBC_COMPARE_ACTUAL_INVALID,
    DXBC_COMPARE_CONTAINER_VERSION,
    DXBC_COMPARE_CHUNK_COUNT,
    DXBC_COMPARE_CHUNK_FOURCC,
    DXBC_COMPARE_PROGRAM_VERSION,
    DXBC_COMPARE_INSTRUCTION_COUNT,
    DXBC_COMPARE_INSTRUCTION_OPCODE,
    DXBC_COMPARE_INSTRUCTION_LENGTH,
    DXBC_COMPARE_INSTRUCTION_TOKEN,
    DXBC_COMPARE_CHUNK_PAYLOAD_SIZE,
    DXBC_COMPARE_CHUNK_OFFSET,
    DXBC_COMPARE_CONTAINER_SIZE,
    DXBC_COMPARE_CHUNK_PAYLOAD,
    DXBC_COMPARE_RAW_BYTE
} DXBCCompareStatus;

typedef struct {
    DXBCCompareStatus status;

    /* Parse diagnostics are meaningful for *_INVALID results. */
    DXBCDocumentDiagnostic expected_diagnostic;
    DXBCDocumentDiagnostic actual_diagnostic;

    size_t expected_size;
    size_t actual_size;

    /* SIZE_MAX/UINT32_MAX mean that the coordinate does not apply. */
    size_t first_differing_byte;
    uint32_t chunk_index;
    uint32_t instruction_index;
    uint32_t token_index;

    /* Values at the classified coordinate, zero-extended when byte-sized. */
    uint64_t expected_value;
    uint64_t actual_value;
} DXBCCompareResult;

void dxbc_compare_result_init(DXBCCompareResult* result);

DXBCCompareStatus dxbc_compare_exact(const uint8_t* expected,
                                     size_t expected_size,
                                     const uint8_t* actual,
                                     size_t actual_size,
                                     DXBCCompareResult* result);

const char* dxbc_compare_status_name(DXBCCompareStatus status);

#endif /* DXBC_COMPARE_H */
