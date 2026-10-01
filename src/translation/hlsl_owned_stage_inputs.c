// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_owned_stage_inputs_internal.h"

#include <stdlib.h>
#include <string.h>

static bool parameters_empty(const SerializedProgramParameters *parameters) {
    return !parameters->version && parameters->dialect == PLAYER_BLOB_DIALECT_INVALID &&
        !parameters->is_binary && !parameters->owned_strings.count &&
        !parameters->owned_strings.capacity && !parameters->owned_strings.strings &&
        !parameters->cb_count && !parameters->constant_buffers &&
        !parameters->res_count && !parameters->resources;
}

static bool inputs_empty(const HLSLOwnedStageInputs *inputs) {
    const PlayerSubProgramMetadata *player = &inputs->player;
    return !inputs->target && !inputs->target_size && !inputs->player_payload && !inputs->player_payload_size &&
        !player->version && player->dialect == PLAYER_BLOB_DIALECT_INVALID && !player->program_type &&
        !player->has_player_blob_header && !player->player_header_words[0] && !player->player_header_words[1] &&
        !player->player_header_words[2] && !player->player_header_words[3] && !player->source_map &&
        !player->local_keyword_count && !player->local_keywords && !player->global_keyword_count &&
        !player->global_keywords && !player->bytecode_length && !player->bytecode &&
        !player->binding_count && !player->bindings && parameters_empty(&inputs->current) && parameters_empty(&inputs->common);
}

void hlsl_owned_stage_inputs_dispose(HLSLOwnedStageInputs *inputs) {
    if (!inputs) return;
    subprogram_metadata_free_variant(&inputs->player);
    serialized_program_parameters_free(&inputs->current);
    serialized_program_parameters_free(&inputs->common);
    free(inputs->target);
    free(inputs->player_payload);
    memset(inputs, 0, sizeof(*inputs));
}

bool hlsl_owned_stage_inputs_capture(HLSLOwnedStageInputs *inputs,
    const uint8_t *target, size_t target_size,
    const uint8_t *player_payload, size_t player_payload_size,
    const SerializedProgramParameters *current,
    const SerializedProgramParameters *common, size_t *aggregate_bytes) {
    if (!inputs || !inputs_empty(inputs) || !target || !target_size || !player_payload || !player_payload_size ||
        !aggregate_bytes || target_size > HLSL_OWNED_STAGE_INPUT_BYTE_LIMIT ||
        player_payload_size > HLSL_OWNED_STAGE_INPUT_BYTE_LIMIT - target_size ||
        *aggregate_bytes > HLSL_OWNED_STAGE_INPUT_BYTE_LIMIT - target_size - player_payload_size) return false;
    HLSLOwnedStageInputs retained = {0};
    retained.target = malloc(target_size);
    retained.player_payload = malloc(player_payload_size);
    if (!retained.target || !retained.player_payload) goto fail;
    retained.target_size = target_size;
    retained.player_payload_size = player_payload_size;
    memcpy(retained.target, target, target_size);
    memcpy(retained.player_payload, player_payload, player_payload_size);
    ByteStream stream;
    stream_init(&stream, retained.player_payload, player_payload_size);
    stream_set_endian(&stream, false);
    if (!subprogram_metadata_parse_variant(&stream, &retained.player) ||
        (current && !serialized_program_parameters_copy(&retained.current, current)) ||
        (common && !serialized_program_parameters_copy(&retained.common, common))) goto fail;
    *inputs = retained;
    *aggregate_bytes += target_size + player_payload_size;
    return true;
fail:
    hlsl_owned_stage_inputs_dispose(&retained);
    return false;
}

bool hlsl_owned_stage_inputs_equal(const HLSLOwnedStageInputs *left,
    const HLSLOwnedStageInputs *right) {
    return left && right && left->target && right->target && left->player_payload && right->player_payload &&
        left->target_size && left->target_size <= HLSL_OWNED_STAGE_INPUT_BYTE_LIMIT &&
        left->player_payload_size && left->player_payload_size <= HLSL_OWNED_STAGE_INPUT_BYTE_LIMIT - left->target_size &&
        left->target_size == right->target_size && left->player_payload_size == right->player_payload_size &&
        !memcmp(left->target, right->target, left->target_size) &&
        !memcmp(left->player_payload, right->player_payload, left->player_payload_size) &&
        subprogram_metadata_variant_equal(&left->player, &right->player) &&
        serialized_program_parameters_equal(&left->current, &right->current) &&
        serialized_program_parameters_equal(&left->common, &right->common);
}
