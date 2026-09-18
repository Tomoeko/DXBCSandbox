#include "app/verification_scope.h"

#include <stdio.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                    __LINE__, #condition);                                   \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(void) {
    const VerificationScopeInput selected_with_unrelated_compute = {
        .selection_filter_active = true,
        .selected_checks_exact = true,
        .selected_shader_objects = 1U,
        .unsupported_shader_objects = 1U,
    };
    VerificationScopeResult result = verification_scope_evaluate(
        &selected_with_unrelated_compute);
    CHECK(result.selected_scope_exact);
    CHECK(!result.whole_input_object_kind_coverage_complete);
    CHECK(result.requested_scope_passed);

    const VerificationScopeInput whole_input_with_compute = {
        .selection_filter_active = false,
        .selected_checks_exact = true,
        .selected_shader_objects = 1U,
        .unsupported_shader_objects = 1U,
    };
    result = verification_scope_evaluate(&whole_input_with_compute);
    CHECK(result.selected_scope_exact);
    CHECK(!result.whole_input_object_kind_coverage_complete);
    CHECK(!result.requested_scope_passed);

    const VerificationScopeInput no_selected_shader = {
        .selection_filter_active = true,
        .selected_checks_exact = true,
        .selected_shader_objects = 0U,
        .unsupported_shader_objects = 0U,
    };
    result = verification_scope_evaluate(&no_selected_shader);
    CHECK(!result.selected_scope_exact);
    CHECK(result.whole_input_object_kind_coverage_complete);
    CHECK(!result.requested_scope_passed);

    CHECK(!verification_scope_evaluate(NULL).requested_scope_passed);
    puts("verification scope unit tests passed");
    return 0;
}
