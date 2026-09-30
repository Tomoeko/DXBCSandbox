// SPDX-License-Identifier: GPL-3.0-only
#include "common/sha256.h"
#include "compiler/unity_compiler_cache.h"
#include "compiler/unity_compiler_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #x);                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static bool digest(const UnityCompilerComputePreprocessRequest* request, const char* config,
                   const uint8_t compiler[32], const uint8_t environment[32], uint8_t out[32]) {
    uint8_t* bytes = NULL;
    size_t size = 0U;
    bool ok = usc_cache_serialize_compute_preprocess_request(request, config, compiler, environment,
                                                             &bytes, &size);
    if (ok)
        common_sha256(bytes, size, out);
    free(bytes);
    return ok;
}
/* Independent inspection of the canonical length-prefixed little-endian transcript. */
static bool u32(const uint8_t* data, size_t size, size_t* cursor, uint32_t wanted) {
    if (*cursor > size || size - *cursor < 4U)
        return false;
    uint32_t actual = 0U;
    for (unsigned i = 0U; i < 4U; ++i)
        actual |= (uint32_t)data[*cursor + i] << (8U * i);
    *cursor += 4U;
    return actual == wanted;
}
static bool buffer(const uint8_t* data, size_t size, size_t* cursor, const void* wanted,
                   size_t length) {
    if (*cursor > size || size - *cursor < 8U)
        return false;
    uint64_t actual = 0U;
    for (unsigned i = 0U; i < 8U; ++i)
        actual |= (uint64_t)data[*cursor + i] << (8U * i);
    *cursor += 8U;
    if (actual != length || size - *cursor < length || memcmp(data + *cursor, wanted, length))
        return false;
    *cursor += length;
    return true;
}
#define TEXT(text) CHECK(buffer(bytes, size, &cursor, text, strlen(text)))
int main(void) {
    char* platform[] = {"PLATFORM_A", "PLATFORM_B"};
    char* disabled[] = {"DISABLED_A", "DISABLED_B"};
    UnityCompilerComputePreprocessRequest request = {.source = "source",
                                                     .source_filename = "Assets/Compute.compute",
                                                     .caching_preprocessor = true,
                                                     .build_platform = 19U,
                                                     .valid_apis = 311856U,
                                                     .platform_keywords = platform,
                                                     .platform_keyword_count = 2,
                                                     .disabled_keywords = disabled,
                                                     .disabled_keyword_count = 2};
    uint8_t compiler[32] = {1U}, environment[32] = {2U}, base[32], other[32];
    uint8_t* bytes = NULL;
    size_t size = 0U, cursor = 0U;
    CHECK(usc_cache_serialize_compute_preprocess_request(&request, "config", compiler, environment,
                                                         &bytes, &size));
    TEXT("DXBCSandbox.UnityCompiler.preprocessCompute.request.v1");
    CHECK(buffer(bytes, size, &cursor, compiler, 32U));
    CHECK(buffer(bytes, size, &cursor, environment, 32U));
    CHECK(u32(bytes, size, &cursor, UINT32_C(0x0C0BD1E4)));
    TEXT("preprocessCompute");
    TEXT("config");
    TEXT("source");
    TEXT("Assets/Compute.compute");
    CHECK(u32(bytes, size, &cursor, 1U));
    CHECK(u32(bytes, size, &cursor, 19U));
    CHECK(u32(bytes, size, &cursor, 311856U));
    CHECK(u32(bytes, size, &cursor, 2U));
    TEXT("PLATFORM_A");
    TEXT("PLATFORM_B");
    CHECK(u32(bytes, size, &cursor, 2U));
    TEXT("DISABLED_A");
    TEXT("DISABLED_B");
    CHECK(cursor == size);
    free(bytes);
    CHECK(digest(&request, "config", compiler, environment, base));
#define DIFFERENT(change)                                                                          \
    do {                                                                                           \
        UnityCompilerComputePreprocessRequest changed = request;                                   \
        change;                                                                                    \
        CHECK(digest(&changed, "config", compiler, environment, other));                           \
        CHECK(memcmp(base, other, 32U) != 0);                                                      \
    } while (0)
    DIFFERENT(changed.source = "changed");
    DIFFERENT(changed.source_filename = "Assets/Other.compute");
    DIFFERENT(changed.caching_preprocessor = false);
    DIFFERENT(changed.build_platform = 20U);
    DIFFERENT(changed.valid_apis = 311840U);
    DIFFERENT(changed.platform_keyword_count = 1);
    char* reversed_platform[] = {"PLATFORM_B", "PLATFORM_A"};
    DIFFERENT(changed.platform_keywords = reversed_platform);
    DIFFERENT(changed.disabled_keyword_count = 1);
    char* reversed_disabled[] = {"DISABLED_B", "DISABLED_A"};
    DIFFERENT(changed.disabled_keywords = reversed_disabled);
    char* changed_disabled[] = {"DISABLED_A", "OTHER"};
    DIFFERENT(changed.disabled_keywords = changed_disabled);
    CHECK(digest(&request, "other-config", compiler, environment, other) &&
          memcmp(base, other, 32U));
    compiler[0] = 3U;
    CHECK(digest(&request, "config", compiler, environment, other) && memcmp(base, other, 32U));
    compiler[0] = 1U;
    environment[0] = 4U;
    CHECK(digest(&request, "config", compiler, environment, other) && memcmp(base, other, 32U));
#define REJECT(change)                                                                             \
    do {                                                                                           \
        UnityCompilerComputePreprocessRequest changed = request;                                   \
        change;                                                                                    \
        bytes = (void*)1;                                                                          \
        size = 1U;                                                                                 \
        CHECK(!usc_cache_serialize_compute_preprocess_request(&changed, "config", compiler,        \
                                                              environment, &bytes, &size) &&       \
              !bytes && !size);                                                                    \
    } while (0)
    REJECT(changed.source = NULL);
    REJECT(changed.source_filename = NULL);
    REJECT(changed.source_filename = "");
    REJECT(changed.platform_keyword_count = -1);
    REJECT(changed.platform_keyword_count = 1025);
    REJECT(changed.disabled_keyword_count = -1);
    REJECT(changed.disabled_keyword_count = 1025);
    REJECT(changed.platform_keywords = NULL);
    REJECT(changed.disabled_keywords = NULL);
    char* missing[] = {"A", NULL};
    REJECT(changed.disabled_keywords = missing);
    puts("compute preprocessing canonical request tests passed");
    return 0;
}
