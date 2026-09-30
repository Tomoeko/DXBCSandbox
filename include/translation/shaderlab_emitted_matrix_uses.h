// SPDX-License-Identifier: GPL-3.0-only
#ifndef SHADERLAB_EMITTED_MATRIX_USES_H
#define SHADERLAB_EMITTED_MATRIX_USES_H

#include "translation/shaderlab_source_quality.h"
#include "translation/hlsl_current_matrix_reads.h"

/* Opaque observations produced by the normal complete ShaderLab emitter.
 * No caller AST, source map, digest or quality summary supplies success. */
typedef struct ShaderLabEmittedMatrixUses ShaderLabEmittedMatrixUses;

typedef enum {
    SHADERLAB_MATRIX_USES_OK = 0,
    SHADERLAB_MATRIX_USES_NOT_APPLICABLE,
    SHADERLAB_MATRIX_USES_INVALID_ARGUMENT,
    SHADERLAB_MATRIX_USES_SCOPE_UNAVAILABLE,
    SHADERLAB_MATRIX_USES_EMISSION_FAILED,
    SHADERLAB_MATRIX_USES_CAPTURE_FAILED,
    SHADERLAB_MATRIX_USES_ALLOCATION_FAILED
} ShaderLabMatrixUsesStatus;

typedef struct {
    size_t source_size, entry_count, use_count, field_count, read_count;
    uint8_t source_digest[32], modeled_input_digest[32];
    /* Historical base observation, deliberately unchanged by matrix capture. */
    ShaderLabSourceQualityResult base_quality;
} ShaderLabEmittedMatrixSummary;

typedef struct {
    int subshader_index, pass_index, stage_index, subprogram_index;
    int blob_index, hardware_tier_group;
    size_t serialized_state, entry_record_index;
    size_t source_begin, source_end, field_count, read_count, use_count;
    uint8_t target_digest[32], body_digest[32];
    HLSLSourceQualityResult base_quality;
} ShaderLabEmittedMatrixEntry;

typedef struct {
    size_t entry_index, source_begin, source_end, read_count;
    int final_instruction;
    uint8_t result_components;
    uint16_t instruction_count;
    uint16_t instructions[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT];
    uint8_t expression_digest[32];
} ShaderLabEmittedMatrixUse;

/* Initially NULL output. Inputs are borrowed and immutable during capture.
 * Initial scope is one ordinary complete V/F pass and existing authoritative
 * matrix planner forms. Absence supplies no source/declaration coverage.
 * Failure leaves output NULL; nonempty output rejects unchanged. The receipt
 * owns copied source, targets, player/current/common metadata, ASTs and reads.
 * It neither promotes stage/whole quality nor authenticates original source. */
ShaderLabMatrixUsesStatus shaderlab_emitted_matrix_uses_capture(
    const ShaderLabSourceQualityRequest *current, ShaderLabEmittedMatrixUses **output);

/* Internally reconstructs the same current producer and compares typed facts,
 * owned source and model identities. Hash/span equality alone is insufficient.
 * Public copied descriptions are observations, not replayable receipts. */
bool shaderlab_emitted_matrix_uses_replay(
    const ShaderLabSourceQualityRequest *current, const ShaderLabEmittedMatrixUses *owned);
bool shaderlab_emitted_matrix_uses_describe(
    const ShaderLabEmittedMatrixUses *owned, ShaderLabEmittedMatrixSummary *summary);
bool shaderlab_emitted_matrix_uses_entry(
    const ShaderLabEmittedMatrixUses *owned, size_t index, ShaderLabEmittedMatrixEntry *entry);
bool shaderlab_emitted_matrix_uses_use(
    const ShaderLabEmittedMatrixUses *owned, size_t entry, size_t index, ShaderLabEmittedMatrixUse *use);
bool shaderlab_emitted_matrix_uses_field(
    const ShaderLabEmittedMatrixUses *owned, size_t entry, size_t index, HLSLCurrentMatrixField *field);
bool shaderlab_emitted_matrix_uses_read(
    const ShaderLabEmittedMatrixUses *owned, size_t entry, size_t index, HLSLCurrentMatrixRead *read);
/* Immutable borrowed source until free; no mutable witness storage is exposed. */
bool shaderlab_emitted_matrix_uses_source(
    const ShaderLabEmittedMatrixUses *owned, const char **source, size_t *size);
void shaderlab_emitted_matrix_uses_free(ShaderLabEmittedMatrixUses *owned);

#endif
