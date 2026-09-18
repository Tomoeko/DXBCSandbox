// SPDX-License-Identifier: GPL-3.0-only

#include "io/subprogram_metadata.h"
#include <limits.h>
#include <string.h>

typedef struct {
    uint32_t version;
    PlayerBlobDialect dialect;
} PlayerBlobDialectRecord;

static bool player_blob_dialect_from_version(
    uint32_t version, PlayerBlobDialect* out_dialect) {
    static const PlayerBlobDialectRecord records[] = {
        {UNITY_2021_3_PLAYER_BLOB_VERSION,
         PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1},
    };
    if (!out_dialect) return false;
    *out_dialect = PLAYER_BLOB_DIALECT_INVALID;
    for (size_t index = 0; index < sizeof(records) / sizeof(records[0]);
         ++index) {
        if (records[index].version == version) {
            *out_dialect = records[index].dialect;
            return true;
        }
    }
    return false;
}

static char* read_pascal_string(ByteStream* stream) {
    uint32_t len;
    if (!stream_read_uint32(stream, &len)) return NULL;
    if ((size_t)len > stream_remaining(stream) ||
        dxbc_size_add_overflows((size_t)len, 1U)) return NULL;
    
    char* str = (char*)mem_alloc((size_t)len + 1u);
    if (!str) return NULL;
    
    if (len > 0) {
        if (!stream_read_bytes(stream, (uint8_t*)str, len)) {
            mem_free(str, (size_t)len + 1u);
            return NULL;
        }
    }
    str[len] = '\0';
    /* Every consumer models these Unity identifiers as C strings.  Accepting
     * an embedded NUL would both lose wire identity and make strlen-based
     * ownership accounting free a different size than was allocated. */
    if (len > 0 && memchr(str, '\0', len) != NULL) {
        mem_free(str, (size_t)len + 1u);
        return NULL;
    }
    if (!stream_align(stream, 4)) {
        mem_free(str, (size_t)len + 1u);
        return NULL;
    }
    return str;
}

void serialized_string_pool_init(SerializedStringPool* pool) {
    if (pool) memset(pool, 0, sizeof(*pool));
}

void serialized_string_pool_dispose(SerializedStringPool* pool) {
    if (!pool) return;
    if (pool->strings) {
        for (size_t i = 0; i < pool->count; ++i) {
            if (pool->strings[i]) {
                mem_free(pool->strings[i], strlen(pool->strings[i]) + 1U);
            }
        }
        mem_free(pool->strings, pool->capacity * sizeof(*pool->strings));
    }
    memset(pool, 0, sizeof(*pool));
}

bool serialized_string_pool_take(SerializedStringPool* pool,
                                 char* owned_string,
                                 const char** out_string) {
    if (!pool || !owned_string || !out_string) return false;
    *out_string = NULL;
    if (pool->count == pool->capacity) {
        size_t new_capacity = pool->capacity == 0U ? 8U : pool->capacity * 2U;
        if (new_capacity < pool->capacity ||
            dxbc_size_multiply_overflows(new_capacity,
                                         sizeof(*pool->strings))) {
            return false;
        }
        size_t old_size = pool->capacity * sizeof(*pool->strings);
        size_t new_size = new_capacity * sizeof(*pool->strings);
        char** strings = (char**)mem_realloc(pool->strings, old_size,
                                             new_size);
        if (!strings) return false;
        pool->strings = strings;
        pool->capacity = new_capacity;
    }
    pool->strings[pool->count++] = owned_string;
    *out_string = owned_string;
    return true;
}

bool serialized_string_pool_copy(SerializedStringPool* pool,
                                 const char* source,
                                 const char** out_string) {
    if (!pool || !source || !out_string) return false;
    *out_string = NULL;
    size_t length = strlen(source);
    if (dxbc_size_add_overflows(length, 1U)) return false;
    char* copy = (char*)mem_alloc(length + 1U);
    if (!copy) return false;
    memcpy(copy, source, length + 1U);
    if (!serialized_string_pool_take(pool, copy, out_string)) {
        mem_free(copy, length + 1U);
        return false;
    }
    return true;
}

static bool take_pascal_name(SerializedStringPool* pool, char* name,
                             const char** out_name) {
    if (!name) return false;
    if (serialized_string_pool_take(pool, name, out_name)) return true;
    mem_free(name, strlen(name) + 1U);
    return false;
}

static bool copy_model_name(SerializedStringPool* pool, const char* name,
                            const char** out_name) {
    return serialized_string_pool_copy(pool, name ? name : "", out_name);
}

static bool allocate_zeroed_array(void** out, int count, size_t item_size) {
    if (!out || count < 0 || item_size == 0 ||
        (count > 0 && (size_t)count > SIZE_MAX / item_size)) {
        return false;
    }
    *out = NULL;
    if (count == 0) return true;
    size_t byte_count = (size_t)count * item_size;
    void* values = mem_alloc(byte_count);
    if (!values) return false;
    memset(values, 0, byte_count);
    *out = values;
    return true;
}

static bool wire_count_to_int(uint32_t count, size_t remaining,
                              size_t minimum_item_size, int* out_count) {
    if (!out_count || count > INT_MAX || minimum_item_size == 0 ||
        (size_t)count > remaining / minimum_item_size) {
        return false;
    }
    *out_count = (int)count;
    return true;
}

void serialized_program_parameters_init(SerializedProgramParameters* params) {
    if (params) memset(params, 0, sizeof(*params));
}

void serialized_program_parameters_free(SerializedProgramParameters* params) {
    if (!params) return;
    if (params->constant_buffers && params->cb_count >= 0) {
        for (int cb_index = 0; cb_index < params->cb_count; cb_index++) {
            SerializedConstantBuffer* buffer =
                &params->constant_buffers[cb_index];
            if (buffer->variables && buffer->var_count >= 0) {
                mem_free(buffer->variables,
                         (size_t)buffer->var_count *
                             sizeof(*buffer->variables));
            }
            if (buffer->struct_params && buffer->struct_count >= 0) {
                for (int struct_index = 0;
                     struct_index < buffer->struct_count; struct_index++) {
                    SerializedStructParam* parameter =
                        &buffer->struct_params[struct_index];
                    if (parameter->members && parameter->member_count >= 0) {
                        mem_free(parameter->members,
                                 (size_t)parameter->member_count *
                                     sizeof(*parameter->members));
                    }
                }
                mem_free(buffer->struct_params,
                         (size_t)buffer->struct_count *
                             sizeof(*buffer->struct_params));
            }
        }
        mem_free(params->constant_buffers,
                 (size_t)params->cb_count * sizeof(*params->constant_buffers));
    }
    if (params->resources && params->res_count >= 0) {
        mem_free(params->resources,
                 (size_t)params->res_count * sizeof(*params->resources));
    }
    serialized_string_pool_dispose(&params->owned_strings);
    memset(params, 0, sizeof(*params));
}

bool serialized_program_parameters_copy(SerializedProgramParameters* dest,
                                        const SerializedProgramParameters* src) {
    if (!dest || !src || src->cb_count < 0 || src->res_count < 0 ||
        (src->cb_count > 0 && !src->constant_buffers) ||
        (src->res_count > 0 && !src->resources)) {
        return false;
    }
    if (dest == src) return true;

    SerializedProgramParameters copy;
    serialized_program_parameters_init(&copy);
    copy.version = src->version;
    copy.dialect = src->dialect;
    copy.is_binary = src->is_binary;
    copy.cb_count = src->cb_count;
    copy.res_count = src->res_count;
    if (!allocate_zeroed_array((void**)&copy.constant_buffers, copy.cb_count,
                               sizeof(*copy.constant_buffers)) ||
        !allocate_zeroed_array((void**)&copy.resources, copy.res_count,
                               sizeof(*copy.resources))) {
        goto fail;
    }

    for (int resource_index = 0; resource_index < copy.res_count;
         ++resource_index) {
        const SerializedResourceParam* source_resource =
            &src->resources[resource_index];
        SerializedResourceParam* target_resource =
            &copy.resources[resource_index];
        *target_resource = *source_resource;
        target_resource->name = NULL;
        if (!copy_model_name(&copy.owned_strings, source_resource->name,
                             &target_resource->name)) {
            goto fail;
        }
    }
    for (int cb_index = 0; cb_index < copy.cb_count; cb_index++) {
        const SerializedConstantBuffer* source_buffer =
            &src->constant_buffers[cb_index];
        SerializedConstantBuffer* target_buffer =
            &copy.constant_buffers[cb_index];
        if (source_buffer->var_count < 0 || source_buffer->struct_count < 0 ||
            (source_buffer->var_count > 0 && !source_buffer->variables) ||
            (source_buffer->struct_count > 0 &&
             !source_buffer->struct_params)) {
            goto fail;
        }
        *target_buffer = *source_buffer;
        target_buffer->name = NULL;
        target_buffer->variables = NULL;
        target_buffer->struct_params = NULL;
        if (!copy_model_name(&copy.owned_strings, source_buffer->name,
                             &target_buffer->name)) {
            goto fail;
        }
        if (!allocate_zeroed_array((void**)&target_buffer->variables,
                                   target_buffer->var_count,
                                   sizeof(*target_buffer->variables)) ||
            !allocate_zeroed_array((void**)&target_buffer->struct_params,
                                   target_buffer->struct_count,
                                   sizeof(*target_buffer->struct_params))) {
            goto fail;
        }
        for (int variable_index = 0;
             variable_index < target_buffer->var_count; ++variable_index) {
            const SerializedVariable* source_variable =
                &source_buffer->variables[variable_index];
            SerializedVariable* target_variable =
                &target_buffer->variables[variable_index];
            *target_variable = *source_variable;
            target_variable->name = NULL;
            if (!copy_model_name(&copy.owned_strings, source_variable->name,
                                 &target_variable->name)) {
                goto fail;
            }
        }
        for (int struct_index = 0;
             struct_index < target_buffer->struct_count; struct_index++) {
            const SerializedStructParam* source_parameter =
                &source_buffer->struct_params[struct_index];
            SerializedStructParam* target_parameter =
                &target_buffer->struct_params[struct_index];
            if (source_parameter->member_count < 0 ||
                (source_parameter->member_count > 0 &&
                 !source_parameter->members)) {
                goto fail;
            }
            *target_parameter = *source_parameter;
            target_parameter->name = NULL;
            target_parameter->members = NULL;
            if (!copy_model_name(&copy.owned_strings, source_parameter->name,
                                 &target_parameter->name)) {
                goto fail;
            }
            if (!allocate_zeroed_array((void**)&target_parameter->members,
                                       target_parameter->member_count,
                                       sizeof(*target_parameter->members))) {
                goto fail;
            }
            for (int member_index = 0;
                 member_index < target_parameter->member_count;
                 ++member_index) {
                const SerializedVariable* source_member =
                    &source_parameter->members[member_index];
                SerializedVariable* target_member =
                    &target_parameter->members[member_index];
                *target_member = *source_member;
                target_member->name = NULL;
                if (!copy_model_name(&copy.owned_strings,
                                     source_member->name,
                                     &target_member->name)) {
                    goto fail;
                }
            }
        }
    }

    serialized_program_parameters_free(dest);
    *dest = copy;
    return true;

fail:
    serialized_program_parameters_free(&copy);
    return false;
}

bool subprogram_metadata_parse_variant(
    ByteStream* stream, PlayerSubProgramMetadata* out_sub) {
    if (!stream || !out_sub) return false;
    memset(out_sub, 0, sizeof(*out_sub));
    
    // 1. Read Version
    uint32_t version;
    if (!stream_read_uint32(stream, &version)) return false;
    if (!player_blob_dialect_from_version(version, &out_sub->dialect)) {
        LOG_ERROR("Unsupported subprogram version: %u", version);
        return false;
    }
    out_sub->version = version;
    
    // 2. Read Program Type (Stage / Platform)
    if (!stream_read_int32(stream, &out_sub->program_type) ||
        out_sub->program_type <= 0 || out_sub->program_type > 32) {
        return false;
    }
    
    /* 3. Preserve the four post-type words exactly. UnityPlayer's
     * LoadVariantFromData skips these words without validating or assigning
     * TypeTree semantics to them. */
    for (size_t word_index = 0;
         word_index < sizeof(out_sub->player_header_words) /
                          sizeof(out_sub->player_header_words[0]);
         ++word_index) {
        if (!stream_read_uint32(stream,
                                &out_sub->player_header_words[word_index])) {
            return false;
        }
    }
    out_sub->has_player_blob_header = true;
    
    // 4. Read Local Keywords List
    uint32_t local_kw_count;
    if (!stream_read_uint32(stream, &local_kw_count)) return false;
    if (!wire_count_to_int(local_kw_count, stream_remaining(stream),
                           sizeof(uint32_t),
                           &out_sub->local_keyword_count)) {
        return false;
    }
    if (local_kw_count > 0) {
        if (!allocate_zeroed_array((void**)&out_sub->local_keywords,
                                   out_sub->local_keyword_count,
                                   sizeof(*out_sub->local_keywords))) {
            return false;
        }
        for (uint32_t i = 0; i < local_kw_count; i++) {
            out_sub->local_keywords[i] = read_pascal_string(stream);
            if (!out_sub->local_keywords[i]) goto fail;
        }
    }
    
    /* The verified 2021.3 dialect does not carry the historical inline
     * global-keyword array here.  Global keywords are separate TypeTree
     * authority and must not be guessed from an older player-wire layout. */
    
    // 6. Read Bytecode Length & Payload
    uint32_t bytecode_len;
    if (!stream_read_uint32(stream, &bytecode_len)) goto fail;
    out_sub->bytecode_length = bytecode_len;
    if (bytecode_len > 0) {
        if (bytecode_len > stream_remaining(stream)) goto fail;
        out_sub->bytecode = stream->data + stream->position;
        if (!stream_skip(stream, bytecode_len)) goto fail;
        if (!stream_align(stream, 4)) goto fail;
    }
    
    /* 7. UnityPlayer ORs this exact word into
     * SerializedBindChannels::m_SourceMap. It is not the TypeTree
     * m_ShaderRequirements scalar. */
    if (!stream_read_uint32(stream, &out_sub->source_map)) goto fail;
    
    // 8. Read Vertex Channel Bindings List
    uint32_t binding_count;
    if (!stream_read_uint32(stream, &binding_count)) goto fail;
    if (!wire_count_to_int(binding_count, stream_remaining(stream),
                           2u * sizeof(uint32_t),
                           &out_sub->binding_count) ||
        !allocate_zeroed_array((void**)&out_sub->bindings,
                               out_sub->binding_count,
                               sizeof(*out_sub->bindings))) goto fail;
    for (uint32_t i = 0; i < binding_count; i++) {
        if (!stream_read_uint32(stream, &out_sub->bindings[i].channel)) goto fail;
        if (!stream_read_uint32(stream, &out_sub->bindings[i].component)) goto fail;
        /* UnityPlayer 2021.3 LoadVariantFromData validates ShaderChannel
         * against 14 values and VertexComponent against 31 values before
         * calling SerializedBindChannels::Bind.  Component is an enum, not a
         * four-lane swizzle index. */
        if (out_sub->bindings[i].channel >= 14 ||
            out_sub->bindings[i].component >= 31) goto fail;
    }
    if (stream_remaining(stream) != 0) goto fail;

    return true;

fail:
    subprogram_metadata_free_variant(out_sub);
    return false;
}

bool subprogram_metadata_local_keyword_set_matches(
    const PlayerSubProgramMetadata* player,
    const SerializedSubProgram* serialized) {
    if (!player || !serialized || player->local_keyword_count < 0 ||
        serialized->local_keyword_count < 0 ||
        player->local_keyword_count != serialized->local_keyword_count ||
        (player->local_keyword_count > 0 &&
         (!player->local_keywords || !serialized->local_keywords))) {
        return false;
    }
    const int count = player->local_keyword_count;
    for (int player_index = 0; player_index < count; ++player_index) {
        const char* keyword = player->local_keywords[player_index];
        if (!keyword) return false;
        for (int earlier = 0; earlier < player_index; ++earlier) {
            if (strcmp(keyword, player->local_keywords[earlier]) == 0)
                return false;
        }
        bool found = false;
        for (int serialized_index = 0; serialized_index < count;
             ++serialized_index) {
            const char* candidate =
                serialized->local_keywords[serialized_index];
            if (!candidate) return false;
            if (strcmp(keyword, candidate) == 0) found = true;
        }
        if (!found) return false;
    }
    for (int serialized_index = 0; serialized_index < count;
         ++serialized_index) {
        const char* keyword = serialized->local_keywords[serialized_index];
        if (!keyword) return false;
        for (int earlier = 0; earlier < serialized_index; ++earlier) {
            if (strcmp(keyword, serialized->local_keywords[earlier]) == 0)
                return false;
        }
    }
    return true;
}

void subprogram_metadata_free_variant(PlayerSubProgramMetadata* sub) {
    if (!sub) return;
    if (sub->local_keywords) {
        for (int i = 0; i < sub->local_keyword_count; i++) {
            if (sub->local_keywords[i]) {
                mem_free(sub->local_keywords[i], strlen(sub->local_keywords[i]) + 1);
            }
        }
        mem_free(sub->local_keywords, sub->local_keyword_count * sizeof(char*));
        sub->local_keywords = NULL;
    }
    if (sub->global_keywords) {
        for (int i = 0; i < sub->global_keyword_count; i++) {
            if (sub->global_keywords[i]) {
                mem_free(sub->global_keywords[i], strlen(sub->global_keywords[i]) + 1);
            }
        }
        mem_free(sub->global_keywords, sub->global_keyword_count * sizeof(char*));
        sub->global_keywords = NULL;
    }
    if (sub->bindings && sub->binding_count >= 0) {
        mem_free(sub->bindings,
                 (size_t)sub->binding_count * sizeof(*sub->bindings));
        sub->bindings = NULL;
    }
    sub->binding_count = 0;
}

bool subprogram_metadata_parse_parameters(
    ByteStream* stream, SerializedProgramParameters* out_params) {
    if (!stream || !out_params) return false;

    SerializedProgramParameters parsed;
    serialized_program_parameters_init(&parsed);
    parsed.is_binary = true;

    uint32_t version = 0;
    if (!stream_read_uint32(stream, &version)) goto fail;
    if (!player_blob_dialect_from_version(version, &parsed.dialect)) goto fail;
    parsed.version = version;

    uint32_t wire_cb_count = 0;
    if (!stream_read_uint32(stream, &wire_cb_count) ||
        !wire_count_to_int(wire_cb_count, stream_remaining(stream),
                           16u, &parsed.cb_count) ||
        !allocate_zeroed_array((void**)&parsed.constant_buffers,
                               parsed.cb_count,
                               sizeof(*parsed.constant_buffers))) {
        goto fail;
    }

    for (int cb_index = 0; cb_index < parsed.cb_count; cb_index++) {
        SerializedConstantBuffer* buffer =
            &parsed.constant_buffers[cb_index];
        buffer->role = cb_index == 0
            ? SERIALIZED_CBUFFER_LOOSE_PARAMETERS
            : SERIALIZED_CBUFFER_NAMED;
        char* name = read_pascal_string(stream);
        if (!name) goto fail;
        if (cb_index == 0 && name[0] == '\0') {
            mem_free(name, 1);
            if (!serialized_string_pool_copy(&parsed.owned_strings,
                                             "$Globals", &buffer->name)) {
                goto fail;
            }
        } else {
            if (!take_pascal_name(&parsed.owned_strings, name,
                                  &buffer->name)) goto fail;
        }

        uint32_t wire_var_count = 0;
        if (!stream_read_uint32(stream, &buffer->size) ||
            !stream_read_uint32(stream, &wire_var_count) ||
            !wire_count_to_int(wire_var_count, stream_remaining(stream), 28,
                               &buffer->var_count) ||
            !allocate_zeroed_array((void**)&buffer->variables,
                                   buffer->var_count,
                                   sizeof(*buffer->variables))) {
            goto fail;
        }
        for (int variable_index = 0;
             variable_index < buffer->var_count; variable_index++) {
            SerializedVariable* variable =
                &buffer->variables[variable_index];
            char* variable_name = read_pascal_string(stream);
            if (!variable_name) goto fail;
            if (!take_pascal_name(&parsed.owned_strings, variable_name,
                                  &variable->name)) goto fail;
            for (int layout_index = 0; layout_index < 6; layout_index++) {
                if (!stream_read_uint32(
                        stream, &variable->layout[layout_index])) {
                    goto fail;
                }
            }
        }

        uint32_t wire_struct_count = 0;
        if (!stream_read_uint32(stream, &wire_struct_count) ||
            !wire_count_to_int(wire_struct_count, stream_remaining(stream),
                               20u, &buffer->struct_count) ||
            !allocate_zeroed_array((void**)&buffer->struct_params,
                                   buffer->struct_count,
                                   sizeof(*buffer->struct_params))) {
            goto fail;
        }
        for (int struct_index = 0;
             struct_index < buffer->struct_count; struct_index++) {
            SerializedStructParam* parameter =
                &buffer->struct_params[struct_index];
            char* parameter_name = read_pascal_string(stream);
            if (!parameter_name) goto fail;
            if (!take_pascal_name(&parsed.owned_strings, parameter_name,
                                  &parameter->name)) goto fail;
            for (int layout_index = 0; layout_index < 3; layout_index++) {
                if (!stream_read_uint32(
                        stream, &parameter->layout[layout_index])) {
                    goto fail;
                }
            }
            uint32_t wire_member_count = 0;
            if (!stream_read_uint32(stream, &wire_member_count) ||
                !wire_count_to_int(wire_member_count,
                                   stream_remaining(stream), 28u,
                                   &parameter->member_count) ||
                !allocate_zeroed_array((void**)&parameter->members,
                                       parameter->member_count,
                                       sizeof(*parameter->members))) {
                goto fail;
            }
            for (int member_index = 0;
                 member_index < parameter->member_count; member_index++) {
                SerializedVariable* member =
                    &parameter->members[member_index];
                char* member_name = read_pascal_string(stream);
                if (!member_name) goto fail;
                if (!take_pascal_name(&parsed.owned_strings, member_name,
                                      &member->name)) goto fail;
                for (int layout_index = 0; layout_index < 6;
                     layout_index++) {
                    if (!stream_read_uint32(
                            stream, &member->layout[layout_index])) {
                        goto fail;
                    }
                }
            }
        }
    }

    uint32_t wire_resource_count = 0;
    if (!stream_read_uint32(stream, &wire_resource_count) ||
        !wire_count_to_int(wire_resource_count, stream_remaining(stream), 16,
                           &parsed.res_count) ||
        !allocate_zeroed_array((void**)&parsed.resources, parsed.res_count,
                               sizeof(*parsed.resources))) {
        goto fail;
    }
    for (int resource_index = 0;
         resource_index < parsed.res_count; resource_index++) {
        SerializedResourceParam* resource =
            &parsed.resources[resource_index];
        char* name = read_pascal_string(stream);
        if (!name) goto fail;
        if (!take_pascal_name(&parsed.owned_strings, name,
                              &resource->name)) goto fail;

        int32_t raw_bind_type = -1;
        if (!stream_read_int32(stream, &raw_bind_type) ||
            raw_bind_type < SERIALIZED_RESOURCE_TEXTURE ||
            raw_bind_type > SERIALIZED_RESOURCE_SAMPLER ||
            !stream_read_uint32(stream, &resource->bind_index) ||
            !stream_read_uint32(stream, &resource->extra[0])) {
            goto fail;
        }
        resource->bind_type = (SerializedResourceType)raw_bind_type;
        if (resource->bind_type == SERIALIZED_RESOURCE_TEXTURE &&
            !stream_read_uint32(stream, &resource->extra[1])) {
            goto fail;
        }

        switch (resource->bind_type) {
            case SERIALIZED_RESOURCE_TEXTURE:
                resource->sampler_index = resource->extra[0];
                resource->multisampled =
                    (resource->extra[1] & 1u) != 0;
                resource->dimension =
                    (resource->extra[1] >> 1) & 0x7fu;
                break;
            case SERIALIZED_RESOURCE_CONSTANT_BUFFER:
            case SERIALIZED_RESOURCE_BUFFER:
                resource->array_size = resource->extra[0];
                break;
            case SERIALIZED_RESOURCE_UAV:
                resource->original_index = resource->extra[0];
                break;
            case SERIALIZED_RESOURCE_SAMPLER:
                resource->sampler_state = resource->extra[0];
                break;
        }
    }
    if (stream_remaining(stream) != 0) goto fail;

    serialized_program_parameters_free(out_params);
    *out_params = parsed;
    return true;

fail:
    serialized_program_parameters_free(&parsed);
    return false;
}
