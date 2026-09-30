// SPDX-License-Identifier: GPL-3.0-only

#include "io/subprogram_metadata.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static bool complete_equality(void) {
    SerializedVariable fields[2] = {
        {.name = "Active", .layout = {0, 1, 4, 0, 0, 0}},
        {.name = "Dormant", .layout = {16, 2, 2, 0, 8, 0}}
    };
    SerializedVariable member = {.name = "Member", .layout = {32, 1, 4, 0, 0, 2}};
    SerializedStructParam structure = {.name = "DormantStruct", .layout = {32, 16, 1},
        .member_count = 1, .members = &member};
    SerializedConstantBuffer buffer = {.name = "Block", .role = SERIALIZED_CBUFFER_NAMED,
        .size = 64, .has_is_partial = true, .is_partial = true,
        .var_count = 2, .variables = fields, .struct_count = 1, .struct_params = &structure};
    SerializedResourceParam resources[2] = {
        {.name = "Block", .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .bind_index = 2,
            .array_size = 1, .dimension = 4, .sampler_index = 7, .multisampled = true,
            .original_index = 9, .sampler_state = 11, .extra = {13, 17}},
        {.name = "DormantTexture", .bind_type = SERIALIZED_RESOURCE_TEXTURE, .bind_index = 5,
            .extra = {19, 23}}
    };
    SerializedProgramParameters model = {.version = UNITY_2021_3_PLAYER_BLOB_VERSION,
        .dialect = PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1, .is_binary = true,
        .cb_count = 1, .constant_buffers = &buffer, .res_count = 2, .resources = resources};
    SerializedProgramParameters copy = {0};
    CHECK(serialized_program_parameters_copy(&copy, &model));
    CHECK(copy.constant_buffers != model.constant_buffers && copy.resources != model.resources);
    CHECK(copy.constant_buffers[0].name != model.constant_buffers[0].name);
    CHECK(serialized_program_parameters_equal(&model, &copy));
    CHECK(serialized_program_parameters_equal(&copy, &model));
    CHECK(serialized_program_parameters_equal(&model, &model));
#define MUTATION(expression, changed) do { \
    expression = (changed); \
    CHECK(!serialized_program_parameters_equal(&model, &copy)); \
    CHECK(!serialized_program_parameters_equal(&copy, &model)); \
    serialized_program_parameters_free(&copy); \
    CHECK(serialized_program_parameters_copy(&copy, &model)); \
} while (0)
    MUTATION(copy.version, model.version + 1);
    MUTATION(copy.dialect, PLAYER_BLOB_DIALECT_INVALID);
    MUTATION(copy.is_binary, false);
    MUTATION(copy.constant_buffers[0].role, SERIALIZED_CBUFFER_LOOSE_PARAMETERS);
    MUTATION(copy.constant_buffers[0].size, 80);
    MUTATION(copy.constant_buffers[0].has_is_partial, false);
    MUTATION(copy.constant_buffers[0].is_partial, false);
    MUTATION(copy.constant_buffers[0].variables[1].layout[4], 9);
    for (size_t word = 0; word < sizeof(fields[1].layout) / sizeof(fields[1].layout[0]); ++word)
        MUTATION(copy.constant_buffers[0].variables[1].layout[word], fields[1].layout[word] + 1);
    MUTATION(copy.constant_buffers[0].variables[1].name, "OtherDormant");
    for (size_t word = 0; word < sizeof(structure.layout) / sizeof(structure.layout[0]); ++word)
        MUTATION(copy.constant_buffers[0].struct_params[0].layout[word], structure.layout[word] + 1);
    MUTATION(copy.constant_buffers[0].struct_params[0].name, "OtherStruct");
    MUTATION(copy.constant_buffers[0].struct_params[0].members[0].layout[5], 3);
    MUTATION(copy.constant_buffers[0].struct_params[0].members[0].name, "OtherMember");
    MUTATION(copy.resources[0].bind_type, SERIALIZED_RESOURCE_BUFFER);
    MUTATION(copy.resources[0].bind_index, 3);
    MUTATION(copy.resources[0].array_size, 2);
    MUTATION(copy.resources[0].dimension, 5);
    MUTATION(copy.resources[0].sampler_index, 8);
    MUTATION(copy.resources[0].multisampled, false);
    MUTATION(copy.resources[0].original_index, 10);
    MUTATION(copy.resources[0].sampler_state, 12);
    MUTATION(copy.resources[1].extra[0], 20);
    MUTATION(copy.resources[1].extra[1], 24);
    MUTATION(copy.resources[1].name, "OtherTexture");
#undef MUTATION
    /* Invalid same-address models must not shortcut shape validation. */
    const char *name = fields[0].name;
    fields[0].name = NULL;
    CHECK(!serialized_program_parameters_equal(&model, &model));
    fields[0].name = name;
    buffer.var_count = -1;
    CHECK(!serialized_program_parameters_equal(&model, &model));
    buffer.var_count = 2;
    structure.member_count = -1;
    CHECK(!serialized_program_parameters_equal(&model, &model));
    structure.member_count = 1;
    SerializedProgramParameters invalid = model;
    invalid.resources = NULL;
    CHECK(!serialized_program_parameters_equal(&model, &invalid));
    CHECK(!serialized_program_parameters_equal(NULL, &model));
    CHECK(!serialized_program_parameters_equal(&model, NULL));
    CHECK(serialized_program_parameters_equal(&model, &copy));
    serialized_program_parameters_free(&copy);
    return true;
}

static bool player_equality(void) {
    uint8_t target[4] = {1, 2, 3, 4};
    char *local[2] = {"LOCAL_A", "LOCAL_B"};
    char *global[2] = {"GLOBAL_A", "GLOBAL_B"};
    PlayerSubProgramBindChannel bindings[2] = {{0, 2}, {1, 3}};
    PlayerSubProgramMetadata original = {.version = UNITY_2021_3_PLAYER_BLOB_VERSION,
        .dialect = PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1, .program_type = 16,
        .has_player_blob_header = true, .player_header_words = {4, 7, 2, 11},
        .source_map = 13, .local_keyword_count = 2, .local_keywords = local,
        .global_keyword_count = 2, .global_keywords = global,
        .bytecode = target, .bytecode_length = sizeof(target),
        .binding_count = 2, .bindings = bindings};
    PlayerSubProgramMetadata changed = original;
    CHECK(subprogram_metadata_variant_equal(&original, &changed));
#define PLAYER_MUTATION(expression, value) do {     expression = (value);     CHECK(!subprogram_metadata_variant_equal(&original, &changed));     changed = original; } while (0)
    PLAYER_MUTATION(changed.version, original.version + 1);
    PLAYER_MUTATION(changed.dialect, PLAYER_BLOB_DIALECT_INVALID);
    PLAYER_MUTATION(changed.program_type, 18);
    PLAYER_MUTATION(changed.has_player_blob_header, false);
    for (size_t word = 0; word < 4; ++word)
        PLAYER_MUTATION(changed.player_header_words[word], original.player_header_words[word] + 1);
    PLAYER_MUTATION(changed.source_map, original.source_map + 1);
    PLAYER_MUTATION(changed.bytecode_length, original.bytecode_length - 1);
    PLAYER_MUTATION(changed.binding_count, 1);
    PLAYER_MUTATION(changed.local_keyword_count, 1);
    PLAYER_MUTATION(changed.global_keyword_count, 1);
    PLAYER_MUTATION(changed.bytecode, NULL);
#undef PLAYER_MUTATION
    char *reordered[2] = {local[1], local[0]};
    changed.local_keywords = reordered;
    CHECK(!subprogram_metadata_variant_equal(&original, &changed));
    changed = original;
    char *reordered_global[2] = {global[1], global[0]};
    changed.global_keywords = reordered_global;
    CHECK(!subprogram_metadata_variant_equal(&original, &changed));
    changed = original;
    PlayerSubProgramBindChannel other_bindings[2] = {{0, 2}, {1, 2}};
    changed.bindings = other_bindings;
    CHECK(!subprogram_metadata_variant_equal(&original, &changed));
    other_bindings[1] = bindings[1];
    other_bindings[0].channel = 1;
    CHECK(!subprogram_metadata_variant_equal(&original, &changed));
    changed = original;
    uint8_t other_target[4] = {1, 2, 3, 5};
    changed.bytecode = other_target;
    CHECK(!subprogram_metadata_variant_equal(&original, &changed));
    changed = original;
    char *duplicates[2] = {local[0], local[0]};
    changed.local_keywords = duplicates;
    CHECK(!subprogram_metadata_variant_equal(&changed, &changed));
    CHECK(!subprogram_metadata_variant_equal(NULL, &original));
    CHECK(!subprogram_metadata_variant_equal(&original, NULL));
    CHECK(subprogram_metadata_variant_equal(&original, &original));
    return true;
}

int main(void) {
    return complete_equality() && player_equality() ? 0 : 1;
}
