// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_generated_domain_certifier.h"

#include "common/shader_stage.h"
#include "common/sha256.h"
#include "common/stream.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_parser.h"
#include "dxbc/dxbc_stage_contract.h"
#include "io/subprogram_metadata.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    UNITY_D3D11_COMPILER_PLATFORM = 4,
    UNITY_GENERATED_STAGE_COUNT = 5,
    UNITY_SERIALIZED_STAGE_COUNT_WITH_RAYTRACE = 6,
    UNITY_HARDWARE_TIER_FIRST_CAPABILITY = 19
};

typedef struct {
    uint16_t* indices;
    size_t count;
    bool has_default;
} OrderedKeywordRow;

typedef struct {
    OrderedKeywordRow* rows;
    size_t count;
    size_t capacity;
    bool active;
} ExpectedKeywordFamily;

typedef struct {
    int8_t* families;
    size_t keyword_count;
    ShaderLabBuiltinVariantDomain builtin_domain;
    bool has_builtin_domain;
} StageKeywordClassification;

static void diagnostic_coordinates_init(
    UnityGeneratedDomainDiagnostic* diagnostic) {
    diagnostic->stage_index = -1;
    diagnostic->compiler_program = -1;
    diagnostic->hardware_tier_group = -1;
    diagnostic->generated_state_index = SIZE_MAX;
    diagnostic->aliased_state_index = SIZE_MAX;
    diagnostic->subprogram_index = -1;
    diagnostic->keyword_family = UNITY_GENERATED_DOMAIN_FAMILY_NONE;
    diagnostic->contract_row_index = SIZE_MAX;
    diagnostic->compile_authority_status = UNITY_COMPILE_AUTHORITY_OK;
    dxbc_compare_result_init(&diagnostic->dxbc_compare);
    unity_reflection_certificate_report_init(
        &diagnostic->reflection_certificate);
}

void unity_generated_domain_report_init(UnityGeneratedDomainReport* report) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->glsl_status =
        UNITY_GENERATED_GLSL_UNAVAILABLE_NO_PRECISION_AUTHORITY;
    unity_compiler_response_status_init(
        &report->diagnostic.compiler_response);
    diagnostic_coordinates_init(&report->diagnostic);
}

void unity_generated_domain_report_free(UnityGeneratedDomainReport* report) {
    if (!report) return;
    for (size_t i = 0; i < report->compiler_response_count; ++i) {
        unity_compiler_response_status_free(
            &report->compiler_responses[i].response);
        unity_compiler_response_status_free(
            &report->compiler_responses[i].original_response);
    }
    free(report->compiler_responses);
    unity_compiler_response_status_free(
        &report->diagnostic.compiler_response);
    unity_generated_domain_report_init(report);
}

static bool normalized_diagnostic_equal(
    const UnityCompilerDiagnostic* left,
    const UnityCompilerDiagnostic* right) {
    return left && right && left->message && right->message &&
        unity_compiler_diagnostic_is_actionable(left) ==
            unity_compiler_diagnostic_is_actionable(right) &&
        left->fields[0] == right->fields[0] &&
        left->fields[1] == right->fields[1] &&
        strcmp(left->message, right->message) == 0;
}

bool unity_generated_domain_diagnostics_match_normalized(
    const UnityCompilerResponseStatus* generated,
    const UnityCompilerResponseStatus* original) {
    if (!generated || !original ||
        generated->diagnostic_count != original->diagnostic_count) {
        return false;
    }
    const size_t count = generated->diagnostic_count;
    if (count == 0U) return true;
    bool* matched = (bool*)calloc(count, sizeof(*matched));
    if (!matched) return false;
    bool equal = true;
    for (size_t generated_index = 0U; generated_index < count;
         ++generated_index) {
        bool found = false;
        for (size_t original_index = 0U; original_index < count;
             ++original_index) {
            if (matched[original_index] ||
                !normalized_diagnostic_equal(
                    &generated->diagnostics[generated_index],
                    &original->diagnostics[original_index])) {
                continue;
            }
            matched[original_index] = true;
            found = true;
            break;
        }
        if (!found) {
            equal = false;
            break;
        }
    }
    free(matched);
    return equal;
}

static UnityGeneratedDomainStatus fail_report(
    UnityGeneratedDomainReport* report, UnityGeneratedDomainStatus status) {
    report->status = status;
    report->diagnostic.status = status;
    return status;
}

static void free_keyword_row(OrderedKeywordRow* row) {
    if (!row) return;
    free(row->indices);
    memset(row, 0, sizeof(*row));
}

static void free_expected_family(ExpectedKeywordFamily* family) {
    if (!family) return;
    for (size_t i = 0; i < family->count; ++i) {
        free_keyword_row(&family->rows[i]);
    }
    free(family->rows);
    memset(family, 0, sizeof(*family));
}

static bool ordered_rows_equal(
    const OrderedKeywordRow* left, const OrderedKeywordRow* right) {
    return left && right && left->has_default == right->has_default &&
           left->count == right->count &&
           (left->count == 0U ||
            memcmp(left->indices, right->indices,
                   left->count * sizeof(*left->indices)) == 0);
}

static bool rows_have_same_members(
    const OrderedKeywordRow* left, const OrderedKeywordRow* right) {
    if (!left || !right || left->has_default != right->has_default ||
        left->count != right->count) {
        return false;
    }
    for (size_t left_index = 0U; left_index < left->count; ++left_index) {
        bool found = false;
        for (size_t right_index = 0U; right_index < right->count;
             ++right_index) {
            if (left->indices[left_index] == right->indices[right_index]) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool append_unique_row(
    ExpectedKeywordFamily* family, const OrderedKeywordRow* row) {
    for (size_t i = 0; i < family->count; ++i) {
        if (ordered_rows_equal(&family->rows[i], row)) return true;
    }
    if (family->count == family->capacity) {
        size_t capacity = family->capacity == 0U ? 4U : family->capacity * 2U;
        if (capacity < family->capacity ||
            capacity > SIZE_MAX / sizeof(*family->rows)) {
            return false;
        }
        OrderedKeywordRow* rows = (OrderedKeywordRow*)realloc(
            family->rows, capacity * sizeof(*rows));
        if (!rows) return false;
        family->rows = rows;
        family->capacity = capacity;
    }
    OrderedKeywordRow copy = {0};
    copy.has_default = row->has_default;
    if (row->count > 0U) {
        if (row->count > SIZE_MAX / sizeof(*copy.indices)) return false;
        copy.indices = (uint16_t*)malloc(
            row->count * sizeof(*copy.indices));
        if (!copy.indices) return false;
        memcpy(copy.indices, row->indices,
               row->count * sizeof(*copy.indices));
        copy.count = row->count;
    }
    family->rows[family->count++] = copy;
    return true;
}

static bool state_contains(
    const ShaderLabVariantState* state, uint16_t keyword) {
    size_t low = 0U;
    size_t high = state->keyword_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (state->keyword_indices[middle] == keyword) return true;
        if (state->keyword_indices[middle] < keyword) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return false;
}

static int classified_keyword_index(
    const SerializedShader* shader,
    const StageKeywordClassification* classification,
    UnityGeneratedDomainKeywordFamily family,
    const char* name,
    size_t length) {
    int found = -1;
    for (int index = 0; index < shader->keyword_names.count; ++index) {
        const char* candidate = shader->keyword_names.keywords[index];
        if (!candidate || strlen(candidate) != length ||
            memcmp(candidate, name, length) != 0 ||
            classification->families[index] != (int8_t)family) {
            continue;
        }
        if (found >= 0) return -2;
        found = index;
    }
    return found;
}

static bool stage_uses_keyword_index(
    const ShaderLabPassStageVariantPlan* stage, uint16_t raw) {
    if (!stage || !stage->ordered_states) return false;
    for (size_t state = 0U; state < stage->state_count; ++state) {
        if (state_contains(&stage->ordered_states[state], raw)) return true;
    }
    return false;
}

static int unclassified_stage_keyword_index(
    const SerializedShader* shader,
    const ShaderLabPassStageVariantPlan* stage,
    const StageKeywordClassification* classification,
    const char* name,
    size_t length) {
    int found = -1;
    for (int index = 0; index < shader->keyword_names.count; ++index) {
        const char* candidate = shader->keyword_names.keywords[index];
        if (!candidate || strlen(candidate) != length ||
            memcmp(candidate, name, length) != 0 ||
            !stage_uses_keyword_index(stage, (uint16_t)index)) {
            continue;
        }
        if (classification->families[index] >= 0 || found >= 0) return -2;
        found = index;
    }
    return found;
}

static UnityGeneratedDomainStatus classify_stage_keywords(
    const SerializedShader* shader, const ShaderLabVariantPlan* plan,
    int stage_index, StageKeywordClassification* classification,
    UnityGeneratedDomainReport* report) {
    memset(classification, 0, sizeof(*classification));
    classification->keyword_count = (size_t)shader->keyword_names.count;
    if (classification->keyword_count > 0U) {
        classification->families = (int8_t*)malloc(
            classification->keyword_count * sizeof(*classification->families));
        if (!classification->families) {
            return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
        }
        memset(classification->families, -1,
               classification->keyword_count *
                   sizeof(*classification->families));
    }

    const ShaderLabPassStageVariantPlan* stage = &plan->stages[stage_index];
    for (size_t axis_index = 0; axis_index < stage->axis_count; ++axis_index) {
        const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
        if (!axis->keyword_indices || axis->keyword_count == 0U) {
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
        }
        const int8_t family = axis->is_local
            ? (int8_t)UNITY_GENERATED_DOMAIN_FAMILY_USER_LOCAL
            : (int8_t)UNITY_GENERATED_DOMAIN_FAMILY_USER_GLOBAL;
        for (size_t keyword = 0; keyword < axis->keyword_count; ++keyword) {
            const uint16_t raw = axis->keyword_indices[keyword];
            if ((size_t)raw >= classification->keyword_count) {
                report->diagnostic.contract_row_index = axis_index;
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
            }
            if (classification->families[raw] >= 0) {
                report->diagnostic.contract_row_index = axis_index;
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS);
            }
            const bool serialized_local =
                (shader->keyword_flags[raw] & UINT8_C(1)) != 0U;
            if (serialized_local != axis->is_local) {
                report->diagnostic.keyword_family =
                    (UnityGeneratedDomainKeywordFamily)family;
                report->diagnostic.contract_row_index = axis_index;
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_KEYWORD_SCOPE_MISMATCH);
            }
            classification->families[raw] = family;
        }
    }

    if (plan->has_builtin) {
        if (!shaderlab_builtin_variant_domain_get(
                plan->builtin_family,
                (UnitySerializedProgramStage)stage_index,
                &classification->builtin_domain)) {
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
        }
        classification->has_builtin_domain = true;
        for (size_t keyword = 0;
             keyword < classification->builtin_domain.keyword_count;
             ++keyword) {
            const char* name =
                classification->builtin_domain.keyword_names[keyword];
            bool used = false;
            for (size_t row = 0;
                 row < classification->builtin_domain.variant_count &&
                 !used; ++row) {
                used = shaderlab_builtin_variant_is_included(
                           &classification->builtin_domain, row,
                     plan->builtin_exclusions) &&
                    (classification->builtin_domain.variant_masks[row] &
                     (uint16_t)(UINT16_C(1) << keyword)) != 0U;
            }
            if (!used) continue;
            const int raw = unclassified_stage_keyword_index(
                shader, stage, classification, name, strlen(name));
            if (raw == -2) {
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS);
            }
            if (raw < 0) {
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
            }
            if ((shader->keyword_flags[raw] & UINT8_C(1)) != 0U) {
                report->diagnostic.keyword_family =
                    UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN;
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_KEYWORD_SCOPE_MISMATCH);
            }
            classification->families[raw] =
                (int8_t)UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN;
        }
    }
    return UNITY_GENERATED_DOMAIN_OK;
}

static void free_classification(StageKeywordClassification* classification) {
    if (!classification) return;
    free(classification->families);
    memset(classification, 0, sizeof(*classification));
}

static bool row_append_ordered(OrderedKeywordRow* row, uint16_t raw) {
    if (row->count == SIZE_MAX / sizeof(*row->indices)) return false;
    uint16_t* indices = (uint16_t*)realloc(
        row->indices, (row->count + 1U) * sizeof(*indices));
    if (!indices) return false;
    row->indices = indices;
    row->indices[row->count++] = raw;
    return true;
}

static bool row_append_canonical_set(OrderedKeywordRow* row, uint16_t raw) {
    if (!row_append_ordered(row, raw)) return false;
    /* A contract combination is a simultaneous keyword set.  Unity may
     * return the tokens in a different order than its pinned built-in mask
     * table (for example VERTEXLIGHT_ON before LIGHTPROBE_SH).  Canonicalize
     * built-in sets by serialized keyword index while still preserving the
     * order of combination rows themselves.  User rows are pragma option
     * lists, so their token order must remain exact. */
    size_t insertion = 0U;
    while (insertion + 1U < row->count &&
           row->indices[insertion] < raw) {
        ++insertion;
    }
    memmove(&row->indices[insertion + 1U], &row->indices[insertion],
            (row->count - insertion - 1U) * sizeof(*row->indices));
    row->indices[insertion] = raw;
    return true;
}

static UnityGeneratedDomainStatus append_axis_contract_row(
    const ShaderLabVariantAxis* axis, ExpectedKeywordFamily* family,
    UnityGeneratedDomainReport* report) {
    OrderedKeywordRow row = {0};
    row.has_default = axis->has_default;
    for (size_t keyword = 0; keyword < axis->keyword_count; ++keyword) {
        if (!row_append_ordered(&row, axis->keyword_indices[keyword])) {
            free_keyword_row(&row);
            return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
        }
    }
    const size_t previous_count = family->count;
    const bool appended = append_unique_row(family, &row);
    free_keyword_row(&row);
    if (!appended) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }
    if (family->count != previous_count + 1U) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS);
    }
    return UNITY_GENERATED_DOMAIN_OK;
}

typedef struct {
    ExpectedKeywordFamily* families;
    UnityGeneratedDomainReport* report;
    UnityGeneratedDomainStatus status;
} ExpectedAxisVisitorContext;

static bool append_expected_emitted_axis(
    const ShaderLabVariantAxis* axis, void* opaque) {
    ExpectedAxisVisitorContext* context =
        (ExpectedAxisVisitorContext*)opaque;
    if (!axis || !context || !context->families || !context->report)
        return false;
    const size_t family = axis->is_local ? 1U : 0U;
    context->families[family].active = true;
    context->status = append_axis_contract_row(
        axis, &context->families[family], context->report);
    return context->status == UNITY_GENERATED_DOMAIN_OK;
}

static UnityGeneratedDomainStatus append_builtin_contract_rows(
    const SerializedShader* shader, const ShaderLabVariantPlan* plan,
    const StageKeywordClassification* classification,
    ExpectedKeywordFamily* family, UnityGeneratedDomainReport* report) {
    const ShaderLabBuiltinVariantDomain* domain =
        &classification->builtin_domain;
    for (size_t variant = 0; variant < domain->variant_count; ++variant) {
        if (!shaderlab_builtin_variant_is_included(
                domain, variant, plan->builtin_exclusions)) continue;
        OrderedKeywordRow row = {0};
        const uint16_t mask = domain->variant_masks[variant];
        row.has_default = mask == 0U;
        for (size_t bit = 0; bit < domain->keyword_count; ++bit) {
            if ((mask & (uint16_t)(UINT16_C(1) << bit)) == 0U) continue;
            const char* name = domain->keyword_names[bit];
            const int raw = classified_keyword_index(
                shader, classification,
                UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN,
                name, strlen(name));
            if (raw < 0 ||
                !row_append_canonical_set(&row, (uint16_t)raw)) {
                free_keyword_row(&row);
                return fail_report(
                    report, raw < 0
                        ? UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE
                        : UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
            }
        }
        const size_t previous_count = family->count;
        const bool appended = append_unique_row(family, &row);
        free_keyword_row(&row);
        if (!appended) {
            return fail_report(report,
                               UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
        }
        if (family->count != previous_count + 1U) {
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS);
        }
    }
    return UNITY_GENERATED_DOMAIN_OK;
}

static UnityGeneratedDomainStatus project_generated_state(
    const SerializedShader* shader, const ShaderLabVariantPlan* plan,
    int stage_index, const StageKeywordClassification* classification,
    const ShaderLabVariantState* state, OrderedKeywordRow rows[3],
    UnityGeneratedDomainReport* report) {
    if (!shaderlab_variant_state_is_canonical(state)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
    }
    uint8_t* accounted = classification->keyword_count > 0U
        ? (uint8_t*)calloc(classification->keyword_count, 1U)
        : NULL;
    if (classification->keyword_count > 0U && !accounted) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }

    UnityGeneratedDomainStatus status = UNITY_GENERATED_DOMAIN_OK;
    const ShaderLabPassStageVariantPlan* stage = &plan->stages[stage_index];
    for (size_t axis_index = 0; axis_index < stage->axis_count; ++axis_index) {
        const ShaderLabVariantAxis* axis = &stage->axes[axis_index];
        size_t selected_count = 0U;
        for (size_t keyword = 0; keyword < axis->keyword_count; ++keyword) {
            const uint16_t raw = axis->keyword_indices[keyword];
            if (!state_contains(state, raw)) continue;
            ++selected_count;
            accounted[raw] = 1U;
            const size_t family = axis->is_local ? 1U : 0U;
            if (!row_append_ordered(&rows[family], raw)) {
                status = fail_report(
                    report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
                goto cleanup;
            }
        }
        if (selected_count > 1U ||
            (!axis->has_default && selected_count != 1U)) {
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
            goto cleanup;
        }
    }

    if (classification->has_builtin_domain) {
        for (size_t keyword = 0;
             keyword < classification->builtin_domain.keyword_count;
             ++keyword) {
            const char* name =
                classification->builtin_domain.keyword_names[keyword];
            const int raw = classified_keyword_index(
                shader, classification,
                UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN,
                name, strlen(name));
            if (raw < 0 || !state_contains(state, (uint16_t)raw)) continue;
            accounted[raw] = 1U;
            if (!row_append_canonical_set(&rows[2], (uint16_t)raw)) {
                status = fail_report(
                    report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
                goto cleanup;
            }
        }
    }
    for (size_t keyword = 0; keyword < state->keyword_count; ++keyword) {
        const uint16_t raw = state->keyword_indices[keyword];
        if ((size_t)raw >= classification->keyword_count ||
            !accounted[raw] || classification->families[raw] < 0) {
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
            goto cleanup;
        }
    }

cleanup:
    free(accounted);
    return status;
}

static bool token_is_default(const char* token, size_t length) {
    if (length == 0U) return false;
    for (size_t i = 0; i < length; ++i) {
        if (token[i] != '_') return false;
    }
    return true;
}

static UnityGeneratedDomainStatus parse_contract_row(
    const SerializedShader* shader,
    const StageKeywordClassification* classification,
    UnityGeneratedDomainKeywordFamily family, const char* combination,
    OrderedKeywordRow* row, UnityGeneratedDomainReport* report) {
    const char* cursor = combination;
    bool saw_default = false;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor == '\0') break;
        const char* begin = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) ++cursor;
        const size_t length = (size_t)(cursor - begin);
        if (token_is_default(begin, length)) {
            if (saw_default || row->count != 0U) {
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MALFORMED);
            }
            saw_default = true;
            row->has_default = true;
            continue;
        }
        const int raw = classified_keyword_index(
            shader, classification, family, begin, length);
        if (raw == -2) {
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS);
        }
        if (raw < 0 ||
            classification->families[raw] != (int8_t)family) {
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_KEYWORD_SCOPE_MISMATCH);
        }
        for (size_t i = 0; i < row->count; ++i) {
            if (row->indices[i] == (uint16_t)raw) {
                return fail_report(
                    report, UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MALFORMED);
            }
        }
        const bool appended =
            family == UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN
                ? row_append_canonical_set(row, (uint16_t)raw)
                : row_append_ordered(row, (uint16_t)raw);
        if (!appended) {
            return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
        }
    }
    /* Built-in compiler callback rows encode the empty/default variant as an
     * empty string.  Source pragma axes may encode the same semantic cell as
     * one or more underscores.  Both spellings are exact protocol forms of a
     * default row; retaining a false distinction here made the complete
     * shadow-collector domain appear to be missing row zero. */
    if (row->count == 0U && !saw_default) row->has_default = true;
    return UNITY_GENERATED_DOMAIN_OK;
}

static UnityGeneratedDomainStatus compare_contract_family(
    const SerializedShader* shader,
    const StageKeywordClassification* classification,
    UnityGeneratedDomainKeywordFamily family,
    const ExpectedKeywordFamily* expected,
    const SnippetKeywordVariantSet* actual,
    UnityGeneratedDomainReport* report) {
    report->diagnostic.keyword_family = family;
    if (!actual->present) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_MISSING_PROGRAM_CONTRACT);
    }
    if (!expected->active) {
        if (expected->count != 1U || expected->rows[0].count != 0U) {
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
        }
        if (actual->combination_count != 0) {
            report->diagnostic.contract_row_index = 0U;
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_CONTRACT_ROW_EXTRA);
        }
        return UNITY_GENERATED_DOMAIN_OK;
    }

    const size_t actual_count = (size_t)actual->combination_count;
    OrderedKeywordRow* parsed = actual_count > 0U
        ? (OrderedKeywordRow*)calloc(actual_count, sizeof(*parsed))
        : NULL;
    if (actual_count > 0U && !parsed) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }
    UnityGeneratedDomainStatus status = UNITY_GENERATED_DOMAIN_OK;
    for (size_t row = 0; row < actual_count; ++row) {
        report->diagnostic.contract_row_index = row;
        status = parse_contract_row(
            shader, classification, family, actual->combinations[row],
            &parsed[row], report);
        if (status != UNITY_GENERATED_DOMAIN_OK) goto cleanup;
    }
    for (size_t row = 0; row < actual_count; ++row) {
        for (size_t earlier = 0; earlier < row; ++earlier) {
            if (ordered_rows_equal(&parsed[row], &parsed[earlier])) {
                report->diagnostic.contract_row_index = row;
                status = fail_report(
                    report, UNITY_GENERATED_DOMAIN_CONTRACT_ROW_DUPLICATE);
                goto cleanup;
            }
        }
    }
    for (size_t expected_row = 0; expected_row < expected->count;
         ++expected_row) {
        size_t matches = 0U;
        for (size_t row = 0; row < actual_count; ++row) {
            if (ordered_rows_equal(&expected->rows[expected_row],
                                   &parsed[row])) {
                ++matches;
            }
        }
        if (matches == 0U) {
            report->diagnostic.contract_row_index = expected_row;
            if (family != UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN) {
                for (size_t row = 0U; row < actual_count; ++row) {
                    if (rows_have_same_members(
                            &expected->rows[expected_row], &parsed[row])) {
                        status = fail_report(
                            report,
                            UNITY_GENERATED_DOMAIN_CONTRACT_ORDER_MISMATCH);
                        goto cleanup;
                    }
                }
            }
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MISSING);
            goto cleanup;
        }
        if (matches != 1U) {
            report->diagnostic.contract_row_index = expected_row;
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_CONTRACT_ROW_DUPLICATE);
            goto cleanup;
        }
    }
    for (size_t row = 0; row < actual_count; ++row) {
        size_t matches = 0U;
        for (size_t expected_row = 0; expected_row < expected->count;
             ++expected_row) {
            if (ordered_rows_equal(&parsed[row],
                                   &expected->rows[expected_row])) {
                ++matches;
            }
        }
        if (matches == 0U) {
            report->diagnostic.contract_row_index = row;
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_CONTRACT_ROW_EXTRA);
            goto cleanup;
        }
    }
    if (actual_count != expected->count) {
        status = fail_report(
            report, actual_count < expected->count
                ? UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MISSING
                : UNITY_GENERATED_DOMAIN_CONTRACT_ROW_EXTRA);
        goto cleanup;
    }
    for (size_t row = 0; row < actual_count; ++row) {
        if (!ordered_rows_equal(&parsed[row], &expected->rows[row])) {
            report->diagnostic.contract_row_index = row;
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_CONTRACT_ORDER_MISMATCH);
            goto cleanup;
        }
    }

cleanup:
    for (size_t row = 0; row < actual_count; ++row) {
        free_keyword_row(&parsed[row]);
    }
    free(parsed);
    return status;
}

static bool variant_matches_state(
    const ShaderLabPlannedVariant* variant,
    const ShaderLabVariantState* state) {
    return variant && state && variant->keyword_count == state->keyword_count &&
           (state->keyword_count == 0U ||
            memcmp(variant->keyword_indices, state->keyword_indices,
                   state->keyword_count * sizeof(*state->keyword_indices)) ==
                0);
}

static bool identity_contains_raw(
    const int* indices, int count, uint16_t raw) {
    if (count < 0 || (count > 0 && !indices)) return false;
    for (int i = 0; i < count; ++i) {
        if (indices[i] == (int)raw) return true;
    }
    return false;
}

static bool identity_matches_variant(
    const SerializedSubProgramIdentity* identity,
    const ShaderLabPlannedVariant* variant) {
    if (!identity || !variant || identity->global_keyword_index_count < 0 ||
        identity->local_keyword_index_count < 0 ||
        (identity->global_keyword_index_count > 0 &&
         !identity->global_keyword_indices) ||
        (identity->local_keyword_index_count > 0 &&
         !identity->local_keyword_indices)) {
        return false;
    }
    for (size_t keyword = 0; keyword < variant->keyword_count; ++keyword) {
        const uint16_t raw = variant->keyword_indices[keyword];
        if (!identity_contains_raw(
                identity->global_keyword_indices,
                identity->global_keyword_index_count, raw) &&
            !identity_contains_raw(
                identity->local_keyword_indices,
                identity->local_keyword_index_count, raw)) {
            return false;
        }
    }
    const int* arrays[2] = {
        identity->global_keyword_indices,
        identity->local_keyword_indices};
    const int counts[2] = {
        identity->global_keyword_index_count,
        identity->local_keyword_index_count};
    for (size_t scope = 0; scope < 2U; ++scope) {
        for (int keyword = 0; keyword < counts[scope]; ++keyword) {
            const int raw = arrays[scope][keyword];
            if (raw < 0 || raw > UINT16_MAX ||
                !state_contains(
                    &(ShaderLabVariantState){
                        variant->keyword_indices, variant->keyword_count},
                    (uint16_t)raw)) {
                return false;
            }
        }
    }
    return true;
}

static int subprogram_for_state_and_tier(
    const ShaderLabPassStageVariantPlan* stage, size_t state_index,
    int hardware_tier_group) {
    size_t ordinal = 0U;
    for (size_t i = 0; i < stage->variant_count; ++i) {
        if (stage->variants[i].hardware_tier_group != hardware_tier_group) {
            continue;
        }
        if (ordinal++ == state_index) return stage->variants[i].subprogram_index;
    }
    return -1;
}

static UnityGeneratedDomainStatus attest_stage(
    const SerializedShader* shader, const SerializedPass* pass,
    const ShaderLabVariantPlan* plan, const SnippetCompileContract* contract,
    int stage_index, UnityGeneratedDomainReport* report) {
    const ShaderLabPassStageVariantPlan* stage = &plan->stages[stage_index];
    int32_t compiler_program = -1;
    report->diagnostic.stage_index = stage_index;
    if (!unity_serialized_stage_to_compiler_program(
            stage_index, &compiler_program)) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_UNSUPPORTED_STAGE);
    }
    report->diagnostic.compiler_program = compiler_program;
    const SnippetProgramKeywordVariants* program =
        unity_compiler_snippet_contract_find_program_variants(
            contract, compiler_program);
    if (!program || !unity_compiler_snippet_contract_has_variant_families(
                        contract, compiler_program)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_MISSING_PROGRAM_CONTRACT);
    }
    if (compiler_program < 0 || compiler_program >= 32 ||
        (contract->program_types_mask &
         (UINT32_C(1) << (uint32_t)compiler_program)) == 0U) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_MISSING_PROGRAM_CONTRACT);
    }
    uint32_t serialized_stage_bit = 0U;
    if (!shader_stage_serialized_program_mask_bit(
            (UnitySerializedProgramStage)stage_index,
            &serialized_stage_bit) ||
        (pass->program_mask & serialized_stage_bit) == 0U) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_PLAN_AUTHORITY_MISMATCH);
    }
    const bool symbolic = stage->generated_domain_is_symbolic_boolean;
    if (!stage->ordered_states || !stage->variants ||
        stage->state_count == 0U || stage->generated_state_count == 0U ||
        (symbolic
             ? (stage->generated_states || stage->generated_aliases ||
                plan->has_builtin)
             : (!stage->generated_states || !stage->generated_aliases))) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
    }

    StageKeywordClassification classification;
    UnityGeneratedDomainStatus status = classify_stage_keywords(
        shader, plan, stage_index, &classification, report);
    if (status != UNITY_GENERATED_DOMAIN_OK) {
        free_classification(&classification);
        return status;
    }
    ExpectedKeywordFamily expected[3] = {{0}};
    ExpectedAxisVisitorContext expected_context = {
        .families = expected,
        .report = report,
        .status = UNITY_GENERATED_DOMAIN_OK,
    };
    if (!shaderlab_variant_plan_visit_emitted_stage_axes(
            plan, (size_t)stage_index, append_expected_emitted_axis,
            &expected_context)) {
        status = expected_context.status != UNITY_GENERATED_DOMAIN_OK
            ? expected_context.status
            : fail_report(
                  report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
        goto cleanup;
    }
    expected[2].active = classification.has_builtin_domain;
    if (classification.has_builtin_domain) {
        status = append_builtin_contract_rows(
            shader, plan, &classification, &expected[2], report);
        if (status != UNITY_GENERATED_DOMAIN_OK) goto cleanup;
    }
    for (size_t family = 0; family < 3U; ++family) {
        if (expected[family].active) continue;
        const OrderedKeywordRow empty = {0};
        if (!append_unique_row(&expected[family], &empty)) {
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
            goto cleanup;
        }
    }

    if (symbolic) {
        uint8_t* covered = shader->keyword_names.count > 0
            ? (uint8_t*)calloc((size_t)shader->keyword_names.count, 1U)
            : NULL;
        if (shader->keyword_names.count > 0 && !covered) {
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
            goto cleanup;
        }
        size_t symbolic_product = 1U;
        for (size_t axis = 0; axis < stage->axis_count; ++axis) {
            const ShaderLabVariantAxis* candidate = &stage->axes[axis];
            if (!candidate->has_default || candidate->keyword_count != 1U ||
                !candidate->keyword_indices ||
                (size_t)candidate->keyword_indices[0] >=
                    (size_t)shader->keyword_names.count ||
                covered[candidate->keyword_indices[0]] ||
                symbolic_product > SIZE_MAX / 2U) {
                free(covered);
                status = fail_report(
                    report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
                goto cleanup;
            }
            covered[candidate->keyword_indices[0]] = 1U;
            symbolic_product *= 2U;
        }
        if (symbolic_product != stage->generated_state_count) {
            free(covered);
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_CARDINALITY_MISMATCH);
            goto cleanup;
        }
        for (size_t state_index = 0; state_index < stage->state_count;
             ++state_index) {
            const ShaderLabVariantState* state =
                &stage->ordered_states[state_index];
            if (!shaderlab_variant_state_is_canonical(state)) {
                free(covered);
                status = fail_report(
                    report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
                goto cleanup;
            }
            for (size_t keyword = 0; keyword < state->keyword_count;
                 ++keyword) {
                if ((size_t)state->keyword_indices[keyword] >=
                        (size_t)shader->keyword_names.count ||
                    !covered[state->keyword_indices[keyword]]) {
                    free(covered);
                    status = fail_report(
                        report,
                        UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
                    goto cleanup;
                }
            }
            for (size_t earlier = 0; earlier < state_index; ++earlier) {
                if (state->keyword_count ==
                        stage->ordered_states[earlier].keyword_count &&
                    (state->keyword_count == 0U ||
                     memcmp(state->keyword_indices,
                            stage->ordered_states[earlier].keyword_indices,
                            state->keyword_count *
                                sizeof(*state->keyword_indices)) == 0)) {
                    free(covered);
                    status = fail_report(
                        report, UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS);
                    goto cleanup;
                }
            }
        }
        free(covered);
    } else {
        for (size_t state_index = 0;
             state_index < stage->generated_state_count; ++state_index) {
            report->diagnostic.generated_state_index = state_index;
            OrderedKeywordRow projection[3] = {{0}};
            status = project_generated_state(
                shader, plan, stage_index, &classification,
                &stage->generated_states[state_index], projection, report);
            for (size_t family = 0; family < 3U; ++family)
                free_keyword_row(&projection[family]);
            if (status != UNITY_GENERATED_DOMAIN_OK) goto cleanup;
            if (stage->generated_aliases[state_index] >= stage->state_count) {
                report->diagnostic.aliased_state_index =
                    stage->generated_aliases[state_index];
                status = fail_report(
                    report, UNITY_GENERATED_DOMAIN_ALIAS_OUT_OF_RANGE);
                goto cleanup;
            }
            for (size_t earlier = 0; earlier < state_index; ++earlier) {
                const ShaderLabVariantState* left =
                    &stage->generated_states[earlier];
                const ShaderLabVariantState* right =
                    &stage->generated_states[state_index];
                if (left->keyword_count == right->keyword_count &&
                    (left->keyword_count == 0U ||
                     memcmp(left->keyword_indices, right->keyword_indices,
                            left->keyword_count *
                                sizeof(*left->keyword_indices)) == 0)) {
                    status = fail_report(
                        report, UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS);
                    goto cleanup;
                }
            }
        }
    }

    status = compare_contract_family(
        shader, &classification,
        UNITY_GENERATED_DOMAIN_FAMILY_USER_GLOBAL, &expected[0],
        &program->user_global, report);
    if (status != UNITY_GENERATED_DOMAIN_OK) goto cleanup;
    status = compare_contract_family(
        shader, &classification,
        UNITY_GENERATED_DOMAIN_FAMILY_USER_LOCAL, &expected[1],
        &program->user_local, report);
    if (status != UNITY_GENERATED_DOMAIN_OK) goto cleanup;
    status = compare_contract_family(
        shader, &classification, UNITY_GENERATED_DOMAIN_FAMILY_BUILTIN,
        &expected[2], &program->builtin, report);
    if (status != UNITY_GENERATED_DOMAIN_OK) goto cleanup;

    size_t product = 1U;
    for (size_t axis = 0; axis < stage->axis_count; ++axis) {
        const ShaderLabVariantAxis* candidate = &stage->axes[axis];
        const size_t radix = candidate->keyword_count +
                             (candidate->has_default ? 1U : 0U);
        if (radix == 0U || radix > SIZE_MAX / product) {
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_CARDINALITY_MISMATCH);
            goto cleanup;
        }
        product *= radix;
    }
    if (classification.has_builtin_domain) {
        if (expected[2].count == 0U ||
            expected[2].count > SIZE_MAX / product) {
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_CARDINALITY_MISMATCH);
            goto cleanup;
        }
        product *= expected[2].count;
    }
    if (product != stage->generated_state_count) {
        status = fail_report(
            report, UNITY_GENERATED_DOMAIN_CARDINALITY_MISMATCH);
        goto cleanup;
    }

    const int tier_begin = stage->uses_specific_hardware_tiers ? 0 : 3;
    const int tier_end = stage->uses_specific_hardware_tiers ? 3 : 4;
    for (size_t variant_index = 0; variant_index < stage->variant_count;
         ++variant_index) {
        const int tier = stage->variants[variant_index].hardware_tier_group;
        if (tier < tier_begin || tier >= tier_end) {
            report->diagnostic.hardware_tier_group = tier;
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_TIER_DOMAIN_MISMATCH);
            goto cleanup;
        }
    }
    for (int tier = tier_begin; tier < tier_end; ++tier) {
        size_t tier_count = 0U;
        for (size_t variant_index = 0; variant_index < stage->variant_count;
             ++variant_index) {
            const ShaderLabPlannedVariant* variant =
                &stage->variants[variant_index];
            if (variant->hardware_tier_group != tier) continue;
            if (tier_count >= stage->state_count ||
                !variant_matches_state(
                    variant, &stage->ordered_states[tier_count]) ||
                variant->subprogram_index < 0 ||
                variant->subprogram_index >= pass->subprogram_count[stage_index] ||
                !pass->subprogram_identities[stage_index] ||
                pass->subprogram_identities[stage_index]
                        [variant->subprogram_index]
                            .hardware_tier_group != tier ||
                !identity_matches_variant(
                    &pass->subprogram_identities[stage_index]
                         [variant->subprogram_index],
                    variant) ||
                !serialized_pass_subprogram_is_platform(
                    pass, stage_index, variant->subprogram_index,
                    UNITY_D3D11_COMPILER_PLATFORM)) {
                report->diagnostic.hardware_tier_group = tier;
                report->diagnostic.subprogram_index = variant->subprogram_index;
                status = fail_report(
                    report, UNITY_GENERATED_DOMAIN_TIER_DOMAIN_MISMATCH);
                goto cleanup;
            }
            ++tier_count;
        }
        if (tier_count != stage->state_count) {
            report->diagnostic.hardware_tier_group = tier;
            status = fail_report(
                report, UNITY_GENERATED_DOMAIN_TIER_DOMAIN_MISMATCH);
            goto cleanup;
        }
    }

    report->generated_state_count += stage->generated_state_count;
    const size_t compile_state_count =
        symbolic ? stage->state_count : stage->generated_state_count;
    if (compile_state_count >
        SIZE_MAX / (size_t)(tier_end - tier_begin) ||
        report->planned_compile_count >
            SIZE_MAX - compile_state_count *
                           (size_t)(tier_end - tier_begin)) {
        status = fail_report(
            report, UNITY_GENERATED_DOMAIN_CARDINALITY_MISMATCH);
        goto cleanup;
    }
    report->planned_compile_count +=
        compile_state_count * (size_t)(tier_end - tier_begin);
    ++report->attested_stage_count;

cleanup:
    for (size_t family = 0; family < 3U; ++family) {
        free_expected_family(&expected[family]);
    }
    free_classification(&classification);
    return status;
}

static bool pass_has_d3d11_stage(
    const SerializedPass* pass, int stage_index) {
    if (!pass || stage_index < 0 ||
        stage_index >= UNITY_SERIALIZED_STAGE_COUNT_WITH_RAYTRACE ||
        pass->subprogram_count[stage_index] < 0) {
        return false;
    }
    for (int subprogram = 0;
         subprogram < pass->subprogram_count[stage_index]; ++subprogram) {
        if (serialized_pass_subprogram_is_platform(
                pass, stage_index, subprogram,
                UNITY_D3D11_COMPILER_PLATFORM)) {
            return true;
        }
    }
    return false;
}

UnityGeneratedDomainStatus unity_generated_domain_attest_contract(
    const SerializedShader* shader, const SerializedPass* pass,
    const ShaderLabVariantPlan* plan,
    const SnippetCompileContract* contract,
    UnityGeneratedDomainReport* report) {
    if (!report) return UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT;
    unity_generated_domain_report_free(report);
    if (!shader || !pass || !plan || !contract || plan->shader != shader ||
        plan->pass != pass || shader->keyword_names.count < 0 ||
        (shader->keyword_names.count > 0 &&
         (!shader->keyword_names.keywords || !shader->keyword_flags))) {
        return fail_report(
            report, plan && (plan->shader != shader || plan->pass != pass)
                ? UNITY_GENERATED_DOMAIN_PLAN_AUTHORITY_MISMATCH
                : UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT);
    }
    if (!unity_compiler_snippet_contract_validate(contract)) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_INVALID_CONTRACT);
    }
    if (pass_has_d3d11_stage(pass, 5)) {
        report->diagnostic.stage_index = 5;
        return fail_report(report, UNITY_GENERATED_DOMAIN_UNSUPPORTED_STAGE);
    }
    for (int stage_index = 0; stage_index < UNITY_GENERATED_STAGE_COUNT;
         ++stage_index) {
        const bool has_d3d11 = pass_has_d3d11_stage(pass, stage_index);
        const bool active = plan->stages[stage_index].active;
        if (has_d3d11 != active) {
            report->diagnostic.stage_index = stage_index;
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_PLAN_AUTHORITY_MISMATCH);
        }
        if (active) ++report->active_stage_count;
    }
    if (report->active_stage_count == 0U) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_PLAN_AUTHORITY_MISMATCH);
    }
    for (int stage_index = 0; stage_index < UNITY_GENERATED_STAGE_COUNT;
         ++stage_index) {
        if (!plan->stages[stage_index].active) continue;
        const UnityGeneratedDomainStatus status = attest_stage(
            shader, pass, plan, contract, stage_index, report);
        if (status != UNITY_GENERATED_DOMAIN_OK) return status;
    }
    report->diagnostic.keyword_family =
        UNITY_GENERATED_DOMAIN_FAMILY_NONE;
    report->diagnostic.contract_row_index = SIZE_MAX;
    report->diagnostic.generated_state_index = SIZE_MAX;
    report->status = UNITY_GENERATED_DOMAIN_OK;
    report->diagnostic.status = UNITY_GENERATED_DOMAIN_OK;
    return UNITY_GENERATED_DOMAIN_OK;
}

static bool broker_compile_callback(
    void* context, const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* response) {
    return unity_compiler_broker_compile_contract_response(
        (UnityCompilerBroker*)context, request, response);
}

static unsigned int bit_count_u64(uint64_t value) {
    unsigned int count = 0U;
    while (value != 0U) {
        count += (unsigned int)(value & UINT64_C(1));
        value >>= 1U;
    }
    return count;
}

static UnityGeneratedDomainStatus build_scoped_indices(
    const SerializedShader* shader, const ShaderLabVariantState* state,
    int** out_global, size_t* out_global_count,
    int** out_local, size_t* out_local_count,
    UnityGeneratedDomainReport* report) {
    *out_global = NULL;
    *out_global_count = 0U;
    *out_local = NULL;
    *out_local_count = 0U;
    if (!shaderlab_variant_state_is_canonical(state)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
    }
    for (size_t i = 0; i < state->keyword_count; ++i) {
        const uint16_t raw = state->keyword_indices[i];
        if ((size_t)raw >= (size_t)shader->keyword_names.count) {
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE);
        }
        if ((shader->keyword_flags[raw] & UINT8_C(1)) != 0U) {
            ++*out_local_count;
        } else {
            ++*out_global_count;
        }
    }
    if (*out_global_count > 0U) {
        *out_global = (int*)malloc(*out_global_count * sizeof(**out_global));
        if (!*out_global) {
            return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
        }
    }
    if (*out_local_count > 0U) {
        *out_local = (int*)malloc(*out_local_count * sizeof(**out_local));
        if (!*out_local) {
            free(*out_global);
            *out_global = NULL;
            *out_global_count = 0U;
            return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
        }
    }
    size_t global = 0U;
    size_t local = 0U;
    for (size_t i = 0; i < state->keyword_count; ++i) {
        const int raw = (int)state->keyword_indices[i];
        if ((shader->keyword_flags[raw] & UINT8_C(1)) != 0U) {
            (*out_local)[local++] = raw;
        } else {
            (*out_global)[global++] = raw;
        }
    }
    return UNITY_GENERATED_DOMAIN_OK;
}

static bool exact_container_view(
    const uint8_t* bytes, size_t size, DXBCContainerView* view) {
    if (!bytes || !view || !dxbc_container_view_first(bytes, size, view)) {
        return false;
    }
    const size_t prefix = (size_t)(view->data - bytes);
    return prefix <= size && view->size == size - prefix;
}

static bool reference_stage_matches(
    const SerializedPass* pass, int stage_index,
    const PlayerSubProgramMetadata* player,
    const DXBCContainerView* container) {
    DXBCDocument document;
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContract contract;
    DXBCStageContractDiagnostic contract_diagnostic;
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    bool valid = dxbc_document_parse(
                     &document, container->data, container->size,
                     &document_diagnostic) &&
        dxbc_stage_contract_decode_document(
            &document, &contract, &contract_diagnostic);
    if (valid) {
        UnityCompilerProgramStage compiler_program;
        ShaderStageTuple tuple;
        memset(&tuple, 0, sizeof(tuple));
        valid = shader_stage_serialized_to_compiler(
            (UnitySerializedProgramStage)stage_index, &compiler_program);
        if (valid) {
            tuple.serialized_stage =
                (UnitySerializedProgramStage)stage_index;
            tuple.compiler_program = compiler_program;
            tuple.serialized_program_mask = pass->program_mask;
            tuple.gpu_program_type =
                (UnityGPUProgramType)player->program_type;
            tuple.dxbc_program_type = contract.program_type;
            tuple.shader_model_major = contract.shader_model_major;
            tuple.shader_model_minor = contract.shader_model_minor;
            valid = shader_stage_validate_d3d11_tuple(&tuple) ==
                SHADER_STAGE_TUPLE_OK;
        }
    }
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return valid;
}

static UnityGeneratedDomainStatus load_reference_container(
    const UnityGeneratedDomainCertificationInput* input, int stage_index,
    int subprogram_index, PlayerSubProgramMetadata* player,
    DXBCContainerView* container, UnityGeneratedDomainReport* report) {
    const SerializedPass* pass = input->pass;
    report->diagnostic.subprogram_index = subprogram_index;
    if (subprogram_index < 0 ||
        subprogram_index >= pass->subprogram_count[stage_index]) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_SUBPROGRAM_OUT_OF_RANGE);
    }
    if (!serialized_pass_subprogram_is_platform(
            pass, stage_index, subprogram_index,
            UNITY_D3D11_COMPILER_PLATFORM)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_SUBPROGRAM_NOT_D3D11);
    }
    const SerializedSubProgram* serialized =
        &pass->subprograms[stage_index][subprogram_index];
    const uint8_t* payload = NULL;
    size_t payload_size = 0U;
    if (serialized->blob_index < 0 ||
        !shader_blob_archive_get(input->d3d11_archive,
                                 serialized->blob_index, &payload,
                                 &payload_size)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_ARCHIVE_BLOB_MISSING);
    }
    ByteStream stream;
    stream_init(&stream, payload, payload_size);
    stream_set_endian(&stream, false);
    if (!subprogram_metadata_parse_variant(&stream, player)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_PLAYER_BLOB_INVALID);
    }
    if (!player->has_player_blob_header ||
        player->program_type != serialized->program_type ||
        !subprogram_metadata_local_keyword_set_matches(player, serialized)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_PLAYER_METADATA_MISMATCH);
    }
    if (!exact_container_view(
            player->bytecode, player->bytecode_length, container)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_REFERENCE_DXBC_INVALID);
    }
    if (!reference_stage_matches(
            pass, stage_index, player, container)) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_PLAYER_METADATA_MISMATCH);
    }
    return UNITY_GENERATED_DOMAIN_OK;
}

static UnityGeneratedDomainStatus certify_variant_reflection(
    const UnityGeneratedDomainCertificationInput* input, int stage_index,
    int subprogram_index, const PlayerSubProgramMetadata* player,
    const UnityCompilerBinaryResponse* response,
    UnityGeneratedDomainCompilerResponseRecord* response_record,
    UnityGeneratedDomainReport* report) {
    if (!input || !input->pass || !input->d3d11_archive || !player ||
        !response || !report || stage_index < 0 || stage_index >= 6 ||
        subprogram_index < 0 ||
        subprogram_index >= input->pass->subprogram_count[stage_index]) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID);
    }

    SerializedProgramParameters binary_parameters;
    serialized_program_parameters_init(&binary_parameters);
    const SerializedProgramParameters* common =
        &input->pass->common_parameters[stage_index];
    const SerializedProgramParameters* residual = NULL;
    int parameter_blob_index = -1;
    if (input->pass->subprogram_param_blob_indices[stage_index]) {
        parameter_blob_index =
            input->pass->subprogram_param_blob_indices[stage_index]
                                                        [subprogram_index];
    }
    if (parameter_blob_index < -1) {
        serialized_program_parameters_free(&binary_parameters);
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID);
    }
    if (parameter_blob_index >= 0) {
        const uint8_t* payload = NULL;
        size_t payload_size = 0U;
        if (!shader_blob_archive_get(input->d3d11_archive,
                                     parameter_blob_index, &payload,
                                     &payload_size)) {
            serialized_program_parameters_free(&binary_parameters);
            return fail_report(
                report,
                UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID);
        }
        ByteStream stream;
        stream_init(&stream, payload, payload_size);
        stream_set_endian(&stream, false);
        if (!subprogram_metadata_parse_parameters(
                &stream, &binary_parameters)) {
            serialized_program_parameters_free(&binary_parameters);
            return fail_report(
                report,
                UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID);
        }
        residual = &binary_parameters;
    }

    UnityReflectionCertificateReport certificate;
    const UnityReflectionCertificateStatus certificate_status =
        unity_reflection_certify_d3d11_bindings(
            player, common, residual, response->reflection_records,
            response->reflection_record_count, &certificate);
    serialized_program_parameters_free(&binary_parameters);
    report->diagnostic.reflection_certificate = certificate;
    if (response_record) {
        response_record->reflection_certificate_present = true;
        response_record->reflection_certificate = certificate;
    }
    if (certificate_status == UNITY_REFLECTION_CERTIFICATE_OK) {
        ++report->runtime_binding_attested_compile_count;
        return UNITY_GENERATED_DOMAIN_OK;
    }
    if (certificate_status == UNITY_REFLECTION_CERTIFICATE_COMPATIBLE) {
        ++report->runtime_binding_compatible_compile_count;
        return UNITY_GENERATED_DOMAIN_OK;
    }
    if (certificate_status ==
        UNITY_REFLECTION_CERTIFICATE_OUT_OF_MEMORY) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }
    if (certificate_status ==
            UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT ||
        certificate_status ==
            UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA) {
        return fail_report(
            report, UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID);
    }
    return fail_report(
        report, UNITY_GENERATED_DOMAIN_REFLECTION_MISMATCH);
}

static UnityGeneratedDomainStatus retain_compiler_status(
    UnityGeneratedDomainReport* report,
    const UnityCompilerResponseStatus* status) {
    if (!unity_compiler_response_status_copy(
            &report->diagnostic.compiler_response, status)) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }
    return UNITY_GENERATED_DOMAIN_OK;
}

static UnityGeneratedDomainStatus append_compiler_response(
    UnityGeneratedDomainReport* report, int stage_index, int tier,
    size_t generated_state, size_t aliased_state, int subprogram_index,
    const UnityCompilerResponseStatus* response, bool force_record,
    UnityGeneratedDomainCompilerResponseRecord** out_record) {
    if (out_record) *out_record = NULL;
    if (!force_record && response->diagnostic_count == 0U) {
        return UNITY_GENERATED_DOMAIN_OK;
    }
    if (report->compiler_response_count ==
        SIZE_MAX / sizeof(*report->compiler_responses)) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }
    const size_t next_count = report->compiler_response_count + 1U;
    UnityGeneratedDomainCompilerResponseRecord* records =
        (UnityGeneratedDomainCompilerResponseRecord*)realloc(
            report->compiler_responses,
            next_count * sizeof(*records));
    if (!records) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }
    report->compiler_responses = records;
    UnityGeneratedDomainCompilerResponseRecord* record =
        &records[report->compiler_response_count];
    memset(record, 0, sizeof(*record));
    record->stage_index = stage_index;
    record->hardware_tier_group = tier;
    record->generated_state_index = generated_state;
    record->aliased_state_index = aliased_state;
    record->subprogram_index = subprogram_index;
    unity_compiler_response_status_init(&record->response);
    unity_compiler_response_status_init(&record->original_response);
    dxbc_compare_result_init(&record->original_dxbc_compare);
    if (!unity_compiler_response_status_copy(
            &record->response, response)) {
        return fail_report(report, UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
    }
    report->compiler_response_count = next_count;
    if (out_record) *out_record = record;
    return UNITY_GENERATED_DOMAIN_OK;
}

static void record_compile_provenance(
    UnityGeneratedCompileProvenance *provenance,
    const UnityCompilerSnippetCompileRequest *request,
    const UnityCompilerBinaryResponse *response, bool received,
    const DXBCContainerView *reference) {
    memset(provenance, 0, sizeof(*provenance));
    provenance->recorded = true;
    provenance->response_received = received;
    provenance->has_request_identity = response->has_request_identity;
    if (response->has_request_identity) {
        memcpy(provenance->request_digest, response->request_digest,
               sizeof(provenance->request_digest));
        memcpy(provenance->controls_digest, response->controls_digest,
               sizeof(provenance->controls_digest));
    }
    common_sha256(request->snippet_source, strlen(request->snippet_source),
                  provenance->source_digest);
    common_sha256(reference->data, reference->size, provenance->target_digest);
    DXBCContainerView output;
    if (received && exact_container_view(response->data, response->size, &output)) {
        common_sha256(output.data, output.size, provenance->output_digest);
        provenance->has_output_digest = true;
    }
}

UnityGeneratedDomainStatus unity_generated_domain_certify_d3d11(
    const UnityGeneratedDomainCertificationInput* input,
    UnityGeneratedDomainReport* report) {
    if (!report) return UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT;
    const bool has_any_original_source = input &&
        (input->original_snippet || input->original_source_directory ||
         input->original_source_basename);
    const bool has_complete_original_source = input &&
        input->original_snippet && input->original_snippet->has_contract &&
        input->original_snippet->source &&
        input->original_source_directory &&
        input->original_source_directory[0] &&
        input->original_source_basename &&
        input->original_source_basename[0];
    if (!input || !input->shader || !input->pass || !input->plan ||
        !input->generated_snippet || !input->generated_snippet->has_contract ||
        !input->generated_snippet->source || !input->d3d11_archive ||
        !input->compile_profile || !input->source_directory ||
        !input->source_directory[0] || !input->source_basename ||
        !input->source_basename[0] || !input->pass_name ||
        (has_any_original_source && !has_complete_original_source) ||
        (!input->compile_callback && !input->broker)) {
        unity_generated_domain_report_free(report);
        return fail_report(report, UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT);
    }
    if (!unity_compile_profile_validate(input->compile_profile)) {
        unity_generated_domain_report_free(report);
        return fail_report(report, UNITY_GENERATED_DOMAIN_INVALID_PROFILE);
    }
    const uint64_t tier_mask = UINT64_C(7)
        << UNITY_HARDWARE_TIER_FIRST_CAPABILITY;
    if (bit_count_u64(input->compile_profile->d3d11_capabilities &
                      tier_mask) > 1U) {
        unity_generated_domain_report_free(report);
        return fail_report(report, UNITY_GENERATED_DOMAIN_INVALID_PROFILE);
    }

    UnityGeneratedDomainStatus status =
        unity_generated_domain_attest_contract(
            input->shader, input->pass, input->plan,
            &input->generated_snippet->contract, report);
    if (status != UNITY_GENERATED_DOMAIN_OK) return status;

    UnityGeneratedDomainCompileCallback compile = input->compile_callback
        ? input->compile_callback
        : broker_compile_callback;
    void* compile_context = input->compile_callback
        ? input->compile_context
        : input->broker;
    for (int stage_index = 0; stage_index < UNITY_GENERATED_STAGE_COUNT;
         ++stage_index) {
        const ShaderLabPassStageVariantPlan* stage =
            &input->plan->stages[stage_index];
        if (!stage->active) continue;
        int32_t compiler_program = -1;
        if (!unity_serialized_stage_to_compiler_program(
                stage_index, &compiler_program)) {
            report->diagnostic.stage_index = stage_index;
            return fail_report(
                report, UNITY_GENERATED_DOMAIN_UNSUPPORTED_STAGE);
        }
        const int tier_begin = stage->uses_specific_hardware_tiers ? 0 : 3;
        const int tier_end = stage->uses_specific_hardware_tiers ? 3 : 4;
        const bool symbolic =
            stage->generated_domain_is_symbolic_boolean;
        const size_t certification_state_count =
            symbolic ? stage->state_count : stage->generated_state_count;
        for (int tier = tier_begin; tier < tier_end; ++tier) {
            for (size_t generated_state = 0;
                 generated_state < certification_state_count;
                 ++generated_state) {
                report->diagnostic.stage_index = stage_index;
                report->diagnostic.compiler_program = compiler_program;
                report->diagnostic.hardware_tier_group = tier;
                report->diagnostic.generated_state_index = generated_state;
                /* The symbolic Boolean proof makes each distinct serialized
                 * winner reachable at its exact keyword set.  Compile those
                 * representatives instead of the intractable 2^N rows; the
                 * score inequalities in emitted source establish all other
                 * requests and first-tie regions algebraically. */
                const size_t alias = symbolic
                    ? generated_state
                    : stage->generated_aliases[generated_state];
                const ShaderLabVariantState* request_state = symbolic
                    ? &stage->ordered_states[generated_state]
                    : &stage->generated_states[generated_state];
                report->diagnostic.aliased_state_index = alias;
                const int subprogram_index = subprogram_for_state_and_tier(
                    stage, alias, tier);
                report->diagnostic.subprogram_index = subprogram_index;

                PlayerSubProgramMetadata player;
                DXBCContainerView reference;
                memset(&player, 0, sizeof(player));
                memset(&reference, 0, sizeof(reference));
                status = load_reference_container(
                    input, stage_index, subprogram_index, &player,
                    &reference, report);
                if (status != UNITY_GENERATED_DOMAIN_OK) {
                    subprogram_metadata_free_variant(&player);
                    return status;
                }

                int* global_indices = NULL;
                int* local_indices = NULL;
                size_t global_count = 0U;
                size_t local_count = 0U;
                status = build_scoped_indices(
                    input->shader,
                    request_state,
                    &global_indices, &global_count, &local_indices,
                    &local_count, report);
                if (status != UNITY_GENERATED_DOMAIN_OK) {
                    subprogram_metadata_free_variant(&player);
                    return status;
                }

                uint64_t capability_bits =
                    input->compile_profile->d3d11_capabilities;
                if (stage->uses_specific_hardware_tiers) {
                    capability_bits &= ~tier_mask;
                    capability_bits |= UINT64_C(1)
                        << (UNITY_HARDWARE_TIER_FIRST_CAPABILITY + tier);
                }
                UnityCompileAuthority authority;
                unity_compile_authority_init(&authority);
                UnityCompileAuthorityInput authority_input = {
                    .contract = &input->generated_snippet->contract,
                    .keyword_names = (const char* const*)
                        input->shader->keyword_names.keywords,
                    .keyword_name_count =
                        (size_t)input->shader->keyword_names.count,
                    .global_keyword_indices = global_indices,
                    .global_keyword_index_count = global_count,
                    .local_keyword_indices = local_indices,
                    .local_keyword_index_count = local_count,
                    .compiler_program = compiler_program,
                    .pass_type = input->pass->pass_type,
                    .platform_capabilities = {
                        .present = true,
                        .bits = capability_bits,
                    },
                };
                const UnityCompileAuthorityStatus authority_status =
                    unity_compile_authority_build(
                        &authority_input, &authority);
                free(global_indices);
                free(local_indices);
                if (authority_status != UNITY_COMPILE_AUTHORITY_OK) {
                    report->diagnostic.compile_authority_status =
                        authority_status;
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    return fail_report(
                        report,
                        UNITY_GENERATED_DOMAIN_COMPILE_AUTHORITY_FAILED);
                }
                const SerializedSubProgram* serialized =
                    &input->pass->subprograms[stage_index][subprogram_index];
                if (authority.requirements !=
                    serialized->shader_requirements) {
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    return fail_report(
                        report, UNITY_GENERATED_DOMAIN_REQUIREMENTS_MISMATCH);
                }

                UnityCompilerSnippetCompileRequest request = {
                    .snippet_source = input->generated_snippet->source,
                    .source_directory = input->source_directory,
                    .source_basename = input->source_basename,
                    .pass_name = input->pass_name,
                    .caching_preprocessor = true,
                    .preprocess_only = false,
                    .strip_line_directives = false,
                    .build_platform = input->compile_profile->build_platform,
                    .render_state_length = 0,
                    .variant_keywords = authority.platform_keywords,
                    .variant_keyword_count =
                        authority.platform_keyword_count,
                    .user_keywords = authority.user_keywords,
                    .user_keyword_count = authority.user_keyword_count,
                    .disabled_keywords = authority.disabled_keywords,
                    .disabled_keyword_count =
                        authority.disabled_keyword_count,
                    .compiler_flags = authority.compiler_flags,
                    .shader_type = compiler_program,
                    .platform = UNITY_D3D11_COMPILER_PLATFORM,
                    .requirements = authority.requirements,
                    .program_mask = (int32_t)input->pass->program_mask,
                    .program_start =
                        input->generated_snippet->contract.start_line,
                    .contract = &input->generated_snippet->contract,
                };
                UnityCompilerBinaryResponse response;
                unity_compiler_binary_response_init(&response);
                ++report->compile_attempt_count;
                const bool received = compile(
                    compile_context, &request, &response);
                UnityGeneratedDomainCompilerResponseRecord *response_record = NULL;
                if (input->retain_compile_provenance) {
                    status = append_compiler_response(
                        report, stage_index, tier, generated_state, alias,
                        subprogram_index, &response.status, true, &response_record);
                    if (status != UNITY_GENERATED_DOMAIN_OK) {
                        unity_compiler_binary_response_free(&response);
                        unity_compile_authority_free(&authority);
                        subprogram_metadata_free_variant(&player);
                        return status;
                    }
                    record_compile_provenance(&response_record->provenance,
                                              &request, &response, received, &reference);
                }
                if (!received) {
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    return fail_report(
                        report,
                        UNITY_GENERATED_DOMAIN_COMPILER_TRANSPORT_FAILED);
                }
                if (response.status.availability != UNITY_COMPILER_RESPONSE_AVAILABLE) {
                    const UnityGeneratedDomainStatus unavailable =
                        response.status.availability == UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS
                            ? UNITY_GENERATED_DOMAIN_COMPILER_CACHE_ONLY_MISS
                            : UNITY_GENERATED_DOMAIN_INCLUDE_AUTHORITY_UNAVAILABLE;
                    status = retain_compiler_status(
                        report, &response.status);
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    if (status != UNITY_GENERATED_DOMAIN_OK) return status;
                    return fail_report(report, unavailable);
                }
                report->compiler_diagnostic_count +=
                    response.status.diagnostic_count;
                if (!response_record) {
                    status = append_compiler_response(
                        report, stage_index, tier, generated_state, alias,
                        subprogram_index, &response.status, false, &response_record);
                }
                if (status != UNITY_GENERATED_DOMAIN_OK) {
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    return status;
                }
                const size_t actionable_diagnostic_count =
                    unity_compiler_response_status_actionable_diagnostic_count(
                        &response.status);
                if (!response.status.compiler_success) {
                    status = retain_compiler_status(
                        report, &response.status);
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    if (status != UNITY_GENERATED_DOMAIN_OK) return status;
                    return fail_report(
                        report, UNITY_GENERATED_DOMAIN_COMPILER_REJECTED);
                }
                DXBCContainerView compiled;
                if (!exact_container_view(
                        response.data, response.size, &compiled)) {
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    return fail_report(
                        report,
                        UNITY_GENERATED_DOMAIN_COMPILED_DXBC_INVALID);
                }
                const DXBCCompareStatus compare_status = dxbc_compare_exact(
                    reference.data, reference.size, compiled.data,
                    compiled.size, &report->diagnostic.dxbc_compare);
                if (compare_status != DXBC_COMPARE_EQUAL) {
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    return fail_report(
                        report, UNITY_GENERATED_DOMAIN_DXBC_MISMATCH);
                }

                status = certify_variant_reflection(
                    input, stage_index, subprogram_index, &player,
                    &response, response_record, report);
                if (status != UNITY_GENERATED_DOMAIN_OK) {
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    return status;
                }

                if (actionable_diagnostic_count != 0U) {
                    status = retain_compiler_status(
                        report, &response.status);
                    if (status != UNITY_GENERATED_DOMAIN_OK) {
                        unity_compiler_binary_response_free(&response);
                        unity_compile_authority_free(&authority);
                        subprogram_metadata_free_variant(&player);
                        return status;
                    }
                    if (!response_record || !input->original_snippet ||
                        !input->original_snippet->has_contract ||
                        !input->original_snippet->source ||
                        !input->original_source_directory ||
                        !input->original_source_directory[0] ||
                        !input->original_source_basename ||
                        !input->original_source_basename[0]) {
                        if (response_record) {
                            response_record->diagnostic_parity_status =
                                UNITY_GENERATED_DIAGNOSTIC_PARITY_SOURCE_UNAVAILABLE;
                        }
                        unity_compiler_binary_response_free(&response);
                        unity_compile_authority_free(&authority);
                        subprogram_metadata_free_variant(&player);
                        return fail_report(
                            report,
                            UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC);
                    }

                    UnityCompilerSnippetCompileRequest original_request =
                        request;
                    original_request.snippet_source =
                        input->original_snippet->source;
                    original_request.source_directory =
                        input->original_source_directory;
                    original_request.source_basename =
                        input->original_source_basename;
                    original_request.program_start =
                        input->original_snippet->contract.start_line;
                    original_request.contract =
                        &input->original_snippet->contract;
                    UnityCompilerBinaryResponse original_response;
                    unity_compiler_binary_response_init(&original_response);
                    ++report->diagnostic_attestation_compile_count;
                    const bool original_received = compile(
                        compile_context, &original_request,
                        &original_response);
                    if (input->retain_compile_provenance && response_record)
                        record_compile_provenance(&response_record->original_provenance,
                                                  &original_request, &original_response,
                                                  original_received, &reference);
                    if (!original_received) {
                        response_record->diagnostic_parity_status =
                            UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_TRANSPORT_FAILED;
                    } else {
                        response_record->original_response_present = true;
                        if (!unity_compiler_response_status_copy(
                                &response_record->original_response,
                                &original_response.status)) {
                            unity_compiler_binary_response_free(
                                &original_response);
                            unity_compiler_binary_response_free(&response);
                            unity_compile_authority_free(&authority);
                            subprogram_metadata_free_variant(&player);
                            return fail_report(
                                report,
                                UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY);
                        }
                        report->compiler_diagnostic_count +=
                            original_response.status.diagnostic_count;
                        if (!original_response.status.compiler_success) {
                            response_record->diagnostic_parity_status =
                                UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_REJECTED;
                        } else {
                            DXBCContainerView original_compiled;
                            if (!exact_container_view(
                                    original_response.data,
                                    original_response.size,
                                    &original_compiled)) {
                                response_record->diagnostic_parity_status =
                                    UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_DXBC_INVALID;
                            } else if (dxbc_compare_exact(
                                           reference.data, reference.size,
                                           original_compiled.data,
                                           original_compiled.size,
                                           &response_record
                                                ->original_dxbc_compare) !=
                                       DXBC_COMPARE_EQUAL) {
                                response_record->diagnostic_parity_status =
                                    UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_DXBC_MISMATCH;
                            } else if (!unity_compiler_reflection_records_equal(
                                           response.reflection_records,
                                           response.reflection_record_count,
                                           original_response
                                               .reflection_records,
                                           original_response
                                               .reflection_record_count)) {
                                response_record->diagnostic_parity_status =
                                    UNITY_GENERATED_DIAGNOSTIC_PARITY_REFLECTION_MISMATCH;
                            } else if (!unity_generated_domain_diagnostics_match_normalized(
                                           &response.status,
                                           &original_response.status)) {
                                response_record->diagnostic_parity_status =
                                    UNITY_GENERATED_DIAGNOSTIC_PARITY_DIAGNOSTICS_MISMATCH;
                            } else {
                                response_record->diagnostic_parity_status =
                                    UNITY_GENERATED_DIAGNOSTIC_PARITY_ATTESTED;
                                ++report->diagnostic_attested_compile_count;
                                report->diagnostic_attested_actionable_count +=
                                    actionable_diagnostic_count;
                            }
                        }
                    }
                    const bool diagnostic_attested = original_received &&
                        response_record->diagnostic_parity_status ==
                            UNITY_GENERATED_DIAGNOSTIC_PARITY_ATTESTED;
                    unity_compiler_binary_response_free(&original_response);
                    unity_compiler_binary_response_free(&response);
                    unity_compile_authority_free(&authority);
                    subprogram_metadata_free_variant(&player);
                    if (!diagnostic_attested) {
                        return fail_report(
                            report,
                            UNITY_GENERATED_DOMAIN_DIAGNOSTIC_ATTESTATION_FAILED);
                    }
                    ++report->matched_dxbc_count;
                    continue;
                }

                ++report->clean_compile_count;
                unity_compiler_binary_response_free(&response);
                unity_compile_authority_free(&authority);
                subprogram_metadata_free_variant(&player);
                ++report->matched_dxbc_count;
            }
        }
    }
    report->diagnostic.stage_index = -1;
    report->diagnostic.compiler_program = -1;
    report->diagnostic.hardware_tier_group = -1;
    report->diagnostic.generated_state_index = SIZE_MAX;
    report->diagnostic.aliased_state_index = SIZE_MAX;
    report->diagnostic.subprogram_index = -1;
    report->status = UNITY_GENERATED_DOMAIN_OK;
    report->diagnostic.status = UNITY_GENERATED_DOMAIN_OK;
    return UNITY_GENERATED_DOMAIN_OK;
}

const char* unity_generated_domain_status_name(
    UnityGeneratedDomainStatus status) {
    switch (status) {
        case UNITY_GENERATED_DOMAIN_OK: return "ok";
        case UNITY_GENERATED_DOMAIN_INVALID_ARGUMENT: return "invalid-argument";
        case UNITY_GENERATED_DOMAIN_INVALID_PROFILE: return "invalid-profile";
        case UNITY_GENERATED_DOMAIN_PLAN_AUTHORITY_MISMATCH:
            return "plan-authority-mismatch";
        case UNITY_GENERATED_DOMAIN_UNSUPPORTED_STAGE:
            return "unsupported-stage";
        case UNITY_GENERATED_DOMAIN_INVALID_CONTRACT:
            return "invalid-contract";
        case UNITY_GENERATED_DOMAIN_MISSING_PROGRAM_CONTRACT:
            return "missing-program-contract";
        case UNITY_GENERATED_DOMAIN_INVALID_GENERATED_STATE:
            return "invalid-generated-state";
        case UNITY_GENERATED_DOMAIN_KEYWORD_AMBIGUOUS:
            return "keyword-ambiguous";
        case UNITY_GENERATED_DOMAIN_KEYWORD_SCOPE_MISMATCH:
            return "keyword-scope-mismatch";
        case UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MALFORMED:
            return "contract-row-malformed";
        case UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MISSING:
            return "contract-row-missing";
        case UNITY_GENERATED_DOMAIN_CONTRACT_ROW_DUPLICATE:
            return "contract-row-duplicate";
        case UNITY_GENERATED_DOMAIN_CONTRACT_ROW_EXTRA:
            return "contract-row-extra";
        case UNITY_GENERATED_DOMAIN_CONTRACT_ORDER_MISMATCH:
            return "contract-order-mismatch";
        case UNITY_GENERATED_DOMAIN_CARDINALITY_MISMATCH:
            return "cardinality-mismatch";
        case UNITY_GENERATED_DOMAIN_ALIAS_OUT_OF_RANGE:
            return "alias-out-of-range";
        case UNITY_GENERATED_DOMAIN_TIER_DOMAIN_MISMATCH:
            return "tier-domain-mismatch";
        case UNITY_GENERATED_DOMAIN_SUBPROGRAM_OUT_OF_RANGE:
            return "subprogram-out-of-range";
        case UNITY_GENERATED_DOMAIN_SUBPROGRAM_NOT_D3D11:
            return "subprogram-not-d3d11";
        case UNITY_GENERATED_DOMAIN_ARCHIVE_BLOB_MISSING:
            return "archive-blob-missing";
        case UNITY_GENERATED_DOMAIN_PLAYER_BLOB_INVALID:
            return "player-blob-invalid";
        case UNITY_GENERATED_DOMAIN_PLAYER_METADATA_MISMATCH:
            return "player-metadata-mismatch";
        case UNITY_GENERATED_DOMAIN_REFERENCE_DXBC_INVALID:
            return "reference-dxbc-invalid";
        case UNITY_GENERATED_DOMAIN_COMPILE_AUTHORITY_FAILED:
            return "compile-authority-failed";
        case UNITY_GENERATED_DOMAIN_REQUIREMENTS_MISMATCH:
            return "requirements-mismatch";
        case UNITY_GENERATED_DOMAIN_INCLUDE_AUTHORITY_UNAVAILABLE:
            return "include-authority-unavailable";
        case UNITY_GENERATED_DOMAIN_COMPILER_CACHE_ONLY_MISS:
            return "compiler-cache-only-miss";
        case UNITY_GENERATED_DOMAIN_COMPILER_TRANSPORT_FAILED:
            return "compiler-transport-failed";
        case UNITY_GENERATED_DOMAIN_COMPILER_REJECTED:
            return "compiler-rejected";
        case UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC:
            return "compiler-diagnostic";
        case UNITY_GENERATED_DOMAIN_DIAGNOSTIC_ATTESTATION_FAILED:
            return "diagnostic-attestation-failed";
        case UNITY_GENERATED_DOMAIN_COMPILED_DXBC_INVALID:
            return "compiled-dxbc-invalid";
        case UNITY_GENERATED_DOMAIN_DXBC_MISMATCH: return "dxbc-mismatch";
        case UNITY_GENERATED_DOMAIN_REFLECTION_METADATA_INVALID:
            return "reflection-metadata-invalid";
        case UNITY_GENERATED_DOMAIN_REFLECTION_MISMATCH:
            return "reflection-mismatch";
        case UNITY_GENERATED_DOMAIN_OUT_OF_MEMORY: return "out-of-memory";
        default: return "unknown";
    }
}

const char* unity_generated_diagnostic_parity_status_name(
    UnityGeneratedDiagnosticParityStatus status) {
    switch (status) {
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_NOT_APPLICABLE:
            return "not-applicable";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_ATTESTED:
            return "attested";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_SOURCE_UNAVAILABLE:
            return "source-unavailable";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_TRANSPORT_FAILED:
            return "original-transport-failed";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_REJECTED:
            return "original-rejected";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_DXBC_INVALID:
            return "original-dxbc-invalid";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_ORIGINAL_DXBC_MISMATCH:
            return "original-dxbc-mismatch";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_REFLECTION_MISMATCH:
            return "reflection-mismatch";
        case UNITY_GENERATED_DIAGNOSTIC_PARITY_DIAGNOSTICS_MISMATCH:
            return "diagnostics-mismatch";
        default:
            return "unknown";
    }
}

const char* unity_generated_glsl_status_name(UnityGeneratedGLSLStatus status) {
    switch (status) {
        case UNITY_GENERATED_GLSL_UNAVAILABLE_NO_PRECISION_AUTHORITY:
            return "unavailable-no-gl-source-or-precision-authority";
        case UNITY_GENERATED_GLSL_AUTHORITY_AVAILABLE:
            return "authority-available";
        default: return "unknown";
    }
}
