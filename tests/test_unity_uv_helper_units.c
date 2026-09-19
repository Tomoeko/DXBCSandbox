// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_uv_helper.h"
#include "common/source_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

/* Deliberately small authored token fixtures; installed Unity includes are
 * read only by the optional live compiler check, never redistributed here. */
static const char scalar[] =
    "inline float2 UnityStereoScreenSpaceUVAdjustInternal(float2 uv, float4 scaleAndOffset) {"
    "return uv.xy * scaleAndOffset.xy + scaleAndOffset.zw;}\n";
static const char vector[] =
    "inline float4 UnityStereoScreenSpaceUVAdjustInternal(float4 uv, float4 scaleAndOffset) {"
    "return float4(UnityStereoScreenSpaceUVAdjustInternal(uv.xy, scaleAndOffset),"
    "UnityStereoScreenSpaceUVAdjustInternal(uv.zw, scaleAndOffset));}\n";
static const char probe[] =
    "float4 dxbc_unity_uv_contract_probe(float4 dxbc_uv, float4 dxbc_scale_offset) {"
    "return UnityStereoScreenSpaceUVAdjustInternal(dxbc_uv, dxbc_scale_offset);}\n";

static void expansion_fixture(StringBuilder *source) {
    sb_init(source);
    sb_append(source, scalar);
    sb_append(source, vector);
    sb_append(source, probe);
}

static bool expect_status(const StringBuilder *source, UnityUvHelperStatus expected) {
    UnityUvHelperExpansion expansion;
    memset(&expansion, 0xff, sizeof(expansion));
    const UnityUvHelperStatus status =
        unity_uv_helper_validate_expansion((const uint8_t *)source->buf, source->len, &expansion);
    if (status != expected)
        fprintf(stderr, "Expected %s, received %s\n", unity_uv_helper_status_name(expected),
                unity_uv_helper_status_name(status));
    CHECK(status == expected);
    if (status != UNITY_UV_HELPER_OK) {
        const UnityUvHelperExpansion empty = {0};
        CHECK(memcmp(&expansion, &empty, sizeof(empty)) == 0);
    }
    return true;
}

static bool replace_once(StringBuilder *source, const char *before, const char *after) {
    const char *found = strstr(source->buf, before);
    CHECK(found != NULL);
    StringBuilder changed;
    sb_init(&changed);
    sb_append_len(&changed, source->buf, (size_t)(found - source->buf));
    sb_append(&changed, after);
    sb_append(&changed, found + strlen(before));
    sb_free(source);
    *source = changed;
    return sb_ok(source);
}

static bool definitions_and_mutations(void) {
    StringBuilder source;
    expansion_fixture(&source);
    UnityUvHelperExpansion expansion;
    CHECK(unity_uv_helper_validate_expansion((const uint8_t *)source.buf, source.len, &expansion) ==
          UNITY_UV_HELPER_OK);
    CHECK(expansion.definitions[0].begin == 0);
    CHECK(expansion.definitions[0].end == strlen(scalar) - 1);
    CHECK(expansion.definitions[1].begin == strlen(scalar));
    CHECK(expansion.probe.begin == strlen(scalar) + strlen(vector));
    CHECK(expansion.probe.end == source.len - 1);
    CHECK(replace_once(&source, "return uv.xy", "return /* comment */ uv . xy"));
    CHECK(expect_status(&source, UNITY_UV_HELPER_OK));
    sb_free(&source);

    const struct {
        const char *before;
        const char *after;
        UnityUvHelperStatus status;
    } mutations[] = {
        {" * ", " + ", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {" + ", " - ", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {"scaleAndOffset.zw", "scaleAndOffset.wz", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {"float2 uv", "half2 uv", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {"inline float2", "inline half2", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {"inline float4", "precise float4", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {"(uv.zw, scaleAndOffset)", "(uv.xy, scaleAndOffset)", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {"return uv.xy", "return -uv.xy", UNITY_UV_HELPER_CHANGED_DEFINITION},
        {"dxbc_scale_offset);}", "dxbc_scale_offset.zyxw);}", UNITY_UV_HELPER_CHANGED_INVOCATION},
        {"return UnityStereoScreenSpaceUVAdjustInternal(dxbc_uv", "return AnotherHelper(dxbc_uv",
         UNITY_UV_HELPER_CHANGED_INVOCATION},
        {"return UnityStereoScreenSpaceUVAdjustInternal(dxbc_uv", "return (dxbc_uv",
         UNITY_UV_HELPER_CHANGED_INVOCATION},
        {"return uv.xy", "return 0; /* return uv.xy", UNITY_UV_HELPER_MALFORMED_EXPANSION},
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        expansion_fixture(&source);
        CHECK(replace_once(&source, mutations[i].before, mutations[i].after));
        CHECK(expect_status(&source, mutations[i].status));
        sb_free(&source);
    }
    const char *tails[] = {
        scalar,
        vector,
        probe,
        "float4 UnityStereoScreenSpaceUVAdjustInternal(float4 uv, float4 scaleAndOffset);",
        "half2 UnityStereoScreenSpaceUVAdjustInternal(half2 uv, half4 st) {return uv;}",
        "float4 dxbc_unity_uv_contract_probe(float4 x) {return x;}",
        "float4 MissingBody(",
        "}",
        "/*"};
    const UnityUvHelperStatus statuses[] = {
        UNITY_UV_HELPER_DUPLICATE_DEFINITION, UNITY_UV_HELPER_DUPLICATE_DEFINITION,
        UNITY_UV_HELPER_DUPLICATE_DEFINITION, UNITY_UV_HELPER_CHANGED_DEFINITION,
        UNITY_UV_HELPER_CHANGED_DEFINITION,   UNITY_UV_HELPER_CHANGED_INVOCATION,
        UNITY_UV_HELPER_MALFORMED_EXPANSION,  UNITY_UV_HELPER_MALFORMED_EXPANSION,
        UNITY_UV_HELPER_MALFORMED_EXPANSION};
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); ++i) {
        expansion_fixture(&source);
        sb_append(&source, tails[i]);
        CHECK(expect_status(&source, statuses[i]));
        sb_free(&source);
    }
    for (int missing = 0; missing < 3; ++missing) {
        sb_init(&source);
        if (missing != 0)
            sb_append(&source, scalar);
        if (missing != 1)
            sb_append(&source, vector);
        if (missing != 2)
            sb_append(&source, probe);
        CHECK(expect_status(&source, UNITY_UV_HELPER_MISSING_DEFINITION));
        sb_free(&source);
    }
    sb_init(&source);
    CHECK(unity_uv_helper_append_probe(&source));
    CHECK(strstr(source.buf,
                 "return UnityStereoScreenSpaceUVAdjust(dxbc_uv, dxbc_scale_offset);") != NULL);
    CHECK(expect_status(&source, UNITY_UV_HELPER_CHANGED_INVOCATION));
    sb_free(&source);
    return true;
}

static bool directives_and_limits(void) {
    const char *prefixes[] = {
        "#line 42 \"generic/include.cginc\"\n",
        "  #line 100\r\n",
        ("#pragma warning ( disable : 3205 )\n#pragma warning(disable:3568)\n"
         "#pragma warning(disable:3571)\n#pragma warning(disable:3206)\n"),
        "#define UnityStereoScreenSpaceUVAdjustInternal AnotherHelper\n",
        "#pragma pack_matrix(row_major)\n",
        "#pragma warning(disable:9999)\n",
        "#line 4u\n",
        "#line 4 trailing\n",
        "#line 4 \"unterminated\n",
        "#line 4\n#undef float4\n",
        "identifier #line 4\n",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        StringBuilder source;
        sb_init(&source);
        sb_append(&source, prefixes[i]);
        sb_append(&source, scalar);
        sb_append(&source, vector);
        sb_append(&source, probe);
        CHECK(expect_status(&source,
                            i < 3 ? UNITY_UV_HELPER_OK : UNITY_UV_HELPER_UNSUPPORTED_DIRECTIVE));
        sb_free(&source);
    }
    UnityUvHelperExpansion expansion;
    CHECK(unity_uv_helper_validate_expansion(NULL, 1, &expansion) ==
          UNITY_UV_HELPER_INVALID_ARGUMENT);
    CHECK(unity_uv_helper_validate_expansion(NULL, 0, &expansion) ==
          UNITY_UV_HELPER_MISSING_DEFINITION);
    CHECK(unity_uv_helper_validate_expansion((const uint8_t *)"",
                                             UNITY_UV_HELPER_EXPANSION_LIMIT + 1U,
                                             &expansion) == UNITY_UV_HELPER_LIMIT_EXCEEDED);
    CHECK(unity_uv_helper_validate_expansion(NULL, 0, NULL) == UNITY_UV_HELPER_INVALID_ARGUMENT);
    StringBuilder source;
    expansion_fixture(&source);
    sb_append_len(&source, "\0", 1);
    CHECK(expect_status(&source, UNITY_UV_HELPER_MALFORMED_EXPANSION));
    sb_free(&source);
    expansion_fixture(&source);
    for (size_t i = 0; i < 131072; ++i)
        sb_append(&source, ";");
    CHECK(expect_status(&source, UNITY_UV_HELPER_LIMIT_EXCEEDED));
    sb_free(&source);
    expansion_fixture(&source);
    for (size_t length = 0; length + 1 < source.len; ++length) {
        CHECK(unity_uv_helper_validate_expansion((const uint8_t *)source.buf, length, &expansion) !=
              UNITY_UV_HELPER_OK);
    }
    const size_t complete_size = source.len;
    for (size_t i = source.len; i < UNITY_UV_HELPER_EXPANSION_LIMIT; ++i)
        sb_append_char(&source, ' ');
    CHECK(sb_ok(&source) && source.len == UNITY_UV_HELPER_EXPANSION_LIMIT);
    CHECK(expect_status(&source, UNITY_UV_HELPER_OK));
    CHECK(source.len > complete_size);
    sb_free(&source);
    const char *splices[] = {"// continued \\\n", "// continued \\\r\n",
                             ("// ?"
                              "?/\n")};
    for (size_t i = 0; i < sizeof(splices) / sizeof(splices[0]); ++i) {
        sb_init(&source);
        sb_append(&source, splices[i]);
        sb_append(&source, scalar);
        sb_append(&source, vector);
        sb_append(&source, probe);
        CHECK(expect_status(&source, UNITY_UV_HELPER_MALFORMED_EXPANSION));
        sb_free(&source);
    }
    return true;
}

static bool scanner_boundaries(void) {
    const char *text = " /* hi */ float4 uv.xy + .5e-2f // hi\n \"a\\\"b\";";
    SourceScanner scanner = {(const uint8_t *)text, strlen(text), 0};
    const char *expected[] = {"float4", "uv", ".", "xy", "+", ".5e-2f", "\"a\\\"b\"", ";"};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const SourceToken token = source_scan_next(&scanner);
        CHECK(token.kind != SOURCE_TOKEN_INVALID);
        CHECK(source_token_equals(scanner.source, token, expected[i]));
    }
    CHECK(source_scan_next(&scanner).kind == SOURCE_TOKEN_END);
    const char *malformed[] = {"/*", "\"unterminated", "\\", "\"a\\"};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        scanner = (SourceScanner){(const uint8_t *)malformed[i], strlen(malformed[i]), 0};
        CHECK(source_scan_next(&scanner).kind == SOURCE_TOKEN_INVALID);
    }
    const uint8_t embedded[] = {'/', '/', 0, '\n', 'x'};
    scanner = (SourceScanner){embedded, sizeof(embedded), 0};
    CHECK(source_scan_next(&scanner).kind == SOURCE_TOKEN_INVALID);
    size_t offset = 0;
    CHECK(!source_scan_skip_trivia(NULL, 1, &offset));
    CHECK(!source_scan_skip_quoted(NULL, 1, &offset, '\"'));
    CHECK(!source_scan_skip_trivia(embedded, sizeof(embedded), NULL));
    CHECK(!source_scan_skip_quoted(embedded, sizeof(embedded), NULL, '\"'));
    offset = sizeof(embedded) + 1U;
    CHECK(!source_scan_skip_trivia(embedded, sizeof(embedded), &offset));
    CHECK(!source_scan_skip_quoted(embedded, sizeof(embedded), &offset, '\"'));
    CHECK(source_scan_next(NULL).kind == SOURCE_TOKEN_INVALID);
    scanner = (SourceScanner){NULL, 1, 0};
    CHECK(source_scan_next(&scanner).kind == SOURCE_TOKEN_INVALID);
    return true;
}

typedef enum {
    REQUEST_OK,
    REQUEST_MISSING_IDENTITY,
    REQUEST_WRONG_IDENTITY,
    REQUEST_EMPTY_CONTROLS,
    REQUEST_DRIFT,
    REQUEST_UNAVAILABLE,
    REQUEST_REJECTED,
    REQUEST_WRONG_DEFINITION
} RequestCase;

typedef struct {
    UnityCompilerBroker *broker;
    UnityCompilerSnippetCompileRequest expected;
    RequestCase mode;
    size_t digest_calls;
    size_t compiles;
    bool controls_preserved;
    uint8_t compiler_digest[32];
    uint8_t environment_digest[32];
} RequestFixture;

static bool canonical_digest(RequestFixture *fixture,
                             const UnityCompilerSnippetCompileRequest *request,
                             uint8_t digest[32]) {
    const UnityCompilerOfflineAuthority authority = {fixture->compiler_digest,
                                                     fixture->environment_digest};
    uint8_t *transcript = NULL;
    size_t size = 0;
    const bool ok = unity_compiler_broker_serialize_compile_request_with_authority(
        fixture->broker, request, &authority, &transcript, &size, digest);
    free(transcript);
    return ok;
}

static bool fake_digest(void *opaque, const UnityCompilerSnippetCompileRequest *request,
                        uint8_t digest[32]) {
    RequestFixture *fixture = opaque;
    ++fixture->digest_calls;
    return canonical_digest(fixture, request, digest);
}

static bool fake_compile(void *opaque, const UnityCompilerSnippetCompileRequest *request,
                         UnityCompilerBinaryResponse *response) {
    RequestFixture *fixture = opaque;
    ++fixture->compiles;
    const UnityCompilerSnippetCompileRequest *expected = &fixture->expected;
    fixture->controls_preserved =
        request->preprocess_only && request->source_directory == expected->source_directory &&
        request->source_basename == expected->source_basename &&
        request->pass_name == expected->pass_name &&
        request->caching_preprocessor == expected->caching_preprocessor &&
        request->strip_line_directives == expected->strip_line_directives &&
        request->build_platform == expected->build_platform &&
        request->render_state_length == expected->render_state_length &&
        request->variant_keywords == expected->variant_keywords &&
        request->variant_keyword_count == expected->variant_keyword_count &&
        request->user_keywords == expected->user_keywords &&
        request->user_keyword_count == expected->user_keyword_count &&
        request->disabled_keywords == expected->disabled_keywords &&
        request->disabled_keyword_count == expected->disabled_keyword_count &&
        request->compiler_flags == expected->compiler_flags &&
        request->shader_type == expected->shader_type && request->platform == expected->platform &&
        request->requirements == expected->requirements &&
        request->program_mask == expected->program_mask &&
        request->program_start == expected->program_start &&
        request->contract == expected->contract &&
        !strncmp(request->snippet_source, expected->snippet_source,
                 strlen(expected->snippet_source)) &&
        strstr(request->snippet_source,
               "return UnityStereoScreenSpaceUVAdjust(dxbc_uv, dxbc_scale_offset);");
    CHECK(fixture->controls_preserved);
    unity_compiler_binary_response_init(response);
    if (fixture->mode == REQUEST_UNAVAILABLE)
        return false;
    response->status.availability = UNITY_COMPILER_RESPONSE_AVAILABLE;
    response->status.compiler_success = fixture->mode != REQUEST_REJECTED;
    response->status.from_cache = true;
    response->has_request_identity = fixture->mode != REQUEST_MISSING_IDENTITY;
    CHECK(canonical_digest(fixture, request, response->request_digest));
    UnityCompilerSnippetCompileRequest controls = *request;
    controls.snippet_source = "";
    CHECK(canonical_digest(fixture, &controls, response->controls_digest));
    if (fixture->mode == REQUEST_EMPTY_CONTROLS)
        memset(response->controls_digest, 0, 32);
    if (fixture->mode == REQUEST_WRONG_IDENTITY)
        response->request_digest[0] ^= 1;
    if (fixture->mode == REQUEST_DRIFT)
        fixture->environment_digest[0] ^= 1;
    StringBuilder source;
    expansion_fixture(&source);
    if (fixture->mode == REQUEST_WRONG_DEFINITION)
        CHECK(replace_once(&source, "float2 uv", "half2 uv"));
    response->data = (uint8_t *)source.buf;
    response->size = source.len;
    return true;
}

static bool typed_request_authority(void) {
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(".", ".");
    CHECK(broker != NULL);
    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    char *platform[] = {"SHADER_API_D3D11"};
    char *enabled[] = {"UNITY_SINGLE_PASS_STEREO"};
    char *disabled[] = {"UNSELECTED_KEYWORD"};
    UnityCompilerSnippetCompileRequest request = {
        .snippet_source = "#include \"UnityCG.cginc\"\nfloat4 main():SV_Target{return 0;}\n",
        .source_directory = "Assets",
        .source_basename = "Controlled.shader",
        .pass_name = "PASS",
        .caching_preprocessor = false,
        .strip_line_directives = true,
        .build_platform = 19,
        .render_state_length = 7,
        .variant_keywords = platform,
        .variant_keyword_count = 1,
        .user_keywords = enabled,
        .user_keyword_count = 1,
        .disabled_keywords = disabled,
        .disabled_keyword_count = 1,
        .compiler_flags = 0x918aU,
        .shader_type = 1,
        .platform = 4,
        .requirements = UINT64_C(1) << 40,
        .program_mask = 6,
        .program_start = 7,
        .contract = &contract};
    const UnityUvHelperStatus statuses[] = {UNITY_UV_HELPER_OK,
                                            UNITY_UV_HELPER_AUTHORITY_MISMATCH,
                                            UNITY_UV_HELPER_AUTHORITY_MISMATCH,
                                            UNITY_UV_HELPER_AUTHORITY_MISMATCH,
                                            UNITY_UV_HELPER_AUTHORITY_MISMATCH,
                                            UNITY_UV_HELPER_COMPILER_UNAVAILABLE,
                                            UNITY_UV_HELPER_COMPILER_REJECTED,
                                            UNITY_UV_HELPER_CHANGED_DEFINITION};
    for (int stage = 0; stage < 2; ++stage)
        for (int stereo = 0; stereo < 2; ++stereo) {
            request.shader_type = stage;
            request.user_keyword_count = stereo;
            for (size_t mode = 0; mode < sizeof(statuses) / sizeof(statuses[0]); ++mode) {
                RequestFixture fixture = {
                    .broker = broker, .expected = request, .mode = (RequestCase)mode};
                memset(fixture.compiler_digest, 0x35, 32);
                memset(fixture.environment_digest, 0x53, 32);
                const UnityUvHelperServices services = {fake_digest, fake_compile, &fixture};
                UnityCompilerBinaryResponse response;
                UnityUvHelperEvidence evidence;
                const UnityUvHelperStatus status = unity_uv_helper_inspect_request(
                    NULL, &request, &services, &response, &evidence);
                CHECK(status == statuses[mode]);
                CHECK(fixture.compiles == 1 && fixture.controls_preserved);
                CHECK(fixture.digest_calls >= 2 && fixture.digest_calls <= 4);
                if (status == UNITY_UV_HELPER_OK) {
                    uint8_t expected[32];
                    CHECK(canonical_digest(&fixture, &request, expected));
                    CHECK(!memcmp(expected, evidence.compile_request_digest, 32));
                    CHECK(!memcmp(response.request_digest, evidence.preprocess_request_digest, 32));
                    CHECK(response.status.from_cache);
                } else {
                    const UnityUvHelperEvidence empty = {0};
                    CHECK(!memcmp(&evidence, &empty, sizeof(empty)));
                }
                unity_compiler_binary_response_free(&response);
            }
        }
    UnityCompilerBinaryResponse response;
    UnityUvHelperEvidence evidence;
    for (int i = 0; i < 3; ++i) {
        UnityCompilerSnippetCompileRequest invalid = request;
        if (i == 0)
            invalid.preprocess_only = true;
        if (i == 1)
            invalid.platform = 15;
        if (i == 2)
            invalid.shader_type = 5;
        CHECK(unity_uv_helper_inspect_request(broker, &invalid, NULL, &response, &evidence) ==
              UNITY_UV_HELPER_INVALID_ARGUMENT);
        unity_compiler_binary_response_free(&response);
    }
    unity_compiler_snippet_contract_free(&contract);
    unity_compiler_broker_destroy(broker);
    return true;
}

int main(void) {
    if (!scanner_boundaries() || !definitions_and_mutations() || !directives_and_limits() ||
        !typed_request_authority())
        return 1;
    puts("Unity UV helper contract tests passed");
    return 0;
}
