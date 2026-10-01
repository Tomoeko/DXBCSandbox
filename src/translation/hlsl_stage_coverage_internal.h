// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_STAGE_COVERAGE_INTERNAL_H
#define HLSL_STAGE_COVERAGE_INTERNAL_H

#include "translation/hlsl_source_quality.h"

enum {
    HLSL_STAGE_COVERAGE_BODY = 1u << 0,
    HLSL_STAGE_COVERAGE_LOCAL_DECLARATION = 1u << 1,
    HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION = 1u << 2,
    HLSL_STAGE_COVERAGE_SYNTAX = 1u << 3,
    HLSL_STAGE_COVERAGE_AST = 1u << 4,
    HLSL_STAGE_COVERAGE_ROOT_LIMIT = 256,
    HLSL_STAGE_COVERAGE_EVENT_LIMIT = 4096,
    HLSL_STAGE_COVERAGE_NODE_LIMIT = 8192,
    /* Both the live analysis tree and its immutable typed snapshot are owned;
     * these limits count logical nodes, bounding physical copies to twice this. */
    HLSL_STAGE_COVERAGE_GLOBAL_NODE_LIMIT = 32768,
    HLSL_STAGE_COVERAGE_GLOBAL_EVENT_LIMIT = 16384
};

typedef enum {
    HLSL_STAGE_COVERAGE_ORDINARY_ENTRY = 0,
    HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT
} HLSLStageCoverageSchema;

typedef enum {
    HLSL_STAGE_ROOT_INSTRUCTION = 0,
    HLSL_STAGE_ROOT_HULL_PHASE_INSTANCE,
    HLSL_STAGE_ROOT_HULL_FACTOR_RETURN,
    HLSL_STAGE_ROOT_HULL_MAXIMUM,
    HLSL_STAGE_ROOT_HULL_IMPLICIT_COPY,
    HLSL_STAGE_ROOT_HULL_POINT_RETURN
} HLSLStageRootOwnerKind;

typedef struct {
    HLSLStageRootOwnerKind kind;
    int phase_index;
    uint32_t source_instruction_index, value_bits, instance_count;
    uint32_t input_signature_index, output_signature_index;
} HLSLStageRootOwner;

typedef struct {
    uint32_t source_unit_id;
    HLSLSourceQualityUnitKind kind;
    size_t begin, end, root_begin, root_end, syntax_begin, syntax_end;
    uint32_t obligations;
} HLSLStageOwnedUnit;

/* The initial FORK producer has at most three phases, six patch semantics and
 * eleven signature declarations. Signature names are independently owned.
 * Contract coordinates without a retained raw declaration index remain typed
 * current-projection owners; independent original-target replay is separate. */
typedef struct {
    USILTessellationContract tessellation;
    USILHullPhase phases[3];
    DXBCSignatureElement input, output, patch_constants[6];
    int patch_constant_count, signature_declaration_count;
    USILSignatureDeclaration signature_declarations[11];
    int cbuffer_count;
    USILConstantBuffer cbuffer;
} HLSLStageHullContract;

typedef struct {
    ASTExpr *tree;
    ASTExpr *recorded_tree;
    const ASTExpr *live_tree;
    int instruction;
    uint32_t source_unit_id;
    HLSLSourceQualityUnitKind unit_kind;
    HLSLStageRootOwner owner, recorded_owner;
    size_t begin, end, whole_begin, whole_end;
    bool emitted;
} HLSLStageOwnedRoot;

typedef struct {
    HLSLSourceQualityFacts facts;
    size_t source_end;
    uint32_t source_unit_id, reasons;
    HLSLSourceQualityUnitKind unit_kind;
    HLSLSourceQualityObservationKind kind;
} HLSLStageOwnedSyntax;

/* The existing source modifier/projection producer uses the consuming
 * operand's demanded lanes, which need not equal a scalar reduction's output
 * lanes. Retain the real operand coordinate and type alongside that demand. */
typedef struct {
    DXBCOperandType type;
    uint8_t source_lanes;
    bool is_source, absolute, negative;
} HLSLStageOwnedOperandUse;

typedef struct HLSLStageCoverage {
    uint32_t obligations, required_binding_mask;
    DXBCProgramType stage;
    uint32_t source_instructions[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    uint32_t recorded_source_instructions[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    /* Decoded operation owners come from the actual program at emission, not
     * from AST spellings or a caller-supplied source inventory. */
    USILOpcode opcodes[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    USILOpcode recorded_opcodes[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    uint8_t destination_lanes[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    uint8_t recorded_destination_lanes[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    uint8_t operand_counts[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    HLSLStageOwnedOperandUse operand_uses[HLSL_STAGE_COVERAGE_ROOT_LIMIT][DXBC_MAX_OPERANDS];
    uint8_t recorded_operand_counts[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    HLSLStageOwnedOperandUse recorded_operand_uses[HLSL_STAGE_COVERAGE_ROOT_LIMIT][DXBC_MAX_OPERANDS];
    size_t instruction_count, source_size, node_count;
    char *source;
    size_t *global_node_count;
    size_t *global_event_count;
    HLSLStageOwnedRoot *roots;
    size_t root_count;
    HLSLStageOwnedSyntax *syntax;
    size_t syntax_count;
    /* Immutable final producer ledger, compared by typed fields on replay. */
    HLSLStageOwnedSyntax *recorded_syntax;
    size_t recorded_syntax_count, recorded_root_count;
    HLSLStageCoverageSchema schema;
    HLSLStageOwnedUnit units[3], recorded_units[3];
    size_t unit_count, recorded_unit_count;
    HLSLStageHullContract hull_contract, recorded_hull_contract;
    uint8_t hull_owner_digest[32];
    bool began, finished;
} HLSLStageCoverage;

struct HLSLEmitterContext;
bool hlsl_stage_coverage_begin(struct HLSLEmitterContext *ctx);
bool hlsl_stage_coverage_begin_unit(struct HLSLEmitterContext *ctx,
    uint32_t id, HLSLSourceQualityUnitKind kind);
bool hlsl_stage_coverage_root(struct HLSLEmitterContext *ctx, const ASTExpr *root, int instruction);
bool hlsl_stage_coverage_owned_root(struct HLSLEmitterContext *ctx,
    const ASTExpr *root, int instruction, const HLSLStageRootOwner *owner);
bool hlsl_hull_owned_contract_digest(const USILProgram *program, uint8_t digest[32]);
bool hlsl_stage_coverage_observation(struct HLSLEmitterContext *ctx,
    const HLSLSourceQualityObservation *observation);
bool hlsl_stage_coverage_span(HLSLStageCoverage *coverage,
    const ASTExpr *root, size_t begin, size_t end);
void hlsl_stage_coverage_finish(struct HLSLEmitterContext *ctx);
bool hlsl_stage_coverage_validate(const HLSLStageCoverage *coverage, const StringBuilder *source);
bool hlsl_stage_coverage_equal(const HLSLStageCoverage *a, const HLSLStageCoverage *b);
void hlsl_stage_coverage_dispose(HLSLStageCoverage *coverage);
/* Private stage-local capture only. Existing bounded parsed FORK HULL scope,
 * with no JOIN or ICB admission. Source quality and retained gaps are unchanged.
 * The initially empty destination owns trees/contracts after success. A failed
 * new capture disposes partial ownership; argument rejection preserves previous
 * results and nonempty output. This is not an original-target receipt. */
bool hlsl_emit_with_stage_coverage(const USILProgram *program, StringBuilder *output,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    HLSLStageCoverage *coverage, HLSLEmitDiagnostic *diagnostic);
/* Shared bounded mechanical copy, with no inferred provenance. */
ASTExpr *hlsl_owned_expression_copy(const ASTExpr *source, size_t *node_count);

#endif
