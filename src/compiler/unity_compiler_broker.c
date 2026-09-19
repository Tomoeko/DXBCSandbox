// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compiler_broker.h"

#include "common/sha256.h"
#include "compiler/unity_compiler_singleflight.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    uint64_t size;
    uint64_t epoch;
} BrokerSourceRecord;

struct UnityCompilerBroker {
    UnityCompilerChannel channel;
    pthread_mutex_t protocol_mutex;
    UnityCompilerSingleFlight* compile_single_flight;
    atomic_uint_fast64_t submitted_requests;
    atomic_uint_fast64_t executed_requests;
    atomic_uint_fast64_t preprocess_requests;
    atomic_uint_fast64_t compile_requests;
    atomic_uint_fast64_t preprocess_expanded_requests;
    atomic_uint_fast64_t disassemble_requests;
    atomic_uint_fast64_t coalesced_compile_requests;
    atomic_uint_fast64_t compiler_process_starts;
    atomic_uint_fast64_t compiler_process_recycles;
    uint64_t source_residency_budget_bytes;
    uint64_t tracked_source_window_bytes;
    uint64_t peak_tracked_source_window_bytes;
    uint64_t unique_source_submissions;
    uint64_t source_digest_scans;
    uint64_t source_tracking_failures;
    uint64_t source_budget_recycles;
    BrokerSourceRecord* source_records;
    size_t source_record_count;
    size_t source_record_capacity;
    uint64_t source_record_epoch;
};

#define BROKER_MAX_KEYWORD_ITEMS (1024 * 1024)

static bool valid_array_shape(char** values, int count) {
    return count >= 0 && count <= BROKER_MAX_KEYWORD_ITEMS &&
           (count == 0 || values != NULL);
}

static bool canonical_contract_compile_key(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE]) {
    _Static_assert(USC_SINGLE_FLIGHT_KEY_SIZE ==
                       UNITY_COMPILER_FINGERPRINT_SIZE,
                   "single-flight and request digests must have equal size");
    uint8_t* transcript = NULL;
    size_t transcript_size = 0;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool ok = unity_compiler_serialize_compile_request(
        &broker->channel, request, &transcript, &transcript_size, key);
    pthread_mutex_unlock(&broker->protocol_mutex);
    free(transcript);
    return ok;
}

static void source_window_clear_locked(UnityCompilerBroker* broker) {
    broker->tracked_source_window_bytes = 0U;
    broker->source_record_count = 0U;
    broker->source_record_epoch++;
    if (broker->source_record_epoch == 0U) {
        if (broker->source_records) {
            memset(broker->source_records, 0,
                   broker->source_record_capacity *
                       sizeof(*broker->source_records));
        }
        broker->source_record_epoch = 1U;
    }
}

static size_t source_record_slot(
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE], uint64_t size,
    size_t capacity) {
    uint64_t hash = 0U;
    memcpy(&hash, digest, sizeof(hash));
    hash ^= size + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6U) +
            (hash >> 2U);
    hash ^= hash >> 30U;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27U;
    hash *= UINT64_C(0x94d049bb133111eb);
    hash ^= hash >> 31U;
    return (size_t)hash & (capacity - 1U);
}

static void source_record_insert_unchecked(
    BrokerSourceRecord* records, size_t capacity, uint64_t epoch,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE], uint64_t size) {
    size_t slot = source_record_slot(digest, size, capacity);
    while (records[slot].epoch == epoch) {
        slot = (slot + 1U) & (capacity - 1U);
    }
    memcpy(records[slot].digest, digest, COMMON_SHA256_DIGEST_SIZE);
    records[slot].size = size;
    records[slot].epoch = epoch;
}

static bool source_record_grow_locked(UnityCompilerBroker* broker) {
    const size_t capacity = broker->source_record_capacity
        ? broker->source_record_capacity * 2U : 64U;
    if (capacity < broker->source_record_capacity ||
        capacity > SIZE_MAX / sizeof(*broker->source_records)) {
        return false;
    }
    BrokerSourceRecord* records = (BrokerSourceRecord*)calloc(
        capacity, sizeof(*records));
    if (!records) return false;
    for (size_t i = 0U; i < broker->source_record_capacity; ++i) {
        const BrokerSourceRecord* old = &broker->source_records[i];
        if (old->epoch == broker->source_record_epoch) {
            source_record_insert_unchecked(
                records, capacity, broker->source_record_epoch,
                old->digest, old->size);
        }
    }
    free(broker->source_records);
    broker->source_records = records;
    broker->source_record_capacity = capacity;
    return true;
}

static bool recycle_process_locked(UnityCompilerBroker* broker,
                                   bool source_budget_recycle) {
    const bool had_process = broker->channel.process_id > 0 ||
                             broker->channel.socket_fd >= 0;
    const bool recycled = unity_compiler_recycle_process(&broker->channel);
    if (!recycled) return false;
    source_window_clear_locked(broker);
    if (had_process) {
        atomic_fetch_add_explicit(&broker->compiler_process_recycles, 1,
                                  memory_order_relaxed);
        if (source_budget_recycle) broker->source_budget_recycles++;
    }
    return true;
}

static bool source_window_record_locked(UnityCompilerBroker* broker,
                                        const char* source) {
    if (broker->source_residency_budget_bytes == 0U || !source) return true;
    const size_t source_size = strlen(source);
#if SIZE_MAX > UINT64_MAX
    if (source_size > UINT64_MAX) return false;
#endif
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source, source_size, digest);
    broker->source_digest_scans++;
    if (broker->source_record_capacity > 0U) {
        size_t slot = source_record_slot(
            digest, (uint64_t)source_size,
            broker->source_record_capacity);
        for (;;) {
            const BrokerSourceRecord* record = &broker->source_records[slot];
            if (record->epoch != broker->source_record_epoch) break;
            if (record->size == (uint64_t)source_size &&
                memcmp(record->digest, digest, sizeof(digest)) == 0) {
                return true;
            }
            slot = (slot + 1U) &
                   (broker->source_record_capacity - 1U);
        }
    }
    if (broker->source_record_capacity == 0U ||
        broker->source_record_count + 1U >
            broker->source_record_capacity -
                broker->source_record_capacity / 4U) {
        if (!source_record_grow_locked(broker)) return false;
    }
    if ((uint64_t)source_size >
        UINT64_MAX - broker->tracked_source_window_bytes) {
        return false;
    }
    source_record_insert_unchecked(
        broker->source_records, broker->source_record_capacity,
        broker->source_record_epoch, digest, (uint64_t)source_size);
    broker->source_record_count++;
    broker->tracked_source_window_bytes += (uint64_t)source_size;
    if (broker->tracked_source_window_bytes >
        broker->peak_tracked_source_window_bytes) {
        broker->peak_tracked_source_window_bytes =
            broker->tracked_source_window_bytes;
    }
    broker->unique_source_submissions++;
    return true;
}

bool unity_compiler_broker_source_window_requires_recycle(
    uint64_t budget_bytes, uint64_t tracked_bytes,
    uint64_t tracked_unique_source_count) {
    if (budget_bytes == 0U || tracked_unique_source_count <= 1U) {
        return false;
    }
    return tracked_bytes > budget_bytes;
}

static void finish_protocol_transaction_locked(
    UnityCompilerBroker* broker, pid_t process_before,
    const char* submitted_source) {
    const pid_t process_after = broker->channel.process_id;
    const bool process_changed =
        (process_before <= 0 && process_after > 0) ||
        (process_before > 0 && process_after <= 0) ||
        (process_before > 0 && process_after > 0 &&
         process_before != process_after);
    if (process_changed) source_window_clear_locked(broker);
    if (process_before <= 0 && broker->channel.process_id > 0) {
        atomic_fetch_add_explicit(&broker->compiler_process_starts, 1,
                                  memory_order_relaxed);
    }
    if (!source_window_record_locked(broker, submitted_source)) {
        broker->source_tracking_failures++;
        /* A tracking allocation failure must not leave an unbounded live
         * process.  The completed result remains authoritative; only the
         * process residency is discarded. */
        if (process_after > 0 && !recycle_process_locked(broker, true)) {
            return;
        }
        source_window_clear_locked(broker);
        return;
    }
    if (process_after > 0 &&
        unity_compiler_broker_source_window_requires_recycle(
            broker->source_residency_budget_bytes,
            broker->tracked_source_window_bytes,
            (uint64_t)broker->source_record_count)) {
        if (!recycle_process_locked(broker, true)) {
            broker->source_tracking_failures++;
        }
    }
}

static UnityCompilerBroker* unity_compiler_broker_create_internal(
    const char* project_root, const char* includes_dir, bool lazy_start) {
    UnityCompilerBroker* broker =
        (UnityCompilerBroker*)calloc(1, sizeof(*broker));
    if (!broker) return NULL;
    broker->channel.socket_fd = -1;
    broker->source_record_epoch = 1U;
    if (pthread_mutex_init(&broker->protocol_mutex, NULL) != 0) {
        free(broker);
        return NULL;
    }
    broker->compile_single_flight = usc_single_flight_create();
    if (!broker->compile_single_flight) {
        pthread_mutex_destroy(&broker->protocol_mutex);
        free(broker);
        return NULL;
    }
    bool started = lazy_start
        ? unity_compiler_start_lazy(
              &broker->channel, project_root, includes_dir)
        : unity_compiler_start(
              &broker->channel, project_root, includes_dir);
    if (!started) {
        usc_single_flight_destroy(broker->compile_single_flight);
        pthread_mutex_destroy(&broker->protocol_mutex);
        free(broker);
        return NULL;
    }
    if (broker->channel.process_id > 0) {
        atomic_store_explicit(&broker->compiler_process_starts, 1,
                              memory_order_relaxed);
    }
    return broker;
}

UnityCompilerBroker* unity_compiler_broker_create(
    const char* project_root, const char* includes_dir) {
    return unity_compiler_broker_create_internal(
        project_root, includes_dir, false);
}

UnityCompilerBroker* unity_compiler_broker_create_lazy(
    const char* project_root, const char* includes_dir) {
    return unity_compiler_broker_create_internal(
        project_root, includes_dir, true);
}

bool unity_compiler_broker_set_source_residency_budget(
    UnityCompilerBroker* broker, uint64_t budget_bytes) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    if (broker->source_residency_budget_bytes != budget_bytes) {
        const bool has_process = broker->channel.process_id > 0 ||
                                 broker->channel.socket_fd >= 0;
        if (has_process && !recycle_process_locked(broker, false)) {
            pthread_mutex_unlock(&broker->protocol_mutex);
            return false;
        }
        if (!has_process) source_window_clear_locked(broker);
        broker->source_residency_budget_bytes = budget_bytes;
    }
    pthread_mutex_unlock(&broker->protocol_mutex);
    return true;
}

bool unity_compiler_broker_recycle_process(UnityCompilerBroker* broker) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    const bool recycled = recycle_process_locked(broker, false);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return recycled;
}

void unity_compiler_broker_destroy(UnityCompilerBroker* broker) {
    if (!broker) return;
    usc_single_flight_destroy(broker->compile_single_flight);
    pthread_mutex_lock(&broker->protocol_mutex);
    unity_compiler_shutdown(&broker->channel);
    pthread_mutex_unlock(&broker->protocol_mutex);
    pthread_mutex_destroy(&broker->protocol_mutex);
    free(broker->source_records);
    free(broker);
}

bool unity_compiler_broker_preprocess(
    UnityCompilerBroker* broker, const char* source, const char* shader_name,
    PreprocessResult* out_result) {
    if (!broker) return false;
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->preprocess_requests, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    const bool result = unity_compiler_preprocess(
        &broker->channel, source, shader_name, out_result);
    finish_protocol_transaction_locked(broker, process_before, source);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_preprocess_contract(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    PreprocessResult* out_result) {
    if (!broker || !request || !out_result) return false;
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->preprocess_requests, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    const bool result = unity_compiler_preprocess_contract(
        &broker->channel, request, out_result);
    finish_protocol_transaction_locked(
        broker, process_before, request->source);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_preprocess_contract_response(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    UnityCompilerPreprocessResponse* out_response) {
    if (!out_response) return false;
    unity_compiler_preprocess_response_init(out_response);
    if (!broker || !request) return false;
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->preprocess_requests, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    const bool result = unity_compiler_preprocess_contract_response(
        &broker->channel, request, out_response);
    finish_protocol_transaction_locked(
        broker, process_before,
        out_response->status.from_cache ? NULL : request->source);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_serialize_preprocess_request(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool result = unity_compiler_serialize_preprocess_request(
        &broker->channel, request, out_transcript, out_transcript_size,
        out_request_digest);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_serialize_preprocess_request_with_authority(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool result =
        unity_compiler_serialize_preprocess_request_with_authority(
            &broker->channel, request, authority, out_transcript,
            out_transcript_size, out_request_digest);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_serialize_preprocess_result(
    UnityCompilerBroker* broker, const PreprocessResult* result,
    uint8_t** out_data, size_t* out_size) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool ok = unity_compiler_serialize_preprocess_result(
        result, out_data, out_size);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return ok;
}

bool unity_compiler_broker_deserialize_preprocess_result(
    UnityCompilerBroker* broker, const uint8_t* data, size_t size,
    PreprocessResult* out_result) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool ok = unity_compiler_deserialize_preprocess_result(
        data, size, out_result);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return ok;
}

typedef struct {
    UnityCompilerBroker* broker;
    const UnityCompilerSnippetCompileRequest* request;
} ContractCompileContext;

static uint8_t* execute_contract_compile(void* opaque, size_t* out_size,
                                         char** out_error) {
    ContractCompileContext* context = (ContractCompileContext*)opaque;
    UnityCompilerBroker* broker = context->broker;
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    uint8_t* result = unity_compiler_compile_contract(
        &broker->channel, context->request, out_size, out_error);
    finish_protocol_transaction_locked(
        broker, process_before, context->request->snippet_source);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

uint8_t* unity_compiler_broker_compile_contract(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    size_t* out_size, char** out_error) {
    if (out_size) *out_size = 0;
    if (out_error) *out_error = NULL;
    if (!broker || !out_size || !request || !request->contract ||
        !unity_compiler_snippet_contract_validate(request->contract) ||
        !valid_array_shape(request->variant_keywords,
                           request->variant_keyword_count) ||
        !valid_array_shape(request->user_keywords,
                           request->user_keyword_count) ||
        !valid_array_shape(request->disabled_keywords,
                           request->disabled_keyword_count)) {
        return NULL;
    }
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->compile_requests, 1,
                              memory_order_relaxed);
    ContractCompileContext context = {broker, request};
    uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE];
    if (!canonical_contract_compile_key(broker, request, key)) {
        if (out_error) {
            *out_error = strdup(
                "Could not form canonical compiler request identity");
        }
        return NULL;
    }
    bool joined = false;
    uint8_t* result = usc_single_flight_execute(
        broker->compile_single_flight, key, execute_contract_compile,
        &context, out_size, out_error, &joined);
    if (joined) {
        atomic_fetch_add_explicit(&broker->coalesced_compile_requests, 1,
                                  memory_order_relaxed);
    }
    return result;
}

bool unity_compiler_broker_compile_contract_response(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* out_response) {
    if (!out_response) return false;
    unity_compiler_binary_response_init(out_response);
    if (!broker || !request || !request->contract ||
        !unity_compiler_snippet_contract_validate(request->contract) ||
        !valid_array_shape(request->variant_keywords,
                           request->variant_keyword_count) ||
        !valid_array_shape(request->user_keywords,
                           request->user_keyword_count) ||
        !valid_array_shape(request->disabled_keywords,
                           request->disabled_keyword_count)) {
        return false;
    }
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->compile_requests, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    const bool result = unity_compiler_compile_contract_response(
        &broker->channel, request, out_response);
    finish_protocol_transaction_locked(
        broker, process_before,
        out_response->status.from_cache ? NULL : request->snippet_source);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_get_toolchain_provenance(
    UnityCompilerBroker* broker,
    UnityCompilerToolchainProvenance* out_provenance) {
    if (!broker || !out_provenance) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool result = unity_compiler_get_toolchain_provenance(
        &broker->channel, out_provenance);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_session_capabilities_snapshot(
    UnityCompilerBroker* broker,
    UnityCompilerSessionCapabilities* out_capabilities) {
    if (!broker || !out_capabilities) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool result = unity_compiler_session_capabilities_snapshot(
        &broker->channel, out_capabilities);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_capture_session_capabilities(
    UnityCompilerBroker* broker,
    UnityCompilerSessionCapabilities* out_capabilities) {
    if (!broker || !out_capabilities) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    const pid_t process_before = broker->channel.process_id;
    bool result = unity_compiler_capture_session_capabilities(
        &broker->channel, out_capabilities);
    finish_protocol_transaction_locked(broker, process_before, NULL);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_set_expected_valid_apis(
    UnityCompilerBroker* broker, uint32_t expected_valid_apis) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    const bool result = unity_compiler_set_expected_valid_apis(
        &broker->channel, expected_valid_apis);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_clear_expected_valid_apis(
    UnityCompilerBroker* broker) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    const bool result = unity_compiler_clear_expected_valid_apis(
        &broker->channel);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_expected_valid_apis_authority(
    UnityCompilerBroker* broker,
    UnityCompilerValidApisAuthority* out_authority) {
    if (!broker || !out_authority) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    const bool result = unity_compiler_expected_valid_apis_authority(
        &broker->channel, out_authority);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_serialize_compile_request(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool result = unity_compiler_serialize_compile_request(
        &broker->channel, request, out_transcript, out_transcript_size,
        out_request_digest);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_serialize_compile_request_with_authority(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!broker) return false;
    pthread_mutex_lock(&broker->protocol_mutex);
    bool result = unity_compiler_serialize_compile_request_with_authority(
        request, authority, out_transcript, out_transcript_size,
        out_request_digest);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_compile_response(
    UnityCompilerBroker* broker, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count,
    UnityCompilerBinaryResponse* out_response) {
    if (!out_response) return false;
    unity_compiler_binary_response_init(out_response);
    if (!broker || !valid_array_shape(keywords, keyword_count) ||
        !valid_array_shape(defines, define_count)) return false;
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->compile_requests, 1,
                              memory_order_relaxed);
    /* Legacy requests lack the contract needed for canonical coalescing. */
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    bool result = unity_compiler_compile_response(
        &broker->channel, snippet_src, shader_name, shader_type, platform,
        reqs, keywords, keyword_count, defines, define_count, out_response);
    finish_protocol_transaction_locked(broker, process_before, snippet_src);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

uint8_t* unity_compiler_broker_compile(
    UnityCompilerBroker* broker, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count,
    size_t* out_size, char** out_error) {
    if (out_size) *out_size = 0U;
    if (out_error) *out_error = NULL;
    if (!out_size) return NULL;
    UnityCompilerBinaryResponse response;
    if (!unity_compiler_broker_compile_response(
            broker, snippet_src, shader_name, shader_type, platform, reqs,
            keywords, keyword_count, defines, define_count, &response)) {
        if (out_error) {
            *out_error = strdup("Unity compiler transport or protocol failure");
        }
        unity_compiler_binary_response_free(&response);
        return NULL;
    }
    return unity_compiler_binary_response_take_clean_data(
        &response, out_size, out_error);
}

char* unity_compiler_broker_preprocess_expanded(
    UnityCompilerBroker* broker, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count) {
    if (!broker) return NULL;
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->preprocess_expanded_requests, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    char* result = unity_compiler_preprocess_expanded(
        &broker->channel, snippet_src, shader_name, shader_type, platform,
        reqs, keywords, keyword_count, defines, define_count);
    finish_protocol_transaction_locked(broker, process_before, snippet_src);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

char* unity_compiler_broker_disassemble(
    UnityCompilerBroker* broker, const char* shader_name, int platform,
    int stage, const uint8_t* bytecode, size_t size) {
    if (!broker) return NULL;
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->disassemble_requests, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    char* result = unity_compiler_disassemble(
        &broker->channel, shader_name, platform, stage, bytecode, size);
    finish_protocol_transaction_locked(broker, process_before, NULL);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

bool unity_compiler_broker_disassemble_response(
    UnityCompilerBroker* broker, const char* shader_name, int platform,
    int stage, const uint8_t* bytecode, size_t size,
    UnityCompilerTextResponse* out_response) {
    if (!out_response) return false;
    unity_compiler_text_response_init(out_response);
    if (!broker || !shader_name || (size > 0U && !bytecode)) return false;
    atomic_fetch_add_explicit(&broker->submitted_requests, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&broker->disassemble_requests, 1,
                              memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    atomic_fetch_add_explicit(&broker->executed_requests, 1,
                              memory_order_relaxed);
    const pid_t process_before = broker->channel.process_id;
    const bool result = unity_compiler_disassemble_response(
        &broker->channel, shader_name, platform, stage, bytecode, size,
        out_response);
    finish_protocol_transaction_locked(broker, process_before, NULL);
    pthread_mutex_unlock(&broker->protocol_mutex);
    return result;
}

void unity_compiler_broker_get_stats(
    UnityCompilerBroker* broker, UnityCompilerBrokerStats* out_stats) {
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!broker) return;
    out_stats->submitted_requests = atomic_load_explicit(
        &broker->submitted_requests, memory_order_relaxed);
    out_stats->executed_requests = atomic_load_explicit(
        &broker->executed_requests, memory_order_relaxed);
    out_stats->preprocess_requests = atomic_load_explicit(
        &broker->preprocess_requests, memory_order_relaxed);
    out_stats->compile_requests = atomic_load_explicit(
        &broker->compile_requests, memory_order_relaxed);
    out_stats->preprocess_expanded_requests = atomic_load_explicit(
        &broker->preprocess_expanded_requests, memory_order_relaxed);
    out_stats->disassemble_requests = atomic_load_explicit(
        &broker->disassemble_requests, memory_order_relaxed);
    out_stats->coalesced_compile_requests = atomic_load_explicit(
        &broker->coalesced_compile_requests, memory_order_relaxed);
    out_stats->compiler_process_starts = atomic_load_explicit(
        &broker->compiler_process_starts, memory_order_relaxed);
    out_stats->compiler_process_recycles = atomic_load_explicit(
        &broker->compiler_process_recycles, memory_order_relaxed);
    pthread_mutex_lock(&broker->protocol_mutex);
    out_stats->source_residency_budget_bytes =
        broker->source_residency_budget_bytes;
    out_stats->tracked_source_window_bytes =
        broker->tracked_source_window_bytes;
    out_stats->peak_tracked_source_window_bytes =
        broker->peak_tracked_source_window_bytes;
    out_stats->tracked_unique_source_count =
        (uint64_t)broker->source_record_count;
    out_stats->unique_source_submissions =
        broker->unique_source_submissions;
    out_stats->source_digest_scans = broker->source_digest_scans;
    out_stats->source_tracking_failures = broker->source_tracking_failures;
    out_stats->source_budget_recycles = broker->source_budget_recycles;
    out_stats->compiler_process_running = broker->channel.process_id > 0;
    pthread_mutex_unlock(&broker->protocol_mutex);
}
