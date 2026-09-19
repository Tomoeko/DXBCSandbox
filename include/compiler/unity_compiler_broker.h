// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPILER_BROKER_H
#define UNITY_COMPILER_BROKER_H

#include "compiler/unity_compiler_client.h"

/*
 * Thread-safe owner of one lazy UnityShaderCompiler channel.
 *
 * CPU-side parsing and comparison remain caller-owned and parallel.  Only a
 * complete compiler protocol transaction is serialized, because Unity's
 * socket protocol permits exactly one request/response exchange at a time.
 */
typedef struct UnityCompilerBroker UnityCompilerBroker;

typedef struct {
    uint64_t submitted_requests;
    /* Client executions after single-flight coalescing.  A persistent-cache
     * hit is still an execution, but does not contact/start USC. */
    uint64_t executed_requests;
    uint64_t preprocess_requests;
    uint64_t compile_requests;
    uint64_t preprocess_expanded_requests;
    uint64_t disassemble_requests;
    uint64_t coalesced_compile_requests;
    uint64_t compiler_process_starts;
    uint64_t compiler_process_recycles;
    /* Exact, content-deduplicated source accounting for the current compiler
     * residency window.  The window is reset whenever the process changes or
     * is recycled.  A non-running lazy broker may retain a prospective window
     * until the next process start; it is never reported as a live process. */
    uint64_t source_residency_budget_bytes;
    uint64_t tracked_source_window_bytes;
    uint64_t peak_tracked_source_window_bytes;
    uint64_t tracked_unique_source_count;
    uint64_t unique_source_submissions;
    uint64_t source_digest_scans;
    uint64_t source_tracking_failures;
    uint64_t source_budget_recycles;
    bool compiler_process_running;
} UnityCompilerBrokerStats;

UnityCompilerBroker* unity_compiler_broker_create(
    const char* project_root, const char* includes_dir);

/* Configures the broker without starting UnityShaderCompiler. */
UnityCompilerBroker* unity_compiler_broker_create_lazy(
    const char* project_root, const char* includes_dir);

/* Returns whether a completed residency window should be recycled.  The
 * configured budget is inclusive.  A source is atomic, so one source larger
 * than the budget is retained as an exclusive singleton instead of forcing a
 * restart for every variant compiled from the same source. */
bool unity_compiler_broker_source_window_requires_recycle(
    uint64_t budget_bytes, uint64_t tracked_bytes,
    uint64_t tracked_unique_source_count);

/* Sets the exact, unique-source threshold for one compiler residency window.
 * Zero disables source tracking and automatic recycling.  The broker
 * recycles only after a complete protocol transaction has returned.  One
 * individually oversized source is therefore admitted as an exclusive
 * singleton window; repeated variants of that exact source stay on the same
 * process.  A second distinct source which takes the aggregate window above
 * the inclusive budget causes a recycle after its completed transaction.
 * Changing the value recycles a live process before starting a fresh
 * accounting window. */
bool unity_compiler_broker_set_source_residency_budget(
    UnityCompilerBroker* broker, uint64_t budget_bytes);

/* Waits for the current transaction and releases only the live compiler
 * process. Configuration and persistent-cache identity remain intact.  A
 * manual recycle also starts a fresh source accounting window. */
bool unity_compiler_broker_recycle_process(UnityCompilerBroker* broker);

/* All callers must have returned before destroy. */
void unity_compiler_broker_destroy(UnityCompilerBroker* broker);

bool unity_compiler_broker_preprocess(
    UnityCompilerBroker* broker, const char* source, const char* shader_name,
    PreprocessResult* out_result);

bool unity_compiler_broker_preprocess_contract(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    PreprocessResult* out_result);

bool unity_compiler_broker_preprocess_contract_response(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    UnityCompilerPreprocessResponse* out_response);

bool unity_compiler_broker_serialize_preprocess_request(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

/* Offline authority serialization retains the broker's configured path
 * spellings but never acquires a local toolchain lease or starts USC. */
bool unity_compiler_broker_serialize_preprocess_request_with_authority(
    UnityCompilerBroker* broker,
    const UnityCompilerShaderPreprocessRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

bool unity_compiler_broker_serialize_preprocess_result(
    UnityCompilerBroker* broker, const PreprocessResult* result,
    uint8_t** out_data, size_t* out_size);

bool unity_compiler_broker_deserialize_preprocess_result(
    UnityCompilerBroker* broker, const uint8_t* data, size_t size,
    PreprocessResult* out_result);

/* Exact contract compiles are coalesced only by the canonical serialized
 * request digest, including compiler and environment fingerprints. */
uint8_t* unity_compiler_broker_compile_contract(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    size_t* out_size, char** out_error);

bool unity_compiler_broker_compile_contract_response(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* out_response);

bool unity_compiler_broker_get_toolchain_provenance(
    UnityCompilerBroker* broker,
    UnityCompilerToolchainProvenance* out_provenance);

/* Serialized access to the typed initializeCompiler authority.  Snapshot is
 * process-free; capture may initialize the broker's one shared compiler once
 * and is disabled by cache-only mode when no record has been captured. */
bool unity_compiler_broker_session_capabilities_snapshot(
    UnityCompilerBroker* broker,
    UnityCompilerSessionCapabilities* out_capabilities);
bool unity_compiler_broker_capture_session_capabilities(
    UnityCompilerBroker* broker,
    UnityCompilerSessionCapabilities* out_capabilities);

/* Serialized, process-free configuration of the validApis value which every
 * later live work command must match against initializeCompiler. */
bool unity_compiler_broker_set_expected_valid_apis(
    UnityCompilerBroker* broker, uint32_t expected_valid_apis);
bool unity_compiler_broker_clear_expected_valid_apis(
    UnityCompilerBroker* broker);
bool unity_compiler_broker_expected_valid_apis_authority(
    UnityCompilerBroker* broker,
    UnityCompilerValidApisAuthority* out_authority);

bool unity_compiler_broker_serialize_compile_request(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

bool unity_compiler_broker_serialize_compile_request_with_authority(
    UnityCompilerBroker* broker,
    const UnityCompilerSnippetCompileRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

/* Compatibility surface without a complete preprocess contract.  Calls are
 * serialized through the broker but deliberately not single-flight coalesced,
 * because this legacy request cannot form the canonical exact identity above. */
uint8_t* unity_compiler_broker_compile(
    UnityCompilerBroker* broker, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count,
    size_t* out_size, char** out_error);

/* Same legacy authority as compile(), retaining rejection/cache availability
 * and lossless diagnostics instead of collapsing all failures into NULL. */
bool unity_compiler_broker_compile_response(
    UnityCompilerBroker* broker, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count,
    UnityCompilerBinaryResponse* out_response);

char* unity_compiler_broker_preprocess_expanded(
    UnityCompilerBroker* broker, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count);

char* unity_compiler_broker_disassemble(
    UnityCompilerBroker* broker, const char* shader_name, int platform,
    int stage, const uint8_t* bytecode, size_t size);

bool unity_compiler_broker_disassemble_response(
    UnityCompilerBroker* broker, const char* shader_name, int platform,
    int stage, const uint8_t* bytecode, size_t size,
    UnityCompilerTextResponse* out_response);

void unity_compiler_broker_get_stats(
    UnityCompilerBroker* broker, UnityCompilerBrokerStats* out_stats);

#endif /* UNITY_COMPILER_BROKER_H */
