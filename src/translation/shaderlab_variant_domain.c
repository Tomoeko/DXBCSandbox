// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_variant_domain.h"

#include <limits.h>
#include <string.h>

/*
 * Runtime-selection evidence (UnityPlayer 2021.3.35f1):
 *   keywords::ComputeKeywordMatch
 *   ShaderLab::Program::FindBestMatchingSubProgram
 * The pinned Unity-owned binary ranges and instruction fingerprints in
 * the retained reference fixtures show the exact -16 penalty, strict greater-than winner
 * update, and unsupported-state skip.  The implementation below keeps those
 * details separate from ShaderLab source emission.
 */

bool shaderlab_variant_state_is_canonical(const ShaderLabVariantState* state) {
    if (!state || (state->keyword_count != 0 && !state->keyword_indices)) {
        return false;
    }
    for (size_t i = 1; i < state->keyword_count; ++i) {
        if (state->keyword_indices[i - 1] >= state->keyword_indices[i]) {
            return false;
        }
    }
    return true;
}

bool shaderlab_variant_merge_scopes(
    const ShaderLabVariantScopedState* scoped_state,
    uint16_t* out_indices,
    size_t out_capacity,
    size_t* out_count) {
    if (!scoped_state || !out_count ||
        !shaderlab_variant_state_is_canonical(
            &scoped_state->global_keywords) ||
        !shaderlab_variant_state_is_canonical(&scoped_state->local_keywords) ||
        (!out_indices && out_capacity != 0)) {
        return false;
    }

    size_t global_index = 0;
    size_t local_index = 0;
    size_t count = 0;
    while (global_index < scoped_state->global_keywords.keyword_count ||
           local_index < scoped_state->local_keywords.keyword_count) {
        if (local_index == scoped_state->local_keywords.keyword_count ||
            (global_index < scoped_state->global_keywords.keyword_count &&
             scoped_state->global_keywords.keyword_indices[global_index] <
                 scoped_state->local_keywords.keyword_indices[local_index])) {
            ++global_index;
        } else if (
            global_index == scoped_state->global_keywords.keyword_count ||
            scoped_state->local_keywords.keyword_indices[local_index] <
                scoped_state->global_keywords.keyword_indices[global_index]) {
            ++local_index;
        } else {
            ++global_index;
            ++local_index;
        }
        ++count;
    }

    *out_count = count;
    if (!out_indices) return true;
    if (out_capacity < count) return false;

    global_index = 0;
    local_index = 0;
    size_t output_index = 0;
    while (global_index < scoped_state->global_keywords.keyword_count ||
           local_index < scoped_state->local_keywords.keyword_count) {
        if (local_index == scoped_state->local_keywords.keyword_count ||
            (global_index < scoped_state->global_keywords.keyword_count &&
             scoped_state->global_keywords.keyword_indices[global_index] <
                 scoped_state->local_keywords.keyword_indices[local_index])) {
            out_indices[output_index++] =
                scoped_state->global_keywords.keyword_indices[global_index++];
        } else if (
            global_index == scoped_state->global_keywords.keyword_count ||
            scoped_state->local_keywords.keyword_indices[local_index] <
                scoped_state->global_keywords.keyword_indices[global_index]) {
            out_indices[output_index++] =
                scoped_state->local_keywords.keyword_indices[local_index++];
        } else {
            out_indices[output_index++] =
                scoped_state->global_keywords.keyword_indices[global_index++];
            ++local_index;
        }
    }
    return output_index == count;
}

bool shaderlab_variant_apply_pass_mask(
    const ShaderLabVariantState* request,
    const ShaderLabVariantState* pass_mask,
    uint16_t* out_indices,
    size_t out_capacity,
    size_t* out_count) {
    if (!out_count || !shaderlab_variant_state_is_canonical(request) ||
        !shaderlab_variant_state_is_canonical(pass_mask) ||
        (!out_indices && out_capacity != 0)) {
        return false;
    }

    size_t request_index = 0;
    size_t mask_index = 0;
    size_t count = 0;
    while (request_index < request->keyword_count &&
           mask_index < pass_mask->keyword_count) {
        const uint16_t request_keyword =
            request->keyword_indices[request_index];
        const uint16_t mask_keyword = pass_mask->keyword_indices[mask_index];
        if (request_keyword < mask_keyword) {
            ++request_index;
        } else if (request_keyword > mask_keyword) {
            ++mask_index;
        } else {
            ++count;
            ++request_index;
            ++mask_index;
        }
    }

    *out_count = count;
    if (!out_indices) return true;
    if (out_capacity < count) return false;

    request_index = 0;
    mask_index = 0;
    size_t output_index = 0;
    while (request_index < request->keyword_count &&
           mask_index < pass_mask->keyword_count) {
        const uint16_t request_keyword =
            request->keyword_indices[request_index];
        const uint16_t mask_keyword = pass_mask->keyword_indices[mask_index];
        if (request_keyword < mask_keyword) {
            ++request_index;
        } else if (request_keyword > mask_keyword) {
            ++mask_index;
        } else {
            out_indices[output_index++] = request_keyword;
            ++request_index;
            ++mask_index;
        }
    }
    return output_index == count;
}

bool shaderlab_variant_match_score(
    const ShaderLabVariantState* request,
    const ShaderLabVariantState* candidate,
    int32_t* out_score) {
    if (!out_score || !shaderlab_variant_state_is_canonical(request) ||
        !shaderlab_variant_state_is_canonical(candidate)) {
        return false;
    }

    size_t request_index = 0;
    size_t candidate_index = 0;
    uint32_t common = 0;
    while (request_index < request->keyword_count &&
           candidate_index < candidate->keyword_count) {
        const uint16_t request_keyword =
            request->keyword_indices[request_index];
        const uint16_t candidate_keyword =
            candidate->keyword_indices[candidate_index];
        if (request_keyword < candidate_keyword) {
            ++request_index;
        } else if (request_keyword > candidate_keyword) {
            ++candidate_index;
        } else {
            ++common;
            ++request_index;
            ++candidate_index;
        }
    }

    const uint32_t candidate_only =
        (uint32_t)candidate->keyword_count - common;
    *out_score = (int32_t)common - 16 * (int32_t)candidate_only;
    return true;
}

static bool state_is_subset(const ShaderLabVariantState* state,
                            const ShaderLabVariantState* superset) {
    size_t state_index = 0;
    size_t superset_index = 0;
    while (state_index < state->keyword_count &&
           superset_index < superset->keyword_count) {
        const uint16_t state_keyword = state->keyword_indices[state_index];
        const uint16_t superset_keyword =
            superset->keyword_indices[superset_index];
        if (state_keyword < superset_keyword) return false;
        if (state_keyword > superset_keyword) {
            ++superset_index;
        } else {
            ++state_index;
            ++superset_index;
        }
    }
    return state_index == state->keyword_count;
}

bool shaderlab_variant_pass_authority_is_valid(
    const ShaderLabVariantState* pass_mask,
    const ShaderLabVariantCandidate* candidates,
    size_t candidate_count) {
    if (!shaderlab_variant_state_is_canonical(pass_mask) ||
        (candidate_count != 0 && !candidates)) {
        return false;
    }
    for (size_t i = 0; i < candidate_count; ++i) {
        if (!shaderlab_variant_state_is_canonical(&candidates[i].state) ||
            !state_is_subset(&candidates[i].state, pass_mask)) {
            return false;
        }
    }
    return true;
}

bool shaderlab_variant_select_best(
    const ShaderLabVariantState* request,
    const ShaderLabVariantState* pass_mask,
    const ShaderLabVariantCandidate* candidates,
    size_t candidate_count,
    size_t* out_candidate_index,
    int32_t* out_score) {
    if (!out_candidate_index || !out_score) {
        return false;
    }
    *out_candidate_index = SIZE_MAX;
    *out_score = INT32_MIN;

    if (!shaderlab_variant_state_is_canonical(request) ||
        !shaderlab_variant_state_is_canonical(pass_mask) ||
        !shaderlab_variant_pass_authority_is_valid(pass_mask, candidates,
                                                   candidate_count)) {
        return false;
    }
    /*
     * Candidate states were proven to be subsets of pass_mask above.  Request
     * bits outside that mask therefore cannot occur in either score term, so
     * scoring request directly is exactly equivalent to materializing the
     * masked intersection and avoids an allocation in this hot path.
     */
    for (size_t i = 0; i < candidate_count; ++i) {
        if (!candidates[i].supported) continue;
        int32_t score = 0;
        if (!shaderlab_variant_match_score(request, &candidates[i].state,
                                           &score)) return false;
        if (*out_candidate_index == SIZE_MAX || score > *out_score) {
            *out_candidate_index = i;
            *out_score = score;
        }
    }

    return *out_candidate_index != SIZE_MAX;
}

/*
 * Compiler-domain evidence (UnityShaderCompiler 2021.3.35f1,
 * SHA-256 11f7079bb50af435db858381ee932619f97564950b6578d0d93cfcb7859fc131):
 *   FindBuiltinCompileVariants @ 0x100083a7c (ARM64 image base 0x100000000)
 *   FillBuiltinVariants        @ 0x10008a130
 *   static initializer         @ 0x10008b95c
 *
 * FillBuiltinVariants consumes fixed-width rows terminated by an all-null row,
 * drops all-underscore dummy cells, and preserves row order.  These compact
 * masks are those initialized rows, not a reconstruction from keyword spelling.
 */

static const char* const k_forward_base_keywords[] = {
    "DIRECTIONAL", "LIGHTMAP_ON", "DIRLIGHTMAP_COMBINED",
    "DYNAMICLIGHTMAP_ON", "SHADOWS_SCREEN", "SHADOWS_SHADOWMASK",
    "LIGHTMAP_SHADOW_MIXING", "LIGHTPROBE_SH", "VERTEXLIGHT_ON"};

static const uint16_t k_forward_base_vertex_masks[] = {
    0x001, 0x081, 0x0a1, 0x009, 0x003, 0x00b, 0x083, 0x043, 0x04b,
    0x0c3, 0x00d, 0x007, 0x087, 0x00f, 0x047, 0x0c7, 0x04f, 0x023,
    0x02b, 0x0a3, 0x027, 0x02f, 0x0a7, 0x063, 0x06b, 0x0e3, 0x067,
    0x06f, 0x0e7, 0x011, 0x091, 0x019, 0x0d1, 0x059, 0x0b1, 0x039,
    0x0f1, 0x079, 0x013, 0x01b, 0x093, 0x053, 0x05b, 0x0d3, 0x033,
    0x03b, 0x0b3, 0x073, 0x07b, 0x0f3, 0x01d, 0x017, 0x097, 0x01f,
    0x057, 0x0d7, 0x05f, 0x037, 0x0b7, 0x03f, 0x077, 0x0f7, 0x07f,
    0x101, 0x181, 0x109, 0x10d, 0x111, 0x191, 0x119, 0x11d, 0x151,
    0x1d1, 0x159, 0x15d, 0x131, 0x1b1, 0x139, 0x13d, 0x171, 0x1f1,
    0x179, 0x17d};

static const uint16_t k_forward_base_nonvertex_masks[] = {
    0x001, 0x081, 0x0a1, 0x009, 0x003, 0x00b, 0x083, 0x043, 0x04b,
    0x0c3, 0x00d, 0x007, 0x087, 0x00f, 0x047, 0x0c7, 0x04f, 0x023,
    0x02b, 0x0a3, 0x027, 0x02f, 0x0a7, 0x063, 0x06b, 0x0e3, 0x067,
    0x06f, 0x0e7, 0x011, 0x091, 0x019, 0x0d1, 0x059, 0x0b1, 0x039,
    0x0f1, 0x079, 0x013, 0x01b, 0x093, 0x053, 0x05b, 0x0d3, 0x033,
    0x03b, 0x0b3, 0x073, 0x07b, 0x0f3, 0x01d, 0x017, 0x097, 0x01f,
    0x057, 0x0d7, 0x05f, 0x037, 0x0b7, 0x03f, 0x077, 0x0f7, 0x07f};

static const uint16_t k_forward_base_alpha_vertex_masks[] = {
    0x001, 0x081, 0x009, 0x003, 0x083, 0x00b, 0x043, 0x0c3, 0x04b,
    0x00d, 0x007, 0x087, 0x00f, 0x047, 0x0c7, 0x04f, 0x181, 0x109,
    0x10d};

static const uint16_t k_forward_base_alpha_nonvertex_masks[] = {
    0x001, 0x081, 0x009, 0x003, 0x083, 0x00b, 0x043, 0x0c3,
    0x04b, 0x00d, 0x007, 0x087, 0x00f, 0x047, 0x0c7, 0x04f};

static const char* const k_forward_add_keywords[] = {
    "POINT", "DIRECTIONAL", "SPOT", "POINT_COOKIE", "DIRECTIONAL_COOKIE",
    "SHADOWS_SHADOWMASK", "LIGHTMAP_SHADOW_MIXING", "SHADOWS_DEPTH",
    "SHADOWS_SOFT", "SHADOWS_SCREEN", "SHADOWS_CUBE"};

static const uint16_t k_forward_add_masks[] = {
    0x001, 0x002, 0x004, 0x008, 0x010};

static const uint16_t k_forward_add_fullshadows_masks[] = {
    0x001, 0x002, 0x004, 0x008, 0x010, 0x021, 0x022, 0x024, 0x028,
    0x030, 0x061, 0x062, 0x064, 0x068, 0x070, 0x084, 0x184, 0x0c4,
    0x1c4, 0x0a4, 0x1a4, 0x0e4, 0x1e4, 0x202, 0x210, 0x242, 0x250,
    0x222, 0x230, 0x262, 0x270, 0x401, 0x501, 0x441, 0x541, 0x421,
    0x521, 0x461, 0x561, 0x408, 0x508, 0x448, 0x548, 0x428, 0x528,
    0x468, 0x568};

static const char* const k_prepass_final_keywords[] = {
    "LIGHTMAP_ON", "DIRLIGHTMAP_COMBINED", "DYNAMICLIGHTMAP_ON",
    "UNITY_HDR_ON", "SHADOWS_SHADOWMASK", "LIGHTPROBE_SH"};

static const uint16_t k_prepass_final_masks[] = {
    0x00, 0x20, 0x04, 0x30, 0x14, 0x01, 0x21, 0x05, 0x11, 0x31,
    0x15, 0x06, 0x16, 0x03, 0x23, 0x07, 0x13, 0x33, 0x17, 0x08,
    0x28, 0x0c, 0x18, 0x38, 0x09, 0x29, 0x0d, 0x19, 0x39, 0x1d,
    0x0e, 0x1e, 0x0b, 0x0f, 0x2b, 0x1b, 0x3b, 0x1f};

static const char* const k_shadowcaster_keywords[] = {
    "SHADOWS_DEPTH", "SHADOWS_CUBE"};

static const uint16_t k_shadowcaster_masks[] = {0x1, 0x2};

static const char* const k_shadowcollector_keywords[] = {
    "SHADOWS_SPLIT_SPHERES", "SHADOWS_SINGLE_CASCADE"};

static const uint16_t k_shadowcollector_masks[] = {0x0, 0x1, 0x2, 0x3};

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static bool stage_uses_builtin_nonvertex_table(
    UnitySerializedProgramStage stage) {
    return stage == UNITY_SERIALIZED_STAGE_FRAGMENT ||
           stage == UNITY_SERIALIZED_STAGE_GEOMETRY ||
           stage == UNITY_SERIALIZED_STAGE_HULL ||
           stage == UNITY_SERIALIZED_STAGE_DOMAIN;
}

bool shaderlab_builtin_variant_domain_get(
    ShaderLabBuiltinVariantFamily family,
    UnitySerializedProgramStage stage,
    ShaderLabBuiltinVariantDomain* out_domain) {
    if (!out_domain ||
        (stage != UNITY_SERIALIZED_STAGE_VERTEX &&
         !stage_uses_builtin_nonvertex_table(stage))) {
        return false;
    }

    const bool vertex = stage == UNITY_SERIALIZED_STAGE_VERTEX;
    memset(out_domain, 0, sizeof(*out_domain));
    out_domain->family = family;
    out_domain->stage = stage;
    switch (family) {
    case SHADERLAB_BUILTIN_VARIANT_FWDADD_FULLSHADOWS:
        out_domain->directive = "multi_compile_fwdadd_fullshadows";
        out_domain->keyword_names = k_forward_add_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_forward_add_keywords);
        out_domain->variant_masks = k_forward_add_fullshadows_masks;
        out_domain->variant_count =
            ARRAY_COUNT(k_forward_add_fullshadows_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_FWDADD:
        out_domain->directive = "multi_compile_fwdadd";
        out_domain->keyword_names = k_forward_add_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_forward_add_keywords);
        out_domain->variant_masks = k_forward_add_masks;
        out_domain->variant_count = ARRAY_COUNT(k_forward_add_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_FWDBASE_ALPHA:
        out_domain->directive = "multi_compile_fwdbasealpha";
        out_domain->keyword_names = k_forward_base_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_forward_base_keywords);
        out_domain->variant_masks = vertex
            ? k_forward_base_alpha_vertex_masks
            : k_forward_base_alpha_nonvertex_masks;
        out_domain->variant_count = vertex
            ? ARRAY_COUNT(k_forward_base_alpha_vertex_masks)
            : ARRAY_COUNT(k_forward_base_alpha_nonvertex_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_FWDBASE:
        out_domain->directive = "multi_compile_fwdbase";
        out_domain->keyword_names = k_forward_base_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_forward_base_keywords);
        out_domain->variant_masks = vertex
            ? k_forward_base_vertex_masks
            : k_forward_base_nonvertex_masks;
        out_domain->variant_count = vertex
            ? ARRAY_COUNT(k_forward_base_vertex_masks)
            : ARRAY_COUNT(k_forward_base_nonvertex_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_LIGHTPASS:
        out_domain->directive = "multi_compile_lightpass";
        out_domain->keyword_names = k_forward_add_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_forward_add_keywords);
        out_domain->variant_masks = k_forward_add_fullshadows_masks;
        out_domain->variant_count =
            ARRAY_COUNT(k_forward_add_fullshadows_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_SHADOWCASTER:
        out_domain->directive = "multi_compile_shadowcaster";
        out_domain->keyword_names = k_shadowcaster_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_shadowcaster_keywords);
        out_domain->variant_masks = k_shadowcaster_masks;
        out_domain->variant_count = ARRAY_COUNT(k_shadowcaster_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR:
        out_domain->directive = "multi_compile_shadowcollector";
        out_domain->keyword_names = k_shadowcollector_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_shadowcollector_keywords);
        out_domain->variant_masks = k_shadowcollector_masks;
        out_domain->variant_count = ARRAY_COUNT(k_shadowcollector_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_PREPASSFINAL:
        out_domain->directive = "multi_compile_prepassfinal";
        out_domain->keyword_names = k_prepass_final_keywords;
        out_domain->keyword_count = ARRAY_COUNT(k_prepass_final_keywords);
        out_domain->variant_masks = k_prepass_final_masks;
        out_domain->variant_count = ARRAY_COUNT(k_prepass_final_masks);
        return true;
    case SHADERLAB_BUILTIN_VARIANT_INVALID:
        break;
    }
    memset(out_domain, 0, sizeof(*out_domain));
    return false;
}

size_t shaderlab_builtin_variant_family_candidates_for_pass(
    int serialized_pass_type,
    ShaderLabBuiltinVariantFamily* out_families,
    size_t out_capacity) {
    (void)serialized_pass_type;
    (void)out_families;
    (void)out_capacity;
    return 0;
}

static bool domain_is_valid(const ShaderLabBuiltinVariantDomain* domain) {
    return domain && domain->family != SHADERLAB_BUILTIN_VARIANT_INVALID &&
           domain->directive && domain->keyword_names &&
           domain->keyword_count > 0 && domain->keyword_count <= 16 &&
           domain->variant_masks && domain->variant_count > 0;
}

static bool mask_has_keyword(const ShaderLabBuiltinVariantDomain* domain,
                             uint16_t mask,
                             const char* keyword) {
    for (size_t i = 0; i < domain->keyword_count; ++i) {
        if ((mask & (uint16_t)(UINT16_C(1) << i)) != 0 &&
            strcmp(domain->keyword_names[i], keyword) == 0) {
            return true;
        }
    }
    return false;
}

static bool builtin_exclusions_are_valid(uint32_t exclusions) {
    const uint32_t known_exclusions =
        SHADERLAB_BUILTIN_EXCLUDE_SHADOWS |
        SHADERLAB_BUILTIN_EXCLUDE_LIGHTMAP |
        SHADERLAB_BUILTIN_EXCLUDE_DIR_LIGHTMAP |
        SHADERLAB_BUILTIN_EXCLUDE_DYN_LIGHTMAP |
        SHADERLAB_BUILTIN_EXCLUDE_SHADOWMASK |
        SHADERLAB_BUILTIN_EXCLUDE_VERTEX_LIGHT;
    return (exclusions & ~known_exclusions) == 0;
}

bool shaderlab_builtin_variant_is_included(
    const ShaderLabBuiltinVariantDomain* domain,
    size_t variant_index,
    uint32_t exclusions) {
    if (!domain_is_valid(domain) || variant_index >= domain->variant_count ||
        !builtin_exclusions_are_valid(exclusions)) {
        return false;
    }

    const uint16_t mask = domain->variant_masks[variant_index];
    if ((exclusions & SHADERLAB_BUILTIN_EXCLUDE_SHADOWS) != 0) {
        for (size_t i = 0; i < domain->keyword_count; ++i) {
            if ((mask & (uint16_t)(UINT16_C(1) << i)) != 0 &&
                strncmp(domain->keyword_names[i], "SHADOWS_", 8) == 0) {
                return false;
            }
        }
    }
    if ((exclusions & SHADERLAB_BUILTIN_EXCLUDE_LIGHTMAP) != 0 &&
        mask_has_keyword(domain, mask, "LIGHTMAP_ON")) {
        return false;
    }
    if ((exclusions & SHADERLAB_BUILTIN_EXCLUDE_DIR_LIGHTMAP) != 0 &&
        mask_has_keyword(domain, mask, "DIRLIGHTMAP_COMBINED")) {
        return false;
    }
    if ((exclusions & SHADERLAB_BUILTIN_EXCLUDE_DYN_LIGHTMAP) != 0 &&
        mask_has_keyword(domain, mask, "DYNAMICLIGHTMAP_ON")) {
        return false;
    }
    if ((exclusions & SHADERLAB_BUILTIN_EXCLUDE_SHADOWMASK) != 0 &&
        mask_has_keyword(domain, mask, "SHADOWS_SHADOWMASK")) {
        return false;
    }
    if ((exclusions & SHADERLAB_BUILTIN_EXCLUDE_VERTEX_LIGHT) != 0 &&
        mask_has_keyword(domain, mask, "VERTEXLIGHT_ON")) {
        return false;
    }
    return true;
}

size_t shaderlab_builtin_variant_keyword_count(
    const ShaderLabBuiltinVariantDomain* domain,
    size_t variant_index) {
    if (!domain_is_valid(domain) || variant_index >= domain->variant_count) {
        return 0;
    }
    uint16_t mask = domain->variant_masks[variant_index];
    size_t count = 0;
    while (mask != 0) {
        count += mask & UINT16_C(1);
        mask >>= 1;
    }
    return count;
}

const char* shaderlab_builtin_variant_keyword_at(
    const ShaderLabBuiltinVariantDomain* domain,
    size_t variant_index,
    size_t keyword_ordinal) {
    if (!domain_is_valid(domain) || variant_index >= domain->variant_count) {
        return NULL;
    }
    const uint16_t mask = domain->variant_masks[variant_index];
    size_t ordinal = 0;
    for (size_t i = 0; i < domain->keyword_count; ++i) {
        if ((mask & (uint16_t)(UINT16_C(1) << i)) == 0) continue;
        if (ordinal++ == keyword_ordinal) return domain->keyword_names[i];
    }
    return NULL;
}

bool shaderlab_builtin_variant_mask_from_keywords(
    const ShaderLabBuiltinVariantDomain* domain,
    const char* const* keywords,
    size_t keyword_count,
    uint16_t* out_mask,
    size_t* out_unrecognized_count) {
    if (!domain_is_valid(domain) || !out_mask || !out_unrecognized_count ||
        (keyword_count != 0 && !keywords)) {
        return false;
    }

    uint16_t mask = 0;
    size_t unrecognized = 0;
    for (size_t i = 0; i < keyword_count; ++i) {
        if (!keywords[i]) return false;
        for (size_t previous = 0; previous < i; ++previous) {
            if (strcmp(keywords[i], keywords[previous]) == 0) return false;
        }

        bool recognized = false;
        for (size_t keyword_index = 0;
             keyword_index < domain->keyword_count;
             ++keyword_index) {
            if (strcmp(keywords[i], domain->keyword_names[keyword_index]) ==
                0) {
                mask |= (uint16_t)(UINT16_C(1) << keyword_index);
                recognized = true;
                break;
            }
        }
        if (!recognized) ++unrecognized;
    }
    *out_mask = mask;
    *out_unrecognized_count = unrecognized;
    return true;
}

bool shaderlab_builtin_variant_find_mask(
    const ShaderLabBuiltinVariantDomain* domain,
    uint16_t mask,
    uint32_t exclusions,
    size_t* out_variant_index) {
    if (!domain_is_valid(domain) || !out_variant_index ||
        (mask >> domain->keyword_count) != 0) {
        return false;
    }
    for (size_t i = 0; i < domain->variant_count; ++i) {
        if (domain->variant_masks[i] == mask &&
            shaderlab_builtin_variant_is_included(domain, i, exclusions)) {
            *out_variant_index = i;
            return true;
        }
    }
    return false;
}

ShaderLabBuiltinRecognitionStatus
shaderlab_builtin_variant_recognize_for_pass(
    int serialized_pass_type,
    UnitySerializedProgramStage stage,
    const uint16_t* observed_builtin_masks,
    size_t observed_count,
    uint32_t exclusions,
    ShaderLabBuiltinVariantFamily* out_family) {
    (void)serialized_pass_type;
    (void)stage;
    if (!out_family || !observed_builtin_masks || observed_count == 0) {
        return SHADERLAB_BUILTIN_RECOGNITION_INVALID_ARGUMENT;
    }
    *out_family = SHADERLAB_BUILTIN_VARIANT_INVALID;
    if (!builtin_exclusions_are_valid(exclusions)) {
        return SHADERLAB_BUILTIN_RECOGNITION_INVALID_ARGUMENT;
    }
    return SHADERLAB_BUILTIN_RECOGNITION_NO_MATCH;
}

#undef ARRAY_COUNT
