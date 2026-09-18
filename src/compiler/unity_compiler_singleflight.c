// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compiler_singleflight.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct SingleFlightEntry {
    uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE];
    pthread_cond_t completed_condition;
    size_t participant_count;
    bool completed;
    uint8_t* result;
    size_t result_size;
    char* error;
    struct SingleFlightEntry* next;
} SingleFlightEntry;

struct UnityCompilerSingleFlight {
    pthread_mutex_t mutex;
    SingleFlightEntry* entries;
};

static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    const size_t size = strlen(value) + 1u;
    char* copy = (char*)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static SingleFlightEntry* find_entry(
    UnityCompilerSingleFlight* single_flight,
    const uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE]) {
    for (SingleFlightEntry* entry = single_flight->entries; entry;
         entry = entry->next) {
        if (memcmp(entry->key, key, USC_SINGLE_FLIGHT_KEY_SIZE) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void destroy_entry(SingleFlightEntry* entry) {
    if (!entry) return;
    pthread_cond_destroy(&entry->completed_condition);
    free(entry->result);
    free(entry->error);
    free(entry);
}

static void unlink_entry(UnityCompilerSingleFlight* single_flight,
                         SingleFlightEntry* entry) {
    SingleFlightEntry** cursor = &single_flight->entries;
    while (*cursor && *cursor != entry) cursor = &(*cursor)->next;
    if (*cursor == entry) *cursor = entry->next;
}

UnityCompilerSingleFlight* usc_single_flight_create(void) {
    UnityCompilerSingleFlight* single_flight =
        (UnityCompilerSingleFlight*)calloc(1, sizeof(*single_flight));
    if (!single_flight) return NULL;
    if (pthread_mutex_init(&single_flight->mutex, NULL) != 0) {
        free(single_flight);
        return NULL;
    }
    return single_flight;
}

void usc_single_flight_destroy(UnityCompilerSingleFlight* single_flight) {
    if (!single_flight) return;
    SingleFlightEntry* entry = single_flight->entries;
    while (entry) {
        SingleFlightEntry* next = entry->next;
        destroy_entry(entry);
        entry = next;
    }
    pthread_mutex_destroy(&single_flight->mutex);
    free(single_flight);
}

static uint8_t* copy_entry_result(const SingleFlightEntry* entry,
                                  size_t* out_size, char** out_error) {
    *out_size = 0;
    if (out_error) *out_error = NULL;
    if (!entry->result) {
        if (out_error && entry->error) {
            *out_error = duplicate_string(entry->error);
        }
        return NULL;
    }

    const size_t allocation_size = entry->result_size == 0
        ? 1u
        : entry->result_size;
    uint8_t* copy = (uint8_t*)malloc(allocation_size);
    if (!copy) {
        if (out_error) *out_error = duplicate_string("Out of memory");
        return NULL;
    }
    if (entry->result_size > 0) {
        memcpy(copy, entry->result, entry->result_size);
    }
    *out_size = entry->result_size;
    return copy;
}

uint8_t* usc_single_flight_execute(
    UnityCompilerSingleFlight* single_flight,
    const uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE],
    UnityCompilerSingleFlightOperation operation,
    void* operation_context,
    size_t* out_size,
    char** out_error,
    bool* out_joined_existing) {
    if (out_size) *out_size = 0;
    if (out_error) *out_error = NULL;
    if (out_joined_existing) *out_joined_existing = false;
    if (!single_flight || !key || !operation || !out_size) return NULL;

    pthread_mutex_lock(&single_flight->mutex);
    SingleFlightEntry* entry = find_entry(single_flight, key);
    bool owner = entry == NULL;
    if (owner) {
        entry = (SingleFlightEntry*)calloc(1, sizeof(*entry));
        if (!entry || pthread_cond_init(&entry->completed_condition, NULL) != 0) {
            free(entry);
            pthread_mutex_unlock(&single_flight->mutex);
            /* Allocation failure in the coalescing optimization must not turn
             * a valid compiler request into a false compiler failure. */
            return operation(operation_context, out_size, out_error);
        }
        memcpy(entry->key, key, sizeof(entry->key));
        entry->participant_count = 1;
        entry->next = single_flight->entries;
        single_flight->entries = entry;
    } else {
        entry->participant_count++;
        if (out_joined_existing) *out_joined_existing = true;
        while (!entry->completed) {
            pthread_cond_wait(&entry->completed_condition,
                              &single_flight->mutex);
        }
    }
    pthread_mutex_unlock(&single_flight->mutex);

    if (owner) {
        size_t operation_size = 0;
        char* operation_error = NULL;
        uint8_t* operation_result = operation(
            operation_context, &operation_size, &operation_error);

        pthread_mutex_lock(&single_flight->mutex);
        entry->result = operation_result;
        entry->result_size = operation_result ? operation_size : 0;
        entry->error = operation_error;
        entry->completed = true;
        pthread_cond_broadcast(&entry->completed_condition);
    } else {
        pthread_mutex_lock(&single_flight->mutex);
    }

    uint8_t* result = copy_entry_result(entry, out_size, out_error);
    entry->participant_count--;
    const bool last_participant = entry->participant_count == 0;
    if (last_participant) unlink_entry(single_flight, entry);
    pthread_mutex_unlock(&single_flight->mutex);
    if (last_participant) destroy_entry(entry);
    return result;
}

size_t usc_single_flight_participant_count(
    UnityCompilerSingleFlight* single_flight,
    const uint8_t key[USC_SINGLE_FLIGHT_KEY_SIZE]) {
    if (!single_flight || !key) return 0;
    pthread_mutex_lock(&single_flight->mutex);
    SingleFlightEntry* entry = find_entry(single_flight, key);
    const size_t count = entry ? entry->participant_count : 0;
    pthread_mutex_unlock(&single_flight->mutex);
    return count;
}
