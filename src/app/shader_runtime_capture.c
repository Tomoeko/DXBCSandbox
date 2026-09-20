// SPDX-License-Identifier: GPL-3.0-only
#include "app/shader_runtime_capture.h"
#include "common/relative_path.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *relative_path;
    size_t discovered_index;
    CommonFileView view;
    bool opened;
} RuntimeFile;

struct ShaderRuntimeCapture {
    char *root;
    CommonPathDiscoveryOptions discovery_options;
    CommonPathDiscoveryResult discovered;
    RuntimeFile *files;
    size_t file_count;
    bool closed;
    bool sealed;
    ShaderRuntimeImageSummary summary;
};

static void diagnostic_init(ShaderRuntimeCaptureDiagnostic *diagnostic) {
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->file_index = SIZE_MAX;
}

static bool separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static char *copy_string(const char *value) {
    size_t size = strlen(value) + 1U;
    char *copy = malloc(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

static ShaderRuntimeCaptureStatus discovery_status(CommonPathDiscoveryStatus status) {
    switch (status) {
    case COMMON_PATH_DISCOVERY_OK:
        return SHADER_RUNTIME_CAPTURE_OK;
    case COMMON_PATH_DISCOVERY_ALLOCATION_FAILED:
        return SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED;
    case COMMON_PATH_DISCOVERY_FILE_LIMIT_EXCEEDED:
    case COMMON_PATH_DISCOVERY_DIRECTORY_LIMIT_EXCEEDED:
    case COMMON_PATH_DISCOVERY_DEPTH_LIMIT_EXCEEDED:
    case COMMON_PATH_DISCOVERY_PATH_LIMIT_EXCEEDED:
        return SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED;
    default:
        return SHADER_RUNTIME_CAPTURE_DISCOVERY_FAILED;
    }
}

static int compare_files(const void *left, const void *right) {
    const RuntimeFile *a = left, *b = right;
    return strcmp(a->relative_path, b->relative_path);
}

static void hash_u64(CommonSha256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0U; i < 8U; ++i)
        bytes[i] = (uint8_t)(value >> (8U * i));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static bool close_files(ShaderRuntimeCapture *capture, ShaderRuntimeCaptureDiagnostic *diagnostic) {
    bool valid = true;
    for (size_t i = 0U; i < capture->file_count; ++i) {
        if (!capture->files[i].opened)
            continue;
        const CommonFileStatus status = common_file_view_close(&capture->files[i].view);
        capture->files[i].opened = false;
        if (status != COMMON_FILE_OK && valid) {
            diagnostic->file_status = status;
            diagnostic->file_index = i;
            valid = false;
        }
    }
    capture->closed = true;
    return valid;
}

void shader_runtime_capture_free(ShaderRuntimeCapture *capture) {
    if (!capture)
        return;
    ShaderRuntimeCaptureDiagnostic ignored;
    diagnostic_init(&ignored);
    (void)close_files(capture, &ignored);
    for (size_t i = 0U; i < capture->file_count; ++i)
        free(capture->files[i].relative_path);
    free(capture->files);
    free(capture->root);
    common_path_discovery_result_dispose(&capture->discovered);
    free(capture);
}

ShaderRuntimeCaptureStatus
shader_runtime_capture_begin(const char *root, const ShaderRuntimeCaptureLimits *limits,
                             ShaderRuntimeCapture **output,
                             ShaderRuntimeCaptureDiagnostic *diagnostic) {
    if (output)
        *output = NULL;
    if (!diagnostic)
        return SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT;
    diagnostic_init(diagnostic);
    if (!root || !root[0] || !limits || !output || !limits->max_files || !limits->max_directories ||
        !limits->max_depth || !limits->max_path_bytes || !limits->max_file_bytes ||
        !limits->max_total_bytes)
        return SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT;
    /* Bound the caller's path before duplicating it. */
    size_t root_size = 0U;
    while (root[root_size]) {
        if (root_size == limits->max_path_bytes)
            return SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED;
        ++root_size;
    }
    ShaderRuntimeCapture *capture = calloc(1U, sizeof(*capture));
    if (!capture)
        return SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED;
    ShaderRuntimeCaptureStatus status = SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED;
    capture->root = copy_string(root);
    if (!capture->root)
        goto failure;
    common_path_discovery_options_default(&capture->discovery_options);
    capture->discovery_options.reject_descendant_links = true;
    capture->discovery_options.reject_descendant_unsupported_nodes = true;
    capture->discovery_options.max_files = limits->max_files;
    capture->discovery_options.max_directories = limits->max_directories;
    capture->discovery_options.max_depth = limits->max_depth;
    capture->discovery_options.max_path_bytes = limits->max_path_bytes;
    diagnostic->discovery_status =
        common_path_discover(&root, 1U, &capture->discovery_options, &capture->discovered);
    status = discovery_status(diagnostic->discovery_status);
    if (status != SHADER_RUNTIME_CAPTURE_OK)
        goto failure;
    if (!capture->discovered.count) {
        status = SHADER_RUNTIME_CAPTURE_EMPTY;
        goto failure;
    }
    if (capture->discovered.count > SIZE_MAX / sizeof(*capture->files)) {
        status = SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED;
        goto failure;
    }
    capture->files = calloc(capture->discovered.count, sizeof(*capture->files));
    if (!capture->files) {
        status = SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED;
        goto failure;
    }
    capture->file_count = capture->discovered.count;
    for (size_t i = 0U; i < capture->file_count; ++i) {
        const CommonDiscoveredPath *path = &capture->discovered.paths[i];
        size_t offset = root_size;
        if (path->explicit_file || strncmp(path->path, root, root_size) != 0 ||
            (!separator(root[root_size - 1U]) && !separator(path->path[offset]))) {
            status = SHADER_RUNTIME_CAPTURE_INVALID_PATH;
            goto failure;
        }
        if (!separator(root[root_size - 1U]))
            ++offset;
        char *relative = copy_string(path->path + offset);
        if (!relative) {
            status = SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED;
            goto failure;
        }
        capture->files[i].relative_path = relative;
        capture->files[i].discovered_index = i;
#ifdef _WIN32
        for (char *p = relative; *p; ++p)
            if (*p == '\\')
                *p = '/';
#endif
        if (common_relative_path_validate_utf8(relative, strlen(relative)).status !=
            COMMON_RELATIVE_PATH_OK) {
            status = SHADER_RUNTIME_CAPTURE_INVALID_PATH;
            goto failure;
        }
    }
    /* Windows discovery uses backslashes. Sort after encoding portable names. */
    qsort(capture->files, capture->file_count, sizeof(*capture->files), compare_files);
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char domain[] = "DXBCSandbox.ShaderRuntimeImage.v1";
    common_sha256_update(&hash, domain, sizeof(domain));
    hash_u64(&hash, capture->file_count);
    for (size_t i = 0U; i < capture->file_count; ++i) {
        RuntimeFile *file = &capture->files[i];
        const size_t remaining = limits->max_total_bytes - capture->summary.total_bytes;
        const size_t bound =
            remaining < limits->max_file_bytes ? remaining : limits->max_file_bytes;
        diagnostic->file_status = common_file_view_open_regular(
            capture->discovered.paths[file->discovered_index].path, bound, &file->view);
        if (diagnostic->file_status != COMMON_FILE_OK) {
            diagnostic->file_index = i;
            status = diagnostic->file_status == COMMON_FILE_TOO_LARGE
                         ? SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED
                     : diagnostic->file_status == COMMON_FILE_ALLOCATION_FAILED
                         ? SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED
                         : SHADER_RUNTIME_CAPTURE_FILE_UNAVAILABLE;
            goto failure;
        }
        file->opened = true;
        uint8_t digest[32];
        if (!common_file_view_sha256(&file->view, digest)) {
            diagnostic->file_index = i;
            status = SHADER_RUNTIME_CAPTURE_FILE_UNAVAILABLE;
            goto failure;
        }
        capture->summary.total_bytes += file->view.size;
        hash_u64(&hash, strlen(file->relative_path));
        common_sha256_update(&hash, file->relative_path, strlen(file->relative_path));
        hash_u64(&hash, file->view.size);
        common_sha256_update(&hash, digest, sizeof(digest));
    }
    capture->summary.file_count = capture->file_count;
    common_sha256_final(&hash, capture->summary.image_digest);
    *output = capture;
    return SHADER_RUNTIME_CAPTURE_OK;
failure:
    shader_runtime_capture_free(capture);
    return status;
}

size_t shader_runtime_capture_file_count(const ShaderRuntimeCapture *capture) {
    return capture ? capture->file_count : 0U;
}

bool shader_runtime_capture_file(const ShaderRuntimeCapture *capture, size_t index,
                                 const char **relative_path, const uint8_t **data, size_t *size) {
    if (!capture || capture->closed || index >= capture->file_count || !relative_path || !data ||
        !size)
        return false;
    const RuntimeFile *file = &capture->files[index];
    *relative_path = file->relative_path;
    *data = file->view.data;
    *size = file->view.size;
    return true;
}

ShaderRuntimeCaptureStatus
shader_runtime_capture_finish(ShaderRuntimeCapture *capture,
                              ShaderRuntimeCaptureDiagnostic *diagnostic) {
    if (!diagnostic)
        return SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT;
    diagnostic_init(diagnostic);
    if (!capture)
        return SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT;
    if (capture->closed)
        return SHADER_RUNTIME_CAPTURE_CLOSED;
    CommonPathDiscoveryResult current = {0};
    const char *root = capture->root;
    diagnostic->discovery_status =
        common_path_discover(&root, 1U, &capture->discovery_options, &current);
    bool same = diagnostic->discovery_status == COMMON_PATH_DISCOVERY_OK &&
                current.count == capture->discovered.count;
    for (size_t i = 0U; same && i < current.count; ++i)
        same = strcmp(current.paths[i].path, capture->discovered.paths[i].path) == 0 &&
               current.paths[i].explicit_file == capture->discovered.paths[i].explicit_file;
    common_path_discovery_result_dispose(&current);
    /* Do not short circuit: every lease must close even when discovery failed. */
    const bool stable_files = close_files(capture, diagnostic);
    capture->sealed = same && stable_files;
    return capture->sealed ? SHADER_RUNTIME_CAPTURE_OK : SHADER_RUNTIME_CAPTURE_INPUT_CHANGED;
}

bool shader_runtime_capture_describe(const ShaderRuntimeCapture *capture,
                                     ShaderRuntimeImageSummary *summary) {
    if (!capture || !capture->sealed || !summary)
        return false;
    *summary = capture->summary;
    return true;
}

const char *shader_runtime_capture_status_name(ShaderRuntimeCaptureStatus status) {
    switch (status) {
    case SHADER_RUNTIME_CAPTURE_OK:
        return "ok";
    case SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT:
        return "invalid-argument";
    case SHADER_RUNTIME_CAPTURE_DISCOVERY_FAILED:
        return "discovery-failed";
    case SHADER_RUNTIME_CAPTURE_EMPTY:
        return "empty";
    case SHADER_RUNTIME_CAPTURE_INVALID_PATH:
        return "invalid-path";
    case SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED:
        return "limit-exceeded";
    case SHADER_RUNTIME_CAPTURE_FILE_UNAVAILABLE:
        return "file-unavailable";
    case SHADER_RUNTIME_CAPTURE_INPUT_CHANGED:
        return "input-changed";
    case SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED:
        return "allocation-failed";
    case SHADER_RUNTIME_CAPTURE_CLOSED:
        return "closed";
    }
    return "unknown";
}
