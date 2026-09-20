// SPDX-License-Identifier: GPL-3.0-only
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "app/shader_runtime_capture.h"
#include "test_support/file_mutation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define TEST_PID() ((unsigned long)_getpid())
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_PID() ((unsigned long)getpid())
#define TEST_MKDIR(path) mkdir(path, 0700)
#define TEST_RMDIR(path) rmdir(path)
#endif

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x);                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static const ShaderRuntimeCaptureLimits limits = {16U, 16U, 8U, 4096U, 64U, 128U};

static int capture_image(const char *root, ShaderRuntimeImageSummary *summary) {
    ShaderRuntimeCapture *capture = NULL;
    ShaderRuntimeCaptureDiagnostic diagnostic;
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(shader_runtime_capture_file_count(capture) == 2U);
    ShaderRuntimeImageSummary untouched;
    memset(&untouched, 0xa5, sizeof(untouched));
    const ShaderRuntimeImageSummary before = untouched;
    CHECK(!shader_runtime_capture_describe(capture, &untouched));
    CHECK(memcmp(&untouched, &before, sizeof(before)) == 0);
    ShaderRuntimeFileIdentity identity;
    memset(&identity, 0xa5, sizeof(identity));
    const ShaderRuntimeFileIdentity unopened = identity;
    CHECK(!shader_runtime_capture_find_file(capture, "a.bin", &identity));
    CHECK(memcmp(&identity, &unopened, sizeof(identity)) == 0);
    const char *path = NULL;
    const uint8_t *data = NULL;
    size_t size = SIZE_MAX;
    CHECK(shader_runtime_capture_file(capture, 0U, &path, &data, &size));
    CHECK(strcmp(path, "a.bin") == 0 && size == 3U && memcmp(data, "abc", 3U) == 0);
    CHECK(shader_runtime_capture_file(capture, 1U, &path, &data, &size));
    CHECK(strcmp(path, "nested/z.bin") == 0 && size == 0U && data == NULL);
    CHECK(!shader_runtime_capture_file(capture, 2U, &path, &data, &size));
    CHECK(strcmp(path, "nested/z.bin") == 0 && size == 0U && data == NULL);
    CHECK(shader_runtime_capture_finish(capture, &diagnostic) == SHADER_RUNTIME_CAPTURE_OK);
    CHECK(!shader_runtime_capture_file(capture, 0U, &path, &data, &size));
    CHECK(shader_runtime_capture_describe(capture, summary));
    CHECK(summary->file_count == 2U && summary->total_bytes == 3U);
    CHECK(shader_runtime_capture_find_file(capture, "a.bin", &identity));
    uint8_t expected[32];
    common_sha256("abc", 3U, expected);
    CHECK(strcmp(identity.relative_path, "a.bin") == 0 && identity.size == 3U);
    CHECK(memcmp(identity.content_digest, expected, sizeof(expected)) == 0);
    const char *borrowed = identity.relative_path;
    CHECK(shader_runtime_capture_find_file(capture, "nested/z.bin", &identity));
    common_sha256(NULL, 0U, expected);
    CHECK(strcmp(identity.relative_path, "nested/z.bin") == 0 && identity.size == 0U);
    CHECK(memcmp(identity.content_digest, expected, sizeof(expected)) == 0);
    const ShaderRuntimeFileIdentity unchanged = identity;
    const char *invalid[] = {NULL, "", "A.bin", "absent", "0.bin", "z.bin", "nested",
                            "../a.bin", "./a.bin", "/a.bin", "nested/../a.bin",
                            "nested\\z.bin", "a.bin/", "a.bin//", "\xff"};
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(!shader_runtime_capture_find_file(capture, invalid[i], &identity));
        CHECK(memcmp(&identity, &unchanged, sizeof(identity)) == 0);
    }
    char oversized[4098];
    memset(oversized, 'a', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    CHECK(!shader_runtime_capture_find_file(capture, oversized, &identity));
    CHECK(!shader_runtime_capture_find_file(NULL, "a.bin", &identity));
    CHECK(!shader_runtime_capture_find_file(capture, "a.bin", NULL));
    CHECK(memcmp(&identity, &unchanged, sizeof(identity)) == 0);
    CHECK(strcmp(borrowed, "a.bin") == 0);
    CHECK(shader_runtime_capture_finish(capture, &diagnostic) == SHADER_RUNTIME_CAPTURE_CLOSED);
    CHECK(shader_runtime_capture_describe(capture, &untouched));
    CHECK(memcmp(summary->image_digest, untouched.image_digest, 32U) == 0);
    shader_runtime_capture_free(capture);
    return 0;
}

static int reject_begin(const char *root, const ShaderRuntimeCaptureLimits *bounds,
                        ShaderRuntimeCaptureStatus expected) {
    ShaderRuntimeCapture *capture = NULL;
    ShaderRuntimeCaptureDiagnostic diagnostic;
    CHECK(shader_runtime_capture_begin(root, bounds, &capture, &diagnostic) == expected);
    CHECK(!capture);
    return 0;
}

static int reject_finish(ShaderRuntimeCapture *capture) {
    ShaderRuntimeCaptureDiagnostic diagnostic;
    ShaderRuntimeImageSummary summary;
    memset(&summary, 0xa5, sizeof(summary));
    const ShaderRuntimeImageSummary before = summary;
    CHECK(shader_runtime_capture_finish(capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_INPUT_CHANGED);
    CHECK(!shader_runtime_capture_describe(capture, &summary));
    CHECK(memcmp(&summary, &before, sizeof(summary)) == 0);
    ShaderRuntimeFileIdentity identity;
    memset(&identity, 0xa5, sizeof(identity));
    const ShaderRuntimeFileIdentity untouched = identity;
    CHECK(!shader_runtime_capture_find_file(capture, "a.bin", &identity));
    CHECK(memcmp(&identity, &untouched, sizeof(identity)) == 0);
    CHECK(shader_runtime_capture_finish(capture, &diagnostic) == SHADER_RUNTIME_CAPTURE_CLOSED);
    shader_runtime_capture_free(capture);
    return 0;
}

static int fixtures(void) {
    char root[128], nested[160], file[192], empty[192], extra[192], moved[160];
    CHECK(snprintf(root, sizeof(root), "shader-runtime-test-%lu", TEST_PID()) > 0);
    CHECK(snprintf(nested, sizeof(nested), "%s/nested", root) > 0);
    CHECK(snprintf(file, sizeof(file), "%s/a.bin", root) > 0);
    CHECK(snprintf(empty, sizeof(empty), "%s/z.bin", nested) > 0);
    CHECK(snprintf(extra, sizeof(extra), "%s/extra.bin", root) > 0);
    CHECK(snprintf(moved, sizeof(moved), "%s-relocated", root) > 0);
    CHECK(TEST_MKDIR(root) == 0);
    CHECK(reject_begin(root, &limits, SHADER_RUNTIME_CAPTURE_EMPTY) == 0);
    CHECK(TEST_MKDIR(nested) == 0);
    CHECK(common_file_write_new_atomic(empty, NULL, 0U) == COMMON_FILE_OK);
    CHECK(common_file_write_new_atomic(file, "abc", 3U) == COMMON_FILE_OK);
    ShaderRuntimeImageSummary original, repeated, relocated;
    CHECK(capture_image(root, &original) == 0);
    CHECK(capture_image(root, &repeated) == 0);
    CHECK(memcmp(original.image_digest, repeated.image_digest, 32U) == 0);
    CHECK(rename(root, moved) == 0);
    CHECK(capture_image(moved, &relocated) == 0);
    CHECK(memcmp(original.image_digest, relocated.image_digest, 32U) == 0);
    CHECK(rename(moved, root) == 0);
    char trailing[160];
    CHECK(snprintf(trailing, sizeof(trailing), "%s/", root) > 0);
    CHECK(capture_image(trailing, &repeated) == 0);
    CHECK(memcmp(original.image_digest, repeated.image_digest, 32U) == 0);
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(original.image_digest, hex);
    /* Independently encoded with Python hashlib and struct.pack('<Q', ...). */
    CHECK(strcmp(hex, "4c535416c001b9dbf59e19d1f5e61ff9acb826c3c272915f6b3308603830aea6") == 0);

    CHECK(reject_begin(file, &limits, SHADER_RUNTIME_CAPTURE_INVALID_PATH) == 0);
    ShaderRuntimeCaptureLimits bounded = limits;
    bounded.max_files = 1U;
    CHECK(reject_begin(root, &bounded, SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED) == 0);
    bounded = limits;
    bounded.max_directories = 1U;
    CHECK(reject_begin(root, &bounded, SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED) == 0);
    char deep[192];
    CHECK(snprintf(deep, sizeof(deep), "%s/deep", nested) > 0);
    CHECK(TEST_MKDIR(deep) == 0);
    bounded = limits;
    bounded.max_depth = 1U;
    CHECK(reject_begin(root, &bounded, SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED) == 0);
    CHECK(TEST_RMDIR(deep) == 0);
    bounded = limits;
    bounded.max_path_bytes = strlen(root) - 1U;
    CHECK(reject_begin(root, &bounded, SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED) == 0);
    bounded = limits;
    bounded.max_file_bytes = 2U;
    CHECK(reject_begin(root, &bounded, SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED) == 0);
    bounded = limits;
    bounded.max_total_bytes = 2U;
    CHECK(reject_begin(root, &bounded, SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED) == 0);
    bounded = limits;
    bounded.max_total_bytes = 3U;
    ShaderRuntimeCapture *at_limit = NULL;
    ShaderRuntimeCaptureDiagnostic at_limit_diagnostic;
    CHECK(shader_runtime_capture_begin(root, &bounded, &at_limit, &at_limit_diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK); /* Empty last file fits an exhausted byte budget. */
    CHECK(shader_runtime_capture_finish(at_limit, &at_limit_diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    shader_runtime_capture_free(at_limit);
    bounded = limits;
    bounded.max_total_bytes = 0U;
    CHECK(reject_begin(root, &bounded, SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT) == 0);
    CHECK(reject_begin(NULL, &limits, SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT) == 0);

    ShaderRuntimeCapture *capture = NULL;
    ShaderRuntimeCaptureDiagnostic diagnostic;
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(common_file_write_new_atomic(extra, "addition", 8U) == COMMON_FILE_OK);
    CHECK(reject_finish(capture) == 0);
    CHECK(remove(extra) == 0);

    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(test_replace_regular_file(file, "changed", 7U));
    const char *name = NULL;
    const uint8_t *data = NULL;
    size_t size = 0U;
    CHECK(shader_runtime_capture_file(capture, 0U, &name, &data, &size));
    CHECK(size == 3U && memcmp(data, "abc", 3U) == 0);
    CHECK(reject_finish(capture) == 0);
    CHECK(test_replace_regular_file(file, "abc", 3U));
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(test_replace_regular_file(file, "abc", 3U));
    CHECK(reject_finish(capture) == 0); /* Same bytes, different opening identity. */

    CHECK(test_replace_regular_file(file, "abd", 3U));
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(shader_runtime_capture_finish(capture, &diagnostic) == SHADER_RUNTIME_CAPTURE_OK);
    CHECK(shader_runtime_capture_describe(capture, &repeated));
    CHECK(memcmp(original.image_digest, repeated.image_digest, 32U) != 0);
    shader_runtime_capture_free(capture);
    CHECK(test_replace_regular_file(file, "abc", 3U));

    CHECK(rename(empty, extra) == 0);
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(shader_runtime_capture_finish(capture, &diagnostic) == SHADER_RUNTIME_CAPTURE_OK);
    CHECK(shader_runtime_capture_describe(capture, &repeated));
    CHECK(memcmp(original.image_digest, repeated.image_digest, 32U) != 0);
    shader_runtime_capture_free(capture);
    CHECK(rename(extra, empty) == 0);

#ifndef _WIN32
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    FILE *mutated = fopen(file, "wb");
    CHECK(mutated && fwrite("abd", 1U, 3U, mutated) == 3U);
    CHECK(fclose(mutated) == 0);
    CHECK(shader_runtime_capture_file(capture, 0U, &name, &data, &size));
    CHECK(size == 3U && memcmp(data, "abc", 3U) == 0);
    CHECK(reject_finish(capture) == 0);
    CHECK(test_replace_regular_file(file, "abc", 3U));
    /* Root replacement preserves every byte but invalidates held namespaces. */
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(rename(root, moved) == 0);
    CHECK(TEST_MKDIR(root) == 0 && TEST_MKDIR(nested) == 0);
    CHECK(common_file_write_new_atomic(file, "abc", 3U) == COMMON_FILE_OK);
    CHECK(common_file_write_new_atomic(empty, NULL, 0U) == COMMON_FILE_OK);
    CHECK(reject_finish(capture) == 0);
    CHECK(remove(file) == 0 && remove(empty) == 0);
    CHECK(TEST_RMDIR(nested) == 0 && TEST_RMDIR(root) == 0);
    CHECK(rename(moved, root) == 0);
    CHECK(symlink("a.bin", extra) == 0);
    CHECK(reject_begin(root, &limits, SHADER_RUNTIME_CAPTURE_DISCOVERY_FAILED) == 0);
    CHECK(unlink(extra) == 0);
    CHECK(mkfifo(extra, 0600) == 0);
    CHECK(reject_begin(root, &limits, SHADER_RUNTIME_CAPTURE_DISCOVERY_FAILED) == 0);
    CHECK(unlink(extra) == 0);
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(symlink("a.bin", extra) == 0);
    CHECK(reject_finish(capture) == 0);
    CHECK(unlink(extra) == 0);
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(remove(empty) == 0);
    CHECK(reject_finish(capture) == 0);
    CHECK(common_file_write_new_atomic(empty, NULL, 0U) == COMMON_FILE_OK);
    char invalid[192];
    CHECK(snprintf(invalid, sizeof(invalid), "%s/invalid\\\\name", root) > 0);
    CHECK(common_file_write_new_atomic(invalid, NULL, 0U) == COMMON_FILE_OK);
    CHECK(reject_begin(root, &limits, SHADER_RUNTIME_CAPTURE_INVALID_PATH) == 0);
    CHECK(remove(invalid) == 0);
#endif
    /* Abort releases every lease without publishing a summary. */
    CHECK(shader_runtime_capture_begin(root, &limits, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    shader_runtime_capture_free(capture);
    CHECK(capture_image(root, &repeated) == 0);
    CHECK(memcmp(original.image_digest, repeated.image_digest, 32U) == 0);
    CHECK(remove(file) == 0 && remove(empty) == 0);
    CHECK(TEST_RMDIR(nested) == 0 && TEST_RMDIR(root) == 0);
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    if (argc == 1)
        return fixtures();
    /* Optional private shader-validation package; paths never enter output. */
    const ShaderRuntimeCaptureLimits live = {
        4096U, 512U, 32U, 4096U, 512U * 1024U * 1024U, 2U * 1024U * 1024U * 1024U};
    ShaderRuntimeCapture *capture = NULL;
    ShaderRuntimeCaptureDiagnostic diagnostic;
    CHECK(shader_runtime_capture_begin(argv[1], &live, &capture, &diagnostic) ==
          SHADER_RUNTIME_CAPTURE_OK);
    CHECK(shader_runtime_capture_finish(capture, &diagnostic) == SHADER_RUNTIME_CAPTURE_OK);
    ShaderRuntimeImageSummary summary;
    CHECK(shader_runtime_capture_describe(capture, &summary));
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(summary.image_digest, hex);
    printf("files=%zu bytes=%zu image=%s\n", summary.file_count, summary.total_bytes, hex);
    shader_runtime_capture_free(capture);
    return 0;
}
