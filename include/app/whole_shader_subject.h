// SPDX-License-Identifier: GPL-3.0-only

#ifndef WHOLE_SHADER_SUBJECT_H
#define WHOLE_SHADER_SUBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WHOLE_SHADER_SUBJECT_FORMAT_VERSION 3U
#define WHOLE_SHADER_SUBJECT_DIGEST_SIZE 32U
#define WHOLE_SHADER_SUBJECT_MAX_STRING_BYTES UINT32_C(1048576)
#define WHOLE_SHADER_SUBJECT_SHADER_CLASS_ID 48

typedef enum {
    WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE = 1,
    WHOLE_SHADER_SOURCE_BUNDLE_MEMBER = 2
} WholeShaderSourceResidency;

/* Immutable content identity shared by every whole-shader evidence plane.
 * Host paths are deliberately absent. A known empty dependency closure is
 * represented by its canonical digest, never by an absent field. */
typedef struct {
    int64_t target_shader_path_id;
    int32_t target_class_id;
    uint32_t serialized_target_platform;
    uint32_t build_platform;
    int32_t compiler_platform;
    uint32_t graphics_api;
    WholeShaderSourceResidency source_residency;
    /* Zero for a standalone SerializedFile; exact node ordinal in a bundle. */
    uint64_t target_member_index;

    /* Empty is canonical for a standalone SerializedFile. */
    const char* target_member_identity;
    const char* candidate_logical_name;
    const char* unity_version;

    /* Catalog occurrence authority. Unlike the serialized-file digest, this
     * distinguishes repeated copies/nodes of identical bytes. It contains no
     * unhashed host path in the canonical subject. */
    uint8_t target_occurrence_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t target_serialized_file_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t target_object_payload_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t candidate_source_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t schema_authority_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t compiler_profile_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t compiler_session_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t player_profile_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    /* Canonical all/selected subshader, pass, stage, keyword-state, and tier
     * request. Evidence for a narrower request cannot be reused as whole. */
    uint8_t verification_scope_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t dependency_map_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t producer_fingerprint[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    /* CapturedShaderRelease.v1 digest from shader_catalog_decode_object() for
     * the candidate actually built/imported and re-extracted. This binds its
     * complete outer artifact, member/object coordinates, payload and schema;
     * source identity alone cannot bind a particular build. Appended in v2. */
    uint8_t candidate_release_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    /* Actual runtime image, backend/device and execution-environment authority,
     * distinct from serialized player build/capability metadata. Appended in
     * v3 so evidence from different environments cannot share a subject. A
     * digest is an identity binding, not itself proof of execution/selection. */
    uint8_t runtime_environment_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
} WholeShaderSubjectDescriptor;

typedef enum {
    WHOLE_SHADER_SUBJECT_OK = 0,
    WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT,
    WHOLE_SHADER_SUBJECT_INVALID_VALUE,
    WHOLE_SHADER_SUBJECT_SIZE_OVERFLOW,
    WHOLE_SHADER_SUBJECT_ALLOCATION_FAILED
} WholeShaderSubjectStatus;

typedef struct WholeShaderSubject WholeShaderSubject;

WholeShaderSubjectStatus whole_shader_subject_create(
    WholeShaderSubject** out_subject,
    const WholeShaderSubjectDescriptor* descriptor);
void whole_shader_subject_free(WholeShaderSubject* subject);
/* Returns a value copy whose string pointers remain owned by subject and are
 * valid only until whole_shader_subject_free(). */
WholeShaderSubjectStatus whole_shader_subject_describe(
    const WholeShaderSubject* subject,
    WholeShaderSubjectDescriptor* out_descriptor);
bool whole_shader_subject_equal(const WholeShaderSubject* left,
                                const WholeShaderSubject* right);

/* Stable little-endian encoding, in order: eight-byte `DXWSSUBJ` domain,
 * u32 version, u64 total size, fixed scalar coordinates, three u32-length-
 * prefixed non-NUL byte strings, then every named 32-byte digest in descriptor
 * order. No native struct bytes, pointer values, host paths, or JSON enter the
 * encoding. */
WholeShaderSubjectStatus whole_shader_subject_serialize(
    const WholeShaderSubject* subject, uint8_t** out_data, size_t* out_size);
void whole_shader_subject_serialized_free(uint8_t* data);
WholeShaderSubjectStatus whole_shader_subject_digest(
    const WholeShaderSubject* subject,
    uint8_t digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE]);

const char* whole_shader_subject_status_name(WholeShaderSubjectStatus status);

#endif /* WHOLE_SHADER_SUBJECT_H */
