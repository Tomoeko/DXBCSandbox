// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compute_verifier.h"
#include "dxbc/dxbc_hash.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #condition);           \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    uint8_t code[72];
    uint32_t groups[3];
    ComputeShaderResource textures[2];
    ComputeShaderResource input;
    ComputeShaderResource output;
    ComputeShaderBuiltinSampler samplers[2];
    ComputeShaderKernelVariant variant;
    UnityComputeKernelExpectation expected;
} ExpectationFixture;

typedef struct {
    uint8_t bytes[2048];
    size_t size;
} NativeFixture;

static ComputeShaderStringView text(const char* value) {
    return (ComputeShaderStringView){(const uint8_t*)value, strlen(value)};
}

static void store_u32(uint8_t* bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = (uint8_t)(value >> (8 * index));
}

static bool expectation_init(ExpectationFixture* fixture) {
    memset(fixture, 0, sizeof(*fixture));
    memcpy(fixture->code, "DXBC", 4);
    store_u32(fixture->code + 20, 1);
    store_u32(fixture->code + 24, sizeof(fixture->code));
    store_u32(fixture->code + 28, 1);
    store_u32(fixture->code + 32, 36);
    memcpy(fixture->code + 36, "SHEX", 4);
    store_u32(fixture->code + 40, 28);
    store_u32(fixture->code + 44, 0x00050050);
    store_u32(fixture->code + 48, 7);
    store_u32(fixture->code + 52, 155 | (4U << 24));
    fixture->groups[0] = 8;
    fixture->groups[1] = 4;
    fixture->groups[2] = 1;
    for (size_t axis = 0; axis < 3; ++axis)
        store_u32(fixture->code + 56 + axis * 4, fixture->groups[axis]);
    store_u32(fixture->code + 68, 62 | (1U << 24));
    CHECK(dxbc_compute_hash(fixture->code, sizeof(fixture->code), fixture->code + 4));
    fixture->textures[0] = (ComputeShaderResource){text("First"), text("GeneratedFirst"), 2, 4, 2};
    fixture->textures[1] = (ComputeShaderResource){text("Second"), text(""), 3, -1, 3};
    fixture->input = (ComputeShaderResource){text("Input"), text(""), 1, -1, -1};
    fixture->output = (ComputeShaderResource){text("Output"), text("OutputGenerated"), 0, -1, 2};
    fixture->samplers[0] = (ComputeShaderBuiltinSampler){85, 4};
    fixture->samplers[1] = (ComputeShaderBuiltinSampler){273, 5};
    fixture->variant = (ComputeShaderKernelVariant){.textures = fixture->textures,
                                                    .texture_count = 2,
                                                    .input_buffers = &fixture->input,
                                                    .input_buffer_count = 1,
                                                    .output_buffers = &fixture->output,
                                                    .output_buffer_count = 1,
                                                    .builtin_samplers = fixture->samplers,
                                                    .builtin_sampler_count = 2,
                                                    .code = fixture->code,
                                                    .code_size = sizeof(fixture->code),
                                                    .thread_group_size = fixture->groups,
                                                    .thread_group_size_count = 3,
                                                    .keyword_key = text("ARBITRARY_CLASS72_KEY"),
                                                    .requirements = 0x4001};
    fixture->expected = (UnityComputeKernelExpectation){.kernel_name = text("Kernel"),
                                                        .variant = &fixture->variant,
                                                        .target_level = 0,
                                                        .resources_resolved = true};
    return true;
}

static void append_u32(NativeFixture* native, uint32_t value) {
    store_u32(native->bytes + native->size, value);
    native->size += 4;
}

static void append_u64(NativeFixture* native, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        native->bytes[native->size++] = (uint8_t)(value >> (8 * index));
}

static void append_string(NativeFixture* native, ComputeShaderStringView value) {
    append_u32(native, (uint32_t)value.size);
    if (value.size)
        memcpy(native->bytes + native->size, value.bytes, value.size);
    native->size += value.size;
}

static void append_resources(NativeFixture* native, const ComputeShaderResource* resources,
                             size_t count) {
    append_u64(native, count);
    for (size_t index = 0; index < count; ++index) {
        append_string(native, resources[index].name);
        append_string(native, resources[index].generated_name);
        append_u32(native, (uint32_t)resources[index].bind_point);
        append_u32(native, (uint32_t)resources[index].sampler_bind_point);
        append_u32(native, (uint32_t)resources[index].texture_dimension);
    }
}

/* Controlled expected and observed models test the comparator only. They do
 * not supply immutable Class72 provenance or a native compiler certificate. */
static NativeFixture native_fixture(const ExpectationFixture* fixture, size_t kernels,
                                    size_t buffer_variants, bool nonempty_buffer) {
    NativeFixture native = {{0}, 0};
    append_u64(&native, 0); /* directives */
    append_u32(&native, 0); /* target level */
    append_u64(&native, buffer_variants);
    for (size_t index = 0; index < buffer_variants; ++index) {
        append_u64(&native, nonempty_buffer ? 1 : 0);
        if (nonempty_buffer) {
            append_string(&native, text("Parameters"));
            append_u32(&native, 16);
            append_u64(&native, 0);
        }
    }
    append_u64(&native, kernels);
    for (size_t index = 0; index < kernels; ++index) {
        append_string(&native, text("Kernel"));
        append_resources(&native, NULL, 0);
        append_resources(&native, fixture->textures, 2);
        append_resources(&native, &fixture->input, 1);
        append_resources(&native, &fixture->output, 1);
        append_u64(&native, 2);
        for (size_t sampler = 0; sampler < 2; ++sampler) {
            append_u32(&native, fixture->samplers[sampler].sampler);
            append_u32(&native, (uint32_t)fixture->samplers[sampler].bind_point);
        }
        append_u64(&native, sizeof(fixture->code));
        memcpy(native.bytes + native.size, fixture->code, sizeof(fixture->code));
        native.size += sizeof(fixture->code);
        for (size_t axis = 0; axis < 3; ++axis)
            append_u32(&native, fixture->groups[axis]);
    }
    native.bytes[native.size++] = 1; /* resolved */
    return native;
}

static UnityComputeVerifyStatus compare(const ExpectationFixture* expected,
                                        const NativeFixture* native,
                                        UnityComputeVerifyReport* report) {
    return unity_compute_verify_kernel(&expected->expected, native->bytes, native->size, report);
}

static bool verify_exact_and_scope(void) {
    ExpectationFixture expected;
    CHECK(expectation_init(&expected));
    NativeFixture native = native_fixture(&expected, 1, 1, false);
    UnityComputeVerifyReport report;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_OK);
    CHECK(report.dxbc_compared && report.dxbc_equal && report.expected_stage_valid &&
          report.actual_stage_valid);
    CHECK(report.expected_group_matches_code && report.actual_group_matches_code &&
          report.common_metadata_compared && report.common_metadata_equal);
    CHECK(report.native_decode_status == COMPUTE_SHADER_OBJECT_OK &&
          report.resource_index == SIZE_MAX);
    CHECK(report.native_kernel_count == 1 && report.native_buffer_variant_count == 1);
    expected.variant.keyword_key = text("NOT_PRESENT_IN_NATIVE_WIRE");
    expected.variant.requirements = INT64_MAX;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_OK);

    uint32_t index = 0;
    expected.variant.constant_buffer_variant_indices = &index;
    expected.variant.constant_buffer_variant_index_count = 1;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION);
    CHECK(report.dxbc_equal && !report.common_metadata_compared && !report.common_metadata_equal);
    expected.variant.constant_buffer_variant_index_count = 0;
    ComputeShaderConstantBuffer buffer = {.name = text("Parameters"), .byte_size = 16};
    expected.expected.selected_buffer_definitions = &buffer;
    expected.expected.selected_buffer_definition_count = 1;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION);
    expected.expected.selected_buffer_definition_count = 0;
    native = native_fixture(&expected, 1, 1, true);
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION);
    native = native_fixture(&expected, 1, 0, false);
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION);
    native = native_fixture(&expected, 1, 2, false);
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION);
    native = native_fixture(&expected, 0, 1, false);
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_NATIVE_KERNEL_COUNT);
    native = native_fixture(&expected, 2, 1, false);
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_NATIVE_KERNEL_COUNT);
    return true;
}

static bool verify_metadata_fields(void) {
    ExpectationFixture expected;
    CHECK(expectation_init(&expected));
    const NativeFixture native = native_fixture(&expected, 1, 1, false);
    UnityComputeVerifyReport report;
    expected.expected.kernel_name = text("AnotherKernel");
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_KERNEL_NAME_MISMATCH);
    CHECK(!report.dxbc_compared);
    expected.expected.kernel_name = text("Kernel");
    expected.expected.target_level = 1;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_TARGET_LEVEL_MISMATCH);
    CHECK(report.dxbc_equal && report.common_metadata_compared && !report.common_metadata_equal);
    expected.expected.target_level = 0;
    expected.expected.resources_resolved = false;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_RESOLUTION_MISMATCH);
    expected.expected.resources_resolved = true;

    ComputeShaderResource* records[] = {expected.textures, &expected.input, &expected.output};
    const UnityComputeVerifyResourceRole roles[] = {UNITY_COMPUTE_VERIFY_RESOURCE_TEXTURE,
                                                    UNITY_COMPUTE_VERIFY_RESOURCE_INPUT_BUFFER,
                                                    UNITY_COMPUTE_VERIFY_RESOURCE_OUTPUT_BUFFER};
    for (size_t role = 0; role < 3; ++role) {
        const ComputeShaderResource original = *records[role];
        for (size_t field = 0; field < 5; ++field) {
            *records[role] = original;
            switch (field) {
            case 0:
                records[role]->name = text("Changed");
                break;
            case 1:
                records[role]->generated_name = text("ChangedGenerated");
                break;
            case 2:
                ++records[role]->bind_point;
                break;
            case 3:
                ++records[role]->sampler_bind_point;
                break;
            case 4:
                ++records[role]->texture_dimension;
                break;
            }
            CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_RESOURCE_MISMATCH);
            CHECK(report.dxbc_equal && report.resource_role == roles[role] &&
                  report.resource_index == 0);
        }
        *records[role] = original;
    }
    const ComputeShaderResource first = expected.textures[0];
    expected.textures[0] = expected.textures[1];
    expected.textures[1] = first;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_RESOURCE_MISMATCH);
    expected.textures[1] = expected.textures[0];
    expected.textures[0] = first;
    expected.variant.texture_count = 1;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_RESOURCE_MISMATCH);
    CHECK(report.resource_index == SIZE_MAX);
    expected.variant.texture_count = 2;
    ++expected.samplers[1].sampler;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_SAMPLER_MISMATCH);
    CHECK(report.resource_role == UNITY_COMPUTE_VERIFY_RESOURCE_BUILTIN_SAMPLER &&
          report.resource_index == 1);
    --expected.samplers[1].sampler;
    ++expected.samplers[0].bind_point;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_SAMPLER_MISMATCH);
    --expected.samplers[0].bind_point;
    expected.variant.builtin_sampler_count = 1;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_SAMPLER_MISMATCH);
    CHECK(report.resource_index == SIZE_MAX);
    expected.variant.builtin_sampler_count = 2;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_OK);
    return true;
}

static bool verify_code_and_groups(void) {
    ExpectationFixture expected;
    CHECK(expectation_init(&expected));
    NativeFixture native = native_fixture(&expected, 1, 1, false);
    UnityComputeVerifyReport report;
    ++expected.groups[0];
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_THREAD_GROUP_MISMATCH);
    CHECK(report.dxbc_equal && !report.expected_group_matches_code &&
          report.actual_group_matches_code);
    --expected.groups[0];
    ++native.bytes[native.size - 13];
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_THREAD_GROUP_MISMATCH);
    CHECK(report.dxbc_equal && report.expected_group_matches_code &&
          !report.actual_group_matches_code);
    --native.bytes[native.size - 13];
    const size_t native_code_offset = native.size - 13 - sizeof(expected.code);
    native.bytes[native_code_offset + 4] ^= 1;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_DXBC_MISMATCH);
    CHECK(report.dxbc.status == DXBC_COMPARE_ACTUAL_INVALID);
    native.bytes[native_code_offset + 4] ^= 1;
    store_u32(expected.code + 68, 58 | (1U << 24));
    CHECK(dxbc_compute_hash(expected.code, sizeof(expected.code), expected.code + 4));
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_DXBC_MISMATCH);
    CHECK(report.dxbc_compared && !report.dxbc_equal &&
          report.dxbc.status == DXBC_COMPARE_INSTRUCTION_OPCODE);
    expected.code[4] ^= 1;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_DXBC_MISMATCH);
    CHECK(report.dxbc.status == DXBC_COMPARE_EXPECTED_INVALID);

    CHECK(expectation_init(&expected));
    /* Equal bytes still fail if the purported compute payload is a pixel
     * executable or lacks the required thread-group declaration. */
    store_u32(expected.code + 44, 0x00000050);
    CHECK(dxbc_compute_hash(expected.code, sizeof(expected.code), expected.code + 4));
    native = native_fixture(&expected, 1, 1, false);
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_STAGE_INVALID);
    CHECK(report.dxbc_equal && !report.expected_stage_valid && !report.actual_stage_valid);
    CHECK(expectation_init(&expected));
    store_u32(expected.code + 52, 53);
    store_u32(expected.code + 56, 4);
    CHECK(dxbc_compute_hash(expected.code, sizeof(expected.code), expected.code + 4));
    native = native_fixture(&expected, 1, 1, false);
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_STAGE_INVALID);
    return true;
}

static bool verify_malformed_inputs_and_reset(void) {
    ExpectationFixture expected;
    CHECK(expectation_init(&expected));
    const NativeFixture native = native_fixture(&expected, 1, 1, false);
    UnityComputeVerifyReport report;
    CHECK(unity_compute_verify_kernel(NULL, native.bytes, native.size, &report) ==
          UNITY_COMPUTE_VERIFY_INVALID_ARGUMENT);
    CHECK(!report.dxbc_compared && !report.common_metadata_equal &&
          report.resource_index == SIZE_MAX);
    CHECK(unity_compute_verify_kernel(&expected.expected, native.bytes, native.size, NULL) ==
          UNITY_COMPUTE_VERIFY_INVALID_ARGUMENT);
    expected.variant.textures = NULL;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_EXPECTATION_INVALID);
    expected.variant.textures = expected.textures;
    expected.variant.texture_count = 1048577;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_EXPECTATION_INVALID);
    expected.variant.texture_count = 2;
    expected.textures[0].name = (ComputeShaderStringView){(const uint8_t*)"Has\0Nul", 7};
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_EXPECTATION_INVALID);
    expected.textures[0].name = text("First");
    expected.variant.thread_group_size_count = 2;
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_EXPECTATION_INVALID);
    expected.variant.thread_group_size_count = 3;
    for (size_t size = 1; size < native.size; ++size) {
        CHECK(unity_compute_verify_kernel(&expected.expected, native.bytes, size, &report) ==
              UNITY_COMPUTE_VERIFY_NATIVE_PAYLOAD_INVALID);
        CHECK(!report.dxbc_compared && !report.common_metadata_compared);
    }
    CHECK(compare(&expected, &native, &report) == UNITY_COMPUTE_VERIFY_OK);
    CHECK(strcmp(unity_compute_verify_status_name(UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION),
                 "unsupported-selection") == 0);
    CHECK(strcmp(unity_compute_verify_status_name((UnityComputeVerifyStatus)99), "unknown") == 0);
    return true;
}

int main(void) {
    if (!verify_exact_and_scope() || !verify_metadata_fields() || !verify_code_and_groups() ||
        !verify_malformed_inputs_and_reset())
        return 1;
    puts("native compute independent verifier unit tests passed");
    return 0;
}
