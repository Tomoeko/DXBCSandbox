// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_mapping.h"
#include "translation/shaderlab_emitter.h"

#include <limits.h>

/* Public mapping inputs can also be constructed by callers. Reject malformed
 * hierarchy and overflowing ordinals before walking any borrowed arrays. */
static bool valid_pass_hierarchy(const SerializedShader *shader) {
    if (!shader || shader->subshader_count < 0 || (shader->subshader_count && !shader->subshaders))
        return false;
    int count = 0;
    for (int i = 0; i < shader->subshader_count; ++i) {
        const SerializedSubShader *subshader = &shader->subshaders[i];
        if (subshader->pass_count < 0 || (subshader->pass_count && !subshader->passes) ||
            subshader->pass_count > INT_MAX - count)
            return false;
        count += subshader->pass_count;
    }
    return true;
}

bool unity_shaderlab_pass_emits_snippet(const SerializedPass *pass) {
    return pass && pass->pass_type != 2 && pass->pass_type != 1 &&
           (!pass->use_name || pass->use_name[0] == '\0') &&
           !shaderlab_pass_is_proven_not_platform(pass, 4);
}

bool unity_shaderlab_pass_has_d3d11(const SerializedPass *pass) {
    if (!pass)
        return false;
    for (int stage = 0; stage < 6; ++stage) {
        for (int subprogram = 0; subprogram < pass->subprogram_count[stage]; ++subprogram) {
            if (serialized_pass_subprogram_is_platform(pass, stage, subprogram, 4)) {
                return true;
            }
        }
    }
    return false;
}

int unity_shaderlab_snippet_count(const SerializedShader *shader) {
    if (!valid_pass_hierarchy(shader))
        return -1;
    int count = 0;
    for (int subshader = 0; subshader < shader->subshader_count; ++subshader) {
        const SerializedSubShader *serialized_subshader = &shader->subshaders[subshader];
        for (int pass = 0; pass < serialized_subshader->pass_count; ++pass) {
            if (!unity_shaderlab_pass_emits_snippet(&serialized_subshader->passes[pass])) {
                continue;
            }
            ++count;
        }
    }
    return count;
}

/* Generated ShaderLab preserves serialized subshader/pass order and emits
 * exactly one HLSLPROGRAM for each ordinary pass retained by the fail-closed
 * D3D11 projection. Requiring the complete projected count makes this ordinal
 * mapping bijective instead of silently accepting a shifted or partial
 * preprocess result. */
const PreprocessedSnippet *unity_shaderlab_find_generated_snippet(const SerializedShader *shader,
                                                                  int serialized_pass_index,
                                                                  const PreprocessResult *generated,
                                                                  int *out_snippet_index) {
    if (out_snippet_index)
        *out_snippet_index = -1;
    if (!shader || !generated || generated->snippet_count < 0 ||
        (generated->snippet_count > 0 && !generated->snippets) ||
        unity_shaderlab_snippet_count(shader) != generated->snippet_count) {
        return NULL;
    }
    const int snippet_index = unity_shaderlab_pass_snippet_index(shader, serialized_pass_index);
    if (snippet_index < 0 || snippet_index >= generated->snippet_count) {
        return NULL;
    }
    const PreprocessedSnippet *snippet = &generated->snippets[snippet_index];
    if (!snippet->source || !snippet->has_contract)
        return NULL;
    if (out_snippet_index)
        *out_snippet_index = snippet_index;
    return snippet;
}

int unity_shaderlab_pass_snippet_index(const SerializedShader *shader, int serialized_pass_index) {
    if (!valid_pass_hierarchy(shader) || serialized_pass_index < 0)
        return -1;
    int current_pass_index = 0;
    int current_snippet_index = 0;
    for (int subshader_idx = 0; subshader_idx < shader->subshader_count; subshader_idx++) {
        const SerializedSubShader *subshader = &shader->subshaders[subshader_idx];
        for (int pass_idx = 0; pass_idx < subshader->pass_count; pass_idx++) {
            const SerializedPass *pass = &subshader->passes[pass_idx];
            if (current_pass_index == serialized_pass_index) {
                return unity_shaderlab_pass_emits_snippet(pass) ? current_snippet_index : -1;
            }
            if (unity_shaderlab_pass_emits_snippet(pass)) {
                current_snippet_index++;
            }
            current_pass_index++;
        }
    }
    return -1;
}

const SerializedPass *unity_shaderlab_pass_at(const SerializedShader *shader,
                                              int serialized_pass_index) {
    if (!valid_pass_hierarchy(shader) || serialized_pass_index < 0)
        return NULL;
    int current_pass_index = 0;
    for (int subshader_idx = 0; subshader_idx < shader->subshader_count; subshader_idx++) {
        const SerializedSubShader *subshader = &shader->subshaders[subshader_idx];
        for (int pass_idx = 0; pass_idx < subshader->pass_count; pass_idx++) {
            if (current_pass_index++ == serialized_pass_index) {
                return &subshader->passes[pass_idx];
            }
        }
    }
    return NULL;
}

const PreprocessedSnippet *unity_shaderlab_find_original_snippet(const SerializedShader *shader,
                                                                 int serialized_pass_index,
                                                                 const PreprocessResult *original) {
    if (!original || original->snippet_count < 0 ||
        (original->snippet_count && !original->snippets))
        return NULL;
    const SerializedPass *pass = unity_shaderlab_pass_at(shader, serialized_pass_index);
    if (!pass)
        return NULL;
    const PreprocessedSnippet *match = NULL;
    for (int i = 0; i < original->snippet_count; i++) {
        if (original->snippets[i].gpu_program_id == pass->state.gpuProgramID) {
            /* A duplicate GPU program ID is not enough to identify source. */
            if (match)
                return NULL;
            match = &original->snippets[i];
        }
    }
    return match;
}
