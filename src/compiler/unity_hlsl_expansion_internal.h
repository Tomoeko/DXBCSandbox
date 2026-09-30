// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_HLSL_EXPANSION_INTERNAL_H
#define UNITY_HLSL_EXPANSION_INTERNAL_H

#include "common/source_scan.h"
#include "compiler/unity_hlsl_expansion.h"

/* Private shared lexical boundary for compiler-expanded HLSL. The original
 * source is borrowed; tokens retain byte offsets. Only harmless line/warning
 * directives are removed. This supplies no declaration or helper semantics. */
typedef struct {
    SourceToken *tokens;
    size_t count;
} UnityHlslExpansionTokens;

UnityHlslExpansionStatus unity_hlsl_expansion_tokenize(const uint8_t *source, size_t size,
                                                      UnityHlslExpansionTokens *out);
void unity_hlsl_expansion_tokens_dispose(UnityHlslExpansionTokens *tokens);
bool unity_hlsl_expansion_same_tokens(const uint8_t *source, const SourceToken *tokens,
                                      size_t begin, size_t end, const char *expected);

#endif
