// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shader_dependency_closure.h"
#include "compiler/unity_reflection_certificate.h"
#include "io/serialized_shader_profile.h"
#include "io/typetree_value_digest.h"

#include <string.h>

#define DEPENDENCY_MAX_PROGRAMS 1048576U

static bool empty_string(const char *text) { return text && !text[0]; }

static bool state_name_is_static(const char *text) {
    return text && (!text[0] || strcmp(text, "<noninit>") == 0);
}

static bool empty_array(const TypeTreeValue *value) {
    return value && value->type == VAL_TYPE_ARRAY && value->array_val.count == 0;
}

static void hash_word(CommonSha256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(value >> (8U * i));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static bool hash_value(CommonSha256Context *hash, const TypeTreeValue *value) {
    uint8_t digest[32];
    if (!typetree_value_digest(value, digest))
        return false;
    common_sha256_update(hash, digest, sizeof(digest));
    return true;
}

static bool state_is_static(const SerializedShaderState *state) {
    const SerializedShaderFloatValue *const values[] = {
        &state->zClip,
        &state->zTest,
        &state->zWrite,
        &state->culling,
        &state->conservative,
        &state->offsetFactor,
        &state->offsetUnits,
        &state->alphaToMask,
        &state->stencilOp.pass,
        &state->stencilOp.fail,
        &state->stencilOp.zFail,
        &state->stencilOp.comp,
        &state->stencilOpFront.pass,
        &state->stencilOpFront.fail,
        &state->stencilOpFront.zFail,
        &state->stencilOpFront.comp,
        &state->stencilOpBack.pass,
        &state->stencilOpBack.fail,
        &state->stencilOpBack.zFail,
        &state->stencilOpBack.comp,
        &state->stencilReadMask,
        &state->stencilWriteMask,
        &state->stencilRef,
        &state->fogColor.x,
        &state->fogColor.y,
        &state->fogColor.z,
        &state->fogColor.w,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (!state_name_is_static(values[i]->name))
            return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        const SerializedShaderRTBlendState *blend = &state->rtBlend[i];
        const SerializedShaderFloatValue *const fields[] = {
            &blend->srcBlend, &blend->destBlend,    &blend->srcBlendAlpha, &blend->destBlendAlpha,
            &blend->blendOp,  &blend->blendOpAlpha, &blend->colMask,
        };
        for (size_t j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j) {
            if (!state_name_is_static(fields[j]->name))
                return false;
        }
    }
    return !state->lighting && state->fogMode >= -1 && state->fogMode <= 0;
}

static bool pipeline_is_builtin(const SerializedTagMap *tags) {
    if (tags->tag_count < 0 || (tags->tag_count && !tags->tags))
        return false;
    for (int i = 0; i < tags->tag_count; ++i) {
        if (!tags->tags[i].key || !tags->tags[i].value ||
            strcmp(tags->tags[i].key, "RenderPipeline") == 0)
            return false;
    }
    return true;
}

static UnityShaderDependencyStatus inspect_program(const ShaderBlobArchive *archive,
                                                   const SerializedPass *pass, int stage, int index,
                                                   CommonSha256Context *hash,
                                                   UnityShaderDependencyReport *report) {
    const SerializedSubProgram *serialized = &pass->subprograms[stage][index];
    const uint8_t *bytes = NULL;
    size_t size = 0;
    PlayerSubProgramMetadata player = {0};
    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    UnityShaderDependencyStatus status = UNITY_SHADER_DEPENDENCIES_INVALID_METADATA;
    if (serialized->blob_index < 0 ||
        !shader_blob_archive_get(archive, serialized->blob_index, &bytes, &size))
        goto cleanup;
    ByteStream stream;
    stream_init(&stream, bytes, size);
    stream_set_endian(&stream, false);
    if (!subprogram_metadata_parse_variant(&stream, &player) || !player.has_player_blob_header ||
        player.program_type != serialized->program_type ||
        !subprogram_metadata_local_keyword_set_matches(&player, serialized))
        goto cleanup;
    const int parameter_index = pass->subprogram_param_blob_indices[stage]
                                    ? pass->subprogram_param_blob_indices[stage][index]
                                    : -1;
    if (parameter_index < -1)
        goto cleanup;
    if (parameter_index >= 0) {
        if (!shader_blob_archive_get(archive, parameter_index, &bytes, &size))
            goto cleanup;
        stream_init(&stream, bytes, size);
        stream_set_endian(&stream, false);
        if (!subprogram_metadata_parse_parameters(&stream, &parameters))
            goto cleanup;
    }
    UnityReflectionCertificateReport bindings;
    /* No callbacks are supplied: only the independently validated expected
     * inventory is consumed, never the resulting callback-match status. */
    (void)unity_reflection_certify_d3d11_bindings(&player, &pass->common_parameters[stage],
                                                  parameter_index >= 0 ? &parameters : NULL, NULL,
                                                  0, &bindings);
    if (!bindings.expected_bindings_digest_valid)
        goto cleanup;
    report->parameter_record_count = bindings.expected_non_input_record_count;
    if (report->parameter_record_count) {
        status = UNITY_SHADER_DEPENDENCIES_EXTERNAL_BINDING;
        goto cleanup;
    }
    hash_word(hash, (uint32_t)index);
    common_sha256_update(hash, bindings.expected_bindings_digest, 32);
    status = UNITY_SHADER_DEPENDENCIES_OK;
cleanup:
    serialized_program_parameters_free(&parameters);
    subprogram_metadata_free_variant(&player);
    return status;
}

UnityShaderDependencyStatus unity_shader_dependency_closure(const ShaderObject *object,
                                                            UnityShaderDependencyReport *report) {
    if (!report)
        return UNITY_SHADER_DEPENDENCIES_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    report->subshader_index = report->pass_index = report->stage_index = report->subprogram_index =
        -1;
    report->status = UNITY_SHADER_DEPENDENCIES_INVALID_ARGUMENT;
    if (!object || !object->decoded)
        return report->status;
    report->status = UNITY_SHADER_DEPENDENCIES_INVALID_METADATA;
    if (!serialized_shader_profile_validate_value(&object->root, object->profile))
        return report->status;
    const SerializedShader *shader = &object->shader;
    const TypeTreeValue *parsed = typetree_find_child(&object->root, "m_ParsedForm");
    const TypeTreeValue *root_dependencies = typetree_find_child(&object->root, "m_Dependencies");
    const TypeTreeValue *textures = typetree_find_child(&object->root, "m_NonModifiableTextures");
    if (!empty_array(root_dependencies) || !empty_array(textures) || shader->dependency_count ||
        shader->property_count || !empty_string(shader->fallback_name) ||
        !empty_string(shader->custom_editor_name) ||
        shader->custom_editor_for_render_pipeline_count) {
        report->status = UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE;
        return report->status;
    }
    if (shader->subshader_count <= 0 || !shader->subshaders)
        return report->status;
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char domain[] = "DXBCSandbox.ResourceFreeVF.DependencyClosure.v1";
    common_sha256_update(&hash, domain, sizeof(domain));
    static const char *const fields[] = {
        "m_PropInfo",
        "m_FallbackName",
        "m_Dependencies",
        "m_CustomEditorName",
        "m_CustomEditorForRenderPipelines",
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (!hash_value(&hash, typetree_find_child(parsed, fields[i])))
            return report->status;
    }
    if (!hash_value(&hash, root_dependencies) || !hash_value(&hash, textures))
        return report->status;
    ShaderBlobArchive archive = {0};
    if (shader_object_open_d3d11_archive(object, &archive) != SHADER_OBJECT_OK) {
        report->status = UNITY_SHADER_DEPENDENCIES_ARCHIVE_UNAVAILABLE;
        return report->status;
    }
    hash_word(&hash, (uint32_t)shader->subshader_count);
    for (int s = 0; s < shader->subshader_count; ++s) {
        report->subshader_index = s;
        const SerializedSubShader *subshader = &shader->subshaders[s];
        report->status = UNITY_SHADER_DEPENDENCIES_UNSUPPORTED_PASS;
        if (subshader->pass_count <= 0 || !subshader->passes ||
            !pipeline_is_builtin(&subshader->tags))
            goto cleanup;
        hash_word(&hash, (uint32_t)subshader->pass_count);
        for (int p = 0; p < subshader->pass_count; ++p) {
            report->pass_index = p;
            const SerializedPass *pass = &subshader->passes[p];
            report->status = UNITY_SHADER_DEPENDENCIES_UNSUPPORTED_PASS;
            if (pass->pass_type != 0 || !empty_string(pass->use_name) ||
                !empty_string(pass->texture_name) || pass->has_instancing_variant ||
                pass->has_procedural_instancing_variant || !state_is_static(&pass->state) ||
                !pipeline_is_builtin(&pass->tags))
                goto cleanup;
            for (int stage = 2; stage < 6; ++stage) {
                if (pass->subprogram_count[stage])
                    goto cleanup;
            }
            if (++report->pass_count > DEPENDENCY_MAX_PROGRAMS) {
                report->status = UNITY_SHADER_DEPENDENCIES_LIMIT_EXCEEDED;
                goto cleanup;
            }
            const char *const fog_inputs[] = {
                pass->state.fogColor.name,
                pass->state.fogStart.name,
                pass->state.fogEnd.name,
                pass->state.fogDensity.name,
            };
            static const char *const fog_names[] = {
                "unity_FogColor",
                "unity_FogStart",
                "unity_FogEnd",
                "unity_FogDensity",
            };
            for (size_t f = 0; f < 4; ++f) {
                if (fog_inputs[f] && strcmp(fog_inputs[f], fog_names[f]) == 0)
                    ++report->engine_fog_input_count;
                else if (!state_name_is_static(fog_inputs[f])) {
                    report->status = UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE;
                    goto cleanup;
                }
                hash_word(&hash, strlen(fog_inputs[f]));
                common_sha256_update(&hash, fog_inputs[f], strlen(fog_inputs[f]));
            }
            for (int stage = 0; stage < 2; ++stage) {
                report->stage_index = stage;
                size_t stage_count = 0;
                for (int i = 0; i < pass->subprogram_count[stage]; ++i) {
                    if (!serialized_pass_subprogram_is_platform(pass, stage, i, 4))
                        continue;
                    report->subprogram_index = i;
                    if (++report->subprogram_count > DEPENDENCY_MAX_PROGRAMS) {
                        report->status = UNITY_SHADER_DEPENDENCIES_LIMIT_EXCEEDED;
                        goto cleanup;
                    }
                    report->status = inspect_program(&archive, pass, stage, i, &hash, report);
                    if (report->status != UNITY_SHADER_DEPENDENCIES_OK)
                        goto cleanup;
                    ++stage_count;
                }
                if (!stage_count) {
                    report->status = UNITY_SHADER_DEPENDENCIES_UNSUPPORTED_PASS;
                    goto cleanup;
                }
                hash_word(&hash, stage_count);
            }
        }
    }
    common_sha256_final(&hash, report->digest);
    report->status = UNITY_SHADER_DEPENDENCIES_OK;
cleanup:
    shader_blob_archive_close(&archive);
    return report->status;
}

const char *unity_shader_dependency_status_name(UnityShaderDependencyStatus status) {
    switch (status) {
    case UNITY_SHADER_DEPENDENCIES_OK:
        return "ok";
    case UNITY_SHADER_DEPENDENCIES_INVALID_ARGUMENT:
        return "invalid-argument";
    case UNITY_SHADER_DEPENDENCIES_INVALID_METADATA:
        return "invalid-metadata";
    case UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE:
        return "external-reference";
    case UNITY_SHADER_DEPENDENCIES_UNSUPPORTED_PASS:
        return "unsupported-pass";
    case UNITY_SHADER_DEPENDENCIES_EXTERNAL_BINDING:
        return "external-binding";
    case UNITY_SHADER_DEPENDENCIES_ARCHIVE_UNAVAILABLE:
        return "archive-unavailable";
    case UNITY_SHADER_DEPENDENCIES_LIMIT_EXCEEDED:
        return "limit-exceeded";
    }
    return "unknown";
}
