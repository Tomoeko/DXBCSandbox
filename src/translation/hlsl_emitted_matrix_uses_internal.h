// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_EMITTED_MATRIX_USES_INTERNAL_H
#define HLSL_EMITTED_MATRIX_USES_INTERNAL_H

#include "translation/shaderlab_emitted_matrix_uses.h"
#include "translation/hlsl_matrix_lift.h"
#include "translation/hlsl_stage_coverage_internal.h"
#include "translation/hlsl_owned_stage_inputs_internal.h"

struct HLSLEmitterContext;
typedef struct {
    ShaderLabEmittedMatrixUse observation;
    HLSLInstructionOwners owners;
    ASTExpr *tree;
    const ASTExpr *live_tree; /* Only during the owning emitter call. */
    size_t raw_begin, raw_end;
    bool emitted;
} HLSLEmittedMatrixUse;

typedef struct HLSLMatrixUseCapture {
    ShaderLabEmittedMatrixEntry observation;
    HLSLOwnedStageInputs inputs;
    HLSLCurrentMatrixReads reads;
    HLSLEmittedMatrixUse *uses;
    size_t use_count;
    HLSLExpressionSourceMap raw_map;
    HLSLStageCoverage coverage;
    bool finished;
} HLSLMatrixUseCapture;

struct ShaderLabEmittedMatrixUses {
    StringBuilder source;
    ShaderLabSourceQualityInventory inventory;
    HLSLMatrixUseCapture *entries;
    size_t entry_count;
    size_t owned_input_bytes;
    size_t owned_stage_node_count, owned_stage_event_count;
    bool sealed;
};

/* This private capture is forwarded by the owned ShaderLab stage factory only.
 * Public HLSLEmitOptions has no corresponding success/capture field. */
bool hlsl_emit_with_matrix_capture(
    const USILProgram *program, StringBuilder *output,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    HLSLMatrixUseCapture *capture, HLSLEmitDiagnostic *diagnostic);
bool hlsl_matrix_uses_plan(struct HLSLEmitterContext *ctx, const HLSLMatrixLiftPlan *plan);
bool hlsl_matrix_uses_span(HLSLMatrixUseCapture *capture,
    const ASTExpr *expression, size_t begin, size_t end);
bool shaderlab_matrix_uses_begin(ShaderLabEmittedMatrixUses *owned,
    const ShaderLabExpressionSourceRecord *record, const USILProgram *program,
    const uint8_t *target, size_t target_size, const uint8_t *payload, size_t payload_size,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    HLSLMatrixUseCapture **capture);
bool shaderlab_matrix_uses_finish(HLSLMatrixUseCapture *capture,
    const StringBuilder *source, const HLSLExpressionSourceMap *map);
bool hlsl_matrix_uses_trees_equal(const ASTExpr *left, const ASTExpr *right);
bool hlsl_matrix_uses_field_matches(const HLSLMatrixUseCapture *entry, size_t index,
    const HLSLCurrentMatrixField *field);
bool hlsl_matrix_uses_read_matches(const HLSLMatrixUseCapture *entry, size_t index,
    const HLSLCurrentMatrixRead *read);
bool shaderlab_matrix_uses_seal(ShaderLabEmittedMatrixUses *owned);

#endif
