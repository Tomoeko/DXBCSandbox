// SPDX-License-Identifier: GPL-3.0-only

#include "common/output_publish.h"
#include "common/sha256.h"
#include "compiler/unity_compiler_client.h"
#include "compiler/unity_compute_domain.h"
#include "compiler/unity_compute_verifier.h"
#include "io/unity_input.h"
#include "translation/compute_source_candidate.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Manual source-free selected-domain comparison, never a Class72 producer,
 * original-control, import, semantic, runtime or whole-source certificate. */
enum { INPUT_LIMIT = 16 * 1024 * 1024, MAX_MEMBERS = 64, D3D11_CLASS72_RENDERER = 2, D3D11_COMPILER_PLATFORM = 4 };

typedef struct {
    const char* requested_name;
    ComputeShaderObject object;
    ComputeSourceCandidate candidate;
    SerializedFile selected_file;
    bool selected_file_open;
    size_t matching_objects;
    size_t requested_variants;
    size_t requested_kernels;
    size_t visited_members;
    size_t member_index;
    bool failed;
    uint8_t member_digest[COMMON_SHA256_DIGEST_SIZE];
} Selection;

static void print_digest(const char* name, const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(digest, hex);
    printf("%s=%s\n", name, hex);
}

static void print_hash(const char* name, const void* bytes, size_t size) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(bytes, size, digest);
    print_digest(name, digest);
}

static void print_view(const char* name, ComputeShaderStringView view) {
    printf("%s[%zu]=", name, view.size);
    if (view.size)
        fwrite(view.bytes, 1, view.size, stdout);
    putchar('\n');
}

static bool view_equal(ComputeShaderStringView view, const char* string) {
    const size_t size = strlen(string);
    return view.size == size && (!size || memcmp(view.bytes, string, size) == 0);
}

static bool parse_u32(const char* text, uint32_t* value) {
    if (!text || text[0] < '0' || text[0] > '9')
        return false;
    char* end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || *end || parsed > UINT32_MAX)
        return false;
    *value = (uint32_t)parsed;
    return true;
}

static const char* availability_name(UnityCompilerResponseAvailability availability) {
    switch (availability) {
    case UNITY_COMPILER_RESPONSE_AVAILABLE:
        return "available";
    case UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS:
        return "cache-only-miss";
    case UNITY_COMPILER_RESPONSE_INCLUDE_AUTHORITY_UNAVAILABLE:
        return "include-authority-unavailable";
    }
    return "unknown";
}

static bool select_source(const UnitySerializedSource* source, void* opaque) {
    Selection* selection = opaque;
    if (++selection->visited_members > MAX_MEMBERS || source->size > INPUT_LIMIT) {
        selection->failed = true;
        return false;
    }
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        selection->failed = true;
        return false;
    }
    bool ok = true, retained_file = false;
    for (int index = 0; ok && index < file.object_count; ++index) {
        const AssetObjectInfo* asset = &file.objects[index];
        if (asset->type_id != 72)
            continue;
        ComputeShaderNameView name;
        ComputeShaderInventoryStatus inventory = compute_shader_object_name_view(&file, asset, &name);
        if (inventory != COMPUTE_SHADER_INVENTORY_OK) {
            fprintf(stderr, "compute inventory unavailable: %s\n", compute_shader_inventory_status_name(inventory));
            ok = false;
            break;
        }
        if (!view_equal((ComputeShaderStringView){name.name_bytes, name.name_size}, selection->requested_name))
            continue;
        if (++selection->matching_objects != 1)
            continue;
        ComputeShaderObjectStatus decoded = compute_shader_object_decode_borrowed(&selection->object, &file, asset);
        if (decoded != COMPUTE_SHADER_OBJECT_OK) {
            fprintf(stderr, "selected object decoding failed: %s\n", compute_shader_object_status_name(decoded));
            ok = false;
            break;
        }
        /* The object also borrows file.unity_version. Keep this metadata
         * owner alive alongside the snapshot's member bytes. */
        selection->selected_file = file;
        selection->selected_file_open = true;
        retained_file = true;
        ComputeSourceDiagnostic diagnostic;
        ComputeSourceStatus built =
            compute_source_candidate_build(&selection->object, &selection->candidate, &diagnostic);
        selection->requested_variants = diagnostic.requested_variants;
        selection->requested_kernels = diagnostic.requested_kernels;
        printf("candidate status=%s requested_kernels=%zu requested_variants=%zu examined=%zu represented=%zu\n",
               compute_source_status_name(built), diagnostic.requested_kernels, diagnostic.requested_variants,
               diagnostic.examined_variants, diagnostic.represented_variants);
        if (built != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED || !selection->candidate.domain_complete) {
            ok = false;
            break;
        }
        selection->member_index = source->member_index;
        common_sha256(source->data, source->size, selection->member_digest);
    }
    if (!retained_file)
        serialized_file_close(&file);
    selection->failed = !ok;
    return ok;
}

static size_t print_diagnostics(const UnityCompilerDiagnostic* diagnostics, size_t count) {
    size_t actionable = 0;
    for (size_t index = 0; index < count; ++index) {
        const UnityCompilerDiagnostic* diagnostic = &diagnostics[index];
        const bool reported = unity_compiler_diagnostic_is_actionable(diagnostic);
        actionable += reported ? 1 : 0;
        printf("diagnostic[%zu] actionable=%d fields=%" PRId32 ",%" PRId32 ",%" PRId32 "\n", index, reported ? 1 : 0,
               diagnostic->fields[0], diagnostic->fields[1], diagnostic->fields[2]);
        print_hash("diagnostic_record_sha256", diagnostic->record, strlen(diagnostic->record));
        print_hash("diagnostic_file_sha256", diagnostic->file, strlen(diagnostic->file));
        print_hash("diagnostic_message_sha256", diagnostic->message, strlen(diagnostic->message));
        printf("diagnostic_message_preview=%.512s\n", diagnostic->message);
    }
    return actionable;
}

static bool request_provenance(UnityCompilerChannel* channel, const char* source,
                               UnityCompilerToolchainProvenance* provenance) {
    if (!channel->cache_source_root)
        return false;
    const size_t size = strlen(channel->cache_source_root) + 1;
    char* root = malloc(size);
    if (!root)
        return false;
    memcpy(root, channel->cache_source_root, size);
    /* Lease refresh may replace the channel's owned root. */
    const bool captured = unity_compiler_get_request_provenance(channel, root, source, provenance);
    free(root);
    return captured;
}

static bool replay_request(UnityCompilerChannel* channel, bool preprocess, const void* request, const char* source,
                           const uint8_t retained[UNITY_COMPILER_FINGERPRINT_SIZE],
                           UnityCompilerToolchainProvenance* provenance) {
    uint8_t* transcript = NULL;
    size_t transcript_size = 0;
    uint8_t current[UNITY_COMPILER_FINGERPRINT_SIZE];
    bool ok = request_provenance(channel, source, provenance);
    if (ok)
        ok = preprocess
                 ? unity_compiler_serialize_compute_preprocess_request(channel, request, &transcript, &transcript_size,
                                                                       current)
                 : unity_compiler_serialize_compute_request(channel, request, &transcript, &transcript_size, current);
    ok = ok && memcmp(current, retained, sizeof(current)) == 0 &&
         provenance->source_authority_revision == channel->source_authority_revision;
    free(transcript);
    if (ok)
        printf("canonical_request_replayed=1 canonical_bytes=%zu revision=%" PRIu64 "\n", transcript_size,
               provenance->source_authority_revision);
    return ok;
}

static bool same_lease(const UnityCompilerToolchainProvenance* left, const UnityCompilerToolchainProvenance* right) {
    return left->source_authority_revision == right->source_authority_revision &&
           memcmp(left->compiler_fingerprint, right->compiler_fingerprint, UNITY_COMPILER_FINGERPRINT_SIZE) == 0 &&
           memcmp(left->environment_fingerprint, right->environment_fingerprint, UNITY_COMPILER_FINGERPRINT_SIZE) == 0;
}

static bool attest_keyword_families(const ComputeSourceCandidate* candidate, const UnityComputeDomain* domain) {
    const UnityComputeDomainScope scopes[] = {UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL, UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL};
    const size_t counts[] = {candidate->global_keyword_count, candidate->local_keyword_count};
    for (size_t scope = 0, offset = 0; scope < 2; offset += counts[scope], ++scope) {
        if (unity_compute_domain_family_count(domain, scopes[scope]) != counts[scope])
            return false;
        bool seen[COMPUTE_SOURCE_MAX_KEYWORDS] = {false};
        for (size_t index = 0; index < counts[scope]; ++index) {
            UnityComputeDomainFamily family;
            if (unity_compute_domain_family_at(domain, scopes[scope], index, &family) != UNITY_COMPUTE_DOMAIN_OK ||
                family.choice_count != 2 || (family.choices[0] == NULL) == (family.choices[1] == NULL))
                return false;
            const char* selected = family.choices[0] ? family.choices[0] : family.choices[1];
            size_t match = counts[scope];
            for (size_t keyword = 0; keyword < counts[scope]; ++keyword)
                if (strcmp(selected, candidate->keywords[offset + keyword]) == 0)
                    match = keyword;
            if (match == counts[scope] || seen[match])
                return false;
            seen[match] = true;
            printf("returned_family scope=%s index=%zu raw=%s\n", scope ? "local" : "global", index, family.raw_line);
        }
    }
    return true;
}

static bool state_mask(const ComputeSourceCandidate* candidate, const UnityComputeDomainState* state, uint64_t* mask) {
    *mask = 0;
    const char* const* spans[] = {state->global_keywords, state->local_keywords};
    const size_t selected_counts[] = {state->global_keyword_count, state->local_keyword_count};
    const size_t counts[] = {candidate->global_keyword_count, candidate->local_keyword_count};
    for (size_t scope = 0, offset = 0; scope < 2; offset += counts[scope], ++scope)
        for (size_t selected = 0; selected < selected_counts[scope]; ++selected) {
            size_t match = counts[scope];
            for (size_t index = 0; index < counts[scope]; ++index)
                if (strcmp(spans[scope][selected], candidate->keywords[offset + index]) == 0)
                    match = index;
            if (match == counts[scope] || (*mask & (UINT64_C(1) << (offset + match))))
                return false;
            *mask |= UINT64_C(1) << (offset + match);
        }
    return true;
}

static bool map_complete_domain(const Selection* selection, const UnityCompilerComputePreprocessResult* result,
                                const UnityComputeDomain* domain, size_t* target_rows) {
    const ComputeSourceCandidate* candidate = &selection->candidate;
    const size_t states = unity_compute_domain_state_count(domain);
    if (result->kernel_count != candidate->kernel_count || states != candidate->variant_count ||
        !attest_keyword_families(candidate, domain))
        return false;
    bool seen[COMPUTE_SOURCE_MAX_VARIANTS] = {false};
    for (size_t state_index = 0; state_index < states; ++state_index) {
        const char* keywords[COMPUTE_SOURCE_MAX_KEYWORDS];
        size_t required;
        UnityComputeDomainState state;
        UnityComputeDomainDiagnostic diagnostic;
        if (unity_compute_domain_state_at(domain, state_index, keywords, COMPUTE_SOURCE_MAX_KEYWORDS, &required, &state,
                                          &diagnostic) != UNITY_COMPUTE_DOMAIN_OK)
            return false;
        uint64_t mask;
        if (!state_mask(candidate, &state, &mask))
            return false;
        size_t match = candidate->variant_count;
        for (size_t row = 0; row < candidate->variant_count; ++row)
            if (strcmp(candidate->variants[row].kernel_name, state.kernel->name) == 0 &&
                candidate->variants[row].keyword_mask == mask) {
                if (match != candidate->variant_count)
                    return false;
                match = row;
            }
        if (match == candidate->variant_count || seen[match])
            return false;
        const ComputeSourceVariant* evidence = &candidate->variants[match];
        const ComputeShaderPlatformVariant* platform = &selection->object.platforms[evidence->platform_index];
        const ComputeShaderKernelParent* kernel = &platform->kernels[evidence->kernel_index];
        const ComputeShaderKernelVariant* variant = &kernel->variants[evidence->variant_index];
        if (!view_equal(kernel->name, state.kernel->name) || state.requirements != (uint64_t)variant->requirements ||
            !view_equal(variant->keyword_key, evidence->keyword_key))
            return false;
        seen[match] = true;
        target_rows[state_index] = match;
    }
    for (size_t row = 0; row < candidate->variant_count; ++row)
        if (!seen[row])
            return false;
    return true;
}

static bool compile_state(UnityCompilerChannel* channel, const Selection* selection,
                          const UnityComputeDomainState* state, size_t target_row, uint32_t compiler_platform,
                          const UnityCompilerToolchainProvenance* lease) {
    const ComputeSourceVariant* evidence = &selection->candidate.variants[target_row];
    const ComputeShaderPlatformVariant* platform = &selection->object.platforms[evidence->platform_index];
    const ComputeShaderKernelParent* kernel = &platform->kernels[evidence->kernel_index];
    const ComputeShaderKernelVariant* variant = &kernel->variants[evidence->variant_index];
    UnityCompilerComputeMacro macros[1024];
    char* keywords[COMPUTE_SOURCE_MAX_KEYWORDS];
    if (state->kernel->macro_count > 1024 || state->user_keyword_count > COMPUTE_SOURCE_MAX_KEYWORDS)
        return false;
    for (size_t index = 0; index < state->kernel->macro_count; ++index)
        macros[index] =
            (UnityCompilerComputeMacro){state->kernel->macros[index].name, state->kernel->macros[index].value};
    for (size_t index = 0; index < state->user_keyword_count; ++index)
        keywords[index] = (char*)state->user_keywords[index];
    const UnityCompilerComputePreprocessRequest* original = state->preprocess_request;
    const UnityCompilerComputePreprocessResult* result = state->preprocess_result;
    UnityCompilerComputeKernelRequest request = {.source = result->source,
                                                 .source_filename = original->source_filename,
                                                 .kernel_name = state->kernel->name,
                                                 .caching_preprocessor = original->caching_preprocessor,
                                                 .build_platform = original->build_platform,
                                                 .kernel_macros = macros,
                                                 .kernel_macro_count = (int)state->kernel->macro_count,
                                                 .platform_keywords = original->platform_keywords,
                                                 .platform_keyword_count = original->platform_keyword_count,
                                                 .user_keywords = keywords,
                                                 .user_keyword_count = (int)state->user_keyword_count,
                                                 .compiler_platform = (int32_t)compiler_platform,
                                                 .compilation_flags = result->compilation_flags,
                                                 .requirements = state->requirements,
                                                 .force_dxc = result->use_dxc_mask,
                                                 .force_fxc = result->never_use_dxc_mask};
    UnityCompilerBinaryResponse response;
    unity_compiler_binary_response_init(&response);
    bool ok = unity_compiler_compile_compute_response(channel, &request, &response);
    printf("state native_kernel=%zu native_variant=%zu target_platform=%zu target_kernel=%zu target_variant=%zu "
           "transport=%d availability=%s compiler_success=%d requirements=0x%016" PRIx64 "\n",
           state->kernel_index, state->variant_index, evidence->platform_index, evidence->kernel_index,
           evidence->variant_index, ok ? 1 : 0, availability_name(response.status.availability),
           response.status.compiler_success ? 1 : 0, request.requirements);
    print_view("target_keyword_key", variant->keyword_key);
    print_hash("target_complete_dxbc_sha256", variant->code, variant->code_size);
    if (response.has_request_identity) {
        print_digest("compile_request_sha256", response.request_digest);
        print_digest("compile_controls_sha256", response.controls_digest);
    }
    const size_t actionable = print_diagnostics(response.status.diagnostics, response.status.diagnostic_count);
    UnityCompilerToolchainProvenance provenance;
    ok = ok && response.has_request_identity && unity_compiler_response_status_is_clean_success(&response.status) &&
         replay_request(channel, false, &request, request.source, response.request_digest, &provenance) &&
         same_lease(lease, &provenance);
    UnityComputeVerifyReport report;
    unity_compute_verify_report_init(&report);
    if (ok) {
        const UnityComputeKernelExpectation expectation = {.kernel_name = kernel->name,
                                                           .variant = variant,
                                                           .target_level = platform->target_level,
                                                           .resources_resolved = platform->resources_resolved,
                                                           .selected_buffer_definitions = platform->constant_buffers,
                                                           .selected_buffer_definition_count =
                                                               platform->constant_buffer_count};
        ok =
            unity_compute_verify_kernel(&expectation, response.data, response.size, &report) == UNITY_COMPUTE_VERIFY_OK;
        printf("state_comparison=%s dxbc_equal=%d common_metadata_equal=%d declared_groups_equal=%d "
               "compiler_diagnostics=%zu actionable=%zu target_bytes=%zu native_bytes=%zu\n",
               unity_compute_verify_status_name(report.status), report.dxbc_equal ? 1 : 0,
               report.common_metadata_equal ? 1 : 0,
               report.expected_group_matches_code && report.actual_group_matches_code ? 1 : 0,
               response.status.diagnostic_count, actionable, variant->code_size, response.size);
    }
    unity_compiler_binary_response_free(&response);
    return ok;
}

static void usage(const char* executable) {
    fprintf(
        stderr,
        "usage: %s RELEASED_INPUT COMPUTE_OBJECT_NAME PROJECT_ROOT BUILD_PLATFORM COMPILER_PLATFORM [INCLUDES_DIR]\n",
        executable ? executable : "compute_domain_probe");
    fputs("Explicit decimal controls; native D3D11 platform 4 only. '-' means no extra includes.\n"
          "Reads released bytes and reconstructs source; exports no source or binary files. Observes every supported "
          "returned "
          "kernel/keyword state.\n"
          "No original-control, Class72 producer, import, semantic or physical Windows certificate.\n",
          stderr);
}

int main(int argc, char** argv) {
    uint32_t build_platform, compiler_platform;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    if ((argc != 6 && argc != 7) || !parse_u32(argv[4], &build_platform) || !parse_u32(argv[5], &compiler_platform) ||
        compiler_platform != D3D11_COMPILER_PLATFORM) {
        usage(argv[0]);
        return 2;
    }
    Selection selection = {.requested_name = argv[2]};
    compute_shader_object_init(&selection.object);
    compute_source_candidate_init(&selection.candidate);
    UnityInputSnapshot snapshot;
    unity_input_snapshot_init(&snapshot);
    UnityCompilerChannel channel = {.socket_fd = -1};
    UnityCompilerComputePreprocessResponse* preprocess = NULL;
    UnityComputeDomain* domain = NULL;
    char* source_filename = NULL;
    size_t* target_rows = NULL;
    size_t attempts = 0, matched = 0, planned = 0;
    bool lease_unchanged = false, input_unchanged = false;
    int exit_code = 1;
    UnityInputProbe probe;
    UnityInputVisitStats stats;
    uint8_t input_digest[COMMON_SHA256_DIGEST_SIZE];
    if (unity_input_probe_path(argv[1], &probe) != UNITY_INPUT_OK || probe.file_size > INPUT_LIMIT ||
        unity_input_snapshot_open(argv[1], &snapshot) != UNITY_INPUT_OK ||
        unity_input_snapshot_digest(&snapshot, input_digest) != UNITY_INPUT_OK ||
        unity_input_snapshot_visit(&snapshot, select_source, &selection, &stats) != UNITY_INPUT_OK ||
        selection.failed || selection.matching_objects != 1) {
        fprintf(stderr, "released input selection unavailable or ambiguous; exact_name_matches=%zu\n",
                selection.matching_objects);
        goto done;
    }
    /* Class72 renderer 2 and native compiler platform 4 identify D3D11 in
     * different enum domains. Their integer values must not be equated. */
    if (selection.object.platform_count != 1 ||
        selection.object.platforms[0].target_renderer != D3D11_CLASS72_RENDERER) {
        fputs("selected object does not expose the admitted Class72 D3D11 renderer\n", stderr);
        goto done;
    }
    print_digest("released_input_sha256", input_digest);
    print_digest("serialized_member_sha256", selection.member_digest);
    print_digest("serialized_object_sha256", selection.candidate.serialized_object_sha256);
    print_digest("modeled_input_sha256", selection.candidate.modeled_input_sha256);
    print_digest("reconstructed_source_sha256", selection.candidate.source_sha256);
    printf("selected member=%zu path_id=%" PRId64
           " target_platform=%u target_renderer=%d source_bytes=%zu source_quality=%s "
           "requested_kernels=%zu requested_variants=%zu\n",
           selection.member_index, selection.object.path_id, selection.object.target_platform,
           selection.object.platforms[0].target_renderer, selection.candidate.source.len,
           hlsl_source_quality_class_name(selection.candidate.source_quality.classification),
           selection.candidate.kernel_count, selection.candidate.variant_count);
    for (size_t row = 0; row < selection.candidate.variant_count; ++row) {
        printf("requested_row=%zu kernel=%s serialized_variant=%zu mask=0x%016" PRIx64 " key=%s\n", row,
               selection.candidate.variants[row].kernel_name, selection.candidate.variants[row].variant_index,
               selection.candidate.variants[row].keyword_mask, selection.candidate.variants[row].keyword_key);
    }
    source_filename = common_output_join_path(argv[3], "DXBCSandboxGenerated.compute");
    const char* includes = argc == 7 && strcmp(argv[6], "-") != 0 ? argv[6] : NULL;
    UnityCompilerSessionCapabilities capabilities;
    uint32_t valid_apis;
    if (!source_filename || !unity_compiler_start_lazy(&channel, argv[3], includes) ||
        !unity_compiler_capture_session_capabilities(&channel, &capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(&capabilities, &valid_apis) ||
        !(valid_apis & (UINT32_C(1) << compiler_platform)) ||
        !unity_compiler_set_expected_valid_apis(&channel, valid_apis))
        goto done;
    printf("fresh_compiler_process=1 source_submissions=derived_source_only build_platform=%u compiler_platform=%u "
           "session_valid_apis=0x%08" PRIx32 "\n",
           build_platform, compiler_platform, valid_apis);
    UnityCompilerComputePreprocessRequest request = {.source = selection.candidate.source.buf,
                                                     .source_filename = source_filename,
                                                     .caching_preprocessor = true,
                                                     .build_platform = build_platform,
                                                     .valid_apis = valid_apis};
    if (!unity_compiler_preprocess_compute_response(&channel, &request, &preprocess))
        goto done;
    UnityCompilerComputePreprocessInfo info;
    if (!unity_compiler_compute_preprocess_response_info(preprocess, &info))
        goto done;
    printf("preprocess transport_complete=%d availability=%s native_success_present=%d\n",
           info.transport_complete ? 1 : 0, availability_name(info.availability), info.native_success_present ? 1 : 0);
    size_t diagnostic_count;
    const UnityCompilerDiagnostic* diagnostics =
        unity_compiler_compute_preprocess_response_diagnostics(preprocess, &diagnostic_count);
    const size_t actionable = print_diagnostics(diagnostics, diagnostic_count);
    const UnityCompilerComputePreprocessResult* result = unity_compiler_compute_preprocess_response_result(preprocess);
    UnityCompilerToolchainProvenance lease;
    if (!result || !info.transport_complete || info.native_success_present || !info.has_request_identity ||
        info.availability != UNITY_COMPILER_RESPONSE_AVAILABLE || actionable ||
        !replay_request(&channel, true, &request, request.source, info.request_digest, &lease))
        goto done;
    print_digest("preprocess_request_sha256", info.request_digest);
    print_digest("preprocess_controls_sha256", info.controls_digest);
    print_digest("compiler_fingerprint", lease.compiler_fingerprint);
    print_digest("environment_fingerprint", lease.environment_fingerprint);
    print_hash("returned_source_sha256", result->source, result->source_size);
    printf("returned_controls requirements=0x%016" PRIx64 " flags=0x%08" PRIx32 " supported_apis=0x%08" PRIx32
           " force_dxc=0x%08" PRIx32 " force_fxc=0x%08" PRIx32 " dependencies=%zu\n",
           result->requirements, result->compilation_flags, (uint32_t)result->supported_apis, result->use_dxc_mask,
           result->never_use_dxc_mask, result->dependency_count);
    const UnityComputeDomainContext context = {.preprocess_request = &request,
                                               .max_variant_count = COMPUTE_SOURCE_MAX_VARIANTS,
                                               .max_kernel_state_count = COMPUTE_SOURCE_MAX_VARIANTS};
    UnityComputeDomainDiagnostic domain_diagnostic;
    UnityComputeDomainStatus domain_status = unity_compute_domain_create(result, &context, &domain, &domain_diagnostic);
    if (domain_status != UNITY_COMPUTE_DOMAIN_OK ||
        unity_compute_domain_require_api(domain, compiler_platform) != UNITY_COMPUTE_DOMAIN_OK) {
        fprintf(stderr, "returned domain unavailable: status=%d scope=%d family=%zu choice=%zu\n", (int)domain_status,
                (int)domain_diagnostic.scope, domain_diagnostic.family_index, domain_diagnostic.choice_index);
        goto done;
    }
    planned = unity_compute_domain_state_count(domain);
    target_rows = calloc(selection.candidate.variant_count, sizeof(*target_rows));
    if (!target_rows || !map_complete_domain(&selection, result, domain, target_rows)) {
        fputs("returned complete kernel/keyword/requirements domain does not uniquely match the decoded target\n",
              stderr);
        goto done;
    }
    printf("complete_kernel_keyword_domain_matched=1 planned_states=%zu target_rows=%zu\n", planned,
           selection.candidate.variant_count);
    for (size_t index = 0; index < planned; ++index) {
        const char* keywords[COMPUTE_SOURCE_MAX_KEYWORDS];
        size_t required;
        UnityComputeDomainState state;
        if (unity_compute_domain_state_at(domain, index, keywords, COMPUTE_SOURCE_MAX_KEYWORDS, &required, &state,
                                          &domain_diagnostic) != UNITY_COMPUTE_DOMAIN_OK)
            goto done;
        ++attempts;
        if (compile_state(&channel, &selection, &state, target_rows[index], compiler_platform, &lease))
            ++matched;
    }
    UnityCompilerToolchainProvenance final_lease;
    lease_unchanged = request_provenance(&channel, result->source, &final_lease) && same_lease(&lease, &final_lease);
    uint8_t final_digest[COMMON_SHA256_DIGEST_SIZE];
    input_unchanged = unity_input_snapshot_digest(&snapshot, final_digest) == UNITY_INPUT_OK &&
                      memcmp(input_digest, final_digest, sizeof(input_digest)) == 0;
    exit_code = planned && attempts == planned && matched == planned && lease_unchanged && input_unchanged ? 0 : 1;
done:
    unity_compute_domain_free(domain);
    unity_compiler_compute_preprocess_response_free(preprocess);
    unity_compiler_shutdown(&channel);
    free(target_rows);
    free(source_filename);
    compute_source_candidate_dispose(&selection.candidate);
    compute_shader_object_dispose(&selection.object);
    if (selection.selected_file_open)
        serialized_file_close(&selection.selected_file);
    if (unity_input_snapshot_is_open(&snapshot) && unity_input_snapshot_close(&snapshot) != UNITY_INPUT_OK)
        exit_code = 1;
    printf("selected_domain_observation=%s requested=%zu planned=%zu attempted=%zu exact_metadata_matches=%zu "
           "final_input_unchanged=%d final_compiler_include_lease_unchanged=%d\n",
           exit_code == 0 ? "passed" : "failed", selection.requested_variants, planned, attempts, matched,
           input_unchanged ? 1 : 0, lease_unchanged ? 1 : 0);
    puts("scope=selected-domain-compiler-comparison original-control-certificate=0 Class72-producer=0 "
         "Editor-import=0 semantic-certificate=0 physical-Windows=0 warm-replay=not-run");
    return exit_code;
}
