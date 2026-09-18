// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compile_authority.h"
#include "common/shader_stage.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char* const k_platform_capability_keywords[] = {
    "UNITY_NO_DXT5nm",
    "UNITY_NO_RGBM",
    "UNITY_USE_NATIVE_HDR",
    "UNITY_ENABLE_REFLECTION_BUFFERS",
    "UNITY_FRAMEBUFFER_FETCH_AVAILABLE",
    "UNITY_ENABLE_NATIVE_SHADOW_LOOKUPS",
    "UNITY_METAL_SHADOWS_USE_POINT_FILTERING",
    "UNITY_NO_CUBEMAP_ARRAY",
    "UNITY_NO_SCREENSPACE_SHADOWS",
    "UNITY_USE_DITHER_MASK_FOR_ALPHABLENDED_SHADOWS",
    "UNITY_PBS_USE_BRDF1",
    "UNITY_PBS_USE_BRDF2",
    "UNITY_PBS_USE_BRDF3",
    "UNITY_NO_FULL_STANDARD_SHADER",
    "UNITY_SPECCUBE_BOX_PROJECTION",
    "UNITY_SPECCUBE_BLENDING",
    "UNITY_ENABLE_DETAIL_NORMALMAP",
    "SHADER_API_MOBILE",
    "SHADER_API_DESKTOP",
    "UNITY_HARDWARE_TIER1",
    "UNITY_HARDWARE_TIER2",
    "UNITY_HARDWARE_TIER3",
    "UNITY_COLORSPACE_GAMMA",
    "UNITY_LIGHT_PROBE_PROXY_VOLUME",
    "UNITY_HALF_PRECISION_FRAGMENT_SHADER_REGISTERS",
    "UNITY_LIGHTMAP_DLDR_ENCODING",
    "UNITY_LIGHTMAP_RGBM_ENCODING",
    "UNITY_LIGHTMAP_FULL_HDR",
    "UNITY_VIRTUAL_TEXTURING",
    "UNITY_PRETRANSFORM_TO_DISPLAY_ORIENTATION",
    "UNITY_ASTC_NORMALMAP_ENCODING",
    "SHADER_API_GLES30",
    "UNITY_UNIFIED_SHADER_PRECISION_MODEL",
};

_Static_assert(
    sizeof(k_platform_capability_keywords) /
            sizeof(k_platform_capability_keywords[0]) ==
        UNITY_PLATFORM_CAPABILITY_COUNT,
    "Unity platform capability table changed");

/* Unity's IsKeywordSettingsDependent mask for the table above. */
static const uint64_t k_settings_dependent_mask = UINT64_C(0x1fff9ff79);

typedef struct {
    const char** slots;
    size_t capacity;
    size_t count;
} StringSet;

typedef struct {
    char** values;
    size_t count;
    size_t capacity;
    StringSet set;
} OrderedOwnedStringSet;

typedef enum {
    STRING_SET_ERROR = -1,
    STRING_SET_EXISTS = 0,
    STRING_SET_INSERTED = 1,
} StringSetInsertResult;

static uint64_t hash_span(const char* value, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)value[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool string_matches_span(
    const char* string, const char* value, size_t length) {
    return string && strlen(string) == length &&
           memcmp(string, value, length) == 0;
}

static bool string_set_rehash(StringSet* set, size_t requested_capacity) {
    size_t capacity = 16U;
    while (capacity < requested_capacity) {
        if (capacity > SIZE_MAX / 2U) return false;
        capacity *= 2U;
    }
    const char** slots = (const char**)calloc(capacity, sizeof(*slots));
    if (!slots) return false;
    for (size_t i = 0; i < set->capacity; i++) {
        const char* value = set->slots[i];
        if (!value) continue;
        size_t slot = (size_t)hash_span(value, strlen(value)) &
                      (capacity - 1U);
        while (slots[slot]) slot = (slot + 1U) & (capacity - 1U);
        slots[slot] = value;
    }
    free(set->slots);
    set->slots = slots;
    set->capacity = capacity;
    return true;
}

static StringSetInsertResult string_set_insert_span(
    StringSet* set, const char* value, size_t length,
    const char* stored_value) {
    if (!set || !value || !stored_value) return STRING_SET_ERROR;
    if (set->capacity == 0 ||
        set->count + 1U > (set->capacity * 7U) / 10U) {
        size_t requested = set->capacity ? set->capacity * 2U : 16U;
        if (requested < set->capacity ||
            !string_set_rehash(set, requested)) {
            return STRING_SET_ERROR;
        }
    }
    size_t slot = (size_t)hash_span(value, length) & (set->capacity - 1U);
    while (set->slots[slot]) {
        if (string_matches_span(set->slots[slot], value, length)) {
            return STRING_SET_EXISTS;
        }
        slot = (slot + 1U) & (set->capacity - 1U);
    }
    set->slots[slot] = stored_value;
    set->count++;
    return STRING_SET_INSERTED;
}

static bool string_set_contains_span(
    const StringSet* set, const char* value, size_t length) {
    if (!set || set->capacity == 0 || !value) return false;
    size_t slot = (size_t)hash_span(value, length) & (set->capacity - 1U);
    while (set->slots[slot]) {
        if (string_matches_span(set->slots[slot], value, length)) {
            return true;
        }
        slot = (slot + 1U) & (set->capacity - 1U);
    }
    return false;
}

static void string_set_free(StringSet* set) {
    if (!set) return;
    free(set->slots);
    memset(set, 0, sizeof(*set));
}

static void free_string_array(char** values, size_t count) {
    if (!values) return;
    for (size_t i = 0; i < count; i++) free(values[i]);
    free(values);
}

static bool append_owned_span(
    OrderedOwnedStringSet* ordered, const char* value, size_t length) {
    if (string_set_contains_span(&ordered->set, value, length)) return true;
    if (ordered->count == (size_t)INT_MAX) return false;
    if (ordered->count == ordered->capacity) {
        size_t capacity = ordered->capacity ? ordered->capacity * 2U : 16U;
        if (capacity < ordered->capacity ||
            capacity > SIZE_MAX / sizeof(*ordered->values)) {
            return false;
        }
        char** values = (char**)realloc(
            ordered->values, capacity * sizeof(*values));
        if (!values) return false;
        ordered->values = values;
        ordered->capacity = capacity;
    }
    char* copy = (char*)malloc(length + 1U);
    if (!copy) return false;
    memcpy(copy, value, length);
    copy[length] = '\0';
    StringSetInsertResult inserted = string_set_insert_span(
        &ordered->set, copy, length, copy);
    if (inserted != STRING_SET_INSERTED) {
        free(copy);
        return inserted == STRING_SET_EXISTS;
    }
    ordered->values[ordered->count++] = copy;
    return true;
}

static bool append_owned_string(
    OrderedOwnedStringSet* ordered, const char* value) {
    return value && append_owned_span(ordered, value, strlen(value));
}

static void ordered_owned_string_set_free(OrderedOwnedStringSet* ordered) {
    if (!ordered) return;
    free_string_array(ordered->values, ordered->count);
    string_set_free(&ordered->set);
    memset(ordered, 0, sizeof(*ordered));
}

void unity_compile_authority_init(UnityCompileAuthority* authority) {
    if (authority) memset(authority, 0, sizeof(*authority));
}

void unity_compile_authority_free(UnityCompileAuthority* authority) {
    if (!authority) return;
    free_string_array(authority->platform_keywords,
                      (size_t)authority->platform_keyword_count);
    free_string_array(authority->user_keywords,
                      (size_t)authority->user_keyword_count);
    free_string_array(authority->disabled_keywords,
                      (size_t)authority->disabled_keyword_count);
    memset(authority, 0, sizeof(*authority));
}

const char* unity_platform_capability_keyword(size_t capability_index) {
    return capability_index < UNITY_PLATFORM_CAPABILITY_COUNT
               ? k_platform_capability_keywords[capability_index]
               : NULL;
}

bool unity_platform_capability_is_settings_dependent(
    size_t capability_index) {
    return capability_index < UNITY_PLATFORM_CAPABILITY_COUNT &&
           (k_settings_dependent_mask & (UINT64_C(1) << capability_index)) !=
               0;
}

const char* unity_pass_type_keyword(int pass_type) {
    switch (pass_type) {
        case 4: return "UNITY_PASS_FORWARDBASE";
        case 5: return "UNITY_PASS_FORWARDADD";
        case 6: return "UNITY_PASS_PREPASSBASE";
        case 7: return "UNITY_PASS_PREPASSFINAL";
        case 8: return "UNITY_PASS_SHADOWCASTER";
        case 10: return "UNITY_PASS_DEFERRED";
        case 11: return "UNITY_PASS_META";
        case 12: return "UNITY_PASS_MOTIONVECTORS";
        case 14: return "UNITY_PASS_SRPDEFAULTUNLIT";
        default: return NULL;
    }
}

bool unity_serialized_stage_to_compiler_program(
    int serialized_stage, int32_t* out_compiler_program) {
    if (!out_compiler_program || serialized_stage < 0 ||
        serialized_stage >= UNITY_SERIALIZED_STAGE_COUNT) {
        return false;
    }
    UnityCompilerProgramStage compiler_program;
    if (!shader_stage_serialized_to_compiler(
            (UnitySerializedProgramStage)serialized_stage,
            &compiler_program)) {
        return false;
    }
    *out_compiler_program = (int32_t)compiler_program;
    return true;
}

static UnityCompileAuthorityStatus select_keyword_indices(
    const UnityCompileAuthorityInput* input, bool* selected) {
    const int* arrays[] = {
        input->global_keyword_indices,
        input->local_keyword_indices,
    };
    const size_t counts[] = {
        input->global_keyword_index_count,
        input->local_keyword_index_count,
    };
    for (size_t scope = 0; scope < 2U; scope++) {
        if (counts[scope] > 0 && !arrays[scope]) {
            return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < counts[scope]; i++) {
            int index = arrays[scope][i];
            if (index < 0 || (size_t)index >= input->keyword_name_count ||
                !input->keyword_names[index] ||
                input->keyword_names[index][0] == '\0') {
                return UNITY_COMPILE_AUTHORITY_INVALID_KEYWORD_INDEX;
            }
            selected[index] = true;
        }
    }
    return UNITY_COMPILE_AUTHORITY_OK;
}

static UnityCompileAuthorityStatus build_user_keywords(
    const UnityCompileAuthorityInput* input, const bool* selected,
    UnityCompileAuthority* authority, StringSet* active_names) {
    size_t count = 0;
    for (size_t i = 0; i < input->keyword_name_count; i++) {
        if (selected[i]) count++;
    }
    if (count > (size_t)INT_MAX ||
        (count > 0 && count > SIZE_MAX / sizeof(char*))) {
        return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    }
    if (count > 0) {
        authority->user_keywords = (char**)calloc(
            count, sizeof(*authority->user_keywords));
        if (!authority->user_keywords) {
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
    }
    authority->user_keyword_count = (int)count;
    size_t output = 0;
    for (size_t i = 0; i < input->keyword_name_count; i++) {
        if (!selected[i]) continue;
        const char* name = input->keyword_names[i];
        StringSetInsertResult inserted = string_set_insert_span(
            active_names, name, strlen(name), name);
        if (inserted == STRING_SET_ERROR) {
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
        if (inserted == STRING_SET_EXISTS) {
            return UNITY_COMPILE_AUTHORITY_AMBIGUOUS_KEYWORD_NAME;
        }
        authority->user_keywords[output] = strdup(name);
        if (!authority->user_keywords[output]) {
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
        output++;
    }
    return UNITY_COMPILE_AUTHORITY_OK;
}

static UnityCompileAuthorityStatus build_platform_keywords(
    const UnityCompileAuthorityInput* input,
    UnityCompileAuthority* authority) {
    uint64_t bits = input->platform_capabilities.bits;
    size_t count = 0;
    for (size_t i = 0; i < UNITY_PLATFORM_CAPABILITY_COUNT; i++) {
        if (bits & (UINT64_C(1) << i)) count++;
    }
    const char* pass_keyword = unity_pass_type_keyword(input->pass_type);
    if (pass_keyword) count++;
    if (count > 0) {
        authority->platform_keywords = (char**)calloc(
            count, sizeof(*authority->platform_keywords));
        if (!authority->platform_keywords) {
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
    }
    authority->platform_keyword_count = (int)count;
    size_t output = 0;
    for (size_t i = 0; i < UNITY_PLATFORM_CAPABILITY_COUNT; i++) {
        if (!(bits & (UINT64_C(1) << i))) continue;
        authority->platform_keywords[output] =
            strdup(k_platform_capability_keywords[i]);
        if (!authority->platform_keywords[output]) {
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
        output++;
    }
    if (pass_keyword) {
        authority->platform_keywords[output] = strdup(pass_keyword);
        if (!authority->platform_keywords[output]) {
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
    }

    if (bits & (UINT64_C(1) << 17U)) {
        authority->compiler_flags |= UINT32_C(1) << 28U;
    }
    if (bits & (UINT64_C(1) << 32U)) {
        authority->compiler_flags |= UINT32_C(1) << 31U;
    }
    if (bits & (UINT64_C(1) << 6U)) {
        authority->compiler_flags |= UINT32_C(1) << 14U;
    }
    return UNITY_COMPILE_AUTHORITY_OK;
}

static UnityCompileAuthorityStatus add_disabled_combination(
    const char* combination, const StringSet* active_names,
    OrderedOwnedStringSet* disabled) {
    if (!combination) return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    const char* cursor = combination;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        const char* begin = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
        size_t length = (size_t)(cursor - begin);
        if (length == 0 || (length == 1U && begin[0] == '_') ||
            string_set_contains_span(active_names, begin, length)) {
            continue;
        }
        if (!append_owned_span(disabled, begin, length)) {
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
    }
    return UNITY_COMPILE_AUTHORITY_OK;
}

static UnityCompileAuthorityStatus build_disabled_keywords(
    const UnityCompileAuthorityInput* input, const StringSet* active_names,
    UnityCompileAuthority* authority) {
    const SnippetProgramKeywordVariants* program =
        unity_compiler_snippet_contract_find_program_variants(
            input->contract, input->compiler_program);
    if (!program) {
        return UNITY_COMPILE_AUTHORITY_MISSING_VARIANT_FAMILIES;
    }
    const SnippetKeywordVariantSet* families[] = {
        &program->user_global,
        &program->user_local,
        &program->builtin,
    };
    OrderedOwnedStringSet disabled = {0};
    for (size_t family = 0; family < 3U; family++) {
        const SnippetKeywordVariantSet* set = families[family];
        for (int i = 0; i < set->combination_count; i++) {
            UnityCompileAuthorityStatus status = add_disabled_combination(
                set->combinations[i], active_names, &disabled);
            if (status != UNITY_COMPILE_AUTHORITY_OK) {
                ordered_owned_string_set_free(&disabled);
                return status;
            }
        }
    }
    for (size_t i = 0; i < UNITY_PLATFORM_CAPABILITY_COUNT; i++) {
        if (!unity_platform_capability_is_settings_dependent(i) ||
            (input->platform_capabilities.bits & (UINT64_C(1) << i))) {
            continue;
        }
        if (!append_owned_string(&disabled,
                                 k_platform_capability_keywords[i])) {
            ordered_owned_string_set_free(&disabled);
            return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
        }
    }
    string_set_free(&disabled.set);
    authority->disabled_keywords = disabled.values;
    authority->disabled_keyword_count = (int)disabled.count;
    return UNITY_COMPILE_AUTHORITY_OK;
}

static uint64_t build_requirements(
    const SnippetCompileContract* contract, const StringSet* active_names) {
    uint64_t requirements = contract->requirements;
    for (int i = 0; i < contract->conditional_requirement_count; i++) {
        const ConditionalShaderRequirement* conditional =
            &contract->conditional_requirements[i];
        if (string_set_contains_span(
                active_names, conditional->keyword,
                strlen(conditional->keyword))) {
            requirements |= conditional->requirements;
        }
    }
    return requirements;
}

UnityCompileAuthorityStatus unity_compile_authority_build(
    const UnityCompileAuthorityInput* input,
    UnityCompileAuthority* out_authority) {
    if (!input || !out_authority) {
        return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    }
    if (!input->contract) {
        return UNITY_COMPILE_AUTHORITY_MISSING_PREPROCESS_CONTRACT;
    }
    if (!unity_compiler_snippet_contract_validate(input->contract) ||
        (input->keyword_name_count > 0 && !input->keyword_names) ||
        input->keyword_name_count > (size_t)INT_MAX) {
        return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    }
    if (!unity_compiler_snippet_contract_has_variant_families(
            input->contract, input->compiler_program)) {
        return UNITY_COMPILE_AUTHORITY_MISSING_VARIANT_FAMILIES;
    }
    if (!input->platform_capabilities.present) {
        return UNITY_COMPILE_AUTHORITY_MISSING_PLATFORM_CAPABILITIES;
    }
    const uint64_t valid_capability_bits =
        (UINT64_C(1) << UNITY_PLATFORM_CAPABILITY_COUNT) - UINT64_C(1);
    if (input->platform_capabilities.bits & ~valid_capability_bits) {
        return UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT;
    }

    UnityCompileAuthority authority;
    unity_compile_authority_init(&authority);
    bool* selected = input->keyword_name_count > 0
                         ? (bool*)calloc(input->keyword_name_count,
                                        sizeof(*selected))
                         : NULL;
    if (input->keyword_name_count > 0 && !selected) {
        return UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY;
    }
    UnityCompileAuthorityStatus status =
        select_keyword_indices(input, selected);
    StringSet active_names = {0};
    if (status == UNITY_COMPILE_AUTHORITY_OK) {
        status = build_user_keywords(
            input, selected, &authority, &active_names);
    }
    if (status == UNITY_COMPILE_AUTHORITY_OK) {
        status = build_platform_keywords(input, &authority);
    }
    if (status == UNITY_COMPILE_AUTHORITY_OK) {
        status = build_disabled_keywords(input, &active_names, &authority);
    }
    if (status == UNITY_COMPILE_AUTHORITY_OK) {
        authority.requirements = build_requirements(
            input->contract, &active_names);
    }
    free(selected);
    string_set_free(&active_names);
    if (status != UNITY_COMPILE_AUTHORITY_OK) {
        unity_compile_authority_free(&authority);
        return status;
    }
    unity_compile_authority_free(out_authority);
    *out_authority = authority;
    return UNITY_COMPILE_AUTHORITY_OK;
}

const char* unity_compile_authority_status_string(
    UnityCompileAuthorityStatus status) {
    switch (status) {
        case UNITY_COMPILE_AUTHORITY_OK: return "ok";
        case UNITY_COMPILE_AUTHORITY_INVALID_ARGUMENT:
            return "invalid compile-authority input";
        case UNITY_COMPILE_AUTHORITY_MISSING_PREPROCESS_CONTRACT:
            return "lossless preprocess contract is unavailable";
        case UNITY_COMPILE_AUTHORITY_MISSING_VARIANT_FAMILIES:
            return "preprocess keyword-variant families are unavailable";
        case UNITY_COMPILE_AUTHORITY_MISSING_PLATFORM_CAPABILITIES:
            return "Unity platform-capability snapshot is unavailable";
        case UNITY_COMPILE_AUTHORITY_INVALID_KEYWORD_INDEX:
            return "serialized keyword index is invalid";
        case UNITY_COMPILE_AUTHORITY_AMBIGUOUS_KEYWORD_NAME:
            return "multiple selected keyword indices have the same name";
        case UNITY_COMPILE_AUTHORITY_OUT_OF_MEMORY:
            return "out of memory while building compile authority";
        default: return "unknown compile-authority status";
    }
}
