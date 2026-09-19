// SPDX-License-Identifier: GPL-3.0-only

#include "common/source_scan.h"

#include <string.h>

static bool is_space(uint8_t value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
           value == '\v';
}

bool source_scan_identifier_start(uint8_t value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}

bool source_scan_identifier_continue(uint8_t value) {
    return source_scan_identifier_start(value) || (value >= '0' && value <= '9');
}

/* Returns false for an unterminated comment. */
bool source_scan_skip_trivia(const uint8_t *source, size_t size, size_t *offset) {
    if (!offset || (!source && size) || *offset > size)
        return false;
    size_t at = *offset;
    for (;;) {
        while (at < size && is_space(source[at]))
            ++at;
        if (size - at < 2U || source[at] != '/')
            break;
        if (source[at + 1U] == '/') {
            at += 2U;
            while (at < size && source[at] != '\n') {
                if (!source[at])
                    return false;
                ++at;
            }
            continue;
        }
        if (source[at + 1U] != '*')
            break;
        at += 2U;
        bool closed = false;
        while (at + 1U < size) {
            if (!source[at])
                return false;
            if (source[at] == '*' && source[at + 1U] == '/') {
                at += 2U;
                closed = true;
                break;
            }
            ++at;
        }
        if (!closed)
            return false;
    }
    *offset = at;
    return true;
}

/* The terminating quote is consumed. C-like escapes are skipped only so a
 * quote inside unrelated source cannot terminate the token prematurely. */
bool source_scan_skip_quoted(const uint8_t *source, size_t size, size_t *offset, uint8_t quote) {
    if (!offset || (!source && size) || *offset > size)
        return false;
    size_t at = *offset;
    if (at >= size || source[at] != quote)
        return false;
    ++at;
    while (at < size) {
        if (!source[at])
            return false;
        if (source[at] == quote) {
            *offset = at + 1U;
            return true;
        }
        if (source[at] == '\\') {
            if (at + 1U >= size || !source[at + 1U])
                return false;
            at += 2U;
        } else {
            ++at;
        }
    }
    return false;
}

SourceToken source_scan_next(SourceScanner *scanner) {
    SourceToken token = {.kind = SOURCE_TOKEN_INVALID};
    if (!scanner || (!scanner->source && scanner->size) || scanner->offset > scanner->size)
        return token;
    size_t at = scanner->offset;
    if (!source_scan_skip_trivia(scanner->source, scanner->size, &at))
        return token;
    token.begin = at;
    if (at == scanner->size) {
        token.kind = SOURCE_TOKEN_END;
    } else {
        const uint8_t *source = scanner->source;
        const uint8_t first = source[at];
        if (!first || first == '\\')
            return token;
        if (source_scan_identifier_start(first)) {
            token.kind = SOURCE_TOKEN_IDENTIFIER;
            do {
                ++at;
            } while (at < scanner->size && source_scan_identifier_continue(source[at]));
        } else if ((first >= '0' && first <= '9') ||
                   (first == '.' && at + 1U < scanner->size && source[at + 1U] >= '0' &&
                    source[at + 1U] <= '9')) {
            token.kind = SOURCE_TOKEN_NUMBER;
            ++at;
            while (at < scanner->size) {
                const uint8_t c = source[at], previous = source[at - 1U];
                if (source_scan_identifier_continue(c) || c == '.' ||
                    ((c == '+' || c == '-') &&
                     (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P')))
                    ++at;
                else
                    break;
            }
        } else if (first == '"' || first == '\'') {
            token.kind = SOURCE_TOKEN_QUOTED;
            if (!source_scan_skip_quoted(source, scanner->size, &at, first)) {
                token.kind = SOURCE_TOKEN_INVALID;
                return token;
            }
        } else {
            token.kind = SOURCE_TOKEN_PUNCTUATION;
            ++at;
        }
    }
    token.end = at;
    scanner->offset = at;
    return token;
}

bool source_token_equals(const uint8_t *source, SourceToken token, const char *text) {
    return source && text && token.kind != SOURCE_TOKEN_INVALID && token.kind != SOURCE_TOKEN_END &&
           token.end >= token.begin && token.end - token.begin == strlen(text) &&
           memcmp(source + token.begin, text, token.end - token.begin) == 0;
}
