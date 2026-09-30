// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/sha256.h"
#include "compiler/unity_compiler_client.h"
#include "io/unity_compute_binary.h"
#include "dxbc/dxbc_compare.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_FILE_LIMIT (16U * 1024U * 1024U)
#define PROBE_PLATFORM 4U
#define PROBE_BUILD_PLATFORM 1U

/* Manual native protocol observation only. Preprocessing has no native
 * success flag; this probe grants no Class72, import or runtime certificate. */
static void print_bytes(const char* label, const void* bytes, size_t size) {
    printf("%s[%zu]=", label, size);
    if (size != 0U) fwrite(bytes, 1U, size, stdout);
    putchar('\n');
}

static void print_text(const char* label, const char* text) {
    print_bytes(label, text, strlen(text));
}

static void print_hash(const char* label, const void* bytes, size_t size) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, hex);
    printf("%s=%s bytes=%zu\n", label, hex, size);
}

static void print_digest(const char* label, const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(digest, hex);
    printf("%s=%s\n", label, hex);
}

static bool capture_request_provenance(UnityCompilerChannel* channel, const char* source,
                                       UnityCompilerToolchainProvenance* provenance) {
    if (!channel->cache_source_root) return false;
    size_t root_size = strlen(channel->cache_source_root) + 1U;
    char* source_root = malloc(root_size);
    if (!source_root) return false;
    memcpy(source_root, channel->cache_source_root, root_size);
    /* Refreshing a lease may replace the channel's root; do not lend that
     * owned pointer back to the provenance API. */
    bool captured = unity_compiler_get_request_provenance(channel, source_root, source, provenance);
    free(source_root);
    return captured;
}

static void print_request_provenance(const char* phase,
                                    const UnityCompilerToolchainProvenance* provenance,
                                    size_t transcript_size) {
    printf("request_provenance_phase=%s canonical_digest_matches=1 canonical_bytes=%zu "
           "source_authority_revision=%" PRIu64 "\n",
           phase, transcript_size, provenance->source_authority_revision);
    print_digest("request_compiler_fingerprint", provenance->compiler_fingerprint);
    print_digest("request_environment_fingerprint", provenance->environment_fingerprint);
    print_text("toolchain_contents", provenance->unity_contents_path);
    print_text("toolchain_compiler", provenance->compiler_path);
    print_text("toolchain_builtin_includes", provenance->builtin_includes_dir);
    print_text("toolchain_playback_engines", provenance->playback_engines_dir);
    print_text("toolchain_glslang", provenance->glslang_path);
    print_text("toolchain_dxcompiler", provenance->dxcompiler_path);
}

static bool verify_preprocess_provenance(
    UnityCompilerChannel* channel, const UnityCompilerComputePreprocessRequest* request,
    const uint8_t retained_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    UnityCompilerToolchainProvenance provenance;
    uint8_t* transcript = NULL;
    size_t transcript_size = 0U;
    uint8_t current_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    bool matches = capture_request_provenance(channel, request->source, &provenance) &&
                   unity_compiler_serialize_compute_preprocess_request(
                       channel, request, &transcript, &transcript_size, current_digest) &&
                   memcmp(current_digest, retained_digest, sizeof(current_digest)) == 0 &&
                   provenance.source_authority_revision == channel->source_authority_revision;
    free(transcript);
    if (matches) print_request_provenance("preprocess", &provenance, transcript_size);
    return matches;
}

static bool verify_compile_provenance(
    UnityCompilerChannel* channel, const UnityCompilerComputeKernelRequest* request,
    const uint8_t retained_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    UnityCompilerToolchainProvenance provenance;
    uint8_t* transcript = NULL;
    size_t transcript_size = 0U;
    uint8_t current_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    bool matches = capture_request_provenance(channel, request->source, &provenance) &&
                   unity_compiler_serialize_compute_request(
                       channel, request, &transcript, &transcript_size, current_digest) &&
                   memcmp(current_digest, retained_digest, sizeof(current_digest)) == 0 &&
                   provenance.source_authority_revision == channel->source_authority_revision;
    free(transcript);
    if (matches) print_request_provenance("compile", &provenance, transcript_size);
    return matches;
}

static size_t print_diagnostics(const UnityCompilerDiagnostic* diagnostics, size_t count) {
    size_t actionable = 0U;
    printf("diagnostics=%zu\n", count);
    for (size_t i = 0U; i < count; ++i) {
        bool reported = unity_compiler_diagnostic_is_actionable(&diagnostics[i]);
        actionable += reported ? 1U : 0U;
        printf("diagnostic[%zu] actionable=%d\n", i, reported ? 1 : 0);
        print_text("record", diagnostics[i].record);
        print_text("file", diagnostics[i].file);
        print_text("message", diagnostics[i].message);
    }
    return actionable;
}

static const UnityCompilerComputePreprocessedKernel* print_preprocess(
    const UnityCompilerComputePreprocessResult* result, const char* selected_name) {
    printf("preprocess flags=0x%08" PRIx32 " requirements=0x%016" PRIx64
           " supported_apis=0x%08" PRIx32 " use_dxc=0x%08" PRIx32
           " never_use_dxc=0x%08" PRIx32 "\n",
           result->compilation_flags, result->requirements, (uint32_t)result->supported_apis,
           result->use_dxc_mask, result->never_use_dxc_mask);
    printf("include_hash_words=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
           result->include_hash_words[0], result->include_hash_words[1],
           result->include_hash_words[2], result->include_hash_words[3]);
    const UnityCompilerComputeKeywordLines* families[] = {&result->user_global, &result->user_local};
    for (size_t family = 0U; family < 2U; ++family) {
        printf("keyword_family=%s lines=%zu\n", family == 0U ? "global" : "local",
               families[family]->line_count);
        for (size_t i = 0U; i < families[family]->line_count; ++i)
            print_text("keyword_line", families[family]->lines[i]);
    }
    printf("dependencies=%zu kernels=%zu conditionals=%zu\n", result->dependency_count,
           result->kernel_count, result->conditional_requirement_count);
    for (size_t i = 0U; i < result->dependency_count; ++i)
        print_text("dependency", result->dependencies[i]);
    for (size_t i = 0U; i < result->conditional_requirement_count; ++i) {
        print_text("conditional_keyword", result->conditional_requirements[i].keyword);
        printf("conditional_requirements=0x%016" PRIx64 "\n",
               result->conditional_requirements[i].requirements);
    }
    const UnityCompilerComputePreprocessedKernel* selected = NULL;
    for (size_t i = 0U; i < result->kernel_count; ++i) {
        const UnityCompilerComputePreprocessedKernel* entry = &result->kernels[i];
        print_text("kernel", entry->name);
        printf("kernel_macros=%zu\n", entry->macro_count);
        for (size_t j = 0U; j < entry->macro_count; ++j) {
            print_text("macro_name", entry->macros[j].name);
            print_text("macro_value", entry->macros[j].value);
        }
        if (strcmp(entry->name, selected_name) == 0) selected = entry;
    }
    print_hash("returned_source_sha256", result->source, result->source_size);
    return selected;
}

static void print_resources(const char* role, const ComputeShaderResource* resources, size_t count) {
    printf("resource_role=%s count=%zu\n", role, count);
    for (size_t i = 0U; i < count; ++i) {
        print_bytes("name", resources[i].name.bytes, resources[i].name.size);
        print_bytes("generated_name", resources[i].generated_name.bytes, resources[i].generated_name.size);
        printf("binding=%" PRId32 " sampler=%" PRId32 " dimension=%" PRId32 "\n",
               resources[i].bind_point, resources[i].sampler_bind_point, resources[i].texture_dimension);
    }
}

static void print_native_declarations(const UnityComputeBinary* binary) {
    for (size_t i = 0U; i < binary->directive_count; ++i) {
        const UnityComputeKernelDirective* directive = &binary->directives[i];
        printf("native_directive[%zu] macros=%zu\n", i, directive->macro_count);
        print_bytes("name", directive->name.bytes, directive->name.size);
        for (size_t j = 0U; j < directive->macro_count; ++j) {
            printf("native_macro[%zu]\n", j);
            print_bytes("name", directive->macros[j].name.bytes, directive->macros[j].name.size);
            print_bytes("value", directive->macros[j].value.bytes, directive->macros[j].value.size);
        }
    }
    for (size_t i = 0U; i < binary->buffer_variant_count; ++i) {
        const UnityComputeBufferVariant* variant = &binary->buffer_variants[i];
        printf("native_buffer_variant[%zu] buffers=%zu\n", i, variant->buffer_count);
        for (size_t j = 0U; j < variant->buffer_count; ++j) {
            const ComputeShaderConstantBuffer* buffer = &variant->buffers[j];
            printf("native_cbuffer[%zu] byte_size=%" PRId32 " parameters=%zu\n",
                   j, buffer->byte_size, buffer->parameter_count);
            print_bytes("name", buffer->name.bytes, buffer->name.size);
            for (size_t k = 0U; k < buffer->parameter_count; ++k) {
                const ComputeShaderParameter* parameter = &buffer->parameters[k];
                printf("native_parameter[%zu] type=%" PRId32 " offset=%" PRIu32
                       " array_size=%" PRIu32 " rows=%" PRIu32 " columns=%" PRIu32 "\n",
                       k, parameter->type, parameter->offset, parameter->array_size,
                       parameter->row_count, parameter->column_count);
                print_bytes("name", parameter->name.bytes, parameter->name.size);
            }
        }
    }
}

static void print_kernel(const UnityComputeKernelBinary* kernel) {
    const ComputeShaderKernelVariant* data = &kernel->data;
    print_bytes("compiled_kernel", kernel->name.bytes, kernel->name.size);
    print_hash("complete_dxbc_sha256", data->code, data->code_size);
    printf("groups=%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
           data->thread_group_size[0], data->thread_group_size[1], data->thread_group_size[2]);
    print_resources("cb", data->constant_buffers, data->constant_buffer_count);
    print_resources("texture", data->textures, data->texture_count);
    print_resources("input_buffer", data->input_buffers, data->input_buffer_count);
    print_resources("output_buffer", data->output_buffers, data->output_buffer_count);
    printf("builtin_samplers=%zu\n", data->builtin_sampler_count);
    for (size_t i = 0U; i < data->builtin_sampler_count; ++i)
        printf("sampler=%" PRIu32 " binding=%" PRId32 "\n",
               data->builtin_samplers[i].sampler, data->builtin_samplers[i].bind_point);
}

static void usage(const char* executable) {
    fprintf(stderr, "usage: %s SOURCE.compute PROJECT_ROOT INCLUDES_DIR KERNEL "
                    "[--target DXBC_FILE] [USER_KEYWORD ...]\n"
                    "Use '-' for no additional includes directory. Optional target comparison "
                    "uses every byte. No files are written.\n", executable);
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (argc < 5 || !argv[4][0]) {
        usage(argv[0]);
        return 2;
    }
    const char* target_path = NULL;
    int keyword_start = 5;
    if (argc > 5 && strcmp(argv[5], "--target") == 0) {
        if (argc < 7) {
            usage(argv[0]);
            return 2;
        }
        target_path = argv[6];
        keyword_start = 7;
    }
    int keyword_count = argc - keyword_start;
    if (keyword_count > 1024) {
        fputs("at most 1024 explicit user keywords are supported\n", stderr);
        return 2;
    }
    CommonFileBytes source = {0}, target = {0};
    UnityCompilerChannel channel = {.socket_fd = -1};
    UnityCompilerComputePreprocessResponse* preprocess = NULL;
    UnityCompilerBinaryResponse compiled;
    unity_compiler_binary_response_init(&compiled);
    UnityComputeBinary binary;
    unity_compute_binary_init(&binary);
    UnityCompilerComputeMacro* macros = NULL;
    int exit_code = 1;
    if (common_file_read_regular_terminated(argv[1], PROBE_FILE_LIMIT, &source) != COMMON_FILE_OK ||
        !source.data || memchr(source.data, 0, source.size)) {
        fputs("could not read a bounded complete source without embedded NUL bytes\n", stderr);
        goto done;
    }
    if (target_path && common_file_read_regular(target_path, PROBE_FILE_LIMIT, &target) != COMMON_FILE_OK) {
        fputs("could not read explicit target\n", stderr);
        goto done;
    }
    print_text("source_filename", argv[1]);
    print_hash("original_source_sha256", source.data, source.size);
    const char* includes = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
    UnityCompilerSessionCapabilities capabilities;
    uint32_t valid_apis = 0U;
    if (!unity_compiler_start_lazy(&channel, argv[2], includes) ||
        !unity_compiler_capture_session_capabilities(&channel, &capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(&capabilities, &valid_apis) ||
        !unity_compiler_set_expected_valid_apis(&channel, valid_apis)) {
        fputs("could not capture the selected compiler's session authority\n", stderr);
        goto done;
    }
    printf("session raw_mask=0x%08" PRIx32 " valid_apis=0x%08" PRIx32
           " compiler_platform=%u build_platform=%u\n",
           capabilities.raw_available_platform_mask, valid_apis, PROBE_PLATFORM, PROBE_BUILD_PLATFORM);
    const uint32_t platform_bit = UINT32_C(1) << PROBE_PLATFORM;
    if ((valid_apis & platform_bit) == 0U) {
        fputs("the session does not expose the requested D3D11 compiler platform\n", stderr);
        goto done;
    }
    UnityCompilerComputePreprocessRequest preprocess_request = {
        .source = (const char*)source.data, .source_filename = argv[1], .caching_preprocessor = true,
        .build_platform = PROBE_BUILD_PLATFORM, .valid_apis = valid_apis};
    if (!unity_compiler_preprocess_compute_response(&channel, &preprocess_request, &preprocess)) {
        fputs("native preprocessing transport failed\n", stderr);
        goto done;
    }
    UnityCompilerComputePreprocessInfo info;
    if (!unity_compiler_compute_preprocess_response_info(preprocess, &info)) goto done;
    printf("preprocess transport_complete=%d availability=%d native_success_present=%d\n",
           info.transport_complete ? 1 : 0, (int)info.availability, info.native_success_present ? 1 : 0);
    if (info.has_request_identity) {
        print_digest("preprocess_request_sha256", info.request_digest);
        print_digest("preprocess_controls_sha256", info.controls_digest);
    }
    size_t diagnostic_count = 0U;
    const UnityCompilerDiagnostic* diagnostics =
        unity_compiler_compute_preprocess_response_diagnostics(preprocess, &diagnostic_count);
    bool clean_diagnostics = print_diagnostics(diagnostics, diagnostic_count) == 0U;
    const UnityCompilerComputePreprocessResult* result =
        unity_compiler_compute_preprocess_response_result(preprocess);
    if (!result || !info.transport_complete || !info.has_request_identity || info.native_success_present) goto done;
    if (!verify_preprocess_provenance(&channel, &preprocess_request, info.request_digest)) {
        fputs("preprocessing request or compiler/include lease changed before provenance replay\n", stderr);
        goto done;
    }
    const UnityCompilerComputePreprocessedKernel* selected = print_preprocess(result, argv[4]);
    if (!selected || selected->macro_count > (size_t)INT_MAX ||
        (((uint32_t)result->supported_apis & platform_bit) == 0U)) {
        fputs("selected kernel or compiler platform is absent from the native result\n", stderr);
        goto done;
    }
    if (selected->macro_count != 0U) {
        macros = calloc(selected->macro_count, sizeof(*macros));
        if (!macros) goto done;
    }
    for (size_t i = 0U; i < selected->macro_count; ++i) {
        macros[i].name = selected->macros[i].name;
        macros[i].value = selected->macros[i].value;
    }
    uint64_t requirements = result->requirements;
    for (size_t i = 0U; i < result->conditional_requirement_count; ++i)
        for (int j = keyword_start; j < argc; ++j)
            if (strcmp(result->conditional_requirements[i].keyword, argv[j]) == 0)
                requirements |= result->conditional_requirements[i].requirements;
    for (int i = keyword_start; i < argc; ++i) print_text("selected_user_keyword", argv[i]);
    UnityCompilerComputeKernelRequest request = {
        .source = result->source, .source_filename = argv[1], .kernel_name = selected->name,
        .caching_preprocessor = preprocess_request.caching_preprocessor,
        .build_platform = PROBE_BUILD_PLATFORM, .kernel_macros = macros,
        .kernel_macro_count = (int)selected->macro_count,
        .user_keywords = keyword_count ? argv + keyword_start : NULL, .user_keyword_count = keyword_count,
        .compiler_platform = (int32_t)PROBE_PLATFORM, .compilation_flags = result->compilation_flags,
        .requirements = requirements, .force_dxc = result->use_dxc_mask, .force_fxc = result->never_use_dxc_mask};
    printf("compile caching_preprocessor=%d preprocess_only=0 strip_line_directives=0 "
           "requirements=0x%016" PRIx64 " flags=0x%08" PRIx32
           " force_dxc=0x%08" PRIx32 " force_fxc=0x%08" PRIx32 "\n",
           request.caching_preprocessor ? 1 : 0, request.requirements,
           request.compilation_flags, request.force_dxc, request.force_fxc);
    if (!unity_compiler_compile_compute_response(&channel, &request, &compiled)) {
        fputs("native compute compilation transport failed\n", stderr);
        goto done;
    }
    printf("compile success=%d availability=%d payload_bytes=%zu\n",
           compiled.status.compiler_success ? 1 : 0, (int)compiled.status.availability, compiled.size);
    if (compiled.has_request_identity) {
        print_digest("compile_request_sha256", compiled.request_digest);
        print_digest("compile_controls_sha256", compiled.controls_digest);
    }
    clean_diagnostics = print_diagnostics(compiled.status.diagnostics, compiled.status.diagnostic_count) == 0U &&
                        clean_diagnostics;
    if (!compiled.has_request_identity ||
        !verify_compile_provenance(&channel, &request, compiled.request_digest)) {
        fputs("compute request or compiler/include lease changed before provenance replay\n", stderr);
        goto done;
    }
    if (!compiled.has_request_identity || !compiled.status.compiler_success ||
        compiled.status.availability != UNITY_COMPILER_RESPONSE_AVAILABLE ||
        unity_compute_binary_decode(&binary, compiled.data, compiled.size) != COMPUTE_SHADER_OBJECT_OK) goto done;
    printf("native_payload directives=%zu target_level=%" PRId32
           " kernels=%zu cb_variants=%zu resources_resolved=%d\n",
           binary.directive_count, binary.target_level, binary.kernel_count,
           binary.buffer_variant_count, binary.resources_resolved ? 1 : 0);
    print_native_declarations(&binary);
    for (size_t i = 0U; i < binary.kernel_count; ++i) print_kernel(&binary.kernels[i]);
    if (binary.kernel_count != 1U || binary.kernels[0].name.size != strlen(selected->name) ||
        memcmp(binary.kernels[0].name.bytes, selected->name, binary.kernels[0].name.size) != 0) goto done;
    const ComputeShaderKernelVariant* data = &binary.kernels[0].data;
    DXBCCompareResult comparison;
    /* The shared comparator validates the complete raw container even when
     * comparing it to itself. That is distinct from an external target match. */
    DXBCCompareStatus validation_status = dxbc_compare_exact(
        data->code, data->code_size, data->code, data->code_size, &comparison);
    printf("complete_dxbc_container_validation=%s validation_status=%s\n",
           validation_status == DXBC_COMPARE_EQUAL ? "valid" : "failed",
           dxbc_compare_status_name(validation_status));
    DXBCCompareStatus comparison_status = validation_status;
    if (target_path) {
        comparison_status = dxbc_compare_exact(target.data, target.size, data->code, data->code_size, &comparison);
        printf("complete_dxbc_target_equality=%s\n", dxbc_compare_status_name(comparison_status));
    } else {
        puts("complete_dxbc_target_equality=not-requested");
    }
    printf("clean_compile_diagnostics=%d\n", clean_diagnostics ? 1 : 0);
    exit_code = clean_diagnostics && validation_status == DXBC_COMPARE_EQUAL &&
                comparison_status == DXBC_COMPARE_EQUAL ? 0 : 1;
done:
    unity_compute_binary_dispose(&binary);
    unity_compiler_binary_response_free(&compiled);
    free(macros);
    unity_compiler_compute_preprocess_response_free(preprocess);
    unity_compiler_shutdown(&channel);
    common_file_bytes_dispose(&target);
    common_file_bytes_dispose(&source);
    return exit_code;
}
