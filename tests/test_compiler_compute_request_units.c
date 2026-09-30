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
static bool digest(const UnityCompilerComputeKernelRequest* r, const char* config, const uint8_t* c,
                   const uint8_t* e, uint8_t out[32]) {
    uint8_t* data = NULL;
    size_t size = 0U;
    bool ok = usc_cache_serialize_compute_request(r, config, c, e, &data, &size);
    if (ok)
        common_sha256(data, size, out);
    free(data);
    return ok;
}
int main(void) {
    UnityCompilerComputeMacro macros[] = {{"A", "1"}, {"B", "two"}};
    char* pkw[] = {"P1", "P2"};
    char* ukw[] = {"U1", "U2"};
    UnityCompilerComputeKernelRequest r = {.source = "source",
                                           .source_filename = "Assets/File.compute",
                                           .kernel_name = "Kernel",
                                           .caching_preprocessor = true,
                                           .strip_line_directives = true,
                                           .build_platform = 19U,
                                           .kernel_macros = macros,
                                           .kernel_macro_count = 2,
                                           .platform_keywords = pkw,
                                           .platform_keyword_count = 2,
                                           .user_keywords = ukw,
                                           .user_keyword_count = 2,
                                           .compiler_platform = 4,
                                           .compilation_flags = 7U,
                                           .requirements = 16385U,
                                           .force_dxc = 3U,
                                           .force_fxc = 4U};
    uint8_t c[32] = {1U}, e[32] = {2U}, base[32], other[32];
    CHECK(digest(&r, "config", c, e, base));
#define DIFFERENT(change)                                                                          \
    do {                                                                                           \
        UnityCompilerComputeKernelRequest changed = r;                                             \
        change;                                                                                    \
        CHECK(digest(&changed, "config", c, e, other));                                            \
        CHECK(memcmp(base, other, 32U) != 0);                                                      \
    } while (0)
    DIFFERENT(changed.source = "changed");
    DIFFERENT(changed.source_filename = "Assets/Other.compute");
    DIFFERENT(changed.kernel_name = "Other");
    DIFFERENT(changed.caching_preprocessor = false);
    DIFFERENT(changed.preprocess_only = true);
    DIFFERENT(changed.strip_line_directives = false);
    DIFFERENT(changed.build_platform = 20U);
    DIFFERENT(changed.kernel_macro_count = 1);
    UnityCompilerComputeMacro reordered[] = {{"B", "two"}, {"A", "1"}};
    DIFFERENT(changed.kernel_macros = reordered);
    UnityCompilerComputeMacro value[] = {{"A", "2"}, {"B", "two"}};
    DIFFERENT(changed.kernel_macros = value);
    DIFFERENT(changed.platform_keyword_count = 1);
    char* rp[] = {"P2", "P1"};
    DIFFERENT(changed.platform_keywords = rp);
    DIFFERENT(changed.user_keyword_count = 1);
    char* ru[] = {"U2", "U1"};
    DIFFERENT(changed.user_keywords = ru);
    DIFFERENT(changed.compiler_platform = 5);
    DIFFERENT(changed.compilation_flags = 8U);
    DIFFERENT(changed.requirements = UINT64_C(0x100004001));
    DIFFERENT(changed.force_dxc = 5U);
    DIFFERENT(changed.force_fxc = 6U);
    CHECK(digest(&r, "other-config", c, e, other) && memcmp(base, other, 32U) != 0);
    c[0] = 3U;
    CHECK(digest(&r, "config", c, e, other) && memcmp(base, other, 32U) != 0);
    c[0] = 1U;
    e[0] = 4U;
    CHECK(digest(&r, "config", c, e, other) && memcmp(base, other, 32U) != 0);
    uint8_t* data = (void*)1;
    size_t size = 1U;
    CHECK(!usc_cache_serialize_compute_request(NULL, "config", c, e, &data, &size) && !data &&
          !size);
    puts("compute request canonical encoding tests passed");
    return 0;
}
