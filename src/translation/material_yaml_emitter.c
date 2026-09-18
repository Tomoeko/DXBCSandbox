// SPDX-License-Identifier: GPL-3.0-only

#include "translation/material_yaml_emitter.h"

#include "common/unity_asset_guid.h"
#include "translation/unity_yaml.h"

#include <inttypes.h>
#include <string.h>

const char* unity_material_yaml_status_name(UnityMaterialYamlStatus status) {
    switch (status) {
        case UNITY_MATERIAL_YAML_OK: return "ok";
        case UNITY_MATERIAL_YAML_INVALID_ARGUMENT: return "invalid_argument";
        case UNITY_MATERIAL_YAML_INVALID_UTF8: return "invalid_utf8";
        case UNITY_MATERIAL_YAML_INVALID_GUID: return "invalid_guid";
        case UNITY_MATERIAL_YAML_UNRESOLVED_REFERENCE:
            return "unresolved_reference";
        case UNITY_MATERIAL_YAML_DUPLICATE_KEY: return "duplicate_key";
        case UNITY_MATERIAL_YAML_OUTPUT_FAILED: return "output_failed";
    }
    return "unknown";
}

static UnityMaterialYamlStatus validate_text(UnityMaterialYamlString text,
                                             bool require_nonempty) {
    if ((!text.bytes && text.size != 0U) ||
        (require_nonempty && text.size == 0U)) {
        return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    }
    return unity_yaml_utf8_is_valid(text.bytes, text.size)
        ? UNITY_MATERIAL_YAML_OK
        : UNITY_MATERIAL_YAML_INVALID_UTF8;
}

static bool text_equal(UnityMaterialYamlString left,
                       UnityMaterialYamlString right) {
    return left.size == right.size &&
        (left.size == 0U || memcmp(left.bytes, right.bytes, left.size) == 0);
}

static UnityMaterialYamlStatus validate_reference(
    const UnityMaterialYamlReference* reference,
    bool permit_null) {
    if (!reference) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    if (reference->is_null) {
        if (!permit_null) return UNITY_MATERIAL_YAML_UNRESOLVED_REFERENCE;
        if (reference->file_id != 0 || reference->type != 0 ||
            (reference->guid && reference->guid[0] != '\0')) {
            return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
        }
        return UNITY_MATERIAL_YAML_OK;
    }
    if (reference->file_id == 0 || !reference->guid) {
        return UNITY_MATERIAL_YAML_UNRESOLVED_REFERENCE;
    }
    if (!unity_asset_guid_is_valid(reference->guid)) {
        return UNITY_MATERIAL_YAML_INVALID_GUID;
    }
    if (reference->type < 0 || reference->type > 3) {
        return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_string_vector(
    const UnityMaterialYamlString* values, size_t count,
    bool reject_duplicates) {
    if (count != 0U && !values) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    for (size_t index = 0U; index < count; ++index) {
        UnityMaterialYamlStatus status = validate_text(values[index], true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        if (reject_duplicates) {
            for (size_t prior = 0U; prior < index; ++prior) {
                if (text_equal(values[prior], values[index])) {
                    return UNITY_MATERIAL_YAML_DUPLICATE_KEY;
                }
            }
        }
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_string_pairs(
    const UnityMaterialYamlStringPair* pairs, size_t count) {
    if (count != 0U && !pairs) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    for (size_t index = 0U; index < count; ++index) {
        UnityMaterialYamlStatus status = validate_text(pairs[index].key, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        status = validate_text(pairs[index].value, false);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        for (size_t prior = 0U; prior < index; ++prior) {
            if (text_equal(pairs[prior].key, pairs[index].key)) {
                return UNITY_MATERIAL_YAML_DUPLICATE_KEY;
            }
        }
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_texture_properties(
    const UnityMaterialYamlTextureProperty* values, size_t count) {
    if (count != 0U && !values) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    for (size_t index = 0U; index < count; ++index) {
        UnityMaterialYamlStatus status = validate_text(values[index].name, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        status = validate_reference(&values[index].texture, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        for (size_t prior = 0U; prior < index; ++prior) {
            if (text_equal(values[prior].name, values[index].name)) {
                return UNITY_MATERIAL_YAML_DUPLICATE_KEY;
            }
        }
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_int_properties(
    const UnityMaterialYamlIntProperty* values, size_t count) {
    if (count != 0U && !values) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    for (size_t index = 0U; index < count; ++index) {
        UnityMaterialYamlStatus status = validate_text(values[index].name, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        for (size_t prior = 0U; prior < index; ++prior) {
            if (text_equal(values[prior].name, values[index].name)) {
                return UNITY_MATERIAL_YAML_DUPLICATE_KEY;
            }
        }
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_float_properties(
    const UnityMaterialYamlFloatProperty* values, size_t count) {
    if (count != 0U && !values) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    for (size_t index = 0U; index < count; ++index) {
        UnityMaterialYamlStatus status = validate_text(values[index].name, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        for (size_t prior = 0U; prior < index; ++prior) {
            if (text_equal(values[prior].name, values[index].name)) {
                return UNITY_MATERIAL_YAML_DUPLICATE_KEY;
            }
        }
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_color_properties(
    const UnityMaterialYamlColorProperty* values, size_t count) {
    if (count != 0U && !values) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    for (size_t index = 0U; index < count; ++index) {
        UnityMaterialYamlStatus status = validate_text(values[index].name, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        for (size_t prior = 0U; prior < index; ++prior) {
            if (text_equal(values[prior].name, values[index].name)) {
                return UNITY_MATERIAL_YAML_DUPLICATE_KEY;
            }
        }
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_build_texture_stacks(
    const UnityMaterialYamlBuildTextureStack* values, size_t count) {
    if (count != 0U && !values) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    for (size_t index = 0U; index < count; ++index) {
        UnityMaterialYamlStatus status =
            validate_text(values[index].group_name, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
        status = validate_text(values[index].item_name, true);
        if (status != UNITY_MATERIAL_YAML_OK) return status;
    }
    return UNITY_MATERIAL_YAML_OK;
}

static UnityMaterialYamlStatus validate_document(
    const UnityMaterialYamlDocument* document) {
    if (!document) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    UnityMaterialYamlStatus status = validate_text(document->name, false);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_reference(&document->shader, false);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_string_vector(document->valid_keywords,
                                    document->valid_keyword_count, true);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_string_vector(document->invalid_keywords,
                                    document->invalid_keyword_count, true);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_string_pairs(document->string_tags,
                                   document->string_tag_count);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_string_vector(document->disabled_shader_passes,
                                    document->disabled_shader_pass_count,
                                    false);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_texture_properties(document->textures,
                                         document->texture_count);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_int_properties(document->ints, document->int_count);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_float_properties(document->floats,
                                       document->float_count);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    status = validate_color_properties(document->colors,
                                       document->color_count);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    return validate_build_texture_stacks(document->build_texture_stacks,
                                         document->build_texture_stack_count);
}

static void append_quoted_unchecked(StringBuilder* output,
                                    UnityMaterialYamlString text) {
    (void)unity_yaml_append_quoted_n(output, text.bytes, text.size);
}

static void append_float_unchecked(StringBuilder* output, uint32_t bits) {
    (void)unity_yaml_append_float32(output, bits);
}

static void append_reference(StringBuilder* output,
                             const UnityMaterialYamlReference* reference) {
    if (reference->is_null) {
        sb_append(output, "{fileID: 0}");
        return;
    }
    sb_appendf(output, "{fileID: %" PRId64 ", guid: %s, type: %d}",
               reference->file_id, reference->guid, reference->type);
}

static void append_string_sequence(StringBuilder* output,
                                   const char* field_indent,
                                   const char* field_name,
                                   const UnityMaterialYamlString* values,
                                   size_t count) {
    sb_append(output, field_indent);
    sb_append(output, field_name);
    if (count == 0U) {
        sb_append(output, ": []\n");
        return;
    }
    sb_append(output, ":\n");
    for (size_t index = 0U; index < count; ++index) {
        sb_append(output, field_indent);
        sb_append(output, "- ");
        append_quoted_unchecked(output, values[index]);
        sb_append_char(output, '\n');
    }
}

UnityMaterialYamlStatus unity_material_yaml_emit(
    const UnityMaterialYamlDocument* document,
    StringBuilder* output) {
    if (!output) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    UnityMaterialYamlStatus status = validate_document(document);
    if (status != UNITY_MATERIAL_YAML_OK) return status;

    sb_append(output,
        "%YAML 1.1\n"
        "%TAG !u! tag:unity3d.com,2011:\n"
        "--- !u!21 &2100000\n"
        "Material:\n"
        "  serializedVersion: 8\n"
        /* Player Material v8 has no ObjectHideFlags authority.  A newly
         * generated native asset uses Unity's deterministic importer default
         * along with null prefab provenance. */
        "  m_ObjectHideFlags: 0\n"
        "  m_CorrespondingSourceObject: {fileID: 0}\n"
        "  m_PrefabInstance: {fileID: 0}\n"
        "  m_PrefabAsset: {fileID: 0}\n"
        "  m_Name: ");
    append_quoted_unchecked(output, document->name);
    sb_append(output, "\n  m_Shader: ");
    append_reference(output, &document->shader);
    sb_append_char(output, '\n');
    append_string_sequence(output, "  ", "m_ValidKeywords",
                           document->valid_keywords,
                           document->valid_keyword_count);
    append_string_sequence(output, "  ", "m_InvalidKeywords",
                           document->invalid_keywords,
                           document->invalid_keyword_count);
    sb_appendf(output,
               "  m_LightmapFlags: %" PRIu32 "\n"
               "  m_EnableInstancingVariants: %d\n"
               "  m_DoubleSidedGI: %d\n"
               "  m_CustomRenderQueue: %d\n",
               document->lightmap_flags,
               document->enable_instancing_variants ? 1 : 0,
               document->double_sided_gi ? 1 : 0,
               document->custom_render_queue);

    if (document->string_tag_count == 0U) {
        sb_append(output, "  stringTagMap: {}\n");
    } else {
        sb_append(output, "  stringTagMap:\n");
        for (size_t index = 0U; index < document->string_tag_count; ++index) {
            sb_append(output, "    ");
            append_quoted_unchecked(output, document->string_tags[index].key);
            sb_append(output, ": ");
            append_quoted_unchecked(output,
                                    document->string_tags[index].value);
            sb_append_char(output, '\n');
        }
    }
    append_string_sequence(output, "  ", "disabledShaderPasses",
                           document->disabled_shader_passes,
                           document->disabled_shader_pass_count);
    sb_append(output,
              "  m_SavedProperties:\n"
              "    serializedVersion: 3\n");

    if (document->texture_count == 0U) {
        sb_append(output, "    m_TexEnvs: []\n");
    } else {
        sb_append(output, "    m_TexEnvs:\n");
        for (size_t index = 0U; index < document->texture_count; ++index) {
            const UnityMaterialYamlTextureProperty* property =
                &document->textures[index];
            sb_append(output, "    - ");
            append_quoted_unchecked(output, property->name);
            sb_append(output, ":\n        m_Texture: ");
            append_reference(output, &property->texture);
            sb_append(output, "\n        m_Scale: {x: ");
            append_float_unchecked(output, property->scale_x_bits);
            sb_append(output, ", y: ");
            append_float_unchecked(output, property->scale_y_bits);
            sb_append(output, "}\n        m_Offset: {x: ");
            append_float_unchecked(output, property->offset_x_bits);
            sb_append(output, ", y: ");
            append_float_unchecked(output, property->offset_y_bits);
            sb_append(output, "}\n");
        }
    }

    if (document->int_count == 0U) {
        sb_append(output, "    m_Ints: []\n");
    } else {
        sb_append(output, "    m_Ints:\n");
        for (size_t index = 0U; index < document->int_count; ++index) {
            sb_append(output, "    - ");
            append_quoted_unchecked(output, document->ints[index].name);
            sb_appendf(output, ": %d\n", document->ints[index].value);
        }
    }

    if (document->float_count == 0U) {
        sb_append(output, "    m_Floats: []\n");
    } else {
        sb_append(output, "    m_Floats:\n");
        for (size_t index = 0U; index < document->float_count; ++index) {
            sb_append(output, "    - ");
            append_quoted_unchecked(output, document->floats[index].name);
            sb_append(output, ": ");
            append_float_unchecked(output,
                                   document->floats[index].value_bits);
            sb_append_char(output, '\n');
        }
    }

    if (document->color_count == 0U) {
        sb_append(output, "    m_Colors: []\n");
    } else {
        sb_append(output, "    m_Colors:\n");
        for (size_t index = 0U; index < document->color_count; ++index) {
            const UnityMaterialYamlColorProperty* property =
                &document->colors[index];
            sb_append(output, "    - ");
            append_quoted_unchecked(output, property->name);
            sb_append(output, ": {r: ");
            append_float_unchecked(output, property->red_bits);
            sb_append(output, ", g: ");
            append_float_unchecked(output, property->green_bits);
            sb_append(output, ", b: ");
            append_float_unchecked(output, property->blue_bits);
            sb_append(output, ", a: ");
            append_float_unchecked(output, property->alpha_bits);
            sb_append(output, "}\n");
        }
    }

    if (document->build_texture_stack_count == 0U) {
        sb_append(output, "  m_BuildTextureStacks: []\n");
    } else {
        sb_append(output, "  m_BuildTextureStacks:\n");
        for (size_t index = 0U;
             index < document->build_texture_stack_count; ++index) {
            sb_append(output, "  - groupName: ");
            append_quoted_unchecked(
                output, document->build_texture_stacks[index].group_name);
            sb_append(output, "\n    itemName: ");
            append_quoted_unchecked(
                output, document->build_texture_stacks[index].item_name);
            sb_append_char(output, '\n');
        }
    }
    return sb_ok(output)
        ? UNITY_MATERIAL_YAML_OK
        : UNITY_MATERIAL_YAML_OUTPUT_FAILED;
}

static UnityMaterialYamlStatus validate_meta_arguments(
    const char* guid, StringBuilder* output) {
    if (!output || !guid) return UNITY_MATERIAL_YAML_INVALID_ARGUMENT;
    return unity_asset_guid_is_valid(guid)
        ? UNITY_MATERIAL_YAML_OK
        : UNITY_MATERIAL_YAML_INVALID_GUID;
}

UnityMaterialYamlStatus unity_shader_importer_meta_emit(
    const char* guid,
    StringBuilder* output) {
    UnityMaterialYamlStatus status = validate_meta_arguments(guid, output);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    sb_append(output, "fileFormatVersion: 2\nguid: ");
    sb_append(output, guid);
    sb_append(output,
        "\nShaderImporter:\n"
        "  externalObjects: {}\n"
        "  defaultTextures: []\n"
        "  nonModifiableTextures: []\n"
        "  preprocessorOverride: 0\n"
        "  userData: \n"
        "  assetBundleName: \n"
        "  assetBundleVariant: \n");
    return sb_ok(output)
        ? UNITY_MATERIAL_YAML_OK
        : UNITY_MATERIAL_YAML_OUTPUT_FAILED;
}

UnityMaterialYamlStatus unity_material_native_meta_emit(
    const char* guid,
    StringBuilder* output) {
    UnityMaterialYamlStatus status = validate_meta_arguments(guid, output);
    if (status != UNITY_MATERIAL_YAML_OK) return status;
    sb_append(output, "fileFormatVersion: 2\nguid: ");
    sb_append(output, guid);
    sb_append(output,
        "\nNativeFormatImporter:\n"
        "  externalObjects: {}\n"
        "  mainObjectFileID: 2100000\n"
        "  userData: \n"
        "  assetBundleName: \n"
        "  assetBundleVariant: \n");
    return sb_ok(output)
        ? UNITY_MATERIAL_YAML_OK
        : UNITY_MATERIAL_YAML_OUTPUT_FAILED;
}
