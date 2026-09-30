// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_hlsl_expansion.h"
#include "common/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #value); return false; } } while (0)

typedef enum { AVAILABLE, UNAVAILABLE, REJECTED, NO_IDENTITY, WRONG_REQUEST, WRONG_CONTROLS,
               ENVIRONMENT_DRIFT, COMPILER_DRIFT, OVERSIZED, MISSING_BYTES } ResponseCase;

typedef struct {
    UnityCompilerBroker *broker;
    UnityCompilerSnippetCompileRequest expected;
    ResponseCase mode;
    uint8_t compiler[32], environment[32];
    size_t compiles, digests;
    bool plain_source;
} Fixture;

static const char expansion[] = "cbuffer Data {float4 value;};\nfloat4 entry():SV_Target{return value;}\n";

static bool canonical(Fixture *fixture, const UnityCompilerSnippetCompileRequest *request, uint8_t digest[32]) {
    const UnityCompilerOfflineAuthority authority = {fixture->compiler, fixture->environment};
    uint8_t *transcript = NULL;
    size_t size = 0;
    const bool ok = unity_compiler_broker_serialize_compile_request_with_authority(
        fixture->broker, request, &authority, &transcript, &size, digest);
    free(transcript);
    return ok;
}

static bool observe_digest(void *context, const UnityCompilerSnippetCompileRequest *request, uint8_t digest[32]) {
    Fixture *fixture = context;
    ++fixture->digests;
    return canonical(fixture, request, digest);
}

static bool observe_compile(void *context, const UnityCompilerSnippetCompileRequest *request,
                            UnityCompilerBinaryResponse *response) {
    Fixture *fixture = context;
    ++fixture->compiles;
    UnityCompilerSnippetCompileRequest expected = fixture->expected;
    expected.preprocess_only = true;
    expected.snippet_source = request->snippet_source;
    uint8_t actual[32], same[32];
    CHECK(canonical(fixture, request, actual) && canonical(fixture, &expected, same));
    CHECK(!memcmp(actual, same, 32));
    if (fixture->plain_source) CHECK(request->snippet_source == fixture->expected.snippet_source);
    else {
        const size_t original = strlen(fixture->expected.snippet_source);
        CHECK(!strncmp(request->snippet_source, fixture->expected.snippet_source, original));
        CHECK(!strcmp(request->snippet_source + original, "\nfloat probe(){return 1;}\n"));
    }
    if (fixture->mode == UNAVAILABLE) return false;
    response->status.availability = UNITY_COMPILER_RESPONSE_AVAILABLE;
    response->status.compiler_success = fixture->mode != REJECTED;
    response->has_request_identity = fixture->mode != NO_IDENTITY;
    CHECK(canonical(fixture, request, response->request_digest));
    UnityCompilerSnippetCompileRequest controls = *request;
    controls.snippet_source = "";
    CHECK(canonical(fixture, &controls, response->controls_digest));
    if (fixture->mode == WRONG_REQUEST) response->request_digest[0] ^= 1;
    if (fixture->mode == WRONG_CONTROLS) response->controls_digest[0] ^= 1;
    if (fixture->mode == ENVIRONMENT_DRIFT) fixture->environment[0] ^= 1;
    if (fixture->mode == COMPILER_DRIFT) fixture->compiler[0] ^= 1;
    response->size = sizeof(expansion) - 1;
    if (fixture->mode != MISSING_BYTES) {
        response->data = malloc(sizeof(expansion));
        CHECK(response->data);
        memcpy(response->data, expansion, sizeof(expansion));
    }
    if (fixture->mode == OVERSIZED) response->size = UNITY_HLSL_EXPANSION_BYTE_LIMIT + 1u;
    return true;
}

static bool request_authority(void) {
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(".", ".");
    CHECK(broker);
    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    char *platform[] = {"SHADER_API_D3D11"}, *enabled[] = {"ENABLED"}, *disabled[] = {"DISABLED"};
    UnityCompilerSnippetCompileRequest request = {.snippet_source = "#include \"generic.cginc\"\nfloat4 entry():SV_Target{return 0;}\n",
        .source_directory = "Assets", .source_basename = "Generic.shader", .pass_name = "PASS",
        .build_platform = 19, .render_state_length = 7, .variant_keywords = platform, .variant_keyword_count = 1,
        .user_keywords = enabled, .user_keyword_count = 1, .disabled_keywords = disabled, .disabled_keyword_count = 1,
        .compiler_flags = 0x918aU, .platform = 4, .requirements = UINT64_C(1) << 40,
        .program_mask = 6, .program_start = 7, .contract = &contract};
    const UnityHlslExpansionStatus expected[] = {UNITY_HLSL_EXPANSION_OK, UNITY_HLSL_EXPANSION_COMPILER_UNAVAILABLE,
        UNITY_HLSL_EXPANSION_COMPILER_REJECTED, UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH,
        UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH, UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH,
        UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH, UNITY_HLSL_EXPANSION_AUTHORITY_MISMATCH,
        UNITY_HLSL_EXPANSION_LIMIT, UNITY_HLSL_EXPANSION_MALFORMED};
    for (int stage = 0; stage < 5; ++stage) for (int language = 0; language < 2; ++language)
        for (int plain = 0; plain < 2; ++plain) for (size_t mode = 0; mode < sizeof(expected) / sizeof(expected[0]); ++mode) {
            contract.language = language ? 3 : 0;
            request.shader_type = stage;
            request.caching_preprocessor = plain != 0;
            request.strip_line_directives = plain == 0;
            Fixture fixture = {.broker = broker, .expected = request, .mode = (ResponseCase)mode, .plain_source = plain != 0};
            memset(fixture.compiler, 0x35, 32); memset(fixture.environment, 0x53, 32);
            const UnityHlslExpansionServices services = {observe_digest, observe_compile, &fixture};
            UnityCompilerBinaryResponse response;
            UnityHlslExpansionEvidence evidence;
            memset(&evidence, 0xff, sizeof(evidence));
            CHECK(unity_hlsl_expansion_inspect_request(NULL, &request, plain ? NULL : "\nfloat probe(){return 1;}\n",
                &services, &response, &evidence) == expected[mode]);
            CHECK(fixture.compiles == 1 && fixture.digests >= 3 && fixture.digests <= 6);
            if (mode == AVAILABLE) {
                uint8_t digest[32];
                CHECK(canonical(&fixture, &request, digest) && !memcmp(digest, evidence.compile_request_digest, 32));
                common_sha256(expansion, sizeof(expansion) - 1, digest);
                CHECK(!memcmp(digest, evidence.expansion_digest, 32));
                CHECK(!memcmp(response.request_digest, evidence.preprocess_request_digest, 32));
                CHECK(!memcmp(response.controls_digest, evidence.preprocess_controls_digest, 32));
            } else { const UnityHlslExpansionEvidence empty = {0}; CHECK(!memcmp(&evidence, &empty, sizeof(empty))); }
            unity_compiler_binary_response_free(&response);
        }
    UnityCompilerBinaryResponse response;
    UnityHlslExpansionEvidence evidence;
    for (unsigned mode = 0; mode < 5; ++mode) {
        UnityCompilerSnippetCompileRequest invalid = request;
        if (mode == 0) invalid.preprocess_only = true;
        if (mode == 1) invalid.shader_type = 5;
        if (mode == 2) invalid.platform = 8;
        if (mode == 3) invalid.contract = NULL;
        CHECK(unity_hlsl_expansion_inspect_request(broker, &invalid, mode == 4 ? "invalid probe" : NULL, NULL,
            &response, &evidence) == UNITY_HLSL_EXPANSION_INVALID_ARGUMENT);
        unity_compiler_binary_response_free(&response);
    }
    unity_compiler_snippet_contract_free(&contract);
    unity_compiler_broker_destroy(broker);
    return true;
}

int main(void) {
    if (!request_authority()) return 1;
    puts("Expanded HLSL request authority tests passed");
    return 0;
}
