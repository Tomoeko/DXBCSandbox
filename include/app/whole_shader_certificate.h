// SPDX-License-Identifier: GPL-3.0-only

#ifndef WHOLE_SHADER_CERTIFICATE_H
#define WHOLE_SHADER_CERTIFICATE_H

#include "app/whole_shader_subject.h"

#include <stdbool.h>
#include <stdint.h>

#define WHOLE_SHADER_CERTIFICATE_FORMAT_VERSION 2U
#define WHOLE_SHADER_CERTIFICATE_DIGEST_SIZE WHOLE_SHADER_SUBJECT_DIGEST_SIZE

typedef enum {
    /* Candidate ShaderLab projection is structurally representable/exact. */
    WHOLE_SHADER_PLANE_STRUCTURAL = 0,
    /* Complete selected keyword/state/tier domain and alias mapping. */
    WHOLE_SHADER_PLANE_VARIANT_DOMAIN,
    /* Exact stripped DXBC container for every selected domain item. */
    WHOLE_SHADER_PLANE_FULL_DXBC,
    /* Complete normalized diagnostic multiset under pinned controls. */
    WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS,
    /* Runtime-visible parameter/resource binding contract. */
    WHOLE_SHADER_PLANE_REFLECTION_BINDING,
    /* Isolated ShaderImporter result, including its warning policy. */
    WHOLE_SHADER_PLANE_IMPORTER,
    /* Exact canonical Release Shader-object re-extraction certificate. */
    WHOLE_SHADER_PLANE_REEXTRACTION,
    /* Serialized pass/subshader render state. */
    WHOLE_SHADER_PLANE_RENDER_STATE,
    /* Exact player build/capability profile. */
    WHOLE_SHADER_PLANE_PLAYER_PROFILE,
    /* Runtime pass/subshader/variant winner selection. */
    WHOLE_SHADER_PLANE_RUNTIME_SELECTION,
    /* Resolved PPtr/fallback/include/runtime dependency closure. */
    WHOLE_SHADER_PLANE_DEPENDENCY_CLOSURE,
    /* Exact serialized released GLCore text/wrapper evidence. */
    WHOLE_SHADER_PLANE_GLSL,
    /* Finite render fixture inputs and graphics environment. */
    WHOLE_SHADER_PLANE_RUNTIME_INPUTS,
    /* Finite expected/observed pixel artifact comparisons. */
    WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS,
    WHOLE_SHADER_PLANE_COUNT
} WholeShaderVerificationPlane;

#define WHOLE_SHADER_PLANE_BIT(plane) \
    (UINT64_C(1) << (unsigned)(plane))

/* Conditional logical equivalence on the pinned D3D11 player for identical,
 * defined runtime inputs and dependency closure. It is not a pixel claim. */
#define WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK                         \
    (WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_STRUCTURAL) |             \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_VARIANT_DOMAIN) |         \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_FULL_DXBC) |              \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS) |   \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_REFLECTION_BINDING) |     \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_IMPORTER) |               \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_REEXTRACTION) |           \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_RENDER_STATE) |           \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_PLAYER_PROFILE) |         \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_RUNTIME_SELECTION) |      \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_DEPENDENCY_CLOSURE))

/* Complete selected variant-domain authority plus exact stripped containers.
 * This is a byte claim, not import acceptance or runtime/pixel equivalence. */
#define WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK                            \
    (WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_VARIANT_DOMAIN) |         \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_FULL_DXBC))

/* ShaderImporter success is separate from compiler-request diagnostics. The
 * import plane's evidence authority binds the warning policy used by its
 * producer. */
#define WHOLE_SHADER_IMPORT_ACCEPTANCE_REQUIRED_MASK                     \
    WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_IMPORTER)

#define WHOLE_SHADER_RELEASE_OBJECT_REQUIRED_MASK                        \
    WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_REEXTRACTION)

#define WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK                          \
    (WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_RUNTIME_INPUTS) |         \
     WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS))

#define WHOLE_SHADER_EMPIRICAL_VISUAL_REQUIRED_MASK                      \
    (WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK |                          \
     WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK)

typedef enum {
    WHOLE_SHADER_PLANE_NOT_REQUESTED = 0,
    WHOLE_SHADER_PLANE_PASS,
    WHOLE_SHADER_PLANE_FAIL,
    WHOLE_SHADER_PLANE_UNAVAILABLE,
    WHOLE_SHADER_PLANE_NOT_RUN
} WholeShaderPlaneStatus;

typedef struct WholeShaderEvidence WholeShaderEvidence;
typedef struct WholeShaderCertificateInput WholeShaderCertificateInput;

typedef enum {
    WHOLE_SHADER_CERTIFICATE_OK = 0,
    WHOLE_SHADER_CERTIFICATE_INVALID_ARGUMENT,
    WHOLE_SHADER_CERTIFICATE_UNSUPPORTED_VERSION,
    WHOLE_SHADER_CERTIFICATE_INVALID_REQUEST_MASK,
    WHOLE_SHADER_CERTIFICATE_SUBJECT_AUTHORITY_MISSING,
    WHOLE_SHADER_CERTIFICATE_PLANE_EVIDENCE_INVALID,
    WHOLE_SHADER_CERTIFICATE_COVERAGE_MISMATCH,
    WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_FAILED,
    WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_UNAVAILABLE,
    WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE,
    WHOLE_SHADER_CERTIFICATE_ALLOCATION_FAILED
} WholeShaderCertificateStatus;

typedef enum {
    WHOLE_SHADER_CERTIFICATE_ADD_OK = 0,
    WHOLE_SHADER_CERTIFICATE_ADD_INVALID_ARGUMENT,
    WHOLE_SHADER_CERTIFICATE_ADD_PLANE_NOT_REQUESTED,
    WHOLE_SHADER_CERTIFICATE_ADD_SUBJECT_MISMATCH,
    WHOLE_SHADER_CERTIFICATE_ADD_DUPLICATE_PLANE
} WholeShaderCertificateAddStatus;

typedef struct {
    WholeShaderCertificateStatus status;
    uint64_t requested_plane_mask;
    uint64_t passed_plane_mask;
    uint64_t failed_plane_mask;
    uint64_t unavailable_plane_mask;
    uint64_t incomplete_plane_mask;
    uint64_t invalid_plane_mask;
    uint64_t coverage_mismatch_plane_mask;
    WholeShaderVerificationPlane first_problem_plane;
    WholeShaderPlaneStatus first_problem_status;
    bool requested_scope_certified;
    bool d3d11_byte_equivalence_certified;
    bool importer_acceptance_certified;
    bool release_object_equivalence_certified;
    bool d3d11_logical_equivalence_certified;
    bool glsl_artifact_equivalence_certified;
    bool finite_pixel_observations_certified;
    /* Conditional logical proof plus equality for the exact finite fixture
     * observations only. It is not a proof over every possible scene/input. */
    bool empirical_visual_equivalence_certified;
    /* No source-identity evidence plane exists. A stripped Release object and
     * equal compiled artifacts can never make this true. */
    bool source_identity_certified;
    /* Finite observations never imply universal output identity. */
    bool universal_visual_equivalence_certified;
} WholeShaderCertificateReport;

/* Opaque input accepting only typed evidence objects. There is intentionally
 * no JSON/report deserializer capable of setting PASS. Partial subjects may
 * collect evidence, but a runtime-selection PASS requires nonzero player,
 * dependency, released-candidate and runtime-environment coordinates. Their
 * presence is necessary, not sufficient: the runtime producer must establish
 * their association and selection contract independently. */
WholeShaderCertificateStatus whole_shader_certificate_input_create(
    WholeShaderCertificateInput** out_input,
    const WholeShaderSubject* subject, uint64_t requested_plane_mask);
void whole_shader_certificate_input_free(WholeShaderCertificateInput* input);
WholeShaderCertificateAddStatus whole_shader_certificate_input_add_evidence(
    WholeShaderCertificateInput* input,
    const WholeShaderEvidence* evidence);

void whole_shader_certificate_report_init(
    WholeShaderCertificateReport* report);
WholeShaderCertificateStatus whole_shader_certificate_evaluate(
    const WholeShaderCertificateInput* input,
    WholeShaderCertificateReport* report);

const char* whole_shader_verification_plane_name(
    WholeShaderVerificationPlane plane);
const char* whole_shader_plane_status_name(WholeShaderPlaneStatus status);
const char* whole_shader_certificate_status_name(
    WholeShaderCertificateStatus status);
const char* whole_shader_certificate_add_status_name(
    WholeShaderCertificateAddStatus status);

#endif /* WHOLE_SHADER_CERTIFICATE_H */
