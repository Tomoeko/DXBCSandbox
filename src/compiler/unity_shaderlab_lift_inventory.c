// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_lift_internal.h"

#include <string.h>

static UnityShaderLabLiftInventoryStatus inventory_status(ShaderLabSourceQualityStatus status) {
    switch (status) {
    case SHADERLAB_SOURCE_QUALITY_OK: return UNITY_SHADERLAB_INVENTORY_OBSERVED;
    case SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE: return UNITY_SHADERLAB_INVENTORY_SCOPE_UNAVAILABLE;
    case SHADERLAB_SOURCE_QUALITY_STRUCTURE_FAILED: return UNITY_SHADERLAB_INVENTORY_SCHEMA_FAILED;
    case SHADERLAB_SOURCE_QUALITY_EMISSION_FAILED: return UNITY_SHADERLAB_INVENTORY_EMISSION_FAILED;
    case SHADERLAB_SOURCE_QUALITY_ALLOCATION_FAILED: return UNITY_SHADERLAB_INVENTORY_ALLOCATION_FAILED;
    case SHADERLAB_SOURCE_QUALITY_INVALID_ARGUMENT: return UNITY_SHADERLAB_INVENTORY_INVALID_ARGUMENT;
    default: return UNITY_SHADERLAB_INVENTORY_SOURCE_MISMATCH;
    }
}

void unity_shaderlab_lift_record_inventory(const UnityShaderLabLiftInput *input,
                                           UnityShaderLabLiftArtifact *artifact) {
    if (!artifact) return;
    UnityShaderLabLiftSourceInventory *snapshot = &artifact->bounded_source_inventory;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!artifact->high_level || artifact->unity_uv_helpers) {
        snapshot->status = UNITY_SHADERLAB_INVENTORY_UNSUPPORTED_MODE;
        return;
    }
    if (!input || !artifact->source.buf || !artifact->source.len || !sb_ok(&artifact->source)) {
        snapshot->status = UNITY_SHADERLAB_INVENTORY_INVALID_ARGUMENT;
        return;
    }
    const ShaderLabSourceQualityRequest request = {.shader = input->shader,
        .archive = input->archive, .object = input->source_object};
    StringBuilder canonical;
    sb_init(&canonical);
    ShaderLabSourceQualityInventory inventory = {0};
    ShaderLabSourceQualityStatus status = shaderlab_source_quality_emit(
        &request, &canonical, &inventory, &snapshot->diagnostic);
    snapshot->status = inventory_status(status);
    if (status != SHADERLAB_SOURCE_QUALITY_OK) goto cleanup;
    if (canonical.len != artifact->source.len ||
        memcmp(canonical.buf, artifact->source.buf, canonical.len)) {
        snapshot->status = UNITY_SHADERLAB_INVENTORY_SOURCE_MISMATCH;
        goto cleanup;
    }
    status = shaderlab_source_quality_inventory_analyze(&request, &artifact->source,
        &inventory, &snapshot->quality, &snapshot->diagnostic);
    snapshot->status = inventory_status(status);
    if (status != SHADERLAB_SOURCE_QUALITY_OK) goto cleanup;
    snapshot->receipt_count = inventory.receipt_count;
    snapshot->source_size = inventory.source_size;
    memcpy(snapshot->source_digest, inventory.source_digest, sizeof(snapshot->source_digest));
    memcpy(snapshot->modeled_input_digest, inventory.modeled_input_digest,
           sizeof(snapshot->modeled_input_digest));
    snapshot->has_structural_authority = inventory.has_structural_authority;
    snapshot->structure = inventory.structure;
cleanup:
    shaderlab_source_quality_inventory_dispose(&inventory);
    sb_free(&canonical);
}

bool unity_shaderlab_lift_inventory_matches_source(const UnityShaderLabLiftArtifact *artifact,
    size_t source_size, const uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!artifact || !artifact->high_level || artifact->unity_uv_helpers || !source_digest ||
        artifact->bounded_source_inventory.status != UNITY_SHADERLAB_INVENTORY_OBSERVED ||
        !artifact->source.buf || !sb_ok(&artifact->source)) return false;
    const UnityShaderLabLiftSourceInventory *snapshot = &artifact->bounded_source_inventory;
    if (!snapshot->source_size || source_size != snapshot->source_size ||
        artifact->source.len != snapshot->source_size ||
        memcmp(source_digest, snapshot->source_digest, sizeof(snapshot->source_digest))) return false;
    uint8_t actual[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(artifact->source.buf, artifact->source.len, actual);
    return !memcmp(actual, snapshot->source_digest, sizeof(actual));
}

static const char *status_name(UnityShaderLabLiftInventoryStatus status) {
    switch (status) {
    case UNITY_SHADERLAB_INVENTORY_NOT_RUN: return "not-run";
    case UNITY_SHADERLAB_INVENTORY_UNSUPPORTED_MODE: return "unsupported-mode";
    case UNITY_SHADERLAB_INVENTORY_OBSERVED: return "observed";
    case UNITY_SHADERLAB_INVENTORY_SCOPE_UNAVAILABLE: return "scope-unavailable";
    case UNITY_SHADERLAB_INVENTORY_SCHEMA_FAILED: return "schema-failed";
    case UNITY_SHADERLAB_INVENTORY_EMISSION_FAILED: return "emission-failed";
    case UNITY_SHADERLAB_INVENTORY_SOURCE_MISMATCH: return "source-binding-mismatch";
    case UNITY_SHADERLAB_INVENTORY_ALLOCATION_FAILED: return "allocation-failed";
    default: return "invalid-argument";
    }
}

static void digest_json(StringBuilder *out, const char *name, const uint8_t digest[32]) {
    char hex[65];
    common_sha256_digest_to_hex(digest, hex);
    sb_appendf(out, "\"%s\":\"%s\"", name, hex);
}

bool unity_shaderlab_lift_append_inventory_json(const UnityShaderLabLiftArtifact *artifact,
                                                StringBuilder *out) {
    if (!out || !sb_ok(out)) return false;
    UnityShaderLabLiftInventoryStatus status = artifact ? artifact->bounded_source_inventory.status :
        UNITY_SHADERLAB_INVENTORY_NOT_RUN;
    if (artifact && artifact->attempted && (!artifact->high_level || artifact->unity_uv_helpers))
        status = UNITY_SHADERLAB_INVENTORY_UNSUPPORTED_MODE;
    if (status == UNITY_SHADERLAB_INVENTORY_OBSERVED &&
        !unity_shaderlab_lift_inventory_matches_source(artifact,
            artifact->bounded_source_inventory.source_size, artifact->bounded_source_inventory.source_digest))
        status = UNITY_SHADERLAB_INVENTORY_SOURCE_MISMATCH;
    sb_appendf(out, "{\"scope\":\"emitted-shaderlab-with-explicit-gaps\",\"status\":\"%s\",",
               status_name(status));
    if (status != UNITY_SHADERLAB_INVENTORY_OBSERVED) {
        sb_append(out, "\"source_sha256\":null,\"modeled_input_sha256\":null,\"quality\":null}");
        return sb_ok(out);
    }
    const UnityShaderLabLiftSourceInventory *snapshot = &artifact->bounded_source_inventory;
    const ShaderLabSourceQualityResult *quality = &snapshot->quality;
    digest_json(out, "source_sha256", snapshot->source_digest);
    sb_append_char(out, ',');
    digest_json(out, "modeled_input_sha256", snapshot->modeled_input_digest);
    sb_appendf(out, ",\"source_bytes\":%zu,\"receipts\":%zu,\"quality\":{"
        "\"classification\":\"%s\",\"reasons\":%u,\"gaps\":%u,\"wrapper_complete\":%s,"
        "\"wrapper_receipts\":%zu,\"linked_entries\":%zu,\"stage_class_counts\":{",
        snapshot->source_size, snapshot->receipt_count, hlsl_source_quality_class_name(quality->classification),
        quality->reasons, quality->gaps, quality->wrapper_complete ? "true" : "false",
        quality->wrapper_receipt_count, quality->linked_entry_count);
    for (unsigned index = 0; index < 5; ++index)
        sb_appendf(out, "%s\"%s\":%zu", index ? "," : "",
                   hlsl_source_quality_class_name((HLSLSourceQualityClass)index), quality->stage_class_counts[index]);
    sb_appendf(out, "},\"observed_stage_residual_lower_bound\":%zu,"
        "\"observed_stage_unknown_lower_bound\":%zu,\"observed_stage_incomplete_units\":%zu,"
        "\"literal_include_roots\":%zu,\"gap_names\":[",
        quality->observed_stage_residual_total, quality->observed_stage_unknown_provenance,
        quality->observed_stage_incomplete_units, quality->required_external_include_root_count);
    const struct { uint32_t bit; const char *name; } gaps[] = {
        {SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE, "external-include-semantics"},
        {SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY, "dependency-inventory"},
        {SHADERLAB_SOURCE_GAP_SCHEMA_AUTHORITY, "schema-authority"},
        {SHADERLAB_SOURCE_GAP_STAGE_COVERAGE, "stage-coverage"}
    };
    bool first = true;
    for (unsigned index = 0; index < sizeof(gaps) / sizeof(gaps[0]); ++index) {
        if (!(quality->gaps & gaps[index].bit)) continue;
        sb_appendf(out, "%s\"%s\"", first ? "" : ",", gaps[index].name);
        first = false;
    }
    sb_appendf(out, "]},\"schema_structure\":{\"present\":%s,"
        "\"scope\":\"serialized-d3d11-shaderlab-structure\",\"status\":\"%s\","
        "\"runtime_selection_certified\":false,\"visual_output_certified\":false}}",
        snapshot->has_structural_authority ? "true" : "false",
        snapshot->has_structural_authority ? shaderlab_structural_status_name(snapshot->structure.status) : "not-run");
    return sb_ok(out);
}
