// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitted_hull_coverage_internal.h"
#include "translation/shaderlab_source_quality_internal.h"
#include "translation/hlsl_source_quality_internal.h"
#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

static void entry_dispose(HLSLHullCoverageCapture *entry) {
    hlsl_owned_stage_inputs_dispose(&entry->inputs);
    hlsl_stage_coverage_dispose(&entry->coverage);
    free(entry->placement.roots);
    free(entry->placement.syntax_ends);
    memset(entry, 0, sizeof(*entry));
}

void shaderlab_emitted_hull_coverage_free(ShaderLabEmittedHullCoverage *owned) {
    if (!owned) return;
    for (size_t index = 0; index < owned->entry_count; ++index) entry_dispose(&owned->entries[index]);
    free(owned->entries);
    shaderlab_source_quality_inventory_dispose(&owned->inventory);
    sb_free(&owned->source);
    free(owned);
}

bool shaderlab_hull_coverage_begin(ShaderLabEmittedHullCoverage *owned,
    const ShaderLabExpressionSourceRecord *record, size_t record_index,
    const uint8_t *target, size_t target_size, const uint8_t *payload, size_t payload_size,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    HLSLHullCoverageCapture **capture) {
    if (!owned || !owned->entries || owned->sealed || !record || record->stage_index != 3 ||
        record_index >= HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT ||
        owned->entry_count >= HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT || !capture || *capture) return false;
    HLSLHullCoverageCapture *entry = &owned->entries[owned->entry_count];
    if (!hlsl_owned_stage_inputs_capture(&entry->inputs, target, target_size, payload,
            payload_size, current, common, &owned->owned_input_bytes)) return false;
    ++owned->entry_count;
    entry->observation = (ShaderLabEmittedHullEntry){
        .subshader_index = record->subshader_index, .pass_index = record->pass_index,
        .stage_index = record->stage_index, .subprogram_index = record->subprogram_index,
        .blob_index = record->blob_index, .hardware_tier_group = record->hardware_tier_group,
        .serialized_state = record->serialized_state, .entry_record_index = record_index};
    common_sha256(entry->inputs.target, entry->inputs.target_size, entry->observation.target_digest);
    entry->coverage.global_node_count = &owned->owned_stage_node_count;
    entry->coverage.global_event_count = &owned->owned_stage_event_count;
    *capture = entry;
    return true;
}

bool shaderlab_hull_coverage_finish(HLSLHullCoverageCapture *entry,
    const StringBuilder *source, const HLSLExpressionSourceMap *map) {
    if (!entry || entry->finished || !map || !map->complete ||
        map->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT || map->count != entry->coverage.instruction_count ||
        !hlsl_stage_coverage_validate(&entry->coverage, source) ||
        entry->coverage.schema != HLSL_STAGE_COVERAGE_HULL_FORK_THREE_UNIT ||
        entry->coverage.unit_count != 3) return false;
    entry->raw_map = *map;
    HLSLHullWholePlacement *placement = &entry->placement;
    placement->root_count = entry->coverage.root_count;
    placement->unit_count = entry->coverage.unit_count;
    placement->syntax_count = entry->coverage.syntax_count;
    placement->has_icb_declaration = entry->coverage.hull_icb.plan.present;
    if (placement->has_icb_declaration)
        placement->icb_declaration = (HLSLHullWholeRange){SIZE_MAX, SIZE_MAX};
    if (placement->root_count) {
        placement->roots = malloc(placement->root_count * sizeof(*placement->roots));
        if (!placement->roots) return false;
        for (size_t index = 0; index < placement->root_count; ++index)
            placement->roots[index] = (HLSLHullWholeRange){SIZE_MAX, SIZE_MAX};
    }
    if (placement->syntax_count) {
        placement->syntax_ends = malloc(placement->syntax_count * sizeof(*placement->syntax_ends));
        if (!placement->syntax_ends) return false;
        for (size_t index = 0; index < placement->syntax_count; ++index)
            placement->syntax_ends[index] = SIZE_MAX;
    }
    for (size_t index = 0; index < placement->unit_count; ++index)
        placement->units[index] = (HLSLHullWholeRange){SIZE_MAX, SIZE_MAX};
    entry->observation.unit_count = placement->unit_count;
    entry->observation.root_count = placement->root_count;
    entry->observation.syntax_count = placement->syntax_count;
    entry->observation.obligations = entry->coverage.obligations;
    entry->finished = true;
    return true;
}

static void rebase_range(HLSLHullWholeRange *range, size_t begin, size_t end,
    size_t line_begin, size_t line_end, size_t output_begin) {
    /* End-at-newline precedes the following indentation. A range start at
     * that same local coordinate belongs to the following line instead. */
    if (begin >= line_begin && begin < line_end)
        range->begin = output_begin + begin - line_begin;
    if (end > line_begin && end <= line_end)
        range->end = output_begin + end - line_begin;
}

bool shaderlab_hull_coverage_rebase_line(HLSLHullCoverageCapture *entry,
    size_t line_begin, size_t line_end, size_t output_begin) {
    if (!entry) return true;
    HLSLHullWholePlacement *placement = &entry->placement;
    const HLSLStageCoverage *coverage = &entry->coverage;
    if (!entry->finished || placement->offset || placement->rebased ||
        line_begin != placement->line_cursor || line_begin >= line_end ||
        line_end > coverage->source_size || output_begin > SIZE_MAX - (line_end - line_begin) ||
        placement->root_count != coverage->root_count || placement->unit_count != coverage->unit_count ||
        placement->syntax_count != coverage->syntax_count || placement->unit_count != 3 ||
        placement->has_icb_declaration != coverage->hull_icb.plan.present ||
        placement->root_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
        placement->syntax_count > HLSL_STAGE_COVERAGE_EVENT_LIMIT ||
        (placement->root_count && (!placement->roots || !coverage->roots)) ||
        (placement->syntax_count && (!placement->syntax_ends || !coverage->syntax))) return false;
    for (size_t index = 0; index < placement->root_count; ++index)
        rebase_range(&placement->roots[index], coverage->roots[index].begin, coverage->roots[index].end,
            line_begin, line_end, output_begin);
    for (size_t index = 0; index < placement->unit_count; ++index)
        rebase_range(&placement->units[index], coverage->units[index].begin, coverage->units[index].end,
            line_begin, line_end, output_begin);
    if (placement->has_icb_declaration)
        rebase_range(&placement->icb_declaration, coverage->hull_icb.declaration_begin,
            coverage->hull_icb.declaration_end, line_begin, line_end, output_begin);
    for (size_t index = 0; index < placement->syntax_count; ++index) {
        const HLSLStageOwnedSyntax *syntax = &coverage->syntax[index];
        const size_t end = syntax->source_end;
        const bool at_unit_begin = syntax->source_unit_id < coverage->unit_count &&
            end == coverage->units[syntax->source_unit_id].begin;
        /* A unit-opening observation belongs after that unit's first line
         * indentation, even when the preceding unit ends at the same byte. */
        if ((at_unit_begin && end >= line_begin && end < line_end) ||
            (!at_unit_begin && end > line_begin && end <= line_end))
            placement->syntax_ends[index] = output_begin + end - line_begin;
    }
    placement->line_cursor = line_end;
    placement->rebased = line_end == coverage->source_size;
    return true;
}

static bool range_can_offset(const HLSLHullWholeRange *range, size_t offset) {
    return range->begin != SIZE_MAX && range->end != SIZE_MAX && range->begin < range->end &&
        range->end <= SIZE_MAX - offset;
}

bool shaderlab_hull_coverage_offset(ShaderLabEmittedHullCoverage *owned,
    size_t first_record, size_t offset) {
    if (!owned) return true;
    if (owned->sealed || owned->entry_count > HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT ||
        (owned->entry_count && !owned->entries)) return false;
    for (size_t index = 0; index < owned->entry_count; ++index) {
        const HLSLHullCoverageCapture *entry = &owned->entries[index];
        if (entry->observation.entry_record_index < first_record) continue;
        const HLSLHullWholePlacement *placement = &entry->placement;
        if (!entry->finished || !placement->rebased || placement->offset ||
            placement->root_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT ||
            placement->unit_count != 3 || placement->syntax_count > HLSL_STAGE_COVERAGE_EVENT_LIMIT ||
            (placement->root_count && !placement->roots) || (placement->syntax_count && !placement->syntax_ends))
            return false;
        for (size_t root = 0; root < placement->root_count; ++root)
            if (!range_can_offset(&placement->roots[root], offset)) return false;
        for (size_t unit = 0; unit < placement->unit_count; ++unit)
            if (!range_can_offset(&placement->units[unit], offset)) return false;
        if (placement->has_icb_declaration && !range_can_offset(&placement->icb_declaration, offset))
            return false;
        for (size_t syntax = 0; syntax < placement->syntax_count; ++syntax)
            if (placement->syntax_ends[syntax] == SIZE_MAX || placement->syntax_ends[syntax] > SIZE_MAX - offset)
                return false;
    }
    for (size_t index = 0; index < owned->entry_count; ++index) {
        HLSLHullCoverageCapture *entry = &owned->entries[index];
        if (entry->observation.entry_record_index < first_record) continue;
        HLSLHullWholePlacement *placement = &entry->placement;
        for (size_t root = 0; root < placement->root_count; ++root) {
            placement->roots[root].begin += offset;
            placement->roots[root].end += offset;
        }
        for (size_t unit = 0; unit < placement->unit_count; ++unit) {
            placement->units[unit].begin += offset;
            placement->units[unit].end += offset;
        }
        if (placement->has_icb_declaration) {
            placement->icb_declaration.begin += offset;
            placement->icb_declaration.end += offset;
        }
        for (size_t syntax = 0; syntax < placement->syntax_count; ++syntax)
            placement->syntax_ends[syntax] += offset;
        placement->offset = true;
    }
    return true;
}

static bool coordinates_match(const ShaderLabEmittedHullEntry *entry,
    const ShaderLabExpressionSourceRecord *record) {
    return entry->subshader_index == record->subshader_index && entry->pass_index == record->pass_index &&
        entry->stage_index == record->stage_index && entry->subprogram_index == record->subprogram_index &&
        entry->blob_index == record->blob_index && entry->hardware_tier_group == record->hardware_tier_group &&
        entry->serialized_state == record->serialized_state && !memcmp(entry->target_digest, record->target_digest, 32);
}

static bool placements_valid(const HLSLHullCoverageCapture *entry, const StringBuilder *source,
    const ShaderLabSourceSyntaxReceipt *body) {
    const HLSLStageCoverage *coverage = &entry->coverage;
    const HLSLHullWholePlacement *placement = &entry->placement;
    const StringBuilder local = {.buf = coverage->source, .len = coverage->source_size,
        .capacity = coverage->source_size + 1};
    if (!hlsl_stage_coverage_validate(coverage, &local) || !placement->rebased || !placement->offset ||
        placement->line_cursor != coverage->source_size || placement->root_count != coverage->root_count ||
        placement->unit_count != coverage->unit_count || placement->unit_count != 3 ||
        placement->syntax_count != coverage->syntax_count ||
        placement->has_icb_declaration != coverage->hull_icb.plan.present ||
        (placement->root_count && !placement->roots) || (placement->syntax_count && !placement->syntax_ends)) return false;
    size_t previous = body->source_begin;
    for (size_t unit = 0; unit < placement->unit_count; ++unit) {
        const HLSLHullWholeRange *range = &placement->units[unit];
        if (range->begin < previous || range->begin >= range->end || range->end > body->source_end) return false;
        previous = range->end;
    }
    if (placement->has_icb_declaration) {
        const HLSLHullWholeRange *declaration = &placement->icb_declaration;
        if (declaration->begin < placement->units[0].begin || declaration->begin >= declaration->end ||
            declaration->end > placement->units[0].end) return false;
    } else if (placement->icb_declaration.begin || placement->icb_declaration.end) return false;
    for (size_t index = 0; index < placement->root_count; ++index) {
        const HLSLStageOwnedRoot *root = &coverage->roots[index];
        const HLSLHullWholeRange *range = &placement->roots[index];
        if (root->source_unit_id >= placement->unit_count ||
            range->begin < placement->units[root->source_unit_id].begin ||
            range->end > placement->units[root->source_unit_id].end || range->begin >= range->end ||
            range->end - range->begin != root->end - root->begin ||
            memcmp(source->buf + range->begin, coverage->source + root->begin, root->end - root->begin)) return false;
    }
    previous = body->source_begin;
    for (size_t index = 0; index < placement->syntax_count; ++index) {
        const uint32_t unit = coverage->syntax[index].source_unit_id;
        const size_t end = placement->syntax_ends[index];
        if (unit >= placement->unit_count || end < previous || end < placement->units[unit].begin ||
            end > placement->units[unit].end) return false;
        previous = end;
    }
    return true;
}

static bool seal(ShaderLabEmittedHullCoverage *owned) {
    if (!owned || owned->sealed || !owned->inventory.complete || !sb_ok(&owned->source) ||
        !owned->entries || !owned->source.buf || !owned->inventory.entries.records ||
        owned->inventory.entries.count > HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT ||
        owned->inventory.entries.count > owned->inventory.entries.capacity ||
        owned->inventory.receipt_count > owned->inventory.receipt_capacity || !owned->inventory.receipts ||
        !owned->entry_count || owned->entry_count > HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT) return false;
    bool seen[HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT] = {false};
    size_t hull_count = 0;
    for (size_t index = 0; index < owned->inventory.entries.count; ++index)
        if (owned->inventory.entries.records[index].stage_index == 3) ++hull_count;
    if (hull_count != owned->entry_count) return false;
    for (size_t index = 0; index < owned->entry_count; ++index) {
        HLSLHullCoverageCapture *entry = &owned->entries[index];
        const size_t ordinal = entry->observation.entry_record_index;
        if (!entry->finished || ordinal >= owned->inventory.entries.count ||
            ordinal >= HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT || seen[ordinal]) return false;
        seen[ordinal] = true;
        const ShaderLabExpressionSourceRecord *record = &owned->inventory.entries.records[ordinal];
        if (!coordinates_match(&entry->observation, record) || !record->has_source_quality ||
            !entry->raw_map.complete || entry->raw_map.count != record->instructions.count) return false;
        const ShaderLabSourceSyntaxReceipt *body = NULL;
        for (size_t receipt = 0; receipt < owned->inventory.receipt_count; ++receipt) {
            const ShaderLabSourceSyntaxReceipt *candidate = &owned->inventory.receipts[receipt];
            if (candidate->kind != SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY || candidate->entry_record_index != ordinal) continue;
            if (body) return false;
            body = candidate;
        }
        if (!body || body->source_begin >= body->source_end || body->source_end > owned->source.len ||
            body->subshader_index != entry->observation.subshader_index ||
            body->pass_index != entry->observation.pass_index || body->stage_index != 3 ||
            !placements_valid(entry, &owned->source, body)) return false;
        entry->observation.source_begin = body->source_begin;
        entry->observation.source_end = body->source_end;
        memcpy(entry->observation.body_digest, body->source_digest, 32);
        entry->observation.base_quality = record->source_quality;
    }
    owned->sealed = true;
    return true;
}

static bool initial_scope(const ShaderLabSourceQualityRequest *request) {
    if (!request || !request->shader || !request->archive) return false;
    const SerializedShader *shader = request->shader;
    if (shader->subshader_count != 1 || !shader->subshaders ||
        shader->subshaders[0].pass_count != 1 || !shader->subshaders[0].passes) return false;
    const SerializedPass *pass = &shader->subshaders[0].passes[0];
    return pass->subprogram_count[0] > 0 && pass->subprogram_count[1] > 0 &&
        pass->subprogram_count[3] > 0 && pass->subprogram_count[4] > 0 &&
        !pass->subprogram_count[5];
}

static ShaderLabHullCoverageStatus capture_once(const ShaderLabSourceQualityRequest *request,
    ShaderLabEmittedHullCoverage **output) {
    if (!initial_scope(request)) return SHADERLAB_HULL_COVERAGE_SCOPE_UNAVAILABLE;
    ShaderLabEmittedHullCoverage *owned = calloc(1, sizeof(*owned));
    if (!owned) return SHADERLAB_HULL_COVERAGE_ALLOCATION_FAILED;
    sb_init(&owned->source);
    owned->entries = calloc(HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT, sizeof(*owned->entries));
    if (!owned->entries) {
        shaderlab_emitted_hull_coverage_free(owned);
        return SHADERLAB_HULL_COVERAGE_ALLOCATION_FAILED;
    }
    const ShaderLabSourceQualityStatus status = shaderlab_source_quality_emit_with_hull_capture(
        request, &owned->source, &owned->inventory, owned, NULL);
    if (status != SHADERLAB_SOURCE_QUALITY_OK || !seal(owned)) {
        shaderlab_emitted_hull_coverage_free(owned);
        return status == SHADERLAB_SOURCE_QUALITY_OK ? SHADERLAB_HULL_COVERAGE_CAPTURE_FAILED :
            status == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE ? SHADERLAB_HULL_COVERAGE_SCOPE_UNAVAILABLE :
            status == SHADERLAB_SOURCE_QUALITY_ALLOCATION_FAILED ? SHADERLAB_HULL_COVERAGE_ALLOCATION_FAILED :
            SHADERLAB_HULL_COVERAGE_EMISSION_FAILED;
    }
    *output = owned;
    return SHADERLAB_HULL_COVERAGE_OK;
}

static bool maps_equal(const HLSLExpressionSourceMap *a, const HLSLExpressionSourceMap *b) {
    if (!a->complete || !b->complete || a->count != b->count || a->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT) return false;
    for (size_t index = 0; index < a->count; ++index)
        if (!hlsl_expression_origins_equal(&a->origins[index], &b->origins[index])) return false;
    return true;
}

static bool placements_equal(const HLSLHullWholePlacement *a, const HLSLHullWholePlacement *b) {
    if (a->root_count != b->root_count || a->unit_count != b->unit_count || a->syntax_count != b->syntax_count ||
        a->line_cursor != b->line_cursor || !a->rebased || !b->rebased || !a->offset || !b->offset ||
        a->has_icb_declaration != b->has_icb_declaration ||
        a->icb_declaration.begin != b->icb_declaration.begin || a->icb_declaration.end != b->icb_declaration.end ||
        a->root_count > HLSL_STAGE_COVERAGE_ROOT_LIMIT || a->unit_count != 3 ||
        a->syntax_count > HLSL_STAGE_COVERAGE_EVENT_LIMIT ||
        (a->root_count && (!a->roots || !b->roots)) || (a->syntax_count && (!a->syntax_ends || !b->syntax_ends))) return false;
    for (size_t index = 0; index < a->root_count; ++index)
        if (a->roots[index].begin != b->roots[index].begin || a->roots[index].end != b->roots[index].end) return false;
    for (size_t index = 0; index < a->unit_count; ++index)
        if (a->units[index].begin != b->units[index].begin || a->units[index].end != b->units[index].end) return false;
    for (size_t index = 0; index < a->syntax_count; ++index)
        if (a->syntax_ends[index] != b->syntax_ends[index]) return false;
    return true;
}

static bool observations_equal(const ShaderLabEmittedHullCoverage *a, const ShaderLabEmittedHullCoverage *b) {
    if (!a->sealed || !b->sealed || a->entry_count != b->entry_count || !a->entry_count ||
        a->entry_count > HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT || !a->entries || !b->entries ||
        a->source.len != b->source.len || !a->source.buf || !b->source.buf ||
        a->owned_input_bytes != b->owned_input_bytes || a->owned_stage_node_count != b->owned_stage_node_count ||
        a->owned_stage_event_count != b->owned_stage_event_count || memcmp(a->source.buf, b->source.buf, a->source.len)) return false;
    for (size_t index = 0; index < a->entry_count; ++index) {
        const HLSLHullCoverageCapture *left = &a->entries[index], *right = &b->entries[index];
        const ShaderLabEmittedHullEntry *x = &left->observation, *y = &right->observation;
        if (!left->finished || !right->finished || x->entry_record_index != y->entry_record_index ||
            x->entry_record_index >= b->inventory.entries.count ||
            !coordinates_match(x, &b->inventory.entries.records[x->entry_record_index]) ||
            x->source_begin != y->source_begin || x->source_end != y->source_end ||
            x->unit_count != y->unit_count || x->root_count != y->root_count || x->syntax_count != y->syntax_count ||
            x->obligations != y->obligations || memcmp(x->body_digest, y->body_digest, 32) ||
            !hlsl_source_quality_results_equal(&x->base_quality, &y->base_quality) ||
            !hlsl_owned_stage_inputs_equal(&left->inputs, &right->inputs) ||
            !maps_equal(&left->raw_map, &right->raw_map) ||
            !hlsl_stage_coverage_equal(&left->coverage, &right->coverage) ||
            !placements_equal(&left->placement, &right->placement)) return false;
    }
    return true;
}

ShaderLabHullCoverageStatus shaderlab_emitted_hull_coverage_capture(
    const ShaderLabSourceQualityRequest *request, ShaderLabEmittedHullCoverage **output) {
    if (!output || *output || !request || !request->shader || !request->archive)
        return SHADERLAB_HULL_COVERAGE_INVALID_ARGUMENT;
    ShaderLabEmittedHullCoverage *owned = NULL;
    ShaderLabHullCoverageStatus status = capture_once(request, &owned);
    if (status != SHADERLAB_HULL_COVERAGE_OK) return status;
    /* Syntax observers run after ordinary stage emission. Independently rebuild
     * the current model afterward: a callback cannot publish stale input bytes,
     * copied metadata or coverage by changing the original request in place. */
    ShaderLabSourceQualityRequest stable = *request;
    stable.observer = NULL;
    stable.observer_context = NULL;
    ShaderLabSourceQualityResult quality;
    ShaderLabEmittedHullCoverage *current = NULL;
    if (shaderlab_source_quality_inventory_analyze(&stable, &owned->source, &owned->inventory, &quality, NULL) !=
            SHADERLAB_SOURCE_QUALITY_OK ||
        capture_once(&stable, &current) != SHADERLAB_HULL_COVERAGE_OK || !observations_equal(owned, current)) {
        status = SHADERLAB_HULL_COVERAGE_CAPTURE_FAILED;
        shaderlab_emitted_hull_coverage_free(owned);
    } else {
        *output = owned;
    }
    shaderlab_emitted_hull_coverage_free(current);
    return status;
}

bool shaderlab_emitted_hull_coverage_replay(
    const ShaderLabSourceQualityRequest *request, const ShaderLabEmittedHullCoverage *owned) {
    if (!request || !owned || !owned->sealed) return false;
    ShaderLabSourceQualityResult quality;
    if (shaderlab_source_quality_inventory_analyze(request, &owned->source, &owned->inventory, &quality, NULL) !=
        SHADERLAB_SOURCE_QUALITY_OK) return false;
    ShaderLabSourceQualityRequest stable = *request;
    stable.observer = NULL;
    stable.observer_context = NULL;
    ShaderLabEmittedHullCoverage *current = NULL;
    if (capture_once(&stable, &current) != SHADERLAB_HULL_COVERAGE_OK) return false;
    bool equal = observations_equal(owned, current);
    shaderlab_emitted_hull_coverage_free(current);
    return equal;
}

bool shaderlab_emitted_hull_coverage_describe(
    const ShaderLabEmittedHullCoverage *owned, ShaderLabEmittedHullSummary *summary) {
    if (!owned || !owned->sealed || !summary) return false;
    ShaderLabEmittedHullSummary result = {.source_size = owned->source.len, .entry_count = owned->entry_count,
        .linked_entry_count = owned->inventory.entries.count, .base_quality = owned->inventory.quality};
    memcpy(result.source_digest, owned->inventory.source_digest, 32);
    memcpy(result.modeled_input_digest, owned->inventory.modeled_input_digest, 32);
    for (size_t index = 0; index < owned->entry_count; ++index) {
        result.unit_count += owned->entries[index].coverage.unit_count;
        result.root_count += owned->entries[index].coverage.root_count;
        result.syntax_count += owned->entries[index].coverage.syntax_count;
    }
    *summary = result;
    return true;
}

bool shaderlab_emitted_hull_coverage_entry(
    const ShaderLabEmittedHullCoverage *owned, size_t index, ShaderLabEmittedHullEntry *entry) {
    if (!owned || !owned->sealed || !entry || index >= owned->entry_count) return false;
    *entry = owned->entries[index].observation;
    return true;
}

bool shaderlab_emitted_hull_coverage_source(
    const ShaderLabEmittedHullCoverage *owned, const char **source, size_t *size) {
    if (!owned || !owned->sealed || !source || !size) return false;
    *source = owned->source.buf;
    *size = owned->source.len;
    return true;
}
