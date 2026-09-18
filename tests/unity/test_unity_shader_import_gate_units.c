#include "compiler/unity_shader_import_gate.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static UnityShaderImportGateOptions valid_options(void) {
    static const char* inputs[] = {"candidate.shader"};
    UnityShaderImportGateOptions options;
    unity_shader_import_gate_options_init(&options);
    options.unity_executable = "/Applications/Unity/Unity";
    options.expected_unity_version = "2021.3.35f1";
    options.bridge_path = "DXBCShaderImportGate.cs";
    options.report_path = "report.json";
    options.log_path = "editor.log";
    options.inputs = inputs;
    options.input_count = 1U;
    return options;
}

static int test_argument_validation(void) {
    UnityShaderImportGateOptions options = valid_options();
    CHECK(unity_shader_import_gate_options_validate(&options));
    CHECK(options.warning_policy == UNITY_SHADER_IMPORT_WARNINGS_FAIL);
    CHECK(options.keep_policy == UNITY_SHADER_IMPORT_KEEP_ON_FAILURE);

    options.unity_executable = NULL;
    CHECK(!unity_shader_import_gate_options_validate(&options));
    options = valid_options();
    options.expected_unity_version = "";
    CHECK(!unity_shader_import_gate_options_validate(&options));
    options = valid_options();
    options.bridge_path = NULL;
    CHECK(!unity_shader_import_gate_options_validate(&options));
    options = valid_options();
    options.report_path = options.log_path;
    CHECK(!unity_shader_import_gate_options_validate(&options));
    options = valid_options();
    options.input_count = 0U;
    CHECK(!unity_shader_import_gate_options_validate(&options));
    options = valid_options();
    options.warning_policy = (UnityShaderImportWarningPolicy)99;
    CHECK(!unity_shader_import_gate_options_validate(&options));
    options = valid_options();
    options.keep_policy = (UnityShaderImportKeepPolicy)99;
    CHECK(!unity_shader_import_gate_options_validate(&options));
    return 0;
}

static const char valid_allow_report[] =
    "{\n"
    " \"schema\":\"dxbc-sandbox-unity-shader-import/v1\",\n"
    " \"unity_version\":\"2021.3.35f1\",\n"
    " \"warning_policy\":\"allow\",\n"
    " \"candidate_count\":2,\n"
    " \"message_count\":2,\n"
    " \"error_count\":0,\n"
    " \"warning_count\":1,\n"
    " \"status\":\"passed\",\n"
    " \"shaders\":[\n"
    "  {\"source_path\":\"/tmp/a.shader\","
       "\"asset_path\":\"Assets/DXBCImportGate/00000000.shader\","
       "\"shader_name\":\"A\",\"imported\":true,\"messages\":["
       "{\"severity\":\"warning\",\"file\":\"a.shader\",\"line\":7,"
       "\"message\":\"escaped \\\"warning\\\"\","
       "\"platform\":\"d3d11\",\"details\":\"detail\"}]},\n"
    "  {\"source_path\":\"/tmp/b.shader\","
       "\"asset_path\":\"Assets/DXBCImportGate/00000001.shader\","
       "\"shader_name\":\"B\",\"imported\":true,\"messages\":["
       "{\"severity\":\"info\",\"file\":\"\",\"line\":0,"
       "\"message\":\"note\",\"platform\":\"metal\","
       "\"details\":\"\"}]}\n"
    " ]\n"
    "}\n";

static int test_valid_result_and_warning_policy(void) {
    UnityShaderImportGateSummary summary;
    memset(&summary, 0xa5, sizeof(summary));
    CHECK(unity_shader_import_gate_parse_result_json(
        (const uint8_t*)valid_allow_report,
        sizeof(valid_allow_report) - 1U, &summary) ==
        UNITY_SHADER_IMPORT_GATE_OK);
    CHECK(summary.candidate_count == 2U);
    CHECK(summary.message_count == 2U);
    CHECK(summary.error_count == 0U);
    CHECK(summary.warning_count == 1U);
    CHECK(summary.passed);
    CHECK(summary.warning_policy == UNITY_SHADER_IMPORT_WARNINGS_ALLOW);

    static const char fail_warnings[] =
        "{\"schema\":\"dxbc-sandbox-unity-shader-import/v1\","
        "\"unity_version\":\"2021.3.35f1\",\"warning_policy\":\"fail\","
        "\"candidate_count\":1,\"message_count\":1,\"error_count\":0,"
        "\"warning_count\":1,\"status\":\"failed\",\"shaders\":[{"
        "\"source_path\":\"x\",\"asset_path\":\"Assets/x.shader\","
        "\"shader_name\":\"X\",\"imported\":true,\"messages\":[{"
        "\"severity\":\"warning\",\"file\":\"x\",\"line\":1,"
        "\"message\":\"w\",\"platform\":\"d3d11\","
        "\"details\":\"\"}]}]}";
    CHECK(unity_shader_import_gate_parse_result_json(
        (const uint8_t*)fail_warnings, sizeof(fail_warnings) - 1U,
        &summary) == UNITY_SHADER_IMPORT_GATE_OK);
    CHECK(!summary.passed);
    CHECK(summary.warning_policy == UNITY_SHADER_IMPORT_WARNINGS_FAIL);
    return 0;
}

static int expect_invalid_unchanged(const char* text) {
    UnityShaderImportGateSummary summary;
    memset(&summary, 0x5a, sizeof(summary));
    UnityShaderImportGateSummary before = summary;
    CHECK(unity_shader_import_gate_parse_result_json(
        (const uint8_t*)text, strlen(text), &summary) ==
        UNITY_SHADER_IMPORT_GATE_RESULT_INVALID);
    CHECK(memcmp(&summary, &before, sizeof(summary)) == 0);
    return 0;
}

static int test_result_corruption_rejected(void) {
    char corrupted[sizeof(valid_allow_report) + 32U];
    memcpy(corrupted, valid_allow_report, sizeof(valid_allow_report));
    char* count = strstr(corrupted, "\"message_count\":2");
    CHECK(count != NULL);
    count[strlen("\"message_count\":")] = '3';
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    memcpy(corrupted, valid_allow_report, sizeof(valid_allow_report));
    char* status = strstr(corrupted, "\"status\":\"passed\"");
    CHECK(status != NULL);
    memcpy(status + strlen("\"status\":\""), "failed", 6U);
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    memcpy(corrupted, valid_allow_report, sizeof(valid_allow_report));
    char* severity = strstr(corrupted, "\"severity\":\"warning\"");
    CHECK(severity != NULL);
    severity[strlen("\"severity\":\"")] = 'x';
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    memcpy(corrupted, valid_allow_report, sizeof(valid_allow_report));
    size_t length = strlen(corrupted);
    memcpy(corrupted + length, "garbage", 8U);
    CHECK(expect_invalid_unchanged(corrupted) == 0);

    static const char duplicate_field[] =
        "{\"schema\":\"dxbc-sandbox-unity-shader-import/v1\","
        "\"schema\":\"dxbc-sandbox-unity-shader-import/v1\"}";
    CHECK(expect_invalid_unchanged(duplicate_field) == 0);

    static const char false_import_pass[] =
        "{\"schema\":\"dxbc-sandbox-unity-shader-import/v1\","
        "\"unity_version\":\"2021.3.35f1\",\"warning_policy\":\"fail\","
        "\"candidate_count\":1,\"message_count\":0,\"error_count\":0,"
        "\"warning_count\":0,\"status\":\"passed\",\"shaders\":[{"
        "\"source_path\":\"x\",\"asset_path\":\"Assets/x.shader\","
        "\"shader_name\":\"X\",\"imported\":false,\"messages\":[]}]}";
    CHECK(expect_invalid_unchanged(false_import_pass) == 0);

    static const char empty_name_pass[] =
        "{\"schema\":\"dxbc-sandbox-unity-shader-import/v1\","
        "\"unity_version\":\"2021.3.35f1\",\"warning_policy\":\"fail\","
        "\"candidate_count\":1,\"message_count\":0,\"error_count\":0,"
        "\"warning_count\":0,\"status\":\"passed\",\"shaders\":[{"
        "\"source_path\":\"x\",\"asset_path\":\"Assets/x.shader\","
        "\"shader_name\":\"\",\"imported\":true,\"messages\":[]}]}";
    CHECK(expect_invalid_unchanged(empty_name_pass) == 0);

    const char* closing = strrchr(valid_allow_report, '}');
    CHECK(closing != NULL);
    size_t complete_size = (size_t)(closing - valid_allow_report) + 1U;
    for (size_t cut = 0U; cut < complete_size; ++cut) {
        UnityShaderImportGateSummary summary;
        CHECK(unity_shader_import_gate_parse_result_json(
            (const uint8_t*)valid_allow_report, cut, &summary) ==
            UNITY_SHADER_IMPORT_GATE_RESULT_INVALID);
    }
    return 0;
}

int main(void) {
    if (test_argument_validation() != 0) return 1;
    if (test_valid_result_and_warning_policy() != 0) return 1;
    if (test_result_corruption_rejected() != 0) return 1;
    puts("unity shader import gate unit tests passed");
    return 0;
}
