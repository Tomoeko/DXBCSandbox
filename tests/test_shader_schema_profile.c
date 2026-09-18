#include "common/file_io.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"
#include "io/serialized_shader_profile.h"
#include "io/typetree_schema_profile.h"
#include "io/typetree_schema_registry.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifndef SHADER_SCHEMA_PROFILE_REGISTRY
#error SHADER_SCHEMA_PROFILE_REGISTRY must name the pinned registry
#endif
#ifndef SHADER_SCHEMA_PROFILE_BUNDLE
#error SHADER_SCHEMA_PROFILE_BUNDLE must name the pinned shader bundle
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

typedef struct {
    int32_t class_id;
    uint8_t type_hash[16];
} KnownSchema;

static const KnownSchema k_known_schemas[] = {
    {21, {0xc6, 0x00, 0x98, 0xac, 0x66, 0xa2, 0x8b, 0x50,
          0xaa, 0x05, 0x80, 0xdb, 0x11, 0xbf, 0x01, 0x8c}},
    {48, {0xf0, 0xc1, 0x84, 0x27, 0x2a, 0x05, 0xe5, 0x44,
          0x47, 0xe3, 0x63, 0x66, 0xec, 0x48, 0x76, 0xa6}},
    {142, {0x97, 0xda, 0x5f, 0x46, 0x88, 0xe4, 0x5a, 0x57,
           0xc8, 0xb4, 0x2d, 0x4f, 0x42, 0x49, 0x72, 0x97}},
};

static TypeTreeSchemaKey known_key(const KnownSchema* known) {
    TypeTreeSchemaKey key;
    memset(&key, 0, sizeof(key));
    key.serialized_file_version = 22U;
    key.unity_version = "2021.3.35f1";
    key.unity_version_size = strlen(key.unity_version);
    key.class_id = known->class_id;
    key.serialized_type_id = known->class_id;
    key.script_type_index = UINT16_MAX;
    memcpy(key.type_hash, known->type_hash, sizeof(key.type_hash));
    return key;
}

static int find_last_root_child(const TypeTreeType* schema) {
    int result = -1;
    for (int index = 1; index < schema->node_count; ++index) {
        if (schema->nodes[index].level == 1U) result = index;
    }
    return result;
}

static bool mutate_known_schema(const KnownSchema* known,
                                const TypeTreeSchemaRegistry* registry) {
    TypeTreeSchemaKey key = known_key(known);
    TypeTreeType schema;
    CHECK(typetree_schema_registry_lookup(registry, &key, &schema) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_validate_known_profile(
              key.unity_version, key.unity_version_size, key.class_id,
              key.type_hash, &schema) == TYPETREE_SCHEMA_PROFILE_VALID);

    uint8_t first_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t second_digest[COMMON_SHA256_DIGEST_SIZE];
    CHECK(typetree_schema_shape_digest(&schema, first_digest));
    CHECK(typetree_schema_shape_digest(&schema, second_digest));
    CHECK(memcmp(first_digest, second_digest, sizeof(first_digest)) == 0);

    int original_node_count = schema.node_count;
    int last_root_child = find_last_root_child(&schema);
    CHECK(last_root_child > 0);
    schema.node_count = last_root_child;
    CHECK(typetree_validate_schema(&schema));
    CHECK(typetree_schema_validate_known_profile(
              key.unity_version, key.unity_version_size, key.class_id,
              key.type_hash, &schema) == TYPETREE_SCHEMA_PROFILE_INVALID);
    TypeTreeSchemaRegistry rejected;
    typetree_schema_registry_init(&rejected);
    CHECK(typetree_schema_registry_learn(
              &rejected, &key, &schema,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_INVALID_SCHEMA);
    CHECK(typetree_schema_registry_count(&rejected) == 0U);
    typetree_schema_registry_dispose(&rejected);
    schema.node_count = original_node_count;

    int adjacent = -1;
    for (int index = 1; index + 1 < schema.node_count; ++index) {
        const TypeTreeNode* left = &schema.nodes[index];
        const TypeTreeNode* right = &schema.nodes[index + 1];
        bool left_leaf = index + 1 >= schema.node_count ||
                         schema.nodes[index + 1].level <= left->level;
        bool right_leaf = index + 2 >= schema.node_count ||
                          schema.nodes[index + 2].level <= right->level;
        if (left_leaf && right_leaf && left->level == right->level &&
            strcmp(left->name_str, "size") != 0 &&
            strcmp(right->name_str, "size") != 0 &&
            strcmp(left->name_str, "data") != 0 &&
            strcmp(right->name_str, "data") != 0) {
            adjacent = index;
            break;
        }
    }
    CHECK(adjacent > 0);
    TypeTreeNode temporary = schema.nodes[adjacent];
    schema.nodes[adjacent] = schema.nodes[adjacent + 1];
    schema.nodes[adjacent + 1] = temporary;
    CHECK(typetree_validate_schema(&schema));
    CHECK(typetree_schema_validate_known_profile(
              key.unity_version, key.unity_version_size, key.class_id,
              key.type_hash, &schema) == TYPETREE_SCHEMA_PROFILE_INVALID);
    temporary = schema.nodes[adjacent];
    schema.nodes[adjacent] = schema.nodes[adjacent + 1];
    schema.nodes[adjacent + 1] = temporary;

    schema.nodes[0].meta_flags ^= UINT32_C(0x20);
    CHECK(typetree_validate_schema(&schema));
    CHECK(typetree_schema_validate_known_profile(
              key.unity_version, key.unity_version_size, key.class_id,
              key.type_hash, &schema) == TYPETREE_SCHEMA_PROFILE_INVALID);
    schema.nodes[0].meta_flags ^= UINT32_C(0x20);
    CHECK(typetree_schema_validate_known_profile(
              "2021.3.35f2", strlen("2021.3.35f2"), key.class_id,
              key.type_hash, &schema) == TYPETREE_SCHEMA_PROFILE_UNKNOWN);

    typetree_free_type(&schema);
    return true;
}

static bool verify_schema_profiles(void) {
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(
              &registry, SHADER_SCHEMA_PROFILE_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&registry) == 3U);
    for (size_t index = 0;
         index < sizeof(k_known_schemas) / sizeof(k_known_schemas[0]);
         ++index) {
        CHECK(mutate_known_schema(&k_known_schemas[index], &registry));
    }

    TypeTreeSchemaKey retained_player_resource =
        known_key(&k_known_schemas[1]);
    retained_player_resource.unity_version = "2021.3.29f1";
    retained_player_resource.unity_version_size =
        strlen(retained_player_resource.unity_version);
    CHECK(typetree_schema_registry_bind_exact_version(
              &registry, &retained_player_resource,
              TYPETREE_SCHEMA_PROVENANCE_UNTRUSTED_INPUT) ==
          TYPETREE_SCHEMA_INVALID_ARGUMENT);
    CHECK(typetree_schema_registry_count(&registry) == 3U);
    CHECK(typetree_schema_registry_bind_exact_version(
              &registry, &retained_player_resource,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&registry) == 4U);
    TypeTreeType retained_schema;
    CHECK(typetree_schema_registry_lookup(
              &registry, &retained_player_resource, &retained_schema) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_validate_known_profile(
              retained_player_resource.unity_version,
              retained_player_resource.unity_version_size,
              retained_player_resource.class_id,
              retained_player_resource.type_hash, &retained_schema) ==
          TYPETREE_SCHEMA_PROFILE_VALID);
    typetree_free_type(&retained_schema);

    TypeTreeSchemaKey retained_material = known_key(&k_known_schemas[0]);
    retained_material.unity_version = "2021.3.29f1";
    retained_material.unity_version_size =
        strlen(retained_material.unity_version);
    CHECK(typetree_schema_registry_bind_exact_version(
              &registry, &retained_material,
              TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_registry_count(&registry) == 5U);
    CHECK(typetree_schema_registry_lookup(
              &registry, &retained_material, &retained_schema) ==
          TYPETREE_SCHEMA_OK);
    CHECK(typetree_schema_validate_known_profile(
              retained_material.unity_version,
              retained_material.unity_version_size,
              retained_material.class_id,
              retained_material.type_hash, &retained_schema) ==
          TYPETREE_SCHEMA_PROFILE_VALID);
    typetree_free_type(&retained_schema);

    SerializedShaderSchemaProfile shader_profile;
    CHECK(serialized_shader_profile_from_unity_version(
        "2021.3.29f1", &shader_profile));
    CHECK(shader_profile ==
          SERIALIZED_SHADER_PROFILE_UNITY_2021_3_29F1_PLAYER_RESOURCES);
    CHECK(!serialized_shader_profile_from_unity_version(
        "2021.3.29f2", &shader_profile));
    typetree_schema_registry_dispose(&registry);
    return true;
}

static bool rejection_preserves_destination(SerializedShader* shader,
                                            const TypeTreeValue* root) {
    const char* retained_name = shader->name;
    return !serialized_shader_parse_with_profile(
               shader, root,
               SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1) &&
           shader->name == retained_name;
}

static TypeTreeValue* first_player_subprogram(TypeTreeValue* pass) {
    if (!pass || pass->type != VAL_TYPE_STRUCT ||
        pass->struct_val.count != 19) {
        return NULL;
    }
    for (int stage = 6; stage <= 11; ++stage) {
        TypeTreeValue* program = &pass->struct_val.members[stage];
        if (program->type != VAL_TYPE_STRUCT ||
            program->struct_val.count != 4) {
            continue;
        }
        TypeTreeValue* groups = &program->struct_val.members[1];
        if (groups->type != VAL_TYPE_ARRAY ||
            groups->array_val.storage != TYPETREE_ARRAY_VALUES) {
            continue;
        }
        for (int group = 0; group < groups->array_val.count; ++group) {
            TypeTreeValue* inner = &groups->array_val.elements[group];
            if (inner->type == VAL_TYPE_ARRAY &&
                inner->array_val.storage == TYPETREE_ARRAY_VALUES &&
                inner->array_val.count > 0 && inner->array_val.elements) {
                return &inner->array_val.elements[0];
            }
        }
    }
    return NULL;
}

typedef struct {
    bool root;
    bool property;
    bool pass;
    bool render_state;
    bool subprogram;
    size_t shaders;
} MutationCoverage;

static bool mutate_shader_value(TypeTreeValue* root,
                                MutationCoverage* coverage) {
    SerializedShader shader;
    serialized_shader_init(&shader);
    CHECK(serialized_shader_parse_with_profile(
        &shader, root, SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1));
    coverage->shaders++;

    if (!coverage->root) {
        int count = root->struct_val.count;
        root->struct_val.count = count - 1;
        CHECK(rejection_preserves_destination(&shader, root));
        root->struct_val.count = count;

        TypeTreeValue temporary = root->struct_val.members[1];
        root->struct_val.members[1] = root->struct_val.members[2];
        root->struct_val.members[2] = temporary;
        CHECK(rejection_preserves_destination(&shader, root));
        temporary = root->struct_val.members[1];
        root->struct_val.members[1] = root->struct_val.members[2];
        root->struct_val.members[2] = temporary;

        ValueType parsed_type = root->struct_val.members[1].type;
        root->struct_val.members[1].type = VAL_TYPE_ARRAY;
        CHECK(rejection_preserves_destination(&shader, root));
        root->struct_val.members[1].type = parsed_type;
        coverage->root = true;
    }

    TypeTreeValue* parsed = &root->struct_val.members[1];
    TypeTreeValue* property_info = &parsed->struct_val.members[0];
    TypeTreeValue* properties = &property_info->struct_val.members[0];
    if (!coverage->property && properties->type == VAL_TYPE_ARRAY &&
        properties->array_val.storage == TYPETREE_ARRAY_VALUES &&
        properties->array_val.count > 0) {
        TypeTreeValue* property = &properties->array_val.elements[0];
        int count = property->struct_val.count;
        property->struct_val.count = count - 1;
        CHECK(rejection_preserves_destination(&shader, root));
        property->struct_val.count = count;
        ValueType default_type = property->struct_val.members[5].type;
        property->struct_val.members[5].type = VAL_TYPE_INT;
        CHECK(rejection_preserves_destination(&shader, root));
        property->struct_val.members[5].type = default_type;
        coverage->property = true;
    }

    TypeTreeValue* subshaders = &parsed->struct_val.members[1];
    if (subshaders->type == VAL_TYPE_ARRAY &&
        subshaders->array_val.storage == TYPETREE_ARRAY_VALUES &&
        subshaders->array_val.count > 0) {
        TypeTreeValue* subshader = &subshaders->array_val.elements[0];
        TypeTreeValue* passes = &subshader->struct_val.members[0];
        if (passes->type == VAL_TYPE_ARRAY &&
            passes->array_val.storage == TYPETREE_ARRAY_VALUES &&
            passes->array_val.count > 0) {
            TypeTreeValue* pass = &passes->array_val.elements[0];
            if (!coverage->pass) {
                int count = pass->struct_val.count;
                pass->struct_val.count = count - 1;
                CHECK(rejection_preserves_destination(&shader, root));
                pass->struct_val.count = count;
                TypeTreeValue temporary = pass->struct_val.members[3];
                pass->struct_val.members[3] = pass->struct_val.members[4];
                pass->struct_val.members[4] = temporary;
                CHECK(rejection_preserves_destination(&shader, root));
                temporary = pass->struct_val.members[3];
                pass->struct_val.members[3] = pass->struct_val.members[4];
                pass->struct_val.members[4] = temporary;
                coverage->pass = true;
            }
            TypeTreeValue* state = &pass->struct_val.members[4];
            if (!coverage->render_state) {
                int count = state->struct_val.count;
                state->struct_val.count = count - 1;
                CHECK(rejection_preserves_destination(&shader, root));
                state->struct_val.count = count;
                TypeTreeValue* z_test = &state->struct_val.members[11];
                ValueType value_type = z_test->struct_val.members[0].type;
                z_test->struct_val.members[0].type = VAL_TYPE_INT;
                CHECK(rejection_preserves_destination(&shader, root));
                z_test->struct_val.members[0].type = value_type;
                coverage->render_state = true;
            }
            TypeTreeValue* player = first_player_subprogram(pass);
            if (!coverage->subprogram && player) {
                int count = player->struct_val.count;
                player->struct_val.count = count - 1;
                CHECK(rejection_preserves_destination(&shader, root));
                player->struct_val.count = count;
                ValueType blob_type = player->struct_val.members[0].type;
                player->struct_val.members[0].type = VAL_TYPE_STRING;
                CHECK(rejection_preserves_destination(&shader, root));
                player->struct_val.members[0].type = blob_type;
                coverage->subprogram = true;
            }
        }
    }
    serialized_shader_free(&shader);
    return true;
}

static bool verify_shader_value_profile(void) {
    CommonFileBytes input;
    CHECK(common_file_read_regular(SHADER_SCHEMA_PROFILE_BUNDLE,
                                   SIZE_MAX, &input) == COMMON_FILE_OK);
    BundleArchive bundle;
    CHECK(bundle_open(&bundle, input.data, input.size));
    MutationCoverage coverage;
    memset(&coverage, 0, sizeof(coverage));

    for (int member_index = 0; member_index < bundle.directory_count;
         ++member_index) {
        const BundleDirectoryInfo* member =
            &bundle.directories[member_index];
        if (bundle_member_classify(member) !=
            BUNDLE_MEMBER_SERIALIZED_FILE) {
            continue;
        }
        const uint8_t* member_data = NULL;
        size_t member_size = 0U;
        CHECK(bundle_get_member_view(&bundle, (size_t)member_index,
                                     &member_data, &member_size));
        SerializedFile file;
        CHECK(serialized_file_open(&file, member_data, member_size));
        for (int object_index = 0; object_index < file.object_count;
             ++object_index) {
            const AssetObjectInfo* object = &file.objects[object_index];
            if (object->type_id != 48 || object->type_id_or_index < 0 ||
                object->type_id_or_index >= file.type_count) {
                continue;
            }
            size_t object_size = 0U;
            const uint8_t* object_data = serialized_file_get_object_data(
                &file, object, &object_size);
            CHECK(object_data != NULL && object_size != 0U);
            ByteStream stream;
            stream_init(&stream, object_data, object_size);
            stream_set_endian(&stream, file.big_endian);
            TypeTreeValue root;
            memset(&root, 0, sizeof(root));
            int node_index = 0;
            CHECK(typetree_parse_value_ex(
                &file.types[object->type_id_or_index], &node_index,
                &stream, &root, TYPETREE_PARSE_PACK_BYTE_ARRAYS));
            CHECK(stream.position == object_size);
            CHECK(node_index == file.types[object->type_id_or_index].node_count);
            CHECK(mutate_shader_value(&root, &coverage));
            typetree_free_value(&root);
            if (coverage.root && coverage.property && coverage.pass &&
                coverage.render_state && coverage.subprogram) {
                break;
            }
        }
        serialized_file_close(&file);
        if (coverage.root && coverage.property && coverage.pass &&
            coverage.render_state && coverage.subprogram) {
            break;
        }
    }
    bundle_close(&bundle);
    common_file_bytes_dispose(&input);
    CHECK(coverage.shaders > 0U);
    CHECK(coverage.root && coverage.property && coverage.pass &&
          coverage.render_state && coverage.subprogram);
    return true;
}

int main(void) {
    size_t allocations_before = g_allocations_count;
    size_t bytes_before = g_allocated_bytes;
    if (!verify_schema_profiles() || !verify_shader_value_profile()) return 1;
    if (g_allocations_count != allocations_before ||
        g_allocated_bytes != bytes_before) {
        fprintf(stderr, "tracked allocation leak: blocks=%zu/%zu bytes=%zu/%zu\n",
                (size_t)g_allocations_count, allocations_before,
                (size_t)g_allocated_bytes, bytes_before);
        return 1;
    }
    puts("Pinned Shader schema/value profile tests passed.");
    return 0;
}
