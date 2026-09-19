// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADERLAB_LIFT_H
#define UNITY_SHADERLAB_LIFT_H

#include "compiler/unity_generated_domain_certifier.h"
#include "compiler/unity_uv_helper.h"
#include "translation/hlsl_lift_transaction.h"
#include "translation/shaderlab_emitter.h"

typedef struct {
    const SerializedShader *shader;
    const ShaderBlobArchive *archive;
    const UnityCompileProfile *profile;
    UnityCompilerBroker *broker;
    /* One immutable source identity is used for both candidate modes. */
    const char *source_path;
    const char *source_directory;
    const char *source_basename;
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
} UnityShaderLabLiftArtifact;

typedef struct UnityShaderLabLiftResult UnityShaderLabLiftResult;

/* Verify the ordinary low-level baseline, then the ordinary expression candidate.
 * With a second candidate budget, an emission-rejected expression attempt can
 * try the closed Unity UV family under its own included low-level baseline.
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

#endif
