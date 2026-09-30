// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_SOURCE_QUALITY_INTERNAL_H
#define HLSL_SOURCE_QUALITY_INTERNAL_H

#include "translation/hlsl_source_quality.h"

/* Typed historical-result equality. This compares observations; it does not
 * authenticate a caller-owned report or replace producer replay. */
bool hlsl_source_quality_facts_equal(const HLSLSourceQualityFacts *a,
    const HLSLSourceQualityFacts *b);
bool hlsl_source_quality_results_equal(const HLSLSourceQualityResult *a,
    const HLSLSourceQualityResult *b);

#endif
