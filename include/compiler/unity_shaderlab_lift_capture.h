// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_SHADERLAB_LIFT_CAPTURE_H
#define UNITY_SHADERLAB_LIFT_CAPTURE_H

#include "app/shader_catalog_object.h"
#include "app/whole_shader_evidence.h"
#include "compiler/unity_shaderlab_lift.h"
#include "translation/shaderlab_structural_certificate.h"

typedef struct UnityShaderLabLiftCapture UnityShaderLabLiftCapture;

typedef struct {
    const ShaderCatalog *catalog;
    const ShaderCatalogRecord *record;
    const TypeTreeSchemaRegistry *registry;
    const UnityCompileProfile *profile;
    UnityCompilerBroker *broker;
    const char *source_path;
    const char *source_directory;
    const char *source_basename;
    const HLSLLiftLimits *limits;
} UnityShaderLabLiftCaptureInput;

typedef enum {
    UNITY_SHADERLAB_CAPTURE_OK = 0,
    UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT,
    UNITY_SHADERLAB_CAPTURE_SOURCE_UNAVAILABLE,
    UNITY_SHADERLAB_CAPTURE_ARCHIVE_UNAVAILABLE,
    UNITY_SHADERLAB_CAPTURE_NO_ACCEPTED_SOURCE,
    UNITY_SHADERLAB_CAPTURE_SOURCE_CHANGED,
    UNITY_SHADERLAB_CAPTURE_OUT_OF_MEMORY,
    UNITY_SHADERLAB_CAPTURE_SUBJECT_UNAVAILABLE
} UnityShaderLabLiftCaptureStatus;

typedef struct {
    ShaderCatalogObjectStatus source_status;
    ShaderObjectStatus archive_status;
    HLSLLiftStatus lift_status;
    ShaderCatalogObjectReport target;
    uint8_t accepted_source_digest[32];
    uint8_t compiler_digest[32];
    uint8_t environment_digest[32];
    uint8_t profile_digest[32];
    ShaderLabStructuralDiagnostic structure;
    bool structural_digest_valid;
    uint8_t structural_digest[32];
} UnityShaderLabLiftCaptureReport;

/* Execute the production lift against an owned captured catalog record. The
 * profile is copied, the decoded object/archive are held for the whole run,
 * and the source snapshot is revalidated afterwards. No injected callbacks or
 * caller-supplied result reports can create this authority. OK means an
 * accepted artifact exists (possibly the verified low-level fallback), not
 * whole-shader equivalence or import acceptance. Other returns leave output
 * NULL. Inputs and borrowed broker must remain valid throughout this call. */
UnityShaderLabLiftCaptureStatus
unity_shaderlab_lift_capture(const UnityShaderLabLiftCaptureInput *input,
                             UnityShaderLabLiftCapture **output,
                             UnityShaderLabLiftCaptureReport *report);
const UnityShaderLabLiftResult *
unity_shaderlab_lift_capture_result(const UnityShaderLabLiftCapture *capture);
/* Copy the actual target/source/profile/session/scope bindings. String views
 * are owned by capture. Player, dependencies, producer and candidate release
 * remain zero for the coordinator to fill from their own authorities. Scope
 * is every emitted local D3D11 pass and its full generated compile domain. */
bool unity_shaderlab_lift_capture_subject(const UnityShaderLabLiftCapture *capture,
                                          WholeShaderSubjectDescriptor *descriptor);

/* Produce VARIANT_DOMAIN, FULL_DXBC, COMPILER_DIAGNOSTICS or REFLECTION_BINDING
 * using retained primary compile results and shared ordered compile-item identities. Subject fields
 * owned by this capture must match exactly. Other evidence planes must bind
 * the coordinator-supplied player/dependency/released-artifact identities.
 * Domain items combine the independent ordered keyword-family fingerprints;
 * the opaque capture also requires the existing complete alias/tier/cardinality
 * attestation. Runtime eligibility and selection remain separate. Output is
 * NULL on invalid/incomplete evidence or allocation error. Diagnostics compare
 * the complete normalized callback multiset against a strict empty policy;
 * even informational records therefore produce FAIL, not silent acceptance. */
WholeShaderEvidenceStatus unity_shaderlab_lift_capture_make_evidence(
    const UnityShaderLabLiftCapture *capture, const WholeShaderSubject *subject,
    WholeShaderVerificationPlane plane, WholeShaderEvidence **output);
typedef struct {
    ShaderCatalogObjectStatus source_status;
    ShaderCatalogObjectReport candidate;
    ShaderObjectStatus archive_status;
    ShaderLabStructuralDiagnostic target_structure;
    ShaderLabStructuralDiagnostic candidate_structure;
    bool emission_attempted;
    bool emitted;
    ShaderLabCandidateDiagnostic emission;
} UnityShaderLabStructuralEvidenceReport;

/* Execute candidate capture, structural coverage and exact-mode emission.
 * Requires the captured target's successful emission and structural coverage.
 * Compares independently derived ordered m_ParsedForm, schema, and re-emitted
 * source hashes. This strict projection retains compiled metadata and map
 * order; it can reject otherwise semantically equivalent representations.
 * Candidate release/name/version/platform must match subject. Unsupported
 * structure/emission or missing bounded digests produce typed UNAVAILABLE;
 * identity/input errors produce no evidence. OK means constructed, not PASS.
 * No source-identity or runtime claim is made. */
WholeShaderEvidenceStatus unity_shaderlab_lift_capture_structural_evidence(
    const UnityShaderLabLiftCapture *capture, const ShaderCatalog *candidate_catalog,
    const ShaderCatalogRecord *candidate_record, const TypeTreeSchemaRegistry *registry,
    const WholeShaderSubject *subject, WholeShaderEvidence **output,
    UnityShaderLabStructuralEvidenceReport *report);

void unity_shaderlab_lift_capture_free(UnityShaderLabLiftCapture *capture);

#endif
