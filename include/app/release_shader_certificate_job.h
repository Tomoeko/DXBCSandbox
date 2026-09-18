// SPDX-License-Identifier: GPL-3.0-only

#ifndef RELEASE_SHADER_CERTIFICATE_JOB_H
#define RELEASE_SHADER_CERTIFICATE_JOB_H

#include "app/release_shader_object_certificate.h"
#include "app/shader_catalog.h"
#include "io/typetree_schema_registry.h"

#define RELEASE_SHADER_CERTIFICATE_SHA256_HEX_SIZE 65U

typedef struct {
    size_t line_number;
    char expected_occurrence_id[96];
    char expected_content_id[96];
    char expected_serialized_sha256[
        RELEASE_SHADER_CERTIFICATE_SHA256_HEX_SIZE];
    char actual_occurrence_id[96];
    char actual_content_id[96];
    char actual_serialized_sha256[
        RELEASE_SHADER_CERTIFICATE_SHA256_HEX_SIZE];
} ReleaseShaderCertificatePair;

typedef struct {
    ReleaseShaderCertificatePair* rows;
    size_t count;
} ReleaseShaderCertificatePairTable;

typedef struct {
    ReleaseShaderObjectSide side;
    uint8_t owner_serialized_sha256[COMMON_SHA256_DIGEST_SIZE];
    int32_t file_id;
    int64_t path_id;
    uint8_t stable_id[RELEASE_SHADER_REFERENCE_ID_SIZE];
} ReleaseShaderCertificateReference;

typedef struct {
    ReleaseShaderCertificateReference* rows;
    size_t count;
} ReleaseShaderCertificateReferenceMap;

typedef enum {
    RELEASE_SHADER_CERTIFICATE_TABLE_OK = 0,
    RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ARGUMENT,
    RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_HEADER,
    RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW,
    RELEASE_SHADER_CERTIFICATE_TABLE_DUPLICATE_ROW,
    RELEASE_SHADER_CERTIFICATE_TABLE_ALLOCATION_FAILED,
    RELEASE_SHADER_CERTIFICATE_TABLE_IO_ERROR,
    RELEASE_SHADER_CERTIFICATE_TABLE_LIMIT_EXCEEDED,
} ReleaseShaderCertificateTableStatus;

void release_shader_certificate_pair_table_init(
    ReleaseShaderCertificatePairTable* table);
void release_shader_certificate_pair_table_dispose(
    ReleaseShaderCertificatePairTable* table);
void release_shader_certificate_reference_map_init(
    ReleaseShaderCertificateReferenceMap* map);
void release_shader_certificate_reference_map_dispose(
    ReleaseShaderCertificateReferenceMap* map);

ReleaseShaderCertificateTableStatus release_shader_certificate_pairs_parse(
    const uint8_t* data, size_t size,
    ReleaseShaderCertificatePairTable* table, size_t* error_line);
ReleaseShaderCertificateTableStatus
release_shader_certificate_reference_map_parse(
    const uint8_t* data, size_t size,
    ReleaseShaderCertificateReferenceMap* map, size_t* error_line);
ReleaseShaderCertificateTableStatus release_shader_certificate_pairs_load(
    const char* path, ReleaseShaderCertificatePairTable* table,
    size_t* error_line);
ReleaseShaderCertificateTableStatus
release_shader_certificate_reference_map_load(
    const char* path, ReleaseShaderCertificateReferenceMap* map,
    size_t* error_line);

typedef enum {
    RELEASE_SHADER_CERTIFICATE_PAIR_EXACT = 0,
    RELEASE_SHADER_CERTIFICATE_PAIR_MISMATCH,
    RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_UNAVAILABLE,
    RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_INVALID,
} ReleaseShaderCertificatePairStatus;

typedef enum {
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_NONE = 0,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_RECORD_NOT_FOUND,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_RECORD_NOT_FOUND,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_COORDINATE_MISMATCH,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_COORDINATE_MISMATCH,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_NOT_GRAPHICS,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_NOT_GRAPHICS,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_SOURCE_UNAVAILABLE,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_SOURCE_UNAVAILABLE,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_DECODE_UNAVAILABLE,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_DECODE_UNAVAILABLE,
    RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ALLOCATION_FAILED,
} ReleaseShaderCertificateDiagnostic;

typedef struct {
    char occurrence_id[96];
    char content_id[96];
    char serialized_sha256[RELEASE_SHADER_CERTIFICATE_SHA256_HEX_SIZE];
    int64_t path_id;
    char* name;
    char* outer_path;
    char* member_name;
    size_t member_index;
    bool is_bundle_member;
} ReleaseShaderCertificateEndpoint;

typedef struct {
    size_t line_number;
    ReleaseShaderCertificateEndpoint expected;
    ReleaseShaderCertificateEndpoint actual;
    ReleaseShaderCertificatePairStatus status;
    ReleaseShaderCertificateDiagnostic diagnostic;
    ShaderObjectStatus expected_decode_status;
    ShaderObjectStatus actual_decode_status;
    TypeTreeSchemaStatus expected_schema_status;
    TypeTreeSchemaStatus actual_schema_status;
    ReleaseShaderObjectCertificateReport certificate;
} ReleaseShaderCertificatePairResult;

typedef enum {
    RELEASE_SHADER_CERTIFICATE_COMPARE_EXACT = 0,
    RELEASE_SHADER_CERTIFICATE_COMPARE_MISMATCH,
    RELEASE_SHADER_CERTIFICATE_COMPARE_AUTHORITY_UNAVAILABLE,
    RELEASE_SHADER_CERTIFICATE_COMPARE_INVALID_ARGUMENT,
    RELEASE_SHADER_CERTIFICATE_COMPARE_INPUT_INVALID,
    RELEASE_SHADER_CERTIFICATE_COMPARE_CATALOG_FAILED,
    RELEASE_SHADER_CERTIFICATE_COMPARE_ALLOCATION_FAILED,
} ReleaseShaderCertificateCompareStatus;

typedef struct {
    ReleaseShaderCertificateCompareStatus status;
    ReleaseShaderCertificateDiagnostic diagnostic;
    ShaderCatalogStatus expected_catalog_status;
    ShaderCatalogStatus actual_catalog_status;
    size_t expected_catalog_issues;
    size_t actual_catalog_issues;
    ReleaseShaderCertificatePairResult* pairs;
    size_t pair_count;
    size_t exact_count;
    size_t mismatch_count;
    size_t unavailable_count;
    size_t invalid_count;
} ReleaseShaderCertificateCompareResult;

void release_shader_certificate_compare_result_init(
    ReleaseShaderCertificateCompareResult* result);
void release_shader_certificate_compare_result_dispose(
    ReleaseShaderCertificateCompareResult* result);

/*
 * Compares only the explicitly paired ClassID 48 occurrences. Names are
 * copied for display but never participate in selection. A non-null PPtr is
 * resolved only through references; no raw path-ID equality fallback exists.
 */
ReleaseShaderCertificateCompareStatus release_shader_certificate_compare(
    const char* expected_input, const char* actual_input,
    const ReleaseShaderCertificatePairTable* pairs,
    const ReleaseShaderCertificateReferenceMap* references,
    const TypeTreeSchemaRegistry* schema_registry,
    ReleaseShaderCertificateCompareResult* result);

const char* release_shader_certificate_table_status_name(
    ReleaseShaderCertificateTableStatus status);
const char* release_shader_certificate_pair_status_name(
    ReleaseShaderCertificatePairStatus status);
const char* release_shader_certificate_diagnostic_name(
    ReleaseShaderCertificateDiagnostic diagnostic);
const char* release_shader_certificate_compare_status_name(
    ReleaseShaderCertificateCompareStatus status);

#endif /* RELEASE_SHADER_CERTIFICATE_JOB_H */
