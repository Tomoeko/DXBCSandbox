#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define main dxbc_sandbox_cli_embedded_main
#endif

#include "cli/dxbc_sandbox_cli.c"

#ifndef _WIN32
#undef main
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        status = 1; \
        goto cleanup; \
    } \
} while (0)

int main(void) {
    int status = 0;
    CliOptions parsed_options;
    memset(&parsed_options, 0, sizeof(parsed_options));
    StringBuilder report;
    sb_init(&report);

    char* flat_arguments[] = {
        (char*)"dxbc-sandbox", (char*)"extract", (char*)"fixture.assets",
        (char*)"--kind", (char*)"graphics", (char*)"--all",
        (char*)"--flat-shaders", (char*)"--out", (char*)"flat-output",
    };
    bool help_requested = false;
    CHECK(parse_cli(
        (int)(sizeof(flat_arguments) / sizeof(flat_arguments[0])),
        flat_arguments, &parsed_options, &help_requested));
    CHECK(!help_requested);
    CHECK(parsed_options.command == CLI_COMMAND_EXTRACT);
    CHECK(parsed_options.shader_kind == CLI_SHADER_KIND_GRAPHICS);
    CHECK(parsed_options.all);
    CHECK(parsed_options.flat_shaders);
    CHECK(strcmp(parsed_options.output_directory, "flat-output") == 0);
    cli_options_dispose(&parsed_options);

    char *lift_arguments[] = {
        (char *)"dxbc-sandbox", (char *)"extract", (char *)"fixture.assets",
        (char *)"--all", (char *)"--out", (char *)"lift-output",
        (char *)"--high-level", (char *)"--compile-profile", (char *)"captured.profile",
        (char *)"--lift-max-compiles", (char *)"17", (char *)"--lift-timeout-ms", (char *)"1200",
    };
    CHECK(parse_cli(13, lift_arguments, &parsed_options, &help_requested) == cli_shaderlab_lift_supported());
    if (cli_shaderlab_lift_supported()) {
        CHECK(parsed_options.lift.enabled && parsed_options.lift.max_compiles == 17 &&
              parsed_options.lift.max_elapsed_ms == 1200);
        CHECK(strcmp(parsed_options.lift.profile_path, "captured.profile") == 0);
    }
    cli_options_dispose(&parsed_options);
    lift_arguments[10] = (char *)"0";
    CHECK(!parse_cli(13, lift_arguments, &parsed_options, &help_requested));
    cli_options_dispose(&parsed_options);
    lift_arguments[10] = (char *)"17";
    CHECK(!parse_cli(7, lift_arguments, &parsed_options, &help_requested)); /* Missing profile. */
    cli_options_dispose(&parsed_options);
    lift_arguments[6] = (char *)"--sources";
    CHECK(!parse_cli(13, lift_arguments, &parsed_options, &help_requested)); /* Orphan controls. */
    cli_options_dispose(&parsed_options);
    lift_arguments[6] = (char *)"--high-level";
    lift_arguments[12] = (char *)"184467440737095516160";
    CHECK(!parse_cli(13, lift_arguments, &parsed_options, &help_requested));
    cli_options_dispose(&parsed_options);

    ShaderCatalogRecord catalog_records[2];
    memset(catalog_records, 0, sizeof(catalog_records));
    memcpy(catalog_records[0].occurrence_id, "compute-id",
           sizeof("compute-id"));
    memcpy(catalog_records[0].content_id, "compute-content",
           sizeof("compute-content"));
    catalog_records[0].name = (char*)"Compute/Fixture";
    catalog_records[0].outer_path = (char*)"fixture.assets";
    catalog_records[0].class_id = 72;
    catalog_records[0].path_id = 11;
    catalog_records[0].status = SHADER_CATALOG_RECORD_READY;
    memcpy(catalog_records[1].occurrence_id, "graphics-id",
           sizeof("graphics-id"));
    memcpy(catalog_records[1].content_id, "graphics-content",
           sizeof("graphics-content"));
    catalog_records[1].name = (char*)"Graphics/Fixture";
    catalog_records[1].outer_path = (char*)"fixture.assets";
    catalog_records[1].class_id = 48;
    catalog_records[1].path_id = 12;
    catalog_records[1].status = SHADER_CATALOG_RECORD_READY;

    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    catalog.records = catalog_records;
    catalog.record_count = 2U;
    catalog.stats.shader_objects = 2U;
    catalog.stats.ready_shaders = 2U;

    ShaderBatchComputeArtifactPublication artifacts[2];
    memset(artifacts, 0, sizeof(artifacts));
    memcpy(artifacts[0].filename, "compute_Fixture__11.bin",
           sizeof("compute_Fixture__11.bin"));
    artifacts[0].preflight_attempted = true;
    artifacts[0].preflight_status = COMMON_OUTPUT_PREFLIGHT_MISSING;
    artifacts[0].publish_attempted = true;
    artifacts[0].publish_status = COMMON_OUTPUT_PUBLISH_EMITTED;
    artifacts[0].publication_residue = true;
    memcpy(artifacts[1].filename, "compute_Fixture__11.compute.json",
           sizeof("compute_Fixture__11.compute.json"));
    artifacts[1].is_manifest = true;
    artifacts[1].preflight_attempted = true;
    artifacts[1].preflight_status = COMMON_OUTPUT_PREFLIGHT_UNCHANGED;

    ShaderBatchRecordResult batch_records[2];
    memset(batch_records, 0, sizeof(batch_records));
    batch_records[0].status = SHADER_BATCH_FAILED;
    batch_records[0].failure = SHADER_BATCH_FAILURE_OUTPUT_IO;
    batch_records[0].compute_artifact_status = COMPUTE_SHADER_ARTIFACT_OK;
    batch_records[0].compute_artifact_publications = artifacts;
    batch_records[0].compute_artifact_publication_count = 2U;
    batch_records[0].compute_preflight_attempted = true;
    batch_records[0].compute_preflight_status =
        COMMON_OUTPUT_PREFLIGHT_MISSING;
    batch_records[0].compute_publish_attempted = true;
    batch_records[0].compute_publish_status = COMMON_OUTPUT_PUBLISH_IO_ERROR;
    batch_records[0].compute_publication_residue = true;
    batch_records[0].publication_residue = true;

    batch_records[1].status = SHADER_BATCH_FAILED;
    batch_records[1].failure = SHADER_BATCH_FAILURE_OUTPUT_IO;
    batch_records[1].meta_publish_attempted = true;
    batch_records[1].meta_publish_status = COMMON_OUTPUT_PUBLISH_EMITTED;
    batch_records[1].meta_publication_residue = true;
    batch_records[1].meta_publication_residue_path =
        (char*)"/deterministic/failed.shader.meta";
    batch_records[1].publication_residue = true;

    ShaderBatchResult batch;
    shader_batch_result_init(&batch);
    batch.records = batch_records;
    batch.record_count = 2U;
    batch.stats.selected = 2U;
    batch.stats.failed = 2U;
    bool selected[2] = {true, true};
    bool texture_batch_complete = false;

    CHECK(render_extract_json(
        &catalog, selected, &batch, NULL, NULL, CLI_SHADER_KIND_ALL,
        NULL, NULL, &report, &texture_batch_complete, NULL));
    CHECK(texture_batch_complete);
    CHECK(strstr(report.buf, "\"report_version\":7") != NULL);
    CHECK(strstr(
        report.buf,
        "\"artifacts\":[{\"filename\":\"compute_Fixture__11.bin\","
        "\"is_manifest\":false,\"preflight_attempted\":true,"
        "\"preflight_status\":\"missing\",\"publish_attempted\":true,"
        "\"publish_status\":\"emitted\","
        "\"publication_residue\":true}") != NULL);
    CHECK(strstr(
        report.buf,
        "{\"filename\":\"compute_Fixture__11.compute.json\","
        "\"is_manifest\":true,\"preflight_attempted\":true,"
        "\"preflight_status\":\"unchanged\","
        "\"publish_attempted\":false,\"publish_status\":null,"
        "\"publication_residue\":false}") != NULL);
    CHECK(strstr(
        report.buf,
        "\"shader_publication_residue\":false,"
        "\"meta_publication_residue\":true,"
        "\"shader_publication_residue_path\":null,"
        "\"meta_publication_residue_path\":"
        "\"/deterministic/failed.shader.meta\"") != NULL);

    /* A failed compute-only selection is an unpublished compute package at
     * both report levels; successful-package counters cannot relabel it as a
     * graphics candidate. */
    sb_free(&report);
    sb_init(&report);
    bool compute_only_selected[2] = {true, false};
    batch_records[1].status = SHADER_BATCH_UNSELECTED;
    batch_records[1].failure = SHADER_BATCH_FAILURE_NONE;
    batch.stats.selected = 1U;
    batch.stats.failed = 1U;
    CHECK(render_extract_json(
        &catalog, compute_only_selected, &batch, NULL, NULL,
        CLI_SHADER_KIND_COMPUTE, NULL, NULL, &report,
        &texture_batch_complete, NULL));
    CHECK(texture_batch_complete);
    CHECK(strstr(
        report.buf,
        "\"selection_kind\":\"compute\","
        "\"artifact_kind\":\"unpublished-compute-package\"") != NULL);

    /* A counter-only native success claim cannot make the report complete
     * when its sole record is failed and therefore unpublished. */
    sb_free(&report);
    sb_init(&report);
    NativeTextureBatchRecordResult texture_record;
    memset(&texture_record, 0, sizeof(texture_record));
    texture_record.status = NATIVE_TEXTURE_BATCH_FAILED;
    texture_record.failure = NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_IO;
    texture_record.class_id = UNITY_TEXTURE2D_CLASS_ID;
    NativeTextureBatchResult textures;
    native_texture_batch_result_init(&textures);
    textures.source_transaction_complete = true;
    textures.staging_cleanup_complete = true;
    textures.records = &texture_record;
    textures.record_count = 1U;
    textures.stats.selected = 1U;
    textures.stats.texture2d = 1U;
    textures.stats.emitted = 1U;
    CHECK(render_extract_json(
        &catalog, compute_only_selected, &batch, NULL, &textures,
        CLI_SHADER_KIND_COMPUTE, NULL, NULL, &report,
        &texture_batch_complete, NULL));
    CHECK(!texture_batch_complete);
    CHECK(strstr(
        report.buf,
        "\"native_textures\":{\"requested\":true,"
        "\"complete\":false") != NULL);
    CHECK(strstr(report.buf, "\"guid\":null") != NULL);

    /* The Material subsection is catalog-bound and preserves the complete
     * external-table evidence needed to interpret a positive serialized
     * fileID.  A value-copy catalog is not the originating authority. */
    sb_free(&report);
    sb_init(&report);
    static uint8_t property_name[] = "_MainTex";
    MaterialBatchTextureDependency dependency;
    memset(&dependency, 0, sizeof(dependency));
    dependency.property_name = property_name;
    dependency.property_name_size = sizeof(property_name) - 1U;
    dependency.serialized_file_id = 1;
    dependency.serialized_path_id = 77;
    dependency.resolve_status = UNITY_PPTR_RESOLVE_EXTERNAL_EXACT;
    dependency.status = MATERIAL_BATCH_DEPENDENCY_RESOLVED_EXPORTED;
    dependency.reference_status = MATERIAL_BATCH_TEXTURE_REFERENCE_OK;
    dependency.source_index = 0U;
    dependency.target_source_index = 0U;
    dependency.external_index = 0U;
    dependency.has_target = true;
    dependency.target_class_id = 28;
    dependency.target_path_id = 77;
    memset(dependency.target_serialized_digest_hex, 'a',
           COMMON_SHA256_DIGEST_SIZE * 2U);
    dependency.has_external_serialized_guid = true;
    dependency.external_serialized_type = 0;
    dependency.has_yaml_reference = true;
    dependency.yaml_reference.file_id = 2800000;
    memcpy(dependency.yaml_reference.guid,
           "0123456789abcdef0123456789abcdef",
           UNITY_ASSET_GUID_TEXT_CAPACITY);
    dependency.yaml_reference.type = 2;
    dependency.yaml_reference.asset_exported = true;

    ShaderCatalogMaterialRecord material_record;
    memset(&material_record, 0, sizeof(material_record));
    material_record.status = SHADER_CATALOG_MATERIAL_READY;
    material_record.object.decoded = true;
    material_record.shader_record_index = 0U;
    ShaderCatalog material_catalog;
    shader_catalog_init(&material_catalog);
    material_catalog.materials_included = true;
    material_catalog.materials = &material_record;
    material_catalog.material_count = 1U;
    material_catalog.records = catalog_records;
    material_catalog.record_count = 2U;

    MaterialBatchRecordResult material_result;
    memset(&material_result, 0, sizeof(material_result));
    material_result.status = MATERIAL_BATCH_UNCHANGED;
    material_result.shader_record_index = 0U;
    memcpy(material_result.asset_guid,
           "11111111111111111111111111111111",
           UNITY_ASSET_GUID_TEXT_CAPACITY);
    memset(material_result.artifact_identity_hex, 'b',
           COMMON_SHA256_DIGEST_SIZE * 2U);
    material_result.output_path = (char*)"fixture.mat";
    material_result.output_meta_path = (char*)"fixture.mat.meta";
    material_result.dependency_evidence_path =
        (char*)"fixture.mat.dependencies.json";
    material_result.has_artifacts = true;
    material_result.material_publish_attempted = true;
    material_result.meta_publish_attempted = true;
    material_result.evidence_publish_attempted = true;
    material_result.material_publish_status =
        COMMON_OUTPUT_PUBLISH_UNCHANGED;
    material_result.meta_publish_status = COMMON_OUTPUT_PUBLISH_UNCHANGED;
    material_result.evidence_publish_status =
        COMMON_OUTPUT_PUBLISH_UNCHANGED;
    material_result.dependencies = &dependency;
    material_result.dependency_count = 1U;
    material_result.texture_dependency_closure_complete = true;

    MaterialBatchResult materials;
    material_batch_result_init(&materials);
    bool material_selected[2] = {true, false};
    materials.catalog_authority = &material_catalog;
    materials.selection_authority = material_selected;
    materials.records = &material_result;
    materials.record_count = 1U;
    materials.stats.catalog_materials = 1U;
    materials.stats.selected = 1U;
    materials.stats.unchanged = 1U;
    materials.stats.shader_dependency_groups = 1U;
    materials.stats.texture_dependencies = 1U;
    materials.stats.resolved_texture_dependencies = 1U;
    materials.stats.exported_texture_dependencies = 1U;
    CHECK(material_batch_is_complete(&materials));
    CHECK(material_batch_texture_dependencies_are_closed(&materials));
    append_material_batch_json(
        &report, &material_catalog, material_selected, &materials);
    CHECK(strstr(report.buf,
                 "\"shader_dependency_groups\":1") != NULL);
    CHECK(strstr(report.buf,
                 "\"external_index\":1,"
                 "\"external_serialized_type\":0") != NULL);

    sb_free(&report);
    sb_init(&report);
    append_material_batch_table(&report, &material_catalog, &materials);
    CHECK(strstr(report.buf, "shader_dependency_groups=1") != NULL);

    sb_free(&report);
    sb_init(&report);
    ShaderCatalog detached_material_catalog = material_catalog;
    append_material_batch_json(
        &report, &detached_material_catalog, material_selected, &materials);
    CHECK(strstr(report.buf, "\"emission_complete\":false") != NULL);
    CHECK(strstr(
        report.buf,
        "\"texture_dependency_closure_complete\":false") != NULL);

    sb_free(&report);
    sb_init(&report);
    bool detached_selection[2] = {true, false};
    append_material_batch_json(
        &report, &material_catalog, detached_selection, &materials);
    CHECK(strstr(report.buf, "\"emission_complete\":false") != NULL);
    CHECK(strstr(
        report.buf,
        "\"texture_dependency_closure_complete\":false") != NULL);

cleanup:
    cli_options_dispose(&parsed_options);
    sb_free(&report);
    return status;
}
