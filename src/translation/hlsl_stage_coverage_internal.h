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

typedef struct {
    ASTExpr *tree;
    ASTExpr *recorded_tree;
    const ASTExpr *live_tree;
    int instruction;
    size_t begin, end, whole_begin, whole_end;
    bool emitted;
} HLSLStageOwnedRoot;

typedef struct {
    HLSLSourceQualityFacts facts;
    size_t source_end;
} HLSLStageOwnedSyntax;

/* The existing source modifier/projection producer uses the consuming
 * operand's demanded lanes, which need not equal a scalar reduction's output
 * lanes. Retain the real operand coordinate and type alongside that demand. */
typedef struct {
    DXBCOperandType type;
    uint8_t source_lanes;
    bool is_source, absolute, negative;
} HLSLStageOwnedOperandUse;

typedef struct {
    uint32_t obligations, required_binding_mask;
    DXBCProgramType stage;
    uint32_t source_instructions[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    uint8_t destination_lanes[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
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
    bool began, finished;
} HLSLStageCoverage;

struct HLSLEmitterContext;
bool hlsl_stage_coverage_begin(struct HLSLEmitterContext *ctx);
bool hlsl_stage_coverage_root(struct HLSLEmitterContext *ctx, const ASTExpr *root, int instruction);
bool hlsl_stage_coverage_observation(struct HLSLEmitterContext *ctx,
    const HLSLSourceQualityObservation *observation);
bool hlsl_stage_coverage_span(HLSLStageCoverage *coverage,
    const ASTExpr *root, size_t begin, size_t end);
void hlsl_stage_coverage_finish(struct HLSLEmitterContext *ctx);
bool hlsl_stage_coverage_validate(const HLSLStageCoverage *coverage, const StringBuilder *source);
bool hlsl_stage_coverage_equal(const HLSLStageCoverage *a, const HLSLStageCoverage *b);
void hlsl_stage_coverage_dispose(HLSLStageCoverage *coverage);
/* Shared bounded mechanical copy, with no inferred provenance. */
ASTExpr *hlsl_owned_expression_copy(const ASTExpr *source, size_t *node_count);

#endif
