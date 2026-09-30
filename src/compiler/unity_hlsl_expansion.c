// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_hlsl_expansion_internal.h"
#include "common/sha256.h"
#include "common/string_builder.h"

#include <stdlib.h>
#include <string.h>

bool unity_hlsl_expansion_same_tokens(const uint8_t *source, const SourceToken *tokens, size_t begin, size_t end,
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
            matched = matched || unity_hlsl_expansion_same_tokens(line.source, tokens, 0, 7, warnings[i]);
        if (!matched)
            return false;
    }
    scanner->offset = end;
    return true;
}

static UnityHlslExpansionStatus tokenize(const uint8_t *source, size_t size, SourceToken *tokens,
                                    size_t *count) {
    /* A compiler expansion must not contain preprocessing splices. Scanning
     * them as physical lines would give comments a different meaning. */
    for (size_t i = 0; i < size; ++i) {
        if (!source[i] ||
            (source[i] == '\\' && i + 1U < size &&
             (source[i + 1U] == '\n' || source[i + 1U] == '\r')) ||
            (source[i] == '?' && i + 2U < size && source[i + 1U] == '?' &&
             strchr("=/'()!<>-", source[i + 2U])))
            return UNITY_HLSL_EXPANSION_MALFORMED;
    }
    SourceScanner scanner = {source, size, 0};
    *count = 0;
    for (;;) {
        SourceToken token = source_scan_next(&scanner);
        if (token.kind == SOURCE_TOKEN_END)
            return UNITY_HLSL_EXPANSION_OK;
        if (token.kind == SOURCE_TOKEN_INVALID)
            return UNITY_HLSL_EXPANSION_MALFORMED;
        if (source_token_equals(source, token, "#")) {
            if (!directive_supported(&scanner, token))
                return UNITY_HLSL_EXPANSION_DIRECTIVE;
            continue;
        }
        if (*count == UNITY_HLSL_EXPANSION_TOKEN_LIMIT)
            return UNITY_HLSL_EXPANSION_LIMIT;
        tokens[(*count)++] = token;
    }
}

void unity_hlsl_expansion_tokens_dispose(UnityHlslExpansionTokens *tokens) {
    if (!tokens) return;
    free(tokens->tokens);
    memset(tokens, 0, sizeof(*tokens));
}

UnityHlslExpansionStatus unity_hlsl_expansion_tokenize(const uint8_t *source, size_t size,
                                                      UnityHlslExpansionTokens *out) {
    if (!out) return UNITY_HLSL_EXPANSION_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!source && size) return UNITY_HLSL_EXPANSION_INVALID_ARGUMENT;
    if (size > UNITY_HLSL_EXPANSION_BYTE_LIMIT) return UNITY_HLSL_EXPANSION_LIMIT;
    if (!size) return UNITY_HLSL_EXPANSION_OK;
    const size_t capacity = size < UNITY_HLSL_EXPANSION_TOKEN_LIMIT ? size : UNITY_HLSL_EXPANSION_TOKEN_LIMIT;
    UnityHlslExpansionTokens tokens = {.tokens = malloc(capacity * sizeof(*tokens.tokens))};
    if (!tokens.tokens) return UNITY_HLSL_EXPANSION_ALLOCATION_FAILED;
    const UnityHlslExpansionStatus status = tokenize(source, size, tokens.tokens, &tokens.count);
    if (status == UNITY_HLSL_EXPANSION_OK) *out = tokens;
    else unity_hlsl_expansion_tokens_dispose(&tokens);
    return status;
}

static bool digest_present(const uint8_t digest[32]) {
    uint8_t bits = 0;
    for (size_t i = 0; i < 32; ++i) bits |= digest[i];
    return bits != 0;
}

static bool request_digest(UnityCompilerBroker *broker, const UnityHlslExpansionServices *services,
                           const UnityCompilerSnippetCompileRequest *request, uint8_t digest[32]) {
    memset(digest, 0, 32);
    bool ok;
    if (services && services->request_digest) {
        ok = services->request_digest(services->context, request, digest);
    } else {
        uint8_t *transcript = NULL;
        size_t size = 0;
        ok = unity_compiler_broker_serialize_compile_request(broker, request, &transcript, &size, digest);
        free(transcript);
    }
    return ok && digest_present(digest);
}

UnityHlslExpansionStatus unity_hlsl_expansion_inspect_request(
    UnityCompilerBroker *broker, const UnityCompilerSnippetCompileRequest *request,
    const char *probe_source, const UnityHlslExpansionServices *services,
    UnityCompilerBinaryResponse *response, UnityHlslExpansionEvidence *evidence) {
    if (response) unity_compiler_binary_response_init(response);
    if (evidence) memset(evidence, 0, sizeof(*evidence));
    if (!request || !response || !evidence || !request->snippet_source || request->preprocess_only ||
        request->platform != 4 || request->shader_type < 0 || request->shader_type > 4 || !request->contract ||
        (request->contract->language != 0 && request->contract->language != 3) ||
        !unity_compiler_snippet_contract_validate(request->contract) ||
        (!broker && (!services || !services->request_digest || !services->compile)) ||
        (probe_source && *probe_source && *probe_source != '\n' && *probe_source != '\r'))
        return UNITY_HLSL_EXPANSION_INVALID_ARGUMENT;
    const size_t size = strlen(request->snippet_source);
    const size_t probe_size = probe_source ? strlen(probe_source) : 0;
    if (size > UNITY_HLSL_EXPANSION_BYTE_LIMIT || probe_size > UNITY_HLSL_EXPANSION_BYTE_LIMIT - size)
        return UNITY_HLSL_EXPANSION_LIMIT;
    StringBuilder source;
    sb_init(&source);
    UnityCompilerSnippetCompileRequest expanded = *request;
    expanded.preprocess_only = true;
    if (probe_size) {
        sb_append(&source, request->snippet_source);
        sb_append(&source, probe_source);
        if (!sb_ok(&source)) { sb_free(&source); return UNITY_HLSL_EXPANSION_ALLOCATION_FAILED; }
        expanded.snippet_source = source.buf;
    }
    UnityHlslExpansionEvidence checked = {0};
    UnityCompilerSnippetCompileRequest controls = expanded;
    controls.snippet_source = "";
    UnityHlslExpansionStatus status = UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH;
    if (!request_digest(broker, services, request, checked.compile_request_digest) ||
        !request_digest(broker, services, &expanded, checked.preprocess_request_digest) ||
        !request_digest(broker, services, &controls, checked.preprocess_controls_digest)) goto done;
    const bool received = services && services->compile
        ? services->compile(services->context, &expanded, response)
        : unity_compiler_broker_compile_contract_response(broker, &expanded, response);
    if (!received || response->status.availability != UNITY_COMPILER_RESPONSE_AVAILABLE) {
        status = UNITY_HLSL_EXPANSION_COMPILER_UNAVAILABLE;
        goto done;
    }
    if (!unity_compiler_response_status_is_clean_success(&response->status)) {
        status = UNITY_HLSL_EXPANSION_COMPILER_REJECTED;
        goto done;
    }
    uint8_t current_compile[32], current_preprocess[32], current_controls[32];
    if (!response->has_request_identity || !digest_present(response->controls_digest) ||
        memcmp(response->request_digest, checked.preprocess_request_digest, 32) ||
        memcmp(response->controls_digest, checked.preprocess_controls_digest, 32) ||
        !request_digest(broker, services, request, current_compile) ||
        !request_digest(broker, services, &expanded, current_preprocess) ||
        !request_digest(broker, services, &controls, current_controls) ||
        memcmp(current_compile, checked.compile_request_digest, 32) ||
        memcmp(current_preprocess, checked.preprocess_request_digest, 32) ||
        memcmp(current_controls, checked.preprocess_controls_digest, 32)) goto done;
    if ((!response->data && response->size) || response->size > UNITY_HLSL_EXPANSION_BYTE_LIMIT) {
        status = response->size > UNITY_HLSL_EXPANSION_BYTE_LIMIT
            ? UNITY_HLSL_EXPANSION_LIMIT : UNITY_HLSL_EXPANSION_MALFORMED;
        goto done;
    }
    common_sha256(response->data, response->size, checked.expansion_digest);
    *evidence = checked;
    status = UNITY_HLSL_EXPANSION_OK;
done:
    sb_free(&source);
    return status;
}
