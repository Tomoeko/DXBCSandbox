// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_uv_helper.h"
#include "compiler/unity_compile_authority.h"
#include "compiler/unity_compile_profile.h"
#include "common/sha256.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Explicit profile/root inputs keep this optional live check reproducible.
 * It establishes this authored V/F fixture's helper-expansion and byte gates,
 * not a serialized ShaderLab, runtime, pixel or whole-shader certificate. */
static const char source[] = "Shader \"Hidden/UVHelperContract\" {\n"
                             "SubShader {\nPass {\n%sPROGRAM\n"
                             "#pragma vertex vert\n#pragma fragment frag\n#pragma target 4.0\n"
                             "#pragma multi_compile __ UNITY_SINGLE_PASS_STEREO\n"
                             "#include \"UnityCG.cginc\"\n"
                             "float4 vert(float4 uv:POSITION,float4 st:TEXCOORD1):SV_POSITION {"
                             "return UnityStereoScreenSpaceUVAdjust(uv,st);}\n"
                             "float4 frag(float4 uv:TEXCOORD0,float4 st:TEXCOORD1):SV_Target {"
                             "return UnityStereoScreenSpaceUVAdjust(uv,st);}\n"
                             "END%s\n}}}\n";

static bool explicit_baseline(const char *snippet, StringBuilder *baseline) {
    const char call[] = "UnityStereoScreenSpaceUVAdjust(uv,st)";
    const char *cursor = snippet;
    size_t replacements = 0;
    for (const char *found; (found = strstr(cursor, call)) != NULL;) {
        sb_append_len(baseline, cursor, (size_t)(found - cursor));
        sb_append(baseline, "(uv * st.xyxy + st.zwzw)");
        cursor = found + strlen(call);
        ++replacements;
    }
    sb_append(baseline, cursor);
    return sb_ok(baseline) && replacements == 2;
}

static bool clean_compile(UnityCompilerBroker *broker,
                          const UnityCompilerSnippetCompileRequest *request,
                          UnityCompilerBinaryResponse *response) {
    return unity_compiler_broker_compile_contract_response(broker, request, response) &&
           unity_compiler_response_status_is_clean_success(&response->status) &&
           response->has_request_identity;
}

static void print_digest(const char *name, const uint8_t digest[32]) {
    char hex[65];
    common_sha256_digest_to_hex(digest, hex);
    printf(" %s=%s", name, hex);
}

static bool check_mutations(UnityCompilerBroker *broker,
                            const UnityCompilerSnippetCompileRequest *original,
                            const char *baseline_source, const DXBCContainerView *baseline,
                            const uint8_t controls[32]) {
    static const char expression[] = "(uv * st.xyxy + st.zwzw)";
    const struct {
        const char *name;
        const char *replacement;
        bool equal;
    } cases[] = {
        {"offset-lanes", "(uv * st.xyxy + st.wzwz)", false},
        {"sign", "(uv * st.xyxy - st.zwzw)", false},
        {"bit-cast", "float4(asuint(uv * st.xyxy + st.zwzw))", false},
        /* First establish a same-byte zero control, then change exactly that
         * literal. No reference fixture or comparison boundary is rewritten. */
        {"constant-zero-control", "(uv * st.xyxy + st.zwzw + 0.0)", true},
        {"constant-one", "(uv * st.xyxy + st.zwzw + 1.0)", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        StringBuilder source;
        sb_init(&source);
        const char *cursor = baseline_source;
        size_t replacements = 0;
        for (const char *found; (found = strstr(cursor, expression)) != NULL;) {
            sb_append_len(&source, cursor, (size_t)(found - cursor));
            sb_append(&source, cases[i].replacement);
            cursor = found + strlen(expression);
            ++replacements;
        }
        sb_append(&source, cursor);
        UnityCompilerSnippetCompileRequest request = *original;
        request.snippet_source = source.buf;
        UnityCompilerBinaryResponse response;
        unity_compiler_binary_response_init(&response);
        DXBCContainerView actual;
        bool valid = sb_ok(&source) && replacements == 2 &&
                     clean_compile(broker, &request, &response) &&
                     memcmp(controls, response.controls_digest, 32) == 0 &&
                     dxbc_container_view_first(response.data, response.size, &actual);
        DXBCCompareResult report;
        DXBCCompareStatus comparison = DXBC_COMPARE_INVALID_ARGUMENT;
        if (valid) {
            comparison = dxbc_compare_exact(baseline->data, baseline->size, actual.data,
                                             actual.size, &report);
            valid = comparison != DXBC_COMPARE_INVALID_ARGUMENT &&
                    comparison != DXBC_COMPARE_EXPECTED_INVALID &&
                    comparison != DXBC_COMPARE_ACTUAL_INVALID &&
                    ((comparison == DXBC_COMPARE_EQUAL) == cases[i].equal);
        }
        printf(" %s=%s", cases[i].name, valid ? dxbc_compare_status_name(comparison) : "failed");
        unity_compiler_binary_response_free(&response);
        sb_free(&source);
        if (!valid)
            return false;
    }
    return true;
}

static bool check_state(UnityCompilerBroker *broker, const PreprocessedSnippet *snippet,
                        const UnityCompileProfile *profile, const char *directory,
                        const char *baseline_source, int stage, bool stereo) {
    const char *keywords[] = {"UNITY_SINGLE_PASS_STEREO"};
    const int indices[] = {0};
    const UnityCompileAuthorityInput input = {
        .contract = &snippet->contract,
        .keyword_names = keywords,
        .keyword_name_count = 1,
        .global_keyword_indices = indices,
        .global_keyword_index_count = stereo ? 1 : 0,
        .compiler_program = stage,
        .pass_type = 0,
        .platform_capabilities = {true, profile->d3d11_capabilities},
    };
    UnityCompileAuthority authority;
    unity_compile_authority_init(&authority);
    UnityCompilerBinaryResponse expanded, candidate, baseline;
    unity_compiler_binary_response_init(&expanded);
    unity_compiler_binary_response_init(&candidate);
    unity_compiler_binary_response_init(&baseline);
    bool success = false;
    if (unity_compile_authority_build(&input, &authority) != UNITY_COMPILE_AUTHORITY_OK)
        goto done;
    UnityCompilerSnippetCompileRequest request = {
        .snippet_source = snippet->source,
        .source_directory = directory,
        .source_basename = "UVHelperContract.shader",
        .pass_name = "",
        .caching_preprocessor = true,
        .build_platform = profile->build_platform,
        .variant_keywords = authority.platform_keywords,
        .variant_keyword_count = authority.platform_keyword_count,
        .user_keywords = authority.user_keywords,
        .user_keyword_count = authority.user_keyword_count,
        .disabled_keywords = authority.disabled_keywords,
        .disabled_keyword_count = authority.disabled_keyword_count,
        .compiler_flags = authority.compiler_flags,
        .shader_type = stage,
        .platform = 4,
        .requirements = authority.requirements,
        .program_mask = (int32_t)snippet->contract.program_types_mask,
        .program_start = snippet->contract.start_line,
        .contract = &snippet->contract,
    };
    UnityUvHelperEvidence evidence;
    const UnityUvHelperStatus status =
        unity_uv_helper_inspect_request(broker, &request, NULL, &expanded, &evidence);
    printf("language=%d stage=%d stereo=%d flags=%u helper=%s", snippet->contract.language, stage,
           stereo, request.compiler_flags, unity_uv_helper_status_name(status));
    if (status != UNITY_UV_HELPER_OK || !clean_compile(broker, &request, &candidate) ||
        memcmp(candidate.request_digest, evidence.compile_request_digest, 32))
        goto done;
    request.snippet_source = baseline_source;
    if (!clean_compile(broker, &request, &baseline) ||
        memcmp(candidate.controls_digest, baseline.controls_digest, 32))
        goto done;
    DXBCContainerView candidate_view, baseline_view;
    DXBCCompareResult comparison;
    if (!dxbc_container_view_first(candidate.data, candidate.size, &candidate_view) ||
        !dxbc_container_view_first(baseline.data, baseline.size, &baseline_view) ||
        dxbc_compare_exact(baseline_view.data, baseline_view.size, candidate_view.data,
                           candidate_view.size, &comparison) != DXBC_COMPARE_EQUAL)
        goto done;
    if (!check_mutations(broker, &request, baseline_source, &baseline_view, candidate.controls_digest))
        goto done;
    print_digest("compile", evidence.compile_request_digest);
    print_digest("preprocess", evidence.preprocess_request_digest);
    print_digest("expansion", evidence.expansion.expansion_digest);
    print_digest("controls", candidate.controls_digest);
    printf(" exact_bytes=%zu helper_cache=%d candidate_cache=%d baseline_cache=%d",
           candidate_view.size, expanded.status.from_cache, candidate.status.from_cache,
           baseline.status.from_cache);
    success = true;
done:
    printf(" result=%s\n", success ? "pass" : "fail");
    unity_compiler_binary_response_free(&baseline);
    unity_compiler_binary_response_free(&candidate);
    unity_compiler_binary_response_free(&expanded);
    unity_compile_authority_free(&authority);
    return success;
}

static bool check_language(UnityCompilerBroker *broker, const UnityCompileProfile *profile,
                           const char *directory, bool hlsl) {
    StringBuilder shader;
    sb_init(&shader);
    sb_appendf(&shader, source, hlsl ? "HLSL" : "CG", hlsl ? "HLSL" : "CG");
    if (!sb_ok(&shader)) {
        sb_free(&shader);
        return false;
    }
    const UnityCompilerShaderPreprocessRequest request = {
        .source = shader.buf,
        .source_directory = directory,
        .shader_name = "Hidden/UVHelperContract",
        .caching_preprocessor = true,
        .build_platform = profile->build_platform,
        .valid_apis = profile->valid_apis,
    };
    UnityCompilerPreprocessResponse response;
    unity_compiler_preprocess_response_init(&response);
    StringBuilder baseline;
    sb_init(&baseline);
    bool success =
        unity_compiler_broker_preprocess_contract_response(broker, &request, &response) &&
        unity_compiler_response_status_is_clean_success(&response.status) &&
        response.result.snippet_count == 1 && response.result.snippets[0].has_contract &&
        response.result.snippets[0].contract.language == (hlsl ? 3 : 0);
    if (success)
        success = explicit_baseline(response.result.snippets[0].source, &baseline);
    if (success) {
        for (int stage = 0; stage < 2; ++stage)
            for (int stereo = 0; stereo < 2; ++stereo) {
                const bool passed = check_state(broker, &response.result.snippets[0], profile,
                                                directory, baseline.buf, stage, stereo != 0);
                success = passed && success;
            }
    }
    if (!success)
        fputs("UV helper live contract probe failed\n", stderr);
    sb_free(&baseline);
    unity_compiler_preprocess_response_free(&response);
    sb_free(&shader);
    return success;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s COMPILE_PROFILE PROJECT_ROOT\n", argv[0]);
        return 2;
    }
    UnityCompileProfile profile;
    if (unity_compile_profile_load(argv[1], &profile) != UNITY_COMPILE_PROFILE_OK) {
        fputs("could not load the selected compile profile\n", stderr);
        return 2;
    }
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(argv[2], NULL);
    if (!broker)
        return 2;
    bool success = true;
    for (int language = 0; language < 2; ++language)
        success = check_language(broker, &profile, argv[2], language != 0) && success;
    unity_compiler_broker_destroy(broker);
    return success ? 0 : 1;
}
