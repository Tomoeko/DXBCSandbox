// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_HLSL_EXPANSION_H
#define UNITY_HLSL_EXPANSION_H

#include "compiler/unity_compiler_broker.h"

enum { UNITY_HLSL_EXPANSION_BYTE_LIMIT = 2 * 1024 * 1024,
       UNITY_HLSL_EXPANSION_TOKEN_LIMIT = 131072 };

typedef enum {
    UNITY_HLSL_EXPANSION_OK = 0,
    UNITY_HLSL_EXPANSION_INVALID_ARGUMENT,
    UNITY_HLSL_EXPANSION_LIMIT,
    UNITY_HLSL_EXPANSION_MALFORMED,
    UNITY_HLSL_EXPANSION_DIRECTIVE,
    UNITY_HLSL_EXPANSION_ALLOCATION_FAILED,
    UNITY_HLSL_EXPANSION_COMPILER_UNAVAILABLE,
    UNITY_HLSL_EXPANSION_COMPILER_REJECTED,
    UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH
} UnityHlslExpansionStatus;

typedef struct {
    /* Optional injection for transaction limits/tests. Digests must use the
     * canonical serializer, including live include/toolchain authority. */
    bool (*request_digest)(void *context, const UnityCompilerSnippetCompileRequest *request,
                           uint8_t digest[32]);
    bool (*compile)(void *context, const UnityCompilerSnippetCompileRequest *request,
                    UnityCompilerBinaryResponse *response);
    void *context;
} UnityHlslExpansionServices;

typedef struct {
    uint8_t compile_request_digest[32];
    uint8_t preprocess_request_digest[32];
    uint8_t preprocess_controls_digest[32];
    uint8_t expansion_digest[32];
} UnityHlslExpansionEvidence;

/* Request the expansion of this exact D3D11 Cg/HLSL graphics invocation.
 * The only control change is preprocess_only. Optional nonempty probe_source
 * must start with a newline and is appended verbatim; NULL preserves the source
 * byte-for-byte. Before and after canonical identities must agree. Response
 * must not own an earlier result; this call initializes it. The returned response
 * is owned by the caller, including diagnostics on failure. Evidence clears on failure.
 *
 * This observes expanded bytes, not their semantics. A later consumer must
 * validate its own declarations/helpers and bind its receipt to this request,
 * current metadata and active include/toolchain lease. It grants no compiler,
 * byte-equality, import, source-quality or runtime certificate. */
UnityHlslExpansionStatus unity_hlsl_expansion_inspect_request(
    UnityCompilerBroker *broker, const UnityCompilerSnippetCompileRequest *request,
    const char *probe_source, const UnityHlslExpansionServices *services,
    UnityCompilerBinaryResponse *response, UnityHlslExpansionEvidence *evidence);

#endif
