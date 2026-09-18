// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_SHADERLAB_SOURCE_H
#define COMMON_SHADERLAB_SOURCE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SHADERLAB_SOURCE_NAME_OK = 0,
    SHADERLAB_SOURCE_NAME_NOT_FOUND,
    SHADERLAB_SOURCE_NAME_MALFORMED,
    SHADERLAB_SOURCE_NAME_MULTIPLE,
    SHADERLAB_SOURCE_NAME_ALLOCATION_FAILED,
} ShaderLabSourceNameStatus;

/*
 * Extracts the unique root Shader declaration name from an exact byte span.
 * Comments, quoted strings, and nested brace scopes cannot create a match.
 * Escapes in the declaration name are rejected because this verifier does not
 * guess Unity's source-string escape normalization.  On success, the caller
 * owns *out_name and releases it with shaderlab_source_name_free().
 */
ShaderLabSourceNameStatus shaderlab_source_extract_name(
    const uint8_t* source, size_t source_size, char** out_name);

void shaderlab_source_name_free(char* name);
const char* shaderlab_source_name_status_string(
    ShaderLabSourceNameStatus status);

#endif /* COMMON_SHADERLAB_SOURCE_H */
