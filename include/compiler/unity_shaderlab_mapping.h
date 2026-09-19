// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_SHADERLAB_MAPPING_H
#define UNITY_SHADERLAB_MAPPING_H

#include "compiler/unity_compiler_client.h"
#include "io/serialized_shader.h"

/* Routing for the fail-closed D3D11 candidate projection. Unknown platform
 * authority remains a requested pass; only proven foreign passes disappear.
 * UsePass and GrabPass do not contribute HLSLPROGRAM snippets. These predicates
 * describe routing, not whether the pass can be emitted or certified. */
bool unity_shaderlab_pass_emits_snippet(const SerializedPass *pass);
bool unity_shaderlab_pass_has_d3d11(const SerializedPass *pass);

/* Counts/ordinals include all serialized subshaders and passes. Invalid
 * hierarchies and out-of-range/non-emitting queries return -1 or NULL. */
int unity_shaderlab_snippet_count(const SerializedShader *shader);
int unity_shaderlab_pass_snippet_index(const SerializedShader *shader, int serialized_pass_index);
const SerializedPass *unity_shaderlab_pass_at(const SerializedShader *shader,
                                              int serialized_pass_index);

/* Borrowed result for source emitted by the complete candidate pipeline only.
 * Require the entire projected snippet count before mapping by preserved pass
 * order. A matching count alone is not a source, domain, or DXBC certificate. */
const PreprocessedSnippet *unity_shaderlab_find_generated_snippet(const SerializedShader *shader,
                                                                  int serialized_pass_index,
                                                                  const PreprocessResult *generated,
                                                                  int *out_snippet_index);

/* Original source has a separate identity boundary: a unique GPU program ID.
 * Never apply generated-source ordinal routing to original source. */
const PreprocessedSnippet *unity_shaderlab_find_original_snippet(const SerializedShader *shader,
                                                                 int serialized_pass_index,
                                                                 const PreprocessResult *original);

#endif
