// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_GLCORE_TARGET_H
#define SERIALIZED_GLCORE_TARGET_H

#include "common/shader_stage.h"
#include "io/shader_blob_archive.h"
#include "io/shader_object.h"

/*
 * Exact Unity 2021.3 player representation observed for a linked desktop
 * OpenGL program.  These constants deliberately do not generalize the
 * broader enum ranges accepted by SerializedShader: program types 7 and 8
 * have not been assigned linked-program semantics by this API.
 */
#define SERIALIZED_GLCORE_PLATFORM 15
#define SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE 6
#define SERIALIZED_GLCORE_ARCHIVE_STAGE_COUNT UINT32_C(1)
#define SERIALIZED_GLCORE_GENERIC_TIER_GROUP 3

typedef enum {
    SERIALIZED_GLCORE_TARGET_OK = 0,
    SERIALIZED_GLCORE_TARGET_INVALID_ARGUMENT,
    SERIALIZED_GLCORE_TARGET_NOT_DECODED,
    SERIALIZED_GLCORE_TARGET_UNSUPPORTED_UNITY_VERSION,
    SERIALIZED_GLCORE_TARGET_WRONG_PLATFORM,
    SERIALIZED_GLCORE_TARGET_PLATFORM_ABSENT,
    SERIALIZED_GLCORE_TARGET_PLATFORM_AMBIGUOUS,
    SERIALIZED_GLCORE_TARGET_PLATFORM_TABLE_INVALID,
    SERIALIZED_GLCORE_TARGET_ARCHIVE_INVALID,
    SERIALIZED_GLCORE_TARGET_ARCHIVE_STAGE_COUNT_INVALID,
    SERIALIZED_GLCORE_TARGET_PASS_INVALID,
    SERIALIZED_GLCORE_TARGET_LINK_OWNER_INVALID,
    SERIALIZED_GLCORE_TARGET_PROGRAM_TYPE_UNSUPPORTED,
    SERIALIZED_GLCORE_TARGET_BLOB_INDEX_INVALID,
    SERIALIZED_GLCORE_TARGET_WRAPPER_DIALECT_UNSUPPORTED,
    SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID,
    SERIALIZED_GLCORE_TARGET_WRAPPER_PROGRAM_TYPE_MISMATCH,
    SERIALIZED_GLCORE_TARGET_WRAPPER_KEYWORDS_MISMATCH,
    SERIALIZED_GLCORE_TARGET_LINK_TEXT_INVALID,
    SERIALIZED_GLCORE_TARGET_ALLOCATION_FAILED
} SerializedGLCoreTargetStatus;

/* Complete serialized owner of one linked program.  A blob index may be
 * referenced by more than one owner; archive_entry_index is therefore an
 * identity coordinate, not a uniqueness assertion. */
typedef struct {
    int64_t shader_path_id;
    int32_t subshader_index;
    int32_t pass_index;
    UnitySerializedProgramStage serialized_stage;
    int32_t flattened_subprogram_index;
    int32_t hardware_tier_group;
    int32_t inner_subprogram_index;
    int32_t archive_entry_index;
} SerializedGLCoreLinkOwner;

/*
 * Owns an exact copy of the complete player-blob wrapper. released_text_bytes
 * points into wrapper_bytes; neither span is NUL-terminated or normalized.
 * Padding, line endings, and the final newline are all identity-bearing.
 */
typedef struct {
    const char* unity_version;
    SerializedShaderSchemaProfile schema_profile;
    int32_t compiler_platform;
    int32_t program_type;
    uint32_t player_blob_version;
    PlayerBlobDialect player_blob_dialect;
    uint32_t archive_stage_count;
    SerializedGLCoreLinkOwner owner;

    uint8_t* wrapper_bytes;
    size_t wrapper_size;
    size_t released_text_offset;
    const uint8_t* released_text_bytes;
    size_t released_text_size;
} SerializedGLCoreTarget;

/* Lower, decompressed boundary used by bounded batch processing and tests. */
typedef struct {
    const char* unity_version;
    int32_t compiler_platform;
    const ShaderBlobArchive* archive;
    const SerializedPass* pass;
    int64_t shader_path_id;
    int32_t subshader_index;
    int32_t pass_index;
    int32_t stage_index;
    int32_t flattened_subprogram_index;
} SerializedGLCoreTargetInput;

void serialized_glcore_target_init(SerializedGLCoreTarget* target);
void serialized_glcore_target_dispose(SerializedGLCoreTarget* target);

/* Revalidates the self-contained target and its exact wrapper/text view. */
SerializedGLCoreTargetStatus serialized_glcore_target_validate(
    const SerializedGLCoreTarget* target);

/* Validates the platform-15 archive plane without inferring it from D3D. */
SerializedGLCoreTargetStatus serialized_glcore_object_readiness(
    const ShaderObject* object);

/* Opens one exact serialized link owner.  Success replaces destination;
 * failure leaves an initialized destination unchanged. */
SerializedGLCoreTargetStatus serialized_glcore_target_open(
    SerializedGLCoreTarget* destination,
    const SerializedGLCoreTargetInput* input);

/* First-class decoded ShaderObject entry point. */
SerializedGLCoreTargetStatus serialized_glcore_target_open_object(
    SerializedGLCoreTarget* destination, const ShaderObject* object,
    int32_t subshader_index, int32_t pass_index, int32_t stage_index,
    int32_t flattened_subprogram_index);

const char* serialized_glcore_target_status_name(
    SerializedGLCoreTargetStatus status);

#endif /* SERIALIZED_GLCORE_TARGET_H */
