// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_MATRIX_LIFT_H
#define HLSL_MATRIX_LIFT_H

#include "translation/hlsl_ast.h"
#include "translation/usil.h"

struct HLSLEmitterContext;

/* One contiguous compiler graph: a full four-component matrix transform
 * with implicit input w=1, followed by a second full matrix transform. The
 * graph matcher proves physical operations; metadata supplies matrix types
 * and orientation. Neither shader identity nor authored source is an input. */
typedef struct {
    int start_instruction;
    int position_input_register;
    int world_temporary;
    int clip_temporary;
    int world_matrix_buffer;
    int world_matrix_first_row;
    int clip_matrix_buffer;
    int clip_matrix_first_row;
} HLSLMatrixVectorChain;

typedef struct {
    int start_instruction;
    int end_instruction;
    uint64_t instruction_owners;
    uint8_t result_components;
    uint8_t claimed_instruction_count;
    /* Owns the entire tree. world_expression is borrowed from this tree.
     * The caller may transfer expression into its existing AST planner. */
    ASTExpr *expression;
    ASTExpr *world_expression;
} HLSLMatrixLiftPlan;

bool hlsl_compiler_matrix_vector_chain_matches(const USILProgram *program,
                                               int start_instruction,
                                               HLSLMatrixVectorChain *out_chain);
/* Borrowed projected identifier. Only serialized float4x4 declarations with
 * a complete, unambiguous four-row extent supply this authority. */
const char *hlsl_matrix_lift_identifier(const struct HLSLEmitterContext *ctx,
                                       int buffer, int first_row, bool *row_major);
/* Requires the existing SSA/use-def, projected cbuffer layout and high-level
 * interface plan. Admits the nested eight-operation graph, one full four-lane
 * transform, or one three-lane transform projected from complete float4x4
 * metadata. The latter requires a natural float3 input and constructs zero w;
 * no missing float3x3 declaration shape is inferred. Failure leaves out_plan
 * zeroed and owns no AST nodes. */
bool hlsl_matrix_lift_prepare(struct HLSLEmitterContext *ctx, int start_instruction,
                              HLSLMatrixLiftPlan *out_plan);
void hlsl_matrix_lift_plan_free(HLSLMatrixLiftPlan *plan);

#endif
