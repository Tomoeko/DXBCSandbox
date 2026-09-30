// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_emitted_matrix_attachment.h"
#include "compiler/unity_generated_owned_request_internal.h"
#include "common/file_io.h"
#include "dxbc/usbd.h"
#include "test_shaderlab_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static bool unavailable_compile(void *context, const UnityCompilerSnippetCompileRequest *request,
    UnityCompilerBinaryResponse *response) {
    (void)request; (void)response;
    ++*(size_t *)context;
    return false;
}

static bool observe(void *context, const UnityGeneratedOwnedRequest *request) {
    (void)request;
    ++*(size_t *)context;
    return true;
}

static bool absence_and_injected_boundary(void) {
    CommonFileBytes bytes = {0};
    CHECK(common_file_read_regular(EMITTED_MATRIX_ABSENCE_FIXTURE, 1024 * 1024, &bytes) == COMMON_FILE_OK);
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, bytes.data, bytes.size, NULL));
    CHECK(table.record_count == 2);
    SerializedSubProgram programs[2] = {0};
    SerializedSubProgramIdentity identities[2] = {0};
    SerializedPass pass = {.has_serialized_platforms = true, .program_mask = 6};
    BlobEntry entries[2];
    uint8_t *segments[2];
    int lengths[2];
    int platform = 4;
    pass.platforms = &platform;
    pass.platform_count = 1;
    for (int stage = 0; stage < 2; ++stage) {
        DXBCUSBDRecordView record;
        CHECK(dxbc_usbd_table_record(&table, (uint32_t)stage, &record));
        size_t size;
        segments[stage] = test_shaderlab_variant_blob(record.dxbc, record.dxbc_size, stage ? 17 : 15, NULL, &size);
        CHECK(segments[stage] && size <= INT32_MAX);
        lengths[stage] = (int)size;
        entries[stage] = (BlobEntry){0, (int32_t)size, stage};
        programs[stage] = (SerializedSubProgram){.blob_index = stage,
            .program_type = stage ? 17 : 15, .shader_requirements = 0xe3};
        identities[stage].hardware_tier_group = 3;
        pass.subprogram_count[stage] = 1;
        pass.subprograms[stage] = &programs[stage];
        pass.subprogram_identities[stage] = &identities[stage];
    }
    SerializedSubShader subshader = {.pass_count = 1, .passes = &pass};
    SerializedShader shader = {.name = "Fixture/Quality/MatrixAbsence",
        .subshader_count = 1, .subshaders = &subshader};
    ShaderBlobArchive archive = {.entries = entries, .entry_count = 2,
        .segments = segments, .segment_lengths = lengths, .segment_count = 2};
    ShaderLabSourceQualityRequest source = {.shader = &shader, .archive = &archive};
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19;
    profile.valid_apis = UINT32_C(1) << 4;
    strcpy(profile.provenance, "controlled-unit");
    CHECK(unity_compile_profile_fingerprint(&profile, profile.fingerprint) == UNITY_COMPILE_PROFILE_OK);
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(".", ".");
    CHECK(broker);
    UnityEmittedMatrixAttachmentInput input = {.source = &source, .profile = &profile,
        .broker = broker, .source_directory = ".", .source_basename = "Absence.shader"};
    UnityEmittedMatrixAttachment *attachment = NULL;
    UnityEmittedMatrixAttachmentDiagnostic diagnostic;
    CHECK(unity_emitted_matrix_attachment_capture(&input, &attachment, &diagnostic) == UNITY_EMITTED_MATRIX_NOT_APPLICABLE);
    CHECK(!attachment && diagnostic.emission == SHADERLAB_MATRIX_USES_NOT_APPLICABLE);
    profile.fingerprint[0] ^= 1;
    CHECK(unity_emitted_matrix_attachment_capture(&input, &attachment, NULL) == UNITY_EMITTED_MATRIX_INVALID_PROFILE);
    CHECK(!attachment);
    profile.fingerprint[0] ^= 1;
    input.source_directory = NULL;
    CHECK(unity_emitted_matrix_attachment_capture(&input, &attachment, NULL) == UNITY_EMITTED_MATRIX_INVALID_ARGUMENT);
    input.source_directory = ".";
    attachment = (UnityEmittedMatrixAttachment *)&shader;
    CHECK(unity_emitted_matrix_attachment_capture(&input, &attachment, NULL) == UNITY_EMITTED_MATRIX_INVALID_ARGUMENT);
    CHECK(attachment == (UnityEmittedMatrixAttachment *)&shader);
    attachment = NULL;
    CHECK(!unity_emitted_matrix_attachment_replay(&input, NULL));
    CHECK(!unity_emitted_matrix_attachment_describe(NULL, NULL));
    CHECK(!unity_emitted_matrix_attachment_request(NULL, 0, NULL));
    CHECK(!unity_emitted_matrix_attachment_field(NULL, 0, 0, NULL));
    CHECK(!unity_emitted_matrix_attachment_read(NULL, 0, 0, NULL));
    CHECK(!unity_emitted_matrix_attachment_emission(NULL));
    unity_emitted_matrix_attachment_free(NULL);

    /* The private observation route must reject an injected compiler service
     * before any request/observer execution, even when the callback says yes. */
    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    PreprocessedSnippet snippet = {.source = "float4 vert(float4 p:POSITION):SV_POSITION{return p;}",
        .has_contract = true, .contract = contract};
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    size_t calls = 0;
    UnityGeneratedDomainCertificationInput domain = {.shader = &shader, .pass = &pass, .plan = &plan,
        .generated_snippet = &snippet, .d3d11_archive = &archive, .compile_profile = &profile,
        .broker = broker, .source_directory = ".", .source_basename = "Absence.shader", .pass_name = "",
        .compile_callback = unavailable_compile, .compile_context = &calls};
    UnityGeneratedDomainReport report;
    unity_generated_domain_report_init(&report);
    UnityGeneratedOwnedRequestObserver observer = {.observe = observe, .context = &calls};
    CHECK(unity_generated_domain_certify_owned_requests(&domain, &observer, &report) == UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT);
    CHECK(!calls);
    unity_generated_domain_report_free(&report);
    shaderlab_variant_plan_free(&plan);
    unity_compiler_snippet_contract_free(&contract);
    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(!stats.submitted_requests && !stats.compiler_process_starts);
    unity_compiler_broker_destroy(broker);
    free(segments[0]);
    free(segments[1]);
    common_file_bytes_dispose(&bytes);
    return true;
}

int main(void) {
    return absence_and_injected_boundary() ? 0 : 1;
}
