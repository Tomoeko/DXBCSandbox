// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADERLAB_STRUCTURAL_CERTIFICATE_H
#define SHADERLAB_STRUCTURAL_CERTIFICATE_H

#include "io/shader_object.h"

/*
 * This certificate covers only the source-level D3D11 ShaderLab projection
 * retained by SerializedShader: properties, D3D-relevant subshader/pass
 * topology, tags, pass kind/name, render state, dependencies, custom editors,
 * LOD, and fallback. It deliberately does not certify compiled program bytes,
 * keyword/variant selection, resource binding, render-pipeline selection, or
 * pixels. Those require independent compiler and runtime gates.
 *
 * A successful result is meaningful only together with successful
 * shaderlab_emit_candidate_with_diagnostic() for the same immutable
 * ShaderObject. The emitter proves representability; this pass proves that no
 * source-semantic serialized field was silently ignored or ambiguously
 * defaulted.
 */

typedef enum {
    SHADERLAB_STRUCTURE_OK = 0,
    SHADERLAB_STRUCTURE_INVALID_ARGUMENT,
    SHADERLAB_STRUCTURE_SCHEMA_AUTHORITY_FAILED,
    SHADERLAB_STRUCTURE_MODEL_INVALID,
    SHADERLAB_STRUCTURE_PROJECTION_MISMATCH,
    SHADERLAB_STRUCTURE_DROPPED_SEMANTIC,
    SHADERLAB_STRUCTURE_AMBIGUOUS_SEMANTIC,
    SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC,
} ShaderLabStructuralStatus;

typedef enum {
    SHADERLAB_STRUCTURE_FIELD_NONE = 0,
    SHADERLAB_STRUCTURE_FIELD_SHADER_MODEL,
    SHADERLAB_STRUCTURE_FIELD_PROPERTY,
    SHADERLAB_STRUCTURE_FIELD_DEPENDENCY,
    SHADERLAB_STRUCTURE_FIELD_CUSTOM_EDITOR,
    SHADERLAB_STRUCTURE_FIELD_DISABLE_NO_SUBSHADERS_MESSAGE,
    SHADERLAB_STRUCTURE_FIELD_FALLBACK,
    SHADERLAB_STRUCTURE_FIELD_SUBSHADER_TAGS,
    SHADERLAB_STRUCTURE_FIELD_SUBSHADER_LOD,
    SHADERLAB_STRUCTURE_FIELD_PASS_TYPE,
    SHADERLAB_STRUCTURE_FIELD_PASS_NAME,
    SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD,
    SHADERLAB_STRUCTURE_FIELD_PASS_TAGS,
    SHADERLAB_STRUCTURE_FIELD_STATE_TAGS,
    SHADERLAB_STRUCTURE_FIELD_STATE_LOD,
    SHADERLAB_STRUCTURE_FIELD_RENDER_STATE,
    SHADERLAB_STRUCTURE_FIELD_FOG,
} ShaderLabStructuralField;

typedef struct {
    ShaderLabStructuralStatus status;
    ShaderLabStructuralField field;
    int property_index;
    int subshader_index;
    int pass_index;
    int element_index;
    size_t covered_semantic_fields;
    size_t excluded_compiled_fields;
    bool runtime_selection_certified;
    bool visual_output_certified;
} ShaderLabStructuralDiagnostic;

ShaderLabStructuralStatus shaderlab_structural_certify(
    const ShaderObject* object, ShaderLabStructuralDiagnostic* diagnostic);

const char* shaderlab_structural_status_name(
    ShaderLabStructuralStatus status);
const char* shaderlab_structural_field_name(ShaderLabStructuralField field);

#endif /* SHADERLAB_STRUCTURAL_CERTIFICATE_H */
