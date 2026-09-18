// SPDX-License-Identifier: GPL-3.0-only

#include "common/shaderlab_source.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool is_space(uint8_t value) {
    return value == ' ' || value == '\t' || value == '\r' ||
           value == '\n' || value == '\f' || value == '\v';
}

static bool is_identifier_start(uint8_t value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '_';
}

static bool is_identifier_continue(uint8_t value) {
    return is_identifier_start(value) || (value >= '0' && value <= '9');
}

/* Returns false for an unterminated comment. */
static bool skip_trivia(const uint8_t* source, size_t size, size_t* offset) {
    size_t at = *offset;
    for (;;) {
        while (at < size && is_space(source[at])) ++at;
        if (at + 1U >= size || source[at] != '/') break;
        if (source[at + 1U] == '/') {
            at += 2U;
            while (at < size && source[at] != '\n') ++at;
            continue;
        }
        if (source[at + 1U] != '*') break;
        at += 2U;
        bool closed = false;
        while (at + 1U < size) {
            if (source[at] == '*' && source[at + 1U] == '/') {
                at += 2U;
                closed = true;
                break;
            }
            ++at;
        }
        if (!closed) return false;
    }
    *offset = at;
    return true;
}

/* The terminating quote is consumed. C-like escapes are skipped only so a
 * quote inside unrelated source cannot terminate the token prematurely. */
static bool skip_quoted(const uint8_t* source, size_t size, size_t* offset,
                        uint8_t quote) {
    size_t at = *offset;
    if (at >= size || source[at] != quote) return false;
    ++at;
    while (at < size) {
        if (source[at] == quote) {
            *offset = at + 1U;
            return true;
        }
        if (source[at] == '\\') {
            if (at + 1U >= size) return false;
            at += 2U;
        } else {
            ++at;
        }
    }
    return false;
}

ShaderLabSourceNameStatus shaderlab_source_extract_name(
    const uint8_t* source, size_t source_size, char** out_name) {
    if (!out_name || (!source && source_size != 0U)) {
        return SHADERLAB_SOURCE_NAME_MALFORMED;
    }
    *out_name = NULL;
    size_t at = 0U;
    size_t brace_depth = 0U;
    char* found_name = NULL;

    while (at < source_size) {
        if (!skip_trivia(source, source_size, &at)) {
            free(found_name);
            return SHADERLAB_SOURCE_NAME_MALFORMED;
        }
        if (at >= source_size) break;

        if (source[at] == '"' || source[at] == '\'') {
            uint8_t quote = source[at];
            if (!skip_quoted(source, source_size, &at, quote)) {
                free(found_name);
                return SHADERLAB_SOURCE_NAME_MALFORMED;
            }
            continue;
        }
        if (source[at] == '{') {
            if (brace_depth == SIZE_MAX) {
                free(found_name);
                return SHADERLAB_SOURCE_NAME_MALFORMED;
            }
            ++brace_depth;
            ++at;
            continue;
        }
        if (source[at] == '}') {
            if (brace_depth == 0U) {
                free(found_name);
                return SHADERLAB_SOURCE_NAME_MALFORMED;
            }
            --brace_depth;
            ++at;
            continue;
        }
        if (!is_identifier_start(source[at])) {
            ++at;
            continue;
        }

        size_t token_start = at++;
        while (at < source_size && is_identifier_continue(source[at])) ++at;
        size_t token_size = at - token_start;
        if (brace_depth != 0U || token_size != sizeof("Shader") - 1U ||
            memcmp(source + token_start, "Shader", sizeof("Shader") - 1U) !=
                0) {
            continue;
        }

        if (found_name) {
            free(found_name);
            return SHADERLAB_SOURCE_NAME_MULTIPLE;
        }
        if (!skip_trivia(source, source_size, &at) || at >= source_size ||
            source[at] != '"') {
            free(found_name);
            return SHADERLAB_SOURCE_NAME_MALFORMED;
        }
        size_t name_start = ++at;
        while (at < source_size && source[at] != '"') {
            if (source[at] == '\\' || source[at] == '\r' ||
                source[at] == '\n' || source[at] == 0U) {
                free(found_name);
                return SHADERLAB_SOURCE_NAME_MALFORMED;
            }
            ++at;
        }
        if (at >= source_size || at == name_start) {
            free(found_name);
            return SHADERLAB_SOURCE_NAME_MALFORMED;
        }
        size_t name_size = at - name_start;
        if (name_size == SIZE_MAX) {
            free(found_name);
            return SHADERLAB_SOURCE_NAME_MALFORMED;
        }
        found_name = (char*)malloc(name_size + 1U);
        if (!found_name) return SHADERLAB_SOURCE_NAME_ALLOCATION_FAILED;
        memcpy(found_name, source + name_start, name_size);
        found_name[name_size] = '\0';
        ++at;

        if (!skip_trivia(source, source_size, &at) || at >= source_size ||
            source[at] != '{') {
            free(found_name);
            return SHADERLAB_SOURCE_NAME_MALFORMED;
        }
        brace_depth = 1U;
        ++at;
    }

    if (brace_depth != 0U) {
        free(found_name);
        return SHADERLAB_SOURCE_NAME_MALFORMED;
    }
    if (!found_name) return SHADERLAB_SOURCE_NAME_NOT_FOUND;
    *out_name = found_name;
    return SHADERLAB_SOURCE_NAME_OK;
}

void shaderlab_source_name_free(char* name) {
    free(name);
}

const char* shaderlab_source_name_status_string(
    ShaderLabSourceNameStatus status) {
    switch (status) {
        case SHADERLAB_SOURCE_NAME_OK: return "ok";
        case SHADERLAB_SOURCE_NAME_NOT_FOUND: return "not found";
        case SHADERLAB_SOURCE_NAME_MALFORMED: return "malformed source";
        case SHADERLAB_SOURCE_NAME_MULTIPLE:
            return "multiple root Shader declarations";
        case SHADERLAB_SOURCE_NAME_ALLOCATION_FAILED:
            return "allocation failed";
        default: return "unknown status";
    }
}
