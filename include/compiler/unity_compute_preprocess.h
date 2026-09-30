// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_COMPUTE_PREPROCESS_H
#define UNITY_COMPUTE_PREPROCESS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Borrowed inputs, immutable until the call returns. The native command uses
 * the complete source filename, not the graphics source-directory contract.
 * Counts are bounded to 1024; filenames/keywords to 1 MiB, source to 512 MiB.
 * All request fields and their order belong to the canonical identity. */
typedef struct UnityCompilerComputePreprocessRequest {
    const char* source;
    const char* source_filename;
    bool caching_preprocessor;
    uint32_t build_platform;
    uint32_t valid_apis;
    char** platform_keywords;
    int platform_keyword_count;
    char** disabled_keywords;
    int disabled_keyword_count;
} UnityCompilerComputePreprocessRequest;
typedef struct {
    const char* name;
    const char* value;
} UnityCompilerComputePreprocessMacro;
typedef struct {
    const char* name;
    const UnityCompilerComputePreprocessMacro* macros;
    size_t macro_count;
} UnityCompilerComputePreprocessedKernel;
typedef struct {
    const char* keyword;
    uint64_t requirements;
} UnityCompilerComputeConditionalRequirement;
/* Raw ordered space-joined lines supplied by WriteKeywordVariants. Empty
 * lines are retained. This view does not expand a Cartesian variant domain,
 * normalize whitespace, or infer a keyword universe. */
typedef struct {
    const char* const* lines;
    size_t line_count;
} UnityCompilerComputeKeywordLines;
/* Every pointer is owned by the opaque response and borrowed read-only by
 * this view. Native preprocessing has NO compiler-success flag. Complete
 * framing or a kernel list cannot replace that absent field or establish a
 * clean import, Class72 producer, source-quality or semantic certificate.
 * The flags, hash words, dependencies, source and API/DXC masks are exact
 * returned values, not caller-invented controls. */
typedef struct {
    UnityCompilerComputeKeywordLines user_global;
    UnityCompilerComputeKeywordLines user_local;
    const UnityCompilerComputePreprocessedKernel* kernels;
    size_t kernel_count;
    uint64_t requirements;
    const UnityCompilerComputeConditionalRequirement* conditional_requirements;
    size_t conditional_requirement_count;
    uint32_t compilation_flags;
    uint32_t include_hash_words[4];
    const char* const* dependencies;
    size_t dependency_count;
    const char* source;
    size_t source_size;
    int32_t supported_apis;
    uint32_t use_dxc_mask;
    uint32_t never_use_dxc_mask;
} UnityCompilerComputePreprocessResult;
typedef struct UnityCompilerComputePreprocessResponse UnityCompilerComputePreprocessResponse;
#endif
