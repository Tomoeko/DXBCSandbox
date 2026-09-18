// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_parser_internal.h"
#include <string.h>

#define DXBC_HEADER_SIZE 32u
#define USBD_HEADER_SIZE 8u
#define UNITY_DXBC_HEADER_SIZE 38u

static bool read_le_u32_at(const uint8_t* data, size_t size, size_t offset,
                           uint32_t* value) {
    uint32_t raw;
    if (!data || !value || offset > size || size - offset < sizeof(raw)) {
        return false;
    }
    memcpy(&raw, data + offset, sizeof(raw));
    *value = read_le32(raw);
    return true;
}

static bool raw_dxbc_view(const uint8_t* data, size_t size,
                          DXBCContainerView* out_view) {
    uint32_t total_size;
    if (!data || !out_view || size < DXBC_HEADER_SIZE ||
        memcmp(data, "DXBC", 4) != 0 ||
        !read_le_u32_at(data, size, 24, &total_size) ||
        total_size < DXBC_HEADER_SIZE || total_size > size) {
        return false;
    }
    out_view->data = data;
    out_view->size = total_size;
    return true;
}

static bool usbd_view_first(const uint8_t* data, size_t size,
                            DXBCContainerView* out_view) {
    uint32_t record_count;
    size_t offset = USBD_HEADER_SIZE;
    DXBCContainerView first = {0};

    if (size < USBD_HEADER_SIZE || memcmp(data, "USBD", 4) != 0 ||
        !read_le_u32_at(data, size, 4, &record_count) || record_count == 0 ||
        record_count > (size - USBD_HEADER_SIZE) / 9u) {
        return false;
    }

    for (uint32_t i = 0; i < record_count; ++i) {
        uint32_t name_length;
        uint32_t payload_length;
        DXBCContainerView record;

        if (!read_le_u32_at(data, size, offset, &name_length)) {
            return false;
        }
        offset += 4;
        if (name_length == 0 || name_length > size - offset) {
            return false;
        }
        offset += name_length;
        if (!read_le_u32_at(data, size, offset, &payload_length)) {
            return false;
        }
        offset += 4;
        if (payload_length > size - offset ||
            !raw_dxbc_view(data + offset, payload_length, &record) ||
            record.size != payload_length) {
            return false;
        }
        if (i == 0) {
            first = record;
        }
        offset += payload_length;
    }

    if (offset != size) {
        return false;
    }
    *out_view = first;
    return true;
}

static bool unity_program_view(const uint8_t* data, size_t size,
                               DXBCContainerView* out_view) {
    if (!data || size <= UNITY_DXBC_HEADER_SIZE || data[0] != 2) {
        return false;
    }
    if (!raw_dxbc_view(data + UNITY_DXBC_HEADER_SIZE,
                       size - UNITY_DXBC_HEADER_SIZE, out_view)) {
        return false;
    }
    return out_view->size == size - UNITY_DXBC_HEADER_SIZE;
}

bool dxbc_container_view_first(const uint8_t* data, size_t size,
                               DXBCContainerView* out_view) {
    if (!out_view) {
        return false;
    }
    out_view->data = NULL;
    out_view->size = 0;
    if (raw_dxbc_view(data, size, out_view)) {
        return true;
    }
    if (unity_program_view(data, size, out_view)) {
        return true;
    }
    return usbd_view_first(data, size, out_view);
}
