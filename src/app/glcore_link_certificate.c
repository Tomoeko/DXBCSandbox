// SPDX-License-Identifier: GPL-3.0-only

#include "app/glcore_link_certificate.h"

#include <string.h>

void glcore_link_certificate_report_init(
    GLCoreLinkCertificateReport* report) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->status = GLCORE_LINK_CERTIFICATE_INVALID_ARGUMENT;
    report->first_differing_byte = SIZE_MAX;
}

static bool owner_equal(const SerializedGLCoreLinkOwner* left,
                        const SerializedGLCoreLinkOwner* right) {
    return left && right &&
           left->shader_path_id == right->shader_path_id &&
           left->subshader_index == right->subshader_index &&
           left->pass_index == right->pass_index &&
           left->serialized_stage == right->serialized_stage &&
           left->flattened_subprogram_index ==
               right->flattened_subprogram_index &&
           left->hardware_tier_group == right->hardware_tier_group &&
           left->inner_subprogram_index == right->inner_subprogram_index &&
           left->archive_entry_index == right->archive_entry_index;
}

static bool target_is_valid(const SerializedGLCoreTarget* target) {
    return serialized_glcore_target_validate(target) ==
           SERIALIZED_GLCORE_TARGET_OK;
}

static bool vector_shape_is_valid(const GLCoreGeneratedLinkVector* vector,
                                  const GLCoreLinkOutput** combined) {
    if (!vector || !combined || !vector->outputs) return false;
    *combined = NULL;
    if (vector->shape == GLCORE_LINK_VECTOR_COMBINED_ONLY) {
        if (vector->output_count != 1U ||
            vector->outputs[0].stage != UNITY_SERIALIZED_STAGE_VERTEX ||
            !vector->outputs[0].bytes || vector->outputs[0].size == 0U) {
            return false;
        }
        *combined = &vector->outputs[0];
        return true;
    }
    if (vector->shape ==
        GLCORE_LINK_VECTOR_COMBINED_WITH_EMPTY_FRAGMENT) {
        if (vector->output_count != 2U ||
            vector->outputs[0].stage != UNITY_SERIALIZED_STAGE_VERTEX ||
            !vector->outputs[0].bytes || vector->outputs[0].size == 0U ||
            vector->outputs[1].stage != UNITY_SERIALIZED_STAGE_FRAGMENT ||
            vector->outputs[1].size != 0U) {
            return false;
        }
        *combined = &vector->outputs[0];
        return true;
    }
    return false;
}

GLCoreLinkCertificateStatus glcore_link_certificate_compare_vector(
    const SerializedGLCoreTarget* target,
    const GLCoreGeneratedLinkVector* generated,
    GLCoreLinkCertificateReport* report) {
    if (report) glcore_link_certificate_report_init(report);
    if (!target || !generated || !report || !generated->unity_version) {
        return GLCORE_LINK_CERTIFICATE_INVALID_ARGUMENT;
    }
    if (!target_is_valid(target)) {
        report->status = GLCORE_LINK_CERTIFICATE_TARGET_INVALID;
        return report->status;
    }
    report->serialized_target_available = true;
    if (strcmp(target->unity_version, generated->unity_version) != 0) {
        report->status = GLCORE_LINK_CERTIFICATE_UNITY_VERSION_MISMATCH;
        return report->status;
    }
    if (generated->compiler_platform != SERIALIZED_GLCORE_PLATFORM) {
        report->status = GLCORE_LINK_CERTIFICATE_PLATFORM_MISMATCH;
        return report->status;
    }
    if (generated->program_type !=
        SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE) {
        report->status = GLCORE_LINK_CERTIFICATE_PROGRAM_TYPE_MISMATCH;
        return report->status;
    }
    if (!owner_equal(&target->owner, &generated->owner)) {
        report->status = GLCORE_LINK_CERTIFICATE_OWNER_MISMATCH;
        return report->status;
    }
    const GLCoreLinkOutput* combined = NULL;
    if (!vector_shape_is_valid(generated, &combined)) {
        report->status = GLCORE_LINK_CERTIFICATE_VECTOR_SHAPE_INVALID;
        return report->status;
    }
    report->response_vector_valid = true;
    report->expected_size = target->released_text_size;
    report->actual_size = combined->size;

    const size_t shared = report->expected_size < report->actual_size
        ? report->expected_size : report->actual_size;
    for (size_t offset = 0U; offset < shared; ++offset) {
        if (target->released_text_bytes[offset] != combined->bytes[offset]) {
            report->first_differing_byte = offset;
            report->status = GLCORE_LINK_CERTIFICATE_TEXT_MISMATCH;
            return report->status;
        }
    }
    if (report->expected_size != report->actual_size) {
        report->first_differing_byte = shared;
        report->status = GLCORE_LINK_CERTIFICATE_TEXT_MISMATCH;
        return report->status;
    }
    report->released_text_exact = true;
    report->status = GLCORE_LINK_CERTIFICATE_OK;
    return report->status;
}

GLCoreLinkCertificateStatus glcore_link_certificate_compare_text(
    const SerializedGLCoreTarget* target,
    const char* generated_unity_version,
    const uint8_t* generated_text, size_t generated_text_size,
    GLCoreLinkCertificateReport* report) {
    if (!target || !generated_unity_version || !generated_text ||
        generated_text_size == 0U) {
        if (report) glcore_link_certificate_report_init(report);
        return GLCORE_LINK_CERTIFICATE_INVALID_ARGUMENT;
    }
    const GLCoreLinkOutput output = {
        .stage = UNITY_SERIALIZED_STAGE_VERTEX,
        .bytes = generated_text,
        .size = generated_text_size,
    };
    const GLCoreGeneratedLinkVector vector = {
        .unity_version = generated_unity_version,
        .compiler_platform = SERIALIZED_GLCORE_PLATFORM,
        .program_type = SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE,
        .owner = target->owner,
        .shape = GLCORE_LINK_VECTOR_COMBINED_ONLY,
        .outputs = &output,
        .output_count = 1U,
    };
    return glcore_link_certificate_compare_vector(target, &vector, report);
}

const char* glcore_link_certificate_status_name(
    GLCoreLinkCertificateStatus status) {
    switch (status) {
        case GLCORE_LINK_CERTIFICATE_OK: return "ok";
        case GLCORE_LINK_CERTIFICATE_INVALID_ARGUMENT:
            return "invalid-argument";
        case GLCORE_LINK_CERTIFICATE_TARGET_INVALID:
            return "target-invalid";
        case GLCORE_LINK_CERTIFICATE_UNITY_VERSION_MISMATCH:
            return "unity-version-mismatch";
        case GLCORE_LINK_CERTIFICATE_PLATFORM_MISMATCH:
            return "platform-mismatch";
        case GLCORE_LINK_CERTIFICATE_PROGRAM_TYPE_MISMATCH:
            return "program-type-mismatch";
        case GLCORE_LINK_CERTIFICATE_OWNER_MISMATCH:
            return "link-owner-mismatch";
        case GLCORE_LINK_CERTIFICATE_VECTOR_SHAPE_INVALID:
            return "link-vector-shape-invalid";
        case GLCORE_LINK_CERTIFICATE_TEXT_MISMATCH:
            return "released-text-mismatch";
        default: return "unknown";
    }
}
