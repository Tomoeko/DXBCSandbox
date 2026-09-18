#include "common/common.h"
#include "common/file_io.h"
#include "dxbc/usbd.h"

#include <stdio.h>
#include <string.h>

#ifndef DXBC_USBD_TEST_FIXTURE
#error DXBC_USBD_TEST_FIXTURE must name a checked-in USBD fixture
#endif

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                   \
            goto cleanup;                                                    \
        }                                                                    \
    } while (0)

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

int main(void) {
    int result = 1;
    CommonFileBytes fixture = {0};
    uint8_t* encoded = NULL;
    size_t encoded_size = 0u;
    uint8_t* corrupt = NULL;
    CHECK(common_file_read_regular(DXBC_USBD_TEST_FIXTURE,
                                   16u * 1024u * 1024u,
                                   &fixture) == COMMON_FILE_OK);

    DXBCUSBDTableView table;
    DXBCUSBDDiagnostic diagnostic;
    CHECK(dxbc_usbd_table_open(&table, fixture.data, fixture.size,
                               &diagnostic));
    CHECK(diagnostic.status == DXBC_USBD_OK);
    CHECK(table.record_count == 2u);

    DXBCUSBDRecordView records[2];
    CHECK(dxbc_usbd_table_record(&table, 0u, &records[0]));
    CHECK(dxbc_usbd_table_record(&table, 1u, &records[1]));
    CHECK(records[0].name_size == 6u);
    CHECK(memcmp(records[0].name, "vertex", 6u) == 0);
    CHECK(records[1].name_size == 8u);
    CHECK(memcmp(records[1].name, "fragment", 8u) == 0);
    CHECK(!dxbc_usbd_table_record(&table, 2u, &records[0]));

    CHECK(dxbc_usbd_table_encode(records, 2u, &encoded, &encoded_size,
                                 &diagnostic));
    CHECK(encoded_size == fixture.size);
    CHECK(memcmp(encoded, fixture.data, fixture.size) == 0);
    mem_free(encoded, encoded_size);
    encoded = NULL;
    encoded_size = 0u;

    corrupt = (uint8_t*)malloc(fixture.size + 1u);
    CHECK(corrupt != NULL);
    memcpy(corrupt, fixture.data, fixture.size);
    corrupt[0] = 'X';
    CHECK(!dxbc_usbd_table_open(&table, corrupt, fixture.size, &diagnostic));
    CHECK(diagnostic.status == DXBC_USBD_BAD_MAGIC);

    memcpy(corrupt, fixture.data, fixture.size);
    write_u32_le(corrupt + 4u, 0u);
    CHECK(!dxbc_usbd_table_open(&table, corrupt, fixture.size, &diagnostic));
    CHECK(diagnostic.status == DXBC_USBD_BAD_RECORD_COUNT);

    memcpy(corrupt, fixture.data, fixture.size);
    corrupt[12u] = 0u;
    CHECK(!dxbc_usbd_table_open(&table, corrupt, fixture.size, &diagnostic));
    CHECK(diagnostic.status == DXBC_USBD_INVALID_NAME);

    memcpy(corrupt, fixture.data, fixture.size);
    size_t first_dxbc = 8u + 4u + read_u32_le(fixture.data + 8u) + 4u;
    CHECK(first_dxbc < fixture.size);
    corrupt[first_dxbc + 4u] ^= 1u;
    CHECK(!dxbc_usbd_table_open(&table, corrupt, fixture.size, &diagnostic));
    CHECK(diagnostic.status == DXBC_USBD_INVALID_DXBC);

    memcpy(corrupt, fixture.data, fixture.size);
    corrupt[fixture.size] = 0u;
    CHECK(!dxbc_usbd_table_open(&table, corrupt, fixture.size + 1u,
                                &diagnostic));
    CHECK(diagnostic.status == DXBC_USBD_TRAILING_BYTES);

    CHECK(!dxbc_usbd_table_open(NULL, fixture.data, fixture.size,
                                &diagnostic));
    CHECK(!dxbc_usbd_table_encode(NULL, 0u, &encoded, &encoded_size,
                                  &diagnostic));

    result = 0;

cleanup:
    if (encoded) mem_free(encoded, encoded_size);
    free(corrupt);
    common_file_bytes_dispose(&fixture);
    if (atomic_load_explicit(&g_allocations_count,
                             memory_order_relaxed) != 0u ||
        atomic_load_explicit(&g_allocated_bytes,
                             memory_order_relaxed) != 0u) {
        fprintf(stderr, "tracked allocation leak after USBD tests\n");
        result = 1;
    }
    if (result != 0) return result;
    puts("USBD unit tests passed");
    return 0;
}
