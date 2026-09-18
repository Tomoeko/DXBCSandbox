// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMPUTE_SHADER_ARTIFACT_H
#define COMPUTE_SHADER_ARTIFACT_H

#include "common/sha256.h"
#include "common/string_builder.h"
#include "io/compute_shader_object.h"

typedef enum {
    COMPUTE_SHADER_BINARY_SERIALIZED_OBJECT = 0,
    COMPUTE_SHADER_BINARY_PROGRAM,
    COMPUTE_SHADER_BINARY_DXBC,
} ComputeShaderBinaryArtifactKind;

typedef struct {
    char filename[224];
    const uint8_t* data;
    size_t size;
    uint8_t sha256[COMMON_SHA256_DIGEST_SIZE];
    char sha256_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    ComputeShaderBinaryArtifactKind kind;
} ComputeShaderBinaryArtifact;

typedef struct {
    char manifest_filename[160];
    StringBuilder manifest;
    ComputeShaderBinaryArtifact* binaries;
    size_t binary_count;
} ComputeShaderArtifactPackage;

typedef enum {
    COMPUTE_SHADER_ARTIFACT_NOT_APPLICABLE = 0,
    COMPUTE_SHADER_ARTIFACT_OK,
    COMPUTE_SHADER_ARTIFACT_INVALID_ARGUMENT,
    COMPUTE_SHADER_ARTIFACT_NOT_DECODED,
    COMPUTE_SHADER_ARTIFACT_SIZE_OVERFLOW,
    COMPUTE_SHADER_ARTIFACT_ALLOCATION_FAILED,
    COMPUTE_SHADER_ARTIFACT_FORMAT_FAILED,
} ComputeShaderArtifactStatus;

void compute_shader_artifact_package_init(ComputeShaderArtifactPackage* package);
void compute_shader_artifact_package_dispose(ComputeShaderArtifactPackage* package);

/* Builds a deterministic exact-authority manifest, the complete serialized
 * ClassID 72 object bytes, and one or two binary records per compiled code
 * blob. A wrapped Unity program is retained in full and its embedded DXBC is
 * also exposed as a first-class artifact. */
ComputeShaderArtifactStatus compute_shader_artifact_build(
    const ComputeShaderObject* object, ComputeShaderArtifactPackage* package);

const char* compute_shader_artifact_status_name(ComputeShaderArtifactStatus status);

#endif /* COMPUTE_SHADER_ARTIFACT_H */
