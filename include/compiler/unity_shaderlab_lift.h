// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADERLAB_LIFT_H
#define UNITY_SHADERLAB_LIFT_H

#include "compiler/unity_generated_domain_certifier.h"
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
} UnityShaderLabLiftServices;

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
} UnityShaderLabLiftPassReport;

typedef struct {
    bool attempted;
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

/* Emit and verify the low-level baseline, then attempt the bounded expression
 * candidate under the same profile/toolchain/source identity. Every emitted
 * local D3D11 pass is checked across its full generated stage/state/tier domain.
 * No tuple filtering or original-source diagnostic substitution is performed.
 *
 * Return the failed baseline status, or the high-level attempt's status. A
 * failed high-level attempt can still leave a verified low-level artifact;
 * query accepted() explicitly. An allocation-successful result retains both
 * attempts for review even on failure. Inputs stay immutable during this call.
 *
 * Limits include baseline compiles and one optional high-level candidate.
 * The returned evidence covers generated local D3D11 program domains only,
 * not external UsePass/dependencies, import, render state, or whole-shader
 * logical/visual equivalence. Late, cancelled or unproven output is never
 * returned by accepted(). A baseline accepted before a later candidate failure
 * remains available unless the pinned compiler, includes, or profile changed.
 * Both artifacts retain their original evidence for review. Compile request digests can
 * differ between forms because preprocessed contracts carry source hashes and
 * line locations; each complete contract is independently attested. */
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
unity_shaderlab_lift_accepted(const UnityShaderLabLiftResult *result);
void unity_shaderlab_lift_stats(const UnityShaderLabLiftResult *result, HLSLLiftStats *stats,
                                size_t *preprocess_requests);
void unity_shaderlab_lift_result_free(UnityShaderLabLiftResult *result);

/* Deterministic malloc-owned JSON with source/target/request digests and spans.
 * Omits source text, names, paths and raw compiler diagnostics. This is a
 * rendering of the typed result, never an independent certificate. */
char *unity_shaderlab_lift_format_json(const UnityShaderLabLiftResult *result);

#endif
