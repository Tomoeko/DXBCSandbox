// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_YAML_H
#define UNITY_YAML_H

#include "common/string_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An exact finite binary32 decimal can require 151 characters including the
 * decimal point. */
#define UNITY_YAML_FLOAT32_TEXT_CAPACITY 192U

typedef enum {
    UNITY_YAML_OK = 0,
    UNITY_YAML_INVALID_ARGUMENT,
    UNITY_YAML_INVALID_UTF8,
    UNITY_YAML_OUTPUT_FAILED
} UnityYamlStatus;

const char* unity_yaml_status_name(UnityYamlStatus status);

/* Strict RFC 3629 validation: rejects overlong forms, surrogates, truncated
 * sequences, and code points above U+10FFFF.  U+0000 is a valid scalar here
 * and is escaped when emitted by the length-aware quoting API. */
bool unity_yaml_utf8_is_valid(const void* bytes, size_t size);

/* Append a YAML 1.1 double-quoted scalar.  These APIs validate before writing,
 * escape all control characters, quotes, backslashes, NEL, and Unicode line
 * separators, and otherwise preserve valid UTF-8 bytes verbatim. */
UnityYamlStatus unity_yaml_append_quoted_n(
    StringBuilder* output, const void* bytes, size_t size);
UnityYamlStatus unity_yaml_append_quoted(
    StringBuilder* output, const char* text);

/* Format raw IEEE-754 binary32 bits without evaluating the float.  Finite
 * values use an exact, locale-independent decimal, including distinct -0.
 * Unity 2021.3.35f1's Material importer accepts the canonical Infinity,
 * -Infinity and NaN spellings and preserves those three non-finite classes;
 * Unity YAML does not encode NaN payloads. */
bool unity_yaml_format_float32(
    uint32_t bits,
    char output[UNITY_YAML_FLOAT32_TEXT_CAPACITY]);
UnityYamlStatus unity_yaml_append_float32(
    StringBuilder* output, uint32_t bits);

#endif /* UNITY_YAML_H */
