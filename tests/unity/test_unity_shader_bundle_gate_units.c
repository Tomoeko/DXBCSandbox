#include "compiler/unity_shader_bundle_gate.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static UnityShaderBundleGateOptions valid_options(void) {
    static const char* inputs[] = {"candidate.shader"};
    UnityShaderBundleGateOptions options;
    unity_shader_bundle_gate_options_init(&options);
    options.unity_executable = "/Applications/Unity/Unity";
    options.expected_unity_version = "2021.3.35f1";
    options.bridge_path = "DXBCShaderBundleGate.cs";
    options.report_path = "report.json";
    options.log_path = "editor.log";
    options.output_bundle_path = "release.bundle";
    options.inputs = inputs;
    options.input_count = 1U;
    return options;
}

static int test_argument_validation(void) {
    UnityShaderBundleGateOptions options = valid_options();
    CHECK(unity_shader_bundle_gate_options_validate(&options));
    CHECK(options.warning_policy == UNITY_SHADER_IMPORT_WARNINGS_FAIL);
    CHECK(options.keep_policy == UNITY_SHADER_IMPORT_KEEP_ON_FAILURE);
    CHECK(options.target == UNITY_SHADER_BUNDLE_TARGET_MACOS);
    CHECK(options.backend == UNITY_SHADER_BUNDLE_BACKEND_METAL);

    options.output_bundle_path = NULL;
    CHECK(!unity_shader_bundle_gate_options_validate(&options));
    options = valid_options();
    options.output_bundle_path = options.report_path;
    CHECK(!unity_shader_bundle_gate_options_validate(&options));
    options = valid_options();
    options.target = UNITY_SHADER_BUNDLE_TARGET_WINDOWS64;
    CHECK(!unity_shader_bundle_gate_options_validate(&options));
    options.backend = UNITY_SHADER_BUNDLE_BACKEND_D3D11;
    CHECK(unity_shader_bundle_gate_options_validate(&options));

    CHECK(unity_shader_bundle_target_supports_backend(
        UNITY_SHADER_BUNDLE_TARGET_MACOS,
        UNITY_SHADER_BUNDLE_BACKEND_OPENGLCORE));
    CHECK(!unity_shader_bundle_target_supports_backend(
        UNITY_SHADER_BUNDLE_TARGET_MACOS,
        UNITY_SHADER_BUNDLE_BACKEND_D3D11));
    CHECK(unity_shader_bundle_target_supports_backend(
        UNITY_SHADER_BUNDLE_TARGET_LINUX64,
        UNITY_SHADER_BUNDLE_BACKEND_VULKAN));
    return 0;
}

static int test_deterministic_candidate_guid(void) {
    static const char source[] = "Shader \"A\" {}";
    char first[33];
    char repeated[33];
    char second_index[33];
    CHECK(unity_shader_bundle_candidate_guid(
        source, sizeof(source) - 1U, 0U, first));
    CHECK(unity_shader_bundle_candidate_guid(
        source, sizeof(source) - 1U, 0U, repeated));
    CHECK(unity_shader_bundle_candidate_guid(
        source, sizeof(source) - 1U, 1U, second_index));
    CHECK(strcmp(first, "336710dacf6745f8cb210a89af5011d7") == 0);
    CHECK(strcmp(repeated, first) == 0);
    CHECK(strcmp(second_index, "c7017a0f811b8eab48bf101f05a31924") == 0);
    CHECK(strcmp(first, second_index) != 0);
    CHECK(!unity_shader_bundle_candidate_guid(NULL, 1U, 0U, first));
    CHECK(!unity_shader_bundle_candidate_guid(source, sizeof(source), 0U,
                                              NULL));
    return 0;
}

static int test_existing_output_short_circuits_without_unity(void) {
    static const char output[] =
        "dxbc_bundle_gate_existing_output.unit.tmp";
    (void)remove(output);
    FILE* file = fopen(output, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("owned", 1U, 5U, file) == 5U);
    CHECK(fclose(file) == 0);

    UnityShaderBundleGateOptions options = valid_options();
    options.unity_executable = "definitely-not-a-unity-executable";
    options.output_bundle_path = output;
    UnityShaderBundleGateRunResult result;
    CHECK(unity_shader_bundle_gate_run(&options, &result) ==
          UNITY_SHADER_BUNDLE_GATE_OUTPUT_EXISTS);
    CHECK(result.unity_exit_code == -1);
    CHECK(!result.bundle_published);
    CHECK(!result.workspace_preserved);

    file = fopen(output, "rb");
    CHECK(file != NULL);
    char bytes[6] = {0};
    CHECK(fread(bytes, 1U, 5U, file) == 5U);
    CHECK(fclose(file) == 0);
    CHECK(strcmp(bytes, "owned") == 0);
    CHECK(remove(output) == 0);
    return 0;
}

static const char valid_pass_report[] =
    "{"
    "\"schema\":\"dxbc-sandbox-unity-shader-bundle/v1\","
    "\"unity_version\":\"2021.3.35f1\","
    "\"build_target\":\"StandaloneOSX\","
    "\"graphics_backend\":\"metal\","
    "\"warning_policy\":\"fail\","
    "\"candidate_count\":1,\"message_count\":0,"
    "\"error_count\":0,\"warning_count\":0,\"failure_count\":0,"
    "\"status\":\"passed\","
    "\"output_bundle_path\":\"/tmp/release.bundle\","
    "\"bundle_built\":true,\"bundle_size\":123,"
    "\"bundle_sha256\":"
      "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"shaders\":[{"
      "\"source_path\":\"/tmp/a.shader\","
      "\"asset_path\":\"Assets/DXBCReleaseBundle/00000000.shader\","
      "\"bundle_asset_name\":"
        "\"Assets/DXBCReleaseBundle/00000000.shader\","
      "\"shader_name\":\"Hidden/A\",\"imported\":true,"
      "\"messages\":[]}],"
    "\"build_messages\":[],\"failures\":[]"
    "}";

static int test_valid_pass(void) {
    UnityShaderBundleGateSummary summary;
    memset(&summary, 0xa5, sizeof(summary));
    CHECK(unity_shader_bundle_gate_parse_result_json(
        (const uint8_t*)valid_pass_report,
        sizeof(valid_pass_report) - 1U, &summary) ==
        UNITY_SHADER_BUNDLE_GATE_OK);
    CHECK(summary.passed);
    CHECK(summary.bundle_built);
    CHECK(summary.all_candidates_imported);
    CHECK(summary.bundle_size == 123U);
    CHECK(summary.target == UNITY_SHADER_BUNDLE_TARGET_MACOS);
    CHECK(summary.backend == UNITY_SHADER_BUNDLE_BACKEND_METAL);
    CHECK(strcmp(summary.unity_version, "2021.3.35f1") == 0);
    CHECK(strcmp(summary.output_bundle_path, "/tmp/release.bundle") == 0);
    CHECK(strlen(summary.candidate_mapping_sha256) == 64U);
    return 0;
}

static int expect_invalid_unchanged(const char* text) {
    UnityShaderBundleGateSummary summary;
    memset(&summary, 0x5a, sizeof(summary));
    UnityShaderBundleGateSummary before = summary;
    CHECK(unity_shader_bundle_gate_parse_result_json(
        (const uint8_t*)text, strlen(text), &summary) ==
        UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID);
    CHECK(memcmp(&summary, &before, sizeof(summary)) == 0);
    return 0;
}

static int test_failure_statuses(void) {
    static const char target_unavailable[] =
        "{\"schema\":\"dxbc-sandbox-unity-shader-bundle/v1\","
        "\"unity_version\":\"2021.3.35f1\","
        "\"build_target\":\"StandaloneWindows64\","
        "\"graphics_backend\":\"d3d11\",\"warning_policy\":\"fail\","
        "\"candidate_count\":1,\"message_count\":0,\"error_count\":0,"
        "\"warning_count\":0,\"failure_count\":1,"
        "\"status\":\"target_unavailable\","
        "\"output_bundle_path\":\"/tmp/windows.bundle\","
        "\"bundle_built\":false,\"bundle_size\":0,"
        "\"bundle_sha256\":\"\",\"shaders\":[{"
        "\"source_path\":\"a\","
        "\"asset_path\":\"Assets/DXBCReleaseBundle/00000000.shader\","
        "\"bundle_asset_name\":"
          "\"Assets/DXBCReleaseBundle/00000000.shader\","
        "\"shader_name\":\"\",\"imported\":false,\"messages\":[]}],"
        "\"build_messages\":[],\"failures\":[{"
        "\"kind\":\"target_unavailable\",\"message\":\"module absent\"}]}";
    UnityShaderBundleGateSummary summary;
    CHECK(unity_shader_bundle_gate_parse_result_json(
        (const uint8_t*)target_unavailable,
        sizeof(target_unavailable) - 1U, &summary) ==
        UNITY_SHADER_BUNDLE_GATE_OK);
    CHECK(summary.reported_status ==
          UNITY_SHADER_BUNDLE_GATE_TARGET_UNAVAILABLE);
    CHECK(!summary.bundle_built);
    CHECK(!summary.all_candidates_imported);

    static const char diagnostics[] =
        "{\"schema\":\"dxbc-sandbox-unity-shader-bundle/v1\","
        "\"unity_version\":\"2021.3.35f1\","
        "\"build_target\":\"StandaloneOSX\","
        "\"graphics_backend\":\"metal\",\"warning_policy\":\"fail\","
        "\"candidate_count\":1,\"message_count\":1,\"error_count\":0,"
        "\"warning_count\":1,\"failure_count\":0,"
        "\"status\":\"diagnostics_found\","
        "\"output_bundle_path\":\"/tmp/mac.bundle\","
        "\"bundle_built\":false,\"bundle_size\":0,"
        "\"bundle_sha256\":\"\",\"shaders\":[{"
        "\"source_path\":\"a\","
        "\"asset_path\":\"Assets/DXBCReleaseBundle/00000000.shader\","
        "\"bundle_asset_name\":"
          "\"Assets/DXBCReleaseBundle/00000000.shader\","
        "\"shader_name\":\"A\",\"imported\":true,\"messages\":[{"
        "\"severity\":\"warning\",\"file\":\"a\",\"line\":1,"
        "\"message\":\"implicit truncation\",\"platform\":\"metal\","
        "\"details\":\"\"}]}],\"build_messages\":[],\"failures\":[]}";
    CHECK(unity_shader_bundle_gate_parse_result_json(
        (const uint8_t*)diagnostics, sizeof(diagnostics) - 1U, &summary) ==
        UNITY_SHADER_BUNDLE_GATE_OK);
    CHECK(summary.reported_status ==
          UNITY_SHADER_BUNDLE_GATE_DIAGNOSTICS_FOUND);
    CHECK(summary.warning_count == 1U);
    return 0;
}

static int test_corruption_rejected(void) {
    char corrupted[sizeof(valid_pass_report) + 32U];
    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* size = strstr(corrupted, "\"bundle_size\":123");
    CHECK(size != NULL);
    memcpy(size + strlen("\"bundle_size\":"), "000", 3U);
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* imported = strstr(corrupted, "\"imported\":true");
    CHECK(imported != NULL);
    memcpy(imported + strlen("\"imported\":"), "false", 5U);
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* hash = strstr(corrupted, "0123456789abcdef");
    CHECK(hash != NULL);
    hash[0] = 'A';
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* asset = strstr(corrupted,
        "Assets/DXBCReleaseBundle/00000000.shader");
    CHECK(asset != NULL);
    asset[strlen("Assets/DXBCReleaseBundle/")] = '1';
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    size_t length = strlen(corrupted);
    memcpy(corrupted + length, "garbage", 8U);
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    const char* closing = strrchr(valid_pass_report, '}');
    CHECK(closing != NULL);
    size_t complete_size = (size_t)(closing - valid_pass_report) + 1U;
    for (size_t cut = 0U; cut < complete_size; ++cut) {
        UnityShaderBundleGateSummary summary;
        CHECK(unity_shader_bundle_gate_parse_result_json(
            (const uint8_t*)valid_pass_report, cut, &summary) ==
            UNITY_SHADER_BUNDLE_GATE_RESULT_INVALID);
    }
    return 0;
}

int main(void) {
    if (test_argument_validation() != 0) return 1;
    if (test_deterministic_candidate_guid() != 0) return 1;
    if (test_existing_output_short_circuits_without_unity() != 0) return 1;
    if (test_valid_pass() != 0) return 1;
    if (test_failure_statuses() != 0) return 1;
    if (test_corruption_rejected() != 0) return 1;
    puts("unity shader bundle gate unit tests passed");
    return 0;
}
