// SPDX-License-Identifier: GPL-3.0-only
#ifndef SHADER_RUNTIME_CAPTURE_H
#define SHADER_RUNTIME_CAPTURE_H

#include "common/file_io.h"
#include "common/path_discovery.h"

typedef struct ShaderRuntimeCapture ShaderRuntimeCapture;

typedef struct {
    size_t max_files;
    size_t max_directories;
    size_t max_depth;
    size_t max_path_bytes;
    size_t max_file_bytes;
    size_t max_total_bytes;
} ShaderRuntimeCaptureLimits;

typedef enum {
    SHADER_RUNTIME_CAPTURE_OK = 0,
    SHADER_RUNTIME_CAPTURE_INVALID_ARGUMENT,
    SHADER_RUNTIME_CAPTURE_DISCOVERY_FAILED,
    SHADER_RUNTIME_CAPTURE_EMPTY,
    SHADER_RUNTIME_CAPTURE_INVALID_PATH,
    SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED,
    SHADER_RUNTIME_CAPTURE_FILE_UNAVAILABLE,
    SHADER_RUNTIME_CAPTURE_INPUT_CHANGED,
    SHADER_RUNTIME_CAPTURE_ALLOCATION_FAILED,
    SHADER_RUNTIME_CAPTURE_CLOSED
} ShaderRuntimeCaptureStatus;

typedef struct {
    CommonPathDiscoveryStatus discovery_status;
    CommonFileStatus file_status;
    size_t file_index; /* SIZE_MAX unless an individual file failed. */
} ShaderRuntimeCaptureDiagnostic;

typedef struct {
    size_t file_count;
    size_t total_bytes;
    uint8_t image_digest[32];
} ShaderRuntimeImageSummary;

/* Own the complete shader-validation package directory, with no extension
 * filter. Every bound must be nonzero. Strict shared discovery rejects links,
 * special files and traversal limits. File views hold immutable snapshots and
 * their opening identities until finish/free. No caller report creates this
 * owner. Failed calls leave *output NULL. */
ShaderRuntimeCaptureStatus shader_runtime_capture_begin(const char *root,
                                                        const ShaderRuntimeCaptureLimits *limits,
                                                        ShaderRuntimeCapture **output,
                                                        ShaderRuntimeCaptureDiagnostic *diagnostic);

/* Borrow immutable file bytes during capture. Paths use '/' and Unicode UTF-8
 * bytes, sorted by unsigned byte ordering; absolute host paths are not exposed
 * or hashed. Views expire on finish/free. A failed call leaves outputs alone. */
bool shader_runtime_capture_file(const ShaderRuntimeCapture *capture, size_t index,
                                 const char **relative_path, const uint8_t **data, size_t *size);
size_t shader_runtime_capture_file_count(const ShaderRuntimeCapture *capture);

/* Re-discover the full namespace and close/revalidate every held file before
 * publishing the summary. Either outcome closes all views. Failure is terminal;
 * it cannot be retried into success. Repeated finish returns CLOSED. This is a
 * point-in-time package identity, not proof of loaded module closure, native
 * D3D11, execution, selection, or instrumentation transparency. The runtime
 * producer must independently bind those facts before issuing evidence. */
ShaderRuntimeCaptureStatus
shader_runtime_capture_finish(ShaderRuntimeCapture *capture,
                              ShaderRuntimeCaptureDiagnostic *diagnostic);
bool shader_runtime_capture_describe(const ShaderRuntimeCapture *capture,
                                     ShaderRuntimeImageSummary *summary);
void shader_runtime_capture_free(ShaderRuntimeCapture *capture);
const char *shader_runtime_capture_status_name(ShaderRuntimeCaptureStatus status);

#endif
