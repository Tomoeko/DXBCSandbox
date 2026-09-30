// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_GEOMETRY_FLOW_H
#define HLSL_GEOMETRY_FLOW_H

#include "translation/hlsl_emitter.h"
struct HLSLEmitterContext;

/* Private receipts for actual emitted syntax. Semantic eligibility is rebuilt
 * from CFG/SSA and metadata at finalization; source spans never grant it. */
typedef enum {
  HLSL_GEOMETRY_FLOW_SYNTAX_NONE,
  HLSL_GEOMETRY_FLOW_SYNTAX_EXPRESSION,
  HLSL_GEOMETRY_FLOW_SYNTAX_LOOP_HEADER,
  HLSL_GEOMETRY_FLOW_SYNTAX_IF,
  HLSL_GEOMETRY_FLOW_SYNTAX_ELSE,
  HLSL_GEOMETRY_FLOW_SYNTAX_ENDIF,
  HLSL_GEOMETRY_FLOW_SYNTAX_ENDLOOP,
  HLSL_GEOMETRY_FLOW_SYNTAX_EFFECT,
  HLSL_GEOMETRY_FLOW_SYNTAX_RETURN,
  HLSL_GEOMETRY_FLOW_SYNTAX_NOP
} HLSLGeometryFlowSyntaxKind;

typedef struct {
  HLSLGeometryFlowSyntaxKind kind;
  uint32_t source_instruction_index;
  uint8_t destination_lanes;
  size_t source_begin, source_end;
  uint64_t source_digest;
} HLSLGeometryFlowSyntaxRecord;

typedef enum {
  HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_FIELD,
  HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_END,
  HLSL_GEOMETRY_FLOW_INTERFACE_OUTPUT_FIELD,
  HLSL_GEOMETRY_FLOW_INTERFACE_OUTPUT_END,
  HLSL_GEOMETRY_FLOW_INTERFACE_ATTRIBUTE,
  HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_PARAMETER,
  HLSL_GEOMETRY_FLOW_INTERFACE_STREAM_PARAMETER,
  HLSL_GEOMETRY_FLOW_INTERFACE_FUNCTION_OPEN,
  HLSL_GEOMETRY_FLOW_INTERFACE_RESULT_LOCAL
} HLSLGeometryFlowInterfaceKind;

typedef struct {
  HLSLGeometryFlowInterfaceKind kind;
  int field_index;
  size_t source_begin, source_end;
  uint64_t source_digest;
} HLSLGeometryFlowInterfaceRecord;

typedef struct HLSLGeometryFlowSourceInventory {
  int instruction_count;
  int loop, end, compare, test, increment, initial;
  size_t source_begin;
  uint64_t recorded_instructions;
  uint64_t emitted_expressions;
  HLSLGeometryFlowSyntaxRecord records[64];
  size_t interface_begin;
  size_t interface_record_count;
  HLSLGeometryFlowInterfaceRecord interface_records[72];
} HLSLGeometryFlowSourceInventory;

bool hlsl_geometry_control_flow_record_interface(struct HLSLEmitterContext *ctx,
    HLSLGeometryFlowInterfaceKind kind, int field_index, size_t source_begin);
bool hlsl_geometry_control_flow_inventory_supported(struct HLSLEmitterContext *ctx);
bool hlsl_geometry_control_flow_inventory_complete(struct HLSLEmitterContext *ctx);
void hlsl_geometry_control_flow_inventory_free(struct HLSLEmitterContext *ctx);
/* Called by the actual final function-brace emitter only. */
bool hlsl_geometry_control_flow_record_return(struct HLSLEmitterContext *ctx,
                                              size_t source_begin);

/* Admission reserves a natural interface; full CFG/SSA/material/type/bound
 * proof remains transactional in the emitter. Unknown ownership fails closed.
 * The explicit 64-instruction bound is independent of the V/F owner capacity. */
bool hlsl_geometry_control_flow_admission(const USILProgram *program, HLSLEmitMode mode);
bool hlsl_geometry_control_flow_emit(struct HLSLEmitterContext *ctx);
/* Structural provenance only; never replaces compiler/control/reflection gates. */
bool hlsl_geometry_control_flow_source_map_matches(
    const HLSLExpressionSourceMap *map, const USILProgram *program, const char *source);

#endif
