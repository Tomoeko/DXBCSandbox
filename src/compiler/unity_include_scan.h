// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_INCLUDE_SCAN_H
#define UNITY_INCLUDE_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    USC_INCLUDE_SCAN_OK = 0,
    USC_INCLUDE_SCAN_UNSUPPORTED,
    USC_INCLUDE_SCAN_ALLOCATION_FAILED,
    USC_INCLUDE_SCAN_VISITOR_FAILED,
} UscIncludeScanStatus;

/* Visit every literal include in every conditional branch, in source order.
 * This deliberately does not evaluate macros. Nonliteral include operands,
 * Surface Shader generation, malformed directives/comments/strings, embedded
 * NULs, and trigraphs that could alter preprocessing tokens fail
 * closed. Comments and continued lines follow preprocessing lexical order.
 * Paths are borrowed only during the visitor call, with original separators.
 * Earlier visitor calls are not rolled back when a later directive fails. */
UscIncludeScanStatus usc_include_scan(const uint8_t *source, size_t size,
                                      bool (*visit)(void *context, const char *path),
                                      void *context);

#endif
