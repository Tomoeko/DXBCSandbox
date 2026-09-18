#include "translation/shaderlab_variant_plan.h"
#include "translation/shaderlab_emitter_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

typedef struct {
    SerializedShader shader;
    SerializedPass pass;
    SerializedSubProgram* subprograms;
    SerializedSubProgramIdentity* identities;
    int platform;
} Fixture;

typedef struct {
    uint16_t indices[16];
    size_t count;
} EmittedAxisOrder;

static bool collect_single_keyword_axis(
    const ShaderLabVariantAxis* axis, void* opaque) {
    EmittedAxisOrder* order = (EmittedAxisOrder*)opaque;
    if (!axis || !order || axis->keyword_count != 1U ||
        !axis->keyword_indices ||
        order->count >= sizeof(order->indices) / sizeof(order->indices[0])) {
        return false;
    }
    order->indices[order->count++] = axis->keyword_indices[0];
    return true;
}

static void fixture_init(Fixture* fixture,
                         char** keyword_names,
                         uint8_t* keyword_flags,
                         int keyword_count,
                         int pass_type,
                         size_t variant_count) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->shader.keyword_names.count = keyword_count;
    fixture->shader.keyword_names.keywords = keyword_names;
    fixture->shader.keyword_flags = keyword_flags;
    fixture->platform = 4;
    fixture->pass.pass_type = pass_type;
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = &fixture->platform;
    fixture->pass.subprogram_count[0] = (int)variant_count;
    fixture->subprograms = (SerializedSubProgram*)calloc(
        variant_count, sizeof(*fixture->subprograms));
    fixture->identities = (SerializedSubProgramIdentity*)calloc(
        variant_count, sizeof(*fixture->identities));
    fixture->pass.subprograms[0] = fixture->subprograms;
    fixture->pass.subprogram_identities[0] = fixture->identities;
    for (size_t i = 0; i < variant_count; ++i) {
        fixture->subprograms[i].program_type = 15;
        fixture->identities[i].hardware_tier_group = 3;
        fixture->identities[i].keyword_scopes_are_explicit = false;
    }
}

static void fixture_set_mask(Fixture* fixture,
                             uint16_t* mask,
                             int count) {
    fixture->pass.serialized_keyword_state_mask = mask;
    fixture->pass.serialized_keyword_state_mask_count = count;
}

static void fixture_set_legacy_state(Fixture* fixture,
                                     size_t variant_index,
                                     const uint16_t* indices,
                                     size_t count) {
    int* raw = NULL;
    if (count != 0) {
        raw = (int*)calloc(count, sizeof(*raw));
        for (size_t i = 0; i < count; ++i) raw[i] = indices[i];
    }
    fixture->identities[variant_index].local_keyword_indices = raw;
    fixture->identities[variant_index].local_keyword_index_count = (int)count;
}

static void fixture_free(Fixture* fixture) {
    if (fixture->identities) {
        for (int i = 0; i < fixture->pass.subprogram_count[0]; ++i) {
            free(fixture->identities[i].global_keyword_indices);
            free(fixture->identities[i].local_keyword_indices);
        }
    }
    free(fixture->identities);
    free(fixture->subprograms);
    memset(fixture, 0, sizeof(*fixture));
}

static int test_sparse_shape_and_legacy_combined_authority(void) {
    ShaderLabBuiltinVariantDomain domain;
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    const size_t keyword_count = domain.keyword_count + 1u;
    char** names = (char**)calloc(keyword_count, sizeof(*names));
    uint8_t* flags = (uint8_t*)calloc(keyword_count, sizeof(*flags));
    uint16_t* pass_mask =
        (uint16_t*)calloc(keyword_count, sizeof(*pass_mask));
    CHECK(names != NULL && flags != NULL && pass_mask != NULL);
    for (size_t i = 0; i < domain.keyword_count; ++i) {
        names[i] = (char*)domain.keyword_names[i];
        pass_mask[i] = (uint16_t)i;
    }
    names[domain.keyword_count] = "FOG_LINEAR";
    pass_mask[domain.keyword_count] = (uint16_t)domain.keyword_count;

    static const uint16_t sparse_vertex_masks[] = {
        0x001, 0x081, 0x00d, 0x007, 0x087, 0x00f,
        0x011, 0x091, 0x01d, 0x017, 0x097, 0x01f,
        0x101, 0x181, 0x10d, 0x111, 0x191, 0x11d};
    Fixture fixture;
    /* The sparse regression serializes every pass with m_Type == 0.
     * Recognition must be driven by the ordered keyword domain, never this
     * field. */
    fixture_init(&fixture, names, flags, (int)keyword_count, 0,
                 (sizeof(sparse_vertex_masks) /
                  sizeof(sparse_vertex_masks[0])) * 2u);
    fixture_set_mask(&fixture, pass_mask, (int)keyword_count);
    size_t variant_index = 0;
    for (size_t fog = 0; fog < 2; ++fog) {
        for (size_t row = 0;
             row < sizeof(sparse_vertex_masks) /
                       sizeof(sparse_vertex_masks[0]);
             ++row) {
            uint16_t state[17];
            size_t state_count = 0;
            for (size_t bit = 0; bit < domain.keyword_count; ++bit) {
                if ((sparse_vertex_masks[row] &
                     (uint16_t)(UINT16_C(1) << bit)) != 0) {
                    state[state_count++] = (uint16_t)bit;
                }
            }
            if (fog != 0) state[state_count++] = (uint16_t)domain.keyword_count;
            fixture_set_legacy_state(&fixture, variant_index++, state,
                                     state_count);
        }
    }

    ShaderLabVariantPlan plan;
    ShaderLabVariantPlanDiagnostic diagnostic;
    shaderlab_variant_plan_init(&plan);
    const ShaderLabVariantPlanStatus build_status =
        shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                     &diagnostic);
    if (build_status != SHADERLAB_VARIANT_PLAN_OK) {
        fprintf(stderr, "Sparse plan failed: %s stage=%d raw=%d\n",
                shaderlab_variant_plan_status_name(build_status),
                diagnostic.stage_index, diagnostic.raw_keyword_index);
    }
    CHECK(build_status == SHADERLAB_VARIANT_PLAN_OK);
    CHECK(!plan.has_builtin);
    CHECK(plan.builtin_family == SHADERLAB_BUILTIN_VARIANT_INVALID);
    CHECK(plan.stages[0].state_count == 36);
    CHECK(plan.stages[0].axis_count == 8);
    CHECK(plan.stages[0].generated_state_count == 256);
    CHECK(plan.stages[0].generated_domain_is_symbolic_boolean);
    CHECK(plan.stages[0].generated_states == NULL);
    CHECK(plan.stages[0].generated_aliases == NULL);
    for (size_t i = 0; i < plan.stages[0].axis_count; ++i) {
        CHECK(plan.stages[0].axes[i].has_default);
        CHECK(plan.stages[0].axes[i].keyword_count == 1);
    }

    StringBuilder output;
    sb_init(&output);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &output, 0));
    CHECK(strstr(output.buf, "multi_compile_fwdbase") == NULL);
    CHECK(strstr(output.buf,
                 "#pragma multi_compile __ FOG_LINEAR\n") != NULL);
    sb_free(&output);
    shaderlab_variant_plan_free(&plan);
    fixture_free(&fixture);
    free(pass_mask);
    free(flags);
    free(names);
    return 0;
}

static int test_shadowcollector_pinned_domain_without_pass_type(void) {
    char* names[] = {"SHADOWS_SPLIT_SPHERES",
                     "SHADOWS_SINGLE_CASCADE"};
    uint8_t flags[] = {0, 0};
    uint16_t pass_mask[] = {0, 1};
    const uint16_t split[] = {0};
    const uint16_t single[] = {1};
    const uint16_t both[] = {0, 1};
    const uint16_t* states[] = {NULL, split, single, both};
    const size_t counts[] = {0, 1, 1, 2};
    Fixture fixture;
    fixture_init(&fixture, names, flags, 2, INT_MIN, 4);
    fixture_set_mask(&fixture, pass_mask, 2);
    for (size_t i = 0; i < 4; ++i)
        fixture_set_legacy_state(&fixture, i, states[i], counts[i]);

    ShaderLabVariantPlan plan;
    ShaderLabVariantPlanDiagnostic diagnostic;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_OK);
    CHECK(plan.has_builtin);
    CHECK(plan.builtin_family ==
          SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR);
    CHECK(plan.stages[0].generated_state_count == 4);
    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strcmp(pragmas.buf,
                 "#pragma multi_compile_shadowcollector\n") == 0);
    sb_free(&pragmas);
    shaderlab_variant_plan_free(&plan);
    fixture_free(&fixture);
    return 0;
}

static int build_generic(char** names,
                         uint8_t* flags,
                         int keyword_count,
                         const uint16_t* mask,
                         const uint16_t* const* states,
                         const size_t* state_counts,
                         size_t state_count,
                         ShaderLabVariantPlanStatus expected_status,
                         ShaderLabVariantPlan* out_plan) {
    Fixture fixture;
    fixture_init(&fixture, names, flags, keyword_count, 0, state_count);
    fixture_set_mask(&fixture, (uint16_t*)mask, keyword_count);
    for (size_t i = 0; i < state_count; ++i)
        fixture_set_legacy_state(&fixture, i, states[i], state_counts[i]);
    ShaderLabVariantPlanDiagnostic diagnostic;
    const ShaderLabVariantPlanStatus status = shaderlab_variant_plan_build(
        &fixture.shader, &fixture.pass, out_plan, &diagnostic);
    CHECK(status == expected_status);
    CHECK(diagnostic.status == expected_status);
    fixture_free(&fixture);
    return 0;
}

static int test_sparse_and_reordered_domains_use_proven_boolean_fallback(void) {
    char* names[] = {"A", "B", "C"};
    uint8_t flags[] = {0, 0, 0};
    const uint16_t mask[] = {0, 1, 2};
    const uint16_t a[] = {0};
    const uint16_t b[] = {1};
    const uint16_t c[] = {2};
    const uint16_t ac[] = {0, 2};
    const uint16_t* nonfactorable[] = {NULL, a, b, c, ac};
    const size_t nonfactorable_counts[] = {0, 1, 1, 1, 2};
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    CHECK(build_generic(names, flags, 3, mask, nonfactorable,
                        nonfactorable_counts, 5,
                        SHADERLAB_VARIANT_PLAN_OK, &plan) == 0);
    CHECK(plan.stages[0].axis_count == 3);
    CHECK(plan.stages[0].generated_state_count == 8);
    CHECK(plan.stages[0].generated_domain_is_symbolic_boolean);
    CHECK(plan.stages[0].generated_states == NULL);
    CHECK(plan.stages[0].generated_aliases == NULL);
    shaderlab_variant_plan_free(&plan);
    shaderlab_variant_plan_init(&plan);

    char* two_names[] = {"A", "B"};
    uint8_t two_flags[] = {0, 0};
    const uint16_t two_mask[] = {0, 1};
    const uint16_t ab[] = {0, 1};
    const uint16_t* gray_order[] = {NULL, a, ab, b};
    const size_t gray_counts[] = {0, 1, 2, 1};
    CHECK(build_generic(two_names, two_flags, 2, two_mask, gray_order,
                        gray_counts, 4,
                        SHADERLAB_VARIANT_PLAN_OK, &plan) == 0);
    CHECK(plan.stages[0].axis_count == 2);
    CHECK(plan.stages[0].generated_state_count == 4);
    CHECK(plan.stages[0].generated_domain_is_symbolic_boolean);
    CHECK(plan.stages[0].generated_states == NULL);
    CHECK(plan.stages[0].generated_aliases == NULL);
    shaderlab_variant_plan_free(&plan);
    return 0;
}

static int test_raw_index_and_scope_failures(void) {
    char* names[] = {"A", "B"};
    uint8_t flags[] = {0, 0};
    uint16_t mask[] = {0, 1};
    Fixture fixture;
    fixture_init(&fixture, names, flags, 2, 0, 1);
    fixture_set_mask(&fixture, mask, 2);
    int noncanonical[] = {1, 0};
    fixture.identities[0].keyword_scopes_are_explicit = true;
    fixture.identities[0].global_keyword_indices = noncanonical;
    fixture.identities[0].global_keyword_index_count = 2;
    ShaderLabVariantPlan plan;
    ShaderLabVariantPlanDiagnostic diagnostic;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_NONCANONICAL_RAW_INDICES);
    fixture.identities[0].global_keyword_indices = NULL;
    fixture.identities[0].global_keyword_index_count = 0;
    fixture_free(&fixture);

    uint8_t local_flag[] = {1};
    char* one_name[] = {"LOCAL_A"};
    uint16_t one_mask[] = {0};
    fixture_init(&fixture, one_name, local_flag, 1, 0, 1);
    fixture_set_mask(&fixture, one_mask, 1);
    int wrong_scope[] = {0};
    fixture.identities[0].keyword_scopes_are_explicit = true;
    fixture.identities[0].global_keyword_indices = wrong_scope;
    fixture.identities[0].global_keyword_index_count = 1;
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_SCOPE_CONFLICT);
    fixture.identities[0].global_keyword_indices = NULL;
    fixture.identities[0].global_keyword_index_count = 0;
    fixture_free(&fixture);

    fixture_init(&fixture, one_name, local_flag, 1, 0, 1);
    fixture_set_mask(&fixture, one_mask, 1);
    const uint16_t out_of_range[] = {1};
    fixture_set_legacy_state(&fixture, 0, out_of_range, 1);
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_INVALID_RAW_INDEX);
    fixture_free(&fixture);
    shaderlab_variant_plan_free(&plan);
    return 0;
}

static int test_pass_authority_and_scope_factor_failures(void) {
    char* names[] = {"A", "B"};
    uint8_t flags[] = {0, 0};
    uint16_t reversed_mask[] = {1, 0};
    Fixture fixture;
    fixture_init(&fixture, names, flags, 2, 0, 1);
    fixture_set_mask(&fixture, reversed_mask, 2);
    fixture_set_legacy_state(&fixture, 0, NULL, 0);
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(
              &fixture.shader, &fixture.pass, &plan, NULL) ==
          SHADERLAB_VARIANT_PLAN_INVALID_PASS_MASK);
    fixture_free(&fixture);

    uint16_t one_mask[] = {0};
    const uint16_t outside[] = {1};
    fixture_init(&fixture, names, flags, 2, 0, 1);
    fixture_set_mask(&fixture, one_mask, 1);
    fixture_set_legacy_state(&fixture, 0, outside, 1);
    ShaderLabVariantPlanDiagnostic diagnostic;
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_CANDIDATE_OUTSIDE_PASS_MASK);
    CHECK(diagnostic.raw_keyword_index == 1);
    fixture_free(&fixture);

    uint16_t full_mask[] = {0, 1};
    const uint16_t a[] = {0};
    const uint16_t b[] = {1};
    fixture_init(&fixture, names, NULL, 2, 0, 1);
    fixture_set_mask(&fixture, full_mask, 2);
    fixture_set_legacy_state(&fixture, 0, NULL, 0);
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_MISSING_KEYWORD_AUTHORITY);
    fixture_free(&fixture);

    uint8_t conflicting_flags[] = {0, 1};
    const uint16_t* states[] = {NULL, a, b};
    const size_t counts[] = {0, 1, 1};
    CHECK(build_generic(names, conflicting_flags, 2, full_mask, states,
                        counts, 3, SHADERLAB_VARIANT_PLAN_OK,
                        &plan) == 0);
    CHECK(plan.stages[0].axis_count == 2);
    CHECK(!plan.stages[0].axes[0].is_local);
    CHECK(plan.stages[0].axes[1].is_local);
    CHECK(plan.stages[0].generated_state_count == 4);
    CHECK(plan.stages[0].generated_domain_is_symbolic_boolean);
    shaderlab_variant_plan_free(&plan);
    return 0;
}

static int test_exact_order_and_first_tie_authority(void) {
    char* names[] = {"A", "B"};
    /* FillKeywordSpace masks flags with one: higher bits cannot steer scope. */
    uint8_t flags[] = {0xfe, 0xff};
    const uint16_t mask[] = {0, 1};
    const uint16_t a[] = {0};
    const uint16_t b[] = {1};
    const uint16_t ab[] = {0, 1};
    const uint16_t* states[] = {NULL, a, b, ab};
    const size_t counts[] = {0, 1, 1, 2};
    Fixture fixture;
    fixture_init(&fixture, names, flags, 2, 0, 4);
    fixture_set_mask(&fixture, (uint16_t*)mask, 2);
    for (size_t i = 0; i < 4; ++i)
        fixture_set_legacy_state(&fixture, i, states[i], counts[i]);
    ShaderLabVariantPlan plan;
    ShaderLabVariantPlanDiagnostic diagnostic;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_OK);
    CHECK(plan.stages[0].axis_count == 2);
    CHECK(plan.stages[0].axes[0].keyword_indices[0] == 0);
    CHECK(plan.stages[0].axes[1].keyword_indices[0] == 1);
    CHECK(!plan.stages[0].axes[0].is_local);
    CHECK(plan.stages[0].axes[1].is_local);
    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strstr(pragmas.buf, "#pragma multi_compile __ A\n") != NULL);
    CHECK(strstr(pragmas.buf,
                 "#pragma multi_compile_local __ B\n") != NULL);
    sb_free(&pragmas);

    const uint16_t request_indices[] = {0, 1};
    const ShaderLabVariantState request = {request_indices, 2};
    ShaderLabVariantCandidate tied[] = {
        {{a, 1}, true}, {{b, 1}, true}};
    size_t selected = SIZE_MAX;
    int32_t score = 0;
    CHECK(shaderlab_variant_select_best(&request, &request, tied, 2,
                                        &selected, &score));
    CHECK(selected == 0 && score == 1);
    shaderlab_variant_plan_free(&plan);
    fixture_free(&fixture);
    return 0;
}

static int test_three_tier_alias_routing(void) {
    char* names[] = {"A"};
    uint8_t flags[] = {0};
    uint16_t mask[] = {0};
    const uint16_t enabled[] = {0};
    Fixture fixture;
    fixture_init(&fixture, names, flags, 1, 0, 6);
    fixture_set_mask(&fixture, mask, 1);
    for (int tier = 0; tier < 3; ++tier) {
        const size_t first = (size_t)tier * 2u;
        fixture.identities[first].hardware_tier_group = tier;
        fixture.identities[first + 1u].hardware_tier_group = tier;
        fixture_set_legacy_state(&fixture, first, NULL, 0);
        fixture_set_legacy_state(&fixture, first + 1u, enabled, 1);
    }
    ShaderLabVariantPlan plan;
    ShaderLabVariantPlanDiagnostic diagnostic;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&fixture.shader, &fixture.pass, &plan,
                                       &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_OK);
    CHECK(plan.uses_specific_hardware_tiers);
    CHECK(shaderlab_stage_planned_subprogram_index(&plan, 0, 0, 0) == 0);
    CHECK(shaderlab_stage_planned_subprogram_index(&plan, 0, 1, 0) == 1);
    CHECK(shaderlab_stage_planned_subprogram_index(&plan, 0, 0, 1) == 2);
    CHECK(shaderlab_stage_planned_subprogram_index(&plan, 0, 1, 1) == 3);
    CHECK(shaderlab_stage_planned_subprogram_index(&plan, 0, 0, 2) == 4);
    CHECK(shaderlab_stage_planned_subprogram_index(&plan, 0, 1, 2) == 5);
    CHECK(shaderlab_stage_planned_subprogram_index(&plan, 0, 0, 3) == -1);
    shaderlab_variant_plan_free(&plan);
    fixture_free(&fixture);
    return 0;
}

static int test_large_sparse_domain_uses_symbolic_boolean_selector(void) {
    enum { KEYWORD_COUNT = 17, STATE_COUNT = 19 };
    char name_storage[KEYWORD_COUNT][8];
    char* names[KEYWORD_COUNT];
    uint8_t flags[KEYWORD_COUNT];
    uint16_t mask[KEYWORD_COUNT];
    uint16_t singleton[KEYWORD_COUNT][1];
    const uint16_t* states[STATE_COUNT];
    size_t counts[STATE_COUNT];
    memset(flags, 0, sizeof(flags));
    states[0] = NULL;
    counts[0] = 0;
    for (size_t keyword = 0; keyword < KEYWORD_COUNT; ++keyword) {
        snprintf(name_storage[keyword], sizeof(name_storage[keyword]),
                 "K%zu", keyword);
        names[keyword] = name_storage[keyword];
        mask[keyword] = (uint16_t)keyword;
        singleton[keyword][0] = (uint16_t)keyword;
        states[keyword + 1u] = singleton[keyword];
        counts[keyword + 1u] = 1;
    }
    const uint16_t pair[] = {0, 1};
    states[STATE_COUNT - 1u] = pair;
    counts[STATE_COUNT - 1u] = 2;

    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    CHECK(build_generic(names, flags, KEYWORD_COUNT, mask, states, counts,
                        STATE_COUNT, SHADERLAB_VARIANT_PLAN_OK, &plan) == 0);
    CHECK(!plan.has_builtin);
    CHECK(plan.stages[0].generated_domain_is_symbolic_boolean);
    CHECK(plan.stages[0].axis_count == KEYWORD_COUNT);
    CHECK(plan.stages[0].generated_state_count ==
          (((size_t)1u) << KEYWORD_COUNT));
    CHECK(plan.stages[0].generated_states == NULL);
    CHECK(plan.stages[0].generated_aliases == NULL);
    for (size_t axis = 0; axis < plan.stages[0].axis_count; ++axis) {
        CHECK(plan.stages[0].axes[axis].has_default);
        CHECK(plan.stages[0].axes[axis].keyword_count == 1u);
    }

    shaderlab_variant_plan_free(&plan);
    return 0;
}

static int test_all_common_stage_axes_are_shared_across_order(void) {
    char* names[] = {"FOG_LINEAR", "SHADOWS_SCREEN", "VERTEXLIGHT_ON",
                     "FRAGMENT_ONLY"};
    uint16_t fog[] = {0};
    uint16_t shadows[] = {1};
    uint16_t vertex_light[] = {2};
    uint16_t fragment_only[] = {3};
    ShaderLabVariantAxis vertex_axes[] = {
        {.has_default = true, .keyword_count = 1, .keyword_indices = fog},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = vertex_light},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = shadows},
    };
    ShaderLabVariantAxis fragment_axes[] = {
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = shadows},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = fragment_only},
        {.has_default = true, .keyword_count = 1, .keyword_indices = fog},
    };
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count = 4;
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count =
        sizeof(vertex_axes) / sizeof(vertex_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = vertex_axes;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count =
        sizeof(fragment_axes) / sizeof(fragment_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = fragment_axes;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strcmp(pragmas.buf,
                 "#pragma multi_compile __ FOG_LINEAR\n"
                 "#pragma multi_compile __ SHADOWS_SCREEN\n"
                 "#pragma multi_compile_vertex __ VERTEXLIGHT_ON\n"
                 "#pragma multi_compile_fragment __ FRAGMENT_ONLY\n") == 0);
    sb_free(&pragmas);

    EmittedAxisOrder vertex_order = {{0}, 0U};
    EmittedAxisOrder fragment_order = {{0}, 0U};
    CHECK(shaderlab_variant_plan_visit_emitted_stage_axes(
        &plan, UNITY_SERIALIZED_STAGE_VERTEX,
        collect_single_keyword_axis, &vertex_order));
    CHECK(shaderlab_variant_plan_visit_emitted_stage_axes(
        &plan, UNITY_SERIALIZED_STAGE_FRAGMENT,
        collect_single_keyword_axis, &fragment_order));
    const uint16_t expected_vertex[] = {0U, 1U, 2U};
    const uint16_t expected_fragment[] = {0U, 1U, 3U};
    CHECK(vertex_order.count ==
          sizeof(expected_vertex) / sizeof(expected_vertex[0]));
    CHECK(fragment_order.count ==
          sizeof(expected_fragment) / sizeof(expected_fragment[0]));
    CHECK(memcmp(vertex_order.indices, expected_vertex,
                 sizeof(expected_vertex)) == 0);
    CHECK(memcmp(fragment_order.indices, expected_fragment,
                 sizeof(expected_fragment)) == 0);
    return 0;
}

static int test_particle_order_and_distinct_raw_aliases_are_synchronized(void) {
    char* names[] = {
        "PROCEDURAL_INSTANCING_ON", "FOG_LINEAR", "SOFTPARTICLES_ON",
        "_FADING_ON", "PROCEDURAL_INSTANCING_ON", "SOFTPARTICLES_ON",
        "FOG_LINEAR", "_FADING_ON", "FRAGMENT_ONLY"};
    uint16_t vertex_fading[] = {3};
    uint16_t vertex_procedural[] = {0};
    uint16_t vertex_fog[] = {1};
    uint16_t vertex_soft[] = {2};
    uint16_t fragment_procedural[] = {4};
    uint16_t fragment_soft[] = {5};
    uint16_t fragment_fog[] = {6};
    uint16_t fragment_fading[] = {7};
    uint16_t fragment_only[] = {8};
    ShaderLabVariantAxis vertex_axes[] = {
        {.has_default = true, .is_local = true, .keyword_count = 1,
         .keyword_indices = vertex_fading},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = vertex_procedural},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = vertex_fog},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = vertex_soft},
    };
    ShaderLabVariantAxis fragment_axes[] = {
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = fragment_procedural},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = fragment_soft},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = fragment_fog},
        {.has_default = true, .is_local = true, .keyword_count = 1,
         .keyword_indices = fragment_fading},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = fragment_only},
    };
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count =
        (int)(sizeof(names) / sizeof(names[0]));
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count =
        sizeof(vertex_axes) / sizeof(vertex_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = vertex_axes;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count =
        sizeof(fragment_axes) / sizeof(fragment_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = fragment_axes;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strcmp(pragmas.buf,
                 "#pragma multi_compile_local __ _FADING_ON\n"
                 "#pragma multi_compile __ PROCEDURAL_INSTANCING_ON\n"
                 "#pragma multi_compile __ FOG_LINEAR\n"
                 "#pragma multi_compile __ SOFTPARTICLES_ON\n"
                 "#pragma multi_compile_fragment __ FRAGMENT_ONLY\n") == 0);
    sb_free(&pragmas);
    return 0;
}

static int test_cross_stage_axis_partition_conflicts_fail_closed(void) {
    char* names[] = {"SHARED", "VERTEX_PEER", "FRAGMENT_PEER"};
    uint16_t vertex_keywords[] = {0, 1};
    uint16_t fragment_keywords[] = {0, 2};
    ShaderLabVariantAxis vertex_axis = {
        .has_default = true,
        .keyword_count = 2,
        .keyword_indices = vertex_keywords,
    };
    ShaderLabVariantAxis fragment_axis = {
        .has_default = true,
        .keyword_count = 2,
        .keyword_indices = fragment_keywords,
    };
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count = 3;
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = &vertex_axis;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = &fragment_axis;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(!shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    sb_free(&pragmas);
    return 0;
}

static int test_subset_shared_axis_fails_closed(void) {
    char* names[] = {"SHARED"};
    uint16_t keyword[] = {0};
    ShaderLabVariantAxis vertex_axis = {
        .has_default = true, .keyword_count = 1, .keyword_indices = keyword};
    ShaderLabVariantAxis fragment_axis = vertex_axis;
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count = 1;
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = &vertex_axis;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = &fragment_axis;
    plan.stages[UNITY_SERIALIZED_STAGE_GEOMETRY].active = true;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(!shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    sb_free(&pragmas);
    return 0;
}

static int test_common_stage_axes_remain_synchronized_around_builtin(void) {
    char* names[] = {"BEFORE_SHARED", "VERTEX_ONLY", "FRAGMENT_ONLY",
                     "AFTER_SHARED"};
    uint16_t before_shared[] = {0};
    uint16_t vertex_only[] = {1};
    uint16_t fragment_only[] = {2};
    uint16_t after_shared[] = {3};
    ShaderLabVariantAxis vertex_axes[] = {
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = before_shared},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = vertex_only},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = after_shared},
    };
    ShaderLabVariantAxis fragment_axes[] = {
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = before_shared},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = fragment_only},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = after_shared},
    };
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count = 4;
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.has_builtin = true;
    plan.builtin_family = SHADERLAB_BUILTIN_VARIANT_FWDBASE;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count =
        sizeof(vertex_axes) / sizeof(vertex_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = vertex_axes;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].builtin_axis_position = 2;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count =
        sizeof(fragment_axes) / sizeof(fragment_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = fragment_axes;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].builtin_axis_position = 2;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strcmp(pragmas.buf,
                 "#pragma multi_compile __ BEFORE_SHARED\n"
                 "#pragma multi_compile_vertex __ VERTEX_ONLY\n"
                 "#pragma multi_compile_fragment __ FRAGMENT_ONLY\n"
                 "#pragma multi_compile_fwdbase\n"
                 "#pragma multi_compile __ AFTER_SHARED\n") == 0);
    sb_free(&pragmas);
    return 0;
}

static int test_cross_stage_scope_and_default_mismatches_fail_closed(void) {
    char* names[] = {"SHARED"};
    uint16_t keyword[] = {0};
    ShaderLabVariantAxis vertex_axis = {
        .has_default = true,
        .keyword_count = 1,
        .keyword_indices = keyword,
    };
    ShaderLabVariantAxis fragment_axis = {
        .has_default = true,
        .is_local = true,
        .keyword_count = 1,
        .keyword_indices = keyword,
    };
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count = 1;
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = &vertex_axis;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = &fragment_axis;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(!shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    sb_free(&pragmas);

    fragment_axis.is_local = false;
    fragment_axis.has_default = false;
    sb_init(&pragmas);
    CHECK(!shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    sb_free(&pragmas);
    return 0;
}

static int test_all_common_axis_is_hoisted_across_three_stages(void) {
    char* names[] = {"SHARED", "VERTEX_ONLY", "FRAGMENT_ONLY",
                     "GEOMETRY_ONLY"};
    uint16_t shared[] = {0};
    uint16_t vertex_only[] = {1};
    uint16_t fragment_only[] = {2};
    uint16_t geometry_only[] = {3};
    ShaderLabVariantAxis vertex_axes[] = {
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = shared},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = vertex_only},
    };
    ShaderLabVariantAxis fragment_axes[] = {
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = fragment_only},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = shared},
    };
    ShaderLabVariantAxis geometry_axes[] = {
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = geometry_only},
        {.has_default = true, .keyword_count = 1,
         .keyword_indices = shared},
    };
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count = 4;
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count =
        sizeof(vertex_axes) / sizeof(vertex_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = vertex_axes;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count =
        sizeof(fragment_axes) / sizeof(fragment_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = fragment_axes;
    plan.stages[UNITY_SERIALIZED_STAGE_GEOMETRY].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_GEOMETRY].axis_count =
        sizeof(geometry_axes) / sizeof(geometry_axes[0]);
    plan.stages[UNITY_SERIALIZED_STAGE_GEOMETRY].axes = geometry_axes;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strcmp(pragmas.buf,
                 "#pragma multi_compile __ SHARED\n"
                 "#pragma multi_compile_vertex __ VERTEX_ONLY\n"
                 "#pragma multi_compile_fragment __ FRAGMENT_ONLY\n"
                 "#pragma multi_compile_geometry __ GEOMETRY_ONLY\n") ==
          0);
    sb_free(&pragmas);
    return 0;
}

static int test_cross_stage_option_order_mismatch_fails_closed(void) {
    char* names[] = {"OPTION_A", "OPTION_B"};
    uint16_t vertex_keywords[] = {0, 1};
    uint16_t fragment_keywords[] = {1, 0};
    ShaderLabVariantAxis vertex_axis = {
        .has_default = true,
        .keyword_count = 2,
        .keyword_indices = vertex_keywords,
    };
    ShaderLabVariantAxis fragment_axis = {
        .has_default = true,
        .keyword_count = 2,
        .keyword_indices = fragment_keywords,
    };
    SerializedShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.keyword_names.count = 2;
    shader.keyword_names.keywords = names;
    SerializedPass pass;
    memset(&pass, 0, sizeof(pass));
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    plan.shader = &shader;
    plan.pass = &pass;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_VERTEX].axes = &vertex_axis;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].active = true;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axis_count = 1;
    plan.stages[UNITY_SERIALIZED_STAGE_FRAGMENT].axes = &fragment_axis;

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(!shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    sb_free(&pragmas);
    return 0;
}

int main(void) {
    CHECK(test_sparse_shape_and_legacy_combined_authority() == 0);
    CHECK(test_shadowcollector_pinned_domain_without_pass_type() == 0);
    CHECK(test_sparse_and_reordered_domains_use_proven_boolean_fallback() ==
          0);
    CHECK(test_raw_index_and_scope_failures() == 0);
    CHECK(test_pass_authority_and_scope_factor_failures() == 0);
    CHECK(test_exact_order_and_first_tie_authority() == 0);
    CHECK(test_three_tier_alias_routing() == 0);
    CHECK(test_large_sparse_domain_uses_symbolic_boolean_selector() == 0);
    CHECK(test_all_common_stage_axes_are_shared_across_order() == 0);
    CHECK(test_particle_order_and_distinct_raw_aliases_are_synchronized() ==
          0);
    CHECK(test_cross_stage_axis_partition_conflicts_fail_closed() == 0);
    CHECK(test_subset_shared_axis_fails_closed() == 0);
    CHECK(test_common_stage_axes_remain_synchronized_around_builtin() == 0);
    CHECK(test_cross_stage_scope_and_default_mismatches_fail_closed() == 0);
    CHECK(test_all_common_axis_is_hoisted_across_three_stages() == 0);
    CHECK(test_cross_stage_option_order_mismatch_fails_closed() == 0);
    puts("shaderlab variant plan unit tests passed");
    return 0;
}
