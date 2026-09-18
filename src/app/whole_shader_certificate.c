// SPDX-License-Identifier: GPL-3.0-only

#include "app/whole_shader_certificate.h"

#include "app/whole_shader_evidence.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(WHOLE_SHADER_PLANE_COUNT > 0 && WHOLE_SHADER_PLANE_COUNT < 64,
               "whole-shader plane mask must fit uint64_t");

typedef struct {
    bool present;
    WholeShaderEvidenceSummary summary;
} StoredPlaneEvidence;

struct WholeShaderCertificateInput {
    uint32_t format_version;
    uint8_t subject_digest[WHOLE_SHADER_CERTIFICATE_DIGEST_SIZE];
    uint64_t requested_plane_mask;
    uint64_t present_plane_mask;
    StoredPlaneEvidence planes[WHOLE_SHADER_PLANE_COUNT];
};

static uint64_t all_plane_mask(void) {
    return (UINT64_C(1) << WHOLE_SHADER_PLANE_COUNT) - UINT64_C(1);
}

static WholeShaderVerificationPlane first_set_plane(uint64_t mask) {
    for (unsigned plane = 0U; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        if ((mask & WHOLE_SHADER_PLANE_BIT(plane)) != 0U) {
            return (WholeShaderVerificationPlane)plane;
        }
    }
    return WHOLE_SHADER_PLANE_COUNT;
}

static bool mask_is_requested_and_passed(
    const WholeShaderCertificateReport* report, uint64_t required) {
    return report &&
        (report->requested_plane_mask & required) == required &&
        (report->passed_plane_mask & required) == required;
}

static void reject_split_coverage(
    const WholeShaderCertificateInput* input,
    WholeShaderCertificateReport* report,
    WholeShaderVerificationPlane left,
    WholeShaderVerificationPlane right) {
    if (!input || !report) return;
    const uint64_t left_bit = WHOLE_SHADER_PLANE_BIT(left);
    const uint64_t right_bit = WHOLE_SHADER_PLANE_BIT(right);
    const uint64_t pair = left_bit | right_bit;
    if ((report->passed_plane_mask & pair) != pair) return;
    if (memcmp(input->planes[left].summary.coverage_digest,
               input->planes[right].summary.coverage_digest,
               WHOLE_SHADER_CERTIFICATE_DIGEST_SIZE) != 0) {
        report->coverage_mismatch_plane_mask |= pair;
    }
}

static bool evidence_shape_valid(const WholeShaderCertificateInput* input,
                                 unsigned plane) {
    if (!input || plane >= WHOLE_SHADER_PLANE_COUNT ||
        !input->planes[plane].present) {
        return false;
    }
    const WholeShaderEvidenceSummary* summary = &input->planes[plane].summary;
    if (summary->plane != (WholeShaderVerificationPlane)plane ||
        memcmp(summary->subject_digest, input->subject_digest,
               WHOLE_SHADER_CERTIFICATE_DIGEST_SIZE) != 0) {
        return false;
    }
    switch (summary->status) {
        case WHOLE_SHADER_PLANE_PASS:
            return summary->complete && summary->expected_item_count != 0U &&
                summary->observed_item_count == summary->expected_item_count &&
                summary->matched_item_count == summary->expected_item_count;
        case WHOLE_SHADER_PLANE_FAIL:
            return summary->complete && summary->expected_item_count != 0U &&
                summary->observed_item_count == summary->expected_item_count &&
                summary->matched_item_count < summary->expected_item_count;
        case WHOLE_SHADER_PLANE_UNAVAILABLE:
        case WHOLE_SHADER_PLANE_NOT_RUN:
            return !summary->complete && summary->matched_item_count == 0U &&
                summary->observed_item_count <= summary->expected_item_count;
        case WHOLE_SHADER_PLANE_NOT_REQUESTED:
            return false;
    }
    return false;
}

WholeShaderCertificateStatus whole_shader_certificate_input_create(
    WholeShaderCertificateInput** out_input, const WholeShaderSubject* subject,
    uint64_t requested_plane_mask) {
    if (!out_input || !subject) {
        return WHOLE_SHADER_CERTIFICATE_INVALID_ARGUMENT;
    }
    *out_input = NULL;
    if (requested_plane_mask == 0U ||
        (requested_plane_mask & ~all_plane_mask()) != 0U) {
        return WHOLE_SHADER_CERTIFICATE_INVALID_REQUEST_MASK;
    }
    WholeShaderCertificateInput* input =
        (WholeShaderCertificateInput*)calloc(1U, sizeof(*input));
    if (!input) return WHOLE_SHADER_CERTIFICATE_ALLOCATION_FAILED;
    const WholeShaderSubjectStatus subject_status =
        whole_shader_subject_digest(subject, input->subject_digest);
    if (subject_status != WHOLE_SHADER_SUBJECT_OK) {
        free(input);
        if (subject_status == WHOLE_SHADER_SUBJECT_ALLOCATION_FAILED) {
            return WHOLE_SHADER_CERTIFICATE_ALLOCATION_FAILED;
        }
        return WHOLE_SHADER_CERTIFICATE_SUBJECT_AUTHORITY_MISSING;
    }
    input->format_version = WHOLE_SHADER_CERTIFICATE_FORMAT_VERSION;
    input->requested_plane_mask = requested_plane_mask;
    *out_input = input;
    return WHOLE_SHADER_CERTIFICATE_OK;
}

void whole_shader_certificate_input_free(WholeShaderCertificateInput* input) {
    if (!input) return;
    memset(input, 0, sizeof(*input));
    free(input);
}

WholeShaderCertificateAddStatus whole_shader_certificate_input_add_evidence(
    WholeShaderCertificateInput* input, const WholeShaderEvidence* evidence) {
    if (!input || !evidence) {
        return WHOLE_SHADER_CERTIFICATE_ADD_INVALID_ARGUMENT;
    }
    WholeShaderEvidenceSummary summary;
    if (whole_shader_evidence_describe(evidence, &summary) !=
            WHOLE_SHADER_EVIDENCE_OK ||
        summary.plane < WHOLE_SHADER_PLANE_STRUCTURAL ||
        summary.plane >= WHOLE_SHADER_PLANE_COUNT) {
        return WHOLE_SHADER_CERTIFICATE_ADD_INVALID_ARGUMENT;
    }
    const uint64_t bit = WHOLE_SHADER_PLANE_BIT(summary.plane);
    if ((input->requested_plane_mask & bit) == 0U) {
        return WHOLE_SHADER_CERTIFICATE_ADD_PLANE_NOT_REQUESTED;
    }
    if (memcmp(summary.subject_digest, input->subject_digest,
               WHOLE_SHADER_CERTIFICATE_DIGEST_SIZE) != 0) {
        return WHOLE_SHADER_CERTIFICATE_ADD_SUBJECT_MISMATCH;
    }
    if ((input->present_plane_mask & bit) != 0U) {
        return WHOLE_SHADER_CERTIFICATE_ADD_DUPLICATE_PLANE;
    }
    input->planes[summary.plane].present = true;
    input->planes[summary.plane].summary = summary;
    input->present_plane_mask |= bit;
    return WHOLE_SHADER_CERTIFICATE_ADD_OK;
}

void whole_shader_certificate_report_init(WholeShaderCertificateReport* report) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->status = WHOLE_SHADER_CERTIFICATE_INVALID_ARGUMENT;
    report->first_problem_plane = WHOLE_SHADER_PLANE_COUNT;
    report->first_problem_status = WHOLE_SHADER_PLANE_NOT_REQUESTED;
}

WholeShaderCertificateStatus whole_shader_certificate_evaluate(
    const WholeShaderCertificateInput* input,
    WholeShaderCertificateReport* report) {
    if (!report) return WHOLE_SHADER_CERTIFICATE_INVALID_ARGUMENT;
    whole_shader_certificate_report_init(report);
    if (!input) return report->status;
    if (input->format_version != WHOLE_SHADER_CERTIFICATE_FORMAT_VERSION) {
        report->status = WHOLE_SHADER_CERTIFICATE_UNSUPPORTED_VERSION;
        return report->status;
    }
    if (input->requested_plane_mask == 0U ||
        (input->requested_plane_mask & ~all_plane_mask()) != 0U) {
        report->status = WHOLE_SHADER_CERTIFICATE_INVALID_REQUEST_MASK;
        return report->status;
    }
    report->requested_plane_mask = input->requested_plane_mask;
    for (unsigned plane = 0U; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        const uint64_t bit = WHOLE_SHADER_PLANE_BIT(plane);
        if ((input->requested_plane_mask & bit) == 0U) continue;
        if ((input->present_plane_mask & bit) == 0U) {
            report->incomplete_plane_mask |= bit;
            continue;
        }
        if (!evidence_shape_valid(input, plane)) {
            report->invalid_plane_mask |= bit;
            continue;
        }
        switch (input->planes[plane].summary.status) {
            case WHOLE_SHADER_PLANE_PASS:
                report->passed_plane_mask |= bit;
                break;
            case WHOLE_SHADER_PLANE_FAIL:
                report->failed_plane_mask |= bit;
                break;
            case WHOLE_SHADER_PLANE_UNAVAILABLE:
                report->unavailable_plane_mask |= bit;
                break;
            case WHOLE_SHADER_PLANE_NOT_RUN:
                report->incomplete_plane_mask |= bit;
                break;
            case WHOLE_SHADER_PLANE_NOT_REQUESTED:
                report->invalid_plane_mask |= bit;
                break;
        }
    }
    reject_split_coverage(input, report, WHOLE_SHADER_PLANE_VARIANT_DOMAIN,
                          WHOLE_SHADER_PLANE_FULL_DXBC);
    reject_split_coverage(input, report, WHOLE_SHADER_PLANE_VARIANT_DOMAIN,
                          WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS);
    reject_split_coverage(input, report, WHOLE_SHADER_PLANE_VARIANT_DOMAIN,
                          WHOLE_SHADER_PLANE_REFLECTION_BINDING);
    reject_split_coverage(input, report, WHOLE_SHADER_PLANE_RUNTIME_INPUTS,
                          WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS);
    if (report->invalid_plane_mask != 0U) {
        report->status = WHOLE_SHADER_CERTIFICATE_PLANE_EVIDENCE_INVALID;
        report->first_problem_plane = first_set_plane(report->invalid_plane_mask);
        report->first_problem_status =
            input->planes[report->first_problem_plane].summary.status;
    } else if (report->coverage_mismatch_plane_mask != 0U) {
        report->status = WHOLE_SHADER_CERTIFICATE_COVERAGE_MISMATCH;
        report->first_problem_plane =
            first_set_plane(report->coverage_mismatch_plane_mask);
        report->first_problem_status =
            input->planes[report->first_problem_plane].summary.status;
    } else if (report->failed_plane_mask != 0U) {
        report->status = WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_FAILED;
        report->first_problem_plane = first_set_plane(report->failed_plane_mask);
        report->first_problem_status = WHOLE_SHADER_PLANE_FAIL;
    } else if (report->unavailable_plane_mask != 0U) {
        report->status = WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_UNAVAILABLE;
        report->first_problem_plane =
            first_set_plane(report->unavailable_plane_mask);
        report->first_problem_status = WHOLE_SHADER_PLANE_UNAVAILABLE;
    } else if (report->incomplete_plane_mask != 0U) {
        report->status = WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE;
        report->first_problem_plane =
            first_set_plane(report->incomplete_plane_mask);
        report->first_problem_status = WHOLE_SHADER_PLANE_NOT_RUN;
    } else {
        report->status = WHOLE_SHADER_CERTIFICATE_OK;
    }
    const bool shapes_valid = report->invalid_plane_mask == 0U;
    const uint64_t d3d_coverage_mask =
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_VARIANT_DOMAIN) |
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_FULL_DXBC) |
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS) |
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_REFLECTION_BINDING);
    const bool d3d_coverage_valid =
        (report->coverage_mismatch_plane_mask & d3d_coverage_mask) == 0U;
    const bool finite_coverage_valid =
        (report->coverage_mismatch_plane_mask &
         WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK) == 0U;
    report->requested_scope_certified =
        report->status == WHOLE_SHADER_CERTIFICATE_OK;
    report->d3d11_byte_equivalence_certified = shapes_valid &&
        d3d_coverage_valid &&
        mask_is_requested_and_passed(
            report, WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK);
    report->importer_acceptance_certified = shapes_valid &&
        mask_is_requested_and_passed(
            report, WHOLE_SHADER_IMPORT_ACCEPTANCE_REQUIRED_MASK);
    report->release_object_equivalence_certified = shapes_valid &&
        mask_is_requested_and_passed(
            report, WHOLE_SHADER_RELEASE_OBJECT_REQUIRED_MASK);
    report->d3d11_logical_equivalence_certified = shapes_valid &&
        d3d_coverage_valid &&
        mask_is_requested_and_passed(
            report, WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK);
    const uint64_t glsl_bit =
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_GLSL);
    report->glsl_artifact_equivalence_certified = shapes_valid &&
        (report->requested_plane_mask & glsl_bit) != 0U &&
        (report->passed_plane_mask & glsl_bit) != 0U;
    report->finite_pixel_observations_certified = shapes_valid &&
        finite_coverage_valid &&
        mask_is_requested_and_passed(
            report, WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK);
    report->empirical_visual_equivalence_certified = shapes_valid &&
        d3d_coverage_valid && finite_coverage_valid &&
        mask_is_requested_and_passed(
            report, WHOLE_SHADER_EMPIRICAL_VISUAL_REQUIRED_MASK);
    report->source_identity_certified = false;
    report->universal_visual_equivalence_certified = false;
    return report->status;
}

const char* whole_shader_verification_plane_name(
    WholeShaderVerificationPlane plane) {
    switch (plane) {
        case WHOLE_SHADER_PLANE_STRUCTURAL: return "structural";
        case WHOLE_SHADER_PLANE_VARIANT_DOMAIN: return "variant-domain";
        case WHOLE_SHADER_PLANE_FULL_DXBC: return "full-dxbc";
        case WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS:
            return "compiler-diagnostics";
        case WHOLE_SHADER_PLANE_REFLECTION_BINDING:
            return "reflection-binding";
        case WHOLE_SHADER_PLANE_IMPORTER: return "importer";
        case WHOLE_SHADER_PLANE_REEXTRACTION: return "re-extraction";
        case WHOLE_SHADER_PLANE_RENDER_STATE: return "render-state";
        case WHOLE_SHADER_PLANE_PLAYER_PROFILE: return "player-profile";
        case WHOLE_SHADER_PLANE_RUNTIME_SELECTION: return "runtime-selection";
        case WHOLE_SHADER_PLANE_DEPENDENCY_CLOSURE:
            return "dependency-closure";
        case WHOLE_SHADER_PLANE_GLSL: return "glsl";
        case WHOLE_SHADER_PLANE_RUNTIME_INPUTS: return "runtime-inputs";
        case WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS:
            return "empirical-pixels";
        case WHOLE_SHADER_PLANE_COUNT: return "none";
    }
    return "unknown";
}

const char* whole_shader_plane_status_name(WholeShaderPlaneStatus status) {
    switch (status) {
        case WHOLE_SHADER_PLANE_NOT_REQUESTED: return "not-requested";
        case WHOLE_SHADER_PLANE_PASS: return "pass";
        case WHOLE_SHADER_PLANE_FAIL: return "fail";
        case WHOLE_SHADER_PLANE_UNAVAILABLE: return "unavailable";
        case WHOLE_SHADER_PLANE_NOT_RUN: return "not-run";
    }
    return "unknown";
}

const char* whole_shader_certificate_status_name(
    WholeShaderCertificateStatus status) {
    switch (status) {
        case WHOLE_SHADER_CERTIFICATE_OK: return "ok";
        case WHOLE_SHADER_CERTIFICATE_INVALID_ARGUMENT:
            return "invalid-argument";
        case WHOLE_SHADER_CERTIFICATE_UNSUPPORTED_VERSION:
            return "unsupported-version";
        case WHOLE_SHADER_CERTIFICATE_INVALID_REQUEST_MASK:
            return "invalid-request-mask";
        case WHOLE_SHADER_CERTIFICATE_SUBJECT_AUTHORITY_MISSING:
            return "subject-authority-missing";
        case WHOLE_SHADER_CERTIFICATE_PLANE_EVIDENCE_INVALID:
            return "plane-evidence-invalid";
        case WHOLE_SHADER_CERTIFICATE_COVERAGE_MISMATCH:
            return "coverage-mismatch";
        case WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_FAILED:
            return "requested-plane-failed";
        case WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_UNAVAILABLE:
            return "requested-plane-unavailable";
        case WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE:
            return "requested-plane-incomplete";
        case WHOLE_SHADER_CERTIFICATE_ALLOCATION_FAILED:
            return "allocation-failed";
    }
    return "unknown";
}

const char* whole_shader_certificate_add_status_name(
    WholeShaderCertificateAddStatus status) {
    switch (status) {
        case WHOLE_SHADER_CERTIFICATE_ADD_OK: return "ok";
        case WHOLE_SHADER_CERTIFICATE_ADD_INVALID_ARGUMENT:
            return "invalid-argument";
        case WHOLE_SHADER_CERTIFICATE_ADD_PLANE_NOT_REQUESTED:
            return "plane-not-requested";
        case WHOLE_SHADER_CERTIFICATE_ADD_SUBJECT_MISMATCH:
            return "subject-mismatch";
        case WHOLE_SHADER_CERTIFICATE_ADD_DUPLICATE_PLANE:
            return "duplicate-plane";
    }
    return "unknown";
}
