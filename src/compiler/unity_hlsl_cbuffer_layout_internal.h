// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_HLSL_CBUFFER_LAYOUT_INTERNAL_H
#define UNITY_HLSL_CBUFFER_LAYOUT_INTERNAL_H

#include "compiler/unity_hlsl_cbuffer_inventory.h"

/* Shared closed scalar/vector/column-major float4x4 packing. This supplies
 * physical layout only; the parser/owning receipt separately authorizes syntax,
 * legacy-half storage and the current compiler request. Failure changes neither
 * field nor cursor. No arrays, structures or smaller matrices are admitted. */
UnityHlslCBufferStatus unity_hlsl_cbuffer_layout_field(UnityHlslCBufferField *field,
    uint32_t *cursor);
UnityHlslCBufferStatus unity_hlsl_cbuffer_layout_extent(uint32_t cursor,
    uint32_t *byte_size);

#endif
