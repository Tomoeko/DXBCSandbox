// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_source_quality_internal.h"
#include "translation/shaderlab_emitter_internal.h"

#include <stdlib.h>
#include <string.h>

enum { MAX_RECEIPTS = 256, MAX_ENTRY_RECORDS = 32, MAX_SOURCE_BYTES = 4 * 1024 * 1024 };

void shaderlab_source_quality_inventory_dispose(ShaderLabSourceQualityInventory *inventory) {
    if (!inventory) return;
    free(inventory->receipts);
    shaderlab_expression_source_map_free(&inventory->entries);
    memset(inventory, 0, sizeof(*inventory));
}

static bool append_receipt(ShaderLabSourceQualityCapture *capture, const StringBuilder *source,
                           size_t end, ShaderLabSourceSyntaxKind kind, int property,
                           int subshader, int pass, int stage, size_t record_index) {
    if (!capture) return true;
    if (capture->failed || !capture->inventory || !source || !sb_ok(source) ||
        end < capture->cursor || end > source->len || source->len > MAX_SOURCE_BYTES) {
        capture->failed = true;
        return false;
    }
    if (end == capture->cursor) return true;
    ShaderLabSourceQualityInventory *inventory = capture->inventory;
    if (inventory->receipt_count == MAX_RECEIPTS) {
        capture->failed = true;
        return false;
    }
    if (inventory->receipt_count == inventory->receipt_capacity) {
        const size_t capacity = inventory->receipt_capacity ? inventory->receipt_capacity * 2 : 32;
        ShaderLabSourceSyntaxReceipt *receipts = realloc(inventory->receipts, capacity * sizeof(*receipts));
        if (!receipts) {
            capture->failed = true;
            return false;
        }
        inventory->receipts = receipts;
        inventory->receipt_capacity = capacity;
    }
    ShaderLabSourceSyntaxReceipt *receipt = &inventory->receipts[inventory->receipt_count++];
    *receipt = (ShaderLabSourceSyntaxReceipt){.kind = kind,
        .source_begin = capture->cursor, .source_end = end,
        .property_index = property, .subshader_index = subshader, .pass_index = pass,
        .stage_index = stage, .entry_record_index = record_index};
    common_sha256(source->buf + receipt->source_begin, receipt->source_end - receipt->source_begin,
                  receipt->source_digest);
    capture->cursor = end;
    return true;
}

bool shaderlab_source_quality_capture_receipt(ShaderLabSourceQualityCapture *capture,
    const StringBuilder *source, ShaderLabSourceSyntaxKind kind, int property,
    int subshader, int pass, int stage, size_t record_index) {
    return !capture || append_receipt(capture, source, source->len, kind, property,
                                     subshader, pass, stage, record_index);
}

bool shaderlab_source_quality_capture_body(ShaderLabSourceQualityCapture *capture,
    size_t begin, size_t end, size_t record_index) {
    if (!capture) return true;
    if (capture->failed || begin >= end || record_index >= MAX_ENTRY_RECORDS ||
        capture->body_count == MAX_ENTRY_RECORDS) {
        capture->failed = true;
        return false;
    }
    capture->bodies[capture->body_count++] = (ShaderLabSourceQualityBody){begin, end, record_index};
    return true;
}

bool shaderlab_source_quality_capture_stage(ShaderLabSourceQualityCapture *capture,
    const StringBuilder *source, size_t stage_begin, size_t first_body,
    int subshader, int pass, int stage) {
    if (!capture) return true;
    if (capture->failed || capture->cursor != stage_begin || first_body >= capture->body_count) {
        capture->failed = true;
        return false;
    }
    for (size_t index = first_body; index < capture->body_count; ++index) {
        const ShaderLabSourceQualityBody *body = &capture->bodies[index];
        if (body->end > source->len - stage_begin ||
            !append_receipt(capture, source, stage_begin + body->begin,
                SHADERLAB_SOURCE_SYNTAX_ROUTING, -1, subshader, pass, stage, SIZE_MAX) ||
            !append_receipt(capture, source, stage_begin + body->end,
                SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY, -1, subshader, pass, stage, body->record_index))
            return false;
    }
    return append_receipt(capture, source, source->len, SHADERLAB_SOURCE_SYNTAX_ROUTING,
                          -1, subshader, pass, stage, SIZE_MAX);
}

static bool bounded_text(const char *text, size_t bound) {
    if (!text) return true;
    for (size_t index = 0; index <= bound; ++index)
        if (!text[index]) return true;
    return false;
}

static bool tags_bounded(const SerializedTagMap *tags) {
    if (!tags || tags->tag_count < 0 || tags->tag_count > 16 ||
        (tags->tag_count && !tags->tags)) return false;
    for (int index = 0; index < tags->tag_count; ++index)
        if (!tags->tags[index].key || !tags->tags[index].value ||
            !bounded_text(tags->tags[index].key, 255) || !bounded_text(tags->tags[index].value, 1024))
            return false;
    return true;
}

static ShaderLabSourceQualityStatus validate_scope(const ShaderLabSourceQualityRequest *request) {
    if (!request || !request->shader || !request->archive ||
        (request->object && request->shader != &request->object->shader))
        return SHADERLAB_SOURCE_QUALITY_INVALID_ARGUMENT;
    const SerializedShader *shader = request->shader;
    if (!shader->name || !bounded_text(shader->name, 1024) ||
        shader->property_count < 0 || shader->property_count > 32 ||
        (shader->property_count && !shader->properties) ||
        shader->subshader_count != 1 || !shader->subshaders ||
        shader->keyword_names.count < 0 || shader->keyword_names.count > 8 ||
        (shader->keyword_names.count && (!shader->keyword_names.keywords || !shader->keyword_flags)) ||
        shader->dependency_count || shader->custom_editor_for_render_pipeline_count ||
        (shader->fallback_name && shader->fallback_name[0]) ||
        (shader->custom_editor_name && shader->custom_editor_name[0]))
        return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    for (int index = 0; index < shader->property_count; ++index) {
        const ParsedShaderProperty *property = &shader->properties[index];
        if (property->type < 0 || property->type > 5 ||
            property->attribute_count || !property->name || !property->description ||
            !bounded_text(property->name, 255) || !bounded_text(property->description, 1024) ||
            !bounded_text(property->def_texture_name, 1024) ||
            (property->type == 4 && (!property->def_texture_name ||
                property->def_texture_dim < 1 || property->def_texture_dim > 6)))
            return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    }
    for (int index = 0; index < shader->keyword_names.count; ++index)
        if (!shader->keyword_names.keywords[index] ||
            !bounded_text(shader->keyword_names.keywords[index], 255))
            return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    const SerializedSubShader *subshader = &shader->subshaders[0];
    if (subshader->pass_count != 1 || !subshader->passes || !tags_bounded(&subshader->tags))
        return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    const SerializedPass *pass = &subshader->passes[0];
    if (pass->pass_type || (pass->use_name && pass->use_name[0]) ||
        pass->has_instancing_variant || pass->has_procedural_instancing_variant ||
        !tags_bounded(&pass->tags) || !bounded_text(pass->state.name, 1024))
        return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    bool tessellation_stage_present[2] = {false, false};
    for (int stage = 0; stage < 6; ++stage) {
        if (pass->subprogram_count[stage] < 0 || pass->subprogram_count[stage] > 32 ||
            (pass->subprogram_count[stage] && !pass->subprograms[stage]))
            return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
        for (int index = 0; index < pass->subprogram_count[stage]; ++index) {
            if (!serialized_pass_subprogram_is_platform(pass, stage, index, 4)) continue;
            if (stage == 5)
                return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
            if (stage == 3 || stage == 4) tessellation_stage_present[stage - 3] = true;
        }
    }
    /* This is a one-pass inventory of the linked generated route. A lone hull
     * or domain stage has no admitted linked tessellation route. Individual
     * contracts and variants are still validated by the shared stage emitter;
     * their quality is never supplied by these wrapper observations. */
    if (tessellation_stage_present[0] != tessellation_stage_present[1])
        return SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE;
    return SHADERLAB_SOURCE_QUALITY_OK;
}

static void hash_number(CommonSha256Context *hash, uint64_t number) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8; ++index) bytes[index] = (uint8_t)(number >> (index * 8u));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static void hash_text(CommonSha256Context *hash, const char *text) {
    hash_number(hash, text != NULL);
    const size_t size = text ? strlen(text) : 0;
    hash_number(hash, size);
    if (size) common_sha256_update(hash, text, size);
}

static void hash_float_value(CommonSha256Context *hash, const SerializedShaderFloatValue *value) {
    uint32_t bits;
    memcpy(&bits, &value->val, sizeof(bits));
    hash_number(hash, value->present);
    hash_number(hash, bits);
    hash_text(hash, value->name);
}

static void hash_render_state(CommonSha256Context *hash, const SerializedShaderState *state) {
    hash_text(hash, state->name);
    hash_number(hash, state->rtSeparateBlend);
    for (unsigned index = 0; index < 8; ++index) {
        const SerializedShaderRTBlendState *blend = &state->rtBlend[index];
        const SerializedShaderFloatValue *values[] = {&blend->srcBlend, &blend->destBlend,
            &blend->srcBlendAlpha, &blend->destBlendAlpha, &blend->blendOp, &blend->blendOpAlpha, &blend->colMask};
        for (unsigned field = 0; field < sizeof(values) / sizeof(values[0]); ++field)
            hash_float_value(hash, values[field]);
    }
    const SerializedShaderFloatValue *values[] = {&state->zClip, &state->zTest, &state->zWrite,
        &state->culling, &state->conservative, &state->offsetFactor, &state->offsetUnits, &state->alphaToMask,
        &state->stencilOp.pass, &state->stencilOp.fail, &state->stencilOp.zFail, &state->stencilOp.comp,
        &state->stencilOpFront.pass, &state->stencilOpFront.fail, &state->stencilOpFront.zFail, &state->stencilOpFront.comp,
        &state->stencilOpBack.pass, &state->stencilOpBack.fail, &state->stencilOpBack.zFail, &state->stencilOpBack.comp,
        &state->stencilReadMask, &state->stencilWriteMask, &state->stencilRef,
        &state->fogStart, &state->fogEnd, &state->fogDensity, &state->fogColor.x,
        &state->fogColor.y, &state->fogColor.z, &state->fogColor.w};
    for (unsigned field = 0; field < sizeof(values) / sizeof(values[0]); ++field)
        hash_float_value(hash, values[field]);
    hash_text(hash, state->fogColor.name);
    hash_number(hash, (uint64_t)state->fogMode);
    hash_number(hash, (uint64_t)state->lod);
    hash_number(hash, state->lighting);
    /* gpuProgramID is compiler identity, not wrapper source-quality evidence. */
}

static void model_digest(const ShaderLabSourceQualityRequest *request,
                         ShaderLabSourceQualityInventory *inventory) {
    static const char domain[] = "DXBCSandbox.ShaderLabSourceQuality.SelectedModel.v1";
    CommonSha256Context hash;
    common_sha256_init(&hash);
    common_sha256_update(&hash, domain, sizeof(domain));
    common_sha256_update(&hash, inventory->source_digest, sizeof(inventory->source_digest));
    const SerializedShader *shader = request->shader;
    hash_number(&hash, inventory->has_structural_authority);
    if (request->object) hash_number(&hash, request->object->profile);
    hash_number(&hash, shader->property_count);
    for (int index = 0; index < shader->property_count; ++index) {
        const ParsedShaderProperty *property = &shader->properties[index];
        hash_text(&hash, property->name);
        hash_text(&hash, property->description);
        hash_number(&hash, (uint64_t)property->type);
        hash_number(&hash, property->flags);
        for (unsigned component = 0; component < 4; ++component) {
            uint32_t bits;
            memcpy(&bits, &property->def_value[component], sizeof(bits));
            hash_number(&hash, bits);
        }
        hash_text(&hash, property->def_texture_name);
        hash_number(&hash, (uint64_t)property->def_texture_dim);
    }
    hash_number(&hash, shader->keyword_names.count);
    for (int index = 0; index < shader->keyword_names.count; ++index) {
        hash_text(&hash, shader->keyword_names.keywords[index]);
        hash_number(&hash, shader->keyword_flags[index]);
    }
    const SerializedPass *pass = &shader->subshaders[0].passes[0];
    hash_render_state(&hash, &pass->state);
    hash_number(&hash, pass->program_mask);
    hash_number(&hash, pass->serialized_keyword_state_mask_count);
    for (int index = 0; index < pass->serialized_keyword_state_mask_count; ++index)
        hash_number(&hash, pass->serialized_keyword_state_mask[index]);
    hash_number(&hash, inventory->entries.count);
    for (size_t index = 0; index < inventory->entries.count; ++index) {
        const ShaderLabExpressionSourceRecord *entry = &inventory->entries.records[index];
        hash_number(&hash, entry->subshader_index);
        hash_number(&hash, entry->pass_index);
        hash_number(&hash, entry->stage_index);
        hash_number(&hash, entry->subprogram_index);
        hash_number(&hash, entry->blob_index);
        hash_number(&hash, entry->hardware_tier_group);
        hash_number(&hash, entry->serialized_state);
        common_sha256_update(&hash, entry->target_digest, sizeof(entry->target_digest));
        const SerializedSubProgram *program = &pass->subprograms[entry->stage_index][entry->subprogram_index];
        hash_number(&hash, program->program_type);
        hash_number(&hash, program->shader_requirements);
    }
    common_sha256_final(&hash, inventory->modeled_input_digest);
}

static bool summarize_inventory(ShaderLabSourceQualityInventory *inventory) {
    ShaderLabSourceQualityResult result = {0};
    result.classification = HLSL_SOURCE_QUALITY_MIXED;
    result.reasons = HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE;
    result.gaps = SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE | SHADERLAB_SOURCE_GAP_DEPENDENCY_INVENTORY;
    /* The pass emitted one literal UnityShaderVariables include. Native
     * implicit roots and the transitive closure are not inventoried by this
     * source-only request; SDK convention cannot supply their authority. */
    result.required_external_include_root_count = 1;
    if (!inventory->has_structural_authority) result.gaps |= SHADERLAB_SOURCE_GAP_SCHEMA_AUTHORITY;
    bool seen[MAX_ENTRY_RECORDS] = {false};
    size_t cursor = 0;
    for (size_t index = 0; index < inventory->receipt_count; ++index) {
        const ShaderLabSourceSyntaxReceipt *receipt = &inventory->receipts[index];
        if (receipt->source_begin != cursor || receipt->source_end <= cursor ||
            receipt->source_end > inventory->source_size) return false;
        cursor = receipt->source_end;
        if (receipt->kind != SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY) {
            ++result.wrapper_receipt_count;
            continue;
        }
        if (receipt->entry_record_index >= inventory->entries.count || seen[receipt->entry_record_index]) return false;
        const ShaderLabExpressionSourceRecord *entry = &inventory->entries.records[receipt->entry_record_index];
        if (receipt->subshader_index != entry->subshader_index || receipt->pass_index != entry->pass_index ||
            receipt->stage_index != entry->stage_index) return false;
        seen[receipt->entry_record_index] = true;
        ++result.linked_entry_count;
        if (!entry->has_source_quality || entry->source_quality.classification > HLSL_SOURCE_QUALITY_FAILED) {
            result.gaps |= SHADERLAB_SOURCE_GAP_STAGE_COVERAGE;
            continue;
        }
        const HLSLSourceQualityResult *quality = &entry->source_quality;
        ++result.stage_class_counts[quality->classification];
        result.observed_stage_residual_total += quality->counts.residual_total;
        result.observed_stage_unknown_provenance += quality->counts.unknown_provenance;
        result.observed_stage_incomplete_units += quality->counts.incomplete_units;
        result.reasons |= quality->reasons;
        if (quality->counts.incomplete_units) result.gaps |= SHADERLAB_SOURCE_GAP_STAGE_COVERAGE;
        if (quality->classification == HLSL_SOURCE_QUALITY_FAILED ||
            quality->classification == HLSL_SOURCE_QUALITY_UNSUPPORTED) return false;
    }
    if (cursor != inventory->source_size || result.linked_entry_count != inventory->entries.count ||
        !result.linked_entry_count) return false;
    result.wrapper_complete = true;
    inventory->quality = result;
    return true;
}

static ShaderLabSourceQualityStatus emit_inventory(const ShaderLabSourceQualityRequest *request,
    StringBuilder *source, ShaderLabSourceQualityInventory *inventory,
    ShaderLabSourceQualityDiagnostic *diagnostic) {
    ShaderLabSourceQualityStatus status = validate_scope(request);
    if (status != SHADERLAB_SOURCE_QUALITY_OK) return status;
    if (request->object) {
        if (shaderlab_structural_certify(request->object, &inventory->structure) != SHADERLAB_STRUCTURE_OK) {
            diagnostic->structure = inventory->structure;
            return SHADERLAB_SOURCE_QUALITY_STRUCTURE_FAILED;
        }
        inventory->has_structural_authority = true;
    }
    ShaderLabSourceQualityCapture capture = {.inventory = inventory};
    if (!shaderlab_emit_high_level_candidate_inventory(request->shader, request->archive, source,
            &inventory->entries, &capture, &diagnostic->emission))
        return capture.failed ? SHADERLAB_SOURCE_QUALITY_ALLOCATION_FAILED : SHADERLAB_SOURCE_QUALITY_EMISSION_FAILED;
    if (source->len > MAX_SOURCE_BYTES || capture.cursor != source->len ||
        inventory->entries.count > MAX_ENTRY_RECORDS || capture.body_count != inventory->entries.count)
        return SHADERLAB_SOURCE_QUALITY_INVENTORY_MISMATCH;
    inventory->source_size = source->len;
    common_sha256(source->buf, source->len, inventory->source_digest);
    model_digest(request, inventory);
    if (!summarize_inventory(inventory)) return SHADERLAB_SOURCE_QUALITY_INVENTORY_MISMATCH;
    for (size_t index = 0; request->observer && index < inventory->receipt_count; ++index)
        if (!request->observer(request->observer_context, &inventory->receipts[index]))
            return SHADERLAB_SOURCE_QUALITY_OBSERVER_REJECTED;
    inventory->complete = true;
    return SHADERLAB_SOURCE_QUALITY_OK;
}

ShaderLabSourceQualityStatus shaderlab_source_quality_emit(const ShaderLabSourceQualityRequest *request,
    StringBuilder *source, ShaderLabSourceQualityInventory *destination,
    ShaderLabSourceQualityDiagnostic *diagnostic) {
    ShaderLabSourceQualityDiagnostic local = {0};
    if (!diagnostic) diagnostic = &local;
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (!source || !sb_ok(source) || source->len || !destination) {
        diagnostic->status = SHADERLAB_SOURCE_QUALITY_INVALID_ARGUMENT;
        return diagnostic->status;
    }
    StringBuilder generated;
    sb_init(&generated);
    ShaderLabSourceQualityInventory inventory = {0};
    ShaderLabSourceQualityStatus status = emit_inventory(request, &generated, &inventory, diagnostic);
    if (status == SHADERLAB_SOURCE_QUALITY_OK) {
        /* Transfer the complete builder, avoiding a late append failure after
         * the inventory's already successful transaction. */
        sb_free(source);
        *source = generated;
        memset(&generated, 0, sizeof(generated));
        shaderlab_source_quality_inventory_dispose(destination);
        *destination = inventory;
        memset(&inventory, 0, sizeof(inventory));
    }
    sb_free(&generated);
    shaderlab_source_quality_inventory_dispose(&inventory);
    diagnostic->status = status;
    return status;
}

static bool receipts_equal(const ShaderLabSourceSyntaxReceipt *a, const ShaderLabSourceSyntaxReceipt *b) {
    return a->kind == b->kind && a->source_begin == b->source_begin && a->source_end == b->source_end &&
        a->property_index == b->property_index && a->subshader_index == b->subshader_index &&
        a->pass_index == b->pass_index && a->stage_index == b->stage_index &&
        a->entry_record_index == b->entry_record_index && !memcmp(a->source_digest, b->source_digest, 32);
}

static bool stage_counters_equal(const HLSLSourceQualityCounters *a, const HLSLSourceQualityCounters *b) {
    return a->ast_expressions == b->ast_expressions &&
        a->ast_statements == b->ast_statements &&
        a->emission_events == b->emission_events &&
        a->inspected_units == b->inspected_units &&
        a->incomplete_units == b->incomplete_units &&
        a->unknown_provenance == b->unknown_provenance &&
        a->logical_operations == b->logical_operations &&
        a->logical_value_references == b->logical_value_references &&
        a->semantic_projections == b->semantic_projections &&
        a->real_bitcasts == b->real_bitcasts &&
        a->register_storage == b->register_storage &&
        a->lane_transport == b->lane_transport &&
        a->scalarized_intrinsics == b->scalarized_intrinsics &&
        a->raw_buffer_reconstruction == b->raw_buffer_reconstruction &&
        a->synthetic_interface == b->synthetic_interface &&
        a->instruction_assignments == b->instruction_assignments &&
        a->unstructured_control == b->unstructured_control &&
        a->storage_bitcasts == b->storage_bitcasts &&
        a->sibling_declarations == b->sibling_declarations &&
        a->sibling_declaration_witnesses == b->sibling_declaration_witnesses &&
        a->resource_declarations == b->resource_declarations &&
        a->residual_total == b->residual_total &&
        a->cbuffer_declarations == b->cbuffer_declarations &&
        a->cbuffer_fields == b->cbuffer_fields;
}

static bool stage_facts_equal(const HLSLSourceQualityFacts *a, const HLSLSourceQualityFacts *b) {
    return a->known == b->known &&
        a->value_kind == b->value_kind &&
        a->logical_value_id == b->logical_value_id &&
        a->components == b->components &&
        a->artifacts == b->artifacts &&
        a->semantic_projection == b->semantic_projection &&
        a->real_bitcast == b->real_bitcast &&
        a->logical_operation == b->logical_operation &&
        a->instruction_index == b->instruction_index &&
        a->source_instruction_index == b->source_instruction_index &&
        a->lanes == b->lanes &&
        a->declaration_witness_count == b->declaration_witness_count &&
        a->declaration_variant_index == b->declaration_variant_index &&
        a->declaration_witness_record == b->declaration_witness_record &&
        a->declaration_field_index == b->declaration_field_index &&
        a->declaration_witness_subprogram_index == b->declaration_witness_subprogram_index &&
        a->resource_declaration_kind == b->resource_declaration_kind &&
        a->resource_binding_register == b->resource_binding_register &&
        a->cbuffer_declaration_kind == b->cbuffer_declaration_kind &&
        a->cbuffer_binding_register == b->cbuffer_binding_register &&
        a->cbuffer_field_index == b->cbuffer_field_index &&
        a->cbuffer_byte_offset == b->cbuffer_byte_offset &&
        a->cbuffer_byte_size == b->cbuffer_byte_size &&
        a->cbuffer_declaration_authority == b->cbuffer_declaration_authority;
}

static bool stage_observations_equal(const HLSLSourceQualityObservation *a, const HLSLSourceQualityObservation *b) {
    return a->stage == b->stage &&
        a->pass_index == b->pass_index &&
        a->entry_point_index == b->entry_point_index &&
        a->source_unit_id == b->source_unit_id &&
        a->unit_kind == b->unit_kind &&
        a->kind == b->kind &&
        a->ast_kind == b->ast_kind &&
        a->reasons == b->reasons &&
        stage_facts_equal(&a->facts, &b->facts);
}

static bool stage_quality_equal(const HLSLSourceQualityResult *a, const HLSLSourceQualityResult *b) {
    return a->stage == b->stage &&
        a->pass_index == b->pass_index &&
        a->entry_point_index == b->entry_point_index &&
        a->emission_status == b->emission_status &&
        a->classification == b->classification &&
        a->reasons == b->reasons &&
        a->has_first_issue == b->has_first_issue &&
        stage_counters_equal(&a->counts, &b->counts) &&
        (!a->has_first_issue || stage_observations_equal(&a->first_issue, &b->first_issue));
}

static bool instruction_origins_equal(const HLSLExpressionOrigin *a, const HLSLExpressionOrigin *b) {
    return a->kind == b->kind &&
        a->instruction_index == b->instruction_index &&
        a->source_instruction_index == b->source_instruction_index &&
        a->destination_lanes == b->destination_lanes &&
        a->source_begin == b->source_begin &&
        a->source_end == b->source_end &&
        a->definition_begin == b->definition_begin &&
        a->definition_end == b->definition_end;
}

static bool instruction_maps_equal(const HLSLExpressionSourceMap *a, const HLSLExpressionSourceMap *b) {
    if (a->count != b->count || a->complete != b->complete || a->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT)
        return false;
    for (size_t index = 0; index < a->count; ++index)
        if (!instruction_origins_equal(&a->origins[index], &b->origins[index])) return false;
    return true;
}

static bool entries_equal(const ShaderLabExpressionSourceRecord *a, const ShaderLabExpressionSourceRecord *b) {
    /* Compare typed fields, not C padding, pointers or borrowed AST nodes. */
    return a->subshader_index == b->subshader_index && a->pass_index == b->pass_index &&
        a->stage_index == b->stage_index && a->subprogram_index == b->subprogram_index &&
        a->blob_index == b->blob_index && a->hardware_tier_group == b->hardware_tier_group &&
        a->serialized_state == b->serialized_state && !memcmp(a->target_digest, b->target_digest, 32) &&
        a->has_source_quality == b->has_source_quality &&
        stage_quality_equal(&a->source_quality, &b->source_quality) &&
        instruction_maps_equal(&a->instructions, &b->instructions);
}

static bool summaries_equal(const ShaderLabSourceQualityResult *a, const ShaderLabSourceQualityResult *b) {
    if (a->classification != b->classification || a->reasons != b->reasons || a->gaps != b->gaps ||
        a->wrapper_complete != b->wrapper_complete || a->wrapper_receipt_count != b->wrapper_receipt_count ||
        a->linked_entry_count != b->linked_entry_count ||
        a->observed_stage_residual_total != b->observed_stage_residual_total ||
        a->observed_stage_unknown_provenance != b->observed_stage_unknown_provenance ||
        a->observed_stage_incomplete_units != b->observed_stage_incomplete_units ||
        a->required_external_include_root_count != b->required_external_include_root_count) return false;
    for (unsigned index = 0; index < 5; ++index)
        if (a->stage_class_counts[index] != b->stage_class_counts[index]) return false;
    return true;
}

static bool structures_equal(const ShaderLabStructuralDiagnostic *a, const ShaderLabStructuralDiagnostic *b) {
    return a->status == b->status && a->field == b->field && a->property_index == b->property_index &&
        a->subshader_index == b->subshader_index && a->pass_index == b->pass_index &&
        a->element_index == b->element_index && a->covered_semantic_fields == b->covered_semantic_fields &&
        a->excluded_compiled_fields == b->excluded_compiled_fields &&
        a->runtime_selection_certified == b->runtime_selection_certified &&
        a->visual_output_certified == b->visual_output_certified;
}

ShaderLabSourceQualityStatus shaderlab_source_quality_inventory_analyze(
    const ShaderLabSourceQualityRequest *request, const StringBuilder *source,
    const ShaderLabSourceQualityInventory *inventory, ShaderLabSourceQualityResult *result,
    ShaderLabSourceQualityDiagnostic *diagnostic) {
    ShaderLabSourceQualityDiagnostic local = {0};
    if (!diagnostic) diagnostic = &local;
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (!source || !sb_ok(source) || !source->buf || !inventory || !result ||
        !inventory->complete || inventory->receipt_count > inventory->receipt_capacity ||
        inventory->receipt_count > MAX_RECEIPTS || !inventory->receipts ||
        inventory->entries.count > MAX_ENTRY_RECORDS || inventory->entries.count > inventory->entries.capacity ||
        !inventory->entries.records) {
        diagnostic->status = SHADERLAB_SOURCE_QUALITY_INVALID_ARGUMENT;
        return diagnostic->status;
    }
    StringBuilder expected;
    sb_init(&expected);
    ShaderLabSourceQualityInventory canonical = {0};
    ShaderLabSourceQualityStatus status = emit_inventory(request, &expected, &canonical, diagnostic);
    if (status != SHADERLAB_SOURCE_QUALITY_OK) goto cleanup;
    status = SHADERLAB_SOURCE_QUALITY_INVENTORY_MISMATCH;
    if (source->len != expected.len || memcmp(source->buf, expected.buf, expected.len) ||
        inventory->source_size != canonical.source_size || inventory->receipt_count != canonical.receipt_count ||
        inventory->entries.count != canonical.entries.count ||
        !shaderlab_expression_source_map_matches_source(&inventory->entries, source) ||
        memcmp(inventory->source_digest, canonical.source_digest, 32) ||
        memcmp(inventory->modeled_input_digest, canonical.modeled_input_digest, 32) ||
        inventory->has_structural_authority != canonical.has_structural_authority ||
        !summaries_equal(&inventory->quality, &canonical.quality) ||
        !structures_equal(&inventory->structure, &canonical.structure))
        goto cleanup;
    for (size_t index = 0; index < canonical.receipt_count; ++index)
        if (!receipts_equal(&inventory->receipts[index], &canonical.receipts[index])) goto cleanup;
    for (size_t index = 0; index < canonical.entries.count; ++index)
        if (!entries_equal(&inventory->entries.records[index], &canonical.entries.records[index])) goto cleanup;
    *result = canonical.quality;
    status = SHADERLAB_SOURCE_QUALITY_OK;
cleanup:
    sb_free(&expected);
    shaderlab_source_quality_inventory_dispose(&canonical);
    diagnostic->status = status;
    return status;
}
