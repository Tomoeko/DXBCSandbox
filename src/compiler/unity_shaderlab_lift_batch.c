// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_lift_batch.h"

#include <stdlib.h>

struct UnityShaderLabLiftBatch {
    UnityCompilerBroker *broker;
    UnityCompileProfile profile;
    HLSLLiftLimits limits;
    size_t record_count;
    UnityShaderLabLiftResult **results;
};

UnityShaderLabLiftBatch *unity_shaderlab_lift_batch_create(UnityCompilerBroker *broker,
                                                           const UnityCompileProfile *profile,
                                                           const HLSLLiftLimits *limits,
                                                           size_t record_count) {
    if (!broker || !limits || !unity_compile_profile_validate(profile))
        return NULL;
    UnityShaderLabLiftBatch *batch = calloc(1, sizeof(*batch));
    if (!batch)
        return NULL;
    if (record_count) {
        batch->results = calloc(record_count, sizeof(*batch->results));
        if (!batch->results) {
            free(batch);
            return NULL;
        }
    }
    batch->broker = broker;
    batch->profile = *profile;
    batch->limits = *limits;
    batch->record_count = record_count;
    return batch;
}

static bool select_candidate(void *opaque, const ShaderBatchCandidateInput *input,
                             StringBuilder *source, ShaderLabCandidateDiagnostic *diagnostic) {
    UnityShaderLabLiftBatch *batch = opaque;
    if (!batch || !input || input->catalog_record_index >= batch->record_count ||
        batch->results[input->catalog_record_index] || !source || !diagnostic)
        return false;
    const UnityShaderLabLiftInput lift = {
        .shader = &input->object->shader,
        .archive = input->archive,
        .profile = &batch->profile,
        .broker = batch->broker,
        .source_path = input->source_path,
        .source_directory = input->source_directory,
        .source_basename = input->source_basename,
    };
    UnityShaderLabLiftResult **result = &batch->results[input->catalog_record_index];
    (void)unity_shaderlab_lift_run(&lift, NULL, &batch->limits, result);
    const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(*result);
    if (!accepted) {
        const UnityShaderLabLiftArtifact *baseline = unity_shaderlab_lift_baseline(*result);
        if (baseline)
            *diagnostic = baseline->emission_diagnostic;
        return false;
    }
    *diagnostic = accepted->emission_diagnostic;
    sb_append_len(source, accepted->source.buf, accepted->source.len);
    return sb_ok(source);
}

void unity_shaderlab_lift_batch_attach(UnityShaderLabLiftBatch *batch,
                                       ShaderBatchOptions *options) {
    if (options && batch) {
        options->select_candidate = select_candidate;
        options->candidate_context = batch;
    }
}

const UnityShaderLabLiftResult *
unity_shaderlab_lift_batch_result(const UnityShaderLabLiftBatch *batch, size_t record_index) {
    return batch && record_index < batch->record_count ? batch->results[record_index] : NULL;
}

void unity_shaderlab_lift_batch_free(UnityShaderLabLiftBatch *batch) {
    if (batch) {
        for (size_t i = 0; i < batch->record_count; ++i)
            unity_shaderlab_lift_result_free(batch->results[i]);
        free(batch->results);
        free(batch);
    }
}
