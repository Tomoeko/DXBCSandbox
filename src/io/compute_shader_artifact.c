// SPDX-License-Identifier: GPL-3.0-only

#include "io/compute_shader_artifact.h"

#include "dxbc/dxbc_parser.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARTIFACT_APPEND(output, text) \
    (sb_append((output), (text)), sb_ok(output))
#define ARTIFACT_APPEND_CHAR(output, value) \
    (sb_append_char((output), (value)), sb_ok(output))
#define ARTIFACT_APPENDF(output, ...) \
    (sb_appendf((output), __VA_ARGS__), sb_ok(output))

static bool append_json_bytes(StringBuilder* output,
                              ComputeShaderStringView value) {
    if ((!value.bytes && value.size != 0U) ||
        !ARTIFACT_APPEND_CHAR(output, '"')) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < value.size; ++index) {
        const uint8_t byte = value.bytes[index];
        if (byte == '"' || byte == '\\') {
            if (!ARTIFACT_APPEND_CHAR(output, '\\') ||
                !ARTIFACT_APPEND_CHAR(output, (char)byte)) return false;
        } else if (byte >= 0x20U && byte <= 0x7eU) {
            if (!ARTIFACT_APPEND_CHAR(output, (char)byte)) return false;
        } else {
            char escaped[7] = {'\\', 'u', '0', '0',
                               hex[byte >> 4U], hex[byte & 0xfU], '\0'};
            if (!ARTIFACT_APPEND(output, escaped)) return false;
        }
    }
    return ARTIFACT_APPEND_CHAR(output, '"');
}

static bool append_u32_array(StringBuilder* output, const uint32_t* values,
                             size_t count) {
    if ((count != 0U && !values) || !ARTIFACT_APPEND_CHAR(output, '[')) return false;
    for (size_t index = 0U; index < count; ++index) {
        if ((index != 0U && !ARTIFACT_APPEND_CHAR(output, ',')) ||
            !ARTIFACT_APPENDF(output, "%u", values[index])) return false;
    }
    return ARTIFACT_APPEND_CHAR(output, ']');
}

static bool append_string_array(StringBuilder* output,
                                const ComputeShaderStringView* values,
                                size_t count) {
    if ((count != 0U && !values) || !ARTIFACT_APPEND_CHAR(output, '[')) return false;
    for (size_t index = 0U; index < count; ++index) {
        if ((index != 0U && !ARTIFACT_APPEND_CHAR(output, ',')) ||
            !append_json_bytes(output, values[index])) return false;
    }
    return ARTIFACT_APPEND_CHAR(output, ']');
}

static bool append_resources(StringBuilder* output,
                             const ComputeShaderResource* resources,
                             size_t count) {
    if ((count != 0U && !resources) || !ARTIFACT_APPEND_CHAR(output, '[')) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        const ComputeShaderResource* resource = &resources[index];
        if ((index != 0U && !ARTIFACT_APPEND_CHAR(output, ',')) ||
            !ARTIFACT_APPEND(output, "{\"name\":") ||
            !append_json_bytes(output, resource->name) ||
            !ARTIFACT_APPEND(output, ",\"generated_name\":") ||
            !append_json_bytes(output, resource->generated_name) ||
            !ARTIFACT_APPENDF(output,
                ",\"bind_point\":%" PRId32
                ",\"sampler_bind_point\":%" PRId32
                ",\"texture_dimension\":%" PRId32 "}",
                resource->bind_point, resource->sampler_bind_point,
                resource->texture_dimension)) {
            return false;
        }
    }
    return ARTIFACT_APPEND_CHAR(output, ']');
}

static bool append_builtin_samplers(
    StringBuilder* output, const ComputeShaderBuiltinSampler* samplers,
    size_t count) {
    if ((count != 0U && !samplers) || !ARTIFACT_APPEND_CHAR(output, '[')) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        if ((index != 0U && !ARTIFACT_APPEND_CHAR(output, ',')) ||
            !ARTIFACT_APPENDF(output,
                "{\"sampler\":%u,\"bind_point\":%" PRId32 "}",
                samplers[index].sampler, samplers[index].bind_point)) {
            return false;
        }
    }
    return ARTIFACT_APPEND_CHAR(output, ']');
}

static bool append_parameters(StringBuilder* output,
                              const ComputeShaderParameter* parameters,
                              size_t count) {
    if ((count != 0U && !parameters) || !ARTIFACT_APPEND_CHAR(output, '[')) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        const ComputeShaderParameter* parameter = &parameters[index];
        if ((index != 0U && !ARTIFACT_APPEND_CHAR(output, ',')) ||
            !ARTIFACT_APPEND(output, "{\"name\":") ||
            !append_json_bytes(output, parameter->name) ||
            !ARTIFACT_APPENDF(output,
                ",\"type\":%" PRId32 ",\"offset\":%u,"
                "\"array_size\":%u,\"row_count\":%u,"
                "\"column_count\":%u}",
                parameter->type, parameter->offset, parameter->array_size,
                parameter->row_count, parameter->column_count)) {
            return false;
        }
    }
    return ARTIFACT_APPEND_CHAR(output, ']');
}

static bool append_constant_buffers(
    StringBuilder* output, const ComputeShaderConstantBuffer* buffers,
    size_t count) {
    if ((count != 0U && !buffers) || !ARTIFACT_APPEND_CHAR(output, '[')) return false;
    for (size_t index = 0U; index < count; ++index) {
        const ComputeShaderConstantBuffer* buffer = &buffers[index];
        if ((index != 0U && !ARTIFACT_APPEND_CHAR(output, ',')) ||
            !ARTIFACT_APPEND(output, "{\"name\":") ||
            !append_json_bytes(output, buffer->name) ||
            !ARTIFACT_APPENDF(output, ",\"byte_size\":%" PRId32
                        ",\"parameters\":", buffer->byte_size) ||
            !append_parameters(output, buffer->parameters,
                               buffer->parameter_count) ||
            !ARTIFACT_APPEND_CHAR(output, '}')) {
            return false;
        }
    }
    return ARTIFACT_APPEND_CHAR(output, ']');
}

static bool build_slug(const ComputeShaderObject* object,
                       char slug[97]) {
    size_t size = 0U;
    if (!object || !object->name.bytes || object->name.size == 0U) return false;
    for (size_t index = 0U; index < object->name.size && size < 96U; ++index) {
        uint8_t value = object->name.bytes[index];
        bool portable = (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '-' ||
            value == '_' || value == '.';
        slug[size++] = portable ? (char)value : '_';
    }
    while (size != 0U && slug[size - 1U] == '.') --size;
    if (size == 0U) slug[size++] = '_';
    slug[size] = '\0';
    return true;
}

static bool append_binary_named(ComputeShaderArtifactPackage* package,
                                const char* filename,
                                ComputeShaderBinaryArtifactKind kind,
                                const uint8_t* data, size_t size,
                                size_t* out_index) {
    if (!package || !filename || !out_index ||
        package->binary_count == SIZE_MAX || !data || size == 0U ||
        strlen(filename) >= sizeof(package->binaries[0].filename)) {
        return false;
    }
    size_t count = package->binary_count + 1U;
    if (count > SIZE_MAX / sizeof(*package->binaries)) return false;
    ComputeShaderBinaryArtifact* binaries =
        (ComputeShaderBinaryArtifact*)realloc(
            package->binaries, count * sizeof(*package->binaries));
    if (!binaries) return false;
    package->binaries = binaries;
    ComputeShaderBinaryArtifact* artifact =
        &binaries[package->binary_count];
    memset(artifact, 0, sizeof(*artifact));
    memcpy(artifact->filename, filename, strlen(filename) + 1U);
    artifact->data = data;
    artifact->size = size;
    artifact->kind = kind;
    common_sha256(data, size, artifact->sha256);
    common_sha256_digest_to_hex(artifact->sha256, artifact->sha256_hex);
    *out_index = package->binary_count;
    package->binary_count = count;
    return true;
}

static bool append_binary(ComputeShaderArtifactPackage* package,
                          const char* slug, int64_t path_id,
                          size_t platform_index, size_t kernel_index,
                          size_t variant_index,
                          ComputeShaderBinaryArtifactKind kind,
                          const uint8_t* data, size_t size,
                          size_t* out_index) {
    char filename[224];
    const char* suffix = kind == COMPUTE_SHADER_BINARY_DXBC
        ? "dxbc" : "program.bin";
    int written = snprintf(
        filename, sizeof(filename),
        "compute_%s__%" PRId64 ".p%zu.k%zu.v%zu.%s", slug, path_id,
        platform_index, kernel_index, variant_index, suffix);
    if (written < 0 || (size_t)written >= sizeof(filename)) {
        return false;
    }
    return append_binary_named(package, filename, kind, data, size,
                               out_index);
}

static bool append_binary_json(StringBuilder* output,
                               const ComputeShaderBinaryArtifact* artifact) {
    const char* kind = "unity-compiled-program";
    if (artifact->kind == COMPUTE_SHADER_BINARY_SERIALIZED_OBJECT) {
        kind = "unity-serialized-compute-object";
    } else if (artifact->kind == COMPUTE_SHADER_BINARY_DXBC) {
        kind = "dxbc";
    }
    return ARTIFACT_APPEND(output, "{\"file\":\"") &&
        ARTIFACT_APPEND(output, artifact->filename) &&
        ARTIFACT_APPENDF(output, "\",\"kind\":\"%s\",\"bytes\":%zu,"
                   "\"sha256\":\"%s\"}",
                   kind, artifact->size, artifact->sha256_hex);
}

void compute_shader_artifact_package_init(
    ComputeShaderArtifactPackage* package) {
    if (!package) return;
    memset(package, 0, sizeof(*package));
    sb_init(&package->manifest);
}

void compute_shader_artifact_package_dispose(
    ComputeShaderArtifactPackage* package) {
    if (!package) return;
    free(package->binaries);
    sb_free(&package->manifest);
    compute_shader_artifact_package_init(package);
}

ComputeShaderArtifactStatus compute_shader_artifact_build(
    const ComputeShaderObject* object, ComputeShaderArtifactPackage* package) {
    if (!object || !package) return COMPUTE_SHADER_ARTIFACT_INVALID_ARGUMENT;
    if (!object->decoded) return COMPUTE_SHADER_ARTIFACT_NOT_DECODED;

    ComputeShaderObjectSummary summary;
    if (!compute_shader_object_summarize(object, &summary)) {
        return COMPUTE_SHADER_ARTIFACT_NOT_DECODED;
    }
    char slug[97];
    if (!build_slug(object, slug)) return COMPUTE_SHADER_ARTIFACT_FORMAT_FAILED;

    ComputeShaderArtifactPackage candidate;
    compute_shader_artifact_package_init(&candidate);
    int written = snprintf(candidate.manifest_filename,
                           sizeof(candidate.manifest_filename),
                           "compute_%s__%" PRId64 ".compute.json",
                           slug, object->path_id);
    if (written < 0 ||
        (size_t)written >= sizeof(candidate.manifest_filename)) {
        compute_shader_artifact_package_dispose(&candidate);
        return COMPUTE_SHADER_ARTIFACT_FORMAT_FAILED;
    }
    char serialized_filename[224];
    written = snprintf(serialized_filename, sizeof(serialized_filename),
                       "compute_%s__%" PRId64 ".serialized-object.bin",
                       slug, object->path_id);
    size_t serialized_index = SIZE_MAX;
    if (written < 0 || (size_t)written >= sizeof(serialized_filename) ||
        !append_binary_named(
            &candidate, serialized_filename,
            COMPUTE_SHADER_BINARY_SERIALIZED_OBJECT,
            object->serialized_object_bytes,
            object->serialized_object_size, &serialized_index)) {
        compute_shader_artifact_package_dispose(&candidate);
        return COMPUTE_SHADER_ARTIFACT_FORMAT_FAILED;
    }

    const ComputeShaderSourceAuthorityStatus source_authority =
        compute_shader_object_source_authority(object);
    StringBuilder* output = &candidate.manifest;
    bool ok = ARTIFACT_APPEND(output,
        "{\"schema\":\"dxbc-sandbox-compute-artifact\","
        "\"version\":1,\"class_id\":72,\"layout_authority\":{"
        "\"kind\":\"" COMPUTE_SHADER_OBJECT_LAYOUT_AUTHORITY "\","
        "\"serialized_file_version\":") &&
        ARTIFACT_APPENDF(output, "%u", object->serialized_file_version) &&
        ARTIFACT_APPEND(output, ",\"unity_version\":") &&
        append_json_bytes(output, object->unity_version) &&
        ARTIFACT_APPEND(output,
            ",\"typetree_nodes\":146,\"type_hash\":"
            "\"abd9135b8ce83d043fef4e9ec7f53366\"},\"path_id\":\"") &&
        ARTIFACT_APPENDF(output, "%" PRId64, object->path_id) &&
        ARTIFACT_APPEND(output, "\",\"name\":") &&
        append_json_bytes(output, object->name) &&
        ARTIFACT_APPEND(output, ",\"serialized_object\":") &&
        append_binary_json(output, &candidate.binaries[serialized_index]) &&
        ARTIFACT_APPENDF(output,
            ",\"target_platform\":%u,\"string_encoding\":"
            "\"byte-preserving-json-u00xx\",\"binary_exact\":true,"
            "\"source_ready\":%s,\"source_authority\":\"%s\","
            "\"summary\":{\"platforms\":%zu,\"kernel_parents\":%zu,"
            "\"kernel_variants\":%zu,\"code_blobs\":%zu,"
            "\"dxbc_code_blobs\":%zu,\"empty_code_blobs\":%zu,"
            "\"non_dxbc_code_blobs\":%zu,"
            "\"exact_thread_groups\":%zu,\"invalid_thread_groups\":%zu,"
            "\"resources\":%zu,"
            "\"constant_buffers\":%zu,\"parameters\":%zu},"
            "\"platform_variants\":[",
            object->target_platform,
            source_authority == COMPUTE_SHADER_SOURCE_AUTHORITY_EXACT
                ? "true" : "false",
            compute_shader_source_authority_status_name(source_authority),
            summary.platform_count, summary.kernel_parent_count,
            summary.kernel_variant_count, summary.code_blob_count,
            summary.dxbc_code_blob_count,
            summary.empty_code_blob_count,
            summary.non_dxbc_code_blob_count,
            summary.exact_thread_group_count,
            summary.invalid_thread_group_count, summary.resource_count,
            summary.constant_buffer_count, summary.parameter_count);

    for (size_t platform_index = 0U;
         ok && platform_index < object->platform_count; ++platform_index) {
        const ComputeShaderPlatformVariant* platform =
            &object->platforms[platform_index];
        ok = (platform_index == 0U || ARTIFACT_APPEND_CHAR(output, ',')) &&
            ARTIFACT_APPENDF(output,
                "{\"index\":%zu,\"target_renderer\":%" PRId32
                ",\"target_level\":%" PRId32
                ",\"resources_resolved\":%s,\"constant_buffers\":",
                platform_index, platform->target_renderer,
                platform->target_level,
                platform->resources_resolved ? "true" : "false") &&
            append_constant_buffers(output, platform->constant_buffers,
                                    platform->constant_buffer_count) &&
            ARTIFACT_APPEND(output, ",\"kernels\":[");
        for (size_t kernel_index = 0U;
             ok && kernel_index < platform->kernel_count; ++kernel_index) {
            const ComputeShaderKernelParent* kernel =
                &platform->kernels[kernel_index];
            ok = (kernel_index == 0U || ARTIFACT_APPEND_CHAR(output, ',')) &&
                ARTIFACT_APPENDF(output, "{\"index\":%zu,\"name\":",
                           kernel_index) &&
                append_json_bytes(output, kernel->name) &&
                ARTIFACT_APPEND(output, ",\"global_keywords\":") &&
                append_string_array(output, kernel->global_keywords,
                                    kernel->global_keyword_count) &&
                ARTIFACT_APPEND(output, ",\"local_keywords\":") &&
                append_string_array(output, kernel->local_keywords,
                                    kernel->local_keyword_count) &&
                ARTIFACT_APPEND(output, ",\"variants\":[");
            for (size_t variant_index = 0U;
                 ok && variant_index < kernel->variant_count;
                 ++variant_index) {
                const ComputeShaderKernelVariant* variant =
                    &kernel->variants[variant_index];
                size_t raw_index = SIZE_MAX;
                size_t dxbc_index = SIZE_MAX;
                DXBCContainerView view = {0};
                bool has_dxbc = dxbc_container_view_first(
                    variant->code, variant->code_size, &view);
                bool raw_is_dxbc = has_dxbc && view.data == variant->code &&
                    view.size == variant->code_size;
                if (variant->code_size != 0U) {
                    if (!append_binary(
                            &candidate, slug, object->path_id,
                            platform_index, kernel_index, variant_index,
                            raw_is_dxbc ? COMPUTE_SHADER_BINARY_DXBC
                                        : COMPUTE_SHADER_BINARY_PROGRAM,
                            variant->code, variant->code_size, &raw_index)) {
                        ok = false;
                        break;
                    }
                    if (has_dxbc && !raw_is_dxbc &&
                        !append_binary(
                            &candidate, slug, object->path_id,
                            platform_index, kernel_index, variant_index,
                            COMPUTE_SHADER_BINARY_DXBC, view.data, view.size,
                            &dxbc_index)) {
                        ok = false;
                        break;
                    }
                    if (raw_is_dxbc) dxbc_index = raw_index;
                }
                ok = (variant_index == 0U || ARTIFACT_APPEND_CHAR(output, ',')) &&
                    ARTIFACT_APPENDF(output,
                        "{\"index\":%zu,\"keyword_key\":",
                        variant_index) &&
                    append_json_bytes(output, variant->keyword_key) &&
                    ARTIFACT_APPEND(output, ",\"constant_buffer_variant_indices\":") &&
                    append_u32_array(
                        output, variant->constant_buffer_variant_indices,
                        variant->constant_buffer_variant_index_count) &&
                    ARTIFACT_APPEND(output, ",\"constant_buffers\":") &&
                    append_resources(output, variant->constant_buffers,
                                     variant->constant_buffer_count) &&
                    ARTIFACT_APPEND(output, ",\"textures\":") &&
                    append_resources(output, variant->textures,
                                     variant->texture_count) &&
                    ARTIFACT_APPEND(output, ",\"builtin_samplers\":") &&
                    append_builtin_samplers(
                        output, variant->builtin_samplers,
                        variant->builtin_sampler_count) &&
                    ARTIFACT_APPEND(output, ",\"input_buffers\":") &&
                    append_resources(output, variant->input_buffers,
                                     variant->input_buffer_count) &&
                    ARTIFACT_APPEND(output, ",\"output_buffers\":") &&
                    append_resources(output, variant->output_buffers,
                                     variant->output_buffer_count) &&
                    ARTIFACT_APPEND(output, ",\"thread_group_size\":") &&
                    append_u32_array(output, variant->thread_group_size,
                                     variant->thread_group_size_count) &&
                    ARTIFACT_APPENDF(output,
                        ",\"requirements\":\"%" PRId64
                        "\",\"serialized_program\":",
                        variant->requirements);
                if (ok && raw_index == SIZE_MAX) {
                    ok = ARTIFACT_APPEND(output, "null");
                } else if (ok) {
                    ok = append_binary_json(output,
                                            &candidate.binaries[raw_index]);
                }
                ok = ok && ARTIFACT_APPEND(output, ",\"dxbc\":");
                if (ok && dxbc_index == SIZE_MAX) {
                    ok = ARTIFACT_APPEND(output, "null");
                } else if (ok) {
                    ok = append_binary_json(output,
                                            &candidate.binaries[dxbc_index]);
                }
                ok = ok && ARTIFACT_APPEND_CHAR(output, '}');
            }
            ok = ok && ARTIFACT_APPEND(output, "]}");
        }
        ok = ok && ARTIFACT_APPEND(output, "]}");
    }
    ok = ok && ARTIFACT_APPEND(output, "]}\n") && sb_ok(output);
    if (!ok) {
        compute_shader_artifact_package_dispose(&candidate);
        return COMPUTE_SHADER_ARTIFACT_FORMAT_FAILED;
    }
    compute_shader_artifact_package_dispose(package);
    *package = candidate;
    return COMPUTE_SHADER_ARTIFACT_OK;
}

const char* compute_shader_artifact_status_name(
    ComputeShaderArtifactStatus status) {
    switch (status) {
        case COMPUTE_SHADER_ARTIFACT_NOT_APPLICABLE:
            return "not-applicable";
        case COMPUTE_SHADER_ARTIFACT_OK: return "ok";
        case COMPUTE_SHADER_ARTIFACT_INVALID_ARGUMENT: return "invalid-argument";
        case COMPUTE_SHADER_ARTIFACT_NOT_DECODED: return "not-decoded";
        case COMPUTE_SHADER_ARTIFACT_SIZE_OVERFLOW: return "size-overflow";
        case COMPUTE_SHADER_ARTIFACT_ALLOCATION_FAILED: return "allocation-failed";
        case COMPUTE_SHADER_ARTIFACT_FORMAT_FAILED: return "format-failed";
        default: return "unknown";
    }
}
