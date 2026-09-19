// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_SHADER_DEPENDENCY_CLOSURE_H
#define UNITY_SHADER_DEPENDENCY_CLOSURE_H

#include "io/shader_object.h"

typedef enum {
    UNITY_SHADER_DEPENDENCIES_OK = 0,
    UNITY_SHADER_DEPENDENCIES_INVALID_ARGUMENT,
    UNITY_SHADER_DEPENDENCIES_INVALID_METADATA,
    UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE,
    UNITY_SHADER_DEPENDENCIES_UNSUPPORTED_PASS,
    UNITY_SHADER_DEPENDENCIES_EXTERNAL_BINDING,
    UNITY_SHADER_DEPENDENCIES_ARCHIVE_UNAVAILABLE,
    UNITY_SHADER_DEPENDENCIES_LIMIT_EXCEEDED
} UnityShaderDependencyStatus;

typedef struct {
    UnityShaderDependencyStatus status;
    int subshader_index;
    int pass_index;
    int stage_index;
    int subprogram_index;
    size_t pass_count;
    size_t subprogram_count;
    size_t parameter_record_count;
    size_t engine_fog_input_count;
    /* Only valid on OK. Ordered dependency-bearing metadata and every local
     * D3D11 V/F subprogram's independently reconstructed binding inventory. */
    uint8_t digest[32];
} UnityShaderDependencyReport;

/* Bounded inspection, not captured production authority. The object must be
 * decoded and its model must still correspond to its validated root. Admits
 * only ordinary V/F passes without parameter/resource bindings: no properties,
 * shader/PPtr refs, fallback, UsePass, GrabPass, arbitrary dynamic state,
 * instancing or pipeline tags. Even null entries in reference arrays are
 * outside this initial scope. It does not recover original includes or prove
 * runtime selection. Vertex inputs and explicit built-in unity_FogColor,
 * unity_FogStart, unity_FogEnd and unity_FogDensity state inputs must be supplied
 * identically by the conditional runtime contract. These are inventoried by
 * name, not treated as an empty graph or a resolved asset.
 * Uses the existing archive, parameter parser and reflection union; it never
 * treats raw IDs, missing callbacks or an absent archive as resolved inputs. */
UnityShaderDependencyStatus unity_shader_dependency_closure(const ShaderObject *object,
                                                            UnityShaderDependencyReport *report);

const char *unity_shader_dependency_status_name(UnityShaderDependencyStatus status);

#endif
