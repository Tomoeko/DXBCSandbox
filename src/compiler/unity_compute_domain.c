// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compute_domain.h"

#include "common/source_scan.h"

#include <stdlib.h>
#include <string.h>

enum {
    DOMAIN_MAX_ITEMS = 1024,
    DOMAIN_MAX_STRING_BYTES = 1024 * 1024,
    DOMAIN_MAX_METADATA_BYTES = 8 * 1024 * 1024,
    DOMAIN_MAX_SOURCE_BYTES = 512 * 1024 * 1024
};

typedef struct {
    UnityComputeDomainScope scope;
    size_t scope_index;
    const char* raw_line;
    char* text;
    const char** choices;
    size_t choice_count;
    size_t stride;
} DomainFamily;

struct UnityComputeDomain {
    const UnityCompilerComputePreprocessResult* result;
    const UnityCompilerComputePreprocessRequest* request;
    DomainFamily* families;
    size_t family_count;
    size_t global_family_count;
    size_t variant_count;
    size_t state_count;
};

static void diagnostic_init(UnityComputeDomainDiagnostic* diagnostic) {
    if (!diagnostic)
        return;
    *diagnostic = (UnityComputeDomainDiagnostic){
        .family_index = SIZE_MAX,
        .choice_index = SIZE_MAX,
        .kernel_index = SIZE_MAX,
        .macro_index = SIZE_MAX,
        .conditional_index = SIZE_MAX,
    };
}

static UnityComputeDomainStatus fail(UnityComputeDomainDiagnostic* diagnostic,
                                     UnityComputeDomainStatus status) {
    if (diagnostic)
        diagnostic->status = status;
    return status;
}

static bool bounded_text_size(const char* text, size_t limit, size_t* out_size) {
    if (!text || !out_size)
        return false;
    size_t size = 0U;
    while (size <= limit && text[size])
        ++size;
    if (size > limit)
        return false;
    *out_size = size;
    return true;
}

static bool metadata_text_size(const char* text, size_t* total, size_t* size) {
    if (!bounded_text_size(text, DOMAIN_MAX_STRING_BYTES, size) ||
        *size >= DOMAIN_MAX_METADATA_BYTES - *total)
        return false;
    *total += *size + 1U;
    return true;
}

static bool identifier_span(const char* name, size_t length) {
    if (!length || !source_scan_identifier_start((uint8_t)name[0]))
        return false;
    for (size_t index = 1U; index < length; ++index)
        if (!source_scan_identifier_continue((uint8_t)name[index]))
            return false;
    return length != 7U || memcmp(name, "defined", 7U) != 0;
}

static bool all_underscores(const char* text, size_t size) {
    if (!size)
        return false;
    for (size_t index = 0U; index < size; ++index)
        if (text[index] != '_')
            return false;
    return true;
}

static bool names_contain(const char* const* names, size_t count, const char* name) {
    for (size_t index = 0U; index < count; ++index)
        if (strcmp(names[index], name) == 0)
            return true;
    return false;
}

static bool families_contain(const UnityComputeDomain* domain, const char* name) {
    for (size_t family = 0U; family < domain->family_count; ++family)
        for (size_t choice = 0U; choice < domain->families[family].choice_count; ++choice) {
            const char* candidate = domain->families[family].choices[choice];
            if (candidate && strcmp(candidate, name) == 0)
                return true;
        }
    return false;
}

static UnityComputeDomainStatus validate_context_names(const char* const* names, size_t count,
                                                       size_t* text_size,
                                                       UnityComputeDomainDiagnostic* diagnostic) {
    if (count > DOMAIN_MAX_ITEMS)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    if (count && !names)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    for (size_t index = 0U; index < count; ++index) {
        size_t length = 0U;
        if (!metadata_text_size(names[index], text_size, &length))
            return fail(diagnostic, names[index] ? UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED
                                                 : UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
        if (!identifier_span(names[index], length) || all_underscores(names[index], length))
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_UNSUPPORTED_FAMILY_SYNTAX);
        if (names_contain(names, index, names[index]))
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD);
    }
    return UNITY_COMPUTE_DOMAIN_OK;
}

static UnityComputeDomainStatus parse_family(UnityComputeDomain* domain, DomainFamily* family,
                                             size_t* text_size, size_t* total_choices,
                                             UnityComputeDomainDiagnostic* diagnostic) {
    if (diagnostic) {
        diagnostic->scope = family->scope;
        diagnostic->family_index = family->scope_index;
        diagnostic->choice_index = SIZE_MAX;
    }
    size_t length = 0U;
    if (!metadata_text_size(family->raw_line, text_size, &length))
        return fail(diagnostic, family->raw_line ? UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED
                                                 : UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    if (!length)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_UNSUPPORTED_EMPTY_FAMILY);

    size_t choices = 1U;
    for (size_t index = 0U; index < length; ++index) {
        if (family->raw_line[index] != ' ')
            continue;
        if (index == 0U || index + 1U == length || family->raw_line[index - 1U] == ' ')
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_UNSUPPORTED_FAMILY_SYNTAX);
        ++choices;
    }
    if (choices > DOMAIN_MAX_ITEMS - *total_choices)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    *total_choices += choices;
    family->text = malloc(length + 1U);
    family->choices = calloc(choices, sizeof(*family->choices));
    if (!family->text || !family->choices)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_OUT_OF_MEMORY);
    memcpy(family->text, family->raw_line, length + 1U);

    bool has_default = false;
    char* start = family->text;
    for (size_t index = 0U; index < choices; ++index) {
        if (diagnostic)
            diagnostic->choice_index = index;
        char* end = strchr(start, ' ');
        const size_t token_length = end ? (size_t)(end - start) : strlen(start);
        if (end)
            *end = '\0';
        if (!identifier_span(start, token_length))
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_UNSUPPORTED_FAMILY_SYNTAX);
        if (all_underscores(start, token_length)) {
            if (has_default)
                return fail(diagnostic, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD);
            has_default = true;
        } else {
            if (families_contain(domain, start) ||
                names_contain((const char* const*)domain->request->platform_keywords,
                              (size_t)domain->request->platform_keyword_count, start) ||
                names_contain((const char* const*)domain->request->disabled_keywords,
                              (size_t)domain->request->disabled_keyword_count, start))
                return fail(diagnostic, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD);
            family->choices[index] = start;
        }
        family->choice_count = index + 1U;
        if (end)
            start = end + 1U;
    }
    return UNITY_COMPUTE_DOMAIN_OK;
}

static UnityComputeDomainStatus validate_kernels(const UnityCompilerComputePreprocessResult* result,
                                                 size_t* text_size,
                                                 UnityComputeDomainDiagnostic* diagnostic) {
    if (!result->kernel_count)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_NO_KERNELS);
    if (result->kernel_count > DOMAIN_MAX_ITEMS)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    if (!result->kernels)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    size_t macro_count = 0U;
    for (size_t kernel_index = 0U; kernel_index < result->kernel_count; ++kernel_index) {
        if (diagnostic)
            diagnostic->kernel_index = kernel_index;
        const UnityCompilerComputePreprocessedKernel* kernel = &result->kernels[kernel_index];
        size_t length = 0U;
        if (!metadata_text_size(kernel->name, text_size, &length))
            return fail(diagnostic, kernel->name ? UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED
                                                 : UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
        if (!identifier_span(kernel->name, length))
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KERNEL);
        for (size_t previous = 0U; previous < kernel_index; ++previous)
            if (strcmp(result->kernels[previous].name, kernel->name) == 0)
                return fail(diagnostic, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KERNEL);
        if (kernel->macro_count > DOMAIN_MAX_ITEMS - macro_count)
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
        macro_count += kernel->macro_count;
        if (kernel->macro_count && !kernel->macros)
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
        for (size_t macro = 0U; macro < kernel->macro_count; ++macro) {
            if (diagnostic)
                diagnostic->macro_index = macro;
            const UnityCompilerComputePreprocessMacro* entry = &kernel->macros[macro];
            if (!metadata_text_size(entry->name, text_size, &length) ||
                !metadata_text_size(entry->value, text_size, &length))
                return fail(diagnostic, entry->name && entry->value
                                            ? UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED
                                            : UNITY_COMPUTE_DOMAIN_INVALID_KERNEL_MACRO);
            if (!identifier_span(entry->name, strlen(entry->name)))
                return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_KERNEL_MACRO);
            for (size_t previous = 0U; previous < macro; ++previous)
                if (strcmp(kernel->macros[previous].name, entry->name) == 0)
                    return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_KERNEL_MACRO);
        }
    }
    return UNITY_COMPUTE_DOMAIN_OK;
}

static UnityComputeDomainStatus validate_conditionals(const UnityComputeDomain* domain,
                                                      size_t* text_size,
                                                      UnityComputeDomainDiagnostic* diagnostic) {
    const UnityCompilerComputePreprocessResult* result = domain->result;
    const UnityCompilerComputePreprocessRequest* request = domain->request;
    if (result->conditional_requirement_count > DOMAIN_MAX_ITEMS)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    if (result->conditional_requirement_count && !result->conditional_requirements)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    for (size_t index = 0U; index < result->conditional_requirement_count; ++index) {
        if (diagnostic)
            diagnostic->conditional_index = index;
        const char* name = result->conditional_requirements[index].keyword;
        size_t length = 0U;
        if (!metadata_text_size(name, text_size, &length))
            return fail(diagnostic, name ? UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED
                                         : UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
        if (!identifier_span(name, length) || all_underscores(name, length))
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_CONDITIONAL_CONTEXT_UNAVAILABLE);
        for (size_t previous = 0U; previous < index; ++previous)
            if (strcmp(result->conditional_requirements[previous].keyword, name) == 0)
                return fail(diagnostic, UNITY_COMPUTE_DOMAIN_CONDITIONAL_CONTEXT_UNAVAILABLE);
        if (!families_contain(domain, name) &&
            !names_contain((const char* const*)request->platform_keywords,
                           (size_t)request->platform_keyword_count, name) &&
            !names_contain((const char* const*)request->disabled_keywords,
                           (size_t)request->disabled_keyword_count, name))
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_CONDITIONAL_CONTEXT_UNAVAILABLE);
    }
    return UNITY_COMPUTE_DOMAIN_OK;
}

void unity_compute_domain_free(UnityComputeDomain* domain) {
    if (!domain)
        return;
    for (size_t family = 0U; family < domain->family_count; ++family) {
        free(domain->families[family].text);
        free(domain->families[family].choices);
    }
    free(domain->families);
    free(domain);
}

UnityComputeDomainStatus unity_compute_domain_create(
    const UnityCompilerComputePreprocessResult* result, const UnityComputeDomainContext* context,
    UnityComputeDomain** out_domain, UnityComputeDomainDiagnostic* diagnostic) {
    diagnostic_init(diagnostic);
    if (!result || !context || !context->preprocess_request || !out_domain || *out_domain)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    const UnityCompilerComputePreprocessRequest* request = context->preprocess_request;
    if (!context->max_variant_count || !context->max_kernel_state_count ||
        context->max_variant_count > UNITY_COMPUTE_DOMAIN_MAX_STATES ||
        context->max_kernel_state_count > UNITY_COMPUTE_DOMAIN_MAX_STATES)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    if (request->platform_keyword_count < 0 || request->disabled_keyword_count < 0 ||
        !request->source || !request->source_filename || !request->source_filename[0] ||
        !result->source)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    if (result->source_size > DOMAIN_MAX_SOURCE_BYTES)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    size_t source_size = 0U;
    if (!bounded_text_size(result->source, DOMAIN_MAX_SOURCE_BYTES, &source_size))
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    if (source_size != result->source_size)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    if (result->user_global.line_count > DOMAIN_MAX_ITEMS ||
        result->user_local.line_count > DOMAIN_MAX_ITEMS - result->user_global.line_count)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    if ((result->user_global.line_count && !result->user_global.lines) ||
        (result->user_local.line_count && !result->user_local.lines))
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);

    size_t text_size = 0U, length = 0U;
    if (!metadata_text_size(request->source_filename, &text_size, &length))
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    UnityComputeDomainStatus status =
        validate_context_names((const char* const*)request->platform_keywords,
                               (size_t)request->platform_keyword_count, &text_size, diagnostic);
    if (status != UNITY_COMPUTE_DOMAIN_OK)
        return status;
    status =
        validate_context_names((const char* const*)request->disabled_keywords,
                               (size_t)request->disabled_keyword_count, &text_size, diagnostic);
    if (status != UNITY_COMPUTE_DOMAIN_OK)
        return status;
    for (int index = 0; index < request->platform_keyword_count; ++index)
        if (names_contain((const char* const*)request->disabled_keywords,
                          (size_t)request->disabled_keyword_count,
                          request->platform_keywords[index]))
            return fail(diagnostic, UNITY_COMPUTE_DOMAIN_AMBIGUOUS_KEYWORD);
    status = validate_kernels(result, &text_size, diagnostic);
    if (status != UNITY_COMPUTE_DOMAIN_OK)
        return status;
    if (result->dependency_count > DOMAIN_MAX_ITEMS)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
    if (result->dependency_count && !result->dependencies)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    for (size_t index = 0U; index < result->dependency_count; ++index)
        if (!metadata_text_size(result->dependencies[index], &text_size, &length))
            return fail(diagnostic, result->dependencies[index]
                                        ? UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED
                                        : UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);

    UnityComputeDomain* domain = calloc(1U, sizeof(*domain));
    if (!domain)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_OUT_OF_MEMORY);
    domain->result = result;
    domain->request = request;
    domain->global_family_count = result->user_global.line_count;
    domain->family_count = result->user_global.line_count + result->user_local.line_count;
    domain->families =
        domain->family_count ? calloc(domain->family_count, sizeof(*domain->families)) : NULL;
    if (domain->family_count && !domain->families) {
        free(domain);
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_OUT_OF_MEMORY);
    }
    size_t choices = 0U;
    diagnostic_init(diagnostic);
    for (size_t index = 0U; index < domain->family_count; ++index) {
        DomainFamily* family = &domain->families[index];
        const bool global = index < domain->global_family_count;
        family->scope =
            global ? UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL : UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL;
        family->scope_index = global ? index : index - domain->global_family_count;
        family->raw_line = global ? result->user_global.lines[family->scope_index]
                                  : result->user_local.lines[family->scope_index];
        status = parse_family(domain, family, &text_size, &choices, diagnostic);
        if (status != UNITY_COMPUTE_DOMAIN_OK)
            goto failed;
    }
    diagnostic_init(diagnostic);
    status = validate_conditionals(domain, &text_size, diagnostic);
    if (status != UNITY_COMPUTE_DOMAIN_OK)
        goto failed;
    diagnostic_init(diagnostic);
    domain->variant_count = 1U;
    for (size_t remaining = domain->family_count; remaining > 0U; --remaining) {
        DomainFamily* family = &domain->families[remaining - 1U];
        family->stride = domain->variant_count;
        if (family->choice_count > context->max_variant_count / domain->variant_count) {
            status = fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
            goto failed;
        }
        domain->variant_count *= family->choice_count;
    }
    if (result->kernel_count > context->max_kernel_state_count / domain->variant_count) {
        status = fail(diagnostic, UNITY_COMPUTE_DOMAIN_LIMIT_EXCEEDED);
        goto failed;
    }
    domain->state_count = result->kernel_count * domain->variant_count;
    diagnostic_init(diagnostic);
    *out_domain = domain;
    return UNITY_COMPUTE_DOMAIN_OK;

failed:
    unity_compute_domain_free(domain);
    return status;
}

size_t unity_compute_domain_variant_count(const UnityComputeDomain* domain) {
    return domain ? domain->variant_count : 0U;
}

size_t unity_compute_domain_state_count(const UnityComputeDomain* domain) {
    return domain ? domain->state_count : 0U;
}

size_t unity_compute_domain_family_count(const UnityComputeDomain* domain,
                                         UnityComputeDomainScope scope) {
    if (!domain)
        return 0U;
    if (scope == UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL)
        return domain->global_family_count;
    if (scope == UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL)
        return domain->family_count - domain->global_family_count;
    return 0U;
}

UnityComputeDomainStatus unity_compute_domain_family_at(const UnityComputeDomain* domain,
                                                        UnityComputeDomainScope scope,
                                                        size_t family_index,
                                                        UnityComputeDomainFamily* out_family) {
    if (!domain || !out_family ||
        (scope != UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL && scope != UNITY_COMPUTE_DOMAIN_SCOPE_LOCAL))
        return UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT;
    if (family_index >= unity_compute_domain_family_count(domain, scope))
        return UNITY_COMPUTE_DOMAIN_INVALID_STATE_INDEX;
    const size_t offset =
        scope == UNITY_COMPUTE_DOMAIN_SCOPE_GLOBAL ? 0U : domain->global_family_count;
    const DomainFamily* family = &domain->families[offset + family_index];
    *out_family =
        (UnityComputeDomainFamily){family->raw_line, family->choices, family->choice_count};
    return UNITY_COMPUTE_DOMAIN_OK;
}

static const char* selected_choice(const DomainFamily* family, size_t variant_index) {
    return family->choices[(variant_index / family->stride) % family->choice_count];
}

UnityComputeDomainStatus
unity_compute_domain_state_at(const UnityComputeDomain* domain, size_t state_index,
                              const char** keyword_buffer, size_t keyword_capacity,
                              size_t* out_required_count, UnityComputeDomainState* out_state,
                              UnityComputeDomainDiagnostic* diagnostic) {
    diagnostic_init(diagnostic);
    if (!domain || !out_required_count || (!keyword_buffer && keyword_capacity) ||
        (!out_state && (keyword_buffer || keyword_capacity)))
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT);
    if (state_index >= domain->state_count)
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_INVALID_STATE_INDEX);
    const size_t variant_index = state_index % domain->variant_count;
    UnityComputeDomainState state = {
        .kernel_index = state_index / domain->variant_count,
        .variant_index = variant_index,
        .preprocess_result = domain->result,
        .preprocess_request = domain->request,
        .requirements = domain->result->requirements,
    };
    state.kernel = &domain->result->kernels[state.kernel_index];
    for (size_t family = 0U; family < domain->family_count; ++family) {
        if (!selected_choice(&domain->families[family], variant_index))
            continue;
        if (family < domain->global_family_count)
            ++state.global_keyword_count;
        else
            ++state.local_keyword_count;
    }
    state.user_keyword_count = state.global_keyword_count + state.local_keyword_count;
    *out_required_count = state.user_keyword_count;
    if (!out_state)
        return UNITY_COMPUTE_DOMAIN_OK;
    if (keyword_capacity < state.user_keyword_count ||
        (state.user_keyword_count && !keyword_buffer))
        return fail(diagnostic, UNITY_COMPUTE_DOMAIN_BUFFER_TOO_SMALL);
    for (size_t conditional = 0U; conditional < domain->result->conditional_requirement_count;
         ++conditional) {
        const UnityCompilerComputeConditionalRequirement* requirement =
            &domain->result->conditional_requirements[conditional];
        bool selected =
            names_contain((const char* const*)domain->request->platform_keywords,
                          (size_t)domain->request->platform_keyword_count, requirement->keyword);
        for (size_t family = 0U; !selected && family < domain->family_count; ++family) {
            const char* name = selected_choice(&domain->families[family], variant_index);
            selected = name && strcmp(name, requirement->keyword) == 0;
        }
        if (selected)
            state.requirements |= requirement->requirements;
    }
    size_t output = 0U;
    for (size_t family = 0U; family < domain->family_count; ++family) {
        const char* name = selected_choice(&domain->families[family], variant_index);
        if (name)
            keyword_buffer[output++] = name;
    }
    state.user_keywords = state.user_keyword_count ? keyword_buffer : NULL;
    state.global_keywords = state.global_keyword_count ? keyword_buffer : NULL;
    state.local_keywords =
        state.local_keyword_count ? keyword_buffer + state.global_keyword_count : NULL;
    *out_state = state;
    return UNITY_COMPUTE_DOMAIN_OK;
}

UnityComputeDomainStatus unity_compute_domain_require_api(const UnityComputeDomain* domain,
                                                          uint32_t compiler_platform) {
    if (!domain || compiler_platform >= 32U)
        return UNITY_COMPUTE_DOMAIN_INVALID_ARGUMENT;
    return ((uint32_t)domain->result->supported_apis & (UINT32_C(1) << compiler_platform))
               ? UNITY_COMPUTE_DOMAIN_OK
               : UNITY_COMPUTE_DOMAIN_UNSUPPORTED_API;
}
