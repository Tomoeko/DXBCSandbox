// SPDX-License-Identifier: GPL-3.0-only
#ifndef SHADERLAB_EMITTED_HULL_COVERAGE_INTERNAL_H
#define SHADERLAB_EMITTED_HULL_COVERAGE_INTERNAL_H

#include "translation/shaderlab_emitted_hull_coverage.h"
#include "translation/hlsl_owned_stage_inputs_internal.h"
#include "translation/hlsl_stage_coverage_internal.h"

typedef struct {
    size_t begin, end;
} HLSLHullWholeRange;

/* Local coverage coordinates remain immutable. These separate placements are
 * populated by the normal producer's line-copy and stage-offset operations. */
typedef struct {
    HLSLHullWholeRange *roots;
    HLSLHullWholeRange units[3];
    size_t *syntax_ends;
    size_t root_count, unit_count, syntax_count, line_cursor;
    bool rebased, offset;
} HLSLHullWholePlacement;

typedef struct HLSLHullCoverageCapture {
    ShaderLabEmittedHullEntry observation;
    HLSLOwnedStageInputs inputs;
    HLSLStageCoverage coverage;
    HLSLExpressionSourceMap raw_map;
    HLSLHullWholePlacement placement;
    bool finished;
} HLSLHullCoverageCapture;

struct ShaderLabEmittedHullCoverage {
    StringBuilder source;
    ShaderLabSourceQualityInventory inventory;
    HLSLHullCoverageCapture *entries;
    size_t entry_count, owned_input_bytes, owned_stage_node_count, owned_stage_event_count;
    bool sealed;
};

bool shaderlab_hull_coverage_begin(ShaderLabEmittedHullCoverage *owned,
    const ShaderLabExpressionSourceRecord *record, size_t record_index,
    const uint8_t *target, size_t target_size, const uint8_t *payload, size_t payload_size,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    HLSLHullCoverageCapture **capture);
bool shaderlab_hull_coverage_finish(HLSLHullCoverageCapture *capture,
    const StringBuilder *source, const HLSLExpressionSourceMap *map);
bool shaderlab_hull_coverage_rebase_line(HLSLHullCoverageCapture *capture,
    size_t line_begin, size_t line_end, size_t output_begin);
bool shaderlab_hull_coverage_offset(ShaderLabEmittedHullCoverage *owned,
    size_t first_record, size_t offset);

#endif
