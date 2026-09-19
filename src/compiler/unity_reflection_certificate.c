// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_reflection_certificate.h"

#include "io/parameter_layout.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    UnityCompilerReflectionKind kind;
    const char* name;
    /* Common and residual parameter blocks store struct members as bare names
     * while Unity's callback reports `StructName.MemberName`. */
    const char* name_prefix;
    int32_t values[UNITY_COMPILER_REFLECTION_MAX_VALUES];
    size_t value_count;
} ExpectedReflectionRecord;

static int32_t signed_wire_word(uint32_t word) {
    int32_t value = 0;
    memcpy(&value, &word, sizeof(value));
    return value;
}

void unity_reflection_certificate_report_init(
    UnityReflectionCertificateReport* report) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->expected_record_index = SIZE_MAX;
    report->observed_record_index = SIZE_MAX;
}

static size_t count_set_bits_u32(uint32_t value) {
    size_t count = 0U;
    while (value != 0U) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

static bool player_input_model_is_valid(
    const PlayerSubProgramMetadata* player, size_t* input_count) {
    if (!player || !input_count || player->binding_count < 0 ||
        (player->binding_count > 0 && !player->bindings) ||
        (player->source_map & ~UINT32_C(0x3fff)) != 0U) {
        return false;
    }
    for (int index = 0; index < player->binding_count; ++index) {
        const PlayerSubProgramBindChannel* binding = &player->bindings[index];
        if (binding->channel >= 14U || binding->component >= 31U ||
            (player->source_map & (UINT32_C(1) << binding->channel)) == 0U) {
            return false;
        }
        for (int prior = 0; prior < index; ++prior) {
            /* SerializedBindChannels::Bind replaces an existing entry with
             * the same VertexComponent. A valid final array therefore cannot
             * contain duplicate source channels or destination components. */
            if (player->bindings[prior].channel == binding->channel ||
                player->bindings[prior].component == binding->component) {
                return false;
            }
        }
    }
    *input_count = count_set_bits_u32(player->source_map);
    return true;
}

static bool parameter_block_shape_is_valid(
    const SerializedProgramParameters* parameters, bool expect_binary) {
    if (!parameters || parameters->is_binary != expect_binary ||
        parameters->cb_count < 0 ||
        parameters->res_count < 0 ||
        (parameters->cb_count > 0 && !parameters->constant_buffers) ||
        (parameters->res_count > 0 && !parameters->resources)) {
        return false;
    }

    bool saw_loose_parameters = false;
    for (int cb_index = 0; cb_index < parameters->cb_count; ++cb_index) {
        const SerializedConstantBuffer* buffer =
            &parameters->constant_buffers[cb_index];
        if (!buffer->name ||
            (buffer->role != SERIALIZED_CBUFFER_NAMED &&
             buffer->role != SERIALIZED_CBUFFER_LOOSE_PARAMETERS) ||
            buffer->var_count < 0 ||
            buffer->struct_count < 0 ||
            (!buffer->has_is_partial && buffer->is_partial) ||
            (expect_binary &&
             (buffer->has_is_partial || buffer->is_partial)) ||
            (buffer->var_count > 0 && !buffer->variables) ||
            (buffer->struct_count > 0 && !buffer->struct_params) ||
            buffer->var_count > INT_MAX - buffer->struct_count) {
            return false;
        }
        if (buffer->role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS) {
            if (saw_loose_parameters ||
                buffer->has_is_partial || buffer->is_partial ||
                buffer->struct_count != 0 ||
                strcmp(buffer->name, "$Globals") != 0) {
                return false;
            }
            saw_loose_parameters = true;
        }
        for (int variable = 0; variable < buffer->var_count; ++variable) {
            if (!buffer->variables[variable].name) return false;
        }
        for (int structure = 0; structure < buffer->struct_count;
             ++structure) {
            const SerializedStructParam* parameter =
                &buffer->struct_params[structure];
            if (!parameter->name || parameter->member_count < 0 ||
                (parameter->member_count > 0 && !parameter->members) ||
                parameter->member_count > INT_MAX) {
                return false;
            }
            for (int member = 0; member < parameter->member_count;
                 ++member) {
                if (!parameter->members[member].name) return false;
            }
        }
        for (int prior = 0; prior < cb_index; ++prior) {
            if (buffer->role == SERIALIZED_CBUFFER_NAMED &&
                parameters->constant_buffers[prior].role ==
                    SERIALIZED_CBUFFER_NAMED &&
                strcmp(parameters->constant_buffers[prior].name,
                       buffer->name) == 0) {
                return false;
            }
        }
    }
    for (int resource = 0; resource < parameters->res_count; ++resource) {
        const SerializedResourceParam* value =
            &parameters->resources[resource];
        if (!value->name ||
            value->bind_type < SERIALIZED_RESOURCE_TEXTURE ||
            value->bind_type > SERIALIZED_RESOURCE_SAMPLER) {
            return false;
        }
        switch (value->bind_type) {
            case SERIALIZED_RESOURCE_TEXTURE:
                if (value->extra[0] != value->sampler_index ||
                    value->dimension > 0x7fU ||
                    value->extra[1] !=
                        ((value->dimension << 1U) |
                         (value->multisampled ? 1U : 0U))) {
                    return false;
                }
                break;
            case SERIALIZED_RESOURCE_CONSTANT_BUFFER:
                /* The player parameter stream preserves m_ArraySize here,
                 * including zero for ordinary non-array cbuffers, while the
                 * D3D11 reflection callback reports only name and bind point.
                 * Validate the parser projection but do not invent a callback
                 * constraint for a field Unity does not report. */
                if (value->extra[0] != value->array_size) return false;
                break;
            case SERIALIZED_RESOURCE_BUFFER:
                /* The 2021.3 D3D11 reporter emits a literal BindCount of one;
                 * the player also canonicalizes zero to one on load. */
                if (value->extra[0] != 1U || value->array_size != 1U) {
                    return false;
                }
                break;
            case SERIALIZED_RESOURCE_UAV:
                if (value->extra[0] != value->original_index) return false;
                break;
            case SERIALIZED_RESOURCE_SAMPLER:
                /* Sampler callbacks carry state and bind point only. The
                 * serialized resource name is the canonical empty sentinel. */
                if (value->name[0] != '\0' ||
                    value->extra[0] != value->sampler_state ||
                    value->sampler_state > UINT32_C(0xfff)) {
                    return false;
                }
                break;
        }
    }
    return true;
}

static int find_named_constant_buffer(
    const SerializedProgramParameters* parameters, const char* name) {
    if (!parameters || !name) return -1;
    for (int index = 0; index < parameters->cb_count; ++index) {
        if (parameters->constant_buffers[index].role ==
                SERIALIZED_CBUFFER_NAMED &&
            strcmp(parameters->constant_buffers[index].name, name) == 0) {
            return index;
        }
    }
    return -1;
}

static int find_loose_parameter_buffer(
    const SerializedProgramParameters* parameters) {
    if (!parameters) return -1;
    for (int index = 0; index < parameters->cb_count; ++index) {
        if (parameters->constant_buffers[index].role ==
            SERIALIZED_CBUFFER_LOOSE_PARAMETERS) {
            return index;
        }
    }
    return -1;
}

static void set_expected(
    ExpectedReflectionRecord* record, UnityCompilerReflectionKind kind,
    const char* name, const int32_t* values, size_t value_count) {
    memset(record, 0, sizeof(*record));
    record->kind = kind;
    record->name = name;
    record->value_count = value_count;
    if (value_count > 0U) {
        memcpy(record->values, values,
               value_count * sizeof(*record->values));
    }
}

typedef struct {
    ExpectedReflectionRecord* records;
    size_t capacity;
    size_t cursor;
} ExpectedReflectionSink;

typedef struct {
    const SerializedProgramParameters* parameters;
    const SerializedConstantBuffer* buffer;
} ReflectionBufferSource;

static bool append_expected_record(
    ExpectedReflectionSink* sink, UnityCompilerReflectionKind kind,
    const char* name, const char* name_prefix, const int32_t* values,
    size_t value_count) {
    if (!sink || value_count > UNITY_COMPILER_REFLECTION_MAX_VALUES ||
        (value_count > 0U && !values) || sink->cursor == SIZE_MAX) {
        return false;
    }
    if (sink->records) {
        if (sink->cursor >= sink->capacity) return false;
        set_expected(&sink->records[sink->cursor], kind, name, values,
                     value_count);
        sink->records[sink->cursor].name_prefix = name_prefix;
    }
    ++sink->cursor;
    return true;
}

static bool decode_variable_for_shell(
    const SerializedProgramParameters* parameters,
    const SerializedVariable* variable, uint32_t shell_size,
    bool project_to_shell, DecodedVariableLayout* layout,
    bool* should_append) {
    if (!parameters || !variable || !variable->name || !layout ||
        !should_append || shell_size == 0U ||
        !parameter_layout_decode(parameters, variable, layout)) {
        return false;
    }
    const uint32_t byte_size = parameter_layout_byte_size(layout);
    if (byte_size == 0U || layout->byte_offset > UINT32_MAX - byte_size) {
        return false;
    }
    *should_append = !project_to_shell || layout->byte_offset < shell_size;
    if (!project_to_shell && layout->byte_offset + byte_size > shell_size) {
        return false;
    }
    return true;
}

static bool append_variable_record(
    ExpectedReflectionSink* sink,
    const SerializedProgramParameters* parameters,
    const SerializedVariable* variable, const char* name_prefix,
    uint32_t shell_size, bool project_to_shell) {
    DecodedVariableLayout layout;
    bool should_append = false;
    if (!decode_variable_for_shell(parameters, variable, shell_size,
                                   project_to_shell, &layout,
                                   &should_append)) {
        return false;
    }
    if (!should_append) return true;
    const int32_t values[] = {
        signed_wire_word(layout.byte_offset),
        signed_wire_word(layout.scalar_type),
        layout.is_matrix ? 1 : 0,
        signed_wire_word(layout.rows),
        signed_wire_word(layout.columns),
        signed_wire_word(layout.array_size),
    };
    return append_expected_record(
        sink, UNITY_COMPILER_REFLECTION_CONSTANT, variable->name,
        name_prefix, values, sizeof(values) / sizeof(values[0]));
}

static bool append_input_records(
    const PlayerSubProgramMetadata* player, ExpectedReflectionSink* sink) {
    if (!player || !sink) return false;
    /* SerializedBindChannels::Bind always ORs the ShaderChannel into
     * m_SourceMap, but only stores a binding-array entry when the reflected
     * VertexComponent is not -1. Reconstruct the omitted callbacks from the
     * source-map bits instead of treating the explicit array as a transcript. */
    for (uint32_t channel = 0U; channel < 14U; ++channel) {
        if ((player->source_map & (UINT32_C(1) << channel)) == 0U) continue;
        uint32_t component = UINT32_MAX;
        for (int binding = 0; binding < player->binding_count; ++binding) {
            if (player->bindings[binding].channel == channel) {
                component = player->bindings[binding].component;
                break;
            }
        }
        const int32_t values[] = {
            signed_wire_word(channel),
            signed_wire_word(component),
        };
        if (!append_expected_record(
                sink, UNITY_COMPILER_REFLECTION_INPUT, NULL, NULL, values,
                sizeof(values) / sizeof(values[0]))) {
            return false;
        }
    }
    return true;
}

static bool append_buffer_body_records(
    ExpectedReflectionSink* sink,
    const SerializedProgramParameters* parameters,
    const SerializedConstantBuffer* buffer, uint32_t shell_size) {
    if (!sink || !parameters || !buffer) return false;
    const bool project_to_shell =
        buffer->role == SERIALIZED_CBUFFER_LOOSE_PARAMETERS ||
        (buffer->has_is_partial && buffer->is_partial);
    for (int variable = 0; variable < buffer->var_count; ++variable) {
        if (!append_variable_record(
                sink, parameters, &buffer->variables[variable], NULL,
                shell_size, project_to_shell)) {
            return false;
        }
    }
    for (int structure = 0; structure < buffer->struct_count; ++structure) {
        const SerializedStructParam* parameter =
            &buffer->struct_params[structure];
        if (parameter->layout[1] == 0U || parameter->layout[2] == 0U ||
            parameter->layout[1] >
                (UINT32_MAX - parameter->layout[0]) /
                    parameter->layout[2]) {
            return false;
        }
        const uint32_t end = parameter->layout[0] +
            parameter->layout[1] * parameter->layout[2];
        if (project_to_shell && parameter->layout[0] >= shell_size) continue;
        if (!project_to_shell && end > shell_size) return false;
        const int32_t struct_values[] = {
            signed_wire_word(parameter->layout[0]),
            0,
            2,
            signed_wire_word(parameter->layout[2]),
            1,
            signed_wire_word(parameter->layout[1]),
        };
        if (!append_expected_record(
                sink, UNITY_COMPILER_REFLECTION_CONSTANT, parameter->name,
                NULL, struct_values,
                sizeof(struct_values) / sizeof(struct_values[0]))) {
            return false;
        }
        for (int member = 0; member < parameter->member_count; ++member) {
            /* Member offsets are relative to the struct, not the containing
             * cbuffer.  The containing struct has already been projected. */
            if (!append_variable_record(
                    sink, parameters, &parameter->members[member],
                    parameter->name, UINT32_MAX, false)) {
                return false;
            }
        }
    }
    return true;
}

static bool append_buffer_records(
    ExpectedReflectionSink* sink, const char* shell_name,
    uint32_t shell_size, const ReflectionBufferSource* sources,
    size_t source_count) {
    if (!sink || !shell_name || shell_size == 0U ||
        (source_count > 0U && !sources)) {
        return false;
    }
    const int32_t cb_values[] = {
        signed_wire_word(shell_size),
        UNITY_REFLECTION_CB_VARIABLE_COUNT_UNAVAILABLE,
    };
    if (!append_expected_record(
            sink, UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER, shell_name,
            NULL, cb_values, sizeof(cb_values) / sizeof(cb_values[0]))) {
        return false;
    }
    for (size_t source = 0U; source < source_count; ++source) {
        if (!sources[source].parameters || !sources[source].buffer ||
            !append_buffer_body_records(
                sink, sources[source].parameters, sources[source].buffer,
                shell_size)) {
            return false;
        }
    }
    return true;
}

static bool append_resource_records(
    const SerializedProgramParameters* parameters,
    ExpectedReflectionSink* sink) {
    if (!parameters || !sink) return false;
    for (int resource_index = 0; resource_index < parameters->res_count;
         ++resource_index) {
        const SerializedResourceParam* resource =
            &parameters->resources[resource_index];
        UnityCompilerReflectionKind kind;
        int32_t values[4] = {0};
        size_t value_count = 0U;
        const char* name = resource->name;
        switch (resource->bind_type) {
            case SERIALIZED_RESOURCE_TEXTURE:
                kind = UNITY_COMPILER_REFLECTION_TEXTURE_BINDING;
                values[0] = signed_wire_word(resource->bind_index);
                values[1] = signed_wire_word(resource->sampler_index);
                values[2] = resource->multisampled ? 1 : 0;
                values[3] = signed_wire_word(resource->dimension);
                value_count = 4U;
                break;
            case SERIALIZED_RESOURCE_CONSTANT_BUFFER:
                kind = UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER_BINDING;
                values[0] = signed_wire_word(resource->bind_index);
                value_count = 1U;
                break;
            case SERIALIZED_RESOURCE_BUFFER:
                kind = UNITY_COMPILER_REFLECTION_BUFFER_BINDING;
                values[0] = signed_wire_word(resource->bind_index);
                values[1] = signed_wire_word(resource->array_size);
                value_count = 2U;
                break;
            case SERIALIZED_RESOURCE_UAV:
                kind = UNITY_COMPILER_REFLECTION_UAV_BINDING;
                values[0] = signed_wire_word(resource->bind_index);
                values[1] = signed_wire_word(resource->original_index);
                value_count = 2U;
                break;
            case SERIALIZED_RESOURCE_SAMPLER:
                kind = UNITY_COMPILER_REFLECTION_SAMPLER;
                name = NULL;
                values[0] = signed_wire_word(resource->sampler_state);
                values[1] = signed_wire_word(resource->bind_index);
                value_count = 2U;
                break;
            default:
                return false;
        }
        if (!append_expected_record(
                sink, kind, name, NULL, values, value_count)) {
            return false;
        }
    }
    return true;
}

static bool add_buffer_source(
    ReflectionBufferSource* sources, size_t capacity, size_t* count,
    const SerializedProgramParameters* parameters, int buffer_index) {
    if (!sources || !count || !parameters || buffer_index < 0 ||
        buffer_index >= parameters->cb_count || *count >= capacity) {
        return false;
    }
    sources[*count].parameters = parameters;
    sources[*count].buffer = &parameters->constant_buffers[buffer_index];
    ++*count;
    return true;
}

static bool add_globals_loose_sources(
    ReflectionBufferSource* sources, size_t capacity, size_t* count,
    const SerializedProgramParameters* common, int common_loose,
    const SerializedProgramParameters* residual, int residual_loose) {
    return (common_loose < 0 ||
            add_buffer_source(sources, capacity, count, common,
                              common_loose)) &&
        (residual_loose < 0 ||
         add_buffer_source(sources, capacity, count, residual,
                           residual_loose));
}

static bool loose_buffer_has_body(
    const SerializedProgramParameters* parameters, int buffer_index) {
    if (!parameters || buffer_index < 0 ||
        buffer_index >= parameters->cb_count) {
        return false;
    }
    const SerializedConstantBuffer* buffer =
        &parameters->constant_buffers[buffer_index];
    return buffer->var_count > 0 || buffer->struct_count > 0;
}

static bool build_merged_expected_records(
    const PlayerSubProgramMetadata* player,
    const SerializedProgramParameters* common,
    const SerializedProgramParameters* residual,
    ExpectedReflectionRecord* records, size_t capacity,
    size_t* output_count) {
    size_t ignored_input_count = 0U;
    if (!player || !common || !output_count ||
        !player_input_model_is_valid(player, &ignored_input_count) ||
        !parameter_block_shape_is_valid(common, false) ||
        (residual &&
         !parameter_block_shape_is_valid(residual, true))) {
        return false;
    }
    ExpectedReflectionSink sink = {
        .records = records,
        .capacity = capacity,
    };
    if (!append_input_records(player, &sink)) {
        return false;
    }

    bool has_named_globals = false;
    const int common_loose = find_loose_parameter_buffer(common);
    const int residual_loose = find_loose_parameter_buffer(residual);
    for (int common_index = 0; common_index < common->cb_count;
         ++common_index) {
        const SerializedConstantBuffer* common_buffer =
            &common->constant_buffers[common_index];
        if (common_buffer->role != SERIALIZED_CBUFFER_NAMED) continue;
        const int residual_index = residual
            ? find_named_constant_buffer(residual, common_buffer->name) : -1;
        const bool partial = common_buffer->has_is_partial &&
            common_buffer->is_partial;
        if ((!partial && residual_index >= 0) ||
            (partial && residual_index < 0)) {
            return false;
        }
        const SerializedConstantBuffer* shell = partial
            ? &residual->constant_buffers[residual_index] : common_buffer;
        ReflectionBufferSource sources[4];
        size_t source_count = 0U;
        if (partial &&
            !add_buffer_source(sources, 4U, &source_count, residual,
                               residual_index)) {
            return false;
        }
        if (!add_buffer_source(sources, 4U, &source_count, common,
                               common_index)) {
            return false;
        }
        if (strcmp(common_buffer->name, "$Globals") == 0) {
            has_named_globals = true;
            if (!add_globals_loose_sources(
                    sources, 4U, &source_count, common, common_loose,
                    residual, residual_loose)) {
                return false;
            }
        }
        if (!append_buffer_records(
                &sink, shell->name, shell->size, sources, source_count)) {
            return false;
        }
    }
    if (residual) {
        for (int residual_index = 0;
             residual_index < residual->cb_count; ++residual_index) {
            const SerializedConstantBuffer* residual_buffer =
                &residual->constant_buffers[residual_index];
            if (residual_buffer->role != SERIALIZED_CBUFFER_NAMED) continue;
            const int common_index =
                find_named_constant_buffer(common, residual_buffer->name);
            if (common_index >= 0) continue;
            ReflectionBufferSource sources[3];
            size_t source_count = 0U;
            if (!add_buffer_source(sources, 3U, &source_count, residual,
                                   residual_index)) {
                return false;
            }
            if (strcmp(residual_buffer->name, "$Globals") == 0) {
                has_named_globals = true;
                if (!add_globals_loose_sources(
                        sources, 3U, &source_count, common, common_loose,
                        residual, residual_loose)) {
                    return false;
                }
            }
            if (!append_buffer_records(
                    &sink, residual_buffer->name, residual_buffer->size,
                    sources, source_count)) {
                return false;
            }
        }
    }

    if (!has_named_globals &&
        (loose_buffer_has_body(common, common_loose) ||
         loose_buffer_has_body(residual, residual_loose))) {
        uint32_t shell_size = 0U;
        if (residual_loose >= 0 &&
            residual->constant_buffers[residual_loose].size > 0U) {
            shell_size = residual->constant_buffers[residual_loose].size;
        } else if (common_loose >= 0) {
            shell_size = common->constant_buffers[common_loose].size;
        }
        ReflectionBufferSource sources[2];
        size_t source_count = 0U;
        if (shell_size == 0U || !add_globals_loose_sources(
                sources, 2U, &source_count, common, common_loose,
                residual, residual_loose) ||
            !append_buffer_records(
                &sink, "$Globals", shell_size, sources, source_count)) {
            return false;
        }
    }

    if (!append_resource_records(common, &sink) ||
        (residual && !append_resource_records(residual, &sink))) {
        return false;
    }
    if (records && sink.cursor != capacity) return false;
    *output_count = sink.cursor;
    return true;
}

static bool expected_matches_observed(
    const ExpectedReflectionRecord* expected,
    const UnityCompilerReflectionRecord* observed) {
    if (!expected || !observed || expected->kind != observed->kind) {
        return false;
    }
    if (!expected->name || !observed->name) {
        if (expected->name != NULL || observed->name != NULL) return false;
    } else if (expected->name_prefix) {
        const size_t prefix_size = strlen(expected->name_prefix);
        const size_t name_size = strlen(expected->name);
        if (strlen(observed->name) != prefix_size + 1U + name_size ||
            memcmp(observed->name, expected->name_prefix, prefix_size) != 0 ||
            observed->name[prefix_size] != '.' ||
            memcmp(observed->name + prefix_size + 1U,
                   expected->name, name_size) != 0) {
            return false;
        }
    } else if (strcmp(expected->name, observed->name) != 0) {
        return false;
    }
    if (expected->kind == UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER) {
        /* GpuProgramReflectionReportImpl::OnConstantBuffer retains the name
         * and byte size through SetConstBuffer, but discards D3D's Variables
         * field. Never infer a bound from the serialized used-child set. */
        return expected->value_count == 2U &&
            observed->value_count == 2U &&
            expected->values[0] == observed->values[0];
    }
    return expected->value_count == observed->value_count &&
        memcmp(expected->values, observed->values,
               expected->value_count * sizeof(*expected->values)) == 0;
}

static bool expected_name_matches_observed(
    const ExpectedReflectionRecord* expected,
    const UnityCompilerReflectionRecord* observed) {
    if (!expected || !observed || expected->kind != observed->kind) {
        return false;
    }
    if (!expected->name || !observed->name) {
        return expected->name == NULL && observed->name == NULL;
    }
    if (!expected->name_prefix) {
        return strcmp(expected->name, observed->name) == 0;
    }
    const size_t prefix_size = strlen(expected->name_prefix);
    const size_t name_size = strlen(expected->name);
    return strlen(observed->name) == prefix_size + 1U + name_size &&
        memcmp(observed->name, expected->name_prefix, prefix_size) == 0 &&
        observed->name[prefix_size] == '.' &&
        memcmp(observed->name + prefix_size + 1U,
               expected->name, name_size) == 0;
}

static bool expected_binding_values_match_observed(
    const ExpectedReflectionRecord* expected,
    const UnityCompilerReflectionRecord* observed) {
    return expected && observed && expected->kind == observed->kind &&
        expected->kind != UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER &&
        expected->kind != UNITY_COMPILER_REFLECTION_CONSTANT &&
        expected->value_count == observed->value_count &&
        memcmp(expected->values, observed->values,
               expected->value_count * sizeof(*expected->values)) == 0;
}

static void summarize_name_parts(
    UnityReflectionCertificateRecordSummary* summary,
    const char* prefix, const char* name) {
    if (!summary || !name) return;
    summary->has_name = true;
    const size_t prefix_size = prefix ? strlen(prefix) : 0U;
    const size_t name_size = strlen(name);
    const size_t separator_size = prefix ? 1U : 0U;
    if (prefix_size > SIZE_MAX - separator_size ||
        name_size > SIZE_MAX - prefix_size - separator_size) {
        summary->name_length = SIZE_MAX;
    } else {
        summary->name_length = prefix_size + separator_size + name_size;
    }
    CommonSha256Context sha;
    common_sha256_init(&sha);
    size_t cursor = 0U;
    if (prefix) {
        common_sha256_update(&sha, prefix, prefix_size);
        const char dot = '.';
        common_sha256_update(&sha, &dot, 1U);
        const size_t copy = prefix_size < sizeof(summary->name_preview) - 1U
            ? prefix_size : sizeof(summary->name_preview) - 1U;
        memcpy(summary->name_preview, prefix, copy);
        cursor = copy;
        if (cursor < sizeof(summary->name_preview) - 1U) {
            summary->name_preview[cursor++] = '.';
        }
    }
    common_sha256_update(&sha, name, name_size);
    if (cursor < sizeof(summary->name_preview) - 1U) {
        size_t remaining = sizeof(summary->name_preview) - 1U - cursor;
        if (remaining > name_size) remaining = name_size;
        memcpy(summary->name_preview + cursor, name, remaining);
        cursor += remaining;
    }
    summary->name_preview[cursor] = '\0';
    common_sha256_final(&sha, summary->name_sha256);
}

static void summarize_expected(
    UnityReflectionCertificateRecordSummary* summary,
    const ExpectedReflectionRecord* record) {
    if (!summary || !record) return;
    memset(summary, 0, sizeof(*summary));
    summary->present = true;
    summary->kind = record->kind;
    summary->value_count = record->value_count;
    memcpy(summary->values, record->values,
           record->value_count * sizeof(*record->values));
    summarize_name_parts(summary, record->name_prefix, record->name);
}

static void summarize_observed(
    UnityReflectionCertificateRecordSummary* summary,
    const UnityCompilerReflectionRecord* record) {
    if (!summary || !record) return;
    memset(summary, 0, sizeof(*summary));
    summary->present = true;
    summary->kind = record->kind;
    summary->value_count = record->value_count;
    memcpy(summary->values, record->values,
           record->value_count * sizeof(*record->values));
    summarize_name_parts(summary, NULL, record->name);
}

/* Explicit little-endian words avoid host ABI and struct-padding identity. */
static void binding_hash_word(CommonSha256Context* hash, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static int compare_binding_digests(const void* left, const void* right) {
    return memcmp(left, right, COMMON_SHA256_DIGEST_SIZE);
}

/* Either expected or observed is supplied. Both take the same canonical path,
 * but each digest is derived solely from that side's records. */
static bool fingerprint_bindings(
    const ExpectedReflectionRecord* expected,
    const UnityCompilerReflectionRecord* observed, size_t count,
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (count > SIZE_MAX / COMMON_SHA256_DIGEST_SIZE) return false;
    uint8_t (*rows)[COMMON_SHA256_DIGEST_SIZE] = count
        ? malloc(count * sizeof(*rows)) : NULL;
    if (count && !rows) return false;
    uint8_t owner[COMMON_SHA256_DIGEST_SIZE] = {0};
    bool have_owner = false;
    size_t row_count = 0;
    for (size_t i = 0; i < count; ++i) {
        const UnityCompilerReflectionKind kind = expected
            ? expected[i].kind : observed[i].kind;
        if (kind == UNITY_COMPILER_REFLECTION_STATS) continue;
        const size_t value_count = expected
            ? expected[i].value_count : observed[i].value_count;
        if (kind < UNITY_COMPILER_REFLECTION_INPUT ||
            kind > UNITY_COMPILER_REFLECTION_UAV_BINDING ||
            value_count > UNITY_COMPILER_REFLECTION_MAX_VALUES ||
            (kind == UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER && value_count != 2U) ||
            (kind == UNITY_COMPILER_REFLECTION_CONSTANT && !have_owner)) {
            free(rows);
            return false;
        }
        UnityReflectionCertificateRecordSummary summary;
        if (expected) summarize_expected(&summary, &expected[i]);
        else summarize_observed(&summary, &observed[i]);
        CommonSha256Context hash;
        common_sha256_init(&hash);
        static const char domain[] = "DXBCSandbox.ReflectionBindingRecord.v1";
        common_sha256_update(&hash, domain, sizeof(domain));
        binding_hash_word(&hash, (uint64_t)kind);
        binding_hash_word(&hash, summary.has_name);
        binding_hash_word(&hash, summary.name_length);
        common_sha256_update(&hash, summary.name_sha256, sizeof(summary.name_sha256));
        binding_hash_word(&hash, value_count);
        for (size_t j = 0; j < value_count; ++j) {
            const int32_t value = kind == UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER && j == 1U
                ? UNITY_REFLECTION_CB_VARIABLE_COUNT_UNAVAILABLE : summary.values[j];
            binding_hash_word(&hash, (uint32_t)value);
        }
        if (kind == UNITY_COMPILER_REFLECTION_CONSTANT) {
            common_sha256_update(&hash, owner, sizeof(owner));
        }
        common_sha256_final(&hash, rows[row_count]);
        if (kind == UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER) {
            memcpy(owner, rows[row_count], sizeof(owner));
            have_owner = true;
        } else if (kind != UNITY_COMPILER_REFLECTION_CONSTANT) {
            have_owner = false;
        }
        ++row_count;
    }
    if (row_count > 1U) qsort(rows, row_count, sizeof(*rows), compare_binding_digests);
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char domain[] = "DXBCSandbox.ReflectionBindingSet.v1";
    common_sha256_update(&hash, domain, sizeof(domain));
    binding_hash_word(&hash, row_count);
    if (row_count) common_sha256_update(&hash, rows, row_count * sizeof(*rows));
    common_sha256_final(&hash, digest);
    free(rows);
    return true;
}

static size_t constant_block_end(
    const ExpectedReflectionRecord* expected, size_t expected_count,
    size_t cb_index) {
    size_t end = cb_index + 1U;
    while (end < expected_count &&
           expected[end].kind == UNITY_COMPILER_REFLECTION_CONSTANT) {
        ++end;
    }
    return end;
}

static UnityReflectionCertificateStatus fail_common_record(
    UnityReflectionCertificateReport* report,
    UnityReflectionCertificateStatus status,
    const ExpectedReflectionRecord* expected, size_t expected_index,
    const UnityCompilerReflectionRecord* observed, size_t observed_index) {
    report->status = status;
    if (expected) {
        report->expected_record_index = expected_index;
        summarize_expected(&report->expected_record, expected);
    }
    if (observed) {
        report->observed_record_index = observed_index;
        summarize_observed(&report->observed_record, observed);
    }
    return status;
}

/* Unity stores a common intersection plus an optional per-subprogram
 * residual, not a literal callback transcript. Their validated merge is the
 * exact active named parameter/resource set: every reconstructed record is
 * mandatory and every observed named record must belong to it. Constants are
 * scoped to the most recent constant-buffer callback, matching Unity's sender
 * grouping. Only the raw CB variable-count field is unavailable after the
 * Editor receiver calls SetConstBuffer; the shell size remains exact. */
static UnityReflectionCertificateStatus certify_common_parameters(
    const ExpectedReflectionRecord* expected, size_t expected_count,
    bool* matched, const UnityCompilerReflectionRecord* observed_records,
    size_t observed_record_count, UnityReflectionCertificateReport* report) {
    size_t current_expected_cb = SIZE_MAX;
    bool discarded_cb_count = false;

    for (size_t observed_index = 0U;
         observed_index < observed_record_count; ++observed_index) {
        const UnityCompilerReflectionRecord* observed =
            &observed_records[observed_index];
        if (observed->kind == UNITY_COMPILER_REFLECTION_STATS) {
            continue;
        }
        if (observed->kind != UNITY_COMPILER_REFLECTION_CONSTANT) {
            current_expected_cb = SIZE_MAX;
        }

        size_t found = SIZE_MAX;
        if (observed->kind == UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER) {
            size_t nearest = SIZE_MAX;
            for (size_t index = 0U; index < expected_count; ++index) {
                if (matched[index] || expected[index].kind !=
                        UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER) {
                    continue;
                }
                if (expected_matches_observed(&expected[index], observed)) {
                    found = index;
                    break;
                }
                if (nearest == SIZE_MAX &&
                    expected_name_matches_observed(
                        &expected[index], observed)) {
                    nearest = index;
                }
            }
            if (found == SIZE_MAX) {
                return fail_common_record(
                    report, UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD,
                    nearest == SIZE_MAX ? NULL : &expected[nearest], nearest,
                    observed, observed_index);
            }
            matched[found] = true;
            current_expected_cb = found;
            discarded_cb_count = true;
        } else if (observed->kind == UNITY_COMPILER_REFLECTION_CONSTANT) {
            if (current_expected_cb == SIZE_MAX) {
                return fail_common_record(
                    report, UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD,
                    NULL, SIZE_MAX, observed, observed_index);
            }
            size_t nearest = SIZE_MAX;
            const size_t block_end = constant_block_end(
                expected, expected_count, current_expected_cb);
            for (size_t index = current_expected_cb + 1U;
                 index < block_end; ++index) {
                if (!matched[index] &&
                    expected_matches_observed(&expected[index], observed)) {
                    found = index;
                    break;
                }
                if (nearest == SIZE_MAX && !matched[index] &&
                    expected_name_matches_observed(
                        &expected[index], observed)) {
                    nearest = index;
                }
            }
            if (found == SIZE_MAX) {
                return fail_common_record(
                    report, UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD,
                    nearest == SIZE_MAX ? NULL : &expected[nearest],
                    nearest, observed, observed_index);
            }
            matched[found] = true;
        } else {
            for (size_t index = 0U; index < expected_count; ++index) {
                if (!matched[index] &&
                    expected_matches_observed(&expected[index], observed)) {
                    found = index;
                    break;
                }
            }
            if (found == SIZE_MAX) {
                size_t nearest = SIZE_MAX;
                for (size_t index = 0U; index < expected_count; ++index) {
                    if (!matched[index] &&
                        expected_name_matches_observed(
                            &expected[index], observed)) {
                        nearest = index;
                        break;
                    }
                }
                /* A resource renamed to a synthetic t#/u# identifier keeps
                 * the same wire binding. Pair it with the unmatched expected
                 * record so diagnostics expose both names without treating
                 * the rename as compatible. */
                if (nearest == SIZE_MAX) {
                    for (size_t index = 0U; index < expected_count; ++index) {
                        if (!matched[index] &&
                            expected_binding_values_match_observed(
                                &expected[index], observed)) {
                            nearest = index;
                            break;
                        }
                    }
                }
                return fail_common_record(
                    report, UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD,
                    nearest == SIZE_MAX ? NULL : &expected[nearest],
                    nearest, observed, observed_index);
            }
            matched[found] = true;
        }
        ++report->matched_record_count;
    }

    for (size_t index = 0U; index < expected_count; ++index) {
        if (!matched[index]) {
            return fail_common_record(
                report, UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD,
                &expected[index], index, NULL, SIZE_MAX);
        }
    }
    report->status = discarded_cb_count
        ? UNITY_REFLECTION_CERTIFICATE_COMPATIBLE
        : UNITY_REFLECTION_CERTIFICATE_OK;
    return report->status;
}

UnityReflectionCertificateStatus unity_reflection_certify_d3d11_bindings(
    const PlayerSubProgramMetadata* player,
    const SerializedProgramParameters* common_parameters,
    const SerializedProgramParameters* residual_parameters,
    const UnityCompilerReflectionRecord* observed_records,
    size_t observed_record_count,
    UnityReflectionCertificateReport* report) {
    if (!report) return UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT;
    unity_reflection_certificate_report_init(report);
    if (!player || !common_parameters ||
        (observed_record_count > 0U && !observed_records)) {
        report->status = UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT;
        return report->status;
    }

    report->authority = residual_parameters
        ? UNITY_REFLECTION_AUTHORITY_COMMON_PLUS_RESIDUAL
        : UNITY_REFLECTION_AUTHORITY_COMMON_ONLY;
    for (size_t index = 0U; index < observed_record_count; ++index) {
        if (observed_records[index].value_count > UNITY_COMPILER_REFLECTION_MAX_VALUES ||
            observed_records[index].kind < UNITY_COMPILER_REFLECTION_INPUT ||
            observed_records[index].kind > UNITY_COMPILER_REFLECTION_STATS) {
            report->status = UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT;
            return report->status;
        }
        if (observed_records[index].kind == UNITY_COMPILER_REFLECTION_STATS) {
            ++report->ignored_stats_record_count;
        } else {
            ++report->observed_record_count;
        }
    }

    size_t expected_count = 0U;
    if (!build_merged_expected_records(
            player, common_parameters, residual_parameters, NULL, 0U,
            &expected_count)) {
        report->status = UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA;
        return report->status;
    }
    report->expected_record_count = expected_count;
    if (expected_count > SIZE_MAX / sizeof(ExpectedReflectionRecord)) {
        report->status = UNITY_REFLECTION_CERTIFICATE_OUT_OF_MEMORY;
        return report->status;
    }
    ExpectedReflectionRecord* expected = expected_count > 0U
        ? (ExpectedReflectionRecord*)calloc(expected_count,
                                           sizeof(*expected))
        : NULL;
    bool* matched = expected_count > 0U
        ? (bool*)calloc(expected_count, sizeof(*matched))
        : NULL;
    if (expected_count > 0U && (!expected || !matched)) {
        free(expected);
        free(matched);
        report->status = UNITY_REFLECTION_CERTIFICATE_OUT_OF_MEMORY;
        return report->status;
    }
    size_t built_count = 0U;
    if (!build_merged_expected_records(
            player, common_parameters, residual_parameters,
            expected, expected_count, &built_count) ||
        built_count != expected_count) {
        free(expected);
        free(matched);
        report->status = UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA;
        return report->status;
    }

    report->expected_bindings_digest_valid = fingerprint_bindings(
        expected, NULL, expected_count, report->expected_bindings_digest);
    report->observed_bindings_digest_valid = fingerprint_bindings(
        NULL, observed_records, observed_record_count, report->observed_bindings_digest);

    const UnityReflectionCertificateStatus compatibility_status =
        certify_common_parameters(
            expected, expected_count, matched, observed_records,
            observed_record_count, report);
    free(expected);
    free(matched);
    return compatibility_status;
}

const char* unity_reflection_certificate_status_name(
    UnityReflectionCertificateStatus status) {
    switch (status) {
        case UNITY_REFLECTION_CERTIFICATE_OK: return "ok";
        case UNITY_REFLECTION_CERTIFICATE_COMPATIBLE: return "compatible";
        case UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT:
            return "invalid-argument";
        case UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA:
            return "invalid-metadata";
        case UNITY_REFLECTION_CERTIFICATE_OUT_OF_MEMORY:
            return "out-of-memory";
        case UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD:
            return "extra-record";
        case UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD:
            return "missing-record";
    }
    return "unknown";
}

const char* unity_compiler_reflection_kind_name(
    UnityCompilerReflectionKind kind) {
    switch (kind) {
        case UNITY_COMPILER_REFLECTION_INPUT: return "input";
        case UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER:
            return "constant-buffer";
        case UNITY_COMPILER_REFLECTION_CONSTANT: return "constant";
        case UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER_BINDING:
            return "constant-buffer-binding";
        case UNITY_COMPILER_REFLECTION_TEXTURE_BINDING:
            return "texture-binding";
        case UNITY_COMPILER_REFLECTION_SAMPLER: return "sampler";
        case UNITY_COMPILER_REFLECTION_BUFFER_BINDING:
            return "buffer-binding";
        case UNITY_COMPILER_REFLECTION_UAV_BINDING:
            return "uav-binding";
        case UNITY_COMPILER_REFLECTION_STATS: return "stats";
    }
    return "unknown";
}

const char* unity_reflection_certificate_authority_name(
    UnityReflectionCertificateAuthority authority) {
    switch (authority) {
        case UNITY_REFLECTION_AUTHORITY_COMMON_ONLY:
            return "common-only";
        case UNITY_REFLECTION_AUTHORITY_COMMON_PLUS_RESIDUAL:
            return "common-plus-residual";
    }
    return "unknown";
}
