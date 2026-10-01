// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADERLAB_LIFT_CLI_H
#define SHADERLAB_LIFT_CLI_H

#include "app/shader_batch.h"

typedef struct {
    bool enabled;
    const char *profile_path;
    const char *project_root;
    const char *includes;
    size_t max_compiles;
    uint64_t max_elapsed_ms;
} CliShaderLabLiftOptions;

typedef struct CliShaderLabLift CliShaderLabLift;

bool cli_shaderlab_lift_supported(void);
bool cli_shaderlab_lift_verifier_supported(void);
CliShaderLabLift *cli_shaderlab_lift_create(const CliShaderLabLiftOptions *options, size_t records);
/* A profile selects the existing verifier. Without one, the portable core
 * produces complete source candidates without compiler or runtime evidence. */
bool cli_shaderlab_lift_verification_requested(const CliShaderLabLift *lift);
void cli_shaderlab_lift_attach(CliShaderLabLift *lift, ShaderBatchOptions *options);
bool cli_shaderlab_lift_append_json(const CliShaderLabLift *lift, size_t record,
                                    StringBuilder *out);
/* Source-only outcomes are not-run, high-level or unavailable. Verification
 * mode retains high-level, low-level-fallback and unverified outcomes. */
const char *cli_shaderlab_lift_selection(const CliShaderLabLift *lift, size_t record);
/* Both publication predicates require exact source binding, final identity
 * closure and no publication residue. Generated does not imply verified. */
bool cli_shaderlab_lift_output_generated(const CliShaderLabLift *lift, size_t record,
                                         const ShaderBatchRecordResult *publication);
bool cli_shaderlab_lift_output_verified(const CliShaderLabLift *lift, size_t record,
                                        const ShaderBatchRecordResult *publication);
/* An immutable historical producer summary bound to current publication bytes;
 * it is not retained typed receipt replay or an independent certificate. */
bool cli_shaderlab_lift_append_published_inventory_json(
    const CliShaderLabLift *lift, size_t record,
    const ShaderBatchRecordResult *publication, StringBuilder *out);
void cli_shaderlab_lift_free(CliShaderLabLift *lift);

#endif
