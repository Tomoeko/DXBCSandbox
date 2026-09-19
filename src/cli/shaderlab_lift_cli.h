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
CliShaderLabLift *cli_shaderlab_lift_create(const CliShaderLabLiftOptions *options, size_t records);
void cli_shaderlab_lift_attach(CliShaderLabLift *lift, ShaderBatchOptions *options);
bool cli_shaderlab_lift_append_json(const CliShaderLabLift *lift, size_t record,
                                    StringBuilder *out);
const char *cli_shaderlab_lift_selection(const CliShaderLabLift *lift, size_t record);
bool cli_shaderlab_lift_output_verified(const CliShaderLabLift *lift, size_t record,
                                        const ShaderBatchRecordResult *publication);
void cli_shaderlab_lift_free(CliShaderLabLift *lift);

#endif
