#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <stdbool.h>
#include <sys/stat.h>

static inline bool find_repo_root_from_anchor(const char* anchor,
                                              char* out_path,
                                              size_t max_len) {
    if (!anchor || !anchor[0] || !out_path || max_len == 0) return false;

    char temp[PATH_MAX];
    if (!realpath(anchor, temp)) return false;

    struct stat anchor_stat;
    if (stat(temp, &anchor_stat) == 0 && !S_ISDIR(anchor_stat.st_mode)) {
        char* parent = dirname(temp);
        if (!parent) return false;
        memmove(temp, parent, strlen(parent) + 1);
    }

    for (;;) {
        char sandbox_path[PATH_MAX];
        int sandbox_len = snprintf(sandbox_path, sizeof(sandbox_path),
                                   "%s/src/compiler/unity_compiler_client.c", temp);
        if (sandbox_len > 0 &&
            (size_t)sandbox_len < sizeof(sandbox_path) &&
            access(sandbox_path, F_OK) == 0) {
            size_t length = strlen(temp);
            if (length >= max_len) return false;
            memcpy(out_path, temp, length + 1U);
            return true;
        }

        if (strcmp(temp, "/") == 0) break;
        char parent_path[PATH_MAX];
        int parent_len = snprintf(parent_path, sizeof(parent_path), "%s/..",
                                  temp);
        if (parent_len <= 0 || (size_t)parent_len >= sizeof(parent_path) ||
            !realpath(parent_path, temp)) {
            break;
        }
    }
    return false;
}

static inline void find_repo_root_from_path(char* out_path, size_t max_len,
                                            const char* anchor) {
    if (!out_path || max_len == 0) return;
    if (find_repo_root_from_anchor(anchor, out_path, max_len)) return;

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL &&
        find_repo_root_from_anchor(cwd, out_path, max_len)) {
        return;
    }

    if (find_repo_root_from_anchor(__FILE__, out_path, max_len)) return;

    // Fallback to relative path
    strncpy(out_path, ".", max_len);
    out_path[max_len - 1] = '\0';
}

#endif // TEST_HELPERS_H
