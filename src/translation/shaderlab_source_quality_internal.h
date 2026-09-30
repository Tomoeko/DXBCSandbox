// SPDX-License-Identifier: GPL-3.0-only
#ifndef SHADERLAB_SOURCE_QUALITY_INTERNAL_H
#define SHADERLAB_SOURCE_QUALITY_INTERNAL_H

#include "translation/shaderlab_source_quality.h"

/* Temporary stage builders cannot borrow final receipt storage. Their ranges
 * are copied, rebased and bound to the final source when the stage commits. */
typedef struct {
    size_t begin;
    size_t end;
    size_t record_index;
} ShaderLabSourceQualityBody;

typedef struct ShaderLabSourceQualityCapture {
    ShaderLabSourceQualityInventory *inventory;
    ShaderLabSourceQualityBody bodies[32];
    size_t body_count;
    size_t cursor;
    bool failed;
    struct ShaderLabEmittedMatrixUses *matrix_uses;
} ShaderLabSourceQualityCapture;

bool shaderlab_source_quality_capture_receipt(
    ShaderLabSourceQualityCapture *capture, const StringBuilder *source,
    ShaderLabSourceSyntaxKind kind, int property, int subshader, int pass,
    int stage, size_t record_index);
bool shaderlab_source_quality_capture_body(
    ShaderLabSourceQualityCapture *capture, size_t begin, size_t end,
    size_t record_index);
bool shaderlab_source_quality_capture_stage(
    ShaderLabSourceQualityCapture *capture, const StringBuilder *source,
    size_t stage_begin, size_t first_body, int subshader, int pass, int stage);

bool shaderlab_emit_high_level_candidate_inventory(
    const SerializedShader *shader, const ShaderBlobArchive *archive,
    StringBuilder *source, ShaderLabExpressionSourceMap *map,
    ShaderLabSourceQualityCapture *capture,
    ShaderLabCandidateDiagnostic *diagnostic);

ShaderLabSourceQualityStatus shaderlab_source_quality_emit_with_matrix_capture(
    const ShaderLabSourceQualityRequest *request, StringBuilder *source,
    ShaderLabSourceQualityInventory *destination,
    struct ShaderLabEmittedMatrixUses *matrix_uses, ShaderLabSourceQualityDiagnostic *diagnostic);

#endif
