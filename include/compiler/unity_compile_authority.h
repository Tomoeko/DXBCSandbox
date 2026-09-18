// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPILE_AUTHORITY_H
#define UNITY_COMPILE_AUTHORITY_H

#include "compiler/unity_compiler_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNITY_PLATFORM_CAPABILITY_COUNT 33U

/*
 * A complete snapshot produced by Unity's platform-capability initialization.
 * The selected graphics API and hardware tier are not sufficient to recreate
 * these bits: project, player, color-space, lightmap, precision-model and
 * platform settings also participate.  Exact mode therefore requires the
 * snapshot instead of inventing a desktop profile.
 */
typedef struct {
    bool present;
    uint64_t bits;
} UnityPlatformCapabilitySnapshot;

typedef enum {
    UNITY_COMPILE_AUTHORITY_OK = 0,
    UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT,
    UNITY_COMPILE_AUTHORITY_MISSING_PREPROCESS_CONTRACT,
    UNITY_COMPILE_AUTHORITY_MISSING_VARIANT_FAMILIES,
    UNITY_COMPILE_AUTHORITY_MISSING_PLATFORM_CAPABILITIES,
    UNITY_COMPILE_AUTHORITY_INVALID_KEYWORD_INDEX,
    UNITY_COMPILE_AUTHORITY_AMBIGUOUS_KEYWORD_NAME,
    UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY,
} UnityCompileAuthorityStatus;

typedef struct {
    const SnippetCompileContract* contract;
    const char* const* keyword_names;
    size_t keyword_name_count;
    const int* global_keyword_indices;
    size_t global_keyword_index_count;
    const int* local_keyword_indices;
    size_t local_keyword_index_count;
    int32_t compiler_program;
    int pass_type;
    UnityPlatformCapabilitySnapshot platform_capabilities;
} UnityCompileAuthorityInput;

/* Owned pKW/uKW/dKW arrays and scalar values for one compileSnippet call. */
typedef struct {
    char** platform_keywords;
    int platform_keyword_count;
    char** user_keywords;
    int user_keyword_count;
    char** disabled_keywords;
    int disabled_keyword_count;
    uint32_t compiler_flags;
    uint64_t requirements;
} UnityCompileAuthority;

void unity_compile_authority_init(UnityCompileAuthority* authority);
void unity_compile_authority_free(UnityCompileAuthority* authority);

UnityCompileAuthorityStatus unity_compile_authority_build(
    const UnityCompileAuthorityInput* input,
    UnityCompileAuthority* out_authority);

const char* unity_compile_authority_status_string(
    UnityCompileAuthorityStatus status);

/* Exact Unity 2021.3 tables recovered from the Editor compile path. */
const char* unity_platform_capability_keyword(size_t capability_index);
bool unity_platform_capability_is_settings_dependent(
    size_t capability_index);
const char* unity_pass_type_keyword(int pass_type);

/* Serialized stage index: vertex, fragment, geometry, hull, domain, raytrace. */
bool unity_serialized_stage_to_compiler_program(
    int serialized_stage, int32_t* out_compiler_program);

#endif
