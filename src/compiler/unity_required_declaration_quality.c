// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_required_declaration_quality_internal.h"
#include "compiler/unity_hlsl_cbuffer_layout_internal.h"
#include "translation/hlsl_emitter_internal.h"
#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

void unity_required_declaration_quality_init(UnityRequiredDeclarationQuality *owned) {
    memset(owned, 0, sizeof(*owned));
    unity_hlsl_cbuffer_inventory_init(&owned->inventory);
}

void unity_required_declaration_quality_dispose(UnityRequiredDeclarationQuality *owned) {
    if (!owned) return;
    unity_hlsl_cbuffer_inventory_dispose(&owned->inventory);
    memset(owned, 0, sizeof(*owned));
}

bool unity_required_declaration_quality_block(void *context, size_t block_index,
    const UnityHlslCBufferBlock *block, const UnityHlslCBufferField *fields, size_t field_count) {
    UnityRequiredDeclarationQuality *owned = context;
    if (!owned || owned->sealed || !block || !fields || !field_count ||
        field_count != block->field_count || block_index != owned->inventory.block_count ||
        block_index >= UNITY_HLSL_CBUFFER_COUNT_LIMIT ||
        owned->inventory.field_count > UNITY_HLSL_CBUFFER_FIELD_LIMIT ||
        field_count > UNITY_HLSL_CBUFFER_FIELD_LIMIT - owned->inventory.field_count ||
        !memchr(block->name, 0, sizeof(block->name)) || !block->name[0] ||
        !block->byte_size || block->byte_size > 65536 || (block->byte_size & 15u) ||
        block->source_begin >= block->source_end) return false;
    for (size_t prior = 0; prior < block_index; ++prior)
        if (!strcmp(owned->inventory.blocks[prior].name, block->name)) return false;
    uint32_t cursor = 0;
    size_t previous_end = block->source_begin;
    for (size_t index = 0; index < field_count; ++index) {
        const UnityHlslCBufferField *field = &fields[index];
        if (!memchr(field->name, 0, sizeof(field->name)) || !field->name[0] ||
            field->source_begin < previous_end || field->source_end <= field->source_begin ||
            field->source_end > block->source_end) return false;
        for (size_t prior = 0; prior < index; ++prior)
            if (!strcmp(fields[prior].name, field->name)) return false;
        UnityHlslCBufferField expected = *field;
        if (unity_hlsl_cbuffer_layout_field(&expected, &cursor) != UNITY_HLSL_CBUFFER_OK ||
            field->byte_offset != expected.byte_offset || field->byte_size != expected.byte_size ||
            cursor > block->byte_size) return false;
        previous_end = field->source_end;
    }
    uint32_t extent;
    if (unity_hlsl_cbuffer_layout_extent(cursor, &extent) != UNITY_HLSL_CBUFFER_OK ||
        extent != block->byte_size) return false;
    const size_t first = owned->inventory.field_count;
    UnityHlslCBufferField *copy = realloc(owned->inventory.fields, (first + field_count) * sizeof(*copy));
    if (!copy) return false;
    owned->inventory.fields = copy;
    memcpy(copy + first, fields, field_count * sizeof(*copy));
    owned->inventory.blocks[block_index] = *block;
    owned->inventory.blocks[block_index].first_field = first;
    owned->inventory.field_count += field_count;
    ++owned->inventory.block_count;
    return true;
}

static void hash_word(CommonSha256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8; ++index) bytes[index] = (uint8_t)(value >> (index * 8));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static bool snapshot_digest(const UnityRequiredDeclarationQuality *owned, uint8_t digest[32]) {
    if (!owned || owned->inventory.block_count > UNITY_HLSL_CBUFFER_COUNT_LIMIT ||
        owned->inventory.field_count > UNITY_HLSL_CBUFFER_FIELD_LIMIT ||
        (owned->inventory.field_count && !owned->inventory.fields)) return false;
    CommonSha256Context hash;
    common_sha256_init(&hash);
    hash_word(&hash, owned->inventory.block_count);
    hash_word(&hash, owned->inventory.field_count);
    for (size_t index = 0; index < owned->inventory.block_count; ++index) {
        const UnityHlslCBufferBlock *block = &owned->inventory.blocks[index];
        if (!memchr(block->name, 0, sizeof(block->name))) return false;
        common_sha256_update(&hash, block->name, strlen(block->name) + 1);
        hash_word(&hash, block->byte_size);
        hash_word(&hash, block->first_field);
        hash_word(&hash, block->field_count);
        hash_word(&hash, block->source_begin);
        hash_word(&hash, block->source_end);
    }
    for (size_t index = 0; index < owned->inventory.field_count; ++index) {
        const UnityHlslCBufferField *field = &owned->inventory.fields[index];
        if (!memchr(field->name, 0, sizeof(field->name))) return false;
        common_sha256_update(&hash, field->name, strlen(field->name) + 1);
        hash_word(&hash, field->scalar);
        hash_word(&hash, field->rows);
        hash_word(&hash, field->columns);
        hash_word(&hash, field->is_matrix);
        hash_word(&hash, field->byte_offset);
        hash_word(&hash, field->byte_size);
        hash_word(&hash, field->source_begin);
        hash_word(&hash, field->source_end);
    }
    common_sha256_final(&hash, digest);
    return true;
}

bool unity_required_declaration_quality_seal(UnityRequiredDeclarationQuality *owned) {
    if (!owned || owned->sealed || !snapshot_digest(owned, owned->snapshot_digest)) return false;
    owned->sealed = true;
    return true;
}

/* Required block selection uses actual binding/read owners. Declarations that
 * are already emitted locally cannot be substituted for missing local syntax. */
static bool select_required_blocks(const HLSLMatrixUseCapture *entry,
    const UnityRequiredDeclarationQuality *declarations, bool selected[UNITY_HLSL_CBUFFER_COUNT_LIMIT],
    size_t *selected_count) {
    uint32_t seen = 0;
    *selected_count = 0;
    for (size_t index = 0; index < declarations->inventory.block_count; ++index) {
        const UnityHlslCBufferBlock *block = &declarations->inventory.blocks[index];
        uint32_t bindings = 0;
        for (size_t owner_index = 0; owner_index < entry->reads.field_count; ++owner_index) {
            const HLSLCurrentMatrixField *owner = &entry->reads.fields[owner_index];
            if (owner->binding_register >= 15 || !(entry->coverage.required_binding_mask &
                (UINT32_C(1) << owner->binding_register)) || strcmp(owner->block_name, block->name)) continue;
            if (block->byte_size != owner->reflected_byte_size ||
                block->first_field > declarations->inventory.field_count ||
                block->field_count > declarations->inventory.field_count - block->first_field) return false;
            size_t matches = 0;
            for (size_t field_index = block->first_field; field_index < block->first_field + block->field_count; ++field_index) {
                const UnityHlslCBufferField *field = &declarations->inventory.fields[field_index];
                if (strcmp(field->name, owner->field_name)) continue;
                if (field->scalar != UNITY_HLSL_CBUFFER_FLOAT || !field->is_matrix ||
                    field->rows != 4 || field->columns != 4 || field->byte_offset != owner->field_byte_offset ||
                    field->byte_size != owner->field_byte_size) return false;
                ++matches;
            }
            if (matches != 1) return false;
            bindings |= UINT32_C(1) << owner->binding_register;
        }
        if (!bindings) continue;
        if ((bindings & (bindings - 1u)) || (bindings & seen)) return false;
        seen |= bindings;
        selected[index] = true;
        ++*selected_count;
    }
    return seen == entry->coverage.required_binding_mask;
}

bool unity_required_declaration_quality_analyze(const HLSLMatrixUseCapture *entry,
    const UnityRequiredDeclarationQuality *declarations, HLSLSourceQualityResult *result,
    uint32_t *remaining_obligations, HLSLSourceQualityObserver observer, void *observer_context) {
    if (remaining_obligations) *remaining_obligations = UINT32_MAX;
    if (result) {
        memset(result, 0, sizeof(*result));
        result->classification = HLSL_SOURCE_QUALITY_FAILED;
        result->reasons = HLSL_SOURCE_QUALITY_REASON_INVALID_INPUT;
    }
    if (!entry || !entry->finished || !result || !remaining_obligations || !declarations || !declarations->sealed)
        return false;
    uint8_t digest[32];
    if (!snapshot_digest(declarations, digest) || memcmp(digest, declarations->snapshot_digest, 32)) return false;
    const HLSLStageCoverage *coverage = &entry->coverage;
    StringBuilder source = {.buf = coverage->source, .len = coverage->source_size,
        .capacity = coverage->source_size + 1};
    if (!hlsl_stage_coverage_validate(coverage, &source)) return false;
    bool selected[UNITY_HLSL_CBUFFER_COUNT_LIMIT] = {false};
    size_t selected_count;
    if (!select_required_blocks(entry, declarations, selected, &selected_count)) return false;
    const uint32_t remaining = coverage->obligations & ~HLSL_STAGE_COVERAGE_REQUIRED_EXTERNAL_DECLARATION;
    const ASTExpr *roots[HLSL_STAGE_COVERAGE_ROOT_LIMIT];
    for (size_t index = 0; index < coverage->root_count; ++index) roots[index] = coverage->roots[index].tree;
    HLSLSourceQualityFacts *entry_facts = calloc(coverage->syntax_count, sizeof(*entry_facts));
    if (coverage->syntax_count && !entry_facts) return false;
    for (size_t index = 0; index < coverage->syntax_count; ++index) entry_facts[index] = coverage->syntax[index].facts;
    HLSLSourceQualityUnit units[UNITY_HLSL_CBUFFER_COUNT_LIMIT + 1] = {{
        .source_unit_id = 0, .kind = HLSL_SOURCE_UNIT_ENTRY_POINT, .coverage_complete = !remaining,
        .expressions = roots, .expression_count = coverage->root_count,
        .emission_facts = entry_facts, .emission_fact_count = coverage->syntax_count}};
    size_t unit_count = 1;
    for (size_t index = 0; index < declarations->inventory.block_count; ++index) {
        if (!selected[index]) continue;
        const UnityHlslCBufferBlock *block = &declarations->inventory.blocks[index];
        /* Neutral known syntax: external declaration origins are retained in
         * the typed receipt, never mislabeled as current/common authority. */
        HLSLSourceQualityFacts *facts = calloc(block->field_count + 2, sizeof(*facts));
        if (!facts) goto failure;
        for (size_t field = 0; field < block->field_count + 2; ++field) {
            hlsl_source_quality_facts_init(&facts[field]);
            facts[field].known = true;
        }
        units[unit_count] = (HLSLSourceQualityUnit){.source_unit_id = (uint32_t)unit_count,
            .kind = HLSL_SOURCE_UNIT_REQUIRED_EXTERNAL_DECLARATION, .coverage_complete = true,
            .emission_facts = facts, .emission_fact_count = block->field_count + 2};
        ++unit_count;
    }
    if (unit_count != selected_count + 1) goto failure;
    HLSLSourceQualityRequest request = {.stage = coverage->stage,
        .pass_index = (uint32_t)entry->observation.pass_index,
        .entry_point_index = (uint32_t)entry->observation.subprogram_index,
        .emission_status = HLSL_EMIT_STATUS_OK, .units = units, .unit_count = unit_count,
        .expected_unit_count = unit_count, .expected_entry_point_count = 1,
        .expression_facts = hlsl_source_quality_owned_expression_facts,
        .observer = observer, .observer_context = observer_context};
    const bool valid = hlsl_source_quality_analyze(&request, result);
    for (size_t index = 1; index < unit_count; ++index) free((void *)units[index].emission_facts);
    free(entry_facts);
    if (valid) *remaining_obligations = remaining;
    return valid;
failure:
    for (size_t index = 1; index < unit_count; ++index) free((void *)units[index].emission_facts);
    free(entry_facts);
    return false;
}
