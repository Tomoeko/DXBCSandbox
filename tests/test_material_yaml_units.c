#include "common/stream.h"
#include "common/string_builder.h"
#include "common/unity_asset_guid.h"
#include "translation/material_yaml_emitter.h"
#include "translation/unity_yaml.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int test_guid_derivation(void) {
    static const uint8_t identity[] = {
        'a', 's', 's', 'e', 't', 0, 'i', 'd', 'e', 'n', 't', 'i', 't', 'y'
    };
    char shader_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    char material_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    CHECK(unity_asset_guid_derive(
        UNITY_ASSET_GUID_DOMAIN_SHADER, identity, sizeof(identity),
        shader_guid));
    CHECK(unity_asset_guid_derive(
        UNITY_ASSET_GUID_DOMAIN_MATERIAL, identity, sizeof(identity),
        material_guid));
    CHECK(strcmp(shader_guid, "5c7fc0f9d88e746e6035a9da58f48873") == 0);
    CHECK(strcmp(material_guid, "10c82d85afd81b0a7de80f4c1828ac95") == 0);
    CHECK(strcmp(shader_guid, material_guid) != 0);
    CHECK(unity_asset_guid_is_valid(shader_guid));
    CHECK(!unity_asset_guid_is_valid("5C7fc0f9d88e746e6035a9da58f48873"));
    CHECK(!unity_asset_guid_is_valid("5c7fc0f9d88e746e6035a9da58f4887"));
    CHECK(!unity_asset_guid_is_valid(
        "5c7fc0f9d88e746e6035a9da58f488730"));

    char left[UNITY_ASSET_GUID_TEXT_CAPACITY];
    char right[UNITY_ASSET_GUID_TEXT_CAPACITY];
    CHECK(unity_asset_guid_derive("ab", "c", 1U, left));
    CHECK(unity_asset_guid_derive("a", "bc", 2U, right));
    CHECK(strcmp(left, "1d39cbf5dafc0047690b8cfad59ec93c") == 0);
    CHECK(strcmp(right, "a8f919638955898843c72c5ade06efab") == 0);
    CHECK(strcmp(left, right) != 0);
    CHECK(!unity_asset_guid_derive("", identity, sizeof(identity), left));
    CHECK(!unity_asset_guid_derive("bad domain", identity,
                                   sizeof(identity), left));
    CHECK(!unity_asset_guid_derive("domain", NULL, 1U, left));
    CHECK(!unity_asset_guid_derive("domain", NULL, 0U, NULL));

    static const uint8_t arbitrary_serialized[16] = {
        0x10U, 0x32U, 0x54U, 0x76U, 0x98U, 0xbaU, 0xdcU, 0xfeU,
        0x10U, 0x32U, 0x54U, 0x76U, 0x98U, 0xbaU, 0xdcU, 0xfeU,
    };
    char decoded[UNITY_ASSET_GUID_TEXT_CAPACITY];
    CHECK(unity_asset_guid_from_serialized_bytes(arbitrary_serialized,
                                                  decoded));
    CHECK(strcmp(decoded, "0123456789abcdef0123456789abcdef") == 0);
    static const uint8_t builtin_extra_serialized[16] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0x0fU, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    };
    CHECK(unity_asset_guid_from_serialized_bytes(builtin_extra_serialized,
                                                  decoded));
    CHECK(strcmp(decoded, "0000000000000000f000000000000000") == 0);
    CHECK(!unity_asset_guid_from_serialized_bytes(NULL, decoded));
    CHECK(!unity_asset_guid_from_serialized_bytes(arbitrary_serialized,
                                                  NULL));
    return 0;
}

static int test_utf8_and_escaping(void) {
    static const uint8_t valid[] = {
        'A', 0, 0xc3, 0xa9, 0xe9, 0x9b, 0xaa,
        0xf0, 0x9f, 0x98, 0x80
    };
    static const uint8_t overlong[] = {0xc0, 0x80};
    static const uint8_t surrogate[] = {0xed, 0xa0, 0x80};
    static const uint8_t too_large[] = {0xf4, 0x90, 0x80, 0x80};
    static const uint8_t truncated[] = {0xe2, 0x82};
    CHECK(unity_yaml_utf8_is_valid(valid, sizeof(valid)));
    CHECK(!unity_yaml_utf8_is_valid(overlong, sizeof(overlong)));
    CHECK(!unity_yaml_utf8_is_valid(surrogate, sizeof(surrogate)));
    CHECK(!unity_yaml_utf8_is_valid(too_large, sizeof(too_large)));
    CHECK(!unity_yaml_utf8_is_valid(truncated, sizeof(truncated)));
    CHECK(!unity_yaml_utf8_is_valid(NULL, 1U));
    CHECK(unity_yaml_utf8_is_valid(NULL, 0U));

    static const uint8_t escaped_input[] = {
        'A', '"', '\n', '\\', 0, 0xc2, 0x85,
        0xe2, 0x80, 0xa8, 0xe9, 0x9b, 0xaa
    };
    StringBuilder output;
    sb_init(&output);
    CHECK(unity_yaml_append_quoted_n(
        &output, escaped_input, sizeof(escaped_input)) == UNITY_YAML_OK);
    CHECK(strcmp(output.buf,
        "\"A\\\"\\n\\\\\\0\\u0085\\u2028\xe9\x9b\xaa\"") == 0);
    sb_free(&output);

    sb_init(&output);
    CHECK(unity_yaml_append_quoted_n(
        &output, overlong, sizeof(overlong)) == UNITY_YAML_INVALID_UTF8);
    CHECK(output.buf == NULL && output.len == 0U);
    sb_free(&output);
    return 0;
}

static uint32_t float_to_bits(float value) {
    uint32_t bits = 0U;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int test_float_formatting(void) {
    char text[UNITY_YAML_FLOAT32_TEXT_CAPACITY];
    CHECK(unity_yaml_format_float32(0x00000000U, text));
    CHECK(strcmp(text, "0") == 0);
    CHECK(unity_yaml_format_float32(0x80000000U, text));
    CHECK(strcmp(text, "-0") == 0);
    CHECK(unity_yaml_format_float32(0x3f800000U, text));
    CHECK(strcmp(text, "1") == 0);
    CHECK(unity_yaml_format_float32(0x3dcccccdU, text));
    CHECK(strcmp(text, "0.100000001490116119384765625") == 0);
    CHECK(unity_yaml_format_float32(0x7f800000U, text));
    CHECK(strcmp(text, "Infinity") == 0);
    CHECK(unity_yaml_format_float32(0xff800000U, text));
    CHECK(strcmp(text, "-Infinity") == 0);
    CHECK(unity_yaml_format_float32(0x7fc12345U, text));
    CHECK(strcmp(text, "NaN") == 0);

    static const uint32_t edge_values[] = {
        0x00000001U, 0x80000001U,
        0x007fffffU, 0x807fffffU,
        0x00800000U, 0x80800000U,
        0x7f7fffffU, 0xff7fffffU,
    };
    for (size_t index = 0U;
         index < sizeof(edge_values) / sizeof(edge_values[0]); ++index) {
        CHECK(unity_yaml_format_float32(edge_values[index], text));
        char* end = NULL;
        float parsed = strtof(text, &end);
        CHECK(end && *end == '\0');
        CHECK(float_to_bits(parsed) == edge_values[index]);
    }

    uint32_t state = 0x12345678U;
    for (unsigned index = 0U; index < 20000U; ++index) {
        state = state * 1664525U + 1013904223U;
        CHECK(unity_yaml_format_float32(state, text));
        uint32_t exponent = (state >> 23U) & 0xffU;
        uint32_t fraction = state & 0x7fffffU;
        char* end = NULL;
        float parsed = strtof(text, &end);
        CHECK(end && *end == '\0');
        if (exponent == 0xffU && fraction != 0U) {
            CHECK(isnan(parsed));
        } else {
            CHECK(float_to_bits(parsed) == state);
        }
    }
    CHECK(!unity_yaml_format_float32(0U, NULL));
    return 0;
}

static void init_reference(UnityMaterialYamlReference* reference,
                           bool is_null, int64_t file_id,
                           const char* guid, int32_t type) {
    reference->is_null = is_null;
    reference->file_id = file_id;
    reference->guid = guid;
    reference->type = type;
}

static UnityMaterialYamlString yaml_string(const char* text) {
    UnityMaterialYamlString result;
    result.bytes = (const uint8_t*)text;
    result.size = strlen(text);
    return result;
}

static void build_golden_document(UnityMaterialYamlDocument* document,
                                  UnityMaterialYamlTextureProperty textures[2],
                                  UnityMaterialYamlFloatProperty floats[2],
                                  UnityMaterialYamlColorProperty colors[1]) {
    static const UnityMaterialYamlString valid_keywords[] = {
        {(const uint8_t*)"Z_KEYWORD", 9U},
        {(const uint8_t*)"A_KEYWORD", 9U},
    };
    static const UnityMaterialYamlString invalid_keywords[] = {
        {(const uint8_t*)"BAD_KEYWORD", 11U},
    };
    static const UnityMaterialYamlString disabled_passes[] = {
        {(const uint8_t*)"ShadowCaster", 12U},
    };
    static const UnityMaterialYamlStringPair tags[] = {
        {
            {(const uint8_t*)"RenderType", 10U},
            {(const uint8_t*)"Opaque", 6U},
        },
        {
            {(const uint8_t*)"line\nkey", 8U},
            {(const uint8_t*)"value: #literal", 15U},
        },
    };
    static const UnityMaterialYamlIntProperty ints[] = {
        {{(const uint8_t*)"_Mode", 5U}, -2},
    };
    static const UnityMaterialYamlBuildTextureStack stacks[] = {
        {
            {(const uint8_t*)"Group", 5U},
            {(const uint8_t*)"Item", 4U},
        },
    };

    memset(document, 0, sizeof(*document));
    document->name = yaml_string("Mat \"\xe9\x9b\xaa\"\n");
    init_reference(&document->shader, false, 4800000,
                   "11111111111111111111111111111111", 3);
    document->valid_keywords = valid_keywords;
    document->valid_keyword_count = 2U;
    document->invalid_keywords = invalid_keywords;
    document->invalid_keyword_count = 1U;
    document->lightmap_flags = 4U;
    document->enable_instancing_variants = true;
    document->double_sided_gi = false;
    document->custom_render_queue = -1;
    document->string_tags = tags;
    document->string_tag_count = 2U;
    document->disabled_shader_passes = disabled_passes;
    document->disabled_shader_pass_count = 1U;

    memset(textures, 0, sizeof(*textures) * 2U);
    textures[0].name = yaml_string("_MainTex");
    init_reference(&textures[0].texture, true, 0, NULL, 0);
    textures[0].scale_x_bits = 0x3f800000U;
    textures[0].scale_y_bits = 0x3f800000U;
    textures[0].offset_x_bits = 0x00000000U;
    textures[0].offset_y_bits = 0x80000000U;
    textures[1].name = yaml_string("_Detail");
    init_reference(&textures[1].texture, false, 2800000,
                   "22222222222222222222222222222222", 3);
    textures[1].scale_x_bits = 0x3dcccccdU;
    textures[1].scale_y_bits = 0x7f800000U;
    textures[1].offset_x_bits = 0xff800000U;
    textures[1].offset_y_bits = 0x7fc12345U;
    document->textures = textures;
    document->texture_count = 2U;
    document->ints = ints;
    document->int_count = 1U;

    floats[0].name = yaml_string("_Cutoff");
    floats[0].value_bits = 0x3dcccccdU;
    floats[1].name = yaml_string("_NegZero");
    floats[1].value_bits = 0x80000000U;
    document->floats = floats;
    document->float_count = 2U;

    colors[0].name = yaml_string("_Color");
    colors[0].red_bits = 0x3f800000U;
    colors[0].green_bits = 0x3f000000U;
    colors[0].blue_bits = 0x80000000U;
    colors[0].alpha_bits = 0x7fc00000U;
    document->colors = colors;
    document->color_count = 1U;
    document->build_texture_stacks = stacks;
    document->build_texture_stack_count = 1U;
}

static int test_material_golden_and_repeat(void) {
    static const char expected[] =
        "%YAML 1.1\n"
        "%TAG !u! tag:unity3d.com,2011:\n"
        "--- !u!21 &2100000\n"
        "Material:\n"
        "  serializedVersion: 8\n"
        "  m_ObjectHideFlags: 0\n"
        "  m_CorrespondingSourceObject: {fileID: 0}\n"
        "  m_PrefabInstance: {fileID: 0}\n"
        "  m_PrefabAsset: {fileID: 0}\n"
        "  m_Name: \"Mat \\\"\xe9\x9b\xaa\\\"\\n\"\n"
        "  m_Shader: {fileID: 4800000, guid: 11111111111111111111111111111111, type: 3}\n"
        "  m_ValidKeywords:\n"
        "  - \"Z_KEYWORD\"\n"
        "  - \"A_KEYWORD\"\n"
        "  m_InvalidKeywords:\n"
        "  - \"BAD_KEYWORD\"\n"
        "  m_LightmapFlags: 4\n"
        "  m_EnableInstancingVariants: 1\n"
        "  m_DoubleSidedGI: 0\n"
        "  m_CustomRenderQueue: -1\n"
        "  stringTagMap:\n"
        "    \"RenderType\": \"Opaque\"\n"
        "    \"line\\nkey\": \"value: #literal\"\n"
        "  disabledShaderPasses:\n"
        "  - \"ShadowCaster\"\n"
        "  m_SavedProperties:\n"
        "    serializedVersion: 3\n"
        "    m_TexEnvs:\n"
        "    - \"_MainTex\":\n"
        "        m_Texture: {fileID: 0}\n"
        "        m_Scale: {x: 1, y: 1}\n"
        "        m_Offset: {x: 0, y: -0}\n"
        "    - \"_Detail\":\n"
        "        m_Texture: {fileID: 2800000, guid: 22222222222222222222222222222222, type: 3}\n"
        "        m_Scale: {x: 0.100000001490116119384765625, y: Infinity}\n"
        "        m_Offset: {x: -Infinity, y: NaN}\n"
        "    m_Ints:\n"
        "    - \"_Mode\": -2\n"
        "    m_Floats:\n"
        "    - \"_Cutoff\": 0.100000001490116119384765625\n"
        "    - \"_NegZero\": -0\n"
        "    m_Colors:\n"
        "    - \"_Color\": {r: 1, g: 0.5, b: -0, a: NaN}\n"
        "  m_BuildTextureStacks:\n"
        "  - groupName: \"Group\"\n"
        "    itemName: \"Item\"\n";

    UnityMaterialYamlDocument document;
    UnityMaterialYamlTextureProperty textures[2];
    UnityMaterialYamlFloatProperty floats[2];
    UnityMaterialYamlColorProperty colors[1];
    build_golden_document(&document, textures, floats, colors);

    StringBuilder first;
    StringBuilder second;
    sb_init(&first);
    sb_init(&second);
    CHECK(unity_material_yaml_emit(&document, &first) ==
          UNITY_MATERIAL_YAML_OK);
    CHECK(unity_material_yaml_emit(&document, &second) ==
          UNITY_MATERIAL_YAML_OK);
    if (strcmp(first.buf, expected) != 0) {
        fprintf(stderr, "material golden mismatch:\n%s", first.buf);
        sb_free(&first);
        sb_free(&second);
        return 1;
    }
    CHECK(first.len == strlen(expected));
    CHECK(second.len == first.len);
    CHECK(memcmp(first.buf, second.buf, first.len + 1U) == 0);
    sb_free(&first);
    sb_free(&second);
    return 0;
}

static int test_fail_closed_validation(void) {
    UnityMaterialYamlDocument document;
    UnityMaterialYamlTextureProperty textures[2];
    UnityMaterialYamlFloatProperty floats[2];
    UnityMaterialYamlColorProperty colors[1];
    build_golden_document(&document, textures, floats, colors);

    StringBuilder output;
    sb_init(&output);
    sb_append(&output, "unchanged");
    textures[1].texture.guid = NULL;
    CHECK(unity_material_yaml_emit(&document, &output) ==
          UNITY_MATERIAL_YAML_UNRESOLVED_REFERENCE);
    CHECK(strcmp(output.buf, "unchanged") == 0);
    textures[1].texture.guid = "22222222222222222222222222222222";

    textures[1].texture.is_null = true;
    CHECK(unity_material_yaml_emit(&document, &output) ==
          UNITY_MATERIAL_YAML_INVALID_ARGUMENT);
    textures[1].texture.is_null = false;

    document.shader.is_null = true;
    document.shader.file_id = 0;
    document.shader.guid = NULL;
    document.shader.type = 0;
    CHECK(unity_material_yaml_emit(&document, &output) ==
          UNITY_MATERIAL_YAML_UNRESOLVED_REFERENCE);
    init_reference(&document.shader, false, 4800000,
                   "11111111111111111111111111111111", 3);

    floats[1].name = floats[0].name;
    CHECK(unity_material_yaml_emit(&document, &output) ==
          UNITY_MATERIAL_YAML_DUPLICATE_KEY);
    floats[1].name = yaml_string("_NegZero");

    static const char invalid_utf8[] = {(char)0xc3, '(', '\0'};
    document.name.bytes = (const uint8_t*)invalid_utf8;
    document.name.size = 2U;
    CHECK(unity_material_yaml_emit(&document, &output) ==
          UNITY_MATERIAL_YAML_INVALID_UTF8);
    CHECK(strcmp(output.buf, "unchanged") == 0);
    sb_free(&output);
    return 0;
}

static int test_meta_goldens(void) {
    static const char shader_expected[] =
        "fileFormatVersion: 2\n"
        "guid: 0123456789abcdef0123456789abcdef\n"
        "ShaderImporter:\n"
        "  externalObjects: {}\n"
        "  defaultTextures: []\n"
        "  nonModifiableTextures: []\n"
        "  preprocessorOverride: 0\n"
        "  userData: \n"
        "  assetBundleName: \n"
        "  assetBundleVariant: \n";
    static const char material_expected[] =
        "fileFormatVersion: 2\n"
        "guid: 0123456789abcdef0123456789abcdef\n"
        "NativeFormatImporter:\n"
        "  externalObjects: {}\n"
        "  mainObjectFileID: 2100000\n"
        "  userData: \n"
        "  assetBundleName: \n"
        "  assetBundleVariant: \n";
    StringBuilder output;
    sb_init(&output);
    CHECK(unity_shader_importer_meta_emit(
        "0123456789abcdef0123456789abcdef", &output) ==
        UNITY_MATERIAL_YAML_OK);
    CHECK(strcmp(output.buf, shader_expected) == 0);
    sb_clear(&output);
    CHECK(unity_material_native_meta_emit(
        "0123456789abcdef0123456789abcdef", &output) ==
        UNITY_MATERIAL_YAML_OK);
    CHECK(strcmp(output.buf, material_expected) == 0);
    sb_clear(&output);
    CHECK(unity_shader_importer_meta_emit(
        "0123456789ABCDEF0123456789abcdef", &output) ==
        UNITY_MATERIAL_YAML_INVALID_GUID);
    CHECK(output.len == 0U);
    sb_free(&output);
    return 0;
}

int main(void) {
    if (test_guid_derivation() != 0) return 1;
    if (test_utf8_and_escaping() != 0) return 1;
    if (test_float_formatting() != 0) return 1;
    if (test_material_golden_and_repeat() != 0) return 1;
    if (test_fail_closed_validation() != 0) return 1;
    if (test_meta_goldens() != 0) return 1;
    CHECK(g_allocations_count == 0U);
    CHECK(g_allocated_bytes == 0U);
    puts("material YAML unit tests passed");
    return 0;
}
