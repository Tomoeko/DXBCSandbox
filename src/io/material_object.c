// SPDX-License-Identifier: GPL-3.0-only

#include "io/material_object.h"

#include "common/stream.h"
#include "io/typetree_schema_profile.h"

#include <limits.h>
#include <string.h>

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "Material binary32 projection requires 32-bit float");

enum {
    MATERIAL_CLASS_ID = 21,
    MATERIAL_SERIALIZED_FILE_VERSION = 22,
};

static const uint8_t k_material_type_hash[16] = {
    0xc6U, 0x00U, 0x98U, 0xacU, 0x66U, 0xa2U, 0x8bU, 0x50U,
    0xaaU, 0x05U, 0x80U, 0xdbU, 0x11U, 0xbfU, 0x01U, 0x8cU,
};

typedef enum {
    SCHEMA_CLONE_OK = 0,
    SCHEMA_CLONE_INVALID,
    SCHEMA_CLONE_SIZE_OVERFLOW,
    SCHEMA_CLONE_ALLOCATION_FAILED,
} SchemaCloneStatus;

typedef struct {
    ByteStream stream;
    MaterialObjectStatus status;
} MaterialWireReader;

static bool source_file_is_structurally_valid(const SerializedFile* file) {
    return file && file->raw_data && file->unity_version &&
        file->file_size == (uint64_t)file->raw_size &&
        file->data_offset <= file->file_size && file->type_count >= 0 &&
        file->object_count >= 0 &&
        (file->type_count == 0 || file->types) &&
        (file->object_count == 0 || file->objects);
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

static bool bytes_are_zero(const uint8_t* bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) return false;
    }
    return true;
}

static SchemaCloneStatus clone_material_schema(TypeTreeType* destination,
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
    size_t node_bytes = (size_t)source->node_count * sizeof(TypeTreeNode);
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
        size_t dependency_bytes =
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

static MaterialObjectStatus map_schema_clone_status(
    SchemaCloneStatus status) {
    switch (status) {
        case SCHEMA_CLONE_OK: return MATERIAL_OBJECT_OK;
        case SCHEMA_CLONE_INVALID: return MATERIAL_OBJECT_SCHEMA_INVALID;
        case SCHEMA_CLONE_SIZE_OVERFLOW:
            return MATERIAL_OBJECT_SCHEMA_SIZE_OVERFLOW;
        case SCHEMA_CLONE_ALLOCATION_FAILED:
            return MATERIAL_OBJECT_ALLOCATION_FAILED;
        default: return MATERIAL_OBJECT_SCHEMA_INVALID;
    }
}

static bool valid_utf8(const uint8_t* bytes, size_t size) {
    if (!bytes && size != 0U) return false;
    size_t at = 0U;
    while (at < size) {
        uint8_t first = bytes[at++];
        if (first == 0U) return false;
        if (first < 0x80U) continue;

        size_t continuation;
        uint32_t codepoint;
        uint32_t minimum;
        if ((first & 0xe0U) == 0xc0U) {
            continuation = 1U;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation = 2U;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation = 3U;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation > size - at) return false;
        for (size_t index = 0U; index < continuation; ++index) {
            uint8_t next = bytes[at++];
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

static bool value_has_shape(const TypeTreeValue* value, ValueType type,
                            const char* wire_type, const char* name) {
    return value && value->type == type && value->type_str && value->name &&
        strcmp(value->type_str, wire_type) == 0 &&
        strcmp(value->name, name) == 0;
}

static bool struct_has_shape(const TypeTreeValue* value,
                             const char* wire_type, const char* name,
                             int count) {
    return value_has_shape(value, VAL_TYPE_STRUCT, wire_type, name) &&
        value->struct_val.count == count &&
        (count == 0 || value->struct_val.members);
}

static bool array_has_shape(const TypeTreeValue* value,
                            const char* wire_type, const char* name) {
    return value_has_shape(value, VAL_TYPE_ARRAY, wire_type, name) &&
        value->array_val.count >= 0 &&
        value->array_val.storage == TYPETREE_ARRAY_VALUES &&
        (value->array_val.count == 0 || value->array_val.elements);
}

static MaterialObjectStatus string_view_from_value(
    const TypeTreeValue* value, const char* name, MaterialStringView* view) {
    if (!view || !value_has_shape(value, VAL_TYPE_STRING, "string", name) ||
        (!value->string_val && value->string_length != 0U) ||
        !valid_utf8((const uint8_t*)value->string_val,
                    value->string_length)) {
        return MATERIAL_OBJECT_STRING_INVALID;
    }
    view->bytes = (const uint8_t*)value->string_val;
    view->size = value->string_length;
    return MATERIAL_OBJECT_OK;
}

static bool string_views_equal(MaterialStringView left,
                               MaterialStringView right) {
    return left.size == right.size &&
        (left.size == 0U || memcmp(left.bytes, right.bytes, left.size) == 0);
}

static bool string_key_is_duplicate(const MaterialStringView* keys,
                                    size_t count,
                                    MaterialStringView candidate) {
    for (size_t index = 0U; index < count; ++index) {
        if (string_views_equal(keys[index], candidate)) return true;
    }
    return false;
}

static MaterialObjectStatus allocate_array(size_t count, size_t element_size,
                                           void** allocation) {
    *allocation = NULL;
    if (count == 0U) return MATERIAL_OBJECT_OK;
    if (element_size == 0U ||
        dxbc_size_multiply_overflows(count, element_size)) {
        return MATERIAL_OBJECT_COUNT_INVALID;
    }
    size_t bytes = count * element_size;
    void* result = mem_alloc(bytes);
    if (!result) return MATERIAL_OBJECT_ALLOCATION_FAILED;
    memset(result, 0, bytes);
    *allocation = result;
    return MATERIAL_OBJECT_OK;
}

static MaterialObjectStatus project_string_array(
    const TypeTreeValue* value, const char* wire_type, const char* name,
    MaterialStringView** strings, size_t* count) {
    *strings = NULL;
    *count = 0U;
    if (!array_has_shape(value, wire_type, name)) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    size_t result_count = (size_t)value->array_val.count;
    MaterialObjectStatus status = allocate_array(
        result_count, sizeof(MaterialStringView), (void**)strings);
    if (status != MATERIAL_OBJECT_OK) return status;
    *count = result_count;
    for (size_t index = 0U; index < result_count; ++index) {
        status = string_view_from_value(&value->array_val.elements[index],
                                        "data", &(*strings)[index]);
        if (status != MATERIAL_OBJECT_OK) return status;
    }
    return MATERIAL_OBJECT_OK;
}

static MaterialObjectStatus project_pptr(const TypeTreeValue* value,
                                         const char* wire_type,
                                         const char* name, AssetPPtr* pptr) {
    if (!struct_has_shape(value, wire_type, name, 2)) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    const TypeTreeValue* file_id = &value->struct_val.members[0];
    const TypeTreeValue* path_id = &value->struct_val.members[1];
    int64_t file_value = 0;
    int64_t path_value = 0;
    if (!value_has_shape(file_id, VAL_TYPE_INT, "int", "m_FileID") ||
        file_id->integer_is_unsigned ||
        !value_has_shape(path_id, VAL_TYPE_INT, "SInt64", "m_PathID") ||
        path_id->integer_is_unsigned ||
        !typetree_value_get_int(file_id, &file_value) ||
        !typetree_value_get_int(path_id, &path_value) ||
        file_value < INT32_MIN || file_value > INT32_MAX) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    pptr->file_id = (int32_t)file_value;
    pptr->path_id = path_value;
    return MATERIAL_OBJECT_OK;
}

static MaterialObjectStatus project_i32(const TypeTreeValue* value,
                                        const char* wire_type,
                                        const char* name, int32_t* result) {
    int64_t parsed = 0;
    if (!value_has_shape(value, VAL_TYPE_INT, wire_type, name) ||
        value->integer_is_unsigned ||
        !typetree_value_get_int(value, &parsed) ||
        parsed < INT32_MIN || parsed > INT32_MAX) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    *result = (int32_t)parsed;
    return MATERIAL_OBJECT_OK;
}

static MaterialObjectStatus project_u32(const TypeTreeValue* value,
                                        const char* wire_type,
                                        const char* name, uint32_t* result) {
    uint64_t parsed = 0U;
    if (!value_has_shape(value, VAL_TYPE_INT, wire_type, name) ||
        !value->integer_is_unsigned ||
        !typetree_value_get_uint(value, &parsed) || parsed > UINT32_MAX) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    *result = (uint32_t)parsed;
    return MATERIAL_OBJECT_OK;
}

static MaterialObjectStatus project_bool(const TypeTreeValue* value,
                                         const char* name, bool* result) {
    uint64_t parsed = 0U;
    if (!value_has_shape(value, VAL_TYPE_INT, "bool", name) ||
        !value->integer_is_unsigned ||
        !typetree_value_get_uint(value, &parsed) || parsed > 1U) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    *result = parsed != 0U;
    return MATERIAL_OBJECT_OK;
}

static MaterialObjectStatus project_string_tags(MaterialObject* object,
                                                const TypeTreeValue* value) {
    if (!array_has_shape(value, "map", "stringTagMap")) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    size_t count = (size_t)value->array_val.count;
    MaterialObjectStatus status = allocate_array(
        count, sizeof(MaterialStringTag), (void**)&object->string_tags);
    if (status != MATERIAL_OBJECT_OK) return status;
    object->string_tag_count = count;
    MaterialStringView* prior_keys = NULL;
    status = allocate_array(count, sizeof(MaterialStringView),
                            (void**)&prior_keys);
    if (status != MATERIAL_OBJECT_OK) return status;

    for (size_t index = 0U; index < count; ++index) {
        const TypeTreeValue* pair = &value->array_val.elements[index];
        if (!struct_has_shape(pair, "pair", "data", 2)) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
        status = string_view_from_value(&pair->struct_val.members[0], "first",
                                        &object->string_tags[index].key);
        if (status != MATERIAL_OBJECT_OK) goto done;
        status = string_view_from_value(&pair->struct_val.members[1], "second",
                                        &object->string_tags[index].value);
        if (status != MATERIAL_OBJECT_OK) goto done;
        if (string_key_is_duplicate(prior_keys, index,
                                    object->string_tags[index].key)) {
            status = MATERIAL_OBJECT_DUPLICATE_KEY;
            goto done;
        }
        prior_keys[index] = object->string_tags[index].key;
    }
done:
    if (prior_keys) mem_free(prior_keys, count * sizeof(*prior_keys));
    return status;
}

static MaterialObjectStatus project_texture_properties(
    MaterialObject* object, const TypeTreeValue* value) {
    if (!array_has_shape(value, "map", "m_TexEnvs")) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    size_t count = (size_t)value->array_val.count;
    MaterialObjectStatus status = allocate_array(
        count, sizeof(MaterialTextureProperty),
        (void**)&object->texture_properties);
    if (status != MATERIAL_OBJECT_OK) return status;
    object->texture_property_count = count;
    MaterialStringView* keys = NULL;
    status = allocate_array(count, sizeof(MaterialStringView), (void**)&keys);
    if (status != MATERIAL_OBJECT_OK) return status;

    for (size_t index = 0U; index < count; ++index) {
        const TypeTreeValue* pair = &value->array_val.elements[index];
        if (!struct_has_shape(pair, "pair", "data", 2)) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
        MaterialTextureProperty* property =
            &object->texture_properties[index];
        status = string_view_from_value(&pair->struct_val.members[0], "first",
                                        &property->name);
        if (status != MATERIAL_OBJECT_OK) goto done;
        if (string_key_is_duplicate(keys, index, property->name)) {
            status = MATERIAL_OBJECT_DUPLICATE_KEY;
            goto done;
        }
        keys[index] = property->name;

        const TypeTreeValue* tex_env = &pair->struct_val.members[1];
        if (!struct_has_shape(tex_env, "UnityTexEnv", "second", 3) ||
            !struct_has_shape(&tex_env->struct_val.members[1], "Vector2f",
                              "m_Scale", 2) ||
            !struct_has_shape(&tex_env->struct_val.members[2], "Vector2f",
                              "m_Offset", 2)) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
        status = project_pptr(&tex_env->struct_val.members[0],
                              "PPtr<Texture>", "m_Texture",
                              &property->texture);
        if (status != MATERIAL_OBJECT_OK) goto done;
        const TypeTreeValue* scale = &tex_env->struct_val.members[1];
        const TypeTreeValue* offset = &tex_env->struct_val.members[2];
        if (!value_has_shape(&scale->struct_val.members[0], VAL_TYPE_FLOAT,
                             "float", "x") ||
            !value_has_shape(&scale->struct_val.members[1], VAL_TYPE_FLOAT,
                             "float", "y") ||
            !value_has_shape(&offset->struct_val.members[0], VAL_TYPE_FLOAT,
                             "float", "x") ||
            !value_has_shape(&offset->struct_val.members[1], VAL_TYPE_FLOAT,
                             "float", "y")) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
    }
done:
    if (keys) mem_free(keys, count * sizeof(*keys));
    return status;
}

static MaterialObjectStatus project_int_properties(
    MaterialObject* object, const TypeTreeValue* value) {
    if (!array_has_shape(value, "map", "m_Ints")) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    size_t count = (size_t)value->array_val.count;
    MaterialObjectStatus status = allocate_array(
        count, sizeof(MaterialIntProperty),
        (void**)&object->int_properties);
    if (status != MATERIAL_OBJECT_OK) return status;
    object->int_property_count = count;
    MaterialStringView* keys = NULL;
    status = allocate_array(count, sizeof(MaterialStringView), (void**)&keys);
    if (status != MATERIAL_OBJECT_OK) return status;
    for (size_t index = 0U; index < count; ++index) {
        const TypeTreeValue* pair = &value->array_val.elements[index];
        if (!struct_has_shape(pair, "pair", "data", 2)) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
        MaterialIntProperty* property = &object->int_properties[index];
        status = string_view_from_value(&pair->struct_val.members[0], "first",
                                        &property->name);
        if (status != MATERIAL_OBJECT_OK) goto done;
        if (string_key_is_duplicate(keys, index, property->name)) {
            status = MATERIAL_OBJECT_DUPLICATE_KEY;
            goto done;
        }
        keys[index] = property->name;
        status = project_i32(&pair->struct_val.members[1], "int", "second",
                             &property->value);
        if (status != MATERIAL_OBJECT_OK) goto done;
    }
done:
    if (keys) mem_free(keys, count * sizeof(*keys));
    return status;
}

static MaterialObjectStatus project_float_properties(
    MaterialObject* object, const TypeTreeValue* value) {
    if (!array_has_shape(value, "map", "m_Floats")) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    size_t count = (size_t)value->array_val.count;
    MaterialObjectStatus status = allocate_array(
        count, sizeof(MaterialFloatProperty),
        (void**)&object->float_properties);
    if (status != MATERIAL_OBJECT_OK) return status;
    object->float_property_count = count;
    MaterialStringView* keys = NULL;
    status = allocate_array(count, sizeof(MaterialStringView), (void**)&keys);
    if (status != MATERIAL_OBJECT_OK) return status;
    for (size_t index = 0U; index < count; ++index) {
        const TypeTreeValue* pair = &value->array_val.elements[index];
        if (!struct_has_shape(pair, "pair", "data", 2)) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
        MaterialFloatProperty* property = &object->float_properties[index];
        status = string_view_from_value(&pair->struct_val.members[0], "first",
                                        &property->name);
        if (status != MATERIAL_OBJECT_OK) goto done;
        if (string_key_is_duplicate(keys, index, property->name)) {
            status = MATERIAL_OBJECT_DUPLICATE_KEY;
            goto done;
        }
        keys[index] = property->name;
        if (!value_has_shape(&pair->struct_val.members[1], VAL_TYPE_FLOAT,
                             "float", "second")) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
    }
done:
    if (keys) mem_free(keys, count * sizeof(*keys));
    return status;
}

static MaterialObjectStatus project_color_properties(
    MaterialObject* object, const TypeTreeValue* value) {
    if (!array_has_shape(value, "map", "m_Colors")) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    size_t count = (size_t)value->array_val.count;
    MaterialObjectStatus status = allocate_array(
        count, sizeof(MaterialColorProperty),
        (void**)&object->color_properties);
    if (status != MATERIAL_OBJECT_OK) return status;
    object->color_property_count = count;
    MaterialStringView* keys = NULL;
    status = allocate_array(count, sizeof(MaterialStringView), (void**)&keys);
    if (status != MATERIAL_OBJECT_OK) return status;
    static const char* const component_names[] = {"r", "g", "b", "a"};
    for (size_t index = 0U; index < count; ++index) {
        const TypeTreeValue* pair = &value->array_val.elements[index];
        if (!struct_has_shape(pair, "pair", "data", 2)) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
        MaterialColorProperty* property = &object->color_properties[index];
        status = string_view_from_value(&pair->struct_val.members[0], "first",
                                        &property->name);
        if (status != MATERIAL_OBJECT_OK) goto done;
        if (string_key_is_duplicate(keys, index, property->name)) {
            status = MATERIAL_OBJECT_DUPLICATE_KEY;
            goto done;
        }
        keys[index] = property->name;
        const TypeTreeValue* color = &pair->struct_val.members[1];
        if (!struct_has_shape(color, "ColorRGBA", "second", 4)) {
            status = MATERIAL_OBJECT_MODEL_INVALID;
            goto done;
        }
        for (int component = 0; component < 4; ++component) {
            if (!value_has_shape(&color->struct_val.members[component],
                                 VAL_TYPE_FLOAT, "float",
                                 component_names[component])) {
                status = MATERIAL_OBJECT_MODEL_INVALID;
                goto done;
            }
        }
    }
done:
    if (keys) mem_free(keys, count * sizeof(*keys));
    return status;
}

static MaterialObjectStatus project_texture_stacks(
    MaterialObject* object, const TypeTreeValue* value) {
    if (!array_has_shape(value, "vector", "m_BuildTextureStacks")) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    size_t count = (size_t)value->array_val.count;
    MaterialObjectStatus status = allocate_array(
        count, sizeof(MaterialTextureStackReference),
        (void**)&object->texture_stacks);
    if (status != MATERIAL_OBJECT_OK) return status;
    object->texture_stack_count = count;
    for (size_t index = 0U; index < count; ++index) {
        const TypeTreeValue* stack = &value->array_val.elements[index];
        if (!struct_has_shape(stack, "BuildTextureStackReference", "data",
                              2)) {
            return MATERIAL_OBJECT_MODEL_INVALID;
        }
        status = string_view_from_value(&stack->struct_val.members[0],
                                        "groupName",
                                        &object->texture_stacks[index]
                                             .group_name);
        if (status != MATERIAL_OBJECT_OK) return status;
        status = string_view_from_value(&stack->struct_val.members[1],
                                        "itemName",
                                        &object->texture_stacks[index]
                                             .item_name);
        if (status != MATERIAL_OBJECT_OK) return status;
    }
    return MATERIAL_OBJECT_OK;
}

static MaterialObjectStatus project_material_model(MaterialObject* object) {
    if (!object || !struct_has_shape(&object->root, "Material", "Base", 12)) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    const TypeTreeValue* fields = object->root.struct_val.members;
    MaterialObjectStatus status = string_view_from_value(
        &fields[0], "m_Name", &object->name);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_pptr(&fields[1], "PPtr<Shader>", "m_Shader",
                          &object->shader);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_string_array(&fields[2], "vector", "m_ValidKeywords",
                                  &object->valid_keywords,
                                  &object->valid_keyword_count);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_string_array(&fields[3], "vector", "m_InvalidKeywords",
                                  &object->invalid_keywords,
                                  &object->invalid_keyword_count);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_u32(&fields[4], "unsigned int", "m_LightmapFlags",
                         &object->lightmap_flags);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_bool(&fields[5], "m_EnableInstancingVariants",
                          &object->enable_instancing_variants);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_bool(&fields[6], "m_DoubleSidedGI",
                          &object->double_sided_gi);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_i32(&fields[7], "int", "m_CustomRenderQueue",
                         &object->custom_render_queue);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_string_tags(object, &fields[8]);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_string_array(
        &fields[9], "vector", "disabledShaderPasses",
        &object->disabled_shader_passes,
        &object->disabled_shader_pass_count);
    if (status != MATERIAL_OBJECT_OK) return status;

    if (!struct_has_shape(&fields[10], "UnityPropertySheet",
                          "m_SavedProperties", 4)) {
        return MATERIAL_OBJECT_MODEL_INVALID;
    }
    const TypeTreeValue* saved = fields[10].struct_val.members;
    status = project_texture_properties(object, &saved[0]);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_int_properties(object, &saved[1]);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_float_properties(object, &saved[2]);
    if (status != MATERIAL_OBJECT_OK) return status;
    status = project_color_properties(object, &saved[3]);
    if (status != MATERIAL_OBJECT_OK) return status;
    return project_texture_stacks(object, &fields[11]);
}

static void wire_fail(MaterialWireReader* reader,
                      MaterialObjectStatus status) {
    if (reader->status == MATERIAL_OBJECT_OK) reader->status = status;
}

static bool wire_u8(MaterialWireReader* reader, uint8_t* value) {
    if (reader->status != MATERIAL_OBJECT_OK) return false;
    if (!stream_read_uint8(&reader->stream, value)) {
        wire_fail(reader, MATERIAL_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool wire_u32(MaterialWireReader* reader, uint32_t* value) {
    if (reader->status != MATERIAL_OBJECT_OK) return false;
    if (!stream_read_uint32(&reader->stream, value)) {
        wire_fail(reader, MATERIAL_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool wire_i32(MaterialWireReader* reader, int32_t* value) {
    if (reader->status != MATERIAL_OBJECT_OK) return false;
    if (!stream_read_int32(&reader->stream, value)) {
        wire_fail(reader, MATERIAL_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool wire_i64(MaterialWireReader* reader, int64_t* value) {
    if (reader->status != MATERIAL_OBJECT_OK) return false;
    if (!stream_read_int64(&reader->stream, value)) {
        wire_fail(reader, MATERIAL_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool wire_align4(MaterialWireReader* reader) {
    if (reader->status != MATERIAL_OBJECT_OK) return false;
    if (!stream_align(&reader->stream, 4U)) {
        wire_fail(reader, MATERIAL_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    return true;
}

static bool wire_string(MaterialWireReader* reader,
                        MaterialStringView expected) {
    uint32_t size = 0U;
    if (!wire_u32(reader, &size)) return false;
    if ((size_t)size > stream_remaining(&reader->stream)) {
        wire_fail(reader, MATERIAL_OBJECT_PAYLOAD_TRUNCATED);
        return false;
    }
    const uint8_t* bytes = reader->stream.data + reader->stream.position;
    if ((size_t)size != expected.size ||
        (size > 0U && memcmp(bytes, expected.bytes, size) != 0) ||
        !stream_skip(&reader->stream, size) || !wire_align4(reader)) {
        wire_fail(reader, MATERIAL_OBJECT_MODEL_INVALID);
        return false;
    }
    return true;
}

static bool wire_count(MaterialWireReader* reader, size_t expected) {
    uint32_t count = 0U;
    if (!wire_u32(reader, &count)) return false;
    if (count > INT_MAX || (size_t)count != expected) {
        wire_fail(reader, MATERIAL_OBJECT_COUNT_INVALID);
        return false;
    }
    return true;
}

static bool wire_pptr(MaterialWireReader* reader, AssetPPtr expected) {
    int32_t file_id = 0;
    int64_t path_id = 0;
    if (!wire_i32(reader, &file_id) || !wire_i64(reader, &path_id)) {
        return false;
    }
    if (file_id != expected.file_id || path_id != expected.path_id) {
        wire_fail(reader, MATERIAL_OBJECT_MODEL_INVALID);
        return false;
    }
    return true;
}

static MaterialObjectStatus scan_material_wire(MaterialObject* object,
                                               const uint8_t* bytes,
                                               size_t size,
                                               bool big_endian) {
    MaterialWireReader reader;
    stream_init(&reader.stream, bytes, size);
    stream_set_endian(&reader.stream, big_endian);
    reader.status = MATERIAL_OBJECT_OK;

    if (!wire_string(&reader, object->name) ||
        !wire_pptr(&reader, object->shader) ||
        !wire_count(&reader, object->valid_keyword_count)) goto done;
    for (size_t index = 0U; index < object->valid_keyword_count; ++index) {
        if (!wire_string(&reader, object->valid_keywords[index])) goto done;
    }
    if (!wire_count(&reader, object->invalid_keyword_count)) goto done;
    for (size_t index = 0U; index < object->invalid_keyword_count; ++index) {
        if (!wire_string(&reader, object->invalid_keywords[index])) goto done;
    }

    uint32_t u32 = 0U;
    uint8_t u8 = 0U;
    int32_t i32 = 0;
    if (!wire_u32(&reader, &u32) || u32 != object->lightmap_flags ||
        !wire_u8(&reader, &u8) ||
        u8 != (object->enable_instancing_variants ? 1U : 0U) ||
        !wire_u8(&reader, &u8) || u8 != (object->double_sided_gi ? 1U : 0U) ||
        !wire_align4(&reader) || !wire_i32(&reader, &i32) ||
        i32 != object->custom_render_queue) {
        wire_fail(&reader, MATERIAL_OBJECT_MODEL_INVALID);
        goto done;
    }

    if (!wire_count(&reader, object->string_tag_count)) goto done;
    for (size_t index = 0U; index < object->string_tag_count; ++index) {
        if (!wire_string(&reader, object->string_tags[index].key) ||
            !wire_string(&reader, object->string_tags[index].value)) {
            goto done;
        }
    }
    if (!wire_count(&reader, object->disabled_shader_pass_count)) goto done;
    for (size_t index = 0U; index < object->disabled_shader_pass_count;
         ++index) {
        if (!wire_string(&reader, object->disabled_shader_passes[index])) {
            goto done;
        }
    }

    if (!wire_count(&reader, object->texture_property_count)) goto done;
    for (size_t index = 0U; index < object->texture_property_count; ++index) {
        MaterialTextureProperty* property =
            &object->texture_properties[index];
        if (!wire_string(&reader, property->name) ||
            !wire_pptr(&reader, property->texture) ||
            !wire_u32(&reader, &property->scale.x.bits) ||
            !wire_u32(&reader, &property->scale.y.bits) ||
            !wire_u32(&reader, &property->offset.x.bits) ||
            !wire_u32(&reader, &property->offset.y.bits)) {
            goto done;
        }
    }
    if (!wire_count(&reader, object->int_property_count)) goto done;
    for (size_t index = 0U; index < object->int_property_count; ++index) {
        MaterialIntProperty* property = &object->int_properties[index];
        if (!wire_string(&reader, property->name) ||
            !wire_i32(&reader, &i32)) goto done;
        if (i32 != property->value) {
            wire_fail(&reader, MATERIAL_OBJECT_MODEL_INVALID);
            goto done;
        }
    }
    if (!wire_count(&reader, object->float_property_count)) goto done;
    for (size_t index = 0U; index < object->float_property_count; ++index) {
        MaterialFloatProperty* property = &object->float_properties[index];
        if (!wire_string(&reader, property->name) ||
            !wire_u32(&reader, &property->value.bits)) goto done;
    }
    if (!wire_count(&reader, object->color_property_count)) goto done;
    for (size_t index = 0U; index < object->color_property_count; ++index) {
        MaterialColorProperty* property = &object->color_properties[index];
        if (!wire_string(&reader, property->name) ||
            !wire_u32(&reader, &property->value.r.bits) ||
            !wire_u32(&reader, &property->value.g.bits) ||
            !wire_u32(&reader, &property->value.b.bits) ||
            !wire_u32(&reader, &property->value.a.bits)) goto done;
    }

    if (!wire_count(&reader, object->texture_stack_count)) goto done;
    for (size_t index = 0U; index < object->texture_stack_count; ++index) {
        if (!wire_string(&reader, object->texture_stacks[index].group_name) ||
            !wire_string(&reader, object->texture_stacks[index].item_name)) {
            goto done;
        }
    }
done:
    if (reader.status == MATERIAL_OBJECT_OK &&
        reader.stream.position != size) {
        reader.status = MATERIAL_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED;
    }
    return reader.status;
}

void material_object_init(MaterialObject* object) {
    if (object) memset(object, 0, sizeof(*object));
}

void material_object_dispose(MaterialObject* object) {
    if (!object) return;
    if (object->valid_keywords) {
        mem_free(object->valid_keywords,
                 object->valid_keyword_count * sizeof(*object->valid_keywords));
    }
    if (object->invalid_keywords) {
        mem_free(object->invalid_keywords,
                 object->invalid_keyword_count *
                     sizeof(*object->invalid_keywords));
    }
    if (object->string_tags) {
        mem_free(object->string_tags,
                 object->string_tag_count * sizeof(*object->string_tags));
    }
    if (object->disabled_shader_passes) {
        mem_free(object->disabled_shader_passes,
                 object->disabled_shader_pass_count *
                     sizeof(*object->disabled_shader_passes));
    }
    if (object->texture_properties) {
        mem_free(object->texture_properties,
                 object->texture_property_count *
                     sizeof(*object->texture_properties));
    }
    if (object->int_properties) {
        mem_free(object->int_properties,
                 object->int_property_count * sizeof(*object->int_properties));
    }
    if (object->float_properties) {
        mem_free(object->float_properties,
                 object->float_property_count *
                     sizeof(*object->float_properties));
    }
    if (object->color_properties) {
        mem_free(object->color_properties,
                 object->color_property_count *
                     sizeof(*object->color_properties));
    }
    if (object->texture_stacks) {
        mem_free(object->texture_stacks,
                 object->texture_stack_count *
                     sizeof(*object->texture_stacks));
    }
    typetree_free_value(&object->root);
    typetree_free_type(&object->schema);
    material_object_init(object);
}

static MaterialObjectStatus validate_identity(
    const SerializedFile* file, const AssetObjectInfo* object) {
    if (!file || !object) return MATERIAL_OBJECT_INVALID_ARGUMENT;
    if (!source_file_is_structurally_valid(file)) {
        return MATERIAL_OBJECT_INVALID_SOURCE_FILE;
    }
    if (file->version != MATERIAL_SERIALIZED_FILE_VERSION) {
        return MATERIAL_OBJECT_UNSUPPORTED_FILE_VERSION;
    }
    if (strcmp(file->unity_version, "2021.3.29f1") != 0 &&
        strcmp(file->unity_version, "2021.3.35f1") != 0) {
        return MATERIAL_OBJECT_UNSUPPORTED_UNITY_VERSION;
    }
    if (!source_file_owns_object(file, object)) {
        return MATERIAL_OBJECT_OBJECT_NOT_OWNED;
    }
    if (!object_range_is_valid(file, object) || object->byte_size == 0U) {
        return MATERIAL_OBJECT_OBJECT_RANGE_INVALID;
    }
    if (object->type_id != MATERIAL_CLASS_ID) {
        return MATERIAL_OBJECT_NOT_MATERIAL;
    }
    if (object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        return MATERIAL_OBJECT_TYPE_INDEX_INVALID;
    }
    const TypeTreeType* type = &file->types[object->type_id_or_index];
    if (type->type_id != MATERIAL_CLASS_ID || type->is_ref_type ||
        type->script_type_index != object->script_type_index) {
        return MATERIAL_OBJECT_TYPE_RECORD_MISMATCH;
    }
    if (type->node_count == 0 && !type->nodes) {
        return MATERIAL_OBJECT_SCHEMA_UNRESOLVED;
    }
    if (type->node_count <= 0 || !type->nodes ||
        !typetree_validate_schema(type)) {
        return MATERIAL_OBJECT_SCHEMA_INVALID;
    }
    if (type->is_stripped || type->script_type_index != UINT16_MAX ||
        !bytes_are_zero(type->script_id_hash, sizeof(type->script_id_hash)) ||
        memcmp(type->type_hash, k_material_type_hash,
               sizeof(k_material_type_hash)) != 0 ||
        type->node_count != (int)MATERIAL_OBJECT_LAYOUT_NODE_COUNT ||
        typetree_schema_validate_known_profile(
            file->unity_version, strlen(file->unity_version),
            MATERIAL_CLASS_ID, type->type_hash, type) !=
            TYPETREE_SCHEMA_PROFILE_VALID) {
        return MATERIAL_OBJECT_TYPE_IDENTITY_UNSUPPORTED;
    }
    return MATERIAL_OBJECT_OK;
}

MaterialObjectStatus material_object_decode(
    MaterialObject* destination, const SerializedFile* file,
    const AssetObjectInfo* object) {
    if (!destination || !file || !object) {
        return MATERIAL_OBJECT_INVALID_ARGUMENT;
    }
    MaterialObjectStatus status = validate_identity(file, object);
    if (status != MATERIAL_OBJECT_OK) return status;

    MaterialObject candidate;
    material_object_init(&candidate);
    status = map_schema_clone_status(clone_material_schema(
        &candidate.schema, &file->types[object->type_id_or_index]));
    if (status != MATERIAL_OBJECT_OK) goto fail;

    const uint64_t absolute_offset = file->data_offset + object->byte_offset;
    const uint8_t* object_data = file->raw_data + (size_t)absolute_offset;
    ByteStream stream;
    stream_init(&stream, object_data, object->byte_size);
    stream_set_endian(&stream, file->big_endian);
    int node_index = 0;
    if (!typetree_parse_value(&candidate.schema, &node_index, &stream,
                              &candidate.root)) {
        status = MATERIAL_OBJECT_TYPETREE_PARSE_FAILED;
        goto fail;
    }
    if (node_index != candidate.schema.node_count) {
        status = MATERIAL_OBJECT_TYPETREE_NODES_NOT_EXHAUSTED;
        goto fail;
    }
    if (stream.position != (size_t)object->byte_size) {
        status = MATERIAL_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED;
        goto fail;
    }
    status = project_material_model(&candidate);
    if (status != MATERIAL_OBJECT_OK) goto fail;
    status = scan_material_wire(&candidate, object_data, object->byte_size,
                                file->big_endian);
    if (status != MATERIAL_OBJECT_OK) goto fail;

    candidate.path_id = object->path_id;
    candidate.byte_offset = object->byte_offset;
    candidate.byte_size = object->byte_size;
    candidate.source_type_index = object->type_id_or_index;
    candidate.serialized_file_version = file->version;
    candidate.target_platform = file->target_platform;
    memcpy(candidate.serialized_type_hash,
           file->types[object->type_id_or_index].type_hash,
           sizeof(candidate.serialized_type_hash));
    candidate.decoded = true;

    material_object_dispose(destination);
    *destination = candidate;
    return MATERIAL_OBJECT_OK;

fail:
    material_object_dispose(&candidate);
    return status;
}

float material_float32_value(MaterialFloat32 value) {
    float result;
    memcpy(&result, &value.bits, sizeof(result));
    return result;
}

const char* material_object_status_name(MaterialObjectStatus status) {
    switch (status) {
        case MATERIAL_OBJECT_OK: return "ok";
        case MATERIAL_OBJECT_INVALID_ARGUMENT: return "invalid-argument";
        case MATERIAL_OBJECT_INVALID_SOURCE_FILE:
            return "invalid-source-file";
        case MATERIAL_OBJECT_UNSUPPORTED_FILE_VERSION:
            return "unsupported-file-version";
        case MATERIAL_OBJECT_UNSUPPORTED_UNITY_VERSION:
            return "unsupported-unity-version";
        case MATERIAL_OBJECT_OBJECT_NOT_OWNED: return "object-not-owned";
        case MATERIAL_OBJECT_OBJECT_RANGE_INVALID:
            return "object-range-invalid";
        case MATERIAL_OBJECT_NOT_MATERIAL: return "not-material";
        case MATERIAL_OBJECT_TYPE_INDEX_INVALID: return "type-index-invalid";
        case MATERIAL_OBJECT_TYPE_RECORD_MISMATCH:
            return "type-record-mismatch";
        case MATERIAL_OBJECT_SCHEMA_UNRESOLVED: return "schema-unresolved";
        case MATERIAL_OBJECT_SCHEMA_INVALID: return "schema-invalid";
        case MATERIAL_OBJECT_TYPE_IDENTITY_UNSUPPORTED:
            return "type-identity-unsupported";
        case MATERIAL_OBJECT_SCHEMA_SIZE_OVERFLOW:
            return "schema-size-overflow";
        case MATERIAL_OBJECT_ALLOCATION_FAILED: return "allocation-failed";
        case MATERIAL_OBJECT_TYPETREE_PARSE_FAILED:
            return "typetree-parse-failed";
        case MATERIAL_OBJECT_TYPETREE_NODES_NOT_EXHAUSTED:
            return "typetree-nodes-not-exhausted";
        case MATERIAL_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED:
            return "object-bytes-not-exhausted";
        case MATERIAL_OBJECT_STRING_INVALID: return "string-invalid";
        case MATERIAL_OBJECT_COUNT_INVALID: return "count-invalid";
        case MATERIAL_OBJECT_DUPLICATE_KEY: return "duplicate-key";
        case MATERIAL_OBJECT_PAYLOAD_TRUNCATED: return "payload-truncated";
        case MATERIAL_OBJECT_MODEL_INVALID: return "model-invalid";
        case MATERIAL_OBJECT_NOT_DECODED: return "not-decoded";
        default: return "unknown";
    }
}
