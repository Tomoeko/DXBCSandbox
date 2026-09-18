// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "app/shader_batch.h"
#include "app/shader_catalog.h"
#include "app/material_batch.h"
#include "app/native_texture_batch.h"
#include "common/ascii_glob.h"
#include "common/common.h"
#include "common/file_io.h"
#include "common/sha256.h"
#include "common/string_builder.h"
#include "io/typetree_schema_registry.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

typedef enum {
    CLI_COMMAND_NONE = 0,
    CLI_COMMAND_LIST,
    CLI_COMMAND_EXTRACT,
} CliCommand;

typedef enum {
    CLI_FORMAT_TABLE = 0,
    CLI_FORMAT_JSON,
} CliFormat;

typedef enum {
    CLI_SHADER_KIND_ALL = 0,
    CLI_SHADER_KIND_GRAPHICS,
    CLI_SHADER_KIND_COMPUTE,
} CliShaderKind;

typedef struct {
    const char** values;
    size_t count;
} CliStringList;

typedef struct {
    size_t* values;
    size_t count;
} CliIndexList;

typedef struct {
    CliCommand command;
    CliFormat format;
    CliShaderKind shader_kind;
    bool recursive;
    bool all;
    bool show_sources;
    bool export_materials;
    bool flat_shaders;
    const char* output_directory;
    const char* schema_registry;
    const char* report_path;
    CliStringList inputs;
    CliStringList ids;
    CliStringList content_ids;
    CliStringList names;
    CliStringList patterns;
    CliIndexList indices;
} CliOptions;

typedef enum {
    SELECTOR_OK = 0,
    SELECTOR_UNMATCHED,
    SELECTOR_AMBIGUOUS,
    SELECTOR_INVALID,
    SELECTOR_ALLOCATION_FAILED,
} SelectorStatus;

static void print_usage(FILE* output, const char* program) {
    fprintf(output,
        "Usage:\n"
        "  %s list [OPTIONS] INPUT...\n"
        "  %s extract [OPTIONS] INPUT... (--all | SELECTOR...) --out DIR\n"
        "\n"
        "INPUT may be a Unity player folder, a bare SerializedFile, or a "
        "UnityFS bundle.\n"
        "Directories recurse by default. Unrelated descendants are skipped.\n"
        "\n"
        "Selectors (repeatable; combined as a union):\n"
        "  --index N          1-based row number in the current --kind view\n"
        "  --id ID            one source occurrence (o:DIGEST:PATH_ID)\n"
        "  --content-id ID    every identical content alias (s:DIGEST:PATH_ID)\n"
        "  --name NAME        exact, case-sensitive Shader name\n"
        "  --match GLOB       case-sensitive Shader-name glob\n"
        "  --all              every discovered asset of the selected kind "
        "(extract only)\n"
        "\n"
        "Options:\n"
        "  -o, --out DIR      output root for extract\n"
        "  --flat-shaders     place graphics .shader files directly in DIR\n"
        "                     using names derived from Shader \"name\"\n"
        "                     (default: SerializedFile digest subfolders)\n"
        "  --materials        also export every Material whose exact m_Shader\n"
        "                     PPtr resolves to a selected graphics Shader\n"
        "                     and exact Unity-native Texture2D/RenderTexture\n"
        "                     dependencies supported by the pinned layouts\n"
        "  --schema-registry FILE\n"
        "                     exact TypeTree registry override\n"
        "  --format table|json\n"
        "  --sources          include the serialized-source/TypeTree ledger\n"
        "                     in table output (JSON always includes it)\n"
        "  --kind all|graphics|compute\n"
        "                     filter the inventory/selection (default: all)\n"
        "  --report FILE      also publish this exact report (no overwrite)\n"
        "  --recursive        recurse into directories (default)\n"
        "  --no-recursive     scan direct directory children only\n"
        "  -h, --help\n"
        "\n"
        "Examples:\n"
        "  %s list /path/to/player\n"
        "  %s extract /path/to/player --kind graphics --all --out recovered\n"
        "  %s extract /path/to/player --kind graphics --all --flat-shaders "
        "--out recovered\n"
        "  %s extract /path/to/player --name 'Sprites/Default' --materials "
        "--out recovered\n"
        "  %s extract a.assets b.bundle --match 'Hidden/*' --out recovered\n",
        program, program, program, program, program, program, program);
#ifdef DXBCSANDBOX_LEGACY_ASSET_CLI
    fprintf(output,
        "\nLegacy compatibility:\n"
        "  %s INPUT [OUTPUT_DIR]\n"
        "  (equivalent to extract INPUT --all; default output is "
        "../out_shaders)\n", program);
#endif
}

static void cli_options_dispose(CliOptions* options) {
    if (!options) return;
    free(options->inputs.values);
    free(options->ids.values);
    free(options->content_ids.values);
    free(options->names.values);
    free(options->patterns.values);
    free(options->indices.values);
    memset(options, 0, sizeof(*options));
}

static char* duplicate_c_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1U);
    if (!copy) return NULL;
    memcpy(copy, value, size + 1U);
    return copy;
}

static bool append_string(CliStringList* list, const char* value) {
    if (!list || !value || !value[0] || list->count == SIZE_MAX ||
        dxbc_size_multiply_overflows(
            list->count + 1U, sizeof(*list->values))) {
        return false;
    }
    const char** values = (const char**)realloc(
        list->values, (list->count + 1U) * sizeof(*list->values));
    if (!values) return false;
    list->values = values;
    values[list->count++] = value;
    return true;
}

static bool append_index(CliIndexList* list, size_t value) {
    if (!list || value == 0U || list->count == SIZE_MAX ||
        dxbc_size_multiply_overflows(
            list->count + 1U, sizeof(*list->values))) {
        return false;
    }
    size_t* values = (size_t*)realloc(
        list->values, (list->count + 1U) * sizeof(*list->values));
    if (!values) return false;
    list->values = values;
    values[list->count++] = value;
    return true;
}

static bool parse_positive_size(const char* text, size_t* value) {
    if (!text || !text[0] || !value || text[0] == '-') return false;
    errno = 0;
    char* end = NULL;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0U ||
        parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool is_ascii_hex(unsigned char value) {
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

static bool identity_selector_is_valid(const char* value,
                                       char expected_prefix) {
    if (!value || !value[0]) return false;
    if (value[1] == ':') {
        if (value[0] != expected_prefix) return false;
        value += 2U;
    }
    const char* colon = strrchr(value, ':');
    if (!colon || colon == value) return false;
    size_t digest_size = (size_t)(colon - value);
    if (digest_size < 8U ||
        digest_size > COMMON_SHA256_DIGEST_SIZE * 2U) {
        return false;
    }
    for (size_t i = 0U; i < digest_size; ++i) {
        if (!is_ascii_hex((unsigned char)value[i])) return false;
    }
    const char* path = colon + 1U;
    if (*path == '-') ++path;
    if (!*path) return false;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    errno = 0;
    char* end = NULL;
    (void)strtoimax(colon + 1U, &end, 10);
    return errno == 0 && end && *end == '\0';
}

static bool option_value(int argc, char** argv, int* index,
                         const char* option, const char** value) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '\0') {
        fprintf(stderr, "Error: %s requires a value.\n", option);
        return false;
    }
    *value = argv[++*index];
    return true;
}

static bool has_selectors(const CliOptions* options) {
    return options->indices.count != 0U || options->ids.count != 0U ||
        options->content_ids.count != 0U ||
        options->names.count != 0U || options->patterns.count != 0U;
}

static bool parse_cli(int argc, char** argv, CliOptions* options,
                      bool* help_requested) {
    memset(options, 0, sizeof(*options));
    options->format = CLI_FORMAT_TABLE;
    options->shader_kind = CLI_SHADER_KIND_ALL;
    options->recursive = true;
    *help_requested = false;
    if (argc < 2) return false;

#ifdef DXBCSANDBOX_LEGACY_ASSET_CLI
    int first_argument = 1;
    bool legacy_implicit_extract = false;
    if (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "extract") == 0) {
        options->command = strcmp(argv[1], "list") == 0
            ? CLI_COMMAND_LIST : CLI_COMMAND_EXTRACT;
        first_argument = 2;
    } else if (strcmp(argv[1], "--help") == 0 ||
               strcmp(argv[1], "-h") == 0) {
        *help_requested = true;
        return true;
    } else {
        options->command = CLI_COMMAND_EXTRACT;
        options->all = true;
        legacy_implicit_extract = true;
    }
#else
    int first_argument = 2;
    if (strcmp(argv[1], "list") == 0) {
        options->command = CLI_COMMAND_LIST;
    } else if (strcmp(argv[1], "extract") == 0) {
        options->command = CLI_COMMAND_EXTRACT;
    } else if (strcmp(argv[1], "--help") == 0 ||
               strcmp(argv[1], "-h") == 0) {
        *help_requested = true;
        return true;
    } else {
        fprintf(stderr, "Error: expected `list` or `extract`.\n");
        return false;
    }
#endif

    bool positional_only = false;
    for (int i = first_argument; i < argc; ++i) {
        const char* argument = argv[i];
        if (!positional_only && strcmp(argument, "--") == 0) {
            positional_only = true;
        } else if (!positional_only &&
                   (strcmp(argument, "--help") == 0 ||
                    strcmp(argument, "-h") == 0)) {
            *help_requested = true;
            return true;
        } else if (!positional_only && strcmp(argument, "--recursive") == 0) {
            options->recursive = true;
        } else if (!positional_only &&
                   strcmp(argument, "--no-recursive") == 0) {
            options->recursive = false;
        } else if (!positional_only && strcmp(argument, "--all") == 0) {
            options->all = true;
        } else if (!positional_only && strcmp(argument, "--sources") == 0) {
            options->show_sources = true;
        } else if (!positional_only && strcmp(argument, "--materials") == 0) {
            options->export_materials = true;
        } else if (!positional_only &&
                   strcmp(argument, "--flat-shaders") == 0) {
            options->flat_shaders = true;
        } else if (!positional_only &&
                   (strcmp(argument, "--out") == 0 ||
                    strcmp(argument, "-o") == 0 ||
                    strcmp(argument, "--out-dir") == 0)) {
            if (!option_value(argc, argv, &i, argument,
                              &options->output_directory)) return false;
        } else if (!positional_only &&
                   strcmp(argument, "--schema-registry") == 0) {
            if (!option_value(argc, argv, &i, argument,
                              &options->schema_registry)) return false;
        } else if (!positional_only &&
                   strncmp(argument, "--schema-registry=", 18U) == 0) {
            if (!argument[18]) {
                fprintf(stderr,
                        "Error: --schema-registry requires a value.\n");
                return false;
            }
            options->schema_registry = argument + 18U;
        } else if (!positional_only && strcmp(argument, "--report") == 0) {
            if (!option_value(argc, argv, &i, argument,
                              &options->report_path)) return false;
        } else if (!positional_only && strcmp(argument, "--format") == 0) {
            const char* format = NULL;
            if (!option_value(argc, argv, &i, argument, &format)) return false;
            if (strcmp(format, "table") == 0) {
                options->format = CLI_FORMAT_TABLE;
            } else if (strcmp(format, "json") == 0) {
                options->format = CLI_FORMAT_JSON;
            } else {
                fprintf(stderr,
                        "Error: --format must be `table` or `json`.\n");
                return false;
            }
        } else if (!positional_only && strcmp(argument, "--kind") == 0) {
            const char* kind = NULL;
            if (!option_value(argc, argv, &i, argument, &kind)) return false;
            if (strcmp(kind, "all") == 0) {
                options->shader_kind = CLI_SHADER_KIND_ALL;
            } else if (strcmp(kind, "graphics") == 0) {
                options->shader_kind = CLI_SHADER_KIND_GRAPHICS;
            } else if (strcmp(kind, "compute") == 0) {
                options->shader_kind = CLI_SHADER_KIND_COMPUTE;
            } else {
                fprintf(stderr,
                        "Error: --kind must be `all`, `graphics`, or "
                        "`compute`.\n");
                return false;
            }
        } else if (!positional_only &&
                   strncmp(argument, "--kind=", 7U) == 0) {
            const char* kind = argument + 7U;
            if (strcmp(kind, "all") == 0) {
                options->shader_kind = CLI_SHADER_KIND_ALL;
            } else if (strcmp(kind, "graphics") == 0) {
                options->shader_kind = CLI_SHADER_KIND_GRAPHICS;
            } else if (strcmp(kind, "compute") == 0) {
                options->shader_kind = CLI_SHADER_KIND_COMPUTE;
            } else {
                fprintf(stderr,
                        "Error: --kind must be `all`, `graphics`, or "
                        "`compute`.\n");
                return false;
            }
        } else if (!positional_only && strcmp(argument, "--index") == 0) {
            const char* text = NULL;
            size_t value = 0U;
            if (!option_value(argc, argv, &i, argument, &text) ||
                !parse_positive_size(text, &value) ||
                !append_index(&options->indices, value)) {
                fprintf(stderr, "Error: --index requires a positive integer.\n");
                return false;
            }
        } else if (!positional_only && strcmp(argument, "--id") == 0) {
            const char* value = NULL;
            if (!option_value(argc, argv, &i, argument, &value) ||
                !identity_selector_is_valid(value, 'o') ||
                !append_string(&options->ids, value)) {
                fprintf(stderr,
                        "Error: --id requires o:DIGEST:PATH_ID (at least "
                        "8 hexadecimal digest characters).\n");
                return false;
            }
        } else if (!positional_only &&
                   strcmp(argument, "--content-id") == 0) {
            const char* value = NULL;
            if (!option_value(argc, argv, &i, argument, &value) ||
                !identity_selector_is_valid(value, 's') ||
                !append_string(&options->content_ids, value)) {
                fprintf(stderr,
                        "Error: --content-id requires s:DIGEST:PATH_ID (at "
                        "least 8 hexadecimal digest characters).\n");
                return false;
            }
        } else if (!positional_only && strcmp(argument, "--name") == 0) {
            const char* value = NULL;
            if (!option_value(argc, argv, &i, argument, &value) ||
                !append_string(&options->names, value)) return false;
        } else if (!positional_only && strcmp(argument, "--match") == 0) {
            const char* value = NULL;
            if (!option_value(argc, argv, &i, argument, &value) ||
                ascii_glob_validate(value) != ASCII_GLOB_OK ||
                !append_string(&options->patterns, value)) {
                fprintf(stderr, "Error: --match requires a valid glob.\n");
                return false;
            }
        } else if (!positional_only && argument[0] == '-') {
            if (strcmp(argument, "--filter") == 0) {
                fprintf(stderr,
                        "Error: legacy --filter is unsafe and unsupported; "
                        "use --index, --id, --name, or --match.\n");
            } else {
                fprintf(stderr, "Error: unknown option: %s\n", argument);
            }
            return false;
        } else {
#ifdef DXBCSANDBOX_LEGACY_ASSET_CLI
            if (legacy_implicit_extract && options->inputs.count != 0U) {
                if (options->output_directory) {
                    fprintf(stderr,
                            "Error: unexpected extra positional argument: "
                            "%s\n", argument);
                    return false;
                }
                options->output_directory = argument;
            } else
#endif
            if (!append_string(&options->inputs, argument)) {
                return false;
            }
        }
    }

    if (options->inputs.count == 0U) {
        fprintf(stderr, "Error: at least one INPUT is required.\n");
        return false;
    }
    if (options->command == CLI_COMMAND_LIST && options->all) {
        fprintf(stderr, "Error: --all is implicit for list; omit it.\n");
        return false;
    }
    if (options->command == CLI_COMMAND_LIST && options->output_directory) {
        fprintf(stderr, "Error: --out is valid only with extract.\n");
        return false;
    }
    if (options->command == CLI_COMMAND_LIST && options->export_materials) {
        fprintf(stderr, "Error: --materials is valid only with extract.\n");
        return false;
    }
    if (options->command == CLI_COMMAND_LIST && options->flat_shaders) {
        fprintf(stderr,
                "Error: --flat-shaders is valid only with extract.\n");
        return false;
    }
    if (options->command == CLI_COMMAND_EXTRACT) {
#ifdef DXBCSANDBOX_LEGACY_ASSET_CLI
        if (legacy_implicit_extract && !options->output_directory) {
            options->output_directory = "../out_shaders";
        }
#endif
        if (!options->output_directory) {
            fprintf(stderr, "Error: extract requires --out DIRECTORY.\n");
            return false;
        }
        if (options->all == has_selectors(options)) {
            fprintf(stderr,
                    "Error: extract requires exactly one of --all or one or "
                    "more selectors.\n");
            return false;
        }
    }
    return true;
}

static bool parse_expected_default_digest(
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    static const uint8_t expected[COMMON_SHA256_DIGEST_SIZE] = {
        0xa4, 0x9a, 0x83, 0x82, 0x48, 0xed, 0x56, 0xc2,
        0xc1, 0xc2, 0x44, 0xa2, 0x0a, 0x34, 0xcb, 0x17,
        0xca, 0xae, 0x6f, 0xd9, 0xd0, 0x59, 0x6a, 0x2f,
        0xf9, 0x97, 0x17, 0xf7, 0x45, 0x1e, 0x2e, 0xcc,
    };
    memcpy(digest, expected, sizeof(expected));
    return true;
}

static char* join_path_alloc(const char* directory, const char* suffix) {
    size_t directory_size = directory ? strlen(directory) : 0U;
    size_t suffix_size = suffix ? strlen(suffix) : 0U;
    bool separator = directory_size != 0U &&
        directory[directory_size - 1U] != '/' &&
        directory[directory_size - 1U] != '\\';
    size_t extra = separator ? 1U : 0U;
    if (!directory || !suffix || directory_size > SIZE_MAX - extra ||
        directory_size + extra > SIZE_MAX - suffix_size ||
        directory_size + extra + suffix_size == SIZE_MAX) return NULL;
    size_t total = directory_size + extra + suffix_size;
    char* result = (char*)malloc(total + 1U);
    if (!result) return NULL;
    memcpy(result, directory, directory_size);
    if (separator) {
#ifdef _WIN32
        result[directory_size] = '\\';
#else
        result[directory_size] = '/';
#endif
    }
    memcpy(result + directory_size + extra, suffix, suffix_size + 1U);
    return result;
}

static char* directory_from_path(const char* path) {
    if (!path || !path[0]) return NULL;
    const char* last = NULL;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') last = cursor;
    }
    if (!last) return NULL;
    size_t size = (size_t)(last - path);
#ifdef _WIN32
    size_t root_size = 0U;
    if (!common_windows_directory_root_length(
            path, strlen(path), &root_size)) {
        return NULL;
    }
    if (size < root_size) size = root_size;
#endif
    if (size == 0U) size = 1U;
    char* directory = (char*)malloc(size + 1U);
    if (!directory) return NULL;
    memcpy(directory, path, size);
    directory[size] = '\0';
    return directory;
}

static char* running_executable_path(void) {
#ifdef _WIN32
    DWORD capacity = 512U;
    while (capacity <= 32768U) {
        wchar_t* path = (wchar_t*)malloc(
            (size_t)capacity * sizeof(*path));
        if (!path) return NULL;
        SetLastError(ERROR_SUCCESS);
        DWORD size = GetModuleFileNameW(NULL, path, capacity);
        if (size != 0U && size < capacity) {
            path[size] = L'\0';
            char* utf8 = common_windows_wide_to_utf8(path);
            free(path);
            return utf8;
        }
        DWORD error = GetLastError();
        free(path);
        if (size == 0U || (size < capacity &&
                           error != ERROR_INSUFFICIENT_BUFFER) ||
            capacity > 16384U) {
            return NULL;
        }
        capacity *= 2U;
    }
    return NULL;
#elif defined(__APPLE__)
    uint32_t capacity = 1024U;
    while (capacity <= 1024U * 1024U) {
        char* path = (char*)malloc((size_t)capacity);
        if (!path) return NULL;
        uint32_t required = capacity;
        if (_NSGetExecutablePath(path, &required) == 0) {
            char* canonical = realpath(path, NULL);
            if (canonical) {
                free(path);
                return canonical;
            }
            return path;
        }
        free(path);
        if (required <= capacity) return NULL;
        capacity = required;
    }
    return NULL;
#else
    size_t capacity = 512U;
    while (capacity <= 1024U * 1024U) {
        char* path = (char*)malloc(capacity + 1U);
        if (!path) return NULL;
        ssize_t size = readlink("/proc/self/exe", path, capacity);
        if (size >= 0 && (size_t)size < capacity) {
            path[(size_t)size] = '\0';
            return path;
        }
        free(path);
        if (size < 0 || capacity > (1024U * 1024U) / 2U) return NULL;
        capacity *= 2U;
    }
    return NULL;
#endif
}

static bool executable_candidate_exists(const char* path) {
#ifdef _WIN32
    wchar_t* wide_path = common_windows_utf8_to_wide(path);
    if (!wide_path) return false;
    DWORD attributes = GetFileAttributesW(wide_path);
    free(wide_path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT |
                       FILE_ATTRIBUTE_DEVICE)) == 0U;
#else
    FILE* stream = fopen(path, "rb");
    if (!stream) return false;
    fclose(stream);
    return true;
#endif
}

static char* executable_directory(const char* executable) {
    char* running_path = running_executable_path();
    if (running_path) {
        char* directory = directory_from_path(running_path);
        free(running_path);
        if (directory) return directory;
    }
    char* directory = directory_from_path(executable);
    if (directory) return directory;
    if (!executable || !executable[0]) return NULL;

    {
        char* search_storage = NULL;
#ifdef _WIN32
        const wchar_t* wide_search = _wgetenv(L"PATH");
        if (wide_search) {
            search_storage = common_windows_wide_to_utf8(wide_search);
        }
        const char* search = search_storage;
#else
        const char* search = getenv("PATH");
#endif
        if (!search) return NULL;
#ifdef _WIN32
        const char delimiter = ';';
#else
        const char delimiter = ':';
#endif
        const char* entry = search;
        while (true) {
            const char* end = strchr(entry, delimiter);
            size_t entry_size = end ? (size_t)(end - entry) : strlen(entry);
            char* candidate_directory = NULL;
            if (entry_size == 0U) {
                candidate_directory = duplicate_c_string(".");
            } else {
                candidate_directory = (char*)malloc(entry_size + 1U);
                if (candidate_directory) {
                    memcpy(candidate_directory, entry, entry_size);
                    candidate_directory[entry_size] = '\0';
                }
            }
            if (!candidate_directory) {
                free(search_storage);
                return NULL;
            }
            char* candidate = join_path_alloc(
                candidate_directory, executable);
            if (!candidate) {
                free(candidate_directory);
                free(search_storage);
                return NULL;
            }
            bool exists = executable_candidate_exists(candidate);
            free(candidate);
            if (exists) {
                free(search_storage);
                return candidate_directory;
            }
            free(candidate_directory);
            if (!end) break;
            entry = end + 1U;
        }
        free(search_storage);
        return NULL;
    }
}

static TypeTreeSchemaStatus import_registry_checked(
    TypeTreeSchemaRegistry* registry, const char* path,
    bool require_default_digest,
    uint8_t loaded_digest[COMMON_SHA256_DIGEST_SIZE]) {
    CommonFileBytes file;
    CommonFileStatus file_status = common_file_read_regular(
        path, SIZE_MAX, &file);
    if (file_status != COMMON_FILE_OK) return TYPETREE_SCHEMA_IO_ERROR;
    uint8_t actual[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(file.data, file.size, actual);
    if (require_default_digest) {
        uint8_t expected[COMMON_SHA256_DIGEST_SIZE];
        (void)parse_expected_default_digest(expected);
        if (memcmp(actual, expected, sizeof(actual)) != 0) {
            common_file_bytes_dispose(&file);
            return TYPETREE_SCHEMA_DIGEST_MISMATCH;
        }
    }
    TypeTreeSchemaStatus status =
        typetree_schema_registry_deserialize_replace(
            registry, file.data, file.size);
    if (status == TYPETREE_SCHEMA_OK && loaded_digest) {
        memcpy(loaded_digest, actual, sizeof(actual));
    }
    common_file_bytes_dispose(&file);
    return status;
}

static TypeTreeSchemaStatus load_schema_registry(
    TypeTreeSchemaRegistry* registry, const char* explicit_path,
    const char* executable, char** resolved_path,
    uint8_t loaded_digest[COMMON_SHA256_DIGEST_SIZE]) {
    *resolved_path = NULL;
    if (explicit_path) {
        TypeTreeSchemaStatus status = import_registry_checked(
            registry, explicit_path, false, loaded_digest);
        if (status == TYPETREE_SCHEMA_OK) {
            *resolved_path = duplicate_c_string(explicit_path);
            if (!*resolved_path) return TYPETREE_SCHEMA_ALLOCATION_FAILED;
        }
        return status;
    }
    static const char relative[] =
        "schemas/unity-2021.3-player-shader.registry";
    char* directory = executable_directory(executable);
    char* candidates[4] = {NULL, NULL, NULL, NULL};
    size_t candidate_count = 0U;
    if (directory) {
        candidates[candidate_count++] = join_path_alloc(directory, relative);
        candidates[candidate_count++] = join_path_alloc(
            directory, "../share/dxbc-sandbox/"
                       "unity-2021.3-player-shader.registry");
        candidates[candidate_count++] = join_path_alloc(
            directory, "../schemas/"
                       "unity-2021.3-player-shader.registry");
    }
    candidates[candidate_count++] = duplicate_c_string(relative);
    free(directory);
    TypeTreeSchemaStatus final_status = TYPETREE_SCHEMA_IO_ERROR;
    for (size_t i = 0U; i < candidate_count; ++i) {
        if (!candidates[i]) {
            final_status = TYPETREE_SCHEMA_ALLOCATION_FAILED;
            break;
        }
        TypeTreeSchemaStatus status = import_registry_checked(
            registry, candidates[i], true, loaded_digest);
        if (status == TYPETREE_SCHEMA_OK) {
            *resolved_path = candidates[i];
            candidates[i] = NULL;
            final_status = status;
            break;
        }
        if (status != TYPETREE_SCHEMA_IO_ERROR) {
            final_status = status;
            break;
        }
    }
    for (size_t i = 0U; i < candidate_count; ++i) free(candidates[i]);
    return final_status;
}

static bool record_matches_identity(const ShaderCatalogRecord* record,
                                    const char* selector,
                                    bool content_identity) {
    const char* full_id = content_identity
        ? record->content_id : record->occurrence_id;
    const char* digest = content_identity
        ? record->serialized_digest_hex : record->occurrence_digest_hex;
    if (strcmp(full_id, selector) == 0) return true;
    if (selector[0] != '\0' && selector[1] == ':') {
        const char expected = content_identity ? 's' : 'o';
        if (selector[0] != expected) return false;
        selector += 2U;
    }
    const char* colon = strrchr(selector, ':');
    if (!colon || colon == selector) return false;
    size_t digest_size = (size_t)(colon - selector);
    if (digest_size < 8U || digest_size >
            COMMON_SHA256_DIGEST_SIZE * 2U) {
        return false;
    }
    for (size_t i = 0U; i < digest_size; ++i) {
        unsigned char candidate = (unsigned char)selector[i];
        if (candidate >= 'A' && candidate <= 'F') {
            candidate = (unsigned char)(candidate - 'A' + 'a');
        }
        if ((unsigned char)digest[i] != candidate) return false;
    }
    char path_id[32];
    int written = snprintf(path_id, sizeof(path_id), "%" PRId64,
                           record->path_id);
    return written > 0 && (size_t)written < sizeof(path_id) &&
           strcmp(path_id, colon + 1U) == 0;
}

static const char* cli_shader_kind_name(CliShaderKind kind) {
    switch (kind) {
        case CLI_SHADER_KIND_ALL: return "all";
        case CLI_SHADER_KIND_GRAPHICS: return "graphics";
        case CLI_SHADER_KIND_COMPUTE: return "compute";
        default: return "unknown";
    }
}

static const char* catalog_record_kind_name(
    const ShaderCatalogRecord* record) {
    if (!record) return "unknown";
    if (record->class_id == 48) return "graphics";
    if (record->class_id == 72) return "compute";
    return "unknown";
}

static bool record_matches_kind(const ShaderCatalogRecord* record,
                                CliShaderKind kind) {
    if (!record) return false;
    if (kind == CLI_SHADER_KIND_ALL) return true;
    if (kind == CLI_SHADER_KIND_GRAPHICS) return record->class_id == 48;
    if (kind == CLI_SHADER_KIND_COMPUTE) return record->class_id == 72;
    return false;
}

static size_t selected_record_count(const ShaderCatalog* catalog,
                                    const bool* selected) {
    if (!catalog || (!selected && catalog->record_count != 0U)) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (selected[i]) ++count;
    }
    return count;
}

static bool catalog_inventory_is_complete(const ShaderCatalog* catalog) {
    if (!catalog || catalog->issue_count != 0U ||
        catalog->stats.shader_objects != catalog->record_count) {
        return false;
    }
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        const ShaderCatalogRecord* record = &catalog->records[i];
        if (!record->name) return false;
        if (record->class_id == 48) continue;
        if (record->class_id == 72 &&
            record->compute_inventory_status ==
                COMPUTE_SHADER_INVENTORY_OK) {
            continue;
        }
        return false;
    }
    return true;
}

static bool record_artifact_is_ready(const ShaderCatalogRecord* record) {
    return record && record->status == SHADER_CATALOG_RECORD_READY;
}

static bool record_source_conversion_is_ready(
    const ShaderCatalogRecord* record) {
    if (!record_artifact_is_ready(record)) return false;
    if (record->class_id == 48) return true;
    return record->class_id == 72 &&
        record->compute_source_authority_status ==
            COMPUTE_SHADER_SOURCE_AUTHORITY_EXACT;
}

static bool selected_artifact_is_ready(const ShaderCatalog* catalog,
                                       const bool* selected) {
    const size_t count = selected_record_count(catalog, selected);
    if (count == 0U) return false;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (selected[i] && !record_artifact_is_ready(&catalog->records[i])) {
            return false;
        }
    }
    return true;
}

static bool selected_conversion_is_ready(const ShaderCatalog* catalog,
                                         const bool* selected) {
    const size_t count = selected_record_count(catalog, selected);
    if (count == 0U) return false;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (selected[i] &&
            !record_source_conversion_is_ready(&catalog->records[i])) {
            return false;
        }
    }
    return true;
}

static bool conversion_catalog_is_complete(const ShaderCatalog* catalog) {
    if (!catalog || catalog->issue_count != 0U ||
        catalog->record_count != catalog->stats.shader_objects) {
        return false;
    }
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (!record_source_conversion_is_ready(&catalog->records[i])) {
            return false;
        }
    }
    return true;
}

static SelectorStatus apply_selectors(const CliOptions* options,
                                      const ShaderCatalog* catalog,
                                      bool* selected,
                                      const char** failed_selector) {
    *failed_selector = NULL;
    if (options->all || !has_selectors(options)) {
        for (size_t i = 0U; i < catalog->record_count; ++i) {
            selected[i] = record_matches_kind(&catalog->records[i],
                                              options->shader_kind);
        }
        return SELECTOR_OK;
    }
    for (size_t i = 0U; i < options->indices.count; ++i) {
        const size_t requested = options->indices.values[i];
        size_t view_index = 0U;
        size_t catalog_index = SIZE_MAX;
        for (size_t record_index = 0U;
             record_index < catalog->record_count; ++record_index) {
            if (!record_matches_kind(&catalog->records[record_index],
                                     options->shader_kind)) {
                continue;
            }
            ++view_index;
            if (view_index == requested) {
                catalog_index = record_index;
                break;
            }
        }
        if (requested == 0U || catalog_index == SIZE_MAX) {
            return SELECTOR_UNMATCHED;
        }
        selected[catalog_index] = true;
    }
    for (size_t selector_index = 0U;
         selector_index < options->ids.count; ++selector_index) {
        const char* selector = options->ids.values[selector_index];
        const char* identity = NULL;
        size_t matches = 0U;
        for (size_t i = 0U; i < catalog->record_count; ++i) {
            if (!record_matches_kind(&catalog->records[i],
                                     options->shader_kind)) continue;
            if (!record_matches_identity(
                    &catalog->records[i], selector, false)) continue;
            if (!identity) {
                identity = catalog->records[i].occurrence_id;
            } else if (strcmp(identity,
                              catalog->records[i].occurrence_id) != 0) {
                *failed_selector = selector;
                return SELECTOR_AMBIGUOUS;
            }
            selected[i] = true;
            ++matches;
        }
        if (matches == 0U) {
            *failed_selector = selector;
            return SELECTOR_UNMATCHED;
        }
    }
    for (size_t selector_index = 0U;
         selector_index < options->content_ids.count; ++selector_index) {
        const char* selector = options->content_ids.values[selector_index];
        const char* identity = NULL;
        size_t matches = 0U;
        for (size_t i = 0U; i < catalog->record_count; ++i) {
            if (!record_matches_kind(&catalog->records[i],
                                     options->shader_kind)) continue;
            if (!record_matches_identity(
                    &catalog->records[i], selector, true)) continue;
            if (!identity) {
                identity = catalog->records[i].content_id;
            } else if (strcmp(identity,
                              catalog->records[i].content_id) != 0) {
                *failed_selector = selector;
                return SELECTOR_AMBIGUOUS;
            }
            selected[i] = true;
            ++matches;
        }
        if (matches == 0U) {
            *failed_selector = selector;
            return SELECTOR_UNMATCHED;
        }
    }
    for (size_t selector_index = 0U;
         selector_index < options->names.count; ++selector_index) {
        const char* selector = options->names.values[selector_index];
        size_t matches = 0U;
        for (size_t i = 0U; i < catalog->record_count; ++i) {
            if (!record_matches_kind(&catalog->records[i],
                                     options->shader_kind)) continue;
            if (catalog->records[i].name &&
                strcmp(catalog->records[i].name, selector) == 0) {
                selected[i] = true;
                ++matches;
            }
        }
        if (matches == 0U) {
            *failed_selector = selector;
            return SELECTOR_UNMATCHED;
        }
    }
    for (size_t selector_index = 0U;
         selector_index < options->patterns.count; ++selector_index) {
        const char* selector = options->patterns.values[selector_index];
        size_t matches = 0U;
        for (size_t i = 0U; i < catalog->record_count; ++i) {
            if (!record_matches_kind(&catalog->records[i],
                                     options->shader_kind)) continue;
            bool match = false;
            AsciiGlobStatus glob_status = catalog->records[i].name
                ? ascii_glob_match(selector, catalog->records[i].name,
                                   &match)
                : ASCII_GLOB_OK;
            if (glob_status == ASCII_GLOB_ALLOCATION_FAILED) {
                *failed_selector = selector;
                return SELECTOR_ALLOCATION_FAILED;
            }
            if (glob_status != ASCII_GLOB_OK) {
                *failed_selector = selector;
                return SELECTOR_INVALID;
            }
            if (match) {
                selected[i] = true;
                ++matches;
            }
        }
        if (matches == 0U) {
            *failed_selector = selector;
            return SELECTOR_UNMATCHED;
        }
    }
    return SELECTOR_OK;
}

static void sb_json_string(StringBuilder* builder, const char* value) {
    if (!value) {
        sb_append(builder, "null");
        return;
    }
    sb_append_char(builder, '"');
    for (const unsigned char* cursor = (const unsigned char*)value;
         *cursor;) {
        unsigned char byte = *cursor;
        if (byte == '"') { sb_append(builder, "\\\""); ++cursor; }
        else if (byte == '\\') { sb_append(builder, "\\\\"); ++cursor; }
        else if (byte == '\b') { sb_append(builder, "\\b"); ++cursor; }
        else if (byte == '\f') { sb_append(builder, "\\f"); ++cursor; }
        else if (byte == '\n') { sb_append(builder, "\\n"); ++cursor; }
        else if (byte == '\r') { sb_append(builder, "\\r"); ++cursor; }
        else if (byte == '\t') { sb_append(builder, "\\t"); ++cursor; }
        else if (byte < 0x20U) {
            sb_appendf(builder, "\\u%04x", (unsigned)byte);
            ++cursor;
        } else if (byte < 0x80U) {
            sb_append_char(builder, (char)byte);
            ++cursor;
        } else {
            size_t sequence_size = 0U;
            if (byte >= 0xc2U && byte <= 0xdfU &&
                cursor[1] >= 0x80U && cursor[1] <= 0xbfU) {
                sequence_size = 2U;
            } else if (byte == 0xe0U && cursor[1] >= 0xa0U &&
                       cursor[1] <= 0xbfU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU) {
                sequence_size = 3U;
            } else if (((byte >= 0xe1U && byte <= 0xecU) ||
                        (byte >= 0xeeU && byte <= 0xefU)) &&
                       cursor[1] >= 0x80U && cursor[1] <= 0xbfU &&
                       cursor[2] >= 0x80U && cursor[2] <= 0xbfU) {
                sequence_size = 3U;
            } else if (byte == 0xedU && cursor[1] >= 0x80U &&
                       cursor[1] <= 0x9fU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU) {
                sequence_size = 3U;
            } else if (byte == 0xf0U && cursor[1] >= 0x90U &&
                       cursor[1] <= 0xbfU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU && cursor[3] >= 0x80U &&
                       cursor[3] <= 0xbfU) {
                sequence_size = 4U;
            } else if (byte >= 0xf1U && byte <= 0xf3U &&
                       cursor[1] >= 0x80U && cursor[1] <= 0xbfU &&
                       cursor[2] >= 0x80U && cursor[2] <= 0xbfU &&
                       cursor[3] >= 0x80U && cursor[3] <= 0xbfU) {
                sequence_size = 4U;
            } else if (byte == 0xf4U && cursor[1] >= 0x80U &&
                       cursor[1] <= 0x8fU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU && cursor[3] >= 0x80U &&
                       cursor[3] <= 0xbfU) {
                sequence_size = 4U;
            }
            if (sequence_size != 0U) {
                sb_append_len(builder, (const char*)cursor, sequence_size);
                cursor += sequence_size;
            } else {
                /* Invalid POSIX filename bytes are escaped deterministically
                 * while valid UTF-8 is preserved verbatim. */
                sb_appendf(builder, "\\u%04x", (unsigned)byte);
                ++cursor;
            }
        }
    }
    sb_append_char(builder, '"');
}

static void sb_table_text(StringBuilder* builder, const char* value) {
    if (!value) {
        sb_append(builder, "<unknown>");
        return;
    }
    for (const unsigned char* cursor = (const unsigned char*)value;
         *cursor; ++cursor) {
        sb_append_char(builder, *cursor < 0x20U ? ' ' : (char)*cursor);
    }
}

static void sb_json_bytes(StringBuilder* builder, const uint8_t* value,
                          size_t size) {
    if (!value && size != 0U) {
        sb_append(builder, "null");
        return;
    }
    sb_append_char(builder, '"');
    for (size_t index = 0U; index < size; ++index) {
        unsigned char byte = value[index];
        if (byte == '"') sb_append(builder, "\\\"");
        else if (byte == '\\') sb_append(builder, "\\\\");
        else if (byte == '\b') sb_append(builder, "\\b");
        else if (byte == '\f') sb_append(builder, "\\f");
        else if (byte == '\n') sb_append(builder, "\\n");
        else if (byte == '\r') sb_append(builder, "\\r");
        else if (byte == '\t') sb_append(builder, "\\t");
        else if (byte < 0x20U) {
            sb_appendf(builder, "\\u%04x", (unsigned)byte);
        } else {
            sb_append_char(builder, (char)byte);
        }
    }
    sb_append_char(builder, '"');
}

static void append_catalog_stats_json(StringBuilder* output,
                                      const ShaderCatalog* catalog) {
    const ShaderCatalogStats* s = &catalog->stats;
    sb_appendf(output,
        "{\"requested_inputs\":%zu,"
        "\"discovered_files\":%zu,\"ignored_unrelated_files\":%zu,"
        "\"unityfs_files\":%zu,\"standalone_serialized_files\":%zu,"
        "\"serialized_sources\":%zu,"
        "\"visited_serialized_sources\":%zu,\"shaders\":%zu,"
        "\"compute_shaders\":%zu,\"named_compute_shaders\":%zu,"
        "\"materials_included\":%s,\"materials\":%zu,"
        "\"decoded_materials\":%zu,"
        "\"resolved_material_shader_links\":%zu,"
        "\"unresolved_material_shader_links\":%zu,"
        "\"null_material_shader_links\":%zu,"
        "\"failed_materials\":%zu,"
        "\"decoded\":%zu,\"ready\":%zu,\"unavailable\":%zu,"
        "\"failed\":%zu,\"issues\":%zu}",
        s->requested_inputs, s->discovered_files,
        s->ignored_unrelated_files, s->unity_container_files,
        s->standalone_serialized_files, s->serialized_sources,
        s->visited_serialized_sources,
        s->shader_objects, s->compute_shader_objects,
        s->named_compute_shaders,
        catalog->materials_included ? "true" : "false",
        s->material_objects, s->decoded_materials,
        s->resolved_material_shader_links,
        s->unresolved_material_shader_links,
        s->null_material_shader_links, s->failed_materials,
        s->decoded_shaders, s->ready_shaders,
        s->unavailable_shaders, s->failed_shaders, catalog->issue_count);
}

static void sb_hex_bytes_raw(StringBuilder* output, const uint8_t* bytes,
                             size_t size) {
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < size; ++index) {
        sb_append_char(output, hex[bytes[index] >> 4U]);
        sb_append_char(output, hex[bytes[index] & 0x0fU]);
    }
}

static void sb_hex_bytes(StringBuilder* output, const uint8_t* bytes,
                         size_t size) {
    sb_append_char(output, '"');
    sb_hex_bytes_raw(output, bytes, size);
    sb_append_char(output, '"');
}

static void append_catalog_source_schema_key_json(
    StringBuilder* output, const ShaderCatalogSource* source,
    const ShaderCatalogSchemaKeyRecord* key) {
    sb_appendf(output,
               "{\"serialized_file_version\":%u,\"unity_version\":",
               key->serialized_file_version);
    sb_json_string(output, source->unity_version);
    sb_appendf(output,
               ",\"target_platform\":%u,\"class_id\":%" PRId32
               ",\"serialized_type_id\":%" PRId32
               ",\"script_type_index\":%u,"
               "\"has_script_id_hash\":%s,\"script_id_hash\":",
               source->target_platform, key->class_id,
               key->serialized_type_id, (unsigned)key->script_type_index,
               key->has_script_id_hash ? "true" : "false");
    if (key->has_script_id_hash) {
        sb_hex_bytes(output, key->script_id_hash,
                     sizeof(key->script_id_hash));
    } else {
        sb_append(output, "null");
    }
    sb_append(output, ",\"type_hash\":");
    sb_hex_bytes(output, key->type_hash, sizeof(key->type_hash));
    sb_appendf(output,
               ",\"stripped\":%s,\"reference_type\":%s,"
               "\"lookup_required\":%s,\"lookup_status\":",
               key->is_stripped ? "true" : "false",
               key->is_ref_type ? "true" : "false",
               key->lookup_required ? "true" : "false");
    if (key->lookup_required) {
        sb_json_string(output,
                       typetree_schema_status_name(key->lookup_status));
    } else {
        sb_append(output, "null");
    }
    sb_append(output, ",\"resolution_status\":");
    sb_json_string(output,
                   typetree_schema_status_name(key->lookup_status));
    sb_append(output, ",\"provenance\":");
    if (key->provenance == SHADER_CATALOG_SCHEMA_PROVENANCE_NONE) {
        sb_append(output, "null");
    } else {
        sb_json_string(output, shader_catalog_schema_provenance_name(
                                   key->provenance));
    }
    sb_append(output, ",\"profile\":");
    if (key->provenance == SHADER_CATALOG_SCHEMA_PROVENANCE_NONE) {
        sb_append(output, "null");
    } else {
        sb_json_string(output,
                       shader_catalog_schema_profile_name(key->profile));
    }
    sb_append_char(output, '}');
}

static void append_catalog_sources_json(StringBuilder* output,
                                        const ShaderCatalog* catalog) {
    sb_append(output, "\"serialized_sources\":[");
    for (size_t index = 0U; index < catalog->source_count; ++index) {
        const ShaderCatalogSource* source = &catalog->sources[index];
        if (index != 0U) sb_append_char(output, ',');
        sb_appendf(output, "{\"index\":%zu,\"id\":", index + 1U);
        sb_json_string(output, source->occurrence_id);
        sb_append(output, ",\"source\":");
        sb_json_string(output, source->outer_path);
        sb_append(output, ",\"member\":");
        sb_json_string(output, source->member_name);
        sb_appendf(output,
                   ",\"member_index\":%zu,\"bundle_member\":%s,"
                   "\"serialized_sha256\":",
                   source->member_index,
                   source->is_bundle_member ? "true" : "false");
        sb_json_string(output, source->serialized_digest_hex);
        sb_append(output, ",\"metadata_status\":");
        sb_json_string(output, shader_catalog_source_metadata_status_name(
                                   source->metadata_status));
        sb_append(output, ",\"serialized_file_version\":");
        if (source->metadata_status != SHADER_CATALOG_SOURCE_METADATA_OK) {
            sb_append(output, "null,\"unity_version\":null,"
                              "\"target_platform\":null,"
                              "\"type_tree_enabled\":null,"
                              "\"object_count\":null,"
                              "\"class_id_21_count\":null,"
                              "\"class_id_48_count\":null,"
                              "\"class_id_72_count\":null,"
                              "\"class_id_21_schema_required\":null,"
                              "\"class_id_21_schema_status\":null,"
                              "\"class_id_21_schema_keys\":[],"
                              "\"class_id_48_schema_required\":null,"
                              "\"class_id_48_schema_status\":null,"
                              "\"class_id_48_schema_keys\":[]}");
            continue;
        }
        sb_appendf(output, "%u,\"unity_version\":",
                   source->serialized_file_version);
        sb_json_string(output, source->unity_version);
        sb_appendf(output,
                   ",\"target_platform\":%u,"
                   "\"type_tree_enabled\":%s,\"object_count\":%zu,"
                   "\"class_id_21_count\":%zu,"
                   "\"class_id_48_count\":%zu,"
                   "\"class_id_72_count\":%zu,"
                   "\"class_id_21_schema_required\":%s,"
                   "\"class_id_21_schema_status\":",
                   source->target_platform,
                   source->type_tree_enabled ? "true" : "false",
                   source->object_count, source->class_id_21_count,
                   source->class_id_48_count,
                   source->class_id_72_count,
                   source->class_id_21_schema_required ? "true" : "false");
        if (source->class_id_21_schema_required) {
            sb_json_string(output, typetree_schema_status_name(
                                       source->class_id_21_schema_status));
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ",\"class_id_21_schema_keys\":[");
        for (size_t key_index = 0U;
             key_index < source->class_id_21_schema_key_count;
             ++key_index) {
            if (key_index != 0U) sb_append_char(output, ',');
            append_catalog_source_schema_key_json(
                output, source,
                &source->class_id_21_schema_keys[key_index]);
        }
        sb_append(output, "],");
        sb_appendf(output,
                   "\"class_id_48_schema_required\":%s,"
                   "\"class_id_48_schema_status\":",
                   source->class_id_48_schema_required ? "true" : "false");
        if (source->class_id_48_schema_required) {
            sb_json_string(output, typetree_schema_status_name(
                                       source->class_id_48_schema_status));
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ",\"class_id_48_schema_keys\":[");
        for (size_t key_index = 0U;
             key_index < source->class_id_48_schema_key_count;
             ++key_index) {
            if (key_index != 0U) sb_append_char(output, ',');
            append_catalog_source_schema_key_json(
                output, source,
                &source->class_id_48_schema_keys[key_index]);
        }
        sb_append(output, "]}");
    }
    sb_append_char(output, ']');
}

static void append_catalog_sources_table(StringBuilder* output,
                                         const ShaderCatalog* catalog) {
    sb_append(output,
        "\nSerialized sources:\n"
        "SRC#\tID\tMETADATA\tSHA256\tFORMAT\tUNITY\tTARGET\tTYPETREE\t"
        "OBJECTS\tCLASS21\tCLASS48\tCLASS72\tCLASS21_SCHEMA\t"
        "CLASS48_SCHEMA\tSOURCE\n");
    for (size_t index = 0U; index < catalog->source_count; ++index) {
        const ShaderCatalogSource* source = &catalog->sources[index];
        sb_appendf(output, "%zu\tf:%.16s\t%s\t%s\t", index + 1U,
                   source->occurrence_digest_hex,
                   shader_catalog_source_metadata_status_name(
                       source->metadata_status),
                   source->serialized_digest_hex);
        if (source->metadata_status != SHADER_CATALOG_SOURCE_METADATA_OK) {
            sb_append(output,
                      "<unavailable>\t<unavailable>\t<unavailable>\t"
                      "<unavailable>\t<unavailable>\t<unavailable>\t"
                      "<unavailable>\t<unavailable>\t<unavailable>\t"
                      "<unavailable>\t");
        } else {
            sb_appendf(output, "%u\t", source->serialized_file_version);
            sb_table_text(output, source->unity_version);
            sb_appendf(output, "\t%u\t%s\t%zu\t%zu\t%zu\t%zu\t",
                       source->target_platform,
                       source->type_tree_enabled ? "enabled" : "disabled",
                       source->object_count, source->class_id_21_count,
                       source->class_id_48_count,
                       source->class_id_72_count);
            if (source->class_id_21_schema_required) {
                sb_append(output, typetree_schema_status_name(
                                      source->class_id_21_schema_status));
            } else {
                sb_append(output, "not-required");
            }
            sb_append_char(output, '\t');
            if (source->class_id_48_schema_required) {
                sb_append(output, typetree_schema_status_name(
                                      source->class_id_48_schema_status));
            } else {
                sb_append(output, "not-required");
            }
            sb_append_char(output, '\t');
        }
        sb_table_text(output, source->outer_path);
        if (source->member_name) {
            sb_appendf(output, "::[%zu]", source->member_index);
            sb_table_text(output, source->member_name);
        }
        sb_append_char(output, '\n');

        for (size_t key_index = 0U;
             key_index < source->class_id_21_schema_key_count;
             ++key_index) {
            const ShaderCatalogSchemaKeyRecord* key =
                &source->class_id_21_schema_keys[key_index];
            sb_appendf(output,
                       "  Schema21[%zu]: format=%u unity=",
                       key_index, key->serialized_file_version);
            sb_table_text(output, source->unity_version);
            sb_appendf(output,
                       " target=%u class=%" PRId32
                       " serialized_type=%" PRId32
                       " script_type_index=%u script_hash=",
                       source->target_platform, key->class_id,
                       key->serialized_type_id,
                       (unsigned)key->script_type_index);
            if (key->has_script_id_hash) {
                sb_hex_bytes_raw(output, key->script_id_hash,
                                 sizeof(key->script_id_hash));
            } else {
                sb_append(output, "<absent>");
            }
            sb_append(output, " type_hash=");
            sb_hex_bytes_raw(output, key->type_hash,
                             sizeof(key->type_hash));
            sb_appendf(output,
                       " stripped=%s ref=%s lookup=%s provenance=%s "
                       "profile=%s\n",
                       key->is_stripped ? "yes" : "no",
                       key->is_ref_type ? "yes" : "no",
                       key->lookup_required
                           ? typetree_schema_status_name(key->lookup_status)
                           : "not-required",
                       key->provenance ==
                               SHADER_CATALOG_SCHEMA_PROVENANCE_NONE
                           ? "<unresolved>"
                           : shader_catalog_schema_provenance_name(
                                 key->provenance),
                       key->provenance ==
                               SHADER_CATALOG_SCHEMA_PROVENANCE_NONE
                           ? "<unresolved>"
                           : shader_catalog_schema_profile_name(
                                 key->profile));
        }

        for (size_t key_index = 0U;
             key_index < source->class_id_48_schema_key_count;
             ++key_index) {
            const ShaderCatalogSchemaKeyRecord* key =
                &source->class_id_48_schema_keys[key_index];
            sb_appendf(output,
                       "  Schema48[%zu]: format=%u unity=",
                       key_index, key->serialized_file_version);
            sb_table_text(output, source->unity_version);
            sb_appendf(output,
                       " target=%u class=%" PRId32
                       " serialized_type=%" PRId32
                       " script_type_index=%u script_hash=",
                       source->target_platform, key->class_id,
                       key->serialized_type_id,
                       (unsigned)key->script_type_index);
            if (key->has_script_id_hash) {
                sb_hex_bytes_raw(output, key->script_id_hash,
                                 sizeof(key->script_id_hash));
            } else {
                sb_append(output, "<absent>");
            }
            sb_append(output, " type_hash=");
            sb_hex_bytes_raw(output, key->type_hash,
                             sizeof(key->type_hash));
            sb_appendf(output,
                       " stripped=%s ref=%s lookup=%s provenance=%s "
                       "profile=%s\n",
                       key->is_stripped ? "yes" : "no",
                       key->is_ref_type ? "yes" : "no",
                       key->lookup_required
                           ? typetree_schema_status_name(key->lookup_status)
                           : "not-required",
                       key->provenance ==
                               SHADER_CATALOG_SCHEMA_PROVENANCE_NONE
                           ? "<unresolved>"
                           : shader_catalog_schema_provenance_name(
                                 key->provenance),
                       key->provenance ==
                               SHADER_CATALOG_SCHEMA_PROVENANCE_NONE
                           ? "<unresolved>"
                           : shader_catalog_schema_profile_name(
                                 key->profile));
        }
    }
}

static void append_catalog_issues_json(StringBuilder* output,
                                       const ShaderCatalog* catalog) {
    sb_append(output, ",\"issues\":[");
    for (size_t i = 0U; i < catalog->issue_count; ++i) {
        const ShaderCatalogIssue* issue = &catalog->issues[i];
        if (i != 0U) sb_append_char(output, ',');
        sb_append_char(output, '{');
        sb_append(output, "\"code\":");
        sb_json_string(output, shader_catalog_issue_code_name(issue->code));
        sb_append(output, ",\"input_status\":");
        if (issue->discovery_status != COMMON_PATH_DISCOVERY_OK) {
            sb_append(output, "null");
        } else {
            sb_json_string(output,
                           unity_input_status_name(issue->input_status));
        }
        sb_append(output, ",\"discovery_status\":");
        if (issue->discovery_status == COMMON_PATH_DISCOVERY_OK) {
            sb_append(output, "null");
        } else {
            sb_json_string(output,
                           common_path_discovery_status_name(
                               issue->discovery_status));
        }
        sb_append(output, ",\"source\":");
        sb_json_string(output, issue->outer_path);
        sb_append(output, ",\"member\":");
        sb_json_string(output, issue->member_name);
        sb_append_char(output, '}');
    }
    sb_append_char(output, ']');
}

static void append_catalog_failures_json(StringBuilder* output,
                                         const ShaderCatalog* catalog) {
    sb_append(output, ",\"catalog_failures\":[");
    bool first = true;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        const ShaderCatalogRecord* record = &catalog->records[i];
        if (record->status == SHADER_CATALOG_RECORD_READY) continue;
        if (!first) sb_append_char(output, ',');
        first = false;
        sb_appendf(output, "{\"index\":%zu,\"id\":", i + 1U);
        sb_json_string(output, record->occurrence_id);
        sb_append(output, ",\"content_id\":");
        sb_json_string(output, record->content_id);
        sb_append(output, ",\"name\":");
        sb_json_string(output, record->name);
        sb_append(output, ",\"kind\":");
        sb_json_string(output, catalog_record_kind_name(record));
        sb_appendf(output, ",\"class_id\":%" PRId32,
                   record->class_id);
        sb_append(output, ",\"status\":");
        sb_json_string(output,
                       shader_catalog_record_status_name(record->status));
        sb_append(output, ",\"schema_status\":");
        sb_json_string(output,
                       typetree_schema_status_name(record->schema_status));
        sb_append(output, ",\"object_status\":");
        sb_json_string(output,
                       shader_object_status_name(record->object_status));
        sb_append(output, ",\"compute_inventory_status\":");
        sb_json_string(output, compute_shader_inventory_status_name(
                                   record->compute_inventory_status));
        sb_append(output, ",\"compute_object_status\":");
        sb_json_string(output, compute_shader_object_status_name(
                                   record->compute_object_status));
        sb_append(output, ",\"compute_layout_authority\":");
        sb_json_string(
            output,
            record->compute_object_status == COMPUTE_SHADER_OBJECT_OK
                ? COMPUTE_SHADER_OBJECT_LAYOUT_AUTHORITY : NULL);
        sb_append(output, ",\"compute_source_authority\":");
        sb_json_string(output, compute_shader_source_authority_status_name(
                                   record->compute_source_authority_status));
        sb_append(output, ",\"source\":");
        sb_json_string(output, record->outer_path);
        sb_append(output, ",\"member\":");
        sb_json_string(output, record->member_name);
        sb_appendf(output,
                   ",\"member_index\":%zu,\"path_id\":\"%" PRId64
                   "\"}", record->member_index, record->path_id);
    }
    sb_append_char(output, ']');
}

static void append_catalog_material_failures_json(
    StringBuilder* output, const ShaderCatalog* catalog) {
    sb_append(output, ",\"catalog_material_failures\":[");
    bool first = true;
    for (size_t index = 0U; index < catalog->material_count; ++index) {
        const ShaderCatalogMaterialRecord* record =
            &catalog->materials[index];
        if (record->status == SHADER_CATALOG_MATERIAL_READY ||
            record->status == SHADER_CATALOG_MATERIAL_SHADER_NULL) {
            continue;
        }
        if (!first) sb_append_char(output, ',');
        first = false;
        sb_appendf(output, "{\"index\":%zu,\"id\":", index + 1U);
        sb_json_string(output, record->occurrence_id);
        sb_append(output, ",\"content_id\":");
        sb_json_string(output, record->content_id);
        sb_append(output, ",\"name\":");
        sb_json_string(output, record->name);
        sb_append(output, ",\"status\":");
        sb_json_string(output,
                       shader_catalog_material_status_name(record->status));
        sb_append(output, ",\"schema_status\":");
        sb_json_string(output,
                       typetree_schema_status_name(record->schema_status));
        sb_append(output, ",\"object_status\":");
        sb_json_string(output,
                       material_object_status_name(record->object_status));
        sb_append(output, ",\"shader_link_status\":");
        sb_json_string(output,
                       unity_pptr_resolve_status_name(
                           record->shader_link_status));
        sb_append(output, ",\"source\":");
        sb_json_string(output, record->outer_path);
        sb_append(output, ",\"member\":");
        sb_json_string(output, record->member_name);
        sb_appendf(output,
                   ",\"member_index\":%zu,\"path_id\":\"%" PRId64
                   "\"}", record->member_index, record->path_id);
    }
    sb_append_char(output, ']');
}

static void append_registry_json(StringBuilder* output, const char* path,
                                 const char* digest_hex) {
    sb_append(output, "\"schema_registry\":{\"path\":");
    sb_json_string(output, path);
    sb_append(output, ",\"sha256\":");
    sb_json_string(output, digest_hex);
    sb_append_char(output, '}');
}

static bool render_list_json(const ShaderCatalog* catalog,
                             const bool* selected,
                             CliShaderKind selection_kind,
                             const char* registry_path,
                             const char* registry_digest,
                             StringBuilder* output) {
    sb_append(output,
              "{\"report_schema\":\"dxbc-sandbox-report\","
              "\"report_version\":4,\"command\":\"list\","
              "\"selection_kind\":");
    sb_json_string(output, cli_shader_kind_name(selection_kind));
    sb_append(output, ",\"complete\":");
    sb_append(output, catalog_inventory_is_complete(catalog)
                          ? "true," : "false,");
    sb_append(output, "\"inventory_complete\":");
    sb_append(output, catalog_inventory_is_complete(catalog)
                          ? "true," : "false,");
    sb_append(output, "\"conversion_catalog_complete\":");
    sb_append(output, conversion_catalog_is_complete(catalog)
                          ? "true," : "false,");
    sb_append(output, "\"artifact_catalog_complete\":");
    sb_append(output, shader_catalog_is_complete(catalog)
                          ? "true," : "false,");
    sb_append(output, "\"selection_artifact_ready\":");
    sb_append(output, selected_artifact_is_ready(catalog, selected)
                          ? "true," : "false,");
    sb_append(output, "\"selection_conversion_ready\":");
    sb_append(output, selected_conversion_is_ready(catalog, selected)
                          ? "true," : "false,");
    sb_appendf(output, "\"listed\":%zu,",
               selected_record_count(catalog, selected));
    append_registry_json(output, registry_path, registry_digest);
    sb_append(output, ",\"summary\":");
    append_catalog_stats_json(output, catalog);
    sb_append_char(output, ',');
    append_catalog_sources_json(output, catalog);
    sb_append(output, ",\"shaders\":[");
    bool first = true;
    size_t view_index = 0U;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (record_matches_kind(&catalog->records[i], selection_kind)) {
            ++view_index;
        }
        if (!selected[i]) continue;
        const ShaderCatalogRecord* record = &catalog->records[i];
        if (!first) sb_append_char(output, ',');
        first = false;
        sb_appendf(output,
                   "{\"index\":%zu,\"catalog_index\":%zu,\"id\":",
                   view_index, i + 1U);
        sb_json_string(output, record->occurrence_id);
        sb_append(output, ",\"content_id\":");
        sb_json_string(output, record->content_id);
        sb_append(output, ",\"name\":");
        sb_json_string(output, record->name);
        sb_append(output, ",\"kind\":");
        sb_json_string(output, catalog_record_kind_name(record));
        sb_appendf(output, ",\"path_id\":\"%" PRId64 "\""
                   ",\"class_id\":%" PRId32
                   ",\"object_bytes\":%u,\"d3d11_entries\":%zu,",
                   record->path_id, record->class_id, record->object_size,
                   record->d3d11_blob_entries);
        sb_append(output, "\"status\":");
        sb_json_string(output,
                       shader_catalog_record_status_name(record->status));
        sb_append(output, ",\"schema_status\":");
        sb_json_string(output,
                       typetree_schema_status_name(record->schema_status));
        sb_append(output, ",\"object_status\":");
        sb_json_string(output,
                       shader_object_status_name(record->object_status));
        sb_append(output, ",\"compute_inventory_status\":");
        sb_json_string(output, compute_shader_inventory_status_name(
                                   record->compute_inventory_status));
        sb_append(output, ",\"compute_object_status\":");
        sb_json_string(output, compute_shader_object_status_name(
                                   record->compute_object_status));
        sb_append(output, ",\"compute_layout_authority\":");
        sb_json_string(
            output,
            record->compute_object_status == COMPUTE_SHADER_OBJECT_OK
                ? COMPUTE_SHADER_OBJECT_LAYOUT_AUTHORITY : NULL);
        sb_append(output, ",\"compute_source_authority\":");
        sb_json_string(output, compute_shader_source_authority_status_name(
                                   record->compute_source_authority_status));
        sb_append(output, ",\"compute_platform_variants_declared\":");
        if (record->class_id == 72 &&
            record->compute_inventory_status ==
                COMPUTE_SHADER_INVENTORY_OK) {
            sb_appendf(output, "%u",
                       record->compute_platform_variants_declared);
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ",\"compute_summary\":");
        if (record->class_id == 72 &&
            record->compute_object_status == COMPUTE_SHADER_OBJECT_OK) {
            const ComputeShaderObjectSummary* summary =
                &record->compute_summary;
            sb_appendf(output,
                "{\"platforms\":%zu,\"kernel_parents\":%zu,"
                "\"kernel_variants\":%zu,\"code_blobs\":%zu,"
                "\"dxbc_code_blobs\":%zu,\"empty_code_blobs\":%zu,"
                "\"non_dxbc_code_blobs\":%zu,"
                "\"exact_thread_groups\":%zu,"
                "\"invalid_thread_groups\":%zu,\"resources\":%zu,"
                "\"constant_buffers\":%zu,\"parameters\":%zu}",
                summary->platform_count, summary->kernel_parent_count,
                summary->kernel_variant_count, summary->code_blob_count,
                summary->dxbc_code_blob_count,
                summary->empty_code_blob_count,
                summary->non_dxbc_code_blob_count,
                summary->exact_thread_group_count,
                summary->invalid_thread_group_count,
                summary->resource_count, summary->constant_buffer_count,
                summary->parameter_count);
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ",\"unity_version\":");
        sb_json_string(output, record->unity_version);
        sb_appendf(output, ",\"target_platform\":%u,\"source\":",
                   record->target_platform);
        sb_json_string(output, record->outer_path);
        sb_append(output, ",\"member\":");
        sb_json_string(output, record->member_name);
        sb_appendf(output, ",\"member_index\":%zu,"
                   "\"bundle_member\":%s",
                   record->member_index,
                   record->is_bundle_member ? "true" : "false");
        sb_append_char(output, '}');
    }
    sb_append_char(output, ']');
    append_catalog_issues_json(output, catalog);
    append_catalog_failures_json(output, catalog);
    append_catalog_material_failures_json(output, catalog);
    sb_append(output, "}\n");
    return sb_ok(output);
}

static bool render_list_table(const ShaderCatalog* catalog,
                              const bool* selected,
                              CliShaderKind selection_kind,
                              bool show_sources,
                              const char* registry_path,
                              const char* registry_digest,
                              StringBuilder* output) {
    sb_append(output,
              "#\tID\tCONTENT_ID\tKIND\tSTATUS\tD3D11\tSHADER\tSOURCE\tPATH_ID\n");
    size_t view_index = 0U;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (record_matches_kind(&catalog->records[i], selection_kind)) {
            ++view_index;
        }
        if (!selected[i]) continue;
        const ShaderCatalogRecord* record = &catalog->records[i];
        sb_appendf(output,
                   "%zu\to:%.16s:%" PRId64 "\ts:%.16s:%" PRId64
                   "\t%s\t%s\t%zu\t",
                   view_index, record->occurrence_digest_hex, record->path_id,
                   record->serialized_digest_hex, record->path_id,
                   catalog_record_kind_name(record),
                   shader_catalog_record_status_name(record->status),
                   record->d3d11_blob_entries);
        sb_table_text(output, record->name);
        sb_append_char(output, '\t');
        sb_table_text(output, record->outer_path);
        if (record->member_name) {
            sb_appendf(output, "::[%zu]", record->member_index);
            sb_table_text(output, record->member_name);
        }
        sb_appendf(output, "\t%" PRId64 "\n", record->path_id);
    }
    if (show_sources) append_catalog_sources_table(output, catalog);
    const ShaderCatalogStats* s = &catalog->stats;
    sb_appendf(output,
        "\nSummary: kind=%s listed=%zu shaders=%zu ready=%zu unavailable=%zu "
        "failed=%zu serialized=%zu visited_serialized=%zu unityfs=%zu "
        "ignored=%zu issues=%zu "
        "inventory_complete=%s selection_artifact_ready=%s "
        "selection_conversion_ready=%s artifact_catalog_complete=%s "
        "conversion_catalog_complete=%s\n",
        cli_shader_kind_name(selection_kind),
        selected_record_count(catalog, selected), s->shader_objects,
        s->ready_shaders, s->unavailable_shaders,
        s->failed_shaders, s->serialized_sources,
        s->visited_serialized_sources,
        s->unity_container_files, s->ignored_unrelated_files,
        catalog->issue_count,
        catalog_inventory_is_complete(catalog) ? "yes" : "no",
        selected_artifact_is_ready(catalog, selected) ? "yes" : "no",
        selected_conversion_is_ready(catalog, selected) ? "yes" : "no",
        shader_catalog_is_complete(catalog) ? "yes" : "no",
        conversion_catalog_is_complete(catalog) ? "yes" : "no");
    sb_appendf(output, "Registry: %s  %s\n",
               registry_digest, registry_path);
    for (size_t i = 0U; i < catalog->issue_count; ++i) {
        const ShaderCatalogIssue* issue = &catalog->issues[i];
        sb_appendf(output, "Issue: %s (%s): ",
                   shader_catalog_issue_code_name(issue->code),
                   issue->discovery_status != COMMON_PATH_DISCOVERY_OK
                       ? common_path_discovery_status_name(
                             issue->discovery_status)
                       : unity_input_status_name(issue->input_status));
        sb_table_text(output, issue->outer_path);
        if (issue->member_name) {
            sb_append(output, "::");
            sb_table_text(output, issue->member_name);
        }
        sb_append_char(output, '\n');
    }
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        const ShaderCatalogRecord* record = &catalog->records[i];
        if (selected[i] || record->status == SHADER_CATALOG_RECORD_READY) {
            continue;
        }
        sb_appendf(output, "Catalog failure: #%zu %s (%s): ", i + 1U,
                   shader_catalog_record_status_name(record->status),
                   record->class_id == 72
                       ? compute_shader_inventory_status_name(
                             record->compute_inventory_status)
                       : shader_object_status_name(record->object_status));
        sb_table_text(output, record->name);
        sb_append(output, " @ ");
        sb_table_text(output, record->outer_path);
        sb_append_char(output, '\n');
    }
    return sb_ok(output);
}

static void append_optional_index_json(StringBuilder* output,
                                       const char* name, int value) {
    sb_append_char(output, ',');
    sb_json_string(output, name);
    sb_append_char(output, ':');
    if (value < 0) {
        sb_append(output, "null");
    } else {
        sb_appendf(output, "%d", value);
    }
}

static void append_hlsl_metadata_location_json(
    StringBuilder* output, const HLSLEmitMetadataLocation* location) {
    if (!location || location->kind == HLSL_EMIT_METADATA_NONE) {
        sb_append(output, "null");
        return;
    }
    sb_append(output, "{\"source\":");
    sb_json_string(output,
                   hlsl_emit_metadata_source_name(location->source));
    sb_append(output, ",\"kind\":");
    sb_json_string(output, hlsl_emit_metadata_kind_name(location->kind));
    append_optional_index_json(output, "record_index",
                               location->record_index);
    append_optional_index_json(output, "member_index",
                               location->member_index);
    append_optional_index_json(output, "register_index",
                               location->register_index);
    sb_append_char(output, '}');
}

static void append_hlsl_diagnostic_json(
    StringBuilder* output, const ShaderLabStageDiagnostic* stage) {
    if (!stage || stage->status != SHADERLAB_STAGE_HLSL_EMISSION_FAILED) {
        sb_append(output, "null");
        return;
    }
    const HLSLEmitDiagnostic* diagnostic = &stage->hlsl;
    sb_append(output, "{\"status\":");
    sb_json_string(output, hlsl_emit_status_name(diagnostic->status));
    sb_append(output, ",\"phase\":");
    sb_json_string(output, hlsl_emit_phase_name(diagnostic->phase));
    sb_append(output, ",\"reason\":");
    sb_json_string(output, hlsl_emit_reason_name(diagnostic->reason));
    append_optional_index_json(output, "instruction_index",
                               diagnostic->instruction_index);
    sb_append(output, ",\"source_instruction_index\":");
    if (diagnostic->source_instruction_index == UINT32_MAX) {
        sb_append(output, "null");
    } else {
        sb_appendf(output, "%" PRIu32,
                   diagnostic->source_instruction_index);
    }
    sb_append(output, ",\"opcode\":");
    if (diagnostic->opcode < 0) {
        sb_append(output, "null");
    } else {
        sb_json_string(output, hlsl_emit_opcode_name(diagnostic->opcode));
    }
    append_optional_index_json(output, "opcode_value", diagnostic->opcode);
    append_optional_index_json(output, "operand_index",
                               diagnostic->operand_index);
    sb_append(output, ",\"metadata\":");
    append_hlsl_metadata_location_json(output, &diagnostic->metadata);
    sb_append(output, ",\"related_metadata\":");
    append_hlsl_metadata_location_json(output,
                                       &diagnostic->related_metadata);
    sb_append_char(output, '}');
}

static void append_candidate_diagnostic_json(
    StringBuilder* output, const ShaderBatchRecordResult* result) {
    sb_append(output, ",\"emission_diagnostic\":");
    if (!result ||
        result->failure != SHADER_BATCH_FAILURE_CANDIDATE_EMISSION) {
        sb_append(output, "null");
        return;
    }
    const ShaderLabCandidateDiagnostic* diagnostic =
        &result->candidate_diagnostic;
    sb_append(output, "{\"status\":");
    sb_json_string(output,
                   shaderlab_candidate_status_name(diagnostic->status));
    sb_append(output, ",\"reason\":");
    sb_json_string(output, shaderlab_candidate_reason_name(diagnostic));
    append_optional_index_json(output, "property_index",
                               diagnostic->property_index);
    append_optional_index_json(output, "subshader_index",
                               diagnostic->subshader_index);
    append_optional_index_json(output, "pass_index", diagnostic->pass_index);

    int stage_index = -1;
    int subprogram_index = -1;
    int conflicting_subprogram_index = -1;
    if (diagnostic->status == SHADERLAB_CANDIDATE_STAGE_FAILED) {
        stage_index = diagnostic->stage.stage_index;
        subprogram_index = diagnostic->stage.subprogram_index;
        conflicting_subprogram_index =
            diagnostic->stage.conflicting_subprogram_index;
    } else if (diagnostic->status ==
               SHADERLAB_CANDIDATE_PASS_TARGET_FAILED) {
        stage_index = diagnostic->target.stage_index;
        subprogram_index = diagnostic->target.subprogram_index;
    } else if (diagnostic->status ==
               SHADERLAB_CANDIDATE_UNSUPPORTED_STAGE) {
        stage_index = diagnostic->unsupported_stage_index;
    }
    append_optional_index_json(output, "stage_index", stage_index);
    append_optional_index_json(output, "subprogram_index",
                               subprogram_index);
    append_optional_index_json(output, "conflicting_subprogram_index",
                               conflicting_subprogram_index);

    if (diagnostic->status == SHADERLAB_CANDIDATE_STAGE_FAILED) {
        sb_append(output, ",\"stage_contract_status\":");
        sb_json_string(output, dxbc_stage_contract_status_name(
                                   diagnostic->stage.stage_contract_status));
        sb_append(output, ",\"stage_tuple_status\":");
        sb_json_string(output, shader_stage_tuple_status_name(
                                   diagnostic->stage.stage_tuple_status));
        sb_append(output, ",\"hlsl\":");
        append_hlsl_diagnostic_json(output, &diagnostic->stage);
    } else if (diagnostic->status ==
               SHADERLAB_CANDIDATE_PASS_TARGET_FAILED) {
        sb_append(output, ",\"target_status\":");
        sb_json_string(output,
                       shaderlab_target_status_name(diagnostic->target.status));
        sb_appendf(output,
                   ",\"shader_requirements\":\"0x%016" PRIx64 "\","
                   "\"shader_model_major\":%u,"
                   "\"shader_model_minor\":%u",
                   diagnostic->target.shader_requirements,
                   diagnostic->target.shader_model_major,
                   diagnostic->target.shader_model_minor);
        sb_append(output, ",\"document_status\":");
        sb_json_string(output, dxbc_document_diagnostic_code_name(
                                   diagnostic->target.document_status));
        sb_append(output, ",\"stage_contract_status\":");
        sb_json_string(output, dxbc_stage_contract_status_name(
                                   diagnostic->target.stage_contract_status));
        sb_append(output, ",\"stage_tuple_status\":");
        sb_json_string(output, shader_stage_tuple_status_name(
                                   diagnostic->target.stage_tuple_status));
    }
    sb_append_char(output, '}');
}

static void append_candidate_diagnostic_table(
    StringBuilder* output, const ShaderBatchRecordResult* result) {
    if (!result ||
        result->failure != SHADER_BATCH_FAILURE_CANDIDATE_EMISSION) {
        return;
    }
    const ShaderLabCandidateDiagnostic* diagnostic =
        &result->candidate_diagnostic;
    sb_appendf(output, ": %s/%s",
               shaderlab_candidate_status_name(diagnostic->status),
               shaderlab_candidate_reason_name(diagnostic));
    if (diagnostic->property_index >= 0)
        sb_appendf(output, " property=%d", diagnostic->property_index);
    if (diagnostic->subshader_index >= 0)
        sb_appendf(output, " subshader=%d", diagnostic->subshader_index);
    if (diagnostic->pass_index >= 0)
        sb_appendf(output, " pass=%d", diagnostic->pass_index);
    if (diagnostic->status == SHADERLAB_CANDIDATE_STAGE_FAILED) {
        if (diagnostic->stage.stage_index >= 0)
            sb_appendf(output, " stage=%d", diagnostic->stage.stage_index);
        if (diagnostic->stage.subprogram_index >= 0)
            sb_appendf(output, " subprogram=%d",
                       diagnostic->stage.subprogram_index);
        if (diagnostic->stage.conflicting_subprogram_index >= 0)
            sb_appendf(output, " conflict=%d",
                       diagnostic->stage.conflicting_subprogram_index);
        if (diagnostic->stage.status ==
            SHADERLAB_STAGE_HLSL_EMISSION_FAILED) {
            const HLSLEmitDiagnostic* hlsl = &diagnostic->stage.hlsl;
            sb_appendf(output, " hlsl=%s/%s/%s",
                       hlsl_emit_status_name(hlsl->status),
                       hlsl_emit_phase_name(hlsl->phase),
                       hlsl_emit_reason_name(hlsl->reason));
            if (hlsl->instruction_index >= 0) {
                sb_appendf(output, " instruction=%d opcode=%s(%d)",
                           hlsl->instruction_index,
                           hlsl_emit_opcode_name(hlsl->opcode), hlsl->opcode);
            }
            if (hlsl->source_instruction_index != UINT32_MAX)
                sb_appendf(output, " source-instruction=%" PRIu32,
                           hlsl->source_instruction_index);
            if (hlsl->operand_index >= 0)
                sb_appendf(output, " operand=%d", hlsl->operand_index);
            if (hlsl->metadata.kind != HLSL_EMIT_METADATA_NONE) {
                sb_appendf(output, " metadata=%s/%s[%d] register=%d",
                           hlsl_emit_metadata_source_name(
                               hlsl->metadata.source),
                           hlsl_emit_metadata_kind_name(
                               hlsl->metadata.kind),
                           hlsl->metadata.record_index,
                           hlsl->metadata.register_index);
            }
        }
    } else if (diagnostic->status ==
               SHADERLAB_CANDIDATE_PASS_TARGET_FAILED) {
        sb_appendf(output, " requirements=0x%016" PRIx64 " sm=%u.%u",
                   diagnostic->target.shader_requirements,
                   diagnostic->target.shader_model_major,
                   diagnostic->target.shader_model_minor);
        if (diagnostic->target.stage_index >= 0)
            sb_appendf(output, " stage=%d", diagnostic->target.stage_index);
        if (diagnostic->target.subprogram_index >= 0)
            sb_appendf(output, " subprogram=%d",
                       diagnostic->target.subprogram_index);
    } else if (diagnostic->status ==
               SHADERLAB_CANDIDATE_UNSUPPORTED_STAGE &&
               diagnostic->unsupported_stage_index >= 0) {
        sb_appendf(output, " stage=%d",
                   diagnostic->unsupported_stage_index);
    }
}

static bool record_has_structural_certificate(
    const ShaderBatchRecordResult* result) {
    return result &&
        result->compute_object_status ==
            COMPUTE_SHADER_OBJECT_NOT_APPLICABLE &&
        result->publication_authorized &&
        (result->status == SHADER_BATCH_EMITTED ||
         result->status == SHADER_BATCH_UNCHANGED) &&
        result->structural_diagnostic.status == SHADERLAB_STRUCTURE_OK;
}

static void append_structural_diagnostic_json(
    StringBuilder* output, const ShaderBatchRecordResult* result) {
    sb_append(output, ",\"structural_coverage\":{");
    if (record_has_structural_certificate(result)) {
        sb_append(output, "\"status\":\"ok\"");
    } else if (result &&
               result->failure ==
                   SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE) {
        sb_append(output, "\"status\":");
        sb_json_string(output, shaderlab_structural_status_name(
                                   result->structural_diagnostic.status));
    } else {
        sb_append(output, "\"status\":\"not-run\"");
    }
    sb_append(output,
              ",\"scope\":\"serialized-d3d11-shaderlab-structure\","
              "\"runtime_selection_certified\":false,"
              "\"visual_output_certified\":false");
    if (record_has_structural_certificate(result) ||
        (result && result->failure ==
                       SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE)) {
        const ShaderLabStructuralDiagnostic* diagnostic =
            &result->structural_diagnostic;
        sb_append(output, ",\"field\":");
        sb_json_string(output,
                       shaderlab_structural_field_name(diagnostic->field));
        append_optional_index_json(output, "property_index",
                                   diagnostic->property_index);
        append_optional_index_json(output, "subshader_index",
                                   diagnostic->subshader_index);
        append_optional_index_json(output, "pass_index",
                                   diagnostic->pass_index);
        append_optional_index_json(output, "element_index",
                                   diagnostic->element_index);
        sb_appendf(output,
                   ",\"covered_semantic_fields\":%zu,"
                   "\"excluded_compiled_fields\":%zu",
                   diagnostic->covered_semantic_fields,
                   diagnostic->excluded_compiled_fields);
    }
    sb_append_char(output, '}');
}

static void append_structural_diagnostic_table(
    StringBuilder* output, const ShaderBatchRecordResult* result) {
    if (!result ||
        result->failure != SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE) {
        return;
    }
    const ShaderLabStructuralDiagnostic* diagnostic =
        &result->structural_diagnostic;
    sb_appendf(output, ": %s field=%s",
               shaderlab_structural_status_name(diagnostic->status),
               shaderlab_structural_field_name(diagnostic->field));
    if (diagnostic->property_index >= 0)
        sb_appendf(output, " property=%d", diagnostic->property_index);
    if (diagnostic->subshader_index >= 0)
        sb_appendf(output, " subshader=%d", diagnostic->subshader_index);
    if (diagnostic->pass_index >= 0)
        sb_appendf(output, " pass=%d", diagnostic->pass_index);
    if (diagnostic->element_index >= 0)
        sb_appendf(output, " element=%d", diagnostic->element_index);
}

static void append_material_batch_json(
    StringBuilder* output, const ShaderCatalog* catalog,
    const bool* selected, const MaterialBatchResult* batch) {
    sb_append(output, "\"materials\":{\"requested\":");
    sb_append(output, batch ? "true" : "false");
    sb_append(output, ",\"emission_complete\":");
    sb_append(output, batch && batch->catalog_authority == catalog &&
                          batch->selection_authority == selected &&
                          material_batch_is_complete(batch)
                          ? "true" : (batch ? "false" : "null"));
    sb_append(output, ",\"texture_dependency_closure_complete\":");
    sb_append(output,
              batch && batch->catalog_authority == catalog &&
                      batch->selection_authority == selected &&
                      material_batch_texture_dependencies_are_closed(batch)
                  ? "true" : (batch ? "false" : "null"));
    if (!batch) {
        sb_append(output,
                  ",\"summary\":null,\"records\":[]}");
        return;
    }

    const MaterialBatchStats* stats = &batch->stats;
    sb_appendf(output,
        ",\"summary\":{\"catalog_materials\":%zu,"
        "\"selected\":%zu,\"unselected\":%zu,"
        "\"unassociated\":%zu,\"emitted\":%zu,"
        "\"unchanged\":%zu,\"failed\":%zu,"
        "\"shader_dependency_groups\":%zu,"
        "\"texture_dependencies\":%zu,"
        "\"null_texture_dependencies\":%zu,"
        "\"resolved_texture_dependencies\":%zu,"
        "\"exported_texture_dependencies\":%zu,"
        "\"unexported_texture_dependencies\":%zu,"
        "\"failed_texture_dependencies\":%zu},\"records\":[",
        stats->catalog_materials, stats->selected, stats->unselected,
        stats->unassociated, stats->emitted, stats->unchanged, stats->failed,
        stats->shader_dependency_groups,
        stats->texture_dependencies, stats->null_texture_dependencies,
        stats->resolved_texture_dependencies,
        stats->exported_texture_dependencies,
        stats->unexported_texture_dependencies,
        stats->failed_texture_dependencies);

    bool first_record = true;
    for (size_t index = 0U; index < batch->record_count; ++index) {
        const MaterialBatchRecordResult* result = &batch->records[index];
        bool material_published = result->has_artifacts &&
            (result->status == MATERIAL_BATCH_EMITTED ||
             result->status == MATERIAL_BATCH_UNCHANGED);
        const ShaderCatalogMaterialRecord* record =
            index < catalog->material_count ? &catalog->materials[index]
                                             : NULL;
        if (!first_record) sb_append_char(output, ',');
        first_record = false;
        sb_appendf(output, "{\"index\":%zu,\"id\":", index + 1U);
        sb_json_string(output, record ? record->occurrence_id : NULL);
        sb_append(output, ",\"content_id\":");
        sb_json_string(output, record ? record->content_id : NULL);
        sb_append(output, ",\"name\":");
        sb_json_string(output, record ? record->name : NULL);
        sb_append(output, ",\"status\":");
        sb_json_string(output,
                       material_batch_record_status_name(result->status));
        sb_append(output, ",\"failure\":");
        sb_json_string(output, material_batch_failure_name(result->failure));
        sb_append(output, ",\"catalog_status\":");
        sb_json_string(output, record
            ? shader_catalog_material_status_name(record->status) : NULL);
        sb_append(output, ",\"object_status\":");
        sb_json_string(output, record
            ? material_object_status_name(record->object_status) : NULL);
        sb_append(output, ",\"shader_link_status\":");
        sb_json_string(output, record
            ? unity_pptr_resolve_status_name(record->shader_link_status)
            : NULL);
        sb_append(output, ",\"shader_catalog_index\":");
        if (result->shader_record_index < catalog->record_count) {
            sb_appendf(output, "%zu", result->shader_record_index + 1U);
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ",\"guid\":");
        sb_json_string(output, material_published && result->asset_guid[0]
                                   ? result->asset_guid : NULL);
        sb_append(output, ",\"artifact_identity_sha256\":");
        sb_json_string(output, result->artifact_identity_hex[0]
                                   ? result->artifact_identity_hex : NULL);
        sb_appendf(output, ",\"publication_residue\":%s",
                   result->publication_residue ? "true" : "false");
        sb_append(output, ",\"material_publish_status\":");
        sb_json_string(output, result->material_publish_attempted
            ? common_output_publish_status_name(
                  result->material_publish_status)
            : NULL);
        sb_append(output, ",\"meta_publish_status\":");
        sb_json_string(output, result->meta_publish_attempted
            ? common_output_publish_status_name(result->meta_publish_status)
            : NULL);
        sb_append(output, ",\"dependency_evidence_publish_status\":");
        sb_json_string(output, result->evidence_publish_attempted
            ? common_output_publish_status_name(
                  result->evidence_publish_status)
            : NULL);
        sb_append(output, ",\"output\":");
        sb_json_string(output, result->output_path);
        sb_append(output, ",\"meta_output\":");
        sb_json_string(output, result->output_meta_path);
        sb_append(output, ",\"dependency_evidence_output\":");
        sb_json_string(output, result->dependency_evidence_path);
        sb_append(output, ",\"yaml_status\":");
        if (result->status == MATERIAL_BATCH_EMITTED ||
            result->status == MATERIAL_BATCH_UNCHANGED ||
            result->failure == MATERIAL_BATCH_FAILURE_MATERIAL_YAML) {
            sb_json_string(output,
                           unity_material_yaml_status_name(
                               result->yaml_status));
        } else {
            sb_append(output, "null");
        }
        sb_appendf(output,
                   ",\"texture_dependency_closure_complete\":%s,"
                   "\"dependencies\":[",
                   result->texture_dependency_closure_complete
                       ? "true" : "false");
        for (size_t dependency_index = 0U;
             dependency_index < result->dependency_count;
             ++dependency_index) {
            const MaterialBatchTextureDependency* dependency =
                &result->dependencies[dependency_index];
            if (dependency_index != 0U) sb_append_char(output, ',');
            sb_appendf(output,
                       "{\"property_index\":%zu,\"property_name\":",
                       dependency->property_index);
            sb_json_bytes(output, dependency->property_name,
                          dependency->property_name_size);
            sb_appendf(output,
                       ",\"serialized_file_id\":%" PRId32
                       ",\"serialized_path_id\":\"%" PRId64 "\","
                       "\"resolve_status\":",
                       dependency->serialized_file_id,
                       dependency->serialized_path_id);
            sb_json_string(output, unity_pptr_resolve_status_name(
                                       dependency->resolve_status));
            sb_append(output, ",\"status\":");
            sb_json_string(output, material_batch_dependency_status_name(
                                       dependency->status));
            sb_append(output, ",\"reference_status\":");
            sb_json_string(output,
                           material_batch_texture_reference_status_name(
                               dependency->reference_status));
            sb_append(output, ",\"target_source_index\":");
            if (dependency->has_target) {
                sb_appendf(output, "%zu", dependency->target_source_index + 1U);
            } else {
                sb_append(output, "null");
            }
            sb_append(output, ",\"target_serialized_sha256\":");
            sb_json_string(output, dependency->has_target
                ? dependency->target_serialized_digest_hex : NULL);
            sb_append(output, ",\"target_class_id\":");
            if (dependency->has_target) {
                sb_appendf(output, "%" PRId32, dependency->target_class_id);
            } else {
                sb_append(output, "null");
            }
            sb_append(output, ",\"target_path_id\":");
            if (dependency->has_target) {
                sb_appendf(output, "\"%" PRId64 "\"",
                           dependency->target_path_id);
            } else {
                sb_append(output, "null");
            }
            sb_append(output, ",\"external_serialized_guid\":");
            if (dependency->has_external_serialized_guid) {
                sb_hex_bytes(output, dependency->external_serialized_guid,
                             sizeof(dependency->external_serialized_guid));
            } else {
                sb_append(output, "null");
            }
            sb_append(output, ",\"external_index\":");
            if (dependency->external_index != SIZE_MAX) {
                sb_appendf(output, "%zu", dependency->external_index + 1U);
            } else {
                sb_append(output, "null");
            }
            sb_append(output, ",\"external_serialized_type\":");
            if (dependency->has_external_serialized_guid) {
                sb_appendf(output, "%" PRId32,
                           dependency->external_serialized_type);
            } else {
                sb_append(output, "null");
            }
            sb_append(output, ",\"external_unity_guid_text\":");
            if (dependency->has_external_serialized_guid) {
                char external_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
                if (unity_asset_guid_from_serialized_bytes(
                        dependency->external_serialized_guid,
                        external_guid)) {
                    sb_json_string(output, external_guid);
                } else {
                    sb_append(output, "null");
                }
            } else {
                sb_append(output, "null");
            }
            sb_append(output, ",\"yaml_reference\":");
            if (dependency->has_yaml_reference) {
                sb_appendf(output, "{\"file_id\":\"%" PRId64
                                   "\",\"guid\":",
                           dependency->yaml_reference.file_id);
                sb_json_string(output, dependency->yaml_reference.guid);
                sb_appendf(output, ",\"type\":%" PRId32
                                   ",\"asset_exported\":%s}",
                           dependency->yaml_reference.type,
                           dependency->yaml_reference.asset_exported
                               ? "true" : "false");
            } else {
                sb_append(output, "null");
            }
            sb_append_char(output, '}');
        }
        sb_append(output, "],\"source\":");
        sb_json_string(output, record ? record->outer_path : NULL);
        sb_append(output, ",\"member\":");
        sb_json_string(output, record ? record->member_name : NULL);
        sb_append(output, ",\"path_id\":");
        if (record) {
            sb_appendf(output, "\"%" PRId64 "\"", record->path_id);
        } else {
            sb_append(output, "null");
        }
        sb_append_char(output, '}');
    }
    sb_append(output, "]}");
}

static void append_material_batch_table(
    StringBuilder* output, const ShaderCatalog* catalog,
    const MaterialBatchResult* batch) {
    if (!batch) return;
    sb_append(output,
              "\nMaterials:\n"
              "MAT#\tSTATUS\tMATERIAL\tSHADER#\tOUTPUT/FAILURE\t"
              "TEXTURE_CLOSURE\n");
    for (size_t index = 0U; index < batch->record_count; ++index) {
        const MaterialBatchRecordResult* result = &batch->records[index];
        /* The complete machine-readable ledger remains in JSON.  The table
         * is an operator view, so omit unrelated and shader-null records and
         * retain only attempted exports and actionable failures. */
        if (result->status == MATERIAL_BATCH_UNSELECTED ||
            result->status == MATERIAL_BATCH_UNASSOCIATED) {
            continue;
        }
        const ShaderCatalogMaterialRecord* record =
            index < catalog->material_count ? &catalog->materials[index]
                                             : NULL;
        sb_appendf(output, "%zu\t%s\t", index + 1U,
                   material_batch_record_status_name(result->status));
        sb_table_text(output, record ? record->name : NULL);
        if (result->shader_record_index < catalog->record_count) {
            sb_appendf(output, "\t%zu\t", result->shader_record_index + 1U);
        } else {
            sb_append(output, "\t-\t");
        }
        if (result->output_path) {
            sb_table_text(output, result->output_path);
        } else {
            sb_append(output, material_batch_failure_name(result->failure));
        }
        sb_appendf(output, "\t%s\n",
                   result->texture_dependency_closure_complete
                       ? "complete" : "open");
    }
    sb_appendf(output,
        "Material summary: catalog=%zu selected=%zu unselected=%zu "
        "unassociated=%zu emitted=%zu unchanged=%zu failed=%zu "
        "shader_dependency_groups=%zu "
        "material_emission_complete=%s "
        "texture_dependencies=%zu resolved=%zu unexported=%zu "
        "dependency_closure_complete=%s\n",
        batch->stats.catalog_materials, batch->stats.selected,
        batch->stats.unselected, batch->stats.unassociated,
        batch->stats.emitted, batch->stats.unchanged, batch->stats.failed,
        batch->stats.shader_dependency_groups,
        material_batch_is_complete(batch) ? "yes" : "no",
        batch->stats.texture_dependencies,
        batch->stats.resolved_texture_dependencies,
        batch->stats.unexported_texture_dependencies,
        material_batch_texture_dependencies_are_closed(batch)
            ? "yes" : "no");
    if (!material_batch_texture_dependencies_are_closed(batch)) {
        sb_append(output,
            "Material dependency note: exact dependency closure was not "
            "established, so affected .mat artifacts were not published. "
            "Inspect the typed record failures before retrying.\n");
    }
}

static void append_native_texture_batch_json(
    StringBuilder* output, const ShaderCatalog* catalog,
    const NativeTextureBatchResult* batch, bool batch_complete,
    const bool* record_publications) {
    sb_append(output, "\"native_textures\":{\"requested\":");
    sb_append(output, batch ? "true" : "false");
    sb_append(output, ",\"complete\":");
    sb_append(output, batch ? (batch_complete ? "true" : "false")
                            : "null");
    sb_append(output, ",\"source_transaction_complete\":");
    sb_append(output, batch ? (batch->source_transaction_complete
                                  ? "true" : "false")
                            : "null");
    sb_append(output, ",\"source_transaction_failure\":");
    sb_json_string(output, batch
        ? native_texture_batch_failure_name(
              batch->source_transaction_failure)
        : NULL);
    sb_append(output, ",\"staging_cleanup_complete\":");
    sb_append(output, batch ? (batch->staging_cleanup_complete
                                  ? "true" : "false")
                            : "null");
    sb_append(output, ",\"staging_directory_residue\":");
    sb_json_string(output, batch ? batch->staging_directory : NULL);
    if (!batch) {
        sb_append(output, ",\"summary\":null,\"records\":[]}");
        return;
    }
    sb_appendf(output,
        ",\"summary\":{\"selected\":%zu,\"texture2d\":%zu,"
        "\"render_texture\":%zu,\"emitted\":%zu,"
        "\"unchanged\":%zu,\"failed\":%zu,"
        "\"staging_residues\":%zu},\"records\":[",
        batch->stats.selected, batch->stats.texture2d,
        batch->stats.render_texture, batch->stats.emitted,
        batch->stats.unchanged, batch->stats.failed,
        batch->staging_residue_count);
    for (size_t index = 0U; index < batch->record_count; ++index) {
        const NativeTextureBatchRecordResult* record = &batch->records[index];
        const ShaderCatalogSource* source =
            record->source_index < catalog->source_count
                ? &catalog->sources[record->source_index] : NULL;
        bool published = record_publications && record_publications[index];
        if (index != 0U) sb_append_char(output, ',');
        sb_appendf(output, "{\"index\":%zu,\"source_index\":%zu,"
                           "\"source_occurrence_id\":",
                   index + 1U, record->source_index + 1U);
        sb_json_string(output, record->source_occurrence_id[0]
                                   ? record->source_occurrence_id : NULL);
        sb_append(output, ",\"source_serialized_sha256\":");
        char source_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
        common_sha256_digest_to_hex(record->source_serialized_digest,
                                    source_digest_hex);
        sb_json_string(output, record->source_occurrence_id[0]
                                   ? source_digest_hex : NULL);
        sb_append(output, ",\"source\":");
        sb_json_string(output, source ? source->outer_path : NULL);
        sb_append(output, ",\"member\":");
        sb_json_string(output, source ? source->member_name : NULL);
        sb_appendf(output,
                   ",\"class_id\":%" PRId32
                   ",\"path_id\":\"%" PRId64 "\",\"status\":",
                   record->class_id, record->path_id);
        sb_json_string(output,
                       native_texture_batch_record_status_name(
                           record->status));
        sb_append(output, ",\"failure\":");
        sb_json_string(output,
                       native_texture_batch_failure_name(record->failure));
        sb_append(output, ",\"object_status\":");
        sb_json_string(output,
                       unity_texture_object_status_name(
                           record->object_status));
        sb_append(output, ",\"resource_status\":");
        sb_json_string(output,
                       common_file_status_name(record->resource_status));
        sb_append(output, ",\"yaml_status\":");
        sb_json_string(output, record->yaml_attempted
            ? unity_native_texture_yaml_status_name(record->yaml_status)
            : NULL);
        sb_append(output, ",\"resource_sha256\":");
        sb_json_string(output, record->resource_digest_hex[0]
                                   ? record->resource_digest_hex : NULL);
        sb_append(output, ",\"artifact_identity_sha256\":");
        sb_json_string(output, record->artifact_identity_hex[0]
                                   ? record->artifact_identity_hex : NULL);
        sb_append(output, ",\"guid\":");
        sb_json_string(output, published ? record->asset_guid : NULL);
        sb_append(output, ",\"file_id\":");
        if (published && record->class_id == UNITY_TEXTURE2D_CLASS_ID) {
            sb_appendf(output, "\"%" PRId64 "\"",
                       UNITY_TEXTURE2D_LOCAL_FILE_ID);
        } else if (published &&
                   record->class_id == UNITY_RENDER_TEXTURE_CLASS_ID) {
            sb_appendf(output, "\"%" PRId64 "\"",
                       UNITY_RENDER_TEXTURE_LOCAL_FILE_ID);
        } else {
            sb_append(output, "null");
        }
        sb_append(output, ",\"reference_type\":");
        sb_append(output, published ? "2" : "null");
        sb_append(output, ",\"output\":");
        sb_json_string(output, record->output_path);
        sb_append(output, ",\"meta_output\":");
        sb_json_string(output, record->output_meta_path);
        sb_append(output, ",\"asset_publish_status\":");
        sb_json_string(output, record->asset_publish_attempted
            ? common_output_publish_status_name(
                  record->asset_publish_status)
            : NULL);
        sb_append(output, ",\"meta_publish_status\":");
        sb_json_string(output, record->meta_publish_attempted
            ? common_output_publish_status_name(
                  record->meta_publish_status)
            : NULL);
        sb_appendf(output, ",\"publication_residue\":%s",
                   record->publication_residue ? "true" : "false");
        sb_appendf(output, ",\"staging_residue\":%s",
                   record->staging_residue ? "true" : "false");
        sb_append(output, ",\"staging_residue_path\":");
        sb_json_string(output, record->staging_residue
                                   ? record->staging_path : NULL);
        sb_append_char(output, '}');
    }
    sb_append(output, "]}");
}

static void append_native_texture_batch_table(
    StringBuilder* output, const NativeTextureBatchResult* batch,
    bool batch_complete) {
    if (!batch) return;
    sb_append(output,
              "\nNative textures:\n"
              "TEX#\tSTATUS\tCLASS\tPATH_ID\tOUTPUT/FAILURE\n");
    for (size_t index = 0U; index < batch->record_count; ++index) {
        const NativeTextureBatchRecordResult* record = &batch->records[index];
        sb_appendf(output, "%zu\t%s\t%" PRId32 "\t%" PRId64 "\t",
                   index + 1U,
                   native_texture_batch_record_status_name(record->status),
                   record->class_id, record->path_id);
        if (record->output_path) sb_table_text(output, record->output_path);
        else sb_append(output,
                       native_texture_batch_failure_name(record->failure));
        sb_append_char(output, '\n');
    }
    sb_appendf(output,
        "Native texture summary: selected=%zu texture2d=%zu "
        "render_texture=%zu emitted=%zu unchanged=%zu failed=%zu "
        "source_transaction_complete=%s staging_cleanup_complete=%s "
        "staging_residues=%zu complete=%s\n",
        batch->stats.selected, batch->stats.texture2d,
        batch->stats.render_texture, batch->stats.emitted,
        batch->stats.unchanged, batch->stats.failed,
        batch->source_transaction_complete ? "yes" : "no",
        batch->staging_cleanup_complete ? "yes" : "no",
        batch->staging_residue_count,
        batch_complete ? "yes" : "no");
}

static bool append_compute_publication_json(
    StringBuilder* output, const ShaderBatchRecordResult* result) {
    if (!output || !result ||
        (result->compute_artifact_publication_count != 0U &&
         !result->compute_artifact_publications)) {
        return false;
    }
    sb_appendf(output,
               ",\"compute_publication\":{"
               "\"artifact_count\":%zu,"
               "\"preflight_attempted\":%s,"
               "\"preflight_status\":",
               result->compute_artifact_publication_count,
               result->compute_preflight_attempted ? "true" : "false");
    sb_json_string(output, result->compute_preflight_attempted
        ? common_output_preflight_status_name(
              result->compute_preflight_status)
        : NULL);
    sb_appendf(output,
               ",\"publish_attempted\":%s,"
               "\"publish_status\":",
               result->compute_publish_attempted ? "true" : "false");
    sb_json_string(output, result->compute_publish_attempted
        ? common_output_publish_status_name(
              result->compute_publish_status)
        : NULL);
    sb_appendf(output, ",\"residue\":%s,\"artifacts\":[",
               result->compute_publication_residue
                   ? "true" : "false");
    for (size_t index = 0U;
         index < result->compute_artifact_publication_count; ++index) {
        const ShaderBatchComputeArtifactPublication* artifact =
            &result->compute_artifact_publications[index];
        if (index != 0U) sb_append_char(output, ',');
        sb_append(output, "{\"filename\":");
        sb_json_string(output, artifact->filename);
        sb_appendf(output,
                   ",\"is_manifest\":%s,"
                   "\"preflight_attempted\":%s,"
                   "\"preflight_status\":",
                   artifact->is_manifest ? "true" : "false",
                   artifact->preflight_attempted ? "true" : "false");
        sb_json_string(output, artifact->preflight_attempted
            ? common_output_preflight_status_name(
                  artifact->preflight_status)
            : NULL);
        sb_appendf(output,
                   ",\"publish_attempted\":%s,"
                   "\"publish_status\":",
                   artifact->publish_attempted ? "true" : "false");
        sb_json_string(output, artifact->publish_attempted
            ? common_output_publish_status_name(
                  artifact->publish_status)
            : NULL);
        sb_appendf(output, ",\"publication_residue\":%s}",
                   artifact->publication_residue ? "true" : "false");
    }
    sb_append(output, "]}");
    return sb_ok(output);
}

static bool render_extract_json(const ShaderCatalog* catalog,
                                const bool* selected,
                                const ShaderBatchResult* batch,
                                const MaterialBatchResult* materials,
                                const NativeTextureBatchResult* textures,
                                CliShaderKind selection_kind,
                                const char* registry_path,
                                const char* registry_digest,
                                StringBuilder* output,
                                bool* out_texture_batch_complete) {
    if (!out_texture_batch_complete) return false;
    *out_texture_batch_complete = false;
    bool* texture_publications = NULL;
    bool texture_batch_complete = true;
    if (textures) {
        if (textures->record_count != 0U) {
            texture_publications = (bool*)calloc(
                textures->record_count, sizeof(*texture_publications));
            if (!texture_publications) return false;
        }
        texture_batch_complete =
            textures->catalog_authority == catalog &&
            textures->selection_authority == selected &&
            native_texture_batch_validate_publication_snapshot(
                textures, texture_publications, textures->record_count);
    }
    const bool catalog_complete = shader_catalog_is_complete(catalog);
    const bool input_complete = catalog->issue_count == 0U;
    const bool emission_complete = batch->stats.selected != 0U &&
        shader_batch_is_complete(batch);
    const bool material_emission_complete =
        !materials || (materials->catalog_authority == catalog &&
                       materials->selection_authority == selected &&
                       material_batch_is_complete(materials));
    const bool texture_dependency_closure_complete =
        (!textures || texture_batch_complete) &&
        (!materials || (materials->catalog_authority == catalog &&
                        materials->selection_authority == selected &&
                        material_batch_texture_dependencies_are_closed(
                            materials)));
    const bool complete = input_complete && emission_complete &&
        material_emission_complete && texture_dependency_closure_complete;
    size_t structurally_covered = 0U;
    size_t exact_compute_packages = 0U;
    size_t structural_failures = 0U;
    size_t selected_graphics = 0U;
    size_t selected_compute = 0U;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (!selected[i]) continue;
        const ShaderBatchRecordResult* record_result = &batch->records[i];
        if (catalog->records[i].class_id == 48) ++selected_graphics;
        else if (catalog->records[i].class_id == 72) ++selected_compute;
        if (catalog->records[i].class_id == 48 &&
            record_has_structural_certificate(record_result)) {
            ++structurally_covered;
        } else if (catalog->records[i].class_id == 72 &&
                   record_result->compute_artifact_status ==
                       COMPUTE_SHADER_ARTIFACT_OK &&
                   record_result->publication_authorized &&
                   (record_result->status == SHADER_BATCH_EMITTED ||
                    record_result->status == SHADER_BATCH_UNCHANGED)) {
            ++exact_compute_packages;
        }
        if (record_result->failure ==
                SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE) {
            ++structural_failures;
        }
    }
    const char* artifact_kind =
        selected_graphics != 0U && selected_compute != 0U
            ? "mixed"
            : (selected_compute != 0U
                   ? (exact_compute_packages == selected_compute
                          ? "exact-compute-package"
                          : "unpublished-compute-package")
                   : "uncertified-shaderlab-candidate");
    sb_append(output,
              "{\"report_schema\":\"dxbc-sandbox-report\","
              "\"report_version\":7,\"command\":\"extract\","
              "\"selection_kind\":");
    sb_json_string(output, cli_shader_kind_name(selection_kind));
    sb_append(output,
              ","
              "\"artifact_kind\":");
    sb_json_string(output, artifact_kind);
    sb_append(output, ","
              "\"complete\":");
    sb_append(output, complete ? "true," : "false,");
    sb_append(output, "\"input_complete\":");
    sb_append(output, input_complete ? "true," : "false,");
    sb_append(output, "\"catalog_complete\":");
    sb_append(output, catalog_complete ? "true," : "false,");
    sb_append(output, "\"emission_complete\":");
    sb_append(output, emission_complete ? "true," : "false,");
    sb_append(output, "\"material_emission_complete\":");
    sb_append(output, material_emission_complete ? "true," : "false,");
    sb_append(output, "\"texture_dependency_closure_complete\":");
    sb_append(output, texture_dependency_closure_complete
                          ? "true," : "false,");
    sb_append(output, "\"certification\":{\"status\":");
    sb_json_string(output,
        structurally_covered != 0U
            ? (exact_compute_packages != 0U ? "mixed" : "structural-only")
            : (exact_compute_packages != 0U
                   ? "binary-exact-source-fail-closed" : "not-run"));
    sb_append(output, ",\"scope\":");
    sb_json_string(output,
        structurally_covered != 0U
            ? (exact_compute_packages != 0U
                   ? "mixed-graphics-structure-and-compute-binary"
                   : "serialized-d3d11-shaderlab-structure")
            : (exact_compute_packages != 0U
                   ? "serialized-compute-binary-authority" : "not-run"));
    sb_append(output,
              ",\"d3d11\":\"not-run\","
              "\"glsl\":\"not-run\","
              "\"variant_selection\":\"not-run\","
              "\"pipeline_state\":");
    sb_json_string(output, structural_failures != 0U
        ? "structural-coverage-failed"
        : (structurally_covered != 0U
               ? "serialized-structural-only" : "not-run"));
    sb_append(output,
              ",\"runtime_selection\":\"not-run\","
              "\"visual_output\":\"not-run\"},");
    append_registry_json(output, registry_path, registry_digest);
    sb_appendf(output,
        ",\"summary\":{\"selected\":%zu,\"emitted\":%zu,"
        "\"unchanged\":%zu,\"unavailable\":%zu,\"failed\":%zu,"
        "\"structurally_covered\":%zu,\"structural_failures\":%zu,"
        "\"exact_compute_packages\":%zu},"
        "\"shaders\":[",
        batch->stats.selected, batch->stats.emitted,
        batch->stats.unchanged, batch->stats.unavailable,
        batch->stats.failed, structurally_covered, structural_failures,
        exact_compute_packages);
    bool first = true;
    size_t view_index = 0U;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (record_matches_kind(&catalog->records[i], selection_kind)) {
            ++view_index;
        }
        if (!selected[i]) continue;
        if (!first) sb_append_char(output, ',');
        first = false;
        const ShaderCatalogRecord* record = &catalog->records[i];
        const ShaderBatchRecordResult* result = &batch->records[i];
        sb_appendf(output,
                   "{\"index\":%zu,\"catalog_index\":%zu,\"id\":",
                   view_index, i + 1U);
        sb_json_string(output, record->occurrence_id);
        sb_append(output, ",\"content_id\":");
        sb_json_string(output, record->content_id);
        sb_append(output, ",\"name\":");
        sb_json_string(output, record->name);
        sb_append(output, ",\"kind\":");
        sb_json_string(output, catalog_record_kind_name(record));
        sb_append(output, ",\"status\":");
        sb_json_string(output,
                       shader_batch_record_status_name(result->status));
        sb_append(output, ",\"failure\":");
        sb_json_string(output, shader_batch_failure_name(result->failure));
        sb_append(output, ",\"schema_status\":");
        sb_json_string(output,
                       typetree_schema_status_name(result->schema_status));
        sb_append(output, ",\"object_status\":");
        sb_json_string(output,
                       shader_object_status_name(result->object_status));
        sb_append(output, ",\"compute_object_status\":");
        sb_json_string(output, compute_shader_object_status_name(
                                   result->compute_object_status));
        sb_append(output, ",\"compute_layout_authority\":");
        sb_json_string(
            output,
            result->compute_object_status == COMPUTE_SHADER_OBJECT_OK
                ? COMPUTE_SHADER_OBJECT_LAYOUT_AUTHORITY : NULL);
        sb_append(output, ",\"compute_artifact_status\":");
        sb_json_string(output, compute_shader_artifact_status_name(
                                   result->compute_artifact_status));
        sb_append(output, ",\"compute_source_authority\":");
        sb_json_string(output, compute_shader_source_authority_status_name(
                                   result->compute_source_authority_status));
        sb_append(output, ",\"input_status\":");
        sb_json_string(output,
                       unity_input_status_name(result->input_status));
        sb_append(output, ",\"output\":");
        sb_json_string(output, result->publication_authorized
                                   ? result->output_path : NULL);
        sb_append(output, ",\"asset_guid\":");
        sb_json_string(output, result->publication_authorized &&
                               result->has_asset_meta
                                   ? result->asset_guid : NULL);
        sb_append(output, ",\"meta_output\":");
        sb_json_string(output, result->publication_authorized
                                   ? result->output_meta_path : NULL);
        sb_appendf(output,
                   ",\"publication_authorized\":%s,"
                   "\"source_identity_close_deferred\":%s,"
                   "\"publication_residue\":%s,"
                   "\"shader_publication_residue\":%s,"
                   "\"meta_publication_residue\":%s",
                   result->publication_authorized ? "true" : "false",
                   result->source_identity_close_deferred
                       ? "true" : "false",
                   result->publication_residue ? "true" : "false",
                   result->shader_publication_residue ? "true" : "false",
                   result->meta_publication_residue ? "true" : "false");
        sb_append(output, ",\"shader_publication_residue_path\":");
        sb_json_string(output,
            result->shader_publication_residue
                ? result->shader_publication_residue_path : NULL);
        sb_append(output, ",\"meta_publication_residue_path\":");
        sb_json_string(output,
            result->meta_publication_residue
                ? result->meta_publication_residue_path : NULL);
        sb_append(output, ",\"shader_preflight_status\":");
        sb_json_string(output, result->shader_preflight_attempted
            ? common_output_preflight_status_name(
                  result->shader_preflight_status)
            : NULL);
        sb_append(output, ",\"shader_publish_status\":");
        sb_json_string(output, result->shader_publish_attempted
            ? common_output_publish_status_name(
                  result->shader_publish_status)
            : NULL);
        sb_append(output, ",\"meta_preflight_status\":");
        sb_json_string(output, result->meta_preflight_attempted
            ? common_output_preflight_status_name(
                  result->meta_preflight_status)
            : NULL);
        sb_append(output, ",\"meta_publish_status\":");
        sb_json_string(output, result->meta_publish_attempted
            ? common_output_publish_status_name(
                  result->meta_publish_status)
            : NULL);
        if (!append_compute_publication_json(output, result)) {
            free(texture_publications);
            return false;
        }
        append_candidate_diagnostic_json(output, result);
        append_structural_diagnostic_json(output, result);
        sb_append(output, ",\"artifact_kind\":");
        sb_json_string(output, record->class_id == 72
            ? (result->publication_authorized
                   ? "exact-compute-package"
                   : "unpublished-compute-package")
            : "uncertified-shaderlab-candidate");
        sb_append(output, ",\"certification_status\":");
        if (record->class_id == 72 &&
            result->compute_artifact_status == COMPUTE_SHADER_ARTIFACT_OK &&
            result->publication_authorized) {
            sb_json_string(output, "binary-exact-source-fail-closed");
        } else if (record->class_id == 72 &&
                   result->compute_artifact_status ==
                       COMPUTE_SHADER_ARTIFACT_OK) {
            sb_json_string(output, "failed-closed");
        } else {
            sb_json_string(output, record_has_structural_certificate(result)
                ? "structural-only"
                : (result->failure ==
                           SHADER_BATCH_FAILURE_STRUCTURAL_COVERAGE
                       ? "failed-closed" : "not-run"));
        }
        sb_append(output, ",\"source\":");
        sb_json_string(output, record->outer_path);
        sb_append(output, ",\"member\":");
        sb_json_string(output, record->member_name);
        sb_appendf(output,
                   ",\"member_index\":%zu,\"path_id\":\"%" PRId64 "\"}",
                   record->member_index, record->path_id);
    }
    sb_append(output, "],");
    append_native_texture_batch_json(
        output, catalog, textures, texture_batch_complete,
        texture_publications);
    sb_append_char(output, ',');
    append_material_batch_json(output, catalog, selected, materials);
    sb_append_char(output, ',');
    append_catalog_sources_json(output, catalog);
    sb_append(output, ",\"catalog\":");
    append_catalog_stats_json(output, catalog);
    append_catalog_issues_json(output, catalog);
    append_catalog_failures_json(output, catalog);
    append_catalog_material_failures_json(output, catalog);
    sb_append(output, "}\n");
    free(texture_publications);
    *out_texture_batch_complete = texture_batch_complete;
    return sb_ok(output);
}

static bool render_extract_table(const ShaderCatalog* catalog,
                                 const bool* selected,
                                 const ShaderBatchResult* batch,
                                 const MaterialBatchResult* materials,
                                 const NativeTextureBatchResult* textures,
                                 CliShaderKind selection_kind,
                                 bool show_sources,
                                 const char* registry_path,
                                 const char* registry_digest,
                                 StringBuilder* output,
                                 bool* out_texture_batch_complete) {
    if (!out_texture_batch_complete) return false;
    const bool texture_batch_complete =
        !textures || native_texture_batch_is_complete(textures);
    *out_texture_batch_complete = texture_batch_complete;
    sb_append(output, "#\tKIND\tSTATUS\tSHADER\tOUTPUT/FAILURE\n");
    size_t view_index = 0U;
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        if (record_matches_kind(&catalog->records[i], selection_kind)) {
            ++view_index;
        }
        if (!selected[i]) continue;
        const ShaderBatchRecordResult* result = &batch->records[i];
        sb_appendf(output, "%zu\t%s\t%s\t", view_index,
                   catalog_record_kind_name(&catalog->records[i]),
                   shader_batch_record_status_name(result->status));
        sb_table_text(output, catalog->records[i].name);
        sb_append_char(output, '\t');
        if (result->publication_authorized && result->output_path) {
            sb_table_text(output, result->output_path);
        } else if (result->output_path) {
            sb_append(output, "publication-not-authorized");
        } else {
            sb_append(output, shader_batch_failure_name(result->failure));
            append_candidate_diagnostic_table(output, result);
            append_structural_diagnostic_table(output, result);
            if (result->object_status != SHADER_OBJECT_OK) {
                sb_appendf(output, " (%s)",
                           shader_object_status_name(result->object_status));
            }
            if (result->compute_object_status !=
                    COMPUTE_SHADER_OBJECT_NOT_APPLICABLE &&
                result->compute_object_status != COMPUTE_SHADER_OBJECT_OK) {
                sb_appendf(output, " (%s)",
                    compute_shader_object_status_name(
                        result->compute_object_status));
            }
        }
        sb_append_char(output, '\n');
    }
    append_native_texture_batch_table(
        output, textures, texture_batch_complete);
    append_material_batch_table(output, catalog, materials);
    if (show_sources) append_catalog_sources_table(output, catalog);
    sb_appendf(output,
        "\nSummary: kind=%s selected=%zu emitted=%zu unchanged=%zu unavailable=%zu "
        "failed=%zu complete=%s input_complete=%s emission_complete=%s "
        "material_emission_complete=%s texture_dependency_closure=%s "
        "catalog_complete=%s\n",
        cli_shader_kind_name(selection_kind), batch->stats.selected,
        batch->stats.emitted,
        batch->stats.unchanged, batch->stats.unavailable,
        batch->stats.failed,
        catalog->issue_count == 0U && batch->stats.selected != 0U &&
                shader_batch_is_complete(batch) &&
                (!materials || material_batch_is_complete(materials)) &&
                texture_batch_complete &&
                (!materials || material_batch_texture_dependencies_are_closed(
                                  materials))
            ? "yes" : "no",
        catalog->issue_count == 0U ? "yes" : "no",
        batch->stats.selected != 0U && shader_batch_is_complete(batch)
            ? "yes" : "no",
        !materials || material_batch_is_complete(materials)
            ? "yes" : "no",
        texture_batch_complete &&
                (!materials || material_batch_texture_dependencies_are_closed(
                                  materials)) ? "yes" : "no",
        shader_catalog_is_complete(catalog) ? "yes" : "no");
    if (selection_kind == CLI_SHADER_KIND_COMPUTE) {
        sb_append(output,
            "Artifacts: exact serialized compute manifests and compiled "
            "program bytes. Embedded DXBC is also extracted when present; "
            ".compute source is not emitted without a certified declaration "
            "inverse.\n");
    } else if (selection_kind == CLI_SHADER_KIND_GRAPHICS) {
        sb_append(output,
            "Artifacts: uncertified ShaderLab candidates. Successful "
            "outputs passed serialized ShaderLab structural coverage only; "
            "compiler variants, runtime selection, resource binding, and "
            "visual output were not certified.\n");
    } else {
        sb_append(output,
            "Artifacts: graphics outputs are structurally covered ShaderLab "
            "candidates; compute outputs preserve exact serialized binaries "
            "and manifests while .compute source remains fail-closed.\n");
    }
    sb_appendf(output, "Registry: %s  %s\n",
               registry_digest, registry_path);
    if (!shader_catalog_is_complete(catalog)) {
        sb_appendf(output,
            "Catalog: shaders=%zu ready=%zu unavailable=%zu failed=%zu "
            "issues=%zu complete=no\n",
            catalog->stats.shader_objects, catalog->stats.ready_shaders,
            catalog->stats.unavailable_shaders,
            catalog->stats.failed_shaders, catalog->issue_count);
        for (size_t i = 0U; i < catalog->issue_count; ++i) {
            const ShaderCatalogIssue* issue = &catalog->issues[i];
            sb_appendf(output, "Issue: %s (%s): ",
                       shader_catalog_issue_code_name(issue->code),
                       issue->discovery_status != COMMON_PATH_DISCOVERY_OK
                           ? common_path_discovery_status_name(
                                 issue->discovery_status)
                           : unity_input_status_name(issue->input_status));
            sb_table_text(output, issue->outer_path);
            if (issue->member_name) {
                sb_append(output, "::");
                sb_table_text(output, issue->member_name);
            }
            sb_append_char(output, '\n');
        }
        for (size_t i = 0U; i < catalog->record_count; ++i) {
            const ShaderCatalogRecord* record = &catalog->records[i];
            if (selected[i] ||
                record->status == SHADER_CATALOG_RECORD_READY) {
                continue;
            }
            sb_appendf(output,
                       "Catalog failure: #%zu %s (%s): ", i + 1U,
                       shader_catalog_record_status_name(record->status),
                       record->class_id == 72
                           ? compute_shader_inventory_status_name(
                                 record->compute_inventory_status)
                           : shader_object_status_name(
                                 record->object_status));
            sb_table_text(output, record->name);
            sb_append(output, " @ ");
            sb_table_text(output, record->outer_path);
            sb_append_char(output, '\n');
        }
    }
    return sb_ok(output);
}

static bool publish_report(const char* path, const StringBuilder* output) {
    CommonFileStatus status = common_file_write_new_atomic(
        path, output->buf, output->len);
    if (status == COMMON_FILE_OK) return true;
    if (status != COMMON_FILE_ALREADY_EXISTS) return false;
    CommonFileBytes existing;
    status = common_file_read_regular(path, SIZE_MAX, &existing);
    bool identical = status == COMMON_FILE_OK &&
        existing.size == output->len &&
        (existing.size == 0U ||
         memcmp(existing.data, output->buf, existing.size) == 0);
    common_file_bytes_dispose(&existing);
    return identical;
}

static int dxbc_sandbox_main(int argc, char** argv) {
    CliOptions options;
    bool help_requested = false;
    if (!parse_cli(argc, argv, &options, &help_requested)) {
        print_usage(stderr, argv[0]);
        cli_options_dispose(&options);
        return 2;
    }
    if (help_requested) {
        print_usage(stdout, argv[0]);
        cli_options_dispose(&options);
        return 0;
    }

    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    char* registry_path = NULL;
    uint8_t registry_digest_bytes[COMMON_SHA256_DIGEST_SIZE];
    TypeTreeSchemaStatus registry_status = load_schema_registry(
        &registry, options.schema_registry, argv[0], &registry_path,
        registry_digest_bytes);
    if (registry_status != TYPETREE_SCHEMA_OK) {
        fprintf(stderr,
                "Error: could not load the exact Shader TypeTree registry: "
                "%s. Use --schema-registry FILE.\n",
                typetree_schema_status_name(registry_status));
        typetree_schema_registry_dispose(&registry);
        cli_options_dispose(&options);
        return 1;
    }
    char registry_digest[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    common_sha256_digest_to_hex(registry_digest_bytes, registry_digest);

    ShaderCatalogOptions catalog_options;
    shader_catalog_options_default(&catalog_options);
    catalog_options.recursive = options.recursive;
    catalog_options.schema_registry = &registry;
    catalog_options.include_materials = options.export_materials;
    catalog_options.retain_source_snapshots =
        options.command == CLI_COMMAND_EXTRACT && options.export_materials;
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    ShaderCatalogStatus catalog_status = shader_catalog_build(
        options.inputs.values, options.inputs.count,
        &catalog_options, &catalog);
    if (catalog_status != SHADER_CATALOG_OK) {
        fprintf(stderr, "Error: shader catalog failed: %s.\n",
                shader_catalog_status_name(catalog_status));
        free(registry_path);
        shader_catalog_dispose(&catalog);
        typetree_schema_registry_dispose(&registry);
        cli_options_dispose(&options);
        return 1;
    }

    bool* selected = NULL;
    if (catalog.record_count != 0U) {
        selected = (bool*)calloc(catalog.record_count, sizeof(*selected));
        if (!selected) {
            fprintf(stderr, "Error: out of memory selecting shaders.\n");
            free(registry_path);
            shader_catalog_dispose(&catalog);
            typetree_schema_registry_dispose(&registry);
            cli_options_dispose(&options);
            return 1;
        }
    }
    const char* failed_selector = NULL;
    SelectorStatus selector_status = apply_selectors(
        &options, &catalog, selected, &failed_selector);
    bool selection_blocked_by_incomplete_catalog = false;
    if (selector_status != SELECTOR_OK) {
        if (selector_status == SELECTOR_ALLOCATION_FAILED) {
            fprintf(stderr, "Error: out of memory evaluating selector '%s'.\n",
                    failed_selector ? failed_selector : "<unknown>");
        } else if (selector_status == SELECTOR_INVALID) {
            fprintf(stderr, "Error: selector '%s' is invalid.\n",
                    failed_selector ? failed_selector : "<unknown>");
        } else if (failed_selector) {
            fprintf(stderr, "Error: selector '%s' %s.\n",
                    failed_selector,
                    selector_status == SELECTOR_AMBIGUOUS
                        ? "is ambiguous" : "matched nothing");
        } else {
            fprintf(stderr, "Error: an --index selector matched nothing.\n");
        }
        if (selector_status == SELECTOR_ALLOCATION_FAILED) {
            free(selected);
            free(registry_path);
            shader_catalog_dispose(&catalog);
            typetree_schema_registry_dispose(&registry);
            cli_options_dispose(&options);
            return 1;
        }
        if (selector_status == SELECTOR_UNMATCHED &&
            !catalog_inventory_is_complete(&catalog)) {
            if (selected) {
                memset(selected, 0,
                       catalog.record_count * sizeof(*selected));
            }
            selection_blocked_by_incomplete_catalog = true;
        } else {
            free(selected);
            free(registry_path);
            shader_catalog_dispose(&catalog);
            typetree_schema_registry_dispose(&registry);
            cli_options_dispose(&options);
            return 2;
        }
    }

    StringBuilder report;
    sb_init_with_capacity(&report, 16384U);
    int exit_code = selection_blocked_by_incomplete_catalog ? 1 : 0;
    if (options.command == CLI_COMMAND_LIST) {
        bool rendered = options.format == CLI_FORMAT_JSON
            ? render_list_json(&catalog, selected, options.shader_kind,
                               registry_path, registry_digest, &report)
            : render_list_table(&catalog, selected, options.shader_kind,
                                options.show_sources,
                                registry_path, registry_digest, &report);
        if (!rendered) exit_code = 1;
        else if (!catalog_inventory_is_complete(&catalog)) exit_code = 1;
    } else {
        ShaderBatchResult batch;
        shader_batch_result_init(&batch);
        MaterialBatchResult material_batch;
        material_batch_result_init(&material_batch);
        NativeTextureBatchResult texture_batch;
        native_texture_batch_result_init(&texture_batch);
        MaterialBatchStatus material_status = MATERIAL_BATCH_OK;
        NativeTextureBatchStatus texture_status = NATIVE_TEXTURE_BATCH_OK;
        bool texture_batch_pre_material_complete =
            !options.export_materials;

        /* Dependency extraction owns and closes every catalog-retained input
         * snapshot first. Shader extraction then reopens exact sources under
         * its normal digest/identity checks; no generated Shader is exposed
         * provisionally across the texture transaction. */
        if (options.export_materials) {
            texture_status = native_texture_batch_export(
                &catalog, selected, options.output_directory,
                &texture_batch);
            if (texture_status != NATIVE_TEXTURE_BATCH_OK) {
                fprintf(stderr,
                        "Error: native texture export failed: %s.\n",
                        native_texture_batch_status_name(texture_status));
                exit_code = 1;
            }
            if (texture_status == NATIVE_TEXTURE_BATCH_OK &&
                texture_batch.stats.failed != 0U) {
                for (size_t texture_index = 0U;
                     texture_index < texture_batch.record_count;
                     ++texture_index) {
                    const NativeTextureBatchRecordResult* texture =
                        &texture_batch.records[texture_index];
                    if (texture->status != NATIVE_TEXTURE_BATCH_FAILED) {
                        continue;
                    }
                    fprintf(stderr,
                            "Texture dependency failed: source=%zu "
                            "class=%" PRId32 " path=%" PRId64
                            " failure=%s object=%s resource=%s yaml=%s.\n",
                            texture->source_index + 1U, texture->class_id,
                            texture->path_id,
                            native_texture_batch_failure_name(
                                texture->failure),
                            unity_texture_object_status_name(
                                texture->object_status),
                            common_file_status_name(texture->resource_status),
                            unity_native_texture_yaml_status_name(
                                texture->yaml_status));
                }
            }
        }
        ShaderBatchOptions batch_options;
        shader_batch_options_default(&batch_options);
        batch_options.emit_shader_meta = options.export_materials;
        batch_options.flat_graphics_output = options.flat_shaders;
        batch_options.defer_source_snapshot_close = false;
        ShaderBatchStatus batch_status = shader_batch_extract_ex(
            &catalog, selected, &registry, options.output_directory,
            &batch_options, &batch);
        if (batch_status != SHADER_BATCH_OK) {
            fprintf(stderr, "Error: batch extraction failed: %s.\n",
                    shader_batch_status_name(batch_status));
            exit_code = 1;
        } else {
            if (options.export_materials) {
                texture_batch_pre_material_complete =
                    texture_status == NATIVE_TEXTURE_BATCH_OK &&
                    native_texture_batch_is_complete(&texture_batch);
                if (!texture_batch_pre_material_complete) {
                    fprintf(stderr,
                            "Error: native texture publication identity "
                            "changed before Material export.\n");
                }
                MaterialBatchOptions material_options;
                material_batch_options_default(&material_options);
                material_options.dependency_authority_complete =
                    texture_status == NATIVE_TEXTURE_BATCH_OK &&
                    texture_batch_pre_material_complete &&
                    shader_batch_is_complete(&batch);
                material_options.resolve_texture_reference =
                    native_texture_batch_resolve_reference;
                material_options.texture_reference_context = &texture_batch;
                material_options.close_texture_reference_publication_lease =
                    native_texture_batch_close_publication_lease;
                material_status = material_batch_export(
                    &catalog, selected, &batch, options.output_directory,
                    &material_options, &material_batch);
                if (material_status != MATERIAL_BATCH_OK) {
                    fprintf(stderr,
                            "Error: Material export failed: %s.\n",
                            material_batch_status_name(material_status));
                    exit_code = 1;
                }
            }
            const MaterialBatchResult* material_report =
                options.export_materials ? &material_batch : NULL;
            const NativeTextureBatchResult* texture_report =
                options.export_materials ? &texture_batch : NULL;
            bool texture_batch_render_complete = !options.export_materials;
            bool rendered = options.format == CLI_FORMAT_JSON
                ? render_extract_json(&catalog, selected, &batch,
                                      material_report, texture_report,
                                      options.shader_kind,
                                      registry_path,
                                      registry_digest,
                                      &report,
                                      &texture_batch_render_complete)
                : render_extract_table(&catalog, selected, &batch,
                                       material_report, texture_report,
                                       options.shader_kind,
                                       options.show_sources, registry_path,
                                       registry_digest,
                                       &report,
                                       &texture_batch_render_complete);
            const bool texture_batch_exit_complete =
                !options.export_materials ||
                native_texture_batch_is_complete(&texture_batch);
            /* A targeted extraction is complete when every selected record
             * was emitted. Unselected catalog failures remain explicit in
             * the report but do not make that selected operation fail. With
             * --all those records are selected, so the command still fails
             * closed. */
            if (!rendered || catalog.issue_count != 0U ||
                batch.stats.selected == 0U ||
                !shader_batch_is_complete(&batch) ||
                (options.export_materials &&
                 (texture_status != NATIVE_TEXTURE_BATCH_OK ||
                  !texture_batch_render_complete ||
                  !texture_batch_exit_complete ||
                  material_status != MATERIAL_BATCH_OK ||
                  !material_batch_is_complete(&material_batch) ||
                  !material_batch_texture_dependencies_are_closed(
                      &material_batch)))) {
                exit_code = 1;
            }
        }
        native_texture_batch_result_dispose(&texture_batch);
        material_batch_result_dispose(&material_batch);
        shader_batch_result_dispose(&batch);
    }
    if (sb_ok(&report)) {
        if (report.len != 0U && fwrite(
                report.buf, 1U, report.len, stdout) != report.len) {
            exit_code = 1;
        }
        if (options.report_path &&
            !publish_report(options.report_path, &report)) {
            fprintf(stderr, "Error: could not publish report '%s'.\n",
                    options.report_path);
            exit_code = 1;
        }
    } else {
        fprintf(stderr, "Error: out of memory rendering report.\n");
        exit_code = 1;
    }

    sb_free(&report);
    free(selected);
    free(registry_path);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    cli_options_dispose(&options);
    return exit_code;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
    if (argc < 0 || !wide_argv ||
        (size_t)argc > SIZE_MAX / sizeof(char*) - 1U) {
        fputs("Error: invalid Windows command line.\n", stderr);
        return 2;
    }
    char** argv = (char**)calloc((size_t)argc + 1U, sizeof(*argv));
    if (!argv) {
        fputs("Error: out of memory decoding Windows command line.\n", stderr);
        return 1;
    }
    int converted = 0;
    for (; converted < argc; ++converted) {
        argv[converted] = common_windows_wide_to_utf8(wide_argv[converted]);
        if (!argv[converted]) break;
    }
    if (converted != argc) {
        DWORD conversion_error = GetLastError();
        for (int i = 0; i < converted; ++i) free(argv[i]);
        free(argv);
        fputs("Error: Windows command line is not valid Unicode.\n", stderr);
        return conversion_error == ERROR_NOT_ENOUGH_MEMORY ? 1 : 2;
    }
    int result = dxbc_sandbox_main(argc, argv);
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    return result;
}
#else
int main(int argc, char** argv) {
    return dxbc_sandbox_main(argc, argv);
}
#endif
