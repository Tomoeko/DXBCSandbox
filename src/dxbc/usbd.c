// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/usbd.h"

#include "common/common.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_parser.h"

#include <limits.h>
#include <string.h>

enum { DXBC_USBD_HEADER_SIZE = 8 };

static void set_diagnostic(DXBCUSBDDiagnostic* diagnostic,
                           DXBCUSBDStatus status, size_t byte_offset,
                           uint32_t record_index) {
    if (!diagnostic) return;
    diagnostic->status = status;
    diagnostic->byte_offset = byte_offset;
    diagnostic->record_index = record_index;
}

static uint32_t read_u32_le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_u32_le(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static bool valid_utf8_name(const uint8_t* bytes, size_t size) {
    if (!bytes || size == 0u) return false;
    size_t at = 0u;
    while (at < size) {
        uint8_t first = bytes[at++];
        if (first == 0u) return false;
        if (first < 0x80u) continue;

        size_t continuation = 0u;
        uint32_t codepoint = 0u;
        uint32_t minimum = 0u;
        if ((first & 0xe0u) == 0xc0u) {
            continuation = 1u;
            codepoint = first & 0x1fu;
            minimum = 0x80u;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation = 2u;
            codepoint = first & 0x0fu;
            minimum = 0x800u;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation = 3u;
            codepoint = first & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (continuation > size - at) return false;
        for (size_t index = 0u; index < continuation; ++index) {
            uint8_t next = bytes[at++];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

static bool valid_raw_dxbc(const uint8_t* bytes, size_t size) {
    DXBCContainerView container;
    if (!bytes || !dxbc_container_view_first(bytes, size, &container) ||
        container.data != bytes || container.size != size) {
        return false;
    }
    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    bool valid = dxbc_document_parse(&document, bytes, size, &diagnostic);
    dxbc_document_free(&document);
    return valid;
}

void dxbc_usbd_table_init(DXBCUSBDTableView* table) {
    if (table) memset(table, 0, sizeof(*table));
}

static bool parse_record(const uint8_t* bytes, size_t size, size_t* at,
                         uint32_t record_index,
                         DXBCUSBDRecordView* out_record,
                         DXBCUSBDDiagnostic* diagnostic) {
    if (*at > size || size - *at < 4u) {
        set_diagnostic(diagnostic, DXBC_USBD_TRUNCATED, *at, record_index);
        return false;
    }
    size_t name_size = read_u32_le(bytes + *at);
    *at += 4u;
    if (name_size == 0u || name_size > size - *at ||
        !valid_utf8_name(bytes + *at, name_size)) {
        set_diagnostic(diagnostic, DXBC_USBD_INVALID_NAME, *at,
                       record_index);
        return false;
    }
    const uint8_t* name = bytes + *at;
    *at += name_size;
    if (size - *at < 4u) {
        set_diagnostic(diagnostic, DXBC_USBD_TRUNCATED, *at, record_index);
        return false;
    }
    size_t dxbc_size = read_u32_le(bytes + *at);
    *at += 4u;
    if (dxbc_size > size - *at) {
        set_diagnostic(diagnostic, DXBC_USBD_TRUNCATED, *at, record_index);
        return false;
    }
    if (!valid_raw_dxbc(bytes + *at, dxbc_size)) {
        set_diagnostic(diagnostic, DXBC_USBD_INVALID_DXBC, *at,
                       record_index);
        return false;
    }
    if (out_record) {
        out_record->name = name;
        out_record->name_size = name_size;
        out_record->dxbc = bytes + *at;
        out_record->dxbc_size = dxbc_size;
    }
    *at += dxbc_size;
    return true;
}

bool dxbc_usbd_table_open(DXBCUSBDTableView* table, const uint8_t* bytes,
                          size_t size, DXBCUSBDDiagnostic* diagnostic) {
    if (table) dxbc_usbd_table_init(table);
    set_diagnostic(diagnostic, DXBC_USBD_INVALID_ARGUMENT, 0u, UINT32_MAX);
    if (!table || !bytes) return false;
    if (size < DXBC_USBD_HEADER_SIZE || memcmp(bytes, "USBD", 4u) != 0) {
        set_diagnostic(diagnostic, DXBC_USBD_BAD_MAGIC, 0u, UINT32_MAX);
        return false;
    }
    uint32_t count = read_u32_le(bytes + 4u);
    if (count == 0u || (size_t)count > (size - 8u) / 9u) {
        set_diagnostic(diagnostic, DXBC_USBD_BAD_RECORD_COUNT, 4u,
                       UINT32_MAX);
        return false;
    }
    size_t at = DXBC_USBD_HEADER_SIZE;
    for (uint32_t index = 0u; index < count; ++index) {
        if (!parse_record(bytes, size, &at, index, NULL, diagnostic)) {
            return false;
        }
    }
    if (at != size) {
        set_diagnostic(diagnostic, DXBC_USBD_TRAILING_BYTES, at, count);
        return false;
    }
    table->bytes = bytes;
    table->size = size;
    table->record_count = count;
    set_diagnostic(diagnostic, DXBC_USBD_OK, size, count);
    return true;
}

bool dxbc_usbd_table_record(const DXBCUSBDTableView* table,
                            uint32_t record_index,
                            DXBCUSBDRecordView* out_record) {
    if (!table || !table->bytes || !out_record ||
        record_index >= table->record_count) {
        return false;
    }
    size_t at = DXBC_USBD_HEADER_SIZE;
    for (uint32_t index = 0u; index <= record_index; ++index) {
        DXBCUSBDRecordView current;
        if (!parse_record(table->bytes, table->size, &at, index, &current,
                          NULL)) {
            return false;
        }
        if (index == record_index) {
            *out_record = current;
            return true;
        }
    }
    return false;
}

bool dxbc_usbd_table_encode(const DXBCUSBDRecordView* records,
                            size_t record_count, uint8_t** out_bytes,
                            size_t* out_size,
                            DXBCUSBDDiagnostic* diagnostic) {
    if (out_bytes) *out_bytes = NULL;
    if (out_size) *out_size = 0u;
    set_diagnostic(diagnostic, DXBC_USBD_INVALID_ARGUMENT, 0u, UINT32_MAX);
    if (!records || record_count == 0u || record_count > UINT32_MAX ||
        !out_bytes || !out_size) {
        return false;
    }

    size_t size = DXBC_USBD_HEADER_SIZE;
    for (size_t index = 0u; index < record_count; ++index) {
        const DXBCUSBDRecordView* record = &records[index];
        if (record->name_size == 0u || record->name_size > UINT32_MAX ||
            !valid_utf8_name(record->name, record->name_size)) {
            set_diagnostic(diagnostic, DXBC_USBD_INVALID_NAME, size,
                           (uint32_t)index);
            return false;
        }
        if (record->dxbc_size > UINT32_MAX ||
            !valid_raw_dxbc(record->dxbc, record->dxbc_size)) {
            set_diagnostic(diagnostic, DXBC_USBD_INVALID_DXBC, size,
                           (uint32_t)index);
            return false;
        }
        size_t overhead = 8u;
        if (dxbc_size_add_overflows(overhead, record->name_size) ||
            dxbc_size_add_overflows(overhead + record->name_size,
                                    record->dxbc_size) ||
            dxbc_size_add_overflows(size, overhead + record->name_size +
                                              record->dxbc_size)) {
            set_diagnostic(diagnostic, DXBC_USBD_SIZE_OVERFLOW, size,
                           (uint32_t)index);
            return false;
        }
        size += overhead + record->name_size + record->dxbc_size;
    }

    uint8_t* bytes = (uint8_t*)mem_alloc(size);
    if (!bytes) {
        set_diagnostic(diagnostic, DXBC_USBD_ALLOCATION_FAILED, 0u,
                       UINT32_MAX);
        return false;
    }
    memcpy(bytes, "USBD", 4u);
    write_u32_le(bytes + 4u, (uint32_t)record_count);
    size_t at = DXBC_USBD_HEADER_SIZE;
    for (size_t index = 0u; index < record_count; ++index) {
        const DXBCUSBDRecordView* record = &records[index];
        write_u32_le(bytes + at, (uint32_t)record->name_size);
        at += 4u;
        memcpy(bytes + at, record->name, record->name_size);
        at += record->name_size;
        write_u32_le(bytes + at, (uint32_t)record->dxbc_size);
        at += 4u;
        memcpy(bytes + at, record->dxbc, record->dxbc_size);
        at += record->dxbc_size;
    }
    *out_bytes = bytes;
    *out_size = size;
    set_diagnostic(diagnostic, DXBC_USBD_OK, size,
                   (uint32_t)record_count);
    return true;
}

const char* dxbc_usbd_status_name(DXBCUSBDStatus status) {
    switch (status) {
        case DXBC_USBD_OK: return "ok";
        case DXBC_USBD_INVALID_ARGUMENT: return "invalid-argument";
        case DXBC_USBD_BAD_MAGIC: return "bad-magic";
        case DXBC_USBD_BAD_RECORD_COUNT: return "bad-record-count";
        case DXBC_USBD_TRUNCATED: return "truncated";
        case DXBC_USBD_INVALID_NAME: return "invalid-name";
        case DXBC_USBD_INVALID_DXBC: return "invalid-dxbc";
        case DXBC_USBD_TRAILING_BYTES: return "trailing-bytes";
        case DXBC_USBD_SIZE_OVERFLOW: return "size-overflow";
        case DXBC_USBD_ALLOCATION_FAILED: return "allocation-failed";
        default: return "unknown";
    }
}
