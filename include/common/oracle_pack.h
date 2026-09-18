// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_ORACLE_PACK_H
#define COMMON_ORACLE_PACK_H

#include "common/variant_key.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORACLE_PACK_FORMAT_VERSION 4U
#define ORACLE_PACK_METADATA_FORMAT_VERSION 1U
#define ORACLE_PACK_DIGEST_SIZE 32U

/*
 * The precision value is deliberately not optional.  UNKNOWN records that the
 * serialized/compiler metadata did not state a precision; it is different
 * from silently substituting a platform default.
 */
typedef enum {
    ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN = 0,
    ORACLE_PACK_RESOURCE_PRECISION_LOW = 1,
    ORACLE_PACK_RESOURCE_PRECISION_MEDIUM = 2,
    ORACLE_PACK_RESOURCE_PRECISION_HIGH = 3,
} OraclePackResourcePrecision;

typedef struct {
    const uint8_t* data;
    size_t size;
} OraclePackBytes;

/*
 * Pack-wide compiler authority.  Each pointer addresses exactly
 * ORACLE_PACK_DIGEST_SIZE bytes; an all-zero digest is rejected as absent.
 * Inputs are copied by the writer; views are borrowed from a fully validated
 * OraclePack and remain valid until it is freed.
 */
typedef struct {
    const uint8_t* compiler_fingerprint;
    const uint8_t* environment_fingerprint;
} OraclePackAuthorityInput;

typedef struct {
    const uint8_t* compiler_fingerprint;
    const uint8_t* environment_fingerprint;
} OraclePackAuthorityView;

/*
 * resource_key is a canonical, non-empty resource identity supplied by the
 * metadata normalizer (for example kind/space/bind point/name).  The writer
 * sorts records by these raw bytes and rejects duplicate identities.
 */
typedef struct {
    OraclePackBytes resource_key;
    OraclePackResourcePrecision precision;
} OraclePackResourcePrecisionInput;

typedef struct {
    /* Canonical metadata excluding the precision table. May be empty. */
    OraclePackBytes canonical_bytes;
    const OraclePackResourcePrecisionInput* resource_precisions;
    size_t resource_precision_count;
} OraclePackNormalizedMetadataInput;

typedef struct {
    const VariantKey* variant_key;
    /* A complete stripped DXBC container, including its DXBC header/chunks. */
    OraclePackBytes stripped_dxbc;
    /* Absent iff both data == NULL and size == 0; present data is non-empty. */
    OraclePackBytes linked_glcore;
    /* Exact length-delimited bytes sent to/received from the compiler oracle. */
    OraclePackBytes compile_request_transcript;
    OraclePackNormalizedMetadataInput normalized_metadata;
    /* Must agree byte-for-byte with the authority supplied to the writer. */
    OraclePackAuthorityInput authority;
} OraclePackEntryInput;

/* Complete, canonical preprocess authority.  The request transcript includes
 * raw source, ordered preprocessing inputs, resolved include paths, compiler
 * identity, and environment identity.  serialized_result is the exact typed
 * preprocess result encoding used by the compiler cache. */
typedef struct {
    OraclePackBytes request_transcript;
    OraclePackBytes serialized_result;
    /* Must agree byte-for-byte with the authority supplied to the writer. */
    OraclePackAuthorityInput authority;
} OraclePackPreprocessInput;

typedef enum {
    ORACLE_PACK_OK = 0,
    ORACLE_PACK_INVALID_ARGUMENT,
    ORACLE_PACK_INVALID_VALUE,
    ORACLE_PACK_INVALID_DXBC,
    ORACLE_PACK_INVALID_METADATA,
    ORACLE_PACK_AUTHORITY_MISMATCH,
    ORACLE_PACK_DUPLICATE_KEY,
    ORACLE_PACK_ALLOCATION_FAILED,
    ORACLE_PACK_SIZE_OVERFLOW,
    ORACLE_PACK_ALREADY_FINALIZED,
    ORACLE_PACK_BAD_MAGIC,
    ORACLE_PACK_UNSUPPORTED_VERSION,
    ORACLE_PACK_TRUNCATED,
    ORACLE_PACK_CORRUPT,
    ORACLE_PACK_NOT_FOUND,
} OraclePackStatus;

typedef struct OraclePackWriter OraclePackWriter;
typedef struct OraclePack OraclePack;

/* Borrowed immutable views remain valid until their OraclePack is freed. */
typedef struct {
    OraclePackBytes variant_key_bytes;
    const uint8_t* variant_key_digest;
    OraclePackBytes stripped_dxbc;
    OraclePackBytes linked_glcore;
    OraclePackBytes compile_request_transcript;
    const uint8_t* compile_request_digest;
    OraclePackBytes normalized_metadata_bytes;
} OraclePackEntryView;

typedef struct {
    OraclePackBytes request_transcript;
    const uint8_t* request_digest;
    OraclePackBytes serialized_result;
    const uint8_t* result_digest;
} OraclePackPreprocessView;

typedef struct {
    OraclePackBytes canonical_bytes;
    size_t resource_precision_count;
} OraclePackNormalizedMetadataView;

typedef struct {
    OraclePackBytes resource_key;
    OraclePackResourcePrecision precision;
} OraclePackResourcePrecisionView;

OraclePackStatus oracle_pack_writer_create(
    const OraclePackAuthorityInput* authority,
    OraclePackWriter** out_writer);
void oracle_pack_writer_free(OraclePackWriter* writer);

/* Copies all input. The caller may mutate/free it after this call returns. */
OraclePackStatus oracle_pack_writer_add(
    OraclePackWriter* writer, const OraclePackEntryInput* input);

/* Exact duplicate records are idempotent; a digest collision or conflicting
 * result for the same request is rejected.  Pack authority is checked first. */
OraclePackStatus oracle_pack_writer_add_preprocess(
    OraclePackWriter* writer, const OraclePackPreprocessInput* input);

/*
 * Sorts entries by VariantKey digest and emits a canonical little-endian pack.
 * Finalization is single-use.  Release output with oracle_pack_bytes_free().
 */
OraclePackStatus oracle_pack_writer_finalize(
    OraclePackWriter* writer, uint8_t** out_data, size_t* out_size);
void oracle_pack_bytes_free(uint8_t* data);

/* Copies and fully validates an in-memory pack; no Unity installation needed. */
OraclePackStatus oracle_pack_open_memory(
    const uint8_t* data, size_t size, OraclePack** out_pack);
void oracle_pack_free(OraclePack* pack);

size_t oracle_pack_entry_count(const OraclePack* pack);
size_t oracle_pack_preprocess_count(const OraclePack* pack);
OraclePackStatus oracle_pack_authority(
    const OraclePack* pack, OraclePackAuthorityView* out_authority);
OraclePackStatus oracle_pack_entry_at(
    const OraclePack* pack, size_t index, OraclePackEntryView* out_entry);
OraclePackStatus oracle_pack_lookup_digest(
    const OraclePack* pack,
    const uint8_t variant_key_digest[ORACLE_PACK_DIGEST_SIZE],
    OraclePackEntryView* out_entry);
OraclePackStatus oracle_pack_lookup_variant_key(
    const OraclePack* pack, const VariantKey* key,
    OraclePackEntryView* out_entry);

OraclePackStatus oracle_pack_preprocess_at(
    const OraclePack* pack, size_t index,
    OraclePackPreprocessView* out_preprocess);
OraclePackStatus oracle_pack_lookup_preprocess_digest(
    const OraclePack* pack,
    const uint8_t request_digest[ORACLE_PACK_DIGEST_SIZE],
    OraclePackPreprocessView* out_preprocess);

/* Decode the canonical metadata envelope retained by an entry. */
OraclePackStatus oracle_pack_metadata_view(
    OraclePackBytes encoded, OraclePackNormalizedMetadataView* out_metadata);
OraclePackStatus oracle_pack_metadata_precision_at(
    OraclePackBytes encoded, size_t index,
    OraclePackResourcePrecisionView* out_precision);

/* Canonically compares current normalized metadata with a pack envelope. */
OraclePackStatus oracle_pack_metadata_matches_input(
    OraclePackBytes encoded,
    const OraclePackNormalizedMetadataInput* input,
    bool* out_equal);

const char* oracle_pack_status_string(OraclePackStatus status);

#endif /* COMMON_ORACLE_PACK_H */
