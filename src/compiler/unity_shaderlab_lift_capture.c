// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shaderlab_lift_capture.h"
#include "compiler/unity_shaderlab_lift_internal.h"

#include <stdlib.h>
#include <string.h>

struct UnityShaderLabLiftCapture {
    UnityShaderLabLiftResult *result;
    UnityShaderLabLiftCaptureReport authority;
};

void unity_shaderlab_lift_capture_free(UnityShaderLabLiftCapture *capture) {
    if (!capture)
        return;
    unity_shaderlab_lift_result_free(capture->result);
    free(capture);
}

const UnityShaderLabLiftResult *
unity_shaderlab_lift_capture_result(const UnityShaderLabLiftCapture *capture) {
    return capture ? capture->result : NULL;
}

UnityShaderLabLiftCaptureStatus
unity_shaderlab_lift_capture(const UnityShaderLabLiftCaptureInput *input,
                             UnityShaderLabLiftCapture **output,
                             UnityShaderLabLiftCaptureReport *report) {
    if (output)
        *output = NULL;
    if (!report)
        return UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->source_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    report->archive_status = SHADER_OBJECT_NOT_DECODED;
    report->lift_status = HLSL_LIFT_INVALID_ARGUMENT;
    if (!input || !output || !input->catalog || !input->record || !input->broker ||
        !input->limits || !unity_compile_profile_validate(input->profile) || !input->source_path ||
        !input->source_path[0] || !input->source_directory || !input->source_directory[0] ||
        !input->source_basename || !input->source_basename[0])
        return UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT;

    UnityShaderLabLiftCapture *capture = calloc(1, sizeof(*capture));
    if (!capture)
        return UNITY_SHADERLAB_CAPTURE_OUT_OF_MEMORY;
    ShaderObject target, revalidated;
    shader_object_init(&target);
    shader_object_init(&revalidated);
    ShaderBlobArchive archive = {0};
    UnityShaderLabLiftCaptureStatus status = UNITY_SHADERLAB_CAPTURE_SOURCE_UNAVAILABLE;
    report->source_status = shader_catalog_decode_object(input->catalog, input->record,
                                                         input->registry, &target, &report->target);
    if (report->source_status != SHADER_CATALOG_OBJECT_OK)
        goto cleanup;
    report->archive_status = shader_object_open_d3d11_archive(&target, &archive);
    if (report->archive_status != SHADER_OBJECT_OK) {
        status = UNITY_SHADERLAB_CAPTURE_ARCHIVE_UNAVAILABLE;
        goto cleanup;
    }
    const UnityCompileProfile profile = *input->profile;
    const UnityShaderLabLiftInput lift = {
        .shader = &target.shader,
        .archive = &archive,
        .profile = &profile,
        .broker = input->broker,
        .source_path = input->source_path,
        .source_directory = input->source_directory,
        .source_basename = input->source_basename,
    };
    report->lift_status = unity_shaderlab_lift_run(&lift, NULL, input->limits, &capture->result);
    const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(capture->result);
    if (!accepted || !capture->result->authority_pinned || !sb_ok(&accepted->source)) {
        status = UNITY_SHADERLAB_CAPTURE_NO_ACCEPTED_SOURCE;
        goto cleanup;
    }
    ShaderCatalogObjectReport after;
    report->source_status = shader_catalog_decode_object(input->catalog, input->record,
                                                         input->registry, &revalidated, &after);
    if (report->source_status != SHADER_CATALOG_OBJECT_OK ||
        memcmp(report->target.release_digest, after.release_digest, 32) != 0) {
        status = UNITY_SHADERLAB_CAPTURE_SOURCE_CHANGED;
        goto cleanup;
    }
    common_sha256(accepted->source.buf, accepted->source.len, report->accepted_source_digest);
    memcpy(report->compiler_digest, capture->result->compiler_digest, 32);
    memcpy(report->environment_digest, capture->result->environment_digest, 32);
    memcpy(report->profile_digest, capture->result->profile_digest, 32);
    capture->authority = *report;
    *output = capture;
    capture = NULL;
    status = UNITY_SHADERLAB_CAPTURE_OK;
cleanup:
    shader_blob_archive_close(&archive);
    shader_object_dispose(&revalidated);
    shader_object_dispose(&target);
    unity_shaderlab_lift_capture_free(capture);
    return status;
}
