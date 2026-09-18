// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_USBD_H
#define DXBC_USBD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * UnityShaderCompiler's multi-program result container:
 *
 *   "USBD" | u32 record_count |
 *   repeated { u32 UTF-8-name-size | name | u32 DXBC-size | raw DXBC }
 *
 * All integers are little-endian. Parsed views borrow the input bytes.
 * Encoding validates every name and complete raw DXBC document before it
 * allocates output, so this type can be used as an exact golden authority.
 */

typedef enum {
    DXBC_USBD_OK = 0,
    DXBC_USBD_INVALID_ARGUMENT,
    DXBC_USBD_BAD_MAGIC,
    DXBC_USBD_BAD_RECORD_COUNT,
    DXBC_USBD_TRUNCATED,
    DXBC_USBD_INVALID_NAME,
    DXBC_USBD_INVALID_DXBC,
    DXBC_USBD_TRAILING_BYTES,
    DXBC_USBD_SIZE_OVERFLOW,
    DXBC_USBD_ALLOCATION_FAILED,
} DXBCUSBDStatus;

typedef struct {
    DXBCUSBDStatus status;
    size_t byte_offset;
    uint32_t record_index;
} DXBCUSBDDiagnostic;

typedef struct {
    const uint8_t* name;
    size_t name_size;
    const uint8_t* dxbc;
    size_t dxbc_size;
} DXBCUSBDRecordView;

typedef struct {
    const uint8_t* bytes;
    size_t size;
    uint32_t record_count;
} DXBCUSBDTableView;

void dxbc_usbd_table_init(DXBCUSBDTableView* table);

bool dxbc_usbd_table_open(DXBCUSBDTableView* table, const uint8_t* bytes,
                          size_t size, DXBCUSBDDiagnostic* diagnostic);

bool dxbc_usbd_table_record(const DXBCUSBDTableView* table,
                            uint32_t record_index,
                            DXBCUSBDRecordView* out_record);

/* The returned allocation uses mem_alloc(); release it with
 * mem_free(*out_bytes, *out_size). The output pair is cleared on failure. */
bool dxbc_usbd_table_encode(const DXBCUSBDRecordView* records,
                            size_t record_count, uint8_t** out_bytes,
                            size_t* out_size,
                            DXBCUSBDDiagnostic* diagnostic);

const char* dxbc_usbd_status_name(DXBCUSBDStatus status);

#endif /* DXBC_USBD_H */
