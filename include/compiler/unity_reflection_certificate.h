// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_REFLECTION_CERTIFICATE_H
#define UNITY_REFLECTION_CERTIFICATE_H

#include "compiler/unity_compiler_client.h"
#include "common/sha256.h"
#include "io/subprogram_metadata.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A stripped DXBC container does not retain Unity's source identifiers.
 * UnityShaderCompiler reports those identifiers and their runtime layouts on
 * a separate callback channel. Unity deduplicates parameter sets into a
 * common intersection plus an optional per-subprogram residual; neither blob
 * alone is a literal callback transcript. Bytecode equality is therefore
 * checked together with a proof against their reconstructed runtime union.
 * A partial common constant buffer contributes its in-range fields to the
 * selected binary residual's stage-specific shell; the common family shell may
 * legitimately be larger. Unity's distinct loose-parameter area contributes
 * fields to the named `$Globals` shell but is never itself a duplicate shell.
 * The selected shell size, all serialized constants, inputs, and resources are
 * exact. Unity's receiver deliberately discards only the raw constant-buffer
 * callback's variable-count field. A successful proof containing a
 * constant-buffer callback is consequently `COMPATIBLE`; a proof with no
 * discarded callback field is `OK`.
 *
 * Stats records are deliberately outside this certificate: complete DXBC
 * equality already covers executable instructions, while this API proves the
 * independent input/property/resource binding domain.
 */
typedef enum {
    UNITY_REFLECTION_CERTIFICATE_OK = 0,
    UNITY_REFLECTION_CERTIFICATE_COMPATIBLE,
    UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT,
    UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA,
    UNITY_REFLECTION_CERTIFICATE_OUT_OF_MEMORY,
    UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD,
    UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD
} UnityReflectionCertificateStatus;

typedef enum {
    UNITY_REFLECTION_AUTHORITY_COMMON_ONLY = 0,
    UNITY_REFLECTION_AUTHORITY_COMMON_PLUS_RESIDUAL = 1
} UnityReflectionCertificateAuthority;

#define UNITY_REFLECTION_NAME_PREVIEW_SIZE 96U

/* Allocation-free failure evidence. Long identifiers retain a preview, exact
 * byte length, and SHA-256 so diagnostics never silently conflate names. */
typedef struct {
    bool present;
    UnityCompilerReflectionKind kind;
    bool has_name;
    char name_preview[UNITY_REFLECTION_NAME_PREVIEW_SIZE];
    size_t name_length;
    uint8_t name_sha256[COMMON_SHA256_DIGEST_SIZE];
    int32_t values[UNITY_COMPILER_REFLECTION_MAX_VALUES];
    size_t value_count;
} UnityReflectionCertificateRecordSummary;

/* Expected constant-buffer summaries use this value in values[1]. Unity does
 * not retain the corresponding callback field, so it is never compared. */
#define UNITY_REFLECTION_CB_VARIABLE_COUNT_UNAVAILABLE INT32_C(-1)

typedef struct {
    UnityReflectionCertificateStatus status;
    UnityReflectionCertificateAuthority authority;
    size_t expected_record_count;
    /* Valid with expected_bindings_digest_valid, even when callbacks are
     * absent. All non-input rows require runtime parameter/resource authority. */
    size_t expected_non_input_record_count;
    size_t observed_record_count;
    size_t matched_record_count;
    /* Independently hashed binding multisets, including constant-buffer scope.
     * Stats and the discarded CB variable count are excluded. Valid digests
     * can disagree on failure; validity alone is never certification. */
    bool expected_bindings_digest_valid;
    bool observed_bindings_digest_valid;
    uint8_t expected_bindings_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t observed_bindings_digest[COMMON_SHA256_DIGEST_SIZE];
    size_t ignored_stats_record_count;
    /* SIZE_MAX when no record-specific failure is available. */
    size_t expected_record_index;
    size_t observed_record_index;
    UnityReflectionCertificateRecordSummary expected_record;
    UnityReflectionCertificateRecordSummary observed_record;
} UnityReflectionCertificateReport;

void unity_reflection_certificate_report_init(
    UnityReflectionCertificateReport* report);

UnityReflectionCertificateStatus unity_reflection_certify_d3d11_bindings(
    const PlayerSubProgramMetadata* player,
    const SerializedProgramParameters* common_parameters,
    const SerializedProgramParameters* residual_parameters,
    const UnityCompilerReflectionRecord* observed_records,
    size_t observed_record_count,
    UnityReflectionCertificateReport* report);

const char* unity_reflection_certificate_status_name(
    UnityReflectionCertificateStatus status);
const char* unity_compiler_reflection_kind_name(
    UnityCompilerReflectionKind kind);
const char* unity_reflection_certificate_authority_name(
    UnityReflectionCertificateAuthority authority);

#endif /* UNITY_REFLECTION_CERTIFICATE_H */
