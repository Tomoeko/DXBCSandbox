// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADERLAB_VARIANT_PLAN_H
#define SHADERLAB_VARIANT_PLAN_H

#include "common/string_builder.h"
#include "io/serialized_shader.h"
#include "translation/shaderlab_variant_domain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A pass plan is a proof artifact, not a best-effort pragma guess.  It owns a
 * lossless projection of every D3D11 VariantKey in serialized order together
 * with a pragma product whose ordered runtime-selector composition was proven
 * equivalent.  The generated domain may be a proven superset of the sparse
 * serialized domain.
 */
typedef enum {
    SHADERLAB_VARIANT_PLAN_OK = 0,
    SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
    SHADERLAB_VARIANT_PLAN_MISSING_KEYWORD_AUTHORITY,
    SHADERLAB_VARIANT_PLAN_INVALID_KEYWORD,
    SHADERLAB_VARIANT_PLAN_INVALID_KEYWORD_FLAGS,
    SHADERLAB_VARIANT_PLAN_INVALID_PASS_MASK,
    SHADERLAB_VARIANT_PLAN_INVALID_RAW_INDEX,
    SHADERLAB_VARIANT_PLAN_NONCANONICAL_RAW_INDICES,
    SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT,
    SHADERLAB_VARIANT_PLAN_CANDIDATE_OUTSIDE_PASS_MASK,
    SHADERLAB_VARIANT_PLAN_NO_D3D_VARIANTS,
    SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
    SHADERLAB_VARIANT_PLAN_BUILTIN_AMBIGUOUS,
    SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
    SHADERLAB_VARIANT_PLAN_ORDER_MISMATCH,
    SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED,
    SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED,
    SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
    SHADERLAB_VARIANT_PLAN_OUTPUT_FAILED
} ShaderLabVariantPlanStatus;

typedef struct {
    ShaderLabVariantPlanStatus status;
    int stage_index;
    int subprogram_index;
    int conflicting_subprogram_index;
    int raw_keyword_index;
} ShaderLabVariantPlanDiagnostic;

typedef struct {
    int subprogram_index;
    int hardware_tier_group;
    size_t keyword_count;
    uint16_t* keyword_indices;
} ShaderLabPlannedVariant;

typedef struct {
    bool has_default;
    bool is_local;
    size_t keyword_count;
    uint16_t* keyword_indices;
} ShaderLabVariantAxis;

typedef struct {
    bool active;
    bool uses_specific_hardware_tiers;
    size_t variant_count;
    ShaderLabPlannedVariant* variants; /* exact flattened source order */
    size_t state_count;
    ShaderLabVariantState* ordered_states; /* one runtime tier domain */
    /*
     * A nonfactorable domain can be represented by one defaulted Boolean axis
     * per observed keyword.  In symbolic mode generated_state_count is the
     * exact Cartesian cardinality, while generated_states/aliases stay NULL.
     * Stage emission evaluates the finite Boolean domain into a bounded,
     * subtree-pruned numeric-truthiness decision tree whose leaves contain the
     * first-best serialized state over the planner's candidate set. Runtime
     * unsupported-row eligibility is separate authority.
     */
    bool generated_domain_is_symbolic_boolean;
    size_t generated_state_count;
    ShaderLabVariantState* generated_states;
    /* generated_aliases[i] is an ordinal in ordered_states. */
    size_t* generated_aliases;
    size_t axis_count;
    ShaderLabVariantAxis* axes; /* exact pragma order, fastest first */
    /* Number of external axes emitted before the built-in directive. */
    size_t builtin_axis_position;
} ShaderLabPassStageVariantPlan;

typedef struct {
    const SerializedShader* shader;
    const SerializedPass* pass;
    ShaderLabBuiltinVariantFamily builtin_family;
    uint32_t builtin_exclusions;
    bool has_builtin;
    bool uses_specific_hardware_tiers;
    bool external_axes_are_shared;
    ShaderLabPassStageVariantPlan stages[5];
} ShaderLabVariantPlan;

/*
 * Visits the external pragma axes that Unity applies to one linked stage, in
 * the exact order produced by shaderlab_variant_plan_emit_pragmas().  Common
 * axes are represented by the matching axis owned by `stage_index`, even
 * though their unsuffixed directive is emitted from the first active stage's
 * order.  The built-in directive is intentionally excluded because Unity
 * reports it through a separate keyword family.
 */
typedef bool (*ShaderLabEmittedAxisVisitor)(
    const ShaderLabVariantAxis* axis, void* context);

bool shaderlab_variant_plan_visit_emitted_stage_axes(
    const ShaderLabVariantPlan* plan,
    size_t stage_index,
    ShaderLabEmittedAxisVisitor visitor,
    void* context);

void shaderlab_variant_plan_init(ShaderLabVariantPlan* plan);
void shaderlab_variant_plan_free(ShaderLabVariantPlan* plan);

ShaderLabVariantPlanStatus shaderlab_variant_plan_build(
    const SerializedShader* shader,
    const SerializedPass* pass,
    ShaderLabVariantPlan* out_plan,
    ShaderLabVariantPlanDiagnostic* diagnostic);

const char* shaderlab_variant_plan_status_name(
    ShaderLabVariantPlanStatus status);

/* Emits only directives represented by the proven plan. */
bool shaderlab_variant_plan_emit_pragmas(
    const ShaderLabVariantPlan* plan,
    StringBuilder* output,
    int indent);

#endif /* SHADERLAB_VARIANT_PLAN_H */
