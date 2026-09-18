#include "common/executable_resource.h"
#include "common/file_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int check_resource(const char* executable, const char* relative,
                          const char* marker) {
    char* path = common_executable_resource_find(executable, relative);
    CHECK(path != NULL);
    CommonFileBytes file = {0};
    CHECK(common_file_read_regular_terminated(path, 1024U * 1024U, &file) ==
          COMMON_FILE_OK);
    CHECK(file.data != NULL);
    CHECK(strstr((const char*)file.data, marker) != NULL);
    common_file_bytes_dispose(&file);
    free(path);
    return 0;
}

int main(int argc, char** argv) {
    CHECK(argc > 0 && argv && argv[0]);
    CHECK(check_resource(
        argv[0],
        "dxbc-sandbox/unity/Editor/DXBCShaderImportGate.cs",
        "DXBCShaderImportGate") == 0);
    CHECK(check_resource(
        argv[0],
        "dxbc-sandbox/unity/Editor/DXBCShaderBundleGate.cs",
        "DXBCShaderBundleGate") == 0);
    CHECK(check_resource(
        argv[0],
        "dxbc-sandbox/unity/Editor/DXBCFiniteVisualGate.cs",
        "DXBCFiniteVisualGate") == 0);
    CHECK(common_executable_resource_find(argv[0], "../escape") == NULL);
    CHECK(common_executable_resource_find(argv[0], "one/../../escape") ==
          NULL);
    CHECK(common_executable_resource_find(argv[0], "/absolute") == NULL);
    CHECK(common_executable_resource_find(argv[0], "missing/resource") ==
          NULL);
    return 0;
}
