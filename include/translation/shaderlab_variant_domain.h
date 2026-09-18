// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADERLAB_VARIANT_DOMAIN_H
#define SHADERLAB_VARIANT_DOMAIN_H

#include "common/shader_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A serialized Unity LocalKeywordState projected to the stable uint16 keyword
 * indices used by SerializedPass.m_SerializedKeywordStateMask.  Indices are a
 * mathematical set: they must be strictly increasing and unique.
 */
typedef struct {
    const uint16_t* keyword_indices;
    size_t keyword_count;
} ShaderLabVariantState;

typedef struct {
    ShaderLabVariantState state;
    /* Unity removes an unsupported candidate before comparing later rows. */
    bool supported;
} ShaderLabVariantCandidate;

/* Raw TypeTree keyword scopes before they are merged into LocalKeywordState. */
typedef struct {
    ShaderLabVariantState global_keywords;
    ShaderLabVariantState local_keywords;
} ShaderLabVariantScopedState;

bool shaderlab_variant_state_is_canonical(const ShaderLabVariantState* state);

/*
 * Merges exact uint16 global/local indices into the state compared by the
 * player.  An index present in both scopes is emitted once because runtime
 * LocalKeywordState is a set.  The output follows the same
 * size-query/strong-output convention as pass masking.
 */
bool shaderlab_variant_merge_scopes(
    const ShaderLabVariantScopedState* scoped_state,
    uint16_t* out_indices,
    size_t out_capacity,
    size_t* out_count);

/*
 * Intersects request with the serialized pass mask.  A NULL output with zero
 * capacity is a size query.  On insufficient capacity, out_count receives the
 * required count and the function returns false without partially writing.
 */
bool shaderlab_variant_apply_pass_mask(
    const ShaderLabVariantState* request,
    const ShaderLabVariantState* pass_mask,
    uint16_t* out_indices,
    size_t out_capacity,
    size_t* out_count);

/* Exact Unity 2021.3 player score: common - 16 * candidate-only. */
bool shaderlab_variant_match_score(
    const ShaderLabVariantState* request,
    const ShaderLabVariantState* candidate,
    int32_t* out_score);

/*
 * Applies the pass mask and selects in serialized order.  Equal scores retain
 * the earlier candidate because Unity updates its winner only for score > best.
 * Every candidate must already be a subset of pass_mask.
 */
bool shaderlab_variant_select_best(
    const ShaderLabVariantState* request,
    const ShaderLabVariantState* pass_mask,
    const ShaderLabVariantCandidate* candidates,
    size_t candidate_count,
    size_t* out_candidate_index,
    int32_t* out_score);

/* Validates the pass mask and all serialized candidate subsets without a request. */
bool shaderlab_variant_pass_authority_is_valid(
    const ShaderLabVariantState* pass_mask,
    const ShaderLabVariantCandidate* candidates,
    size_t candidate_count);

typedef enum {
    SHADERLAB_BUILTIN_VARIANT_INVALID = 0,
    SHADERLAB_BUILTIN_VARIANT_FWDADD_FULLSHADOWS,
    SHADERLAB_BUILTIN_VARIANT_FWDADD,
    SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA,
    SHADERLAB_BUILTIN_VARIANT_FWDBASE,
    SHADERLAB_BUILTIN_VARIANT_LIGHTPASS,
    SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER,
    SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR,
    SHADERLAB_BUILTIN_VARIANT_PREPASSFINAL
} ShaderLabBuiltinVariantFamily;

typedef enum {
    SHADERLAB_BUILTIN_EXCLUDE_NONE = 0,
    SHADERLAB_BUILTIN_EXCLUDE_SHADOWS = 1u << 0,       /* noshadow */
    SHADERLAB_BUILTIN_EXCLUDE_LIGHTMAP = 1u << 1,      /* nolightmap */
    SHADERLAB_BUILTIN_EXCLUDE_DIR_LIGHTMAP = 1u << 2,  /* nodirlightmap */
    SHADERLAB_BUILTIN_EXCLUDE_DYN_LIGHTMAP = 1u << 3,  /* nodynlightmap */
    SHADERLAB_BUILTIN_EXCLUDE_SHADOWMASK = 1u << 4,    /* noshadowmask */
    SHADERLAB_BUILTIN_EXCLUDE_VERTEX_LIGHT = 1u << 5   /* novertexlight */
} ShaderLabBuiltinVariantExclusion;

typedef struct {
    ShaderLabBuiltinVariantFamily family;
    UnitySerializedProgramStage stage;
    const char* directive;
    const char* const* keyword_names;
    size_t keyword_count;
    const uint16_t* variant_masks;
    size_t variant_count;
} ShaderLabBuiltinVariantDomain;

typedef enum {
    SHADERLAB_BUILTIN_RECOGNITION_OK = 0,
    SHADERLAB_BUILTIN_RECOGNITION_INVALID_ARGUMENT,
    SHADERLAB_BUILTIN_RECOGNITION_NO_MATCH,
    SHADERLAB_BUILTIN_RECOGNITION_AMBIGUOUS
} ShaderLabBuiltinRecognitionStatus;

/*
 * Returns the pinned Unity 2021.3.35f1 compiler expansion.  Vertex uses the
 * first table supplied to FillBuiltinVariants; fragment/geometry/hull/domain
 * share its second table.  Ray-tracing is not accepted by that code path.
 */
bool shaderlab_builtin_variant_domain_get(
    ShaderLabBuiltinVariantFamily family,
    UnitySerializedProgramStage stage,
    ShaderLabBuiltinVariantDomain* out_domain);

/*
 * Retained as a fail-closed compatibility API.  SerializedPass.m_Type is not
 * pragma-family authority (real ForwardBase/Add/Prepass/ShadowCaster passes
 * can all carry value zero), so this returns no candidates for every value.
 */
size_t shaderlab_builtin_variant_family_candidates_for_pass(
    int serialized_pass_type,
    ShaderLabBuiltinVariantFamily* out_families,
    size_t out_capacity);

bool shaderlab_builtin_variant_is_included(
    const ShaderLabBuiltinVariantDomain* domain,
    size_t variant_index,
    uint32_t exclusions);

size_t shaderlab_builtin_variant_keyword_count(
    const ShaderLabBuiltinVariantDomain* domain,
    size_t variant_index);

const char* shaderlab_builtin_variant_keyword_at(
    const ShaderLabBuiltinVariantDomain* domain,
    size_t variant_index,
    size_t keyword_ordinal);

/*
 * Projects an unordered keyword-name set onto one exact built-in vocabulary.
 * Unknown names are counted separately so independent pragma domains can be
 * reconstructed without silently treating them as part of the built-in.
 */
bool shaderlab_builtin_variant_mask_from_keywords(
    const ShaderLabBuiltinVariantDomain* domain,
    const char* const* keywords,
    size_t keyword_count,
    uint16_t* out_mask,
    size_t* out_unrecognized_count);

bool shaderlab_builtin_variant_find_mask(
    const ShaderLabBuiltinVariantDomain* domain,
    uint16_t mask,
    uint32_t exclusions,
    size_t* out_variant_index);

/*
 * Compatibility wrapper around the retired pass-type route.  Since m_Type is
 * not family authority, this returns NO_MATCH for otherwise-valid inputs.
 * Pass planning instead searches all pinned tables and proves ordered-domain
 * and runtime-selector composition directly.
 */
ShaderLabBuiltinRecognitionStatus
shaderlab_builtin_variant_recognize_for_pass(
    int serialized_pass_type,
    UnitySerializedProgramStage stage,
    const uint16_t* observed_builtin_masks,
    size_t observed_count,
    uint32_t exclusions,
    ShaderLabBuiltinVariantFamily* out_family);

#endif /* SHADERLAB_VARIANT_DOMAIN_H */
