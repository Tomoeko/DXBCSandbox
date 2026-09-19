// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_INCLUDE_CLOSURE_H
#define UNITY_INCLUDE_CLOSURE_H

#include "compiler/unity_compiler_cache.h"

/* Conservative closure of literal includes in explicit Unity HLSL/CG
 * programs. Conditional branches are all visited. Roots include the source
 * directory first and the compiler working directory last. Native implicit
 * HLSLSupport/UnityShaderVariables headers participate even when omitted from
 * the source. Nonliteral includes, special nodes, and exhausted bounds fail.
 * The returned lease owns every existing and missing candidate observation;
 * it must remain valid through compilation/publication. No compiler runs. */
bool usc_include_closure_create(const uint8_t *source, size_t source_size, const char *const *roots,
                                size_t root_count,
                                const uint8_t environment_digest[USC_CACHE_DIGEST_SIZE],
                                uint8_t closure_environment_digest[USC_CACHE_DIGEST_SIZE],
                                UscCacheToolchainLease **out_lease);

#endif
