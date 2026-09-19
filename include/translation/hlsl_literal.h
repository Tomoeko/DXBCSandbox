// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_LITERAL_H
#define HLSL_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Emit one binary32 literal without dropping significant bits. Exceptional
 * and subnormal values use asfloat(raw bits); signed zero is retained.
 * Returns false rather than truncating the destination. */
bool format_float_bits_hlsl(uint32_t bits, char *buffer, size_t capacity);

#endif
