// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_lift_internal.h"

#include <inttypes.h>

static const char *boolean(bool value) { return value ? "true" : "false"; }

/* Every textual label below is a fixed schema key or an internal enum name.
 * User-controlled strings are deliberately not part of this report. */
static void digest(StringBuilder *out, const char *name, const uint8_t bytes[32], bool present) {
    sb_appendf(out, "\"%s\":", name);
    if (present) {
        char hex[65];
        common_sha256_digest_to_hex(bytes, hex);
        sb_appendf(out, "\"%s\"", hex);
    } else {
        sb_append(out, "null");
    }
}

static const char *availability_name(UnityCompilerResponseAvailability availability) {
    switch (availability) {
    case UNITY_COMPILER_RESPONSE_AVAILABLE:
        return "available";
    case UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS:
        return "cache-only-miss";
    case UNITY_COMPILER_RESPONSE_INCLUDE_AUTHORITY_UNAVAILABLE:
        return "include-authority-unavailable";
    default:
        return "unknown";
    }
}

static void response_status(StringBuilder *out, const UnityCompilerResponseStatus *status) {
    sb_appendf(out,
               "{\"availability\":\"%s\",\"compiler_success\":%s,\"cache_hit\":%s,"
               "\"diagnostic_count\":%zu,\"actionable_diagnostic_count\":%zu}",
               availability_name(status->availability),
               boolean(status->compiler_success), boolean(status->from_cache),
               status->diagnostic_count,
               unity_compiler_response_status_actionable_diagnostic_count(status));
}

static void provenance(StringBuilder *out, const UnityGeneratedCompileProvenance *record) {
    sb_appendf(out, "{\"recorded\":%s,\"response_received\":%s,", boolean(record->recorded),
               boolean(record->response_received));
    digest(out, "request_sha256", record->request_digest, record->has_request_identity);
    sb_append_char(out, ',');
    digest(out, "controls_sha256", record->controls_digest, record->has_request_identity);
    sb_append_char(out, ',');
    digest(out, "source_sha256", record->source_digest, record->recorded);
    sb_append_char(out, ',');
    digest(out, "target_sha256", record->target_digest, record->recorded);
    sb_append_char(out, ',');
    digest(out, "output_sha256", record->output_digest, record->has_output_digest);
    sb_append_char(out, '}');
}

static void domain_report(StringBuilder *out, const UnityGeneratedDomainReport *report) {
    sb_appendf(out,
               "{\"status\":\"%s\",\"active_stages\":%zu,\"attested_stages\":%zu,"
               "\"generated_states\":%zu,\"planned_compiles\":%zu,\"compile_attempts\":%zu,"
               "\"matched_containers\":%zu,\"binding_attested_compiles\":%zu,"
               "\"binding_compatible_compiles\":%zu,\"requests\":[",
               unity_generated_domain_status_name(report->status), report->active_stage_count,
               report->attested_stage_count, report->generated_state_count,
               report->planned_compile_count, report->compile_attempt_count,
               report->matched_dxbc_count, report->runtime_binding_attested_compile_count,
               report->runtime_binding_compatible_compile_count);
    for (size_t i = 0; i < report->compiler_response_count; ++i) {
        const UnityGeneratedDomainCompilerResponseRecord *record = &report->compiler_responses[i];
        if (i)
            sb_append_char(out, ',');
        sb_appendf(out,
                   "{\"stage\":%d,\"tier\":%d,\"generated_state\":%zu,"
                   "\"aliased_state\":%zu,\"subprogram\":%d,\"response\":",
                   record->stage_index, record->hardware_tier_group, record->generated_state_index,
                   record->aliased_state_index, record->subprogram_index);
        response_status(out, &record->response);
        sb_append(out, ",\"provenance\":");
        provenance(out, &record->provenance);
        sb_append_char(out, '}');
    }
    sb_append(out, "],\"dxbc_mismatch\":");
    if (report->status == UNITY_GENERATED_DOMAIN_DXBC_MISMATCH) {
        const UnityGeneratedDomainDiagnostic *diagnostic = &report->diagnostic;
        const DXBCCompareResult *comparison = &diagnostic->dxbc_compare;
        sb_appendf(out,
                   "{\"kind\":\"%s\",\"stage\":%d,\"tier\":%d,"
                   "\"generated_state\":%zu,\"subprogram\":%d,\"expected_size\":%zu,"
                   "\"actual_size\":%zu,\"expected_value_hex\":\"%016" PRIx64
                   "\",\"actual_value_hex\":\"%016" PRIx64 "\",\"byte_offset\":",
                   dxbc_compare_status_name(comparison->status), diagnostic->stage_index,
                   diagnostic->hardware_tier_group, diagnostic->generated_state_index,
                   diagnostic->subprogram_index, comparison->expected_size, comparison->actual_size,
                   comparison->expected_value, comparison->actual_value);
        if (comparison->first_differing_byte == SIZE_MAX)
            sb_append(out, "null");
        else
            sb_appendf(out, "%zu", comparison->first_differing_byte);
        sb_append(out, ",\"instruction\":");
        if (comparison->instruction_index == UINT32_MAX)
            sb_append(out, "null");
        else
            sb_appendf(out, "%" PRIu32, comparison->instruction_index);
        sb_append(out, ",\"token\":");
        if (comparison->token_index == UINT32_MAX)
            sb_append(out, "null");
        else
            sb_appendf(out, "%" PRIu32, comparison->token_index);
        sb_append_char(out, '}');
    } else
        sb_append(out, "null");
    sb_append_char(out, '}');
}

static void source_map(StringBuilder *out, const ShaderLabExpressionSourceMap *map) {
    sb_appendf(out, "{\"complete\":%s,\"source_bytes\":%zu,", boolean(map->complete),
               map->source_size);
    digest(out, "source_sha256", map->source_digest, map->complete);
    sb_append(out, ",\"bodies\":[");
    for (size_t i = 0; i < map->count; ++i) {
        const ShaderLabExpressionSourceRecord *record = &map->records[i];
        if (i)
            sb_append_char(out, ',');
        sb_appendf(out,
                   "{\"subshader\":%d,\"pass\":%d,\"stage\":%d,\"subprogram\":%d,"
                   "\"blob\":%d,\"tier\":%d,\"serialized_state\":%zu,",
                   record->subshader_index, record->pass_index, record->stage_index,
                   record->subprogram_index, record->blob_index, record->hardware_tier_group,
                   record->serialized_state);
        digest(out, "target_sha256", record->target_digest, true);
        sb_append(out, ",\"instructions\":[");
        for (size_t j = 0; j < record->instructions.count; ++j) {
            const HLSLExpressionOrigin *origin = &record->instructions.origins[j];
            if (j)
                sb_append_char(out, ',');
            sb_appendf(out,
                       "{\"kind\":\"%s\",\"instruction\":%d,\"source_instruction\":%" PRIu32
                       ",\"destination_lanes\":%u,\"begin\":%zu,\"end\":%zu,"
                       "\"definition_begin\":%zu,\"definition_end\":%zu}",
                       hlsl_expression_origin_kind_name(origin->kind), origin->instruction_index,
                       origin->source_instruction_index, (unsigned)origin->destination_lanes,
                       origin->source_begin, origin->source_end, origin->definition_begin,
                       origin->definition_end);
        }
        sb_append(out, "]}");
    }
    sb_append(out, "]}");
}

static void helper_checks(StringBuilder *out, const UnityShaderLabLiftPassReport *pass) {
    sb_append_char(out, '[');
    for (size_t i = 0; i < pass->helper_check_count; ++i) {
        const UnityShaderLabLiftHelperCheck *check = &pass->helper_checks[i];
        if (i)
            sb_append_char(out, ',');
        const bool valid = check->status == UNITY_UV_HELPER_OK;
        sb_appendf(out, "{\"domain_compile_index\":%zu,\"status\":\"%s\",",
                   check->domain_compile_index, unity_uv_helper_status_name(check->status));
        digest(out, "compile_request_sha256", check->evidence.compile_request_digest, valid);
        sb_append_char(out, ',');
        digest(out, "preprocess_request_sha256", check->evidence.preprocess_request_digest, valid);
        sb_append_char(out, ',');
        digest(out, "expansion_sha256", check->evidence.expansion.expansion_digest, valid);
        sb_append(out, ",\"definition_ranges\":[");
        if (valid) {
            const UnityUvHelperExpansion *expansion = &check->evidence.expansion;
            sb_appendf(out, "[%zu,%zu],[%zu,%zu],[%zu,%zu]", expansion->definitions[0].begin,
                       expansion->definitions[0].end, expansion->definitions[1].begin,
                       expansion->definitions[1].end, expansion->probe.begin, expansion->probe.end);
        }
        sb_appendf(out,
                   "],\"compile_received\":%s,\"compile_identity_matched\":%s,"
                   "\"response\":",
                   boolean(check->compile_received), boolean(check->compile_identity_matched));
        response_status(out, &check->preprocessing.status);
        sb_append_char(out, '}');
    }
    sb_append_char(out, ']');
}

static void artifact(StringBuilder *out, const UnityShaderLabLiftArtifact *value) {
    uint8_t source_digest[32];
    const bool has_source = value->source.buf && value->source.len && sb_ok(&value->source);
    if (has_source)
        common_sha256(value->source.buf, value->source.len, source_digest);
    sb_appendf(out,
               "{\"attempted\":%s,\"high_level\":%s,\"unity_uv_helpers\":%s,"
               "\"status\":\"%s\",\"source_bytes\":%zu,",
               boolean(value->attempted), boolean(value->high_level),
               boolean(value->unity_uv_helpers), hlsl_lift_status_name(value->status),
               value->source.len);
    digest(out, "source_sha256", source_digest, has_source);
    sb_appendf(
        out, ",\"emission_attempted\":%s,\"emission_reason\":", boolean(value->emission_attempted));
    if (value->emission_attempted)
        sb_appendf(out, "\"%s\"", shaderlab_candidate_reason_name(&value->emission_diagnostic));
    else
        sb_append(out, "null");
    sb_appendf(out, ",\"preprocessing\":{\"attempted\":%s,\"received\":%s,",
               boolean(value->preprocess_attempted), boolean(value->preprocess_received));
    digest(out, "request_sha256", value->preprocessing.request_digest,
           value->preprocessing.has_request_identity);
    sb_append_char(out, ',');
    digest(out, "controls_sha256", value->preprocessing.controls_digest,
           value->preprocessing.has_request_identity);
    sb_append(out, ",\"response\":");
    if (value->preprocess_attempted)
        response_status(out, &value->preprocessing.status);
    else
        sb_append(out, "null");
    sb_appendf(out, "},\"pass_count\":%zu,\"certified_pass_count\":%zu,\"passes\":[",
               value->pass_count, value->certified_pass_count);
    for (size_t p = 0; p < value->pass_count; ++p) {
        const UnityShaderLabLiftPassReport *pass = &value->passes[p];
        if (p)
            sb_append_char(out, ',');
        sb_appendf(out,
                   "{\"subshader\":%d,\"pass\":%d,\"serialized_pass\":%d,\"snippet\":%d,"
                   "\"plan_attempted\":%s,\"plan_status\":",
                   pass->subshader_index, pass->pass_index, pass->serialized_pass_index,
                   pass->snippet_index, boolean(pass->plan_attempted));
        if (pass->plan_attempted)
            sb_appendf(out, "\"%s\"", shaderlab_variant_plan_status_name(pass->plan_status));
        else
            sb_append(out, "null");
        sb_appendf(out, ",\"certification_attempted\":%s,\"domain\":",
                   boolean(pass->certification_attempted));
        if (pass->certification_attempted)
            domain_report(out, &pass->domain);
        else
            sb_append(out, "null");
        sb_append(out, ",\"helper_checks\":");
        helper_checks(out, pass);
        sb_append_char(out, '}');
    }
    sb_append(out, "],\"source_map\":");
    source_map(out, &value->source_map);
    sb_append_char(out, '}');
}

char *unity_shaderlab_lift_format_json(const UnityShaderLabLiftResult *result) {
    if (!result)
        return NULL;
    StringBuilder out;
    sb_init(&out);
    const char *selection = !result->accepted              ? "unverified"
                            : result->accepted->high_level ? "high-level"
                                                           : "low-level-fallback";
    sb_appendf(&out,
               "{\"schema\":\"dxbc-shaderlab-lift-v1\","
               "\"scope\":\"generated-local-d3d11-program-domain\","
               "\"lift\":{\"id\":\"%s\",\"version\":%u},\"selection\":\"%s\","
               "\"authority\":{\"pinned\":%s,",
               HLSL_HIGH_LEVEL_LIFT_ID, HLSL_HIGH_LEVEL_LIFT_VERSION, selection,
               boolean(result->authority_pinned));
    digest(&out, "compiler_sha256", result->compiler_digest, result->authority_pinned);
    sb_append_char(&out, ',');
    digest(&out, "environment_sha256", result->environment_digest, result->authority_pinned);
    sb_append_char(&out, ',');
    digest(&out, "profile_sha256", result->profile_digest, result->authority_pinned);
    sb_append_char(&out, ',');
    digest(&out, "source_path_sha256", result->source_path_digest, result->authority_pinned);
    sb_append_char(&out, ',');
    digest(&out, "source_directory_sha256", result->source_directory_digest,
           result->authority_pinned);
    sb_append_char(&out, ',');
    digest(&out, "source_basename_sha256", result->source_basename_digest,
           result->authority_pinned);
    sb_appendf(&out,
               "},\"limits\":{\"candidates\":%zu,\"compiles\":%zu,\"elapsed_ms\":%" PRIu64
               "},\"stats\":{\"candidates\":%zu,\"compiles\":%zu,\"compile_cache_hits\":%zu,"
               "\"accepted_lifts\":%zu,\"preprocess_requests\":%zu,\"elapsed_ms\":%" PRIu64
               "},\"baseline\":",
               result->limits.max_candidates, result->limits.max_compiles,
               result->limits.max_elapsed_ms, result->stats.candidates, result->stats.compiles,
               result->stats.cache_hits, result->stats.accepted, result->preprocess_requests,
               result->stats.elapsed_ms);
    artifact(&out, &result->baseline);
    sb_append(&out, ",\"candidate\":");
    artifact(&out, &result->candidate);
    sb_appendf(&out, ",\"unity_helper\":{\"id\":\"%s\",\"version\":%u,\"baseline\":",
               UNITY_UV_HELPER_LIFT_ID, UNITY_UV_HELPER_LIFT_VERSION);
    artifact(&out, &result->helper_baseline);
    sb_append(&out, ",\"candidate\":");
    artifact(&out, &result->helper_candidate);
    sb_append_char(&out, '}');
    sb_append(&out, "}\n");
    if (!sb_ok(&out)) {
        sb_free(&out);
        return NULL;
    }
    return sb_detach(&out);
}
