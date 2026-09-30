// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_source_quality.h"

#include <inttypes.h>

bool hlsl_source_quality_append_json(const HLSLSourceQualityResult *result,
                                     StringBuilder *output) {
    if (!result || !output) return false;
    sb_appendf(output,
        "{\"scope\":\"observed-source-units\",\"classification\":\"%s\","
        "\"emission_status\":\"%s\",\"stage\":%u,\"pass\":%" PRIu32
        ",\"entry\":%" PRIu32 ",\"reasons\":%" PRIu32 ",\"counts\":{",
        hlsl_source_quality_class_name(result->classification),
        hlsl_emit_status_name(result->emission_status), (unsigned)result->stage,
        result->pass_index, result->entry_point_index, result->reasons);
    const HLSLSourceQualityCounters *counts = &result->counts;
    struct Counter { const char *name; size_t count; };
    const struct Counter counters[] = {
        {"ast_expressions", counts->ast_expressions},
        {"ast_statements", counts->ast_statements},
        {"emission_events", counts->emission_events},
        {"inspected_units", counts->inspected_units},
        {"incomplete_units", counts->incomplete_units},
        {"unknown_provenance", counts->unknown_provenance},
        {"logical_operations", counts->logical_operations},
        {"logical_value_references", counts->logical_value_references},
        {"semantic_projections", counts->semantic_projections},
        {"real_bitcasts", counts->real_bitcasts},
        {"register_storage", counts->register_storage},
        {"lane_transport", counts->lane_transport},
        {"scalarized_intrinsics", counts->scalarized_intrinsics},
        {"raw_buffer_reconstruction", counts->raw_buffer_reconstruction},
        {"synthetic_interface", counts->synthetic_interface},
        {"instruction_assignments", counts->instruction_assignments},
        {"unstructured_control", counts->unstructured_control},
        {"storage_bitcasts", counts->storage_bitcasts},
        {"sibling_declarations", counts->sibling_declarations},
        {"sibling_declaration_witnesses", counts->sibling_declaration_witnesses},
        {"resource_declarations", counts->resource_declarations},
        {"residual_total", counts->residual_total},
        {"cbuffer_declarations", counts->cbuffer_declarations},
        {"cbuffer_fields", counts->cbuffer_fields}
    };
    for (size_t index = 0; index < sizeof(counters) / sizeof(counters[0]); ++index)
        sb_appendf(output, "%s\"%s\":%zu", index ? "," : "", counters[index].name,
                   counters[index].count);
    sb_append(output, "},\"first_issue\":");
    if (result->has_first_issue) {
        const HLSLSourceQualityObservation *issue = &result->first_issue;
        sb_appendf(output,
            "{\"unit\":%" PRIu32 ",\"unit_kind\":%u,\"kind\":%u,\"ast_kind\":%d,"
            "\"reasons\":%" PRIu32 ",\"instruction\":%d,\"source_instruction\":%" PRIu32
            ",\"lanes\":%u,\"artifacts\":%" PRIu32 ",\"known\":%s}",
            issue->source_unit_id, (unsigned)issue->unit_kind, (unsigned)issue->kind,
            issue->ast_kind, issue->reasons, issue->facts.instruction_index,
            issue->facts.source_instruction_index, (unsigned)issue->facts.lanes,
            issue->facts.artifacts, issue->facts.known ? "true" : "false");
    } else {
        sb_append(output, "null");
    }
    sb_append_char(output, '}');
    return sb_ok(output);
}
