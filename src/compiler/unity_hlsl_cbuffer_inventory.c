// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_hlsl_cbuffer_inventory.h"
#include "compiler/unity_hlsl_expansion_internal.h"
#include "common/sha256.h"
#include "translation/hlsl_source_identifier.h"

#include <stdlib.h>
#include <string.h>

void unity_hlsl_cbuffer_inventory_init(UnityHlslCBufferInventory *inventory) {
    if (inventory) memset(inventory, 0, sizeof(*inventory));
}

void unity_hlsl_cbuffer_inventory_dispose(UnityHlslCBufferInventory *inventory) {
    if (!inventory) return;
    free(inventory->fields);
    unity_hlsl_cbuffer_inventory_init(inventory);
}

static bool token_name(const uint8_t *source, SourceToken token, char name[UNITY_HLSL_CBUFFER_NAME_LIMIT]) {
    const size_t length = token.end - token.begin;
    if (token.kind != SOURCE_TOKEN_IDENTIFIER || !length || length >= UNITY_HLSL_CBUFFER_NAME_LIMIT) return false;
    memcpy(name, source + token.begin, length);
    name[length] = 0;
    return hlsl_source_identifier_valid(name);
}

static int selected_block(const uint8_t *source, SourceToken token, const char *const *names, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (source_token_equals(source, token, names[i])) return (int)i;
    return -1;
}

static bool scalar_shape(const uint8_t *source, SourceToken token, UnityHlslCBufferField *field) {
    static const char *const types[] = {"float", "half", "int", "uint", "bool"};
    if (token.kind != SOURCE_TOKEN_IDENTIFIER) return false;
    const size_t length = token.end - token.begin;
    for (unsigned scalar = 0; scalar < sizeof(types) / sizeof(types[0]); ++scalar) {
        const size_t prefix = strlen(types[scalar]);
        if (length < prefix || memcmp(source + token.begin, types[scalar], prefix)) continue;
        const uint8_t *shape = source + token.begin + prefix;
        const size_t remaining = length - prefix;
        field->scalar = (UnityHlslCBufferScalar)scalar;
        field->rows = 1;
        if (!remaining) field->columns = 1;
        else if (remaining == 1 && shape[0] >= '1' && shape[0] <= '4') field->columns = (uint8_t)(shape[0] - '0');
        else if (scalar == UNITY_HLSL_CBUFFER_FLOAT && remaining == 3 && shape[1] == 'x' &&
                 shape[0] >= '1' && shape[0] <= '4' && shape[2] >= '1' && shape[2] <= '4') {
            field->is_matrix = true;
            field->rows = (uint8_t)(shape[0] - '0');
            field->columns = (uint8_t)(shape[2] - '0');
        } else return false;
        return true;
    }
    return false;
}

static UnityHlslCBufferStatus parse_block(const uint8_t *source, const SourceToken *tokens, size_t count,
    size_t *position, size_t selected, UnityHlslCBufferInventory *inventory) {
    size_t at = *position;
    UnityHlslCBufferBlock *block = &inventory->blocks[selected];
    if (block->field_count) return UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION;
    if (count - at < 3 || !source_token_equals(source, tokens[at + 2], "{"))
        return UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION;
    block->source_begin = tokens[at].begin;
    block->first_field = inventory->field_count;
    at += 3;
    uint32_t cursor = 0;
    while (at < count && !source_token_equals(source, tokens[at], "}")) {
        UnityHlslCBufferField field = {.source_begin = tokens[at].begin};
        const bool explicit_column_major = source_token_equals(source, tokens[at], "column_major");
        if (explicit_column_major) ++at;
        if (count - at < 3 || !scalar_shape(source, tokens[at], &field) ||
            (explicit_column_major && !field.is_matrix) ||
            /* Smaller matrix vectors can share final-row padding with the
             * next declaration. Do not guess that layout from physical rows. */
            (field.is_matrix && (field.rows != 4 || field.columns != 4)) ||
            (field.scalar == UNITY_HLSL_CBUFFER_HALF && !inventory->storage.legacy_half_is_float32) ||
            !token_name(source, tokens[at + 1], field.name) || !source_token_equals(source, tokens[at + 2], ";"))
            return UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION;
        for (size_t previous = 0; previous < inventory->field_count; ++previous)
            if (!strcmp(inventory->fields[previous].name, field.name))
                return UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION;
        if (inventory->field_count == UNITY_HLSL_CBUFFER_FIELD_LIMIT) return UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT;
        field.byte_size = field.is_matrix ? (uint32_t)field.columns * 16u : (uint32_t)field.columns * 4u;
        if (field.is_matrix || (cursor & 15u) + field.byte_size > 16u) cursor = (cursor + 15u) & ~15u;
        if (field.byte_size > 65536u - cursor) return UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT;
        field.byte_offset = cursor;
        cursor += field.byte_size;
        field.source_end = tokens[at + 2].end;
        inventory->fields[inventory->field_count++] = field;
        ++block->field_count;
        at += 3;
    }
    if (at == count) return UNITY_HLSL_CBUFFER_MALFORMED_EXPANSION;
    if (!block->field_count) return UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION;
    block->byte_size = (cursor + 15u) & ~15u;
    block->source_end = tokens[at++].end;
    if (at < count && source_token_equals(source, tokens[at], ";")) block->source_end = tokens[at++].end;
    *position = at;
    return UNITY_HLSL_CBUFFER_OK;
}

static bool grouping_valid(const uint8_t *source, const SourceToken *tokens, size_t count) {
    char stack[256];
    size_t depth = 0;
    for (size_t at = 0; at < count; ++at) {
        if (tokens[at].kind != SOURCE_TOKEN_PUNCTUATION) continue;
        const char symbol = (char)source[tokens[at].begin];
        if (symbol == '(' || symbol == '[' || symbol == '{') {
            if (depth == sizeof(stack)) return false;
            stack[depth++] = symbol;
        } else if (symbol == ')' || symbol == ']' || symbol == '}') {
            if (!depth || stack[--depth] != (symbol == ')' ? '(' : symbol == ']' ? '[' : '{')) return false;
        }
    }
    return !depth;
}

/* Skip a declaration initializer, retaining the next same-level declarator.
 * Its identifiers are uses, not new global aliases. This does not inspect or
 * grant semantic authority to the initializer expression. Grouping has already
 * been bounded and validated for the complete token stream. */
static size_t initializer_end(const uint8_t *source, const SourceToken *tokens,
                              size_t count, size_t at) {
    size_t depth = 0;
    for (; at < count; ++at) {
        if (tokens[at].kind != SOURCE_TOKEN_PUNCTUATION) continue;
        const char symbol = (char)source[tokens[at].begin];
        if (!depth && (symbol == ',' || symbol == ';' || symbol == ')' ||
                       symbol == ']' || symbol == '}')) break;
        if (symbol == '(' || symbol == '[' || symbol == '{') ++depth;
        else if (symbol == ')' || symbol == ']' || symbol == '}') --depth;
    }
    return at;
}

/* Other top-level declarations must not supply an alias with an inventoried
 * name. Function bodies and declaration initializer expressions are excluded:
 * ordinary uses require independent AST/read authority. Unknown declarator
 * forms remain conservative; this is not a general HLSL declaration parser. */
static UnityHlslCBufferStatus aliases_valid(const uint8_t *source, const SourceToken *tokens, size_t count,
    const UnityHlslCBufferInventory *inventory) {
    size_t depth = 0, budget = 1048576;
    bool constant_member_body = false;
    for (size_t at = 0; at < count; ++at) {
        bool owned = false;
        for (size_t block = 0; block < inventory->block_count; ++block)
            if (tokens[at].begin >= inventory->blocks[block].source_begin &&
                tokens[at].end <= inventory->blocks[block].source_end) { owned = true; break; }
        if (!owned && (!depth || constant_member_body) && source_token_equals(source, tokens[at], "=")) {
            at = initializer_end(source, tokens, count, at + 1);
            if (at == count) break;
        }
        if (!owned && (!depth || constant_member_body) && tokens[at].kind == SOURCE_TOKEN_IDENTIFIER) {
            for (size_t block = 0; block < inventory->block_count; ++block) {
                if (!budget--) return UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT;
                if (source_token_equals(source, tokens[at], inventory->blocks[block].name))
                    return UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION;
            }
            for (size_t field = 0; field < inventory->field_count; ++field) {
                if (!budget--) return UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT;
                if (source_token_equals(source, tokens[at], inventory->fields[field].name))
                    return UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION;
            }
        }
        if (!depth && (source_token_equals(source, tokens[at], "cbuffer") ||
                       source_token_equals(source, tokens[at], "tbuffer"))) constant_member_body = true;
        if (source_token_equals(source, tokens[at], "{")) ++depth;
        if (source_token_equals(source, tokens[at], "}") && --depth == 0) constant_member_body = false;
    }
    return UNITY_HLSL_CBUFFER_OK;
}

UnityHlslCBufferStatus unity_hlsl_cbuffer_inventory_build(const uint8_t *expanded, size_t size,
    const char *const *names, size_t name_count, UnityHlslCBufferStoragePolicy storage,
    UnityHlslCBufferInventory *out) {
    if (!out || out->fields || out->field_count || out->block_count || (!expanded && size) ||
        !names || !name_count || name_count > UNITY_HLSL_CBUFFER_COUNT_LIMIT)
        return UNITY_HLSL_CBUFFER_INVALID_ARGUMENT;
    UnityHlslCBufferInventory inventory = {.storage = storage, .block_count = name_count};
    for (size_t block = 0; block < name_count; ++block) {
        if (!names[block] || strlen(names[block]) >= UNITY_HLSL_CBUFFER_NAME_LIMIT ||
            !hlsl_source_identifier_valid(names[block])) return UNITY_HLSL_CBUFFER_INVALID_ARGUMENT;
        for (size_t previous = 0; previous < block; ++previous)
            if (!strcmp(names[previous], names[block])) return UNITY_HLSL_CBUFFER_INVALID_ARGUMENT;
        strcpy(inventory.blocks[block].name, names[block]);
    }
    UnityHlslExpansionTokens stream;
    const UnityHlslExpansionStatus lexical = unity_hlsl_expansion_tokenize(expanded, size, &stream);
    if (lexical != UNITY_HLSL_EXPANSION_OK) {
        switch (lexical) {
        case UNITY_HLSL_EXPANSION_LIMIT: return UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT;
        case UNITY_HLSL_EXPANSION_DIRECTIVE: return UNITY_HLSL_CBUFFER_UNSUPPORTED_DIRECTIVE;
        case UNITY_HLSL_EXPANSION_ALLOCATION_FAILED: return UNITY_HLSL_CBUFFER_ALLOCATION_FAILED;
        default: return UNITY_HLSL_CBUFFER_MALFORMED_EXPANSION;
        }
    }
    UnityHlslCBufferStatus status = UNITY_HLSL_CBUFFER_MALFORMED_EXPANSION;
    if (!grouping_valid(expanded, stream.tokens, stream.count)) goto done;
    inventory.fields = calloc(UNITY_HLSL_CBUFFER_FIELD_LIMIT, sizeof(*inventory.fields));
    if (!inventory.fields) { status = UNITY_HLSL_CBUFFER_ALLOCATION_FAILED; goto done; }
    size_t depth = 0;
    for (size_t at = 0; at < stream.count;) {
        if (!depth && source_token_equals(expanded, stream.tokens[at], "cbuffer") && at + 1 < stream.count) {
            const int selected = selected_block(expanded, stream.tokens[at + 1], names, name_count);
            if (selected >= 0) {
                status = parse_block(expanded, stream.tokens, stream.count, &at, (size_t)selected, &inventory);
                if (status != UNITY_HLSL_CBUFFER_OK) goto done;
                continue;
            }
        }
        if (source_token_equals(expanded, stream.tokens[at], "{")) ++depth;
        if (source_token_equals(expanded, stream.tokens[at], "}")) --depth;
        ++at;
    }
    status = UNITY_HLSL_CBUFFER_MISSING_DECLARATION;
    for (size_t block = 0; block < name_count; ++block) if (!inventory.blocks[block].field_count) goto done;
    status = aliases_valid(expanded, stream.tokens, stream.count, &inventory);
    if (status == UNITY_HLSL_CBUFFER_OK) {
        common_sha256(expanded, size, inventory.expansion_digest);
        *out = inventory;
        unity_hlsl_cbuffer_inventory_init(&inventory);
    }
done:
    unity_hlsl_expansion_tokens_dispose(&stream);
    unity_hlsl_cbuffer_inventory_dispose(&inventory);
    return status;
}

bool unity_hlsl_cbuffer_field_matches_float_parameter(const UnityHlslCBufferField *field,
    const SerializedProgramParameters *parameters, const SerializedVariable *variable) {
    DecodedVariableLayout decoded;
    return field && parameters && variable && variable->name &&
        memchr(field->name, 0, sizeof(field->name)) && !strcmp(field->name, variable->name) &&
        field->scalar == UNITY_HLSL_CBUFFER_FLOAT && parameter_layout_decode(parameters, variable, &decoded) &&
        (!field->is_matrix || (field->rows == 4 && field->columns == 4)) &&
        !decoded.scalar_type && !decoded.array_size && field->is_matrix == decoded.is_matrix &&
        field->rows == decoded.rows && field->columns == decoded.columns &&
        field->byte_offset == decoded.byte_offset && field->byte_size == parameter_layout_byte_size(&decoded);
}

bool unity_hlsl_cbuffer_inventory_matches(const UnityHlslCBufferInventory *inventory,
    const uint8_t *expanded, size_t size, const char *const *names, size_t name_count,
    UnityHlslCBufferStoragePolicy storage) {
    if (!inventory || !inventory->fields || inventory->block_count != name_count ||
        inventory->field_count > UNITY_HLSL_CBUFFER_FIELD_LIMIT ||
        inventory->storage.legacy_half_is_float32 != storage.legacy_half_is_float32) return false;
    UnityHlslCBufferInventory replay;
    unity_hlsl_cbuffer_inventory_init(&replay);
    if (unity_hlsl_cbuffer_inventory_build(expanded, size, names, name_count, storage, &replay) != UNITY_HLSL_CBUFFER_OK)
        return false;
    bool same = inventory->field_count == replay.field_count &&
        !memcmp(inventory->expansion_digest, replay.expansion_digest, 32);
    for (size_t block = 0; same && block < name_count; ++block) {
        const UnityHlslCBufferBlock *a = &inventory->blocks[block], *b = &replay.blocks[block];
        same = memchr(a->name, 0, sizeof(a->name)) && !strcmp(a->name, b->name) &&
            a->byte_size == b->byte_size && a->first_field == b->first_field && a->field_count == b->field_count &&
            a->source_begin == b->source_begin && a->source_end == b->source_end;
    }
    for (size_t field = 0; same && field < replay.field_count; ++field) {
        const UnityHlslCBufferField *a = &inventory->fields[field], *b = &replay.fields[field];
        same = memchr(a->name, 0, sizeof(a->name)) && !strcmp(a->name, b->name) && a->scalar == b->scalar &&
            a->rows == b->rows && a->columns == b->columns && a->is_matrix == b->is_matrix &&
            a->byte_offset == b->byte_offset && a->byte_size == b->byte_size &&
            a->source_begin == b->source_begin && a->source_end == b->source_end;
    }
    unity_hlsl_cbuffer_inventory_dispose(&replay);
    return same;
}

const char *unity_hlsl_cbuffer_status_name(UnityHlslCBufferStatus status) {
    switch (status) {
    case UNITY_HLSL_CBUFFER_OK: return "ok";
    case UNITY_HLSL_CBUFFER_INVALID_ARGUMENT: return "invalid-argument";
    case UNITY_HLSL_CBUFFER_ANALYSIS_LIMIT: return "analysis-limit";
    case UNITY_HLSL_CBUFFER_MALFORMED_EXPANSION: return "malformed-expansion";
    case UNITY_HLSL_CBUFFER_UNSUPPORTED_DIRECTIVE: return "unsupported-directive";
    case UNITY_HLSL_CBUFFER_UNSUPPORTED_DECLARATION: return "unsupported-declaration";
    case UNITY_HLSL_CBUFFER_MISSING_DECLARATION: return "missing-declaration";
    case UNITY_HLSL_CBUFFER_CONFLICTING_DECLARATION: return "conflicting-declaration";
    case UNITY_HLSL_CBUFFER_ALLOCATION_FAILED: return "allocation-failed";
    default: return "unknown";
    }
}
