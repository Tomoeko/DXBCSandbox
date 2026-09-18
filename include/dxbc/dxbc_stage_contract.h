// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_STAGE_CONTRACT_H
#define DXBC_STAGE_CONTRACT_H

#include "common/shader_stage.h"
#include "dxbc/dxbc_document.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DXBC_STAGE_CONTRACT_OK = 0,
    DXBC_STAGE_CONTRACT_INVALID_ARGUMENT,
    DXBC_STAGE_CONTRACT_OUT_OF_MEMORY,
    DXBC_STAGE_CONTRACT_MISSING_EXECUTABLE,
    DXBC_STAGE_CONTRACT_MULTIPLE_EXECUTABLES,
    DXBC_STAGE_CONTRACT_INVALID_VERSION_TOKEN,
    DXBC_STAGE_CONTRACT_UNSUPPORTED_PROGRAM,
    DXBC_STAGE_CONTRACT_INVALID_DECLARATION_LENGTH,
    DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS,
    DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
    DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION,
    DXBC_STAGE_CONTRACT_DECLARATION_ORDER,
    DXBC_STAGE_CONTRACT_DECLARATION_STAGE_MISMATCH,
    DXBC_STAGE_CONTRACT_MISSING_DECLARATION,
    DXBC_STAGE_CONTRACT_PHASE_ORDER,
    DXBC_STAGE_CONTRACT_CONTAINER_MISMATCH
} DXBCStageContractStatus;

typedef struct {
    DXBCStageContractStatus status;
    uint32_t chunk_index;
    uint32_t instruction_index;
    uint32_t opcode;
    uint64_t expected;
    uint64_t actual;
} DXBCStageContractDiagnostic;

typedef enum {
    DXBC_INPUT_PRIMITIVE_UNDEFINED = 0,
    DXBC_INPUT_PRIMITIVE_POINT = 1,
    DXBC_INPUT_PRIMITIVE_LINE = 2,
    DXBC_INPUT_PRIMITIVE_TRIANGLE = 3,
    DXBC_INPUT_PRIMITIVE_LINE_ADJACENCY = 6,
    DXBC_INPUT_PRIMITIVE_TRIANGLE_ADJACENCY = 7
} DXBCInputPrimitive;

typedef enum {
    DXBC_OUTPUT_TOPOLOGY_UNDEFINED = 0,
    DXBC_OUTPUT_TOPOLOGY_POINT_LIST = 1,
    DXBC_OUTPUT_TOPOLOGY_LINE_STRIP = 3,
    DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP = 5
} DXBCOutputTopology;

typedef enum {
    DXBC_TESSELLATOR_DOMAIN_UNDEFINED = 0,
    DXBC_TESSELLATOR_DOMAIN_ISOLINE = 1,
    DXBC_TESSELLATOR_DOMAIN_TRIANGLE = 2,
    DXBC_TESSELLATOR_DOMAIN_QUAD = 3
} DXBCTessellatorDomain;

typedef enum {
    DXBC_TESSELLATOR_PARTITIONING_UNDEFINED = 0,
    DXBC_TESSELLATOR_PARTITIONING_INTEGER = 1,
    DXBC_TESSELLATOR_PARTITIONING_POW2 = 2,
    DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD = 3,
    DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN = 4
} DXBCTessellatorPartitioning;

typedef enum {
    DXBC_TESSELLATOR_OUTPUT_UNDEFINED = 0,
    DXBC_TESSELLATOR_OUTPUT_POINT = 1,
    DXBC_TESSELLATOR_OUTPUT_LINE = 2,
    DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW = 3,
    DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CCW = 4
} DXBCTessellatorOutputPrimitive;

typedef enum {
    DXBC_HULL_PHASE_CONTROL_POINT = 0,
    DXBC_HULL_PHASE_FORK = 1,
    DXBC_HULL_PHASE_JOIN = 2
} DXBCHullPhaseKind;

typedef enum {
    DXBC_GEOMETRY_EFFECT_APPEND = 0,
    DXBC_GEOMETRY_EFFECT_RESTART_STRIP = 1
} DXBCGeometryEffectKind;

typedef struct {
    DXBCGeometryEffectKind kind;
    uint32_t instruction_index;
    uint8_t stream_id;
    bool explicit_stream;
} DXBCGeometryEffectContract;

typedef struct {
    DXBCHullPhaseKind kind;
    uint32_t marker_instruction_index;
    uint32_t first_instruction_index;
    uint32_t end_instruction_index;
    bool instance_count_declared;
    uint32_t instance_count;
} DXBCHullPhaseContract;

typedef struct {
    uint32_t version_token;
    DXBCProgramType program_type;
    uint8_t shader_model_major;
    uint8_t shader_model_minor;
    uint32_t executable_chunk_index;
    uint32_t first_instruction_index;
    uint32_t end_instruction_index;

    bool has_input_primitive;
    DXBCInputPrimitive input_primitive;
    bool has_output_topology;
    DXBCOutputTopology output_topology;
    bool has_max_output_vertex_count;
    uint32_t max_output_vertex_count;
    bool has_geometry_instance_count;
    uint32_t geometry_instance_count;
    uint8_t declared_stream_mask;
    uint8_t referenced_stream_mask;

    /* Ordered executable stream effects decoded from the raw instruction
     * tokens.  Keeping these beside the declaration contract prevents the
     * semantic USIL projection from inventing or reordering Append/Cut. */
    DXBCGeometryEffectContract* geometry_effects;
    size_t geometry_effect_count;
    size_t geometry_effect_capacity;

    bool has_input_control_point_count;
    uint32_t input_control_point_count;
    bool has_output_control_point_count;
    uint32_t output_control_point_count;
    bool has_tessellator_domain;
    DXBCTessellatorDomain tessellator_domain;
    bool has_tessellator_partitioning;
    DXBCTessellatorPartitioning tessellator_partitioning;
    bool has_tessellator_output_primitive;
    DXBCTessellatorOutputPrimitive tessellator_output_primitive;
    bool has_max_tessellation_factor;
    uint32_t max_tessellation_factor_bits;
    float max_tessellation_factor;

    DXBCHullPhaseContract* hull_phases;
    size_t hull_phase_count;
    size_t hull_phase_capacity;
} DXBCStageContract;

/* Initialize before first decode and free when finished.  Do not copy a live
 * contract by value: geometry_effects and hull_phases are owned storage. */
void dxbc_stage_contract_init(DXBCStageContract* contract);
void dxbc_stage_contract_free(DXBCStageContract* contract);

/*
 * Decode stage metadata directly from the lossless instruction tokens.  The
 * optional semantic container is cross-checked only; it is never the scalar
 * declaration authority.
 */
bool dxbc_stage_contract_decode(
    const DXBCDocument* document, const DXBCContainer* semantic_container,
    DXBCStageContract* out_contract,
    DXBCStageContractDiagnostic* out_diagnostic);

bool dxbc_stage_contract_decode_document(
    const DXBCDocument* document, DXBCStageContract* out_contract,
    DXBCStageContractDiagnostic* out_diagnostic);

bool dxbc_stage_contract_validate_container(
    const DXBCStageContract* contract, const DXBCContainer* container,
    DXBCStageContractDiagnostic* out_diagnostic);

const char* dxbc_stage_contract_status_name(DXBCStageContractStatus status);

#endif /* DXBC_STAGE_CONTRACT_H */
