// SPDX-License-Identifier: GPL-3.0-only

#include "io/shader_object.h"

#include "common/stream.h"

#include <limits.h>
#include <string.h>

enum {
    SHADER_OBJECT_CLASS_ID = 48,
    SHADER_OBJECT_D3D11_PLATFORM = 4,
};

typedef enum {
    SCHEMA_CLONE_OK = 0,
    SCHEMA_CLONE_INVALID,
    SCHEMA_CLONE_SIZE_OVERFLOW,
    SCHEMA_CLONE_ALLOCATION_FAILED,
} SchemaCloneStatus;

static bool source_file_is_structurally_valid(const SerializedFile* file) {
    if (!file || !file->raw_data || !file->unity_version ||
        file->file_size != (uint64_t)file->raw_size ||
        file->data_offset > file->file_size || file->type_count < 0 ||
        file->object_count < 0 ||
        (file->type_count > 0 && !file->types) ||
        (file->object_count > 0 && !file->objects)) {
        return false;
    }
    return true;
}

static bool source_file_owns_object(const SerializedFile* file,
                                    const AssetObjectInfo* object) {
    for (int index = 0; index < file->object_count; ++index) {
        if (&file->objects[index] == object) return true;
    }
    return false;
}

static bool object_range_is_valid(const SerializedFile* file,
                                  const AssetObjectInfo* object) {
    const uint64_t data_size = file->file_size - file->data_offset;
    return object->byte_offset <= data_size &&
           (uint64_t)object->byte_size <= data_size - object->byte_offset;
}

static SchemaCloneStatus clone_shader_schema(TypeTreeType* destination,
                                              const TypeTreeType* source) {
    if (!destination || !source || source->node_count <= 0 ||
        !source->nodes || source->dependency_count < 0 ||
        (source->string_buffer_size > 0U && !source->string_buffer) ||
        (source->dependency_count > 0 && !source->dependencies) ||
        source->is_ref_type || source->ref_class_name ||
        source->ref_namespace || source->ref_asm_name ||
        !typetree_validate_schema(source)) {
        return SCHEMA_CLONE_INVALID;
    }

    memset(destination, 0, sizeof(*destination));
    destination->type_id = source->type_id;
    destination->is_stripped = source->is_stripped;
    destination->script_type_index = source->script_type_index;
    memcpy(destination->script_id_hash, source->script_id_hash,
           sizeof(destination->script_id_hash));
    memcpy(destination->type_hash, source->type_hash,
           sizeof(destination->type_hash));
    destination->node_count = source->node_count;
    destination->string_buffer_size = source->string_buffer_size;
    destination->dependency_count = source->dependency_count;

    if (dxbc_size_multiply_overflows((size_t)source->node_count,
                                     sizeof(TypeTreeNode))) {
        typetree_free_type(destination);
        return SCHEMA_CLONE_SIZE_OVERFLOW;
    }
    const size_t node_bytes =
        (size_t)source->node_count * sizeof(TypeTreeNode);
    destination->nodes = (TypeTreeNode*)mem_alloc(node_bytes);
    if (!destination->nodes) {
        typetree_free_type(destination);
        return SCHEMA_CLONE_ALLOCATION_FAILED;
    }
    memcpy(destination->nodes, source->nodes, node_bytes);

    if (source->string_buffer_size > 0U) {
        destination->string_buffer =
            (uint8_t*)mem_alloc(source->string_buffer_size);
        if (!destination->string_buffer) {
            typetree_free_type(destination);
            return SCHEMA_CLONE_ALLOCATION_FAILED;
        }
        memcpy(destination->string_buffer, source->string_buffer,
               source->string_buffer_size);
    }
    if (!typetree_bind_node_strings(destination)) {
        typetree_free_type(destination);
        return SCHEMA_CLONE_INVALID;
    }
    for (int index = 0; index < source->node_count; ++index) {
        if (strcmp(destination->nodes[index].type_str,
                   source->nodes[index].type_str) != 0 ||
            strcmp(destination->nodes[index].name_str,
                   source->nodes[index].name_str) != 0) {
            typetree_free_type(destination);
            return SCHEMA_CLONE_INVALID;
        }
    }

    if (source->dependency_count > 0) {
        if (dxbc_size_multiply_overflows(
                (size_t)source->dependency_count, sizeof(int32_t))) {
            typetree_free_type(destination);
            return SCHEMA_CLONE_SIZE_OVERFLOW;
        }
        const size_t dependency_bytes =
            (size_t)source->dependency_count * sizeof(int32_t);
        destination->dependencies = (int32_t*)mem_alloc(dependency_bytes);
        if (!destination->dependencies) {
            typetree_free_type(destination);
            return SCHEMA_CLONE_ALLOCATION_FAILED;
        }
        memcpy(destination->dependencies, source->dependencies,
               dependency_bytes);
    }
    if (!typetree_validate_schema(destination)) {
        typetree_free_type(destination);
        return SCHEMA_CLONE_INVALID;
    }
    return SCHEMA_CLONE_OK;
}

static ShaderObjectStatus map_schema_clone_status(
    SchemaCloneStatus status) {
    switch (status) {
        case SCHEMA_CLONE_OK: return SHADER_OBJECT_OK;
        case SCHEMA_CLONE_INVALID: return SHADER_OBJECT_SCHEMA_INVALID;
        case SCHEMA_CLONE_SIZE_OVERFLOW:
            return SHADER_OBJECT_SCHEMA_SIZE_OVERFLOW;
        case SCHEMA_CLONE_ALLOCATION_FAILED:
            return SHADER_OBJECT_ALLOCATION_FAILED;
        default: return SHADER_OBJECT_SCHEMA_INVALID;
    }
}

void shader_object_init(ShaderObject* object) {
    if (!object) return;
    memset(object, 0, sizeof(*object));
    serialized_shader_init(&object->shader);
}

void shader_object_dispose(ShaderObject* object) {
    if (!object) return;
    serialized_shader_free(&object->shader);
    typetree_free_value(&object->root);
    typetree_free_type(&object->schema);
    shader_object_init(object);
}

static ShaderObjectStatus shader_object_decode_internal(
    ShaderObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object, uint32_t parse_flags) {
    if (!destination || !file || !object) {
        return SHADER_OBJECT_INVALID_ARGUMENT;
    }
    if (!source_file_is_structurally_valid(file)) {
        return SHADER_OBJECT_INVALID_SOURCE_FILE;
    }
    if (file->version != 22U) {
        return SHADER_OBJECT_UNSUPPORTED_FILE_VERSION;
    }
    if (!source_file_owns_object(file, object)) {
        return SHADER_OBJECT_OBJECT_NOT_OWNED;
    }
    if (!object_range_is_valid(file, object) || object->byte_size == 0U) {
        return SHADER_OBJECT_OBJECT_RANGE_INVALID;
    }
    if (object->type_id != SHADER_OBJECT_CLASS_ID) {
        return SHADER_OBJECT_NOT_SHADER;
    }
    if (object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        return SHADER_OBJECT_TYPE_INDEX_INVALID;
    }

    const TypeTreeType* source_schema =
        &file->types[object->type_id_or_index];
    if (source_schema->type_id != SHADER_OBJECT_CLASS_ID ||
        source_schema->script_type_index != object->script_type_index ||
        source_schema->is_ref_type) {
        return SHADER_OBJECT_TYPE_RECORD_MISMATCH;
    }
    if (source_schema->node_count == 0 && !source_schema->nodes) {
        return SHADER_OBJECT_SCHEMA_UNRESOLVED;
    }
    if (source_schema->node_count <= 0 || !source_schema->nodes ||
        !typetree_validate_schema(source_schema)) {
        return SHADER_OBJECT_SCHEMA_INVALID;
    }

    SerializedShaderSchemaProfile profile;
    if (!serialized_shader_profile_from_unity_version(
            file->unity_version, &profile)) {
        return SHADER_OBJECT_UNSUPPORTED_UNITY_VERSION;
    }

    ShaderObject candidate;
    shader_object_init(&candidate);
    ShaderObjectStatus status = map_schema_clone_status(
        clone_shader_schema(&candidate.schema, source_schema));
    if (status != SHADER_OBJECT_OK) goto fail;

    const uint64_t absolute_offset =
        file->data_offset + object->byte_offset;
    const uint8_t* object_data =
        file->raw_data + (size_t)absolute_offset;
    ByteStream stream;
    stream_init(&stream, object_data, object->byte_size);
    stream_set_endian(&stream, file->big_endian);
    int node_index = 0;
    if (!typetree_parse_value_ex(
            &candidate.schema, &node_index, &stream, &candidate.root,
            parse_flags)) {
        status = SHADER_OBJECT_TYPETREE_PARSE_FAILED;
        goto fail;
    }
    if (node_index != candidate.schema.node_count) {
        status = SHADER_OBJECT_TYPETREE_NODES_NOT_EXHAUSTED;
        goto fail;
    }
    if (stream.position != (size_t)object->byte_size) {
        status = SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED;
        goto fail;
    }
    if (!serialized_shader_parse_with_profile(
            &candidate.shader, &candidate.root, profile)) {
        status = SHADER_OBJECT_SHADER_MODEL_INVALID;
        goto fail;
    }

    candidate.profile = profile;
    candidate.path_id = object->path_id;
    candidate.byte_offset = object->byte_offset;
    candidate.byte_size = object->byte_size;
    candidate.source_type_index = object->type_id_or_index;
    candidate.serialized_file_version = file->version;
    candidate.target_platform = file->target_platform;
    candidate.decoded = true;

    shader_object_dispose(destination);
    *destination = candidate;
    return SHADER_OBJECT_OK;

fail:
    shader_object_dispose(&candidate);
    return status;
}

ShaderObjectStatus shader_object_decode(
    ShaderObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object) {
    return shader_object_decode_internal(
        destination, file, object, TYPETREE_PARSE_PACK_COMPRESSED_BLOB);
}

ShaderObjectStatus shader_object_decode_borrowed(
    ShaderObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object) {
    return shader_object_decode_internal(
        destination, file, object, TYPETREE_PARSE_BORROW_BYTE_ARRAYS);
}

ShaderObjectStatus shader_object_d3d11_platform_status(
    const ShaderObject* object) {
    if (!object) return SHADER_OBJECT_INVALID_ARGUMENT;
    if (!object->decoded) return SHADER_OBJECT_NOT_DECODED;
    const TypeTreeValue* platforms =
        typetree_find_child(&object->root, "platforms");
    const TypeTreeValue* array = typetree_get_array(platforms);
    if (!array || array->type != VAL_TYPE_ARRAY ||
        array->array_val.count < 0 ||
        array->array_val.storage != TYPETREE_ARRAY_VALUES ||
        (array->array_val.count > 0 && !array->array_val.elements)) {
        return SHADER_OBJECT_PLATFORM_TABLE_INVALID;
    }

    int d3d11_count = 0;
    for (int index = 0; index < array->array_val.count; ++index) {
        int64_t value = 0;
        if (!typetree_array_get_int(array, index, &value) ||
            value < INT_MIN || value > INT_MAX) {
            return SHADER_OBJECT_PLATFORM_TABLE_INVALID;
        }
        for (int previous = 0; previous < index; ++previous) {
            int64_t previous_value = 0;
            if (!typetree_array_get_int(
                    array, previous, &previous_value)) {
                return SHADER_OBJECT_PLATFORM_TABLE_INVALID;
            }
            if (previous_value == value) {
                return value == SHADER_OBJECT_D3D11_PLATFORM
                    ? SHADER_OBJECT_D3D11_PLATFORM_AMBIGUOUS
                    : SHADER_OBJECT_PLATFORM_TABLE_INVALID;
            }
        }
        if (value == SHADER_OBJECT_D3D11_PLATFORM) ++d3d11_count;
    }
    if (d3d11_count == 0) {
        return SHADER_OBJECT_D3D11_PLATFORM_ABSENT;
    }
    if (d3d11_count != 1) {
        return SHADER_OBJECT_D3D11_PLATFORM_AMBIGUOUS;
    }
    return SHADER_OBJECT_OK;
}

ShaderObjectStatus shader_object_open_d3d11_archive(
    const ShaderObject* object, ShaderBlobArchive* destination) {
    if (!object || !destination) return SHADER_OBJECT_INVALID_ARGUMENT;
    if (!object->decoded) return SHADER_OBJECT_NOT_DECODED;

    ShaderObjectStatus status =
        shader_object_d3d11_platform_status(object);
    if (status != SHADER_OBJECT_OK) return status;

    ShaderBlobArchive candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (!shader_blob_archive_open(
            &object->root, SHADER_OBJECT_D3D11_PLATFORM, &candidate)) {
        shader_blob_archive_close(&candidate);
        return SHADER_OBJECT_D3D11_ARCHIVE_INVALID;
    }
    shader_blob_archive_close(destination);
    *destination = candidate;
    return SHADER_OBJECT_OK;
}

const char* shader_object_status_name(ShaderObjectStatus status) {
    switch (status) {
        case SHADER_OBJECT_OK: return "ok";
        case SHADER_OBJECT_INVALID_ARGUMENT: return "invalid-argument";
        case SHADER_OBJECT_INVALID_SOURCE_FILE:
            return "invalid-source-file";
        case SHADER_OBJECT_UNSUPPORTED_FILE_VERSION:
            return "unsupported-file-version";
        case SHADER_OBJECT_OBJECT_NOT_OWNED: return "object-not-owned";
        case SHADER_OBJECT_OBJECT_RANGE_INVALID:
            return "object-range-invalid";
        case SHADER_OBJECT_NOT_SHADER: return "not-shader";
        case SHADER_OBJECT_TYPE_INDEX_INVALID: return "type-index-invalid";
        case SHADER_OBJECT_TYPE_RECORD_MISMATCH:
            return "type-record-mismatch";
        case SHADER_OBJECT_SCHEMA_UNRESOLVED: return "schema-unresolved";
        case SHADER_OBJECT_SCHEMA_INVALID: return "schema-invalid";
        case SHADER_OBJECT_SCHEMA_SIZE_OVERFLOW:
            return "schema-size-overflow";
        case SHADER_OBJECT_ALLOCATION_FAILED: return "allocation-failed";
        case SHADER_OBJECT_UNSUPPORTED_UNITY_VERSION:
            return "unsupported-unity-version";
        case SHADER_OBJECT_TYPETREE_PARSE_FAILED:
            return "typetree-parse-failed";
        case SHADER_OBJECT_TYPETREE_NODES_NOT_EXHAUSTED:
            return "typetree-nodes-not-exhausted";
        case SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED:
            return "object-bytes-not-exhausted";
        case SHADER_OBJECT_SHADER_MODEL_INVALID:
            return "shader-model-invalid";
        case SHADER_OBJECT_NOT_DECODED: return "not-decoded";
        case SHADER_OBJECT_D3D11_PLATFORM_ABSENT:
            return "d3d11-platform-absent";
        case SHADER_OBJECT_D3D11_PLATFORM_AMBIGUOUS:
            return "d3d11-platform-ambiguous";
        case SHADER_OBJECT_PLATFORM_TABLE_INVALID:
            return "platform-table-invalid";
        case SHADER_OBJECT_D3D11_ARCHIVE_INVALID:
            return "d3d11-archive-invalid";
        default: return "unknown";
    }
}
