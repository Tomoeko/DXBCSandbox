// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADER_BUNDLE_EVIDENCE_H
#define UNITY_SHADER_BUNDLE_EVIDENCE_H

#include "app/shader_catalog_object.h"
#include "app/whole_shader_evidence.h"
#include "compiler/unity_shader_bundle_gate.h"

typedef struct UnityShaderBundleAuthority UnityShaderBundleAuthority;

typedef enum {
    UNITY_SHADER_BUNDLE_EVIDENCE_OK = 0,
    UNITY_SHADER_BUNDLE_EVIDENCE_INVALID_ARGUMENT,
    UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_UNAVAILABLE,
    UNITY_SHADER_BUNDLE_EVIDENCE_INPUT_CHANGED,
    UNITY_SHADER_BUNDLE_EVIDENCE_GATE_FAILED,
    UNITY_SHADER_BUNDLE_EVIDENCE_CAPTURE_FAILED,
    UNITY_SHADER_BUNDLE_EVIDENCE_ARTIFACT_MISMATCH,
    UNITY_SHADER_BUNDLE_EVIDENCE_ALLOCATION_FAILED
} UnityShaderBundleEvidenceStatus;

typedef struct {
    uint8_t source_digest[32];
    /* The selected launch file, which may be a platform wrapper. This is not
     * a claim to have inventoried every loaded Editor library or compiler. */
    uint8_t launcher_digest[32];
    uint8_t bridge_digest[32];
    uint8_t artifact_digest[32];
    uint8_t release_digest[32];
    uint8_t authority_digest[32];
} UnityShaderBundleAuthoritySummary;

typedef struct {
    UnityShaderBundleGateStatus gate_status;
    UnityShaderBundleGateRunResult gate;
    ShaderCatalogStatus catalog_status;
    ShaderCatalogObjectStatus object_status;
    ShaderCatalogObjectReport object;
} UnityShaderBundleEvidenceDiagnostic;

/* Execute the existing isolated import/build gate and capture its exact
 * published object. Initial scope: one regular .shader input, one Class48
 * object in the produced bundle, Windows64/D3D11, Unity 2021.3.35f1, warnings
 * rejected. Hold validated source/launcher/bridge file snapshots throughout
 * execution and reject observed drift before returning an authority.
 *
 * Only this execution API creates the opaque authority; a caller-written
 * report or Boolean cannot create one. A late capture/drift failure may leave
 * the gate's already published bundle for inspection, without evidence.
 * Schema resolution is the normal catalog boundary (embedded or exact registry).
 * No dependency, compiler-session, player, runtime or pixel claim is made. */
UnityShaderBundleEvidenceStatus unity_shader_bundle_capture(
    const UnityShaderBundleGateOptions *options, const TypeTreeSchemaRegistry *registry,
    UnityShaderBundleAuthority **out_authority, UnityShaderBundleEvidenceDiagnostic *diagnostic);

void unity_shader_bundle_authority_free(UnityShaderBundleAuthority *authority);
bool unity_shader_bundle_authority_describe(const UnityShaderBundleAuthority *authority,
                                            UnityShaderBundleAuthoritySummary *summary);
/* Borrowed immutable catalog and its sole Shader record, valid until free.
 * Re-extraction uses these same retained snapshots, never a separate input. */
const ShaderCatalog *
unity_shader_bundle_authority_catalog(const UnityShaderBundleAuthority *authority);

/* Revalidate the retained artifact, then compare the actually imported source
 * and produced release identity with subject. Mismatches yield typed FAIL;
 * stale snapshots and incompatible target/name/version yield no evidence.
 * Registry must resolve the same schema used by capture. */
WholeShaderEvidenceStatus unity_shader_bundle_authority_make_evidence(
    const UnityShaderBundleAuthority *authority, const TypeTreeSchemaRegistry *registry,
    const WholeShaderSubject *subject, WholeShaderEvidence **out_evidence);

const char *unity_shader_bundle_evidence_status_name(UnityShaderBundleEvidenceStatus status);

#endif
