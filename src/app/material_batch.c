// SPDX-License-Identifier: GPL-3.0-only

#include "app/material_batch.h"

#include "common/common.h"
#include "common/file_io.h"
#include "common/sha256.h"
#include "common/shader_artifact.h"
#include "common/string_builder.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    UnityMaterialYamlDocument document;
    UnityMaterialYamlString* valid_keywords;
    UnityMaterialYamlString* invalid_keywords;
    UnityMaterialYamlStringPair* string_tags;
    UnityMaterialYamlString* disabled_shader_passes;
    UnityMaterialYamlTextureProperty* textures;
    UnityMaterialYamlIntProperty* ints;
    UnityMaterialYamlFloatProperty* floats;
    UnityMaterialYamlColorProperty* colors;
    UnityMaterialYamlBuildTextureStack* texture_stacks;
} MaterialYamlProjection;

typedef struct {
    CommonFileView shader;
    CommonFileView meta;
    bool shader_open;
    bool meta_open;
} PublishedShaderDependencyViews;

static bool open_digest_matched_view(
    const char* path, size_t expected_size,
    const uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE],
    CommonFileView* view) {
    CommonFileStatus status = common_file_view_open_regular(
        path, expected_size, view);
    if (status != COMMON_FILE_OK || view->size != expected_size) {
        if (status == COMMON_FILE_OK) (void)common_file_view_close(view);
        return false;
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    if (!common_file_view_sha256(view, digest) ||
        memcmp(digest, expected_digest, sizeof(digest)) != 0) {
        (void)common_file_view_close(view);
        return false;
    }
    return true;
}

static bool open_published_shader_dependencies(
    const ShaderBatchRecordResult* shader,
    const char expected_guid[UNITY_ASSET_GUID_TEXT_CAPACITY],
    PublishedShaderDependencyViews* views) {
    if (!shader || !expected_guid || !views ||
        strcmp(shader->asset_guid, expected_guid) != 0 ||
        !shader->published_shader_content_recorded ||
        !shader->published_meta_content_recorded) {
        return false;
    }
    memset(views, 0, sizeof(*views));
    views->shader_open = open_digest_matched_view(
        shader->output_path, shader->published_shader_size,
        shader->published_shader_digest, &views->shader);
    if (!views->shader_open) return false;
    views->meta_open = open_digest_matched_view(
        shader->output_meta_path, shader->published_meta_size,
        shader->published_meta_digest, &views->meta);
    if (!views->meta_open) {
        (void)common_file_view_close(&views->shader);
        views->shader_open = false;
        return false;
    }
    StringBuilder expected_meta;
    sb_init(&expected_meta);
    bool meta_matches = unity_shader_importer_meta_emit(
                            expected_guid, &expected_meta) ==
                            UNITY_MATERIAL_YAML_OK &&
        sb_ok(&expected_meta) && expected_meta.len == views->meta.size &&
        (expected_meta.len == 0U ||
         memcmp(expected_meta.buf, views->meta.data,
                expected_meta.len) == 0);
    sb_free(&expected_meta);
    if (!meta_matches) {
        (void)common_file_view_close(&views->meta);
        (void)common_file_view_close(&views->shader);
        memset(views, 0, sizeof(*views));
        return false;
    }
    return true;
}

static bool shader_batch_record_matches_catalog(
    const ShaderCatalog* catalog, const ShaderBatchResult* batch,
    size_t record_index,
    char expected_guid[UNITY_ASSET_GUID_TEXT_CAPACITY]) {
    if (!catalog || !batch || !expected_guid ||
        batch->catalog_authority != catalog ||
        record_index >= catalog->record_count ||
        record_index >= batch->record_count ||
        !catalog->records || !batch->records) {
        return false;
    }
    const ShaderCatalogRecord* catalog_record =
        &catalog->records[record_index];
    const ShaderBatchRecordResult* batch_record =
        &batch->records[record_index];
    if (catalog_record->class_id != 48 ||
        !batch_record->catalog_provenance_recorded ||
        batch_record->catalog_path_id != catalog_record->path_id ||
        batch_record->catalog_class_id != catalog_record->class_id ||
        memcmp(batch_record->catalog_serialized_digest,
               catalog_record->serialized_digest,
               COMMON_SHA256_DIGEST_SIZE) != 0) {
        return false;
    }
    uint8_t identity[COMMON_SHA256_DIGEST_SIZE + 8U];
    memcpy(identity, catalog_record->serialized_digest,
           COMMON_SHA256_DIGEST_SIZE);
    uint64_t path_bits = (uint64_t)catalog_record->path_id;
    for (unsigned index = 0U; index < 8U; ++index) {
        identity[COMMON_SHA256_DIGEST_SIZE + index] =
            (uint8_t)(path_bits >> (index * 8U));
    }
    return unity_asset_guid_derive(
        UNITY_ASSET_GUID_DOMAIN_SHADER, identity, sizeof(identity),
        expected_guid);
}

static bool close_published_shader_dependencies(
    PublishedShaderDependencyViews* views) {
    if (!views) return false;
    bool ok = true;
    if (views->meta_open &&
        common_file_view_close(&views->meta) != COMMON_FILE_OK) {
        ok = false;
    }
    if (views->shader_open &&
        common_file_view_close(&views->shader) != COMMON_FILE_OK) {
        ok = false;
    }
    memset(views, 0, sizeof(*views));
    return ok;
}

static bool close_texture_publication_leases(
    const MaterialBatchOptions* options, void** leases, size_t count) {
    if (!leases && count != 0U) return false;
    bool ok = true;
    for (size_t index = 0U; index < count; ++index) {
        if (!leases[index]) continue;
        if (!options ||
            !options->close_texture_reference_publication_lease ||
            !options->close_texture_reference_publication_lease(
                leases[index])) {
            ok = false;
        }
        leases[index] = NULL;
    }
    return ok;
}

static void material_yaml_projection_init(MaterialYamlProjection* projection) {
    if (projection) memset(projection, 0, sizeof(*projection));
}

static void material_yaml_projection_dispose(
    MaterialYamlProjection* projection) {
    if (!projection) return;
    free(projection->valid_keywords);
    free(projection->invalid_keywords);
    free(projection->string_tags);
    free(projection->disabled_shader_passes);
    free(projection->textures);
    free(projection->ints);
    free(projection->floats);
    free(projection->colors);
    free(projection->texture_stacks);
    material_yaml_projection_init(projection);
}

static void* allocate_array(size_t count, size_t element_size) {
    if (count == 0U) return NULL;
    if (dxbc_size_multiply_overflows(count, element_size)) return NULL;
    return calloc(count, element_size);
}

static UnityMaterialYamlString yaml_string(MaterialStringView value) {
    UnityMaterialYamlString result;
    result.bytes = value.bytes;
    result.size = value.size;
    return result;
}

static bool material_yaml_projection_build(
    const MaterialObject* object, const char* shader_guid,
    MaterialYamlProjection* projection) {
    if (!object || !object->decoded || !shader_guid || !projection) {
        return false;
    }
    material_yaml_projection_init(projection);

#define ALLOCATE_PROJECTION(member, count)                                      \
    do {                                                                        \
        if ((count) != 0U) {                                                    \
            projection->member = allocate_array(                               \
                (count), sizeof(*projection->member));                          \
            if (!projection->member) goto allocation_failed;                   \
        }                                                                       \
    } while (0)

    ALLOCATE_PROJECTION(valid_keywords, object->valid_keyword_count);
    ALLOCATE_PROJECTION(invalid_keywords, object->invalid_keyword_count);
    ALLOCATE_PROJECTION(string_tags, object->string_tag_count);
    ALLOCATE_PROJECTION(disabled_shader_passes,
                        object->disabled_shader_pass_count);
    ALLOCATE_PROJECTION(textures, object->texture_property_count);
    ALLOCATE_PROJECTION(ints, object->int_property_count);
    ALLOCATE_PROJECTION(floats, object->float_property_count);
    ALLOCATE_PROJECTION(colors, object->color_property_count);
    ALLOCATE_PROJECTION(texture_stacks, object->texture_stack_count);
#undef ALLOCATE_PROJECTION

    for (size_t index = 0U; index < object->valid_keyword_count; ++index) {
        projection->valid_keywords[index] =
            yaml_string(object->valid_keywords[index]);
    }
    for (size_t index = 0U; index < object->invalid_keyword_count; ++index) {
        projection->invalid_keywords[index] =
            yaml_string(object->invalid_keywords[index]);
    }
    for (size_t index = 0U; index < object->string_tag_count; ++index) {
        projection->string_tags[index].key =
            yaml_string(object->string_tags[index].key);
        projection->string_tags[index].value =
            yaml_string(object->string_tags[index].value);
    }
    for (size_t index = 0U;
         index < object->disabled_shader_pass_count; ++index) {
        projection->disabled_shader_passes[index] =
            yaml_string(object->disabled_shader_passes[index]);
    }
    for (size_t index = 0U; index < object->texture_property_count; ++index) {
        const MaterialTextureProperty* source =
            &object->texture_properties[index];
        UnityMaterialYamlTextureProperty* target =
            &projection->textures[index];
        target->name = yaml_string(source->name);
        target->scale_x_bits = source->scale.x.bits;
        target->scale_y_bits = source->scale.y.bits;
        target->offset_x_bits = source->offset.x.bits;
        target->offset_y_bits = source->offset.y.bits;
    }
    for (size_t index = 0U; index < object->int_property_count; ++index) {
        projection->ints[index].name =
            yaml_string(object->int_properties[index].name);
        projection->ints[index].value = object->int_properties[index].value;
    }
    for (size_t index = 0U; index < object->float_property_count; ++index) {
        projection->floats[index].name =
            yaml_string(object->float_properties[index].name);
        projection->floats[index].value_bits =
            object->float_properties[index].value.bits;
    }
    for (size_t index = 0U; index < object->color_property_count; ++index) {
        projection->colors[index].name =
            yaml_string(object->color_properties[index].name);
        projection->colors[index].red_bits =
            object->color_properties[index].value.r.bits;
        projection->colors[index].green_bits =
            object->color_properties[index].value.g.bits;
        projection->colors[index].blue_bits =
            object->color_properties[index].value.b.bits;
        projection->colors[index].alpha_bits =
            object->color_properties[index].value.a.bits;
    }
    for (size_t index = 0U; index < object->texture_stack_count; ++index) {
        projection->texture_stacks[index].group_name =
            yaml_string(object->texture_stacks[index].group_name);
        projection->texture_stacks[index].item_name =
            yaml_string(object->texture_stacks[index].item_name);
    }

    projection->document.name = yaml_string(object->name);
    projection->document.shader.is_null = false;
    projection->document.shader.file_id = 4800000;
    projection->document.shader.guid = shader_guid;
    projection->document.shader.type = 3;
    projection->document.valid_keywords = projection->valid_keywords;
    projection->document.valid_keyword_count = object->valid_keyword_count;
    projection->document.invalid_keywords = projection->invalid_keywords;
    projection->document.invalid_keyword_count = object->invalid_keyword_count;
    projection->document.lightmap_flags = object->lightmap_flags;
    projection->document.enable_instancing_variants =
        object->enable_instancing_variants;
    projection->document.double_sided_gi = object->double_sided_gi;
    projection->document.custom_render_queue = object->custom_render_queue;
    projection->document.string_tags = projection->string_tags;
    projection->document.string_tag_count = object->string_tag_count;
    projection->document.disabled_shader_passes =
        projection->disabled_shader_passes;
    projection->document.disabled_shader_pass_count =
        object->disabled_shader_pass_count;
    projection->document.textures = projection->textures;
    projection->document.texture_count = object->texture_property_count;
    projection->document.ints = projection->ints;
    projection->document.int_count = object->int_property_count;
    projection->document.floats = projection->floats;
    projection->document.float_count = object->float_property_count;
    projection->document.colors = projection->colors;
    projection->document.color_count = object->color_property_count;
    projection->document.build_texture_stacks = projection->texture_stacks;
    projection->document.build_texture_stack_count =
        object->texture_stack_count;
    return true;

allocation_failed:
    material_yaml_projection_dispose(projection);
    return false;
}

void material_batch_options_default(MaterialBatchOptions* options) {
    if (options) {
        memset(options, 0, sizeof(*options));
        options->dependency_authority_complete = true;
    }
}

void material_batch_texture_reference_init(
    MaterialBatchTextureReference* reference) {
    if (reference) memset(reference, 0, sizeof(*reference));
}

void material_batch_result_init(MaterialBatchResult* result) {
    if (result) memset(result, 0, sizeof(*result));
}

void material_batch_result_dispose(MaterialBatchResult* result) {
    if (!result) return;
    for (size_t record_index = 0U;
         record_index < result->record_count; ++record_index) {
        MaterialBatchRecordResult* record = &result->records[record_index];
        for (size_t dependency_index = 0U;
             dependency_index < record->dependency_count;
             ++dependency_index) {
            free(record->dependencies[dependency_index].property_name);
        }
        free(record->dependencies);
        free(record->output_path);
        free(record->output_meta_path);
        free(record->dependency_evidence_path);
    }
    free(result->records);
    material_batch_result_init(result);
}

static void material_record_fail(MaterialBatchResult* result, size_t index,
                                 MaterialBatchFailure failure) {
    MaterialBatchRecordResult* record = &result->records[index];
    if (record->status != MATERIAL_BATCH_FAILED) {
        record->status = MATERIAL_BATCH_FAILED;
        ++result->stats.failed;
    }
    if (record->failure == MATERIAL_BATCH_FAILURE_NONE) {
        record->failure = failure;
    }
}

static bool copy_property_name(MaterialBatchTextureDependency* dependency,
                               MaterialStringView name) {
    if (!dependency || (!name.bytes && name.size != 0U)) return false;
    dependency->property_name_size = name.size;
    if (name.size == 0U) return true;
    dependency->property_name = (uint8_t*)malloc(name.size);
    if (!dependency->property_name) return false;
    memcpy(dependency->property_name, name.bytes, name.size);
    return true;
}

static void record_external_evidence(
    MaterialBatchTextureDependency* dependency,
    const UnityPPtrResolveResult* resolution) {
    dependency->external_index = resolution->external_index;
    if (!resolution->external) return;
    dependency->has_external_serialized_guid = true;
    memcpy(dependency->external_serialized_guid,
           resolution->external->guid,
           sizeof(dependency->external_serialized_guid));
    dependency->external_serialized_type = resolution->external->type;
}

static MaterialBatchFailure project_texture_dependencies(
    const ShaderCatalog* catalog, size_t material_index,
    const ShaderCatalogPPtrGraph* resolver_graph,
    const MaterialBatchOptions* options, MaterialYamlProjection* projection,
    void** publication_leases, MaterialBatchResult* result) {
    const ShaderCatalogMaterialRecord* material =
        &catalog->materials[material_index];
    const MaterialObject* object = &material->object;
    MaterialBatchRecordResult* record = &result->records[material_index];
    record->texture_dependency_closure_complete = true;

    if (object->texture_property_count != 0U) {
        record->dependencies = allocate_array(
            object->texture_property_count, sizeof(*record->dependencies));
        if (!record->dependencies) {
            record->texture_dependency_closure_complete = false;
            return MATERIAL_BATCH_FAILURE_PROJECTION_ALLOCATION;
        }
    }
    record->dependency_count = object->texture_property_count;

    MaterialBatchFailure first_failure = MATERIAL_BATCH_FAILURE_NONE;
    for (size_t index = 0U; index < object->texture_property_count; ++index) {
        const MaterialTextureProperty* property =
            &object->texture_properties[index];
        MaterialBatchTextureDependency* dependency =
            &record->dependencies[index];
        UnityMaterialYamlTextureProperty* yaml_texture =
            &projection->textures[index];
        dependency->property_index = index;
        dependency->serialized_file_id = property->texture.file_id;
        dependency->serialized_path_id = property->texture.path_id;
        dependency->source_index = material->source_index;
        dependency->target_source_index = SIZE_MAX;
        dependency->external_index = SIZE_MAX;
        dependency->resolve_status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        dependency->status = MATERIAL_BATCH_DEPENDENCY_RESOLUTION_FAILED;
        dependency->reference_status =
            MATERIAL_BATCH_TEXTURE_REFERENCE_AUTHORITY_MISSING;

        ++result->stats.texture_dependencies;
        if (!copy_property_name(dependency, property->name)) {
            if (first_failure == MATERIAL_BATCH_FAILURE_NONE) {
                first_failure = MATERIAL_BATCH_FAILURE_PROJECTION_ALLOCATION;
            }
            ++result->stats.failed_texture_dependencies;
            record->texture_dependency_closure_complete = false;
            continue;
        }
        UnityPPtrResolveResult resolution;
        unity_pptr_resolve_result_init(&resolution);
        bool resolved = unity_pptr_resolve(
            &resolver_graph->graph, material->source_index,
            &property->texture, UNITY_PPTR_TARGET_CLASS_ANY, &resolution);
        dependency->resolve_status = resolution.status;
        record_external_evidence(dependency, &resolution);

        if (resolved && resolution.status == UNITY_PPTR_RESOLVE_NULL) {
            dependency->status = MATERIAL_BATCH_DEPENDENCY_NULL;
            dependency->reference_status = MATERIAL_BATCH_TEXTURE_REFERENCE_OK;
            yaml_texture->texture.is_null = true;
            ++result->stats.null_texture_dependencies;
            continue;
        }
        if (!resolved ||
            !unity_pptr_resolve_status_is_success(resolution.status) ||
            !resolution.object ||
            resolution.target_index >= catalog->source_count) {
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_RESOLUTION_FAILED;
            ++result->stats.failed_texture_dependencies;
            record->texture_dependency_closure_complete = false;
            if (first_failure == MATERIAL_BATCH_FAILURE_NONE) {
                first_failure = MATERIAL_BATCH_FAILURE_TEXTURE_RESOLUTION;
            }
            continue;
        }

        dependency->has_target = true;
        dependency->target_source_index = resolution.target_index;
        dependency->target_class_id = resolution.object->type_id;
        dependency->target_path_id = resolution.object->path_id;
        memcpy(dependency->target_serialized_digest_hex,
               catalog->sources[resolution.target_index].serialized_digest_hex,
               sizeof(dependency->target_serialized_digest_hex));
        ++result->stats.resolved_texture_dependencies;

        if (!options->resolve_texture_reference) {
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_REFERENCE_AUTHORITY_MISSING;
            dependency->reference_status =
                MATERIAL_BATCH_TEXTURE_REFERENCE_AUTHORITY_MISSING;
            ++result->stats.failed_texture_dependencies;
            record->texture_dependency_closure_complete = false;
            if (first_failure == MATERIAL_BATCH_FAILURE_NONE) {
                first_failure =
                    MATERIAL_BATCH_FAILURE_TEXTURE_REFERENCE_AUTHORITY_MISSING;
            }
            continue;
        }

        MaterialBatchTextureReference reference;
        material_batch_texture_reference_init(&reference);
        MaterialBatchTextureReferenceRequest request;
        memset(&request, 0, sizeof(request));
        request.catalog = catalog;
        request.material_index = material_index;
        request.texture_property_index = index;
        request.source_index = material->source_index;
        request.serialized_pointer = &property->texture;
        request.resolution = &resolution;
        request.target_source =
            &catalog->sources[resolution.target_index];
        request.target_object = resolution.object;
        dependency->reference_status = options->resolve_texture_reference(
            &request, options->texture_reference_context, &reference);

        if (reference.publication_lease &&
            dependency->reference_status !=
                MATERIAL_BATCH_TEXTURE_REFERENCE_OK) {
            if (!options->close_texture_reference_publication_lease ||
                !options->close_texture_reference_publication_lease(
                    reference.publication_lease)) {
                dependency->reference_status =
                    MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED;
            }
            reference.publication_lease = NULL;
        }

        if (dependency->reference_status ==
            MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_CLASS_UNSUPPORTED) {
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_CLASS_UNSUPPORTED;
            ++result->stats.failed_texture_dependencies;
            record->texture_dependency_closure_complete = false;
            if (first_failure == MATERIAL_BATCH_FAILURE_NONE) {
                first_failure =
                    MATERIAL_BATCH_FAILURE_TEXTURE_CLASS_UNSUPPORTED;
            }
            continue;
        }
        if (dependency->reference_status ==
            MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED) {
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_TARGET_ASSET_UNEXPORTED;
            ++result->stats.unexported_texture_dependencies;
            ++result->stats.failed_texture_dependencies;
            record->texture_dependency_closure_complete = false;
            if (first_failure == MATERIAL_BATCH_FAILURE_NONE) {
                first_failure =
                    MATERIAL_BATCH_FAILURE_TEXTURE_TARGET_ASSET_UNEXPORTED;
            }
            continue;
        }
        if (dependency->reference_status ==
            MATERIAL_BATCH_TEXTURE_REFERENCE_AUTHORITY_MISSING) {
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_REFERENCE_AUTHORITY_MISSING;
            ++result->stats.failed_texture_dependencies;
            record->texture_dependency_closure_complete = false;
            if (first_failure == MATERIAL_BATCH_FAILURE_NONE) {
                first_failure =
                    MATERIAL_BATCH_FAILURE_TEXTURE_REFERENCE_AUTHORITY_MISSING;
            }
            continue;
        }
        if (dependency->reference_status !=
                MATERIAL_BATCH_TEXTURE_REFERENCE_OK ||
            reference.file_id == 0 ||
            !unity_asset_guid_is_valid(reference.guid) ||
            reference.type < 0 || reference.type > 3 ||
            (reference.asset_exported &&
             (!reference.publication_lease ||
              !options->close_texture_reference_publication_lease)) ||
            (!reference.asset_exported && reference.publication_lease)) {
            if (reference.publication_lease &&
                options->close_texture_reference_publication_lease) {
                (void)options->close_texture_reference_publication_lease(
                    reference.publication_lease);
                reference.publication_lease = NULL;
            }
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_REFERENCE_INVALID;
            dependency->reference_status =
                MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED;
            ++result->stats.failed_texture_dependencies;
            record->texture_dependency_closure_complete = false;
            if (first_failure == MATERIAL_BATCH_FAILURE_NONE) {
                first_failure =
                    MATERIAL_BATCH_FAILURE_TEXTURE_REFERENCE_INVALID;
            }
            continue;
        }

        dependency->has_yaml_reference = true;
        if (reference.asset_exported) {
            publication_leases[index] = reference.publication_lease;
            reference.publication_lease = NULL;
        }
        dependency->yaml_reference = reference;
        yaml_texture->texture.is_null = false;
        yaml_texture->texture.file_id = reference.file_id;
        yaml_texture->texture.guid = dependency->yaml_reference.guid;
        yaml_texture->texture.type = reference.type;
        if (reference.asset_exported) {
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED;
            ++result->stats.exported_texture_dependencies;
        } else {
            dependency->status =
                MATERIAL_BATCH_DEPENDENCY_RESOLVED_NOT_EXPORTED;
            ++result->stats.unexported_texture_dependencies;
            record->texture_dependency_closure_complete = false;
        }
    }
    return first_failure;
}

static void append_json_bytes(StringBuilder* output, const uint8_t* bytes,
                              size_t size) {
    static const char hex[] = "0123456789abcdef";
    if ((!bytes && size != 0U) || !output) {
        if (output) output->failed = true;
        return;
    }
    sb_append_char(output, '"');
    for (size_t index = 0U; index < size && sb_ok(output); ++index) {
        uint8_t value = bytes[index];
        if (value == '"' || value == '\\') {
            sb_append_char(output, '\\');
            sb_append_char(output, (char)value);
        } else if (value >= 0x20U && value != 0x7fU) {
            sb_append_char(output, (char)value);
        } else {
            char escaped[7] = {
                '\\', 'u', '0', '0',
                hex[value >> 4U], hex[value & 0x0fU], '\0'};
            sb_append(output, escaped);
        }
    }
    sb_append_char(output, '"');
}

static void append_raw_guid_hex(StringBuilder* output,
                                const uint8_t guid[16]) {
    static const char hex[] = "0123456789abcdef";
    sb_append_char(output, '"');
    for (size_t index = 0U; index < 16U; ++index) {
        sb_append_char(output, hex[guid[index] >> 4U]);
        sb_append_char(output, hex[guid[index] & 0x0fU]);
    }
    sb_append_char(output, '"');
}

static bool append_dependency_evidence(
    StringBuilder* output, const ShaderCatalog* catalog,
    size_t material_index, const ShaderBatchResult* shader_batch,
    const MaterialBatchRecordResult* result) {
    const ShaderCatalogMaterialRecord* material =
        &catalog->materials[material_index];
    const ShaderCatalogRecord* shader =
        &catalog->records[result->shader_record_index];
    const ShaderBatchRecordResult* shader_output =
        &shader_batch->records[result->shader_record_index];
    if (!output) return false;

    sb_append(output,
        "{\n  \"format\": \"DXBCSandbox.material-dependencies.v2\",\n"
        "  \"material\": {\"serialized_digest\": \"");
    sb_append(output, material->serialized_digest_hex);
    sb_appendf(output, "\", \"path_id\": %" PRId64
               ", \"asset_guid\": \"%s\"},\n",
               material->path_id, result->asset_guid);
    sb_append(output, "  \"shader\": {\"serialized_digest\": \"");
    sb_appendf(output, "%s\", "
               "\"path_id\": %" PRId64 ", \"asset_guid\": \"%s\"},\n",
               shader->serialized_digest_hex, shader->path_id,
               shader_output->asset_guid);
    sb_append(output, "  \"texture_dependency_closure_complete\": ");
    sb_append(output, result->texture_dependency_closure_complete
                          ? "true,\n" : "false,\n");
    sb_append(output, "  \"dependencies\": [");
    if (result->dependency_count != 0U) sb_append_char(output, '\n');
    for (size_t index = 0U; index < result->dependency_count; ++index) {
        const MaterialBatchTextureDependency* dependency =
            &result->dependencies[index];
        sb_append(output, "    {\"property_index\": ");
        sb_appendf(output, "%zu, \"property_name\": ",
                   dependency->property_index);
        append_json_bytes(output, dependency->property_name,
                          dependency->property_name_size);
        sb_appendf(output,
            ", \"serialized_pointer\": {\"file_id\": %" PRId32
            ", \"path_id\": %" PRId64 "}, \"resolve_status\": \"%s\", "
            "\"status\": \"%s\", \"reference_status\": \"%s\", "
            "\"external_serialized_guid_raw\": ",
            dependency->serialized_file_id,
            dependency->serialized_path_id,
            unity_pptr_resolve_status_name(dependency->resolve_status),
            material_batch_dependency_status_name(dependency->status),
            material_batch_texture_reference_status_name(
                dependency->reference_status));
        if (dependency->has_external_serialized_guid) {
            append_raw_guid_hex(output,
                                dependency->external_serialized_guid);
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ", \"external_serialized_type\": ");
        if (dependency->has_external_serialized_guid) {
            sb_appendf(output, "%" PRId32,
                       dependency->external_serialized_type);
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ", \"target\": ");
        if (dependency->has_target) {
            sb_appendf(output,
                "{\"serialized_digest\": \"%s\", "
                "\"class_id\": %" PRId32 ", \"path_id\": %" PRId64 "}",
                dependency->target_serialized_digest_hex,
                dependency->target_class_id, dependency->target_path_id);
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ", \"yaml_reference\": ");
        if (dependency->has_yaml_reference) {
            sb_appendf(output,
                "{\"file_id\": %" PRId64 ", \"guid\": \"%s\", "
                "\"type\": %" PRId32 ", \"asset_exported\": %s}",
                dependency->yaml_reference.file_id,
                dependency->yaml_reference.guid,
                dependency->yaml_reference.type,
                dependency->yaml_reference.asset_exported
                    ? "true" : "false");
        } else {
            sb_append(output, "null");
        }
        sb_append(output, index + 1U == result->dependency_count
                              ? "}\n" : "},\n");
    }
    sb_append(output, "  ]\n}\n");
    return sb_ok(output);
}

static char* append_path_suffix(const char* path, const char* suffix) {
    if (!path || !suffix) return NULL;
    size_t path_size = strlen(path);
    size_t suffix_size = strlen(suffix);
    if (path_size > SIZE_MAX - suffix_size - 1U) return NULL;
    char* output = (char*)malloc(path_size + suffix_size + 1U);
    if (!output) return NULL;
    memcpy(output, path, path_size);
    memcpy(output + path_size, suffix, suffix_size + 1U);
    return output;
}

static MaterialBatchFailure publish_material(
    const ShaderCatalog* catalog, size_t material_index,
    const ShaderBatchResult* shader_batch, const char* output_directory,
    MaterialYamlProjection* projection, MaterialBatchRecordResult* result) {
    const ShaderCatalogMaterialRecord* material =
        &catalog->materials[material_index];
    const ShaderCatalogRecord* shader =
        &catalog->records[result->shader_record_index];

    StringBuilder material_yaml;
    StringBuilder material_meta;
    StringBuilder evidence;
    sb_init_with_capacity(&material_yaml, 16384U);
    sb_init(&material_meta);
    sb_init_with_capacity(&evidence, 4096U);
    result->yaml_status = unity_material_yaml_emit(
        &projection->document, &material_yaml);
    if (result->yaml_status != UNITY_MATERIAL_YAML_OK ||
        !sb_ok(&material_yaml)) {
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_MATERIAL_YAML;
    }

    /* A player Material's emitted bytes include its ordered resolved texture
     * GUID/fileID/type tuples.  Bind those final bytes into both identity and
     * filename so equal SerializedFiles with different external .resS slices
     * cannot collide at one .mat path. */
    uint8_t yaml_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(material_yaml.buf, material_yaml.len, yaml_digest);
    uint8_t identity[COMMON_SHA256_DIGEST_SIZE + 8U +
                     COMMON_SHA256_DIGEST_SIZE];
    memcpy(identity, material->serialized_digest, COMMON_SHA256_DIGEST_SIZE);
    uint64_t path_bits = (uint64_t)material->path_id;
    for (unsigned index = 0U; index < 8U; ++index) {
        identity[COMMON_SHA256_DIGEST_SIZE + index] =
            (uint8_t)(path_bits >> (index * 8U));
    }
    memcpy(identity + COMMON_SHA256_DIGEST_SIZE + 8U, yaml_digest,
           sizeof(yaml_digest));
    uint8_t artifact_identity[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(identity, sizeof(identity), artifact_identity);
    common_sha256_digest_to_hex(artifact_identity,
                                result->artifact_identity_hex);
    if (!unity_asset_guid_derive(
            UNITY_ASSET_GUID_DOMAIN_MATERIAL, identity, sizeof(identity),
            result->asset_guid)) {
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_MATERIAL_GUID;
    }
    if (unity_material_native_meta_emit(result->asset_guid, &material_meta) !=
            UNITY_MATERIAL_YAML_OK ||
        !sb_ok(&material_meta)) {
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_MATERIAL_META;
    }

    char artifact[384];
    const char* artifact_name =
        material->name && material->name[0] ? material->name : "_";
    if (!unity_asset_artifact_filename(
            artifact, sizeof(artifact), "material", artifact_name,
            result->artifact_identity_hex, material->path_id, "mat")) {
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_OUTPUT_NAME;
    }
    char* digest_directory = common_output_join_path(
        output_directory, shader->serialized_digest_hex);
    if (!digest_directory ||
        !common_output_ensure_directory_tree(digest_directory)) {
        free(digest_directory);
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_OUTPUT_DIRECTORY;
    }
    char* material_path = common_output_join_path(
        digest_directory, artifact);
    free(digest_directory);
    char* meta_path = append_path_suffix(material_path, ".meta");
    char* evidence_path = append_path_suffix(
        material_path, ".dependencies.json");
    if (!material_path || !meta_path || !evidence_path) {
        free(material_path);
        free(meta_path);
        free(evidence_path);
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_OUTPUT_NAME;
    }

    bool evidence_ok = append_dependency_evidence(
        &evidence, catalog, material_index, shader_batch, result);
    if (!evidence_ok) {
        free(material_path);
        free(meta_path);
        free(evidence_path);
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_DEPENDENCY_EVIDENCE;
    }

    CommonOutputPreflightStatus material_preflight =
        common_output_preflight_exact(
            material_path, material_yaml.buf, material_yaml.len);
    CommonOutputPreflightStatus meta_preflight =
        common_output_preflight_exact(
            meta_path, material_meta.buf, material_meta.len);
    CommonOutputPreflightStatus evidence_preflight =
        common_output_preflight_exact(
            evidence_path, evidence.buf, evidence.len);
    if (material_preflight == COMMON_OUTPUT_PREFLIGHT_COLLISION ||
        meta_preflight == COMMON_OUTPUT_PREFLIGHT_COLLISION ||
        evidence_preflight == COMMON_OUTPUT_PREFLIGHT_COLLISION) {
        free(material_path);
        free(meta_path);
        free(evidence_path);
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_OUTPUT_COLLISION;
    }
    if (material_preflight == COMMON_OUTPUT_PREFLIGHT_IO_ERROR ||
        meta_preflight == COMMON_OUTPUT_PREFLIGHT_IO_ERROR ||
        evidence_preflight == COMMON_OUTPUT_PREFLIGHT_IO_ERROR) {
        free(material_path);
        free(meta_path);
        free(evidence_path);
        sb_free(&material_yaml);
        sb_free(&material_meta);
        sb_free(&evidence);
        return MATERIAL_BATCH_FAILURE_OUTPUT_IO;
    }

    /* Publish the importable .mat last.  A late failure can leave only its
     * inert metadata/evidence companions; no partially identified Material
     * is exposed to Unity. */
    result->meta_publish_attempted = true;
    result->meta_publish_status = common_output_publish_exact(
        meta_path, material_meta.buf, material_meta.len);
    bool collision = result->meta_publish_status ==
        COMMON_OUTPUT_PUBLISH_COLLISION;
    bool io_error = result->meta_publish_status ==
        COMMON_OUTPUT_PUBLISH_IO_ERROR;
    if (!collision && !io_error) {
        result->evidence_publish_attempted = true;
        result->evidence_publish_status = common_output_publish_exact(
            evidence_path, evidence.buf, evidence.len);
        collision = result->evidence_publish_status ==
            COMMON_OUTPUT_PUBLISH_COLLISION;
        io_error = result->evidence_publish_status ==
            COMMON_OUTPUT_PUBLISH_IO_ERROR;
    }
    if (!collision && !io_error) {
        result->material_publish_attempted = true;
        result->material_publish_status = common_output_publish_exact(
            material_path, material_yaml.buf, material_yaml.len);
        collision = result->material_publish_status ==
            COMMON_OUTPUT_PUBLISH_COLLISION;
        io_error = result->material_publish_status ==
            COMMON_OUTPUT_PUBLISH_IO_ERROR;
    }
    bool material_ambiguous = result->material_publish_attempted &&
        common_output_publish_exact_residue_possible(
        material_preflight, result->material_publish_status,
        material_path, material_yaml.buf, material_yaml.len);
    bool meta_ambiguous = result->meta_publish_attempted &&
        common_output_publish_exact_residue_possible(
        meta_preflight, result->meta_publish_status,
        meta_path, material_meta.buf, material_meta.len);
    bool evidence_ambiguous = result->evidence_publish_attempted &&
        common_output_publish_exact_residue_possible(
        evidence_preflight, result->evidence_publish_status,
        evidence_path, evidence.buf, evidence.len);
    sb_free(&material_yaml);
    sb_free(&material_meta);
    sb_free(&evidence);
    if (collision || io_error) {
        if ((result->material_publish_attempted &&
             result->material_publish_status ==
                 COMMON_OUTPUT_PUBLISH_EMITTED) || material_ambiguous) {
            result->output_path = material_path;
            material_path = NULL;
            result->publication_residue = true;
        }
        if ((result->meta_publish_attempted &&
             result->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED) ||
            meta_ambiguous) {
            result->output_meta_path = meta_path;
            meta_path = NULL;
            result->publication_residue = true;
        }
        if ((result->evidence_publish_attempted &&
             result->evidence_publish_status ==
                 COMMON_OUTPUT_PUBLISH_EMITTED) || evidence_ambiguous) {
            result->dependency_evidence_path = evidence_path;
            evidence_path = NULL;
            result->publication_residue = true;
        }
        free(material_path);
        free(meta_path);
        free(evidence_path);
        return collision ? MATERIAL_BATCH_FAILURE_OUTPUT_COLLISION
                         : MATERIAL_BATCH_FAILURE_OUTPUT_IO;
    }

    bool emitted =
        result->material_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        result->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
        result->evidence_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED;
    result->output_path = material_path;
    result->output_meta_path = meta_path;
    result->dependency_evidence_path = evidence_path;
    result->has_artifacts = true;
    result->status = emitted ? MATERIAL_BATCH_EMITTED
                             : MATERIAL_BATCH_UNCHANGED;
    return MATERIAL_BATCH_FAILURE_NONE;
}

static bool catalog_material_object_shapes_are_valid(
    const ShaderCatalog* catalog) {
    if (!catalog ||
        (catalog->material_count != 0U && !catalog->materials)) {
        return false;
    }
    for (size_t index = 0U; index < catalog->material_count; ++index) {
        const MaterialObject* object = &catalog->materials[index].object;
        if ((object->valid_keyword_count != 0U &&
             !object->valid_keywords) ||
            (object->invalid_keyword_count != 0U &&
             !object->invalid_keywords) ||
            (object->string_tag_count != 0U && !object->string_tags) ||
            (object->disabled_shader_pass_count != 0U &&
             !object->disabled_shader_passes) ||
            (object->texture_property_count != 0U &&
             !object->texture_properties) ||
            (object->int_property_count != 0U &&
             !object->int_properties) ||
            (object->float_property_count != 0U &&
             !object->float_properties) ||
            (object->color_property_count != 0U &&
             !object->color_properties) ||
            (object->texture_stack_count != 0U &&
             !object->texture_stacks)) {
            return false;
        }
    }
    return true;
}

static void publish_material_record(
    const ShaderCatalog* catalog, size_t material_index,
    const ShaderBatchResult* shader_batch,
    const ShaderBatchRecordResult* shader_output,
    const char* output_directory,
    const ShaderCatalogPPtrGraph* resolver_graph,
    const MaterialBatchOptions* options, MaterialBatchResult* result) {
    const ShaderCatalogMaterialRecord* material =
        &catalog->materials[material_index];
    MaterialBatchRecordResult* record = &result->records[material_index];
    MaterialYamlProjection projection;
    if (!material_yaml_projection_build(
            &material->object, shader_output->asset_guid, &projection)) {
        material_record_fail(
            result, material_index,
            MATERIAL_BATCH_FAILURE_PROJECTION_ALLOCATION);
        return;
    }

    const size_t texture_property_count =
        material->object.texture_property_count;
    void** texture_publication_leases = allocate_array(
        texture_property_count, sizeof(*texture_publication_leases));
    if (texture_property_count != 0U && !texture_publication_leases) {
        material_yaml_projection_dispose(&projection);
        material_record_fail(
            result, material_index,
            MATERIAL_BATCH_FAILURE_PROJECTION_ALLOCATION);
        return;
    }

    MaterialBatchFailure failure = project_texture_dependencies(
        catalog, material_index, resolver_graph, options, &projection,
        texture_publication_leases, result);
    if (failure == MATERIAL_BATCH_FAILURE_NONE) {
        failure = publish_material(
            catalog, material_index, shader_batch, output_directory,
            &projection, record);
    }

    const bool texture_dependencies_stable =
        close_texture_publication_leases(
            options, texture_publication_leases, texture_property_count);
    free(texture_publication_leases);
    material_yaml_projection_dispose(&projection);

    if (!texture_dependencies_stable) {
        if (record->material_publish_status ==
                COMMON_OUTPUT_PUBLISH_EMITTED ||
            record->meta_publish_status == COMMON_OUTPUT_PUBLISH_EMITTED ||
            record->evidence_publish_status ==
                COMMON_OUTPUT_PUBLISH_EMITTED) {
            record->publication_residue = true;
        }
        failure = MATERIAL_BATCH_FAILURE_TEXTURE_DEPENDENCY_IDENTITY;
    }
    if (failure != MATERIAL_BATCH_FAILURE_NONE) {
        material_record_fail(result, material_index, failure);
    }
}

MaterialBatchStatus material_batch_export(
    const ShaderCatalog* catalog, const bool* selected_shaders,
    const ShaderBatchResult* shader_batch, const char* output_directory,
    const MaterialBatchOptions* options, MaterialBatchResult* result) {
    if (!catalog || !catalog->materials_included ||
        (!catalog->materials && catalog->material_count != 0U) ||
        !catalog_material_object_shapes_are_valid(catalog) ||
        (!catalog->sources && catalog->source_count != 0U) ||
        (!catalog->records && catalog->record_count != 0U) ||
        (!selected_shaders && catalog->record_count != 0U) ||
        !shader_batch ||
        shader_batch->catalog_authority != catalog ||
        shader_batch->record_count != catalog->record_count ||
        (!shader_batch->records && shader_batch->record_count != 0U) ||
        !output_directory || output_directory[0] == '\0' || !result) {
        return MATERIAL_BATCH_INVALID_ARGUMENT;
    }
    if (catalog->material_count != 0U &&
        dxbc_size_multiply_overflows(
            catalog->material_count, sizeof(*result->records))) {
        return MATERIAL_BATCH_ALLOCATION_FAILED;
    }

    MaterialBatchOptions defaults;
    material_batch_options_default(&defaults);
    if (!options) options = &defaults;

    MaterialBatchResult pending;
    material_batch_result_init(&pending);
    pending.catalog_authority = catalog;
    pending.selection_authority = selected_shaders;
    pending.record_count = catalog->material_count;
    pending.stats.catalog_materials = catalog->material_count;
    if (pending.record_count != 0U) {
        pending.records = allocate_array(
            pending.record_count, sizeof(*pending.records));
        if (!pending.records) return MATERIAL_BATCH_ALLOCATION_FAILED;
    }
    for (size_t index = 0U; index < pending.record_count; ++index) {
        pending.records[index].shader_record_index = SIZE_MAX;
        pending.records[index].material_publish_status =
            COMMON_OUTPUT_PUBLISH_IO_ERROR;
        pending.records[index].meta_publish_status =
            COMMON_OUTPUT_PUBLISH_IO_ERROR;
        pending.records[index].evidence_publish_status =
            COMMON_OUTPUT_PUBLISH_IO_ERROR;
    }

    ShaderCatalogPPtrGraph resolver_graph;
    shader_catalog_pptr_graph_init(&resolver_graph);
    if (!shader_catalog_pptr_graph_build(catalog, &resolver_graph)) {
        material_batch_result_dispose(&pending);
        return MATERIAL_BATCH_RESOLVER_GRAPH_FAILED;
    }

    /* Generated ShaderLab candidates can be tens of megabytes and many
     * Materials commonly reference the same exact Shader.  CommonFileView
     * deliberately makes two verified reads on open and one on close.  Group
     * by the catalog shader identity so those reads bracket the complete
     * group once, while record/report order remains catalog order. */
    bool* grouped_materials = allocate_array(
        catalog->material_count, sizeof(*grouped_materials));
    size_t* shader_material_counts = allocate_array(
        catalog->record_count, sizeof(*shader_material_counts));
    size_t* shader_material_offsets =
        catalog->record_count == SIZE_MAX
            ? NULL
            : allocate_array(catalog->record_count + 1U,
                             sizeof(*shader_material_offsets));
    size_t* shader_material_cursors = allocate_array(
        catalog->record_count, sizeof(*shader_material_cursors));
    if ((catalog->material_count != 0U && !grouped_materials) ||
        (catalog->record_count != 0U &&
         (!shader_material_counts || !shader_material_cursors)) ||
        !shader_material_offsets) {
        free(grouped_materials);
        free(shader_material_counts);
        free(shader_material_offsets);
        free(shader_material_cursors);
        shader_catalog_pptr_graph_dispose(&resolver_graph);
        material_batch_result_dispose(&pending);
        return MATERIAL_BATCH_ALLOCATION_FAILED;
    }

    for (size_t index = 0U; index < catalog->material_count; ++index) {
        const ShaderCatalogMaterialRecord* material =
            &catalog->materials[index];
        MaterialBatchRecordResult* record = &pending.records[index];
        if (material->status == SHADER_CATALOG_MATERIAL_SHADER_NULL) {
            record->status = MATERIAL_BATCH_UNASSOCIATED;
            ++pending.stats.unassociated;
            continue;
        }
        if (material->status != SHADER_CATALOG_MATERIAL_READY ||
            !material->object.decoded) {
            material_record_fail(
                &pending, index,
                MATERIAL_BATCH_FAILURE_CATALOG_MATERIAL_NOT_READY);
            continue;
        }
        if (material->shader_record_index >= catalog->record_count) {
            material_record_fail(
                &pending, index,
                MATERIAL_BATCH_FAILURE_SHADER_INDEX_INVALID);
            continue;
        }
        record->shader_record_index = material->shader_record_index;
        if (!selected_shaders[record->shader_record_index]) {
            record->status = MATERIAL_BATCH_UNSELECTED;
            ++pending.stats.unselected;
            continue;
        }
        ++pending.stats.selected;
        if (!options->dependency_authority_complete) {
            material_record_fail(
                &pending, index,
                MATERIAL_BATCH_FAILURE_DEPENDENCY_AUTHORITY_INCOMPLETE);
            continue;
        }
        grouped_materials[index] = true;
        ++shader_material_counts[record->shader_record_index];
    }

    shader_material_offsets[0] = 0U;
    for (size_t shader_index = 0U;
         shader_index < catalog->record_count; ++shader_index) {
        if (shader_material_counts[shader_index] >
            catalog->material_count - shader_material_offsets[shader_index]) {
            free(grouped_materials);
            free(shader_material_counts);
            free(shader_material_offsets);
            free(shader_material_cursors);
            shader_catalog_pptr_graph_dispose(&resolver_graph);
            material_batch_result_dispose(&pending);
            return MATERIAL_BATCH_ALLOCATION_FAILED;
        }
        shader_material_offsets[shader_index + 1U] =
            shader_material_offsets[shader_index] +
            shader_material_counts[shader_index];
        shader_material_cursors[shader_index] =
            shader_material_offsets[shader_index];
    }
    const size_t grouped_material_count =
        shader_material_offsets[catalog->record_count];
    size_t* grouped_material_indices = allocate_array(
        grouped_material_count, sizeof(*grouped_material_indices));
    if (grouped_material_count != 0U && !grouped_material_indices) {
        free(grouped_materials);
        free(shader_material_counts);
        free(shader_material_offsets);
        free(shader_material_cursors);
        shader_catalog_pptr_graph_dispose(&resolver_graph);
        material_batch_result_dispose(&pending);
        return MATERIAL_BATCH_ALLOCATION_FAILED;
    }
    for (size_t material_index = 0U;
         material_index < catalog->material_count; ++material_index) {
        if (!grouped_materials[material_index]) continue;
        const size_t shader_index =
            pending.records[material_index].shader_record_index;
        grouped_material_indices[shader_material_cursors[shader_index]++] =
            material_index;
    }
    free(grouped_materials);
    free(shader_material_counts);
    free(shader_material_cursors);

    for (size_t shader_index = 0U;
         shader_index < catalog->record_count; ++shader_index) {
        const size_t group_begin = shader_material_offsets[shader_index];
        const size_t group_end = shader_material_offsets[shader_index + 1U];
        if (group_begin == group_end) continue;

        const ShaderBatchRecordResult* shader_output =
            &shader_batch->records[shader_index];
        MaterialBatchFailure group_failure = MATERIAL_BATCH_FAILURE_NONE;
        char expected_shader_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
        if (!shader_batch_record_matches_catalog(
                catalog, shader_batch, shader_index,
                expected_shader_guid)) {
            group_failure =
                MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY;
        } else if (!shader_output->publication_authorized ||
                   (shader_output->status != SHADER_BATCH_EMITTED &&
                    shader_output->status != SHADER_BATCH_UNCHANGED)) {
            group_failure = MATERIAL_BATCH_FAILURE_SHADER_BATCH_NOT_READY;
        } else if (!shader_output->has_asset_meta ||
                   !shader_output->output_path ||
                   !shader_output->output_meta_path ||
                   !unity_asset_guid_is_valid(shader_output->asset_guid)) {
            group_failure = MATERIAL_BATCH_FAILURE_SHADER_META_MISSING;
        }
        if (group_failure != MATERIAL_BATCH_FAILURE_NONE) {
            for (size_t cursor = group_begin; cursor < group_end; ++cursor) {
                material_record_fail(
                    &pending, grouped_material_indices[cursor],
                    group_failure);
            }
            continue;
        }

        PublishedShaderDependencyViews shader_dependency_views;
        if (!open_published_shader_dependencies(
                shader_output, expected_shader_guid,
                &shader_dependency_views)) {
            for (size_t cursor = group_begin; cursor < group_end; ++cursor) {
                material_record_fail(
                    &pending, grouped_material_indices[cursor],
                    MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY);
            }
            continue;
        }
        ++pending.stats.shader_dependency_groups;
        for (size_t cursor = group_begin; cursor < group_end; ++cursor) {
            publish_material_record(
                catalog, grouped_material_indices[cursor], shader_batch,
                shader_output, output_directory, &resolver_graph, options,
                &pending);
        }

        const bool shader_dependencies_stable =
            close_published_shader_dependencies(&shader_dependency_views);
        for (size_t cursor = group_begin; cursor < group_end; ++cursor) {
            MaterialBatchRecordResult* record =
                &pending.records[grouped_material_indices[cursor]];
            if (!shader_dependencies_stable) {
                if (record->material_publish_status ==
                        COMMON_OUTPUT_PUBLISH_EMITTED ||
                    record->meta_publish_status ==
                        COMMON_OUTPUT_PUBLISH_EMITTED ||
                    record->evidence_publish_status ==
                        COMMON_OUTPUT_PUBLISH_EMITTED) {
                    record->publication_residue = true;
                }
                if (record->failure !=
                    MATERIAL_BATCH_FAILURE_TEXTURE_DEPENDENCY_IDENTITY) {
                    record->failure =
                        MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY;
                }
                material_record_fail(
                    &pending, grouped_material_indices[cursor],
                    MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY);
            } else if (record->status == MATERIAL_BATCH_EMITTED) {
                ++pending.stats.emitted;
            } else if (record->status == MATERIAL_BATCH_UNCHANGED) {
                ++pending.stats.unchanged;
            }
        }
    }

    free(grouped_material_indices);
    free(shader_material_offsets);

    shader_catalog_pptr_graph_dispose(&resolver_graph);
    material_batch_result_dispose(result);
    *result = pending;
    return MATERIAL_BATCH_OK;
}

static bool lowercase_sha256_hex_is_valid(
    const char value[COMMON_SHA256_DIGEST_SIZE * 2U + 1U]);

bool material_batch_is_complete(const MaterialBatchResult* result) {
    if (!result || !result->catalog_authority ||
        !result->catalog_authority->materials_included ||
        (result->catalog_authority->record_count != 0U &&
         !result->selection_authority) ||
        result->catalog_authority->material_count != result->record_count ||
        (result->record_count != 0U &&
         (!result->records || !result->catalog_authority->materials)) ||
        result->record_count != result->stats.catalog_materials) {
        return false;
    }
    size_t unselected = 0U;
    size_t unassociated = 0U;
    size_t emitted = 0U;
    size_t unchanged = 0U;
    size_t failed = 0U;
    size_t shader_dependency_groups = 0U;
    for (size_t index = 0U; index < result->record_count; ++index) {
        const MaterialBatchRecordResult* record = &result->records[index];
        const ShaderCatalogMaterialRecord* catalog_record =
            &result->catalog_authority->materials[index];
        if (record->dependency_count != 0U && !record->dependencies) {
            return false;
        }
        switch (record->status) {
            case MATERIAL_BATCH_UNSELECTED:
                if (catalog_record->status !=
                        SHADER_CATALOG_MATERIAL_READY ||
                    !catalog_record->object.decoded ||
                    record->shader_record_index !=
                        catalog_record->shader_record_index ||
                    record->shader_record_index >=
                        result->catalog_authority->record_count ||
                    result->selection_authority[
                        record->shader_record_index] ||
                    record->failure != MATERIAL_BATCH_FAILURE_NONE ||
                    record->has_artifacts || record->publication_residue ||
                    record->material_publish_attempted ||
                    record->meta_publish_attempted ||
                    record->evidence_publish_attempted ||
                    record->output_path || record->output_meta_path ||
                    record->dependency_evidence_path ||
                    record->dependency_count != 0U || record->dependencies ||
                    record->asset_guid[0] != '\0' ||
                    record->artifact_identity_hex[0] != '\0' ||
                    record->texture_dependency_closure_complete) {
                    return false;
                }
                ++unselected;
                break;
            case MATERIAL_BATCH_UNASSOCIATED:
                if (catalog_record->status !=
                        SHADER_CATALOG_MATERIAL_SHADER_NULL ||
                    record->shader_record_index != SIZE_MAX ||
                    record->failure != MATERIAL_BATCH_FAILURE_NONE ||
                    record->has_artifacts || record->publication_residue ||
                    record->material_publish_attempted ||
                    record->meta_publish_attempted ||
                    record->evidence_publish_attempted ||
                    record->output_path || record->output_meta_path ||
                    record->dependency_evidence_path ||
                    record->dependency_count != 0U || record->dependencies ||
                    record->asset_guid[0] != '\0' ||
                    record->artifact_identity_hex[0] != '\0' ||
                    record->texture_dependency_closure_complete) {
                    return false;
                }
                ++unassociated;
                break;
            case MATERIAL_BATCH_EMITTED:
            case MATERIAL_BATCH_UNCHANGED: {
                const bool publication_emitted =
                    record->material_publish_status ==
                        COMMON_OUTPUT_PUBLISH_EMITTED ||
                    record->meta_publish_status ==
                        COMMON_OUTPUT_PUBLISH_EMITTED ||
                    record->evidence_publish_status ==
                        COMMON_OUTPUT_PUBLISH_EMITTED;
                if (catalog_record->status !=
                        SHADER_CATALOG_MATERIAL_READY ||
                    !catalog_record->object.decoded ||
                    record->shader_record_index !=
                        catalog_record->shader_record_index ||
                    record->shader_record_index >=
                        result->catalog_authority->record_count ||
                    !result->selection_authority[
                        record->shader_record_index] ||
                    record->failure != MATERIAL_BATCH_FAILURE_NONE ||
                    !record->has_artifacts || record->publication_residue ||
                    !record->output_path || !record->output_meta_path ||
                    !record->dependency_evidence_path ||
                    !unity_asset_guid_is_valid(record->asset_guid) ||
                    !lowercase_sha256_hex_is_valid(
                        record->artifact_identity_hex) ||
                    record->yaml_status != UNITY_MATERIAL_YAML_OK ||
                    !record->material_publish_attempted ||
                    !record->meta_publish_attempted ||
                    !record->evidence_publish_attempted ||
                    (record->material_publish_status !=
                         COMMON_OUTPUT_PUBLISH_EMITTED &&
                     record->material_publish_status !=
                         COMMON_OUTPUT_PUBLISH_UNCHANGED) ||
                    (record->meta_publish_status !=
                         COMMON_OUTPUT_PUBLISH_EMITTED &&
                     record->meta_publish_status !=
                         COMMON_OUTPUT_PUBLISH_UNCHANGED) ||
                    (record->evidence_publish_status !=
                         COMMON_OUTPUT_PUBLISH_EMITTED &&
                     record->evidence_publish_status !=
                         COMMON_OUTPUT_PUBLISH_UNCHANGED) ||
                    ((record->status == MATERIAL_BATCH_EMITTED) !=
                     publication_emitted)) {
                    return false;
                }
                if (record->status == MATERIAL_BATCH_EMITTED) ++emitted;
                else ++unchanged;
                break;
            }
            case MATERIAL_BATCH_FAILED:
                ++failed;
                break;
            default:
                return false;
        }
    }
    bool* seen_shader_groups = allocate_array(
        result->catalog_authority->record_count,
        sizeof(*seen_shader_groups));
    if (result->catalog_authority->record_count != 0U &&
        !seen_shader_groups) {
        return false;
    }
    for (size_t index = 0U; index < result->record_count; ++index) {
        const MaterialBatchRecordResult* record = &result->records[index];
        if (record->status != MATERIAL_BATCH_EMITTED &&
            record->status != MATERIAL_BATCH_UNCHANGED) {
            continue;
        }
        if (!seen_shader_groups[record->shader_record_index]) {
            seen_shader_groups[record->shader_record_index] = true;
            ++shader_dependency_groups;
        }
    }
    free(seen_shader_groups);
    return result->stats.unselected == unselected &&
        result->stats.unassociated == unassociated &&
        result->stats.emitted == emitted &&
        result->stats.unchanged == unchanged &&
        result->stats.failed == failed && failed == 0U &&
        result->stats.shader_dependency_groups ==
            shader_dependency_groups &&
        result->stats.selected == emitted + unchanged &&
        result->record_count ==
            unselected + unassociated + emitted + unchanged;
}

static bool lowercase_sha256_hex_is_valid(
    const char value[COMMON_SHA256_DIGEST_SIZE * 2U + 1U]) {
    if (!value || value[COMMON_SHA256_DIGEST_SIZE * 2U] != '\0') {
        return false;
    }
    for (size_t index = 0U; index < COMMON_SHA256_DIGEST_SIZE * 2U;
         ++index) {
        const char byte = value[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool closed_dependency_evidence_is_valid(
    const MaterialBatchTextureDependency* dependency,
    size_t dependency_index) {
    if (!dependency || dependency->property_index != dependency_index ||
        dependency->property_name_size == 0U ||
        !dependency->property_name || dependency->source_index == SIZE_MAX) {
        return false;
    }
    if (dependency->status == MATERIAL_BATCH_DEPENDENCY_NULL) {
        return dependency->serialized_path_id == 0 &&
            dependency->resolve_status == UNITY_PPTR_RESOLVE_NULL &&
            dependency->reference_status ==
                MATERIAL_BATCH_TEXTURE_REFERENCE_OK &&
            !dependency->has_target &&
            dependency->target_source_index == SIZE_MAX &&
            dependency->target_class_id == 0 &&
            dependency->target_path_id == 0 &&
            dependency->target_serialized_digest_hex[0] == '\0' &&
            dependency->external_index == SIZE_MAX &&
            !dependency->has_external_serialized_guid &&
            dependency->external_serialized_type == 0 &&
            !dependency->has_yaml_reference &&
            dependency->yaml_reference.file_id == 0 &&
            dependency->yaml_reference.guid[0] == '\0' &&
            dependency->yaml_reference.type == 0 &&
            !dependency->yaml_reference.asset_exported &&
            dependency->yaml_reference.publication_lease == NULL;
    }
    if (dependency->status !=
        MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED) {
        return true;
    }

    if (dependency->serialized_path_id == 0 ||
        !unity_pptr_resolve_status_is_success(dependency->resolve_status) ||
        dependency->reference_status != MATERIAL_BATCH_TEXTURE_REFERENCE_OK ||
        !dependency->has_target || dependency->target_source_index == SIZE_MAX ||
        dependency->target_class_id <= 0 ||
        dependency->target_path_id == 0 ||
        dependency->target_path_id != dependency->serialized_path_id ||
        !lowercase_sha256_hex_is_valid(
            dependency->target_serialized_digest_hex) ||
        !dependency->has_yaml_reference ||
        dependency->yaml_reference.file_id == 0 ||
        !unity_asset_guid_is_valid(dependency->yaml_reference.guid) ||
        dependency->yaml_reference.type < 0 ||
        dependency->yaml_reference.type > 3 ||
        !dependency->yaml_reference.asset_exported ||
        dependency->yaml_reference.publication_lease != NULL) {
        return false;
    }

    if (dependency->resolve_status == UNITY_PPTR_RESOLVE_LOCAL) {
        return dependency->serialized_file_id == 0 &&
            dependency->target_source_index == dependency->source_index &&
            dependency->external_index == SIZE_MAX &&
            !dependency->has_external_serialized_guid &&
            dependency->external_serialized_type == 0;
    }
    return dependency->serialized_file_id > 0 &&
        dependency->external_index != SIZE_MAX &&
        dependency->external_index ==
            (size_t)(dependency->serialized_file_id - 1) &&
        dependency->has_external_serialized_guid;
}

bool material_batch_texture_dependencies_are_closed(
    const MaterialBatchResult* result) {
    if (!material_batch_is_complete(result)) return false;
    size_t dependencies = 0U;
    size_t null_dependencies = 0U;
    size_t resolved = 0U;
    size_t exported = 0U;
    size_t unexported = 0U;
    size_t failed = 0U;
    bool closed = true;
    for (size_t record_index = 0U;
         record_index < result->record_count; ++record_index) {
        const MaterialBatchRecordResult* record =
            &result->records[record_index];
        if (record->status != MATERIAL_BATCH_EMITTED &&
            record->status != MATERIAL_BATCH_UNCHANGED) {
            if (record->dependency_count != 0U) return false;
            continue;
        }
        if (!record->texture_dependency_closure_complete) closed = false;
        for (size_t dependency_index = 0U;
             dependency_index < record->dependency_count;
             ++dependency_index) {
            const MaterialBatchTextureDependency* dependency =
                &record->dependencies[dependency_index];
            ++dependencies;
            if (!closed_dependency_evidence_is_valid(
                    dependency, dependency_index)) {
                closed = false;
            }
            switch (dependency->status) {
                case MATERIAL_BATCH_DEPENDENCY_NULL:
                    ++null_dependencies;
                    break;
                case MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED:
                    ++resolved;
                    ++exported;
                    break;
                case MATERIAL_BATCH_DEPENDENCY_RESOLVED_NOT_EXPORTED:
                    ++resolved;
                    ++unexported;
                    closed = false;
                    break;
                case MATERIAL_BATCH_DEPENDENCY_RESOLUTION_FAILED:
                    ++failed;
                    closed = false;
                    break;
                case MATERIAL_BATCH_DEPENDENCY_CLASS_UNSUPPORTED:
                case MATERIAL_BATCH_DEPENDENCY_REFERENCE_AUTHORITY_MISSING:
                case MATERIAL_BATCH_DEPENDENCY_REFERENCE_INVALID:
                    ++resolved;
                    ++failed;
                    closed = false;
                    break;
                case MATERIAL_BATCH_DEPENDENCY_TARGET_ASSET_UNEXPORTED:
                    ++resolved;
                    ++unexported;
                    ++failed;
                    closed = false;
                    break;
                default:
                    return false;
            }
        }
    }
    return result->stats.texture_dependencies == dependencies &&
        result->stats.null_texture_dependencies == null_dependencies &&
        result->stats.resolved_texture_dependencies == resolved &&
        result->stats.exported_texture_dependencies == exported &&
        result->stats.unexported_texture_dependencies == unexported &&
        result->stats.failed_texture_dependencies == failed && closed &&
        failed == 0U && unexported == 0U && resolved == exported;
}

const char* material_batch_texture_reference_status_name(
    MaterialBatchTextureReferenceStatus status) {
    switch (status) {
        case MATERIAL_BATCH_TEXTURE_REFERENCE_OK: return "ok";
        case MATERIAL_BATCH_TEXTURE_REFERENCE_AUTHORITY_MISSING:
            return "authority-missing";
        case MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_CLASS_UNSUPPORTED:
            return "target-class-unsupported";
        case MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED:
            return "target-asset-unexported";
        case MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED: return "rejected";
        default: return "unknown";
    }
}

const char* material_batch_record_status_name(MaterialBatchRecordStatus status) {
    switch (status) {
        case MATERIAL_BATCH_UNSELECTED: return "unselected";
        case MATERIAL_BATCH_UNASSOCIATED: return "unassociated";
        case MATERIAL_BATCH_EMITTED: return "emitted";
        case MATERIAL_BATCH_UNCHANGED: return "unchanged";
        case MATERIAL_BATCH_FAILED: return "failed";
        default: return "unknown";
    }
}

const char* material_batch_failure_name(MaterialBatchFailure failure) {
    switch (failure) {
        case MATERIAL_BATCH_FAILURE_NONE: return "none";
        case MATERIAL_BATCH_FAILURE_CATALOG_MATERIAL_NOT_READY:
            return "catalog-material-not-ready";
        case MATERIAL_BATCH_FAILURE_SHADER_INDEX_INVALID:
            return "shader-index-invalid";
        case MATERIAL_BATCH_FAILURE_SHADER_BATCH_NOT_READY:
            return "shader-batch-not-ready";
        case MATERIAL_BATCH_FAILURE_SHADER_META_MISSING:
            return "shader-meta-missing";
        case MATERIAL_BATCH_FAILURE_SHADER_DEPENDENCY_IDENTITY:
            return "shader-dependency-identity";
        case MATERIAL_BATCH_FAILURE_TEXTURE_DEPENDENCY_IDENTITY:
            return "texture-dependency-identity";
        case MATERIAL_BATCH_FAILURE_DEPENDENCY_AUTHORITY_INCOMPLETE:
            return "dependency-authority-incomplete";
        case MATERIAL_BATCH_FAILURE_TEXTURE_RESOLUTION:
            return "texture-resolution";
        case MATERIAL_BATCH_FAILURE_TEXTURE_CLASS_UNSUPPORTED:
            return "texture-class-unsupported";
        case MATERIAL_BATCH_FAILURE_TEXTURE_TARGET_ASSET_UNEXPORTED:
            return "texture-target-asset-unexported";
        case MATERIAL_BATCH_FAILURE_TEXTURE_REFERENCE_AUTHORITY_MISSING:
            return "texture-reference-authority-missing";
        case MATERIAL_BATCH_FAILURE_TEXTURE_REFERENCE_INVALID:
            return "texture-reference-invalid";
        case MATERIAL_BATCH_FAILURE_PROJECTION_ALLOCATION:
            return "projection-allocation";
        case MATERIAL_BATCH_FAILURE_MATERIAL_GUID: return "material-guid";
        case MATERIAL_BATCH_FAILURE_MATERIAL_YAML: return "material-yaml";
        case MATERIAL_BATCH_FAILURE_MATERIAL_META: return "material-meta";
        case MATERIAL_BATCH_FAILURE_DEPENDENCY_EVIDENCE:
            return "dependency-evidence";
        case MATERIAL_BATCH_FAILURE_OUTPUT_DIRECTORY:
            return "output-directory";
        case MATERIAL_BATCH_FAILURE_OUTPUT_NAME: return "output-name";
        case MATERIAL_BATCH_FAILURE_OUTPUT_COLLISION:
            return "output-collision";
        case MATERIAL_BATCH_FAILURE_OUTPUT_IO: return "output-io";
        default: return "unknown";
    }
}

const char* material_batch_dependency_status_name(
    MaterialBatchDependencyStatus status) {
    switch (status) {
        case MATERIAL_BATCH_DEPENDENCY_NULL: return "null";
        case MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED:
            return "resolved-exported";
        case MATERIAL_BATCH_DEPENDENCY_RESOLVED_NOT_EXPORTED:
            return "resolved-not-exported";
        case MATERIAL_BATCH_DEPENDENCY_RESOLUTION_FAILED:
            return "resolution-failed";
        case MATERIAL_BATCH_DEPENDENCY_CLASS_UNSUPPORTED:
            return "class-unsupported";
        case MATERIAL_BATCH_DEPENDENCY_TARGET_ASSET_UNEXPORTED:
            return "target-asset-unexported";
        case MATERIAL_BATCH_DEPENDENCY_REFERENCE_AUTHORITY_MISSING:
            return "reference-authority-missing";
        case MATERIAL_BATCH_DEPENDENCY_REFERENCE_INVALID:
            return "reference-invalid";
        default: return "unknown";
    }
}

const char* material_batch_status_name(MaterialBatchStatus status) {
    switch (status) {
        case MATERIAL_BATCH_OK: return "ok";
        case MATERIAL_BATCH_INVALID_ARGUMENT: return "invalid-argument";
        case MATERIAL_BATCH_ALLOCATION_FAILED: return "allocation-failed";
        case MATERIAL_BATCH_RESOLVER_GRAPH_FAILED:
            return "resolver-graph-failed";
        default: return "unknown";
    }
}
