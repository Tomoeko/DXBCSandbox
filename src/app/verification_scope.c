// SPDX-License-Identifier: GPL-3.0-only

#include "app/verification_scope.h"

VerificationScopeResult verification_scope_evaluate(
    const VerificationScopeInput* input) {
    VerificationScopeResult result = {0};
    if (!input) return result;

    result.selected_scope_exact = input->selected_checks_exact &&
        input->selected_shader_objects != 0U;
    result.whole_input_object_kind_coverage_complete =
        input->unsupported_shader_objects == 0U;
    result.requested_scope_passed = result.selected_scope_exact &&
        (input->selection_filter_active ||
         result.whole_input_object_kind_coverage_complete);
    return result;
}
