#include "app/whole_shader_certificate.h"
#include "app/whole_shader_evidence.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                    __LINE__, #condition);                                   \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static void fill_digest(uint8_t digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE],
                        uint8_t seed) {
    for (size_t index = 0U; index < WHOLE_SHADER_SUBJECT_DIGEST_SIZE;
         ++index) {
        digest[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static WholeShaderSubject* make_subject(uint8_t seed) {
    WholeShaderSubjectDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.target_shader_path_id = 17;
    descriptor.target_class_id = 48;
    descriptor.serialized_target_platform = 19U;
    descriptor.build_platform = 19U;
    descriptor.compiler_platform = 4;
    descriptor.graphics_api = 4U;
    descriptor.source_residency = WHOLE_SHADER_SOURCE_BUNDLE_MEMBER;
    descriptor.target_member_index = 2U;
    descriptor.target_member_identity = "CAB-fixture";
    descriptor.candidate_logical_name = "DXBCTests/Certificate";
    descriptor.unity_version = "2021.3.35f1";
    uint8_t* fields[] = {
        descriptor.target_occurrence_digest,
        descriptor.target_serialized_file_digest,
        descriptor.target_object_payload_digest,
        descriptor.candidate_source_digest,
        descriptor.schema_authority_digest,
        descriptor.compiler_profile_digest,
        descriptor.compiler_session_digest,
        descriptor.player_profile_digest,
        descriptor.verification_scope_digest,
        descriptor.dependency_map_digest,
        descriptor.producer_fingerprint,
        descriptor.candidate_release_digest,
        descriptor.runtime_environment_digest,
    };
    for (size_t index = 0U; index < sizeof(fields) / sizeof(fields[0]);
         ++index) {
        fill_digest(fields[index], (uint8_t)(seed + index));
    }
    WholeShaderSubject* subject = NULL;
    if (whole_shader_subject_create(&subject, &descriptor) !=
        WHOLE_SHADER_SUBJECT_OK) {
        return NULL;
    }
    return subject;
}

static int add_comparison_with_identity(
    WholeShaderCertificateInput* input, WholeShaderSubject* subject,
    WholeShaderVerificationPlane plane, uint8_t identity_seed, bool match) {
    WholeShaderEvidenceComparisonItem item;
    memset(&item, 0, sizeof(item));
    fill_digest(item.identity_digest, identity_seed);
    fill_digest(item.expected_digest, (uint8_t)(41U + plane));
    memcpy(item.observed_digest, item.expected_digest,
           sizeof(item.observed_digest));
    if (!match) item.observed_digest[0] ^= 1U;
    WholeShaderComparisonEvidenceDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.plane = plane;
    descriptor.producer = "certificate-unit";
    descriptor.producer_version = 1U;
    fill_digest(descriptor.authority_digest, 91U);
    descriptor.items = &item;
    descriptor.item_count = 1U;
    WholeShaderEvidence* evidence = NULL;
    CHECK(whole_shader_evidence_create_comparison(
              &evidence, subject, &descriptor) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    whole_shader_evidence_free(evidence);
    return 0;
}

static int add_comparison(WholeShaderCertificateInput* input,
                          WholeShaderSubject* subject,
                          WholeShaderVerificationPlane plane, bool match) {
    const bool d3d_compile_coverage =
        plane == WHOLE_SHADER_PLANE_VARIANT_DOMAIN ||
        plane == WHOLE_SHADER_PLANE_FULL_DXBC ||
        plane == WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS ||
        plane == WHOLE_SHADER_PLANE_REFLECTION_BINDING;
    const bool finite_render_coverage =
        plane == WHOLE_SHADER_PLANE_RUNTIME_INPUTS ||
        plane == WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS;
    return add_comparison_with_identity(
        input, subject, plane,
        d3d_compile_coverage ? 11U
        : finite_render_coverage ? 21U
                                 : (uint8_t)(11U + plane),
        match);
}

static int add_pass_mask(WholeShaderCertificateInput* input,
                         WholeShaderSubject* subject, uint64_t mask) {
    for (unsigned plane = 0U; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        if ((mask & WHOLE_SHADER_PLANE_BIT(plane)) != 0U) {
            CHECK(add_comparison(input, subject,
                                 (WholeShaderVerificationPlane)plane,
                                 true) == 0);
        }
    }
    return 0;
}

static int test_claim_scopes(void) {
    WholeShaderSubject* subject = make_subject(3U);
    CHECK(subject != NULL);
    WholeShaderCertificateInput* input = NULL;
    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject,
                        WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) == 0);
    WholeShaderCertificateReport report;
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(report.requested_scope_certified);
    CHECK(report.d3d11_byte_equivalence_certified);
    CHECK(report.importer_acceptance_certified);
    CHECK(report.release_object_equivalence_certified);
    CHECK(report.d3d11_logical_equivalence_certified);
    CHECK(!report.glsl_artifact_equivalence_certified);
    CHECK(!report.empirical_visual_equivalence_certified);
    CHECK(!report.source_identity_certified);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_EMPIRICAL_VISUAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject,
                        WHOLE_SHADER_EMPIRICAL_VISUAL_REQUIRED_MASK) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(report.d3d11_logical_equivalence_certified);
    CHECK(report.d3d11_byte_equivalence_certified);
    CHECK(report.empirical_visual_equivalence_certified);
    CHECK(report.finite_pixel_observations_certified);
    CHECK(!report.glsl_artifact_equivalence_certified);
    CHECK(!report.source_identity_certified);
    CHECK(!report.universal_visual_equivalence_certified);
    whole_shader_certificate_input_free(input);

    const uint64_t pixel_bit =
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS);
    CHECK(whole_shader_certificate_input_create(&input, subject, pixel_bit) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject, pixel_bit) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(!report.d3d11_logical_equivalence_certified);
    CHECK(!report.empirical_visual_equivalence_certified);
    CHECK(!report.d3d11_byte_equivalence_certified);
    CHECK(!report.importer_acceptance_certified);
    CHECK(!report.release_object_equivalence_certified);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject,
                        WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(report.finite_pixel_observations_certified);
    CHECK(!report.empirical_visual_equivalence_certified);
    CHECK(!report.universal_visual_equivalence_certified);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_comparison_with_identity(
              input, subject, WHOLE_SHADER_PLANE_RUNTIME_INPUTS, 1U, true) ==
          0);
    CHECK(add_comparison_with_identity(
              input, subject, WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS, 2U,
              true) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_COVERAGE_MISMATCH);
    CHECK(!report.finite_pixel_observations_certified);
    CHECK(!report.empirical_visual_equivalence_certified);
    CHECK((report.coverage_mismatch_plane_mask &
           WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK) ==
          WHOLE_SHADER_FINITE_PIXEL_REQUIRED_MASK);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_EMPIRICAL_VISUAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    for (unsigned plane = 0U; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        if ((WHOLE_SHADER_EMPIRICAL_VISUAL_REQUIRED_MASK &
             WHOLE_SHADER_PLANE_BIT(plane)) == 0U) {
            continue;
        }
        if (plane == WHOLE_SHADER_PLANE_RUNTIME_INPUTS) {
            CHECK(add_comparison_with_identity(
                      input, subject, (WholeShaderVerificationPlane)plane,
                      1U, true) == 0);
        } else if (plane == WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS) {
            CHECK(add_comparison_with_identity(
                      input, subject, (WholeShaderVerificationPlane)plane,
                      2U, true) == 0);
        } else {
            CHECK(add_comparison(input, subject,
                                 (WholeShaderVerificationPlane)plane,
                                 true) == 0);
        }
    }
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_COVERAGE_MISMATCH);
    CHECK(report.d3d11_logical_equivalence_certified);
    CHECK(!report.finite_pixel_observations_certified);
    CHECK(!report.empirical_visual_equivalence_certified);
    whole_shader_certificate_input_free(input);
    whole_shader_subject_free(subject);
    return 0;
}

static int test_fail_closed(void) {
    WholeShaderSubject* subject = make_subject(4U);
    CHECK(subject != NULL);
    const uint64_t bit =
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_STRUCTURAL);
    WholeShaderCertificateInput* input = NULL;
    CHECK(whole_shader_certificate_input_create(&input, subject, bit) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    WholeShaderCertificateReport report;
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE);
    CHECK(report.first_problem_status == WHOLE_SHADER_PLANE_NOT_RUN);
    CHECK(add_comparison(input, subject, WHOLE_SHADER_PLANE_STRUCTURAL,
                         false) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_FAILED);
    CHECK(!report.requested_scope_certified);
    whole_shader_certificate_input_free(input);

    WholeShaderNonpassEvidenceDescriptor unavailable;
    memset(&unavailable, 0, sizeof(unavailable));
    unavailable.plane = WHOLE_SHADER_PLANE_GLSL;
    unavailable.producer = "certificate-unit";
    unavailable.producer_version = 1U;
    unavailable.expected_item_count = 2U;
    unavailable.reason_code = 7U;
    fill_digest(unavailable.authority_digest, 5U);
    fill_digest(unavailable.reason_digest, 6U);
    WholeShaderEvidence* evidence = NULL;
    CHECK(whole_shader_evidence_create_unavailable(
              &evidence, subject, &unavailable) == WHOLE_SHADER_EVIDENCE_OK);
    const uint64_t glsl = WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_GLSL);
    CHECK(whole_shader_certificate_input_create(&input, subject, glsl) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_DUPLICATE_PLANE);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_UNAVAILABLE);
    CHECK(!report.glsl_artifact_equivalence_certified);
    whole_shader_evidence_free(evidence);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject,
              WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK | glsl) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject,
                        WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) == 0);
    CHECK(whole_shader_evidence_create_unavailable(
              &evidence, subject, &unavailable) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_UNAVAILABLE);
    CHECK(!report.requested_scope_certified);
    CHECK(report.d3d11_logical_equivalence_certified);
    CHECK(!report.glsl_artifact_equivalence_certified);
    whole_shader_evidence_free(evidence);
    whole_shader_certificate_input_free(input);
    whole_shader_subject_free(subject);
    return 0;
}

static int test_independent_claim_boundaries(void) {
    WholeShaderSubject* subject = make_subject(8U);
    CHECK(subject != NULL);
    WholeShaderCertificateInput* input = NULL;
    WholeShaderCertificateReport report;

    const WholeShaderVerificationPlane new_logical_planes[] = {
        WHOLE_SHADER_PLANE_PLAYER_PROFILE,
        WHOLE_SHADER_PLANE_RUNTIME_SELECTION,
        WHOLE_SHADER_PLANE_DEPENDENCY_CLOSURE,
    };
    for (size_t index = 0U;
         index < sizeof(new_logical_planes) / sizeof(new_logical_planes[0]);
         ++index) {
        const uint64_t mask = WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK &
            ~WHOLE_SHADER_PLANE_BIT(new_logical_planes[index]);
        CHECK(whole_shader_certificate_input_create(&input, subject, mask) ==
              WHOLE_SHADER_CERTIFICATE_OK);
        CHECK(add_pass_mask(input, subject, mask) == 0);
        CHECK(whole_shader_certificate_evaluate(input, &report) ==
              WHOLE_SHADER_CERTIFICATE_OK);
        CHECK(report.requested_scope_certified);
        CHECK(report.d3d11_byte_equivalence_certified);
        CHECK(!report.d3d11_logical_equivalence_certified);
        whole_shader_certificate_input_free(input);
        input = NULL;
    }

    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject,
                        WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(report.d3d11_byte_equivalence_certified);
    CHECK(!report.importer_acceptance_certified);
    CHECK(!report.release_object_equivalence_certified);
    CHECK(!report.d3d11_logical_equivalence_certified);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_comparison_with_identity(
              input, subject, WHOLE_SHADER_PLANE_VARIANT_DOMAIN, 1U, true) ==
          0);
    CHECK(add_comparison_with_identity(
              input, subject, WHOLE_SHADER_PLANE_FULL_DXBC, 2U, true) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_COVERAGE_MISMATCH);
    CHECK(!report.requested_scope_certified);
    CHECK(!report.d3d11_byte_equivalence_certified);
    CHECK((report.coverage_mismatch_plane_mask &
           WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK) ==
          WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject,
              WHOLE_SHADER_IMPORT_ACCEPTANCE_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject,
                        WHOLE_SHADER_IMPORT_ACCEPTANCE_REQUIRED_MASK) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(report.importer_acceptance_certified);
    CHECK(!report.d3d11_byte_equivalence_certified);
    CHECK(!report.release_object_equivalence_certified);
    whole_shader_certificate_input_free(input);

    CHECK(whole_shader_certificate_input_create(
              &input, subject,
              WHOLE_SHADER_RELEASE_OBJECT_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject,
                        WHOLE_SHADER_RELEASE_OBJECT_REQUIRED_MASK) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(report.release_object_equivalence_certified);
    CHECK(!report.d3d11_byte_equivalence_certified);
    CHECK(!report.importer_acceptance_certified);
    whole_shader_certificate_input_free(input);

    const uint64_t glsl = WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_GLSL);
    CHECK(whole_shader_certificate_input_create(&input, subject, glsl) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(add_pass_mask(input, subject, glsl) == 0);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(report.glsl_artifact_equivalence_certified);
    CHECK(!report.d3d11_byte_equivalence_certified);
    CHECK(!report.d3d11_logical_equivalence_certified);
    CHECK(!report.empirical_visual_equivalence_certified);
    whole_shader_certificate_input_free(input);
    whole_shader_subject_free(subject);
    return 0;
}

static int test_subject_and_scope_rejections(void) {
    WholeShaderSubject* first = make_subject(1U);
    WholeShaderSubject* second = make_subject(2U);
    CHECK(first != NULL && second != NULL);
    WholeShaderCertificateInput* input = NULL;
    CHECK(whole_shader_certificate_input_create(&input, first, 0U) ==
          WHOLE_SHADER_CERTIFICATE_INVALID_REQUEST_MASK);
    CHECK(whole_shader_certificate_input_create(
              &input, first, UINT64_C(1) << 63U) ==
          WHOLE_SHADER_CERTIFICATE_INVALID_REQUEST_MASK);
    const uint64_t structural =
        WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_STRUCTURAL);
    CHECK(whole_shader_certificate_input_create(&input, first, structural) ==
          WHOLE_SHADER_CERTIFICATE_OK);

    WholeShaderEvidenceComparisonItem item;
    memset(&item, 9, sizeof(item));
    WholeShaderComparisonEvidenceDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.plane = WHOLE_SHADER_PLANE_STRUCTURAL;
    descriptor.producer = "certificate-unit";
    descriptor.producer_version = 1U;
    descriptor.items = &item;
    descriptor.item_count = 1U;
    WholeShaderEvidence* evidence = NULL;
    CHECK(whole_shader_evidence_create_comparison(
              &evidence, second, &descriptor) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_SUBJECT_MISMATCH);
    whole_shader_evidence_free(evidence);

    descriptor.plane = WHOLE_SHADER_PLANE_GLSL;
    CHECK(whole_shader_evidence_create_comparison(
              &evidence, first, &descriptor) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_PLANE_NOT_REQUESTED);
    whole_shader_evidence_free(evidence);
    whole_shader_certificate_input_free(input);
    whole_shader_subject_free(first);
    whole_shader_subject_free(second);
    return 0;
}

/* Inject each outcome into every required plane while all siblings pass.
 * This tests aggregation only; synthetic items are not production evidence. */
static int test_every_required_plane(void) {
    WholeShaderSubject *subject = make_subject(12U);
    CHECK(subject);
    const uint64_t required = WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK;
    for (unsigned plane = 0; plane < WHOLE_SHADER_PLANE_COUNT; ++plane) {
        const uint64_t bit = WHOLE_SHADER_PLANE_BIT(plane);
        if (!(required & bit))
            continue;
        for (unsigned fault = 0; fault < 5; ++fault) {
            const uint64_t requested = fault == 4 ? required & ~bit : required;
            WholeShaderCertificateInput *input = NULL;
            CHECK(whole_shader_certificate_input_create(&input, subject, requested) ==
                  WHOLE_SHADER_CERTIFICATE_OK);
            CHECK(add_pass_mask(input, subject, required & ~bit) == 0);
            if (fault == 1) {
                CHECK(add_comparison(input, subject, (WholeShaderVerificationPlane)plane, false) == 0);
            } else if (fault == 2 || fault == 3) {
                WholeShaderNonpassEvidenceDescriptor nonpass = {0};
                nonpass.plane = (WholeShaderVerificationPlane)plane;
                nonpass.producer = "certificate-unit";
                nonpass.producer_version = 1;
                nonpass.expected_item_count = 1;
                nonpass.reason_code = 1;
                fill_digest(nonpass.authority_digest, 23);
                fill_digest(nonpass.reason_digest, 25);
                WholeShaderEvidence *evidence = NULL;
                CHECK((fault == 2 ? whole_shader_evidence_create_unavailable(&evidence, subject, &nonpass)
                                  : whole_shader_evidence_create_not_run(&evidence, subject, &nonpass)) ==
                      WHOLE_SHADER_EVIDENCE_OK);
                CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
                      WHOLE_SHADER_CERTIFICATE_ADD_OK);
                whole_shader_evidence_free(evidence);
            }
            WholeShaderCertificateReport report;
            const WholeShaderCertificateStatus expected[] = {
                WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE,
                WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_FAILED,
                WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_UNAVAILABLE,
                WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE,
                WHOLE_SHADER_CERTIFICATE_OK,
            };
            CHECK(whole_shader_certificate_evaluate(input, &report) == expected[fault]);
            CHECK(report.passed_plane_mask == (required & ~bit));
            CHECK(report.failed_plane_mask == (fault == 1 ? bit : 0));
            CHECK(report.unavailable_plane_mask == (fault == 2 ? bit : 0));
            CHECK(report.incomplete_plane_mask == (fault == 0 || fault == 3 ? bit : 0));
            CHECK(report.invalid_plane_mask == 0 && report.coverage_mismatch_plane_mask == 0);
            CHECK(report.requested_scope_certified == (fault == 4));
            CHECK(!report.d3d11_logical_equivalence_certified);
            CHECK(!report.empirical_visual_equivalence_certified && !report.source_identity_certified);
            CHECK(report.d3d11_byte_equivalence_certified ==
                  ((WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK & bit) == 0));
            const WholeShaderPlaneStatus statuses[] = {
                WHOLE_SHADER_PLANE_NOT_RUN, WHOLE_SHADER_PLANE_FAIL,
                WHOLE_SHADER_PLANE_UNAVAILABLE, WHOLE_SHADER_PLANE_NOT_RUN,
                WHOLE_SHADER_PLANE_NOT_REQUESTED,
            };
            CHECK(report.first_problem_status == statuses[fault]);
            CHECK(report.first_problem_plane == (fault == 4 ? WHOLE_SHADER_PLANE_COUNT :
                                                   (WholeShaderVerificationPlane)plane));
            whole_shader_certificate_input_free(input);
        }
    }
    whole_shader_subject_free(subject);
    return 0;
}

static int test_missing_runtime_coordinates(void) {
    WholeShaderSubject *complete = make_subject(14U);
    CHECK(complete);
    for (unsigned field = 0; field < 4; ++field) {
        WholeShaderSubjectDescriptor descriptor;
        CHECK(whole_shader_subject_describe(complete, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
        uint8_t *coordinates[] = {descriptor.player_profile_digest, descriptor.dependency_map_digest,
                                  descriptor.candidate_release_digest, descriptor.runtime_environment_digest};
        memset(coordinates[field], 0, 32);
        WholeShaderSubject *subject = NULL;
        CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
        WholeShaderCertificateInput *input = NULL;
        CHECK(whole_shader_certificate_input_create(&input, subject,
                  WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) == WHOLE_SHADER_CERTIFICATE_OK);
        const uint64_t runtime = WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_RUNTIME_SELECTION);
        CHECK(add_pass_mask(input, subject, WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK & ~runtime) == 0);
        WholeShaderCertificateReport report;
        CHECK(whole_shader_certificate_evaluate(input, &report) ==
              WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_INCOMPLETE);
        CHECK(report.incomplete_plane_mask == runtime && report.invalid_plane_mask == 0);
        CHECK(add_comparison(input, subject, WHOLE_SHADER_PLANE_RUNTIME_SELECTION, true) == 0);
        CHECK(whole_shader_certificate_evaluate(input, &report) ==
              WHOLE_SHADER_CERTIFICATE_SUBJECT_AUTHORITY_MISSING);
        CHECK(report.invalid_plane_mask == runtime && !(report.passed_plane_mask & runtime));
        CHECK(!report.requested_scope_certified && !report.d3d11_logical_equivalence_certified);
        whole_shader_certificate_input_free(input);
        /* Missing runtime coordinates do not prevent independent byte evidence. */
        CHECK(whole_shader_certificate_input_create(&input, subject,
                  WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK) == WHOLE_SHADER_CERTIFICATE_OK);
        CHECK(add_pass_mask(input, subject, WHOLE_SHADER_D3D11_BYTE_REQUIRED_MASK) == 0);
        CHECK(whole_shader_certificate_evaluate(input, &report) == WHOLE_SHADER_CERTIFICATE_OK);
        CHECK(report.d3d11_byte_equivalence_certified && !report.d3d11_logical_equivalence_certified);
        whole_shader_certificate_input_free(input);
        whole_shader_subject_free(subject);
    }
    whole_shader_subject_free(complete);
    return 0;
}

static int test_names(void) {
    CHECK(strcmp(whole_shader_verification_plane_name(
                     WHOLE_SHADER_PLANE_PLAYER_PROFILE),
                 "player-profile") == 0);
    CHECK(strcmp(whole_shader_verification_plane_name(
                     WHOLE_SHADER_PLANE_RUNTIME_SELECTION),
                 "runtime-selection") == 0);
    CHECK(strcmp(whole_shader_verification_plane_name(
                     WHOLE_SHADER_PLANE_DEPENDENCY_CLOSURE),
                 "dependency-closure") == 0);
    CHECK(strcmp(whole_shader_certificate_add_status_name(
                     WHOLE_SHADER_CERTIFICATE_ADD_SUBJECT_MISMATCH),
                 "subject-mismatch") == 0);
    CHECK(strcmp(whole_shader_certificate_status_name(
                     WHOLE_SHADER_CERTIFICATE_COVERAGE_MISMATCH),
                 "coverage-mismatch") == 0);
    return 0;
}

int main(void) {
    CHECK(test_claim_scopes() == 0);
    CHECK(test_fail_closed() == 0);
    CHECK(test_independent_claim_boundaries() == 0);
    CHECK(test_subject_and_scope_rejections() == 0);
    CHECK(test_every_required_plane() == 0);
    CHECK(test_missing_runtime_coordinates() == 0);
    CHECK(test_names() == 0);
    puts("Whole-shader certificate unit tests passed.");
    return 0;
}
