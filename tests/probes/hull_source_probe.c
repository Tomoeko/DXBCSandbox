// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/sha256.h"
#include "compiler/unity_compiler_client.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"
#include "translation/usil_validation.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROBE_SOURCE_LIMIT = 1024 * 1024, PROBE_DIRECTORY_LIMIT = 4096 };

/* Manual selected-native HULL comparison. V/D/F are authored compilation
 * stubs. It grants no Editor, import, linked-stage or runtime certificate. */
static const char *const probe_shader_name =
    "Fixture/HighLevel/HullFloat3Implicit";

static void print_digest(const char *role, const uint8_t digest[32]) {
    char hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(digest, hex);
    printf("%s=%s\n", role, hex);
}

static void print_hash(const char *role, const void *bytes, size_t size) {
    uint8_t digest[32];
    common_sha256(bytes, size, digest);
    print_digest(role, digest);
    printf("%s_bytes=%zu\n", role, size);
}

static void print_status(const char *role,
                         const UnityCompilerResponseStatus *status) {
    printf("%s success=%d diagnostics=%zu actionable=%zu\n", role,
           status->compiler_success, status->diagnostic_count,
           unity_compiler_response_status_actionable_diagnostic_count(status));
    for (size_t index = 0; index < status->diagnostic_count; ++index) {
        const UnityCompilerDiagnostic *diagnostic = &status->diagnostics[index];
        fprintf(stderr, "%s diagnostic=%d,%d,%d message=%s\n", role,
                diagnostic->fields[0], diagnostic->fields[1],
                diagnostic->fields[2],
                diagnostic->message ? diagnostic->message : "");
    }
}

static bool source_directory(const char *path,
                             char output[PROBE_DIRECTORY_LIMIT]) {
    const char *slash = strrchr(path, '/');
    size_t length = slash ? (size_t)(slash - path) : 1;
    if (!length)
        length = 1;
    if (length >= PROBE_DIRECTORY_LIMIT)
        return false;
    memcpy(output, slash ? path : ".", length);
    output[length] = '\0';
    return true;
}

static bool retain_provenance(UnityCompilerChannel *channel, const char *source,
                              UnityCompilerToolchainProvenance *provenance) {
    if (!channel->cache_source_root)
        return false;
    const size_t size = strlen(channel->cache_source_root) + 1;
    char *root = malloc(size);
    if (!root)
        return false;
    memcpy(root, channel->cache_source_root, size);
    /* A lease refresh may replace the channel-owned root. */
    const bool captured = unity_compiler_get_request_provenance(
        channel, root, source, provenance);
    free(root);
    return captured;
}

static bool
verify_preprocess_identity(UnityCompilerChannel *channel,
                           const UnityCompilerShaderPreprocessRequest *request,
                           const UnityCompilerPreprocessResponse *response) {
    UnityCompilerToolchainProvenance provenance;
    uint8_t *transcript = NULL, digest[32];
    size_t size = 0;
    const bool matches =
        response->has_request_identity &&
        retain_provenance(channel, request->source, &provenance) &&
        unity_compiler_serialize_preprocess_request(
            channel, request, &transcript, &size, digest) &&
        !memcmp(digest, response->request_digest, sizeof(digest)) &&
        provenance.source_authority_revision ==
            channel->source_authority_revision;
    free(transcript);
    if (matches)
        print_digest("preprocess_request_sha256", response->request_digest);
    return matches;
}

static bool
verify_compile_identity(UnityCompilerChannel *channel,
                        const UnityCompilerSnippetCompileRequest *request,
                        const UnityCompilerBinaryResponse *response,
                        UnityCompilerToolchainProvenance *provenance) {
    uint8_t *transcript = NULL, digest[32];
    size_t size = 0;
    const bool matches =
        response->has_request_identity &&
        retain_provenance(channel, request->snippet_source, provenance) &&
        unity_compiler_serialize_compile_request(channel, request, &transcript,
                                                 &size, digest) &&
        !memcmp(digest, response->request_digest, sizeof(digest)) &&
        provenance->source_authority_revision ==
            channel->source_authority_revision;
    free(transcript);
    if (matches) {
        print_digest("compile_request_sha256", response->request_digest);
        print_digest("compile_controls_sha256", response->controls_digest);
        print_digest("compiler_fingerprint", provenance->compiler_fingerprint);
        print_digest("environment_fingerprint",
                     provenance->environment_fingerprint);
        printf("canonical_digest_matches=1 canonical_bytes=%zu "
               "source_revision=%" PRIu64 "\n",
               size, provenance->source_authority_revision);
    }
    return matches;
}

static bool compile_source(UnityCompilerChannel *channel, const char *role,
                           const char *source, const char *directory,
                           uint32_t valid_apis,
                           UnityCompilerPreprocessResponse *preprocess,
                           UnityCompilerSnippetCompileRequest *request,
                           UnityCompilerBinaryResponse *compiled,
                           UnityCompilerToolchainProvenance *provenance) {
    printf("request_role=%s\n", role);
    const UnityCompilerShaderPreprocessRequest preprocessing = {
        .source = source,
        .source_directory = directory,
        .shader_name = probe_shader_name,
        .caching_preprocessor = true,
        .build_platform = 1,
        .valid_apis = valid_apis};
    if (!unity_compiler_preprocess_contract_response(channel, &preprocessing,
                                                     preprocess))
        return false;
    print_status("preprocess", &preprocess->status);
    if (!unity_compiler_response_status_is_clean_success(&preprocess->status) ||
        !verify_preprocess_identity(channel, &preprocessing, preprocess) ||
        preprocess->result.snippet_count != 1 || !preprocess->result.snippets ||
        !preprocess->result.snippets[0].has_contract)
        return false;
    const PreprocessedSnippet *snippet = &preprocess->result.snippets[0];
    if (!unity_compiler_snippet_contract_validate(&snippet->contract))
        return false;
    *request = (UnityCompilerSnippetCompileRequest){
        .snippet_source = snippet->source,
        .source_directory = directory,
        .source_basename = "HullFloat3Implicit",
        .pass_name = "",
        .caching_preprocessor = true,
        /* Explicit selected-native experiment policy, not inferred Editor
         * flags.
         */
        .build_platform = 1,
        .compiler_flags = UINT32_C(0x9000),
        .shader_type = UNITY_COMPILER_PROGRAM_HULL,
        .platform = 4,
        .requirements = unity_compiler_variant_requirements(snippet, NULL, 0),
        .program_mask = (int32_t)snippet->contract.program_types_mask,
        .program_start = snippet->contract.start_line,
        .contract = &snippet->contract};
    printf("compile build=1 platform=4 stage=%d flags=%" PRIu32
           " requirements=%" PRIu64
           " program_mask=%d start=%d caching=1 pKW=0 uKW=0 dKW=0\n",
           request->shader_type, request->compiler_flags, request->requirements,
           request->program_mask, request->program_start);
    printf("contract flags=%" PRIu32
           " language=%d platforms=%d use_dxc=%d never_dxc=%d\n",
           snippet->contract.compilation_flags, snippet->contract.language,
           snippet->contract.platforms, snippet->contract.use_dxc_apis,
           snippet->contract.never_use_dxc_apis);
    if (!unity_compiler_compile_contract_response(channel, request, compiled))
        return false;
    print_status("compile", &compiled->status);
    return unity_compiler_response_status_is_clean_success(&compiled->status) &&
           verify_compile_identity(channel, request, compiled, provenance);
}

/* The source inverse consumes only the compiler's complete target container.
 * Neither authored source nor its preprocessing contract enters this function.
 */
static bool reconstruct_hull(const DXBCContainerView *target,
                             StringBuilder *source) {
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    USILProgram program = {0};
    bool accepted = false;
    if (!dxbc_document_parse(&document, target->data, target->size, NULL) ||
        !dxbc_document_decode_semantic(&document, &semantic) ||
        !dxbc_stage_contract_decode(&document, &semantic, &contract, NULL) ||
        !usil_translate_with_stage_contract(&program, &semantic, &contract))
        goto done;
    /* The authored wrapper below supplies only this explicit evaluation shape.
     */
    if (program.program_type != DXBC_PROGRAM_TYPE_HULL ||
        program.tessellation.domain != DXBC_TESSELLATOR_DOMAIN_TRIANGLE ||
        program.tessellation.input_control_point_count != 3 ||
        program.tessellation.output_control_point_count != 3 ||
        program.input_count != 1 || program.output_count != 1 ||
        program.inputs[0].mask != 7 || program.outputs[0].mask != 7)
        goto done;
    HLSLExpressionSourceMap map = {0};
    HLSLSourceQualityResult quality = {0};
    HLSLEmitDiagnostic diagnostic;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    const HLSLEmitNames names = {.entry_point = "hull"};
    if (!hlsl_emit_with_options_diagnostic(&program, source, NULL, NULL, &names,
                                           &options, &diagnostic)) {
        fprintf(stderr, "source status=%s phase=%s reason=%s instruction=%d\n",
                hlsl_emit_status_name(diagnostic.status),
                hlsl_emit_phase_name(diagnostic.phase),
                hlsl_emit_reason_name(diagnostic.reason),
                diagnostic.instruction_index);
        goto done;
    }
    const bool map_valid =
        hlsl_expression_source_map_matches(&map, &program, source->buf);
    accepted = quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
               !quality.counts.unknown_provenance &&
               !quality.counts.incomplete_units && map_valid;
    printf("source scope=target-only-hull quality=%s map_valid=%d "
           "instructions=%d units=%zu "
           "input_mask=%u input_rw=%u output_mask=%u output_rw=%u\n",
           hlsl_source_quality_class_name(quality.classification), map_valid,
           program.instruction_count, quality.counts.inspected_units,
           program.inputs[0].mask, program.inputs[0].rw_mask,
           program.outputs[0].mask, program.outputs[0].rw_mask);
done:
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    return accepted;
}

static bool candidate_wrapper(const StringBuilder *hull,
                              StringBuilder *wrapper) {
    sb_appendf(
        wrapper,
        "// SPDX-License-Identifier: GPL-3.0-only\n"
        "// Other stages are authored evaluation stubs.\nShader \"%s\"\n{\n"
        "    SubShader\n    {\n        Pass\n        {\n            "
        "HLSLPROGRAM\n"
        "#pragma target 5.0\n#pragma only_renderers d3d11\n#pragma vertex "
        "vert\n"
        "#pragma hull hull\n#pragma domain domain\n#pragma fragment frag\n%s\n"
        "HullPoint vert(float4 input : POSITION) {\n"
        "    HullPoint result; result.pointValue = input.xyz; return "
        "result;\n}\n"
        "[domain(\"tri\")]\n"
        "float4 domain(HullFactors factors, const OutputPatch<HullPoint, 3> "
        "patch,\n"
        "              float3 weights : SV_DomainLocation) : SV_POSITION {\n"
        "    float3 value = patch[0].pointValue * weights.x + "
        "patch[1].pointValue * weights.y\n"
        "                 + patch[2].pointValue * weights.z;\n    return "
        "float4(value, 1.0f);\n}\n"
        "float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); "
        "}\n"
        "ENDHLSL\n        }\n    }\n}\n",
        probe_shader_name, hull->buf);
    return sb_ok(wrapper);
}

static bool changed_factor_wrapper(const StringBuilder *hull,
                                   StringBuilder *wrapper) {
    static const char original[] = "factors.outer[factorIndex] = 3.0f;";
    static const char changed[] = "factors.outer[factorIndex] = 5.0f;";
    /* This is an adversary for the explicit constant-factor fixture, not an
     * alternate source producer. Match once inside the owned inverse text. */
    if (!sb_ok(hull) || !hull->buf || hull->len > PROBE_SOURCE_LIMIT)
        return false;
    const char *match = strstr(hull->buf, original);
    if (!match || strstr(match + sizeof(original) - 1, original))
        return false;
    StringBuilder mutated;
    sb_init(&mutated);
    sb_append_len(&mutated, hull->buf, (size_t)(match - hull->buf));
    sb_append(&mutated, changed);
    sb_append(&mutated, match + sizeof(original) - 1);
    const bool built = sb_ok(&mutated) && candidate_wrapper(&mutated, wrapper);
    if (built) {
        printf("warm_mutation matched_owned_inverse_occurrences=1 "
               "outer_factor_before=3.0f outer_factor_after=5.0f\n");
        print_hash("mutated_hull_sha256", mutated.buf, mutated.len);
    }
    sb_free(&mutated);
    return built;
}

static bool string_arrays_equal(char *const *left, int left_count,
                                char *const *right, int right_count) {
    if (left_count != right_count || left_count < 0 ||
        (left_count && (!left || !right)))
        return false;
    for (int index = 0; index < left_count; ++index)
        if (!left[index] || !right[index] || strcmp(left[index], right[index]))
            return false;
    return true;
}

static bool keyword_sets_equal(const SnippetKeywordVariantSet *left,
                               const SnippetKeywordVariantSet *right) {
    return left->present == right->present &&
           string_arrays_equal(left->combinations, left->combination_count,
                               right->combinations, right->combination_count);
}

static bool keyword_contracts_equal(const SnippetCompileContract *left,
                                    const SnippetCompileContract *right) {
    if (!string_arrays_equal(left->non_stripped_user_keywords,
                             left->non_stripped_user_keyword_count,
                             right->non_stripped_user_keywords,
                             right->non_stripped_user_keyword_count) ||
        !string_arrays_equal(
            left->builtin_keywords, left->builtin_keyword_count,
            right->builtin_keywords, right->builtin_keyword_count) ||
        left->program_keyword_variant_count !=
            right->program_keyword_variant_count ||
        left->conditional_requirement_count !=
            right->conditional_requirement_count)
        return false;
    for (int index = 0; index < left->program_keyword_variant_count; ++index) {
        const SnippetProgramKeywordVariants *x =
            &left->program_keyword_variants[index];
        const SnippetProgramKeywordVariants *y =
            &right->program_keyword_variants[index];
        if (x->compiler_program != y->compiler_program ||
            !keyword_sets_equal(&x->user_global, &y->user_global) ||
            !keyword_sets_equal(&x->user_local, &y->user_local) ||
            !keyword_sets_equal(&x->builtin, &y->builtin))
            return false;
    }
    for (int index = 0; index < left->conditional_requirement_count; ++index) {
        const ConditionalShaderRequirement *x =
            &left->conditional_requirements[index];
        const ConditionalShaderRequirement *y =
            &right->conditional_requirements[index];
        if (x->requirements != y->requirements ||
            strcmp(x->keyword, y->keyword))
            return false;
    }
    return true;
}

static bool
selected_controls_equal(const UnityCompilerSnippetCompileRequest *left,
                        const UnityCompilerSnippetCompileRequest *right) {
    const SnippetCompileContract *x = left->contract, *y = right->contract;
    /* These are measured selected wire controls. Both complete actual contracts
     * remain canonicalized independently; differing source hashes/start lines
     * are reported, never rewritten to assert whole request equivalence. */
    return left->compiler_flags == right->compiler_flags &&
           left->requirements == right->requirements &&
           left->program_mask == right->program_mask &&
           left->build_platform == right->build_platform &&
           left->platform == right->platform &&
           left->shader_type == right->shader_type &&
           x->compilation_flags == y->compilation_flags &&
           x->language == y->language && x->platforms == y->platforms &&
           x->quality_variants == y->quality_variants &&
           x->use_dxc_apis == y->use_dxc_apis &&
           x->never_use_dxc_apis == y->never_use_dxc_apis &&
           keyword_contracts_equal(x, y);
}

static bool
compiler_environment_equal(const UnityCompilerToolchainProvenance *left,
                           const UnityCompilerToolchainProvenance *right) {
    return !memcmp(left->compiler_fingerprint, right->compiler_fingerprint,
                   32) &&
           !memcmp(left->environment_fingerprint,
                   right->environment_fingerprint, 32);
}

static void usage(const char *name) {
    fprintf(
        stderr,
        "usage: %s SOURCE.shader PROJECT_ROOT INCLUDES_DIR\n"
        "Use '-' for no additional includes. Selected-native HULL comparison "
        "only; no files written.\n",
        name);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }
    if (argc != 4) {
        usage(argv[0]);
        return 2;
    }
    CommonFileBytes authored = {0};
    UnityCompilerChannel channel = {.socket_fd = -1};
    UnityCompilerPreprocessResponse preprocessing[4];
    UnityCompilerBinaryResponse compiled[4];
    UnityCompilerSnippetCompileRequest requests[4] = {0};
    UnityCompilerToolchainProvenance provenance[4];
    StringBuilder hull, wrapper, changed_wrapper;
    sb_init(&hull);
    sb_init(&wrapper);
    sb_init(&changed_wrapper);
    for (unsigned index = 0; index < 4; ++index) {
        unity_compiler_preprocess_response_init(&preprocessing[index]);
        unity_compiler_binary_response_init(&compiled[index]);
    }
    int result = 1;
    char directory[PROBE_DIRECTORY_LIMIT];
    UnityCompilerSessionCapabilities capabilities;
    uint32_t valid_apis = 0;
    if (common_file_read_regular_terminated(argv[1], PROBE_SOURCE_LIMIT,
                                            &authored) != COMMON_FILE_OK ||
        !authored.data || memchr(authored.data, 0, authored.size) ||
        !source_directory(argv[1], directory))
        goto done;
    if (!unity_compiler_start_lazy(&channel, argv[2],
                                   !strcmp(argv[3], "-") ? NULL : argv[3]) ||
        !unity_compiler_capture_session_capabilities(&channel, &capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(&capabilities,
                                                        &valid_apis) ||
        !(valid_apis & (UINT32_C(1) << 4)) ||
        !unity_compiler_set_expected_valid_apis(&channel, valid_apis))
        goto done;
    printf("scope=selected-native-hull authored_stubs=vertex,domain,fragment "
           "editor=not-run "
           "import=not-run semantic_certificate=not-run native_D3D11=not-run\n"
           "session raw_mask=0x%08" PRIx32 " valid_apis=0x%08" PRIx32 "\n",
           capabilities.raw_available_platform_mask, valid_apis);
    print_hash("authored_source_sha256", authored.data, authored.size);
    if (!compile_source(&channel, "authored-target", (char *)authored.data,
                        directory, valid_apis, &preprocessing[0], &requests[0],
                        &compiled[0], &provenance[0]))
        goto done;
    DXBCContainerView target = {0}, candidate = {0};
    if (!dxbc_container_view_first(compiled[0].data, compiled[0].size,
                                   &target) ||
        !reconstruct_hull(&target, &hull) ||
        !candidate_wrapper(&hull, &wrapper))
        goto done;
    print_hash("target_complete_dxbc_sha256", target.data, target.size);
    print_hash("reconstructed_hull_sha256", hull.buf, hull.len);
    printf("generated_hull_begin\n%s\ngenerated_hull_end\n", hull.buf);
    /* Retain the owned target response and contract, but remove all native
     * resident source before the candidate is independently preprocessed. */
    unity_compiler_shutdown(&channel);
    channel = (UnityCompilerChannel){.socket_fd = -1};
    UnityCompilerSessionCapabilities candidate_capabilities;
    uint32_t candidate_valid_apis = 0;
    if (!unity_compiler_start_lazy(&channel, argv[2],
                                   !strcmp(argv[3], "-") ? NULL : argv[3]) ||
        !unity_compiler_capture_session_capabilities(&channel,
                                                     &candidate_capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(
            &candidate_capabilities, &candidate_valid_apis) ||
        candidate_valid_apis != valid_apis ||
        !unity_compiler_session_capabilities_equal(&capabilities,
                                                   &candidate_capabilities) ||
        !unity_compiler_set_expected_valid_apis(&channel, valid_apis))
        goto done;
    printf(
        "process_count=2 candidate_cold=1 session_capabilities_identical=1\n");
    if (!compile_source(&channel, "target-only-hull-candidate", wrapper.buf,
                        directory, valid_apis, &preprocessing[1], &requests[1],
                        &compiled[1], &provenance[1]) ||
        !dxbc_container_view_first(compiled[1].data, compiled[1].size,
                                   &candidate))
        goto done;
    const bool controls_equal =
        selected_controls_equal(&requests[0], &requests[1]);
    const bool toolchain_equal =
        compiler_environment_equal(&provenance[0], &provenance[1]);
    printf(
        "selected_wire_scalars_and_keyword_records_equal=%d "
        "compiler_environment_equal=%d "
        "complete_controls_digest_identical=%d original_start=%d "
        "candidate_start=%d "
        "preprocess_source_hash_identical=%d\n",
        controls_equal, toolchain_equal,
        !memcmp(compiled[0].controls_digest, compiled[1].controls_digest, 32),
        requests[0].program_start, requests[1].program_start,
        !memcmp(requests[0].contract->source_hash,
                requests[1].contract->source_hash,
                sizeof(requests[0].contract->source_hash)));
    DXBCCompareResult comparison;
    const DXBCCompareStatus status = dxbc_compare_exact(
        target.data, target.size, candidate.data, candidate.size, &comparison);
    print_hash("candidate_complete_dxbc_sha256", candidate.data,
               candidate.size);
    printf("complete_container_comparison=%s expected_bytes=%zu "
           "actual_bytes=%zu\n",
           dxbc_compare_status_name(status), target.size, candidate.size);
    if (!controls_equal || !toolchain_equal || status != DXBC_COMPARE_EQUAL)
        goto done;

    /* A changed source must affect a warm compiler, and returning to the
     * original source must restore both exact bytes and canonical identity. */
    const pid_t candidate_process = channel.process_id;
    DXBCContainerView changed = {0}, repeated = {0};
    if (!candidate_process ||
        !changed_factor_wrapper(&hull, &changed_wrapper) ||
        !compile_source(&channel, "warm-mutated-hull", changed_wrapper.buf,
                        directory, valid_apis, &preprocessing[2], &requests[2],
                        &compiled[2], &provenance[2]) ||
        !dxbc_container_view_first(compiled[2].data, compiled[2].size,
                                   &changed))
        goto done;
    const DXBCCompareStatus mutation_status = dxbc_compare_exact(
        target.data, target.size, changed.data, changed.size, &comparison);
    const bool mutation_diff =
        mutation_status != DXBC_COMPARE_EQUAL &&
        mutation_status != DXBC_COMPARE_INVALID_ARGUMENT &&
        mutation_status != DXBC_COMPARE_EXPECTED_INVALID &&
        mutation_status != DXBC_COMPARE_ACTUAL_INVALID;
    const bool mutation_authority =
        selected_controls_equal(&requests[1], &requests[2]) &&
        compiler_environment_equal(&provenance[1], &provenance[2]) &&
        provenance[1].source_authority_revision ==
            provenance[2].source_authority_revision &&
        candidate_process == channel.process_id;
    print_hash("warm_mutation_complete_dxbc_sha256", changed.data,
               changed.size);
    printf("warm_mutation_diff=%d comparison=%s authority_equal=%d "
           "same_process=%d\n",
           mutation_diff, dxbc_compare_status_name(mutation_status),
           mutation_authority, candidate_process == channel.process_id);
    if (!mutation_diff || !mutation_authority)
        goto done;
    if (!compile_source(&channel, "warm-original-hull", wrapper.buf, directory,
                        valid_apis, &preprocessing[3], &requests[3],
                        &compiled[3], &provenance[3]) ||
        !dxbc_container_view_first(compiled[3].data, compiled[3].size,
                                   &repeated))
        goto done;
    const DXBCCompareStatus repeated_status = dxbc_compare_exact(
        target.data, target.size, repeated.data, repeated.size, &comparison);
    const bool repeated_identity =
        !memcmp(compiled[1].request_digest, compiled[3].request_digest, 32) &&
        !memcmp(compiled[1].controls_digest, compiled[3].controls_digest, 32);
    const bool repeated_authority =
        selected_controls_equal(&requests[1], &requests[3]) &&
        compiler_environment_equal(&provenance[1], &provenance[3]) &&
        provenance[1].source_authority_revision ==
            provenance[3].source_authority_revision &&
        candidate_process == channel.process_id;
    print_hash("warm_original_complete_dxbc_sha256", repeated.data,
               repeated.size);
    printf("warm_original_equal=%d comparison=%s canonical_identity_equal=%d "
           "authority_equal=%d same_process=%d\n",
           repeated_status == DXBC_COMPARE_EQUAL,
           dxbc_compare_status_name(repeated_status), repeated_identity,
           repeated_authority, candidate_process == channel.process_id);
    result = repeated_status == DXBC_COMPARE_EQUAL && repeated_identity &&
                     repeated_authority
                 ? 0
                 : 1;
done:
    for (unsigned index = 0; index < 4; ++index) {
        unity_compiler_binary_response_free(&compiled[index]);
        unity_compiler_preprocess_response_free(&preprocessing[index]);
    }
    unity_compiler_shutdown(&channel);
    common_file_bytes_dispose(&authored);
    sb_free(&hull);
    sb_free(&wrapper);
    sb_free(&changed_wrapper);
    return result;
}
