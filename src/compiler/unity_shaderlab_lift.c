// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_lift.h"
#include "compiler/unity_shaderlab_mapping.h"
#include "translation/hlsl_lift_control.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct UnityShaderLabLiftResult {
    UnityShaderLabLiftArtifact baseline;
    UnityShaderLabLiftArtifact candidate;
    const UnityShaderLabLiftArtifact *accepted;
    HLSLLiftStats stats;
    size_t preprocess_requests;
};

typedef struct {
    const UnityShaderLabLiftInput *input;
    UnityShaderLabLiftServices services;
    HLSLLiftLimits limits;
    HLSLLiftControl control;
    UnityShaderLabLiftResult *result;
    uint8_t compiler_digest[32];
    uint8_t environment_digest[32];
    uint8_t profile_digest[32];
} LiftContext;

static bool monotonic_ms(void *context, uint64_t *milliseconds) {
    (void)context;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return false;
    *milliseconds =
        (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
    return true;
}

void unity_shaderlab_lift_default_services(UnityShaderLabLiftServices *services) {
    if (services) {
        memset(services, 0, sizeof(*services));
        services->monotonic_ms = monotonic_ms;
    }
}

static bool has_digest(const uint8_t digest[32]) {
    uint8_t bits = 0;
    for (size_t i = 0; i < 32; ++i)
        bits |= digest[i];
    return bits != 0;
}

static bool toolchain(LiftContext *context, UnityCompilerToolchainProvenance *provenance) {
    memset(provenance, 0, sizeof(*provenance));
    const bool received =
        context->services.toolchain
            ? context->services.toolchain(context->services.context, provenance)
            : unity_compiler_broker_get_toolchain_provenance(context->input->broker, provenance);
    return received && has_digest(provenance->compiler_fingerprint) &&
           has_digest(provenance->environment_fingerprint);
}

static HLSLLiftStatus work_status(LiftContext *context) {
    HLSLLiftStatus status = hlsl_lift_control_check(&context->control);
    if (status == HLSL_LIFT_VERIFIED) {
        UnityCompilerToolchainProvenance current;
        uint8_t profile_digest[32];
        if (!toolchain(context, &current) ||
            unity_compile_profile_fingerprint(context->input->profile, profile_digest) !=
                UNITY_COMPILE_PROFILE_OK ||
            memcmp(profile_digest, context->profile_digest, 32) != 0 ||
            memcmp(current.compiler_fingerprint, context->compiler_digest, 32) != 0 ||
            memcmp(current.environment_fingerprint, context->environment_digest, 32) != 0) {
            context->control.status = HLSL_LIFT_AUTHORITY_MISMATCH;
        }
        /* Fingerprinting is also work and must finish within the deadline. */
        status = hlsl_lift_control_check(&context->control);
    }
    context->result->stats.elapsed_ms = context->control.elapsed_ms;
    return status;
}

static bool compile(void *opaque, const UnityCompilerSnippetCompileRequest *request,
                    UnityCompilerBinaryResponse *response) {
    LiftContext *context = opaque;
    if (work_status(context) != HLSL_LIFT_VERIFIED)
        return false;
    if (context->result->stats.compiles >= context->limits.max_compiles) {
        context->control.status = HLSL_LIFT_BUDGET_EXHAUSTED;
        return false;
    }
    ++context->result->stats.compiles;
    const bool received =
        context->services.compile
            ? context->services.compile(context->services.context, request, response)
            : unity_compiler_broker_compile_contract_response(context->input->broker, request,
                                                              response);
    if (response->status.from_cache)
        ++context->result->stats.cache_hits;
    if (work_status(context) != HLSL_LIFT_VERIFIED)
        return false;
    if (received && (!response->has_request_identity || !has_digest(response->request_digest) ||
                     !has_digest(response->controls_digest))) {
        context->control.status = HLSL_LIFT_PROVENANCE_MISMATCH;
        return false;
    }
    return received;
}

static HLSLLiftStatus domain_status(UnityGeneratedDomainStatus status) {
    switch (status) {
    case UNITY_GENERATED_DOMAIN_OK:
        return HLSL_LIFT_VERIFIED;
    case UNITY_GENERATED_DOMAIN_DXBC_MISMATCH:
        return HLSL_LIFT_DXBC_MISMATCH;
    case UNITY_GENERATED_DOMAIN_REFERENCE_DXBC_INVALID:
    case UNITY_GENERATED_DOMAIN_COMPILED_DXBC_INVALID:
        return HLSL_LIFT_INVALID_DXBC;
    case UNITY_GENERATED_DOMAIN_COMPILER_CACHE_ONLY_MISS:
    case UNITY_GENERATED_DOMAIN_COMPILER_TRANSPORT_FAILED:
        return HLSL_LIFT_COMPILER_UNAVAILABLE;
    case UNITY_GENERATED_DOMAIN_COMPILER_REJECTED:
    case UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC:
        return HLSL_LIFT_COMPILER_REJECTED;
    case UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY:
        return HLSL_LIFT_OUT_OF_MEMORY;
    default:
        return HLSL_LIFT_AUTHORITY_MISMATCH;
    }
}

static HLSLLiftStatus preprocess(LiftContext *context, UnityShaderLabLiftArtifact *artifact,
                                 bool high_level) {
    HLSLLiftStatus status = work_status(context);
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    const UnityShaderLabLiftInput *input = context->input;
    const UnityCompilerShaderPreprocessRequest request = {
        .source = artifact->source.buf,
        .file_path = input->source_path,
        .shader_name = input->shader->name,
        .caching_preprocessor = true,
        .build_platform = input->profile->build_platform,
        .valid_apis = input->profile->valid_apis,
    };
    artifact->preprocess_attempted = true;
    ++context->result->preprocess_requests;
    UnityCompilerPreprocessResponse *response = &artifact->preprocessing;
    const bool received =
        context->services.preprocess
            ? context->services.preprocess(context->services.context, &request, response)
            : unity_compiler_broker_preprocess_contract_response(input->broker, &request, response);
    status = work_status(context);
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    if (!received || response->status.availability == UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS)
        return HLSL_LIFT_COMPILER_UNAVAILABLE;
    if (!unity_compiler_response_status_is_clean_success(&response->status))
        return HLSL_LIFT_COMPILER_REJECTED;
    if (!response->has_request_identity || !has_digest(response->request_digest) ||
        !has_digest(response->controls_digest))
        return HLSL_LIFT_PROVENANCE_MISMATCH;
    if (high_level && memcmp(response->controls_digest,
                             context->result->baseline.preprocessing.controls_digest, 32) != 0)
        return HLSL_LIFT_AUTHORITY_MISMATCH;
    return HLSL_LIFT_VERIFIED;
}

static HLSLLiftStatus inventory(const SerializedShader *shader,
                                UnityShaderLabLiftArtifact *artifact) {
    const int count = unity_shaderlab_snippet_count(shader);
    if (count <= 0)
        return HLSL_LIFT_PRECONDITION_REJECTED;
    artifact->passes = calloc((size_t)count, sizeof(*artifact->passes));
    if (!artifact->passes)
        return HLSL_LIFT_OUT_OF_MEMORY;
    artifact->pass_count = (size_t)count;
    size_t next = 0;
    int ordinal = 0;
    for (int s = 0; s < shader->subshader_count; ++s) {
        const SerializedSubShader *subshader = &shader->subshaders[s];
        for (int p = 0; p < subshader->pass_count; ++p, ++ordinal) {
            if (!unity_shaderlab_pass_emits_snippet(&subshader->passes[p]))
                continue;
            UnityShaderLabLiftPassReport *pass = &artifact->passes[next++];
            pass->subshader_index = s;
            pass->pass_index = p;
            pass->serialized_pass_index = ordinal;
            pass->snippet_index = -1;
            unity_generated_domain_report_init(&pass->domain);
        }
    }
    return HLSL_LIFT_VERIFIED;
}

static HLSLLiftStatus certify_pass(LiftContext *context, UnityShaderLabLiftArtifact *artifact,
                                   UnityShaderLabLiftPassReport *report) {
    const UnityShaderLabLiftInput *input = context->input;
    const SerializedPass *pass =
        unity_shaderlab_pass_at(input->shader, report->serialized_pass_index);
    const PreprocessedSnippet *snippet = unity_shaderlab_find_generated_snippet(
        input->shader, report->serialized_pass_index, &artifact->preprocessing.result,
        &report->snippet_index);
    if (!snippet)
        return HLSL_LIFT_AUTHORITY_MISMATCH;
    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    report->plan_attempted = true;
    report->plan_status =
        shaderlab_variant_plan_build(input->shader, pass, &plan, &report->plan_diagnostic);
    HLSLLiftStatus status = work_status(context);
    if (status == HLSL_LIFT_VERIFIED && report->plan_status != SHADERLAB_VARIANT_PLAN_OK)
        status = HLSL_LIFT_PRECONDITION_REJECTED;
    if (status == HLSL_LIFT_VERIFIED) {
        const UnityGeneratedDomainCertificationInput certification = {
            .shader = input->shader,
            .pass = pass,
            .plan = &plan,
            .generated_snippet = snippet,
            .d3d11_archive = input->archive,
            .compile_profile = input->profile,
            .source_directory = input->source_directory,
            .source_basename = input->source_basename,
            .pass_name = pass->name ? pass->name : "",
            .compile_callback = compile,
            .compile_context = context,
            .retain_compile_provenance = true,
        };
        report->certification_attempted = true;
        status =
            domain_status(unity_generated_domain_certify_d3d11(&certification, &report->domain));
        const HLSLLiftStatus controlled = work_status(context);
        if (controlled != HLSL_LIFT_VERIFIED)
            status = controlled;
    }
    shaderlab_variant_plan_free(&plan);
    return status;
}

static HLSLLiftStatus attempt(LiftContext *context, UnityShaderLabLiftArtifact *artifact,
                              bool high_level) {
    artifact->attempted = true;
    HLSLLiftStatus status = work_status(context);
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    const UnityShaderLabLiftInput *input = context->input;
    status = inventory(input->shader, artifact);
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    const ShaderBlobArchive *archive = input->archive;
    const bool emitted =
        high_level ? shaderlab_emit_high_level_candidate_with_source_map(
                         input->shader, archive->entries, archive->entry_count, archive->segments,
                         archive->segment_lengths, archive->segment_count, &artifact->source,
                         &artifact->source_map, &artifact->emission_diagnostic)
                   : shaderlab_emit_candidate_with_diagnostic(
                         input->shader, archive->entries, archive->entry_count, archive->segments,
                         archive->segment_lengths, archive->segment_count, &artifact->source,
                         &artifact->emission_diagnostic);
    status = work_status(context);
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    if (!emitted)
        return HLSL_LIFT_EMISSION_REJECTED;
    if (high_level &&
        !shaderlab_expression_source_map_matches_source(&artifact->source_map, &artifact->source))
        return HLSL_LIFT_PROVENANCE_MISMATCH;
    status = preprocess(context, artifact, high_level);
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    for (size_t p = 0; p < artifact->pass_count; ++p) {
        status = certify_pass(context, artifact, &artifact->passes[p]);
        if (status != HLSL_LIFT_VERIFIED)
            return status;
        ++artifact->certified_pass_count;
    }
    return work_status(context);
}

static void artifact_init(UnityShaderLabLiftArtifact *artifact) {
    memset(artifact, 0, sizeof(*artifact));
    artifact->status = HLSL_LIFT_PRECONDITION_REJECTED;
    sb_init(&artifact->source);
    unity_compiler_preprocess_response_init(&artifact->preprocessing);
}

HLSLLiftStatus unity_shaderlab_lift_run(const UnityShaderLabLiftInput *input,
                                        const UnityShaderLabLiftServices *services,
                                        const HLSLLiftLimits *limits,
                                        UnityShaderLabLiftResult **out_result) {
    if (out_result)
        *out_result = NULL;
    if (!out_result || !input || !input->shader || !input->archive || !input->profile ||
        !input->shader->name || !input->shader->name[0] || !input->source_path ||
        !input->source_path[0] || !input->source_directory || !input->source_directory[0] ||
        !input->source_basename || !input->source_basename[0] || !limits ||
        !unity_compile_profile_validate(input->profile))
        return HLSL_LIFT_INVALID_ARGUMENT;
    LiftContext context = {.input = input, .limits = *limits};
    unity_shaderlab_lift_default_services(&context.services);
    if (services)
        context.services = *services;
    if (!context.services.monotonic_ms ||
        (!input->broker && (!context.services.preprocess || !context.services.compile ||
                            !context.services.toolchain)))
        return HLSL_LIFT_INVALID_ARGUMENT;
    UnityShaderLabLiftResult *result = calloc(1, sizeof(*result));
    if (!result)
        return HLSL_LIFT_OUT_OF_MEMORY;
    *out_result = result;
    context.result = result;
    artifact_init(&result->baseline);
    artifact_init(&result->candidate);
    HLSLLiftStatus status = hlsl_lift_control_begin(
        &context.control, context.services.monotonic_ms, context.services.cancelled,
        context.services.context, limits->max_elapsed_ms);
    if (status == HLSL_LIFT_VERIFIED)
        status = hlsl_lift_control_check(&context.control);
    result->stats.elapsed_ms = context.control.elapsed_ms;
    UnityCompilerToolchainProvenance provenance;
    if (status == HLSL_LIFT_VERIFIED &&
        (!toolchain(&context, &provenance) ||
         unity_compile_profile_fingerprint(input->profile, context.profile_digest) !=
             UNITY_COMPILE_PROFILE_OK))
        status = HLSL_LIFT_AUTHORITY_MISMATCH;
    if (status == HLSL_LIFT_VERIFIED) {
        memcpy(context.compiler_digest, provenance.compiler_fingerprint, 32);
        memcpy(context.environment_digest, provenance.environment_fingerprint, 32);
        status = attempt(&context, &result->baseline, false);
    }
    result->baseline.status = status;
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    result->accepted = &result->baseline;
    if (limits->max_candidates == 0) {
        result->candidate.status = HLSL_LIFT_BUDGET_EXHAUSTED;
    } else {
        result->stats.candidates = 1;
        result->candidate.status = attempt(&context, &result->candidate, true);
    }
    if (result->candidate.status == HLSL_LIFT_VERIFIED) {
        result->accepted = &result->candidate;
        result->stats.accepted = 1;
    }
    return result->candidate.status;
}

const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_baseline(const UnityShaderLabLiftResult *result) {
    return result ? &result->baseline : NULL;
}

const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_candidate(const UnityShaderLabLiftResult *result) {
    return result ? &result->candidate : NULL;
}

const UnityShaderLabLiftArtifact *
unity_shaderlab_lift_accepted(const UnityShaderLabLiftResult *result) {
    return result ? result->accepted : NULL;
}

void unity_shaderlab_lift_stats(const UnityShaderLabLiftResult *result, HLSLLiftStats *stats,
                                size_t *preprocess_requests) {
    if (stats)
        *stats = result ? result->stats : (HLSLLiftStats){0};
    if (preprocess_requests)
        *preprocess_requests = result ? result->preprocess_requests : 0;
}

static void artifact_free(UnityShaderLabLiftArtifact *artifact) {
    for (size_t p = 0; p < artifact->pass_count; ++p)
        unity_generated_domain_report_free(&artifact->passes[p].domain);
    free(artifact->passes);
    unity_compiler_preprocess_response_free(&artifact->preprocessing);
    shaderlab_expression_source_map_free(&artifact->source_map);
    sb_free(&artifact->source);
}

void unity_shaderlab_lift_result_free(UnityShaderLabLiftResult *result) {
    if (result) {
        artifact_free(&result->baseline);
        artifact_free(&result->candidate);
        free(result);
    }
}
