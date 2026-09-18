#ifndef DXBCSANDBOX_TEST_FIXTURE_H
#define DXBCSANDBOX_TEST_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decodes a whitespace-tolerant, canonically padded Base64 fixture.  The
 * returned exact-size allocation uses mem_alloc/mem_free. */
bool test_fixture_decode_base64(const char* path, uint8_t** out_bytes,
                                size_t* out_size);

#endif /* DXBCSANDBOX_TEST_FIXTURE_H */
