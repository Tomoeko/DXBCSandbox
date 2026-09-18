#include "compiler/unity_compiler_client.h"
#include "common/sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DXBC_PRECISION_DEFAULT_PROJECT_ROOT
#define DXBC_PRECISION_DEFAULT_PROJECT_ROOT "."
#endif

#ifndef DXBC_PRECISION_DEFAULT_INCLUDES
#define DXBC_PRECISION_DEFAULT_INCLUDES \
    ""
#endif

/*
 * Opt-in regression/proof probe.  A Unity-enabled CMake build creates the
 * `unity_precision_collision_probe` executable.  It is omitted from CTest by
 * default because it launches the installed UnityShaderCompiler; configure
 * with DXBCSANDBOX_REGISTER_LIVE_UNITY_TESTS=ON to register it.
 *
 * Unity 2021.3.35f1 expected result:
 *
 *   half/float stripped D3D fragment (426 bytes, identical):
 *     6b5b735995e589e28f06098e85207d8a5f5f62f3027c66b9d61a01ab322d0ee1
 *   half linked GLCore (1299 bytes):
 *     629be170d8e7d8ca303f8ef945542e64785a72c53813a1e72a38159f1fb1711f
 *   float linked GLCore (1297 bytes):
 *     a6f68dd0f0c96e4e894618290e9d1a71ec29425d36468d7b0264d6a8c15b4018
 *
 * The GLCore difference is `rgba16f mediump` versus `rgba32f highp`.
 * The inner DXBC starts at byte 38, contains ISGN/OSGN/SHEX/SFI0 and no
 * RDEF, and hashes identically for the pair:
 *
 *     715512d43134376d4f90a04f9250ca11b448c0a754c4e287bdbc30e72afd50b7
 *
 * The second request set deliberately maps preprocess fields into explicit
 * compileSnippet fields to exercise the lossless API.  Those mappings are an
 * experiment, not yet proof that Unity's editor uses the same values.  The
 * compatibility request set independently produces the same collision and
 * GLSL difference.  Unity's own exact wire transcript is written under
 * /tmp/usc_log_<port> by the compiler client.
 */

static const char* const kHalfShader =
    "Shader \"Hidden/DXBCSandboxPrecisionProbe\" {\n"
    "SubShader { Pass {\n"
    "HLSLPROGRAM\n"
    "#pragma target 5.0\n"
    "#pragma vertex vert\n"
    "#pragma fragment frag\n"
    "RWTexture2D<half4> _Target;\n"
    "struct Varyings { float4 position : SV_POSITION; };\n"
    "Varyings vert(uint id : SV_VertexID) { Varyings o; o.position = float4((id == 1) ? 3.0 : -1.0, (id == 2) ? 3.0 : -1.0, 0.0, 1.0); return o; }\n"
    "float4 frag(Varyings input) : SV_Target { int2 p = int2(input.position.xy); float4 v = _Target[p]; _Target[p] = v + float4(0.25, 0.5, 0.75, 1.0); return v; }\n"
    "ENDHLSL\n"
    "} } }\n";

static const char* const kFloatShader =
    "Shader \"Hidden/DXBCSandboxPrecisionProbe\" {\n"
    "SubShader { Pass {\n"
    "HLSLPROGRAM\n"
    "#pragma target 5.0\n"
    "#pragma vertex vert\n"
    "#pragma fragment frag\n"
    "RWTexture2D<float4> _Target;\n"
    "struct Varyings { float4 position : SV_POSITION; };\n"
    "Varyings vert(uint id : SV_VertexID) { Varyings o; o.position = float4((id == 1) ? 3.0 : -1.0, (id == 2) ? 3.0 : -1.0, 0.0, 1.0); return o; }\n"
    "float4 frag(Varyings input) : SV_Target { int2 p = int2(input.position.xy); float4 v = _Target[p]; _Target[p] = v + float4(0.25, 0.5, 0.75, 1.0); return v; }\n"
    "ENDHLSL\n"
    "} } }\n";

static int write_bytes(const char* path, const void* bytes, size_t size) {
    FILE* file = fopen(path, "wb");
    if (!file) return 0;
    int ok = size == 0 || fwrite(bytes, 1, size, file) == size;
    ok = fclose(file) == 0 && ok;
    return ok;
}

static void print_keywords(const char* label, char** values, int count) {
    printf("%s[%d]=", label, count);
    for (int i = 0; i < count; i++) {
        printf("%s%s", i ? " " : "", values[i]);
    }
    putchar('\n');
}

typedef struct {
    uint8_t* stripped_d3d_fragment;
    size_t stripped_d3d_fragment_size;
    uint8_t* linked_glcore;
    size_t linked_glcore_size;
} PrecisionArtifacts;

static void free_artifacts(PrecisionArtifacts* artifacts) {
    if (!artifacts) return;
    free(artifacts->stripped_d3d_fragment);
    free(artifacts->linked_glcore);
    memset(artifacts, 0, sizeof(*artifacts));
}

static int copy_artifact(uint8_t** destination, size_t* destination_size,
                         const uint8_t* source, size_t source_size) {
    if (!destination || !destination_size || *destination ||
        !source || source_size == 0) {
        return 0;
    }
    uint8_t* copy = (uint8_t*)malloc(source_size);
    if (!copy) return 0;
    memcpy(copy, source, source_size);
    *destination = copy;
    *destination_size = source_size;
    return 1;
}

static int contains_bytes(const uint8_t* haystack, size_t haystack_size,
                          const char* needle) {
    if (!haystack || !needle) return 0;
    const size_t needle_size = strlen(needle);
    if (needle_size == 0 || needle_size > haystack_size) return 0;
    for (size_t offset = 0; offset <= haystack_size - needle_size; ++offset) {
        if (memcmp(haystack + offset, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static void digest_hex(const uint8_t* bytes, size_t size, char output[65]) {
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(bytes, size, digest);
    for (size_t index = 0; index < sizeof(digest); ++index) {
        output[index * 2u] = digits[digest[index] >> 4u];
        output[index * 2u + 1u] = digits[digest[index] & 0x0fu];
    }
    output[64] = '\0';
}

static int verify_collision(const PrecisionArtifacts* half,
                            const PrecisionArtifacts* full) {
    static const char expected_d3d[] =
        "6b5b735995e589e28f06098e85207d8a5f5f62f3027c66b9d61a01ab322d0ee1";
    static const char expected_half_gl[] =
        "629be170d8e7d8ca303f8ef945542e64785a72c53813a1e72a38159f1fb1711f";
    static const char expected_full_gl[] =
        "a6f68dd0f0c96e4e894618290e9d1a71ec29425d36468d7b0264d6a8c15b4018";
    if (!half || !full || !half->stripped_d3d_fragment ||
        !full->stripped_d3d_fragment || !half->linked_glcore ||
        !full->linked_glcore ||
        half->stripped_d3d_fragment_size !=
            full->stripped_d3d_fragment_size ||
        memcmp(half->stripped_d3d_fragment, full->stripped_d3d_fragment,
               half->stripped_d3d_fragment_size) != 0 ||
        contains_bytes(half->stripped_d3d_fragment,
                       half->stripped_d3d_fragment_size, "RDEF") ||
        contains_bytes(full->stripped_d3d_fragment,
                       full->stripped_d3d_fragment_size, "RDEF") ||
        (half->linked_glcore_size == full->linked_glcore_size &&
         memcmp(half->linked_glcore, full->linked_glcore,
                half->linked_glcore_size) == 0) ||
        !contains_bytes(half->linked_glcore, half->linked_glcore_size,
                        "rgba16f") ||
        !contains_bytes(half->linked_glcore, half->linked_glcore_size,
                        "mediump") ||
        !contains_bytes(full->linked_glcore, full->linked_glcore_size,
                        "rgba32f") ||
        !contains_bytes(full->linked_glcore, full->linked_glcore_size,
                        "highp")) {
        fprintf(stderr, "precision collision invariant failed\n");
        return 0;
    }

    char d3d_digest[65];
    char half_gl_digest[65];
    char full_gl_digest[65];
    digest_hex(half->stripped_d3d_fragment,
               half->stripped_d3d_fragment_size, d3d_digest);
    digest_hex(half->linked_glcore, half->linked_glcore_size,
               half_gl_digest);
    digest_hex(full->linked_glcore, full->linked_glcore_size,
               full_gl_digest);
    printf("verified stripped-D3D collision sha256=%s\n", d3d_digest);
    printf("verified half linked-GLCore sha256=%s\n", half_gl_digest);
    printf("verified float linked-GLCore sha256=%s\n", full_gl_digest);
    if (strcmp(d3d_digest, expected_d3d) != 0 ||
        strcmp(half_gl_digest, expected_half_gl) != 0 ||
        strcmp(full_gl_digest, expected_full_gl) != 0) {
        fprintf(stderr, "Unity 2021.3.35f1 precision proof digest mismatch\n");
        return 0;
    }
    return 1;
}

static int run_one(UnityCompilerChannel* channel, const char* tag,
                   const char* shader, PrecisionArtifacts* artifacts) {
    if (!artifacts) return 0;
    PreprocessResult prep;
    if (!unity_compiler_preprocess(channel, shader,
                                   "Hidden/DXBCSandboxPrecisionProbe",
                                   &prep)) {
        fprintf(stderr, "%s: preprocess failed\n", tag);
        return 0;
    }
    printf("%s snippets=%d blob=%zu\n", tag, prep.snippet_count,
           prep.blob_len);
    char prep_path[256];
    snprintf(prep_path, sizeof(prep_path),
             "/tmp/dxbc_rw_precision_%s_prep.bin", tag);
    if (!write_bytes(prep_path, prep.blob, prep.blob_len)) {
        unity_compiler_free_preprocess(&prep);
        return 0;
    }
    if (prep.snippet_count != 1) {
        unity_compiler_free_preprocess(&prep);
        return 0;
    }

    PreprocessedSnippet* snippet = &prep.snippets[0];
    char snippet_path[256];
    snprintf(snippet_path, sizeof(snippet_path),
             "/tmp/dxbc_rw_precision_%s_snippet.hlsl", tag);
    if (!write_bytes(snippet_path, snippet->source,
                     strlen(snippet->source))) {
        unity_compiler_free_preprocess(&prep);
        return 0;
    }
    char** non_stripped_user = NULL;
    int non_stripped_user_count = 0;
    char** builtin = NULL;
    int builtin_count = 0;
    if (snippet->has_contract) {
        SnippetCompileContract* c = &snippet->contract;
        printf("%s contract id=%" PRId32 " platforms=%" PRId32
               " quality=%" PRId32 " types=%" PRIu32
               " flags=%" PRIu32 " lang=%" PRId32
               " hash=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
               " start=%" PRId32 " dxc=%" PRId32 ",%" PRId32
               " reqs=%" PRIu64 "\n",
               tag, c->snippet_id, c->platforms, c->quality_variants,
               c->program_types_mask, c->compilation_flags, c->language,
               c->source_hash[0], c->source_hash[1], c->source_hash[2],
               c->source_hash[3], c->start_line, c->use_dxc_apis,
               c->never_use_dxc_apis, c->requirements);
        non_stripped_user = c->non_stripped_user_keywords;
        non_stripped_user_count = c->non_stripped_user_keyword_count;
        builtin = c->builtin_keywords;
        builtin_count = c->builtin_keyword_count;
        print_keywords("nonStrippedUserKeywords", non_stripped_user,
                       non_stripped_user_count);
        print_keywords("builtinKeywords", builtin, builtin_count);
    }

    const int platforms[] = {4, 4, 15, 15};
    const int stages[] = {1, 0, 1, 0};
    const char* suffixes[] = {"d3d_frag", "d3d_vert", "gl_frag", "gl_vert"};
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        size_t output_size = 0;
        char* error = NULL;
        uint8_t* output = unity_compiler_compile(
            channel, snippet->source, "Hidden/DXBCSandboxPrecisionProbe",
            stages[i], platforms[i], snippet->reqs, NULL, 0,
            NULL, 0, &output_size, &error);
        printf("%s %s request: dir=Assets/Hidden/DXBCSandboxPrecisionProbe.shader basename=Hidden/DXBCSandboxPrecisionProbe pass='' caching=1 ppOnly=0 stripLines=0 buildPlatform=%d rsLen=0 flags=%u lang=0 shaderType=%d platform=%d reqs=%" PRIu64 " mask=6 start=0 bytes=%zu\n",
               tag, suffixes[i], platforms[i] == 4 ? 1 : 2,
               platforms[i] == 4 ? 0x9000U : 0U, stages[i], platforms[i],
               snippet->reqs, output_size);
        if (!output) {
            fprintf(stderr, "%s %s compile failed: %s\n", tag,
                    suffixes[i], error ? error : "unknown");
            free(error);
            ok = 0;
            continue;
        }
        char path[256];
        snprintf(path, sizeof(path), "/tmp/dxbc_rw_precision_%s_%s.bin",
                 tag, suffixes[i]);
        if (!write_bytes(path, output, output_size)) ok = 0;
        if (i == 0 &&
            !copy_artifact(&artifacts->stripped_d3d_fragment,
                           &artifacts->stripped_d3d_fragment_size,
                           output, output_size)) {
            ok = 0;
        }
        if (i == 3 &&
            !copy_artifact(&artifacts->linked_glcore,
                           &artifacts->linked_glcore_size,
                           output, output_size)) {
            ok = 0;
        }
        free(output);
        free(error);
    }
    if (snippet->has_contract) {
        const int exact_platforms[] = {4, 4, 15, 15};
        const int exact_stages[] = {1, 0, 1, 0};
        const char* exact_suffixes[] = {
            "contract_only_d3d_frag", "contract_only_d3d_vert",
            "contract_only_gl_frag", "contract_only_gl_vert"
        };
        for (int i = 0; i < 4; i++) {
            UnityCompilerSnippetCompileRequest request = {
                .snippet_source = snippet->source,
                .source_directory = "Assets/Hidden",
                .source_basename = "DXBCSandboxPrecisionProbe",
                .pass_name = "",
                .caching_preprocessor = true,
                .preprocess_only = false,
                .strip_line_directives = false,
                .build_platform = exact_platforms[i] == 4 ? 1U : 2U,
                .render_state_length = 0,
                .variant_keywords = NULL,
                .variant_keyword_count = 0,
                .user_keywords = NULL,
                .user_keyword_count = 0,
                .disabled_keywords = NULL,
                .disabled_keyword_count = 0,
                .compiler_flags = 0,
                .shader_type = exact_stages[i],
                .platform = exact_platforms[i],
                .requirements = snippet->contract.requirements,
                .program_mask = (int32_t)snippet->contract.program_types_mask,
                .program_start = snippet->contract.start_line,
                .contract = &snippet->contract,
            };
            size_t output_size = 0;
            char* error = NULL;
            uint8_t* output = unity_compiler_compile_contract(
                channel, &request, &output_size, &error);
            printf("%s %s contract-only request (not an Editor reconstruction): dir=%s basename=%s pass='' caching=1 ppOnly=0 stripLines=0 buildPlatform=%u rsLen=0 flags=%u lang=%d shaderType=%d platform=%d reqs=%" PRIu64 " mask=%d start=%d bytes=%zu\n",
                   tag, exact_suffixes[i], request.source_directory,
                   request.source_basename, request.build_platform,
                   request.compiler_flags, snippet->contract.language,
                   request.shader_type, request.platform,
                   request.requirements, request.program_mask,
                   request.program_start, output_size);
            if (!output) {
                fprintf(stderr, "%s %s compile failed: %s\n", tag,
                        exact_suffixes[i], error ? error : "unknown");
                free(error);
                ok = 0;
                continue;
            }
            char path[256];
            snprintf(path, sizeof(path),
                     "/tmp/dxbc_rw_precision_%s_%s.bin", tag,
                     exact_suffixes[i]);
            if (!write_bytes(path, output, output_size)) ok = 0;
            free(output);
            free(error);
        }
    }
    unity_compiler_free_preprocess(&prep);
    return ok;
}

int main(void) {
    UnityCompilerChannel channel = {.socket_fd = -1, .process_id = 0};
    const char* repository = getenv("DXBC_REPO_ROOT");
    if (!repository || !repository[0]) {
        repository = DXBC_PRECISION_DEFAULT_PROJECT_ROOT;
    }
    const char* includes = getenv("DXBC_UNITY_INCLUDES");
    if (!includes || !includes[0]) {
        includes = DXBC_PRECISION_DEFAULT_INCLUDES;
    }
    if (!unity_compiler_start(&channel, repository, includes)) {
        return 2;
    }
    PrecisionArtifacts half = {0};
    PrecisionArtifacts full = {0};
    int ok = run_one(&channel, "half", kHalfShader, &half) &&
             run_one(&channel, "float", kFloatShader, &full) &&
             verify_collision(&half, &full);
    unity_compiler_shutdown(&channel);
    free_artifacts(&half);
    free_artifacts(&full);
    return ok ? 0 : 1;
}
