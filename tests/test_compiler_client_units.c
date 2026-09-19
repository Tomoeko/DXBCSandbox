#include "compiler/unity_compiler_client.h"
#include "compiler/unity_compiler_cache.h"
#include "common/oracle_pack.h"
#include "common/sha256.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
        return 1; \
    } \
} while (0)

#define FAKE_COMPILER_MAGIC 0x0C0BD1E4U
#define FAKE_SESSION_VALID_APIS UINT32_C(0x00048230)
#define FAKE_SESSION_RAW_MASK \
    (FAKE_SESSION_VALID_APIS | ~UNITY_COMPILER_PLATFORM_MASK)

static uint64_t fake_session_features(size_t platform) {
    return UINT64_C(0x1020304050607080) + (uint64_t)platform;
}

static int32_t fake_session_version(size_t platform) {
    return INT32_C(7300) + (int32_t)platform;
}

static int fake_write_all(int fd, const void* data, size_t size) {
    const uint8_t* cursor = (const uint8_t*)data;
    while (size > 0U) {
        ssize_t count = write(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return 1;
}

static int fake_read_all(int fd, void* data, size_t size) {
    uint8_t* cursor = (uint8_t*)data;
    while (size > 0U) {
        ssize_t count = read(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return 1;
}

static int fake_write_u32(int fd, uint32_t value) {
    uint32_t words[2] = {FAKE_COMPILER_MAGIC, value};
    return fake_write_all(fd, words, sizeof(words));
}

static int fake_write_i32(int fd, int32_t value) {
    return fake_write_u32(fd, (uint32_t)value);
}

static int fake_write_u64(int fd, uint64_t value) {
    uint32_t magic = FAKE_COMPILER_MAGIC;
    return fake_write_all(fd, &magic, sizeof(magic)) &&
           fake_write_all(fd, &value, sizeof(value));
}

static int fake_read_u32(int fd, uint32_t* out_value) {
    uint32_t magic = 0;
    return fake_read_all(fd, &magic, sizeof(magic)) &&
           magic == FAKE_COMPILER_MAGIC &&
           fake_read_all(fd, out_value, sizeof(*out_value));
}

static int fake_read_i32(int fd, int32_t* out_value) {
    return fake_read_u32(fd, (uint32_t*)out_value);
}

static int fake_read_u64(int fd, uint64_t* out_value) {
    uint32_t magic = 0;
    return fake_read_all(fd, &magic, sizeof(magic)) &&
           magic == FAKE_COMPILER_MAGIC &&
           fake_read_all(fd, out_value, sizeof(*out_value));
}

static int fake_write_buffer(
    int fd, const void* data, size_t size) {
    uint32_t magic = FAKE_COMPILER_MAGIC;
    uint64_t wire_size = (uint64_t)size;
    return fake_write_all(fd, &magic, sizeof(magic)) &&
           fake_write_all(fd, &wire_size, sizeof(wire_size)) &&
           (size == 0U || fake_write_all(fd, data, size));
}

static int fake_write_string(int fd, const char* text) {
    return fake_write_buffer(fd, text, text ? strlen(text) : 0U);
}

static int fake_read_buffer(int fd, uint8_t** out_data, size_t* out_size) {
    *out_data = NULL;
    *out_size = 0U;
    uint32_t magic = 0;
    uint64_t wire_size = 0;
    if (!fake_read_all(fd, &magic, sizeof(magic)) ||
        magic != FAKE_COMPILER_MAGIC ||
        !fake_read_all(fd, &wire_size, sizeof(wire_size)) ||
        wire_size > 64U * 1024U * 1024U || wire_size > SIZE_MAX - 1U) {
        return 0;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)wire_size + 1U);
    if (!data) return 0;
    if (wire_size > 0U && !fake_read_all(fd, data, (size_t)wire_size)) {
        free(data);
        return 0;
    }
    data[wire_size] = '\0';
    *out_data = data;
    *out_size = (size_t)wire_size;
    return 1;
}

static char* fake_read_string(int fd) {
    uint8_t* data = NULL;
    size_t size = 0U;
    if (!fake_read_buffer(fd, &data, &size)) return NULL;
    return (char*)data;
}

static int fake_read_string_array(int fd) {
    int32_t count = -1;
    if (!fake_read_i32(fd, &count) || count < 0 || count > 1024) return 0;
    for (int32_t index = 0; index < count; index++) {
        char* value = fake_read_string(fd);
        if (!value) return 0;
        free(value);
    }
    return 1;
}

static int fake_consume_initialization(int fd) {
    char* command = fake_read_string(fd);
    if (!command || strcmp(command, "initializeCompiler") != 0) {
        free(command);
        return 0;
    }
    free(command);
    int32_t include_count = -1;
    if (!fake_read_i32(fd, &include_count) || include_count < 1 ||
        include_count > 16) {
        return 0;
    }
    char* includes[16] = {0};
    for (int32_t index = 0; index < include_count; index++) {
        includes[index] = fake_read_string(fd);
        if (!includes[index]) {
            for (int32_t prior = 0; prior < index; prior++) {
                free(includes[prior]);
            }
            return 0;
        }
        for (int32_t prior = 0; prior < index; prior++) {
            if (strcmp(includes[index], includes[prior]) == 0) {
                for (int32_t cleanup = 0; cleanup <= index; cleanup++) {
                    free(includes[cleanup]);
                }
                return 0;
            }
        }
    }
    for (int32_t index = 0; index < include_count; index++) {
        free(includes[index]);
    }
    int32_t reserved = -1;
    char* toolchains = NULL;
    if (!fake_read_i32(fd, &reserved) || reserved != 0 ||
        !(toolchains = fake_read_string(fd))) {
        free(toolchains);
        return 0;
    }
    free(toolchains);
    const char* mode = getenv("DXBC_USC_FAKE_INIT_MODE");
    uint32_t raw_mask = FAKE_SESSION_RAW_MASK;
    if (mode && strcmp(mode, "invalid-mask") == 0) {
        raw_mask = FAKE_SESSION_VALID_APIS;
    } else if (mode && strcmp(mode, "alternate-valid-apis") == 0) {
        raw_mask &= ~(UINT32_C(1) << 4U);
    }
    if (!fake_write_i32(fd, (int32_t)raw_mask)) return 0;
    size_t record_count =
        mode && strcmp(mode, "truncated") == 0
            ? 7U : UNITY_COMPILER_PLATFORM_COUNT;
    for (size_t index = 0; index < record_count; index++) {
        uint64_t features = fake_session_features(index);
        if (mode && strcmp(mode, "changed-record") == 0 && index == 12U) {
            features++;
        }
        if (!fake_write_u64(fd, features) ||
            !fake_write_i32(fd, fake_session_version(index))) {
            return 0;
        }
    }
    return record_count == UNITY_COMPILER_PLATFORM_COUNT;
}

static int fake_consume_preprocess_request(
    int fd, char** out_shader_name, char** out_file_path,
    uint32_t* out_valid_apis) {
    *out_shader_name = NULL;
    *out_file_path = NULL;
    if (out_valid_apis) *out_valid_apis = 0U;
    char* source = fake_read_string(fd);
    *out_file_path = fake_read_string(fd);
    *out_shader_name = fake_read_string(fd);
    uint32_t scalar = 0;
    int ok = source && *out_file_path && *out_shader_name &&
             fake_read_u32(fd, &scalar) && fake_read_u32(fd, &scalar) &&
             fake_read_u32(fd, &scalar) &&
             fake_read_u32(fd, out_valid_apis ? out_valid_apis : &scalar) &&
             fake_read_string_array(fd) && fake_read_string_array(fd);
    free(source);
    return ok;
}

static int fake_consume_compile_request(
    int fd, char** out_source, char** out_directory, char** out_basename) {
    *out_source = NULL;
    *out_directory = NULL;
    *out_basename = NULL;
    *out_source = fake_read_string(fd);
    *out_directory = fake_read_string(fd);
    *out_basename = fake_read_string(fd);
    char* pass_name = fake_read_string(fd);
    uint32_t scalar = 0;
    int32_t signed_scalar = 0;
    uint64_t requirements = 0;
    int ok = *out_source && *out_directory && *out_basename && pass_name &&
             fake_read_u32(fd, &scalar) && fake_read_u32(fd, &scalar) &&
             fake_read_u32(fd, &scalar) && fake_read_u32(fd, &scalar) &&
             fake_read_i32(fd, &signed_scalar) &&
             fake_read_string_array(fd) && fake_read_string_array(fd) &&
             fake_read_string_array(fd) && fake_read_u32(fd, &scalar) &&
             fake_read_i32(fd, &signed_scalar) &&
             fake_read_i32(fd, &signed_scalar) &&
             fake_read_i32(fd, &signed_scalar) &&
             fake_read_u64(fd, &requirements) &&
             fake_read_i32(fd, &signed_scalar) &&
             fake_read_i32(fd, &signed_scalar);
    free(pass_name);
    return ok;
}

static int fake_shader_path_matches(
    const char* shader_name, const char* file_path) {
    static const char assets_prefix[] = "Assets/";
    static const char packages_prefix[] = "Packages/";
    static const char shader_suffix[] = ".shader";
    if (!shader_name || !file_path) return 0;
    if (strncmp(shader_name, assets_prefix,
                sizeof(assets_prefix) - 1U) == 0 ||
        strncmp(shader_name, packages_prefix,
                sizeof(packages_prefix) - 1U) == 0) {
        return strcmp(shader_name, file_path) == 0;
    }
    size_t shader_name_size = strlen(shader_name);
    size_t prefix_size = sizeof(assets_prefix) - 1U;
    size_t suffix_size = sizeof(shader_suffix) - 1U;
    size_t file_path_size = strlen(file_path);
    return file_path_size == prefix_size + shader_name_size + suffix_size &&
           memcmp(file_path, assets_prefix, prefix_size) == 0 &&
           memcmp(file_path + prefix_size, shader_name,
                  shader_name_size) == 0 &&
           memcmp(file_path + prefix_size + shader_name_size,
                  shader_suffix, suffix_size + 1U) == 0;
}

static int run_fake_compiler(
    const char* unity_contents_path, const char* port_text) {
    char* end = NULL;
    long parsed_port = strtol(port_text, &end, 10);
    if (end == port_text || *end != '\0' || parsed_port <= 0 ||
        parsed_port > 65535) {
        return 2;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 3;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)parsed_port);
    while (connect(fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        if (errno == EINTR) continue;
        close(fd);
        return 4;
    }
    char expected_library_path[PATH_MAX * 2U];
    int expected_size = snprintf(
        expected_library_path, sizeof(expected_library_path),
        "%s/Frameworks:%s/Tools", unity_contents_path,
        unity_contents_path);
    const char* tmpdir = getenv("TMPDIR");
    const char* dyld_path = getenv("DYLD_LIBRARY_PATH");
    const char* ld_path = getenv("LD_LIBRARY_PATH");
    bool environment_ok =
        expected_size > 0 && (size_t)expected_size <
                                 sizeof(expected_library_path) &&
        tmpdir && strcmp(tmpdir, "/tmp") == 0 && dyld_path &&
        strcmp(dyld_path, expected_library_path) == 0 && ld_path &&
        strcmp(ld_path, expected_library_path) == 0 &&
        getenv("DYLD_INSERT_LIBRARIES") == NULL &&
        getenv("PROXY_DYLD_INSERT_LIBRARIES") == NULL;
    if (!fake_write_string(
            fd, environment_ok ? "" : "fake environment mismatch")) {
        close(fd);
        return 5;
    }
    if (!environment_ok || !fake_consume_initialization(fd)) {
        close(fd);
        return 5;
    }

    int ignore_shutdown = 0;
    for (;;) {
        char* command = fake_read_string(fd);
        if (!command) break;
        if (strcmp(command, "shutdown") == 0) {
            free(command);
            if (ignore_shutdown) sleep(5);
            break;
        }
        if (strcmp(command, "disassembleShader") == 0) {
            free(command);
            char* name = NULL;
            int stall_write = 0;
            /* The name precedes the payload, so inspect it after the scalar
             * fields and leave the framed payload unread for this fixture. */
            name = fake_read_string(fd);
            int32_t scalar = 0;
            if (!name || !fake_read_i32(fd, &scalar) ||
                !fake_read_i32(fd, &scalar) ||
                !fake_read_i32(fd, &scalar)) {
                free(name);
                break;
            }
            stall_write = strcmp(name, "stall-write") == 0;
            uint8_t* payload = NULL;
            size_t payload_size = 0U;
            if (stall_write) {
                uint32_t magic = 0;
                uint64_t size = 0;
                if (!fake_read_all(fd, &magic, sizeof(magic)) ||
                    magic != FAKE_COMPILER_MAGIC ||
                    !fake_read_all(fd, &size, sizeof(size))) {
                    free(name);
                    break;
                }
                sleep(5);
                free(name);
                break;
            }
            if (!fake_read_buffer(fd, &payload, &payload_size)) {
                free(name);
                break;
            }
            free(payload);
            if (strcmp(name, "stall-read") == 0) {
                sleep(5);
            } else if (strcmp(name, "partial") == 0) {
                uint32_t magic = FAKE_COMPILER_MAGIC;
                (void)fake_write_all(fd, &magic, sizeof(magic));
                sleep(5);
            } else if (strcmp(name, "malformed") == 0) {
                (void)fake_write_string(fd, "disasm: 1 trailing");
                (void)fake_write_string(fd, "must not be accepted");
            } else {
                ignore_shutdown = strcmp(name, "ignore-shutdown") == 0;
                if (strcmp(name, "error-disassemble") == 0 ||
                    strcmp(name, "success-diagnostic-disassemble") == 0 ||
                    strcmp(name, "success-info-disassemble") == 0) {
                    (void)fake_write_string(
                        fd, strcmp(name, "success-info-disassemble") == 0
                                ? "err: 0 -1 0" : "err: 4 5 6");
                    (void)fake_write_string(fd, "Fixture.bin");
                    (void)fake_write_string(
                        fd, strcmp(name, "success-info-disassemble") == 0
                                ? "fixture disassembly note"
                                : "fixture disassembly error");
                }
                (void)fake_write_string(
                    fd, (strcmp(name, "valid-failure") == 0 ||
                         strcmp(name, "error-disassemble") == 0)
                            ? "disasm: 0"
                            : "disasm: 1");
                (void)fake_write_string(fd, "fake disassembly");
            }
            free(name);
            continue;
        }
        if (strcmp(command, "preprocess") == 0) {
            free(command);
            char* name = NULL;
            char* file_path = NULL;
            uint32_t valid_apis = 0U;
            if (!fake_consume_preprocess_request(
                    fd, &name, &file_path, &valid_apis)) {
                free(name);
                free(file_path);
                break;
            }
            int path_matches = fake_shader_path_matches(name, file_path);
            if (strcmp(name, "unknown-preprocess") == 0) {
                (void)fake_write_string(fd, "unknown: 1");
            } else if (strcmp(name, "error-preprocess") == 0) {
                (void)fake_write_string(fd, "err: 1 2 3");
                (void)fake_write_string(fd, "Assets/Fixture.shader");
                (void)fake_write_string(fd, "fixture preprocess error");
            } else if (strcmp(name, "success-diagnostic-preprocess") == 0) {
                (void)fake_write_string(fd, "err: 10 11 12");
                (void)fake_write_string(fd, "Assets/Diagnosed.shader");
                (void)fake_write_string(fd, "fixture preprocess warning");
            } else if (strcmp(name, "success-info-preprocess") == 0) {
                (void)fake_write_string(fd, "err: 0 -1 0");
                (void)fake_write_string(fd, "");
                (void)fake_write_string(fd, "fixture preprocess note");
            }
            const char* status = "shader: 1 0 0";
            if (strcmp(name, "primary-false") == 0) {
                status = "shader: 0 1 0";
            } else if (strcmp(name, "error-preprocess") == 0) {
                status = "shader: 0 0 0";
            } else if (strcmp(name, "malformed-preprocess") == 0) {
                status = "shader: 1 0 0 trailing";
            } else if (strncmp(name, "verify-long-preprocess-", 23U) == 0 &&
                       !path_matches) {
                status = "shader: 0 0 0";
            } else if (strcmp(name, "legacy-session-valid-apis") == 0 &&
                       valid_apis !=
                           (FAKE_SESSION_VALID_APIS &
                            ~(UINT32_C(1) << 4U))) {
                status = "shader: 0 0 0";
            }
            (void)fake_write_string(fd, status);
            (void)fake_write_buffer(fd, NULL, 0U);
            free(name);
            free(file_path);
            continue;
        }
        if (strcmp(command, "compileSnippet") == 0) {
            free(command);
            char* source = NULL;
            char* directory = NULL;
            char* basename = NULL;
            if (!fake_consume_compile_request(
                    fd, &source, &directory, &basename)) {
                free(source);
                free(directory);
                free(basename);
                break;
            }
            /* This source is a wire tripwire.  Authority-gate tests require
             * the client to reject it locally; receiving it poisons the fake
             * stream so a subsequent same-process success assertion fails. */
            if (strcmp(source,
                       "authority-mismatch-must-not-be-sent") == 0) {
                free(source);
                free(directory);
                free(basename);
                break;
            }
            int path_matches = fake_shader_path_matches(basename, directory);
            if (strcmp(source, "reflection-records") == 0) {
                static const char* records[] = {
                    "input: 1 2",
                    "cb: UnityPerDraw 0 64",
                    "const: Value 0 1 2 3 4 5",
                    "cbbind: UnityPerDraw 0",
                    "texbind: MainTex 0 1 2 3",
                    "sampler: 0 1",
                    "bufferbind: Data 1 2",
                    "uavbind: Output 2 3",
                    "stats: 1 2 3 4",
                };
                for (size_t index = 0;
                     index < sizeof(records) / sizeof(records[0]); index++) {
                    (void)fake_write_string(fd, records[index]);
                }
            } else if (strcmp(source, "compile-error-record") == 0) {
                (void)fake_write_string(fd, "err: 7 8 9");
                (void)fake_write_string(fd, "Assets/Fixture.shader");
                (void)fake_write_string(fd, "fixture compile error");
            } else if (strcmp(source, "compile-success-diagnostic") == 0) {
                (void)fake_write_string(fd, "err: 13 14 15");
                (void)fake_write_string(fd, "Assets/Diagnosed.shader");
                (void)fake_write_string(fd, "fixture compile warning");
            } else if (strcmp(source, "compile-success-info") == 0) {
                (void)fake_write_string(fd, "err: 0 -1 0");
                (void)fake_write_string(fd, "");
                (void)fake_write_string(fd, "fixture compile note");
            } else if (strcmp(source, "unknown-structured-record") == 0) {
                (void)fake_write_string(fd, "uav: 0");
            }
            const char* status = "shader: 1";
            if (strcmp(source, "compile-three-status") == 0) {
                status = "shader: 0 1 0";
            } else if (strcmp(source, "compile-error-record") == 0) {
                status = "shader: 0";
            } else if ((strcmp(source, "verify-long-compile-path") == 0 ||
                        strcmp(source, "verify-long-expanded-path") == 0) &&
                       !path_matches) {
                status = "shader: 0";
            }
            static const char expanded[] = "expanded fixture";
            static const uint8_t diagnosed_binary[] = {0x00, 0xff, 0x41};
            (void)fake_write_string(fd, status);
            (void)fake_write_i32(fd, 0);
            (void)fake_write_buffer(
                fd,
                strcmp(source, "compile-success-diagnostic") == 0
                    ? diagnosed_binary
                    : (const uint8_t*)expanded,
                strcmp(source, "compile-success-diagnostic") == 0
                    ? sizeof(diagnosed_binary)
                    : sizeof(expanded) - 1U);
            free(source);
            free(directory);
            free(basename);
            continue;
        }
        free(command);
        break;
    }
    close(fd);
    return 0;
}

static int write_file(const char* path, const void* data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    const uint8_t* cursor = (const uint8_t*)data;
    while (size > 0) {
        ssize_t count = write(fd, cursor, size);
        if (count <= 0) {
            close(fd);
            return 0;
        }
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return close(fd) == 0;
}

static void cache_entry_path(
    const char* cache_dir,
    const uint8_t digest[USC_CACHE_DIGEST_SIZE],
    char path[PATH_MAX]) {
    static const char digits[] = "0123456789abcdef";
    char hex[65];
    for (size_t i = 0; i < USC_CACHE_DIGEST_SIZE; i++) {
        hex[i * 2U] = digits[digest[i] >> 4U];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    hex[64] = '\0';
    (void)snprintf(path, PATH_MAX, "%s/%c%c/%s.usc-cache", cache_dir,
                   hex[0], hex[1], hex + 2);
}

static void remove_tree(const char* path) {
    DIR* directory = opendir(path);
    if (!directory) {
        unlink(path);
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

static void request_digest(
    const UnityCompilerCompileRequest* request,
    const uint8_t fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t digest[USC_CACHE_DIGEST_SIZE]) {
    usc_cache_request_digest(request, fingerprint, digest);
}

static int string_arrays_equal(
    char** left, int left_count, char** right, int right_count) {
    if (left_count != right_count) return 0;
    for (int i = 0; i < left_count; i++) {
        if (strcmp(left[i], right[i]) != 0) return 0;
    }
    return 1;
}

static int variant_sets_equal(
    const SnippetKeywordVariantSet* left,
    const SnippetKeywordVariantSet* right) {
    return left->present == right->present &&
           (!left->present || string_arrays_equal(
               left->combinations, left->combination_count,
               right->combinations, right->combination_count));
}

static int program_variant_arrays_equal(
    const SnippetCompileContract* left,
    const SnippetCompileContract* right) {
    if (left->program_keyword_variant_count !=
        right->program_keyword_variant_count) {
        return 0;
    }
    for (int i = 0; i < left->program_keyword_variant_count; i++) {
        const SnippetProgramKeywordVariants* a =
            &left->program_keyword_variants[i];
        const SnippetProgramKeywordVariants* b =
            &right->program_keyword_variants[i];
        if (a->compiler_program != b->compiler_program ||
            !variant_sets_equal(&a->user_global, &b->user_global) ||
            !variant_sets_equal(&a->user_local, &b->user_local) ||
            !variant_sets_equal(&a->builtin, &b->builtin)) {
            return 0;
        }
    }
    return 1;
}

static int contracts_equal(
    const SnippetCompileContract* left,
    const SnippetCompileContract* right) {
    if (left->snippet_id != right->snippet_id ||
        left->platforms != right->platforms ||
        left->quality_variants != right->quality_variants ||
        left->program_types_mask != right->program_types_mask ||
        left->compilation_flags != right->compilation_flags ||
        left->language != right->language ||
        memcmp(left->source_hash, right->source_hash,
               sizeof(left->source_hash)) != 0 ||
        left->start_line != right->start_line ||
        left->use_dxc_apis != right->use_dxc_apis ||
        left->never_use_dxc_apis != right->never_use_dxc_apis ||
        left->requirements != right->requirements ||
        !program_variant_arrays_equal(left, right) ||
        !string_arrays_equal(
            left->non_stripped_user_keywords,
            left->non_stripped_user_keyword_count,
            right->non_stripped_user_keywords,
            right->non_stripped_user_keyword_count) ||
        !string_arrays_equal(left->builtin_keywords,
                             left->builtin_keyword_count,
                             right->builtin_keywords,
                             right->builtin_keyword_count) ||
        left->conditional_requirement_count !=
            right->conditional_requirement_count) {
        return 0;
    }
    for (int i = 0; i < left->conditional_requirement_count; i++) {
        if (strcmp(left->conditional_requirements[i].keyword,
                   right->conditional_requirements[i].keyword) != 0 ||
            left->conditional_requirements[i].requirements !=
                right->conditional_requirements[i].requirements) {
            return 0;
        }
    }
    return 1;
}

static int preprocess_results_equal(
    const PreprocessResult* left, const PreprocessResult* right) {
    if (left->snippet_count != right->snippet_count ||
        left->blob_len != right->blob_len ||
        (left->blob_len > 0 &&
         memcmp(left->blob, right->blob, left->blob_len) != 0)) {
        return 0;
    }
    for (int i = 0; i < left->snippet_count; i++) {
        const PreprocessedSnippet* a = &left->snippets[i];
        const PreprocessedSnippet* b = &right->snippets[i];
        if (strcmp(a->source, b->source) != 0 || a->reqs != b->reqs ||
            a->language != b->language ||
            a->gpu_program_id != b->gpu_program_id ||
            a->has_contract != b->has_contract ||
            (a->has_contract &&
             !contracts_equal(&a->contract, &b->contract)) ||
            a->conditional_requirement_count !=
                b->conditional_requirement_count) {
            return 0;
        }
        for (int j = 0; j < a->conditional_requirement_count; j++) {
            if (strcmp(a->conditional_requirements[j].keyword,
                       b->conditional_requirements[j].keyword) != 0 ||
                a->conditional_requirements[j].requirements !=
                    b->conditional_requirements[j].requirements) {
                return 0;
            }
        }
    }
    return 1;
}

static char* make_long_shader_name(const char* prefix, size_t size) {
    size_t prefix_size = strlen(prefix);
    if (prefix_size >= size || size == SIZE_MAX) return NULL;
    char* name = (char*)malloc(size + 1U);
    if (!name) return NULL;
    memcpy(name, prefix, prefix_size);
    memset(name + prefix_size, 'x', size - prefix_size);
    name[size] = '\0';
    return name;
}

int main(int argc, char** argv) {
    if (argc == 5 && getenv("DXBC_USC_FAKE_SERVER")) {
        return run_fake_compiler(argv[1], argv[3]);
    }
    UnityCompilerPreprocessStatus preprocess_status;
    CHECK(unity_compiler_parse_preprocess_status_record(
        "shader: 1 0 1", &preprocess_status));
    CHECK(preprocess_status.primary_success);
    CHECK(!preprocess_status.secondary_status);
    CHECK(preprocess_status.tertiary_status);
    CHECK(unity_compiler_parse_preprocess_status_record(
        "shader: 0 1 0", &preprocess_status));
    CHECK(!preprocess_status.primary_success);
    CHECK(preprocess_status.secondary_status);
    CHECK(!preprocess_status.tertiary_status);
    CHECK(!unity_compiler_parse_preprocess_status_record(
        "shader: 1 0", &preprocess_status));
    CHECK(!unity_compiler_parse_preprocess_status_record(
        "shader: 1 0 0 trailing", &preprocess_status));
    CHECK(!unity_compiler_parse_preprocess_status_record(
        "shader: 1  0 0", &preprocess_status));
    CHECK(!unity_compiler_parse_preprocess_status_record(
        "shader: 2 0 0", &preprocess_status));

    bool command_success = false;
    CHECK(unity_compiler_parse_compile_status_record(
        "shader: 1", &command_success));
    CHECK(command_success);
    CHECK(unity_compiler_parse_compile_status_record(
        "shader: 0", &command_success));
    CHECK(!command_success);
    CHECK(!unity_compiler_parse_compile_status_record(
        "shader: 0 1 0", &command_success));
    CHECK(!unity_compiler_parse_compile_status_record(
        "shader: 1 trailing", &command_success));
    CHECK(!unity_compiler_parse_compile_status_record(
        "shader:  1", &command_success));

    CHECK(unity_compiler_parse_disassemble_status_record(
        "disasm: 1", &command_success));
    CHECK(command_success);
    CHECK(unity_compiler_parse_disassemble_status_record(
        "disasm: 0", &command_success));
    CHECK(!command_success);
    CHECK(!unity_compiler_parse_disassemble_status_record(
        "disasm: 1 trailing", &command_success));
    CHECK(!unity_compiler_parse_disassemble_status_record(
        "shader: 1", &command_success));

    UnityCompilerSessionCapabilities capability_fixture;
    memset(&capability_fixture, 0, sizeof(capability_fixture));
    capability_fixture.raw_available_platform_mask = FAKE_SESSION_RAW_MASK;
    for (size_t index = 0; index < UNITY_COMPILER_PLATFORM_COUNT; index++) {
        capability_fixture.platforms[index].supported_features =
            fake_session_features(index);
        capability_fixture.platforms[index].version =
            fake_session_version(index);
    }
    CHECK(unity_compiler_session_capabilities_validate(&capability_fixture));
    uint32_t session_valid_apis = 0U;
    CHECK(unity_compiler_session_capabilities_valid_apis(
        &capability_fixture, &session_valid_apis));
    CHECK(session_valid_apis == FAKE_SESSION_VALID_APIS);
    CHECK(unity_compiler_session_capabilities_support_valid_apis(
        &capability_fixture, UINT32_C(1) << 4U));
    CHECK(unity_compiler_session_capabilities_support_valid_apis(
        &capability_fixture, FAKE_SESSION_VALID_APIS));
    CHECK(!unity_compiler_session_capabilities_support_valid_apis(
        &capability_fixture, UINT32_C(1)));
    CHECK(!unity_compiler_session_capabilities_support_valid_apis(
        &capability_fixture, UINT32_C(1) << UNITY_COMPILER_PLATFORM_COUNT));
    CHECK(unity_compiler_session_capabilities_match_valid_apis(
        &capability_fixture, FAKE_SESSION_VALID_APIS));
    CHECK(!unity_compiler_session_capabilities_match_valid_apis(
        &capability_fixture,
        FAKE_SESSION_VALID_APIS & ~(UINT32_C(1) << 4U)));
    CHECK(!unity_compiler_session_capabilities_match_valid_apis(
        &capability_fixture,
        FAKE_SESSION_VALID_APIS | UINT32_C(1)));
    CHECK(!unity_compiler_session_capabilities_match_valid_apis(
        &capability_fixture,
        FAKE_SESSION_VALID_APIS |
            (UINT32_C(1) << UNITY_COMPILER_PLATFORM_COUNT)));
    char* session_json = unity_compiler_session_capabilities_format_json(
        &capability_fixture);
    CHECK(session_json != NULL);
    CHECK(strstr(session_json,
                 "\"schema\":\"dxbc-unity-compiler-session-v1\"") !=
          NULL);
    CHECK(strstr(session_json,
                 "\"raw_available_platform_mask\":") != NULL);
    CHECK(strstr(session_json,
                 "\"valid_apis\":295472") != NULL);
    CHECK(strstr(session_json,
                 "\"platform\":0,\"supported_features\":"
                 "1161981756646125696,\"version\":7300") != NULL);
    CHECK(strstr(session_json,
                 "\"platform\":24,\"supported_features\":"
                 "1161981756646125720,\"version\":7324") != NULL);
    size_t reported_platform_count = 0U;
    const char* platform_cursor = session_json;
    while ((platform_cursor = strstr(platform_cursor, "\"platform\":")) !=
           NULL) {
        reported_platform_count++;
        platform_cursor += sizeof("\"platform\":") - 1U;
    }
    CHECK(reported_platform_count == UNITY_COMPILER_PLATFORM_COUNT);
    free(session_json);
    UnityCompilerSessionCapabilities changed_capabilities =
        capability_fixture;
    CHECK(unity_compiler_session_capabilities_equal(
        &capability_fixture, &changed_capabilities));
    changed_capabilities.platforms[24].supported_features++;
    CHECK(!unity_compiler_session_capabilities_equal(
        &capability_fixture, &changed_capabilities));
    changed_capabilities = capability_fixture;
    changed_capabilities.platforms[0].version++;
    CHECK(!unity_compiler_session_capabilities_equal(
        &capability_fixture, &changed_capabilities));
    changed_capabilities = capability_fixture;
    changed_capabilities.raw_available_platform_mask &=
        UNITY_COMPILER_PLATFORM_MASK;
    CHECK(!unity_compiler_session_capabilities_validate(
        &changed_capabilities));
    CHECK(!unity_compiler_session_capabilities_valid_apis(
        &changed_capabilities, &session_valid_apis));
    CHECK(!unity_compiler_session_capabilities_validate(NULL));
    CHECK(!unity_compiler_session_capabilities_valid_apis(
        &capability_fixture, NULL));

    static const struct {
        const char* text;
        UnityCompilerReflectionKind kind;
        const char* name;
        size_t value_count;
    } reflection_cases[] = {
        {"input: 1 2", UNITY_COMPILER_REFLECTION_INPUT, NULL, 2U},
        {"cb: UnityPerDraw 0 64",
         UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER, "UnityPerDraw", 2U},
        {"const: Value 0 1 2 3 4 5",
         UNITY_COMPILER_REFLECTION_CONSTANT, "Value", 6U},
        {"cbbind: UnityPerDraw 0",
         UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER_BINDING,
         "UnityPerDraw", 1U},
        {"texbind: MainTex 0 1 2 3",
         UNITY_COMPILER_REFLECTION_TEXTURE_BINDING, "MainTex", 4U},
        {"sampler: 0 1", UNITY_COMPILER_REFLECTION_SAMPLER, NULL, 2U},
        {"bufferbind: Data 1 2",
         UNITY_COMPILER_REFLECTION_BUFFER_BINDING, "Data", 2U},
        {"uavbind: Output 2 3",
         UNITY_COMPILER_REFLECTION_UAV_BINDING, "Output", 2U},
        {"stats: 1 2 3 4", UNITY_COMPILER_REFLECTION_STATS, NULL, 4U},
    };
    for (size_t index = 0U;
         index < sizeof(reflection_cases) / sizeof(reflection_cases[0]);
         ++index) {
        UnityCompilerReflectionRecord record;
        CHECK(unity_compiler_reflection_record_parse(
            reflection_cases[index].text, &record));
        CHECK(record.kind == reflection_cases[index].kind &&
              record.value_count == reflection_cases[index].value_count &&
              strcmp(record.record, reflection_cases[index].text) == 0);
        CHECK((!record.name && !reflection_cases[index].name) ||
              (record.name && reflection_cases[index].name &&
               strcmp(record.name, reflection_cases[index].name) == 0));
        unity_compiler_reflection_record_free(&record);
    }
    UnityCompilerReflectionRecord malformed_reflection;
    CHECK(!unity_compiler_reflection_record_parse(
        "const: Value 0 1 2 3 4", &malformed_reflection));
    CHECK(!unity_compiler_reflection_record_parse(
        "texbind: Main Tex 0 1 2 3", &malformed_reflection));
    CHECK(!unity_compiler_reflection_record_parse(
        "stats: 01 2 3 4", &malformed_reflection));
    CHECK(!unity_compiler_reflection_record_parse(
        "unknown: 1", &malformed_reflection));

    UnityCompilerDiagnostic informational_diagnostic = {
        .fields = {0, -1, 0},
    };
    UnityCompilerResponseStatus informational_status = {
        .compiler_success = true,
        .diagnostics = &informational_diagnostic,
        .diagnostic_count = 1U,
    };
    CHECK(!unity_compiler_diagnostic_is_actionable(
        &informational_diagnostic));
    CHECK(unity_compiler_response_status_actionable_diagnostic_count(
              &informational_status) == 0U);
    CHECK(unity_compiler_response_status_is_clean_success(
        &informational_status));
    informational_diagnostic.fields[0] = 1;
    CHECK(unity_compiler_diagnostic_is_actionable(
        &informational_diagnostic));
    CHECK(unity_compiler_response_status_actionable_diagnostic_count(
              &informational_status) == 1U);
    CHECK(!unity_compiler_response_status_is_clean_success(
        &informational_status));
    informational_diagnostic.fields[0] = -1;
    CHECK(unity_compiler_diagnostic_is_actionable(
        &informational_diagnostic));

    static const char captured_header[] =
        "snip: 35897 -1 0 3 0 3 1811220761 589760252 "
        "-1259843129 -1955402139 18 0 0";
    SnippetCompileContract parsed_contract;
    unity_compiler_snippet_contract_init(&parsed_contract);
    CHECK(unity_compiler_snippet_contract_parse_header(
        captured_header, &parsed_contract));
    CHECK(parsed_contract.snippet_id == 35897);
    CHECK(parsed_contract.platforms == -1);
    CHECK(parsed_contract.quality_variants == 0);
    CHECK(parsed_contract.program_types_mask == 3U);
    CHECK(parsed_contract.compilation_flags == 0U);
    CHECK(parsed_contract.language == 3);
    CHECK(parsed_contract.source_hash[0] == 1811220761U);
    CHECK(parsed_contract.source_hash[1] == 589760252U);
    CHECK(parsed_contract.source_hash[2] == (uint32_t)-1259843129);
    CHECK(parsed_contract.source_hash[3] == (uint32_t)-1955402139);
    CHECK(parsed_contract.start_line == 18);
    CHECK(parsed_contract.use_dxc_apis == 0);
    CHECK(parsed_contract.never_use_dxc_apis == 0);
    const char* user_global_variants[] = {"", "FOO", "FOO BAR"};
    const char* user_local_variants[] = {"", "LOCAL_A LOCAL_B"};
    const char* builtin_variants[] = {"", "BUILTIN_A", "BUILTIN_B"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &parsed_contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        user_global_variants, 3));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &parsed_contract, 0, UNITY_KEYWORD_VARIANTS_USER_LOCAL,
        user_local_variants, 2));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &parsed_contract, 0, UNITY_KEYWORD_VARIANTS_BUILTIN,
        builtin_variants, 3));
    const char* fragment_user_global_variants[] = {"", "FRAGMENT_ONLY"};
    const char* fragment_user_local_variants[] = {""};
    const char* fragment_builtin_variants[] = {"", "FOG_LINEAR"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &parsed_contract, 1, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        fragment_user_global_variants, 2));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &parsed_contract, 1, UNITY_KEYWORD_VARIANTS_USER_LOCAL,
        fragment_user_local_variants, 1));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &parsed_contract, 1, UNITY_KEYWORD_VARIANTS_BUILTIN,
        fragment_builtin_variants, 2));
    CHECK(unity_compiler_snippet_contract_has_variant_families(
        &parsed_contract, 0));
    CHECK(unity_compiler_snippet_contract_has_variant_families(
        &parsed_contract, 1));
    CHECK(!unity_compiler_snippet_contract_has_variant_families(
        &parsed_contract, 4));
    CHECK(unity_compiler_snippet_contract_set_keyword_lines(
        &parsed_contract, "FOO BAR FOO", "DISABLED_A DISABLED_B"));
    CHECK(parsed_contract.non_stripped_user_keyword_count == 3);
    CHECK(strcmp(parsed_contract.non_stripped_user_keywords[0], "FOO") == 0);
    CHECK(strcmp(parsed_contract.non_stripped_user_keywords[1], "BAR") == 0);
    CHECK(strcmp(parsed_contract.non_stripped_user_keywords[2], "FOO") == 0);
    CHECK(parsed_contract.builtin_keyword_count == 2);
    parsed_contract.requirements = 0x123456789abcdef0ULL;
    parsed_contract.conditional_requirement_count = 2;
    parsed_contract.conditional_requirements =
        (ConditionalShaderRequirement*)calloc(
            2, sizeof(*parsed_contract.conditional_requirements));
    CHECK(parsed_contract.conditional_requirements != NULL);
    parsed_contract.conditional_requirements[0].keyword = strdup("COND_A");
    parsed_contract.conditional_requirements[0].requirements = 0x100;
    parsed_contract.conditional_requirements[1].keyword = strdup("COND_B");
    parsed_contract.conditional_requirements[1].requirements = 0x200;
    CHECK(parsed_contract.conditional_requirements[0].keyword != NULL);
    CHECK(parsed_contract.conditional_requirements[1].keyword != NULL);
    CHECK(unity_compiler_snippet_contract_validate(&parsed_contract));

    static const char captured_2021_3_29_header[] =
        "snip: 35897 -1 0 3 0 3 1811220761 589760252 "
        "-1259843129 -1955402139 18";
    SnippetCompileContract parsed_2021_3_29_contract;
    unity_compiler_snippet_contract_init(&parsed_2021_3_29_contract);
    CHECK(unity_compiler_snippet_contract_parse_header(
        captured_2021_3_29_header, &parsed_2021_3_29_contract));
    CHECK(parsed_2021_3_29_contract.snippet_id == 35897);
    CHECK(parsed_2021_3_29_contract.platforms == -1);
    CHECK(parsed_2021_3_29_contract.program_types_mask == 3U);
    CHECK(parsed_2021_3_29_contract.start_line == 18);
    CHECK(parsed_2021_3_29_contract.use_dxc_apis == 0);
    CHECK(parsed_2021_3_29_contract.never_use_dxc_apis == 0);
    unity_compiler_snippet_contract_free(&parsed_2021_3_29_contract);

    SnippetCompileContract copied_contract;
    unity_compiler_snippet_contract_init(&copied_contract);
    CHECK(unity_compiler_snippet_contract_copy(
        &copied_contract, &parsed_contract));
    CHECK(contracts_equal(&copied_contract, &parsed_contract));
    copied_contract.non_stripped_user_keywords[0][0] = 'X';
    copied_contract.program_keyword_variants[0]
        .user_global.combinations[1][0] = 'X';
    copied_contract.conditional_requirements[0].keyword[0] = 'X';
    CHECK(strcmp(parsed_contract.non_stripped_user_keywords[0], "FOO") == 0);
    CHECK(strcmp(parsed_contract.program_keyword_variants[0]
                     .user_global.combinations[1],
                 "FOO") == 0);
    CHECK(strcmp(parsed_contract.conditional_requirements[0].keyword,
                 "COND_A") == 0);
    unity_compiler_snippet_contract_free(&copied_contract);

    SnippetCompileContract rejected_contract;
    unity_compiler_snippet_contract_init(&rejected_contract);
    CHECK(!unity_compiler_snippet_contract_parse_header(
        "snip: 1 -1 0 3 0 3 1 2 3 4 18 0", &rejected_contract));
    CHECK(!unity_compiler_snippet_contract_parse_header(
        "snip: 1 -1 0 3 0 3 1 2 3 4 18 0 0 trailing",
        &rejected_contract));
    CHECK(!unity_compiler_snippet_contract_parse_header(
        "snip: 2147483648 -1 0 3 0 3 1 2 3 4 18 0 0",
        &rejected_contract));
    CHECK(!unity_compiler_snippet_contract_parse_header(
        "snip: 1 -1 0 3 0 3 1 2 3 4 18 2 0",
        &rejected_contract));
    CHECK(unity_compiler_snippet_contract_parse_header(
        "snip: 1 -1 0 3 0 3 1 2 3 4 18 1 1",
        &rejected_contract));
    CHECK(unity_compiler_snippet_contract_validate(&rejected_contract));
    rejected_contract.start_line = -1;
    CHECK(!unity_compiler_snippet_contract_validate(&rejected_contract));
    unity_compiler_snippet_contract_free(&rejected_contract);

    ConditionalShaderRequirement conditional[] = {
        {"PROCEDURAL_INSTANCING_ON", 0x40},
        {"SOFTPARTICLES_ON", 0x100},
    };
    PreprocessedSnippet snippet = {
        .reqs = 0x2,
        .conditional_requirements = conditional,
        .conditional_requirement_count = 2,
    };

    char* procedural[] = {"PROCEDURAL_INSTANCING_ON"};
    CHECK(unity_compiler_variant_requirements(&snippet, procedural, 1) ==
          0x42);

    char* both[] = {"SOFTPARTICLES_ON", "PROCEDURAL_INSTANCING_ON"};
    CHECK(unity_compiler_variant_requirements(&snippet, both, 2) == 0x142);
    CHECK(unity_compiler_variant_requirements(&snippet, NULL, 0) == 0x2);

    char temporary_template[] = "/tmp/dxbc_usc_cache_units.XXXXXX";
    char* temporary_dir = mkdtemp(temporary_template);
    CHECK(temporary_dir != NULL);

    char fingerprint_path[PATH_MAX];
    CHECK(snprintf(fingerprint_path, sizeof(fingerprint_path), "%s/compiler",
                   temporary_dir) > 0);
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t abc_sha256[USC_CACHE_DIGEST_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE];
    CHECK(write_file(fingerprint_path, abc, sizeof(abc)));
    CHECK(usc_cache_sha256_file(fingerprint_path, compiler_fingerprint));
    CHECK(memcmp(compiler_fingerprint, abc_sha256,
                 USC_CACHE_DIGEST_SIZE) == 0);
    static const char multi_block_input[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t multi_block_sha256[USC_CACHE_DIGEST_SIZE] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
    };
    uint8_t multi_block_fingerprint[USC_CACHE_DIGEST_SIZE];
    CHECK(write_file(fingerprint_path, multi_block_input,
                     sizeof(multi_block_input) - 1U));
    CHECK(usc_cache_sha256_file(fingerprint_path, multi_block_fingerprint));
    CHECK(memcmp(multi_block_fingerprint, multi_block_sha256,
                 USC_CACHE_DIGEST_SIZE) == 0);

    char explicit_includes[PATH_MAX];
    char sandbox_includes[PATH_MAX];
    char builtin_includes[PATH_MAX];
    char proxy_path[PATH_MAX];
    char glslang_path[PATH_MAX];
    char dxcompiler_path[PATH_MAX];
    CHECK(snprintf(explicit_includes, sizeof(explicit_includes),
                   "%s/explicit", temporary_dir) > 0);
    CHECK(snprintf(sandbox_includes, sizeof(sandbox_includes),
                   "%s/sandbox", temporary_dir) > 0);
    CHECK(snprintf(builtin_includes, sizeof(builtin_includes),
                   "%s/builtin", temporary_dir) > 0);
    CHECK(snprintf(proxy_path, sizeof(proxy_path), "%s/proxy.dylib",
                   temporary_dir) > 0);
    CHECK(snprintf(glslang_path, sizeof(glslang_path), "%s/glslang.dylib",
                   temporary_dir) > 0);
    CHECK(snprintf(dxcompiler_path, sizeof(dxcompiler_path),
                   "%s/libdxcompiler.dylib", temporary_dir) > 0);
    CHECK(mkdir(explicit_includes, 0700) == 0);
    CHECK(mkdir(sandbox_includes, 0700) == 0);
    CHECK(mkdir(builtin_includes, 0700) == 0);
    char explicit_file[PATH_MAX];
    char sandbox_file[PATH_MAX];
    char builtin_file[PATH_MAX];
    CHECK(snprintf(explicit_file, sizeof(explicit_file), "%s/no_extension",
                   explicit_includes) > 0);
    CHECK(snprintf(sandbox_file, sizeof(sandbox_file), "%s/library.hlsl",
                   sandbox_includes) > 0);
    CHECK(snprintf(builtin_file, sizeof(builtin_file), "%s/Unity.cginc",
                   builtin_includes) > 0);
    CHECK(write_file(explicit_file, "explicit-a", 10));
    CHECK(write_file(sandbox_file, "sandbox-a", 9));
    CHECK(write_file(builtin_file, "builtin-a", 9));
    CHECK(write_file(glslang_path, "glslang-a", 9));
    CHECK(write_file(dxcompiler_path, "dxcompiler-a", 12));
    uint8_t environment_digest_a[USC_CACHE_DIGEST_SIZE];
    uint8_t environment_digest_b[USC_CACHE_DIGEST_SIZE];
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir,
        environment_digest_a));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir,
        environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) == 0);
    CHECK(write_file(sandbox_file, "sandbox-b", 9));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir,
        environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) != 0);
    memcpy(environment_digest_a, environment_digest_b,
           USC_CACHE_DIGEST_SIZE);
    CHECK(write_file(explicit_file, "explicit-b", 10));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir,
        environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) != 0);
    memcpy(environment_digest_a, environment_digest_b,
           USC_CACHE_DIGEST_SIZE);
    CHECK(write_file(proxy_path, "proxy", 5));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir,
        environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) != 0);
    memcpy(environment_digest_a, environment_digest_b,
           USC_CACHE_DIGEST_SIZE);
    CHECK(write_file(glslang_path, "glslang-b", 9));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir,
        environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) != 0);
    memcpy(environment_digest_a, environment_digest_b,
           USC_CACHE_DIGEST_SIZE);
    CHECK(write_file(dxcompiler_path, "dxcompiler-b", 12));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir,
        environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) != 0);

    memcpy(environment_digest_a, environment_digest_b,
           USC_CACHE_DIGEST_SIZE);
    char playback_engine[PATH_MAX];
    char playback_plugins[PATH_MAX];
    char playback_plugin[PATH_MAX];
    CHECK(snprintf(playback_engine, sizeof(playback_engine), "%s/EngineA",
                   temporary_dir) > 0);
    CHECK(snprintf(playback_plugins, sizeof(playback_plugins),
                   "%s/CgBatchPlugins64", playback_engine) > 0);
    CHECK(snprintf(playback_plugin, sizeof(playback_plugin), "%s/plugin.dylib",
                   playback_plugins) > 0);
    CHECK(mkdir(playback_engine, 0700) == 0);
    CHECK(mkdir(playback_plugins, 0700) == 0);
    CHECK(write_file(playback_plugin, "plugin-a", 8));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir, environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) != 0);
    memcpy(environment_digest_a, environment_digest_b,
           USC_CACHE_DIGEST_SIZE);
    CHECK(write_file(playback_plugin, "plugin-b", 8));
    CHECK(usc_cache_environment_fingerprint(
        temporary_dir, explicit_includes, sandbox_includes,
        builtin_includes, proxy_path, glslang_path, dxcompiler_path,
        temporary_dir, temporary_dir, environment_digest_b));
    CHECK(memcmp(environment_digest_a, environment_digest_b,
                 USC_CACHE_DIGEST_SIZE) != 0);

    char* request_keywords[] = {"KEY_A", "KEY_B"};
    char* request_user_keywords[] = {"USER_A", "USER_B"};
    char* request_disabled_keywords[] = {"DEF_A=1", "DEF_B=2"};
    UnityCompilerCompileRequest request = {
        .command = "compileSnippet",
        .toolchain_configuration = "Local",
        .snippet_source = "float4 frag() : SV_Target { return 1; }",
        .source_directory = "Assets",
        .source_basename = "Test.shader",
        .pass_name = "Forward",
        .caching_preprocessor = true,
        .preprocess_only = false,
        .strip_line_directives = false,
        .build_platform = 1,
        .render_state_length = 0,
        .variant_keywords = request_keywords,
        .variant_keyword_count = 2,
        .user_keywords = request_user_keywords,
        .user_keyword_count = 2,
        .disabled_keywords = request_disabled_keywords,
        .disabled_keyword_count = 2,
        .compiler_flags = 0x9000,
        .language = 0,
        .shader_type = 1,
        .platform = 4,
        .requirements = 0x123456789abcdef0ULL,
        .program_mask = 6,
        .program_start = 0,
        .snippet_contract = &parsed_contract,
    };
    uint8_t baseline_digest[USC_CACHE_DIGEST_SIZE];
    uint8_t changed_digest[USC_CACHE_DIGEST_SIZE];
    request_digest(&request, compiler_fingerprint, baseline_digest);
    request_digest(&request, compiler_fingerprint, changed_digest);
    CHECK(memcmp(baseline_digest, changed_digest,
                 USC_CACHE_DIGEST_SIZE) == 0);
    uint8_t* request_transcript = NULL;
    size_t request_transcript_size = 0;
    CHECK(usc_cache_serialize_compile_request(
        &request, compiler_fingerprint, &request_transcript,
        &request_transcript_size));
    CHECK(request_transcript != NULL && request_transcript_size > 0);
    uint8_t transcript_digest[USC_CACHE_DIGEST_SIZE];
    common_sha256(request_transcript, request_transcript_size,
                  transcript_digest);
    CHECK(memcmp(baseline_digest, transcript_digest,
                 USC_CACHE_DIGEST_SIZE) == 0);
    uint8_t* second_transcript = NULL;
    size_t second_transcript_size = 0;
    CHECK(usc_cache_serialize_compile_request(
        &request, compiler_fingerprint, &second_transcript,
        &second_transcript_size));
    CHECK(second_transcript_size == request_transcript_size);
    CHECK(memcmp(second_transcript, request_transcript,
                 request_transcript_size) == 0);
    free(second_transcript);
    free(request_transcript);

#define CHECK_REQUEST_CHANGE(modification) do { \
    UnityCompilerCompileRequest changed = request; \
    modification; \
    request_digest(&changed, compiler_fingerprint, changed_digest); \
    CHECK(memcmp(baseline_digest, changed_digest, \
                 USC_CACHE_DIGEST_SIZE) != 0); \
} while (0)
    CHECK_REQUEST_CHANGE(changed.command = "compileSnippet2");
    CHECK_REQUEST_CHANGE(changed.toolchain_configuration = "Remote");
    CHECK_REQUEST_CHANGE(changed.snippet_source = "different snippet");
    CHECK_REQUEST_CHANGE(changed.source_directory = "Packages");
    CHECK_REQUEST_CHANGE(changed.source_basename = "Other.shader");
    CHECK_REQUEST_CHANGE(changed.pass_name = "ShadowCaster");
    CHECK_REQUEST_CHANGE(changed.caching_preprocessor = false);
    CHECK_REQUEST_CHANGE(changed.preprocess_only = true);
    CHECK_REQUEST_CHANGE(changed.strip_line_directives = true);
    CHECK_REQUEST_CHANGE(changed.build_platform = 2);
    CHECK_REQUEST_CHANGE(changed.render_state_length = 1);
    char* reversed_keywords[] = {"KEY_B", "KEY_A"};
    CHECK_REQUEST_CHANGE(changed.variant_keywords = reversed_keywords);
    CHECK_REQUEST_CHANGE(changed.variant_keyword_count = 1);
    char* reversed_user_keywords[] = {"USER_B", "USER_A"};
    CHECK_REQUEST_CHANGE(changed.user_keywords = reversed_user_keywords);
    CHECK_REQUEST_CHANGE(changed.user_keyword_count = 1);
    char* reversed_disabled_keywords[] = {"DEF_B=2", "DEF_A=1"};
    CHECK_REQUEST_CHANGE(
        changed.disabled_keywords = reversed_disabled_keywords);
    CHECK_REQUEST_CHANGE(changed.disabled_keyword_count = 1);
    CHECK_REQUEST_CHANGE(changed.compiler_flags = 0);
    CHECK_REQUEST_CHANGE(changed.language = 1);
    CHECK_REQUEST_CHANGE(changed.shader_type = 0);
    CHECK_REQUEST_CHANGE(changed.platform = 15);
    CHECK_REQUEST_CHANGE(changed.requirements++);
    CHECK_REQUEST_CHANGE(changed.program_mask = 7);
    CHECK_REQUEST_CHANGE(changed.program_start = 1);
    CHECK_REQUEST_CHANGE(changed.snippet_contract = NULL);
#undef CHECK_REQUEST_CHANGE

#define CHECK_CONTRACT_CHANGE(modification) do { \
    SnippetCompileContract changed_contract = parsed_contract; \
    modification; \
    UnityCompilerCompileRequest changed = request; \
    changed.snippet_contract = &changed_contract; \
    request_digest(&changed, compiler_fingerprint, changed_digest); \
    CHECK(memcmp(baseline_digest, changed_digest, \
                 USC_CACHE_DIGEST_SIZE) != 0); \
} while (0)
    CHECK_CONTRACT_CHANGE(changed_contract.snippet_id++);
    CHECK_CONTRACT_CHANGE(changed_contract.platforms++);
    CHECK_CONTRACT_CHANGE(changed_contract.quality_variants++);
    CHECK_CONTRACT_CHANGE(changed_contract.program_types_mask++);
    CHECK_CONTRACT_CHANGE(changed_contract.compilation_flags++);
    CHECK_CONTRACT_CHANGE(changed_contract.language++);
    CHECK_CONTRACT_CHANGE(changed_contract.source_hash[0]++);
    CHECK_CONTRACT_CHANGE(changed_contract.source_hash[1]++);
    CHECK_CONTRACT_CHANGE(changed_contract.source_hash[2]++);
    CHECK_CONTRACT_CHANGE(changed_contract.source_hash[3]++);
    CHECK_CONTRACT_CHANGE(changed_contract.start_line++);
    CHECK_CONTRACT_CHANGE(changed_contract.use_dxc_apis = 1);
    CHECK_CONTRACT_CHANGE(changed_contract.never_use_dxc_apis = 1);
    CHECK_CONTRACT_CHANGE(changed_contract.program_keyword_variant_count--);
    CHECK_CONTRACT_CHANGE(changed_contract.non_stripped_user_keyword_count--);
    CHECK_CONTRACT_CHANGE(changed_contract.builtin_keyword_count--);
    CHECK_CONTRACT_CHANGE(changed_contract.requirements++);
    CHECK_CONTRACT_CHANGE(changed_contract.conditional_requirement_count--);
    char* reordered_contract_enabled[] = {"BAR", "FOO", "FOO"};
    CHECK_CONTRACT_CHANGE(
        changed_contract.non_stripped_user_keywords =
            reordered_contract_enabled);
    char* reordered_contract_disabled[] = {"DISABLED_B", "DISABLED_A"};
    CHECK_CONTRACT_CHANGE(
        changed_contract.builtin_keywords = reordered_contract_disabled);
    ConditionalShaderRequirement reordered_contract_conditionals[] = {
        {"COND_B", 0x200},
        {"COND_A", 0x100},
    };
    CHECK_CONTRACT_CHANGE(
        changed_contract.conditional_requirements =
            reordered_contract_conditionals);
#undef CHECK_CONTRACT_CHANGE

#define CHECK_PROGRAM_CONTRACT_CHANGE(modification) do { \
    SnippetCompileContract changed_contract = parsed_contract; \
    SnippetProgramKeywordVariants changed_programs[2]; \
    CHECK(parsed_contract.program_keyword_variant_count == 2); \
    memcpy(changed_programs, parsed_contract.program_keyword_variants, \
           sizeof(changed_programs)); \
    changed_contract.program_keyword_variants = changed_programs; \
    modification; \
    UnityCompilerCompileRequest changed = request; \
    changed.snippet_contract = &changed_contract; \
    request_digest(&changed, compiler_fingerprint, changed_digest); \
    CHECK(memcmp(baseline_digest, changed_digest, \
                 USC_CACHE_DIGEST_SIZE) != 0); \
} while (0)
    CHECK_PROGRAM_CONTRACT_CHANGE(
        changed_programs[0].user_global.combination_count--);
    CHECK_PROGRAM_CONTRACT_CHANGE(
        changed_programs[0].user_local.combination_count--);
    CHECK_PROGRAM_CONTRACT_CHANGE(
        changed_programs[0].builtin.combination_count--);
    CHECK_PROGRAM_CONTRACT_CHANGE(
        changed_programs[0].user_global.present = false);
    CHECK_PROGRAM_CONTRACT_CHANGE(
        changed_programs[0].compiler_program = 6);
#undef CHECK_PROGRAM_CONTRACT_CHANGE

    uint8_t environment_a[USC_CACHE_DIGEST_SIZE] = {0};
    uint8_t environment_b[USC_CACHE_DIGEST_SIZE] = {0};
    environment_b[31] = 1;
    UnityCompilerCompileRequest environment_request = request;
    environment_request.environment_fingerprint = environment_a;
    uint8_t environment_request_digest[USC_CACHE_DIGEST_SIZE];
    request_digest(&environment_request, compiler_fingerprint,
                   environment_request_digest);
    environment_request.environment_fingerprint = environment_b;
    request_digest(&environment_request, compiler_fingerprint,
                   changed_digest);
    CHECK(memcmp(environment_request_digest, changed_digest,
                 USC_CACHE_DIGEST_SIZE) != 0);

    char* preprocess_include_paths[] = {
        "/project/Includes",
        "/Applications/Unity/CGIncludes",
    };
    UnityCompilerPreprocessRequest preprocess_request = {
        .command = "preprocess",
        .source = "Shader \"Test\" {}",
        .file_path = "Assets/Test.shader",
        .shader_name = "Test",
        .surface_only = false,
        .caching_preprocessor = true,
        .build_platform = 19,
        .valid_apis = 295472,
        .keywords = request_keywords,
        .keyword_count = 2,
        .defines = request_disabled_keywords,
        .define_count = 2,
        .include_paths = preprocess_include_paths,
        .include_path_count = 2,
        .toolchain_configuration = "Local",
        .environment_fingerprint = environment_a,
    };
    uint8_t preprocess_digest[USC_CACHE_DIGEST_SIZE];
    usc_cache_preprocess_request_digest(
        &preprocess_request, compiler_fingerprint, preprocess_digest);
    uint8_t* preprocess_transcript = NULL;
    size_t preprocess_transcript_size = 0U;
    CHECK(usc_cache_serialize_preprocess_request(
        &preprocess_request, compiler_fingerprint,
        &preprocess_transcript, &preprocess_transcript_size));
    CHECK(preprocess_transcript != NULL && preprocess_transcript_size > 0U);
    uint8_t preprocess_transcript_digest[USC_CACHE_DIGEST_SIZE];
    common_sha256(preprocess_transcript, preprocess_transcript_size,
                  preprocess_transcript_digest);
    CHECK(memcmp(preprocess_digest, preprocess_transcript_digest,
                 USC_CACHE_DIGEST_SIZE) == 0);
    free(preprocess_transcript);
#define CHECK_PREPROCESS_CHANGE(modification) do { \
    UnityCompilerPreprocessRequest changed = preprocess_request; \
    modification; \
    usc_cache_preprocess_request_digest( \
        &changed, compiler_fingerprint, changed_digest); \
    CHECK(memcmp(preprocess_digest, changed_digest, \
                 USC_CACHE_DIGEST_SIZE) != 0); \
} while (0)
    CHECK_PREPROCESS_CHANGE(changed.command = "preprocess2");
    CHECK_PREPROCESS_CHANGE(changed.source = "different");
    CHECK_PREPROCESS_CHANGE(changed.file_path = "Assets/Other.shader");
    CHECK_PREPROCESS_CHANGE(changed.shader_name = "Other");
    CHECK_PREPROCESS_CHANGE(changed.surface_only = true);
    CHECK_PREPROCESS_CHANGE(changed.caching_preprocessor = false);
    CHECK_PREPROCESS_CHANGE(changed.build_platform = 20);
    CHECK_PREPROCESS_CHANGE(changed.valid_apis++);
    CHECK_PREPROCESS_CHANGE(changed.keyword_count = 1);
    CHECK_PREPROCESS_CHANGE(changed.define_count = 1);
    CHECK_PREPROCESS_CHANGE(changed.include_path_count = 1);
    CHECK_PREPROCESS_CHANGE(changed.toolchain_configuration = "Remote");
    CHECK_PREPROCESS_CHANGE(changed.environment_fingerprint = environment_b);
#undef CHECK_PREPROCESS_CHANGE

    uint8_t different_fingerprint[USC_CACHE_DIGEST_SIZE];
    memcpy(different_fingerprint, compiler_fingerprint,
           USC_CACHE_DIGEST_SIZE);
    different_fingerprint[0] ^= 1U;
    request_digest(&request, different_fingerprint, changed_digest);
    CHECK(memcmp(baseline_digest, changed_digest,
                 USC_CACHE_DIGEST_SIZE) != 0);

    /* Length framing prevents concatenation collisions and order is retained. */
    char* joined_a[] = {"ab", "c"};
    char* joined_b[] = {"a", "bc"};
    UnityCompilerCompileRequest split_a = request;
    UnityCompilerCompileRequest split_b = request;
    split_a.variant_keywords = joined_a;
    split_b.variant_keywords = joined_b;
    request_digest(&split_a, compiler_fingerprint, changed_digest);
    uint8_t second_changed_digest[USC_CACHE_DIGEST_SIZE];
    request_digest(&split_b, compiler_fingerprint, second_changed_digest);
    CHECK(memcmp(changed_digest, second_changed_digest,
                 USC_CACHE_DIGEST_SIZE) != 0);

    static const uint8_t preprocess_blob[] = {
        0x00, 0x01, 0x00, 0x7f, 0xff,
    };
    ConditionalShaderRequirement serialized_conditionals[] = {
        {"KEY_A", 0x100},
        {"KEY_B", 0x200},
    };
    PreprocessedSnippet serialized_snippets[] = {
        {
            .source = "void vert() {}",
        },
        {
            .source = "void frag() {}",
            .reqs = 0x5678,
            .language = 2,
            .gpu_program_id = -1,
            .conditional_requirements = serialized_conditionals,
            .conditional_requirement_count = 2,
        },
    };
    serialized_snippets[0].contract = parsed_contract;
    serialized_snippets[0].has_contract = true;
    serialized_snippets[0].reqs = parsed_contract.requirements;
    serialized_snippets[0].language = parsed_contract.language;
    serialized_snippets[0].gpu_program_id = parsed_contract.snippet_id;
    serialized_snippets[0].conditional_requirements =
        parsed_contract.conditional_requirements;
    serialized_snippets[0].conditional_requirement_count =
        parsed_contract.conditional_requirement_count;
    PreprocessResult serialized_result = {
        .snippets = serialized_snippets,
        .snippet_count = 2,
        .blob = (uint8_t*)preprocess_blob,
        .blob_len = sizeof(preprocess_blob),
    };
    uint8_t* serialized_data = NULL;
    size_t serialized_size = 0;
    CHECK(usc_cache_serialize_preprocess_result(
        &serialized_result, &serialized_data, &serialized_size));
    CHECK(serialized_data != NULL && serialized_size > 32);
    PreprocessResult decoded_result;
    CHECK(usc_cache_deserialize_preprocess_result(
        serialized_data, serialized_size, &decoded_result));
    CHECK(preprocess_results_equal(&serialized_result, &decoded_result));
    unity_compiler_free_preprocess(&decoded_result);
    CHECK(!usc_cache_deserialize_preprocess_result(
        serialized_data, serialized_size - 1U, &decoded_result));
    CHECK(decoded_result.snippets == NULL && decoded_result.blob == NULL);
    uint8_t saved_magic = serialized_data[0];
    serialized_data[0] ^= 1U;
    CHECK(!usc_cache_deserialize_preprocess_result(
        serialized_data, serialized_size, &decoded_result));
    serialized_data[0] = saved_magic;
    uint8_t* trailing_data = (uint8_t*)malloc(serialized_size + 1U);
    CHECK(trailing_data != NULL);
    memcpy(trailing_data, serialized_data, serialized_size);
    trailing_data[serialized_size] = 0;
    CHECK(!usc_cache_deserialize_preprocess_result(
        trailing_data, serialized_size + 1U, &decoded_result));
    free(trailing_data);
    free(serialized_data);

    UnityCompilerDiagnostic cached_diagnostic = {
        .fields = {13, 14, 15},
        .record = "err: 13 14 15",
        .file = "Assets/Diagnosed.shader",
        .message = "fixture compile warning",
    };
    static const uint8_t cached_binary[] = {0x00, 0xff, 0x41, 0x00};
    UnityCompilerReflectionRecord cached_reflection;
    CHECK(unity_compiler_reflection_record_parse(
        "texbind: MainTex 0 1 2 3", &cached_reflection));
    UnityCompilerBinaryResponse binary_response = {
        .status =
            {
                .compiler_success = true,
                .from_cache = false,
                .diagnostics = &cached_diagnostic,
                .diagnostic_count = 1U,
            },
        .reflection_records = &cached_reflection,
        .reflection_record_count = 1U,
        .data = (uint8_t *)cached_binary,
        .size = sizeof(cached_binary),
        .has_request_identity = true,
        .request_digest = {1},
        .controls_digest = {2},
    };
    uint8_t* encoded_response = NULL;
    size_t encoded_response_size = 0U;
    CHECK(usc_cache_serialize_binary_response(
        &binary_response, &encoded_response, &encoded_response_size));
    UnityCompilerBinaryResponse decoded_binary_response;
    CHECK(usc_cache_deserialize_binary_response(
        encoded_response, encoded_response_size, &decoded_binary_response));
    CHECK(!decoded_binary_response.has_request_identity &&
          decoded_binary_response.request_digest[0] == 0 &&
          decoded_binary_response.controls_digest[0] == 0);
    CHECK(decoded_binary_response.status.compiler_success &&
          !decoded_binary_response.status.from_cache &&
          decoded_binary_response.status.diagnostic_count == 1U &&
          decoded_binary_response.reflection_record_count == 1U &&
          unity_compiler_reflection_records_equal(
              &cached_reflection, 1U,
              decoded_binary_response.reflection_records, 1U));
    CHECK(decoded_binary_response.size == sizeof(cached_binary) &&
          memcmp(decoded_binary_response.data, cached_binary,
                 sizeof(cached_binary)) == 0);
    CHECK(decoded_binary_response.status.diagnostics[0].fields[2] == 15 &&
          strcmp(decoded_binary_response.status.diagnostics[0].record,
                 "err: 13 14 15") == 0 &&
          strcmp(decoded_binary_response.status.diagnostics[0].file,
                 "Assets/Diagnosed.shader") == 0 &&
          strcmp(decoded_binary_response.status.diagnostics[0].message,
                 "fixture compile warning") == 0);
    unity_compiler_binary_response_free(&decoded_binary_response);
    CHECK(!usc_cache_deserialize_binary_response(
        encoded_response, encoded_response_size - 1U,
        &decoded_binary_response));
    free(encoded_response);
    unity_compiler_reflection_record_free(&cached_reflection);

    UnityCompilerPreprocessResponse preprocess_response = {
        .status = {
            .compiler_success = true,
            .diagnostics = &cached_diagnostic,
            .diagnostic_count = 1U,
        },
        .result = serialized_result,
    };
    preprocess_response.has_request_identity = true;
    memset(preprocess_response.request_digest, 0x31, sizeof(preprocess_response.request_digest));
    memset(preprocess_response.controls_digest, 0x42, sizeof(preprocess_response.controls_digest));
    encoded_response = NULL;
    encoded_response_size = 0U;
    CHECK(usc_cache_serialize_preprocess_response(
        &preprocess_response, &encoded_response, &encoded_response_size));
    UnityCompilerPreprocessResponse decoded_preprocess_response;
    CHECK(usc_cache_deserialize_preprocess_response(
        encoded_response, encoded_response_size,
        &decoded_preprocess_response));
    CHECK(!decoded_preprocess_response.has_request_identity);
    CHECK(decoded_preprocess_response.request_digest[0] == 0 &&
          decoded_preprocess_response.controls_digest[0] == 0);
    CHECK(decoded_preprocess_response.status.compiler_success &&
          decoded_preprocess_response.status.diagnostic_count == 1U &&
          preprocess_results_equal(
              &serialized_result, &decoded_preprocess_response.result));
    unity_compiler_preprocess_response_free(&decoded_preprocess_response);
    CHECK(!usc_cache_deserialize_preprocess_response(
        encoded_response, encoded_response_size - 1U,
        &decoded_preprocess_response));
    free(encoded_response);

    char cache_dir[PATH_MAX];
    CHECK(snprintf(cache_dir, sizeof(cache_dir), "%s/cache", temporary_dir) > 0);

    const char* previous_cache_env = getenv("DXBC_USC_CACHE_DIR");
    char* saved_cache_env = previous_cache_env ? strdup(previous_cache_env)
                                               : NULL;
    CHECK(!previous_cache_env || saved_cache_env != NULL);
    CHECK(setenv("DXBC_USC_CACHE_DIR", cache_dir, 1) == 0);
    char configured_contents[PATH_MAX];
    CHECK(snprintf(configured_contents, sizeof(configured_contents),
                   "%s/portable-unity/Contents/", temporary_dir) > 0);
    CHECK(setenv("DXBC_UNITY_CONTENTS_PATH", configured_contents, 1) == 0);
    CHECK(setenv("DXBC_UNITY_COMPILER_PATH", fingerprint_path, 1) == 0);
    CHECK(setenv("DXBC_UNITY_BUILTIN_INCLUDES_PATH", builtin_includes, 1) ==
          0);
    CHECK(setenv("DXBC_UNITY_PLAYBACK_ENGINES_PATH", temporary_dir, 1) == 0);
    CHECK(setenv("DXBC_UNITY_GLSLANG_PATH", glslang_path, 1) == 0);
    CHECK(setenv("DXBC_UNITY_DXCOMPILER_PATH", dxcompiler_path, 1) == 0);
    UnityCompilerChannel lazy_channel;
    CHECK(unity_compiler_start(&lazy_channel, temporary_dir,
                               explicit_includes));
    CHECK(lazy_channel.configured);
    CHECK(lazy_channel.socket_fd == -1);
    CHECK(lazy_channel.process_id == 0);
    char expected_sandbox_includes[PATH_MAX];
    CHECK(snprintf(expected_sandbox_includes,
                   sizeof(expected_sandbox_includes),
                   "%s/shader_includes",
                   temporary_dir) > 0);
    CHECK(strcmp(lazy_channel.sandbox_includes_dir,
                 expected_sandbox_includes) == 0);
    char expected_contents[PATH_MAX];
    CHECK(snprintf(expected_contents, sizeof(expected_contents),
                   "%s/portable-unity/Contents", temporary_dir) > 0);
    CHECK(strcmp(lazy_channel.unity_contents_path, expected_contents) == 0);
    CHECK(strcmp(lazy_channel.compiler_path, fingerprint_path) == 0);
    CHECK(strcmp(lazy_channel.builtin_includes_dir, builtin_includes) == 0);
    UnityCompilerToolchainProvenance provenance;
    CHECK(unity_compiler_get_toolchain_provenance(&lazy_channel,
                                                   &provenance));
    CHECK(strcmp(provenance.compiler_path, fingerprint_path) == 0);
    CHECK(memcmp(provenance.compiler_fingerprint, multi_block_sha256,
                 USC_CACHE_DIGEST_SIZE) == 0);

    /* Environment identity is a per-channel content snapshot, not a
     * process-global memo keyed only by path strings. */
    CHECK(write_file(explicit_file, "explicit-c", 10));
    UnityCompilerChannel refreshed_channel;
    CHECK(unity_compiler_start_lazy(&refreshed_channel, temporary_dir,
                                    explicit_includes));
    UnityCompilerToolchainProvenance refreshed_provenance;
    CHECK(unity_compiler_get_toolchain_provenance(
        &refreshed_channel, &refreshed_provenance));
    CHECK(memcmp(provenance.compiler_fingerprint,
                 refreshed_provenance.compiler_fingerprint,
                 USC_CACHE_DIGEST_SIZE) == 0);
    CHECK(memcmp(provenance.environment_fingerprint,
                 refreshed_provenance.environment_fingerprint,
                 USC_CACHE_DIGEST_SIZE) != 0);

    UnityCompilerSnippetCompileRequest public_request = {
        .snippet_source = request.snippet_source,
        .source_directory = request.source_directory,
        .source_basename = request.source_basename,
        .pass_name = request.pass_name,
        .caching_preprocessor = request.caching_preprocessor,
        .preprocess_only = request.preprocess_only,
        .strip_line_directives = request.strip_line_directives,
        .build_platform = request.build_platform,
        .render_state_length = request.render_state_length,
        .variant_keywords = request.variant_keywords,
        .variant_keyword_count = request.variant_keyword_count,
        .user_keywords = request.user_keywords,
        .user_keyword_count = request.user_keyword_count,
        .disabled_keywords = request.disabled_keywords,
        .disabled_keyword_count = request.disabled_keyword_count,
        .compiler_flags = request.compiler_flags,
        .shader_type = request.shader_type,
        .platform = request.platform,
        .requirements = request.requirements,
        .program_mask = request.program_mask,
        .program_start = request.program_start,
        .contract = &parsed_contract,
    };
    uint8_t* public_transcript = NULL;
    size_t public_transcript_size = 0;
    uint8_t public_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    memset(public_request_digest, 0xa5, sizeof(public_request_digest));
    CHECK(!unity_compiler_serialize_compile_request(
        &lazy_channel, &public_request, &public_transcript,
        &public_transcript_size, public_request_digest));
    CHECK(public_transcript == NULL && public_transcript_size == 0U);
    for (size_t digest_index = 0;
         digest_index < sizeof(public_request_digest); digest_index++) {
        CHECK(public_request_digest[digest_index] == 0U);
    }
    CHECK(lazy_channel.cache_toolchain_lease == NULL);
    CHECK(lazy_channel.process_id == 0 && lazy_channel.socket_fd == -1);

    CHECK(unity_compiler_serialize_compile_request(
        &refreshed_channel, &public_request, &public_transcript,
        &public_transcript_size, public_request_digest));
    CHECK(public_transcript != NULL && public_transcript_size > 0);
    common_sha256(public_transcript, public_transcript_size,
                  transcript_digest);
    CHECK(memcmp(public_request_digest, transcript_digest,
                 sizeof(public_request_digest)) == 0);
    free(public_transcript);

    UnityCompilerShaderPreprocessRequest public_preprocess_request = {
        .source = preprocess_request.source,
        .file_path = preprocess_request.file_path,
        .shader_name = preprocess_request.shader_name,
        .surface_only = preprocess_request.surface_only,
        .caching_preprocessor = preprocess_request.caching_preprocessor,
        .build_platform = preprocess_request.build_platform,
        .valid_apis = preprocess_request.valid_apis,
        .keywords = preprocess_request.keywords,
        .keyword_count = preprocess_request.keyword_count,
        .defines = preprocess_request.defines,
        .define_count = preprocess_request.define_count,
    };
    public_transcript = NULL;
    public_transcript_size = 0U;
    CHECK(unity_compiler_serialize_preprocess_request(
        &refreshed_channel, &public_preprocess_request, &public_transcript,
        &public_transcript_size, public_request_digest));
    CHECK(public_transcript != NULL && public_transcript_size > 0U);
    common_sha256(public_transcript, public_transcript_size,
                  transcript_digest);
    CHECK(memcmp(public_request_digest, transcript_digest,
                 sizeof(public_request_digest)) == 0);
    CHECK(refreshed_channel.process_id == 0 &&
          refreshed_channel.socket_fd == -1);
    free(public_transcript);

    /* A validated v4 pack can supply canonical request authority when the
     * configured Unity path spellings exist only as identity strings. */
    uint8_t pack_compiler[ORACLE_PACK_DIGEST_SIZE];
    uint8_t pack_environment[ORACLE_PACK_DIGEST_SIZE];
    for (size_t index = 0; index < ORACLE_PACK_DIGEST_SIZE; ++index) {
        pack_compiler[index] = (uint8_t)(0x21U + index);
        pack_environment[index] = (uint8_t)(0x81U + index);
    }
    OraclePackAuthorityInput pack_authority_input = {
        pack_compiler, pack_environment};
    OraclePackWriter* authority_writer = NULL;
    CHECK(oracle_pack_writer_create(&pack_authority_input,
                                    &authority_writer) == ORACLE_PACK_OK);
    uint8_t* authority_pack_bytes = NULL;
    size_t authority_pack_size = 0U;
    CHECK(oracle_pack_writer_finalize(
              authority_writer, &authority_pack_bytes,
              &authority_pack_size) == ORACLE_PACK_OK);
    OraclePack* authority_pack = NULL;
    CHECK(oracle_pack_open_memory(authority_pack_bytes, authority_pack_size,
                                  &authority_pack) == ORACLE_PACK_OK);
    OraclePackAuthorityView authority_view;
    CHECK(oracle_pack_authority(authority_pack, &authority_view) ==
          ORACLE_PACK_OK);
    UnityCompilerOfflineAuthority offline_authority = {
        authority_view.compiler_fingerprint,
        authority_view.environment_fingerprint,
    };

    static const char nonexistent_contents[] =
        "/definitely/nonexistent/Unity.app/Contents";
    static const char nonexistent_compiler[] =
        "/definitely/nonexistent/UnityShaderCompiler";
    static const char nonexistent_builtins[] =
        "/definitely/nonexistent/CGIncludes";
    static const char nonexistent_playback[] =
        "/definitely/nonexistent/PlaybackEngines";
    static const char nonexistent_glslang[] =
        "/definitely/nonexistent/glslang.dylib";
    static const char nonexistent_dxcompiler[] =
        "/definitely/nonexistent/libdxcompiler.dylib";
    CHECK(setenv("DXBC_UNITY_CONTENTS_PATH", nonexistent_contents, 1) == 0);
    CHECK(setenv("DXBC_UNITY_COMPILER_PATH", nonexistent_compiler, 1) == 0);
    CHECK(setenv("DXBC_UNITY_BUILTIN_INCLUDES_PATH",
                 nonexistent_builtins, 1) == 0);
    CHECK(setenv("DXBC_UNITY_PLAYBACK_ENGINES_PATH",
                 nonexistent_playback, 1) == 0);
    CHECK(setenv("DXBC_UNITY_GLSLANG_PATH", nonexistent_glslang, 1) == 0);
    CHECK(setenv("DXBC_UNITY_DXCOMPILER_PATH",
                 nonexistent_dxcompiler, 1) == 0);
    UnityCompilerChannel offline_channel;
    CHECK(unity_compiler_start_lazy(
        &offline_channel, "/definitely/nonexistent/project",
        "/definitely/nonexistent/includes"));
    CHECK(offline_channel.process_id == 0 &&
          offline_channel.socket_fd == -1 &&
          offline_channel.cache_toolchain_lease == NULL &&
          !offline_channel.cache_compiler_ready &&
          !offline_channel.cache_environment_ready);

    public_transcript = NULL;
    public_transcript_size = 0U;
    CHECK(unity_compiler_serialize_compile_request_with_authority(
        &public_request, &offline_authority, &public_transcript,
        &public_transcript_size, public_request_digest));
    CHECK(public_transcript != NULL && public_transcript_size > 0U);
    common_sha256(public_transcript, public_transcript_size,
                  transcript_digest);
    CHECK(memcmp(public_request_digest, transcript_digest,
                 sizeof(public_request_digest)) == 0);
    free(public_transcript);
    CHECK(offline_channel.process_id == 0 &&
          offline_channel.socket_fd == -1 &&
          offline_channel.cache_toolchain_lease == NULL &&
          !offline_channel.cache_compiler_ready &&
          !offline_channel.cache_environment_ready);

    public_transcript = NULL;
    public_transcript_size = 0U;
    CHECK(unity_compiler_serialize_preprocess_request_with_authority(
        &offline_channel, &public_preprocess_request, &offline_authority,
        &public_transcript, &public_transcript_size,
        public_request_digest));
    CHECK(public_transcript != NULL && public_transcript_size > 0U);
    common_sha256(public_transcript, public_transcript_size,
                  transcript_digest);
    CHECK(memcmp(public_request_digest, transcript_digest,
                 sizeof(public_request_digest)) == 0);
    free(public_transcript);
    CHECK(offline_channel.process_id == 0 &&
          offline_channel.socket_fd == -1 &&
          offline_channel.cache_toolchain_lease == NULL &&
          !offline_channel.cache_compiler_ready &&
          !offline_channel.cache_environment_ready);

    uint8_t zero_authority_bytes[UNITY_COMPILER_FINGERPRINT_SIZE] = {0};
    UnityCompilerOfflineAuthority invalid_offline_authority = {
        zero_authority_bytes, authority_view.environment_fingerprint};
    public_transcript = (uint8_t*)(uintptr_t)1U;
    public_transcript_size = SIZE_MAX;
    memset(public_request_digest, 0xa5, sizeof(public_request_digest));
    CHECK(!unity_compiler_serialize_compile_request_with_authority(
        &public_request, &invalid_offline_authority, &public_transcript,
        &public_transcript_size, public_request_digest));
    CHECK(public_transcript == NULL && public_transcript_size == 0U);
    for (size_t index = 0; index < sizeof(public_request_digest); ++index) {
        CHECK(public_request_digest[index] == 0U);
    }

    unity_compiler_shutdown(&offline_channel);
    oracle_pack_free(authority_pack);
    oracle_pack_bytes_free(authority_pack_bytes);
    oracle_pack_writer_free(authority_writer);
    CHECK(setenv("DXBC_UNITY_CONTENTS_PATH", configured_contents, 1) == 0);
    CHECK(setenv("DXBC_UNITY_COMPILER_PATH", fingerprint_path, 1) == 0);
    CHECK(setenv("DXBC_UNITY_BUILTIN_INCLUDES_PATH", builtin_includes, 1) ==
          0);
    CHECK(setenv("DXBC_UNITY_PLAYBACK_ENGINES_PATH", temporary_dir, 1) == 0);
    CHECK(setenv("DXBC_UNITY_GLSLANG_PATH", glslang_path, 1) == 0);
    CHECK(setenv("DXBC_UNITY_DXCOMPILER_PATH", dxcompiler_path, 1) == 0);

    unity_compiler_shutdown(&refreshed_channel);
    unity_compiler_shutdown(&lazy_channel);
    CHECK(!lazy_channel.configured && lazy_channel.socket_fd == -1 &&
          lazy_channel.process_id == 0);
    if (saved_cache_env) {
        CHECK(setenv("DXBC_USC_CACHE_DIR", saved_cache_env, 1) == 0);
    } else {
        CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);
    }
    free(saved_cache_env);

    unity_compiler_cache_reset_stats();
    uint8_t* loaded = NULL;
    size_t loaded_size = 99;
    CHECK(usc_cache_load(cache_dir, baseline_digest, &loaded, &loaded_size) ==
          USC_CACHE_MISS);
    CHECK(loaded == NULL && loaded_size == 0);

    static const uint8_t payload[] = {0x00, 0x11, 0x00, 0xff, 0x7e};
    CHECK(usc_cache_store(cache_dir, baseline_digest, payload,
                          sizeof(payload)));
    CHECK(usc_cache_load(cache_dir, baseline_digest, &loaded, &loaded_size) ==
          USC_CACHE_HIT);
    CHECK(loaded_size == sizeof(payload));
    CHECK(memcmp(loaded, payload, sizeof(payload)) == 0);
    free(loaded);

    UnityCompilerCompileRequest empty_request = request;
    empty_request.snippet_source = "empty successful output";
    uint8_t empty_digest[USC_CACHE_DIGEST_SIZE];
    request_digest(&empty_request, compiler_fingerprint, empty_digest);
    CHECK(usc_cache_store(cache_dir, empty_digest, NULL, 0));
    loaded = NULL;
    loaded_size = 99;
    CHECK(usc_cache_load(cache_dir, empty_digest, &loaded, &loaded_size) ==
          USC_CACHE_HIT);
    CHECK(loaded != NULL && loaded_size == 0);
    free(loaded);

    /* A truncated or request-mismatched header must never be returned. */
    char entry_path[PATH_MAX];
    cache_entry_path(cache_dir, baseline_digest, entry_path);
    static const uint8_t truncated[] = {'b', 'a', 'd'};
    CHECK(write_file(entry_path, truncated, sizeof(truncated)));
    CHECK(usc_cache_load(cache_dir, baseline_digest, &loaded, &loaded_size) ==
          USC_CACHE_MISS);
    CHECK(loaded == NULL);

    CHECK(usc_cache_store(cache_dir, baseline_digest, payload,
                          sizeof(payload)));
    int corrupt_fd = open(entry_path, O_WRONLY);
    CHECK(corrupt_fd >= 0);
    uint8_t wrong_request_byte = (uint8_t)(baseline_digest[0] ^ 1U);
    CHECK(pwrite(corrupt_fd, &wrong_request_byte, 1, 24) == 1);
    CHECK(close(corrupt_fd) == 0);
    CHECK(usc_cache_load(cache_dir, baseline_digest, &loaded, &loaded_size) ==
          USC_CACHE_MISS);

    CHECK(usc_cache_store(cache_dir, baseline_digest, payload,
                          sizeof(payload)));
    corrupt_fd = open(entry_path, O_WRONLY);
    CHECK(corrupt_fd >= 0);
    uint8_t wrong_payload_byte = 0xaa;
    CHECK(pwrite(corrupt_fd, &wrong_payload_byte, 1, 96) == 1);
    CHECK(close(corrupt_fd) == 0);
    CHECK(usc_cache_load(cache_dir, baseline_digest, &loaded, &loaded_size) ==
          USC_CACHE_MISS);

    /* Competing processes publish through unique temporaries and atomic rename. */
    pid_t children[4];
    for (int i = 0; i < 4; i++) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) {
            _exit(usc_cache_store(cache_dir, baseline_digest, payload,
                                  sizeof(payload)) ? 0 : 1);
        }
    }
    for (int i = 0; i < 4; i++) {
        int status = 0;
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(usc_cache_load(cache_dir, baseline_digest, &loaded, &loaded_size) ==
          USC_CACHE_HIT);
    CHECK(loaded_size == sizeof(payload));
    CHECK(memcmp(loaded, payload, sizeof(payload)) == 0);
    free(loaded);

    UnityCompilerCacheStats stats;
    unity_compiler_cache_get_stats(&stats);
    CHECK(stats.hits == 3);
    CHECK(stats.misses == 4);
    CHECK(stats.stores == 4);
    CHECK(stats.corrupt_entries == 3);
    CHECK(stats.write_errors == 0);

    /* Exercise the persistent channel against this test executable acting as
     * a fixture UnityShaderCompiler.  Each transport/protocol failure must
     * leave a configured but disconnected channel, and the following request
     * must launch a clean replacement process. */
    char self_path[PATH_MAX];
    CHECK(realpath(argv[0], self_path) != NULL);
    const char* previous_transport_cache = getenv("DXBC_USC_CACHE_DIR");
    char* saved_transport_cache = previous_transport_cache
        ? strdup(previous_transport_cache)
        : NULL;
    CHECK(!previous_transport_cache || saved_transport_cache != NULL);
    const char* previous_cache_only = getenv("DXBC_USC_CACHE_ONLY");
    char* saved_cache_only = previous_cache_only
        ? strdup(previous_cache_only)
        : NULL;
    CHECK(!previous_cache_only || saved_cache_only != NULL);
    CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);
    CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);
    CHECK(setenv("DXBC_USC_FAKE_SERVER", "1", 1) == 0);
    CHECK(unsetenv("DXBC_USC_FAKE_INIT_MODE") == 0);
    CHECK(setenv("DXBC_USC_IO_TIMEOUT_MS", "300", 1) == 0);
    CHECK(setenv("TMPDIR", "parent-tmpdir", 1) == 0);
    CHECK(setenv("DYLD_LIBRARY_PATH", "parent-dyld-path", 1) == 0);
    CHECK(setenv("LD_LIBRARY_PATH", "parent-ld-path", 1) == 0);
    CHECK(setenv("DYLD_INSERT_LIBRARIES", "parent-insert", 1) == 0);
    CHECK(setenv(
        "PROXY_DYLD_INSERT_LIBRARIES", "parent-proxy-insert", 1) == 0);
    CHECK(setenv("DXBC_UNITY_COMPILER_PATH", self_path, 1) == 0);
    CHECK(setenv("DXBC_UNITY_CONTENTS_PATH", temporary_dir, 1) == 0);
    CHECK(setenv("DXBC_UNITY_BUILTIN_INCLUDES_PATH", builtin_includes, 1) ==
          0);
    CHECK(setenv("DXBC_UNITY_PLAYBACK_ENGINES_PATH", temporary_dir, 1) == 0);
    CHECK(setenv("DXBC_UNITY_GLSLANG_PATH", glslang_path, 1) == 0);
    CHECK(setenv("DXBC_UNITY_DXCOMPILER_PATH", dxcompiler_path, 1) == 0);

    /* Cache-only authority acquisition is process-free and fails closed when
     * no initializeCompiler record has previously been captured. */
    UnityCompilerChannel offline_capability_channel;
    CHECK(unity_compiler_start_lazy(
        &offline_capability_channel, temporary_dir, builtin_includes));
    UnityCompilerSessionCapabilities observed_capabilities;
    CHECK(!unity_compiler_session_capabilities_snapshot(
        &offline_capability_channel, &observed_capabilities));
    CHECK(setenv("DXBC_USC_CACHE_ONLY", "1", 1) == 0);
    CHECK(!unity_compiler_capture_session_capabilities(
        &offline_capability_channel, &observed_capabilities));
    CHECK(offline_capability_channel.process_id == 0 &&
          offline_capability_channel.socket_fd == -1);
    UnityCompilerShaderPreprocessRequest offline_preprocess = {
        .source = "Shader \"Offline\" {}",
        .file_path = "Assets/Offline.shader",
        .shader_name = "offline-cache-only-miss",
        .surface_only = false,
        .caching_preprocessor = true,
        .build_platform = 19U,
        .valid_apis = UINT32_C(1),
    };
    UnityCompilerPreprocessResponse offline_preprocess_response;
    CHECK(unity_compiler_preprocess_contract_response(
        &offline_capability_channel, &offline_preprocess,
        &offline_preprocess_response));
    CHECK(offline_preprocess_response.status.availability ==
              UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS &&
          offline_capability_channel.process_id == 0 &&
          offline_capability_channel.socket_fd == -1);
    CHECK(offline_preprocess_response.has_request_identity);
    uint8_t offline_request_bits = 0;
    for (size_t i = 0; i < sizeof(offline_preprocess_response.request_digest); ++i)
        offline_request_bits |= offline_preprocess_response.request_digest[i];
    CHECK(offline_request_bits != 0);
    unity_compiler_preprocess_response_free(&offline_preprocess_response);
    CHECK(!offline_preprocess_response.has_request_identity);
    UnityCompilerTextResponse offline_disassemble_response;
    CHECK(unity_compiler_disassemble_response(
        &offline_capability_channel, "offline-cache-only-disassemble", 4,
        0, NULL, 0U, &offline_disassemble_response));
    CHECK(offline_disassemble_response.status.availability ==
              UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS &&
          !offline_disassemble_response.status.compiler_success &&
          !offline_disassemble_response.status.from_cache &&
          offline_disassemble_response.status.diagnostic_count == 0U &&
          offline_disassemble_response.text == NULL &&
          offline_disassemble_response.size == 0U &&
          offline_capability_channel.process_id == 0 &&
          offline_capability_channel.socket_fd == -1);
    unity_compiler_text_response_free(&offline_disassemble_response);
    CHECK(unity_compiler_disassemble(
              &offline_capability_channel,
              "offline-cache-only-disassemble-compat", 4, 0, NULL, 0U) ==
          NULL);
    CHECK(offline_capability_channel.process_id == 0 &&
          offline_capability_channel.socket_fd == -1);
    CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);
    unity_compiler_shutdown(&offline_capability_channel);

    /* A malformed mask and a truncated 25-record response are protocol
     * failures. Neither partially publishes session authority. */
    static const char* invalid_initializations[] = {
        "invalid-mask", "truncated",
    };
    for (size_t mode_index = 0;
         mode_index < sizeof(invalid_initializations) /
                          sizeof(invalid_initializations[0]);
         mode_index++) {
        CHECK(setenv("DXBC_USC_FAKE_INIT_MODE",
                     invalid_initializations[mode_index], 1) == 0);
        UnityCompilerChannel invalid_initialization_channel;
        CHECK(unity_compiler_start_lazy(
            &invalid_initialization_channel, temporary_dir,
            builtin_includes));
        CHECK(!unity_compiler_capture_session_capabilities(
            &invalid_initialization_channel, &observed_capabilities));
        CHECK(!invalid_initialization_channel.session_capabilities_ready &&
              invalid_initialization_channel.process_id == 0 &&
              invalid_initialization_channel.socket_fd == -1);
        CHECK(!unity_compiler_session_capabilities_snapshot(
            &invalid_initialization_channel, &observed_capabilities));
        unity_compiler_shutdown(&invalid_initialization_channel);
    }
    CHECK(unsetenv("DXBC_USC_FAKE_INIT_MODE") == 0);

    /* The incomplete legacy preprocess wrapper must take validApis from the
     * live initializeCompiler record, never from the historical literal. */
    CHECK(setenv(
        "DXBC_USC_FAKE_INIT_MODE", "alternate-valid-apis", 1) == 0);
    UnityCompilerChannel legacy_preprocess_channel;
    CHECK(unity_compiler_start_lazy(
        &legacy_preprocess_channel, temporary_dir, builtin_includes));
    PreprocessResult legacy_preprocess_result;
    CHECK(unity_compiler_preprocess(
        &legacy_preprocess_channel, "Shader \"LegacyDynamic\" {}",
        "legacy-session-valid-apis", &legacy_preprocess_result));
    CHECK(legacy_preprocess_result.snippet_count == 0 &&
          legacy_preprocess_result.blob_len == 0U);
    unity_compiler_free_preprocess(&legacy_preprocess_result);
    CHECK(unity_compiler_session_capabilities_snapshot(
        &legacy_preprocess_channel, &observed_capabilities));
    CHECK(unity_compiler_session_capabilities_valid_apis(
        &observed_capabilities, &session_valid_apis));
    CHECK(session_valid_apis ==
          (FAKE_SESSION_VALID_APIS & ~(UINT32_C(1) << 4U)));
    unity_compiler_shutdown(&legacy_preprocess_channel);
    CHECK(unsetenv("DXBC_USC_FAKE_INIT_MODE") == 0);

    /* Two processes under one immutable toolchain lease must report the same
     * exact capability record. A disagreement poisons that authority instead
     * of silently retaining either response. */
    UnityCompilerChannel changed_initialization_channel;
    CHECK(unity_compiler_start_lazy(
        &changed_initialization_channel, temporary_dir, builtin_includes));
    CHECK(unity_compiler_capture_session_capabilities(
        &changed_initialization_channel, &observed_capabilities));
    CHECK(unity_compiler_recycle_process(
        &changed_initialization_channel));
    CHECK(setenv("DXBC_USC_FAKE_INIT_MODE", "changed-record", 1) == 0);
    CHECK(unity_compiler_disassemble(
              &changed_initialization_channel, "success", 4, 0,
              NULL, 0U) == NULL);
    CHECK(changed_initialization_channel.session_capabilities_failed &&
          !changed_initialization_channel.session_capabilities_ready &&
          changed_initialization_channel.process_id == 0 &&
          changed_initialization_channel.socket_fd == -1);
    CHECK(!unity_compiler_session_capabilities_snapshot(
        &changed_initialization_channel, &observed_capabilities));
    CHECK(unsetenv("DXBC_USC_FAKE_INIT_MODE") == 0);
    CHECK(!unity_compiler_capture_session_capabilities(
        &changed_initialization_channel, &observed_capabilities));
    CHECK(changed_initialization_channel.process_id == 0 &&
          changed_initialization_channel.socket_fd == -1);
    unity_compiler_shutdown(&changed_initialization_channel);

    UnityCompilerChannel fixture_channel;
    /* Explicit and builtin include authority intentionally use the same path.
     * The client must send that spelling once; the fake server rejects any
     * duplicate initialization path above. */
    CHECK(unity_compiler_start_lazy(
        &fixture_channel, temporary_dir, builtin_includes));
    CHECK(fixture_channel.configured && fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);

    CHECK(!unity_compiler_session_capabilities_snapshot(
        &fixture_channel, &observed_capabilities));
    CHECK(unity_compiler_capture_session_capabilities(
        &fixture_channel, &observed_capabilities));
    CHECK(observed_capabilities.raw_available_platform_mask ==
          FAKE_SESSION_RAW_MASK);
    for (size_t index = 0; index < UNITY_COMPILER_PLATFORM_COUNT; index++) {
        CHECK(observed_capabilities.platforms[index].supported_features ==
              fake_session_features(index));
        CHECK(observed_capabilities.platforms[index].version ==
              fake_session_version(index));
    }
    CHECK(unity_compiler_session_capabilities_valid_apis(
        &observed_capabilities, &session_valid_apis));
    CHECK(session_valid_apis == FAKE_SESSION_VALID_APIS);
    pid_t capability_pid = fixture_channel.process_id;
    CHECK(capability_pid > 0 && fixture_channel.socket_fd >= 0);
    UnityCompilerSessionCapabilities second_capability_snapshot;
    CHECK(unity_compiler_capture_session_capabilities(
        &fixture_channel, &second_capability_snapshot));
    CHECK(fixture_channel.process_id == capability_pid);
    CHECK(unity_compiler_session_capabilities_equal(
        &observed_capabilities, &second_capability_snapshot));
    CHECK(unity_compiler_session_capabilities_snapshot(
        &fixture_channel, &second_capability_snapshot));
    CHECK(fixture_channel.process_id == capability_pid);

    CHECK(unity_compiler_disassemble(
              &fixture_channel, "stall-read", 4, 0, NULL, 0U) == NULL);
    CHECK(fixture_channel.configured && fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    char* fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);
    CHECK(strcmp(getenv("TMPDIR"), "parent-tmpdir") == 0);
    CHECK(strcmp(getenv("DYLD_LIBRARY_PATH"), "parent-dyld-path") == 0);
    CHECK(strcmp(getenv("LD_LIBRARY_PATH"), "parent-ld-path") == 0);
    CHECK(strcmp(getenv("DYLD_INSERT_LIBRARIES"), "parent-insert") == 0);
    CHECK(strcmp(getenv("PROXY_DYLD_INSERT_LIBRARIES"),
                 "parent-proxy-insert") == 0);
    pid_t healthy_pid = fixture_channel.process_id;
    CHECK(healthy_pid > 0 && fixture_channel.socket_fd >= 0);

    /* A memory-lifetime recycle must reap only the child while retaining the
     * exact configured toolchain authority for a lazy replacement. */
    char* retained_compiler_path = strdup(fixture_channel.compiler_path);
    char* retained_contents_path = strdup(fixture_channel.unity_contents_path);
    CHECK(retained_compiler_path != NULL && retained_contents_path != NULL);
    CHECK(unity_compiler_recycle_process(&fixture_channel));
    CHECK(fixture_channel.configured && fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    CHECK(unity_compiler_session_capabilities_snapshot(
        &fixture_channel, &second_capability_snapshot));
    CHECK(unity_compiler_session_capabilities_equal(
        &observed_capabilities, &second_capability_snapshot));
    CHECK(strcmp(fixture_channel.compiler_path, retained_compiler_path) == 0);
    CHECK(strcmp(fixture_channel.unity_contents_path,
                 retained_contents_path) == 0);
    errno = 0;
    CHECK(kill(healthy_pid, 0) < 0 && errno == ESRCH);
    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);
    CHECK(unity_compiler_session_capabilities_snapshot(
        &fixture_channel, &second_capability_snapshot));
    CHECK(unity_compiler_session_capabilities_equal(
        &observed_capabilities, &second_capability_snapshot));
    free(retained_compiler_path);
    free(retained_contents_path);
    healthy_pid = fixture_channel.process_id;
    CHECK(healthy_pid > 0 && fixture_channel.socket_fd >= 0);

    /* A valid negative status is not a transport failure: the synchronized
     * channel remains reusable without a restart. */
    CHECK(unity_compiler_disassemble(
              &fixture_channel, "valid-failure", 4, 0, NULL, 0U) == NULL);
    CHECK(fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);
    CHECK(unity_compiler_disassemble(
              &fixture_channel, "error-disassemble", 4, 0, NULL, 0U) ==
          NULL);
    CHECK(fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);
    UnityCompilerTextResponse diagnosed_disassembly;
    CHECK(unity_compiler_disassemble_response(
        &fixture_channel, "success-diagnostic-disassemble", 4, 0,
        NULL, 0U, &diagnosed_disassembly));
    CHECK(diagnosed_disassembly.status.compiler_success &&
          !diagnosed_disassembly.status.from_cache &&
          diagnosed_disassembly.status.diagnostic_count == 1U &&
          diagnosed_disassembly.size == sizeof("fake disassembly") - 1U &&
          memcmp(diagnosed_disassembly.text, "fake disassembly",
                 diagnosed_disassembly.size) == 0 &&
          diagnosed_disassembly.text[diagnosed_disassembly.size] == '\0');
    CHECK(strcmp(diagnosed_disassembly.status.diagnostics[0].record,
                 "err: 4 5 6") == 0 &&
          strcmp(diagnosed_disassembly.status.diagnostics[0].file,
                 "Fixture.bin") == 0 &&
          strcmp(diagnosed_disassembly.status.diagnostics[0].message,
                 "fixture disassembly error") == 0);
    unity_compiler_text_response_free(&diagnosed_disassembly);
    CHECK(unity_compiler_disassemble_response(
        &fixture_channel, "error-disassemble", 4, 0, NULL, 0U,
        &diagnosed_disassembly));
    CHECK(!diagnosed_disassembly.status.compiler_success &&
          diagnosed_disassembly.status.diagnostic_count == 1U &&
          diagnosed_disassembly.size == sizeof("fake disassembly") - 1U);
    unity_compiler_text_response_free(&diagnosed_disassembly);
    CHECK(unity_compiler_disassemble(
              &fixture_channel, "success-diagnostic-disassemble", 4, 0,
              NULL, 0U) == NULL);
    CHECK(fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);
    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    CHECK(fixture_channel.process_id == healthy_pid);
    free(fixture_text);
    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success-info-disassemble", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);

    pid_t partial_pid = fixture_channel.process_id;
    CHECK(unity_compiler_disassemble(
              &fixture_channel, "partial", 4, 0, NULL, 0U) == NULL);
    CHECK(fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    errno = 0;
    CHECK(kill(partial_pid, 0) < 0 && errno == ESRCH);
    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);

    CHECK(unity_compiler_disassemble(
              &fixture_channel, "malformed", 4, 0, NULL, 0U) == NULL);
    CHECK(fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);

    size_t large_size = 8U * 1024U * 1024U;
    uint8_t* large_payload = (uint8_t*)calloc(large_size, 1U);
    CHECK(large_payload != NULL);
    CHECK(unity_compiler_disassemble(
              &fixture_channel, "stall-write", 4, 0,
              large_payload, large_size) == NULL);
    free(large_payload);
    CHECK(fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);

    UnityCompilerShaderPreprocessRequest fixture_preprocess = {
        .source = "Shader \"Fixture\" {}",
        .file_path = "Assets/Fixture.shader",
        .shader_name = "primary-false",
        .surface_only = false,
        .caching_preprocessor = true,
        .build_platform = 19U,
        .valid_apis = 295472U,
    };
    healthy_pid = fixture_channel.process_id;
    PreprocessResult fixture_result;

    /* A live session makes valid_apis exact authority, not a subset hint.
     * Reject the near miss before writing a command and preserve stream
     * synchronization so the same process accepts the corrected request. */
    fixture_preprocess.valid_apis =
        FAKE_SESSION_VALID_APIS & ~(UINT32_C(1) << 4U);
    CHECK(!unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_result.snippets == NULL && fixture_result.blob == NULL &&
          fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);
    fixture_preprocess.valid_apis = FAKE_SESSION_VALID_APIS;
    fixture_preprocess.shader_name = "preprocess-success";
    CHECK(unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_result.snippet_count == 0 && fixture_result.blob_len == 0U &&
          fixture_channel.process_id == healthy_pid);
    unity_compiler_free_preprocess(&fixture_result);

    fixture_preprocess.shader_name = "primary-false";
    CHECK(!unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_result.snippets == NULL && fixture_result.blob == NULL);
    CHECK(fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);

    fixture_preprocess.shader_name = "error-preprocess";
    CHECK(!unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);

    fixture_preprocess.shader_name = "unknown-preprocess";
    CHECK(!unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);

    fixture_preprocess.shader_name = "malformed-preprocess";
    CHECK(!unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    fixture_preprocess.shader_name = "preprocess-success";
    CHECK(unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_result.snippet_count == 0 && fixture_result.blob_len == 0U);
    unity_compiler_free_preprocess(&fixture_result);
    healthy_pid = fixture_channel.process_id;

    /* A successful terminal status does not erase preceding diagnostics.
     * Cache replay must reproduce the same diagnosed response, and the
     * compatibility verifier surface must fail closed on both paths. */
    char diagnostic_cache_template[] =
        "/tmp/dxbc_usc_diagnostic_cache.XXXXXX";
    char* diagnostic_cache_dir = mkdtemp(diagnostic_cache_template);
    CHECK(diagnostic_cache_dir != NULL);
    CHECK(setenv("DXBC_USC_CACHE_DIR", diagnostic_cache_dir, 1) == 0);
    fixture_preprocess.shader_name = "success-diagnostic-preprocess";
    UnityCompilerPreprocessResponse diagnosed_preprocess;
    CHECK(unity_compiler_preprocess_contract_response(
        &fixture_channel, &fixture_preprocess, &diagnosed_preprocess));
    CHECK(diagnosed_preprocess.status.compiler_success &&
          !diagnosed_preprocess.status.from_cache &&
          diagnosed_preprocess.status.diagnostic_count == 1U &&
          diagnosed_preprocess.result.snippet_count == 0);
    CHECK(strcmp(diagnosed_preprocess.status.diagnostics[0].record,
                 "err: 10 11 12") == 0 &&
          strcmp(diagnosed_preprocess.status.diagnostics[0].message,
                 "fixture preprocess warning") == 0);
    CHECK(diagnosed_preprocess.has_request_identity);
    uint8_t preprocess_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t preprocess_controls_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    memcpy(preprocess_request_digest, diagnosed_preprocess.request_digest, sizeof(preprocess_request_digest));
    memcpy(preprocess_controls_digest, diagnosed_preprocess.controls_digest, sizeof(preprocess_controls_digest));
    uint8_t *preprocess_identity_transcript = NULL;
    size_t preprocess_identity_transcript_size = 0;
    uint8_t serialized_preprocess_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    CHECK(unity_compiler_serialize_preprocess_request(
        &fixture_channel, &fixture_preprocess, &preprocess_identity_transcript,
        &preprocess_identity_transcript_size, serialized_preprocess_digest));
    CHECK(preprocess_identity_transcript != NULL && preprocess_identity_transcript_size > 0);
    free(preprocess_identity_transcript);
    CHECK(memcmp(serialized_preprocess_digest, preprocess_request_digest,
                 sizeof(preprocess_request_digest)) == 0);
    unity_compiler_preprocess_response_free(&diagnosed_preprocess);
    CHECK(unity_compiler_preprocess_contract_response(
        &fixture_channel, &fixture_preprocess, &diagnosed_preprocess));
    CHECK(diagnosed_preprocess.status.compiler_success &&
          diagnosed_preprocess.status.from_cache &&
          diagnosed_preprocess.status.diagnostic_count == 1U);
    CHECK(diagnosed_preprocess.has_request_identity);
    CHECK(memcmp(diagnosed_preprocess.request_digest, preprocess_request_digest,
                 sizeof(preprocess_request_digest)) == 0);
    CHECK(memcmp(diagnosed_preprocess.controls_digest, preprocess_controls_digest,
                 sizeof(preprocess_controls_digest)) == 0);
    unity_compiler_preprocess_response_free(&diagnosed_preprocess);
    const char *saved_preprocess_source = fixture_preprocess.source;
    fixture_preprocess.source = "Shader \"ChangedSource\" {}";
    CHECK(unity_compiler_preprocess_contract_response(
        &fixture_channel, &fixture_preprocess, &diagnosed_preprocess));
    CHECK(diagnosed_preprocess.has_request_identity);
    CHECK(memcmp(diagnosed_preprocess.request_digest, preprocess_request_digest,
                 sizeof(preprocess_request_digest)) != 0);
    CHECK(memcmp(diagnosed_preprocess.controls_digest, preprocess_controls_digest,
                 sizeof(preprocess_controls_digest)) == 0);
    unity_compiler_preprocess_response_free(&diagnosed_preprocess);
    fixture_preprocess.source = "";
    preprocess_identity_transcript = NULL;
    CHECK(unity_compiler_serialize_preprocess_request(
        &fixture_channel, &fixture_preprocess, &preprocess_identity_transcript,
        &preprocess_identity_transcript_size, serialized_preprocess_digest));
    free(preprocess_identity_transcript);
    CHECK(memcmp(serialized_preprocess_digest, preprocess_controls_digest,
                 sizeof(preprocess_controls_digest)) == 0);
    fixture_preprocess.source = saved_preprocess_source;
    const char *saved_preprocess_path = fixture_preprocess.file_path;
    fixture_preprocess.file_path = "Assets/ChangedIdentity.shader";
    CHECK(unity_compiler_preprocess_contract_response(
        &fixture_channel, &fixture_preprocess, &diagnosed_preprocess));
    CHECK(diagnosed_preprocess.has_request_identity);
    CHECK(memcmp(diagnosed_preprocess.controls_digest, preprocess_controls_digest,
                 sizeof(preprocess_controls_digest)) != 0);
    unity_compiler_preprocess_response_free(&diagnosed_preprocess);
    fixture_preprocess.file_path = saved_preprocess_path;
    CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);
    CHECK(unity_compiler_preprocess_contract_response(
        &fixture_channel, &fixture_preprocess, &diagnosed_preprocess));
    CHECK(diagnosed_preprocess.has_request_identity && !diagnosed_preprocess.status.from_cache);
    CHECK(memcmp(diagnosed_preprocess.request_digest, preprocess_request_digest,
                 sizeof(preprocess_request_digest)) == 0);
    CHECK(memcmp(diagnosed_preprocess.controls_digest, preprocess_controls_digest,
                 sizeof(preprocess_controls_digest)) == 0);
    unity_compiler_preprocess_response_free(&diagnosed_preprocess);
    CHECK(setenv("DXBC_USC_CACHE_DIR", diagnostic_cache_dir, 1) == 0);
    CHECK(!unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_result.snippets == NULL && fixture_result.blob == NULL &&
          fixture_channel.process_id == healthy_pid);
    fixture_preprocess.shader_name = "success-info-preprocess";
    CHECK(unity_compiler_preprocess_contract(
        &fixture_channel, &fixture_preprocess, &fixture_result));
    CHECK(fixture_result.snippet_count == 0 && fixture_result.blob_len == 0U);
    unity_compiler_free_preprocess(&fixture_result);

    /* The compatibility wrappers derive paths from shader names.  Exercise
     * names well beyond the former 1024-byte stack buffers and have the fake
     * compiler reject any byte-for-byte path mismatch. */
    char* long_preprocess_name = make_long_shader_name(
        "verify-long-preprocess-", 4096U);
    CHECK(long_preprocess_name != NULL);
    CHECK(unity_compiler_preprocess(
        &fixture_channel, "Shader \"LongPreprocess\" {}",
        long_preprocess_name, &fixture_result));
    CHECK(fixture_result.snippet_count == 0 && fixture_result.blob_len == 0U);
    unity_compiler_free_preprocess(&fixture_result);
    free(long_preprocess_name);

    CHECK(unity_compiler_preprocess_expanded(
              &fixture_channel, "compile-three-status", "Fixture", 0, 4,
              0U, NULL, 0, NULL, 0) == NULL);
    CHECK(fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    fixture_text = unity_compiler_preprocess_expanded(
        &fixture_channel, "expanded-success", "Fixture", 0, 4, 0U,
        NULL, 0, NULL, 0);
    CHECK(fixture_text && strcmp(fixture_text, "expanded fixture") == 0);
    free(fixture_text);

    char* long_assets_name = make_long_shader_name("Assets/", 4096U);
    CHECK(long_assets_name != NULL);
    fixture_text = unity_compiler_preprocess_expanded(
        &fixture_channel, "verify-long-expanded-path", long_assets_name,
        0, 4, 0U, NULL, 0, NULL, 0);
    CHECK(fixture_text && strcmp(fixture_text, "expanded fixture") == 0);
    free(fixture_text);
    free(long_assets_name);

    size_t fixture_binary_size = 0U;
    char* fixture_error = NULL;
    uint8_t* fixture_binary = unity_compiler_compile(
        &fixture_channel, "reflection-records", "Fixture", 0, 4, 0U,
        NULL, 0, NULL, 0, &fixture_binary_size, &fixture_error);
    CHECK(fixture_binary && fixture_binary_size ==
                                sizeof("expanded fixture") - 1U);
    CHECK(fixture_error == NULL);
    free(fixture_binary);
    healthy_pid = fixture_channel.process_id;

    char* long_package_name = make_long_shader_name("Packages/", 4096U);
    CHECK(long_package_name != NULL);
    fixture_binary = unity_compiler_compile(
        &fixture_channel, "verify-long-compile-path", long_package_name,
        0, 4, 0U, NULL, 0, NULL, 0, &fixture_binary_size, &fixture_error);
    CHECK(fixture_binary && fixture_binary_size ==
                                sizeof("expanded fixture") - 1U);
    CHECK(fixture_error == NULL);
    free(fixture_binary);
    free(long_package_name);

    UnityCompilerSnippetCompileRequest diagnosed_compile_request = {
        .snippet_source = "compile-success-diagnostic",
        .source_directory = "Assets",
        .source_basename = "Diagnosed.shader",
        .pass_name = "Forward",
        .caching_preprocessor = true,
        .preprocess_only = false,
        .strip_line_directives = false,
        .build_platform = 1U,
        .render_state_length = 0,
        .compiler_flags = 0x9000U,
        .shader_type = 0,
        .platform = 4,
        .requirements = parsed_contract.requirements,
        .program_mask = 6,
        .program_start = 0,
        .contract = &parsed_contract,
    };
    UnityCompilerSnippetCompileRequest reflected_compile_request =
        diagnosed_compile_request;
    reflected_compile_request.snippet_source = "reflection-records";
    UnityCompilerBinaryResponse reflected_compile;
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &reflected_compile_request, &reflected_compile));
    CHECK(reflected_compile.status.compiler_success &&
          reflected_compile.reflection_record_count == 9U &&
          reflected_compile.reflection_records[0].kind ==
              UNITY_COMPILER_REFLECTION_INPUT &&
          reflected_compile.reflection_records[4].kind ==
              UNITY_COMPILER_REFLECTION_TEXTURE_BINDING &&
          strcmp(reflected_compile.reflection_records[4].name,
                 "MainTex") == 0 &&
          reflected_compile.reflection_records[8].kind ==
              UNITY_COMPILER_REFLECTION_STATS);
    unity_compiler_binary_response_free(&reflected_compile);
    UnityCompilerBinaryResponse diagnosed_compile;
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &diagnosed_compile_request, &diagnosed_compile));
    static const uint8_t expected_diagnosed_binary[] = {0x00, 0xff, 0x41};
    CHECK(diagnosed_compile.status.compiler_success &&
          !diagnosed_compile.status.from_cache &&
          diagnosed_compile.status.diagnostic_count == 1U &&
          diagnosed_compile.size == sizeof(expected_diagnosed_binary) &&
          memcmp(diagnosed_compile.data, expected_diagnosed_binary,
                 sizeof(expected_diagnosed_binary)) == 0);
    uint8_t *identity_transcript = NULL;
    size_t identity_transcript_size = 0;
    uint8_t identity_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t controls_digest[UNITY_COMPILER_FINGERPRINT_SIZE];
    CHECK(unity_compiler_serialize_compile_request(&fixture_channel, &diagnosed_compile_request,
                                                   &identity_transcript, &identity_transcript_size,
                                                   identity_digest));
    free(identity_transcript);
    CHECK(diagnosed_compile.has_request_identity &&
          memcmp(identity_digest, diagnosed_compile.request_digest, sizeof(identity_digest)) == 0);
    memcpy(controls_digest, diagnosed_compile.controls_digest, sizeof(controls_digest));
    CHECK(strcmp(diagnosed_compile.status.diagnostics[0].record,
                 "err: 13 14 15") == 0 &&
          strcmp(diagnosed_compile.status.diagnostics[0].message,
                 "fixture compile warning") == 0);
    unity_compiler_binary_response_free(&diagnosed_compile);
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &diagnosed_compile_request, &diagnosed_compile));
    CHECK(diagnosed_compile.has_request_identity &&
          memcmp(identity_digest, diagnosed_compile.request_digest, sizeof(identity_digest)) == 0 &&
          memcmp(controls_digest, diagnosed_compile.controls_digest, sizeof(controls_digest)) == 0);
    CHECK(diagnosed_compile.status.compiler_success &&
          diagnosed_compile.status.from_cache &&
          diagnosed_compile.status.diagnostic_count == 1U &&
          diagnosed_compile.size == sizeof(expected_diagnosed_binary) &&
          memcmp(diagnosed_compile.data, expected_diagnosed_binary,
                 sizeof(expected_diagnosed_binary)) == 0);
    unity_compiler_binary_response_free(&diagnosed_compile);

    UnityCompilerSnippetCompileRequest changed_identity = diagnosed_compile_request;
    changed_identity.snippet_source = "compile-success-info";
    CHECK(unity_compiler_compile_contract_response(&fixture_channel, &changed_identity,
                                                   &diagnosed_compile));
    CHECK(diagnosed_compile.has_request_identity &&
          memcmp(identity_digest, diagnosed_compile.request_digest, sizeof(identity_digest)) != 0 &&
          memcmp(controls_digest, diagnosed_compile.controls_digest, sizeof(controls_digest)) == 0);
    unity_compiler_binary_response_free(&diagnosed_compile);
    changed_identity.source_basename = "ChangedIdentity.shader";
    CHECK(unity_compiler_compile_contract_response(&fixture_channel, &changed_identity,
                                                   &diagnosed_compile));
    CHECK(diagnosed_compile.has_request_identity &&
          memcmp(controls_digest, diagnosed_compile.controls_digest, sizeof(controls_digest)) != 0);
    unity_compiler_binary_response_free(&diagnosed_compile);

    /* The channel-wide expected validApis authority covers compileSnippet,
     * which has no validApis field of its own.  A mismatch on an already
     * synchronized process must be a typed local rejection before even the
     * command string is written.  The tripwire fake request would close the
     * stream if it arrived, so same-PID recovery also proves zero command
     * bytes reached the fake compiler. */
    UnityCompilerValidApisAuthority valid_apis_authority;
    CHECK(unity_compiler_expected_valid_apis_authority(
        &fixture_channel, &valid_apis_authority));
    CHECK(valid_apis_authority.status ==
          UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED);
    CHECK(!unity_compiler_set_expected_valid_apis(
        &fixture_channel,
        UNITY_COMPILER_PLATFORM_MASK | UINT32_C(0x80000000)));
    const uint32_t mismatched_valid_apis =
        FAKE_SESSION_VALID_APIS & ~(UINT32_C(1) << 4U);
    CHECK(unity_compiler_set_expected_valid_apis(
        &fixture_channel, mismatched_valid_apis));
    CHECK(unity_compiler_expected_valid_apis_authority(
        &fixture_channel, &valid_apis_authority));
    CHECK(valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH &&
          valid_apis_authority.expected_valid_apis ==
              mismatched_valid_apis &&
          valid_apis_authority.observed_valid_apis ==
              FAKE_SESSION_VALID_APIS);
    UnityCompilerSnippetCompileRequest authority_compile_request =
        diagnosed_compile_request;
    authority_compile_request.snippet_source =
        "authority-mismatch-must-not-be-sent";
    const pid_t authority_guarded_pid = fixture_channel.process_id;
    UnityCompilerBinaryResponse authority_compile_response;
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &authority_compile_request,
        &authority_compile_response));
    CHECK(authority_compile_response.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH &&
          !authority_compile_response.status.compiler_success &&
          authority_compile_response.data == NULL &&
          authority_compile_response.size == 0U &&
          fixture_channel.process_id == authority_guarded_pid &&
          fixture_channel.socket_fd >= 0);
    char* authority_message = unity_compiler_response_status_format(
        &authority_compile_response.status, "wrong fallback");
    CHECK(authority_message != NULL &&
          strstr(authority_message, "validApis authority mismatch") != NULL &&
          strstr(authority_message, "expected 295456") != NULL &&
          strstr(authority_message, "returned 295472") != NULL);
    free(authority_message);
    unity_compiler_binary_response_free(&authority_compile_response);

    CHECK(unity_compiler_set_expected_valid_apis(
        &fixture_channel, FAKE_SESSION_VALID_APIS));
    authority_compile_request.snippet_source = "authority-match";
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &authority_compile_request,
        &authority_compile_response));
    CHECK(authority_compile_response.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED &&
          authority_compile_response.status.compiler_success &&
          fixture_channel.process_id == authority_guarded_pid);
    unity_compiler_binary_response_free(&authority_compile_response);

    /* Process retirement retains both the expected value and the immutable
     * observed session record.  The replacement revalidates both after its
     * own initializeCompiler exchange before accepting work. */
    CHECK(unity_compiler_recycle_process(&fixture_channel));
    CHECK(fixture_channel.process_id == 0 &&
          fixture_channel.socket_fd == -1);
    CHECK(unity_compiler_expected_valid_apis_authority(
        &fixture_channel, &valid_apis_authority));
    CHECK(valid_apis_authority.status ==
          UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED);
    authority_compile_request.snippet_source = "authority-after-recycle";
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &authority_compile_request,
        &authority_compile_response));
    CHECK(authority_compile_response.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED &&
          authority_compile_response.status.compiler_success &&
          fixture_channel.process_id > 0 &&
          fixture_channel.process_id != authority_guarded_pid);
    unity_compiler_binary_response_free(&authority_compile_response);
    CHECK(unity_compiler_clear_expected_valid_apis(&fixture_channel));
    healthy_pid = fixture_channel.process_id;

    /* Reproduce the original authority hole exactly: preprocess is a warm
     * persistent-cache hit on a brand-new process-free channel, while the
     * following compile is cold.  The cold compile may initialize the fake
     * compiler, but a mismatching initializeCompiler mask terminates it before
     * the compileSnippet tripwire command is sent. */
    UnityCompilerShaderPreprocessRequest authority_warm_preprocess =
        fixture_preprocess;
    authority_warm_preprocess.shader_name =
        "success-diagnostic-preprocess";
    UnityCompilerChannel warm_preprocess_cold_compile_channel;
    CHECK(unity_compiler_start_lazy(
        &warm_preprocess_cold_compile_channel, temporary_dir,
        builtin_includes));
    CHECK(unity_compiler_set_expected_valid_apis(
        &warm_preprocess_cold_compile_channel, mismatched_valid_apis));
    UnityCompilerPreprocessResponse authority_preprocess_response;
    CHECK(unity_compiler_preprocess_contract_response(
        &warm_preprocess_cold_compile_channel, &authority_warm_preprocess,
        &authority_preprocess_response));
    CHECK(authority_preprocess_response.status.from_cache &&
          authority_preprocess_response.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING &&
          warm_preprocess_cold_compile_channel.process_id == 0 &&
          warm_preprocess_cold_compile_channel.socket_fd == -1);
    unity_compiler_preprocess_response_free(
        &authority_preprocess_response);
    authority_compile_request.snippet_source =
        "authority-mismatch-must-not-be-sent";
    CHECK(unity_compiler_compile_contract_response(
        &warm_preprocess_cold_compile_channel, &authority_compile_request,
        &authority_compile_response));
    CHECK(authority_compile_response.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH &&
          authority_compile_response.status.valid_apis_authority
                  .observed_valid_apis == FAKE_SESSION_VALID_APIS &&
          !authority_compile_response.status.compiler_success &&
          warm_preprocess_cold_compile_channel.process_id == 0 &&
          warm_preprocess_cold_compile_channel.socket_fd == -1);
    unity_compiler_binary_response_free(&authority_compile_response);
    unity_compiler_shutdown(&warm_preprocess_cold_compile_channel);

    /* Cache-only hits and misses never launch a process merely to turn the
     * PENDING state into MATCHED. */
    UnityCompilerChannel cache_only_authority_channel;
    CHECK(unity_compiler_start_lazy(
        &cache_only_authority_channel, temporary_dir, builtin_includes));
    CHECK(unity_compiler_set_expected_valid_apis(
        &cache_only_authority_channel, FAKE_SESSION_VALID_APIS));
    CHECK(setenv("DXBC_USC_CACHE_ONLY", "1", 1) == 0);
    CHECK(unity_compiler_preprocess_contract_response(
        &cache_only_authority_channel, &authority_warm_preprocess,
        &authority_preprocess_response));
    CHECK(authority_preprocess_response.status.from_cache &&
          authority_preprocess_response.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING &&
          cache_only_authority_channel.process_id == 0 &&
          cache_only_authority_channel.socket_fd == -1);
    unity_compiler_preprocess_response_free(
        &authority_preprocess_response);
    authority_compile_request.snippet_source =
        "authority-cache-only-cold-compile";
    CHECK(unity_compiler_compile_contract_response(
        &cache_only_authority_channel, &authority_compile_request,
        &authority_compile_response));
    CHECK(authority_compile_response.status.availability ==
              UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS &&
          authority_compile_response.status.valid_apis_authority.status ==
              UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING &&
          cache_only_authority_channel.process_id == 0 &&
          cache_only_authority_channel.socket_fd == -1);
    unity_compiler_binary_response_free(&authority_compile_response);
    CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);
    unity_compiler_shutdown(&cache_only_authority_channel);

    /* Cache-only mode is a typed offline lookup. A hit remains the exact
     * stored compiler response; a miss is neither a compiler rejection nor a
     * transport failure and must leave the persistent child untouched. */
    CHECK(setenv("DXBC_USC_CACHE_ONLY", "1", 1) == 0);
    pid_t cache_only_pid = fixture_channel.process_id;
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &diagnosed_compile_request, &diagnosed_compile));
    CHECK(diagnosed_compile.status.availability ==
              UNITY_COMPILER_RESPONSE_AVAILABLE &&
          diagnosed_compile.status.compiler_success &&
          diagnosed_compile.status.from_cache &&
          fixture_channel.process_id == cache_only_pid);
    unity_compiler_binary_response_free(&diagnosed_compile);

    UnityCompilerSnippetCompileRequest cache_only_compile_miss =
        diagnosed_compile_request;
    cache_only_compile_miss.snippet_source = "cache-only-compile-miss";
    CHECK(unity_compiler_compile_contract_response(
        &fixture_channel, &cache_only_compile_miss, &diagnosed_compile));
    CHECK(diagnosed_compile.status.availability ==
              UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS &&
          !diagnosed_compile.status.compiler_success &&
          !diagnosed_compile.status.from_cache &&
          diagnosed_compile.status.diagnostic_count == 0U &&
          diagnosed_compile.data == NULL && diagnosed_compile.size == 0U &&
          fixture_channel.process_id == cache_only_pid);
    char* cache_only_message = unity_compiler_response_status_format(
        &diagnosed_compile.status, "wrong fallback");
    CHECK(cache_only_message && strstr(cache_only_message,
                                       "cache-only lookup missed") != NULL);
    free(cache_only_message);
    unity_compiler_binary_response_free(&diagnosed_compile);

    fixture_preprocess.shader_name = "cache-only-preprocess-miss";
    CHECK(unity_compiler_preprocess_contract_response(
        &fixture_channel, &fixture_preprocess, &diagnosed_preprocess));
    CHECK(diagnosed_preprocess.status.availability ==
              UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS &&
          !diagnosed_preprocess.status.compiler_success &&
          !diagnosed_preprocess.status.from_cache &&
          diagnosed_preprocess.status.diagnostic_count == 0U &&
          diagnosed_preprocess.result.snippets == NULL &&
          diagnosed_preprocess.result.blob == NULL &&
          fixture_channel.process_id == cache_only_pid);
    unity_compiler_preprocess_response_free(&diagnosed_preprocess);
    CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);

    fixture_binary = unity_compiler_compile_contract(
        &fixture_channel, &diagnosed_compile_request, &fixture_binary_size,
        &fixture_error);
    CHECK(fixture_binary == NULL && fixture_binary_size == 0U &&
          fixture_error && strstr(fixture_error, "err: 13 14 15") &&
          strstr(fixture_error, "fixture compile warning"));
    free(fixture_error);
    fixture_error = NULL;
    CHECK(fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);

    UnityCompilerSnippetCompileRequest informational_compile_request =
        diagnosed_compile_request;
    informational_compile_request.snippet_source = "compile-success-info";
    fixture_binary = unity_compiler_compile_contract(
        &fixture_channel, &informational_compile_request,
        &fixture_binary_size, &fixture_error);
    CHECK(fixture_binary != NULL && fixture_error == NULL &&
          fixture_binary_size == sizeof("expanded fixture") - 1U);
    free(fixture_binary);

    UnityCompilerBinaryResponse legacy_response;
    CHECK(unity_compiler_compile_response(
        &fixture_channel, "compile-error-record", "Fixture", 0, 4, 0U,
        NULL, 0, NULL, 0, &legacy_response));
    CHECK(legacy_response.status.availability == UNITY_COMPILER_RESPONSE_AVAILABLE);
    CHECK(!legacy_response.status.compiler_success &&
          legacy_response.status.diagnostic_count == 1U);
    CHECK(strstr(legacy_response.status.diagnostics[0].message,
                 "fixture compile error"));
    unity_compiler_binary_response_free(&legacy_response);
    CHECK(setenv("DXBC_USC_CACHE_ONLY", "1", 1) == 0);
    CHECK(unity_compiler_compile_response(
        &fixture_channel, "legacy-uncached-source", "Fixture", 0, 4, 0U,
        NULL, 0, NULL, 0, &legacy_response));
    CHECK(legacy_response.status.availability == UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS);
    CHECK(legacy_response.status.diagnostic_count == 0U && legacy_response.has_request_identity);
    unity_compiler_binary_response_free(&legacy_response);
    CHECK(!legacy_response.has_request_identity);
    CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);

    unity_compiler_binary_response_init(&legacy_response);
    legacy_response.status.compiler_success = true;
    legacy_response.size = 10U; /* Invalid ownership must never yield dummy bytes. */
    CHECK(!unity_compiler_binary_response_take_clean_data(
        &legacy_response, &fixture_binary_size, &fixture_error));
    CHECK(fixture_binary_size == 0U && fixture_error);
    free(fixture_error);
    fixture_error = NULL;

    fixture_binary = unity_compiler_compile(
        &fixture_channel, "compile-error-record", "Fixture", 0, 4, 0U,
        NULL, 0, NULL, 0, &fixture_binary_size, &fixture_error);
    CHECK(fixture_binary == NULL && fixture_binary_size == 0U);
    CHECK(fixture_error && strstr(fixture_error, "err: 7 8 9") &&
          strstr(fixture_error, "Assets/Fixture.shader") &&
          strstr(fixture_error, "fixture compile error"));
    free(fixture_error);
    fixture_error = NULL;
    CHECK(fixture_channel.process_id == healthy_pid &&
          fixture_channel.socket_fd >= 0);

    fixture_binary = unity_compiler_compile(
        &fixture_channel, "unknown-structured-record", "Fixture", 0, 4,
        0U, NULL, 0, NULL, 0, &fixture_binary_size, &fixture_error);
    CHECK(fixture_binary == NULL && fixture_error != NULL &&
          strstr(fixture_error, "transport or protocol failure"));
    free(fixture_error);
    fixture_error = NULL;
    CHECK(fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "success", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);

    fixture_text = unity_compiler_disassemble(
        &fixture_channel, "ignore-shutdown", 4, 0, NULL, 0U);
    CHECK(fixture_text && strcmp(fixture_text, "fake disassembly") == 0);
    free(fixture_text);
    pid_t shutdown_pid = fixture_channel.process_id;
    unity_compiler_shutdown(&fixture_channel);
    CHECK(!fixture_channel.configured && fixture_channel.socket_fd == -1 &&
          fixture_channel.process_id == 0);
    errno = 0;
    CHECK(kill(shutdown_pid, 0) < 0 && errno == ESRCH);

    CHECK(unsetenv("DXBC_USC_FAKE_SERVER") == 0);
    CHECK(unsetenv("DXBC_USC_FAKE_INIT_MODE") == 0);
    CHECK(unsetenv("DXBC_USC_IO_TIMEOUT_MS") == 0);
    CHECK(unsetenv("TMPDIR") == 0);
    CHECK(unsetenv("DYLD_LIBRARY_PATH") == 0);
    CHECK(unsetenv("LD_LIBRARY_PATH") == 0);
    CHECK(unsetenv("DYLD_INSERT_LIBRARIES") == 0);
    CHECK(unsetenv("PROXY_DYLD_INSERT_LIBRARIES") == 0);
    if (saved_transport_cache) {
        CHECK(setenv("DXBC_USC_CACHE_DIR", saved_transport_cache, 1) == 0);
    } else {
        CHECK(unsetenv("DXBC_USC_CACHE_DIR") == 0);
    }
    free(saved_transport_cache);
    if (saved_cache_only) {
        CHECK(setenv("DXBC_USC_CACHE_ONLY", saved_cache_only, 1) == 0);
    } else {
        CHECK(unsetenv("DXBC_USC_CACHE_ONLY") == 0);
    }
    free(saved_cache_only);

    remove_tree(diagnostic_cache_dir);
    remove_tree(temporary_dir);
    unity_compiler_snippet_contract_free(&parsed_contract);

    puts("compiler client unit tests passed");
    return 0;
}
