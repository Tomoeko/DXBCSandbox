#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>

#include "common/common.h"
#include "common/file_io.h"
#include "common/oracle_metadata.h"
#include "common/oracle_pack.h"
#include "common/path_discovery.h"
#include "common/shader_artifact.h"
#include "common/sha256.h"
#include "common/shaderlab_source.h"
#include "common/variant_key.h"
#include "app/glcore_link_certificate.h"
#include "app/verification_scope.h"
#include "io/serialized_file.h"
#include "io/serialized_glcore_target.h"
#include "io/shader_object.h"
#include "io/shader_blob_archive.h"
#include "io/subprogram_metadata.h"
#include "io/unity_input.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_parser.h"
#include "dxbc/dxbc_hash.h"
#include "compiler/unity_compile_authority.h"
#include "compiler/unity_compile_profile.h"
#include "compiler/unity_compiler_broker.h"
#include "compiler/unity_generated_domain_certifier.h"
#include "compiler/unity_shaderlab_mapping.h"
#include "translation/shaderlab_emitter.h"
#include "test_helpers.h"

_Static_assert(
    UNITY_COMPILE_PROFILE_CAPABILITY_COUNT ==
        UNITY_PLATFORM_CAPABILITY_COUNT,
    "compile-profile and compiler-authority capability widths differ");

typedef struct {
    const SerializedShader* shader;
    PlayerSubProgramMetadata* sub_meta;
    size_t shader_result_index;
    long long path_id;
    int stage;
    int sub_idx;
    int pass_idx;
    int subshader_idx;
    int local_pass_idx;
    const SerializedPass* pass;
    const SerializedSubProgramIdentity* identity;
    const char* generated_source_path;
    const PreprocessResult* gen_prep;
    const PreprocessResult* orig_prep;
    const char* shaderlab_content;
    OracleMetadataNormalization* oracle_metadata;
} SubProgramJob;

typedef struct {
    bool preprocess_present;
    uint32_t build_platform;
    uint32_t valid_apis;
    UnityPlatformCapabilitySnapshot d3d11_capabilities;
    UnityPlatformCapabilitySnapshot glcore_capabilities;
} VerificationCompileProfile;

typedef struct {
    bool has_build_platform;
    bool has_valid_apis;
    uint32_t build_platform;
    uint32_t valid_apis;
    UnityPlatformCapabilitySnapshot d3d11_capabilities;
    UnityPlatformCapabilitySnapshot glcore_capabilities;
} VerificationCompileOverrides;

enum {
    COMPILE_OVERRIDE_BUILD_PLATFORM = 1u << 0,
    COMPILE_OVERRIDE_VALID_APIS = 1u << 1,
    COMPILE_OVERRIDE_D3D11_CAPABILITIES = 1u << 2,
    COMPILE_OVERRIDE_GLCORE_CAPABILITIES = 1u << 3,
};

static VerificationCompileProfile g_compile_profile;
static UnityCompileProfile g_effective_compile_profile;

typedef struct {
    OraclePack* frozen_pack;
    OraclePackWriter* capture_writer;
    const char* frozen_path;
    const char* capture_path;
    bool strict_hits;
    bool capture_failed;
    bool ready;
    bool live_authority_matches;
    uint8_t compiler_fingerprint[ORACLE_PACK_DIGEST_SIZE];
    uint8_t environment_fingerprint[ORACLE_PACK_DIGEST_SIZE];
    uint64_t hits;
    uint64_t misses;
    uint64_t broker_fallbacks;
    uint64_t captured_entries;
    uint64_t preprocess_hits;
    uint64_t preprocess_misses;
    uint64_t preprocess_broker_fallbacks;
    uint64_t captured_preprocesses;
    uint64_t authority_failures;
    pthread_mutex_t mutex;
} VerificationOracle;

static VerificationOracle g_oracle = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static SubProgramJob* g_job_queue = NULL;
static size_t g_job_queue_size = 0U;
static size_t g_job_queue_capacity = 0U;
static size_t g_active_jobs = 0U;
static bool g_job_queue_failed = false;
static bool g_should_exit = false;

static pthread_mutex_t g_job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_done_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    long long path_id;
    int stage;
    int sub_idx;
    int pass_idx;
} FailureFilter;

static FailureFilter* g_filters = NULL;
static int g_filter_count = 0;
static int g_filter_capacity = 0;

static void load_filters(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) {
        printf("[INFO] No filter file found or failed to open: %s\n", filepath);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long path_id = 0;
        int stage = 0;
        int sub_idx = 0;
        int pass_idx = -1;
        int parsed = sscanf(line, "%lld %d %d %d", &path_id, &stage, &sub_idx, &pass_idx);
        if (parsed >= 3) {
            if (g_filter_count >= g_filter_capacity) {
                g_filter_capacity = g_filter_capacity == 0 ? 128 : g_filter_capacity * 2;
                FailureFilter* new_filters = realloc(g_filters, g_filter_capacity * sizeof(FailureFilter));
                if (!new_filters) {
                    fprintf(stderr, "Out of memory loading filters\n");
                    exit(1);
                }
                g_filters = new_filters;
            }
            g_filters[g_filter_count].path_id = path_id;
            g_filters[g_filter_count].stage = stage;
            g_filters[g_filter_count].sub_idx = sub_idx;
            g_filters[g_filter_count].pass_idx = (parsed >= 4) ? pass_idx : -1;
            g_filter_count++;
        }
    }
    fclose(f);
    printf("[INFO] Loaded %d variant filters from %s\n", g_filter_count, filepath);
}

static bool is_shader_filtered(long long path_id) {
    if (g_filter_count == 0) return true;
    for (int i = 0; i < g_filter_count; i++) {
        if (g_filters[i].path_id == path_id) {
            return true;
        }
    }
    return false;
}

static bool is_variant_filtered(long long path_id, int stage, int sub_idx, int pass_idx) {
    if (g_filter_count == 0) return true;
    for (int i = 0; i < g_filter_count; i++) {
        if (g_filters[i].path_id == path_id &&
            g_filters[i].stage == stage &&
            g_filters[i].sub_idx == sub_idx) {
            if (g_filters[i].pass_idx == -1 || g_filters[i].pass_idx == pass_idx) {
                return true;
            }
        }
    }
    return false;
}

static bool pass_is_admitted_by_tuple_filter(
    long long path_id, const SerializedPass* pass,
    int serialized_pass_index) {
    if (!pass) return false;
    if (g_filter_count == 0) return true;
    for (int stage = 0; stage < 6; ++stage) {
        for (int subprogram = 0;
             subprogram < pass->subprogram_count[stage]; ++subprogram) {
            if (serialized_pass_subprogram_is_platform(
                    pass, stage, subprogram, 4) &&
                is_variant_filtered(path_id, stage, subprogram,
                                    serialized_pass_index)) {
                return true;
            }
        }
    }
    return false;
}

typedef struct {
    size_t shader_result_index;
    long long path_id;
    int stage;
    int sub_idx;
    int pass_idx;
    uint32_t failure_kinds;
} FailureRecord;

/* This gate is deliberately separate from the legacy per-subprogram loop.
 * A tuple filter can admit a pass, but an admitted pass is always certified
 * across its complete generated D3D11 state/tier domain. */
typedef struct {
    size_t eligible_passes;
    size_t filter_admitted_passes;
    size_t filter_skipped_passes;
    size_t plans_built;
    size_t snippets_uniquely_mapped;
    size_t certified_passes;
    size_t failed_passes;
    size_t active_stages;
    size_t generated_states;
    size_t planned_compiles;
    size_t compile_attempts;
    size_t clean_compiles;
    size_t matched_dxbc_containers;
    size_t runtime_binding_attested_compiles;
    size_t runtime_binding_compatible_compiles;
    size_t compiler_diagnostics;
    size_t diagnostic_attestation_compiles;
    size_t diagnostic_attested_compiles;
    size_t diagnostic_attested_actionable;
} GeneratedDomainCounts;

typedef enum {
    GENERATED_DOMAIN_PASS_NOT_REACHED = 0,
    GENERATED_DOMAIN_PASS_FILTERED_OUT,
    GENERATED_DOMAIN_PASS_PLAN_FAILED,
    GENERATED_DOMAIN_PASS_SNIPPET_MAPPING_FAILED,
    GENERATED_DOMAIN_PASS_CERTIFICATION_FAILED,
    GENERATED_DOMAIN_PASS_CERTIFIED,
} GeneratedDomainPassStatus;

typedef struct {
    size_t shader_result_index;
    long long path_id;
    int subshader_index;
    int local_pass_index;
    int serialized_pass_index;
    GeneratedDomainPassStatus status;
    bool filter_admitted;
    bool plan_attempted;
    bool snippet_mapping_attempted;
    bool certification_attempted;
    int generated_snippet_index;
    ShaderLabVariantPlanStatus plan_status;
    ShaderLabVariantPlanDiagnostic plan_diagnostic;
    UnityGeneratedDomainStatus certification_status;
    UnityGeneratedGLSLStatus glsl_status;
    size_t active_stage_count;
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
    int failure_stage_index;
    int32_t failure_compiler_program;
    int failure_hardware_tier_group;
    size_t failure_generated_state_index;
    size_t failure_aliased_state_index;
    int failure_subprogram_index;
    UnityGeneratedDomainKeywordFamily failure_keyword_family;
    size_t failure_contract_row_index;
    UnityCompileAuthorityStatus failure_compile_authority_status;
    bool failure_compiler_terminal_available;
    bool failure_compiler_terminal_success;
    bool failure_compiler_terminal_from_cache;
    size_t failure_compiler_terminal_diagnostic_count;
    DXBCCompareResult failure_dxbc_compare;
    UnityReflectionCertificateStatus failure_reflection_status;
    UnityReflectionCertificateAuthority failure_reflection_authority;
    size_t failure_reflection_expected_count;
    size_t failure_reflection_observed_count;
    size_t failure_reflection_matched_count;
    size_t failure_reflection_expected_index;
    size_t failure_reflection_observed_index;
    UnityReflectionCertificateRecordSummary failure_reflection_expected_record;
    UnityReflectionCertificateRecordSummary failure_reflection_observed_record;
} GeneratedDomainPassRecord;

/* GLCore is certified against the linked program stored in the released
 * player archive.  It is deliberately not inferred from D3D11 and never
 * requires the original ShaderLab source. */
typedef struct {
    size_t objects_evaluated;
    size_t platform_present_objects;
    size_t platform_absent_objects;
    size_t unavailable_objects;
    size_t targets_discovered;
    size_t targets_expected;
    size_t targets_filtered_out;
    size_t targets_opened;
    size_t compile_attempts;
    size_t targets_compiled;
    size_t targets_exact;
    size_t targets_mismatched;
    size_t targets_unavailable;
    size_t oracle_pack_v4_unsupported;
} SerializedGLCoreCounts;

typedef enum {
    SERIALIZED_GLCORE_VERIFY_FILTERED_OUT = 0,
    SERIALIZED_GLCORE_VERIFY_TARGET_UNAVAILABLE,
    SERIALIZED_GLCORE_VERIFY_SNIPPET_UNAVAILABLE,
    SERIALIZED_GLCORE_VERIFY_ORACLE_PACK_V4_UNSUPPORTED,
    SERIALIZED_GLCORE_VERIFY_COMPILE_AUTHORITY_UNAVAILABLE,
    SERIALIZED_GLCORE_VERIFY_COMPILE_FAILED,
    SERIALIZED_GLCORE_VERIFY_TEXT_MISMATCH,
    SERIALIZED_GLCORE_VERIFY_EXACT,
} SerializedGLCoreVerificationStatus;

typedef struct {
    size_t shader_result_index;
    long long path_id;
    int subshader_index;
    int local_pass_index;
    int serialized_pass_index;
    int serialized_stage;
    int flattened_subprogram_index;
    int program_type;
    int hardware_tier_group;
    int inner_subprogram_index;
    int archive_entry_index;
    SerializedGLCoreVerificationStatus status;
    SerializedGLCoreTargetStatus target_status;
    UnityCompileAuthorityStatus compile_authority_status;
    GLCoreLinkCertificateStatus certificate_status;
    bool compile_attempted;
    bool compiler_terminal_success;
    size_t expected_size;
    size_t actual_size;
    size_t first_differing_byte;
} SerializedGLCoreTargetRecord;

/* One row is retained for every selected serialized Shader object, including
 * objects that fail before a name can be decoded.  This deliberately reports
 * compiled-artifact evidence; it must not be presented as whole-ShaderLab or
 * visual certification because this verifier does not render the result or
 * prove Unity's runtime keyword-selection behavior. */
typedef struct {
    char* outer_path;
    char* serialized_member;
    size_t member_index;
    bool is_bundle_member;
    size_t serialized_source_index;
    char serialized_file_sha256[COMMON_SHA256_HEX_SIZE];
    long long path_id;
    char* shader_name;
    char* generated_source_path;
    const char* terminal_phase;
    bool parsed;
    bool original_source_unique;
    bool original_preprocess_ok;
    bool generated_source_present;
    bool generated_preprocess_ok;
    bool d3d11_archive_ok;
    bool glcore_readiness_evaluated;
    SerializedGLCoreTargetStatus glcore_readiness_status;
    int dxbc_expected;
    int unsupported_stage_variants;
    int scheduled_and_decoded;
    int dxbc_compiled;
    int dxbc_token_matched;
    int dxbc_byte_matched;
    SerializedGLCoreCounts serialized_glcore;
    size_t compiler_diagnostic_count;
    size_t informational_compiler_diagnostic_count;
    size_t actionable_compiler_diagnostic_count;
    size_t attested_actionable_compiler_diagnostic_count;
    size_t unattested_actionable_compiler_diagnostic_count;
    size_t preprocess_diagnostic_count;
    size_t compile_diagnostic_count;
    size_t disassemble_diagnostic_count;
    size_t diagnostic_authority_failure_count;
    GeneratedDomainCounts generated_domain;
    uint32_t failure_kinds;
} ShaderVerificationResult;

typedef struct {
    char* outer_path;
    char* member_name;
    size_t member_index;
    bool is_bundle_member;
    char serialized_file_sha256[COMMON_SHA256_HEX_SIZE];
    bool metadata_parsed;
    bool shader_schema_required;
    bool shader_schema_resolved;
    size_t shader_objects;
    size_t compute_shader_objects;
} VerificationSerializedSource;

typedef struct {
    size_t serialized_source_index;
    long long path_id;
    uint32_t object_size;
} UnsupportedComputeShader;

typedef struct {
    size_t requested_inputs;
    size_t discovered_files;
    size_t ignored_unrelated_descendants;
    size_t unityfs_files;
    size_t standalone_serialized_files;
    size_t serialized_sources;
    size_t resource_members;
    size_t directory_members;
    size_t deleted_members;
    size_t compute_shader_objects;
} VerificationInputStats;

enum {
    FAILURE_DXBC_COMPILE = 1u << 0,
    FAILURE_DXBC_DISASSEMBLE = 1u << 1,
    FAILURE_DXBC_MISMATCH = 1u << 2,
    FAILURE_GLSL_ORIGINAL_COMPILE = 1u << 3,
    FAILURE_GLSL_GENERATED_COMPILE = 1u << 4,
    FAILURE_GLSL_DISASSEMBLE = 1u << 5,
    FAILURE_GLSL_MISMATCH = 1u << 6,
    FAILURE_GLSL_ORACLE_MAPPING = 1u << 7,
    FAILURE_COMPILE_AUTHORITY = 1u << 8,
    FAILURE_ORACLE_AUTHORITY = 1u << 9,
    FAILURE_PREPROCESS_DIAGNOSTIC = 1u << 10,
    FAILURE_COMPILE_DIAGNOSTIC = 1u << 11,
    FAILURE_DISASSEMBLE_DIAGNOSTIC = 1u << 12,
    FAILURE_DIAGNOSTIC_AUTHORITY = 1u << 13,
    FAILURE_GENERATED_DOMAIN = 1u << 14,
    FAILURE_RUNTIME_BINDING = 1u << 15,
    FAILURE_GLCORE_TARGET_UNAVAILABLE = 1u << 16,
    FAILURE_GLCORE_ORACLE_UNSUPPORTED = 1u << 17,
};

typedef enum {
    VERIFICATION_DIAGNOSTIC_PREPROCESS = 0,
    VERIFICATION_DIAGNOSTIC_COMPILE,
    VERIFICATION_DIAGNOSTIC_DISASSEMBLE,
} VerificationDiagnosticOperation;

typedef struct {
    uint64_t response_sequence;
    size_t response_diagnostic_index;
    size_t shader_result_index;
    long long path_id;
    int stage;
    int sub_idx;
    int pass_idx;
    VerificationDiagnosticOperation operation;
    char* phase;
    bool compiler_success;
    bool from_cache;
    bool actionable;
    bool source_equivalent_attested;
    uint64_t diagnostic_attestation_id;
    UnityGeneratedDiagnosticParityStatus diagnostic_parity_status;
    char* attestation_role;
    int32_t fields[3];
    char* record;
    char* file;
    char* message;
} VerificationCompilerDiagnostic;

static FailureRecord* g_failures = NULL;
static int g_failure_count = 0;
static int g_failure_capacity = 0;
static pthread_mutex_t g_failures_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char* g_failure_dir = "failed_shaders";
static bool g_save_failure_artifacts = false;
static ShaderVerificationResult* g_shader_results = NULL;
static size_t g_shader_result_count = 0U;
static size_t g_shader_result_capacity = 0U;
static _Thread_local size_t g_thread_shader_result_index = SIZE_MAX;
static bool g_shader_report_allocation_failed = false;
static VerificationSerializedSource* g_serialized_sources = NULL;
static size_t g_serialized_source_count = 0U;
static size_t g_serialized_source_capacity = 0U;
static UnsupportedComputeShader* g_unsupported_compute_shaders = NULL;
static size_t g_unsupported_compute_shader_count = 0U;
static size_t g_unsupported_compute_shader_capacity = 0U;
static bool g_input_report_allocation_failed = false;
static VerificationInputStats g_input_stats;
static VerificationCompilerDiagnostic* g_compiler_diagnostics = NULL;
static size_t g_compiler_diagnostic_count = 0U;
static size_t g_compiler_diagnostic_capacity = 0U;
static size_t g_compiler_diagnostic_observed_count = 0U;
static size_t g_compiler_diagnostic_informational_count = 0U;
static size_t g_compiler_diagnostic_actionable_count = 0U;
static size_t g_compiler_diagnostic_attested_actionable_count = 0U;
static size_t g_compiler_diagnostic_unattested_actionable_count = 0U;
static uint64_t g_compiler_diagnostic_attestation_sequence = 1U;
static size_t g_diagnostic_authority_failure_count = 0U;
static bool g_compiler_diagnostic_allocation_failed = false;
static uint64_t g_compiler_diagnostic_response_sequence = 0U;
static pthread_mutex_t g_compiler_diagnostic_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_compiler_response_record_mutex = PTHREAD_MUTEX_INITIALIZER;
static GeneratedDomainCounts g_generated_domain;
static GeneratedDomainPassRecord* g_generated_domain_passes = NULL;
static size_t g_generated_domain_pass_count = 0U;
static size_t g_generated_domain_pass_capacity = 0U;
static bool g_generated_domain_report_allocation_failed = false;
static SerializedGLCoreCounts g_serialized_glcore;
static SerializedGLCoreTargetRecord* g_serialized_glcore_targets = NULL;
static size_t g_serialized_glcore_target_count = 0U;
static size_t g_serialized_glcore_target_capacity = 0U;
static bool g_serialized_glcore_report_allocation_failed = false;

static void record_failure_at(
    size_t shader_result_index, long long path_id, int stage, int sub_idx,
    int pass_idx, uint32_t failure_kind) {
    pthread_mutex_lock(&g_failures_mutex);
    if (shader_result_index < g_shader_result_count) {
        g_shader_results[shader_result_index].failure_kinds |=
            failure_kind;
    }
    for (int i = 0; i < g_failure_count; i++) {
        if (g_failures[i].shader_result_index ==
                shader_result_index &&
            g_failures[i].path_id == path_id &&
            g_failures[i].stage == stage &&
            g_failures[i].sub_idx == sub_idx &&
            g_failures[i].pass_idx == pass_idx) {
            g_failures[i].failure_kinds |= failure_kind;
            pthread_mutex_unlock(&g_failures_mutex);
            return;
        }
    }
    if (g_failure_count >= g_failure_capacity) {
        g_failure_capacity = g_failure_capacity == 0 ? 128 : g_failure_capacity * 2;
        FailureRecord* new_failures = realloc(g_failures, g_failure_capacity * sizeof(FailureRecord));
        if (!new_failures) {
            fprintf(stderr, "Out of memory recording failures\n");
            exit(1);
        }
        g_failures = new_failures;
    }
    g_failures[g_failure_count].shader_result_index =
        shader_result_index;
    g_failures[g_failure_count].path_id = path_id;
    g_failures[g_failure_count].stage = stage;
    g_failures[g_failure_count].sub_idx = sub_idx;
    g_failures[g_failure_count].pass_idx = pass_idx;
    g_failures[g_failure_count].failure_kinds = failure_kind;
    g_failure_count++;
    pthread_mutex_unlock(&g_failures_mutex);
}

static void record_failure(long long path_id, int stage, int sub_idx,
                           int pass_idx, uint32_t failure_kind) {
    record_failure_at(g_thread_shader_result_index, path_id, stage, sub_idx,
                      pass_idx, failure_kind);
}
static void verify_subprogram_shaderlab(
    UnityCompilerBroker* broker,
    const SerializedShader* shader,
    const PlayerSubProgramMetadata* sub_meta,
    long long path_id,
    int stage,
    int sub_idx,
    int pass_idx,
    int subshader_idx,
    int local_pass_idx,
    const SerializedPass* pass,
    const SerializedSubProgramIdentity* identity,
    const char* generated_source_path,
    const PreprocessResult* gen_prep,
    const PreprocessResult* orig_prep,
    const char* shaderlab_content,
    const OracleMetadataNormalization* oracle_metadata
);

static void record_unavailable_shader_variants(
    const SerializedShader* shader, long long path_id, uint32_t failure_kind) {
    int serialized_pass_index = 0;
    for (int subshader_idx = 0; subshader_idx < shader->subshader_count;
         subshader_idx++) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_idx];
        for (int pass_idx = 0; pass_idx < subshader->pass_count;
             pass_idx++, serialized_pass_index++) {
            const SerializedPass* pass = &subshader->passes[pass_idx];
            for (int stage = 0; stage < 5; stage++) {
                for (int sub = 0; sub < pass->subprogram_count[stage]; sub++) {
                    if (serialized_pass_subprogram_is_platform(pass, stage,
                                                               sub, 4) &&
                        is_variant_filtered(path_id, stage, sub,
                                            serialized_pass_index)) {
                        record_failure(path_id, stage, sub,
                                       serialized_pass_index, failure_kind);
                    }
                }
            }
        }
    }
}

static void debug_serialized_glsl_archive(const SerializedShader* shader,
                                          const TypeTreeValue* shader_value) {
    if (!getenv("DXBC_DEBUG_GLSL_ARCHIVE")) return;
    ShaderBlobArchive archive;
    if (!shader_blob_archive_open(shader_value, 15, &archive)) {
        fprintf(stderr, "    [GLSL Archive] platform 15 unavailable\n");
        return;
    }
    for (int subshader = 0; subshader < shader->subshader_count;
         subshader++) {
        for (int pass_index = 0;
             pass_index < shader->subshaders[subshader].pass_count;
             pass_index++) {
            const SerializedPass* pass =
                &shader->subshaders[subshader].passes[pass_index];
            for (int stage = 0; stage < 5; stage++) {
                for (int sub = 0; sub < pass->subprogram_count[stage]; sub++) {
                    if (!serialized_pass_subprogram_is_platform(pass, stage,
                                                                sub, 15)) {
                        continue;
                    }
                    const SerializedSubProgram* program =
                        &pass->subprograms[stage][sub];
                    const uint8_t* payload = NULL;
                    size_t payload_size = 0;
                    if (!shader_blob_archive_get(&archive,
                                                 program->blob_index,
                                                 &payload, &payload_size)) {
                        continue;
                    }
                    ByteStream stream;
                    stream_init(&stream, payload, payload_size);
                    stream_set_endian(&stream, false);
                    PlayerSubProgramMetadata metadata;
                    if (!subprogram_metadata_parse_variant(&stream,
                                                           &metadata)) {
                        continue;
                    }
                    const SerializedSubProgramIdentity* identity =
                        &pass->subprogram_identities[stage][sub];
                    fprintf(stderr,
                            "    [GLSL Archive] pass=%d stage=%d sub=%d "
                            "tier_group=%d inner=%d blob=%d tier=%d "
                            "header={%u,%u,%u,%u} source_map=%u req=%" PRIu64 " "
                            "bytes=%u magic=%02x%02x%02x%02x\n",
                            pass_index, stage, sub,
                            identity->hardware_tier_group,
                            identity->inner_subprogram_index,
                            program->blob_index,
                            program->has_hardware_tier
                                ? program->hardware_tier
                                : -1,
                            metadata.player_header_words[0],
                            metadata.player_header_words[1],
                            metadata.player_header_words[2],
                            metadata.player_header_words[3],
                            metadata.source_map,
                            program->shader_requirements,
                            metadata.bytecode_length,
                            metadata.bytecode_length > 0
                                ? metadata.bytecode[0]
                                : 0,
                            metadata.bytecode_length > 1
                                ? metadata.bytecode[1]
                                : 0,
                            metadata.bytecode_length > 2
                                ? metadata.bytecode[2]
                                : 0,
                            metadata.bytecode_length > 3
                                ? metadata.bytecode[3]
                                : 0);
                    subprogram_metadata_free_variant(&metadata);
                }
            }
        }
    }
    shader_blob_archive_close(&archive);
}

/* Expected counts come from the authoritative serialized bundle, never from
 * successfully emitted/preprocessed files.  Otherwise omitting a shader can
 * shrink the denominator and make an incomplete reconstruction look exact. */
static int g_dxbc_expected = 0;
static int g_dxbc_unsupported_stage = 0;
static int g_shaders_total = 0;
static int g_shader_objects_seen = 0;
static int g_shader_objects_parsed = 0;

// DXBC native statistics
static int g_dxbc_compiled = 0;
static int g_dxbc_token_matched = 0;
static int g_dxbc_matched = 0;

static bool g_verify_glsl = true;
/* The complete generated pass-domain certificate is the primary D3D11
 * proof.  The legacy serialized-subprogram census is useful for exhaustive
 * debugging, but it recompiles many rows already covered by that certificate.
 * It remains enabled by default for backwards compatibility. */
static bool g_direct_census_enabled = true;
/* `--direct-only` retains the generated preprocess response solely because
 * its exact snippet source and contract are required inputs to compileSnippet.
 * It deliberately disables generated-domain discovery/certification and all
 * original-source preprocessing. */
static bool g_generated_domain_enabled = true;
static bool g_direct_only_enabled = false;
static size_t g_generated_preprocess_authority_requests = 0U;
static size_t g_original_preprocess_authority_requests = 0U;

typedef struct {
    char* name;
    char* path;
} OriginalShaderMapping;

static OriginalShaderMapping* g_original_shaders = NULL;
static size_t g_original_shader_count = 0U;
static size_t g_original_shader_capacity = 0U;

static char* copy_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1U);
    if (!copy) return NULL;
    memcpy(copy, value, size + 1U);
    return copy;
}

typedef struct {
    const char* phase;
    size_t shader_result_index;
    long long path_id;
    int stage;
    int sub_idx;
    int pass_idx;
    VerificationDiagnosticOperation operation;
    bool source_equivalent_attested;
    uint64_t diagnostic_attestation_id;
    UnityGeneratedDiagnosticParityStatus diagnostic_parity_status;
    const char* attestation_role;
} VerificationDiagnosticContext;

static const char* verification_diagnostic_operation_name(
    VerificationDiagnosticOperation operation) {
    switch (operation) {
        case VERIFICATION_DIAGNOSTIC_PREPROCESS: return "preprocess";
        case VERIFICATION_DIAGNOSTIC_COMPILE: return "compile";
        case VERIFICATION_DIAGNOSTIC_DISASSEMBLE: return "disassemble";
        default: return "invalid";
    }
}

static uint32_t verification_diagnostic_failure_kind(
    VerificationDiagnosticOperation operation) {
    switch (operation) {
        case VERIFICATION_DIAGNOSTIC_PREPROCESS:
            return FAILURE_PREPROCESS_DIAGNOSTIC;
        case VERIFICATION_DIAGNOSTIC_COMPILE:
            return FAILURE_COMPILE_DIAGNOSTIC;
        case VERIFICATION_DIAGNOSTIC_DISASSEMBLE:
            return FAILURE_DISASSEMBLE_DIAGNOSTIC;
        default:
            return FAILURE_DIAGNOSTIC_AUTHORITY;
    }
}

static void note_shader_diagnostics_locked(
    size_t shader_result_index, VerificationDiagnosticOperation operation,
    size_t informational_count, size_t actionable_count,
    bool source_equivalent_attested) {
    if (shader_result_index >= g_shader_result_count) return;
    ShaderVerificationResult* result =
        &g_shader_results[shader_result_index];
    const size_t count = informational_count + actionable_count;
    result->compiler_diagnostic_count += count;
    result->informational_compiler_diagnostic_count += informational_count;
    result->actionable_compiler_diagnostic_count += actionable_count;
    if (source_equivalent_attested) {
        result->attested_actionable_compiler_diagnostic_count +=
            actionable_count;
    } else {
        result->unattested_actionable_compiler_diagnostic_count +=
            actionable_count;
    }
    switch (operation) {
        case VERIFICATION_DIAGNOSTIC_PREPROCESS:
            result->preprocess_diagnostic_count += count;
            break;
        case VERIFICATION_DIAGNOSTIC_COMPILE:
            result->compile_diagnostic_count += count;
            break;
        case VERIFICATION_DIAGNOSTIC_DISASSEMBLE:
            result->disassemble_diagnostic_count += count;
            break;
        default:
            break;
    }
}

static bool append_compiler_diagnostic(
    const VerificationDiagnosticContext* context,
    const UnityCompilerResponseStatus* status,
    const UnityCompilerDiagnostic* source, uint64_t response_sequence,
    size_t response_diagnostic_index) {
    if (!context || !context->phase || !status || !source ||
        !source->record || !source->file || !source->message) {
        return false;
    }

    VerificationCompilerDiagnostic diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.response_sequence = response_sequence;
    diagnostic.response_diagnostic_index = response_diagnostic_index;
    diagnostic.shader_result_index = context->shader_result_index;
    diagnostic.path_id = context->path_id;
    diagnostic.stage = context->stage;
    diagnostic.sub_idx = context->sub_idx;
    diagnostic.pass_idx = context->pass_idx;
    diagnostic.operation = context->operation;
    diagnostic.compiler_success = status->compiler_success;
    diagnostic.from_cache = status->from_cache;
    diagnostic.actionable =
        unity_compiler_diagnostic_is_actionable(source);
    diagnostic.source_equivalent_attested =
        context->source_equivalent_attested;
    diagnostic.diagnostic_attestation_id =
        context->diagnostic_attestation_id;
    diagnostic.diagnostic_parity_status =
        context->diagnostic_parity_status;
    memcpy(diagnostic.fields, source->fields, sizeof(diagnostic.fields));
    diagnostic.phase = copy_string(context->phase);
    diagnostic.attestation_role = copy_string(
        context->attestation_role ? context->attestation_role : "none");
    diagnostic.record = copy_string(source->record);
    diagnostic.file = copy_string(source->file);
    diagnostic.message = copy_string(source->message);
    if (!diagnostic.phase || !diagnostic.attestation_role ||
        !diagnostic.record || !diagnostic.file ||
        !diagnostic.message) {
        free(diagnostic.phase);
        free(diagnostic.attestation_role);
        free(diagnostic.record);
        free(diagnostic.file);
        free(diagnostic.message);
        return false;
    }

    pthread_mutex_lock(&g_compiler_diagnostic_mutex);
    if (g_compiler_diagnostic_count == g_compiler_diagnostic_capacity) {
        size_t capacity = g_compiler_diagnostic_capacity == 0U
            ? 32U : g_compiler_diagnostic_capacity * 2U;
        if (capacity < g_compiler_diagnostic_capacity ||
            capacity > SIZE_MAX / sizeof(*g_compiler_diagnostics)) {
            pthread_mutex_unlock(&g_compiler_diagnostic_mutex);
            free(diagnostic.phase);
            free(diagnostic.attestation_role);
            free(diagnostic.record);
            free(diagnostic.file);
            free(diagnostic.message);
            return false;
        }
        VerificationCompilerDiagnostic* records = realloc(
            g_compiler_diagnostics, capacity * sizeof(*records));
        if (!records) {
            pthread_mutex_unlock(&g_compiler_diagnostic_mutex);
            free(diagnostic.phase);
            free(diagnostic.attestation_role);
            free(diagnostic.record);
            free(diagnostic.file);
            free(diagnostic.message);
            return false;
        }
        g_compiler_diagnostics = records;
        g_compiler_diagnostic_capacity = capacity;
    }
    g_compiler_diagnostics[g_compiler_diagnostic_count++] = diagnostic;
    pthread_mutex_unlock(&g_compiler_diagnostic_mutex);
    return true;
}

static bool retain_and_report_compiler_diagnostics(
    const VerificationDiagnosticContext* context,
    const UnityCompilerResponseStatus* status) {
    if (!context || !status || status->diagnostic_count == 0U) return true;

    size_t actionable_count =
        unity_compiler_response_status_actionable_diagnostic_count(status);
    size_t informational_count =
        status->diagnostic_count - actionable_count;
    bool retained = true;
    pthread_mutex_lock(&g_compiler_response_record_mutex);
    uint64_t response_sequence =
        g_compiler_diagnostic_response_sequence++;
    pthread_mutex_lock(&g_compiler_diagnostic_mutex);
    g_compiler_diagnostic_observed_count += status->diagnostic_count;
    g_compiler_diagnostic_informational_count += informational_count;
    g_compiler_diagnostic_actionable_count += actionable_count;
    if (context->source_equivalent_attested) {
        g_compiler_diagnostic_attested_actionable_count += actionable_count;
    } else {
        g_compiler_diagnostic_unattested_actionable_count += actionable_count;
    }
    pthread_mutex_unlock(&g_compiler_diagnostic_mutex);
    pthread_mutex_lock(&g_failures_mutex);
    note_shader_diagnostics_locked(
        context->shader_result_index, context->operation,
        informational_count, actionable_count,
        context->source_equivalent_attested);
    pthread_mutex_unlock(&g_failures_mutex);
    pthread_mutex_lock(&g_stats_mutex);
    for (size_t i = 0U; i < status->diagnostic_count; ++i) {
        const UnityCompilerDiagnostic* diagnostic = &status->diagnostics[i];
        fprintf(stderr,
                "    [UNITY COMPILER %s] operation=%s phase=%s "
                "shader_result=%zu path_id=%lld stage=%d subprogram=%d "
                "pass=%d "
                "attestation=%s attestation_id=%" PRIu64 " role=%s "
                "terminal_success=%s from_cache=%s "
                "fields={%" PRId32 ",%" PRId32 ",%" PRId32 "}\n"
                "      record=%s\n      file=%s\n      message=%s\n",
                unity_compiler_diagnostic_is_actionable(diagnostic)
                    ? "DIAGNOSTIC" : "INFORMATION",
                verification_diagnostic_operation_name(context->operation),
                context->phase, context->shader_result_index,
                context->path_id, context->stage,
                context->sub_idx, context->pass_idx,
                context->source_equivalent_attested ? "source-equivalent"
                                                    : "none",
                context->diagnostic_attestation_id,
                context->attestation_role ? context->attestation_role
                                          : "none",
                status->compiler_success ? "true" : "false",
                status->from_cache ? "true" : "false",
                diagnostic->fields[0], diagnostic->fields[1],
                diagnostic->fields[2], diagnostic->record,
                diagnostic->file, diagnostic->message);
    }
    pthread_mutex_unlock(&g_stats_mutex);
    for (size_t i = 0U; i < status->diagnostic_count; ++i) {
        if (!append_compiler_diagnostic(context, status,
                                        &status->diagnostics[i],
                                        response_sequence, i)) {
            retained = false;
        }
    }
    pthread_mutex_unlock(&g_compiler_response_record_mutex);
    if (!retained) {
        pthread_mutex_lock(&g_compiler_diagnostic_mutex);
        g_compiler_diagnostic_allocation_failed = true;
        pthread_mutex_unlock(&g_compiler_diagnostic_mutex);
        record_failure_at(
            context->shader_result_index, context->path_id, context->stage,
            context->sub_idx, context->pass_idx,
            FAILURE_DIAGNOSTIC_AUTHORITY);
        return false;
    }
    if (actionable_count != 0U &&
        !context->source_equivalent_attested) {
        record_failure_at(
            context->shader_result_index, context->path_id, context->stage,
            context->sub_idx, context->pass_idx,
            verification_diagnostic_failure_kind(context->operation));
        return false;
    }
    return true;
}

static void record_diagnostic_authority_failure(
    const VerificationDiagnosticContext* context, const char* reason) {
    if (!context) return;
    pthread_mutex_lock(&g_compiler_diagnostic_mutex);
    g_diagnostic_authority_failure_count++;
    pthread_mutex_unlock(&g_compiler_diagnostic_mutex);
    pthread_mutex_lock(&g_failures_mutex);
    if (context->shader_result_index < g_shader_result_count) {
        g_shader_results[context->shader_result_index]
            .diagnostic_authority_failure_count++;
    }
    pthread_mutex_unlock(&g_failures_mutex);
    pthread_mutex_lock(&g_stats_mutex);
    fprintf(stderr,
            "    [DIAGNOSTIC AUTHORITY UNAVAILABLE] operation=%s phase=%s "
            "shader_result=%zu path_id=%lld stage=%d subprogram=%d "
            "pass=%d: %s\n",
            verification_diagnostic_operation_name(context->operation),
            context->phase, context->shader_result_index,
            context->path_id, context->stage,
            context->sub_idx, context->pass_idx,
            reason ? reason : "diagnostic provenance is unavailable");
    pthread_mutex_unlock(&g_stats_mutex);
    record_failure_at(
        context->shader_result_index, context->path_id, context->stage,
        context->sub_idx, context->pass_idx,
        FAILURE_DIAGNOSTIC_AUTHORITY);
}

static void free_compiler_diagnostics(void) {
    for (size_t i = 0U; i < g_compiler_diagnostic_count; ++i) {
        free(g_compiler_diagnostics[i].phase);
        free(g_compiler_diagnostics[i].attestation_role);
        free(g_compiler_diagnostics[i].record);
        free(g_compiler_diagnostics[i].file);
        free(g_compiler_diagnostics[i].message);
    }
    free(g_compiler_diagnostics);
    g_compiler_diagnostics = NULL;
    g_compiler_diagnostic_count = 0U;
    g_compiler_diagnostic_capacity = 0U;
    g_compiler_diagnostic_observed_count = 0U;
    g_compiler_diagnostic_informational_count = 0U;
    g_compiler_diagnostic_actionable_count = 0U;
    g_compiler_diagnostic_attested_actionable_count = 0U;
    g_compiler_diagnostic_unattested_actionable_count = 0U;
    g_compiler_diagnostic_attestation_sequence = 1U;
}

static const char* generated_domain_pass_status_name(
    GeneratedDomainPassStatus status) {
    switch (status) {
        case GENERATED_DOMAIN_PASS_NOT_REACHED: return "not-reached";
        case GENERATED_DOMAIN_PASS_FILTERED_OUT: return "filtered-out";
        case GENERATED_DOMAIN_PASS_PLAN_FAILED: return "plan-failed";
        case GENERATED_DOMAIN_PASS_SNIPPET_MAPPING_FAILED:
            return "snippet-mapping-failed";
        case GENERATED_DOMAIN_PASS_CERTIFICATION_FAILED:
            return "certification-failed";
        case GENERATED_DOMAIN_PASS_CERTIFIED: return "exact";
        default: return "invalid";
    }
}

static GeneratedDomainPassRecord* append_generated_domain_pass_record(
    size_t shader_result_index, long long path_id, int subshader_index,
    int local_pass_index, int serialized_pass_index) {
    if (g_generated_domain_pass_count ==
        g_generated_domain_pass_capacity) {
        const size_t capacity = g_generated_domain_pass_capacity == 0U
            ? 32U : g_generated_domain_pass_capacity * 2U;
        if (capacity < g_generated_domain_pass_capacity ||
            capacity > SIZE_MAX / sizeof(*g_generated_domain_passes)) {
            g_generated_domain_report_allocation_failed = true;
            return NULL;
        }
        GeneratedDomainPassRecord* records = realloc(
            g_generated_domain_passes, capacity * sizeof(*records));
        if (!records) {
            g_generated_domain_report_allocation_failed = true;
            return NULL;
        }
        g_generated_domain_passes = records;
        g_generated_domain_pass_capacity = capacity;
    }
    GeneratedDomainPassRecord* record =
        &g_generated_domain_passes[g_generated_domain_pass_count++];
    memset(record, 0, sizeof(*record));
    record->shader_result_index = shader_result_index;
    record->path_id = path_id;
    record->subshader_index = subshader_index;
    record->local_pass_index = local_pass_index;
    record->serialized_pass_index = serialized_pass_index;
    record->generated_snippet_index = -1;
    record->plan_status = SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT;
    record->plan_diagnostic.status =
        SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT;
    record->plan_diagnostic.stage_index = -1;
    record->plan_diagnostic.subprogram_index = -1;
    record->plan_diagnostic.conflicting_subprogram_index = -1;
    record->plan_diagnostic.raw_keyword_index = -1;
    record->certification_status = UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT;
    record->glsl_status =
        UNITY_GENERATED_GLSL_UNAVAILABLE_NO_PRECISION_AUTHORITY;
    record->failure_stage_index = -1;
    record->failure_compiler_program = -1;
    record->failure_hardware_tier_group = -1;
    record->failure_generated_state_index = SIZE_MAX;
    record->failure_aliased_state_index = SIZE_MAX;
    record->failure_subprogram_index = -1;
    record->failure_keyword_family = UNITY_GENERATED_DOMAIN_FAMILY_NONE;
    record->failure_contract_row_index = SIZE_MAX;
    record->failure_compile_authority_status = UNITY_COMPILE_AUTHORITY_OK;
    dxbc_compare_result_init(&record->failure_dxbc_compare);
    record->failure_reflection_status =
        UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT;
    record->failure_reflection_authority =
        UNITY_REFLECTION_AUTHORITY_COMMON_ONLY;
    record->failure_reflection_expected_index = SIZE_MAX;
    record->failure_reflection_observed_index = SIZE_MAX;
    return record;
}

static void generated_domain_counts_add_report(
    GeneratedDomainCounts* counts,
    const UnityGeneratedDomainReport* report) {
    if (!counts || !report) return;
    counts->active_stages += report->active_stage_count;
    counts->generated_states += report->generated_state_count;
    counts->planned_compiles += report->planned_compile_count;
    counts->compile_attempts += report->compile_attempt_count;
    counts->clean_compiles += report->clean_compile_count;
    counts->matched_dxbc_containers += report->matched_dxbc_count;
    counts->runtime_binding_attested_compiles +=
        report->runtime_binding_attested_compile_count;
    counts->runtime_binding_compatible_compiles +=
        report->runtime_binding_compatible_compile_count;
    counts->compiler_diagnostics += report->compiler_diagnostic_count;
    counts->diagnostic_attestation_compiles +=
        report->diagnostic_attestation_compile_count;
    counts->diagnostic_attested_compiles +=
        report->diagnostic_attested_compile_count;
    counts->diagnostic_attested_actionable +=
        report->diagnostic_attested_actionable_count;
}

static uint32_t generated_domain_failure_mask(
    UnityGeneratedDomainStatus status) {
    uint32_t mask = FAILURE_GENERATED_DOMAIN;
    switch (status) {
        case UNITY_GENERATED_DOMAIN_INVALID_PROFILE:
        case UNITY_GENERATED_DOMAIN_PLAN_AUTHORITY_MISMATCH:
        case UNITY_GENERATED_DOMAIN_INVALID_CONTRACT:
        case UNITY_GENERATED_DOMAIN_MISSING_PROGRAM_CONTRACT:
        case UNITY_GENERATED_DOMAIN_COMPILE_AUTHORITY_FAILED:
        case UNITY_GENERATED_DOMAIN_REQUIREMENTS_MISMATCH:
            mask |= FAILURE_COMPILE_AUTHORITY;
            break;
        case UNITY_GENERATED_DOMAIN_COMPILER_TRANSPORT_FAILED:
        case UNITY_GENERATED_DOMAIN_COMPILER_REJECTED:
        case UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC:
        case UNITY_GENERATED_DOMAIN_DIAGNOSTIC_ATTESTATION_FAILED:
        case UNITY_GENERATED_DOMAIN_COMPILED_DXBC_INVALID:
            mask |= FAILURE_DXBC_COMPILE;
            break;
        case UNITY_GENERATED_DOMAIN_DXBC_MISMATCH:
            mask |= FAILURE_DXBC_MISMATCH;
            break;
        case UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID:
        case UNITY_GENERATED_DOMAIN_REFLECTION_MISMATCH:
            mask |= FAILURE_RUNTIME_BINDING;
            break;
        default:
            break;
    }
    return mask;
}

static void retain_generated_domain_compiler_responses(
    const UnityGeneratedDomainReport* report, size_t shader_result_index,
    long long path_id, int serialized_pass_index) {
    if (!report) return;
    for (size_t i = 0U; i < report->compiler_response_count; ++i) {
        const UnityGeneratedDomainCompilerResponseRecord* response =
            &report->compiler_responses[i];
        uint64_t attestation_id = 0U;
        const bool attested = response->diagnostic_parity_status ==
            UNITY_GENERATED_DIAGNOSTIC_PARITY_ATTESTED;
        if (attested) {
            pthread_mutex_lock(&g_compiler_diagnostic_mutex);
            if (g_compiler_diagnostic_attestation_sequence == UINT64_MAX) {
                g_compiler_diagnostic_allocation_failed = true;
            } else {
                attestation_id =
                    g_compiler_diagnostic_attestation_sequence++;
            }
            pthread_mutex_unlock(&g_compiler_diagnostic_mutex);
        }
        const VerificationDiagnosticContext generated_context = {
            .phase = "generated-domain-d3d11-certification",
            .shader_result_index = shader_result_index,
            .path_id = path_id,
            .stage = response->stage_index,
            .sub_idx = response->subprogram_index,
            .pass_idx = serialized_pass_index,
            .operation = VERIFICATION_DIAGNOSTIC_COMPILE,
            .source_equivalent_attested = attested,
            .diagnostic_attestation_id = attestation_id,
            .diagnostic_parity_status =
                response->diagnostic_parity_status,
            .attestation_role = attested ? "generated" : "none",
        };
        (void)retain_and_report_compiler_diagnostics(
            &generated_context, &response->response);
        if (response->original_response_present) {
            const VerificationDiagnosticContext original_context = {
                .phase = "original-source-diagnostic-attestation",
                .shader_result_index = shader_result_index,
                .path_id = path_id,
                .stage = response->stage_index,
                .sub_idx = response->subprogram_index,
                .pass_idx = serialized_pass_index,
                .operation = VERIFICATION_DIAGNOSTIC_COMPILE,
                .source_equivalent_attested = attested,
                .diagnostic_attestation_id = attestation_id,
                .diagnostic_parity_status =
                    response->diagnostic_parity_status,
                .attestation_role = "original",
            };
            (void)retain_and_report_compiler_diagnostics(
                &original_context, &response->original_response);
        }
    }
}

static void copy_generated_domain_report_to_pass_record(
    GeneratedDomainPassRecord* record,
    const UnityGeneratedDomainReport* report) {
    if (!record || !report) return;
    record->certification_status = report->status;
    record->glsl_status = report->glsl_status;
    record->active_stage_count = report->active_stage_count;
    record->generated_state_count = report->generated_state_count;
    record->planned_compile_count = report->planned_compile_count;
    record->compile_attempt_count = report->compile_attempt_count;
    record->clean_compile_count = report->clean_compile_count;
    record->matched_dxbc_count = report->matched_dxbc_count;
    record->runtime_binding_attested_compile_count =
        report->runtime_binding_attested_compile_count;
    record->runtime_binding_compatible_compile_count =
        report->runtime_binding_compatible_compile_count;
    record->compiler_diagnostic_count =
        report->compiler_diagnostic_count;
    record->diagnostic_attestation_compile_count =
        report->diagnostic_attestation_compile_count;
    record->diagnostic_attested_compile_count =
        report->diagnostic_attested_compile_count;
    record->diagnostic_attested_actionable_count =
        report->diagnostic_attested_actionable_count;
    record->failure_stage_index = report->diagnostic.stage_index;
    record->failure_compiler_program =
        report->diagnostic.compiler_program;
    record->failure_hardware_tier_group =
        report->diagnostic.hardware_tier_group;
    record->failure_generated_state_index =
        report->diagnostic.generated_state_index;
    record->failure_aliased_state_index =
        report->diagnostic.aliased_state_index;
    record->failure_subprogram_index =
        report->diagnostic.subprogram_index;
    record->failure_keyword_family =
        report->diagnostic.keyword_family;
    record->failure_contract_row_index =
        report->diagnostic.contract_row_index;
    record->failure_compile_authority_status =
        report->diagnostic.compile_authority_status;
    record->failure_compiler_terminal_available =
        report->status == UNITY_GENERATED_DOMAIN_COMPILER_REJECTED ||
        report->status == UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC ||
        report->status ==
            UNITY_GENERATED_DOMAIN_DIAGNOSTIC_ATTESTATION_FAILED;
    record->failure_compiler_terminal_success =
        report->diagnostic.compiler_response.compiler_success;
    record->failure_compiler_terminal_from_cache =
        report->diagnostic.compiler_response.from_cache;
    record->failure_compiler_terminal_diagnostic_count =
        report->diagnostic.compiler_response.diagnostic_count;
    record->failure_dxbc_compare = report->diagnostic.dxbc_compare;
    record->failure_reflection_status =
        report->diagnostic.reflection_certificate.status;
    record->failure_reflection_authority =
        report->diagnostic.reflection_certificate.authority;
    record->failure_reflection_expected_count =
        report->diagnostic.reflection_certificate.expected_record_count;
    record->failure_reflection_observed_count =
        report->diagnostic.reflection_certificate.observed_record_count;
    record->failure_reflection_matched_count =
        report->diagnostic.reflection_certificate.matched_record_count;
    record->failure_reflection_expected_index =
        report->diagnostic.reflection_certificate.expected_record_index;
    record->failure_reflection_observed_index =
        report->diagnostic.reflection_certificate.observed_record_index;
    record->failure_reflection_expected_record =
        report->diagnostic.reflection_certificate.expected_record;
    record->failure_reflection_observed_record =
        report->diagnostic.reflection_certificate.observed_record;
}

static GeneratedDomainPassRecord* find_generated_domain_pass_record(
    size_t shader_result_index, int serialized_pass_index) {
    for (size_t i = 0U; i < g_generated_domain_pass_count; ++i) {
        GeneratedDomainPassRecord* record =
            &g_generated_domain_passes[i];
        if (record->shader_result_index == shader_result_index &&
            record->serialized_pass_index == serialized_pass_index) {
            return record;
        }
    }
    return NULL;
}

static void discover_generated_d3d11_pass_domains(
    const SerializedShader* shader, long long path_id,
    size_t shader_result_index) {
    if (!shader) return;
    ShaderVerificationResult* shader_result =
        shader_result_index < g_shader_result_count
            ? &g_shader_results[shader_result_index] : NULL;
    int serialized_pass_index = 0;
    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; ++subshader_index) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        for (int local_pass_index = 0;
             local_pass_index < subshader->pass_count;
             ++local_pass_index, ++serialized_pass_index) {
            const SerializedPass* pass =
                &subshader->passes[local_pass_index];
            if (!unity_shaderlab_pass_emits_snippet(pass) ||
                !unity_shaderlab_pass_has_d3d11(pass)) {
                continue;
            }
            ++g_generated_domain.eligible_passes;
            if (shader_result) {
                ++shader_result->generated_domain.eligible_passes;
            }
            GeneratedDomainPassRecord* record =
                append_generated_domain_pass_record(
                    shader_result_index, path_id, subshader_index,
                    local_pass_index, serialized_pass_index);
            const bool admitted = pass_is_admitted_by_tuple_filter(
                path_id, pass, serialized_pass_index);
            if (admitted) {
                ++g_generated_domain.filter_admitted_passes;
                if (shader_result) {
                    ++shader_result->generated_domain
                         .filter_admitted_passes;
                }
                if (record) record->filter_admitted = true;
            } else {
                ++g_generated_domain.filter_skipped_passes;
                if (shader_result) {
                    ++shader_result->generated_domain
                         .filter_skipped_passes;
                }
                if (record) {
                    record->status =
                        GENERATED_DOMAIN_PASS_FILTERED_OUT;
                }
            }
        }
    }
}

static void certify_generated_d3d11_pass_domains(
    UnityCompilerBroker* broker, const SerializedShader* shader,
    const ShaderBlobArchive* archive, const PreprocessResult* generated,
    const char* generated_source_path, const PreprocessResult* original,
    const char* original_source_path, long long path_id,
    size_t shader_result_index) {
    if (!broker || !shader || !archive || !generated ||
        !generated_source_path) {
        return;
    }
    ShaderVerificationResult* shader_result =
        shader_result_index < g_shader_result_count
            ? &g_shader_results[shader_result_index] : NULL;
    int serialized_pass_index = 0;
    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; ++subshader_index) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        for (int local_pass_index = 0;
             local_pass_index < subshader->pass_count;
             ++local_pass_index, ++serialized_pass_index) {
            const SerializedPass* pass =
                &subshader->passes[local_pass_index];
            if (!unity_shaderlab_pass_emits_snippet(pass) ||
                !unity_shaderlab_pass_has_d3d11(pass)) {
                continue;
            }

            GeneratedDomainPassRecord* pass_record =
                find_generated_domain_pass_record(
                    shader_result_index, serialized_pass_index);
            const bool admitted = pass_is_admitted_by_tuple_filter(
                path_id, pass, serialized_pass_index);
            if (!admitted) {
                continue;
            }

            ShaderLabVariantPlan plan;
            ShaderLabVariantPlanDiagnostic plan_diagnostic;
            shaderlab_variant_plan_init(&plan);
            memset(&plan_diagnostic, 0, sizeof(plan_diagnostic));
            const ShaderLabVariantPlanStatus plan_status =
                shaderlab_variant_plan_build(
                    shader, pass, &plan, &plan_diagnostic);
            if (pass_record) {
                pass_record->plan_attempted = true;
                pass_record->plan_status = plan_status;
                pass_record->plan_diagnostic = plan_diagnostic;
            }
            if (plan_status != SHADERLAB_VARIANT_PLAN_OK) {
                ++g_generated_domain.failed_passes;
                if (shader_result) {
                    ++shader_result->generated_domain.failed_passes;
                }
                if (pass_record) {
                    pass_record->status =
                        GENERATED_DOMAIN_PASS_PLAN_FAILED;
                }
                fprintf(stderr,
                        "    [GENERATED DOMAIN FAIL] path_id=%lld pass=%d "
                        "plan=%s stage=%d subprogram=%d conflict=%d "
                        "keyword=%d\n",
                        path_id, serialized_pass_index,
                        shaderlab_variant_plan_status_name(plan_status),
                        plan_diagnostic.stage_index,
                        plan_diagnostic.subprogram_index,
                        plan_diagnostic.conflicting_subprogram_index,
                        plan_diagnostic.raw_keyword_index);
                record_failure_at(
                    shader_result_index, path_id,
                    plan_diagnostic.stage_index,
                    plan_diagnostic.subprogram_index,
                    serialized_pass_index, FAILURE_GENERATED_DOMAIN);
                shaderlab_variant_plan_free(&plan);
                continue;
            }
            ++g_generated_domain.plans_built;
            if (shader_result) {
                ++shader_result->generated_domain.plans_built;
            }

            int generated_snippet_index = -1;
            if (pass_record) {
                pass_record->snippet_mapping_attempted = true;
            }
            const PreprocessedSnippet* snippet =
                unity_shaderlab_find_generated_snippet(
                    shader, serialized_pass_index, generated,
                    &generated_snippet_index);
            if (pass_record) {
                pass_record->generated_snippet_index =
                    generated_snippet_index;
            }
            if (!snippet) {
                ++g_generated_domain.failed_passes;
                if (shader_result) {
                    ++shader_result->generated_domain.failed_passes;
                }
                if (pass_record) {
                    pass_record->status =
                        GENERATED_DOMAIN_PASS_SNIPPET_MAPPING_FAILED;
                }
                fprintf(stderr,
                        "    [GENERATED DOMAIN FAIL] path_id=%lld pass=%d "
                        "does not have a unique generated preprocess "
                        "snippet (serialized emitted passes=%d, "
                        "compiler snippets=%d)\n",
                        path_id, serialized_pass_index,
                        unity_shaderlab_snippet_count(shader),
                        generated->snippet_count);
                record_failure_at(
                    shader_result_index, path_id, -1, -1,
                    serialized_pass_index, FAILURE_GENERATED_DOMAIN);
                shaderlab_variant_plan_free(&plan);
                continue;
            }
            ++g_generated_domain.snippets_uniquely_mapped;
            if (shader_result) {
                ++shader_result->generated_domain
                     .snippets_uniquely_mapped;
            }

            const PreprocessedSnippet* original_snippet =
                unity_shaderlab_find_original_snippet(
                    shader, serialized_pass_index, original);

            UnityGeneratedDomainReport report;
            unity_generated_domain_report_init(&report);
            const UnityGeneratedDomainCertificationInput input = {
                .shader = shader,
                .pass = pass,
                .plan = &plan,
                .generated_snippet = snippet,
                .original_snippet = original_snippet,
                .d3d11_archive = archive,
                .compile_profile = &g_effective_compile_profile,
                .broker = broker,
                .source_directory = generated_source_path,
                .source_basename = shader->name,
                .pass_name = pass->name,
                .original_source_directory = original_snippet
                    ? original_source_path : NULL,
                .original_source_basename = original_snippet
                    ? shader->name : NULL,
            };
            if (pass_record) pass_record->certification_attempted = true;
            const UnityGeneratedDomainStatus certification_status =
                unity_generated_domain_certify_d3d11(&input, &report);
            generated_domain_counts_add_report(
                &g_generated_domain, &report);
            generated_domain_counts_add_report(
                shader_result ? &shader_result->generated_domain : NULL,
                &report);
            retain_generated_domain_compiler_responses(
                &report, shader_result_index, path_id,
                serialized_pass_index);
            copy_generated_domain_report_to_pass_record(
                pass_record, &report);

            if (certification_status == UNITY_GENERATED_DOMAIN_OK) {
                ++g_generated_domain.certified_passes;
                if (shader_result) {
                    ++shader_result->generated_domain.certified_passes;
                }
                if (pass_record) {
                    pass_record->status =
                        GENERATED_DOMAIN_PASS_CERTIFIED;
                }
                printf("    [GENERATED DOMAIN EXACT] path_id=%lld pass=%d "
                       "stages=%zu states=%zu compiles=%zu\n",
                       path_id, serialized_pass_index,
                       report.active_stage_count,
                       report.generated_state_count,
                       report.matched_dxbc_count);
            } else {
                ++g_generated_domain.failed_passes;
                if (shader_result) {
                    ++shader_result->generated_domain.failed_passes;
                }
                if (pass_record) {
                    pass_record->status =
                        GENERATED_DOMAIN_PASS_CERTIFICATION_FAILED;
                }
                fprintf(stderr,
                        "    [GENERATED DOMAIN FAIL] path_id=%lld pass=%d "
                        "status=%s stage=%d tier=%d state=%zu alias=%zu "
                        "subprogram=%d family=%d row=%zu authority=%s "
                        "dxbc=%s\n",
                        path_id, serialized_pass_index,
                        unity_generated_domain_status_name(
                            certification_status),
                        report.diagnostic.stage_index,
                        report.diagnostic.hardware_tier_group,
                        report.diagnostic.generated_state_index,
                        report.diagnostic.aliased_state_index,
                        report.diagnostic.subprogram_index,
                        (int)report.diagnostic.keyword_family,
                        report.diagnostic.contract_row_index,
                        unity_compile_authority_status_string(
                            report.diagnostic.compile_authority_status),
                        dxbc_compare_status_name(
                            report.diagnostic.dxbc_compare.status));
                if (report.diagnostic.dxbc_compare.status !=
                    DXBC_COMPARE_EQUAL) {
                    const DXBCCompareResult* comparison =
                        &report.diagnostic.dxbc_compare;
                    fprintf(stderr,
                            "    [GENERATED DOMAIN DXBC LOCALIZE] "
                            "byte=%zu chunk=%" PRIu32
                            " instruction=%" PRIu32 " token=%" PRIu32
                            " expected=0x%016" PRIx64
                            " actual=0x%016" PRIx64 "\n",
                            comparison->first_differing_byte,
                            comparison->chunk_index,
                            comparison->instruction_index,
                            comparison->token_index,
                            comparison->expected_value,
                            comparison->actual_value);
                }
                record_failure_at(
                    shader_result_index, path_id,
                    report.diagnostic.stage_index,
                    report.diagnostic.subprogram_index,
                    serialized_pass_index,
                    generated_domain_failure_mask(certification_status));
            }
            unity_generated_domain_report_free(&report);
            shaderlab_variant_plan_free(&plan);
        }
    }
}

static void free_generated_domain_pass_records(void) {
    free(g_generated_domain_passes);
    g_generated_domain_passes = NULL;
    g_generated_domain_pass_count = 0U;
    g_generated_domain_pass_capacity = 0U;
}

typedef struct {
    int dxbc_expected;
    int unsupported_stage_variants;
    int scheduled_and_decoded;
    int dxbc_compiled;
    int dxbc_token_matched;
    int dxbc_byte_matched;
} VerificationStatsSnapshot;

static VerificationStatsSnapshot verification_stats_snapshot(void) {
    VerificationStatsSnapshot snapshot;
    pthread_mutex_lock(&g_stats_mutex);
    snapshot.dxbc_expected = g_dxbc_expected;
    snapshot.unsupported_stage_variants = g_dxbc_unsupported_stage;
    snapshot.scheduled_and_decoded = g_shaders_total;
    snapshot.dxbc_compiled = g_dxbc_compiled;
    snapshot.dxbc_token_matched = g_dxbc_token_matched;
    snapshot.dxbc_byte_matched = g_dxbc_matched;
    pthread_mutex_unlock(&g_stats_mutex);
    return snapshot;
}

static ShaderVerificationResult* begin_shader_result(
    const UnitySerializedSource* source, size_t serialized_source_index,
    const char serialized_file_sha256[COMMON_SHA256_HEX_SIZE],
    long long path_id, size_t* out_index) {
    if (out_index) *out_index = SIZE_MAX;
    pthread_mutex_lock(&g_failures_mutex);
    if (g_shader_result_count == g_shader_result_capacity) {
        size_t new_capacity = g_shader_result_capacity == 0U
            ? 64U : g_shader_result_capacity * 2U;
        if (new_capacity < g_shader_result_capacity ||
            new_capacity > SIZE_MAX / sizeof(*g_shader_results)) {
            g_shader_report_allocation_failed = true;
            pthread_mutex_unlock(&g_failures_mutex);
            return NULL;
        }
        ShaderVerificationResult* new_results = realloc(
            g_shader_results, new_capacity * sizeof(*new_results));
        if (!new_results) {
            g_shader_report_allocation_failed = true;
            pthread_mutex_unlock(&g_failures_mutex);
            return NULL;
        }
        g_shader_results = new_results;
        g_shader_result_capacity = new_capacity;
    }

    size_t result_index = g_shader_result_count++;
    ShaderVerificationResult* result = &g_shader_results[result_index];
    memset(result, 0, sizeof(*result));
    result->outer_path = copy_string(source ? source->outer_path : NULL);
    result->serialized_member = copy_string(
        source && source->member_name ? source->member_name : "<standalone>");
    if (!result->outer_path || !result->serialized_member) {
        g_shader_report_allocation_failed = true;
    }
    result->member_index = source ? source->member_index : 0U;
    result->is_bundle_member = source && source->is_bundle_member;
    result->serialized_source_index = serialized_source_index;
    memcpy(result->serialized_file_sha256, serialized_file_sha256,
           COMMON_SHA256_HEX_SIZE);
    result->path_id = path_id;
    result->terminal_phase = "object-discovery";
    if (out_index) *out_index = result_index;
    pthread_mutex_unlock(&g_failures_mutex);
    g_thread_shader_result_index = result_index;
    return result;
}

static void finalize_shader_result(
    ShaderVerificationResult* result,
    VerificationStatsSnapshot before,
    const char* terminal_phase) {
    VerificationStatsSnapshot after = verification_stats_snapshot();
    if (result) {
        result->terminal_phase = terminal_phase;
        result->dxbc_expected = after.dxbc_expected - before.dxbc_expected;
        result->unsupported_stage_variants =
            after.unsupported_stage_variants -
            before.unsupported_stage_variants;
        result->scheduled_and_decoded =
            after.scheduled_and_decoded - before.scheduled_and_decoded;
        result->dxbc_compiled = after.dxbc_compiled - before.dxbc_compiled;
        result->dxbc_token_matched =
            after.dxbc_token_matched - before.dxbc_token_matched;
        result->dxbc_byte_matched =
            after.dxbc_byte_matched - before.dxbc_byte_matched;
    }
    g_thread_shader_result_index = SIZE_MAX;
}

static void free_shader_results(void) {
    for (size_t i = 0U; i < g_shader_result_count; ++i) {
        free(g_shader_results[i].outer_path);
        free(g_shader_results[i].serialized_member);
        free(g_shader_results[i].shader_name);
        free(g_shader_results[i].generated_source_path);
    }
    free(g_shader_results);
    g_shader_results = NULL;
    g_shader_result_count = 0U;
    g_shader_result_capacity = 0U;
}

static bool append_original_shader(const char* name, const char* path) {
    if (g_original_shader_count == g_original_shader_capacity) {
        size_t new_capacity = g_original_shader_capacity == 0U
            ? 128U : g_original_shader_capacity * 2U;
        if (new_capacity < g_original_shader_capacity ||
            new_capacity > SIZE_MAX / sizeof(*g_original_shaders)) {
            return false;
        }
        OriginalShaderMapping* new_mappings = realloc(
            g_original_shaders,
            new_capacity * sizeof(*new_mappings));
        if (!new_mappings) {
            return false;
        }
        g_original_shaders = new_mappings;
        g_original_shader_capacity = new_capacity;
    }

    char* name_copy = copy_string(name);
    char* path_copy = copy_string(path);
    if (!name_copy || !path_copy) {
        free(name_copy);
        free(path_copy);
        return false;
    }
    OriginalShaderMapping* mapping =
        &g_original_shaders[g_original_shader_count++];
    mapping->name = name_copy;
    mapping->path = path_copy;
    return true;
}

static int compare_original_shaders(const void* lhs, const void* rhs) {
    const OriginalShaderMapping* left = lhs;
    const OriginalShaderMapping* right = rhs;
    int name_order = strcmp(left->name, right->name);
    return name_order != 0 ? name_order : strcmp(left->path, right->path);
}

typedef enum {
    ORIGINAL_SHADER_NOT_FOUND = 0,
    ORIGINAL_SHADER_UNIQUE,
    ORIGINAL_SHADER_AMBIGUOUS,
} OriginalShaderLookupStatus;

static OriginalShaderLookupStatus find_original_shader_path(
    const char* name, const char** out_path) {
    *out_path = NULL;
    size_t first = 0U;
    size_t count = g_original_shader_count;
    while (count > 0) {
        size_t step = count / 2U;
        size_t index = first + step;
        int order = strcmp(g_original_shaders[index].name, name);
        if (order < 0) {
            first = index + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    if (first >= g_original_shader_count ||
        strcmp(g_original_shaders[first].name, name) != 0) {
        return ORIGINAL_SHADER_NOT_FOUND;
    }
    if (first + 1U < g_original_shader_count &&
        strcmp(g_original_shaders[first + 1U].name, name) == 0) {
        return ORIGINAL_SHADER_AMBIGUOUS;
    }
    *out_path = g_original_shaders[first].path;
    return ORIGINAL_SHADER_UNIQUE;
}

static void free_original_shaders(void) {
    for (size_t i = 0U; i < g_original_shader_count; ++i) {
        free(g_original_shaders[i].name);
        free(g_original_shaders[i].path);
    }
    free(g_original_shaders);
    g_original_shaders = NULL;
    g_original_shader_count = 0U;
    g_original_shader_capacity = 0U;
}

static uint8_t* read_file_to_buffer(const char* path, size_t* out_size) {
    if (!path || !out_size) return NULL;
    *out_size = 0U;
    CommonFileBytes file;
    if (common_file_read_regular_terminated(path, SIZE_MAX, &file) !=
        COMMON_FILE_OK) {
        return NULL;
    }
    *out_size = file.size;
    return file.data;
}

static bool oracle_is_enabled(void) {
    return g_oracle.frozen_pack != NULL ||
           g_oracle.capture_writer != NULL;
}

static void oracle_note_authority_failure(void) {
    pthread_mutex_lock(&g_oracle.mutex);
    g_oracle.authority_failures++;
    pthread_mutex_unlock(&g_oracle.mutex);
}

static void oracle_variant_authority_failure(
    long long path_id, int stage, int sub_idx, int pass_idx) {
    oracle_note_authority_failure();
    record_failure(path_id, stage, sub_idx, pass_idx,
                   FAILURE_ORACLE_AUTHORITY);
}

static OraclePackAuthorityInput oracle_pack_authority_input(void) {
    OraclePackAuthorityInput authority = {
        .compiler_fingerprint = g_oracle.compiler_fingerprint,
        .environment_fingerprint = g_oracle.environment_fingerprint,
    };
    return authority;
}

static UnityCompilerOfflineAuthority oracle_compiler_authority(void) {
    UnityCompilerOfflineAuthority authority = {
        .compiler_fingerprint = g_oracle.compiler_fingerprint,
        .environment_fingerprint = g_oracle.environment_fingerprint,
    };
    return authority;
}

static bool oracle_authority_matches_provenance(
    const UnityCompilerToolchainProvenance* provenance) {
    return provenance &&
        memcmp(g_oracle.compiler_fingerprint,
               provenance->compiler_fingerprint,
               sizeof(g_oracle.compiler_fingerprint)) == 0 &&
        memcmp(g_oracle.environment_fingerprint,
               provenance->environment_fingerprint,
               sizeof(g_oracle.environment_fingerprint)) == 0;
}

/* Strict frozen lookup never inspects Unity. Live fallback must revalidate
 * configured authority for every operation; request transcripts additionally
 * bind the current implicit include search tree. */
static bool oracle_validate_live_authority(UnityCompilerBroker* broker) {
    if (!broker) return false;
    pthread_mutex_lock(&g_oracle.mutex);
    UnityCompilerToolchainProvenance provenance;
    g_oracle.live_authority_matches =
        unity_compiler_broker_get_toolchain_provenance(broker, &provenance) &&
        oracle_authority_matches_provenance(&provenance);
    bool matches = g_oracle.live_authority_matches;
    pthread_mutex_unlock(&g_oracle.mutex);
    return matches;
}

static bool verification_oracle_initialize(
    UnityCompilerBroker* broker, const char* frozen_path,
    const char* capture_path, bool strict_hits) {
    if (strict_hits && !frozen_path) {
        fprintf(stderr, "--oracle-strict requires --oracle-pack PATH\n");
        return false;
    }
    if (frozen_path && capture_path && strcmp(frozen_path, capture_path) == 0) {
        fprintf(stderr, "The read-only oracle pack and capture output must "
                        "be different paths\n");
        return false;
    }
    if (capture_path) {
        errno = 0;
        if (access(capture_path, F_OK) == 0 || errno != ENOENT) {
            fprintf(stderr, "Oracle capture output already exists or cannot "
                            "be checked safely: %s\n", capture_path);
            return false;
        }
    }

    g_oracle.frozen_path = frozen_path;
    g_oracle.capture_path = capture_path;
    g_oracle.strict_hits = strict_hits;
    if (frozen_path) {
        size_t size = 0;
        uint8_t* data = read_file_to_buffer(frozen_path, &size);
        if (!data) {
            fprintf(stderr, "Could not read frozen oracle pack: %s\n",
                    frozen_path);
            return false;
        }
        OraclePackStatus status = oracle_pack_open_memory(
            data, size, &g_oracle.frozen_pack);
        free(data);
        if (status != ORACLE_PACK_OK) {
            fprintf(stderr, "Frozen oracle pack '%s' is invalid: %s\n",
                    frozen_path, oracle_pack_status_string(status));
            return false;
        }
        OraclePackAuthorityView authority;
        status = oracle_pack_authority(g_oracle.frozen_pack, &authority);
        if (status != ORACLE_PACK_OK || !authority.compiler_fingerprint ||
            !authority.environment_fingerprint) {
            fprintf(stderr, "Frozen oracle pack '%s' has no validated "
                            "compiler authority: %s\n",
                    frozen_path, oracle_pack_status_string(status));
            oracle_pack_free(g_oracle.frozen_pack);
            g_oracle.frozen_pack = NULL;
            return false;
        }
        memcpy(g_oracle.compiler_fingerprint,
               authority.compiler_fingerprint,
               sizeof(g_oracle.compiler_fingerprint));
        memcpy(g_oracle.environment_fingerprint,
               authority.environment_fingerprint,
               sizeof(g_oracle.environment_fingerprint));
        printf("[INFO] Loaded %zu compile entries and %zu preprocess "
               "contracts from read-only oracle pack %s\n",
               oracle_pack_entry_count(g_oracle.frozen_pack),
               oracle_pack_preprocess_count(g_oracle.frozen_pack),
               frozen_path);
    }
    if (capture_path) {
        UnityCompilerToolchainProvenance provenance;
        if (!unity_compiler_broker_get_toolchain_provenance(
                broker, &provenance)) {
            fprintf(stderr, "Could not fingerprint the compiler toolchain "
                            "for oracle capture\n");
            oracle_pack_free(g_oracle.frozen_pack);
            g_oracle.frozen_pack = NULL;
            return false;
        }
        if (g_oracle.frozen_pack &&
            !oracle_authority_matches_provenance(&provenance)) {
            fprintf(stderr, "Frozen oracle authority does not match the "
                            "live toolchain requested for capture\n");
            oracle_pack_free(g_oracle.frozen_pack);
            g_oracle.frozen_pack = NULL;
            return false;
        }
        if (!g_oracle.frozen_pack) {
            memcpy(g_oracle.compiler_fingerprint,
                   provenance.compiler_fingerprint,
                   sizeof(g_oracle.compiler_fingerprint));
            memcpy(g_oracle.environment_fingerprint,
                   provenance.environment_fingerprint,
                   sizeof(g_oracle.environment_fingerprint));
        }
        g_oracle.live_authority_matches = true;
        OraclePackAuthorityInput authority = oracle_pack_authority_input();
        OraclePackStatus status = oracle_pack_writer_create(
            &authority, &g_oracle.capture_writer);
        if (status != ORACLE_PACK_OK) {
            fprintf(stderr, "Could not create oracle capture writer: %s\n",
                    oracle_pack_status_string(status));
            oracle_pack_free(g_oracle.frozen_pack);
            g_oracle.frozen_pack = NULL;
            return false;
        }
        printf("[INFO] Oracle capture is explicit and write-once: %s\n",
               capture_path);
    }
    g_oracle.ready = true;
    return true;
}

static bool verification_oracle_finalize_capture(void) {
    if (!g_oracle.capture_writer) return true;
    if (g_oracle.capture_failed) {
        fprintf(stderr, "Oracle capture was not written because at least one "
                        "entry failed validation or insertion\n");
        return false;
    }
    uint8_t* data = NULL;
    size_t size = 0;
    OraclePackStatus status = oracle_pack_writer_finalize(
        g_oracle.capture_writer, &data, &size);
    if (status != ORACLE_PACK_OK) {
        fprintf(stderr, "Could not finalize oracle capture: %s\n",
                oracle_pack_status_string(status));
        return false;
    }
    CommonFileStatus write_status = common_file_write_new_atomic(
        g_oracle.capture_path, data, size);
    oracle_pack_bytes_free(data);
    if (write_status != COMMON_FILE_OK) {
        fprintf(stderr, "Could not create oracle capture '%s' without "
                        "overwriting an existing file: %s\n",
                g_oracle.capture_path,
                common_file_status_name(write_status));
        return false;
    }
    printf("[INFO] Wrote %llu compile entries and %llu preprocess "
           "contracts to %s\n",
           (unsigned long long)g_oracle.captured_entries,
           (unsigned long long)g_oracle.captured_preprocesses,
           g_oracle.capture_path);
    return true;
}

static void verification_oracle_dispose(void) {
    oracle_pack_writer_free(g_oracle.capture_writer);
    oracle_pack_free(g_oracle.frozen_pack);
    g_oracle.capture_writer = NULL;
    g_oracle.frozen_pack = NULL;
    g_oracle.ready = false;
    g_oracle.live_authority_matches = false;
    memset(g_oracle.compiler_fingerprint, 0,
           sizeof(g_oracle.compiler_fingerprint));
    memset(g_oracle.environment_fingerprint, 0,
           sizeof(g_oracle.environment_fingerprint));
}

/* Caller holds g_job_mutex. The queue grows from the evidence actually found
 * in a shader instead of imposing a corpus-shaped variant ceiling. */
static bool enqueue_subprogram_job(const SubProgramJob* input) {
    if (!input) return false;
    if (g_job_queue_size == g_job_queue_capacity) {
        size_t capacity = g_job_queue_capacity == 0U
            ? 128U : g_job_queue_capacity * 2U;
        if (capacity < g_job_queue_capacity ||
            capacity > SIZE_MAX / sizeof(*g_job_queue)) {
            return false;
        }
        SubProgramJob* queue = (SubProgramJob*)realloc(
            g_job_queue, capacity * sizeof(*queue));
        if (!queue) return false;
        g_job_queue = queue;
        g_job_queue_capacity = capacity;
    }
    g_job_queue[g_job_queue_size++] = *input;
    return true;
}

static void* worker_thread_func(void* arg) {
    UnityCompilerBroker* broker = (UnityCompilerBroker*)arg;

    while (true) {
        SubProgramJob job;
        pthread_mutex_lock(&g_job_mutex);
        while (g_job_queue_size == 0 && !g_should_exit) {
            pthread_cond_wait(&g_job_cond, &g_job_mutex);
        }
        if (g_should_exit && g_job_queue_size == 0) {
            pthread_mutex_unlock(&g_job_mutex);
            break;
        }

        job = g_job_queue[--g_job_queue_size];
        g_active_jobs++;
        pthread_mutex_unlock(&g_job_mutex);

        g_thread_shader_result_index = job.shader_result_index;
        verify_subprogram_shaderlab(
            broker,
            job.shader,
            job.sub_meta,
            job.path_id,
            job.stage,
            job.sub_idx,
            job.pass_idx,
            job.subshader_idx,
            job.local_pass_idx,
            job.pass,
            job.identity,
            job.generated_source_path,
            job.gen_prep,
            job.orig_prep,
            job.shaderlab_content,
            job.oracle_metadata
        );
        g_thread_shader_result_index = SIZE_MAX;

        if (job.sub_meta) {
            subprogram_metadata_free_variant(job.sub_meta);
            free(job.sub_meta);
        }
        if (job.oracle_metadata) {
            oracle_metadata_normalization_free(job.oracle_metadata);
            free(job.oracle_metadata);
        }

        pthread_mutex_lock(&g_job_mutex);
        g_active_jobs--;
        if (g_job_queue_size == 0 && g_active_jobs == 0) {
            pthread_cond_signal(&g_done_cond);
        }
        pthread_mutex_unlock(&g_job_mutex);
    }

    return NULL;
}

static bool has_shader_extension(const char* name) {
    size_t size = strlen(name);
    return size >= sizeof(".shader") - 1U &&
           strcmp(name + size - (sizeof(".shader") - 1U), ".shader") == 0;
}

static char* join_source_path(const char* directory, const char* name) {
    size_t directory_size = strlen(directory);
    size_t name_size = strlen(name);
    bool slash = directory_size != 0U && directory[directory_size - 1U] != '/';
    size_t separator_size = slash ? 1U : 0U;
    if (directory_size > SIZE_MAX - separator_size ||
        directory_size + separator_size > SIZE_MAX - name_size ||
        directory_size + separator_size + name_size == SIZE_MAX) {
        return NULL;
    }
    size_t size = directory_size + separator_size + name_size;
    char* path = (char*)malloc(size + 1U);
    if (!path) return NULL;
    memcpy(path, directory, directory_size);
    if (slash) path[directory_size] = '/';
    memcpy(path + directory_size + separator_size, name, name_size);
    path[size] = '\0';
    return path;
}

static bool index_original_shader_file(const char* path) {
    size_t source_size = 0U;
    uint8_t* source = read_file_to_buffer(path, &source_size);
    if (!source) {
        fprintf(stderr, "Could not read original ShaderLab source: %s\n",
                path);
        return false;
    }
    char* name = NULL;
    ShaderLabSourceNameStatus status = shaderlab_source_extract_name(
        source, source_size, &name);
    free(source);
    if (status == SHADERLAB_SOURCE_NAME_OK) {
        bool appended = append_original_shader(name, path);
        shaderlab_source_name_free(name);
        return appended;
    }
    fprintf(stderr, "Ignoring non-authoritative ShaderLab source '%s': %s\n",
            path, shaderlab_source_name_status_string(status));
    return status != SHADERLAB_SOURCE_NAME_ALLOCATION_FAILED;
}

static bool scan_original_shader_tree(const char* directory) {
    struct dirent** entries = NULL;
    int count = scandir(directory, &entries, NULL, alphasort);
    if (count < 0) {
        fprintf(stderr, "Could not scan original shader directory '%s': %s\n",
                directory, strerror(errno));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        struct dirent* entry = entries[i];
        if (!ok || strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            free(entry);
            continue;
        }
        char* path = join_source_path(directory, entry->d_name);
        if (!path) {
            ok = false;
            free(entry);
            continue;
        }
        struct stat status;
        if (lstat(path, &status) != 0) {
            fprintf(stderr, "Could not inspect original source path '%s': %s\n",
                    path, strerror(errno));
            ok = false;
        } else if (S_ISLNK(status.st_mode)) {
            /* Directory symlinks can introduce cycles and a mutable mapping
             * root. Treat every symlink as unavailable source authority. */
            fprintf(stderr, "Ignoring symlink in original source tree: %s\n",
                    path);
        } else if (S_ISDIR(status.st_mode)) {
            ok = scan_original_shader_tree(path);
        } else if (S_ISREG(status.st_mode) &&
                   has_shader_extension(entry->d_name)) {
            ok = index_original_shader_file(path);
        }
        free(path);
        free(entry);
    }
    free(entries);
    return ok;
}

static bool scan_original_shaders(const char* directory) {
    if (!scan_original_shader_tree(directory)) return false;
    qsort(g_original_shaders, g_original_shader_count,
          sizeof(*g_original_shaders), compare_original_shaders);
    return true;
}

static void sanitize_line(char* out, const char* in) {
    while (*in == ' ' || *in == '\t') in++;
    char* p = out;
    while (*in && *in != '\r' && *in != '\n') {
        if (*in == '/' && *(in + 1) == '/') {
            break;
        }
        *p++ = *in++;
    }
    *p = '\0';
    p--;
    while (p >= out && (*p == ' ' || *p == '\t')) {
        *p = '\0';
        p--;
    }
}

static bool has_stage_markers(const char* text) {
    return strstr(text, "#ifdef VERTEX") != NULL ||
           strstr(text, "#ifdef FRAGMENT") != NULL ||
           strstr(text, "#ifdef GEOMETRY") != NULL ||
           strstr(text, "#if defined(VERTEX)") != NULL ||
           strstr(text, "#if defined(FRAGMENT)") != NULL ||
           strstr(text, "#if defined(GEOMETRY)") != NULL;
}

static void sort_constant_buffers(char** lines, int count) {
    int* indices = malloc(count * sizeof(int));
    if (!indices) return;
    int cb_count = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(lines[i], "dcl_constantbuffer", 18) == 0) {
            indices[cb_count++] = i;
        }
    }
    for (int i = 0; i < cb_count - 1; i++) {
        for (int j = i + 1; j < cb_count; j++) {
            int idx_a = indices[i];
            int idx_b = indices[j];
            const char* cb_a = strstr(lines[idx_a], "CB");
            const char* cb_b = strstr(lines[idx_b], "CB");
            int val_a = cb_a ? atoi(cb_a + 2) : 0;
            int val_b = cb_b ? atoi(cb_b + 2) : 0;
            if (val_a > val_b) {
                char* temp = lines[idx_a];
                lines[idx_a] = lines[idx_b];
                lines[idx_b] = temp;
            }
        }
    }
    free(indices);
}

static bool compare_disassembly_internal(
    const char* orig_asm, const char* gen_asm, long long path_id, int stage,
    int sub_idx, int pass_idx, const char* platform_name,
    bool save_artifacts) {
    char** orig_lines = malloc(2000 * sizeof(char*));
    char** gen_lines = malloc(2000 * sizeof(char*));
    int orig_count = 0;
    int gen_count = 0;

    if (!orig_lines || !gen_lines) {
        free(orig_lines);
        free(gen_lines);
        return false;
    }

    bool orig_has_markers = has_stage_markers(orig_asm);
    bool in_target_stage = !orig_has_markers;

    const char* p = orig_asm;
    while (*p && orig_count < 2000) {
        char line[512];
        char* l = line;
        while (*p && *p != '\n' && (l - line) < 511) {
            *l++ = *p++;
        }
        if (*p == '\n') p++;
        *l = '\0';

        if (orig_has_markers) {
            char trimmed[512];
            sanitize_line(trimmed, line);
            if (strncmp(trimmed, "#ifdef VERTEX", 13) == 0 || strncmp(trimmed, "#if defined(VERTEX)", 19) == 0) {
                in_target_stage = (stage == 0);
                continue;
            } else if (strncmp(trimmed, "#ifdef FRAGMENT", 15) == 0 || strncmp(trimmed, "#if defined(FRAGMENT)", 21) == 0) {
                in_target_stage = (stage == 1);
                continue;
            } else if (strncmp(trimmed, "#ifdef GEOMETRY", 15) == 0 || strncmp(trimmed, "#if defined(GEOMETRY)", 21) == 0) {
                in_target_stage = (stage == 2);
                continue;
            } else if (strncmp(trimmed, "#ifdef", 6) == 0 || strncmp(trimmed, "#if", 3) == 0) {
                in_target_stage = false;
                continue;
            } else if (strncmp(trimmed, "#endif", 6) == 0) {
                in_target_stage = false;
                continue;
            }
        }

        if (in_target_stage) {
            char clean[512];
            sanitize_line(clean, line);
            if (clean[0] != '\0') {
                if (strncmp(clean, "#version", 8) != 0 &&
                    strncmp(clean, "#extension", 10) != 0 &&
                    strncmp(clean, "#define HLSLCC_", 15) != 0) {
                    orig_lines[orig_count++] = strdup(clean);
                }
            }
        }
    }

    bool gen_has_markers = has_stage_markers(gen_asm);
    in_target_stage = !gen_has_markers;

    p = gen_asm;
    while (*p && gen_count < 2000) {
        char line[512];
        char* l = line;
        while (*p && *p != '\n' && (l - line) < 511) {
            *l++ = *p++;
        }
        if (*p == '\n') p++;
        *l = '\0';

        if (gen_has_markers) {
            char trimmed[512];
            sanitize_line(trimmed, line);
            if (strncmp(trimmed, "#ifdef VERTEX", 13) == 0 || strncmp(trimmed, "#if defined(VERTEX)", 19) == 0) {
                in_target_stage = (stage == 0);
                continue;
            } else if (strncmp(trimmed, "#ifdef FRAGMENT", 15) == 0 || strncmp(trimmed, "#if defined(FRAGMENT)", 21) == 0) {
                in_target_stage = (stage == 1);
                continue;
            } else if (strncmp(trimmed, "#ifdef GEOMETRY", 15) == 0 || strncmp(trimmed, "#if defined(GEOMETRY)", 21) == 0) {
                in_target_stage = (stage == 2);
                continue;
            } else if (strncmp(trimmed, "#ifdef", 6) == 0 || strncmp(trimmed, "#if", 3) == 0) {
                in_target_stage = false;
                continue;
            } else if (strncmp(trimmed, "#endif", 6) == 0) {
                in_target_stage = false;
                continue;
            }
        }

        if (in_target_stage) {
            char clean[512];
            sanitize_line(clean, line);
            if (clean[0] != '\0') {
                if (strncmp(clean, "#version", 8) != 0 &&
                    strncmp(clean, "#extension", 10) != 0 &&
                    strncmp(clean, "#define HLSLCC_", 15) != 0) {
                    gen_lines[gen_count++] = strdup(clean);
                }
            }
        }
    }

    sort_constant_buffers(orig_lines, orig_count);
    sort_constant_buffers(gen_lines, gen_count);

    bool match = true;
    if (orig_count != gen_count) {
        match = false;
    } else {
        for (int i = 0; i < orig_count; i++) {
            if (strcmp(orig_lines[i], gen_lines[i]) != 0) {
                match = false;
                break;
            }
        }
    }

    if (!match && save_artifacts && g_save_failure_artifacts) {
        mkdir(g_failure_dir, 0755);
        char filename_orig[1024];
        char filename_gen[1024];
        snprintf(filename_orig, sizeof(filename_orig), "%s/mismatch_%lld_%d_%d_%d_%s_original.asm", g_failure_dir, path_id, stage, sub_idx, pass_idx, platform_name);
        snprintf(filename_gen, sizeof(filename_gen), "%s/mismatch_%lld_%d_%d_%d_%s_generated.asm", g_failure_dir, path_id, stage, sub_idx, pass_idx, platform_name);

        FILE* f_orig = fopen(filename_orig, "w");
        if (f_orig) {
            for (int i = 0; i < orig_count; i++) {
                fprintf(f_orig, "%s\n", orig_lines[i]);
            }
            fclose(f_orig);
        }

        FILE* f_gen = fopen(filename_gen, "w");
        if (f_gen) {
            for (int i = 0; i < gen_count; i++) {
                fprintf(f_gen, "%s\n", gen_lines[i]);
            }
            fclose(f_gen);
        }
    }

    for (int i = 0; i < orig_count; i++) free(orig_lines[i]);
    for (int i = 0; i < gen_count; i++) free(gen_lines[i]);
    free(orig_lines);
    free(gen_lines);

    return match;
}

static bool compare_disassembly(const char* orig_asm, const char* gen_asm,
                                long long path_id, int stage, int sub_idx,
                                int pass_idx, const char* platform_name) {
    return compare_disassembly_internal(orig_asm, gen_asm, path_id, stage,
                                        sub_idx, pass_idx, platform_name, true);
}

typedef struct {
    const uint8_t* container_data;
    size_t container_size;
    const uint8_t* data;
    size_t size;
    char fourcc[5];
} DXBCExecutableChunkView;

static bool read_dxbc_u32(const uint8_t* data, size_t size, size_t offset,
                          uint32_t* value) {
    uint32_t raw = 0;
    if (!data || !value || offset > size || size - offset < sizeof(raw)) {
        return false;
    }
    memcpy(&raw, data + offset, sizeof(raw));
    *value = read_le32(raw);
    return true;
}

/*
 * Unity's D3D11 result may be a raw DXBC container, a Unity compiled-program
 * record, or a USBD record table.  The executable token stream is the SHDR or
 * SHEX chunk.  Comparing that chunk directly is both stricter and much cheaper
 * than round-tripping both blobs through UnityShaderCompiler's disassembler.
 */
static bool dxbc_executable_chunk_view(const uint8_t* data, size_t size,
                                       DXBCExecutableChunkView* out) {
    DXBCContainerView container;
    uint32_t total_size = 0;
    uint32_t chunk_count = 0;
    bool found = false;

    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!dxbc_container_view_first(data, size, &container) ||
        container.size < 32 ||
        !dxbc_verify_hash(container.data, container.size) ||
        !read_dxbc_u32(container.data, container.size, 24, &total_size) ||
        !read_dxbc_u32(container.data, container.size, 28, &chunk_count) ||
        total_size != container.size ||
        (size_t)chunk_count > (container.size - 32) / sizeof(uint32_t)) {
        return false;
    }

    size_t table_end = 32 + (size_t)chunk_count * sizeof(uint32_t);
    for (uint32_t i = 0; i < chunk_count; i++) {
        uint32_t chunk_offset = 0;
        uint32_t chunk_size = 0;
        if (!read_dxbc_u32(container.data, container.size,
                           32 + (size_t)i * sizeof(uint32_t),
                           &chunk_offset) ||
            (size_t)chunk_offset < table_end ||
            (size_t)chunk_offset > container.size ||
            container.size - (size_t)chunk_offset < 8 ||
            !read_dxbc_u32(container.data, container.size,
                           (size_t)chunk_offset + 4, &chunk_size)) {
            return false;
        }

        size_t payload_offset = (size_t)chunk_offset + 8;
        if ((size_t)chunk_size > container.size - payload_offset) {
            return false;
        }
        const uint8_t* fourcc = container.data + chunk_offset;
        if (memcmp(fourcc, "SHDR", 4) != 0 &&
            memcmp(fourcc, "SHEX", 4) != 0) {
            continue;
        }
        if (found || chunk_size == 0) {
            return false;
        }
        found = true;
        out->container_data = container.data;
        out->container_size = container.size;
        out->data = container.data + payload_offset;
        out->size = chunk_size;
        memcpy(out->fourcc, fourcc, 4);
        out->fourcc[4] = '\0';
    }
    return found;
}

static bool dxbc_executable_chunks_equal(
    const DXBCExecutableChunkView* original,
    const DXBCExecutableChunkView* generated) {
    return original && generated &&
           memcmp(original->fourcc, generated->fourcc, 4) == 0 &&
           original->size == generated->size &&
           memcmp(original->data, generated->data, original->size) == 0;
}

static bool dxbc_containers_equal(const DXBCExecutableChunkView* original,
                                  const DXBCExecutableChunkView* generated) {
    return original && generated &&
           original->container_size == generated->container_size &&
           memcmp(original->container_data, generated->container_data,
                  original->container_size) == 0;
}

static void report_dxbc_structural_difference(
    const DXBCExecutableChunkView* original,
    const DXBCExecutableChunkView* generated) {
    DXBCCompareResult comparison;
    DXBCCompareStatus status = dxbc_compare_exact(
        original->container_data, original->container_size,
        generated->container_data, generated->container_size, &comparison);
    if (status == DXBC_COMPARE_EXPECTED_INVALID ||
        status == DXBC_COMPARE_ACTUAL_INVALID) {
        const DXBCDocumentDiagnostic* diagnostic =
            status == DXBC_COMPARE_EXPECTED_INVALID
                ? &comparison.expected_diagnostic
                : &comparison.actual_diagnostic;
        printf("    [DXBC LOCALIZE] %s: %s at byte %zu\n",
               dxbc_compare_status_name(status),
               dxbc_document_diagnostic_code_name(diagnostic->code),
               diagnostic->byte_offset);
        return;
    }
    printf("    [DXBC LOCALIZE] %s byte=%zu chunk=%" PRIu32
           " instruction=%" PRIu32 " token=%" PRIu32
           " expected=0x%016" PRIx64 " actual=0x%016" PRIx64 "\n",
           dxbc_compare_status_name(status), comparison.first_differing_byte,
           comparison.chunk_index, comparison.instruction_index,
           comparison.token_index, comparison.expected_value,
           comparison.actual_value);
}

static char* broker_disassemble_clean(
    UnityCompilerBroker* broker, const char* shader_name, int platform,
    int stage, const uint8_t* bytecode, size_t size,
    const VerificationDiagnosticContext* context) {
    UnityCompilerTextResponse response;
    unity_compiler_text_response_init(&response);
    bool received = unity_compiler_broker_disassemble_response(
        broker, shader_name, platform, stage, bytecode, size, &response);
    if (!received) {
        unity_compiler_text_response_free(&response);
        pthread_mutex_lock(&g_stats_mutex);
        fprintf(stderr,
                "    [DISASSEMBLE FAIL] phase=%s: Unity compiler "
                "transport or protocol failure\n",
                context->phase);
        pthread_mutex_unlock(&g_stats_mutex);
        record_failure_at(
            context->shader_result_index, context->path_id, context->stage,
            context->sub_idx, context->pass_idx,
            FAILURE_DXBC_DISASSEMBLE);
        return NULL;
    }

    if (response.status.availability ==
        UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS) {
        /* Mismatch disassembly is an optional human-readable artifact.  In
         * cache-only mode the exact raw containers above remain the evidence;
         * never turn a missing, non-cached rendering into either a compiler
         * launch or an additional verification failure. */
        pthread_mutex_lock(&g_stats_mutex);
        fprintf(stderr,
                "    [DISASSEMBLE UNAVAILABLE] phase=%s: cache-only mode "
                "retained raw DXBC without launching UnityShaderCompiler\n",
                context->phase);
        pthread_mutex_unlock(&g_stats_mutex);
        unity_compiler_text_response_free(&response);
        return NULL;
    }

    bool diagnostics_acceptable = retain_and_report_compiler_diagnostics(
        context, &response.status);
    bool clean = diagnostics_acceptable &&
        unity_compiler_response_status_is_clean_success(&response.status);
    if (!clean) {
        if (response.status.diagnostic_count == 0U) {
            char* detail = unity_compiler_response_status_format(
                &response.status, "Unity disassembly failed");
            pthread_mutex_lock(&g_stats_mutex);
            fprintf(stderr, "    [DISASSEMBLE FAIL] phase=%s: %s\n",
                    context->phase,
                    detail ? detail : "Unity disassembly failed");
            pthread_mutex_unlock(&g_stats_mutex);
            free(detail);
            record_failure_at(
                context->shader_result_index, context->path_id,
                context->stage, context->sub_idx, context->pass_idx,
                FAILURE_DXBC_DISASSEMBLE);
        }
        unity_compiler_text_response_free(&response);
        return NULL;
    }

    char* text = response.text;
    response.text = NULL;
    response.size = 0U;
    unity_compiler_text_response_free(&response);
    return text;
}

static void save_dxbc_mismatch_disassembly(
    UnityCompilerBroker* broker, const char* original_name,
    const char* generated_name, int stage,
    const DXBCExecutableChunkView* original,
    const DXBCExecutableChunkView* generated, long long path_id, int sub_idx,
    int pass_idx) {
    if (!g_save_failure_artifacts || !original || !generated) return;

    mkdir(g_failure_dir, 0755);
    char original_path[1024];
    char generated_path[1024];
    snprintf(original_path, sizeof(original_path),
             "%s/mismatch_%lld_%d_%d_%d_dxbc_original.dxbc", g_failure_dir,
             path_id, stage, sub_idx, pass_idx);
    snprintf(generated_path, sizeof(generated_path),
             "%s/mismatch_%lld_%d_%d_%d_dxbc_generated.dxbc", g_failure_dir,
             path_id, stage, sub_idx, pass_idx);
    FILE* file = fopen(original_path, "wb");
    if (file) {
        (void)fwrite(original->container_data, 1, original->container_size,
                     file);
        fclose(file);
    }
    file = fopen(generated_path, "wb");
    if (file) {
        (void)fwrite(generated->container_data, 1, generated->container_size,
                     file);
        fclose(file);
    }

    VerificationDiagnosticContext original_context = {
        .phase = "original-dxbc-disassemble",
        .shader_result_index = g_thread_shader_result_index,
        .path_id = path_id,
        .stage = stage,
        .sub_idx = sub_idx,
        .pass_idx = pass_idx,
        .operation = VERIFICATION_DIAGNOSTIC_DISASSEMBLE,
    };
    VerificationDiagnosticContext generated_context = original_context;
    generated_context.phase = "generated-dxbc-disassemble";
    char* original_disassembly = broker_disassemble_clean(
        broker, original_name, 4, stage, original->container_data,
        original->container_size, &original_context);
    char* generated_disassembly = broker_disassemble_clean(
        broker, generated_name, 4, stage, generated->container_data,
        generated->container_size, &generated_context);
    if (original_disassembly && generated_disassembly) {
        (void)compare_disassembly(original_disassembly, generated_disassembly,
                                  path_id, stage, sub_idx, pass_idx, "dxbc");
    }
    free(original_disassembly);
    free(generated_disassembly);
}

static void save_blob_mismatch(const uint8_t* original, size_t original_size,
                               const uint8_t* generated,
                               size_t generated_size, long long path_id,
                               int stage, int sub_idx, int pass_idx,
                               const char* platform_name) {
    if (!g_save_failure_artifacts) return;
    mkdir(g_failure_dir, 0755);
    char original_path[1024];
    char generated_path[1024];
    snprintf(original_path, sizeof(original_path),
             "%s/mismatch_%lld_%d_%d_%d_%s_original.asm", g_failure_dir,
             path_id, stage, sub_idx, pass_idx, platform_name);
    snprintf(generated_path, sizeof(generated_path),
             "%s/mismatch_%lld_%d_%d_%d_%s_generated.asm", g_failure_dir,
             path_id, stage, sub_idx, pass_idx, platform_name);
    FILE* file = fopen(original_path, "wb");
    if (file) {
        if (original_size > 0) fwrite(original, 1, original_size, file);
        fclose(file);
    }
    file = fopen(generated_path, "wb");
    if (file) {
        if (generated_size > 0) fwrite(generated, 1, generated_size, file);
        fclose(file);
    }
}

static void save_failed_hlsl(const char* hlsl_src, const char* prep_src,
                             const char* orig_prep_src, long long path_id,
                             int stage, int sub_idx, int pass_idx) {
    if (!g_save_failure_artifacts) return;
    mkdir(g_failure_dir, 0755);
    char filename[1024];
    snprintf(filename, sizeof(filename), "%s/failed_shader_%lld_%d_%d_%d.hlsl", g_failure_dir, path_id, stage, sub_idx, pass_idx);
    FILE* f_hlsl = fopen(filename, "w");
    if (f_hlsl) {
        fprintf(f_hlsl, "%s", hlsl_src);
        fclose(f_hlsl);
    }

    if (prep_src) {
        char prep_filename[1024];
        snprintf(prep_filename, sizeof(prep_filename), "%s/preprocessed_shader_%lld_%d_%d_%d.hlsl", g_failure_dir, path_id, stage, sub_idx, pass_idx);
        FILE* f_prep = fopen(prep_filename, "w");
        if (f_prep) {
            fprintf(f_prep, "%s", prep_src);
            fclose(f_prep);
        }
    }

    if (orig_prep_src) {
        char orig_prep_filename[1024];
        snprintf(orig_prep_filename, sizeof(orig_prep_filename), "%s/original_preprocessed_shader_%lld_%d_%d_%d.hlsl", g_failure_dir, path_id, stage, sub_idx, pass_idx);
        FILE* f_orig_prep = fopen(orig_prep_filename, "w");
        if (f_orig_prep) {
            fprintf(f_orig_prep, "%s", orig_prep_src);
            fclose(f_orig_prep);
        }
    }
}

static void save_failed_sources(
    const char* shaderlab_content, const PreprocessedSnippet* generated,
    const PreprocessedSnippet* original, long long path_id, int stage,
    int sub_idx, int pass_idx) {
    if (!g_save_failure_artifacts || !generated) return;
    save_failed_hlsl(
        shaderlab_content, generated->source,
        original ? original->source : NULL, path_id, stage, sub_idx,
        pass_idx);
}

typedef struct {
    UnityCompileAuthority authority;
    UnityCompilerSnippetCompileRequest request;
} ExactCompileInvocation;

static void exact_compile_invocation_init(ExactCompileInvocation* invocation) {
    memset(invocation, 0, sizeof(*invocation));
    unity_compile_authority_init(&invocation->authority);
}

static void exact_compile_invocation_free(ExactCompileInvocation* invocation) {
    if (!invocation) return;
    unity_compile_authority_free(&invocation->authority);
    memset(&invocation->request, 0, sizeof(invocation->request));
}

static UnityCompileAuthorityStatus prepare_exact_compile_invocation(
    const SerializedShader* shader, const SerializedPass* pass,
    const SerializedSubProgramIdentity* identity,
    const PreprocessedSnippet* snippet, const char* source_path, int stage,
    int platform, UnityPlatformCapabilitySnapshot capabilities,
    ExactCompileInvocation* invocation) {
    if (!shader || !pass || !identity || !snippet || !source_path ||
        !source_path[0] || !invocation || shader->keyword_names.count < 0 ||
        identity->global_keyword_index_count < 0 ||
        identity->local_keyword_index_count < 0) {
        return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    }
    if (!snippet->has_contract) {
        return UNITY_COMPILE_AUTHORITY_MISSING_PREPROCESS_CONTRACT;
    }
    int32_t compiler_program = 0;
    if (!unity_serialized_stage_to_compiler_program(
            stage, &compiler_program)) {
        return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    }
    UnityCompileAuthorityInput input = {
        .contract = &snippet->contract,
        .keyword_names = (const char* const*)shader->keyword_names.keywords,
        .keyword_name_count = (size_t)shader->keyword_names.count,
        .global_keyword_indices = identity->global_keyword_indices,
        .global_keyword_index_count =
            (size_t)identity->global_keyword_index_count,
        .local_keyword_indices = identity->local_keyword_indices,
        .local_keyword_index_count =
            (size_t)identity->local_keyword_index_count,
        .compiler_program = compiler_program,
        .pass_type = pass->pass_type,
        .platform_capabilities = capabilities,
    };
    UnityCompileAuthorityStatus status = unity_compile_authority_build(
        &input, &invocation->authority);
    if (status != UNITY_COMPILE_AUTHORITY_OK) return status;

    invocation->request = (UnityCompilerSnippetCompileRequest){
        .snippet_source = snippet->source,
        .source_directory = source_path,
        .source_basename = shader->name,
        .pass_name = pass->name,
        .caching_preprocessor = true,
        .preprocess_only = false,
        .strip_line_directives = false,
        .build_platform = g_compile_profile.build_platform,
        .render_state_length = 0,
        .variant_keywords = invocation->authority.platform_keywords,
        .variant_keyword_count =
            invocation->authority.platform_keyword_count,
        .user_keywords = invocation->authority.user_keywords,
        .user_keyword_count = invocation->authority.user_keyword_count,
        .disabled_keywords = invocation->authority.disabled_keywords,
        .disabled_keyword_count =
            invocation->authority.disabled_keyword_count,
        .compiler_flags = invocation->authority.compiler_flags,
        .shader_type = compiler_program,
        .platform = platform,
        .requirements = invocation->authority.requirements,
        .program_mask = (int32_t)pass->program_mask,
        .program_start = snippet->contract.start_line,
        .contract = &snippet->contract,
    };
    return UNITY_COMPILE_AUTHORITY_OK;
}

static void set_owned_error(char** out_error, const char* message) {
    if (!out_error || *out_error || !message) return;
    size_t size = strlen(message) + 1U;
    char* copy = (char*)malloc(size);
    if (!copy) return;
    memcpy(copy, message, size);
    *out_error = copy;
}

static bool find_archive_platform_index(
    const SerializedShader* shader, int platform, uint32_t* output) {
    if (!shader || !output || shader->archive_platform_count < 0 ||
        (shader->archive_platform_count > 0 && !shader->archive_platforms)) {
        return false;
    }
    int match = -1;
    for (int i = 0; i < shader->archive_platform_count; i++) {
        if (shader->archive_platforms[i] != platform) continue;
        if (match >= 0) return false;
        match = i;
    }
    if (match < 0) return false;
    *output = (uint32_t)match;
    return true;
}

static bool copy_keyword_indices(
    const int* source, int count, uint32_t** output) {
    *output = NULL;
    if (count < 0 || (count == 0) != (source == NULL) ||
        (size_t)count > SIZE_MAX / sizeof(**output)) {
        return false;
    }
    if (count == 0) return true;
    uint32_t* values = (uint32_t*)malloc(
        (size_t)count * sizeof(*values));
    if (!values) return false;
    for (int i = 0; i < count; i++) {
        if (source[i] < 0) {
            free(values);
            return false;
        }
        values[i] = (uint32_t)source[i];
    }
    *output = values;
    return true;
}

static bool build_oracle_variant_key(
    const SerializedShader* shader, const PlayerSubProgramMetadata* sub_meta,
    long long path_id, int stage, int sub_idx, int subshader_idx,
    int local_pass_idx, const SerializedPass* pass,
    const SerializedSubProgramIdentity* identity,
    const ExactCompileInvocation* invocation,
    const uint8_t request_digest[ORACLE_PACK_DIGEST_SIZE],
    VariantKey** out_key, char** out_error) {
    *out_key = NULL;
    if (!shader || !sub_meta || !pass || !identity || !invocation ||
        !request_digest || stage < 0 || stage >= 6 || sub_idx < 0 ||
        sub_idx >= pass->subprogram_count[stage] ||
        identity->hardware_tier_group < 0 ||
        identity->inner_subprogram_index < 0 ||
        subshader_idx < 0 || local_pass_idx < 0 ||
        !pass->subprogram_param_blob_indices[stage]) {
        set_owned_error(out_error,
                        "serialized variant identity is incomplete");
        return false;
    }
    const SerializedSubProgram* serialized_subprogram =
        &pass->subprograms[stage][sub_idx];
    if (serialized_subprogram->program_type != sub_meta->program_type ||
        serialized_subprogram->blob_index < 0 ||
        !sub_meta->has_player_blob_header) {
        set_owned_error(out_error,
                        "TypeTree and player-blob variant authorities differ");
        return false;
    }
    uint32_t archive_platform_index = 0;
    if (!find_archive_platform_index(shader, 4,
                                     &archive_platform_index)) {
        set_owned_error(out_error,
                        "D3D11 archive platform index is absent or ambiguous");
        return false;
    }
    uint32_t* global_indices = NULL;
    uint32_t* local_indices = NULL;
    if (!copy_keyword_indices(identity->global_keyword_indices,
                              identity->global_keyword_index_count,
                              &global_indices) ||
        !copy_keyword_indices(identity->local_keyword_indices,
                              identity->local_keyword_index_count,
                              &local_indices)) {
        free(global_indices);
        free(local_indices);
        set_owned_error(out_error,
                        "serialized keyword scopes are invalid");
        return false;
    }

    const UnityCompilerSnippetCompileRequest* request =
        &invocation->request;
    VariantKeyDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.shader_path_id = path_id;
    descriptor.subshader_index = (uint32_t)subshader_idx;
    descriptor.pass_index = (uint32_t)local_pass_idx;
    descriptor.stage = (uint32_t)stage;
    descriptor.compiler_platform = request->platform;
    descriptor.archive_platform_index = archive_platform_index;
    descriptor.hardware_tier_group =
        (uint32_t)identity->hardware_tier_group;
    descriptor.subprogram_index =
        (uint32_t)identity->inner_subprogram_index;
    descriptor.parameter_blob_index =
        pass->subprogram_param_blob_indices[stage][sub_idx];
    descriptor.serialized_program_type = serialized_subprogram->program_type;
    descriptor.serialized_hardware_tier_present =
        serialized_subprogram->has_hardware_tier;
    descriptor.serialized_hardware_tier =
        serialized_subprogram->hardware_tier;
    descriptor.serialized_requirements =
        serialized_subprogram->shader_requirements;
    descriptor.serialized_program_mask = pass->program_mask;
    descriptor.player_metadata_present = true;
    descriptor.player_program_type = sub_meta->program_type;
    memcpy(descriptor.player_header_words, sub_meta->player_header_words,
           sizeof(descriptor.player_header_words));
    descriptor.player_source_map = sub_meta->source_map;
    descriptor.keyword_scopes_are_explicit =
        identity->keyword_scopes_are_explicit;
    descriptor.global_keyword_indices = global_indices;
    descriptor.global_keyword_index_count =
        (size_t)identity->global_keyword_index_count;
    descriptor.local_keyword_indices = local_indices;
    descriptor.local_keyword_index_count =
        (size_t)identity->local_keyword_index_count;

    descriptor.compiler.schema_version = 1U;
    descriptor.compiler.build_platform = request->build_platform;
    descriptor.compiler.compiler_flags = request->compiler_flags;
    descriptor.compiler.language = request->contract->language;
    descriptor.compiler.shader_type = request->shader_type;
    descriptor.compiler.request_requirements = request->requirements;
    descriptor.compiler.request_program_mask = (uint32_t)request->program_mask;
    descriptor.compiler.program_start = request->program_start;
    descriptor.compiler.render_state_length = request->render_state_length;
    descriptor.compiler.valid_apis = g_compile_profile.valid_apis;
    descriptor.compiler.caching_preprocessor = request->caching_preprocessor;
    descriptor.compiler.preprocess_only = request->preprocess_only;
    descriptor.compiler.strip_line_directives =
        request->strip_line_directives;
    descriptor.compiler.source_directory = request->source_directory;
    descriptor.compiler.source_basename = request->source_basename;
    descriptor.compiler.pass_name = request->pass_name;
    descriptor.compiler.variant_keywords =
        (const char* const*)request->variant_keywords;
    descriptor.compiler.variant_keyword_count =
        (size_t)request->variant_keyword_count;
    descriptor.compiler.user_keywords =
        (const char* const*)request->user_keywords;
    descriptor.compiler.user_keyword_count =
        (size_t)request->user_keyword_count;
    descriptor.compiler.enabled_platform_keywords =
        (const char* const*)request->variant_keywords;
    descriptor.compiler.enabled_platform_keyword_count =
        (size_t)request->variant_keyword_count;
    descriptor.compiler.disabled_keywords =
        (const char* const*)request->disabled_keywords;
    descriptor.compiler.disabled_keyword_count =
        (size_t)request->disabled_keyword_count;
    descriptor.compiler.compiler_fingerprint.present = true;
    memcpy(descriptor.compiler.compiler_fingerprint.bytes,
           g_oracle.compiler_fingerprint,
           sizeof(descriptor.compiler.compiler_fingerprint.bytes));
    descriptor.compiler.environment_fingerprint.present = true;
    memcpy(descriptor.compiler.environment_fingerprint.bytes,
           g_oracle.environment_fingerprint,
           sizeof(descriptor.compiler.environment_fingerprint.bytes));
    descriptor.compiler.source_fingerprint.present = true;
    common_sha256(request->snippet_source,
                  strlen(request->snippet_source),
                  descriptor.compiler.source_fingerprint.bytes);
    descriptor.compiler.request_input_fingerprint.present = true;
    memcpy(descriptor.compiler.request_input_fingerprint.bytes,
           request_digest,
           sizeof(descriptor.compiler.request_input_fingerprint.bytes));

    VariantKeyStatus status = variant_key_init(out_key, &descriptor);
    free(global_indices);
    free(local_indices);
    if (status != VARIANT_KEY_OK) {
        char message[160];
        snprintf(message, sizeof(message),
                 "could not construct exact VariantKey: %s",
                 variant_key_status_string(status));
        set_owned_error(out_error, message);
        return false;
    }
    return true;
}

static bool oracle_capture_add(
    const VariantKey* key, OraclePackBytes stripped_dxbc,
    OraclePackBytes linked_glcore, OraclePackBytes transcript,
    const OracleMetadataNormalization* metadata, char** out_error) {
    if (!g_oracle.capture_writer) return true;
    OraclePackEntryInput input = {
        .variant_key = key,
        .stripped_dxbc = stripped_dxbc,
        .linked_glcore = linked_glcore,
        .compile_request_transcript = transcript,
        .normalized_metadata = metadata->pack_input,
        .authority = oracle_pack_authority_input(),
    };
    pthread_mutex_lock(&g_oracle.mutex);
    OraclePackStatus status = oracle_pack_writer_add(
        g_oracle.capture_writer, &input);
    if (status == ORACLE_PACK_OK) {
        g_oracle.captured_entries++;
    } else {
        g_oracle.capture_failed = true;
        g_oracle.authority_failures++;
    }
    pthread_mutex_unlock(&g_oracle.mutex);
    if (status != ORACLE_PACK_OK) {
        char message[160];
        snprintf(message, sizeof(message),
                 "could not add exact oracle capture entry: %s",
                 oracle_pack_status_string(status));
        set_owned_error(out_error, message);
        return false;
    }
    return true;
}

static bool broker_preprocess_clean(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    const VerificationDiagnosticContext* context,
    PreprocessResult* out_result) {
    UnityCompilerPreprocessResponse response;
    unity_compiler_preprocess_response_init(&response);
    bool received = unity_compiler_broker_preprocess_contract_response(
        broker, request, &response);
    if (!received) {
        unity_compiler_preprocess_response_free(&response);
        return false;
    }

    bool diagnostics_acceptable = retain_and_report_compiler_diagnostics(
        context, &response.status);
    bool clean = diagnostics_acceptable &&
        unity_compiler_response_status_is_clean_success(&response.status);
    if (!clean) {
        if (response.status.diagnostic_count == 0U) {
            char* detail = unity_compiler_response_status_format(
                &response.status, "Unity preprocessing failed");
            pthread_mutex_lock(&g_stats_mutex);
            fprintf(stderr, "    [PREPROCESS FAIL] phase=%s: %s\n",
                    context->phase,
                    detail ? detail : "Unity preprocessing failed");
            pthread_mutex_unlock(&g_stats_mutex);
            free(detail);
        }
        unity_compiler_preprocess_response_free(&response);
        return false;
    }

    *out_result = response.result;
    memset(&response.result, 0, sizeof(response.result));
    unity_compiler_preprocess_response_free(&response);
    return true;
}

static bool oracle_preprocess_or_lookup(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    long long path_id, const char* phase,
    PreprocessResult* out_result) {
    if (!broker || !request || !out_result) return false;
    memset(out_result, 0, sizeof(*out_result));
    VerificationDiagnosticContext diagnostic_context = {
        .phase = phase,
        .shader_result_index = g_thread_shader_result_index,
        .path_id = path_id,
        .stage = -1,
        .sub_idx = -1,
        .pass_idx = -1,
        .operation = VERIFICATION_DIAGNOSTIC_PREPROCESS,
    };
    if (!oracle_is_enabled()) {
        return broker_preprocess_clean(
            broker, request, &diagnostic_context, out_result);
    }
    if (!g_oracle.ready) {
        oracle_note_authority_failure();
        return false;
    }

    uint8_t* transcript = NULL;
    size_t transcript_size = 0U;
    uint8_t request_digest[ORACLE_PACK_DIGEST_SIZE];
    UnityCompilerOfflineAuthority authority = oracle_compiler_authority();
    const bool serialized = g_oracle.strict_hits
        ? unity_compiler_broker_serialize_preprocess_request_with_authority(
              broker, request, &authority, &transcript, &transcript_size, request_digest)
        : unity_compiler_broker_serialize_preprocess_request(
              broker, request, &transcript, &transcript_size, request_digest);
    if (!serialized || !transcript || transcript_size == 0U) {
        free(transcript);
        oracle_note_authority_failure();
        fprintf(stderr, "Preprocess authority request could not be "
                        "serialized for %s\n", request->file_path);
        return false;
    }
    uint8_t calculated_digest[ORACLE_PACK_DIGEST_SIZE];
    common_sha256(transcript, transcript_size, calculated_digest);
    if (memcmp(calculated_digest, request_digest,
               sizeof(request_digest)) != 0) {
        free(transcript);
        oracle_note_authority_failure();
        fprintf(stderr, "Preprocess request transcript digest diverged "
                        "from the cache authority for %s\n",
                request->file_path);
        return false;
    }

    OraclePackPreprocessView record;
    OraclePackStatus lookup = g_oracle.frozen_pack
        ? oracle_pack_lookup_preprocess_digest(
              g_oracle.frozen_pack, request_digest, &record)
        : ORACLE_PACK_NOT_FOUND;
    if (lookup == ORACLE_PACK_OK) {
        uint8_t result_digest[ORACLE_PACK_DIGEST_SIZE];
        common_sha256(record.serialized_result.data,
                      record.serialized_result.size, result_digest);
        bool valid =
            record.request_transcript.size == transcript_size &&
            memcmp(record.request_transcript.data, transcript,
                   transcript_size) == 0 &&
            memcmp(record.request_digest, request_digest,
                   sizeof(request_digest)) == 0 &&
            memcmp(record.result_digest, result_digest,
                   sizeof(result_digest)) == 0;
        if (valid) {
            valid = unity_compiler_broker_deserialize_preprocess_result(
                broker, record.serialized_result.data,
                record.serialized_result.size, out_result);
        }
        pthread_mutex_lock(&g_oracle.mutex);
        if (!valid) {
            g_oracle.authority_failures++;
        }
        pthread_mutex_unlock(&g_oracle.mutex);
        free(transcript);
        if (!valid) {
            unity_compiler_free_preprocess(out_result);
            memset(out_result, 0, sizeof(*out_result));
            fprintf(stderr, "Preprocess oracle record failed transcript, "
                            "result, or typed-payload validation "
                            "for %s\n", request->file_path);
            return false;
        }
        unity_compiler_free_preprocess(out_result);
        memset(out_result, 0, sizeof(*out_result));
        oracle_note_authority_failure();
        record_diagnostic_authority_failure(
            &diagnostic_context,
            "OraclePack v4 preprocess records do not encode the ordered "
            "Unity compiler diagnostics or an attestation that none were "
            "emitted");
        return false;
    }
    if (lookup != ORACLE_PACK_NOT_FOUND) {
        free(transcript);
        oracle_note_authority_failure();
        fprintf(stderr, "Preprocess oracle lookup failed closed for %s: "
                        "%s\n", request->file_path,
                oracle_pack_status_string(lookup));
        return false;
    }

    pthread_mutex_lock(&g_oracle.mutex);
    g_oracle.preprocess_misses++;
    pthread_mutex_unlock(&g_oracle.mutex);
    if (g_oracle.strict_hits) {
        free(transcript);
        oracle_note_authority_failure();
        fprintf(stderr, "Strict preprocess oracle miss for %s\n",
                request->file_path);
        return false;
    }

    pthread_mutex_lock(&g_oracle.mutex);
    g_oracle.preprocess_broker_fallbacks++;
    pthread_mutex_unlock(&g_oracle.mutex);
    if (!oracle_validate_live_authority(broker)) {
        free(transcript);
        oracle_note_authority_failure();
        fprintf(stderr, "Live compiler authority does not match the oracle "
                        "pack for preprocess fallback: %s\n",
                request->file_path);
        return false;
    }
    if (!broker_preprocess_clean(
            broker, request, &diagnostic_context, out_result)) {
        free(transcript);
        return false;
    }
    uint8_t* current_transcript = NULL;
    size_t current_transcript_size = 0;
    uint8_t current_digest[ORACLE_PACK_DIGEST_SIZE];
    const bool request_unchanged = unity_compiler_broker_serialize_preprocess_request(
        broker, request, &current_transcript, &current_transcript_size, current_digest) &&
        memcmp(request_digest, current_digest, sizeof(current_digest)) == 0;
    free(current_transcript);
    if (!request_unchanged) {
        oracle_note_authority_failure();
        unity_compiler_free_preprocess(out_result);
        memset(out_result, 0, sizeof(*out_result));
        free(transcript);
        return false;
    }
    if (g_oracle.capture_writer) {
        uint8_t* serialized_result = NULL;
        size_t serialized_result_size = 0U;
        if (!unity_compiler_broker_serialize_preprocess_result(
                broker, out_result, &serialized_result,
                &serialized_result_size) || !serialized_result ||
            serialized_result_size == 0U) {
            free(serialized_result);
            free(transcript);
            unity_compiler_free_preprocess(out_result);
            memset(out_result, 0, sizeof(*out_result));
            pthread_mutex_lock(&g_oracle.mutex);
            g_oracle.capture_failed = true;
            g_oracle.authority_failures++;
            pthread_mutex_unlock(&g_oracle.mutex);
            return false;
        }
        OraclePackPreprocessInput input = {
            .request_transcript = {transcript, transcript_size},
            .serialized_result = {
                serialized_result, serialized_result_size},
            .authority = oracle_pack_authority_input(),
        };
        pthread_mutex_lock(&g_oracle.mutex);
        OraclePackStatus status = oracle_pack_writer_add_preprocess(
            g_oracle.capture_writer, &input);
        if (status == ORACLE_PACK_OK) {
            g_oracle.captured_preprocesses++;
        } else {
            g_oracle.capture_failed = true;
            g_oracle.authority_failures++;
        }
        pthread_mutex_unlock(&g_oracle.mutex);
        free(serialized_result);
        if (status != ORACLE_PACK_OK) {
            free(transcript);
            unity_compiler_free_preprocess(out_result);
            memset(out_result, 0, sizeof(*out_result));
            fprintf(stderr, "Could not capture preprocess authority for "
                            "%s: %s\n", request->file_path,
                    oracle_pack_status_string(status));
            return false;
        }
    }
    free(transcript);
    return true;
}

static bool oracle_dxbc_container_bytes(
    const uint8_t* data, size_t size, OraclePackBytes* output) {
    DXBCContainerView container;
    if (!output || !dxbc_container_view_first(data, size, &container) ||
        !dxbc_verify_hash(container.data, container.size)) {
        return false;
    }
    *output = (OraclePackBytes){container.data, container.size};
    return true;
}

static uint8_t* broker_compile_clean(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    const VerificationDiagnosticContext* context,
    size_t* out_size, char** out_error) {
    if (out_size) *out_size = 0U;
    if (out_error) *out_error = NULL;
    if (!broker || !request || !context || !out_size) return NULL;

    UnityCompilerBinaryResponse response;
    unity_compiler_binary_response_init(&response);
    bool received = unity_compiler_broker_compile_contract_response(
        broker, request, &response);
    if (!received) {
        unity_compiler_binary_response_free(&response);
        set_owned_error(out_error,
                        "Unity compiler transport or protocol failure");
        return NULL;
    }

    bool diagnostics_acceptable = retain_and_report_compiler_diagnostics(
        context, &response.status);
    bool clean = diagnostics_acceptable &&
        unity_compiler_response_status_is_clean_success(&response.status);
    if (!clean) {
        if (out_error) {
            *out_error = unity_compiler_response_status_format(
                &response.status,
                response.status.compiler_success
                    ? "Unity compiler returned diagnostics"
                    : "Unity compiler rejected the request");
        }
        unity_compiler_binary_response_free(&response);
        return NULL;
    }

    uint8_t* data = response.data;
    size_t size = response.size;
    response.data = NULL;
    response.size = 0U;
    unity_compiler_binary_response_free(&response);
    if (!data) {
        data = (uint8_t*)malloc(1U);
        if (!data) {
            set_owned_error(out_error,
                            "out of memory retaining compiler artifact");
            return NULL;
        }
    }
    *out_size = size;
    return data;
}

static uint8_t* oracle_compile_or_lookup(
    UnityCompilerBroker* broker, const SerializedShader* shader,
    const PlayerSubProgramMetadata* sub_meta,
    long long path_id, int stage, int sub_idx, int report_pass_idx,
    int subshader_idx, int local_pass_idx, const SerializedPass* pass,
    const SerializedSubProgramIdentity* identity,
    const ExactCompileInvocation* invocation,
    const OracleMetadataNormalization* metadata,
    const char* diagnostic_phase,
    size_t* out_size, char** out_error) {
    *out_size = 0;
    if (out_error) *out_error = NULL;
    VerificationDiagnosticContext diagnostic_context = {
        .phase = diagnostic_phase,
        .shader_result_index = g_thread_shader_result_index,
        .path_id = path_id,
        .stage = stage,
        .sub_idx = sub_idx,
        .pass_idx = report_pass_idx,
        .operation = VERIFICATION_DIAGNOSTIC_COMPILE,
    };
    if (!oracle_is_enabled()) {
        return broker_compile_clean(
            broker, &invocation->request, &diagnostic_context,
            out_size, out_error);
    }
    if (!g_oracle.ready || !metadata) {
        oracle_variant_authority_failure(path_id, stage, sub_idx,
                                         report_pass_idx);
        set_owned_error(out_error, "oracle metadata authority is unavailable");
        return NULL;
    }

    uint8_t* transcript = NULL;
    size_t transcript_size = 0;
    uint8_t request_digest[ORACLE_PACK_DIGEST_SIZE];
    UnityCompilerOfflineAuthority authority = oracle_compiler_authority();
    const bool serialized = g_oracle.strict_hits
        ? unity_compiler_broker_serialize_compile_request_with_authority(
              broker, &invocation->request, &authority, &transcript,
              &transcript_size, request_digest)
        : unity_compiler_broker_serialize_compile_request(
              broker, &invocation->request, &transcript, &transcript_size, request_digest);
    if (!serialized || !transcript || transcript_size == 0U) {
        free(transcript);
        oracle_variant_authority_failure(path_id, stage, sub_idx,
                                         report_pass_idx);
        set_owned_error(out_error,
                        "could not serialize exact compiler request");
        return NULL;
    }

    VariantKey* key = NULL;
    if (!build_oracle_variant_key(
            shader, sub_meta, path_id, stage, sub_idx, subshader_idx,
            local_pass_idx, pass, identity, invocation, request_digest,
            &key, out_error)) {
        free(transcript);
        oracle_variant_authority_failure(path_id, stage, sub_idx,
                                         report_pass_idx);
        return NULL;
    }

    OraclePackEntryView entry;
    OraclePackStatus lookup_status = g_oracle.frozen_pack
        ? oracle_pack_lookup_variant_key(g_oracle.frozen_pack, key, &entry)
        : ORACLE_PACK_NOT_FOUND;
    if (lookup_status == ORACLE_PACK_OK) {
        bool metadata_equal = false;
        OraclePackStatus metadata_status =
            oracle_pack_metadata_matches_input(
                entry.normalized_metadata_bytes, &metadata->pack_input,
                &metadata_equal);
        bool valid =
            entry.compile_request_transcript.size == transcript_size &&
            memcmp(entry.compile_request_transcript.data, transcript,
                   transcript_size) == 0 &&
            memcmp(entry.compile_request_digest, request_digest,
                   sizeof(request_digest)) == 0 &&
            metadata_status == ORACLE_PACK_OK && metadata_equal;
        if (!valid) {
            pthread_mutex_lock(&g_oracle.mutex);
            g_oracle.authority_failures++;
            pthread_mutex_unlock(&g_oracle.mutex);
            variant_key_free(key);
            free(transcript);
            set_owned_error(out_error,
                            "oracle hit failed transcript, metadata, or "
                            "DXBC-anchor validation");
            return NULL;
        }

        oracle_variant_authority_failure(path_id, stage, sub_idx,
                                         report_pass_idx);
        record_diagnostic_authority_failure(
            &diagnostic_context,
            "OraclePack v4 compile entries do not encode the ordered Unity "
            "compiler diagnostics or an attestation that none were emitted");
        variant_key_free(key);
        free(transcript);
        set_owned_error(out_error,
                        "oracle artifact lacks compiler diagnostic authority");
        return NULL;
    }

    if (lookup_status != ORACLE_PACK_NOT_FOUND) {
        oracle_variant_authority_failure(path_id, stage, sub_idx,
                                         report_pass_idx);
        char message[160];
        snprintf(message, sizeof(message), "oracle lookup failed: %s",
                 oracle_pack_status_string(lookup_status));
        set_owned_error(out_error, message);
        variant_key_free(key);
        free(transcript);
        return NULL;
    }
    if (g_oracle.frozen_pack) {
        pthread_mutex_lock(&g_oracle.mutex);
        g_oracle.misses++;
        pthread_mutex_unlock(&g_oracle.mutex);
    }
    if (g_oracle.strict_hits) {
        oracle_variant_authority_failure(path_id, stage, sub_idx,
                                         report_pass_idx);
        set_owned_error(out_error,
                        "exact VariantKey is absent from strict oracle pack");
        variant_key_free(key);
        free(transcript);
        return NULL;
    }

    if (!oracle_validate_live_authority(broker)) {
        oracle_variant_authority_failure(path_id, stage, sub_idx,
                                         report_pass_idx);
        set_owned_error(out_error,
                        "live compiler authority does not match oracle pack");
        variant_key_free(key);
        free(transcript);
        return NULL;
    }
    uint8_t* result = broker_compile_clean(
        broker, &invocation->request, &diagnostic_context,
        out_size, out_error);
    if (result) {
        uint8_t* current_transcript = NULL;
        size_t current_transcript_size = 0;
        uint8_t current_digest[ORACLE_PACK_DIGEST_SIZE];
        const bool request_unchanged = unity_compiler_broker_serialize_compile_request(
            broker, &invocation->request, &current_transcript, &current_transcript_size,
            current_digest) && memcmp(request_digest, current_digest, sizeof(current_digest)) == 0;
        free(current_transcript);
        if (!request_unchanged) {
            oracle_variant_authority_failure(path_id, stage, sub_idx, report_pass_idx);
            free(result);
            result = NULL;
            *out_size = 0;
            set_owned_error(out_error, "compiler request authority changed during oracle capture");
        }
    }
    if (result) {
        pthread_mutex_lock(&g_oracle.mutex);
        g_oracle.broker_fallbacks++;
        pthread_mutex_unlock(&g_oracle.mutex);
        OraclePackBytes stripped = {NULL, 0U};
        if (!oracle_dxbc_container_bytes(
                result, *out_size, &stripped)) {
            pthread_mutex_lock(&g_oracle.mutex);
            g_oracle.capture_failed = g_oracle.capture_writer != NULL;
            g_oracle.authority_failures++;
            pthread_mutex_unlock(&g_oracle.mutex);
            record_failure(path_id, stage, sub_idx, report_pass_idx,
                           FAILURE_ORACLE_AUTHORITY);
            set_owned_error(out_error,
                            "compiler artifact has no exact valid DXBC "
                            "container for OraclePack anchoring");
            free(result);
            result = NULL;
            *out_size = 0;
        }
        if (result && !oracle_capture_add(
                key, stripped, (OraclePackBytes){NULL, 0U},
                (OraclePackBytes){transcript, transcript_size}, metadata,
                out_error)) {
            free(result);
            result = NULL;
            *out_size = 0;
        }
    }
    variant_key_free(key);
    free(transcript);
    return result;
}

static void print_compile_authority(
    const char* label, const UnityCompileAuthority* authority) {
    if (!getenv("DXBC_DEBUG_SNIPPETS") || !authority) return;
    fprintf(stderr, "    [%s pKW]", label);
    for (int i = 0; i < authority->platform_keyword_count; i++) {
        fprintf(stderr, " %s", authority->platform_keywords[i]);
    }
    fprintf(stderr, "\n    [%s uKW]", label);
    for (int i = 0; i < authority->user_keyword_count; i++) {
        fprintf(stderr, " %s", authority->user_keywords[i]);
    }
    fprintf(stderr, "\n    [%s dKW]", label);
    for (int i = 0; i < authority->disabled_keyword_count; i++) {
        fprintf(stderr, " %s", authority->disabled_keywords[i]);
    }
    fprintf(stderr, "\n");
}

static void record_compile_authority_failure(
    long long path_id, int stage, int sub_idx, int pass_idx,
    const char* target, UnityCompileAuthorityStatus status) {
    pthread_mutex_lock(&g_stats_mutex);
    printf("    [%s AUTHORITY UNAVAILABLE] %s\n", target,
           unity_compile_authority_status_string(status));
    pthread_mutex_unlock(&g_stats_mutex);
    record_failure(path_id, stage, sub_idx, pass_idx,
                   FAILURE_COMPILE_AUTHORITY);
}

static void verify_subprogram_shaderlab(
    UnityCompilerBroker* broker,
    const SerializedShader* shader,
    const PlayerSubProgramMetadata* sub_meta,
    long long path_id,
    int stage,
    int sub_idx,
    int pass_idx,
    int subshader_idx,
    int local_pass_idx,
    const SerializedPass* pass,
    const SerializedSubProgramIdentity* identity,
    const char* generated_source_path,
    const PreprocessResult* gen_prep,
    const PreprocessResult* orig_prep,
    const char* shaderlab_content,
    const OracleMetadataNormalization* oracle_metadata
) {
    if (sub_meta->bytecode_length == 0 || sub_meta->bytecode == NULL) {
        return;
    }

    int snippet_idx = unity_shaderlab_pass_snippet_index(shader, pass_idx);
    if (snippet_idx < 0 || snippet_idx >= gen_prep->snippet_count) {
        pthread_mutex_lock(&g_stats_mutex);
        printf("    [FAIL] Serialized pass %d maps to generated snippet %d "
               "with only %d snippets\n", pass_idx, snippet_idx,
               gen_prep->snippet_count);
        pthread_mutex_unlock(&g_stats_mutex);
        record_failure(path_id, stage, sub_idx, pass_idx,
                       FAILURE_DXBC_COMPILE);
        return;
    }
    const PreprocessedSnippet* gen_snip = &gen_prep->snippets[snippet_idx];
    const PreprocessedSnippet* orig_snip =
        unity_shaderlab_find_original_snippet(shader, pass_idx, orig_prep);


    char gen_name[512];
    snprintf(gen_name, sizeof(gen_name), "DXBCSandbox/Gen/%s", shader->name);

    // -------------------------------------------------------------------------
    // PASS 1: DXBC Verification
    // -------------------------------------------------------------------------
    pthread_mutex_lock(&g_stats_mutex);
    printf("  [DXBC Native] Compiling...\n");
    pthread_mutex_unlock(&g_stats_mutex);

    ExactCompileInvocation generated_d3d;
    exact_compile_invocation_init(&generated_d3d);
    UnityCompileAuthorityStatus generated_d3d_status =
        prepare_exact_compile_invocation(
            shader, pass, identity, gen_snip, generated_source_path, stage, 4,
            g_compile_profile.d3d11_capabilities, &generated_d3d);
    if (generated_d3d_status != UNITY_COMPILE_AUTHORITY_OK) {
        record_compile_authority_failure(
            path_id, stage, sub_idx, pass_idx, "DXBC",
            generated_d3d_status);
        exact_compile_invocation_free(&generated_d3d);
        return;
    }
    print_compile_authority("DXBC", &generated_d3d.authority);

    size_t gen_dxbc_size = 0;
    char* gen_dxbc_err = NULL;
    uint8_t* gen_dxbc_bytecode = oracle_compile_or_lookup(
        broker, shader, sub_meta, path_id, stage,
        sub_idx, pass_idx, subshader_idx, local_pass_idx, pass, identity,
        &generated_d3d, oracle_metadata, "generated-dxbc-compile",
        &gen_dxbc_size, &gen_dxbc_err);

    if (gen_dxbc_bytecode) {
        pthread_mutex_lock(&g_stats_mutex);
        g_dxbc_compiled++;
        pthread_mutex_unlock(&g_stats_mutex);

        DXBCExecutableChunkView original;
        DXBCExecutableChunkView generated;
        bool original_valid = dxbc_executable_chunk_view(
            sub_meta->bytecode, sub_meta->bytecode_length, &original);
        bool generated_valid = dxbc_executable_chunk_view(
            gen_dxbc_bytecode, gen_dxbc_size, &generated);

        if (original_valid && generated_valid) {
            bool token_match =
                dxbc_executable_chunks_equal(&original, &generated);
            bool byte_match = dxbc_containers_equal(&original, &generated);
            if (token_match) {
                pthread_mutex_lock(&g_stats_mutex);
                g_dxbc_token_matched++;
                pthread_mutex_unlock(&g_stats_mutex);
            }
            if (byte_match) {
                pthread_mutex_lock(&g_stats_mutex);
                printf("    [DXBC BYTE MATCH 1:1] Full release DXBC container "
                       "matches original exactly!\n");
                g_dxbc_matched++;
                pthread_mutex_unlock(&g_stats_mutex);
            } else {
                pthread_mutex_lock(&g_stats_mutex);
                if (token_match) {
                    printf("    [DXBC BYTE MISMATCH] Full container differs; "
                           "SHDR/SHEX token stream matches exactly.\n");
                } else {
                    printf("    [DXBC BYTE MISMATCH] Full container and "
                           "SHDR/SHEX token stream differ.\n");
                }
                report_dxbc_structural_difference(&original, &generated);
                pthread_mutex_unlock(&g_stats_mutex);
                save_dxbc_mismatch_disassembly(
                    broker, shader->name, gen_name, stage, &original,
                    &generated, path_id, sub_idx, pass_idx);
                save_failed_sources(shaderlab_content, gen_snip, orig_snip,
                                    path_id, stage, sub_idx, pass_idx);
                record_failure(path_id, stage, sub_idx, pass_idx,
                               FAILURE_DXBC_MISMATCH);
            }
        } else {
            pthread_mutex_lock(&g_stats_mutex);
            printf("    [DXBC WARNING] Could not locate a valid SHDR/SHEX chunk "
                   "(original=%d, generated=%d)\n",
                   original_valid ? 1 : 0, generated_valid ? 1 : 0);
            pthread_mutex_unlock(&g_stats_mutex);
            record_failure(path_id, stage, sub_idx, pass_idx,
                           FAILURE_DXBC_DISASSEMBLE);
        }
        free(gen_dxbc_bytecode);
    } else {
        pthread_mutex_lock(&g_stats_mutex);
        printf("    [DXBC FAIL] DXBC compilation failed: %s\n", gen_dxbc_err ? gen_dxbc_err : "Unknown");
        pthread_mutex_unlock(&g_stats_mutex);
        save_failed_sources(shaderlab_content, gen_snip, orig_snip, path_id,
                            stage, sub_idx, pass_idx);
        record_failure(path_id, stage, sub_idx, pass_idx,
                       FAILURE_DXBC_COMPILE);
    }
    if (gen_dxbc_err) free(gen_dxbc_err);
    exact_compile_invocation_free(&generated_d3d);
}

static const char* serialized_glcore_verification_status_name(
    SerializedGLCoreVerificationStatus status) {
    switch (status) {
        case SERIALIZED_GLCORE_VERIFY_FILTERED_OUT:
            return "filtered-out";
        case SERIALIZED_GLCORE_VERIFY_TARGET_UNAVAILABLE:
            return "serialized-target-unavailable";
        case SERIALIZED_GLCORE_VERIFY_SNIPPET_UNAVAILABLE:
            return "generated-snippet-unavailable";
        case SERIALIZED_GLCORE_VERIFY_ORACLE_PACK_V4_UNSUPPORTED:
            return "oracle-pack-v4-unsupported";
        case SERIALIZED_GLCORE_VERIFY_COMPILE_AUTHORITY_UNAVAILABLE:
            return "compile-authority-unavailable";
        case SERIALIZED_GLCORE_VERIFY_COMPILE_FAILED:
            return "generated-compile-failed";
        case SERIALIZED_GLCORE_VERIFY_TEXT_MISMATCH:
            return "released-text-mismatch";
        case SERIALIZED_GLCORE_VERIFY_EXACT:
            return "exact";
        default:
            return "unknown";
    }
}

static SerializedGLCoreTargetRecord* append_serialized_glcore_target_record(
    size_t shader_result_index, long long path_id, int subshader_index,
    int local_pass_index, int serialized_pass_index,
    int serialized_stage, int flattened_subprogram_index,
    const SerializedSubProgram* program,
    const SerializedSubProgramIdentity* identity) {
    if (!program) return NULL;
    if (g_serialized_glcore_target_count ==
        g_serialized_glcore_target_capacity) {
        const size_t capacity = g_serialized_glcore_target_capacity == 0U
            ? 64U : g_serialized_glcore_target_capacity * 2U;
        if (capacity < g_serialized_glcore_target_capacity ||
            capacity > SIZE_MAX / sizeof(*g_serialized_glcore_targets)) {
            g_serialized_glcore_report_allocation_failed = true;
            return NULL;
        }
        SerializedGLCoreTargetRecord* records = realloc(
            g_serialized_glcore_targets,
            capacity * sizeof(*g_serialized_glcore_targets));
        if (!records) {
            g_serialized_glcore_report_allocation_failed = true;
            return NULL;
        }
        g_serialized_glcore_targets = records;
        g_serialized_glcore_target_capacity = capacity;
    }
    SerializedGLCoreTargetRecord* record =
        &g_serialized_glcore_targets[g_serialized_glcore_target_count++];
    memset(record, 0, sizeof(*record));
    record->shader_result_index = shader_result_index;
    record->path_id = path_id;
    record->subshader_index = subshader_index;
    record->local_pass_index = local_pass_index;
    record->serialized_pass_index = serialized_pass_index;
    record->serialized_stage = serialized_stage;
    record->flattened_subprogram_index = flattened_subprogram_index;
    record->program_type = program->program_type;
    record->hardware_tier_group = identity
        ? identity->hardware_tier_group : -1;
    record->inner_subprogram_index = identity
        ? identity->inner_subprogram_index : -1;
    record->archive_entry_index = program->blob_index;
    record->status = SERIALIZED_GLCORE_VERIFY_FILTERED_OUT;
    record->target_status = SERIALIZED_GLCORE_TARGET_INVALID_ARGUMENT;
    record->compile_authority_status =
        UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    record->certificate_status = GLCORE_LINK_CERTIFICATE_INVALID_ARGUMENT;
    record->first_differing_byte = SIZE_MAX;
    return record;
}

static void free_serialized_glcore_target_records(void) {
    free(g_serialized_glcore_targets);
    g_serialized_glcore_targets = NULL;
    g_serialized_glcore_target_count = 0U;
    g_serialized_glcore_target_capacity = 0U;
}

static void serialized_glcore_count_object_unavailable(
    ShaderVerificationResult* result) {
    ++g_serialized_glcore.unavailable_objects;
    if (result) ++result->serialized_glcore.unavailable_objects;
}

static void serialized_glcore_count_target_unavailable(
    ShaderVerificationResult* result) {
    ++g_serialized_glcore.targets_unavailable;
    if (result) ++result->serialized_glcore.targets_unavailable;
}

static void verify_serialized_glcore_targets(
    UnityCompilerBroker* broker, const SerializedFile* file,
    const TypeTreeValue* shader_value, const SerializedShader* shader,
    SerializedShaderSchemaProfile shader_profile, long long path_id,
    size_t shader_result_index, ShaderVerificationResult* result,
    const PreprocessResult* generated_preprocess,
    const char* generated_source_path, const char* shaderlab_content) {
    if (!g_verify_glsl || !broker || !file || !shader_value || !shader ||
        !generated_preprocess || !generated_source_path) {
        return;
    }

    ++g_serialized_glcore.objects_evaluated;
    if (result) {
        ++result->serialized_glcore.objects_evaluated;
        result->glcore_readiness_evaluated = true;
    }

    /* Use the first-class readiness API without transferring ownership of
     * the already parsed object.  The shallow view is read-only and is never
     * disposed. */
    ShaderObject object_view;
    memset(&object_view, 0, sizeof(object_view));
    object_view.root = *shader_value;
    object_view.shader = *shader;
    object_view.profile = shader_profile;
    object_view.path_id = path_id;
    object_view.decoded = true;
    SerializedGLCoreTargetStatus readiness =
        serialized_glcore_object_readiness(&object_view);
    if (result) result->glcore_readiness_status = readiness;
    if (readiness == SERIALIZED_GLCORE_TARGET_PLATFORM_ABSENT) {
        ++g_serialized_glcore.platform_absent_objects;
        if (result) ++result->serialized_glcore.platform_absent_objects;
        printf("    [GLCORE NOT APPLICABLE] Released Shader object has no "
               "serialized platform-15 plane.\n");
        return;
    }
    if (readiness != SERIALIZED_GLCORE_TARGET_OK) {
        serialized_glcore_count_object_unavailable(result);
        printf("    [GLCORE TARGET UNAVAILABLE] %s\n",
               serialized_glcore_target_status_name(readiness));
        record_failure_at(shader_result_index, path_id, -1, -1, -1,
                          FAILURE_GLCORE_TARGET_UNAVAILABLE);
        return;
    }

    ++g_serialized_glcore.platform_present_objects;
    if (result) ++result->serialized_glcore.platform_present_objects;

    ShaderBlobArchive archive;
    memset(&archive, 0, sizeof(archive));
    if (!shader_blob_archive_open(shader_value, SERIALIZED_GLCORE_PLATFORM,
                                  &archive)) {
        if (result) {
            result->glcore_readiness_status =
                SERIALIZED_GLCORE_TARGET_ARCHIVE_INVALID;
        }
        serialized_glcore_count_object_unavailable(result);
        record_failure_at(shader_result_index, path_id, -1, -1, -1,
                          FAILURE_GLCORE_TARGET_UNAVAILABLE);
        return;
    }

    size_t platform_owner_count = 0U;
    int serialized_pass_index = 0;
    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; ++subshader_index) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        for (int local_pass_index = 0;
             local_pass_index < subshader->pass_count;
             ++local_pass_index, ++serialized_pass_index) {
            const SerializedPass* pass =
                &subshader->passes[local_pass_index];
            for (int stage = 0;
                 stage < UNITY_SERIALIZED_STAGE_COUNT; ++stage) {
                for (int subprogram_index = 0;
                     subprogram_index < pass->subprogram_count[stage];
                     ++subprogram_index) {
                if (!serialized_pass_subprogram_is_platform(
                        pass, stage, subprogram_index,
                        SERIALIZED_GLCORE_PLATFORM)) {
                    continue;
                }
                ++platform_owner_count;
                ++g_serialized_glcore.targets_discovered;
                if (result) ++result->serialized_glcore.targets_discovered;
                const SerializedSubProgram* program =
                    &pass->subprograms[stage][subprogram_index];
                const SerializedSubProgramIdentity* identity =
                    &pass->subprogram_identities[stage][subprogram_index];
                SerializedGLCoreTargetRecord* record =
                    append_serialized_glcore_target_record(
                        shader_result_index, path_id, subshader_index,
                        local_pass_index, serialized_pass_index,
                        stage, subprogram_index, program, identity);
                if (!record) {
                    serialized_glcore_count_target_unavailable(result);
                    record_failure_at(
                        shader_result_index, path_id, stage,
                        subprogram_index, serialized_pass_index,
                        FAILURE_GLCORE_TARGET_UNAVAILABLE);
                    continue;
                }

                if (!pass_is_admitted_by_tuple_filter(
                        path_id, pass, serialized_pass_index)) {
                    ++g_serialized_glcore.targets_filtered_out;
                    if (result) {
                        ++result->serialized_glcore.targets_filtered_out;
                    }
                    continue;
                }
                ++g_serialized_glcore.targets_expected;
                if (result) ++result->serialized_glcore.targets_expected;

                SerializedGLCoreTarget target;
                serialized_glcore_target_init(&target);
                const SerializedGLCoreTargetInput input = {
                    .unity_version = file->unity_version,
                    .compiler_platform = SERIALIZED_GLCORE_PLATFORM,
                    .archive = &archive,
                    .pass = pass,
                    .shader_path_id = path_id,
                    .subshader_index = subshader_index,
                    .pass_index = local_pass_index,
                    .stage_index = stage,
                    .flattened_subprogram_index = subprogram_index,
                };
                record->target_status =
                    serialized_glcore_target_open(&target, &input);
                if (record->target_status != SERIALIZED_GLCORE_TARGET_OK) {
                    record->status =
                        SERIALIZED_GLCORE_VERIFY_TARGET_UNAVAILABLE;
                    serialized_glcore_count_target_unavailable(result);
                    printf("    [GLCORE TARGET UNAVAILABLE] pass=%d sub=%d "
                           "%s\n", serialized_pass_index,
                           subprogram_index,
                           serialized_glcore_target_status_name(
                               record->target_status));
                    record_failure_at(
                        shader_result_index, path_id, stage,
                        subprogram_index, serialized_pass_index,
                        FAILURE_GLCORE_TARGET_UNAVAILABLE);
                    serialized_glcore_target_dispose(&target);
                    continue;
                }
                ++g_serialized_glcore.targets_opened;
                if (result) ++result->serialized_glcore.targets_opened;
                record->archive_entry_index =
                    target.owner.archive_entry_index;
                record->expected_size = target.released_text_size;

                const int snippet_index =
                    unity_shaderlab_pass_snippet_index(
                        shader, serialized_pass_index);
                if (snippet_index < 0 ||
                    snippet_index >= generated_preprocess->snippet_count) {
                    record->status =
                        SERIALIZED_GLCORE_VERIFY_SNIPPET_UNAVAILABLE;
                    serialized_glcore_count_target_unavailable(result);
                    record_failure_at(
                        shader_result_index, path_id, stage,
                        subprogram_index, serialized_pass_index,
                        FAILURE_GLCORE_TARGET_UNAVAILABLE);
                    serialized_glcore_target_dispose(&target);
                    continue;
                }
                const PreprocessedSnippet* generated_snippet =
                    &generated_preprocess->snippets[snippet_index];

                /* OraclePack v4 keys GLCore payloads through a D3D anchor.
                 * A serialized link owner has its own independent identity,
                 * so strict v4 replay cannot represent this row. */
                if (g_oracle.strict_hits && g_oracle.frozen_pack) {
                    record->status =
                        SERIALIZED_GLCORE_VERIFY_ORACLE_PACK_V4_UNSUPPORTED;
                    ++g_serialized_glcore.oracle_pack_v4_unsupported;
                    if (result) {
                        ++result->serialized_glcore
                              .oracle_pack_v4_unsupported;
                    }
                    oracle_variant_authority_failure(
                        path_id, stage, subprogram_index,
                        serialized_pass_index);
                    record_failure_at(
                        shader_result_index, path_id, stage,
                        subprogram_index, serialized_pass_index,
                        FAILURE_GLCORE_ORACLE_UNSUPPORTED);
                    printf("    [GLCORE ORACLE UNSUPPORTED] Strict "
                           "OraclePack v4 cannot identify an independent "
                           "serialized GLCore link owner.\n");
                    serialized_glcore_target_dispose(&target);
                    continue;
                }

                ExactCompileInvocation invocation;
                exact_compile_invocation_init(&invocation);
                record->compile_authority_status =
                    prepare_exact_compile_invocation(
                        shader, pass, identity, generated_snippet,
                        generated_source_path, stage,
                        SERIALIZED_GLCORE_PLATFORM,
                        g_compile_profile.glcore_capabilities,
                        &invocation);
                if (record->compile_authority_status !=
                    UNITY_COMPILE_AUTHORITY_OK) {
                    record->status =
                        SERIALIZED_GLCORE_VERIFY_COMPILE_AUTHORITY_UNAVAILABLE;
                    serialized_glcore_count_target_unavailable(result);
                    record_compile_authority_failure(
                        path_id, stage, subprogram_index,
                        serialized_pass_index, "SERIALIZED GLCORE",
                        record->compile_authority_status);
                    exact_compile_invocation_free(&invocation);
                    serialized_glcore_target_dispose(&target);
                    continue;
                }
                print_compile_authority("SERIALIZED GLCORE",
                                        &invocation.authority);
                VerificationDiagnosticContext diagnostic_context = {
                    .phase = "serialized-glcore-generated-compile",
                    .shader_result_index = shader_result_index,
                    .path_id = path_id,
                    .stage = stage,
                    .sub_idx = subprogram_index,
                    .pass_idx = serialized_pass_index,
                    .operation = VERIFICATION_DIAGNOSTIC_COMPILE,
                };
                ++g_serialized_glcore.compile_attempts;
                if (result) ++result->serialized_glcore.compile_attempts;
                record->compile_attempted = true;
                size_t generated_size = 0U;
                char* compile_error = NULL;
                uint8_t* generated_text = broker_compile_clean(
                    broker, &invocation.request, &diagnostic_context,
                    &generated_size, &compile_error);
                if (!generated_text || generated_size == 0U) {
                    record->status =
                        SERIALIZED_GLCORE_VERIFY_COMPILE_FAILED;
                    record->actual_size = generated_size;
                    serialized_glcore_count_target_unavailable(result);
                    printf("    [GLCORE COMPILE FAILED] pass=%d sub=%d: %s\n",
                           serialized_pass_index, subprogram_index,
                           compile_error ? compile_error
                                         : "empty linked program");
                    save_failed_sources(
                        shaderlab_content, generated_snippet, NULL, path_id,
                        stage, subprogram_index, serialized_pass_index);
                    record_failure_at(
                        shader_result_index, path_id, stage,
                        subprogram_index, serialized_pass_index,
                        FAILURE_GLSL_GENERATED_COMPILE);
                    free(generated_text);
                    free(compile_error);
                    exact_compile_invocation_free(&invocation);
                    serialized_glcore_target_dispose(&target);
                    continue;
                }

                record->compiler_terminal_success = true;
                ++g_serialized_glcore.targets_compiled;
                if (result) ++result->serialized_glcore.targets_compiled;
                const GLCoreLinkOutput linked_output = {
                    .stage = UNITY_SERIALIZED_STAGE_VERTEX,
                    .bytes = generated_text,
                    .size = generated_size,
                };
                const GLCoreGeneratedLinkVector generated_vector = {
                    .unity_version = file->unity_version,
                    .compiler_platform = SERIALIZED_GLCORE_PLATFORM,
                    .program_type =
                        SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE,
                    .owner = {
                        .shader_path_id = path_id,
                        .subshader_index = subshader_index,
                        .pass_index = local_pass_index,
                        .serialized_stage =
                            UNITY_SERIALIZED_STAGE_VERTEX,
                        .flattened_subprogram_index = subprogram_index,
                        .hardware_tier_group =
                            identity->hardware_tier_group,
                        .inner_subprogram_index =
                            identity->inner_subprogram_index,
                        .archive_entry_index = program->blob_index,
                    },
                    .shape = GLCORE_LINK_VECTOR_COMBINED_ONLY,
                    .outputs = &linked_output,
                    .output_count = 1U,
                };
                GLCoreLinkCertificateReport certificate;
                record->certificate_status =
                    glcore_link_certificate_compare_vector(
                        &target, &generated_vector, &certificate);
                record->actual_size = certificate.actual_size;
                record->first_differing_byte =
                    certificate.first_differing_byte;
                if (record->certificate_status ==
                    GLCORE_LINK_CERTIFICATE_OK) {
                    record->status = SERIALIZED_GLCORE_VERIFY_EXACT;
                    ++g_serialized_glcore.targets_exact;
                    if (result) ++result->serialized_glcore.targets_exact;
                    printf("    [GLCORE SERIALIZED MATCH 1:1] pass=%d "
                           "owner=%d released text matches exactly.\n",
                           serialized_pass_index, subprogram_index);
                } else {
                    record->status =
                        SERIALIZED_GLCORE_VERIFY_TEXT_MISMATCH;
                    ++g_serialized_glcore.targets_mismatched;
                    if (result) {
                        ++result->serialized_glcore.targets_mismatched;
                    }
                    if (certificate.first_differing_byte == SIZE_MAX) {
                        printf("    [GLCORE SERIALIZED MISMATCH] pass=%d "
                               "sub=%d certificate=%s expected=%zu "
                               "actual=%zu first_byte=unavailable\n",
                               serialized_pass_index, subprogram_index,
                               glcore_link_certificate_status_name(
                                   record->certificate_status),
                               certificate.expected_size,
                               certificate.actual_size);
                    } else {
                        printf("    [GLCORE SERIALIZED MISMATCH] pass=%d "
                               "sub=%d expected=%zu actual=%zu "
                               "first_byte=%zu\n",
                               serialized_pass_index, subprogram_index,
                               certificate.expected_size,
                               certificate.actual_size,
                               certificate.first_differing_byte);
                    }
                    save_blob_mismatch(
                        target.released_text_bytes,
                        target.released_text_size, generated_text,
                        generated_size, path_id, stage, subprogram_index,
                        serialized_pass_index, "glcore-serialized");
                    save_failed_sources(
                        shaderlab_content, generated_snippet, NULL, path_id,
                        stage, subprogram_index, serialized_pass_index);
                    record_failure_at(
                        shader_result_index, path_id, stage,
                        subprogram_index, serialized_pass_index,
                        FAILURE_GLSL_MISMATCH);
                }
                free(generated_text);
                free(compile_error);
                exact_compile_invocation_free(&invocation);
                serialized_glcore_target_dispose(&target);
                }
            }
        }
    }
    shader_blob_archive_close(&archive);

    if (platform_owner_count == 0U) {
        serialized_glcore_count_object_unavailable(result);
        printf("    [GLCORE TARGET UNAVAILABLE] Platform 15 is present, "
               "but no serialized platform-15 owner record exists.\n");
        record_failure_at(shader_result_index, path_id, -1, -1, -1,
                          FAILURE_GLCORE_TARGET_UNAVAILABLE);
    }
}

static bool prepare_oracle_metadata_for_variant(
    const ShaderBlobArchive* archive, const SerializedPass* pass,
    int stage, int sub_idx, OracleMetadataNormalization** output) {
    *output = NULL;
    if (!oracle_is_enabled()) return true;
    if (!archive || !pass || stage < 0 || stage >= 6 || sub_idx < 0 ||
        sub_idx >= pass->subprogram_count[stage] ||
        !pass->subprogram_param_blob_indices[stage]) {
        return false;
    }

    SerializedProgramParameters binary_parameters;
    serialized_program_parameters_init(&binary_parameters);
    const SerializedProgramParameters* selected = NULL;
    int parameter_index =
        pass->subprogram_param_blob_indices[stage][sub_idx];
    if (parameter_index >= 0) {
        const uint8_t* payload = NULL;
        size_t payload_size = 0;
        if (!shader_blob_archive_get(archive, parameter_index, &payload,
                                     &payload_size)) {
            return false;
        }
        ByteStream stream;
        stream_init(&stream, payload, payload_size);
        stream_set_endian(&stream, false);
        if (!subprogram_metadata_parse_parameters(
                &stream, &binary_parameters)) {
            serialized_program_parameters_free(&binary_parameters);
            return false;
        }
        selected = &binary_parameters;
    } else if (parameter_index == -1) {
        selected = &pass->common_parameters[stage];
    } else {
        return false;
    }

    OracleMetadataNormalization* normalization =
        (OracleMetadataNormalization*)malloc(sizeof(*normalization));
    if (!normalization) {
        serialized_program_parameters_free(&binary_parameters);
        return false;
    }
    oracle_metadata_normalization_init(normalization);
    OracleMetadataStatus status = oracle_metadata_normalize(
        selected, NULL, 0U, normalization);
    serialized_program_parameters_free(&binary_parameters);
    if (status != ORACLE_METADATA_OK) {
        fprintf(stderr, "Could not normalize exact oracle metadata: %s\n",
                oracle_metadata_status_string(status));
        oracle_metadata_normalization_free(normalization);
        free(normalization);
        return false;
    }
    *output = normalization;
    return true;
}

static void process_shader_object(
    UnityCompilerBroker* broker,
    SerializedFile* file,
    const AssetObjectInfo* obj,
    const char* shaderlab_dir,
    const UnitySerializedSource* source,
    size_t serialized_source_index
) {
    if (!is_shader_filtered((long long)obj->path_id)) {
        return;
    }
    uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE];
    char serialized_digest_hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(file->raw_data, file->raw_size, serialized_digest);
    common_sha256_digest_to_hex(serialized_digest, serialized_digest_hex);
    VerificationStatsSnapshot stats_before = verification_stats_snapshot();
    size_t verification_index = SIZE_MAX;
    ShaderVerificationResult* verification = begin_shader_result(
        source, serialized_source_index, serialized_digest_hex,
        (long long)obj->path_id, &verification_index);
    g_shader_objects_seen++;

    size_t shader_size = 0;
    const uint8_t* shader_data = serialized_file_get_object_data(file, obj, &shader_size);
    if (!shader_data || shader_size == 0) {
        finalize_shader_result(verification, stats_before,
                               "object-bytes-unavailable");
        return;
    }

    TypeTreeValue shader_value;
    memset(&shader_value, 0, sizeof(shader_value));
    int node_idx = 0;
    ByteStream stream;
    stream_init(&stream, shader_data, shader_size);
    stream_set_endian(&stream, file->big_endian);
    if (!typetree_parse_value_ex(
            &file->types[obj->type_id_or_index], &node_idx, &stream,
            &shader_value, TYPETREE_PARSE_PACK_COMPRESSED_BLOB)) {
        finalize_shader_result(verification, stats_before,
                               "typetree-parse-failed");
        return;
    }
    if (stream.position != shader_size ||
        node_idx != file->types[obj->type_id_or_index].node_count) {
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "typetree-not-fully-consumed");
        return;
    }

    SerializedShaderSchemaProfile shader_profile;
    if (!serialized_shader_profile_from_unity_version(
            file->unity_version, &shader_profile)) {
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "shader-schema-profile-unavailable");
        return;
    }
    SerializedShader shader;
    serialized_shader_init(&shader);
    if (!serialized_shader_parse_with_profile(
            &shader, &shader_value, shader_profile)) {
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "serialized-shader-parse-failed");
        return;
    }
    if (verification) {
        verification->parsed = true;
        verification->shader_name = copy_string(shader.name);
        if (!verification->shader_name) {
            g_shader_report_allocation_failed = true;
        }
    }
    g_shader_objects_parsed++;
    int serialized_pass_index = 0;
    for (int subshader_idx = 0; subshader_idx < shader.subshader_count;
         subshader_idx++) {
        const SerializedSubShader* subshader =
            &shader.subshaders[subshader_idx];
        for (int pass_idx = 0; pass_idx < subshader->pass_count;
             pass_idx++, serialized_pass_index++) {
            const SerializedPass* pass = &subshader->passes[pass_idx];
            for (int stage = 0; stage < 6; stage++) {
                for (int sub = 0; sub < pass->subprogram_count[stage]; sub++) {
                    if (!serialized_pass_subprogram_is_platform(pass, stage,
                                                                sub, 4) ||
                        !is_variant_filtered((long long)obj->path_id, stage,
                                             sub, serialized_pass_index)) {
                        continue;
                    }
                    if (stage < 5) {
                        g_dxbc_expected++;
                    } else {
                        g_dxbc_unsupported_stage++;
                    }
                }
            }
        }
    }
    if (g_generated_domain_enabled) {
        discover_generated_d3d11_pass_domains(
            &shader, (long long)obj->path_id, verification_index);
    }
    if (getenv("DXBC_DEBUG_SNIPPETS")) {
        int global_pass = 0;
        for (int s = 0; s < shader.subshader_count; s++) {
            for (int p = 0; p < shader.subshaders[s].pass_count; p++) {
                const SerializedPass* pass = &shader.subshaders[s].passes[p];
                fprintf(stderr,
                        "    [Serialized Pass %d] type=%d gpuProgramID=%d "
                        "name=%s use=%s\n",
                        global_pass++, pass->pass_type,
                        pass->state.gpuProgramID, pass->name, pass->use_name);
                for (int stage = 0; stage < 5; stage++) {
                    for (int sub = 0; sub < pass->subprogram_count[stage]; sub++) {
                        if (!serialized_pass_subprogram_is_platform(
                                pass, stage, sub, 4)) {
                            continue;
                        }
                        fprintf(stderr,
                                "      [Stage %d Sub %d] blob=%d params=%d\n",
                                stage, sub,
                                pass->subprograms[stage][sub].blob_index,
                                pass->subprogram_param_blob_indices[stage][sub]);
                    }
                }
            }
        }
    }

    printf("[VERIFYING COMPILED ARTIFACTS] Shader Name: %s, "
           "path_id=%lld\n", shader.name, (long long)obj->path_id);

    /* Original source is optional diagnostic-parity evidence only when the
     * Shader name maps to exactly one complete file.  It is never the GLCore
     * released-artifact target. Duplicate declarations fail closed for that
     * optional role. */
    const char* original_path = NULL;
    OriginalShaderLookupStatus original_lookup = ORIGINAL_SHADER_NOT_FOUND;
    if (!g_direct_only_enabled) {
        original_lookup = find_original_shader_path(
            shader.name, &original_path);
        if (verification) {
            verification->original_source_unique =
                original_lookup == ORIGINAL_SHADER_UNIQUE;
        }
        if (original_lookup == ORIGINAL_SHADER_AMBIGUOUS) {
            printf("    [INFO] Original source name is ambiguous; GLSL "
                   "source authority is unavailable for '%s'\n",
                   shader.name);
        }
    } else {
        printf("    [INFO] Original-source preprocessing not run "
               "(--direct-only); released artifacts remain the target.\n");
    }
    PreprocessResult orig_prep;
    memset(&orig_prep, 0, sizeof(orig_prep));
    bool has_original_source = false;
    if (original_path) {
        size_t orig_src_size = 0;
        uint8_t* orig_src_buf = read_file_to_buffer(original_path, &orig_src_size);
        if (!orig_src_buf) {
            printf("    [FAIL] Failed to read original shader: %s\n", original_path);
        } else {
            UnityCompilerShaderPreprocessRequest preprocess_request = {
                .source = (const char*)orig_src_buf,
                .file_path = original_path,
                .shader_name = shader.name,
                .surface_only = false,
                .caching_preprocessor = true,
                .build_platform = g_compile_profile.build_platform,
                .valid_apis = g_compile_profile.valid_apis,
            };
            ++g_original_preprocess_authority_requests;
            if (!oracle_preprocess_or_lookup(
                    broker, &preprocess_request, (long long)obj->path_id,
                    "original-shader-preprocess", &orig_prep)) {
            printf("    [FAIL] Preprocessing original shader failed: %s\n", original_path);
            } else {
                has_original_source = true;
                if (verification) {
                    verification->original_preprocess_ok = true;
                }
            }
        }
        free(orig_src_buf);
    } else if (!original_path) {
        printf("    [INFO] Original source unavailable; verifying DXBC target directly\n");
    }

    // Read generated ShaderLab source code & preprocess
    char artifact_name[384];
    if (!shader_artifact_filename(artifact_name, sizeof(artifact_name),
                                  shader.name, obj->path_id)) {
        printf("    [FAIL] Could not construct generated ShaderLab filename\n");
        record_unavailable_shader_variants(&shader, (long long)obj->path_id,
                                           FAILURE_DXBC_COMPILE);
        unity_compiler_free_preprocess(&orig_prep);
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "artifact-name-unavailable");
        return;
    }
    char* shaderlab_scope = join_source_path(shaderlab_dir,
                                             serialized_digest_hex);
    char* shaderlab_path = shaderlab_scope
        ? join_source_path(shaderlab_scope, artifact_name) : NULL;
    free(shaderlab_scope);
    if (!shaderlab_path) {
        printf("    [FAIL] Could not construct generated ShaderLab path\n");
        record_unavailable_shader_variants(&shader, (long long)obj->path_id,
                                           FAILURE_DXBC_COMPILE);
        unity_compiler_free_preprocess(&orig_prep);
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "artifact-path-allocation-failed");
        return;
    }
    if (verification) {
        verification->generated_source_path = copy_string(shaderlab_path);
        if (!verification->generated_source_path) {
            g_shader_report_allocation_failed = true;
        }
    }

    size_t slab_src_size = 0;
    uint8_t* slab_src_buf = read_file_to_buffer(shaderlab_path, &slab_src_size);
    if (!slab_src_buf) {
        printf("    [FAIL] Generated ShaderLab file not found: %s\n", shaderlab_path);
        record_unavailable_shader_variants(&shader, (long long)obj->path_id,
                                           FAILURE_DXBC_COMPILE);
        free(shaderlab_path);
        unity_compiler_free_preprocess(&orig_prep);
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "generated-candidate-missing");
        return;
    }
    if (verification) {
        verification->generated_source_present = true;
    }

    PreprocessResult gen_prep;
    memset(&gen_prep, 0, sizeof(gen_prep));
    UnityCompilerShaderPreprocessRequest generated_preprocess_request = {
        .source = (const char*)slab_src_buf,
        .file_path = shaderlab_path,
        .shader_name = shader.name,
        .surface_only = false,
        .caching_preprocessor = true,
        .build_platform = g_compile_profile.build_platform,
        .valid_apis = g_compile_profile.valid_apis,
    };
    ++g_generated_preprocess_authority_requests;
    if (!oracle_preprocess_or_lookup(
            broker, &generated_preprocess_request,
            (long long)obj->path_id, "generated-shader-preprocess",
            &gen_prep)) {
        printf("    [FAIL] Preprocessing generated ShaderLab failed: %s\n", shaderlab_path);
        record_unavailable_shader_variants(&shader, (long long)obj->path_id,
                                           FAILURE_DXBC_COMPILE);
        free(slab_src_buf);
        free(shaderlab_path);
        unity_compiler_free_preprocess(&orig_prep);
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "generated-preprocess-failed");
        return;
    }
    if (verification) {
        verification->generated_preprocess_ok = true;
    }

    verify_serialized_glcore_targets(
        broker, file, &shader_value, &shader, shader_profile,
        (long long)obj->path_id, verification_index, verification,
        &gen_prep, shaderlab_path, (const char*)slab_src_buf);

    ShaderBlobArchive blob_archive;
    if (!shader_blob_archive_open(&shader_value, 4, &blob_archive)) {
        printf("    [FAIL] Shader has no valid D3D11 blob archive\n");
        record_unavailable_shader_variants(&shader, (long long)obj->path_id,
                                           FAILURE_DXBC_COMPILE);
        free(slab_src_buf);
        free(shaderlab_path);
        unity_compiler_free_preprocess(&gen_prep);
        unity_compiler_free_preprocess(&orig_prep);
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        finalize_shader_result(verification, stats_before,
                               "d3d11-archive-unavailable");
        return;
    }
    if (verification) {
        verification->d3d11_archive_ok = true;
    }
    if (g_generated_domain_enabled) {
        certify_generated_d3d11_pass_domains(
            broker, &shader, &blob_archive, &gen_prep, shaderlab_path,
            has_original_source ? &orig_prep : NULL,
            has_original_source ? original_path : NULL,
            (long long)obj->path_id, verification_index);
    }
    debug_serialized_glsl_archive(&shader, &shader_value);

    if (g_direct_census_enabled) {
        // Loop over D3D11 subprograms.
        for (int subshader_idx = 0; subshader_idx < shader.subshader_count; subshader_idx++) {
            const SerializedSubShader* subshader = &shader.subshaders[subshader_idx];
            for (int pass_idx = 0; pass_idx < subshader->pass_count; pass_idx++) {
                const SerializedPass* pass = &subshader->passes[pass_idx];

                for (int stage_idx = 0; stage_idx < 5; stage_idx++) {
                    for (int sub_idx = 0; sub_idx < pass->subprogram_count[stage_idx]; sub_idx++) {
                        if (!serialized_pass_subprogram_is_platform(
                                pass, stage_idx, sub_idx, 4)) {
                            continue;
                        }
                        int global_pass_idx = 0;
                        for (int s_idx = 0; s_idx < subshader_idx; s_idx++) {
                            global_pass_idx += shader.subshaders[s_idx].pass_count;
                        }
                        global_pass_idx += pass_idx;

                        if (!is_variant_filtered((long long)obj->path_id, stage_idx, sub_idx, global_pass_idx)) {
                            continue;
                        }
                        const SerializedSubProgram* ours_sub = &pass->subprograms[stage_idx][sub_idx];
                        int32_t b_idx = ours_sub->blob_index;
                        const uint8_t* payload = NULL;
                        size_t payload_len = 0;
                        if (!shader_blob_archive_get(&blob_archive, b_idx,
                                                     &payload,
                                                     &payload_len)) {
                            continue;
                        }

                        ByteStream sub_stream;
                        stream_init(&sub_stream, payload, payload_len);
                        stream_set_endian(&sub_stream, false);

                        PlayerSubProgramMetadata sub_meta;
                        if (subprogram_metadata_parse_variant(&sub_stream, &sub_meta)) {
                            g_shaders_total++;
                            printf("[VERIFYING SHADERLAB] Shader object path_id=%lld, subprogram stage=%d, sub_idx=%d (blob_idx=%d, tier=%d, keywords=%d)\n",
                                   (long long)obj->path_id, stage_idx, sub_idx,
                                   ours_sub->blob_index,
                                   ours_sub->has_hardware_tier
                                       ? ours_sub->hardware_tier
                                       : -1,
                                   sub_meta.local_keyword_count + sub_meta.global_keyword_count);

                            OracleMetadataNormalization* oracle_metadata = NULL;
                            if (!prepare_oracle_metadata_for_variant(
                                    &blob_archive, pass, stage_idx, sub_idx,
                                    &oracle_metadata)) {
                                printf("    [ORACLE AUTHORITY UNAVAILABLE] "
                                       "Could not decode exact parameter "
                                       "metadata\n");
                                oracle_variant_authority_failure(
                                    (long long)obj->path_id, stage_idx,
                                    sub_idx, global_pass_idx);
                                subprogram_metadata_free_variant(&sub_meta);
                                continue;
                            }

                            PlayerSubProgramMetadata* p_sub_meta =
                                malloc(sizeof(*p_sub_meta));
                            if (!p_sub_meta) {
                                if (oracle_metadata) {
                                    oracle_metadata_normalization_free(
                                        oracle_metadata);
                                    free(oracle_metadata);
                                }
                                subprogram_metadata_free_variant(&sub_meta);
                                fprintf(stderr, "Out of memory\n");
                                exit(1);
                            }
                            *p_sub_meta = sub_meta;

                            SubProgramJob pending_job = {
                                .shader = &shader,
                                .sub_meta = p_sub_meta,
                                .shader_result_index = verification_index,
                                .path_id = (long long)obj->path_id,
                                .stage = stage_idx,
                                .sub_idx = sub_idx,
                                .pass_idx = global_pass_idx,
                                .subshader_idx = subshader_idx,
                                .local_pass_idx = pass_idx,
                                .pass = pass,
                                .identity =
                                    &pass->subprogram_identities[stage_idx]
                                                                 [sub_idx],
                                .generated_source_path = shaderlab_path,
                                .gen_prep = &gen_prep,
                                .orig_prep = has_original_source
                                    ? &orig_prep : NULL,
                                .shaderlab_content =
                                    (const char*)slab_src_buf,
                                .oracle_metadata = oracle_metadata,
                            };
                            pthread_mutex_lock(&g_job_mutex);
                            if (!enqueue_subprogram_job(&pending_job)) {
                                g_job_queue_failed = true;
                                pthread_mutex_unlock(&g_job_mutex);
                                fprintf(stderr,
                                        "Could not grow verification job "
                                        "queue\n");
                                subprogram_metadata_free_variant(p_sub_meta);
                                free(p_sub_meta);
                                oracle_metadata_normalization_free(
                                    oracle_metadata);
                                free(oracle_metadata);
                                continue;
                            }
                            pthread_cond_signal(&g_job_cond);
                            pthread_mutex_unlock(&g_job_mutex);
                        }
                    }
                }
            }
        }

        // Wait for all queued jobs to finish processing before we free segments and preprocess results
        pthread_mutex_lock(&g_job_mutex);
        while (g_job_queue_size > 0 || g_active_jobs > 0) {
            pthread_cond_wait(&g_done_cond, &g_job_mutex);
        }
        pthread_mutex_unlock(&g_job_mutex);
    } else {
        printf("    [INFO] Direct serialized-subprogram census not run "
               "(--domain-only); the complete generated D3D11 pass domain "
               "remains the active strict gate.\n");
    }

    shader_blob_archive_close(&blob_archive);

    free(slab_src_buf);
    free(shaderlab_path);
    unity_compiler_free_preprocess(&gen_prep);
    unity_compiler_free_preprocess(&orig_prep);
    serialized_shader_free(&shader);
    typetree_free_value(&shader_value);
    finalize_shader_result(verification, stats_before, "completed");
}

typedef struct {
    UnityCompilerBroker* broker;
    TypeTreeSchemaRegistry* schema_registry;
    const char* generated_shaderlab_dir;
    bool serialized_inputs_ok;
    bool allocation_failed;
} VerificationInputVisitorContext;

static bool append_serialized_source_record(
    const UnitySerializedSource* source,
    const char digest_hex[COMMON_SHA256_HEX_SIZE], size_t* out_index) {
    if (out_index) *out_index = SIZE_MAX;
    if (!source || !source->outer_path || !digest_hex) return false;
    if (g_serialized_source_count == g_serialized_source_capacity) {
        size_t capacity = g_serialized_source_capacity == 0U
            ? 32U : g_serialized_source_capacity * 2U;
        if (capacity < g_serialized_source_capacity ||
            capacity > SIZE_MAX / sizeof(*g_serialized_sources)) {
            return false;
        }
        VerificationSerializedSource* records = realloc(
            g_serialized_sources, capacity * sizeof(*records));
        if (!records) return false;
        g_serialized_sources = records;
        g_serialized_source_capacity = capacity;
    }

    VerificationSerializedSource record;
    memset(&record, 0, sizeof(record));
    record.outer_path = copy_string(source->outer_path);
    record.member_name = copy_string(source->member_name);
    if (!record.outer_path || (source->member_name && !record.member_name)) {
        free(record.outer_path);
        free(record.member_name);
        return false;
    }
    record.member_index = source->member_index;
    record.is_bundle_member = source->is_bundle_member;
    memcpy(record.serialized_file_sha256, digest_hex,
           sizeof(record.serialized_file_sha256));
    size_t index = g_serialized_source_count++;
    g_serialized_sources[index] = record;
    g_input_stats.serialized_sources++;
    if (out_index) *out_index = index;
    return true;
}

static bool append_unsupported_compute_shader(
    size_t serialized_source_index, const AssetObjectInfo* object) {
    if (!object || object->type_id != 72) return false;
    if (g_unsupported_compute_shader_count ==
        g_unsupported_compute_shader_capacity) {
        size_t capacity = g_unsupported_compute_shader_capacity == 0U
            ? 16U : g_unsupported_compute_shader_capacity * 2U;
        if (capacity < g_unsupported_compute_shader_capacity ||
            capacity > SIZE_MAX / sizeof(*g_unsupported_compute_shaders)) {
            return false;
        }
        UnsupportedComputeShader* records = realloc(
            g_unsupported_compute_shaders, capacity * sizeof(*records));
        if (!records) return false;
        g_unsupported_compute_shaders = records;
        g_unsupported_compute_shader_capacity = capacity;
    }
    UnsupportedComputeShader* record =
        &g_unsupported_compute_shaders[g_unsupported_compute_shader_count++];
    record->serialized_source_index = serialized_source_index;
    record->path_id = (long long)object->path_id;
    record->object_size = object->byte_size;
    g_input_stats.compute_shader_objects++;
    return true;
}

static void free_input_records(void) {
    for (size_t i = 0U; i < g_serialized_source_count; ++i) {
        free(g_serialized_sources[i].outer_path);
        free(g_serialized_sources[i].member_name);
    }
    free(g_serialized_sources);
    g_serialized_sources = NULL;
    g_serialized_source_count = 0U;
    g_serialized_source_capacity = 0U;
    free(g_unsupported_compute_shaders);
    g_unsupported_compute_shaders = NULL;
    g_unsupported_compute_shader_count = 0U;
    g_unsupported_compute_shader_capacity = 0U;
}

static bool verification_visit_serialized_source(
    const UnitySerializedSource* source, void* opaque_context) {
    VerificationInputVisitorContext* context =
        (VerificationInputVisitorContext*)opaque_context;
    if (!source || !context || !context->broker ||
        !context->generated_shaderlab_dir) {
        return false;
    }

    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char digest_hex[COMMON_SHA256_HEX_SIZE];
    common_sha256(source->data, source->size, digest);
    common_sha256_digest_to_hex(digest, digest_hex);
    size_t source_index = SIZE_MAX;
    if (!append_serialized_source_record(source, digest_hex, &source_index)) {
        context->allocation_failed = true;
        g_input_report_allocation_failed = true;
        return false;
    }
    VerificationSerializedSource* source_record =
        &g_serialized_sources[source_index];

    SerializedFile metadata;
    if (!serialized_file_open_metadata(
            &metadata, source->data, source->size)) {
        fprintf(stderr,
                "Failed to parse SerializedFile metadata: outer='%s' "
                "member=%s member_index=%zu sha256=%s\n",
                source->outer_path,
                source->member_name ? source->member_name : "<standalone>",
                source->member_index, digest_hex);
        context->serialized_inputs_ok = false;
        return true;
    }
    source_record->metadata_parsed = true;
    for (int object_index = 0; object_index < metadata.object_count;
         ++object_index) {
        const AssetObjectInfo* object = &metadata.objects[object_index];
        if (object->type_id == 48) {
            source_record->shader_objects++;
        } else if (object->type_id == 72) {
            source_record->compute_shader_objects++;
            if (!append_unsupported_compute_shader(source_index, object)) {
                serialized_file_close(&metadata);
                context->allocation_failed = true;
                g_input_report_allocation_failed = true;
                return false;
            }
            fprintf(stderr,
                    "[UNSUPPORTED SHADER OBJECT] ClassID=72 path_id=%lld "
                    "outer='%s' member=%s member_index=%zu sha256=%s\n",
                    (long long)object->path_id, source->outer_path,
                    source->member_name ? source->member_name
                                        : "<standalone>",
                    source->member_index, digest_hex);
        }
    }
    source_record->shader_schema_required =
        source_record->shader_objects != 0U;
    if (source_record->shader_schema_required) {
        TypeTreeSchemaStatus schema_status =
            serialized_file_resolve_class_schema(
                &metadata, 48, context->schema_registry);
        if (schema_status != TYPETREE_SCHEMA_OK) {
            fprintf(stderr,
                    "Failed exact ClassID 48 schema resolution: outer='%s' "
                    "member=%s member_index=%zu sha256=%s status=%s\n",
                    source->outer_path,
                    source->member_name ? source->member_name
                                        : "<standalone>",
                    source->member_index, digest_hex,
                    typetree_schema_status_name(schema_status));
            context->serialized_inputs_ok = false;
            serialized_file_close(&metadata);
            return true;
        }
        source_record->shader_schema_resolved = true;
    }
    for (int object_index = 0; object_index < metadata.object_count;
         ++object_index) {
        const AssetObjectInfo* object = &metadata.objects[object_index];
        if (object->type_id == 48) {
            process_shader_object(
                context->broker, &metadata, object,
                context->generated_shaderlab_dir, source, source_index);
        }
    }
    serialized_file_close(&metadata);
    return true;
}

static bool parse_worker_count(const char* text, int* worker_count) {
    char* end = NULL;
    long parsed = text ? strtol(text, &end, 10) : 0;
    if (!text || end == text || *end != '\0' || parsed < 1 || parsed > 64) {
        return false;
    }
    *worker_count = (int)parsed;
    return true;
}

static bool parse_u32_option(const char* text, uint32_t* value) {
    if (!text || !*text || !value || text[0] == '-') return false;
    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (end == text || *end != '\0' || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_source_budget_mib(const char* text, uint64_t* value) {
    if (!text || !*text || !value || text[0] == '-') return false;
    errno = 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    const uint64_t max_mib = UINT64_MAX / UINT64_C(1048576);
    if (errno == ERANGE || end == text || *end != '\0' ||
        (uint64_t)parsed > max_mib) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_capability_option(
    const char* text, UnityPlatformCapabilitySnapshot* snapshot) {
    if (!text || !*text || !snapshot || text[0] == '-') return false;
    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    const uint64_t valid_bits =
        (UINT64_C(1) << UNITY_PLATFORM_CAPABILITY_COUNT) - UINT64_C(1);
    if (end == text || *end != '\0' || ((uint64_t)parsed & ~valid_bits)) {
        return false;
    }
    snapshot->present = true;
    snapshot->bits = (uint64_t)parsed;
    return true;
}

static bool apply_compile_profile_environment(
    const VerificationCompileOverrides* command_line,
    bool* has_build_platform, bool* has_valid_apis,
    uint32_t* applied_overrides) {
    const char* build_platform = getenv("DXBC_BUILD_PLATFORM");
    const char* valid_apis = getenv("DXBC_VALID_APIS");
    const char* d3d_capabilities = getenv("DXBC_D3D11_PLATFORM_CAPS");
    const char* gl_capabilities = getenv("DXBC_GLCORE_PLATFORM_CAPS");
    if (!command_line->has_build_platform &&
        build_platform && *build_platform) {
        if (!parse_u32_option(build_platform,
                              &g_compile_profile.build_platform)) {
            return false;
        }
        *has_build_platform = true;
        *applied_overrides |= COMPILE_OVERRIDE_BUILD_PLATFORM;
    }
    if (!command_line->has_valid_apis && valid_apis && *valid_apis) {
        if (!parse_u32_option(valid_apis, &g_compile_profile.valid_apis)) {
            return false;
        }
        *has_valid_apis = true;
        *applied_overrides |= COMPILE_OVERRIDE_VALID_APIS;
    }
    if (!command_line->d3d11_capabilities.present &&
        d3d_capabilities && *d3d_capabilities &&
        !parse_capability_option(
            d3d_capabilities, &g_compile_profile.d3d11_capabilities)) {
        return false;
    }
    if (!command_line->d3d11_capabilities.present &&
        d3d_capabilities && *d3d_capabilities) {
        *applied_overrides |= COMPILE_OVERRIDE_D3D11_CAPABILITIES;
    }
    if (!command_line->glcore_capabilities.present &&
        gl_capabilities && *gl_capabilities &&
        !parse_capability_option(
            gl_capabilities, &g_compile_profile.glcore_capabilities)) {
        return false;
    }
    if (!command_line->glcore_capabilities.present &&
        gl_capabilities && *gl_capabilities) {
        *applied_overrides |= COMPILE_OVERRIDE_GLCORE_CAPABILITIES;
    }
    return true;
}

static void apply_compile_profile_command_line(
    const VerificationCompileOverrides* overrides,
    bool* has_build_platform, bool* has_valid_apis,
    uint32_t* applied_overrides) {
    if (overrides->has_build_platform) {
        g_compile_profile.build_platform = overrides->build_platform;
        *has_build_platform = true;
        *applied_overrides |= COMPILE_OVERRIDE_BUILD_PLATFORM;
    }
    if (overrides->has_valid_apis) {
        g_compile_profile.valid_apis = overrides->valid_apis;
        *has_valid_apis = true;
        *applied_overrides |= COMPILE_OVERRIDE_VALID_APIS;
    }
    if (overrides->d3d11_capabilities.present) {
        g_compile_profile.d3d11_capabilities =
            overrides->d3d11_capabilities;
        *applied_overrides |= COMPILE_OVERRIDE_D3D11_CAPABILITIES;
    }
    if (overrides->glcore_capabilities.present) {
        g_compile_profile.glcore_capabilities =
            overrides->glcore_capabilities;
        *applied_overrides |= COMPILE_OVERRIDE_GLCORE_CAPABILITIES;
    }
}

static bool load_verification_compile_profile(
    const char* path, bool* has_build_platform, bool* has_valid_apis,
    UnityCompileProfile* loaded_profile, bool* profile_loaded) {
    if (!path) return true;
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    UnityCompileProfileStatus status =
        unity_compile_profile_load(path, &profile);
    if (status != UNITY_COMPILE_PROFILE_OK) {
        fprintf(stderr, "Failed to load compile profile '%s': %s\n", path,
                unity_compile_profile_status_string(status));
        return false;
    }
    g_compile_profile.build_platform = profile.build_platform;
    g_compile_profile.valid_apis = profile.valid_apis;
    g_compile_profile.d3d11_capabilities.present = true;
    g_compile_profile.d3d11_capabilities.bits =
        profile.d3d11_capabilities;
    g_compile_profile.glcore_capabilities.present = true;
    g_compile_profile.glcore_capabilities.bits =
        profile.glcore_capabilities;
    *has_build_platform = true;
    *has_valid_apis = true;

    *loaded_profile = profile;
    *profile_loaded = true;

    return true;
}

static void fingerprint_to_hex(
    const uint8_t digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE],
    char fingerprint[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U + 1U]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE; i++) {
        fingerprint[i * 2U] = digits[digest[i] >> 4U];
        fingerprint[i * 2U + 1U] =
            digits[digest[i] & 0x0fU];
    }
    fingerprint[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U] = '\0';
}

static bool report_effective_compile_profile(
    const char* path, const UnityCompileProfile* loaded_profile,
    bool profile_loaded, uint32_t environment_overrides,
    uint32_t command_line_overrides) {
    if (profile_loaded) {
        char base_fingerprint[
            UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U + 1U];
        fingerprint_to_hex(loaded_profile->fingerprint, base_fingerprint);
        printf("[INFO] Loaded base compile profile %s\n", path);
        printf("[INFO] Base compile profile provenance: %s\n",
               loaded_profile->provenance);
        printf("[INFO] Base compile profile fingerprint: %s\n",
               base_fingerprint);
    }

    UnityCompileProfile effective;
    unity_compile_profile_init(&effective);
    effective.build_platform = g_compile_profile.build_platform;
    effective.valid_apis = g_compile_profile.valid_apis;
    effective.d3d11_capabilities =
        g_compile_profile.d3d11_capabilities.bits;
    effective.glcore_capabilities =
        g_compile_profile.glcore_capabilities.bits;
    if (profile_loaded && environment_overrides == 0U &&
        command_line_overrides == 0U) {
        memcpy(effective.provenance, loaded_profile->provenance,
               sizeof(effective.provenance));
    } else if (profile_loaded) {
        strcpy(effective.provenance,
               "effective-authority:profile-plus-overrides");
    } else if (environment_overrides != 0U &&
               command_line_overrides != 0U) {
        strcpy(effective.provenance,
               "effective-authority:environment-plus-command-line");
    } else if (environment_overrides != 0U) {
        strcpy(effective.provenance,
               "effective-authority:environment-scalars");
    } else {
        strcpy(effective.provenance,
               "effective-authority:command-line-scalars");
    }
    uint8_t digest[UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE];
    UnityCompileProfileStatus status =
        unity_compile_profile_fingerprint(&effective, digest);
    if (status != UNITY_COMPILE_PROFILE_OK) {
        fprintf(stderr, "Could not fingerprint effective compile profile: %s\n",
                unity_compile_profile_status_string(status));
        return false;
    }
    memcpy(effective.fingerprint, digest, sizeof(effective.fingerprint));
    g_effective_compile_profile = effective;
    char fingerprint[
        UNITY_COMPILE_PROFILE_FINGERPRINT_SIZE * 2U + 1U];
    fingerprint_to_hex(digest, fingerprint);
    printf("[INFO] Effective compile authority provenance: %s\n",
           effective.provenance);
    printf("[INFO] Effective compile authority fingerprint: %s\n",
           fingerprint);
    printf("[INFO] Applied authority override masks: environment=0x%02x, "
           "command-line=0x%02x\n",
           environment_overrides, command_line_overrides);
    return true;
}

static void stop_worker_threads(pthread_t* workers, int worker_count) {
    pthread_mutex_lock(&g_job_mutex);
    g_should_exit = true;
    pthread_cond_broadcast(&g_job_cond);
    pthread_mutex_unlock(&g_job_mutex);
    for (int i = 0; i < worker_count; ++i) {
        pthread_join(workers[i], NULL);
    }
    free(workers);
    free(g_job_queue);
    g_job_queue = NULL;
    g_job_queue_size = 0U;
    g_job_queue_capacity = 0U;
}

static void json_write_string(FILE* output, const char* value) {
    fputc('"', output);
    if (value) {
        const unsigned char* cursor = (const unsigned char*)value;
        while (*cursor) {
            unsigned char ch = *cursor++;
            switch (ch) {
                case '"': fputs("\\\"", output); break;
                case '\\': fputs("\\\\", output); break;
                case '\b': fputs("\\b", output); break;
                case '\f': fputs("\\f", output); break;
                case '\n': fputs("\\n", output); break;
                case '\r': fputs("\\r", output); break;
                case '\t': fputs("\\t", output); break;
                default:
                    if (ch < 0x20U) {
                        fprintf(output, "\\u%04x", (unsigned int)ch);
                    } else {
                        fputc((int)ch, output);
                    }
                    break;
            }
        }
    }
    fputc('"', output);
}

static void json_write_reflection_record_summary(
    FILE* output,
    const UnityReflectionCertificateRecordSummary* summary) {
    if (!summary || !summary->present) {
        fputs("null", output);
        return;
    }
    fputs("{\"kind\":", output);
    json_write_string(
        output, unity_compiler_reflection_kind_name(summary->kind));
    fputs(",\"name\":", output);
    if (summary->has_name) {
        char digest[COMMON_SHA256_HEX_SIZE];
        common_sha256_digest_to_hex(summary->name_sha256, digest);
        json_write_string(output, summary->name_preview);
        fprintf(output, ",\"name_length\":%zu,\"name_sha256\":",
                summary->name_length);
        json_write_string(output, digest);
    } else {
        fputs("null,\"name_length\":0,\"name_sha256\":null", output);
    }
    fputs(",\"values\":[", output);
    for (size_t value_index = 0U;
         value_index < summary->value_count; ++value_index) {
        fprintf(output, "%s%" PRId32,
                value_index ? "," : "", summary->values[value_index]);
    }
    fputs("]}", output);
}

static bool generated_domain_counts_are_exact(
    const GeneratedDomainCounts* counts) {
    return counts && counts->filter_admitted_passes > 0U &&
           counts->failed_passes == 0U &&
           counts->plans_built == counts->filter_admitted_passes &&
           counts->snippets_uniquely_mapped ==
               counts->filter_admitted_passes &&
           counts->certified_passes == counts->filter_admitted_passes &&
           counts->planned_compiles > 0U &&
           counts->compile_attempts == counts->planned_compiles &&
           counts->clean_compiles <= counts->planned_compiles &&
           counts->diagnostic_attested_compiles <=
               counts->planned_compiles - counts->clean_compiles &&
           counts->clean_compiles +
                   counts->diagnostic_attested_compiles ==
               counts->planned_compiles &&
           counts->matched_dxbc_containers == counts->planned_compiles &&
           counts->runtime_binding_attested_compiles <=
               counts->planned_compiles &&
           counts->runtime_binding_compatible_compiles <=
               counts->planned_compiles -
                   counts->runtime_binding_attested_compiles &&
           counts->runtime_binding_attested_compiles +
                   counts->runtime_binding_compatible_compiles ==
               counts->planned_compiles;
}

static size_t generated_domain_unreached_passes(
    const GeneratedDomainCounts* counts) {
    if (!counts ||
        counts->certified_passes > counts->filter_admitted_passes ||
        counts->failed_passes >
            counts->filter_admitted_passes - counts->certified_passes) {
        return 0U;
    }
    return counts->filter_admitted_passes - counts->certified_passes -
           counts->failed_passes;
}

static const char* generated_domain_counts_status(
    const GeneratedDomainCounts* counts) {
    if (!g_generated_domain_enabled) return "not-run";
    if (!counts || counts->eligible_passes == 0U) return "not-applicable";
    if (counts->filter_admitted_passes == 0U) return "filtered-out";
    if (generated_domain_counts_are_exact(counts)) return "exact";
    return counts->failed_passes > 0U ? "failed" : "incomplete";
}

static const char* runtime_binding_counts_status(
    const GeneratedDomainCounts* counts) {
    if (!g_generated_domain_enabled) return "not-run";
    if (!counts || counts->planned_compiles == 0U) return "not-applicable";
    if (counts->runtime_binding_attested_compiles ==
        counts->planned_compiles) {
        return "exact";
    }
    if (counts->runtime_binding_attested_compiles <=
            counts->planned_compiles &&
        counts->runtime_binding_compatible_compiles ==
            counts->planned_compiles -
                counts->runtime_binding_attested_compiles) {
        return "compatible";
    }
    return "failed";
}

static const char* direct_census_counts_status(void) {
    if (!g_direct_census_enabled) return "not-run";
    if (g_dxbc_expected == 0 && g_dxbc_unsupported_stage == 0) {
        return "not-applicable";
    }
    if (g_dxbc_expected > 0 && g_dxbc_unsupported_stage == 0 &&
        g_shaders_total == g_dxbc_expected &&
        g_dxbc_compiled == g_dxbc_expected &&
        g_dxbc_token_matched == g_dxbc_expected &&
        g_dxbc_matched == g_dxbc_expected) {
        return "exact";
    }
    return "failed";
}

static const char* shader_dxbc_status(
    const ShaderVerificationResult* result) {
    if (!g_direct_census_enabled) return "not-run";
    if (!result->parsed) return "not-evaluated";
    if (result->dxbc_expected == 0 &&
        result->unsupported_stage_variants == 0) {
        return "not-applicable";
    }
    if (result->dxbc_expected > 0 &&
        result->unsupported_stage_variants == 0 &&
        result->scheduled_and_decoded == result->dxbc_expected &&
        result->dxbc_compiled == result->dxbc_expected &&
        result->dxbc_token_matched == result->dxbc_expected &&
        result->dxbc_byte_matched == result->dxbc_expected) {
        return "exact";
    }
    return "failed";
}

static bool serialized_glcore_counts_are_exact(
    const SerializedGLCoreCounts* counts) {
    if (!counts || counts->objects_evaluated == 0U ||
        counts->unavailable_objects != 0U ||
        counts->targets_unavailable != 0U ||
        counts->oracle_pack_v4_unsupported != 0U ||
        counts->targets_mismatched != 0U) {
        return false;
    }
    if (counts->platform_present_objects == 0U) {
        return counts->platform_absent_objects ==
               counts->objects_evaluated;
    }
    return counts->targets_expected > 0U &&
        counts->targets_opened == counts->targets_expected &&
        counts->compile_attempts == counts->targets_expected &&
        counts->targets_compiled == counts->targets_expected &&
        counts->targets_exact == counts->targets_expected;
}

static const char* serialized_glcore_counts_status(
    const SerializedGLCoreCounts* counts) {
    if (!g_verify_glsl) return "not-run";
    if (!counts || counts->objects_evaluated == 0U) {
        return "not-evaluated";
    }
    if (counts->unavailable_objects != 0U ||
        counts->targets_unavailable != 0U ||
        counts->oracle_pack_v4_unsupported != 0U ||
        counts->targets_mismatched != 0U) {
        return "failed";
    }
    if (counts->platform_present_objects == 0U &&
        counts->platform_absent_objects == counts->objects_evaluated) {
        return "platform-absent-not-applicable";
    }
    if (counts->targets_expected == 0U &&
        counts->targets_filtered_out > 0U) {
        return "filtered-out";
    }
    return serialized_glcore_counts_are_exact(counts)
        ? "exact" : "incomplete";
}

static const char* shader_glsl_status(
    const ShaderVerificationResult* result) {
    if (!result || !result->parsed ||
        !result->glcore_readiness_evaluated) {
        return g_verify_glsl ? "not-evaluated" : "not-run";
    }
    return serialized_glcore_counts_status(&result->serialized_glcore);
}

static const char* shader_compiler_diagnostic_status(
    const ShaderVerificationResult* result) {
    if (result->diagnostic_authority_failure_count > 0U) {
        return "diagnostic-authority-unavailable";
    }
    if (result->unattested_actionable_compiler_diagnostic_count > 0U) {
        return "diagnostics-emitted";
    }
    if (result->attested_actionable_compiler_diagnostic_count > 0U) {
        return "source-equivalent-diagnostics-attested";
    }
    if (result->informational_compiler_diagnostic_count > 0U) {
        return "informational-records-retained";
    }
    if (!result->parsed || !result->generated_source_present) {
        return "not-run";
    }
    return "clean";
}

static const char* verification_scope_name(void) {
    if (g_direct_only_enabled) {
        return g_verify_glsl
            ? "filter-selected-serialized-dxbc-subprograms-plus-"
              "serialized-glcore-where-present"
            : "filter-selected-serialized-dxbc-subprograms-d3d11-only";
    }
    if (!g_direct_census_enabled) {
        return "whole-generated-d3d11-pass-domains";
    }
    return g_verify_glsl
        ? "serialized-dxbc-and-glcore-plus-whole-generated-d3d11-"
          "pass-domains"
        : "serialized-dxbc-plus-whole-generated-d3d11-pass-domains";
}

static const char* verification_mode_name(void) {
    if (g_direct_only_enabled) {
        return g_verify_glsl
            ? "direct-dxbc-and-serialized-glcore"
            : "direct-dxbc-only";
    }
    if (!g_direct_census_enabled) return "generated-domain-only";
    return g_verify_glsl
        ? "generated-domain-plus-direct-dxbc-and-serialized-glcore"
        : "generated-domain-plus-direct-dxbc";
}

static const char* tuple_filter_semantics_name(void) {
    if (!g_direct_only_enabled) {
        return "admits-pass-then-certifies-entire-generated-d3d11-domain";
    }
    return g_verify_glsl
        ? "selects-exact-d3d11-rows-and-admits-corresponding-glcore-"
          "pass-targets"
        : "selects-exact-d3d11-serialized-subprogram-rows";
}

static void write_failure_kind_names(FILE* output, uint32_t failure_kinds) {
    static const struct {
        uint32_t bit;
        const char* name;
    } names[] = {
        {FAILURE_DXBC_COMPILE, "dxbc-compile"},
        {FAILURE_DXBC_DISASSEMBLE, "dxbc-container"},
        {FAILURE_DXBC_MISMATCH, "dxbc-byte-mismatch"},
        {FAILURE_GLSL_ORIGINAL_COMPILE,
         "deprecated-glsl-original-compile"},
        {FAILURE_GLSL_GENERATED_COMPILE, "glsl-generated-compile"},
        {FAILURE_GLSL_DISASSEMBLE, "glsl-container"},
        {FAILURE_GLSL_MISMATCH, "glsl-byte-mismatch"},
        {FAILURE_GLSL_ORACLE_MAPPING,
         "deprecated-glsl-source-oracle-mapping"},
        {FAILURE_COMPILE_AUTHORITY, "compile-authority"},
        {FAILURE_ORACLE_AUTHORITY, "oracle-authority"},
        {FAILURE_PREPROCESS_DIAGNOSTIC, "preprocess-diagnostic"},
        {FAILURE_COMPILE_DIAGNOSTIC, "compile-diagnostic"},
        {FAILURE_DISASSEMBLE_DIAGNOSTIC, "disassemble-diagnostic"},
        {FAILURE_DIAGNOSTIC_AUTHORITY, "diagnostic-authority"},
        {FAILURE_GENERATED_DOMAIN, "d3d11-generated-domain"},
        {FAILURE_RUNTIME_BINDING, "runtime-binding"},
        {FAILURE_GLCORE_TARGET_UNAVAILABLE,
         "serialized-glcore-target-unavailable"},
        {FAILURE_GLCORE_ORACLE_UNSUPPORTED,
         "serialized-glcore-oracle-pack-v4-unsupported"},
    };
    fputc('[', output);
    bool first = true;
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        if ((failure_kinds & names[i].bit) == 0U) continue;
        if (!first) fputc(',', output);
        json_write_string(output, names[i].name);
        first = false;
    }
    fputc(']', output);
}

static const char* valid_apis_authority_status_name(
    UnityCompilerValidApisAuthorityStatus status) {
    switch (status) {
        case UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED:
            return "not-configured";
        case UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING:
            return "pending-process-free";
        case UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED:
            return "exact-live-match";
        case UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH:
            return "live-mismatch";
        case UNITY_COMPILER_VALID_APIS_AUTHORITY_SESSION_FAILED:
            return "initializeCompiler-session-failed";
    }
    return "invalid";
}

static bool write_verification_report(
    const char* report_path, const char* unity_input_path,
    const char* original_shader_dir, const char* generated_shaderlab_dir,
    bool serialized_inputs_ok, bool oracle_capture_ok, bool diagnostics_ok,
    bool verification_passed, const VerificationScopeResult* scope,
    const UnityCompilerBrokerStats* broker_stats,
    const UnityCompilerSessionCapabilities* compiler_session,
    const UnityCompilerValidApisAuthority* valid_apis_authority) {
    if (!report_path) return true;
    char* compiler_session_json = compiler_session
        ? unity_compiler_session_capabilities_format_json(compiler_session)
        : NULL;
    if (compiler_session && !compiler_session_json) {
        fprintf(stderr,
                "Could not format retained UnityShaderCompiler session "
                "authority\n");
        return false;
    }
    int descriptor = open(report_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        fprintf(stderr, "Could not create verification report '%s': %s\n",
                report_path, strerror(errno));
        free(compiler_session_json);
        return false;
    }
    FILE* output = fdopen(descriptor, "w");
    if (!output) {
        fprintf(stderr, "Could not open verification report '%s': %s\n",
                report_path, strerror(errno));
        close(descriptor);
        free(compiler_session_json);
        return false;
    }

    bool diagnostic_gate_clean =
        g_compiler_diagnostic_unattested_actionable_count == 0U &&
        g_diagnostic_authority_failure_count == 0U &&
        !g_compiler_diagnostic_allocation_failed;
    fputs("{\n  \"report_schema\":\"dxbc-sandbox-compiled-artifact-"
          "verification\",\n  \"report_version\":12,\n", output);
    fputs("  \"verification_scope\":", output);
    json_write_string(output, verification_scope_name());
    fputs(",\n  \"verification_mode\":", output);
    json_write_string(output, verification_mode_name());
    fputs(",\n", output);
    fputs("  \"candidate_kind\":\"generated-shaderlab-candidate\",\n",
          output);
    fputs("  \"result\":", output);
    json_write_string(output,
                      verification_passed && diagnostics_ok &&
                              diagnostic_gate_clean
                          ? "passed" : "failed");
    fputs(",\n  \"certification\":{\n", output);
    fputs("    \"compiled_artifact_equivalence\":", output);
    json_write_string(output,
                      verification_passed && diagnostic_gate_clean
                          ? "exact" : "failed");
    fputs(",\n    \"compiled_artifact_platform_scope\":", output);
    json_write_string(
        output, g_verify_glsl
            ? "d3d11-plus-serialized-glcore-where-present"
            : "d3d11-only");
    fputs(",\n    \"serialized_glcore_released_text\":", output);
    json_write_string(
        output, serialized_glcore_counts_status(&g_serialized_glcore));
    fputs(",\n    \"direct_serialized_subprogram_census\":", output);
    json_write_string(output, direct_census_counts_status());
    fputs(",\n    \"whole_generated_d3d11_pass_domain\":", output);
    json_write_string(
        output, generated_domain_counts_status(&g_generated_domain));
    fputs(",\n    \"active_stage_runtime_binding_callbacks\":", output);
    json_write_string(
        output, runtime_binding_counts_status(&g_generated_domain));
    fputs(",\n    \"tuple_filter_semantics\":", output);
    json_write_string(output, tuple_filter_semantics_name());
    fputs(",\n    \"selected_class48_scope\":", output);
    json_write_string(output,
                      scope && scope->selected_scope_exact
                          ? "exact" : "failed");
    fputs(",\n    \"whole_input_object_kind_coverage\":", output);
    json_write_string(
        output,
        scope && scope->whole_input_object_kind_coverage_complete
            ? "complete" : "incomplete");
    fprintf(output, ",\n    \"full_input_coverage_requested\":%s",
            g_filter_count == 0 ? "true" : "false");
    fputs(",\n    \"whole_shader\":\"not-certified\",\n"
          "    \"not_verified\":[\"runtime-keyword-selection\","
          "\"render-and-pipeline-state-roundtrip\","
          "\"unused-or-unreferenced-shaderlab-properties\","
          "\"visual-output\"", output);
    if (!g_direct_census_enabled) {
        fputs(",\"direct-serialized-subprogram-census\"", output);
    }
    if (!g_generated_domain_enabled) {
        fputs(",\"whole-generated-d3d11-pass-domain\","
              "\"original-source-diagnostic-parity\"", output);
    }
    fputs("]\n  },\n", output);
    fputs("  \"inputs\":{\"unity_input\":", output);
    json_write_string(output, unity_input_path);
    /* Keep the legacy key so existing single-bundle report consumers remain
     * source-compatible. It now carries the same first positional argument,
     * which may also be a standalone SerializedFile or directory. */
    fputs(",\"bundle\":", output);
    json_write_string(output, unity_input_path);
    fputs(",\"original_shader_directory\":", output);
    json_write_string(output, original_shader_dir);
    fputs(",\"generated_shaderlab_directory\":", output);
    json_write_string(output, generated_shaderlab_dir);
    fputs("},\n", output);
    fprintf(output,
            "  \"preprocess_authority\":{"
            "\"generated_candidate_requests\":%zu,"
            "\"original_source_requests\":%zu,"
            "\"generated_candidate_purpose\":",
            g_generated_preprocess_authority_requests,
            g_original_preprocess_authority_requests);
    json_write_string(
        output, g_direct_only_enabled
            ? "exact-compileSnippet-source-and-contract-only"
            : "exact-compileSnippet-source-contract-and-generated-domain");
    fputs(",\"original_source_policy\":", output);
    json_write_string(
        output, g_direct_only_enabled
            ? "not-run-explicit-direct-only"
            : "optional-diagnostic-parity-when-uniquely-available");
    fputs(",\"generated_domain_requested\":", output);
    fputs(g_generated_domain_enabled ? "true" : "false", output);
    fputs("},\n", output);
    fprintf(output,
            "  \"integrity\":{\"serialized_inputs_ok\":%s,"
            "\"scheduler_ok\":%s,\"oracle_capture_ok\":%s,"
            "\"diagnostic_outputs_ok\":%s,"
            "\"compiler_diagnostic_capture_complete\":%s,"
            "\"compiler_source_residency_tracking_complete\":%s,"
            "\"per_shader_report_complete\":%s,"
            "\"generated_domain_report_complete\":%s,"
            "\"serialized_glcore_report_complete\":%s,"
            "\"input_report_complete\":%s,"
            "\"selected_class48_scope_exact\":%s,"
            "\"whole_input_object_kind_coverage_complete\":%s,"
            "\"unsupported_compute_shaders_absent\":%s},\n",
            serialized_inputs_ok ? "true" : "false",
            g_job_queue_failed ? "false" : "true",
            oracle_capture_ok ? "true" : "false",
            diagnostics_ok ? "true" : "false",
            g_compiler_diagnostic_allocation_failed ? "false" : "true",
            broker_stats->source_tracking_failures == 0U ? "true" : "false",
            g_shader_report_allocation_failed ? "false" : "true",
            g_generated_domain_report_allocation_failed ? "false" : "true",
            g_serialized_glcore_report_allocation_failed
                ? "false" : "true",
            g_input_report_allocation_failed ? "false" : "true",
            scope && scope->selected_scope_exact ? "true" : "false",
            scope && scope->whole_input_object_kind_coverage_complete
                ? "true" : "false",
            g_unsupported_compute_shader_count == 0U ? "true" : "false");
    fprintf(output,
            "  \"totals\":{\"requested_inputs\":%zu,"
            "\"discovered_files\":%zu,"
            "\"ignored_unrelated_descendants\":%zu,"
            "\"unityfs_files\":%zu,"
            "\"standalone_serialized_files\":%zu,"
            "\"serialized_sources\":%zu,"
            "\"resource_members\":%zu,"
            "\"directory_members\":%zu,"
            "\"deleted_members\":%zu,"
            "\"compute_shader_objects_unsupported\":%zu,"
            "\"shader_objects_seen\":%d,"
            "\"shader_objects_parsed\":%d,\"dxbc_expected\":%d,"
            "\"generated_preprocess_authority_requests\":%zu,"
            "\"original_preprocess_authority_requests\":%zu,"
            "\"scheduled_and_decoded\":%d,"
            "\"unsupported_stage_variants\":%d,"
            "\"dxbc_compiled\":%d,\"dxbc_token_matched\":%d,"
            "\"dxbc_byte_matched\":%d,"
            "\"glcore_objects_evaluated\":%zu,"
            "\"glcore_platform_present_objects\":%zu,"
            "\"glcore_platform_absent_objects\":%zu,"
            "\"glcore_unavailable_objects\":%zu,"
            "\"glcore_targets_discovered\":%zu,"
            "\"glcore_targets_expected\":%zu,"
            "\"glcore_targets_filtered_out\":%zu,"
            "\"glcore_targets_opened\":%zu,"
            "\"glcore_compile_attempts\":%zu,"
            "\"glcore_targets_compiled\":%zu,"
            "\"glcore_targets_exact\":%zu,"
            "\"glcore_targets_mismatched\":%zu,"
            "\"glcore_targets_unavailable\":%zu,"
            "\"glcore_oracle_pack_v4_unsupported\":%zu,"
            "\"failure_records\":%d,"
            "\"compiler_diagnostics_observed\":%zu,"
            "\"compiler_diagnostics_retained\":%zu,"
            "\"compiler_diagnostics_informational\":%zu,"
            "\"compiler_diagnostics_actionable\":%zu,"
            "\"compiler_diagnostics_actionable_attested\":%zu,"
            "\"compiler_diagnostics_actionable_unattested\":%zu,"
            "\"diagnostic_authority_failures\":%zu},\n",
            g_input_stats.requested_inputs,
            g_input_stats.discovered_files,
            g_input_stats.ignored_unrelated_descendants,
            g_input_stats.unityfs_files,
            g_input_stats.standalone_serialized_files,
            g_input_stats.serialized_sources,
            g_input_stats.resource_members,
            g_input_stats.directory_members,
            g_input_stats.deleted_members,
            g_input_stats.compute_shader_objects,
            g_shader_objects_seen, g_shader_objects_parsed, g_dxbc_expected,
            g_generated_preprocess_authority_requests,
            g_original_preprocess_authority_requests,
            g_shaders_total, g_dxbc_unsupported_stage, g_dxbc_compiled,
            g_dxbc_token_matched, g_dxbc_matched,
            g_serialized_glcore.objects_evaluated,
            g_serialized_glcore.platform_present_objects,
            g_serialized_glcore.platform_absent_objects,
            g_serialized_glcore.unavailable_objects,
            g_serialized_glcore.targets_discovered,
            g_serialized_glcore.targets_expected,
            g_serialized_glcore.targets_filtered_out,
            g_serialized_glcore.targets_opened,
            g_serialized_glcore.compile_attempts,
            g_serialized_glcore.targets_compiled,
            g_serialized_glcore.targets_exact,
            g_serialized_glcore.targets_mismatched,
            g_serialized_glcore.targets_unavailable,
            g_serialized_glcore.oracle_pack_v4_unsupported,
            g_failure_count,
            g_compiler_diagnostic_observed_count,
            g_compiler_diagnostic_count,
            g_compiler_diagnostic_informational_count,
            g_compiler_diagnostic_actionable_count,
            g_compiler_diagnostic_attested_actionable_count,
            g_compiler_diagnostic_unattested_actionable_count,
            g_diagnostic_authority_failure_count);
    fputs("  \"direct_serialized_subprogram_census\":{\"status\":",
          output);
    json_write_string(output, direct_census_counts_status());
    fprintf(output,
            ",\"requested\":%s,\"serialized_variants_inventory\":%d,"
            "\"filter_semantics\":\"exact-serialized-row\","
            "\"scheduled_and_decoded\":%d,\"compiled\":%d,"
            "\"executable_token_matched\":%d,"
            "\"full_container_matched\":%d,\"skip_reason\":",
            g_direct_census_enabled ? "true" : "false",
            g_dxbc_expected, g_shaders_total, g_dxbc_compiled,
            g_dxbc_token_matched, g_dxbc_matched);
    if (g_direct_census_enabled) {
        fputs("null", output);
    } else {
        json_write_string(output, "explicit-generated-domain-only-mode");
    }
    fputs("},\n", output);
    fputs("  \"serialized_glcore_gate\":{\"status\":", output);
    json_write_string(
        output, serialized_glcore_counts_status(&g_serialized_glcore));
    fputs(",\"authority\":\"released-platform-15-linked-text\","
          "\"target_identity\":\"all-serialized-platform-15-owners\","
          "\"supported_target_identity\":"
          "\"type-6-stage-0-tier-group-3-linked-owner\","
          "\"filter_semantics\":\"tuple-admits-corresponding-pass-targets\","
          "\"candidate_source\":\"generated-shaderlab-only\","
          "\"original_source_required\":false,"
          "\"fragment_companion_required\":false,"
          "\"oracle_pack_v4_used_for_serialized_targets\":false,"
          "\"oracle_pack_v4_policy\":"
          "\"strict-replay-unsupported-independent-owner-identity\","
          "\"non_strict_oracle_policy\":"
          "\"bypass-pack-use-persistent-cache-or-live-compiler\","
          "\"normal_compiler_cache_usable\":true", output);
    fprintf(output,
            ",\"objects_evaluated\":%zu,"
            "\"platform_present_objects\":%zu,"
            "\"platform_absent_objects\":%zu,"
            "\"unavailable_objects\":%zu,"
            "\"targets_discovered\":%zu,"
            "\"targets_expected\":%zu,"
            "\"targets_filtered_out\":%zu,"
            "\"targets_opened\":%zu,"
            "\"compile_attempts\":%zu,"
            "\"targets_compiled\":%zu,"
            "\"targets_exact\":%zu,"
            "\"targets_mismatched\":%zu,"
            "\"targets_unavailable\":%zu,"
            "\"oracle_pack_v4_unsupported\":%zu,"
            "\"report_allocation_complete\":%s},\n",
            g_serialized_glcore.objects_evaluated,
            g_serialized_glcore.platform_present_objects,
            g_serialized_glcore.platform_absent_objects,
            g_serialized_glcore.unavailable_objects,
            g_serialized_glcore.targets_discovered,
            g_serialized_glcore.targets_expected,
            g_serialized_glcore.targets_filtered_out,
            g_serialized_glcore.targets_opened,
            g_serialized_glcore.compile_attempts,
            g_serialized_glcore.targets_compiled,
            g_serialized_glcore.targets_exact,
            g_serialized_glcore.targets_mismatched,
            g_serialized_glcore.targets_unavailable,
            g_serialized_glcore.oracle_pack_v4_unsupported,
            g_serialized_glcore_report_allocation_failed
                ? "false" : "true");
    fputs("  \"serialized_glcore_targets\":[\n", output);
    for (size_t i = 0U; i < g_serialized_glcore_target_count; ++i) {
        const SerializedGLCoreTargetRecord* record =
            &g_serialized_glcore_targets[i];
        fprintf(output,
                "    %s{\"shader_result_index\":%zu,"
                "\"path_id\":%lld,\"subshader_index\":%d,"
                "\"local_pass_index\":%d,"
                "\"serialized_pass_index\":%d,"
                "\"serialized_stage\":%d,"
                "\"flattened_subprogram_index\":%d,"
                "\"program_type\":%d,"
                "\"hardware_tier_group\":%d,"
                "\"inner_subprogram_index\":%d,"
                "\"archive_entry_index\":%d,\"status\":",
                i ? "," : "", record->shader_result_index,
                record->path_id, record->subshader_index,
                record->local_pass_index,
                record->serialized_pass_index,
                record->serialized_stage,
                record->flattened_subprogram_index,
                record->program_type,
                record->hardware_tier_group,
                record->inner_subprogram_index,
                record->archive_entry_index);
        json_write_string(
            output, serialized_glcore_verification_status_name(
                        record->status));
        fputs(",\"serialized_target_status\":", output);
        json_write_string(
            output, serialized_glcore_target_status_name(
                        record->target_status));
        fputs(",\"compile_authority_status\":", output);
        json_write_string(
            output, unity_compile_authority_status_string(
                        record->compile_authority_status));
        fputs(",\"certificate_status\":", output);
        json_write_string(
            output, glcore_link_certificate_status_name(
                        record->certificate_status));
        fprintf(output,
                ",\"compile_attempted\":%s,"
                "\"compiler_terminal_success\":%s,"
                "\"expected_size\":%zu,\"actual_size\":%zu,"
                "\"first_differing_byte\":",
                record->compile_attempted ? "true" : "false",
                record->compiler_terminal_success ? "true" : "false",
                record->expected_size, record->actual_size);
        if (record->first_differing_byte == SIZE_MAX) {
            fputs("null", output);
        } else {
            fprintf(output, "%zu", record->first_differing_byte);
        }
        fputs("}\n", output);
    }
    fputs("  ],\n", output);
    fputs("  \"generated_d3d11_domain_gate\":{\"status\":", output);
    json_write_string(
        output, generated_domain_counts_status(&g_generated_domain));
    fputs(",\"requested\":", output);
    fputs(g_generated_domain_enabled ? "true" : "false", output);
    fputs(",\"skip_reason\":", output);
    if (g_generated_domain_enabled) {
        fputs("null", output);
    } else {
        json_write_string(output, "explicit-direct-only-mode");
    }
    fputs(",\"scope\":\"whole-generated-pass-domain\","
          "\"filter_semantics\":\"tuple-admits-whole-pass\","
          "\"dxbc_derived_glsl_status\":", output);
    json_write_string(
        output, unity_generated_glsl_status_name(
            UNITY_GENERATED_GLSL_UNAVAILABLE_NO_PRECISION_AUTHORITY));
    fprintf(output,
            ",\"eligible_passes\":%zu,"
            "\"filter_admitted_passes\":%zu,"
            "\"filter_skipped_passes\":%zu,"
            "\"plans_built\":%zu,"
            "\"snippets_uniquely_mapped\":%zu,"
            "\"certified_passes\":%zu,\"failed_passes\":%zu,"
            "\"unreached_passes\":%zu,"
            "\"active_stages\":%zu,\"generated_states\":%zu,"
            "\"planned_compiles\":%zu,\"compile_attempts\":%zu,"
            "\"clean_compiles\":%zu,"
            "\"matched_dxbc_containers\":%zu,"
            "\"runtime_binding_attested_compiles\":%zu,"
            "\"runtime_binding_compatible_compiles\":%zu,"
            "\"compiler_diagnostics\":%zu,"
            "\"diagnostic_attestation_compiles\":%zu,"
            "\"diagnostic_attested_compiles\":%zu,"
            "\"diagnostic_attested_actionable\":%zu,"
            "\"report_allocation_complete\":%s},\n",
            g_generated_domain.eligible_passes,
            g_generated_domain.filter_admitted_passes,
            g_generated_domain.filter_skipped_passes,
            g_generated_domain.plans_built,
            g_generated_domain.snippets_uniquely_mapped,
            g_generated_domain.certified_passes,
            g_generated_domain.failed_passes,
            generated_domain_unreached_passes(&g_generated_domain),
            g_generated_domain.active_stages,
            g_generated_domain.generated_states,
            g_generated_domain.planned_compiles,
            g_generated_domain.compile_attempts,
            g_generated_domain.clean_compiles,
            g_generated_domain.matched_dxbc_containers,
            g_generated_domain.runtime_binding_attested_compiles,
            g_generated_domain.runtime_binding_compatible_compiles,
            g_generated_domain.compiler_diagnostics,
            g_generated_domain.diagnostic_attestation_compiles,
            g_generated_domain.diagnostic_attested_compiles,
            g_generated_domain.diagnostic_attested_actionable,
            g_generated_domain_report_allocation_failed ? "false" : "true");
    fputs("  \"generated_d3d11_domain_passes\":[\n", output);
    for (size_t i = 0U; i < g_generated_domain_pass_count; ++i) {
        const GeneratedDomainPassRecord* record =
            &g_generated_domain_passes[i];
        fprintf(output,
                "    %s{\"shader_result_index\":%zu,"
                "\"path_id\":%lld,\"subshader_index\":%d,"
                "\"local_pass_index\":%d,"
                "\"serialized_pass_index\":%d,\"status\":",
                i ? "," : "", record->shader_result_index,
                record->path_id, record->subshader_index,
                record->local_pass_index,
                record->serialized_pass_index);
        json_write_string(
            output, generated_domain_pass_status_name(record->status));
        fprintf(output,
                ",\"filter_admitted\":%s,"
                "\"plan_attempted\":%s,"
                "\"snippet_mapping_attempted\":%s,"
                "\"certification_attempted\":%s,"
                "\"generated_snippet_index\":%d,\"plan_status\":",
                record->filter_admitted ? "true" : "false",
                record->plan_attempted ? "true" : "false",
                record->snippet_mapping_attempted ? "true" : "false",
                record->certification_attempted ? "true" : "false",
                record->generated_snippet_index);
        if (record->plan_attempted) {
            json_write_string(
                output, shaderlab_variant_plan_status_name(
                    record->plan_status));
        } else {
            fputs("null", output);
        }
        fprintf(output,
                ",\"plan_diagnostic\":{\"stage_index\":%d,"
                "\"subprogram_index\":%d,"
                "\"conflicting_subprogram_index\":%d,"
                "\"raw_keyword_index\":%d},"
                "\"certification_status\":",
                record->plan_diagnostic.stage_index,
                record->plan_diagnostic.subprogram_index,
                record->plan_diagnostic.conflicting_subprogram_index,
                record->plan_diagnostic.raw_keyword_index);
        if (record->certification_attempted) {
            json_write_string(
                output, unity_generated_domain_status_name(
                    record->certification_status));
        } else {
            fputs("null", output);
        }
        fputs(",\"dxbc_derived_glsl_status\":", output);
        json_write_string(
            output, unity_generated_glsl_status_name(record->glsl_status));
        fprintf(output,
                ",\"counts\":{\"active_stages\":%zu,"
                "\"generated_states\":%zu,\"planned_compiles\":%zu,"
                "\"compile_attempts\":%zu,\"clean_compiles\":%zu,"
                "\"matched_dxbc_containers\":%zu,"
                "\"runtime_binding_attested_compiles\":%zu,"
                "\"runtime_binding_compatible_compiles\":%zu,"
                "\"compiler_diagnostics\":%zu,"
                "\"diagnostic_attestation_compiles\":%zu,"
                "\"diagnostic_attested_compiles\":%zu,"
                "\"diagnostic_attested_actionable\":%zu},"
                "\"failure\":{\"stage_index\":%d,"
                "\"compiler_program\":%" PRId32 ","
                "\"hardware_tier_group\":%d,"
                "\"generated_state_index\":",
                record->active_stage_count,
                record->generated_state_count,
                record->planned_compile_count,
                record->compile_attempt_count,
                record->clean_compile_count,
                record->matched_dxbc_count,
                record->runtime_binding_attested_compile_count,
                record->runtime_binding_compatible_compile_count,
                record->compiler_diagnostic_count,
                record->diagnostic_attestation_compile_count,
                record->diagnostic_attested_compile_count,
                record->diagnostic_attested_actionable_count,
                record->failure_stage_index,
                record->failure_compiler_program,
                record->failure_hardware_tier_group);
        if (record->failure_generated_state_index == SIZE_MAX) {
            fputs("null", output);
        } else {
            fprintf(output, "%zu",
                    record->failure_generated_state_index);
        }
        fputs(",\"aliased_state_index\":", output);
        if (record->failure_aliased_state_index == SIZE_MAX) {
            fputs("null", output);
        } else {
            fprintf(output, "%zu", record->failure_aliased_state_index);
        }
        fprintf(output,
                ",\"subprogram_index\":%d,\"keyword_family\":%d,"
                "\"contract_row_index\":",
                record->failure_subprogram_index,
                (int)record->failure_keyword_family);
        if (record->failure_contract_row_index == SIZE_MAX) {
            fputs("null", output);
        } else {
            fprintf(output, "%zu", record->failure_contract_row_index);
        }
        fputs(",\"compile_authority_status\":", output);
        json_write_string(
            output, unity_compile_authority_status_string(
                record->failure_compile_authority_status));
        fputs(",\"compiler_terminal_status\":", output);
        if (record->failure_compiler_terminal_available) {
            fprintf(output,
                    "{\"success\":%s,\"from_cache\":%s,"
                    "\"diagnostic_count\":%zu}",
                    record->failure_compiler_terminal_success
                        ? "true" : "false",
                    record->failure_compiler_terminal_from_cache
                        ? "true" : "false",
                    record->failure_compiler_terminal_diagnostic_count);
        } else {
            fputs("null", output);
        }
        fputs(",\"dxbc_compare_status\":", output);
        json_write_string(
            output, dxbc_compare_status_name(
                record->failure_dxbc_compare.status));
        fputs(",\"dxbc_compare_detail\":", output);
        if (record->failure_dxbc_compare.status == DXBC_COMPARE_EQUAL) {
            fputs("null", output);
        } else {
            const DXBCCompareResult* comparison =
                &record->failure_dxbc_compare;
            fprintf(output,
                    "{\"expected_size\":%zu,\"actual_size\":%zu,"
                    "\"first_differing_byte\":",
                    comparison->expected_size, comparison->actual_size);
            if (comparison->first_differing_byte == SIZE_MAX) {
                fputs("null", output);
            } else {
                fprintf(output, "%zu", comparison->first_differing_byte);
            }
            fputs(",\"chunk_index\":", output);
            if (comparison->chunk_index == UINT32_MAX) {
                fputs("null", output);
            } else {
                fprintf(output, "%" PRIu32, comparison->chunk_index);
            }
            fputs(",\"instruction_index\":", output);
            if (comparison->instruction_index == UINT32_MAX) {
                fputs("null", output);
            } else {
                fprintf(output, "%" PRIu32,
                        comparison->instruction_index);
            }
            fputs(",\"token_index\":", output);
            if (comparison->token_index == UINT32_MAX) {
                fputs("null", output);
            } else {
                fprintf(output, "%" PRIu32, comparison->token_index);
            }
            fprintf(output,
                    ",\"expected_value\":\"0x%016" PRIx64 "\","
                    "\"actual_value\":\"0x%016" PRIx64 "\"}",
                    comparison->expected_value,
                    comparison->actual_value);
        }
        fputs(",\"reflection_certificate\":{\"status\":", output);
        json_write_string(
            output, unity_reflection_certificate_status_name(
                record->failure_reflection_status));
        fputs(",\"authority\":", output);
        json_write_string(
            output, unity_reflection_certificate_authority_name(
                record->failure_reflection_authority));
        fprintf(output,
                ",\"expected_records\":%zu,\"observed_records\":%zu,"
                "\"matched_records\":%zu,\"expected_record_index\":",
                record->failure_reflection_expected_count,
                record->failure_reflection_observed_count,
                record->failure_reflection_matched_count);
        if (record->failure_reflection_expected_index == SIZE_MAX) {
            fputs("null", output);
        } else {
            fprintf(output, "%zu",
                    record->failure_reflection_expected_index);
        }
        fputs(",\"observed_record_index\":", output);
        if (record->failure_reflection_observed_index == SIZE_MAX) {
            fputs("null", output);
        } else {
            fprintf(output, "%zu",
                    record->failure_reflection_observed_index);
        }
        fputs(",\"expected_record\":", output);
        json_write_reflection_record_summary(
            output, &record->failure_reflection_expected_record);
        fputs(",\"observed_record\":", output);
        json_write_reflection_record_summary(
            output, &record->failure_reflection_observed_record);
        fputc('}', output);
        fputs("}}\n", output);
    }
    fputs("  ],\n", output);
    fputs("  \"serialized_sources\":[\n", output);
    for (size_t i = 0U; i < g_serialized_source_count; ++i) {
        const VerificationSerializedSource* source =
            &g_serialized_sources[i];
        fprintf(output, "    %s{\"serialized_source_index\":%zu,"
                        "\"outer_path\":",
                i ? "," : "", i);
        json_write_string(output, source->outer_path);
        fputs(",\"member_name\":", output);
        if (source->member_name) {
            json_write_string(output, source->member_name);
        } else {
            fputs("null", output);
        }
        fprintf(output,
                ",\"member_index\":%zu,\"is_bundle_member\":%s,"
                "\"serialized_file_sha256\":",
                source->member_index,
                source->is_bundle_member ? "true" : "false");
        json_write_string(output, source->serialized_file_sha256);
        fprintf(output,
                ",\"metadata_parsed\":%s,"
                "\"shader_schema_required\":%s,"
                "\"shader_schema_resolved\":%s,"
                "\"shader_objects\":%zu,\"compute_shader_objects\":%zu}"
                "\n",
                source->metadata_parsed ? "true" : "false",
                source->shader_schema_required ? "true" : "false",
                source->shader_schema_resolved ? "true" : "false",
                source->shader_objects, source->compute_shader_objects);
    }
    fputs("  ],\n  \"unsupported_shader_objects\":[\n", output);
    for (size_t i = 0U; i < g_unsupported_compute_shader_count; ++i) {
        const UnsupportedComputeShader* unsupported =
            &g_unsupported_compute_shaders[i];
        const VerificationSerializedSource* source =
            unsupported->serialized_source_index < g_serialized_source_count
                ? &g_serialized_sources[unsupported->serialized_source_index]
                : NULL;
        fprintf(output,
                "    %s{\"reason\":\"compute-shader-unsupported\","
                "\"class_id\":72,\"serialized_source_index\":%zu,"
                "\"outer_path\":",
                i ? "," : "", unsupported->serialized_source_index);
        if (source) {
            json_write_string(output, source->outer_path);
        } else {
            fputs("null", output);
        }
        fputs(",\"member_name\":", output);
        if (source && source->member_name) {
            json_write_string(output, source->member_name);
        } else {
            fputs("null", output);
        }
        fprintf(output,
                ",\"member_index\":%zu,\"is_bundle_member\":%s,"
                "\"serialized_file_sha256\":",
                source ? source->member_index : 0U,
                source && source->is_bundle_member ? "true" : "false");
        if (source) {
            json_write_string(output, source->serialized_file_sha256);
        } else {
            fputs("null", output);
        }
        fprintf(output, ",\"path_id\":%lld,\"object_size\":%u}\n",
                unsupported->path_id, unsupported->object_size);
    }
    fputs("  ],\n", output);
    fprintf(output,
            "  \"unity_shader_compiler\":{\"maximum_live_processes\":1,"
            "\"persistent_process_reaped_on_exit\":true,"
            "\"running_before_shutdown\":%s,"
            "\"process_starts\":%" PRIu64
            ",\"process_recycles\":%" PRIu64
            ",\"source_budget_recycles\":%" PRIu64
            ",\"source_residency_budget_bytes\":%" PRIu64
            ",\"tracked_source_window_bytes\":%" PRIu64
            ",\"peak_tracked_source_window_bytes\":%" PRIu64
            ",\"tracked_unique_source_count\":%" PRIu64
            ",\"unique_source_submissions\":%" PRIu64
            ",\"source_digest_scans\":%" PRIu64
            ",\"source_tracking_failures\":%" PRIu64
            ",\"submitted_requests\":%" PRIu64
            ",\"executed_requests\":%" PRIu64
            ",\"preprocess_requests\":%" PRIu64
            ",\"compile_requests\":%" PRIu64
            ",\"preprocess_expanded_requests\":%" PRIu64
            ",\"disassemble_requests\":%" PRIu64
            ",\"coalesced_compile_requests\":%" PRIu64 "},\n",
            broker_stats->compiler_process_running ? "true" : "false",
            broker_stats->compiler_process_starts,
            broker_stats->compiler_process_recycles,
            broker_stats->source_budget_recycles,
            broker_stats->source_residency_budget_bytes,
            broker_stats->tracked_source_window_bytes,
            broker_stats->peak_tracked_source_window_bytes,
            broker_stats->tracked_unique_source_count,
            broker_stats->unique_source_submissions,
            broker_stats->source_digest_scans,
            broker_stats->source_tracking_failures,
            broker_stats->submitted_requests,
            broker_stats->executed_requests,
            broker_stats->preprocess_requests,
            broker_stats->compile_requests,
            broker_stats->preprocess_expanded_requests,
            broker_stats->disassemble_requests,
            broker_stats->coalesced_compile_requests);
    fputs("  \"compiler_session_authority\":{\"available\":", output);
    fputs(compiler_session_json ? "true" : "false", output);
    fputs(",\"expected_valid_apis_configured\":", output);
    const bool valid_apis_configured = valid_apis_authority &&
        valid_apis_authority->status !=
            UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED;
    fputs(valid_apis_configured ? "true" : "false", output);
    fputs(",\"expected_valid_apis\":", output);
    if (valid_apis_configured) {
        fprintf(output, "%" PRIu32,
                valid_apis_authority->expected_valid_apis);
    } else {
        fputs("null", output);
    }
    fputs(",\"pre_command_gate_status\":", output);
    json_write_string(
        output, valid_apis_authority
            ? valid_apis_authority_status_name(valid_apis_authority->status)
            : "query-failed");
    fputs(",\"observed_valid_apis\":", output);
    if (valid_apis_authority &&
        (valid_apis_authority->status ==
             UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED ||
         valid_apis_authority->status ==
             UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH)) {
        fprintf(output, "%" PRIu32,
                valid_apis_authority->observed_valid_apis);
    } else {
        fputs("null", output);
    }
    if (compiler_session_json) {
        const bool exact_profile_match =
            unity_compiler_session_capabilities_match_valid_apis(
                compiler_session, g_compile_profile.valid_apis);
        fputs(",\"status\":", output);
        json_write_string(
            output, exact_profile_match
                        ? "exact-live-initializeCompiler"
                        : "profile-valid-apis-mismatch");
        fprintf(output, ",\"profile_valid_apis_exact_match\":%s,"
                        "\"record\":",
                exact_profile_match ? "true" : "false");
        fputs(compiler_session_json, output);
    } else {
        fputs(",\"status\":\"not-captured-process-free-cache-path\","
              "\"profile_valid_apis_exact_match\":null,\"record\":null",
              output);
    }
    fputs("},\n", output);
    UnityCompilerCacheStats cache_stats;
    unity_compiler_cache_get_stats(&cache_stats);
    const char* cache_dir = getenv("DXBC_USC_CACHE_DIR");
    fputs("  \"persistent_cache\":{\"enabled\":", output);
    fputs(cache_dir && *cache_dir ? "true" : "false", output);
    fputs(",\"path\":", output);
    if (cache_dir && *cache_dir) {
        json_write_string(output, cache_dir);
    } else {
        fputs("null", output);
    }
    fprintf(output,
            ",\"hits\":%" PRIu64 ",\"misses\":%" PRIu64
            ",\"stores\":%" PRIu64 ",\"corrupt_entries\":%" PRIu64
            ",\"write_errors\":%" PRIu64 "},\n",
            cache_stats.hits, cache_stats.misses, cache_stats.stores,
            cache_stats.corrupt_entries, cache_stats.write_errors);
    fputs("  \"compiler_diagnostic_gate\":{\"status\":", output);
    json_write_string(
        output,
        g_compiler_diagnostic_allocation_failed
            ? "retention-failed"
            : (g_diagnostic_authority_failure_count > 0U
                   ? "diagnostic-authority-unavailable"
                   : (g_compiler_diagnostic_unattested_actionable_count > 0U
                          ? "diagnostics-emitted"
                          : (g_compiler_diagnostic_attested_actionable_count > 0U
                                 ? "source-equivalent-diagnostics-attested"
                          : (g_compiler_diagnostic_informational_count > 0U
                                 ? "informational-records-retained"
                                 : "clean")))));
    fprintf(output,
            ",\"hard_failure\":%s,\"observed_records\":%zu,"
            "\"retained_records\":%zu,"
            "\"informational_records\":%zu,"
            "\"actionable_records\":%zu,"
            "\"attested_actionable_records\":%zu,"
            "\"unattested_actionable_records\":%zu,"
            "\"authority_failures\":%zu,\"retention_complete\":%s,"
            "\"source_equivalence_policy\":\"source-backed-only\","
            "\"normalized_multiset_fields\":[\"actionable-class\","
            "\"fields[0]\",\"fields[1]\",\"exact-message\"],"
            "\"location_fields_retained_not_compared\":[\"record\","
            "\"file\",\"fields[2]\"]},\n",
            diagnostic_gate_clean ? "false" : "true",
            g_compiler_diagnostic_observed_count,
            g_compiler_diagnostic_count,
            g_compiler_diagnostic_informational_count,
            g_compiler_diagnostic_actionable_count,
            g_compiler_diagnostic_attested_actionable_count,
            g_compiler_diagnostic_unattested_actionable_count,
            g_diagnostic_authority_failure_count,
            g_compiler_diagnostic_allocation_failed ? "false" : "true");
    fputs("  \"compiler_diagnostics\":[\n", output);
    for (size_t i = 0U; i < g_compiler_diagnostic_count; ++i) {
        const VerificationCompilerDiagnostic* diagnostic =
            &g_compiler_diagnostics[i];
        fprintf(output,
                "    %s{\"response_sequence\":%" PRIu64
                ",\"response_diagnostic_index\":%zu,"
                "\"shader_result_index\":",
                i ? "," : "", diagnostic->response_sequence,
                diagnostic->response_diagnostic_index);
        if (diagnostic->shader_result_index == SIZE_MAX) {
            fputs("null", output);
        } else {
            fprintf(output, "%zu", diagnostic->shader_result_index);
        }
        fprintf(output,
                ",\"path_id\":%lld,\"stage\":%d,"
                "\"subprogram_index\":%d,\"pass_index\":%d,"
                "\"operation\":",
                diagnostic->path_id, diagnostic->stage,
                diagnostic->sub_idx, diagnostic->pass_idx);
        json_write_string(
            output,
            verification_diagnostic_operation_name(diagnostic->operation));
        fputs(",\"phase\":", output);
        json_write_string(output, diagnostic->phase);
        fprintf(output,
                ",\"terminal_success\":%s,\"from_cache\":%s,"
                "\"severity\":\"%s\","
                "\"source_equivalent_attested\":%s,"
                "\"diagnostic_attestation_id\":",
                diagnostic->compiler_success ? "true" : "false",
                diagnostic->from_cache ? "true" : "false",
                diagnostic->actionable ? "actionable" : "informational",
                diagnostic->source_equivalent_attested ? "true" : "false");
        if (diagnostic->diagnostic_attestation_id == 0U) {
            fputs("null", output);
        } else {
            fprintf(output, "%" PRIu64,
                    diagnostic->diagnostic_attestation_id);
        }
        fputs(",\"attestation_role\":", output);
        json_write_string(output, diagnostic->attestation_role);
        fputs(",\"diagnostic_parity_status\":", output);
        json_write_string(
            output, unity_generated_diagnostic_parity_status_name(
                        diagnostic->diagnostic_parity_status));
        fprintf(output,
                ",\"normalized_comparison_key\":{"
                "\"actionable\":%s,\"fields\":[%" PRId32
                ",%" PRId32 "],\"exact_message\":",
                diagnostic->actionable ? "true" : "false",
                diagnostic->fields[0], diagnostic->fields[1]);
        json_write_string(output, diagnostic->message);
        fprintf(output,
                "},\"fields\":[%" PRId32 ",%" PRId32 ",%" PRId32 "],"
                "\"record\":",
                diagnostic->fields[0], diagnostic->fields[1],
                diagnostic->fields[2]);
        json_write_string(output, diagnostic->record);
        fputs(",\"file\":", output);
        json_write_string(output, diagnostic->file);
        fputs(",\"message\":", output);
        json_write_string(output, diagnostic->message);
        fputs("}\n", output);
    }
    fputs("  ],\n", output);
    fputs("  \"shaders\":[\n", output);
    for (size_t i = 0U; i < g_shader_result_count; ++i) {
        const ShaderVerificationResult* result = &g_shader_results[i];
        const char* dxbc_status = shader_dxbc_status(result);
        const char* glsl_status = shader_glsl_status(result);
        fprintf(output,
            "    %s{\"shader_result_index\":%zu,"
                "\"serialized_source_index\":%zu,\"outer_path\":",
                i ? "," : "", i, result->serialized_source_index);
        json_write_string(output, result->outer_path);
        fputs(",\"member_index\":", output);
        fprintf(output, "%zu", result->member_index);
        fputs(",\"is_bundle_member\":", output);
        fputs(result->is_bundle_member ? "true" : "false", output);
        fputs(",\"serialized_member\":", output);
        json_write_string(output, result->serialized_member);
        fputs(",\"serialized_file_sha256\":", output);
        json_write_string(output, result->serialized_file_sha256);
        fprintf(output, ",\"path_id\":%lld,\"shader_name\":",
                result->path_id);
        if (result->shader_name) {
            json_write_string(output, result->shader_name);
        } else {
            fputs("null", output);
        }
        fputs(",\"generated_source_path\":", output);
        if (result->generated_source_path) {
            json_write_string(output, result->generated_source_path);
        } else {
            fputs("null", output);
        }
        fputs(",\"terminal_phase\":", output);
        json_write_string(output, result->terminal_phase);
        fprintf(output,
                ",\"parsed\":%s,\"original_source_unique\":%s,"
                "\"original_preprocess_ok\":%s,"
                "\"generated_source_present\":%s,"
                "\"generated_preprocess_ok\":%s,"
                "\"d3d11_archive_ok\":%s,\"dxbc_status\":",
                result->parsed ? "true" : "false",
                result->original_source_unique ? "true" : "false",
                result->original_preprocess_ok ? "true" : "false",
                result->generated_source_present ? "true" : "false",
                result->generated_preprocess_ok ? "true" : "false",
                result->d3d11_archive_ok ? "true" : "false");
        json_write_string(output, dxbc_status);
        fputs(",\"direct_serialized_subprogram_census\":", output);
        json_write_string(output, dxbc_status);
        fputs(",\"glsl_status\":", output);
        json_write_string(output, glsl_status);
        fputs(",\"serialized_glcore\":{\"status\":", output);
        json_write_string(output, glsl_status);
        fputs(",\"readiness_evaluated\":", output);
        fputs(result->glcore_readiness_evaluated ? "true" : "false",
              output);
        fputs(",\"readiness_status\":", output);
        if (result->glcore_readiness_evaluated) {
            json_write_string(
                output, serialized_glcore_target_status_name(
                            result->glcore_readiness_status));
        } else {
            fputs("null", output);
        }
        fprintf(output,
                ",\"objects_evaluated\":%zu,"
                "\"platform_present_objects\":%zu,"
                "\"platform_absent_objects\":%zu,"
                "\"unavailable_objects\":%zu,"
                "\"targets_discovered\":%zu,"
                "\"targets_expected\":%zu,"
                "\"targets_filtered_out\":%zu,"
                "\"targets_opened\":%zu,"
                "\"compile_attempts\":%zu,"
                "\"targets_compiled\":%zu,"
                "\"targets_exact\":%zu,"
                "\"targets_mismatched\":%zu,"
                "\"targets_unavailable\":%zu,"
                "\"oracle_pack_v4_unsupported\":%zu}",
                result->serialized_glcore.objects_evaluated,
                result->serialized_glcore.platform_present_objects,
                result->serialized_glcore.platform_absent_objects,
                result->serialized_glcore.unavailable_objects,
                result->serialized_glcore.targets_discovered,
                result->serialized_glcore.targets_expected,
                result->serialized_glcore.targets_filtered_out,
                result->serialized_glcore.targets_opened,
                result->serialized_glcore.compile_attempts,
                result->serialized_glcore.targets_compiled,
                result->serialized_glcore.targets_exact,
                result->serialized_glcore.targets_mismatched,
                result->serialized_glcore.targets_unavailable,
                result->serialized_glcore.oracle_pack_v4_unsupported);
        fputs(",\"compiler_diagnostic_status\":", output);
        json_write_string(output,
                          shader_compiler_diagnostic_status(result));
        fputs(",\"d3d11_generated_domain\":{\"status\":", output);
        json_write_string(
            output,
            generated_domain_counts_status(&result->generated_domain));
        fputs(",\"requested\":", output);
        fputs(g_generated_domain_enabled ? "true" : "false", output);
        fputs(",\"scope\":", output);
        json_write_string(
            output, g_generated_domain_enabled
                ? "whole-filter-admitted-pass-domain"
                : "not-run-explicit-direct-only");
        fprintf(output,
                ",\"eligible_passes\":%zu,"
                "\"filter_admitted_passes\":%zu,"
                "\"filter_skipped_passes\":%zu,"
                "\"plans_built\":%zu,"
                "\"snippets_uniquely_mapped\":%zu,"
                "\"certified_passes\":%zu,"
                "\"failed_passes\":%zu,\"unreached_passes\":%zu,"
                "\"active_stages\":%zu,"
                "\"generated_states\":%zu,\"planned_compiles\":%zu,"
                "\"compile_attempts\":%zu,\"clean_compiles\":%zu,"
                "\"matched_dxbc_containers\":%zu,"
                "\"runtime_binding_attested_compiles\":%zu,"
                "\"runtime_binding_compatible_compiles\":%zu,"
                "\"compiler_diagnostics\":%zu,"
                "\"diagnostic_attestation_compiles\":%zu,"
                "\"diagnostic_attested_compiles\":%zu,"
                "\"diagnostic_attested_actionable\":%zu},",
                result->generated_domain.eligible_passes,
                result->generated_domain.filter_admitted_passes,
                result->generated_domain.filter_skipped_passes,
                result->generated_domain.plans_built,
                result->generated_domain.snippets_uniquely_mapped,
                result->generated_domain.certified_passes,
                result->generated_domain.failed_passes,
                generated_domain_unreached_passes(
                    &result->generated_domain),
                result->generated_domain.active_stages,
                result->generated_domain.generated_states,
                result->generated_domain.planned_compiles,
                result->generated_domain.compile_attempts,
                result->generated_domain.clean_compiles,
                result->generated_domain.matched_dxbc_containers,
                result->generated_domain.runtime_binding_attested_compiles,
                result->generated_domain.runtime_binding_compatible_compiles,
                result->generated_domain.compiler_diagnostics,
                result->generated_domain.diagnostic_attestation_compiles,
                result->generated_domain.diagnostic_attested_compiles,
                result->generated_domain.diagnostic_attested_actionable);
        fputs("\"whole_shader_certification\":\"not-certified\",",
              output);
        fprintf(output,
                "\"counts\":{\"dxbc_expected\":%d,"
                "\"unsupported_stage_variants\":%d,"
                "\"scheduled_and_decoded\":%d,"
                "\"dxbc_compiled\":%d,\"dxbc_token_matched\":%d,"
                "\"dxbc_byte_matched\":%d,"
                "\"glcore_targets_expected\":%zu,"
                "\"glcore_targets_opened\":%zu,"
                "\"glcore_targets_compiled\":%zu,"
                "\"glcore_targets_exact\":%zu,"
                "\"glcore_targets_unavailable\":%zu,"
                "\"glcore_targets_mismatched\":%zu,"
                "\"compiler_diagnostics\":%zu,"
                "\"compiler_diagnostics_informational\":%zu,"
                "\"compiler_diagnostics_actionable\":%zu,"
                "\"compiler_diagnostics_actionable_attested\":%zu,"
                "\"compiler_diagnostics_actionable_unattested\":%zu,"
                "\"preprocess_diagnostics\":%zu,"
                "\"compile_diagnostics\":%zu,"
                "\"disassemble_diagnostics\":%zu,"
                "\"diagnostic_authority_failures\":%zu},"
                "\"failure_mask\":%u,"
                "\"failure_kinds\":",
                result->dxbc_expected, result->unsupported_stage_variants,
                result->scheduled_and_decoded, result->dxbc_compiled,
                result->dxbc_token_matched, result->dxbc_byte_matched,
                result->serialized_glcore.targets_expected,
                result->serialized_glcore.targets_opened,
                result->serialized_glcore.targets_compiled,
                result->serialized_glcore.targets_exact,
                result->serialized_glcore.targets_unavailable,
                result->serialized_glcore.targets_mismatched,
                result->compiler_diagnostic_count,
                result->informational_compiler_diagnostic_count,
                result->actionable_compiler_diagnostic_count,
                result->attested_actionable_compiler_diagnostic_count,
                result->unattested_actionable_compiler_diagnostic_count,
                result->preprocess_diagnostic_count,
                result->compile_diagnostic_count,
                result->disassemble_diagnostic_count,
                result->diagnostic_authority_failure_count,
                result->failure_kinds);
        write_failure_kind_names(output, result->failure_kinds);
        fputs("}\n", output);
    }
    fputs("  ]\n}\n", output);

    bool ok = fflush(output) == 0 && ferror(output) == 0;
    if (fclose(output) != 0) ok = false;
    free(compiler_session_json);
    if (!ok) {
        fprintf(stderr, "Could not finish verification report '%s': %s\n",
                report_path, strerror(errno));
        return false;
    }
    printf("[INFO] Wrote per-shader compiled-artifact report to %s\n",
           report_path);
    return true;
}

static void print_usage(const char* executable) {
    printf("Usage: %s <unity_input> <original_shaders_dir> "
           "<generated_shaderlab_dir> [filter_file.txt] [failure_dir] "
           "[--dxbc-only] [--domain-only | --direct-only] "
           "[--artifacts] [--no-artifacts] [--workers N] "
           "[--compiler-source-budget-mib N] "
           "[--oracle-pack PATH] [--oracle-strict] "
           "[--oracle-capture PATH] "
           "[--schema-registry PATH] "
           "[--report PATH] "
           "[--profile PATH] [--build-platform N --valid-apis MASK "
           "--d3d11-platform-caps BITS "
           "[--glcore-platform-caps BITS]]\n\n"
           "Verification modes:\n"
           "  default        certify the complete generated D3D11 pass "
           "domain, then run the exhaustive direct serialized-subprogram "
           "census\n"
           "  --domain-only  certify only the complete generated D3D11 "
           "pass domain; requires --dxbc-only and reports the direct census "
           "as not run\n"
           "  --direct-only  preprocess each selected generated candidate "
           "once for exact snippet authority, then compile only the exact "
           "filter-selected serialized rows; skips the generated pass "
           "domain and all original-source preprocessing\n"
           "  original_shaders_dir may be '-' when source is unavailable; "
           "serialized DXBC and GLCore verification does not require it\n",
           executable);
}

int main(int argc, char** argv) {
    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 ||
         strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    int requested_workers = 0;
    uint64_t compiler_source_budget_mib = UINT64_C(128);
    bool compiler_source_budget_option = false;
    bool has_build_platform = false;
    bool has_valid_apis = false;
    const char* oracle_pack_path = NULL;
    const char* oracle_capture_path = NULL;
    bool oracle_pack_option = false;
    bool oracle_capture_option = false;
    bool oracle_strict = false;
    const char* schema_registry_path = NULL;
    bool schema_registry_option = false;
    const char* report_path = NULL;
    bool report_option = false;
    bool domain_only_option = false;
    bool direct_only_option = false;
    const char* filter_path = NULL;
    int options_start = 4;
    if (options_start < argc &&
        strncmp(argv[options_start], "--", 2) != 0) {
        filter_path = argv[options_start++];
    }
    if (options_start < argc &&
        strncmp(argv[options_start], "--", 2) != 0) {
        g_failure_dir = argv[options_start++];
    }

    const char* compile_profile_path = getenv("DXBC_COMPILE_PROFILE");
    if (compile_profile_path && !compile_profile_path[0]) {
        compile_profile_path = NULL;
    }
    bool command_line_profile = false;
    VerificationCompileOverrides command_line_overrides;
    memset(&command_line_overrides, 0, sizeof(command_line_overrides));
    for (int i = options_start; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--dxbc-only") == 0) {
            g_verify_glsl = false;
        } else if (strcmp(argv[i], "--domain-only") == 0) {
            if (domain_only_option) {
                fprintf(stderr,
                        "--domain-only may be specified only once\n");
                return 1;
            }
            domain_only_option = true;
            g_direct_census_enabled = false;
        } else if (strcmp(argv[i], "--direct-only") == 0) {
            if (direct_only_option) {
                fprintf(stderr,
                        "--direct-only may be specified only once\n");
                return 1;
            }
            direct_only_option = true;
            g_direct_only_enabled = true;
            g_generated_domain_enabled = false;
        } else if (strcmp(argv[i], "--artifacts") == 0) {
            g_save_failure_artifacts = true;
        } else if (strcmp(argv[i], "--no-artifacts") == 0) {
            g_save_failure_artifacts = false;
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (++i >= argc ||
                !parse_worker_count(argv[i], &requested_workers)) {
                fprintf(stderr, "--workers requires an integer from 1 to 64\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--workers=", 10) == 0) {
            if (!parse_worker_count(argv[i] + 10, &requested_workers)) {
                fprintf(stderr, "--workers requires an integer from 1 to 64\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--compiler-source-budget-mib") == 0) {
            if (compiler_source_budget_option || ++i >= argc ||
                !parse_source_budget_mib(
                    argv[i], &compiler_source_budget_mib)) {
                fprintf(stderr,
                        "--compiler-source-budget-mib requires one decimal "
                        "MiB value (0 disables)\n");
                return 1;
            }
            compiler_source_budget_option = true;
        } else if (strncmp(argv[i],
                           "--compiler-source-budget-mib=", 29) == 0) {
            if (compiler_source_budget_option ||
                !parse_source_budget_mib(
                    argv[i] + 29, &compiler_source_budget_mib)) {
                fprintf(stderr,
                        "--compiler-source-budget-mib requires one decimal "
                        "MiB value (0 disables)\n");
                return 1;
            }
            compiler_source_budget_option = true;
        } else if (strcmp(argv[i], "--oracle-pack") == 0) {
            if (oracle_pack_option || ++i >= argc || !argv[i][0] ||
                strncmp(argv[i], "--", 2) == 0) {
                fprintf(stderr, "--oracle-pack requires one nonempty path "
                                "and may be specified only once\n");
                return 1;
            }
            oracle_pack_path = argv[i];
            oracle_pack_option = true;
        } else if (strncmp(argv[i], "--oracle-pack=", 14) == 0) {
            if (oracle_pack_option || !argv[i][14]) {
                fprintf(stderr, "--oracle-pack requires one nonempty path "
                                "and may be specified only once\n");
                return 1;
            }
            oracle_pack_path = argv[i] + 14;
            oracle_pack_option = true;
        } else if (strcmp(argv[i], "--oracle-capture") == 0) {
            if (oracle_capture_option || ++i >= argc || !argv[i][0] ||
                strncmp(argv[i], "--", 2) == 0) {
                fprintf(stderr, "--oracle-capture requires one nonempty "
                                "write-once output path\n");
                return 1;
            }
            oracle_capture_path = argv[i];
            oracle_capture_option = true;
        } else if (strncmp(argv[i], "--oracle-capture=", 17) == 0) {
            if (oracle_capture_option || !argv[i][17]) {
                fprintf(stderr, "--oracle-capture requires one nonempty "
                                "write-once output path\n");
                return 1;
            }
            oracle_capture_path = argv[i] + 17;
            oracle_capture_option = true;
        } else if (strcmp(argv[i], "--oracle-strict") == 0) {
            if (oracle_strict) {
                fprintf(stderr, "--oracle-strict may be specified only once\n");
                return 1;
            }
            oracle_strict = true;
        } else if (strcmp(argv[i], "--schema-registry") == 0) {
            if (schema_registry_option || ++i >= argc || !argv[i][0] ||
                strncmp(argv[i], "--", 2) == 0) {
                fprintf(stderr, "--schema-registry requires one nonempty "
                                "path and may be specified only once\n");
                return 1;
            }
            schema_registry_path = argv[i];
            schema_registry_option = true;
        } else if (strncmp(argv[i], "--schema-registry=", 18) == 0) {
            if (schema_registry_option || !argv[i][18]) {
                fprintf(stderr, "--schema-registry requires one nonempty "
                                "path and may be specified only once\n");
                return 1;
            }
            schema_registry_path = argv[i] + 18;
            schema_registry_option = true;
        } else if (strcmp(argv[i], "--report") == 0) {
            if (report_option || ++i >= argc || !argv[i][0] ||
                strncmp(argv[i], "--", 2) == 0) {
                fprintf(stderr, "--report requires one nonempty, new path "
                                "and may be specified only once\n");
                return 1;
            }
            report_path = argv[i];
            report_option = true;
        } else if (strncmp(argv[i], "--report=", 9) == 0) {
            if (report_option || !argv[i][9]) {
                fprintf(stderr, "--report requires one nonempty, new path "
                                "and may be specified only once\n");
                return 1;
            }
            report_path = argv[i] + 9;
            report_option = true;
        } else if (strcmp(argv[i], "--profile") == 0) {
            if (command_line_profile || ++i >= argc || !argv[i][0] ||
                strncmp(argv[i], "--", 2) == 0) {
                fprintf(stderr,
                        "--profile requires one nonempty path and may be "
                        "specified only once\n");
                return 1;
            }
            compile_profile_path = argv[i];
            command_line_profile = true;
        } else if (strncmp(argv[i], "--profile=", 10) == 0) {
            if (command_line_profile || !argv[i][10]) {
                fprintf(stderr,
                        "--profile requires one nonempty path and may be "
                        "specified only once\n");
                return 1;
            }
            compile_profile_path = argv[i] + 10;
            command_line_profile = true;
        } else if (strcmp(argv[i], "--build-platform") == 0) {
            if (++i >= argc || !parse_u32_option(
                                   argv[i],
                                   &command_line_overrides.build_platform)) {
                fprintf(stderr, "--build-platform requires a uint32 value\n");
                return 1;
            }
            command_line_overrides.has_build_platform = true;
        } else if (strcmp(argv[i], "--valid-apis") == 0) {
            if (++i >= argc ||
                !parse_u32_option(argv[i],
                                  &command_line_overrides.valid_apis)) {
                fprintf(stderr, "--valid-apis requires a uint32 mask\n");
                return 1;
            }
            command_line_overrides.has_valid_apis = true;
        } else if (strcmp(argv[i], "--d3d11-platform-caps") == 0) {
            if (++i >= argc || !parse_capability_option(
                                   argv[i],
                                   &command_line_overrides
                                        .d3d11_capabilities)) {
                fprintf(stderr,
                        "--d3d11-platform-caps requires a 33-bit mask\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--glcore-platform-caps") == 0) {
            if (++i >= argc || !parse_capability_option(
                                   argv[i],
                                   &command_line_overrides
                                        .glcore_capabilities)) {
                fprintf(stderr,
                        "--glcore-platform-caps requires a 33-bit mask\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (domain_only_option && direct_only_option) {
        fprintf(stderr,
                "--domain-only and --direct-only are mutually exclusive\n");
        return 1;
    }
    if (domain_only_option && g_verify_glsl) {
        fprintf(stderr,
                "--domain-only requires --dxbc-only because linked GLCore "
                "verification uses the direct source/subprogram mapping\n");
        return 1;
    }

    UnityCompileProfile loaded_profile;
    unity_compile_profile_init(&loaded_profile);
    bool profile_loaded = false;
    if (!load_verification_compile_profile(
            compile_profile_path, &has_build_platform, &has_valid_apis,
            &loaded_profile, &profile_loaded)) {
        return 1;
    }
    uint32_t environment_override_mask = 0U;
    if (!apply_compile_profile_environment(
            &command_line_overrides, &has_build_platform, &has_valid_apis,
            &environment_override_mask)) {
        fprintf(stderr, "Invalid compiler-profile environment value\n");
        return 1;
    }
    uint32_t command_line_override_mask = 0U;
    apply_compile_profile_command_line(
        &command_line_overrides, &has_build_platform, &has_valid_apis,
        &command_line_override_mask);

    if (requested_workers == 0) {
        const char* worker_env = getenv("DXBC_WORKERS");
        if (worker_env && *worker_env &&
            !parse_worker_count(worker_env, &requested_workers)) {
            fprintf(stderr,
                    "DXBC_WORKERS must be an integer from 1 to 64\n");
            return 1;
        }
    }
    g_compile_profile.preprocess_present =
        has_build_platform && has_valid_apis;
    if (!g_compile_profile.preprocess_present ||
        !g_compile_profile.d3d11_capabilities.present ||
        (g_verify_glsl &&
         !g_compile_profile.glcore_capabilities.present)) {
        fprintf(stderr,
                "Exact verification requires explicit build target, valid "
                "API mask, and per-API Unity platform-capability snapshots. "
                "Use --profile PATH or --build-platform, --valid-apis, "
                "--d3d11-platform-caps and (unless --dxbc-only) "
                "--glcore-platform-caps; the corresponding DXBC_* "
                "environment variables and DXBC_COMPILE_PROFILE are also "
                "accepted.\n");
        return 1;
    }
    if (!report_effective_compile_profile(
            compile_profile_path, &loaded_profile, profile_loaded,
            environment_override_mask, command_line_override_mask)) {
        return 1;
    }
    unity_compiler_cache_reset_stats();
    if (filter_path) load_filters(filter_path);
    if (mkdir(g_failure_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Could not create diagnostics directory '%s': %s\n",
                g_failure_dir, strerror(errno));
        return 1;
    }
    struct stat failure_dir_status;
    if (stat(g_failure_dir, &failure_dir_status) != 0 ||
        !S_ISDIR(failure_dir_status.st_mode)) {
        fprintf(stderr, "Diagnostics path is not a directory: %s\n",
                g_failure_dir);
        return 1;
    }

    const char* unity_input_path = argv[1];
    const char* orig_shaders_dir = argv[2];
    const char* gen_shaderlab_dir = argv[3];

    /* Original source is optional diagnostic-parity/failure-artifact
     * evidence.  Serialized GLCore verification is anchored directly to the
     * released platform-15 text and must work when no source exists.  A '-'
     * positional value explicitly declares source unavailable. */
    struct stat original_source_status;
    bool original_source_directory_available =
        !g_direct_only_enabled &&
        strcmp(orig_shaders_dir, "-") != 0 &&
        stat(orig_shaders_dir, &original_source_status) == 0 &&
        S_ISDIR(original_source_status.st_mode);
    if (original_source_directory_available) {
        if (!scan_original_shaders(orig_shaders_dir)) {
            fprintf(stderr, "Original ShaderLab source index is unavailable; "
                    "continuing with released artifact authority only\n");
            free_original_shaders();
        }
        printf("[INFO] Indexed %zu original ShaderLab declarations from %s\n",
               g_original_shader_count, orig_shaders_dir);
    } else {
        printf("[INFO] Original ShaderLab source %s; serialized DXBC/GLCore "
               "verification does not require it\n",
               g_direct_only_enabled
                   ? "intentionally not indexed in --direct-only mode"
                   : "unavailable");
    }

    char repo_root[1024];
    find_repo_root_from_path(repo_root, sizeof(repo_root), unity_input_path);

    char includes_dir[1024];
    char input_directory[1024];
    struct stat input_path_status;
    bool input_is_directory =
        stat(unity_input_path, &input_path_status) == 0 &&
        S_ISDIR(input_path_status.st_mode);
    strncpy(input_directory, unity_input_path,
            sizeof(input_directory) - 1U);
    input_directory[sizeof(input_directory) - 1U] = '\0';
    if (!input_is_directory) {
        char* last_slash = strrchr(input_directory, '/');
        if (last_slash) {
            *last_slash = '\0';
        } else {
            strcpy(input_directory, ".");
        }
    }
    snprintf(includes_dir, sizeof(includes_dir), "%s/Includes",
             input_directory);

    // 2. Configure the one shared, lazy UnityShaderCompiler authority.
    printf("[INFO] Configuring shared UnityShaderCompiler broker...\n");
    UnityCompilerBroker* broker =
        unity_compiler_broker_create_lazy(repo_root, includes_dir);
    if (!broker) {
        printf("[FAIL] Failed to configure UnityShaderCompiler broker\n");
        return 1;
    }
    if (!unity_compiler_broker_set_expected_valid_apis(
            broker, g_compile_profile.valid_apis)) {
        printf("[FAIL] Failed to install exact initializeCompiler "
               "validApis authority\n");
        unity_compiler_broker_destroy(broker);
        return 1;
    }
    const uint64_t compiler_source_budget_bytes =
        compiler_source_budget_mib * UINT64_C(1048576);
    if (!unity_compiler_broker_set_source_residency_budget(
            broker, compiler_source_budget_bytes)) {
        printf("[FAIL] Failed to configure compiler source-residency budget\n");
        unity_compiler_broker_destroy(broker);
        return 1;
    }
    if (compiler_source_budget_bytes > 0U) {
        printf("[INFO] UnityShaderCompiler source-residency budget: %" PRIu64
               " MiB of exact unique submitted source content; one "
               "oversized source is retained as an exclusive window.\n",
               compiler_source_budget_mib);
    } else {
        printf("[INFO] UnityShaderCompiler source-residency recycling is "
               "disabled.\n");
    }
    UnityCompilerBrokerStats initial_broker_stats;
    unity_compiler_broker_get_stats(broker, &initial_broker_stats);
    if (initial_broker_stats.compiler_process_running) {
        printf("[INFO] One shared UnityShaderCompiler process is connected.\n");
    } else {
        printf("[INFO] Lazy compiler broker ready; warm cache hits require "
               "no UnityShaderCompiler process.\n");
    }
    if (!verification_oracle_initialize(
            broker, oracle_pack_path, oracle_capture_path, oracle_strict)) {
        unity_compiler_broker_destroy(broker);
        return 1;
    }
    if (oracle_strict) {
        printf("[INFO] Strict cold-oracle mode: every preprocess and "
               "compileSnippet request must be an exact validated pack hit; "
               "UnityShaderCompiler will not be started.\n");
    }

    // Worker count controls CPU-side parsing/comparison only.  All workers
    // share the broker's single protocol channel and at most one USC process.
    int num_workers = g_direct_census_enabled
        ? (requested_workers > 0 ? requested_workers : 2) : 0;
    if (g_direct_census_enabled) {
        printf("[INFO] Spawning %d CPU verification workers; compiler "
               "protocol traffic uses at most one shared "
               "UnityShaderCompiler process.\n", num_workers);
    } else {
        printf("[INFO] Direct-census CPU workers not started in "
               "--domain-only mode; generated-domain compiler requests "
               "still use the one shared broker.\n");
    }
    if (g_direct_only_enabled) {
        printf("[INFO] Direct-only scope: one generated preprocess authority "
               "request per selected ShaderLab candidate, exact selected "
               "serialized rows only, and no generated-domain or "
               "original-source work.\n");
    }
    pthread_t* workers = NULL;
    if (num_workers > 0) {
        workers = malloc((size_t)num_workers * sizeof(*workers));
        if (!workers) {
            verification_oracle_dispose();
            unity_compiler_broker_destroy(broker);
            return 1;
        }
    }
    int workers_started = 0;
    for (; workers_started < num_workers; ++workers_started) {
        if (pthread_create(&workers[workers_started], NULL,
                           worker_thread_func, broker) != 0) {
            fprintf(stderr, "Failed to create verification worker %d\n",
                    workers_started);
            stop_worker_threads(workers, workers_started);
            verification_oracle_dispose();
            unity_compiler_broker_destroy(broker);
            return 1;
        }
    }

    if (atexit(free_original_shaders) != 0) {
        fprintf(stderr, "Failed to register source-index cleanup\n");
        stop_worker_threads(workers, workers_started);
        verification_oracle_dispose();
        unity_compiler_broker_destroy(broker);
        return 1;
    }

    TypeTreeSchemaRegistry schema_registry;
    typetree_schema_registry_init(&schema_registry);
    TypeTreeSchemaRegistry* schema_registry_ptr = NULL;
    if (schema_registry_path) {
        schema_registry_ptr = &schema_registry;
        TypeTreeSchemaStatus status =
            typetree_schema_registry_import_file_replace(
                &schema_registry, schema_registry_path);
        if (status != TYPETREE_SCHEMA_OK) {
            fprintf(stderr,
                    "Failed to import TypeTree schema registry '%s': %s. "
                    "Create it from a SHA-256-pinned bundle with "
                    "typetree_schema_cli\n",
                    schema_registry_path,
                    typetree_schema_status_name(status));
            typetree_schema_registry_dispose(&schema_registry);
            stop_worker_threads(workers, workers_started);
            verification_oracle_dispose();
            unity_compiler_broker_destroy(broker);
            return 1;
        }
        printf("[INFO] Loaded %zu exact TypeTree schemas from %s\n",
               typetree_schema_registry_count(&schema_registry),
               schema_registry_path);
    }

    // 3. Discover and process a standalone SerializedFile, a UnityFS bundle,
    // or every supported Unity input below a directory. Discovery is sorted
    // and deterministic, and never follows descendant links.
    CommonPathDiscoveryOptions discovery_options;
    common_path_discovery_options_default(&discovery_options);
    CommonPathDiscoveryResult discovered;
    common_path_discovery_result_init(&discovered);
    const char* input_paths[] = {unity_input_path};
    CommonPathDiscoveryStatus discovery_status = common_path_discover(
        input_paths, 1U, &discovery_options, &discovered);
    if (discovery_status != COMMON_PATH_DISCOVERY_OK) {
        fprintf(stderr, "Unity input discovery failed for '%s': %s\n",
                unity_input_path,
                common_path_discovery_status_name(discovery_status));
        common_path_discovery_result_dispose(&discovered);
        typetree_schema_registry_dispose(&schema_registry);
        stop_worker_threads(workers, workers_started);
        verification_oracle_dispose();
        unity_compiler_broker_destroy(broker);
        return 1;
    }
    g_input_stats.requested_inputs = 1U;
    g_input_stats.discovered_files = discovered.count;
    bool serialized_inputs_ok = true;
    VerificationInputVisitorContext input_context = {
        .broker = broker,
        .schema_registry = schema_registry_ptr,
        .generated_shaderlab_dir = gen_shaderlab_dir,
        .serialized_inputs_ok = true,
        .allocation_failed = false,
    };
    for (size_t path_index = 0U; path_index < discovered.count;
         ++path_index) {
        const CommonDiscoveredPath* path = &discovered.paths[path_index];
        UnityInputProbe probe;
        UnityInputStatus input_status = unity_input_probe_path(
            path->path, &probe);
        if (input_status == UNITY_INPUT_UNRELATED) {
            if (path->explicit_file) {
                fprintf(stderr,
                        "Explicit Unity input is unrelated: '%s'\n",
                        path->path);
                serialized_inputs_ok = false;
            } else {
                g_input_stats.ignored_unrelated_descendants++;
            }
            continue;
        }
        if (input_status != UNITY_INPUT_OK) {
            fprintf(stderr, "Unity input probe failed for '%s': %s\n",
                    path->path, unity_input_status_name(input_status));
            serialized_inputs_ok = false;
            continue;
        }
        if (probe.kind == UNITY_INPUT_KIND_UNITYFS) {
            g_input_stats.unityfs_files++;
        } else if (probe.kind == UNITY_INPUT_KIND_SERIALIZED_FILE_V22) {
            g_input_stats.standalone_serialized_files++;
        } else {
            fprintf(stderr,
                    "Unity input probe returned unsupported kind for '%s': "
                    "%s\n", path->path,
                    unity_input_kind_name(probe.kind));
            serialized_inputs_ok = false;
            continue;
        }

        UnityInputVisitStats visit_stats;
        input_status = unity_input_visit_serialized(
            path->path, verification_visit_serialized_source,
            &input_context, &visit_stats);
        g_input_stats.resource_members += visit_stats.resource_members;
        g_input_stats.directory_members += visit_stats.directory_members;
        g_input_stats.deleted_members += visit_stats.deleted_members;
        if (input_status != UNITY_INPUT_OK) {
            fprintf(stderr, "Unity input visit failed for '%s': %s\n",
                    path->path, unity_input_status_name(input_status));
            serialized_inputs_ok = false;
        }
        if (input_context.allocation_failed) break;
    }
    serialized_inputs_ok = serialized_inputs_ok &&
        input_context.serialized_inputs_ok &&
        !input_context.allocation_failed;
    common_path_discovery_result_dispose(&discovered);

    // Cleanup
    typetree_schema_registry_dispose(&schema_registry);
    stop_worker_threads(workers, workers_started);
    UnityCompilerBrokerStats broker_stats;
    unity_compiler_broker_get_stats(broker, &broker_stats);
    UnityCompilerSessionCapabilities compiler_session;
    const bool compiler_session_available =
        unity_compiler_broker_session_capabilities_snapshot(
            broker, &compiler_session);
    UnityCompilerValidApisAuthority valid_apis_authority;
    memset(&valid_apis_authority, 0, sizeof(valid_apis_authority));
    const bool valid_apis_authority_available =
        unity_compiler_broker_expected_valid_apis_authority(
            broker, &valid_apis_authority);
    bool oracle_capture_ok = verification_oracle_finalize_capture();
    verification_oracle_dispose();
    unity_compiler_broker_destroy(broker);

    // Write out the legacy variant failure list.  A write failure is itself a
    // verification failure: silently retaining an older list is unsafe.
    bool diagnostics_ok = true;
    char failures_path[1024];
    snprintf(failures_path, sizeof(failures_path), "%s/failures.txt", g_failure_dir);
    FILE* f_fail = fopen(failures_path, "w");
    if (f_fail) {
        for (int i = 0; i < g_failure_count; i++) {
            fprintf(f_fail, "%lld %d %d %d 0x%02x\n", g_failures[i].path_id,
                    g_failures[i].stage, g_failures[i].sub_idx,
                    g_failures[i].pass_idx, g_failures[i].failure_kinds);
        }
        bool failure_list_ok = fflush(f_fail) == 0 && ferror(f_fail) == 0;
        if (fclose(f_fail) != 0) failure_list_ok = false;
        if (!failure_list_ok) {
            fprintf(stderr, "Could not finish failure list '%s': %s\n",
                    failures_path, strerror(errno));
            diagnostics_ok = false;
        } else {
            printf("[INFO] Wrote %d failures to %s\n", g_failure_count,
                   failures_path);
        }
    } else {
        fprintf(stderr, "Could not create failure list '%s': %s\n",
                failures_path, strerror(errno));
        diagnostics_ok = false;
    }

    // Summary report
    printf("\n");
    printf("============================================================\n");
    printf("       COMPILED-ARTIFACT ROUNDTRIP VERIFICATION SUMMARY     \n");
    printf("============================================================\n");
    if (g_direct_only_enabled) {
        printf("Scope: exact filter-selected serialized D3D11 subprogram "
               "rows%s (--direct-only).\n",
               g_verify_glsl
                   ? " plus released serialized GLCore targets"
                   : "");
        printf("A tuple filter selects exact serialized rows; the whole "
               "generated pass domain is NOT run or certified.\n");
    } else {
        printf("Scope: complete generated D3D11 pass domains%s.\n",
               g_direct_census_enabled
                   ? " plus exhaustive direct serialized-subprogram "
                     "artifacts"
                   : " (generated-domain-only mode)");
        printf("A tuple filter admits passes only; every admitted pass is "
               "certified across all generated states, stages, and tiers.\n");
    }
    printf("Whole-shader runtime selection, render/pipeline state, and "
           "visual output are NOT certified by this verifier.\n\n");
    printf("Unity input discovery:\n");
    printf("  - Files discovered: %zu\n", g_input_stats.discovered_files);
    printf("  - UnityFS / standalone SerializedFile: %zu / %zu\n",
           g_input_stats.unityfs_files,
           g_input_stats.standalone_serialized_files);
    printf("  - Serialized sources visited: %zu\n",
           g_input_stats.serialized_sources);
    printf("  - Unrelated descendants ignored: %zu\n",
           g_input_stats.ignored_unrelated_descendants);
    printf("  - Unsupported ComputeShader objects (ClassID 72): %zu\n\n",
           g_input_stats.compute_shader_objects);
    printf("Preprocess authority requests:\n");
    printf("  - Generated candidate / original source: %zu / %zu\n",
           g_generated_preprocess_authority_requests,
           g_original_preprocess_authority_requests);
    printf("  - Generated purpose: %s\n\n",
           g_direct_only_enabled
               ? "exact compileSnippet source and contract only"
               : "exact compileSnippet source/contract and generated domain");
    printf("D3D11 vertex/fragment/geometry/hull/domain variants "
           "inventoried: %d\n",
           g_dxbc_expected);
    printf("  - Shader objects parsed: %d / %d\n", g_shader_objects_parsed,
           g_shader_objects_seen);
    printf("  - Direct serialized-subprogram census: %s\n",
           direct_census_counts_status());
    if (g_direct_census_enabled) {
        printf("  - Scheduled and decoded: %d / %d\n", g_shaders_total,
               g_dxbc_expected);
        printf("  - Missing/unavailable:   %d\n",
               g_dxbc_expected > g_shaders_total
                   ? g_dxbc_expected - g_shaders_total
                   : 0);
    } else {
        printf("  - Scheduled/compiled/matched direct rows: not run "
               "(explicit --domain-only)\n");
    }
    printf("  - Unsupported stage variants (ray tracing): %d\n\n",
           g_dxbc_unsupported_stage);
    if (g_job_queue_failed) {
        printf("  - Verification scheduler allocation failure: yes\n\n");
    }
    printf("D3D11 whole generated-pass domain gate:\n");
    printf("  - Status: %s\n",
           generated_domain_counts_status(&g_generated_domain));
    if (!g_generated_domain_enabled) {
        printf("  - Not run: explicit --direct-only selected exact "
               "serialized rows instead.\n\n");
    } else {
    printf("  - Eligible / filter-admitted / filter-skipped passes: "
           "%zu / %zu / %zu\n",
           g_generated_domain.eligible_passes,
           g_generated_domain.filter_admitted_passes,
           g_generated_domain.filter_skipped_passes);
    printf("  - Plans / unique snippets / certified / failed / unreached: "
           "%zu / %zu / %zu / %zu / %zu\n",
           g_generated_domain.plans_built,
           g_generated_domain.snippets_uniquely_mapped,
           g_generated_domain.certified_passes,
           g_generated_domain.failed_passes,
           generated_domain_unreached_passes(&g_generated_domain));
    printf("  - Active stages / generated states: %zu / %zu\n",
           g_generated_domain.active_stages,
           g_generated_domain.generated_states);
    printf("  - Planned / attempted / clean / exact DXBC compiles: "
           "%zu / %zu / %zu / %zu\n",
           g_generated_domain.planned_compiles,
           g_generated_domain.compile_attempts,
           g_generated_domain.clean_compiles,
           g_generated_domain.matched_dxbc_containers);
    printf("  - Runtime-binding exact / compatible / planned: "
           "%zu / %zu / %zu\n",
           g_generated_domain.runtime_binding_attested_compiles,
           g_generated_domain.runtime_binding_compatible_compiles,
           g_generated_domain.planned_compiles);
    printf("  - Compiler diagnostic records retained: %zu\n",
           g_generated_domain.compiler_diagnostics);
    printf("  - Diagnostic attestation compiles / attested candidates: "
           "%zu / %zu\n",
           g_generated_domain.diagnostic_attestation_compiles,
           g_generated_domain.diagnostic_attested_compiles);
    printf("  - GLSL: %s\n\n",
           unity_generated_glsl_status_name(
               UNITY_GENERATED_GLSL_UNAVAILABLE_NO_PRECISION_AUTHORITY));
    }
    printf("Direct DXBC serialized-subprogram census:\n");
    if (g_direct_census_enabled) {
        printf("  - Compiled successfully: %d / %d\n", g_dxbc_compiled,
               g_dxbc_expected);
        printf("  - Executable token match: %d / %d\n",
               g_dxbc_token_matched, g_dxbc_expected);
        printf("  - Full byte match 1:1:   %d / %d\n\n", g_dxbc_matched,
               g_dxbc_expected);
    } else {
        printf("  - Status: not run (the complete generated domain is "
               "the active strict gate)\n\n");
    }
    printf("GLSL (released Platform 15 linked-program authority):\n");
    if (g_verify_glsl) {
        printf("  - Status: %s\n",
               serialized_glcore_counts_status(&g_serialized_glcore));
        printf("  - Objects evaluated / platform present / platform absent: "
               "%zu / %zu / %zu\n",
               g_serialized_glcore.objects_evaluated,
               g_serialized_glcore.platform_present_objects,
               g_serialized_glcore.platform_absent_objects);
        printf("  - Unavailable objects: %zu\n",
               g_serialized_glcore.unavailable_objects);
        printf("  - Targets discovered / expected / filtered: %zu / %zu / "
               "%zu\n",
               g_serialized_glcore.targets_discovered,
               g_serialized_glcore.targets_expected,
               g_serialized_glcore.targets_filtered_out);
        printf("  - Targets opened / compiled / exact: %zu / %zu / %zu\n",
               g_serialized_glcore.targets_opened,
               g_serialized_glcore.targets_compiled,
               g_serialized_glcore.targets_exact);
        printf("  - Targets unavailable / mismatched: %zu / %zu\n",
               g_serialized_glcore.targets_unavailable,
               g_serialized_glcore.targets_mismatched);
        printf("  - Strict OraclePack v4 unsupported rows: %zu\n",
               g_serialized_glcore.oracle_pack_v4_unsupported);
        printf("  - Original ShaderLab source required: no\n");
        printf("  - Empty fragment companion serialized/required: no / no\n");
    } else {
        printf("  - Status: not run (--dxbc-only)\n");
    }
    if (oracle_pack_path || oracle_capture_path) {
        printf("OraclePack authority:\n");
        printf("  - Exact hits / misses: %llu / %llu\n",
               (unsigned long long)g_oracle.hits,
               (unsigned long long)g_oracle.misses);
        printf("  - Broker/cache fallbacks: %llu\n",
               (unsigned long long)g_oracle.broker_fallbacks);
        printf("  - Captured / authority failures: %llu / %llu\n",
               (unsigned long long)g_oracle.captured_entries,
               (unsigned long long)g_oracle.authority_failures);
        printf("  - Preprocess hits / misses: %llu / %llu\n",
               (unsigned long long)g_oracle.preprocess_hits,
               (unsigned long long)g_oracle.preprocess_misses);
        printf("  - Preprocess broker fallbacks / captures: %llu / %llu\n",
               (unsigned long long)g_oracle.preprocess_broker_fallbacks,
               (unsigned long long)g_oracle.captured_preprocesses);
    }
    const char* cache_dir = getenv("DXBC_USC_CACHE_DIR");
    if (cache_dir && *cache_dir) {
        UnityCompilerCacheStats cache_stats;
        unity_compiler_cache_get_stats(&cache_stats);
        printf("UnityShaderCompiler cache (%s):\n", cache_dir);
        printf("  - Hits / misses / stores: %llu / %llu / %llu\n",
               (unsigned long long)cache_stats.hits,
               (unsigned long long)cache_stats.misses,
               (unsigned long long)cache_stats.stores);
        printf("  - Corrupt entries / write errors: %llu / %llu\n",
               (unsigned long long)cache_stats.corrupt_entries,
               (unsigned long long)cache_stats.write_errors);
    }
    printf("UnityShaderCompiler broker:\n");
    printf("  - Submitted / executed client requests: %llu / %llu\n",
           (unsigned long long)broker_stats.submitted_requests,
           (unsigned long long)broker_stats.executed_requests);
    printf("  - Compile requests coalesced in flight: %llu\n",
           (unsigned long long)broker_stats.coalesced_compile_requests);
    printf("  - Compiler process starts: %llu (hard maximum live: 1)\n",
           (unsigned long long)broker_stats.compiler_process_starts);
    printf("  - Process recycles / source-budget recycles: %llu / %llu\n",
           (unsigned long long)broker_stats.compiler_process_recycles,
           (unsigned long long)broker_stats.source_budget_recycles);
    printf("  - Source budget / current window / peak window bytes: "
           "%llu / %llu / %llu\n",
           (unsigned long long)broker_stats.source_residency_budget_bytes,
           (unsigned long long)broker_stats.tracked_source_window_bytes,
           (unsigned long long)
               broker_stats.peak_tracked_source_window_bytes);
    printf("  - Distinct sources in current residency window: %llu\n",
           (unsigned long long)broker_stats.tracked_unique_source_count);
    printf("  - Unique source submissions / exact digest scans: %llu / "
           "%llu\n",
           (unsigned long long)broker_stats.unique_source_submissions,
           (unsigned long long)broker_stats.source_digest_scans);
    printf("  - Source tracking failures: %llu\n",
           (unsigned long long)broker_stats.source_tracking_failures);
    if (compiler_session_available) {
        uint32_t session_valid_apis = 0U;
        if (unity_compiler_session_capabilities_valid_apis(
                &compiler_session, &session_valid_apis)) {
            printf("  - initializeCompiler raw mask / valid_apis: "
                   "0x%08" PRIx32 " / %" PRIu32 "\n",
                   compiler_session.raw_available_platform_mask,
                   session_valid_apis);
            printf("  - Profile valid_apis exact session match: %s\n",
                   unity_compiler_session_capabilities_match_valid_apis(
                       &compiler_session, g_compile_profile.valid_apis)
                       ? "yes" : "no");
        }
    } else {
        printf("  - initializeCompiler session: not captured "
               "(process-free cache path)\n");
    }
    printf("  - Pre-command validApis gate: %s",
           valid_apis_authority_available
               ? valid_apis_authority_status_name(
                     valid_apis_authority.status)
               : "query-failed");
    if (valid_apis_authority_available) {
        printf(" (expected=%" PRIu32,
               valid_apis_authority.expected_valid_apis);
        if (valid_apis_authority.status ==
                UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED ||
            valid_apis_authority.status ==
                UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH) {
            printf(", observed=%" PRIu32,
                   valid_apis_authority.observed_valid_apis);
        }
        printf(")");
    }
    printf("\n");
    printf("Unity compiler diagnostic gate:\n");
    printf("  - Observed / retained err: records: %zu / %zu\n",
           g_compiler_diagnostic_observed_count,
           g_compiler_diagnostic_count);
    printf("  - Informational / actionable records: %zu / %zu\n",
           g_compiler_diagnostic_informational_count,
           g_compiler_diagnostic_actionable_count);
    printf("  - Attested / unattested actionable records: %zu / %zu\n",
           g_compiler_diagnostic_attested_actionable_count,
           g_compiler_diagnostic_unattested_actionable_count);
    printf("  - Missing diagnostic authority: %zu\n",
           g_diagnostic_authority_failure_count);
    printf("  - Diagnostic retention complete: %s\n",
           g_compiler_diagnostic_allocation_failed ? "no" : "yes");
    printf("============================================================\n");

    bool glsl_complete = !g_verify_glsl ||
        (serialized_glcore_counts_are_exact(&g_serialized_glcore) &&
         !g_serialized_glcore_report_allocation_failed);
    bool generated_domain_complete = !g_generated_domain_enabled ||
        (generated_domain_counts_are_exact(&g_generated_domain) &&
         !g_generated_domain_report_allocation_failed);
    bool direct_census_complete = !g_direct_census_enabled ||
        strcmp(direct_census_counts_status(), "exact") == 0;
    const bool compiler_session_profile_exact =
        !compiler_session_available ||
        unity_compiler_session_capabilities_match_valid_apis(
            &compiler_session, g_compile_profile.valid_apis);
    const bool valid_apis_pre_command_gate_exact =
        valid_apis_authority_available &&
        ((compiler_session_available &&
          valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED) ||
         (!compiler_session_available &&
          valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING));
    bool selected_checks_exact =
        generated_domain_complete &&
        direct_census_complete &&
        compiler_session_profile_exact &&
        valid_apis_pre_command_gate_exact &&
        g_shader_objects_parsed == g_shader_objects_seen &&
        serialized_inputs_ok &&
        !g_job_queue_failed &&
        !g_shader_report_allocation_failed &&
        !g_generated_domain_report_allocation_failed &&
        !g_serialized_glcore_report_allocation_failed &&
        !g_input_report_allocation_failed &&
        !g_compiler_diagnostic_allocation_failed &&
        broker_stats.source_tracking_failures == 0U &&
        g_compiler_diagnostic_unattested_actionable_count == 0U &&
        g_diagnostic_authority_failure_count == 0U &&
        g_failure_count == 0 &&
        g_dxbc_unsupported_stage == 0 && glsl_complete &&
        g_oracle.authority_failures == 0 && oracle_capture_ok &&
        g_dxbc_expected > 0;
    const VerificationScopeInput scope_input = {
        .selection_filter_active = g_filter_count != 0,
        .selected_checks_exact = selected_checks_exact,
        .selected_shader_objects = (size_t)g_shader_objects_seen,
        .unsupported_shader_objects =
            g_unsupported_compute_shader_count,
    };
    const VerificationScopeResult scope =
        verification_scope_evaluate(&scope_input);
    const bool verification_passed = scope.requested_scope_passed;
    printf("Verification scope policy:\n");
    printf("  - Selected ClassID 48 scope: %s\n",
           scope.selected_scope_exact ? "exact" : "failed");
    printf("  - Direct serialized-subprogram census: %s\n",
           direct_census_counts_status());
    printf("  - Whole-input object-kind coverage: %s%s\n",
           scope.whole_input_object_kind_coverage_complete
               ? "complete" : "incomplete",
           g_filter_count == 0 ? " (requested)" : " (reported only)");
    bool report_ok = write_verification_report(
        report_path, unity_input_path, orig_shaders_dir, gen_shaderlab_dir,
        serialized_inputs_ok, oracle_capture_ok, diagnostics_ok,
        verification_passed, &scope,
        &broker_stats,
        compiler_session_available ? &compiler_session : NULL,
        valid_apis_authority_available ? &valid_apis_authority : NULL);

    free(g_failures);
    g_failures = NULL;
    free(g_filters);
    g_filters = NULL;
    free_shader_results();
    free_input_records();
    free_compiler_diagnostics();
    free_generated_domain_pass_records();
    free_serialized_glcore_target_records();

    if (verification_passed && diagnostics_ok && report_ok) {
        if (g_direct_only_enabled) {
            printf("[SUCCESS] Selected direct serialized-subprogram%s "
                   "verification PASSED.\n",
                   g_verify_glsl ? " plus serialized GLCore" : "");
        } else {
            printf("[SUCCESS] Whole generated D3D11 pass-domain%s "
                   "verification PASSED.\n",
                   g_direct_census_enabled
                       ? " plus direct serialized-subprogram"
                       : "");
        }
        printf("[NOTICE] This is not whole-shader or visual certification.\n");
        return 0;
    } else {
        printf("[FAIL] Requested compiled-artifact verification scope "
               "FAILED.\n");
        return 1;
    }
}
