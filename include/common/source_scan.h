// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_SOURCE_SCAN_H
#define COMMON_SOURCE_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared byte-span scanning for source inspection, not macro evaluation.
 * Inputs must be non-NULL when size is nonzero, with offset <= size. */
bool source_scan_identifier_start(uint8_t value);
bool source_scan_identifier_continue(uint8_t value);
bool source_scan_skip_trivia(const uint8_t *source, size_t size, size_t *offset);
bool source_scan_skip_quoted(const uint8_t *source, size_t size, size_t *offset, uint8_t quote);

typedef enum {
    SOURCE_TOKEN_END = 0,
    SOURCE_TOKEN_IDENTIFIER,
    SOURCE_TOKEN_NUMBER,
    SOURCE_TOKEN_QUOTED,
    SOURCE_TOKEN_PUNCTUATION,
    SOURCE_TOKEN_INVALID
} SourceTokenKind;

typedef struct {
    SourceTokenKind kind;
    size_t begin;
    size_t end;
} SourceToken;

typedef struct {
    const uint8_t *source;
    size_t size;
    size_t offset;
} SourceScanner;

/* Skips whitespace/comments and returns one identifier, preprocessing number,
 * quoted span or punctuation byte. A punctuation token is intentionally NOT a
 * parsed operator. This scanner does not splice lines or interpret directives;
 * a backslash outside a quote rejects. Callers accepting preprocessed HLSL must
 * validate directives separately. All offsets retain the original source. */
SourceToken source_scan_next(SourceScanner *scanner);
bool source_token_equals(const uint8_t *source, SourceToken token, const char *text);

#endif
