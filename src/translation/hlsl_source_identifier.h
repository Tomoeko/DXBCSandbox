// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_SOURCE_IDENTIFIER_H
#define HLSL_SOURCE_IDENTIFIER_H

#include <stdbool.h>

/* Bounded natural compute source names and preprocessor macro names share
 * lexical, reserved-word, type-spelling and emitted intrinsic exclusions. */
bool hlsl_source_identifier_valid(const char *name);

#endif
