// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_HLSL_CBUFFER_INVENTORY_H
#define UNITY_HLSL_CBUFFER_INVENTORY_H

#include "compiler/unity_hlsl_expansion.h"
#include "io/parameter_layout.h"

enum { UNITY_HLSL_CBUFFER_NAME_LIMIT = 96, UNITY_HLSL_CBUFFER_COUNT_LIMIT = 14,
       UNITY_HLSL_CBUFFER_FIELD_LIMIT = 1024 };

typedef enum {
    UNITY_HLSL_CBUFFER_OK = 0,
    UNITY_HLSL_CBUFFER_INVALID_ARGUMENT,
    UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT,
    UNITY_HLSL_CBUFFER_MALFORMED_EXPANSION,
    UNITY_HLSL_CBUFFER_UNSUPPORTED_DIRECTIVE,
    UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION,
    UNITY_HLSL_CBUFFER_MISSING_DECLARATION,
    UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION,
    UNITY_HLSL_CBUFFER_ALLOCATION_FAILED
} UnityHlslCBufferStatus;

typedef enum {
    UNITY_HLSL_CBUFFER_FLOAT = 0,
    UNITY_HLSL_CBUFFER_HALF,
    UNITY_HLSL_CBUFFER_INT,
    UNITY_HLSL_CBUFFER_UINT,
    UNITY_HLSL_CBUFFER_BOOL
} UnityHlslCBufferScalar;

typedef struct {
    /* Explicit selected D3D11 storage contract, default false. The owning
     * transaction must bind legacy half storage to its captured precision
     * model. An expansion spelling half does not itself authorize this choice. */
    bool legacy_half_is_float32;
} UnityHlslCBufferStoragePolicy;

typedef struct {
    char name[UNITY_HLSL_CBUFFER_NAME_LIMIT];
    UnityHlslCBufferScalar scalar;
    uint8_t rows, columns;
    bool is_matrix;
    uint32_t byte_offset, byte_size;
    /* Actual expanded-response declaration spans, never entry-source spans. */
    size_t source_begin, source_end;
} UnityHlslCBufferField;

typedef struct {
    char name[UNITY_HLSL_CBUFFER_NAME_LIMIT];
    uint32_t byte_size;
    size_t first_field, field_count;
    size_t source_begin, source_end;
} UnityHlslCBufferBlock;

typedef struct {
    /* Owned ordered declarations; blocks follow the requested name order,
     * fields retain each actual block's declaration order. No register number
     * is inferred from include order; current target bindings stay separate. */
    UnityHlslCBufferBlock blocks[UNITY_HLSL_CBUFFER_COUNT_LIMIT];
    size_t block_count;
    UnityHlslCBufferField *fields;
    size_t field_count;
    UnityHlslCBufferStoragePolicy storage;
    uint8_t expansion_digest[32];
} UnityHlslCBufferInventory;

void unity_hlsl_cbuffer_inventory_init(UnityHlslCBufferInventory *inventory);
void unity_hlsl_cbuffer_inventory_dispose(UnityHlslCBufferInventory *inventory);

/* Closed declaration grammar over the shared expanded-HLSL scanner: plain
 * scalar/vectors and column-major float4x4 matrices, no arrays/structs/packoffset,
 * explicit registers, row-major or precision/packing directives. Complete
 * block order, packing and extent are derived from actual expanded syntax.
 * Duplicate blocks/fields and top-level aliases of requested names reject.
 * Unrelated source is scanned/balanced, but is not semantically inventoried.
 *
 * The destination must be initialized and empty. Failure leaves it empty.
 * This is declaration-only evidence. Bind the expanded bytes to the same
 * current compiler request/lease and intersect fields with actual metadata
 * and instruction reads before use. It never closes an entire include unit,
 * infers an original source declaration, or promotes source quality. */
UnityHlslCBufferStatus unity_hlsl_cbuffer_inventory_build(
    const uint8_t *expanded, size_t size, const char *const *names, size_t name_count,
    UnityHlslCBufferStoragePolicy storage, UnityHlslCBufferInventory *out);

/* Replay all declarations and compare every owned typed/span fact. A matching
 * expansion digest alone is insufficient for a mutable caller-owned receipt. */
bool unity_hlsl_cbuffer_inventory_matches(
    const UnityHlslCBufferInventory *inventory, const uint8_t *expanded, size_t size,
    const char *const *names, size_t name_count, UnityHlslCBufferStoragePolicy storage);

/* Narrow float-layout intersection using the existing metadata decoder. This
 * does not authorize a b-register, a metadata owner, or an executable read. */
bool unity_hlsl_cbuffer_field_matches_float_parameter(const UnityHlslCBufferField *field,
    const SerializedProgramParameters *parameters, const SerializedVariable *variable);

const char *unity_hlsl_cbuffer_status_name(UnityHlslCBufferStatus status);

#endif
