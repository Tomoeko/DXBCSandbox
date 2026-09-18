// SPDX-License-Identifier: GPL-3.0-only

#ifndef VERIFICATION_SCOPE_H
#define VERIFICATION_SCOPE_H

#include <stdbool.h>
#include <stddef.h>

/* Verification has two intentionally separate scopes. A tuple/path filter
 * requests evidence for selected ClassID 48 Shader objects. It does not
 * silently claim that every shader-bearing object kind elsewhere in the
 * input was processed. Without a selection filter, whole-input object-kind
 * coverage is part of the requested gate. */
typedef struct {
    bool selection_filter_active;
    bool selected_checks_exact;
    size_t selected_shader_objects;
    size_t unsupported_shader_objects;
} VerificationScopeInput;

typedef struct {
    bool selected_scope_exact;
    bool whole_input_object_kind_coverage_complete;
    bool requested_scope_passed;
} VerificationScopeResult;

VerificationScopeResult verification_scope_evaluate(
    const VerificationScopeInput* input);

#endif /* VERIFICATION_SCOPE_H */
