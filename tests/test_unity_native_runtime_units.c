// SPDX-License-Identifier: GPL-3.0-only
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "compiler/unity_native_runtime_internal.h"
#include "compiler/unity_shaderlab_lift_capture.h"
#include "common/string_builder.h"
#include "dxbc/dxbc_parser.h"
#include "dxbc/usbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define PID() ((unsigned long)_getpid())
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define PID() ((unsigned long)getpid())
#define MKDIR(path) mkdir(path, 0700)
#define RMDIR(path) rmdir(path)
#endif

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #value); return 1; \
} } while (0)

static const ShaderRuntimeCaptureLimits limits = {4096, 512, 32, 4096, 512U * 1024U * 1024U,
                                                   2U * 1024U * 1024U * 1024U};

static void integer(StringBuilder *data, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(value >> (i * 8));
    sb_append_len(data, (const char *)bytes, 4);
}

static void span(StringBuilder *data, const void *bytes, size_t size) {
    integer(data, (uint32_t)size);
    sb_append_len(data, bytes, size);
}

static int portable(void) {
    CommonFileBytes metadata = {0}, programs = {0};
    CHECK(common_file_read_regular(NATIVE_TEST_METADATA, 65536, &metadata) == COMMON_FILE_OK);
    CHECK(common_file_read_regular(NATIVE_TEST_DXBC, 65536, &programs) == COMMON_FILE_OK);
    DXBCContainerView stages[2] = {{0}};
    DXBCUSBDTableView table;
    CHECK(dxbc_usbd_table_open(&table, programs.data, programs.size, NULL));
    for (uint32_t i = 0; i < table.record_count; ++i) {
        DXBCUSBDRecordView record;
        CHECK(dxbc_usbd_table_record(&table, i, &record));
        DXBCContainerView view;
        CHECK(dxbc_container_view_first(record.dxbc, record.dxbc_size, &view));
        DXBCContainer container = {0};
        CHECK(dxbc_parse(&container, view.data, view.size));
        if (container.program_type == DXBC_PROGRAM_TYPE_VERTEX)
            stages[0] = view;
        else if (container.program_type == DXBC_PROGRAM_TYPE_PIXEL)
            stages[1] = view;
        dxbc_free(&container);
    }
    CHECK(stages[0].data && stages[1].data);
    char root[128], path[160];
    CHECK(snprintf(root, sizeof(root), "native-observation-%lu", PID()) > 0);
    CHECK(snprintf(path, sizeof(path), "%s/metadata.assets", root) > 0);
    CHECK(MKDIR(root) == 0);
    CHECK(common_file_write_new_atomic(path, metadata.data, metadata.size) == COMMON_FILE_OK);
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19;
    profile.valid_apis = 311856;
    profile.d3d11_capabilities = profile.glcore_capabilities = 147179016;
    strcpy(profile.provenance, "native-observation-unit-test");
    UnityPlayerPackageAuthority *player = NULL;
    UnityPlayerPackageDiagnostic diagnostic;
    CHECK(unity_player_package_capture_d3d11(root, "metadata.assets", &limits, &profile, 7,
                                              &player, &diagnostic) == UNITY_PLAYER_PACKAGE_OK);
    CHECK(remove(path) == 0 && RMDIR(root) == 0);

    StringBuilder data;
    sb_init(&data);
    sb_append_len(&data, "DVUOBS01", 8);
    integer(&data, 1);
    integer(&data, 1);
    integer(&data, 12);
    uint8_t digest[32];
    for (unsigned i = 0; i < 6; ++i) {
        memset(digest, (int)i + 1, sizeof(digest));
        sb_append_len(&data, (const char *)digest, sizeof(digest));
    }
    span(&data, "metadata.assets", 15);
    common_sha256(metadata.data, metadata.size, digest);
    const size_t member_digest_offset = data.len;
    sb_append_len(&data, (const char *)digest, sizeof(digest));
    size_t records[12], artifacts[12][4];
    uint8_t pixels[256] = {0};
    for (unsigned i = 0; i < 12; ++i) {
        records[i] = data.len;
        memset(digest, (int)i + 1, sizeof(digest));
        sb_append_len(&data, (const char *)digest, sizeof(digest));
        for (unsigned artifact = 0; artifact < 4; ++artifact) {
            artifacts[i][artifact] = data.len;
            if (artifact < 2)
                span(&data, stages[artifact].data, stages[artifact].size);
            else if (artifact == 2)
                span(&data, pixels, sizeof(pixels));
            else
                span(&data, "inspection-only", 15);
        }
    }
    CHECK(sb_ok(&data));
    UnityNativeRuntimeSummary summary;
    CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, data.len, player, &summary) ==
          UNITY_NATIVE_RUNTIME_OK);
    CHECK(summary.observation_count == 12 && summary.equal_pair_count == 6 && summary.member_count == 1);
    CHECK(!unity_native_runtime_describe(NULL, &summary));
    /* Every byte truncation remains unavailable, including complete prefixes. */
    for (size_t size = 0; size < data.len; ++size)
        CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, size, player, &summary) !=
              UNITY_NATIVE_RUNTIME_OK);
    const size_t invalid_offsets[] = {0, 8, 12, 16, 212, 216};
    for (size_t i = 0; i < sizeof(invalid_offsets) / sizeof(*invalid_offsets); ++i) {
        const size_t at = invalid_offsets[i];
        data.buf[at] ^= 0x40;
        CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, data.len, player, &summary) !=
              UNITY_NATIVE_RUNTIME_OK);
        data.buf[at] ^= 0x40;
    }
    data.buf[member_digest_offset] ^= 1;
    CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, data.len, player, &summary) ==
          UNITY_NATIVE_RUNTIME_BINDING_MISMATCH);
    data.buf[member_digest_offset] ^= 1;
    memcpy(data.buf + records[1], data.buf + records[0], 32);
    CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, data.len, player, &summary) ==
          UNITY_NATIVE_RUNTIME_CAPTURE_INVALID);
    memset(data.buf + records[1], 2, 32);
    data.buf[artifacts[1][2] + 4] ^= 1;
    CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, data.len, player, &summary) ==
          UNITY_NATIVE_RUNTIME_OBSERVATIONS_DIFFER);
    data.buf[artifacts[1][2] + 4] ^= 1;
    data.buf[artifacts[1][0] + 4] ^= 1;
    CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, data.len, player, &summary) ==
          UNITY_NATIVE_RUNTIME_CAPTURE_INVALID);
    data.buf[artifacts[1][0] + 4] ^= 1;
    sb_append_char(&data, 'x');
    CHECK(unity_native_runtime_inspect((uint8_t *)data.buf, data.len, player, &summary) ==
          UNITY_NATIVE_RUNTIME_CAPTURE_INVALID);
    UnityNativeRuntime *runtime = (UnityNativeRuntime *)(uintptr_t)1;
    UnityNativeRuntimeDiagnostic capture;
    CHECK(unity_native_runtime_capture(NULL, &runtime, &capture) == UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT);
    CHECK(!runtime && capture.exit_code == -1);
    unity_native_runtime_free(NULL);
    sb_free(&data);
    unity_player_package_free(player);
    common_file_bytes_dispose(&programs);
    common_file_bytes_dispose(&metadata);
    return 0;
}

static int selection(const UnityNativeRuntime *native, const UnityPlayerPackageAuthority *player,
                      const UnityCompileProfile *profile, const ShaderCatalog *target,
                      const ShaderCatalog *candidate, const TypeTreeSchemaRegistry *registry,
                      const char *project, const char *includes) {
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(project, includes);
    CHECK(broker);
    HLSLLiftLimits budget = {2, 128, 60000};
    UnityShaderLabLiftCaptureInput input = {
        .catalog = target, .record = target->records, .registry = registry, .profile = profile,
        .broker = broker, .source_path = "Assets/Contract.shader", .source_directory = project,
        .source_basename = "Contract.shader", .limits = &budget};
    UnityShaderLabLiftCapture *capture = NULL;
    UnityShaderLabLiftCaptureReport captured;
    CHECK(unity_shaderlab_lift_capture(&input, &capture, &captured) == UNITY_SHADERLAB_CAPTURE_OK);
    UnityNativeRuntimeSummary observed;
    UnityPlayerPackageSummary package;
    CHECK(unity_native_runtime_describe(native, &observed));
    CHECK(unity_player_package_describe(player, &package));
    WholeShaderSubjectDescriptor descriptor;
    CHECK(unity_shaderlab_lift_capture_subject(capture, &descriptor));
    memcpy(descriptor.player_profile_digest, package.player.profile_digest, 32);
    memcpy(descriptor.runtime_environment_digest, observed.native_environment_digest, 32);
    memcpy(descriptor.candidate_release_digest, observed.candidate_release_digest, 32);
    memcpy(descriptor.producer_fingerprint, observed.authority_digest, 32);
    WholeShaderSubject *subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
    WholeShaderEvidence *evidence = NULL;
    UnityShaderSelectionEvidenceReport report;
    CHECK(unity_shaderlab_lift_capture_selection_evidence(
              capture, candidate, candidate->records, registry, player, native, subject,
              &evidence, &report) == WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary summary;
    CHECK(whole_shader_evidence_describe(evidence, &summary) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(summary.status == WHOLE_SHADER_PLANE_PASS && summary.matched_item_count == 4);
    whole_shader_evidence_free(evidence);
    evidence = NULL;
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        WholeShaderSubjectDescriptor changed = descriptor;
        uint8_t *digests[] = {changed.runtime_environment_digest, changed.player_profile_digest,
            changed.candidate_release_digest, changed.target_object_payload_digest,
            changed.schema_authority_digest, changed.verification_scope_digest,
            changed.dependency_map_digest, changed.compiler_session_digest,
            changed.candidate_source_digest};
        digests[mutation][0] ^= 1;
        WholeShaderSubject *wrong = NULL;
        CHECK(whole_shader_subject_create(&wrong, &changed) == WHOLE_SHADER_SUBJECT_OK);
        CHECK(unity_shaderlab_lift_capture_selection_evidence(
                  capture, candidate, candidate->records, registry, player, native, wrong,
                  &evidence, &report) == WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
        CHECK(!evidence);
        whole_shader_subject_free(wrong);
    }
    CHECK(unity_shaderlab_lift_capture_selection_evidence(
              capture, candidate, candidate->records, registry, NULL, native, subject,
              &evidence, &report) == WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!evidence);
    ShaderCatalogRecord unowned = *candidate->records;
    CHECK(unity_shaderlab_lift_capture_selection_evidence(
              capture, candidate, &unowned, registry, player, native, subject,
              &evidence, &report) == WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!evidence);
    CHECK(unity_shaderlab_lift_capture_selection_evidence(
              capture, candidate, candidate->records, registry, player, NULL, subject,
              &evidence, &report) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_evidence_describe(evidence, &summary) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(summary.status == WHOLE_SHADER_PLANE_UNAVAILABLE &&
          report.availability == UNITY_SHADER_SELECTION_NATIVE_UNAVAILABLE);
    whole_shader_evidence_free(evidence);
    whole_shader_subject_free(subject);
    unity_shaderlab_lift_capture_free(capture);
    unity_compiler_broker_destroy(broker);
    puts("selection=pass binding-negatives=11 missing-native=unavailable");
    return 0;
}

static int live(int argc, char **argv) {
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    CHECK(unity_compile_profile_load(argv[4], &profile) == UNITY_COMPILE_PROFILE_OK);
    UnityPlayerPackageAuthority *player = NULL;
    UnityPlayerPackageDiagnostic package_diagnostic;
    CHECK(unity_player_package_capture_d3d11(argv[3], "RuntimeProbe_Data/globalgamemanagers", &limits,
                                              &profile, 7, &player, &package_diagnostic) == UNITY_PLAYER_PACKAGE_OK);
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(&registry, NATIVE_TEST_REGISTRY) == TYPETREE_SCHEMA_OK);
    ShaderCatalog target, candidate;
    shader_catalog_init(&target);
    shader_catalog_init(&candidate);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.retain_source_snapshots = true;
    options.schema_registry = &registry;
    const char *target_path = argv[1], *candidate_path = argv[2];
    CHECK(shader_catalog_build(&target_path, 1, &options, &target) == SHADER_CATALOG_OK && target.record_count == 1);
    CHECK(shader_catalog_build(&candidate_path, 1, &options, &candidate) == SHADER_CATALOG_OK && candidate.record_count == 1);
    UnityNativeRuntimeOptions input = {.target_catalog = &target, .target_record = target.records,
        .candidate_catalog = &candidate, .candidate_record = candidate.records, .registry = &registry,
        .player = player, .python = argv[5], .client_directory = argv[6], .ssh_config = argv[7],
        .policy_path = argv[8], .jobs_path = argv[9], .output_path = argv[10], .timeout_ms = 120000};
    UnityNativeRuntime *runtime = NULL;
    UnityNativeRuntimeDiagnostic diagnostic;
    const UnityNativeRuntimeStatus status = unity_native_runtime_capture(&input, &runtime, &diagnostic);
    printf("native=%s process=%s exit=%d file=%s input=%zu\n", unity_native_runtime_status_name(status),
           common_process_status_name(diagnostic.process_status), diagnostic.exit_code,
           common_file_status_name(diagnostic.file_status), diagnostic.input_index);
    CHECK(status == UNITY_NATIVE_RUNTIME_OK && runtime);
    UnityNativeRuntimeSummary summary;
    CHECK(unity_native_runtime_describe(runtime, &summary));
    CHECK(summary.observation_count == 12 && summary.equal_pair_count == 6);
    char hex[65];
    common_sha256_digest_to_hex(summary.authority_digest, hex);
    printf("observations=%u pairs=%u members=%u authority=%s\n", summary.observation_count,
           summary.equal_pair_count, summary.member_count, hex);
    if (argc == 13)
        CHECK(selection(runtime, player, &profile, &target, &candidate, &registry,
                         argv[11], argv[12]) == 0);
    unity_native_runtime_free(runtime);
    runtime = NULL;
    CHECK(unity_native_runtime_capture(&input, &runtime, &diagnostic) == UNITY_NATIVE_RUNTIME_OUTPUT_EXISTS);
    CHECK(!runtime);
    shader_catalog_dispose(&candidate);
    shader_catalog_dispose(&target);
    typetree_schema_registry_dispose(&registry);
    unity_player_package_free(player);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1)
        return portable();
    if (argc == 11 || argc == 13)
        return live(argc, argv);
    fprintf(stderr, "Usage: %s [TARGET CANDIDATE PLAYER_ROOT PROFILE PYTHON CLIENT SSH_CONFIG POLICY JOBS OUTPUT [PROJECT INCLUDES]]\n", argv[0]);
    return 2;
}
