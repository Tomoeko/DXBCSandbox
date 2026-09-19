// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compiler_client.h"
#include "compiler/unity_compiler_cache.h"
#include "common/string_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>

#define MAGIC_NUMBER 0x0C0BD1E4
#define DEFAULT_UNITY_CONTENTS_PATH \
    "/Applications/Unity/Unity.app/Contents"
#define UNITY_TOOLCHAIN_CONFIGURATION \
    "{ \"activeUserToolchains\": [ \"Local\" ], " \
    "\"activeInternalToolchains\": [ \"Local\" ] }"

#define UNITY_SNIPPET_CONTRACT_MAX_ITEMS (1024 * 1024)
#define UNITY_COMPILER_DEFAULT_IO_TIMEOUT_MS 120000
#define UNITY_COMPILER_STARTUP_TIMEOUT_MS 10000
#define UNITY_COMPILER_SHUTDOWN_GRACE_MS 500
#define UNITY_COMPILER_KILL_GRACE_MS 1000
#define UNITY_COMPILER_MAX_BUFFER_SIZE (512ULL * 1024ULL * 1024ULL)

extern char** environ;

static bool parse_exact_status_record(
    const char* record, const char* prefix, size_t flag_count,
    bool flags[3]) {
    if (!record || !prefix || !flags || flag_count == 0U || flag_count > 3U) {
        return false;
    }
    flags[0] = false;
    flags[1] = false;
    flags[2] = false;
    size_t prefix_size = strlen(prefix);
    if (strncmp(record, prefix, prefix_size) != 0) return false;
    const char* cursor = record + prefix_size;
    for (size_t index = 0; index < flag_count; index++) {
        if (index > 0U && *cursor++ != ' ') return false;
        if (*cursor != '0' && *cursor != '1') return false;
        flags[index] = *cursor++ == '1';
    }
    return *cursor == '\0';
}

bool unity_compiler_parse_preprocess_status_record(
    const char* record, UnityCompilerPreprocessStatus* out_status) {
    if (!out_status) return false;
    memset(out_status, 0, sizeof(*out_status));
    bool flags[3];
    if (!parse_exact_status_record(record, "shader: ", 3U, flags)) {
        return false;
    }
    out_status->primary_success = flags[0];
    out_status->secondary_status = flags[1];
    out_status->tertiary_status = flags[2];
    return true;
}

bool unity_compiler_parse_compile_status_record(
    const char* record, bool* out_success) {
    if (!out_success) return false;
    *out_success = false;
    bool flags[3];
    if (!parse_exact_status_record(record, "shader: ", 1U, flags)) {
        return false;
    }
    *out_success = flags[0];
    return true;
}

bool unity_compiler_parse_disassemble_status_record(
    const char* record, bool* out_success) {
    if (!out_success) return false;
    *out_success = false;
    bool flags[3];
    if (!parse_exact_status_record(record, "disasm: ", 1U, flags)) {
        return false;
    }
    *out_success = flags[0];
    return true;
}

bool unity_compiler_session_capabilities_validate(
    const UnityCompilerSessionCapabilities* capabilities) {
    if (!capabilities) return false;
    const uint32_t reserved_mask = ~UNITY_COMPILER_PLATFORM_MASK;
    /* Unity 2021.3 initializes this wire Int to -1 and only clears one of the
     * low 25 bits when that platform fails initialization.  A different high
     * bit pattern is therefore not this protocol record. */
    return (capabilities->raw_available_platform_mask & reserved_mask) ==
           reserved_mask;
}

bool unity_compiler_session_capabilities_equal(
    const UnityCompilerSessionCapabilities* left,
    const UnityCompilerSessionCapabilities* right) {
    if (!unity_compiler_session_capabilities_validate(left) ||
        !unity_compiler_session_capabilities_validate(right) ||
        left->raw_available_platform_mask !=
            right->raw_available_platform_mask) {
        return false;
    }
    for (size_t index = 0; index < UNITY_COMPILER_PLATFORM_COUNT; index++) {
        if (left->platforms[index].supported_features !=
                right->platforms[index].supported_features ||
            left->platforms[index].version !=
                right->platforms[index].version) {
            return false;
        }
    }
    return true;
}

bool unity_compiler_session_capabilities_valid_apis(
    const UnityCompilerSessionCapabilities* capabilities,
    uint32_t* out_valid_apis) {
    if (!out_valid_apis ||
        !unity_compiler_session_capabilities_validate(capabilities)) {
        return false;
    }
    *out_valid_apis = capabilities->raw_available_platform_mask &
                      UNITY_COMPILER_PLATFORM_MASK;
    return true;
}

bool unity_compiler_session_capabilities_support_valid_apis(
    const UnityCompilerSessionCapabilities* capabilities,
    uint32_t valid_apis) {
    uint32_t available = 0U;
    return unity_compiler_session_capabilities_valid_apis(
               capabilities, &available) &&
           (valid_apis & ~UNITY_COMPILER_PLATFORM_MASK) == 0U &&
           (valid_apis & ~available) == 0U;
}

bool unity_compiler_session_capabilities_match_valid_apis(
    const UnityCompilerSessionCapabilities* capabilities,
    uint32_t valid_apis) {
    uint32_t available = 0U;
    return unity_compiler_session_capabilities_valid_apis(
               capabilities, &available) &&
           valid_apis == available;
}

static void expected_valid_apis_authority_snapshot(
    const UnityCompilerChannel* channel,
    UnityCompilerValidApisAuthority* out_authority) {
    memset(out_authority, 0, sizeof(*out_authority));
    if (!channel || !channel->expected_valid_apis_ready) return;

    out_authority->expected_valid_apis = channel->expected_valid_apis;
    if (channel->session_capabilities_failed) {
        out_authority->status =
            UNITY_COMPILER_VALID_APIS_AUTHORITY_SESSION_FAILED;
        return;
    }
    if (!channel->session_capabilities_ready) {
        out_authority->status =
            UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING;
        return;
    }
    if (!unity_compiler_session_capabilities_valid_apis(
            &channel->session_capabilities,
            &out_authority->observed_valid_apis)) {
        out_authority->status =
            UNITY_COMPILER_VALID_APIS_AUTHORITY_SESSION_FAILED;
        return;
    }
    out_authority->status =
        out_authority->expected_valid_apis ==
                out_authority->observed_valid_apis
            ? UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED
            : UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH;
}

bool unity_compiler_set_expected_valid_apis(
    UnityCompilerChannel* channel, uint32_t expected_valid_apis) {
    if (!channel || !channel->configured ||
        (expected_valid_apis & ~UNITY_COMPILER_PLATFORM_MASK) != 0U) {
        return false;
    }
    channel->expected_valid_apis = expected_valid_apis;
    channel->expected_valid_apis_ready = true;
    return true;
}

bool unity_compiler_clear_expected_valid_apis(
    UnityCompilerChannel* channel) {
    if (!channel || !channel->configured) return false;
    channel->expected_valid_apis = 0U;
    channel->expected_valid_apis_ready = false;
    return true;
}

bool unity_compiler_expected_valid_apis_authority(
    const UnityCompilerChannel* channel,
    UnityCompilerValidApisAuthority* out_authority) {
    if (!channel || !channel->configured || !out_authority) return false;
    expected_valid_apis_authority_snapshot(channel, out_authority);
    return true;
}

static bool expected_valid_apis_authority_permits_live_command(
    const UnityCompilerChannel* channel) {
    UnityCompilerValidApisAuthority authority;
    expected_valid_apis_authority_snapshot(channel, &authority);
    return authority.status ==
               UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED ||
           authority.status ==
               UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED;
}

static bool expected_valid_apis_authority_is_rejection(
    const UnityCompilerValidApisAuthority* authority) {
    return authority &&
           (authority->status ==
                UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH ||
            authority->status ==
                UNITY_COMPILER_VALID_APIS_AUTHORITY_SESSION_FAILED);
}

static void response_status_capture_valid_apis_authority(
    UnityCompilerResponseStatus* status,
    const UnityCompilerChannel* channel) {
    if (!status) return;
    expected_valid_apis_authority_snapshot(
        channel, &status->valid_apis_authority);
}

char* unity_compiler_session_capabilities_format_json(
    const UnityCompilerSessionCapabilities* capabilities) {
    uint32_t valid_apis = 0U;
    if (!unity_compiler_session_capabilities_valid_apis(
            capabilities, &valid_apis)) {
        return NULL;
    }

    StringBuilder report;
    sb_init(&report);
    sb_appendf(
        &report,
        "{\"schema\":\"dxbc-unity-compiler-session-v1\","
        "\"raw_available_platform_mask\":%" PRIu32 ","
        "\"valid_apis\":%" PRIu32 ",\"platform_records\":[",
        capabilities->raw_available_platform_mask, valid_apis);
    for (size_t index = 0; index < UNITY_COMPILER_PLATFORM_COUNT; index++) {
        if (index > 0U) sb_append_char(&report, ',');
        sb_appendf(
            &report,
            "{\"platform\":%zu,\"supported_features\":%" PRIu64
            ",\"version\":%" PRId32 "}",
            index, capabilities->platforms[index].supported_features,
            capabilities->platforms[index].version);
    }
    sb_append(&report, "]}");
    if (!sb_ok(&report)) {
        sb_free(&report);
        return NULL;
    }
    char* result = sb_detach(&report);
    sb_free(&report);
    return result;
}

void unity_compiler_response_status_init(
    UnityCompilerResponseStatus* status) {
    if (status) memset(status, 0, sizeof(*status));
}

void unity_compiler_response_status_free(
    UnityCompilerResponseStatus* status) {
    if (!status) return;
    for (size_t i = 0; i < status->diagnostic_count; i++) {
        free(status->diagnostics[i].record);
        free(status->diagnostics[i].file);
        free(status->diagnostics[i].message);
    }
    free(status->diagnostics);
    memset(status, 0, sizeof(*status));
}

static bool response_status_append_diagnostic(
    UnityCompilerResponseStatus* status, const int32_t fields[3],
    const char* record, const char* file, const char* message) {
    if (!status || !fields || !record || !file || !message ||
        status->diagnostic_count == SIZE_MAX / sizeof(*status->diagnostics)) {
        return false;
    }
    size_t next_count = status->diagnostic_count + 1U;
    UnityCompilerDiagnostic* diagnostics =
        (UnityCompilerDiagnostic*)realloc(
            status->diagnostics, next_count * sizeof(*diagnostics));
    if (!diagnostics) return false;
    status->diagnostics = diagnostics;
    UnityCompilerDiagnostic* diagnostic =
        &status->diagnostics[status->diagnostic_count];
    memset(diagnostic, 0, sizeof(*diagnostic));
    memcpy(diagnostic->fields, fields, sizeof(diagnostic->fields));
    diagnostic->record = strdup(record);
    diagnostic->file = strdup(file);
    diagnostic->message = strdup(message);
    if (!diagnostic->record || !diagnostic->file || !diagnostic->message) {
        free(diagnostic->record);
        free(diagnostic->file);
        free(diagnostic->message);
        memset(diagnostic, 0, sizeof(*diagnostic));
        return false;
    }
    status->diagnostic_count = next_count;
    return true;
}

bool unity_compiler_response_status_copy(
    UnityCompilerResponseStatus* destination,
    const UnityCompilerResponseStatus* source) {
    if (!destination || !source || destination == source) return false;
    UnityCompilerResponseStatus copy;
    unity_compiler_response_status_init(&copy);
    copy.availability = source->availability;
    copy.compiler_success = source->compiler_success;
    copy.from_cache = source->from_cache;
    copy.valid_apis_authority = source->valid_apis_authority;
    for (size_t i = 0; i < source->diagnostic_count; i++) {
        const UnityCompilerDiagnostic* diagnostic = &source->diagnostics[i];
        if (!response_status_append_diagnostic(
                &copy, diagnostic->fields, diagnostic->record,
                diagnostic->file, diagnostic->message)) {
            unity_compiler_response_status_free(&copy);
            return false;
        }
    }
    unity_compiler_response_status_free(destination);
    *destination = copy;
    return true;
}

bool unity_compiler_diagnostic_is_actionable(
    const UnityCompilerDiagnostic* diagnostic) {
    /* CgBatchPreprocessShader deliberately emits its elapsed-time message
     * through OnError with CgBatchErrorType 0.  A live pinned-compiler probe
     * emits warning 3206 as type 1 with a successful terminal status.
     * ShaderCompilerErrorReportSend also treats every callback type >= 1 as
     * reportable. Admit exactly the proven informational value; an unknown
     * negative enum remains fail-closed. */
    return !diagnostic || diagnostic->fields[0] != 0;
}

size_t unity_compiler_response_status_actionable_diagnostic_count(
    const UnityCompilerResponseStatus* status) {
    if (!status) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < status->diagnostic_count; ++i) {
        if (unity_compiler_diagnostic_is_actionable(
                &status->diagnostics[i])) {
            ++count;
        }
    }
    return count;
}

bool unity_compiler_response_status_is_clean_success(
    const UnityCompilerResponseStatus* status) {
    return status &&
           status->availability == UNITY_COMPILER_RESPONSE_AVAILABLE &&
           status->compiler_success &&
           !expected_valid_apis_authority_is_rejection(
               &status->valid_apis_authority) &&
           unity_compiler_response_status_actionable_diagnostic_count(
               status) == 0U;
}

char* unity_compiler_response_status_format(
    const UnityCompilerResponseStatus* status, const char* fallback) {
    StringBuilder formatted;
    sb_init(&formatted);
    if (status && status->valid_apis_authority.status ==
                      UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH) {
        sb_appendf(
            &formatted,
            "Unity compiler validApis authority mismatch: expected %" PRIu32
            ", initializeCompiler returned %" PRIu32,
            status->valid_apis_authority.expected_valid_apis,
            status->valid_apis_authority.observed_valid_apis);
    } else if (status && status->valid_apis_authority.status ==
                             UNITY_COMPILER_VALID_APIS_AUTHORITY_SESSION_FAILED) {
        sb_append(
            &formatted,
            "Unity compiler initializeCompiler session authority failed");
    } else if (status && status->availability ==
                      UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS) {
        sb_append(&formatted,
                  "Unity compiler cache-only lookup missed; no compiler "
                  "process was started");
    } else if (status) {
        for (size_t i = 0; i < status->diagnostic_count; i++) {
            const UnityCompilerDiagnostic* diagnostic =
                &status->diagnostics[i];
            sb_append(&formatted, diagnostic->record);
            sb_append(&formatted, "\n");
            sb_append(&formatted, diagnostic->file);
            sb_append(&formatted, "\n");
            sb_append(&formatted, diagnostic->message);
            sb_append(&formatted, "\n");
        }
    }
    if (formatted.len == 0U) {
        sb_append(&formatted, fallback ? fallback : "Compiler request failed");
    }
    if (!sb_ok(&formatted)) {
        sb_free(&formatted);
        return NULL;
    }
    char* result = sb_detach(&formatted);
    sb_free(&formatted);
    return result;
}

static bool compiler_cache_only_enabled(void) {
    const char* value = getenv("DXBC_USC_CACHE_ONLY");
    return value && strcmp(value, "1") == 0;
}

void unity_compiler_preprocess_response_init(
    UnityCompilerPreprocessResponse* response) {
    if (response) memset(response, 0, sizeof(*response));
}

void unity_compiler_preprocess_response_free(
    UnityCompilerPreprocessResponse* response) {
    if (!response) return;
    unity_compiler_response_status_free(&response->status);
    unity_compiler_free_preprocess(&response->result);
    memset(response, 0, sizeof(*response));
}

void unity_compiler_binary_response_init(
    UnityCompilerBinaryResponse* response) {
    if (response) memset(response, 0, sizeof(*response));
}

void unity_compiler_binary_response_free(
    UnityCompilerBinaryResponse* response) {
    if (!response) return;
    unity_compiler_response_status_free(&response->status);
    for (size_t index = 0U;
         index < response->reflection_record_count; ++index) {
        unity_compiler_reflection_record_free(
            &response->reflection_records[index]);
    }
    free(response->reflection_records);
    free(response->data);
    memset(response, 0, sizeof(*response));
}

void unity_compiler_text_response_init(
    UnityCompilerTextResponse* response) {
    if (response) memset(response, 0, sizeof(*response));
}

void unity_compiler_text_response_free(
    UnityCompilerTextResponse* response) {
    if (!response) return;
    unity_compiler_response_status_free(&response->status);
    free(response->text);
    memset(response, 0, sizeof(*response));
}

static void free_string_array(char** values, int count) {
    if (!values) return;
    for (int i = 0; i < count; i++) free(values[i]);
    free(values);
}

void unity_compiler_snippet_contract_init(SnippetCompileContract* contract) {
    if (contract) memset(contract, 0, sizeof(*contract));
}

void unity_compiler_snippet_contract_free(SnippetCompileContract* contract) {
    if (!contract) return;
    if (contract->program_keyword_variants) {
        for (int i = 0; i < contract->program_keyword_variant_count; i++) {
            SnippetProgramKeywordVariants* program =
                &contract->program_keyword_variants[i];
            free_string_array(program->user_global.combinations,
                              program->user_global.combination_count);
            free_string_array(program->user_local.combinations,
                              program->user_local.combination_count);
            free_string_array(program->builtin.combinations,
                              program->builtin.combination_count);
        }
        free(contract->program_keyword_variants);
    }
    free_string_array(contract->non_stripped_user_keywords,
                      contract->non_stripped_user_keyword_count);
    free_string_array(contract->builtin_keywords,
                      contract->builtin_keyword_count);
    if (contract->conditional_requirements) {
        for (int i = 0; i < contract->conditional_requirement_count; i++) {
            free(contract->conditional_requirements[i].keyword);
        }
        free(contract->conditional_requirements);
    }
    memset(contract, 0, sizeof(*contract));
}

static bool valid_owned_string_array(char** values, int count) {
    if (count < 0 || count > UNITY_SNIPPET_CONTRACT_MAX_ITEMS ||
        (count > 0 && !values)) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!values[i] || values[i][0] == '\0') return false;
    }
    return true;
}

static bool valid_variant_set(const SnippetKeywordVariantSet* set) {
    if (!set || set->combination_count < 0 ||
        set->combination_count > UNITY_SNIPPET_CONTRACT_MAX_ITEMS) {
        return false;
    }
    if (!set->present) {
        return set->combination_count == 0 && set->combinations == NULL;
    }
    if (set->combination_count > 0 && !set->combinations) return false;
    for (int i = 0; i < set->combination_count; i++) {
        /* An empty string is a valid representation of an empty variant. */
        if (!set->combinations[i]) return false;
    }
    return true;
}

bool unity_compiler_snippet_contract_validate(
    const SnippetCompileContract* contract) {
    if (!contract || contract->start_line < 0 ||
        (contract->use_dxc_apis != 0 && contract->use_dxc_apis != 1) ||
        (contract->never_use_dxc_apis != 0 &&
         contract->never_use_dxc_apis != 1) ||
        contract->program_keyword_variant_count < 0 ||
        contract->program_keyword_variant_count >
            UNITY_SNIPPET_CONTRACT_MAX_ITEMS ||
        (contract->program_keyword_variant_count > 0 &&
         !contract->program_keyword_variants) ||
        !valid_owned_string_array(
            contract->non_stripped_user_keywords,
            contract->non_stripped_user_keyword_count) ||
        !valid_owned_string_array(contract->builtin_keywords,
                                  contract->builtin_keyword_count) ||
        contract->conditional_requirement_count < 0 ||
        contract->conditional_requirement_count >
            UNITY_SNIPPET_CONTRACT_MAX_ITEMS ||
        (contract->conditional_requirement_count > 0 &&
         !contract->conditional_requirements)) {
        return false;
    }
    for (int i = 0; i < contract->program_keyword_variant_count; i++) {
        const SnippetProgramKeywordVariants* program =
            &contract->program_keyword_variants[i];
        if (program->compiler_program < 0 ||
            !valid_variant_set(&program->user_global) ||
            !valid_variant_set(&program->user_local) ||
            !valid_variant_set(&program->builtin)) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (contract->program_keyword_variants[j].compiler_program ==
                program->compiler_program) {
                return false;
            }
        }
    }
    for (int i = 0; i < contract->conditional_requirement_count; i++) {
        const ConditionalShaderRequirement* requirement =
            &contract->conditional_requirements[i];
        if (!requirement->keyword || requirement->keyword[0] == '\0') {
            return false;
        }
    }
    return true;
}

static bool copy_string_array(
    char*** out_values, int count, const char* const* values) {
    *out_values = NULL;
    if (count == 0) return true;
    char** copy = (char**)calloc((size_t)count, sizeof(*copy));
    if (!copy) return false;
    for (int i = 0; i < count; i++) {
        copy[i] = strdup(values[i]);
        if (!copy[i]) {
            free_string_array(copy, count);
            return false;
        }
    }
    *out_values = copy;
    return true;
}

static bool copy_variant_set(
    SnippetKeywordVariantSet* destination,
    const SnippetKeywordVariantSet* source) {
    *destination = *source;
    destination->combinations = NULL;
    return !source->present || copy_string_array(
        &destination->combinations, source->combination_count,
        (const char* const*)source->combinations);
}

bool unity_compiler_snippet_contract_copy(
    SnippetCompileContract* destination,
    const SnippetCompileContract* source) {
    if (!destination || !unity_compiler_snippet_contract_validate(source)) {
        return false;
    }
    if (destination == source) return true;

    SnippetCompileContract copy = *source;
    copy.program_keyword_variants = NULL;
    copy.non_stripped_user_keywords = NULL;
    copy.builtin_keywords = NULL;
    copy.conditional_requirements = NULL;
    if (source->program_keyword_variant_count > 0) {
        copy.program_keyword_variants =
            (SnippetProgramKeywordVariants*)calloc(
                (size_t)source->program_keyword_variant_count,
                sizeof(*copy.program_keyword_variants));
        if (!copy.program_keyword_variants) {
            unity_compiler_snippet_contract_free(&copy);
            return false;
        }
        for (int i = 0; i < source->program_keyword_variant_count; i++) {
            const SnippetProgramKeywordVariants* source_program =
                &source->program_keyword_variants[i];
            SnippetProgramKeywordVariants* destination_program =
                &copy.program_keyword_variants[i];
            destination_program->compiler_program =
                source_program->compiler_program;
            if (!copy_variant_set(&destination_program->user_global,
                                  &source_program->user_global) ||
                !copy_variant_set(&destination_program->user_local,
                                  &source_program->user_local) ||
                !copy_variant_set(&destination_program->builtin,
                                  &source_program->builtin)) {
                unity_compiler_snippet_contract_free(&copy);
                return false;
            }
        }
    }
    if (!copy_string_array(&copy.non_stripped_user_keywords,
                           source->non_stripped_user_keyword_count,
                           (const char* const*)
                               source->non_stripped_user_keywords) ||
        !copy_string_array(&copy.builtin_keywords,
                           source->builtin_keyword_count,
                           (const char* const*)source->builtin_keywords)) {
        unity_compiler_snippet_contract_free(&copy);
        return false;
    }
    if (source->conditional_requirement_count > 0) {
        copy.conditional_requirements =
            (ConditionalShaderRequirement*)calloc(
                (size_t)source->conditional_requirement_count,
                sizeof(*copy.conditional_requirements));
        if (!copy.conditional_requirements) {
            unity_compiler_snippet_contract_free(&copy);
            return false;
        }
        for (int i = 0; i < source->conditional_requirement_count; i++) {
            copy.conditional_requirements[i].keyword =
                strdup(source->conditional_requirements[i].keyword);
            if (!copy.conditional_requirements[i].keyword) {
                unity_compiler_snippet_contract_free(&copy);
                return false;
            }
            copy.conditional_requirements[i].requirements =
                source->conditional_requirements[i].requirements;
        }
    }

    unity_compiler_snippet_contract_free(destination);
    *destination = copy;
    return true;
}

static bool parse_header_i32(const char** cursor, int32_t* value) {
    while (isspace((unsigned char)**cursor)) (*cursor)++;
    if (**cursor == '\0') return false;
    errno = 0;
    char* end = NULL;
    long parsed = strtol(*cursor, &end, 10);
    if (errno == ERANGE || end == *cursor || parsed < INT32_MIN ||
        parsed > INT32_MAX || (*end != '\0' &&
        !isspace((unsigned char)*end))) {
        return false;
    }
    *value = (int32_t)parsed;
    *cursor = end;
    return true;
}

static bool parse_canonical_i32(const char** cursor, int32_t* out_value) {
    if (!cursor || !*cursor || !out_value) return false;
    const char* text = *cursor;
    bool negative = *text == '-';
    if (negative) text++;
    if (!isdigit((unsigned char)*text)) return false;
    if (*text == '0' && (negative || isdigit((unsigned char)text[1]))) {
        return false;
    }
    uint64_t magnitude = 0U;
    uint64_t limit = negative ? UINT64_C(2147483648)
                              : UINT64_C(2147483647);
    while (isdigit((unsigned char)*text)) {
        unsigned digit = (unsigned)(*text - '0');
        if (magnitude > (limit - digit) / 10U) return false;
        magnitude = magnitude * 10U + digit;
        text++;
    }
    *out_value = negative
        ? (magnitude == UINT64_C(2147483648)
               ? INT32_MIN
               : -(int32_t)magnitude)
        : (int32_t)magnitude;
    *cursor = text;
    return true;
}

static bool parse_exact_i32_record(
    const char* record, const char* prefix, size_t value_count,
    int32_t* values) {
    if (!record || !prefix || (value_count > 0U && !values)) return false;
    size_t prefix_size = strlen(prefix);
    if (strncmp(record, prefix, prefix_size) != 0) return false;
    const char* cursor = record + prefix_size;
    for (size_t index = 0; index < value_count; index++) {
        if (*cursor++ != ' ' ||
            !parse_canonical_i32(&cursor, &values[index])) {
            return false;
        }
    }
    return *cursor == '\0';
}

typedef struct {
    UnityCompilerReflectionKind kind;
    const char* prefix;
    size_t value_count;
    bool named;
} ReflectionRecordShape;

static const ReflectionRecordShape kReflectionRecordShapes[] = {
    {UNITY_COMPILER_REFLECTION_INPUT, "input:", 2U, false},
    {UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER, "cb:", 2U, true},
    {UNITY_COMPILER_REFLECTION_CONSTANT, "const:", 6U, true},
    {UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER_BINDING, "cbbind:", 1U,
     true},
    {UNITY_COMPILER_REFLECTION_TEXTURE_BINDING, "texbind:", 4U, true},
    {UNITY_COMPILER_REFLECTION_SAMPLER, "sampler:", 2U, false},
    {UNITY_COMPILER_REFLECTION_BUFFER_BINDING, "bufferbind:", 2U, true},
    {UNITY_COMPILER_REFLECTION_UAV_BINDING, "uavbind:", 2U, true},
    {UNITY_COMPILER_REFLECTION_STATS, "stats:", 4U, false},
};

void unity_compiler_reflection_record_free(
    UnityCompilerReflectionRecord* record) {
    if (!record) return;
    free(record->record);
    free(record->name);
    memset(record, 0, sizeof(*record));
}

bool unity_compiler_reflection_record_parse(
    const char* text, UnityCompilerReflectionRecord* out_record) {
    if (!out_record) return false;
    memset(out_record, 0, sizeof(*out_record));
    if (!text) return false;

    for (size_t shape_index = 0U;
         shape_index < sizeof(kReflectionRecordShapes) /
                           sizeof(kReflectionRecordShapes[0]);
         ++shape_index) {
        const ReflectionRecordShape* shape =
            &kReflectionRecordShapes[shape_index];
        const size_t prefix_size = strlen(shape->prefix);
        if (strncmp(text, shape->prefix, prefix_size) != 0) continue;

        const char* name_begin = NULL;
        size_t name_size = 0U;
        bool parsed = false;
        if (!shape->named) {
            parsed = parse_exact_i32_record(
                text, shape->prefix, shape->value_count,
                out_record->values);
        } else {
            const char* cursor = text + prefix_size;
            if (*cursor++ != ' ') return false;
            name_begin = cursor;
            while (*cursor && !isspace((unsigned char)*cursor)) ++cursor;
            name_size = (size_t)(cursor - name_begin);
            if (name_size == 0U) return false;
            parsed = true;
            for (size_t value = 0U;
                 value < shape->value_count; ++value) {
                if (*cursor++ != ' ' ||
                    !parse_canonical_i32(
                        &cursor, &out_record->values[value])) {
                    parsed = false;
                    break;
                }
            }
            parsed = parsed && *cursor == '\0';
        }
        if (!parsed) return false;

        out_record->record = strdup(text);
        if (!out_record->record) return false;
        if (shape->named) {
            out_record->name = (char*)malloc(name_size + 1U);
            if (!out_record->name) {
                unity_compiler_reflection_record_free(out_record);
                return false;
            }
            memcpy(out_record->name, name_begin, name_size);
            out_record->name[name_size] = '\0';
        }
        out_record->kind = shape->kind;
        out_record->value_count = shape->value_count;
        return true;
    }
    return false;
}

bool unity_compiler_reflection_records_equal(
    const UnityCompilerReflectionRecord* left, size_t left_count,
    const UnityCompilerReflectionRecord* right, size_t right_count) {
    if (left_count != right_count ||
        (left_count > 0U && (!left || !right))) {
        return false;
    }
    for (size_t index = 0U; index < left_count; ++index) {
        if (!left[index].record || !right[index].record ||
            strcmp(left[index].record, right[index].record) != 0) {
            return false;
        }
    }
    return true;
}

static bool reflection_record_has_known_prefix(const char* text) {
    if (!text) return false;
    for (size_t index = 0U;
         index < sizeof(kReflectionRecordShapes) /
                     sizeof(kReflectionRecordShapes[0]);
         ++index) {
        const size_t prefix_size =
            strlen(kReflectionRecordShapes[index].prefix);
        if (strncmp(text, kReflectionRecordShapes[index].prefix,
                    prefix_size) == 0) {
            return true;
        }
    }
    return false;
}

static bool binary_response_append_reflection(
    UnityCompilerBinaryResponse* response, const char* text) {
    if (!response || !text ||
        response->reflection_record_count ==
            SIZE_MAX / sizeof(*response->reflection_records)) {
        return false;
    }
    const size_t next_count = response->reflection_record_count + 1U;
    UnityCompilerReflectionRecord* records =
        (UnityCompilerReflectionRecord*)realloc(
            response->reflection_records,
            next_count * sizeof(*response->reflection_records));
    if (!records) return false;
    response->reflection_records = records;
    UnityCompilerReflectionRecord* record =
        &records[response->reflection_record_count];
    if (!unity_compiler_reflection_record_parse(text, record)) return false;
    response->reflection_record_count = next_count;
    return true;
}

static bool parse_error_record(const char* record, int32_t fields[3]) {
    return parse_exact_i32_record(record, "err:", 3U, fields);
}

bool unity_compiler_snippet_contract_parse_header(
    const char* header,
    SnippetCompileContract* contract) {
    if (!header || !contract || strncmp(header, "snip:", 5) != 0) {
        return false;
    }
    int32_t tokens[13];
    const char* cursor = header + 5;
    /* Unity 2021.3.29f1 writes the original 11-field record.  The two DXC
     * policy fields were appended by 2021.3.35f1.  Accept exactly those two
     * observed protocol shapes; an intermediate/trailing field count remains
     * malformed so a shifted stream cannot be mistaken for a contract. */
    const size_t legacy_token_count = 11U;
    for (size_t i = 0; i < legacy_token_count; i++) {
        if (!parse_header_i32(&cursor, &tokens[i])) return false;
    }
    while (isspace((unsigned char)*cursor)) cursor++;
    const bool has_dxc_policy_fields = *cursor != '\0';
    if (has_dxc_policy_fields) {
        if (!parse_header_i32(&cursor, &tokens[11]) ||
            !parse_header_i32(&cursor, &tokens[12])) {
            return false;
        }
        while (isspace((unsigned char)*cursor)) cursor++;
        if (*cursor != '\0') return false;
    }

    SnippetCompileContract parsed;
    unity_compiler_snippet_contract_init(&parsed);
    parsed.snippet_id = tokens[0];
    parsed.platforms = tokens[1];
    parsed.quality_variants = tokens[2];
    parsed.program_types_mask = (uint32_t)tokens[3];
    parsed.compilation_flags = (uint32_t)tokens[4];
    parsed.language = tokens[5];
    for (int i = 0; i < UNITY_SNIPPET_SOURCE_HASH_WORD_COUNT; i++) {
        parsed.source_hash[i] = (uint32_t)tokens[6 + i];
    }
    parsed.start_line = tokens[10];
    parsed.use_dxc_apis = has_dxc_policy_fields ? tokens[11] : 0;
    parsed.never_use_dxc_apis = has_dxc_policy_fields ? tokens[12] : 0;
    if (!unity_compiler_snippet_contract_validate(&parsed)) return false;

    unity_compiler_snippet_contract_free(contract);
    *contract = parsed;
    return true;
}

static bool split_keyword_line(
    const char* line, char*** out_keywords, int* out_count) {
    *out_keywords = NULL;
    *out_count = 0;
    if (!line) return false;

    const char* cursor = line;
    int count = 0;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) cursor++;
        if (!*cursor) break;
        count++;
        if (count > UNITY_SNIPPET_CONTRACT_MAX_ITEMS) return false;
        while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
    }
    if (count == 0) return true;

    char** keywords = (char**)calloc((size_t)count, sizeof(*keywords));
    if (!keywords) return false;
    cursor = line;
    for (int i = 0; i < count; i++) {
        while (isspace((unsigned char)*cursor)) cursor++;
        const char* begin = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
        size_t length = (size_t)(cursor - begin);
        keywords[i] = (char*)malloc(length + 1U);
        if (!keywords[i]) {
            free_string_array(keywords, count);
            return false;
        }
        memcpy(keywords[i], begin, length);
        keywords[i][length] = '\0';
    }
    *out_keywords = keywords;
    *out_count = count;
    return true;
}

bool unity_compiler_snippet_contract_set_keyword_lines(
    SnippetCompileContract* contract,
    const char* non_stripped_user_keywords,
    const char* builtin_keywords) {
    if (!contract) return false;
    char** non_stripped_user = NULL;
    char** builtin = NULL;
    int non_stripped_user_count = 0;
    int builtin_count = 0;
    if (!split_keyword_line(non_stripped_user_keywords, &non_stripped_user,
                            &non_stripped_user_count) ||
        !split_keyword_line(builtin_keywords, &builtin, &builtin_count)) {
        free_string_array(non_stripped_user, non_stripped_user_count);
        free_string_array(builtin, builtin_count);
        return false;
    }
    free_string_array(contract->non_stripped_user_keywords,
                      contract->non_stripped_user_keyword_count);
    free_string_array(contract->builtin_keywords,
                      contract->builtin_keyword_count);
    contract->non_stripped_user_keywords = non_stripped_user;
    contract->non_stripped_user_keyword_count = non_stripped_user_count;
    contract->builtin_keywords = builtin;
    contract->builtin_keyword_count = builtin_count;
    return true;
}

static SnippetKeywordVariantSet* select_variant_set(
    SnippetProgramKeywordVariants* program,
    UnityKeywordVariantFamily family) {
    if (!program) return NULL;
    switch (family) {
        case UNITY_KEYWORD_VARIANTS_USER_GLOBAL:
            return &program->user_global;
        case UNITY_KEYWORD_VARIANTS_USER_LOCAL:
            return &program->user_local;
        case UNITY_KEYWORD_VARIANTS_BUILTIN:
            return &program->builtin;
        default:
            return NULL;
    }
}

const SnippetProgramKeywordVariants*
unity_compiler_snippet_contract_find_program_variants(
    const SnippetCompileContract* contract,
    int32_t compiler_program) {
    if (!contract || compiler_program < 0) return NULL;
    for (int i = 0; i < contract->program_keyword_variant_count; i++) {
        if (contract->program_keyword_variants[i].compiler_program ==
            compiler_program) {
            return &contract->program_keyword_variants[i];
        }
    }
    return NULL;
}

static SnippetProgramKeywordVariants* find_program_variants_mutable(
    SnippetCompileContract* contract, int32_t compiler_program) {
    return (SnippetProgramKeywordVariants*)
        unity_compiler_snippet_contract_find_program_variants(
            contract, compiler_program);
}

bool unity_compiler_snippet_contract_set_variant_combinations(
    SnippetCompileContract* contract,
    int32_t compiler_program,
    UnityKeywordVariantFamily family,
    const char* const* combinations,
    int combination_count) {
    if (!contract || compiler_program < 0 ||
        family < UNITY_KEYWORD_VARIANTS_USER_GLOBAL ||
        family > UNITY_KEYWORD_VARIANTS_BUILTIN || combination_count < 0 ||
        combination_count > UNITY_SNIPPET_CONTRACT_MAX_ITEMS ||
        (combination_count > 0 && !combinations)) {
        return false;
    }
    for (int i = 0; i < combination_count; i++) {
        if (!combinations[i]) return false;
    }
    char** copy = NULL;
    if (!copy_string_array(&copy, combination_count, combinations)) {
        return false;
    }

    SnippetProgramKeywordVariants* program =
        find_program_variants_mutable(contract, compiler_program);
    if (!program) {
        if (contract->program_keyword_variant_count >=
                UNITY_SNIPPET_CONTRACT_MAX_ITEMS ||
            (size_t)(contract->program_keyword_variant_count + 1) >
                SIZE_MAX / sizeof(*contract->program_keyword_variants)) {
            free_string_array(copy, combination_count);
            return false;
        }
        size_t new_count =
            (size_t)contract->program_keyword_variant_count + 1U;
        SnippetProgramKeywordVariants* programs =
            (SnippetProgramKeywordVariants*)realloc(
                contract->program_keyword_variants,
                new_count * sizeof(*programs));
        if (!programs) {
            free_string_array(copy, combination_count);
            return false;
        }
        contract->program_keyword_variants = programs;
        program = &programs[contract->program_keyword_variant_count++];
        memset(program, 0, sizeof(*program));
        program->compiler_program = compiler_program;
    }
    SnippetKeywordVariantSet* set = select_variant_set(program, family);
    if (!set) {
        free_string_array(copy, combination_count);
        return false;
    }
    free_string_array(set->combinations, set->combination_count);
    set->present = true;
    set->combinations = copy;
    set->combination_count = combination_count;
    return true;
}

bool unity_compiler_snippet_contract_has_variant_families(
    const SnippetCompileContract* contract,
    int32_t compiler_program) {
    const SnippetProgramKeywordVariants* program =
        unity_compiler_snippet_contract_find_program_variants(
            contract, compiler_program);
    return program && program->user_global.present &&
           program->user_local.present && program->builtin.present;
}

static bool path_has_suffix(const char* path, const char* suffix) {
    if (!path || !suffix) return false;
    size_t path_size = strlen(path);
    size_t suffix_size = strlen(suffix);
    return path_size >= suffix_size &&
           strcmp(path + path_size - suffix_size, suffix) == 0;
}

static char* copy_trimmed_path(const char* path) {
    if (!path) return NULL;
    size_t size = strlen(path);
    while (size > 1U && path[size - 1U] == '/') size--;
    char* copy = (char*)malloc(size + 1U);
    if (!copy) return NULL;
    memcpy(copy, path, size);
    copy[size] = '\0';
    return copy;
}

static char* path_with_suffix(const char* root, const char* suffix) {
    const char* effective_root = root ? root : "";
    size_t root_size = strlen(effective_root);
    while (root_size > 1U && effective_root[root_size - 1U] == '/') {
        root_size--;
    }
    size_t suffix_size = strlen(suffix);
    if (root_size > SIZE_MAX - suffix_size - 1U) return NULL;
    char* path = (char*)malloc(root_size + suffix_size + 1U);
    if (!path) return NULL;
    memcpy(path, effective_root, root_size);
    memcpy(path + root_size, suffix, suffix_size + 1U);
    return path;
}

static char* unity_contents_from_editor_path(const char* editor_path) {
    if (!editor_path || !editor_path[0]) return NULL;
    const char* marker = strstr(editor_path, ".app/Contents");
    if (marker) {
        size_t size = (size_t)(marker - editor_path) +
                      strlen(".app/Contents");
        char* contents = (char*)malloc(size + 1U);
        if (!contents) return NULL;
        memcpy(contents, editor_path, size);
        contents[size] = '\0';
        return contents;
    }
    if (path_has_suffix(editor_path, ".app")) {
        return path_with_suffix(editor_path, "/Contents");
    }
    if (path_has_suffix(editor_path, "/Contents")) {
        return copy_trimmed_path(editor_path);
    }
    return NULL;
}

static char* resolve_unity_contents_path(void) {
    const char* explicit_contents = getenv("DXBC_UNITY_CONTENTS_PATH");
    if (explicit_contents && explicit_contents[0]) {
        return copy_trimmed_path(explicit_contents);
    }
    const char* unity_app = getenv("DXBC_UNITY_APP");
    if (unity_app && unity_app[0]) {
        if (path_has_suffix(unity_app, "/Contents")) {
            return copy_trimmed_path(unity_app);
        }
        return path_with_suffix(unity_app, "/Contents");
    }
    const char* editor_path = getenv("UNITY_EDITOR_PATH");
    char* from_editor = unity_contents_from_editor_path(editor_path);
    if (from_editor) return from_editor;
    return strdup(DEFAULT_UNITY_CONTENTS_PATH);
}

static char* resolve_artifact_path(const char* environment_name,
                                   const char* contents_path,
                                   const char* suffix) {
    const char* explicit_path = getenv(environment_name);
    if (explicit_path && explicit_path[0]) {
        return copy_trimmed_path(explicit_path);
    }
    return path_with_suffix(contents_path, suffix);
}

static char* make_dynamic_library_path(const char* frameworks_dir,
                                       const char* tools_dir) {
    size_t frameworks_size = strlen(frameworks_dir);
    size_t tools_size = strlen(tools_dir);
    if (frameworks_size > SIZE_MAX - tools_size - 2U) return NULL;
    char* result = (char*)malloc(frameworks_size + tools_size + 2U);
    if (!result) return NULL;
    memcpy(result, frameworks_dir, frameworks_size);
    result[frameworks_size] = ':';
    memcpy(result + frameworks_size + 1U, tools_dir, tools_size + 1U);
    return result;
}

static bool configure_toolchain_paths(UnityCompilerChannel* channel) {
    channel->unity_contents_path = resolve_unity_contents_path();
    if (!channel->unity_contents_path) return false;
    channel->compiler_path = resolve_artifact_path(
        "DXBC_UNITY_COMPILER_PATH", channel->unity_contents_path,
        "/Tools/UnityShaderCompiler");
    channel->builtin_includes_dir = resolve_artifact_path(
        "DXBC_UNITY_BUILTIN_INCLUDES_PATH", channel->unity_contents_path,
        "/CGIncludes");
    channel->playback_engines_dir = resolve_artifact_path(
        "DXBC_UNITY_PLAYBACK_ENGINES_PATH", channel->unity_contents_path,
        "/PlaybackEngines");
    channel->frameworks_dir = path_with_suffix(
        channel->unity_contents_path, "/Frameworks");
    channel->tools_dir = path_with_suffix(
        channel->unity_contents_path, "/Tools");
    channel->glslang_path = resolve_artifact_path(
        "DXBC_UNITY_GLSLANG_PATH", channel->unity_contents_path,
        "/Tools/glslang.dylib");
    channel->dxcompiler_path = resolve_artifact_path(
        "DXBC_UNITY_DXCOMPILER_PATH", channel->unity_contents_path,
        "/Tools/libdxcompiler.dylib");
    if (!channel->compiler_path || !channel->builtin_includes_dir ||
        !channel->playback_engines_dir || !channel->frameworks_dir ||
        !channel->tools_dir || !channel->glslang_path ||
        !channel->dxcompiler_path) {
        return false;
    }
    channel->dynamic_library_path = make_dynamic_library_path(
        channel->frameworks_dir, channel->tools_dir);
    return channel->dynamic_library_path != NULL;
}

static void invalidate_compiler_channel(UnityCompilerChannel* channel);

static void discard_toolchain_authority(UnityCompilerChannel* channel,
                                        bool invalidate_process) {
    if (!channel) return;
    if (invalidate_process) invalidate_compiler_channel(channel);
    usc_cache_toolchain_lease_destroy(channel->cache_toolchain_lease);
    channel->cache_toolchain_lease = NULL;
    channel->cache_compiler_ready = false;
    channel->cache_compiler_failed = false;
    channel->cache_environment_ready = false;
    channel->cache_environment_failed = false;
    memset(channel->cache_compiler_fingerprint, 0,
           sizeof(channel->cache_compiler_fingerprint));
    memset(channel->cache_environment_fingerprint, 0,
           sizeof(channel->cache_environment_fingerprint));
    memset(&channel->session_capabilities, 0,
           sizeof(channel->session_capabilities));
    channel->session_capabilities_ready = false;
    channel->session_capabilities_failed = false;
}

static UscCacheToolchainPaths toolchain_paths(
    const UnityCompilerChannel* channel) {
    UscCacheToolchainPaths paths = {
        .compiler_path = channel->compiler_path,
        .project_root = channel->project_root,
        .includes_dir = channel->includes_dir,
        .sandbox_includes_dir = channel->sandbox_includes_dir,
        .builtin_includes_dir = channel->builtin_includes_dir,
        .proxy_path = NULL, /* Reserved v4 cache field; no compiler injection. */
        .glslang_path = channel->glslang_path,
        .dxcompiler_path = channel->dxcompiler_path,
        .unity_contents_path = channel->unity_contents_path,
        .playback_engines_path = channel->playback_engines_dir,
    };
    return paths;
}

/* One channel is tied to one immutable content snapshot.  If any observed
 * file, symlink, or directory changes, the current operation fails and the
 * possibly stale compiler process is discarded.  A later operation may then
 * capture a new snapshot; the failing operation never refreshes in place. */
static bool capture_or_validate_toolchain_authority(
    UnityCompilerChannel* channel) {
    if (!channel || !channel->configured) return false;
    if (channel->cache_toolchain_lease) {
        if (usc_cache_toolchain_lease_validate(
                channel->cache_toolchain_lease)) {
            return !channel->session_capabilities_failed;
        }
        discard_toolchain_authority(channel, true);
        return false;
    }

    UscCacheToolchainPaths paths = toolchain_paths(channel);
    UscCacheToolchainLease* lease = NULL;
    uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE];
    uint8_t environment_fingerprint[USC_CACHE_DIGEST_SIZE];
    if (!usc_cache_toolchain_lease_create(
            &paths, compiler_fingerprint, environment_fingerprint,
            &lease)) {
        return false;
    }
    memcpy(channel->cache_compiler_fingerprint, compiler_fingerprint,
           USC_CACHE_DIGEST_SIZE);
    memcpy(channel->cache_environment_fingerprint, environment_fingerprint,
           USC_CACHE_DIGEST_SIZE);
    channel->cache_compiler_ready = true;
    channel->cache_compiler_failed = false;
    channel->cache_environment_ready = true;
    channel->cache_environment_failed = false;
    channel->cache_toolchain_lease = lease;
    return true;
}

static bool validate_existing_toolchain_authority(
    UnityCompilerChannel* channel) {
    if (!channel || !channel->cache_toolchain_lease) return false;
    if (usc_cache_toolchain_lease_validate(channel->cache_toolchain_lease)) {
        return !channel->session_capabilities_failed;
    }
    discard_toolchain_authority(channel, true);
    return false;
}

static bool get_toolchain_fingerprints(
    UnityCompilerChannel* channel,
    uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE],
    uint8_t environment_fingerprint[USC_CACHE_DIGEST_SIZE]) {
    if (!channel || !channel->configured || !compiler_fingerprint ||
        !environment_fingerprint) {
        return false;
    }
    if (!capture_or_validate_toolchain_authority(channel)) return false;
    memcpy(compiler_fingerprint, channel->cache_compiler_fingerprint,
           USC_CACHE_DIGEST_SIZE);
    memcpy(environment_fingerprint, channel->cache_environment_fingerprint,
           USC_CACHE_DIGEST_SIZE);
    return true;
}

static bool environment_entry_has_name(
    const char* entry, const char* name) {
    if (!entry || !name) return false;
    size_t name_size = strlen(name);
    return strncmp(entry, name, name_size) == 0 && entry[name_size] == '=';
}

static bool compiler_environment_overrides(const char* entry) {
    static const char* names[] = {
        "TMPDIR",
        "DYLD_LIBRARY_PATH",
        "LD_LIBRARY_PATH",
        "DYLD_INSERT_LIBRARIES",
        "PROXY_DYLD_INSERT_LIBRARIES",
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (environment_entry_has_name(entry, names[index])) return true;
    }
    return false;
}

static char* make_environment_entry(const char* name, const char* value) {
    if (!name || !value) return NULL;
    size_t name_size = strlen(name);
    size_t value_size = strlen(value);
    if (name_size > SIZE_MAX - value_size - 2U) return NULL;
    char* entry = (char*)malloc(name_size + value_size + 2U);
    if (!entry) return NULL;
    memcpy(entry, name, name_size);
    entry[name_size] = '=';
    memcpy(entry + name_size + 1U, value, value_size + 1U);
    return entry;
}

static void free_compiler_environment(char** environment) {
    if (!environment) return;
    for (size_t index = 0; environment[index]; index++) {
        free(environment[index]);
    }
    free(environment);
}

static char** build_compiler_environment(
    const UnityCompilerChannel* channel) {
    if (!channel || !channel->dynamic_library_path) return NULL;
    size_t inherited_count = 0U;
    for (char** entry = environ; entry && *entry; entry++) {
        if (!compiler_environment_overrides(*entry)) inherited_count++;
    }
    const size_t override_count = 3U;
    if (inherited_count > SIZE_MAX - override_count - 1U) return NULL;
    size_t capacity = inherited_count + override_count + 1U;
    if (capacity > SIZE_MAX / sizeof(char*)) return NULL;
    char** environment = (char**)calloc(capacity, sizeof(*environment));
    if (!environment) return NULL;

    size_t count = 0U;
    for (char** entry = environ; entry && *entry; entry++) {
        if (compiler_environment_overrides(*entry)) continue;
        environment[count] = strdup(*entry);
        if (!environment[count++]) {
            free_compiler_environment(environment);
            return NULL;
        }
    }
#define APPEND_ENVIRONMENT(name, value) do { \
    environment[count] = make_environment_entry((name), (value)); \
    if (!environment[count++]) { \
        free_compiler_environment(environment); \
        return NULL; \
    } \
} while (0)
    APPEND_ENVIRONMENT("TMPDIR", "/tmp");
    APPEND_ENVIRONMENT("DYLD_LIBRARY_PATH", channel->dynamic_library_path);
    APPEND_ENVIRONMENT("LD_LIBRARY_PATH", channel->dynamic_library_path);
#undef APPEND_ENVIRONMENT
    return environment;
}

static _Thread_local int64_t g_io_deadline_ms = -1;

static bool monotonic_milliseconds(int64_t* out_milliseconds) {
    if (!out_milliseconds) return false;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
    if (now.tv_sec > INT64_MAX / 1000) return false;
    *out_milliseconds =
        (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
    return true;
}

static int compiler_io_timeout_ms(void) {
    const char* configured = getenv("DXBC_USC_IO_TIMEOUT_MS");
    if (!configured || !configured[0]) {
        return UNITY_COMPILER_DEFAULT_IO_TIMEOUT_MS;
    }
    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(configured, &end, 10);
    if (errno != 0 || end == configured || *end != '\0' || parsed == 0UL ||
        parsed > 3600000UL) {
        fprintf(stderr,
                "[CompilerClient] Ignoring invalid "
                "DXBC_USC_IO_TIMEOUT_MS='%s'\n",
                configured);
        return UNITY_COMPILER_DEFAULT_IO_TIMEOUT_MS;
    }
    return (int)parsed;
}

static bool io_deadline_begin(int timeout_ms) {
    int64_t now = 0;
    if (timeout_ms <= 0 || !monotonic_milliseconds(&now) ||
        now > INT64_MAX - timeout_ms) {
        g_io_deadline_ms = -1;
        return false;
    }
    g_io_deadline_ms = now + timeout_ms;
    return true;
}

static void io_deadline_end(void) {
    g_io_deadline_ms = -1;
}

static bool wait_for_socket(int fd, short events) {
    if (fd < 0 || g_io_deadline_ms < 0) {
        errno = EINVAL;
        return false;
    }
    for (;;) {
        int64_t now = 0;
        if (!monotonic_milliseconds(&now)) return false;
        int64_t remaining = g_io_deadline_ms - now;
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return false;
        }
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {
            .fd = fd,
            .events = events,
        };
        int result = poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        if (result < 0) return false;
        if ((descriptor.revents & events) != 0) return true;
        errno = ECONNRESET;
        return false;
    }
}

static bool set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool configure_client_socket(int fd) {
    if (!set_socket_nonblocking(fd)) return false;
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0) {
        return false;
    }
#endif
    return true;
}

static bool write_all(int fd, const void* data, size_t size) {
    const uint8_t* cursor = (const uint8_t*)data;
    while (size > 0) {
        if (!wait_for_socket(fd, POLLOUT)) return false;
        ssize_t written = send(fd, cursor, size,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
        );
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (written <= 0) return false;
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

static bool read_all(int fd, void* data, size_t size) {
    uint8_t* cursor = (uint8_t*)data;
    while (size > 0) {
        if (!wait_for_socket(fd, POLLIN)) return false;
        ssize_t received = read(fd, cursor, size);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (received <= 0) return false;
        cursor += (size_t)received;
        size -= (size_t)received;
    }
    return true;
}

static int start_tcp_listener(int* out_port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "[CompilerClient] socket failed: %s\n",
                strerror(errno));
        return -1;
    }
    
    // Allow address reuse
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    addr.sin_port = htons(0); // auto-allocate
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[CompilerClient] bind failed: %s\n",
                strerror(errno));
        close(listen_fd);
        return -1;
    }
    
    if (listen(listen_fd, 1) < 0) {
        fprintf(stderr, "[CompilerClient] listen failed: %s\n",
                strerror(errno));
        close(listen_fd);
        return -1;
    }
    
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd, (struct sockaddr*)&addr, &len) < 0) {
        fprintf(stderr, "[CompilerClient] getsockname failed: %s\n",
                strerror(errno));
        close(listen_fd);
        return -1;
    }
    
    *out_port = ntohs(addr.sin_port);
    return listen_fd;
}

static bool write_uint32(int fd, uint32_t val) {
    uint32_t buf[2] = { MAGIC_NUMBER, val };
    return write_all(fd, buf, sizeof(buf));
}

static bool write_int32(int fd, int32_t val) {
    return write_uint32(fd, (uint32_t)val);
}

static bool write_uint64(int fd, uint64_t val) {
    uint32_t magic = MAGIC_NUMBER;
    return write_all(fd, &magic, sizeof(magic)) &&
           write_all(fd, &val, sizeof(val));
}

static bool write_bool(int fd, bool val) {
    return write_uint32(fd, val ? 1 : 0);
}

static bool write_buffer(int fd, const void* data, size_t len) {
    uint32_t magic = MAGIC_NUMBER;
    uint64_t ulen = (uint64_t)len;
    return write_all(fd, &magic, sizeof(magic)) &&
           write_all(fd, &ulen, sizeof(ulen)) &&
           (len == 0 || write_all(fd, data, len));
}

static bool write_string(int fd, const char* str) {
    return write_buffer(fd, str, str ? strlen(str) : 0);
}

static bool write_keyword_array(int fd, char** kws, int count) {
    if (kws && count > 0) {
        if (!write_int32(fd, count)) return false;
        for (int i = 0; i < count; i++) {
            if (!write_string(fd, kws[i])) return false;
        }
    } else {
        if (!write_int32(fd, 0)) return false;
    }
    return true;
}

static bool read_uint32_check_magic(int fd, uint32_t* out_val) {
    uint32_t magic = 0;
    if (!read_all(fd, &magic, sizeof(magic))) return false;
    if (magic != MAGIC_NUMBER) {
        return false;
    }
    return read_all(fd, out_val, sizeof(uint32_t));
}

static bool read_int32(int fd, int32_t* out_val) {
    return read_uint32_check_magic(fd, (uint32_t*)out_val);
}

static bool read_uint64(int fd, uint64_t* out_val) {
    uint32_t magic = 0;
    if (!read_all(fd, &magic, sizeof(magic))) return false;
    if (magic != MAGIC_NUMBER) return false;
    return read_all(fd, out_val, sizeof(uint64_t));
}

static bool read_buffer(int fd, void** out_data, size_t* out_len) {
    uint32_t magic = 0;
    if (!read_all(fd, &magic, sizeof(magic))) return false;
    if (magic != MAGIC_NUMBER) return false;
    uint64_t len64 = 0;
    if (!read_all(fd, &len64, sizeof(len64))) return false;
    if (len64 > SIZE_MAX - 1 || len64 > UNITY_COMPILER_MAX_BUFFER_SIZE) {
        return false;
    }
    
    *out_len = (size_t)len64;
    if (len64 == 0) {
        *out_data = NULL;
        return true;
    }
    
    uint8_t* buf = (uint8_t*)malloc(len64 + 1);
    if (!buf) return false;
    
    if (!read_all(fd, buf, (size_t)len64)) {
        free(buf);
        return false;
    }
    buf[len64] = '\0';
    *out_data = buf;
    return true;
}

static char* read_string_with_size(int fd, size_t* out_size) {
    if (out_size) *out_size = 0U;
    void* data = NULL;
    size_t len = 0;
    if (!read_buffer(fd, &data, &len)) {
        return NULL;
    }
    if (!data) {
        return strdup("");
    }
    if (out_size) *out_size = len;
    return (char*)data;
}

static char* read_string(int fd) {
    return read_string_with_size(fd, NULL);
}

#define MAX_ACTIVE_COMPILERS 64

/*
 * Standalone clients may create independent channels, while the round-trip
 * verifier shares one broker-owned channel.  Keep process IDs, rather than
 * pointers to caller-owned channel objects, in the signal-visible registry.
 * sig_atomic_t entries can be inspected safely by the handler and the atomic
 * compare/exchange operations prevent channels claiming one slot.
 */
static volatile sig_atomic_t g_active_compiler_pids[MAX_ACTIVE_COMPILERS];
static volatile sig_atomic_t g_handlers_installed;

static bool reap_process_bounded(pid_t pid, int timeout_ms) {
    if (pid <= 0) return true;
    int64_t started = 0;
    if (timeout_ms < 0 || !monotonic_milliseconds(&started)) return false;
    for (;;) {
        int status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) return true;
        if (result < 0 && errno != EINTR) return false;
        int64_t now = 0;
        if (!monotonic_milliseconds(&now) || now - started >= timeout_ms) {
            return false;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
    }
}

static void terminate_and_reap_process(pid_t pid) {
    if (pid <= 0) return;
    if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
        fprintf(stderr,
                "[CompilerClient] Failed to terminate process %d: %s\n",
                (int)pid, strerror(errno));
    }
    if (reap_process_bounded(pid, UNITY_COMPILER_SHUTDOWN_GRACE_MS)) return;
    if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
        fprintf(stderr,
                "[CompilerClient] Failed to kill process %d: %s\n",
                (int)pid, strerror(errno));
    }
    if (!reap_process_bounded(pid, UNITY_COMPILER_KILL_GRACE_MS)) {
        fprintf(stderr,
                "[CompilerClient] Timed out reaping compiler process %d\n",
                (int)pid);
    }
}

static void unregister_compiler_process(pid_t pid) {
    if (pid <= 0) return;
    for (int i = 0; i < MAX_ACTIVE_COMPILERS; i++) {
        __sync_bool_compare_and_swap(&g_active_compiler_pids[i],
                                     (sig_atomic_t)pid, 0);
    }
}

static bool register_compiler_process(pid_t pid) {
    for (int i = 0; i < MAX_ACTIVE_COMPILERS; i++) {
        if (__sync_bool_compare_and_swap(&g_active_compiler_pids[i], 0,
                                        (sig_atomic_t)pid)) {
            return true;
        }
    }
    return false;
}

static void invalidate_compiler_channel(UnityCompilerChannel* channel) {
    if (!channel) return;
    int socket_fd = channel->socket_fd;
    pid_t pid = channel->process_id;
    channel->socket_fd = -1;
    channel->process_id = 0;
    if (socket_fd >= 0) {
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
    }
    terminate_and_reap_process(pid);
    unregister_compiler_process(pid);
}

static void cleanup_compiler_processes(void) {
    for (int i = 0; i < MAX_ACTIVE_COMPILERS; i++) {
        pid_t pid = (pid_t)__sync_lock_test_and_set(
            &g_active_compiler_pids[i], 0);
        terminate_and_reap_process(pid);
    }
}

static void handle_signal(int sig) {
    /* kill(2) and sigaction(2) are async-signal-safe; cleanup/reaping occurs
     * during ordinary shutdown.  On forced parent termination, killing every
     * registered child prevents compiler processes surviving the verifier. */
    for (int i = 0; i < MAX_ACTIVE_COMPILERS; i++) {
        pid_t pid = (pid_t)g_active_compiler_pids[i];
        if (pid > 0) kill(pid, SIGTERM);
    }
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(sig, &sa, NULL);
    kill(getpid(), sig);
}

static void install_process_handlers(void) {
    if (!__sync_bool_compare_and_swap(&g_handlers_installed, 0, 1)) return;

    atexit(cleanup_compiler_processes);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

static int unity_compiler_include_paths(
    const UnityCompilerChannel* channel, char* include_paths[3]) {
    if (!channel || !include_paths) return 0;
    int count = 0;
#define APPEND_UNIQUE_INCLUDE(path_value) do { \
    const char* candidate = (path_value); \
    if (candidate && candidate[0]) { \
        bool duplicate = false; \
        for (int index = 0; index < count; ++index) { \
            if (strcmp(include_paths[index], candidate) == 0) { \
                duplicate = true; \
                break; \
            } \
        } \
        if (!duplicate) include_paths[count++] = (char*)candidate; \
    } \
} while (0)
    APPEND_UNIQUE_INCLUDE(channel->includes_dir);
    if (channel->has_sandbox_includes) {
        APPEND_UNIQUE_INCLUDE(channel->sandbox_includes_dir);
    }
    APPEND_UNIQUE_INCLUDE(channel->builtin_includes_dir);
#undef APPEND_UNIQUE_INCLUDE
    return count;
}

static bool read_session_capabilities(
    int fd, UnityCompilerSessionCapabilities* out_capabilities) {
    if (!out_capabilities) return false;
    UnityCompilerSessionCapabilities parsed;
    memset(&parsed, 0, sizeof(parsed));
    int32_t raw_mask = 0;
    if (!read_int32(fd, &raw_mask)) return false;
    parsed.raw_available_platform_mask = (uint32_t)raw_mask;
    for (size_t index = 0; index < UNITY_COMPILER_PLATFORM_COUNT; index++) {
        if (!read_uint64(
                fd, &parsed.platforms[index].supported_features) ||
            !read_int32(fd, &parsed.platforms[index].version)) {
            return false;
        }
    }
    if (!unity_compiler_session_capabilities_validate(&parsed)) {
        fprintf(stderr,
                "[CompilerClient] initializeCompiler returned an invalid "
                "platform mask 0x%08x\n",
                parsed.raw_available_platform_mask);
        return false;
    }
    *out_capabilities = parsed;
    return true;
}

static bool unity_compiler_start_process(UnityCompilerChannel* channel) {
    if (!channel || !channel->configured ||
        !capture_or_validate_toolchain_authority(channel)) {
        return false;
    }
    UnityCompilerValidApisAuthority preflight_authority;
    expected_valid_apis_authority_snapshot(
        channel, &preflight_authority);
    if (expected_valid_apis_authority_is_rejection(
            &preflight_authority)) {
        return false;
    }
    if (channel->socket_fd >= 0 && channel->process_id > 0) return true;
    if (channel->socket_fd >= 0 || channel->process_id > 0) {
        invalidate_compiler_channel(channel);
    }
    if (!channel->compiler_path || access(channel->compiler_path, X_OK) != 0) {
        fprintf(stderr, "[CompilerClient] UnityShaderCompiler is unavailable "
                        "or not executable at %s\n",
                channel->compiler_path ? channel->compiler_path : "(unset)");
        return false;
    }
    if (!channel->builtin_includes_dir ||
        access(channel->builtin_includes_dir, R_OK) != 0) {
        fprintf(stderr, "[CompilerClient] Unity builtin includes are "
                        "unavailable at %s\n",
                channel->builtin_includes_dir
                    ? channel->builtin_includes_dir
                    : "(unset)");
        return false;
    }

    install_process_handlers();

    int port = 0;
    int listen_fd = start_tcp_listener(&port);
    if (listen_fd < 0) {
        fprintf(stderr, "[CompilerClient] Failed to start TCP listener\n");
        return false;
    }
    if (!set_socket_nonblocking(listen_fd)) {
        close(listen_fd);
        return false;
    }
    
    char port_str[16];
    char log_str[64];
    if (snprintf(port_str, sizeof(port_str), "%d", port) <= 0 ||
        snprintf(log_str, sizeof(log_str), "/tmp/usc_log_%d", port) <= 0) {
        close(listen_fd);
        return false;
    }
    char* child_argv[] = {
        channel->compiler_path,
        channel->unity_contents_path,
        log_str,
        port_str,
        channel->playback_engines_dir,
        NULL,
    };
    char** child_environment = build_compiler_environment(channel);
    if (!child_environment) {
        close(listen_fd);
        return false;
    }
    posix_spawn_file_actions_t file_actions;
    int spawn_error = posix_spawn_file_actions_init(&file_actions);
    bool file_actions_initialized = spawn_error == 0;
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_addclose(
            &file_actions, listen_fd);
    }
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_addopen(
            &file_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_addopen(
            &file_actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    pid_t pid = 0;
    bool pid_registered = false;
    if (spawn_error == 0) {
        spawn_error = posix_spawn(
            &pid, channel->compiler_path, &file_actions, NULL,
            child_argv, child_environment);
        if (spawn_error == 0) {
            /* Register before any allocation cleanup: an asynchronous parent
             * signal from this point onward must be able to terminate the
             * newly spawned compiler. */
            pid_registered = register_compiler_process(pid);
        }
    }
    if (file_actions_initialized) {
        (void)posix_spawn_file_actions_destroy(&file_actions);
    }
    free_compiler_environment(child_environment);
    if (spawn_error != 0) {
        fprintf(stderr, "[CompilerClient] posix_spawn failed: %s\n",
                strerror(spawn_error));
        close(listen_fd);
        return false;
    }
    if (!pid_registered) {
        fprintf(stderr, "[CompilerClient] Too many active compiler processes\n");
        close(listen_fd);
        terminate_and_reap_process(pid);
        return false;
    }

    if (!io_deadline_begin(UNITY_COMPILER_STARTUP_TIMEOUT_MS)) {
        close(listen_fd);
        terminate_and_reap_process(pid);
        unregister_compiler_process(pid);
        return false;
    }

    // Parent process: wait for the connection and complete the full
    // initialization exchange under one absolute startup deadline.
    int client_fd = -1;
    for (;;) {
        if (!wait_for_socket(listen_fd, POLLIN)) {
            fprintf(stderr,
                    "[CompilerClient] Timeout waiting for "
                    "UnityShaderCompiler to connect\n");
            goto startup_failure;
        }
        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd >= 0) break;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            goto startup_failure;
        }
    }
    close(listen_fd);
    listen_fd = -1;
    if (!configure_client_socket(client_fd)) goto startup_failure;
    
    char* hs = read_string(client_fd);
    if (!hs) {
        fprintf(stderr, "[CompilerClient] Failed to read handshake\n");
        goto startup_failure;
    }
    if (hs[0] != '\0') {
        fprintf(stderr, "[CompilerClient] Handshake error: %s\n", hs);
        free(hs);
        goto startup_failure;
    }
    free(hs);
    
    // Send initializeCompiler
    if (!write_string(client_fd, "initializeCompiler")) {
        goto startup_failure;
    }
    
    char* include_paths[3];
    int include_count = unity_compiler_include_paths(
        channel, include_paths);
    if (include_count <= 0) goto startup_failure;
    if (!write_int32(client_fd, include_count)) {
        goto startup_failure;
    }
    for (int index = 0; index < include_count; ++index) {
        if (!write_string(client_fd, include_paths[index])) {
            goto startup_failure;
        }
    }
    
    if (!write_int32(client_fd, 0)) {
        goto startup_failure;
    }
    if (!write_string(client_fd, UNITY_TOOLCHAIN_CONFIGURATION)) {
        goto startup_failure;
    }
    
    UnityCompilerSessionCapabilities session_capabilities;
    if (!read_session_capabilities(client_fd, &session_capabilities)) {
        goto startup_failure;
    }
    if (channel->session_capabilities_ready &&
        !unity_compiler_session_capabilities_equal(
            &channel->session_capabilities, &session_capabilities)) {
        /* The toolchain lease still describes one immutable content snapshot.
         * A replacement process returning different capabilities would make
         * that snapshot non-deterministic, so never continue or reuse it. */
        fprintf(stderr,
                "[CompilerClient] initializeCompiler capability record "
                "changed within one pinned toolchain snapshot\n");
        memset(&channel->session_capabilities, 0,
               sizeof(channel->session_capabilities));
        channel->session_capabilities_ready = false;
        channel->session_capabilities_failed = true;
        goto startup_failure;
    }
    channel->session_capabilities = session_capabilities;
    channel->session_capabilities_ready = true;

    /* initializeCompiler is the source of truth.  Publish its complete typed
     * record first so callers can inspect a deterministic mismatch, then
     * reject the child before any preprocess/compile/disassemble command is
     * written.  A caller may correct or clear the expectation and retry
     * without discarding the immutable session record. */
    if (!expected_valid_apis_authority_permits_live_command(channel)) {
        UnityCompilerValidApisAuthority authority;
        expected_valid_apis_authority_snapshot(channel, &authority);
        fprintf(
            stderr,
            "[CompilerClient] expected valid_apis=%" PRIu32
            " does not exactly match initializeCompiler valid_apis=%" PRIu32
            "\n",
            authority.expected_valid_apis,
            authority.observed_valid_apis);
        goto startup_failure;
    }

    channel->socket_fd = client_fd;
    channel->process_id = pid;
    io_deadline_end();
    return true;

startup_failure:
    io_deadline_end();
    if (listen_fd >= 0) close(listen_fd);
    if (client_fd >= 0) close(client_fd);
    terminate_and_reap_process(pid);
    unregister_compiler_process(pid);
    return false;
}

static bool begin_compiler_transaction(UnityCompilerChannel* channel) {
    if (!unity_compiler_start_process(channel)) return false;
    /* This is intentionally repeated after initialization and immediately
     * before the command deadline/write.  It covers already-running channels
     * whose expected authority was changed between transactions. */
    if (!expected_valid_apis_authority_permits_live_command(channel)) {
        return false;
    }
    if (io_deadline_begin(compiler_io_timeout_ms())) return true;
    invalidate_compiler_channel(channel);
    return false;
}

static void finish_compiler_transaction(void) {
    io_deadline_end();
}

static void fail_compiler_transaction(UnityCompilerChannel* channel) {
    io_deadline_end();
    invalidate_compiler_channel(channel);
}

static bool unity_compiler_configure_channel(
    UnityCompilerChannel* channel,
    const char* project_root,
    const char* includes_dir,
    bool lazy_start) {
    if (!channel) return false;
    memset(channel, 0, sizeof(*channel));
    channel->socket_fd = -1;
    if (!configure_toolchain_paths(channel)) {
        unity_compiler_shutdown(channel);
        return false;
    }
    channel->project_root = strdup(project_root ? project_root : "");
    channel->includes_dir = strdup(includes_dir ? includes_dir : "");
    channel->sandbox_includes_dir = path_with_suffix(
        project_root, "/shader_includes");
    if (!channel->project_root || !channel->includes_dir ||
        !channel->sandbox_includes_dir) {
        unity_compiler_shutdown(channel);
        return false;
    }
    channel->has_sandbox_includes =
        access(channel->sandbox_includes_dir, R_OK) == 0;
    channel->configured = true;

    if (lazy_start) return true;
    const char* cache_dir = getenv("DXBC_USC_CACHE_DIR");
    if (cache_dir && cache_dir[0]) {
        /* A warm cache must not pay the process startup/RAM cost. */
        return true;
    }
    if (unity_compiler_start_process(channel)) return true;
    unity_compiler_shutdown(channel);
    return false;
}

bool unity_compiler_start(
    UnityCompilerChannel* channel,
    const char* project_root,
    const char* includes_dir) {
    return unity_compiler_configure_channel(
        channel, project_root, includes_dir, false);
}

bool unity_compiler_start_lazy(
    UnityCompilerChannel* channel,
    const char* project_root,
    const char* includes_dir) {
    return unity_compiler_configure_channel(
        channel, project_root, includes_dir, true);
}

bool unity_compiler_session_capabilities_snapshot(
    const UnityCompilerChannel* channel,
    UnityCompilerSessionCapabilities* out_capabilities) {
    if (!channel || !channel->configured ||
        !channel->session_capabilities_ready ||
        channel->session_capabilities_failed || !out_capabilities ||
        !unity_compiler_session_capabilities_validate(
            &channel->session_capabilities)) {
        return false;
    }
    *out_capabilities = channel->session_capabilities;
    return true;
}

bool unity_compiler_capture_session_capabilities(
    UnityCompilerChannel* channel,
    UnityCompilerSessionCapabilities* out_capabilities) {
    if (!channel || !channel->configured || !out_capabilities) return false;
    if (!channel->session_capabilities_ready) {
        /* Cache-only is an explicit promise that this operation will not
         * create a compiler process merely to acquire missing authority. */
        if (compiler_cache_only_enabled() ||
            !unity_compiler_start_process(channel)) {
            return false;
        }
    }
    return unity_compiler_session_capabilities_snapshot(
        channel, out_capabilities);
}

static bool seal_pending(
    PreprocessedSnippet** snippets,
    int* snippet_count,
    int* snippet_capacity,
    char** pending_source,
    SnippetCompileContract* pending_contract,
    bool* pending_contract_complete
) {
    if (!*pending_source) return true;
    if (!*pending_contract_complete ||
        !unity_compiler_snippet_contract_validate(pending_contract)) {
        return false;
    }
    if (*snippet_count >= *snippet_capacity) {
        if (*snippet_capacity > INT_MAX / 2) return false;
        int new_capacity = *snippet_capacity == 0
            ? 4
            : *snippet_capacity * 2;
        if ((size_t)new_capacity >
            SIZE_MAX / sizeof(PreprocessedSnippet)) {
            return false;
        }
        PreprocessedSnippet* resized = (PreprocessedSnippet*)realloc(
            *snippets,
            (size_t)new_capacity * sizeof(PreprocessedSnippet));
        if (!resized) return false;
        *snippets = resized;
        *snippet_capacity = new_capacity;
    }
    PreprocessedSnippet* snippet = &(*snippets)[*snippet_count];
    memset(snippet, 0, sizeof(*snippet));
    snippet->source = *pending_source;
    snippet->contract = *pending_contract;
    snippet->has_contract = true;
    snippet->reqs = snippet->contract.requirements;
    snippet->language = snippet->contract.language;
    snippet->gpu_program_id = snippet->contract.snippet_id;
    snippet->conditional_requirements =
        snippet->contract.conditional_requirements;
    snippet->conditional_requirement_count =
        snippet->contract.conditional_requirement_count;
    (*snippet_count)++;
    
    *pending_source = NULL;
    unity_compiler_snippet_contract_init(pending_contract);
    *pending_contract_complete = false;
    return true;
}

static void free_preprocess_parse_state(
    PreprocessedSnippet* snippets,
    int snippet_count,
    char* pending_source,
    SnippetCompileContract* pending_contract) {
    PreprocessResult result = {
        .snippets = snippets,
        .snippet_count = snippet_count,
    };
    unity_compiler_free_preprocess(&result);
    free(pending_source);
    unity_compiler_snippet_contract_free(pending_contract);
}

static bool parse_keyword_variant_header(
    const char* header, const char* prefix, int32_t* snippet_id,
    int32_t* compiler_program, int32_t* count) {
    if (!header || !prefix || !snippet_id || !compiler_program || !count) {
        return false;
    }
    size_t prefix_length = strlen(prefix);
    if (strncmp(header, prefix, prefix_length) != 0) return false;
    const char* cursor = header + prefix_length;
    if (!parse_header_i32(&cursor, snippet_id) ||
        !parse_header_i32(&cursor, compiler_program) ||
        !parse_header_i32(&cursor, count)) {
        return false;
    }
    while (isspace((unsigned char)*cursor)) cursor++;
    return *cursor == '\0';
}

static bool contract_variant_programs_complete(
    const SnippetCompileContract* contract) {
    if (!contract) return false;
    for (int i = 0; i < contract->program_keyword_variant_count; i++) {
        const SnippetProgramKeywordVariants* program =
            &contract->program_keyword_variants[i];
        if (program->compiler_program < 0 ||
            program->compiler_program >= 32 ||
            !(contract->program_types_mask &
              (UINT32_C(1) << (uint32_t)program->compiler_program)) ||
            !program->user_global.present || !program->user_local.present ||
            !program->builtin.present) {
            return false;
        }
    }
    for (int program = 0; program < 32; program++) {
        if ((contract->program_types_mask &
             (UINT32_C(1) << (uint32_t)program)) &&
            !unity_compiler_snippet_contract_has_variant_families(
                contract, program)) {
            return false;
        }
    }
    return true;
}

static char* get_shader_file_path(const char* shader_name) {
    static const char assets_prefix[] = "Assets/";
    static const char packages_prefix[] = "Packages/";
    static const char shader_suffix[] = ".shader";
    if (!shader_name) return NULL;

    size_t shader_name_size = strlen(shader_name);
    if (strncmp(shader_name, assets_prefix,
                sizeof(assets_prefix) - 1U) == 0 ||
        strncmp(shader_name, packages_prefix,
                sizeof(packages_prefix) - 1U) == 0) {
        if (shader_name_size == SIZE_MAX) return NULL;
        char* path = (char*)malloc(shader_name_size + 1U);
        if (!path) return NULL;
        memcpy(path, shader_name, shader_name_size + 1U);
        return path;
    }

    const size_t prefix_size = sizeof(assets_prefix) - 1U;
    const size_t suffix_size = sizeof(shader_suffix) - 1U;
    if (shader_name_size > SIZE_MAX - prefix_size - suffix_size - 1U) {
        return NULL;
    }
    size_t path_size = prefix_size + shader_name_size + suffix_size;
    char* path = (char*)malloc(path_size + 1U);
    if (!path) return NULL;
    memcpy(path, assets_prefix, prefix_size);
    memcpy(path + prefix_size, shader_name, shader_name_size);
    memcpy(path + prefix_size + shader_name_size, shader_suffix,
           suffix_size + 1U);
    return path;
}

static bool preprocess_request_to_wire(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* input_request,
    const uint8_t environment_fingerprint[USC_CACHE_DIGEST_SIZE],
    char* include_paths[3],
    UnityCompilerPreprocessRequest* out_request) {
    if (!channel || !channel->configured || !input_request ||
        !input_request->source || !input_request->file_path ||
        !input_request->shader_name || !environment_fingerprint ||
        !include_paths || !out_request ||
        !valid_owned_string_array(input_request->keywords,
                                  input_request->keyword_count) ||
        !valid_owned_string_array(input_request->defines,
                                  input_request->define_count)) {
        return false;
    }
    const int include_path_count = unity_compiler_include_paths(
        channel, include_paths);
    UnityCompilerPreprocessRequest request = {
        .command = "preprocess",
        .source = input_request->source,
        .file_path = input_request->file_path,
        .shader_name = input_request->shader_name,
        .surface_only = input_request->surface_only,
        .caching_preprocessor = input_request->caching_preprocessor,
        .build_platform = input_request->build_platform,
        .valid_apis = input_request->valid_apis,
        .keywords = input_request->keywords,
        .keyword_count = input_request->keyword_count,
        .defines = input_request->defines,
        .define_count = input_request->define_count,
        .include_paths = include_paths,
        .include_path_count = include_path_count,
        .toolchain_configuration = UNITY_TOOLCHAIN_CONFIGURATION,
        .environment_fingerprint = environment_fingerprint,
    };
    *out_request = request;
    return true;
}

/* Validate without acquiring missing authority.  `require_captured` is used
 * only after begin_compiler_transaction has started/initialized a live
 * process.  Cache replay passes false so a cold warm-cache run stays
 * process-free, while an already captured session still guards every replay. */
static bool validate_captured_session_valid_apis(
    const UnityCompilerChannel* channel, uint32_t supplied_valid_apis,
    bool require_captured) {
    if (!channel || channel->session_capabilities_failed) return false;
    if (!channel->session_capabilities_ready) return !require_captured;
    if (unity_compiler_session_capabilities_match_valid_apis(
            &channel->session_capabilities, supplied_valid_apis)) {
        return true;
    }

    uint32_t session_valid_apis = 0U;
    (void)unity_compiler_session_capabilities_valid_apis(
        &channel->session_capabilities, &session_valid_apis);
    char* session_report = unity_compiler_session_capabilities_format_json(
        &channel->session_capabilities);
    fprintf(
        stderr,
        "[CompilerClient] preprocess valid_apis=%" PRIu32
        " does not exactly match initializeCompiler valid_apis=%" PRIu32
        "%s%s\n",
        supplied_valid_apis, session_valid_apis,
        session_report ? "; session=" : "",
        session_report ? session_report : "");
    free(session_report);
    return false;
}

bool unity_compiler_preprocess_contract_response(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* input_request,
    UnityCompilerPreprocessResponse* out_response) {
    if (!out_response) {
        return false;
    }
    unity_compiler_preprocess_response_init(out_response);
    PreprocessResult* out_result = &out_response->result;
    UnityCompilerResponseStatus* response_status = &out_response->status;

    char* include_paths[3];
    uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE];
    uint8_t environment_fingerprint[USC_CACHE_DIGEST_SIZE];
    UnityCompilerPreprocessRequest request;
    if (!get_toolchain_fingerprints(channel, compiler_fingerprint,
                                    environment_fingerprint) ||
        !preprocess_request_to_wire(
            channel, input_request, environment_fingerprint,
            include_paths, &request)) {
        return false;
    }
    response_status_capture_valid_apis_authority(response_status, channel);

    const char* cache_dir = getenv("DXBC_USC_CACHE_DIR");
    uint8_t request_digest[USC_CACHE_DIGEST_SIZE];
    bool cache_ready = false;
    if (cache_dir && cache_dir[0]) {
        usc_cache_preprocess_request_digest(
            &request, compiler_fingerprint, request_digest);
        uint8_t* cached_data = NULL;
        size_t cached_size = 0;
        cache_ready = true;
        if (usc_cache_load(cache_dir, request_digest, &cached_data,
                           &cached_size) == USC_CACHE_HIT) {
            bool decoded = usc_cache_deserialize_preprocess_response(
                cached_data, cached_size, out_response);
            free(cached_data);
            if (decoded) {
                const bool toolchain_valid =
                    validate_existing_toolchain_authority(channel);
                response_status_capture_valid_apis_authority(
                    response_status, channel);
                if (expected_valid_apis_authority_is_rejection(
                        &response_status->valid_apis_authority)) {
                    response_status->from_cache = true;
                    return true;
                }
                if (toolchain_valid &&
                    validate_captured_session_valid_apis(
                        channel, request.valid_apis, false)) {
                    response_status->from_cache = true;
                    return true;
                }
                unity_compiler_preprocess_response_free(out_response);
                return false;
            }
            /* The outer entry was intact, but the typed payload was not.
             * Treat it as a miss and let a successful compiler response
             * atomically replace it. */
            unity_compiler_preprocess_response_init(out_response);
            out_result = &out_response->result;
            response_status = &out_response->status;
        }
    }

    if (compiler_cache_only_enabled()) {
        response_status->availability =
            UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS;
        response_status_capture_valid_apis_authority(
            response_status, channel);
        return true;
    }

    PreprocessedSnippet* snippets = NULL;
    int snippet_count = 0;
    int snippet_capacity = 0;
    char* pending_source = NULL;
    SnippetCompileContract pending_contract;
    unity_compiler_snippet_contract_init(&pending_contract);
    bool pending_contract_complete = false;
    char* msg = NULL;

    if (!begin_compiler_transaction(channel)) {
        response_status_capture_valid_apis_authority(
            response_status, channel);
        return expected_valid_apis_authority_is_rejection(
            &response_status->valid_apis_authority);
    }
    response_status_capture_valid_apis_authority(response_status, channel);
    if (!validate_captured_session_valid_apis(
            channel, request.valid_apis, true)) {
        finish_compiler_transaction();
        free_preprocess_parse_state(
            snippets, snippet_count, pending_source, &pending_contract);
        unity_compiler_preprocess_response_free(out_response);
        return false;
    }
    int fd = channel->socket_fd;
    if (!write_string(fd, request.command) ||
        !write_string(fd, request.source) ||
        !write_string(fd, request.file_path) ||
        !write_string(fd, request.shader_name) ||
        !write_bool(fd, request.surface_only) ||
        !write_bool(fd, request.caching_preprocessor) ||
        !write_uint32(fd, request.build_platform) ||
        !write_uint32(fd, request.valid_apis) ||
        !write_keyword_array(fd, request.keywords, request.keyword_count) ||
        !write_keyword_array(fd, request.defines, request.define_count)) {
        goto preprocess_parse_failure;
    }

    const int max_response_count = 1024 * 1024;
    while (true) {
        msg = read_string(fd);
        if (!msg) goto preprocess_parse_failure;

        if (strncmp(msg, "shader:", 7) == 0) {
            UnityCompilerPreprocessStatus status;
            bool parsed_status =
                unity_compiler_parse_preprocess_status_record(msg, &status);
            if (!parsed_status) goto preprocess_parse_failure;
            void* blob_data = NULL;
            size_t blob_size = 0;
            bool read_blob = read_buffer(fd, &blob_data, &blob_size);
            free(msg);
            msg = NULL;
            if (!read_blob) {
                free(blob_data);
                goto preprocess_parse_failure;
            }
            response_status->compiler_success = status.primary_success;
            if (!status.primary_success) {
                free(blob_data);
                free_preprocess_parse_state(
                    snippets, snippet_count, pending_source,
                    &pending_contract);
                finish_compiler_transaction();
                if (!validate_existing_toolchain_authority(channel)) {
                    unity_compiler_preprocess_response_free(out_response);
                    return false;
                }
                return true;
            }
            if (!seal_pending(
                    &snippets, &snippet_count, &snippet_capacity,
                    &pending_source, &pending_contract,
                    &pending_contract_complete)) {
                free(blob_data);
                goto preprocess_parse_failure;
            }

            out_result->snippets = snippets;
            out_result->snippet_count = snippet_count;
            out_result->blob = (uint8_t*)blob_data;
            out_result->blob_len = blob_size;
            finish_compiler_transaction();
            if (!validate_existing_toolchain_authority(channel)) {
                unity_compiler_free_preprocess(out_result);
                return false;
            }
            if (cache_ready) {
                uint8_t* serialized = NULL;
                size_t serialized_size = 0;
                if (usc_cache_serialize_preprocess_response(
                        out_response, &serialized, &serialized_size)) {
                    (void)usc_cache_store(cache_dir, request_digest,
                                          serialized, serialized_size);
                }
                free(serialized);
            }
            return true;
        } else if (strncmp(msg, "snip:", 5) == 0) {
            if (!seal_pending(
                    &snippets, &snippet_count, &snippet_capacity,
                    &pending_source, &pending_contract,
                    &pending_contract_complete)) {
                goto preprocess_parse_failure;
            }
            if (!unity_compiler_snippet_contract_parse_header(
                    msg, &pending_contract)) {
                goto preprocess_parse_failure;
            }
            pending_source = read_string(fd);
            if (!pending_source) goto preprocess_parse_failure;
            if (getenv("DXBC_DEBUG_SNIPPETS")) {
                fprintf(stderr, "    [Preprocess Snippet] %s\n", msg);
            }
        } else if (strncmp(msg, "includes:", 9) == 0 ||
                   strncmp(msg, "filepaths:", 10) == 0) {
            int32_t count = -1;
            const char* prefix = strncmp(msg, "includes:", 9) == 0
                ? "includes:"
                : "filepaths:";
            if (!parse_exact_i32_record(msg, prefix, 1U, &count)) {
                goto preprocess_parse_failure;
            }
            if (count < 0 || count > max_response_count) {
                goto preprocess_parse_failure;
            }
            /* Dependency paths are consumed exactly but are not a second
             * source of artifact identity: the serialized preprocess result
             * contains the emitted snippets/blob, while the request identity
             * hashes all resolved include trees and toolchain inputs. */
            for (int i = 0; i < count; i++) {
                char* dependency = read_string(fd);
                if (!dependency) goto preprocess_parse_failure;
                free(dependency);
            }
        } else if (strncmp(msg, "keywordsUserGlobal:", 19) == 0 ||
                   strncmp(msg, "keywordsUserLocal:", 18) == 0 ||
                   strncmp(msg, "keywordsBuiltin:", 16) == 0) {
            UnityKeywordVariantFamily family;
            const char* prefix = NULL;
            bool family_in_sequence = false;
            const SnippetProgramKeywordVariants* last_program =
                pending_contract.program_keyword_variant_count > 0
                    ? &pending_contract.program_keyword_variants[
                          pending_contract.program_keyword_variant_count - 1]
                    : NULL;
            if (strncmp(msg, "keywordsUserGlobal:", 19) == 0) {
                family = UNITY_KEYWORD_VARIANTS_USER_GLOBAL;
                prefix = "keywordsUserGlobal:";
                family_in_sequence =
                    !last_program ||
                    (last_program->user_global.present &&
                     last_program->user_local.present &&
                     last_program->builtin.present);
            } else if (strncmp(msg, "keywordsUserLocal:", 18) == 0) {
                family = UNITY_KEYWORD_VARIANTS_USER_LOCAL;
                prefix = "keywordsUserLocal:";
                family_in_sequence =
                    last_program && last_program->user_global.present &&
                    !last_program->user_local.present &&
                    !last_program->builtin.present;
            } else {
                family = UNITY_KEYWORD_VARIANTS_BUILTIN;
                prefix = "keywordsBuiltin:";
                family_in_sequence =
                    last_program && last_program->user_global.present &&
                    last_program->user_local.present &&
                    !last_program->builtin.present;
            }
            int32_t keyword_snippet_id = -1;
            int32_t compiler_program = -1;
            int32_t count = -1;
            if (!pending_source || pending_contract_complete ||
                !family_in_sequence ||
                !parse_keyword_variant_header(
                    msg, prefix, &keyword_snippet_id, &compiler_program,
                    &count) ||
                keyword_snippet_id != pending_contract.snippet_id ||
                compiler_program < 0) {
                goto preprocess_parse_failure;
            }
            if ((family == UNITY_KEYWORD_VARIANTS_USER_GLOBAL &&
                 unity_compiler_snippet_contract_find_program_variants(
                     &pending_contract, compiler_program)) ||
                (family != UNITY_KEYWORD_VARIANTS_USER_GLOBAL &&
                 (!last_program || last_program->compiler_program !=
                                      compiler_program))) {
                goto preprocess_parse_failure;
            }
            if (count < 0 || count > max_response_count) {
                goto preprocess_parse_failure;
            }

            char** combinations = NULL;
            if (count > 0) {
                combinations = (char**)calloc(
                    (size_t)count, sizeof(*combinations));
                if (!combinations) goto preprocess_parse_failure;
            }
            bool read_all_combinations = true;
            for (int i = 0; i < count; i++) {
                combinations[i] = read_string(fd);
                if (!combinations[i]) {
                    read_all_combinations = false;
                    break;
                }
                if (getenv("DXBC_DEBUG_SNIPPETS")) {
                    fprintf(stderr,
                            "    [Preprocess Keyword Combination] "
                            "program=%d family=%d row=%d value='%s'\n",
                            compiler_program, (int)family, i,
                            combinations[i]);
                }
            }
            if (!read_all_combinations ||
                !unity_compiler_snippet_contract_set_variant_combinations(
                    &pending_contract, compiler_program, family,
                    (const char* const*)combinations, count)) {
                free_string_array(combinations, count);
                goto preprocess_parse_failure;
            }
            free_string_array(combinations, count);
        } else if (strncmp(msg, "keywordsEnd:", 12) == 0) {
            int32_t keyword_snippet_id = 0;
            const char* id_cursor = msg + 12;
            if (!pending_source || pending_contract_complete ||
                !contract_variant_programs_complete(&pending_contract) ||
                !parse_header_i32(&id_cursor, &keyword_snippet_id)) {
                goto preprocess_parse_failure;
            }
            while (isspace((unsigned char)*id_cursor)) id_cursor++;
            if (*id_cursor != '\0' ||
                keyword_snippet_id != pending_contract.snippet_id) {
                goto preprocess_parse_failure;
            }
            char* non_stripped_user_keywords = read_string(fd);
            char* builtin_keywords = read_string(fd);
            if (!non_stripped_user_keywords || !builtin_keywords) {
                free(non_stripped_user_keywords);
                free(builtin_keywords);
                goto preprocess_parse_failure;
            }
            bool parsed_keywords =
                unity_compiler_snippet_contract_set_keyword_lines(
                    &pending_contract, non_stripped_user_keywords,
                    builtin_keywords);
            if (getenv("DXBC_DEBUG_SNIPPETS")) {
                fprintf(stderr,
                        "    [Preprocess Keyword Universes] "
                        "non-stripped-user='%s' builtin='%s'\n",
                        non_stripped_user_keywords, builtin_keywords);
            }
            free(non_stripped_user_keywords);
            free(builtin_keywords);
            if (!parsed_keywords ||
                !read_uint64(fd, &pending_contract.requirements)) {
                goto preprocess_parse_failure;
            }

            int32_t pairs = 0;
            if (!read_int32(fd, &pairs) || pairs < 0 ||
                pairs > max_response_count ||
                pending_contract.conditional_requirements) {
                goto preprocess_parse_failure;
            }
            if (pairs > 0) {
                pending_contract.conditional_requirements =
                    (ConditionalShaderRequirement*)calloc(
                        (size_t)pairs,
                        sizeof(*pending_contract.conditional_requirements));
                if (!pending_contract.conditional_requirements) {
                    goto preprocess_parse_failure;
                }
            }
            for (int i = 0; i < pairs; i++) {
                char* name = read_string(fd);
                uint64_t variant_requirements = 0;
                if (!name || !read_uint64(fd, &variant_requirements)) {
                    free(name);
                    goto preprocess_parse_failure;
                }
                pending_contract.conditional_requirements[i].keyword = name;
                pending_contract.conditional_requirements[i].requirements =
                    variant_requirements;
                pending_contract.conditional_requirement_count++;
            }
            pending_contract_complete =
                unity_compiler_snippet_contract_validate(
                    &pending_contract);
            if (!pending_contract_complete) goto preprocess_parse_failure;
        } else if (strncmp(msg, "err:", 4) == 0) {
            int32_t error_fields[3];
            if (!parse_error_record(msg, error_fields)) {
                goto preprocess_parse_failure;
            }
            /* ShaderCompilerErrorReportSend writes exactly two framed strings
             * after the line record: filename then message.  They are
             * diagnostics rather than preprocess artifact identity, but must
             * be consumed structurally to keep the persistent stream aligned. */
            char* error_file = read_string(fd);
            char* error_message = read_string(fd);
            if (!error_file || !error_message) {
                free(error_file);
                free(error_message);
                goto preprocess_parse_failure;
            }
            if (!response_status_append_diagnostic(
                    response_status, error_fields, msg, error_file,
                    error_message)) {
                free(error_file);
                free(error_message);
                goto preprocess_parse_failure;
            }
            if (getenv("DXBC_DEBUG_SNIPPETS")) {
                fprintf(stderr,
                        "    [Preprocess %s] %s: %s\n",
                        error_fields[0] == 0 ? "Info" : "Diagnostic",
                        error_file, error_message);
            }
            free(error_file);
            free(error_message);
        } else {
            /* Unknown tags may carry additional framed payload.  Continuing
             * would make the next payload look like a record and poison every
             * later request on this channel, so fail closed. */
            goto preprocess_parse_failure;
        }

        free(msg);
        msg = NULL;
    }

preprocess_parse_failure:
    free(msg);
    free_preprocess_parse_state(
        snippets, snippet_count, pending_source, &pending_contract);
    unity_compiler_preprocess_response_free(out_response);
    fail_compiler_transaction(channel);
    return false;
}

bool unity_compiler_preprocess_contract(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* input_request,
    PreprocessResult* out_result) {
    if (!out_result) return false;
    memset(out_result, 0, sizeof(*out_result));
    UnityCompilerPreprocessResponse response;
    if (!unity_compiler_preprocess_contract_response(
            channel, input_request, &response)) {
        return false;
    }
    if (!unity_compiler_response_status_is_clean_success(
            &response.status)) {
        unity_compiler_preprocess_response_free(&response);
        return false;
    }
    *out_result = response.result;
    memset(&response.result, 0, sizeof(response.result));
    unity_compiler_preprocess_response_free(&response);
    return true;
}

bool unity_compiler_preprocess(UnityCompilerChannel* channel,
                               const char* source,
                               const char* shader_name,
                               PreprocessResult* out_result) {
    if (!source || !shader_name || !out_result) return false;
    char* file_path = get_shader_file_path(shader_name);
    if (!file_path) {
        memset(out_result, 0, sizeof(*out_result));
        return false;
    }
    UnityCompilerSessionCapabilities capabilities;
    uint32_t valid_apis = 0U;
    if ((!unity_compiler_session_capabilities_snapshot(
             channel, &capabilities) &&
         !unity_compiler_capture_session_capabilities(
             channel, &capabilities)) ||
        !unity_compiler_session_capabilities_valid_apis(
            &capabilities, &valid_apis)) {
        memset(out_result, 0, sizeof(*out_result));
        free(file_path);
        return false;
    }
    UnityCompilerShaderPreprocessRequest request = {
        .source = source,
        .file_path = file_path,
        .shader_name = shader_name,
        .surface_only = false,
        .caching_preprocessor = true,
        /* This compatibility surface predates the complete preprocess
         * contract and remains pinned to the historical standalone-player
         * build target. Exact callers must use preprocess_contract and
         * provide the serialized BuildSettings target explicitly. */
        .build_platform = 19U,
        .valid_apis = valid_apis,
    };
    bool result = unity_compiler_preprocess_contract(
        channel, &request, out_result);
    free(file_path);
    return result;
}

static bool compiler_fingerprint_has_value(const uint8_t *fingerprint);

static void binary_response_request_identity(UnityCompilerBinaryResponse *response,
                                             const uint8_t request_digest[USC_CACHE_DIGEST_SIZE],
                                             const uint8_t controls_digest[USC_CACHE_DIGEST_SIZE]) {
    memcpy(response->request_digest, request_digest, USC_CACHE_DIGEST_SIZE);
    memcpy(response->controls_digest, controls_digest, USC_CACHE_DIGEST_SIZE);
    response->has_request_identity = true;
}

static bool unity_compiler_compile_request_response_internal(
    UnityCompilerChannel* channel,
    const UnityCompilerCompileRequest* input_request,
    UnityCompilerBinaryResponse* out_response
) {
    if (!out_response) return false;
    unity_compiler_binary_response_init(out_response);
    if (!channel || !input_request ||
        !input_request->command ||
        !input_request->toolchain_configuration ||
        !input_request->snippet_source ||
        !input_request->source_directory ||
        !input_request->source_basename || !input_request->pass_name ||
        !valid_owned_string_array(input_request->variant_keywords,
                                  input_request->variant_keyword_count) ||
        !valid_owned_string_array(input_request->user_keywords,
                                  input_request->user_keyword_count) ||
        !valid_owned_string_array(input_request->disabled_keywords,
                                  input_request->disabled_keyword_count)) {
        return false;
    }
    uint8_t compiler_fingerprint[USC_CACHE_DIGEST_SIZE];
    uint8_t environment_fingerprint[USC_CACHE_DIGEST_SIZE];
    if (!get_toolchain_fingerprints(channel, compiler_fingerprint,
                                    environment_fingerprint)) {
        return false;
    }
    UnityCompilerCompileRequest request = *input_request;
    request.environment_fingerprint = environment_fingerprint;
    response_status_capture_valid_apis_authority(
        &out_response->status, channel);

    const char* cache_dir = getenv("DXBC_USC_CACHE_DIR");
    uint8_t request_digest[USC_CACHE_DIGEST_SIZE];
    uint8_t controls_digest[USC_CACHE_DIGEST_SIZE];
    usc_cache_request_digest(&request, compiler_fingerprint, request_digest);
    UnityCompilerCompileRequest controls = request;
    controls.snippet_source = "";
    usc_cache_request_digest(&controls, compiler_fingerprint, controls_digest);
    if (!compiler_fingerprint_has_value(request_digest) ||
        !compiler_fingerprint_has_value(controls_digest))
        return false;
    bool cache_ready = false;
    if (cache_dir && cache_dir[0]) {
        uint8_t* cached_data = NULL;
        size_t cached_size = 0;
        cache_ready = true;
        if (usc_cache_load(cache_dir, request_digest, &cached_data,
                           &cached_size) == USC_CACHE_HIT) {
            bool decoded = usc_cache_deserialize_binary_response(
                cached_data, cached_size, out_response);
            free(cached_data);
            if (!decoded) {
                unity_compiler_binary_response_init(out_response);
            } else {
                binary_response_request_identity(out_response, request_digest, controls_digest);
                const bool toolchain_valid =
                    validate_existing_toolchain_authority(channel);
                response_status_capture_valid_apis_authority(
                    &out_response->status, channel);
                if (expected_valid_apis_authority_is_rejection(
                        &out_response->status.valid_apis_authority)) {
                    out_response->status.from_cache = true;
                    return true;
                }
                if (toolchain_valid) {
                    out_response->status.from_cache = true;
                    return true;
                }
                unity_compiler_binary_response_free(out_response);
                return false;
            }
        }
    }

    binary_response_request_identity(out_response, request_digest, controls_digest);
    if (compiler_cache_only_enabled()) {
        out_response->status.availability =
            UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS;
        response_status_capture_valid_apis_authority(
            &out_response->status, channel);
        return true;
    }

    bool ok = false;
    uint8_t* blob = NULL;
    size_t blob_len = 0;
    char* msg = NULL;

    if (!begin_compiler_transaction(channel)) {
        response_status_capture_valid_apis_authority(
            &out_response->status, channel);
        return expected_valid_apis_authority_is_rejection(
            &out_response->status.valid_apis_authority);
    }
    response_status_capture_valid_apis_authority(
        &out_response->status, channel);
    int fd = channel->socket_fd;
    if (!write_string(fd, request.command) ||
        !write_string(fd, request.snippet_source) ||
        !write_string(fd, request.source_directory) ||
        !write_string(fd, request.source_basename) ||
        !write_string(fd, request.pass_name) ||
        !write_bool(fd, request.caching_preprocessor) ||
        !write_bool(fd, request.preprocess_only) ||
        !write_bool(fd, request.strip_line_directives) ||
        !write_uint32(fd, request.build_platform) ||
        !write_int32(fd, request.render_state_length) ||
        !write_keyword_array(fd, request.variant_keywords,
                             request.variant_keyword_count) ||
        !write_keyword_array(fd, request.user_keywords,
                             request.user_keyword_count) ||
        !write_keyword_array(fd, request.disabled_keywords,
                             request.disabled_keyword_count) ||
        !write_uint32(fd, request.compiler_flags) ||
        !write_int32(fd, request.language) ||
        !write_int32(fd, request.shader_type) ||
        !write_int32(fd, request.platform) ||
        !write_uint64(fd, request.requirements) ||
        !write_int32(fd, request.program_mask) ||
        !write_int32(fd, request.program_start)) {
        goto compile_protocol_failure;
    }

    while (true) {
        msg = read_string(fd);
        if (!msg) goto compile_protocol_failure;
        
        if (strncmp(msg, "shader:", 7) == 0) {
            if (!unity_compiler_parse_compile_status_record(msg, &ok)) {
                goto compile_protocol_failure;
            }
            int32_t intermediate = 0;
            if (!read_int32(fd, &intermediate)) {
                goto compile_protocol_failure;
            }
            void* buf_data = NULL;
            if (!read_buffer(fd, &buf_data, &blob_len)) {
                goto compile_protocol_failure;
            }
            blob = (uint8_t*)buf_data;
            free(msg);
            msg = NULL;
            break;
        }
        
        if (reflection_record_has_known_prefix(msg)) {
            if (!binary_response_append_reflection(out_response, msg)) {
                goto compile_protocol_failure;
            }
            free(msg);
            msg = NULL;
            continue;
        }

        if (strncmp(msg, "err:", 4) == 0) {
            int32_t error_fields[3];
            if (!parse_error_record(msg, error_fields)) {
                goto compile_protocol_failure;
            }
            char* error_file = read_string(fd);
            char* error_message = read_string(fd);
            if (!error_file || !error_message) {
                free(error_file);
                free(error_message);
                goto compile_protocol_failure;
            }
            if (!response_status_append_diagnostic(
                    &out_response->status, error_fields, msg, error_file,
                    error_message)) {
                free(error_file);
                free(error_message);
                goto compile_protocol_failure;
            }
            free(error_file);
            free(error_message);
            free(msg);
            msg = NULL;
            continue;
        }

        /* The 2021.3 callbacks have no arbitrary line-only diagnostic form.
         * An unknown tag may own subsequent framed payload, so accepting it
         * would desynchronize this persistent stream. */
        goto compile_protocol_failure;
    }

    finish_compiler_transaction();
    if (!validate_existing_toolchain_authority(channel)) {
        free(blob);
        unity_compiler_binary_response_free(out_response);
        return false;
    }
    out_response->status.compiler_success = ok;
    out_response->data = blob;
    out_response->size = blob_len;

    if (cache_ready && ok) {
        uint8_t* serialized = NULL;
        size_t serialized_size = 0U;
        if (usc_cache_serialize_binary_response(
                out_response, &serialized, &serialized_size)) {
            (void)usc_cache_store(cache_dir, request_digest, serialized,
                                  serialized_size);
        }
        free(serialized);
    }

    return true;

compile_protocol_failure:
    free(msg);
    free(blob);
    unity_compiler_binary_response_free(out_response);
    fail_compiler_transaction(channel);
    return false;
}

static uint8_t* unity_compiler_compile_request_internal(
    UnityCompilerChannel* channel,
    const UnityCompilerCompileRequest* input_request,
    size_t* out_size,
    char** out_error) {
    if (out_size) *out_size = 0U;
    if (out_error) *out_error = NULL;
    if (!out_size) return NULL;
    UnityCompilerBinaryResponse response;
    if (!unity_compiler_compile_request_response_internal(
            channel, input_request, &response)) {
        if (out_error) {
            *out_error = strdup(
                "Unity compiler transport or protocol failure");
        }
        return NULL;
    }
    return unity_compiler_binary_response_take_clean_data(
        &response, out_size, out_error);
}

uint8_t* unity_compiler_binary_response_take_clean_data(
    UnityCompilerBinaryResponse* response, size_t* out_size, char** out_error) {
    if (out_size) *out_size = 0U;
    if (out_error) *out_error = NULL;
    if (!response) return NULL;
    if (!out_size) {
        unity_compiler_binary_response_free(response);
        return NULL;
    }
    if (!unity_compiler_response_status_is_clean_success(
            &response->status)) {
        if (out_error) {
            *out_error = unity_compiler_response_status_format(
                &response->status,
                response->status.compiler_success
                    ? "Unity compiler returned diagnostics"
                    : "Unknown compiler error");
        }
        unity_compiler_binary_response_free(response);
        return NULL;
    }
    if (response->size != 0U && !response->data) {
        if (out_error) {
            *out_error = strdup("Unity compiler response has no payload");
        }
        unity_compiler_binary_response_free(response);
        return NULL;
    }
    uint8_t* data = response->data;
    size_t size = response->size;
    response->data = NULL;
    response->size = 0U;
    unity_compiler_binary_response_free(response);
    if (!data) {
        data = (uint8_t*)malloc(1U);
        if (!data) return NULL;
    }
    *out_size = size;
    return data;
}

static bool contract_request_to_wire(
    const UnityCompilerSnippetCompileRequest* request,
    const uint8_t* environment_fingerprint,
    UnityCompilerCompileRequest* out_request) {
    if (!request || !out_request || !request->contract ||
        !unity_compiler_snippet_contract_validate(request->contract) ||
        !valid_owned_string_array(request->variant_keywords,
                                  request->variant_keyword_count) ||
        !valid_owned_string_array(request->user_keywords,
                                  request->user_keyword_count) ||
        !valid_owned_string_array(request->disabled_keywords,
                                  request->disabled_keyword_count)) {
        return false;
    }
    UnityCompilerCompileRequest wire_request = {
        .command = "compileSnippet",
        .toolchain_configuration = UNITY_TOOLCHAIN_CONFIGURATION,
        .snippet_source = request->snippet_source,
        .source_directory = request->source_directory,
        .source_basename = request->source_basename,
        .pass_name = request->pass_name,
        .caching_preprocessor = request->caching_preprocessor,
        .preprocess_only = request->preprocess_only,
        .strip_line_directives = request->strip_line_directives,
        .build_platform = request->build_platform,
        .render_state_length = request->render_state_length,
        .variant_keywords = request->variant_keywords,
        .variant_keyword_count = request->variant_keyword_count,
        .user_keywords = request->user_keywords,
        .user_keyword_count = request->user_keyword_count,
        .disabled_keywords = request->disabled_keywords,
        .disabled_keyword_count = request->disabled_keyword_count,
        .compiler_flags = request->compiler_flags,
        .language = request->contract->language,
        .shader_type = request->shader_type,
        .platform = request->platform,
        .requirements = request->requirements,
        .program_mask = request->program_mask,
        .program_start = request->program_start,
        .snippet_contract = request->contract,
        .environment_fingerprint = environment_fingerprint,
    };
    *out_request = wire_request;
    return true;
}

static bool compiler_fingerprint_has_value(const uint8_t* fingerprint) {
    if (!fingerprint) return false;
    uint8_t combined = 0U;
    for (size_t i = 0; i < UNITY_COMPILER_FINGERPRINT_SIZE; ++i) {
        combined |= fingerprint[i];
    }
    return combined != 0U;
}

static bool offline_authority_is_valid(
    const UnityCompilerOfflineAuthority* authority) {
    return authority &&
           compiler_fingerprint_has_value(
               authority->compiler_fingerprint) &&
           compiler_fingerprint_has_value(
               authority->environment_fingerprint);
}

static bool serialize_preprocess_request_authority(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* request,
    const uint8_t compiler_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE],
    const uint8_t environment_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE],
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!out_transcript || !out_transcript_size || !out_request_digest) {
        return false;
    }
    *out_transcript = NULL;
    *out_transcript_size = 0U;
    memset(out_request_digest, 0, UNITY_COMPILER_FINGERPRINT_SIZE);
    if (!compiler_fingerprint_has_value(compiler_fingerprint) ||
        !compiler_fingerprint_has_value(environment_fingerprint)) {
        return false;
    }
    char* include_paths[3];
    UnityCompilerPreprocessRequest wire_request;
    if (!preprocess_request_to_wire(
            channel, request, environment_fingerprint,
            include_paths, &wire_request) ||
        !usc_cache_serialize_preprocess_request(
            &wire_request, compiler_fingerprint,
            out_transcript, out_transcript_size)) {
        return false;
    }
    usc_cache_preprocess_request_digest(
        &wire_request, compiler_fingerprint, out_request_digest);
    return true;
}

static bool serialize_compile_request_authority(
    const UnityCompilerSnippetCompileRequest* request,
    const uint8_t compiler_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE],
    const uint8_t environment_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE],
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!out_transcript || !out_transcript_size || !out_request_digest) {
        return false;
    }
    *out_transcript = NULL;
    *out_transcript_size = 0U;
    memset(out_request_digest, 0, UNITY_COMPILER_FINGERPRINT_SIZE);
    if (!compiler_fingerprint_has_value(compiler_fingerprint) ||
        !compiler_fingerprint_has_value(environment_fingerprint)) {
        return false;
    }
    UnityCompilerCompileRequest wire_request;
    if (!contract_request_to_wire(
            request, environment_fingerprint, &wire_request) ||
        !usc_cache_serialize_compile_request(
            &wire_request, compiler_fingerprint,
            out_transcript, out_transcript_size)) {
        return false;
    }
    usc_cache_request_digest(
        &wire_request, compiler_fingerprint, out_request_digest);
    return true;
}

bool unity_compiler_get_toolchain_provenance(
    UnityCompilerChannel* channel,
    UnityCompilerToolchainProvenance* out_provenance) {
    if (!channel || !channel->configured || !out_provenance) return false;
    UnityCompilerToolchainProvenance provenance;
    memset(&provenance, 0, sizeof(provenance));
    if (!get_toolchain_fingerprints(
            channel, provenance.compiler_fingerprint,
            provenance.environment_fingerprint)) {
        return false;
    }
    provenance.unity_contents_path = channel->unity_contents_path;
    provenance.compiler_path = channel->compiler_path;
    provenance.builtin_includes_dir = channel->builtin_includes_dir;
    provenance.playback_engines_dir = channel->playback_engines_dir;
    provenance.glslang_path = channel->glslang_path;
    provenance.dxcompiler_path = channel->dxcompiler_path;
    *out_provenance = provenance;
    return true;
}

bool unity_compiler_serialize_preprocess_request(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!out_transcript || !out_transcript_size || !out_request_digest) {
        return false;
    }
    *out_transcript = NULL;
    *out_transcript_size = 0U;
    memset(out_request_digest, 0, UNITY_COMPILER_FINGERPRINT_SIZE);
    UnityCompilerToolchainProvenance provenance;
    if (!unity_compiler_get_toolchain_provenance(channel, &provenance)) {
        return false;
    }
    if (!serialize_preprocess_request_authority(
            channel, request, provenance.compiler_fingerprint,
            provenance.environment_fingerprint, out_transcript,
            out_transcript_size, out_request_digest)) {
        return false;
    }
    if (!validate_existing_toolchain_authority(channel)) {
        free(*out_transcript);
        *out_transcript = NULL;
        *out_transcript_size = 0U;
        memset(out_request_digest, 0, UNITY_COMPILER_FINGERPRINT_SIZE);
        return false;
    }
    return true;
}

bool unity_compiler_serialize_preprocess_request_with_authority(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!offline_authority_is_valid(authority)) {
        if (out_transcript) *out_transcript = NULL;
        if (out_transcript_size) *out_transcript_size = 0U;
        if (out_request_digest) {
            memset(out_request_digest, 0,
                   UNITY_COMPILER_FINGERPRINT_SIZE);
        }
        return false;
    }
    return serialize_preprocess_request_authority(
        channel, request, authority->compiler_fingerprint,
        authority->environment_fingerprint, out_transcript,
        out_transcript_size, out_request_digest);
}

bool unity_compiler_serialize_preprocess_result(
    const PreprocessResult* result, uint8_t** out_data, size_t* out_size) {
    return usc_cache_serialize_preprocess_result(result, out_data, out_size);
}

bool unity_compiler_deserialize_preprocess_result(
    const uint8_t* data, size_t size, PreprocessResult* out_result) {
    return usc_cache_deserialize_preprocess_result(data, size, out_result);
}

bool unity_compiler_serialize_compile_request(
    UnityCompilerChannel* channel,
    const UnityCompilerSnippetCompileRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!out_transcript || !out_transcript_size || !out_request_digest) {
        return false;
    }
    *out_transcript = NULL;
    *out_transcript_size = 0;
    memset(out_request_digest, 0, UNITY_COMPILER_FINGERPRINT_SIZE);
    UnityCompilerToolchainProvenance provenance;
    if (!unity_compiler_get_toolchain_provenance(channel, &provenance)) {
        return false;
    }
    if (!serialize_compile_request_authority(
            request, provenance.compiler_fingerprint,
            provenance.environment_fingerprint, out_transcript,
            out_transcript_size, out_request_digest)) {
        return false;
    }
    if (!validate_existing_toolchain_authority(channel)) {
        free(*out_transcript);
        *out_transcript = NULL;
        *out_transcript_size = 0U;
        memset(out_request_digest, 0, UNITY_COMPILER_FINGERPRINT_SIZE);
        return false;
    }
    return true;
}

bool unity_compiler_serialize_compile_request_with_authority(
    const UnityCompilerSnippetCompileRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]) {
    if (!offline_authority_is_valid(authority)) {
        if (out_transcript) *out_transcript = NULL;
        if (out_transcript_size) *out_transcript_size = 0U;
        if (out_request_digest) {
            memset(out_request_digest, 0,
                   UNITY_COMPILER_FINGERPRINT_SIZE);
        }
        return false;
    }
    return serialize_compile_request_authority(
        request, authority->compiler_fingerprint,
        authority->environment_fingerprint, out_transcript,
        out_transcript_size, out_request_digest);
}

uint8_t* unity_compiler_compile_contract(
    UnityCompilerChannel* channel,
    const UnityCompilerSnippetCompileRequest* request,
    size_t* out_size,
    char** out_error) {
    if (out_size) *out_size = 0;
    if (out_error) *out_error = NULL;
    UnityCompilerCompileRequest wire_request;
    if (!contract_request_to_wire(request, NULL, &wire_request)) return NULL;
    return unity_compiler_compile_request_internal(
        channel, &wire_request, out_size, out_error);
}

bool unity_compiler_compile_contract_response(
    UnityCompilerChannel* channel,
    const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* out_response) {
    if (!out_response) return false;
    unity_compiler_binary_response_init(out_response);
    UnityCompilerCompileRequest wire_request;
    if (!contract_request_to_wire(request, NULL, &wire_request)) return false;
    return unity_compiler_compile_request_response_internal(
        channel, &wire_request, out_response);
}

bool unity_compiler_compile_response(
    UnityCompilerChannel* channel, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count,
    UnityCompilerBinaryResponse* out_response) {
    if (!out_response) return false;
    unity_compiler_binary_response_init(out_response);
    if (!shader_name) return false;
    char* legacy_source_directory = get_shader_file_path(shader_name);
    if (!legacy_source_directory) return false;
    UnityCompilerCompileRequest request = {
        .command = "compileSnippet",
        .toolchain_configuration = UNITY_TOOLCHAIN_CONFIGURATION,
        .snippet_source = snippet_src,
        /* Preserve the historical wire request in the compatibility API. */
        .source_directory = legacy_source_directory,
        .source_basename = shader_name,
        .pass_name = "",
        .caching_preprocessor = true,
        .preprocess_only = false,
        .strip_line_directives = false,
        .build_platform = platform == 4 ? 1U : 2U,
        .render_state_length = 0,
        .variant_keywords = keywords,
        .variant_keyword_count = keyword_count,
        .user_keywords = NULL,
        .user_keyword_count = 0,
        .disabled_keywords = defines,
        .disabled_keyword_count = define_count,
        .compiler_flags = platform == 4 ? 0x9000U : 0U,
        .language = 0,
        .shader_type = shader_type,
        .platform = platform,
        .requirements = reqs,
        .program_mask = 6,
        .program_start = 0,
        .snippet_contract = NULL,
    };
    bool result = unity_compiler_compile_request_response_internal(
        channel, &request, out_response);
    free(legacy_source_directory);
    return result;
}

uint8_t* unity_compiler_compile(
    UnityCompilerChannel* channel, const char* snippet_src,
    const char* shader_name, int shader_type, int platform, uint64_t reqs,
    char** keywords, int keyword_count, char** defines, int define_count,
    size_t* out_size, char** out_error) {
    if (out_size) *out_size = 0U;
    if (out_error) *out_error = NULL;
    if (!out_size) return NULL;
    UnityCompilerBinaryResponse response;
    if (!unity_compiler_compile_response(
            channel, snippet_src, shader_name, shader_type, platform, reqs,
            keywords, keyword_count, defines, define_count, &response)) {
        if (out_error) {
            *out_error = strdup("Unity compiler transport or protocol failure");
        }
        unity_compiler_binary_response_free(&response);
        return NULL;
    }
    return unity_compiler_binary_response_take_clean_data(
        &response, out_size, out_error);
}

uint64_t unity_compiler_variant_requirements(
    const PreprocessedSnippet* snippet, char** keywords, int keyword_count) {
    if (!snippet) return 0;
    const ConditionalShaderRequirement* conditionals =
        snippet->has_contract
            ? snippet->contract.conditional_requirements
            : snippet->conditional_requirements;
    int conditional_count = snippet->has_contract
        ? snippet->contract.conditional_requirement_count
        : snippet->conditional_requirement_count;
    uint64_t requirements = snippet->has_contract
        ? snippet->contract.requirements
        : snippet->reqs;
    for (int i = 0; i < conditional_count; i++) {
        const ConditionalShaderRequirement* conditional =
            &conditionals[i];
        for (int keyword_index = 0; keyword_index < keyword_count;
             keyword_index++) {
            if (strcmp(conditional->keyword, keywords[keyword_index]) == 0) {
                requirements |= conditional->requirements;
                break;
            }
        }
    }
    return requirements;
}

char* unity_compiler_preprocess_expanded(
    UnityCompilerChannel* channel,
    const char* snippet_src,
    const char* shader_name,
    int shader_type,
    int platform,
    uint64_t reqs,
    char** keywords,
    int keyword_count,
    char** defines,
    int define_count
) {
    if (!channel || !snippet_src || !shader_name) return NULL;
    char* file_path = get_shader_file_path(shader_name);
    if (!file_path) return NULL;
    UnityCompilerCompileRequest request = {
        .command = "compileSnippet",
        .toolchain_configuration = UNITY_TOOLCHAIN_CONFIGURATION,
        .snippet_source = snippet_src,
        .source_directory = file_path,
        .source_basename = shader_name,
        .pass_name = "",
        .caching_preprocessor = true,
        .preprocess_only = true,
        .strip_line_directives = false,
        .build_platform = platform == 4 ? 1U : 2U,
        .render_state_length = 0,
        .variant_keywords = keywords,
        .variant_keyword_count = keyword_count,
        .user_keywords = NULL,
        .user_keyword_count = 0,
        .disabled_keywords = defines,
        .disabled_keyword_count = define_count,
        .compiler_flags = platform == 4 ? 0x9000U : 0U,
        .language = 0,
        .shader_type = shader_type,
        .platform = platform,
        .requirements = reqs,
        .program_mask = 6,
        .program_start = 0,
        .snippet_contract = NULL,
    };
    size_t expanded_size = 0;
    char* error = NULL;
    uint8_t* expanded = unity_compiler_compile_request_internal(
        channel, &request, &expanded_size, &error);
    free(file_path);
    free(error);
    if (!expanded || expanded_size == SIZE_MAX) {
        free(expanded);
        return NULL;
    }
    char* text = (char*)malloc(expanded_size + 1U);
    if (!text) {
        free(expanded);
        return NULL;
    }
    if (expanded_size > 0U) memcpy(text, expanded, expanded_size);
    text[expanded_size] = '\0';
    free(expanded);
    return text;
}

bool unity_compiler_disassemble_response(
    UnityCompilerChannel* channel,
    const char* shader_name,
    int platform,    // 4 = d3d11, 15 = glcore
    int stage,       // 0 = vertex, 1 = fragment
    const uint8_t* bytecode,
    size_t size,
    UnityCompilerTextResponse* out_response
) {
    if (!out_response) return false;
    unity_compiler_text_response_init(out_response);
    if (!channel || !shader_name || (size > 0U && !bytecode)) {
        return false;
    }
    response_status_capture_valid_apis_authority(
        &out_response->status, channel);
    /* Disassembly has no persistent-cache record today.  Cache-only is a
     * process-free contract for every compiler operation, including optional
     * mismatch diagnostics: report typed unavailability before lazy channel
     * initialization can launch UnityShaderCompiler. */
    if (compiler_cache_only_enabled()) {
        out_response->status.availability =
            UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS;
        return true;
    }
    if (!begin_compiler_transaction(channel)) {
        response_status_capture_valid_apis_authority(
            &out_response->status, channel);
        return expected_valid_apis_authority_is_rejection(
            &out_response->status.valid_apis_authority);
    }
    response_status_capture_valid_apis_authority(
        &out_response->status, channel);
    int fd = channel->socket_fd;
    if (!write_string(fd, "disassembleShader") ||
        !write_string(fd, shader_name) || !write_int32(fd, platform) ||
        !write_int32(fd, stage) || !write_int32(fd, stage) ||
        !write_buffer(fd, bytecode, size)) {
        unity_compiler_text_response_free(out_response);
        fail_compiler_transaction(channel);
        return false;
    }

    bool ok = false;
    for (;;) {
        char* status = read_string(fd);
        if (!status) {
            unity_compiler_text_response_free(out_response);
            fail_compiler_transaction(channel);
            return false;
        }
        if (strncmp(status, "err:", 4) == 0) {
            int32_t error_fields[3];
            if (!parse_error_record(status, error_fields)) {
                free(status);
                unity_compiler_text_response_free(out_response);
                fail_compiler_transaction(channel);
                return false;
            }
            char* error_file = read_string(fd);
            char* error_message = read_string(fd);
            if (!error_file || !error_message) {
                free(status);
                free(error_file);
                free(error_message);
                unity_compiler_text_response_free(out_response);
                fail_compiler_transaction(channel);
                return false;
            }
            if (!response_status_append_diagnostic(
                    &out_response->status, error_fields, status, error_file,
                    error_message)) {
                free(status);
                free(error_file);
                free(error_message);
                unity_compiler_text_response_free(out_response);
                fail_compiler_transaction(channel);
                return false;
            }
            free(status);
            free(error_file);
            free(error_message);
            continue;
        }
        bool parsed = unity_compiler_parse_disassemble_status_record(
            status, &ok);
        free(status);
        if (!parsed) {
            unity_compiler_text_response_free(out_response);
            fail_compiler_transaction(channel);
            return false;
        }
        out_response->status.compiler_success = ok;
        break;
    }
    size_t text_size = 0U;
    char* glsl = read_string_with_size(fd, &text_size);
    if (!glsl) {
        unity_compiler_text_response_free(out_response);
        fail_compiler_transaction(channel);
        return false;
    }
    finish_compiler_transaction();
    if (!validate_existing_toolchain_authority(channel)) {
        free(glsl);
        unity_compiler_text_response_free(out_response);
        return false;
    }
    out_response->text = glsl;
    out_response->size = text_size;
    return true;
}

char* unity_compiler_disassemble(
    UnityCompilerChannel* channel,
    const char* shader_name,
    int platform,
    int stage,
    const uint8_t* bytecode,
    size_t size) {
    UnityCompilerTextResponse response;
    if (!unity_compiler_disassemble_response(
            channel, shader_name, platform, stage, bytecode, size,
            &response)) {
        return NULL;
    }
    if (!unity_compiler_response_status_is_clean_success(
            &response.status)) {
        unity_compiler_text_response_free(&response);
        return NULL;
    }
    char* text = response.text;
    response.text = NULL;
    response.size = 0U;
    unity_compiler_text_response_free(&response);
    return text;
}

void unity_compiler_free_preprocess(PreprocessResult* result) {
    if (!result) return;
    if (result->snippets) {
        for (int i = 0; i < result->snippet_count; i++) {
            free(result->snippets[i].source);
            if (result->snippets[i].has_contract) {
                unity_compiler_snippet_contract_free(
                    &result->snippets[i].contract);
            } else {
                for (int j = 0;
                     j < result->snippets[i].conditional_requirement_count;
                     j++) {
                    free(result->snippets[i]
                             .conditional_requirements[j]
                             .keyword);
                }
                free(result->snippets[i].conditional_requirements);
            }
        }
        free(result->snippets);
    }
    if (result->blob) {
        free(result->blob);
    }
    memset(result, 0, sizeof(PreprocessResult));
}

static void unity_compiler_stop_process(UnityCompilerChannel* channel) {
    if (!channel) return;

    pid_t pid = channel->process_id;
    channel->process_id = 0;
    if (channel->socket_fd >= 0) {
        /* DispatchCommand returns false for this command, which performs the
         * compiler's orderly queue/plugin teardown before process exit. */
        if (io_deadline_begin(UNITY_COMPILER_SHUTDOWN_GRACE_MS)) {
            (void)write_string(channel->socket_fd, "shutdown");
            io_deadline_end();
        }
        shutdown(channel->socket_fd, SHUT_WR);
        close(channel->socket_fd);
        channel->socket_fd = -1;
    }
    if (!reap_process_bounded(pid, UNITY_COMPILER_SHUTDOWN_GRACE_MS)) {
        terminate_and_reap_process(pid);
    }
    unregister_compiler_process(pid);
}

bool unity_compiler_recycle_process(UnityCompilerChannel* channel) {
    if (!channel || !channel->configured) return false;
    unity_compiler_stop_process(channel);
    return true;
}

void unity_compiler_shutdown(UnityCompilerChannel* channel) {
    if (!channel) return;
    unity_compiler_stop_process(channel);
    usc_cache_toolchain_lease_destroy(channel->cache_toolchain_lease);
    channel->cache_toolchain_lease = NULL;
    free(channel->project_root);
    free(channel->includes_dir);
    free(channel->sandbox_includes_dir);
    free(channel->unity_contents_path);
    free(channel->compiler_path);
    free(channel->builtin_includes_dir);
    free(channel->playback_engines_dir);
    free(channel->frameworks_dir);
    free(channel->tools_dir);
    free(channel->glslang_path);
    free(channel->dxcompiler_path);
    free(channel->dynamic_library_path);
    memset(channel, 0, sizeof(*channel));
    channel->socket_fd = -1;
}
