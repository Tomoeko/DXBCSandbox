// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_stage_contract.h"

#include <string.h>

enum {
    OP_CUT = 9,
    OP_EMIT = 19,
    OP_RET = 62,
    OP_DCL_IMMEDIATE_CONSTANT_BUFFER = 53,
    OP_DCL_RESOURCE = 88,
    OP_DCL_GLOBAL_FLAGS = 106,
    OP_HS_DECLS = 113,
    OP_HS_CONTROL_POINT_PHASE = 114,
    OP_HS_FORK_PHASE = 115,
    OP_HS_JOIN_PHASE = 116,
    OP_EMIT_STREAM = 117,
    OP_CUT_STREAM = 118,
    OP_DCL_STREAM = 143,
    OP_DCL_INPUT_CONTROL_POINT_COUNT = 147,
    OP_DCL_OUTPUT_CONTROL_POINT_COUNT = 148,
    OP_DCL_TESSELLATOR_DOMAIN = 149,
    OP_DCL_TESSELLATOR_PARTITIONING = 150,
    OP_DCL_TESSELLATOR_OUTPUT_PRIMITIVE = 151,
    OP_DCL_HS_MAX_TESS_FACTOR = 152,
    OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT = 153,
    OP_DCL_HS_JOIN_PHASE_INSTANCE_COUNT = 154,
    OP_DCL_GS_INSTANCE_COUNT = 206,
    OP_DCL_RESOURCE_STRUCTURED = 162,
    OP_DCL_OUTPUT_TOPOLOGY = 92,
    OP_DCL_INPUT_PRIMITIVE = 93,
    OP_DCL_MAX_OUTPUT_VERTEX_COUNT = 94
};

#define NO_INSTRUCTION UINT32_MAX

static bool instruction_token(const DXBCDocumentInstruction* instruction,
                              size_t index, uint32_t* out_token,
                              DXBCStageContractDiagnostic* diagnostic);
static bool declaration_header(
    const DXBCDocumentInstruction* instruction, uint32_t expected_length,
    uint32_t* out_control, DXBCStageContractDiagnostic* diagnostic);

static void clear_diagnostic(DXBCStageContractDiagnostic* diagnostic) {
    if (!diagnostic) return;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = DXBC_STAGE_CONTRACT_OK;
    diagnostic->chunk_index = UINT32_MAX;
    diagnostic->instruction_index = UINT32_MAX;
}

static bool fail(DXBCStageContractDiagnostic* diagnostic,
                 DXBCStageContractStatus status, uint32_t chunk_index,
                 uint32_t instruction_index, uint32_t opcode,
                 uint64_t expected, uint64_t actual) {
    if (diagnostic) {
        diagnostic->status = status;
        diagnostic->chunk_index = chunk_index;
        diagnostic->instruction_index = instruction_index;
        diagnostic->opcode = opcode;
        diagnostic->expected = expected;
        diagnostic->actual = actual;
    }
    return false;
}

void dxbc_stage_contract_init(DXBCStageContract* contract) {
    if (!contract) return;
    memset(contract, 0, sizeof(*contract));
    contract->program_type = DXBC_PROGRAM_TYPE_INVALID;
    contract->executable_chunk_index = UINT32_MAX;
}

void dxbc_stage_contract_free(DXBCStageContract* contract) {
    if (!contract) return;
    mem_free(contract->geometry_effects,
             contract->geometry_effect_capacity *
                 sizeof(*contract->geometry_effects));
    mem_free(contract->hull_phases,
             contract->hull_phase_capacity * sizeof(*contract->hull_phases));
    dxbc_stage_contract_init(contract);
}

static bool append_geometry_effect(
    DXBCStageContract* contract, DXBCGeometryEffectKind kind,
    uint32_t instruction_index, uint8_t stream_id, bool explicit_stream,
    DXBCStageContractDiagnostic* diagnostic, uint32_t chunk_index,
    uint32_t opcode) {
    if (contract->geometry_effect_count ==
        contract->geometry_effect_capacity) {
        const size_t old_capacity = contract->geometry_effect_capacity;
        const size_t new_capacity = old_capacity == 0u ? 8u : old_capacity * 2u;
        if (new_capacity < old_capacity ||
            new_capacity > SIZE_MAX / sizeof(*contract->geometry_effects)) {
            return fail(diagnostic, DXBC_STAGE_CONTRACT_OUT_OF_MEMORY,
                        chunk_index, instruction_index, opcode, 0, 0);
        }
        DXBCGeometryEffectContract* resized =
            (DXBCGeometryEffectContract*)mem_realloc(
                contract->geometry_effects,
                old_capacity * sizeof(*contract->geometry_effects),
                new_capacity * sizeof(*contract->geometry_effects));
        if (!resized) {
            return fail(diagnostic, DXBC_STAGE_CONTRACT_OUT_OF_MEMORY,
                        chunk_index, instruction_index, opcode, 0, 0);
        }
        contract->geometry_effects = resized;
        contract->geometry_effect_capacity = new_capacity;
    }
    DXBCGeometryEffectContract* effect =
        &contract->geometry_effects[contract->geometry_effect_count++];
    effect->kind = kind;
    effect->instruction_index = instruction_index;
    effect->stream_id = stream_id;
    effect->explicit_stream = explicit_stream;
    contract->referenced_stream_mask |= (uint8_t)(1u << stream_id);
    return true;
}

static bool decode_geometry_effect(
    const DXBCDocumentInstruction* instruction, DXBCStageContract* contract,
    DXBCGeometryEffectKind kind, bool explicit_stream,
    DXBCStageContractDiagnostic* diagnostic) {
    uint32_t control;
    uint32_t stream = 0u;
    const uint32_t expected_length = explicit_stream ? 3u : 1u;
    if (!declaration_header(instruction, expected_length, &control,
                            diagnostic)) {
        return false;
    }
    if (control != 0u) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode, 0,
                    control);
    }
    if (explicit_stream) {
        uint32_t operand;
        if (!instruction_token(instruction, 1u, &operand, diagnostic) ||
            !instruction_token(instruction, 2u, &stream, diagnostic)) {
            return false;
        }
        const uint32_t expected_operand = UINT32_C(0x00110000);
        if (operand != expected_operand || stream > 3u) {
            return fail(diagnostic,
                        operand != expected_operand
                            ? DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS
                            : DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
                        instruction->chunk_index,
                        instruction->instruction_index, instruction->opcode,
                        operand != expected_operand ? expected_operand : 3u,
                        operand != expected_operand ? operand : stream);
        }
    }
    return append_geometry_effect(
        contract, kind, instruction->instruction_index, (uint8_t)stream,
        explicit_stream, diagnostic, instruction->chunk_index,
        instruction->opcode);
}

static bool instruction_token(const DXBCDocumentInstruction* instruction,
                              size_t index, uint32_t* out_token,
                              DXBCStageContractDiagnostic* diagnostic) {
    if (dxbc_document_instruction_token(instruction, index, out_token)) {
        return true;
    }
    return fail(diagnostic, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_LENGTH,
                instruction->chunk_index, instruction->instruction_index,
                instruction->opcode, index + 1u, instruction->token_count);
}

static bool declaration_header(
    const DXBCDocumentInstruction* instruction, uint32_t expected_length,
    uint32_t* out_control, DXBCStageContractDiagnostic* diagnostic) {
    uint32_t token;
    if (!instruction_token(instruction, 0, &token, diagnostic)) return false;
    if (instruction->uses_customdata_length ||
        instruction->extended_opcode_token_count != 0u ||
        instruction->token_count != expected_length ||
        instruction->encoded_length != expected_length) {
        return fail(diagnostic,
                    DXBC_STAGE_CONTRACT_INVALID_DECLARATION_LENGTH,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode,
                    expected_length, instruction->token_count);
    }
    if ((token & UINT32_C(0x80000000)) != 0u) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode, 0,
                    token & UINT32_C(0x80000000));
    }
    if (out_control) *out_control = (token >> 11u) & UINT32_C(0x1fff);
    return true;
}

static bool declaration_control(
    const DXBCDocumentInstruction* instruction, uint32_t expected_length,
    uint32_t* out_control, DXBCStageContractDiagnostic* diagnostic) {
    return declaration_header(instruction, expected_length, out_control,
                              diagnostic);
}

static bool scalar_declaration(
    const DXBCDocumentInstruction* instruction, uint32_t* out_value,
    DXBCStageContractDiagnostic* diagnostic) {
    uint32_t control;
    if (!declaration_header(instruction, 2u, &control, diagnostic)) {
        return false;
    }
    if (control != 0u) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode, 0,
                    control);
    }
    return instruction_token(instruction, 1u, out_value, diagnostic);
}

static bool valid_input_primitive(uint32_t value) {
    return value == DXBC_INPUT_PRIMITIVE_POINT ||
           value == DXBC_INPUT_PRIMITIVE_LINE ||
           value == DXBC_INPUT_PRIMITIVE_TRIANGLE ||
           value == DXBC_INPUT_PRIMITIVE_LINE_ADJACENCY ||
           value == DXBC_INPUT_PRIMITIVE_TRIANGLE_ADJACENCY;
}

static bool valid_output_topology(uint32_t value) {
    return value == DXBC_OUTPUT_TOPOLOGY_POINT_LIST ||
           value == DXBC_OUTPUT_TOPOLOGY_LINE_STRIP ||
           value == DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP;
}

static bool valid_domain(uint32_t value) {
    return value >= DXBC_TESSELLATOR_DOMAIN_ISOLINE &&
           value <= DXBC_TESSELLATOR_DOMAIN_QUAD;
}

static bool valid_partitioning(uint32_t value) {
    return value >= DXBC_TESSELLATOR_PARTITIONING_INTEGER &&
           value <= DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN;
}

static bool valid_output_primitive(uint32_t value) {
    return value >= DXBC_TESSELLATOR_OUTPUT_POINT &&
           value <= DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CCW;
}

static bool is_declaration_opcode(uint32_t opcode) {
    return opcode == OP_DCL_IMMEDIATE_CONSTANT_BUFFER ||
           (opcode >= OP_DCL_RESOURCE && opcode <= OP_DCL_GLOBAL_FLAGS) ||
           (opcode >= OP_DCL_STREAM &&
            opcode <= OP_DCL_RESOURCE_STRUCTURED) ||
           opcode == OP_DCL_GS_INSTANCE_COUNT;
}

static bool is_hull_phase_marker(uint32_t opcode) {
    return opcode >= OP_HS_CONTROL_POINT_PHASE &&
           opcode <= OP_HS_JOIN_PHASE;
}

static bool append_phase(DXBCStageContract* contract,
                         DXBCHullPhaseKind kind,
                         uint32_t marker_instruction_index,
                         DXBCStageContractDiagnostic* diagnostic,
                         uint32_t chunk_index, uint32_t opcode) {
    if (contract->hull_phase_count == contract->hull_phase_capacity) {
        size_t old_capacity = contract->hull_phase_capacity;
        size_t new_capacity = old_capacity == 0u ? 4u : old_capacity * 2u;
        if (new_capacity < old_capacity ||
            new_capacity > SIZE_MAX / sizeof(*contract->hull_phases)) {
            return fail(diagnostic, DXBC_STAGE_CONTRACT_OUT_OF_MEMORY,
                        chunk_index, marker_instruction_index, opcode, 0, 0);
        }
        DXBCHullPhaseContract* resized =
            (DXBCHullPhaseContract*)mem_realloc(
                contract->hull_phases,
                old_capacity * sizeof(*contract->hull_phases),
                new_capacity * sizeof(*contract->hull_phases));
        if (!resized) {
            return fail(diagnostic, DXBC_STAGE_CONTRACT_OUT_OF_MEMORY,
                        chunk_index, marker_instruction_index, opcode, 0, 0);
        }
        contract->hull_phases = resized;
        contract->hull_phase_capacity = new_capacity;
    }
    DXBCHullPhaseContract* phase =
        &contract->hull_phases[contract->hull_phase_count++];
    memset(phase, 0, sizeof(*phase));
    phase->kind = kind;
    phase->marker_instruction_index = marker_instruction_index;
    phase->first_instruction_index = marker_instruction_index + 1u;
    phase->end_instruction_index = marker_instruction_index + 1u;
    /* The tokenized format defines one instance when the declaration is
     * absent.  Keep the presence bit so raw omission remains observable. */
    phase->instance_count = 1u;
    return true;
}

static bool decode_stream(const DXBCDocumentInstruction* instruction,
                          DXBCStageContract* contract,
                          DXBCStageContractDiagnostic* diagnostic) {
    uint32_t control;
    uint32_t operand;
    uint32_t stream;
    if (!declaration_header(instruction, 3u, &control, diagnostic) ||
        !instruction_token(instruction, 1u, &operand, diagnostic) ||
        !instruction_token(instruction, 2u, &stream, diagnostic)) {
        return false;
    }
    /* Zero-component STREAM, one immediate32 index: mN. */
    const uint32_t expected_operand = UINT32_C(0x00110000);
    if (control != 0u || operand != expected_operand || stream > 3u) {
        return fail(diagnostic,
                    control != 0u || operand != expected_operand
                        ? DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS
                        : DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode,
                    control != 0u ? 0u :
                        (operand != expected_operand ? expected_operand : 3u),
                    control != 0u ? control :
                        (operand != expected_operand ? operand : stream));
    }
    uint8_t bit = (uint8_t)(1u << stream);
    if ((contract->declared_stream_mask & bit) != 0u) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode, 0,
                    stream);
    }
    contract->declared_stream_mask |= bit;
    return true;
}

static bool decode_enum_declaration(
    const DXBCDocumentInstruction* instruction, bool* present,
    uint32_t* out_value, bool (*validator)(uint32_t),
    DXBCStageContractDiagnostic* diagnostic) {
    uint32_t control;
    if (!declaration_control(instruction, 1u, &control, diagnostic)) {
        return false;
    }
    if (*present) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode, 1,
                    2);
    }
    if (!validator(control)) {
        return fail(diagnostic,
                    DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode, 0,
                    control);
    }
    *present = true;
    *out_value = control;
    return true;
}

static bool decode_count_declaration(
    const DXBCDocumentInstruction* instruction, bool scalar_payload,
    uint32_t maximum, bool* present, uint32_t* out_value,
    DXBCStageContractDiagnostic* diagnostic) {
    uint32_t value;
    if (scalar_payload) {
        if (!scalar_declaration(instruction, &value, diagnostic)) return false;
    } else if (!declaration_control(instruction, 1u, &value, diagnostic)) {
        return false;
    }
    if (*present) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode, 1,
                    2);
    }
    if (value == 0u || value > maximum) {
        return fail(diagnostic,
                    DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
                    instruction->chunk_index,
                    instruction->instruction_index, instruction->opcode,
                    maximum, value);
    }
    *present = true;
    *out_value = value;
    return true;
}

static bool decode_version(const DXBCDocumentChunk* chunk,
                           DXBCStageContract* contract,
                           DXBCStageContractDiagnostic* diagnostic) {
    uint32_t token;
    memcpy(&token, chunk->payload_bytes, sizeof(token));
    token = read_le32(token);
    uint32_t program = token >> 16u;
    uint32_t major = (token >> 4u) & 0xfu;
    uint32_t minor = token & 0xfu;
    if ((token & UINT32_C(0x0000ff00)) != 0u ||
        program >= DXBC_PROGRAM_TYPE_COUNT) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_INVALID_VERSION_TOKEN,
                    chunk->table_index, NO_INSTRUCTION, 0, 0, token);
    }
    if ((major != 4u && major != 5u) ||
        (major == 4u && minor > 1u) ||
        (major == 5u && minor != 0u) ||
        ((program == DXBC_PROGRAM_TYPE_HULL ||
          program == DXBC_PROGRAM_TYPE_DOMAIN) && major != 5u)) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_UNSUPPORTED_PROGRAM,
                    chunk->table_index, NO_INSTRUCTION, 0, 0, token);
    }
    contract->version_token = token;
    contract->program_type = (DXBCProgramType)program;
    contract->shader_model_major = (uint8_t)major;
    contract->shader_model_minor = (uint8_t)minor;
    contract->executable_chunk_index = chunk->table_index;
    return true;
}

static bool stage_mismatch(const DXBCDocumentInstruction* instruction,
                           DXBCStageContractDiagnostic* diagnostic) {
    return fail(diagnostic,
                DXBC_STAGE_CONTRACT_DECLARATION_STAGE_MISMATCH,
                instruction->chunk_index, instruction->instruction_index,
                instruction->opcode, 0, instruction->opcode);
}

static bool decode_instruction_contract(
    const DXBCDocumentInstruction* instruction, DXBCStageContract* contract,
    bool* seen_hs_decls, bool* seen_phase, bool* phase_has_executable,
    bool* seen_any_executable, int* hull_phase_order,
    bool* seen_control_point_phase,
    DXBCStageContractDiagnostic* diagnostic) {
    const uint32_t opcode = instruction->opcode;
    const bool is_hull = contract->program_type == DXBC_PROGRAM_TYPE_HULL;
    const bool is_domain = contract->program_type == DXBC_PROGRAM_TYPE_DOMAIN;
    const bool is_geometry =
        contract->program_type == DXBC_PROGRAM_TYPE_GEOMETRY;

    if (opcode == OP_HS_DECLS) {
        uint32_t control;
        if (!is_hull) return stage_mismatch(instruction, diagnostic);
        if (*seen_hs_decls) {
            return fail(diagnostic,
                        DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION,
                        instruction->chunk_index,
                        instruction->instruction_index, opcode, 1, 2);
        }
        if (instruction->instruction_index !=
            contract->first_instruction_index) {
            return fail(diagnostic, DXBC_STAGE_CONTRACT_DECLARATION_ORDER,
                        instruction->chunk_index,
                        instruction->instruction_index, opcode,
                        contract->first_instruction_index,
                        instruction->instruction_index);
        }
        if (!declaration_header(instruction, 1u, &control, diagnostic)) {
            return false;
        }
        if (control != 0u) {
            return fail(diagnostic,
                        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS,
                        instruction->chunk_index,
                        instruction->instruction_index, opcode, 0, control);
        }
        *seen_hs_decls = true;
        return true;
    }

    if (is_hull_phase_marker(opcode)) {
        uint32_t control;
        if (!is_hull) return stage_mismatch(instruction, diagnostic);
        if (!*seen_hs_decls ||
            !declaration_header(instruction, 1u, &control, diagnostic)) {
            if (!*seen_hs_decls) {
                return fail(diagnostic, DXBC_STAGE_CONTRACT_PHASE_ORDER,
                            instruction->chunk_index,
                            instruction->instruction_index, opcode,
                            OP_HS_DECLS, opcode);
            }
            return false;
        }
        if (control != 0u) {
            return fail(diagnostic,
                        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS,
                        instruction->chunk_index,
                        instruction->instruction_index, opcode, 0, control);
        }
        DXBCHullPhaseKind kind = DXBC_HULL_PHASE_CONTROL_POINT;
        int order = 0;
        if (opcode == OP_HS_FORK_PHASE) {
            kind = DXBC_HULL_PHASE_FORK;
            order = 1;
        } else if (opcode == OP_HS_JOIN_PHASE) {
            kind = DXBC_HULL_PHASE_JOIN;
            order = 2;
        }
        if (order < *hull_phase_order ||
            (kind == DXBC_HULL_PHASE_CONTROL_POINT &&
             *seen_control_point_phase)) {
            return fail(diagnostic, DXBC_STAGE_CONTRACT_PHASE_ORDER,
                        instruction->chunk_index,
                        instruction->instruction_index, opcode,
                        (uint64_t)*hull_phase_order, (uint64_t)order);
        }
        if (contract->hull_phase_count > 0u) {
            contract->hull_phases[contract->hull_phase_count - 1u]
                .end_instruction_index = instruction->instruction_index;
        }
        if (!append_phase(contract, kind, instruction->instruction_index,
                          diagnostic, instruction->chunk_index, opcode)) {
            return false;
        }
        if (kind == DXBC_HULL_PHASE_CONTROL_POINT) {
            *seen_control_point_phase = true;
        }
        *hull_phase_order = order;
        *seen_phase = true;
        *phase_has_executable = false;
        return true;
    }

    const bool stage_level_tess_declaration =
        opcode >= OP_DCL_INPUT_CONTROL_POINT_COUNT &&
        opcode <= OP_DCL_HS_MAX_TESS_FACTOR;
    if ((stage_level_tess_declaration || opcode == OP_DCL_MAX_OUTPUT_VERTEX_COUNT ||
         opcode == OP_DCL_GS_INSTANCE_COUNT || opcode == OP_DCL_STREAM ||
         opcode == OP_DCL_INPUT_PRIMITIVE ||
         opcode == OP_DCL_OUTPUT_TOPOLOGY) &&
        (*seen_any_executable || (is_hull && *seen_phase))) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_DECLARATION_ORDER,
                    instruction->chunk_index,
                    instruction->instruction_index, opcode, 0,
                    instruction->instruction_index);
    }

    uint32_t value;
    switch (opcode) {
        case OP_EMIT:
        case OP_CUT:
        case OP_EMIT_STREAM:
        case OP_CUT_STREAM:
            if (!is_geometry) return stage_mismatch(instruction, diagnostic);
            *seen_any_executable = true;
            return decode_geometry_effect(
                instruction, contract,
                opcode == OP_EMIT || opcode == OP_EMIT_STREAM
                    ? DXBC_GEOMETRY_EFFECT_APPEND
                    : DXBC_GEOMETRY_EFFECT_RESTART_STRIP,
                opcode == OP_EMIT_STREAM || opcode == OP_CUT_STREAM,
                diagnostic);
        case OP_DCL_INPUT_PRIMITIVE:
            if (!is_geometry) return stage_mismatch(instruction, diagnostic);
            if (!decode_enum_declaration(
                    instruction, &contract->has_input_primitive, &value,
                    valid_input_primitive, diagnostic)) {
                return false;
            }
            contract->input_primitive = (DXBCInputPrimitive)value;
            return true;
        case OP_DCL_OUTPUT_TOPOLOGY:
            if (!is_geometry) return stage_mismatch(instruction, diagnostic);
            if (!decode_enum_declaration(
                    instruction, &contract->has_output_topology, &value,
                    valid_output_topology, diagnostic)) {
                return false;
            }
            contract->output_topology = (DXBCOutputTopology)value;
            return true;
        case OP_DCL_MAX_OUTPUT_VERTEX_COUNT:
            if (!is_geometry) return stage_mismatch(instruction, diagnostic);
            return decode_count_declaration(
                instruction, true, 1024u,
                &contract->has_max_output_vertex_count,
                &contract->max_output_vertex_count, diagnostic);
        case OP_DCL_STREAM:
            if (!is_geometry) return stage_mismatch(instruction, diagnostic);
            return decode_stream(instruction, contract, diagnostic);
        case OP_DCL_GS_INSTANCE_COUNT:
            if (!is_geometry) return stage_mismatch(instruction, diagnostic);
            return decode_count_declaration(
                instruction, true, 32u,
                &contract->has_geometry_instance_count,
                &contract->geometry_instance_count, diagnostic);
        case OP_DCL_INPUT_CONTROL_POINT_COUNT:
            if (!is_hull && !is_domain)
                return stage_mismatch(instruction, diagnostic);
            return decode_count_declaration(
                instruction, false, 32u,
                &contract->has_input_control_point_count,
                &contract->input_control_point_count, diagnostic);
        case OP_DCL_OUTPUT_CONTROL_POINT_COUNT:
            if (!is_hull) return stage_mismatch(instruction, diagnostic);
            return decode_count_declaration(
                instruction, false, 32u,
                &contract->has_output_control_point_count,
                &contract->output_control_point_count, diagnostic);
        case OP_DCL_TESSELLATOR_DOMAIN:
            if (!is_hull && !is_domain)
                return stage_mismatch(instruction, diagnostic);
            if (!decode_enum_declaration(
                    instruction, &contract->has_tessellator_domain, &value,
                    valid_domain, diagnostic)) {
                return false;
            }
            contract->tessellator_domain = (DXBCTessellatorDomain)value;
            return true;
        case OP_DCL_TESSELLATOR_PARTITIONING:
            if (!is_hull) return stage_mismatch(instruction, diagnostic);
            if (!decode_enum_declaration(
                    instruction, &contract->has_tessellator_partitioning,
                    &value, valid_partitioning, diagnostic)) {
                return false;
            }
            contract->tessellator_partitioning =
                (DXBCTessellatorPartitioning)value;
            return true;
        case OP_DCL_TESSELLATOR_OUTPUT_PRIMITIVE:
            if (!is_hull) return stage_mismatch(instruction, diagnostic);
            if (!decode_enum_declaration(
                    instruction,
                    &contract->has_tessellator_output_primitive, &value,
                    valid_output_primitive, diagnostic)) {
                return false;
            }
            contract->tessellator_output_primitive =
                (DXBCTessellatorOutputPrimitive)value;
            return true;
        case OP_DCL_HS_MAX_TESS_FACTOR: {
            if (!is_hull) return stage_mismatch(instruction, diagnostic);
            if (contract->has_max_tessellation_factor) {
                return fail(diagnostic,
                            DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION,
                            instruction->chunk_index,
                            instruction->instruction_index, opcode, 1, 2);
            }
            if (!scalar_declaration(instruction, &value, diagnostic)) {
                return false;
            }
            float factor;
            memcpy(&factor, &value, sizeof(factor));
            if ((value & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) ||
                factor < 1.0f || factor > 64.0f) {
                return fail(diagnostic,
                            DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
                            instruction->chunk_index,
                            instruction->instruction_index, opcode, 0, value);
            }
            contract->has_max_tessellation_factor = true;
            contract->max_tessellation_factor_bits = value;
            contract->max_tessellation_factor = factor;
            return true;
        }
        case OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT:
        case OP_DCL_HS_JOIN_PHASE_INSTANCE_COUNT: {
            if (!is_hull || contract->hull_phase_count == 0u) {
                return stage_mismatch(instruction, diagnostic);
            }
            DXBCHullPhaseContract* phase =
                &contract->hull_phases[contract->hull_phase_count - 1u];
            DXBCHullPhaseKind expected =
                opcode == OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT
                    ? DXBC_HULL_PHASE_FORK
                    : DXBC_HULL_PHASE_JOIN;
            if (phase->kind != expected || *phase_has_executable) {
                return fail(diagnostic, DXBC_STAGE_CONTRACT_PHASE_ORDER,
                            instruction->chunk_index,
                            instruction->instruction_index, opcode, expected,
                            phase->kind);
            }
            if (phase->instance_count_declared) {
                return fail(diagnostic,
                            DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION,
                            instruction->chunk_index,
                            instruction->instruction_index, opcode, 1, 2);
            }
            if (!scalar_declaration(instruction, &value, diagnostic)) {
                return false;
            }
            if (value == 0u) {
                return fail(diagnostic,
                            DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
                            instruction->chunk_index,
                            instruction->instruction_index, opcode,
                            UINT32_MAX, value);
            }
            phase->instance_count_declared = true;
            phase->instance_count = value;
            return true;
        }
        default:
            break;
    }

    if (is_declaration_opcode(opcode)) {
        if ((is_hull && *seen_phase && *phase_has_executable) ||
            (!is_hull && *seen_any_executable)) {
            return fail(diagnostic, DXBC_STAGE_CONTRACT_DECLARATION_ORDER,
                        instruction->chunk_index,
                        instruction->instruction_index, opcode, 0,
                        instruction->instruction_index);
        }
        return true;
    }

    if (is_hull && !*seen_phase) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_PHASE_ORDER,
                    instruction->chunk_index,
                    instruction->instruction_index, opcode, 1, 0);
    }
    *seen_any_executable = true;
    if (is_hull) {
        *phase_has_executable = true;
        contract->hull_phases[contract->hull_phase_count - 1u]
            .end_instruction_index = instruction->instruction_index + 1u;
    }
    return true;
}

static bool require_declaration(bool present, uint32_t opcode,
                                const DXBCStageContract* contract,
                                DXBCStageContractDiagnostic* diagnostic) {
    if (present) return true;
    return fail(diagnostic, DXBC_STAGE_CONTRACT_MISSING_DECLARATION,
                contract->executable_chunk_index, NO_INSTRUCTION, opcode, 1,
                0);
}

static bool finish_contract(DXBCStageContract* contract, bool seen_hs_decls,
                            bool seen_phase,
                            DXBCStageContractDiagnostic* diagnostic) {
    switch (contract->program_type) {
        case DXBC_PROGRAM_TYPE_GEOMETRY:
            if (!require_declaration(
                       contract->has_input_primitive,
                       OP_DCL_INPUT_PRIMITIVE, contract, diagnostic)) {
                return false;
            }
            if (!require_declaration(
                       contract->has_output_topology,
                       OP_DCL_OUTPUT_TOPOLOGY, contract, diagnostic) ||
                !require_declaration(
                       contract->has_max_output_vertex_count,
                       OP_DCL_MAX_OUTPUT_VERTEX_COUNT, contract, diagnostic)) {
                return false;
            }
            for (size_t i = 0; i < contract->geometry_effect_count; ++i) {
                const DXBCGeometryEffectContract* effect =
                    &contract->geometry_effects[i];
                if (effect->explicit_stream) {
                    const uint8_t stream_bit =
                        (uint8_t)(1u << effect->stream_id);
                    if ((contract->declared_stream_mask & stream_bit) == 0u) {
                        return fail(
                            diagnostic,
                            DXBC_STAGE_CONTRACT_MISSING_DECLARATION,
                            contract->executable_chunk_index,
                            effect->instruction_index, OP_DCL_STREAM,
                            stream_bit, contract->declared_stream_mask);
                    }
                } else if (contract->declared_stream_mask != 0u) {
                    return fail(diagnostic,
                                DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE,
                                contract->executable_chunk_index,
                                effect->instruction_index,
                                effect->kind == DXBC_GEOMETRY_EFFECT_APPEND
                                    ? OP_EMIT
                                    : OP_CUT,
                                0, contract->declared_stream_mask);
                }
            }
            return true;
        case DXBC_PROGRAM_TYPE_HULL:
            return require_declaration(seen_hs_decls, OP_HS_DECLS, contract,
                                       diagnostic) &&
                   require_declaration(
                       contract->has_input_control_point_count,
                       OP_DCL_INPUT_CONTROL_POINT_COUNT, contract,
                       diagnostic) &&
                   require_declaration(
                       contract->has_output_control_point_count,
                       OP_DCL_OUTPUT_CONTROL_POINT_COUNT, contract,
                       diagnostic) &&
                   require_declaration(
                       contract->has_tessellator_domain,
                       OP_DCL_TESSELLATOR_DOMAIN, contract, diagnostic) &&
                   require_declaration(
                       contract->has_tessellator_partitioning,
                       OP_DCL_TESSELLATOR_PARTITIONING, contract,
                       diagnostic) &&
                   require_declaration(
                       contract->has_tessellator_output_primitive,
                       OP_DCL_TESSELLATOR_OUTPUT_PRIMITIVE, contract,
                       diagnostic) &&
                   require_declaration(seen_phase, OP_HS_FORK_PHASE,
                                       contract, diagnostic);
        case DXBC_PROGRAM_TYPE_DOMAIN:
            return require_declaration(
                       contract->has_input_control_point_count,
                       OP_DCL_INPUT_CONTROL_POINT_COUNT, contract,
                       diagnostic) &&
                   require_declaration(
                       contract->has_tessellator_domain,
                       OP_DCL_TESSELLATOR_DOMAIN, contract, diagnostic);
        default:
            return true;
    }
}

static bool decode_document_internal(
    const DXBCDocument* document, DXBCStageContract* contract,
    DXBCStageContractDiagnostic* diagnostic) {
    if (!document || !contract || !document->owned_bytes ||
        !document->chunks) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_INVALID_ARGUMENT,
                    UINT32_MAX, UINT32_MAX, 0, 0, 0);
    }
    const DXBCDocumentChunk* executable = NULL;
    for (uint32_t i = 0; i < document->chunk_count; ++i) {
        if (document->chunks[i].kind != DXBC_DOCUMENT_CHUNK_EXECUTABLE)
            continue;
        if (executable) {
            return fail(diagnostic,
                        DXBC_STAGE_CONTRACT_MULTIPLE_EXECUTABLES,
                        document->chunks[i].table_index, UINT32_MAX, 0, 1,
                        2);
        }
        executable = &document->chunks[i];
    }
    if (!executable) {
        return fail(diagnostic, DXBC_STAGE_CONTRACT_MISSING_EXECUTABLE,
                    UINT32_MAX, UINT32_MAX, 0, 1, 0);
    }
    if (!decode_version(executable, contract, diagnostic)) return false;

    size_t first = SIZE_MAX;
    size_t end = 0u;
    for (size_t i = 0; i < document->instruction_count; ++i) {
        if (document->instructions[i].chunk_index !=
            executable->table_index) {
            continue;
        }
        if (first == SIZE_MAX) first = i;
        end = i + 1u;
    }
    contract->first_instruction_index =
        first == SIZE_MAX ? 0u :
        document->instructions[first].instruction_index;
    contract->end_instruction_index =
        end == 0u ? 0u :
        document->instructions[end - 1u].instruction_index + 1u;

    bool seen_hs_decls = false;
    bool seen_phase = false;
    bool phase_has_executable = false;
    bool seen_any_executable = false;
    bool seen_control_point_phase = false;
    int hull_phase_order = 0;
    for (size_t i = first == SIZE_MAX ? 0u : first; i < end; ++i) {
        const DXBCDocumentInstruction* instruction =
            &document->instructions[i];
        if (!decode_instruction_contract(
                instruction, contract, &seen_hs_decls, &seen_phase,
                &phase_has_executable, &seen_any_executable,
                &hull_phase_order, &seen_control_point_phase, diagnostic)) {
            return false;
        }
    }
    if (contract->hull_phase_count > 0u) {
        contract->hull_phases[contract->hull_phase_count - 1u]
            .end_instruction_index = contract->end_instruction_index;
    }
    return finish_contract(contract, seen_hs_decls, seen_phase, diagnostic);
}

bool dxbc_stage_contract_decode_document(
    const DXBCDocument* document, DXBCStageContract* out_contract,
    DXBCStageContractDiagnostic* out_diagnostic) {
    clear_diagnostic(out_diagnostic);
    if (!out_contract) {
        return fail(out_diagnostic, DXBC_STAGE_CONTRACT_INVALID_ARGUMENT,
                    UINT32_MAX, UINT32_MAX, 0, 0, 0);
    }
    DXBCStageContract parsed;
    dxbc_stage_contract_init(&parsed);
    if (!decode_document_internal(document, &parsed, out_diagnostic)) {
        dxbc_stage_contract_free(&parsed);
        return false;
    }
    dxbc_stage_contract_free(out_contract);
    *out_contract = parsed;
    return true;
}

bool dxbc_stage_contract_validate_container(
    const DXBCStageContract* contract, const DXBCContainer* container,
    DXBCStageContractDiagnostic* out_diagnostic) {
    clear_diagnostic(out_diagnostic);
    if (!contract || !container ||
        (uint32_t)contract->program_type >= DXBC_PROGRAM_TYPE_COUNT) {
        return fail(out_diagnostic, DXBC_STAGE_CONTRACT_INVALID_ARGUMENT,
                    UINT32_MAX, UINT32_MAX, 0, 0, 0);
    }
    if (!container->has_executable_program ||
        container->program_type != contract->program_type ||
        container->major_version != contract->shader_model_major ||
        container->minor_version != contract->shader_model_minor) {
        const uint64_t expected =
            ((uint64_t)(uint32_t)contract->program_type << 16u) |
            ((uint64_t)contract->shader_model_major << 8u) |
            contract->shader_model_minor;
        const uint64_t actual =
            ((uint64_t)(uint32_t)container->program_type << 16u) |
            ((uint64_t)container->major_version << 8u) |
            container->minor_version;
        return fail(out_diagnostic,
                    DXBC_STAGE_CONTRACT_CONTAINER_MISMATCH,
                    contract->executable_chunk_index, UINT32_MAX, 0,
                    expected, actual);
    }
    return true;
}

bool dxbc_stage_contract_decode(
    const DXBCDocument* document, const DXBCContainer* semantic_container,
    DXBCStageContract* out_contract,
    DXBCStageContractDiagnostic* out_diagnostic) {
    clear_diagnostic(out_diagnostic);
    if (!out_contract) {
        return fail(out_diagnostic, DXBC_STAGE_CONTRACT_INVALID_ARGUMENT,
                    UINT32_MAX, UINT32_MAX, 0, 0, 0);
    }
    DXBCStageContract parsed;
    dxbc_stage_contract_init(&parsed);
    if (!decode_document_internal(document, &parsed, out_diagnostic) ||
        (semantic_container && !dxbc_stage_contract_validate_container(
                                   &parsed, semantic_container,
                                   out_diagnostic))) {
        dxbc_stage_contract_free(&parsed);
        return false;
    }
    dxbc_stage_contract_free(out_contract);
    *out_contract = parsed;
    return true;
}

const char* dxbc_stage_contract_status_name(DXBCStageContractStatus status) {
    switch (status) {
        case DXBC_STAGE_CONTRACT_OK: return "ok";
        case DXBC_STAGE_CONTRACT_INVALID_ARGUMENT: return "invalid_argument";
        case DXBC_STAGE_CONTRACT_OUT_OF_MEMORY: return "out_of_memory";
        case DXBC_STAGE_CONTRACT_MISSING_EXECUTABLE:
            return "missing_executable";
        case DXBC_STAGE_CONTRACT_MULTIPLE_EXECUTABLES:
            return "multiple_executables";
        case DXBC_STAGE_CONTRACT_INVALID_VERSION_TOKEN:
            return "invalid_version_token";
        case DXBC_STAGE_CONTRACT_UNSUPPORTED_PROGRAM:
            return "unsupported_program";
        case DXBC_STAGE_CONTRACT_INVALID_DECLARATION_LENGTH:
            return "invalid_declaration_length";
        case DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS:
            return "invalid_declaration_bits";
        case DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE:
            return "invalid_declaration_value";
        case DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION:
            return "duplicate_declaration";
        case DXBC_STAGE_CONTRACT_DECLARATION_ORDER:
            return "declaration_order";
        case DXBC_STAGE_CONTRACT_DECLARATION_STAGE_MISMATCH:
            return "declaration_stage_mismatch";
        case DXBC_STAGE_CONTRACT_MISSING_DECLARATION:
            return "missing_declaration";
        case DXBC_STAGE_CONTRACT_PHASE_ORDER: return "phase_order";
        case DXBC_STAGE_CONTRACT_CONTAINER_MISMATCH:
            return "container_mismatch";
        default: return "unknown";
    }
}
