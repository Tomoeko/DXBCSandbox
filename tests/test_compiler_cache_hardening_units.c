#include "compiler/unity_compiler_cache.h"
#include "compiler/unity_include_closure.h"
#include "compiler/unity_compiler_client.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define RACE_PROCESS_COUNT 8

static bool write_file(const char* path, const void* data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    const uint8_t* cursor = (const uint8_t*)data;
    while (size > 0) {
        ssize_t written = write(fd, cursor, size);
        if (written <= 0) {
            close(fd);
            return false;
        }
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return close(fd) == 0;
}

static void remove_tree(const char* path) {
    struct stat status;
    if (lstat(path, &status) != 0) return;
    if (!S_ISDIR(status.st_mode)) {
        unlink(path);
        return;
    }
    DIR* directory = opendir(path);
    if (!directory) {
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        int length = snprintf(child, sizeof(child), "%s/%s", path,
                              entry->d_name);
        if (length > 0 && (size_t)length < sizeof(child)) remove_tree(child);
    }
    closedir(directory);
    rmdir(path);
}

static bool environment_fingerprint(
    const char* project_root,
    const char* explicit_includes,
    const char* sandbox_includes,
    uint8_t digest[USC_CACHE_DIGEST_SIZE]) {
    return usc_cache_environment_fingerprint(
        project_root, explicit_includes, sandbox_includes,
        NULL, NULL, NULL, NULL, NULL, NULL, digest);
}

static bool verify_environment_fingerprint_hardening(
    const char* temporary_dir) {
    char environment_root[PATH_MAX];
    char explicit_includes[PATH_MAX];
    char explicit_alias[PATH_MAX];
    char sandbox_includes[PATH_MAX];
    char opaque_include[PATH_MAX];
    char target_a[PATH_MAX];
    char target_b[PATH_MAX];
    char target_a_file[PATH_MAX];
    char target_b_file[PATH_MAX];
    char linked_directory[PATH_MAX];
    char cycle_directory[PATH_MAX];
    char cycle_link[PATH_MAX];
    char fifo_path[PATH_MAX];
    char compiler_target[PATH_MAX];
    char compiler_link[PATH_MAX];
    char added_include[PATH_MAX];
    CHECK(snprintf(environment_root, sizeof(environment_root), "%s/env",
                   temporary_dir) > 0);
    CHECK(snprintf(explicit_includes, sizeof(explicit_includes),
                   "%s/explicit", environment_root) > 0);
    CHECK(snprintf(explicit_alias, sizeof(explicit_alias), "%s/explicit-alias",
                   environment_root) > 0);
    CHECK(snprintf(sandbox_includes, sizeof(sandbox_includes), "%s/sandbox",
                   environment_root) > 0);
    CHECK(snprintf(opaque_include, sizeof(opaque_include), "%s/no_extension",
                   sandbox_includes) > 0);
    CHECK(snprintf(target_a, sizeof(target_a), "%s/target-a",
                   environment_root) > 0);
    CHECK(snprintf(target_b, sizeof(target_b), "%s/target-b",
                   environment_root) > 0);
    CHECK(snprintf(target_a_file, sizeof(target_a_file), "%s/payload.custom",
                   target_a) > 0);
    CHECK(snprintf(target_b_file, sizeof(target_b_file), "%s/payload.custom",
                   target_b) > 0);
    CHECK(snprintf(linked_directory, sizeof(linked_directory), "%s/linked",
                   explicit_includes) > 0);
    CHECK(snprintf(cycle_directory, sizeof(cycle_directory), "%s/cycle",
                   explicit_includes) > 0);
    CHECK(snprintf(cycle_link, sizeof(cycle_link), "%s/up",
                   cycle_directory) > 0);
    CHECK(snprintf(fifo_path, sizeof(fifo_path), "%s/unstable-input",
                   explicit_includes) > 0);
    CHECK(snprintf(compiler_target, sizeof(compiler_target), "%s/compiler",
                   environment_root) > 0);
    CHECK(snprintf(compiler_link, sizeof(compiler_link), "%s/compiler-link",
                   environment_root) > 0);
    CHECK(snprintf(added_include, sizeof(added_include), "%s/added.data",
                   explicit_includes) > 0);

    CHECK(mkdir(environment_root, 0700) == 0);
    CHECK(mkdir(explicit_includes, 0700) == 0);
    CHECK(mkdir(sandbox_includes, 0700) == 0);
    CHECK(mkdir(target_a, 0700) == 0);
    CHECK(mkdir(target_b, 0700) == 0);
    CHECK(write_file(opaque_include, "opaque-a", 8U));
    CHECK(write_file(target_a_file, "target-a", 8U));
    CHECK(write_file(target_b_file, "target-b", 8U));
    CHECK(symlink("../target-a", linked_directory) == 0);

    uint8_t digest_a[USC_CACHE_DIGEST_SIZE];
    uint8_t digest_b[USC_CACHE_DIGEST_SIZE];
    CHECK(environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_a));
    CHECK(environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_b));
    CHECK(memcmp(digest_a, digest_b, sizeof(digest_a)) == 0);

    /* Arbitrary include names are compiler-visible; an extension allowlist
     * must not permit a stale digest. */
    CHECK(write_file(opaque_include, "opaque-b", 8U));
    CHECK(environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_b));
    CHECK(memcmp(digest_a, digest_b, sizeof(digest_a)) != 0);
    memcpy(digest_a, digest_b, sizeof(digest_a));

    /* A symlink contributes both its spelling and its resolved target bytes. */
    CHECK(write_file(target_a_file, "target-c", 8U));
    CHECK(environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_b));
    CHECK(memcmp(digest_a, digest_b, sizeof(digest_a)) != 0);
    memcpy(digest_a, digest_b, sizeof(digest_a));
    CHECK(write_file(target_b_file, "target-c", 8U));
    CHECK(unlink(linked_directory) == 0);
    CHECK(symlink("../target-b", linked_directory) == 0);
    CHECK(environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_b));
    CHECK(memcmp(digest_a, digest_b, sizeof(digest_a)) != 0);

    /* Root path identity remains part of the canonical environment. */
    CHECK(symlink("explicit", explicit_alias) == 0);
    CHECK(environment_fingerprint(
        environment_root, explicit_alias, sandbox_includes, digest_a));
    CHECK(memcmp(digest_a, digest_b, sizeof(digest_a)) != 0);

    /* Broken links, ancestor cycles, and special nodes cannot be represented
     * as deterministic byte trees, so fingerprinting rejects them. */
    CHECK(unlink(linked_directory) == 0);
    CHECK(symlink("../missing-target", linked_directory) == 0);
    CHECK(!environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_a));
    CHECK(unlink(linked_directory) == 0);
    CHECK(symlink("../target-b", linked_directory) == 0);
    CHECK(mkdir(cycle_directory, 0700) == 0);
    CHECK(symlink("..", cycle_link) == 0);
    CHECK(!environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_a));
    CHECK(unlink(cycle_link) == 0);
    CHECK(rmdir(cycle_directory) == 0);
    CHECK(mkfifo(fifo_path, 0600) == 0);
    CHECK(!environment_fingerprint(
        environment_root, explicit_includes, sandbox_includes, digest_a));
    CHECK(unlink(fifo_path) == 0);

    /* The standalone compiler fingerprint follows a stable regular-file
     * target and changes when freshly recomputed after target mutation. */
    CHECK(write_file(compiler_target, "compiler-a", 10U));
    CHECK(symlink("compiler", compiler_link) == 0);
    CHECK(usc_cache_sha256_file(compiler_target, digest_a));
    CHECK(usc_cache_sha256_file(compiler_link, digest_b));
    CHECK(memcmp(digest_a, digest_b, sizeof(digest_a)) == 0);
    CHECK(write_file(compiler_target, "compiler-b", 10U));
    CHECK(usc_cache_sha256_file(compiler_link, digest_b));
    CHECK(memcmp(digest_a, digest_b, sizeof(digest_a)) != 0);

    UscCacheToolchainPaths paths = {
        .compiler_path = compiler_link,
        .project_root = environment_root,
        .includes_dir = explicit_includes,
        .sandbox_includes_dir = sandbox_includes,
    };
    UscCacheToolchainLease* lease = NULL;
    uint8_t compiler_digest[USC_CACHE_DIGEST_SIZE];
    uint8_t environment_digest[USC_CACHE_DIGEST_SIZE];
    CHECK(usc_cache_toolchain_lease_create(
        &paths, compiler_digest, environment_digest, &lease));
    CHECK(lease != NULL && usc_cache_toolchain_lease_validate(lease));
    CHECK(write_file(opaque_include, "opaque-c", 8U));
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);

    lease = NULL;
    CHECK(usc_cache_toolchain_lease_create(
        &paths, compiler_digest, environment_digest, &lease));
    CHECK(write_file(added_include, "new", 3U));
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);
    CHECK(unlink(added_include) == 0);

    lease = NULL;
    CHECK(usc_cache_toolchain_lease_create(
        &paths, compiler_digest, environment_digest, &lease));
    CHECK(unlink(linked_directory) == 0);
    CHECK(symlink("../target-a", linked_directory) == 0);
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);
    CHECK(unlink(linked_directory) == 0);
    CHECK(symlink("../target-b", linked_directory) == 0);

    lease = NULL;
    CHECK(usc_cache_toolchain_lease_create(
        &paths, compiler_digest, environment_digest, &lease));
    CHECK(write_file(compiler_target, "compiler-c", 10U));
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);
    usc_cache_toolchain_lease_destroy(NULL);

    CHECK(unlink(compiler_target) == 0);
    CHECK(!usc_cache_sha256_file(compiler_link, digest_a));
    memset(compiler_digest, 0xa5, sizeof(compiler_digest));
    memset(environment_digest, 0xa5, sizeof(environment_digest));
    lease = (UscCacheToolchainLease*)(uintptr_t)1U;
    CHECK(!usc_cache_toolchain_lease_create(
        &paths, compiler_digest, environment_digest, &lease));
    CHECK(lease == NULL);
    uint8_t zero_digest[USC_CACHE_DIGEST_SIZE] = {0};
    CHECK(memcmp(compiler_digest, zero_digest, sizeof(zero_digest)) == 0);
    CHECK(memcmp(environment_digest, zero_digest, sizeof(zero_digest)) == 0);
    return true;
}

static bool verify_request_search_roots(const char* temporary_dir) {
    char root[PATH_MAX], header[PATH_MAX], alias[PATH_MAX], absent[PATH_MAX];
    CHECK(snprintf(root, sizeof(root), "%s/source", temporary_dir) > 0);
    CHECK(snprintf(header, sizeof(header), "%s/UnityShaderVariables.cginc", root) > 0);
    CHECK(snprintf(alias, sizeof(alias), "%s/source-alias", temporary_dir) > 0);
    CHECK(snprintf(absent, sizeof(absent), "%s/future-source", temporary_dir) > 0);
    CHECK(mkdir(root, 0700) == 0);
    const char* roots[] = {root, absent};
    uint8_t base[32] = {1}, first[32], second[32];
    UscCacheToolchainLease* lease = NULL;
    CHECK(usc_cache_search_roots_lease_create(roots, 2, base, first, &lease));
    CHECK(usc_cache_toolchain_lease_validate(lease));
    /* A newly added source-local header invalidates an otherwise unchanged
     * configured Unity include environment. */
    CHECK(write_file(header, "#error shadow\n", 14));
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);
    CHECK(usc_cache_search_roots_lease_create(roots, 2, base, second, &lease));
    CHECK(memcmp(first, second, 32) != 0);
    memcpy(first, second, 32);
    CHECK(write_file(header, "#error change\n", 14));
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);
    CHECK(usc_cache_search_roots_lease_create(roots, 2, base, second, &lease));
    CHECK(memcmp(first, second, 32) != 0);
    memcpy(first, second, 32);
    CHECK(mkdir(absent, 0700) == 0);
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);
    CHECK(usc_cache_search_roots_lease_create(roots, 2, base, second, &lease));
    CHECK(memcmp(first, second, 32) != 0);
    memcpy(first, second, 32);
    usc_cache_toolchain_lease_destroy(lease);
    /* Stable snapshots are deterministic, including in-place extension. */
    memcpy(second, base, 32);
    CHECK(usc_cache_search_roots_lease_create(roots, 2, second, second, &lease));
    CHECK(memcmp(first, second, 32) == 0);
    usc_cache_toolchain_lease_destroy(lease);
    const char* reversed[] = {absent, root};
    CHECK(usc_cache_search_roots_lease_create(reversed, 2, base, second, &lease));
    CHECK(memcmp(first, second, 32) != 0);
    usc_cache_toolchain_lease_destroy(lease);
    CHECK(symlink("source", alias) == 0);
    const char* linked[] = {alias};
    CHECK(usc_cache_search_roots_lease_create(linked, 1, base, first, &lease));
    CHECK(unlink(alias) == 0 && symlink("future-source", alias) == 0);
    CHECK(!usc_cache_toolchain_lease_validate(lease));
    usc_cache_toolchain_lease_destroy(lease);
    CHECK(usc_cache_search_roots_lease_create(linked, 1, base, second, &lease));
    CHECK(memcmp(first, second, 32) != 0);
    usc_cache_toolchain_lease_destroy(lease);
    const char* relative[] = {"source"};
    lease = (UscCacheToolchainLease*)(uintptr_t)1;
    CHECK(!usc_cache_search_roots_lease_create(relative, 1, base, second, &lease));
    CHECK(lease == NULL);
    uint8_t zero[32] = {0};
    CHECK(memcmp(second, zero, 32) == 0);
    return true;
}

static void cache_entry_path(
    const char* cache_dir,
    const uint8_t digest[USC_CACHE_DIGEST_SIZE],
    char path[PATH_MAX]) {
    static const char digits[] = "0123456789abcdef";
    char hex[USC_CACHE_DIGEST_SIZE * 2U + 1U];
    for (size_t i = 0; i < USC_CACHE_DIGEST_SIZE; i++) {
        hex[i * 2U] = digits[digest[i] >> 4U];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    hex[USC_CACHE_DIGEST_SIZE * 2U] = '\0';
    (void)snprintf(path, PATH_MAX, "%s/%c%c/%s.usc-cache", cache_dir,
                   hex[0], hex[1], hex + 2);
}

static bool verify_preprocess_shape_validation(void) {
    uint8_t* encoded = (uint8_t*)(uintptr_t)1U;
    size_t encoded_size = 99U;
    PreprocessResult malformed = {.snippet_count = 1};
    CHECK(!usc_cache_serialize_preprocess_result(
        &malformed, &encoded, &encoded_size));
    CHECK(encoded == NULL && encoded_size == 0U);

    uint8_t byte = 0;
    malformed.snippet_count = 0;
    malformed.blob_len = 1U;
    CHECK(!usc_cache_serialize_preprocess_result(
        &malformed, &encoded, &encoded_size));

    PreprocessedSnippet snippet = {
        .source = "source",
        .conditional_requirement_count = 1,
    };
    malformed.snippets = &snippet;
    malformed.snippet_count = 1;
    malformed.blob = &byte;
    malformed.blob_len = 1U;
    CHECK(!usc_cache_serialize_preprocess_result(
        &malformed, &encoded, &encoded_size));

    ConditionalShaderRequirement requirement = {0};
    snippet.conditional_requirements = &requirement;
    CHECK(!usc_cache_serialize_preprocess_result(
        &malformed, &encoded, &encoded_size));

    PreprocessResult empty = {0};
    CHECK(usc_cache_serialize_preprocess_result(
        &empty, &encoded, &encoded_size));
    CHECK(encoded != NULL && encoded_size > 0U);
    free(encoded);
    return true;
}

static bool load_equals(const char* cache_dir, const uint8_t digest[32],
                        const uint8_t* left, const uint8_t* right,
                        size_t size) {
    uint8_t* loaded = NULL;
    size_t loaded_size = 0;
    CHECK(usc_cache_load(cache_dir, digest, &loaded, &loaded_size) ==
          USC_CACHE_HIT);
    CHECK(loaded_size == size);
    bool matches_left = memcmp(loaded, left, size) == 0;
    bool matches_right = memcmp(loaded, right, size) == 0;
    free(loaded);
    CHECK(matches_left || matches_right);
    return true;
}

static bool run_competing_publishers(
    const char* cache_dir,
    const uint8_t digest[USC_CACHE_DIGEST_SIZE],
    const uint8_t* payload_a,
    const uint8_t* payload_b,
    size_t payload_size) {
    int gate[2];
    CHECK(pipe(gate) == 0);
    pid_t children[RACE_PROCESS_COUNT];
    for (int i = 0; i < RACE_PROCESS_COUNT; i++) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) {
            close(gate[1]);
            uint8_t token = 0;
            ssize_t received;
            do {
                received = read(gate[0], &token, 1U);
            } while (received < 0);
            close(gate[0]);
            if (received != 1) _exit(2);
            const uint8_t* payload = (i & 1) ? payload_b : payload_a;
            _exit(usc_cache_store(cache_dir, digest, payload, payload_size)
                      ? 0
                      : 1);
        }
    }
    close(gate[0]);
    for (int i = 0; i < RACE_PROCESS_COUNT; i++) {
        uint8_t token = (uint8_t)i;
        CHECK(write(gate[1], &token, 1U) == 1);
    }
    CHECK(close(gate[1]) == 0);

    int successes = 0;
    int conflicts = 0;
    for (int i = 0; i < RACE_PROCESS_COUNT; i++) {
        int status = 0;
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status));
        if (WEXITSTATUS(status) == 0) successes++;
        else if (WEXITSTATUS(status) == 1) conflicts++;
        else CHECK(false);
    }
    CHECK(successes == RACE_PROCESS_COUNT / 2);
    CHECK(conflicts == RACE_PROCESS_COUNT / 2);
    return load_equals(cache_dir, digest, payload_a, payload_b, payload_size);
}

static bool verify_collision_safe_publication(const char* cache_dir) {
    static const uint8_t payload_a[] = {0x00, 0x11, 0x22, 0x33, 0xff};
    static const uint8_t payload_b[] = {0xff, 0x33, 0x22, 0x11, 0x00};
    uint8_t digest[USC_CACHE_DIGEST_SIZE];
    memset(digest, 0x11, sizeof(digest));

    unity_compiler_cache_reset_stats();
    CHECK(usc_cache_store(cache_dir, digest, payload_a, sizeof(payload_a)));
    CHECK(usc_cache_store(cache_dir, digest, payload_a, sizeof(payload_a)));
    CHECK(!usc_cache_store(cache_dir, digest, payload_b, sizeof(payload_b)));
    CHECK(load_equals(cache_dir, digest, payload_a, payload_a,
                      sizeof(payload_a)));
    UnityCompilerCacheStats stats;
    unity_compiler_cache_get_stats(&stats);
    CHECK(stats.stores == 2U && stats.write_errors == 1U);

    uint8_t absent_race_digest[USC_CACHE_DIGEST_SIZE];
    memset(absent_race_digest, 0x22, sizeof(absent_race_digest));
    CHECK(run_competing_publishers(
        cache_dir, absent_race_digest, payload_a, payload_b,
        sizeof(payload_a)));

    uint8_t repair_race_digest[USC_CACHE_DIGEST_SIZE];
    memset(repair_race_digest, 0x33, sizeof(repair_race_digest));
    CHECK(usc_cache_store(cache_dir, repair_race_digest, payload_a,
                          sizeof(payload_a)));
    char entry_path[PATH_MAX];
    cache_entry_path(cache_dir, repair_race_digest, entry_path);
    static const uint8_t corrupt[] = {'b', 'a', 'd'};
    CHECK(write_file(entry_path, corrupt, sizeof(corrupt)));
    CHECK(run_competing_publishers(
        cache_dir, repair_race_digest, payload_a, payload_b,
        sizeof(payload_a)));
    return true;
}

static bool verify_include_file_leases(const char* temporary_dir) {
    char header[PATH_MAX], absent[PATH_MAX], link[PATH_MAX], empty[PATH_MAX];
    CHECK(snprintf(header, sizeof(header), "%s/lease.inc", temporary_dir) > 0);
    CHECK(snprintf(absent, sizeof(absent), "%s/absent.inc", temporary_dir) > 0);
    CHECK(snprintf(link, sizeof(link), "%s/alias.inc", temporary_dir) > 0);
    CHECK(snprintf(empty, sizeof(empty), "%s/empty.inc", temporary_dir) > 0);
    const uint8_t contents[] = {1, 0, 3};
    CHECK(write_file(header, contents, sizeof(contents)));
    CHECK(write_file(empty, NULL, 0));
    CHECK(symlink("lease.inc", link) == 0);
    uint8_t* bytes = NULL;
    size_t size = 0;
    uint8_t digest[32], reference[32];
    bool missing = false;
    UscCacheToolchainLease *all = NULL, *addition = NULL;
    CHECK(usc_cache_sha256_file(header, reference));
    const char* paths[] = {header, header, link};
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); ++i) {
        CHECK(usc_cache_include_file_lease_create(
            paths[i], sizeof(contents), &bytes, &size, digest, &missing, &addition));
        CHECK(!missing && size == sizeof(contents) && !memcmp(bytes, contents, size) &&
              bytes[size] == 0 && !memcmp(digest, reference, sizeof(digest)));
        free(bytes);
        CHECK(usc_cache_toolchain_lease_merge(&all, &addition));
        CHECK(all && !addition && usc_cache_toolchain_lease_validate(all));
    }
    CHECK(!usc_cache_toolchain_lease_merge(&all, &all));
    CHECK(usc_cache_include_file_lease_create(
        empty, 0, &bytes, &size, digest, &missing, &addition));
    CHECK(!missing && bytes && !bytes[0] && size == 0);
    free(bytes);
    CHECK(usc_cache_toolchain_lease_merge(&all, &addition));
    CHECK(usc_cache_include_file_lease_create(
        absent, 0, &bytes, &size, digest, &missing, &addition));
    CHECK(missing && !bytes && size == 0);
    CHECK(usc_cache_toolchain_lease_merge(&all, &addition));
    CHECK(write_file(absent, contents, sizeof(contents)));
    CHECK(!usc_cache_toolchain_lease_validate(all));
    CHECK(usc_cache_include_file_lease_create(
        absent, sizeof(contents), &bytes, &size, digest, &missing, &addition));
    free(bytes);
    CHECK(!usc_cache_toolchain_lease_merge(&all, &addition));
    CHECK(all && addition && usc_cache_toolchain_lease_validate(addition));
    usc_cache_toolchain_lease_destroy(all);
    usc_cache_toolchain_lease_destroy(addition);
    all = addition = NULL;
    CHECK(usc_cache_include_file_lease_create(
        link, sizeof(contents), &bytes, &size, digest, &missing, &all));
    free(bytes);
    CHECK(write_file(header, "new", 3));
    CHECK(!usc_cache_toolchain_lease_validate(all));
    usc_cache_toolchain_lease_destroy(all);
    all = NULL;
    CHECK(!usc_cache_include_file_lease_create(
        header, 2, &bytes, &size, digest, &missing, &all));
    CHECK(!bytes && size == 0 && !all && !missing);
    CHECK(!usc_cache_include_file_lease_create(
        temporary_dir, SIZE_MAX, &bytes, &size, digest, &missing, &all));
    CHECK(!usc_cache_include_file_lease_create(
        "relative.inc", SIZE_MAX, &bytes, &size, digest, &missing, &all));
    CHECK(!usc_cache_include_file_lease_create(NULL, 0, NULL, NULL, NULL, NULL, NULL));
    return true;
}

static bool verify_include_closure(const char* temporary_dir) {
    char source_root[PATH_MAX], outside[PATH_MAX], cwd[PATH_MAX];
    char entry[PATH_MAX], dependency[PATH_MAX], missing_header[PATH_MAX];
    CHECK(snprintf(source_root, sizeof(source_root), "%s/closure-source", temporary_dir) > 0);
    CHECK(snprintf(outside, sizeof(outside), "%s/closure-outside", temporary_dir) > 0);
    CHECK(snprintf(cwd, sizeof(cwd), "%s/closure-working", temporary_dir) > 0);
    CHECK(mkdir(source_root, 0700) == 0 && mkdir(outside, 0700) == 0 && mkdir(cwd, 0700) == 0);
    CHECK(snprintf(entry, sizeof(entry), "%s/entry.inc", source_root) > 0);
    CHECK(snprintf(dependency, sizeof(dependency), "%s/external.inc", outside) > 0);
    CHECK(snprintf(missing_header, sizeof(missing_header), "%s/missing.inc", source_root) > 0);
    const char contents[] = "#if SOMETIMES\n#include \"../closure-outside/external.inc\"\n#endif\n"
                            "#include \"missing.inc\"\n";
    const char cycle[] = "#include \"./external.inc\"\n";
    CHECK(write_file(entry, contents, sizeof(contents) - 1U));
    CHECK(write_file(dependency, cycle, sizeof(cycle) - 1U));
    const char source[] = "#include \"entry.inc\"\n";
    const char* roots[] = {source_root, cwd};
    uint8_t base[32] = {1}, first[32], second[32];
    UscCacheToolchainLease *original = NULL, *current = NULL;
    CHECK(usc_include_closure_create((const uint8_t*)source, sizeof(source) - 1U,
        roots, 2, base, first, &original));
    CHECK(usc_cache_toolchain_lease_contains_path(original, dependency, true));
    CHECK(usc_cache_toolchain_lease_contains_path(original, missing_header, false));
    CHECK(!usc_cache_toolchain_lease_contains_path(original, missing_header, true));
    CHECK(usc_include_closure_create((const uint8_t*)source, sizeof(source) - 1U,
        roots, 2, base, second, &current));
    CHECK(!memcmp(first, second, sizeof(first)));
    usc_cache_toolchain_lease_destroy(current);
    CHECK(write_file(dependency, "// changed\n", 11));
    CHECK(!usc_cache_toolchain_lease_validate(original));
    usc_cache_toolchain_lease_destroy(original);
    CHECK(usc_include_closure_create((const uint8_t*)source, sizeof(source) - 1U,
        roots, 2, base, second, &current));
    CHECK(memcmp(first, second, sizeof(first)) != 0);
    memcpy(first, second, sizeof(first));
    CHECK(write_file(missing_header, "// newly present\n", 17));
    CHECK(!usc_cache_toolchain_lease_validate(current));
    usc_cache_toolchain_lease_destroy(current);
    CHECK(usc_include_closure_create((const uint8_t*)source, sizeof(source) - 1U,
        roots, 2, base, second, &current));
    CHECK(memcmp(first, second, sizeof(first)) != 0);
    usc_cache_toolchain_lease_destroy(current);
    const char macro[] = "#include HEADER_MACRO\n";
    CHECK(!usc_include_closure_create((const uint8_t*)macro, sizeof(macro) - 1U,
        roots, 2, base, second, &current));
    CHECK(current == NULL);
    uint8_t zero[32] = {0};
    CHECK(!memcmp(second, zero, sizeof(zero)));

    char deep[PATH_MAX], alias[PATH_MAX], lexical_choice[PATH_MAX], physical_choice[PATH_MAX];
    CHECK(snprintf(deep, sizeof(deep), "%s/deep", outside) > 0);
    CHECK(snprintf(alias, sizeof(alias), "%s/alias", source_root) > 0);
    CHECK(snprintf(lexical_choice, sizeof(lexical_choice), "%s/choice.inc", source_root) > 0);
    CHECK(snprintf(physical_choice, sizeof(physical_choice), "%s/choice.inc", outside) > 0);
    CHECK(mkdir(deep, 0700) == 0 && symlink(deep, alias) == 0);
    CHECK(write_file(lexical_choice, "// lexical\n", 11));
    CHECK(write_file(physical_choice, "// physical\n", 12));
    const char ambiguous[] = "#include \"alias/../choice.inc\"\n";
    CHECK(usc_include_closure_create((const uint8_t*)ambiguous, sizeof(ambiguous) - 1U,
        roots, 2, base, first, &original));
    CHECK(usc_cache_toolchain_lease_contains_path(original, lexical_choice, true));
    CHECK(write_file(physical_choice, "// changed physical\n", 20));
    CHECK(!usc_cache_toolchain_lease_validate(original));
    usc_cache_toolchain_lease_destroy(original);
    CHECK(usc_include_closure_create((const uint8_t*)ambiguous, sizeof(ambiguous) - 1U,
        roots, 2, base, second, &current));
    CHECK(memcmp(first, second, sizeof(first)) != 0);
    CHECK(write_file(lexical_choice, "// changed lexical\n", 19));
    CHECK(!usc_cache_toolchain_lease_validate(current));
    usc_cache_toolchain_lease_destroy(current);
    /* The same hard-linked header can acquire a different nested-include
     * parent when an ancestor directory symlink moves. File inode checks alone
     * cannot detect that change after a symlink followed by '..'. */
    char moved[PATH_MAX], moved_deep[PATH_MAX], moved_choice[PATH_MAX];
    CHECK(snprintf(moved, sizeof(moved), "%s/closure-moved", temporary_dir) > 0);
    CHECK(snprintf(moved_deep, sizeof(moved_deep), "%s/deep", moved) > 0);
    CHECK(snprintf(moved_choice, sizeof(moved_choice), "%s/choice.inc", moved) > 0);
    CHECK(mkdir(moved, 0700) == 0 && mkdir(moved_deep, 0700) == 0);
    const char nested[] = "#include \"child.inc\"\n";
    CHECK(write_file(physical_choice, nested, sizeof(nested) - 1U));
    CHECK(link(physical_choice, moved_choice) == 0);
    CHECK(usc_include_closure_create((const uint8_t*)ambiguous, sizeof(ambiguous) - 1U,
        roots, 2, base, first, &original));
    CHECK(unlink(alias) == 0 && symlink(moved_deep, alias) == 0);
    CHECK(!usc_cache_toolchain_lease_validate(original));
    usc_cache_toolchain_lease_destroy(original);
    return true;
}

static bool run_all_tests(void) {
    CHECK(verify_preprocess_shape_validation());
    char temporary_template[] = "/tmp/dxbc-cache-hardening.XXXXXX";
    char* temporary_dir = mkdtemp(temporary_template);
    CHECK(temporary_dir != NULL);
    bool ok = verify_collision_safe_publication(temporary_dir) &&
              verify_environment_fingerprint_hardening(temporary_dir) &&
              verify_request_search_roots(temporary_dir) &&
              verify_include_file_leases(temporary_dir) &&
              verify_include_closure(temporary_dir);
    remove_tree(temporary_dir);
    CHECK(ok);
    return true;
}

int main(void) {
    if (!run_all_tests()) return 1;
    puts("compiler cache hardening unit tests passed");
    return 0;
}
