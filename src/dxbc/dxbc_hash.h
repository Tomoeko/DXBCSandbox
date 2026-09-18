// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_HASH_H
#define DXBC_HASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Verifies that the DXBC container's header checksum matches the computed hash
 * of its bytecode contents.
 *
 * @param data Pointer to the start of the DXBC container data.
 * @param size The size of the DXBC container.
 * @return true if the hash matches, false otherwise.
 */
bool dxbc_verify_hash(const uint8_t* data, size_t size);

/* Computes the checksum encoded in bytes 4..19 from the container's declared
 * total size.  This is primarily useful when producing or mutating a DXBC
 * container; verification remains the normal read path. */
bool dxbc_compute_hash(const uint8_t* data, size_t size,
                       uint8_t out_hash[16]);

#endif /* DXBC_HASH_H */
