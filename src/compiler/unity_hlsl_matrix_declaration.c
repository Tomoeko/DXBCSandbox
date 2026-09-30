// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_hlsl_matrix_declaration.h"
#include "compiler/unity_compile_authority.h"
#include "common/sha256.h"
#include "common/shader_stage.h"
#include "dxbc/dxbc_stage_contract.h"
#include <stdlib.h>
#include <string.h>

/* No public caller-filled receipt constructor. All pointed-to inputs and live
 * response bytes are owned here; public accessors return only by-value facts. */
struct UnityHlslMatrixDeclarationReceipt {
    UnityCompilerSnippetCompileRequest request;
    SnippetCompileContract contract;
    UnityCompileProfile profile;
    PlayerSubProgramMetadata player;
    SerializedProgramParameters current, common;
    bool has_current, has_common;
    uint8_t *target, *profile_bytes, *compile_bytes, *preprocess_bytes, *control_bytes;
    size_t target_size, profile_size, compile_size, preprocess_size, control_size;
    UnityCompilerBinaryResponse compiled, expanded;
    HLSLCurrentMatrixReads reads;
    UnityHlslCBufferInventory declarations;
    UnityHlslMatrixDeclarationField *fields;
    UnityHlslMatrixDeclarationSummary summary;
};

enum { REQUEST_KEYWORD_LIMIT = 4096, REQUEST_TEXT_LIMIT = 2 * 1024 * 1024,
       TARGET_BYTE_LIMIT = 64 * 1024 * 1024 };

static bool text_shape(const char *text, size_t maximum) {
    if (!text) return false;
    for (size_t index = 0; index <= maximum; ++index)
        if (!text[index]) return true;
    return false;
}
static bool keywords_shape(char *const *values, int count) {
    if (count < 0 || count > REQUEST_KEYWORD_LIMIT || (count && !values)) return false;
    for (int index = 0; index < count; ++index) {
        if (!text_shape(values[index], 4096) || !values[index][0]) return false;
        for (int prior = 0; prior < index; ++prior)
            if (!strcmp(values[index], values[prior])) return false;
    }
    return true;
}
static bool keyword_lists_overlap(char *const *a, int a_count, char *const *b, int b_count) {
    for (int left = 0; left < a_count; ++left) for (int right = 0; right < b_count; ++right)
        if (!strcmp(a[left], b[right])) return true;
    return false;
}
static bool request_shape(const UnityCompilerSnippetCompileRequest *request) {
    return request && text_shape(request->snippet_source, REQUEST_TEXT_LIMIT) &&
        text_shape(request->source_directory, 4096) && text_shape(request->source_basename, 4096) &&
        text_shape(request->pass_name, 4096) && !request->preprocess_only && request->platform == 4 &&
        request->shader_type >= 0 && request->shader_type <= 4 && request->program_mask > 0 &&
        keywords_shape(request->variant_keywords, request->variant_keyword_count) &&
        keywords_shape(request->user_keywords, request->user_keyword_count) &&
        keywords_shape(request->disabled_keywords, request->disabled_keyword_count) &&
        !keyword_lists_overlap(request->variant_keywords, request->variant_keyword_count, request->user_keywords, request->user_keyword_count) &&
        !keyword_lists_overlap(request->variant_keywords, request->variant_keyword_count, request->disabled_keywords, request->disabled_keyword_count) &&
        !keyword_lists_overlap(request->user_keywords, request->user_keyword_count, request->disabled_keywords, request->disabled_keyword_count) &&
        request->contract && unity_compiler_snippet_contract_validate(request->contract) &&
        (request->contract->language == 0 || request->contract->language == 3);
}
static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}
static void keywords_free(char **values, int count) {
    for (int index = 0; values && index < count; ++index) free(values[index]);
    free(values);
}
static bool keywords_copy(char ***out, char *const *values, int count) {
    *out = NULL;
    if (!count) return true;
    *out = calloc((size_t)count, sizeof(**out));
    if (!*out) return false;
    for (int index = 0; index < count; ++index)
        if (!((*out)[index] = copy_text(values[index]))) return false;
    return true;
}
static bool keywords_equal(char *const *left, int left_count, char *const *right, int right_count) {
    if (!keywords_shape(right, right_count) || left_count != right_count) return false;
    for (int index = 0; index < left_count; ++index)
        if (strcmp(left[index], right[index])) return false;
    return true;
}
static bool request_copy(UnityHlslMatrixDeclarationReceipt *receipt,
                         const UnityCompilerSnippetCompileRequest *request) {
    receipt->request = *request;
    receipt->request.snippet_source = receipt->request.source_directory = NULL;
    receipt->request.source_basename = receipt->request.pass_name = NULL;
    receipt->request.variant_keywords = receipt->request.user_keywords = NULL;
    receipt->request.disabled_keywords = NULL;
    receipt->request.contract = &receipt->contract;
    receipt->request.snippet_source = copy_text(request->snippet_source);
    receipt->request.source_directory = copy_text(request->source_directory);
    receipt->request.source_basename = copy_text(request->source_basename);
    receipt->request.pass_name = copy_text(request->pass_name);
    return receipt->request.snippet_source && receipt->request.source_directory &&
        receipt->request.source_basename && receipt->request.pass_name &&
        keywords_copy(&receipt->request.variant_keywords, request->variant_keywords, request->variant_keyword_count) &&
        keywords_copy(&receipt->request.user_keywords, request->user_keywords, request->user_keyword_count) &&
        keywords_copy(&receipt->request.disabled_keywords, request->disabled_keywords, request->disabled_keyword_count) &&
        unity_compiler_snippet_contract_copy(&receipt->contract, request->contract);
}
static bool player_shape(const PlayerSubProgramMetadata *player) {
    return player && keywords_shape(player->local_keywords, player->local_keyword_count) &&
        keywords_shape(player->global_keywords, player->global_keyword_count) &&
        player->bytecode && player->bytecode_length && player->bytecode_length <= TARGET_BYTE_LIMIT &&
        player->binding_count >= 0 && player->binding_count <= REQUEST_KEYWORD_LIMIT &&
        (!player->binding_count || player->bindings);
}
static bool player_copy(PlayerSubProgramMetadata *out, const PlayerSubProgramMetadata *player) {
    *out = *player;
    out->local_keywords = out->global_keywords = NULL;
    out->bytecode = NULL;
    out->bindings = NULL;
    if (!keywords_copy(&out->local_keywords, player->local_keywords, player->local_keyword_count) ||
        !keywords_copy(&out->global_keywords, player->global_keywords, player->global_keyword_count)) return false;
    uint8_t *bytecode = malloc(player->bytecode_length);
    if (!bytecode) return false;
    memcpy(bytecode, player->bytecode, player->bytecode_length);
    out->bytecode = bytecode;
    if (player->binding_count) {
        out->bindings = malloc((size_t)player->binding_count * sizeof(*out->bindings));
        if (!out->bindings) return false;
        memcpy(out->bindings, player->bindings, (size_t)player->binding_count * sizeof(*out->bindings));
    }
    return true;
}
static bool player_equal(const PlayerSubProgramMetadata *left, const PlayerSubProgramMetadata *right) {
    if (!player_shape(right) || left->version != right->version || left->dialect != right->dialect ||
        left->program_type != right->program_type || left->has_player_blob_header != right->has_player_blob_header ||
        memcmp(left->player_header_words, right->player_header_words, sizeof(left->player_header_words)) ||
        left->source_map != right->source_map || left->bytecode_length != right->bytecode_length ||
        memcmp(left->bytecode, right->bytecode, left->bytecode_length) || left->binding_count != right->binding_count ||
        !keywords_equal(left->local_keywords, left->local_keyword_count, right->local_keywords, right->local_keyword_count) ||
        !keywords_equal(left->global_keywords, left->global_keyword_count, right->global_keywords, right->global_keyword_count)) return false;
    for (int index = 0; index < left->binding_count; ++index)
        if (left->bindings[index].channel != right->bindings[index].channel ||
            left->bindings[index].component != right->bindings[index].component) return false;
    return true;
}
/* Compare typed fields rather than allocation addresses, pools or C padding. */
static bool variable_equal(const SerializedVariable *left, const SerializedVariable *right) {
    return right->name && !strcmp(left->name, right->name) &&
        !memcmp(left->layout, right->layout, sizeof(left->layout));
}
static bool parameters_equal(const SerializedProgramParameters *left, const SerializedProgramParameters *right) {
    if (!right || left->version != right->version || left->dialect != right->dialect ||
        left->is_binary != right->is_binary || left->cb_count != right->cb_count || left->res_count != right->res_count ||
        (right->cb_count && !right->constant_buffers) || (right->res_count && !right->resources)) return false;
    for (int index = 0; index < left->cb_count; ++index) {
        const SerializedConstantBuffer *a = &left->constant_buffers[index], *b = &right->constant_buffers[index];
        if (!b->name || strcmp(a->name, b->name) || a->role != b->role || a->size != b->size ||
            a->has_is_partial != b->has_is_partial || a->is_partial != b->is_partial ||
            a->var_count != b->var_count || a->struct_count != b->struct_count ||
            (b->var_count && !b->variables) || (b->struct_count && !b->struct_params)) return false;
        for (int field = 0; field < a->var_count; ++field)
            if (!variable_equal(&a->variables[field], &b->variables[field])) return false;
        for (int structure = 0; structure < a->struct_count; ++structure) {
            const SerializedStructParam *sa = &a->struct_params[structure], *sb = &b->struct_params[structure];
            if (!sb->name || strcmp(sa->name, sb->name) || memcmp(sa->layout, sb->layout, sizeof(sa->layout)) ||
                sa->member_count != sb->member_count || (sb->member_count && !sb->members)) return false;
            for (int member = 0; member < sa->member_count; ++member)
                if (!variable_equal(&sa->members[member], &sb->members[member])) return false;
        }
    }
    for (int index = 0; index < left->res_count; ++index) {
        const SerializedResourceParam *a = &left->resources[index], *b = &right->resources[index];
        if (!b->name || strcmp(a->name, b->name) || a->bind_type != b->bind_type || a->bind_index != b->bind_index ||
            a->array_size != b->array_size || a->dimension != b->dimension || a->sampler_index != b->sampler_index ||
            a->multisampled != b->multisampled || a->original_index != b->original_index ||
            a->sampler_state != b->sampler_state || memcmp(a->extra, b->extra, sizeof(a->extra))) return false;
    }
    return true;
}

void unity_hlsl_matrix_declaration_free(UnityHlslMatrixDeclarationReceipt *receipt) {
    if (!receipt) return;
    free((char *)receipt->request.snippet_source); free((char *)receipt->request.source_directory);
    free((char *)receipt->request.source_basename); free((char *)receipt->request.pass_name);
    keywords_free(receipt->request.variant_keywords, receipt->request.variant_keyword_count);
    keywords_free(receipt->request.user_keywords, receipt->request.user_keyword_count);
    keywords_free(receipt->request.disabled_keywords, receipt->request.disabled_keyword_count);
    unity_compiler_snippet_contract_free(&receipt->contract);
    keywords_free(receipt->player.local_keywords, receipt->player.local_keyword_count);
    keywords_free(receipt->player.global_keywords, receipt->player.global_keyword_count);
    free((uint8_t *)receipt->player.bytecode); free(receipt->player.bindings);
    serialized_program_parameters_free(&receipt->current);
    serialized_program_parameters_free(&receipt->common);
    unity_compiler_binary_response_free(&receipt->compiled);
    unity_compiler_binary_response_free(&receipt->expanded);
    hlsl_current_matrix_reads_dispose(&receipt->reads);
    unity_hlsl_cbuffer_inventory_dispose(&receipt->declarations);
    free(receipt->fields); free(receipt->target); free(receipt->profile_bytes);
    free(receipt->compile_bytes); free(receipt->preprocess_bytes); free(receipt->control_bytes);
    free(receipt);
}

static HLSLCurrentMatrixStatus decode_reads(const UnityHlslMatrixDeclarationInput *input,
                                           HLSLCurrentMatrixReads *reads) {
    DXBCDocument document; dxbc_document_init(&document);
    DXBCStageContract stage; dxbc_stage_contract_init(&stage);
    DXBCContainer semantic = {0}; USILProgram program = {0};
    HLSLCurrentMatrixStatus status = HLSL_CURRENT_MATRIX_INVALID_PROGRAM;
    DXBCDocumentDiagnostic document_diagnostic; DXBCStageContractDiagnostic stage_diagnostic;
    DXBCContainerView player_target;
    ShaderStageTuple tuple = {0};
    if (!dxbc_container_view_first(input->player->bytecode, input->player->bytecode_length, &player_target) ||
        player_target.size != input->target_size || memcmp(player_target.data, input->target, input->target_size) ||
        !dxbc_document_parse(&document, input->target, input->target_size, &document_diagnostic) ||
        !dxbc_document_decode_semantic(&document, &semantic) ||
        !dxbc_stage_contract_decode(&document, &semantic, &stage, &stage_diagnostic) ||
        !shader_stage_compiler_to_serialized((UnityCompilerProgramStage)input->request->shader_type, &tuple.serialized_stage)) goto done;
    tuple.compiler_program = (UnityCompilerProgramStage)input->request->shader_type;
    tuple.serialized_program_mask = (uint32_t)input->request->program_mask;
    tuple.gpu_program_type = (UnityGPUProgramType)input->player->program_type;
    tuple.dxbc_program_type = stage.program_type;
    tuple.shader_model_major = stage.shader_model_major; tuple.shader_model_minor = stage.shader_model_minor;
    if (shader_stage_validate_d3d11_tuple(&tuple) != SHADER_STAGE_TUPLE_OK ||
        !usil_translate_with_stage_contract(&program, &semantic, &stage)) goto done;
    status = hlsl_current_matrix_reads_build(&program, input->current_parameters, input->common_parameters, reads);
 done:
    usil_free(&program); dxbc_free(&semantic); dxbc_stage_contract_free(&stage); dxbc_document_free(&document);
    return status;
}
static bool keyword_present(char *const *values, int count, const char *name) {
    for (int index = 0; index < count; ++index) if (!strcmp(values[index], name)) return true;
    return false;
}
/* Reuse the authoritative platform keyword table/settings mask. Check every
 * D3D11 capability, not merely the precision bit: a freshly fingerprinted
 * gamma/mobile/tier mismatch cannot authorize the same request. Unrelated
 * authoring keyword/pass controls remain bound by the complete transcript. */
static bool precision_authority(const UnityHlslMatrixDeclarationInput *input) {
    if (!input->profile || input->legacy_half_contract != UNITY_HLSL_MATRIX_LEGACY_HALF_CAPTURED_FLOAT32 ||
        !unity_compile_profile_validate(input->profile) || input->request->build_platform != input->profile->build_platform ||
        (input->profile->d3d11_capabilities & (UINT64_C(1) << 32))) return false;
    uint8_t fingerprint[32];
    if (unity_compile_profile_fingerprint(input->profile, fingerprint) != UNITY_COMPILE_PROFILE_OK ||
        memcmp(fingerprint, input->profile->fingerprint, sizeof(fingerprint))) return false;
    for (size_t capability = 0; capability < UNITY_PLATFORM_CAPABILITY_COUNT; ++capability) {
        const char *name = unity_platform_capability_keyword(capability);
        const bool enabled = (input->profile->d3d11_capabilities & (UINT64_C(1) << capability)) != 0;
        if (!name || keyword_present(input->request->variant_keywords, input->request->variant_keyword_count, name) != enabled ||
            (!enabled && keyword_present(input->request->user_keywords, input->request->user_keyword_count, name)) ||
            (enabled && keyword_present(input->request->disabled_keywords, input->request->disabled_keyword_count, name)) ||
            (!enabled && unity_platform_capability_is_settings_dependent(capability) &&
             !keyword_present(input->request->disabled_keywords, input->request->disabled_keyword_count, name))) return false;
    }
    return true;
}

static bool bytes_equal(const uint8_t *a, size_t a_size, const uint8_t *b, size_t b_size) {
    return a_size == b_size && (!a_size || (a && b && !memcmp(a, b, a_size)));
}
static bool serialize_request(UnityCompilerBroker *broker, const UnityCompilerSnippetCompileRequest *request,
                              uint8_t **bytes, size_t *size, uint8_t digest[32]) {
    return unity_compiler_broker_serialize_compile_request(broker, request, bytes, size, digest);
}
static bool lease_matches(UnityCompilerBroker *broker, const UnityHlslMatrixDeclarationReceipt *receipt,
                          const UnityCompilerSnippetCompileRequest *request) {
    uint8_t *bytes = NULL, digest[32]; size_t size = 0;
    bool matches = serialize_request(broker, request, &bytes, &size, digest) &&
        bytes_equal(receipt->compile_bytes, receipt->compile_size, bytes, size) &&
        !memcmp(digest, receipt->summary.compile_request_digest, 32);
    free(bytes);
    return matches;
}
static bool bind_fields(UnityHlslMatrixDeclarationReceipt *receipt) {
    receipt->fields = calloc(receipt->reads.field_count, sizeof(*receipt->fields));
    if (!receipt->fields) return false;
    for (size_t index = 0; index < receipt->reads.field_count; ++index) {
        const HLSLCurrentMatrixField *current = &receipt->reads.fields[index];
        UnityHlslMatrixDeclarationField *out = &receipt->fields[index];
        out->current = *current;
        const UnityHlslCBufferBlock *block = NULL;
        for (size_t candidate = 0; candidate < receipt->declarations.block_count; ++candidate)
            if (!strcmp(receipt->declarations.blocks[candidate].name, current->block_name))
                block = &receipt->declarations.blocks[candidate];
        if (!block || block->byte_size != current->reflected_byte_size ||
            block->source_begin >= block->source_end || block->source_end > receipt->expanded.size) return false;
        const UnityHlslCBufferField *field = NULL;
        for (size_t candidate = block->first_field; candidate < block->first_field + block->field_count; ++candidate)
            if (!strcmp(receipt->declarations.fields[candidate].name, current->field_name))
                field = &receipt->declarations.fields[candidate];
        const SerializedProgramParameters *owner = current->field_authority == 1 ? &receipt->current : &receipt->common;
        const SerializedVariable *variable = &owner->constant_buffers[current->metadata_buffer_index].variables[current->metadata_field_index];
        if (!field || field->scalar != UNITY_HLSL_CBUFFER_FLOAT || !field->is_matrix ||
            field->rows != 4 || field->columns != 4 || field->byte_offset != current->field_byte_offset ||
            field->byte_size != current->field_byte_size || field->source_begin < block->source_begin ||
            field->source_end > block->source_end || field->source_begin >= field->source_end ||
            !unity_hlsl_cbuffer_field_matches_float_parameter(field, owner, variable)) return false;
        out->expanded_block = *block; out->expanded_field = *field;
    }
    for (size_t index = 0; index < receipt->reads.read_count; ++index) {
        const HLSLCurrentMatrixRead *read = &receipt->reads.reads[index];
        if (read->field_index >= receipt->reads.field_count || read->field_relative_byte_offset >= 64 ||
            read->physical_lane > 3 || read->logical_lane > 3 ||
            read->byte_offset != receipt->reads.fields[read->field_index].field_byte_offset + read->field_relative_byte_offset)
            return false;
    }
    return true;
}

UnityHlslMatrixDeclarationStatus unity_hlsl_matrix_declaration_capture(
    UnityCompilerBroker *broker, const UnityHlslMatrixDeclarationInput *input,
    UnityHlslMatrixDeclarationReceipt **output, UnityHlslMatrixDeclarationDiagnostic *diagnostic) {
    UnityHlslMatrixDeclarationDiagnostic local = {.status = UNITY_HLSL_MATRIX_DECLARATION_INVALID_ARGUMENT,
        .read_status = HLSL_CURRENT_MATRIX_INVALID_ARGUMENT, .expansion = UNITY_HLSL_EXPANSION_INVALID_ARGUMENT,
        .declaration = UNITY_HLSL_CBUFFER_INVALID_ARGUMENT};
    if (!diagnostic) diagnostic = &local; else *diagnostic = local;
    dxbc_compare_result_init(&diagnostic->comparison); unity_reflection_certificate_report_init(&diagnostic->reflection);
    if (!output || *output) return diagnostic->status;
    *output = NULL;
    if (!broker || !input || !request_shape(input->request) || !player_shape(input->player) || !input->profile ||
        !input->target || !input->target_size || input->target_size > TARGET_BYTE_LIMIT) return diagnostic->status;
    UnityHlslMatrixDeclarationReceipt *receipt = calloc(1, sizeof(*receipt));
    if (!receipt) return diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_ALLOCATION_FAILED;
    diagnostic->read_status = decode_reads(input, &receipt->reads);
    if (diagnostic->read_status != HLSL_CURRENT_MATRIX_OK) {
        diagnostic->status = diagnostic->read_status == HLSL_CURRENT_MATRIX_NOT_APPLICABLE
            ? UNITY_HLSL_MATRIX_DECLARATION_NOT_APPLICABLE
            : diagnostic->read_status == HLSL_CURRENT_MATRIX_INVALID_PROGRAM
            ? UNITY_HLSL_MATRIX_DECLARATION_TARGET_INVALID : UNITY_HLSL_MATRIX_DECLARATION_METADATA_REJECTED;
        goto fail;
    }
    if (!precision_authority(input)) { diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_PRECISION_REJECTED; goto fail; }
    receipt->profile = *input->profile;
    receipt->has_current = input->current_parameters != NULL; receipt->has_common = input->common_parameters != NULL;
    receipt->summary.legacy_half_contract = input->legacy_half_contract;
    if (!request_copy(receipt, input->request) || !player_copy(&receipt->player, input->player) ||
        (receipt->has_current && !serialized_program_parameters_copy(&receipt->current, input->current_parameters)) ||
        (receipt->has_common && !serialized_program_parameters_copy(&receipt->common, input->common_parameters)) ||
        unity_compile_profile_serialize(&receipt->profile, &receipt->profile_bytes, &receipt->profile_size) != UNITY_COMPILE_PROFILE_OK ||
        !(receipt->target = malloc(input->target_size))) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_ALLOCATION_FAILED; goto fail;
    }
    receipt->target_size = input->target_size; memcpy(receipt->target, input->target, input->target_size);
    common_sha256(receipt->target, receipt->target_size, receipt->summary.target_digest);
    memcpy(receipt->summary.profile_digest, receipt->profile.fingerprint, 32);
    common_sha256(receipt->request.snippet_source, strlen(receipt->request.snippet_source), receipt->summary.source_digest);
    UnityCompilerValidApisAuthority apis; UnityCompilerSessionCapabilities capabilities;
    if (!unity_compiler_broker_expected_valid_apis_authority(broker, &apis) ||
        apis.status == UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED ||
        apis.expected_valid_apis != receipt->profile.valid_apis ||
        !unity_compiler_broker_capture_session_capabilities(broker, &capabilities) ||
        !unity_compiler_session_capabilities_match_valid_apis(&capabilities, receipt->profile.valid_apis) ||
        !unity_compiler_broker_expected_valid_apis_authority(broker, &apis) ||
        apis.status != UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_COMPILER_UNAVAILABLE; goto fail;
    }
    if (!serialize_request(broker, &receipt->request, &receipt->compile_bytes, &receipt->compile_size,
                           receipt->summary.compile_request_digest)) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_COMPILER_UNAVAILABLE; goto fail;
    }
    UnityCompilerSnippetCompileRequest control = receipt->request; control.snippet_source = "";
    if (!serialize_request(broker, &control, &receipt->control_bytes, &receipt->control_size,
                           receipt->summary.compile_controls_digest)) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_COMPILER_UNAVAILABLE; goto fail;
    }
    UnityCompilerSnippetCompileRequest preprocessing = receipt->request; preprocessing.preprocess_only = true;
    uint8_t preprocess_digest[32];
    if (!serialize_request(broker, &preprocessing, &receipt->preprocess_bytes, &receipt->preprocess_size, preprocess_digest)) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_COMPILER_UNAVAILABLE; goto fail;
    }
    diagnostic->compile_attempted = true;
    if (!unity_compiler_broker_compile_contract_response(broker, &receipt->request, &receipt->compiled) ||
        !unity_compiler_response_status_is_clean_success(&receipt->compiled.status) || !receipt->compiled.has_request_identity) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_COMPILER_REJECTED; goto fail;
    }
    if (memcmp(receipt->compiled.request_digest, receipt->summary.compile_request_digest, 32) ||
        memcmp(receipt->compiled.controls_digest, receipt->summary.compile_controls_digest, 32) ||
        !lease_matches(broker, receipt, &receipt->request)) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_AUTHORITY_CHANGED; goto fail;
    }
    DXBCContainerView compiled;
    if (!dxbc_container_view_first(receipt->compiled.data, receipt->compiled.size, &compiled) ||
        dxbc_compare_exact(receipt->target, receipt->target_size, compiled.data, compiled.size,
                           &diagnostic->comparison) != DXBC_COMPARE_EQUAL) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_TARGET_MISMATCH; goto fail;
    }
    UnityReflectionCertificateStatus reflection = unity_reflection_certify_d3d11_bindings(&receipt->player,
        receipt->has_common ? &receipt->common : NULL, receipt->has_current ? &receipt->current : NULL,
        receipt->compiled.reflection_records, receipt->compiled.reflection_record_count, &diagnostic->reflection);
    if (reflection != UNITY_REFLECTION_CERTIFICATE_OK && reflection != UNITY_REFLECTION_CERTIFICATE_COMPATIBLE) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_REFLECTION_REJECTED; goto fail;
    }
    diagnostic->expansion_attempted = true;
    diagnostic->expansion = unity_hlsl_expansion_inspect_request(broker, &receipt->request, NULL, NULL,
        &receipt->expanded, &receipt->summary.expansion);
    if (diagnostic->expansion != UNITY_HLSL_EXPANSION_OK) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_EXPANSION_REJECTED; goto fail;
    }
    if (memcmp(receipt->summary.expansion.compile_request_digest, receipt->summary.compile_request_digest, 32) ||
        memcmp(receipt->summary.expansion.preprocess_request_digest, preprocess_digest, 32) ||
        !lease_matches(broker, receipt, &receipt->request)) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_AUTHORITY_CHANGED; goto fail;
    }
    const char *names[UNITY_HLSL_CBUFFER_COUNT_LIMIT]; size_t name_count = 0;
    for (size_t index = 0; index < receipt->reads.field_count; ++index) {
        const char *name = receipt->reads.fields[index].block_name; bool seen = false;
        for (size_t prior = 0; prior < name_count; ++prior) if (!strcmp(names[prior], name)) seen = true;
        if (!seen) { if (name_count == UNITY_HLSL_CBUFFER_COUNT_LIMIT) {
            diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_DECLARATION_REJECTED; goto fail;
        } names[name_count++] = name; }
    }
    diagnostic->declaration = unity_hlsl_cbuffer_inventory_build(receipt->expanded.data, receipt->expanded.size,
        names, name_count, (UnityHlslCBufferStoragePolicy){.legacy_half_is_float32 = true}, &receipt->declarations);
    if (diagnostic->declaration != UNITY_HLSL_CBUFFER_OK || !bind_fields(receipt)) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_DECLARATION_REJECTED; goto fail;
    }
    /* Last lease check covers inventory CPU time as well as compile/expansion. */
    if (!lease_matches(broker, receipt, &receipt->request)) {
        diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_AUTHORITY_CHANGED; goto fail;
    }
    receipt->summary.field_count = receipt->reads.field_count; receipt->summary.read_count = receipt->reads.read_count;
    *output = receipt; return diagnostic->status = UNITY_HLSL_MATRIX_DECLARATION_OK;
 fail:
    unity_hlsl_matrix_declaration_free(receipt); return diagnostic->status;
}

static bool field_equal(const HLSLCurrentMatrixField *a, const HLSLCurrentMatrixField *b) {
    return !strcmp(a->block_name, b->block_name) && !strcmp(a->field_name, b->field_name) &&
        a->binding_register == b->binding_register && a->declared_byte_size == b->declared_byte_size &&
        a->reflected_byte_size == b->reflected_byte_size && a->field_byte_offset == b->field_byte_offset &&
        a->field_byte_size == b->field_byte_size && a->logical_aggregate_id == b->logical_aggregate_id &&
        a->field_authority == b->field_authority && a->binding_authority == b->binding_authority &&
        a->full_shell_authorities == b->full_shell_authorities && a->metadata_buffer_index == b->metadata_buffer_index &&
        a->metadata_field_index == b->metadata_field_index && a->metadata_binding_index == b->metadata_binding_index &&
        a->row_major == b->row_major;
}
static bool reads_equal(const HLSLCurrentMatrixReads *a, const HLSLCurrentMatrixReads *b) {
    if (a->field_count != b->field_count || a->read_count != b->read_count) return false;
    for (size_t index = 0; index < a->field_count; ++index)
        if (!field_equal(&a->fields[index], &b->fields[index])) return false;
    for (size_t index = 0; index < a->read_count; ++index) {
        const HLSLCurrentMatrixRead *x = &a->reads[index], *y = &b->reads[index];
        if (x->field_index != y->field_index || x->instruction_index != y->instruction_index ||
            x->operand_index != y->operand_index || x->source_instruction_index != y->source_instruction_index ||
            x->destination_lanes != y->destination_lanes || x->logical_lane != y->logical_lane ||
            x->physical_lane != y->physical_lane || x->physical_row != y->physical_row ||
            x->byte_offset != y->byte_offset || x->field_relative_byte_offset != y->field_relative_byte_offset) return false;
    }
    return true;
}
bool unity_hlsl_matrix_declaration_replay(UnityCompilerBroker *broker,
    const UnityHlslMatrixDeclarationReceipt *receipt, const UnityHlslMatrixDeclarationInput *input) {
    if (!broker || !receipt || !input || !request_shape(input->request) || !player_shape(input->player) ||
        !input->target || input->legacy_half_contract != receipt->summary.legacy_half_contract ||
        !precision_authority(input) || !player_equal(&receipt->player, input->player) ||
        !bytes_equal(receipt->target, receipt->target_size, input->target, input->target_size) ||
        receipt->has_current != (input->current_parameters != NULL) || receipt->has_common != (input->common_parameters != NULL) ||
        (receipt->has_current && !parameters_equal(&receipt->current, input->current_parameters)) ||
        (receipt->has_common && !parameters_equal(&receipt->common, input->common_parameters))) return false;
    uint8_t *profile = NULL; size_t profile_size = 0;
    bool profile_matches = unity_compile_profile_serialize(input->profile, &profile, &profile_size) == UNITY_COMPILE_PROFILE_OK &&
        bytes_equal(receipt->profile_bytes, receipt->profile_size, profile, profile_size);
    free(profile);
    if (!profile_matches || !lease_matches(broker, receipt, input->request)) return false;
    UnityCompilerSnippetCompileRequest controls = *input->request; controls.snippet_source = "";
    uint8_t *control_bytes = NULL, control_digest[32]; size_t control_size = 0;
    bool controls_match = serialize_request(broker, &controls, &control_bytes, &control_size, control_digest) &&
        bytes_equal(receipt->control_bytes, receipt->control_size, control_bytes, control_size) &&
        !memcmp(control_digest, receipt->summary.compile_controls_digest, 32);
    free(control_bytes);
    if (!controls_match) return false;
    UnityCompilerValidApisAuthority apis;
    if (!unity_compiler_broker_expected_valid_apis_authority(broker, &apis) ||
        apis.status != UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED || apis.expected_valid_apis != receipt->profile.valid_apis ||
        apis.observed_valid_apis != receipt->profile.valid_apis) return false;
    UnityCompilerSnippetCompileRequest preprocessing = *input->request; preprocessing.preprocess_only = true;
    uint8_t *bytes = NULL, digest[32]; size_t size = 0;
    bool request_matches = serialize_request(broker, &preprocessing, &bytes, &size, digest) &&
        bytes_equal(receipt->preprocess_bytes, receipt->preprocess_size, bytes, size) &&
        !memcmp(digest, receipt->summary.expansion.preprocess_request_digest, 32);
    free(bytes);
    if (!request_matches) return false;
    const char *names[UNITY_HLSL_CBUFFER_COUNT_LIMIT]; size_t name_count = receipt->declarations.block_count;
    for (size_t index = 0; index < name_count; ++index) names[index] = receipt->declarations.blocks[index].name;
    if (!unity_hlsl_cbuffer_inventory_matches(&receipt->declarations, receipt->expanded.data, receipt->expanded.size,
        names, name_count, (UnityHlslCBufferStoragePolicy){.legacy_half_is_float32 = true})) return false;
    HLSLCurrentMatrixReads current = {0};
    bool matches = decode_reads(input, &current) == HLSL_CURRENT_MATRIX_OK && reads_equal(&receipt->reads, &current);
    hlsl_current_matrix_reads_dispose(&current);
    return matches && lease_matches(broker, receipt, input->request);
}
bool unity_hlsl_matrix_declaration_describe(const UnityHlslMatrixDeclarationReceipt *receipt,
                                            UnityHlslMatrixDeclarationSummary *summary) {
    if (!receipt || !summary) return false;
    *summary = receipt->summary; return true;
}
bool unity_hlsl_matrix_declaration_field(const UnityHlslMatrixDeclarationReceipt *receipt,
                                        size_t index, UnityHlslMatrixDeclarationField *field) {
    if (!receipt || !field || index >= receipt->reads.field_count) return false;
    *field = receipt->fields[index]; return true;
}
bool unity_hlsl_matrix_declaration_read(const UnityHlslMatrixDeclarationReceipt *receipt,
                                       size_t index, HLSLCurrentMatrixRead *read) {
    if (!receipt || !read || index >= receipt->reads.read_count) return false;
    *read = receipt->reads.reads[index]; return true;
}
