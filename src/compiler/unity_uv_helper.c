// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_uv_helper.h"
#include "compiler/unity_hlsl_expansion_internal.h"
#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

#define UV_INTERNAL "UnityStereoScreenSpaceUVAdjustInternal"
#define UV_PROBE "dxbc_unity_uv_contract_probe"

/* These are closed semantic token contracts, not copied include files or a
 * stock-Editor hash. The actual selected include closure remains authority. */
static const char *const definitions[] = {
    "inline float2 " UV_INTERNAL "(float2 uv, float4 scaleAndOffset) {"
    "return uv.xy * scaleAndOffset.xy + scaleAndOffset.zw;}",
    "inline float4 " UV_INTERNAL "(float4 uv, float4 scaleAndOffset) {"
    "return float4(" UV_INTERNAL "(uv.xy, scaleAndOffset)," UV_INTERNAL
    "(uv.zw, scaleAndOffset));}",
    "float4 " UV_PROBE "(float4 dxbc_uv, float4 dxbc_scale_offset) {"
    "return " UV_INTERNAL "(dxbc_uv, dxbc_scale_offset);}",
};

bool unity_uv_helper_append_probe(StringBuilder *source) {
    if (!source || !sb_ok(source))
        return false;
    sb_append(source, "\nfloat4 " UV_PROBE "(float4 dxbc_uv, float4 dxbc_scale_offset) {\n"
                      "    return " HLSL_UNITY_UV_FUNCTION "(dxbc_uv, dxbc_scale_offset);\n"
                      "}\n");
    return sb_ok(source);
}

static UnityUvHelperStatus inspect_item(const uint8_t *source, const SourceToken *tokens,
                                        size_t begin, size_t header_end, size_t end, bool seen[3],
                                        UnityUvHelperExpansion *expansion) {
    bool helper = false, probe = false;
    for (size_t i = begin; i < header_end; ++i) {
        helper = helper || source_token_equals(source, tokens[i], UV_INTERNAL);
        probe = probe || source_token_equals(source, tokens[i], UV_PROBE);
    }
    if (!helper && !probe)
        return UNITY_UV_HELPER_OK;
    for (size_t kind = 0; kind < 3; ++kind) {
        if (!unity_hlsl_expansion_same_tokens(source, tokens, begin, end, definitions[kind]))
            continue;
        if (seen[kind])
            return UNITY_UV_HELPER_DUPLICATE_DEFINITION;
        seen[kind] = true;
        UnityUvHelperSourceRange *range =
            kind == 2 ? &expansion->probe : &expansion->definitions[kind];
        *range = (UnityUvHelperSourceRange){tokens[begin].begin, tokens[end - 1U].end};
        return UNITY_UV_HELPER_OK;
    }
    return probe ? UNITY_UV_HELPER_CHANGED_INVOCATION : UNITY_UV_HELPER_CHANGED_DEFINITION;
}

UnityUvHelperStatus unity_uv_helper_validate_expansion(const uint8_t *source, size_t size,
                                                       UnityUvHelperExpansion *out_expansion) {
    if (!out_expansion)
        return UNITY_UV_HELPER_INVALID_ARGUMENT;
    memset(out_expansion, 0, sizeof(*out_expansion));
    if (!source && size)
        return UNITY_UV_HELPER_INVALID_ARGUMENT;
    if (size > UNITY_UV_HELPER_EXPANSION_LIMIT)
        return UNITY_UV_HELPER_LIMIT_EXCEEDED;
    if (!size)
        return UNITY_UV_HELPER_MISSING_DEFINITION;
    UnityHlslExpansionTokens stream;
    const UnityHlslExpansionStatus lexical = unity_hlsl_expansion_tokenize(source, size, &stream);
    UnityUvHelperStatus status;
    switch (lexical) {
    case UNITY_HLSL_EXPANSION_OK: status = UNITY_UV_HELPER_OK; break;
    case UNITY_HLSL_EXPANSION_INVALID_ARGUMENT: status = UNITY_UV_HELPER_INVALID_ARGUMENT; break;
    case UNITY_HLSL_EXPANSION_LIMIT: status = UNITY_UV_HELPER_LIMIT_EXCEEDED; break;
    case UNITY_HLSL_EXPANSION_MALFORMED: status = UNITY_UV_HELPER_MALFORMED_EXPANSION; break;
    case UNITY_HLSL_EXPANSION_DIRECTIVE: status = UNITY_UV_HELPER_UNSUPPORTED_DIRECTIVE; break;
    default: status = UNITY_UV_HELPER_OUT_OF_MEMORY; break;
    }
    SourceToken *tokens = stream.tokens;
    const size_t count = stream.count;
    UnityUvHelperExpansion expansion = {0};
    size_t depth = 0, item_begin = 0, header_end = 0;
    bool seen[3] = {0};
    for (size_t i = 0; status == UNITY_UV_HELPER_OK && i < count; ++i) {
        if (source_token_equals(source, tokens[i], "{")) {
            if (depth == 0)
                header_end = i;
            ++depth;
        } else if (source_token_equals(source, tokens[i], "}")) {
            if (depth == 0) {
                status = UNITY_UV_HELPER_MALFORMED_EXPANSION;
            } else if (--depth == 0) {
                status =
                    inspect_item(source, tokens, item_begin, header_end, i + 1U, seen, &expansion);
                item_begin = i + 1U;
            }
        } else if (depth == 0 && source_token_equals(source, tokens[i], ";")) {
            status = inspect_item(source, tokens, item_begin, i, i + 1U, seen, &expansion);
            item_begin = i + 1U;
        }
    }
    if (status == UNITY_UV_HELPER_OK && (depth || item_begin != count))
        status = UNITY_UV_HELPER_MALFORMED_EXPANSION;
    if (status == UNITY_UV_HELPER_OK && (!seen[0] || !seen[1] || !seen[2]))
        status = UNITY_UV_HELPER_MISSING_DEFINITION;
    unity_hlsl_expansion_tokens_dispose(&stream);
    if (status == UNITY_UV_HELPER_OK) {
        common_sha256(source, size, expansion.expansion_digest);
        *out_expansion = expansion;
    }
    return status;
}

UnityUvHelperStatus unity_uv_helper_inspect_request(
    UnityCompilerBroker *broker, const UnityCompilerSnippetCompileRequest *request,
    const UnityUvHelperServices *services, UnityCompilerBinaryResponse *response,
    UnityUvHelperEvidence *evidence) {
    if (response) unity_compiler_binary_response_init(response);
    if (evidence) memset(evidence, 0, sizeof(*evidence));
    if (!response || !evidence || !request ||
        (request->shader_type != 0 && request->shader_type != 1)) return UNITY_UV_HELPER_INVALID_ARGUMENT;
    StringBuilder probe;
    sb_init(&probe);
    if (!unity_uv_helper_append_probe(&probe)) {
        sb_free(&probe);
        return UNITY_UV_HELPER_OUT_OF_MEMORY;
    }
    UnityHlslExpansionEvidence expansion;
    const UnityHlslExpansionStatus observed = unity_hlsl_expansion_inspect_request(
        broker, request, probe.buf, services, response, &expansion);
    sb_free(&probe);
    UnityUvHelperStatus status;
    switch (observed) {
    case UNITY_HLSL_EXPANSION_OK: status = UNITY_UV_HELPER_OK; break;
    case UNITY_HLSL_EXPANSION_INVALID_ARGUMENT: status = UNITY_UV_HELPER_INVALID_ARGUMENT; break;
    case UNITY_HLSL_EXPANSION_LIMIT: status = UNITY_UV_HELPER_LIMIT_EXCEEDED; break;
    case UNITY_HLSL_EXPANSION_COMPILER_UNAVAILABLE: status = UNITY_UV_HELPER_COMPILER_UNAVAILABLE; break;
    case UNITY_HLSL_EXPANSION_COMPILER_REJECTED: status = UNITY_UV_HELPER_COMPILER_REJECTED; break;
    case UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH: status = UNITY_UV_HELPER_AUTHORITY_MISMATCH; break;
    case UNITY_HLSL_EXPANSION_MALFORMED: status = UNITY_UV_HELPER_MALFORMED_EXPANSION; break;
    case UNITY_HLSL_EXPANSION_DIRECTIVE: status = UNITY_UV_HELPER_UNSUPPORTED_DIRECTIVE; break;
    default: status = UNITY_UV_HELPER_OUT_OF_MEMORY; break;
    }
    UnityUvHelperEvidence checked = {0};
    if (status == UNITY_UV_HELPER_OK) {
        status = unity_uv_helper_validate_expansion(response->data, response->size, &checked.expansion);
        if (status == UNITY_UV_HELPER_OK) {
            memcpy(checked.compile_request_digest, expansion.compile_request_digest, 32);
            memcpy(checked.preprocess_request_digest, expansion.preprocess_request_digest, 32);
            *evidence = checked;
        }
    }
    return status;
}

const char *unity_uv_helper_status_name(UnityUvHelperStatus status) {
    switch (status) {
    case UNITY_UV_HELPER_OK:
        return "ok";
    case UNITY_UV_HELPER_INVALID_ARGUMENT:
        return "invalid_argument";
    case UNITY_UV_HELPER_LIMIT_EXCEEDED:
        return "limit_exceeded";
    case UNITY_UV_HELPER_MALFORMED_EXPANSION:
        return "malformed_expansion";
    case UNITY_UV_HELPER_UNSUPPORTED_DIRECTIVE:
        return "unsupported_directive";
    case UNITY_UV_HELPER_MISSING_DEFINITION:
        return "missing_definition";
    case UNITY_UV_HELPER_CHANGED_DEFINITION:
        return "changed_definition";
    case UNITY_UV_HELPER_CHANGED_INVOCATION:
        return "changed_invocation";
    case UNITY_UV_HELPER_DUPLICATE_DEFINITION:
        return "duplicate_definition";
    case UNITY_UV_HELPER_OUT_OF_MEMORY:
        return "out_of_memory";
    case UNITY_UV_HELPER_COMPILER_UNAVAILABLE:
        return "compiler_unavailable";
    case UNITY_UV_HELPER_COMPILER_REJECTED:
        return "compiler_rejected";
    case UNITY_UV_HELPER_AUTHORITY_MISMATCH:
        return "authority_mismatch";
    default:
        return "unknown";
    }
}
