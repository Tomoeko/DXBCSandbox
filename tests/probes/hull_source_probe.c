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
static const char *const default_shader_name =
    "Fixture/HighLevel/HullFloat3Implicit";

/* Explicit API calibration, never a player metadata capture. The supplied
 * fixture layout is checked against native callbacks, rather than treating
 * those callbacks as serialized current/common declaration authority. */
static bool scalar_fixture_reflection(const UnityCompilerBinaryResponse *response) {
    if (!response || !response->reflection_records || !response->reflection_record_count)
        return false;
    const int32_t expected_values[][6] = {{16, 1}, {0, 0, 0, 1, 1, 0}, {0}};
    const UnityCompilerReflectionKind kinds[] = {
        UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER, UNITY_COMPILER_REFLECTION_CONSTANT,
        UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER_BINDING};
    const char *const names[] = {"FactorInputs", "_Factor", "FactorInputs"};
    const size_t counts[] = {2, 6, 1};
    unsigned seen = 0;
    for (size_t index = 0; index < response->reflection_record_count; ++index) {
        const UnityCompilerReflectionRecord *record = &response->reflection_records[index];
        if (record->kind == UNITY_COMPILER_REFLECTION_INPUT ||
            record->kind == UNITY_COMPILER_REFLECTION_STATS) continue;
        unsigned expected = 0;
        while (expected < 3 && record->kind != kinds[expected]) ++expected;
        if (expected == 3 || (seen & (1u << expected)) || !record->name ||
            strcmp(record->name, names[expected]) || record->value_count != counts[expected] ||
            memcmp(record->values, expected_values[expected], counts[expected] * sizeof(int32_t)))
            return false;
        seen |= 1u << expected;
    }
    return seen == 7;
}

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
                           const char *source, const char *directory, const char *shader_name,
                           uint32_t valid_apis,
                           UnityCompilerPreprocessResponse *preprocess,
                           UnityCompilerSnippetCompileRequest *request,
                           UnityCompilerBinaryResponse *compiled,
                           UnityCompilerToolchainProvenance *provenance) {
    printf("request_role=%s\n", role);
    const UnityCompilerShaderPreprocessRequest preprocessing = {
        .source = source,
        .source_directory = directory,
        .shader_name = shader_name,
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

/* The source inverse consumes the complete target and, in the explicit scalar
 * calibration, a fixed controlled API layout. Neither authored source nor its
 * preprocessing contract enters this function. No player authority is inferred.
 */
static bool reconstruct_hull(const DXBCContainerView *target,
                             const SerializedProgramParameters *parameters,
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
    if (parameters) {
        printf("scalar_target phases=%zu cbuffers=%d icb_words=%d instructions=%d\n",
               program.tessellation.phase_count, program.cbuffer_count,
               program.icb_value_count, program.instruction_count);
        for (size_t index = 0; index < program.tessellation.phase_count; ++index) {
            const USILHullPhase *phase = &program.tessellation.phases[index];
            printf("scalar_phase=%zu kind=%u instances=%u first=%d end=%d\n",
                   index, (unsigned)phase->kind, phase->instance_count,
                   phase->first_instruction_index, phase->end_instruction_index);
        }
        for (int index = 0; index < program.instruction_count; ++index) {
            const USILInstruction *instruction = &program.instructions[index];
            printf("scalar_instruction=%d opcode=%u operands=%d destination_mask=%u\n",
                   index, (unsigned)instruction->opcode, instruction->operand_count,
                   instruction->operand_count ? usil_operand_destination_lane_mask(
                       &instruction->operands[0]) : 0);
        }
    }
    if (!hlsl_emit_with_options_diagnostic(&program, source, parameters, NULL, &names,
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
    printf("source scope=%s quality=%s map_valid=%d "
           "instructions=%d units=%zu "
           "input_mask=%u input_rw=%u output_mask=%u output_rw=%u\n",
           parameters ? "controlled-scalar-API-calibration" : "target-only-hull",
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
                              StringBuilder *wrapper, const char *shader_name) {
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
        shader_name, hull->buf);
    return sb_ok(wrapper);
}

static bool changed_factor_wrapper(const StringBuilder *hull,
                                   StringBuilder *wrapper, const char *shader_name,
                                   bool scalar_fixture) {
    const char *original = scalar_fixture ? "factors.outer[factorIndex] = min((_Factor), 32.0f);"
                                         : "factors.outer[factorIndex] = 3.0f;";
    const char *changed = scalar_fixture ? "factors.outer[factorIndex] = min((_Factor * 2.0f), 32.0f);"
                                        : "factors.outer[factorIndex] = 5.0f;";
    const size_t original_length = strlen(original);
    /* Match exactly one owned factor assignment in either controlled fixture.
     * A failed cold comparison remains a failure after this replay check. */
    if (!sb_ok(hull) || !hull->buf || hull->len > PROBE_SOURCE_LIMIT)
        return false;
    const char *match = strstr(hull->buf, original);
    if (!match || strstr(match + original_length, original))
        return false;
    StringBuilder mutated;
    sb_init(&mutated);
    sb_append_len(&mutated, hull->buf, (size_t)(match - hull->buf));
    sb_append(&mutated, changed);
    sb_append(&mutated, match + original_length);
    const bool built = sb_ok(&mutated) && candidate_wrapper(&mutated, wrapper, shader_name);
    if (built) {
        printf("warm_mutation matched_owned_inverse_occurrences=1 scalar_fixture=%d\n",
               scalar_fixture);
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
        "usage: %s SOURCE.shader PROJECT_ROOT INCLUDES_DIR [--scalar-fixture]\n"
        "Use '-' for no additional includes. Selected-native HULL comparison "
        "only; no source or binary files exported.\n"
        "--scalar-fixture supplies a controlled FactorInputs/_Factor API layout,\n"
        "validated against native reflection; no player metadata authority.\n",
        name);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }
    if (argc != 4 && (argc != 5 || strcmp(argv[4], "--scalar-fixture"))) {
        usage(argv[0]);
        return 2;
    }
    CommonFileBytes authored = {0};
    const bool scalar_fixture = argc == 5;
    const char *shader_name = scalar_fixture ? "Fixture/HighLevel/HullFloat3ScalarCBuffer"
                                            : default_shader_name;
    SerializedVariable field = {.name = "_Factor", .layout = {0, 0, 0, 1, 0, 0}};
    SerializedConstantBuffer buffer = {.name = "FactorInputs", .size = 16,
        .role = SERIALIZED_CBUFFER_NAMED, .variables = &field, .var_count = 1};
    SerializedResourceParam binding = {.name = "FactorInputs",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .array_size = 1};
    SerializedProgramParameters parameters = {.constant_buffers = &buffer,
        .cb_count = 1, .resources = &binding, .res_count = 1};
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
    printf("metadata_authority=%s player_metadata=not-supplied\n",
           scalar_fixture ? "controlled-API-fixture" : "none-required");
    if (!compile_source(&channel, "authored-target", (char *)authored.data,
                        directory, shader_name, valid_apis, &preprocessing[0], &requests[0],
                        &compiled[0], &provenance[0]))
        goto done;
    DXBCContainerView target = {0}, candidate = {0};
    if (scalar_fixture && !scalar_fixture_reflection(&compiled[0])) {
        fputs("Controlled scalar fixture reflection mismatch.\n", stderr);
        goto done;
    }
    if (scalar_fixture) printf("controlled_API_layout_native_reflection_checked=1\n");
    if (!dxbc_container_view_first(compiled[0].data, compiled[0].size,
                                   &target) ||
        !reconstruct_hull(&target, scalar_fixture ? &parameters : NULL, &hull) ||
        !candidate_wrapper(&hull, &wrapper, shader_name))
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
                        directory, shader_name, valid_apis, &preprocessing[1], &requests[1],
                        &compiled[1], &provenance[1]) ||
        !dxbc_container_view_first(compiled[1].data, compiled[1].size,
                                   &candidate))
        goto done;
    if (scalar_fixture && !scalar_fixture_reflection(&compiled[1])) goto done;
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
    printf("comparison chunk=%u instruction=%u token=%u expected=0x%" PRIx64
           " actual=0x%" PRIx64 "\n", comparison.chunk_index,
           comparison.instruction_index, comparison.token_index,
           comparison.expected_value, comparison.actual_value);
    const bool cold_equal = status == DXBC_COMPARE_EQUAL;
    if (!controls_equal || !toolchain_equal || (!cold_equal && !scalar_fixture))
        goto done;

    /* A changed source must affect a warm compiler, and returning to the
     * original source must restore both exact bytes and canonical identity. */
    const pid_t candidate_process = channel.process_id;
    DXBCContainerView changed = {0}, repeated = {0};
    if (!candidate_process ||
        !changed_factor_wrapper(&hull, &changed_wrapper, shader_name, scalar_fixture) ||
        !compile_source(&channel, "warm-mutated-hull", changed_wrapper.buf,
                        directory, shader_name, valid_apis, &preprocessing[2], &requests[2],
                        &compiled[2], &provenance[2]) ||
        !dxbc_container_view_first(compiled[2].data, compiled[2].size,
                                   &changed))
        goto done;
    if (scalar_fixture && !scalar_fixture_reflection(&compiled[2])) goto done;
    const DXBCCompareStatus mutation_status = dxbc_compare_exact(
        candidate.data, candidate.size, changed.data, changed.size, &comparison);
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
                        shader_name, valid_apis, &preprocessing[3], &requests[3],
                        &compiled[3], &provenance[3]) ||
        !dxbc_container_view_first(compiled[3].data, compiled[3].size,
                                   &repeated))
        goto done;
    if (scalar_fixture && !scalar_fixture_reflection(&compiled[3])) goto done;
    const DXBCCompareStatus repeated_status = dxbc_compare_exact(
        candidate.data, candidate.size, repeated.data, repeated.size, &comparison);
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
    printf("warm_original_equal=%d comparison=%s reference=cold-candidate canonical_identity_equal=%d "
           "authority_equal=%d same_process=%d\n",
           repeated_status == DXBC_COMPARE_EQUAL,
           dxbc_compare_status_name(repeated_status), repeated_identity,
           repeated_authority, candidate_process == channel.process_id);
    result = cold_equal && repeated_status == DXBC_COMPARE_EQUAL && repeated_identity &&
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
