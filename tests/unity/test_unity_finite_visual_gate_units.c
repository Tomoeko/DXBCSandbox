#include "compiler/unity_finite_visual_gate.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SHA_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define SHA_D "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define SHA_E "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
#define SHA_F "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
#define SHA_ZERO "0000000000000000000000000000000000000000000000000000000000000000"

static UnityFiniteVisualGateOptions valid_options(void) {
    UnityFiniteVisualGateOptions options;
    unity_finite_visual_gate_options_init(&options);
    options.unity_executable = "Unity";
    options.expected_unity_version = "2021.3.35f1";
    options.bridge_path = "DXBCFiniteVisualGate.cs";
    options.baseline_shader_path = "baseline.shader";
    options.candidate_shader_path = "candidate.shader";
    options.fixture_path = "fixture.tsv";
    options.report_path = "report.tsv";
    options.log_path = "editor.log";
    options.baseline_pixels_path = "baseline.rgba32f";
    options.candidate_pixels_path = "candidate.rgba32f";
    options.backend = UNITY_FINITE_VISUAL_BACKEND_METAL;
    return options;
}

static int test_argument_validation(void) {
    UnityFiniteVisualGateOptions options = valid_options();
    CHECK(unity_finite_visual_gate_options_validate(&options));
    CHECK(options.keep_policy == UNITY_FINITE_VISUAL_KEEP_ON_FAILURE);

    options.fixture_path = NULL;
    CHECK(!unity_finite_visual_gate_options_validate(&options));
    options = valid_options();
    options.report_path = options.log_path;
    CHECK(!unity_finite_visual_gate_options_validate(&options));
    options = valid_options();
    options.candidate_pixels_path = options.baseline_pixels_path;
    CHECK(!unity_finite_visual_gate_options_validate(&options));
    options = valid_options();
    options.backend = (UnityFiniteVisualBackend)99;
    CHECK(!unity_finite_visual_gate_options_validate(&options));
    options = valid_options();
    options.keep_policy = (UnityFiniteVisualKeepPolicy)99;
    CHECK(!unity_finite_visual_gate_options_validate(&options));
    options = valid_options();
    options.expected_unity_version = "2021.3.35f1\n-forged";
    CHECK(!unity_finite_visual_gate_options_validate(&options));
    options = valid_options();
    options.expected_unity_version =
        "1234567890123456789012345678901234567890123456789012345678901234";
    CHECK(!unity_finite_visual_gate_options_validate(&options));

    CHECK(strcmp(unity_finite_visual_backend_name(
                     UNITY_FINITE_VISUAL_BACKEND_D3D11), "d3d11") == 0);
    CHECK(strcmp(unity_finite_visual_backend_name(
                     UNITY_FINITE_VISUAL_BACKEND_OPENGLCORE),
                 "openglcore") == 0);
    CHECK(strcmp(unity_finite_visual_gate_status_name(
                     UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC),
                 "nondeterministic") == 0);
    return 0;
}

static const char valid_pass_report[] =
    "dxbc-sandbox-unity-finite-visual/v1\n"
    "status\tok\n"
    "unity_version\tMjAyMS4zLjM1ZjE=\n"
    "backend\tmetal\n"
    "device_name\tR1BV\n"
    "device_vendor\tVmVuZG9y\n"
    "device_version\tVmVyc2lvbg==\n"
    "requested_color_space\tbGluZWFy\n"
    "color_space\tbGluZWFy\n"
    "width\t2\n"
    "height\t2\n"
    "pass\t0\n"
    "fixture_sha256\t" SHA_A "\n"
    "bridge_sha256\t" SHA_B "\n"
    "baseline_source_sha256\t" SHA_C "\n"
    "candidate_source_sha256\t" SHA_D "\n"
    "baseline_imported\t1\n"
    "candidate_imported\t1\n"
    "baseline_stable\t1\n"
    "candidate_stable\t1\n"
    "pixel_equal\t1\n"
    "pixel_byte_count\t64\n"
    "first_mismatch_offset\t18446744073709551615\n"
    "baseline_pixels_sha256\t" SHA_E "\n"
    "candidate_pixels_sha256\t" SHA_E "\n"
    "diagnostic_count\t0\n"
    "error_count\t0\n"
    "warning_count\t0\n"
    "failure_count\t0\n"
    "end\n";

static bool bytes_are_zero(const uint8_t* bytes, size_t size) {
    uint8_t combined = 0U;
    for (size_t index = 0U; index < size; ++index) combined |= bytes[index];
    return combined == 0U;
}

static int test_valid_pass_result(void) {
    UnityFiniteVisualGateSummary summary;
    memset(&summary, 0xa5, sizeof(summary));
    CHECK(unity_finite_visual_gate_parse_result(
        (const uint8_t*)valid_pass_report, sizeof(valid_pass_report) - 1U,
        &summary) == UNITY_FINITE_VISUAL_GATE_OK);
    CHECK(summary.reported_status == UNITY_FINITE_VISUAL_GATE_OK);
    CHECK(summary.passed);
    CHECK(summary.backend == UNITY_FINITE_VISUAL_BACKEND_METAL);
    CHECK(summary.width == 2U && summary.height == 2U);
    CHECK(summary.pixel_byte_count == 64U);
    CHECK(summary.first_mismatch_offset == UINT64_MAX);
    CHECK(strcmp(summary.unity_version, "2021.3.35f1") == 0);
    CHECK(strcmp(summary.graphics_device_name, "GPU") == 0);
    /* Parsing bridge text alone must not manufacture evidence authority.
     * The launcher binds these coordinates only after checking exact inputs,
     * requested Editor/backend pins, and raw pixel artifacts. */
    CHECK(bytes_are_zero(summary.render_case_identity,
                         sizeof(summary.render_case_identity)));
    CHECK(bytes_are_zero(summary.authority_digest,
                         sizeof(summary.authority_digest)));
    return 0;
}

static const char valid_mismatch_report[] =
    "dxbc-sandbox-unity-finite-visual/v1\n"
    "status\tpixel-mismatch\n"
    "unity_version\tMjAyMS4zLjM1ZjE=\n"
    "backend\tmetal\n"
    "device_name\tR1BV\n"
    "device_vendor\tVmVuZG9y\n"
    "device_version\tVmVyc2lvbg==\n"
    "requested_color_space\tbGluZWFy\n"
    "color_space\tbGluZWFy\n"
    "width\t2\n"
    "height\t2\n"
    "pass\t0\n"
    "fixture_sha256\t" SHA_A "\n"
    "bridge_sha256\t" SHA_B "\n"
    "baseline_source_sha256\t" SHA_C "\n"
    "candidate_source_sha256\t" SHA_D "\n"
    "baseline_imported\t1\n"
    "candidate_imported\t1\n"
    "baseline_stable\t1\n"
    "candidate_stable\t1\n"
    "pixel_equal\t0\n"
    "pixel_byte_count\t64\n"
    "first_mismatch_offset\t7\n"
    "baseline_pixels_sha256\t" SHA_E "\n"
    "candidate_pixels_sha256\t" SHA_F "\n"
    "diagnostic_count\t0\n"
    "error_count\t0\n"
    "warning_count\t0\n"
    "failure_count\t0\n"
    "end\n";

static const char valid_diagnostic_report[] =
    "dxbc-sandbox-unity-finite-visual/v1\n"
    "status\tdiagnostics-found\n"
    "unity_version\tMjAyMS4zLjM1ZjE=\n"
    "backend\tmetal\n"
    "device_name\tR1BV\n"
    "device_vendor\tVmVuZG9y\n"
    "device_version\tVmVyc2lvbg==\n"
    "requested_color_space\tbGluZWFy\n"
    "color_space\tbGluZWFy\n"
    "width\t2\n"
    "height\t2\n"
    "pass\t0\n"
    "fixture_sha256\t" SHA_A "\n"
    "bridge_sha256\t" SHA_B "\n"
    "baseline_source_sha256\t" SHA_C "\n"
    "candidate_source_sha256\t" SHA_D "\n"
    "baseline_imported\t1\n"
    "candidate_imported\t1\n"
    "baseline_stable\t0\n"
    "candidate_stable\t0\n"
    "pixel_equal\t0\n"
    "pixel_byte_count\t64\n"
    "first_mismatch_offset\t18446744073709551615\n"
    "baseline_pixels_sha256\t" SHA_ZERO "\n"
    "candidate_pixels_sha256\t" SHA_ZERO "\n"
    "diagnostic_count\t1\n"
    "error_count\t0\n"
    "warning_count\t1\n"
    "failure_count\t0\n"
    "diagnostic\tcandidate\twarning\t7\t\tbWVzc2FnZQ==\t\t\n"
    "end\n";

static const char valid_unsupported_report[] =
    "dxbc-sandbox-unity-finite-visual/v1\n"
    "status\tunsupported-fixture\n"
    "unity_version\tMjAyMS4zLjM1ZjE=\n"
    "backend\tunsupported\n"
    "device_name\tR1BV\n"
    "device_vendor\tVmVuZG9y\n"
    "device_version\tVmVyc2lvbg==\n"
    "requested_color_space\tbGluZWFy\n"
    "color_space\tbGluZWFy\n"
    "width\t2\n"
    "height\t2\n"
    "pass\t0\n"
    "fixture_sha256\t" SHA_A "\n"
    "bridge_sha256\t" SHA_B "\n"
    "baseline_source_sha256\t" SHA_C "\n"
    "candidate_source_sha256\t" SHA_D "\n"
    "baseline_imported\t1\n"
    "candidate_imported\t1\n"
    "baseline_stable\t0\n"
    "candidate_stable\t0\n"
    "pixel_equal\t0\n"
    "pixel_byte_count\t64\n"
    "first_mismatch_offset\t18446744073709551615\n"
    "baseline_pixels_sha256\t" SHA_ZERO "\n"
    "candidate_pixels_sha256\t" SHA_ZERO "\n"
    "diagnostic_count\t0\n"
    "error_count\t0\n"
    "warning_count\t0\n"
    "failure_count\t1\n"
    "failure\tZ3JhcGhpY3MtYmFja2VuZA==\tYWN0dWFsIGJhY2tlbmQgdW5zdXBwb3J0ZWQ=\n"
    "end\n";

static int test_valid_nonpass_results(void) {
    UnityFiniteVisualGateSummary summary;
    CHECK(unity_finite_visual_gate_parse_result(
        (const uint8_t*)valid_mismatch_report,
        sizeof(valid_mismatch_report) - 1U, &summary) ==
        UNITY_FINITE_VISUAL_GATE_OK);
    CHECK(summary.reported_status ==
          UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH);
    CHECK(!summary.passed && summary.first_mismatch_offset == 7U);

    CHECK(unity_finite_visual_gate_parse_result(
        (const uint8_t*)valid_diagnostic_report,
        sizeof(valid_diagnostic_report) - 1U, &summary) ==
        UNITY_FINITE_VISUAL_GATE_OK);
    CHECK(summary.reported_status ==
          UNITY_FINITE_VISUAL_GATE_DIAGNOSTICS_FOUND);
    CHECK(summary.diagnostic_count == 1U && summary.warning_count == 1U);

    CHECK(unity_finite_visual_gate_parse_result(
        (const uint8_t*)valid_unsupported_report,
        sizeof(valid_unsupported_report) - 1U, &summary) ==
        UNITY_FINITE_VISUAL_GATE_OK);
    CHECK(summary.reported_status ==
          UNITY_FINITE_VISUAL_GATE_UNSUPPORTED_FIXTURE);
    CHECK(summary.backend == UNITY_FINITE_VISUAL_BACKEND_UNSUPPORTED);
    CHECK(summary.failure_count == 1U);
    return 0;
}

static int expect_invalid_unchanged(const char* text, size_t size) {
    UnityFiniteVisualGateSummary summary;
    memset(&summary, 0x5a, sizeof(summary));
    UnityFiniteVisualGateSummary before = summary;
    CHECK(unity_finite_visual_gate_parse_result(
        (const uint8_t*)text, size, &summary) ==
        UNITY_FINITE_VISUAL_GATE_RESULT_INVALID);
    CHECK(memcmp(&summary, &before, sizeof(summary)) == 0);
    return 0;
}

static int test_result_corruption_rejected(void) {
    char corrupted[sizeof(valid_pass_report) + 96U];
    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* count = strstr(corrupted, "pixel_byte_count\t64");
    CHECK(count != NULL);
    count[strlen("pixel_byte_count\t")] = '6';
    count[strlen("pixel_byte_count\t") + 1U] = '3';
    CHECK(expect_invalid_unchanged(corrupted, strlen(corrupted)) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* hash = strstr(corrupted, SHA_E);
    CHECK(hash != NULL);
    hash[0] = 'A';
    CHECK(expect_invalid_unchanged(corrupted, strlen(corrupted)) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* base64 = strstr(corrupted, "MjAyMS4zLjM1ZjE=");
    CHECK(base64 != NULL);
    base64[strlen("MjAyMS4zLjM1Zj")] = 'F';
    CHECK(expect_invalid_unchanged(corrupted, strlen(corrupted)) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* device = strstr(corrupted, "device_name\tR1BV");
    CHECK(device != NULL);
    memcpy(device + strlen("device_name\t"), "/w==", 4U);
    CHECK(expect_invalid_unchanged(corrupted, strlen(corrupted)) == 0);

    memcpy(corrupted, valid_pass_report, sizeof(valid_pass_report));
    char* end = strstr(corrupted, "end\n");
    CHECK(end != NULL);
    const char duplicate[] = "width\t2\nend\n";
    memcpy(end, duplicate, sizeof(duplicate));
    CHECK(expect_invalid_unchanged(corrupted, strlen(corrupted)) == 0);

    for (size_t cut = 0U; cut < sizeof(valid_pass_report) - 1U; ++cut) {
        CHECK(expect_invalid_unchanged(valid_pass_report, cut) == 0);
    }
    return 0;
}

static int test_existing_output_short_circuits_without_unity(void) {
    static const char output[] =
        "dxbc_finite_visual_existing_output.unit.tmp";
    (void)remove(output);
    FILE* file = fopen(output, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("owned", 1U, 5U, file) == 5U);
    CHECK(fclose(file) == 0);

    UnityFiniteVisualGateOptions options = valid_options();
    options.unity_executable = "definitely-not-a-unity-executable";
    options.report_path = output;
    UnityFiniteVisualGateRunResult result;
    CHECK(unity_finite_visual_gate_run(&options, &result) ==
          UNITY_FINITE_VISUAL_GATE_OUTPUT_EXISTS);
    CHECK(result.unity_exit_code == -1);
    CHECK(!result.report_published);
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

static int test_launcher_with_fake_editor(const char* fake_editor) {
    static const char report[] = "dxbc_finite_visual_fake.report.tsv";
    static const char log[] = "dxbc_finite_visual_fake.editor.log";
    static const char baseline_pixels[] =
        "dxbc_finite_visual_fake.baseline.rgba32f";
    static const char candidate_pixels[] =
        "dxbc_finite_visual_fake.candidate.rgba32f";
    (void)remove(report);
    (void)remove(log);
    (void)remove(baseline_pixels);
    (void)remove(candidate_pixels);

    UnityFiniteVisualGateOptions options = valid_options();
    options.unity_executable = fake_editor;
    options.bridge_path = DXBC_TEST_FINITE_VISUAL_BRIDGE;
    options.baseline_shader_path = DXBC_TEST_FINITE_VISUAL_SHADER;
    options.candidate_shader_path = DXBC_TEST_FINITE_VISUAL_SHADER;
    options.fixture_path = DXBC_TEST_FINITE_VISUAL_FIXTURE;
    options.report_path = report;
    options.log_path = log;
    options.baseline_pixels_path = baseline_pixels;
    options.candidate_pixels_path = candidate_pixels;
    options.keep_policy = UNITY_FINITE_VISUAL_KEEP_NEVER;
    UnityFiniteVisualGateRunResult result;
    CHECK(unity_finite_visual_gate_run(&options, &result) ==
          UNITY_FINITE_VISUAL_GATE_OK);
    CHECK(result.unity_exit_code == 0);
    CHECK(result.report_published);
    CHECK(result.baseline_pixels_published);
    CHECK(result.candidate_pixels_published);
    CHECK(!result.workspace_preserved);
    CHECK(result.summary.passed && result.summary.pixel_equal);
    CHECK(!bytes_are_zero(result.summary.render_case_identity,
                          sizeof(result.summary.render_case_identity)));
    CHECK(!bytes_are_zero(result.summary.authority_digest,
                          sizeof(result.summary.authority_digest)));
    CHECK(memcmp(result.summary.expected_runtime_inputs_digest,
                 result.summary.observed_runtime_inputs_digest,
                 sizeof(result.summary.expected_runtime_inputs_digest)) == 0);

    CHECK(remove(report) == 0);
    CHECK(remove(log) == 0);
    CHECK(remove(baseline_pixels) == 0);
    CHECK(remove(candidate_pixels) == 0);
    return 0;
}

int main(int argc, char** argv) {
    CHECK(argc == 2 && argv && argv[1] && argv[1][0]);
    if (test_argument_validation() != 0) return 1;
    if (test_valid_pass_result() != 0) return 1;
    if (test_valid_nonpass_results() != 0) return 1;
    if (test_result_corruption_rejected() != 0) return 1;
    if (test_existing_output_short_circuits_without_unity() != 0) return 1;
    if (test_launcher_with_fake_editor(argv[1]) != 0) return 1;
    puts("unity finite visual gate unit tests passed");
    return 0;
}
