// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPILER_CACHE_H
#define UNITY_COMPILER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USC_CACHE_DIGEST_SIZE 32

typedef struct UscCacheToolchainLease UscCacheToolchainLease;

typedef struct {
    const char* compiler_path;
    const char* project_root;
    const char* includes_dir;
    const char* sandbox_includes_dir;
    const char* builtin_includes_dir;
    const char* proxy_path;
    const char* glslang_path;
    const char* dxcompiler_path;
    const char* unity_contents_path;
    const char* playback_engines_path;
} UscCacheToolchainPaths;

/*
 * This structure is deliberately a description of the complete
 * compileSnippet wire request.  The client writes these fields and the cache
 * hashes the same fields, so changing a protocol flag cannot silently reuse
 * an entry produced by a different request.
 */
typedef struct {
    const char* command;
    const char* toolchain_configuration;
    const char* snippet_source;
    const char* source_directory;
    const char* source_basename;
    const char* pass_name;
    bool caching_preprocessor;
    bool preprocess_only;
    bool strip_line_directives;
    uint32_t build_platform;
    int32_t render_state_length;
    char** variant_keywords;
    int variant_keyword_count;
    char** user_keywords;
    int user_keyword_count;
    char** disabled_keywords;
    int disabled_keyword_count;
    uint32_t compiler_flags;
    int32_t language;
    int32_t shader_type;
    int32_t platform;
    uint64_t requirements;
    int32_t program_mask;
    int32_t program_start;
    const struct SnippetCompileContract* snippet_contract;
    const uint8_t* environment_fingerprint;
} UnityCompilerCompileRequest;

/* Complete preprocess wire request plus the compiler initialization state
 * that controls include resolution. */
typedef struct {
    const char* command;
    const char* source;
    const char* file_path;
    const char* shader_name;
    bool surface_only;
    bool caching_preprocessor;
    uint32_t build_platform;
    uint32_t valid_apis;
    char** keywords;
    int keyword_count;
    char** defines;
    int define_count;
    char** include_paths;
    int include_path_count;
    const char* toolchain_configuration;
    const uint8_t* environment_fingerprint;
} UnityCompilerPreprocessRequest;

typedef enum {
    USC_CACHE_MISS = 0,
    USC_CACHE_HIT = 1,
} UnityCompilerCacheLookup;

/* Portable SHA-256 of one stable snapshot of a regular file's complete byte
 * contents.  Symlinks are followed to their compiler-visible target; broken
 * links, non-regular targets, and concurrent mutation fail closed. */
bool usc_cache_sha256_file(
    const char* path, uint8_t digest[USC_CACHE_DIGEST_SIZE]);

/*
 * Hashes the compiler content fingerprint followed by a canonical,
 * length-delimited encoding of every compileSnippet request field.
 */
void usc_cache_request_digest(
    const UnityCompilerCompileRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t digest[USC_CACHE_DIGEST_SIZE]);

/*
 * Canonical byte transcript hashed by usc_cache_request_digest.  This is the
 * portable authority record stored in oracle packs; the caller owns *out_data.
 */
bool usc_cache_serialize_compile_request(
    const UnityCompilerCompileRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t** out_data,
    size_t* out_size);

void usc_cache_preprocess_request_digest(
    const UnityCompilerPreprocessRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t digest[USC_CACHE_DIGEST_SIZE]);

/* Canonical byte transcript hashed by usc_cache_preprocess_request_digest. */
bool usc_cache_serialize_preprocess_request(
    const UnityCompilerPreprocessRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t** out_data,
    size_t* out_size);

/*
 * Fingerprints all include and toolchain inputs visible to this client
 * configuration.  Every regular file is hashed regardless of extension: the
 * shader preprocessor can include arbitrary names.  Symlink spelling and the
 * complete compiler-visible target tree both contribute.  Broken links,
 * directory cycles, unsupported special nodes, and detected concurrent
 * mutation fail closed rather than produce a partial digest.
 */
bool usc_cache_environment_fingerprint(
    const char* project_root,
    const char* includes_dir,
    const char* sandbox_includes_dir,
    const char* builtin_includes_dir,
    const char* proxy_path,
    const char* glslang_path,
    const char* dxcompiler_path,
    const char* unity_contents_path,
    const char* playback_engines_path,
    uint8_t digest[USC_CACHE_DIGEST_SIZE]);

/* Captures the compiler/environment content digests and an in-process
 * immutable-environment lease over every observed path.  Lease validation is
 * metadata-only (no file contents are reread) and detects replacement,
 * content metadata changes, symlink retargeting, and directory membership
 * changes.  Validate immediately before a cache lookup/compiler transaction
 * and again before accepting or publishing its result.  The caller owns the
 * returned lease and must destroy it. */
bool usc_cache_toolchain_lease_create(
    const UscCacheToolchainPaths* paths,
    uint8_t compiler_digest[USC_CACHE_DIGEST_SIZE],
    uint8_t environment_digest[USC_CACHE_DIGEST_SIZE],
    UscCacheToolchainLease** out_lease);

bool usc_cache_toolchain_lease_validate(
    const UscCacheToolchainLease* lease);

void usc_cache_toolchain_lease_destroy(UscCacheToolchainLease* lease);

struct PreprocessResult;
struct UnityCompilerPreprocessResponse;
struct UnityCompilerBinaryResponse;

/* Stable, bounds-checked encoding used as the preprocess cache payload. */
bool usc_cache_serialize_preprocess_result(
    const struct PreprocessResult* result,
    uint8_t** out_data,
    size_t* out_size);

bool usc_cache_deserialize_preprocess_result(
    const uint8_t* data,
    size_t size,
    struct PreprocessResult* out_result);

/* Diagnostic-preserving cache payloads used by the live client.  These wrap
 * binary/preprocess artifacts with the terminal status and every ordered
 * `err:` record, so replay cannot turn a diagnosed response into a clean one. */
bool usc_cache_serialize_binary_response(
    const struct UnityCompilerBinaryResponse* response,
    uint8_t** out_data,
    size_t* out_size);

bool usc_cache_deserialize_binary_response(
    const uint8_t* data,
    size_t size,
    struct UnityCompilerBinaryResponse* out_response);

bool usc_cache_serialize_preprocess_response(
    const struct UnityCompilerPreprocessResponse* response,
    uint8_t** out_data,
    size_t* out_size);

bool usc_cache_deserialize_preprocess_response(
    const uint8_t* data,
    size_t size,
    struct UnityCompilerPreprocessResponse* out_response);

/* Load/store one successful result.  Store atomically creates a complete entry
 * when absent, accepts an identical valid entry, rejects a different valid
 * payload for the same request digest, and repairs corrupt entries under a
 * cross-process shard lock.  Load validates the entry header, request digest,
 * length and payload digest.  A zero-length success is returned as a non-NULL
 * allocation. */
UnityCompilerCacheLookup usc_cache_load(
    const char* cache_dir,
    const uint8_t request_digest[USC_CACHE_DIGEST_SIZE],
    uint8_t** out_data,
    size_t* out_size);

bool usc_cache_store(
    const char* cache_dir,
    const uint8_t request_digest[USC_CACHE_DIGEST_SIZE],
    const uint8_t* data,
    size_t size);

#endif
