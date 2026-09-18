// SPDX-License-Identifier: GPL-3.0-only

#include "io/parameter_layout.h"

#include <limits.h>
#include <string.h>

bool parameter_layout_decode(const SerializedProgramParameters* parameters,
                             const SerializedVariable* variable,
                             DecodedVariableLayout* decoded) {
    if (!parameters || !variable || !decoded) {
        return false;
    }

    memset(decoded, 0, sizeof(*decoded));
    if (parameters->is_binary) {
        /*
         * LoadParametersFromData forwards the packed words as follows:
         *   AddMatrixParam(name, word5, word4, word0, word1)
         *   AddVectorParam(name, word5, word4, word0, word2)
         * with word3 selecting the matrix path.
         */
        decoded->scalar_type = variable->layout[0];
        decoded->is_matrix = variable->layout[3] != 0;
        decoded->array_size = variable->layout[4];
        decoded->byte_offset = variable->layout[5];
        decoded->rows = decoded->is_matrix ? variable->layout[1] : 1;
        decoded->columns = decoded->is_matrix ? 4 : variable->layout[2];
    } else {
        decoded->byte_offset = variable->layout[0];
        decoded->array_size = variable->layout[1];
        decoded->scalar_type = variable->layout[2];
        decoded->is_matrix = variable->layout[4] != 0;
        decoded->rows = decoded->is_matrix ? variable->layout[3] : 1;
        decoded->columns = decoded->is_matrix ? 4 : variable->layout[3];
    }

    return decoded->rows >= 1 && decoded->rows <= 4 &&
           decoded->columns >= 1 && decoded->columns <= 4;
}

uint32_t parameter_layout_register_count(const DecodedVariableLayout* layout) {
    if (!layout) {
        return 0;
    }
    uint64_t element_count = layout->array_size ? layout->array_size : 1;
    uint64_t register_count;
    if (layout->is_matrix) {
        register_count = element_count * layout->rows;
    } else {
        register_count = layout->array_size ? element_count : 1;
    }
    return register_count <= UINT32_MAX ? (uint32_t)register_count : 0;
}

uint32_t parameter_layout_byte_size(const DecodedVariableLayout* layout) {
    if (!layout) {
        return 0;
    }
    uint64_t byte_size;
    if (layout->is_matrix) {
        byte_size = (uint64_t)(layout->array_size ? layout->array_size : 1) *
                    layout->rows * 16u;
    } else if (layout->array_size) {
        /* Array elements start in separate 16-byte registers. The last
         * element occupies only its declared components, so the next value
         * may share that register's remaining lanes. */
        byte_size = (uint64_t)(layout->array_size - 1u) * 16u +
                    (uint64_t)layout->columns * 4u;
    } else {
        byte_size = (uint64_t)layout->columns * 4u;
    }
    return byte_size <= UINT32_MAX ? (uint32_t)byte_size : 0;
}
