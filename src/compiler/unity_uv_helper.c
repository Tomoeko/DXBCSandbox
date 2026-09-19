// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_uv_helper.h"
#include "common/source_scan.h"
#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

#define UV_TOKEN_LIMIT 131072U
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

static bool same_tokens(const uint8_t *source, const SourceToken *tokens, size_t begin, size_t end,
                        const char *expected) {
    SourceScanner scanner = {(const uint8_t *)expected, strlen(expected), 0};
    for (size_t i = begin; i < end; ++i) {
        const SourceToken token = source_scan_next(&scanner);
        if (token.kind != tokens[i].kind ||
            token.end - token.begin != tokens[i].end - tokens[i].begin ||
            memcmp(scanner.source + token.begin, source + tokens[i].begin, token.end - token.begin))
            return false;
    }
    return source_scan_next(&scanner).kind == SOURCE_TOKEN_END;
}

/* Expanded HLSL may retain diagnostics and line positions. Neither can alter
 * arithmetic. All other directives (especially residual macros) reject. */
static bool directive_supported(SourceScanner *scanner, SourceToken hash) {
    size_t line_begin = hash.begin;
    while (line_begin && scanner->source[line_begin - 1U] != '\n' &&
           scanner->source[line_begin - 1U] != '\r') {
        const uint8_t c = scanner->source[--line_begin];
        if (c != ' ' && c != '\t')
            return false;
    }
    size_t end = hash.end;
    while (end < scanner->size && scanner->source[end] != '\n' && scanner->source[end] != '\r')
        ++end;
    SourceScanner line = {scanner->source, end, hash.end};
    const SourceToken name = source_scan_next(&line);
    if (source_token_equals(line.source, name, "line")) {
        const SourceToken number = source_scan_next(&line);
        if (number.kind != SOURCE_TOKEN_NUMBER || number.begin == number.end)
            return false;
        for (size_t i = number.begin; i < number.end; ++i)
            if (line.source[i] < '0' || line.source[i] > '9')
                return false;
        SourceToken tail = source_scan_next(&line);
        if (tail.kind == SOURCE_TOKEN_QUOTED && line.source[tail.begin] == '"')
            tail = source_scan_next(&line);
        if (tail.kind != SOURCE_TOKEN_END)
            return false;
    } else {
        SourceToken tokens[8];
        tokens[0] = name;
        for (size_t i = 1; i < 8; ++i)
            tokens[i] = source_scan_next(&line);
        if (tokens[7].kind != SOURCE_TOKEN_END)
            return false;
        bool matched = false;
        const char *warnings[] = {"pragma warning(disable:3205)", "pragma warning(disable:3568)",
                                  "pragma warning(disable:3571)", "pragma warning(disable:3206)"};
        for (size_t i = 0; i < sizeof(warnings) / sizeof(warnings[0]); ++i)
            matched = matched || same_tokens(line.source, tokens, 0, 7, warnings[i]);
        if (!matched)
            return false;
    }
    scanner->offset = end;
    return true;
}

static UnityUvHelperStatus tokenize(const uint8_t *source, size_t size, SourceToken *tokens,
                                    size_t *count) {
    /* A compiler expansion must not contain preprocessing splices. Scanning
     * them as physical lines would give comments a different meaning. */
    for (size_t i = 0; i < size; ++i) {
        if (!source[i] ||
            (source[i] == '\\' && i + 1U < size &&
             (source[i + 1U] == '\n' || source[i + 1U] == '\r')) ||
            (source[i] == '?' && i + 2U < size && source[i + 1U] == '?' &&
             strchr("=/'()!<>-", source[i + 2U])))
            return UNITY_UV_HELPER_MALFORMED_EXPANSION;
    }
    SourceScanner scanner = {source, size, 0};
    *count = 0;
    for (;;) {
        SourceToken token = source_scan_next(&scanner);
        if (token.kind == SOURCE_TOKEN_END)
            return UNITY_UV_HELPER_OK;
        if (token.kind == SOURCE_TOKEN_INVALID)
            return UNITY_UV_HELPER_MALFORMED_EXPANSION;
        if (source_token_equals(source, token, "#")) {
            if (!directive_supported(&scanner, token))
                return UNITY_UV_HELPER_UNSUPPORTED_DIRECTIVE;
            continue;
        }
        if (*count == UV_TOKEN_LIMIT)
            return UNITY_UV_HELPER_LIMIT_EXCEEDED;
        tokens[(*count)++] = token;
    }
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
        if (!same_tokens(source, tokens, begin, end, definitions[kind]))
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
    const size_t capacity = size < UV_TOKEN_LIMIT ? size : UV_TOKEN_LIMIT;
    SourceToken *tokens = malloc(capacity * sizeof(*tokens));
    if (!tokens)
        return UNITY_UV_HELPER_OUT_OF_MEMORY;
    UnityUvHelperExpansion expansion = {0};
    size_t count = 0;
    UnityUvHelperStatus status = tokenize(source, size, tokens, &count);
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
    free(tokens);
    if (status == UNITY_UV_HELPER_OK) {
        common_sha256(source, size, expansion.expansion_digest);
        *out_expansion = expansion;
    }
    return status;
}

static bool nonzero_digest(const uint8_t digest[32]) {
    uint8_t bits = 0;
    for (size_t i = 0; i < 32; ++i)
        bits |= digest[i];
    return bits != 0;
}

static bool request_digest(UnityCompilerBroker *broker, const UnityUvHelperServices *services,
                           const UnityCompilerSnippetCompileRequest *request, uint8_t digest[32]) {
    memset(digest, 0, 32);
    bool ok;
    if (services && services->request_digest) {
        ok = services->request_digest(services->context, request, digest);
    } else {
        uint8_t *transcript = NULL;
        size_t size = 0;
        ok = unity_compiler_broker_serialize_compile_request(broker, request, &transcript, &size,
                                                             digest);
        free(transcript);
    }
    return ok && nonzero_digest(digest);
}

UnityUvHelperStatus unity_uv_helper_inspect_request(
    UnityCompilerBroker *broker, const UnityCompilerSnippetCompileRequest *request,
    const UnityUvHelperServices *services, UnityCompilerBinaryResponse *response,
    UnityUvHelperEvidence *evidence) {
    if (response)
        unity_compiler_binary_response_init(response);
    if (evidence)
        memset(evidence, 0, sizeof(*evidence));
    if (!request || !response || !evidence || !request->snippet_source ||
        request->preprocess_only || request->platform != 4 ||
        (request->shader_type != 0 && request->shader_type != 1) || !request->contract ||
        (request->contract->language != 0 && request->contract->language != 3) ||
        !unity_compiler_snippet_contract_validate(request->contract) ||
        (!broker && (!services || !services->request_digest || !services->compile)))
        return UNITY_UV_HELPER_INVALID_ARGUMENT;
    const size_t size = strlen(request->snippet_source);
    if (size > UNITY_UV_HELPER_EXPANSION_LIMIT)
        return UNITY_UV_HELPER_LIMIT_EXCEEDED;
    StringBuilder probe;
    sb_init(&probe);
    sb_append(&probe, request->snippet_source);
    if (!unity_uv_helper_append_probe(&probe)) {
        sb_free(&probe);
        return UNITY_UV_HELPER_OUT_OF_MEMORY;
    }
    UnityCompilerSnippetCompileRequest expanded = *request;
    expanded.snippet_source = probe.buf;
    expanded.preprocess_only = true;
    UnityUvHelperEvidence checked = {0};
    UnityUvHelperStatus status = UNITY_UV_HELPER_AUTHORITY_MISMATCH;
    if (!request_digest(broker, services, request, checked.compile_request_digest) ||
        !request_digest(broker, services, &expanded, checked.preprocess_request_digest))
        goto done;
    const bool received =
        services && services->compile
            ? services->compile(services->context, &expanded, response)
            : unity_compiler_broker_compile_contract_response(broker, &expanded, response);
    if (!received || response->status.availability != UNITY_COMPILER_RESPONSE_AVAILABLE) {
        status = UNITY_UV_HELPER_COMPILER_UNAVAILABLE;
        goto done;
    }
    if (!unity_compiler_response_status_is_clean_success(&response->status)) {
        status = UNITY_UV_HELPER_COMPILER_REJECTED;
        goto done;
    }
    uint8_t current_compile[32], current_preprocess[32];
    if (!response->has_request_identity || !nonzero_digest(response->controls_digest) ||
        memcmp(response->request_digest, checked.preprocess_request_digest, 32) ||
        !request_digest(broker, services, request, current_compile) ||
        !request_digest(broker, services, &expanded, current_preprocess) ||
        memcmp(current_compile, checked.compile_request_digest, 32) ||
        memcmp(current_preprocess, checked.preprocess_request_digest, 32))
        goto done;
    status = unity_uv_helper_validate_expansion(response->data, response->size, &checked.expansion);
    if (status == UNITY_UV_HELPER_OK)
        *evidence = checked;
done:
    sb_free(&probe);
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
