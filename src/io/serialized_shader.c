// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_shader.h"
#include "io/serialized_shader_profile.h"
#include "io/parameter_layout.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

static bool allocate_serialized_array(void** out, int count,
                                      size_t element_size);

static bool own_string_exact(SerializedStringPool* pool,
                             const char** destination,
                             const char* source) {
    return serialized_string_pool_copy(pool, source, destination);
}

void serialized_shader_init(SerializedShader* shader) {
    if (shader) memset(shader, 0, sizeof(*shader));
}

typedef enum {
    SHADER_FIELD_REQUIRED = 0,
    SHADER_FIELD_OPTIONAL
} ShaderFieldPresence;

static bool profile_is_supported(SerializedShaderSchemaProfile profile) {
    return profile == SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1 ||
           profile ==
               SERIALIZED_SHADER_PROFILE_UNITY_2021_3_29F1_PLAYER_RESOURCES;
}

bool serialized_shader_profile_from_unity_version(
    const char* unity_version, SerializedShaderSchemaProfile* out_profile) {
    if (!unity_version || !out_profile) return false;
    if (strcmp(unity_version, "2021.3.35f1") == 0) {
        *out_profile = SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1;
        return true;
    }
    if (strcmp(unity_version, "2021.3.29f1") == 0) {
        *out_profile =
            SERIALIZED_SHADER_PROFILE_UNITY_2021_3_29F1_PLAYER_RESOURCES;
        return true;
    }
    return false;
}

static bool string_value_is_exact(const TypeTreeValue* value) {
    return value && value->type == VAL_TYPE_STRING && value->string_val &&
           strlen(value->string_val) == value->string_length;
}

static bool profile_get_field(SerializedShaderSchemaProfile profile,
                              const TypeTreeValue* parent,
                              const char* name,
                              ShaderFieldPresence presence,
                              const TypeTreeValue** out_field) {
    if (!profile_is_supported(profile) || !parent ||
        parent->type != VAL_TYPE_STRUCT || !name || !out_field) return false;
    *out_field = typetree_find_child(parent, name);
    if (*out_field != NULL || presence == SHADER_FIELD_OPTIONAL) return true;
    LOG_ERROR("Unity 2021.3 Shader field '%s' is required but missing", name);
    return false;
}

static bool profile_get_string(SerializedShaderSchemaProfile profile,
                               const TypeTreeValue* parent, const char* name,
                               ShaderFieldPresence presence,
                               const char** out_value) {
    if (!out_value) return false;
    *out_value = "";
    const TypeTreeValue* field = NULL;
    if (!profile_get_field(profile, parent, name, presence, &field)) {
        return false;
    }
    if (!field) return true;
    if (!string_value_is_exact(field)) {
        LOG_ERROR("Unity 2021.3 Shader field '%s' is not an exact string",
                  name);
        return false;
    }
    *out_value = field->string_val;
    return true;
}

static bool profile_get_int32(SerializedShaderSchemaProfile profile,
                              const TypeTreeValue* parent, const char* name,
                              ShaderFieldPresence presence, int* out_value) {
    if (!out_value) return false;
    *out_value = 0;
    const TypeTreeValue* field = NULL;
    if (!profile_get_field(profile, parent, name, presence, &field)) {
        return false;
    }
    if (!field) return true;
    int64_t value = 0;
    if (!typetree_value_get_int(field, &value) || value < INT_MIN ||
        value > INT_MAX) {
        LOG_ERROR("Unity 2021.3 Shader field '%s' is not an int32", name);
        return false;
    }
    *out_value = (int)value;
    return true;
}

static bool profile_get_uint32(SerializedShaderSchemaProfile profile,
                               const TypeTreeValue* parent, const char* name,
                               ShaderFieldPresence presence,
                               uint32_t* out_value) {
    if (!out_value) return false;
    *out_value = 0;
    const TypeTreeValue* field = NULL;
    if (!profile_get_field(profile, parent, name, presence, &field)) {
        return false;
    }
    if (!field) return true;
    uint64_t value = 0;
    if (!typetree_value_get_uint(field, &value) || value > UINT32_MAX) {
        LOG_ERROR("Unity 2021.3 Shader field '%s' is not a uint32", name);
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool profile_get_float(SerializedShaderSchemaProfile profile,
                              const TypeTreeValue* parent, const char* name,
                              ShaderFieldPresence presence,
                              float* out_value) {
    if (!out_value) return false;
    *out_value = 0.0f;
    const TypeTreeValue* field = NULL;
    if (!profile_get_field(profile, parent, name, presence, &field)) {
        return false;
    }
    if (!field) return true;
    if (field->type != VAL_TYPE_FLOAT) {
        LOG_ERROR("Unity 2021.3 Shader field '%s' is not a float", name);
        return false;
    }
    *out_value = (float)field->float_val;
    return true;
}

static bool profile_get_bool(SerializedShaderSchemaProfile profile,
                             const TypeTreeValue* parent, const char* name,
                             ShaderFieldPresence presence, bool* out_value) {
    if (!out_value) return false;
    *out_value = false;
    const TypeTreeValue* field = NULL;
    if (!profile_get_field(profile, parent, name, presence, &field)) {
        return false;
    }
    if (!field) return true;
    uint64_t value = 0;
    if (!typetree_value_get_uint(field, &value) || value > 1U) {
        LOG_ERROR("Unity 2021.3 Shader field '%s' is not a bool", name);
        return false;
    }
    *out_value = value != 0U;
    return true;
}

static bool value_array_is_valid(const TypeTreeValue* array,
                                 bool require_value_elements) {
    if (!array || array->type != VAL_TYPE_ARRAY ||
        array->array_val.count < 0) {
        return false;
    }
    if (array->array_val.count == 0) return true;
    if (require_value_elements) {
        return array->array_val.storage == TYPETREE_ARRAY_VALUES &&
               array->array_val.elements != NULL;
    }
    if (array->array_val.storage == TYPETREE_ARRAY_VALUES) {
        return array->array_val.elements != NULL;
    }
    if (array->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
        return array->array_val.packed_bytes != NULL;
    }
    return false;
}

static bool profile_optional_array(SerializedShaderSchemaProfile profile,
                                   const TypeTreeValue* field,
                                   bool require_value_elements,
                                   const TypeTreeValue** out_array) {
    if (!profile_is_supported(profile) || !out_array) return false;
    *out_array = NULL;
    if (!field) return true;
    const TypeTreeValue* array = typetree_get_array(field);
    if (!value_array_is_valid(array, require_value_elements)) return false;
    *out_array = array;
    return true;
}

static bool parse_properties(const TypeTreeValue* parent,
                             SerializedShader* shader,
                             SerializedShaderSchemaProfile profile) {
    const TypeTreeValue* prop_info =
        typetree_find_child(parent, "m_PropInfo");
    if (!prop_info) return true; /* Explicitly optional in this profile. */
    if (prop_info->type != VAL_TYPE_STRUCT) return false;
    const TypeTreeValue* props_field =
        typetree_find_child(prop_info, "m_Props");
    const TypeTreeValue* arr = NULL;
    if (!profile_optional_array(profile, props_field, true, &arr)) return false;
    if (!arr) return true;
    shader->property_count = arr->array_val.count;
    if (!allocate_serialized_array((void**)&shader->properties,
                                   shader->property_count,
                                   sizeof(*shader->properties))) {
        return false;
    }
    
    for (int i = 0; i < shader->property_count; i++) {
        const TypeTreeValue* prop_val = &arr->array_val.elements[i];
        ParsedShaderProperty* prop = &shader->properties[i];

        const char* property_name = NULL;
        const char* description = NULL;
        if (!profile_get_string(profile, prop_val, "m_Name",
                                SHADER_FIELD_REQUIRED, &property_name) ||
            !profile_get_string(profile, prop_val, "m_Description",
                                SHADER_FIELD_REQUIRED, &description) ||
            !profile_get_int32(profile, prop_val, "m_Type",
                               SHADER_FIELD_REQUIRED, &prop->type) ||
            !profile_get_uint32(profile, prop_val, "m_Flags",
                                SHADER_FIELD_REQUIRED, &prop->flags) ||
            !own_string_exact(&shader->owned_strings, &prop->name,
                              property_name) ||
            !own_string_exact(&shader->owned_strings, &prop->description,
                              description)) {
            return false;
        }
        
        char def_val_name[32];
        for (int j = 0; j < 4; j++) {
            snprintf(def_val_name, sizeof(def_val_name), "m_DefValue[%d]", j);
            if (!profile_get_float(profile, prop_val, def_val_name,
                                   SHADER_FIELD_OPTIONAL,
                                   &prop->def_value[j])) return false;
        }
        
        const TypeTreeValue* def_tex = typetree_find_child(prop_val, "m_DefTexture");
        if (def_tex) {
            const char* texture_name = NULL;
            if (!profile_get_string(profile, def_tex, "m_DefaultName",
                                    SHADER_FIELD_REQUIRED, &texture_name) ||
                !profile_get_int32(profile, def_tex, "m_TexDim",
                                   SHADER_FIELD_REQUIRED,
                                   &prop->def_texture_dim) ||
                !own_string_exact(&shader->owned_strings,
                                  &prop->def_texture_name,
                                  texture_name)) {
                return false;
            }
        }
        
        const TypeTreeValue* attrs = typetree_find_child(prop_val, "m_Attributes");
        const TypeTreeValue* attrs_arr = NULL;
        if (!profile_optional_array(profile, attrs, true, &attrs_arr)) {
            return false;
        }
        if (attrs_arr) {
            prop->attribute_count = attrs_arr->array_val.count;
            if (!allocate_serialized_array((void**)&prop->attributes,
                                           prop->attribute_count,
                                           sizeof(*prop->attributes))) {
                return false;
            }
            for (int j = 0; j < prop->attribute_count; j++) {
                const TypeTreeValue* attribute =
                    &attrs_arr->array_val.elements[j];
                if (!string_value_is_exact(attribute)) {
                    return false;
                }
                const char* attr_str = attribute->string_val;
                prop->attributes[j] = (char*)mem_alloc(strlen(attr_str) + 1);
                if (!prop->attributes[j]) return false;
                strcpy(prop->attributes[j], attr_str);
            }
        }
    }
    return true;
}

static bool parse_shader_dependencies(
    const TypeTreeValue* parsed_form, SerializedShader* shader,
    SerializedShaderSchemaProfile profile) {
    const TypeTreeValue* field =
        typetree_find_child(parsed_form, "m_Dependencies");
    const TypeTreeValue* array = NULL;
    if (!profile_optional_array(profile, field, true, &array)) return false;
    if (!array) return true;
    shader->dependency_count = array->array_val.count;
    if (!allocate_serialized_array((void**)&shader->dependencies,
                                   shader->dependency_count,
                                   sizeof(*shader->dependencies))) {
        return false;
    }
    for (int index = 0; index < shader->dependency_count; ++index) {
        const TypeTreeValue* value = &array->array_val.elements[index];
        const char* from = NULL;
        const char* to = NULL;
        if (!profile_get_string(profile, value, "from",
                                SHADER_FIELD_REQUIRED, &from) ||
            !profile_get_string(profile, value, "to",
                                SHADER_FIELD_REQUIRED, &to) ||
            !own_string_exact(&shader->owned_strings,
                              &shader->dependencies[index].from, from) ||
            !own_string_exact(&shader->owned_strings,
                              &shader->dependencies[index].to, to)) {
            return false;
        }
    }
    return true;
}

static bool parse_custom_editors_for_render_pipelines(
    const TypeTreeValue* parsed_form, SerializedShader* shader,
    SerializedShaderSchemaProfile profile) {
    const TypeTreeValue* field = typetree_find_child(
        parsed_form, "m_CustomEditorForRenderPipelines");
    const TypeTreeValue* array = NULL;
    if (!profile_optional_array(profile, field, true, &array)) return false;
    if (!array) return true;
    shader->custom_editor_for_render_pipeline_count =
        array->array_val.count;
    if (!allocate_serialized_array(
            (void**)&shader->custom_editors_for_render_pipelines,
            shader->custom_editor_for_render_pipeline_count,
            sizeof(*shader->custom_editors_for_render_pipelines))) {
        return false;
    }
    for (int index = 0;
         index < shader->custom_editor_for_render_pipeline_count; ++index) {
        const TypeTreeValue* value = &array->array_val.elements[index];
        SerializedCustomEditorForRenderPipeline* editor =
            &shader->custom_editors_for_render_pipelines[index];
        const char* editor_name = NULL;
        const char* pipeline_type = NULL;
        if (!profile_get_string(profile, value, "customEditorName",
                                SHADER_FIELD_REQUIRED, &editor_name) ||
            !profile_get_string(profile, value, "renderPipelineType",
                                SHADER_FIELD_REQUIRED, &pipeline_type) ||
            !own_string_exact(&shader->owned_strings,
                              &editor->custom_editor_name, editor_name) ||
            !own_string_exact(&shader->owned_strings,
                              &editor->render_pipeline_type,
                              pipeline_type)) {
            return false;
        }
    }
    return true;
}

typedef enum {
    /* Normal passes carry their effective map in m_State.m_Tags.  The outer
     * m_Tags field is the payload authority for UsePass and GrabPass. */
    SERIALIZED_TAGS_DIRECT,
    SERIALIZED_TAGS_RENDER_STATE,
} SerializedTagSource;

static bool parse_tags(const TypeTreeValue* parent,
                       SerializedTagSource source,
                       SerializedTagMap* tags_map,
                       SerializedStringPool* string_pool,
                       SerializedShaderSchemaProfile profile) {
    if (!profile_is_supported(profile) || !string_pool) return false;
    memset(tags_map, 0, sizeof(SerializedTagMap));
    const TypeTreeValue* owner = parent;
    if (source == SERIALIZED_TAGS_RENDER_STATE) {
        owner = typetree_find_child(parent, "m_State");
        if (owner && owner->type != VAL_TYPE_STRUCT) return false;
    }
    const TypeTreeValue* tags = typetree_find_child(owner, "m_Tags");
    if (!tags) return true;
    const TypeTreeValue* tags_arr = typetree_get_array(tags);
    if (!tags_arr) {
        tags_arr = typetree_find_child(tags, "tags");
        tags_arr = typetree_get_array(tags_arr);
    }
    if (!value_array_is_valid(tags_arr, true)) return false;
    
    tags_map->tag_count = tags_arr->array_val.count;
    if (!allocate_serialized_array((void**)&tags_map->tags,
                                   tags_map->tag_count,
                                   sizeof(*tags_map->tags))) {
        return false;
    }
    
    for (int i = 0; i < tags_map->tag_count; i++) {
        const TypeTreeValue* pair = &tags_arr->array_val.elements[i];
        const TypeTreeValue* first = typetree_find_child(pair, "first");
        const TypeTreeValue* second = typetree_find_child(pair, "second");
        if (!string_value_is_exact(first) ||
            !string_value_is_exact(second)) {
            return false;
        }
        if (!own_string_exact(string_pool, &tags_map->tags[i].key,
                              first->string_val) ||
            !own_string_exact(string_pool, &tags_map->tags[i].value,
                              second->string_val)) {
            return false;
        }
    }
    return true;
}

static bool parse_name_table(const TypeTreeValue* parent,
                             SerializedNameTable* name_table,
                             SerializedStringPool* string_pool,
                             SerializedShaderSchemaProfile profile) {
    if (!profile_is_supported(profile) || !string_pool) return false;
    memset(name_table, 0, sizeof(SerializedNameTable));
    const TypeTreeValue* name_indices = typetree_find_child(parent, "m_NameIndices");
    const TypeTreeValue* arr = NULL;
    if (!profile_optional_array(profile, name_indices, true, &arr)) {
        return false;
    }
    if (!arr) return true;
    
    name_table->count = arr->array_val.count;
    if (!allocate_serialized_array((void**)&name_table->entries,
                                   name_table->count,
                                   sizeof(*name_table->entries))) {
        return false;
    }
    
    for (int i = 0; i < name_table->count; i++) {
        const TypeTreeValue* pair = &arr->array_val.elements[i];
        const TypeTreeValue* first = typetree_find_child(pair, "first");
        const TypeTreeValue* second = typetree_find_child(pair, "second");
        int64_t index = 0;
        if (!string_value_is_exact(first) ||
            !typetree_value_get_int(second, &index) || index < INT_MIN ||
            index > INT_MAX) {
            return false;
        }
        if (!own_string_exact(string_pool, &name_table->entries[i].name,
                              first->string_val)) {
            return false;
        }
        name_table->entries[i].index = (int)index;
    }
    return true;
}

static bool parse_float_value(const TypeTreeValue* parent, const char* name,
                              SerializedShaderFloatValue* val,
                              SerializedStringPool* string_pool,
                              SerializedShaderSchemaProfile profile) {
    memset(val, 0, sizeof(SerializedShaderFloatValue));
    const TypeTreeValue* field = typetree_find_child(parent, name);
    if (field) {
        val->present = true;
        const char* serialized_name = NULL;
        return profile_get_float(profile, field, "val",
                                 SHADER_FIELD_REQUIRED, &val->val) &&
               profile_get_string(profile, field, "name",
                                  SHADER_FIELD_REQUIRED,
                                  &serialized_name) &&
               own_string_exact(string_pool, &val->name, serialized_name);
    }
    return true;
}

static bool parse_rt_blend(const TypeTreeValue* parent, const char* name,
                           SerializedShaderRTBlendState* state,
                           SerializedStringPool* string_pool,
                           SerializedShaderSchemaProfile profile) {
    const TypeTreeValue* field = typetree_find_child(parent, name);
    if (field) {
        return parse_float_value(field, "srcBlend", &state->srcBlend,
                                 string_pool, profile) &&
               parse_float_value(field, "destBlend", &state->destBlend,
                                 string_pool, profile) &&
               parse_float_value(field, "srcBlendAlpha",
                                 &state->srcBlendAlpha, string_pool,
                                 profile) &&
               parse_float_value(field, "destBlendAlpha",
                                 &state->destBlendAlpha, string_pool,
                                 profile) &&
               parse_float_value(field, "blendOp", &state->blendOp,
                                 string_pool, profile) &&
               parse_float_value(field, "blendOpAlpha",
                                 &state->blendOpAlpha, string_pool,
                                 profile) &&
               parse_float_value(field, "colMask", &state->colMask,
                                 string_pool, profile);
    }
    return true;
}

static bool parse_stencil_op(const TypeTreeValue* parent, const char* name,
                             SerializedStencilOp* op,
                             SerializedStringPool* string_pool,
                             SerializedShaderSchemaProfile profile) {
    const TypeTreeValue* field = typetree_find_child(parent, name);
    if (field) {
        return parse_float_value(field, "pass", &op->pass, string_pool,
                                 profile) &&
               parse_float_value(field, "fail", &op->fail, string_pool,
                                 profile) &&
               parse_float_value(field, "zFail", &op->zFail, string_pool,
                                 profile) &&
               parse_float_value(field, "comp", &op->comp, string_pool,
                                 profile);
    }
    return true;
}

static bool parse_render_state(const TypeTreeValue* parent,
                               SerializedShaderState* state,
                               SerializedStringPool* string_pool,
                               SerializedShaderSchemaProfile profile) {
    memset(state, 0, sizeof(SerializedShaderState));
    const char* state_name = NULL;
    if (!profile_get_string(profile, parent, "m_Name",
                            SHADER_FIELD_REQUIRED, &state_name) ||
        !own_string_exact(string_pool, &state->name, state_name)) {
        return false;
    }
    
    char rtName[16];
    for (int i = 0; i < 8; i++) {
        snprintf(rtName, sizeof(rtName), "rtBlend%d", i);
        if (!parse_rt_blend(parent, rtName, &state->rtBlend[i], string_pool,
                            profile)) {
            return false;
        }
    }
    if (!profile_get_bool(profile, parent, "rtSeparateBlend",
                          SHADER_FIELD_REQUIRED,
                          &state->rtSeparateBlend) ||
        !parse_float_value(parent, "zClip", &state->zClip, string_pool,
                           profile) ||
        !parse_float_value(parent, "zTest", &state->zTest, string_pool,
                           profile) ||
        !parse_float_value(parent, "zWrite", &state->zWrite, string_pool,
                           profile) ||
        !parse_float_value(parent, "culling", &state->culling, string_pool,
                           profile) ||
        !parse_float_value(parent, "conservative", &state->conservative,
                           string_pool, profile) ||
        !parse_float_value(parent, "offsetFactor", &state->offsetFactor,
                           string_pool, profile) ||
        !parse_float_value(parent, "offsetUnits", &state->offsetUnits,
                           string_pool, profile) ||
        !parse_float_value(parent, "alphaToMask", &state->alphaToMask,
                           string_pool, profile) ||
        !parse_stencil_op(parent, "stencilOp", &state->stencilOp,
                          string_pool, profile) ||
        !parse_stencil_op(parent, "stencilOpFront", &state->stencilOpFront,
                          string_pool, profile) ||
        !parse_stencil_op(parent, "stencilOpBack", &state->stencilOpBack,
                          string_pool, profile) ||
        !parse_float_value(parent, "stencilReadMask",
                           &state->stencilReadMask, string_pool, profile) ||
        !parse_float_value(parent, "stencilWriteMask",
                           &state->stencilWriteMask, string_pool, profile) ||
        !parse_float_value(parent, "stencilRef", &state->stencilRef,
                           string_pool, profile) ||
        !parse_float_value(parent, "fogStart", &state->fogStart,
                           string_pool, profile) ||
        !parse_float_value(parent, "fogEnd", &state->fogEnd, string_pool,
                           profile) ||
        !parse_float_value(parent, "fogDensity", &state->fogDensity,
                           string_pool, profile)) {
        return false;
    }
    
    const TypeTreeValue* fc = typetree_find_child(parent, "fogColor");
    if (fc) {
        const char* fog_name = NULL;
        if (!parse_float_value(fc, "x", &state->fogColor.x, string_pool,
                               profile) ||
            !parse_float_value(fc, "y", &state->fogColor.y, string_pool,
                               profile) ||
            !parse_float_value(fc, "z", &state->fogColor.z, string_pool,
                               profile) ||
            !parse_float_value(fc, "w", &state->fogColor.w, string_pool,
                               profile) ||
            !profile_get_string(profile, fc, "name", SHADER_FIELD_REQUIRED,
                                &fog_name) ||
            !own_string_exact(string_pool, &state->fogColor.name,
                              fog_name)) {
            return false;
        }
    }
    
    return profile_get_int32(profile, parent, "fogMode",
                             SHADER_FIELD_REQUIRED, &state->fogMode) &&
           profile_get_int32(profile, parent, "gpuProgramID",
                             SHADER_FIELD_REQUIRED, &state->gpuProgramID) &&
           profile_get_int32(profile, parent, "m_LOD",
                             SHADER_FIELD_REQUIRED, &state->lod) &&
           profile_get_bool(profile, parent, "lighting",
                            SHADER_FIELD_REQUIRED, &state->lighting);
}

static bool copy_indexed_name(const SerializedNameTable* name_table,
                              int name_index,
                              SerializedStringPool* string_pool,
                              const char** destination) {
    if (!destination || !string_pool) return false;
    *destination = NULL;
    if (!name_table) return false;
    for (int i = 0; i < name_table->count; i++) {
        if (name_table->entries[i].index != name_index) continue;
        return own_string_exact(string_pool, destination,
                                name_table->entries[i].name);
    }
    return false;
}

static bool allocate_serialized_array(void** out, int count,
                                      size_t element_size) {
    if (!out || count < 0 || element_size == 0 ||
        (count > 0 && (size_t)count > SIZE_MAX / element_size)) {
        return false;
    }
    *out = NULL;
    if (count == 0) return true;
    size_t byte_count = (size_t)count * element_size;
    void* values = mem_alloc(byte_count);
    if (!values) return false;
    memset(values, 0, byte_count);
    *out = values;
    return true;
}

static bool append_typetree_variables(const TypeTreeValue* list,
                                      const SerializedNameTable* name_table,
                                      SerializedStringPool* string_pool,
                                      bool is_matrix,
                                      SerializedVariable** variables,
                                      int* variable_count,
                                      SerializedShaderSchemaProfile profile) {
    const TypeTreeValue* array = NULL;
    if (!profile_optional_array(profile, list, true, &array)) return false;
    if (!array) return true;
    if (!variables || !variable_count || *variable_count < 0 ||
        array->array_val.count < 0 ||
        (array->array_val.count > 0 && !array->array_val.elements) ||
        array->array_val.count > INT_MAX - *variable_count) {
        return false;
    }
    int old_count = *variable_count;
    int new_count = old_count + array->array_val.count;
    if ((size_t)new_count > SIZE_MAX / sizeof(**variables)) return false;
    size_t old_size = (size_t)old_count * sizeof(**variables);
    size_t new_size = (size_t)new_count * sizeof(**variables);
    if (new_count != old_count) {
        SerializedVariable* resized = (SerializedVariable*)mem_realloc(
            *variables, old_size, new_size);
        if (!resized) return false;
        memset(resized + old_count, 0,
               (size_t)(new_count - old_count) * sizeof(*resized));
        *variables = resized;
    }
    for (int i = 0; i < array->array_val.count; i++) {
        const TypeTreeValue* value = &array->array_val.elements[i];
        SerializedVariable* variable = &(*variables)[old_count + i];
        int name_index = 0;
        if (!profile_get_int32(profile, value, "m_NameIndex",
                               SHADER_FIELD_REQUIRED, &name_index) ||
            !profile_get_uint32(profile, value, "m_Index",
                                SHADER_FIELD_REQUIRED,
                                &variable->layout[0]) ||
            !profile_get_uint32(profile, value, "m_ArraySize",
                                SHADER_FIELD_REQUIRED,
                                &variable->layout[1]) ||
            !profile_get_uint32(profile, value, "m_Type",
                                SHADER_FIELD_REQUIRED,
                                &variable->layout[2]) ||
            !profile_get_uint32(profile, value,
                                is_matrix ? "m_RowCount" : "m_Dim",
                                SHADER_FIELD_REQUIRED,
                                &variable->layout[3]) ||
            !copy_indexed_name(name_table, name_index, string_pool,
                               &variable->name)) {
            return false;
        }
        variable->layout[4] = is_matrix ? 1u : 0u;
    }
    *variable_count = new_count;
    return true;
}

static bool parse_typetree_structs(const TypeTreeValue* list,
                                   const SerializedNameTable* name_table,
                                   SerializedConstantBuffer* buffer,
                                   SerializedStringPool* string_pool,
                                   SerializedShaderSchemaProfile profile) {
    const TypeTreeValue* array = NULL;
    if (!profile_optional_array(profile, list, true, &array)) return false;
    if (!array) return true;
    if (array->array_val.count < 0 ||
        (array->array_val.count > 0 && !array->array_val.elements) ||
        !allocate_serialized_array((void**)&buffer->struct_params,
                                   array->array_val.count,
                                   sizeof(*buffer->struct_params))) {
        return false;
    }
    buffer->struct_count = array->array_val.count;
    for (int i = 0; i < array->array_val.count; i++) {
        const TypeTreeValue* value = &array->array_val.elements[i];
        SerializedStructParam* parameter = &buffer->struct_params[i];
        int name_index = 0;
        if (!profile_get_int32(profile, value, "m_NameIndex",
                               SHADER_FIELD_REQUIRED, &name_index) ||
            !profile_get_uint32(profile, value, "m_Index",
                                SHADER_FIELD_REQUIRED,
                                &parameter->layout[0]) ||
            !profile_get_uint32(profile, value, "m_ArraySize",
                                SHADER_FIELD_REQUIRED,
                                &parameter->layout[1]) ||
            !profile_get_uint32(profile, value, "m_StructSize",
                                SHADER_FIELD_REQUIRED,
                                &parameter->layout[2]) ||
            !copy_indexed_name(name_table, name_index, string_pool,
                               &parameter->name)) {
            return false;
        }
        if (!append_typetree_variables(
                typetree_find_child(value, "m_VectorMembers"), name_table,
                string_pool, false, &parameter->members,
                &parameter->member_count,
                profile) ||
            !append_typetree_variables(
                typetree_find_child(value, "m_MatrixMembers"), name_table,
                string_pool, true, &parameter->members,
                &parameter->member_count,
                profile)) {
            return false;
        }
    }
    return true;
}

static bool add_typetree_array_count(const TypeTreeValue* array,
                                     int* total) {
    if (!total || *total < 0 || !array) return total != NULL;
    if (array->array_val.count < 0 ||
        (array->array_val.count > 0 && !array->array_val.elements) ||
        array->array_val.count > INT_MAX - *total) {
        return false;
    }
    *total += array->array_val.count;
    return true;
}

static bool derive_constant_buffer_size(
    const SerializedProgramParameters* parameters,
    SerializedConstantBuffer* buffer) {
    if (!parameters || !buffer || buffer->var_count < 0 ||
        (buffer->var_count > 0 && !buffer->variables)) {
        return false;
    }
    uint32_t extent = 0U;
    for (int variable_index = 0;
         variable_index < buffer->var_count; ++variable_index) {
        DecodedVariableLayout layout;
        if (!parameter_layout_decode(
                parameters, &buffer->variables[variable_index], &layout)) {
            return false;
        }
        const uint32_t byte_size = parameter_layout_byte_size(&layout);
        if (byte_size == 0U || layout.byte_offset > UINT32_MAX - byte_size) {
            return false;
        }
        const uint32_t end = layout.byte_offset + byte_size;
        if (end > extent) extent = end;
    }
    if (extent > UINT32_MAX - 15U) return false;
    buffer->size = (extent + 15U) & ~UINT32_C(15);
    return true;
}

static bool serialized_program_parameters_parse_typetree_impl(
    SerializedProgramParameters* params, const TypeTreeValue* parent,
    const SerializedNameTable* name_table,
    SerializedShaderSchemaProfile profile) {
    if (!profile_is_supported(profile) || !parent ||
        parent->type != VAL_TYPE_STRUCT) return false;
    const TypeTreeValue* params_node =
        typetree_find_child(parent, "m_CommonParameters");
    if (params_node) {
        if (params_node->type != VAL_TYPE_STRUCT) return false;
        parent = params_node;
    }

    const TypeTreeValue* loose_vectors =
        typetree_find_child(parent, "m_VectorParams");
    const TypeTreeValue* loose_matrices =
        typetree_find_child(parent, "m_MatrixParams");
    const TypeTreeValue* loose_vector_array = NULL;
    const TypeTreeValue* loose_matrix_array = NULL;
    if (!profile_optional_array(profile, loose_vectors, true,
                                &loose_vector_array) ||
        !profile_optional_array(profile, loose_matrices, true,
                                &loose_matrix_array)) {
        return false;
    }
    bool has_loose_variables =
        (loose_vector_array && loose_vector_array->array_val.count > 0) ||
        (loose_matrix_array && loose_matrix_array->array_val.count > 0);
    int constant_buffer_base = has_loose_variables ? 1 : 0;
    const TypeTreeValue* cb_list = typetree_find_child(parent, "m_ConstantBuffers");
    const TypeTreeValue* cb_arr = NULL;
    if (!profile_optional_array(profile, cb_list, true, &cb_arr)) return false;
    int serialized_cb_count = cb_arr ? cb_arr->array_val.count : 0;
    if (serialized_cb_count < 0 ||
        (serialized_cb_count > 0 && !cb_arr->array_val.elements) ||
        serialized_cb_count > INT_MAX - constant_buffer_base) {
        return false;
    }
    params->cb_count = constant_buffer_base + serialized_cb_count;
    if (!allocate_serialized_array((void**)&params->constant_buffers,
                                   params->cb_count,
                                   sizeof(*params->constant_buffers))) {
        return false;
    }
    if (has_loose_variables) {
        SerializedConstantBuffer* globals = &params->constant_buffers[0];
        globals->role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
        if (!own_string_exact(&params->owned_strings, &globals->name,
                              "$Globals")) {
            return false;
        }
        if (!append_typetree_variables(loose_vectors, name_table,
                                       &params->owned_strings, false,
                                       &globals->variables,
                                       &globals->var_count, profile) ||
            !append_typetree_variables(loose_matrices, name_table,
                                       &params->owned_strings, true,
                                       &globals->variables,
                                       &globals->var_count, profile)) {
            return false;
        }
        if (!derive_constant_buffer_size(params, globals)) return false;
    }

    if (cb_arr) {
        for (int i = 0; i < cb_arr->array_val.count; i++) {
            const TypeTreeValue* cb_val = &cb_arr->array_val.elements[i];
            SerializedConstantBuffer* cb =
                &params->constant_buffers[constant_buffer_base + i];
            cb->role = SERIALIZED_CBUFFER_NAMED;
            
            int name_idx = 0;
            if (!profile_get_int32(profile, cb_val, "m_NameIndex",
                                   SHADER_FIELD_REQUIRED, &name_idx) ||
                !profile_get_uint32(profile, cb_val, "m_Size",
                                    SHADER_FIELD_REQUIRED, &cb->size) ||
                !copy_indexed_name(name_table, name_idx,
                                   &params->owned_strings, &cb->name)) {
                return false;
            }
            cb->has_is_partial =
                typetree_find_child(cb_val, "m_IsPartialCB") != NULL;
            if (!profile_get_bool(profile, cb_val, "m_IsPartialCB",
                                  SHADER_FIELD_OPTIONAL,
                                  &cb->is_partial)) return false;
            if (!append_typetree_variables(
                    typetree_find_child(cb_val, "m_MatrixParams"),
                    name_table, &params->owned_strings, true,
                    &cb->variables, &cb->var_count,
                    profile) ||
                !append_typetree_variables(
                    typetree_find_child(cb_val, "m_VectorParams"),
                    name_table, &params->owned_strings, false,
                    &cb->variables, &cb->var_count,
                    profile) ||
                !parse_typetree_structs(
                    typetree_find_child(cb_val, "m_StructParams"),
                    name_table, cb, &params->owned_strings, profile)) {
                return false;
            }
        }
    }
    
    const TypeTreeValue* tex_list =
        typetree_find_child(parent, "m_TextureParams");
    const TypeTreeValue* tex_arr = NULL;
    const TypeTreeValue* buf_list = typetree_find_child(parent, "m_BufferParams");
    const TypeTreeValue* buf_arr = NULL;
    const TypeTreeValue* uav_list = typetree_find_child(parent, "m_UAVParams");
    const TypeTreeValue* uav_arr = NULL;
    const TypeTreeValue* smp_list =
        typetree_find_child(parent, "m_Samplers");
    const TypeTreeValue* smp_arr = NULL;
    const TypeTreeValue* cbb_list =
        typetree_find_child(parent, "m_ConstantBufferBindings");
    const TypeTreeValue* cbb_arr = NULL;
    if (!profile_optional_array(profile, tex_list, true, &tex_arr) ||
        !profile_optional_array(profile, buf_list, true, &buf_arr) ||
        !profile_optional_array(profile, uav_list, true, &uav_arr) ||
        !profile_optional_array(profile, smp_list, true, &smp_arr) ||
        !profile_optional_array(profile, cbb_list, true, &cbb_arr)) {
        return false;
    }

    params->res_count = 0;
    if (!add_typetree_array_count(tex_arr, &params->res_count) ||
        !add_typetree_array_count(buf_arr, &params->res_count) ||
        !add_typetree_array_count(uav_arr, &params->res_count) ||
        !add_typetree_array_count(smp_arr, &params->res_count) ||
        !add_typetree_array_count(cbb_arr, &params->res_count) ||
        !allocate_serialized_array((void**)&params->resources,
                                   params->res_count,
                                   sizeof(*params->resources))) {
        return false;
    }

    int res_idx = 0;
    if (tex_arr) {
        for (int i = 0; i < tex_arr->array_val.count; i++) {
            const TypeTreeValue* tex_val = &tex_arr->array_val.elements[i];
            SerializedResourceParam* res = &params->resources[res_idx++];
            int r_name_idx = 0;
            int sampler_index = 0;
            if (!profile_get_int32(profile, tex_val, "m_NameIndex",
                                   SHADER_FIELD_REQUIRED, &r_name_idx) ||
                !profile_get_uint32(profile, tex_val, "m_Index",
                                    SHADER_FIELD_REQUIRED,
                                    &res->bind_index) ||
                !profile_get_int32(profile, tex_val, "m_SamplerIndex",
                                   SHADER_FIELD_REQUIRED,
                                   &sampler_index) ||
                !profile_get_bool(profile, tex_val, "m_MultiSampled",
                                  SHADER_FIELD_REQUIRED,
                                  &res->multisampled) ||
                !profile_get_uint32(profile, tex_val, "m_Dim",
                                    SHADER_FIELD_REQUIRED,
                                    &res->dimension) ||
                !copy_indexed_name(name_table, r_name_idx,
                                   &params->owned_strings, &res->name)) {
                return false;
            }
            res->bind_type = SERIALIZED_RESOURCE_TEXTURE;
            /* Unity uses -1 as the no-sampler sentinel; preserve its raw
             * two's-complement bits in the unsigned projection. */
            res->sampler_index = (uint32_t)sampler_index;
            res->extra[0] = res->sampler_index;
            res->extra[1] =
                (res->dimension << 1) | (res->multisampled ? 1u : 0u);
        }
    }
    
    if (buf_arr) {
        for (int i = 0; i < buf_arr->array_val.count; i++) {
            const TypeTreeValue* buf_val = &buf_arr->array_val.elements[i];
            SerializedResourceParam* res = &params->resources[res_idx++];
            int r_name_idx = 0;
            if (!profile_get_int32(profile, buf_val, "m_NameIndex",
                                   SHADER_FIELD_REQUIRED, &r_name_idx) ||
                !profile_get_uint32(profile, buf_val, "m_Index",
                                    SHADER_FIELD_REQUIRED,
                                    &res->bind_index) ||
                !profile_get_uint32(profile, buf_val, "m_ArraySize",
                                    SHADER_FIELD_REQUIRED,
                                    &res->array_size) ||
                !copy_indexed_name(name_table, r_name_idx,
                                   &params->owned_strings, &res->name)) {
                return false;
            }
            res->bind_type = SERIALIZED_RESOURCE_BUFFER;
            res->extra[0] = res->array_size;
        }
    }
    
    if (uav_arr) {
        for (int i = 0; i < uav_arr->array_val.count; i++) {
            const TypeTreeValue* uav_val = &uav_arr->array_val.elements[i];
            SerializedResourceParam* res = &params->resources[res_idx++];
            int r_name_idx = 0;
            if (!profile_get_int32(profile, uav_val, "m_NameIndex",
                                   SHADER_FIELD_REQUIRED, &r_name_idx) ||
                !profile_get_uint32(profile, uav_val, "m_Index",
                                    SHADER_FIELD_REQUIRED,
                                    &res->bind_index) ||
                !profile_get_uint32(profile, uav_val, "m_OriginalIndex",
                                    SHADER_FIELD_REQUIRED,
                                    &res->original_index) ||
                !copy_indexed_name(name_table, r_name_idx,
                                   &params->owned_strings, &res->name)) {
                return false;
            }
            res->bind_type = SERIALIZED_RESOURCE_UAV;
            res->extra[0] = res->original_index;
        }
    }
    
    if (smp_arr) {
        for (int i = 0; i < smp_arr->array_val.count; i++) {
            const TypeTreeValue* smp_val = &smp_arr->array_val.elements[i];
            SerializedResourceParam* res = &params->resources[res_idx++];
            if (!own_string_exact(&params->owned_strings, &res->name, "")) {
                return false;
            }
            res->bind_type = SERIALIZED_RESOURCE_SAMPLER;
            if (!profile_get_uint32(profile, smp_val, "bindPoint",
                                    SHADER_FIELD_REQUIRED,
                                    &res->bind_index) ||
                !profile_get_uint32(profile, smp_val, "sampler",
                                    SHADER_FIELD_REQUIRED,
                                    &res->sampler_state)) return false;
            res->extra[0] = res->sampler_state;
        }
    }
    
    // Parse m_ConstantBufferBindings: the definitive cbuffer name → DXBC register mapping
    // (confirmed from ida-graphify: SerializedProgramParameters::Transfer reads this at offset 30)
    if (cbb_arr) {
        for (int i = 0; i < cbb_arr->array_val.count; i++) {
            const TypeTreeValue* cbb_val = &cbb_arr->array_val.elements[i];
            SerializedResourceParam* res = &params->resources[res_idx++];
            int r_name_idx = 0;
            if (!profile_get_int32(profile, cbb_val, "m_NameIndex",
                                   SHADER_FIELD_REQUIRED, &r_name_idx) ||
                !profile_get_uint32(profile, cbb_val, "m_Index",
                                    SHADER_FIELD_REQUIRED,
                                    &res->bind_index) ||
                !profile_get_uint32(profile, cbb_val, "m_ArraySize",
                                    SHADER_FIELD_REQUIRED,
                                    &res->array_size) ||
                !copy_indexed_name(name_table, r_name_idx,
                                   &params->owned_strings, &res->name)) {
                return false;
            }
            res->bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER;
            res->extra[0] = res->array_size;
        }
    }
    return res_idx == params->res_count;
}

bool serialized_program_parameters_parse_typetree(
    SerializedProgramParameters* params, const TypeTreeValue* parent,
    const SerializedNameTable* name_table) {
    if (!params || !parent) return false;
    SerializedProgramParameters parsed;
    serialized_program_parameters_init(&parsed);
    if (!serialized_program_parameters_parse_typetree_impl(
            &parsed, parent, name_table,
            SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1)) {
        serialized_program_parameters_free(&parsed);
        return false;
    }
    serialized_program_parameters_free(params);
    *params = parsed;
    return true;
}

static bool copy_int_array(const TypeTreeValue* field, int* out_count,
                           int** out_values,
                           SerializedShaderSchemaProfile profile) {
    if (!out_count || !out_values) return false;
    *out_count = 0;
    *out_values = NULL;
    const TypeTreeValue* array = NULL;
    if (!profile_optional_array(profile, field, false, &array)) return false;
    if (!array || array->array_val.count == 0) return true;

    int count = array->array_val.count;
    int* values = NULL;
    if (!allocate_serialized_array((void**)&values, count,
                                   sizeof(*values))) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        int64_t value = 0;
        if (!typetree_array_get_int(array, i, &value) || value < INT_MIN ||
            value > INT_MAX) {
            mem_free(values, (size_t)count * sizeof(*values));
            return false;
        }
        values[i] = (int)value;
    }
    *out_count = count;
    *out_values = values;
    return true;
}

static bool copy_u16_array(const TypeTreeValue* field, int* out_count,
                           uint16_t** out_values,
                           SerializedShaderSchemaProfile profile) {
    if (!out_count || !out_values) return false;
    *out_count = 0;
    *out_values = NULL;
    const TypeTreeValue* array = NULL;
    if (!profile_optional_array(profile, field, false, &array)) return false;
    if (!array || array->array_val.count == 0) return true;

    int count = array->array_val.count;
    uint16_t* values = NULL;
    if (!allocate_serialized_array((void**)&values, count,
                                   sizeof(*values))) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        int64_t value = 0;
        if (!typetree_array_get_int(array, i, &value) || value < 0 ||
            value > UINT16_MAX) {
            mem_free(values, (size_t)count * sizeof(*values));
            return false;
        }
        values[i] = (uint16_t)value;
    }
    *out_count = count;
    *out_values = values;
    return true;
}

static char* duplicate_keyword_name(const SerializedShader* shader,
                                    int keyword_index) {
    if (!shader || keyword_index < 0 ||
        keyword_index >= shader->keyword_names.count ||
        !shader->keyword_names.keywords ||
        !shader->keyword_names.keywords[keyword_index]) {
        return NULL;
    }
    const char* name = shader->keyword_names.keywords[keyword_index];
    size_t length = strlen(name);
    char* copy = (char*)mem_alloc(length + 1);
    if (copy) memcpy(copy, name, length + 1);
    return copy;
}

static bool copy_keyword_scope(const TypeTreeValue* field,
                               const SerializedShader* shader,
                               int* out_name_count, char*** out_names,
                               int* out_index_count, int** out_indices,
                               SerializedShaderSchemaProfile profile) {
    if (!shader || !out_name_count || !out_names || !out_index_count ||
        !out_indices || shader->keyword_names.count < 0 ||
        (shader->keyword_names.count > 0 &&
         !shader->keyword_names.keywords)) {
        return false;
    }
    *out_name_count = 0;
    *out_names = NULL;
    *out_index_count = 0;
    *out_indices = NULL;
    const TypeTreeValue* indices = NULL;
    if (!profile_optional_array(profile, field, false, &indices)) return false;
    if (!indices || indices->array_val.count == 0) return true;

    int count = indices->array_val.count;
    char** names = NULL;
    int* raw_indices = NULL;
    if (!allocate_serialized_array((void**)&names, count, sizeof(*names)) ||
        !allocate_serialized_array((void**)&raw_indices, count,
                                   sizeof(*raw_indices))) {
        if (names) mem_free(names, (size_t)count * sizeof(*names));
        if (raw_indices)
            mem_free(raw_indices, (size_t)count * sizeof(*raw_indices));
        return false;
    }

    for (int i = 0; i < count; i++) {
        int64_t raw_index = -1;
        if (!typetree_array_get_int(indices, i, &raw_index) ||
            raw_index < 0 || raw_index >= shader->keyword_names.count) {
            for (int j = 0; j < i; j++) {
                mem_free(names[j], strlen(names[j]) + 1);
            }
            mem_free(names, (size_t)count * sizeof(*names));
            mem_free(raw_indices, (size_t)count * sizeof(*raw_indices));
            return false;
        }
        raw_indices[i] = (int)raw_index;
        names[i] = duplicate_keyword_name(shader, raw_indices[i]);
        if (!names[i]) {
            for (int j = 0; j < i; j++) {
                mem_free(names[j], strlen(names[j]) + 1);
            }
            mem_free(names, (size_t)count * sizeof(*names));
            mem_free(raw_indices, (size_t)count * sizeof(*raw_indices));
            return false;
        }
    }

    *out_name_count = count;
    *out_names = names;
    *out_index_count = count;
    *out_indices = raw_indices;
    return true;
}

static bool parse_subprogram_keywords(
    const TypeTreeValue* subprogram_value, SerializedSubProgram* subprogram,
    SerializedSubProgramIdentity* identity, const SerializedShader* shader,
    SerializedShaderSchemaProfile profile) {
    if (!subprogram_value || !subprogram || !identity || !shader) {
        return false;
    }
    const TypeTreeValue* global_field =
        typetree_find_child(subprogram_value, "m_GlobalKeywordIndices");
    const TypeTreeValue* local_field =
        typetree_find_child(subprogram_value, "m_LocalKeywordIndices");
    if (global_field || local_field) {
        identity->keyword_scopes_are_explicit = true;
        return copy_keyword_scope(
                   global_field, shader, &subprogram->global_keyword_count,
                   &subprogram->global_keywords,
                   &identity->global_keyword_index_count,
                   &identity->global_keyword_indices, profile) &&
               copy_keyword_scope(
                   local_field, shader, &subprogram->local_keyword_count,
                   &subprogram->local_keywords,
                   &identity->local_keyword_index_count,
                   &identity->local_keyword_indices, profile);
    }

    const TypeTreeValue* legacy_field =
        typetree_find_child(subprogram_value, "m_KeywordIndices");
    if (legacy_field) {
        identity->keyword_scopes_are_explicit = false;
        return copy_keyword_scope(
            legacy_field, shader, &subprogram->local_keyword_count,
            &subprogram->local_keywords, &identity->local_keyword_index_count,
            &identity->local_keyword_indices, profile);
    }
    return true;
}

static void serialized_subprogram_free(SerializedSubProgram* subprogram) {
    if (!subprogram) return;
    if (subprogram->local_keywords && subprogram->local_keyword_count >= 0) {
        for (int index = 0; index < subprogram->local_keyword_count; ++index) {
            if (subprogram->local_keywords[index]) {
                mem_free(subprogram->local_keywords[index],
                         strlen(subprogram->local_keywords[index]) + 1U);
            }
        }
        mem_free(subprogram->local_keywords,
                 (size_t)subprogram->local_keyword_count * sizeof(char*));
    }
    if (subprogram->global_keywords && subprogram->global_keyword_count >= 0) {
        for (int index = 0; index < subprogram->global_keyword_count;
             ++index) {
            if (subprogram->global_keywords[index]) {
                mem_free(subprogram->global_keywords[index],
                         strlen(subprogram->global_keywords[index]) + 1U);
            }
        }
        mem_free(subprogram->global_keywords,
                 (size_t)subprogram->global_keyword_count * sizeof(char*));
    }
    memset(subprogram, 0, sizeof(*subprogram));
}

static bool get_required_s64_bits(const TypeTreeValue* parent,
                                  const char* name, uint64_t* out_value,
                                  SerializedShaderSchemaProfile profile) {
    if (!out_value) return false;
    const TypeTreeValue* field = NULL;
    if (!profile_get_field(profile, parent, name, SHADER_FIELD_REQUIRED,
                           &field)) return false;
    /* Unity 2021.3's SerializedPlayerSubProgram::Transfer declares this
     * authority as SInt64.  Conversion from signed to unsigned is defined
     * modulo 2^64 and therefore preserves every serialized mask bit,
     * including the sign bit. */
    if (!field || field->type != VAL_TYPE_INT || field->integer_is_unsigned) {
        return false;
    }
    *out_value = (uint64_t)field->int_val;
    return true;
}

static bool get_optional_int32(SerializedShaderSchemaProfile profile,
                               const TypeTreeValue* parent, const char* name,
                               bool* out_present, int* out_value) {
    if (!out_present || !out_value) return false;
    const TypeTreeValue* field = NULL;
    if (!profile_get_field(profile, parent, name, SHADER_FIELD_OPTIONAL,
                           &field)) return false;
    *out_present = field != NULL;
    return profile_get_int32(profile, parent, name, SHADER_FIELD_OPTIONAL,
                             out_value);
}

static bool parse_subprograms_for_stage(const TypeTreeValue* prog_struct,
                                        SerializedPass* pass, int stage_idx,
                                        const SerializedShader* shader,
                                        SerializedShaderSchemaProfile profile) {
    if (!prog_struct || !pass || !shader || stage_idx < 0 || stage_idx >= 6 ||
        pass->subprograms[stage_idx] ||
        pass->subprogram_identities[stage_idx] ||
        pass->subprogram_param_blob_indices[stage_idx] ||
        pass->subprogram_count[stage_idx] != 0) {
        return false;
    }
    const TypeTreeValue* subprogs = typetree_find_child(prog_struct, "m_PlayerSubPrograms");
    if (!subprogs) subprogs = typetree_find_child(prog_struct, "m_SubPrograms");
    const TypeTreeValue* outer_arr = NULL;
    if (!profile_optional_array(profile, subprogs, true, &outer_arr)) {
        return false;
    }
    if (!outer_arr) return true;

    const TypeTreeValue* param_blob_indices =
        typetree_find_child(prog_struct, "m_ParameterBlobIndices");
    const TypeTreeValue* pbi_outer = NULL;
    if (!profile_optional_array(profile, param_blob_indices, true,
                                &pbi_outer)) return false;
    if (pbi_outer &&
        pbi_outer->array_val.count != outer_arr->array_val.count) {
        return false;
    }
    
    int total_subs = 0;
    for (int i = 0; i < outer_arr->array_val.count; i++) {
        const TypeTreeValue* inner = typetree_get_array(
            &outer_arr->array_val.elements[i]);
        if (!value_array_is_valid(inner, true) ||
            inner->array_val.count > INT_MAX - total_subs) {
            return false;
        }
        if (pbi_outer) {
            const TypeTreeValue* pbi_inner = typetree_get_array(
                &pbi_outer->array_val.elements[i]);
            if (!value_array_is_valid(pbi_inner, false) ||
                pbi_inner->array_val.count != inner->array_val.count) {
                return false;
            }
            for (int j = 0; j < pbi_inner->array_val.count; j++) {
                int64_t parameter_blob_index = -1;
                if (!typetree_array_get_int(pbi_inner, j,
                                            &parameter_blob_index) ||
                    parameter_blob_index < -1 ||
                    parameter_blob_index > INT_MAX) {
                    return false;
                }
            }
        }
        total_subs += inner->array_val.count;
    }
    if (total_subs == 0) return true;
    
    SerializedSubProgram* subprograms = NULL;
    SerializedSubProgramIdentity* identities = NULL;
    int* parameter_indices = NULL;
    if (!allocate_serialized_array((void**)&subprograms, total_subs,
                                   sizeof(*subprograms)) ||
        !allocate_serialized_array((void**)&identities, total_subs,
                                   sizeof(*identities)) ||
        !allocate_serialized_array((void**)&parameter_indices, total_subs,
                                   sizeof(*parameter_indices))) {
        if (subprograms)
            mem_free(subprograms,
                     (size_t)total_subs * sizeof(*subprograms));
        if (identities)
            mem_free(identities,
                     (size_t)total_subs * sizeof(*identities));
        if (parameter_indices)
            mem_free(parameter_indices,
                     (size_t)total_subs * sizeof(*parameter_indices));
        return false;
    }
    pass->subprograms[stage_idx] = subprograms;
    pass->subprogram_identities[stage_idx] = identities;
    pass->subprogram_param_blob_indices[stage_idx] = parameter_indices;
    /* Set the owned extent before populating it so outer cleanup can release
     * every partially built keyword scope after any later parse failure. */
    pass->subprogram_count[stage_idx] = total_subs;
    for (int i = 0; i < total_subs; i++) {
        pass->subprogram_param_blob_indices[stage_idx][i] = -1;
    }
    
    int sub_idx = 0;
    for (int i = 0; i < outer_arr->array_val.count; i++) {
        const TypeTreeValue* inner = typetree_get_array(
            &outer_arr->array_val.elements[i]);
        
        const TypeTreeValue* pbi_inner = NULL;
        if (pbi_outer) {
            pbi_inner = typetree_get_array(&pbi_outer->array_val.elements[i]);
        }
        
        for (int j = 0; j < inner->array_val.count; j++) {
            const TypeTreeValue* sp_val = &inner->array_val.elements[j];
            if (sp_val->type != VAL_TYPE_STRUCT) return false;
            SerializedSubProgram* sub = &pass->subprograms[stage_idx][sub_idx];
            SerializedSubProgramIdentity* identity =
                &pass->subprogram_identities[stage_idx][sub_idx];

            identity->hardware_tier_group = i;
            identity->inner_subprogram_index = j;

            if (!profile_get_int32(profile, sp_val, "m_BlobIndex",
                                   SHADER_FIELD_REQUIRED,
                                   &sub->blob_index) ||
                !get_optional_int32(profile, sp_val,
                                    "m_ShaderHardwareTier",
                                    &sub->has_hardware_tier,
                                    &sub->hardware_tier) ||
                (sub->has_hardware_tier &&
                 (sub->hardware_tier < 0 || sub->hardware_tier > 3)) ||
                !get_required_s64_bits(sp_val, "m_ShaderRequirements",
                                       &sub->shader_requirements, profile) ||
                !profile_get_int32(profile, sp_val, "m_GpuProgramType",
                                   SHADER_FIELD_REQUIRED,
                                   &sub->program_type) ||
                !parse_subprogram_keywords(sp_val, sub, identity, shader,
                                           profile)) {
                return false;
            }

            if (pbi_inner) {
                int64_t parameter_blob_index = -1;
                if (!typetree_array_get_int(pbi_inner, j,
                                            &parameter_blob_index) ||
                    parameter_blob_index < -1 ||
                    parameter_blob_index > INT_MAX) {
                    return false;
                }
                pass->subprogram_param_blob_indices[stage_idx][sub_idx] =
                    (int)parameter_blob_index;
            }
            
            sub_idx++;
        }
    }
    return sub_idx == total_subs;
}

static bool serialized_shader_parse_with_profile_impl(
    SerializedShader* shader, const TypeTreeValue* value,
    SerializedShaderSchemaProfile profile) {
    if (!shader || !value || value->type != VAL_TYPE_STRUCT ||
        !profile_is_supported(profile)) return false;
    serialized_shader_init(shader);
    
    const TypeTreeValue* parsed_form = NULL;
    if (!profile_get_field(profile, value, "m_ParsedForm",
                           SHADER_FIELD_REQUIRED, &parsed_form)) return false;
    if (parsed_form->type != VAL_TYPE_STRUCT) return false;
    const TypeTreeValue* archive_platforms =
        typetree_find_child(value, "platforms");
    if (!copy_int_array(archive_platforms, &shader->archive_platform_count,
                        &shader->archive_platforms, profile)) {
        serialized_shader_free(shader);
        return false;
    }
    
    const char* shader_name = NULL;
    const char* fallback_name = NULL;
    const char* custom_editor_name = NULL;
    if (!profile_get_string(profile, parsed_form, "m_Name",
                            SHADER_FIELD_REQUIRED, &shader_name) ||
        !profile_get_string(profile, parsed_form, "m_FallbackName",
                            SHADER_FIELD_OPTIONAL, &fallback_name) ||
        !profile_get_string(profile, parsed_form, "m_CustomEditorName",
                            SHADER_FIELD_OPTIONAL, &custom_editor_name) ||
        !own_string_exact(&shader->owned_strings, &shader->name,
                          shader_name) ||
        !own_string_exact(&shader->owned_strings, &shader->fallback_name,
                          fallback_name) ||
        !own_string_exact(&shader->owned_strings,
                          &shader->custom_editor_name,
                          custom_editor_name)) {
        serialized_shader_free(shader);
        return false;
    }

    if (!parse_shader_dependencies(parsed_form, shader, profile) ||
        !parse_custom_editors_for_render_pipelines(
            parsed_form, shader, profile) ||
        !profile_get_bool(profile, parsed_form,
                          "m_DisableNoSubshadersMessage",
                          SHADER_FIELD_OPTIONAL,
                          &shader->disable_no_subshaders_message)) {
        serialized_shader_free(shader);
        return false;
    }
    
    const TypeTreeValue* kw_names = typetree_find_child(parsed_form, "m_KeywordNames");
    const TypeTreeValue* kw_arr = NULL;
    if (!profile_optional_array(profile, kw_names, true, &kw_arr)) {
        serialized_shader_free(shader);
        return false;
    }
    if (kw_arr) {
        shader->keyword_names.count = kw_arr->array_val.count;
        if (!allocate_serialized_array(
                (void**)&shader->keyword_names.keywords,
                shader->keyword_names.count,
                sizeof(*shader->keyword_names.keywords))) {
            serialized_shader_free(shader);
            return false;
        }
        for (int i = 0; i < shader->keyword_names.count; i++) {
            const TypeTreeValue* keyword = &kw_arr->array_val.elements[i];
            if (!string_value_is_exact(keyword)) {
                serialized_shader_free(shader);
                return false;
            }
            const char* kw = keyword->string_val;
            shader->keyword_names.keywords[i] = (char*)mem_alloc(strlen(kw) + 1);
            if (!shader->keyword_names.keywords[i]) {
                serialized_shader_free(shader);
                return false;
            }
            strcpy(shader->keyword_names.keywords[i], kw);
        }
    }
    
    const TypeTreeValue* kw_flags = typetree_find_child(parsed_form, "m_KeywordFlags");
    const TypeTreeValue* kwf_arr = NULL;
    if (!profile_optional_array(profile, kw_flags, false, &kwf_arr) ||
        (kwf_arr &&
         kwf_arr->array_val.count != shader->keyword_names.count)) {
        serialized_shader_free(shader);
        return false;
    }
    if (kwf_arr) {
        if (!allocate_serialized_array((void**)&shader->keyword_flags,
                                       shader->keyword_names.count,
                                       sizeof(*shader->keyword_flags))) {
            serialized_shader_free(shader);
            return false;
        }
        for (int i = 0; i < shader->keyword_names.count; i++) {
            int64_t flag = 0;
            if (!typetree_array_get_int(kwf_arr, i, &flag) || flag < 0 ||
                flag > UINT8_MAX) {
                serialized_shader_free(shader);
                return false;
            }
            shader->keyword_flags[i] = (uint8_t)flag;
        }
    }
    
    if (!parse_properties(parsed_form, shader, profile)) {
        serialized_shader_free(shader);
        return false;
    }
    
    const TypeTreeValue* subshaders_field = typetree_find_child(parsed_form, "m_SubShaders");
    const TypeTreeValue* sub_arr = NULL;
    if (!profile_optional_array(profile, subshaders_field, true, &sub_arr)) {
        serialized_shader_free(shader);
        return false;
    }
    if (sub_arr) {
        shader->subshader_count = sub_arr->array_val.count;
        if (!allocate_serialized_array((void**)&shader->subshaders,
                                       shader->subshader_count,
                                       sizeof(*shader->subshaders))) {
            serialized_shader_free(shader);
            return false;
        }
        
        for (int i = 0; i < shader->subshader_count; i++) {
            const TypeTreeValue* sub_val = &sub_arr->array_val.elements[i];
            if (sub_val->type != VAL_TYPE_STRUCT) {
                serialized_shader_free(shader);
                return false;
            }
            SerializedSubShader* subshader = &shader->subshaders[i];
            
            if (!profile_get_int32(profile, sub_val, "m_LOD",
                                   SHADER_FIELD_OPTIONAL,
                                   &subshader->lod) ||
                !parse_tags(sub_val, SERIALIZED_TAGS_DIRECT,
                            &subshader->tags,
                            &shader->owned_strings, profile)) {
                serialized_shader_free(shader);
                return false;
            }
            
            const TypeTreeValue* passes_field = typetree_find_child(sub_val, "m_Passes");
            const TypeTreeValue* pass_arr = NULL;
            if (!profile_optional_array(profile, passes_field, true,
                                        &pass_arr)) {
                serialized_shader_free(shader);
                return false;
            }
            if (pass_arr) {
                subshader->pass_count = pass_arr->array_val.count;
                if (!allocate_serialized_array((void**)&subshader->passes,
                                               subshader->pass_count,
                                               sizeof(*subshader->passes))) {
                    serialized_shader_free(shader);
                    return false;
                }
                
                for (int j = 0; j < subshader->pass_count; j++) {
                    const TypeTreeValue* pass_val = &pass_arr->array_val.elements[j];
                    if (pass_val->type != VAL_TYPE_STRUCT) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    SerializedPass* pass = &subshader->passes[j];

                    const TypeTreeValue* pass_platforms =
                        typetree_find_child(pass_val, "m_Platforms");
                    pass->has_serialized_platforms = pass_platforms != NULL;
                    if (!copy_int_array(pass_platforms,
                                        &pass->platform_count,
                                        &pass->platforms, profile)) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    if (!profile_get_uint32(profile, pass_val,
                                            "m_ProgramMask",
                                            SHADER_FIELD_REQUIRED,
                                            &pass->program_mask)) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    if (!copy_u16_array(
                            typetree_find_child(
                                pass_val,
                                "m_SerializedKeywordStateMask"),
                            &pass->serialized_keyword_state_mask_count,
                            &pass->serialized_keyword_state_mask,
                            profile)) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    
                    const char* use_name = NULL;
                    const char* pass_name = NULL;
                    const char* texture_name = NULL;
                    if (!profile_get_int32(profile, pass_val, "m_Type",
                                           SHADER_FIELD_REQUIRED,
                                           &pass->pass_type) ||
                        !profile_get_string(profile, pass_val, "m_UseName",
                                            SHADER_FIELD_OPTIONAL,
                                            &use_name) ||
                        !profile_get_string(profile, pass_val, "m_Name",
                                            SHADER_FIELD_OPTIONAL,
                                            &pass_name) ||
                        !profile_get_string(profile, pass_val,
                                            "m_TextureName",
                                            SHADER_FIELD_OPTIONAL,
                                            &texture_name) ||
                        !own_string_exact(&shader->owned_strings,
                                          &pass->use_name, use_name) ||
                        !own_string_exact(&shader->owned_strings,
                                          &pass->name, pass_name) ||
                        !own_string_exact(&shader->owned_strings,
                                          &pass->texture_name,
                                          texture_name)) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    
                    if (!parse_name_table(pass_val, &pass->name_table,
                                          &shader->owned_strings,
                                          profile)) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    
                    const TypeTreeValue* state_field = typetree_find_child(pass_val, "m_State");
                    if (state_field) {
                        if (state_field->type != VAL_TYPE_STRUCT) {
                            serialized_shader_free(shader);
                            return false;
                        }
                        if (!parse_render_state(state_field, &pass->state,
                                                &shader->owned_strings,
                                                profile)) {
                            serialized_shader_free(shader);
                            return false;
                        }
                    }
                    const SerializedTagSource pass_tag_source =
                        pass->pass_type == 0
                            ? SERIALIZED_TAGS_RENDER_STATE
                            : SERIALIZED_TAGS_DIRECT;
                    if (!parse_tags(pass_val, pass_tag_source, &pass->tags,
                                    &shader->owned_strings, profile)) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    
                    if (!profile_get_bool(
                            profile, pass_val, "m_HasInstancingVariant",
                            SHADER_FIELD_OPTIONAL,
                            &pass->has_instancing_variant) ||
                        !profile_get_bool(
                            profile, pass_val,
                            "m_HasProceduralInstancingVariant",
                            SHADER_FIELD_OPTIONAL,
                            &pass->has_procedural_instancing_variant)) {
                        serialized_shader_free(shader);
                        return false;
                    }
                    
                    const char* prog_names[6] = { "progVertex", "progFragment", "progGeometry", "progHull", "progDomain", "progRayTracing" };
                    for (int s = 0; s < 6; s++) {
                        const TypeTreeValue* prog_struct = typetree_find_child(pass_val, prog_names[s]);
                        if (prog_struct) {
                            if (prog_struct->type != VAL_TYPE_STRUCT) {
                                serialized_shader_free(shader);
                                return false;
                            }
                            if (!serialized_program_parameters_parse_typetree(
                                    &pass->common_parameters[s], prog_struct,
                                    &pass->name_table)) {
                                serialized_shader_free(shader);
                                return false;
                            }
                            if (!parse_subprograms_for_stage(
                                    prog_struct, pass, s, shader, profile)) {
                                serialized_shader_free(shader);
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    
    return true;
}

bool serialized_shader_parse_with_profile(
    SerializedShader* shader, const TypeTreeValue* value,
    SerializedShaderSchemaProfile profile) {
    if (!shader) return false;
    if (!serialized_shader_profile_validate_value(value, profile)) {
        return false;
    }
    SerializedShader parsed;
    serialized_shader_init(&parsed);
    if (!serialized_shader_parse_with_profile_impl(&parsed, value, profile)) {
        serialized_shader_free(&parsed);
        return false;
    }
    serialized_shader_free(shader);
    *shader = parsed;
    return true;
}

bool serialized_shader_parse(SerializedShader* shader,
                             const TypeTreeValue* value) {
    if (!shader) return false;
    SerializedShader parsed;
    serialized_shader_init(&parsed);
    if (!serialized_shader_parse_with_profile_impl(
            &parsed, value,
            SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1)) {
        serialized_shader_free(&parsed);
        return false;
    }
    serialized_shader_free(shader);
    *shader = parsed;
    return true;
}

static bool is_serialized_console_program(int program_type) {
    return program_type >= 26 && program_type <= 30;
}

bool serialized_shader_platform_is_known(int platform) {
    /* Exact ShaderCompilerPlatform values present in Unity 2021.3's table.
     * Removed/reserved holes are not neighboring-version compatibility. */
    switch (platform) {
        case 0:  /* OpenGL (legacy) */
        case 1:  /* D3D9 */
        case 2:  /* Xbox 360 */
        case 3:  /* PS3 */
        case 4:  /* D3D11/D3D12 desktop compiler family */
        case 5:  /* GLES 2.0 */
        case 8:  /* D3D 11 level 9.x */
        case 9:  /* GLES 3+ */
        case 10: /* PSP2 */
        case 11: /* PS4 */
        case 12: /* Xbox One */
        case 14: /* Metal */
        case 15: /* OpenGL Core */
        case 16: /* N3DS */
        case 17: /* Wii U */
        case 18: /* Vulkan */
        case 19: /* Switch */
        case 20: /* Xbox One D3D12 */
            return true;
        default:
            return false;
    }
}

bool serialized_gpu_program_type_is_platform(int program_type, int platform) {
    /* Values are pinned to Unity 2021.3's ShaderCompilerPlatform and
     * UnityShaderCompilerExtGPUProgramType tables. Keep this exhaustive and
     * fail closed: treating an unknown record as D3D11 corrupts variant
     * identity before DXBC parsing even begins. */
    if (!serialized_shader_platform_is_known(platform)) return false;
    switch (platform) {
        case 0:  /* OpenGL (legacy) */
            return program_type == 1;
        case 1:  /* D3D9 */
            return program_type >= 9 && program_type <= 12;
        case 2:  /* Xbox 360 */
        case 3:  /* PS3 */
        case 10: /* PSP2 */
        case 11: /* PS4 */
        case 12: /* Xbox One */
        case 16: /* N3DS */
        case 17: /* Wii U */
        case 19: /* Switch */
            return is_serialized_console_program(program_type);
        case 4:  /* D3D11/D3D12 desktop compiler family */
            return (program_type >= 15 && program_type <= 22) ||
                   program_type == 31;
        case 5:  /* GLES 2.0 */
            return program_type == 5;
        case 8:  /* D3D 11 level 9.x */
            return program_type == 13 || program_type == 14;
        case 9:  /* GLES 3+ */
            return program_type >= 2 && program_type <= 4;
        case 14: /* Metal */
            return program_type == 23 || program_type == 24;
        case 15: /* OpenGL Core */
            return program_type >= 6 && program_type <= 8;
        case 18: /* Vulkan */
            return program_type == 25;
        case 20: /* Xbox One D3D12 */
            return is_serialized_console_program(program_type) ||
                   program_type == 31;
        default:
            return false;
    }
}

bool serialized_pass_subprogram_is_platform(const SerializedPass* pass,
                                            int stage, int subprogram_index,
                                            int platform) {
    if (!pass || stage < 0 || stage >= 6 || subprogram_index < 0 ||
        subprogram_index >= pass->subprogram_count[stage]) {
        return false;
    }
    if (!pass->has_serialized_platforms) {
        return pass->subprogram_identities[stage] == NULL &&
               pass->platform_count == 0;
    }
    bool pass_has_platform = false;
    for (int i = 0; i < pass->platform_count; i++) {
        if (pass->platforms[i] == platform) {
            pass_has_platform = true;
            break;
        }
    }
    if (!pass_has_platform || !pass->subprograms[stage]) return false;
    return serialized_gpu_program_type_is_platform(
        pass->subprograms[stage][subprogram_index].program_type, platform);
}

void serialized_shader_free(SerializedShader* shader) {
    if (shader->properties) {
        for (int i = 0; i < shader->property_count; i++) {
            ParsedShaderProperty* prop = &shader->properties[i];
            if (prop->attributes) {
                for (int j = 0; j < prop->attribute_count; j++) {
                    if (prop->attributes[j]) mem_free(prop->attributes[j], strlen(prop->attributes[j]) + 1);
                }
                mem_free(prop->attributes, prop->attribute_count * sizeof(char*));
            }
        }
        mem_free(shader->properties, shader->property_count * sizeof(ParsedShaderProperty));
        shader->properties = NULL;
    }
    if (shader->subshaders) {
        for (int i = 0; i < shader->subshader_count; i++) {
            SerializedSubShader* sub = &shader->subshaders[i];
            if (sub->tags.tags) {
                mem_free(sub->tags.tags, sub->tags.tag_count * sizeof(SerializedTag));
            }
            if (sub->passes) {
                for (int j = 0; j < sub->pass_count; j++) {
                    SerializedPass* pass = &sub->passes[j];
                    if (pass->platforms) {
                        mem_free(pass->platforms, pass->platform_count * sizeof(int));
                    }
                    if (pass->name_table.entries) {
                        mem_free(pass->name_table.entries, pass->name_table.count * sizeof(SerializedNameTableEntry));
                    }
                    if (pass->tags.tags) {
                        mem_free(pass->tags.tags, pass->tags.tag_count * sizeof(SerializedTag));
                    }
                    if (pass->serialized_keyword_state_mask) {
                        mem_free(pass->serialized_keyword_state_mask,
                                 (size_t)pass->serialized_keyword_state_mask_count *
                                     sizeof(uint16_t));
                    }
                    for (int s = 0; s < 6; s++) {
                        serialized_program_parameters_free(
                            &pass->common_parameters[s]);
                        if (pass->subprograms[s]) {
                            for (int k = 0; k < pass->subprogram_count[s]; k++) {
                                serialized_subprogram_free(
                                    &pass->subprograms[s][k]);
                            }
                            mem_free(pass->subprograms[s], pass->subprogram_count[s] * sizeof(SerializedSubProgram));
                        }
                        if (pass->subprogram_identities[s]) {
                            for (int k = 0; k < pass->subprogram_count[s]; k++) {
                                SerializedSubProgramIdentity* identity =
                                    &pass->subprogram_identities[s][k];
                                if (identity->global_keyword_indices) {
                                    mem_free(identity->global_keyword_indices,
                                             (size_t)identity->global_keyword_index_count *
                                                 sizeof(int));
                                }
                                if (identity->local_keyword_indices) {
                                    mem_free(identity->local_keyword_indices,
                                             (size_t)identity->local_keyword_index_count *
                                                 sizeof(int));
                                }
                            }
                            mem_free(pass->subprogram_identities[s],
                                     (size_t)pass->subprogram_count[s] *
                                         sizeof(SerializedSubProgramIdentity));
                        }
                        if (pass->subprogram_param_blob_indices[s]) {
                            mem_free(pass->subprogram_param_blob_indices[s], pass->subprogram_count[s] * sizeof(int));
                        }
                    }
                }
                mem_free(sub->passes, sub->pass_count * sizeof(SerializedPass));
            }
        }
        mem_free(shader->subshaders, shader->subshader_count * sizeof(SerializedSubShader));
        shader->subshaders = NULL;
    }
    if (shader->keyword_names.keywords) {
        for (int i = 0; i < shader->keyword_names.count; i++) {
            if (shader->keyword_names.keywords[i]) {
                mem_free(shader->keyword_names.keywords[i], strlen(shader->keyword_names.keywords[i]) + 1);
            }
        }
        mem_free(shader->keyword_names.keywords, shader->keyword_names.count * sizeof(char*));
        shader->keyword_names.keywords = NULL;
    }
    if (shader->keyword_flags) {
        mem_free(shader->keyword_flags, shader->keyword_names.count * sizeof(uint8_t));
        shader->keyword_flags = NULL;
    }
    if (shader->dependencies) {
        mem_free(shader->dependencies,
                 (size_t)shader->dependency_count *
                     sizeof(*shader->dependencies));
        shader->dependencies = NULL;
        shader->dependency_count = 0;
    }
    if (shader->custom_editors_for_render_pipelines) {
        mem_free(shader->custom_editors_for_render_pipelines,
                 (size_t)shader->custom_editor_for_render_pipeline_count *
                     sizeof(*shader->custom_editors_for_render_pipelines));
        shader->custom_editors_for_render_pipelines = NULL;
        shader->custom_editor_for_render_pipeline_count = 0;
    }
    if (shader->archive_platforms) {
        mem_free(shader->archive_platforms,
                 (size_t)shader->archive_platform_count * sizeof(int));
        shader->archive_platforms = NULL;
        shader->archive_platform_count = 0;
    }
    serialized_string_pool_dispose(&shader->owned_strings);
    memset(shader, 0, sizeof(*shader));
}
