#include "app/release_shader_certificate_job.h"
#include "app/shader_catalog_object.h"

#include <stdio.h>
#include <string.h>

#ifndef DXBC_TEST_PLAYER_SCHEMA_REGISTRY
#error DXBC_TEST_PLAYER_SCHEMA_REGISTRY must name the pinned registry
#endif
#ifndef DXBC_TEST_SHADER_BUNDLE
#error DXBC_TEST_SHADER_BUNDLE must name the collected Shader bundle
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static const char k_occurrence_a[] =
    "o:1111111111111111111111111111111111111111111111111111111111111111:7";
static const char k_occurrence_b[] =
    "o:2222222222222222222222222222222222222222222222222222222222222222:8";
static const char k_content_a[] =
    "s:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:7";
static const char k_content_b[] =
    "s:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb:8";
static const char k_sha_a[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char k_sha_b[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static bool test_pair_parser(void) {
    char valid[1024];
    int count = snprintf(valid, sizeof(valid),
        "expected_occurrence_id\texpected_content_id\t"
        "expected_serialized_sha256\tactual_occurrence_id\t"
        "actual_content_id\tactual_serialized_sha256\n"
        "%s\t%s\t%s\t%s\t%s\t%s\n",
        k_occurrence_a, k_content_a, k_sha_a,
        k_occurrence_b, k_content_b, k_sha_b);
    CHECK(count > 0 && (size_t)count < sizeof(valid));
    const size_t valid_size = (size_t)count;
    ReleaseShaderCertificatePairTable table;
    release_shader_certificate_pair_table_init(&table);
    size_t error_line = 99U;
    CHECK(release_shader_certificate_pairs_parse(
              (const uint8_t*)valid, (size_t)count, &table,
              &error_line) == RELEASE_SHADER_CERTIFICATE_TABLE_OK);
    CHECK(error_line == 0U && table.count == 1U);
    CHECK(table.rows[0].line_number == 2U);
    CHECK(strcmp(table.rows[0].expected_occurrence_id,
                 k_occurrence_a) == 0);
    CHECK(strcmp(table.rows[0].actual_serialized_sha256, k_sha_b) == 0);

    char duplicate[2048];
    count = snprintf(duplicate, sizeof(duplicate),
        "expected_occurrence_id\texpected_content_id\t"
        "expected_serialized_sha256\tactual_occurrence_id\t"
        "actual_content_id\tactual_serialized_sha256\n"
        "%s\t%s\t%s\t%s\t%s\t%s\n"
        "%s\t%s\t%s\t%s\t%s\t%s\n",
        k_occurrence_a, k_content_a, k_sha_a,
        k_occurrence_b, k_content_b, k_sha_b,
        k_occurrence_a, k_content_a, k_sha_a,
        k_occurrence_a, k_content_a, k_sha_a);
    CHECK(count > 0 && (size_t)count < sizeof(duplicate));
    CHECK(release_shader_certificate_pairs_parse(
              (const uint8_t*)duplicate, (size_t)count, &table,
              &error_line) == RELEASE_SHADER_CERTIFICATE_TABLE_DUPLICATE_ROW);
    CHECK(error_line == 3U);
    /* Strong output: failed replacement retained the valid table. */
    CHECK(table.count == 1U &&
          strcmp(table.rows[0].actual_occurrence_id, k_occurrence_b) == 0);

    char bad_binding[1024];
    count = snprintf(bad_binding, sizeof(bad_binding),
        "expected_occurrence_id\texpected_content_id\t"
        "expected_serialized_sha256\tactual_occurrence_id\t"
        "actual_content_id\tactual_serialized_sha256\n"
        "%s\t%s\t%s\t%s\t%s\t%s\n",
        k_occurrence_a, k_content_a, k_sha_b,
        k_occurrence_b, k_content_b, k_sha_b);
    CHECK(count > 0 && (size_t)count < sizeof(bad_binding));
    CHECK(release_shader_certificate_pairs_parse(
              (const uint8_t*)bad_binding, (size_t)count, &table,
              &error_line) == RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW);
    CHECK(error_line == 2U);
    CHECK(release_shader_certificate_pairs_parse(
              (const uint8_t*)valid, valid_size - 1U, &table,
              &error_line) == RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW);
    release_shader_certificate_pair_table_dispose(&table);
    return true;
}

static bool test_reference_parser(void) {
    static const char valid[] =
        "side\towner_serialized_sha256\tfile_id\tpath_id\t"
        "stable_id_sha256\n"
        "expected\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "\t0\t41\tcccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "actual\tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "\t2\t-9\tcccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n";
    ReleaseShaderCertificateReferenceMap map;
    release_shader_certificate_reference_map_init(&map);
    size_t error_line = 0U;
    CHECK(release_shader_certificate_reference_map_parse(
              (const uint8_t*)valid, sizeof(valid) - 1U, &map,
              &error_line) == RELEASE_SHADER_CERTIFICATE_TABLE_OK);
    CHECK(map.count == 2U);
    CHECK(map.rows[0].side == RELEASE_SHADER_SIDE_EXPECTED);
    CHECK(map.rows[0].file_id == 0 && map.rows[0].path_id == 41);
    CHECK(memcmp(map.rows[0].stable_id, map.rows[1].stable_id,
                 RELEASE_SHADER_REFERENCE_ID_SIZE) == 0);

    static const char duplicate[] =
        "side\towner_serialized_sha256\tfile_id\tpath_id\t"
        "stable_id_sha256\n"
        "expected\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "\t0\t41\tcccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "expected\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "\t0\t41\tdddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n";
    CHECK(release_shader_certificate_reference_map_parse(
              (const uint8_t*)duplicate, sizeof(duplicate) - 1U, &map,
              &error_line) == RELEASE_SHADER_CERTIFICATE_TABLE_DUPLICATE_ROW);
    CHECK(error_line == 3U && map.count == 2U);

    static const char null_row[] =
        "side\towner_serialized_sha256\tfile_id\tpath_id\t"
        "stable_id_sha256\n"
        "expected\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "\t0\t0\tcccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n";
    CHECK(release_shader_certificate_reference_map_parse(
              (const uint8_t*)null_row, sizeof(null_row) - 1U, &map,
              &error_line) == RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW);
    CHECK(error_line == 2U);
    release_shader_certificate_reference_map_dispose(&map);
    return true;
}

static bool test_collected_bundle_self_pair(void) {
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(
              &registry, DXBC_TEST_PLAYER_SCHEMA_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = &registry;
    options.retain_source_snapshots = true;
    const char* inputs[] = {DXBC_TEST_SHADER_BUNDLE};
    CHECK(shader_catalog_build(inputs, 1U, &options, &catalog) ==
          SHADER_CATALOG_OK);
    const ShaderCatalogRecord* selected = NULL;
    for (size_t index = 0U; index < catalog.record_count; ++index) {
        if (catalog.records[index].class_id == 48 &&
            catalog.records[index].status == SHADER_CATALOG_RECORD_READY &&
            catalog.records[index].name &&
            strcmp(catalog.records[index].name,
                   "Hidden/SeparableBlur") == 0) {
            selected = &catalog.records[index];
            break;
        }
    }
    CHECK(selected != NULL);
    ShaderObject decoded;
    ShaderCatalogObjectReport decoded_report;
    shader_object_init(&decoded);
    CHECK(shader_catalog_decode_object(&catalog, selected, &registry, &decoded,
                                        &decoded_report) == SHADER_CATALOG_OBJECT_OK);
    CHECK(decoded.decoded && decoded.path_id == selected->path_id);
    CHECK(decoded_report.source_matches == 1U);
    uint8_t schema_digest[COMMON_SHA256_DIGEST_SIZE];
    CHECK(typetree_schema_shape_digest(&decoded.schema, schema_digest));
    CHECK(memcmp(schema_digest, decoded_report.schema_digest, sizeof(schema_digest)) == 0);
    ShaderCatalogObjectReport original_report = decoded_report;
    const void* original_schema_nodes = decoded.schema.nodes;
    ShaderCatalogRecord copy = *selected;
    CHECK(shader_catalog_decode_object(&catalog, &copy, &registry, &decoded,
                                        &decoded_report) ==
          SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED);
    CHECK(decoded.schema.nodes == original_schema_nodes);
    ShaderCatalogRecord* mutable_record = &catalog.records[selected - catalog.records];
    for (unsigned mutation = 0U; mutation < 8U; ++mutation) {
        switch (mutation) {
            case 0U: ++mutable_record->path_id; break;
            case 1U: ++mutable_record->object_size; break;
            case 2U: ++mutable_record->target_platform; break;
            case 3U: ++mutable_record->member_index; break;
            case 4U: mutable_record->serialized_digest[0] ^= 1U; break;
            case 5U: mutable_record->serialized_digest_hex[0] ^= 1U; break;
            case 6U: mutable_record->occurrence_digest_hex[0] ^= 1U; break;
            case 7U: mutable_record->content_id[0] ^= 1U; break;
        }
        CHECK(shader_catalog_decode_object(&catalog, selected, &registry, &decoded,
                                            &decoded_report) ==
              SHADER_CATALOG_OBJECT_COORDINATE_MISMATCH);
        CHECK(decoded.schema.nodes == original_schema_nodes);
        const uint8_t zeros[COMMON_SHA256_DIGEST_SIZE] = {0};
        CHECK(memcmp(decoded_report.payload_digest, zeros, sizeof(zeros)) == 0);
        CHECK(memcmp(decoded_report.schema_digest, zeros, sizeof(zeros)) == 0);
        CHECK(memcmp(decoded_report.source_artifact_digest, zeros, sizeof(zeros)) == 0);
        CHECK(memcmp(decoded_report.release_digest, zeros, sizeof(zeros)) == 0);
        *mutable_record = copy;
    }
    size_t retained_count = catalog.retained_source_snapshot_count;
    catalog.retained_source_snapshot_count = 0U;
    CHECK(shader_catalog_decode_object(&catalog, selected, &registry, &decoded,
                                        &decoded_report) ==
          SHADER_CATALOG_OBJECT_SOURCE_UNAVAILABLE);
    CHECK(decoded.schema.nodes == original_schema_nodes);
    catalog.retained_source_snapshot_count = retained_count;
    /* An embedded schema remains authoritative without an external registry. */
    CHECK(catalog.source_count == 1U && catalog.sources[0].type_tree_enabled);
    CHECK(shader_catalog_decode_object(&catalog, selected, NULL, &decoded,
                                        &decoded_report) == SHADER_CATALOG_OBJECT_OK);
    CHECK(shader_catalog_decode_object(&catalog, selected, &registry, &decoded,
                                        &decoded_report) == SHADER_CATALOG_OBJECT_OK);
    CHECK(memcmp(decoded_report.payload_digest, original_report.payload_digest,
                 sizeof(decoded_report.payload_digest)) == 0);
    shader_object_dispose(&decoded);
    ReleaseShaderCertificatePair pair;
    memset(&pair, 0, sizeof(pair));
    pair.line_number = 2U;
    memcpy(pair.expected_occurrence_id, selected->occurrence_id,
           sizeof(pair.expected_occurrence_id));
    memcpy(pair.expected_content_id, selected->content_id,
           sizeof(pair.expected_content_id));
    memcpy(pair.expected_serialized_sha256, selected->serialized_digest_hex,
           sizeof(pair.expected_serialized_sha256));
    memcpy(pair.actual_occurrence_id, selected->occurrence_id,
           sizeof(pair.actual_occurrence_id));
    memcpy(pair.actual_content_id, selected->content_id,
           sizeof(pair.actual_content_id));
    memcpy(pair.actual_serialized_sha256, selected->serialized_digest_hex,
           sizeof(pair.actual_serialized_sha256));
    ReleaseShaderCertificatePairTable pairs = {&pair, 1U};
    ReleaseShaderCertificateReferenceMap references = {NULL, 0U};
    ReleaseShaderCertificateCompareResult result;
    release_shader_certificate_compare_result_init(&result);
    CHECK(release_shader_certificate_compare(
              DXBC_TEST_SHADER_BUNDLE, DXBC_TEST_SHADER_BUNDLE,
              &pairs, &references, &registry, &result) ==
          RELEASE_SHADER_CERTIFICATE_COMPARE_EXACT);
    CHECK(result.pair_count == 1U && result.exact_count == 1U);
    CHECK(result.pairs[0].status == RELEASE_SHADER_CERTIFICATE_PAIR_EXACT);
    CHECK(result.pairs[0].certificate.canonical_release_identity_certified);
    CHECK(result.pairs[0].certificate.compiled_artifacts_exact);
    CHECK(!result.pairs[0].certificate.source_identity_certified);
    CHECK(!result.pairs[0].certificate.visual_output_certified);
    CHECK(strcmp(result.pairs[0].expected.name,
                 "Hidden/SeparableBlur") == 0);
    release_shader_certificate_compare_result_dispose(&result);

    pair.actual_serialized_sha256[0] = '0';
    release_shader_certificate_compare_result_init(&result);
    CHECK(release_shader_certificate_compare(
              DXBC_TEST_SHADER_BUNDLE, DXBC_TEST_SHADER_BUNDLE,
              &pairs, &references, &registry, &result) ==
          RELEASE_SHADER_CERTIFICATE_COMPARE_INPUT_INVALID);
    CHECK(result.invalid_count == 1U);
    CHECK(result.pairs[0].diagnostic ==
          RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_COORDINATE_MISMATCH);
    release_shader_certificate_compare_result_dispose(&result);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    return true;
}

int main(void) {
    if (!test_pair_parser() || !test_reference_parser() ||
        !test_collected_bundle_self_pair()) {
        return 1;
    }
    puts("Release Shader certificate job tests passed.");
    return 0;
}
