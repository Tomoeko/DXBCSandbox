// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_OBJECT_H
#define SHADER_OBJECT_H

#include "io/serialized_file.h"
#include "io/shader_blob_archive.h"
#include "io/serialized_shader.h"

/*
 * Exact, reusable decoding boundary for one Unity ClassID 48 object.
 *
 * ShaderObject owns its cloned TypeTree schema, parsed TypeTreeValue, and
 * projected SerializedShader.  It therefore remains valid after its source
 * SerializedFile and source bytes have been released.  Initialize it before
 * first use and dispose it when finished.  Decode has a strong output
 * guarantee: failure leaves an initialized destination unchanged; success
 * replaces it.
 */

typedef enum {
    SHADER_OBJECT_OK = 0,
    SHADER_OBJECT_INVALID_ARGUMENT,
    SHADER_OBJECT_INVALID_SOURCE_FILE,
    SHADER_OBJECT_UNSUPPORTED_FILE_VERSION,
    SHADER_OBJECT_OBJECT_NOT_OWNED,
    SHADER_OBJECT_OBJECT_RANGE_INVALID,
    SHADER_OBJECT_NOT_SHADER,
    SHADER_OBJECT_TYPE_INDEX_INVALID,
    SHADER_OBJECT_TYPE_RECORD_MISMATCH,
    SHADER_OBJECT_SCHEMA_UNRESOLVED,
    SHADER_OBJECT_SCHEMA_INVALID,
    SHADER_OBJECT_SCHEMA_SIZE_OVERFLOW,
    SHADER_OBJECT_ALLOCATION_FAILED,
    SHADER_OBJECT_UNSUPPORTED_UNITY_VERSION,
    SHADER_OBJECT_TYPETREE_PARSE_FAILED,
    SHADER_OBJECT_TYPETREE_NODES_NOT_EXHAUSTED,
    SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED,
    SHADER_OBJECT_SHADER_MODEL_INVALID,
    SHADER_OBJECT_NOT_DECODED,
    SHADER_OBJECT_D3D11_PLATFORM_ABSENT,
    SHADER_OBJECT_D3D11_PLATFORM_AMBIGUOUS,
    SHADER_OBJECT_PLATFORM_TABLE_INVALID,
    SHADER_OBJECT_D3D11_ARCHIVE_INVALID,
} ShaderObjectStatus;

typedef struct {
    TypeTreeType schema;
    TypeTreeValue root;
    SerializedShader shader;
    SerializedShaderSchemaProfile profile;

    int64_t path_id;
    uint64_t byte_offset;
    uint32_t byte_size;
    int32_t source_type_index;
    uint32_t serialized_file_version;
    uint32_t target_platform;
    bool decoded;
} ShaderObject;

void shader_object_init(ShaderObject* object);
void shader_object_dispose(ShaderObject* object);

ShaderObjectStatus shader_object_decode(
    ShaderObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object);

/* Allocation-light variant for bounded processing. Packed UInt8 arrays in
 * the parsed root borrow the SerializedFile backing bytes, so destination
 * must be disposed before file and its raw_data become invalid. All other
 * validation and ownership guarantees are identical to shader_object_decode.
 */
ShaderObjectStatus shader_object_decode_borrowed(
    ShaderObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object);

/* Validates the platform plane and reports whether exactly one D3D11 archive
 * is present without decompressing it. */
ShaderObjectStatus shader_object_d3d11_platform_status(
    const ShaderObject* object);

/*
 * Opens Unity ShaderCompilerPlatform 4 (D3D11) from a decoded object.
 * A well-formed archive with no D3D11 platform returns
 * SHADER_OBJECT_D3D11_PLATFORM_ABSENT; malformed/ambiguous platform planes
 * and corrupt compressed archives have separate statuses.
 *
 * destination must be all-zero or a previously opened ShaderBlobArchive.
 * Failure leaves it unchanged; success replaces it.  The returned archive
 * owns its decompressed segments and may outlive the ShaderObject.
 */
ShaderObjectStatus shader_object_open_d3d11_archive(
    const ShaderObject* object, ShaderBlobArchive* destination);

const char* shader_object_status_name(ShaderObjectStatus status);

#endif /* SHADER_OBJECT_H */
