// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_CATALOG_PPTR_H
#define SHADER_CATALOG_PPTR_H

#include "app/shader_catalog.h"

typedef struct {
    SerializedFile* files;
    UnityPPtrSourceNode* nodes;
    UnityPPtrResolverGraph graph;
} ShaderCatalogPPtrGraph;

void shader_catalog_pptr_graph_init(ShaderCatalogPPtrGraph* graph);
void shader_catalog_pptr_graph_dispose(ShaderCatalogPPtrGraph* graph);

/* Builds a borrowed-metadata resolver graph from the exact source/object and
 * external tables retained by an include_materials catalog. */
bool shader_catalog_pptr_graph_build(
    const ShaderCatalog* catalog, ShaderCatalogPPtrGraph* graph);

#endif /* SHADER_CATALOG_PPTR_H */
