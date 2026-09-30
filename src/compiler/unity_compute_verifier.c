// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_compute_verifier.h"

#include <string.h>

enum { COMPUTE_VERIFY_MAX_ITEMS = 1048576, COMPUTE_VERIFY_MAX_TEXT = 1048576 };

void unity_compute_verify_report_init(UnityComputeVerifyReport* report) {
    if (!report)
        return;
    memset(report, 0, sizeof(*report));
    report->resource_index = SIZE_MAX;
    report->native_decode_status = COMPUTE_SHADER_OBJECT_NOT_DECODED;
    dxbc_compare_result_init(&report->dxbc);
    report->expected_stage.chunk_index = DXBC_DOCUMENT_NO_INDEX;
    report->expected_stage.instruction_index = DXBC_DOCUMENT_NO_INDEX;
    report->actual_stage.chunk_index = DXBC_DOCUMENT_NO_INDEX;
    report->actual_stage.instruction_index = DXBC_DOCUMENT_NO_INDEX;
}

static bool string_valid(ComputeShaderStringView string) {
    return string.size <= COMPUTE_VERIFY_MAX_TEXT &&
           (!string.size || (string.bytes && !memchr(string.bytes, 0, string.size)));
}

static bool string_equal(ComputeShaderStringView expected, ComputeShaderStringView actual) {
    return expected.size == actual.size &&
           (!expected.size || memcmp(expected.bytes, actual.bytes, expected.size) == 0);
}

static bool resource_array_valid(const ComputeShaderResource* resources, size_t count) {
    if (count > COMPUTE_VERIFY_MAX_ITEMS || (count && !resources))
        return false;
    for (size_t index = 0; index < count; ++index)
        if (!string_valid(resources[index].name) || !string_valid(resources[index].generated_name))
            return false;
    return true;
}

static bool expectation_valid(const UnityComputeKernelExpectation* expected) {
    if (!expected || !expected->variant || !expected->kernel_name.size ||
        !string_valid(expected->kernel_name) ||
        expected->selected_buffer_definition_count > COMPUTE_VERIFY_MAX_ITEMS ||
        (expected->selected_buffer_definition_count && !expected->selected_buffer_definitions))
        return false;
    const ComputeShaderKernelVariant* variant = expected->variant;
    return variant->code && variant->code_size && variant->code_size <= COMPUTE_VERIFY_MAX_ITEMS &&
           variant->thread_group_size_count == 3 && variant->thread_group_size &&
           variant->constant_buffer_variant_index_count <= COMPUTE_VERIFY_MAX_ITEMS &&
           (!variant->constant_buffer_variant_index_count ||
            variant->constant_buffer_variant_indices) &&
           resource_array_valid(variant->constant_buffers, variant->constant_buffer_count) &&
           resource_array_valid(variant->textures, variant->texture_count) &&
           resource_array_valid(variant->input_buffers, variant->input_buffer_count) &&
           resource_array_valid(variant->output_buffers, variant->output_buffer_count) &&
           variant->builtin_sampler_count <= COMPUTE_VERIFY_MAX_ITEMS &&
           (!variant->builtin_sampler_count || variant->builtin_samplers);
}

static bool resources_equal(const ComputeShaderResource* expected, size_t expected_count,
                            const ComputeShaderResource* actual, size_t actual_count,
                            UnityComputeVerifyResourceRole role, UnityComputeVerifyReport* report) {
    if (expected_count != actual_count) {
        report->resource_role = role;
        return false;
    }
    for (size_t index = 0; index < expected_count; ++index) {
        const ComputeShaderResource* left = &expected[index];
        const ComputeShaderResource* right = &actual[index];
        if (!string_equal(left->name, right->name) ||
            !string_equal(left->generated_name, right->generated_name) ||
            left->bind_point != right->bind_point ||
            left->sampler_bind_point != right->sampler_bind_point ||
            left->texture_dimension != right->texture_dimension) {
            report->resource_role = role;
            report->resource_index = index;
            return false;
        }
    }
    return true;
}

static bool samplers_equal(const ComputeShaderKernelVariant* expected,
                           const ComputeShaderKernelVariant* actual,
                           UnityComputeVerifyReport* report) {
    if (expected->builtin_sampler_count != actual->builtin_sampler_count) {
        report->resource_role = UNITY_COMPUTE_VERIFY_RESOURCE_BUILTIN_SAMPLER;
        return false;
    }
    for (size_t index = 0; index < expected->builtin_sampler_count; ++index)
        if (expected->builtin_samplers[index].sampler != actual->builtin_samplers[index].sampler ||
            expected->builtin_samplers[index].bind_point !=
                actual->builtin_samplers[index].bind_point) {
            report->resource_role = UNITY_COMPUTE_VERIFY_RESOURCE_BUILTIN_SAMPLER;
            report->resource_index = index;
            return false;
        }
    return true;
}

static bool validate_stage(const ComputeShaderKernelVariant* variant,
                           DXBCStageContractDiagnostic* diagnostic, bool* group_matches_code) {
    DXBCDocument document;
    DXBCStageContract contract;
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    bool valid = dxbc_document_parse(&document, variant->code, variant->code_size, NULL) &&
                 dxbc_stage_contract_decode_document(&document, &contract, diagnostic);
    if (valid && contract.program_type != DXBC_PROGRAM_TYPE_COMPUTE) {
        diagnostic->status = DXBC_STAGE_CONTRACT_DECLARATION_STAGE_MISMATCH;
        diagnostic->chunk_index = contract.executable_chunk_index;
        diagnostic->expected = DXBC_PROGRAM_TYPE_COMPUTE;
        diagnostic->actual = contract.program_type;
        valid = false;
    }
    if (valid && !contract.has_thread_group_size) {
        diagnostic->status = DXBC_STAGE_CONTRACT_MISSING_DECLARATION;
        diagnostic->chunk_index = contract.executable_chunk_index;
        diagnostic->expected = 1;
        diagnostic->actual = 0;
        valid = false;
    }
    if (valid)
        *group_matches_code = memcmp(variant->thread_group_size, contract.thread_group_size,
                                     sizeof(contract.thread_group_size)) == 0;
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return valid;
}

static UnityComputeVerifyStatus compare_kernel(const UnityComputeKernelExpectation* expected,
                                               const UnityComputeBinary* native,
                                               UnityComputeVerifyReport* report) {
    report->native_kernel_count = native->kernel_count;
    report->native_buffer_variant_count = native->buffer_variant_count;
    if (native->kernel_count != 1)
        return UNITY_COMPUTE_VERIFY_NATIVE_KERNEL_COUNT;
    const UnityComputeKernelBinary* kernel = &native->kernels[0];
    const ComputeShaderKernelVariant* left = expected->variant;
    const ComputeShaderKernelVariant* right = &kernel->data;
    if (!string_equal(expected->kernel_name, kernel->name))
        return UNITY_COMPUTE_VERIFY_KERNEL_NAME_MISMATCH;

    report->dxbc_compared = true;
    report->dxbc_equal = dxbc_compare_exact(left->code, left->code_size, right->code,
                                            right->code_size, &report->dxbc) == DXBC_COMPARE_EQUAL;
    if (!report->dxbc_equal)
        return UNITY_COMPUTE_VERIFY_DXBC_MISMATCH;
    report->expected_stage_valid =
        validate_stage(left, &report->expected_stage, &report->expected_group_matches_code);
    report->actual_stage_valid =
        validate_stage(right, &report->actual_stage, &report->actual_group_matches_code);
    if (!report->expected_stage_valid || !report->actual_stage_valid)
        return UNITY_COMPUTE_VERIFY_STAGE_INVALID;
    if (!report->expected_group_matches_code || !report->actual_group_matches_code ||
        memcmp(left->thread_group_size, right->thread_group_size, 3 * sizeof(uint32_t)) != 0)
        return UNITY_COMPUTE_VERIFY_THREAD_GROUP_MISMATCH;

    /* The single-kernel native compile has one reflected buffer variant. A
     * player selection index is a different serialized field; do not treat it
     * as an index into this native response without proving that relationship. */
    if (left->constant_buffer_variant_index_count || expected->selected_buffer_definition_count ||
        left->constant_buffer_count || native->buffer_variant_count != 1 ||
        native->buffer_variants[0].buffer_count || right->constant_buffer_count ||
        native->directive_count)
        return UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION;

    report->common_metadata_compared = true;
    if (expected->target_level != native->target_level)
        return UNITY_COMPUTE_VERIFY_TARGET_LEVEL_MISMATCH;
    if (expected->resources_resolved != native->resources_resolved)
        return UNITY_COMPUTE_VERIFY_RESOLUTION_MISMATCH;
    if (!resources_equal(left->constant_buffers, left->constant_buffer_count,
                         right->constant_buffers, right->constant_buffer_count,
                         UNITY_COMPUTE_VERIFY_RESOURCE_CONSTANT_BUFFER, report) ||
        !resources_equal(left->textures, left->texture_count, right->textures, right->texture_count,
                         UNITY_COMPUTE_VERIFY_RESOURCE_TEXTURE, report) ||
        !resources_equal(left->input_buffers, left->input_buffer_count, right->input_buffers,
                         right->input_buffer_count, UNITY_COMPUTE_VERIFY_RESOURCE_INPUT_BUFFER,
                         report) ||
        !resources_equal(left->output_buffers, left->output_buffer_count, right->output_buffers,
                         right->output_buffer_count, UNITY_COMPUTE_VERIFY_RESOURCE_OUTPUT_BUFFER,
                         report))
        return UNITY_COMPUTE_VERIFY_RESOURCE_MISMATCH;
    if (!samplers_equal(left, right, report))
        return UNITY_COMPUTE_VERIFY_SAMPLER_MISMATCH;
    report->common_metadata_equal = true;
    return UNITY_COMPUTE_VERIFY_OK;
}

UnityComputeVerifyStatus unity_compute_verify_kernel(const UnityComputeKernelExpectation* expected,
                                                     const uint8_t* native_payload,
                                                     size_t native_payload_size,
                                                     UnityComputeVerifyReport* report) {
    if (!report)
        return UNITY_COMPUTE_VERIFY_INVALID_ARGUMENT;
    unity_compute_verify_report_init(report);
    if (!expected || !native_payload || !native_payload_size) {
        report->status = UNITY_COMPUTE_VERIFY_INVALID_ARGUMENT;
        return report->status;
    }
    if (!expectation_valid(expected)) {
        report->status = UNITY_COMPUTE_VERIFY_EXPECTATION_INVALID;
        return report->status;
    }
    UnityComputeBinary native;
    unity_compute_binary_init(&native);
    report->native_decode_status =
        unity_compute_binary_decode(&native, native_payload, native_payload_size);
    report->status = report->native_decode_status == COMPUTE_SHADER_OBJECT_OK
                         ? compare_kernel(expected, &native, report)
                         : UNITY_COMPUTE_VERIFY_NATIVE_PAYLOAD_INVALID;
    unity_compute_binary_dispose(&native);
    return report->status;
}

const char* unity_compute_verify_status_name(UnityComputeVerifyStatus status) {
    switch (status) {
    case UNITY_COMPUTE_VERIFY_OK:
        return "ok";
    case UNITY_COMPUTE_VERIFY_INVALID_ARGUMENT:
        return "invalid-argument";
    case UNITY_COMPUTE_VERIFY_EXPECTATION_INVALID:
        return "expectation-invalid";
    case UNITY_COMPUTE_VERIFY_NATIVE_PAYLOAD_INVALID:
        return "native-payload-invalid";
    case UNITY_COMPUTE_VERIFY_UNSUPPORTED_SELECTION:
        return "unsupported-selection";
    case UNITY_COMPUTE_VERIFY_NATIVE_KERNEL_COUNT:
        return "native-kernel-count";
    case UNITY_COMPUTE_VERIFY_KERNEL_NAME_MISMATCH:
        return "kernel-name-mismatch";
    case UNITY_COMPUTE_VERIFY_DXBC_MISMATCH:
        return "dxbc-mismatch";
    case UNITY_COMPUTE_VERIFY_STAGE_INVALID:
        return "stage-invalid";
    case UNITY_COMPUTE_VERIFY_THREAD_GROUP_MISMATCH:
        return "thread-group-mismatch";
    case UNITY_COMPUTE_VERIFY_TARGET_LEVEL_MISMATCH:
        return "target-level-mismatch";
    case UNITY_COMPUTE_VERIFY_RESOLUTION_MISMATCH:
        return "resolution-mismatch";
    case UNITY_COMPUTE_VERIFY_RESOURCE_MISMATCH:
        return "resource-mismatch";
    case UNITY_COMPUTE_VERIFY_SAMPLER_MISMATCH:
        return "sampler-mismatch";
    }
    return "unknown";
}
