#include "io/material_object.h"
#include "io/typetree_schema_registry.h"

#include <stdio.h>
#include <string.h>

#ifndef MATERIAL_OBJECT_TEST_SCHEMA_REGISTRY
#define MATERIAL_OBJECT_TEST_SCHEMA_REGISTRY \
    "schemas/unity-2021.3.35f1-shader.registry"
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint8_t bytes[4096];
    size_t size;
    size_t material_name_data_offset;
    size_t string_tag_count_offset;
    TypeTreeType type;
    AssetObjectInfo object;
    SerializedFile file;
} MaterialFixture;

static void store_le32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static bool append_bytes(MaterialFixture* fixture, const void* bytes,
                         size_t size) {
    if (fixture->size > sizeof(fixture->bytes) ||
        size > sizeof(fixture->bytes) - fixture->size) return false;
    memcpy(fixture->bytes + fixture->size, bytes, size);
    fixture->size += size;
    return true;
}

static bool append_u8(MaterialFixture* fixture, uint8_t value) {
    return append_bytes(fixture, &value, sizeof(value));
}

static bool append_u32(MaterialFixture* fixture, uint32_t value) {
    uint8_t bytes[4];
    store_le32(bytes, value);
    return append_bytes(fixture, bytes, sizeof(bytes));
}

static bool append_i32(MaterialFixture* fixture, int32_t value) {
    return append_u32(fixture, (uint32_t)value);
}

static bool append_i64(MaterialFixture* fixture, int64_t value) {
    uint8_t bytes[8];
    uint64_t raw = (uint64_t)value;
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(raw >> (index * 8U));
    }
    return append_bytes(fixture, bytes, sizeof(bytes));
}

static bool append_align4(MaterialFixture* fixture) {
    while ((fixture->size & 3U) != 0U) {
        if (!append_u8(fixture, 0xa5U)) return false;
    }
    return true;
}

static bool append_string_bytes(MaterialFixture* fixture,
                                const uint8_t* bytes, size_t size) {
    return size <= UINT32_MAX && append_u32(fixture, (uint32_t)size) &&
        append_bytes(fixture, bytes, size) && append_align4(fixture);
}

static bool append_string(MaterialFixture* fixture, const char* value) {
    return append_string_bytes(fixture, (const uint8_t*)value,
                               strlen(value));
}

static bool append_pptr(MaterialFixture* fixture, int32_t file_id,
                        int64_t path_id) {
    return append_i32(fixture, file_id) && append_i64(fixture, path_id);
}

static int build_material_payload(MaterialFixture* fixture,
                                  bool duplicate_tag) {
    static const uint8_t material_name[] = {
        'M', 'a', 't', ' ', 0xcfU, 0x80U,
    };
    fixture->size = 0U;
    CHECK(append_u32(fixture, (uint32_t)sizeof(material_name)));
    fixture->material_name_data_offset = fixture->size;
    CHECK(append_bytes(fixture, material_name, sizeof(material_name)));
    CHECK(append_align4(fixture));
    CHECK(append_pptr(fixture, -2, INT64_C(0x102030405060708)));

    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "FOO"));
    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "BAR"));
    CHECK(append_u32(fixture, UINT32_C(0x80000005)));
    CHECK(append_u8(fixture, 1U));
    CHECK(append_u8(fixture, 0U));
    CHECK(append_align4(fixture));
    CHECK(append_i32(fixture, -1));

    fixture->string_tag_count_offset = fixture->size;
    CHECK(append_u32(fixture, duplicate_tag ? 2U : 1U));
    CHECK(append_string(fixture, "RenderType"));
    CHECK(append_string(fixture, "Opaque"));
    if (duplicate_tag) {
        CHECK(append_string(fixture, "RenderType"));
        CHECK(append_string(fixture, "Transparent"));
    }
    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "ShadowCaster"));

    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "_MainTex"));
    CHECK(append_pptr(fixture, 2, INT64_C(-99)));
    CHECK(append_u32(fixture, UINT32_C(0x80000000))); /* -0 */
    CHECK(append_u32(fixture, UINT32_C(0x7f800000))); /* +Inf */
    CHECK(append_u32(fixture, UINT32_C(0x7fc01234))); /* NaN payload */
    CHECK(append_u32(fixture, UINT32_C(0xff800000))); /* -Inf */

    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "_Mode"));
    CHECK(append_i32(fixture, -7));

    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "_Cutoff"));
    CHECK(append_u32(fixture, UINT32_C(0x7fa12345)));

    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "_Color"));
    CHECK(append_u32(fixture, UINT32_C(0x3f800000)));
    CHECK(append_u32(fixture, UINT32_C(0xbf800000)));
    CHECK(append_u32(fixture, UINT32_C(0x00000001)));
    CHECK(append_u32(fixture, UINT32_C(0x80000000)));

    CHECK(append_u32(fixture, 1U));
    CHECK(append_string(fixture, "Layer"));
    CHECK(append_string(fixture, "Item"));
    return 0;
}

static bool load_material_schema(TypeTreeType* type) {
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    if (typetree_schema_registry_import_file_replace(
            &registry, MATERIAL_OBJECT_TEST_SCHEMA_REGISTRY) !=
        TYPETREE_SCHEMA_OK) {
        typetree_schema_registry_dispose(&registry);
        return false;
    }
    bool found = false;
    for (size_t index = 0U;
         index < typetree_schema_registry_count(&registry); ++index) {
        TypeTreeSchemaKey key;
        const TypeTreeType* schema = NULL;
        if (typetree_schema_registry_entry_view(
                &registry, index, &key, &schema) != TYPETREE_SCHEMA_OK) {
            break;
        }
        if (key.class_id == 21 && key.unity_version_size == 11U &&
            memcmp(key.unity_version, "2021.3.35f1", 11U) == 0) {
            found = typetree_schema_registry_lookup(
                        &registry, &key, type) == TYPETREE_SCHEMA_OK;
            break;
        }
    }
    typetree_schema_registry_dispose(&registry);
    return found;
}

static bool fixture_init(MaterialFixture* fixture, bool duplicate_tag) {
    memset(fixture, 0, sizeof(*fixture));
    if (!load_material_schema(&fixture->type) ||
        build_material_payload(fixture, duplicate_tag) != 0) {
        typetree_free_type(&fixture->type);
        return false;
    }
    fixture->object.path_id = INT64_C(123456789);
    fixture->object.byte_offset = 0U;
    fixture->object.byte_size = (uint32_t)fixture->size;
    fixture->object.type_id_or_index = 0;
    fixture->object.type_id = 21;
    fixture->object.script_type_index = UINT16_MAX;

    fixture->file.file_size = fixture->size;
    fixture->file.version = 22U;
    fixture->file.data_offset = 0U;
    fixture->file.unity_version = (char*)"2021.3.35f1";
    fixture->file.target_platform = 19U;
    fixture->file.type_tree_enabled = true;
    fixture->file.type_count = 1;
    fixture->file.types = &fixture->type;
    fixture->file.object_count = 1;
    fixture->file.objects = &fixture->object;
    fixture->file.raw_data = fixture->bytes;
    fixture->file.raw_size = fixture->size;
    return true;
}

static void fixture_dispose(MaterialFixture* fixture) {
    typetree_free_type(&fixture->type);
    memset(fixture, 0, sizeof(*fixture));
}

static bool view_equals(MaterialStringView view, const char* text) {
    size_t size = strlen(text);
    return view.size == size &&
        (size == 0U || memcmp(view.bytes, text, size) == 0);
}

int main(void) {
    size_t baseline_bytes = atomic_load_explicit(
        &g_allocated_bytes, memory_order_relaxed);
    size_t baseline_allocations = atomic_load_explicit(
        &g_allocations_count, memory_order_relaxed);
    MaterialFixture fixture;
    CHECK(fixture_init(&fixture, false));

    MaterialObject material;
    material_object_init(&material);
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) == MATERIAL_OBJECT_OK);
    CHECK(material.decoded);
    CHECK(material.schema.node_count ==
          (int)MATERIAL_OBJECT_LAYOUT_NODE_COUNT);
    CHECK(material.path_id == fixture.object.path_id);
    CHECK(material.target_platform == 19U);
    CHECK(material.name.size == 6U);
    CHECK(material.name.bytes[4] == 0xcfU && material.name.bytes[5] == 0x80U);
    CHECK(material.shader.file_id == -2);
    CHECK(material.shader.path_id == INT64_C(0x102030405060708));
    CHECK(material.valid_keyword_count == 1U);
    CHECK(view_equals(material.valid_keywords[0], "FOO"));
    CHECK(material.invalid_keyword_count == 1U);
    CHECK(view_equals(material.invalid_keywords[0], "BAR"));
    CHECK(material.lightmap_flags == UINT32_C(0x80000005));
    CHECK(material.enable_instancing_variants);
    CHECK(!material.double_sided_gi);
    CHECK(material.custom_render_queue == -1);
    CHECK(material.string_tag_count == 1U);
    CHECK(view_equals(material.string_tags[0].key, "RenderType"));
    CHECK(view_equals(material.string_tags[0].value, "Opaque"));
    CHECK(material.disabled_shader_pass_count == 1U);
    CHECK(view_equals(material.disabled_shader_passes[0], "ShadowCaster"));

    CHECK(material.texture_property_count == 1U);
    CHECK(view_equals(material.texture_properties[0].name, "_MainTex"));
    CHECK(material.texture_properties[0].texture.file_id == 2);
    CHECK(material.texture_properties[0].texture.path_id == -99);
    CHECK(material.texture_properties[0].scale.x.bits ==
          UINT32_C(0x80000000));
    CHECK(material.texture_properties[0].scale.y.bits ==
          UINT32_C(0x7f800000));
    CHECK(material.texture_properties[0].offset.x.bits ==
          UINT32_C(0x7fc01234));
    CHECK(material.texture_properties[0].offset.y.bits ==
          UINT32_C(0xff800000));
    CHECK(material.int_property_count == 1U);
    CHECK(material.int_properties[0].value == -7);
    CHECK(material.float_property_count == 1U);
    CHECK(material.float_properties[0].value.bits ==
          UINT32_C(0x7fa12345));
    CHECK(material.color_property_count == 1U);
    CHECK(material.color_properties[0].value.r.bits ==
          UINT32_C(0x3f800000));
    CHECK(material.color_properties[0].value.g.bits ==
          UINT32_C(0xbf800000));
    CHECK(material.color_properties[0].value.b.bits == UINT32_C(1));
    CHECK(material.color_properties[0].value.a.bits ==
          UINT32_C(0x80000000));
    CHECK(material.texture_stack_count == 1U);
    CHECK(view_equals(material.texture_stacks[0].group_name, "Layer"));
    CHECK(view_equals(material.texture_stacks[0].item_name, "Item"));

    /* Truncation at every byte boundary fails closed and cannot replace a
     * previously decoded destination. */
    uint32_t complete_size = fixture.object.byte_size;
    for (uint32_t prefix = 1U; prefix < complete_size; ++prefix) {
        fixture.object.byte_size = prefix;
        CHECK(material_object_decode(&material, &fixture.file,
                                     &fixture.object) != MATERIAL_OBJECT_OK);
        CHECK(material.decoded && material.path_id == INT64_C(123456789));
    }
    fixture.object.byte_size = complete_size;

    /* Generic TypeTree parsing consumes the exact object span. */
    fixture.bytes[fixture.size] = 0U;
    fixture.object.byte_size = complete_size + 1U;
    fixture.file.file_size = fixture.size + 1U;
    fixture.file.raw_size = fixture.size + 1U;
    CHECK(material_object_decode(&material, &fixture.file, &fixture.object) ==
          MATERIAL_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED);
    fixture.object.byte_size = complete_size;
    fixture.file.file_size = fixture.size;
    fixture.file.raw_size = fixture.size;

    /* Counts are bounded by the remaining payload before allocation. */
    uint8_t saved_count[4];
    memcpy(saved_count, fixture.bytes + fixture.string_tag_count_offset, 4U);
    store_le32(fixture.bytes + fixture.string_tag_count_offset,
               UINT32_C(0x7fffffff));
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) != MATERIAL_OBJECT_OK);
    memcpy(fixture.bytes + fixture.string_tag_count_offset, saved_count, 4U);

    uint8_t saved_name = fixture.bytes[fixture.material_name_data_offset];
    fixture.bytes[fixture.material_name_data_offset] = 0U;
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) ==
          MATERIAL_OBJECT_STRING_INVALID);
    fixture.bytes[fixture.material_name_data_offset] = 0xc0U;
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) ==
          MATERIAL_OBJECT_STRING_INVALID);
    fixture.bytes[fixture.material_name_data_offset] = saved_name;

    /* A schema-valid mutation still fails the pinned semantic profile. */
    fixture.type.nodes[0].meta_flags ^= 1U;
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) ==
          MATERIAL_OBJECT_TYPE_IDENTITY_UNSUPPORTED);
    fixture.type.nodes[0].meta_flags ^= 1U;

    char* saved_version = fixture.file.unity_version;
    fixture.file.unity_version = (char*)"2021.3.29f1";
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) == MATERIAL_OBJECT_OK);
    fixture.file.unity_version = (char*)"2021.3.34f1";
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) ==
          MATERIAL_OBJECT_UNSUPPORTED_UNITY_VERSION);
    fixture.file.unity_version = saved_version;

    /* The successful model owns its cloned schema/value tree and survives
     * destruction of every source-file view. */
    fixture_dispose(&fixture);
    CHECK(material.decoded);
    CHECK(view_equals(material.texture_properties[0].name, "_MainTex"));
    CHECK(material.float_properties[0].value.bits ==
          UINT32_C(0x7fa12345));
    material_object_dispose(&material);

    CHECK(fixture_init(&fixture, true));
    material_object_init(&material);
    CHECK(material_object_decode(&material, &fixture.file,
                                 &fixture.object) ==
          MATERIAL_OBJECT_DUPLICATE_KEY);
    CHECK(!material.decoded);
    material_object_dispose(&material);
    fixture_dispose(&fixture);

    CHECK(strcmp(material_object_status_name(MATERIAL_OBJECT_OK), "ok") == 0);
    CHECK(strcmp(material_object_status_name(MATERIAL_OBJECT_DUPLICATE_KEY),
                 "duplicate-key") == 0);
    CHECK(atomic_load_explicit(&g_allocated_bytes, memory_order_relaxed) ==
          baseline_bytes);
    CHECK(atomic_load_explicit(&g_allocations_count, memory_order_relaxed) ==
          baseline_allocations);
    return 0;
}
