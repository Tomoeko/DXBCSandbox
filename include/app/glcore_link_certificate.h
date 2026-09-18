// SPDX-License-Identifier: GPL-3.0-only

#ifndef GLCORE_LINK_CERTIFICATE_H
#define GLCORE_LINK_CERTIFICATE_H

#include "io/serialized_glcore_target.h"

/* A compiler response may expose either only the combined linked program or
 * the stage-pair response in which the fragment companion is exactly empty.
 * The latter is compiler-response semantics and is never represented as an
 * additional serialized archive entry. */
typedef enum {
    GLCORE_LINK_VECTOR_COMBINED_ONLY = 0,
    GLCORE_LINK_VECTOR_COMBINED_WITH_EMPTY_FRAGMENT = 1
} GLCoreLinkVectorShape;

typedef struct {
    UnitySerializedProgramStage stage;
    const uint8_t* bytes;
    size_t size;
} GLCoreLinkOutput;

typedef struct {
    const char* unity_version;
    int32_t compiler_platform;
    int32_t program_type;
    SerializedGLCoreLinkOwner owner;
    GLCoreLinkVectorShape shape;
    const GLCoreLinkOutput* outputs;
    size_t output_count;
} GLCoreGeneratedLinkVector;

typedef enum {
    GLCORE_LINK_CERTIFICATE_OK = 0,
    GLCORE_LINK_CERTIFICATE_INVALID_ARGUMENT,
    GLCORE_LINK_CERTIFICATE_TARGET_INVALID,
    GLCORE_LINK_CERTIFICATE_UNITY_VERSION_MISMATCH,
    GLCORE_LINK_CERTIFICATE_PLATFORM_MISMATCH,
    GLCORE_LINK_CERTIFICATE_PROGRAM_TYPE_MISMATCH,
    GLCORE_LINK_CERTIFICATE_OWNER_MISMATCH,
    GLCORE_LINK_CERTIFICATE_VECTOR_SHAPE_INVALID,
    GLCORE_LINK_CERTIFICATE_TEXT_MISMATCH
} GLCoreLinkCertificateStatus;

typedef struct {
    GLCoreLinkCertificateStatus status;
    size_t expected_size;
    size_t actual_size;
    /* SIZE_MAX when no differing byte exists or comparison was unavailable. */
    size_t first_differing_byte;
    bool serialized_target_available;
    bool response_vector_valid;
    bool released_text_exact;
} GLCoreLinkCertificateReport;

void glcore_link_certificate_report_init(
    GLCoreLinkCertificateReport* report);

GLCoreLinkCertificateStatus glcore_link_certificate_compare_vector(
    const SerializedGLCoreTarget* target,
    const GLCoreGeneratedLinkVector* generated,
    GLCoreLinkCertificateReport* report);

/* Direct byte comparison for callers that already own the linked text. */
GLCoreLinkCertificateStatus glcore_link_certificate_compare_text(
    const SerializedGLCoreTarget* target,
    const char* generated_unity_version,
    const uint8_t* generated_text, size_t generated_text_size,
    GLCoreLinkCertificateReport* report);

const char* glcore_link_certificate_status_name(
    GLCoreLinkCertificateStatus status);

#endif /* GLCORE_LINK_CERTIFICATE_H */
