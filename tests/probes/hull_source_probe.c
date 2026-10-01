// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/sha256.h"
#include "compiler/unity_compiler_client.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_source_quality.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/hlsl_stage_coverage_internal.h"
#include "translation/usil_validation.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROBE_SOURCE_LIMIT = 1024 * 1024, PROBE_DIRECTORY_LIMIT = 4096,
       PROBE_COMPILE_SLOTS = 7, PROBE_DOMAIN_INSTRUCTION_LIMIT = 64,
       PROBE_DOMAIN_DECLARATION_LIMIT = 32, PROBE_DOMAIN_SIGNATURE_LIMIT = 16,
       PROBE_DOMAIN_OPERAND_LIMIT = 128, PROBE_DOMAIN_OPERAND_DEPTH_LIMIT = 4,
       PROBE_LINKED_STAGE_COUNT = 2, PROBE_LINKED_COMPILE_SLOTS = 12,
       PROBE_LINKED_FACTOR_LIMIT = 6, PROBE_LINKED_CONTROL_POINT_LIMIT = 32 };

typedef struct {
    size_t begin, end;
    uint32_t original_bits;
    bool valid;
} OwnedLiteralMutation;

typedef struct {
    int instruction, operand;
    uint8_t component_mask;
    uint32_t raw_instruction, original_bits;
    USILOpcode opcode;
    bool valid;
} ProbeDecodedLiteral;

/* A small by-value link summary of actual admitted targets. No source spelling
 * or compiler reflection is used to manufacture an interface. This fixture
 * route requires inline names and retains role-specific RW masks. */
typedef struct {
    DXBCProgramType stage;
    DXBCTessellatorDomain domain;
    uint32_t input_control_points, output_control_points;
    uint8_t coordinate_count, outer_count, inner_count;
    bool inner_first;
    DXBCSignatureElement input, output, patch_constants[PROBE_LINKED_FACTOR_LIMIT];
    bool valid;
} ProbeLinkedInterface;

static bool linked_inline_signature(const DXBCSignatureElement *signature) {
    return signature && !signature->semantic_name_extended &&
        memchr(signature->semantic_name, 0, sizeof(signature->semantic_name));
}

static bool linked_interface_freeze(const USILProgram *program,
                                    ProbeLinkedInterface *interface) {
    HLSLDomainShape shape;
    bool inner_first;
    if (!program || !interface || interface->valid ||
        (program->program_type != DXBC_PROGRAM_TYPE_HULL &&
         program->program_type != DXBC_PROGRAM_TYPE_DOMAIN) ||
        !program->has_stage_contract || !program->has_parsed_signature_authority ||
        !program->tessellation.valid ||
        !hlsl_domain_shape(program->tessellation.domain, &shape) ||
        (unsigned)shape.outer_count + (unsigned)shape.inner_count > (unsigned)PROBE_LINKED_FACTOR_LIMIT ||
        !program->tessellation.input_control_point_count ||
        program->tessellation.input_control_point_count > (unsigned)PROBE_LINKED_CONTROL_POINT_LIMIT ||
        program->tessellation.output_control_point_count !=
            (program->program_type == DXBC_PROGRAM_TYPE_HULL
                ? program->tessellation.input_control_point_count : 0u) ||
        program->input_count != 1 || program->output_count != 1 ||
        program->input_alloc < 1 || program->output_alloc < 1 ||
        program->patch_constant_count != (int)shape.outer_count + (int)shape.inner_count ||
        program->patch_constant_alloc < program->patch_constant_count ||
        !program->inputs || !program->outputs || !program->patch_constants ||
        !usil_signature_authority_is_valid(program) ||
        !hlsl_domain_factor_order(program, &inner_first) ||
        !linked_inline_signature(program->inputs) ||
        !linked_inline_signature(program->outputs)) return false;
    ProbeLinkedInterface captured = {
        .stage = program->program_type, .domain = program->tessellation.domain,
        .input_control_points = program->tessellation.input_control_point_count,
        .output_control_points = program->tessellation.output_control_point_count,
        .coordinate_count = shape.coordinate_count,
        .outer_count = shape.outer_count, .inner_count = shape.inner_count,
        .inner_first = inner_first,
        .input = program->inputs[0], .output = program->outputs[0]};
    const unsigned factor_count = (unsigned)shape.outer_count + (unsigned)shape.inner_count;
    unsigned seen = 0;
    for (unsigned index = 0; index < factor_count; ++index) {
        const DXBCSignatureElement *signature = &program->patch_constants[index];
        if (!linked_inline_signature(signature) || signature->register_id >= factor_count ||
            (seen & (1u << signature->register_id))) return false;
        seen |= 1u << signature->register_id;
        captured.patch_constants[signature->register_id] = *signature;
    }
    if (seen != (1u << factor_count) - 1u) return false;
    captured.valid = true;
    *interface = captured;
    return true;
}

static bool linked_signature_equal(const DXBCSignatureElement *left,
                                   const DXBCSignatureElement *right,
                                   uint8_t left_rw, uint8_t right_rw) {
    return linked_inline_signature(left) && linked_inline_signature(right) &&
        !strcmp(left->semantic_name, right->semantic_name) &&
        left->semantic_index == right->semantic_index &&
        left->system_value == right->system_value &&
        left->component_type == right->component_type &&
        left->register_id == right->register_id && left->mask == right->mask &&
        left->rw_mask == left_rw && right->rw_mask == right_rw &&
        left->stream_index == right->stream_index &&
        left->min_precision == right->min_precision &&
        left->interpolation_mode == right->interpolation_mode;
}

static bool linked_interfaces_match(const ProbeLinkedInterface *hull,
                                    const ProbeLinkedInterface *domain) {
    HLSLDomainShape shape;
    if (!hull || !domain || !hull->valid || !domain->valid ||
        hull->stage != DXBC_PROGRAM_TYPE_HULL || domain->stage != DXBC_PROGRAM_TYPE_DOMAIN ||
        !hlsl_domain_shape(hull->domain, &shape) || hull->domain != domain->domain ||
        hull->coordinate_count != shape.coordinate_count || domain->coordinate_count != shape.coordinate_count ||
        hull->outer_count != shape.outer_count || domain->outer_count != shape.outer_count ||
        hull->inner_count != shape.inner_count || domain->inner_count != shape.inner_count ||
        hull->inner_first != domain->inner_first || (!shape.inner_count && hull->inner_first) ||
        !hull->input_control_points || hull->input_control_points > (unsigned)PROBE_LINKED_CONTROL_POINT_LIMIT ||
        hull->output_control_points != hull->input_control_points ||
        domain->input_control_points != hull->output_control_points || domain->output_control_points ||
        hull->input.mask != 7 || hull->input.component_type != 3 || hull->input.system_value ||
        hull->input.register_id || hull->input.semantic_index ||
        strcmp(hull->input.semantic_name, "POINTVALUE") ||
        !linked_signature_equal(&hull->input, &hull->output, 7, 8) ||
        !linked_signature_equal(&hull->output, &domain->input, 8, 7) ||
        domain->output.mask != 15 || domain->output.component_type != 3 ||
        domain->output.system_value != 1 || domain->output.register_id ||
        domain->output.semantic_index || domain->output.rw_mask) return false;
    const unsigned factor_count = (unsigned)shape.outer_count + (unsigned)shape.inner_count;
    if (factor_count > (unsigned)PROBE_LINKED_FACTOR_LIMIT) return false;
    for (unsigned index = 0; index < factor_count; ++index) {
        const DXBCSignatureElement *factor = &hull->patch_constants[index];
        const bool inner = hull->inner_first ? index < shape.inner_count : index >= shape.outer_count;
        const unsigned first = inner ? (hull->inner_first ? 0u : (unsigned)shape.outer_count)
            : (hull->inner_first ? (unsigned)shape.inner_count : 0u);
        const unsigned semantic = index - first;
        const uint32_t system = inner ? shape.inner_system_values[semantic]
            : shape.outer_system_values[semantic];
        if (factor->component_type != 3 || factor->mask != 1 ||
            factor->system_value != system || factor->semantic_index != semantic ||
            !linked_signature_equal(factor, &domain->patch_constants[index], 14, 0)) return false;
    }
    return true;
}

/* Manual selected-native isolated stage comparison. Other stages are
 * authored compilation stubs. No Editor, import, linked or runtime certificate. */
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

static void print_status_selected(const char *role,
                                  const UnityCompilerResponseStatus *status,
                                  bool summary_only) {
    enum { SUMMARY_DIAGNOSTIC_LIMIT = 8, SUMMARY_MESSAGE_LIMIT = 384 };
    printf("%s success=%d diagnostics=%zu actionable=%zu\n", role,
           status->compiler_success, status->diagnostic_count,
           unity_compiler_response_status_actionable_diagnostic_count(status));
    if (summary_only && unity_compiler_response_status_is_clean_success(status))
        return;
    const size_t count = summary_only && status->diagnostic_count > (size_t)SUMMARY_DIAGNOSTIC_LIMIT
        ? (size_t)SUMMARY_DIAGNOSTIC_LIMIT : status->diagnostic_count;
    for (size_t index = 0; index < count; ++index) {
        const UnityCompilerDiagnostic *diagnostic = &status->diagnostics[index];
        if (summary_only)
            fprintf(stderr, "%s diagnostic=%d,%d,%d message=%.*s\n", role,
                diagnostic->fields[0], diagnostic->fields[1], diagnostic->fields[2],
                SUMMARY_MESSAGE_LIMIT, diagnostic->message ? diagnostic->message : "");
        else fprintf(stderr, "%s diagnostic=%d,%d,%d message=%s\n", role,
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

static bool compile_selected(UnityCompilerChannel *channel, const char *role,
                           const char *source, const char *directory, const char *shader_name,
                           uint32_t valid_apis, UnityCompilerProgramStage stage,
                           bool summary_only,
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
    print_status_selected("preprocess", &preprocess->status, summary_only);
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
        .shader_type = stage,
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
    print_status_selected("compile", &compiled->status, summary_only);
    return unity_compiler_response_status_is_clean_success(&compiled->status) &&
           verify_compile_identity(channel, request, compiled, provenance);
}

static bool compile_source(UnityCompilerChannel *channel, const char *role,
                           const char *source, const char *directory, const char *shader_name,
                           uint32_t valid_apis,
                           UnityCompilerPreprocessResponse *preprocess,
                           UnityCompilerSnippetCompileRequest *request,
                           UnityCompilerBinaryResponse *compiled,
                           UnityCompilerToolchainProvenance *provenance) {
    return compile_selected(channel, role, source, directory, shader_name,
                            valid_apis, UNITY_COMPILER_PROGRAM_HULL, false,
                            preprocess, request, compiled, provenance);
}

static bool print_domain_signature(const char *role,
                                   const DXBCSignatureElement *elements,
                                   int count) {
    const int retained = count < PROBE_DOMAIN_SIGNATURE_LIMIT
        ? count : PROBE_DOMAIN_SIGNATURE_LIMIT;
    for (int index = 0; index < retained; ++index) {
        const DXBCSignatureElement *element = &elements[index];
        const char *name = dxbc_signature_semantic_name(element);
        printf("domain_signature role=%s index=%d semantic=%.64s semantic_index=%u "
               "system=%u component_type=%u register=%u mask=%u rw_mask=%u "
               "stream=%u precision=%u name_bytes=%zu\n", role, index, name,
               element->semantic_index, element->system_value, element->component_type,
               element->register_id, (unsigned)element->mask,
               (unsigned)element->rw_mask, element->stream_index,
               element->min_precision, element->semantic_name_length);
    }
    return retained == count;
}

static bool print_domain_operand(const DXBCOperand *operand, int instruction,
                                 int operand_index, unsigned path, unsigned depth,
                                 size_t *remaining) {
    if (!*remaining || depth > PROBE_DOMAIN_OPERAND_DEPTH_LIMIT)
        return false;
    --*remaining;
    printf("domain_operand instruction=%d operand=%d path=%u depth=%u type=%u "
           "register=%d dimensions=%d mode=%u mask=%u components=%u,%u,%u,%u "
           "neg=%d abs=%d precision=%u extended_tokens=%zu\n",
           instruction, operand_index, path, depth, (unsigned)operand->type,
           operand->register_index, operand->register_index_dim,
           (unsigned)operand->swizzle_mode,
           (unsigned)usil_operand_destination_lane_mask(operand),
           (unsigned)operand->swizzle[0], (unsigned)operand->swizzle[1],
           (unsigned)operand->swizzle[2], (unsigned)operand->swizzle[3],
           operand->has_neg, operand->has_abs, (unsigned)operand->min_precision,
           operand->extended_token_count);
    for (int dimension = 0; dimension < operand->register_index_dim && dimension < 3;
         ++dimension)
        printf("domain_operand_index instruction=%d operand=%d path=%u dimension=%d "
               "representation=%u immediate=%d value=%" PRIu64 " exceeds_int=%d\n",
               instruction, operand_index, path, dimension,
               (unsigned)operand->index_representations[dimension],
               operand->index_has_immediate[dimension], operand->index_values[dimension],
               operand->index_value_exceeds_int[dimension]);
    for (int word = 0; word < operand->immediate_word_count && word < 8; ++word)
        printf("domain_operand_literal instruction=%d operand=%d path=%u word=%d "
               "bits=0x%08" PRIx32 "\n", instruction, operand_index, path, word,
               operand->immediate_words[word]);
    const DXBCOperand *relative[] = {operand->rel_op0, operand->rel_op1,
                                    operand->rel_op2};
    bool complete = true;
    for (unsigned dimension = 0; dimension < 3; ++dimension)
        if (relative[dimension] &&
            !print_domain_operand(relative[dimension], instruction, operand_index,
                                  path * 4 + dimension + 1, depth + 1, remaining))
            complete = false;
    return complete;
}

/* The warm counterexample changes the demanded components of one broadcast
 * literal owned by the decoded target. It replays the ordinary inverse; it never guesses a
 * literal span or supplies authored fragment text to reconstruction. */
static bool reconstruct_structured(const DXBCContainerView *target,
    StringBuilder *source, ProbeDecodedLiteral *literal_owner, bool change_literal, bool vertex_fixture) {
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    USILProgram program = {0};
    HLSLExpressionSourceMap map = {0};
    HLSLSourceQualityResult quality = {0};
    HLSLEmitDiagnostic diagnostic;
    bool accepted = false;
    if (!target || !source || !literal_owner || !sb_ok(source) || source->len ||
        !dxbc_document_parse(&document, target->data, target->size, NULL) ||
        !dxbc_document_decode_semantic(&document, &semantic) ||
        !dxbc_stage_contract_decode(&document, &semantic, &contract, NULL) ||
        !usil_translate_with_stage_contract(&program, &semantic, &contract) ||
        program.program_type != (vertex_fixture ? DXBC_PROGRAM_TYPE_VERTEX : DXBC_PROGRAM_TYPE_PIXEL) ||
        program.instruction_count > PROBE_DOMAIN_INSTRUCTION_LIMIT ||
        program.input_count > PROBE_DOMAIN_SIGNATURE_LIMIT ||
        program.output_count != (vertex_fixture ? 2 : 1) || !program.outputs) {
        puts("structured_decode=failed structured_source_result=not-attempted");
        goto done;
    }
    bool output_shape = !vertex_fixture && program.outputs[0].mask == 7 &&
        program.outputs[0].component_type == 3;
    if (vertex_fixture) {
        unsigned position = 0, value = 0;
        for (int index = 0; index < program.output_count; ++index) {
            const DXBCSignatureElement *field = &program.outputs[index];
            if (field->component_type != 3) goto done;
            if (field->system_value == 1 && field->mask == 15) ++position;
            else if (!field->system_value && field->mask == 7) ++value;
        }
        output_shape = position == 1 && value == 1;
    }
    if (!output_shape) goto done;
    printf("structured_decode=complete stage=%u signature_authority=%d stage_contract=%d "
           "inputs=%d outputs=%d instructions=%d temps=%d\n",
           (unsigned)program.program_type, program.has_parsed_signature_authority, program.has_stage_contract,
           program.input_count, program.output_count, program.instruction_count, program.temp_count);
    for (int index = 0; index < program.input_count; ++index) {
        const DXBCSignatureElement *field = &program.inputs[index];
        printf("structured_input index=%d semantic=%.64s register=%u mask=%u rw_mask=%u "
               "type=%u interpolation=%u\n", index, dxbc_signature_semantic_name(field),
               field->register_id, (unsigned)field->mask, (unsigned)field->rw_mask,
               field->component_type, (unsigned)field->interpolation_mode);
    }
    ProbeDecodedLiteral captured = {0};
    for (int index = 0; index < program.instruction_count; ++index) {
        USILInstruction *instruction = &program.instructions[index];
        printf("structured_instruction index=%d raw_instruction=%u opcode=%u name=%s "
               "operands=%d destination_lanes=%u\n", index,
               instruction->source_instruction_index, (unsigned)instruction->opcode,
               hlsl_emit_opcode_name(instruction->opcode), instruction->operand_count,
               instruction->operand_count && (instruction->opcode == USIL_OP_MOV ||
               instruction->opcode == USIL_OP_ADD || instruction->opcode == USIL_OP_MUL ||
               instruction->opcode == USIL_OP_MIN || instruction->opcode == USIL_OP_MAX || instruction->opcode == USIL_OP_DIV ||
               instruction->opcode == USIL_OP_LT || instruction->opcode == USIL_OP_GE ||
               instruction->opcode == USIL_OP_EQ || instruction->opcode == USIL_OP_NE)
                   ? (unsigned)usil_operand_destination_lane_mask(&instruction->operands[0]) : 0u);
        if (instruction->opcode == USIL_OP_LT || instruction->opcode == USIL_OP_GE ||
            instruction->opcode == USIL_OP_EQ || instruction->opcode == USIL_OP_NE ||
            instruction->opcode == USIL_OP_IF) {
            size_t remaining = PROBE_DOMAIN_OPERAND_LIMIT;
            for (int operand_index = 0; operand_index < instruction->operand_count; ++operand_index)
                if (!print_domain_operand(&instruction->operands[operand_index], index,
                    operand_index, 0, 0, &remaining)) goto done;
        }
        const bool comparison = instruction->opcode == USIL_OP_LT || instruction->opcode == USIL_OP_GE ||
            instruction->opcode == USIL_OP_EQ || instruction->opcode == USIL_OP_NE;
        if (captured.valid || (!comparison && instruction->opcode != USIL_OP_MUL && instruction->opcode != USIL_OP_ADD &&
            instruction->opcode != USIL_OP_MIN && instruction->opcode != USIL_OP_MAX) ||
            instruction->operand_count != 3) continue;
        for (int operand_index = 1; operand_index < 3 && !captured.valid; ++operand_index) {
            DXBCOperand *operand = &instruction->operands[operand_index];
            USILOperandUseInfo use = {0};
            if (operand->type != OPERAND_TYPE_IMMEDIATE32 || operand->has_neg || operand->has_abs ||
                operand->min_precision || operand->extended_token_count ||
                !usil_instruction_operand_use(&program, instruction, operand_index, &use) ||
                use.use != USIL_OPERAND_USE_SOURCE || !use.source_lane_mask) continue;
            uint8_t component_mask = 0;
            bool broadcast = true;
            const uint32_t expected_bits = comparison ? UINT32_C(0x3ec00000)
                : instruction->opcode == USIL_OP_MUL || instruction->opcode == USIL_OP_MAX
                    ? UINT32_C(0x40000000) : UINT32_C(0x3f800000);
            for (int lane = 0; lane < 4; ++lane) {
                if (!(use.source_lane_mask & (1u << lane))) continue;
                const int selected = operand->imm_value_count == 1 ? 0 :
                    usil_operand_source_component(operand, lane);
                if (selected < 0 || selected >= operand->imm_value_count ||
                    selected >= operand->immediate_word_count ||
                    operand->imm_values[selected] != expected_bits ||
                    operand->immediate_words[selected] != expected_bits) {
                    broadcast = false;
                    break;
                }
                component_mask |= (uint8_t)(1u << selected);
            }
            if (!broadcast || !component_mask) continue;
            captured = (ProbeDecodedLiteral){index, operand_index, component_mask,
                instruction->source_instruction_index, expected_bits,
                instruction->opcode, true};
        }
    }
    if (!captured.valid) {
        puts("structured_literal_owner=unavailable");
        goto done;
    }
    if (change_literal) {
        if (!literal_owner->valid || captured.instruction != literal_owner->instruction ||
            captured.operand != literal_owner->operand || captured.component_mask != literal_owner->component_mask ||
            captured.raw_instruction != literal_owner->raw_instruction ||
            captured.opcode != literal_owner->opcode || captured.original_bits != literal_owner->original_bits)
            goto done;
        DXBCOperand *operand = &program.instructions[captured.instruction].operands[captured.operand];
        for (int component = 0; component < 4; ++component) {
            if (!(captured.component_mask & (1u << component))) continue;
            operand->imm_values[component] = UINT32_C(0x40400000);
            operand->immediate_words[component] = UINT32_C(0x40400000);
        }
    } else {
        *literal_owner = captured;
    }
    printf("structured_literal_owner=decoded-target instruction=%d raw_instruction=%u "
           "operand=%d component_mask=%u original_bits=0x%08" PRIx32 " changed=%d\n",
           captured.instruction, captured.raw_instruction, captured.operand,
           (unsigned)captured.component_mask, captured.original_bits, change_literal);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    const HLSLEmitNames names = {.entry_point = vertex_fixture ? "vert" : "frag",
        .input_struct = vertex_fixture ? "VertexInput" : "FragmentInput",
        .output_struct = vertex_fixture ? "VertexOutput" : "FragmentOutput"};
    const bool emitted = hlsl_emit_with_options_diagnostic(&program, source,
        NULL, NULL, &names, &options, &diagnostic);
    printf("structured_source_result=%s status=%s phase=%s reason=%s instruction=%d "
           "raw_instruction=%u bytes=%zu\n", emitted ? "generated" : "unavailable",
           hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase),
           hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index,
           diagnostic.source_instruction_index, source->len);
    if (emitted) {
        const bool map_valid = hlsl_expression_source_map_matches(&map, &program, source->buf);
        print_hash(vertex_fixture
                       ? (change_literal ? "mutated_vertex_sha256" : "reconstructed_vertex_sha256")
                       : (change_literal ? "mutated_fragment_sha256" : "reconstructed_fragment_sha256"),
                   source->buf, source->len);
        printf("structured_quality=%s reasons=0x%x units=%zu incomplete=%zu residual=%zu "
               "unknown_provenance=%zu map_complete=%d map_count=%zu map_valid=%d\n",
               hlsl_source_quality_class_name(quality.classification), quality.reasons,
               quality.counts.inspected_units, quality.counts.incomplete_units,
               quality.counts.residual_total, quality.counts.unknown_provenance,
               map.complete, map.count, map_valid);
        accepted = map_valid && map.complete &&
            quality.classification != HLSL_SOURCE_QUALITY_FAILED;
    }
done:
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    return accepted;
}

static bool structured_wrapper(const StringBuilder *fragment, StringBuilder *wrapper,
                               const char *shader_name, bool vertex_fixture) {
    if (!fragment || !sb_ok(fragment) || !fragment->buf || fragment->len > PROBE_SOURCE_LIMIT ||
        !wrapper || !sb_ok(wrapper) || wrapper->len) return false;
    sb_appendf(wrapper, "Shader \"%s\"\n{\n    SubShader\n    {\n        Pass\n        {\n"
        "            HLSLPROGRAM\n#pragma target 5.0\n#pragma only_renderers d3d11\n"
        "#pragma vertex vert\n#pragma fragment frag\n", shader_name);
    sb_append_len(wrapper, fragment->buf, fragment->len);
    sb_append(wrapper, vertex_fixture
        ? "float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); }\n"
        : "float4 vert(float4 position : POSITION) : SV_POSITION { return position; }\n");
    sb_append(wrapper, "ENDHLSL\n        }\n    }\n}\n");
    return sb_ok(wrapper) && wrapper->len <= PROBE_SOURCE_LIMIT;
}

/* Inspect the compiler's actual DOMAIN target, then attempt the normal inverse.
 * Authored fixture text and preprocessing controls are not inverse inputs. This
 * mode prints bounded facts only and supplies no substitute DOMAIN or metadata.
 */
static bool inspect_domain(const DXBCContainerView *target, StringBuilder *source,
                           OwnedLiteralMutation *mutation,
                           ProbeLinkedInterface *linked_interface) {
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    USILProgram program = {0};
    StringBuilder owned_source;
    sb_init(&owned_source);
    HLSLStageCoverage coverage = {0};
    HLSLExpressionSourceMap map = {0};
    HLSLSourceQualityResult quality = {0};
    HLSLEmitDiagnostic diagnostic;
    bool accepted = false;
    if (!dxbc_document_parse(&document, target->data, target->size, NULL) ||
        !dxbc_document_decode_semantic(&document, &semantic) ||
        !dxbc_stage_contract_decode(&document, &semantic, &contract, NULL) ||
        !usil_translate_with_stage_contract(&program, &semantic, &contract) ||
        program.program_type != DXBC_PROGRAM_TYPE_DOMAIN) {
        puts("domain_decode=failed domain_source_result=not-attempted");
        goto done;
    }
    printf("domain_decode=complete stage=%u model=%u.%u signature_authority=%d "
           "stage_contract=%d domain=%u input_control_points=%u "
           "inputs=%d outputs=%d patch_constants=%d declarations=%d "
           "instructions=%d temps=%d cbuffers=%d textures=%d samplers=%d "
           "uavs=%d icb_words=%d global_flags=0x%x\n",
           (unsigned)program.program_type, (unsigned)program.shader_model_major,
           (unsigned)program.shader_model_minor, program.has_parsed_signature_authority,
           program.has_stage_contract, (unsigned)program.tessellation.domain,
           program.tessellation.input_control_point_count, program.input_count,
           program.output_count, program.patch_constant_count,
           program.signature_declaration_count, program.instruction_count,
           program.temp_count, program.cbuffer_count, program.texture_count,
           program.sampler_count, program.uav_count, program.icb_value_count,
           program.global_flags);
    bool complete = print_domain_signature("input", program.inputs, program.input_count);
    if (!print_domain_signature("output", program.outputs, program.output_count))
        complete = false;
    if (!print_domain_signature("patch", program.patch_constants,
                                 program.patch_constant_count))
        complete = false;
    const int declaration_count = program.signature_declaration_count <
        PROBE_DOMAIN_DECLARATION_LIMIT ? program.signature_declaration_count
        : PROBE_DOMAIN_DECLARATION_LIMIT;
    for (int index = 0; index < declaration_count; ++index) {
        const USILSignatureDeclaration *declaration = &program.signature_declarations[index];
        printf("domain_declaration index=%d raw_instruction=%u kind=%u type=%u "
               "has_register=%d register=%u mask=%u stream=%u has_array=%d "
               "array_count=%u has_system=%d system=%u has_interpolation=%d "
               "interpolation=%u\n", index, declaration->source_instruction_index,
               (unsigned)declaration->kind, (unsigned)declaration->operand_type,
               declaration->has_signature_register, declaration->register_id,
               (unsigned)declaration->mask, (unsigned)declaration->stream_index,
               declaration->has_array_element_count,
               (unsigned)declaration->array_element_count, declaration->has_system_value,
               declaration->system_value_name, declaration->has_interpolation,
               (unsigned)declaration->interpolation_mode);
    }
    if (declaration_count != program.signature_declaration_count)
        complete = false;
    const int instruction_count = program.instruction_count < PROBE_DOMAIN_INSTRUCTION_LIMIT
        ? program.instruction_count : PROBE_DOMAIN_INSTRUCTION_LIMIT;
    size_t operands_remaining = PROBE_DOMAIN_OPERAND_LIMIT;
    for (int index = 0; index < instruction_count; ++index) {
        const USILInstruction *instruction = &program.instructions[index];
        printf("domain_instruction index=%d raw_instruction=%u opcode=%u name=%s "
               "operands=%d saturate=%d precise=%u shape_valid=%d\n", index,
               instruction->source_instruction_index, (unsigned)instruction->opcode,
               hlsl_emit_opcode_name(instruction->opcode), instruction->operand_count,
               instruction->saturate, (unsigned)instruction->precise_mask,
               usil_instruction_shape_valid(&program, instruction));
        for (int operand = 0; operand < instruction->operand_count && operand < DXBC_MAX_OPERANDS;
             ++operand) {
            USILOperandUseInfo use = {0};
            const bool valid = usil_instruction_operand_use(&program, instruction, operand, &use);
            printf("domain_operand_use instruction=%d operand=%d valid=%d use=%u "
                   "demanded_lanes=%u\n", index, operand, valid, (unsigned)use.use,
                   (unsigned)use.source_lane_mask);
            if (!print_domain_operand(&instruction->operands[operand], index, operand,
                                      0, 0, &operands_remaining))
                complete = false;
        }
    }
    if (instruction_count != program.instruction_count)
        complete = false;
    printf("domain_summary_complete=%d instruction_limit=%d declaration_limit=%d "
           "signature_limit=%d operand_limit=%d operand_depth_limit=%d\n", complete,
           PROBE_DOMAIN_INSTRUCTION_LIMIT, PROBE_DOMAIN_DECLARATION_LIMIT,
           PROBE_DOMAIN_SIGNATURE_LIMIT, PROBE_DOMAIN_OPERAND_LIMIT,
           PROBE_DOMAIN_OPERAND_DEPTH_LIMIT);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    const HLSLEmitNames names = {.entry_point = "domain", .input_struct = "DomainPoint"};
    const bool emitted = hlsl_emit_with_options_diagnostic(&program, source,
        NULL, NULL, &names, &options, &diagnostic);
    printf("domain_source_result=%s status=%s phase=%s reason=%s instruction=%d "
           "raw_instruction=%u opcode=%d operand=%d bytes=%zu\n",
           emitted ? "generated" : diagnostic.status == HLSL_EMIT_STATUS_UNSUPPORTED
               ? "unavailable" : "failed",
           hlsl_emit_status_name(diagnostic.status), hlsl_emit_phase_name(diagnostic.phase),
           hlsl_emit_reason_name(diagnostic.reason), diagnostic.instruction_index,
           diagnostic.source_instruction_index, diagnostic.opcode,
           diagnostic.operand_index, source->len);
    if (emitted) {
        const bool map_valid = hlsl_expression_source_map_matches(&map, &program, source->buf);
        print_hash("reconstructed_domain_sha256", source->buf, source->len);
        printf("domain_quality=%s reasons=0x%x units=%zu incomplete=%zu residual=%zu "
               "unknown_provenance=%zu map_complete=%d map_count=%zu map_valid=%d\n",
               hlsl_source_quality_class_name(quality.classification), quality.reasons,
               quality.counts.inspected_units, quality.counts.incomplete_units,
               quality.counts.residual_total, quality.counts.unknown_provenance,
               map.complete, map.count, map_valid);
        HLSLExpressionSourceMap owned_map = {0};
        HLSLSourceQualityResult owned_quality = {0};
        HLSLEmitOptions owned_options = options;
        owned_options.expression_source_map = &owned_map;
        owned_options.source_quality = &owned_quality;
        const bool captured = hlsl_emit_with_stage_coverage(&program, &owned_source,
            NULL, NULL, &names, &owned_options, &coverage, &diagnostic);
        const bool coverage_valid = captured &&
            hlsl_stage_coverage_validate(&coverage, &owned_source);
        bool unchanged = captured && owned_source.len == source->len &&
            !memcmp(owned_source.buf, source->buf, source->len) &&
            hlsl_source_quality_results_equal(&owned_quality, &quality) &&
            owned_map.complete == map.complete && owned_map.count == map.count &&
            hlsl_expression_source_map_matches(&owned_map, &program, owned_source.buf);
        for (size_t index = 0; unchanged && index < map.count; ++index)
            unchanged = hlsl_expression_origins_equal(&map.origins[index], &owned_map.origins[index]);
        printf("domain_private_stage_capture=%d quality=%s units=%zu roots=%zu syntax=%zu "
               "obligations=0x%x source_and_classification_unchanged=%d "
               "original_target_receipt=not-supplied\n", coverage_valid,
               captured ? hlsl_source_quality_class_name(owned_quality.classification) : "unavailable",
               coverage.unit_count, coverage.root_count, coverage.syntax_count,
               coverage.obligations, unchanged);
        /* Isolated authored stubs remain the established triangle shape. The
         * paired route links the actual decoded domain/counts and appends both
         * generated stages without replacing their source. */
        HLSLDomainShape linked_shape;
        const bool wrapper_shape = linked_interface
            ? hlsl_domain_shape(program.tessellation.domain, &linked_shape) &&
                program.tessellation.input_control_point_count > 0u &&
                program.tessellation.input_control_point_count <= (unsigned)PROBE_LINKED_CONTROL_POINT_LIMIT
            : program.tessellation.domain == DXBC_TESSELLATOR_DOMAIN_TRIANGLE &&
                program.tessellation.input_control_point_count == 3u;
        accepted = map_valid && coverage_valid && unchanged && mutation &&
            wrapper_shape &&
            program.input_count == 1 && program.inputs[0].mask == 7 &&
            program.inputs[0].component_type == 3 && !program.inputs[0].system_value &&
            !strcmp(dxbc_signature_semantic_name(&program.inputs[0]), "POINTVALUE") &&
            program.output_count == 1 && program.outputs[0].mask == 15 &&
            program.outputs[0].system_value == 1;
        size_t matches = 0;
        for (size_t index = 0; accepted && index < coverage.root_count; ++index) {
            const HLSLStageOwnedRoot *root = &coverage.roots[index];
            if (root->owner.kind != HLSL_STAGE_ROOT_INSTRUCTION || root->instruction < 0 ||
                root->instruction >= program.instruction_count)
                continue;
            const USILInstruction *instruction = &program.instructions[root->instruction];
            if (instruction->opcode != USIL_OP_MOV || instruction->operand_count != 2 ||
                instruction->operands[0].type != OPERAND_TYPE_OUTPUT ||
                usil_operand_destination_lane_mask(&instruction->operands[0]) != 8)
                continue;
            const DXBCOperand *literal = &instruction->operands[1];
            const int component = literal->imm_value_count == 1 ? 0 :
                usil_operand_source_component(literal, 3);
            if (root->owner.source_instruction_index != instruction->source_instruction_index ||
                literal->type != OPERAND_TYPE_IMMEDIATE32 || component < 0 ||
                component >= literal->imm_value_count ||
                literal->imm_values[component] != UINT32_C(0x3f800000) ||
                !root->tree || root->tree->kind != AST_EXPR_LITERAL ||
                root->tree->u.literal.scalar_type != AST_SCALAR_FLOAT32 ||
                root->tree->u.literal.components != 1 ||
                root->tree->u.literal.val[0] != UINT32_C(0x3f800000) ||
                root->begin >= root->end || root->end > source->len) {
                accepted = false;
                break;
            }
            *mutation = (OwnedLiteralMutation){root->begin, root->end,
                root->tree->u.literal.val[0], true};
            ++matches;
            printf("domain_mutation_owner instruction=%d raw_instruction=%u "
                   "destination_lanes=8 begin=%zu end=%zu bits=0x%08" PRIx32 "\n",
                   root->instruction, root->owner.source_instruction_index,
                   root->begin, root->end, root->tree->u.literal.val[0]);
        }
        accepted = accepted && matches == 1;
        if (accepted && linked_interface)
            accepted = linked_interface_freeze(&program, linked_interface);
        printf("domain_%s_wrapper_shape_and_mutation_owned=%d\n",
               linked_interface ? "paired" : "isolated", accepted);
    } else {
        puts("domain_quality=unavailable domain_map=unavailable");
    }
done:
    if (!accepted)
        puts("domain_candidate_comparison=not-run domain_warm_mutation=not-run "
             "domain_warm_restore=not-run");
    if (!linked_interface)
        puts("linked_hull_domain_reconstruction=not-run linked_stage_certificate=not-run");
    hlsl_stage_coverage_dispose(&coverage);
    sb_free(&owned_source);
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    return accepted;
}

/* The source inverse consumes the complete target and, in the explicit scalar
 * calibration, a fixed controlled API layout. Neither authored source nor its
 * preprocessing contract enters this function. No player authority is inferred.
 */
static bool reconstruct_hull(const DXBCContainerView *target,
                             const SerializedProgramParameters *parameters,
                             StringBuilder *source, bool icb_fixture,
                             OwnedLiteralMutation *icb_mutation,
                             ProbeLinkedInterface *linked_interface,
                             OwnedLiteralMutation *maximum_mutation) {
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
    /* The isolated authored wrapper remains triangle-only. A paired wrapper
     * takes domain and patch extent from the actual copied stage interface. */
    HLSLDomainShape linked_shape;
    const bool wrapper_shape = linked_interface
        ? hlsl_domain_shape(program.tessellation.domain, &linked_shape) &&
            program.tessellation.input_control_point_count > 0u &&
            program.tessellation.input_control_point_count <= (unsigned)PROBE_LINKED_CONTROL_POINT_LIMIT &&
            program.tessellation.output_control_point_count == program.tessellation.input_control_point_count
        : program.tessellation.domain == DXBC_TESSELLATOR_DOMAIN_TRIANGLE &&
            program.tessellation.input_control_point_count == 3u &&
            program.tessellation.output_control_point_count == 3u;
    if (program.program_type != DXBC_PROGRAM_TYPE_HULL ||
        !wrapper_shape ||
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
    if (accepted && linked_interface) {
        accepted = maximum_mutation && linked_interface_freeze(&program, linked_interface) &&
            coverage.hull_contract.tessellation.has_max_tessellation_factor &&
            coverage.hull_contract.tessellation.max_tessellation_factor_bits == UINT32_C(0x42000000);
        size_t matches = 0;
        for (size_t index = 0; accepted && index < coverage.root_count; ++index) {
            const HLSLStageOwnedRoot *root = &coverage.roots[index];
            if (root->owner.kind != HLSL_STAGE_ROOT_HULL_MAXIMUM) continue;
            if (root->instruction != -1 || root->owner.phase_index != -1 ||
                root->owner.source_instruction_index !=
                    coverage.hull_contract.tessellation.max_tessellation_factor_source_instruction_index ||
                root->owner.value_bits != UINT32_C(0x42000000) ||
                !root->tree || root->tree->kind != AST_EXPR_LITERAL ||
                root->tree->u.literal.scalar_type != AST_SCALAR_FLOAT32 ||
                root->tree->u.literal.components != 1 ||
                root->tree->u.literal.val[0] != root->owner.value_bits ||
                root->source_unit_id != 2 || root->begin >= root->end || root->end > source->len) {
                accepted = false;
                break;
            }
            *maximum_mutation = (OwnedLiteralMutation){root->begin, root->end,
                root->owner.value_bits, true};
            ++matches;
            printf("hull_maximum_mutation_owner raw_instruction=%u begin=%zu end=%zu bits=0x%08" PRIx32 "\n",
                   root->owner.source_instruction_index, root->begin, root->end, root->owner.value_bits);
        }
        accepted = accepted && matches == 1;
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

static bool candidate_wrapper(const StringBuilder *stage,
                              StringBuilder *wrapper, const char *shader_name,
                              bool domain_fixture, const StringBuilder *paired_domain) {
    if (paired_domain && (domain_fixture || !sb_ok(stage) || !stage->buf ||
        !sb_ok(paired_domain) || !paired_domain->buf ||
        stage->len > PROBE_SOURCE_LIMIT || paired_domain->len > PROBE_SOURCE_LIMIT - stage->len))
        return false;
    sb_appendf(
        wrapper,
        "// SPDX-License-Identifier: GPL-3.0-only\n"
        "%sShader \"%s\"\n{\n"
        "    SubShader\n    {\n        Pass\n        {\n            "
        "HLSLPROGRAM\n"
        "#pragma target 5.0\n#pragma only_renderers d3d11\n#pragma vertex "
        "vert\n"
        "#pragma hull hull\n#pragma domain domain\n#pragma fragment frag\n%s\n",
        paired_domain ? "// Vertex and fragment are authored evaluation stubs.\n"
                      : "// Other stages are authored evaluation stubs.\n",
        shader_name, stage->buf);
    if (paired_domain) {
        sb_append_len(wrapper, paired_domain->buf, paired_domain->len);
        sb_append(wrapper,
        "\nHullPoint vert(float4 input : POSITION) {\n"
        "    HullPoint result = { input.xyz }; return result;\n}\n");
    } else if (domain_fixture) {
        sb_append(wrapper,
        "DomainPoint vert(float4 input : POSITION) {\n"
        "    DomainPoint result = { input.xyz }; return result;\n}\n"
        "DomainFactors ProbePatchFactors(InputPatch<DomainPoint, 3> patch,\n"
        "                                uint id : SV_PrimitiveID) {\n"
        "    DomainFactors result;\n"
        "    result.outer[0] = 3.0f; result.outer[1] = 3.0f; result.outer[2] = 3.0f;\n"
        "    result.inner = 4.0f; return result;\n}\n"
        "[domain(\"tri\")]\n[partitioning(\"integer\")]\n"
        "[outputtopology(\"triangle_cw\")]\n[outputcontrolpoints(3)]\n"
        "[patchconstantfunc(\"ProbePatchFactors\")]\n[maxtessfactor(32.0f)]\n"
        "DomainPoint hull(InputPatch<DomainPoint, 3> patch,\n"
        "                 uint index : SV_OutputControlPointID) { return patch[index]; }\n");
    } else {
        sb_append(wrapper,
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
        "float4(value, 1.0f);\n}\n");
    }
    sb_append(wrapper,
        "float4 frag() : SV_Target { return float4(0.25f, 0.5f, 0.75f, 1.0f); "
        "}\n"
        "ENDHLSL\n        }\n    }\n}\n");
    return sb_ok(wrapper) && (!paired_domain || wrapper->len <= PROBE_SOURCE_LIMIT);
}

static bool changed_factor_wrapper(const StringBuilder *hull,
                                   StringBuilder *wrapper, const char *shader_name,
                                   bool scalar_fixture, const OwnedLiteralMutation *icb_mutation,
                                   bool domain_fixture) {
    /* Match one owned factor assignment or use an independently captured literal.
     * A failed cold comparison remains a failure after this replay check. */
    if (!sb_ok(hull) || !hull->buf || hull->len > PROBE_SOURCE_LIMIT)
        return false;
    if (domain_fixture && !icb_mutation)
        return false;
    if (icb_mutation) {
        if (!icb_mutation->valid || icb_mutation->begin >= icb_mutation->end ||
            icb_mutation->end > hull->len) return false;
        if (domain_fixture && icb_mutation->original_bits != UINT32_C(0x3f800000))
            return false;
        const uint32_t changed_bits = domain_fixture ? UINT32_C(0x40000000)
            : icb_mutation->original_bits == UINT32_C(0x40200000)
            ? UINT32_C(0x40700000) : UINT32_C(0x40200000);
        ASTExpr *literal = ast_create_literal_bits(&changed_bits, 1, AST_SCALAR_FLOAT32);
        if (!literal) return false;
        StringBuilder mutated;
        sb_init(&mutated);
        sb_append_len(&mutated, hull->buf, icb_mutation->begin);
        ast_format_expr(literal, &mutated);
        sb_append_len(&mutated, hull->buf + icb_mutation->end, hull->len - icb_mutation->end);
        ast_free_expr(literal);
        const bool built = sb_ok(&mutated) && candidate_wrapper(&mutated, wrapper, shader_name,
                                                                domain_fixture, NULL);
        if (built) {
            puts(domain_fixture
                ? "warm_mutation owned_domain_w_literal_span=1 classification=not-promoted"
                : "warm_mutation owned_icb_literal_span=1 classification=not-promoted");
            print_hash(domain_fixture ? "mutated_domain_sha256" : "mutated_hull_sha256",
                       mutated.buf, mutated.len);
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
    const bool built = sb_ok(&mutated) && candidate_wrapper(&mutated, wrapper, shader_name, false, NULL);
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
        candidate_wrapper(&calibration, wrapper, shader_name, false, NULL);
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

static bool mutate_owned_literal(const StringBuilder *source,
                                 const OwnedLiteralMutation *mutation,
                                 uint32_t changed_bits, StringBuilder *changed) {
    if (!source || !sb_ok(source) || !source->buf || source->len > PROBE_SOURCE_LIMIT ||
        !mutation || !mutation->valid || mutation->begin >= mutation->end ||
        mutation->end > source->len || !changed || !sb_ok(changed) || changed->len ||
        changed_bits == mutation->original_bits) return false;
    ASTExpr *original = ast_create_literal_bits(&mutation->original_bits, 1, AST_SCALAR_FLOAT32);
    ASTExpr *replacement = ast_create_literal_bits(&changed_bits, 1, AST_SCALAR_FLOAT32);
    StringBuilder formatted;
    sb_init(&formatted);
    if (original) ast_format_expr(original, &formatted);
    const bool bound = original && replacement && sb_ok(&formatted) &&
        formatted.len == mutation->end - mutation->begin &&
        !memcmp(formatted.buf, source->buf + mutation->begin, formatted.len);
    if (bound) {
        sb_append_len(changed, source->buf, mutation->begin);
        ast_format_expr(replacement, changed);
        sb_append_len(changed, source->buf + mutation->end, source->len - mutation->end);
    }
    ast_free_expr(original);
    ast_free_expr(replacement);
    sb_free(&formatted);
    return bound && sb_ok(changed) && changed->len <= PROBE_SOURCE_LIMIT;
}

/* Each pair compiles exactly the same actual source for two selected stages.
 * Responses own their contracts/containers until all comparisons finish. */
static bool compile_linked_pair(UnityCompilerChannel *channel, const char *role,
    const StringBuilder *source, const char *directory, const char *shader_name,
    uint32_t valid_apis, bool scalar_fixture,
    UnityCompilerPreprocessResponse preprocessing[PROBE_LINKED_STAGE_COUNT],
    UnityCompilerSnippetCompileRequest requests[PROBE_LINKED_STAGE_COUNT],
    UnityCompilerBinaryResponse compiled[PROBE_LINKED_STAGE_COUNT],
    UnityCompilerToolchainProvenance provenance[PROBE_LINKED_STAGE_COUNT],
    DXBCContainerView containers[PROBE_LINKED_STAGE_COUNT]) {
    const UnityCompilerProgramStage stages[] = {
        UNITY_COMPILER_PROGRAM_HULL, UNITY_COMPILER_PROGRAM_DOMAIN};
    const char *const names[] = {"hull", "domain"};
    if (!source || !sb_ok(source) || !source->buf || source->len > PROBE_SOURCE_LIMIT)
        return false;
    for (unsigned stage = 0; stage < (unsigned)PROBE_LINKED_STAGE_COUNT; ++stage) {
        printf("paired_request phase=%s stage=%s\n", role, names[stage]);
        if (!compile_selected(channel, role, source->buf, directory, shader_name,
            valid_apis, stages[stage], true, &preprocessing[stage], &requests[stage],
            &compiled[stage], &provenance[stage]) ||
            !dxbc_container_view_first(compiled[stage].data, compiled[stage].size,
                &containers[stage]) ||
            (stage == 0 && scalar_fixture && !scalar_fixture_reflection(&compiled[stage])))
            return false;
        print_hash(stage ? "paired_domain_complete_dxbc_sha256"
                         : "paired_hull_complete_dxbc_sha256",
                   containers[stage].data, containers[stage].size);
    }
    return true;
}

static int run_linked_fixture(char *const argv[], const char *shader_name,
    bool scalar_fixture, bool icb_fixture, const SerializedProgramParameters *parameters) {
    CommonFileBytes authored = {0};
    UnityCompilerChannel channel = {.socket_fd = -1};
    UnityCompilerPreprocessResponse preprocessing[PROBE_LINKED_COMPILE_SLOTS];
    UnityCompilerSnippetCompileRequest requests[PROBE_LINKED_COMPILE_SLOTS] = {0};
    UnityCompilerBinaryResponse compiled[PROBE_LINKED_COMPILE_SLOTS];
    UnityCompilerToolchainProvenance provenance[PROBE_LINKED_COMPILE_SLOTS];
    DXBCContainerView containers[PROBE_LINKED_COMPILE_SLOTS] = {0};
    ProbeLinkedInterface interfaces[PROBE_LINKED_STAGE_COUNT] = {0};
    OwnedLiteralMutation maximum_mutation = {0}, domain_mutation = {0}, icb_mutation = {0};
    StringBuilder source, hull, domain, wrapper, changed, changed_wrapper;
    sb_init(&source);
    sb_init(&hull);
    sb_init(&domain);
    sb_init(&wrapper);
    sb_init(&changed);
    sb_init(&changed_wrapper);
    for (unsigned slot = 0; slot < (unsigned)PROBE_LINKED_COMPILE_SLOTS; ++slot) {
        unity_compiler_preprocess_response_init(&preprocessing[slot]);
        unity_compiler_binary_response_init(&compiled[slot]);
    }
    char directory[PROBE_DIRECTORY_LIMIT];
    UnityCompilerSessionCapabilities capabilities, cold_capabilities;
    uint32_t valid_apis = 0, cold_valid_apis = 0;
    bool reconstructed = false, cold_exact = false;
    unsigned warm_cycles = 0;
    int result = 1;
    puts("scope=selected-native-paired-hull-domain authored_stubs=vertex,fragment "
         "editor=not-run import=not-run semantic_certificate=not-run native_D3D11=not-run "
         "linked_stage_certificate=not-run exported_source=none exported_binary=none");
    printf("metadata_authority=%s player_metadata=not-supplied compile_response_limit=%d\n",
           scalar_fixture ? "controlled-API-fixture" : "none-required", PROBE_LINKED_COMPILE_SLOTS);
    if (common_file_read_regular_terminated(argv[1], PROBE_SOURCE_LIMIT, &authored) != COMMON_FILE_OK ||
        !authored.data || memchr(authored.data, 0, authored.size) ||
        !source_directory(argv[1], directory)) goto done;
    sb_append_len(&source, (char *)authored.data, authored.size);
    print_hash("paired_authored_source_sha256", authored.data, authored.size);
    if (!sb_ok(&source) || !unity_compiler_start_lazy(&channel, argv[2],
            !strcmp(argv[3], "-") ? NULL : argv[3]) ||
        !unity_compiler_capture_session_capabilities(&channel, &capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(&capabilities, &valid_apis) ||
        !(valid_apis & (UINT32_C(1) << 4)) ||
        !unity_compiler_set_expected_valid_apis(&channel, valid_apis) ||
        !compile_linked_pair(&channel, "authored-target", &source, directory, shader_name,
            valid_apis, scalar_fixture, &preprocessing[0], &requests[0], &compiled[0],
            &provenance[0], &containers[0])) goto done;
    if (!reconstruct_hull(&containers[0], scalar_fixture ? parameters : NULL, &hull,
            icb_fixture, icb_fixture ? &icb_mutation : NULL, &interfaces[0], &maximum_mutation) ||
        !inspect_domain(&containers[1], &domain, &domain_mutation, &interfaces[1]) ||
        !linked_interfaces_match(&interfaces[0], &interfaces[1]) ||
        !candidate_wrapper(&hull, &wrapper, shader_name, false, &domain)) goto done;
    reconstructed = true;
    print_hash("paired_reconstructed_hull_sha256", hull.buf, hull.len);
    print_hash("paired_reconstructed_domain_sha256", domain.buf, domain.len);
    print_hash("paired_candidate_source_sha256", wrapper.buf, wrapper.len);
    HLSLDomainShape linked_shape;
    if (!hlsl_domain_shape(interfaces[0].domain, &linked_shape)) goto done;
    printf("paired_interface_match=1 point_semantic=POINTVALUE point_components=3 "
         "hull_input_rw=7 hull_output_rw=8 domain_input_rw=7 hull_patch_rw=14 domain_patch_rw=0 "
         "control_points=%u generated_stage_text_unchanged=1 distinct_generated_types=1 "
         "source_quality=not-promoted domain=%s coordinates=%u outer_factors=%u inner_factors=%u "
         "factor_order=%s\n", interfaces[0].output_control_points, linked_shape.attribute,
         (unsigned)linked_shape.coordinate_count, (unsigned)linked_shape.outer_count,
         (unsigned)linked_shape.inner_count, interfaces[0].inner_first ? "inner-first" : "outer-first");
    /* Both actual target containers and contracts outlive the original process. */
    unity_compiler_shutdown(&channel);
    channel = (UnityCompilerChannel){.socket_fd = -1};
    if (!unity_compiler_start_lazy(&channel, argv[2], !strcmp(argv[3], "-") ? NULL : argv[3]) ||
        !unity_compiler_capture_session_capabilities(&channel, &cold_capabilities) ||
        !unity_compiler_session_capabilities_valid_apis(&cold_capabilities, &cold_valid_apis) ||
        cold_valid_apis != valid_apis ||
        !unity_compiler_session_capabilities_equal(&capabilities, &cold_capabilities) ||
        !unity_compiler_set_expected_valid_apis(&channel, valid_apis) ||
        !compile_linked_pair(&channel, "cold-generated-pair", &wrapper, directory, shader_name,
            valid_apis, scalar_fixture, &preprocessing[2], &requests[2], &compiled[2],
            &provenance[2], &containers[2])) goto done;
    const pid_t cold_process = channel.process_id;
    if (!cold_process) goto done;
    cold_exact = true;
    for (unsigned stage = 0; stage < (unsigned)PROBE_LINKED_STAGE_COUNT; ++stage) {
        DXBCCompareResult comparison;
        const DXBCCompareStatus status = dxbc_compare_exact(containers[stage].data,
            containers[stage].size, containers[2 + stage].data, containers[2 + stage].size, &comparison);
        const bool controls = selected_controls_equal(&requests[stage], &requests[2 + stage]);
        const bool environment = compiler_environment_equal(&provenance[stage], &provenance[2 + stage]);
        printf("paired_cold stage=%s complete_comparison=%s expected_bytes=%zu actual_bytes=%zu "
               "selected_controls_equal=%d compiler_environment_equal=%d chunk=%u instruction=%u token=%u\n",
               stage ? "domain" : "hull", dxbc_compare_status_name(status),
               containers[stage].size, containers[2 + stage].size, controls, environment,
               comparison.chunk_index, comparison.instruction_index, comparison.token_index);
        cold_exact = cold_exact && status == DXBC_COMPARE_EQUAL && controls && environment;
    }
    if (!cold_exact) goto done;
    puts("paired_cold_exact=1 process_count=2 candidate_cold=1 session_capabilities_identical=1");
    /* Independent source changes: maximum32->16, then W1->2. The unchanged
     * sibling is compiled from the same joint source and must remain exact. */
    for (unsigned mutation_stage = 0; mutation_stage < (unsigned)PROBE_LINKED_STAGE_COUNT; ++mutation_stage) {
        const unsigned mutation_slot = 4 + mutation_stage * 4;
        const unsigned restore_slot = mutation_slot + 2;
        const StringBuilder *original = mutation_stage ? &domain : &hull;
        const OwnedLiteralMutation *mutation = mutation_stage ? &domain_mutation : &maximum_mutation;
        const uint32_t changed_bits = mutation_stage ? UINT32_C(0x40000000) : UINT32_C(0x41800000);
        if (mutation->original_bits != (mutation_stage ? UINT32_C(0x3f800000) : UINT32_C(0x42000000)) ||
            !mutate_owned_literal(original, mutation, changed_bits, &changed) ||
            !candidate_wrapper(mutation_stage ? &hull : &changed, &changed_wrapper, shader_name,
                false, mutation_stage ? &changed : &domain)) goto done;
        print_hash("paired_mutated_source_sha256", changed_wrapper.buf, changed_wrapper.len);
        if (!compile_linked_pair(&channel, mutation_stage ? "warm-domain-w-mutated" : "warm-hull-maximum-mutated",
            &changed_wrapper, directory, shader_name, valid_apis, scalar_fixture,
            &preprocessing[mutation_slot], &requests[mutation_slot], &compiled[mutation_slot],
            &provenance[mutation_slot], &containers[mutation_slot])) goto done;
        for (unsigned stage = 0; stage < (unsigned)PROBE_LINKED_STAGE_COUNT; ++stage) {
            DXBCCompareResult comparison;
            const unsigned slot = mutation_slot + stage, cold_slot = 2 + stage;
            const DXBCCompareStatus status = dxbc_compare_exact(containers[stage].data,
                containers[stage].size, containers[slot].data, containers[slot].size, &comparison);
            const bool differs = status != DXBC_COMPARE_EQUAL && status != DXBC_COMPARE_INVALID_ARGUMENT &&
                status != DXBC_COMPARE_EXPECTED_INVALID && status != DXBC_COMPARE_ACTUAL_INVALID;
            const bool authority = selected_controls_equal(&requests[cold_slot], &requests[slot]) &&
                compiler_environment_equal(&provenance[cold_slot], &provenance[slot]) &&
                provenance[cold_slot].source_authority_revision == provenance[slot].source_authority_revision &&
                cold_process == channel.process_id;
            printf("paired_warm_mutation changed_stage=%s compiled_stage=%s complete_comparison=%s "
                   "expected_relation=%s authority_equal=%d same_process=%d\n",
                   mutation_stage ? "domain" : "hull", stage ? "domain" : "hull",
                   dxbc_compare_status_name(status), stage == mutation_stage ? "different" : "exact-sibling",
                   authority, cold_process == channel.process_id);
            if (!authority || (stage == mutation_stage ? !differs : status != DXBC_COMPARE_EQUAL)) goto done;
        }
        if (!compile_linked_pair(&channel, mutation_stage ? "warm-domain-original-restored" : "warm-hull-original-restored",
            &wrapper, directory, shader_name, valid_apis, scalar_fixture,
            &preprocessing[restore_slot], &requests[restore_slot], &compiled[restore_slot],
            &provenance[restore_slot], &containers[restore_slot])) goto done;
        for (unsigned stage = 0; stage < (unsigned)PROBE_LINKED_STAGE_COUNT; ++stage) {
            DXBCCompareResult comparison;
            const unsigned slot = restore_slot + stage, cold_slot = 2 + stage;
            const DXBCCompareStatus status = dxbc_compare_exact(containers[stage].data,
                containers[stage].size, containers[slot].data, containers[slot].size, &comparison);
            const bool identity = !memcmp(compiled[cold_slot].request_digest, compiled[slot].request_digest, 32) &&
                !memcmp(compiled[cold_slot].controls_digest, compiled[slot].controls_digest, 32);
            const bool authority = selected_controls_equal(&requests[cold_slot], &requests[slot]) &&
                compiler_environment_equal(&provenance[cold_slot], &provenance[slot]) &&
                provenance[cold_slot].source_authority_revision == provenance[slot].source_authority_revision &&
                cold_process == channel.process_id;
            printf("paired_warm_restore changed_stage=%s compiled_stage=%s reference=original-target "
                   "complete_comparison=%s canonical_identity_equal=%d authority_equal=%d same_process=%d\n",
                   mutation_stage ? "domain" : "hull", stage ? "domain" : "hull",
                   dxbc_compare_status_name(status), identity, authority, cold_process == channel.process_id);
            if (status != DXBC_COMPARE_EQUAL || !identity || !authority) goto done;
        }
        ++warm_cycles;
        sb_free(&changed);
        sb_init(&changed);
        sb_free(&changed_wrapper);
        sb_init(&changed_wrapper);
    }
    result = 0;
done:
    printf("paired_generated_sources=%s paired_cold_comparison=%s paired_warm_cycles=%u paired_warm_comparison=%s "
           "paired_source_native_stage_exact=%d linked_stage_certificate=not-run "
           "editor=not-run import=not-run semantic_certificate=not-run native_D3D11=not-run\n",
           reconstructed ? "complete" : "unavailable-or-failed",
           cold_exact ? "exact" : "not-qualified", warm_cycles,
           !cold_exact ? "not-run" : warm_cycles == 2 ? "qualified" : "not-qualified", !result);
    for (unsigned slot = 0; slot < (unsigned)PROBE_LINKED_COMPILE_SLOTS; ++slot) {
        unity_compiler_binary_response_free(&compiled[slot]);
        unity_compiler_preprocess_response_free(&preprocessing[slot]);
    }
    unity_compiler_shutdown(&channel);
    common_file_bytes_dispose(&authored);
    sb_free(&source);
    sb_free(&hull);
    sb_free(&domain);
    sb_free(&wrapper);
    sb_free(&changed);
    sb_free(&changed_wrapper);
    return result;
}

static void usage(const char *name) {
    fprintf(
        stderr,
        "usage: %s SOURCE.shader PROJECT_ROOT INCLUDES_DIR [--icb-fixture | "
        "--scalar-fixture [--static-factor-calibration | --explicit-packoffset-calibration | "
        "--inner-factor-brace-calibration]] [--domain-fixture | --linked-fixture | --structured-fixture | --structured-vertex-fixture]\n"
        "Use '-' for no additional includes. Selected-native isolated stage comparison "
        "only; no source or binary files exported.\n"
        "--scalar-fixture supplies a controlled FactorInputs/_Factor API layout,\n"
        "validated against native reflection; no player metadata authority.\n"
        "--icb-fixture requires an actual parsed scalar table; its configuration "
        "gap remains MIXED.\n"
        "--domain-fixture instead compiles the supplied fixture's actual DOMAIN "
        "target, prints bounded decoded facts, and attempts the high-level inverse. "
        "Generated source is compared in a separate cold process; an exact result "
        "enables an owned W-literal mutation and exact warm restoration. "
        "It prints no source and grants no linked HULL/DOMAIN certificate. "
        "It cannot be combined with a HULL source calibration.\n"
        "--linked-fixture reconstructs the actual HULL and DOMAIN targets together; "
        "only vertex/fragment are authored stubs. Typed triangle/quad/isoline FLOAT3 interfaces "
        "must match. Both cold full-container comparisons must be exact before "
        "independent owned maximum/W mutations and exact sibling/restoration checks. "
        "This summary-only experiment grants no runtime or semantic certificate "
        "and cannot be combined with a source calibration.\n"
        "--structured-fixture reconstructs the actual FLOAT3 fragment target. "
        "Only vertex is an authored stub; --structured-vertex-fixture instead "
        "reconstructs vertex with a fragment stub. An exact cold comparison enables a "
        "decoded broadcast literal change, followed by exact original-target "
        "warm restoration. It cannot be combined with other fixture options.\n"
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
    const bool structured_vertex_fixture = argc == 5 && !strcmp(argv[4], "--structured-vertex-fixture");
    const bool structured_fixture = structured_vertex_fixture ||
        (argc == 5 && !strcmp(argv[4], "--structured-fixture"));
    const bool scalar_fixture = argc >= 5 && !strcmp(argv[4], "--scalar-fixture");
    const bool icb_fixture = argc >= 5 && !strcmp(argv[4], "--icb-fixture");
    const bool domain_fixture =
        (argc == 5 && !strcmp(argv[4], "--domain-fixture")) ||
        (argc == 6 && (scalar_fixture || icb_fixture) &&
         !strcmp(argv[5], "--domain-fixture"));
    const bool linked_fixture =
        (argc == 5 && !strcmp(argv[4], "--linked-fixture")) ||
        (argc == 6 && (scalar_fixture || icb_fixture) &&
         !strcmp(argv[5], "--linked-fixture"));
    const bool calibrate_static_factors = argc == 6 && scalar_fixture &&
        !strcmp(argv[5], "--static-factor-calibration");
    const bool calibrate_explicit_packoffset = argc == 6 && scalar_fixture &&
        !strcmp(argv[5], "--explicit-packoffset-calibration");
    const bool calibrate_inner_factor_brace = argc == 6 && scalar_fixture &&
        !strcmp(argv[5], "--inner-factor-brace-calibration");
    const bool calibrate_source_shape = calibrate_static_factors ||
        calibrate_explicit_packoffset || calibrate_inner_factor_brace;
    if (argc != 4 && !domain_fixture && !linked_fixture && !structured_fixture && !(icb_fixture && argc == 5) &&
        !(scalar_fixture && (argc == 5 || calibrate_source_shape))) {
        usage(argv[0]);
        return 2;
    }
    CommonFileBytes authored = {0};
    const char *shader_name = structured_vertex_fixture ? "Fixture/HighLevel/StructuredFloat3VertexBranch" :
        structured_fixture ? "Fixture/HighLevel/StructuredFloat3Branch" : scalar_fixture ? "Fixture/HighLevel/HullFloat3ScalarCBuffer"
        : icb_fixture ? "Fixture/HighLevel/HullFloat3ICB" : default_shader_name;
    OwnedLiteralMutation icb_mutation = {0};
    OwnedLiteralMutation domain_mutation = {0};
    ProbeDecodedLiteral structured_literal = {0};
    const UnityCompilerProgramStage selected_stage = structured_vertex_fixture ? UNITY_COMPILER_PROGRAM_VERTEX :
        structured_fixture ? UNITY_COMPILER_PROGRAM_FRAGMENT :
        domain_fixture ? UNITY_COMPILER_PROGRAM_DOMAIN : UNITY_COMPILER_PROGRAM_HULL;
    const bool summary_only = domain_fixture || structured_fixture;
    SerializedVariable field = {.name = "_Factor", .layout = {0, 0, 0, 1, 0, 0}};
    SerializedConstantBuffer buffer = {.name = "FactorInputs", .size = 16,
        .role = SERIALIZED_CBUFFER_NAMED, .variables = &field, .var_count = 1};
    SerializedResourceParam binding = {.name = "FactorInputs",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .array_size = 1};
    SerializedProgramParameters parameters = {.constant_buffers = &buffer,
        .cb_count = 1, .resources = &binding, .res_count = 1};
    if (linked_fixture)
        return run_linked_fixture(argv, shader_name, scalar_fixture, icb_fixture, &parameters);
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
    printf("scope=%s authored_stubs=%s "
           "editor=not-run "
           "import=not-run semantic_certificate=not-run native_D3D11=not-run\n"
           "session raw_mask=0x%08" PRIx32 " valid_apis=0x%08" PRIx32 "\n",
           structured_vertex_fixture ? "selected-native-structured-vertex" :
               structured_fixture ? "selected-native-structured-fragment" :
               domain_fixture ? "selected-native-domain" : "selected-native-hull",
           structured_vertex_fixture ? "fragment" : structured_fixture ? "vertex" : domain_fixture ? "vertex,hull,fragment" : "vertex,domain,fragment",
           capabilities.raw_available_platform_mask, valid_apis);
    print_hash("authored_source_sha256", authored.data, authored.size);
    printf("metadata_authority=%s player_metadata=not-supplied\n",
           scalar_fixture && !domain_fixture ? "controlled-API-fixture" : "none-required");
    if (!compile_selected(&channel, structured_fixture ? "authored-structured-target" :
                          domain_fixture ? "authored-domain-target" : "authored-target",
                          (char *)authored.data, directory, shader_name, valid_apis,
                          selected_stage, summary_only, &preprocessing[0], &requests[0],
                          &compiled[0], &provenance[0]))
        goto done;
    DXBCContainerView target = {0}, candidate = {0};
    if (structured_fixture) {
        if (!dxbc_container_view_first(compiled[0].data, compiled[0].size, &target)) goto done;
        print_hash("target_complete_dxbc_sha256", target.data, target.size);
        if (!reconstruct_structured(&target, &hull, &structured_literal, false, structured_vertex_fixture) ||
            !structured_wrapper(&hull, &wrapper, shader_name, structured_vertex_fixture)) goto done;
    }
    if (domain_fixture) {
        if (!dxbc_container_view_first(compiled[0].data, compiled[0].size, &target))
            goto done;
        print_hash("target_complete_dxbc_sha256", target.data, target.size);
        if (!inspect_domain(&target, &hull, &domain_mutation, NULL) ||
            !candidate_wrapper(&hull, &wrapper, shader_name, true, NULL))
            goto done;
        puts("stage_comparison=isolated-domain authored_stubs=vertex,hull,fragment "
             "linked_hull_domain_reconstruction=not-run linked_stage_certificate=not-run");
    }
    if (!domain_fixture && scalar_fixture && !scalar_fixture_reflection(&compiled[0])) {
        fputs("Controlled scalar fixture reflection mismatch.\n", stderr);
        goto done;
    }
    if (!domain_fixture && !structured_fixture) {
        if (scalar_fixture) printf("controlled_API_layout_native_reflection_checked=1\n");
        if (!dxbc_container_view_first(compiled[0].data, compiled[0].size, &target))
            goto done;
        print_hash("target_complete_dxbc_sha256", target.data, target.size);
        if (!reconstruct_hull(&target, scalar_fixture ? &parameters : NULL, &hull,
                          icb_fixture, icb_fixture ? &icb_mutation : NULL, NULL, NULL) ||
            !candidate_wrapper(&hull, &wrapper, shader_name, false, NULL))
            goto done;
        print_hash("reconstructed_hull_sha256", hull.buf, hull.len);
        printf("generated_hull_begin\n%s\ngenerated_hull_end\n", hull.buf);
    }
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
    if (!compile_selected(&channel, structured_fixture ? "target-only-structured-candidate" :
                        domain_fixture ? "target-only-domain-candidate" : "target-only-hull-candidate",
                        wrapper.buf, directory, shader_name, valid_apis,
                        selected_stage, summary_only, &preprocessing[1], &requests[1],
                        &compiled[1], &provenance[1]) ||
        !dxbc_container_view_first(compiled[1].data, compiled[1].size,
                                   &candidate))
        goto done;
    if (!domain_fixture && scalar_fixture && !scalar_fixture_reflection(&compiled[1])) goto done;
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
    if (domain_fixture)
        printf("domain_candidate_comparison=%s source_quality=not-promoted "
               "linked_stage_certificate=not-run\n", dxbc_compare_status_name(status));
    if (!controls_equal || !toolchain_equal ||
        (!cold_equal && (domain_fixture || (!scalar_fixture && !icb_fixture)))) {
        if (domain_fixture)
            puts("domain_warm_mutation=not-run domain_warm_restore=not-run cold_exact=0");
        goto done;
    }

    /* A changed source must affect a warm compiler, and returning to the
     * original source must restore both exact bytes and canonical identity. */
    const pid_t candidate_process = channel.process_id;
    DXBCContainerView changed = {0}, repeated = {0};
    bool mutation_built = false;
    if (structured_fixture) {
        StringBuilder mutated_fragment;
        sb_init(&mutated_fragment);
        mutation_built = reconstruct_structured(&target, &mutated_fragment, &structured_literal, true, structured_vertex_fixture) &&
            structured_wrapper(&mutated_fragment, &changed_wrapper, shader_name, structured_vertex_fixture);
        sb_free(&mutated_fragment);
    } else {
        mutation_built = changed_factor_wrapper(&hull, &changed_wrapper, shader_name, scalar_fixture,
            domain_fixture ? &domain_mutation : icb_fixture ? &icb_mutation : NULL, domain_fixture);
    }
    if (!candidate_process || !mutation_built ||
        !compile_selected(&channel, structured_fixture ? "warm-mutated-structured" :
                        domain_fixture ? "warm-mutated-domain" : "warm-mutated-hull",
                        changed_wrapper.buf, directory, shader_name, valid_apis,
                        selected_stage, summary_only, &preprocessing[2], &requests[2],
                        &compiled[2], &provenance[2]) ||
        !dxbc_container_view_first(compiled[2].data, compiled[2].size,
                                   &changed))
        goto done;
    if (!domain_fixture && scalar_fixture && !scalar_fixture_reflection(&compiled[2])) goto done;
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
    if (!compile_selected(&channel, structured_fixture ? "warm-original-structured" :
                        domain_fixture ? "warm-original-domain" : "warm-original-hull",
                        wrapper.buf, directory, shader_name, valid_apis,
                        selected_stage, summary_only, &preprocessing[3], &requests[3],
                        &compiled[3], &provenance[3]) ||
        !dxbc_container_view_first(compiled[3].data, compiled[3].size,
                                   &repeated))
        goto done;
    if (!domain_fixture && scalar_fixture && !scalar_fixture_reflection(&compiled[3])) goto done;
    const DXBCCompareStatus repeated_status = dxbc_compare_exact(
        summary_only ? target.data : candidate.data,
        summary_only ? target.size : candidate.size,
        repeated.data, repeated.size, &comparison);
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
    printf("warm_original_equal=%d comparison=%s reference=%s canonical_identity_equal=%d "
           "authority_equal=%d same_process=%d\n",
           repeated_status == DXBC_COMPARE_EQUAL,
           dxbc_compare_status_name(repeated_status),
           summary_only ? "original-target" : "cold-candidate", repeated_identity,
           repeated_authority, candidate_process == channel.process_id);
    result = cold_equal && repeated_status == DXBC_COMPARE_EQUAL && repeated_identity &&
                     repeated_authority
                 ? 0
                 : 1;
    if (structured_fixture)
        printf("structured_isolated_stage_qualified=%d original_target_reference=1 "
               "decoded_literal_mutation=%s warm_restore=%s source_quality=not-promoted\n",
               !result, mutation_diff && mutation_authority ? "different" : "failed",
               !result ? "exact" : "failed");
    if (domain_fixture)
        printf("domain_isolated_stage_qualified=%d domain_warm_mutation=%s "
               "domain_warm_restore=%s linked_hull_domain_reconstruction=not-run "
               "linked_stage_certificate=not-run\n", !result,
               mutation_diff && mutation_authority ? "different" : "failed",
               !result ? "exact" : "failed");
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
            if (!transformed || !candidate_wrapper(&changed_wrapper, &calibration_wrapper, shader_name, false, NULL))
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
                !changed_factor_wrapper(&changed_wrapper, &wrapper, shader_name, true, NULL, false) ||
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
