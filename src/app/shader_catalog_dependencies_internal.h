// SPDX-License-Identifier: GPL-3.0-only
#ifndef SHADER_CATALOG_DEPENDENCIES_INTERNAL_H
#define SHADER_CATALOG_DEPENDENCIES_INTERNAL_H

#include "app/shader_catalog_dependencies.h"

/* Private owned storage for the future normal attachment join. Public
 * descriptions cannot construct or modify any of these witnesses. */
struct ShaderCatalogDependencies {
    ShaderObject object;
    ShaderBlobArchive archive;
    StringBuilder source;
    ShaderLabSourceQualityInventory inventory;
    ShaderCatalogDependenciesSummary summary;
    uint8_t root_digest[COMMON_SHA256_DIGEST_SIZE];
    bool has_d3d11_archive;
    bool sealed;
};

#endif
