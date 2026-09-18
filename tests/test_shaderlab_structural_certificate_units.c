#include "io/serialized_file.h"
#include "io/shader_object.h"
#include "io/typetree_schema_registry.h"
#include "io/unity_input.h"
#include "translation/shaderlab_structural_certificate.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    TypeTreeSchemaRegistry registry;
    int failures;
    bool found_certifiable;
    bool found_disabled_message;
    bool found_dependency;
    bool found_grab_pass;
    bool found_use_pass;
    size_t normal_passes_with_lightmode;
    size_t grab_passes_with_grabpass_tag;
    size_t use_passes_with_empty_tags;
} CertificateTestContext;

static void check_at(CertificateTestContext* context, bool condition,
                     int line, const char* expression) {
    if (condition) return;
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, line,
            expression);
    ++context->failures;
}

#define CONTEXT_CHECK(context, condition) \
    check_at((context), (condition), __LINE__, #condition)

static const char* tag_value(const SerializedTagMap* tags,
                             const char* key) {
    if (!tags || !key) return NULL;
    for (int index = 0; index < tags->tag_count; ++index) {
        if (tags->tags[index].key && tags->tags[index].value &&
            strcmp(tags->tags[index].key, key) == 0) {
            return tags->tags[index].value;
        }
    }
    return NULL;
}

static void census_pass_tag_sources(CertificateTestContext* context,
                                    const ShaderObject* object) {
    for (int subshader_index = 0;
         subshader_index < object->shader.subshader_count;
         ++subshader_index) {
        const SerializedSubShader* subshader =
            &object->shader.subshaders[subshader_index];
        for (int pass_index = 0; pass_index < subshader->pass_count;
             ++pass_index) {
            const SerializedPass* pass = &subshader->passes[pass_index];
            const char* light_mode = tag_value(&pass->tags, "LIGHTMODE");
            if (pass->pass_type == 0 && light_mode) {
                ++context->normal_passes_with_lightmode;
            } else if (pass->pass_type == 1 && pass->tags.tag_count == 0) {
                ++context->use_passes_with_empty_tags;
            } else if (pass->pass_type == 2 && light_mode &&
                       strcmp(light_mode, "GRABPASS") == 0) {
                ++context->grab_passes_with_grabpass_tag;
            }
        }
    }
}

static void test_certifiable_object(CertificateTestContext* context,
                                    ShaderObject* object) {
    ShaderLabStructuralDiagnostic diagnostic;
    CONTEXT_CHECK(context,
        shaderlab_structural_certify(object, &diagnostic) ==
            SHADERLAB_STRUCTURE_OK);
    CONTEXT_CHECK(context, diagnostic.status == SHADERLAB_STRUCTURE_OK);
    CONTEXT_CHECK(context, diagnostic.covered_semantic_fields > 0U);
    CONTEXT_CHECK(context, diagnostic.excluded_compiled_fields > 0U);
    CONTEXT_CHECK(context, !diagnostic.runtime_selection_certified);
    CONTEXT_CHECK(context, !diagnostic.visual_output_certified);
    if (object->shader.subshader_count <= 0 ||
        object->shader.subshaders[0].pass_count <= 0) {
        CONTEXT_CHECK(context, false);
        return;
    }

    SerializedPass* pass = &object->shader.subshaders[0].passes[0];
    const bool saved_disable = object->shader.disable_no_subshaders_message;
    const char* saved_fallback = object->shader.fallback_name;
    object->shader.disable_no_subshaders_message = true;
    object->shader.fallback_name = "Contradictory/Fallback";
    CONTEXT_CHECK(context,
        shaderlab_structural_certify(object, &diagnostic) ==
            SHADERLAB_STRUCTURE_OK);
    object->shader.disable_no_subshaders_message = saved_disable;
    object->shader.fallback_name = saved_fallback;

    const int saved_lod = pass->state.lod;
    pass->state.lod = -1;
    CONTEXT_CHECK(context,
        shaderlab_structural_certify(object, &diagnostic) ==
            SHADERLAB_STRUCTURE_UNSUPPORTED_SEMANTIC);
    CONTEXT_CHECK(context,
        diagnostic.field == SHADERLAB_STRUCTURE_FIELD_STATE_LOD);
    pass->state.lod = saved_lod;

    const char* saved_pass_name = pass->name;
    pass->name = "unprojected-normal-pass-payload";
    CONTEXT_CHECK(context,
        shaderlab_structural_certify(object, &diagnostic) ==
            SHADERLAB_STRUCTURE_DROPPED_SEMANTIC);
    CONTEXT_CHECK(context,
        diagnostic.field == SHADERLAB_STRUCTURE_FIELD_PASS_PAYLOAD);
    pass->name = saved_pass_name;

    SerializedTag synthetic_tag = {"Mismatched", "Projection"};
    const SerializedTagMap saved_tags = pass->tags;
    pass->tags.tag_count = 1;
    pass->tags.tags = &synthetic_tag;
    CONTEXT_CHECK(context,
        shaderlab_structural_certify(object, &diagnostic) ==
            SHADERLAB_STRUCTURE_PROJECTION_MISMATCH);
    CONTEXT_CHECK(context,
        diagnostic.field == SHADERLAB_STRUCTURE_FIELD_STATE_TAGS);
    pass->tags = saved_tags;

    const SerializedShaderState saved_state = pass->state;
    pass->state.fogMode = -1;
    pass->state.fogColor.name = "<noninit>";
    SerializedShaderFloatValue* fog_values[] = {
        &pass->state.fogColor.x, &pass->state.fogColor.y,
        &pass->state.fogColor.z, &pass->state.fogColor.w,
        &pass->state.fogStart, &pass->state.fogEnd,
        &pass->state.fogDensity,
    };
    for (size_t index = 0U;
         index < sizeof(fog_values) / sizeof(fog_values[0]); ++index) {
        fog_values[index]->present = true;
        fog_values[index]->val = 0.0f;
        fog_values[index]->name = "<noninit>";
    }
    CONTEXT_CHECK(context,
        shaderlab_structural_certify(object, &diagnostic) ==
            SHADERLAB_STRUCTURE_AMBIGUOUS_SEMANTIC);
    CONTEXT_CHECK(context,
        diagnostic.field == SHADERLAB_STRUCTURE_FIELD_FOG);
    pass->state = saved_state;

    context->found_certifiable = true;
}

static bool inspect_source(const UnitySerializedSource* source,
                           void* opaque) {
    CertificateTestContext* context =
        (CertificateTestContext*)opaque;
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        CONTEXT_CHECK(context, false);
        return false;
    }
    if (serialized_file_resolve_class_schema(
            &file, 48, &context->registry) != TYPETREE_SCHEMA_OK) {
        CONTEXT_CHECK(context, false);
        serialized_file_close(&file);
        return false;
    }

    for (int index = 0; index < file.object_count; ++index) {
        const AssetObjectInfo* asset = &file.objects[index];
        if (asset->type_id != 48) continue;
        ShaderObject object;
        shader_object_init(&object);
        if (shader_object_decode_borrowed(&object, &file, asset) !=
            SHADER_OBJECT_OK) {
            CONTEXT_CHECK(context, false);
            shader_object_dispose(&object);
            serialized_file_close(&file);
            return false;
        }

        if (!context->found_certifiable && object.shader.name &&
            strcmp(object.shader.name, "Hidden/SeparableBlur") == 0) {
            test_certifiable_object(context, &object);
        }
        if (!context->found_disabled_message && object.shader.name &&
            strcmp(object.shader.name, "Hidden/BlitCopy") == 0) {
            ShaderLabStructuralDiagnostic diagnostic;
            CONTEXT_CHECK(context,
                object.shader.disable_no_subshaders_message);
            CONTEXT_CHECK(context,
                object.shader.fallback_name != NULL &&
                object.shader.fallback_name[0] == '\0');
            CONTEXT_CHECK(context,
                shaderlab_structural_certify(&object, &diagnostic) ==
                    SHADERLAB_STRUCTURE_OK);
            context->found_disabled_message = true;
        }
        if (!context->found_dependency &&
            object.shader.dependency_count > 0 &&
            !object.shader.disable_no_subshaders_message) {
            ShaderLabStructuralDiagnostic diagnostic;
            CONTEXT_CHECK(context,
                object.shader.dependencies != NULL);
            CONTEXT_CHECK(context,
                object.shader.dependencies[0].from != NULL);
            CONTEXT_CHECK(context,
                object.shader.dependencies[0].to != NULL);
            CONTEXT_CHECK(context,
                shaderlab_structural_certify(&object, &diagnostic) ==
                    SHADERLAB_STRUCTURE_OK);
            context->found_dependency = true;
        }
        if (!context->found_grab_pass && object.shader.name &&
            strcmp(object.shader.name, "Particles/Standard Unlit") == 0) {
            ShaderLabStructuralDiagnostic diagnostic;
            CONTEXT_CHECK(context,
                object.shader.subshader_count > 0 &&
                object.shader.subshaders[0].pass_count > 0 &&
                object.shader.subshaders[0].passes[0].pass_type == 2);
            CONTEXT_CHECK(context,
                object.shader.subshaders[0].passes[0].tags.tag_count == 1);
            const char* light_mode = tag_value(
                &object.shader.subshaders[0].passes[0].tags, "LIGHTMODE");
            CONTEXT_CHECK(context,
                light_mode && strcmp(light_mode, "GRABPASS") == 0);
            CONTEXT_CHECK(context,
                shaderlab_structural_certify(&object, &diagnostic) ==
                    SHADERLAB_STRUCTURE_OK);
            context->found_grab_pass = true;
        }
        if (!context->found_use_pass && object.shader.name &&
            strcmp(object.shader.name,
                   "Legacy Shaders/Reflective/Bumped VertexLit") == 0) {
            ShaderLabStructuralDiagnostic diagnostic;
            CONTEXT_CHECK(context,
                object.shader.subshader_count > 0 &&
                object.shader.subshaders[0].pass_count > 0 &&
                object.shader.subshaders[0].passes[0].pass_type == 1);
            CONTEXT_CHECK(context,
                object.shader.subshaders[0].passes[0].tags.tag_count == 0);
            bool found_normal_lightmode = false;
            for (int pass_index = 0;
                 pass_index < object.shader.subshaders[0].pass_count;
                 ++pass_index) {
                const SerializedPass* pass =
                    &object.shader.subshaders[0].passes[pass_index];
                const char* light_mode = tag_value(&pass->tags, "LIGHTMODE");
                if (pass->pass_type == 0 && light_mode &&
                    strcmp(light_mode, "Vertex") == 0) {
                    found_normal_lightmode = true;
                }
            }
            CONTEXT_CHECK(context, found_normal_lightmode);
            CONTEXT_CHECK(context,
                shaderlab_structural_certify(&object, &diagnostic) ==
                    SHADERLAB_STRUCTURE_OK);
            context->found_use_pass = true;
        }
        census_pass_tag_sources(context, &object);
        shader_object_dispose(&object);
    }
    serialized_file_close(&file);
    return true;
}

int main(void) {
    CertificateTestContext context;
    memset(&context, 0, sizeof(context));
    typetree_schema_registry_init(&context.registry);
    if (typetree_schema_registry_import_file_replace(
            &context.registry, DXBC_TEST_PLAYER_SCHEMA_REGISTRY) !=
        TYPETREE_SCHEMA_OK) {
        fprintf(stderr, "could not import exact player schema\n");
        typetree_schema_registry_dispose(&context.registry);
        return 1;
    }

    UnityInputVisitStats stats;
    const UnityInputStatus input_status = unity_input_visit_serialized(
        DXBC_TEST_SHADER_BUNDLE, inspect_source, &context, &stats);
    CONTEXT_CHECK(&context, input_status == UNITY_INPUT_OK);
    CONTEXT_CHECK(&context, context.found_certifiable);
    CONTEXT_CHECK(&context, context.found_disabled_message);
    CONTEXT_CHECK(&context, context.found_dependency);
    CONTEXT_CHECK(&context, context.found_grab_pass);
    CONTEXT_CHECK(&context, context.found_use_pass);
    CONTEXT_CHECK(&context, context.normal_passes_with_lightmode > 0U);
    CONTEXT_CHECK(&context, context.grab_passes_with_grabpass_tag > 0U);
    CONTEXT_CHECK(&context, context.use_passes_with_empty_tags > 0U);
    typetree_schema_registry_dispose(&context.registry);
    CONTEXT_CHECK(&context, g_allocations_count == 0U);
    CONTEXT_CHECK(&context, g_allocated_bytes == 0U);
    if (context.failures != 0) return 1;
    printf("ShaderLab structural certificate unit tests passed.\n");
    return 0;
}
