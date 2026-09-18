#include "common/oracle_metadata.h"
#include "common/sha256.h"
#include "common/variant_key.h"
#include "dxbc/dxbc_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define FIXTURE_DXBC_SIZE 52U

static uint32_t read_u32_le(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t read_u64_le(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned byte = 0; byte < 8U; byte++) {
        value |= (uint64_t)data[byte] << (byte * 8U);
    }
    return value;
}

static void write_u32_le(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static void* tracked_array(int count, size_t element_size) {
    if (count <= 0 || element_size == 0U ||
        (size_t)count > SIZE_MAX / element_size) {
        return NULL;
    }
    size_t size = (size_t)count * element_size;
    void* result = mem_alloc(size);
    if (result) memset(result, 0, size);
    return result;
}

static bool set_name(SerializedProgramParameters* parameters,
                     const char** name, const char* value) {
    return serialized_string_pool_copy(&parameters->owned_strings, value,
                                       name);
}

static bool set_variable(SerializedProgramParameters* parameters,
                         SerializedVariable* variable, const char* name,
                         uint32_t base) {
    memset(variable, 0, sizeof(*variable));
    if (!set_name(parameters, &variable->name, name)) return false;
    for (size_t word = 0; word < 6U; word++) {
        variable->layout[word] = base + (uint32_t)word;
    }
    return true;
}

static bool set_resource(SerializedProgramParameters* parameters,
                         SerializedResourceParam* resource,
                         const char* name, SerializedResourceType type,
                         uint32_t base) {
    memset(resource, 0, sizeof(*resource));
    if (!set_name(parameters, &resource->name, name)) return false;
    resource->bind_type = type;
    resource->bind_index = base + 1U;
    resource->array_size = base + 2U;
    resource->dimension = base + 3U;
    resource->sampler_index = base + 4U;
    resource->multisampled = (base & 1U) != 0U;
    resource->original_index = base + 5U;
    resource->sampler_state = base + 6U;
    resource->extra[0] = base + 7U;
    resource->extra[1] = base + 8U;
    return true;
}

static int make_fixture(SerializedProgramParameters* parameters) {
    serialized_program_parameters_init(parameters);
    parameters->version = UNITY_2021_3_PLAYER_BLOB_VERSION;
    parameters->is_binary = true;
    parameters->cb_count = 2;
    parameters->constant_buffers = (SerializedConstantBuffer*)tracked_array(
        parameters->cb_count, sizeof(*parameters->constant_buffers));
    CHECK(parameters->constant_buffers != NULL);

    SerializedConstantBuffer* globals = &parameters->constant_buffers[0];
    CHECK(set_name(parameters, &globals->name, "$Globals"));
    globals->role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
    globals->size = 144U;
    globals->has_is_partial = true;
    globals->is_partial = false;
    globals->var_count = 2;
    globals->variables = (SerializedVariable*)tracked_array(
        globals->var_count, sizeof(*globals->variables));
    CHECK(globals->variables != NULL);
    CHECK(set_variable(parameters, &globals->variables[0], "_Tint", 10U));
    CHECK(set_variable(parameters, &globals->variables[1], "_Cutoff", 20U));
    globals->struct_count = 1;
    globals->struct_params = (SerializedStructParam*)tracked_array(
        globals->struct_count, sizeof(*globals->struct_params));
    CHECK(globals->struct_params != NULL);
    SerializedStructParam* structure = &globals->struct_params[0];
    CHECK(set_name(parameters, &structure->name, "LightData"));
    structure->layout[0] = 31U;
    structure->layout[1] = 32U;
    structure->layout[2] = 33U;
    structure->member_count = 2;
    structure->members = (SerializedVariable*)tracked_array(
        structure->member_count, sizeof(*structure->members));
    CHECK(structure->members != NULL);
    CHECK(set_variable(parameters, &structure->members[0], "color", 40U));
    CHECK(set_variable(parameters, &structure->members[1], "direction", 50U));

    SerializedConstantBuffer* per_draw = &parameters->constant_buffers[1];
    CHECK(set_name(parameters, &per_draw->name, "UnityPerDraw"));
    per_draw->role = SERIALIZED_CBUFFER_NAMED;
    per_draw->size = 208U;
    per_draw->has_is_partial = true;
    per_draw->is_partial = true;

    parameters->res_count = 3;
    parameters->resources = (SerializedResourceParam*)tracked_array(
        parameters->res_count, sizeof(*parameters->resources));
    CHECK(parameters->resources != NULL);
    CHECK(set_resource(parameters, &parameters->resources[0], "_MainTex",
                       SERIALIZED_RESOURCE_TEXTURE, 60U));
    CHECK(set_resource(parameters, &parameters->resources[1], "_Data",
                       SERIALIZED_RESOURCE_BUFFER, 70U));
    CHECK(set_resource(parameters, &parameters->resources[2], "_Output",
                       SERIALIZED_RESOURCE_UAV, 80U));
    return 0;
}

static bool normalization_bytes_equal(
    const OracleMetadataNormalization* left,
    const OracleMetadataNormalization* right) {
    return left->pack_input.canonical_bytes.size ==
               right->pack_input.canonical_bytes.size &&
           memcmp(left->pack_input.canonical_bytes.data,
                  right->pack_input.canonical_bytes.data,
                  left->pack_input.canonical_bytes.size) == 0;
}

static bool digest_matches_hex(const uint8_t digest[32], const char* hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t byte = 0; byte < 32U; byte++) {
        if (hex[byte * 2U] != digits[digest[byte] >> 4U] ||
            hex[byte * 2U + 1U] != digits[digest[byte] & 15U]) {
            fprintf(stderr, "canonical digest: ");
            for (size_t i = 0; i < 32U; i++) {
                fprintf(stderr, "%02x", digest[i]);
            }
            fputc('\n', stderr);
            return false;
        }
    }
    return hex[64] == '\0';
}

static int normalize_and_require_difference(
    const SerializedProgramParameters* parameters,
    const OracleMetadataNormalization* baseline) {
    OracleMetadataNormalization changed;
    oracle_metadata_normalization_init(&changed);
    CHECK(oracle_metadata_normalize(parameters, NULL, 0U, &changed) ==
          ORACLE_METADATA_OK);
    CHECK(!normalization_bytes_equal(baseline, &changed));
    oracle_metadata_normalization_free(&changed);
    return 0;
}

static int test_canonical_bytes_fields_order_and_precision(void) {
    SerializedProgramParameters parameters;
    CHECK(make_fixture(&parameters) == 0);

    OracleMetadataNormalization baseline;
    oracle_metadata_normalization_init(&baseline);
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &baseline) ==
          ORACLE_METADATA_OK);
    OraclePackBytes bytes = baseline.pack_input.canonical_bytes;
    CHECK(bytes.data != NULL && bytes.size > ORACLE_METADATA_HEADER_SIZE);
    CHECK(memcmp(bytes.data, "DXBCMETA", 8U) == 0);
    CHECK(ORACLE_METADATA_FORMAT_VERSION == 2U);
    CHECK(read_u32_le(bytes.data + 8U) == ORACLE_METADATA_FORMAT_VERSION);
    CHECK(read_u32_le(bytes.data + 12U) == ORACLE_METADATA_HEADER_SIZE);
    CHECK(read_u64_le(bytes.data + 16U) == bytes.size);
    CHECK(read_u32_le(bytes.data + 24U) == parameters.version);
    CHECK(read_u32_le(bytes.data + 28U) == 1U);
    CHECK(read_u64_le(bytes.data + ORACLE_METADATA_HEADER_SIZE) == 2U);
    CHECK(bytes.size == 463U);
    const size_t first_buffer_role_offset =
        ORACLE_METADATA_HEADER_SIZE + 8U + 4U + strlen("$Globals");
    CHECK(read_u32_le(bytes.data + first_buffer_role_offset) ==
          SERIALIZED_CBUFFER_LOOSE_PARAMETERS);
    CHECK(baseline.pack_input.resource_precision_count == 3U);
    CHECK(baseline.pack_input.resource_precisions != NULL);
    for (size_t resource = 0; resource < 3U; resource++) {
        const OraclePackResourcePrecisionInput* precision =
            &baseline.pack_input.resource_precisions[resource];
        CHECK(precision->precision ==
              ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN);
        CHECK(precision->resource_key.data != NULL);
        CHECK(precision->resource_key.size >=
              ORACLE_METADATA_RESOURCE_KEY_HEADER_SIZE);
        CHECK(memcmp(precision->resource_key.data, "DXBCRKEY", 8U) == 0);
        CHECK(read_u32_le(precision->resource_key.data + 8U) ==
              ORACLE_METADATA_RESOURCE_KEY_FORMAT_VERSION);
        CHECK(read_u32_le(precision->resource_key.data + 12U) ==
              ORACLE_METADATA_RESOURCE_KEY_HEADER_SIZE);
        CHECK(read_u64_le(precision->resource_key.data + 16U) ==
              precision->resource_key.size);
        CHECK(read_u64_le(precision->resource_key.data + 24U) == resource);
        CHECK(read_u32_le(precision->resource_key.data + 32U) ==
              (uint32_t)parameters.resources[resource].bind_type);
        CHECK(read_u32_le(precision->resource_key.data + 36U) ==
              parameters.resources[resource].bind_index);
    }

    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(bytes.data, bytes.size, digest);
    CHECK(digest_matches_hex(
        digest,
        "df3fb210fa145b2644604e9ee3b85747"
        "57e3d852d54d1f781c65171e8a264815"));

    OracleMetadataPrecisionOverride override = {
        1U, ORACLE_PACK_RESOURCE_PRECISION_HIGH
    };
    OracleMetadataNormalization authoritative;
    oracle_metadata_normalization_init(&authoritative);
    CHECK(oracle_metadata_normalize(&parameters, &override, 1U,
                                    &authoritative) == ORACLE_METADATA_OK);
    CHECK(normalization_bytes_equal(&baseline, &authoritative));
    CHECK(authoritative.pack_input.resource_precisions[0].precision ==
          ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN);
    CHECK(authoritative.pack_input.resource_precisions[1].precision ==
          ORACLE_PACK_RESOURCE_PRECISION_HIGH);
    CHECK(authoritative.pack_input.resource_precisions[2].precision ==
          ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN);
    for (size_t resource = 0; resource < 3U; resource++) {
        CHECK(authoritative.pack_input.resource_precisions[resource]
                  .resource_key.size ==
              baseline.pack_input.resource_precisions[resource]
                  .resource_key.size);
        CHECK(memcmp(authoritative.pack_input.resource_precisions[resource]
                         .resource_key.data,
                     baseline.pack_input.resource_precisions[resource]
                         .resource_key.data,
                     baseline.pack_input.resource_precisions[resource]
                         .resource_key.size) == 0);
    }
    oracle_metadata_normalization_free(&authoritative);

    SerializedConstantBuffer cb_swap = parameters.constant_buffers[0];
    parameters.constant_buffers[0] = parameters.constant_buffers[1];
    parameters.constant_buffers[1] = cb_swap;
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0);
    cb_swap = parameters.constant_buffers[0];
    parameters.constant_buffers[0] = parameters.constant_buffers[1];
    parameters.constant_buffers[1] = cb_swap;

    SerializedVariable variable_swap =
        parameters.constant_buffers[0].variables[0];
    parameters.constant_buffers[0].variables[0] =
        parameters.constant_buffers[0].variables[1];
    parameters.constant_buffers[0].variables[1] = variable_swap;
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0);
    variable_swap = parameters.constant_buffers[0].variables[0];
    parameters.constant_buffers[0].variables[0] =
        parameters.constant_buffers[0].variables[1];
    parameters.constant_buffers[0].variables[1] = variable_swap;

    SerializedVariable member_swap =
        parameters.constant_buffers[0].struct_params[0].members[0];
    parameters.constant_buffers[0].struct_params[0].members[0] =
        parameters.constant_buffers[0].struct_params[0].members[1];
    parameters.constant_buffers[0].struct_params[0].members[1] = member_swap;
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0);
    member_swap =
        parameters.constant_buffers[0].struct_params[0].members[0];
    parameters.constant_buffers[0].struct_params[0].members[0] =
        parameters.constant_buffers[0].struct_params[0].members[1];
    parameters.constant_buffers[0].struct_params[0].members[1] = member_swap;

    SerializedResourceParam resource_swap = parameters.resources[0];
    parameters.resources[0] = parameters.resources[2];
    parameters.resources[2] = resource_swap;
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0);
    resource_swap = parameters.resources[0];
    parameters.resources[0] = parameters.resources[2];
    parameters.resources[2] = resource_swap;

#define REQUIRE_U32_FIELD(field) do { \
    uint32_t saved = (uint32_t)(field); \
    (field) = saved ^ UINT32_C(0x80000001); \
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0); \
    (field) = saved; \
} while (0)
#define REQUIRE_BOOL_FIELD(field) do { \
    bool saved = (field); \
    (field) = !saved; \
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0); \
    (field) = saved; \
} while (0)
#define REQUIRE_NAME_FIELD(field) do { \
    const char* saved = (field); \
    (field) = "X-different-name"; \
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0); \
    (field) = saved; \
} while (0)

    REQUIRE_U32_FIELD(parameters.version);
    REQUIRE_BOOL_FIELD(parameters.is_binary);
    REQUIRE_NAME_FIELD(parameters.constant_buffers[0].name);
    SerializedConstantBufferRole saved_role =
        parameters.constant_buffers[0].role;
    parameters.constant_buffers[0].role = SERIALIZED_CBUFFER_NAMED;
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0);
    parameters.constant_buffers[0].role = saved_role;
    REQUIRE_U32_FIELD(parameters.constant_buffers[0].size);
    REQUIRE_BOOL_FIELD(parameters.constant_buffers[0].has_is_partial);
    REQUIRE_BOOL_FIELD(parameters.constant_buffers[0].is_partial);
    REQUIRE_NAME_FIELD(parameters.constant_buffers[0].variables[0].name);
    for (size_t word = 0; word < 6U; word++) {
        REQUIRE_U32_FIELD(
            parameters.constant_buffers[0].variables[0].layout[word]);
    }
    REQUIRE_NAME_FIELD(parameters.constant_buffers[0].struct_params[0].name);
    for (size_t word = 0; word < 3U; word++) {
        REQUIRE_U32_FIELD(
            parameters.constant_buffers[0].struct_params[0].layout[word]);
    }
    REQUIRE_NAME_FIELD(
        parameters.constant_buffers[0].struct_params[0].members[0].name);
    for (size_t word = 0; word < 6U; word++) {
        REQUIRE_U32_FIELD(parameters.constant_buffers[0]
                              .struct_params[0]
                              .members[0]
                              .layout[word]);
    }
    REQUIRE_NAME_FIELD(parameters.resources[0].name);
    SerializedResourceType saved_type = parameters.resources[0].bind_type;
    parameters.resources[0].bind_type = SERIALIZED_RESOURCE_SAMPLER;
    CHECK(normalize_and_require_difference(&parameters, &baseline) == 0);
    parameters.resources[0].bind_type = saved_type;
    REQUIRE_U32_FIELD(parameters.resources[0].bind_index);
    REQUIRE_U32_FIELD(parameters.resources[0].array_size);
    REQUIRE_U32_FIELD(parameters.resources[0].dimension);
    REQUIRE_U32_FIELD(parameters.resources[0].sampler_index);
    REQUIRE_BOOL_FIELD(parameters.resources[0].multisampled);
    REQUIRE_U32_FIELD(parameters.resources[0].original_index);
    REQUIRE_U32_FIELD(parameters.resources[0].sampler_state);
    REQUIRE_U32_FIELD(parameters.resources[0].extra[0]);
    REQUIRE_U32_FIELD(parameters.resources[0].extra[1]);

#undef REQUIRE_NAME_FIELD
#undef REQUIRE_BOOL_FIELD
#undef REQUIRE_U32_FIELD

    oracle_metadata_normalization_free(&baseline);
    serialized_program_parameters_free(&parameters);
    return 0;
}

static int make_large_fixture(SerializedProgramParameters* parameters) {
    serialized_program_parameters_init(parameters);
    parameters->version = UNITY_2021_3_PLAYER_BLOB_VERSION;
    parameters->is_binary = true;
    parameters->cb_count = 17;
    parameters->constant_buffers = (SerializedConstantBuffer*)tracked_array(
        parameters->cb_count, sizeof(*parameters->constant_buffers));
    CHECK(parameters->constant_buffers != NULL);
    for (int buffer = 0; buffer < parameters->cb_count; buffer++) {
        char name[64];
        snprintf(name, sizeof(name), "CB%d", buffer);
        CHECK(set_name(parameters,
                       &parameters->constant_buffers[buffer].name, name));
        parameters->constant_buffers[buffer].size = (uint32_t)buffer * 16U;
    }
    SerializedConstantBuffer* first = &parameters->constant_buffers[0];
    first->var_count = 65;
    first->variables = (SerializedVariable*)tracked_array(
        first->var_count, sizeof(*first->variables));
    CHECK(first->variables != NULL);
    for (int variable = 0; variable < first->var_count; variable++) {
        char name[64];
        snprintf(name, sizeof(name), "Variable%d", variable);
        CHECK(set_variable(parameters, &first->variables[variable], name,
                           (uint32_t)variable * 10U));
    }
    first->struct_count = 17;
    first->struct_params = (SerializedStructParam*)tracked_array(
        first->struct_count, sizeof(*first->struct_params));
    CHECK(first->struct_params != NULL);
    for (int structure = 0; structure < first->struct_count; structure++) {
        char name[64];
        snprintf(name, sizeof(name), "Struct%d", structure);
        CHECK(set_name(parameters,
                       &first->struct_params[structure].name, name));
        first->struct_params[structure].layout[0] = (uint32_t)structure;
        first->struct_params[structure].layout[1] =
            (uint32_t)structure + 1U;
        first->struct_params[structure].layout[2] =
            (uint32_t)structure + 2U;
    }
    first->struct_params[0].member_count = 65;
    first->struct_params[0].members = (SerializedVariable*)tracked_array(
        first->struct_params[0].member_count,
        sizeof(*first->struct_params[0].members));
    CHECK(first->struct_params[0].members != NULL);
    for (int member = 0;
         member < first->struct_params[0].member_count; member++) {
        char name[64];
        snprintf(name, sizeof(name), "Member%d", member);
        CHECK(set_variable(parameters,
                           &first->struct_params[0].members[member], name,
                           (uint32_t)member * 10U));
    }
    parameters->res_count = 65;
    parameters->resources = (SerializedResourceParam*)tracked_array(
        parameters->res_count, sizeof(*parameters->resources));
    CHECK(parameters->resources != NULL);
    for (int resource = 0; resource < parameters->res_count; resource++) {
        char name[64];
        snprintf(name, sizeof(name), "Resource%d", resource);
        CHECK(set_resource(parameters, &parameters->resources[resource], name,
                           (SerializedResourceType)(resource % 5),
                           (uint32_t)resource * 10U));
    }
    return 0;
}

static int test_former_collection_limits(void) {
    size_t allocation_count = g_allocations_count;
    size_t allocated_bytes = g_allocated_bytes;
    SerializedProgramParameters parameters;
    CHECK(make_large_fixture(&parameters) == 0);
    OracleMetadataNormalization normalized;
    oracle_metadata_normalization_init(&normalized);
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &normalized) ==
          ORACLE_METADATA_OK);
    CHECK(read_u64_le(normalized.pack_input.canonical_bytes.data +
                      ORACLE_METADATA_HEADER_SIZE) == 17U);
    CHECK(normalized.pack_input.resource_precision_count == 65U);
    CHECK(normalized.pack_input.resource_precisions[64].precision ==
          ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN);
    CHECK(read_u64_le(normalized.pack_input.resource_precisions[64]
                          .resource_key.data + 24U) == 64U);

    char long_name[301];
    memset(long_name, 'L', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';
    const char* saved_resource_name = parameters.resources[64].name;
    parameters.resources[64].name = long_name;
    OracleMetadataNormalization long_name_normalized;
    oracle_metadata_normalization_init(&long_name_normalized);
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U,
                                    &long_name_normalized) ==
          ORACLE_METADATA_OK);
    CHECK(long_name_normalized.pack_input.resource_precisions[64]
              .resource_key.size ==
          ORACLE_METADATA_RESOURCE_KEY_HEADER_SIZE + strlen(long_name));
    oracle_metadata_normalization_free(&long_name_normalized);
    parameters.resources[64].name = saved_resource_name;

    SerializedStructParam struct_swap =
        parameters.constant_buffers[0].struct_params[0];
    parameters.constant_buffers[0].struct_params[0] =
        parameters.constant_buffers[0].struct_params[16];
    parameters.constant_buffers[0].struct_params[16] = struct_swap;
    CHECK(normalize_and_require_difference(&parameters, &normalized) == 0);
    struct_swap = parameters.constant_buffers[0].struct_params[0];
    parameters.constant_buffers[0].struct_params[0] =
        parameters.constant_buffers[0].struct_params[16];
    parameters.constant_buffers[0].struct_params[16] = struct_swap;

    oracle_metadata_normalization_free(&normalized);
    serialized_program_parameters_free(&parameters);
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int output_is_retained(
    const OracleMetadataNormalization* output,
    const uint8_t* canonical, const OraclePackResourcePrecisionInput* records,
    const uint8_t* keys) {
    CHECK(output->pack_input.canonical_bytes.data == canonical);
    CHECK(output->pack_input.resource_precisions == records);
    CHECK(output->resource_key_storage == keys);
    return 0;
}

static int test_malformed_shapes_and_strong_failure(void) {
    SerializedProgramParameters parameters;
    CHECK(make_fixture(&parameters) == 0);
    OracleMetadataNormalization output;
    oracle_metadata_normalization_init(&output);
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_OK);
    const uint8_t* canonical = output.pack_input.canonical_bytes.data;
    const OraclePackResourcePrecisionInput* records =
        output.pack_input.resource_precisions;
    const uint8_t* keys = output.resource_key_storage;

#define CHECK_RETAINED() \
    CHECK(output_is_retained(&output, canonical, records, keys) == 0)

    SerializedConstantBuffer* saved_buffers = parameters.constant_buffers;
    parameters.constant_buffers = NULL;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.constant_buffers = saved_buffers;

    SerializedVariable* saved_variables =
        parameters.constant_buffers[0].variables;
    parameters.constant_buffers[0].variables = NULL;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.constant_buffers[0].variables = saved_variables;

    SerializedStructParam* saved_structs =
        parameters.constant_buffers[0].struct_params;
    parameters.constant_buffers[0].struct_params = NULL;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.constant_buffers[0].struct_params = saved_structs;

    SerializedVariable* saved_members =
        parameters.constant_buffers[0].struct_params[0].members;
    parameters.constant_buffers[0].struct_params[0].members = NULL;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.constant_buffers[0].struct_params[0].members = saved_members;

    SerializedResourceParam* saved_resources = parameters.resources;
    parameters.resources = NULL;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.resources = saved_resources;

    SerializedVariable* unexpected_empty = (SerializedVariable*)(uintptr_t)1;
    parameters.constant_buffers[1].variables = unexpected_empty;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.constant_buffers[1].variables = NULL;

    int saved_member_count =
        parameters.constant_buffers[0].struct_params[0].member_count;
    parameters.constant_buffers[0].struct_params[0].member_count = -1;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.constant_buffers[0].struct_params[0].member_count =
        saved_member_count;

    int saved_resource_count = parameters.res_count;
    parameters.res_count = 0;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.res_count = saved_resource_count;

    SerializedResourceType saved_type = parameters.resources[0].bind_type;
    parameters.resources[0].bind_type = (SerializedResourceType)99;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_VALUE);
    CHECK_RETAINED();
    parameters.resources[0].bind_type = saved_type;

    const char* saved_name = parameters.resources[0].name;
    parameters.resources[0].name = NULL;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_VALUE);
    CHECK_RETAINED();
    parameters.resources[0].name = saved_name;

    SerializedConstantBufferRole saved_role =
        parameters.constant_buffers[0].role;
    parameters.constant_buffers[0].role =
        (SerializedConstantBufferRole)99;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_VALUE);
    CHECK_RETAINED();
    parameters.constant_buffers[0].role = saved_role;

    OracleMetadataPrecisionOverride duplicate[] = {
        {0U, ORACLE_PACK_RESOURCE_PRECISION_LOW},
        {0U, ORACLE_PACK_RESOURCE_PRECISION_HIGH},
    };
    CHECK(oracle_metadata_normalize(&parameters, duplicate, 2U, &output) ==
          ORACLE_METADATA_INVALID_VALUE);
    CHECK_RETAINED();
    OracleMetadataPrecisionOverride outside = {
        3U, ORACLE_PACK_RESOURCE_PRECISION_HIGH
    };
    CHECK(oracle_metadata_normalize(&parameters, &outside, 1U, &output) ==
          ORACLE_METADATA_INVALID_VALUE);
    CHECK_RETAINED();
    OracleMetadataPrecisionOverride invalid_precision = {
        0U, (OraclePackResourcePrecision)99
    };
    CHECK(oracle_metadata_normalize(&parameters, &invalid_precision, 1U,
                                    &output) ==
          ORACLE_METADATA_INVALID_VALUE);
    CHECK_RETAINED();
    CHECK(oracle_metadata_normalize(&parameters, NULL, 1U, &output) ==
          ORACLE_METADATA_INVALID_ARGUMENT);
    CHECK_RETAINED();

    int saved_count = parameters.cb_count;
    parameters.cb_count = -1;
    CHECK(oracle_metadata_normalize(&parameters, NULL, 0U, &output) ==
          ORACLE_METADATA_INVALID_SHAPE);
    CHECK_RETAINED();
    parameters.cb_count = saved_count;

#undef CHECK_RETAINED
    oracle_metadata_normalization_free(&output);
    serialized_program_parameters_free(&parameters);
    return 0;
}

static bool make_minimal_dxbc(uint8_t bytes[FIXTURE_DXBC_SIZE]) {
    memset(bytes, 0, FIXTURE_DXBC_SIZE);
    memcpy(bytes, "DXBC", 4U);
    write_u32_le(bytes + 20U, 1U);
    write_u32_le(bytes + 24U, FIXTURE_DXBC_SIZE);
    write_u32_le(bytes + 28U, 1U);
    write_u32_le(bytes + 32U, 36U);
    memcpy(bytes + 36U, "SHDR", 4U);
    write_u32_le(bytes + 40U, 8U);
    write_u32_le(bytes + 44U, UINT32_C(0x00010050));
    write_u32_le(bytes + 48U, 2U);
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, FIXTURE_DXBC_SIZE, hash)) return false;
    memcpy(bytes + 4U, hash, sizeof(hash));
    return true;
}

static VariantKey* make_minimal_key(void) {
    VariantKeyDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.shader_path_id = 1;
    descriptor.compiler_platform = 4;
    descriptor.parameter_blob_index = -1;
    descriptor.serialized_program_type = 15;
    descriptor.player_metadata_present = true;
    descriptor.player_program_type = 15;
    descriptor.compiler.source_directory = "";
    descriptor.compiler.source_basename = "Metadata.shader";
    descriptor.compiler.pass_name = "";
    VariantKey* key = NULL;
    if (variant_key_init(&key, &descriptor) != VARIANT_KEY_OK) return NULL;
    return key;
}

static int test_oracle_pack_interoperability(void) {
    SerializedProgramParameters parameters;
    CHECK(make_fixture(&parameters) == 0);
    OracleMetadataPrecisionOverride override = {
        2U, ORACLE_PACK_RESOURCE_PRECISION_MEDIUM
    };
    OracleMetadataNormalization normalized;
    oracle_metadata_normalization_init(&normalized);
    CHECK(oracle_metadata_normalize(&parameters, &override, 1U,
                                    &normalized) == ORACLE_METADATA_OK);
    uint8_t dxbc[FIXTURE_DXBC_SIZE];
    CHECK(make_minimal_dxbc(dxbc));
    VariantKey* key = make_minimal_key();
    CHECK(key != NULL);
    static const uint8_t transcript[] = {1U, 2U, 3U};
    uint8_t compiler[ORACLE_PACK_DIGEST_SIZE];
    uint8_t environment[ORACLE_PACK_DIGEST_SIZE];
    memset(compiler, 0x5a, sizeof(compiler));
    memset(environment, 0xa5, sizeof(environment));
    OraclePackAuthorityInput authority = {compiler, environment};
    OraclePackEntryInput entry;
    memset(&entry, 0, sizeof(entry));
    entry.variant_key = key;
    entry.stripped_dxbc = (OraclePackBytes){dxbc, sizeof(dxbc)};
    entry.compile_request_transcript =
        (OraclePackBytes){transcript, sizeof(transcript)};
    entry.normalized_metadata = normalized.pack_input;
    entry.authority = authority;
    OraclePackWriter* writer = NULL;
    CHECK(oracle_pack_writer_create(&authority, &writer) == ORACLE_PACK_OK);
    CHECK(oracle_pack_writer_add(writer, &entry) == ORACLE_PACK_OK);
    uint8_t* pack_bytes = NULL;
    size_t pack_size = 0;
    CHECK(oracle_pack_writer_finalize(writer, &pack_bytes, &pack_size) ==
          ORACLE_PACK_OK);
    OraclePack* pack = NULL;
    CHECK(oracle_pack_open_memory(pack_bytes, pack_size, &pack) ==
          ORACLE_PACK_OK);
    OraclePackEntryView view;
    CHECK(oracle_pack_lookup_variant_key(pack, key, &view) == ORACLE_PACK_OK);
    OraclePackNormalizedMetadataView metadata;
    CHECK(oracle_pack_metadata_view(view.normalized_metadata_bytes,
                                    &metadata) == ORACLE_PACK_OK);
    CHECK(metadata.canonical_bytes.size ==
          normalized.pack_input.canonical_bytes.size);
    CHECK(memcmp(metadata.canonical_bytes.data,
                 normalized.pack_input.canonical_bytes.data,
                 metadata.canonical_bytes.size) == 0);
    CHECK(metadata.resource_precision_count == 3U);
    bool saw_unknown = false;
    bool saw_medium = false;
    for (size_t index = 0; index < metadata.resource_precision_count;
         index++) {
        OraclePackResourcePrecisionView precision;
        CHECK(oracle_pack_metadata_precision_at(view.normalized_metadata_bytes,
                                                index, &precision) ==
              ORACLE_PACK_OK);
        CHECK(precision.resource_key.size > 0U);
        saw_unknown |= precision.precision ==
                       ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN;
        saw_medium |= precision.precision ==
                      ORACLE_PACK_RESOURCE_PRECISION_MEDIUM;
    }
    CHECK(saw_unknown && saw_medium);
    oracle_pack_free(pack);
    oracle_pack_bytes_free(pack_bytes);
    oracle_pack_writer_free(writer);
    variant_key_free(key);
    oracle_metadata_normalization_free(&normalized);
    serialized_program_parameters_free(&parameters);
    return 0;
}

int main(void) {
    CHECK(test_canonical_bytes_fields_order_and_precision() == 0);
    CHECK(test_former_collection_limits() == 0);
    CHECK(test_malformed_shapes_and_strong_failure() == 0);
    CHECK(test_oracle_pack_interoperability() == 0);
    CHECK(strcmp(oracle_metadata_status_string(ORACLE_METADATA_OK), "ok") ==
          0);
    CHECK(strcmp(oracle_metadata_status_string((OracleMetadataStatus)99),
                 "unknown_status") == 0);
    CHECK(g_allocations_count == 0U);
    CHECK(g_allocated_bytes == 0U);
    return 0;
}
