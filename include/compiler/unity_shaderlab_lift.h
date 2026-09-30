// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADERLAB_LIFT_H
#define UNITY_SHADERLAB_LIFT_H

#include "compiler/unity_generated_domain_certifier.h"
#include "compiler/unity_uv_helper.h"
#include "translation/hlsl_lift_transaction.h"
#include "translation/shaderlab_emitter.h"
#include "translation/shaderlab_source_quality.h"

typedef struct {
    const SerializedShader *shader;
    const ShaderBlobArchive *archive;
    const UnityCompileProfile *profile;
    UnityCompilerBroker *broker;
    /* One immutable source identity is used for both candidate modes. */
    const char *source_path;
    const char *source_directory;
    const char *source_basename;
    /* Optional decoded object borrowed only for this call. Must pair exactly
     * with shader. Legacy/direct callers without it retain the schema gap. */
    const ShaderObject *source_object;
} UnityShaderLabLiftInput;

/* Production callers can use the default services and provide a broker.
 * Optional compiler callbacks exist for deterministic boundary tests. They
 * must execute the exact supplied request and transfer owned responses as the
 * broker APIs do. Responses must include their actual canonical request
 * identities. Compiler services
 * retain their own bounded I/O timeouts; acceptance checks run afterwards. */
typedef struct {
    bool (*monotonic_ms)(void *context, uint64_t *milliseconds);
    bool (*cancelled)(void *context);
    bool (*preprocess)(void *context, const UnityCompilerShaderPreprocessRequest *request,
                       UnityCompilerPreprocessResponse *response);
    UnityGeneratedDomainCompileCallback compile;
    bool (*toolchain)(void *context, UnityCompilerToolchainProvenance *provenance);
    void *context;
    /* Optional canonical serializer for helper expansion gates. The production
     * default uses the broker and the same live include/toolchain authority. */
    bool (*request_digest)(void *context, const UnityCompilerSnippetCompileRequest *request,
                           uint8_t digest[32]);
} UnityShaderLabLiftServices;

typedef struct {
    size_t domain_compile_index;
    UnityUvHelperStatus status;
    UnityUvHelperEvidence evidence;
    UnityCompilerBinaryResponse preprocessing;
    bool compile_received;
    bool compile_identity_matched;
} UnityShaderLabLiftHelperCheck;

void unity_shaderlab_lift_default_services(UnityShaderLabLiftServices *services);

typedef struct {
    int subshader_index;
    int pass_index;
    int serialized_pass_index;
    bool plan_attempted;
    ShaderLabVariantPlanStatus plan_status;
    ShaderLabVariantPlanDiagnostic plan_diagnostic;
    int snippet_index;
    bool certification_attempted;
    UnityGeneratedDomainReport domain;
    UnityShaderLabLiftHelperCheck *helper_checks;
    size_t helper_check_count;
} UnityShaderLabLiftPassReport;

typedef enum {
    UNITY_SHADERLAB_INVENTORY_NOT_RUN = 0,
    UNITY_SHADERLAB_INVENTORY_UNSUPPORTED_MODE,
    UNITY_SHADERLAB_INVENTORY_OBSERVED,
    UNITY_SHADERLAB_INVENTORY_SCOPE_UNAVAILABLE,
    UNITY_SHADERLAB_INVENTORY_SCHEMA_FAILED,
    UNITY_SHADERLAB_INVENTORY_EMISSION_FAILED,
    UNITY_SHADERLAB_INVENTORY_SOURCE_MISMATCH,
    UNITY_SHADERLAB_INVENTORY_ALLOCATION_FAILED,
    UNITY_SHADERLAB_INVENTORY_INVALID_ARGUMENT
} UnityShaderLabLiftInventoryStatus;

/* Historical producer snapshot, not a replayable receipt or certificate.
 * It owns no model pointers, receipts or duplicate stage map. The modeled-input
 * digest describes the inputs observed during production; it does not validate
 * a later model. Quality remains independent of compilation and publication.
 * Result views must stay immutable until result_free(). Public structs are not
 * authenticated: callers can forge a summary, so consumers must trust its
 * producer. Retain the core ShaderLabSourceQualityInventory and original inputs
 * when later typed receipt replay is required. */
typedef struct {
    UnityShaderLabLiftInventoryStatus status;
    ShaderLabSourceQualityDiagnostic diagnostic;
    ShaderLabSourceQualityResult quality;
    size_t receipt_count;
    size_t source_size;
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t modeled_input_digest[COMMON_SHA256_DIGEST_SIZE];
    bool has_structural_authority;
    ShaderLabStructuralDiagnostic structure;
} UnityShaderLabLiftSourceInventory;

typedef struct {
    bool attempted;
    bool high_level;
    bool unity_uv_helpers;
    HLSLLiftStatus status;
    bool emission_attempted;
    ShaderLabCandidateDiagnostic emission_diagnostic;
    StringBuilder source;
    ShaderLabExpressionSourceMap source_map;
    bool preprocess_attempted;
    bool preprocess_received;
    UnityCompilerPreprocessResponse preprocessing;
    UnityShaderLabLiftPassReport *passes;
    size_t pass_count;
    size_t certified_pass_count;
    UnityShaderLabLiftSourceInventory bounded_source_inventory;
} UnityShaderLabLiftArtifact;

typedef struct UnityShaderLabLiftResult UnityShaderLabLiftResult;

/* Verify the ordinary low-level baseline, then the ordinary expression candidate.
 * If raw source emission is unsupported, the expression candidate may instead
 * establish its own full generated-domain evidence. The failed baseline stays
 * recorded and is never a fallback. Other baseline failures stop candidate work.
 * With a second candidate budget, an expression rejected by emission, compilation
 * or complete DXBC comparison can try the closed Unity UV family under its own
 * included low-level baseline. Missing evidence and authority failures cannot
 * start this alternate spelling.
 * Every emitted local D3D11 pass is checked across its full generated domain.
 * A helper pass additionally checks the actual expanded definitions for every
 * selected request, including stages whose bodies do not call the helper.
 *
 * Returns the last attempted status; query accepted() explicitly for fallback.
 * Both included artifacts retain their own controls/evidence; ordinary and
 * included controls are never equated. Failed helper work preserves the earlier
 * accepted artifact unless pinned source/include/compiler/profile authority
 * changed. Limits count both baselines and preprocess-only helper requests as
 * compiles. Inputs remain immutable until the call completes.
 *
 * Evidence covers generated local D3D11 domains, not external UsePass, imports,
 * render state or whole-shader logical/visual equivalence. All four artifacts
 * retain observations for review; missing or late evidence cannot be accepted. */
HLSLLiftStatus unity_shaderlab_lift_run(const UnityShaderLabLiftInput *input,
                                        const UnityShaderLabLiftServices *services,
                                        const HLSLLiftLimits *limits,
                                        UnityShaderLabLiftResult **out_result);

/* All views are borrowed and must remain unmodified until result_free(). */
const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_baseline(const UnityShaderLabLiftResult *result);
const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_candidate(const UnityShaderLabLiftResult *result);
const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_helper_baseline(const UnityShaderLabLiftResult *result);
const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_helper_candidate(const UnityShaderLabLiftResult *result);
const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_accepted(const UnityShaderLabLiftResult *result);
void unity_shaderlab_lift_stats(const UnityShaderLabLiftResult *result, HLSLLiftStats *stats,
                                size_t *preprocess_requests);
void unity_shaderlab_lift_result_free(UnityShaderLabLiftResult *result);

/* Deterministic malloc-owned JSON with source/target/request digests and spans.
 * Omits source text, names, paths and raw compiler diagnostics. This is a
 * rendering of the typed result, never an independent certificate. */
char *unity_shaderlab_lift_format_json(const UnityShaderLabLiftResult *result);

/* Reporting of an immutable, trusted-producer snapshot with current source
 * binding only. These functions check source bytes/size/digest, not a current
 * model, receipt replay or the authenticity of caller-filled public structs.
 * They grant no compiler, schema, dependency, publication or native authority.
 * JSON renders the historical quality/structural observations and omits source
 * text and names. A changed source cannot reuse the captured inventory; NULL is
 * not-run. Use shaderlab_source_quality_inventory_analyze() for typed replay. */
bool unity_shaderlab_lift_inventory_matches_source(
    const UnityShaderLabLiftArtifact *artifact, size_t source_size,
    const uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE]);
bool unity_shaderlab_lift_append_inventory_json(
    const UnityShaderLabLiftArtifact *artifact, StringBuilder *output);

#endif
