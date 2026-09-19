// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_include_scan.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    const char *const *expected;
    size_t count;
    size_t visited;
} Paths;

static bool visit(void *context, const char *path) {
    Paths *paths = context;
    if (paths->visited == paths->count || strcmp(path, paths->expected[paths->visited]))
        return false;
    ++paths->visited;
    return true;
}

int main(void) {
    const char *expected[] = {"A.inc",         "nested\\B.cginc",    "inactive.inc",
                              "continued.inc", "angle//literal.inc", "block/*literal*/.inc"};
    Paths paths = {expected, sizeof(expected) / sizeof(*expected), 0};
    const char *source = "\xef\xbb\xbf/* ignore #include MACRO */\r\n"
                         "# /*gap*/ include /* across\nlines */ \"A.inc\" // trailing\n"
                         "#include_with_pragmas \"nested\\B.cginc\"\r"
                         "#if 0\n#include <inactive.inc>\n#endif\n"
                         "// continued comment \\\n#include MUST_NOT_VISIT\n"
                         "#inc\\\r\nlude \\\n\"continued.inc\"\n"
                         "const char* ignored = \"#include FAKE\";\n"
                         "#include <angle//literal.inc>\n"
                         "#include <block/*literal*/.inc>\n"
                         "// harmless comment (??"
                         "?)\n";
    CHECK(usc_include_scan((const uint8_t *)source, strlen(source), visit, &paths) ==
          USC_INCLUDE_SCAN_OK);
    CHECK(paths.visited == paths.count);

    const char *rejected[] = {
        "#pragma surface surf Lambert\n",
        "#pragma include_alias(\"A.inc\", \"B.inc\")\n",
        "%:include \"A.inc\"\n",
        "#include SOME_MACRO\n",
        "#include \"A\" \"B\"",
        "#include \"\"",
        "#include <>",
        "#include <unterminated",
        "#include \"unterminated\n",
        "#include_next \"A\"",
        "#import \"A\"",
        "/* unterminated",
        "#include <\"path\"> trailing",
        "#include \"trailing\\\"",
        ("?"
         "?/\n#include \"A\""),
        "#define X 1\xef\xbb\xbf\n#include \"A\"",
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        paths.visited = 0;
        CHECK(usc_include_scan((const uint8_t *)rejected[i], strlen(rejected[i]), visit, &paths) ==
              USC_INCLUDE_SCAN_UNSUPPORTED);
        CHECK(paths.visited == 0);
    }
    const uint8_t nul_source[] = {'#', 0, '\n'};
    CHECK(usc_include_scan(nul_source, sizeof(nul_source), visit, &paths) ==
          USC_INCLUDE_SCAN_UNSUPPORTED);
    CHECK(usc_include_scan(NULL, 0, visit, &paths) == USC_INCLUDE_SCAN_OK);
    CHECK(usc_include_scan(NULL, 1, visit, &paths) == USC_INCLUDE_SCAN_UNSUPPORTED);
    CHECK(usc_include_scan((const uint8_t *)"", SIZE_MAX, visit, &paths) ==
          USC_INCLUDE_SCAN_UNSUPPORTED);
    CHECK(usc_include_scan(NULL, 0, NULL, &paths) == USC_INCLUDE_SCAN_UNSUPPORTED);
    paths.count = 0;
    const char *include = "#include \"A.inc\"";
    CHECK(usc_include_scan((const uint8_t *)include, strlen(include), visit, &paths) ==
          USC_INCLUDE_SCAN_VISITOR_FAILED);
    return 0;
}
