// SPDX-License-Identifier: GPL-3.0-only

#ifndef RELEASE_SHADER_EVIDENCE_H
#define RELEASE_SHADER_EVIDENCE_H

#include "app/release_shader_object_certificate.h"
#include "app/shader_catalog_object.h"
#include "app/whole_shader_evidence.h"

typedef enum {
    RELEASE_SHADER_EVIDENCE_OK = 0,
    RELEASE_SHADER_EVIDENCE_INVALID_ARGUMENT,
    RELEASE_SHADER_EVIDENCE_TARGET_UNAVAILABLE,
    RELEASE_SHADER_EVIDENCE_CANDIDATE_UNAVAILABLE,
    RELEASE_SHADER_EVIDENCE_SUBJECT_MISMATCH,
    RELEASE_SHADER_EVIDENCE_COMPARISON_INVALID,
    RELEASE_SHADER_EVIDENCE_CONSTRUCTION_FAILED
} ReleaseShaderEvidenceStatus;

typedef struct {
    ShaderCatalogObjectStatus target_status;
    ShaderCatalogObjectStatus candidate_status;
    ShaderCatalogObjectReport target;
    ShaderCatalogObjectReport candidate;
    ReleaseShaderObjectCertificateReport canonical;
    WholeShaderEvidenceStatus evidence_status;
} ReleaseShaderEvidenceReport;

/* Execute re-extraction from two owned catalog records and their captured
 * snapshots, then the unchanged canonical Release comparator. This initial
 * producer admits only byte-identical object payloads and schema shapes, a
 * stricter policy than canonical equality. Different storage encodings that
 * pass the canonical comparator can therefore still yield FAIL here.
 *
 * The subject must bind every target coordinate/payload/schema and the actual
 * candidate release_digest from shader_catalog_decode_object(). Non-null
 * object references currently yield UNAVAILABLE: this producer has no resolved
 * dependency authority and never accepts raw PPtr equality as a substitute.
 *
 * OK means typed evidence was constructed; inspect its derived PASS, FAIL or
 * UNAVAILABLE status. Other returns leave *out_evidence NULL. The report is
 * always initialized. Inputs must remain immutable until the call completes.
 * No importer, structure, runtime, dependency or pixel claim is produced. */
ReleaseShaderEvidenceStatus release_shader_make_reextraction_evidence(
    const ShaderCatalog *target_catalog, const ShaderCatalogRecord *target_record,
    const ShaderCatalog *candidate_catalog, const ShaderCatalogRecord *candidate_record,
    const TypeTreeSchemaRegistry *registry, const WholeShaderSubject *subject,
    WholeShaderEvidence **out_evidence, ReleaseShaderEvidenceReport *report);

/* Reuses the same capture and subject-binding boundary, but compares the
 * complete ordered m_State sequence and topology. This is deliberately
 * stricter than the canonical comparator's map-order normalization. Unresolved
 * dependencies do not erase available state evidence; the independent
 * dependency/re-extraction planes still prevent a whole logical claim. */
ReleaseShaderEvidenceStatus release_shader_make_render_state_evidence(
    const ShaderCatalog *target_catalog, const ShaderCatalogRecord *target_record,
    const ShaderCatalog *candidate_catalog, const ShaderCatalogRecord *candidate_record,
    const TypeTreeSchemaRegistry *registry, const WholeShaderSubject *subject,
    WholeShaderEvidence **out_evidence, ReleaseShaderEvidenceReport *report);

#endif
