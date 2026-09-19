// SPDX-License-Identifier: GPL-3.0-only

#include "shaderlab_lift_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DXBCSANDBOX_CLI_UNITY_COMPILER
#include "compiler/unity_shaderlab_lift_batch.h"

struct CliShaderLabLift {
    UnityCompilerBroker *broker;
    UnityShaderLabLiftBatch *batch;
};

bool cli_shaderlab_lift_supported(void) { return true; }

CliShaderLabLift *cli_shaderlab_lift_create(const CliShaderLabLiftOptions *options,
                                            size_t records) {
    if (!options || !options->enabled)
        return NULL;
    UnityCompileProfile profile;
    UnityCompileProfileStatus status = unity_compile_profile_load(options->profile_path, &profile);
    if (status != UNITY_COMPILE_PROFILE_OK) {
        fprintf(stderr, "Error: could not load compile profile: %s.\n",
                unity_compile_profile_status_string(status));
        return NULL;
    }
    CliShaderLabLift *lift = calloc(1, sizeof(*lift));
    if (!lift)
        return NULL;
    lift->broker =
        unity_compiler_broker_create_lazy(options->project_root ? options->project_root : ".",
                                          options->includes ? options->includes : "");
    const HLSLLiftLimits limits = {1, options->max_compiles, options->max_elapsed_ms};
    if (lift->broker &&
        unity_compiler_broker_set_expected_valid_apis(lift->broker, profile.valid_apis))
        lift->batch = unity_shaderlab_lift_batch_create(lift->broker, &profile, &limits, records);
    if (!lift->batch) {
        fputs("Error: could not initialize the ShaderLab compiler verifier.\n", stderr);
        cli_shaderlab_lift_free(lift);
        return NULL;
    }
    return lift;
}

void cli_shaderlab_lift_attach(CliShaderLabLift *lift, ShaderBatchOptions *options) {
    if (lift)
        unity_shaderlab_lift_batch_attach(lift->batch, options);
}

bool cli_shaderlab_lift_append_json(const CliShaderLabLift *lift, size_t record,
                                    StringBuilder *out) {
    const UnityShaderLabLiftResult *result =
        lift ? unity_shaderlab_lift_batch_result(lift->batch, record) : NULL;
    if (!result) {
        sb_append(out, "null");
        return sb_ok(out);
    }
    char *json = unity_shaderlab_lift_format_json(result);
    if (!json)
        return false;
    sb_append(out, json);
    free(json);
    return sb_ok(out);
}

const char *cli_shaderlab_lift_selection(const CliShaderLabLift *lift, size_t record) {
    const UnityShaderLabLiftResult *result =
        lift ? unity_shaderlab_lift_batch_result(lift->batch, record) : NULL;
    if (!result)
        return "not-run";
    const UnityShaderLabLiftArtifact *accepted = unity_shaderlab_lift_accepted(result);
    if (!accepted)
        return "unverified";
    return accepted == unity_shaderlab_lift_candidate(result) ? "high-level" : "low-level-fallback";
}

bool cli_shaderlab_lift_output_verified(const CliShaderLabLift *lift, size_t record,
                                        const ShaderBatchRecordResult *publication) {
    if (!lift || !publication || !publication->publication_authorized ||
        !publication->published_shader_content_recorded)
        return false;
    const UnityShaderLabLiftArtifact *accepted =
        unity_shaderlab_lift_accepted(unity_shaderlab_lift_batch_result(lift->batch, record));
    if (!accepted || accepted->source.len != publication->published_shader_size)
        return false;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(accepted->source.buf, accepted->source.len, digest);
    return memcmp(digest, publication->published_shader_digest, sizeof(digest)) == 0;
}

void cli_shaderlab_lift_free(CliShaderLabLift *lift) {
    if (lift) {
        unity_shaderlab_lift_batch_free(lift->batch);
        unity_compiler_broker_destroy(lift->broker);
        free(lift);
    }
}

#else
/* The portable executable recognizes the option and gives an actionable build
 * error before opening inputs. Its normal extraction pipeline is unchanged. */
bool cli_shaderlab_lift_supported(void) { return false; }
CliShaderLabLift *cli_shaderlab_lift_create(const CliShaderLabLiftOptions *options,
                                            size_t records) {
    (void)options;
    (void)records;
    return NULL;
}
void cli_shaderlab_lift_attach(CliShaderLabLift *lift, ShaderBatchOptions *options) {
    (void)lift;
    (void)options;
}
bool cli_shaderlab_lift_append_json(const CliShaderLabLift *lift, size_t record,
                                    StringBuilder *out) {
    (void)lift;
    (void)record;
    sb_append(out, "null");
    return sb_ok(out);
}
const char *cli_shaderlab_lift_selection(const CliShaderLabLift *lift, size_t record) {
    (void)lift;
    (void)record;
    return "not-run";
}
bool cli_shaderlab_lift_output_verified(const CliShaderLabLift *lift, size_t record,
                                        const ShaderBatchRecordResult *publication) {
    (void)lift;
    (void)record;
    (void)publication;
    return false;
}
void cli_shaderlab_lift_free(CliShaderLabLift *lift) { (void)lift; }
#endif
