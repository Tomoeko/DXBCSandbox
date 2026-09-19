// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_lift_control.h"

#include <string.h>

HLSLLiftStatus hlsl_lift_control_begin(HLSLLiftControl *control,
                                       bool (*monotonic_ms)(void *, uint64_t *),
                                       bool (*cancelled)(void *), void *context,
                                       uint64_t max_elapsed_ms) {
    if (!control)
        return HLSL_LIFT_INVALID_ARGUMENT;
    memset(control, 0, sizeof(*control));
    control->monotonic_ms = monotonic_ms;
    control->cancelled = cancelled;
    control->context = context;
    control->max_elapsed_ms = max_elapsed_ms;
    control->status = HLSL_LIFT_INVALID_ARGUMENT;
    if (monotonic_ms)
        control->status = monotonic_ms(context, &control->started_ms) ? HLSL_LIFT_VERIFIED
                                                                      : HLSL_LIFT_CLOCK_UNAVAILABLE;
    return control->status;
}

HLSLLiftStatus hlsl_lift_control_check(HLSLLiftControl *control) {
    if (!control)
        return HLSL_LIFT_INVALID_ARGUMENT;
    if (control->status != HLSL_LIFT_VERIFIED)
        return control->status;
    if (control->cancelled && control->cancelled(control->context)) {
        control->status = HLSL_LIFT_CANCELLED;
        return control->status;
    }
    uint64_t now;
    if (!control->monotonic_ms(control->context, &now) || now < control->started_ms ||
        now - control->started_ms < control->elapsed_ms) {
        control->status = HLSL_LIFT_CLOCK_UNAVAILABLE;
    } else {
        control->elapsed_ms = now - control->started_ms;
        if (control->elapsed_ms >= control->max_elapsed_ms)
            control->status = HLSL_LIFT_BUDGET_EXHAUSTED;
    }
    return control->status;
}
