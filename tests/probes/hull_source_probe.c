// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/sha256.h"
#include "compiler/unity_compiler_client.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/hlsl_stage_coverage_internal.h"
#include "translation/usil_validation.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROBE_SOURCE_LIMIT = 1024 * 1024, PROBE_DIRECTORY_LIMIT = 4096,
       PROBE_COMPILE_SLOTS = 7 };

typedef struct {
    size_t begin, end;
    uint32_t original_bits;
    bool valid;
} OwnedLiteralMutation;

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
                             StringBuilder *source, bool icb_fixture,
                             OwnedLiteralMutation *icb_mutation) {
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
    if (icb_fixture && (!icb_mutation || !program.icb_value_count ||
                        !usil_icb_declaration_is_valid(&program))) {
        fputs("Requested native ICB grammar was not produced.\n", stderr);
        goto done;
    }
    HLSLExpressionSourceMap map = {0};
    HLSLSourceQualityResult quality = {0};
    HLSLEmitDiagnostic diagnostic;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    const HLSLEmitNames names = {.entry_point = "hull"};
    if (parameters || icb_fixture) {
        const char *kind = icb_fixture ? "icb" : "scalar";
        printf("%s_target phases=%zu cbuffers=%d icb_words=%d instructions=%d\n", kind,
               program.tessellation.phase_count, program.cbuffer_count,
               program.icb_value_count, program.instruction_count);
        for (size_t index = 0; index < program.tessellation.phase_count; ++index) {
            const USILHullPhase *phase = &program.tessellation.phases[index];
            printf("%s_phase=%zu kind=%u instances=%u first=%d end=%d\n",
                   kind, index, (unsigned)phase->kind, phase->instance_count,
                   phase->first_instruction_index, phase->end_instruction_index);
        }
        for (int index = 0; index < program.instruction_count; ++index) {
            const USILInstruction *instruction = &program.instructions[index];
            printf("%s_instruction=%d opcode=%u operands=%d destination_mask=%u\n",
                   kind, index, (unsigned)instruction->opcode, instruction->operand_count,
                   instruction->operand_count ? usil_operand_destination_lane_mask(
                       &instruction->operands[0]) : 0);
        }
        if (icb_fixture) {
            for (int word = 0; word < program.icb_value_count && word < HLSL_HULL_ICB_WORD_LIMIT; ++word)
                printf("icb_word=%d bits=0x%08" PRIx32 "\n", word, program.icb_values[word]);
            for (int index = 0; index < program.instruction_count && index < HLSL_HULL_ICB_CONSUMER_LIMIT; ++index) {
                const USILInstruction *instruction = &program.instructions[index];
                for (int operand = 1; operand < instruction->operand_count; ++operand) {
                    const DXBCOperand *source_operand = &instruction->operands[operand];
                    if (source_operand->type != OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER) continue;
                    printf("icb_consumer instruction=%d operand=%d dimensions=%d base=%d "
                           "swizzle_mode=%u column=%u relative_type=%u\n", index, operand,
                           source_operand->register_index_dim, source_operand->register_index,
                           source_operand->swizzle_mode, source_operand->swizzle[0],
                           source_operand->rel_op0 ? (unsigned)source_operand->rel_op0->type : UINT32_MAX);
                }
            }
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
    size_t compiler_clamps = 0;
    for (size_t index = 0; index < map.count; ++index)
        if (map.origins[index].kind == HLSL_EXPRESSION_ORIGIN_HULL_FACTOR_CLAMP)
            ++compiler_clamps;
    const bool quality_observed = icb_fixture
        ? quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
          quality.counts.incomplete_units == 1
        : quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          !quality.counts.incomplete_units;
    accepted = quality_observed && !quality.counts.unknown_provenance && map_valid;
    printf("source scope=%s quality=%s map_valid=%d "
           "instructions=%d units=%zu compiler_clamps=%zu "
           "input_mask=%u input_rw=%u output_mask=%u output_rw=%u\n",
           icb_fixture ? "target-only-icb-hull-configuration-gap"
                       : parameters ? "controlled-scalar-API-calibration" : "target-only-hull",
           hlsl_source_quality_class_name(quality.classification), map_valid,
           program.instruction_count, quality.counts.inspected_units, compiler_clamps,
           program.inputs[0].mask, program.inputs[0].rw_mask,
           program.outputs[0].mask, program.outputs[0].rw_mask);
    StringBuilder owned_source;
    sb_init(&owned_source);
    HLSLStageCoverage coverage = {0};
    HLSLExpressionSourceMap owned_map = {0};
    HLSLSourceQualityResult owned_quality = {0};
    HLSLEmitOptions owned_options = options;
    owned_options.expression_source_map = &owned_map;
    owned_options.source_quality = &owned_quality;
    const bool captured = hlsl_emit_with_stage_coverage(&program, &owned_source,
        parameters, NULL, &names, &owned_options, &coverage, &diagnostic);
    const bool coverage_valid = captured &&
        hlsl_stage_coverage_validate(&coverage, &owned_source);
    bool unchanged = captured && owned_source.len == source->len &&
        !memcmp(owned_source.buf, source->buf, source->len) &&
        hlsl_source_quality_results_equal(&owned_quality, &quality) &&
        owned_map.complete == map.complete && owned_map.count == map.count &&
        hlsl_expression_source_map_matches(&owned_map, &program, owned_source.buf);
    for (size_t index = 0; unchanged && index < map.count; ++index)
        unchanged = hlsl_expression_origins_equal(&map.origins[index], &owned_map.origins[index]);
    printf("private_stage_capture=%d units=%zu roots=%zu syntax=%zu "
           "obligations=0x%x source_and_classification_unchanged=%d "
           "original_target_receipt=not-supplied\n",
           coverage_valid, coverage.unit_count, coverage.root_count,
           coverage.syntax_count, coverage.obligations, unchanged);
    accepted = accepted && coverage_valid && unchanged && coverage.unit_count == 3 &&
        (coverage.obligations & HLSL_STAGE_COVERAGE_BODY);
    if (icb_fixture) {
        const HLSLHullICBPlan *plan = &coverage.hull_icb.plan;
        const size_t root_index = coverage.hull_icb.literal_root_indices[0];
        accepted = accepted && plan->present && plan->row_count == 3 &&
            plan->consumer_count && root_index < coverage.root_count;
        if (accepted) {
            const HLSLStageOwnedRoot *literal = &coverage.roots[root_index];
            accepted = literal->begin < literal->end && literal->end <= source->len;
            if (accepted) {
                *icb_mutation = (OwnedLiteralMutation){literal->begin, literal->end,
                    plan->payload[plan->physical_column], true};
                printf("icb_target words=%zu rows=%zu column=%u consumers=%zu transports=%zu "
                       "declaration=%u token=0x%x block_words=%u configuration_gap=retained\n",
                       (size_t)plan->payload_count, (size_t)plan->row_count,
                       (unsigned)plan->physical_column,
                       (size_t)plan->consumer_count, (size_t)plan->transport_count,
                       plan->declaration_source_instruction_index, plan->declaration_token,
                       plan->declaration_word_count);
            }
        }
    }
    hlsl_stage_coverage_dispose(&coverage);
    sb_free(&owned_source);
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
                                   bool scalar_fixture, const OwnedLiteralMutation *icb_mutation) {
    /* Match exactly one owned factor assignment in either controlled fixture.
     * A failed cold comparison remains a failure after this replay check. */
    if (!sb_ok(hull) || !hull->buf || hull->len > PROBE_SOURCE_LIMIT)
        return false;
    if (icb_mutation) {
        if (!icb_mutation->valid || icb_mutation->begin >= icb_mutation->end ||
            icb_mutation->end > hull->len) return false;
        const uint32_t changed_bits = icb_mutation->original_bits == UINT32_C(0x40200000)
            ? UINT32_C(0x40700000) : UINT32_C(0x40200000);
        ASTExpr *literal = ast_create_literal_bits(&changed_bits, 1, AST_SCALAR_FLOAT32);
        if (!literal) return false;
        StringBuilder mutated;
        sb_init(&mutated);
        sb_append_len(&mutated, hull->buf, icb_mutation->begin);
        ast_format_expr(literal, &mutated);
        sb_append_len(&mutated, hull->buf + icb_mutation->end, hull->len - icb_mutation->end);
        ast_free_expr(literal);
        const bool built = sb_ok(&mutated) && candidate_wrapper(&mutated, wrapper, shader_name);
        if (built) {
            puts("warm_mutation owned_icb_literal_span=1 classification=not-promoted");
            print_hash("mutated_hull_sha256", mutated.buf, mutated.len);
        }
        sb_free(&mutated);
        return built;
    }
    const char *original = "factors.outer[factorIndex] = 3.0f;";
    const char *changed = "factors.outer[factorIndex] = 5.0f;";
    if (scalar_fixture) {
        original = "factors.outer[factorIndex] = (_Factor);";
        changed = "factors.outer[factorIndex] = (_Factor * 2.0f);";
        if (!strstr(hull->buf, original)) {
            original = "factors.outer[factorIndex] = min((_Factor), 32.0f);";
            changed = "factors.outer[factorIndex] = min((_Factor * 2.0f), 32.0f);";
        }
    }
    const size_t original_length = strlen(original);
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

static bool static_factor_calibration(const StringBuilder *hull,
                                      StringBuilder *wrapper,
                                      const char *shader_name) {
    /* A controlled compiler experiment, not an inverse-source lift. Keep the
     * normal candidate's comparison and provenance unchanged. */
    const char *loop =
        "    for (uint factorIndex = 0; factorIndex < 3; ++factorIndex) {\n"
        "        factors.outer[factorIndex] = (_Factor);\n"
        "    }\n";
    const char *assignments =
        "    factors.outer[0] = (_Factor);\n"
        "    factors.outer[1] = (_Factor);\n"
        "    factors.outer[2] = (_Factor);\n";
    if (!sb_ok(hull) || !hull->buf || hull->len > PROBE_SOURCE_LIMIT)
        return false;
    const char *match = strstr(hull->buf, loop);
    if (!match || strstr(match + strlen(loop), loop)) return false;
    StringBuilder calibration;
    sb_init(&calibration);
    sb_append_len(&calibration, hull->buf, (size_t)(match - hull->buf));
    sb_append(&calibration, assignments);
    sb_append(&calibration, match + strlen(loop));
    const bool built = sb_ok(&calibration) &&
        candidate_wrapper(&calibration, wrapper, shader_name);
    if (built) {
        puts("source_shape_calibration=three-authored-static-assignments "
             "inverse_source_map=not-retained quality=not-classified");
        print_hash("calibration_hull_sha256", calibration.buf, calibration.len);
    }
    sb_free(&calibration);
    return built;
}

static bool explicit_packoffset_calibration(const StringBuilder *hull,
                                          StringBuilder *calibration) {
    /* This one controlled declaration counterexample is independent of the
     * static-factor experiment and never changes the ordinary inverse. */
    const char *declaration = "float _Factor;";
    if (!sb_ok(hull) || !hull->buf || hull->len > PROBE_SOURCE_LIMIT ||
        !sb_ok(calibration) || calibration->len) return false;
    const char *match = strstr(hull->buf, declaration);
    if (!match || strstr(match + strlen(declaration), declaration)) return false;
    sb_append_len(calibration, hull->buf, (size_t)(match - hull->buf));
    sb_append(calibration, "float _Factor : packoffset(c0);");
    sb_append(calibration, match + strlen(declaration));
    if (!sb_ok(calibration)) return false;
    puts("source_shape_calibration=add-only-scalar-packoffset "
         "inverse_source_map=not-retained quality=not-classified");
    print_hash("calibration_hull_sha256", calibration->buf, calibration->len);
    return true;
}

static bool inner_factor_brace_calibration(const StringBuilder *hull,
                                          StringBuilder *calibration) {
    /* Start from the untouched inverse, retaining its cbuffer declaration,
     * outer-factor loop and every other scope and identifier. */
    const char *block =
        "    {\n"
        "        factors.inner = (_Factor);\n"
        "    }\n";
    if (!sb_ok(hull) || !hull->buf || hull->len > PROBE_SOURCE_LIMIT ||
        !sb_ok(calibration) || calibration->len) return false;
    const char *match = strstr(hull->buf, block);
    if (!match || strstr(match + strlen(block), block)) return false;
    sb_append_len(calibration, hull->buf, (size_t)(match - hull->buf));
    sb_append(calibration, "    factors.inner = (_Factor);\n");
    sb_append(calibration, match + strlen(block));
    if (!sb_ok(calibration)) return false;
    puts("source_shape_calibration=omit-only-singleton-inner-factor-brace "
         "inverse_source_map=not-retained quality=not-classified");
    print_hash("calibration_hull_sha256", calibration->buf, calibration->len);
    return true;
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
        "usage: %s SOURCE.shader PROJECT_ROOT INCLUDES_DIR [--icb-fixture | "
        "--scalar-fixture [--static-factor-calibration | --explicit-packoffset-calibration | "
        "--inner-factor-brace-calibration]]\n"
        "Use '-' for no additional includes. Selected-native HULL comparison "
        "only; no source or binary files exported.\n"
        "--scalar-fixture supplies a controlled FactorInputs/_Factor API layout,\n"
        "validated against native reflection; no player metadata authority.\n"
        "--icb-fixture requires an actual parsed scalar table; its configuration "
        "gap remains MIXED.\n"
        "Static-factor calibration is a separate cold compiler experiment; it "
        "does not repair the normal inverse-source comparison.\n"
        "Explicit-packoffset and inner-factor-brace calibrations each change only one "
        "fragment of the original inverse in an independent cold process; warm "
        "mutation/restoration runs only after their exact cold match.\n",
        name);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }
    const bool scalar_fixture = argc >= 5 && !strcmp(argv[4], "--scalar-fixture");
    const bool icb_fixture = argc == 5 && !strcmp(argv[4], "--icb-fixture");
    const bool calibrate_static_factors = argc == 6 && scalar_fixture &&
        !strcmp(argv[5], "--static-factor-calibration");
    const bool calibrate_explicit_packoffset = argc == 6 && scalar_fixture &&
        !strcmp(argv[5], "--explicit-packoffset-calibration");
    const bool calibrate_inner_factor_brace = argc == 6 && scalar_fixture &&
        !strcmp(argv[5], "--inner-factor-brace-calibration");
    const bool calibrate_source_shape = calibrate_static_factors ||
        calibrate_explicit_packoffset || calibrate_inner_factor_brace;
    if (argc != 4 && !icb_fixture &&
        !(scalar_fixture && (argc == 5 || calibrate_source_shape))) {
        usage(argv[0]);
        return 2;
    }
    CommonFileBytes authored = {0};
    const char *shader_name = scalar_fixture ? "Fixture/HighLevel/HullFloat3ScalarCBuffer"
        : icb_fixture ? "Fixture/HighLevel/HullFloat3ICB" : default_shader_name;
    OwnedLiteralMutation icb_mutation = {0};
    SerializedVariable field = {.name = "_Factor", .layout = {0, 0, 0, 1, 0, 0}};
    SerializedConstantBuffer buffer = {.name = "FactorInputs", .size = 16,
        .role = SERIALIZED_CBUFFER_NAMED, .variables = &field, .var_count = 1};
    SerializedResourceParam binding = {.name = "FactorInputs",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .array_size = 1};
    SerializedProgramParameters parameters = {.constant_buffers = &buffer,
        .cb_count = 1, .resources = &binding, .res_count = 1};
    UnityCompilerChannel channel = {.socket_fd = -1};
    UnityCompilerPreprocessResponse preprocessing[PROBE_COMPILE_SLOTS];
    UnityCompilerBinaryResponse compiled[PROBE_COMPILE_SLOTS];
    UnityCompilerSnippetCompileRequest requests[PROBE_COMPILE_SLOTS] = {0};
    UnityCompilerToolchainProvenance provenance[PROBE_COMPILE_SLOTS];
    StringBuilder hull, wrapper, changed_wrapper, calibration_wrapper;
    sb_init(&hull);
    sb_init(&wrapper);
    sb_init(&changed_wrapper);
    sb_init(&calibration_wrapper);
    for (unsigned index = 0; index < PROBE_COMPILE_SLOTS; ++index) {
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
    if (!dxbc_container_view_first(compiled[0].data, compiled[0].size, &target))
        goto done;
    print_hash("target_complete_dxbc_sha256", target.data, target.size);
    if (!reconstruct_hull(&target, scalar_fixture ? &parameters : NULL, &hull,
                          icb_fixture, icb_fixture ? &icb_mutation : NULL) ||
        !candidate_wrapper(&hull, &wrapper, shader_name))
        goto done;
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
    if (!controls_equal || !toolchain_equal || (!cold_equal && !scalar_fixture && !icb_fixture))
        goto done;

    /* A changed source must affect a warm compiler, and returning to the
     * original source must restore both exact bytes and canonical identity. */
    const pid_t candidate_process = channel.process_id;
    DXBCContainerView changed = {0}, repeated = {0};
    if (!candidate_process ||
        !changed_factor_wrapper(&hull, &changed_wrapper, shader_name, scalar_fixture,
                                icb_fixture ? &icb_mutation : NULL) ||
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
    if (calibrate_source_shape) {
        /* The held target bytes survive a third independent cold process.
         * This optional result cannot change the normal candidate's failure. */
        const int normal_candidate_result = result;
        result = 1;
        DXBCContainerView calibration = {0};
        const char *calibration_name = calibrate_static_factors ? "static" :
            calibrate_explicit_packoffset ? "explicit_packoffset" : "inner_factor_brace";
        const char *calibration_role = calibrate_static_factors ? "cold-static-factor-calibration" :
            calibrate_explicit_packoffset ? "cold-explicit-packoffset-calibration" : "cold-inner-factor-brace-calibration";
        if (calibrate_static_factors) {
            if (!static_factor_calibration(&hull, &calibration_wrapper, shader_name)) goto done;
        } else {
            /* The normal mutation wrapper is no longer needed. Reuse its
             * builder for one transformed ORIGINAL hull, without a source bank. */
            sb_clear(&changed_wrapper);
            const bool transformed = calibrate_explicit_packoffset
                ? explicit_packoffset_calibration(&hull, &changed_wrapper)
                : inner_factor_brace_calibration(&hull, &changed_wrapper);
            if (!transformed || !candidate_wrapper(&changed_wrapper, &calibration_wrapper, shader_name))
                goto done;
        }
        unity_compiler_shutdown(&channel);
        channel = (UnityCompilerChannel){.socket_fd = -1};
        UnityCompilerSessionCapabilities calibration_capabilities;
        uint32_t calibration_valid_apis = 0;
        if (!unity_compiler_start_lazy(&channel, argv[2],
                !strcmp(argv[3], "-") ? NULL : argv[3]) ||
            !unity_compiler_capture_session_capabilities(&channel,
                &calibration_capabilities) ||
            !unity_compiler_session_capabilities_valid_apis(
                &calibration_capabilities, &calibration_valid_apis) ||
            calibration_valid_apis != valid_apis ||
            !unity_compiler_session_capabilities_equal(&capabilities,
                &calibration_capabilities) ||
            !unity_compiler_set_expected_valid_apis(&channel, valid_apis) ||
            !compile_source(&channel, calibration_role,
                calibration_wrapper.buf, directory, shader_name, valid_apis,
                &preprocessing[4], &requests[4], &compiled[4], &provenance[4]) ||
            !scalar_fixture_reflection(&compiled[4]) ||
            !dxbc_container_view_first(compiled[4].data, compiled[4].size,
                &calibration)) goto done;
        const DXBCCompareStatus calibration_status = dxbc_compare_exact(
            target.data, target.size, calibration.data, calibration.size,
            &comparison);
        print_hash("calibration_complete_dxbc_sha256", calibration.data,
                   calibration.size);
        const bool calibration_controls =
            selected_controls_equal(&requests[0], &requests[4]);
        const bool calibration_environment =
            compiler_environment_equal(&provenance[0], &provenance[4]);
        printf("%s_calibration_comparison=%s cold_process=3 "
               "selected_controls_equal=%d compiler_environment_equal=%d "
               "normal_candidate_result=%d\n",
               calibration_name, dxbc_compare_status_name(calibration_status),
               calibration_controls, calibration_environment,
               normal_candidate_result);
        printf("%s_calibration_difference chunk=%u instruction=%u token=%u "
               "expected=0x%" PRIx64 " actual=0x%" PRIx64 "\n",
               calibration_name, comparison.chunk_index, comparison.instruction_index,
               comparison.token_index, comparison.expected_value,
               comparison.actual_value);
        const bool calibration_exact = calibration_status == DXBC_COMPARE_EQUAL &&
            calibration_controls && calibration_environment;
        if (calibrate_static_factors) {
            /* Preserve the original static calibration's cold-only behavior. */
            result = !normal_candidate_result && calibration_exact ? 0 : 1;
        } else if (!calibration_exact) {
            printf("%s_calibration_warm=not-run cold_exact=0 normal_candidate_result=%d\n",
                   calibration_name, normal_candidate_result);
        } else {
            const pid_t calibration_process = channel.process_id;
            DXBCContainerView calibration_changed = {0}, calibration_repeated = {0};
            sb_clear(&wrapper);
            if (!calibration_process ||
                !changed_factor_wrapper(&changed_wrapper, &wrapper, shader_name, true, NULL) ||
                !compile_source(&channel, "warm-mutated-calibration", wrapper.buf,
                    directory, shader_name, valid_apis, &preprocessing[5], &requests[5],
                    &compiled[5], &provenance[5]) ||
                !scalar_fixture_reflection(&compiled[5]) ||
                !dxbc_container_view_first(compiled[5].data, compiled[5].size,
                    &calibration_changed)) goto done;
            const DXBCCompareStatus calibration_mutation_status = dxbc_compare_exact(
                calibration.data, calibration.size, calibration_changed.data,
                calibration_changed.size, &comparison);
            const bool calibration_mutation_diff =
                calibration_mutation_status != DXBC_COMPARE_EQUAL &&
                calibration_mutation_status != DXBC_COMPARE_INVALID_ARGUMENT &&
                calibration_mutation_status != DXBC_COMPARE_EXPECTED_INVALID &&
                calibration_mutation_status != DXBC_COMPARE_ACTUAL_INVALID;
            const bool calibration_mutation_authority =
                selected_controls_equal(&requests[4], &requests[5]) &&
                compiler_environment_equal(&provenance[4], &provenance[5]) &&
                provenance[4].source_authority_revision == provenance[5].source_authority_revision &&
                calibration_process == channel.process_id;
            print_hash("calibration_warm_mutation_complete_dxbc_sha256",
                       calibration_changed.data, calibration_changed.size);
            printf("%s_calibration_warm_mutation_diff=%d comparison=%s authority_equal=%d "
                   "same_process=%d\n", calibration_name, calibration_mutation_diff,
                   dxbc_compare_status_name(calibration_mutation_status),
                   calibration_mutation_authority, calibration_process == channel.process_id);
            if (!calibration_mutation_diff || !calibration_mutation_authority) goto done;
            if (!compile_source(&channel, "warm-original-calibration", calibration_wrapper.buf,
                    directory, shader_name, valid_apis, &preprocessing[6], &requests[6],
                    &compiled[6], &provenance[6]) ||
                !scalar_fixture_reflection(&compiled[6]) ||
                !dxbc_container_view_first(compiled[6].data, compiled[6].size,
                    &calibration_repeated)) goto done;
            const DXBCCompareStatus calibration_repeated_status = dxbc_compare_exact(
                target.data, target.size, calibration_repeated.data,
                calibration_repeated.size, &comparison);
            const bool calibration_repeated_identity =
                !memcmp(compiled[4].request_digest, compiled[6].request_digest, 32) &&
                !memcmp(compiled[4].controls_digest, compiled[6].controls_digest, 32);
            const bool calibration_repeated_authority =
                selected_controls_equal(&requests[4], &requests[6]) &&
                compiler_environment_equal(&provenance[4], &provenance[6]) &&
                provenance[4].source_authority_revision == provenance[6].source_authority_revision &&
                calibration_process == channel.process_id;
            print_hash("calibration_warm_original_complete_dxbc_sha256",
                       calibration_repeated.data, calibration_repeated.size);
            printf("%s_calibration_warm_original_equal=%d comparison=%s reference=original-target "
                   "canonical_identity_equal=%d authority_equal=%d same_process=%d\n",
                   calibration_name, calibration_repeated_status == DXBC_COMPARE_EQUAL,
                   dxbc_compare_status_name(calibration_repeated_status), calibration_repeated_identity,
                   calibration_repeated_authority, calibration_process == channel.process_id);
            const bool calibration_warm_qualified = calibration_repeated_status == DXBC_COMPARE_EQUAL &&
                calibration_repeated_identity && calibration_repeated_authority;
            printf("%s_calibration_qualified=%d normal_candidate_result=%d "
                   "inverse_source_map=not-retained quality=not-classified\n",
                   calibration_name, calibration_warm_qualified, normal_candidate_result);
            result = !normal_candidate_result && calibration_warm_qualified ? 0 : 1;
        }
    }
done:
    for (unsigned index = 0; index < PROBE_COMPILE_SLOTS; ++index) {
        unity_compiler_binary_response_free(&compiled[index]);
        unity_compiler_preprocess_response_free(&preprocessing[index]);
    }
    unity_compiler_shutdown(&channel);
    common_file_bytes_dispose(&authored);
    sb_free(&hull);
    sb_free(&wrapper);
    sb_free(&changed_wrapper);
    sb_free(&calibration_wrapper);
    return result;
}
