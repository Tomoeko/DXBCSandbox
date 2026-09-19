// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_UV_HELPER_H
#define UNITY_UV_HELPER_H

#include "common/string_builder.h"
#include "compiler/unity_compiler_broker.h"

#include <stddef.h>
#include <stdint.h>

#define UNITY_UV_HELPER_LIFT_ID "unity-packed-uv-adjust"
#define UNITY_UV_HELPER_LIFT_VERSION 1U
#define UNITY_UV_HELPER_EXPANSION_LIMIT (2U * 1024U * 1024U)

typedef enum {
    UNITY_UV_HELPER_OK = 0,
    UNITY_UV_HELPER_INVALID_ARGUMENT,
    UNITY_UV_HELPER_LIMIT_EXCEEDED,
    UNITY_UV_HELPER_MALFORMED_EXPANSION,
    UNITY_UV_HELPER_UNSUPPORTED_DIRECTIVE,
    UNITY_UV_HELPER_MISSING_DEFINITION,
    UNITY_UV_HELPER_CHANGED_DEFINITION,
    UNITY_UV_HELPER_CHANGED_INVOCATION,
    UNITY_UV_HELPER_DUPLICATE_DEFINITION,
    UNITY_UV_HELPER_OUT_OF_MEMORY,
    UNITY_UV_HELPER_COMPILER_UNAVAILABLE,
    UNITY_UV_HELPER_COMPILER_REJECTED,
    UNITY_UV_HELPER_AUTHORITY_MISMATCH
} UnityUvHelperStatus;

typedef struct {
    size_t begin;
    size_t end;
} UnityUvHelperSourceRange;

typedef struct {
    /* Exact byte ranges in the expanded compiler response: float2 overload,
     * float4 overload, then the public-macro probe. No include text is owned. */
    UnityUvHelperSourceRange definitions[2];
    UnityUvHelperSourceRange probe;
    uint8_t expansion_digest[32];
} UnityUvHelperExpansion;

/* Append to the exact candidate snippet only for a preprocess-only request.
 * The caller preserves all selected request controls and include authority.
 * This unused function observes the public macro's expansion at this point. */
bool unity_uv_helper_append_probe(StringBuilder *source);

/* Closed token contract for the selected float overloads and macro expansion.
 * Whitespace, comments and harmless line/warning directives are permitted;
 * altered operations, types, extra overloads, prototypes and duplicate probes
 * reject. The complete response is bounded and scanned, including its tail.
 *
 * Success is only definition/expansion evidence, not a compiler, instruction,
 * ABI or equivalence certificate. The caller must bind this response to the
 * actual typed request, prove each call's lanes, and compare complete DXBC.
 * Failure clears out_expansion. */
UnityUvHelperStatus unity_uv_helper_validate_expansion(const uint8_t *source, size_t size,
                                                       UnityUvHelperExpansion *out_expansion);
typedef struct {
    /* Optional service injection for the owning transaction's limits and tests.
     * NULL callbacks use the broker. A digest must use the canonical request
     * serializer, including live source/include/toolchain authority. */
    bool (*request_digest)(void *context, const UnityCompilerSnippetCompileRequest *request,
                           uint8_t digest[32]);
    bool (*compile)(void *context, const UnityCompilerSnippetCompileRequest *request,
                    UnityCompilerBinaryResponse *response);
    void *context;
} UnityUvHelperServices;

typedef struct {
    UnityUvHelperExpansion expansion;
    uint8_t compile_request_digest[32];
    uint8_t preprocess_request_digest[32];
} UnityUvHelperEvidence;

/* Inspect this exact D3D11 vertex/fragment compile request. Appends only the
 * probe and sets preprocess_only; flags, keywords, stage and all other controls
 * stay unchanged. Canonical identities are checked before and after the work.
 * The caller owns the returned response (including failure diagnostics) and
 * frees it with unity_compiler_binary_response_free(). Evidence is cleared on
 * failure. The eventual compile must match compile_request_digest, and still
 * needs the independent instruction/ABI/full-container transaction gates. */
UnityUvHelperStatus unity_uv_helper_inspect_request(
    UnityCompilerBroker *broker, const UnityCompilerSnippetCompileRequest *request,
    const UnityUvHelperServices *services, UnityCompilerBinaryResponse *response,
    UnityUvHelperEvidence *evidence);

const char *unity_uv_helper_status_name(UnityUvHelperStatus status);

#endif
