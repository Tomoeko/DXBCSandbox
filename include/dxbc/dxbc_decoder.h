// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_DECODER_H
#define DXBC_DECODER_H

#include "dxbc/dxbc_parser.h"

const char* dxbc_opcode_name(uint32_t op);
bool dxbc_opcode_is_known(uint32_t op);
bool dxbc_opcode_is_declaration(uint32_t op);
const char* dxbc_sys_value_name(uint32_t sv);
const char* dxbc_interpolation_name(uint32_t mode);
const char* dxbc_resource_dim_name(uint32_t dim);

#endif // DXBC_DECODER_H
