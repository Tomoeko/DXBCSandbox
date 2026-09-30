// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_GEOMETRY_FLOW_H
#define HLSL_GEOMETRY_FLOW_H

#include "translation/hlsl_emitter.h"
struct HLSLEmitterContext;

/* Admission reserves a natural interface; full CFG/SSA/material/type/bound
 * proof remains transactional in the emitter. Unknown ownership fails closed.
 * The explicit 64-instruction bound is independent of the V/F owner capacity. */
bool hlsl_geometry_control_flow_admission(const USILProgram *program, HLSLEmitMode mode);
bool hlsl_geometry_control_flow_emit(struct HLSLEmitterContext *ctx);
/* Structural provenance only; never replaces compiler/control/reflection gates. */
bool hlsl_geometry_control_flow_source_map_matches(
    const HLSLExpressionSourceMap *map, const USILProgram *program, const char *source);

#endif
