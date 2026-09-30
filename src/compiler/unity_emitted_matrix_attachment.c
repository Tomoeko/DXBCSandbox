// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_emitted_matrix_attachment.h"
#include "compiler/unity_generated_owned_request_internal.h"
#include "compiler/unity_shaderlab_mapping.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"

#include <stdlib.h>
#include <string.h>

enum { ATTACHMENT_REQUEST_LIMIT = 32, ATTACHMENT_TEXT_LIMIT = 4096 };

typedef struct {
    UnityEmittedMatrixAttachmentRequest observation;
    UnityHlslMatrixDeclarationReceipt *receipt;
} AttachedRequest;

struct UnityEmittedMatrixAttachment {
    ShaderLabEmittedMatrixUses *emitted;
    UnityCompileProfile profile;
    char *source_directory, *source_basename;
    UnityHlslMatrixLegacyHalfContract legacy_half_contract;
    UnityCompilerPreprocessResponse preprocessing;
    UnityGeneratedDomainReport domain;
    AttachedRequest requests[ATTACHMENT_REQUEST_LIMIT];
    size_t request_count;
    bool sealed;
};

typedef struct {
    const UnityEmittedMatrixAttachmentInput *input;
    UnityEmittedMatrixAttachment *owned;
    const UnityEmittedMatrixAttachment *replaying;
    UnityEmittedMatrixAttachmentDiagnostic *diagnostic;
    size_t body_request_counts[32];
} AttachmentCapture;

static bool text_valid(const char *text) {
    if (!text || !text[0]) return false;
    for (size_t index = 0; index <= ATTACHMENT_TEXT_LIMIT; ++index)
        if (!text[index]) return true;
    return false;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

void unity_emitted_matrix_attachment_free(UnityEmittedMatrixAttachment *owned) {
    if (!owned) return;
    for (size_t index = 0; index < owned->request_count; ++index)
        unity_hlsl_matrix_declaration_free(owned->requests[index].receipt);
    shaderlab_emitted_matrix_uses_free(owned->emitted);
    unity_compiler_preprocess_response_free(&owned->preprocessing);
    unity_generated_domain_report_free(&owned->domain);
    free(owned->source_directory);
    free(owned->source_basename);
    free(owned);
}

static bool profile_integrity_valid(const UnityCompileProfile *profile) {
    uint8_t fingerprint[32];
    return profile && unity_compile_profile_validate(profile) &&
        unity_compile_profile_fingerprint(profile, fingerprint) == UNITY_COMPILE_PROFILE_OK &&
        !memcmp(fingerprint, profile->fingerprint, 32);
}

static bool profiles_equal(const UnityCompileProfile *a, const UnityCompileProfile *b) {
    if (!profile_integrity_valid(a) || !profile_integrity_valid(b)) return false;
    uint8_t *left = NULL, *right = NULL;
    size_t left_size = 0, right_size = 0;
    bool equal = unity_compile_profile_serialize(a, &left, &left_size) == UNITY_COMPILE_PROFILE_OK &&
        unity_compile_profile_serialize(b, &right, &right_size) == UNITY_COMPILE_PROFILE_OK &&
        left_size == right_size && !memcmp(left, right, left_size);
    free(left);
    free(right);
    return equal;
}

static bool preprocess_request_matches(UnityCompilerBroker *broker,
    const UnityCompilerShaderPreprocessRequest *request, const uint8_t digest[32]) {
    uint8_t *transcript = NULL, observed[32];
    size_t size = 0;
    bool valid = unity_compiler_broker_serialize_preprocess_request(broker, request, &transcript, &size, observed);
    free(transcript);
    return valid && !memcmp(observed, digest, 32);
}

static bool coordinate_matches(const UnityEmittedMatrixAttachmentRequest *a,
    const UnityGeneratedOwnedRequest *b, size_t entry_index) {
    return a->entry_index == entry_index && a->stage_index == b->stage_index &&
        a->subprogram_index == b->subprogram_index && a->hardware_tier_group == b->hardware_tier_group &&
        a->generated_state_index == b->generated_state_index && a->aliased_state_index == b->aliased_state_index;
}

static bool observe_request(void *context, const UnityGeneratedOwnedRequest *actual) {
    AttachmentCapture *capture = context;
    UnityEmittedMatrixAttachment *owned = capture->owned;
    if (!actual || !actual->request || !actual->player || !actual->current || !actual->common ||
        owned->request_count >= ATTACHMENT_REQUEST_LIMIT) return false;
    size_t entry_index = SIZE_MAX;
    for (size_t index = 0; index < owned->emitted->entry_count; ++index) {
        const ShaderLabEmittedMatrixEntry *entry = &owned->emitted->entries[index].observation;
        if (entry->stage_index != actual->stage_index || entry->subprogram_index != actual->subprogram_index ||
            entry->hardware_tier_group != actual->hardware_tier_group ||
            entry->serialized_state != actual->aliased_state_index) continue;
        if (entry_index != SIZE_MAX) return false;
        entry_index = index;
    }
    if (entry_index == SIZE_MAX) return false;
    HLSLMatrixUseCapture *entry = &owned->emitted->entries[entry_index];
    if (!entry->finished || entry->target_size != actual->target_size ||
        memcmp(entry->target, actual->target, entry->target_size) ||
        !subprogram_metadata_variant_equal(&entry->player, actual->player) ||
        !serialized_program_parameters_equal(&entry->current, actual->current) ||
        !serialized_program_parameters_equal(&entry->common, actual->common)) return false;
    for (size_t index = 0; index < owned->request_count; ++index)
        if (coordinate_matches(&owned->requests[index].observation, actual, entry_index)) return false;

    UnityHlslMatrixDeclarationInput declaration_input = {
        .request = actual->request, .profile = capture->input->profile,
        .player = actual->player, .current_parameters = actual->current,
        .common_parameters = actual->common, .target = actual->target,
        .target_size = actual->target_size, .legacy_half_contract = capture->input->legacy_half_contract};
    const size_t request_index = owned->request_count;
    if (capture->replaying) {
        if (request_index >= capture->replaying->request_count) return false;
        const AttachedRequest *prior = &capture->replaying->requests[request_index];
        if (!coordinate_matches(&prior->observation, actual, entry_index)) return false;
        if (entry->reads.read_count) {
            if (!prior->receipt || prior->observation.declaration_status != UNITY_HLSL_MATRIX_DECLARATION_OK ||
                !unity_hlsl_matrix_declaration_replay(capture->input->broker,
                    prior->receipt, &declaration_input)) return false;
        } else if (prior->receipt || prior->observation.declaration_status !=
                UNITY_HLSL_MATRIX_DECLARATION_NOT_APPLICABLE) return false;
    }

    AttachedRequest *request = &owned->requests[owned->request_count++];
    request->observation = (UnityEmittedMatrixAttachmentRequest){
        .entry_index = entry_index, .stage_index = actual->stage_index,
        .subprogram_index = actual->subprogram_index, .hardware_tier_group = actual->hardware_tier_group,
        .generated_state_index = actual->generated_state_index,
        .aliased_state_index = actual->aliased_state_index};
    UnityHlslMatrixDeclarationStatus status = unity_hlsl_matrix_declaration_capture(
        capture->input->broker, &declaration_input, &request->receipt, NULL);
    capture->diagnostic->declaration = status;
    request->observation.declaration_status = status;
    if (!entry->reads.read_count) {
        if (status != UNITY_HLSL_MATRIX_DECLARATION_NOT_APPLICABLE || request->receipt ||
            entry->reads.field_count || entry->use_count) return false;
    } else {
        UnityHlslMatrixDeclarationSummary summary;
        if (status != UNITY_HLSL_MATRIX_DECLARATION_OK || !request->receipt ||
            !unity_hlsl_matrix_declaration_replay(capture->input->broker, request->receipt, &declaration_input) ||
            !unity_hlsl_matrix_declaration_describe(request->receipt, &summary) ||
            summary.field_count != entry->reads.field_count || summary.read_count != entry->reads.read_count) return false;
        for (size_t field_index = 0; field_index < summary.field_count; ++field_index) {
            UnityHlslMatrixDeclarationField field;
            if (!unity_hlsl_matrix_declaration_field(request->receipt, field_index, &field) ||
                !hlsl_matrix_uses_field_matches(entry, field_index, &field.current)) return false;
        }
        for (size_t read_index = 0; read_index < summary.read_count; ++read_index) {
            HLSLCurrentMatrixRead read;
            if (!unity_hlsl_matrix_declaration_read(request->receipt, read_index, &read) ||
                !hlsl_matrix_uses_read_matches(entry, read_index, &read)) return false;
        }
        request->observation.declaration = summary;
    }
    ++capture->body_request_counts[entry_index];
    return true;
}

static bool plan_in_scope(const ShaderLabVariantPlan *plan) {
    size_t request_count = 0;
    for (int index = 0; index < 5; ++index) {
        const ShaderLabPassStageVariantPlan *stage = &plan->stages[index];
        if (!stage->active) continue;
        const size_t states = stage->generated_domain_is_symbolic_boolean ? stage->state_count : stage->generated_state_count;
        const size_t tiers = stage->uses_specific_hardware_tiers ? 3 : 1;
        if (!states || states > ATTACHMENT_REQUEST_LIMIT / tiers ||
            request_count > ATTACHMENT_REQUEST_LIMIT - states * tiers) return false;
        request_count += states * tiers;
    }
    return request_count > 0;
}

static UnityEmittedMatrixAttachmentStatus capture_attachment(
    const UnityEmittedMatrixAttachmentInput *input, const UnityEmittedMatrixAttachment *replaying,
    UnityEmittedMatrixAttachment **output, UnityEmittedMatrixAttachmentDiagnostic *diagnostic) {
    UnityEmittedMatrixAttachmentDiagnostic local = {0};
    if (!diagnostic) diagnostic = &local;
    memset(diagnostic, 0, sizeof(*diagnostic));
    UnityEmittedMatrixAttachmentStatus status = UNITY_EMITTED_MATRIX_INVALID_ARGUMENT;
    if (!input || !input->source || !input->broker || !output || *output ||
        !text_valid(input->source_directory) || !text_valid(input->source_basename) ||
        (input->legacy_half_contract != UNITY_HLSL_MATRIX_LEGACY_HALF_UNAUTHORIZED &&
         input->legacy_half_contract != UNITY_HLSL_MATRIX_LEGACY_HALF_CAPTURED_FLOAT32)) goto result;
    if (!profile_integrity_valid(input->profile)) {
        status = UNITY_EMITTED_MATRIX_INVALID_PROFILE;
        goto result;
    }
    UnityEmittedMatrixAttachment *owned = calloc(1, sizeof(*owned));
    if (!owned) { status = UNITY_EMITTED_MATRIX_ALLOCATION_FAILED; goto result; }
    unity_compiler_preprocess_response_init(&owned->preprocessing);
    unity_generated_domain_report_init(&owned->domain);
    owned->profile = *input->profile;
    owned->legacy_half_contract = input->legacy_half_contract;
    owned->source_directory = copy_text(input->source_directory);
    owned->source_basename = copy_text(input->source_basename);
    if (!owned->source_directory || !owned->source_basename) {
        status = UNITY_EMITTED_MATRIX_ALLOCATION_FAILED;
        goto cleanup;
    }
    diagnostic->emission = shaderlab_emitted_matrix_uses_capture(input->source, &owned->emitted);
    if (diagnostic->emission != SHADERLAB_MATRIX_USES_OK) {
        status = diagnostic->emission == SHADERLAB_MATRIX_USES_NOT_APPLICABLE ?
            UNITY_EMITTED_MATRIX_NOT_APPLICABLE : UNITY_EMITTED_MATRIX_EMISSION_FAILED;
        goto cleanup;
    }
    if (!shaderlab_emitted_matrix_uses_replay(input->source, owned->emitted)) {
        status = UNITY_EMITTED_MATRIX_OWNERSHIP_MISMATCH;
        goto cleanup;
    }
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    const SerializedShader *shader = input->source->shader;
    const SerializedPass *pass = &shader->subshaders[0].passes[0];
    if (shaderlab_variant_plan_build(shader, pass, &plan, NULL) != SHADERLAB_VARIANT_PLAN_OK ||
        !plan_in_scope(&plan)) {
        shaderlab_variant_plan_free(&plan);
        status = UNITY_EMITTED_MATRIX_MAPPING_FAILED;
        goto cleanup;
    }
    const UnityCompilerShaderPreprocessRequest preprocess = {
        .source = owned->emitted->source.buf, .source_directory = owned->source_directory,
        .shader_name = shader->name, .caching_preprocessor = true,
        .build_platform = owned->profile.build_platform, .valid_apis = owned->profile.valid_apis};
    uint8_t *transcript = NULL, expected_request[32];
    size_t transcript_size = 0;
    bool received = unity_compiler_broker_serialize_preprocess_request(input->broker, &preprocess,
        &transcript, &transcript_size, expected_request);
    free(transcript);
    if (!received || !unity_compiler_broker_preprocess_contract_response(input->broker, &preprocess, &owned->preprocessing) ||
        !owned->preprocessing.has_request_identity ||
        memcmp(expected_request, owned->preprocessing.request_digest, 32) ||
        !unity_compiler_response_status_is_clean_success(&owned->preprocessing.status)) {
        shaderlab_variant_plan_free(&plan);
        status = UNITY_EMITTED_MATRIX_PREPROCESS_FAILED;
        goto cleanup;
    }
    const PreprocessedSnippet *snippet = unity_shaderlab_find_generated_snippet(
        shader, 0, &owned->preprocessing.result, NULL);
    if (!snippet) {
        shaderlab_variant_plan_free(&plan);
        status = UNITY_EMITTED_MATRIX_MAPPING_FAILED;
        goto cleanup;
    }
    const UnityGeneratedDomainCertificationInput domain = {
        .shader = shader, .pass = pass, .plan = &plan, .generated_snippet = snippet,
        .d3d11_archive = input->source->archive, .compile_profile = &owned->profile,
        .broker = input->broker, .source_directory = owned->source_directory,
        .source_basename = owned->source_basename, .pass_name = pass->name ? pass->name : "",
        .retain_compile_provenance = true};
    AttachmentCapture capture = {.input = input, .owned = owned,
        .replaying = replaying, .diagnostic = diagnostic};
    const UnityGeneratedOwnedRequestObserver observer = {.observe = observe_request, .context = &capture};
    diagnostic->domain = unity_generated_domain_certify_owned_requests(&domain, &observer, &owned->domain);
    shaderlab_variant_plan_free(&plan);
    if (diagnostic->domain != UNITY_GENERATED_DOMAIN_OK) {
        status = diagnostic->declaration != UNITY_HLSL_MATRIX_DECLARATION_OK &&
            diagnostic->declaration != UNITY_HLSL_MATRIX_DECLARATION_NOT_APPLICABLE ?
            UNITY_EMITTED_MATRIX_DECLARATION_FAILED : UNITY_EMITTED_MATRIX_DOMAIN_FAILED;
        goto cleanup;
    }
    if (owned->request_count != owned->domain.planned_compile_count ||
        owned->domain.matched_dxbc_count != owned->request_count ||
        (replaying && owned->request_count != replaying->request_count)) {
        status = UNITY_EMITTED_MATRIX_OWNERSHIP_MISMATCH;
        goto cleanup;
    }
    for (size_t index = 0; index < owned->emitted->entry_count; ++index)
        if (!capture.body_request_counts[index]) {
            status = UNITY_EMITTED_MATRIX_OWNERSHIP_MISMATCH;
            goto cleanup;
        }
    if (!shaderlab_emitted_matrix_uses_replay(input->source, owned->emitted) ||
        !profiles_equal(input->profile, &owned->profile) ||
        strcmp(input->source_directory, owned->source_directory) ||
        strcmp(input->source_basename, owned->source_basename) ||
        input->legacy_half_contract != owned->legacy_half_contract ||
        !preprocess_request_matches(input->broker, &preprocess, owned->preprocessing.request_digest)) {
        status = UNITY_EMITTED_MATRIX_OWNERSHIP_MISMATCH;
        goto cleanup;
    }
    owned->sealed = true;
    *output = owned;
    status = UNITY_EMITTED_MATRIX_OK;
    goto result;
cleanup:
    unity_emitted_matrix_attachment_free(owned);
result:
    diagnostic->status = status;
    return status;
}

UnityEmittedMatrixAttachmentStatus unity_emitted_matrix_attachment_capture(
    const UnityEmittedMatrixAttachmentInput *input, UnityEmittedMatrixAttachment **output,
    UnityEmittedMatrixAttachmentDiagnostic *diagnostic) {
    return capture_attachment(input, NULL, output, diagnostic);
}

bool unity_emitted_matrix_attachment_replay(
    const UnityEmittedMatrixAttachmentInput *input, const UnityEmittedMatrixAttachment *owned) {
    if (!owned || !owned->sealed || !input || !input->profile ||
        !text_valid(input->source_directory) || !text_valid(input->source_basename) ||
        strcmp(input->source_directory, owned->source_directory) ||
        strcmp(input->source_basename, owned->source_basename) ||
        input->legacy_half_contract != owned->legacy_half_contract ||
        !shaderlab_emitted_matrix_uses_replay(input->source, owned->emitted)) return false;
    if (!profiles_equal(input->profile, &owned->profile)) return false;
    UnityEmittedMatrixAttachment *current = NULL;
    bool valid = capture_attachment(input, owned, &current, NULL) == UNITY_EMITTED_MATRIX_OK;
    if (valid) valid = !memcmp(current->preprocessing.request_digest, owned->preprocessing.request_digest, 32) &&
        !memcmp(current->preprocessing.controls_digest, owned->preprocessing.controls_digest, 32) &&
        owned->domain.status == UNITY_GENERATED_DOMAIN_OK &&
        owned->domain.matched_dxbc_count == owned->request_count &&
        owned->domain.planned_compile_count == owned->request_count;
    unity_emitted_matrix_attachment_free(current);
    return valid;
}

bool unity_emitted_matrix_attachment_describe(const UnityEmittedMatrixAttachment *owned,
    UnityEmittedMatrixAttachmentSummary *summary) {
    if (!owned || !owned->sealed || !summary) return false;
    UnityEmittedMatrixAttachmentSummary result = {.request_count = owned->request_count,
        .normal_domain_status = owned->domain.status, .normal_matched_compile_count = owned->domain.matched_dxbc_count};
    if (!shaderlab_emitted_matrix_uses_describe(owned->emitted, &result.emitted)) return false;
    for (size_t index = 0; index < owned->request_count; ++index) {
        if (owned->requests[index].receipt) {
            ++result.declaration_count;
            UnityHlslMatrixDeclarationSummary declaration;
            if (!unity_hlsl_matrix_declaration_describe(owned->requests[index].receipt, &declaration)) return false;
            result.attached_read_count += declaration.read_count;
        } else {
            ++result.absent_count;
        }
    }
    memcpy(result.preprocess_request_digest, owned->preprocessing.request_digest, 32);
    memcpy(result.preprocess_controls_digest, owned->preprocessing.controls_digest, 32);
    *summary = result;
    return true;
}

bool unity_emitted_matrix_attachment_request(const UnityEmittedMatrixAttachment *owned,
    size_t index, UnityEmittedMatrixAttachmentRequest *request) {
    if (!owned || !owned->sealed || !request || index >= owned->request_count) return false;
    *request = owned->requests[index].observation;
    if (owned->requests[index].receipt && !unity_hlsl_matrix_declaration_describe(
            owned->requests[index].receipt, &request->declaration)) return false;
    return true;
}

bool unity_emitted_matrix_attachment_field(const UnityEmittedMatrixAttachment *owned,
    size_t request, size_t index, UnityHlslMatrixDeclarationField *field) {
    return owned && owned->sealed && request < owned->request_count &&
        unity_hlsl_matrix_declaration_field(owned->requests[request].receipt, index, field);
}

bool unity_emitted_matrix_attachment_read(const UnityEmittedMatrixAttachment *owned,
    size_t request, size_t index, HLSLCurrentMatrixRead *read) {
    return owned && owned->sealed && request < owned->request_count &&
        unity_hlsl_matrix_declaration_read(owned->requests[request].receipt, index, read);
}

const ShaderLabEmittedMatrixUses *unity_emitted_matrix_attachment_emission(const UnityEmittedMatrixAttachment *owned) {
    return owned && owned->sealed ? owned->emitted : NULL;
}
