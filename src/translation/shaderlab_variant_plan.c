// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_variant_plan.h"

#include "common/common.h"

#include <limits.h>
#include <string.h>

#define ARRAY_COUNT(value) (sizeof(value) / sizeof((value)[0]))
#define KNOWN_EXCLUSIONS ((uint32_t)0x3fu)

typedef struct {
    ShaderLabVariantState* states;
    size_t count;
} OwnedStateList;

static void set_diagnostic(ShaderLabVariantPlanDiagnostic* diagnostic,
                           ShaderLabVariantPlanStatus status,
                           int stage_index,
                           int subprogram_index,
                           int conflicting_subprogram_index,
                           int raw_keyword_index) {
    if (!diagnostic) return;
    diagnostic->status = status;
    diagnostic->stage_index = stage_index;
    diagnostic->subprogram_index = subprogram_index;
    diagnostic->conflicting_subprogram_index = conflicting_subprogram_index;
    diagnostic->raw_keyword_index = raw_keyword_index;
}

const char* shaderlab_variant_plan_status_name(
    ShaderLabVariantPlanStatus status) {
    switch (status) {
    case SHADERLAB_VARIANT_PLAN_OK: return "ok";
    case SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT: return "invalid-argument";
    case SHADERLAB_VARIANT_PLAN_MISSING_KEYWORD_AUTHORITY:
        return "missing-keyword-authority";
    case SHADERLAB_VARIANT_PLAN_INVALID_KEYWORD: return "invalid-keyword";
    case SHADERLAB_VARIANT_PLAN_INVALID_KEYWORD_FLAGS:
        return "invalid-keyword-flags";
    case SHADERLAB_VARIANT_PLAN_INVALID_PASS_MASK:
        return "invalid-pass-mask";
    case SHADERLAB_VARIANT_PLAN_INVALID_RAW_INDEX:
        return "invalid-raw-index";
    case SHADERLAB_VARIANT_PLAN_NONCANONICAL_RAW_INDICES:
        return "noncanonical-raw-indices";
    case SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT: return "scope-conflict";
    case SHADERLAB_VARIANT_PLAN_CANDIDATE_OUTSIDE_PASS_MASK:
        return "candidate-outside-pass-mask";
    case SHADERLAB_VARIANT_PLAN_NO_D3D_VARIANTS:
        return "no-d3d-variants";
    case SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH:
        return "tier-domain-mismatch";
    case SHADERLAB_VARIANT_PLAN_BUILTIN_AMBIGUOUS:
        return "builtin-ambiguous";
    case SHADERLAB_VARIANT_PLAN_NONFACTORABLE: return "nonfactorable";
    case SHADERLAB_VARIANT_PLAN_ORDER_MISMATCH: return "order-mismatch";
    case SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED:
        return "proof-limit-exceeded";
    case SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED:
        return "alias-proof-failed";
    case SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED:
        return "allocation-failed";
    case SHADERLAB_VARIANT_PLAN_OUTPUT_FAILED: return "output-failed";
    }
    return "unknown";
}

void shaderlab_variant_plan_init(ShaderLabVariantPlan* plan) {
    if (plan) memset(plan, 0, sizeof(*plan));
}

static void free_state(ShaderLabVariantState* state) {
    if (!state) return;
    if (state->keyword_indices) {
        mem_free((void*)state->keyword_indices,
                 state->keyword_count * sizeof(*state->keyword_indices));
    }
    memset(state, 0, sizeof(*state));
}

static void free_state_list(OwnedStateList* list) {
    if (!list) return;
    if (list->states) {
        for (size_t i = 0; i < list->count; ++i) free_state(&list->states[i]);
        mem_free(list->states, list->count * sizeof(*list->states));
    }
    memset(list, 0, sizeof(*list));
}

void shaderlab_variant_plan_free(ShaderLabVariantPlan* plan) {
    if (!plan) return;
    for (size_t stage_index = 0; stage_index < ARRAY_COUNT(plan->stages);
         ++stage_index) {
        ShaderLabPassStageVariantPlan* stage = &plan->stages[stage_index];
        if (stage->variants) {
            for (size_t i = 0; i < stage->variant_count; ++i) {
                ShaderLabPlannedVariant* variant = &stage->variants[i];
                if (variant->keyword_indices) {
                    mem_free(variant->keyword_indices,
                             variant->keyword_count *
                                 sizeof(*variant->keyword_indices));
                }
            }
            mem_free(stage->variants,
                     stage->variant_count * sizeof(*stage->variants));
        }
        if (stage->ordered_states) {
            for (size_t i = 0; i < stage->state_count; ++i) {
                free_state(&stage->ordered_states[i]);
            }
            mem_free(stage->ordered_states,
                     stage->state_count * sizeof(*stage->ordered_states));
        }
        if (stage->generated_states) {
            for (size_t i = 0; i < stage->generated_state_count; ++i) {
                free_state(&stage->generated_states[i]);
            }
            mem_free(stage->generated_states,
                     stage->generated_state_count *
                         sizeof(*stage->generated_states));
        }
        if (stage->generated_aliases) {
            mem_free(stage->generated_aliases,
                     stage->generated_state_count *
                         sizeof(*stage->generated_aliases));
        }
        if (stage->axes) {
            for (size_t i = 0; i < stage->axis_count; ++i) {
                ShaderLabVariantAxis* axis = &stage->axes[i];
                if (axis->keyword_indices) {
                    mem_free(axis->keyword_indices,
                             axis->keyword_count *
                                 sizeof(*axis->keyword_indices));
                }
            }
            mem_free(stage->axes,
                     stage->axis_count * sizeof(*stage->axes));
        }
    }
    memset(plan, 0, sizeof(*plan));
}

static bool checked_allocate(void** output, size_t count, size_t item_size) {
    if (!output || (count != 0 && item_size > SIZE_MAX / count)) return false;
    *output = NULL;
    if (count == 0) return true;
    *output = mem_alloc(count * item_size);
    if (!*output) return false;
    memset(*output, 0, count * item_size);
    return true;
}

static bool keyword_is_identifier(const char* keyword) {
    if (!keyword || keyword[0] == '\0' || strcmp(keyword, "defined") == 0)
        return false;
    const unsigned char first = (unsigned char)keyword[0];
    if (!((first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z') || first == '_')) return false;
    for (size_t i = 1; keyword[i] != '\0'; ++i) {
        const unsigned char value = (unsigned char)keyword[i];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_')) return false;
    }
    return true;
}

static bool state_equal(const ShaderLabVariantState* left,
                        const ShaderLabVariantState* right) {
    return left->keyword_count == right->keyword_count &&
           (left->keyword_count == 0 ||
            memcmp(left->keyword_indices, right->keyword_indices,
                   left->keyword_count * sizeof(*left->keyword_indices)) == 0);
}

static bool state_contains(const ShaderLabVariantState* state,
                           uint16_t keyword) {
    size_t low = 0;
    size_t high = state->keyword_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2u;
        const uint16_t current = state->keyword_indices[middle];
        if (current == keyword) return true;
        if (current < keyword) low = middle + 1u;
        else high = middle;
    }
    return false;
}

static bool state_is_subset(const ShaderLabVariantState* state,
                            const ShaderLabVariantState* superset) {
    size_t i = 0;
    size_t j = 0;
    while (i < state->keyword_count && j < superset->keyword_count) {
        if (state->keyword_indices[i] < superset->keyword_indices[j])
            return false;
        if (state->keyword_indices[i] > superset->keyword_indices[j]) ++j;
        else {
            ++i;
            ++j;
        }
    }
    return i == state->keyword_count;
}

static bool copy_state(ShaderLabVariantState* destination,
                       const uint16_t* indices,
                       size_t count) {
    memset(destination, 0, sizeof(*destination));
    if (!checked_allocate((void**)&destination->keyword_indices, count,
                          sizeof(*destination->keyword_indices))) return false;
    if (count != 0) {
        memcpy((void*)destination->keyword_indices, indices,
               count * sizeof(*indices));
    }
    destination->keyword_count = count;
    return true;
}

static bool raw_scope_is_canonical(const int* indices, int count) {
    if (count < 0 || (count != 0 && !indices)) return false;
    for (int i = 0; i < count; ++i) {
        if (indices[i] < 0 || indices[i] > UINT16_MAX) return false;
        if (i != 0 && indices[i - 1] >= indices[i]) return false;
    }
    return true;
}

static bool validate_shader_authority(
    const SerializedShader* shader,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    if (!shader || shader->keyword_names.count < 0 ||
        (shader->keyword_names.count != 0 &&
         !shader->keyword_names.keywords)) {
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
                       -1, -1, -1, -1);
        return false;
    }
    if (shader->keyword_names.count != 0 && !shader->keyword_flags) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_MISSING_KEYWORD_AUTHORITY,
                       -1, -1, -1, -1);
        return false;
    }
    if (shader->keyword_names.count > UINT16_MAX + 1) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_INVALID_KEYWORD_FLAGS,
                       -1, -1, -1, -1);
        return false;
    }
    for (int i = 0; i < shader->keyword_names.count; ++i) {
        const char* keyword = shader->keyword_names.keywords[i];
        if (!keyword_is_identifier(keyword)) {
            set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_KEYWORD,
                           -1, -1, -1, i);
            return false;
        }
    }
    /* m_KeywordNames is indexed authority, not a set.  Unity can serialize
     * the same spelling at distinct raw indices owned by different linked
     * stages.  Keep those identities separate while planning; the emitted
     * axis-coherence proof later permits only an exact same-scope axis to be
     * hoisted across every participating stage.  Duplicate spellings within
     * one emitted axis/domain still fail that proof closed. */
    return true;
}

static bool validate_pass_mask(const SerializedShader* shader,
                               const SerializedPass* pass,
                               ShaderLabVariantState* out_mask,
                               ShaderLabVariantPlanDiagnostic* diagnostic) {
    if (!pass || pass->serialized_keyword_state_mask_count < 0 ||
        (pass->serialized_keyword_state_mask_count != 0 &&
         !pass->serialized_keyword_state_mask)) {
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_PASS_MASK,
                       -1, -1, -1, -1);
        return false;
    }
    out_mask->keyword_indices = pass->serialized_keyword_state_mask;
    out_mask->keyword_count =
        (size_t)pass->serialized_keyword_state_mask_count;
    if (!shaderlab_variant_state_is_canonical(out_mask)) {
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_PASS_MASK,
                       -1, -1, -1, -1);
        return false;
    }
    for (size_t i = 0; i < out_mask->keyword_count; ++i) {
        if ((size_t)out_mask->keyword_indices[i] >=
            (size_t)shader->keyword_names.count) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_INVALID_PASS_MASK,
                           -1, -1, -1, out_mask->keyword_indices[i]);
            return false;
        }
    }
    return true;
}

static bool identity_to_state(
    const SerializedShader* shader,
    const SerializedSubProgramIdentity* identity,
    const ShaderLabVariantState* pass_mask,
    int stage_index,
    int subprogram_index,
    ShaderLabVariantState* out_state,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    memset(out_state, 0, sizeof(*out_state));
    if (!identity) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_MISSING_KEYWORD_AUTHORITY,
                       stage_index, subprogram_index, -1, -1);
        return false;
    }
    /* Unity's legacy m_KeywordIndices is a combined runtime set.  The parser
     * retains it in local_keyword_indices only as a storage compatibility
     * detail; m_KeywordFlags bit 0 remains the exact pragma-scope authority. */
    if (!identity->keyword_scopes_are_explicit) {
        if (identity->global_keyword_index_count != 0 ||
            identity->global_keyword_indices ||
            !raw_scope_is_canonical(identity->local_keyword_indices,
                                    identity->local_keyword_index_count)) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_NONCANONICAL_RAW_INDICES,
                           stage_index, subprogram_index, -1, -1);
            return false;
        }
        const size_t count = (size_t)identity->local_keyword_index_count;
        uint16_t* combined = NULL;
        if (!checked_allocate((void**)&combined, count, sizeof(*combined))) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, subprogram_index, -1, -1);
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            const int raw = identity->local_keyword_indices[i];
            if (raw >= shader->keyword_names.count) {
                mem_free(combined, count * sizeof(*combined));
                set_diagnostic(diagnostic,
                               SHADERLAB_VARIANT_PLAN_INVALID_RAW_INDEX,
                               stage_index, subprogram_index, -1, raw);
                return false;
            }
            combined[i] = (uint16_t)raw;
        }
        const ShaderLabVariantState state = {combined, count};
        if (!state_is_subset(&state, pass_mask)) {
            int offending = -1;
            for (size_t i = 0; i < count; ++i) {
                if (!state_contains(pass_mask, combined[i])) {
                    offending = combined[i];
                    break;
                }
            }
            mem_free(combined, count * sizeof(*combined));
            set_diagnostic(
                diagnostic,
                SHADERLAB_VARIANT_PLAN_CANDIDATE_OUTSIDE_PASS_MASK,
                stage_index, subprogram_index, -1, offending);
            return false;
        }
        out_state->keyword_indices = combined;
        out_state->keyword_count = count;
        return true;
    }
    if (!raw_scope_is_canonical(identity->global_keyword_indices,
                                identity->global_keyword_index_count) ||
        !raw_scope_is_canonical(identity->local_keyword_indices,
                                identity->local_keyword_index_count)) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_NONCANONICAL_RAW_INDICES,
                       stage_index, subprogram_index, -1, -1);
        return false;
    }
    const size_t global_count =
        (size_t)identity->global_keyword_index_count;
    const size_t local_count = (size_t)identity->local_keyword_index_count;
    if (global_count > SIZE_MAX - local_count) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, subprogram_index, -1, -1);
        return false;
    }
    for (size_t i = 0; i < global_count; ++i) {
        const int raw = identity->global_keyword_indices[i];
        if (raw >= shader->keyword_names.count) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_INVALID_RAW_INDEX,
                           stage_index, subprogram_index, -1, raw);
            return false;
        }
        if ((shader->keyword_flags[raw] & 1u) != 0u) {
            set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT,
                           stage_index, subprogram_index, -1, raw);
            return false;
        }
    }
    for (size_t i = 0; i < local_count; ++i) {
        const int raw = identity->local_keyword_indices[i];
        if (raw >= shader->keyword_names.count) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_INVALID_RAW_INDEX,
                           stage_index, subprogram_index, -1, raw);
            return false;
        }
        if ((shader->keyword_flags[raw] & 1u) == 0u) {
            set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT,
                           stage_index, subprogram_index, -1, raw);
            return false;
        }
    }

    uint16_t* merged = NULL;
    const size_t merged_capacity = global_count + local_count;
    if (!checked_allocate((void**)&merged, merged_capacity, sizeof(*merged))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, subprogram_index, -1, -1);
        return false;
    }
    size_t global_index = 0;
    size_t local_index = 0;
    size_t merged_count = 0;
    while (global_index < global_count || local_index < local_count) {
        if (local_index == local_count ||
            (global_index < global_count &&
             identity->global_keyword_indices[global_index] <
                 identity->local_keyword_indices[local_index])) {
            merged[merged_count++] = (uint16_t)
                identity->global_keyword_indices[global_index++];
        } else if (global_index == global_count ||
                   identity->local_keyword_indices[local_index] <
                       identity->global_keyword_indices[global_index]) {
            merged[merged_count++] = (uint16_t)
                identity->local_keyword_indices[local_index++];
        } else {
            const int raw = identity->global_keyword_indices[global_index];
            mem_free(merged, merged_capacity * sizeof(*merged));
            set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT,
                           stage_index, subprogram_index, -1, raw);
            return false;
        }
    }
    ShaderLabVariantState state = {merged, merged_count};
    if (!state_is_subset(&state, pass_mask)) {
        int offending = -1;
        for (size_t i = 0; i < merged_count; ++i) {
            if (!state_contains(pass_mask, merged[i])) {
                offending = merged[i];
                break;
            }
        }
        mem_free(merged, merged_capacity * sizeof(*merged));
        set_diagnostic(
            diagnostic,
            SHADERLAB_VARIANT_PLAN_CANDIDATE_OUTSIDE_PASS_MASK,
            stage_index, subprogram_index, -1, offending);
        return false;
    }
    out_state->keyword_indices = merged;
    out_state->keyword_count = merged_count;
    return true;
}

static bool stage_collect_variants(
    const SerializedShader* shader,
    const SerializedPass* pass,
    const ShaderLabVariantState* pass_mask,
    int stage_index,
    ShaderLabPassStageVariantPlan* stage,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    if (pass->subprogram_count[stage_index] < 0 ||
        (pass->subprogram_count[stage_index] != 0 &&
         (!pass->subprograms[stage_index] ||
          !pass->subprogram_identities[stage_index]))) {
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t d3d_count = 0;
    for (int i = 0; i < pass->subprogram_count[stage_index]; ++i) {
        if (serialized_pass_subprogram_is_platform(pass, stage_index, i, 4))
            ++d3d_count;
    }
    if (d3d_count == 0) return true;
    if (!checked_allocate((void**)&stage->variants, d3d_count,
                          sizeof(*stage->variants))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    stage->active = true;
    stage->variant_count = d3d_count;

    size_t output_index = 0;
    size_t tier_counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < pass->subprogram_count[stage_index]; ++i) {
        if (!serialized_pass_subprogram_is_platform(pass, stage_index, i, 4))
            continue;
        const SerializedSubProgramIdentity* identity =
            &pass->subprogram_identities[stage_index][i];
        if (identity->hardware_tier_group < 0 ||
            identity->hardware_tier_group > 3) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
                           stage_index, i, -1, -1);
            return false;
        }
        ShaderLabPlannedVariant* variant = &stage->variants[output_index++];
        variant->subprogram_index = i;
        variant->hardware_tier_group = identity->hardware_tier_group;
        ShaderLabVariantState state;
        if (!identity_to_state(shader, identity, pass_mask, stage_index, i,
                               &state, diagnostic)) return false;
        variant->keyword_count = state.keyword_count;
        variant->keyword_indices = (uint16_t*)state.keyword_indices;
        ++tier_counts[(size_t)identity->hardware_tier_group];
    }

    const bool has_generic = tier_counts[3] != 0;
    const bool has_specific = tier_counts[0] != 0 || tier_counts[1] != 0 ||
                              tier_counts[2] != 0;
    if ((has_generic && has_specific) ||
        (has_specific &&
         (tier_counts[0] == 0 || tier_counts[0] != tier_counts[1] ||
          tier_counts[0] != tier_counts[2]))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
                       stage_index, -1, -1, -1);
        return false;
    }
    stage->uses_specific_hardware_tiers = has_specific;
    stage->state_count = has_generic ? tier_counts[3] : tier_counts[0];
    if (!checked_allocate((void**)&stage->ordered_states, stage->state_count,
                          sizeof(*stage->ordered_states))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }

    const int reference_tier = has_generic ? 3 : 0;
    size_t state_index = 0;
    for (size_t i = 0; i < stage->variant_count; ++i) {
        const ShaderLabPlannedVariant* variant = &stage->variants[i];
        if (variant->hardware_tier_group != reference_tier) continue;
        if (!copy_state(&stage->ordered_states[state_index],
                        variant->keyword_indices, variant->keyword_count)) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, variant->subprogram_index, -1, -1);
            return false;
        }
        for (size_t previous = 0; previous < state_index; ++previous) {
            if (state_equal(&stage->ordered_states[previous],
                            &stage->ordered_states[state_index])) {
                set_diagnostic(
                    diagnostic,
                    SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
                    stage_index, variant->subprogram_index,
                    stage->variants[previous].subprogram_index, -1);
                return false;
            }
        }
        ++state_index;
    }
    if (state_index != stage->state_count) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
                       stage_index, -1, -1, -1);
        return false;
    }

    if (has_specific) {
        for (int tier = 1; tier < 3; ++tier) {
            state_index = 0;
            for (size_t i = 0; i < stage->variant_count; ++i) {
                const ShaderLabPlannedVariant* variant = &stage->variants[i];
                if (variant->hardware_tier_group != tier) continue;
                const ShaderLabVariantState state = {
                    variant->keyword_indices, variant->keyword_count};
                if (state_index >= stage->state_count ||
                    !state_equal(&state, &stage->ordered_states[state_index])) {
                    set_diagnostic(
                        diagnostic,
                        SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
                        stage_index, variant->subprogram_index, -1, -1);
                    return false;
                }
                ++state_index;
            }
            if (state_index != stage->state_count) {
                set_diagnostic(
                    diagnostic,
                    SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
                    stage_index, -1, -1, -1);
                return false;
            }
        }
    }
    return true;
}

static bool keyword_is_builtin(const ShaderLabVariantPlan* plan,
                               const ShaderLabBuiltinVariantDomain* domain,
                               uint16_t raw_index,
                               size_t* out_builtin_index) {
    const char* name = plan->shader->keyword_names.keywords[raw_index];
    for (size_t i = 0; i < domain->keyword_count; ++i) {
        if (strcmp(name, domain->keyword_names[i]) == 0) {
            if (out_builtin_index) *out_builtin_index = i;
            return true;
        }
    }
    return false;
}

static size_t included_builtin_count(
    const ShaderLabBuiltinVariantDomain* domain,
    uint32_t exclusions) {
    size_t count = 0;
    for (size_t i = 0; i < domain->variant_count; ++i) {
        if (shaderlab_builtin_variant_is_included(domain, i, exclusions))
            ++count;
    }
    return count;
}

static bool project_builtin_state(
    const ShaderLabVariantPlan* plan,
    const ShaderLabBuiltinVariantDomain* domain,
    const ShaderLabVariantState* state,
    uint16_t* out_builtin_mask,
    ShaderLabVariantState* out_external,
    ShaderLabVariantPlanDiagnostic* diagnostic,
    int stage_index) {
    uint16_t builtin_mask = 0;
    size_t external_count = 0;
    for (size_t i = 0; i < state->keyword_count; ++i) {
        size_t builtin_index = 0;
        if (keyword_is_builtin(plan, domain, state->keyword_indices[i],
                               &builtin_index)) {
            if ((plan->shader->keyword_flags[state->keyword_indices[i]] & 1u)
                != 0u) {
                set_diagnostic(diagnostic,
                               SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT,
                               stage_index, -1, -1,
                               state->keyword_indices[i]);
                return false;
            }
            builtin_mask |= (uint16_t)(UINT16_C(1) << builtin_index);
        } else {
            ++external_count;
        }
    }
    uint16_t* external = NULL;
    if (!checked_allocate((void**)&external, external_count,
                          sizeof(*external))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t output_index = 0;
    for (size_t i = 0; i < state->keyword_count; ++i) {
        if (!keyword_is_builtin(plan, domain, state->keyword_indices[i], NULL))
            external[output_index++] = state->keyword_indices[i];
    }
    *out_builtin_mask = builtin_mask;
    out_external->keyword_indices = external;
    out_external->keyword_count = external_count;
    return true;
}

typedef struct {
    OwnedStateList external_states;
    uint16_t* observed_builtin_masks;
    size_t observed_builtin_count;
} BuiltinProjection;

static void builtin_projection_free(BuiltinProjection* projection) {
    if (!projection) return;
    free_state_list(&projection->external_states);
    if (projection->observed_builtin_masks) {
        mem_free(projection->observed_builtin_masks,
                 projection->observed_builtin_count *
                     sizeof(*projection->observed_builtin_masks));
    }
    memset(projection, 0, sizeof(*projection));
}

static size_t projection_external_keyword_count(
    const ShaderLabVariantPlan* plan,
    const BuiltinProjection* projection) {
    size_t count = 0;
    for (int raw = 0; raw < plan->shader->keyword_names.count; ++raw) {
        bool present = false;
        for (size_t state_index = 0;
             state_index < projection->external_states.count && !present;
             ++state_index) {
            present = state_contains(
                &projection->external_states.states[state_index],
                (uint16_t)raw);
        }
        if (present) ++count;
    }
    return count;
}

/* The stripped player may retain a strict subset of a compiler built-in
 * table.  This projection is only a candidate: the planner later accepts a
 * built-in directive when its complete generated candidate sequence exactly
 * equals the serialized sequence.  Sparse or reordered products fall back to
 * the symbolic Boolean selector instead of assigning meaning to missing rows. */
static bool stage_project_builtin_subset(
    const ShaderLabVariantPlan* plan,
    int stage_index,
    ShaderLabBuiltinVariantFamily family,
    BuiltinProjection* out_projection,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    memset(out_projection, 0, sizeof(*out_projection));
    const ShaderLabPassStageVariantPlan* stage = &plan->stages[stage_index];
    ShaderLabBuiltinVariantDomain domain;
    if (!stage->active ||
        !shaderlab_builtin_variant_domain_get(
            family, (UnitySerializedProgramStage)stage_index, &domain)) {
        return false;
    }
    uint16_t* all_masks = NULL;
    ShaderLabVariantState* all_external = NULL;
    uint16_t* unique_masks = NULL;
    ShaderLabVariantState* unique_external = NULL;
    if (!checked_allocate((void**)&all_masks, stage->state_count,
                          sizeof(*all_masks)) ||
        !checked_allocate((void**)&all_external, stage->state_count,
                          sizeof(*all_external)) ||
        !checked_allocate((void**)&unique_masks, stage->state_count,
                          sizeof(*unique_masks)) ||
        !checked_allocate((void**)&unique_external, stage->state_count,
                          sizeof(*unique_external))) {
        if (all_masks)
            mem_free(all_masks, stage->state_count * sizeof(*all_masks));
        if (all_external)
            mem_free(all_external,
                     stage->state_count * sizeof(*all_external));
        if (unique_masks)
            mem_free(unique_masks, stage->state_count * sizeof(*unique_masks));
        if (unique_external)
            mem_free(unique_external,
                     stage->state_count * sizeof(*unique_external));
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t unique_mask_count = 0;
    size_t unique_external_count = 0;
    bool success = false;
    for (size_t i = 0; i < stage->state_count; ++i) {
        if (!project_builtin_state(plan, &domain, &stage->ordered_states[i],
                                   &all_masks[i], &all_external[i], diagnostic,
                                   stage_index)) goto cleanup;
        size_t row = 0;
        if (!shaderlab_builtin_variant_find_mask(
                &domain, all_masks[i], SHADERLAB_BUILTIN_EXCLUDE_NONE, &row)) {
            goto cleanup;
        }
        bool seen_mask = false;
        for (size_t previous = 0; previous < unique_mask_count; ++previous) {
            if (unique_masks[previous] == all_masks[i]) {
                seen_mask = true;
                break;
            }
        }
        if (!seen_mask) unique_masks[unique_mask_count++] = all_masks[i];
        bool seen_external = false;
        for (size_t previous = 0; previous < unique_external_count;
             ++previous) {
            if (state_equal(&unique_external[previous], &all_external[i])) {
                seen_external = true;
                break;
            }
        }
        if (!seen_external &&
            !copy_state(&unique_external[unique_external_count++],
                        all_external[i].keyword_indices,
                        all_external[i].keyword_count)) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, -1, -1, -1);
            goto cleanup;
        }
    }

    if (unique_mask_count == 0 || unique_external_count == 0 ||
        unique_mask_count > SIZE_MAX / unique_external_count) {
        goto cleanup;
    }
    bool explains_observed_keyword = false;
    for (size_t i = 0; i < unique_mask_count; ++i) {
        if (unique_masks[i] != 0) {
            explains_observed_keyword = true;
            break;
        }
    }
    if (!explains_observed_keyword) {
        goto cleanup;
    }
    size_t domain_cursor = 0;
    for (size_t i = 0; i < unique_mask_count; ++i) {
        while (domain_cursor < domain.variant_count &&
               domain.variant_masks[domain_cursor] != unique_masks[i])
            ++domain_cursor;
        if (domain_cursor == domain.variant_count) {
            goto cleanup;
        }
        ++domain_cursor;
    }
    if (!checked_allocate(
            (void**)&out_projection->observed_builtin_masks,
            unique_mask_count,
            sizeof(*out_projection->observed_builtin_masks))) {
        builtin_projection_free(out_projection);
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        goto cleanup;
    }
    out_projection->observed_builtin_count = unique_mask_count;
    if (!checked_allocate((void**)&out_projection->external_states.states,
                          unique_external_count,
                          sizeof(*out_projection->external_states.states))) {
        builtin_projection_free(out_projection);
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        goto cleanup;
    }
    out_projection->external_states.count = unique_external_count;
    memcpy(out_projection->observed_builtin_masks, unique_masks,
           unique_mask_count * sizeof(*unique_masks));
    for (size_t i = 0; i < unique_external_count; ++i) {
        out_projection->external_states.states[i] = unique_external[i];
        memset(&unique_external[i], 0, sizeof(unique_external[i]));
    }
    success = true;

cleanup:
    for (size_t i = 0; i < stage->state_count; ++i)
        free_state(&all_external[i]);
    for (size_t i = 0; i < unique_external_count; ++i)
        free_state(&unique_external[i]);
    mem_free(all_external, stage->state_count * sizeof(*all_external));
    mem_free(all_masks, stage->state_count * sizeof(*all_masks));
    mem_free(unique_external,
             stage->state_count * sizeof(*unique_external));
    mem_free(unique_masks, stage->state_count * sizeof(*unique_masks));
    return success;
}

typedef struct {
    uint16_t* keywords;
    size_t keyword_count;
    bool has_default;
    bool is_local;
    bool placed;
} FactorComponent;

static size_t dsu_find(size_t* parents, size_t index) {
    size_t root = index;
    while (parents[root] != root) root = parents[root];
    while (parents[index] != index) {
        const size_t next = parents[index];
        parents[index] = root;
        index = next;
    }
    return root;
}

static bool keywords_never_cooccur(const ShaderLabVariantState* states,
                                   size_t state_count,
                                   uint16_t left,
                                   uint16_t right) {
    for (size_t i = 0; i < state_count; ++i) {
        if (state_contains(&states[i], left) &&
            state_contains(&states[i], right)) return false;
    }
    return true;
}

static int component_option_at(const FactorComponent* component,
                               const ShaderLabVariantState* state) {
    int option = -1;
    for (size_t i = 0; i < component->keyword_count; ++i) {
        if (!state_contains(state, component->keywords[i])) continue;
        if (option != -1) return -2;
        option = (int)component->keywords[i];
    }
    return option;
}

static void free_components(FactorComponent* components,
                            size_t component_count) {
    if (!components) return;
    for (size_t i = 0; i < component_count; ++i) {
        if (components[i].keywords) {
            mem_free(components[i].keywords,
                     components[i].keyword_count *
                         sizeof(*components[i].keywords));
        }
    }
    mem_free(components, component_count * sizeof(*components));
}

static bool component_matches_stride(
    const FactorComponent* component,
    const ShaderLabVariantState* states,
    size_t state_count,
    size_t stride,
    uint16_t** out_ordered_keywords) {
    *out_ordered_keywords = NULL;
    const size_t radix = component->keyword_count +
                         (component->has_default ? 1u : 0u);
    if (radix <= 1 || stride > state_count ||
        radix > state_count / stride) return false;
    uint16_t* options = NULL;
    if (!checked_allocate((void**)&options, component->keyword_count,
                          sizeof(*options))) return false;
    size_t first_option = 0;
    if (component->has_default) {
        if (component_option_at(component, &states[0]) != -1) {
            mem_free(options,
                     component->keyword_count * sizeof(*options));
            return false;
        }
        first_option = 1;
    }
    for (size_t option_index = first_option; option_index < radix;
         ++option_index) {
        const int option = component_option_at(
            component, &states[option_index * stride]);
        if (option < 0) {
            mem_free(options,
                     component->keyword_count * sizeof(*options));
            return false;
        }
        const size_t output_index = option_index - first_option;
        for (size_t previous = 0; previous < output_index; ++previous) {
            if (options[previous] == (uint16_t)option) {
                mem_free(options,
                         component->keyword_count * sizeof(*options));
                return false;
            }
        }
        options[output_index] = (uint16_t)option;
    }
    for (size_t i = 0; i < state_count; ++i) {
        const size_t sequence_option = (i / stride) % radix;
        const int expected =
            component->has_default && sequence_option == 0
                ? -1
                : (int)options[sequence_option - first_option];
        if (component_option_at(component, &states[i]) != expected) {
            mem_free(options,
                     component->keyword_count * sizeof(*options));
            return false;
        }
    }
    *out_ordered_keywords = options;
    return true;
}

static bool factor_ordered_states(
    const SerializedShader* shader,
    const ShaderLabVariantState* states,
    size_t state_count,
    int stage_index,
    ShaderLabVariantAxis** out_axes,
    size_t* out_axis_count,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    *out_axes = NULL;
    *out_axis_count = 0;
    if (!states || state_count == 0) {
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
                       stage_index, -1, -1, -1);
        return false;
    }

    uint8_t* used = NULL;
    if (!checked_allocate((void**)&used,
                          (size_t)shader->keyword_names.count,
                          sizeof(*used))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t used_count = 0;
    for (size_t i = 0; i < state_count; ++i) {
        if (!shaderlab_variant_state_is_canonical(&states[i])) {
            mem_free(used, (size_t)shader->keyword_names.count);
            set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
                           stage_index, -1, -1, -1);
            return false;
        }
        for (size_t j = 0; j < states[i].keyword_count; ++j) {
            const uint16_t raw = states[i].keyword_indices[j];
            if (!used[raw]) {
                used[raw] = 1;
                ++used_count;
            }
        }
    }
    if (used_count == 0) {
        mem_free(used, (size_t)shader->keyword_names.count);
        if (state_count != 1) {
            set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
                           stage_index, -1, -1, -1);
            return false;
        }
        return true;
    }
    uint16_t* universe = NULL;
    size_t* parents = NULL;
    if (!checked_allocate((void**)&universe, used_count,
                          sizeof(*universe)) ||
        !checked_allocate((void**)&parents, used_count, sizeof(*parents))) {
        if (universe) mem_free(universe, used_count * sizeof(*universe));
        if (parents) mem_free(parents, used_count * sizeof(*parents));
        mem_free(used, (size_t)shader->keyword_names.count);
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t universe_index = 0;
    for (int raw = 0; raw < shader->keyword_names.count; ++raw) {
        if (used[raw]) universe[universe_index++] = (uint16_t)raw;
    }
    mem_free(used, (size_t)shader->keyword_names.count);
    for (size_t i = 0; i < used_count; ++i) parents[i] = i;
    for (size_t i = 0; i < used_count; ++i) {
        for (size_t j = i + 1; j < used_count; ++j) {
            if (!keywords_never_cooccur(states, state_count,
                                        universe[i], universe[j])) continue;
            const size_t left = dsu_find(parents, i);
            const size_t right = dsu_find(parents, j);
            if (left != right) parents[right] = left;
        }
    }
    size_t component_count = 0;
    for (size_t i = 0; i < used_count; ++i) {
        if (dsu_find(parents, i) == i) ++component_count;
    }
    FactorComponent* components = NULL;
    size_t* component_for_keyword = NULL;
    if (!checked_allocate((void**)&components, component_count,
                          sizeof(*components)) ||
        !checked_allocate((void**)&component_for_keyword, used_count,
                          sizeof(*component_for_keyword))) {
        if (components)
            mem_free(components, component_count * sizeof(*components));
        if (component_for_keyword)
            mem_free(component_for_keyword,
                     used_count * sizeof(*component_for_keyword));
        mem_free(parents, used_count * sizeof(*parents));
        mem_free(universe, used_count * sizeof(*universe));
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t assigned_components = 0;
    for (size_t i = 0; i < used_count; ++i) {
        const size_t root = dsu_find(parents, i);
        size_t component_index = SIZE_MAX;
        for (size_t previous = 0; previous < i; ++previous) {
            if (dsu_find(parents, previous) == root) {
                component_index = component_for_keyword[previous];
                break;
            }
        }
        if (component_index == SIZE_MAX) component_index = assigned_components++;
        component_for_keyword[i] = component_index;
        ++components[component_index].keyword_count;
    }
    for (size_t i = 0; i < component_count; ++i) {
        if (!checked_allocate((void**)&components[i].keywords,
                              components[i].keyword_count,
                              sizeof(*components[i].keywords))) {
            free_components(components, component_count);
            mem_free(component_for_keyword,
                     used_count * sizeof(*component_for_keyword));
            mem_free(parents, used_count * sizeof(*parents));
            mem_free(universe, used_count * sizeof(*universe));
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, -1, -1, -1);
            return false;
        }
        components[i].keyword_count = 0;
    }
    for (size_t i = 0; i < used_count; ++i) {
        FactorComponent* component = &components[component_for_keyword[i]];
        component->keywords[component->keyword_count++] = universe[i];
    }
    mem_free(component_for_keyword,
             used_count * sizeof(*component_for_keyword));
    mem_free(parents, used_count * sizeof(*parents));
    mem_free(universe, used_count * sizeof(*universe));

    size_t product = 1;
    for (size_t component_index = 0; component_index < component_count;
         ++component_index) {
        FactorComponent* component = &components[component_index];
        for (size_t left = 0; left < component->keyword_count; ++left) {
            for (size_t right = left + 1; right < component->keyword_count;
                 ++right) {
                if (!keywords_never_cooccur(states, state_count,
                                            component->keywords[left],
                                            component->keywords[right])) {
                    free_components(components, component_count);
                    set_diagnostic(
                        diagnostic, SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
                        stage_index, -1, -1, -1);
                    return false;
                }
            }
        }
        component->has_default = false;
        for (size_t state_index = 0; state_index < state_count; ++state_index) {
            const int option = component_option_at(component,
                                                   &states[state_index]);
            if (option == -2) {
                free_components(components, component_count);
                set_diagnostic(diagnostic,
                               SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
                               stage_index, -1, -1, -1);
                return false;
            }
            if (option == -1) component->has_default = true;
        }
        component->is_local =
            (shader->keyword_flags[component->keywords[0]] & 1u) != 0u;
        for (size_t i = 1; i < component->keyword_count; ++i) {
            const bool is_local =
                (shader->keyword_flags[component->keywords[i]] & 1u) != 0u;
            if (is_local != component->is_local) {
                const int raw = component->keywords[i];
                free_components(components, component_count);
                set_diagnostic(diagnostic,
                               SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT,
                               stage_index, -1, -1, raw);
                return false;
            }
        }
        const size_t radix = component->keyword_count +
                             (component->has_default ? 1u : 0u);
        if (radix == 0 || product > SIZE_MAX / radix) {
            free_components(components, component_count);
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
                           stage_index, -1, -1, -1);
            return false;
        }
        product *= radix;
    }
    if (product != state_count) {
        free_components(components, component_count);
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_NONFACTORABLE,
                       stage_index, -1, -1, -1);
        return false;
    }

    ShaderLabVariantAxis* axes = NULL;
    if (!checked_allocate((void**)&axes, component_count, sizeof(*axes))) {
        free_components(components, component_count);
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t axis_count = 0;
    size_t stride = 1;
    for (;;) {
        size_t selected = SIZE_MAX;
        uint16_t* selected_order = NULL;
        for (size_t i = 0; i < component_count; ++i) {
            FactorComponent* component = &components[i];
            const size_t radix = component->keyword_count +
                                 (component->has_default ? 1u : 0u);
            if (component->placed || radix <= 1) continue;
            uint16_t* order = NULL;
            if (!component_matches_stride(component, states, state_count,
                                          stride, &order)) continue;
            if (selected == SIZE_MAX ||
                component->keywords[0] < components[selected].keywords[0]) {
                if (selected_order) {
                    mem_free(selected_order,
                             components[selected].keyword_count *
                                 sizeof(*selected_order));
                }
                selected = i;
                selected_order = order;
            } else {
                mem_free(order,
                         component->keyword_count * sizeof(*order));
            }
        }
        if (selected == SIZE_MAX) break;
        FactorComponent* component = &components[selected];
        ShaderLabVariantAxis* axis = &axes[axis_count++];
        axis->has_default = component->has_default;
        axis->is_local = component->is_local;
        axis->keyword_count = component->keyword_count;
        axis->keyword_indices = selected_order;
        component->placed = true;
        const size_t radix = component->keyword_count +
                             (component->has_default ? 1u : 0u);
        stride *= radix;
    }
    for (size_t i = 0; i < component_count; ++i) {
        FactorComponent* component = &components[i];
        if (component->placed) continue;
        const size_t radix = component->keyword_count +
                             (component->has_default ? 1u : 0u);
        if (radix != 1) {
            for (size_t axis_index = 0; axis_index < axis_count; ++axis_index) {
                mem_free(axes[axis_index].keyword_indices,
                         axes[axis_index].keyword_count *
                             sizeof(*axes[axis_index].keyword_indices));
            }
            mem_free(axes, component_count * sizeof(*axes));
            free_components(components, component_count);
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ORDER_MISMATCH,
                           stage_index, -1, -1, -1);
            return false;
        }
        ShaderLabVariantAxis* axis = &axes[axis_count++];
        axis->has_default = false;
        axis->is_local = component->is_local;
        axis->keyword_count = 1;
        if (!checked_allocate((void**)&axis->keyword_indices, 1,
                              sizeof(*axis->keyword_indices))) {
            for (size_t axis_index = 0; axis_index + 1 < axis_count;
                 ++axis_index) {
                mem_free(axes[axis_index].keyword_indices,
                         axes[axis_index].keyword_count *
                             sizeof(*axes[axis_index].keyword_indices));
            }
            mem_free(axes, component_count * sizeof(*axes));
            free_components(components, component_count);
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, -1, -1, -1);
            return false;
        }
        axis->keyword_indices[0] = component->keywords[0];
        component->placed = true;
    }
    free_components(components, component_count);
    if (axis_count != component_count || stride != state_count) {
        for (size_t i = 0; i < axis_count; ++i) {
            mem_free(axes[i].keyword_indices,
                     axes[i].keyword_count * sizeof(*axes[i].keyword_indices));
        }
        mem_free(axes, component_count * sizeof(*axes));
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_ORDER_MISMATCH,
                       stage_index, -1, -1, -1);
        return false;
    }
    *out_axes = axes;
    *out_axis_count = axis_count;
    return true;
}

static void free_axes(ShaderLabVariantAxis* axes, size_t axis_count) {
    if (!axes) return;
    for (size_t i = 0; i < axis_count; ++i) {
        if (axes[i].keyword_indices) {
            mem_free(axes[i].keyword_indices,
                     axes[i].keyword_count *
                         sizeof(*axes[i].keyword_indices));
        }
    }
    mem_free(axes, axis_count * sizeof(*axes));
}

static int stage_observed_keyword_index(
    const ShaderLabVariantPlan* plan, int stage_index, const char* name) {
    if (!plan || !plan->shader || !name || stage_index < 0 ||
        (size_t)stage_index >= ARRAY_COUNT(plan->stages)) {
        return -1;
    }
    const ShaderLabPassStageVariantPlan* stage =
        &plan->stages[stage_index];
    int found = -1;
    for (int raw = 0; raw < plan->shader->keyword_names.count; ++raw) {
        if (strcmp(plan->shader->keyword_names.keywords[raw], name) != 0)
            continue;
        bool observed = false;
        for (size_t state = 0U; state < stage->state_count && !observed;
             ++state) {
            observed = state_contains(
                &stage->ordered_states[state], (uint16_t)raw);
        }
        if (!observed) continue;
        if (found >= 0) return -2;
        found = raw;
    }
    return found;
}

static bool merge_generated_state(
    const ShaderLabVariantPlan* plan,
    const ShaderLabBuiltinVariantDomain* domain,
    uint16_t builtin_mask,
    const ShaderLabVariantState* external,
    ShaderLabVariantState* output,
    ShaderLabVariantPlanDiagnostic* diagnostic,
    int stage_index) {
    uint16_t builtin_indices[16];
    size_t builtin_count = 0;
    for (size_t bit = 0; bit < domain->keyword_count; ++bit) {
        if ((builtin_mask & (uint16_t)(UINT16_C(1) << bit)) == 0) continue;
        const int raw = stage_observed_keyword_index(
            plan, stage_index, domain->keyword_names[bit]);
        if (raw < 0 || (plan->shader->keyword_flags[raw] & 1u) != 0u) {
            set_diagnostic(diagnostic,
                           raw == -2
                               ? SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED
                               : (raw < 0
                                      ? SHADERLAB_VARIANT_PLAN_MISSING_KEYWORD_AUTHORITY
                                      : SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT),
                           stage_index, -1, -1, raw);
            return false;
        }
        builtin_indices[builtin_count++] = (uint16_t)raw;
    }
    for (size_t i = 1; i < builtin_count; ++i) {
        const uint16_t value = builtin_indices[i];
        size_t insertion = i;
        while (insertion != 0 && builtin_indices[insertion - 1] > value) {
            builtin_indices[insertion] = builtin_indices[insertion - 1];
            --insertion;
        }
        builtin_indices[insertion] = value;
    }
    if (builtin_count > SIZE_MAX - external->keyword_count) return false;
    const size_t count = builtin_count + external->keyword_count;
    uint16_t* merged = NULL;
    if (!checked_allocate((void**)&merged, count, sizeof(*merged))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t left = 0;
    size_t right = 0;
    size_t result = 0;
    while (left < builtin_count || right < external->keyword_count) {
        if (right == external->keyword_count ||
            (left < builtin_count &&
             builtin_indices[left] < external->keyword_indices[right])) {
            merged[result++] = builtin_indices[left++];
        } else if (left == builtin_count ||
                   external->keyword_indices[right] < builtin_indices[left]) {
            merged[result++] = external->keyword_indices[right++];
        } else {
            mem_free(merged, count * sizeof(*merged));
            set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
                           stage_index, -1, -1,
                           builtin_indices[left]);
            return false;
        }
    }
    output->keyword_indices = merged;
    output->keyword_count = result;
    return true;
}

static bool build_generated_builtin_domain(
    const ShaderLabVariantPlan* plan,
    int stage_index,
    const OwnedStateList* external_states,
    uint32_t exclusions,
    ShaderLabPassStageVariantPlan* stage,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    ShaderLabBuiltinVariantDomain domain;
    if (!shaderlab_builtin_variant_domain_get(
            plan->builtin_family, (UnitySerializedProgramStage)stage_index,
            &domain) ||
        included_builtin_count(&domain, exclusions) >
            SIZE_MAX / external_states->count) return false;
    const size_t included_count = included_builtin_count(&domain, exclusions);
    if (included_count == 0) return false;
    stage->generated_state_count =
        included_count * external_states->count;
    if (!checked_allocate((void**)&stage->generated_states,
                          stage->generated_state_count,
                          sizeof(*stage->generated_states)) ||
        !checked_allocate((void**)&stage->generated_aliases,
                          stage->generated_state_count,
                          sizeof(*stage->generated_aliases))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    if (stage->builtin_axis_position > stage->axis_count) {
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t faster_external_count = 1;
    for (size_t axis_index = 0;
         axis_index < stage->builtin_axis_position; ++axis_index) {
        const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
        const size_t radix = axis->keyword_count +
                             (axis->has_default ? 1u : 0u);
        if (radix == 0 || faster_external_count > SIZE_MAX / radix) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED,
                           stage_index, -1, -1, -1);
            return false;
        }
        faster_external_count *= radix;
    }
    if (faster_external_count == 0 ||
        external_states->count % faster_external_count != 0) {
        set_diagnostic(diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
                       stage_index, -1, -1, -1);
        return false;
    }
    const size_t slower_external_count =
        external_states->count / faster_external_count;
    size_t output = 0;
    for (size_t slower = 0; slower < slower_external_count; ++slower) {
        for (size_t row = 0; row < domain.variant_count; ++row) {
            if (!shaderlab_builtin_variant_is_included(
                    &domain, row, exclusions)) continue;
            for (size_t faster = 0; faster < faster_external_count;
                 ++faster) {
                const size_t external =
                    slower * faster_external_count + faster;
                if (!merge_generated_state(
                        plan, &domain, domain.variant_masks[row],
                        &external_states->states[external],
                        &stage->generated_states[output], diagnostic,
                        stage_index)) return false;
                ++output;
            }
        }
    }
    return output == stage->generated_state_count;
}

static bool build_generated_identity_domain(
    ShaderLabPassStageVariantPlan* stage,
    ShaderLabVariantPlanDiagnostic* diagnostic,
    int stage_index) {
    stage->generated_state_count = stage->state_count;
    if (!checked_allocate((void**)&stage->generated_states,
                          stage->generated_state_count,
                          sizeof(*stage->generated_states)) ||
        !checked_allocate((void**)&stage->generated_aliases,
                          stage->generated_state_count,
                          sizeof(*stage->generated_aliases))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    for (size_t i = 0; i < stage->state_count; ++i) {
        if (!copy_state(&stage->generated_states[i],
                        stage->ordered_states[i].keyword_indices,
                        stage->ordered_states[i].keyword_count)) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, -1, -1, -1);
            return false;
        }
        stage->generated_aliases[i] = i;
    }
    return true;
}

static void clear_generated_domain(ShaderLabPassStageVariantPlan* stage) {
    if (stage->generated_states) {
        for (size_t i = 0; i < stage->generated_state_count; ++i)
            free_state(&stage->generated_states[i]);
        mem_free(stage->generated_states,
                 stage->generated_state_count *
                     sizeof(*stage->generated_states));
    }
    if (stage->generated_aliases) {
        mem_free(stage->generated_aliases,
                 stage->generated_state_count *
                     sizeof(*stage->generated_aliases));
    }
    stage->generated_states = NULL;
    stage->generated_aliases = NULL;
    stage->generated_state_count = 0;
    stage->generated_domain_is_symbolic_boolean = false;
}

static bool projection_survives_exclusions(
    const ShaderLabBuiltinVariantDomain* domain,
    const BuiltinProjection* projection,
    uint32_t exclusions) {
    for (size_t i = 0; i < projection->observed_builtin_count; ++i) {
        size_t row = 0;
        if (!shaderlab_builtin_variant_find_mask(
                domain, projection->observed_builtin_masks[i], exclusions,
                &row)) return false;
    }
    return true;
}

static unsigned int exclusion_bit_count(uint32_t value) {
    unsigned int count = 0;
    while (value != 0) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

#define SHADERLAB_VARIANT_MAX_SYMBOLIC_SOURCE_TERMS UINT64_C(100000000)

static bool build_boolean_axes_for_states(
    const SerializedShader* shader,
    const ShaderLabVariantState* states,
    size_t state_count,
    ShaderLabPassStageVariantPlan* stage,
    ShaderLabVariantPlanDiagnostic* diagnostic,
    int stage_index) {
    uint8_t* used = NULL;
    if (!checked_allocate((void**)&used,
                          (size_t)shader->keyword_names.count,
                          sizeof(*used))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    size_t keyword_count = 0;
    for (size_t i = 0; i < state_count; ++i) {
        for (size_t j = 0; j < states[i].keyword_count; ++j) {
            const uint16_t raw = states[i].keyword_indices[j];
            if (!used[raw]) {
                used[raw] = 1;
                ++keyword_count;
            }
        }
    }
    if (keyword_count >= sizeof(size_t) * CHAR_BIT) {
        mem_free(used, (size_t)shader->keyword_names.count);
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED,
                       stage_index, -1, -1, -1);
        return false;
    }
    free_axes(stage->axes, stage->axis_count);
    stage->axes = NULL;
    stage->axis_count = 0;
    if (!checked_allocate((void**)&stage->axes, keyword_count,
                          sizeof(*stage->axes))) {
        mem_free(used, (size_t)shader->keyword_names.count);
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    /* The zeroed block is immediately owned so failure after any per-axis
     * allocation is fully reclaimable by shaderlab_variant_plan_free(). */
    stage->axis_count = keyword_count;
    size_t axis_index = 0;
    for (int raw = 0; raw < shader->keyword_names.count; ++raw) {
        if (!used[raw]) continue;
        ShaderLabVariantAxis* axis = &stage->axes[axis_index++];
        axis->has_default = true;
        axis->is_local = (shader->keyword_flags[raw] & 1u) != 0u;
        axis->keyword_count = 1;
        if (!checked_allocate((void**)&axis->keyword_indices, 1,
                              sizeof(*axis->keyword_indices))) {
            mem_free(used, (size_t)shader->keyword_names.count);
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, -1, -1, -1);
            return false;
        }
        axis->keyword_indices[0] = (uint16_t)raw;
    }
    mem_free(used, (size_t)shader->keyword_names.count);

    return true;
}

static bool build_axis_state_list(
    const ShaderLabPassStageVariantPlan* stage,
    OwnedStateList* output,
    ShaderLabVariantPlanDiagnostic* diagnostic,
    int stage_index) {
    memset(output, 0, sizeof(*output));
    size_t state_count = 1;
    for (size_t axis_index = 0; axis_index < stage->axis_count;
         ++axis_index) {
        const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
        const size_t radix = axis->keyword_count +
                             (axis->has_default ? 1u : 0u);
        if (radix == 0 || state_count > SIZE_MAX / radix) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED,
                           stage_index, -1, -1, -1);
            return false;
        }
        state_count *= radix;
    }
    if (!checked_allocate((void**)&output->states, state_count,
                          sizeof(*output->states))) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    output->count = state_count;
    for (size_t state_index = 0; state_index < state_count; ++state_index) {
        size_t enabled_count = 0;
        size_t stride = 1;
        for (size_t axis_index = 0; axis_index < stage->axis_count;
             ++axis_index) {
            const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
            const size_t radix = axis->keyword_count +
                                 (axis->has_default ? 1u : 0u);
            const size_t option = (state_index / stride) % radix;
            if (!axis->has_default || option != 0) ++enabled_count;
            stride *= radix;
        }
        uint16_t* indices = NULL;
        if (!checked_allocate((void**)&indices, enabled_count,
                              sizeof(*indices))) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, -1, -1, -1);
            free_state_list(output);
            return false;
        }
        size_t enabled_index = 0;
        stride = 1;
        for (size_t axis_index = 0; axis_index < stage->axis_count;
             ++axis_index) {
            const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
            const size_t radix = axis->keyword_count +
                                 (axis->has_default ? 1u : 0u);
            const size_t option = (state_index / stride) % radix;
            stride *= radix;
            if (axis->has_default && option == 0) continue;
            const size_t keyword_option =
                option - (axis->has_default ? 1u : 0u);
            indices[enabled_index++] = axis->keyword_indices[keyword_option];
        }
        for (size_t i = 1; i < enabled_count; ++i) {
            const uint16_t value = indices[i];
            size_t insertion = i;
            while (insertion != 0 && indices[insertion - 1] > value) {
                indices[insertion] = indices[insertion - 1];
                --insertion;
            }
            indices[insertion] = value;
        }
        output->states[state_index].keyword_indices = indices;
        output->states[state_index].keyword_count = enabled_count;
    }
    return true;
}

static bool build_full_boolean_alias_domain(
    const SerializedShader* shader,
    ShaderLabPassStageVariantPlan* stage,
    ShaderLabVariantPlanDiagnostic* diagnostic,
    int stage_index) {
    if (!build_boolean_axes_for_states(
            shader, stage->ordered_states, stage->state_count, stage,
            diagnostic, stage_index)) return false;

    const uint64_t symbolic_width =
        stage->axis_count == 0 ? UINT64_C(1) :
                                 (uint64_t)stage->axis_count;
    if (stage->axis_count >= sizeof(size_t) * CHAR_BIT ||
        (uint64_t)stage->state_count >
            SHADERLAB_VARIANT_MAX_SYMBOLIC_SOURCE_TERMS /
                symbolic_width) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED,
                       stage_index, -1, -1, -1);
        return false;
    }
    stage->generated_domain_is_symbolic_boolean = true;
    stage->generated_state_count = ((size_t)1u) << stage->axis_count;
    return true;
}

static bool prove_generated_aliases(
    const ShaderLabVariantPlan* plan,
    int stage_index,
    ShaderLabPassStageVariantPlan* stage,
    uint32_t generated_builtin_exclusions,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    if (stage->generated_domain_is_symbolic_boolean) {
        if (plan->has_builtin || stage->axis_count == 0 ||
            stage->axis_count >= sizeof(size_t) * CHAR_BIT ||
            stage->generated_states || stage->generated_aliases ||
            stage->generated_state_count !=
                (((size_t)1u) << stage->axis_count)) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED,
                           stage_index, -1, -1, -1);
            return false;
        }
        uint8_t* covered = NULL;
        if (!checked_allocate((void**)&covered,
                              (size_t)plan->shader->keyword_names.count,
                              sizeof(*covered))) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED,
                           stage_index, -1, -1, -1);
            return false;
        }
        bool valid = true;
        for (size_t axis_index = 0; axis_index < stage->axis_count;
             ++axis_index) {
            const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
            if (!axis->has_default || axis->keyword_count != 1 ||
                !axis->keyword_indices) {
                valid = false;
                break;
            }
            const uint16_t raw = axis->keyword_indices[0];
            if ((size_t)raw >=
                    (size_t)plan->shader->keyword_names.count ||
                covered[raw]) {
                valid = false;
                break;
            }
            covered[raw] = 1;
        }
        for (size_t state_index = 0;
             valid && state_index < stage->state_count; ++state_index) {
            const ShaderLabVariantState* state =
                &stage->ordered_states[state_index];
            if (!shaderlab_variant_state_is_canonical(state)) {
                valid = false;
                break;
            }
            for (size_t keyword = 0; keyword < state->keyword_count;
                 ++keyword) {
                if (!covered[state->keyword_indices[keyword]]) {
                    valid = false;
                    break;
                }
            }
            for (size_t earlier = 0; valid && earlier < state_index;
                 ++earlier) {
                if (state_equal(state, &stage->ordered_states[earlier]))
                    valid = false;
            }
        }
        /* Every axis bit occurs in the serialized domain; otherwise the
         * Boolean product would contain an unauthoritative keyword. */
        for (int raw = 0;
             valid && raw < plan->shader->keyword_names.count; ++raw) {
            if (!covered[raw]) continue;
            bool observed = false;
            for (size_t state_index = 0;
                 state_index < stage->state_count && !observed;
                 ++state_index) {
                observed = state_contains(
                    &stage->ordered_states[state_index], (uint16_t)raw);
            }
            if (!observed) valid = false;
        }
        mem_free(covered, (size_t)plan->shader->keyword_names.count);
        if (!valid) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED,
                           stage_index, -1, -1, -1);
            return false;
        }
        /* The Boolean product contains the exact masked runtime request.
         * Planning keeps that complete domain implicit; stage emission maps
         * its bounded truth table into a subtree-pruned decision tree whose
         * literal leaves use Unity's score and strict first-winner rule.
         * Unique serialized states also make each original body reachable at
         * its own exact request. */
        return true;
    }
    (void)generated_builtin_exclusions;
    if (!stage->generated_states || !stage->generated_aliases ||
        stage->generated_state_count != stage->state_count) {
        set_diagnostic(diagnostic,
                       SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED,
                       stage_index, -1, -1, -1);
        return false;
    }
    for (size_t i = 0; i < stage->state_count; ++i) {
        if (!state_equal(&stage->generated_states[i],
                         &stage->ordered_states[i])) {
            set_diagnostic(diagnostic,
                           SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED,
                           stage_index, -1, (int)i, -1);
            return false;
        }
        stage->generated_aliases[i] = i;
    }
    /* The generated and serialized candidate sequences are byte-for-byte
     * identical.  Therefore Unity's score and first-winner tie rule select
     * the same body for every possible request, without enumerating 2^K
     * requests.  Non-identical products use the symbolic Boolean proof. */
    return true;
}

static bool axis_equal(const ShaderLabVariantAxis* left,
                       const ShaderLabVariantAxis* right) {
    return left && right &&
        left->has_default == right->has_default &&
        left->is_local == right->is_local &&
        left->keyword_count == right->keyword_count &&
        (left->keyword_count == 0 ||
         memcmp(left->keyword_indices, right->keyword_indices,
                left->keyword_count * sizeof(*left->keyword_indices)) == 0);
}

static bool axes_equal(const ShaderLabPassStageVariantPlan* left,
                       const ShaderLabPassStageVariantPlan* right) {
    if (left->axis_count != right->axis_count) return false;
    for (size_t i = 0; i < left->axis_count; ++i) {
        if (!axis_equal(&left->axes[i], &right->axes[i])) return false;
    }
    return true;
}

ShaderLabVariantPlanStatus shaderlab_variant_plan_build(
    const SerializedShader* shader,
    const SerializedPass* pass,
    ShaderLabVariantPlan* out_plan,
    ShaderLabVariantPlanDiagnostic* diagnostic) {
    ShaderLabVariantPlan temporary;
    ShaderLabVariantPlanDiagnostic local_diagnostic;
    ShaderLabVariantPlanDiagnostic* active_diagnostic =
        diagnostic ? diagnostic : &local_diagnostic;
    shaderlab_variant_plan_init(&temporary);
    set_diagnostic(active_diagnostic, SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
                   -1, -1, -1, -1);
    if (!out_plan || !pass ||
        !validate_shader_authority(shader, active_diagnostic) ||
        !pass->has_serialized_platforms || pass->platform_count <= 0 ||
        !pass->platforms) {
        if (active_diagnostic->status ==
            SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT) {
            set_diagnostic(active_diagnostic,
                           SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT,
                           -1, -1, -1, -1);
        }
        return active_diagnostic->status;
    }
    temporary.shader = shader;
    temporary.pass = pass;
    ShaderLabVariantState pass_mask;
    if (!validate_pass_mask(shader, pass, &pass_mask, active_diagnostic))
        goto fail;

    size_t active_stage_count = 0;
    bool tier_mode_set = false;
    for (int stage_index = 0; stage_index < 5; ++stage_index) {
        if (!stage_collect_variants(shader, pass, &pass_mask, stage_index,
                                    &temporary.stages[stage_index],
                                    active_diagnostic)) goto fail;
        if (!temporary.stages[stage_index].active) continue;
        ++active_stage_count;
        if (!tier_mode_set) {
            temporary.uses_specific_hardware_tiers =
                temporary.stages[stage_index].uses_specific_hardware_tiers;
            tier_mode_set = true;
        } else if (temporary.uses_specific_hardware_tiers !=
                   temporary.stages[stage_index]
                       .uses_specific_hardware_tiers) {
            set_diagnostic(active_diagnostic,
                           SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH,
                           stage_index, -1, -1, -1);
            goto fail;
        }
    }
    if (active_stage_count == 0) {
        set_diagnostic(active_diagnostic,
                       SHADERLAB_VARIANT_PLAN_NO_D3D_VARIANTS,
                       -1, -1, -1, -1);
        goto fail;
    }

    /* m_Type=0 is used by real ForwardBase/ForwardAdd passes; it is not
     * directive authority.  Search the pinned compiler tables themselves and
     * accept only a unique ordered Cartesian projection. */
    static const ShaderLabBuiltinVariantFamily family_candidates[] = {
        SHADERLAB_BUILTIN_VARIANT_FWDADD_FULLSHADOWS,
        SHADERLAB_BUILTIN_VARIANT_FWDADD,
        SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA,
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER,
        SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR,
        SHADERLAB_BUILTIN_VARIANT_PREPASSFINAL};
    const size_t family_count = ARRAY_COUNT(family_candidates);
    bool found_builtin = false;
    bool builtin_is_ambiguous = false;
    size_t selected_external_keyword_cost = SIZE_MAX;
    size_t selected_domain_row_cost = SIZE_MAX;
    ShaderLabBuiltinVariantFamily selected_family =
        SHADERLAB_BUILTIN_VARIANT_INVALID;
    for (size_t family_index = 0; family_index < family_count;
         ++family_index) {
        bool family_is_proven = true;
        size_t family_external_keyword_cost = 0;
        size_t family_domain_row_cost = 0;
        for (size_t stage_index = 0;
             stage_index < ARRAY_COUNT(temporary.stages); ++stage_index) {
            if (!temporary.stages[stage_index].active) continue;
            BuiltinProjection projection;
            ShaderLabVariantPlanDiagnostic local;
            set_diagnostic(&local, SHADERLAB_VARIANT_PLAN_OK,
                           (int)stage_index, -1, -1, -1);
            if (!stage_project_builtin_subset(
                    &temporary, (int)stage_index,
                    family_candidates[family_index], &projection, &local)) {
                if (local.status ==
                    SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED) {
                    *active_diagnostic = local;
                    goto fail;
                }
                family_is_proven = false;
                break;
            }
            const size_t stage_external_keyword_cost =
                projection_external_keyword_count(&temporary, &projection);
            ShaderLabBuiltinVariantDomain domain;
            if (family_external_keyword_cost >
                    SIZE_MAX - stage_external_keyword_cost ||
                !shaderlab_builtin_variant_domain_get(
                    family_candidates[family_index],
                    (UnitySerializedProgramStage)stage_index, &domain) ||
                family_domain_row_cost >
                    SIZE_MAX - domain.variant_count) {
                builtin_projection_free(&projection);
                family_is_proven = false;
                break;
            }
            family_external_keyword_cost += stage_external_keyword_cost;
            family_domain_row_cost += domain.variant_count;
            builtin_projection_free(&projection);
        }
        if (!family_is_proven) continue;
        if (!found_builtin ||
            family_external_keyword_cost < selected_external_keyword_cost ||
            (family_external_keyword_cost == selected_external_keyword_cost &&
             family_domain_row_cost < selected_domain_row_cost)) {
            found_builtin = true;
            builtin_is_ambiguous = false;
            selected_family = family_candidates[family_index];
            selected_external_keyword_cost = family_external_keyword_cost;
            selected_domain_row_cost = family_domain_row_cost;
        } else if (
            family_external_keyword_cost == selected_external_keyword_cost &&
            family_domain_row_cost == selected_domain_row_cost) {
            /* Equal-cost distinct compiler tables remain observationally
             * ambiguous.  Do not choose by enum, pass type, or name. */
            builtin_is_ambiguous = true;
        }
    }

    if (builtin_is_ambiguous) {
        found_builtin = false;
        selected_family = SHADERLAB_BUILTIN_VARIANT_INVALID;
    }
    temporary.has_builtin = found_builtin;
    temporary.builtin_family = selected_family;
    temporary.builtin_exclusions = SHADERLAB_BUILTIN_EXCLUDE_NONE;
    for (size_t stage_index = 0; stage_index < ARRAY_COUNT(temporary.stages);
         ++stage_index) {
        ShaderLabPassStageVariantPlan* stage = &temporary.stages[stage_index];
        if (!stage->active) continue;
        if (found_builtin) {
            BuiltinProjection projection;
            if (!stage_project_builtin_subset(
                    &temporary, (int)stage_index, selected_family,
                    &projection, active_diagnostic)) {
                if (active_diagnostic->status ==
                    SHADERLAB_VARIANT_PLAN_OK) {
                    set_diagnostic(
                        active_diagnostic,
                        SHADERLAB_VARIANT_PLAN_ORDER_MISMATCH,
                        (int)stage_index, -1, -1, -1);
                }
                goto fail;
            }
            const bool factorable = factor_ordered_states(
                shader, projection.external_states.states,
                projection.external_states.count, (int)stage_index,
                &stage->axes, &stage->axis_count, active_diagnostic);
            if (!factorable) {
                const ShaderLabVariantPlanStatus factor_status =
                    active_diagnostic->status;
                if (factor_status != SHADERLAB_VARIANT_PLAN_NONFACTORABLE &&
                    factor_status != SHADERLAB_VARIANT_PLAN_ORDER_MISMATCH &&
                    factor_status != SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT) {
                    builtin_projection_free(&projection);
                    goto fail;
                }
                if (!build_boolean_axes_for_states(
                        shader, projection.external_states.states,
                        projection.external_states.count, stage,
                        active_diagnostic, (int)stage_index)) {
                    builtin_projection_free(&projection);
                    goto fail;
                }
            }
            builtin_projection_free(&projection);
        } else {
            bool factorable = false;
            if (!builtin_is_ambiguous) {
                factorable = factor_ordered_states(
                    shader, stage->ordered_states, stage->state_count,
                    (int)stage_index, &stage->axes, &stage->axis_count,
                    active_diagnostic);
            }
            if (factorable) {
                if (!build_generated_identity_domain(
                        stage, active_diagnostic, (int)stage_index)) goto fail;
            } else {
                const ShaderLabVariantPlanStatus factor_status =
                    active_diagnostic->status;
                if (!builtin_is_ambiguous &&
                    factor_status != SHADERLAB_VARIANT_PLAN_NONFACTORABLE &&
                    factor_status != SHADERLAB_VARIANT_PLAN_ORDER_MISMATCH &&
                    factor_status != SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT) {
                    goto fail;
                }
                /* A Release player can retain an arbitrary stripped subset
                 * (or a source order that is not expressible as a direct
                 * pragma product).  Emit one Boolean axis per observed bit;
                 * the stage emitter then uses the bounded complete request
                 * universe to build the recovered literal first-best decision
                 * tree.  This uses only VariantKey order and does not infer
                 * shader_feature semantics. */
                if (!build_full_boolean_alias_domain(
                        shader, stage, active_diagnostic,
                        (int)stage_index)) goto fail;
            }
        }
    }

    if (found_builtin) {
        bool found_exclusions = false;
        bool abandon_builtin = false;
        uint32_t selected_exclusions = 0;
        size_t selected_builtin_positions[5] = {0, 0, 0, 0, 0};
        for (uint32_t exclusions = 0; exclusions <= KNOWN_EXCLUSIONS;
             ++exclusions) {
            bool candidate_is_proven = true;
            size_t candidate_builtin_positions[5] = {0, 0, 0, 0, 0};
            for (size_t stage_index = 0;
                 stage_index < ARRAY_COUNT(temporary.stages); ++stage_index) {
                ShaderLabPassStageVariantPlan* original_stage =
                    &temporary.stages[stage_index];
                if (!original_stage->active) continue;
                BuiltinProjection projection;
                ShaderLabVariantPlanDiagnostic local;
                set_diagnostic(&local, SHADERLAB_VARIANT_PLAN_OK,
                               (int)stage_index, -1, -1, -1);
                if (!stage_project_builtin_subset(
                        &temporary, (int)stage_index, selected_family,
                        &projection, &local)) {
                    if (local.status ==
                        SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED) {
                        *active_diagnostic = local;
                        goto fail;
                    }
                    candidate_is_proven = false;
                    break;
                }
                ShaderLabBuiltinVariantDomain domain;
                const bool survives = shaderlab_builtin_variant_domain_get(
                    selected_family,
                    (UnitySerializedProgramStage)stage_index, &domain) &&
                    projection_survives_exclusions(
                        &domain, &projection, exclusions);
                size_t external_product_count = 1;
                bool exact_cardinality = survives;
                for (size_t axis_index = 0;
                     exact_cardinality &&
                     axis_index < original_stage->axis_count;
                     ++axis_index) {
                    const ShaderLabVariantAxis* axis =
                        &original_stage->axes[axis_index];
                    const size_t radix =
                        axis->keyword_count +
                        (axis->has_default ? 1u : 0u);
                    if (radix == 0 ||
                        external_product_count > SIZE_MAX / radix) {
                        exact_cardinality = false;
                    } else {
                        external_product_count *= radix;
                    }
                }
                const size_t builtin_count =
                    survives ? included_builtin_count(&domain, exclusions) : 0;
                if (builtin_count == 0 ||
                    external_product_count > SIZE_MAX / builtin_count ||
                    external_product_count * builtin_count !=
                        original_stage->state_count) {
                    exact_cardinality = false;
                }
                OwnedStateList generated_external;
                memset(&generated_external, 0, sizeof(generated_external));
                bool proven = exact_cardinality && build_axis_state_list(
                    original_stage, &generated_external, &local,
                    (int)stage_index);
                if (proven) {
                    proven = false;
                    for (size_t position = 0;
                         position <= original_stage->axis_count; ++position) {
                        ShaderLabPassStageVariantPlan scratch =
                            *original_stage;
                        scratch.builtin_axis_position = position;
                        scratch.generated_state_count = 0;
                        scratch.generated_states = NULL;
                        scratch.generated_aliases = NULL;
                        bool position_proven =
                            build_generated_builtin_domain(
                                &temporary, (int)stage_index,
                                &generated_external, exclusions, &scratch,
                                &local) &&
                            prove_generated_aliases(
                                &temporary, (int)stage_index, &scratch,
                                exclusions, &local);
                        clear_generated_domain(&scratch);
                        if (position_proven) {
                            candidate_builtin_positions[stage_index] =
                                position;
                            proven = true;
                            break;
                        }
                        if (local.status ==
                                SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED ||
                            local.status ==
                                SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED) {
                            break;
                        }
                    }
                }
                free_state_list(&generated_external);
                builtin_projection_free(&projection);
                if (!proven) {
                    if (local.status ==
                        SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED) {
                        *active_diagnostic = local;
                        goto fail;
                    }
                    if (local.status ==
                        SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED) {
                        abandon_builtin = true;
                    }
                    candidate_is_proven = false;
                    break;
                }
            }
            if (abandon_builtin) break;
            if (!candidate_is_proven) continue;
            if (!found_exclusions ||
                exclusion_bit_count(exclusions) <
                    exclusion_bit_count(selected_exclusions) ||
                (exclusion_bit_count(exclusions) ==
                     exclusion_bit_count(selected_exclusions) &&
                exclusions < selected_exclusions)) {
                found_exclusions = true;
                selected_exclusions = exclusions;
                memcpy(selected_builtin_positions,
                       candidate_builtin_positions,
                       sizeof(selected_builtin_positions));
            }
        }
        if (!found_exclusions) {
            /* Some Release stripping patterns are not representable by the
             * compiler's six no* filters, and a sparse built-in superset can
             * refine the runtime selector incorrectly.  The complete Boolean
             * source domain is the exact fallback.  Planning keeps its 2^K
             * mapping implicit; bounded stage emission materializes only a
             * subtree-pruned literal decision tree. */
            temporary.has_builtin = false;
            temporary.builtin_family = SHADERLAB_BUILTIN_VARIANT_INVALID;
            temporary.builtin_exclusions = 0;
            for (size_t stage_index = 0;
                 stage_index < ARRAY_COUNT(temporary.stages); ++stage_index) {
                ShaderLabPassStageVariantPlan* stage =
                    &temporary.stages[stage_index];
                if (!stage->active) continue;
                if (!build_full_boolean_alias_domain(
                        shader, stage, active_diagnostic,
                        (int)stage_index) ||
                    !prove_generated_aliases(
                        &temporary, (int)stage_index, stage,
                        0, active_diagnostic))
                    goto fail;
            }
        } else {
            temporary.builtin_exclusions = selected_exclusions;
            for (size_t stage_index = 0;
                 stage_index < ARRAY_COUNT(temporary.stages); ++stage_index) {
                ShaderLabPassStageVariantPlan* stage =
                    &temporary.stages[stage_index];
                if (!stage->active) continue;
                stage->builtin_axis_position =
                    selected_builtin_positions[stage_index];
                OwnedStateList generated_external;
                memset(&generated_external, 0, sizeof(generated_external));
                if (!build_axis_state_list(
                        stage, &generated_external, active_diagnostic,
                        (int)stage_index) ||
                    !build_generated_builtin_domain(
                        &temporary, (int)stage_index, &generated_external,
                        selected_exclusions, stage, active_diagnostic)) {
                    free_state_list(&generated_external);
                    goto fail;
                }
                free_state_list(&generated_external);
                if (!prove_generated_aliases(
                        &temporary, (int)stage_index, stage,
                        selected_exclusions, active_diagnostic))
                    goto fail;
            }
        }
    } else {
        for (size_t stage_index = 0;
             stage_index < ARRAY_COUNT(temporary.stages); ++stage_index) {
            ShaderLabPassStageVariantPlan* stage = &temporary.stages[stage_index];
            if (!stage->active) continue;
            if (!prove_generated_aliases(
                    &temporary, (int)stage_index, stage,
                    0, active_diagnostic)) goto fail;
        }
    }

    temporary.external_axes_are_shared = true;
    const ShaderLabPassStageVariantPlan* first_active = NULL;
    for (size_t stage_index = 0; stage_index < ARRAY_COUNT(temporary.stages);
         ++stage_index) {
        const ShaderLabPassStageVariantPlan* stage =
            &temporary.stages[stage_index];
        if (!stage->active) continue;
        if (!first_active) first_active = stage;
        else if (!axes_equal(first_active, stage)) {
            temporary.external_axes_are_shared = false;
            break;
        }
    }

    shaderlab_variant_plan_free(out_plan);
    *out_plan = temporary;
    set_diagnostic(active_diagnostic, SHADERLAB_VARIANT_PLAN_OK,
                   -1, -1, -1, -1);
    return SHADERLAB_VARIANT_PLAN_OK;

fail: {
        const ShaderLabVariantPlanStatus status = active_diagnostic->status;
        shaderlab_variant_plan_free(&temporary);
        return status;
    }
}

static const char* exclusion_name(uint32_t bit) {
    switch (bit) {
    case SHADERLAB_BUILTIN_EXCLUDE_SHADOWS: return "noshadow";
    case SHADERLAB_BUILTIN_EXCLUDE_LIGHTMAP: return "nolightmap";
    case SHADERLAB_BUILTIN_EXCLUDE_DIR_LIGHTMAP: return "nodirlightmap";
    case SHADERLAB_BUILTIN_EXCLUDE_DYN_LIGHTMAP: return "nodynlightmap";
    case SHADERLAB_BUILTIN_EXCLUDE_SHADOWMASK: return "noshadowmask";
    case SHADERLAB_BUILTIN_EXCLUDE_VERTEX_LIGHT: return "novertexlight";
    }
    return NULL;
}

static const char* stage_suffix(size_t stage_index) {
    static const char* const suffixes[] = {
        "vertex", "fragment", "geometry", "hull", "domain"};
    return stage_index < ARRAY_COUNT(suffixes) ? suffixes[stage_index] : NULL;
}

static bool emit_axis(const ShaderLabVariantPlan* plan,
                      const ShaderLabVariantAxis* axis,
                      const char* suffix,
                      StringBuilder* output,
                      int indent) {
    if (!axis || axis->keyword_count == 0) return false;
    for (int i = 0; i < indent; ++i) sb_append(output, "    ");
    sb_append(output, "#pragma multi_compile");
    if (axis->is_local) sb_append(output, "_local");
    if (suffix) {
        sb_append_char(output, '_');
        sb_append(output, suffix);
    }
    if (axis->has_default) sb_append(output, " __");
    for (size_t i = 0; i < axis->keyword_count; ++i) {
        const uint16_t raw = axis->keyword_indices[i];
        if ((size_t)raw >= (size_t)plan->shader->keyword_names.count)
            return false;
        sb_append_char(output, ' ');
        sb_append(output, plan->shader->keyword_names.keywords[raw]);
    }
    sb_append_char(output, '\n');
    return sb_ok(output);
}

static const char* emitted_axis_keyword_name(
    const ShaderLabVariantPlan* plan, const ShaderLabVariantAxis* axis,
    size_t keyword_index) {
    if (!plan || !plan->shader || !axis || !axis->keyword_indices ||
        keyword_index >= axis->keyword_count) return NULL;
    const uint16_t raw = axis->keyword_indices[keyword_index];
    if ((size_t)raw >= (size_t)plan->shader->keyword_names.count ||
        !plan->shader->keyword_names.keywords) return NULL;
    return plan->shader->keyword_names.keywords[raw];
}

static bool emitted_axis_is_valid(const ShaderLabVariantPlan* plan,
                                  const ShaderLabVariantAxis* axis) {
    if (!axis || axis->keyword_count == 0 || !axis->keyword_indices)
        return false;
    for (size_t keyword = 0; keyword < axis->keyword_count; ++keyword) {
        const char* name = emitted_axis_keyword_name(plan, axis, keyword);
        if (!keyword_is_identifier(name)) return false;
        for (size_t previous = 0; previous < keyword; ++previous) {
            const char* previous_name =
                emitted_axis_keyword_name(plan, axis, previous);
            if (!previous_name || strcmp(name, previous_name) == 0)
                return false;
        }
    }
    return true;
}

static bool emitted_axis_equal(const ShaderLabVariantPlan* plan,
                               const ShaderLabVariantAxis* left,
                               const ShaderLabVariantAxis* right) {
    if (!emitted_axis_is_valid(plan, left) ||
        !emitted_axis_is_valid(plan, right) ||
        left->has_default != right->has_default ||
        left->is_local != right->is_local ||
        left->keyword_count != right->keyword_count) return false;
    for (size_t keyword = 0; keyword < left->keyword_count; ++keyword) {
        if (strcmp(emitted_axis_keyword_name(plan, left, keyword),
                   emitted_axis_keyword_name(plan, right, keyword)) != 0) {
            return false;
        }
    }
    return true;
}

static bool emitted_axes_overlap(const ShaderLabVariantPlan* plan,
                                 const ShaderLabVariantAxis* left,
                                 const ShaderLabVariantAxis* right) {
    if (!emitted_axis_is_valid(plan, left) ||
        !emitted_axis_is_valid(plan, right)) return false;
    for (size_t left_index = 0; left_index < left->keyword_count;
         ++left_index) {
        const char* left_name =
            emitted_axis_keyword_name(plan, left, left_index);
        for (size_t right_index = 0; right_index < right->keyword_count;
             ++right_index) {
            if (strcmp(left_name,
                       emitted_axis_keyword_name(plan, right, right_index)) ==
                0) return true;
        }
    }
    return false;
}

/* A keyword axis shared by multiple linked stages cannot remain independently
 * stage-scoped: Unity may otherwise pair variants with incompatible stage
 * interfaces. Exact shared axes are hoisted only when every active stage owns
 * the same emitted directive. Partial sharing, partition conflicts, scope or
 * default conflicts, duplicate axes, and opposite sides of a built-in pragma
 * fail closed. */
static bool validate_emitted_axis_coherence(
    const ShaderLabVariantPlan* plan) {
    if (!plan || !plan->shader || plan->shader->keyword_names.count < 0 ||
        (plan->shader->keyword_names.count > 0 &&
         !plan->shader->keyword_names.keywords)) return false;
    size_t active_stage_count = 0;
    for (size_t stage_index = 0;
         stage_index < ARRAY_COUNT(plan->stages); ++stage_index) {
        const ShaderLabPassStageVariantPlan* stage =
            &plan->stages[stage_index];
        if (!stage->active) continue;
        ++active_stage_count;
        if ((stage->axis_count > 0 && !stage->axes) ||
            stage->builtin_axis_position > stage->axis_count) return false;
        for (size_t axis_index = 0; axis_index < stage->axis_count;
             ++axis_index) {
            if (!emitted_axis_is_valid(plan, &stage->axes[axis_index]))
                return false;
            for (size_t later = axis_index + 1u;
                 later < stage->axis_count; ++later) {
                if (emitted_axes_overlap(plan, &stage->axes[axis_index],
                                         &stage->axes[later])) return false;
            }
        }
    }
    if (active_stage_count == 0) return false;

    for (size_t stage_index = 0;
         stage_index < ARRAY_COUNT(plan->stages); ++stage_index) {
        const ShaderLabPassStageVariantPlan* stage =
            &plan->stages[stage_index];
        if (!stage->active) continue;
        for (size_t axis_index = 0; axis_index < stage->axis_count;
             ++axis_index) {
            const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
            size_t matching_stage_count = 0;
            for (size_t other_stage_index = 0;
                 other_stage_index < ARRAY_COUNT(plan->stages);
                 ++other_stage_index) {
                const ShaderLabPassStageVariantPlan* other_stage =
                    &plan->stages[other_stage_index];
                if (!other_stage->active) continue;
                size_t exact_match_count = 0;
                for (size_t other_axis_index = 0;
                     other_axis_index < other_stage->axis_count;
                     ++other_axis_index) {
                    const ShaderLabVariantAxis* other_axis =
                        &other_stage->axes[other_axis_index];
                    if (!emitted_axes_overlap(plan, axis, other_axis))
                        continue;
                    if (!emitted_axis_equal(plan, axis, other_axis))
                        return false;
                    ++exact_match_count;
                    if (plan->has_builtin &&
                        (axis_index < stage->builtin_axis_position) !=
                            (other_axis_index <
                             other_stage->builtin_axis_position)) {
                        return false;
                    }
                }
                if (exact_match_count > 1u) return false;
                if (exact_match_count == 1u) ++matching_stage_count;
            }
            if (matching_stage_count > 1u &&
                matching_stage_count < active_stage_count) return false;
        }
    }
    return true;
}

static bool emitted_axis_is_common(const ShaderLabVariantPlan* plan,
                                   const ShaderLabVariantAxis* axis) {
    for (size_t stage_index = 0;
         stage_index < ARRAY_COUNT(plan->stages); ++stage_index) {
        const ShaderLabPassStageVariantPlan* stage =
            &plan->stages[stage_index];
        if (!stage->active) continue;
        bool found = false;
        for (size_t axis_index = 0; axis_index < stage->axis_count;
             ++axis_index) {
            if (emitted_axis_equal(plan, axis,
                                   &stage->axes[axis_index])) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static const ShaderLabVariantAxis* matching_stage_axis_in_range(
    const ShaderLabVariantPlan* plan,
    size_t stage_index,
    const ShaderLabVariantAxis* emitted_axis,
    size_t begin,
    size_t end) {
    if (!plan || stage_index >= ARRAY_COUNT(plan->stages) ||
        !emitted_axis) return NULL;
    const ShaderLabPassStageVariantPlan* stage =
        &plan->stages[stage_index];
    if (!stage->active || begin > end || end > stage->axis_count)
        return NULL;
    const ShaderLabVariantAxis* match = NULL;
    for (size_t axis_index = begin; axis_index < end; ++axis_index) {
        const ShaderLabVariantAxis* candidate = &stage->axes[axis_index];
        if (!emitted_axis_equal(plan, emitted_axis, candidate)) continue;
        if (match) return NULL;
        match = candidate;
    }
    return match;
}

static bool visit_emitted_stage_axis_range(
    const ShaderLabVariantPlan* plan,
    size_t first_stage_index,
    size_t stage_index,
    size_t first_begin,
    size_t first_end,
    size_t stage_begin,
    size_t stage_end,
    ShaderLabEmittedAxisVisitor visitor,
    void* context) {
    const ShaderLabPassStageVariantPlan* first =
        &plan->stages[first_stage_index];
    const ShaderLabPassStageVariantPlan* stage =
        &plan->stages[stage_index];
    if (first_begin > first_end || first_end > first->axis_count ||
        stage_begin > stage_end || stage_end > stage->axis_count) {
        return false;
    }

    /* Unsuffixed common directives are emitted in first-active-stage order.
     * Report the target stage's matching axis so callers retain its exact raw
     * keyword indices even when equal names have stage-owned storage. */
    for (size_t axis_index = first_begin; axis_index < first_end;
         ++axis_index) {
        const ShaderLabVariantAxis* axis = &first->axes[axis_index];
        if (!emitted_axis_is_common(plan, axis)) continue;
        const ShaderLabVariantAxis* target = matching_stage_axis_in_range(
            plan, stage_index, axis, stage_begin, stage_end);
        if (!target || !visitor(target, context)) return false;
    }

    /* Stage-only directives keep their original relative order. */
    for (size_t axis_index = stage_begin; axis_index < stage_end;
         ++axis_index) {
        const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
        if (emitted_axis_is_common(plan, axis)) continue;
        if (!visitor(axis, context)) return false;
    }
    return true;
}

bool shaderlab_variant_plan_visit_emitted_stage_axes(
    const ShaderLabVariantPlan* plan,
    size_t stage_index,
    ShaderLabEmittedAxisVisitor visitor,
    void* context) {
    if (!plan || !visitor || stage_index >= ARRAY_COUNT(plan->stages) ||
        !plan->stages[stage_index].active ||
        !validate_emitted_axis_coherence(plan)) {
        return false;
    }

    size_t first_stage = 0U;
    while (first_stage < ARRAY_COUNT(plan->stages) &&
           !plan->stages[first_stage].active) {
        ++first_stage;
    }
    if (first_stage == ARRAY_COUNT(plan->stages)) return false;

    const ShaderLabPassStageVariantPlan* first =
        &plan->stages[first_stage];
    const ShaderLabPassStageVariantPlan* stage =
        &plan->stages[stage_index];
    if (!plan->has_builtin) {
        return visit_emitted_stage_axis_range(
            plan, first_stage, stage_index, 0U, first->axis_count,
            0U, stage->axis_count, visitor, context);
    }

    if (!visit_emitted_stage_axis_range(
            plan, first_stage, stage_index,
            0U, first->builtin_axis_position,
            0U, stage->builtin_axis_position, visitor, context)) {
        return false;
    }
    return visit_emitted_stage_axis_range(
        plan, first_stage, stage_index,
        first->builtin_axis_position, first->axis_count,
        stage->builtin_axis_position, stage->axis_count, visitor, context);
}

/* Pragma axes form a complete Cartesian assignment product and generated
 * bodies select solely through keyword predicates, never a variant ordinal.
 * Therefore exact axes common to every linked stage may be emitted once in a
 * deterministic order without changing the macro-assignment set. Stage-only
 * axes retain their original relative order. */
static bool emit_synchronized_axis_ranges(
    const ShaderLabVariantPlan* plan, const size_t begin[5],
    const size_t end[5], StringBuilder* output, int indent) {
    size_t first_stage = ARRAY_COUNT(plan->stages);
    for (size_t stage_index = 0;
         stage_index < ARRAY_COUNT(plan->stages); ++stage_index) {
        if (plan->stages[stage_index].active &&
            first_stage == ARRAY_COUNT(plan->stages)) {
            first_stage = stage_index;
        }
    }
    if (first_stage == ARRAY_COUNT(plan->stages)) return false;

    const ShaderLabPassStageVariantPlan* first = &plan->stages[first_stage];
    for (size_t axis_index = begin[first_stage];
         axis_index < end[first_stage]; ++axis_index) {
        const ShaderLabVariantAxis* axis = &first->axes[axis_index];
        if (emitted_axis_is_common(plan, axis) &&
            !emit_axis(plan, axis, NULL, output, indent)) return false;
    }

    for (size_t stage_index = 0;
         stage_index < ARRAY_COUNT(plan->stages); ++stage_index) {
        const ShaderLabPassStageVariantPlan* stage =
            &plan->stages[stage_index];
        if (!stage->active) continue;
        const char* suffix = stage_suffix(stage_index);
        if (!suffix) return false;
        for (size_t axis_index = begin[stage_index];
             axis_index < end[stage_index]; ++axis_index) {
            if (emitted_axis_is_common(plan, &stage->axes[axis_index]))
                continue;
            if (!emit_axis(plan, &stage->axes[axis_index], suffix,
                           output, indent)) return false;
        }
    }
    return sb_ok(output);
}

static bool emit_builtin_pragma(const ShaderLabVariantPlan* plan,
                                StringBuilder* output,
                                int indent) {
    ShaderLabBuiltinVariantDomain domain;
    size_t first_stage = 0;
    while (first_stage < ARRAY_COUNT(plan->stages) &&
           !plan->stages[first_stage].active) ++first_stage;
    if (first_stage == ARRAY_COUNT(plan->stages) ||
        !shaderlab_builtin_variant_domain_get(
            plan->builtin_family,
            (UnitySerializedProgramStage)first_stage, &domain)) return false;
    for (int i = 0; i < indent; ++i) sb_append(output, "    ");
    sb_append(output, "#pragma ");
    sb_append(output, domain.directive);
    for (uint32_t bit = 1; bit <= SHADERLAB_BUILTIN_EXCLUDE_VERTEX_LIGHT;
         bit <<= 1u) {
        if ((plan->builtin_exclusions & bit) == 0) continue;
        const char* name = exclusion_name(bit);
        if (!name) return false;
        sb_append_char(output, ' ');
        sb_append(output, name);
    }
    sb_append_char(output, '\n');
    return sb_ok(output);
}

bool shaderlab_variant_plan_emit_pragmas(
    const ShaderLabVariantPlan* plan,
    StringBuilder* output,
    int indent) {
    if (!plan || !plan->shader || !plan->pass || !output || indent < 0)
        return false;
    if (!validate_emitted_axis_coherence(plan)) return false;
    if (plan->uses_specific_hardware_tiers) {
        for (int i = 0; i < indent; ++i) sb_append(output, "    ");
        sb_append(output, "#pragma hardware_tier_variants d3d11\n");
    }
    if (plan->has_builtin) {
        size_t begin[5] = {0, 0, 0, 0, 0};
        size_t end[5] = {0, 0, 0, 0, 0};
        for (size_t stage_index = 0; stage_index < ARRAY_COUNT(plan->stages);
             ++stage_index) {
            const ShaderLabPassStageVariantPlan* stage =
                &plan->stages[stage_index];
            if (!stage->active) continue;
            if (stage->builtin_axis_position > stage->axis_count)
                return false;
            end[stage_index] = stage->builtin_axis_position;
        }
        if (!emit_synchronized_axis_ranges(
                plan, begin, end, output, indent)) return false;
        if (!emit_builtin_pragma(plan, output, indent)) return false;
        for (size_t stage_index = 0; stage_index < ARRAY_COUNT(plan->stages);
             ++stage_index) {
            const ShaderLabPassStageVariantPlan* stage =
                &plan->stages[stage_index];
            if (!stage->active) continue;
            begin[stage_index] = stage->builtin_axis_position;
            end[stage_index] = stage->axis_count;
        }
        if (!emit_synchronized_axis_ranges(
                plan, begin, end, output, indent)) return false;
    } else {
        size_t begin[5] = {0, 0, 0, 0, 0};
        size_t end[5] = {0, 0, 0, 0, 0};
        for (size_t stage_index = 0; stage_index < ARRAY_COUNT(plan->stages);
             ++stage_index) {
            const ShaderLabPassStageVariantPlan* stage =
                &plan->stages[stage_index];
            if (stage->active) end[stage_index] = stage->axis_count;
        }
        if (!emit_synchronized_axis_ranges(
                plan, begin, end, output, indent)) return false;
    }
    return sb_ok(output);
}

#undef ARRAY_COUNT
