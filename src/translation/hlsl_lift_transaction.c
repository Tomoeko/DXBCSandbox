// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_lift_transaction.h"
#include "translation/hlsl_emitter.h"
#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

typedef struct AcceptedCopy {
    HLSLCopyLift *copy;
    struct AcceptedCopy *previous;
} AcceptedCopy;

struct HLSLLiftTransaction {
    const USILProgram *program;
    uint8_t *target;
    size_t target_size;
    HLSLLiftServices services;
    HLSLLiftLimits limits;
    HLSLLiftStats stats;
    HLSLLiftArtifact accepted;
    AcceptedCopy *copies;
    uint64_t started_ms;
    HLSLLiftStatus stopped;
    bool high_level;
};

static void artifact_free(HLSLLiftArtifact *artifact) {
    free(artifact->source);
    free(artifact->dxbc);
    free(artifact->expression_source_map);
    memset(artifact, 0, sizeof(*artifact));
}

static void result_init(HLSLLiftResult *result) {
    memset(result, 0, sizeof(*result));
    result->status = HLSL_LIFT_INVALID_ARGUMENT;
    result->precondition = HLSL_COPY_LIFT_INVALID_PROGRAM;
    dxbc_compare_result_init(&result->comparison);
}

static void hash_hex(const void *bytes, size_t size, char output[65]) {
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, output);
}

static HLSLLiftStatus work_status(HLSLLiftTransaction *transaction) {
    if (transaction->stopped != HLSL_LIFT_VERIFIED)
        return transaction->stopped;
    if (transaction->services.cancelled &&
        transaction->services.cancelled(transaction->services.context)) {
        transaction->stopped = HLSL_LIFT_CANCELLED;
        return transaction->stopped;
    }
    uint64_t now;
    if (!transaction->services.monotonic_ms(transaction->services.context, &now) ||
        now < transaction->started_ms ||
        now - transaction->started_ms < transaction->stats.elapsed_ms) {
        transaction->stopped = HLSL_LIFT_CLOCK_UNAVAILABLE;
    } else {
        transaction->stats.elapsed_ms = now - transaction->started_ms;
        if (transaction->stats.elapsed_ms >= transaction->limits.max_elapsed_ms)
            transaction->stopped = HLSL_LIFT_BUDGET_EXHAUSTED;
    }
    return transaction->stopped;
}

static bool artifact_request_identity_valid(const HLSLLiftArtifact *artifact) {
    uint8_t request_bits = 0, controls_bits = 0;
    for (size_t index = 0; index < sizeof(artifact->request_digest); ++index) {
        request_bits |= artifact->request_digest[index];
        controls_bits |= artifact->controls_digest[index];
    }
    return artifact->has_request_identity && request_bits && controls_bits;
}

static HLSLLiftStatus verify_program(HLSLLiftTransaction *transaction, const USILProgram *program,
                                     HLSLLiftArtifact *artifact, HLSLLiftResult *result,
                                     bool high_level) {
    HLSLLiftStatus status = work_status(transaction);
    if (status != HLSL_LIFT_VERIFIED)
        return status;
    if (transaction->stats.compiles >= transaction->limits.max_compiles) {
        transaction->stopped = HLSL_LIFT_BUDGET_EXHAUSTED;
        return transaction->stopped;
    }
    ++transaction->stats.compiles;
    HLSLLiftStatus (*compile)(void *, const USILProgram *, uint64_t, HLSLLiftArtifact *) =
        high_level ? transaction->services.compile_high_level : transaction->services.compile;
    status = compile(transaction->services.context, program,
                     transaction->limits.max_elapsed_ms - transaction->stats.elapsed_ms, artifact);
    result->cache_hit = artifact->cache_hit;
    if (artifact->cache_hit)
        ++transaction->stats.cache_hits;
    if (artifact->source)
        hash_hex(artifact->source, strlen(artifact->source), result->source_sha256);
    if (artifact->dxbc && artifact->dxbc_size)
        hash_hex(artifact->dxbc, artifact->dxbc_size, result->output_sha256);
    if (artifact->has_request_identity) {
        common_sha256_digest_to_hex(artifact->request_digest, result->request_sha256);
        common_sha256_digest_to_hex(artifact->controls_digest, result->controls_sha256);
    }
    HLSLLiftStatus after = work_status(transaction);
    if (after != HLSL_LIFT_VERIFIED)
        return after;
    switch (status) {
    case HLSL_LIFT_VERIFIED:
        break;
    case HLSL_LIFT_EMISSION_REJECTED:
    case HLSL_LIFT_COMPILER_REJECTED:
    case HLSL_LIFT_COMPILER_UNAVAILABLE:
    case HLSL_LIFT_INVALID_DXBC:
    case HLSL_LIFT_OUT_OF_MEMORY:
        return status;
    case HLSL_LIFT_CANCELLED:
    case HLSL_LIFT_BUDGET_EXHAUSTED:
        transaction->stopped = status;
        return status;
    default:
        return HLSL_LIFT_INVALID_ARGUMENT;
    }
    if (!artifact->source || !artifact->source[0])
        return HLSL_LIFT_EMISSION_REJECTED;
    if (high_level &&
        (transaction->services.require_expression_source_map || artifact->expression_source_map) &&
        !hlsl_expression_source_map_matches(artifact->expression_source_map, program,
                                            artifact->source))
        return HLSL_LIFT_PROVENANCE_MISMATCH;
    if (((transaction->services.require_request_identity || artifact->has_request_identity) &&
         !artifact_request_identity_valid(artifact)) ||
        (transaction->accepted.has_request_identity &&
         (!artifact->has_request_identity ||
          memcmp(transaction->accepted.controls_digest, artifact->controls_digest, 32) != 0)))
        return HLSL_LIFT_AUTHORITY_MISMATCH;
    result->compared = true;
    DXBCCompareStatus comparison =
        dxbc_compare_exact(transaction->target, transaction->target_size, artifact->dxbc,
                           artifact->dxbc_size, &result->comparison);
    after = work_status(transaction);
    if (after != HLSL_LIFT_VERIFIED)
        return after;
    if (comparison == DXBC_COMPARE_EQUAL)
        return HLSL_LIFT_VERIFIED;
    if (comparison == DXBC_COMPARE_EXPECTED_INVALID || comparison == DXBC_COMPARE_ACTUAL_INVALID ||
        comparison == DXBC_COMPARE_INVALID_ARGUMENT)
        return HLSL_LIFT_INVALID_DXBC;
    return HLSL_LIFT_DXBC_MISMATCH;
}

HLSLLiftStatus hlsl_lift_transaction_begin(const USILProgram *baseline, const uint8_t *target,
                                           size_t target_size, const HLSLLiftServices *services,
                                           const HLSLLiftLimits *limits,
                                           HLSLLiftTransaction **out_transaction,
                                           HLSLLiftResult *result) {
    if (out_transaction)
        *out_transaction = NULL;
    if (!result)
        return HLSL_LIFT_INVALID_ARGUMENT;
    result_init(result);
    if (!baseline || !target || !target_size || !services || !services->compile ||
        !services->monotonic_ms || !limits || !out_transaction)
        return result->status;
    /* Reject malformed authority before spending a compiler request. */
    if (dxbc_compare_exact(target, target_size, target, target_size, &result->comparison) !=
        DXBC_COMPARE_EQUAL) {
        result->status = HLSL_LIFT_INVALID_DXBC;
        return result->status;
    }
    HLSLLiftTransaction *transaction = calloc(1u, sizeof(*transaction));
    if (!transaction)
        return result->status = HLSL_LIFT_OUT_OF_MEMORY;
    transaction->program = baseline;
    transaction->target_size = target_size;
    transaction->services = *services;
    transaction->limits = *limits;
    if (!services->monotonic_ms(services->context, &transaction->started_ms)) {
        result->status = HLSL_LIFT_CLOCK_UNAVAILABLE;
    } else {
        transaction->target = malloc(target_size);
        if (!transaction->target) {
            result->status = HLSL_LIFT_OUT_OF_MEMORY;
        } else {
            memcpy(transaction->target, target, target_size);
            result->status =
                verify_program(transaction, baseline, &transaction->accepted, result, false);
        }
    }
    if (result->status == HLSL_LIFT_VERIFIED) {
        *out_transaction = transaction;
    } else {
        hlsl_lift_transaction_destroy(transaction);
    }
    return result->status;
}

static HLSLLiftStatus try_lift(HLSLLiftTransaction *transaction, int instruction,
                               HLSLLiftResult *result, bool forward_result) {
    if (!result)
        return HLSL_LIFT_INVALID_ARGUMENT;
    result_init(result);
    if (!transaction)
        return result->status;
    if (transaction->high_level)
        return result->status = HLSL_LIFT_COMPOSITION_UNSUPPORTED;
    result->status = work_status(transaction);
    if (result->status != HLSL_LIFT_VERIFIED)
        return result->status;
    if (transaction->stats.candidates >= transaction->limits.max_candidates) {
        transaction->stopped = HLSL_LIFT_BUDGET_EXHAUSTED;
        return result->status = transaction->stopped;
    }
    ++transaction->stats.candidates;
    HLSLCopyLift *copy = NULL;
    result->precondition = forward_result
                               ? hlsl_result_lift_create(transaction->program, instruction, &copy)
                               : hlsl_copy_lift_create(transaction->program, instruction, &copy);
    result->status = work_status(transaction);
    if (result->status != HLSL_LIFT_VERIFIED) {
        hlsl_copy_lift_destroy(copy);
        return result->status;
    }
    if (result->precondition != HLSL_COPY_LIFT_OK) {
        return result->status = result->precondition == HLSL_COPY_LIFT_OUT_OF_MEMORY
                                    ? HLSL_LIFT_OUT_OF_MEMORY
                                    : HLSL_LIFT_PRECONDITION_REJECTED;
    }
    /* Reserve ownership before compiling; allocation failure must not discard
     * the previous verified result or leave a partially accepted candidate. */
    AcceptedCopy *node = calloc(1u, sizeof(*node));
    HLSLLiftArtifact artifact = {0};
    if (!node) {
        result->status = HLSL_LIFT_OUT_OF_MEMORY;
    } else {
        result->status =
            verify_program(transaction, hlsl_copy_lift_program(copy), &artifact, result, false);
    }
    if (result->status == HLSL_LIFT_VERIFIED) {
        node->copy = copy;
        node->previous = transaction->copies;
        transaction->copies = node;
        transaction->program = hlsl_copy_lift_program(copy);
        artifact_free(&transaction->accepted);
        transaction->accepted = artifact;
        ++transaction->stats.accepted;
    } else {
        artifact_free(&artifact);
        free(node);
        hlsl_copy_lift_destroy(copy);
    }
    return result->status;
}

HLSLLiftStatus hlsl_lift_transaction_try_copy(HLSLLiftTransaction *transaction, int instruction,
                                              HLSLLiftResult *result) {
    return try_lift(transaction, instruction, result, false);
}

HLSLLiftStatus hlsl_lift_transaction_try_result(HLSLLiftTransaction *transaction, int instruction,
                                                HLSLLiftResult *result) {
    return try_lift(transaction, instruction, result, true);
}

HLSLLiftStatus hlsl_lift_transaction_try_high_level(HLSLLiftTransaction *transaction,
                                                    HLSLLiftResult *result) {
    if (!result)
        return HLSL_LIFT_INVALID_ARGUMENT;
    result_init(result);
    if (!transaction || !transaction->services.compile_high_level)
        return result->status;
    if (transaction->high_level)
        return result->status = HLSL_LIFT_COMPOSITION_UNSUPPORTED;
    result->status = work_status(transaction);
    if (result->status != HLSL_LIFT_VERIFIED)
        return result->status;
    if (transaction->stats.candidates >= transaction->limits.max_candidates) {
        transaction->stopped = HLSL_LIFT_BUDGET_EXHAUSTED;
        return result->status = transaction->stopped;
    }
    ++transaction->stats.candidates;
    HLSLLiftArtifact artifact = {0};
    result->status = verify_program(transaction, transaction->program, &artifact, result, true);
    if (result->status == HLSL_LIFT_VERIFIED) {
        artifact_free(&transaction->accepted);
        transaction->accepted = artifact;
        transaction->high_level = true;
        ++transaction->stats.accepted;
    } else {
        artifact_free(&artifact);
    }
    return result->status;
}

bool hlsl_lift_transaction_is_high_level(const HLSLLiftTransaction *transaction) {
    return transaction && transaction->high_level;
}

const USILProgram *hlsl_lift_transaction_program(const HLSLLiftTransaction *transaction) {
    return transaction ? transaction->program : NULL;
}

const HLSLLiftArtifact *hlsl_lift_transaction_artifact(const HLSLLiftTransaction *transaction) {
    return transaction ? &transaction->accepted : NULL;
}

void hlsl_lift_transaction_stats(const HLSLLiftTransaction *transaction, HLSLLiftStats *stats) {
    if (stats)
        *stats = transaction ? transaction->stats : (HLSLLiftStats){0};
}

void hlsl_lift_transaction_destroy(HLSLLiftTransaction *transaction) {
    if (!transaction)
        return;
    AcceptedCopy *copy = transaction->copies;
    while (copy) {
        AcceptedCopy *previous = copy->previous;
        hlsl_copy_lift_destroy(copy->copy);
        free(copy);
        copy = previous;
    }
    artifact_free(&transaction->accepted);
    free(transaction->target);
    free(transaction);
}

const char *hlsl_lift_status_name(HLSLLiftStatus status) {
    switch (status) {
#define STATUS(value, name)                                                                        \
    case HLSL_LIFT_##value:                                                                        \
        return name
        STATUS(VERIFIED, "verified");
        STATUS(PRECONDITION_REJECTED, "precondition-rejected");
        STATUS(EMISSION_REJECTED, "emission-rejected");
        STATUS(COMPILER_REJECTED, "compiler-rejected");
        STATUS(COMPILER_UNAVAILABLE, "compiler-unavailable");
        STATUS(CANCELLED, "cancelled");
        STATUS(BUDGET_EXHAUSTED, "budget-exhausted");
        STATUS(DXBC_MISMATCH, "dxbc-mismatch");
        STATUS(INVALID_DXBC, "invalid-dxbc");
        STATUS(INVALID_ARGUMENT, "invalid-argument");
        STATUS(OUT_OF_MEMORY, "out-of-memory");
        STATUS(CLOCK_UNAVAILABLE, "clock-unavailable");
        STATUS(COMPOSITION_UNSUPPORTED, "composition-unsupported");
        STATUS(AUTHORITY_MISMATCH, "authority-mismatch");
        STATUS(PROVENANCE_MISMATCH, "provenance-mismatch");
#undef STATUS
    }
    return "unknown";
}
