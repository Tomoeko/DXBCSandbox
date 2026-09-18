#include "compiler/unity_compiler_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DXBC_DIAGNOSTIC_DEFAULT_PROJECT_ROOT
#define DXBC_DIAGNOSTIC_DEFAULT_PROJECT_ROOT "."
#endif

#ifndef DXBC_DIAGNOSTIC_DEFAULT_INCLUDES
#define DXBC_DIAGNOSTIC_DEFAULT_INCLUDES ""
#endif

static const char* const kShader =
    "Shader \"Hidden/DXBCSandboxDiagnosticProbe\" {\n"
    "SubShader { Pass {\n"
    "HLSLPROGRAM\n"
    "#pragma target 4.0\n"
    "#pragma vertex vert\n"
    "#pragma fragment frag\n"
    "float4 vert(float4 value : POSITION) : SV_POSITION { return value; }\n"
    "#pragma warning(default : 3206)\n"
    "float4 frag(float4 value : TEXCOORD0) : SV_Target {\n"
    "    float scalar = value;\n"
    "    return scalar.xxxx;\n"
    "}\n"
    "ENDHLSL\n"
    "} } }\n";

static void print_status(const char* operation,
                         const UnityCompilerResponseStatus* status) {
    printf("%s success=%d diagnostics=%zu actionable=%zu\n", operation,
           status->compiler_success ? 1 : 0, status->diagnostic_count,
           unity_compiler_response_status_actionable_diagnostic_count(
               status));
    for (size_t i = 0U; i < status->diagnostic_count; ++i) {
        const UnityCompilerDiagnostic* diagnostic = &status->diagnostics[i];
        printf("  err=%d,%d,%d actionable=%d file=%s message=%s\n",
               diagnostic->fields[0], diagnostic->fields[1],
               diagnostic->fields[2],
               unity_compiler_diagnostic_is_actionable(diagnostic) ? 1 : 0,
               diagnostic->file ? diagnostic->file : "",
               diagnostic->message ? diagnostic->message : "");
    }
}

static int print_session_capabilities(
    const UnityCompilerChannel* channel, uint32_t* out_valid_apis) {
    UnityCompilerSessionCapabilities capabilities;
    uint32_t valid_apis = 0U;
    if (!unity_compiler_session_capabilities_snapshot(
            channel, &capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(
            &capabilities, &valid_apis)) {
        fputs("could not retain initializeCompiler session authority\n",
              stderr);
        return 0;
    }
    printf("initializeCompiler raw_platform_mask=0x%08" PRIx32
           " valid_apis=%" PRIu32 " (0x%08" PRIx32 ")\n",
           capabilities.raw_available_platform_mask, valid_apis,
           valid_apis);
    for (size_t index = 0U;
         index < UNITY_COMPILER_PLATFORM_COUNT; ++index) {
        printf("  platform[%zu] supported_features=0x%016" PRIx64
               " version=%" PRId32 "\n",
               index, capabilities.platforms[index].supported_features,
               capabilities.platforms[index].version);
    }
    *out_valid_apis = valid_apis;
    return 1;
}

int main(int argc, char** argv) {
    const bool session_only =
        argc == 2 && strcmp(argv[1], "--session-only") == 0;
    if (argc > 1 && !session_only) {
        fprintf(stderr, "usage: %s [--session-only]\n", argv[0]);
        return 1;
    }
    UnityCompilerChannel channel = {.socket_fd = -1, .process_id = 0};
    if (!unity_compiler_start(&channel, DXBC_DIAGNOSTIC_DEFAULT_PROJECT_ROOT,
                              DXBC_DIAGNOSTIC_DEFAULT_INCLUDES)) {
        return 2;
    }
    uint32_t valid_apis = 0U;
    if (!print_session_capabilities(&channel, &valid_apis)) {
        unity_compiler_shutdown(&channel);
        return 6;
    }
    if (session_only) {
        unity_compiler_shutdown(&channel);
        return 0;
    }
    UnityCompilerShaderPreprocessRequest preprocess_request = {
        .source = kShader,
        .file_path = "Assets/Hidden/DXBCSandboxDiagnosticProbe.shader",
        .shader_name = "Hidden/DXBCSandboxDiagnosticProbe",
        .surface_only = false,
        .caching_preprocessor = true,
        .build_platform = 1U,
        .valid_apis = valid_apis,
    };
    UnityCompilerPreprocessResponse preprocess;
    unity_compiler_preprocess_response_init(&preprocess);
    if (!unity_compiler_preprocess_contract_response(
            &channel, &preprocess_request, &preprocess)) {
        unity_compiler_preprocess_response_free(&preprocess);
        unity_compiler_shutdown(&channel);
        return 3;
    }
    print_status("preprocess", &preprocess.status);
    if (!preprocess.status.compiler_success ||
        preprocess.status.diagnostic_count != 1U ||
        preprocess.status.diagnostics[0].fields[0] != 0 ||
        unity_compiler_diagnostic_is_actionable(
            &preprocess.status.diagnostics[0]) ||
        preprocess.result.snippet_count != 1 ||
        !preprocess.result.snippets[0].has_contract) {
        unity_compiler_preprocess_response_free(&preprocess);
        unity_compiler_shutdown(&channel);
        return 4;
    }
    PreprocessedSnippet* snippet = &preprocess.result.snippets[0];
    UnityCompilerSnippetCompileRequest compile_request = {
        .snippet_source = snippet->source,
        .source_directory = "Assets/Hidden",
        .source_basename = "DXBCSandboxDiagnosticProbe",
        .pass_name = "",
        .caching_preprocessor = true,
        .build_platform = 1U,
        .shader_type = 1,
        .platform = 4,
        .requirements = snippet->contract.requirements,
        .program_mask = (int32_t)snippet->contract.program_types_mask,
        .program_start = snippet->contract.start_line,
        .contract = &snippet->contract,
    };
    UnityCompilerBinaryResponse compile;
    unity_compiler_binary_response_init(&compile);
    const bool received = unity_compiler_compile_contract_response(
        &channel, &compile_request, &compile);
    if (received) print_status("compile", &compile.status);
    const bool warning_retained = received && compile.status.compiler_success &&
        compile.status.diagnostic_count != 0U &&
        unity_compiler_response_status_actionable_diagnostic_count(
            &compile.status) != 0U &&
        compile.status.diagnostics[0].message &&
        strstr(compile.status.diagnostics[0].message,
               "implicit truncation") != NULL;
    unity_compiler_binary_response_free(&compile);
    unity_compiler_preprocess_response_free(&preprocess);
    unity_compiler_shutdown(&channel);
    return warning_retained ? 0 : 5;
}
