// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_SHADERLAB_LIFT_CAPTURE_H
#define UNITY_SHADERLAB_LIFT_CAPTURE_H

#include "app/shader_catalog_object.h"
#include "compiler/unity_shaderlab_lift.h"

typedef struct UnityShaderLabLiftCapture UnityShaderLabLiftCapture;

typedef struct {
    const ShaderCatalog *catalog;
    const ShaderCatalogRecord *record;
    const TypeTreeSchemaRegistry *registry;
    const UnityCompileProfile *profile;
    UnityCompilerBroker *broker;
    const char *source_path;
    const char *source_directory;
    const char *source_basename;
    const HLSLLiftLimits *limits;
} UnityShaderLabLiftCaptureInput;

typedef enum {
    UNITY_SHADERLAB_CAPTURE_OK = 0,
    UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT,
    UNITY_SHADERLAB_CAPTURE_SOURCE_UNAVAILABLE,
    UNITY_SHADERLAB_CAPTURE_ARCHIVE_UNAVAILABLE,
    UNITY_SHADERLAB_CAPTURE_NO_ACCEPTED_SOURCE,
    UNITY_SHADERLAB_CAPTURE_SOURCE_CHANGED,
    UNITY_SHADERLAB_CAPTURE_OUT_OF_MEMORY
} UnityShaderLabLiftCaptureStatus;

typedef struct {
    ShaderCatalogObjectStatus source_status;
    ShaderObjectStatus archive_status;
    HLSLLiftStatus lift_status;
    ShaderCatalogObjectReport target;
    uint8_t accepted_source_digest[32];
    uint8_t compiler_digest[32];
    uint8_t environment_digest[32];
    uint8_t profile_digest[32];
} UnityShaderLabLiftCaptureReport;

/* Execute the production lift against an owned captured catalog record. The
 * profile is copied, the decoded object/archive are held for the whole run,
 * and the source snapshot is revalidated afterwards. No injected callbacks or
 * caller-supplied result reports can create this authority. OK means an
 * accepted artifact exists (possibly the verified low-level fallback), not
 * whole-shader equivalence or import acceptance. Other returns leave output
 * NULL. Inputs and borrowed broker must remain valid throughout this call. */
UnityShaderLabLiftCaptureStatus
unity_shaderlab_lift_capture(const UnityShaderLabLiftCaptureInput *input,
                             UnityShaderLabLiftCapture **output,
                             UnityShaderLabLiftCaptureReport *report);
const UnityShaderLabLiftResult *
unity_shaderlab_lift_capture_result(const UnityShaderLabLiftCapture *capture);
void unity_shaderlab_lift_capture_free(UnityShaderLabLiftCapture *capture);

#endif
