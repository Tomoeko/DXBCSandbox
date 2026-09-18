// SPDX-License-Identifier: GPL-3.0-only

#ifndef PARAMETER_LAYOUT_H
#define PARAMETER_LAYOUT_H

#include "io/subprogram_metadata.h"

typedef struct {
    uint32_t byte_offset;
    uint32_t array_size;
    uint32_t scalar_type;
    uint32_t rows;
    uint32_t columns;
    bool is_matrix;
} DecodedVariableLayout;

/*
 * Decodes either Unity's packed player-data record or the TypeTree
 * SerializedProgramParameters representation. The layouts are different and
 * must never be distinguished from field values: byte offset zero is valid in
 * both representations. The owning parameter block supplies the format tag.
 */
bool parameter_layout_decode(const SerializedProgramParameters* parameters,
                             const SerializedVariable* variable,
                             DecodedVariableLayout* decoded);

uint32_t parameter_layout_register_count(const DecodedVariableLayout* layout);
uint32_t parameter_layout_byte_size(const DecodedVariableLayout* layout);

#endif
