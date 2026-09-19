// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_EMITTER_H
#define HLSL_EMITTER_H

#include "translation/usil.h"
#include "common/string_builder.h"
#include "io/subprogram_metadata.h"

// Optional naming overrides for HLSL emission.
// When NULL or empty, defaults to "main", "appdata", "v2f".
typedef struct {
    const char* entry_point;      // Function name (e.g. "vert", "frag"). Default: "main"
    const char* input_struct;     // Input struct name (e.g. "appdata", "ps_input"). Default: "appdata"
    const char* output_struct;    // Output struct name (e.g. "v2f", "fout"). Default: "v2f"
} HLSLEmitNames;

#define HLSL_HIGH_LEVEL_LIFT_ID "float4-expressions"
#define HLSL_HIGH_LEVEL_LIFT_VERSION 1U
#define HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT 64

typedef enum {
    HLSL_EXPRESSION_ORIGIN_UNMAPPED = 0,
    HLSL_EXPRESSION_ORIGIN_EXPRESSION,
    HLSL_EXPRESSION_ORIGIN_DEAD,
    HLSL_EXPRESSION_ORIGIN_NOP,
    HLSL_EXPRESSION_ORIGIN_RETURN
} HLSLExpressionOriginKind;

typedef struct {
    HLSLExpressionOriginKind kind;
    int instruction_index;
    uint32_t source_instruction_index;
    uint8_t destination_lanes;
    size_t source_begin;
    size_t source_end; /* Exclusive byte offset in the emitter's output builder. */
} HLSLExpressionOrigin;

/* One record per decoded instruction for the bounded float4 lift. Nested
 * expressions have nested spans; elided MOVs may share their producer's span.
 * Dead pure expressions and NOPs have no span. RETURN covers the generated
 * return block. Declarations/ABI tokens are not instruction-owned spans.
 * This is provenance, never an independent correctness certificate. */
typedef struct HLSLExpressionSourceMap {
    HLSLExpressionOrigin origins[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT];
    size_t count;
    bool complete;
} HLSLExpressionSourceMap;

/* Structural trace validation against this exact program/source. This checks
 * coverage, bounds and instruction identity, not semantic correctness. */
bool hlsl_expression_source_map_matches(const HLSLExpressionSourceMap *map,
                                        const USILProgram *program, const char *source);
const char *hlsl_expression_origin_kind_name(HLSLExpressionOriginKind kind);

typedef enum HLSLEmitMode {
    /* Emit from the decoded instruction stream without source-level semantic
     * lifts. This is suitable for recompilation/equivalence verification and
     * is the default used by hlsl_emit(). */
    HLSL_EMIT_MODE_RECOMPILE = 0,

    /* Explicit presentation mode. This may replace proven instruction groups
     * with higher-level Unity/source constructs to improve readability. */
    HLSL_EMIT_MODE_READABLE = 1,

    /* Verification-eligible candidate, never a certificate by itself. v1
     * admits at most 64 straight-line SM4/5 vertex/pixel instructions using
     * full float4 input/output/temp lanes, MOV/ADD/MUL and final RET/NOP.
     * No buffers/resources/effects/precision controls or partial definitions.
     * Single-use SSA expressions are nested once; shared values have typed
     * deterministic names. Unsupported input fails instead of silently using
     * presentation recognizers. Reuses compiler inverse operand/MAD spelling;
     * this is not permission to reorder floating-point operations on its own.
     * Supply the complete reserved macro universe. Compile and compare under
     * the original request before accepting; retain RECOMPILE as fallback. */
    HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE = 2
} HLSLEmitMode;

typedef struct HLSLEmitOptions {
    HLSLEmitMode mode;

    /* Readable-mode-only naming override. It is deliberately not part of
     * HLSLEmitNames so an exact/recompile caller cannot accidentally enable
     * the ComputeScreenPos reconstruction by supplying a helper name. */
    const char* readable_screen_pos_helper;

    /* Explicit compile-environment contract. Unity's shader include stack
     * supplies these declarations; standalone HLSL compilation does not.
     * Keeping the choice per emission avoids byte-affecting process-global
     * state and makes concurrent verification deterministic. */
    bool omit_unity_builtin_declarations;

    /* Borrowed for the duration of one hlsl_emit* call.  ShaderLab callers
     * provide their complete keyword/preprocessor universe so exact local
     * compiler inverses cannot be macro-substituted.  The pointer must be
     * NULL exactly when the count is zero; every element is a non-empty HLSL
     * identifier. */
    const char* const* reserved_preprocessor_identifiers;
    size_t reserved_preprocessor_identifier_count;
    /* Optional caller-owned output, HIGH_LEVEL_CANDIDATE only. Cleared on
     * entry and failure; complete only after successful emission. */
    HLSLExpressionSourceMap *expression_source_map;
} HLSLEmitOptions;

/* Stable, allocation-free failure authority for HLSL emission.  Diagnostics
 * contain only values and indices because stage parameter tables are often
 * temporary and are released immediately after ShaderLab stage translation. */
typedef enum HLSLEmitStatus {
    HLSL_EMIT_STATUS_OK = 0,
    HLSL_EMIT_STATUS_INVALID_ARGUMENT,
    HLSL_EMIT_STATUS_INVALID_PROGRAM,
    HLSL_EMIT_STATUS_UNSUPPORTED,
    HLSL_EMIT_STATUS_INVALID_METADATA,
    HLSL_EMIT_STATUS_ALLOCATION_FAILED,
    HLSL_EMIT_STATUS_ANALYSIS_FAILED,
    HLSL_EMIT_STATUS_INTERNAL_INVARIANT,
    HLSL_EMIT_STATUS_OUTPUT_FAILED
} HLSLEmitStatus;

typedef enum HLSLEmitPhase {
    HLSL_EMIT_PHASE_NONE = 0,
    HLSL_EMIT_PHASE_ARGUMENT_VALIDATION,
    HLSL_EMIT_PHASE_PROGRAM_VALIDATION,
    HLSL_EMIT_PHASE_CONTEXT_ALLOCATION,
    HLSL_EMIT_PHASE_STATE_ALLOCATION,
    HLSL_EMIT_PHASE_CBUFFER_BINDING_MAP,
    HLSL_EMIT_PHASE_CBUFFER_LAYOUT,
    HLSL_EMIT_PHASE_SAMPLER_BINDING_MAP,
    HLSL_EMIT_PHASE_LIVE_RANGE_ANALYSIS,
    HLSL_EMIT_PHASE_CONTROL_FLOW_ANALYSIS,
    HLSL_EMIT_PHASE_DOMINANCE_ANALYSIS,
    HLSL_EMIT_PHASE_BLOCK_NESTING_ANALYSIS,
    HLSL_EMIT_PHASE_SSA_ANALYSIS,
    HLSL_EMIT_PHASE_PROVENANCE_ANALYSIS,
    HLSL_EMIT_PHASE_USE_DEF_ANALYSIS,
    HLSL_EMIT_PHASE_VALUE_ANALYSIS,
    HLSL_EMIT_PHASE_STORAGE_PLANNING,
    HLSL_EMIT_PHASE_SEMANTIC_ANALYSIS,
    HLSL_EMIT_PHASE_COMPILER_MODEL_ANALYSIS,
    HLSL_EMIT_PHASE_TESSELLATION_EMISSION,
    HLSL_EMIT_PHASE_ICB_EMISSION,
    HLSL_EMIT_PHASE_CBUFFER_EMISSION,
    HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION,
    HLSL_EMIT_PHASE_RESOURCE_EMISSION,
    HLSL_EMIT_PHASE_STRUCTURAL_HELPER_EMISSION,
    HLSL_EMIT_PHASE_INTERFACE_EMISSION,
    HLSL_EMIT_PHASE_ENTRY_POINT_EMISSION,
    HLSL_EMIT_PHASE_INSTRUCTION_EMISSION,
    HLSL_EMIT_PHASE_RETURN_EMISSION,
    HLSL_EMIT_PHASE_OUTPUT
} HLSLEmitPhase;

typedef enum HLSLEmitReason {
    HLSL_EMIT_REASON_NONE = 0,
    HLSL_EMIT_REASON_INVALID_ARGUMENT,
    HLSL_EMIT_REASON_OUTPUT_ALREADY_FAILED,
    HLSL_EMIT_REASON_INVALID_MODE,
    HLSL_EMIT_REASON_INVALID_PROGRAM_SHAPE,
    HLSL_EMIT_REASON_INVALID_SIGNATURE,
    HLSL_EMIT_REASON_UNSUPPORTED_STAGE,
    HLSL_EMIT_REASON_UNSUPPORTED_FEATURE,
    HLSL_EMIT_REASON_UNSUPPORTED_OPCODE,
    HLSL_EMIT_REASON_INVALID_INSTRUCTION_SHAPE,
    HLSL_EMIT_REASON_INVALID_OPERAND,
    HLSL_EMIT_REASON_MISSING_BINDING,
    HLSL_EMIT_REASON_RESOURCE_CONTRACT_MISMATCH,
    HLSL_EMIT_REASON_INVALID_METADATA_SHAPE,
    HLSL_EMIT_REASON_MISSING_METADATA_AUTHORITY,
    HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY,
    HLSL_EMIT_REASON_INVALID_PARAMETER_LAYOUT,
    HLSL_EMIT_REASON_BUILTIN_CONTRACT_MISMATCH,
    HLSL_EMIT_REASON_SAMPLER_CONTRACT_MISMATCH,
    HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT,
    HLSL_EMIT_REASON_ANALYSIS_CONFLICT,
    HLSL_EMIT_REASON_SIZE_OVERFLOW,
    HLSL_EMIT_REASON_ALLOCATION_FAILED,
    HLSL_EMIT_REASON_FIXED_BUFFER_OVERFLOW,
    HLSL_EMIT_REASON_LOWERING_FAILED,
    HLSL_EMIT_REASON_INTERNAL_INVARIANT,
    HLSL_EMIT_REASON_OUTPUT_BUILDER_FAILED
} HLSLEmitReason;

typedef enum HLSLEmitMetadataSource {
    HLSL_EMIT_METADATA_SOURCE_NONE = 0,
    HLSL_EMIT_METADATA_SOURCE_PROGRAM,
    HLSL_EMIT_METADATA_SOURCE_STAGE_PARAMETERS,
    HLSL_EMIT_METADATA_SOURCE_COMMON_PARAMETERS
} HLSLEmitMetadataSource;

typedef enum HLSLEmitMetadataKind {
    HLSL_EMIT_METADATA_NONE = 0,
    HLSL_EMIT_METADATA_INPUT_SIGNATURE,
    HLSL_EMIT_METADATA_OUTPUT_SIGNATURE,
    HLSL_EMIT_METADATA_PATCH_SIGNATURE,
    HLSL_EMIT_METADATA_CBUFFER,
    HLSL_EMIT_METADATA_CBUFFER_VARIABLE,
    HLSL_EMIT_METADATA_RESOURCE,
    HLSL_EMIT_METADATA_TEXTURE,
    HLSL_EMIT_METADATA_SAMPLER,
    HLSL_EMIT_METADATA_UAV,
    HLSL_EMIT_METADATA_INDEXABLE_TEMP
} HLSLEmitMetadataKind;

typedef struct HLSLEmitMetadataLocation {
    HLSLEmitMetadataSource source;
    HLSLEmitMetadataKind kind;
    int record_index;
    int member_index;
    int register_index;
} HLSLEmitMetadataLocation;

typedef struct HLSLEmitDiagnostic {
    HLSLEmitStatus status;
    HLSLEmitPhase phase;
    HLSLEmitReason reason;
    int instruction_index;
    uint32_t source_instruction_index;
    /* Stored as int so -1 is an unambiguous no-opcode sentinel. */
    int opcode;
    int operand_index;
    HLSLEmitMetadataLocation metadata;
    HLSLEmitMetadataLocation related_metadata;
} HLSLEmitDiagnostic;

void hlsl_emit_diagnostic_init(HLSLEmitDiagnostic* diagnostic);
const char* hlsl_emit_status_name(HLSLEmitStatus status);
const char* hlsl_emit_phase_name(HLSLEmitPhase phase);
const char* hlsl_emit_reason_name(HLSLEmitReason reason);
const char* hlsl_emit_metadata_source_name(HLSLEmitMetadataSource source);
const char* hlsl_emit_metadata_kind_name(HLSLEmitMetadataKind kind);
const char* hlsl_emit_opcode_name(int opcode);

#define HLSL_EMIT_RECOMPILE_OPTIONS_INIT {HLSL_EMIT_MODE_RECOMPILE, NULL, false, NULL, 0, NULL}
#define HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT                                                          \
    {HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE, NULL, false, NULL, 0, NULL}
#define HLSL_EMIT_READABLE_OPTIONS_INIT {HLSL_EMIT_MODE_READABLE, NULL, false, NULL, 0, NULL}

// Translates a USIL program and appends recompilable HLSL to the string builder.
// Semantic reconstruction is disabled. Returns true on success. Pass NULL for
// names to use defaults.
bool hlsl_emit(const USILProgram* program, StringBuilder* sb,
               const SerializedProgramParameters* params,
               const SerializedProgramParameters* common_params,
               const HLSLEmitNames* names);

// Explicit emission-mode entry point. Passing NULL for options is identical to
// HLSL_EMIT_MODE_RECOMPILE. Readable mode is opt-in because its semantic lifts
// are presentation transforms, not evidence about the unavailable source.
bool hlsl_emit_with_options(const USILProgram* program, StringBuilder* sb,
                            const SerializedProgramParameters* params,
                            const SerializedProgramParameters* common_params,
                            const HLSLEmitNames* names,
                            const HLSLEmitOptions* options);

/* Diagnostic-aware superset.  The output record is initialized on every
 * call and remains valid even when allocation fails.  Existing entry points
 * are source-compatible wrappers that pass NULL here. */
bool hlsl_emit_with_options_diagnostic(
    const USILProgram* program, StringBuilder* sb,
    const SerializedProgramParameters* params,
    const SerializedProgramParameters* common_params,
    const HLSLEmitNames* names, const HLSLEmitOptions* options,
    HLSLEmitDiagnostic* diagnostic);

#endif // HLSL_EMITTER_H
