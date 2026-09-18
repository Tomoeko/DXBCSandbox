// SPDX-License-Identifier: GPL-3.0-only

#include "app/shader_catalog_pptr.h"

#include "common/common.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void shader_catalog_pptr_graph_init(ShaderCatalogPPtrGraph* graph) {
    if (graph) memset(graph, 0, sizeof(*graph));
}

void shader_catalog_pptr_graph_dispose(ShaderCatalogPPtrGraph* graph) {
    if (!graph) return;
    free(graph->files);
    free(graph->nodes);
    shader_catalog_pptr_graph_init(graph);
}

bool shader_catalog_pptr_graph_build(
    const ShaderCatalog* catalog, ShaderCatalogPPtrGraph* graph) {
    if (!catalog || !graph || !catalog->materials_included ||
        (catalog->source_count != 0U && !catalog->sources) ||
        dxbc_size_multiply_overflows(
            catalog->source_count, sizeof(*graph->files)) ||
        dxbc_size_multiply_overflows(
            catalog->source_count, sizeof(*graph->nodes))) {
        return false;
    }
    ShaderCatalogPPtrGraph pending;
    shader_catalog_pptr_graph_init(&pending);
    if (catalog->source_count != 0U) {
        pending.files = (SerializedFile*)calloc(
            catalog->source_count, sizeof(*pending.files));
        pending.nodes = (UnityPPtrSourceNode*)calloc(
            catalog->source_count, sizeof(*pending.nodes));
        if (!pending.files || !pending.nodes) {
            shader_catalog_pptr_graph_dispose(&pending);
            return false;
        }
    }
    for (size_t i = 0U; i < catalog->source_count; ++i) {
        const ShaderCatalogSource* source = &catalog->sources[i];
        if (source->object_reference_count > INT_MAX ||
            source->external_count > INT_MAX || !source->outer_path ||
            !source->scope_root ||
            (source->object_reference_count != 0U && !source->objects) ||
            (source->external_count != 0U && !source->externals)) {
            shader_catalog_pptr_graph_dispose(&pending);
            return false;
        }
        pending.files[i].object_count =
            (int)source->object_reference_count;
        pending.files[i].objects = source->objects;
        pending.files[i].external_count = (int)source->external_count;
        pending.files[i].externals = source->externals;
        pending.nodes[i].file = &pending.files[i];
        pending.nodes[i].outer_path = source->outer_path;
        pending.nodes[i].member_name = source->member_name;
        pending.nodes[i].member_index = source->member_index;
        pending.nodes[i].is_bundle_member = source->is_bundle_member;
        pending.nodes[i].scope_root = source->scope_root;
    }
    pending.graph.nodes = pending.nodes;
    pending.graph.node_count = catalog->source_count;
    shader_catalog_pptr_graph_dispose(graph);
    *graph = pending;
    return true;
}
