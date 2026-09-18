#include "compiler/unity_compiler_session_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static size_t count_substring(const char* text, const char* needle) {
    size_t count = 0U;
    const size_t needle_size = strlen(needle);
    while (text && needle_size != 0U &&
           (text = strstr(text, needle)) != NULL) {
        ++count;
        text += needle_size;
    }
    return count;
}

static UnityCompilerSessionReport make_fixture(void) {
    UnityCompilerSessionReport report = {
        .project_root = "/tmp/root\"\\\n",
        .includes_dir = "/tmp/includes\t",
        .toolchain = {
            .unity_contents_path = "/Unity/Contents",
            .compiler_path = "/Unity/Tools/UnityShaderCompiler",
            .builtin_includes_dir = "/Unity/CGIncludes",
            .playback_engines_dir = "/Unity/PlaybackEngines",
            .glslang_path = "/Unity/Tools/glslang.dylib",
            .dxcompiler_path = "/Unity/Tools/libdxcompiler.dylib",
        },
    };
    report.session.raw_available_platform_mask =
        ~UNITY_COMPILER_PLATFORM_MASK | UINT32_C(0x00048230);
    for (size_t index = 0U;
         index < UNITY_COMPILER_FINGERPRINT_SIZE; ++index) {
        report.toolchain.compiler_fingerprint[index] = (uint8_t)index;
        report.toolchain.environment_fingerprint[index] =
            (uint8_t)(UINT8_MAX - index);
    }
    for (size_t index = 0U;
         index < UNITY_COMPILER_PLATFORM_COUNT; ++index) {
        report.session.platforms[index].supported_features =
            UINT64_C(0x1000000000000000) + (uint64_t)index;
        report.session.platforms[index].version =
            index == 0U ? -1 : INT32_C(7000) + (int32_t)index;
    }
    return report;
}

int main(void) {
    UnityCompilerSessionReport report = make_fixture();
    CHECK(unity_compiler_session_report_validate(&report));

    char* json = unity_compiler_session_report_format_json(&report);
    char* json_again = unity_compiler_session_report_format_json(&report);
    CHECK(json != NULL);
    CHECK(json_again != NULL);
    CHECK(strcmp(json, json_again) == 0);
    CHECK(strncmp(json,
                  "{\"schema\":\"dxbc-unity-compiler-session-report-v1\"",
                  strlen("{\"schema\":\"dxbc-unity-compiler-session-report-v1\"")) == 0);
    CHECK(strstr(json,
                 "\"project_root\":\"/tmp/root\\\"\\\\\\u000a\"") != NULL);
    CHECK(strstr(json,
                 "\"includes_dir\":\"/tmp/includes\\u0009\"") != NULL);
    CHECK(strstr(json,
                 "\"compiler_fingerprint_sha256\":"
                 "\"000102030405060708090a0b0c0d0e0f"
                 "101112131415161718191a1b1c1d1e1f\"") != NULL);
    CHECK(strstr(json,
                 "\"environment_fingerprint_sha256\":"
                 "\"fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0"
                 "efeeedecebeae9e8e7e6e5e4e3e2e1e0\"") != NULL);
    CHECK(strstr(json,
                 "\"schema\":\"dxbc-unity-compiler-session-v1\"") != NULL);
    CHECK(strstr(json, "\"valid_apis\":295472") != NULL);
    CHECK(count_substring(json, "\"platform\":") ==
          UNITY_COMPILER_PLATFORM_COUNT);
    CHECK(strstr(json,
                 "{\"platform\":0,\"supported_features\":"
                 "1152921504606846976,\"version\":-1}") != NULL);
    CHECK(strstr(json,
                 "{\"platform\":24,\"supported_features\":"
                 "1152921504606847000,\"version\":7024}") != NULL);
    CHECK(strlen(json) > 1U && json[strlen(json) - 1U] == '\n');

    char* table = unity_compiler_session_report_format_table(&report);
    CHECK(table != NULL);
    CHECK(strstr(table, "raw_available_platform_mask:") != NULL);
    CHECK(strstr(table, "valid_apis: 295472 (0x00048230)") != NULL);
    CHECK(count_substring(table, "  0x") ==
          UNITY_COMPILER_PLATFORM_COUNT);
    CHECK(strstr(table, "        24  0x1000000000000018  7024\n") != NULL);
    free(table);
    free(json_again);
    free(json);

    report.session.raw_available_platform_mask = UINT32_C(0x00048230);
    CHECK(!unity_compiler_session_report_validate(&report));
    CHECK(unity_compiler_session_report_format_json(&report) == NULL);
    CHECK(unity_compiler_session_report_format_table(&report) == NULL);

    report = make_fixture();
    memset(report.toolchain.environment_fingerprint, 0,
           sizeof(report.toolchain.environment_fingerprint));
    CHECK(!unity_compiler_session_report_validate(&report));
    return 0;
}
