// SPDX-License-Identifier: GPL-3.0-only

#ifndef RELEASE_SHADER_OBJECT_CERTIFICATE_H
#define RELEASE_SHADER_OBJECT_CERTIFICATE_H

#include "io/shader_blob_archive.h"
#include "io/shader_object.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELEASE_SHADER_REFERENCE_ID_SIZE 32U

/*
 * Canonical comparison of two decoded Unity Release Shader objects.
 *
 * This certificate compares the serialized, runtime-relevant Shader object;
 * it does not compare ShaderLab source spelling.  Unity's compression
 * segmentation, LZ4 bytes, entry offsets, and map insertion order are storage
 * details and are canonicalized away.  Platform numbers, stage counts, entry
 * indices, and every decompressed archive-entry byte remain identity-bearing.
 * Consequently the artifact field covers player wrappers, compiled programs
 * (DXBC/GLSL/etc.), parameter blobs, and any otherwise-unclassified entry.
 *
 * ShaderObject::shader does not project the root m_Dependencies,
 * m_NonModifiableTextures, or m_ShaderIsBaked fields.  This verifier reads and
 * validates those fields directly from ShaderObject::root instead of guessing
 * model values.  A non-null PPtr is not portable across serialized-file
 * namespaces, so it can match only through the caller's stable resolver.
 * Null PPtrs need no resolver.  Resolver failure is UNAVAILABLE, never a raw
 * path-ID fallback.
 *
 * One exact projection gap remains explicit: a nonempty editor-only
 * SerializedProgram.m_SubPrograms array makes PROGRAM_VARIANTS unavailable.
 * Certifying such an input requires extending SerializedShader with the
 * pinned editor-subprogram record shape; opaque tree equality is deliberately
 * not accepted as semantic authority.  Unity Release player objects normally
 * carry their variants in m_PlayerSubPrograms, which is fully compared here.
 *
 * Exact archive equality and this whole-object certificate are not pixel
 * evidence.  The report therefore never certifies source or visual identity.
 */

typedef enum {
    RELEASE_SHADER_SIDE_EXPECTED = 0,
    RELEASE_SHADER_SIDE_ACTUAL = 1
} ReleaseShaderObjectSide;

typedef bool (*ReleaseShaderReferenceResolver)(
    void* user_data, ReleaseShaderObjectSide side, int32_t file_id,
    int64_t path_id,
    uint8_t stable_id[RELEASE_SHADER_REFERENCE_ID_SIZE]);

typedef struct {
    ReleaseShaderReferenceResolver resolve_reference;
    void* reference_user_data;
} ReleaseShaderObjectCertificateOptions;

typedef enum {
    RELEASE_SHADER_FIELD_SCHEMA_PROFILE = 0,
    RELEASE_SHADER_FIELD_SHADER_IDENTITY,
    RELEASE_SHADER_FIELD_PROPERTIES,
    RELEASE_SHADER_FIELD_KEYWORDS,
    RELEASE_SHADER_FIELD_PARSED_DEPENDENCIES,
    RELEASE_SHADER_FIELD_CUSTOM_EDITORS,
    RELEASE_SHADER_FIELD_SUBSHADER_TOPOLOGY,
    RELEASE_SHADER_FIELD_SUBSHADER_METADATA,
    RELEASE_SHADER_FIELD_PASS_IDENTITY,
    RELEASE_SHADER_FIELD_PASS_TAGS,
    RELEASE_SHADER_FIELD_RENDER_STATE,
    RELEASE_SHADER_FIELD_PROGRAM_VARIANTS,
    RELEASE_SHADER_FIELD_PARAMETER_BINDINGS,
    /* Exhaustive canonical comparison of m_ParsedForm.  This is a coverage
     * backstop for fields added to the pinned projection before this report
     * grows a more specific diagnostic category. */
    RELEASE_SHADER_FIELD_PARSED_FORM_RESIDUAL,
    RELEASE_SHADER_FIELD_OBJECT_DEPENDENCY_PTRS,
    RELEASE_SHADER_FIELD_NONMODIFIABLE_TEXTURES,
    RELEASE_SHADER_FIELD_SHADER_IS_BAKED,
    RELEASE_SHADER_FIELD_PLATFORM_ARCHIVES,
    RELEASE_SHADER_FIELD_COMPILED_ARTIFACTS,
    RELEASE_SHADER_FIELD_COUNT
} ReleaseShaderObjectField;

typedef enum {
    RELEASE_SHADER_FIELD_NOT_EVALUATED = 0,
    RELEASE_SHADER_FIELD_MATCH,
    RELEASE_SHADER_FIELD_MISMATCH,
    RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE,
    RELEASE_SHADER_FIELD_AUTHORITY_INVALID
} ReleaseShaderObjectFieldStatus;

typedef enum {
    RELEASE_SHADER_ARCHIVE_EQUAL = 0,
    RELEASE_SHADER_ARCHIVE_INVALID_ARGUMENT,
    RELEASE_SHADER_ARCHIVE_EXPECTED_INVALID,
    RELEASE_SHADER_ARCHIVE_ACTUAL_INVALID,
    RELEASE_SHADER_ARCHIVE_STAGE_COUNT_MISMATCH,
    RELEASE_SHADER_ARCHIVE_ENTRY_COUNT_MISMATCH,
    RELEASE_SHADER_ARCHIVE_ENTRY_SIZE_MISMATCH,
    RELEASE_SHADER_ARCHIVE_ENTRY_PAYLOAD_MISMATCH
} ReleaseShaderArchiveCompareStatus;

typedef struct {
    ReleaseShaderArchiveCompareStatus status;
    int first_differing_entry;
    size_t first_differing_byte;
    size_t expected_entry_count;
    size_t actual_entry_count;
    size_t matched_entry_count;
} ReleaseShaderArchiveCompareReport;

/* Dependency-free lower boundary used by the object certificate.  Archive
 * inputs are already decompressed/validated owners.  Equality is entry-index
 * and byte exact while compression segmentation is intentionally absent. */
ReleaseShaderArchiveCompareStatus
release_shader_archive_compare_canonical(
    const ShaderBlobArchive* expected, const ShaderBlobArchive* actual,
    ReleaseShaderArchiveCompareReport* report);

typedef enum {
    RELEASE_SHADER_OBJECT_CERTIFICATE_OK = 0,
    RELEASE_SHADER_OBJECT_CERTIFICATE_INVALID_ARGUMENT,
    RELEASE_SHADER_OBJECT_CERTIFICATE_INPUT_NOT_DECODED,
    RELEASE_SHADER_OBJECT_CERTIFICATE_SCHEMA_AUTHORITY_INVALID,
    RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_INVALID,
    RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_UNAVAILABLE,
    RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER
} ReleaseShaderObjectCertificateStatus;

typedef struct {
    ReleaseShaderObjectCertificateStatus status;
    ReleaseShaderObjectFieldStatus fields[RELEASE_SHADER_FIELD_COUNT];
    ReleaseShaderObjectField first_problem_field;

    /* -1 means that the coordinate does not apply. */
    int subshader_index;
    int pass_index;
    int stage_index;
    int element_index;
    int platform;
    int archive_entry_index;
    size_t archive_byte_offset;

    size_t evaluated_field_count;
    size_t matched_field_count;
    size_t expected_artifact_count;
    size_t actual_artifact_count;
    size_t matched_artifact_count;

    bool canonical_release_identity_certified;
    bool compiled_artifacts_exact;
    bool source_identity_certified;
    bool visual_output_certified;
} ReleaseShaderObjectCertificateReport;

void release_shader_object_certificate_options_init(
    ReleaseShaderObjectCertificateOptions* options);
void release_shader_object_certificate_report_init(
    ReleaseShaderObjectCertificateReport* report);

ReleaseShaderObjectCertificateStatus release_shader_object_certify_equal(
    const ShaderObject* expected, const ShaderObject* actual,
    const ReleaseShaderObjectCertificateOptions* options,
    ReleaseShaderObjectCertificateReport* report);

const char* release_shader_object_field_name(ReleaseShaderObjectField field);
const char* release_shader_object_field_status_name(
    ReleaseShaderObjectFieldStatus status);
const char* release_shader_archive_compare_status_name(
    ReleaseShaderArchiveCompareStatus status);
const char* release_shader_object_certificate_status_name(
    ReleaseShaderObjectCertificateStatus status);

#endif /* RELEASE_SHADER_OBJECT_CERTIFICATE_H */
