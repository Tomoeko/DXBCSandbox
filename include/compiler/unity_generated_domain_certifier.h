// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_GENERATED_DOMAIN_CERTIFIER_H
#define UNITY_GENERATED_DOMAIN_CERTIFIER_H

#include "compiler/unity_compile_authority.h"
#include "compiler/unity_compile_profile.h"
#include "compiler/unity_compiler_broker.h"
#include "compiler/unity_reflection_certificate.h"
#include "dxbc/dxbc_compare.h"
#include "io/shader_blob_archive.h"
#include "translation/shaderlab_variant_plan.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Source-free certification has two independent boundaries:
 *
 *   1. the generated pragma domain must be reproduced exactly by Unity's
 *      ordered preprocess contract; and
 *   2. every generated state/tier compile must equal the complete stripped
 *      Release DXBC container selected by the proven alias map.
 *
 * Neither equivalence boundary consults original ShaderLab/HLSL source.
 * Original source is optional, narrowly scoped diagnostic evidence: an
 * actionable candidate diagnostic can be accepted only when the uniquely
 * mapped original snippet produces the same complete target DXBC and exact
 * normalized diagnostic multiset under identical controls. GLCore is not
 * certified by this API: stripped DXBC does not retain the precision and
 * linked-stage authority needed to identify Unity's GLSL output.
 */
typedef enum {
    UNITY_GENERATED_DOMAIN_OK = 0,
    UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT,
    UNITY_GENERATED_DOMAIN_INVALID_PROFILE,
    UNITY_GENERATED_DOMAIN_PLAN_AUTHORITY_MISMATCH,
    UNITY_GENERATED_DOMAIN_UNSUPPORTED_STAGE,
    UNITY_GENERATED_DOMAIN_INVALID_CONTRACT,
    UNITY_GENERATED_DOMAIN_MISSING_PROGRAM_CONTRACT,
    UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE,
    UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS,
    UNITY_GENERATED_DOMAIN_KEYWORD_SCOPE_MISMATCH,
    UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MALFORMED,
    UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MISSING,
    UNITY_GENERATED_DOMAIN_CONTRACT_ROW_DUPLICATE,
    UNITY_GENERATED_DOMAIN_CONTRACT_ROW_EXTRA,
    UNITY_GENERATED_DOMAIN_CONTRACT_ORDER_MISMATCH,
    UNITY_GENERATED_DOMAIN_CARDINALITY_MISMATCH,
    UNITY_GENERATED_DOMAIN_ALIAS_OUT_OF_RANGE,
    UNITY_GENERATED_DOMAIN_TIER_DOMAIN_MISMATCH,
    UNITY_GENERATED_DOMAIN_SUBPROGRAM_OUT_OF_RANGE,
    UNITY_GENERATED_DOMAIN_SUBPROGRAM_NOT_D3D11,
    UNITY_GENERATED_DOMAIN_ARCHIVE_BLOB_MISSING,
    UNITY_GENERATED_DOMAIN_PLAYER_BLOB_INVALID,
    UNITY_GENERATED_DOMAIN_PLAYER_METADATA_MISMATCH,
    UNITY_GENERATED_DOMAIN_REFERENCE_DXBC_INVALID,
    UNITY_GENERATED_DOMAIN_COMPILE_AUTHORITY_FAILED,
    UNITY_GENERATED_DOMAIN_REQUIREMENTS_MISMATCH,
    UNITY_GENERATED_DOMAIN_COMPILER_CACHE_ONLY_MISS,
    UNITY_GENERATED_DOMAIN_COMPILER_TRANSPORT_FAILED,
    UNITY_GENERATED_DOMAIN_COMPILER_REJECTED,
    UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC,
    UNITY_GENERATED_DOMAIN_DIAGNOSTIC_ATTESTATION_FAILED,
    UNITY_GENERATED_DOMAIN_COMPILED_DXBC_INVALID,
    UNITY_GENERATED_DOMAIN_DXBC_MISMATCH,
    UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID,
    UNITY_GENERATED_DOMAIN_REFLECTION_MISMATCH,
    UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY,
    UNITY_GENERATED_DOMAIN_INCLUDE_AUTHORITY_UNAVAILABLE
} UnityGeneratedDomainStatus;

typedef enum {
    UNITY_GENERATED_DIAGNOSTIC_PARITY_NOT_APPLICABLE = 0,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_ATTESTED,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_SOURCE_UNAVAILABLE,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_TRANSPORT_FAILED,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_REJECTED,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_DXBC_INVALID,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_DXBC_MISMATCH,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_REFLECTION_MISMATCH,
    UNITY_GENERATED_DIAGNOSTIC_PARITY_DIAGNOSTICS_MISMATCH
} UnityGeneratedDiagnosticParityStatus;

typedef enum {
    UNITY_GENERATED_DOMAIN_FAMILY_NONE = -1,
    UNITY_GENERATED_DOMAIN_FAMILY_USER_GLOBAL = 0,
    UNITY_GENERATED_DOMAIN_FAMILY_USER_LOCAL = 1,
    UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN = 2
} UnityGeneratedDomainKeywordFamily;

typedef enum {
    UNITY_GENERATED_GLSL_UNAVAILABLE_NO_PRECISION_AUTHORITY = 0,
    UNITY_GENERATED_GLSL_AUTHORITY_AVAILABLE = 1
} UnityGeneratedGLSLStatus;

typedef struct {
    UnityGeneratedDomainStatus status;
    int stage_index;
    int32_t compiler_program;
    int hardware_tier_group;
    size_t generated_state_index;
    size_t aliased_state_index;
    int subprogram_index;
    UnityGeneratedDomainKeywordFamily keyword_family;
    size_t contract_row_index;
    UnityCompileAuthorityStatus compile_authority_status;
    UnityCompilerResponseStatus compiler_response;
    DXBCCompareResult dxbc_compare;
    UnityReflectionCertificateReport reflection_certificate;
} UnityGeneratedDomainDiagnostic;

/* Hashes describe the attempted request and extracted container bytes. None
 * of the presence flags imply a successful compile or equality certificate. */
typedef struct {
    bool recorded;
    bool response_received;
    bool has_request_identity;
    bool has_output_digest;
    uint8_t request_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t controls_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t source_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t target_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t output_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
} UnityGeneratedCompileProvenance;

typedef struct {
    int stage_index;
    int hardware_tier_group;
    size_t generated_state_index;
    size_t aliased_state_index;
    int subprogram_index;
    UnityCompilerResponseStatus response;
    UnityGeneratedDiagnosticParityStatus diagnostic_parity_status;
    bool original_response_present;
    UnityCompilerResponseStatus original_response;
    DXBCCompareResult original_dxbc_compare;
    UnityGeneratedCompileProvenance provenance;
    UnityGeneratedCompileProvenance original_provenance;
} UnityGeneratedDomainCompilerResponseRecord;

typedef struct {
    UnityGeneratedDomainStatus status;
    UnityGeneratedGLSLStatus glsl_status;
    size_t active_stage_count;
    size_t attested_stage_count;
    size_t generated_state_count;
    size_t planned_compile_count;
    size_t compile_attempt_count;
    size_t clean_compile_count;
    size_t matched_dxbc_count;
    size_t runtime_binding_attested_compile_count;
    size_t runtime_binding_compatible_compile_count;
    size_t compiler_diagnostic_count;
    size_t diagnostic_attestation_compile_count;
    size_t diagnostic_attested_compile_count;
    size_t diagnostic_attested_actionable_count;
    UnityGeneratedDomainCompilerResponseRecord* compiler_responses;
    size_t compiler_response_count;
    UnityGeneratedDomainDiagnostic diagnostic;
} UnityGeneratedDomainReport;

void unity_generated_domain_report_init(UnityGeneratedDomainReport* report);
void unity_generated_domain_report_free(UnityGeneratedDomainReport* report);

/*
 * Pure, offline proof of the generated pragma/contract boundary.  The report
 * is deterministic and stops at the first serialized stage/row failure.
 * `report` must have been initialized (or previously freed).
 */
UnityGeneratedDomainStatus unity_generated_domain_attest_contract(
    const SerializedShader* shader,
    const SerializedPass* pass,
    const ShaderLabVariantPlan* plan,
    const SnippetCompileContract* contract,
    UnityGeneratedDomainReport* report);

/* Injectable only to make the exact request/response boundary unit-testable.
 * Production callers leave compile_callback NULL and provide broker. */
typedef bool (*UnityGeneratedDomainCompileCallback)(
    void* context,
    const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* response);

typedef struct {
    const SerializedShader* shader;
    const SerializedPass* pass;
    const ShaderLabVariantPlan* plan;
    const PreprocessedSnippet* generated_snippet;
    /* Optional and strictly source-backed. Actionable generated diagnostics
     * remain fatal unless this uniquely mapped original snippet recompiles to
     * the same complete reference DXBC with the same normalized diagnostic
     * multiset. */
    const PreprocessedSnippet* original_snippet;
    const ShaderBlobArchive* d3d11_archive;
    const UnityCompileProfile* compile_profile;
    UnityCompilerBroker* broker;

    const char* source_directory;
    const char* source_basename;
    const char* pass_name;
    const char* original_source_directory;
    const char* original_source_basename;

    UnityGeneratedDomainCompileCallback compile_callback;
    void* compile_context;
    /* Retain every attempted request, including clean, unavailable and failed
     * responses. Without this flag the historical diagnostic-only ledger is
     * preserved. Original diagnostic-parity requests share their primary row. */
    bool retain_compile_provenance;
} UnityGeneratedDomainCertificationInput;

/*
 * Attests the complete generated domain, then byte-compares D3D11 compiles.
 * Materialized plans compile every stage/state/tier.  A proven symbolic
 * Boolean plan compiles one exact request per distinct serialized winner/tier;
 * its complete 2^N mapping and first-winner ties are established algebraically
 * by the plan's score predicates rather than by enumerating aliases.  Every
 * Every compiler diagnostic is retained. Actionable diagnostics fail closed
 * unless the optional source-backed parity proof succeeds; Unity's type-zero
 * informational records remain non-fatal.
 */
UnityGeneratedDomainStatus unity_generated_domain_certify_d3d11(
    const UnityGeneratedDomainCertificationInput* input,
    UnityGeneratedDomainReport* report);

/* Diagnostic normalization intentionally retains only fields whose meaning
 * is stable across generated/original source locations: actionable class,
 * fields[0], fields[1], and the exact message. File, record text, and
 * fields[2] (source line) are retained in reports but excluded here. The
 * comparison is an exact unordered multiset, including multiplicity. */
bool unity_generated_domain_diagnostics_match_normalized(
    const UnityCompilerResponseStatus* generated,
    const UnityCompilerResponseStatus* original);

const char* unity_generated_diagnostic_parity_status_name(
    UnityGeneratedDiagnosticParityStatus status);

const char* unity_generated_domain_status_name(
    UnityGeneratedDomainStatus status);
const char* unity_generated_glsl_status_name(UnityGeneratedGLSLStatus status);

#endif /* UNITY_GENERATED_DOMAIN_CERTIFIER_H */
