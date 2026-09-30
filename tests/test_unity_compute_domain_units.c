// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compute_domain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #condition);           \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static UnityCompilerComputePreprocessRequest base_request(void) {
    return (UnityCompilerComputePreprocessRequest){
        .source = "original source",
        .source_filename = "Assets/Controlled.compute",
        .caching_preprocessor = true,
        .build_platform = 19U,
        .valid_apis = 311856U,
    };
}

static UnityCompilerComputePreprocessResult
base_result(const UnityCompilerComputePreprocessedKernel* kernels, size_t count) {
    return (UnityCompilerComputePreprocessResult){
        .kernels = kernels,
        .kernel_count = count,
        .requirements = UINT64_C(0x4001),
        .compilation_flags = UINT32_C(0x20000000),
        .include_hash_words = {1U, 2U, UINT32_MAX, UINT32_C(0x80000000)},
        .source = "transformed source",
        .source_size = sizeof("transformed source") - 1U,
        .supported_apis = INT32_MIN + 16,
        .use_dxc_mask = UINT32_C(0x80000010),
        .never_use_dxc_mask = UINT32_C(0x40000000),
    };
}

static UnityComputeDomainContext
base_context(const UnityCompilerComputePreprocessRequest* request) {
    return (UnityComputeDomainContext){
        .preprocess_request = request,
        .max_variant_count = 4096U,
        .max_kernel_state_count = 8192U,
    };
}

static bool expect_rejected(const UnityCompilerComputePreprocessResult* result,
                            const UnityComputeDomainContext* context,
                            UnityComputeDomainStatus wanted) {
    UnityComputeDomain* domain = NULL;
    UnityComputeDomainDiagnostic diagnostic;
    UnityComputeDomainStatus status =
        unity_compute_domain_create(result, context, &domain, &diagnostic);
    if (status != wanted || diagnostic.status != wanted || domain) {
        fprintf(stderr, "domain rejection: expected %d, observed %d\n", (int)wanted, (int)status);
        unity_compute_domain_free(domain);
        return false;
    }
    return true;
}

static bool keyword_text(const UnityComputeDomainState* state, char* text, size_t capacity) {
    size_t size = 0U;
    if (!capacity)
        return false;
    text[0] = '\0';
    for (size_t keyword = 0U; keyword < state->user_keyword_count; ++keyword) {
        const char* name = state->user_keywords[keyword];
        const size_t length = strlen(name);
        const size_t separator = keyword ? 1U : 0U;
        if (length + separator >= capacity - size)
            return false;
        if (separator)
            text[size++] = ' ';
        memcpy(text + size, name, length + 1U);
        size += length;
    }
    return true;
}

static bool test_full_domain(void) {
    /* Independent complete table: three axes, two kernels, 24 requests. */
    static const char* const expected_names[] = {
        "FOG_ON",
        "FOG_ON SHARPEN",
        "FOG_OFF",
        "FOG_OFF SHARPEN",
        "QUALITY_LOW FOG_ON",
        "QUALITY_LOW FOG_ON SHARPEN",
        "QUALITY_LOW FOG_OFF",
        "QUALITY_LOW FOG_OFF SHARPEN",
        "QUALITY_HIGH FOG_ON",
        "QUALITY_HIGH FOG_ON SHARPEN",
        "QUALITY_HIGH FOG_OFF",
        "QUALITY_HIGH FOG_OFF SHARPEN",
    };
    static const uint64_t selected_requirements[] = {
        0x40U, 0xC0U, 0x00U, 0x80U, 0x50U, 0xD0U, 0x10U, 0x90U, 0x60U, 0xE0U, 0x20U, 0xA0U,
    };
    char global0[] = "_ QUALITY_LOW QUALITY_HIGH";
    const char* global[] = {global0, "FOG_ON FOG_OFF"};
    const char* local[] = {"__ SHARPEN"};
    char* platform[] = {"PLATFORM_FEATURE"};
    char* disabled[] = {"STRIPPED_FEATURE"};
    const UnityCompilerComputePreprocessMacro macros[] = {{"KERNEL_MODE", "1"},
                                                          {"BASE_INDEX", "0xFF"}};
    const UnityCompilerComputePreprocessedKernel kernels[] = {
        {"CopyValues", macros, 2U},
        {"UpdateValues", NULL, 0U},
    };
    const UnityCompilerComputeConditionalRequirement conditionals[] = {
        {"QUALITY_LOW", 0x10U},
        {"QUALITY_HIGH", 0x20U},
        {"FOG_ON", 0x40U},
        {"SHARPEN", 0x80U},
        {"PLATFORM_FEATURE", UINT64_C(1) << 48U},
        {"STRIPPED_FEATURE", UINT64_C(1) << 62U},
    };
    const char* dependencies[] = {"ControlledInclude.hlsl"};
    UnityCompilerComputePreprocessRequest request = base_request();
    request.platform_keywords = platform;
    request.platform_keyword_count = 1;
    request.disabled_keywords = disabled;
    request.disabled_keyword_count = 1;
    UnityCompilerComputePreprocessResult result = base_result(kernels, 2U);
    result.user_global = (UnityCompilerComputeKeywordLines){global, 2U};
    result.user_local = (UnityCompilerComputeKeywordLines){local, 1U};
    result.conditional_requirements = conditionals;
    result.conditional_requirement_count = 6U;
    result.dependencies = dependencies;
    result.dependency_count = 1U;
    result.requirements = (UINT64_C(1) << 63U) | UINT64_C(0x4001);
    UnityComputeDomainContext context = base_context(&request);
    context.max_variant_count = 12U;
    context.max_kernel_state_count = 24U;
    UnityComputeDomain* domain = NULL;
    UnityComputeDomainDiagnostic diagnostic;
    CHECK(unity_compute_domain_create(&result, &context, &domain, &diagnostic) ==
          UNITY_COMPUTE_DOMAIN_OK);
    CHECK(diagnostic.status == UNITY_COMPUTE_DOMAIN_OK && diagnostic.family_index == SIZE_MAX);
    CHECK(unity_compute_domain_variant_count(domain) == 12U);
    CHECK(unity_compute_domain_state_count(domain) == 24U);
    CHECK(unity_compute_domain_family_count(domain, UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL) == 2U);
    CHECK(unity_compute_domain_family_count(domain, UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL) == 1U);
    CHECK(unity_compute_domain_family_count(domain, UNITY_COMPUTE_DOMAIN_SCOPE_NONE) == 0U);
    CHECK(strcmp(global0, "_ QUALITY_LOW QUALITY_HIGH") == 0);
    UnityComputeDomainFamily family;
    CHECK(unity_compute_domain_family_at(domain, UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL, 0U, &family) ==
          UNITY_COMPUTE_DOMAIN_OK);
    CHECK(family.raw_line == global0 && family.choice_count == 3U && !family.choices[0]);
    CHECK(strcmp(family.choices[1], "QUALITY_LOW") == 0 &&
          strcmp(family.choices[2], "QUALITY_HIGH") == 0);
    CHECK(unity_compute_domain_family_at(domain, UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL, 0U, &family) ==
          UNITY_COMPUTE_DOMAIN_OK);
    CHECK(family.raw_line == local[0] && family.choice_count == 2U && !family.choices[0]);
    CHECK(strcmp(family.choices[1], "SHARPEN") == 0);

    for (size_t index = 0U; index < 24U; ++index) {
        size_t required = SIZE_MAX;
        CHECK(unity_compute_domain_state_at(domain, index, NULL, 0U, &required, NULL,
                                            &diagnostic) == UNITY_COMPUTE_DOMAIN_OK);
        const char* keywords[3] = {NULL, NULL, NULL};
        UnityComputeDomainState state;
        CHECK(unity_compute_domain_state_at(domain, index, keywords, 3U, &required, &state,
                                            &diagnostic) == UNITY_COMPUTE_DOMAIN_OK);
        CHECK(state.variant_index == index % 12U && state.kernel_index == index / 12U);
        CHECK(state.kernel == &kernels[index / 12U] && state.preprocess_result == &result &&
              state.preprocess_request == &request);
        CHECK(state.kernel->macros == kernels[index / 12U].macros);
        CHECK(state.user_keyword_count == required);
        CHECK(state.global_keyword_count + state.local_keyword_count == required);
        CHECK(state.user_keywords == keywords && state.global_keywords == keywords);
        CHECK(state.local_keyword_count == (index % 2U));
        CHECK(!state.local_keyword_count ||
              state.local_keywords == keywords + state.global_keyword_count);
        CHECK(state.requirements ==
              (result.requirements | (UINT64_C(1) << 48U) | selected_requirements[index % 12U]));
        CHECK(!(state.requirements & (UINT64_C(1) << 62U)));
        CHECK(state.preprocess_result->source == result.source &&
              state.preprocess_result->source_size == result.source_size);
        CHECK(state.preprocess_result->compilation_flags == UINT32_C(0x20000000));
        CHECK(state.preprocess_result->use_dxc_mask == UINT32_C(0x80000010));
        CHECK(state.preprocess_result->never_use_dxc_mask == UINT32_C(0x40000000));
        CHECK(state.preprocess_result->dependencies == dependencies);
        char text[128];
        CHECK(keyword_text(&state, text, sizeof(text)) &&
              strcmp(text, expected_names[index % 12U]) == 0);
    }
    CHECK(unity_compute_domain_require_api(domain, 4U) == UNITY_COMPUTE_DOMAIN_OK);
    CHECK(unity_compute_domain_require_api(domain, 31U) == UNITY_COMPUTE_DOMAIN_OK);
    CHECK(unity_compute_domain_require_api(domain, 3U) == UNITY_COMPUTE_DOMAIN_UNSUPPORTED_API);
    CHECK(unity_compute_domain_require_api(domain, 32U) == UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);

    const char* hold = "unchanged";
    const char* keyword_buffer[3] = {hold, hold, hold};
    UnityComputeDomainState state, saved;
    memset(&state, 0xA5, sizeof(state));
    memcpy(&saved, &state, sizeof(saved));
    size_t required = SIZE_MAX;
    CHECK(unity_compute_domain_state_at(domain, 5U, keyword_buffer, 2U, &required, &state,
                                        &diagnostic) == UNITY_COMPUTE_DOMAIN_BUFFER_TOO_SMALL);
    CHECK(required == 3U && memcmp(&state, &saved, sizeof(state)) == 0);
    CHECK(keyword_buffer[0] == hold && keyword_buffer[1] == hold && keyword_buffer[2] == hold);
    required = 99U;
    CHECK(unity_compute_domain_state_at(domain, 24U, keyword_buffer, 3U, &required, &state,
                                        &diagnostic) == UNITY_COMPUTE_DOMAIN_INVALID_STATE_INDEX);
    CHECK(required == 99U && memcmp(&state, &saved, sizeof(state)) == 0 &&
          keyword_buffer[0] == hold);
    CHECK(unity_compute_domain_state_at(domain, 0U, NULL, 1U, &required, &state, NULL) ==
          UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    CHECK(unity_compute_domain_family_at(domain, UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL, 1U, &family) ==
          UNITY_COMPUTE_DOMAIN_INVALID_STATE_INDEX);
    CHECK(unity_compute_domain_create(&result, &context, &domain, NULL) ==
          UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    CHECK(unity_compute_domain_state_count(domain) == 24U);
    unity_compute_domain_free(domain);
    return true;
}

static bool test_empty_tuple_and_explicit_default(void) {
    const UnityCompilerComputePreprocessedKernel kernel = {"CopyValues", NULL, 0U};
    UnityCompilerComputePreprocessRequest request = base_request();
    UnityCompilerComputePreprocessResult result = base_result(&kernel, 1U);
    UnityComputeDomainContext context = base_context(&request);
    context.max_variant_count = 1U;
    context.max_kernel_state_count = 1U;
    UnityComputeDomain* domain = NULL;
    CHECK(unity_compute_domain_create(&result, &context, &domain, NULL) == UNITY_COMPUTE_DOMAIN_OK);
    CHECK(unity_compute_domain_variant_count(domain) == 1U &&
          unity_compute_domain_state_count(domain) == 1U);
    UnityComputeDomainState state;
    size_t required = 99U;
    CHECK(unity_compute_domain_state_at(domain, 0U, NULL, 0U, &required, &state, NULL) ==
          UNITY_COMPUTE_DOMAIN_OK);
    CHECK(!required && !state.user_keywords && !state.global_keywords && !state.local_keywords);
    CHECK(state.requirements == result.requirements && state.kernel == &kernel);
    unity_compute_domain_free(domain);
    domain = NULL;

    const char* defaults[] = {"___"};
    result.user_local = (UnityCompilerComputeKeywordLines){defaults, 1U};
    CHECK(unity_compute_domain_create(&result, &context, &domain, NULL) == UNITY_COMPUTE_DOMAIN_OK);
    CHECK(unity_compute_domain_state_at(domain, 0U, NULL, 0U, &required, &state, NULL) ==
          UNITY_COMPUTE_DOMAIN_OK);
    CHECK(!required && !state.user_keywords);
    unity_compute_domain_free(domain);
    return true;
}

static bool test_fail_closed_family_and_context(void) {
    const UnityCompilerComputePreprocessedKernel kernel = {"CopyValues", NULL, 0U};
    UnityCompilerComputePreprocessRequest request = base_request();
    UnityCompilerComputePreprocessResult result = base_result(&kernel, 1U);
    UnityComputeDomainContext context = base_context(&request);
    const char* lines[2] = {"", NULL};
    result.user_global = (UnityCompilerComputeKeywordLines){lines, 1U};
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_UNSUPPORTED_EMPTY_FAMILY));
    static const char* const invalid[] = {
        " A", "A ", "A  B", "A\tB", "A\nB", "A/B", "A=1", "1A", "defined", "A /*comment*/",
    };
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(*invalid); ++index) {
        lines[0] = invalid[index];
        CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_UNSUPPORTED_FAMILY_SYNTAX));
    }
    lines[0] = "_ __";
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    lines[0] = "A A";
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    lines[0] = "_ A";
    lines[1] = "B A";
    result.user_global.line_count = 2U;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    result.user_global.line_count = 1U;
    const char* local[] = {"_ A"};
    result.user_local = (UnityCompilerComputeKeywordLines){local, 1U};
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    result.user_local.line_count = 0U;
    char* names[] = {"A", "A"};
    request.platform_keywords = names;
    request.platform_keyword_count = 1;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    request.platform_keyword_count = 0;
    request.disabled_keywords = names;
    request.disabled_keyword_count = 1;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    result.user_global.line_count = 0U;
    request.disabled_keyword_count = 2;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    request.disabled_keyword_count = 1;
    request.platform_keyword_count = 1;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD));
    request = base_request();
    UnityCompilerComputeConditionalRequirement conditional[] = {{"UNKNOWN_FEATURE", UINT64_MAX},
                                                                {"UNKNOWN_FEATURE", 1U}};
    result.conditional_requirements = conditional;
    result.conditional_requirement_count = 1U;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_CONDITIONAL_CONTEXT_UNAVAILABLE));
    const UnityCompilerComputePreprocessMacro macro = {"UNKNOWN_FEATURE", "1"};
    UnityCompilerComputePreprocessedKernel macro_kernel = {"CopyValues", &macro, 1U};
    result.kernels = &macro_kernel;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_CONDITIONAL_CONTEXT_UNAVAILABLE));
    result.kernels = &kernel;
    result.user_global.line_count = 1U;
    conditional[0].keyword = "A";
    conditional[1].keyword = "A";
    result.conditional_requirement_count = 2U;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_CONDITIONAL_CONTEXT_UNAVAILABLE));
    return true;
}

static bool test_bounds_and_invalid_records(void) {
    const UnityCompilerComputePreprocessedKernel kernels[] = {
        {"CopyValues", NULL, 0U},
        {"CopyValues", NULL, 0U},
    };
    UnityCompilerComputePreprocessRequest request = base_request();
    UnityCompilerComputePreprocessResult result = base_result(kernels, 1U);
    UnityComputeDomainContext context = base_context(&request);
    UnityCompilerComputePreprocessResult changed = result;
    changed.kernel_count = 0U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_NO_KERNELS));
    changed.kernel_count = 2U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KERNEL));
    changed.kernel_count = 1025U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    changed = result;
    changed.kernels = NULL;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    const UnityCompilerComputePreprocessMacro macros[] = {{"MODE", "1"}, {"MODE", "2"}};
    UnityCompilerComputePreprocessedKernel macro_kernel = {"CopyValues", macros, 2U};
    changed.kernels = &macro_kernel;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_KERNEL_MACRO));
    macro_kernel.macro_count = 1025U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    macro_kernel.macro_count = 1U;
    macro_kernel.macros = NULL;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    changed = result;
    changed.source_size = result.source_size - 1U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    const char embedded[] = "abc\0def";
    changed.source = embedded;
    changed.source_size = sizeof(embedded) - 1U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    changed.source_size = 512U * 1024U * 1024U + 1U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    changed = result;
    changed.user_global.line_count = 1025U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    changed.user_global.line_count = 1U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    changed = result;
    changed.conditional_requirement_count = 1U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    changed.conditional_requirement_count = 1025U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    changed = result;
    changed.dependency_count = 1U;
    CHECK(expect_rejected(&changed, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    request.platform_keyword_count = -1;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT));
    request.platform_keyword_count = 1025;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    request = base_request();
    const char* families[] = {"_ A", "_ B", "_ C", "_ D"};
    result.user_global = (UnityCompilerComputeKeywordLines){families, 4U};
    context.max_variant_count = 15U;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    context.max_variant_count = 16U;
    context.max_kernel_state_count = 15U;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    context.max_kernel_state_count = UNITY_COMPUTE_DOMAIN_MAX_STATES + 1U;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    context = base_context(&request);
    char* long_line = malloc(1024U * 1024U + 2U);
    CHECK(long_line);
    memset(long_line, 'A', 1024U * 1024U + 1U);
    long_line[1024U * 1024U + 1U] = '\0';
    const char* line[] = {long_line};
    result.user_global = (UnityCompilerComputeKeywordLines){line, 1U};
    bool rejected = expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    free(long_line);
    CHECK(rejected);
    return true;
}

static bool test_large_domain_stays_lazy(void) {
    const UnityCompilerComputePreprocessedKernel kernel = {"CopyValues", NULL, 0U};
    UnityCompilerComputePreprocessRequest request = base_request();
    UnityCompilerComputePreprocessResult result = base_result(&kernel, 1U);
    UnityComputeDomainContext context = base_context(&request);
    context.max_variant_count = UNITY_COMPUTE_DOMAIN_MAX_STATES;
    context.max_kernel_state_count = UNITY_COMPUTE_DOMAIN_MAX_STATES;
    char text[21][16];
    const char* families[21];
    for (size_t index = 0U; index < 21U; ++index) {
        int written = snprintf(text[index], sizeof(text[index]), "_ FEATURE_%zu", index);
        CHECK(written > 0 && (size_t)written < sizeof(text[index]));
        families[index] = text[index];
    }
    result.user_global = (UnityCompilerComputeKeywordLines){families, 20U};
    UnityComputeDomain* domain = NULL;
    CHECK(unity_compute_domain_create(&result, &context, &domain, NULL) == UNITY_COMPUTE_DOMAIN_OK);
    CHECK(unity_compute_domain_state_count(domain) == 1048576U);
    const char* keywords[20];
    UnityComputeDomainState state;
    size_t required = SIZE_MAX;
    CHECK(unity_compute_domain_state_at(domain, 1048575U, keywords, 20U, &required, &state, NULL) ==
          UNITY_COMPUTE_DOMAIN_OK);
    CHECK(required == 20U && state.global_keyword_count == 20U && !state.local_keyword_count);
    for (size_t index = 0U; index < 20U; ++index)
        CHECK(strcmp(keywords[index], text[index] + 2U) == 0);
    unity_compute_domain_free(domain);
    result.user_global.line_count = 21U;
    CHECK(expect_rejected(&result, &context, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED));
    return true;
}

int main(void) {
    if (!test_full_domain() || !test_empty_tuple_and_explicit_default() ||
        !test_fail_closed_family_and_context() || !test_bounds_and_invalid_records() ||
        !test_large_domain_stays_lazy())
        return 1;
    puts("native compute domain planning tests passed");
    return 0;
}
