// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADERLAB_EMITTER_INTERNAL_H
#define SHADERLAB_EMITTER_INTERNAL_H

#include "common/shader_stage.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/shaderlab_emitter.h"

void append_indent(StringBuilder* sb, int level);
bool emit_render_state(StringBuilder* sb, const SerializedShaderState* state,
                       int indent);
bool append_shaderlab_quoted(StringBuilder* sb, const char* value);
bool emit_tags(StringBuilder* sb, const SerializedTagMap* tags, int indent);

typedef enum {
    SHADERLAB_TARGET_2_0 = 0,
    SHADERLAB_TARGET_2_5,
    SHADERLAB_TARGET_3_0,
    SHADERLAB_TARGET_3_5,
    SHADERLAB_TARGET_4_0,
    SHADERLAB_TARGET_4_5,
    SHADERLAB_TARGET_4_6,
    SHADERLAB_TARGET_5_0
} ShaderLabTargetVersion;

typedef struct {
    ShaderLabTargetVersion version;
    /* Only requirements present in every serialized D3D11 variant are
     * candidates for unconditional source pragmas. Variant-only bits may be
     * inferred from that variant's compiled body and must not be promoted to
     * a pass-wide `#pragma require`. */
    uint64_t common_requirements;
    /* Retained as diagnostic authority; never emitted unconditionally. */
    uint64_t union_requirements;
    size_t d3d_variant_count;
} ShaderLabPassTarget;
const char* shaderlab_target_version_name(ShaderLabTargetVersion version);

/* Unity 2021.3's pinned requirements-to-target projection.  This remains an
 * internal test hook; pass resolution additionally verifies every serialized
 * D3D GPU type and strict DXBC stage contract, and rejects a shader model that
 * cannot satisfy the projected requirement level. */
ShaderLabTargetVersion shaderlab_target_from_requirements(
    uint64_t shader_requirements);

/* Emits the exact representable feature delta between a pass-wide target and
 * its serialized D3D11 requirement union. Unknown or source-inexpressible
 * deltas fail closed. */
bool shaderlab_requirements_emit_pragmas(
    StringBuilder* output,
    ShaderLabTargetVersion target,
    uint64_t shader_requirements,
    int indent);

ShaderLabTargetStatus shaderlab_pass_target_resolve(
    const SerializedPass* pass, const BlobEntry* blob_entries,
    int entry_count, uint8_t** segments, const int* segment_lengths,
    int segment_count, ShaderLabPassTarget* out_target,
    ShaderLabTargetDiagnostic* diagnostic);

/* Internal validation hook used by deterministic unit tests.  Production
 * emission builds the same owned plan and therefore has identical predicate
 * validation behavior. */
bool shaderlab_stage_validate_variants(
    const SerializedPass* pass, int stage_index, size_t* out_keyword_count,
    size_t* out_variant_count, ShaderLabStageDiagnostic* diagnostic);

bool emit_stage_hlsl(const SerializedPass* pass, int stage_index,
                     const BlobEntry* blob_entries, int entry_count,
                     uint8_t** segments, const int* segment_lengths,
                     int segment_count, StringBuilder* sb,
                     ShaderLabStageDiagnostic* diagnostic);

bool emit_stage_hlsl_with_variant_plan(
    const ShaderLabVariantPlan* variant_plan,
    int stage_index,
    const BlobEntry* blob_entries,
    int entry_count,
    uint8_t** segments,
    const int* segment_lengths,
    int segment_count,
    StringBuilder* sb,
    ShaderLabStageDiagnostic* diagnostic);

typedef struct {
    ShaderLabExpressionSourceMap *map;
    int subshader_index;
    int pass_index;
    bool unity_uv_helpers;
    bool *unity_uv_used; /* Per-pass accumulation, including baseline emission. */
} ShaderLabExpressionMapContext;

bool shaderlab_expression_source_map_append(
    ShaderLabExpressionSourceMap *map, const ShaderLabExpressionSourceRecord *record);
bool shaderlab_expression_source_map_offset(
    ShaderLabExpressionSourceMap *map, size_t first_record, size_t offset);

bool emit_stage_hlsl_with_variant_plan_mode(
    const ShaderLabVariantPlan* variant_plan,
    int stage_index,
    const BlobEntry* blob_entries,
    int entry_count,
    uint8_t** segments,
    const int* segment_lengths,
    int segment_count,
    bool high_level,
    const ShaderLabExpressionMapContext *trace,
    StringBuilder* sb,
    ShaderLabStageDiagnostic* diagnostic);

/* Deterministic tier-routing test hook used by focused plan tests. */
int shaderlab_stage_planned_subprogram_index(
    const ShaderLabVariantPlan* plan,
    int stage_index,
    size_t original_state,
    int hardware_tier_group);

/* Deterministic work-budget predicate shared by symbolic emission and its
 * boundary/overflow unit tests. */
bool shaderlab_stage_symbolic_score_budget_allows(
    size_t request_count,
    size_t candidate_count);

#endif
