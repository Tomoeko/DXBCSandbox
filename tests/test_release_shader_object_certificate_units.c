#include "app/release_shader_object_certificate.h"
#include "io/serialized_file.h"
#include "io/typetree_schema_registry.h"
#include "io/unity_input.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                    __LINE__, #condition);                                   \
            return false;                                                    \
        }                                                                    \
    } while (0)

static bool stable_fixture_reference(
    void* user_data, ReleaseShaderObjectSide side, int32_t file_id,
    int64_t path_id, uint8_t stable_id[RELEASE_SHADER_REFERENCE_ID_SIZE]) {
    (void)user_data;
    (void)side;
    if (!stable_id) return false;
    memset(stable_id, 0, RELEASE_SHADER_REFERENCE_ID_SIZE);
    for (size_t index = 0U; index < sizeof(file_id); ++index) {
        stable_id[index] =
            (uint8_t)(((uint32_t)file_id >> (index * 8U)) & 0xffU);
    }
    const uint64_t raw_path = (uint64_t)path_id;
    for (size_t index = 0U; index < sizeof(raw_path); ++index) {
        stable_id[8U + index] =
            (uint8_t)((raw_path >> (index * 8U)) & 0xffU);
    }
    return true;
}

static bool test_archive_comparator(void) {
    uint8_t expected_bytes[] = {1U, 2U, 3U, 4U, 5U, 6U};
    uint8_t actual_bytes[] = {1U, 2U, 3U, 4U, 5U, 6U};
    uint8_t* expected_segments[] = {expected_bytes};
    uint8_t* actual_segments[] = {actual_bytes};
    int expected_lengths[] = {(int)sizeof(expected_bytes)};
    int actual_lengths[] = {(int)sizeof(actual_bytes)};
    BlobEntry expected_entries[] = {{0, 3, 0}, {3, 3, 0}};
    BlobEntry actual_entries[] = {{0, 3, 0}, {3, 3, 0}};
    ShaderBlobArchive expected = {
        .entries = expected_entries,
        .entry_count = 2,
        .segments = expected_segments,
        .segment_lengths = expected_lengths,
        .segment_count = 1,
        .stage_count = 7U,
    };
    ShaderBlobArchive actual = {
        .entries = actual_entries,
        .entry_count = 2,
        .segments = actual_segments,
        .segment_lengths = actual_lengths,
        .segment_count = 1,
        .stage_count = 7U,
    };

    ReleaseShaderArchiveCompareReport report;
    CHECK(release_shader_archive_compare_canonical(
              &expected, &actual, &report) == RELEASE_SHADER_ARCHIVE_EQUAL);
    CHECK(report.matched_entry_count == 2U);
    CHECK(report.first_differing_entry == -1);
    CHECK(report.first_differing_byte == SIZE_MAX);

    actual_bytes[4] ^= 0x40U;
    CHECK(release_shader_archive_compare_canonical(
              &expected, &actual, &report) ==
          RELEASE_SHADER_ARCHIVE_ENTRY_PAYLOAD_MISMATCH);
    CHECK(report.first_differing_entry == 1);
    CHECK(report.first_differing_byte == 1U);
    CHECK(report.matched_entry_count == 1U);
    actual_bytes[4] ^= 0x40U;

    actual.stage_count = 8U;
    CHECK(release_shader_archive_compare_canonical(
              &expected, &actual, &report) ==
          RELEASE_SHADER_ARCHIVE_STAGE_COUNT_MISMATCH);
    actual.stage_count = 7U;

    actual_entries[1].length = 2;
    CHECK(release_shader_archive_compare_canonical(
              &expected, &actual, &report) ==
          RELEASE_SHADER_ARCHIVE_ENTRY_SIZE_MISMATCH);
    actual_entries[1].length = 3;

    actual.entry_count = 1;
    CHECK(release_shader_archive_compare_canonical(
              &expected, &actual, &report) ==
          RELEASE_SHADER_ARCHIVE_ENTRY_COUNT_MISMATCH);
    actual.entry_count = 2;

    actual_entries[1].offset = 7;
    CHECK(release_shader_archive_compare_canonical(
              &expected, &actual, &report) ==
          RELEASE_SHADER_ARCHIVE_ACTUAL_INVALID);
    actual_entries[1].offset = 3;
    return true;
}

static TypeTreeValue* mutable_child(TypeTreeValue* parent,
                                    const char* name) {
    if (!parent || parent->type != VAL_TYPE_STRUCT || !parent->struct_val.members)
        return NULL;
    for (int index = 0; index < parent->struct_val.count; ++index) {
        TypeTreeValue* member = &parent->struct_val.members[index];
        if (member->name && strcmp(member->name, name) == 0) return member;
    }
    return NULL;
}

static TypeTreeValue* mutable_array(TypeTreeValue* parent,
                                    const char* name) {
    TypeTreeValue* field = mutable_child(parent, name);
    if (!field) return NULL;
    if (field->type == VAL_TYPE_ARRAY) return field;
    if (field->type != VAL_TYPE_STRUCT) return NULL;
    return mutable_child(field, "Array");
}

static TypeTreeValue* first_property_name(ShaderObject* object) {
    TypeTreeValue* parsed = mutable_child(&object->root, "m_ParsedForm");
    TypeTreeValue* info = mutable_child(parsed, "m_PropInfo");
    TypeTreeValue* properties = mutable_array(info, "m_Props");
    if (!properties || properties->array_val.count <= 0 ||
        properties->array_val.storage != TYPETREE_ARRAY_VALUES) {
        return NULL;
    }
    return mutable_child(&properties->array_val.elements[0], "m_Name");
}

static TypeTreeValue* first_subshader_tag_map(ShaderObject* object,
                                              int minimum_count) {
    TypeTreeValue* parsed = mutable_child(&object->root, "m_ParsedForm");
    TypeTreeValue* subshaders = mutable_array(parsed, "m_SubShaders");
    if (!subshaders ||
        subshaders->array_val.storage != TYPETREE_ARRAY_VALUES) {
        return NULL;
    }
    for (int index = 0; index < subshaders->array_val.count; ++index) {
        TypeTreeValue* tags = mutable_child(
            &subshaders->array_val.elements[index], "m_Tags");
        TypeTreeValue* map = mutable_array(tags, "tags");
        if (!map && tags && tags->type == VAL_TYPE_ARRAY) map = tags;
        if (map && map->array_val.storage == TYPETREE_ARRAY_VALUES &&
            map->array_val.count >= minimum_count) {
            return map;
        }
    }
    return NULL;
}

static TypeTreeValue* first_pass(ShaderObject* object) {
    TypeTreeValue* parsed = mutable_child(&object->root, "m_ParsedForm");
    TypeTreeValue* subshaders = mutable_array(parsed, "m_SubShaders");
    if (!subshaders ||
        subshaders->array_val.storage != TYPETREE_ARRAY_VALUES) {
        return NULL;
    }
    for (int subshader = 0; subshader < subshaders->array_val.count;
         ++subshader) {
        TypeTreeValue* passes = mutable_array(
            &subshaders->array_val.elements[subshader], "m_Passes");
        if (passes && passes->array_val.storage == TYPETREE_ARRAY_VALUES &&
            passes->array_val.count > 0) {
            return &passes->array_val.elements[0];
        }
    }
    return NULL;
}

static TypeTreeValue* first_player_blob_index(ShaderObject* object) {
    static const char* const programs[] = {
        "progVertex", "progFragment", "progGeometry",
        "progHull", "progDomain", "progRayTracing",
    };
    TypeTreeValue* pass = first_pass(object);
    if (!pass) return NULL;
    for (size_t stage = 0U; stage < sizeof(programs) / sizeof(programs[0]);
         ++stage) {
        TypeTreeValue* program = mutable_child(pass, programs[stage]);
        TypeTreeValue* groups =
            mutable_array(program, "m_PlayerSubPrograms");
        if (!groups || groups->array_val.storage != TYPETREE_ARRAY_VALUES) {
            continue;
        }
        for (int group = 0; group < groups->array_val.count; ++group) {
            TypeTreeValue* programs_in_group =
                &groups->array_val.elements[group];
            if (programs_in_group->type == VAL_TYPE_STRUCT) {
                programs_in_group = mutable_child(programs_in_group,
                                                   "Array");
            }
            if (programs_in_group &&
                programs_in_group->type == VAL_TYPE_ARRAY &&
                programs_in_group->array_val.storage ==
                    TYPETREE_ARRAY_VALUES &&
                programs_in_group->array_val.count > 0) {
                return mutable_child(
                    &programs_in_group->array_val.elements[0],
                    "m_BlobIndex");
            }
        }
    }
    return NULL;
}

static TypeTreeValue* first_parameter_name_index(ShaderObject* object) {
    static const char* const programs[] = {
        "progVertex", "progFragment", "progGeometry",
        "progHull", "progDomain", "progRayTracing",
    };
    TypeTreeValue* pass = first_pass(object);
    if (!pass) return NULL;
    for (size_t stage = 0U; stage < sizeof(programs) / sizeof(programs[0]);
         ++stage) {
        TypeTreeValue* program = mutable_child(pass, programs[stage]);
        TypeTreeValue* parameters =
            mutable_child(program, "m_CommonParameters");
        static const char* const arrays[] = {
            "m_VectorParams", "m_MatrixParams", "m_TextureParams",
            "m_BufferParams", "m_ConstantBufferBindings", "m_UAVParams",
        };
        for (size_t kind = 0U; kind < sizeof(arrays) / sizeof(arrays[0]);
             ++kind) {
            TypeTreeValue* values = mutable_array(parameters, arrays[kind]);
            if (values && values->array_val.storage == TYPETREE_ARRAY_VALUES &&
                values->array_val.count > 0) {
                return mutable_child(&values->array_val.elements[0],
                                     "m_NameIndex");
            }
        }
    }
    return NULL;
}

static bool set_integer_delta(TypeTreeValue* value, int64_t delta,
                              uint64_t* saved_unsigned,
                              int64_t* saved_signed) {
    if (!value || value->type != VAL_TYPE_INT || !saved_unsigned ||
        !saved_signed) {
        return false;
    }
    *saved_unsigned = value->uint_val;
    *saved_signed = value->int_val;
    if (value->integer_is_unsigned) {
        value->uint_val += (uint64_t)delta;
    } else {
        value->int_val += delta;
    }
    return true;
}

static void restore_integer(TypeTreeValue* value, uint64_t saved_unsigned,
                            int64_t saved_signed) {
    if (!value || value->type != VAL_TYPE_INT) return;
    if (value->integer_is_unsigned) value->uint_val = saved_unsigned;
    else value->int_val = saved_signed;
}

static TypeTreeValue synthetic_integer(const char* name, const char* type,
                                       int64_t value) {
    TypeTreeValue result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.type_str = type;
    result.type = VAL_TYPE_INT;
    result.int_val = value;
    return result;
}

static TypeTreeValue synthetic_string(const char* name, const char* value) {
    TypeTreeValue result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.type_str = "string";
    result.type = VAL_TYPE_STRING;
    result.string_val = (char*)value;
    result.string_length = strlen(value);
    return result;
}

static TypeTreeValue synthetic_struct(const char* name, const char* type,
                                      TypeTreeValue* members, int count) {
    TypeTreeValue result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.type_str = type;
    result.type = VAL_TYPE_STRUCT;
    result.struct_val.members = members;
    result.struct_val.count = count;
    return result;
}

typedef struct {
    TypeTreeSchemaRegistry registry;
    bool tested;
    int failures;
} ObjectTestContext;

static bool check_object_report(const ShaderObject* expected,
                                const ShaderObject* actual,
                                ReleaseShaderObjectCertificateStatus status,
                                ReleaseShaderObjectCertificateReport* report) {
    ReleaseShaderObjectCertificateOptions options;
    release_shader_object_certificate_options_init(&options);
    options.resolve_reference = stable_fixture_reference;
    return release_shader_object_certify_equal(
               expected, actual, &options, report) == status;
}

static bool exercise_object_pair(ShaderObject* expected,
                                 ShaderObject* actual) {
    ReleaseShaderObjectCertificateReport report;
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OK,
                              &report));
    CHECK(report.canonical_release_identity_certified);
    CHECK(report.compiled_artifacts_exact);
    CHECK(!report.source_identity_certified);
    CHECK(!report.visual_output_certified);
    CHECK(report.evaluated_field_count == RELEASE_SHADER_FIELD_COUNT);
    CHECK(report.matched_field_count == RELEASE_SHADER_FIELD_COUNT);
    CHECK(report.expected_artifact_count > 0U);
    CHECK(report.expected_artifact_count == report.actual_artifact_count);
    CHECK(report.expected_artifact_count == report.matched_artifact_count);

    uint8_t expected_state[COMMON_SHA256_DIGEST_SIZE], actual_state[COMMON_SHA256_DIGEST_SIZE];
    CHECK(release_shader_render_state_digest(expected, expected_state));
    CHECK(release_shader_render_state_digest(actual, actual_state));
    CHECK(memcmp(expected_state, actual_state, sizeof(actual_state)) == 0);
    TypeTreeValue *state = mutable_child(first_pass(actual), "m_State");
    TypeTreeValue *depth_test = mutable_child(mutable_child(state, "zTest"), "val");
    CHECK(depth_test && depth_test->type == VAL_TYPE_FLOAT);
    const double saved_depth_test = depth_test->float_val;
    depth_test->float_val = saved_depth_test == 1.0 ? 2.0 : 1.0;
    CHECK(release_shader_render_state_digest(actual, actual_state));
    CHECK(memcmp(expected_state, actual_state, sizeof(actual_state)) != 0);
    CHECK(check_object_report(expected, actual, RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER, &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_RENDER_STATE] == RELEASE_SHADER_FIELD_MISMATCH);
    depth_test->float_val = saved_depth_test;
    CHECK(release_shader_render_state_digest(actual, actual_state));
    CHECK(memcmp(expected_state, actual_state, sizeof(actual_state)) == 0);

    TypeTreeValue* property_name = first_property_name(actual);
    CHECK(property_name && property_name->type == VAL_TYPE_STRING);
    char* saved_name = property_name->string_val;
    const size_t saved_length = property_name->string_length;
    property_name->string_val = "__release_certificate_changed__";
    property_name->string_length = strlen(property_name->string_val);
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER,
                              &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_PROPERTIES] ==
          RELEASE_SHADER_FIELD_MISMATCH);
    CHECK(report.fields[RELEASE_SHADER_FIELD_PARSED_FORM_RESIDUAL] ==
          RELEASE_SHADER_FIELD_MISMATCH);
    CHECK(report.compiled_artifacts_exact);
    CHECK(!report.canonical_release_identity_certified);
    property_name->string_val = saved_name;
    property_name->string_length = saved_length;

    TypeTreeValue* tags = first_subshader_tag_map(actual, 2);
    CHECK(tags != NULL);
    TypeTreeValue temporary = tags->array_val.elements[0];
    tags->array_val.elements[0] = tags->array_val.elements[1];
    tags->array_val.elements[1] = temporary;
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OK,
                              &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_SUBSHADER_METADATA] ==
          RELEASE_SHADER_FIELD_MATCH);
    tags->array_val.elements[1] = tags->array_val.elements[0];
    tags->array_val.elements[0] = temporary;

    TypeTreeValue* baked = mutable_child(&actual->root, "m_ShaderIsBaked");
    uint64_t saved_unsigned = 0U;
    int64_t saved_signed = 0;
    CHECK(set_integer_delta(baked, baked->integer_is_unsigned
                                       ? (baked->uint_val == 0U ? 1 : -1)
                                       : (baked->int_val == 0 ? 1 : -1),
                            &saved_unsigned, &saved_signed));
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER,
                              &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_SHADER_IS_BAKED] ==
          RELEASE_SHADER_FIELD_MISMATCH);
    restore_integer(baked, saved_unsigned, saved_signed);

    TypeTreeValue* blob_index = first_player_blob_index(actual);
    CHECK(set_integer_delta(blob_index, 1, &saved_unsigned, &saved_signed));
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER,
                              &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_PROGRAM_VARIANTS] ==
          RELEASE_SHADER_FIELD_MISMATCH);
    CHECK(report.compiled_artifacts_exact);
    restore_integer(blob_index, saved_unsigned, saved_signed);

    TypeTreeValue* parameter = first_parameter_name_index(actual);
    CHECK(set_integer_delta(parameter, 1, &saved_unsigned, &saved_signed));
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER,
                              &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_PARAMETER_BINDINGS] ==
          RELEASE_SHADER_FIELD_MISMATCH);
    CHECK(report.compiled_artifacts_exact);
    restore_integer(parameter, saved_unsigned, saved_signed);

    TypeTreeValue* expected_dependencies =
        mutable_array(&expected->root, "m_Dependencies");
    TypeTreeValue* actual_dependencies =
        mutable_array(&actual->root, "m_Dependencies");
    CHECK(expected_dependencies && actual_dependencies);
    const TypeTreeValue saved_expected_dependencies = *expected_dependencies;
    const TypeTreeValue saved_actual_dependencies = *actual_dependencies;
    TypeTreeValue expected_pointer_members[] = {
        synthetic_integer("m_FileID", "int", 0),
        synthetic_integer("m_PathID", "SInt64", 41),
    };
    TypeTreeValue actual_pointer_members[] = {
        synthetic_integer("m_FileID", "int", 0),
        synthetic_integer("m_PathID", "SInt64", 41),
    };
    TypeTreeValue expected_pointer = synthetic_struct(
        "data", "PPtr<Shader>", expected_pointer_members, 2);
    TypeTreeValue actual_pointer = synthetic_struct(
        "data", "PPtr<Shader>", actual_pointer_members, 2);
    expected_dependencies->array_val.elements = &expected_pointer;
    expected_dependencies->array_val.count = 1;
    expected_dependencies->array_val.storage = TYPETREE_ARRAY_VALUES;
    actual_dependencies->array_val.elements = &actual_pointer;
    actual_dependencies->array_val.count = 1;
    actual_dependencies->array_val.storage = TYPETREE_ARRAY_VALUES;

    CHECK(release_shader_object_certify_equal(
              expected, actual, NULL, &report) ==
          RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_UNAVAILABLE);
    CHECK(report.fields[RELEASE_SHADER_FIELD_OBJECT_DEPENDENCY_PTRS] ==
          RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE);
    CHECK(!report.canonical_release_identity_certified);
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OK,
                              &report));
    actual_pointer_members[1].int_val = 42;
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER,
                              &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_OBJECT_DEPENDENCY_PTRS] ==
          RELEASE_SHADER_FIELD_MISMATCH);
    *expected_dependencies = saved_expected_dependencies;
    *actual_dependencies = saved_actual_dependencies;

    TypeTreeValue* expected_textures =
        mutable_array(&expected->root, "m_NonModifiableTextures");
    TypeTreeValue* actual_textures =
        mutable_array(&actual->root, "m_NonModifiableTextures");
    CHECK(expected_textures && actual_textures);
    const TypeTreeValue saved_expected_textures = *expected_textures;
    const TypeTreeValue saved_actual_textures = *actual_textures;
    TypeTreeValue null_texture_members_a[] = {
        synthetic_integer("m_FileID", "int", 0),
        synthetic_integer("m_PathID", "SInt64", 0),
    };
    TypeTreeValue null_texture_members_b[] = {
        synthetic_integer("m_FileID", "int", 0),
        synthetic_integer("m_PathID", "SInt64", 0),
    };
    TypeTreeValue null_textures[] = {
        synthetic_struct("second", "PPtr<Texture>",
                         null_texture_members_a, 2),
        synthetic_struct("second", "PPtr<Texture>",
                         null_texture_members_b, 2),
    };
    TypeTreeValue expected_pair_a_members[] = {
        synthetic_string("first", "TextureA"), null_textures[0],
    };
    TypeTreeValue expected_pair_b_members[] = {
        synthetic_string("first", "TextureB"), null_textures[1],
    };
    TypeTreeValue actual_pair_a_members[] = {
        synthetic_string("first", "TextureA"), null_textures[0],
    };
    TypeTreeValue actual_pair_b_members[] = {
        synthetic_string("first", "TextureB"), null_textures[1],
    };
    TypeTreeValue expected_pairs[] = {
        synthetic_struct("data", "pair", expected_pair_a_members, 2),
        synthetic_struct("data", "pair", expected_pair_b_members, 2),
    };
    TypeTreeValue actual_pairs[] = {
        synthetic_struct("data", "pair", actual_pair_b_members, 2),
        synthetic_struct("data", "pair", actual_pair_a_members, 2),
    };
    expected_textures->array_val.elements = expected_pairs;
    expected_textures->array_val.count = 2;
    expected_textures->array_val.storage = TYPETREE_ARRAY_VALUES;
    actual_textures->array_val.elements = actual_pairs;
    actual_textures->array_val.count = 2;
    actual_textures->array_val.storage = TYPETREE_ARRAY_VALUES;
    CHECK(check_object_report(expected, actual,
                              RELEASE_SHADER_OBJECT_CERTIFICATE_OK,
                              &report));
    CHECK(report.fields[RELEASE_SHADER_FIELD_NONMODIFIABLE_TEXTURES] ==
          RELEASE_SHADER_FIELD_MATCH);
    *expected_textures = saved_expected_textures;
    *actual_textures = saved_actual_textures;
    return true;
}

static bool inspect_source(const UnitySerializedSource* source,
                           void* opaque) {
    ObjectTestContext* context = (ObjectTestContext*)opaque;
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        ++context->failures;
        return true;
    }
    if (serialized_file_resolve_class_schema(
            &file, 48, &context->registry) != TYPETREE_SCHEMA_OK) {
        serialized_file_close(&file);
        return true;
    }
    for (int index = 0; index < file.object_count && !context->tested;
         ++index) {
        const AssetObjectInfo* asset = &file.objects[index];
        if (asset->type_id != 48) continue;
        ShaderObject expected;
        ShaderObject actual;
        shader_object_init(&expected);
        shader_object_init(&actual);
        if (shader_object_decode_borrowed(&expected, &file, asset) !=
                SHADER_OBJECT_OK ||
            shader_object_decode_borrowed(&actual, &file, asset) !=
                SHADER_OBJECT_OK) {
            shader_object_dispose(&expected);
            shader_object_dispose(&actual);
            continue;
        }
        const bool suitable = expected.shader.property_count > 0 &&
            first_subshader_tag_map(&actual, 2) != NULL &&
            first_player_blob_index(&actual) != NULL &&
            first_parameter_name_index(&actual) != NULL;
        if (suitable) {
            if (!exercise_object_pair(&expected, &actual)) {
                ++context->failures;
            }
            context->tested = true;
        }
        shader_object_dispose(&expected);
        shader_object_dispose(&actual);
    }
    serialized_file_close(&file);
    return true;
}

static bool test_real_release_object(void) {
    ObjectTestContext context;
    memset(&context, 0, sizeof(context));
    typetree_schema_registry_init(&context.registry);
    CHECK(typetree_schema_registry_import_file_replace(
              &context.registry, DXBC_TEST_PLAYER_SCHEMA_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    UnityInputVisitStats stats;
    CHECK(unity_input_visit_serialized(
              DXBC_TEST_SHADER_BUNDLE, inspect_source, &context, &stats) ==
          UNITY_INPUT_OK);
    typetree_schema_registry_dispose(&context.registry);
    CHECK(context.failures == 0);
    CHECK(context.tested);
    return true;
}

int main(void) {
    if (!test_archive_comparator() || !test_real_release_object()) return 1;
    if (g_allocations_count != 0U || g_allocated_bytes != 0U) {
        fprintf(stderr, "allocator leak: %zu allocations, %zu bytes\n",
                g_allocations_count, g_allocated_bytes);
        return 1;
    }
    printf("Release Shader-object certificate unit tests passed.\n");
    return 0;
}
