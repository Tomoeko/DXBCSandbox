// SPDX-License-Identifier: GPL-3.0-only

#include "io/serialized_shader_profile.h"

#include <string.h>

typedef struct {
    const char* name;
    ValueType value_type;
    const char* wire_type;
} ValueFieldShape;

#define FIELD(name_, value_type_, wire_type_) \
    { (name_), (value_type_), (wire_type_) }
#define ARRAY_COUNT(values_) \
    (sizeof(values_) / sizeof((values_)[0]))

static bool validate_program(const TypeTreeValue* value);
static bool validate_parameters(const TypeTreeValue* value);

static bool value_has_shape(const TypeTreeValue* value,
                            const ValueFieldShape* shape) {
    if (!value || !shape || !value->name || !value->type_str ||
        strcmp(value->name, shape->name) != 0 ||
        value->type != shape->value_type ||
        strcmp(value->type_str, shape->wire_type) != 0) {
        return false;
    }
    if (value->type == VAL_TYPE_STRING) {
        return value->string_val &&
               strlen(value->string_val) == value->string_length;
    }
    if (value->type == VAL_TYPE_ARRAY) {
        if (value->array_val.count < 0) return false;
        if (value->array_val.count == 0) return true;
        if (value->array_val.storage == TYPETREE_ARRAY_VALUES) {
            return value->array_val.elements != NULL;
        }
        if (value->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
            return value->array_val.packed_bytes != NULL;
        }
        return false;
    }
    if (value->type == VAL_TYPE_STRUCT) {
        return value->struct_val.count >= 0 &&
               (value->struct_val.count == 0 ||
                value->struct_val.members != NULL);
    }
    return true;
}

static bool validate_exact_struct(const TypeTreeValue* value,
                                  const char* wire_type,
                                  const ValueFieldShape* fields,
                                  size_t field_count) {
    if (!value || value->type != VAL_TYPE_STRUCT || !value->type_str ||
        strcmp(value->type_str, wire_type) != 0 ||
        value->struct_val.count < 0 ||
        (size_t)value->struct_val.count != field_count ||
        (field_count != 0U && !value->struct_val.members)) {
        LOG_ERROR("Unity Shader shape: expected %s struct with %zu fields, "
                  "got type=%s value-kind=%d fields=%d",
                  wire_type, field_count,
                  value && value->type_str ? value->type_str : "<missing>",
                  value ? (int)value->type : -1,
                  value && value->type == VAL_TYPE_STRUCT
                      ? value->struct_val.count : -1);
        return false;
    }
    for (size_t index = 0; index < field_count; ++index) {
        if (!value_has_shape(&value->struct_val.members[index],
                             &fields[index])) {
            const TypeTreeValue* actual = &value->struct_val.members[index];
            LOG_ERROR("Unity Shader shape: %s field %zu expected %s/%s/%d, "
                      "got %s/%s/%d", wire_type, index, fields[index].name,
                      fields[index].wire_type, (int)fields[index].value_type,
                      actual->name ? actual->name : "<missing>",
                      actual->type_str ? actual->type_str : "<missing>",
                      (int)actual->type);
            return false;
        }
    }
    return true;
}

static const TypeTreeValue* struct_member(const TypeTreeValue* value,
                                          size_t index) {
    if (!value || value->type != VAL_TYPE_STRUCT || index >=
        (size_t)value->struct_val.count) {
        return NULL;
    }
    return &value->struct_val.members[index];
}

static bool array_values_are_valid(const TypeTreeValue* value) {
    return value && value->type == VAL_TYPE_ARRAY &&
           value->array_val.count >= 0 &&
           (value->array_val.count == 0 ||
            (value->array_val.storage == TYPETREE_ARRAY_VALUES &&
             value->array_val.elements != NULL));
}

static bool array_is_valid(const TypeTreeValue* value) {
    if (!value || value->type != VAL_TYPE_ARRAY ||
        value->array_val.count < 0) {
        return false;
    }
    if (value->array_val.count == 0) return true;
    if (value->array_val.storage == TYPETREE_ARRAY_VALUES) {
        return value->array_val.elements != NULL;
    }
    return value->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES &&
           value->array_val.packed_bytes != NULL;
}

static bool validate_string_pair_array(const TypeTreeValue* value) {
    static const ValueFieldShape pair_fields[] = {
        FIELD("first", VAL_TYPE_STRING, "string"),
        FIELD("second", VAL_TYPE_STRING, "string"),
    };
    if (!array_values_are_valid(value)) return false;
    for (int index = 0; index < value->array_val.count; ++index) {
        const TypeTreeValue* pair = &value->array_val.elements[index];
        if (!pair->name || strcmp(pair->name, "data") != 0 ||
            !validate_exact_struct(pair, "pair", pair_fields,
                                   ARRAY_COUNT(pair_fields))) {
            return false;
        }
    }
    return true;
}

static bool validate_tags(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("tags", VAL_TYPE_ARRAY, "map"),
    };
    return validate_exact_struct(value, "SerializedTagMap", fields,
                                 ARRAY_COUNT(fields)) &&
           validate_string_pair_array(struct_member(value, 0U));
}

static bool validate_float_value(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("val", VAL_TYPE_FLOAT, "float"),
        FIELD("name", VAL_TYPE_STRING, "string"),
    };
    return validate_exact_struct(value, "SerializedShaderFloatValue",
                                 fields, ARRAY_COUNT(fields));
}

static bool validate_rt_blend(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("srcBlend", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("destBlend", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("srcBlendAlpha", VAL_TYPE_STRUCT,
              "SerializedShaderFloatValue"),
        FIELD("destBlendAlpha", VAL_TYPE_STRUCT,
              "SerializedShaderFloatValue"),
        FIELD("blendOp", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("blendOpAlpha", VAL_TYPE_STRUCT,
              "SerializedShaderFloatValue"),
        FIELD("colMask", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
    };
    if (!validate_exact_struct(value, "SerializedShaderRTBlendState",
                               fields, ARRAY_COUNT(fields))) {
        return false;
    }
    for (size_t index = 0; index < ARRAY_COUNT(fields); ++index) {
        if (!validate_float_value(struct_member(value, index))) return false;
    }
    return true;
}

static bool validate_stencil(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("pass", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("fail", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("zFail", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("comp", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
    };
    if (!validate_exact_struct(value, "SerializedStencilOp", fields,
                               ARRAY_COUNT(fields))) {
        return false;
    }
    for (size_t index = 0; index < ARRAY_COUNT(fields); ++index) {
        if (!validate_float_value(struct_member(value, index))) return false;
    }
    return true;
}

static bool validate_fog_color(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("x", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("y", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("z", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("w", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("name", VAL_TYPE_STRING, "string"),
    };
    if (!validate_exact_struct(value, "SerializedShaderVectorValue", fields,
                               ARRAY_COUNT(fields))) {
        return false;
    }
    for (size_t index = 0; index < 4U; ++index) {
        if (!validate_float_value(struct_member(value, index))) return false;
    }
    return true;
}

static bool validate_render_state(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_Name", VAL_TYPE_STRING, "string"),
        FIELD("rtBlend0", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtBlend1", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtBlend2", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtBlend3", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtBlend4", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtBlend5", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtBlend6", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtBlend7", VAL_TYPE_STRUCT, "SerializedShaderRTBlendState"),
        FIELD("rtSeparateBlend", VAL_TYPE_INT, "bool"),
        FIELD("zClip", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("zTest", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("zWrite", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("culling", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("conservative", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("offsetFactor", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("offsetUnits", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("alphaToMask", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("stencilOp", VAL_TYPE_STRUCT, "SerializedStencilOp"),
        FIELD("stencilOpFront", VAL_TYPE_STRUCT, "SerializedStencilOp"),
        FIELD("stencilOpBack", VAL_TYPE_STRUCT, "SerializedStencilOp"),
        FIELD("stencilReadMask", VAL_TYPE_STRUCT,
              "SerializedShaderFloatValue"),
        FIELD("stencilWriteMask", VAL_TYPE_STRUCT,
              "SerializedShaderFloatValue"),
        FIELD("stencilRef", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("fogStart", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("fogEnd", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("fogDensity", VAL_TYPE_STRUCT, "SerializedShaderFloatValue"),
        FIELD("fogColor", VAL_TYPE_STRUCT, "SerializedShaderVectorValue"),
        FIELD("fogMode", VAL_TYPE_INT, "int"),
        FIELD("gpuProgramID", VAL_TYPE_INT, "int"),
        FIELD("m_Tags", VAL_TYPE_STRUCT, "SerializedTagMap"),
        FIELD("m_LOD", VAL_TYPE_INT, "int"),
        FIELD("lighting", VAL_TYPE_INT, "bool"),
    };
    if (!validate_exact_struct(value, "SerializedShaderState", fields,
                               ARRAY_COUNT(fields))) {
        return false;
    }
    for (size_t index = 1U; index <= 8U; ++index) {
        if (!validate_rt_blend(struct_member(value, index))) return false;
    }
    for (size_t index = 10U; index <= 17U; ++index) {
        if (!validate_float_value(struct_member(value, index))) return false;
    }
    for (size_t index = 18U; index <= 20U; ++index) {
        if (!validate_stencil(struct_member(value, index))) return false;
    }
    for (size_t index = 21U; index <= 26U; ++index) {
        if (!validate_float_value(struct_member(value, index))) return false;
    }
    return validate_fog_color(struct_member(value, 27U)) &&
           validate_tags(struct_member(value, 30U));
}

typedef enum {
    PARAM_VECTOR,
    PARAM_MATRIX,
    PARAM_TEXTURE,
    PARAM_BUFFER,
    PARAM_CONSTANT_BUFFER,
    PARAM_UAV,
    PARAM_SAMPLER,
    PARAM_STRUCT
} ParameterKind;

static bool validate_parameter(const TypeTreeValue* value,
                               ParameterKind kind);

static bool validate_parameter_array(const TypeTreeValue* value,
                                     ParameterKind kind) {
    if (!array_values_are_valid(value)) return false;
    for (int index = 0; index < value->array_val.count; ++index) {
        if (!validate_parameter(&value->array_val.elements[index], kind)) {
            return false;
        }
    }
    return true;
}

static bool validate_parameter(const TypeTreeValue* value,
                               ParameterKind kind) {
    static const ValueFieldShape vector_fields[] = {
        FIELD("m_NameIndex", VAL_TYPE_INT, "int"),
        FIELD("m_Index", VAL_TYPE_INT, "int"),
        FIELD("m_ArraySize", VAL_TYPE_INT, "int"),
        FIELD("m_Type", VAL_TYPE_INT, "SInt8"),
        FIELD("m_Dim", VAL_TYPE_INT, "SInt8"),
    };
    static const ValueFieldShape matrix_fields[] = {
        FIELD("m_NameIndex", VAL_TYPE_INT, "int"),
        FIELD("m_Index", VAL_TYPE_INT, "int"),
        FIELD("m_ArraySize", VAL_TYPE_INT, "int"),
        FIELD("m_Type", VAL_TYPE_INT, "SInt8"),
        FIELD("m_RowCount", VAL_TYPE_INT, "SInt8"),
    };
    static const ValueFieldShape texture_fields[] = {
        FIELD("m_NameIndex", VAL_TYPE_INT, "int"),
        FIELD("m_Index", VAL_TYPE_INT, "int"),
        FIELD("m_SamplerIndex", VAL_TYPE_INT, "int"),
        FIELD("m_MultiSampled", VAL_TYPE_INT, "bool"),
        FIELD("m_Dim", VAL_TYPE_INT, "SInt8"),
    };
    static const ValueFieldShape buffer_fields[] = {
        FIELD("m_NameIndex", VAL_TYPE_INT, "int"),
        FIELD("m_Index", VAL_TYPE_INT, "int"),
        FIELD("m_ArraySize", VAL_TYPE_INT, "int"),
    };
    static const ValueFieldShape uav_fields[] = {
        FIELD("m_NameIndex", VAL_TYPE_INT, "int"),
        FIELD("m_Index", VAL_TYPE_INT, "int"),
        FIELD("m_OriginalIndex", VAL_TYPE_INT, "int"),
    };
    static const ValueFieldShape sampler_fields[] = {
        FIELD("sampler", VAL_TYPE_INT, "unsigned int"),
        FIELD("bindPoint", VAL_TYPE_INT, "int"),
    };
    static const ValueFieldShape constant_buffer_fields[] = {
        FIELD("m_NameIndex", VAL_TYPE_INT, "int"),
        FIELD("m_MatrixParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_VectorParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_StructParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_Size", VAL_TYPE_INT, "int"),
        FIELD("m_IsPartialCB", VAL_TYPE_INT, "bool"),
    };
    static const ValueFieldShape struct_fields[] = {
        FIELD("m_NameIndex", VAL_TYPE_INT, "int"),
        FIELD("m_Index", VAL_TYPE_INT, "int"),
        FIELD("m_ArraySize", VAL_TYPE_INT, "int"),
        FIELD("m_StructSize", VAL_TYPE_INT, "int"),
        FIELD("m_VectorMembers", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_MatrixMembers", VAL_TYPE_ARRAY, "vector"),
    };

    const ValueFieldShape* fields = NULL;
    size_t count = 0U;
    const char* wire_type = NULL;
    switch (kind) {
        case PARAM_VECTOR:
            fields = vector_fields; count = ARRAY_COUNT(vector_fields);
            wire_type = "VectorParameter"; break;
        case PARAM_MATRIX:
            fields = matrix_fields; count = ARRAY_COUNT(matrix_fields);
            wire_type = "MatrixParameter"; break;
        case PARAM_TEXTURE:
            fields = texture_fields; count = ARRAY_COUNT(texture_fields);
            wire_type = "TextureParameter"; break;
        case PARAM_BUFFER:
            fields = buffer_fields; count = ARRAY_COUNT(buffer_fields);
            wire_type = "BufferBinding"; break;
        case PARAM_UAV:
            fields = uav_fields; count = ARRAY_COUNT(uav_fields);
            wire_type = "UAVParameter"; break;
        case PARAM_SAMPLER:
            fields = sampler_fields; count = ARRAY_COUNT(sampler_fields);
            wire_type = "SamplerParameter"; break;
        case PARAM_CONSTANT_BUFFER:
            fields = constant_buffer_fields;
            count = ARRAY_COUNT(constant_buffer_fields);
            wire_type = "ConstantBuffer"; break;
        case PARAM_STRUCT:
            fields = struct_fields; count = ARRAY_COUNT(struct_fields);
            wire_type = "StructParameter"; break;
    }
    if (!validate_exact_struct(value, wire_type, fields, count)) return false;
    if (kind == PARAM_CONSTANT_BUFFER) {
        return validate_parameter_array(struct_member(value, 1U),
                                        PARAM_MATRIX) &&
               validate_parameter_array(struct_member(value, 2U),
                                        PARAM_VECTOR) &&
               validate_parameter_array(struct_member(value, 3U),
                                        PARAM_STRUCT);
    }
    if (kind == PARAM_STRUCT) {
        return validate_parameter_array(struct_member(value, 4U),
                                        PARAM_VECTOR) &&
               validate_parameter_array(struct_member(value, 5U),
                                        PARAM_MATRIX);
    }
    return true;
}

static bool validate_parameters(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_VectorParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_MatrixParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_TextureParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_BufferParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_ConstantBuffers", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_ConstantBufferBindings", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_UAVParams", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_Samplers", VAL_TYPE_ARRAY, "vector"),
    };
    return validate_exact_struct(value, "SerializedProgramParameters", fields,
                                 ARRAY_COUNT(fields)) &&
           validate_parameter_array(struct_member(value, 0U), PARAM_VECTOR) &&
           validate_parameter_array(struct_member(value, 1U), PARAM_MATRIX) &&
           validate_parameter_array(struct_member(value, 2U), PARAM_TEXTURE) &&
           validate_parameter_array(struct_member(value, 3U), PARAM_BUFFER) &&
           validate_parameter_array(struct_member(value, 4U),
                                    PARAM_CONSTANT_BUFFER) &&
           validate_parameter_array(struct_member(value, 5U), PARAM_BUFFER) &&
           validate_parameter_array(struct_member(value, 6U), PARAM_UAV) &&
           validate_parameter_array(struct_member(value, 7U), PARAM_SAMPLER);
}

static bool validate_player_subprogram(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_BlobIndex", VAL_TYPE_INT, "unsigned int"),
        FIELD("m_KeywordIndices", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_ShaderRequirements", VAL_TYPE_INT, "SInt64"),
        FIELD("m_GpuProgramType", VAL_TYPE_INT, "SInt8"),
    };
    return validate_exact_struct(value, "SerializedPlayerSubProgram", fields,
                                 ARRAY_COUNT(fields)) &&
           array_values_are_valid(struct_member(value, 1U));
}

static bool validate_player_groups(const TypeTreeValue* value,
                                   const TypeTreeValue* parameter_groups) {
    if (!array_values_are_valid(value) ||
        !array_values_are_valid(parameter_groups) ||
        value->array_val.count != parameter_groups->array_val.count) {
        return false;
    }
    for (int group = 0; group < value->array_val.count; ++group) {
        const TypeTreeValue* programs = &value->array_val.elements[group];
        const TypeTreeValue* indices =
            &parameter_groups->array_val.elements[group];
        static const ValueFieldShape group_shape =
            FIELD("data", VAL_TYPE_ARRAY, "vector");
        if (!value_has_shape(programs, &group_shape) ||
            !value_has_shape(indices, &group_shape) ||
            !array_values_are_valid(programs) ||
            !array_values_are_valid(indices) ||
            programs->array_val.count != indices->array_val.count) {
            return false;
        }
        for (int index = 0; index < programs->array_val.count; ++index) {
            if (!validate_player_subprogram(
                    &programs->array_val.elements[index])) {
                return false;
            }
            const TypeTreeValue* parameter = &indices->array_val.elements[index];
            static const ValueFieldShape parameter_shape =
                FIELD("data", VAL_TYPE_INT, "unsigned int");
            if (!value_has_shape(parameter, &parameter_shape)) return false;
        }
    }
    return true;
}

static bool validate_program(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_SubPrograms", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_PlayerSubPrograms", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_ParameterBlobIndices", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_CommonParameters", VAL_TYPE_STRUCT,
              "SerializedProgramParameters"),
    };
    if (!validate_exact_struct(value, "SerializedProgram", fields,
                               ARRAY_COUNT(fields)) ||
        !array_values_are_valid(struct_member(value, 0U)) ||
        !validate_player_groups(struct_member(value, 1U),
                                struct_member(value, 2U)) ||
        !validate_parameters(struct_member(value, 3U))) {
        return false;
    }
    return true;
}

static bool validate_name_indices(const TypeTreeValue* value) {
    static const ValueFieldShape pair_fields[] = {
        FIELD("first", VAL_TYPE_STRING, "string"),
        FIELD("second", VAL_TYPE_INT, "int"),
    };
    if (!array_values_are_valid(value)) return false;
    for (int index = 0; index < value->array_val.count; ++index) {
        const TypeTreeValue* pair = &value->array_val.elements[index];
        if (!pair->name || strcmp(pair->name, "data") != 0 ||
            !validate_exact_struct(pair, "pair", pair_fields,
                                   ARRAY_COUNT(pair_fields))) {
            return false;
        }
    }
    return true;
}

static bool validate_pass(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_EditorDataHash", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_Platforms", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_NameIndices", VAL_TYPE_ARRAY, "map"),
        FIELD("m_Type", VAL_TYPE_INT, "int"),
        FIELD("m_State", VAL_TYPE_STRUCT, "SerializedShaderState"),
        FIELD("m_ProgramMask", VAL_TYPE_INT, "unsigned int"),
        FIELD("progVertex", VAL_TYPE_STRUCT, "SerializedProgram"),
        FIELD("progFragment", VAL_TYPE_STRUCT, "SerializedProgram"),
        FIELD("progGeometry", VAL_TYPE_STRUCT, "SerializedProgram"),
        FIELD("progHull", VAL_TYPE_STRUCT, "SerializedProgram"),
        FIELD("progDomain", VAL_TYPE_STRUCT, "SerializedProgram"),
        FIELD("progRayTracing", VAL_TYPE_STRUCT, "SerializedProgram"),
        FIELD("m_HasInstancingVariant", VAL_TYPE_INT, "bool"),
        FIELD("m_HasProceduralInstancingVariant", VAL_TYPE_INT, "bool"),
        FIELD("m_UseName", VAL_TYPE_STRING, "string"),
        FIELD("m_Name", VAL_TYPE_STRING, "string"),
        FIELD("m_TextureName", VAL_TYPE_STRING, "string"),
        FIELD("m_Tags", VAL_TYPE_STRUCT, "SerializedTagMap"),
        FIELD("m_SerializedKeywordStateMask", VAL_TYPE_ARRAY, "vector"),
    };
    if (!validate_exact_struct(value, "SerializedPass", fields,
                               ARRAY_COUNT(fields)) ||
        !array_values_are_valid(struct_member(value, 0U)) ||
        !array_is_valid(struct_member(value, 1U)) ||
        !validate_name_indices(struct_member(value, 2U)) ||
        !validate_render_state(struct_member(value, 4U))) {
        return false;
    }
    for (size_t index = 6U; index <= 11U; ++index) {
        if (!validate_program(struct_member(value, index))) return false;
    }
    return validate_tags(struct_member(value, 17U)) &&
           array_values_are_valid(struct_member(value, 18U));
}

static bool validate_subshader(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_Passes", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_Tags", VAL_TYPE_STRUCT, "SerializedTagMap"),
        FIELD("m_LOD", VAL_TYPE_INT, "int"),
    };
    if (!validate_exact_struct(value, "SerializedSubShader", fields,
                               ARRAY_COUNT(fields))) {
        return false;
    }
    const TypeTreeValue* passes = struct_member(value, 0U);
    if (!array_values_are_valid(passes) ||
        !validate_tags(struct_member(value, 1U))) {
        return false;
    }
    for (int index = 0; index < passes->array_val.count; ++index) {
        if (!validate_pass(&passes->array_val.elements[index])) return false;
    }
    return true;
}

static bool validate_property(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_Name", VAL_TYPE_STRING, "string"),
        FIELD("m_Description", VAL_TYPE_STRING, "string"),
        FIELD("m_Attributes", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_Type", VAL_TYPE_INT, "int"),
        FIELD("m_Flags", VAL_TYPE_INT, "unsigned int"),
        FIELD("m_DefValue[0]", VAL_TYPE_FLOAT, "float"),
        FIELD("m_DefValue[1]", VAL_TYPE_FLOAT, "float"),
        FIELD("m_DefValue[2]", VAL_TYPE_FLOAT, "float"),
        FIELD("m_DefValue[3]", VAL_TYPE_FLOAT, "float"),
        FIELD("m_DefTexture", VAL_TYPE_STRUCT, "SerializedTextureProperty"),
    };
    static const ValueFieldShape texture_fields[] = {
        FIELD("m_DefaultName", VAL_TYPE_STRING, "string"),
        FIELD("m_TexDim", VAL_TYPE_INT, "int"),
    };
    if (!validate_exact_struct(value, "SerializedProperty", fields,
                               ARRAY_COUNT(fields))) {
        return false;
    }
    const TypeTreeValue* attributes = struct_member(value, 2U);
    if (!array_values_are_valid(attributes)) return false;
    for (int index = 0; index < attributes->array_val.count; ++index) {
        static const ValueFieldShape attribute_shape =
            FIELD("data", VAL_TYPE_STRING, "string");
        if (!value_has_shape(&attributes->array_val.elements[index],
                             &attribute_shape)) {
            return false;
        }
    }
    return validate_exact_struct(struct_member(value, 9U),
                                 "SerializedTextureProperty",
                                 texture_fields,
                                 ARRAY_COUNT(texture_fields));
}

static bool validate_shader_dependencies(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("from", VAL_TYPE_STRING, "string"),
        FIELD("to", VAL_TYPE_STRING, "string"),
    };
    if (!array_values_are_valid(value)) return false;
    for (int index = 0; index < value->array_val.count; ++index) {
        const TypeTreeValue* dependency = &value->array_val.elements[index];
        if (!dependency->name || strcmp(dependency->name, "data") != 0 ||
            !validate_exact_struct(dependency,
                                   "SerializedShaderDependency", fields,
                                   ARRAY_COUNT(fields))) {
            return false;
        }
    }
    return true;
}

static bool validate_custom_editors(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("customEditorName", VAL_TYPE_STRING, "string"),
        FIELD("renderPipelineType", VAL_TYPE_STRING, "string"),
    };
    if (!array_values_are_valid(value)) return false;
    for (int index = 0; index < value->array_val.count; ++index) {
        const TypeTreeValue* editor = &value->array_val.elements[index];
        if (!editor->name || strcmp(editor->name, "data") != 0 ||
            !validate_exact_struct(
                editor, "SerializedCustomEditorForRenderPipeline", fields,
                ARRAY_COUNT(fields))) {
            return false;
        }
    }
    return true;
}

static bool validate_parsed_form(const TypeTreeValue* value) {
    static const ValueFieldShape fields[] = {
        FIELD("m_PropInfo", VAL_TYPE_STRUCT, "SerializedProperties"),
        FIELD("m_SubShaders", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_KeywordNames", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_KeywordFlags", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_Name", VAL_TYPE_STRING, "string"),
        FIELD("m_CustomEditorName", VAL_TYPE_STRING, "string"),
        FIELD("m_FallbackName", VAL_TYPE_STRING, "string"),
        FIELD("m_Dependencies", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_CustomEditorForRenderPipelines", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_DisableNoSubshadersMessage", VAL_TYPE_INT, "bool"),
    };
    static const ValueFieldShape property_info_fields[] = {
        FIELD("m_Props", VAL_TYPE_ARRAY, "vector"),
    };
    if (!validate_exact_struct(value, "SerializedShader", fields,
                               ARRAY_COUNT(fields))) {
        return false;
    }
    const TypeTreeValue* property_info = struct_member(value, 0U);
    if (!validate_exact_struct(property_info, "SerializedProperties",
                               property_info_fields,
                               ARRAY_COUNT(property_info_fields))) {
        return false;
    }
    const TypeTreeValue* properties = struct_member(property_info, 0U);
    const TypeTreeValue* subshaders = struct_member(value, 1U);
    const TypeTreeValue* keyword_names = struct_member(value, 2U);
    const TypeTreeValue* keyword_flags = struct_member(value, 3U);
    if (!array_values_are_valid(properties) ||
        !array_values_are_valid(subshaders) ||
        !array_values_are_valid(keyword_names) ||
        !array_is_valid(keyword_flags) ||
        keyword_names->array_val.count != keyword_flags->array_val.count ||
        !validate_shader_dependencies(struct_member(value, 7U)) ||
        !validate_custom_editors(struct_member(value, 8U))) {
        return false;
    }
    for (int index = 0; index < properties->array_val.count; ++index) {
        if (!validate_property(&properties->array_val.elements[index])) {
            return false;
        }
    }
    for (int index = 0; index < subshaders->array_val.count; ++index) {
        if (!validate_subshader(&subshaders->array_val.elements[index])) {
            return false;
        }
    }
    for (int index = 0; index < keyword_names->array_val.count; ++index) {
        static const ValueFieldShape keyword_shape =
            FIELD("data", VAL_TYPE_STRING, "string");
        if (!value_has_shape(&keyword_names->array_val.elements[index],
                             &keyword_shape)) {
            return false;
        }
    }
    return true;
}

bool serialized_shader_profile_validate_value(
    const TypeTreeValue* value,
    SerializedShaderSchemaProfile profile) {
    static const ValueFieldShape fields[] = {
        FIELD("m_Name", VAL_TYPE_STRING, "string"),
        FIELD("m_ParsedForm", VAL_TYPE_STRUCT, "SerializedShader"),
        FIELD("platforms", VAL_TYPE_ARRAY, "vector"),
        FIELD("offsets", VAL_TYPE_ARRAY, "vector"),
        FIELD("compressedLengths", VAL_TYPE_ARRAY, "vector"),
        FIELD("decompressedLengths", VAL_TYPE_ARRAY, "vector"),
        FIELD("compressedBlob", VAL_TYPE_ARRAY, "vector"),
        FIELD("stageCounts", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_Dependencies", VAL_TYPE_ARRAY, "vector"),
        FIELD("m_NonModifiableTextures", VAL_TYPE_ARRAY, "map"),
        FIELD("m_ShaderIsBaked", VAL_TYPE_INT, "bool"),
    };
    if ((profile != SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1 &&
         profile !=
             SERIALIZED_SHADER_PROFILE_UNITY_2021_3_29F1_PLAYER_RESOURCES) ||
        !value || !value->name || strcmp(value->name, "Base") != 0 ||
        !validate_exact_struct(value, "Shader", fields,
                               ARRAY_COUNT(fields)) ||
        !validate_parsed_form(struct_member(value, 1U))) {
        LOG_ERROR("Unity 2021.3 player Shader value does not match the pinned "
                  "ordered TypeTree shape");
        return false;
    }

    const TypeTreeValue* platforms = struct_member(value, 2U);
    const TypeTreeValue* offsets = struct_member(value, 3U);
    const TypeTreeValue* compressed_lengths = struct_member(value, 4U);
    const TypeTreeValue* decompressed_lengths = struct_member(value, 5U);
    const TypeTreeValue* compressed_blob = struct_member(value, 6U);
    const TypeTreeValue* stage_counts = struct_member(value, 7U);
    if (!array_values_are_valid(platforms) ||
        !array_values_are_valid(offsets) ||
        !array_values_are_valid(compressed_lengths) ||
        !array_values_are_valid(decompressed_lengths) ||
        !value_has_shape(compressed_blob, &fields[6]) ||
        !array_values_are_valid(stage_counts) ||
        !array_values_are_valid(struct_member(value, 8U)) ||
        !array_values_are_valid(struct_member(value, 9U)) ||
        offsets->array_val.count != platforms->array_val.count ||
        compressed_lengths->array_val.count != platforms->array_val.count ||
        decompressed_lengths->array_val.count != platforms->array_val.count ||
        stage_counts->array_val.count != platforms->array_val.count) {
        LOG_ERROR("Unity 2021.3 player Shader archive fields have incompatible "
                  "shapes");
        return false;
    }
    for (int index = 0; index < offsets->array_val.count; ++index) {
        static const ValueFieldShape group_shape =
            FIELD("data", VAL_TYPE_ARRAY, "vector");
        if (!value_has_shape(&offsets->array_val.elements[index],
                             &group_shape) ||
            !value_has_shape(&compressed_lengths->array_val.elements[index],
                             &group_shape) ||
            !value_has_shape(&decompressed_lengths->array_val.elements[index],
                             &group_shape)) {
            return false;
        }
    }
    return true;
}

#undef ARRAY_COUNT
#undef FIELD
