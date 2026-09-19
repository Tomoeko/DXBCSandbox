// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shaderlab_lift_capture.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #x);                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 5);
    UnityShaderLabLiftCapture *capture = NULL;
    UnityShaderLabLiftCaptureReport report;
    CHECK(unity_shaderlab_lift_capture(NULL, &capture, &report) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    CHECK(!capture && !unity_shaderlab_lift_capture_result(NULL));
    unity_shaderlab_lift_capture_free(NULL);
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(&registry, CAPTURE_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = &registry;
    options.retain_source_snapshots = true;
    const char *path = argc == 5 ? argv[1] : CAPTURE_EMPTY_FIXTURE;
    CHECK(shader_catalog_build(&path, 1, &options, &catalog) == SHADER_CATALOG_OK);
    CHECK(catalog.record_count == 1 && shader_catalog_is_complete(&catalog));
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19;
    profile.valid_apis = 295472;
    strcpy(profile.provenance, "synthetic-unit-test");
    if (argc == 5)
        CHECK(unity_compile_profile_load(argv[2], &profile) == UNITY_COMPILE_PROFILE_OK);
    UnityCompilerBroker *broker =
        unity_compiler_broker_create_lazy(argc == 5 ? argv[3] : ".", argc == 5 ? argv[4] : NULL);
    CHECK(broker);
    HLSLLiftLimits limits = {2, 128, 60000};
    UnityShaderLabLiftCaptureInput input = {
        .catalog = &catalog,
        .record = catalog.records,
        .registry = &registry,
        .profile = &profile,
        .broker = broker,
        .source_path = "Assets/Captured.shader",
        .source_directory = argc == 5 ? argv[3] : ".",
        .source_basename = "Captured.shader",
        .limits = &limits,
    };
    CHECK(unity_shaderlab_lift_capture(&input, NULL, &report) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    CHECK(unity_shaderlab_lift_capture(&input, &capture, NULL) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    ShaderCatalogRecord unowned = *catalog.records;
    input.record = &unowned;
    CHECK(unity_shaderlab_lift_capture(&input, &capture, &report) ==
          UNITY_SHADERLAB_CAPTURE_SOURCE_UNAVAILABLE);
    CHECK(report.source_status == SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED && !capture);
    input.record = catalog.records;
    UnityShaderLabLiftCaptureStatus status =
        unity_shaderlab_lift_capture(&input, &capture, &report);
    printf("capture=%d lift=%d archive=%d\n", status, report.lift_status, report.archive_status);
    if (argc == 1) {
        CHECK(status == UNITY_SHADERLAB_CAPTURE_ARCHIVE_UNAVAILABLE && !capture);
        CHECK(report.archive_status == SHADER_OBJECT_D3D11_PLATFORM_ABSENT);
        UnityCompilerBrokerStats stats;
        unity_compiler_broker_get_stats(broker, &stats);
        CHECK(stats.compiler_process_starts == 0);
    } else {
        CHECK(status == UNITY_SHADERLAB_CAPTURE_OK && capture);
        const UnityShaderLabLiftArtifact *accepted =
            unity_shaderlab_lift_accepted(unity_shaderlab_lift_capture_result(capture));
        CHECK(accepted && accepted->high_level && accepted->certified_pass_count > 0);
        uint8_t digest[32];
        common_sha256(accepted->source.buf, accepted->source.len, digest);
        CHECK(memcmp(digest, report.accepted_source_digest, 32) == 0);
        CHECK(unity_compile_profile_fingerprint(&profile, digest) == UNITY_COMPILE_PROFILE_OK);
        CHECK(memcmp(digest, report.profile_digest, 32) == 0);
        printf("high_level=%d helper=%d passes=%zu\n", accepted->high_level,
               accepted->unity_uv_helpers, accepted->certified_pass_count);
    }
    unity_shaderlab_lift_capture_free(capture);
    unity_compiler_broker_destroy(broker);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    return 0;
}
