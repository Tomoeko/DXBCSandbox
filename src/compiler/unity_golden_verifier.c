// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/sha256.h"
#include "common/string_builder.h"
#include "compiler/unity_compiler_broker.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_parser.h"
#include "dxbc/usbd.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_lift_transaction.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DXBC_GOLDEN_DEFAULT_DIR
#define DXBC_GOLDEN_DEFAULT_DIR "tests/golden"
#endif

#ifndef DXBC_GOLDEN_DEFAULT_PROJECT_ROOT
#define DXBC_GOLDEN_DEFAULT_PROJECT_ROOT "."
#endif

#ifndef DXBC_GOLDEN_DEFAULT_INCLUDES
#define DXBC_GOLDEN_DEFAULT_INCLUDES ""
#endif

#ifndef DXBC_GOLDEN_TEST_FIXTURE
#define DXBC_GOLDEN_TEST_FIXTURE "tests/golden/unlit_color/target.bin"
#endif

#define GOLDEN_MAX_FLAGS_SIZE (64U * 1024U)
#define GOLDEN_MAX_SOURCE_SIZE (64U * 1024U * 1024U)
#define GOLDEN_MAX_BUNDLE_SIZE (512U * 1024U * 1024U)
#define GOLDEN_MAX_CASES 10000U
#define GOLDEN_MAX_RECORDS 1024U
#define GOLDEN_MAX_LIST_ITEMS 1048576U

static const HLSLLiftLimits k_copy_limits = {
    .max_candidates = 64, .max_compiles = 33, .max_elapsed_ms = 30000
};

typedef struct {
    char** values;
    size_t count;
    size_t capacity;
} StringList;

typedef struct {
    bool all_stages;
    bool all_variants;
    int explicit_stage;
    uint64_t requirements;
    char* shader_name;
    StringList keywords;
    StringList defines;
} GoldenFlags;

typedef struct {
    const char* name;
    int id;
    const char* define_prefix;
    const char* pragma_name;
    const char* fallback_entry;
} GoldenStage;

static const GoldenStage k_stages[] = {
    {"vertex", 0, "#define VERTEX 1\n#define SHADER_STAGE_VERTEX 1\n",
     "vertex", "vert"},
    {"fragment", 1,
     "#define FRAGMENT 1\n#define SHADER_STAGE_FRAGMENT 1\n", "fragment",
     "frag"},
    {"hull", 2, "#define HULL 1\n#define SHADER_STAGE_HULL 1\n",
     "hull", "hs"},
    {"domain", 3, "#define DOMAIN 1\n#define SHADER_STAGE_DOMAIN 1\n",
     "domain", "ds"},
    {"geometry", 4,
     "#define GEOMETRY 1\n#define SHADER_STAGE_GEOMETRY 1\n",
     "geometry", "gs"},
};

typedef struct {
    char* name;
    const uint8_t* bytecode;
    size_t bytecode_size;
} GoldenRecord;

typedef struct {
    GoldenRecord* records;
    size_t count;
} GoldenBundle;

typedef struct {
    size_t program_index;
    size_t variant_index;
    const GoldenStage* stage;
    uint64_t blob_index;
    uint64_t requirements;
    /* -1 is the legacy manifest without tier authority; 0..2 are explicit
     * tiers and 3 is the serialized generic/shared tier group. */
    int hardware_tier_group;
    StringList keywords;
} VariantManifest;

typedef struct {
    VariantManifest* values;
    size_t count;
    size_t capacity;
} VariantManifestList;

typedef struct {
    char* record_name;
    const GoldenStage* stage;
    const char* program;
    uint64_t requirements;
    char** keywords;
    size_t keyword_count;
    char** defines;
    size_t define_count;
} CompileJob;

typedef struct {
    CompileJob* values;
    size_t count;
    size_t capacity;
} CompileJobList;

typedef struct {
    const char* golden_dir;
    const char* project_root;
    const char* includes_dir;
    const char* unity_contents;
    const char* case_name;
    bool self_test;
    bool self_test_lifts;
    bool reconstruct;
    bool lift_copies;
    bool high_level;
    const char* report_path;
} CommandLine;

static void set_error(char* error, size_t error_size, const char* format, ...) {
    if (!error || error_size == 0U) return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

static bool checked_add_size(size_t left, size_t right, size_t* result) {
    if (!result || right > SIZE_MAX - left) return false;
    *result = left + right;
    return true;
}

static char* duplicate_span(const char* value, size_t length) {
    if (!value || length == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(length + 1U);
    if (!copy) return NULL;
    memcpy(copy, value, length);
    copy[length] = '\0';
    return copy;
}

static bool string_list_append_owned(StringList* list, char* value) {
    if (!list || !value || list->count >= GOLDEN_MAX_LIST_ITEMS) return false;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0U ? 8U : list->capacity * 2U;
        if (capacity < list->capacity || capacity > GOLDEN_MAX_LIST_ITEMS ||
            capacity > SIZE_MAX / sizeof(*list->values)) {
            return false;
        }
        char** resized =
            (char**)realloc(list->values, capacity * sizeof(*list->values));
        if (!resized) return false;
        list->values = resized;
        list->capacity = capacity;
    }
    list->values[list->count++] = value;
    return true;
}

static void string_list_free(StringList* list) {
    if (!list) return;
    for (size_t index = 0; index < list->count; ++index) {
        free(list->values[index]);
    }
    free(list->values);
    memset(list, 0, sizeof(*list));
}

static bool tokenize_flags(const char* text, StringList* tokens, char* error,
                           size_t error_size) {
    if (!text || !tokens) return false;
    memset(tokens, 0, sizeof(*tokens));
    size_t at = 0U;
    while (text[at] != '\0') {
        while (isspace((unsigned char)text[at])) ++at;
        if (text[at] == '\0') break;

        StringBuilder token;
        sb_init(&token);
        bool started = false;
        char quote = '\0';
        while (text[at] != '\0') {
            char value = text[at];
            if (quote == '\0' && isspace((unsigned char)value)) break;
            started = true;
            if (value == '\\') {
                ++at;
                if (text[at] == '\0') {
                    set_error(error, error_size,
                              "trailing escape in flags record");
                    sb_free(&token);
                    string_list_free(tokens);
                    return false;
                }
                sb_append_char(&token, text[at++]);
                continue;
            }
            if (value == '\'' || value == '"') {
                if (quote == '\0') {
                    quote = value;
                    ++at;
                    continue;
                }
                if (quote == value) {
                    quote = '\0';
                    ++at;
                    continue;
                }
            }
            sb_append_char(&token, value);
            ++at;
        }
        if (quote != '\0') {
            set_error(error, error_size, "unterminated quote in flags record");
            sb_free(&token);
            string_list_free(tokens);
            return false;
        }
        if (!started || !sb_ok(&token)) {
            set_error(error, error_size, "could not allocate flags token");
            sb_free(&token);
            string_list_free(tokens);
            return false;
        }
        char* value = token.buf ? sb_detach(&token) : strdup("");
        sb_free(&token);
        if (!value || !string_list_append_owned(tokens, value)) {
            free(value);
            set_error(error, error_size, "could not allocate flags list");
            string_list_free(tokens);
            return false;
        }
    }
    return true;
}

static bool parse_u64_decimal(const char* text, uint64_t* value) {
    if (!text || !value || text[0] == '\0' || text[0] == '-' ||
        text[0] == '+') {
        return false;
    }
    errno = 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool string_equal_ignore_case(const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right) {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static bool parse_stage_value(const char* text, bool* all_stages,
                              int* explicit_stage) {
    if (!text || !all_stages || !explicit_stage) return false;
    if (string_equal_ignore_case(text, "all")) {
        *all_stages = true;
        *explicit_stage = -1;
        return true;
    }
    static const char* aliases[][3] = {
        {"vertex", "vert", "0"},
        {"fragment", "frag", "1"},
        {"hull", "hs", "2"},
        {"domain", "ds", "3"},
        {"geometry", "gs", "4"},
    };
    for (size_t stage = 0U; stage < sizeof(aliases) / sizeof(aliases[0]);
         ++stage) {
        for (size_t alias = 0U; alias < 3U; ++alias) {
            if (string_equal_ignore_case(text, aliases[stage][alias])) {
                *all_stages = false;
                *explicit_stage = (int)stage;
                return true;
            }
        }
    }
    return false;
}

static bool split_comma_list(const char* text, StringList* values,
                             char* error, size_t error_size) {
    if (!text || !values) return false;
    memset(values, 0, sizeof(*values));
    const char* start = text;
    for (;;) {
        const char* comma = strchr(start, ',');
        size_t length = comma ? (size_t)(comma - start) : strlen(start);
        if (length != 0U) {
            char* value = duplicate_span(start, length);
            if (!value || !string_list_append_owned(values, value)) {
                free(value);
                set_error(error, error_size,
                          "could not allocate comma-separated flags value");
                string_list_free(values);
                return false;
            }
        }
        if (!comma) return true;
        start = comma + 1;
    }
}

static void golden_flags_free(GoldenFlags* flags) {
    if (!flags) return;
    free(flags->shader_name);
    string_list_free(&flags->keywords);
    string_list_free(&flags->defines);
    memset(flags, 0, sizeof(*flags));
}

static bool parse_golden_flags(const char* text, GoldenFlags* flags,
                               char* error, size_t error_size) {
    if (!text || !flags) return false;
    memset(flags, 0, sizeof(*flags));
    flags->all_stages = true;
    flags->explicit_stage = -1;
    flags->shader_name = strdup("MyShader");
    if (!flags->shader_name) return false;

    StringList tokens;
    if (!tokenize_flags(text, &tokens, error, error_size)) {
        golden_flags_free(flags);
        return false;
    }
    bool saw_stage = false;
    bool saw_requirements = false;
    bool saw_keywords = false;
    bool saw_defines = false;
    bool saw_shader_name = false;
    bool saw_all_variants = false;
    bool ok = true;
    for (size_t index = 0U; index < tokens.count && ok; ++index) {
        const char* option = tokens.values[index];
        if (strcmp(option, "-all-variants") == 0 ||
            strcmp(option, "--all-variants") == 0) {
            if (saw_all_variants) {
                set_error(error, error_size,
                          "duplicate all-variants option");
                ok = false;
            }
            flags->all_variants = true;
            saw_all_variants = true;
            continue;
        }
        if (index + 1U >= tokens.count) {
            set_error(error, error_size, "missing value after %s", option);
            ok = false;
            break;
        }
        const char* value = tokens.values[++index];
        if (strcmp(option, "-stage") == 0 ||
            strcmp(option, "--stage") == 0) {
            if (saw_stage || !parse_stage_value(
                                 value, &flags->all_stages,
                                 &flags->explicit_stage)) {
                set_error(error, error_size, "invalid or duplicate stage: %s",
                          value);
                ok = false;
            }
            saw_stage = true;
        } else if (strcmp(option, "-reqs") == 0 ||
                   strcmp(option, "--reqs") == 0) {
            if (saw_requirements ||
                !parse_u64_decimal(value, &flags->requirements)) {
                set_error(error, error_size,
                          "invalid or duplicate requirements: %s", value);
                ok = false;
            }
            saw_requirements = true;
        } else if (strcmp(option, "-keywords") == 0 ||
                   strcmp(option, "--keywords") == 0) {
            if (saw_keywords || !split_comma_list(
                                    value, &flags->keywords, error,
                                    error_size)) {
                if (saw_keywords) {
                    set_error(error, error_size,
                              "duplicate keywords option");
                }
                ok = false;
            }
            saw_keywords = true;
        } else if (strcmp(option, "-defines") == 0 ||
                   strcmp(option, "--defines") == 0) {
            if (saw_defines || !split_comma_list(
                                   value, &flags->defines, error,
                                   error_size)) {
                if (saw_defines) {
                    set_error(error, error_size, "duplicate defines option");
                }
                ok = false;
            }
            saw_defines = true;
        } else if (strcmp(option, "-shader-name") == 0 ||
                   strcmp(option, "--shader-name") == 0) {
            if (saw_shader_name || value[0] == '\0') {
                set_error(error, error_size,
                          "invalid or duplicate shader name");
                ok = false;
            } else {
                char* copy = strdup(value);
                if (!copy) {
                    set_error(error, error_size,
                              "could not allocate shader name");
                    ok = false;
                } else {
                    free(flags->shader_name);
                    flags->shader_name = copy;
                }
            }
            saw_shader_name = true;
        } else {
            set_error(error, error_size, "unknown flags option: %s", option);
            ok = false;
        }
    }
    string_list_free(&tokens);
    if (!ok) golden_flags_free(flags);
    return ok;
}

static char* path_join(const char* parent, const char* child) {
    if (!parent || !child) return NULL;
    size_t parent_size = strlen(parent);
    size_t child_size = strlen(child);
    bool separator = parent_size != 0U && parent[parent_size - 1U] != '/';
    size_t size = 0U;
    if (!checked_add_size(parent_size, separator ? 1U : 0U, &size) ||
        !checked_add_size(size, child_size, &size) || size == SIZE_MAX) {
        return NULL;
    }
    char* path = (char*)malloc(size + 1U);
    if (!path) return NULL;
    memcpy(path, parent, parent_size);
    size_t at = parent_size;
    if (separator) path[at++] = '/';
    memcpy(path + at, child, child_size);
    path[size] = '\0';
    return path;
}


static bool is_directory_path(const char* path) {
    struct stat info;
    return path && lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool read_text_file(const char* path, size_t maximum, char** text,
                           size_t* text_size, char* error,
                           size_t error_size) {
    if (!text || !text_size) return false;
    *text = NULL;
    *text_size = 0U;
    CommonFileBytes file = {0};
    CommonFileStatus status =
        common_file_read_regular_terminated(path, maximum, &file);
    if (status != COMMON_FILE_OK) {
        set_error(error, error_size, "%s: %s", path,
                  common_file_status_name(status));
        return false;
    }
    if (file.size != 0U && memchr(file.data, '\0', file.size)) {
        set_error(error, error_size, "%s: embedded NUL byte", path);
        common_file_bytes_dispose(&file);
        return false;
    }
    char* normalized = (char*)malloc(file.size + 1U);
    if (!normalized) {
        set_error(error, error_size, "%s: allocation failed", path);
        common_file_bytes_dispose(&file);
        return false;
    }
    size_t output = 0U;
    for (size_t input = 0U; input < file.size; ++input) {
        if (file.data[input] == '\r') {
            if (input + 1U < file.size && file.data[input + 1U] == '\n') {
                ++input;
            }
            normalized[output++] = '\n';
        } else {
            normalized[output++] = (char)file.data[input];
        }
    }
    normalized[output] = '\0';
    common_file_bytes_dispose(&file);
    *text = normalized;
    *text_size = output;
    return true;
}

static bool is_word_byte(char value) {
    unsigned char byte = (unsigned char)value;
    return isalnum(byte) || value == '_';
}

static const char* find_source_token(const char* source, const char* token) {
    if (!source || !token || token[0] == '\0') return NULL;
    size_t token_size = strlen(token);
    const char* at = source;
    while ((at = strstr(at, token)) != NULL) {
        bool left_ok = at == source || !is_word_byte(at[-1]);
        bool right_ok = !is_word_byte(at[token_size]);
        if (left_ok && right_ok) return at;
        ++at;
    }
    return NULL;
}

static bool extract_programs(const char* source, StringList* programs,
                             bool* had_blocks, char* error,
                             size_t error_size) {
    if (!source || !programs || !had_blocks) return false;
    memset(programs, 0, sizeof(*programs));
    *had_blocks = false;
    const char* cursor = source;
    for (;;) {
        const char* hlsl = find_source_token(cursor, "HLSLPROGRAM");
        const char* cg = find_source_token(cursor, "CGPROGRAM");
        const char* start = NULL;
        const char* end_token = NULL;
        size_t start_token_size = 0U;
        if (hlsl && (!cg || hlsl < cg)) {
            start = hlsl;
            end_token = "ENDHLSL";
            start_token_size = sizeof("HLSLPROGRAM") - 1U;
        } else if (cg) {
            start = cg;
            end_token = "ENDCG";
            start_token_size = sizeof("CGPROGRAM") - 1U;
        }
        if (!start) break;
        *had_blocks = true;
        const char* content = start + start_token_size;
        const char* end = find_source_token(content, end_token);
        if (!end) {
            set_error(error, error_size, "unterminated %.*s block",
                      (int)start_token_size, start);
            string_list_free(programs);
            return false;
        }
        const char* trimmed_start = content;
        const char* trimmed_end = end;
        while (trimmed_start < trimmed_end &&
               isspace((unsigned char)*trimmed_start)) {
            ++trimmed_start;
        }
        while (trimmed_end > trimmed_start &&
               isspace((unsigned char)trimmed_end[-1])) {
            --trimmed_end;
        }
        char* program = duplicate_span(
            trimmed_start, (size_t)(trimmed_end - trimmed_start));
        if (!program || !string_list_append_owned(programs, program)) {
            free(program);
            set_error(error, error_size, "could not allocate program block");
            string_list_free(programs);
            return false;
        }
        cursor = end + strlen(end_token);
    }
    if (!*had_blocks) {
        char* program = strdup(source);
        if (!program || !string_list_append_owned(programs, program)) {
            free(program);
            set_error(error, error_size, "could not allocate source program");
            string_list_free(programs);
            return false;
        }
    }
    return true;
}

static size_t select_stages(const GoldenFlags* flags, const char* program,
                            const GoldenStage* selected[5]) {
    if (!flags || !program || !selected) return 0U;
    if (!flags->all_stages) {
        if (flags->explicit_stage < 0 || flags->explicit_stage > 4) return 0U;
        selected[0] = &k_stages[flags->explicit_stage];
        return 1U;
    }
    size_t count = 0U;
    if (strstr(program, "#pragma vertex") ||
        strstr(program, "#pragma vert")) {
        selected[count++] = &k_stages[0];
    }
    if (strstr(program, "#pragma fragment") ||
        strstr(program, "#pragma frag")) {
        selected[count++] = &k_stages[1];
    }
    if (strstr(program, "#pragma hull")) selected[count++] = &k_stages[2];
    if (strstr(program, "#pragma domain")) selected[count++] = &k_stages[3];
    if (strstr(program, "#pragma geometry")) {
        selected[count++] = &k_stages[4];
    }
    if (count == 0U) selected[count++] = &k_stages[1];
    return count;
}

static char* build_stage_source(const char* program,
                                const GoldenStage* stage) {
    if (!program || !stage) return NULL;
    StringBuilder source;
    sb_init(&source);
    if (!strstr(program, "#pragma")) {
        const char* entry = strstr(program, "main")
                                ? "main"
                                : stage->fallback_entry;
        sb_appendf(&source, "#pragma %s %s\n", stage->pragma_name, entry);
    }
    sb_append(&source, stage->define_prefix);
    sb_append(&source, program);
    if (!sb_ok(&source)) {
        sb_free(&source);
        return NULL;
    }
    char* result = source.buf ? sb_detach(&source) : strdup("");
    sb_free(&source);
    return result;
}

static const GoldenStage* stage_from_span(const char* value, size_t size) {
    for (size_t index = 0U; index < sizeof(k_stages) / sizeof(k_stages[0]);
         ++index) {
        if (strlen(k_stages[index].name) == size &&
            memcmp(k_stages[index].name, value, size) == 0) {
            return &k_stages[index];
        }
    }
    return NULL;
}

static bool parse_decimal_field(const char** cursor, const char* end,
                                uint64_t* value) {
    if (!cursor || !*cursor || !end || !value || *cursor >= end ||
        !isdigit((unsigned char)**cursor)) {
        return false;
    }
    uint64_t result = 0U;
    const char* at = *cursor;
    while (at < end && isdigit((unsigned char)*at)) {
        uint64_t digit = (uint64_t)(*at - '0');
        if (result > (UINT64_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
        ++at;
    }
    *cursor = at;
    *value = result;
    return true;
}

static bool consume_literal(const char** cursor, const char* end,
                            const char* literal) {
    if (!cursor || !*cursor || !end || !literal) return false;
    size_t size = strlen(literal);
    if ((size_t)(end - *cursor) < size ||
        memcmp(*cursor, literal, size) != 0) {
        return false;
    }
    *cursor += size;
    return true;
}

static bool consume_required_space(const char** cursor, const char* end) {
    if (!cursor || !*cursor || *cursor >= end ||
        !isspace((unsigned char)**cursor)) {
        return false;
    }
    do {
        ++*cursor;
    } while (*cursor < end && isspace((unsigned char)**cursor));
    return true;
}

static void variant_manifest_free(VariantManifest* manifest) {
    if (!manifest) return;
    string_list_free(&manifest->keywords);
    memset(manifest, 0, sizeof(*manifest));
}

static void variant_manifest_list_free(VariantManifestList* manifests) {
    if (!manifests) return;
    for (size_t index = 0U; index < manifests->count; ++index) {
        variant_manifest_free(&manifests->values[index]);
    }
    free(manifests->values);
    memset(manifests, 0, sizeof(*manifests));
}

static bool variant_manifest_list_append(
    VariantManifestList* manifests, VariantManifest* manifest) {
    if (!manifests || !manifest || manifests->count >= GOLDEN_MAX_RECORDS) {
        return false;
    }
    if (manifests->count == manifests->capacity) {
        size_t capacity = manifests->capacity == 0U
                              ? 8U
                              : manifests->capacity * 2U;
        if (capacity < manifests->capacity ||
            capacity > GOLDEN_MAX_RECORDS ||
            capacity > SIZE_MAX / sizeof(*manifests->values)) {
            return false;
        }
        VariantManifest* resized = (VariantManifest*)realloc(
            manifests->values, capacity * sizeof(*manifests->values));
        if (!resized) return false;
        manifests->values = resized;
        manifests->capacity = capacity;
    }
    manifests->values[manifests->count++] = *manifest;
    memset(manifest, 0, sizeof(*manifest));
    return true;
}

/* Returns true for both unrelated lines and valid manifests. `recognized`
 * distinguishes them so a malformed authority-looking line fails closed. */
static bool parse_variant_manifest_line(
    const char* line, size_t line_size, size_t program_index,
    size_t variant_index, VariantManifest* manifest, bool* recognized,
    char* error, size_t error_size) {
    if (!line || !manifest || !recognized) return false;
    memset(manifest, 0, sizeof(*manifest));
    manifest->hardware_tier_group = -1;
    *recognized = false;
    const char* cursor = line;
    const char* end = line + line_size;
    while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
    if ((size_t)(end - cursor) < 2U || cursor[0] != '/' || cursor[1] != '/') {
        return true;
    }
    cursor += 2U;
    while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
    static const char marker[] = "DXBCSandbox-Variant";
    size_t marker_size = sizeof(marker) - 1U;
    if ((size_t)(end - cursor) < marker_size ||
        memcmp(cursor, marker, marker_size) != 0) {
        return true;
    }
    cursor += marker_size;
    *recognized = true;
    const char* stage_begin = NULL;
    const char* stage_end = NULL;
    if (!consume_required_space(&cursor, end) ||
        !consume_literal(&cursor, end, "stage=")) {
        goto malformed;
    }
    stage_begin = cursor;
    while (cursor < end && !isspace((unsigned char)*cursor)) ++cursor;
    stage_end = cursor;
    manifest->stage =
        stage_from_span(stage_begin, (size_t)(stage_end - stage_begin));
    if (!manifest->stage || !consume_required_space(&cursor, end) ||
        !consume_literal(&cursor, end, "blob=") ||
        !parse_decimal_field(&cursor, end, &manifest->blob_index) ||
        !consume_required_space(&cursor, end) ||
        !consume_literal(&cursor, end, "requirements=") ||
        !parse_decimal_field(&cursor, end, &manifest->requirements) ||
        !consume_required_space(&cursor, end)) {
        goto malformed;
    }
    if ((size_t)(end - cursor) >= sizeof("tier=") - 1U &&
        memcmp(cursor, "tier=", sizeof("tier=") - 1U) == 0) {
        cursor += sizeof("tier=") - 1U;
        const char* tier_begin = cursor;
        while (cursor < end && !isspace((unsigned char)*cursor)) ++cursor;
        size_t tier_size = (size_t)(cursor - tier_begin);
        if (tier_size == sizeof("generic") - 1U &&
            memcmp(tier_begin, "generic", tier_size) == 0) {
            manifest->hardware_tier_group = 3;
        } else if (tier_size == 1U && tier_begin[0] >= '1' &&
                   tier_begin[0] <= '3') {
            manifest->hardware_tier_group = tier_begin[0] - '1';
        } else {
            goto malformed;
        }
        if (!consume_required_space(&cursor, end)) goto malformed;
    }
    if (!consume_literal(&cursor, end, "keywords=")) goto malformed;
    /* Keep keyword spelling exactly as captured. The Python authority format
     * permits an empty list but does not trim individual comma fields. */
    char* keyword_text = duplicate_span(cursor, (size_t)(end - cursor));
    if (!keyword_text ||
        !split_comma_list(keyword_text, &manifest->keywords, error,
                          error_size)) {
        free(keyword_text);
        return false;
    }
    free(keyword_text);
    for (size_t index = 0U; index < manifest->keywords.count; ++index) {
        for (size_t prior = 0U; prior < index; ++prior) {
            if (strcmp(manifest->keywords.values[index],
                       manifest->keywords.values[prior]) == 0) {
                set_error(error, error_size,
                          "duplicate variant keyword: %s",
                          manifest->keywords.values[index]);
                variant_manifest_free(manifest);
                return false;
            }
        }
    }
    manifest->program_index = program_index;
    manifest->variant_index = variant_index;
    return true;

malformed:
    set_error(error, error_size,
              "malformed DXBCSandbox-Variant record in program %zu",
              program_index);
    variant_manifest_free(manifest);
    return false;
}

static bool collect_variant_manifests(const StringList* programs,
                                      VariantManifestList* manifests,
                                      char* error, size_t error_size) {
    if (!programs || !manifests) return false;
    memset(manifests, 0, sizeof(*manifests));
    for (size_t program_index = 0U; program_index < programs->count;
         ++program_index) {
        const char* program = programs->values[program_index];
        size_t size = strlen(program);
        size_t at = 0U;
        size_t variant_index = 0U;
        while (at < size) {
            size_t start = at;
            while (at < size && program[at] != '\n' &&
                   program[at] != '\r') {
                ++at;
            }
            VariantManifest manifest;
            bool recognized = false;
            if (!parse_variant_manifest_line(
                    program + start, at - start, program_index,
                    variant_index, &manifest, &recognized, error,
                    error_size)) {
                variant_manifest_list_free(manifests);
                return false;
            }
            if (recognized) {
                if (!variant_manifest_list_append(manifests, &manifest)) {
                    variant_manifest_free(&manifest);
                    set_error(error, error_size,
                              "could not allocate variant manifest list");
                    variant_manifest_list_free(manifests);
                    return false;
                }
                ++variant_index;
            }
            if (at < size && program[at] == '\r' && at + 1U < size &&
                program[at + 1U] == '\n') {
                at += 2U;
            } else if (at < size) {
                ++at;
            }
        }
    }
    if (manifests->count == 0U) {
        set_error(error, error_size,
                  "-all-variants requires DXBCSandbox-Variant records");
        return false;
    }
    return true;
}

static bool string_list_contains(const StringList* list, const char* value) {
    if (!list || !value) return false;
    for (size_t index = 0U; index < list->count; ++index) {
        if (strcmp(list->values[index], value) == 0) return true;
    }
    return false;
}

static void compile_job_free(CompileJob* job) {
    if (!job) return;
    free(job->record_name);
    for (size_t keyword = 0U; keyword < job->keyword_count; ++keyword) {
        free(job->keywords[keyword]);
    }
    free(job->keywords);
    for (size_t define = 0U; define < job->define_count; ++define) {
        free(job->defines[define]);
    }
    free(job->defines);
    memset(job, 0, sizeof(*job));
}

static void compile_job_list_free(CompileJobList* jobs) {
    if (!jobs) return;
    for (size_t index = 0U; index < jobs->count; ++index) {
        compile_job_free(&jobs->values[index]);
    }
    free(jobs->values);
    memset(jobs, 0, sizeof(*jobs));
}

static bool compile_job_list_append(CompileJobList* jobs, CompileJob* job) {
    if (!jobs || !job || jobs->count >= GOLDEN_MAX_RECORDS) return false;
    if (jobs->count == jobs->capacity) {
        size_t capacity = jobs->capacity == 0U ? 8U : jobs->capacity * 2U;
        if (capacity < jobs->capacity || capacity > GOLDEN_MAX_RECORDS ||
            capacity > SIZE_MAX / sizeof(*jobs->values)) {
            return false;
        }
        CompileJob* resized = (CompileJob*)realloc(
            jobs->values, capacity * sizeof(*jobs->values));
        if (!resized) return false;
        jobs->values = resized;
        jobs->capacity = capacity;
    }
    jobs->values[jobs->count++] = *job;
    memset(job, 0, sizeof(*job));
    return true;
}

static bool allocate_job_keywords(CompileJob* job,
                                  const StringList* base_keywords,
                                  const StringList* variant_keywords) {
    if (!job || !base_keywords) return false;
    size_t variant_count = variant_keywords ? variant_keywords->count : 0U;
    size_t count = 0U;
    if (!checked_add_size(base_keywords->count, variant_count, &count) ||
        count > GOLDEN_MAX_LIST_ITEMS ||
        count > SIZE_MAX / sizeof(*job->keywords)) {
        return false;
    }
    if (count != 0U) {
        job->keywords = (char**)calloc(count, sizeof(*job->keywords));
        if (!job->keywords) return false;
        for (size_t index = 0U; index < count; ++index) {
            const char* source = index < base_keywords->count
                                     ? base_keywords->values[index]
                                     : variant_keywords->values[
                                           index - base_keywords->count];
            job->keywords[index] = strdup(source);
            if (!job->keywords[index]) {
                for (size_t prior = 0U; prior < index; ++prior) {
                    free(job->keywords[prior]);
                }
                free(job->keywords);
                job->keywords = NULL;
                return false;
            }
        }
    }
    job->keyword_count = count;
    return true;
}

static int hardware_tier_define(const char* define) {
    static const char* tiers[] = {
        "UNITY_HARDWARE_TIER1",
        "UNITY_HARDWARE_TIER2",
        "UNITY_HARDWARE_TIER3",
    };
    if (!define) return -1;
    for (size_t index = 0U; index < sizeof(tiers) / sizeof(tiers[0]);
         ++index) {
        if (strcmp(define, tiers[index]) == 0) return (int)index;
    }
    return -1;
}

static bool allocate_job_defines(CompileJob* job, const StringList* defines,
                                 int hardware_tier_group, char* error,
                                 size_t error_size) {
    if (!job || !defines || hardware_tier_group < -1 ||
        hardware_tier_group > 3) {
        return false;
    }
    size_t tier_define_count = 0U;
    size_t retained_count = 0U;
    for (size_t index = 0U; index < defines->count; ++index) {
        if (hardware_tier_define(defines->values[index]) >= 0) {
            ++tier_define_count;
            if (hardware_tier_group < 0 || hardware_tier_group == 3) {
                ++retained_count;
            }
        } else {
            ++retained_count;
        }
    }
    if (hardware_tier_group == 3 && tier_define_count != 1U) {
        set_error(error, error_size,
                  "generic tier manifest requires exactly one explicit "
                  "UNITY_HARDWARE_TIERn define in flags");
        return false;
    }
    size_t count = retained_count;
    if (hardware_tier_group >= 0 && hardware_tier_group < 3) {
        if (!checked_add_size(count, 1U, &count)) return false;
    }
    if (count > GOLDEN_MAX_LIST_ITEMS ||
        count > SIZE_MAX / sizeof(*job->defines)) {
        return false;
    }
    if (count != 0U) {
        job->defines = (char**)calloc(count, sizeof(*job->defines));
        if (!job->defines) return false;
    }
    size_t output = 0U;
    bool inserted_numeric_tier = false;
    static const char* tiers[] = {
        "UNITY_HARDWARE_TIER1",
        "UNITY_HARDWARE_TIER2",
        "UNITY_HARDWARE_TIER3",
    };
    for (size_t index = 0U; index < defines->count; ++index) {
        if (hardware_tier_group >= 0 && hardware_tier_group < 3 &&
            hardware_tier_define(defines->values[index]) >= 0) {
            if (inserted_numeric_tier) continue;
            job->defines[output] = strdup(tiers[hardware_tier_group]);
            inserted_numeric_tier = true;
        } else {
            job->defines[output] = strdup(defines->values[index]);
        }
        if (!job->defines[output]) goto allocation_failure;
        ++output;
    }
    if (hardware_tier_group >= 0 && hardware_tier_group < 3 &&
        !inserted_numeric_tier) {
        job->defines[output] = strdup(tiers[hardware_tier_group]);
        if (!job->defines[output]) goto allocation_failure;
        ++output;
    }
    job->define_count = output;
    return true;

allocation_failure:
    for (size_t index = 0U; index < output; ++index) {
        free(job->defines[index]);
    }
    free(job->defines);
    job->defines = NULL;
    set_error(error, error_size, "could not allocate compile defines");
    return false;
}

static bool build_compile_jobs(const GoldenFlags* flags,
                               const StringList* programs, bool had_blocks,
                               CompileJobList* jobs, char* error,
                               size_t error_size) {
    if (!flags || !programs || programs->count == 0U || !jobs) return false;
    memset(jobs, 0, sizeof(*jobs));
    if (!flags->all_variants) {
        const GoldenStage* stages[5] = {0};
        size_t stage_count = select_stages(flags, programs->values[0], stages);
        for (size_t index = 0U; index < stage_count; ++index) {
            CompileJob job = {
                .record_name = strdup(stages[index]->name),
                .stage = stages[index],
                .program = programs->values[0],
                .requirements = flags->requirements,
            };
            if (!job.record_name ||
                !allocate_job_keywords(&job, &flags->keywords, NULL) ||
                !allocate_job_defines(&job, &flags->defines, -1, error,
                                      error_size) ||
                !compile_job_list_append(jobs, &job)) {
                compile_job_free(&job);
                set_error(error, error_size,
                          "could not allocate fixed-stage compile jobs");
                compile_job_list_free(jobs);
                return false;
            }
        }
        return jobs->count != 0U;
    }
    if (!had_blocks) {
        set_error(error, error_size,
                  "-all-variants requires HLSLPROGRAM/CGPROGRAM blocks");
        return false;
    }

    VariantManifestList manifests;
    if (!collect_variant_manifests(programs, &manifests, error, error_size)) {
        return false;
    }
    StringList universe = {0};
    StringList base = {0};
    bool ok = true;
    for (size_t index = 0U; index < manifests.count && ok; ++index) {
        const StringList* keywords = &manifests.values[index].keywords;
        for (size_t keyword = 0U; keyword < keywords->count; ++keyword) {
            if (string_list_contains(&universe, keywords->values[keyword])) {
                continue;
            }
            char* copy = strdup(keywords->values[keyword]);
            if (!copy || !string_list_append_owned(&universe, copy)) {
                free(copy);
                ok = false;
                break;
            }
        }
    }
    for (size_t index = 0U; index < flags->keywords.count && ok; ++index) {
        if (string_list_contains(&universe, flags->keywords.values[index])) {
            continue;
        }
        char* copy = strdup(flags->keywords.values[index]);
        if (!copy || !string_list_append_owned(&base, copy)) {
            free(copy);
            ok = false;
        }
    }
    for (size_t index = 0U; index < manifests.count && ok; ++index) {
        VariantManifest* manifest = &manifests.values[index];
        StringBuilder name;
        sb_init(&name);
        sb_appendf(&name, "pass%03zu.%s.variant%03zu.blob%" PRIu64,
                   manifest->program_index, manifest->stage->name,
                   manifest->variant_index, manifest->blob_index);
        CompileJob job = {
            .record_name = sb_ok(&name) ? sb_detach(&name) : NULL,
            .stage = manifest->stage,
            .program = programs->values[manifest->program_index],
            .requirements = manifest->requirements,
        };
        sb_free(&name);
        if (!job.record_name ||
            !allocate_job_keywords(&job, &base, &manifest->keywords) ||
            !allocate_job_defines(
                &job, &flags->defines, manifest->hardware_tier_group, error,
                error_size) ||
            !compile_job_list_append(jobs, &job)) {
            compile_job_free(&job);
            ok = false;
        }
    }
    if (!ok) {
        if (!error || error_size == 0U || error[0] == '\0') {
            set_error(error, error_size,
                      "could not allocate variant compile jobs");
        }
        compile_job_list_free(jobs);
    }
    string_list_free(&base);
    string_list_free(&universe);
    variant_manifest_list_free(&manifests);
    return ok;
}

static uint32_t read_u32_le(const uint8_t* data) {
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static bool validate_raw_dxbc(const uint8_t* bytecode, size_t size) {
    DXBCContainerView view;
    if (!dxbc_container_view_first(bytecode, size, &view) ||
        view.data != bytecode || view.size != size) {
        return false;
    }
    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    bool valid = dxbc_document_parse(
        &document, bytecode, size, &diagnostic);
    dxbc_document_free(&document);
    return valid;
}

static void golden_bundle_free(GoldenBundle* bundle) {
    if (!bundle) return;
    for (size_t index = 0U; index < bundle->count; ++index) {
        free(bundle->records[index].name);
    }
    free(bundle->records);
    memset(bundle, 0, sizeof(*bundle));
}

static bool parse_usbd(const uint8_t* bytes, size_t size,
                       GoldenBundle* bundle, char* error,
                       size_t error_size) {
    if (!bytes || !bundle) return false;
    memset(bundle, 0, sizeof(*bundle));
    DXBCUSBDTableView table;
    DXBCUSBDDiagnostic diagnostic;
    dxbc_usbd_table_init(&table);
    if (!dxbc_usbd_table_open(&table, bytes, size, &diagnostic)) {
        set_error(error, error_size,
                  "invalid USBD table: %s at byte %zu, record %" PRIu32,
                  dxbc_usbd_status_name(diagnostic.status),
                  diagnostic.byte_offset, diagnostic.record_index);
        return false;
    }
    size_t count = table.record_count;
    if (count > GOLDEN_MAX_RECORDS ||
        count > SIZE_MAX / sizeof(*bundle->records)) {
        set_error(error, error_size,
                  "USBD record count %zu exceeds verifier limit %u", count,
                  (unsigned)GOLDEN_MAX_RECORDS);
        return false;
    }
    GoldenRecord* records =
        (GoldenRecord*)calloc(count, sizeof(*records));
    if (!records) {
        set_error(error, error_size, "could not allocate USBD records");
        return false;
    }
    bundle->records = records;
    bundle->count = count;
    for (size_t index = 0U; index < count; ++index) {
        DXBCUSBDRecordView record;
        if (!dxbc_usbd_table_record(&table, (uint32_t)index, &record)) {
            set_error(error, error_size,
                      "could not read validated USBD record %zu", index);
            goto failure;
        }
        records[index].name =
            duplicate_span((const char*)record.name, record.name_size);
        if (!records[index].name) {
            set_error(error, error_size, "could not allocate USBD name");
            goto failure;
        }
        records[index].bytecode = record.dxbc;
        records[index].bytecode_size = record.dxbc_size;
    }
    return true;

failure:
    golden_bundle_free(bundle);
    return false;
}

static int dxbc_shader_stage(const uint8_t* bytecode, size_t size) {
    if (!bytecode || size < 32U || memcmp(bytecode, "DXBC", 4U) != 0 ||
        read_u32_le(bytecode + 24U) != size) {
        return -1;
    }
    size_t chunk_count = read_u32_le(bytecode + 28U);
    if (chunk_count > (size - 32U) / 4U) return -1;
    int result = -1;
    for (size_t index = 0U; index < chunk_count; ++index) {
        size_t offset = read_u32_le(bytecode + 32U + index * 4U);
        if (offset > size || size - offset < 12U) return -1;
        if (memcmp(bytecode + offset, "SHDR", 4U) != 0 &&
            memcmp(bytecode + offset, "SHEX", 4U) != 0) {
            continue;
        }
        size_t payload_size = read_u32_le(bytecode + offset + 4U);
        if (payload_size < 4U || payload_size > size - offset - 8U) return -1;
        uint32_t program_type =
            (read_u32_le(bytecode + offset + 8U) >> 16U) & 0xffffU;
        int stage = -1;
        switch (program_type) {
            case 0U: stage = 1; break;
            case 1U: stage = 0; break;
            case 2U: stage = 4; break;
            case 3U: stage = 2; break;
            case 4U: stage = 3; break;
            default: return -1;
        }
        if (result >= 0 && result != stage) return -1;
        result = stage;
    }
    return result;
}

static void append_disassembly_line(StringBuilder* output, const char* line,
                                    size_t length, uint32_t* address) {
    size_t begin = 0U;
    size_t end = length;
    while (begin < end && isspace((unsigned char)line[begin])) ++begin;
    while (end > begin && isspace((unsigned char)line[end - 1U])) --end;
    if (begin == end) return;
    if (end - begin >= 2U && line[begin] == '/' && line[begin + 1U] == '/') {
        sb_append_len(output, line, length);
        return;
    }

    size_t content = begin;
    size_t cursor = begin;
    while (cursor < length && isspace((unsigned char)line[cursor])) ++cursor;
    size_t digits = cursor;
    while (cursor < length && isdigit((unsigned char)line[cursor])) ++cursor;
    if (cursor > digits && cursor < length && line[cursor] == ':') {
        ++cursor;
        while (cursor < length && isspace((unsigned char)line[cursor])) {
            ++cursor;
        }
        content = cursor;
        end = length;
    }
    sb_appendf(output, "   %08" PRIx32 ":\t00000000 \t", *address);
    sb_append_len(output, line + content, end - content);
    *address += 4U;
}

static bool append_normalized_disassembly(StringBuilder* output,
                                          const char* disassembly,
                                          uint32_t* address) {
    if (!output || !disassembly || !address) return false;
    size_t size = strlen(disassembly);
    size_t at = 0U;
    bool first = true;
    while (at < size) {
        size_t line_start = at;
        while (at < size && disassembly[at] != '\n' &&
               disassembly[at] != '\r') {
            ++at;
        }
        size_t line_size = at - line_start;
        if (!first) sb_append_char(output, '\n');
        first = false;
        append_disassembly_line(output, disassembly + line_start, line_size,
                                address);
        if (at < size && disassembly[at] == '\r' && at + 1U < size &&
            disassembly[at + 1U] == '\n') {
            at += 2U;
        } else if (at < size) {
            ++at;
        }
    }
    return sb_ok(output);
}

static bool append_disassembled_record(UnityCompilerBroker* broker,
                                       const GoldenRecord* record, int stage,
                                       StringBuilder* output,
                                       uint32_t* address, char* error,
                                       size_t error_size) {
    if (!broker || !record || !output || !address) return false;
    char* disassembly = unity_compiler_broker_disassemble(
        broker, "Shader", 4, stage, record->bytecode,
        record->bytecode_size);
    if (!disassembly) {
        set_error(error, error_size, "Unity disassembly failed for %s",
                  record->name);
        return false;
    }
    uint32_t block_address = *address;
    sb_appendf(output, "%08" PRIx32 " <%s>:\n", block_address,
               record->name);
    bool ok = append_normalized_disassembly(output, disassembly, address);
    free(disassembly);
    if (!ok) {
        set_error(error, error_size,
                  "could not allocate normalized disassembly for %s",
                  record->name);
    }
    return ok;
}

static void report_first_difference(const char* expected, const char* actual) {
    size_t line = 1U;
    const char* left = expected;
    const char* right = actual;
    while (*left && *right && *left == *right) {
        if (*left == '\n') ++line;
        ++left;
        ++right;
    }
    const char* left_begin = left;
    const char* right_begin = right;
    while (left_begin > expected && left_begin[-1] != '\n') --left_begin;
    while (right_begin > actual && right_begin[-1] != '\n') --right_begin;
    const char* left_end = strchr(left, '\n');
    const char* right_end = strchr(right, '\n');
    if (!left_end) left_end = expected + strlen(expected);
    if (!right_end) right_end = actual + strlen(actual);
    size_t left_size = (size_t)(left_end - left_begin);
    size_t right_size = (size_t)(right_end - right_begin);
    if (left_size > 300U) left_size = 300U;
    if (right_size > 300U) right_size = 300U;
    fprintf(stderr, "\n  first mismatch at line %zu\n", line);
    fprintf(stderr, "  target:    %.*s\n", (int)left_size, left_begin);
    fprintf(stderr, "  generated: %.*s\n", (int)right_size, right_begin);
}

static void report_exact_difference(const char* record_name,
                                    const DXBCCompareResult* comparison) {
    if (!record_name || !comparison) return;
    if (comparison->status == DXBC_COMPARE_EXPECTED_INVALID ||
        comparison->status == DXBC_COMPARE_ACTUAL_INVALID) {
        const DXBCDocumentDiagnostic* diagnostic =
            comparison->status == DXBC_COMPARE_EXPECTED_INVALID
                ? &comparison->expected_diagnostic
                : &comparison->actual_diagnostic;
        fprintf(stderr,
                "\n  <%s> exact DXBC mismatch: %s (%s at byte %zu)\n",
                record_name,
                dxbc_compare_status_name(comparison->status),
                dxbc_document_diagnostic_code_name(diagnostic->code),
                diagnostic->byte_offset);
        return;
    }
    fprintf(stderr,
            "\n  <%s> exact DXBC mismatch: %s byte=%zu chunk=%" PRIu32
            " instruction=%" PRIu32 " token=%" PRIu32
            " target=0x%016" PRIx64 " generated=0x%016" PRIx64 "\n",
            record_name, dxbc_compare_status_name(comparison->status),
            comparison->first_differing_byte, comparison->chunk_index,
            comparison->instruction_index, comparison->token_index,
            comparison->expected_value, comparison->actual_value);
}

static bool append_case_path(const char* case_dir, const char* name,
                             char** result) {
    if (!result) return false;
    *result = path_join(case_dir, name);
    return *result != NULL;
}

/* This ledger measures fixture-controlled stage recompilation. It deliberately
 * makes no claim about ShaderLab state, import, or a complete runtime domain.
 * Identifiers and inputs are hashed: paths, source and compiler diagnostics
 * can contain private corpus data and do not belong in this portable report. */
typedef struct {
    const char* decode;
    const char* emission;
    const char* compilation;
    const char* comparison;
    const char* reason;
    char source_sha256[65];
    char output_sha256[65];
} RecordResult;

static void hash_hex(const void* data, size_t size, char result[65]) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(data, size, digest);
    common_sha256_digest_to_hex(digest, result);
}

static RecordResult record_result_init(bool reconstruct) {
    RecordResult result = {
        .decode = reconstruct ? "not_run" : "not_requested",
        .emission = reconstruct ? "not_run" : "not_requested",
        .compilation = "not_run", .comparison = "not_run",
        .reason = "case_setup_failed",
    };
    return result;
}

static void report_record(FILE* report, const char* case_name, size_t index,
                           const GoldenRecord* record,
                           const RecordResult* result) {
    if (!report) return;
    char case_hash[65], record_hash[65], target_hash[65];
    hash_hex(case_name, strlen(case_name), case_hash);
    hash_hex(record->name, strlen(record->name), record_hash);
    hash_hex(record->bytecode, record->bytecode_size, target_hash);
    fprintf(report,
            "{\"event\":\"record\",\"case_sha256\":\"%s\","
            "\"index\":%zu,\"record_name_sha256\":\"%s\","
            "\"target_sha256\":\"%s\",\"target_size\":%zu,"
            "\"decode\":\"%s\",\"emission\":\"%s\","
            "\"compilation\":\"%s\",\"comparison\":\"%s\","
            "\"reason\":\"%s\",\"source_sha256\":\"%s\","
            "\"output_sha256\":\"%s\"}\n",
            case_hash, index, record_hash, target_hash, record->bytecode_size,
            result->decode, result->emission, result->compilation,
            result->comparison, result->reason, result->source_sha256,
            result->output_sha256);
}

static char *emit_stage_with_options(const USILProgram *program, const GoldenStage *stage,
                                     RecordResult *result, const HLSLEmitOptions *options) {
    StringBuilder source;
    HLSLEmitDiagnostic emission_diagnostic;
    char* output = NULL;
    sb_init(&source);
    result->emission = "fail";
    /* Raw fixture containers lack serialized Unity parameter names. Generic
     * register declarations intentionally retain that missing information. */
    if (!hlsl_emit_with_options_diagnostic(program, &source, NULL, NULL, NULL, options,
                                           &emission_diagnostic)) {
        result->reason = hlsl_emit_reason_name(emission_diagnostic.reason);
        goto cleanup;
    }
    /* Unity validates that a graphics snippet declares both entry stages,
     * even when this request compiles just one stage. The opposite declaration
     * is an uncompiled harness entry, not a reconstructed ShaderLab pass. */
    StringBuilder harness;
    sb_init(&harness);
    sb_append(&harness, "#pragma vertex main\n#pragma fragment main\n");
    if (stage->id > 1) {
        sb_appendf(&harness, "#pragma %s main\n", stage->pragma_name);
    }
    sb_append(&harness, source.buf);
    if (sb_ok(&harness)) output = build_stage_source(harness.buf, stage);
    sb_free(&harness);
    result->reason = output ? "emitted" : "source_allocation_failed";
    if (output) result->emission = "pass";
cleanup:
    sb_free(&source);
    return output;
}

static char *emit_reconstructed_stage(const USILProgram *program, const GoldenStage *stage,
                                      RecordResult *result) {
    return emit_stage_with_options(program, stage, result, NULL);
}

static char* reconstruct_stage_source(const GoldenRecord* record,
                                      const GoldenStage* stage,
                                      RecordResult* result,
                                      USILProgram* retained_program) {
    DXBCDocument document;
    DXBCStageContract contract;
    DXBCContainer semantic = {0};
    USILProgram program = {0};
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContractDiagnostic contract_diagnostic;
    char* output = NULL;
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    result->decode = "fail";
    result->reason = "document_decode_failed";
    if (!dxbc_document_parse(&document, record->bytecode,
                              record->bytecode_size, &document_diagnostic)) {
        goto cleanup;
    }
    result->reason = "semantic_decode_failed";
    if (!dxbc_document_decode_semantic(&document, &semantic)) goto cleanup;
    result->reason = "stage_contract_failed";
    if (!dxbc_stage_contract_decode(&document, &semantic, &contract,
                                    &contract_diagnostic)) goto cleanup;
    result->reason = "usil_projection_failed";
    if (!usil_translate_with_stage_contract(&program, &semantic, &contract)) {
        goto cleanup;
    }
    result->decode = "pass";
    output = emit_reconstructed_stage(&program, stage, result);
    if (output && retained_program) {
        *retained_program = program;
        memset(&program, 0, sizeof(program));
    }
cleanup:
    usil_free(&program);
    dxbc_free(&semantic);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return output;
}

typedef struct {
    UnityCompilerBroker* broker;
    const GoldenFlags* flags;
    const CompileJob* job;
} GoldenLiftCompiler;

static bool lift_monotonic_ms(void* context, uint64_t* milliseconds) {
    (void)context;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / 1000u) return false;
    *milliseconds = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return true;
}

static HLSLLiftStatus compile_lift_mode(void *context, const USILProgram *program,
                                        uint64_t remaining_ms, HLSLLiftArtifact *artifact,
                                        bool high_level) {
    GoldenLiftCompiler* compiler = context;
    const CompileJob* job = compiler->job;
    if (!remaining_ms) return HLSL_LIFT_BUDGET_EXHAUSTED;
    RecordResult emission = record_result_init(true);
    const HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    artifact->source =
        emit_stage_with_options(program, job->stage, &emission, high_level ? &options : NULL);
    if (!artifact->source) return HLSL_LIFT_EMISSION_REJECTED;
    UnityCompilerBinaryResponse response;
    bool available = unity_compiler_broker_compile_response(
        compiler->broker, artifact->source, compiler->flags->shader_name,
        job->stage->id, 4, job->requirements, job->keywords,
        (int)job->keyword_count, job->defines, (int)job->define_count, &response);
    artifact->cache_hit = response.status.from_cache;
    artifact->has_request_identity = response.has_request_identity;
    memcpy(artifact->request_digest, response.request_digest, sizeof(artifact->request_digest));
    memcpy(artifact->controls_digest, response.controls_digest, sizeof(artifact->controls_digest));
    if (!available || response.status.availability == UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS) {
        unity_compiler_binary_response_free(&response);
        return HLSL_LIFT_COMPILER_UNAVAILABLE;
    }
    if (!unity_compiler_response_status_is_clean_success(&response.status)) {
        unity_compiler_binary_response_free(&response);
        return HLSL_LIFT_COMPILER_REJECTED;
    }
    size_t size = 0;
    uint8_t* compiled = unity_compiler_binary_response_take_clean_data(&response, &size, NULL);
    if (!compiled) return HLSL_LIFT_COMPILER_UNAVAILABLE;
    DXBCContainerView view;
    HLSLLiftStatus status = HLSL_LIFT_INVALID_DXBC;
    if (dxbc_container_view_first(compiled, size, &view)) {
        artifact->dxbc = malloc(view.size);
        if (artifact->dxbc) {
            memcpy(artifact->dxbc, view.data, view.size);
            artifact->dxbc_size = view.size;
            status = HLSL_LIFT_VERIFIED;
        } else {
            status = HLSL_LIFT_OUT_OF_MEMORY;
        }
    }
    free(compiled);
    return status;
}

static HLSLLiftStatus compile_lift(void *context, const USILProgram *program, uint64_t remaining_ms,
                                   HLSLLiftArtifact *artifact) {
    return compile_lift_mode(context, program, remaining_ms, artifact, false);
}

static HLSLLiftStatus compile_high_level(void *context, const USILProgram *program,
                                         uint64_t remaining_ms, HLSLLiftArtifact *artifact) {
    return compile_lift_mode(context, program, remaining_ms, artifact, true);
}

static void report_lift(FILE *report, const char *case_hash, size_t record, int instruction,
                        const HLSLLiftResult *result, int lift_kind) {
    if (!report) return;
    fprintf(report,
            "{\"event\":\"%s\",\"case_sha256\":\"%s\",\"record\":%zu,"
            "\"lift\":\"%s\",\"version\":%u,\"instruction\":%d,"
            "\"status\":\"%s\",\"precondition\":\"%s\",\"compared\":%s,"
            "\"comparison\":\"%s\",\"cache_hit\":%s,\"source_sha256\":\"%s\","
            "\"output_sha256\":\"%s\",\"request_sha256\":\"%s\","
            "\"controls_sha256\":\"%s\"}\n",
            instruction < 0 ? "lift_baseline" : "lift", case_hash, record,
            lift_kind == 2   ? HLSL_HIGH_LEVEL_LIFT_ID
            : lift_kind == 1 ? HLSL_RESULT_LIFT_ID
                             : HLSL_COPY_LIFT_ID,
            lift_kind == 2   ? HLSL_HIGH_LEVEL_LIFT_VERSION
            : lift_kind == 1 ? HLSL_RESULT_LIFT_VERSION
                             : HLSL_COPY_LIFT_VERSION,
            instruction, hlsl_lift_status_name(result->status),
            instruction < 0  ? "not_requested"
            : lift_kind == 2 ? "emitter_contract"
                             : hlsl_copy_lift_status_name(result->precondition),
            result->compared ? "true" : "false",
            result->compared ? dxbc_compare_status_name(result->comparison.status) : "not_run",
            result->cache_hit ? "true" : "false", result->source_sha256, result->output_sha256,
            result->request_sha256, result->controls_sha256);
}

static void verify_copy_lifts(UnityCompilerBroker *broker, const GoldenFlags *flags,
                              const CompileJob *job, const GoldenRecord *target,
                              const USILProgram *baseline, const char *case_name, size_t record,
                              bool high_level, FILE *report) {
    GoldenLiftCompiler compiler = {.broker = broker, .flags = flags, .job = job};
    HLSLLiftServices services = {.compile = compile_lift,
                                 .monotonic_ms = lift_monotonic_ms,
                                 .context = &compiler,
                                 .compile_high_level = compile_high_level,
                                 .require_request_identity = true};
    HLSLLiftTransaction* transaction = NULL;
    HLSLLiftResult result;
    HLSLLiftStatus status = hlsl_lift_transaction_begin(baseline, target->bytecode,
        target->bytecode_size, &services, &k_copy_limits, &transaction, &result);
    char case_hash[65];
    hash_hex(case_name, strlen(case_name), case_hash);
    report_lift(report, case_hash, record, -1, &result, false);
    if (status != HLSL_LIFT_VERIFIED) return;
    for (int index = 0; index < baseline->instruction_count; ++index) {
        if (baseline->instructions[index].opcode != USIL_OP_MOV) continue;
        status = hlsl_lift_transaction_try_copy(transaction, index, &result);
        report_lift(report, case_hash, record, index, &result, false);
        if (status == HLSL_LIFT_PRECONDITION_REJECTED || status == HLSL_LIFT_DXBC_MISMATCH ||
            status == HLSL_LIFT_EMISSION_REJECTED || status == HLSL_LIFT_COMPILER_REJECTED) {
            status = hlsl_lift_transaction_try_result(transaction, index, &result);
            report_lift(report, case_hash, record, index, &result, true);
        }
        if (status == HLSL_LIFT_BUDGET_EXHAUSTED || status == HLSL_LIFT_CANCELLED ||
            status == HLSL_LIFT_CLOCK_UNAVAILABLE || status == HLSL_LIFT_OUT_OF_MEMORY) break;
    }
    if (high_level) {
        hlsl_lift_transaction_try_high_level(transaction, &result);
        report_lift(report, case_hash, record, 0, &result, 2);
    }
    HLSLLiftStats stats;
    hlsl_lift_transaction_stats(transaction, &stats);
    const HLSLLiftArtifact* accepted = hlsl_lift_transaction_artifact(transaction);
    char source_hash[65], output_hash[65];
    hash_hex(accepted->source, strlen(accepted->source), source_hash);
    hash_hex(accepted->dxbc, accepted->dxbc_size, output_hash);
    if (report)
        fprintf(report,
                "{\"event\":\"lift_summary\",\"case_sha256\":\"%s\",\"record\":%zu,"
                "\"candidates\":%zu,\"compiles\":%zu,\"cache_hits\":%zu,\"accepted\":%zu,"
                "\"elapsed_ms\":%" PRIu64 ",\"output\":\"%s\",\"source_sha256\":\"%s\","
                "\"output_sha256\":\"%s\"}\n",
                case_hash, record, stats.candidates, stats.compiles, stats.cache_hits,
                stats.accepted, stats.elapsed_ms,
                hlsl_lift_transaction_is_high_level(transaction) ? "high_level"
                : stats.accepted                                 ? "mixed"
                                                                 : "low_level_fallback",
                source_hash, output_hash);
    printf(" [copy lifts: %zu/%zu]", stats.accepted, stats.candidates);
    hlsl_lift_transaction_destroy(transaction);
}

static bool verify_case(UnityCompilerBroker *broker, const char *golden_dir, const char *case_name,
                        bool reconstruct, bool lift_copies, bool high_level, FILE *report,
                        bool *assembly_only_match) {
    if (assembly_only_match) *assembly_only_match = false;
    bool ok = false;
    size_t reported_records = 0U;
    bool domain_valid = false;
    bool target_domain_available = false;
    char error[512] = {0};
    char* case_dir = path_join(golden_dir, case_name);
    char* flags_path = NULL;
    char* source_path = NULL;
    char* target_path = NULL;
    char* flags_text = NULL;
    size_t flags_size = 0U;
    char* source_text = NULL;
    size_t source_size = 0U;
    StringList programs = {0};
    bool had_blocks = false;
    CompileJobList jobs = {0};
    GoldenFlags flags;
    memset(&flags, 0, sizeof(flags));
    CommonFileBytes target_file = {0};
    GoldenBundle target_bundle;
    memset(&target_bundle, 0, sizeof(target_bundle));
    StringBuilder target_disassembly;
    StringBuilder generated_disassembly;
    sb_init(&target_disassembly);
    sb_init(&generated_disassembly);

    if (!case_dir || !append_case_path(case_dir, "flags.txt", &flags_path) ||
        !append_case_path(case_dir, "source.shader", &source_path) ||
        !append_case_path(case_dir, "target.bin", &target_path)) {
        set_error(error, sizeof(error), "could not allocate case paths");
        goto cleanup;
    }
    CommonFileStatus target_status = common_file_read_regular(
        target_path, GOLDEN_MAX_BUNDLE_SIZE, &target_file);
    if (target_status != COMMON_FILE_OK) {
        set_error(error, sizeof(error), "%s: %s", target_path,
                  common_file_status_name(target_status));
        goto cleanup;
    }
    if (!parse_usbd(target_file.data, target_file.size, &target_bundle, error,
                    sizeof(error))) {
        goto cleanup;
    }

    target_domain_available = true;
    if (!read_text_file(flags_path, GOLDEN_MAX_FLAGS_SIZE, &flags_text,
                        &flags_size, error, sizeof(error)) ||
        !parse_golden_flags(flags_text, &flags, error, sizeof(error)) ||
        !read_text_file(source_path, GOLDEN_MAX_SOURCE_SIZE, &source_text,
                        &source_size, error, sizeof(error)) ||
        !extract_programs(source_text, &programs, &had_blocks, error,
                          sizeof(error)) ||
        !build_compile_jobs(&flags, &programs, had_blocks, &jobs, error,
                            sizeof(error))) {
        goto cleanup;
    }

    if (jobs.count == 0U || target_bundle.count != jobs.count) {
        set_error(error, sizeof(error),
                  "stage count mismatch: source selects %zu, target has %zu",
                  jobs.count, target_bundle.count);
        goto cleanup;
    }

    domain_valid = true;
    uint32_t target_address = UINT32_C(0x80000000);
    uint32_t generated_address = UINT32_C(0x80000000);
    bool exact_match = true;
    bool all_compared = true;
    bool all_disassembled = true;
    for (size_t index = 0U; index < jobs.count; ++index) {
        const CompileJob* job = &jobs.values[index];
        const GoldenStage* stage = job->stage;
        const GoldenRecord* target = &target_bundle.records[index];
        RecordResult result = record_result_init(reconstruct);
        USILProgram lift_baseline = {0};
        char* stage_source = NULL;
        uint8_t* compiled = NULL;
        char* compile_error = NULL;
        size_t compiled_size = 0U;
        bool record_ok = false;
        int target_stage = dxbc_shader_stage(
            target->bytecode, target->bytecode_size);
        result.reason = "record_domain_mismatch";
        if (strcmp(target->name, job->record_name) != 0 ||
            target_stage != stage->id) {
            domain_valid = false;
            goto record_done;
        }
        stage_source = reconstruct
            ? reconstruct_stage_source(target, stage, &result,
                                        lift_copies ? &lift_baseline : NULL)
            : build_stage_source(job->program, stage);
        if (!stage_source) goto record_done;
        hash_hex(stage_source, strlen(stage_source), result.source_sha256);
        UnityCompilerBinaryResponse response;
        bool response_available = unity_compiler_broker_compile_response(
            broker, stage_source, flags.shader_name, stage->id, 4,
            job->requirements, job->keywords, (int)job->keyword_count,
            job->defines, (int)job->define_count, &response);
        if (!response_available || response.status.availability ==
                UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS) {
            result.compilation = "unavailable";
            result.reason = response_available ? "cache_only_miss"
                                              : "compiler_or_protocol_unavailable";
            unity_compiler_binary_response_free(&response);
            goto record_done;
        }
        result.compilation = unity_compiler_response_status_is_clean_success(
            &response.status) ? "pass" : "fail";
        result.reason = "compiler_rejected";
        compiled = unity_compiler_binary_response_take_clean_data(
            &response, &compiled_size, &compile_error);
        if (!compiled) {
            if (strcmp(result.compilation, "pass") == 0) {
                result.compilation = "unavailable";
                result.reason = "output_allocation_failed";
            }
            fprintf(stderr, "\n  compile failed for %s: %s\n", stage->name,
                    compile_error ? compile_error : "no diagnostic");
            goto record_done;
        }
        DXBCContainerView compiled_view;
        result.reason = "invalid_compiler_output";
        if (!dxbc_container_view_first(compiled, compiled_size,
                                       &compiled_view) ||
            !validate_raw_dxbc(compiled_view.data, compiled_view.size) ||
            dxbc_shader_stage(compiled_view.data, compiled_view.size) !=
                stage->id) goto record_done;
        hash_hex(compiled_view.data, compiled_view.size, result.output_sha256);
        GoldenRecord generated = {
            .name = job->record_name,
            .bytecode = compiled_view.data,
            .bytecode_size = compiled_view.size,
        };
        DXBCCompareResult comparison;
        DXBCCompareStatus compare_status = dxbc_compare_exact(
            target->bytecode, target->bytecode_size, generated.bytecode,
            generated.bytecode_size, &comparison);
        result.comparison = compare_status == DXBC_COMPARE_EQUAL
            ? "pass" : "fail";
        result.reason = dxbc_compare_status_name(compare_status);
        record_ok = compare_status == DXBC_COMPARE_EQUAL;
        if (record_ok && lift_copies) {
            verify_copy_lifts(broker, &flags, job, target, &lift_baseline, case_name, index,
                              high_level, report);
        }
        if (!record_ok) {
            report_exact_difference(job->record_name, &comparison);
            if (target_disassembly.len) {
                sb_append(&target_disassembly, "\n\n");
                sb_append(&generated_disassembly, "\n\n");
            }
            if (!append_disassembled_record(
                    broker, target, stage->id, &target_disassembly,
                    &target_address, error, sizeof(error)) ||
                !append_disassembled_record(
                    broker, &generated, stage->id, &generated_disassembly,
                    &generated_address, error, sizeof(error))) {
                /* Comparison failure remains authoritative even if its
                 * optional assembly diagnostic cannot be produced. */
                all_disassembled = false;
            }
        }
record_done:
        if (strcmp(result.comparison, "not_run") == 0) all_compared = false;
        report_record(report, case_name, index, target, &result);
        ++reported_records;
        if (!record_ok) {
            exact_match = false;
            fprintf(stderr, "\n  record %zu: %s\n", index, result.reason);
        }
        free(compile_error);
        free(compiled);
        free(stage_source);
        usil_free(&lift_baseline);
    }

    if (exact_match) {
        ok = true;
        goto cleanup;
    }
    if (!all_compared || !all_disassembled ||
        !target_disassembly.len || !generated_disassembly.len) goto cleanup;
    if (!sb_ok(&target_disassembly) || !sb_ok(&generated_disassembly)) {
        set_error(error, sizeof(error),
                  "could not allocate complete disassembly");
        goto cleanup;
    }
    const char* target_text =
        target_disassembly.buf ? target_disassembly.buf : "";
    const char* generated_text =
        generated_disassembly.buf ? generated_disassembly.buf : "";
    if (strcmp(target_text, generated_text) != 0) {
        set_error(error, sizeof(error),
                  "container bytes and assembly instructions both differ");
        report_first_difference(target_text, generated_text);
    } else {
        set_error(error, sizeof(error),
                  "container bytes differ although normalized assembly "
                  "matches");
        if (assembly_only_match) *assembly_only_match = true;
    }

cleanup:
    for (size_t index = reported_records; index < target_bundle.count; ++index) {
        RecordResult result = record_result_init(reconstruct);
        report_record(report, case_name, index, &target_bundle.records[index],
                      &result);
    }
    if (report) {
        char case_hash[65], flags_hash[65] = "", source_hash[65] = "";
        hash_hex(case_name, strlen(case_name), case_hash);
        if (flags_text) hash_hex(flags_text, flags_size, flags_hash);
        if (source_text) hash_hex(source_text, source_size, source_hash);
        fprintf(report,
                "{\"event\":\"case\",\"case_sha256\":\"%s\","
                "\"target_records\":%zu,\"compile_jobs\":%zu,"
                "\"target_domain_available\":%s,\"domain_valid\":%s,\"exact\":%s,"
                "\"flags_sha256\":\"%s\",\"retained_source_sha256\":\"%s\"}\n",
                case_hash, target_bundle.count, jobs.count,
                target_domain_available ? "true" : "false",
                domain_valid ? "true" : "false", ok ? "true" : "false",
                flags_hash, source_hash);
    }
    if (!ok) fprintf(stderr, "  %s\n", error[0] ? error : "verification failed");
    sb_free(&generated_disassembly);
    sb_free(&target_disassembly);
    golden_bundle_free(&target_bundle);
    common_file_bytes_dispose(&target_file);
    compile_job_list_free(&jobs);
    string_list_free(&programs);
    golden_flags_free(&flags);
    free(source_text);
    free(flags_text);
    free(target_path);
    free(source_path);
    free(flags_path);
    free(case_dir);
    return ok;
}

static int compare_strings(const void* left, const void* right) {
    const char* const* first = (const char* const*)left;
    const char* const* second = (const char* const*)right;
    return strcmp(*first, *second);
}

static bool discover_cases(const char* golden_dir, const char* only_case,
                           StringList* cases, char* error,
                           size_t error_size) {
    if (!golden_dir || !cases) return false;
    memset(cases, 0, sizeof(*cases));
    DIR* directory = opendir(golden_dir);
    if (!directory) {
        set_error(error, error_size, "%s: %s", golden_dir, strerror(errno));
        return false;
    }
    bool ok = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' ||
            (only_case && strcmp(only_case, entry->d_name) != 0)) {
            continue;
        }
        char* case_dir = path_join(golden_dir, entry->d_name);
        bool is_case = case_dir && is_directory_path(case_dir);
        free(case_dir);
        /* Every case directory belongs to the denominator, including one
         * whose required inputs have all been removed. */
        if (!is_case) continue;
        char* name = strdup(entry->d_name);
        if (!name || !string_list_append_owned(cases, name)) {
            free(name);
            set_error(error, error_size, "could not allocate case list");
            ok = false;
            break;
        }
        if (cases->count > GOLDEN_MAX_CASES) {
            set_error(error, error_size, "too many golden cases");
            ok = false;
            break;
        }
    }
    if (closedir(directory) != 0 && ok) {
        set_error(error, error_size, "%s: %s", golden_dir, strerror(errno));
        ok = false;
    }
    if (!ok) {
        string_list_free(cases);
        return false;
    }
    if (cases->count == 0U) {
        set_error(error, error_size,
                  only_case ? "golden case not found: %s"
                            : "no complete golden cases found in %s",
                  only_case ? only_case : golden_dir);
        string_list_free(cases);
        return false;
    }
    qsort(cases->values, cases->count, sizeof(*cases->values),
          compare_strings);
    return true;
}

static bool configure_unity_contents(const char* explicit_contents,
                                     char* error, size_t error_size) {
    if (explicit_contents) {
        if (setenv("DXBC_UNITY_CONTENTS_PATH", explicit_contents, 1) != 0) {
            set_error(error, error_size, "could not set Unity contents: %s",
                      strerror(errno));
            return false;
        }
    }
    /* Otherwise leave discovery to the shared client.  It honors the
     * DXBC_UNITY_* / UNITY_EDITOR_PATH authority variables and finally the
     * normal installed-Unity location.  Verification never requires a copied
     * platform binary inside this repository. */
    return true;
}

static void print_usage(const char* executable) {
    fprintf(stderr,
            "Usage: %s [--golden-dir DIR] [--project-root DIR] "
            "[--includes-dir DIR]\n"
            "          [--unity-contents DIR] [--case NAME] [--self-test]\n"
            "          [--self-test-lifts (launches the selected Unity compiler)]\n"
            "          [--reconstruct] [--lift-copies] [--high-level] [--report JSONL]\n"
            "--reconstruct regenerates low-level HLSL from target containers;\n"
            "raw fixtures lack Unity parameter/sampler metadata. Reports cover\n"
            "fixture-controlled stages, not whole-shader certificates.\n"
            "--lift-copies implies --reconstruct and audits bounded copy/swizzle and single-use "
            "result\n"
            "transactions: 64 candidates, 33 compiles, 30s acceptance deadline\n"
            "per exact baseline stage. In-flight compiler I/O has its own timeout.\n"
            "--high-level also attempts the closed float4 expression candidate mode.\n"
            "--report creates a new privacy-safe JSONL ledger; existing files\n"
            "are never overwritten.\n",
            executable);
}

static bool parse_command_line(int argc, char** argv, CommandLine* options) {
    if (!options) return false;
    *options = (CommandLine){
        .golden_dir = DXBC_GOLDEN_DEFAULT_DIR,
        .project_root = DXBC_GOLDEN_DEFAULT_PROJECT_ROOT,
        .includes_dir = DXBC_GOLDEN_DEFAULT_INCLUDES,
    };
    for (int index = 1; index < argc; ++index) {
        const char* option = argv[index];
        if (strcmp(option, "--self-test") == 0) {
            options->self_test = true;
        } else if (strcmp(option, "--self-test-lifts") == 0) {
            options->self_test_lifts = true;
        } else if (strcmp(option, "--reconstruct") == 0) {
            options->reconstruct = true;
        } else if (strcmp(option, "--high-level") == 0) {
            options->high_level = true;
            options->lift_copies = true;
            options->reconstruct = true;
        } else if (strcmp(option, "--lift-copies") == 0) {
            options->lift_copies = true;
            options->reconstruct = true;
        } else if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            if (index + 1 >= argc) return false;
            const char* value = argv[++index];
            if (strcmp(option, "--golden-dir") == 0) {
                options->golden_dir = value;
            } else if (strcmp(option, "--project-root") == 0) {
                options->project_root = value;
            } else if (strcmp(option, "--includes-dir") == 0) {
                options->includes_dir = value;
            } else if (strcmp(option, "--unity-contents") == 0) {
                options->unity_contents = value;
            } else if (strcmp(option, "--report") == 0) {
                options->report_path = value;
            } else if (strcmp(option, "--case") == 0) {
                options->case_name = value;
            } else {
                return false;
            }
        }
    }
    return true;
}

#define SELF_CHECK(condition)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "self-test failed at %s:%d: %s\n", __FILE__,   \
                    __LINE__, #condition);                                   \
            goto cleanup;                                                    \
        }                                                                    \
    } while (0)

static bool run_self_test(void) {
    char error[256] = {0};
    bool succeeded = false;
    GoldenFlags flags = {0};
    StringList programs = {0};
    GoldenFlags variant_flags = {0};
    CompileJobList jobs = {0};
    VariantManifest legacy_manifest = {0};
    StringBuilder normalized = {0};
    CommonFileBytes fixture = {0};
    GoldenBundle bundle = {0};
    uint8_t* trailing = NULL;
    char* reconstructed = NULL;

    SELF_CHECK(parse_golden_flags(
        "-stage vertex -shader-name 'Name With Spaces' -reqs 42 "
        "-keywords A,,B -defines D=1,E",
        &flags, error, sizeof(error)));
    SELF_CHECK(!flags.all_stages && flags.explicit_stage == 0);
    SELF_CHECK(flags.requirements == 42U);
    SELF_CHECK(strcmp(flags.shader_name, "Name With Spaces") == 0);
    SELF_CHECK(flags.keywords.count == 2U &&
               strcmp(flags.keywords.values[0], "A") == 0 &&
               strcmp(flags.keywords.values[1], "B") == 0);
    SELF_CHECK(flags.defines.count == 2U &&
               strcmp(flags.defines.values[0], "D=1") == 0);
    golden_flags_free(&flags);
    SELF_CHECK(!parse_golden_flags("-stage 'vertex", &flags, error,
                                   sizeof(error)));
    SELF_CHECK(!parse_golden_flags("-mystery value", &flags, error,
                                   sizeof(error)));

    bool had_blocks = false;
    SELF_CHECK(extract_programs(
        "Shader \"T\" { HLSLPROGRAM\n  #pragma vertex vert\nvalue;  \n"
        "ENDHLSL }",
        &programs, &had_blocks, error, sizeof(error)));
    SELF_CHECK(had_blocks && programs.count == 1U);
    SELF_CHECK(strcmp(programs.values[0],
                      "#pragma vertex vert\nvalue;") == 0);
    const GoldenStage* stages[5] = {0};
    GoldenFlags stage_flags = {.all_stages = true, .explicit_stage = -1};
    SELF_CHECK(select_stages(&stage_flags, programs.values[0], stages) == 1U);
    SELF_CHECK(stages[0]->id == 0);
    string_list_free(&programs);

    SELF_CHECK(parse_golden_flags(
        "-all-variants -stage all -shader-name Variant/Test "
        "-keywords BASE,KW_A -defines OTHER,UNITY_HARDWARE_TIER3",
        &variant_flags, error, sizeof(error)));
    SELF_CHECK(extract_programs(
        "Shader \"V\" { HLSLPROGRAM\n"
        "// DXBCSandbox-Variant stage=vertex blob=7 requirements=9 "
        "tier=1 keywords=KW_A,KW_B\nENDHLSL\nCGPROGRAM\n"
        "// DXBCSandbox-Variant stage=fragment blob=8 requirements=10 "
        "tier=generic keywords=KW_C\nENDCG }",
        &programs, &had_blocks, error, sizeof(error)));
    SELF_CHECK(build_compile_jobs(&variant_flags, &programs, had_blocks,
                                  &jobs, error, sizeof(error)));
    SELF_CHECK(jobs.count == 2U);
    SELF_CHECK(strcmp(jobs.values[0].record_name,
                      "pass000.vertex.variant000.blob7") == 0);
    SELF_CHECK(jobs.values[0].requirements == 9U &&
               jobs.values[0].keyword_count == 3U);
    SELF_CHECK(strcmp(jobs.values[0].keywords[0], "BASE") == 0 &&
               strcmp(jobs.values[0].keywords[1], "KW_A") == 0 &&
               strcmp(jobs.values[0].keywords[2], "KW_B") == 0);
    SELF_CHECK(jobs.values[0].define_count == 2U &&
               strcmp(jobs.values[0].defines[0], "OTHER") == 0 &&
               strcmp(jobs.values[0].defines[1],
                      "UNITY_HARDWARE_TIER1") == 0);
    SELF_CHECK(strcmp(jobs.values[1].record_name,
                      "pass001.fragment.variant000.blob8") == 0);
    SELF_CHECK(jobs.values[1].keyword_count == 2U &&
               strcmp(jobs.values[1].keywords[0], "BASE") == 0 &&
               strcmp(jobs.values[1].keywords[1], "KW_C") == 0);
    SELF_CHECK(jobs.values[1].define_count == 2U &&
               strcmp(jobs.values[1].defines[0], "OTHER") == 0 &&
               strcmp(jobs.values[1].defines[1],
                      "UNITY_HARDWARE_TIER3") == 0);
    compile_job_list_free(&jobs);
    string_list_free(&programs);
    golden_flags_free(&variant_flags);
    bool recognized = false;
    const char legacy_line[] =
        "// DXBCSandbox-Variant stage=hull blob=11 requirements=12 "
        "keywords=LEGACY";
    SELF_CHECK(parse_variant_manifest_line(
        legacy_line, sizeof(legacy_line) - 1U, 0U, 0U, &legacy_manifest,
        &recognized, error, sizeof(error)));
    SELF_CHECK(recognized && legacy_manifest.hardware_tier_group == -1 &&
               legacy_manifest.stage->id == 2);
    variant_manifest_free(&legacy_manifest);
    const char bad_tier_line[] =
        "// DXBCSandbox-Variant stage=vertex blob=1 requirements=2 "
        "tier=4 keywords=";
    SELF_CHECK(!parse_variant_manifest_line(
        bad_tier_line, sizeof(bad_tier_line) - 1U, 0U, 0U,
        &legacy_manifest, &recognized, error, sizeof(error)));

    sb_init(&normalized);
    uint32_t address = UINT32_C(0x80000000);
    SELF_CHECK(append_normalized_disassembly(
        &normalized, "  // comment  \r\n  12: add r0, r1  \n\n custom ",
        &address));
    SELF_CHECK(strcmp(
        normalized.buf,
        "  // comment  \n   80000000:\t00000000 \tadd r0, r1  \n\n"
        "   80000004:\t00000000 \tcustom") == 0);
    SELF_CHECK(address == UINT32_C(0x80000008));
    sb_free(&normalized);

    SELF_CHECK(common_file_read_regular(DXBC_GOLDEN_TEST_FIXTURE,
                                        GOLDEN_MAX_BUNDLE_SIZE,
                                        &fixture) == COMMON_FILE_OK);
    SELF_CHECK(parse_usbd(fixture.data, fixture.size, &bundle, error,
                          sizeof(error)));
    SELF_CHECK(bundle.count == 2U);
    SELF_CHECK(strcmp(bundle.records[0].name, "vertex") == 0);
    SELF_CHECK(strcmp(bundle.records[1].name, "fragment") == 0);
    SELF_CHECK(dxbc_shader_stage(bundle.records[0].bytecode,
                                 bundle.records[0].bytecode_size) == 0);
    SELF_CHECK(dxbc_shader_stage(bundle.records[1].bytecode,
                                 bundle.records[1].bytecode_size) == 1);
    RecordResult reconstructed_result = record_result_init(true);
    reconstructed = reconstruct_stage_source(
        &bundle.records[0], &k_stages[0], &reconstructed_result, NULL);
    SELF_CHECK(reconstructed && strcmp(reconstructed_result.decode, "pass") == 0 &&
               strcmp(reconstructed_result.emission, "pass") == 0);
    SELF_CHECK(strstr(reconstructed, "#pragma vertex main") &&
               strstr(reconstructed, "#pragma fragment main"));
    free(reconstructed);
    reconstructed = NULL;
    GoldenRecord invalid_record = bundle.records[0];
    invalid_record.bytecode_size = 8U;
    reconstructed_result = record_result_init(true);
    reconstructed = reconstruct_stage_source(
        &invalid_record, &k_stages[0], &reconstructed_result, NULL);
    SELF_CHECK(!reconstructed && strcmp(reconstructed_result.decode, "fail") == 0 &&
               strcmp(reconstructed_result.emission, "not_run") == 0 &&
               strcmp(reconstructed_result.compilation, "not_run") == 0);
    golden_bundle_free(&bundle);
    trailing = (uint8_t*)malloc(fixture.size + 1U);
    SELF_CHECK(trailing != NULL);
    memcpy(trailing, fixture.data, fixture.size);
    trailing[fixture.size] = 0U;
    SELF_CHECK(!parse_usbd(trailing, fixture.size + 1U, &bundle, error,
                           sizeof(error)));
    free(trailing);
    trailing = NULL;
    common_file_bytes_dispose(&fixture);
    puts("unity_golden_verifier self-test passed");
    succeeded = true;

cleanup:
    free(reconstructed);
    free(trailing);
    golden_bundle_free(&bundle);
    common_file_bytes_dispose(&fixture);
    sb_free(&normalized);
    variant_manifest_free(&legacy_manifest);
    compile_job_list_free(&jobs);
    golden_flags_free(&variant_flags);
    string_list_free(&programs);
    golden_flags_free(&flags);
    return succeeded;
}

typedef struct {
    GoldenLiftCompiler compiler;
    bool corrupt_candidate;
} LiveLiftFixture;

static HLSLLiftStatus compile_live_lift_fixture_mode(void *context, const USILProgram *program,
                                                     uint64_t remaining_ms,
                                                     HLSLLiftArtifact *artifact, bool high_level) {
    LiveLiftFixture* fixture = context;
    if (!fixture->corrupt_candidate ||
        (!high_level && program->instructions[0].opcode != USIL_OP_NOP))
        return compile_lift_mode(&fixture->compiler, program, remaining_ms, artifact, high_level);
    /* Deliberately inject a bad rewrite after the planner. Compile the changed
     * source honestly; the transaction must reject the resulting DXBC. */
    USILInstruction instructions[4];
    if (program->instruction_count != 4) return HLSL_LIFT_INVALID_ARGUMENT;
    memcpy(instructions, program->instructions, sizeof(instructions));
    for (int index = 0; index < 4; ++index) {
        if (instructions[index].opcode == USIL_OP_MUL) {
            instructions[index].opcode = USIL_OP_ADD;
            break;
        }
        if (instructions[index].opcode == USIL_OP_MOV) {
            instructions[index].operands[1].swizzle[0] ^= 1u;
            break;
        }
    }
    USILProgram wrong = *program;
    wrong.instructions = instructions;
    return compile_lift_mode(&fixture->compiler, &wrong, remaining_ms, artifact, high_level);
}

static HLSLLiftStatus compile_live_lift_fixture(void *context, const USILProgram *program,
                                                uint64_t remaining_ms, HLSLLiftArtifact *artifact) {
    return compile_live_lift_fixture_mode(context, program, remaining_ms, artifact, false);
}

static HLSLLiftStatus compile_live_high_level_fixture(void *context, const USILProgram *program,
                                                      uint64_t remaining_ms,
                                                      HLSLLiftArtifact *artifact) {
    return compile_live_lift_fixture_mode(context, program, remaining_ms, artifact, true);
}

static bool run_live_lift_fixture(UnityCompilerBroker* broker, int stage, bool forward_result) {
    bool succeeded = false;
    USILProgram decoded = {0};
    uint8_t* bytes = NULL;
    size_t byte_count = 0;
    char* error = NULL;
    char* reconstructed = NULL;
    HLSLLiftTransaction* transaction = NULL;
    const char* seed = forward_result ? (stage == 0
        ? "#pragma vertex main\n#pragma fragment main\n"
          "float4 main(float4 value : POSITION) : SV_POSITION { return value * float4(2,3,4,5); }\n"
        : "#pragma vertex main\n#pragma fragment main\n"
          "float4 main(float4 value : TEXCOORD0) : SV_Target { return value * float4(2,3,4,5); }\n") : stage == 0
        ? "#pragma vertex main\n#pragma fragment main\n"
          "float4 main(float4 value : POSITION) : SV_POSITION { return value.wzyx; }\n"
        : "#pragma vertex main\n#pragma fragment main\n"
          "float4 main(float4 value : TEXCOORD0) : SV_Target { return value.wzyx; }\n";
    bytes = unity_compiler_broker_compile(broker, seed, "LiftFixture", stage, 4,
                                          0, NULL, 0, NULL, 0, &byte_count, &error);
    SELF_CHECK(bytes);
    DXBCContainerView container;
    SELF_CHECK(dxbc_container_view_first(bytes, byte_count, &container));
    GoldenRecord target = {.name = "controlled-copy", .bytecode = container.data,
                            .bytecode_size = container.size};
    RecordResult reconstruction = record_result_init(true);
    reconstructed = reconstruct_stage_source(&target, &k_stages[stage],
                                               &reconstruction, &decoded);
    SELF_CHECK(reconstructed && decoded.instruction_count == 2 && decoded.temp_count == 0);
    SELF_CHECK(decoded.instructions[0].opcode == (forward_result ? USIL_OP_MUL : USIL_OP_MOV) &&
               decoded.instructions[1].opcode == USIL_OP_RET);
    SELF_CHECK(forward_result || decoded.instructions[0].operands[1].type == OPERAND_TYPE_INPUT);
    /* A controlled IR fixture introduces redundant copies of the target's
     * swizzled value. This exercises the gate, not recovered-corpus coverage. */
    USILInstruction instructions[4] = {0};
    instructions[0] = decoded.instructions[0];
    instructions[0].operands[0].type = OPERAND_TYPE_TEMP;
    instructions[0].operands[0].raw_token &= ~UINT32_C(0xff000);
    instructions[0].operands[0].register_index = 0;
    instructions[1] = instructions[0];
    instructions[1].operands[0].register_index = 1;
    instructions[1].operands[0].index_values[0] = 1;
    instructions[1].operands[1] = (DXBCOperand){.type = OPERAND_TYPE_TEMP,
        .register_index = 0, .raw_token = 0x00100e46u, .swizzle_mode = 1,
        .swizzle = {0, 1, 2, 3}, .register_index_dim = 1,
        .index_has_immediate = {true, false, false}};
    instructions[2] = decoded.instructions[0];
    instructions[2].operands[1] = instructions[1].operands[1];
    instructions[2].operands[1].register_index = 1;
    instructions[2].operands[1].index_values[0] = 1;
    instructions[3] = decoded.instructions[1];
    if (forward_result) {
        instructions[1] = decoded.instructions[0];
        instructions[1].opcode = USIL_OP_MOV;
        instructions[1].operand_count = 2;
        instructions[1].operands[1] = instructions[2].operands[1];
        instructions[1].operands[1].register_index = 0;
        instructions[1].operands[1].index_values[0] = 0;
        instructions[2] = (USILInstruction){.opcode = USIL_OP_NOP};
    }
    USILProgram baseline = decoded;
    baseline.instructions = instructions;
    baseline.instruction_count = 4;
    baseline.instruction_alloc = 4;
    baseline.temp_count = 2;
    GoldenFlags flags = {.shader_name = "LiftFixture"};
    CompileJob job = {.stage = &k_stages[stage]};
    LiveLiftFixture fixture = {.compiler = {.broker = broker, .flags = &flags, .job = &job},
                               .corrupt_candidate = true};
    HLSLLiftServices services = {.compile = compile_live_lift_fixture,
                                 .monotonic_ms = lift_monotonic_ms,
                                 .context = &fixture,
                                 .compile_high_level = compile_live_high_level_fixture,
                                 .require_request_identity = true};
    HLSLLiftResult result;
    HLSLLiftStatus baseline_status = hlsl_lift_transaction_begin(
        &baseline, target.bytecode, target.bytecode_size,
        &services, &k_copy_limits, &transaction, &result);
    if (baseline_status != HLSL_LIFT_VERIFIED) {
        fprintf(stderr, "controlled lift baseline: %s (%s)\n",
                hlsl_lift_status_name(baseline_status),
                result.compared ? dxbc_compare_status_name(result.comparison.status) : "not compared");
    }
    SELF_CHECK(baseline_status == HLSL_LIFT_VERIFIED);
    char baseline_hash[65];
    memcpy(baseline_hash, result.source_sha256, sizeof(baseline_hash));
    HLSLLiftStatus (*attempt)(HLSLLiftTransaction*, int, HLSLLiftResult*) =
        forward_result ? hlsl_lift_transaction_try_result : hlsl_lift_transaction_try_copy;
    const int first = forward_result ? 1 : 0;
    SELF_CHECK(attempt(transaction, first, &result) == HLSL_LIFT_DXBC_MISMATCH);
    SELF_CHECK(hlsl_lift_transaction_program(transaction) == &baseline);
    fixture.corrupt_candidate = false;
    SELF_CHECK(attempt(transaction, first, &result) == HLSL_LIFT_VERIFIED);
    SELF_CHECK(strcmp(baseline_hash, result.source_sha256) != 0);
    if (!forward_result)
        SELF_CHECK(hlsl_lift_transaction_try_copy(transaction, 1, &result) == HLSL_LIFT_VERIFIED);
    SELF_CHECK(instructions[0].opcode == decoded.instructions[0].opcode &&
               instructions[1].opcode == USIL_OP_MOV);
    HLSLLiftStats stats;
    hlsl_lift_transaction_stats(transaction, &stats);
    SELF_CHECK(stats.accepted == (forward_result ? 1u : 2u) &&
               stats.candidates == stats.accepted + 1u && stats.compiles == stats.accepted + 2u);
    const HLSLLiftArtifact *prior = hlsl_lift_transaction_artifact(transaction);
    const char *prior_source = prior->source;
    fixture.corrupt_candidate = true;
    HLSLLiftStatus wrong_status = hlsl_lift_transaction_try_high_level(transaction, &result);
    if (wrong_status != HLSL_LIFT_DXBC_MISMATCH)
        fprintf(stderr, "wrong high-level fixture: %s\n", hlsl_lift_status_name(wrong_status));
    SELF_CHECK(wrong_status == HLSL_LIFT_DXBC_MISMATCH);
    SELF_CHECK(!hlsl_lift_transaction_is_high_level(transaction));
    SELF_CHECK(hlsl_lift_transaction_artifact(transaction)->source == prior_source);
    fixture.corrupt_candidate = false;
    HLSLLiftStatus high_status = hlsl_lift_transaction_try_high_level(transaction, &result);
    if (high_status != HLSL_LIFT_VERIFIED)
        fprintf(stderr, "high-level fixture: %s\n", hlsl_lift_status_name(high_status));
    SELF_CHECK(high_status == HLSL_LIFT_VERIFIED &&
               hlsl_lift_transaction_is_high_level(transaction));
    SELF_CHECK(hlsl_lift_transaction_try_copy(transaction, 0, &result) ==
               HLSL_LIFT_COMPOSITION_UNSUPPORTED);
    printf("%s: %s exactly reproduced target DXBC; deliberately wrong candidate rejected\n",
           k_stages[stage].name, forward_result ? "single-use result" : "two copy lifts");
    succeeded = true;
cleanup:
    if (!succeeded && error) fprintf(stderr, "%s\n", error);
    hlsl_lift_transaction_destroy(transaction);
    free(reconstructed);
    free(error);
    free(bytes);
    usil_free(&decoded);
    return succeeded;
}

static bool run_live_expression_fixture(UnityCompilerBroker *broker, int stage, bool shared,
                                        bool partial) {
    bool succeeded = false;
    USILProgram program = {0};
    uint8_t *bytes = NULL;
    size_t byte_count = 0;
    char *error = NULL;
    char *reconstructed = NULL;
    HLSLLiftTransaction *transaction = NULL;
    StringBuilder seed;
    sb_init(&seed);
    sb_append(&seed, "#pragma vertex main\n#pragma fragment main\n");
    sb_appendf(&seed, "float4 main(float4 value : %s) : %s {\n",
               stage == 0 ? "POSITION" : "TEXCOORD0", stage == 0 ? "SV_POSITION" : "SV_Target");
    sb_appendf(&seed, "float4 product = value * value.%s;\n", shared && !partial ? "yzwx" : "wzyx");
    sb_append(&seed,
              shared ? "return product + product.zwxy;\n}\n" : "return product * value.zwxy;\n}\n");
    SELF_CHECK(sb_ok(&seed));
    bytes = unity_compiler_broker_compile(broker, seed.buf, "ExpressionFixture", stage, 4, 0, NULL,
                                          0, NULL, 0, &byte_count, &error);
    SELF_CHECK(bytes);
    DXBCContainerView container;
    SELF_CHECK(dxbc_container_view_first(bytes, byte_count, &container));
    GoldenRecord target = {.name = "controlled-expression",
                           .bytecode = container.data,
                           .bytecode_size = container.size};
    RecordResult reconstruction = record_result_init(true);
    reconstructed = reconstruct_stage_source(&target, &k_stages[stage], &reconstruction, &program);
    SELF_CHECK(reconstructed && program.temp_count > 0);
    GoldenFlags flags = {.shader_name = "ExpressionFixture"};
    CompileJob job = {.stage = &k_stages[stage]};
    GoldenLiftCompiler compiler = {.broker = broker, .flags = &flags, .job = &job};
    HLSLLiftServices services = {.compile = compile_lift,
                                 .monotonic_ms = lift_monotonic_ms,
                                 .context = &compiler,
                                 .compile_high_level = compile_high_level,
                                 .require_request_identity = true};
    HLSLLiftResult result;
    HLSLLiftStatus baseline_status =
        hlsl_lift_transaction_begin(&program, target.bytecode, target.bytecode_size, &services,
                                    &k_copy_limits, &transaction, &result);
    if (baseline_status != HLSL_LIFT_VERIFIED)
        fprintf(stderr, "%s expression baseline: %s\n", k_stages[stage].name,
                hlsl_lift_status_name(baseline_status));
    SELF_CHECK(baseline_status == HLSL_LIFT_VERIFIED);
    if (!partial) {
        const char *baseline_source = hlsl_lift_transaction_artifact(transaction)->source;
        flags.shader_name = "ExpressionFixtureChangedAuthority";
        SELF_CHECK(hlsl_lift_transaction_try_high_level(transaction, &result) ==
                   HLSL_LIFT_AUTHORITY_MISMATCH);
        SELF_CHECK(!result.compared &&
                   hlsl_lift_transaction_artifact(transaction)->source == baseline_source);
        flags.shader_name = "ExpressionFixture";
    }
    HLSLLiftStatus status = hlsl_lift_transaction_try_high_level(transaction, &result);
    if (partial) {
        SELF_CHECK(status == HLSL_LIFT_EMISSION_REJECTED);
        SELF_CHECK(!hlsl_lift_transaction_is_high_level(transaction));
        SELF_CHECK(strcmp(hlsl_lift_transaction_artifact(transaction)->source, reconstructed) == 0);
        printf("%s: partial expression retained verified low-level fallback\n",
               k_stages[stage].name);
        succeeded = true;
        goto cleanup;
    }
    if (status != HLSL_LIFT_VERIFIED)
        fprintf(stderr, "%s %s expression: %s\n", k_stages[stage].name,
                shared ? "shared" : "nested", hlsl_lift_status_name(status));
    SELF_CHECK(status == HLSL_LIFT_VERIFIED);
    const char *accepted = hlsl_lift_transaction_artifact(transaction)->source;
    SELF_CHECK(!strstr(accepted, "float4 r") && !strstr(accepted, "u_xlat_temp"));
    SELF_CHECK(shared == (strstr(accepted, "const float4 dxbc_value_") != NULL));
    printf("%s: decoded %s expression reproduced complete target DXBC\n", k_stages[stage].name,
           shared ? "shared" : "nested");
    succeeded = true;
cleanup:
    if (!succeeded && error)
        fprintf(stderr, "%s\n", error);
    hlsl_lift_transaction_destroy(transaction);
    free(reconstructed);
    free(error);
    free(bytes);
    usil_free(&program);
    sb_free(&seed);
    return succeeded;
}

int main(int argc, char** argv) {
    CommandLine options;
    if (!parse_command_line(argc, argv, &options)) {
        print_usage(argv[0]);
        return 2;
    }
    if (options.self_test) return run_self_test() ? 0 : 1;

    char error[512] = {0};
    StringList cases;
    if (!discover_cases(options.golden_dir, options.case_name, &cases, error,
                        sizeof(error))) {
        fprintf(stderr, "golden verifier: %s\n", error);
        return 1;
    }
    if (!configure_unity_contents(options.unity_contents, error,
                                  sizeof(error))) {
        fprintf(stderr, "golden verifier: %s\n", error);
        string_list_free(&cases);
        return 1;
    }

    UnityCompilerBroker* broker = unity_compiler_broker_create_lazy(
        options.project_root, options.includes_dir);
    if (!broker) {
        fprintf(stderr, "golden verifier: compiler broker setup failed\n");
        string_list_free(&cases);
        return 1;
    }

    if (options.self_test_lifts) {
        bool passed =
            run_live_lift_fixture(broker, 0, false) && run_live_lift_fixture(broker, 1, false) &&
            run_live_lift_fixture(broker, 0, true) && run_live_lift_fixture(broker, 1, true) &&
            run_live_expression_fixture(broker, 0, false, false) &&
            run_live_expression_fixture(broker, 1, false, false) &&
            run_live_expression_fixture(broker, 0, true, false) &&
            run_live_expression_fixture(broker, 1, true, false) &&
            run_live_expression_fixture(broker, 0, true, true) &&
            run_live_expression_fixture(broker, 1, true, true);
        unity_compiler_broker_destroy(broker);
        string_list_free(&cases);
        return passed ? 0 : 1;
    }

    FILE* report = options.report_path ? fopen(options.report_path, "wx") : NULL;
    if (options.report_path && !report) {
        fprintf(stderr, "golden verifier: cannot create report: %s\n",
                strerror(errno));
        unity_compiler_broker_destroy(broker);
        string_list_free(&cases);
        return 1;
    }
    if (report) {
        UnityCompilerToolchainProvenance provenance;
        bool available = unity_compiler_broker_get_toolchain_provenance(
            broker, &provenance);
        char compiler_hash[65] = "", environment_hash[65] = "";
        if (available) {
            common_sha256_digest_to_hex(provenance.compiler_fingerprint,
                                        compiler_hash);
            common_sha256_digest_to_hex(provenance.environment_fingerprint,
                                        environment_hash);
        }
        fprintf(report,
                "{\"event\":\"run\",\"schema\":\"dxbc-golden-baseline-v1\","
                "\"mode\":\"%s\",\"discovered_cases\":%zu,"
                "\"authority\":\"legacy_fixture_controls\","
                "\"whole_shader_certificate\":\"not_requested\","
                "\"lift_copies\":%s,\"high_level\":%s,\"lift_max_candidates\":%zu,"
                "\"lift_max_compiles\":%zu,\"lift_acceptance_deadline_ms\":%" PRIu64 ","
                "\"compiler_sha256\":\"%s\",\"environment_sha256\":\"%s\"}\n",
                options.reconstruct ? "reconstruct" : "retained_source", cases.count,
                options.lift_copies ? "true" : "false", options.high_level ? "true" : "false",
                k_copy_limits.max_candidates, k_copy_limits.max_compiles,
                k_copy_limits.max_elapsed_ms, compiler_hash, environment_hash);
    }
    printf("Found %zu golden test cases. Reusing one compiler session.\n",
           cases.count);
    puts("============================================================");
    size_t failures = 0U;
    size_t assembly_only_matches = 0U;
    for (size_t index = 0U; index < cases.count; ++index) {
        printf("Running test: %s ...", cases.values[index]);
        fflush(stdout);
        bool assembly_only_match = false;
        if (verify_case(broker, options.golden_dir, cases.values[index], options.reconstruct,
                        options.lift_copies, options.high_level, report, &assembly_only_match)) {
            puts(" [PASS]");
        } else {
            puts(assembly_only_match ? " [FAIL: ASSEMBLY-ONLY]" : " [FAIL]");
            ++failures;
            if (assembly_only_match) ++assembly_only_matches;
        }
    }
    puts("============================================================");
    UnityCompilerBrokerStats stats;
    unity_compiler_broker_get_stats(broker, &stats);
    printf("Compiler requests: %" PRIu64 ", process starts: %" PRIu64
           "\n",
           stats.executed_requests, stats.compiler_process_starts);
    if (stats.compiler_process_starts > 1U) {
        fprintf(stderr,
                "golden verifier: expected at most one persistent compiler "
                "process; "
                "observed %" PRIu64 "\n",
                stats.compiler_process_starts);
        ++failures;
    }
    if (report) {
        fprintf(report, "{\"event\":\"end\",\"cases\":%zu,\"failed_cases\":%zu}\n",
                cases.count, failures);
        bool report_failed = ferror(report) != 0;
        if (fclose(report) != 0) report_failed = true;
        if (report_failed) {
            fprintf(stderr, "golden verifier: report write failed\n");
            ++failures;
        }
    }
    unity_compiler_broker_destroy(broker);
    string_list_free(&cases);
    if (failures != 0U) {
        fprintf(stderr,
                "%zu golden test(s) failed (%zu normalized-assembly-only "
                "match(es))\n",
                failures, assembly_only_matches);
        return 1;
    }
    puts("All golden tests passed.");
    return 0;
}
