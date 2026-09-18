#include "common/file_io.h"
#include "common/sha256.h"
#include "common/windows_utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char* find_argument(int argc, char** argv, const char* name) {
    const char* value = NULL;
    for (int index = 1; index + 1 < argc; ++index) {
        if (strcmp(argv[index], name) != 0) continue;
        if (value) return NULL;
        value = argv[++index];
    }
    return value;
}

static char hex_digit(unsigned value) {
    static const char digits[] = "0123456789abcdef";
    return digits[value & 15U];
}

static bool file_sha256(const char* path, char output[65]) {
    CommonFileBytes bytes = {0};
    if (common_file_read_regular(path, 512U * 1024U * 1024U, &bytes) !=
        COMMON_FILE_OK) {
        return false;
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(bytes.data, bytes.size, digest);
    common_file_bytes_dispose(&bytes);
    for (size_t index = 0U; index < sizeof(digest); ++index) {
        output[index * 2U] = hex_digit(digest[index] >> 4U);
        output[index * 2U + 1U] = hex_digit(digest[index]);
    }
    output[64] = '\0';
    return true;
}

static int fake_finite_visual_unity_main(int argc, char** argv) {
    const char* project = find_argument(argc, argv, "-projectPath");
    const char* log = find_argument(argc, argv, "-logFile");
    const char* result = find_argument(argc, argv, "-dxbc-finite-result");
    const char* baseline_pixels = find_argument(
        argc, argv, "-dxbc-finite-baseline-pixels");
    const char* candidate_pixels = find_argument(
        argc, argv, "-dxbc-finite-candidate-pixels");
    const char* fixture = find_argument(argc, argv, "-dxbc-finite-fixture");
    const char* unity_version = find_argument(
        argc, argv, "-dxbc-finite-expected-unity");
    const char* backend = find_argument(
        argc, argv, "-dxbc-finite-expected-backend");
    const char* bridge_sha = find_argument(
        argc, argv, "-dxbc-finite-bridge-sha256");
    if (!project || !log || !result || !baseline_pixels ||
        !candidate_pixels || !fixture || !unity_version || !backend ||
        !bridge_sha || strcmp(unity_version, "2021.3.35f1") != 0 ||
        strcmp(backend, "metal") != 0 || strlen(bridge_sha) != 64U) {
        return 41;
    }

    char baseline[4096];
    char candidate[4096];
    const int baseline_size = snprintf(
        baseline, sizeof(baseline),
        "%s/Assets/DXBCFiniteVisual/Baseline.shader", project);
    const int candidate_size = snprintf(
        candidate, sizeof(candidate),
        "%s/Assets/DXBCFiniteVisual/Candidate.shader", project);
    if (baseline_size <= 0 || (size_t)baseline_size >= sizeof(baseline) ||
        candidate_size <= 0 || (size_t)candidate_size >= sizeof(candidate)) {
        return 42;
    }
    char fixture_sha[65];
    char baseline_sha[65];
    char candidate_sha[65];
    if (!file_sha256(fixture, fixture_sha) ||
        !file_sha256(baseline, baseline_sha) ||
        !file_sha256(candidate, candidate_sha)) {
        return 43;
    }

    uint8_t pixels[64] = {0};
    uint8_t pixel_digest[COMMON_SHA256_DIGEST_SIZE];
    char pixel_sha[65];
    common_sha256(pixels, sizeof(pixels), pixel_digest);
    for (size_t index = 0U; index < sizeof(pixel_digest); ++index) {
        pixel_sha[index * 2U] = hex_digit(pixel_digest[index] >> 4U);
        pixel_sha[index * 2U + 1U] = hex_digit(pixel_digest[index]);
    }
    pixel_sha[64] = '\0';

    static const char log_text[] = "fake finite visual Unity process\n";
    if (common_file_write_new_atomic(log, log_text,
                                     sizeof(log_text) - 1U) != COMMON_FILE_OK ||
        common_file_write_new_atomic(baseline_pixels, pixels,
                                     sizeof(pixels)) != COMMON_FILE_OK ||
        common_file_write_new_atomic(candidate_pixels, pixels,
                                     sizeof(pixels)) != COMMON_FILE_OK) {
        return 44;
    }

    char report[4096];
    const int report_size = snprintf(
        report, sizeof(report),
        "dxbc-sandbox-unity-finite-visual/v1\n"
        "status\tok\n"
        "unity_version\tMjAyMS4zLjM1ZjE=\n"
        "backend\tmetal\n"
        "device_name\tRmFrZSBHUFU=\n"
        "device_vendor\tRmFrZSBWZW5kb3I=\n"
        "device_version\tRmFrZSBWZXJzaW9u\n"
        "requested_color_space\tbGluZWFy\n"
        "color_space\tbGluZWFy\n"
        "width\t2\n"
        "height\t2\n"
        "pass\t0\n"
        "fixture_sha256\t%s\n"
        "bridge_sha256\t%s\n"
        "baseline_source_sha256\t%s\n"
        "candidate_source_sha256\t%s\n"
        "baseline_imported\t1\n"
        "candidate_imported\t1\n"
        "baseline_stable\t1\n"
        "candidate_stable\t1\n"
        "pixel_equal\t1\n"
        "pixel_byte_count\t64\n"
        "first_mismatch_offset\t18446744073709551615\n"
        "baseline_pixels_sha256\t%s\n"
        "candidate_pixels_sha256\t%s\n"
        "diagnostic_count\t0\n"
        "error_count\t0\n"
        "warning_count\t0\n"
        "failure_count\t0\n"
        "end\n",
        fixture_sha, bridge_sha, baseline_sha, candidate_sha,
        pixel_sha, pixel_sha);
    if (report_size <= 0 || (size_t)report_size >= sizeof(report) ||
        common_file_write_new_atomic(result, report, (size_t)report_size) !=
            COMMON_FILE_OK) {
        return 45;
    }
    return 0;
}

COMMON_DEFINE_UTF8_MAIN(fake_finite_visual_unity_main)
