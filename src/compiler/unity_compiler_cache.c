// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compiler_cache.h"

#include "common/sha256.h"
#include "compiler/unity_compiler_client.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define USC_CACHE_HEADER_SIZE 96U
#define USC_CACHE_FORMAT_VERSION 1U
#define USC_CACHE_SUCCESS 1U
#define USC_CACHE_MAX_OUTPUT_SIZE (UINT64_C(1024) * 1024U * 1024U)
#define USC_PREPROCESS_MAX_COUNT UINT32_C(1048576)

static const uint8_t USC_CACHE_MAGIC[8] = {
    'U', 'S', 'C', 'C', 'A', 'C', 'H', 'E'
};

typedef CommonSha256Context Sha256Context;

_Static_assert(USC_CACHE_DIGEST_SIZE == COMMON_SHA256_DIGEST_SIZE,
               "cache and common SHA-256 digest sizes must match");

#define sha256_init common_sha256_init
#define sha256_update common_sha256_update
#define sha256_final common_sha256_final

static atomic_uint_fast64_t g_cache_hits;
static atomic_uint_fast64_t g_cache_misses;
static atomic_uint_fast64_t g_cache_stores;
static atomic_uint_fast64_t g_cache_corrupt_entries;
static atomic_uint_fast64_t g_cache_write_errors;
static atomic_uint_fast64_t g_temp_sequence;
static pthread_mutex_t g_cache_publish_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t cache_read_le32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t cache_read_le64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        value |= (uint64_t)data[i] << (i * 8U);
    }
    return value;
}

static void write_le32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static void write_le64(uint8_t* data, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) {
        data[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void sha256_bytes(
    const uint8_t* data, size_t size,
    uint8_t digest[USC_CACHE_DIGEST_SIZE]) {
    common_sha256(data, size, digest);
}

static long cache_stat_mtime_nanoseconds(const struct stat* status) {
#if defined(__APPLE__)
    return status->st_mtimespec.tv_nsec;
#else
    return status->st_mtim.tv_nsec;
#endif
}

static long cache_stat_ctime_nanoseconds(const struct stat* status) {
#if defined(__APPLE__)
    return status->st_ctimespec.tv_nsec;
#else
    return status->st_ctim.tv_nsec;
#endif
}

static bool cache_stat_identity_equal(const struct stat* left,
                                      const struct stat* right) {
    return left->st_dev == right->st_dev &&
           left->st_ino == right->st_ino &&
           left->st_mode == right->st_mode &&
           left->st_nlink == right->st_nlink &&
           left->st_uid == right->st_uid &&
           left->st_gid == right->st_gid &&
           left->st_size == right->st_size &&
           left->st_mtime == right->st_mtime &&
           cache_stat_mtime_nanoseconds(left) ==
               cache_stat_mtime_nanoseconds(right) &&
           left->st_ctime == right->st_ctime &&
           cache_stat_ctime_nanoseconds(left) ==
               cache_stat_ctime_nanoseconds(right);
}

static bool cache_stat_path(const char* path, bool follow,
                            struct stat* status) {
    return (follow ? stat(path, status) : lstat(path, status)) == 0;
}

typedef struct {
    dev_t device;
    ino_t inode;
    mode_t mode;
    nlink_t link_count;
    uid_t user;
    gid_t group;
    off_t size;
    time_t mtime_seconds;
    long mtime_nanoseconds;
    time_t ctime_seconds;
    long ctime_nanoseconds;
} CacheLeaseStat;

typedef enum {
    CACHE_LEASE_ENTRY_MISSING,
    CACHE_LEASE_ENTRY_DIRECT,
    CACHE_LEASE_ENTRY_SYMLINK,
} CacheLeaseEntryKind;

typedef struct {
    char* path;
    CacheLeaseEntryKind kind;
    CacheLeaseStat link_status;
    CacheLeaseStat target_status;
    uint8_t* target_name;
    size_t target_name_size;
} CacheLeaseEntry;

struct UscCacheToolchainLease {
    CacheLeaseEntry* entries;
    size_t entry_count;
    size_t entry_capacity;
};

static void cache_lease_stat_capture(CacheLeaseStat* destination,
                                     const struct stat* source) {
    destination->device = source->st_dev;
    destination->inode = source->st_ino;
    destination->mode = source->st_mode;
    destination->link_count = source->st_nlink;
    destination->user = source->st_uid;
    destination->group = source->st_gid;
    destination->size = source->st_size;
    destination->mtime_seconds = source->st_mtime;
    destination->mtime_nanoseconds =
        cache_stat_mtime_nanoseconds(source);
    destination->ctime_seconds = source->st_ctime;
    destination->ctime_nanoseconds =
        cache_stat_ctime_nanoseconds(source);
}

static bool cache_lease_stat_matches(const CacheLeaseStat* expected,
                                     const struct stat* current) {
    CacheLeaseStat captured;
    cache_lease_stat_capture(&captured, current);
    return expected->device == captured.device &&
           expected->inode == captured.inode &&
           expected->mode == captured.mode &&
           expected->link_count == captured.link_count &&
           expected->user == captured.user &&
           expected->group == captured.group &&
           expected->size == captured.size &&
           expected->mtime_seconds == captured.mtime_seconds &&
           expected->mtime_nanoseconds == captured.mtime_nanoseconds &&
           expected->ctime_seconds == captured.ctime_seconds &&
           expected->ctime_nanoseconds == captured.ctime_nanoseconds;
}

static bool cache_lease_reserve(UscCacheToolchainLease* lease) {
    if (!lease) return true;
    if (lease->entry_count < lease->entry_capacity) return true;
    size_t next = lease->entry_capacity ? lease->entry_capacity * 2U : 64U;
    if (next < lease->entry_capacity ||
        next > SIZE_MAX / sizeof(*lease->entries)) {
        return false;
    }
    CacheLeaseEntry* resized = (CacheLeaseEntry*)realloc(
        lease->entries, next * sizeof(*lease->entries));
    if (!resized) return false;
    lease->entries = resized;
    lease->entry_capacity = next;
    return true;
}

static bool cache_lease_record(
    UscCacheToolchainLease* lease,
    const char* path,
    CacheLeaseEntryKind kind,
    const struct stat* link_status,
    const struct stat* target_status,
    const uint8_t* target_name,
    size_t target_name_size) {
    if (!lease) return true;
    if (!path || !cache_lease_reserve(lease)) return false;
    CacheLeaseEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.path = strdup(path);
    if (!entry.path) return false;
    entry.kind = kind;
    if (link_status) cache_lease_stat_capture(
        &entry.link_status, link_status);
    if (target_status) cache_lease_stat_capture(
        &entry.target_status, target_status);
    if (target_name_size > 0U) {
        entry.target_name = (uint8_t*)malloc(target_name_size);
        if (!entry.target_name) {
            free(entry.path);
            return false;
        }
        memcpy(entry.target_name, target_name, target_name_size);
        entry.target_name_size = target_name_size;
    }
    lease->entries[lease->entry_count++] = entry;
    return true;
}

static bool cache_lease_record_missing(UscCacheToolchainLease* lease,
                                       const char* path) {
    return cache_lease_record(lease, path, CACHE_LEASE_ENTRY_MISSING,
                              NULL, NULL, NULL, 0U);
}

static bool cache_lease_record_direct(UscCacheToolchainLease* lease,
                                      const char* path,
                                      const struct stat* status) {
    return cache_lease_record(lease, path, CACHE_LEASE_ENTRY_DIRECT,
                              status, status, NULL, 0U);
}

static bool cache_lease_record_symlink(
    UscCacheToolchainLease* lease,
    const char* path,
    const struct stat* link_status,
    const struct stat* target_status,
    const uint8_t* target_name,
    size_t target_name_size) {
    return cache_lease_record(lease, path, CACHE_LEASE_ENTRY_SYMLINK,
                              link_status, target_status,
                              target_name, target_name_size);
}

void usc_cache_toolchain_lease_destroy(UscCacheToolchainLease* lease) {
    if (!lease) return;
    for (size_t i = 0; i < lease->entry_count; i++) {
        free(lease->entries[i].path);
        free(lease->entries[i].target_name);
    }
    free(lease->entries);
    free(lease);
}

static bool read_symlink_snapshot(const char* path,
                                  const struct stat* expected,
                                  uint8_t** out_data,
                                  size_t* out_size);

/* Hash a stable snapshot of one regular file.  The pre-open identity prevents
 * path substitution; the post-read descriptor and path identities prevent a
 * concurrent writer or rename from publishing a mixed-generation digest. */
static bool digest_regular_file_snapshot(
    Sha256Context* context,
    const char* path,
    const struct stat* expected,
    bool follow) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    if (!follow) flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) return false;

    struct stat before;
    bool ok = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) &&
              cache_stat_identity_equal(expected, &before) &&
              before.st_size >= 0;
    uint64_t total = 0;
    uint8_t buffer[64U * 1024U];
    while (ok) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            uint64_t amount = (uint64_t)count;
            if (amount > UINT64_MAX - total) {
                ok = false;
                break;
            }
            total += amount;
            sha256_update(context, buffer, (size_t)count);
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            ok = false;
        }
    }

    struct stat after;
    if (ok) {
        ok = fstat(fd, &after) == 0 &&
             cache_stat_identity_equal(&before, &after) &&
             total == (uint64_t)before.st_size;
    }
    if (close(fd) != 0) ok = false;

    struct stat current;
    if (ok) {
        ok = cache_stat_path(path, follow, &current) &&
             cache_stat_identity_equal(&before, &current);
    }
    return ok;
}

static bool cache_sha256_file_snapshot(
    const char* path,
    uint8_t digest[USC_CACHE_DIGEST_SIZE],
    UscCacheToolchainLease* lease) {
    if (!path || !digest) return false;
    memset(digest, 0, USC_CACHE_DIGEST_SIZE);
    struct stat link_status;
    if (lstat(path, &link_status) != 0) return false;
    bool follow = S_ISLNK(link_status.st_mode);
    uint8_t* target_name = NULL;
    size_t target_name_size = 0U;
    if (follow && !read_symlink_snapshot(
            path, &link_status, &target_name, &target_name_size)) {
        return false;
    }
    struct stat target_status = link_status;
    if ((follow && stat(path, &target_status) != 0) ||
        !S_ISREG(target_status.st_mode)) {
        free(target_name);
        return false;
    }
    Sha256Context context;
    sha256_init(&context);
    if (!digest_regular_file_snapshot(&context, path, &target_status,
                                      follow)) {
        free(target_name);
        return false;
    }
    if (follow) {
        struct stat current_link;
        struct stat current_target;
        uint8_t* current_name = NULL;
        size_t current_name_size = 0U;
        bool stable = lstat(path, &current_link) == 0 &&
            cache_stat_identity_equal(&link_status, &current_link) &&
            read_symlink_snapshot(path, &link_status,
                                  &current_name, &current_name_size) &&
            current_name_size == target_name_size &&
            memcmp(current_name, target_name, target_name_size) == 0 &&
            stat(path, &current_target) == 0 &&
            cache_stat_identity_equal(&target_status, &current_target);
        free(current_name);
        if (!stable) {
            free(target_name);
            return false;
        }
    }
    bool recorded = follow
        ? cache_lease_record_symlink(
              lease, path, &link_status, &target_status,
              target_name, target_name_size)
        : cache_lease_record_direct(lease, path, &target_status);
    free(target_name);
    if (!recorded) return false;
    sha256_final(&context, digest);
    return true;
}

bool usc_cache_sha256_file(
    const char* path, uint8_t digest[USC_CACHE_DIGEST_SIZE]) {
    return cache_sha256_file_snapshot(path, digest, NULL);
}

static void digest_u32(Sha256Context* context, uint32_t value) {
    uint8_t encoded[4];
    write_le32(encoded, value);
    sha256_update(context, encoded, sizeof(encoded));
}

static void digest_u64(Sha256Context* context, uint64_t value) {
    uint8_t encoded[8];
    write_le64(encoded, value);
    sha256_update(context, encoded, sizeof(encoded));
}

static void digest_buffer(
    Sha256Context* context, const void* data, size_t size) {
    digest_u64(context, (uint64_t)size);
    if (size > 0) sha256_update(context, data, size);
}

static void digest_string(Sha256Context* context, const char* value) {
    size_t size = value ? strlen(value) : 0;
    digest_buffer(context, value, size);
}

typedef struct {
    Sha256Context* digest;
    uint8_t* data;
    size_t size;
    size_t capacity;
    bool capture;
    bool ok;
} CompileRequestSink;

static bool compile_request_sink_reserve(CompileRequestSink* sink,
                                         size_t additional) {
    if (!sink->ok || additional > USC_CACHE_MAX_OUTPUT_SIZE - sink->size) {
        sink->ok = false;
        return false;
    }
    size_t required = sink->size + additional;
    if (!sink->capture || required <= sink->capacity) return true;
    size_t capacity = sink->capacity ? sink->capacity : 1024U;
    while (capacity < required) {
        size_t next = capacity <= USC_CACHE_MAX_OUTPUT_SIZE / 2U
            ? capacity * 2U
            : (size_t)USC_CACHE_MAX_OUTPUT_SIZE;
        if (next <= capacity) {
            sink->ok = false;
            return false;
        }
        capacity = next;
    }
    uint8_t* resized = (uint8_t*)realloc(sink->data, capacity);
    if (!resized) {
        sink->ok = false;
        return false;
    }
    sink->data = resized;
    sink->capacity = capacity;
    return true;
}

static void compile_request_sink_raw(CompileRequestSink* sink,
                                     const void* data, size_t size) {
    if (!compile_request_sink_reserve(sink, size) ||
        (size > 0 && !data)) {
        sink->ok = false;
        return;
    }
    if (sink->digest && size > 0) sha256_update(sink->digest, data, size);
    if (sink->capture && size > 0) {
        memcpy(sink->data + sink->size, data, size);
    }
    sink->size += size;
}

static void compile_request_sink_u32(CompileRequestSink* sink,
                                     uint32_t value) {
    uint8_t encoded[4];
    write_le32(encoded, value);
    compile_request_sink_raw(sink, encoded, sizeof(encoded));
}

static void compile_request_sink_u64(CompileRequestSink* sink,
                                     uint64_t value) {
    uint8_t encoded[8];
    write_le64(encoded, value);
    compile_request_sink_raw(sink, encoded, sizeof(encoded));
}

static void compile_request_sink_buffer(CompileRequestSink* sink,
                                        const void* data, size_t size) {
    compile_request_sink_u64(sink, (uint64_t)size);
    compile_request_sink_raw(sink, data, size);
}

static void compile_request_sink_string(CompileRequestSink* sink,
                                        const char* value) {
    compile_request_sink_buffer(sink, value, value ? strlen(value) : 0U);
}

static void compile_request_sink_string_array(CompileRequestSink* sink,
                                              char** values, int count) {
    int effective_count = values && count > 0 ? count : 0;
    compile_request_sink_u32(sink, (uint32_t)effective_count);
    for (int i = 0; i < effective_count && sink->ok; i++) {
        compile_request_sink_string(sink, values[i]);
    }
}

static void compile_request_sink_variant_set(
    CompileRequestSink* sink, const SnippetKeywordVariantSet* set) {
    compile_request_sink_u32(sink, set && set->present ? 1U : 0U);
    if (set && set->present) {
        compile_request_sink_string_array(
            sink, set->combinations, set->combination_count);
    }
}

static void compile_request_sink_contract(
    CompileRequestSink* sink, const SnippetCompileContract* contract) {
    compile_request_sink_u32(sink, contract ? 1U : 0U);
    if (!contract) return;
    compile_request_sink_u32(sink, (uint32_t)contract->snippet_id);
    compile_request_sink_u32(sink, (uint32_t)contract->platforms);
    compile_request_sink_u32(sink, (uint32_t)contract->quality_variants);
    compile_request_sink_u32(sink, contract->program_types_mask);
    compile_request_sink_u32(sink, contract->compilation_flags);
    compile_request_sink_u32(sink, (uint32_t)contract->language);
    for (int i = 0; i < UNITY_SNIPPET_SOURCE_HASH_WORD_COUNT; i++) {
        compile_request_sink_u32(sink, contract->source_hash[i]);
    }
    compile_request_sink_u32(sink, (uint32_t)contract->start_line);
    compile_request_sink_u32(sink, (uint32_t)contract->use_dxc_apis);
    compile_request_sink_u32(sink, (uint32_t)contract->never_use_dxc_apis);
    compile_request_sink_u32(
        sink, (uint32_t)contract->program_keyword_variant_count);
    for (int i = 0; i < contract->program_keyword_variant_count; i++) {
        const SnippetProgramKeywordVariants* program =
            &contract->program_keyword_variants[i];
        compile_request_sink_u32(
            sink, (uint32_t)program->compiler_program);
        compile_request_sink_variant_set(sink, &program->user_global);
        compile_request_sink_variant_set(sink, &program->user_local);
        compile_request_sink_variant_set(sink, &program->builtin);
    }
    compile_request_sink_string_array(
        sink, contract->non_stripped_user_keywords,
        contract->non_stripped_user_keyword_count);
    compile_request_sink_string_array(
        sink, contract->builtin_keywords,
        contract->builtin_keyword_count);
    compile_request_sink_u64(sink, contract->requirements);
    int conditional_count =
        contract->conditional_requirements &&
        contract->conditional_requirement_count > 0
            ? contract->conditional_requirement_count
            : 0;
    compile_request_sink_u32(sink, (uint32_t)conditional_count);
    for (int i = 0; i < conditional_count && sink->ok; i++) {
        compile_request_sink_string(
            sink, contract->conditional_requirements[i].keyword);
        compile_request_sink_u64(
            sink, contract->conditional_requirements[i].requirements);
    }
}

static bool encode_compile_request(
    CompileRequestSink* sink, const UnityCompilerCompileRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE]) {
    static const uint8_t schema[] =
        "DXBCSandbox.UnityCompiler.compileSnippet.cache.v7";
    if (!sink || !request || !compiler_fingerprint) return false;
    compile_request_sink_buffer(sink, schema, sizeof(schema) - 1U);
    compile_request_sink_buffer(sink, compiler_fingerprint,
                                USC_CACHE_DIGEST_SIZE);
    compile_request_sink_u32(sink, 0x0C0BD1E4U);
    compile_request_sink_string(sink, request->command);
    compile_request_sink_string(sink, request->toolchain_configuration);
    compile_request_sink_string(sink, request->snippet_source);
    compile_request_sink_string(sink, request->source_directory);
    compile_request_sink_string(sink, request->source_basename);
    compile_request_sink_string(sink, request->pass_name);
    compile_request_sink_u32(sink,
                             request->caching_preprocessor ? 1U : 0U);
    compile_request_sink_u32(sink, request->preprocess_only ? 1U : 0U);
    compile_request_sink_u32(sink,
                             request->strip_line_directives ? 1U : 0U);
    compile_request_sink_u32(sink, request->build_platform);
    compile_request_sink_u32(sink, (uint32_t)request->render_state_length);
    compile_request_sink_string_array(
        sink, request->variant_keywords, request->variant_keyword_count);
    compile_request_sink_string_array(
        sink, request->user_keywords, request->user_keyword_count);
    compile_request_sink_string_array(
        sink, request->disabled_keywords, request->disabled_keyword_count);
    compile_request_sink_u32(sink, request->compiler_flags);
    compile_request_sink_u32(sink, (uint32_t)request->language);
    compile_request_sink_u32(sink, (uint32_t)request->shader_type);
    compile_request_sink_u32(sink, (uint32_t)request->platform);
    compile_request_sink_u64(sink, request->requirements);
    compile_request_sink_u32(sink, (uint32_t)request->program_mask);
    compile_request_sink_u32(sink, (uint32_t)request->program_start);
    compile_request_sink_contract(sink, request->snippet_contract);
    compile_request_sink_buffer(
        sink, request->environment_fingerprint,
        request->environment_fingerprint ? USC_CACHE_DIGEST_SIZE : 0U);
    return sink->ok;
}

void usc_cache_request_digest(
    const UnityCompilerCompileRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t digest[USC_CACHE_DIGEST_SIZE]) {
    if (!digest) return;
    Sha256Context context;
    sha256_init(&context);
    CompileRequestSink sink = {
        .digest = &context,
        .capture = false,
        .ok = true,
    };
    if (!encode_compile_request(&sink, request, compiler_fingerprint)) {
        memset(digest, 0, USC_CACHE_DIGEST_SIZE);
        return;
    }
    sha256_final(&context, digest);
}

bool usc_cache_serialize_compile_request(
    const UnityCompilerCompileRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t** out_data,
    size_t* out_size) {
    if (!out_data || !out_size) return false;
    *out_data = NULL;
    *out_size = 0;
    CompileRequestSink sink = {
        .capture = true,
        .ok = true,
    };
    if (!encode_compile_request(&sink, request, compiler_fingerprint)) {
        free(sink.data);
        return false;
    }
    if (sink.size == 0) {
        sink.data = (uint8_t*)malloc(1U);
        if (!sink.data) return false;
    } else if (sink.capacity != sink.size) {
        uint8_t* exact = (uint8_t*)realloc(sink.data, sink.size);
        if (exact) sink.data = exact;
    }
    *out_data = sink.data;
    *out_size = sink.size;
    return true;
}

static bool encode_preprocess_request(
    CompileRequestSink* sink,
    const UnityCompilerPreprocessRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE]) {
    static const uint8_t schema[] =
        "DXBCSandbox.UnityCompiler.preprocess.cache.v7";
    if (!sink || !request || !compiler_fingerprint) return false;
    compile_request_sink_buffer(sink, schema, sizeof(schema) - 1U);
    compile_request_sink_buffer(sink, compiler_fingerprint,
                                USC_CACHE_DIGEST_SIZE);
    compile_request_sink_u32(sink, 0x0C0BD1E4U);
    compile_request_sink_string(sink, request->command);
    compile_request_sink_string(sink, request->source);
    compile_request_sink_string(sink, request->source_directory);
    compile_request_sink_string(sink, request->shader_name);
    compile_request_sink_u32(sink, request->surface_only ? 1U : 0U);
    compile_request_sink_u32(sink,
                             request->caching_preprocessor ? 1U : 0U);
    compile_request_sink_u32(sink, request->build_platform);
    compile_request_sink_u32(sink, request->valid_apis);
    compile_request_sink_string_array(
        sink, request->keywords, request->keyword_count);
    compile_request_sink_string_array(
        sink, request->defines, request->define_count);
    compile_request_sink_string_array(
        sink, request->include_paths, request->include_path_count);
    compile_request_sink_string(sink, request->toolchain_configuration);
    compile_request_sink_buffer(
        sink, request->environment_fingerprint,
        request->environment_fingerprint ? USC_CACHE_DIGEST_SIZE : 0U);
    return sink->ok;
}

void usc_cache_preprocess_request_digest(
    const UnityCompilerPreprocessRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t digest[USC_CACHE_DIGEST_SIZE]) {
    if (!digest) return;
    Sha256Context context;
    sha256_init(&context);
    CompileRequestSink sink = {
        .digest = &context,
        .capture = false,
        .ok = true,
    };
    if (!encode_preprocess_request(&sink, request,
                                   compiler_fingerprint)) {
        memset(digest, 0, USC_CACHE_DIGEST_SIZE);
        return;
    }
    sha256_final(&context, digest);
}

bool usc_cache_serialize_preprocess_request(
    const UnityCompilerPreprocessRequest* request,
    const uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t** out_data,
    size_t* out_size) {
    if (!out_data || !out_size) return false;
    *out_data = NULL;
    *out_size = 0;
    CompileRequestSink sink = {
        .capture = true,
        .ok = true,
    };
    if (!encode_preprocess_request(&sink, request,
                                   compiler_fingerprint)) {
        free(sink.data);
        return false;
    }
    if (sink.size == 0U) {
        sink.data = (uint8_t*)malloc(1U);
        if (!sink.data) return false;
    } else if (sink.capacity != sink.size) {
        uint8_t* exact = (uint8_t*)realloc(sink.data, sink.size);
        if (exact) sink.data = exact;
    }
    *out_data = sink.data;
    *out_size = sink.size;
    return true;
}

static char* join_path(const char* parent, const char* child) {
    size_t parent_size = strlen(parent);
    size_t child_size = strlen(child);
    bool needs_slash = parent_size > 0 && parent[parent_size - 1U] != '/';
    if (parent_size > SIZE_MAX - child_size - (needs_slash ? 2U : 1U)) {
        return NULL;
    }
    char* joined = (char*)malloc(parent_size + child_size +
                                 (needs_slash ? 2U : 1U));
    if (!joined) return NULL;
    memcpy(joined, parent, parent_size);
    size_t cursor = parent_size;
    if (needs_slash) joined[cursor++] = '/';
    memcpy(joined + cursor, child, child_size + 1U);
    return joined;
}

typedef struct {
    dev_t device;
    ino_t inode;
} CacheDirectoryIdentity;

typedef struct {
    CacheDirectoryIdentity* ancestors;
    size_t ancestor_count;
    size_t ancestor_capacity;
    UscCacheToolchainLease* lease;
} CacheTreeState;

static bool cache_tree_push_directory(CacheTreeState* state,
                                      const struct stat* status) {
    for (size_t i = 0; i < state->ancestor_count; i++) {
        if (state->ancestors[i].device == status->st_dev &&
            state->ancestors[i].inode == status->st_ino) {
            return false;
        }
    }
    if (state->ancestor_count == state->ancestor_capacity) {
        size_t next = state->ancestor_capacity
            ? state->ancestor_capacity * 2U
            : 16U;
        if (next < state->ancestor_capacity ||
            next > SIZE_MAX / sizeof(*state->ancestors)) {
            return false;
        }
        CacheDirectoryIdentity* resized =
            (CacheDirectoryIdentity*)realloc(
                state->ancestors, next * sizeof(*state->ancestors));
        if (!resized) return false;
        state->ancestors = resized;
        state->ancestor_capacity = next;
    }
    state->ancestors[state->ancestor_count].device = status->st_dev;
    state->ancestors[state->ancestor_count].inode = status->st_ino;
    state->ancestor_count++;
    return true;
}

static void cache_tree_pop_directory(CacheTreeState* state) {
    if (state->ancestor_count > 0U) state->ancestor_count--;
}

static bool read_symlink_snapshot(const char* path,
                                  const struct stat* expected,
                                  uint8_t** out_data,
                                  size_t* out_size) {
    *out_data = NULL;
    *out_size = 0U;
    size_t capacity = 256U;
    if (expected->st_size > 0) {
        uintmax_t expected_size = (uintmax_t)expected->st_size;
        if (expected_size >= 1024U * 1024U ||
            expected_size > SIZE_MAX - 1U) {
            return false;
        }
        capacity = (size_t)expected_size + 1U;
    }
    if (capacity < 256U) capacity = 256U;
    while (capacity <= 1024U * 1024U) {
        uint8_t* data = (uint8_t*)malloc(capacity);
        if (!data) return false;
        ssize_t size = readlink(path, (char*)data, capacity);
        if (size < 0) {
            free(data);
            return false;
        }
        if ((size_t)size < capacity) {
            struct stat current;
            if (lstat(path, &current) != 0 ||
                !cache_stat_identity_equal(expected, &current)) {
                free(data);
                return false;
            }
            *out_data = data;
            *out_size = (size_t)size;
            return true;
        }
        free(data);
        if (capacity > (1024U * 1024U) / 2U) break;
        capacity *= 2U;
    }
    return false;
}

bool usc_cache_toolchain_lease_validate(
    const UscCacheToolchainLease* lease) {
    if (!lease) return false;
    for (size_t i = 0; i < lease->entry_count; i++) {
        const CacheLeaseEntry* entry = &lease->entries[i];
        struct stat link_status;
        if (entry->kind == CACHE_LEASE_ENTRY_MISSING) {
            if (lstat(entry->path, &link_status) == 0 || errno != ENOENT) {
                return false;
            }
            continue;
        }
        if (lstat(entry->path, &link_status) != 0 ||
            !cache_lease_stat_matches(&entry->link_status, &link_status)) {
            return false;
        }
        if (entry->kind == CACHE_LEASE_ENTRY_DIRECT) continue;
        if (entry->kind != CACHE_LEASE_ENTRY_SYMLINK ||
            !S_ISLNK(link_status.st_mode)) {
            return false;
        }
        uint8_t* target_name = NULL;
        size_t target_name_size = 0U;
        struct stat target_status;
        bool ok = read_symlink_snapshot(
                      entry->path, &link_status,
                      &target_name, &target_name_size) &&
                  target_name_size == entry->target_name_size &&
                  (target_name_size == 0U ||
                   memcmp(target_name, entry->target_name,
                          target_name_size) == 0) &&
                  stat(entry->path, &target_status) == 0 &&
                  cache_lease_stat_matches(
                      &entry->target_status, &target_status);
        free(target_name);
        if (!ok) return false;
    }
    return true;
}

static int compare_directory_entry_names(const void* left,
                                         const void* right) {
    const char* const* left_name = (const char* const*)left;
    const char* const* right_name = (const char* const*)right;
    return strcmp(*left_name, *right_name);
}

static void free_directory_entry_names(char** names, size_t count) {
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

static bool collect_directory_entry_names(DIR* directory,
                                          char*** out_names,
                                          size_t* out_count) {
    *out_names = NULL;
    *out_count = 0U;
    size_t capacity = 0U;
    errno = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            errno = 0;
            continue;
        }
        if (*out_count == capacity) {
            size_t next = capacity ? capacity * 2U : 32U;
            if (next < capacity || next > SIZE_MAX / sizeof(**out_names)) {
                free_directory_entry_names(*out_names, *out_count);
                *out_names = NULL;
                *out_count = 0U;
                return false;
            }
            char** resized = (char**)realloc(
                *out_names, next * sizeof(**out_names));
            if (!resized) {
                free_directory_entry_names(*out_names, *out_count);
                *out_names = NULL;
                *out_count = 0U;
                return false;
            }
            *out_names = resized;
            capacity = next;
        }
        (*out_names)[*out_count] = strdup(entry->d_name);
        if (!(*out_names)[*out_count]) {
            free_directory_entry_names(*out_names, *out_count);
            *out_names = NULL;
            *out_count = 0U;
            return false;
        }
        (*out_count)++;
        errno = 0;
    }
    if (errno != 0) {
        free_directory_entry_names(*out_names, *out_count);
        *out_names = NULL;
        *out_count = 0U;
        return false;
    }
    if (*out_count > 1U) {
        qsort(*out_names, *out_count, sizeof(**out_names),
              compare_directory_entry_names);
    }
    return true;
}

static bool open_directory_snapshot(const char* path,
                                    const struct stat* expected,
                                    bool follow,
                                    DIR** out_directory) {
    *out_directory = NULL;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    if (!follow) flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) return false;
    struct stat opened;
    if (fstat(fd, &opened) != 0 || !S_ISDIR(opened.st_mode) ||
        !cache_stat_identity_equal(expected, &opened)) {
        close(fd);
        return false;
    }
    DIR* directory = fdopendir(fd);
    if (!directory) {
        close(fd);
        return false;
    }
    *out_directory = directory;
    return true;
}

static bool finish_directory_snapshot(DIR* directory,
                                      const char* path,
                                      const struct stat* expected,
                                      bool follow) {
    struct stat after;
    bool ok = fstat(dirfd(directory), &after) == 0 &&
              cache_stat_identity_equal(expected, &after);
    if (closedir(directory) != 0) ok = false;
    struct stat current;
    if (ok) {
        ok = cache_stat_path(path, follow, &current) &&
             cache_stat_identity_equal(expected, &current);
    }
    return ok;
}

static bool digest_tree_node(Sha256Context* context,
                             const char* path,
                             const char* relative_path,
                             CacheTreeState* state);

static bool digest_directory_contents(Sha256Context* context,
                                      const char* path,
                                      const char* relative_path,
                                      const struct stat* status,
                                      bool follow,
                                      CacheTreeState* state) {
    if (!cache_tree_push_directory(state, status)) return false;
    DIR* directory = NULL;
    bool ok = open_directory_snapshot(path, status, follow, &directory);
    char** names = NULL;
    size_t name_count = 0U;
    if (ok) ok = collect_directory_entry_names(
        directory, &names, &name_count);
    for (size_t i = 0; ok && i < name_count; i++) {
        char* child_path = join_path(path, names[i]);
        char* child_relative = relative_path[0]
            ? join_path(relative_path, names[i])
            : strdup(names[i]);
        if (!child_path || !child_relative ||
            !digest_tree_node(context, child_path, child_relative, state)) {
            ok = false;
        }
        free(child_path);
        free(child_relative);
    }
    free_directory_entry_names(names, name_count);
    if (directory && !finish_directory_snapshot(
            directory, path, status, follow)) {
        ok = false;
    }
    cache_tree_pop_directory(state);
    return ok;
}

static bool digest_resolved_tree_node(Sha256Context* context,
                                      const char* path,
                                      const char* relative_path,
                                      const struct stat* status,
                                      bool follow,
                                      CacheTreeState* state) {
    if (S_ISREG(status->st_mode)) {
        if (status->st_size < 0) return false;
        digest_u32(context, 2U);
        digest_string(context, relative_path);
        digest_u64(context, (uint64_t)status->st_size);
        bool ok = digest_regular_file_snapshot(
            context, path, status, follow);
        if (ok && !follow) {
            ok = cache_lease_record_direct(state->lease, path, status);
        }
        return ok;
    }
    if (S_ISDIR(status->st_mode)) {
        digest_u32(context, 3U);
        digest_string(context, relative_path);
        bool ok = digest_directory_contents(
            context, path, relative_path, status, follow, state);
        if (ok && !follow) {
            ok = cache_lease_record_direct(state->lease, path, status);
        }
        return ok;
    }
    /* Device nodes, FIFOs and sockets are not stable byte inputs.  Failing
     * closed avoids a digest that silently omits compiler-visible behavior. */
    return false;
}

static bool digest_tree_node(Sha256Context* context,
                             const char* path,
                             const char* relative_path,
                             CacheTreeState* state) {
    struct stat link_status;
    if (lstat(path, &link_status) != 0) {
        if (errno != ENOENT) return false;
        digest_u32(context, 0U);
        digest_string(context, relative_path);
        return cache_lease_record_missing(state->lease, path);
    }
    if (!S_ISLNK(link_status.st_mode)) {
        return digest_resolved_tree_node(context, path, relative_path,
                                         &link_status, false, state);
    }

    uint8_t* target_name = NULL;
    size_t target_name_size = 0U;
    if (!read_symlink_snapshot(path, &link_status,
                               &target_name, &target_name_size)) {
        return false;
    }
    struct stat target_status;
    if (stat(path, &target_status) != 0) {
        free(target_name);
        return false;
    }
    digest_u32(context, 1U);
    digest_string(context, relative_path);
    digest_buffer(context, target_name, target_name_size);
    bool ok = digest_resolved_tree_node(context, path, relative_path,
                                        &target_status, true, state);

    uint8_t* current_target_name = NULL;
    size_t current_target_name_size = 0U;
    struct stat current_target_status;
    if (ok) {
        ok = read_symlink_snapshot(path, &link_status,
                                   &current_target_name,
                                   &current_target_name_size) &&
             current_target_name_size == target_name_size &&
             memcmp(current_target_name, target_name,
                    target_name_size) == 0 &&
             stat(path, &current_target_status) == 0 &&
             cache_stat_identity_equal(&target_status,
                                       &current_target_status);
    }
    free(current_target_name);
    if (ok) {
        ok = cache_lease_record_symlink(
            state->lease, path, &link_status, &target_status,
            target_name, target_name_size);
    }
    free(target_name);
    return ok;
}

static bool digest_environment_root(Sha256Context* context,
                                    const char* label,
                                    const char* path,
                                    CacheTreeState* state) {
    digest_string(context, label);
    digest_string(context, path);
    if (!path || !path[0]) {
        digest_u32(context, 0U);
        return true;
    }
    return digest_tree_node(context, path, "", state);
}

static bool begin_selected_directory(const char* path,
                                     Sha256Context* context,
                                     const char* relative_path,
                                     CacheTreeState* state,
                                     struct stat* link_status,
                                     struct stat* target_status,
                                     bool* follow,
                                     uint8_t** target_name,
                                     size_t* target_name_size,
                                     DIR** directory) {
    *follow = false;
    *target_name = NULL;
    *target_name_size = 0U;
    *directory = NULL;
    if (lstat(path, link_status) != 0) return false;
    *target_status = *link_status;
    if (S_ISLNK(link_status->st_mode)) {
        *follow = true;
        if (!read_symlink_snapshot(path, link_status,
                                   target_name, target_name_size) ||
            stat(path, target_status) != 0) {
            free(*target_name);
            *target_name = NULL;
            return false;
        }
        digest_u32(context, 1U);
        digest_string(context, relative_path);
        digest_buffer(context, *target_name, *target_name_size);
    }
    if (!S_ISDIR(target_status->st_mode)) {
        free(*target_name);
        *target_name = NULL;
        return false;
    }
    digest_u32(context, 3U);
    digest_string(context, relative_path);
    if (!cache_tree_push_directory(state, target_status)) {
        free(*target_name);
        *target_name = NULL;
        return false;
    }
    if (!open_directory_snapshot(path, target_status, *follow, directory)) {
        cache_tree_pop_directory(state);
        free(*target_name);
        *target_name = NULL;
        return false;
    }
    return true;
}

static bool finish_selected_directory(const char* path,
                                      CacheTreeState* state,
                                      const struct stat* link_status,
                                      const struct stat* target_status,
                                      bool follow,
                                      const uint8_t* target_name,
                                      size_t target_name_size,
                                      DIR* directory) {
    bool ok = finish_directory_snapshot(directory, path, target_status,
                                        follow);
    cache_tree_pop_directory(state);
    if (ok && follow) {
        uint8_t* current_name = NULL;
        size_t current_name_size = 0U;
        struct stat current_target;
        ok = read_symlink_snapshot(path, link_status,
                                   &current_name, &current_name_size) &&
             current_name_size == target_name_size &&
             memcmp(current_name, target_name, target_name_size) == 0 &&
             stat(path, &current_target) == 0 &&
             cache_stat_identity_equal(target_status, &current_target);
        free(current_name);
    }
    if (ok) {
        ok = follow
            ? cache_lease_record_symlink(
                  state->lease, path, link_status, target_status,
                  target_name, target_name_size)
            : cache_lease_record_direct(
                  state->lease, path, target_status);
    }
    return ok;
}

static bool digest_contents_plugins(Sha256Context* context,
                                    const char* contents_path,
                                    CacheTreeState* state) {
    digest_string(context, "unity-contents");
    digest_string(context, contents_path);
    if (!contents_path || !contents_path[0]) {
        digest_u32(context, 0U);
        return true;
    }
    struct stat missing_check;
    if (lstat(contents_path, &missing_check) != 0) {
        if (errno != ENOENT) return false;
        digest_u32(context, 0U);
        return cache_lease_record_missing(state->lease, contents_path);
    }
    struct stat link_status;
    struct stat target_status;
    bool follow;
    uint8_t* target_name = NULL;
    size_t target_name_size = 0U;
    DIR* directory = NULL;
    if (!begin_selected_directory(
            contents_path, context, "", state, &link_status,
            &target_status, &follow, &target_name, &target_name_size,
            &directory)) {
        return false;
    }
    char* plugin_path = join_path(contents_path, "CgBatchPlugins64");
    bool ok = plugin_path && digest_tree_node(
        context, plugin_path, "CgBatchPlugins64", state);
    free(plugin_path);
    if (!finish_selected_directory(
            contents_path, state, &link_status, &target_status, follow,
            target_name, target_name_size, directory)) {
        ok = false;
    }
    free(target_name);
    return ok;
}

static bool digest_playback_engine_plugins(
    Sha256Context* context,
    const char* engine_path,
    const char* engine_name,
    CacheTreeState* state) {
    struct stat link_status;
    if (lstat(engine_path, &link_status) != 0) return false;
    struct stat target_status = link_status;
    bool follow = S_ISLNK(link_status.st_mode);
    uint8_t* target_name = NULL;
    size_t target_name_size = 0U;
    if (follow &&
        (!read_symlink_snapshot(engine_path, &link_status,
                                &target_name, &target_name_size) ||
         stat(engine_path, &target_status) != 0)) {
        free(target_name);
        return false;
    }
    if (!S_ISDIR(target_status.st_mode)) {
        bool recorded = follow
            ? cache_lease_record_symlink(
                  state->lease, engine_path, &link_status, &target_status,
                  target_name, target_name_size)
            : cache_lease_record_direct(
                  state->lease, engine_path, &target_status);
        free(target_name);
        return recorded;
    }

    DIR* directory = NULL;
    digest_u32(context, follow ? 4U : 3U);
    digest_string(context, engine_name);
    if (follow) digest_buffer(context, target_name, target_name_size);
    if (!cache_tree_push_directory(state, &target_status)) {
        free(target_name);
        return false;
    }
    if (!open_directory_snapshot(engine_path, &target_status, follow,
                                 &directory)) {
        cache_tree_pop_directory(state);
        free(target_name);
        return false;
    }
    char* plugin_path = join_path(engine_path, "CgBatchPlugins64");
    char* relative_path = join_path(engine_name, "CgBatchPlugins64");
    bool ok = plugin_path && relative_path && digest_tree_node(
        context, plugin_path, relative_path, state);
    free(plugin_path);
    free(relative_path);
    if (!finish_directory_snapshot(
            directory, engine_path, &target_status, follow)) {
        ok = false;
    }
    cache_tree_pop_directory(state);
    if (ok && follow) {
        uint8_t* current_name = NULL;
        size_t current_name_size = 0U;
        struct stat current_target;
        ok = read_symlink_snapshot(engine_path, &link_status,
                                   &current_name, &current_name_size) &&
             current_name_size == target_name_size &&
             memcmp(current_name, target_name, target_name_size) == 0 &&
             stat(engine_path, &current_target) == 0 &&
             cache_stat_identity_equal(&target_status, &current_target);
        free(current_name);
    }
    if (ok) {
        ok = follow
            ? cache_lease_record_symlink(
                  state->lease, engine_path, &link_status, &target_status,
                  target_name, target_name_size)
            : cache_lease_record_direct(
                  state->lease, engine_path, &target_status);
    }
    free(target_name);
    return ok;
}

static bool digest_playback_plugins(Sha256Context* context,
                                    const char* playback_path,
                                    CacheTreeState* state) {
    digest_string(context, "playback-engines");
    digest_string(context, playback_path);
    if (!playback_path || !playback_path[0]) {
        digest_u32(context, 0U);
        return true;
    }
    struct stat missing_check;
    if (lstat(playback_path, &missing_check) != 0) {
        if (errno != ENOENT) return false;
        digest_u32(context, 0U);
        return cache_lease_record_missing(state->lease, playback_path);
    }
    struct stat link_status;
    struct stat target_status;
    bool follow;
    uint8_t* target_name = NULL;
    size_t target_name_size = 0U;
    DIR* directory = NULL;
    if (!begin_selected_directory(
            playback_path, context, "", state, &link_status,
            &target_status, &follow, &target_name, &target_name_size,
            &directory)) {
        return false;
    }
    char** names = NULL;
    size_t name_count = 0U;
    bool ok = collect_directory_entry_names(directory, &names, &name_count);
    for (size_t i = 0; ok && i < name_count; i++) {
        char* engine_path = join_path(playback_path, names[i]);
        if (!engine_path || !digest_playback_engine_plugins(
                context, engine_path, names[i], state)) {
            ok = false;
        }
        free(engine_path);
    }
    free_directory_entry_names(names, name_count);
    if (!finish_selected_directory(
            playback_path, state, &link_status, &target_status, follow,
            target_name, target_name_size, directory)) {
        ok = false;
    }
    free(target_name);
    if (ok) digest_u32(context, 0U);
    return ok;
}

static bool cache_environment_fingerprint_snapshot(
    const char* project_root,
    const char* includes_dir,
    const char* sandbox_includes_dir,
    const char* builtin_includes_dir,
    const char* proxy_path,
    const char* glslang_path,
    const char* dxcompiler_path,
    const char* unity_contents_path,
    const char* playback_engines_path,
    uint8_t digest[USC_CACHE_DIGEST_SIZE],
    UscCacheToolchainLease* lease) {
    if (!digest) return false;
    memset(digest, 0, USC_CACHE_DIGEST_SIZE);
    static const uint8_t schema[] =
        "DXBCSandbox.UnityCompiler.environment.v4";
    Sha256Context context;
    CacheTreeState state = {.lease = lease};
    sha256_init(&context);
    digest_buffer(&context, schema, sizeof(schema) - 1U);
    digest_string(&context, project_root);
    bool ok = digest_environment_root(
                  &context, "explicit-includes", includes_dir, &state) &&
              digest_environment_root(
                  &context, "sandbox-includes", sandbox_includes_dir,
                  &state) &&
              digest_environment_root(
                  &context, "unity-builtins", builtin_includes_dir,
                  &state) &&
              digest_environment_root(
                  &context, "compiler-proxy", proxy_path, &state) &&
              digest_environment_root(
                  &context, "glslang-library", glslang_path, &state) &&
              digest_environment_root(
                  &context, "dxcompiler-library", dxcompiler_path,
                  &state) &&
              digest_contents_plugins(
                  &context, unity_contents_path, &state) &&
              digest_playback_plugins(
                  &context, playback_engines_path, &state);
    free(state.ancestors);
    if (!ok || state.ancestor_count != 0U) return false;
    sha256_final(&context, digest);
    return true;
}

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
    uint8_t digest[USC_CACHE_DIGEST_SIZE]) {
    if (!digest) return false;
    UscCacheToolchainLease* snapshot =
        (UscCacheToolchainLease*)calloc(1U, sizeof(*snapshot));
    if (!snapshot) {
        memset(digest, 0, USC_CACHE_DIGEST_SIZE);
        return false;
    }
    bool ok = cache_environment_fingerprint_snapshot(
        project_root, includes_dir, sandbox_includes_dir,
        builtin_includes_dir, proxy_path, glslang_path, dxcompiler_path,
        unity_contents_path, playback_engines_path, digest, snapshot) &&
        usc_cache_toolchain_lease_validate(snapshot);
    usc_cache_toolchain_lease_destroy(snapshot);
    if (!ok) memset(digest, 0, USC_CACHE_DIGEST_SIZE);
    return ok;
}

static bool cache_lease_record_current_directory(
    UscCacheToolchainLease* lease, const char* path) {
    if (!lease || !path || !path[0]) return false;
    struct stat link_status;
    if (lstat(path, &link_status) != 0) return false;
    if (!S_ISLNK(link_status.st_mode)) {
        return S_ISDIR(link_status.st_mode) &&
               cache_lease_record_direct(lease, path, &link_status);
    }
    uint8_t* target_name = NULL;
    size_t target_name_size = 0U;
    struct stat target_status;
    bool ok = read_symlink_snapshot(
                  path, &link_status, &target_name, &target_name_size) &&
              stat(path, &target_status) == 0 &&
              S_ISDIR(target_status.st_mode) &&
              cache_lease_record_symlink(
                  lease, path, &link_status, &target_status,
                  target_name, target_name_size);
    free(target_name);
    return ok;
}

bool usc_cache_toolchain_lease_create(
    const UscCacheToolchainPaths* paths,
    uint8_t compiler_digest[USC_CACHE_DIGEST_SIZE],
    uint8_t environment_digest[USC_CACHE_DIGEST_SIZE],
    UscCacheToolchainLease** out_lease) {
    if (!compiler_digest || !environment_digest || !out_lease) return false;
    memset(compiler_digest, 0, USC_CACHE_DIGEST_SIZE);
    memset(environment_digest, 0, USC_CACHE_DIGEST_SIZE);
    *out_lease = NULL;
    if (!paths || !paths->compiler_path || !paths->compiler_path[0] ||
        !paths->project_root || !paths->project_root[0]) {
        return false;
    }
    UscCacheToolchainLease* lease =
        (UscCacheToolchainLease*)calloc(1U, sizeof(*lease));
    if (!lease) return false;
    bool ok = cache_sha256_file_snapshot(
                  paths->compiler_path, compiler_digest, lease) &&
              cache_lease_record_current_directory(
                  lease, paths->project_root) &&
              cache_environment_fingerprint_snapshot(
                  paths->project_root, paths->includes_dir,
                  paths->sandbox_includes_dir, paths->builtin_includes_dir,
                  paths->proxy_path, paths->glslang_path,
                  paths->dxcompiler_path, paths->unity_contents_path,
                  paths->playback_engines_path, environment_digest,
                  lease) &&
              usc_cache_toolchain_lease_validate(lease);
    if (!ok) {
        memset(compiler_digest, 0, USC_CACHE_DIGEST_SIZE);
        memset(environment_digest, 0, USC_CACHE_DIGEST_SIZE);
        usc_cache_toolchain_lease_destroy(lease);
        return false;
    }
    *out_lease = lease;
    return true;
}

bool usc_cache_search_roots_lease_create(
    const char* const* roots,
    size_t root_count,
    const uint8_t environment_digest[USC_CACHE_DIGEST_SIZE],
    uint8_t request_environment_digest[USC_CACHE_DIGEST_SIZE],
    UscCacheToolchainLease** out_lease) {
    if (!request_environment_digest || !out_lease) return false;
    /* Permit in-place extension of the environment digest. */
    uint8_t base_digest[USC_CACHE_DIGEST_SIZE] = {0};
    if (environment_digest)
        memcpy(base_digest, environment_digest, sizeof(base_digest));
    memset(request_environment_digest, 0, USC_CACHE_DIGEST_SIZE);
    *out_lease = NULL;
    if (!environment_digest || !roots || root_count == 0U || root_count > 64U)
        return false;
    for (size_t i = 0; i < root_count; ++i)
        if (!roots[i] || roots[i][0] != '/') return false;
    UscCacheToolchainLease* lease = calloc(1, sizeof(*lease));
    if (!lease) return false;
    CacheTreeState state = {.lease = lease};
    Sha256Context context;
    sha256_init(&context);
    static const uint8_t schema[] =
        "DXBCSandbox.UnityCompiler.request-environment.v1";
    digest_buffer(&context, schema, sizeof(schema) - 1U);
    digest_buffer(&context, base_digest, sizeof(base_digest));
    digest_u64(&context, (uint64_t)root_count);
    bool ok = true;
    for (size_t i = 0; ok && i < root_count; ++i)
        ok = digest_environment_root(&context, "search-root", roots[i], &state);
    free(state.ancestors);
    ok = ok && state.ancestor_count == 0U &&
         usc_cache_toolchain_lease_validate(lease);
    if (!ok) {
        usc_cache_toolchain_lease_destroy(lease);
        return false;
    }
    sha256_final(&context, request_environment_digest);
    *out_lease = lease;
    return true;
}

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
    bool ok;
} CacheEncoder;

static bool encoder_reserve(CacheEncoder* encoder, size_t additional) {
    if (!encoder->ok || additional > USC_CACHE_MAX_OUTPUT_SIZE - encoder->size) {
        encoder->ok = false;
        return false;
    }
    size_t required = encoder->size + additional;
    if (required <= encoder->capacity) return true;
    size_t capacity = encoder->capacity ? encoder->capacity : 1024U;
    while (capacity < required) {
        size_t next = capacity <= USC_CACHE_MAX_OUTPUT_SIZE / 2U
            ? capacity * 2U
            : (size_t)USC_CACHE_MAX_OUTPUT_SIZE;
        if (next <= capacity) {
            encoder->ok = false;
            return false;
        }
        capacity = next;
    }
    uint8_t* data = (uint8_t*)realloc(encoder->data, capacity);
    if (!data) {
        encoder->ok = false;
        return false;
    }
    encoder->data = data;
    encoder->capacity = capacity;
    return true;
}

static void encoder_bytes(
    CacheEncoder* encoder, const void* data, size_t size) {
    if (!encoder_reserve(encoder, size)) return;
    if (size > 0) memcpy(encoder->data + encoder->size, data, size);
    encoder->size += size;
}

static void encoder_u32(CacheEncoder* encoder, uint32_t value) {
    uint8_t encoded[4];
    write_le32(encoded, value);
    encoder_bytes(encoder, encoded, sizeof(encoded));
}

static void encoder_u64(CacheEncoder* encoder, uint64_t value) {
    uint8_t encoded[8];
    write_le64(encoded, value);
    encoder_bytes(encoder, encoded, sizeof(encoded));
}

static void encoder_string(CacheEncoder* encoder, const char* value) {
    size_t size = value ? strlen(value) : 0U;
    encoder_u64(encoder, (uint64_t)size);
    encoder_bytes(encoder, value, size);
}

static bool cache_diagnostic_is_valid(
    const UnityCompilerDiagnostic* diagnostic) {
    if (!diagnostic || !diagnostic->record || !diagnostic->file ||
        !diagnostic->message) {
        return false;
    }
    char canonical[96];
    int length = snprintf(
        canonical, sizeof(canonical), "err: %d %d %d",
        diagnostic->fields[0], diagnostic->fields[1],
        diagnostic->fields[2]);
    return length > 0 && (size_t)length < sizeof(canonical) &&
           strcmp(canonical, diagnostic->record) == 0;
}

static bool cache_response_status_is_valid(
    const UnityCompilerResponseStatus* status) {
    if (!status ||
        status->availability != UNITY_COMPILER_RESPONSE_AVAILABLE ||
        status->diagnostic_count > USC_PREPROCESS_MAX_COUNT ||
        (status->diagnostic_count > 0U && !status->diagnostics)) {
        return false;
    }
    for (size_t i = 0; i < status->diagnostic_count; i++) {
        if (!cache_diagnostic_is_valid(&status->diagnostics[i])) return false;
    }
    return true;
}

static void encoder_response_status(
    CacheEncoder* encoder, const UnityCompilerResponseStatus* status) {
    if (!cache_response_status_is_valid(status)) {
        encoder->ok = false;
        return;
    }
    encoder_u32(encoder, status->compiler_success ? 1U : 0U);
    encoder_u32(encoder, (uint32_t)status->diagnostic_count);
    for (size_t i = 0; i < status->diagnostic_count && encoder->ok; i++) {
        const UnityCompilerDiagnostic* diagnostic = &status->diagnostics[i];
        for (size_t field = 0; field < 3U; field++) {
            encoder_u32(encoder, (uint32_t)diagnostic->fields[field]);
        }
        encoder_string(encoder, diagnostic->record);
        encoder_string(encoder, diagnostic->file);
        encoder_string(encoder, diagnostic->message);
    }
}

static void encoder_string_array(
    CacheEncoder* encoder, char** values, int count) {
    encoder_u32(encoder, (uint32_t)count);
    for (int i = 0; i < count && encoder->ok; i++) {
        encoder_string(encoder, values[i]);
    }
}

static void encoder_variant_set(
    CacheEncoder* encoder, const SnippetKeywordVariantSet* set) {
    encoder_u32(encoder, set && set->present ? 1U : 0U);
    if (set && set->present) {
        encoder_string_array(
            encoder, set->combinations, set->combination_count);
    }
}

static void encoder_snippet_contract(
    CacheEncoder* encoder, const SnippetCompileContract* contract) {
    encoder_u32(encoder, (uint32_t)contract->snippet_id);
    encoder_u32(encoder, (uint32_t)contract->platforms);
    encoder_u32(encoder, (uint32_t)contract->quality_variants);
    encoder_u32(encoder, contract->program_types_mask);
    encoder_u32(encoder, contract->compilation_flags);
    encoder_u32(encoder, (uint32_t)contract->language);
    for (int i = 0; i < UNITY_SNIPPET_SOURCE_HASH_WORD_COUNT; i++) {
        encoder_u32(encoder, contract->source_hash[i]);
    }
    encoder_u32(encoder, (uint32_t)contract->start_line);
    encoder_u32(encoder, (uint32_t)contract->use_dxc_apis);
    encoder_u32(encoder, (uint32_t)contract->never_use_dxc_apis);
    encoder_u32(encoder,
                (uint32_t)contract->program_keyword_variant_count);
    for (int i = 0; i < contract->program_keyword_variant_count; i++) {
        const SnippetProgramKeywordVariants* program =
            &contract->program_keyword_variants[i];
        encoder_u32(encoder, (uint32_t)program->compiler_program);
        encoder_variant_set(encoder, &program->user_global);
        encoder_variant_set(encoder, &program->user_local);
        encoder_variant_set(encoder, &program->builtin);
    }
    encoder_string_array(encoder, contract->non_stripped_user_keywords,
                         contract->non_stripped_user_keyword_count);
    encoder_string_array(encoder, contract->builtin_keywords,
                         contract->builtin_keyword_count);
    encoder_u64(encoder, contract->requirements);
    encoder_u32(encoder,
                (uint32_t)contract->conditional_requirement_count);
    for (int i = 0;
         i < contract->conditional_requirement_count && encoder->ok; i++) {
        encoder_string(encoder,
                       contract->conditional_requirements[i].keyword);
        encoder_u64(encoder,
                    contract->conditional_requirements[i].requirements);
    }
}

bool usc_cache_serialize_preprocess_result(
    const PreprocessResult* result,
    uint8_t** out_data,
    size_t* out_size) {
    static const uint8_t magic[8] = {
        'U', 'S', 'C', 'P', 'R', 'E', 'P', '4'
    };
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!result || !out_data || !out_size || result->snippet_count < 0 ||
        (uint32_t)result->snippet_count > USC_PREPROCESS_MAX_COUNT ||
        (result->snippet_count > 0 && !result->snippets) ||
        (result->blob_len > 0 && !result->blob)) {
        return false;
    }

    CacheEncoder encoder = {.ok = true};
    encoder_bytes(&encoder, magic, sizeof(magic));
    encoder_u32(&encoder, 4U);
    encoder_u32(&encoder, 0U);
    encoder_u32(&encoder, (uint32_t)result->snippet_count);
    encoder_u64(&encoder, (uint64_t)result->blob_len);
    encoder_bytes(&encoder, result->blob, result->blob_len);
    for (int i = 0; i < result->snippet_count && encoder.ok; i++) {
        const PreprocessedSnippet* snippet = &result->snippets[i];
        if (!snippet->source ||
            (snippet->has_contract &&
             !unity_compiler_snippet_contract_validate(
                 &snippet->contract)) ||
            (!snippet->has_contract &&
             (snippet->conditional_requirement_count < 0 ||
              (uint32_t)snippet->conditional_requirement_count >
                  USC_PREPROCESS_MAX_COUNT ||
              (snippet->conditional_requirement_count > 0 &&
               !snippet->conditional_requirements)))) {
            encoder.ok = false;
            break;
        }
        encoder_string(&encoder, snippet->source);
        encoder_u32(&encoder, snippet->has_contract ? 1U : 0U);
        if (snippet->has_contract) {
            encoder_snippet_contract(&encoder, &snippet->contract);
        } else {
            encoder_u64(&encoder, snippet->reqs);
            encoder_u32(&encoder, (uint32_t)snippet->language);
            encoder_u32(&encoder, (uint32_t)snippet->gpu_program_id);
            encoder_u32(&encoder,
                        (uint32_t)snippet->conditional_requirement_count);
            for (int j = 0; j < snippet->conditional_requirement_count;
                 j++) {
                const ConditionalShaderRequirement* requirement =
                    &snippet->conditional_requirements[j];
                if (!requirement->keyword) {
                    encoder.ok = false;
                    break;
                }
                encoder_string(&encoder, requirement->keyword);
                encoder_u64(&encoder, requirement->requirements);
            }
        }
    }
    if (!encoder.ok) {
        free(encoder.data);
        return false;
    }
    *out_data = encoder.data;
    *out_size = encoder.size;
    return true;
}

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t cursor;
} CacheDecoder;

static bool decoder_bytes(
    CacheDecoder* decoder, void* out_data, size_t size) {
    if (size > decoder->size - decoder->cursor) return false;
    if (size > 0 && out_data) {
        memcpy(out_data, decoder->data + decoder->cursor, size);
    }
    decoder->cursor += size;
    return true;
}

static bool decoder_u32(CacheDecoder* decoder, uint32_t* value) {
    uint8_t encoded[4];
    if (!decoder_bytes(decoder, encoded, sizeof(encoded))) return false;
    *value = cache_read_le32(encoded);
    return true;
}

static bool decoder_u64(CacheDecoder* decoder, uint64_t* value) {
    uint8_t encoded[8];
    if (!decoder_bytes(decoder, encoded, sizeof(encoded))) return false;
    *value = cache_read_le64(encoded);
    return true;
}

static bool decoder_string(CacheDecoder* decoder, char** value) {
    *value = NULL;
    uint64_t size64 = 0;
    if (!decoder_u64(decoder, &size64) || size64 > SIZE_MAX - 1U ||
        size64 > decoder->size - decoder->cursor) {
        return false;
    }
    size_t size = (size_t)size64;
    char* string = (char*)malloc(size + 1U);
    if (!string) return false;
    if (!decoder_bytes(decoder, string, size)) {
        free(string);
        return false;
    }
    string[size] = '\0';
    *value = string;
    return true;
}

static bool decoder_response_status(
    CacheDecoder* decoder, UnityCompilerResponseStatus* status) {
    uint32_t compiler_success = 0;
    uint32_t diagnostic_count = 0;
    if (!decoder || !status ||
        !decoder_u32(decoder, &compiler_success) || compiler_success > 1U ||
        !decoder_u32(decoder, &diagnostic_count) ||
        diagnostic_count > USC_PREPROCESS_MAX_COUNT) {
        return false;
    }
    status->compiler_success = compiler_success != 0U;
    if (diagnostic_count > 0U) {
        status->diagnostics = (UnityCompilerDiagnostic*)calloc(
            diagnostic_count, sizeof(*status->diagnostics));
        if (!status->diagnostics) return false;
    }
    for (uint32_t i = 0; i < diagnostic_count; i++) {
        UnityCompilerDiagnostic* diagnostic = &status->diagnostics[i];
        uint32_t fields[3];
        if (!decoder_u32(decoder, &fields[0]) ||
            !decoder_u32(decoder, &fields[1]) ||
            !decoder_u32(decoder, &fields[2]) ||
            !decoder_string(decoder, &diagnostic->record) ||
            !decoder_string(decoder, &diagnostic->file) ||
            !decoder_string(decoder, &diagnostic->message)) {
            status->diagnostic_count = (size_t)i + 1U;
            return false;
        }
        diagnostic->fields[0] = (int32_t)fields[0];
        diagnostic->fields[1] = (int32_t)fields[1];
        diagnostic->fields[2] = (int32_t)fields[2];
        status->diagnostic_count = (size_t)i + 1U;
        if (!cache_diagnostic_is_valid(diagnostic)) return false;
    }
    return true;
}

static bool decoder_string_array(
    CacheDecoder* decoder, char*** values, int* count,
    bool allow_empty_strings) {
    *values = NULL;
    *count = 0;
    uint32_t encoded_count = 0;
    if (!decoder_u32(decoder, &encoded_count) ||
        encoded_count > USC_PREPROCESS_MAX_COUNT) {
        return false;
    }
    if (encoded_count == 0) return true;
    char** decoded = (char**)calloc(encoded_count, sizeof(*decoded));
    if (!decoded) return false;
    *values = decoded;
    *count = (int)encoded_count;
    for (uint32_t i = 0; i < encoded_count; i++) {
        if (!decoder_string(decoder, &decoded[i]) ||
            (!allow_empty_strings && decoded[i][0] == '\0')) {
            return false;
        }
    }
    return true;
}

static bool decoder_variant_set(
    CacheDecoder* decoder, SnippetKeywordVariantSet* set) {
    uint32_t present = 0;
    if (!decoder_u32(decoder, &present) || present > 1U) return false;
    if (present == 0U) return true;
    set->present = true;
    return decoder_string_array(
        decoder, &set->combinations, &set->combination_count, true);
}

static bool decoder_snippet_contract(
    CacheDecoder* decoder, SnippetCompileContract* contract) {
    uint32_t fields[13];
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!decoder_u32(decoder, &fields[i])) return false;
    }
    contract->snippet_id = (int32_t)fields[0];
    contract->platforms = (int32_t)fields[1];
    contract->quality_variants = (int32_t)fields[2];
    contract->program_types_mask = fields[3];
    contract->compilation_flags = fields[4];
    contract->language = (int32_t)fields[5];
    for (int i = 0; i < UNITY_SNIPPET_SOURCE_HASH_WORD_COUNT; i++) {
        contract->source_hash[i] = fields[6 + i];
    }
    contract->start_line = (int32_t)fields[10];
    contract->use_dxc_apis = (int32_t)fields[11];
    contract->never_use_dxc_apis = (int32_t)fields[12];
    uint32_t program_count = 0;
    if (!decoder_u32(decoder, &program_count) ||
        program_count > USC_PREPROCESS_MAX_COUNT) {
        return false;
    }
    if (program_count > 0) {
        contract->program_keyword_variants =
            (SnippetProgramKeywordVariants*)calloc(
                program_count,
                sizeof(*contract->program_keyword_variants));
        if (!contract->program_keyword_variants) return false;
    }
    contract->program_keyword_variant_count = (int)program_count;
    for (uint32_t i = 0; i < program_count; i++) {
        SnippetProgramKeywordVariants* program =
            &contract->program_keyword_variants[i];
        uint32_t compiler_program = 0;
        if (!decoder_u32(decoder, &compiler_program) ||
            compiler_program > INT32_MAX ||
            !decoder_variant_set(decoder, &program->user_global) ||
            !decoder_variant_set(decoder, &program->user_local) ||
            !decoder_variant_set(decoder, &program->builtin)) {
            return false;
        }
        program->compiler_program = (int32_t)compiler_program;
    }
    if (!decoder_string_array(
            decoder, &contract->non_stripped_user_keywords,
            &contract->non_stripped_user_keyword_count, false) ||
        !decoder_string_array(decoder, &contract->builtin_keywords,
                              &contract->builtin_keyword_count, false) ||
        !decoder_u64(decoder, &contract->requirements)) {
        return false;
    }

    uint32_t conditional_count = 0;
    if (!decoder_u32(decoder, &conditional_count) ||
        conditional_count > USC_PREPROCESS_MAX_COUNT) {
        return false;
    }
    if (conditional_count > 0) {
        contract->conditional_requirements =
            (ConditionalShaderRequirement*)calloc(
                conditional_count,
                sizeof(*contract->conditional_requirements));
        if (!contract->conditional_requirements) return false;
    }
    contract->conditional_requirement_count = (int)conditional_count;
    for (uint32_t i = 0; i < conditional_count; i++) {
        if (!decoder_string(
                decoder,
                &contract->conditional_requirements[i].keyword) ||
            contract->conditional_requirements[i].keyword[0] == '\0' ||
            !decoder_u64(
                decoder,
                &contract->conditional_requirements[i].requirements)) {
            return false;
        }
    }
    return unity_compiler_snippet_contract_validate(contract);
}

bool usc_cache_deserialize_preprocess_result(
    const uint8_t* data,
    size_t size,
    PreprocessResult* out_result) {
    static const uint8_t magic[8] = {
        'U', 'S', 'C', 'P', 'R', 'E', 'P', '4'
    };
    if (!data || !out_result) return false;
    memset(out_result, 0, sizeof(*out_result));
    CacheDecoder decoder = {.data = data, .size = size};
    uint8_t stored_magic[8];
    uint32_t version = 0;
    uint32_t reserved = 0;
    uint32_t snippet_count = 0;
    uint64_t blob_size64 = 0;
    if (!decoder_bytes(&decoder, stored_magic, sizeof(stored_magic)) ||
        memcmp(stored_magic, magic, sizeof(magic)) != 0 ||
        !decoder_u32(&decoder, &version) || version != 4U ||
        !decoder_u32(&decoder, &reserved) || reserved != 0U ||
        !decoder_u32(&decoder, &snippet_count) ||
        snippet_count > USC_PREPROCESS_MAX_COUNT ||
        !decoder_u64(&decoder, &blob_size64) || blob_size64 > SIZE_MAX ||
        blob_size64 > decoder.size - decoder.cursor) {
        return false;
    }

    if (blob_size64 > 0) {
        out_result->blob = (uint8_t*)malloc((size_t)blob_size64);
        if (!out_result->blob ||
            !decoder_bytes(&decoder, out_result->blob,
                           (size_t)blob_size64)) {
            unity_compiler_free_preprocess(out_result);
            return false;
        }
    }
    out_result->blob_len = (size_t)blob_size64;
    if (snippet_count > 0) {
        out_result->snippets = (PreprocessedSnippet*)calloc(
            snippet_count, sizeof(*out_result->snippets));
        if (!out_result->snippets) {
            unity_compiler_free_preprocess(out_result);
            return false;
        }
    }
    out_result->snippet_count = (int)snippet_count;

    bool ok = true;
    for (uint32_t i = 0; i < snippet_count && ok; i++) {
        PreprocessedSnippet* snippet = &out_result->snippets[i];
        uint32_t has_contract = 0;
        ok = decoder_string(&decoder, &snippet->source) &&
             decoder_u32(&decoder, &has_contract) && has_contract <= 1U;
        if (!ok) break;
        if (has_contract) {
            snippet->has_contract = true;
            ok = decoder_snippet_contract(&decoder, &snippet->contract);
            if (ok) {
                snippet->reqs = snippet->contract.requirements;
                snippet->language = snippet->contract.language;
                snippet->gpu_program_id = snippet->contract.snippet_id;
                snippet->conditional_requirements =
                    snippet->contract.conditional_requirements;
                snippet->conditional_requirement_count =
                    snippet->contract.conditional_requirement_count;
            }
        } else {
            uint32_t language = 0;
            uint32_t gpu_program_id = 0;
            uint32_t conditional_count = 0;
            ok = decoder_u64(&decoder, &snippet->reqs) &&
                 decoder_u32(&decoder, &language) &&
                 decoder_u32(&decoder, &gpu_program_id) &&
                 decoder_u32(&decoder, &conditional_count) &&
                 conditional_count <= USC_PREPROCESS_MAX_COUNT;
            if (!ok) break;
            snippet->language = (int32_t)language;
            snippet->gpu_program_id = (int32_t)gpu_program_id;
            if (conditional_count > 0) {
                snippet->conditional_requirements =
                    (ConditionalShaderRequirement*)calloc(
                        conditional_count,
                        sizeof(*snippet->conditional_requirements));
                if (!snippet->conditional_requirements) {
                    ok = false;
                    break;
                }
            }
            snippet->conditional_requirement_count =
                (int)conditional_count;
            for (uint32_t j = 0; j < conditional_count; j++) {
                ConditionalShaderRequirement* requirement =
                    &snippet->conditional_requirements[j];
                ok = decoder_string(&decoder, &requirement->keyword) &&
                     decoder_u64(&decoder, &requirement->requirements);
                if (!ok) break;
            }
        }
    }
    ok = ok && decoder.cursor == decoder.size;
    if (!ok) unity_compiler_free_preprocess(out_result);
    return ok;
}

bool usc_cache_serialize_binary_response(
    const UnityCompilerBinaryResponse* response,
    uint8_t** out_data,
    size_t* out_size) {
    static const uint8_t magic[8] = {
        'U', 'S', 'C', 'B', 'I', 'N', 'R', '2'
    };
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0U;
    if (!response || !out_data || !out_size ||
        !response->status.compiler_success ||
        !cache_response_status_is_valid(&response->status) ||
        response->reflection_record_count > USC_PREPROCESS_MAX_COUNT ||
        (response->reflection_record_count > 0U &&
         !response->reflection_records) ||
        (response->size > 0U && !response->data)) {
        return false;
    }
    for (size_t index = 0U;
         index < response->reflection_record_count; ++index) {
        const UnityCompilerReflectionRecord* record =
            &response->reflection_records[index];
        UnityCompilerReflectionRecord parsed;
        if (!record->record ||
            !unity_compiler_reflection_record_parse(
                record->record, &parsed)) {
            return false;
        }
        const bool exact = parsed.kind == record->kind &&
            parsed.value_count == record->value_count &&
            memcmp(parsed.values, record->values,
                   sizeof(parsed.values)) == 0 &&
            ((parsed.name == NULL && record->name == NULL) ||
             (parsed.name && record->name &&
              strcmp(parsed.name, record->name) == 0));
        unity_compiler_reflection_record_free(&parsed);
        if (!exact) return false;
    }
    CacheEncoder encoder = {.ok = true};
    encoder_bytes(&encoder, magic, sizeof(magic));
    encoder_u32(&encoder, 2U);
    encoder_u32(&encoder, 0U);
    encoder_response_status(&encoder, &response->status);
    encoder_u32(&encoder,
                (uint32_t)response->reflection_record_count);
    for (size_t index = 0U;
         index < response->reflection_record_count; ++index) {
        encoder_string(&encoder,
                       response->reflection_records[index].record);
    }
    encoder_u64(&encoder, (uint64_t)response->size);
    encoder_bytes(&encoder, response->data, response->size);
    if (!encoder.ok) {
        free(encoder.data);
        return false;
    }
    *out_data = encoder.data;
    *out_size = encoder.size;
    return true;
}

bool usc_cache_deserialize_binary_response(
    const uint8_t* data,
    size_t size,
    UnityCompilerBinaryResponse* out_response) {
    static const uint8_t magic[8] = {
        'U', 'S', 'C', 'B', 'I', 'N', 'R', '2'
    };
    if (!data || !out_response) return false;
    unity_compiler_binary_response_init(out_response);
    CacheDecoder decoder = {.data = data, .size = size};
    uint8_t stored_magic[8];
    uint32_t version = 0;
    uint32_t reserved = 0;
    uint32_t reflection_count = 0;
    uint64_t payload_size = 0;
    bool ok = decoder_bytes(&decoder, stored_magic, sizeof(stored_magic)) &&
              memcmp(stored_magic, magic, sizeof(magic)) == 0 &&
              decoder_u32(&decoder, &version) && version == 2U &&
              decoder_u32(&decoder, &reserved) && reserved == 0U &&
              decoder_response_status(&decoder, &out_response->status) &&
              out_response->status.compiler_success &&
              decoder_u32(&decoder, &reflection_count) &&
              reflection_count <= USC_PREPROCESS_MAX_COUNT;
    if (ok && reflection_count > 0U) {
        out_response->reflection_records =
            (UnityCompilerReflectionRecord*)calloc(
                reflection_count,
                sizeof(*out_response->reflection_records));
        ok = out_response->reflection_records != NULL;
    }
    for (uint32_t index = 0U; index < reflection_count && ok; ++index) {
        char* record = NULL;
        ok = decoder_string(&decoder, &record) &&
             unity_compiler_reflection_record_parse(
                 record, &out_response->reflection_records[index]);
        free(record);
        if (ok) out_response->reflection_record_count++;
    }
    if (ok) {
        ok = decoder_u64(&decoder, &payload_size) &&
             payload_size <= SIZE_MAX &&
             payload_size <= decoder.size - decoder.cursor;
    }
    if (ok && payload_size > 0U) {
        out_response->data = (uint8_t*)malloc((size_t)payload_size);
        ok = out_response->data &&
             decoder_bytes(&decoder, out_response->data,
                           (size_t)payload_size);
    } else if (ok) {
        ok = decoder_bytes(&decoder, NULL, 0U);
    }
    if (ok) {
        out_response->size = (size_t)payload_size;
        ok = decoder.cursor == decoder.size;
    }
    if (!ok) unity_compiler_binary_response_free(out_response);
    return ok;
}

bool usc_cache_serialize_preprocess_response(
    const UnityCompilerPreprocessResponse* response,
    uint8_t** out_data,
    size_t* out_size) {
    static const uint8_t magic[8] = {
        'U', 'S', 'C', 'P', 'R', 'S', 'P', '1'
    };
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0U;
    if (!response || !out_data || !out_size ||
        !response->status.compiler_success ||
        !cache_response_status_is_valid(&response->status)) {
        return false;
    }
    uint8_t* preprocess_data = NULL;
    size_t preprocess_size = 0U;
    if (!usc_cache_serialize_preprocess_result(
            &response->result, &preprocess_data, &preprocess_size)) {
        return false;
    }
    CacheEncoder encoder = {.ok = true};
    encoder_bytes(&encoder, magic, sizeof(magic));
    encoder_u32(&encoder, 1U);
    encoder_u32(&encoder, 0U);
    encoder_response_status(&encoder, &response->status);
    encoder_u64(&encoder, (uint64_t)preprocess_size);
    encoder_bytes(&encoder, preprocess_data, preprocess_size);
    free(preprocess_data);
    if (!encoder.ok) {
        free(encoder.data);
        return false;
    }
    *out_data = encoder.data;
    *out_size = encoder.size;
    return true;
}

bool usc_cache_deserialize_preprocess_response(
    const uint8_t* data,
    size_t size,
    UnityCompilerPreprocessResponse* out_response) {
    static const uint8_t magic[8] = {
        'U', 'S', 'C', 'P', 'R', 'S', 'P', '1'
    };
    if (!data || !out_response) return false;
    unity_compiler_preprocess_response_init(out_response);
    CacheDecoder decoder = {.data = data, .size = size};
    uint8_t stored_magic[8];
    uint32_t version = 0;
    uint32_t reserved = 0;
    uint64_t payload_size = 0;
    bool ok = decoder_bytes(&decoder, stored_magic, sizeof(stored_magic)) &&
              memcmp(stored_magic, magic, sizeof(magic)) == 0 &&
              decoder_u32(&decoder, &version) && version == 1U &&
              decoder_u32(&decoder, &reserved) && reserved == 0U &&
              decoder_response_status(&decoder, &out_response->status) &&
              out_response->status.compiler_success &&
              decoder_u64(&decoder, &payload_size) &&
              payload_size <= SIZE_MAX &&
              payload_size <= decoder.size - decoder.cursor;
    if (ok) {
        ok = usc_cache_deserialize_preprocess_result(
                 decoder.data + decoder.cursor, (size_t)payload_size,
                 &out_response->result) &&
             decoder_bytes(&decoder, NULL, (size_t)payload_size) &&
             decoder.cursor == decoder.size;
    }
    if (!ok) unity_compiler_preprocess_response_free(out_response);
    return ok;
}

static void digest_hex(
    const uint8_t digest[USC_CACHE_DIGEST_SIZE], char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < USC_CACHE_DIGEST_SIZE; i++) {
        hex[i * 2U] = digits[digest[i] >> 4U];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    hex[64] = '\0';
}

static size_t root_length(const char* cache_dir) {
    size_t length = strlen(cache_dir);
    while (length > 1U && cache_dir[length - 1U] == '/') length--;
    return length;
}

static char* make_shard_path(
    const char* cache_dir,
    const uint8_t request_digest[USC_CACHE_DIGEST_SIZE]) {
    char hex[65];
    digest_hex(request_digest, hex);
    size_t root_size = root_length(cache_dir);
    if (root_size > SIZE_MAX - 4U) return NULL;
    char* path = (char*)malloc(root_size + 1U + 2U + 1U);
    if (!path) return NULL;
    memcpy(path, cache_dir, root_size);
    path[root_size] = '/';
    path[root_size + 1U] = hex[0];
    path[root_size + 2U] = hex[1];
    path[root_size + 3U] = '\0';
    return path;
}

static char* make_entry_path(
    const char* cache_dir,
    const uint8_t request_digest[USC_CACHE_DIGEST_SIZE]) {
    char hex[65];
    digest_hex(request_digest, hex);
    size_t root_size = root_length(cache_dir);
    static const char suffix[] = ".usc-cache";
    const size_t extra_size = 1U + 2U + 1U + 62U + sizeof(suffix);
    if (root_size > SIZE_MAX - extra_size) return NULL;
    size_t total = root_size + extra_size;
    char* path = (char*)malloc(total);
    if (!path) return NULL;
    uint8_t* cursor = (uint8_t*)path;
    memcpy(cursor, cache_dir, root_size);
    cursor += root_size;
    *cursor++ = '/';
    *cursor++ = (uint8_t)hex[0];
    *cursor++ = (uint8_t)hex[1];
    *cursor++ = '/';
    memcpy(cursor, hex + 2, 62U);
    cursor += 62U;
    memcpy(cursor, suffix, sizeof(suffix));
    return path;
}

static bool ensure_one_directory(const char* path) {
    if (mkdir(path, 0700) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool ensure_directory_tree(const char* path) {
    char* copy = strdup(path);
    if (!copy) return false;
    size_t length = strlen(copy);
    bool ok = true;
    for (size_t i = 1; i < length; i++) {
        if (copy[i] != '/') continue;
        copy[i] = '\0';
        if (copy[0] != '\0' && !ensure_one_directory(copy)) ok = false;
        copy[i] = '/';
        if (!ok) break;
    }
    if (ok) ok = ensure_one_directory(copy);
    free(copy);
    return ok;
}

static bool read_exact(int fd, void* data, size_t size) {
    uint8_t* cursor = (uint8_t*)data;
    while (size > 0) {
        ssize_t count = read(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return true;
}

static bool write_exact(int fd, const void* data, size_t size) {
    const uint8_t* cursor = (const uint8_t*)data;
    while (size > 0) {
        ssize_t count = write(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return true;
}

typedef enum {
    CACHE_ENTRY_MISSING,
    CACHE_ENTRY_CORRUPT,
    CACHE_ENTRY_IDENTICAL,
    CACHE_ENTRY_DIFFERENT,
    CACHE_ENTRY_UNAVAILABLE,
} CacheEntryInspection;

static CacheEntryInspection inspect_cache_entry(
    const char* entry_path,
    const uint8_t request_digest[USC_CACHE_DIGEST_SIZE],
    const uint8_t* expected_data,
    size_t expected_size) {
    int fd = open(entry_path, O_RDONLY);
    if (fd < 0) {
        return errno == ENOENT ? CACHE_ENTRY_MISSING
                              : CACHE_ENTRY_UNAVAILABLE;
    }

    uint8_t header[USC_CACHE_HEADER_SIZE];
    struct stat status;
    bool malformed = fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
                     !read_exact(fd, header, sizeof(header));
    uint64_t stored_size = 0;
    if (!malformed) {
        stored_size = cache_read_le64(header + 88U);
        malformed =
            memcmp(header, USC_CACHE_MAGIC, sizeof(USC_CACHE_MAGIC)) != 0 ||
            cache_read_le32(header + 8U) != USC_CACHE_FORMAT_VERSION ||
            cache_read_le32(header + 12U) != USC_CACHE_HEADER_SIZE ||
            cache_read_le32(header + 16U) != USC_CACHE_SUCCESS ||
            cache_read_le32(header + 20U) != 0U ||
            memcmp(header + 24U, request_digest,
                   USC_CACHE_DIGEST_SIZE) != 0 ||
            stored_size > SIZE_MAX || stored_size > USC_CACHE_MAX_OUTPUT_SIZE ||
            stored_size > (uint64_t)INT64_MAX - USC_CACHE_HEADER_SIZE ||
            status.st_size != (off_t)(USC_CACHE_HEADER_SIZE + stored_size);
    }

    Sha256Context payload_context;
    sha256_init(&payload_context);
    bool identical = !malformed && stored_size == (uint64_t)expected_size;
    uint64_t cursor = 0;
    uint8_t buffer[64U * 1024U];
    while (!malformed && cursor < stored_size) {
        size_t chunk_size = stored_size - cursor > sizeof(buffer)
            ? sizeof(buffer)
            : (size_t)(stored_size - cursor);
        if (!read_exact(fd, buffer, chunk_size)) {
            malformed = true;
            break;
        }
        sha256_update(&payload_context, buffer, chunk_size);
        if (identical &&
            memcmp(buffer, expected_data + (size_t)cursor, chunk_size) != 0) {
            identical = false;
        }
        cursor += chunk_size;
    }

    uint8_t payload_digest[USC_CACHE_DIGEST_SIZE];
    if (!malformed) {
        sha256_final(&payload_context, payload_digest);
        malformed = memcmp(payload_digest, header + 56U,
                           USC_CACHE_DIGEST_SIZE) != 0;
    }
    if (close(fd) != 0 && !malformed) return CACHE_ENTRY_UNAVAILABLE;
    if (malformed) return CACHE_ENTRY_CORRUPT;
    return identical ? CACHE_ENTRY_IDENTICAL : CACHE_ENTRY_DIFFERENT;
}

static char* make_publish_lock_path(const char* shard_path) {
    static const char suffix[] = "/.publish.lock";
    size_t shard_size = strlen(shard_path);
    if (shard_size > SIZE_MAX - sizeof(suffix)) return NULL;
    char* path = (char*)malloc(shard_size + sizeof(suffix));
    if (!path) return NULL;
    memcpy(path, shard_path, shard_size);
    memcpy(path + shard_size, suffix, sizeof(suffix));
    return path;
}

static bool set_publish_lock(int fd, short lock_type) {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    for (;;) {
        if (fcntl(fd, F_SETLKW, &lock) == 0) return true;
        if (errno != EINTR) return false;
    }
}

static void note_miss(bool corrupt) {
    atomic_fetch_add_explicit(&g_cache_misses, 1U, memory_order_relaxed);
    if (corrupt) {
        atomic_fetch_add_explicit(
            &g_cache_corrupt_entries, 1U, memory_order_relaxed);
    }
}

UnityCompilerCacheLookup usc_cache_load(
    const char* cache_dir,
    const uint8_t request_digest[USC_CACHE_DIGEST_SIZE],
    uint8_t** out_data,
    size_t* out_size) {
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!cache_dir || !cache_dir[0] || !request_digest || !out_data ||
        !out_size) {
        return USC_CACHE_MISS;
    }
    char* path = make_entry_path(cache_dir, request_digest);
    if (!path) {
        note_miss(false);
        return USC_CACHE_MISS;
    }
    int fd = open(path, O_RDONLY);
    free(path);
    if (fd < 0) {
        note_miss(errno != ENOENT);
        return USC_CACHE_MISS;
    }

    uint8_t header[USC_CACHE_HEADER_SIZE];
    struct stat status;
    bool malformed = fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
                     !read_exact(fd, header, sizeof(header));
    uint64_t stored_size = 0;
    if (!malformed) {
        stored_size = cache_read_le64(header + 88U);
        malformed = memcmp(header, USC_CACHE_MAGIC, sizeof(USC_CACHE_MAGIC)) != 0 ||
                    cache_read_le32(header + 8U) !=
                        USC_CACHE_FORMAT_VERSION ||
                    cache_read_le32(header + 12U) != USC_CACHE_HEADER_SIZE ||
                    cache_read_le32(header + 16U) != USC_CACHE_SUCCESS ||
                    cache_read_le32(header + 20U) != 0U ||
                    memcmp(header + 24U, request_digest,
                           USC_CACHE_DIGEST_SIZE) != 0 ||
                    stored_size > SIZE_MAX ||
                    stored_size > USC_CACHE_MAX_OUTPUT_SIZE ||
                    stored_size > (uint64_t)INT64_MAX - USC_CACHE_HEADER_SIZE ||
                    status.st_size !=
                        (off_t)(USC_CACHE_HEADER_SIZE + stored_size);
    }

    uint8_t* data = NULL;
    bool unavailable = false;
    if (!malformed) {
        data = (uint8_t*)malloc(stored_size > 0 ? (size_t)stored_size : 1U);
        if (!data) {
            unavailable = true;
        } else if (stored_size > 0 &&
                   !read_exact(fd, data, (size_t)stored_size)) {
            malformed = true;
        }
    }
    if (close(fd) != 0 && !unavailable) malformed = true;
    if (!malformed && !unavailable) {
        uint8_t payload_digest[USC_CACHE_DIGEST_SIZE];
        sha256_bytes(data, (size_t)stored_size, payload_digest);
        malformed = memcmp(payload_digest, header + 56U,
                           USC_CACHE_DIGEST_SIZE) != 0;
    }
    if (malformed || unavailable) {
        free(data);
        note_miss(malformed);
        return USC_CACHE_MISS;
    }
    *out_data = data;
    *out_size = (size_t)stored_size;
    atomic_fetch_add_explicit(&g_cache_hits, 1U, memory_order_relaxed);
    return USC_CACHE_HIT;
}

bool usc_cache_store(
    const char* cache_dir,
    const uint8_t request_digest[USC_CACHE_DIGEST_SIZE],
    const uint8_t* data,
    size_t size) {
    if (!cache_dir || !cache_dir[0] || !request_digest ||
        (size > 0 && !data) || (uint64_t)size > USC_CACHE_MAX_OUTPUT_SIZE) {
        return false;
    }
    char* shard_path = make_shard_path(cache_dir, request_digest);
    char* entry_path = make_entry_path(cache_dir, request_digest);
    if (!shard_path || !entry_path || !ensure_directory_tree(shard_path)) {
        free(shard_path);
        free(entry_path);
        atomic_fetch_add_explicit(&g_cache_write_errors, 1U,
                                  memory_order_relaxed);
        return false;
    }

    uint8_t header[USC_CACHE_HEADER_SIZE] = {0};
    memcpy(header, USC_CACHE_MAGIC, sizeof(USC_CACHE_MAGIC));
    write_le32(header + 8U, USC_CACHE_FORMAT_VERSION);
    write_le32(header + 12U, USC_CACHE_HEADER_SIZE);
    write_le32(header + 16U, USC_CACHE_SUCCESS);
    memcpy(header + 24U, request_digest, USC_CACHE_DIGEST_SIZE);
    sha256_bytes(data, size, header + 56U);
    write_le64(header + 88U, (uint64_t)size);

    size_t entry_path_size = strlen(entry_path);
    size_t temporary_capacity = entry_path_size <= SIZE_MAX - 80U ?
                                entry_path_size + 80U : 0U;
    if (temporary_capacity == 0) {
        free(entry_path);
        free(shard_path);
        atomic_fetch_add_explicit(&g_cache_write_errors, 1U,
                                  memory_order_relaxed);
        return false;
    }
    char* temporary_path = (char*)malloc(temporary_capacity);
    int fd = -1;
    if (temporary_path) {
        temporary_path[0] = '\0';
        for (unsigned attempt = 0; attempt < 16U; attempt++) {
            uint64_t sequence = atomic_fetch_add_explicit(
                &g_temp_sequence, 1U, memory_order_relaxed);
            int count = snprintf(temporary_path, temporary_capacity,
                                 "%s.tmp.%ld.%llu", entry_path, (long)getpid(),
                                 (unsigned long long)sequence);
            if (count < 0 || (size_t)count >= temporary_capacity) break;
            fd = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0 || errno != EEXIST) break;
        }
    }

    bool temporary_ready =
        fd >= 0 && write_exact(fd, header, sizeof(header)) &&
        (size == 0 || write_exact(fd, data, size)) && fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) temporary_ready = false;

    bool ok = false;
    bool namespace_changed = false;
    char* lock_path = temporary_ready ? make_publish_lock_path(shard_path)
                                      : NULL;
    int lock_fd = lock_path
        ? open(lock_path, O_RDWR | O_CREAT, 0600)
        : -1;
    bool thread_locked = false;
    bool process_locked = false;
    if (temporary_ready && lock_fd >= 0 &&
        pthread_mutex_lock(&g_cache_publish_mutex) == 0) {
        thread_locked = true;
        process_locked = set_publish_lock(lock_fd, F_WRLCK);
    }

    if (process_locked) {
        for (unsigned attempt = 0; attempt < 8U; attempt++) {
            CacheEntryInspection inspection = inspect_cache_entry(
                entry_path, request_digest, data, size);
            if (inspection == CACHE_ENTRY_IDENTICAL) {
                ok = true;
                break;
            }
            if (inspection == CACHE_ENTRY_DIFFERENT) {
                char request_hex[65];
                digest_hex(request_digest, request_hex);
                fprintf(stderr,
                        "[CompilerCache] Rejected different payload for "
                        "existing request %s\n",
                        request_hex);
                break;
            }
            if (inspection == CACHE_ENTRY_UNAVAILABLE) break;
            if (inspection == CACHE_ENTRY_CORRUPT) {
                if (unlink(entry_path) == 0) {
                    namespace_changed = true;
                    continue;
                }
                if (errno == ENOENT) continue;
                break;
            }

            if (link(temporary_path, entry_path) == 0) {
                namespace_changed = true;
                ok = true;
                break;
            }
            if (errno != EEXIST) break;
        }
    }

    if (process_locked) (void)set_publish_lock(lock_fd, F_UNLCK);
    if (thread_locked) pthread_mutex_unlock(&g_cache_publish_mutex);
    if (lock_fd >= 0) close(lock_fd);
    free(lock_path);
    if (temporary_path && temporary_path[0]) unlink(temporary_path);

    if (namespace_changed) {
        int directory_fd = open(shard_path, O_RDONLY);
        if (directory_fd >= 0) {
            (void)fsync(directory_fd);
            close(directory_fd);
        }
    }
    if (ok) {
        atomic_fetch_add_explicit(&g_cache_stores, 1U, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_cache_write_errors, 1U,
                                  memory_order_relaxed);
    }
    free(temporary_path);
    free(entry_path);
    free(shard_path);
    return ok;
}

void unity_compiler_cache_get_stats(UnityCompilerCacheStats* out_stats) {
    if (!out_stats) return;
    out_stats->hits = atomic_load_explicit(&g_cache_hits, memory_order_relaxed);
    out_stats->misses = atomic_load_explicit(&g_cache_misses, memory_order_relaxed);
    out_stats->stores = atomic_load_explicit(&g_cache_stores, memory_order_relaxed);
    out_stats->corrupt_entries = atomic_load_explicit(
        &g_cache_corrupt_entries, memory_order_relaxed);
    out_stats->write_errors = atomic_load_explicit(
        &g_cache_write_errors, memory_order_relaxed);
}

void unity_compiler_cache_reset_stats(void) {
    atomic_store_explicit(&g_cache_hits, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_cache_misses, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_cache_stores, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_cache_corrupt_entries, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_cache_write_errors, 0U, memory_order_relaxed);
}
