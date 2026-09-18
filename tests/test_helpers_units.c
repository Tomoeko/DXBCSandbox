// SPDX-License-Identifier: GPL-3.0-only

#define _XOPEN_SOURCE 700
#include "test_helpers.h"

int main(void) {
    char expected[PATH_MAX];
    char actual[PATH_MAX];
    if (!realpath(DXBC_TEST_SOURCE_ROOT, expected)) return 1;
    if (!find_repo_root_from_anchor(DXBC_TEST_SOURCE_ROOT, actual,
                                    sizeof(actual)) ||
        strcmp(actual, expected) != 0) return 2;
    if (!find_repo_root_from_anchor(__FILE__, actual, sizeof(actual)) ||
        strcmp(actual, expected) != 0) return 3;
    char tiny[1] = {'!'};
    if (find_repo_root_from_anchor(__FILE__, tiny, sizeof(tiny)) ||
        tiny[0] != '!') return 4;
    if (find_repo_root_from_anchor(__FILE__, NULL, 1)) return 5;
    find_repo_root_from_path(NULL, 0, __FILE__);
    return 0;
}
