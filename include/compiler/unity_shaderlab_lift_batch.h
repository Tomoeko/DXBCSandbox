// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADERLAB_LIFT_BATCH_H
#define UNITY_SHADERLAB_LIFT_BATCH_H

#include "app/shader_batch.h"
#include "compiler/unity_shaderlab_lift.h"

typedef struct UnityShaderLabLiftBatch UnityShaderLabLiftBatch;

/* Owns per-record lift results and a copy of the profile/limits. The broker is
 * borrowed and must remain valid. Attach before shader_batch_extract_ex().
 * Neither a successful lift nor this adapter authorizes output publication;
 * the batch's source-identity and publication checks still apply. */
UnityShaderLabLiftBatch *unity_shaderlab_lift_batch_create(UnityCompilerBroker *broker,
                                                           const UnityCompileProfile *profile,
                                                           const HLSLLiftLimits *limits,
                                                           size_t record_count);
void unity_shaderlab_lift_batch_attach(UnityShaderLabLiftBatch *batch, ShaderBatchOptions *options);
const UnityShaderLabLiftResult *
unity_shaderlab_lift_batch_result(const UnityShaderLabLiftBatch *batch, size_t record_index);
void unity_shaderlab_lift_batch_free(UnityShaderLabLiftBatch *batch);

#endif
