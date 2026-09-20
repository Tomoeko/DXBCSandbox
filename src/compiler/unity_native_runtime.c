// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_native_runtime_internal.h"
#include "common/relative_path.h"
#include "common/stream.h"
#include "common/string_builder.h"
#include "dxbc/dxbc_compare.h"

#include <stdlib.h>
#include <string.h>

#define NATIVE_CAPTURE_MAX_BYTES (1024U * 1024U)
#define NATIVE_CAPTURE_INPUT_COUNT 10U

struct UnityNativeRuntime {
    UnityNativeRuntimeSummary summary;
};

static bool nonzero(const uint8_t *digest) {
    uint8_t bits = 0;
    for (size_t i = 0; i < 32; ++i)
        bits |= digest[i];
    return bits != 0;
}

static bool read_digest(ByteStream *stream, uint8_t *digest) {
    return stream_read_bytes(stream, digest, 32) && nonzero(digest);
}

static bool read_span(ByteStream *stream, const uint8_t **bytes, size_t *size) {
    uint32_t count;
    if (!stream_read_uint32(stream, &count) || !count || count > 65536 ||
        count > stream_remaining(stream))
        return false;
    *bytes = stream->data + stream->position;
    *size = count;
    return stream_skip(stream, count);
}

UnityNativeRuntimeStatus unity_native_runtime_inspect(const uint8_t *bytes, size_t size,
                                                       const UnityPlayerPackageAuthority *player,
                                                       UnityNativeRuntimeSummary *summary) {
    if (!summary)
        return UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT;
    memset(summary, 0, sizeof(*summary));
    UnityPlayerPackageSummary package;
    if (!bytes || !unity_player_package_describe(player, &package))
        return UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT;
    if (size < 212 || size > NATIVE_CAPTURE_MAX_BYTES || memcmp(bytes, "DVUOBS01", 8) != 0)
        return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
    ByteStream stream;
    stream_init(&stream, bytes, size);
    uint32_t version;
    if (!stream_skip(&stream, 8) || !stream_read_uint32(&stream, &version) || version != 1 ||
        !stream_read_uint32(&stream, &summary->member_count) || !summary->member_count ||
        summary->member_count > 4096 ||
        !stream_read_uint32(&stream, &summary->observation_count) || summary->observation_count != 12 ||
        !read_digest(&stream, summary->deployment_digest) ||
        !read_digest(&stream, summary->package_manifest_digest) ||
        !read_digest(&stream, summary->native_environment_digest) ||
        !read_digest(&stream, summary->worker_epoch) ||
        !read_digest(&stream, summary->target_artifact_digest) ||
        !read_digest(&stream, summary->candidate_artifact_digest))
        return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
    if (package.image.file_count != summary->member_count)
        return UNITY_NATIVE_RUNTIME_BINDING_MISMATCH;
    char previous[4097] = {0};
    size_t previous_size = 0;
    for (uint32_t i = 0; i < summary->member_count; ++i) {
        const uint8_t *name;
        size_t length;
        uint8_t digest[32];
        if (!read_span(&stream, &name, &length) || length > 4096 ||
            common_relative_path_validate_utf8((const char *)name, length).status !=
                COMMON_RELATIVE_PATH_OK || !read_digest(&stream, digest) ||
            (i && common_relative_path_compare_bytes(previous, previous_size,
                                                       (const char *)name, length) >= 0))
            return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
        memcpy(previous, name, length);
        previous[length] = 0;
        previous_size = length;
        ShaderRuntimeFileIdentity identity;
        if (!unity_player_package_find_file(player, previous, &identity) ||
            memcmp(identity.content_digest, digest, 32) != 0)
            return UNITY_NATIVE_RUNTIME_BINDING_MISMATCH;
    }
    uint8_t records[12][32];
    const uint8_t *expected[4] = {0};
    size_t expected_size[4] = {0};
    for (uint32_t i = 0; i < 12; ++i) {
        if (!read_digest(&stream, records[i]))
            return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
        for (uint32_t j = 0; j < i; ++j)
            if (memcmp(records[i], records[j], 32) == 0)
                return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
        for (unsigned artifact = 0; artifact < 4; ++artifact) {
            const uint8_t *data;
            size_t count;
            if (!read_span(&stream, &data, &count) || (artifact == 2 && count != 256))
                return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
            if (artifact < 2) {
                DXBCContainer container = {0};
                const bool parsed = dxbc_parse(&container, data, count);
                const bool stage = parsed && container.program_type ==
                    (artifact == 0 ? DXBC_PROGRAM_TYPE_VERTEX : DXBC_PROGRAM_TYPE_PIXEL);
                dxbc_free(&container);
                if (!stage)
                    return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
            }
            if (!(i & 1)) {
                expected[artifact] = data;
                expected_size[artifact] = count;
            } else if (artifact < 2) {
                DXBCCompareResult comparison;
                const DXBCCompareStatus status = dxbc_compare_exact(
                    expected[artifact], expected_size[artifact], data, count, &comparison);
                if (status == DXBC_COMPARE_EXPECTED_INVALID || status == DXBC_COMPARE_ACTUAL_INVALID ||
                    status == DXBC_COMPARE_INVALID_ARGUMENT)
                    return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
                if (status != DXBC_COMPARE_EQUAL)
                    return UNITY_NATIVE_RUNTIME_OBSERVATIONS_DIFFER;
            } else if (artifact == 2 && memcmp(expected[artifact], data, count) != 0) {
                return UNITY_NATIVE_RUNTIME_OBSERVATIONS_DIFFER;
            }
            /* result.tsv is retained verbatim in the bound observation hash.
             * Its schema/input checks belong to the shared authenticated
             * collector; do not create a second player-record parser here. */
        }
        if (i & 1)
            ++summary->equal_pair_count;
    }
    if (stream_remaining(&stream))
        return UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
    memcpy(summary->player_package_digest, package.package_digest, 32);
    common_sha256(bytes, size, summary->observation_digest);
    return UNITY_NATIVE_RUNTIME_OK;
}

static bool absolute(const char *path) {
    if (!path || !*path)
        return false;
#ifdef _WIN32
    return (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
               path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
           (path[0] == '\\' && path[1] == '\\');
#else
    return path[0] == '/';
#endif
}

static char *member_path(const char *directory, const char *name) {
    StringBuilder path;
    sb_init(&path);
    sb_append(&path, directory);
    sb_append_char(&path, '/');
    sb_append(&path, name);
    if (!sb_ok(&path)) {
        sb_free(&path);
        return NULL;
    }
    return sb_detach(&path);
}

static char *sibling_path(const char *path, const char *name) {
    const char *last = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!last || backslash > last))
        last = backslash;
#endif
    if (!last)
        return NULL;
    StringBuilder sibling;
    sb_init(&sibling);
    sb_append_len(&sibling, path, (size_t)(last - path) + 1);
    sb_append(&sibling, name);
    if (!sb_ok(&sibling)) {
        sb_free(&sibling);
        return NULL;
    }
    return sb_detach(&sibling);
}

static ShaderCatalogObjectStatus decode(const ShaderCatalog *catalog,
                                        const ShaderCatalogRecord *record,
                                        const TypeTreeSchemaRegistry *registry,
                                        ShaderCatalogObjectReport *report) {
    ShaderObject object;
    shader_object_init(&object);
    const ShaderCatalogObjectStatus status =
        shader_catalog_decode_object(catalog, record, registry, &object, report);
    shader_object_dispose(&object);
    return status;
}

UnityNativeRuntimeStatus unity_native_runtime_capture(const UnityNativeRuntimeOptions *options,
                                                       UnityNativeRuntime **output,
                                                       UnityNativeRuntimeDiagnostic *diagnostic) {
    if (output)
        *output = NULL;
    if (!diagnostic)
        return UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->exit_code = -1;
    diagnostic->input_index = SIZE_MAX;
    diagnostic->process_status = COMMON_PROCESS_INVALID_ARGUMENT;
    diagnostic->file_status = COMMON_FILE_INVALID_ARGUMENT;
    diagnostic->target_status = diagnostic->candidate_status = SHADER_CATALOG_OBJECT_INVALID_ARGUMENT;
    UnityPlayerPackageSummary player;
    if (!options || !output || !options->target_catalog || !options->target_record ||
        !options->candidate_catalog || !options->candidate_record ||
        !unity_player_package_describe(options->player, &player) ||
        !absolute(options->python) || !absolute(options->client_directory) ||
        !absolute(options->ssh_config) || !absolute(options->policy_path) ||
        !absolute(options->jobs_path) || !absolute(options->output_path) ||
        !options->timeout_ms || options->timeout_ms > 600000)
        return UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT;
    CommonFileBytes existing = {0};
    const CommonFileStatus exists = common_file_read_regular(options->output_path, 1, &existing);
    common_file_bytes_dispose(&existing);
    if (exists != COMMON_FILE_NOT_FOUND)
        return UNITY_NATIVE_RUNTIME_OUTPUT_EXISTS;

    ShaderCatalogObjectReport target, candidate;
    diagnostic->target_status = decode(options->target_catalog, options->target_record,
                                        options->registry, &target);
    diagnostic->candidate_status = decode(options->candidate_catalog, options->candidate_record,
                                           options->registry, &candidate);
    if (diagnostic->target_status != SHADER_CATALOG_OBJECT_OK ||
        diagnostic->candidate_status != SHADER_CATALOG_OBJECT_OK)
        return UNITY_NATIVE_RUNTIME_SOURCE_UNAVAILABLE;
    char *owned_paths[] = {member_path(options->client_directory, "capture_unity.py"),
                          member_path(options->client_directory, "unity_evidence.py"),
                          member_path(options->client_directory, "adapter_evidence.py"),
                          member_path(options->client_directory, "validation_client.py"),
                          sibling_path(options->policy_path, "unity-package.json"),
                          sibling_path(options->policy_path, "native-baseline.json")};
    const char *paths[NATIVE_CAPTURE_INPUT_COUNT] = {
        options->python, owned_paths[0], owned_paths[1], owned_paths[2], owned_paths[3],
        options->ssh_config, options->policy_path, options->jobs_path, owned_paths[4], owned_paths[5]};
    CommonFileView views[NATIVE_CAPTURE_INPUT_COUNT] = {0};
    size_t opened = 0;
    UnityNativeRuntime *runtime = NULL;
    UnityNativeRuntimeStatus status = UNITY_NATIVE_RUNTIME_TOOL_UNAVAILABLE;
    for (; opened < NATIVE_CAPTURE_INPUT_COUNT; ++opened) {
        if (!paths[opened]) {
            status = UNITY_NATIVE_RUNTIME_ALLOCATION_FAILED;
            goto cleanup;
        }
        diagnostic->file_status = common_file_view_open_regular(
            paths[opened], opened ? NATIVE_CAPTURE_MAX_BYTES : 128U * 1024U * 1024U, &views[opened]);
        if (diagnostic->file_status != COMMON_FILE_OK) {
            diagnostic->input_index = opened;
            goto cleanup;
        }
    }
    char target_hex[65], candidate_hex[65];
    common_sha256_digest_to_hex(target.source_artifact_digest, target_hex);
    common_sha256_digest_to_hex(candidate.source_artifact_digest, candidate_hex);
    const char *arguments[] = {
        options->python, "-I", "-B", owned_paths[0], "--ssh-config", options->ssh_config,
        "--policy", options->policy_path, "--jobs", options->jobs_path,
        "--target-sha256", target_hex, "--candidate-sha256", candidate_hex,
        "--output", options->output_path, NULL};
    diagnostic->process_status = common_process_run(arguments, options->timeout_ms, &diagnostic->exit_code);
    status = UNITY_NATIVE_RUNTIME_PROCESS_FAILED;
    if (diagnostic->process_status != COMMON_PROCESS_OK || diagnostic->exit_code != 0)
        goto cleanup;
    CommonFileBytes captured = {0};
    diagnostic->file_status = common_file_read_regular(options->output_path, NATIVE_CAPTURE_MAX_BYTES,
                                                       &captured);
    status = UNITY_NATIVE_RUNTIME_CAPTURE_INVALID;
    if (diagnostic->file_status != COMMON_FILE_OK)
        goto cleanup;
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        common_file_bytes_dispose(&captured);
        status = UNITY_NATIVE_RUNTIME_ALLOCATION_FAILED;
        goto cleanup;
    }
    UnityNativeRuntimeSummary *summary = &runtime->summary;
    status = unity_native_runtime_inspect(captured.data, captured.size, options->player, summary);
    common_file_bytes_dispose(&captured);
    if (status == UNITY_NATIVE_RUNTIME_BINDING_MISMATCH)
        diagnostic->binding = UNITY_NATIVE_BINDING_PLAYER_MEMBERSHIP;
    if (status != UNITY_NATIVE_RUNTIME_OK)
        goto cleanup;
    if (memcmp(summary->deployment_digest, views[6].captured_sha256, 32) != 0)
        diagnostic->binding = UNITY_NATIVE_BINDING_DEPLOYMENT;
    else if (memcmp(summary->package_manifest_digest, views[8].captured_sha256, 32) != 0)
        diagnostic->binding = UNITY_NATIVE_BINDING_PACKAGE_MANIFEST;
    else if (memcmp(summary->target_artifact_digest, target.source_artifact_digest, 32) != 0)
        diagnostic->binding = UNITY_NATIVE_BINDING_TARGET_BUNDLE;
    else if (memcmp(summary->candidate_artifact_digest, candidate.source_artifact_digest, 32) != 0)
        diagnostic->binding = UNITY_NATIVE_BINDING_CANDIDATE_BUNDLE;
    if (diagnostic->binding != UNITY_NATIVE_BINDING_NONE) {
        status = UNITY_NATIVE_RUNTIME_BINDING_MISMATCH;
        goto cleanup;
    }
    ShaderCatalogObjectReport after;
    if (decode(options->target_catalog, options->target_record, options->registry, &after) !=
            SHADER_CATALOG_OBJECT_OK || memcmp(target.release_digest, after.release_digest, 32) != 0 ||
        decode(options->candidate_catalog, options->candidate_record, options->registry, &after) !=
            SHADER_CATALOG_OBJECT_OK || memcmp(candidate.release_digest, after.release_digest, 32) != 0) {
        status = UNITY_NATIVE_RUNTIME_INPUT_CHANGED;
        goto cleanup;
    }
    memcpy(summary->target_release_digest, target.release_digest, 32);
    memcpy(summary->candidate_release_digest, candidate.release_digest, 32);
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char tool_domain[] = "DXBCSandbox.NativeCollector.Tools.v1";
    common_sha256_update(&hash, tool_domain, sizeof(tool_domain));
    for (size_t i = 0; i < NATIVE_CAPTURE_INPUT_COUNT; ++i)
        common_sha256_update(&hash, views[i].captured_sha256, 32);
    common_sha256_final(&hash, summary->tool_digest);
    common_sha256_init(&hash);
    static const char authority_domain[] = "DXBCSandbox.NativeCollector.Authority.v1";
    common_sha256_update(&hash, authority_domain, sizeof(authority_domain));
    common_sha256_update(&hash, summary->observation_digest, 32);
    common_sha256_update(&hash, summary->tool_digest, 32);
    common_sha256_update(&hash, summary->target_release_digest, 32);
    common_sha256_update(&hash, summary->candidate_release_digest, 32);
    common_sha256_update(&hash, summary->player_package_digest, 32);
    common_sha256_final(&hash, summary->authority_digest);
cleanup:
    for (size_t i = 0; i < opened; ++i) {
        if (common_file_view_close(&views[i]) != COMMON_FILE_OK) {
            diagnostic->input_index = i;
            status = UNITY_NATIVE_RUNTIME_INPUT_CHANGED;
        }
    }
    for (size_t i = 0; i < sizeof(owned_paths) / sizeof(*owned_paths); ++i)
        free(owned_paths[i]);
    if (status == UNITY_NATIVE_RUNTIME_OK)
        *output = runtime;
    else
        unity_native_runtime_free(runtime);
    return status;
}

void unity_native_runtime_free(UnityNativeRuntime *runtime) {
    free(runtime);
}

bool unity_native_runtime_describe(const UnityNativeRuntime *runtime,
                                   UnityNativeRuntimeSummary *summary) {
    if (!runtime || !summary)
        return false;
    *summary = runtime->summary;
    return true;
}

const char *unity_native_runtime_status_name(UnityNativeRuntimeStatus status) {
    switch (status) {
    case UNITY_NATIVE_RUNTIME_OK: return "ok";
    case UNITY_NATIVE_RUNTIME_INVALID_ARGUMENT: return "invalid-argument";
    case UNITY_NATIVE_RUNTIME_SOURCE_UNAVAILABLE: return "source-unavailable";
    case UNITY_NATIVE_RUNTIME_TOOL_UNAVAILABLE: return "tool-unavailable";
    case UNITY_NATIVE_RUNTIME_OUTPUT_EXISTS: return "output-exists";
    case UNITY_NATIVE_RUNTIME_PROCESS_FAILED: return "process-failed";
    case UNITY_NATIVE_RUNTIME_CAPTURE_INVALID: return "capture-invalid";
    case UNITY_NATIVE_RUNTIME_BINDING_MISMATCH: return "binding-mismatch";
    case UNITY_NATIVE_RUNTIME_OBSERVATIONS_DIFFER: return "observations-differ";
    case UNITY_NATIVE_RUNTIME_INPUT_CHANGED: return "input-changed";
    case UNITY_NATIVE_RUNTIME_ALLOCATION_FAILED: return "allocation-failed";
    default: return "unknown";
    }
}
