// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_ORACLE_METADATA_H
#define COMMON_ORACLE_METADATA_H

#include "common/oracle_pack.h"
#include "io/subprogram_metadata.h"

#include <stddef.h>
#include <stdint.h>

#define ORACLE_METADATA_FORMAT_VERSION 2U
#define ORACLE_METADATA_HEADER_SIZE 32U
#define ORACLE_METADATA_RESOURCE_KEY_FORMAT_VERSION 1U
#define ORACLE_METADATA_RESOURCE_KEY_HEADER_SIZE 48U

/*
 * Canonical metadata byte format, version 2. All integers are little-endian.
 * Strings are u32 byte length followed by the bytes before their required NUL.
 * Array counts are u64 and arrays retain their authoritative input order.
 *
 * Header:
 *   u8[8] "DXBCMETA", u32 version, u32 header size, u64 total size,
 *   u32 SerializedProgramParameters.version, u32 is_binary.
 * Body:
 *   u64 constant-buffer count, followed by every constant buffer;
 *   u64 resource count, followed by every resource.
 * Constant buffers include name, u32 SerializedConstantBufferRole, size, both
 * partial flags, variables, and structs. The role field was added in version 2
 * because Unity's binary parameter block stores a loose-parameter area before
 * the named constant-buffer array, and both can conventionally be called
 * "$Globals". Variables/members include name and all six layout words.
 * Structs include name, all three layout words, and members. Resources include
 * name, type, bind index, array size, dimension, sampler index, multisampled,
 * original index, sampler state, and both extra words.
 *
 * Resource keys are independently versioned non-empty byte strings:
 *   u8[8] "DXBCRKEY", u32 version, u32 header size, u64 total size,
 *   u64 resource array index, u32 raw type, u32 bind index,
 *   u32 name byte length, u32 reserved zero, followed by name bytes.
 * Including the authoritative array index makes keys unique even when corrupt
 * or unusual metadata repeats a kind/bind/name tuple.
 */

typedef enum {
    ORACLE_METADATA_OK = 0,
    ORACLE_METADATA_INVALID_ARGUMENT,
    ORACLE_METADATA_INVALID_SHAPE,
    ORACLE_METADATA_INVALID_VALUE,
    ORACLE_METADATA_SIZE_OVERFLOW,
    ORACLE_METADATA_ALLOCATION_FAILED,
} OracleMetadataStatus;

/* Precision authority is associated with the serialized resource array index.
 * Duplicate indices are rejected. UNKNOWN is also a valid explicit value. */
typedef struct {
    size_t resource_index;
    OraclePackResourcePrecision precision;
} OracleMetadataPrecisionOverride;

typedef struct {
    /* Pass this view directly as OraclePackEntryInput.normalized_metadata.
     * Every pointer is owned by this result and remains valid until free. */
    OraclePackNormalizedMetadataInput pack_input;

    /* Owns the contiguous storage referenced by every resource_key view. */
    uint8_t* resource_key_storage;
    size_t resource_key_storage_size;
} OracleMetadataNormalization;

void oracle_metadata_normalization_init(
    OracleMetadataNormalization* normalization);
void oracle_metadata_normalization_free(
    OracleMetadataNormalization* normalization);

/*
 * `output` must be initialized before its first use. On failure, its previous
 * value is unchanged. No precision is inferred from resource type, name,
 * platform, or any other metadata: resources without an override are emitted
 * explicitly as ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN.
 */
OracleMetadataStatus oracle_metadata_normalize(
    const SerializedProgramParameters* parameters,
    const OracleMetadataPrecisionOverride* precision_overrides,
    size_t precision_override_count,
    OracleMetadataNormalization* output);

const char* oracle_metadata_status_string(OracleMetadataStatus status);

#endif /* COMMON_ORACLE_METADATA_H */
