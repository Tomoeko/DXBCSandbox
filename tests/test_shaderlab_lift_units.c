// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_lift.h"
#include "common/file_io.h"
#include "dxbc/usbd.h"
#include "dxbc/dxbc_hash.h"
#include "test_shaderlab_fixture.h"
#include "test_unity_uv_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef enum {
    NONE,
    PREPROCESS_TRANSPORT,
    PREPROCESS_INCLUDE_AUTHORITY,
    PREPROCESS_REJECTED,
    PREPROCESS_MISSING_IDENTITY,
    PREPROCESS_ZERO_IDENTITY,
    PREPROCESS_CONTROL_DRIFT,
    PREPROCESS_EXTRA_SNIPPET,
    COMPILE_TRANSPORT,
    COMPILE_INCLUDE_AUTHORITY,
    COMPILE_REJECTED,
    COMPILE_MISSING_IDENTITY,
    COMPILE_ZERO_IDENTITY,
    COMPILE_WRONG_IDENTITY,
    HELPER_WRONG_DEFINITION,
    COMPILE_WRONG_BYTES,
    COMPILE_INVALID_BYTES,
    COMPILE_CACHE_MISS,
    COMPILER_DRIFT,
    ENVIRONMENT_DRIFT,
    SOURCE_REVISION_DRIFT,
    PROFILE_DRIFT,
    LATE_PREPROCESS,
    LATE_COMPILE,
    CANCEL_PREPROCESS,
    CANCEL_COMPILE,
    CLOCK_FAILURE,
    CLOCK_REGRESSION
} Failure;

typedef struct {
    CommonFileBytes bytes;
    DXBCUSBDRecordView records[2];
    SerializedShader shader;
    SerializedSubShader subshader;
    SerializedPass passes[2];
    SerializedSubProgram programs[2];
    SerializedSubProgramIdentity identities[2];
    int platform;
    ShaderBlobArchive archive;
    BlobEntry entries[2];
    uint8_t *segments[2];
    int lengths[2];
    UnityCompileProfile profile;
    UnityShaderLabLiftInput input;
} Fixture;

typedef struct {
    Fixture *fixture;
    UnityCompilerBroker *canonical_broker;
    Failure failure;
    size_t fail_preprocess;
    size_t fail_compile;
    size_t preprocesses;
    size_t compiles;
    uint64_t now;
    bool cancelled;
    bool clock_failed;
    bool compiler_drift;
    bool environment_drift;
    bool source_revision_drift;
    bool cached;
} Service;

static int fixture_init_path(Fixture *fixture, const char *path) {
    memset(fixture, 0, sizeof(*fixture));
    CHECK(common_file_read_regular(path, 1024 * 1024, &fixture->bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, fixture->bytes.data, fixture->bytes.size, NULL));
    CHECK(table.record_count == 2);
    for (int stage = 0; stage < 2; ++stage) {
        CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &fixture->records[stage]));
        size_t size = 0;
        fixture->segments[stage] = test_shaderlab_variant_blob(fixture->records[stage].dxbc,
                                                               fixture->records[stage].dxbc_size,
                                                               stage ? 17 : 15, NULL, &size);
        CHECK(fixture->segments[stage] && size <= INT32_MAX);
        fixture->lengths[stage] = (int)size;
        fixture->entries[stage] = (BlobEntry){0, (int32_t)size, stage};
        fixture->programs[stage] = (SerializedSubProgram){
            .blob_index = stage, .program_type = stage ? 17 : 15, .shader_requirements = 0xe3};
        fixture->identities[stage].hardware_tier_group = 3;
        fixture->passes[0].subprogram_count[stage] = 1;
        fixture->passes[0].subprograms[stage] = &fixture->programs[stage];
        fixture->passes[0].subprogram_identities[stage] = &fixture->identities[stage];
    }
    fixture->platform = 4;
    fixture->passes[0].name = "Expression";
    fixture->passes[0].has_serialized_platforms = true;
    fixture->passes[0].platform_count = 1;
    fixture->passes[0].platforms = &fixture->platform;
    fixture->passes[0].program_mask = 6;
    fixture->passes[1] = fixture->passes[0];
    fixture->subshader.pass_count = 1;
    fixture->subshader.passes = fixture->passes;
    fixture->shader.name = "Experiment/ExpressionFixture";
    fixture->shader.subshader_count = 1;
    fixture->shader.subshaders = &fixture->subshader;
    fixture->archive = (ShaderBlobArchive){.entries = fixture->entries,
                                           .entry_count = 2,
                                           .segments = fixture->segments,
                                           .segment_lengths = fixture->lengths,
                                           .segment_count = 2};
    unity_compile_profile_init(&fixture->profile);
    fixture->profile.build_platform = 19;
    fixture->profile.valid_apis = 295472;
    strcpy(fixture->profile.provenance, "synthetic-unit-test");
    fixture->input = (UnityShaderLabLiftInput){.shader = &fixture->shader,
                                               .archive = &fixture->archive,
                                               .profile = &fixture->profile,
                                               .source_path = "Assets/Expression.shader",
                                               .source_directory = "Assets",
                                               .source_basename = "Expression.shader"};
    return 0;
}

static int fixture_init(Fixture *fixture) {
    return fixture_init_path(fixture, SHADERLAB_EXPRESSION_TEST_FIXTURE);
}

static void fixture_free(Fixture *fixture) {
    for (int stage = 0; stage < 2; ++stage)
        free(fixture->segments[stage]);
    common_file_bytes_dispose(&fixture->bytes);
}

static bool clock_service(void *opaque, uint64_t *milliseconds) {
    Service *service = opaque;
    *milliseconds = service->now;
    return !service->clock_failed;
}

static bool cancel_service(void *opaque) { return ((Service *)opaque)->cancelled; }

static bool toolchain_service(void *opaque, UnityCompilerToolchainProvenance *provenance) {
    Service *service = opaque;
    memset(provenance->compiler_fingerprint, service->compiler_drift ? 2 : 1, 32);
    memset(provenance->environment_fingerprint, service->environment_drift ? 4 : 3, 32);
    provenance->source_authority_revision = service->source_revision_drift ? 2 : 1;
    return true;
}

static bool preprocess_service(void *opaque, const UnityCompilerShaderPreprocessRequest *request,
                               UnityCompilerPreprocessResponse *response) {
    Service *service = opaque;
    ++service->preprocesses;
    if (strcmp(request->source_directory, service->fixture->input.source_directory) ||
        strcmp(request->shader_name, service->fixture->shader.name) || !request->source)
        return false;
    const Failure failure =
        service->preprocesses == service->fail_preprocess ? service->failure : NONE;
    if (failure == LATE_PREPROCESS)
        service->now += 1000;
    if (failure == CANCEL_PREPROCESS)
        service->cancelled = true;
    if (failure == PREPROCESS_TRANSPORT)
        return false;
    if (failure == PREPROCESS_INCLUDE_AUTHORITY) {
        response->status.availability = UNITY_COMPILER_RESPONSE_INCLUDE_AUTHORITY_UNAVAILABLE;
        return true;
    }
    response->status.compiler_success = failure != PREPROCESS_REJECTED;
    response->status.from_cache = service->cached;
    response->has_request_identity = failure != PREPROCESS_MISSING_IDENTITY;
    /* Synthetic identities exercise transfer/admission only. Client tests and
     * live checks cover actual canonical wire requests. */
    if (failure != PREPROCESS_ZERO_IDENTITY)
        common_sha256(request->source, strlen(request->source), response->request_digest);
    memset(response->controls_digest,
           failure == PREPROCESS_CONTROL_DRIFT                     ? 9
           : strstr(request->source, "#include \"UnityCG.cginc\"") ? 10
                                                                   : 8,
           32);
    response->result.snippet_count =
        service->fixture->subshader.pass_count + (failure == PREPROCESS_EXTRA_SNIPPET);
    response->result.snippets =
        calloc((size_t)response->result.snippet_count, sizeof(*response->result.snippets));
    if (!response->result.snippets)
        return false;
    for (int i = 0; i < response->result.snippet_count; ++i) {
        PreprocessedSnippet *snippet = &response->result.snippets[i];
        snippet->source = strdup(request->source);
        snippet->has_contract = true;
        unity_compiler_snippet_contract_init(&snippet->contract);
        snippet->contract.language = 3;
        snippet->contract.program_types_mask = 3;
        snippet->contract.requirements = 0xe3;
        for (int stage = 0; stage < 2; ++stage)
            for (int family = 0; family < 3; ++family)
                if (!unity_compiler_snippet_contract_set_variant_combinations(
                        &snippet->contract, stage, (UnityKeywordVariantFamily)family, NULL, 0))
                    return false;
        if (!snippet->source)
            return false;
    }
    return true;
}

static bool request_digest_service(void *opaque, const UnityCompilerSnippetCompileRequest *request,
                                   uint8_t digest[32]) {
    Service *service = opaque;
    UnityCompilerToolchainProvenance provenance;
    if (!toolchain_service(service, &provenance))
        return false;
    const UnityCompilerOfflineAuthority authority = {provenance.compiler_fingerprint,
                                                     provenance.environment_fingerprint};
    uint8_t *transcript = NULL;
    size_t size = 0;
    const bool ok = unity_compiler_broker_serialize_compile_request_with_authority(
        service->canonical_broker, request, &authority, &transcript, &size, digest);
    free(transcript);
    return ok;
}

static bool compile_service(void *opaque, const UnityCompilerSnippetCompileRequest *request,
                            UnityCompilerBinaryResponse *response) {
    Service *service = opaque;
    ++service->compiles;
    if (request->platform != 4 || request->shader_type < 0 || request->shader_type > 1 ||
        strcmp(request->source_directory, service->fixture->input.source_directory) ||
        strcmp(request->source_basename, service->fixture->input.source_basename))
        return false;
    const Failure failure = service->compiles == service->fail_compile ? service->failure : NONE;
    if (failure == LATE_COMPILE)
        service->now += 1000;
    if (failure == CANCEL_COMPILE)
        service->cancelled = true;
    if (failure == CLOCK_FAILURE)
        service->clock_failed = true;
    if (failure == CLOCK_REGRESSION)
        service->now = 0;
    if (failure == COMPILER_DRIFT)
        service->compiler_drift = true;
    if (failure == ENVIRONMENT_DRIFT)
        service->environment_drift = true;
    if (failure == SOURCE_REVISION_DRIFT)
        service->source_revision_drift = true;
    if (failure == PROFILE_DRIFT)
        service->fixture->profile.build_platform++;
    if (failure == COMPILE_TRANSPORT)
        return false;
    if (failure == COMPILE_INCLUDE_AUTHORITY) {
        response->status.availability = UNITY_COMPILER_RESPONSE_INCLUDE_AUTHORITY_UNAVAILABLE;
        return true;
    }
    response->status.compiler_success = failure != COMPILE_REJECTED;
    response->status.from_cache = service->cached;
    if (failure == COMPILE_CACHE_MISS)
        response->status.availability = UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS;
    response->has_request_identity = failure != COMPILE_MISSING_IDENTITY;
    memset(response->request_digest, failure == COMPILE_ZERO_IDENTITY ? 0 : (int)service->compiles,
           32);
    memset(response->controls_digest, 7, 32);
    if (service->canonical_broker && failure != COMPILE_ZERO_IDENTITY) {
        if (!request_digest_service(service, request, response->request_digest))
            return false;
        if (failure == COMPILE_WRONG_IDENTITY)
            response->request_digest[0] ^= 1;
    }
    if (request->preprocess_only) {
        StringBuilder expansion;
        test_uv_expansion(&expansion);
        if (!sb_ok(&expansion)) {
            sb_free(&expansion);
            return false;
        }
        if (failure == HELPER_WRONG_DEFINITION) {
            char *operation = strchr(expansion.buf, '+');
            if (!operation) {
                sb_free(&expansion);
                return false;
            }
            *operation = '-';
        }
        response->size = expansion.len;
        response->data = (uint8_t *)sb_detach(&expansion);
        return true;
    }
    const int stage =
        failure == COMPILE_WRONG_BYTES ? 1 - request->shader_type : request->shader_type;
    const DXBCUSBDRecordView *record = &service->fixture->records[stage];
    response->size = failure == COMPILE_INVALID_BYTES ? 1 : record->dxbc_size;
    response->data = malloc(response->size);
    if (!response->data)
        return false;
    memcpy(response->data, record->dxbc, response->size);
    return true;
}

static HLSLLiftStatus run(Service *service, HLSLLiftLimits limits,
                          UnityShaderLabLiftResult **result) {
    const UnityShaderLabLiftServices services = {.monotonic_ms = clock_service,
                                                 .cancelled = cancel_service,
                                                 .preprocess = preprocess_service,
                                                 .compile = compile_service,
                                                 .toolchain = toolchain_service,
                                                 .context = service,
                                                 .request_digest = request_digest_service};
    return unity_shaderlab_lift_run(&service->fixture->input, &services, &limits, result);
}

static int test_verified_and_fallback(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture) == 0);
    const HLSLLiftLimits limits = {1, 16, 1000};
    for (int passes = 1; passes <= 2; ++passes) {
        fixture.subshader.pass_count = passes;
        Service service = {.fixture = &fixture, .now = 10, .cached = true};
        UnityShaderLabLiftResult *result = NULL;
        CHECK(run(&service, limits, &result) == HLSL_LIFT_VERIFIED);
        const UnityShaderLabLiftArtifact *baseline = unity_shaderlab_lift_baseline(result);
        const UnityShaderLabLiftArtifact *candidate = unity_shaderlab_lift_candidate(result);
        CHECK(unity_shaderlab_lift_accepted(result) == candidate);
        CHECK(baseline->status == HLSL_LIFT_VERIFIED && candidate->status == HLSL_LIFT_VERIFIED);
        CHECK(candidate->pass_count == (size_t)passes &&
              candidate->certified_pass_count == (size_t)passes);
        CHECK(shaderlab_expression_source_map_matches_source(&candidate->source_map,
                                                             &candidate->source));
        CHECK(strstr(baseline->source.buf, "float4 r0") &&
              !strstr(candidate->source.buf, "float4 r0"));
        CHECK(strcmp(baseline->source.buf, candidate->source.buf));
        for (int p = 0; p < passes; ++p) {
            const UnityShaderLabLiftPassReport *report = &candidate->passes[p];
            CHECK(report->serialized_pass_index == p && report->snippet_index == p);
            CHECK(report->domain.matched_dxbc_count == 2 &&
                  report->domain.compiler_response_count == 2);
            CHECK(report->domain.compiler_responses[0].provenance.has_request_identity);
        }
        HLSLLiftStats stats;
        size_t preprocesses;
        unity_shaderlab_lift_stats(result, &stats, &preprocesses);
        CHECK(stats.candidates == 1 && stats.accepted == 1 && stats.compiles == (size_t)passes * 4);
        CHECK(stats.cache_hits == stats.compiles && preprocesses == 2);
        char *json = unity_shaderlab_lift_format_json(result);
        char *repeated = unity_shaderlab_lift_format_json(result);
        CHECK(json && repeated && strcmp(json, repeated) == 0);
        CHECK(strstr(json, "\"selection\":\"high-level\""));
        CHECK(strstr(json, "\"scope\":\"generated-local-d3d11-program-domain\""));
        CHECK(strstr(json, "\"instructions\":[{"));
        CHECK(strstr(json, "\"recorded\":true,\"response_received\":true"));
        CHECK(strstr(json, "\"lift\":{\"id\":\"float4-expressions\",\"version\":4}"));
        CHECK(!strstr(json, fixture.shader.name) && !strstr(json, fixture.input.source_path));
        CHECK(!strstr(json, fixture.input.source_directory) &&
              !strstr(json, fixture.input.source_basename) &&
              !strstr(json, fixture.profile.provenance));
        char digest[65];
        common_sha256_digest_to_hex(candidate->source_map.source_digest, digest);
        CHECK(strstr(json, digest));
        CHECK(json[strlen(json) - 1] == '\n');
        free(json);
        free(repeated);
        unity_shaderlab_lift_result_free(result);
    }
    fixture.subshader.pass_count = 1;
    const struct {
        Failure failure;
        HLSLLiftStatus expected;
        bool preprocess;
    } failures[] = {
        {PREPROCESS_TRANSPORT, HLSL_LIFT_COMPILER_UNAVAILABLE, true},
        {PREPROCESS_INCLUDE_AUTHORITY, HLSL_LIFT_COMPILER_UNAVAILABLE, true},
        {PREPROCESS_REJECTED, HLSL_LIFT_COMPILER_REJECTED, true},
        {PREPROCESS_MISSING_IDENTITY, HLSL_LIFT_PROVENANCE_MISMATCH, true},
        {PREPROCESS_ZERO_IDENTITY, HLSL_LIFT_PROVENANCE_MISMATCH, true},
        {PREPROCESS_CONTROL_DRIFT, HLSL_LIFT_AUTHORITY_MISMATCH, true},
        {PREPROCESS_EXTRA_SNIPPET, HLSL_LIFT_AUTHORITY_MISMATCH, true},
        {COMPILE_TRANSPORT, HLSL_LIFT_COMPILER_UNAVAILABLE, false},
        {COMPILE_INCLUDE_AUTHORITY, HLSL_LIFT_COMPILER_UNAVAILABLE, false},
        {COMPILE_REJECTED, HLSL_LIFT_COMPILER_REJECTED, false},
        {COMPILE_MISSING_IDENTITY, HLSL_LIFT_PROVENANCE_MISMATCH, false},
        {COMPILE_ZERO_IDENTITY, HLSL_LIFT_PROVENANCE_MISMATCH, false},
        {COMPILE_WRONG_BYTES, HLSL_LIFT_DXBC_MISMATCH, false},
        {COMPILE_INVALID_BYTES, HLSL_LIFT_INVALID_DXBC, false},
        {COMPILE_CACHE_MISS, HLSL_LIFT_COMPILER_UNAVAILABLE, false},
        {COMPILER_DRIFT, HLSL_LIFT_AUTHORITY_MISMATCH, false},
        {ENVIRONMENT_DRIFT, HLSL_LIFT_AUTHORITY_MISMATCH, false},
        {SOURCE_REVISION_DRIFT, HLSL_LIFT_AUTHORITY_MISMATCH, false},
        {PROFILE_DRIFT, HLSL_LIFT_AUTHORITY_MISMATCH, false},
        {LATE_PREPROCESS, HLSL_LIFT_BUDGET_EXHAUSTED, true},
        {LATE_COMPILE, HLSL_LIFT_BUDGET_EXHAUSTED, false},
        {CANCEL_PREPROCESS, HLSL_LIFT_CANCELLED, true},
        {CANCEL_COMPILE, HLSL_LIFT_CANCELLED, false},
        {CLOCK_FAILURE, HLSL_LIFT_CLOCK_UNAVAILABLE, false},
        {CLOCK_REGRESSION, HLSL_LIFT_CLOCK_UNAVAILABLE, false},
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        Service service = {.fixture = &fixture,
                           .now = 10,
                           .failure = failures[i].failure,
                           .fail_preprocess = 2,
                           .fail_compile = 4};
        UnityShaderLabLiftResult *result = NULL;
        const HLSLLiftStatus status = run(&service, limits, &result);
        if (status != failures[i].expected)
            fprintf(stderr, "failure=%d actual=%s expected=%s\n", failures[i].failure,
                    hlsl_lift_status_name(status), hlsl_lift_status_name(failures[i].expected));
        CHECK(status == failures[i].expected);
        const bool authority_changed = failures[i].failure == COMPILER_DRIFT ||
                                       failures[i].failure == ENVIRONMENT_DRIFT ||
                                       failures[i].failure == SOURCE_REVISION_DRIFT ||
                                       failures[i].failure == PROFILE_DRIFT;
        CHECK(unity_shaderlab_lift_accepted(result) ==
              (authority_changed ? NULL : unity_shaderlab_lift_baseline(result)));
        CHECK(unity_shaderlab_lift_baseline(result)->status == HLSL_LIFT_VERIFIED);
        CHECK(unity_shaderlab_lift_candidate(result)->status == status);
        char *json = unity_shaderlab_lift_format_json(result);
        CHECK(json && strstr(json, authority_changed ? "\"selection\":\"unverified\""
                                                    : "\"selection\":\"low-level-fallback\""));
        CHECK(strstr(json, hlsl_lift_status_name(status)));
        if (failures[i].failure == PREPROCESS_TRANSPORT)
            CHECK(!unity_shaderlab_lift_candidate(result)->preprocess_received &&
                  strstr(json, "\"preprocessing\":{\"attempted\":true,\"received\":false"));
        if (failures[i].failure == PREPROCESS_INCLUDE_AUTHORITY ||
            failures[i].failure == COMPILE_INCLUDE_AUTHORITY)
            CHECK(strstr(json, "\"availability\":\"include-authority-unavailable\""));
        if (status == HLSL_LIFT_DXBC_MISMATCH)
            CHECK(strstr(json, "\"dxbc_mismatch\":{") && strstr(json, "\"byte_offset\":"));
        free(json);
        CHECK(service.preprocesses == 2 && service.compiles == (failures[i].preprocess ? 2 : 4));
        unity_shaderlab_lift_result_free(result);
        fixture.profile.build_platform = 19;
    }
    fixture_free(&fixture);
    return 0;
}

static int test_unsupported_high_level_retains_baseline(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture) == 0);
    DXBCDocument document;
    dxbc_document_init(&document);
    CHECK(dxbc_document_parse(&document, fixture.records[1].dxbc, fixture.records[1].dxbc_size,
                              NULL));
    uint8_t *bytes = (uint8_t *)fixture.records[1].dxbc;
    bool changed = false;
    for (size_t i = 0; i < document.instruction_count; ++i) {
        const DXBCDocumentInstruction *instruction = &document.instructions[i];
        if (instruction->opcode != 56)
            continue; /* MUL */
        /* Saturation is supported in low-level output but explicitly outside
         * the initial expression lift. Keep a valid, rehashed container. */
        bytes[instruction->byte_offset + 1] |= 0x20;
        changed = true;
        break;
    }
    CHECK(changed);
    uint8_t hash[16];
    CHECK(dxbc_compute_hash(bytes, fixture.records[1].dxbc_size, hash));
    memcpy(bytes + 4, hash, sizeof(hash));
    dxbc_document_free(&document);
    free(fixture.segments[1]);
    size_t size;
    fixture.segments[1] =
        test_shaderlab_variant_blob(bytes, fixture.records[1].dxbc_size, 17, NULL, &size);
    CHECK(fixture.segments[1] && size == (size_t)fixture.lengths[1]);
    Service service = {.fixture = &fixture, .now = 10};
    UnityShaderLabLiftResult *result = NULL;
    CHECK(run(&service, (HLSLLiftLimits){1, 16, 1000}, &result) == HLSL_LIFT_EMISSION_REJECTED);
    CHECK(unity_shaderlab_lift_accepted(result) == unity_shaderlab_lift_baseline(result));
    CHECK(service.preprocesses == 1 && service.compiles == 2);
    CHECK(!unity_shaderlab_lift_candidate(result)->preprocess_attempted);
    CHECK(unity_shaderlab_lift_candidate(result)->source_map.records == NULL);
    CHECK(strstr(unity_shaderlab_lift_baseline(result)->source.buf, "saturate("));
    unity_shaderlab_lift_result_free(result);
    fixture_free(&fixture);
    return 0;
}

static int test_admission_and_later_pass(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture) == 0);
    UnityShaderLabLiftResult *result = NULL;
    Service service = {.fixture = &fixture, .now = 10};
    CHECK(run(&service, (HLSLLiftLimits){0, 2, 1000}, &result) == HLSL_LIFT_BUDGET_EXHAUSTED);
    CHECK(unity_shaderlab_lift_accepted(result) == unity_shaderlab_lift_baseline(result));
    CHECK(!unity_shaderlab_lift_candidate(result)->attempted && service.preprocesses == 1);
    unity_shaderlab_lift_result_free(result);
    service = (Service){.fixture = &fixture, .now = 10};
    CHECK(run(&service, (HLSLLiftLimits){1, 3, 1000}, &result) == HLSL_LIFT_BUDGET_EXHAUSTED);
    CHECK(unity_shaderlab_lift_accepted(result) == unity_shaderlab_lift_baseline(result));
    CHECK(service.compiles == 3);
    unity_shaderlab_lift_result_free(result);
    service = (Service){.fixture = &fixture, .now = 10};
    CHECK(run(&service, (HLSLLiftLimits){1, 0, 1000}, &result) == HLSL_LIFT_BUDGET_EXHAUSTED);
    CHECK(!unity_shaderlab_lift_accepted(result) && service.compiles == 0);
    unity_shaderlab_lift_result_free(result);
    service = (Service){.fixture = &fixture, .now = 10, .cancelled = true};
    CHECK(run(&service, (HLSLLiftLimits){1, 16, 1000}, &result) == HLSL_LIFT_CANCELLED);
    CHECK(!unity_shaderlab_lift_accepted(result) && service.preprocesses == 0);
    unity_shaderlab_lift_result_free(result);
    service = (Service){
        .fixture = &fixture, .now = 10, .failure = COMPILE_WRONG_BYTES, .fail_compile = 1};
    CHECK(run(&service, (HLSLLiftLimits){1, 16, 1000}, &result) == HLSL_LIFT_DXBC_MISMATCH);
    CHECK(!unity_shaderlab_lift_accepted(result) &&
          !unity_shaderlab_lift_candidate(result)->attempted);
    unity_shaderlab_lift_result_free(result);
    fixture.subshader.pass_count = 2;
    service = (Service){
        .fixture = &fixture, .now = 10, .failure = COMPILE_WRONG_BYTES, .fail_compile = 8};
    CHECK(run(&service, (HLSLLiftLimits){1, 16, 1000}, &result) == HLSL_LIFT_DXBC_MISMATCH);
    CHECK(unity_shaderlab_lift_accepted(result) == unity_shaderlab_lift_baseline(result));
    CHECK(unity_shaderlab_lift_candidate(result)->certified_pass_count == 1);
    CHECK(unity_shaderlab_lift_baseline(result)->certified_pass_count == 2);
    unity_shaderlab_lift_result_free(result);
    fixture.shader.subshaders = NULL;
    service = (Service){.fixture = &fixture, .now = 10};
    CHECK(run(&service, (HLSLLiftLimits){1, 16, 1000}, &result) == HLSL_LIFT_PRECONDITION_REJECTED);
    CHECK(!unity_shaderlab_lift_accepted(result) && service.preprocesses == 0);
    char *json = unity_shaderlab_lift_format_json(result);
    CHECK(json && strstr(json, "\"selection\":\"unverified\""));
    CHECK(strstr(json, "\"source_sha256\":null") && strstr(json, "\"response\":null"));
    CHECK(strstr(json, "\"emission_attempted\":false,\"emission_reason\":null"));
    free(json);
    unity_shaderlab_lift_result_free(result);
    fixture_free(&fixture);
    CHECK(unity_shaderlab_lift_run(NULL, NULL, NULL, &result) == HLSL_LIFT_INVALID_ARGUMENT);
    CHECK(result == NULL && unity_shaderlab_lift_accepted(NULL) == NULL);
    unity_shaderlab_lift_result_free(NULL);
    CHECK(unity_shaderlab_lift_format_json(NULL) == NULL);
    return 0;
}

static int test_unity_uv_transaction(void) {
    Fixture fixture;
    CHECK(fixture_init_path(&fixture, SHADERLAB_UNITY_UV_TEST_FIXTURE) == 0);
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(".", "");
    CHECK(broker);
    const HLSLLiftLimits limits = {2, 32, 1000};
    for (int passes = 1; passes <= 2; ++passes) {
        fixture.subshader.pass_count = passes;
        Service service = {
            .fixture = &fixture, .canonical_broker = broker, .now = 10, .cached = true};
        UnityShaderLabLiftResult *result = NULL;
        CHECK(run(&service, limits, &result) == HLSL_LIFT_VERIFIED);
        const UnityShaderLabLiftArtifact *baseline = unity_shaderlab_lift_baseline(result);
        const UnityShaderLabLiftArtifact *included = unity_shaderlab_lift_helper_baseline(result);
        const UnityShaderLabLiftArtifact *candidate = unity_shaderlab_lift_helper_candidate(result);
        CHECK(unity_shaderlab_lift_accepted(result) == candidate);
        CHECK(unity_shaderlab_lift_candidate(result)->status == HLSL_LIFT_EMISSION_REJECTED);
        CHECK(baseline->status == HLSL_LIFT_VERIFIED && included->status == HLSL_LIFT_VERIFIED);
        CHECK(candidate->status == HLSL_LIFT_VERIFIED && candidate->high_level &&
              candidate->unity_uv_helpers);
        CHECK(!memcmp(included->preprocessing.controls_digest,
                      candidate->preprocessing.controls_digest, 32));
        CHECK(memcmp(baseline->preprocessing.controls_digest,
                     candidate->preprocessing.controls_digest, 32));
        CHECK(shaderlab_expression_source_map_matches_source(&candidate->source_map,
                                                             &candidate->source));
        for (int p = 0; p < passes; ++p) {
            const UnityShaderLabLiftPassReport *pass = &candidate->passes[p];
            CHECK(pass->domain.matched_dxbc_count == 2 && pass->helper_check_count == 2);
            for (size_t c = 0; c < pass->helper_check_count; ++c) {
                const UnityShaderLabLiftHelperCheck *check = &pass->helper_checks[c];
                CHECK(check->status == UNITY_UV_HELPER_OK && check->domain_compile_index == c);
                CHECK(check->compile_received && check->compile_identity_matched);
                CHECK(!memcmp(check->evidence.compile_request_digest,
                              pass->domain.compiler_responses[c].provenance.request_digest, 32));
                CHECK(check->preprocessing.status.from_cache &&
                      check->evidence.expansion.probe.end);
            }
        }
        HLSLLiftStats stats;
        size_t preprocesses;
        unity_shaderlab_lift_stats(result, &stats, &preprocesses);
        CHECK(stats.candidates == 2 && stats.accepted == 1 && stats.compiles == (size_t)passes * 8);
        CHECK(stats.cache_hits == stats.compiles && preprocesses == 3);
        char *json = unity_shaderlab_lift_format_json(result);
        CHECK(json && strstr(json, "\"selection\":\"high-level\""));
        CHECK(strstr(json, "\"unity_helper\":{\"id\":\"unity-packed-uv-adjust\""));
        CHECK(strstr(json, "\"compile_identity_matched\":true"));
        CHECK(!strstr(json, fixture.input.source_path));
        free(json);
        unity_shaderlab_lift_result_free(result);
    }
    fixture.subshader.pass_count = 1;
    const struct {
        Failure failure;
        size_t compile, preprocess;
        HLSLLiftStatus expected;
        bool invalidates_baseline;
    } cases[] = {
        {HELPER_WRONG_DEFINITION, 5, 0, HLSL_LIFT_PRECONDITION_REJECTED, false},
        {COMPILE_WRONG_IDENTITY, 5, 0, HLSL_LIFT_AUTHORITY_MISMATCH, false},
        {COMPILE_WRONG_IDENTITY, 6, 0, HLSL_LIFT_PROVENANCE_MISMATCH, false},
        {COMPILE_WRONG_BYTES, 6, 0, HLSL_LIFT_DXBC_MISMATCH, false},
        {COMPILE_WRONG_BYTES, 8, 0, HLSL_LIFT_DXBC_MISMATCH, false},
        {COMPILE_REJECTED, 3, 0, HLSL_LIFT_COMPILER_REJECTED, false},
        {COMPILE_TRANSPORT, 5, 0, HLSL_LIFT_COMPILER_UNAVAILABLE, false},
        {COMPILE_MISSING_IDENTITY, 5, 0, HLSL_LIFT_PROVENANCE_MISMATCH, false},
        {COMPILE_MISSING_IDENTITY, 6, 0, HLSL_LIFT_PROVENANCE_MISMATCH, false},
        {COMPILE_CACHE_MISS, 5, 0, HLSL_LIFT_COMPILER_UNAVAILABLE, false},
        {COMPILE_REJECTED, 5, 0, HLSL_LIFT_COMPILER_REJECTED, false},
        {PREPROCESS_REJECTED, 0, 2, HLSL_LIFT_COMPILER_REJECTED, false},
        {PREPROCESS_CONTROL_DRIFT, 0, 3, HLSL_LIFT_AUTHORITY_MISMATCH, false},
        {COMPILER_DRIFT, 5, 0, HLSL_LIFT_AUTHORITY_MISMATCH, true},
        {ENVIRONMENT_DRIFT, 5, 0, HLSL_LIFT_AUTHORITY_MISMATCH, true},
        {SOURCE_REVISION_DRIFT, 6, 0, HLSL_LIFT_AUTHORITY_MISMATCH, true},
        {PROFILE_DRIFT, 6, 0, HLSL_LIFT_AUTHORITY_MISMATCH, true},
        {LATE_COMPILE, 5, 0, HLSL_LIFT_BUDGET_EXHAUSTED, false},
        {CANCEL_COMPILE, 5, 0, HLSL_LIFT_CANCELLED, false},
        {CLOCK_FAILURE, 6, 0, HLSL_LIFT_CLOCK_UNAVAILABLE, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Service service = {.fixture = &fixture,
                           .canonical_broker = broker,
                           .now = 10,
                           .failure = cases[i].failure,
                           .fail_compile = cases[i].compile,
                           .fail_preprocess = cases[i].preprocess};
        UnityShaderLabLiftResult *result = NULL;
        HLSLLiftStatus status = run(&service, limits, &result);
        if (status != cases[i].expected)
            fprintf(stderr, "helper failure %zu: %s expected %s\n", i,
                    hlsl_lift_status_name(status), hlsl_lift_status_name(cases[i].expected));
        CHECK(status == cases[i].expected);
        CHECK(unity_shaderlab_lift_baseline(result)->status == HLSL_LIFT_VERIFIED);
        CHECK(unity_shaderlab_lift_accepted(result) ==
              (cases[i].invalidates_baseline ? NULL : unity_shaderlab_lift_baseline(result)));
        CHECK(service.compiles <= (cases[i].compile ? cases[i].compile : 4));
        unity_shaderlab_lift_result_free(result);
        fixture.profile.build_platform = 19;
    }
    for (size_t maximum = 4; maximum <= 7; ++maximum) {
        Service service = {.fixture = &fixture, .canonical_broker = broker, .now = 10};
        UnityShaderLabLiftResult *result = NULL;
        CHECK(run(&service, (HLSLLiftLimits){2, maximum, 1000}, &result) ==
              HLSL_LIFT_BUDGET_EXHAUSTED);
        CHECK(service.compiles == maximum);
        CHECK(unity_shaderlab_lift_accepted(result) == unity_shaderlab_lift_baseline(result));
        unity_shaderlab_lift_result_free(result);
    }
    Service service = {.fixture = &fixture, .canonical_broker = broker, .now = 10};
    UnityShaderLabLiftResult *result = NULL;
    CHECK(run(&service, (HLSLLiftLimits){1, 32, 1000}, &result) == HLSL_LIFT_EMISSION_REJECTED);
    CHECK(!unity_shaderlab_lift_helper_baseline(result)->attempted && service.compiles == 2);
    CHECK(unity_shaderlab_lift_accepted(result) == unity_shaderlab_lift_baseline(result));
    unity_shaderlab_lift_result_free(result);
    unity_compiler_broker_destroy(broker);
    fixture_free(&fixture);
    return 0;
}

int main(void) {
    if (test_verified_and_fallback() || test_admission_and_later_pass() ||
        test_unsupported_high_level_retains_baseline() || test_unity_uv_transaction())
        return 1;
    puts("ShaderLab lift admission, full-pass certification and rollback passed.");
    return 0;
}
