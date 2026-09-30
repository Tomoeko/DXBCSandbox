#include "compiler/unity_compiler_broker.h"
#include "compiler/unity_compiler_singleflight.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/socket.h>
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
#define BROKER_FAKE_MAGIC UINT32_C(0x0C0BD1E4)
#define BROKER_FAKE_VALID_APIS UINT32_C(0x00048230)

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

/* This small peer exercises the broker's complete protocol boundary.  Its
 * payload is controlled transport data, not a decoded ComputeShaderBinary. */
static bool broker_fake_transfer(int fd, void* data, size_t size, bool writing) {
    uint8_t* cursor = (uint8_t*)data;
    while (size > 0U) {
        const ssize_t count = writing ? write(fd, cursor, size)
                                      : read(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return true;
}

static bool broker_fake_scalar(int fd, void* value, size_t size, bool writing) {
    uint32_t magic = BROKER_FAKE_MAGIC;
    return broker_fake_transfer(fd, &magic, sizeof(magic), writing) &&
           magic == BROKER_FAKE_MAGIC &&
           broker_fake_transfer(fd, value, size, writing);
}

static bool broker_fake_write_u32(int fd, uint32_t value) {
    return broker_fake_scalar(fd, &value, sizeof(value), true);
}

static bool broker_fake_write_u64(int fd, uint64_t value) {
    return broker_fake_scalar(fd, &value, sizeof(value), true);
}

static bool broker_fake_expect_u32(int fd, uint32_t expected) {
    uint32_t value = 0U;
    return broker_fake_scalar(fd, &value, sizeof(value), false) &&
           value == expected;
}

static bool broker_fake_write_text(int fd, const char* text) {
    uint64_t size = (uint64_t)strlen(text);
    return broker_fake_scalar(fd, &size, sizeof(size), true) &&
           broker_fake_transfer(fd, (void*)text, (size_t)size, true);
}

static char* broker_fake_read_text(int fd) {
    uint64_t size = 0U;
    if (!broker_fake_scalar(fd, &size, sizeof(size), false) || size > 4096U) {
        return NULL;
    }
    char* text = (char*)malloc((size_t)size + 1U);
    if (!text) return NULL;
    if (!broker_fake_transfer(fd, text, (size_t)size, false)) {
        free(text);
        return NULL;
    }
    text[size] = '\0';
    return text;
}

static bool broker_fake_expect_text(int fd, const char* expected) {
    char* text = broker_fake_read_text(fd);
    const bool matches = text && strcmp(text, expected) == 0;
    free(text);
    return matches;
}

static bool broker_fake_initialize(int fd) {
    if (!broker_fake_expect_text(fd, "initializeCompiler")) return false;
    uint32_t count = 0U;
    if (!broker_fake_scalar(fd, &count, sizeof(count), false) ||
        count < 1U || count > 16U) return false;
    for (uint32_t index = 0U; index < count; ++index) {
        char* directory = broker_fake_read_text(fd);
        if (!directory) return false;
        free(directory);
    }
    if (!broker_fake_expect_u32(fd, 0U)) return false;
    char* configuration = broker_fake_read_text(fd);
    if (!configuration) return false;
    free(configuration);
    if (!broker_fake_write_u32(fd, BROKER_FAKE_VALID_APIS |
                                    ~UNITY_COMPILER_PLATFORM_MASK)) return false;
    for (size_t platform = 0U; platform < UNITY_COMPILER_PLATFORM_COUNT;
         ++platform) {
        if (!broker_fake_write_u64(fd, UINT64_C(0x1020304050607080) +
                                      (uint64_t)platform) ||
            !broker_fake_write_u32(fd, 7300U + (uint32_t)platform)) return false;
    }
    return true;
}

static bool broker_fake_compute_request(int fd, const char* root,
                                        bool preprocessing, char** source) {
    *source = broker_fake_read_text(fd);
    char filename[PATH_MAX];
    const int written = snprintf(filename, sizeof(filename),
                                 "%s/Compute.compute", root);
    if (!*source || written <= 0 || (size_t)written >= sizeof(filename) ||
        !broker_fake_expect_text(fd, filename)) return false;
    if (!preprocessing && !broker_fake_expect_text(fd, "ComputeMain")) {
        return false;
    }
    if (!broker_fake_expect_u32(fd, 1U)) return false;
    if (!preprocessing && (!broker_fake_expect_u32(fd, 0U) ||
                           !broker_fake_expect_u32(fd, 1U))) return false;
    if (!broker_fake_expect_u32(fd, 19U)) return false;
    if (preprocessing) {
        if (!broker_fake_expect_u32(fd, BROKER_FAKE_VALID_APIS)) return false;
    } else if (!broker_fake_expect_u32(fd, 1U) ||
               !broker_fake_expect_text(fd, "VALUE") ||
               !broker_fake_expect_text(fd, "2")) return false;
    if (!broker_fake_expect_u32(fd, 1U) ||
        !broker_fake_expect_text(fd, "PLATFORM_A") ||
        !broker_fake_expect_u32(fd, 1U) ||
        !broker_fake_expect_text(fd, preprocessing ? "DISABLED_A" : "USER_A")) {
        return false;
    }
    if (preprocessing) return true;
    uint64_t requirements = 0U;
    return broker_fake_expect_u32(fd, 4U) &&
           broker_fake_expect_u32(fd, 7U) &&
           broker_fake_scalar(fd, &requirements, sizeof(requirements), false) &&
           requirements == UINT64_C(0x100004001) &&
           broker_fake_expect_u32(fd, 16U) &&
           broker_fake_expect_u32(fd, 32U);
}

static bool broker_fake_preprocess_response(int fd, const char* source) {
    const bool uncaptured = strcmp(source, "broker-uncaptured") == 0;
    if (!broker_fake_write_text(fd, "computeKeywordsUserGlobal: 0") ||
        !broker_fake_write_text(fd, "computeKeywordsUserLocal: 0") ||
        !broker_fake_write_text(fd, "kernel: ComputeMain 0") ||
        !broker_fake_write_text(fd, "requirements:") ||
        !broker_fake_write_u64(fd, UINT64_C(0x100004001)) ||
        !broker_fake_write_u32(fd, 0U) ||
        !broker_fake_write_text(fd, uncaptured
            ? "endKernels: 7 0 0 0 0 1" : "endKernels: 7 0 0 0 0 0")) {
        return false;
    }
    if (uncaptured && !broker_fake_write_text(
            fd, "/UncapturedBrokerDependency/Foreign.hlsl")) return false;
    return broker_fake_write_text(fd, "// controlled preprocessing output\n") &&
           broker_fake_write_u32(fd, BROKER_FAKE_VALID_APIS) &&
           broker_fake_write_u32(fd, 16U) &&
           broker_fake_write_u32(fd, 32U);
}

static int run_broker_fake_compute(const char* root, const char* port_text) {
    char* end = NULL;
    const long port = strtol(port_text, &end, 10);
    if (end == port_text || *end || port <= 0 || port > 65535) return 2;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 3;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    int connected;
    do {
        connected = connect(fd, (struct sockaddr*)&address, sizeof(address));
    } while (connected < 0 && errno == EINTR);
    if (connected < 0 || !broker_fake_write_text(fd, "") ||
        !broker_fake_initialize(fd)) {
        close(fd);
        return 4;
    }
    for (;;) {
        char* command = broker_fake_read_text(fd);
        if (!command) break;
        if (strcmp(command, "shutdown") == 0) {
            free(command);
            break;
        }
        const bool preprocessing = strcmp(command, "preprocessCompute") == 0;
        const bool known = preprocessing ||
                           strcmp(command, "compileComputeKernel") == 0;
        free(command);
        char* source = NULL;
        bool ok = known && broker_fake_compute_request(fd, root, preprocessing,
                                                       &source);
        /* These calls must be rejected before any native request is sent. */
        if (ok && strcmp(source, "broker-authority-rejected") == 0) ok = false;
        if (ok && preprocessing) {
            ok = broker_fake_preprocess_response(fd, source);
        } else if (ok) {
            static uint8_t payload[] = {0x31U, 0U, 0x7fU, 0xffU};
            const bool rejected = strcmp(source, "broker-native-rejected") == 0;
            uint64_t size = rejected ? 0U : sizeof(payload);
            ok = broker_fake_write_text(fd, rejected
                                           ? "computeData: 0" : "computeData: 1") &&
                 broker_fake_scalar(fd, &size, sizeof(size), true) &&
                 broker_fake_transfer(fd, payload, (size_t)size, true);
        }
        free(source);
        if (!ok) break;
    }
    close(fd);
    return 0;
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
        .source_directory = "Assets",
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
    UnityCompilerBinaryResponse response;
    CHECK(setenv("DXBC_USC_CACHE_ONLY", "1", 1) == 0);
    CHECK(unity_compiler_broker_compile_response(
        broker, "new-uncached-request", "Fixture", 0, 4, 0U,
        NULL, 0, NULL, 0, &response));
    CHECK(response.status.availability == UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS);
    CHECK(!unity_compiler_response_status_is_clean_success(&response.status));
    unity_compiler_binary_response_free(&response);
    CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);
    CHECK(!unity_compiler_broker_compile_response(
        broker, "invalid-array", "Fixture", 0, 4, 0U,
        NULL, 1, NULL, 0, &response));
    CHECK(response.data == NULL && response.status.diagnostic_count == 0U);
    unity_compiler_binary_response_free(&response);
    unity_compiler_broker_destroy(broker);

    CHECK(unlink(glslang_path) == 0);
    CHECK(unlink(dxcompiler_path) == 0);
    CHECK(rmdir(builtin_includes) == 0);
    CHECK(rmdir(temporary_dir) == 0);
    return true;
}

static bool verify_compute_broker_identity_and_residency(
    const char* executable_path) {
    char executable[PATH_MAX];
    CHECK(realpath(executable_path, executable) != NULL);
    char temporary_template[] = "/tmp/dxbc_broker_compute.XXXXXX";
    char* root = mkdtemp(temporary_template);
    CHECK(root != NULL);
    char includes[PATH_MAX];
    char glslang[PATH_MAX];
    char dxcompiler[PATH_MAX];
    char filename[PATH_MAX];
    CHECK(snprintf(includes, sizeof(includes), "%s/builtin", root) > 0);
    CHECK(snprintf(glslang, sizeof(glslang), "%s/glslang.dylib", root) > 0);
    CHECK(snprintf(dxcompiler, sizeof(dxcompiler), "%s/libdxcompiler.dylib",
                   root) > 0);
    CHECK(snprintf(filename, sizeof(filename), "%s/Compute.compute", root) > 0);
    CHECK(mkdir(includes, 0700) == 0);
    CHECK(write_test_file(glslang, "controlled-glslang"));
    CHECK(write_test_file(dxcompiler, "controlled-dxcompiler"));

    struct {
        const char* name;
        const char* value;
        char* previous;
    } environment[] = {
        {"DXBC_USC_CACHE_DIR", NULL, NULL},
        {"DXBC_USC_CACHE_ONLY", NULL, NULL},
        {"DXBC_USC_BROKER_FAKE_COMPUTE", "1", NULL},
        {"DXBC_USC_IO_TIMEOUT_MS", "3000", NULL},
        {"DXBC_UNITY_CONTENTS_PATH", root, NULL},
        {"DXBC_UNITY_COMPILER_PATH", executable, NULL},
        {"DXBC_UNITY_BUILTIN_INCLUDES_PATH", includes, NULL},
        {"DXBC_UNITY_PLAYBACK_ENGINES_PATH", root, NULL},
        {"DXBC_UNITY_GLSLANG_PATH", glslang, NULL},
        {"DXBC_UNITY_DXCOMPILER_PATH", dxcompiler, NULL},
    };
    for (size_t index = 0U; index < sizeof(environment) / sizeof(environment[0]);
         ++index) {
        const char* previous = getenv(environment[index].name);
        environment[index].previous = previous ? duplicate_text(previous) : NULL;
        CHECK(!previous || environment[index].previous != NULL);
        CHECK(environment[index].value
            ? setenv(environment[index].name, environment[index].value, 1) == 0
            : unsetenv(environment[index].name) == 0);
    }

    UnityCompilerBroker* broker = unity_compiler_broker_create_lazy(root, includes);
    CHECK(broker != NULL);
    CHECK(unity_compiler_broker_set_source_residency_budget(broker, 4096U));
    CHECK(unity_compiler_broker_set_expected_valid_apis(
        broker, BROKER_FAKE_VALID_APIS));
    char source[] = "broker-compute-source";
    char* platform_keywords[] = {"PLATFORM_A"};
    char* user_keywords[] = {"USER_A"};
    char* disabled_keywords[] = {"DISABLED_A"};
    const UnityCompilerComputeMacro macros[] = {{"VALUE", "2"}};
    UnityCompilerComputeKernelRequest compile = {
        .source = source,
        .source_filename = filename,
        .kernel_name = "ComputeMain",
        .caching_preprocessor = true,
        .strip_line_directives = true,
        .build_platform = 19U,
        .kernel_macros = macros,
        .kernel_macro_count = 1,
        .platform_keywords = platform_keywords,
        .platform_keyword_count = 1,
        .user_keywords = user_keywords,
        .user_keyword_count = 1,
        .compiler_platform = 4,
        .compilation_flags = 7U,
        .requirements = UINT64_C(0x100004001),
        .force_dxc = 16U,
        .force_fxc = 32U,
    };
    UnityCompilerComputePreprocessRequest preprocess = {
        .source = source,
        .source_filename = filename,
        .caching_preprocessor = true,
        .build_platform = 19U,
        .valid_apis = BROKER_FAKE_VALID_APIS,
        .platform_keywords = platform_keywords,
        .platform_keyword_count = 1,
        .disabled_keywords = disabled_keywords,
        .disabled_keyword_count = 1,
    };
    uint8_t compile_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t preprocess_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t changed_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t* transcript = NULL;
    size_t transcript_size = 0U;
    CHECK(unity_compiler_broker_serialize_compute_request(
        broker, &compile, &transcript, &transcript_size, compile_digest));
    CHECK(transcript != NULL && transcript_size > 0U);
    free(transcript);
    CHECK(unity_compiler_broker_serialize_compute_preprocess_request(
        broker, &preprocess, &transcript, &transcript_size, preprocess_digest));
    CHECK(transcript != NULL && transcript_size > 0U &&
          memcmp(compile_digest, preprocess_digest, sizeof(compile_digest)) != 0);
    free(transcript);
    compile.kernel_name = "DifferentKernel";
    CHECK(unity_compiler_broker_serialize_compute_request(
        broker, &compile, &transcript, &transcript_size, changed_digest));
    CHECK(memcmp(compile_digest, changed_digest, sizeof(compile_digest)) != 0);
    free(transcript);
    compile.kernel_name = "ComputeMain";
    disabled_keywords[0] = "DISABLED_B";
    CHECK(unity_compiler_broker_serialize_compute_preprocess_request(
        broker, &preprocess, &transcript, &transcript_size, changed_digest));
    CHECK(memcmp(preprocess_digest, changed_digest, sizeof(preprocess_digest)) != 0);
    free(transcript);
    disabled_keywords[0] = "DISABLED_A";
    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.submitted_requests == 0U && stats.executed_requests == 0U &&
          stats.compiler_process_starts == 0U && !stats.compiler_process_running &&
          stats.source_digest_scans == 0U && stats.tracked_source_window_bytes == 0U);

    UnityCompilerBinaryResponse binary;
    CHECK(unity_compiler_broker_compile_compute_response(broker, &compile, &binary));
    const uint8_t payload[] = {0x31U, 0U, 0x7fU, 0xffU};
    CHECK(binary.status.availability == UNITY_COMPILER_RESPONSE_AVAILABLE &&
          unity_compiler_response_status_is_clean_success(&binary.status) &&
          binary.size == sizeof(payload) && binary.data != NULL &&
          memcmp(binary.data, payload, sizeof(payload)) == 0 &&
          binary.has_request_identity &&
          memcmp(binary.request_digest, compile_digest, sizeof(compile_digest)) == 0);
    unity_compiler_binary_response_free(&binary);
    UnityCompilerComputePreprocessResponse* response = NULL;
    UnityCompilerComputePreprocessInfo info;
    CHECK(unity_compiler_broker_preprocess_compute_response(
        broker, &preprocess, &response));
    CHECK(unity_compiler_compute_preprocess_response_info(response, &info));
    CHECK(info.transport_complete && !info.native_success_present &&
          info.availability == UNITY_COMPILER_RESPONSE_AVAILABLE &&
          info.has_request_identity &&
          info.valid_apis_authority.status == UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED &&
          memcmp(info.request_digest, preprocess_digest, sizeof(preprocess_digest)) == 0);
    const UnityCompilerComputePreprocessResult* result =
        unity_compiler_compute_preprocess_response_result(response);
    CHECK(result && result->kernel_count == 1U &&
          strcmp(result->kernels[0].name, "ComputeMain") == 0 &&
          result->dependency_count == 0U &&
          strcmp(result->source, "// controlled preprocessing output\n") == 0);
    unity_compiler_compute_preprocess_response_free(response);
    response = NULL;
    unity_compiler_broker_get_stats(broker, &stats);
    const uint64_t source_size = (uint64_t)strlen(source);
    CHECK(stats.submitted_requests == 2U && stats.executed_requests == 2U &&
          stats.compile_requests == 1U && stats.preprocess_requests == 1U &&
          stats.compiler_process_starts == 1U && stats.compiler_process_running &&
          stats.source_digest_scans == 2U && stats.unique_source_submissions == 1U &&
          stats.tracked_unique_source_count == 1U &&
          stats.tracked_source_window_bytes == source_size);

    /* The native terminal rejection is still a completed source submission,
     * even when it has no payload.  Local authority rejection below is not. */
    compile.source = "broker-native-rejected";
    CHECK(unity_compiler_broker_compile_compute_response(broker, &compile, &binary));
    CHECK(binary.status.availability == UNITY_COMPILER_RESPONSE_AVAILABLE &&
          !binary.status.compiler_success && binary.data == NULL && binary.size == 0U &&
          binary.status.valid_apis_authority.status == UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED);
    unity_compiler_binary_response_free(&binary);
    const uint64_t submitted_bytes = source_size + (uint64_t)strlen(compile.source);
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 1U && stats.compiler_process_running &&
          stats.source_digest_scans == 3U && stats.unique_source_submissions == 2U &&
          stats.tracked_unique_source_count == 2U &&
          stats.tracked_source_window_bytes == submitted_bytes);

    /* A typed local miss never submits source to an already live process. */
    CHECK(setenv("DXBC_USC_CACHE_ONLY", "1", 1) == 0);
    compile.source = "broker-cache-only";
    preprocess.source = compile.source;
    CHECK(unity_compiler_broker_compile_compute_response(broker, &compile, &binary));
    CHECK(binary.status.availability == UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS &&
          !binary.status.compiler_success && !binary.data && binary.size == 0U &&
          binary.has_request_identity);
    unity_compiler_binary_response_free(&binary);
    CHECK(unity_compiler_broker_preprocess_compute_response(
        broker, &preprocess, &response));
    CHECK(unity_compiler_compute_preprocess_response_info(response, &info) &&
          info.availability == UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS &&
          !info.transport_complete && !info.native_success_present &&
          info.has_request_identity &&
          !unity_compiler_compute_preprocess_response_result(response));
    unity_compiler_compute_preprocess_response_free(response);
    response = NULL;
    CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);

    /* Mismatch has its own typed authority disposition.  The fake peer will
     * retire if either tripwire source reaches it despite the local guard. */
    CHECK(unity_compiler_broker_set_expected_valid_apis(
        broker, BROKER_FAKE_VALID_APIS ^ (UINT32_C(1) << 4U)));
    compile.source = "broker-authority-rejected";
    preprocess.source = compile.source;
    CHECK(unity_compiler_broker_compile_compute_response(broker, &compile, &binary));
    CHECK(binary.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH &&
          !unity_compiler_response_status_is_clean_success(&binary.status) &&
          !binary.data && binary.size == 0U);
    unity_compiler_binary_response_free(&binary);
    CHECK(unity_compiler_broker_preprocess_compute_response(
        broker, &preprocess, &response));
    CHECK(unity_compiler_compute_preprocess_response_info(response, &info) &&
          info.valid_apis_authority.status == UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH &&
          !info.transport_complete && !unity_compiler_compute_preprocess_response_result(response));
    unity_compiler_compute_preprocess_response_free(response);
    response = NULL;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.compiler_process_starts == 1U && stats.compiler_process_running &&
          stats.source_digest_scans == 3U && stats.unique_source_submissions == 2U &&
          stats.tracked_unique_source_count == 2U &&
          stats.tracked_source_window_bytes == submitted_bytes);
    CHECK(unity_compiler_broker_set_expected_valid_apis(broker, BROKER_FAKE_VALID_APIS));

    /* A late returned dependency cannot be authorized by an earlier source
     * lease.  Its native process and complete residency window must retire. */
    preprocess.source = "broker-uncaptured";
    CHECK(unity_compiler_broker_preprocess_compute_response(
        broker, &preprocess, &response));
    CHECK(unity_compiler_compute_preprocess_response_info(response, &info));
    const uint8_t zero_digest[UNITY_COMPILER_FINGERPRINT_SIZE] = {0};
    CHECK(info.transport_complete &&
          info.availability == UNITY_COMPILER_RESPONSE_INCLUDE_AUTHORITY_UNAVAILABLE &&
          !info.has_request_identity &&
          memcmp(info.request_digest, zero_digest, sizeof(zero_digest)) == 0 &&
          memcmp(info.controls_digest, zero_digest, sizeof(zero_digest)) == 0 &&
          !unity_compiler_compute_preprocess_response_result(response));
    unity_compiler_compute_preprocess_response_free(response);
    response = NULL;
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(!stats.compiler_process_running && stats.compiler_process_starts == 1U &&
          stats.tracked_source_window_bytes == 0U && stats.tracked_unique_source_count == 0U &&
          stats.source_digest_scans == 3U && stats.unique_source_submissions == 2U &&
          stats.source_budget_recycles == 0U);
    UnityCompilerSessionCapabilities capabilities;
    CHECK(!unity_compiler_broker_session_capabilities_snapshot(broker, &capabilities));

    preprocess.source = source;
    CHECK(unity_compiler_broker_preprocess_compute_response(
        broker, &preprocess, &response));
    CHECK(unity_compiler_compute_preprocess_response_info(response, &info) &&
          info.transport_complete && info.has_request_identity &&
          info.availability == UNITY_COMPILER_RESPONSE_AVAILABLE &&
          info.valid_apis_authority.status == UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED);
    unity_compiler_compute_preprocess_response_free(response);
    unity_compiler_broker_get_stats(broker, &stats);
    CHECK(stats.submitted_requests == 9U && stats.executed_requests == 9U &&
          stats.compile_requests == 4U && stats.preprocess_requests == 5U &&
          stats.compiler_process_starts == 2U && stats.compiler_process_running &&
          stats.source_digest_scans == 4U && stats.unique_source_submissions == 3U &&
          stats.tracked_unique_source_count == 1U &&
          stats.tracked_source_window_bytes == source_size);
    unity_compiler_broker_destroy(broker);

    for (size_t index = 0U; index < sizeof(environment) / sizeof(environment[0]);
         ++index) {
        CHECK(environment[index].previous
            ? setenv(environment[index].name, environment[index].previous, 1) == 0
            : unsetenv(environment[index].name) == 0);
        free(environment[index].previous);
    }
    CHECK(unlink(glslang) == 0);
    CHECK(unlink(dxcompiler) == 0);
    CHECK(rmdir(includes) == 0);
    CHECK(rmdir(root) == 0);
    return true;
}

static bool run_all_tests(const char* executable_path) {
    CHECK(verify_single_flight());
    CHECK(verify_broker_stays_lazy());
    CHECK(verify_contract_compile_requires_canonical_identity());
    CHECK(verify_source_budget_exact_mutation_and_lifecycle());
    CHECK(verify_source_budget_recycle_policy());
    CHECK(verify_oversized_source_live_process_lifecycle(executable_path));
    CHECK(verify_compute_broker_identity_and_residency(executable_path));
    return true;
}

int main(int argc, char** argv) {
    if (argc == 5 && getenv("DXBC_USC_BROKER_FAKE_COMPUTE")) {
        return run_broker_fake_compute(argv[1], argv[3]);
    }
    return argc > 0 && run_all_tests(argv[0]) ? 0 : 1;
}
