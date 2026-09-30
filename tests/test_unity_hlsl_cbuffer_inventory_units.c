// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_hlsl_cbuffer_inventory.h"
#include "common/sha256.h"
#include "common/string_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #value); return false; } } while (0)

/* Controlled declarations, not copied compiler includes. */
static const char source[] =
    "#line 14 \"generic/include.cginc\"\n#pragma warning(disable:3205)\n"
    "cbuffer ShapeData { float4x4 transform; float3 direction; float strength; float2 offset; };\n"
    "cbuffer CameraData { half4 tint; column_major float4x4 projection; int eye; float4 background; };\n"
    "float4 entry(float4 p : POSITION) : SV_POSITION { return mul(transform, p); }\n";
static const char *const names[] = {"ShapeData", "CameraData"};
static const UnityHlslCBufferStoragePolicy legacy = {true};

static UnityHlslCBufferStatus build(const char *text, UnityHlslCBufferInventory *out) {
    return unity_hlsl_cbuffer_inventory_build((const uint8_t *)text, strlen(text), names, 2, legacy, out);
}

static bool status_is(const char *text, UnityHlslCBufferStatus expected) {
    UnityHlslCBufferInventory inventory;
    unity_hlsl_cbuffer_inventory_init(&inventory);
    const UnityHlslCBufferStatus actual = build(text, &inventory);
    if (actual != expected) fprintf(stderr, "Expected %s, received %s\n", unity_hlsl_cbuffer_status_name(expected), unity_hlsl_cbuffer_status_name(actual));
    CHECK(actual == expected);
    if (actual != UNITY_HLSL_CBUFFER_OK) {
        const UnityHlslCBufferInventory empty = {0};
        CHECK(!memcmp(&inventory, &empty, sizeof(empty)));
    }
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    return true;
}

static char *replace(const char *before, const char *after) {
    const char *found = strstr(source, before);
    if (!found) return NULL;
    const size_t prefix = (size_t)(found - source), suffix = strlen(found + strlen(before));
    char *changed = malloc(prefix + strlen(after) + suffix + 1);
    if (!changed) return NULL;
    memcpy(changed, source, prefix);
    memcpy(changed + prefix, after, strlen(after));
    memcpy(changed + prefix + strlen(after), found + strlen(before), suffix + 1);
    return changed;
}

static bool packing_and_metadata(void) {
    UnityHlslCBufferInventory inventory;
    unity_hlsl_cbuffer_inventory_init(&inventory);
    CHECK(build(source, &inventory) == UNITY_HLSL_CBUFFER_OK);
    CHECK(inventory.block_count == 2 && inventory.field_count == 8);
    CHECK(inventory.blocks[0].byte_size == 96 && inventory.blocks[0].field_count == 4);
    CHECK(inventory.blocks[1].byte_size == 112 && inventory.blocks[1].field_count == 4);
    const uint32_t offsets[] = {0, 64, 76, 80, 0, 16, 80, 96};
    const uint32_t sizes[] = {64, 12, 4, 8, 16, 64, 4, 16};
    for (size_t field = 0; field < 8; ++field) {
        CHECK(inventory.fields[field].byte_offset == offsets[field]);
        CHECK(inventory.fields[field].byte_size == sizes[field]);
        CHECK(inventory.fields[field].source_begin < inventory.fields[field].source_end);
        CHECK(inventory.fields[field].source_end <= strlen(source));
    }
    CHECK(inventory.fields[0].is_matrix && inventory.fields[0].rows == 4 && inventory.fields[0].columns == 4);
    CHECK(inventory.fields[4].scalar == UNITY_HLSL_CBUFFER_HALF && !inventory.fields[4].is_matrix);
    CHECK(unity_hlsl_cbuffer_inventory_matches(&inventory, (const uint8_t *)source, strlen(source), names, 2, legacy));
    SerializedVariable variable = {.name = "transform", .layout = {0, 0, 0, 4, 1, 0}};
    SerializedProgramParameters parameters = {0};
    CHECK(unity_hlsl_cbuffer_field_matches_float_parameter(&inventory.fields[0], &parameters, &variable));
    for (unsigned word = 0; word < 5; ++word) {
        const uint32_t saved = variable.layout[word];
        variable.layout[word] = word == 3 ? 3 : word == 4 ? 0 : 1;
        CHECK(!unity_hlsl_cbuffer_field_matches_float_parameter(&inventory.fields[0], &parameters, &variable));
        variable.layout[word] = saved;
    }
    variable.name = "other";
    CHECK(!unity_hlsl_cbuffer_field_matches_float_parameter(&inventory.fields[0], &parameters, &variable));
    variable.name = "transform";
    variable.layout[0] = 0; variable.layout[1] = 4; variable.layout[2] = 0; variable.layout[3] = 1; variable.layout[4] = 0; variable.layout[5] = 0;
    parameters.is_binary = true;
    CHECK(unity_hlsl_cbuffer_field_matches_float_parameter(&inventory.fields[0], &parameters, &variable));
    variable.layout[5] = 16;
    CHECK(!unity_hlsl_cbuffer_field_matches_float_parameter(&inventory.fields[0], &parameters, &variable));
    CHECK(!unity_hlsl_cbuffer_field_matches_float_parameter(NULL, &parameters, &variable));
    const char *const reversed[] = {"CameraData", "ShapeData"};
    UnityHlslCBufferInventory reordered;
    unity_hlsl_cbuffer_inventory_init(&reordered);
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)source, strlen(source), reversed, 2, legacy, &reordered) == UNITY_HLSL_CBUFFER_OK);
    CHECK(!strcmp(reordered.blocks[0].name, "CameraData") && reordered.blocks[0].first_field == 4);
    CHECK(!strcmp(reordered.blocks[1].name, "ShapeData") && reordered.blocks[1].first_field == 0);
    unity_hlsl_cbuffer_inventory_dispose(&reordered);
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    return true;
}

static bool mutation_rejection(void) {
    const struct { const char *before, *after; UnityHlslCBufferStatus status; } cases[] = {
        {"cbuffer ShapeData {", "cbuffer ShapeData : register(b3) {", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float4x4 transform;", "row_major float4x4 transform;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float4x4 transform;", "float4x4 transform[2];", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float3 direction;", "float3 direction : packoffset(c4);", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float3 direction;", "float3 direction, alternate;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float3 direction;", "struct Item { float3 x; } direction;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float strength;", "float direction;", UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION},
        {"float4 background;", "float4 transform;", UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION},
        {"float2 offset;", "min16float2 offset;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float3 direction;", "column_major float3 direction;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"cbuffer CameraData", "cbuffer AnotherData", UNITY_HLSL_CBUFFER_MISSING_DECLARATION},
        {"float4 entry(", "float transform; float4 entry(", UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION},
        {"float4 entry(", "cbuffer Other { float transform; }; float4 entry(", UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION},
        {"float4 entry(", "tbuffer Other { float transform; }; float4 entry(", UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION},
        {"float4 entry(", "cbuffer ShapeData {float value;}; float4 entry(", UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION},
        {"float4 entry(", "float ShapeData; float4 entry(", UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION},
        {"return mul(transform, p);", "return mul(transform, p];", UNITY_HLSL_CBUFFER_MALFORMED_EXPANSION},
        {"#line 14", "#line 14u", UNITY_HLSL_CBUFFER_UNSUPPORTED_DIRECTIVE},
        {"#line 14", "#pragma pack_matrix(row_major)\n#line 14", UNITY_HLSL_CBUFFER_UNSUPPORTED_DIRECTIVE},
        {"#line 14", "#define transform other\n#line 14", UNITY_HLSL_CBUFFER_UNSUPPORTED_DIRECTIVE},
        {"float3 direction;", "float3 /*", UNITY_HLSL_CBUFFER_MALFORMED_EXPANSION},
        {"float3 direction;", "float3 float;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float3 direction;", "float5 direction;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float4x4 transform;", "float3x3 transform;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
        {"float4x4 transform;", "float2x3 transform;", UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char *changed = replace(cases[i].before, cases[i].after);
        CHECK(changed && status_is(changed, cases[i].status));
        free(changed);
    }
    UnityHlslCBufferInventory inventory;
    unity_hlsl_cbuffer_inventory_init(&inventory);
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)source, strlen(source), names, 2,
        (UnityHlslCBufferStoragePolicy){0}, &inventory) == UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION);
    CHECK(build(source, &inventory) == UNITY_HLSL_CBUFFER_OK);
    for (unsigned mutation = 0; mutation < 15; ++mutation) {
        UnityHlslCBufferBlock saved_block = inventory.blocks[0];
        UnityHlslCBufferField saved_field = inventory.fields[0];
        if (mutation == 0) ++inventory.blocks[0].byte_size;
        if (mutation == 1) ++inventory.blocks[0].first_field;
        if (mutation == 2) ++inventory.blocks[0].field_count;
        if (mutation == 3) ++inventory.blocks[0].source_begin;
        if (mutation == 4) --inventory.blocks[0].source_end;
        if (mutation == 5) inventory.blocks[0].name[0] = 'X';
        if (mutation == 6) inventory.fields[0].scalar = UNITY_HLSL_CBUFFER_UINT;
        if (mutation == 7) --inventory.fields[0].rows;
        if (mutation == 8) --inventory.fields[0].columns;
        if (mutation == 9) inventory.fields[0].is_matrix = false;
        if (mutation == 10) ++inventory.fields[0].byte_offset;
        if (mutation == 11) --inventory.fields[0].byte_size;
        if (mutation == 12) ++inventory.fields[0].source_begin;
        if (mutation == 13) --inventory.fields[0].source_end;
        if (mutation == 14) inventory.fields[0].name[0] = 'X';
        CHECK(!unity_hlsl_cbuffer_inventory_matches(&inventory, (const uint8_t *)source, strlen(source), names, 2, legacy));
        inventory.blocks[0] = saved_block;
        inventory.fields[0] = saved_field;
    }
    char *changed = replace("float3 direction;", "float4 direction;");
    CHECK(changed && !unity_hlsl_cbuffer_inventory_matches(&inventory, (const uint8_t *)changed, strlen(changed), names, 2, legacy));
    free(changed);
    inventory.expansion_digest[0] ^= 1;
    CHECK(!unity_hlsl_cbuffer_inventory_matches(&inventory, (const uint8_t *)source, strlen(source), names, 2, legacy));
    inventory.expansion_digest[0] ^= 1;
    inventory.storage.legacy_half_is_float32 = false;
    CHECK(!unity_hlsl_cbuffer_inventory_matches(&inventory, (const uint8_t *)source, strlen(source), names, 2, legacy));
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    return true;
}

static bool initializer_uses_and_aliases(void) {
    const char *const uses[] = {
        "static float4x4 combined = mul(projection, transform); ",
        "static float4x4 inverse = transpose(mul(transform, projection)); ",
        "static float amount = strength, another = direction.x; ",
        "static float2 positions[2] = {offset, direction.xy}; ",
        "static float amount = (strength > 0 ? direction.x : offset.y); ",
        "cbuffer Other { float amount = strength; }; ",
        "tbuffer Other { float amount = strength; }; "
    };
    for (size_t index = 0; index < sizeof(uses) / sizeof(uses[0]); ++index) {
        StringBuilder declaration; sb_init(&declaration);
        sb_append(&declaration, uses[index]);
        sb_append(&declaration, "float4 entry(");
        CHECK(sb_ok(&declaration));
        char *changed = replace("float4 entry(", declaration.buf);
        CHECK(changed && status_is(changed, UNITY_HLSL_CBUFFER_OK));
        free(changed); sb_free(&declaration);
    }
    const char *const aliases[] = {
        "static float strength = direction.x; ",
        "static float amount = direction.x, strength = offset.y; ",
        "static float amount = (direction.x + offset.y), strength; ",
        "static float2 positions[2] = {offset, direction.xy}, offset; ",
        "cbuffer Other { float amount = strength; float transform; }; ",
        "tbuffer Other { float amount = strength, transform; }; ",
        "static float amount = strength; float CameraData; "
    };
    for (size_t index = 0; index < sizeof(aliases) / sizeof(aliases[0]); ++index) {
        StringBuilder declaration; sb_init(&declaration);
        sb_append(&declaration, aliases[index]);
        sb_append(&declaration, "float4 entry(");
        CHECK(sb_ok(&declaration));
        char *changed = replace("float4 entry(", declaration.buf);
        CHECK(changed && status_is(changed, UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION));
        free(changed); sb_free(&declaration);
    }
    return true;
}

static bool limits_and_boundaries(void) {
    UnityHlslCBufferInventory inventory;
    unity_hlsl_cbuffer_inventory_init(&inventory);
    CHECK(unity_hlsl_cbuffer_inventory_build(NULL, 1, names, 2, legacy, &inventory) == UNITY_HLSL_CBUFFER_INVALID_ARGUMENT);
    CHECK(unity_hlsl_cbuffer_inventory_build(NULL, 0, names, 2, legacy, &inventory) == UNITY_HLSL_CBUFFER_MISSING_DECLARATION);
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)source, UNITY_HLSL_EXPANSION_BYTE_LIMIT + 1u, names, 2, legacy, &inventory) == UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT);
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)source, strlen(source), names, 0, legacy, &inventory) == UNITY_HLSL_CBUFFER_INVALID_ARGUMENT);
    const char *const duplicates[] = {"ShapeData", "ShapeData"};
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)source, strlen(source), duplicates, 2, legacy, &inventory) == UNITY_HLSL_CBUFFER_INVALID_ARGUMENT);
    const char *const invalid[] = {"float4"};
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)source, strlen(source), invalid, 1, legacy, &inventory) == UNITY_HLSL_CBUFFER_INVALID_ARGUMENT);
    CHECK(build(source, &inventory) == UNITY_HLSL_CBUFFER_OK);
    CHECK(build(source, &inventory) == UNITY_HLSL_CBUFFER_INVALID_ARGUMENT); /* No leaked replacement. */
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    const char *const one[] = {"Data"};
    const char packed[] = "cbuffer Data {float3 first; float2 next; float4x4 matrixValue; bool enabled; uint count;};";
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)packed, strlen(packed), one, 1, (UnityHlslCBufferStoragePolicy){0}, &inventory) == UNITY_HLSL_CBUFFER_OK);
    CHECK(inventory.field_count == 5 && inventory.blocks[0].byte_size == 112);
    CHECK(inventory.fields[1].byte_offset == 16 && inventory.fields[2].byte_offset == 32 && inventory.fields[2].byte_size == 64);
    CHECK(inventory.fields[3].byte_offset == 96 && inventory.fields[4].byte_offset == 100);
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    StringBuilder large;
    sb_init(&large);
    sb_append(&large, "cbuffer Data {");
    for (unsigned field = 0; field <= UNITY_HLSL_CBUFFER_FIELD_LIMIT; ++field) sb_appendf(&large, "float4 field%u;", field);
    sb_append(&large, "};");
    CHECK(sb_ok(&large));
    CHECK(unity_hlsl_cbuffer_inventory_build((const uint8_t *)large.buf, large.len, one, 1, legacy, &inventory) == UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT);
    sb_free(&large);
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    return true;
}

int main(void) {
    if (!packing_and_metadata() || !mutation_rejection() || !initializer_uses_and_aliases() || !limits_and_boundaries()) return 1;
    puts("Expanded HLSL cbuffer inventory tests passed");
    return 0;
}
