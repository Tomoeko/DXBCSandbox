// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_HULL_ICB_INTERNAL_H
#define HLSL_HULL_ICB_INTERNAL_H

#include "translation/usil.h"

enum {
    HLSL_HULL_ICB_WORD_LIMIT = 16,
    HLSL_HULL_ICB_ROW_LIMIT = 4,
    HLSL_HULL_ICB_CONSUMER_LIMIT = 64,
    HLSL_HULL_ICB_TRANSPORT_LIMIT = 64,
    HLSL_HULL_ICB_NAME_LIMIT = 96
};

#define HLSL_HULL_ICB_ACCESS_LOGICAL_ID_BASE UINT64_C(0x8000000000040000)

/* Pointer-free snapshots of the complete admitted operand form. Acquisition
 * rejects extensions, modifiers, precision and immediate payload, and retains
 * the one admitted relative child separately. Diagnostic text is not authority. */
typedef struct {
    DXBCOperandType type;
    uint32_t raw_token;
    int register_index, register_index_dim, rel_offsets[3];
    uint8_t swizzle[4], swizzle_mode, destination_mask;
    uint64_t index_values[3];
    uint8_t index_representations[3];
    bool index_has_immediate[3], index_value_exceeds_int[3];
} HLSLHullICBOperandSnapshot;

typedef struct {
    int instruction_index;
    uint32_t source_instruction_index;
    HLSLHullICBOperandSnapshot destination, source;
    uint8_t destination_lane, source_lane;
    /* -1 identifies the actual declared ForkID source. */
    int predecessor_transport;
} HLSLHullICBTransport;

typedef struct {
    int instruction_index, operand_index;
    uint32_t source_instruction_index;
    uint8_t demanded_lanes, physical_column;
    HLSLHullICBOperandSnapshot operand, relative;
    bool direct_fork_id;
    /* -1 is valid only for the actual declared ForkID source. */
    int transport_tail;
} HLSLHullICBConsumer;

/* The complete bounded plan is independently owned by value. Absent plans
 * have canonical zero fields; consumers are ordered by actual coordinates and
 * shared MOV chains reference one unique transport table. */
typedef struct {
    bool present;
    uint32_t declaration_source_instruction_index, declaration_token, declaration_word_count;
    uint32_t payload[HLSL_HULL_ICB_WORD_LIMIT];
    size_t payload_count, row_count;
    uint8_t physical_column;
    int phase_index;
    USILHullPhase phase;
    size_t fork_declaration_index;
    USILSignatureDeclaration fork_declaration;
    char array_name[HLSL_HULL_ICB_NAME_LIMIT], index_name[HLSL_HULL_ICB_NAME_LIMIT];
    HLSLHullICBConsumer consumers[HLSL_HULL_ICB_CONSUMER_LIMIT];
    size_t consumer_count;
    HLSLHullICBTransport transports[HLSL_HULL_ICB_TRANSPORT_LIMIT];
    size_t transport_count;
} HLSLHullICBPlan;

/* Typed equality includes every retained coordinate and payload bit. The
 * producer rebuilds the actual declaration/phase/SSA evidence for matches. */
bool hlsl_hull_icb_plans_equal(const HLSLHullICBPlan *left, const HLSLHullICBPlan *right);
bool hlsl_hull_icb_operands_equal(const HLSLHullICBOperandSnapshot *left, const HLSLHullICBOperandSnapshot *right);
bool hlsl_hull_icb_consumers_equal(const HLSLHullICBConsumer *left, const HLSLHullICBConsumer *right);
bool hlsl_hull_icb_plan_matches(const USILProgram *program, const HLSLHullICBPlan *plan);

#endif
