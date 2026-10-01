// SPDX-License-Identifier: GPL-3.0-only
#ifndef SHADERLAB_EMITTED_HULL_COVERAGE_H
#define SHADERLAB_EMITTED_HULL_COVERAGE_H

#include "translation/shaderlab_source_quality.h"

typedef struct ShaderLabEmittedHullCoverage ShaderLabEmittedHullCoverage;

typedef enum {
    SHADERLAB_HULL_COVERAGE_OK = 0,
    SHADERLAB_HULL_COVERAGE_INVALID_ARGUMENT,
    SHADERLAB_HULL_COVERAGE_SCOPE_UNAVAILABLE,
    SHADERLAB_HULL_COVERAGE_EMISSION_FAILED,
    SHADERLAB_HULL_COVERAGE_CAPTURE_FAILED,
    SHADERLAB_HULL_COVERAGE_ALLOCATION_FAILED
} ShaderLabHullCoverageStatus;

typedef struct {
    /* entry_count and the ledger totals count HULL rows; linked_entry_count
     * counts every ordinary stage row in the complete source inventory. */
    size_t source_size, entry_count, linked_entry_count, unit_count, root_count, syntax_count;
    uint8_t source_digest[32], modeled_input_digest[32];
    /* The ordinary inventory's historical classification and gaps. */
    ShaderLabSourceQualityResult base_quality;
} ShaderLabEmittedHullSummary;

typedef struct {
    int subshader_index, pass_index, stage_index, subprogram_index;
    int blob_index, hardware_tier_group;
    size_t serialized_state, entry_record_index, source_begin, source_end;
    size_t unit_count, root_count, syntax_count;
    uint32_t obligations;
    uint8_t target_digest[32], body_digest[32];
    HLSLSourceQualityResult base_quality;
} ShaderLabEmittedHullEntry;

/* Initially NULL output; nonempty output rejects unchanged. Scope is one
 * complete V/F + paired HULL/DOMAIN pass, with optional admitted geometry,
 * all selected bounded HULL variants and the existing parsed FORK producer.
 * JOIN and ICB remain unavailable.
 * Inputs are borrowed and immutable during capture. The receipt owns complete
 * source/inventory, target/player/current/common inputs, local typed coverage
 * and its whole-source placements. Capture does not promote source quality,
 * certify linked stage interfaces, prove exact DXBC, authenticate original
 * source or supply runtime acceptance. */
ShaderLabHullCoverageStatus shaderlab_emitted_hull_coverage_capture(
    const ShaderLabSourceQualityRequest *current, ShaderLabEmittedHullCoverage **output);

/* Regenerates normal inventory and fresh owned HULL coverage against current
 * inputs. Public descriptions and matching hashes alone cannot supply replay. */
bool shaderlab_emitted_hull_coverage_replay(
    const ShaderLabSourceQualityRequest *current, const ShaderLabEmittedHullCoverage *owned);
bool shaderlab_emitted_hull_coverage_describe(
    const ShaderLabEmittedHullCoverage *owned, ShaderLabEmittedHullSummary *summary);
bool shaderlab_emitted_hull_coverage_entry(
    const ShaderLabEmittedHullCoverage *owned, size_t index, ShaderLabEmittedHullEntry *entry);
bool shaderlab_emitted_hull_coverage_source(
    const ShaderLabEmittedHullCoverage *owned, const char **source, size_t *size);
void shaderlab_emitted_hull_coverage_free(ShaderLabEmittedHullCoverage *owned);

#endif
