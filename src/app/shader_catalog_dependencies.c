// SPDX-License-Identifier: GPL-3.0-only

#include "app/shader_catalog_dependencies_internal.h"
#include "app/shader_catalog_internal.h"
#include "app/release_shader_object_certificate.h"
#include "io/serialized_shader_profile.h"
#include "io/typetree_value_digest.h"

#include <stdlib.h>
#include <string.h>

static ShaderCatalogDependenciesStatus finish(
    ShaderCatalogDependenciesDiagnostic *diagnostic, ShaderCatalogDependenciesStatus status) {
    diagnostic->status = status;
    return status;
}

static void diagnostic_init(ShaderCatalogDependenciesDiagnostic *diagnostic) {
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = SHADER_CATALOG_DEPENDENCIES_INVALID_ARGUMENT;
    diagnostic->object_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    diagnostic->archive_status = SHADER_OBJECT_NOT_DECODED;
    diagnostic->schema_profile = TYPETREE_SCHEMA_PROFILE_UNKNOWN;
    diagnostic->observed.source_status = SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    diagnostic->source.status = SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
}

static const TypeTreeValue *array_child(const TypeTreeValue *value, const char *name) {
    return typetree_get_array(typetree_find_child(value, name));
}

static bool projected_text_matches(const char *text, const TypeTreeValue *value) {
    return text && value && value->type == VAL_TYPE_STRING && value->string_val &&
        strlen(text) == value->string_length &&
        memcmp(text, value->string_val, value->string_length) == 0;
}

/* The shared profile validator supplies the complete ordered value shape.
 * These checks inspect the dependency boundary and its decoded projection;
 * they do not resolve references or reinterpret serialized namespaces. */
static ShaderCatalogDependenciesStatus inspect_empty_scope(
    const ShaderObject *object, ShaderCatalogDependenciesSummary *summary) {
    if (!object->decoded ||
        !serialized_shader_profile_validate_value(&object->root, object->profile))
        return SHADER_CATALOG_DEPENDENCIES_INVALID_METADATA;
    const TypeTreeValue *parsed = typetree_find_child(&object->root, "m_ParsedForm");
    const TypeTreeValue *outer = array_child(&object->root, "m_Dependencies");
    const TypeTreeValue *textures = array_child(&object->root, "m_NonModifiableTextures");
    const TypeTreeValue *dependencies = array_child(parsed, "m_Dependencies");
    const TypeTreeValue *editors = array_child(parsed, "m_CustomEditorForRenderPipelines");
    const TypeTreeValue *subshaders = array_child(parsed, "m_SubShaders");
    const TypeTreeValue *fallback = typetree_find_child(parsed, "m_FallbackName");
    const TypeTreeValue *editor = typetree_find_child(parsed, "m_CustomEditorName");
    const SerializedShader *shader = &object->shader;
    if (!outer || !textures || !dependencies || !editors || !subshaders ||
        !fallback || !editor ||
        !projected_text_matches(shader->name, typetree_find_child(parsed, "m_Name")) ||
        !projected_text_matches(shader->fallback_name, fallback) ||
        !projected_text_matches(shader->custom_editor_name, editor) ||
        shader->dependency_count != dependencies->array_val.count ||
        shader->custom_editor_for_render_pipeline_count != editors->array_val.count ||
        shader->subshader_count != subshaders->array_val.count ||
        (shader->subshader_count && !shader->subshaders))
        return SHADER_CATALOG_DEPENDENCIES_INVALID_METADATA;
    summary->outer_reference_count = (size_t)outer->array_val.count;
    summary->nonmodifiable_texture_count = (size_t)textures->array_val.count;
    summary->parsed_dependency_count = (size_t)dependencies->array_val.count;
    if (outer->array_val.count || textures->array_val.count || dependencies->array_val.count ||
        editors->array_val.count || fallback->string_length || editor->string_length)
        return SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE;
    for (int subshader_index = 0; subshader_index < shader->subshader_count; ++subshader_index) {
        const SerializedSubShader *subshader = &shader->subshaders[subshader_index];
        const TypeTreeValue *passes = array_child(
            &subshaders->array_val.elements[subshader_index], "m_Passes");
        if (!passes || subshader->pass_count != passes->array_val.count ||
            (subshader->pass_count && !subshader->passes))
            return SHADER_CATALOG_DEPENDENCIES_INVALID_METADATA;
        for (int pass_index = 0; pass_index < subshader->pass_count; ++pass_index) {
            const SerializedPass *pass = &subshader->passes[pass_index];
            const TypeTreeValue *raw = &passes->array_val.elements[pass_index];
            int64_t pass_type;
            if (!typetree_value_get_int(typetree_find_child(raw, "m_Type"), &pass_type) ||
                pass_type != pass->pass_type ||
                !projected_text_matches(pass->use_name, typetree_find_child(raw, "m_UseName")))
                return SHADER_CATALOG_DEPENDENCIES_INVALID_METADATA;
            if (pass_type != 0 || (pass->use_name && pass->use_name[0]))
                return SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE;
        }
    }
    return SHADER_CATALOG_DEPENDENCIES_OK;
}

static ShaderLabSourceQualityRequest source_request(const ShaderCatalogDependencies *owned) {
    ShaderLabSourceQualityRequest request = {0};
    request.shader = &owned->object.shader;
    request.archive = &owned->archive;
    request.object = &owned->object;
    return request;
}

static ShaderCatalogDependenciesStatus observe(
    const ShaderCatalogDependenciesInput *input, ShaderCatalogDependencies *owned,
    ShaderCatalogDependenciesDiagnostic *diagnostic) {
    diagnostic->object_status = shader_catalog_decode_object(
        input->catalog, input->record, input->registry, &owned->object, &owned->summary.release);
    diagnostic->observed = owned->summary;
    if (diagnostic->object_status != SHADER_CATALOG_OBJECT_OK)
        return finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE);
    diagnostic->schema_profile = typetree_schema_validate_known_profile(
        input->record->unity_version, strlen(input->record->unity_version), 48,
        owned->object.schema.type_hash, &owned->object.schema);
    if (diagnostic->schema_profile != TYPETREE_SCHEMA_PROFILE_VALID)
        return finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_SCHEMA_UNAVAILABLE);
    ShaderCatalogDependenciesStatus status = inspect_empty_scope(&owned->object, &owned->summary);
    diagnostic->observed = owned->summary;
    if (status != SHADER_CATALOG_DEPENDENCIES_OK) return finish(diagnostic, status);
    if (!typetree_value_digest(&owned->object.root, owned->root_digest))
        return finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_INVALID_METADATA);
    ShaderObjectStatus archive_status = shader_object_open_d3d11_archive(&owned->object, &owned->archive);
    diagnostic->archive_status = archive_status;
    owned->summary.source_status = SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    if (archive_status == SHADER_OBJECT_OK) {
        owned->has_d3d11_archive = true;
        ShaderLabSourceQualityRequest request = source_request(owned);
        owned->summary.source_status = shaderlab_source_quality_emit(
            &request, &owned->source, &owned->inventory, &diagnostic->source);
        if (owned->summary.source_status == SHADERLAB_SOURCE_QUALITY_OK) {
            owned->summary.source_status = shaderlab_source_quality_inventory_analyze(
                &request, &owned->source, &owned->inventory, &owned->summary.base_quality,
                &diagnostic->source);
            owned->summary.has_emitted_source =
                owned->summary.source_status == SHADERLAB_SOURCE_QUALITY_OK;
        }
        if (owned->summary.source_status != SHADERLAB_SOURCE_QUALITY_OK &&
            owned->summary.source_status != SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE) {
            diagnostic->observed = owned->summary;
            return finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_SOURCE_FAILED);
        }
    } else if (archive_status != SHADER_OBJECT_D3D11_PLATFORM_ABSENT) {
        diagnostic->observed = owned->summary;
        return finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_SOURCE_FAILED);
    }
    diagnostic->observed = owned->summary;
    return finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_OK);
}

static bool release_reports_equal(const ShaderCatalogObjectReport *a, const ShaderCatalogObjectReport *b) {
    return a->source_status == b->source_status && a->schema_status == b->schema_status &&
        a->object_status == b->object_status && a->source_matches == b->source_matches &&
        memcmp(a->payload_digest, b->payload_digest, COMMON_SHA256_DIGEST_SIZE) == 0 &&
        memcmp(a->schema_digest, b->schema_digest, COMMON_SHA256_DIGEST_SIZE) == 0 &&
        memcmp(a->source_artifact_digest, b->source_artifact_digest, COMMON_SHA256_DIGEST_SIZE) == 0 &&
        memcmp(a->release_digest, b->release_digest, COMMON_SHA256_DIGEST_SIZE) == 0;
}

static bool quality_equal(const ShaderLabSourceQualityResult *a, const ShaderLabSourceQualityResult *b) {
    if (a->classification != b->classification || a->reasons != b->reasons || a->gaps != b->gaps ||
        a->wrapper_complete != b->wrapper_complete || a->wrapper_receipt_count != b->wrapper_receipt_count ||
        a->linked_entry_count != b->linked_entry_count ||
        a->observed_stage_residual_total != b->observed_stage_residual_total ||
        a->observed_stage_unknown_provenance != b->observed_stage_unknown_provenance ||
        a->observed_stage_incomplete_units != b->observed_stage_incomplete_units ||
        a->required_external_include_root_count != b->required_external_include_root_count) return false;
    for (unsigned index = 0; index < 5; ++index)
        if (a->stage_class_counts[index] != b->stage_class_counts[index]) return false;
    return true;
}

static bool observations_equal(const ShaderCatalogDependencies *owned, const ShaderCatalogDependencies *current,
    const ShaderCatalogDependenciesInput *input) {
    const ShaderCatalogDependenciesSummary *a = &owned->summary, *b = &current->summary;
    if (!release_reports_equal(&a->release, &b->release) ||
        owned->object.path_id != current->object.path_id ||
        owned->object.byte_offset != current->object.byte_offset ||
        owned->object.byte_size != current->object.byte_size ||
        owned->object.source_type_index != current->object.source_type_index ||
        owned->object.serialized_file_version != current->object.serialized_file_version ||
        owned->object.target_platform != current->object.target_platform ||
        owned->object.profile != current->object.profile ||
        a->outer_reference_count != b->outer_reference_count ||
        a->nonmodifiable_texture_count != b->nonmodifiable_texture_count ||
        a->parsed_dependency_count != b->parsed_dependency_count ||
        a->has_emitted_source != b->has_emitted_source || a->source_status != b->source_status ||
        !quality_equal(&a->base_quality, &b->base_quality) ||
        owned->has_d3d11_archive != current->has_d3d11_archive ||
        memcmp(owned->root_digest, current->root_digest, COMMON_SHA256_DIGEST_SIZE) != 0)
        return false;
    uint8_t root_digest[COMMON_SHA256_DIGEST_SIZE], schema_digest[COMMON_SHA256_DIGEST_SIZE];
    ShaderCatalogDependenciesSummary scope = {0};
    if (typetree_schema_validate_known_profile(input->record->unity_version,
            strlen(input->record->unity_version), 48, owned->object.schema.type_hash,
            &owned->object.schema) != TYPETREE_SCHEMA_PROFILE_VALID ||
        !typetree_schema_shape_digest(&owned->object.schema, schema_digest) ||
        memcmp(schema_digest, a->release.schema_digest, sizeof(schema_digest)) != 0 ||
        inspect_empty_scope(&owned->object, &scope) != SHADER_CATALOG_DEPENDENCIES_OK ||
        !typetree_value_digest(&owned->object.root, root_digest) ||
        memcmp(root_digest, owned->root_digest, sizeof(root_digest)) != 0) return false;
    ReleaseShaderObjectCertificateReport comparison;
    if (release_shader_object_certify_equal(&owned->object, &current->object, NULL, &comparison) !=
        RELEASE_SHADER_OBJECT_CERTIFICATE_OK) return false;
    if (owned->has_d3d11_archive) {
        ReleaseShaderArchiveCompareReport archive;
        if (release_shader_archive_compare_canonical(&owned->archive, &current->archive, &archive) !=
            RELEASE_SHADER_ARCHIVE_EQUAL) return false;
    } else if (owned->archive.entries || owned->archive.entry_count || owned->archive.segments ||
        owned->archive.segment_lengths || owned->archive.segment_count || owned->archive.stage_count) return false;
    if (a->has_emitted_source) {
        ShaderLabSourceQualityResult quality;
        ShaderLabSourceQualityDiagnostic diagnostic;
        ShaderLabSourceQualityRequest original_request = source_request(owned);
        ShaderLabSourceQualityRequest current_request = source_request(current);
        if (shaderlab_source_quality_inventory_analyze(&original_request, &owned->source,
                &owned->inventory, &quality, &diagnostic) != SHADERLAB_SOURCE_QUALITY_OK ||
            !quality_equal(&quality, &a->base_quality) ||
            shaderlab_source_quality_inventory_analyze(&current_request, &owned->source,
                &owned->inventory, &quality, &diagnostic) != SHADERLAB_SOURCE_QUALITY_OK ||
            !quality_equal(&quality, &b->base_quality)) return false;
    } else if (owned->source.len || owned->source.buf || owned->inventory.complete ||
        owned->inventory.receipts || owned->inventory.receipt_count || owned->inventory.receipt_capacity ||
        owned->inventory.entries.records || owned->inventory.entries.count || owned->inventory.entries.capacity ||
        owned->inventory.source_size || owned->inventory.has_structural_authority) return false;
    return true;
}

static void initialize(ShaderCatalogDependencies *owned) {
    memset(owned, 0, sizeof(*owned));
    shader_object_init(&owned->object);
    sb_init(&owned->source);
    owned->summary.source_status = SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
}

static void dispose_contents(ShaderCatalogDependencies *owned) {
    shaderlab_source_quality_inventory_dispose(&owned->inventory);
    sb_free(&owned->source);
    shader_blob_archive_close(&owned->archive);
    shader_object_dispose(&owned->object);
}

static bool replay_current(const ShaderCatalogDependenciesInput *input, const ShaderCatalogDependencies *owned) {
    if (!input || !input->catalog || !input->record || !owned) return false;
    ShaderCatalogDependencies current;
    initialize(&current);
    ShaderCatalogDependenciesDiagnostic diagnostic;
    diagnostic_init(&diagnostic);
    bool matches = observe(input, &current, &diagnostic) == SHADER_CATALOG_DEPENDENCIES_OK &&
        observations_equal(owned, &current, input);
    if (matches) {
        uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
        UnityInputSnapshot *snapshot = shader_catalog_retained_snapshot(
            input->catalog, input->record->outer_path);
        matches = snapshot && unity_input_snapshot_digest(snapshot, digest) == UNITY_INPUT_OK &&
            memcmp(digest, current.summary.release.source_artifact_digest, sizeof(digest)) == 0;
    }
    dispose_contents(&current);
    return matches;
}

ShaderCatalogDependenciesStatus shader_catalog_dependencies_capture(
    const ShaderCatalogDependenciesInput *input, ShaderCatalogDependencies **output,
    ShaderCatalogDependenciesDiagnostic *diagnostic) {
    ShaderCatalogDependenciesDiagnostic local;
    if (!diagnostic) diagnostic = &local;
    diagnostic_init(diagnostic);
    if (!input || !input->catalog || !input->record || !output || *output)
        return diagnostic->status;
    ShaderCatalogDependencies *owned = malloc(sizeof(*owned));
    if (!owned) return finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_ALLOCATION_FAILED);
    initialize(owned);
    ShaderCatalogDependenciesStatus status = observe(input, owned, diagnostic);
    if (status == SHADER_CATALOG_DEPENDENCIES_OK && input->observer &&
        !input->observer(input->observer_context, &owned->summary))
        status = finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_OBSERVER_REJECTED);
    /* Recheck after the callback and all independent source observations.
     * Publication is not authorized by a lease checked only before them. */
    if (status == SHADER_CATALOG_DEPENDENCIES_OK && !replay_current(input, owned))
        status = finish(diagnostic, SHADER_CATALOG_DEPENDENCIES_REPLAY_MISMATCH);
    if (status != SHADER_CATALOG_DEPENDENCIES_OK) {
        shader_catalog_dependencies_free(owned);
        return status;
    }
    owned->sealed = true;
    *output = owned;
    return status;
}

bool shader_catalog_dependencies_replay(const ShaderCatalogDependenciesInput *current,
    const ShaderCatalogDependencies *owned) {
    return owned && owned->sealed && replay_current(current, owned);
}

bool shader_catalog_dependencies_describe(const ShaderCatalogDependencies *owned,
    ShaderCatalogDependenciesSummary *summary) {
    if (!owned || !owned->sealed || !summary) return false;
    *summary = owned->summary;
    return true;
}

void shader_catalog_dependencies_free(ShaderCatalogDependencies *owned) {
    if (!owned) return;
    dispose_contents(owned);
    free(owned);
}

const char *shader_catalog_dependencies_status_name(ShaderCatalogDependenciesStatus status) {
    switch (status) {
    case SHADER_CATALOG_DEPENDENCIES_OK: return "ok";
    case SHADER_CATALOG_DEPENDENCIES_INVALID_ARGUMENT: return "invalid-argument";
    case SHADER_CATALOG_DEPENDENCIES_ALLOCATION_FAILED: return "allocation-failed";
    case SHADER_CATALOG_DEPENDENCIES_OBJECT_UNAVAILABLE: return "object-unavailable";
    case SHADER_CATALOG_DEPENDENCIES_SCHEMA_UNAVAILABLE: return "schema-unavailable";
    case SHADER_CATALOG_DEPENDENCIES_INVALID_METADATA: return "invalid-metadata";
    case SHADER_CATALOG_DEPENDENCIES_GRAPH_UNAVAILABLE: return "graph-unavailable";
    case SHADER_CATALOG_DEPENDENCIES_SOURCE_FAILED: return "source-failed";
    case SHADER_CATALOG_DEPENDENCIES_REPLAY_MISMATCH: return "replay-mismatch";
    case SHADER_CATALOG_DEPENDENCIES_OBSERVER_REJECTED: return "observer-rejected";
    default: return "unknown";
    }
}
