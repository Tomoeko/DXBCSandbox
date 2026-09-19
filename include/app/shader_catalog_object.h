// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_CATALOG_OBJECT_H
#define SHADER_CATALOG_OBJECT_H

#include "app/shader_catalog.h"

typedef enum {
    SHADER_CATALOG_OBJECT_OK = 0,
    SHADER_CATALOG_OBJECT_INVALID_ARGUMENT,
    SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED,
    SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE,
    SHADER_CATALOG_OBJECT_COORDINATE_MISMATCH,
    SHADER_CATALOG_OBJECT_SCHEMA_UNAVAILABLE,
    SHADER_CATALOG_OBJECT_DECODE_FAILED
} ShaderCatalogObjectStatus;

typedef struct {
    UnityInputStatus source_status;
    TypeTreeSchemaStatus schema_status;
    ShaderObjectStatus object_status;
    size_t source_matches;
    /* Valid only on SHADER_CATALOG_OBJECT_OK. These are derived from the
     * captured bytes and the resolved schema, never catalog display fields. */
    uint8_t payload_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t schema_digest[COMMON_SHA256_DIGEST_SIZE];
    /* Complete captured outer file, not just its SerializedFile member. */
    uint8_t source_artifact_digest[COMMON_SHA256_DIGEST_SIZE];
    /* Relocation-stable identity of this exact released object. Binds the
     * outer artifact, serialized member, payload, schema and coordinates.
     * Host path and display name are excluded. See the decoder's v1 encoding. */
    uint8_t release_digest[COMMON_SHA256_DIGEST_SIZE];
} ShaderCatalogObjectReport;

/* Decode an owned ClassID 48 record from its unique retained input snapshot.
 * Cataloging must have enabled retain_source_snapshots. No pathname reopen,
 * name-based lookup, or content-only replacement is permitted. The initialized
 * destination is replaced only after the entire snapshot visit validates.
 * The report is always populated; destination remains unchanged on failure. */
ShaderCatalogObjectStatus shader_catalog_decode_object(const ShaderCatalog *catalog,
                                                       const ShaderCatalogRecord *record,
                                                       const TypeTreeSchemaRegistry *registry,
                                                       ShaderObject *destination,
                                                       ShaderCatalogObjectReport *report);

#endif
