// SPDX-License-Identifier: GPL-3.0-only

#include "common/shaderlab_source.h"
#include "common/source_scan.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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
        if (!source_scan_skip_trivia(source, source_size, &at)) {
            free(found_name);
            return SHADERLAB_SOURCE_NAME_MALFORMED;
        }
        if (at >= source_size) break;

        if (source[at] == '"' || source[at] == '\'') {
            uint8_t quote = source[at];
            if (!source_scan_skip_quoted(source, source_size, &at, quote)) {
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
        if (!source_scan_identifier_start(source[at])) {
            ++at;
            continue;
        }

        size_t token_start = at++;
        while (at < source_size && source_scan_identifier_continue(source[at])) ++at;
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
        if (!source_scan_skip_trivia(source, source_size, &at) || at >= source_size ||
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

        if (!source_scan_skip_trivia(source, source_size, &at) || at >= source_size ||
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
