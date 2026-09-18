#include "compiler/unity_compiler_broker.h"
#include "compiler/unity_compiler_singleflight.h"

#include <pthread.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define CONCURRENT_CALLS 8

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int ready_count;
    bool start;
} StartGate;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool release;
    bool fail;
    atomic_int call_count;
} OperationControl;

typedef struct {
    UnityCompilerSingleFlight* single_flight;
    uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE];
    StartGate* start_gate;
    OperationControl* operation;
    uint8_t* result;
    size_t result_size;
    char* error;
    bool joined;
} ThreadCall;

static char* duplicate_text(const char* text) {
    const size_t size = strlen(text) + 1u;
    char* copy = (char*)malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static bool write_test_file(const char* path, const char* text) {
    FILE* output = fopen(path, "wb");
    if (!output) return false;
    const size_t size = strlen(text);
    bool ok = fwrite(text, 1U, size, output) == size;
    if (fflush(output) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    return ok;
}

static uint8_t* controlled_operation(void* opaque, size_t* out_size,
                                     char** out_error) {
    static const uint8_t payload[] = {0x10, 0x00, 0x7f, 0xff};
    OperationControl* control = (OperationControl*)opaque;
    atomic_fetch_add_explicit(&control->call_count, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&control->mutex);
    while (!control->release) {
        pthread_cond_wait(&control->condition, &control->mutex);
    }
    const bool fail = control->fail;
    pthread_mutex_unlock(&control->mutex);

    *out_size = 0;
    *out_error = NULL;
    if (fail) {
        *out_error = duplicate_text("intentional failure");
        return NULL;
    }
    uint8_t* result = (uint8_t*)malloc(sizeof(payload));
    if (!result) return NULL;
    memcpy(result, payload, sizeof(payload));
    *out_size = sizeof(payload);
    return result;
}

static void* execute_thread_call(void* opaque) {
    ThreadCall* call = (ThreadCall*)opaque;
    pthread_mutex_lock(&call->start_gate->mutex);
    call->start_gate->ready_count++;
    pthread_cond_broadcast(&call->start_gate->condition);
    while (!call->start_gate->start) {
        pthread_cond_wait(&call->start_gate->condition,
                          &call->start_gate->mutex);
    }
    pthread_mutex_unlock(&call->start_gate->mutex);

    call->result = usc_single_flight_execute(
        call->single_flight, call->key, controlled_operation,
        call->operation, &call->result_size, &call->error, &call->joined);
    return NULL;
}

static bool wait_for_participants(UnityCompilerSingleFlight* single_flight,
                                  const uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE],
                                  size_t expected) {
    /* The owner operation is deliberately blocked, so participant count can
     * only move toward expected during this bounded scheduler-yield loop. */
    for (int attempt = 0; attempt < 5000; ++attempt) {
        if (usc_single_flight_participant_count(single_flight, key) ==
            expected) {
            return true;
        }
        usleep(1000);
    }
    return false;
}

static bool run_concurrent_calls(UnityCompilerSingleFlight* single_flight,
                                 OperationControl* operation,
                                 const uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE],
                                 bool expect_success) {
    StartGate gate;
    memset(&gate, 0, sizeof(gate));
    CHECK(pthread_mutex_init(&gate.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&gate.condition, NULL) == 0);

    pthread_t threads[CONCURRENT_CALLS];
    ThreadCall calls[CONCURRENT_CALLS];
    memset(calls, 0, sizeof(calls));
    for (int i = 0; i < CONCURRENT_CALLS; ++i) {
        calls[i].single_flight = single_flight;
        memcpy(calls[i].key, key, sizeof(calls[i].key));
        calls[i].start_gate = &gate;
        calls[i].operation = operation;
        CHECK(pthread_create(&threads[i], NULL, execute_thread_call,
                             &calls[i]) == 0);
    }

    pthread_mutex_lock(&gate.mutex);
    while (gate.ready_count != CONCURRENT_CALLS) {
        pthread_cond_wait(&gate.condition, &gate.mutex);
    }
    gate.start = true;
    pthread_cond_broadcast(&gate.condition);
    pthread_mutex_unlock(&gate.mutex);

    CHECK(wait_for_participants(single_flight, key, CONCURRENT_CALLS));
    pthread_mutex_lock(&operation->mutex);
    operation->release = true;
    pthread_cond_broadcast(&operation->condition);
    pthread_mutex_unlock(&operation->mutex);

    int joined_count = 0;
    for (int i = 0; i < CONCURRENT_CALLS; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        if (calls[i].joined) joined_count++;
        if (expect_success) {
            const uint8_t expected[] = {0x10, 0x00, 0x7f, 0xff};
            CHECK(calls[i].result != NULL);
            CHECK(calls[i].result_size == sizeof(expected));
            CHECK(memcmp(calls[i].result, expected, sizeof(expected)) == 0);
            CHECK(calls[i].error == NULL);
            for (int previous = 0; previous < i; ++previous) {
                CHECK(calls[i].result != calls[previous].result);
            }
        } else {
            CHECK(calls[i].result == NULL && calls[i].result_size == 0);
            CHECK(calls[i].error != NULL);
            CHECK(strcmp(calls[i].error, "intentional failure") == 0);
            for (int previous = 0; previous < i; ++previous) {
                CHECK(calls[i].error != calls[previous].error);
            }
        }
    }
    for (int i = 0; i < CONCURRENT_CALLS; ++i) {
        free(calls[i].result);
        free(calls[i].error);
    }
    CHECK(joined_count == CONCURRENT_CALLS - 1);
    CHECK(usc_single_flight_participant_count(single_flight, key) == 0);
    pthread_cond_destroy(&gate.condition);
    pthread_mutex_destroy(&gate.mutex);
    return true;
}

static bool verify_single_flight(void) {
    UnityCompilerSingleFlight* single_flight = usc_single_flight_create();
    CHECK(single_flight != NULL);
    uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE];
    memset(key, 0x5a, sizeof(key));

    OperationControl operation;
    memset(&operation, 0, sizeof(operation));
    CHECK(pthread_mutex_init(&operation.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&operation.condition, NULL) == 0);
    atomic_init(&operation.call_count, 0);
    CHECK(run_concurrent_calls(single_flight, &operation, key, true));
    CHECK(atomic_load_explicit(&operation.call_count,
                               memory_order_relaxed) == 1);

    operation.release = false;
    operation.fail = true;
    CHECK(run_concurrent_calls(single_flight, &operation, key, false));
    CHECK(atomic_load_explicit(&operation.call_count,
                               memory_order_relaxed) == 2);

    /* A failed flight is gone after its concurrent participants return. */
    operation.fail = false;
    operation.release = true;
    size_t result_size = 0;
    char* error = NULL;
    bool joined = true;
    uint8_t* result = usc_single_flight_execute(
        single_flight, key, controlled_operation, &operation, &result_size,
        &error, &joined);
    CHECK(result != NULL && result_size == 4 && error == NULL && !joined);
    CHECK(atomic_load_explicit(&operation.call_count,
                               memory_order_relaxed) == 3);
    free(result);

    pthread_cond_destroy(&operation.condition);
    pthread_mutex_destroy(&operation.mutex);
    usc_single_flight_destroy(single_flight);
    return true;
}

static bool verify_broker_stays_lazy(void) {
    char temporary_template[] = "/tmp/dxbc-broker-unit.XXXXXX";
    char* temporary_dir = mkdtemp(temporary_template);
    CHECK(temporary_dir != NULL);
    char cache_dir[1024];
    CHECK(snprintf(cache_dir, sizeof(cache_dir), "%s/cache",
                   temporary_dir) > 0);

    const char* previous = getenv("DXBC_USC_CACHE_DIR");
    char* saved = previous ? duplicate_text(previous) : NULL;
    CHECK(!previous || saved != NULL);
    CHECK(setenv("DXBC_USC_CACHE_DIR", cache_dir, 1) == 0);

    UnityCompilerBroker* broker = unity_compiler_broker_create(
        temporary_dir, temporary_dir);
    CHECK(broker != NULL);
    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.submitted_requests == 0);
    CHECK(stats.executed_requests == 0);
    CHECK(stats.compiler_process_starts == 0);
    CHECK(stats.compiler_process_recycles == 0);
    CHECK(!stats.compiler_process_running);
    UnityCompilerValidApisAuthority authority;
    CHECK(unity_compiler_broker_expected_valid_apis_authority(
        broker, &authority));
    CHECK(authority.status ==
          UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED);
    CHECK(!unity_compiler_broker_set_expected_valid_apis(
        broker, UNITY_COMPILER_PLATFORM_MASK | UINT32_C(0x80000000)));
    CHECK(unity_compiler_broker_set_expected_valid_apis(
        broker, UINT32_C(0x00048230)));
    CHECK(unity_compiler_broker_expected_valid_apis_authority(
        broker, &authority));
    CHECK(authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING &&
          authority.expected_valid_apis == UINT32_C(0x00048230) &&
          authority.observed_valid_apis == 0U);
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 0 &&
          !stats.compiler_process_running);
    CHECK(unity_compiler_broker_clear_expected_valid_apis(broker));
    CHECK(unity_compiler_broker_expected_valid_apis_authority(
        broker, &authority));
    CHECK(authority.status ==
          UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED);
    CHECK(unity_compiler_broker_recycle_process(broker));
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_recycles == 0);
    CHECK(!stats.compiler_process_running);
    unity_compiler_broker_destroy(broker);

    if (saved) {
        CHECK(setenv("DXBC_USC_CACHE_DIR", saved, 1) == 0);
    } else {
        CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);
    }
    free(saved);
    CHECK(rmdir(temporary_dir) == 0);
    return true;
}

static bool verify_contract_compile_requires_canonical_identity(void) {
    const char* previous = getenv("DXBC_UNITY_COMPILER_PATH");
    char* saved = previous ? duplicate_text(previous) : NULL;
    CHECK(!previous || saved != NULL);
    CHECK(setenv("DXBC_UNITY_COMPILER_PATH",
                 "/definitely/missing/UnityShaderCompiler", 1) == 0);

    UnityCompilerBroker* broker = unity_compiler_broker_create_lazy(".", ".");
    CHECK(broker != NULL);
    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    CHECK(unity_compiler_snippet_contract_validate(&contract));
    UnityCompilerSnippetCompileRequest request = {
        .snippet_source = "void vert() {}",
        .source_directory = "Assets",
        .source_basename = "CanonicalIdentity.shader",
        .pass_name = "",
        .contract = &contract,
    };
    size_t result_size = 99U;
    char* error = NULL;
    uint8_t* result = unity_compiler_broker_compile_contract(
        broker, &request, &result_size, &error);
    CHECK(result == NULL && result_size == 0U);
    CHECK(error != NULL &&
          strcmp(error, "Could not form canonical compiler request identity") ==
              0);
    free(error);

    uint8_t compiler_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t environment_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE];
    memset(compiler_fingerprint, 0x3c, sizeof(compiler_fingerprint));
    memset(environment_fingerprint, 0xc3, sizeof(environment_fingerprint));
    UnityCompilerOfflineAuthority authority = {
        compiler_fingerprint, environment_fingerprint};
    uint8_t* transcript = NULL;
    size_t transcript_size = 0U;
    uint8_t request_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    CHECK(unity_compiler_broker_serialize_compile_request_with_authority(
        broker, &request, &authority, &transcript, &transcript_size,
        request_digest));
    CHECK(transcript != NULL && transcript_size > 0U);
    free(transcript);

    UnityCompilerShaderPreprocessRequest preprocess = {
        .source = "Shader \"Offline\" {}",
        .file_path = "Assets/Offline.shader",
        .shader_name = "Offline",
    };
    transcript = NULL;
    transcript_size = 0U;
    CHECK(unity_compiler_broker_serialize_preprocess_request_with_authority(
        broker, &preprocess, &authority, &transcript, &transcript_size,
        request_digest));
    CHECK(transcript != NULL && transcript_size > 0U);
    free(transcript);

    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.submitted_requests == 1U);
    CHECK(stats.compile_requests == 1U);
    CHECK(stats.executed_requests == 0U);
    CHECK(stats.compiler_process_starts == 0U);
    CHECK(!stats.compiler_process_running);
    unity_compiler_broker_destroy(broker);

    if (saved) {
        CHECK(setenv("DXBC_UNITY_COMPILER_PATH", saved, 1) == 0);
    } else {
        CHECK(unsetenv("DXBC_UNITY_COMPILER_PATH") == 0);
    }
    free(saved);
    return true;
}

static bool verify_source_budget_exact_mutation_and_lifecycle(void) {
    const char* previous_compiler = getenv("DXBC_UNITY_COMPILER_PATH");
    const char* previous_cache = getenv("DXBC_USC_CACHE_DIR");
    char* saved_compiler = previous_compiler
        ? duplicate_text(previous_compiler) : NULL;
    char* saved_cache = previous_cache ? duplicate_text(previous_cache) : NULL;
    CHECK(!previous_compiler || saved_compiler != NULL);
    CHECK(!previous_cache || saved_cache != NULL);
    CHECK(setenv("DXBC_UNITY_COMPILER_PATH",
                 "/definitely/missing/UnityShaderCompiler", 1) == 0);
    CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);

    UnityCompilerBroker* broker =
        unity_compiler_broker_create_lazy(".", ".");
    CHECK(broker != NULL);
    char source[] = "Shader \"A\" {}";
    const uint64_t source_size = (uint64_t)strlen(source);
    CHECK(unity_compiler_broker_set_source_residency_budget(
        broker, source_size * 8U));

    PreprocessResult result;
    CHECK(!unity_compiler_broker_preprocess(
        broker, source, "BudgetMutation", &result));
    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.source_residency_budget_bytes == source_size * 8U);
    CHECK(stats.tracked_source_window_bytes == source_size);
    CHECK(stats.peak_tracked_source_window_bytes == source_size);
    CHECK(stats.tracked_unique_source_count == 1U);
    CHECK(stats.unique_source_submissions == 1U);
    CHECK(stats.source_digest_scans == 1U);
    CHECK(stats.source_tracking_failures == 0U);
    CHECK(stats.source_budget_recycles == 0U);

    /* An identical submission is scanned for mutation safety, but its exact
     * content is counted only once in the current residency window. */
    CHECK(!unity_compiler_broker_preprocess(
        broker, source, "BudgetMutation", &result));
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.tracked_source_window_bytes == source_size);
    CHECK(stats.tracked_unique_source_count == 1U);
    CHECK(stats.unique_source_submissions == 1U);
    CHECK(stats.source_digest_scans == 2U);

    /* Same address and size, different bytes: pointer/length memoization
     * would miss this mutation.  Exact digesting must create a new record. */
    char* marker = strchr(source, 'A');
    CHECK(marker != NULL);
    *marker = 'B';
    CHECK(!unity_compiler_broker_preprocess(
        broker, source, "BudgetMutation", &result));
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.tracked_source_window_bytes == source_size * 2U);
    CHECK(stats.peak_tracked_source_window_bytes == source_size * 2U);
    CHECK(stats.tracked_unique_source_count == 2U);
    CHECK(stats.unique_source_submissions == 2U);
    CHECK(stats.source_digest_scans == 3U);

    /* Manual lifecycle boundaries reset the window even if the lazy broker
     * has no live process.  Configuration and cumulative stats survive. */
    CHECK(unity_compiler_broker_recycle_process(broker));
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.tracked_source_window_bytes == 0U);
    CHECK(stats.tracked_unique_source_count == 0U);
    CHECK(stats.compiler_process_recycles == 0U);
    CHECK(!unity_compiler_broker_preprocess(
        broker, source, "BudgetMutation", &result));
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.tracked_source_window_bytes == source_size);
    CHECK(stats.tracked_unique_source_count == 1U);
    CHECK(stats.unique_source_submissions == 3U);
    CHECK(stats.source_digest_scans == 4U);

    /* Exercise open-addressing growth with one repeatedly reused mutable
     * buffer, then prove the complete set still deduplicates. */
    CHECK(unity_compiler_broker_set_source_residency_budget(
        broker, UINT64_C(1024) * UINT64_C(1024)));
    uint64_t growth_bytes = 0U;
    char generated_source[64];
    for (unsigned int i = 0U; i < 80U; ++i) {
        const int written = snprintf(
            generated_source, sizeof(generated_source),
            "Shader \"Budget%03u\" {}", i);
        CHECK(written > 0 && (size_t)written < sizeof(generated_source));
        growth_bytes += (uint64_t)written;
        CHECK(!unity_compiler_broker_preprocess(
            broker, generated_source, "BudgetGrowth", &result));
    }
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.tracked_source_window_bytes == growth_bytes);
    CHECK(stats.tracked_unique_source_count == 80U);
    CHECK(stats.unique_source_submissions == 83U);
    CHECK(stats.source_digest_scans == 84U);
    for (unsigned int i = 0U; i < 80U; ++i) {
        const int written = snprintf(
            generated_source, sizeof(generated_source),
            "Shader \"Budget%03u\" {}", i);
        CHECK(written > 0 && (size_t)written < sizeof(generated_source));
        CHECK(!unity_compiler_broker_preprocess(
            broker, generated_source, "BudgetGrowth", &result));
    }
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.tracked_source_window_bytes == growth_bytes);
    CHECK(stats.tracked_unique_source_count == 80U);
    CHECK(stats.unique_source_submissions == 83U);
    CHECK(stats.source_digest_scans == 164U);

    /* Zero is an explicit off switch and releases the accounting window. */
    CHECK(unity_compiler_broker_set_source_residency_budget(broker, 0U));
    CHECK(!unity_compiler_broker_preprocess(
        broker, source, "BudgetMutation", &result));
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.source_residency_budget_bytes == 0U);
    CHECK(stats.tracked_source_window_bytes == 0U);
    CHECK(stats.tracked_unique_source_count == 0U);
    CHECK(stats.unique_source_submissions == 83U);
    CHECK(stats.source_digest_scans == 164U);
    unity_compiler_broker_destroy(broker);

    if (saved_compiler) {
        CHECK(setenv("DXBC_UNITY_COMPILER_PATH", saved_compiler, 1) == 0);
    } else {
        CHECK(unsetenv("DXBC_UNITY_COMPILER_PATH") == 0);
    }
    if (saved_cache) {
        CHECK(setenv("DXBC_USC_CACHE_DIR", saved_cache, 1) == 0);
    } else {
        CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);
    }
    free(saved_compiler);
    free(saved_cache);
    return true;
}

static bool verify_source_budget_recycle_policy(void) {
    const uint64_t mib32 = UINT64_C(32) * UINT64_C(1024) * UINT64_C(1024);
    const uint64_t standard_source = UINT64_C(35794617);

    /* Disabled tracking never recycles. */
    CHECK(!unity_compiler_broker_source_window_requires_recycle(
        0U, UINT64_MAX, UINT64_MAX));

    /* A single source is atomic.  Below, exactly at, or above the budget it
     * remains resident so thousands of variants do not restart USC. */
    CHECK(!unity_compiler_broker_source_window_requires_recycle(
        mib32, mib32 - 1U, 1U));
    CHECK(!unity_compiler_broker_source_window_requires_recycle(
        mib32, mib32, 1U));
    CHECK(!unity_compiler_broker_source_window_requires_recycle(
        mib32, standard_source, 1U));

    /* The configured budget is inclusive for a multi-source window. */
    CHECK(!unity_compiler_broker_source_window_requires_recycle(
        mib32, mib32, 2U));
    CHECK(unity_compiler_broker_source_window_requires_recycle(
        mib32, mib32 + 1U, 2U));
    CHECK(unity_compiler_broker_source_window_requires_recycle(
        mib32, standard_source + 1U, 2U));
    return true;
}

static bool verify_oversized_source_live_process_lifecycle(
    const char* executable_path) {
    char self_path[PATH_MAX];
    CHECK(realpath(executable_path, self_path) != NULL);
    char* separator = strrchr(self_path, '/');
    CHECK(separator != NULL);
    *separator = '\0';
    char fake_compiler[PATH_MAX];
    CHECK(snprintf(fake_compiler, sizeof(fake_compiler),
                   "%s/test_compiler_client_units", self_path) > 0);
    CHECK(access(fake_compiler, X_OK) == 0);

    char temporary_template[] = "/tmp/dxbc_broker_live.XXXXXX";
    char* temporary_dir = mkdtemp(temporary_template);
    CHECK(temporary_dir != NULL);
    char builtin_includes[PATH_MAX];
    char glslang_path[PATH_MAX];
    char dxcompiler_path[PATH_MAX];
    CHECK(snprintf(builtin_includes, sizeof(builtin_includes),
                   "%s/builtin", temporary_dir) > 0);
    CHECK(snprintf(glslang_path, sizeof(glslang_path),
                   "%s/glslang.dylib", temporary_dir) > 0);
    CHECK(snprintf(dxcompiler_path, sizeof(dxcompiler_path),
                   "%s/libdxcompiler.dylib", temporary_dir) > 0);
    CHECK(mkdir(builtin_includes, 0700) == 0);
    CHECK(write_test_file(glslang_path, "fake-glslang"));
    CHECK(write_test_file(dxcompiler_path, "fake-dxcompiler"));

    CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);
    CHECK(setenv("DXBC_USC_FAKE_SERVER", "1", 1) == 0);
    CHECK(setenv("DXBC_USC_IO_TIMEOUT_MS", "3000", 1) == 0);
    CHECK(setenv("DXBC_UNITY_CONTENTS_PATH", temporary_dir, 1) == 0);
    CHECK(setenv("DXBC_UNITY_COMPILER_PATH", fake_compiler, 1) == 0);
    CHECK(setenv("DXBC_UNITY_BUILTIN_INCLUDES_PATH",
                 builtin_includes, 1) == 0);
    CHECK(setenv("DXBC_UNITY_PLAYBACK_ENGINES_PATH",
                 temporary_dir, 1) == 0);
    CHECK(setenv("DXBC_UNITY_GLSLANG_PATH", glslang_path, 1) == 0);
    CHECK(setenv("DXBC_UNITY_DXCOMPILER_PATH", dxcompiler_path, 1) == 0);

    UnityCompilerBroker* broker = unity_compiler_broker_create_lazy(
        temporary_dir, builtin_includes);
    CHECK(broker != NULL);
    CHECK(unity_compiler_broker_set_source_residency_budget(broker, 32U));

    UnityCompilerSessionCapabilities session_capabilities;
    CHECK(!unity_compiler_broker_session_capabilities_snapshot(
        broker, &session_capabilities));
    CHECK(unity_compiler_broker_capture_session_capabilities(
        broker, &session_capabilities));
    uint32_t valid_apis = 0U;
    CHECK(unity_compiler_session_capabilities_valid_apis(
        &session_capabilities, &valid_apis));
    CHECK(valid_apis == UINT32_C(0x00048230));
    CHECK(unity_compiler_broker_set_expected_valid_apis(
        broker, valid_apis));
    UnityCompilerValidApisAuthority authority;
    CHECK(unity_compiler_broker_expected_valid_apis_authority(
        broker, &authority));
    CHECK(authority.status ==
          UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED);
    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 1U);
    CHECK(stats.compiler_process_running);
    CHECK(unity_compiler_broker_capture_session_capabilities(
        broker, &session_capabilities));
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 1U);

    char oversized_source[] = "012345678901234567890123456789012";
    CHECK(strlen(oversized_source) == 33U);
    size_t result_size = 0U;
    char* error = NULL;
    uint8_t* result = unity_compiler_broker_compile(
        broker, oversized_source, "BrokerOversized", 0, 4, 0U,
        NULL, 0, NULL, 0, &result_size, &error);
    CHECK(result != NULL && result_size > 0U && error == NULL);
    free(result);

    char* variant_keywords[] = {"SECOND_VARIANT"};
    result = unity_compiler_broker_compile(
        broker, oversized_source, "BrokerOversized", 0, 4, 0U,
        variant_keywords, 1, NULL, 0, &result_size, &error);
    CHECK(result != NULL && result_size > 0U && error == NULL);
    free(result);

    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 1U);
    CHECK(stats.compiler_process_recycles == 0U);
    CHECK(stats.source_budget_recycles == 0U);
    CHECK(stats.tracked_source_window_bytes == 33U);
    CHECK(stats.tracked_unique_source_count == 1U);
    CHECK(stats.unique_source_submissions == 1U);
    CHECK(stats.source_digest_scans == 2U);
    CHECK(stats.compiler_process_running);

    /* A same-address, same-size mutation is a different exact source.  The
     * completed transaction is returned, then the over-budget two-source
     * window is retired. */
    oversized_source[0] = 'x';
    result = unity_compiler_broker_compile(
        broker, oversized_source, "BrokerOversized", 0, 4, 0U,
        NULL, 0, NULL, 0, &result_size, &error);
    CHECK(result != NULL && result_size > 0U && error == NULL);
    free(result);
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 1U);
    CHECK(stats.compiler_process_recycles == 1U);
    CHECK(stats.source_budget_recycles == 1U);
    CHECK(stats.tracked_source_window_bytes == 0U);
    CHECK(stats.tracked_unique_source_count == 0U);
    CHECK(!stats.compiler_process_running);
    CHECK(unity_compiler_broker_expected_valid_apis_authority(
        broker, &authority));
    CHECK(authority.status ==
          UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED);

    /* The new singleton starts one replacement process and remains resident
     * instead of entering the former request/start/shutdown loop. */
    result = unity_compiler_broker_compile(
        broker, oversized_source, "BrokerOversized", 0, 4, 0U,
        variant_keywords, 1, NULL, 0, &result_size, &error);
    CHECK(result != NULL && result_size > 0U && error == NULL);
    free(result);
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 2U);
    CHECK(stats.compiler_process_recycles == 1U);
    CHECK(stats.source_budget_recycles == 1U);
    CHECK(stats.tracked_source_window_bytes == 33U);
    CHECK(stats.tracked_unique_source_count == 1U);
    CHECK(stats.compiler_process_running);
    CHECK(unity_compiler_broker_expected_valid_apis_authority(
        broker, &authority));
    CHECK(authority.status ==
          UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED);
    unity_compiler_broker_destroy(broker);

    CHECK(unlink(glslang_path) == 0);
    CHECK(unlink(dxcompiler_path) == 0);
    CHECK(rmdir(builtin_includes) == 0);
    CHECK(rmdir(temporary_dir) == 0);
    return true;
}

static bool run_all_tests(const char* executable_path) {
    CHECK(verify_single_flight());
    CHECK(verify_broker_stays_lazy());
    CHECK(verify_contract_compile_requires_canonical_identity());
    CHECK(verify_source_budget_exact_mutation_and_lifecycle());
    CHECK(verify_source_budget_recycle_policy());
    CHECK(verify_oversized_source_live_process_lifecycle(executable_path));
    return true;
}

int main(int argc, char** argv) {
    return argc > 0 && run_all_tests(argv[0]) ? 0 : 1;
}
