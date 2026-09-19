#include "app/whole_shader_evidence.h"
#include "common/sha256.h"

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

static uint32_t read_u32_le(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
        ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static uint64_t read_u64_le(const uint8_t* data) {
    uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= (uint64_t)data[index] << (index * 8U);
    }
    return value;
}

static WholeShaderSubjectDescriptor make_descriptor(uint8_t seed) {
    WholeShaderSubjectDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.target_shader_path_id = -1234567890123LL;
    descriptor.target_class_id = 48;
    descriptor.serialized_target_platform = 19U;
    descriptor.build_platform = 19U;
    descriptor.compiler_platform = 4;
    descriptor.graphics_api = 4U;
    descriptor.source_residency = WHOLE_SHADER_SOURCE_BUNDLE_MEMBER;
    descriptor.target_member_index = 2U;
    descriptor.target_member_identity = "CAB-fixture";
    descriptor.candidate_logical_name = "DXBCTests/5_Keywords";
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
    };
    for (size_t index = 0U; index < sizeof(fields) / sizeof(fields[0]);
         ++index) {
        fill_digest(fields[index], (uint8_t)(seed + index));
    }
    return descriptor;
}

static int digest_differs(const WholeShaderSubject* left,
                          const WholeShaderSubject* right) {
    uint8_t a[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    uint8_t b[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    CHECK(whole_shader_subject_digest(left, a) == WHOLE_SHADER_SUBJECT_OK);
    CHECK(whole_shader_subject_digest(right, b) == WHOLE_SHADER_SUBJECT_OK);
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    return 0;
}

static int expect_subject_near_miss(
    const WholeShaderSubject* baseline,
    const WholeShaderSubjectDescriptor* descriptor) {
    WholeShaderSubject* changed = NULL;
    CHECK(whole_shader_subject_create(&changed, descriptor) ==
          WHOLE_SHADER_SUBJECT_OK);
    CHECK(!whole_shader_subject_equal(baseline, changed));
    CHECK(digest_differs(baseline, changed) == 0);
    whole_shader_subject_free(changed);
    return 0;
}

static int test_subject_canonical_encoding(void) {
    CHECK(!whole_shader_subject_equal(NULL, NULL));
    char member[] = "CAB-fixture";
    char name[] = "DXBCTests/5_Keywords";
    WholeShaderSubjectDescriptor descriptor = make_descriptor(3U);
    descriptor.target_member_identity = member;
    descriptor.candidate_logical_name = name;
    WholeShaderSubject* subject = NULL;
    WholeShaderSubject* same = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) ==
          WHOLE_SHADER_SUBJECT_OK);
    CHECK(whole_shader_subject_create(&same, &descriptor) ==
          WHOLE_SHADER_SUBJECT_OK);
    CHECK(whole_shader_subject_equal(subject, same));
    member[0] = 'X';
    name[0] = 'Y';
    CHECK(whole_shader_subject_equal(subject, same));

    uint8_t* bytes = NULL;
    size_t size = 0U;
    CHECK(whole_shader_subject_serialize(subject, &bytes, &size) ==
          WHOLE_SHADER_SUBJECT_OK);
    CHECK(size > 20U);
    CHECK(memcmp(bytes, "DXWSSUBJ", 8U) == 0);
    CHECK(read_u32_le(bytes + 8U) == WHOLE_SHADER_SUBJECT_FORMAT_VERSION);
    CHECK(read_u64_le(bytes + 12U) == size);
    uint8_t golden_digest[WHOLE_SHADER_SUBJECT_DIGEST_SIZE];
    char golden_hex[COMMON_SHA256_HEX_SIZE];
    CHECK(whole_shader_subject_digest(subject, golden_digest) ==
          WHOLE_SHADER_SUBJECT_OK);
    common_sha256_digest_to_hex(golden_digest, golden_hex);
    if (strcmp(golden_hex,
               "d80392e9ff8e2a160a9f42c8498803fa27576b963c206970a9a948ad19ea5030") != 0) {
        fprintf(stderr, "Unexpected subject digest: %s\n", golden_hex);
    }
    CHECK(strcmp(golden_hex,
                 "d80392e9ff8e2a160a9f42c8498803fa27576b963c206970a9a948ad19ea5030") ==
          0);
    whole_shader_subject_serialized_free(bytes);

    WholeShaderSubjectDescriptor canonical = make_descriptor(3U);
    WholeShaderSubjectDescriptor near = canonical;
    near.target_shader_path_id++;
    WholeShaderSubject* changed = NULL;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.serialized_target_platform++;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.build_platform++;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.compiler_platform++;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.graphics_api++;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.target_member_index++;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.source_residency = WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE;
    near.target_member_identity = "";
    near.target_member_index = 0U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.target_member_identity = "CAB-other";
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.candidate_logical_name = "DXBCTests/Other";
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.unity_version = "2021.3.34f1";
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.target_occurrence_digest[0] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.target_serialized_file_digest[1] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.target_object_payload_digest[2] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.candidate_source_digest[3] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.schema_authority_digest[4] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.compiler_profile_digest[5] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.compiler_session_digest[6] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.player_profile_digest[31] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.verification_scope_digest[9] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.dependency_map_digest[7] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.producer_fingerprint[8] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);
    near = canonical;
    near.candidate_release_digest[10] ^= 1U;
    CHECK(expect_subject_near_miss(subject, &near) == 0);

    WholeShaderSubjectDescriptor split_a = make_descriptor(8U);
    WholeShaderSubjectDescriptor split_b = split_a;
    split_a.target_member_identity = "a";
    split_a.candidate_logical_name = "bc";
    split_b.target_member_identity = "ab";
    split_b.candidate_logical_name = "c";
    WholeShaderSubject* a = NULL;
    WholeShaderSubject* b = NULL;
    CHECK(whole_shader_subject_create(&a, &split_a) ==
          WHOLE_SHADER_SUBJECT_OK);
    CHECK(whole_shader_subject_create(&b, &split_b) ==
          WHOLE_SHADER_SUBJECT_OK);
    CHECK(digest_differs(a, b) == 0);
    whole_shader_subject_free(a);
    whole_shader_subject_free(b);

    near = canonical;
    near.target_member_identity = "/tmp/CAB-fixture";
    CHECK(whole_shader_subject_create(&changed, &near) ==
          WHOLE_SHADER_SUBJECT_INVALID_VALUE);
    CHECK(changed == NULL);
    near = canonical;
    near.source_residency = WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE;
    CHECK(whole_shader_subject_create(&changed, &near) ==
          WHOLE_SHADER_SUBJECT_INVALID_VALUE);
    CHECK(changed == NULL);
    near = canonical;
    near.source_residency = (WholeShaderSourceResidency)0;
    CHECK(whole_shader_subject_create(&changed, &near) ==
          WHOLE_SHADER_SUBJECT_INVALID_VALUE);
    CHECK(changed == NULL);
    near = canonical;
    near.target_class_id = 72;
    CHECK(whole_shader_subject_create(&changed, &near) ==
          WHOLE_SHADER_SUBJECT_INVALID_VALUE);
    CHECK(changed == NULL);
    whole_shader_subject_free(subject);
    whole_shader_subject_free(same);
    return 0;
}

static void make_item(WholeShaderEvidenceComparisonItem* item,
                      uint8_t identity, uint8_t value) {
    memset(item, 0, sizeof(*item));
    fill_digest(item->identity_digest, identity);
    fill_digest(item->expected_digest, value);
    memcpy(item->observed_digest, item->expected_digest,
           sizeof(item->observed_digest));
}

static WholeShaderEvidenceStatus make_evidence(
    WholeShaderEvidence** output, WholeShaderSubject* subject,
    const char* producer, uint32_t version, uint8_t authority,
    WholeShaderEvidenceComparisonItem* items, size_t item_count) {
    WholeShaderComparisonEvidenceDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.plane = WHOLE_SHADER_PLANE_FULL_DXBC;
    descriptor.producer = producer;
    descriptor.producer_version = version;
    fill_digest(descriptor.authority_digest, authority);
    descriptor.items = items;
    descriptor.item_count = item_count;
    return whole_shader_evidence_create_comparison(output, subject,
                                                    &descriptor);
}

static int evidence_digest_differs(const WholeShaderEvidence* left,
                                   const WholeShaderEvidence* right) {
    uint8_t a[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    uint8_t b[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    CHECK(whole_shader_evidence_digest(left, a) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_evidence_digest(right, b) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    return 0;
}

static int test_evidence_is_derived_and_ordered(void) {
    CHECK(!whole_shader_evidence_equal(NULL, NULL));
    WholeShaderSubjectDescriptor subject_descriptor = make_descriptor(5U);
    WholeShaderSubject* subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &subject_descriptor) ==
          WHOLE_SHADER_SUBJECT_OK);
    WholeShaderEvidenceComparisonItem items[2];
    make_item(&items[0], 1U, 31U);
    make_item(&items[1], 2U, 41U);
    WholeShaderEvidence* pass = NULL;
    WholeShaderEvidence* same = NULL;
    CHECK(make_evidence(&pass, subject, "dxbc-container-comparator", 1U,
                        7U, items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(make_evidence(&same, subject, "dxbc-container-comparator", 1U,
                        7U, items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_evidence_equal(pass, same));
    WholeShaderEvidenceSummary summary;
    CHECK(whole_shader_evidence_describe(pass, &summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(summary.status == WHOLE_SHADER_PLANE_PASS);
    CHECK(summary.complete);
    CHECK(summary.expected_item_count == 2U);
    CHECK(summary.matched_item_count == 2U);
    char coverage_hex[COMMON_SHA256_HEX_SIZE];
    common_sha256_digest_to_hex(summary.coverage_digest, coverage_hex);
    CHECK(strcmp(coverage_hex,
                 "597f25f1b9659d06e58bd08d5e5bf7bf6bfe0847fffc63121b1ca285889640e0") ==
          0);

    uint8_t* bytes = NULL;
    size_t size = 0U;
    CHECK(whole_shader_evidence_serialize(pass, &bytes, &size) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(memcmp(bytes, "DXWSEVID", 8U) == 0);
    CHECK(read_u32_le(bytes + 8U) == WHOLE_SHADER_EVIDENCE_FORMAT_VERSION);
    CHECK(read_u64_le(bytes + 12U) == size);
    uint8_t golden_digest[WHOLE_SHADER_EVIDENCE_DIGEST_SIZE];
    char golden_hex[COMMON_SHA256_HEX_SIZE];
    CHECK(whole_shader_evidence_digest(pass, golden_digest) ==
          WHOLE_SHADER_EVIDENCE_OK);
    common_sha256_digest_to_hex(golden_digest, golden_hex);
    if (strcmp(golden_hex,
               "30af4166d917b92f80eeab9776880cf354fb829ba7e9a03673ab72e5b5f8dd81") !=
        0) {
        fprintf(stderr, "unexpected evidence digest: %s\n", golden_hex);
    }
    CHECK(strcmp(golden_hex,
                 "30af4166d917b92f80eeab9776880cf354fb829ba7e9a03673ab72e5b5f8dd81") ==
          0);
    whole_shader_evidence_serialized_free(bytes);

    WholeShaderEvidenceComparisonItem reversed[2] = {items[1], items[0]};
    WholeShaderEvidence* changed = NULL;
    CHECK(make_evidence(&changed, subject, "dxbc-container-comparator", 1U,
                        7U, reversed, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(pass, changed) == 0);
    WholeShaderEvidenceSummary changed_summary;
    CHECK(whole_shader_evidence_describe(changed, &changed_summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(memcmp(summary.coverage_digest, changed_summary.coverage_digest,
                 sizeof(summary.coverage_digest)) != 0);
    whole_shader_evidence_free(changed);

    items[1].observed_digest[0] ^= 1U;
    CHECK(make_evidence(&changed, subject, "dxbc-container-comparator", 1U,
                        7U, items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_evidence_describe(changed, &summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(summary.status == WHOLE_SHADER_PLANE_FAIL);
    CHECK(summary.complete && summary.matched_item_count == 1U);
    CHECK(evidence_digest_differs(pass, changed) == 0);
    whole_shader_evidence_free(changed);
    items[1].observed_digest[0] ^= 1U;

    CHECK(make_evidence(&changed, subject, "other-producer", 1U, 7U,
                        items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(pass, changed) == 0);
    whole_shader_evidence_free(changed);
    CHECK(make_evidence(&changed, subject, "dxbc-container-comparator", 2U,
                        7U, items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(pass, changed) == 0);
    whole_shader_evidence_free(changed);
    CHECK(make_evidence(&changed, subject, "dxbc-container-comparator", 1U,
                        8U, items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(pass, changed) == 0);
    whole_shader_evidence_free(changed);

    items[1].identity_digest[0] ^= 1U;
    CHECK(make_evidence(&changed, subject, "dxbc-container-comparator", 1U,
                        7U, items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(pass, changed) == 0);
    whole_shader_evidence_free(changed);
    items[1].identity_digest[0] ^= 1U;
    items[1].expected_digest[0] ^= 1U;
    items[1].observed_digest[0] ^= 1U;
    CHECK(make_evidence(&changed, subject, "dxbc-container-comparator", 1U,
                        7U, items, 2U) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(pass, changed) == 0);
    whole_shader_evidence_free(changed);
    items[1].expected_digest[0] ^= 1U;
    items[1].observed_digest[0] ^= 1U;

    memcpy(items[1].identity_digest, items[0].identity_digest,
           sizeof(items[1].identity_digest));
    CHECK(make_evidence(&changed, subject, "dxbc-container-comparator", 1U,
                        7U, items, 2U) ==
          WHOLE_SHADER_EVIDENCE_DUPLICATE_ITEM);
    CHECK(changed == NULL);
    whole_shader_evidence_free(pass);
    whole_shader_evidence_free(same);
    whole_shader_subject_free(subject);
    return 0;
}

static int test_nonpass_is_explicit(void) {
    WholeShaderSubjectDescriptor descriptor = make_descriptor(6U);
    WholeShaderSubject* subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) ==
          WHOLE_SHADER_SUBJECT_OK);
    WholeShaderNonpassEvidenceDescriptor nonpass;
    memset(&nonpass, 0, sizeof(nonpass));
    nonpass.plane = WHOLE_SHADER_PLANE_EMPIRICAL_PIXELS;
    nonpass.producer = "pixel-fixture";
    nonpass.producer_version = 1U;
    nonpass.expected_item_count = 4U;
    nonpass.observed_item_count = 0U;
    nonpass.reason_code = 10U;
    fill_digest(nonpass.authority_digest, 1U);
    fill_digest(nonpass.reason_digest, 2U);
    WholeShaderEvidence* evidence = NULL;
    CHECK(whole_shader_evidence_create_unavailable(
              &evidence, subject, &nonpass) == WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary summary;
    CHECK(whole_shader_evidence_describe(evidence, &summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(summary.status == WHOLE_SHADER_PLANE_UNAVAILABLE);
    CHECK(!summary.complete && summary.matched_item_count == 0U);
    WholeShaderEvidence* changed = NULL;
    nonpass.reason_digest[0] ^= 1U;
    CHECK(whole_shader_evidence_create_unavailable(
              &changed, subject, &nonpass) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(evidence, changed) == 0);
    whole_shader_evidence_free(changed);
    nonpass.reason_digest[0] ^= 1U;
    ++nonpass.reason_code;
    CHECK(whole_shader_evidence_create_unavailable(
              &changed, subject, &nonpass) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(evidence, changed) == 0);
    whole_shader_evidence_free(changed);
    --nonpass.reason_code;
    ++nonpass.expected_item_count;
    CHECK(whole_shader_evidence_create_unavailable(
              &changed, subject, &nonpass) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(evidence_digest_differs(evidence, changed) == 0);
    whole_shader_evidence_free(changed);
    --nonpass.expected_item_count;
    whole_shader_evidence_free(evidence);

    nonpass.reason_code = 0U;
    CHECK(whole_shader_evidence_create_not_run(
              &evidence, subject, &nonpass) ==
          WHOLE_SHADER_EVIDENCE_INVALID_COUNT);
    CHECK(evidence == NULL);
    whole_shader_subject_free(subject);
    return 0;
}

static int test_invalid_constructors_fail_closed(void) {
    WholeShaderSubjectDescriptor subject_descriptor = make_descriptor(9U);
    WholeShaderSubject* subject = NULL;
    CHECK(whole_shader_subject_create(NULL, &subject_descriptor) ==
          WHOLE_SHADER_SUBJECT_INVALID_ARGUMENT);
    CHECK(whole_shader_subject_create(&subject, &subject_descriptor) ==
          WHOLE_SHADER_SUBJECT_OK);
    WholeShaderEvidenceComparisonItem item;
    make_item(&item, 1U, 2U);
    WholeShaderComparisonEvidenceDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.plane = WHOLE_SHADER_PLANE_FULL_DXBC;
    descriptor.producer = "producer";
    descriptor.producer_version = 1U;
    descriptor.items = &item;
    descriptor.item_count = 1U;
    WholeShaderEvidence* evidence = NULL;

    descriptor.plane = WHOLE_SHADER_PLANE_COUNT;
    CHECK(whole_shader_evidence_create_comparison(
              &evidence, subject, &descriptor) ==
          WHOLE_SHADER_EVIDENCE_INVALID_PLANE);
    CHECK(evidence == NULL);
    descriptor.plane = WHOLE_SHADER_PLANE_FULL_DXBC;
    descriptor.producer = "";
    CHECK(whole_shader_evidence_create_comparison(
              &evidence, subject, &descriptor) ==
          WHOLE_SHADER_EVIDENCE_INVALID_PRODUCER);
    CHECK(evidence == NULL);
    descriptor.producer = "producer";
    descriptor.producer_version = 0U;
    CHECK(whole_shader_evidence_create_comparison(
              &evidence, subject, &descriptor) ==
          WHOLE_SHADER_EVIDENCE_INVALID_PRODUCER);
    CHECK(evidence == NULL);
    descriptor.producer_version = 1U;
    descriptor.item_count = 0U;
    CHECK(whole_shader_evidence_create_comparison(
              &evidence, subject, &descriptor) ==
          WHOLE_SHADER_EVIDENCE_INVALID_COUNT);
    CHECK(evidence == NULL);
    CHECK(whole_shader_evidence_create_comparison(
              NULL, subject, &descriptor) ==
          WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    whole_shader_subject_free(subject);
    return 0;
}

int main(void) {
    CHECK(test_subject_canonical_encoding() == 0);
    CHECK(test_evidence_is_derived_and_ordered() == 0);
    CHECK(test_nonpass_is_explicit() == 0);
    CHECK(test_invalid_constructors_fail_closed() == 0);
    puts("Whole-shader subject/evidence unit tests passed.");
    return 0;
}
