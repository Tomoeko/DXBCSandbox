// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_VALUE_ANALYSIS_H
#define HLSL_VALUE_ANALYSIS_H

#include <stdbool.h>

typedef enum HLSLValueFacts {
    HLSL_VALUE_UNKNOWN = 0,
    HLSL_VALUE_FLOAT = 1u << 0,
    HLSL_VALUE_SINT = 1u << 1,
    HLSL_VALUE_UINT = 1u << 2,
    HLSL_VALUE_BOOLEAN_BITS = 1u << 3,
    HLSL_VALUE_RAW_BITS = 1u << 4
} HLSLValueFacts;

typedef struct HLSLValueAnalysis {
    unsigned char *lane_facts;
    unsigned char *operand_facts;
    int register_count;
} HLSLValueAnalysis;

#endif
