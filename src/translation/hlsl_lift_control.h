// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_LIFT_CONTROL_H
#define HLSL_LIFT_CONTROL_H

#include "translation/hlsl_lift_transaction.h"

/* Shared admission/acceptance deadline for instruction and whole-ShaderLab
 * lifts. Terminal outcomes are sticky; callers can stop work on other fatal
 * statuses or request-count exhaustion through status. I/O services retain
 * their own transport deadlines and cancellation contracts. */
typedef struct {
    bool (*monotonic_ms)(void *context, uint64_t *milliseconds);
    bool (*cancelled)(void *context);
    void *context;
    uint64_t started_ms;
    uint64_t elapsed_ms;
    uint64_t max_elapsed_ms;
    HLSLLiftStatus status;
} HLSLLiftControl;

HLSLLiftStatus hlsl_lift_control_begin(HLSLLiftControl *control,
                                       bool (*monotonic_ms)(void *, uint64_t *),
                                       bool (*cancelled)(void *), void *context,
                                       uint64_t max_elapsed_ms);
HLSLLiftStatus hlsl_lift_control_check(HLSLLiftControl *control);

#endif
