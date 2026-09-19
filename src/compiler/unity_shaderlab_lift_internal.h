// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADERLAB_LIFT_INTERNAL_H
#define UNITY_SHADERLAB_LIFT_INTERNAL_H

#include "compiler/unity_shaderlab_lift.h"

struct UnityShaderLabLiftResult {
    UnityShaderLabLiftArtifact baseline;
    UnityShaderLabLiftArtifact candidate;
    UnityShaderLabLiftArtifact helper_baseline;
    UnityShaderLabLiftArtifact helper_candidate;
    const UnityShaderLabLiftArtifact *accepted;
    HLSLLiftLimits limits;
    HLSLLiftStats stats;
    size_t preprocess_requests;
    bool authority_pinned;
    uint8_t compiler_digest[32];
    uint8_t environment_digest[32];
    uint8_t profile_digest[32];
    uint8_t source_path_digest[32];
    uint8_t source_directory_digest[32];
    uint8_t source_basename_digest[32];
};

/* Shared mode dispatch for the lift and released-object structural evidence. */
bool unity_shaderlab_lift_emit(const SerializedShader *shader, const ShaderBlobArchive *archive,
                               bool high_level, bool unity_uv_helpers, StringBuilder *source,
                               ShaderLabExpressionSourceMap *map,
                               ShaderLabCandidateDiagnostic *diagnostic);

#endif
