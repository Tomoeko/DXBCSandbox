// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPILER_SINGLEFLIGHT_H
#define UNITY_COMPILER_SINGLEFLIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USC_SINGLE_FLIGHT_KEY_SIZE 32u

typedef struct UnityCompilerSingleFlight UnityCompilerSingleFlight;

/* The operation returns malloc-owned bytes on success and may return a
 * malloc-owned error on failure.  Single-flight takes ownership of both and
 * gives every participant its own result/error copy. */
typedef uint8_t* (*UnityCompilerSingleFlightOperation)(
    void* context, size_t* out_size, char** out_error);

UnityCompilerSingleFlight* usc_single_flight_create(void);

/* No execute call may still be active when this is destroyed. */
void usc_single_flight_destroy(UnityCompilerSingleFlight* single_flight);

/* Coalesces only overlapping calls with the same key.  Completed entries are
 * removed as soon as their participants leave, so neither successes nor
 * failures become an implicit cache. */
uint8_t* usc_single_flight_execute(
    UnityCompilerSingleFlight* single_flight,
    const uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE],
    UnityCompilerSingleFlightOperation operation,
    void* operation_context,
    size_t* out_size,
    char** out_error,
    bool* out_joined_existing);

/* Diagnostic/test visibility; returns zero when the key is not in flight. */
size_t usc_single_flight_participant_count(
    UnityCompilerSingleFlight* single_flight,
    const uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE]);

#endif /* UNITY_COMPILER_SINGLEFLIGHT_H */
