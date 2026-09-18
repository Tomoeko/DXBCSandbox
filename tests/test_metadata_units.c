#include "translation/hlsl_emitter_internal.h"
#include "io/parameter_layout.h"
#include "io/serialized_shader.h"
#include "io/subprogram_metadata.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void set_resource(SerializedResourceParam* resource, const char* name,
                         SerializedResourceType type, uint32_t index,
                         uint32_t sampler_index) {
    memset(resource, 0, sizeof(*resource));
    resource->name = name;
    resource->bind_type = type;
    resource->bind_index = index;
    resource->sampler_index = sampler_index;
}

static TypeTreeValue test_int_value(const char* name, int64_t value) {
    TypeTreeValue result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.type = VAL_TYPE_INT;
    result.int_val = value;
    return result;
}

static TypeTreeValue test_string_value(const char* name, const char* value) {
    TypeTreeValue result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.type = VAL_TYPE_STRING;
    result.string_val = (char*)value;
    result.string_length = strlen(value);
    return result;
}

static TypeTreeValue test_array_value(const char* name,
                                      TypeTreeValue* elements, int count) {
    TypeTreeValue result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.type = VAL_TYPE_ARRAY;
    result.array_val.elements = elements;
    result.array_val.count = count;
    return result;
}

static TypeTreeValue test_struct_value(const char* name,
                                       TypeTreeValue* members, int count) {
    TypeTreeValue result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.type = VAL_TYPE_STRUCT;
    result.struct_val.members = members;
    result.struct_val.count = count;
    return result;
}

static int test_serialized_shader_variant_identity(void) {
    TypeTreeValue keyword_name_values[] = {
        test_string_value("data", "GLOBAL_A"),
        test_string_value("data", "LOCAL_A"),
        test_string_value("data", "LOCAL_B"),
    };
    TypeTreeValue keyword_names = test_array_value(
        "m_KeywordNames", keyword_name_values,
        (int)(sizeof(keyword_name_values) / sizeof(keyword_name_values[0])));

    TypeTreeValue legacy_indices_values[] = {
        test_int_value("data", 2),
    };
    TypeTreeValue legacy_indices = test_array_value(
        "m_KeywordIndices", legacy_indices_values,
        (int)(sizeof(legacy_indices_values) /
              sizeof(legacy_indices_values[0])));
    TypeTreeValue legacy_subprogram_members[] = {
        test_int_value("m_BlobIndex", 90),
        test_int_value("m_ShaderRequirements",
                       INT64_C(0x0000010000000009)),
        test_int_value("m_GpuProgramType", 15),
        legacy_indices,
    };
    TypeTreeValue legacy_subprogram = test_struct_value(
        "data", legacy_subprogram_members,
        (int)(sizeof(legacy_subprogram_members) /
              sizeof(legacy_subprogram_members[0])));

    TypeTreeValue explicit_global_values[] = {
        test_int_value("data", 0),
    };
    TypeTreeValue explicit_local_values[] = {
        test_int_value("data", 1),
    };
    TypeTreeValue explicit_global = test_array_value(
        "m_GlobalKeywordIndices", explicit_global_values,
        (int)(sizeof(explicit_global_values) /
              sizeof(explicit_global_values[0])));
    TypeTreeValue explicit_local = test_array_value(
        "m_LocalKeywordIndices", explicit_local_values,
        (int)(sizeof(explicit_local_values) /
              sizeof(explicit_local_values[0])));
    TypeTreeValue explicit_subprogram_members[] = {
        test_int_value("m_BlobIndex", 100),
        test_int_value("m_ShaderHardwareTier", 2),
        test_int_value("m_ShaderRequirements", 10),
        test_int_value("m_GpuProgramType", 16),
        legacy_indices,
        explicit_global,
        explicit_local,
    };
    TypeTreeValue explicit_subprogram = test_struct_value(
        "data", explicit_subprogram_members,
        (int)(sizeof(explicit_subprogram_members) /
              sizeof(explicit_subprogram_members[0])));

    TypeTreeValue second_local_values[] = {
        test_int_value("data", 2),
    };
    TypeTreeValue second_local = test_array_value(
        "m_LocalKeywordIndices", second_local_values,
        (int)(sizeof(second_local_values) /
              sizeof(second_local_values[0])));
    TypeTreeValue second_subprogram_members[] = {
        test_int_value("m_BlobIndex", 101),
        test_int_value("m_ShaderHardwareTier", 3),
        test_int_value("m_ShaderRequirements", INT64_MIN + INT64_C(11)),
        test_int_value("m_GpuProgramType", 16),
        second_local,
    };
    TypeTreeValue second_subprogram = test_struct_value(
        "data", second_subprogram_members,
        (int)(sizeof(second_subprogram_members) /
              sizeof(second_subprogram_members[0])));

    TypeTreeValue group_one_subprograms[] = {legacy_subprogram};
    TypeTreeValue group_three_subprograms[] = {
        explicit_subprogram,
        second_subprogram,
    };
    TypeTreeValue player_groups[] = {
        test_array_value("data", NULL, 0),
        test_array_value("data", group_one_subprograms, 1),
        test_array_value("data", NULL, 0),
        test_array_value("data", group_three_subprograms, 2),
    };
    TypeTreeValue player_subprograms = test_array_value(
        "m_PlayerSubPrograms", player_groups,
        (int)(sizeof(player_groups) / sizeof(player_groups[0])));

    TypeTreeValue group_one_parameter_values[] = {
        test_int_value("data", 901),
    };
    TypeTreeValue group_three_parameter_values[] = {
        test_int_value("data", 1001),
        test_int_value("data", 1002),
    };
    TypeTreeValue parameter_groups[] = {
        test_array_value("data", NULL, 0),
        test_array_value("data", group_one_parameter_values, 1),
        test_array_value("data", NULL, 0),
        test_array_value("data", group_three_parameter_values, 2),
    };
    TypeTreeValue parameter_indices = test_array_value(
        "m_ParameterBlobIndices", parameter_groups,
        (int)(sizeof(parameter_groups) / sizeof(parameter_groups[0])));

    TypeTreeValue vertex_program_members[] = {
        player_subprograms,
        parameter_indices,
    };
    TypeTreeValue vertex_program = test_struct_value(
        "progVertex", vertex_program_members,
        (int)(sizeof(vertex_program_members) /
              sizeof(vertex_program_members[0])));
    TypeTreeValue pass_platform_values[] = {
        test_int_value("data", 4),
    };
    TypeTreeValue pass_platforms = test_array_value(
        "m_Platforms", pass_platform_values,
        (int)(sizeof(pass_platform_values) /
              sizeof(pass_platform_values[0])));
    TypeTreeValue keyword_state_mask_values[] = {
        test_int_value("data", 1),
        test_int_value("data", 0x200),
    };
    TypeTreeValue keyword_state_mask = test_array_value(
        "m_SerializedKeywordStateMask", keyword_state_mask_values,
        (int)(sizeof(keyword_state_mask_values) /
              sizeof(keyword_state_mask_values[0])));
    TypeTreeValue pass_members[] = {
        test_int_value("m_Type", 0),
        pass_platforms,
        test_int_value("m_ProgramMask", 54),
        keyword_state_mask,
        vertex_program,
    };
    TypeTreeValue pass = test_struct_value(
        "data", pass_members,
        (int)(sizeof(pass_members) / sizeof(pass_members[0])));
    TypeTreeValue pass_values[] = {pass};
    TypeTreeValue passes = test_array_value("m_Passes", pass_values, 1);
    TypeTreeValue subshader_members[] = {passes};
    TypeTreeValue subshader = test_struct_value(
        "data", subshader_members,
        (int)(sizeof(subshader_members) / sizeof(subshader_members[0])));
    TypeTreeValue subshader_values[] = {subshader};
    TypeTreeValue subshaders =
        test_array_value("m_SubShaders", subshader_values, 1);
    TypeTreeValue parsed_members[] = {
        test_string_value("m_Name", "VariantIdentityFixture"),
        keyword_names,
        subshaders,
    };
    TypeTreeValue parsed_form = test_struct_value(
        "m_ParsedForm", parsed_members,
        (int)(sizeof(parsed_members) / sizeof(parsed_members[0])));

    TypeTreeValue platform_values[] = {
        test_int_value("data", 4),
    };
    TypeTreeValue platforms =
        test_array_value("platforms", platform_values, 1);
    TypeTreeValue root_members[] = {platforms, parsed_form};
    TypeTreeValue root = test_struct_value(
        "Shader", root_members,
        (int)(sizeof(root_members) / sizeof(root_members[0])));

    size_t allocation_count_before = g_allocations_count;
    size_t allocated_bytes_before = g_allocated_bytes;
    SerializedShader shader;
    serialized_shader_init(&shader);
    CHECK(serialized_shader_parse(&shader, &root));
    CHECK(strcmp(shader.name, "VariantIdentityFixture") == 0);
    CHECK(shader.subshader_count == 1);
    CHECK(shader.subshaders[0].pass_count == 1);

    SerializedPass* parsed_pass = &shader.subshaders[0].passes[0];
    CHECK(parsed_pass->platform_count == 1);
    CHECK(parsed_pass->platforms[0] == 4);
    CHECK(parsed_pass->has_serialized_platforms);
    CHECK(parsed_pass->program_mask == 54);
    CHECK(parsed_pass->serialized_keyword_state_mask_count == 2);
    CHECK(parsed_pass->serialized_keyword_state_mask[0] == 1);
    CHECK(parsed_pass->serialized_keyword_state_mask[1] == 0x200);
    CHECK(shader.archive_platform_count == 1);
    CHECK(shader.archive_platforms[0] == 4);
    CHECK(parsed_pass->subprogram_count[0] == 3);
    CHECK(parsed_pass->subprogram_identities[0] != NULL);
    CHECK(parsed_pass->subprogram_param_blob_indices[0][0] == 901);
    CHECK(parsed_pass->subprogram_param_blob_indices[0][1] == 1001);
    CHECK(parsed_pass->subprogram_param_blob_indices[0][2] == 1002);
    CHECK(!parsed_pass->subprograms[0][0].has_hardware_tier);
    CHECK(parsed_pass->subprograms[0][0].hardware_tier == 0);
    CHECK(parsed_pass->subprograms[0][1].has_hardware_tier);
    CHECK(parsed_pass->subprograms[0][1].hardware_tier == 2);

    SerializedSubProgramIdentity* identities =
        parsed_pass->subprogram_identities[0];
    CHECK(identities[0].hardware_tier_group == 1);
    CHECK(identities[0].inner_subprogram_index == 0);
    CHECK(!identities[0].keyword_scopes_are_explicit);
    CHECK(identities[0].local_keyword_index_count == 1);
    CHECK(identities[0].local_keyword_indices[0] == 2);

    CHECK(identities[1].hardware_tier_group == 3);
    CHECK(identities[1].inner_subprogram_index == 0);
    CHECK(identities[1].keyword_scopes_are_explicit);
    CHECK(identities[1].global_keyword_index_count == 1);
    CHECK(identities[1].global_keyword_indices[0] == 0);
    CHECK(identities[1].local_keyword_index_count == 1);
    CHECK(identities[1].local_keyword_indices[0] == 1);

    CHECK(identities[2].hardware_tier_group == 3);
    CHECK(identities[2].inner_subprogram_index == 1);
    CHECK(identities[2].keyword_scopes_are_explicit);
    CHECK(identities[2].global_keyword_index_count == 0);
    CHECK(identities[2].local_keyword_index_count == 1);
    CHECK(identities[2].local_keyword_indices[0] == 2);
    CHECK(serialized_pass_subprogram_is_platform(parsed_pass, 0, 0, 4));
    CHECK(serialized_pass_subprogram_is_platform(parsed_pass, 0, 1, 4));
    CHECK(serialized_pass_subprogram_is_platform(parsed_pass, 0, 2, 4));
    CHECK(!serialized_pass_subprogram_is_platform(parsed_pass, 0, 0, 15));
    CHECK(!serialized_pass_subprogram_is_platform(parsed_pass, 0, 3, 4));
    CHECK(!serialized_pass_subprogram_is_platform(parsed_pass, -1, 0, 4));
    CHECK(serialized_gpu_program_type_is_platform(15, 4));
    CHECK(serialized_gpu_program_type_is_platform(22, 4));
    CHECK(serialized_gpu_program_type_is_platform(6, 15));
    CHECK(serialized_gpu_program_type_is_platform(8, 15));
    CHECK(!serialized_gpu_program_type_is_platform(6, 4));
    CHECK(!serialized_gpu_program_type_is_platform(15, 15));
    CHECK(!serialized_gpu_program_type_is_platform(0, 4));
    CHECK(!serialized_gpu_program_type_is_platform(15, 999));

    /* A pass-level platform match is insufficient: mixed-platform player
     * records must also be filtered by m_GpuProgramType. */
    parsed_pass->subprograms[0][0].program_type = 6;
    CHECK(!serialized_pass_subprogram_is_platform(parsed_pass, 0, 0, 4));
    parsed_pass->subprograms[0][0].program_type = 15;
    CHECK(serialized_pass_subprogram_is_platform(parsed_pass, 0, 0, 4));

    SerializedSubProgram* parsed_subprograms = parsed_pass->subprograms[0];
    CHECK(parsed_subprograms[0].blob_index == 90);
    CHECK(parsed_subprograms[0].shader_requirements ==
          UINT64_C(0x0000010000000009));
    CHECK(parsed_subprograms[0].global_keyword_count == 0);
    CHECK(parsed_subprograms[0].local_keyword_count == 1);
    CHECK(strcmp(parsed_subprograms[0].local_keywords[0], "LOCAL_B") == 0);
    CHECK(parsed_subprograms[1].blob_index == 100);
    CHECK(parsed_subprograms[1].shader_requirements == UINT64_C(10));
    CHECK(parsed_subprograms[1].global_keyword_count == 1);
    CHECK(strcmp(parsed_subprograms[1].global_keywords[0], "GLOBAL_A") == 0);
    CHECK(parsed_subprograms[1].local_keyword_count == 1);
    CHECK(strcmp(parsed_subprograms[1].local_keywords[0], "LOCAL_A") == 0);
    CHECK(parsed_subprograms[2].blob_index == 101);
    CHECK(parsed_subprograms[2].shader_requirements ==
          UINT64_C(0x800000000000000b));
    CHECK(parsed_subprograms[2].global_keyword_count == 0);
    CHECK(parsed_subprograms[2].local_keyword_count == 1);
    CHECK(strcmp(parsed_subprograms[2].local_keywords[0], "LOCAL_B") == 0);

    serialized_shader_free(&shader);
    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);

    /* Serialized keyword indices are references into m_KeywordNames.  An
     * unresolved reference must reject the shader instead of silently
     * producing an empty keyword, and partial stage allocations must unwind. */
    explicit_global_values[0].int_val = 3;
    SerializedShader invalid_shader;
    serialized_shader_init(&invalid_shader);
    CHECK(!serialized_shader_parse(&invalid_shader, &root));
    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    explicit_global_values[0].int_val = 0;

    /* Parallel parameter-index arrays are part of the subprogram identity;
     * reject a type mismatch rather than leaving the index at its sentinel. */
    group_one_parameter_values[0] = test_string_value("data", "bad-index");
    CHECK(!serialized_shader_parse(&invalid_shader, &root));
    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    group_one_parameter_values[0] = test_int_value("data", 901);

    /* Nested player-subprogram groups must own the declared element count. */
    TypeTreeValue* group_one_elements = player_groups[1].array_val.elements;
    player_groups[1].array_val.elements = NULL;
    CHECK(!serialized_shader_parse(&invalid_shader, &root));
    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    player_groups[1].array_val.elements = group_one_elements;
    return 0;
}

static int test_typetree_common_parameter_projection(void) {
    SerializedNameTableEntry names[] = {
        {.name = "LooseVec", .index = 1},
        {.name = "MyCB", .index = 2},
        {.name = "MyMatrix", .index = 3},
        {.name = "MyVector", .index = 4},
        {.name = "MyStruct", .index = 5},
        {.name = "MemberVector", .index = 6},
        {.name = "MemberMatrix", .index = 7},
        {.name = "MyTexture", .index = 8},
        {.name = "MyBuffer", .index = 9},
        {.name = "MyUAV", .index = 10},
    };
    SerializedNameTable name_table = {
        .count = (int)(sizeof(names) / sizeof(names[0])),
        .entries = names,
    };

    TypeTreeValue loose_vector_members[] = {
        test_int_value("m_NameIndex", 1),
        test_int_value("m_Index", 16),
        test_int_value("m_ArraySize", 0),
        test_int_value("m_Type", 0),
        test_int_value("m_Dim", 4),
    };
    TypeTreeValue loose_vector = test_struct_value(
        "data", loose_vector_members,
        (int)(sizeof(loose_vector_members) /
              sizeof(loose_vector_members[0])));
    TypeTreeValue loose_vector_values[] = {loose_vector};
    TypeTreeValue loose_vectors =
        test_array_value("m_VectorParams", loose_vector_values, 1);
    TypeTreeValue loose_matrices =
        test_array_value("m_MatrixParams", NULL, 0);

    TypeTreeValue matrix_members[] = {
        test_int_value("m_NameIndex", 3),
        test_int_value("m_Index", 32),
        test_int_value("m_ArraySize", 0),
        test_int_value("m_Type", 0),
        test_int_value("m_RowCount", 4),
    };
    TypeTreeValue matrix = test_struct_value(
        "data", matrix_members,
        (int)(sizeof(matrix_members) / sizeof(matrix_members[0])));
    TypeTreeValue matrices_values[] = {matrix};
    TypeTreeValue matrices =
        test_array_value("m_MatrixParams", matrices_values, 1);

    TypeTreeValue vector_members[] = {
        test_int_value("m_NameIndex", 4),
        test_int_value("m_Index", 96),
        test_int_value("m_ArraySize", 2),
        test_int_value("m_Type", 1),
        test_int_value("m_Dim", 3),
    };
    TypeTreeValue vector = test_struct_value(
        "data", vector_members,
        (int)(sizeof(vector_members) / sizeof(vector_members[0])));
    TypeTreeValue vectors_values[] = {vector};
    TypeTreeValue vectors =
        test_array_value("m_VectorParams", vectors_values, 1);

    TypeTreeValue struct_vector_members[] = {
        test_int_value("m_NameIndex", 6),
        test_int_value("m_Index", 0),
        test_int_value("m_ArraySize", 0),
        test_int_value("m_Type", 0),
        test_int_value("m_Dim", 2),
    };
    TypeTreeValue struct_vector = test_struct_value(
        "data", struct_vector_members,
        (int)(sizeof(struct_vector_members) /
              sizeof(struct_vector_members[0])));
    TypeTreeValue struct_vector_values[] = {struct_vector};
    TypeTreeValue struct_vectors = test_array_value(
        "m_VectorMembers", struct_vector_values, 1);
    TypeTreeValue struct_matrix_members[] = {
        test_int_value("m_NameIndex", 7),
        test_int_value("m_Index", 16),
        test_int_value("m_ArraySize", 0),
        test_int_value("m_Type", 0),
        test_int_value("m_RowCount", 3),
    };
    TypeTreeValue struct_matrix = test_struct_value(
        "data", struct_matrix_members,
        (int)(sizeof(struct_matrix_members) /
              sizeof(struct_matrix_members[0])));
    TypeTreeValue struct_matrix_values[] = {struct_matrix};
    TypeTreeValue struct_matrices = test_array_value(
        "m_MatrixMembers", struct_matrix_values, 1);
    TypeTreeValue struct_members[] = {
        test_int_value("m_NameIndex", 5),
        test_int_value("m_Index", 128),
        test_int_value("m_ArraySize", 3),
        test_int_value("m_StructSize", 64),
        struct_vectors,
        struct_matrices,
    };
    TypeTreeValue structure = test_struct_value(
        "data", struct_members,
        (int)(sizeof(struct_members) / sizeof(struct_members[0])));
    TypeTreeValue structure_values[] = {structure};
    TypeTreeValue structures =
        test_array_value("m_StructParams", structure_values, 1);

    TypeTreeValue cbuffer_members[] = {
        test_int_value("m_NameIndex", 2),
        matrices,
        vectors,
        structures,
        test_int_value("m_Size", 320),
        test_int_value("m_IsPartialCB", 1),
    };
    TypeTreeValue cbuffer = test_struct_value(
        "data", cbuffer_members,
        (int)(sizeof(cbuffer_members) / sizeof(cbuffer_members[0])));
    TypeTreeValue cbuffer_values[] = {cbuffer};
    TypeTreeValue cbuffers =
        test_array_value("m_ConstantBuffers", cbuffer_values, 1);

    TypeTreeValue texture_members[] = {
        test_int_value("m_NameIndex", 8),
        test_int_value("m_Index", 3),
        test_int_value("m_SamplerIndex", 7),
        test_int_value("m_MultiSampled", 1),
        test_int_value("m_Dim", 2),
    };
    TypeTreeValue texture = test_struct_value(
        "data", texture_members,
        (int)(sizeof(texture_members) / sizeof(texture_members[0])));
    TypeTreeValue texture_values[] = {texture};
    TypeTreeValue textures =
        test_array_value("m_TextureParams", texture_values, 1);

    TypeTreeValue buffer_members[] = {
        test_int_value("m_NameIndex", 9),
        test_int_value("m_Index", 4),
        test_int_value("m_ArraySize", 5),
    };
    TypeTreeValue buffer = test_struct_value(
        "data", buffer_members,
        (int)(sizeof(buffer_members) / sizeof(buffer_members[0])));
    TypeTreeValue buffer_values[] = {buffer};
    TypeTreeValue buffers =
        test_array_value("m_BufferParams", buffer_values, 1);

    TypeTreeValue uav_members[] = {
        test_int_value("m_NameIndex", 10),
        test_int_value("m_Index", 6),
        test_int_value("m_OriginalIndex", 12),
    };
    TypeTreeValue uav = test_struct_value(
        "data", uav_members,
        (int)(sizeof(uav_members) / sizeof(uav_members[0])));
    TypeTreeValue uav_values[] = {uav};
    TypeTreeValue uavs = test_array_value("m_UAVParams", uav_values, 1);

    TypeTreeValue sampler_members[] = {
        test_int_value("sampler", 84),
        test_int_value("bindPoint", 7),
    };
    TypeTreeValue sampler = test_struct_value(
        "data", sampler_members,
        (int)(sizeof(sampler_members) / sizeof(sampler_members[0])));
    TypeTreeValue sampler_values[] = {sampler};
    TypeTreeValue samplers =
        test_array_value("m_Samplers", sampler_values, 1);

    TypeTreeValue cbuffer_binding_members[] = {
        test_int_value("m_NameIndex", 2),
        test_int_value("m_Index", 1),
        test_int_value("m_ArraySize", 2),
    };
    TypeTreeValue cbuffer_binding = test_struct_value(
        "data", cbuffer_binding_members,
        (int)(sizeof(cbuffer_binding_members) /
              sizeof(cbuffer_binding_members[0])));
    TypeTreeValue cbuffer_binding_values[] = {cbuffer_binding};
    TypeTreeValue cbuffer_bindings = test_array_value(
        "m_ConstantBufferBindings", cbuffer_binding_values, 1);

    TypeTreeValue common_members[] = {
        loose_vectors, loose_matrices, textures, buffers, cbuffers,
        cbuffer_bindings, uavs, samplers,
    };
    TypeTreeValue common = test_struct_value(
        "m_CommonParameters", common_members,
        (int)(sizeof(common_members) / sizeof(common_members[0])));

    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    CHECK(serialized_program_parameters_parse_typetree(
        &parameters, &common, &name_table));
    CHECK(!parameters.is_binary);
    CHECK(parameters.cb_count == 2);
    CHECK(strcmp(parameters.constant_buffers[0].name, "$Globals") == 0);
    CHECK(parameters.constant_buffers[0].role ==
          SERIALIZED_CBUFFER_LOOSE_PARAMETERS);
    CHECK(parameters.constant_buffers[0].size == 32U);
    CHECK(parameters.constant_buffers[0].var_count == 1);
    CHECK(strcmp(parameters.constant_buffers[0].variables[0].name,
                 "LooseVec") == 0);
    const SerializedConstantBuffer* parsed_cb =
        &parameters.constant_buffers[1];
    CHECK(parsed_cb->role == SERIALIZED_CBUFFER_NAMED);
    CHECK(strcmp(parsed_cb->name, "MyCB") == 0);
    CHECK(parsed_cb->size == 320);
    CHECK(parsed_cb->has_is_partial && parsed_cb->is_partial);
    CHECK(parsed_cb->var_count == 2);
    CHECK(parsed_cb->variables[0].layout[4] == 1);
    CHECK(parsed_cb->variables[1].layout[4] == 0);
    CHECK(parsed_cb->struct_count == 1);
    CHECK(strcmp(parsed_cb->struct_params[0].name, "MyStruct") == 0);
    CHECK(parsed_cb->struct_params[0].layout[0] == 128);
    CHECK(parsed_cb->struct_params[0].layout[1] == 3);
    CHECK(parsed_cb->struct_params[0].layout[2] == 64);
    CHECK(parsed_cb->struct_params[0].member_count == 2);
    CHECK(parsed_cb->struct_params[0].members[0].layout[4] == 0);
    CHECK(parsed_cb->struct_params[0].members[1].layout[4] == 1);

    CHECK(parameters.res_count == 5);
    const SerializedResourceParam* resource = &parameters.resources[0];
    CHECK(resource->bind_type == SERIALIZED_RESOURCE_TEXTURE);
    CHECK(resource->bind_index == 3 && resource->sampler_index == 7);
    CHECK(resource->dimension == 2 && resource->multisampled);
    CHECK(resource->extra[0] == 7 && resource->extra[1] == 5);
    resource = &parameters.resources[1];
    CHECK(resource->bind_type == SERIALIZED_RESOURCE_BUFFER);
    CHECK(resource->bind_index == 4 && resource->array_size == 5);
    resource = &parameters.resources[2];
    CHECK(resource->bind_type == SERIALIZED_RESOURCE_UAV);
    CHECK(resource->bind_index == 6 && resource->original_index == 12);
    resource = &parameters.resources[3];
    CHECK(resource->bind_type == SERIALIZED_RESOURCE_SAMPLER);
    CHECK(resource->bind_index == 7 && resource->sampler_state == 84);
    resource = &parameters.resources[4];
    CHECK(resource->bind_type == SERIALIZED_RESOURCE_CONSTANT_BUFFER);
    CHECK(resource->bind_index == 1 && resource->array_size == 2);
    serialized_program_parameters_free(&parameters);
    return 0;
}

static int test_typetree_dynamic_parameter_collections(void) {
    const size_t allocation_count_before = g_allocations_count;
    const size_t allocated_bytes_before = g_allocated_bytes;
    SerializedNameTableEntry name = {.name = "Dynamic", .index = 1};
    SerializedNameTable name_table = {.count = 1, .entries = &name};

    TypeTreeValue variable_members[] = {
        test_int_value("m_NameIndex", 1),
        test_int_value("m_Index", 0),
        test_int_value("m_ArraySize", 0),
        test_int_value("m_Type", 0),
        test_int_value("m_Dim", 4),
    };
    TypeTreeValue variable_values[65];
    for (int i = 0; i < 65; i++) {
        variable_values[i] = test_struct_value(
            "data", variable_members,
            (int)(sizeof(variable_members) / sizeof(variable_members[0])));
    }
    TypeTreeValue loose_vectors =
        test_array_value("m_VectorParams", variable_values, 65);

    TypeTreeValue member_vectors =
        test_array_value("m_VectorMembers", variable_values, 65);
    TypeTreeValue structure_members[] = {
        test_int_value("m_NameIndex", 1),
        test_int_value("m_Index", 0),
        test_int_value("m_ArraySize", 1),
        test_int_value("m_StructSize", 16),
        member_vectors,
    };
    TypeTreeValue structure_values[17];
    for (int i = 0; i < 17; i++) {
        structure_values[i] = test_struct_value(
            "data", structure_members,
            (int)(sizeof(structure_members) /
                  sizeof(structure_members[0])));
    }
    TypeTreeValue structures =
        test_array_value("m_StructParams", structure_values, 17);

    TypeTreeValue first_cbuffer_members[] = {
        test_int_value("m_NameIndex", 1),
        test_int_value("m_Size", 4096),
        structures,
    };
    TypeTreeValue empty_cbuffer_members[] = {
        test_int_value("m_NameIndex", 1),
        test_int_value("m_Size", 16),
    };
    TypeTreeValue cbuffer_values[17];
    cbuffer_values[0] = test_struct_value(
        "data", first_cbuffer_members,
        (int)(sizeof(first_cbuffer_members) /
              sizeof(first_cbuffer_members[0])));
    for (int i = 1; i < 17; i++) {
        cbuffer_values[i] = test_struct_value(
            "data", empty_cbuffer_members,
            (int)(sizeof(empty_cbuffer_members) /
                  sizeof(empty_cbuffer_members[0])));
    }
    TypeTreeValue cbuffers =
        test_array_value("m_ConstantBuffers", cbuffer_values, 17);

    TypeTreeValue texture_members[] = {
        test_int_value("m_NameIndex", 1),
        test_int_value("m_Index", 0),
        test_int_value("m_SamplerIndex", 0),
        test_int_value("m_MultiSampled", 0),
        test_int_value("m_Dim", 2),
    };
    TypeTreeValue texture_values[65];
    for (int i = 0; i < 65; i++) {
        texture_values[i] = test_struct_value(
            "data", texture_members,
            (int)(sizeof(texture_members) / sizeof(texture_members[0])));
    }
    TypeTreeValue textures =
        test_array_value("m_TextureParams", texture_values, 65);
    TypeTreeValue common_members[] = {loose_vectors, cbuffers, textures};
    TypeTreeValue common = test_struct_value(
        "m_CommonParameters", common_members,
        (int)(sizeof(common_members) / sizeof(common_members[0])));

    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    CHECK(serialized_program_parameters_parse_typetree(
        &parameters, &common, &name_table));
    CHECK(parameters.cb_count == 18);
    CHECK(parameters.constant_buffers[0].var_count == 65);
    CHECK(parameters.constant_buffers[1].struct_count == 17);
    CHECK(parameters.constant_buffers[1].struct_params[16].member_count == 65);
    CHECK(parameters.res_count == 65);
    CHECK(parameters.resources[64].bind_type == SERIALIZED_RESOURCE_TEXTURE);
    serialized_program_parameters_free(&parameters);

    TypeTreeValue invalid_cbuffers =
        test_array_value("m_ConstantBuffers", NULL, 1);
    TypeTreeValue invalid_members[] = {invalid_cbuffers};
    TypeTreeValue invalid = test_struct_value(
        "m_CommonParameters", invalid_members, 1);
    serialized_program_parameters_init(&parameters);
    CHECK(!serialized_program_parameters_parse_typetree(
        &parameters, &invalid, &name_table));
    serialized_program_parameters_free(&parameters);

    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    return 0;
}

typedef struct {
    uint8_t bytes[131072];
    size_t size;
} ParameterBlobBuilder;

static bool parameter_blob_append_bytes(ParameterBlobBuilder* builder,
                                        const void* bytes, size_t size) {
    if (!builder || (!bytes && size != 0) ||
        size > sizeof(builder->bytes) - builder->size) {
        return false;
    }
    if (size != 0) memcpy(builder->bytes + builder->size, bytes, size);
    builder->size += size;
    return true;
}

static bool parameter_blob_append_u32(ParameterBlobBuilder* builder,
                                      uint32_t value) {
    if (builder->size + 4 > sizeof(builder->bytes)) return false;
    builder->bytes[builder->size++] = (uint8_t)value;
    builder->bytes[builder->size++] = (uint8_t)(value >> 8);
    builder->bytes[builder->size++] = (uint8_t)(value >> 16);
    builder->bytes[builder->size++] = (uint8_t)(value >> 24);
    return true;
}

static bool parameter_blob_append_string(ParameterBlobBuilder* builder,
                                         const char* value) {
    size_t length = strlen(value);
    if (!parameter_blob_append_u32(builder, (uint32_t)length) ||
        builder->size + length + 3 > sizeof(builder->bytes)) {
        return false;
    }
    memcpy(builder->bytes + builder->size, value, length);
    builder->size += length;
    while ((builder->size & 3u) != 0) builder->bytes[builder->size++] = 0;
    return true;
}

static bool parameter_blob_append_pascal_bytes(
    ParameterBlobBuilder* builder, const uint8_t* value, size_t length) {
    if (length > UINT32_MAX ||
        !parameter_blob_append_u32(builder, (uint32_t)length) ||
        !parameter_blob_append_bytes(builder, value, length)) {
        return false;
    }
    while ((builder->size & 3u) != 0) {
        const uint8_t zero = 0;
        if (!parameter_blob_append_bytes(builder, &zero, 1)) return false;
    }
    return true;
}

static bool build_parameter_collection_blob(ParameterBlobBuilder* builder,
                                            uint32_t cbuffer_count,
                                            uint32_t variable_count,
                                            uint32_t struct_count,
                                            uint32_t member_count,
                                            uint32_t resource_count);

static int test_embedded_nul_pascal_strings(void) {
    static const uint8_t embedded_identifier[] = {'A', 0, 'B'};
    const size_t allocation_count_before = g_allocations_count;
    const size_t allocated_bytes_before = g_allocated_bytes;
    ByteStream stream;

    ParameterBlobBuilder variant_blob;
    memset(&variant_blob, 0, sizeof(variant_blob));
    CHECK(parameter_blob_append_u32(&variant_blob, UNITY_2021_3_PLAYER_BLOB_VERSION));
    CHECK(parameter_blob_append_u32(&variant_blob, 15u));
    for (uint32_t header_word = 0; header_word < 4; header_word++) {
        CHECK(parameter_blob_append_u32(&variant_blob, header_word));
    }
    CHECK(parameter_blob_append_u32(&variant_blob, 1u));
    CHECK(parameter_blob_append_pascal_bytes(
        &variant_blob, embedded_identifier, sizeof(embedded_identifier)));
    CHECK(parameter_blob_append_u32(&variant_blob, 0u)); /* bytecode */
    CHECK(parameter_blob_append_u32(&variant_blob, 0u)); /* source map */
    CHECK(parameter_blob_append_u32(&variant_blob, 0u)); /* bindings */

    PlayerSubProgramMetadata variant;
    memset(&variant, 0, sizeof(variant));
    stream_init(&stream, variant_blob.bytes, variant_blob.size);
    CHECK(!subprogram_metadata_parse_variant(&stream, &variant));
    CHECK(variant.local_keywords == NULL);
    CHECK(variant.global_keywords == NULL);
    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);

    ParameterBlobBuilder valid_blob;
    CHECK(build_parameter_collection_blob(&valid_blob, 1, 0, 0, 0, 0));
    SerializedProgramParameters retained;
    serialized_program_parameters_init(&retained);
    stream_init(&stream, valid_blob.bytes, valid_blob.size);
    CHECK(subprogram_metadata_parse_parameters(&stream, &retained));
    SerializedConstantBuffer* retained_buffers = retained.constant_buffers;

    ParameterBlobBuilder parameter_blob;
    memset(&parameter_blob, 0, sizeof(parameter_blob));
    CHECK(parameter_blob_append_u32(&parameter_blob, UNITY_2021_3_PLAYER_BLOB_VERSION));
    CHECK(parameter_blob_append_u32(&parameter_blob, 1u));
    CHECK(parameter_blob_append_pascal_bytes(
        &parameter_blob, embedded_identifier, sizeof(embedded_identifier)));
    CHECK(parameter_blob_append_u32(&parameter_blob, 0u)); /* cbuffer size */
    CHECK(parameter_blob_append_u32(&parameter_blob, 0u)); /* variables */
    CHECK(parameter_blob_append_u32(&parameter_blob, 0u)); /* structs */
    CHECK(parameter_blob_append_u32(&parameter_blob, 0u)); /* resources */
    stream_init(&stream, parameter_blob.bytes, parameter_blob.size);
    CHECK(!subprogram_metadata_parse_parameters(&stream, &retained));
    CHECK(retained.constant_buffers == retained_buffers);
    CHECK(retained.cb_count == 1);
    serialized_program_parameters_free(&retained);

    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    return 0;
}

static bool build_parameter_collection_blob(ParameterBlobBuilder* builder,
                                            uint32_t cbuffer_count,
                                            uint32_t variable_count,
                                            uint32_t struct_count,
                                            uint32_t member_count,
                                            uint32_t resource_count) {
    memset(builder, 0, sizeof(*builder));
    if (!parameter_blob_append_u32(builder, UNITY_2021_3_PLAYER_BLOB_VERSION) ||
        !parameter_blob_append_u32(builder, cbuffer_count)) {
        return false;
    }
    for (uint32_t cb = 0; cb < cbuffer_count; cb++) {
        uint32_t variables = cb == 0 ? variable_count : 0;
        uint32_t structures = cb == 0 ? struct_count : 0;
        if (!parameter_blob_append_string(builder, cb == 0 ? "" : "CB") ||
            !parameter_blob_append_u32(builder, cb * 16u) ||
            !parameter_blob_append_u32(builder, variables)) {
            return false;
        }
        for (uint32_t variable = 0; variable < variables; variable++) {
            if (!parameter_blob_append_string(builder, "V")) return false;
            for (uint32_t layout = 0; layout < 6; layout++) {
                if (!parameter_blob_append_u32(
                        builder, variable * 10u + layout)) {
                    return false;
                }
            }
        }
        if (!parameter_blob_append_u32(builder, structures)) return false;
        for (uint32_t structure = 0; structure < structures; structure++) {
            uint32_t members = structure == 0 ? member_count : 0;
            if (!parameter_blob_append_string(builder, "S")) return false;
            for (uint32_t layout = 0; layout < 3; layout++) {
                if (!parameter_blob_append_u32(
                        builder, structure * 10u + layout)) {
                    return false;
                }
            }
            if (!parameter_blob_append_u32(builder, members)) return false;
            for (uint32_t member = 0; member < members; member++) {
                if (!parameter_blob_append_string(builder, "M")) return false;
                for (uint32_t layout = 0; layout < 6; layout++) {
                    if (!parameter_blob_append_u32(
                            builder, member * 10u + layout)) {
                        return false;
                    }
                }
            }
        }
    }
    if (!parameter_blob_append_u32(builder, resource_count)) return false;
    for (uint32_t resource = 0; resource < resource_count; resource++) {
        if (!parameter_blob_append_string(builder, "R") ||
            !parameter_blob_append_u32(
                builder, SERIALIZED_RESOURCE_BUFFER) ||
            !parameter_blob_append_u32(builder, resource) ||
            !parameter_blob_append_u32(builder, resource + 1)) {
            return false;
        }
    }
    return true;
}

static int test_binary_texture_resource_dialect(void) {
    ParameterBlobBuilder builder;
    memset(&builder, 0, sizeof(builder));
    CHECK(parameter_blob_append_u32(
        &builder, UNITY_2021_3_PLAYER_BLOB_VERSION));
    CHECK(parameter_blob_append_u32(&builder, 0)); /* cbuffer count */
    CHECK(parameter_blob_append_u32(&builder, 1)); /* resource count */
    CHECK(parameter_blob_append_string(&builder, "TextureA"));
    CHECK(parameter_blob_append_u32(&builder, SERIALIZED_RESOURCE_TEXTURE));
    CHECK(parameter_blob_append_u32(&builder, 3));
    CHECK(parameter_blob_append_u32(&builder, 5));
    CHECK(parameter_blob_append_u32(&builder, (2u << 1) | 1u));

    ByteStream stream;
    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    stream_init(&stream, builder.bytes, builder.size);
    CHECK(subprogram_metadata_parse_parameters(&stream, &parameters));
    CHECK(stream_remaining(&stream) == 0);
    CHECK(parameters.version == UNITY_2021_3_PLAYER_BLOB_VERSION);
    CHECK(parameters.dialect == PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1);
    CHECK(parameters.res_count == 1);
    const SerializedResourceParam* texture = &parameters.resources[0];
    CHECK(texture->bind_type == SERIALIZED_RESOURCE_TEXTURE);
    CHECK(texture->bind_index == 3);
    CHECK(texture->sampler_index == 5);
    CHECK(texture->dimension == 2);
    CHECK(texture->multisampled);

    /* The layout is selected by an explicit dialect record.  Adjacent and
     * historical version integers must not be decoded with this shape. */
    static const uint32_t unsupported_versions[] = {
        UNITY_2021_3_PLAYER_BLOB_VERSION - 1u,
        UNITY_2021_3_PLAYER_BLOB_VERSION + 1u,
        201708219u,
        201708220u,
        201802150u,
    };
    for (size_t index = 0;
         index < sizeof(unsupported_versions) /
                     sizeof(unsupported_versions[0]);
         ++index) {
        uint32_t saved_version = 0;
        memcpy(&saved_version, builder.bytes, sizeof(saved_version));
        memcpy(builder.bytes, &unsupported_versions[index],
               sizeof(unsupported_versions[index]));
        stream_init(&stream, builder.bytes, builder.size);
        CHECK(!subprogram_metadata_parse_parameters(&stream, &parameters));
        CHECK(parameters.version == UNITY_2021_3_PLAYER_BLOB_VERSION);
        CHECK(parameters.dialect ==
              PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1);
        memcpy(builder.bytes, &saved_version, sizeof(saved_version));
    }

    builder.bytes[builder.size++] = 0xcc;
    stream_init(&stream, builder.bytes, builder.size);
    CHECK(!subprogram_metadata_parse_parameters(&stream, &parameters));
    CHECK(parameters.version == UNITY_2021_3_PLAYER_BLOB_VERSION);
    CHECK(parameters.dialect == PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1);
    serialized_program_parameters_free(&parameters);
    return 0;
}

static int test_dynamic_parameter_collections(void) {
    const size_t allocation_count_before = g_allocations_count;
    const size_t allocated_bytes_before = g_allocated_bytes;

    ParameterBlobBuilder large;
    CHECK(build_parameter_collection_blob(&large, 17, 65, 17, 65, 65));
    ByteStream stream;
    stream_init(&stream, large.bytes, large.size);
    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    CHECK(subprogram_metadata_parse_parameters(&stream, &parameters));
    CHECK(parameters.cb_count == 17);
    CHECK(parameters.constant_buffers != NULL);
    CHECK(strcmp(parameters.constant_buffers[0].name, "$Globals") == 0);
    CHECK(parameters.constant_buffers[0].role ==
          SERIALIZED_CBUFFER_LOOSE_PARAMETERS);
    CHECK(parameters.constant_buffers[1].role ==
          SERIALIZED_CBUFFER_NAMED);
    CHECK(parameters.constant_buffers[0].var_count == 65);
    CHECK(parameters.constant_buffers[0].variables[64].layout[0] == 640);
    CHECK(parameters.constant_buffers[0].struct_count == 17);
    CHECK(parameters.constant_buffers[0].struct_params[0].member_count == 65);
    CHECK(parameters.constant_buffers[0]
              .struct_params[0]
              .members[64]
              .layout[0] == 640);
    CHECK(parameters.res_count == 65);
    CHECK(parameters.resources[64].bind_index == 64);
    CHECK(parameters.resources[64].array_size == 65);

    SerializedProgramParameters copied;
    serialized_program_parameters_init(&copied);
    CHECK(serialized_program_parameters_copy(&copied, &parameters));
    CHECK(copied.constant_buffers[0].role ==
          SERIALIZED_CBUFFER_LOOSE_PARAMETERS);
    CHECK(copied.constant_buffers[1].role ==
          SERIALIZED_CBUFFER_NAMED);
    CHECK(copied.constant_buffers != parameters.constant_buffers);
    CHECK(copied.constant_buffers[0].variables !=
          parameters.constant_buffers[0].variables);
    CHECK(copied.constant_buffers[0].struct_params !=
          parameters.constant_buffers[0].struct_params);
    CHECK(copied.constant_buffers[0].struct_params[0].members !=
          parameters.constant_buffers[0].struct_params[0].members);
    CHECK(copied.resources != parameters.resources);
    parameters.constant_buffers[0].variables[64].layout[0] = 9999;
    parameters.constant_buffers[0].struct_params[0].members[64].layout[0] =
        9999;
    parameters.resources[64].bind_index = 9999;
    CHECK(copied.constant_buffers[0].variables[64].layout[0] == 640);
    CHECK(copied.constant_buffers[0]
              .struct_params[0]
              .members[64]
              .layout[0] == 640);
    CHECK(copied.resources[64].bind_index == 64);
    CHECK(serialized_program_parameters_copy(&copied, &copied));
    serialized_program_parameters_free(&parameters);
    CHECK(copied.cb_count == 17 && copied.res_count == 65);
    serialized_program_parameters_free(&copied);

    ParameterBlobBuilder compact;
    CHECK(build_parameter_collection_blob(&compact, 1, 1, 1, 1, 1));
    for (size_t length = 0; length < compact.size; length++) {
        SerializedProgramParameters truncated;
        serialized_program_parameters_init(&truncated);
        stream_init(&stream, compact.bytes, length);
        CHECK(!subprogram_metadata_parse_parameters(&stream, &truncated));
        serialized_program_parameters_free(&truncated);
        CHECK(g_allocations_count == allocation_count_before);
        CHECK(g_allocated_bytes == allocated_bytes_before);
    }

    SerializedProgramParameters retained;
    serialized_program_parameters_init(&retained);
    stream_init(&stream, compact.bytes, compact.size);
    CHECK(subprogram_metadata_parse_parameters(&stream, &retained));
    SerializedConstantBuffer* retained_buffers = retained.constant_buffers;
    compact.bytes[compact.size++] = 0xcc;
    stream_init(&stream, compact.bytes, compact.size);
    CHECK(!subprogram_metadata_parse_parameters(&stream, &retained));
    CHECK(retained.constant_buffers == retained_buffers);
    CHECK(retained.cb_count == 1 && retained.res_count == 1);

    SerializedProgramParameters malformed_source;
    serialized_program_parameters_init(&malformed_source);
    malformed_source.cb_count = 1;
    malformed_source.constant_buffers = (SerializedConstantBuffer*)mem_alloc(
        sizeof(*malformed_source.constant_buffers));
    CHECK(malformed_source.constant_buffers != NULL);
    memset(malformed_source.constant_buffers, 0,
           sizeof(*malformed_source.constant_buffers));
    malformed_source.constant_buffers[0].var_count = 1;
    CHECK(!serialized_program_parameters_copy(&retained,
                                               &malformed_source));
    CHECK(retained.constant_buffers == retained_buffers);
    CHECK(retained.cb_count == 1 && retained.res_count == 1);
    serialized_program_parameters_free(&malformed_source);
    serialized_program_parameters_free(&retained);

    const uint32_t impossible_count_words[] = {
        UNITY_2021_3_PLAYER_BLOB_VERSION, UINT32_MAX,
    };
    SerializedProgramParameters impossible;
    serialized_program_parameters_init(&impossible);
    stream_init(&stream, (const uint8_t*)impossible_count_words,
                sizeof(impossible_count_words));
    CHECK(!subprogram_metadata_parse_parameters(&stream, &impossible));
    serialized_program_parameters_free(&impossible);

    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    return 0;
}

static int test_long_parameter_names_and_copy_ownership(void) {
    const size_t allocation_count_before = g_allocations_count;
    const size_t allocated_bytes_before = g_allocated_bytes;
    char long_name[301];
    for (size_t i = 0; i + 1U < sizeof(long_name); ++i) {
        long_name[i] = (char)('A' + (i % 26U));
    }
    long_name[sizeof(long_name) - 1U] = '\0';

    ParameterBlobBuilder builder;
    memset(&builder, 0, sizeof(builder));
    CHECK(parameter_blob_append_u32(&builder, UNITY_2021_3_PLAYER_BLOB_VERSION));
    CHECK(parameter_blob_append_u32(&builder, 1U));
    CHECK(parameter_blob_append_string(&builder, long_name));
    CHECK(parameter_blob_append_u32(&builder, 64U));
    CHECK(parameter_blob_append_u32(&builder, 1U));
    CHECK(parameter_blob_append_string(&builder, long_name));
    for (uint32_t i = 0; i < 6U; ++i) {
        CHECK(parameter_blob_append_u32(&builder, i));
    }
    CHECK(parameter_blob_append_u32(&builder, 1U));
    CHECK(parameter_blob_append_string(&builder, long_name));
    for (uint32_t i = 0; i < 3U; ++i) {
        CHECK(parameter_blob_append_u32(&builder, i + 10U));
    }
    CHECK(parameter_blob_append_u32(&builder, 1U));
    CHECK(parameter_blob_append_string(&builder, long_name));
    for (uint32_t i = 0; i < 6U; ++i) {
        CHECK(parameter_blob_append_u32(&builder, i + 20U));
    }
    CHECK(parameter_blob_append_u32(&builder, 1U));
    CHECK(parameter_blob_append_string(&builder, long_name));
    CHECK(parameter_blob_append_u32(
        &builder, (uint32_t)SERIALIZED_RESOURCE_BUFFER));
    CHECK(parameter_blob_append_u32(&builder, 3U));
    CHECK(parameter_blob_append_u32(&builder, 1U));

    ByteStream stream;
    stream_init(&stream, builder.bytes, builder.size);
    SerializedProgramParameters parsed;
    SerializedProgramParameters copied;
    serialized_program_parameters_init(&parsed);
    serialized_program_parameters_init(&copied);
    CHECK(subprogram_metadata_parse_parameters(&stream, &parsed));
    CHECK(strlen(parsed.constant_buffers[0].name) == 300U);
    CHECK(strcmp(parsed.constant_buffers[0].name, long_name) == 0);
    CHECK(strcmp(parsed.constant_buffers[0].variables[0].name,
                 long_name) == 0);
    CHECK(strcmp(parsed.constant_buffers[0].struct_params[0].name,
                 long_name) == 0);
    CHECK(strcmp(parsed.constant_buffers[0]
                     .struct_params[0]
                     .members[0]
                     .name,
                 long_name) == 0);
    CHECK(strcmp(parsed.resources[0].name, long_name) == 0);

    CHECK(serialized_program_parameters_copy(&copied, &parsed));
    CHECK(strcmp(copied.resources[0].name, long_name) == 0);
    CHECK(copied.constant_buffers[0].name !=
          parsed.constant_buffers[0].name);
    CHECK(copied.constant_buffers[0].variables[0].name !=
          parsed.constant_buffers[0].variables[0].name);
    CHECK(copied.constant_buffers[0].struct_params[0].name !=
          parsed.constant_buffers[0].struct_params[0].name);
    CHECK(copied.constant_buffers[0]
              .struct_params[0]
              .members[0]
              .name != parsed.constant_buffers[0]
                           .struct_params[0]
                           .members[0]
                           .name);
    CHECK(copied.resources[0].name != parsed.resources[0].name);

    serialized_program_parameters_free(&parsed);
    CHECK(strcmp(copied.resources[0].name, long_name) == 0);
    serialized_program_parameters_free(&copied);
    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    return 0;
}

static void init_typetree_node(TypeTreeNode* node, uint8_t level,
                               int32_t byte_size, const char* type_name,
                               const char* field_name) {
    memset(node, 0, sizeof(*node));
    node->level = level;
    node->byte_size = byte_size;
    node->type_str = type_name;
    node->name_str = field_name;
}

static int test_typetree_array_alignment_authority(void) {
    size_t allocation_count_before = g_allocations_count;
    size_t allocated_bytes_before = g_allocated_bytes;
    TypeTreeNode nodes[5];
    TypeTreeType type;
    TypeTreeValue value;
    ByteStream stream;
    int node_index = 0;

    memset(nodes, 0, sizeof(nodes));
    memset(&type, 0, sizeof(type));
    init_typetree_node(&nodes[0], 0, -1, "vector", "values");
    init_typetree_node(&nodes[1], 1, -1, "Array", "Array");
    init_typetree_node(&nodes[2], 2, 4, "int", "size");
    init_typetree_node(&nodes[3], 2, 1, "UInt8", "data");
    type.nodes = nodes;
    type.node_count = 4;

    /* Alignment on the vector applies once after all elements. */
    const uint8_t parent_aligned[] = {
        3, 0, 0, 0, 0x11, 0x22, 0x33, 0xee,
    };
    nodes[0].meta_flags = 0x4000u;
    memset(&value, 0, sizeof(value));
    stream_init(&stream, parent_aligned, sizeof(parent_aligned));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &value));
    CHECK(stream.position == sizeof(parent_aligned));
    CHECK(value.array_val.count == 3);
    CHECK(value.array_val.elements[0].uint_val == 0x11);
    CHECK(value.array_val.elements[2].uint_val == 0x33);
    typetree_free_value(&value);

    /* Unity's element-schema alignment bit is array authority. It must not
     * align every primitive element independently. */
    const uint8_t element_aligned[] = {
        2, 0, 0, 0, 0x44, 0x55, 0xee, 0xee,
    };
    nodes[0].meta_flags = 0;
    nodes[3].meta_flags = 0x4000u;
    node_index = 0;
    memset(&value, 0, sizeof(value));
    stream_init(&stream, element_aligned, sizeof(element_aligned));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &value));
    CHECK(stream.position == sizeof(element_aligned));
    CHECK(value.array_val.elements[0].uint_val == 0x44);
    CHECK(value.array_val.elements[1].uint_val == 0x55);
    typetree_free_value(&value);

    /* Packed byte arrays preserve the same alignment semantics. */
    node_index = 0;
    memset(&value, 0, sizeof(value));
    stream_init(&stream, element_aligned, sizeof(element_aligned));
    CHECK(typetree_parse_value_ex(&type, &node_index, &stream, &value,
                                  TYPETREE_PARSE_PACK_BYTE_ARRAYS));
    CHECK(stream.position == sizeof(element_aligned));
    CHECK(value.array_val.storage == TYPETREE_ARRAY_PACKED_BYTES);
    CHECK(value.array_val.packed_bytes[0] == 0x44);
    CHECK(value.array_val.packed_bytes[1] == 0x55);
    typetree_free_value(&value);

    /* Alignment owned by a nested member remains per struct element. */
    init_typetree_node(&nodes[3], 2, -1, "AlignedByte", "data");
    init_typetree_node(&nodes[4], 3, 1, "UInt8", "value");
    nodes[4].meta_flags = 0x4000u;
    type.node_count = 5;
    const uint8_t nested_aligned[] = {
        2, 0, 0, 0,
        0x66, 0xee, 0xee, 0xee,
        0x77, 0xee, 0xee, 0xee,
    };
    node_index = 0;
    memset(&value, 0, sizeof(value));
    stream_init(&stream, nested_aligned, sizeof(nested_aligned));
    CHECK(typetree_parse_value(&type, &node_index, &stream, &value));
    CHECK(stream.position == sizeof(nested_aligned));
    CHECK(value.array_val.elements[0].struct_val.members[0].uint_val == 0x66);
    CHECK(value.array_val.elements[1].struct_val.members[0].uint_val == 0x77);
    typetree_free_value(&value);

    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    return 0;
}

static int test_serialized_shader_profile_field_policy(void) {
    const size_t allocation_count_before = g_allocations_count;
    const size_t allocated_bytes_before = g_allocated_bytes;

    TypeTreeValue name = test_string_value("m_Name", "ProfileFixture");
    TypeTreeValue parsed_members[] = {name};
    TypeTreeValue parsed = test_struct_value("m_ParsedForm", parsed_members, 1);
    TypeTreeValue root_members[] = {parsed};
    TypeTreeValue root = test_struct_value("Shader", root_members, 1);
    SerializedShader shader;
    serialized_shader_init(&shader);
    /* The exact profile entry point accepts only schema-derived complete
     * values.  A convenient projection tree is not wire authority. */
    CHECK(!serialized_shader_parse_with_profile(
        &shader, &root, SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1));
    CHECK(shader.name == NULL);

    char long_name[301];
    for (size_t i = 0; i + 1U < sizeof(long_name); ++i) {
        long_name[i] = (char)('a' + (i % 26U));
    }
    long_name[sizeof(long_name) - 1U] = '\0';
    TypeTreeValue default_texture_members[] = {
        test_string_value("m_DefaultName", long_name),
        test_int_value("m_TexDim", 2),
    };
    TypeTreeValue default_texture = test_struct_value(
        "m_DefTexture", default_texture_members,
        (int)(sizeof(default_texture_members) /
              sizeof(default_texture_members[0])));
    TypeTreeValue attribute_values[] = {
        test_string_value("data", long_name),
    };
    TypeTreeValue attributes = test_array_value(
        "m_Attributes", attribute_values, 1);
    TypeTreeValue property_members[] = {
        test_string_value("m_Name", long_name),
        test_string_value("m_Description", long_name),
        test_int_value("m_Type", 4),
        test_int_value("m_Flags", 0),
        default_texture,
        attributes,
    };
    TypeTreeValue property = test_struct_value(
        "data", property_members,
        (int)(sizeof(property_members) / sizeof(property_members[0])));
    TypeTreeValue property_values[] = {property};
    TypeTreeValue properties = test_array_value(
        "m_Props", property_values, 1);
    TypeTreeValue property_info_members[] = {properties};
    TypeTreeValue property_info = test_struct_value(
        "m_PropInfo", property_info_members, 1);
    TypeTreeValue long_parsed_members[] = {
        test_string_value("m_Name", long_name),
        property_info,
    };
    TypeTreeValue long_parsed = test_struct_value(
        "m_ParsedForm", long_parsed_members, 2);
    TypeTreeValue long_root_members[] = {long_parsed};
    TypeTreeValue long_root = test_struct_value(
        "Shader", long_root_members, 1);
    CHECK(serialized_shader_parse(&shader, &long_root));
    CHECK(strlen(shader.name) == 300U);
    CHECK(strcmp(shader.name, long_name) == 0);
    CHECK(shader.name != long_name);
    CHECK(shader.property_count == 1);
    CHECK(strcmp(shader.properties[0].name, long_name) == 0);
    CHECK(strcmp(shader.properties[0].description, long_name) == 0);
    CHECK(strcmp(shader.properties[0].def_texture_name, long_name) == 0);
    CHECK(strcmp(shader.properties[0].attributes[0], long_name) == 0);
    const char* retained_long_name = shader.name;

    /* The profile explicitly permits an absent fallback, but a present field
     * with the wrong wire type is never equivalent to absence. */
    TypeTreeValue wrong_fallback = test_int_value("m_FallbackName", 0);
    TypeTreeValue malformed_members[] = {name, wrong_fallback};
    root_members[0].struct_val.members = malformed_members;
    root_members[0].struct_val.count = 2;
    CHECK(!serialized_shader_parse(&shader, &root));
    CHECK(shader.name == retained_long_name);
    TypeTreeValue wrong_keyword_array =
        test_int_value("m_KeywordNames", 0);
    TypeTreeValue malformed_array_members[] = {name, wrong_keyword_array};
    root_members[0].struct_val.members = malformed_array_members;
    root_members[0].struct_val.count = 2;
    CHECK(!serialized_shader_parse(&shader, &root));
    CHECK(shader.name == retained_long_name);

    /* Required fields and embedded-NUL identifiers fail transactionally. */
    root_members[0].struct_val.members = NULL;
    root_members[0].struct_val.count = 0;
    CHECK(!serialized_shader_parse(&shader, &root));
    CHECK(shader.name == retained_long_name);
    TypeTreeValue nul_name = test_string_value("m_Name", "A");
    nul_name.string_val = "A\0B";
    nul_name.string_length = 3;
    root_members[0].struct_val.members = &nul_name;
    root_members[0].struct_val.count = 1;
    CHECK(!serialized_shader_parse(&shader, &root));
    CHECK(shader.name == retained_long_name);
    CHECK(!serialized_shader_parse_with_profile(
        &shader, &root, (SerializedShaderSchemaProfile)999));
    CHECK(shader.name == retained_long_name);

    serialized_shader_free(&shader);

    CHECK(g_allocations_count == allocation_count_before);
    CHECK(g_allocated_bytes == allocated_bytes_before);
    return 0;
}

static int test_player_typetree_local_keyword_set_identity(void) {
    char* player_order[] = {
        (char*)"DIRECTIONAL",
        (char*)"DIRLIGHTMAP_COMBINED",
        (char*)"DYNAMICLIGHTMAP_ON",
    };
    char* serialized_order[] = {
        (char*)"DIRECTIONAL",
        (char*)"DYNAMICLIGHTMAP_ON",
        (char*)"DIRLIGHTMAP_COMBINED",
    };
    PlayerSubProgramMetadata player;
    SerializedSubProgram serialized;
    memset(&player, 0, sizeof(player));
    memset(&serialized, 0, sizeof(serialized));
    player.local_keyword_count = 3;
    player.local_keywords = player_order;
    serialized.local_keyword_count = 3;
    serialized.local_keywords = serialized_order;
    CHECK(subprogram_metadata_local_keyword_set_matches(
        &player, &serialized));

    /* Every mutation of set identity fails closed, including duplicate rows
     * that could otherwise make a one-way membership check pass. */
    serialized_order[2] = (char*)"LIGHTMAP_ON";
    CHECK(!subprogram_metadata_local_keyword_set_matches(
        &player, &serialized));
    serialized_order[2] = (char*)"DYNAMICLIGHTMAP_ON";
    CHECK(!subprogram_metadata_local_keyword_set_matches(
        &player, &serialized));
    serialized_order[2] = (char*)"DIRLIGHTMAP_COMBINED";
    player_order[2] = (char*)"DIRLIGHTMAP_COMBINED";
    CHECK(!subprogram_metadata_local_keyword_set_matches(
        &player, &serialized));
    player_order[2] = (char*)"DYNAMICLIGHTMAP_ON";
    serialized.local_keywords[1] = NULL;
    CHECK(!subprogram_metadata_local_keyword_set_matches(
        &player, &serialized));
    serialized.local_keywords[1] = (char*)"DYNAMICLIGHTMAP_ON";
    serialized.local_keyword_count = 2;
    CHECK(!subprogram_metadata_local_keyword_set_matches(
        &player, &serialized));
    CHECK(!subprogram_metadata_local_keyword_set_matches(NULL, &serialized));
    return 0;
}

int main(void) {
    SerializedShaderSchemaProfile schema_profile =
        (SerializedShaderSchemaProfile)0;
    CHECK(serialized_shader_profile_from_unity_version(
        "2021.3.35f1", &schema_profile));
    CHECK(schema_profile ==
          SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1);
    CHECK(!serialized_shader_profile_from_unity_version(
        "2021.3.34f1", &schema_profile));
    CHECK(!serialized_shader_profile_from_unity_version(
        "2021.3.35f1-extra", &schema_profile));
    CHECK(!serialized_shader_profile_from_unity_version(
        NULL, &schema_profile));
    CHECK(!serialized_shader_profile_from_unity_version(
        "2021.3.35f1", NULL));

    CHECK(test_serialized_shader_variant_identity() == 0);
    CHECK(test_player_typetree_local_keyword_set_identity() == 0);
    CHECK(test_serialized_shader_profile_field_policy() == 0);
    CHECK(test_typetree_common_parameter_projection() == 0);
    CHECK(test_typetree_dynamic_parameter_collections() == 0);
    CHECK(test_binary_texture_resource_dialect() == 0);
    CHECK(test_dynamic_parameter_collections() == 0);
    CHECK(test_long_parameter_names_and_copy_ownership() == 0);
    CHECK(test_typetree_array_alignment_authority() == 0);
    CHECK(test_embedded_nul_pascal_strings() == 0);

    static SerializedProgramParameters params;
    serialized_program_parameters_init(&params);
    params.res_count = 3;
    params.resources = (SerializedResourceParam*)mem_alloc(
        (size_t)params.res_count * sizeof(*params.resources));
    CHECK(params.resources != NULL);
    memset(params.resources, 0,
           (size_t)params.res_count * sizeof(*params.resources));

    SerializedVariable variable;
    DecodedVariableLayout layout;
    memset(&variable, 0, sizeof(variable));
    params.is_binary = true;
    variable.layout[0] = 0;   /* float */
    variable.layout[1] = 3;   /* matrix row count */
    variable.layout[2] = 0;   /* unused for matrices */
    variable.layout[3] = 1;   /* matrix */
    variable.layout[4] = 2;   /* array elements */
    variable.layout[5] = 64;  /* byte offset */
    CHECK(parameter_layout_decode(&params, &variable, &layout));
    CHECK(layout.byte_offset == 64);
    CHECK(layout.rows == 3 && layout.columns == 4);
    CHECK(layout.is_matrix && layout.array_size == 2);
    CHECK(parameter_layout_register_count(&layout) == 6);
    CHECK(parameter_layout_byte_size(&layout) == 96);

    memset(&variable, 0, sizeof(variable));
    params.is_binary = false;
    variable.layout[0] = 16;  /* byte offset */
    variable.layout[1] = 4;   /* array elements */
    variable.layout[2] = 1;   /* int */
    variable.layout[3] = 2;   /* vector dimension */
    variable.layout[4] = 0;   /* vector */
    CHECK(parameter_layout_decode(&params, &variable, &layout));
    CHECK(layout.byte_offset == 16 && layout.scalar_type == 1);
    CHECK(layout.rows == 1 && layout.columns == 2);
    CHECK(!layout.is_matrix && layout.array_size == 4);
    CHECK(parameter_layout_register_count(&layout) == 4);
    CHECK(parameter_layout_byte_size(&layout) == 56);

    /* A non-matrix array starts each element on a 16-byte register, but its
     * final element consumes only the declared lanes.  These are allocation
     * spans, not rounded register footprints; a following scalar may legally
     * occupy the tail of the last register. */
    memset(&layout, 0, sizeof(layout));
    layout.rows = 1;
    layout.array_size = 4;
    for (uint32_t columns = 1; columns <= 4; ++columns) {
        layout.columns = columns;
        CHECK(parameter_layout_byte_size(&layout) == 48u + columns * 4u);
    }
    layout.array_size = 1;
    layout.columns = 1;
    CHECK(parameter_layout_byte_size(&layout) == 4);
    layout.array_size = 0;
    layout.columns = 3;
    CHECK(parameter_layout_byte_size(&layout) == 12);
    layout.array_size = UINT32_MAX;
    layout.columns = 4;
    CHECK(parameter_layout_byte_size(&layout) == 0);

    set_resource(&params.resources[0], "_MainTex",
                 SERIALIZED_RESOURCE_TEXTURE, 3, 7);
    const char* resolved = resolve_texture_name(&params, 3);
    CHECK(resolved && strcmp(resolved, "_MainTex") == 0);

    /* Unity texture and buffer reflection records share D3D's t-register
     * namespace.  The exact serialized kind, not record order, selects the
     * source identifier. */
    set_resource(&params.resources[1], "_WaveformBuffer",
                 SERIALIZED_RESOURCE_BUFFER, 3, 7);
    const char *srv_name = NULL;
    CHECK(resolve_srv_name(&params, 3, SERIALIZED_RESOURCE_TEXTURE,
                           &srv_name));
    CHECK(srv_name && strcmp(srv_name, "_MainTex") == 0);
    CHECK(resolve_srv_name(&params, 3, SERIALIZED_RESOURCE_BUFFER,
                           &srv_name));
    CHECK(srv_name && strcmp(srv_name, "_WaveformBuffer") == 0);
    CHECK(resolve_texture_name(&params, 3) == params.resources[0].name);

    /* Duplicate authority of one exact kind is not equivalent to absence and
     * cannot fall through to an otherwise valid common binding. */
    set_resource(&params.resources[2], "_DuplicateTex",
                 SERIALIZED_RESOURCE_TEXTURE, 3, 0);
    CHECK(!resolve_srv_name(&params, 3, SERIALIZED_RESOURCE_TEXTURE,
                            &srv_name));
    CHECK(srv_name == NULL);
    CHECK(resolve_texture_name(&params, 3) == NULL);
    SerializedResourceParam common_texture;
    set_resource(&common_texture, "_CommonTex",
                 SERIALIZED_RESOURCE_TEXTURE, 3, 0);
    SerializedProgramParameters common_lookup;
    serialized_program_parameters_init(&common_lookup);
    common_lookup.resources = &common_texture;
    common_lookup.res_count = 1;
    HLSLEmitterContext lookup_context;
    memset(&lookup_context, 0, sizeof(lookup_context));
    lookup_context.params = &params;
    lookup_context.common_params = &common_lookup;
    CHECK(!resolve_srv_name_ctx(&lookup_context, 3,
                                SERIALIZED_RESOURCE_TEXTURE, &srv_name));
    CHECK(srv_name == NULL);

    set_resource(&params.resources[2], "",
                 SERIALIZED_RESOURCE_TEXTURE, 8, 0);
    CHECK(!resolve_srv_name(&params, 8, SERIALIZED_RESOURCE_TEXTURE,
                            &srv_name));
    CHECK(srv_name == NULL);

    memset(&params.resources[2], 0, sizeof(params.resources[2]));
    char *resolved_sampler = NULL;
    CHECK(resolve_sampler_name(&params, 7, &resolved_sampler));
    CHECK(resolved_sampler && strcmp(resolved_sampler,
                                     "sampler_MainTex") == 0);
    free(resolved_sampler);
    resolved_sampler = NULL;
    CHECK(resolve_sampler_name(&params, 3, &resolved_sampler));
    CHECK(resolved_sampler == NULL);

    set_resource(&params.resources[1], "_DetailTex",
                 SERIALIZED_RESOURCE_TEXTURE, 4, 7);
    CHECK(resolve_sampler_name(&params, 7, &resolved_sampler));
    CHECK(resolved_sampler == NULL);

    set_resource(&params.resources[2], "SharedSampler",
                 SERIALIZED_RESOURCE_SAMPLER, 7, 0);
    params.resources[2].sampler_state = 0x09e6u;
    CHECK(resolve_sampler_name(&params, 7, &resolved_sampler));
    CHECK(resolved_sampler != NULL);
    CHECK(strcmp(resolved_sampler,
                 "sampler_dxbc_s7_trilinear_clampu_mirrorv_"
                 "mirroroncew_compare_aniso16") == 0);
    free(resolved_sampler);
    resolved_sampler = NULL;

    /* The inverse accepts only Unity's exact inline-sampler language. */
    params.resources[2].sampler_state = 3u; /* reserved filter encoding */
    CHECK(!resolve_sampler_name(&params, 7, &resolved_sampler));
    CHECK(resolved_sampler == NULL);
    params.resources[2].sampler_state = 5u << 9u; /* unsupported aniso32 */
    CHECK(!resolve_sampler_name(&params, 7, &resolved_sampler));
    CHECK(resolved_sampler == NULL);
    params.resources[2].sampler_state = 0x1000u; /* unknown high bit */
    CHECK(!resolve_sampler_name(&params, 7, &resolved_sampler));
    CHECK(resolved_sampler == NULL);

    set_resource(&params.resources[1], "", SERIALIZED_RESOURCE_SAMPLER,
                 7, 0);
    params.resources[1].sampler_state = 0x54u;
    params.resources[2].sampler_state = 0x54u;
    CHECK(!resolve_sampler_name(&params, 7, &resolved_sampler));
    CHECK(resolved_sampler == NULL);
    set_resource(&params.resources[1], "_DetailTex",
                 SERIALIZED_RESOURCE_TEXTURE, 4, 7);

    /* The real Preview3DVolume pattern has several texture associations but
     * an independently serialized state for the architectural sampler. */
    params.resources[2].sampler_state = 0x54u;

    static USILProgram program;
    static USILInstruction sample_instructions[2];
    static HLSLEmitterContext sample_context;
    memset(&program, 0, sizeof(program));
    memset(sample_instructions, 0, sizeof(sample_instructions));
    memset(&sample_context, 0, sizeof(sample_context));
    sample_instructions[0].opcode = USIL_OP_SAMPLE;
    sample_instructions[0].operand_count = 2;
    sample_instructions[0].operands[0].type = OPERAND_TYPE_RESOURCE;
    sample_instructions[0].operands[0].register_index = 3;
    sample_instructions[0].operands[1].type = OPERAND_TYPE_SAMPLER;
    sample_instructions[0].operands[1].register_index = 7;
    program.instructions = sample_instructions;
    program.instruction_count = 1;
    sample_context.program = &program;
    sample_context.params = &params;
    CHECK(build_sampler_name_map(&sample_context));
    CHECK(sample_context.sampler_names[7] != NULL);
    CHECK(sample_context.sampler_names[7][0] != '\0');
    CHECK(strcmp(sample_context.sampler_names[7],
                 "sampler_dxbc_s7_point_clampu_clampv_clampw") == 0);

    sample_instructions[1] = sample_instructions[0];
    sample_instructions[1].operands[0].register_index = 4;
    program.instruction_count = 2;
    CHECK(build_sampler_name_map(&sample_context));
    CHECK(strcmp(sample_context.sampler_names[7],
                 "sampler_dxbc_s7_point_clampu_clampv_clampw") == 0);
    free_sampler_name_map(&sample_context);

    static USILInstruction structured_instruction;
    memset(&structured_instruction, 0, sizeof(structured_instruction));
    memset(&program, 0, sizeof(program));
    program.texture_count = 1;
    program.texture_alloc = 1;
    static USILTexture structured_texture;
    program.textures = &structured_texture;
    structured_texture.reg_idx = 0;
    structured_texture.stride = 56;
    strcpy(structured_texture.dimension, "structured");
    structured_instruction.opcode = USIL_OP_LD_STRUCTURED;
    structured_instruction.operand_count = 4;
    structured_instruction.operands[0].destination_mask = 0x70;
    structured_instruction.operands[2].type = OPERAND_TYPE_IMMEDIATE32;
    structured_instruction.operands[2].imm_value_count = 1;
    structured_instruction.operands[2].imm_values[0] = 24;
    structured_instruction.operands[3].type = OPERAND_TYPE_RESOURCE;
    structured_instruction.operands[3].register_index = 0;
    program.instructions = &structured_instruction;
    program.instruction_count = 1;
    StructuredResourceLayout structured_layout;
    CHECK(get_structured_resource_layout(&program, 0, &structured_layout));
    CHECK(structured_layout.stride == 56);
    CHECK(structured_layout.field_count == 1);
    CHECK(structured_layout.offsets[0] == 24);
    CHECK(structured_layout.widths[0] == 3);
    free_structured_resource_layout(&structured_layout);

    /* The field inventory is data-derived.  Sixty-five distinct, legal
     * accesses used to hit an unrelated corpus-shaped ceiling of 64. */
    static USILInstruction structured_instructions[65];
    memset(structured_instructions, 0, sizeof(structured_instructions));
    structured_texture.stride = 260;
    program.instructions = structured_instructions;
    program.instruction_count = 65;
    program.instruction_alloc = 65;
    for (int field = 0; field < 65; ++field) {
        USILInstruction *field_instruction =
            &structured_instructions[field];
        field_instruction->opcode = USIL_OP_LD_STRUCTURED;
        field_instruction->operand_count = 4;
        field_instruction->operands[0].destination_mask = 0x10;
        field_instruction->operands[2].type = OPERAND_TYPE_IMMEDIATE32;
        field_instruction->operands[2].imm_value_count = 1;
        field_instruction->operands[2].imm_values[0] =
            (uint32_t)field * 4u;
        field_instruction->operands[3].type = OPERAND_TYPE_RESOURCE;
        field_instruction->operands[3].register_index = 0;
    }
    CHECK(get_structured_resource_layout(&program, 0, &structured_layout));
    CHECK(structured_layout.field_count == 65);
    CHECK(structured_layout.field_alloc >= structured_layout.field_count);
    CHECK(structured_layout.offsets[64] == 256);
    CHECK(structured_layout.widths[64] == 1);
    free_structured_resource_layout(&structured_layout);

    static HLSLEmitterContext context;
    memset(&context, 0, sizeof(context));
    context.params = &params;
    set_resource(&params.resources[0], "FirstCB",
                 SERIALIZED_RESOURCE_CONSTANT_BUFFER, 2, 0);
    set_resource(&params.resources[1], "SecondCB",
                 SERIALIZED_RESOURCE_CONSTANT_BUFFER, 2, 0);
    CHECK(build_cbuffer_register_map(&context));
    CHECK(get_cbuffer_name_from_map(&context, 2) == NULL);
    CHECK(get_cbuffer_name_from_map(&context, 0) == NULL);

    const int expanded_resource_count = 65;
    SerializedResourceParam* expanded_resources =
        (SerializedResourceParam*)mem_realloc(
            params.resources,
            (size_t)params.res_count * sizeof(*params.resources),
            (size_t)expanded_resource_count * sizeof(*params.resources));
    CHECK(expanded_resources != NULL);
    params.resources = expanded_resources;
    memset(params.resources + params.res_count, 0,
           (size_t)(expanded_resource_count - params.res_count) *
               sizeof(*params.resources));
    params.res_count = expanded_resource_count;
    char expanded_resource_names[65][64];
    for (int i = 3; i < params.res_count; i++) {
        snprintf(expanded_resource_names[i],
                 sizeof(expanded_resource_names[i]),
                 "ConstantBuffer%d", i);
        set_resource(&params.resources[i], expanded_resource_names[i],
                     SERIALIZED_RESOURCE_CONSTANT_BUFFER, (uint32_t)i, 0);
    }
    CHECK(build_cbuffer_register_map(&context));
    CHECK(context.cb_reg_map_count > 16);
    CHECK(strcmp(get_cbuffer_name_from_map(&context, 64),
                 "ConstantBuffer64") == 0);
    free(context.cb_reg_map);
    context.cb_reg_map = NULL;
    context.cb_reg_map_count = 0;
    context.cb_reg_map_alloc = 0;

    const uint32_t variant_words[] = {
        UNITY_2021_3_PLAYER_BLOB_VERSION, 15u, 0x41u, 8u, 4u, 7u, 0u, 0u, 1u, 0u
    };
    ByteStream stream;
    PlayerSubProgramMetadata variant;
    CHECK(!subprogram_metadata_parse_variant(NULL, &variant));
    CHECK(!subprogram_metadata_parse_variant(&stream, NULL));
    subprogram_metadata_free_variant(NULL);
    stream_init(&stream, (const uint8_t*)variant_words, sizeof(variant_words));
    CHECK(subprogram_metadata_parse_variant(&stream, &variant));
    CHECK(stream_remaining(&stream) == 0);
    CHECK(variant.version == UNITY_2021_3_PLAYER_BLOB_VERSION);
    CHECK(variant.dialect == PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1);
    CHECK(variant.program_type == 15);
    CHECK(variant.has_player_blob_header);
    CHECK(variant.player_header_words[0] == 0x41u);
    CHECK(variant.player_header_words[1] == 8u);
    CHECK(variant.player_header_words[2] == 4u);
    CHECK(variant.player_header_words[3] == 7u);
    CHECK(variant.source_map == 1u);
    subprogram_metadata_free_variant(&variant);
    static const uint32_t unsupported_variant_versions[] = {
        UNITY_2021_3_PLAYER_BLOB_VERSION - 1u,
        UNITY_2021_3_PLAYER_BLOB_VERSION + 1u,
        201802150u,
    };
    for (size_t version_index = 0;
         version_index < sizeof(unsupported_variant_versions) /
                             sizeof(unsupported_variant_versions[0]);
         ++version_index) {
        uint32_t rejected_words[
            sizeof(variant_words) / sizeof(variant_words[0])];
        memcpy(rejected_words, variant_words, sizeof(rejected_words));
        rejected_words[0] = unsupported_variant_versions[version_index];
        stream_init(&stream, (const uint8_t*)rejected_words,
                    sizeof(rejected_words));
        CHECK(!subprogram_metadata_parse_variant(&stream, &variant));
    }
    for (size_t truncated = 0; truncated < sizeof(variant_words); ++truncated) {
        stream_init(&stream, (const uint8_t*)variant_words, truncated);
        CHECK(!subprogram_metadata_parse_variant(&stream, &variant));
    }

    /* UnityPlayer accepts ShaderChannel values 0..13 and VertexComponent
     * values 0..30.  The component is not a four-lane swizzle selector. */
    uint32_t binding_variant_words[] = {
        UNITY_2021_3_PLAYER_BLOB_VERSION, 15u, 3u, 0u, 2u, 0u, 0u, 0u, 1u, 1u, 13u, 30u
    };
    stream_init(&stream, (const uint8_t*)binding_variant_words,
                sizeof(binding_variant_words));
    CHECK(subprogram_metadata_parse_variant(&stream, &variant));
    CHECK(variant.binding_count == 1);
    CHECK(variant.bindings != NULL);
    CHECK(variant.bindings[0].channel == 13u);
    CHECK(variant.bindings[0].component == 30u);
    subprogram_metadata_free_variant(&variant);

    binding_variant_words[10] = 14u;
    stream_init(&stream, (const uint8_t*)binding_variant_words,
                sizeof(binding_variant_words));
    CHECK(!subprogram_metadata_parse_variant(&stream, &variant));
    binding_variant_words[10] = 13u;
    binding_variant_words[11] = 31u;
    stream_init(&stream, (const uint8_t*)binding_variant_words,
                sizeof(binding_variant_words));
    CHECK(!subprogram_metadata_parse_variant(&stream, &variant));

    static USILInstruction provenance_instructions[3];
    static USILProgram provenance_program;
    static HLSLEmitterContext provenance_context;
    memset(provenance_instructions, 0, sizeof(provenance_instructions));
    memset(&provenance_program, 0, sizeof(provenance_program));
    memset(&provenance_context, 0, sizeof(provenance_context));
    provenance_instructions[0].opcode = USIL_OP_MOV;
    provenance_instructions[0].operand_count = 2;
    provenance_instructions[0].operands[0].type = OPERAND_TYPE_TEMP;
    provenance_instructions[0].operands[0].register_index = 0;
    provenance_instructions[0].operands[0].destination_mask = 128;
    provenance_instructions[0].operands[1].type = OPERAND_TYPE_INPUT;
    provenance_instructions[0].operands[1].register_index = 0;
    provenance_instructions[0].operands[1].swizzle_mode = 2;
    provenance_instructions[0].operands[1].swizzle[0] = 3;
    provenance_instructions[1].opcode = USIL_OP_ADD;
    provenance_instructions[1].operand_count = 3;
    provenance_instructions[1].operands[0].type = OPERAND_TYPE_TEMP;
    provenance_instructions[1].operands[0].register_index = 0;
    provenance_instructions[1].operands[0].destination_mask = 16 | 32 | 64;
    provenance_instructions[2].opcode = USIL_OP_MOV;
    provenance_instructions[2].operand_count = 2;
    provenance_instructions[2].operands[0].type = OPERAND_TYPE_TEMP;
    provenance_instructions[2].operands[0].register_index = 1;
    provenance_instructions[2].operands[0].destination_mask = 16;
    provenance_instructions[2].operands[1].type = OPERAND_TYPE_TEMP;
    provenance_instructions[2].operands[1].register_index = 0;
    provenance_instructions[2].operands[1].swizzle_mode = 2;
    provenance_instructions[2].operands[1].swizzle[0] = 3;
    provenance_program.instructions = provenance_instructions;
    provenance_program.instruction_count = 3;
    provenance_program.temp_count = 2;
    provenance_context.program = &provenance_program;
    CHECK(build_control_flow_graph(&provenance_context));
    CHECK(build_component_provenance(&provenance_context));
    CHECK(component_value_unchanged(&provenance_context, 0, 3, 1, 2));
    CHECK(!component_value_unchanged(&provenance_context, 0, 0, 1, 2));
    const HLSLComponentProvenance *copied = get_component_provenance(
        &provenance_context, 3, 1, 0);
    CHECK(copied != NULL);
    CHECK(copied->kind == HLSL_PROVENANCE_COPY);
    CHECK(copied->root_instruction == 0);
    CHECK(copied->root_register == 0 && copied->root_component == 3);
    CHECK(build_hlsl_use_def_graph(&provenance_context));
    CHECK(hlsl_operand_definition(&provenance_context, 2, 1, 0) == 0);
    CHECK(hlsl_definition_use_count(&provenance_context, 0, 3) > 0);
    CHECK(analyze_lane_value_types(&provenance_context));
    CHECK(get_lane_value_facts(&provenance_context, 2, 0, 0) ==
          HLSL_VALUE_FLOAT);
    CHECK(get_lane_value_facts(&provenance_context, 2, 0, 3) ==
          HLSL_VALUE_FLOAT);
    CHECK((get_operand_value_facts(&provenance_context, 1, 1, 3) &
           HLSL_VALUE_FLOAT) != 0);
    free_lane_value_types(&provenance_context);
    free_hlsl_use_def_graph(&provenance_context);
    free_component_provenance(&provenance_context);
    free_control_flow_graph(&provenance_context);

    static USILInstruction model_instructions[2];
    static USILProgram model_program;
    static HLSLEmitterContext model_context;
    memset(model_instructions, 0, sizeof(model_instructions));
    memset(&model_program, 0, sizeof(model_program));
    memset(&model_context, 0, sizeof(model_context));
    model_instructions[0].opcode = USIL_OP_MOV;
    model_instructions[0].operand_count = 2;
    model_instructions[0].operands[0].type = OPERAND_TYPE_TEMP;
    model_instructions[0].operands[0].register_index = 1;
    model_instructions[0].operands[0].destination_mask = 16;
    model_instructions[1].opcode = USIL_OP_ADD;
    model_instructions[1].operand_count = 3;
    model_instructions[1].operands[0].type = OPERAND_TYPE_TEMP;
    model_instructions[1].operands[0].register_index = 2;
    model_instructions[1].operands[0].destination_mask = 16;
    model_instructions[1].operands[1].type = OPERAND_TYPE_TEMP;
    model_instructions[1].operands[1].register_index = 1;
    model_instructions[1].operands[2].type = OPERAND_TYPE_TEMP;
    model_instructions[1].operands[2].register_index = 3;
    model_program.instructions = model_instructions;
    model_program.instruction_count = 2;
    model_context.program = &model_program;
    CHECK(analyze_d3dcompiler_model(&model_context));
    CHECK(!compiler_model_swaps_binary_operands(&model_context, 0));
    CHECK(compiler_model_swaps_binary_operands(&model_context, 1));
    free_d3dcompiler_model(&model_context);

    static USILInstruction truthiness_instructions[3];
    static USILProgram truthiness_program;
    static HLSLEmitterContext truthiness_context;
    memset(truthiness_instructions, 0, sizeof(truthiness_instructions));
    memset(&truthiness_program, 0, sizeof(truthiness_program));
    memset(&truthiness_context, 0, sizeof(truthiness_context));
    truthiness_instructions[0].opcode = USIL_OP_NE;
    truthiness_instructions[0].operand_count = 3;
    truthiness_instructions[0].operands[0].type = OPERAND_TYPE_TEMP;
    truthiness_instructions[0].operands[0].register_index = 0;
    truthiness_instructions[0].operands[0].destination_mask = 16 | 32;
    truthiness_instructions[0].operands[1].type = OPERAND_TYPE_IMMEDIATE32;
    truthiness_instructions[0].operands[1].imm_value_count = 4;
    truthiness_instructions[0].operands[2].type =
        OPERAND_TYPE_CONSTANT_BUFFER;
    truthiness_instructions[0].operands[2].swizzle_mode = 1;
    truthiness_instructions[0].operands[2].swizzle[0] = 0;
    truthiness_instructions[0].operands[2].swizzle[1] = 3;
    truthiness_instructions[0].operands[2].swizzle[2] = 0;
    truthiness_instructions[0].operands[2].swizzle[3] = 0;
    for (int instruction = 1; instruction <= 2; instruction++) {
        truthiness_instructions[instruction].opcode = USIL_OP_MOVC;
        truthiness_instructions[instruction].operand_count = 4;
        truthiness_instructions[instruction].operands[0].type =
            OPERAND_TYPE_TEMP;
        truthiness_instructions[instruction].operands[0].register_index = 1;
        truthiness_instructions[instruction].operands[0].destination_mask =
            16 | 32 | 64;
        truthiness_instructions[instruction].operands[1].type =
            OPERAND_TYPE_TEMP;
        truthiness_instructions[instruction].operands[1].register_index = 0;
        truthiness_instructions[instruction].operands[1].swizzle_mode = 2;
        truthiness_instructions[instruction].operands[1].swizzle[0] =
            (uint8_t)(instruction - 1);
        truthiness_instructions[instruction].operands[2].type =
            OPERAND_TYPE_TEMP;
        truthiness_instructions[instruction].operands[2].register_index = 2;
        truthiness_instructions[instruction].operands[3].type =
            OPERAND_TYPE_TEMP;
        truthiness_instructions[instruction].operands[3].register_index = 3;
    }
    snprintf(truthiness_program.shader_type_model,
             sizeof(truthiness_program.shader_type_model), "ps_4_0");
    truthiness_program.instructions = truthiness_instructions;
    truthiness_program.instruction_count = 3;
    truthiness_program.temp_count = 4;
    truthiness_context.program = &truthiness_program;
    StringBuilder truthiness_output;
    sb_init(&truthiness_output);
    truthiness_context.sb = &truthiness_output;
    CHECK(build_control_flow_graph(&truthiness_context));
    CHECK(build_component_provenance(&truthiness_context));
    CHECK(build_hlsl_use_def_graph(&truthiness_context));
    CHECK(analyze_semantic_lifts(&truthiness_context));
    char truthiness_alias[64];
    CHECK(semantic_truthiness_condition_alias(
        &truthiness_context, 1, 1, 0, truthiness_alias,
        sizeof(truthiness_alias)));
    CHECK(strcmp(truthiness_alias, "dxbc_truth_0_x") == 0);
    CHECK(semantic_truthiness_condition_alias(
        &truthiness_context, 2, 1, 0, truthiness_alias,
        sizeof(truthiness_alias)));
    CHECK(strcmp(truthiness_alias, "dxbc_truth_0_y") == 0);
    CHECK(!semantic_truthiness_condition_alias(
        &truthiness_context, 1, 2, 0, truthiness_alias,
        sizeof(truthiness_alias)));
    char tiny_truthiness_alias[4] = "bad";
    CHECK(!semantic_truthiness_condition_alias(
        &truthiness_context, 1, 1, 0, tiny_truthiness_alias,
        sizeof(tiny_truthiness_alias)));
    CHECK(truthiness_output.failed);
    CHECK(tiny_truthiness_alias[0] == '\0');
    free_semantic_lifts(&truthiness_context);
    free_hlsl_use_def_graph(&truthiness_context);
    free_component_provenance(&truthiness_context);
    free_control_flow_graph(&truthiness_context);
    sb_free(&truthiness_output);

    static USILInstruction conflict_instructions[5];
    static USILProgram conflict_program;
    static HLSLEmitterContext conflict_context;
    int modulo_divisors[5] = {2, 0, 0, 0, 0};
    int semantic_claims[5] = {-1, 0, -1, -1, -1};
    memset(conflict_instructions, 0, sizeof(conflict_instructions));
    memset(&conflict_program, 0, sizeof(conflict_program));
    memset(&conflict_context, 0, sizeof(conflict_context));
    conflict_program.instructions = conflict_instructions;
    conflict_program.instruction_count = 5;
    conflict_context.program = &conflict_program;
    conflict_context.modulo_divisor = modulo_divisors;
    conflict_context.semantic_program.claim_owner = semantic_claims;
    CHECK(!analyze_d3dcompiler_model(&conflict_context));
    CHECK(conflict_context.compiler_model.conflict_count == 1);
    free_d3dcompiler_model(&conflict_context);

    static USILInstruction storage_instructions[1];
    static USILProgram storage_program;
    static HLSLEmitterContext storage_context;
    memset(storage_instructions, 0, sizeof(storage_instructions));
    memset(&storage_program, 0, sizeof(storage_program));
    memset(&storage_context, 0, sizeof(storage_context));
    storage_instructions[0].opcode = USIL_OP_AND;
    storage_instructions[0].operand_count = 3;
    storage_instructions[0].operands[0].type = OPERAND_TYPE_TEMP;
    storage_instructions[0].operands[0].register_index = 0;
    storage_instructions[0].operands[1].type = OPERAND_TYPE_TEMP;
    storage_instructions[0].operands[1].register_index = 1;
    storage_instructions[0].operands[2].type = OPERAND_TYPE_IMMEDIATE32;
    storage_program.instructions = storage_instructions;
    storage_program.instruction_count = 1;
    storage_program.temp_count = 3;
    storage_context.program = &storage_program;
    CHECK(build_hlsl_storage_plan(&storage_context));
    CHECK(storage_context.storage_plan.default_storage ==
          HLSL_TEMP_STORAGE_RAW_UINT);
    CHECK(temp_register_requires_raw_storage(&storage_context, 0));
    CHECK(temp_register_requires_raw_storage(&storage_context, 1));
    CHECK(!temp_register_requires_raw_storage(&storage_context, 2));
    free_hlsl_storage_plan(&storage_context);

    static USILInstruction cfg_instructions[6];
    static USILProgram cfg_program;
    static HLSLEmitterContext cfg_context;
    memset(cfg_instructions, 0, sizeof(cfg_instructions));
    memset(&cfg_program, 0, sizeof(cfg_program));
    memset(&cfg_context, 0, sizeof(cfg_context));
    cfg_instructions[0].opcode = USIL_OP_IF;
    cfg_instructions[1].opcode = USIL_OP_NOP;
    cfg_instructions[2].opcode = USIL_OP_ELSE;
    cfg_instructions[3].opcode = USIL_OP_NOP;
    cfg_instructions[4].opcode = USIL_OP_ENDIF;
    cfg_instructions[5].opcode = USIL_OP_NOP;
    cfg_program.instructions = cfg_instructions;
    cfg_program.instruction_count = 6;
    cfg_context.program = &cfg_program;
    CHECK(build_control_flow_graph(&cfg_context));
    CHECK(!instructions_have_unambiguous_path(&cfg_context, 1, 5));
    CHECK(instructions_have_unambiguous_path(&cfg_context, 3, 4));
    free_control_flow_graph(&cfg_context);

    static USILInstruction loop_instructions[5];
    memset(loop_instructions, 0, sizeof(loop_instructions));
    memset(&cfg_program, 0, sizeof(cfg_program));
    memset(&cfg_context, 0, sizeof(cfg_context));
    loop_instructions[0].opcode = USIL_OP_LOOP;
    loop_instructions[1].opcode = USIL_OP_NOP;
    loop_instructions[2].opcode = USIL_OP_BREAK;
    loop_instructions[3].opcode = USIL_OP_ENDLOOP;
    loop_instructions[4].opcode = USIL_OP_NOP;
    cfg_program.instructions = loop_instructions;
    cfg_program.instruction_count = 5;
    cfg_context.program = &cfg_program;
    CHECK(build_control_flow_graph(&cfg_context));
    CHECK(cfg_context.cfg.blocks[
              cfg_context.cfg.instruction_block[2]].successor_count == 1);
    CHECK(cfg_context.cfg.blocks[
              cfg_context.cfg.instruction_block[2]].successors[0] ==
          cfg_context.cfg.instruction_block[4]);
    free_control_flow_graph(&cfg_context);

    static USILInstruction switch_instructions[3];
    memset(switch_instructions, 0, sizeof(switch_instructions));
    memset(&cfg_program, 0, sizeof(cfg_program));
    memset(&cfg_context, 0, sizeof(cfg_context));
    switch_instructions[0].opcode = USIL_OP_SWITCH;
    switch_instructions[1].opcode = USIL_OP_DEFAULT;
    switch_instructions[2].opcode = USIL_OP_ENDSWITCH;
    cfg_program.instructions = switch_instructions;
    cfg_program.instruction_count = 3;
    cfg_context.program = &cfg_program;
    CHECK(build_control_flow_graph(&cfg_context));
    CHECK(!instructions_have_unambiguous_path(&cfg_context, 0, 2));
    free_control_flow_graph(&cfg_context);

    // Dominance & Dominance Frontier Unit Test (Diamond CFG)
    static USILInstruction dom_instructions[6];
    memset(dom_instructions, 0, sizeof(dom_instructions));
    memset(&cfg_program, 0, sizeof(cfg_program));
    memset(&cfg_context, 0, sizeof(cfg_context));

    dom_instructions[0].opcode = USIL_OP_IF;
    dom_instructions[0].operand_count = 1;
    dom_instructions[0].operands[0].type = OPERAND_TYPE_TEMP;
    dom_instructions[0].operands[0].register_index = 0;

    dom_instructions[1].opcode = USIL_OP_MOV; // Block 1
    dom_instructions[2].opcode = USIL_OP_ELSE; // Block 2
    dom_instructions[3].opcode = USIL_OP_MOV; // Block 3
    dom_instructions[4].opcode = USIL_OP_ENDIF; // Block 4
    dom_instructions[5].opcode = USIL_OP_MOV; // Block 5

    cfg_program.instructions = dom_instructions;
    cfg_program.instruction_count = 6;
    cfg_program.temp_count = 1;
    cfg_context.program = &cfg_program;

    CHECK(build_control_flow_graph(&cfg_context));
    CHECK(compute_dominance(&cfg_context.cfg));

    // Assert block count
    CHECK(cfg_context.cfg.block_count == 6);

    // Assert immediate dominators (idom)
    CHECK(cfg_context.cfg.idom[0] == 0);
    CHECK(cfg_context.cfg.idom[1] == 0);
    CHECK(cfg_context.cfg.idom[2] == 1);
    CHECK(cfg_context.cfg.idom[3] == 0);
    CHECK(cfg_context.cfg.idom[4] == 3);
    CHECK(cfg_context.cfg.idom[5] == 0);

    // Assert Dominance Frontiers (df)
    CHECK(cfg_context.cfg.df_count[0] == 0);
    CHECK(cfg_context.cfg.df_count[1] == 1 && cfg_context.cfg.df[1][0] == 5);
    CHECK(cfg_context.cfg.df_count[2] == 1 && cfg_context.cfg.df[2][0] == 5);
    CHECK(cfg_context.cfg.df_count[3] == 1 && cfg_context.cfg.df[3][0] == 5);
    CHECK(cfg_context.cfg.df_count[4] == 1 && cfg_context.cfg.df[4][0] == 5);
    CHECK(cfg_context.cfg.df_count[5] == 0);

    // Verify SSA construction
    CHECK(build_hlsl_ssa_graph(&cfg_context));
    CHECK(cfg_context.ssa.ssa_var_count >= 0);

    free_hlsl_ssa_graph(&cfg_context);
    free_control_flow_graph(&cfg_context);

    // AST Formatting Unit Test
    ASTExpr *var1 = ast_create_var(1, 0, OPERAND_TYPE_TEMP, NULL); // r0_1
    ASTExpr *var2 = ast_create_var(2, 1, OPERAND_TYPE_TEMP, NULL); // r1_2
    ASTExpr *add_expr = ast_create_binary(USIL_OP_ADD, var1, var2);
    int swiz[4] = {0, 1, 2, 3};
    ASTExpr *swiz_expr = ast_create_swizzle(add_expr, swiz, 4);

    StringBuilder sb;
    sb_init(&sb);
    ast_format_expr(swiz_expr, &sb);
    CHECK(strcmp(sb.buf, "(r0_1 + r1_2).xyzw") == 0);
    sb_free(&sb);

    ASTExpr *dest = ast_create_var(3, 0, OPERAND_TYPE_TEMP, NULL); // r0_3
    ASTStmt *assign = ast_create_assign(dest, swiz_expr);

    sb_init(&sb);
    ast_format_stmt(assign, &sb, 1);
    CHECK(strcmp(sb.buf, "  r0_3 = (r0_1 + r1_2).xyzw;\n") == 0);
    sb_free(&sb);

    ast_free_stmt(assign);

    char long_ast_name[513];
    long_ast_name[0] = '_';
    for (size_t index = 1; index + 1u < sizeof(long_ast_name); ++index)
        long_ast_name[index] = (char)('a' + index % 26u);
    long_ast_name[sizeof(long_ast_name) - 1u] = '\0';

    ASTExpr *long_var = ast_create_var(0, 0, OPERAND_TYPE_TEMP,
                                       long_ast_name);
    CHECK(long_var != NULL);
    sb_init(&sb);
    ast_format_expr(long_var, &sb);
    CHECK(sb_ok(&sb));
    CHECK(strcmp(sb.buf, long_ast_name) == 0);
    sb_free(&sb);
    ast_free_expr(long_var);

    ASTExpr *long_call = ast_create_call(long_ast_name, NULL, 0);
    CHECK(long_call != NULL);
    sb_init(&sb);
    ast_format_expr(long_call, &sb);
    CHECK(sb_ok(&sb));
    CHECK(sb.len == strlen(long_ast_name) + 2u);
    CHECK(strncmp(sb.buf, long_ast_name, strlen(long_ast_name)) == 0);
    CHECK(strcmp(sb.buf + strlen(long_ast_name), "()") == 0);
    sb_free(&sb);
    ast_free_expr(long_call);

    ASTExpr *cast_value = ast_create_literal_int(1);
    CHECK(cast_value != NULL);
    ASTExpr *long_cast = ast_create_cast(long_ast_name, cast_value);
    CHECK(long_cast != NULL);
    sb_init(&sb);
    ast_format_expr(long_cast, &sb);
    CHECK(sb_ok(&sb));
    CHECK(sb.buf[0] == '(');
    CHECK(strncmp(sb.buf + 1, long_ast_name, strlen(long_ast_name)) == 0);
    CHECK(strcmp(sb.buf + 1 + strlen(long_ast_name), ")1") == 0);
    sb_free(&sb);
    ast_free_expr(long_cast);

    ASTExpr *wide_float = ast_create_literal_float(FLT_MAX);
    CHECK(wide_float != NULL);
    sb_init(&sb);
    ast_format_expr(wide_float, &sb);
    CHECK(sb_ok(&sb));
    CHECK(sb.len > 40u);
    sb_free(&sb);
    ast_free_expr(wide_float);

    serialized_program_parameters_free(&params);
    return 0;
}
