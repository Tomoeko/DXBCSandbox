// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_OWNED_STAGE_INPUTS_INTERNAL_H
#define HLSL_OWNED_STAGE_INPUTS_INTERNAL_H

#include "io/subprogram_metadata.h"

enum {
    HLSL_OWNED_STAGE_INPUT_ENTRY_LIMIT = 32,
    HLSL_OWNED_STAGE_INPUT_BYTE_LIMIT = 4 * 1024 * 1024
};

/* Complete byte inputs and independent typed metadata for one normal stage
 * emission. Player bytecode borrows only the retained player_payload. Matrix
 * and HULL factories keep their own admission, observations and replay scope. */
typedef struct {
    uint8_t *target, *player_payload;
    size_t target_size, player_payload_size;
    PlayerSubProgramMetadata player;
    SerializedProgramParameters current, common;
} HLSLOwnedStageInputs;

/* Destination must be initialized and empty. Capture reserves the shared
 * target/payload byte budget only after parsing and copying succeed. Failure
 * leaves previous output and the aggregate budget unchanged. NULL parameter
 * projections retain the existing empty-model equality behavior. */
bool hlsl_owned_stage_inputs_capture(HLSLOwnedStageInputs *inputs,
    const uint8_t *target, size_t target_size,
    const uint8_t *player_payload, size_t player_payload_size,
    const SerializedProgramParameters *current,
    const SerializedProgramParameters *common, size_t *aggregate_bytes);
void hlsl_owned_stage_inputs_dispose(HLSLOwnedStageInputs *inputs);
bool hlsl_owned_stage_inputs_equal(const HLSLOwnedStageInputs *left,
    const HLSLOwnedStageInputs *right);

#endif
