// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_glcore_target.h"

#include "common/stream.h"

#include <limits.h>
#include <string.h>

static const char* profile_unity_version(
    SerializedShaderSchemaProfile profile) {
    switch (profile) {
        case SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1:
            return "2021.3.35f1";
        case SERIALIZED_SHADER_PROFILE_UNITY_2021_3_29F1_PLAYER_RESOURCES:
            return "2021.3.29f1";
        default:
            return NULL;
    }
}

static bool exact_profile(const char* unity_version,
                          SerializedShaderSchemaProfile* profile,
                          const char** canonical_version) {
    if (!unity_version || !profile || !canonical_version ||
        !serialized_shader_profile_from_unity_version(unity_version,
                                                      profile)) {
        return false;
    }
    *canonical_version = profile_unity_version(*profile);
    return *canonical_version && strcmp(unity_version, *canonical_version) == 0;
}

void serialized_glcore_target_init(SerializedGLCoreTarget* target) {
    if (target) memset(target, 0, sizeof(*target));
}

void serialized_glcore_target_dispose(SerializedGLCoreTarget* target) {
    if (!target) return;
    if (target->wrapper_bytes) {
        mem_free(target->wrapper_bytes, target->wrapper_size);
    }
    serialized_glcore_target_init(target);
}

static SerializedGLCoreTargetStatus archive_platform_status(
    const SerializedShader* shader) {
    if (!shader || shader->archive_platform_count < 0 ||
        (shader->archive_platform_count > 0 &&
         !shader->archive_platforms)) {
        return SERIALIZED_GLCORE_TARGET_PLATFORM_TABLE_INVALID;
    }
    int glcore_count = 0;
    for (int index = 0; index < shader->archive_platform_count; ++index) {
        for (int previous = 0; previous < index; ++previous) {
            if (shader->archive_platforms[index] ==
                shader->archive_platforms[previous]) {
                return shader->archive_platforms[index] ==
                               SERIALIZED_GLCORE_PLATFORM
                    ? SERIALIZED_GLCORE_TARGET_PLATFORM_AMBIGUOUS
                    : SERIALIZED_GLCORE_TARGET_PLATFORM_TABLE_INVALID;
            }
        }
        if (shader->archive_platforms[index] ==
            SERIALIZED_GLCORE_PLATFORM) {
            ++glcore_count;
        }
    }
    if (glcore_count == 0) return SERIALIZED_GLCORE_TARGET_PLATFORM_ABSENT;
    if (glcore_count != 1) {
        return SERIALIZED_GLCORE_TARGET_PLATFORM_AMBIGUOUS;
    }
    return SERIALIZED_GLCORE_TARGET_OK;
}

static SerializedGLCoreTargetStatus open_object_archive(
    const ShaderObject* object, ShaderBlobArchive* archive,
    const char** unity_version) {
    if (!object || !archive || !unity_version) {
        return SERIALIZED_GLCORE_TARGET_INVALID_ARGUMENT;
    }
    if (!object->decoded) return SERIALIZED_GLCORE_TARGET_NOT_DECODED;
    *unity_version = profile_unity_version(object->profile);
    if (!*unity_version) {
        return SERIALIZED_GLCORE_TARGET_UNSUPPORTED_UNITY_VERSION;
    }
    SerializedGLCoreTargetStatus status =
        archive_platform_status(&object->shader);
    if (status != SERIALIZED_GLCORE_TARGET_OK) return status;

    memset(archive, 0, sizeof(*archive));
    if (!shader_blob_archive_open(&object->root,
                                  SERIALIZED_GLCORE_PLATFORM, archive)) {
        shader_blob_archive_close(archive);
        return SERIALIZED_GLCORE_TARGET_ARCHIVE_INVALID;
    }
    if (archive->stage_count != SERIALIZED_GLCORE_ARCHIVE_STAGE_COUNT) {
        shader_blob_archive_close(archive);
        return SERIALIZED_GLCORE_TARGET_ARCHIVE_STAGE_COUNT_INVALID;
    }
    return SERIALIZED_GLCORE_TARGET_OK;
}

SerializedGLCoreTargetStatus serialized_glcore_object_readiness(
    const ShaderObject* object) {
    ShaderBlobArchive archive;
    const char* unity_version = NULL;
    SerializedGLCoreTargetStatus status = open_object_archive(
        object, &archive, &unity_version);
    (void)unity_version;
    if (status == SERIALIZED_GLCORE_TARGET_OK) {
        shader_blob_archive_close(&archive);
    }
    return status;
}

static SerializedGLCoreTargetStatus pass_platform_status(
    const SerializedPass* pass) {
    if (!pass || !pass->has_serialized_platforms ||
        pass->platform_count < 0 ||
        (pass->platform_count > 0 && !pass->platforms)) {
        return SERIALIZED_GLCORE_TARGET_PASS_INVALID;
    }
    int glcore_count = 0;
    for (int index = 0; index < pass->platform_count; ++index) {
        for (int previous = 0; previous < index; ++previous) {
            if (pass->platforms[index] == pass->platforms[previous]) {
                return SERIALIZED_GLCORE_TARGET_PASS_INVALID;
            }
        }
        if (pass->platforms[index] == SERIALIZED_GLCORE_PLATFORM) {
            ++glcore_count;
        }
    }
    if (glcore_count == 0) return SERIALIZED_GLCORE_TARGET_PLATFORM_ABSENT;
    if (glcore_count != 1) return SERIALIZED_GLCORE_TARGET_PASS_INVALID;
    return SERIALIZED_GLCORE_TARGET_OK;
}

static bool decompressed_archive_is_valid(
    const ShaderBlobArchive* archive) {
    if (!archive || archive->entry_count <= 0 ||
        archive->segment_count <= 0 || !archive->entries ||
        !archive->segments || !archive->segment_lengths) {
        return false;
    }
    for (int segment = 0; segment < archive->segment_count; ++segment) {
        if (archive->segment_lengths[segment] < 0 ||
            (archive->segment_lengths[segment] > 0 &&
             !archive->segments[segment])) {
            return false;
        }
    }
    for (int entry_index = 0; entry_index < archive->entry_count;
         ++entry_index) {
        const BlobEntry* entry = &archive->entries[entry_index];
        if (entry->segment < 0 || entry->segment >= archive->segment_count ||
            entry->offset < 0 || entry->length < 0) {
            return false;
        }
        const size_t segment_size =
            (size_t)archive->segment_lengths[entry->segment];
        if ((size_t)entry->offset > segment_size ||
            (size_t)entry->length >
                segment_size - (size_t)entry->offset) {
            return false;
        }
    }
    return true;
}

static bool span_starts_with(const uint8_t* span, size_t span_size,
                             const char* prefix) {
    const size_t prefix_size = strlen(prefix);
    return span && span_size >= prefix_size &&
           memcmp(span, prefix, prefix_size) == 0;
}

static bool span_contains(const uint8_t* span, size_t span_size,
                          const char* needle) {
    const size_t needle_size = strlen(needle);
    if (!span || needle_size == 0U || span_size < needle_size) return false;
    for (size_t offset = 0U; offset <= span_size - needle_size; ++offset) {
        if (memcmp(span + offset, needle, needle_size) == 0) return true;
    }
    return false;
}

static bool linked_text_is_valid(const uint8_t* text, size_t text_size) {
    return text && text_size > 0U &&
           memchr(text, '\0', text_size) == NULL &&
           span_starts_with(text, text_size, "#ifdef VERTEX\n") &&
           span_contains(text, text_size, "\n#ifdef FRAGMENT\n");
}

SerializedGLCoreTargetStatus serialized_glcore_target_validate(
    const SerializedGLCoreTarget* target) {
    if (!target || !target->unity_version || !target->wrapper_bytes ||
        target->wrapper_size == 0U || !target->released_text_bytes ||
        target->released_text_size == 0U) {
        return SERIALIZED_GLCORE_TARGET_INVALID_ARGUMENT;
    }
    const char* canonical_version = profile_unity_version(
        target->schema_profile);
    if (!canonical_version ||
        strcmp(target->unity_version, canonical_version) != 0) {
        return SERIALIZED_GLCORE_TARGET_UNSUPPORTED_UNITY_VERSION;
    }
    if (target->compiler_platform != SERIALIZED_GLCORE_PLATFORM) {
        return SERIALIZED_GLCORE_TARGET_WRONG_PLATFORM;
    }
    if (target->program_type != SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE) {
        return SERIALIZED_GLCORE_TARGET_PROGRAM_TYPE_UNSUPPORTED;
    }
    if (target->archive_stage_count !=
        SERIALIZED_GLCORE_ARCHIVE_STAGE_COUNT) {
        return SERIALIZED_GLCORE_TARGET_ARCHIVE_STAGE_COUNT_INVALID;
    }
    if (target->owner.subshader_index < 0 || target->owner.pass_index < 0 ||
        target->owner.serialized_stage != UNITY_SERIALIZED_STAGE_VERTEX ||
        target->owner.flattened_subprogram_index < 0 ||
        target->owner.hardware_tier_group !=
            SERIALIZED_GLCORE_GENERIC_TIER_GROUP ||
        target->owner.inner_subprogram_index < 0 ||
        target->owner.archive_entry_index < 0) {
        return SERIALIZED_GLCORE_TARGET_LINK_OWNER_INVALID;
    }
    if (target->released_text_offset > target->wrapper_size ||
        target->released_text_size >
            target->wrapper_size - target->released_text_offset ||
        target->released_text_bytes !=
            target->wrapper_bytes + target->released_text_offset) {
        return SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID;
    }
    ByteStream stream;
    stream_init(&stream, target->wrapper_bytes, target->wrapper_size);
    stream_set_endian(&stream, false);
    PlayerSubProgramMetadata metadata;
    if (!subprogram_metadata_parse_variant(&stream, &metadata)) {
        return SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID;
    }
    SerializedGLCoreTargetStatus status = SERIALIZED_GLCORE_TARGET_OK;
    if (metadata.version != target->player_blob_version ||
        metadata.dialect != target->player_blob_dialect ||
        metadata.version != UNITY_2021_3_PLAYER_BLOB_VERSION ||
        metadata.dialect != PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1) {
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_DIALECT_UNSUPPORTED;
    } else if (metadata.program_type != target->program_type) {
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_PROGRAM_TYPE_MISMATCH;
    } else if (!metadata.bytecode ||
               (size_t)(metadata.bytecode - target->wrapper_bytes) !=
                   target->released_text_offset ||
               metadata.bytecode_length != target->released_text_size) {
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID;
    } else if (!linked_text_is_valid(target->released_text_bytes,
                                     target->released_text_size)) {
        status = SERIALIZED_GLCORE_TARGET_LINK_TEXT_INVALID;
    }
    subprogram_metadata_free_variant(&metadata);
    return status;
}

SerializedGLCoreTargetStatus serialized_glcore_target_open(
    SerializedGLCoreTarget* destination,
    const SerializedGLCoreTargetInput* input) {
    if (!destination || !input || !input->unity_version || !input->archive ||
        !input->pass || input->subshader_index < 0 ||
        input->pass_index < 0 || input->stage_index < 0 ||
        input->stage_index >= UNITY_SERIALIZED_STAGE_COUNT ||
        input->flattened_subprogram_index < 0) {
        return SERIALIZED_GLCORE_TARGET_INVALID_ARGUMENT;
    }
    SerializedShaderSchemaProfile profile;
    const char* canonical_version = NULL;
    if (!exact_profile(input->unity_version, &profile, &canonical_version)) {
        return SERIALIZED_GLCORE_TARGET_UNSUPPORTED_UNITY_VERSION;
    }
    if (input->compiler_platform != SERIALIZED_GLCORE_PLATFORM) {
        return SERIALIZED_GLCORE_TARGET_WRONG_PLATFORM;
    }

    SerializedGLCoreTargetStatus status = pass_platform_status(input->pass);
    if (status != SERIALIZED_GLCORE_TARGET_OK) return status;
    if (!decompressed_archive_is_valid(input->archive)) {
        return SERIALIZED_GLCORE_TARGET_ARCHIVE_INVALID;
    }
    if (input->archive->stage_count !=
        SERIALIZED_GLCORE_ARCHIVE_STAGE_COUNT) {
        return SERIALIZED_GLCORE_TARGET_ARCHIVE_STAGE_COUNT_INVALID;
    }
    if (input->stage_index != UNITY_SERIALIZED_STAGE_VERTEX) {
        return SERIALIZED_GLCORE_TARGET_LINK_OWNER_INVALID;
    }

    const int stage = input->stage_index;
    const int subprogram_index = input->flattened_subprogram_index;
    if (input->pass->subprogram_count[stage] < 0 ||
        subprogram_index >= input->pass->subprogram_count[stage] ||
        !input->pass->subprograms[stage] ||
        !input->pass->subprogram_identities[stage]) {
        return SERIALIZED_GLCORE_TARGET_PASS_INVALID;
    }
    const SerializedSubProgram* serialized =
        &input->pass->subprograms[stage][subprogram_index];
    const SerializedSubProgramIdentity* identity =
        &input->pass->subprogram_identities[stage][subprogram_index];
    if (identity->hardware_tier_group !=
            SERIALIZED_GLCORE_GENERIC_TIER_GROUP ||
        identity->inner_subprogram_index < 0) {
        return SERIALIZED_GLCORE_TARGET_LINK_OWNER_INVALID;
    }
    if (serialized->program_type !=
        SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE) {
        return SERIALIZED_GLCORE_TARGET_PROGRAM_TYPE_UNSUPPORTED;
    }

    const uint8_t* wrapper = NULL;
    size_t wrapper_size = 0U;
    if (serialized->blob_index < 0 ||
        !shader_blob_archive_get(input->archive, serialized->blob_index,
                                 &wrapper, &wrapper_size)) {
        return SERIALIZED_GLCORE_TARGET_BLOB_INDEX_INVALID;
    }
    if (!wrapper || wrapper_size < sizeof(uint32_t)) {
        return SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID;
    }
    uint32_t wrapper_version = 0U;
    memcpy(&wrapper_version, wrapper, sizeof(wrapper_version));
    wrapper_version = read_le32(wrapper_version);
    if (wrapper_version != UNITY_2021_3_PLAYER_BLOB_VERSION) {
        return SERIALIZED_GLCORE_TARGET_WRAPPER_DIALECT_UNSUPPORTED;
    }

    ByteStream stream;
    stream_init(&stream, wrapper, wrapper_size);
    stream_set_endian(&stream, false);
    PlayerSubProgramMetadata metadata;
    if (!subprogram_metadata_parse_variant(&stream, &metadata)) {
        return SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID;
    }
    if (metadata.dialect != PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1) {
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_DIALECT_UNSUPPORTED;
        goto metadata_done;
    }
    if (metadata.program_type != serialized->program_type) {
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_PROGRAM_TYPE_MISMATCH;
        goto metadata_done;
    }
    if (!subprogram_metadata_local_keyword_set_matches(&metadata,
                                                        serialized)) {
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_KEYWORDS_MISMATCH;
        goto metadata_done;
    }
    if (!linked_text_is_valid(metadata.bytecode,
                              metadata.bytecode_length)) {
        status = SERIALIZED_GLCORE_TARGET_LINK_TEXT_INVALID;
        goto metadata_done;
    }
    if (metadata.bytecode < wrapper ||
        (size_t)(metadata.bytecode - wrapper) > wrapper_size ||
        metadata.bytecode_length >
            wrapper_size - (size_t)(metadata.bytecode - wrapper)) {
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID;
        goto metadata_done;
    }

    SerializedGLCoreTarget candidate;
    serialized_glcore_target_init(&candidate);
    candidate.wrapper_bytes = (uint8_t*)mem_alloc(wrapper_size);
    if (!candidate.wrapper_bytes) {
        status = SERIALIZED_GLCORE_TARGET_ALLOCATION_FAILED;
        goto metadata_done;
    }
    memcpy(candidate.wrapper_bytes, wrapper, wrapper_size);
    const size_t text_offset = (size_t)(metadata.bytecode - wrapper);
    candidate.unity_version = canonical_version;
    candidate.schema_profile = profile;
    candidate.compiler_platform = SERIALIZED_GLCORE_PLATFORM;
    candidate.program_type = serialized->program_type;
    candidate.player_blob_version = metadata.version;
    candidate.player_blob_dialect = metadata.dialect;
    candidate.archive_stage_count = input->archive->stage_count;
    candidate.owner.shader_path_id = input->shader_path_id;
    candidate.owner.subshader_index = input->subshader_index;
    candidate.owner.pass_index = input->pass_index;
    candidate.owner.serialized_stage =
        (UnitySerializedProgramStage)input->stage_index;
    candidate.owner.flattened_subprogram_index = subprogram_index;
    candidate.owner.hardware_tier_group = identity->hardware_tier_group;
    candidate.owner.inner_subprogram_index =
        identity->inner_subprogram_index;
    candidate.owner.archive_entry_index = serialized->blob_index;
    candidate.wrapper_size = wrapper_size;
    candidate.released_text_offset = text_offset;
    candidate.released_text_bytes = candidate.wrapper_bytes + text_offset;
    candidate.released_text_size = metadata.bytecode_length;

    if (serialized_glcore_target_validate(&candidate) !=
        SERIALIZED_GLCORE_TARGET_OK) {
        serialized_glcore_target_dispose(&candidate);
        status = SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID;
        goto metadata_done;
    }

    serialized_glcore_target_dispose(destination);
    *destination = candidate;
    status = SERIALIZED_GLCORE_TARGET_OK;

metadata_done:
    subprogram_metadata_free_variant(&metadata);
    return status;
}

SerializedGLCoreTargetStatus serialized_glcore_target_open_object(
    SerializedGLCoreTarget* destination, const ShaderObject* object,
    int32_t subshader_index, int32_t pass_index, int32_t stage_index,
    int32_t flattened_subprogram_index) {
    if (!destination || !object || subshader_index < 0 || pass_index < 0 ||
        stage_index < 0 ||
        flattened_subprogram_index < 0) {
        return SERIALIZED_GLCORE_TARGET_INVALID_ARGUMENT;
    }
    ShaderBlobArchive archive;
    const char* unity_version = NULL;
    SerializedGLCoreTargetStatus status = open_object_archive(
        object, &archive, &unity_version);
    if (status != SERIALIZED_GLCORE_TARGET_OK) return status;

    if (subshader_index >= object->shader.subshader_count ||
        !object->shader.subshaders ||
        pass_index >= object->shader.subshaders[subshader_index].pass_count ||
        !object->shader.subshaders[subshader_index].passes) {
        shader_blob_archive_close(&archive);
        return SERIALIZED_GLCORE_TARGET_PASS_INVALID;
    }
    const SerializedPass* pass =
        &object->shader.subshaders[subshader_index].passes[pass_index];
    const SerializedGLCoreTargetInput input = {
        .unity_version = unity_version,
        .compiler_platform = SERIALIZED_GLCORE_PLATFORM,
        .archive = &archive,
        .pass = pass,
        .shader_path_id = object->path_id,
        .subshader_index = subshader_index,
        .pass_index = pass_index,
        .stage_index = stage_index,
        .flattened_subprogram_index = flattened_subprogram_index,
    };
    status = serialized_glcore_target_open(destination, &input);
    shader_blob_archive_close(&archive);
    return status;
}

const char* serialized_glcore_target_status_name(
    SerializedGLCoreTargetStatus status) {
    switch (status) {
        case SERIALIZED_GLCORE_TARGET_OK: return "ok";
        case SERIALIZED_GLCORE_TARGET_INVALID_ARGUMENT:
            return "invalid-argument";
        case SERIALIZED_GLCORE_TARGET_NOT_DECODED: return "not-decoded";
        case SERIALIZED_GLCORE_TARGET_UNSUPPORTED_UNITY_VERSION:
            return "unsupported-unity-version";
        case SERIALIZED_GLCORE_TARGET_WRONG_PLATFORM:
            return "wrong-platform";
        case SERIALIZED_GLCORE_TARGET_PLATFORM_ABSENT:
            return "glcore-platform-absent";
        case SERIALIZED_GLCORE_TARGET_PLATFORM_AMBIGUOUS:
            return "glcore-platform-ambiguous";
        case SERIALIZED_GLCORE_TARGET_PLATFORM_TABLE_INVALID:
            return "platform-table-invalid";
        case SERIALIZED_GLCORE_TARGET_ARCHIVE_INVALID:
            return "glcore-archive-invalid";
        case SERIALIZED_GLCORE_TARGET_ARCHIVE_STAGE_COUNT_INVALID:
            return "glcore-archive-stage-count-invalid";
        case SERIALIZED_GLCORE_TARGET_PASS_INVALID: return "pass-invalid";
        case SERIALIZED_GLCORE_TARGET_LINK_OWNER_INVALID:
            return "link-owner-invalid";
        case SERIALIZED_GLCORE_TARGET_PROGRAM_TYPE_UNSUPPORTED:
            return "glcore-program-type-unsupported";
        case SERIALIZED_GLCORE_TARGET_BLOB_INDEX_INVALID:
            return "blob-index-invalid";
        case SERIALIZED_GLCORE_TARGET_WRAPPER_DIALECT_UNSUPPORTED:
            return "wrapper-dialect-unsupported";
        case SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID:
            return "wrapper-invalid";
        case SERIALIZED_GLCORE_TARGET_WRAPPER_PROGRAM_TYPE_MISMATCH:
            return "wrapper-program-type-mismatch";
        case SERIALIZED_GLCORE_TARGET_WRAPPER_KEYWORDS_MISMATCH:
            return "wrapper-keywords-mismatch";
        case SERIALIZED_GLCORE_TARGET_LINK_TEXT_INVALID:
            return "linked-text-invalid";
        case SERIALIZED_GLCORE_TARGET_ALLOCATION_FAILED:
            return "allocation-failed";
        default: return "unknown";
    }
}
