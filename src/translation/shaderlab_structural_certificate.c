// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_structural_certificate.h"

#include "io/serialized_shader_profile.h"

#include <string.h>

static void diagnostic_init(ShaderLabStructuralDiagnostic* diagnostic) {
    if (!diagnostic) return;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = SHADERLAB_STRUCTURE_INVALID_ARGUMENT;
    diagnostic->property_index = -1;
    diagnostic->subshader_index = -1;
    diagnostic->pass_index = -1;
    diagnostic->element_index = -1;
    /* These are always separate gates; this module can never set them. */
    diagnostic->runtime_selection_certified = false;
    diagnostic->visual_output_certified = false;
}

static ShaderLabStructuralStatus fail(
    ShaderLabStructuralDiagnostic* diagnostic,
    ShaderLabStructuralStatus status, ShaderLabStructuralField field,
    int property, int subshader, int pass, int element) {
    if (diagnostic) {
        diagnostic->status = status;
        diagnostic->field = field;
        diagnostic->property_index = property;
        diagnostic->subshader_index = subshader;
        diagnostic->pass_index = pass;
        diagnostic->element_index = element;
    }
    return status;
}

static bool string_is_empty(const char* value) {
    return !value || value[0] == '\0';
}

static bool name_is_noninit(const char* value) {
    return string_is_empty(value) || strcmp(value, "<noninit>") == 0;
}

static bool float_value_is_plain(
    const SerializedShaderFloatValue* value, float expected) {
    return value && value->present && value->val == expected &&
           name_is_noninit(value->name);
}

static bool float_value_is_plain_zero(
    const SerializedShaderFloatValue* value) {
    return float_value_is_plain(value, 0.0f);
}

static bool fog_authority_is_wholly_uninitialized(
    const SerializedShaderState* state) {
    return state && state->fogMode == -1 &&
           name_is_noninit(state->fogColor.name) &&
           float_value_is_plain_zero(&state->fogColor.x) &&
           float_value_is_plain_zero(&state->fogColor.y) &&
           float_value_is_plain_zero(&state->fogColor.z) &&
           float_value_is_plain_zero(&state->fogColor.w) &&
           float_value_is_plain_zero(&state->fogStart) &&
           float_value_is_plain_zero(&state->fogEnd) &&
           float_value_is_plain_zero(&state->fogDensity);
}

static bool tag_map_is_valid(const SerializedTagMap* tags) {
    if (!tags || tags->tag_count < 0 ||
        (tags->tag_count > 0 && !tags->tags)) {
        return false;
    }
    for (int index = 0; index < tags->tag_count; ++index) {
        if (!tags->tags[index].key || !tags->tags[index].value) return false;
    }
    return true;
}

static bool pass_has_compiled_program(const SerializedPass* pass) {
    if (!pass) return true;
    if (pass->program_mask != 0U) return true;
    for (int stage = 0; stage < 6; ++stage) {
        if (pass->subprogram_count[stage] != 0) return true;
    }
    return false;
}

static bool rt_blend_is_nonprogram_sentinel(
    const SerializedShaderRTBlendState* state) {
    return float_value_is_plain(&state->srcBlend, 1.0f) &&
           float_value_is_plain_zero(&state->destBlend) &&
           float_value_is_plain(&state->srcBlendAlpha, 1.0f) &&
           float_value_is_plain_zero(&state->destBlendAlpha) &&
           float_value_is_plain_zero(&state->blendOp) &&
           float_value_is_plain_zero(&state->blendOpAlpha) &&
           float_value_is_plain(&state->colMask, 15.0f);
}

static bool stencil_is_nonprogram_sentinel(const SerializedStencilOp* state) {
    return float_value_is_plain_zero(&state->pass) &&
           float_value_is_plain_zero(&state->fail) &&
           float_value_is_plain_zero(&state->zFail) &&
           float_value_is_plain_zero(&state->comp);
}

static bool pass_render_state_is_nonprogram_sentinel(
    const SerializedPass* pass) {
    if (!pass) return false;
    const SerializedShaderState* state = &pass->state;
    for (int index = 0; index < 8; ++index) {
        if (!rt_blend_is_nonprogram_sentinel(&state->rtBlend[index])) {
            return false;
        }
    }
    return string_is_empty(state->name) && !state->rtSeparateBlend &&
           float_value_is_plain_zero(&state->zClip) &&
           float_value_is_plain_zero(&state->zTest) &&
           float_value_is_plain_zero(&state->zWrite) &&
           float_value_is_plain_zero(&state->culling) &&
           float_value_is_plain_zero(&state->conservative) &&
           float_value_is_plain_zero(&state->offsetFactor) &&
           float_value_is_plain_zero(&state->offsetUnits) &&
           float_value_is_plain_zero(&state->alphaToMask) &&
           stencil_is_nonprogram_sentinel(&state->stencilOp) &&
           stencil_is_nonprogram_sentinel(&state->stencilOpFront) &&
           stencil_is_nonprogram_sentinel(&state->stencilOpBack) &&
           float_value_is_plain_zero(&state->stencilReadMask) &&
           float_value_is_plain_zero(&state->stencilWriteMask) &&
           float_value_is_plain_zero(&state->stencilRef) &&
           fog_authority_is_wholly_uninitialized(state) &&
           state->gpuProgramID == -1 && state->lod == 0 &&
           !state->lighting;
}

static const TypeTreeValue* array_field(const TypeTreeValue* parent,
                                        const char* name) {
    const TypeTreeValue* value = typetree_find_child(parent, name);
    return typetree_get_array(value);
}

static const TypeTreeValue* raw_pass_tag_array(
    const ShaderObject* object, int subshader, int pass, int pass_type) {
    const TypeTreeValue* parsed = typetree_find_child(&object->root,
                                                      "m_ParsedForm");
    const TypeTreeValue* subshaders = array_field(parsed, "m_SubShaders");
    if (!subshaders || subshader < 0 ||
        subshader >= subshaders->array_val.count) {
        return NULL;
    }
    const TypeTreeValue* raw_subshader =
        &subshaders->array_val.elements[subshader];
    const TypeTreeValue* passes = array_field(raw_subshader, "m_Passes");
    if (!passes || pass < 0 || pass >= passes->array_val.count) return NULL;
    const TypeTreeValue* raw_pass = &passes->array_val.elements[pass];
    const TypeTreeValue* tags = NULL;
    /* Match the parser's pass-kind authority instead of falling back based on
     * field presence: normal passes serialize both fields, but the outer map
     * is empty and m_State.m_Tags carries the effective runtime tags. */
    if (pass_type == 0) {
        const TypeTreeValue* state = typetree_find_child(raw_pass, "m_State");
        tags = typetree_find_child(state, "m_Tags");
    } else {
        tags = typetree_find_child(raw_pass, "m_Tags");
    }
    const TypeTreeValue* values = array_field(tags, "tags");
    if (!values) values = typetree_get_array(tags);
    return values;
}

static bool raw_pass_tags_match(const ShaderObject* object, int subshader,
                                int pass, int pass_type,
                                const SerializedTagMap* projected) {
    const TypeTreeValue* raw = raw_pass_tag_array(object, subshader, pass,
                                                  pass_type);
    if (!raw || !projected || raw->array_val.count != projected->tag_count) {
        return false;
    }
    for (int index = 0; index < projected->tag_count; ++index) {
        const TypeTreeValue* pair = &raw->array_val.elements[index];
        const TypeTreeValue* first = typetree_find_child(pair, "first");
        const TypeTreeValue* second = typetree_find_child(pair, "second");
        if (!first || first->type != VAL_TYPE_STRING ||
            !second || second->type != VAL_TYPE_STRING ||
            !first->string_val || !second->string_val ||
            strcmp(first->string_val, projected->tags[index].key) != 0 ||
            strcmp(second->string_val, projected->tags[index].value) != 0) {
            return false;
        }
    }
    return true;
}

static bool add_covered(ShaderLabStructuralDiagnostic* diagnostic,
                        size_t count) {
    if (!diagnostic) return true;
    if (diagnostic->covered_semantic_fields > SIZE_MAX - count) return false;
    diagnostic->covered_semantic_fields += count;
    return true;
}

static bool add_excluded(ShaderLabStructuralDiagnostic* diagnostic,
                         size_t count) {
    if (!diagnostic) return true;
    if (diagnostic->excluded_compiled_fields > SIZE_MAX - count) return false;
    diagnostic->excluded_compiled_fields += count;
    return true;
}

ShaderLabStructuralStatus shaderlab_structural_certify(
    const ShaderObject* object, ShaderLabStructuralDiagnostic* diagnostic) {
    diagnostic_init(diagnostic);
    if (!object || !object->decoded || !object->shader.name ||
        object->shader.property_count < 0 ||
        object->shader.subshader_count < 0 ||
        (object->shader.property_count > 0 && !object->shader.properties) ||
        (object->shader.subshader_count > 0 && !object->shader.subshaders)) {
        return fail(diagnostic, SHADERLAB_STRUCTURE_INVALID_ARGUMENT,
                    SHADERLAB_STRUCTURE_FIELD_SHADER_MODEL, -1, -1, -1,
                    -1);
    }
    if (!serialized_shader_profile_validate_value(&object->root,
                                                   object->profile)) {
        return fail(diagnostic,
                    SHADERLAB_STRUCTURE_SCHEMA_AUTHORITY_FAILED,
                    SHADERLAB_STRUCTURE_FIELD_SHADER_MODEL, -1, -1, -1,
                    -1);
    }

    const SerializedShader* shader = &object->shader;
    if (shader->dependency_count < 0 ||
        shader->custom_editor_for_render_pipeline_count < 0 ||
        (shader->dependency_count > 0 && !shader->dependencies) ||
        (shader->custom_editor_for_render_pipeline_count > 0 &&
         !shader->custom_editors_for_render_pipelines)) {
        return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                    SHADERLAB_STRUCTURE_FIELD_SHADER_MODEL, -1, -1, -1,
                    -1);
    }
    if (!add_covered(diagnostic, 4U)) {
        return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                    SHADERLAB_STRUCTURE_FIELD_SHADER_MODEL, -1, -1, -1,
                    -1);
    }

    for (int index = 0; index < shader->property_count; ++index) {
        const ParsedShaderProperty* property = &shader->properties[index];
        if (!property->name || !property->description ||
            property->attribute_count < 0 ||
            (property->attribute_count > 0 && !property->attributes)) {
            return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                        SHADERLAB_STRUCTURE_FIELD_PROPERTY, index, -1, -1,
                        -1);
        }
        for (int attribute = 0; attribute < property->attribute_count;
             ++attribute) {
            if (!property->attributes[attribute]) {
                return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                            SHADERLAB_STRUCTURE_FIELD_PROPERTY, index, -1,
                            -1, attribute);
            }
        }
        /* SerializedShaderFloatValue is a discriminated union selected by
         * m_Type. Unity legitimately leaves non-selected storage populated
         * (for example Vector defaults alongside texture-union members).
         * Only the active arm is a ShaderLab semantic; candidate emission
         * immediately preceding this certificate validates that arm. */
        if (property->type < 0 || property->type > 5) {
            return fail(diagnostic,
                        SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC,
                        SHADERLAB_STRUCTURE_FIELD_PROPERTY, index, -1, -1,
                        -1);
        }
        if (!add_covered(diagnostic,
                         8U + (size_t)property->attribute_count)) {
            return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                        SHADERLAB_STRUCTURE_FIELD_PROPERTY, index, -1, -1,
                        -1);
        }
    }

    for (int index = 0; index < shader->dependency_count; ++index) {
        if (string_is_empty(shader->dependencies[index].from) ||
            string_is_empty(shader->dependencies[index].to)) {
            return fail(diagnostic,
                        SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC,
                        SHADERLAB_STRUCTURE_FIELD_DEPENDENCY, -1, -1, -1,
                        index);
        }
        if (!add_covered(diagnostic, 2U)) {
            return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                        SHADERLAB_STRUCTURE_FIELD_DEPENDENCY, -1, -1, -1,
                        index);
        }
    }
    for (int index = 0;
         index < shader->custom_editor_for_render_pipeline_count; ++index) {
        const SerializedCustomEditorForRenderPipeline* editor =
            &shader->custom_editors_for_render_pipelines[index];
        if (string_is_empty(editor->custom_editor_name) ||
            string_is_empty(editor->render_pipeline_type)) {
            return fail(diagnostic,
                        SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC,
                        SHADERLAB_STRUCTURE_FIELD_CUSTOM_EDITOR, -1, -1,
                        -1, index);
        }
        if (!add_covered(diagnostic, 2U)) {
            return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                        SHADERLAB_STRUCTURE_FIELD_CUSTOM_EDITOR, -1, -1,
                        -1, index);
        }
    }

    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; ++subshader_index) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        if (subshader->pass_count < 0 ||
            (subshader->pass_count > 0 && !subshader->passes) ||
            !tag_map_is_valid(&subshader->tags)) {
            return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                        SHADERLAB_STRUCTURE_FIELD_SUBSHADER_TAGS, -1,
                        subshader_index, -1, -1);
        }
        if (subshader->lod < 0) {
            return fail(diagnostic,
                        SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC,
                        SHADERLAB_STRUCTURE_FIELD_SUBSHADER_LOD, -1,
                        subshader_index, -1, -1);
        }
        if (!add_covered(diagnostic,
                         2U + (size_t)subshader->tags.tag_count * 2U)) {
            return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                        SHADERLAB_STRUCTURE_FIELD_SUBSHADER_TAGS, -1,
                        subshader_index, -1, -1);
        }

        for (int pass_index = 0; pass_index < subshader->pass_count;
             ++pass_index) {
            const SerializedPass* pass = &subshader->passes[pass_index];
            if (pass->pass_type < 0 || pass->pass_type > 2) {
                return fail(diagnostic,
                            SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC,
                            SHADERLAB_STRUCTURE_FIELD_PASS_TYPE, -1,
                            subshader_index, pass_index, -1);
            }
            if (!tag_map_is_valid(&pass->tags)) {
                return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                            SHADERLAB_STRUCTURE_FIELD_PASS_TAGS, -1,
                            subshader_index, pass_index, -1);
            }
            if (!raw_pass_tags_match(object, subshader_index, pass_index,
                                     pass->pass_type, &pass->tags)) {
                return fail(diagnostic,
                            SHADERLAB_STRUCTURE_PROJECTION_MISMATCH,
                            SHADERLAB_STRUCTURE_FIELD_STATE_TAGS, -1,
                            subshader_index, pass_index, -1);
            }

            if (pass->pass_type == 0) {
                if (!string_is_empty(pass->name) ||
                    !string_is_empty(pass->use_name) ||
                    !string_is_empty(pass->texture_name)) {
                    return fail(diagnostic,
                                SHADERLAB_STRUCTURE_DROPPED_SEMANTIC,
                                SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD, -1,
                                subshader_index, pass_index, -1);
                }
                if (pass->state.lod < 0) {
                    return fail(diagnostic,
                                SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC,
                                SHADERLAB_STRUCTURE_FIELD_STATE_LOD, -1,
                                subshader_index, pass_index, -1);
                }
                if (fog_authority_is_wholly_uninitialized(&pass->state)) {
                    return fail(diagnostic,
                                SHADERLAB_STRUCTURE_AMBIGUOUS_SEMANTIC,
                                SHADERLAB_STRUCTURE_FIELD_FOG, -1,
                                subshader_index, pass_index, -1);
                }
            } else if (pass->pass_type == 1) {
                if (string_is_empty(pass->use_name) ||
                    !string_is_empty(pass->name) ||
                    !string_is_empty(pass->texture_name) ||
                    pass->tags.tag_count != 0 ||
                    !string_is_empty(pass->state.name) ||
                    pass->state.lod != 0 ||
                    pass_has_compiled_program(pass) ||
                    !pass_render_state_is_nonprogram_sentinel(pass)) {
                    return fail(diagnostic,
                                SHADERLAB_STRUCTURE_DROPPED_SEMANTIC,
                                SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD, -1,
                                subshader_index, pass_index, -1);
                }
            } else {
                if (!string_is_empty(pass->use_name) ||
                    !string_is_empty(pass->state.name) ||
                    pass->state.lod != 0 ||
                    pass_has_compiled_program(pass) ||
                    !pass_render_state_is_nonprogram_sentinel(pass)) {
                    return fail(diagnostic,
                                SHADERLAB_STRUCTURE_DROPPED_SEMANTIC,
                                SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD, -1,
                                subshader_index, pass_index, -1);
                }
            }
            if (!add_covered(
                    diagnostic,
                    7U + (size_t)pass->tags.tag_count * 2U + 79U)) {
                return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                            SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD, -1,
                            subshader_index, pass_index, -1);
            }
            /* Editor hashes, platform/program tables, name indices, keyword
             * masks, compiled IDs, and parameter bindings are intentionally
             * delegated to compiler/runtime gates. */
            if (!add_excluded(diagnostic, 14U)) {
                return fail(diagnostic, SHADERLAB_STRUCTURE_MODEL_INVALID,
                            SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD, -1,
                            subshader_index, pass_index, -1);
            }
        }
    }

    if (diagnostic) {
        diagnostic->status = SHADERLAB_STRUCTURE_OK;
        diagnostic->field = SHADERLAB_STRUCTURE_FIELD_NONE;
    }
    return SHADERLAB_STRUCTURE_OK;
}

const char* shaderlab_structural_status_name(
    ShaderLabStructuralStatus status) {
    switch (status) {
        case SHADERLAB_STRUCTURE_OK: return "ok";
        case SHADERLAB_STRUCTURE_INVALID_ARGUMENT:
            return "invalid-argument";
        case SHADERLAB_STRUCTURE_SCHEMA_AUTHORITY_FAILED:
            return "schema-authority-failed";
        case SHADERLAB_STRUCTURE_MODEL_INVALID: return "model-invalid";
        case SHADERLAB_STRUCTURE_PROJECTION_MISMATCH:
            return "projection-mismatch";
        case SHADERLAB_STRUCTURE_DROPPED_SEMANTIC:
            return "dropped-semantic";
        case SHADERLAB_STRUCTURE_AMBIGUOUS_SEMANTIC:
            return "ambiguous-semantic";
        case SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC:
            return "unsupported-semantic";
        default: return "unknown";
    }
}

const char* shaderlab_structural_field_name(ShaderLabStructuralField field) {
    switch (field) {
        case SHADERLAB_STRUCTURE_FIELD_NONE: return "none";
        case SHADERLAB_STRUCTURE_FIELD_SHADER_MODEL: return "shader-model";
        case SHADERLAB_STRUCTURE_FIELD_PROPERTY: return "property";
        case SHADERLAB_STRUCTURE_FIELD_DEPENDENCY: return "dependency";
        case SHADERLAB_STRUCTURE_FIELD_CUSTOM_EDITOR:
            return "custom-editor-for-render-pipeline";
        case SHADERLAB_STRUCTURE_FIELD_DISABLE_NO_SUBSHADERS_MESSAGE:
            return "disable-no-subshaders-message";
        case SHADERLAB_STRUCTURE_FIELD_FALLBACK: return "fallback";
        case SHADERLAB_STRUCTURE_FIELD_SUBSHADER_TAGS:
            return "subshader-tags";
        case SHADERLAB_STRUCTURE_FIELD_SUBSHADER_LOD:
            return "subshader-lod";
        case SHADERLAB_STRUCTURE_FIELD_PASS_TYPE: return "pass-type";
        case SHADERLAB_STRUCTURE_FIELD_PASS_NAME: return "pass-name";
        case SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD: return "pass-payload";
        case SHADERLAB_STRUCTURE_FIELD_PASS_TAGS: return "pass-tags";
        case SHADERLAB_STRUCTURE_FIELD_STATE_TAGS: return "state-tags";
        case SHADERLAB_STRUCTURE_FIELD_STATE_LOD: return "state-lod";
        case SHADERLAB_STRUCTURE_FIELD_RENDER_STATE: return "render-state";
        case SHADERLAB_STRUCTURE_FIELD_FOG: return "fog";
        default: return "unknown";
    }
}
