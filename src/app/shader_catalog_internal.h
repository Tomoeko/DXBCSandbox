// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_CATALOG_INTERNAL_H
#define SHADER_CATALOG_INTERNAL_H

#include "app/shader_catalog.h"

/* Borrowed unique snapshot, or NULL when missing/ambiguous. Never reopen a
 * pathname to replace the catalog's captured source authority. */
UnityInputSnapshot *shader_catalog_retained_snapshot(const ShaderCatalog *catalog,
                                                     const char *outer_path);

/* Shared canonical catalog occurrence hashing; callers supply validated data. */
void shader_catalog_source_occurrence_digest(
    const UnitySerializedSource *source, const uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE],
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]);

#endif
