// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPUTE_DOMAIN_H
#define UNITY_COMPUTE_DOMAIN_H

#include "compiler/unity_compute_preprocess.h"

typedef enum {
    UNITY_COMPUTE_DOMAIN_OK = 0,
    UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT,
    UNITY_COMPUTE_DOMAIN_NO_KERNELS,
    UNITY_COMPUTE_DOMAIN_UNSUPPORTED_EMPTY_FAMILY,
    UNITY_COMPUTE_DOMAIN_UNSUPPORTED_FAMILY_SYNTAX,
    UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD,
    UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KERNEL,
    UNITY_COMPUTE_DOMAIN_INVALID_KERNEL_MACRO,
    UNITY_COMPUTE_DOMAIN_CONDITIONAL_CONTEXT_UNAVAILABLE,
    UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED,
    UNITY_COMPUTE_DOMAIN_OUT_OF_MEMORY,
    UNITY_COMPUTE_DOMAIN_INVALID_STATE_INDEX,
    UNITY_COMPUTE_DOMAIN_BUFFER_TOO_SMALL,
    UNITY_COMPUTE_DOMAIN_UNSUPPORTED_API
} UnityComputeDomainStatus;

typedef enum {
    UNITY_COMPUTE_DOMAIN_SCOPE_NONE = 0,
    UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL,
    UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL
} UnityComputeDomainScope;

typedef struct {
    UnityComputeDomainStatus status;
    UnityComputeDomainScope scope;
    /* SIZE_MAX means no corresponding coordinate. */
    size_t family_index;
    size_t choice_index;
    size_t kernel_index;
    size_t macro_index;
    size_t conditional_index;
} UnityComputeDomainDiagnostic;

/* Explicit budgets are required; neither may exceed this fixed ceiling.
 * Families, choices, kernels, macros and each context array are also bounded
 * to 1024 items, individual metadata strings to 1 MiB and all inspected
 * metadata text to 8 MiB. No variant matrix or source copy is allocated. */
#define UNITY_COMPUTE_DOMAIN_MAX_STATES 1048576U

typedef struct {
    /* The actual request whose returned result is being planned. The caller
     * supplies this association; the helper does not establish provenance. */
    const UnityCompilerComputePreprocessRequest* preprocess_request;
    size_t max_variant_count;
    size_t max_kernel_state_count;
} UnityComputeDomainContext;

typedef struct UnityComputeDomain UnityComputeDomain;

typedef struct {
    const char* raw_line;
    /* Choice names are owned by the domain. NULL is an explicit all-underscore
     * default token. Its original spelling stays available in raw_line. */
    const char* const* choices;
    size_t choice_count;
} UnityComputeDomainFamily;

typedef struct {
    size_t kernel_index;
    size_t variant_index;
    const UnityCompilerComputePreprocessedKernel* kernel;
    const UnityCompilerComputePreprocessResult* preprocess_result;
    const UnityCompilerComputePreprocessRequest* preprocess_request;
    /* These spans use the caller's pointer buffer, global first then local.
     * Names are borrowed from the domain; no spelling is normalized. */
    const char* const* global_keywords;
    size_t global_keyword_count;
    const char* const* local_keywords;
    size_t local_keyword_count;
    const char* const* user_keywords;
    size_t user_keyword_count;
    uint64_t requirements;
} UnityComputeDomainState;

/* Builds the full mathematical choice product of supported native family
 * records. Only ASCII identifier tokens separated by single spaces and
 * all-underscore default tokens are accepted. Empty raw families are retained
 * by preprocessing but lack default-choice authority here and fail explicitly.
 * Zero families produce one empty tuple. Repeated names across choices,
 * families, scopes or fixed platform/disabled contexts fail closed.
 *
 * The result and original request, including every pointed-to byte, must stay
 * alive and immutable until domain_free. Their source strings are borrowed;
 * result.source must contain source_size bytes followed by a NUL, with no
 * embedded NUL. Kernel macros and all returned source controls remain exact.
 * Conditional requirements must refer to this known user/platform/disabled
 * vocabulary. Macro-only or otherwise unknown context is unavailable; kernel
 * macros are not silently converted into selected keywords.
 *
 * Output must initially be NULL. Failure leaves it unchanged. Successful
 * planning establishes no native preprocessing-success flag, pragma origin,
 * stripping policy, compiler acceptance, Class72 order, source-quality,
 * import, semantic or runtime certificate. */
UnityComputeDomainStatus unity_compute_domain_create(
    const UnityCompilerComputePreprocessResult* result, const UnityComputeDomainContext* context,
    UnityComputeDomain** out_domain, UnityComputeDomainDiagnostic* diagnostic);
void unity_compute_domain_free(UnityComputeDomain* domain);

size_t unity_compute_domain_variant_count(const UnityComputeDomain* domain);
size_t unity_compute_domain_state_count(const UnityComputeDomain* domain);
size_t unity_compute_domain_family_count(const UnityComputeDomain* domain,
                                         UnityComputeDomainScope scope);
UnityComputeDomainStatus unity_compute_domain_family_at(const UnityComputeDomain* domain,
                                                        UnityComputeDomainScope scope,
                                                        size_t family_index,
                                                        UnityComputeDomainFamily* out_family);

/* Planner order is kernel-major, with the final family varying fastest. It
 * is not a claim about native serialized variant order. A NULL pointer buffer
 * with capacity zero and out_state NULL queries the required pointer count.
 * A successful real call borrows that buffer in out_state. On failure the
 * buffer and out_state are unchanged; out_required_count is set after a valid
 * index has been resolved, including BUFFER_TOO_SMALL. */
UnityComputeDomainStatus
unity_compute_domain_state_at(const UnityComputeDomain* domain, size_t state_index,
                              const char** keyword_buffer, size_t keyword_capacity,
                              size_t* out_required_count, UnityComputeDomainState* out_state,
                              UnityComputeDomainDiagnostic* diagnostic);

/* Interprets the exact returned signed mask as its original uint32 bit pattern.
 * This only checks that bit 0..31 is present; it never adds compiler/session
 * capability authority or rewrites either raw DXC/FXC mask. */
UnityComputeDomainStatus unity_compute_domain_require_api(const UnityComputeDomain* domain,
                                                          uint32_t compiler_platform);

#endif
