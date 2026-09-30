// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_HLSL_MATRIX_DECLARATION_H
#define UNITY_HLSL_MATRIX_DECLARATION_H

#include "compiler/unity_compile_profile.h"
#include "compiler/unity_hlsl_cbuffer_inventory.h"
#include "compiler/unity_reflection_certificate.h"
#include "dxbc/dxbc_compare.h"
#include "translation/hlsl_current_matrix_reads.h"

typedef struct UnityHlslMatrixDeclarationReceipt UnityHlslMatrixDeclarationReceipt;

typedef enum {
    UNITY_HLSL_MATRIX_DECLARATION_OK = 0,
    UNITY_HLSL_MATRIX_DECLARATION_NOT_APPLICABLE,
    UNITY_HLSL_MATRIX_DECLARATION_INVALID_ARGUMENT,
    UNITY_HLSL_MATRIX_DECLARATION_TARGET_INVALID,
    UNITY_HLSL_MATRIX_DECLARATION_METADATA_REJECTED,
    UNITY_HLSL_MATRIX_DECLARATION_PRECISION_REJECTED,
    UNITY_HLSL_MATRIX_DECLARATION_COMPILER_UNAVAILABLE,
    UNITY_HLSL_MATRIX_DECLARATION_COMPILER_REJECTED,
    UNITY_HLSL_MATRIX_DECLARATION_TARGET_MISMATCH,
    UNITY_HLSL_MATRIX_DECLARATION_REFLECTION_REJECTED,
    UNITY_HLSL_MATRIX_DECLARATION_EXPANSION_REJECTED,
    UNITY_HLSL_MATRIX_DECLARATION_DECLARATION_REJECTED,
    UNITY_HLSL_MATRIX_DECLARATION_AUTHORITY_CHANGED,
    UNITY_HLSL_MATRIX_DECLARATION_ALLOCATION_FAILED
} UnityHlslMatrixDeclarationStatus;

typedef enum {
    UNITY_HLSL_MATRIX_LEGACY_HALF_UNAUTHORIZED = 0,
    /* Explicit externally captured selected D3D11 storage contract. The live
     * producer additionally requires a fingerprint-valid profile with unified precision
     * false, all 33 matching D3D11 request capability states and actual block/reflection
     * layout. Profile integrity does not authenticate capture provenance. */
    UNITY_HLSL_MATRIX_LEGACY_HALF_CAPTURED_FLOAT32
} UnityHlslMatrixLegacyHalfContract;

typedef struct {
    const UnityCompilerSnippetCompileRequest *request;
    const UnityCompileProfile *profile;
    const PlayerSubProgramMetadata *player;
    const SerializedProgramParameters *current_parameters;
    const SerializedProgramParameters *common_parameters;
    /* One complete original DXBC container, including all chunks/checksum. */
    const uint8_t *target;
    size_t target_size;
    UnityHlslMatrixLegacyHalfContract legacy_half_contract;
} UnityHlslMatrixDeclarationInput;

typedef struct {
    UnityHlslMatrixDeclarationStatus status;
    HLSLCurrentMatrixStatus read_status;
    bool compile_attempted, expansion_attempted;
    DXBCCompareResult comparison;
    UnityReflectionCertificateReport reflection;
    UnityHlslExpansionStatus expansion;
    UnityHlslCBufferStatus declaration;
} UnityHlslMatrixDeclarationDiagnostic;

typedef struct {
    HLSLCurrentMatrixField current;
    UnityHlslCBufferBlock expanded_block;
    UnityHlslCBufferField expanded_field;
} UnityHlslMatrixDeclarationField;

typedef struct {
    size_t field_count, read_count;
    uint8_t target_digest[32], profile_digest[32], source_digest[32];
    uint8_t compile_request_digest[32], compile_controls_digest[32];
    UnityHlslExpansionEvidence expansion;
    UnityHlslMatrixLegacyHalfContract legacy_half_contract;
} UnityHlslMatrixDeclarationSummary;

/* Live opaque producer, not a caller-filled digest/Boolean certificate. Owns
 * target, current/common/player/profile/request copies, compiler response,
 * expansion and typed lane/declaration facts. Internally decodes original
 * bytes, repeats the unchanged request, compares full containers, certifies
 * reflection and performs NULL-probe same-request expansion with live authority.
 * The output pointer must initially be NULL. Failure/NOT_APPLICABLE leaves it
 * NULL; a nonempty output rejects without replacing it. Absence grants no source coverage.
 * No injected services or public successful-receipt constructor is accepted.
 * This observation does not authenticate caller source maps/ASTs, recover an
 * original declaration, close include semantics or promote source quality. */
UnityHlslMatrixDeclarationStatus unity_hlsl_matrix_declaration_capture(
    UnityCompilerBroker *broker, const UnityHlslMatrixDeclarationInput *input,
    UnityHlslMatrixDeclarationReceipt **output,
    UnityHlslMatrixDeclarationDiagnostic *diagnostic);

/* Rechecks exact current models and canonical request bytes against the active
 * include/toolchain lease, and replays typed read/declaration facts from owned
 * bytes. No source map or mutable public summary can authorize replay. It does
 * not repeat execution or issue an additional correctness certificate. */
bool unity_hlsl_matrix_declaration_replay(UnityCompilerBroker *broker,
    const UnityHlslMatrixDeclarationReceipt *receipt,
    const UnityHlslMatrixDeclarationInput *current_input);

bool unity_hlsl_matrix_declaration_describe(const UnityHlslMatrixDeclarationReceipt *receipt,
    UnityHlslMatrixDeclarationSummary *summary);
bool unity_hlsl_matrix_declaration_field(const UnityHlslMatrixDeclarationReceipt *receipt,
    size_t index, UnityHlslMatrixDeclarationField *field);
bool unity_hlsl_matrix_declaration_read(const UnityHlslMatrixDeclarationReceipt *receipt,
    size_t index, HLSLCurrentMatrixRead *read);
void unity_hlsl_matrix_declaration_free(UnityHlslMatrixDeclarationReceipt *receipt);

#endif
