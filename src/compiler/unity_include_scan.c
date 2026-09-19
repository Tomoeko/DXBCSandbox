// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_include_scan.h"

#include <stdlib.h>
#include <string.h>

static bool horizontal_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

static bool identifier_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool include_name(const char *name, size_t length) {
    return (length == 7U && !memcmp(name, "include", 7U)) ||
           (length == 20U && !memcmp(name, "include_with_pragmas", 20U));
}

/* Recognize the prefix already emitted on this logical line. Angle header
 * names are tokens too: comment delimiters inside them are pathname bytes. */
static bool awaiting_header(const char *line, size_t size) {
    size_t at = 0;
    while (at < size && horizontal_space(line[at]))
        ++at;
    if (at == size || line[at++] != '#')
        return false;
    while (at < size && horizontal_space(line[at]))
        ++at;
    const size_t name = at;
    while (at < size && identifier_char(line[at]))
        ++at;
    if (!include_name(line + name, at - name))
        return false;
    while (at < size && horizontal_space(line[at]))
        ++at;
    return at == size;
}

/* Splice before recognizing comments: a continued // comment consumes the
 * next physical line. Replace a block comment with one space, including its
 * internal newlines, so it cannot split an include directive. */
static UscIncludeScanStatus logical_source(const uint8_t *source, size_t size, char **out) {
    *out = NULL;
    if ((!source && size) || size == SIZE_MAX)
        return USC_INCLUDE_SCAN_UNSUPPORTED;
    char *text = malloc(size + 1U);
    if (!text)
        return USC_INCLUDE_SCAN_ALLOCATION_FAILED;
    size_t count = 0;
    const size_t bom = size >= 3U && !memcmp(source, "\xef\xbb\xbf", 3U) ? 3U : 0U;
    for (size_t i = bom; i < size; ++i) {
        if (!source[i] ||
            (i + 2U < size && source[i] == '?' && source[i + 1U] == '?' && source[i + 2U] == '/') ||
            (i + 2U < size && !memcmp(source + i, "\xef\xbb\xbf", 3U))) {
            free(text);
            return USC_INCLUDE_SCAN_UNSUPPORTED;
        }
        if (source[i] == '\\' && i + 1U < size) {
            if (source[i + 1U] == '\n') {
                ++i;
                continue;
            }
            if (source[i + 1U] == '\r') {
                ++i;
                if (i + 1U < size && source[i + 1U] == '\n')
                    ++i;
                continue;
            }
        }
        text[count++] = source[i] == '\r' ? '\n' : (char)source[i];
        if (source[i] == '\r' && i + 1U < size && source[i + 1U] == '\n')
            ++i;
    }
    text[count] = '\0';
    size_t written = 0, line_start = 0;
    for (size_t i = 0; i < count;) {
        if (text[i] == '<' && awaiting_header(text + line_start, written - line_start)) {
            text[written++] = text[i++];
            while (i < count && text[i] != '\n' && text[i] != '>')
                text[written++] = text[i++];
            if (i == count || text[i] != '>')
                goto unsupported;
            text[written++] = text[i++];
        } else if (text[i] == '"' || text[i] == '\'') {
            const char quote = text[i];
            text[written++] = text[i++];
            bool closed = false;
            while (i < count && text[i] != '\n') {
                const char c = text[i++];
                text[written++] = c;
                if (c == quote) {
                    closed = true;
                    break;
                }
                if (c == '\\' && i < count)
                    text[written++] = text[i++];
            }
            if (!closed)
                goto unsupported;
        } else if (text[i] == '/' && i + 1U < count && text[i + 1U] == '/') {
            text[written++] = ' ';
            i += 2U;
            while (i < count && text[i] != '\n')
                ++i;
        } else if (text[i] == '/' && i + 1U < count && text[i + 1U] == '*') {
            text[written++] = ' ';
            i += 2U;
            while (i + 1U < count && (text[i] != '*' || text[i + 1U] != '/'))
                ++i;
            if (i + 1U >= count)
                goto unsupported;
            i += 2U;
        } else {
            text[written++] = text[i++];
            if (text[written - 1U] == '\n')
                line_start = written;
        }
    }
    text[written] = '\0';
    /* Other trigraphs cannot change comment boundaries or splice lines. They
     * are harmless inside removed comments, but unsupported in actual tokens. */
    for (size_t i = 0; i + 2U < written; ++i)
        if (text[i] == '?' && text[i + 1U] == '?' && strchr("=/'()!<>-", text[i + 2U]))
            goto unsupported;
    *out = text;
    return USC_INCLUDE_SCAN_OK;

unsupported:
    free(text);
    return USC_INCLUDE_SCAN_UNSUPPORTED;
}

static UscIncludeScanStatus scan_directive(char *line, bool (*visit)(void *, const char *),
                                           void *context) {
    while (horizontal_space(*line))
        ++line;
    /* Alternative directive tokens are outside this lexical profile. */
    if (line[0] == '%' && line[1] == ':')
        return USC_INCLUDE_SCAN_UNSUPPORTED;
    if (*line++ != '#')
        return USC_INCLUDE_SCAN_OK;
    while (horizontal_space(*line))
        ++line;
    char *name = line;
    while (identifier_char(*line))
        ++line;
    const size_t length = (size_t)(line - name);
    const bool include = include_name(name, length);
    if (!include) {
        if (length == 6U && !memcmp(name, "pragma", 6U)) {
            while (horizontal_space(*line))
                ++line;
            const char *pragma = line;
            while (identifier_char(*line))
                ++line;
            if ((line - pragma == 7 && !memcmp(pragma, "surface", 7U)) ||
                (line - pragma >= 7 && !memcmp(pragma, "include", 7U)))
                return USC_INCLUDE_SCAN_UNSUPPORTED;
        }
        if ((length >= 7U && !memcmp(name, "include", 7U)) ||
            (length == 6U && !memcmp(name, "import", 6U)))
            return USC_INCLUDE_SCAN_UNSUPPORTED;
        return USC_INCLUDE_SCAN_OK;
    }
    while (horizontal_space(*line))
        ++line;
    const char close = *line == '"' ? '"' : (*line == '<' ? '>' : '\0');
    if (!close)
        return USC_INCLUDE_SCAN_UNSUPPORTED;
    char *path = ++line;
    while (*line && *line != close)
        ++line;
    if (!*line || line == path || line[-1] == '\\')
        return USC_INCLUDE_SCAN_UNSUPPORTED;
    *line++ = '\0';
    while (horizontal_space(*line))
        ++line;
    if (*line)
        return USC_INCLUDE_SCAN_UNSUPPORTED;
    return visit(context, path) ? USC_INCLUDE_SCAN_OK : USC_INCLUDE_SCAN_VISITOR_FAILED;
}

UscIncludeScanStatus usc_include_scan(const uint8_t *source, size_t size,
                                      bool (*visit)(void *context, const char *path),
                                      void *context) {
    if (!visit)
        return USC_INCLUDE_SCAN_UNSUPPORTED;
    char *text = NULL;
    UscIncludeScanStatus status = logical_source(source, size, &text);
    if (status != USC_INCLUDE_SCAN_OK)
        return status;
    char *line = text;
    for (;;) {
        char *next = strchr(line, '\n');
        if (next)
            *next = '\0';
        status = scan_directive(line, visit, context);
        if (status != USC_INCLUDE_SCAN_OK || !next)
            break;
        line = next + 1U;
    }
    free(text);
    return status;
}
