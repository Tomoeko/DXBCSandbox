// SPDX-License-Identifier: GPL-3.0-only

#include "app/release_shader_evidence.h"
#include "common/file_io.h"
#include "test_shader_subject.h"
#include "test_support/file_mutation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define TEST_PID() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define TEST_PID() ((unsigned long)getpid())
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static bool load(const char *path, const TypeTreeSchemaRegistry *registry, ShaderCatalog *catalog) {
    shader_catalog_init(catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = registry;
    options.retain_source_snapshots = true;
    CHECK(shader_catalog_build(&path, 1U, &options, catalog) == SHADER_CATALOG_OK);
    CHECK(shader_catalog_is_complete(catalog));
    CHECK(catalog->record_count == 1U && catalog->records[0].class_id == 48);
    return true;
}

static bool compare_pair(const char *target_path, const char *candidate_path,
                         const TypeTreeSchemaRegistry *registry, WholeShaderPlaneStatus expected,
                         bool near_misses) {
    ShaderCatalog target, candidate;
    CHECK(load(target_path, registry, &target));
    CHECK(load(candidate_path, registry, &candidate));
    WholeShaderSubjectDescriptor descriptor;
    ShaderCatalogObjectReport captures[2];
    CHECK(test_shader_subject_from_catalogs(&target, &candidate, registry, &descriptor, captures));
    const char *paths[] = {target_path, candidate_path};
    for (size_t side = 0U; side < 2U; ++side) {
        CommonFileView outer;
        uint8_t digest[32];
        CHECK(common_file_view_open_regular(paths[side], SIZE_MAX, &outer) == COMMON_FILE_OK);
        CHECK(common_file_view_sha256(&outer, digest));
        CHECK(memcmp(digest, captures[side].source_artifact_digest, 32U) == 0);
        CHECK(common_file_view_close(&outer) == COMMON_FILE_OK);
    }
    WholeShaderSubject *subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
    ReleaseShaderEvidenceReport report;
    WholeShaderEvidence *evidence = NULL;
    CHECK(release_shader_make_reextraction_evidence(&target, target.records, &candidate,
                                                    candidate.records, registry, subject, &evidence,
                                                    &report) == RELEASE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary summary;
    CHECK(whole_shader_evidence_describe(evidence, &summary) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(summary.plane == WHOLE_SHADER_PLANE_REEXTRACTION && summary.status == expected);
    CHECK(report.canonical.evaluated_field_count == RELEASE_SHADER_FIELD_COUNT);
    if (expected == WHOLE_SHADER_PLANE_PASS) {
        CHECK(report.canonical.canonical_release_identity_certified);
        CHECK(report.canonical.matched_field_count == RELEASE_SHADER_FIELD_COUNT);
        CHECK(summary.expected_item_count == 2U && summary.matched_item_count == 2U);
    }
    printf("reextraction=%s canonical=%s fields=%zu/%zu artifacts=%zu/%zu\n",
           whole_shader_plane_status_name(summary.status),
           release_shader_object_certificate_status_name(report.canonical.status),
           report.canonical.matched_field_count, report.canonical.evaluated_field_count,
           report.canonical.matched_artifact_count, report.canonical.expected_artifact_count);

    WholeShaderCertificateInput *input = NULL;
    CHECK(whole_shader_certificate_input_create(&input, subject,
                                                WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    WholeShaderCertificateReport whole;
    (void)whole_shader_certificate_evaluate(input, &whole);
    CHECK(!whole.requested_scope_certified && !whole.d3d11_logical_equivalence_certified &&
          !whole.d3d11_byte_equivalence_certified && !whole.finite_pixel_observations_certified);
    whole_shader_certificate_input_free(input);
    whole_shader_evidence_free(evidence);

    if (near_misses) {
        for (unsigned mutation = 0U; mutation < 13U; ++mutation) {
            WholeShaderSubjectDescriptor changed = descriptor;
            switch (mutation) {
            case 0U:
                ++changed.target_shader_path_id;
                break;
            case 1U:
                ++changed.serialized_target_platform;
                break;
            case 2U:
                ++changed.build_platform;
                break;
            case 3U:
                changed.source_residency = WHOLE_SHADER_SOURCE_BUNDLE_MEMBER;
                changed.target_member_identity = "CAB-wrong";
                break;
            case 4U:
                changed.source_residency = WHOLE_SHADER_SOURCE_BUNDLE_MEMBER;
                changed.target_member_identity = "CAB-wrong";
                ++changed.target_member_index;
                break;
            case 5U:
                changed.unity_version = "2021.3.34f1";
                break;
            case 6U:
                changed.candidate_logical_name = "Experiment/Wrong";
                break;
            case 7U:
                changed.target_occurrence_digest[0] ^= 1U;
                break;
            case 8U:
                changed.target_serialized_file_digest[0] ^= 1U;
                break;
            case 9U:
                changed.target_object_payload_digest[0] ^= 1U;
                break;
            case 10U:
                changed.schema_authority_digest[0] ^= 1U;
                break;
            case 11U:
                changed.candidate_release_digest[0] ^= 1U;
                break;
            case 12U:
                memset(changed.candidate_release_digest, 0, 32U);
                break;
            }
            WholeShaderSubject *bad = NULL;
            CHECK(whole_shader_subject_create(&bad, &changed) == WHOLE_SHADER_SUBJECT_OK);
            CHECK(release_shader_make_reextraction_evidence(
                      &target, target.records, &candidate, candidate.records, registry, bad,
                      &evidence, &report) == RELEASE_SHADER_EVIDENCE_SUBJECT_MISMATCH);
            CHECK(evidence == NULL);
            whole_shader_subject_free(bad);
        }
        ShaderCatalogRecord copied = target.records[0];
        CHECK(release_shader_make_reextraction_evidence(
                  &target, &copied, &candidate, candidate.records, registry, subject, &evidence,
                  &report) == RELEASE_SHADER_EVIDENCE_TARGET_UNAVAILABLE);
        CHECK(report.target_status == SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED && !evidence);
        CHECK(release_shader_make_reextraction_evidence(
                  &target, target.records, &candidate, &copied, registry, subject, &evidence,
                  &report) == RELEASE_SHADER_EVIDENCE_CANDIDATE_UNAVAILABLE);
        CHECK(report.candidate_status == SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED && !evidence);
        CHECK(release_shader_make_reextraction_evidence(
                  &target, target.records, &candidate, candidate.records, NULL, subject, &evidence,
                  &report) == RELEASE_SHADER_EVIDENCE_TARGET_UNAVAILABLE);
        CHECK(report.target_status == SHADER_CATALOG_OBJECT_SCHEMA_UNAVAILABLE && !evidence);
    }
    whole_shader_subject_free(subject);
    shader_catalog_dispose(&target);
    shader_catalog_dispose(&candidate);
    return true;
}

static bool synthetic_cases(const TypeTreeSchemaRegistry *registry) {
    CHECK(compare_pair(DXBC_RELEASE_FIXTURE, DXBC_RELEASE_FIXTURE, registry,
                       WHOLE_SHADER_PLANE_PASS, true));
    CommonFileBytes file = {0};
    CHECK(common_file_read_regular(DXBC_RELEASE_FIXTURE, 4096U, &file) == COMMON_FILE_OK);
    CHECK(file.size == 272U); /* Authored header/metadata 144 + payload 128. */
    char path[160];
    CHECK(snprintf(path, sizeof(path), "release_evidence_%lu.assets", TEST_PID()) > 0);
    /* Alter a real parsed name byte. It remains decodable and is bound into
     * the new candidate identity, but canonical/payload comparison must fail. */
    file.data[144U + 48U] ^= 1U;
    CHECK(common_file_write_new_atomic(path, file.data, file.size) == COMMON_FILE_OK);
    CHECK(compare_pair(DXBC_RELEASE_FIXTURE, path, registry, WHOLE_SHADER_PLANE_FAIL, false));
    CHECK(remove(path) == 0);
    file.data[144U + 48U] ^= 1U;

    /* A non-null self-PPtr is not resolved merely because both sides have
     * identical numeric IDs. Insert it into this authored fixture's empty
     * root dependency array and update its explicit size fields. */
    uint8_t dependency[284];
    memcpy(dependency, file.data, 264U);
    dependency[260] = 1U;
    memset(dependency + 264U, 0, 12U);
    dependency[268] = 7U;
    memcpy(dependency + 276U, file.data + 264U, 8U);
    dependency[30] = 1U;
    dependency[31] = 28U;
    dependency[112] = 140U;
    CHECK(common_file_write_new_atomic(path, dependency, sizeof(dependency)) == COMMON_FILE_OK);
    CHECK(compare_pair(path, path, registry, WHOLE_SHADER_PLANE_UNAVAILABLE, false));
    CHECK(remove(path) == 0);

    /* Relocation preserves release identity, while same-content pathname
     * replacement of a retained capture invalidates its authority. */
    CHECK(common_file_write_new_atomic(path, file.data, file.size) == COMMON_FILE_OK);
    ShaderCatalog original, relocated;
    CHECK(load(DXBC_RELEASE_FIXTURE, registry, &original));
    CHECK(load(path, registry, &relocated));
    WholeShaderSubjectDescriptor descriptor;
    ShaderCatalogObjectReport reports[2];
    CHECK(test_shader_subject_from_catalogs(&original, &relocated, registry, &descriptor, reports));
    CHECK(memcmp(reports[0].release_digest, reports[1].release_digest, 32U) == 0);
    uint8_t digest[32];
    common_sha256(file.data, file.size, digest);
    CHECK(memcmp(digest, reports[1].source_artifact_digest, 32U) == 0);
    WholeShaderSubject *subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
    CHECK(test_replace_regular_file(path, file.data, file.size));
    WholeShaderEvidence *evidence = NULL;
    ReleaseShaderEvidenceReport report;
    CHECK(release_shader_make_reextraction_evidence(
              &original, original.records, &relocated, relocated.records, registry, subject,
              &evidence, &report) == RELEASE_SHADER_EVIDENCE_CANDIDATE_UNAVAILABLE);
    CHECK(report.candidate_status == SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE && !evidence);
    shader_catalog_dispose(&relocated);
    CHECK(remove(path) == 0);
    /* Change only outer-file padding. A freshly captured candidate has the
     * same object and schema, but is not the build artifact bound by subject. */
    file.data[143] = 0x5aU;
    CHECK(common_file_write_new_atomic(path, file.data, file.size) == COMMON_FILE_OK);
    CHECK(load(path, registry, &relocated));
    CHECK(test_shader_subject_from_catalogs(&original, &relocated, registry, &descriptor, reports));
    CHECK(memcmp(reports[0].payload_digest, reports[1].payload_digest, 32U) == 0);
    CHECK(memcmp(reports[0].schema_digest, reports[1].schema_digest, 32U) == 0);
    CHECK(memcmp(reports[0].release_digest, reports[1].release_digest, 32U) != 0);
    CHECK(release_shader_make_reextraction_evidence(
              &original, original.records, &relocated, relocated.records, registry, subject,
              &evidence, &report) == RELEASE_SHADER_EVIDENCE_SUBJECT_MISMATCH);
    CHECK(!evidence);
    whole_shader_subject_free(subject);
    shader_catalog_dispose(&original);
    shader_catalog_dispose(&relocated);
    CHECK(remove(path) == 0);
    common_file_bytes_dispose(&file);
    return true;
}

int main(int argc, char **argv) {
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    if (typetree_schema_registry_import_file_replace(&registry, DXBC_RELEASE_REGISTRY) !=
        TYPETREE_SCHEMA_OK)
        return 1;
    bool ok;
    if (argc == 4 && (strcmp(argv[3], "pass") == 0 || strcmp(argv[3], "fail") == 0)) {
        ok = compare_pair(argv[1], argv[2], &registry,
                          strcmp(argv[3], "pass") == 0 ? WHOLE_SHADER_PLANE_PASS
                                                       : WHOLE_SHADER_PLANE_FAIL,
                          false);
    } else if (argc == 1) {
        ok = synthetic_cases(&registry);
    } else {
        fprintf(stderr, "Usage: %s [TARGET CANDIDATE pass|fail]\n", argv[0]);
        ok = false;
    }
    typetree_schema_registry_dispose(&registry);
    return ok ? 0 : 1;
}
