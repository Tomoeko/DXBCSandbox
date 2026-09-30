// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_SOURCE_QUALITY_H
#define HLSL_SOURCE_QUALITY_H

#include "common/shader_stage.h"
#include "translation/hlsl_ast.h"
#include "translation/hlsl_emitter.h"

/* Source quality is independent of compilation, byte equality and certificates.
 * The analyzer never inspects identifier spellings or parses emitted text. */
typedef enum {
    HLSL_SOURCE_QUALITY_CLEAN = 0,
    HLSL_SOURCE_QUALITY_MIXED,
    HLSL_SOURCE_QUALITY_LOW_LEVEL,
    HLSL_SOURCE_QUALITY_UNSUPPORTED,
    HLSL_SOURCE_QUALITY_FAILED
} HLSLSourceQualityClass;

typedef enum {
    HLSL_SOURCE_UNIT_ENTRY_POINT = 0,
    HLSL_SOURCE_UNIT_HELPER,
    HLSL_SOURCE_UNIT_GENERATED_INCLUDE,
    HLSL_SOURCE_UNIT_EXTERNAL_INCLUDE,
    /* Kernel/keyword pragmas and conditional source selection syntax. */
    HLSL_SOURCE_UNIT_CONFIGURATION,
    /* Complete required declaration fragments, never a whole include. */
    HLSL_SOURCE_UNIT_REQUIRED_EXTERNAL_DECLARATION
} HLSLSourceQualityUnitKind;

typedef enum {
    HLSL_SOURCE_VALUE_UNKNOWN = 0,
    HLSL_SOURCE_VALUE_LOGICAL,
    HLSL_SOURCE_VALUE_REGISTER
} HLSLSourceQualityValueKind;

enum {
    HLSL_SOURCE_ARTIFACT_REGISTER_STORAGE = 1u << 0,
    HLSL_SOURCE_ARTIFACT_LANE_TRANSPORT = 1u << 1,
    HLSL_SOURCE_ARTIFACT_SCALARIZED_INTRINSIC = 1u << 2,
    HLSL_SOURCE_ARTIFACT_RAW_BUFFER_RECONSTRUCTION = 1u << 3,
    HLSL_SOURCE_ARTIFACT_SYNTHETIC_INTERFACE = 1u << 4,
    HLSL_SOURCE_ARTIFACT_INSTRUCTION_ASSIGNMENT = 1u << 5,
    HLSL_SOURCE_ARTIFACT_UNSTRUCTURED_CONTROL = 1u << 6,
    HLSL_SOURCE_ARTIFACT_STORAGE_BITCAST = 1u << 7,
    HLSL_SOURCE_ARTIFACT_ALL = (1u << 8) - 1u
};

/* Facts come from logical-value/SSA, interface, storage and lift authority.
 * Register-index metadata alone is insufficient: a named SSA value can have a
 * TEMP origin without representing an emitted register bank.
 *
 * known means all syntax represented by this node/event is accounted for. A
 * LOGICAL value has a stable logical_value_id; components is 1..4 for a scalar
 * or vector and 0 for a matrix, structure, array or resource. Literal nodes
 * already carry their component/type/raw-bit facts and need no resolver.
 *
 * semantic_projection requires a logical base value and exactly matching
 * result width. It must describe a logical purpose (coordinate projection,
 * weight component, etc.), not merely a swizzle on a nicely named register.
 * real_bitcast describes a program bit reinterpretation; it cannot coexist
 * with STORAGE_BITCAST. logical_operation identifies recovered expression,
 * assignment or control structure rather than an instruction transcription.
 *
 * A fact may contain several artifact flags. Counters count emitted occurrences
 * per category; residual_total is their sum, not a count of unique instructions.
 * With incomplete source coverage these counts are observed lower bounds,
 * never a claim that uninspected declarations/helpers contain no artifacts.
 * For non-AST declarations/lowered regions, report one fact per occurrence.
 * Opaque operands require facts covering their entire formatter expansion;
 * hidden lane transport/bitcasts must appear in those facts as artifacts.
 *
 * instruction_index is -1 for declarations or syntax without an instruction
 * owner. In that case source_instruction_index must be UINT32_MAX and lanes
 * zero. Otherwise source_instruction_index is the retained decoder index and
 * lanes is the owned destination mask (zero is allowed for control/effects).
 * These are provenance records, never proof that a lift is correct. */
typedef enum {
    HLSL_SOURCE_RESOURCE_NONE = 0,
    HLSL_SOURCE_RESOURCE_TEXTURE,
    HLSL_SOURCE_RESOURCE_SAMPLER,
    HLSL_SOURCE_RESOURCE_UAV
} HLSLSourceQualityResourceKind;

typedef enum {
    HLSL_SOURCE_CBUFFER_NONE = 0,
    HLSL_SOURCE_CBUFFER_BEGIN,
    HLSL_SOURCE_CBUFFER_FIELD,
    HLSL_SOURCE_CBUFFER_END
} HLSLSourceQualityCBufferDeclarationKind;

typedef struct {
    bool known;
    HLSLSourceQualityValueKind value_kind;
    uint64_t logical_value_id;
    unsigned components;
    uint32_t artifacts;
    bool semantic_projection;
    bool real_bitcast;
    bool logical_operation;
    int instruction_index;
    uint32_t source_instruction_index;
    uint8_t lanes;
    /* Declaration-only sibling authority, never an executable read origin.
     * Ordinary facts leave the witness record flag and count zero. */
    uint32_t declaration_witness_count;
    uint32_t declaration_variant_index;
    /* One synchronous record per witness. Count is nonzero only on the first
     * record of a declaration; IDs never borrow source or union storage. */
    bool declaration_witness_record;
    uint32_t declaration_field_index;
    uint32_t declaration_witness_subprogram_index;
    /* Actual typed resource declaration identity, independent of names. */
    HLSLSourceQualityResourceKind resource_declaration_kind;
    uint32_t resource_binding_register;
    /* Actual named-cbuffer syntax. Field IDs index the retained declaration
     * inventory; BEGIN/END use UINT32_MAX. The byte range comes from current
     * or common serialized metadata, never from an identifier spelling.
     * Authority is 1 for current-stage parameters, 2 for common parameters. */
    HLSLSourceQualityCBufferDeclarationKind cbuffer_declaration_kind;
    uint32_t cbuffer_binding_register;
    uint32_t cbuffer_field_index;
    uint32_t cbuffer_byte_offset;
    uint32_t cbuffer_byte_size;
    uint8_t cbuffer_declaration_authority;
} HLSLSourceQualityFacts;

void hlsl_source_quality_facts_init(HLSLSourceQualityFacts *facts);

typedef bool (*HLSLSourceQualityExpressionFacts)(
    void *context, uint32_t source_unit_id, const ASTExpr *expression,
    HLSLSourceQualityFacts *facts);
typedef bool (*HLSLSourceQualityStatementFacts)(
    void *context, uint32_t source_unit_id, const ASTStmt *statement,
    HLSLSourceQualityFacts *facts);

enum {
    HLSL_SOURCE_QUALITY_REASON_RESIDUAL = 1u << 0,
    HLSL_SOURCE_QUALITY_REASON_UNKNOWN_PROVENANCE = 1u << 1,
    HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE = 1u << 2,
    HLSL_SOURCE_QUALITY_REASON_EMISSION_UNSUPPORTED = 1u << 3,
    HLSL_SOURCE_QUALITY_REASON_EMISSION_FAILED = 1u << 4,
    HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT = 1u << 5,
    HLSL_SOURCE_QUALITY_REASON_ANALYSIS_BOUND = 1u << 6,
    HLSL_SOURCE_QUALITY_REASON_ALLOCATION_FAILED = 1u << 7
};

typedef enum {
    HLSL_SOURCE_OBSERVATION_EXPRESSION = 0,
    HLSL_SOURCE_OBSERVATION_STATEMENT,
    HLSL_SOURCE_OBSERVATION_EMISSION,
    HLSL_SOURCE_OBSERVATION_COVERAGE
} HLSLSourceQualityObservationKind;

typedef struct HLSLSourceQualityObservation {
    DXBCProgramType stage;
    uint32_t pass_index;
    uint32_t entry_point_index;
    uint32_t source_unit_id;
    HLSLSourceQualityUnitKind unit_kind;
    HLSLSourceQualityObservationKind kind;
    /* AST kind, or -1 for emission/coverage observations. */
    int ast_kind;
    uint32_t reasons;
    HLSLSourceQualityFacts facts;
} HLSLSourceQualityObservation;

/* Called for every inspected AST node/event and every incomplete unit. Borrowed
 * observation is valid only during the call. Keep this ledger if per-instruction
 * reasons are required. Return false to reject the analysis. */
typedef bool (*HLSLSourceQualityObserver)(
    void *context, const HLSLSourceQualityObservation *observation);

typedef struct {
    uint32_t source_unit_id;
    HLSLSourceQualityUnitKind kind;
    /* True only if all emitted syntax in this unit is covered by AST roots or
     * emission_facts, including declarations, helpers and formatter internals.
     * Generated/external includes need their own audited units; a call-name
     * recognizer cannot attest a hidden helper's implementation. */
    bool coverage_complete;
    const ASTExpr *const *expressions;
    size_t expression_count;
    const ASTStmt *const *statements;
    size_t statement_count;
    const HLSLSourceQualityFacts *emission_facts;
    size_t emission_fact_count;
} HLSLSourceQualityUnit;

typedef struct {
    DXBCProgramType stage;
    uint32_t pass_index;
    uint32_t entry_point_index;
    HLSLEmitStatus emission_status;
    /* Units are ordered by strictly increasing source_unit_id. */
    const HLSLSourceQualityUnit *units;
    size_t unit_count;
    /* Independent emitted-unit inventory. Mismatch is incomplete coverage. */
    size_t expected_unit_count;
    HLSLSourceQualityExpressionFacts expression_facts;
    HLSLSourceQualityStatementFacts statement_facts;
    void *facts_context;
    HLSLSourceQualityObserver observer;
    void *observer_context;
    /* Shared node/event budget. Zero selects 1,048,576; depth is bounded at 256.
     * Exceeding either yields an explicit failed analysis, never clean output. */
    size_t node_budget;
    /* Independent entry-definition inventory. Zero retains the one-entry
     * default. Multiple entries still require distinct source unit IDs and
     * complete coverage for every definition, including conditional ones. */
    size_t expected_entry_point_count;
} HLSLSourceQualityRequest;

typedef struct {
    size_t ast_expressions;
    size_t ast_statements;
    size_t emission_events;
    size_t inspected_units;
    size_t incomplete_units;
    size_t unknown_provenance;
    size_t logical_operations;
    size_t logical_value_references;
    size_t semantic_projections;
    size_t real_bitcasts;
    size_t register_storage;
    size_t lane_transport;
    size_t scalarized_intrinsics;
    size_t raw_buffer_reconstruction;
    size_t synthetic_interface;
    size_t instruction_assignments;
    size_t unstructured_control;
    size_t storage_bitcasts;
    size_t sibling_declarations;
    size_t sibling_declaration_witnesses;
    size_t resource_declarations;
    size_t residual_total;
    size_t cbuffer_declarations;
    size_t cbuffer_fields;
} HLSLSourceQualityCounters;

typedef struct HLSLSourceQualityResult {
    DXBCProgramType stage;
    uint32_t pass_index;
    uint32_t entry_point_index;
    HLSLEmitStatus emission_status;
    HLSLSourceQualityClass classification;
    uint32_t reasons;
    HLSLSourceQualityCounters counts;
    bool has_first_issue;
    HLSLSourceQualityObservation first_issue;
} HLSLSourceQualityResult;

/* Streaming counterpart for emitters that release each AST after formatting.
 * The analysis owns a copy of callback configuration and borrows result and
 * callback contexts until destroy. Create initializes result; each unit must be
 * begun once, with increasing IDs, before recording any syntax. Finish consumes
 * no ownership and must precede destroy. Failure remains sticky. */
typedef struct HLSLSourceQualityAnalysis HLSLSourceQualityAnalysis;
HLSLSourceQualityAnalysis *hlsl_source_quality_analysis_create(
    const HLSLSourceQualityRequest *request, HLSLSourceQualityResult *result);
bool hlsl_source_quality_analysis_begin_unit(HLSLSourceQualityAnalysis *analysis,
    uint32_t source_unit_id, HLSLSourceQualityUnitKind kind, bool coverage_complete);
bool hlsl_source_quality_analysis_expression(HLSLSourceQualityAnalysis *analysis,
                                             const ASTExpr *expression);
bool hlsl_source_quality_analysis_statement(HLSLSourceQualityAnalysis *analysis,
                                            const ASTStmt *statement);
bool hlsl_source_quality_analysis_emission(HLSLSourceQualityAnalysis *analysis,
                                           const HLSLSourceQualityFacts *facts);
/* Downgrade the active unit when its independent emitted-syntax inventory
 * discovers a missing span. Repeated calls record one coverage issue. This
 * cannot upgrade an incomplete unit or invent a dependency unit. */
bool hlsl_source_quality_analysis_mark_incomplete_unit(HLSLSourceQualityAnalysis *analysis);
bool hlsl_source_quality_analysis_finish(HLSLSourceQualityAnalysis *analysis,
    HLSLEmitStatus emission_status, size_t expected_unit_count);
void hlsl_source_quality_analysis_destroy(HLSLSourceQualityAnalysis *analysis);

/* Allocation-free. Borrows all input trees/facts; no mutation or ownership
 * transfer. False means malformed input/facts, observer rejection or a budget
 * failure; the initialized result then remains FAILED with its reasons/counts.
 * A true result can be UNSUPPORTED or FAILED because emission did not succeed.
 * A successful but incompletely understood source cannot classify CLEAN. */
bool hlsl_source_quality_analyze(const HLSLSourceQualityRequest *request,
                                  HLSLSourceQualityResult *result);
const char *hlsl_source_quality_class_name(HLSLSourceQualityClass classification);

/* Append deterministic JSON containing only fixed labels and numeric evidence.
 * This renders observed source units, not an unobserved enclosing artifact or
 * a compiler/semantic certificate. Invalid pointers leave output unchanged;
 * allocation failure follows StringBuilder's normal sticky failure contract. */
bool hlsl_source_quality_append_json(const HLSLSourceQualityResult *result,
                                     StringBuilder *output);

#endif
