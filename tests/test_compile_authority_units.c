#include "compiler/unity_compile_authority.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
        return 1; \
    } \
} while (0)

static const char* const k_expected_platform_keywords[] = {
    "UNITY_NO_DXT5nm",
    "UNITY_NO_RGBM",
    "UNITY_USE_NATIVE_HDR",
    "UNITY_ENABLE_REFLECTION_BUFFERS",
    "UNITY_FRAMEBUFFER_FETCH_AVAILABLE",
    "UNITY_ENABLE_NATIVE_SHADOW_LOOKUPS",
    "UNITY_METAL_SHADOWS_USE_POINT_FILTERING",
    "UNITY_NO_CUBEMAP_ARRAY",
    "UNITY_NO_SCREENSPACE_SHADOWS",
    "UNITY_USE_DITHER_MASK_FOR_ALPHABLENDED_SHADOWS",
    "UNITY_PBS_USE_BRDF1",
    "UNITY_PBS_USE_BRDF2",
    "UNITY_PBS_USE_BRDF3",
    "UNITY_NO_FULL_STANDARD_SHADER",
    "UNITY_SPECCUBE_BOX_PROJECTION",
    "UNITY_SPECCUBE_BLENDING",
    "UNITY_ENABLE_DETAIL_NORMALMAP",
    "SHADER_API_MOBILE",
    "SHADER_API_DESKTOP",
    "UNITY_HARDWARE_TIER1",
    "UNITY_HARDWARE_TIER2",
    "UNITY_HARDWARE_TIER3",
    "UNITY_COLORSPACE_GAMMA",
    "UNITY_LIGHT_PROBE_PROXY_VOLUME",
    "UNITY_HALF_PRECISION_FRAGMENT_SHADER_REGISTERS",
    "UNITY_LIGHTMAP_DLDR_ENCODING",
    "UNITY_LIGHTMAP_RGBM_ENCODING",
    "UNITY_LIGHTMAP_FULL_HDR",
    "UNITY_VIRTUAL_TEXTURING",
    "UNITY_PRETRANSFORM_TO_DISPLAY_ORIENTATION",
    "UNITY_ASTC_NORMALMAP_ENCODING",
    "SHADER_API_GLES30",
    "UNITY_UNIFIED_SHADER_PRECISION_MODEL",
};

static const uint64_t k_expected_settings_mask = UINT64_C(0x1fff9ff79);

static int strings_equal(
    char* const* actual, int actual_count,
    const char* const* expected, size_t expected_count) {
    if (actual_count < 0 || (size_t)actual_count != expected_count) return 0;
    for (size_t i = 0; i < expected_count; i++) {
        if (!actual[i] || strcmp(actual[i], expected[i]) != 0) return 0;
    }
    return 1;
}

static int set_complete_empty_families(
    SnippetCompileContract* contract, int32_t compiler_program) {
    return unity_compiler_snippet_contract_set_variant_combinations(
               contract, compiler_program,
               UNITY_KEYWORD_VARIANTS_USER_GLOBAL, NULL, 0) &&
           unity_compiler_snippet_contract_set_variant_combinations(
               contract, compiler_program,
               UNITY_KEYWORD_VARIANTS_USER_LOCAL, NULL, 0) &&
           unity_compiler_snippet_contract_set_variant_combinations(
               contract, compiler_program,
               UNITY_KEYWORD_VARIANTS_BUILTIN, NULL, 0);
}

static int set_conditions(
    SnippetCompileContract* contract,
    const char* const* names, const uint64_t* requirements, size_t count) {
    if (count > (size_t)INT_MAX) return 0;
    ConditionalShaderRequirement* values = count > 0
        ? (ConditionalShaderRequirement*)calloc(count, sizeof(*values))
        : NULL;
    if (count > 0 && !values) return 0;
    for (size_t i = 0; i < count; i++) {
        values[i].keyword = strdup(names[i]);
        if (!values[i].keyword) {
            for (size_t j = 0; j < i; j++) free(values[j].keyword);
            free(values);
            return 0;
        }
        values[i].requirements = requirements[i];
    }
    contract->conditional_requirements = values;
    contract->conditional_requirement_count = (int)count;
    return 1;
}

static int initialize_sentinel(UnityCompileAuthority* authority) {
    unity_compile_authority_init(authority);
    authority->platform_keywords = (char**)calloc(1, sizeof(char*));
    authority->user_keywords = (char**)calloc(1, sizeof(char*));
    authority->disabled_keywords = (char**)calloc(1, sizeof(char*));
    if (!authority->platform_keywords || !authority->user_keywords ||
        !authority->disabled_keywords) {
        unity_compile_authority_free(authority);
        return 0;
    }
    authority->platform_keywords[0] = strdup("old-platform");
    authority->user_keywords[0] = strdup("old-user");
    authority->disabled_keywords[0] = strdup("old-disabled");
    if (!authority->platform_keywords[0] || !authority->user_keywords[0] ||
        !authority->disabled_keywords[0]) {
        unity_compile_authority_free(authority);
        return 0;
    }
    authority->platform_keyword_count = 1;
    authority->user_keyword_count = 1;
    authority->disabled_keyword_count = 1;
    authority->compiler_flags = UINT32_C(0x12345678);
    authority->requirements = UINT64_C(0x0123456789abcdef);
    return 1;
}

static int expect_failure_unchanged(
    const UnityCompileAuthorityInput* input,
    UnityCompileAuthorityStatus expected_status,
    UnityCompileAuthority* output) {
    char** old_platform = output->platform_keywords;
    char** old_user = output->user_keywords;
    char** old_disabled = output->disabled_keywords;
    uint32_t old_flags = output->compiler_flags;
    uint64_t old_requirements = output->requirements;
    UnityCompileAuthorityStatus status =
        unity_compile_authority_build(input, output);
    if (status != expected_status || output->platform_keywords != old_platform ||
        output->user_keywords != old_user ||
        output->disabled_keywords != old_disabled ||
        output->platform_keyword_count != 1 || output->user_keyword_count != 1 ||
        output->disabled_keyword_count != 1 ||
        output->compiler_flags != old_flags ||
        output->requirements != old_requirements ||
        strcmp(output->platform_keywords[0], "old-platform") != 0 ||
        strcmp(output->user_keywords[0], "old-user") != 0 ||
        strcmp(output->disabled_keywords[0], "old-disabled") != 0) {
        return 0;
    }
    return 1;
}

static int test_recovered_tables(void) {
    CHECK(sizeof(k_expected_platform_keywords) /
              sizeof(k_expected_platform_keywords[0]) ==
          UNITY_PLATFORM_CAPABILITY_COUNT);
    for (size_t i = 0; i < UNITY_PLATFORM_CAPABILITY_COUNT; i++) {
        const char* actual = unity_platform_capability_keyword(i);
        CHECK(actual != NULL);
        CHECK(strcmp(actual, k_expected_platform_keywords[i]) == 0);
        CHECK(unity_platform_capability_is_settings_dependent(i) ==
              (((k_expected_settings_mask >> i) & UINT64_C(1)) != 0));
    }
    CHECK(unity_platform_capability_keyword(UNITY_PLATFORM_CAPABILITY_COUNT) ==
          NULL);
    CHECK(unity_platform_capability_keyword(SIZE_MAX) == NULL);
    CHECK(!unity_platform_capability_is_settings_dependent(
        UNITY_PLATFORM_CAPABILITY_COUNT));
    CHECK(!unity_platform_capability_is_settings_dependent(SIZE_MAX));

    static const char* const expected_pass_keywords[] = {
        NULL, NULL, NULL, NULL,
        "UNITY_PASS_FORWARDBASE",
        "UNITY_PASS_FORWARDADD",
        "UNITY_PASS_PREPASSBASE",
        "UNITY_PASS_PREPASSFINAL",
        "UNITY_PASS_SHADOWCASTER",
        NULL,
        "UNITY_PASS_DEFERRED",
        "UNITY_PASS_META",
        "UNITY_PASS_MOTIONVECTORS",
        NULL,
        "UNITY_PASS_SRPDEFAULTUNLIT",
        NULL,
    };
    for (size_t i = 0;
         i < sizeof(expected_pass_keywords) / sizeof(expected_pass_keywords[0]);
         i++) {
        const char* actual = unity_pass_type_keyword((int)i);
        if (expected_pass_keywords[i]) {
            CHECK(actual != NULL);
            CHECK(strcmp(actual, expected_pass_keywords[i]) == 0);
        } else {
            CHECK(actual == NULL);
        }
    }
    CHECK(unity_pass_type_keyword(-1) == NULL);
    CHECK(unity_pass_type_keyword(INT_MIN) == NULL);
    CHECK(unity_pass_type_keyword(INT_MAX) == NULL);

    static const int32_t expected_programs[] = {0, 1, 4, 2, 3, 6};
    for (size_t i = 0;
         i < sizeof(expected_programs) / sizeof(expected_programs[0]); i++) {
        int32_t program = -1;
        CHECK(unity_serialized_stage_to_compiler_program((int)i, &program));
        CHECK(program == expected_programs[i]);
    }
    int32_t unchanged = 0x12345678;
    CHECK(!unity_serialized_stage_to_compiler_program(-1, &unchanged));
    CHECK(unchanged == 0x12345678);
    CHECK(!unity_serialized_stage_to_compiler_program(6, &unchanged));
    CHECK(unchanged == 0x12345678);
    CHECK(!unity_serialized_stage_to_compiler_program(INT_MAX, &unchanged));
    CHECK(unchanged == 0x12345678);
    CHECK(!unity_serialized_stage_to_compiler_program(0, NULL));
    return 0;
}

static int test_exact_compile_inputs(void) {
    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    contract.compilation_flags = UINT32_MAX;
    contract.requirements = UINT64_C(0x1);

    static const char* const global_variants[] = {
        "_", "K7 SHARED ZED", "DISABLED_A ACTIVE_GLOBAL DISABLED_B",
        "SHARED",
    };
    static const char* const local_variants[] = {
        "LOCAL_DISABLED\tDISABLED_A", "ACTIVE_LOCAL _",
    };
    static const char* const builtin_variants[] = {
        "BUILTIN_X\nSHARED", "BUILTIN_Y",
    };
    static const char* const fragment_global_variants[] = {
        "FRAGMENT_GLOBAL",
    };
    static const char* const fragment_local_variants[] = {
        "FRAGMENT_LOCAL",
    };
    static const char* const fragment_builtin_variants[] = {
        "FRAGMENT_BUILTIN",
    };
    /* Deliberately insert the fragment record first. Selection is by Unity's
     * ShaderCompilerProgram wire value, never by response-array position. */
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 1, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        fragment_global_variants, (int)(sizeof(fragment_global_variants) /
                                        sizeof(fragment_global_variants[0]))));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 1, UNITY_KEYWORD_VARIANTS_USER_LOCAL,
        fragment_local_variants, (int)(sizeof(fragment_local_variants) /
                                       sizeof(fragment_local_variants[0]))));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 1, UNITY_KEYWORD_VARIANTS_BUILTIN,
        fragment_builtin_variants, (int)(sizeof(fragment_builtin_variants) /
                                         sizeof(fragment_builtin_variants[0]))));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL, global_variants,
        (int)(sizeof(global_variants) / sizeof(global_variants[0]))));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 0, UNITY_KEYWORD_VARIANTS_USER_LOCAL, local_variants,
        (int)(sizeof(local_variants) / sizeof(local_variants[0]))));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 0, UNITY_KEYWORD_VARIANTS_BUILTIN, builtin_variants,
        (int)(sizeof(builtin_variants) / sizeof(builtin_variants[0]))));

    static const char* const condition_names[] = {
        "ACTIVE_GLOBAL", "ACTIVE_LOCAL", "DISABLED_A",
        "UNITY_USE_NATIVE_HDR", "ACTIVE_GLOBAL",
    };
    static const uint64_t condition_requirements[] = {
        UINT64_C(0x2), UINT64_C(0x4), UINT64_C(0x8), UINT64_C(0x10),
        UINT64_C(0x20),
    };
    CHECK(set_conditions(
        &contract, condition_names, condition_requirements,
        sizeof(condition_names) / sizeof(condition_names[0])));
    CHECK(unity_compiler_snippet_contract_validate(&contract));

    static const char* const names[] = {
        "UNUSED_0", "ACTIVE_LOCAL", "UNUSED_2", "ACTIVE_GLOBAL",
        "UNUSED_4",
    };
    static const int global_indices[] = {3, 1, 3};
    static const int local_indices[] = {3, 1};
    const uint64_t capability_bits =
        k_expected_settings_mask | (UINT64_C(1) << 2U) |
        (UINT64_C(1) << 17U);
    UnityCompileAuthorityInput input = {
        .contract = &contract,
        .keyword_names = names,
        .keyword_name_count = sizeof(names) / sizeof(names[0]),
        .global_keyword_indices = global_indices,
        .global_keyword_index_count =
            sizeof(global_indices) / sizeof(global_indices[0]),
        .local_keyword_indices = local_indices,
        .local_keyword_index_count =
            sizeof(local_indices) / sizeof(local_indices[0]),
        .compiler_program = 0,
        .pass_type = 5,
        .platform_capabilities = {
            .present = true,
            .bits = capability_bits,
        },
    };

    UnityCompileAuthority authority;
    CHECK(initialize_sentinel(&authority));
    CHECK(unity_compile_authority_build(&input, &authority) ==
          UNITY_COMPILE_AUTHORITY_OK);

    const char* expected_platform[UNITY_PLATFORM_CAPABILITY_COUNT + 1U];
    size_t expected_platform_count = 0;
    for (size_t i = 0; i < UNITY_PLATFORM_CAPABILITY_COUNT; i++) {
        if (capability_bits & (UINT64_C(1) << i)) {
            expected_platform[expected_platform_count++] =
                k_expected_platform_keywords[i];
        }
    }
    expected_platform[expected_platform_count++] = "UNITY_PASS_FORWARDADD";
    CHECK(strings_equal(
        authority.platform_keywords, authority.platform_keyword_count,
        expected_platform, expected_platform_count));

    static const char* const expected_user[] = {
        "ACTIVE_LOCAL", "ACTIVE_GLOBAL",
    };
    CHECK(strings_equal(
        authority.user_keywords, authority.user_keyword_count,
        expected_user, sizeof(expected_user) / sizeof(expected_user[0])));

    static const char* const expected_disabled[] = {
        "K7", "SHARED", "ZED", "DISABLED_A", "DISABLED_B",
        "LOCAL_DISABLED", "BUILTIN_X", "BUILTIN_Y",
    };
    CHECK(strings_equal(
        authority.disabled_keywords, authority.disabled_keyword_count,
        expected_disabled,
        sizeof(expected_disabled) / sizeof(expected_disabled[0])));
    CHECK(authority.compiler_flags ==
          ((UINT32_C(1) << 14U) | (UINT32_C(1) << 28U) |
           (UINT32_C(1) << 31U)));
    CHECK(authority.requirements == UINT64_C(0x27));

    /* The same snippet has a different family trio for the fragment program.
     * Verify that no vertex combination leaks into fragment dKW. */
    input.compiler_program = 1;
    CHECK(unity_compile_authority_build(&input, &authority) ==
          UNITY_COMPILE_AUTHORITY_OK);
    static const char* const expected_fragment_disabled[] = {
        "FRAGMENT_GLOBAL", "FRAGMENT_LOCAL", "FRAGMENT_BUILTIN",
    };
    CHECK(strings_equal(
        authority.disabled_keywords, authority.disabled_keyword_count,
        expected_fragment_disabled,
        sizeof(expected_fragment_disabled) /
            sizeof(expected_fragment_disabled[0])));

    unity_compile_authority_free(&authority);
    unity_compiler_snippet_contract_free(&contract);
    return 0;
}

static int test_disabled_platform_order_and_flags(void) {
    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    static const char* const global_variants[] = {
        "FAMILY_FIRST UNITY_NO_DXT5nm", "FAMILY_SECOND FAMILY_FIRST",
    };
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 4, UNITY_KEYWORD_VARIANTS_USER_GLOBAL, global_variants,
        (int)(sizeof(global_variants) / sizeof(global_variants[0]))));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 4, UNITY_KEYWORD_VARIANTS_USER_LOCAL, NULL, 0));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 4, UNITY_KEYWORD_VARIANTS_BUILTIN, NULL, 0));

    UnityCompileAuthorityInput input = {
        .contract = &contract,
        .compiler_program = 4,
        .pass_type = 0,
        .platform_capabilities = {.present = true, .bits = 0},
    };
    UnityCompileAuthority authority;
    unity_compile_authority_init(&authority);
    CHECK(unity_compile_authority_build(&input, &authority) ==
          UNITY_COMPILE_AUTHORITY_OK);
    CHECK(authority.platform_keyword_count == 0);
    CHECK(authority.user_keyword_count == 0);
    CHECK(authority.compiler_flags == 0);

    const char* expected_disabled[2U + UNITY_PLATFORM_CAPABILITY_COUNT];
    size_t expected_count = 0;
    expected_disabled[expected_count++] = "FAMILY_FIRST";
    expected_disabled[expected_count++] = "UNITY_NO_DXT5nm";
    expected_disabled[expected_count++] = "FAMILY_SECOND";
    for (size_t i = 0; i < UNITY_PLATFORM_CAPABILITY_COUNT; i++) {
        if ((k_expected_settings_mask & (UINT64_C(1) << i)) == 0) continue;
        int already_present = 0;
        for (size_t j = 0; j < expected_count; j++) {
            if (strcmp(expected_disabled[j],
                       k_expected_platform_keywords[i]) == 0) {
                already_present = 1;
                break;
            }
        }
        if (!already_present) {
            expected_disabled[expected_count++] =
                k_expected_platform_keywords[i];
        }
    }
    CHECK(strings_equal(
        authority.disabled_keywords, authority.disabled_keyword_count,
        expected_disabled, expected_count));
    unity_compile_authority_free(&authority);

    /* The three recovered capability-to-flag mappings are independent, and
     * the preprocess header's compilation_flags field is not inherited. */
    contract.compilation_flags = UINT32_MAX;
    for (uint32_t combination = 0; combination < 8U; combination++) {
        input.platform_capabilities.bits = UINT64_C(1) << 2U;
        uint32_t expected_flags = 0;
        if (combination & 1U) {
            input.platform_capabilities.bits |= UINT64_C(1) << 6U;
            expected_flags |= UINT32_C(1) << 14U;
        }
        if (combination & 2U) {
            input.platform_capabilities.bits |= UINT64_C(1) << 17U;
            expected_flags |= UINT32_C(1) << 28U;
        }
        if (combination & 4U) {
            input.platform_capabilities.bits |= UINT64_C(1) << 32U;
            expected_flags |= UINT32_C(1) << 31U;
        }
        CHECK(unity_compile_authority_build(&input, &authority) ==
              UNITY_COMPILE_AUTHORITY_OK);
        CHECK(authority.compiler_flags == expected_flags);
        unity_compile_authority_free(&authority);
    }

    unity_compiler_snippet_contract_free(&contract);
    return 0;
}

static int test_fail_closed_and_strong_guarantee(void) {
    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    UnityCompileAuthorityInput input = {
        .contract = &contract,
        .compiler_program = 0,
        .platform_capabilities = {.present = true, .bits = 0},
    };
    UnityCompileAuthority output;
    CHECK(initialize_sentinel(&output));

    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_MISSING_VARIANT_FAMILIES, &output));
    CHECK(set_complete_empty_families(&contract, 0));

    /* A complete vertex trio cannot authorize a fragment compile. */
    input.compiler_program = 1;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_MISSING_VARIANT_FAMILIES, &output));
    static const char* const incomplete_fragment[] = {"ONLY_ONE_FAMILY"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 1, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        incomplete_fragment, 1));
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_MISSING_VARIANT_FAMILIES, &output));
    input.compiler_program = 0;

    input.platform_capabilities.present = false;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_MISSING_PLATFORM_CAPABILITIES,
        &output));
    input.platform_capabilities.present = true;
    input.platform_capabilities.bits = UINT64_C(1) << 33U;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT, &output));
    input.platform_capabilities.bits = 0;

    input.keyword_name_count = 1;
    input.keyword_names = NULL;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT, &output));

    static const char* const names[] = {"A", "DUP", "DUP", ""};
    input.keyword_names = names;
    input.keyword_name_count = sizeof(names) / sizeof(names[0]);
    input.global_keyword_indices = NULL;
    input.global_keyword_index_count = 1;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT, &output));

    static const int negative_index[] = {-1};
    input.global_keyword_indices = negative_index;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_KEYWORD_INDEX, &output));
    static const int out_of_bounds_index[] = {4};
    input.global_keyword_indices = out_of_bounds_index;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_KEYWORD_INDEX, &output));
    static const int empty_name_index[] = {3};
    input.global_keyword_indices = empty_name_index;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_KEYWORD_INDEX, &output));

    static const int duplicate_name_indices[] = {0, 1, 2};
    input.global_keyword_indices = duplicate_name_indices;
    input.global_keyword_index_count =
        sizeof(duplicate_name_indices) / sizeof(duplicate_name_indices[0]);
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_AMBIGUOUS_KEYWORD_NAME, &output));

    static const char* const unique_names[] = {"A", "B"};
    static const int repeated_raw_index[] = {1, 1};
    static const int repeated_across_scope[] = {1};
    input.keyword_names = unique_names;
    input.keyword_name_count = sizeof(unique_names) / sizeof(unique_names[0]);
    input.global_keyword_indices = repeated_raw_index;
    input.global_keyword_index_count =
        sizeof(repeated_raw_index) / sizeof(repeated_raw_index[0]);
    input.local_keyword_indices = repeated_across_scope;
    input.local_keyword_index_count = 1;
    CHECK(unity_compile_authority_build(&input, &output) ==
          UNITY_COMPILE_AUTHORITY_OK);
    static const char* const expected_user[] = {"B"};
    CHECK(strings_equal(
        output.user_keywords, output.user_keyword_count, expected_user, 1));
    unity_compile_authority_free(&output);
    CHECK(initialize_sentinel(&output));

    contract.start_line = -1;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT, &output));
    contract.start_line = 0;
    SnippetProgramKeywordVariants* vertex =
        contract.program_keyword_variant_count > 0
            ? &contract.program_keyword_variants[0]
            : NULL;
    CHECK(vertex != NULL);
    CHECK(vertex->compiler_program == 0);
    vertex->user_global.combination_count = 1;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT, &output));
    vertex->user_global.combination_count = 0;

    const SnippetCompileContract* saved_contract = input.contract;
    input.contract = NULL;
    CHECK(expect_failure_unchanged(
        &input, UNITY_COMPILE_AUTHORITY_MISSING_PREPROCESS_CONTRACT,
        &output));
    input.contract = saved_contract;
    CHECK(unity_compile_authority_build(NULL, &output) ==
          UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT);
    CHECK(unity_compile_authority_build(&input, NULL) ==
          UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT);
    CHECK(strcmp(unity_compile_authority_status_string(
                     UNITY_COMPILE_AUTHORITY_OK),
                 "ok") == 0);
    CHECK(unity_compile_authority_status_string(
              (UnityCompileAuthorityStatus)INT_MAX) != NULL);

    unity_compile_authority_free(&output);
    unity_compiler_snippet_contract_free(&contract);
    return 0;
}

int main(void) {
    CHECK(test_recovered_tables() == 0);
    CHECK(test_exact_compile_inputs() == 0);
    CHECK(test_disabled_platform_order_and_flags() == 0);
    CHECK(test_fail_closed_and_strong_guarantee() == 0);
    puts("compile authority unit tests passed");
    return 0;
}
