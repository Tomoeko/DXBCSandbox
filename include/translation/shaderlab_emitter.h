// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADERLAB_EMITTER_H
#define SHADERLAB_EMITTER_H

#include "common/shader_stage.h"
#include "common/sha256.h"
#include "dxbc/dxbc_document.h"
#include "common/string_builder.h"
#include "dxbc/dxbc_stage_contract.h"
#include "io/serialized_file.h"
#include "io/shader_blob_archive.h"
#include "io/serialized_shader.h"
#include "translation/hlsl_emitter.h"
#include "translation/shaderlab_variant_plan.h"

typedef enum {
    SHADERLAB_STAGE_OK = 0,
    SHADERLAB_STAGE_INVALID_ARGUMENT,
    SHADERLAB_STAGE_MISSING_PLATFORM_AUTHORITY,
    SHADERLAB_STAGE_NO_D3D_VARIANTS,
    SHADERLAB_STAGE_INVALID_KEYWORD,
    SHADERLAB_STAGE_DUPLICATE_KEYWORD,
    SHADERLAB_STAGE_AMBIGUOUS_PREDICATE,
    SHADERLAB_STAGE_INCOMPLETE_PREDICATE,
    SHADERLAB_STAGE_VARIANT_PLAN_FAILED,
    SHADERLAB_STAGE_ALLOCATION_FAILED,
    SHADERLAB_STAGE_INVALID_BLOB,
    SHADERLAB_STAGE_INVALID_PARAMETER_BLOB,
    SHADERLAB_STAGE_VARIANT_PARSE_FAILED,
    SHADERLAB_STAGE_VARIANT_METADATA_MISMATCH,
    SHADERLAB_STAGE_DXBC_DOCUMENT_FAILED,
    SHADERLAB_STAGE_STAGE_CONTRACT_FAILED,
    SHADERLAB_STAGE_SEMANTIC_DECODE_FAILED,
    SHADERLAB_STAGE_USIL_TRANSLATION_FAILED,
    SHADERLAB_STAGE_HLSL_EMISSION_FAILED,
    SHADERLAB_STAGE_OUTPUT_FAILED
} ShaderLabStageStatus;

typedef struct {
    ShaderLabStageStatus status;
    int stage_index;
    int subprogram_index;
    int conflicting_subprogram_index;
    DXBCStageContractStatus stage_contract_status;
    ShaderStageTupleStatus stage_tuple_status;
    ShaderLabVariantPlanStatus variant_plan_status;
    int raw_keyword_index;
    HLSLEmitDiagnostic hlsl;
} ShaderLabStageDiagnostic;

typedef enum {
    SHADERLAB_TARGET_OK = 0,
    SHADERLAB_TARGET_INVALID_ARGUMENT,
    SHADERLAB_TARGET_MISSING_PLATFORM_AUTHORITY,
    SHADERLAB_TARGET_PLATFORM_CONFLICT,
    SHADERLAB_TARGET_NO_D3D_VARIANTS,
    SHADERLAB_TARGET_INVALID_BLOB,
    SHADERLAB_TARGET_VARIANT_PARSE_FAILED,
    SHADERLAB_TARGET_VARIANT_METADATA_MISMATCH,
    SHADERLAB_TARGET_DXBC_DOCUMENT_FAILED,
    SHADERLAB_TARGET_STAGE_CONTRACT_FAILED,
    SHADERLAB_TARGET_STAGE_TUPLE_CONFLICT,
    SHADERLAB_TARGET_REQUIREMENT_MODEL_CONFLICT,
    SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS
} ShaderLabTargetStatus;

typedef struct {
    ShaderLabTargetStatus status;
    int stage_index;
    int subprogram_index;
    uint64_t shader_requirements;
    uint8_t shader_model_major;
    uint8_t shader_model_minor;
    DXBCDocumentDiagnosticCode document_status;
    DXBCStageContractStatus stage_contract_status;
    ShaderStageTupleStatus stage_tuple_status;
} ShaderLabTargetDiagnostic;

typedef enum {
    SHADERLAB_CANDIDATE_OK = 0,
    SHADERLAB_CANDIDATE_INVALID_ARGUMENT,
    SHADERLAB_CANDIDATE_PROPERTY_FAILED,
    SHADERLAB_CANDIDATE_SUBSHADER_TAG_FAILED,
    SHADERLAB_CANDIDATE_GRABPASS_FAILED,
    SHADERLAB_CANDIDATE_USEPASS_FAILED,
    SHADERLAB_CANDIDATE_UNSUPPORTED_STAGE,
    SHADERLAB_CANDIDATE_PASS_NAME_FAILED,
    SHADERLAB_CANDIDATE_PASS_TAG_FAILED,
    SHADERLAB_CANDIDATE_RENDER_STATE_FAILED,
    SHADERLAB_CANDIDATE_PASS_TARGET_FAILED,
    SHADERLAB_CANDIDATE_STAGE_FAILED,
    SHADERLAB_CANDIDATE_TRAILER_FAILED,
    SHADERLAB_CANDIDATE_OUTPUT_FAILED
} ShaderLabCandidateStatus;

typedef struct {
    ShaderLabCandidateStatus status;
    int property_index;
    int subshader_index;
    int pass_index;
    int unsupported_stage_index;
    ShaderLabTargetDiagnostic target;
    ShaderLabStageDiagnostic stage;
} ShaderLabCandidateDiagnostic;

const char* shaderlab_stage_status_name(ShaderLabStageStatus status);
const char* shaderlab_target_status_name(ShaderLabTargetStatus status);
const char* shaderlab_candidate_status_name(ShaderLabCandidateStatus status);
const char* shaderlab_candidate_reason_name(
    const ShaderLabCandidateDiagnostic* diagnostic);

/* Returns true only when complete, internally consistent serialized platform
 * metadata proves that an ordinary pass has no selectable program for
 * `platform`.  This includes Unity's exact stripped-shell shape: known
 * platform authority, a nonempty known-stage program mask, and no serialized
 * subprogram rows.  False includes target-owned rows and unknown/malformed
 * authority, preserving the emitter's fail-closed projection semantics. */
bool shaderlab_pass_is_proven_not_platform(
    const SerializedPass* pass, int platform);

/* Generates a deterministic, fail-closed ShaderLab recompile candidate.
 * Successful emission means every serialized D3D11 variant was represented;
 * it is not a byte-equality certificate.  Certification requires compiling
 * under an authoritative UnityCompileProfile and comparing the complete
 * target artifact (or replaying an exact OraclePack record). */
bool shaderlab_emit_candidate(
    const SerializedShader* shader,
    const BlobEntry* blob_entries,
    int entry_count,
    uint8_t** segments,
    const int* segment_lengths,
    int segment_count,
    StringBuilder* sb
);

/* Candidate emission with a structured failure result.  Every failure carries
 * a stable candidate status.  Pass-target and stage failures additionally
 * preserve their detailed reason and exact subshader/pass/stage location.
 * Existing callers may continue to use shaderlab_emit_candidate(). */
bool shaderlab_emit_candidate_with_diagnostic(
    const SerializedShader* shader,
    const BlobEntry* blob_entries,
    int entry_count,
    uint8_t** segments,
    const int* segment_lengths,
    int segment_count,
    StringBuilder* sb,
    ShaderLabCandidateDiagnostic* diagnostic
);

/* Opt-in high-level candidate: every selected D3D11 variant must satisfy
 * HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE. Unsupported stages fail atomically;
 * no presentation fallback or placeholder is introduced. Metadata, variant
 * selection and declarations share the normal candidate pipeline. The caller
 * must verify the complete generated domain before accepting this source and
 * retain its verified low-level candidate on any failure. */
bool shaderlab_emit_high_level_candidate(
    const SerializedShader* shader,
    const BlobEntry* blob_entries,
    int entry_count,
    uint8_t** segments,
    const int* segment_lengths,
    int segment_count,
    StringBuilder* sb,
    ShaderLabCandidateDiagnostic* diagnostic);

/* One emitted stage body, identified by serialized coordinates and the
 * selected released container. Generic hardware tier is 3; concrete tiers
 * are 0..2. Source ranges refer to the complete output builder, including
 * caller prefixes, routing code and indentation. */
typedef struct {
    int subshader_index;
    int pass_index;
    int stage_index;
    int subprogram_index;
    int blob_index;
    int hardware_tier_group;
    size_t serialized_state;
    uint8_t target_digest[COMMON_SHA256_DIGEST_SIZE];
    HLSLExpressionSourceMap instructions;
} ShaderLabExpressionSourceRecord;

typedef struct {
    ShaderLabExpressionSourceRecord *records;
    size_t count;
    size_t capacity;
    size_t source_size;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    bool complete;
} ShaderLabExpressionSourceMap;

/* Initialize with {0}; free releases all owned records and resets the map.
 * Maps are provenance only: their hashes and spans establish no equivalence. */
void shaderlab_expression_source_map_free(ShaderLabExpressionSourceMap *map);
/* Checks source binding and basic record shape. It does not re-decode the
 * target containers or validate serialized coordinates or semantics. */
bool shaderlab_expression_source_map_matches_source(
    const ShaderLabExpressionSourceMap *map, const StringBuilder *source);

/* Same atomic candidate contract. An optional initialized map is cleared on
 * entry and failure, and completed only after the whole output succeeds.
 * Retain accepted candidate maps separately when attempting another lift. */
bool shaderlab_emit_high_level_candidate_with_source_map(
    const SerializedShader *shader, const BlobEntry *blob_entries,
    int entry_count, uint8_t **segments, const int *segment_lengths,
    int segment_count, StringBuilder *sb, ShaderLabExpressionSourceMap *map,
    ShaderLabCandidateDiagnostic *diagnostic);

/* Non-exact verifier convenience API.  It embeds supplied HLSL and fills an
 * omitted stage with a trivial placeholder so one stage can be compiled in
 * isolation.  Production candidate reconstruction must use
 * shaderlab_emit_candidate(). */
bool shaderlab_emit_raw(
    const SerializedShader* shader,
    const char* vertex_hlsl,
    const char* fragment_hlsl,
    StringBuilder* sb
);

#endif // SHADERLAB_EMITTER_H
