// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_include_closure.h"
#include "compiler/unity_include_scan.h"
#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

enum {
    MAX_FILES = 8192,
    MAX_PATHS = 65536,
    MAX_FILE_BYTES = 16 * 1024 * 1024,
    MAX_CLOSURE_BYTES = 64 * 1024 * 1024
};

typedef struct {
    char *lexical_parent;
    char *physical_parent;
    uint8_t digest[USC_CACHE_DIGEST_SIZE];
    uint8_t *bytes;
    size_t size;
} IncludeFile;

typedef struct {
    const char *const *roots;
    size_t root_count;
    char **paths;
    size_t path_count;
    IncludeFile *files;
    size_t file_count;
    size_t total_bytes;
    const char *lexical_parent;
    const char *physical_parent;
    UscCacheToolchainLease *lease;
    CommonSha256Context hash;
} IncludeClosure;

static void hash_bytes(IncludeClosure *closure, const void *bytes, size_t size) {
    uint8_t length[8];
    for (size_t i = 0; i < sizeof(length); ++i)
        length[i] = (uint8_t)((uint64_t)size >> (i * 8U));
    common_sha256_update(&closure->hash, length, sizeof(length));
    common_sha256_update(&closure->hash, bytes, size);
}

static char *parent_path(const char *path) {
    const char *last = strrchr(path, '/');
    if (!last)
        return NULL;
    const size_t size = last == path ? 1U : (size_t)(last - path);
    char *parent = malloc(size + 1U);
    if (parent) {
        memcpy(parent, path, size);
        parent[size] = 0;
    }
    return parent;
}

/* Unity's newer preprocessor flattens relative components before opening.
 * Its older HLSL include handler also needs the unflattened OS pathname.
 * Capture both candidates; physical and lexical parent contexts are kept
 * separately so directory symlinks followed by '..' cannot hide a dependency. */
static char *flatten_path(const char *path) {
    if (!path || path[0] != '/')
        return NULL;
    const size_t size = strlen(path);
    char *result = malloc(size + 2U);
    if (!result)
        return NULL;
    size_t used = 1U, at = 1U;
    result[0] = '/';
    while (at < size) {
        while (path[at] == '/')
            ++at;
        const size_t begin = at;
        while (path[at] && path[at] != '/')
            ++at;
        const size_t length = at - begin;
        if (!length || (length == 1U && path[begin] == '.'))
            continue;
        if (length == 2U && !memcmp(path + begin, "..", 2U)) {
            while (used > 1U && result[used - 1U] != '/')
                --used;
            if (used > 1U)
                --used;
        } else {
            if (used > 1U)
                result[used++] = '/';
            memcpy(result + used, path + begin, length);
            used += length;
        }
    }
    result[used] = 0;
    return result;
}

static bool capture_path(IncludeClosure *closure, const char *path) {
    for (size_t i = 0; i < closure->path_count; ++i)
        if (!strcmp(path, closure->paths[i]))
            return true;
    if (closure->path_count == MAX_PATHS)
        return false;
    char *retained_path = strdup(path);
    char **paths = realloc(closure->paths, (closure->path_count + 1U) * sizeof(*paths));
    if (!retained_path || !paths) {
        free(retained_path);
        if (paths)
            closure->paths = paths;
        return false;
    }
    closure->paths = paths;
    paths[closure->path_count++] = retained_path;
    uint8_t digest[USC_CACHE_DIGEST_SIZE];
    uint8_t *bytes = NULL;
    size_t size = 0;
    bool missing = false;
    UscCacheToolchainLease *lease = NULL;
    if (!usc_cache_include_file_lease_create(path, MAX_FILE_BYTES, &bytes, &size, digest, &missing,
                                             &lease))
        return false;
    const bool merged = usc_cache_toolchain_lease_merge(&closure->lease, &lease);
    usc_cache_toolchain_lease_destroy(lease);
    if (!merged) {
        free(bytes);
        return false;
    }
    hash_bytes(closure, path, strlen(path));
    const uint8_t kind = missing ? 0U : 1U;
    hash_bytes(closure, &kind, sizeof(kind));
    if (missing)
        return true;
    hash_bytes(closure, digest, sizeof(digest));
    char *raw_parent = parent_path(path);
    char *lexical = raw_parent ? flatten_path(raw_parent) : NULL;
    char *physical = raw_parent ? realpath(raw_parent, NULL) : NULL;
    free(raw_parent);
    if (!lexical || !physical) {
        free(lexical);
        free(physical);
        free(bytes);
        return false;
    }
    /* Equal contents in equal parent contexts have equal literal dependency
     * expansion. Keep both pathname observations, but scan that context once. */
    for (size_t i = 0; i < closure->file_count; ++i) {
        const IncludeFile *file = &closure->files[i];
        if (!memcmp(file->digest, digest, sizeof(digest)) &&
            !strcmp(file->lexical_parent, lexical) && !strcmp(file->physical_parent, physical)) {
            free(lexical);
            free(physical);
            free(bytes);
            return true;
        }
    }
    if (closure->file_count == MAX_FILES || size > MAX_CLOSURE_BYTES - closure->total_bytes) {
        free(lexical);
        free(physical);
        free(bytes);
        return false;
    }
    IncludeFile *files = realloc(closure->files, (closure->file_count + 1U) * sizeof(*files));
    if (!files) {
        free(lexical);
        free(physical);
        free(bytes);
        return false;
    }
    closure->files = files;
    IncludeFile *file = &files[closure->file_count++];
    *file = (IncludeFile){
        .lexical_parent = lexical, .physical_parent = physical, .bytes = bytes, .size = size};
    memcpy(file->digest, digest, sizeof(digest));
    closure->total_bytes += size;
    return true;
}

static bool capture_candidate(IncludeClosure *closure, const char *parent, const char *name) {
    const size_t parent_size = parent ? strlen(parent) : 0U, name_size = strlen(name);
    if (parent_size > SIZE_MAX - name_size - 2U)
        return false;
    char *raw = malloc(parent_size + name_size + 2U);
    if (!raw)
        return false;
    if (parent_size)
        memcpy(raw, parent, parent_size);
    size_t at = parent_size;
    if (parent_size)
        raw[at++] = '/';
    memcpy(raw + at, name, name_size + 1U);
    for (char *c = raw; *c; ++c)
        if (*c == '\\')
            *c = '/';
    char *flat = flatten_path(raw);
    bool ok = flat && capture_path(closure, flat);
    if (ok && strcmp(raw, flat))
        ok = capture_path(closure, raw);
    free(flat);
    free(raw);
    return ok;
}

static bool capture_include(void *context, const char *name) {
    IncludeClosure *closure = context;
    if (closure->lexical_parent && !capture_candidate(closure, closure->lexical_parent, name))
        return false;
    if (closure->physical_parent && !capture_candidate(closure, closure->physical_parent, name))
        return false;
    for (size_t i = 0; i < closure->root_count; ++i)
        if (!capture_candidate(closure, closure->roots[i], name))
            return false;
    return (name[0] != '/' && name[0] != '\\') || capture_candidate(closure, NULL, name);
}

static void dispose_closure(IncludeClosure *closure) {
    for (size_t i = 0; i < closure->path_count; ++i)
        free(closure->paths[i]);
    free(closure->paths);
    for (size_t i = 0; i < closure->file_count; ++i) {
        free(closure->files[i].lexical_parent);
        free(closure->files[i].physical_parent);
        free(closure->files[i].bytes);
    }
    free(closure->files);
    usc_cache_toolchain_lease_destroy(closure->lease);
}

bool usc_include_closure_create(const uint8_t *source, size_t source_size, const char *const *roots,
                                size_t root_count,
                                const uint8_t environment_digest[USC_CACHE_DIGEST_SIZE],
                                uint8_t closure_environment_digest[USC_CACHE_DIGEST_SIZE],
                                UscCacheToolchainLease **out_lease) {
    uint8_t base[USC_CACHE_DIGEST_SIZE] = {0};
    if (environment_digest)
        memcpy(base, environment_digest, sizeof(base));
    if (closure_environment_digest)
        memset(closure_environment_digest, 0, sizeof(base));
    if (out_lease)
        *out_lease = NULL;
    if (!closure_environment_digest || !out_lease || !environment_digest ||
        (!source && source_size) || source_size > MAX_CLOSURE_BYTES || !roots || !root_count ||
        root_count > 64U)
        return false;
    for (size_t i = 0; i < root_count; ++i)
        if (!roots[i] || roots[i][0] != '/')
            return false;
    IncludeClosure closure = {.roots = roots, .root_count = root_count, .total_bytes = source_size};
    common_sha256_init(&closure.hash);
    static const char schema[] = "DXBCSandbox.UnityCompiler.literal-include-closure.v1";
    hash_bytes(&closure, schema, sizeof(schema) - 1U);
    hash_bytes(&closure, base, sizeof(base));
    uint8_t encoded_root_count[8];
    for (size_t i = 0; i < sizeof(encoded_root_count); ++i)
        encoded_root_count[i] = (uint8_t)((uint64_t)root_count >> (i * 8U));
    hash_bytes(&closure, encoded_root_count, sizeof(encoded_root_count));
    for (size_t i = 0; i < root_count; ++i)
        hash_bytes(&closure, roots[i], strlen(roots[i]));
    bool ok =
        capture_include(&closure, "HLSLSupport.cginc") &&
        capture_include(&closure, "UnityShaderVariables.cginc") &&
        usc_include_scan(source, source_size, capture_include, &closure) == USC_INCLUDE_SCAN_OK;
    for (size_t i = 0; ok && i < closure.file_count; ++i) {
        closure.lexical_parent = closure.files[i].lexical_parent;
        closure.physical_parent = closure.files[i].physical_parent;
        /* Visiting can grow the file array; do not retain a pointer into it. */
        uint8_t *bytes = closure.files[i].bytes;
        const size_t size = closure.files[i].size;
        ok = usc_include_scan(bytes, size, capture_include, &closure) == USC_INCLUDE_SCAN_OK;
        free(bytes);
        closure.files[i].bytes = NULL;
    }
    ok = ok && usc_cache_toolchain_lease_validate(closure.lease);
    if (ok) {
        common_sha256_final(&closure.hash, closure_environment_digest);
        *out_lease = closure.lease;
        closure.lease = NULL;
    }
    dispose_closure(&closure);
    return ok;
}
