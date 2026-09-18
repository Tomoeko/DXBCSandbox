#include "translation/shaderlab_variant_domain.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static ShaderLabVariantState state(const uint16_t* indices, size_t count) {
    ShaderLabVariantState value = {indices, count};
    return value;
}

static int test_pass_mask_and_canonical_states(void) {
    const uint16_t request_indices[] = {1, 3, 8};
    const uint16_t mask_indices[] = {0, 1, 2, 3};
    const ShaderLabVariantState request = state(request_indices, 3);
    const ShaderLabVariantState mask = state(mask_indices, 4);
    size_t count = 0;

    CHECK(shaderlab_variant_state_is_canonical(&request));
    CHECK(shaderlab_variant_apply_pass_mask(&request, &mask, NULL, 0,
                                            &count));
    CHECK(count == 2);

    uint16_t too_small[1] = {UINT16_MAX};
    CHECK(!shaderlab_variant_apply_pass_mask(&request, &mask, too_small, 1,
                                             &count));
    CHECK(count == 2);
    CHECK(too_small[0] == UINT16_MAX);

    uint16_t output[2] = {0, 0};
    CHECK(shaderlab_variant_apply_pass_mask(&request, &mask, output, 2,
                                            &count));
    CHECK(count == 2 && output[0] == 1 && output[1] == 3);

    const uint16_t duplicate_indices[] = {1, 1};
    const ShaderLabVariantState duplicate = state(duplicate_indices, 2);
    CHECK(!shaderlab_variant_state_is_canonical(&duplicate));
    CHECK(!shaderlab_variant_apply_pass_mask(&duplicate, &mask, output, 2,
                                             &count));

    const uint16_t global_indices[] = {1, 8};
    const uint16_t local_indices[] = {3, 7};
    const ShaderLabVariantScopedState scoped = {
        {global_indices, 2}, {local_indices, 2}};
    CHECK(shaderlab_variant_merge_scopes(&scoped, NULL, 0, &count));
    CHECK(count == 4);
    uint16_t merged[4] = {0, 0, 0, 0};
    CHECK(shaderlab_variant_merge_scopes(&scoped, merged, 4, &count));
    CHECK(merged[0] == 1 && merged[1] == 3 && merged[2] == 7 &&
          merged[3] == 8);

    const uint16_t overlapping_local[] = {1, 7};
    const ShaderLabVariantScopedState overlap = {
        {global_indices, 2}, {overlapping_local, 2}};
    CHECK(shaderlab_variant_merge_scopes(&overlap, NULL, 0, &count));
    CHECK(count == 3);
    CHECK(shaderlab_variant_merge_scopes(&overlap, merged, 4, &count));
    CHECK(merged[0] == 1 && merged[1] == 7 && merged[2] == 8);
    return 0;
}

static int test_runtime_score_and_serialized_order(void) {
    const uint16_t request_indices[] = {1, 2};
    const uint16_t one_indices[] = {1};
    const uint16_t two_indices[] = {2};
    const uint16_t both_indices[] = {1, 2};
    const ShaderLabVariantState request = state(request_indices, 2);
    const ShaderLabVariantState one = state(one_indices, 1);
    const ShaderLabVariantState two = state(two_indices, 1);
    const ShaderLabVariantState both = state(both_indices, 2);
    const ShaderLabVariantState empty = state(NULL, 0);
    int32_t score = 0;

    CHECK(shaderlab_variant_match_score(&request, &one, &score));
    CHECK(score == 1);
    CHECK(shaderlab_variant_match_score(&request, &two, &score));
    CHECK(score == 1);
    CHECK(shaderlab_variant_match_score(&request, &both, &score));
    CHECK(score == 2);

    ShaderLabVariantCandidate tied[] = {{one, true}, {two, true}};
    size_t selected = SIZE_MAX;
    CHECK(shaderlab_variant_select_best(&request, &both, tied, 2, &selected,
                                        &score));
    CHECK(selected == 0 && score == 1);
    CHECK(shaderlab_variant_pass_authority_is_valid(&both, tied, 2));

    ShaderLabVariantCandidate exact[] = {
        {one, true}, {both, true}, {two, true}};
    CHECK(shaderlab_variant_select_best(&request, &both, exact, 3, &selected,
                                        &score));
    CHECK(selected == 1 && score == 2);

    const uint16_t request_one_indices[] = {1};
    const ShaderLabVariantState request_one = state(request_one_indices, 1);
    ShaderLabVariantCandidate penalty[] = {{both, true}, {empty, true}};
    CHECK(shaderlab_variant_match_score(&request_one, &both, &score));
    CHECK(score == -15);
    CHECK(shaderlab_variant_select_best(&request_one, &both, penalty, 2,
                                        &selected, &score));
    CHECK(selected == 1 && score == 0);

    ShaderLabVariantCandidate unsupported[] = {{both, false}, {one, true}};
    CHECK(shaderlab_variant_select_best(&request, &both, unsupported, 2,
                                        &selected, &score));
    CHECK(selected == 1 && score == 1);

    const uint16_t request_outside_indices[] = {1, 99};
    const ShaderLabVariantState request_outside =
        state(request_outside_indices, 2);
    ShaderLabVariantCandidate masked[] = {{one, true}};
    CHECK(shaderlab_variant_select_best(&request_outside, &one, masked, 1,
                                        &selected, &score));
    CHECK(selected == 0 && score == 1);

    ShaderLabVariantCandidate invalid_candidate[] = {{two, true}};
    CHECK(!shaderlab_variant_pass_authority_is_valid(
        &one, invalid_candidate, 1));
    CHECK(!shaderlab_variant_select_best(&request, &one, invalid_candidate, 1,
                                         &selected, &score));
    CHECK(selected == SIZE_MAX && score == INT32_MIN);
    return 0;
}

static int test_pinned_domain_shapes_and_ambiguity(void) {
    ShaderLabBuiltinVariantDomain domain;
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    CHECK(strcmp(domain.directive, "multi_compile_fwdbase") == 0);
    CHECK(domain.variant_count == 83);
    CHECK(shaderlab_builtin_variant_keyword_count(&domain, 0) == 1);
    CHECK(strcmp(shaderlab_builtin_variant_keyword_at(&domain, 0, 0),
                 "DIRECTIONAL") == 0);
    CHECK(shaderlab_builtin_variant_keyword_at(&domain, 0, 1) == NULL);

    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        UNITY_SERIALIZED_STAGE_FRAGMENT, &domain));
    CHECK(domain.variant_count == 63);
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    CHECK(domain.variant_count == 19);
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA,
        UNITY_SERIALIZED_STAGE_FRAGMENT, &domain));
    CHECK(domain.variant_count == 16);

    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDADD_FULLSHADOWS,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    CHECK(domain.variant_count == 47);
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDADD,
        UNITY_SERIALIZED_STAGE_FRAGMENT, &domain));
    CHECK(domain.variant_count == 5);
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_LIGHTPASS,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    CHECK(strcmp(domain.directive, "multi_compile_lightpass") == 0);
    CHECK(domain.variant_count == 47);
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_PREPASSFINAL,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    CHECK(domain.variant_count == 38);
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER,
        UNITY_SERIALIZED_STAGE_FRAGMENT, &domain));
    CHECK(domain.variant_count == 2);
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR,
        UNITY_SERIALIZED_STAGE_FRAGMENT, &domain));
    CHECK(strcmp(domain.directive, "multi_compile_shadowcollector") == 0);
    CHECK(domain.variant_count == 4);
    CHECK(domain.variant_masks[0] == 0 && domain.variant_masks[1] == 1 &&
          domain.variant_masks[2] == 2 && domain.variant_masks[3] == 3);
    CHECK(!shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER,
        UNITY_SERIALIZED_STAGE_RAY_TRACING, &domain));

    ShaderLabBuiltinVariantFamily families[2] = {
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        SHADERLAB_BUILTIN_VARIANT_FWDADD};
    const int pass_types[] = {INT_MIN, 0, 3, 4, 5, 7, 8, INT_MAX};
    for (size_t i = 0; i < sizeof(pass_types) / sizeof(pass_types[0]); ++i) {
        CHECK(shaderlab_builtin_variant_family_candidates_for_pass(
                  pass_types[i], NULL, 0) == 0);
        CHECK(shaderlab_builtin_variant_family_candidates_for_pass(
                  pass_types[i], families, 2) == 0);
        CHECK(families[0] == SHADERLAB_BUILTIN_VARIANT_FWDBASE);
        CHECK(families[1] == SHADERLAB_BUILTIN_VARIANT_FWDADD);
    }
    return 0;
}

static int find_keywords(const ShaderLabBuiltinVariantDomain* domain,
                         const char* const* keywords, size_t keyword_count,
                         size_t expected_external_count,
                         size_t* out_variant_index) {
    uint16_t mask = 0;
    size_t external_count = 0;
    if (!shaderlab_builtin_variant_mask_from_keywords(
            domain, keywords, keyword_count, &mask, &external_count) ||
        external_count != expected_external_count ||
        !shaderlab_builtin_variant_find_mask(
            domain, mask, SHADERLAB_BUILTIN_EXCLUDE_NONE,
            out_variant_index)) {
        return 1;
    }
    return 0;
}

static int test_sparse_builtin_membership(void) {
    ShaderLabBuiltinVariantDomain domain;
    size_t variant_index = SIZE_MAX;

    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    const char* const forward_base_vertex[] = {
        "DIRECTIONAL", "DYNAMICLIGHTMAP_ON", "DIRLIGHTMAP_COMBINED",
        "SHADOWS_SCREEN", "VERTEXLIGHT_ON", "FOG_LINEAR"};
    CHECK(find_keywords(&domain, forward_base_vertex,
                        sizeof(forward_base_vertex) /
                            sizeof(forward_base_vertex[0]),
                        1, &variant_index) == 0);
    CHECK(variant_index == 70);

    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        UNITY_SERIALIZED_STAGE_FRAGMENT, &domain));
    const char* const forward_base_fragment[] = {
        "DIRECTIONAL", "DYNAMICLIGHTMAP_ON", "DIRLIGHTMAP_COMBINED",
        "SHADOWS_SCREEN", "FOG_LINEAR"};
    CHECK(find_keywords(&domain, forward_base_fragment,
                        sizeof(forward_base_fragment) /
                            sizeof(forward_base_fragment[0]),
                        1, &variant_index) == 0);
    CHECK(variant_index == 50);

    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDADD_FULLSHADOWS,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    const char* const forward_add_spot[] = {
        "SPOT", "SHADOWS_DEPTH", "SHADOWS_SOFT", "FOG_LINEAR"};
    CHECK(find_keywords(&domain, forward_add_spot,
                        sizeof(forward_add_spot) /
                            sizeof(forward_add_spot[0]),
                        1, &variant_index) == 0);
    CHECK(variant_index == 16);
    const char* const forward_add_point_cookie[] = {
        "POINT_COOKIE", "SHADOWS_CUBE", "SHADOWS_SOFT"};
    CHECK(find_keywords(&domain, forward_add_point_cookie,
                        sizeof(forward_add_point_cookie) /
                            sizeof(forward_add_point_cookie[0]),
                        0, &variant_index) == 0);
    CHECK(variant_index == 40);

    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_PREPASSFINAL,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));
    const char* const prepass[] = {
        "DYNAMICLIGHTMAP_ON", "LIGHTMAP_ON", "DIRLIGHTMAP_COMBINED",
        "UNITY_HDR_ON"};
    CHECK(find_keywords(&domain, prepass,
                        sizeof(prepass) / sizeof(prepass[0]), 0,
                        &variant_index) == 0);
    CHECK(variant_index == 33);

    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER,
        UNITY_SERIALIZED_STAGE_FRAGMENT, &domain));
    const char* const shadowcaster[] = {"SHADOWS_CUBE"};
    CHECK(find_keywords(&domain, shadowcaster, 1, 0, &variant_index) == 0);
    CHECK(variant_index == 1);

    ShaderLabBuiltinVariantFamily family = SHADERLAB_BUILTIN_VARIANT_FWDBASE;
    const uint16_t forward_base_observed[] = {0x001, 0x011, 0x11d};
    const int pass_types[] = {INT_MIN, 0, 4, 5, 7, 8, INT_MAX};
    for (size_t i = 0; i < sizeof(pass_types) / sizeof(pass_types[0]); ++i) {
        family = SHADERLAB_BUILTIN_VARIANT_FWDBASE;
        CHECK(shaderlab_builtin_variant_recognize_for_pass(
                  pass_types[i], UNITY_SERIALIZED_STAGE_VERTEX,
                  forward_base_observed,
                  sizeof(forward_base_observed) /
                      sizeof(forward_base_observed[0]),
                  SHADERLAB_BUILTIN_EXCLUDE_NONE, &family) ==
              SHADERLAB_BUILTIN_RECOGNITION_NO_MATCH);
        CHECK(family == SHADERLAB_BUILTIN_VARIANT_INVALID);
    }

    family = SHADERLAB_BUILTIN_VARIANT_FWDBASE;
    CHECK(shaderlab_builtin_variant_recognize_for_pass(
              0, UNITY_SERIALIZED_STAGE_VERTEX, forward_base_observed,
              sizeof(forward_base_observed) /
                  sizeof(forward_base_observed[0]),
              UINT32_C(1) << 31, &family) ==
          SHADERLAB_BUILTIN_RECOGNITION_INVALID_ARGUMENT);
    CHECK(family == SHADERLAB_BUILTIN_VARIANT_INVALID);
    return 0;
}

static int test_exact_compiler_exclusion_rules(void) {
    ShaderLabBuiltinVariantDomain domain;
    size_t variant_index = SIZE_MAX;
    CHECK(shaderlab_builtin_variant_domain_get(
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        UNITY_SERIALIZED_STAGE_VERTEX, &domain));

    const char* const shadows[] = {"DIRECTIONAL", "SHADOWS_SCREEN"};
    CHECK(find_keywords(&domain, shadows, 2, 0, &variant_index) == 0);
    CHECK(shaderlab_builtin_variant_is_included(
        &domain, variant_index, SHADERLAB_BUILTIN_EXCLUDE_NONE));
    CHECK(!shaderlab_builtin_variant_is_included(
        &domain, variant_index, SHADERLAB_BUILTIN_EXCLUDE_SHADOWS));

    const char* const lightmap[] = {"DIRECTIONAL", "LIGHTMAP_ON"};
    CHECK(find_keywords(&domain, lightmap, 2, 0, &variant_index) == 0);
    CHECK(!shaderlab_builtin_variant_is_included(
        &domain, variant_index, SHADERLAB_BUILTIN_EXCLUDE_LIGHTMAP));

    const char* const vertex_light[] = {"DIRECTIONAL", "VERTEXLIGHT_ON"};
    CHECK(find_keywords(&domain, vertex_light, 2, 0, &variant_index) == 0);
    CHECK(!shaderlab_builtin_variant_is_included(
        &domain, variant_index,
        SHADERLAB_BUILTIN_EXCLUDE_VERTEX_LIGHT));

    CHECK(!shaderlab_builtin_variant_is_included(&domain, 0,
                                                  UINT32_C(1) << 31));
    return 0;
}

static uint64_t fnv1a_byte(uint64_t hash, uint8_t byte) {
    return (hash ^ byte) * UINT64_C(0x100000001b3);
}

static uint64_t fnv1a_u64_le(uint64_t hash, uint64_t value) {
    for (unsigned int byte = 0; byte < 8; ++byte) {
        hash = fnv1a_byte(hash, (uint8_t)(value >> (byte * 8)));
    }
    return hash;
}

static uint64_t fnv1a_c_string(uint64_t hash, const char* text) {
    for (size_t i = 0;; ++i) {
        hash = fnv1a_byte(hash, (uint8_t)text[i]);
        if (text[i] == '\0') return hash;
    }
}

static uint64_t domain_fingerprint(
    const ShaderLabBuiltinVariantDomain* domain) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    hash = fnv1a_c_string(hash, domain->directive);
    hash = fnv1a_u64_le(hash, domain->keyword_count);
    for (size_t i = 0; i < domain->keyword_count; ++i) {
        hash = fnv1a_c_string(hash, domain->keyword_names[i]);
    }
    hash = fnv1a_u64_le(hash, domain->variant_count);
    for (size_t i = 0; i < domain->variant_count; ++i) {
        hash = fnv1a_byte(hash, (uint8_t)domain->variant_masks[i]);
        hash = fnv1a_byte(hash,
                          (uint8_t)(domain->variant_masks[i] >> 8));
    }
    return hash;
}

static int test_exact_domain_fingerprints(void) {
    typedef struct {
        ShaderLabBuiltinVariantFamily family;
        UnitySerializedProgramStage stage;
        uint64_t fingerprint;
    } ExpectedDomain;
    static const ExpectedDomain expected[] = {
        {SHADERLAB_BUILTIN_VARIANT_FWDBASE,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0x897d356249be035a)},
        {SHADERLAB_BUILTIN_VARIANT_FWDBASE,
         UNITY_SERIALIZED_STAGE_FRAGMENT, UINT64_C(0xd05d9fc497a56cf2)},
        {SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0x4d2da319129ab21f)},
        {SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA,
         UNITY_SERIALIZED_STAGE_FRAGMENT, UINT64_C(0x81297f2998959136)},
        {SHADERLAB_BUILTIN_VARIANT_FWDADD_FULLSHADOWS,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0x59d0da1e3118aa93)},
        {SHADERLAB_BUILTIN_VARIANT_FWDADD,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0xadb129a765e1169a)},
        {SHADERLAB_BUILTIN_VARIANT_LIGHTPASS,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0x7183ebab7a3a2559)},
        {SHADERLAB_BUILTIN_VARIANT_PREPASSFINAL,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0xc01879dc4632dc45)},
        {SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0xb1f55c44c202da50)},
        {SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR,
         UNITY_SERIALIZED_STAGE_VERTEX, UINT64_C(0x09d07db283fa6b3c)},
    };

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ShaderLabBuiltinVariantDomain domain;
        CHECK(shaderlab_builtin_variant_domain_get(
            expected[i].family, expected[i].stage, &domain));
        CHECK(domain_fingerprint(&domain) == expected[i].fingerprint);
    }
    return 0;
}

static int test_domain_rows_are_unique(void) {
    const ShaderLabBuiltinVariantFamily families[] = {
        SHADERLAB_BUILTIN_VARIANT_FWDADD_FULLSHADOWS,
        SHADERLAB_BUILTIN_VARIANT_FWDADD,
        SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA,
        SHADERLAB_BUILTIN_VARIANT_FWDBASE,
        SHADERLAB_BUILTIN_VARIANT_LIGHTPASS,
        SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER,
        SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR,
        SHADERLAB_BUILTIN_VARIANT_PREPASSFINAL};
    const UnitySerializedProgramStage stages[] = {
        UNITY_SERIALIZED_STAGE_VERTEX,
        UNITY_SERIALIZED_STAGE_FRAGMENT};

    for (size_t family_index = 0;
         family_index < sizeof(families) / sizeof(families[0]);
         ++family_index) {
        for (size_t stage_index = 0;
             stage_index < sizeof(stages) / sizeof(stages[0]);
             ++stage_index) {
            ShaderLabBuiltinVariantDomain domain;
            CHECK(shaderlab_builtin_variant_domain_get(
                families[family_index], stages[stage_index], &domain));
            for (size_t i = 0; i < domain.variant_count; ++i) {
                CHECK((domain.variant_masks[i] >> domain.keyword_count) == 0);
                for (size_t previous = 0; previous < i; ++previous) {
                    CHECK(domain.variant_masks[previous] !=
                          domain.variant_masks[i]);
                }
                const size_t keyword_count =
                    shaderlab_builtin_variant_keyword_count(&domain, i);
                for (size_t keyword = 0; keyword < keyword_count; ++keyword) {
                    CHECK(shaderlab_builtin_variant_keyword_at(
                              &domain, i, keyword) != NULL);
                }
                CHECK(shaderlab_builtin_variant_keyword_at(
                          &domain, i, keyword_count) == NULL);
            }
        }
    }
    return 0;
}

int main(void) {
    if (test_pass_mask_and_canonical_states() != 0) return 1;
    if (test_runtime_score_and_serialized_order() != 0) return 1;
    if (test_pinned_domain_shapes_and_ambiguity() != 0) return 1;
    if (test_sparse_builtin_membership() != 0) return 1;
    if (test_exact_compiler_exclusion_rules() != 0) return 1;
    if (test_exact_domain_fingerprints() != 0) return 1;
    if (test_domain_rows_are_unique() != 0) return 1;
    puts("shaderlab variant-domain unit tests passed");
    return 0;
}
