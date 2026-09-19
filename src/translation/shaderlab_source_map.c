// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitter_internal.h"

#include <stdlib.h>
#include <string.h>

void shaderlab_expression_source_map_free(ShaderLabExpressionSourceMap *map) {
    if (!map)
        return;
    free(map->records);
    memset(map, 0, sizeof(*map));
}

bool shaderlab_expression_source_map_append(ShaderLabExpressionSourceMap *map,
                                            const ShaderLabExpressionSourceRecord *record) {
    if (!map || !record || !record->instructions.complete || map->count > map->capacity ||
        (map->capacity && !map->records))
        return false;
    if (map->count == map->capacity) {
        const size_t capacity = map->capacity ? map->capacity * 2 : 8;
        if (capacity <= map->capacity || capacity > SIZE_MAX / sizeof(*map->records))
            return false;
        ShaderLabExpressionSourceRecord *records =
            realloc(map->records, capacity * sizeof(*records));
        if (!records)
            return false;
        map->records = records;
        map->capacity = capacity;
    }
    map->records[map->count++] = *record;
    return true;
}

/* Rebase only instruction-owned ranges. Dead/NOP dispositions keep zero
 * ranges; treating those zeros as source offsets would invent ownership. */
bool shaderlab_expression_source_map_offset(ShaderLabExpressionSourceMap *map, size_t first_record,
                                            size_t offset) {
    if (!map)
        return true;
    if (first_record > map->count)
        return false;
    for (size_t i = first_record; i < map->count; ++i) {
        HLSLExpressionSourceMap *instructions = &map->records[i].instructions;
        if (instructions->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT)
            return false;
        for (size_t j = 0; j < instructions->count; ++j) {
            HLSLExpressionOrigin *origin = &instructions->origins[j];
            if (origin->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION &&
                origin->kind != HLSL_EXPRESSION_ORIGIN_RETURN)
                continue;
            if (origin->source_begin > origin->source_end || origin->source_end > SIZE_MAX - offset)
                return false;
            origin->source_begin += offset;
            origin->source_end += offset;
        }
    }
    return true;
}

bool shaderlab_expression_source_map_matches_source(const ShaderLabExpressionSourceMap *map,
                                                    const StringBuilder *source) {
    if (!map || !map->complete || !source || !sb_ok(source) || !source->buf ||
        map->source_size != source->len || map->count > map->capacity ||
        (map->count && !map->records))
        return false;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source->buf, source->len, digest);
    if (memcmp(digest, map->source_digest, sizeof(digest)) != 0)
        return false;
    for (size_t i = 0; i < map->count; ++i) {
        const ShaderLabExpressionSourceRecord *record = &map->records[i];
        const HLSLExpressionSourceMap *instructions = &record->instructions;
        if (record->subshader_index < 0 || record->pass_index < 0 || record->stage_index < 0 ||
            record->stage_index > 4 || record->subprogram_index < 0 || record->blob_index < 0 ||
            record->hardware_tier_group < 0 || record->hardware_tier_group > 3 ||
            !instructions->complete || !instructions->count ||
            instructions->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT)
            return false;
        for (size_t j = 0; j < instructions->count; ++j) {
            const HLSLExpressionOrigin *origin = &instructions->origins[j];
            if (origin->instruction_index != (int)j || origin->destination_lanes > 15)
                return false;
            switch (origin->kind) {
            case HLSL_EXPRESSION_ORIGIN_EXPRESSION:
            case HLSL_EXPRESSION_ORIGIN_RETURN:
                if (origin->source_begin >= origin->source_end || origin->source_end > source->len)
                    return false;
                break;
            case HLSL_EXPRESSION_ORIGIN_DEAD:
            case HLSL_EXPRESSION_ORIGIN_NOP:
                if (origin->source_begin || origin->source_end)
                    return false;
                break;
            default:
                return false;
            }
        }
    }
    return true;
}
