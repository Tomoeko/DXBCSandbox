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
                             "SubShader {\nPass {\nCGPROGRAM\n"
                             "#pragma vertex vert\n#pragma fragment frag\n#pragma target 4.0\n"
                             "#pragma multi_compile __ UNITY_SINGLE_PASS_STEREO\n"
                             "#include \"UnityCG.cginc\"\n"
                             "float4 vert(float4 uv:POSITION,float4 st:TEXCOORD1):SV_POSITION {"
                             "return UnityStereoScreenSpaceUVAdjust(uv,st);}\n"
                             "float4 frag(float4 uv:TEXCOORD0,float4 st:TEXCOORD1):SV_Target {"
                             "return UnityStereoScreenSpaceUVAdjust(uv,st);}\n"
                             "ENDCG\n}}}\n";

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
    UnityCompilerBinaryResponse expanded, candidate, baseline, wrong;
    unity_compiler_binary_response_init(&expanded);
    unity_compiler_binary_response_init(&candidate);
    unity_compiler_binary_response_init(&baseline);
    unity_compiler_binary_response_init(&wrong);
    StringBuilder changed;
    sb_init(&changed);
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
    printf("stage=%d stereo=%d flags=%u helper=%s", stage, stereo, request.compiler_flags,
           unity_uv_helper_status_name(status));
    if (status != UNITY_UV_HELPER_OK || !clean_compile(broker, &request, &candidate) ||
        memcmp(candidate.request_digest, evidence.compile_request_digest, 32))
        goto done;
    request.snippet_source = baseline_source;
    if (!clean_compile(broker, &request, &baseline) ||
        memcmp(candidate.controls_digest, baseline.controls_digest, 32))
        goto done;
    DXBCContainerView candidate_view, baseline_view, wrong_view;
    DXBCCompareResult comparison;
    if (!dxbc_container_view_first(candidate.data, candidate.size, &candidate_view) ||
        !dxbc_container_view_first(baseline.data, baseline.size, &baseline_view) ||
        dxbc_compare_exact(baseline_view.data, baseline_view.size, candidate_view.data,
                           candidate_view.size, &comparison) != DXBC_COMPARE_EQUAL)
        goto done;
    /* Change both stage offsets honestly; at least this lane mutation must
     * fail exact production under the otherwise identical request controls. */
    sb_append(&changed, baseline_source);
    if (!sb_ok(&changed))
        goto done;
    size_t mutations = 0;
    for (char *at = changed.buf; (at = strstr(at, "st.zwzw")) != NULL; at += 7) {
        memcpy(at + 3, "wzwz", 4);
        ++mutations;
    }
    request.snippet_source = changed.buf;
    if (mutations != 2 || !clean_compile(broker, &request, &wrong) ||
        memcmp(candidate.controls_digest, wrong.controls_digest, 32) ||
        !dxbc_container_view_first(wrong.data, wrong.size, &wrong_view))
        goto done;
    const DXBCCompareStatus wrong_status = dxbc_compare_exact(
        baseline_view.data, baseline_view.size, wrong_view.data, wrong_view.size, &comparison);
    if (wrong_status == DXBC_COMPARE_EQUAL || wrong_status == DXBC_COMPARE_EXPECTED_INVALID ||
        wrong_status == DXBC_COMPARE_ACTUAL_INVALID ||
        wrong_status == DXBC_COMPARE_INVALID_ARGUMENT)
        goto done;
    print_digest("compile", evidence.compile_request_digest);
    print_digest("preprocess", evidence.preprocess_request_digest);
    print_digest("expansion", evidence.expansion.expansion_digest);
    print_digest("controls", candidate.controls_digest);
    printf(" exact_bytes=%zu mutation=%s", candidate_view.size,
           dxbc_compare_status_name(wrong_status));
    success = true;
done:
    printf(" result=%s\n", success ? "pass" : "fail");
    sb_free(&changed);
    unity_compiler_binary_response_free(&wrong);
    unity_compiler_binary_response_free(&baseline);
    unity_compiler_binary_response_free(&candidate);
    unity_compiler_binary_response_free(&expanded);
    unity_compile_authority_free(&authority);
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
    const UnityCompilerShaderPreprocessRequest request = {
        .source = source,
        .source_directory = argv[2],
        .shader_name = "Hidden/UVHelperContract",
        .caching_preprocessor = true,
        .build_platform = profile.build_platform,
        .valid_apis = profile.valid_apis,
    };
    UnityCompilerPreprocessResponse response;
    unity_compiler_preprocess_response_init(&response);
    StringBuilder baseline;
    sb_init(&baseline);
    bool success =
        unity_compiler_broker_preprocess_contract_response(broker, &request, &response) &&
        unity_compiler_response_status_is_clean_success(&response.status) &&
        response.result.snippet_count == 1 && response.result.snippets[0].has_contract;
    if (success)
        success = explicit_baseline(response.result.snippets[0].source, &baseline);
    if (success) {
        for (int stage = 0; stage < 2; ++stage)
            for (int stereo = 0; stereo < 2; ++stereo) {
                const bool passed = check_state(broker, &response.result.snippets[0], &profile,
                                                argv[2], baseline.buf, stage, stereo != 0);
                success = passed && success;
            }
    }
    if (!success)
        fputs("UV helper live contract probe failed\n", stderr);
    sb_free(&baseline);
    unity_compiler_preprocess_response_free(&response);
    unity_compiler_broker_destroy(broker);
    return success ? 0 : 1;
}
