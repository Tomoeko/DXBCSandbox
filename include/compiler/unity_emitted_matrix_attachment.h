// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_EMITTED_MATRIX_ATTACHMENT_H
#define UNITY_EMITTED_MATRIX_ATTACHMENT_H

#include "compiler/unity_generated_domain_certifier.h"
#include "compiler/unity_hlsl_matrix_declaration.h"
#include "translation/shaderlab_emitted_matrix_uses.h"

typedef struct UnityEmittedMatrixAttachment UnityEmittedMatrixAttachment;

typedef struct {
    const ShaderLabSourceQualityRequest *source;
    const UnityCompileProfile *profile;
    UnityCompilerBroker *broker;
    const char *source_directory, *source_basename;
    UnityHlslMatrixLegacyHalfContract legacy_half_contract;
} UnityEmittedMatrixAttachmentInput;

typedef enum {
    UNITY_EMITTED_MATRIX_OK = 0,
    UNITY_EMITTED_MATRIX_NOT_APPLICABLE,
    UNITY_EMITTED_MATRIX_INVALID_ARGUMENT,
    UNITY_EMITTED_MATRIX_INVALID_PROFILE,
    UNITY_EMITTED_MATRIX_EMISSION_FAILED,
    UNITY_EMITTED_MATRIX_PREPROCESS_FAILED,
    UNITY_EMITTED_MATRIX_MAPPING_FAILED,
    UNITY_EMITTED_MATRIX_DOMAIN_FAILED,
    UNITY_EMITTED_MATRIX_OWNERSHIP_MISMATCH,
    UNITY_EMITTED_MATRIX_DECLARATION_FAILED,
    UNITY_EMITTED_MATRIX_ALLOCATION_FAILED
} UnityEmittedMatrixAttachmentStatus;

typedef struct {
    UnityEmittedMatrixAttachmentStatus status;
    ShaderLabMatrixUsesStatus emission;
    UnityGeneratedDomainStatus domain;
    UnityHlslMatrixDeclarationStatus declaration;
} UnityEmittedMatrixAttachmentDiagnostic;

typedef struct {
    ShaderLabEmittedMatrixSummary emitted;
    size_t request_count, declaration_count, absent_count, attached_read_count;
    uint8_t preprocess_request_digest[32], preprocess_controls_digest[32];
    UnityGeneratedDomainStatus normal_domain_status;
    size_t normal_matched_compile_count;
} UnityEmittedMatrixAttachmentSummary;

typedef struct {
    size_t entry_index, generated_state_index, aliased_state_index;
    int stage_index, subprogram_index, hardware_tier_group;
    UnityHlslMatrixDeclarationStatus declaration_status;
    UnityHlslMatrixDeclarationSummary declaration;
    /* Additive historical observation of the emitted entry and its complete
     * required declaration fragments. It does not replace base stage/whole
     * results or close any include/runtime/asset dependency scope. */
    bool has_scoped_source_quality;
    uint32_t unresolved_coverage_obligations;
    HLSLSourceQualityResult scoped_source_quality;
} UnityEmittedMatrixAttachmentRequest;

/* Observation-only owned factory. It internally emits the complete ordinary
 * one-pass V/F source, broker-preprocesses that exact source, maps its actual
 * complete snippet, and observes the existing normal generated-domain loop.
 * It accepts neither caller source/AST/maps nor injected compiler services.
 * At most 32 actual loop requests are admitted; every retained emitted body
 * must be reached. Matrix-less stages remain explicitly NOT_APPLICABLE.
 * Output must initially be NULL and remains NULL on every failure. All input
 * storage is borrowed, immutable for this call; the result owns copied emitted
 * source/metadata/ASTs, preprocessing, compiler receipts and coordinate facts.
 * A separate fresh analyzer may observe the bounded emitted entry plus its
 * required complete declaration fragments. That result does not supply full
 * include-unit, wrapper or dependency closure; base stage and whole
 * classifications remain their original observations. */
UnityEmittedMatrixAttachmentStatus unity_emitted_matrix_attachment_capture(
    const UnityEmittedMatrixAttachmentInput *input, UnityEmittedMatrixAttachment **output,
    UnityEmittedMatrixAttachmentDiagnostic *diagnostic);

/* Reconstructs the current owned source/preprocess/normal request chain and
 * replays each retained opaque declaration receipt against its actual current
 * request and active broker lease. May issue ordinary compiler requests. */
bool unity_emitted_matrix_attachment_replay(
    const UnityEmittedMatrixAttachmentInput *input, const UnityEmittedMatrixAttachment *owned);
bool unity_emitted_matrix_attachment_describe(const UnityEmittedMatrixAttachment *owned,
    UnityEmittedMatrixAttachmentSummary *summary);
bool unity_emitted_matrix_attachment_request(const UnityEmittedMatrixAttachment *owned,
    size_t index, UnityEmittedMatrixAttachmentRequest *request);
bool unity_emitted_matrix_attachment_field(const UnityEmittedMatrixAttachment *owned,
    size_t request, size_t index, UnityHlslMatrixDeclarationField *field);
bool unity_emitted_matrix_attachment_read(const UnityEmittedMatrixAttachment *owned,
    size_t request, size_t index, HLSLCurrentMatrixRead *read);
/* Immutable owned emission observations borrowed until attachment_free(). */
const ShaderLabEmittedMatrixUses *unity_emitted_matrix_attachment_emission(
    const UnityEmittedMatrixAttachment *owned);
void unity_emitted_matrix_attachment_free(UnityEmittedMatrixAttachment *owned);

#endif
