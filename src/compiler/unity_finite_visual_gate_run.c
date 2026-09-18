// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "compiler/unity_finite_visual_gate.h"

#include "common/file_io.h"
#include "common/sha256.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define FINITE_VISUAL_MAX_SOURCE (UINT64_C(512) * 1024U * 1024U)
#define FINITE_VISUAL_MAX_FIXTURE (UINT64_C(256) * 1024U * 1024U)
#define FINITE_VISUAL_MAX_REPORT (UINT64_C(128) * 1024U * 1024U)
#define FINITE_VISUAL_MAX_LOG (UINT64_C(512) * 1024U * 1024U)

void unity_finite_visual_gate_compute_requested_coordinates(
    UnityFiniteVisualGateSummary* summary, const char* expected_version,
    UnityFiniteVisualBackend expected_backend);

typedef struct {
    CommonFileBytes baseline;
    CommonFileBytes candidate;
    CommonFileBytes fixture;
    CommonFileBytes bridge;
    char baseline_sha[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char candidate_sha[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char fixture_sha[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    char bridge_sha[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
} InputFiles;

static bool write_file(const char* path, const void* data, size_t size) {
    return common_file_write_new_atomic(path, data, size) == COMMON_FILE_OK;
}

static char* join_path(const char* parent, const char* child) {
    if (!parent || !child) return NULL;
    const size_t parent_size = strlen(parent);
    const size_t child_size = strlen(child);
    const bool separator = parent_size != 0U &&
        parent[parent_size - 1U] != '/' && parent[parent_size - 1U] != '\\';
    if (parent_size > SIZE_MAX - child_size - (separator ? 2U : 1U)) {
        return NULL;
    }
    char* path = (char*)malloc(parent_size + child_size +
                              (separator ? 2U : 1U));
    if (!path) return NULL;
    memcpy(path, parent, parent_size);
    size_t next = parent_size;
    if (separator) path[next++] = '/';
    memcpy(path + next, child, child_size + 1U);
    return path;
}

#ifdef _WIN32
static bool make_directory(const char* path) {
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return false;
    bool success = CreateDirectoryW(wide, NULL) != 0;
    free(wide);
    return success;
}

static bool path_exists(const char* path) {
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return true;
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

static char* absolute_path(const char* path) {
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return NULL;
    DWORD needed = GetFullPathNameW(wide, 0U, NULL, NULL);
    if (needed == 0U) {
        free(wide);
        return NULL;
    }
    wchar_t* absolute = (wchar_t*)malloc((size_t)needed * sizeof(*absolute));
    if (!absolute) {
        free(wide);
        return NULL;
    }
    DWORD written = GetFullPathNameW(wide, needed, absolute, NULL);
    free(wide);
    if (written == 0U || written >= needed) {
        free(absolute);
        return NULL;
    }
    char* utf8 = common_windows_wide_to_utf8(absolute);
    free(absolute);
    return utf8;
}

static char* make_temporary_workspace(const char* requested_root) {
    char* root = NULL;
    if (requested_root) {
        root = absolute_path(requested_root);
    } else {
        DWORD size = GetTempPathW(0U, NULL);
        if (size == 0U) return NULL;
        wchar_t* temporary = (wchar_t*)malloc((size_t)size * sizeof(*temporary));
        if (!temporary) return NULL;
        if (GetTempPathW(size, temporary) == 0U) {
            free(temporary);
            return NULL;
        }
        root = common_windows_wide_to_utf8(temporary);
        free(temporary);
    }
    if (!root) return NULL;
    for (unsigned attempt = 0U; attempt < 256U; ++attempt) {
        char name[96];
        int length = snprintf(name, sizeof(name),
            "dxbc-finite-visual-%lu-%u", (unsigned long)GetCurrentProcessId(),
            attempt);
        char* candidate = length > 0 && (size_t)length < sizeof(name)
            ? join_path(root, name) : NULL;
        if (!candidate) {
            free(root);
            return NULL;
        }
        if (make_directory(candidate)) {
            free(root);
            return candidate;
        }
        free(candidate);
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    free(root);
    return NULL;
}

static bool remove_tree_wide(const wchar_t* path) {
    size_t length = wcslen(path);
    wchar_t* pattern = (wchar_t*)malloc((length + 3U) * sizeof(*pattern));
    if (!pattern) return false;
    memcpy(pattern, path, length * sizeof(*pattern));
    pattern[length] = L'\\';
    pattern[length + 1U] = L'*';
    pattern[length + 2U] = L'\0';
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    free(pattern);
    bool success = true;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) continue;
            size_t child_size = length + wcslen(data.cFileName) + 2U;
            wchar_t* child = (wchar_t*)malloc(child_size * sizeof(*child));
            if (!child) { success = false; break; }
            (void)swprintf(child, child_size, L"%ls\\%ls", path,
                           data.cFileName);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U) {
                if (!remove_tree_wide(child)) success = false;
            } else if (!DeleteFileW(child)) {
                success = false;
            }
            free(child);
        } while (success && FindNextFileW(search, &data));
        (void)FindClose(search);
    }
    return success && RemoveDirectoryW(path) != 0;
}

static bool remove_tree(const char* path) {
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return false;
    bool success = remove_tree_wide(wide);
    free(wide);
    return success;
}

static bool remove_file(const char* path) {
    wchar_t* wide = common_windows_utf8_to_wide(path);
    if (!wide) return false;
    bool success = DeleteFileW(wide) != 0;
    free(wide);
    return success;
}
#else
static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    char* copy = (char*)malloc(size + 1U);
    if (copy) memcpy(copy, value, size + 1U);
    return copy;
}

static bool make_directory(const char* path) {
    return mkdir(path, 0700) == 0;
}

static bool path_exists(const char* path) {
    struct stat information;
    return lstat(path, &information) == 0 || errno != ENOENT;
}

static char* absolute_path(const char* path) {
    if (!path || !path[0]) return NULL;
    if (path[0] == '/') return duplicate_string(path);
    char* working = getcwd(NULL, 0U);
    if (!working) return NULL;
    char* result = join_path(working, path);
    free(working);
    return result;
}

static char* make_temporary_workspace(const char* requested_root) {
    const char* root = requested_root ? requested_root :
        (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    char* absolute = absolute_path(root);
    char* pattern = absolute ? join_path(absolute, "dxbc-finite-visual.XXXXXX")
                             : NULL;
    free(absolute);
    if (!pattern) return NULL;
    int descriptor = mkstemp(pattern);
    if (descriptor < 0) {
        free(pattern);
        return NULL;
    }
    const bool created = close(descriptor) == 0 && unlink(pattern) == 0 &&
        make_directory(pattern);
    if (!created) {
        (void)unlink(pattern);
        free(pattern);
        return NULL;
    }
    return pattern;
}

static bool remove_tree(const char* path) {
    struct stat information;
    if (lstat(path, &information) != 0) return false;
    if (!S_ISDIR(information.st_mode)) return unlink(path) == 0;
    DIR* directory = opendir(path);
    if (!directory) return false;
    bool success = true;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        char* child = join_path(path, entry->d_name);
        if (!child || !remove_tree(child)) success = false;
        free(child);
        if (!success) break;
    }
    if (closedir(directory) != 0) success = false;
    return success && rmdir(path) == 0;
}

static bool remove_file(const char* path) {
    return unlink(path) == 0;
}
#endif

static char hex_digit(unsigned value) {
    static const char digits[] = "0123456789abcdef";
    return digits[value & 15U];
}

static void digest_hex(const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
                       char output[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY]) {
    for (size_t index = 0U; index < COMMON_SHA256_DIGEST_SIZE; ++index) {
        output[index * 2U] = hex_digit(digest[index] >> 4U);
        output[index * 2U + 1U] = hex_digit(digest[index]);
    }
    output[64] = '\0';
}

static void hash_file_bytes(const CommonFileBytes* file,
                            char output[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY]) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(file->data, file->size, digest);
    digest_hex(digest, output);
}

static UnityFiniteVisualGateStatus read_inputs(
    const UnityFiniteVisualGateOptions* options, InputFiles* files) {
    memset(files, 0, sizeof(*files));
    CommonFileStatus status = common_file_read_regular(
        options->baseline_shader_path, (size_t)FINITE_VISUAL_MAX_SOURCE,
        &files->baseline);
    if (status != COMMON_FILE_OK) goto failed;
    status = common_file_read_regular(
        options->candidate_shader_path, (size_t)FINITE_VISUAL_MAX_SOURCE,
        &files->candidate);
    if (status != COMMON_FILE_OK) goto failed;
    status = common_file_read_regular(
        options->fixture_path, (size_t)FINITE_VISUAL_MAX_FIXTURE,
        &files->fixture);
    if (status != COMMON_FILE_OK) goto failed;
    status = common_file_read_regular(
        options->bridge_path, (size_t)FINITE_VISUAL_MAX_SOURCE,
        &files->bridge);
    if (status != COMMON_FILE_OK) goto failed;
    if (files->baseline.size == 0U || files->candidate.size == 0U ||
        files->fixture.size == 0U || files->bridge.size == 0U) {
        status = COMMON_FILE_INVALID_ARGUMENT;
        goto failed;
    }
    hash_file_bytes(&files->baseline, files->baseline_sha);
    hash_file_bytes(&files->candidate, files->candidate_sha);
    hash_file_bytes(&files->fixture, files->fixture_sha);
    hash_file_bytes(&files->bridge, files->bridge_sha);
    return UNITY_FINITE_VISUAL_GATE_OK;
failed:
    common_file_bytes_dispose(&files->baseline);
    common_file_bytes_dispose(&files->candidate);
    common_file_bytes_dispose(&files->fixture);
    common_file_bytes_dispose(&files->bridge);
    return status == COMMON_FILE_ALLOCATION_FAILED
        ? UNITY_FINITE_VISUAL_GATE_ALLOCATION_FAILED
        : UNITY_FINITE_VISUAL_GATE_IO_FAILED;
}

static void input_files_dispose(InputFiles* files) {
    if (!files) return;
    common_file_bytes_dispose(&files->baseline);
    common_file_bytes_dispose(&files->candidate);
    common_file_bytes_dispose(&files->fixture);
    common_file_bytes_dispose(&files->bridge);
}

static void role_guid(const char* role, const CommonFileBytes* bytes,
                      char output[33]) {
    CommonSha256Context hash;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    static const char domain[] = "DXBCSandbox finite visual asset guid/v1";
    common_sha256_init(&hash);
    common_sha256_update(&hash, domain, sizeof(domain) - 1U);
    common_sha256_update(&hash, role, strlen(role));
    common_sha256_update(&hash, bytes->data, bytes->size);
    common_sha256_final(&hash, digest);
    for (size_t index = 0U; index < 16U; ++index) {
        output[index * 2U] = hex_digit(digest[index] >> 4U);
        output[index * 2U + 1U] = hex_digit(digest[index]);
    }
    output[32] = '\0';
}

static bool write_shader_meta(const char* path, const char* role,
                              const CommonFileBytes* source) {
    char guid[33];
    role_guid(role, source, guid);
    char text[512];
    int size = snprintf(text, sizeof(text),
        "fileFormatVersion: 2\n"
        "guid: %s\n"
        "ShaderImporter:\n"
        "  externalObjects: {}\n"
        "  defaultTextures: []\n"
        "  nonModifiableTextures: []\n"
        "  userData: \n"
        "  assetBundleName: \n"
        "  assetBundleVariant: \n", guid);
    return size > 0 && (size_t)size < sizeof(text) &&
        write_file(path, text, (size_t)size);
}

static bool prepare_project(const UnityFiniteVisualGateOptions* options,
                            const InputFiles* files, const char* workspace,
                            char** out_fixture, char** out_result,
                            char** out_baseline_pixels,
                            char** out_candidate_pixels) {
    char* assets = join_path(workspace, "Assets");
    char* editor = assets ? join_path(assets, "Editor") : NULL;
    char* lane = assets ? join_path(assets, "DXBCFiniteVisual") : NULL;
    char* settings = join_path(workspace, "ProjectSettings");
    char* packages = join_path(workspace, "Packages");
    bool success = assets && editor && lane && settings && packages &&
        make_directory(assets) && make_directory(editor) &&
        make_directory(lane) && make_directory(settings) &&
        make_directory(packages);
    char* bridge = editor ? join_path(editor, "DXBCFiniteVisualGate.cs") : NULL;
    char* baseline = lane ? join_path(lane, "Baseline.shader") : NULL;
    char* baseline_meta = lane ? join_path(lane, "Baseline.shader.meta") : NULL;
    char* candidate = lane ? join_path(lane, "Candidate.shader") : NULL;
    char* candidate_meta = lane ? join_path(lane, "Candidate.shader.meta") : NULL;
    char* fixture = join_path(workspace, "finite-visual-fixture.tsv");
    char* project_version = settings ?
        join_path(settings, "ProjectVersion.txt") : NULL;
    char* package_manifest = packages ? join_path(packages, "manifest.json") : NULL;
    char* result = join_path(workspace, "finite-visual-result.tsv");
    char* baseline_pixels = join_path(workspace, "baseline.rgba32f");
    char* candidate_pixels = join_path(workspace, "candidate.rgba32f");
    if (!bridge || !baseline || !baseline_meta || !candidate ||
        !candidate_meta || !fixture || !project_version || !package_manifest ||
        !result || !baseline_pixels || !candidate_pixels) success = false;
    if (success) success = write_file(bridge, files->bridge.data,
                                      files->bridge.size);
    if (success) success = write_file(baseline, files->baseline.data,
                                      files->baseline.size);
    if (success) success = write_shader_meta(baseline_meta, "baseline",
                                             &files->baseline);
    if (success) success = write_file(candidate, files->candidate.data,
                                      files->candidate.size);
    if (success) success = write_shader_meta(candidate_meta, "candidate",
                                             &files->candidate);
    if (success) success = write_file(fixture, files->fixture.data,
                                      files->fixture.size);
    if (success) {
        const size_t version_size = strlen(options->expected_unity_version);
        const size_t capacity = version_size + 64U;
        char* text = (char*)malloc(capacity);
        if (!text) {
            success = false;
        } else {
            int length = snprintf(text, capacity, "m_EditorVersion: %s\n",
                                  options->expected_unity_version);
            success = length > 0 && (size_t)length < capacity &&
                write_file(project_version, text, (size_t)length);
            free(text);
        }
    }
    static const char manifest[] = "{\n  \"dependencies\": {}\n}\n";
    if (success) success = write_file(package_manifest, manifest,
                                      sizeof(manifest) - 1U);
    free(assets);
    free(editor);
    free(lane);
    free(settings);
    free(packages);
    free(bridge);
    free(baseline);
    free(baseline_meta);
    free(candidate);
    free(candidate_meta);
    free(project_version);
    free(package_manifest);
    if (!success) {
        free(fixture);
        free(result);
        free(baseline_pixels);
        free(candidate_pixels);
        return false;
    }
    *out_fixture = fixture;
    *out_result = result;
    *out_baseline_pixels = baseline_pixels;
    *out_candidate_pixels = candidate_pixels;
    return true;
}

#ifdef _WIN32
static bool append_windows_argument(char** command, size_t* size,
                                    size_t* capacity, const char* argument) {
    size_t needed = 3U;
    for (const char* at = argument; *at; ++at) {
        needed += (*at == '"' || *at == '\\') ? 2U : 1U;
    }
    if (*size > SIZE_MAX - needed) return false;
    size_t target = *size + needed;
    if (target > *capacity) {
        size_t grown = *capacity == 0U ? 256U : *capacity;
        while (grown < target) {
            if (grown > SIZE_MAX / 2U) { grown = target; break; }
            grown *= 2U;
        }
        char* allocation = (char*)realloc(*command, grown);
        if (!allocation) return false;
        *command = allocation;
        *capacity = grown;
    }
    if (*size != 0U) (*command)[(*size)++] = ' ';
    (*command)[(*size)++] = '"';
    size_t slashes = 0U;
    for (const char* at = argument;; ++at) {
        if (*at == '\\') { ++slashes; continue; }
        if (*at == '"') {
            while (slashes-- != 0U) {
                (*command)[(*size)++] = '\\';
                (*command)[(*size)++] = '\\';
            }
            (*command)[(*size)++] = '\\';
            (*command)[(*size)++] = '"';
            slashes = 0U;
            continue;
        }
        if (*at == '\0') {
            while (slashes != 0U) {
                (*command)[(*size)++] = '\\';
                (*command)[(*size)++] = '\\';
                --slashes;
            }
            break;
        }
        while (slashes-- != 0U) (*command)[(*size)++] = '\\';
        slashes = 0U;
        (*command)[(*size)++] = *at;
    }
    (*command)[(*size)++] = '"';
    (*command)[*size] = '\0';
    return true;
}

static bool launch_unity(const char* const* arguments, int* exit_code) {
    char* command = NULL;
    size_t size = 0U;
    size_t capacity = 0U;
    for (size_t index = 0U; arguments[index]; ++index) {
        if (!append_windows_argument(&command, &size, &capacity,
                                     arguments[index])) {
            free(command);
            return false;
        }
    }
    wchar_t* application = common_windows_utf8_to_wide(arguments[0]);
    wchar_t* wide_command = common_windows_utf8_to_wide(command);
    free(command);
    if (!application || !wide_command) {
        free(application);
        free(wide_command);
        return false;
    }
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    BOOL created = CreateProcessW(application, wide_command, NULL, NULL,
                                  FALSE, 0U, NULL, NULL, &startup, &process);
    free(application);
    free(wide_command);
    if (!created) return false;
    bool success = WaitForSingleObject(process.hProcess, INFINITE) ==
        WAIT_OBJECT_0;
    DWORD code = 0U;
    if (success) success = GetExitCodeProcess(process.hProcess, &code) != 0;
    (void)CloseHandle(process.hThread);
    (void)CloseHandle(process.hProcess);
    if (!success || code > INT_MAX) return false;
    *exit_code = (int)code;
    return true;
}
#else
static bool launch_unity(const char* const* arguments, int* exit_code) {
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (strchr(arguments[0], '/')) {
            execv(arguments[0], (char* const*)arguments);
        } else {
            execvp(arguments[0], (char* const*)arguments);
        }
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
    } else {
        return false;
    }
    return true;
}
#endif

static const char* backend_force_argument(UnityFiniteVisualBackend backend) {
    switch (backend) {
        case UNITY_FINITE_VISUAL_BACKEND_METAL: return "-force-metal";
        case UNITY_FINITE_VISUAL_BACKEND_D3D11: return "-force-d3d11";
        case UNITY_FINITE_VISUAL_BACKEND_OPENGLCORE: return "-force-glcore";
        case UNITY_FINITE_VISUAL_BACKEND_VULKAN: return "-force-vulkan";
        default: return NULL;
    }
}

static bool verify_hex_digest(const CommonFileBytes* bytes, const char* hex) {
    char actual[UNITY_FINITE_VISUAL_SHA256_HEX_CAPACITY];
    hash_file_bytes(bytes, actual);
    return strcmp(actual, hex) == 0;
}

static UnityFiniteVisualGateStatus validate_render_artifacts(
    const UnityFiniteVisualGateSummary* summary,
    const char* baseline_path, const char* candidate_path,
    CommonFileBytes* baseline, CommonFileBytes* candidate) {
    memset(baseline, 0, sizeof(*baseline));
    memset(candidate, 0, sizeof(*candidate));
    CommonFileStatus status = common_file_read_regular(
        baseline_path, (size_t)FINITE_VISUAL_MAX_FIXTURE, baseline);
    if (status != COMMON_FILE_OK) return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
    status = common_file_read_regular(
        candidate_path, (size_t)FINITE_VISUAL_MAX_FIXTURE, candidate);
    if (status != COMMON_FILE_OK) {
        common_file_bytes_dispose(baseline);
        return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
    }
    if (baseline->size != summary->pixel_byte_count ||
        candidate->size != summary->pixel_byte_count ||
        !verify_hex_digest(baseline, summary->baseline_pixels_sha256) ||
        !verify_hex_digest(candidate, summary->candidate_pixels_sha256) ||
        (summary->pixel_equal !=
         (baseline->size == candidate->size &&
          memcmp(baseline->data, candidate->data, baseline->size) == 0))) {
        common_file_bytes_dispose(baseline);
        common_file_bytes_dispose(candidate);
        return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
    }
    if (!summary->pixel_equal) {
        size_t mismatch = 0U;
        while (mismatch < baseline->size &&
               baseline->data[mismatch] == candidate->data[mismatch]) {
            ++mismatch;
        }
        if ((uint64_t)mismatch != summary->first_mismatch_offset) {
            common_file_bytes_dispose(baseline);
            common_file_bytes_dispose(candidate);
            return UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
        }
    }
    return UNITY_FINITE_VISUAL_GATE_OK;
}

static void rollback_published(const UnityFiniteVisualGateOptions* options,
                               UnityFiniteVisualGateRunResult* result) {
    if (result->report_published) {
        (void)remove_file(options->report_path);
        result->report_published = false;
    }
    if (result->baseline_pixels_published) {
        (void)remove_file(options->baseline_pixels_path);
        result->baseline_pixels_published = false;
    }
    if (result->candidate_pixels_published) {
        (void)remove_file(options->candidate_pixels_path);
        result->candidate_pixels_published = false;
    }
}

UnityFiniteVisualGateStatus unity_finite_visual_gate_run(
    const UnityFiniteVisualGateOptions* options,
    UnityFiniteVisualGateRunResult* out_result) {
    if (!out_result) return UNITY_FINITE_VISUAL_GATE_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    out_result->unity_exit_code = -1;
    if (!unity_finite_visual_gate_options_validate(options)) {
        return UNITY_FINITE_VISUAL_GATE_INVALID_ARGUMENT;
    }
    if (path_exists(options->report_path) || path_exists(options->log_path) ||
        path_exists(options->baseline_pixels_path) ||
        path_exists(options->candidate_pixels_path)) {
        return UNITY_FINITE_VISUAL_GATE_OUTPUT_EXISTS;
    }
    InputFiles files;
    UnityFiniteVisualGateStatus status = read_inputs(options, &files);
    if (status != UNITY_FINITE_VISUAL_GATE_OK) return status;
    char* workspace = make_temporary_workspace(options->temporary_root);
    char* raw_fixture = NULL;
    char* raw_result = NULL;
    char* raw_baseline = NULL;
    char* raw_candidate = NULL;
    char* log_path = absolute_path(options->log_path);
    CommonFileBytes result_bytes = {NULL, 0U};
    CommonFileBytes log_bytes = {NULL, 0U};
    CommonFileBytes baseline_bytes = {NULL, 0U};
    CommonFileBytes candidate_bytes = {NULL, 0U};
    if (!workspace || !log_path) {
        status = UNITY_FINITE_VISUAL_GATE_ALLOCATION_FAILED;
        goto done;
    }
    if (strlen(workspace) >= sizeof(out_result->workspace_path)) {
        status = UNITY_FINITE_VISUAL_GATE_PATH_TOO_LONG;
        goto done;
    }
    memcpy(out_result->workspace_path, workspace, strlen(workspace) + 1U);
    if (!prepare_project(options, &files, workspace, &raw_fixture, &raw_result,
                         &raw_baseline, &raw_candidate)) {
        status = UNITY_FINITE_VISUAL_GATE_IO_FAILED;
        goto done;
    }
    const char* force_backend = backend_force_argument(options->backend);
    const char* arguments[] = {
        options->unity_executable,
        "-batchmode",
        force_backend,
        "-noUpm",
        "-job-worker-count", "1",
        "-diag-debug-shader-compiler",
        "-disableManagedDebugger",
        "-projectPath", workspace,
        "-logFile", log_path,
        "-executeMethod", "DXBCSandbox.Editor.DXBCFiniteVisualGate.Run",
        "-dxbc-finite-result", raw_result,
        "-dxbc-finite-baseline-pixels", raw_baseline,
        "-dxbc-finite-candidate-pixels", raw_candidate,
        "-dxbc-finite-fixture", raw_fixture,
        "-dxbc-finite-expected-unity", options->expected_unity_version,
        "-dxbc-finite-expected-backend",
            unity_finite_visual_backend_name(options->backend),
        "-dxbc-finite-bridge-sha256", files.bridge_sha,
        NULL,
    };
    if (!launch_unity(arguments, &out_result->unity_exit_code)) {
        status = UNITY_FINITE_VISUAL_GATE_PROCESS_FAILED;
        goto done;
    }
    CommonFileStatus file_status = common_file_read_regular(
        log_path, (size_t)FINITE_VISUAL_MAX_LOG, &log_bytes);
    if (file_status != COMMON_FILE_OK || log_bytes.size == 0U) {
        status = UNITY_FINITE_VISUAL_GATE_PROCESS_FAILED;
        goto done;
    }
    file_status = common_file_read_regular(
        raw_result, (size_t)FINITE_VISUAL_MAX_REPORT, &result_bytes);
    if (file_status != COMMON_FILE_OK) {
        status = UNITY_FINITE_VISUAL_GATE_PROCESS_FAILED;
        goto done;
    }
    UnityFiniteVisualGateSummary summary;
    status = unity_finite_visual_gate_parse_result(
        result_bytes.data, result_bytes.size, &summary);
    if (status != UNITY_FINITE_VISUAL_GATE_OK) goto done;
    if (strcmp(summary.fixture_sha256, files.fixture_sha) != 0 ||
        strcmp(summary.bridge_sha256, files.bridge_sha) != 0 ||
        strcmp(summary.baseline_source_sha256, files.baseline_sha) != 0 ||
        strcmp(summary.candidate_source_sha256, files.candidate_sha) != 0) {
        status = UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
        goto done;
    }
    const bool rendered =
        summary.reported_status == UNITY_FINITE_VISUAL_GATE_OK ||
        summary.reported_status == UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH ||
        summary.reported_status == UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC;
    if (rendered &&
        (strcmp(summary.unity_version,
                options->expected_unity_version) != 0 ||
         summary.backend != options->backend ||
         strcmp(summary.requested_color_space, summary.color_space) != 0)) {
        status = UNITY_FINITE_VISUAL_GATE_RESULT_INVALID;
        goto done;
    }
    unity_finite_visual_gate_compute_requested_coordinates(
        &summary, options->expected_unity_version, options->backend);
    const bool has_pixels = summary.baseline_imported &&
        summary.candidate_imported &&
        (summary.reported_status == UNITY_FINITE_VISUAL_GATE_OK ||
         summary.reported_status == UNITY_FINITE_VISUAL_GATE_PIXEL_MISMATCH ||
         summary.reported_status == UNITY_FINITE_VISUAL_GATE_NONDETERMINISTIC);
    if (has_pixels) {
        status = validate_render_artifacts(&summary, raw_baseline,
                                           raw_candidate, &baseline_bytes,
                                           &candidate_bytes);
        if (status != UNITY_FINITE_VISUAL_GATE_OK) goto done;
        file_status = common_file_write_new_atomic(
            options->baseline_pixels_path, baseline_bytes.data,
            baseline_bytes.size);
        if (file_status != COMMON_FILE_OK) {
            status = file_status == COMMON_FILE_ALREADY_EXISTS
                ? UNITY_FINITE_VISUAL_GATE_OUTPUT_EXISTS
                : UNITY_FINITE_VISUAL_GATE_IO_FAILED;
            goto done;
        }
        out_result->baseline_pixels_published = true;
        file_status = common_file_write_new_atomic(
            options->candidate_pixels_path, candidate_bytes.data,
            candidate_bytes.size);
        if (file_status != COMMON_FILE_OK) {
            status = file_status == COMMON_FILE_ALREADY_EXISTS
                ? UNITY_FINITE_VISUAL_GATE_OUTPUT_EXISTS
                : UNITY_FINITE_VISUAL_GATE_IO_FAILED;
            rollback_published(options, out_result);
            goto done;
        }
        out_result->candidate_pixels_published = true;
    }
    file_status = common_file_write_new_atomic(
        options->report_path, result_bytes.data, result_bytes.size);
    if (file_status != COMMON_FILE_OK) {
        status = file_status == COMMON_FILE_ALREADY_EXISTS
            ? UNITY_FINITE_VISUAL_GATE_OUTPUT_EXISTS
            : UNITY_FINITE_VISUAL_GATE_IO_FAILED;
        rollback_published(options, out_result);
        goto done;
    }
    out_result->report_published = true;
    out_result->summary = summary;
    if ((summary.reported_status == UNITY_FINITE_VISUAL_GATE_OK &&
         out_result->unity_exit_code != 0) ||
        (summary.reported_status != UNITY_FINITE_VISUAL_GATE_OK &&
         out_result->unity_exit_code != 2)) {
        status = UNITY_FINITE_VISUAL_GATE_PROCESS_FAILED;
        rollback_published(options, out_result);
        goto done;
    }
    status = summary.reported_status;

done:
    common_file_bytes_dispose(&result_bytes);
    common_file_bytes_dispose(&log_bytes);
    common_file_bytes_dispose(&baseline_bytes);
    common_file_bytes_dispose(&candidate_bytes);
    input_files_dispose(&files);
    free(raw_fixture);
    free(raw_result);
    free(raw_baseline);
    free(raw_candidate);
    free(log_path);
    if (workspace) {
        bool preserve = options->keep_policy == UNITY_FINITE_VISUAL_KEEP_ALWAYS ||
            (options->keep_policy == UNITY_FINITE_VISUAL_KEEP_ON_FAILURE &&
             status != UNITY_FINITE_VISUAL_GATE_OK);
        if (!preserve && !remove_tree(workspace)) {
            if (status == UNITY_FINITE_VISUAL_GATE_OK) {
                status = UNITY_FINITE_VISUAL_GATE_IO_FAILED;
            }
            preserve = true;
        }
        out_result->workspace_preserved = preserve;
    }
    free(workspace);
    return status;
}
