// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_shaderlab_lift_capture.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #x);                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Mutate an owned decoded tree and re-project through the production parser;
 * these tests never supply a model that disagrees with its serialized values. */
static int check_dependency_mutation(ShaderObject *object, TypeTreeValue *field,
                                     const TypeTreeValue *replacement,
                                     UnityShaderDependencyStatus expected) {
    CHECK(field && replacement);
    const TypeTreeValue saved = *field;
    *field = *replacement;
    const bool parsed =
        serialized_shader_parse_with_profile(&object->shader, &object->root, object->profile);
    UnityShaderDependencyReport report;
    const UnityShaderDependencyStatus result = unity_shader_dependency_closure(object, &report);
    *field = saved;
    CHECK(serialized_shader_parse_with_profile(&object->shader, &object->root, object->profile));
    CHECK(parsed && result == expected);
    const uint8_t zero[32] = {0};
    CHECK(memcmp(report.digest, zero, sizeof(zero)) == 0);
    return 0;
}

static int check_text_dependency(ShaderObject *object, TypeTreeValue *parent, const char *path,
                                 const char *replacement, UnityShaderDependencyStatus expected) {
    TypeTreeValue *field = (TypeTreeValue *)typetree_find_path(parent, path);
    CHECK(field && field->type == VAL_TYPE_STRING);
    TypeTreeValue changed = *field;
    changed.string_val = (char *)replacement;
    changed.string_length = strlen(replacement);
    return check_dependency_mutation(object, field, &changed, expected);
}

static int check_dependency_boundaries(ShaderObject *object) {
    UnityShaderDependencyReport before, after;
    (void)unity_shader_dependency_closure(object, &before);
    CHECK(check_text_dependency(object, &object->root, "m_ParsedForm/m_FallbackName",
                                "Dependency/Other",
                                UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE) == 0);
    CHECK(check_text_dependency(object, &object->root, "m_ParsedForm/m_CustomEditorName",
                                "ExternalEditor",
                                UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE) == 0);
    /* Even byte-identical raw IDs or a null slot are not a resolved graph. */
    TypeTreeValue pointer_fields[] = {
        {.name = "m_FileID", .type_str = "int", .type = VAL_TYPE_INT, .int_val = 0},
        {.name = "m_PathID", .type_str = "SInt64", .type = VAL_TYPE_INT, .int_val = 7},
    };
    TypeTreeValue pointer = {.name = "data", .type_str = "PPtr<Shader>", .type = VAL_TYPE_STRUCT};
    pointer.struct_val.members = pointer_fields;
    pointer.struct_val.count = 2;
    TypeTreeValue *references =
        (TypeTreeValue *)typetree_find_child(&object->root, "m_Dependencies");
    CHECK(references);
    TypeTreeValue changed = *references;
    changed.array_val.elements = &pointer;
    changed.array_val.count = 1;
    CHECK(check_dependency_mutation(object, references, &changed,
                                    UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE) == 0);
    pointer_fields[1].int_val = 0;
    CHECK(check_dependency_mutation(object, references, &changed,
                                    UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE) == 0);

    TypeTreeValue *subshaders =
        (TypeTreeValue *)typetree_find_path(&object->root, "m_ParsedForm/m_SubShaders");
    CHECK(subshaders);
    if (subshaders->array_val.count) {
        TypeTreeValue *passes =
            (TypeTreeValue *)typetree_find_child(&subshaders->array_val.elements[0], "m_Passes");
        CHECK(passes && passes->array_val.count);
        TypeTreeValue *pass = &passes->array_val.elements[0];
        const char *const text_fields[] = {"m_UseName", "m_TextureName", "m_State/zTest/name"};
        for (size_t i = 0; i < sizeof(text_fields) / sizeof(text_fields[0]); ++i)
            CHECK(check_text_dependency(object, pass, text_fields[i], "ExternalInput",
                                        UNITY_SHADER_DEPENDENCIES_UNSUPPORTED_PASS) == 0);
        CHECK(check_text_dependency(object, pass, "m_State/fogColor/name", "OtherFog",
                                    UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE) == 0);
        CHECK(check_text_dependency(object, pass, "m_State/fogStart/name", "unity_FogEnd",
                                    UNITY_SHADER_DEPENDENCIES_EXTERNAL_REFERENCE) == 0);
        const char *const integer_fields[] = {"m_Type", "m_HasInstancingVariant",
                                              "m_HasProceduralInstancingVariant"};
        for (size_t i = 0; i < sizeof(integer_fields) / sizeof(integer_fields[0]); ++i) {
            TypeTreeValue *field = (TypeTreeValue *)typetree_find_child(pass, integer_fields[i]);
            CHECK(field && field->type == VAL_TYPE_INT);
            changed = *field;
            changed.int_val = 1;
            CHECK(check_dependency_mutation(object, field, &changed,
                                            UNITY_SHADER_DEPENDENCIES_UNSUPPORTED_PASS) == 0);
        }
        /* Add a real common texture binding and its name-table authority. The
         * parameter parser and common/residual reflection union must expose it. */
        TypeTreeValue name_fields[] = {
            {.name = "first",
             .type_str = "string",
             .type = VAL_TYPE_STRING,
             .string_val = "DependencyTexture",
             .string_length = 17},
            {.name = "second", .type_str = "int", .type = VAL_TYPE_INT, .int_val = 0},
        };
        TypeTreeValue name_pair = {.name = "data", .type_str = "pair", .type = VAL_TYPE_STRUCT};
        name_pair.struct_val.members = name_fields;
        name_pair.struct_val.count = 2;
        TypeTreeValue *names = (TypeTreeValue *)typetree_find_child(pass, "m_NameIndices");
        CHECK(names);
        const TypeTreeValue saved_names = *names;
        names->array_val.elements = &name_pair;
        names->array_val.count = 1;
        TypeTreeValue texture_fields[] = {
            {.name = "m_NameIndex", .type_str = "int", .type = VAL_TYPE_INT, .int_val = 0},
            {.name = "m_Index", .type_str = "int", .type = VAL_TYPE_INT, .int_val = 0},
            {.name = "m_SamplerIndex", .type_str = "int", .type = VAL_TYPE_INT, .int_val = 0},
            {.name = "m_MultiSampled", .type_str = "bool", .type = VAL_TYPE_INT, .int_val = 0},
            {.name = "m_Dim", .type_str = "SInt8", .type = VAL_TYPE_INT, .int_val = 2},
        };
        TypeTreeValue texture = {
            .name = "data", .type_str = "TextureParameter", .type = VAL_TYPE_STRUCT};
        texture.struct_val.members = texture_fields;
        texture.struct_val.count = 5;
        TypeTreeValue *textures = (TypeTreeValue *)typetree_find_path(
            pass, "progFragment/m_CommonParameters/m_TextureParams");
        CHECK(textures);
        changed = *textures;
        changed.array_val.elements = &texture;
        changed.array_val.count = 1;
        const int binding_result = check_dependency_mutation(
            object, textures, &changed, UNITY_SHADER_DEPENDENCIES_EXTERNAL_BINDING);
        *names = saved_names;
        CHECK(
            serialized_shader_parse_with_profile(&object->shader, &object->root, object->profile));
        CHECK(binding_result == 0);

        TypeTreeValue *fog = (TypeTreeValue *)typetree_find_path(pass, "m_State/fogColor/name");
        CHECK(fog && fog->type == VAL_TYPE_STRING);
        if (before.status == UNITY_SHADER_DEPENDENCIES_OK &&
            strcmp(fog->string_val, "unity_FogColor") == 0) {
            const TypeTreeValue saved = *fog;
            fog->string_val = "<noninit>";
            fog->string_length = 9;
            CHECK(serialized_shader_parse_with_profile(&object->shader, &object->root,
                                                       object->profile));
            CHECK(unity_shader_dependency_closure(object, &after) == UNITY_SHADER_DEPENDENCIES_OK);
            *fog = saved;
            CHECK(serialized_shader_parse_with_profile(&object->shader, &object->root,
                                                       object->profile));
            CHECK(after.engine_fog_input_count + 1 == before.engine_fog_input_count);
            CHECK(memcmp(before.digest, after.digest, 32) != 0);
        }
    }
    CHECK(unity_shader_dependency_closure(object, &after) == before.status);
    CHECK(memcmp(before.digest, after.digest, sizeof(before.digest)) == 0);
    return 0;
}

static int check_evidence(const UnityShaderLabLiftCapture *capture, const ShaderCatalog *catalog,
                          const TypeTreeSchemaRegistry *registry,
                          const UnityShaderLabLiftCaptureReport *capture_report,
                          WholeShaderPlaneStatus expected_structure) {
    WholeShaderSubjectDescriptor descriptor;
    CHECK(unity_shaderlab_lift_capture_subject(capture, &descriptor));
    ShaderObject candidate;
    shader_object_init(&candidate);
    ShaderCatalogObjectReport candidate_report;
    CHECK(shader_catalog_decode_object(catalog, catalog->records, registry, &candidate,
                                       &candidate_report) == SHADER_CATALOG_OBJECT_OK);
    CHECK(check_dependency_boundaries(&candidate) == 0);
    shader_object_dispose(&candidate);
    memcpy(descriptor.candidate_release_digest, candidate_report.release_digest, 32);
    CHECK(capture_report->structural_digest_valid);
    WholeShaderSubject *subject = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
    WholeShaderEvidence *dxbc = NULL, *bindings = NULL, *domain = NULL, *diagnostics = NULL,
                        *invalid = NULL;
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject, WHOLE_SHADER_PLANE_FULL_DXBC,
                                                     &dxbc) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject,
                                                     WHOLE_SHADER_PLANE_REFLECTION_BINDING,
                                                     &bindings) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject,
                                                     WHOLE_SHADER_PLANE_VARIANT_DOMAIN,
                                                     &domain) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(unity_shaderlab_lift_capture_make_evidence(capture, subject,
                                                     WHOLE_SHADER_PLANE_COMPILER_DIAGNOSTICS,
                                                     &diagnostics) == WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary a, b, d, diagnostic_summary;
    CHECK(whole_shader_evidence_describe(diagnostics, &diagnostic_summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(diagnostic_summary.status == WHOLE_SHADER_PLANE_PASS);
    CHECK(whole_shader_evidence_describe(domain, &d) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(d.status == WHOLE_SHADER_PLANE_PASS);
    CHECK(whole_shader_evidence_describe(dxbc, &a) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(whole_shader_evidence_describe(bindings, &b) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(a.status == WHOLE_SHADER_PLANE_PASS && b.status == WHOLE_SHADER_PLANE_PASS);
    CHECK(a.expected_item_count > 0 && a.expected_item_count == b.expected_item_count);
    CHECK(memcmp(a.coverage_digest, b.coverage_digest, 32) == 0);
    CHECK(memcmp(a.coverage_digest, d.coverage_digest, 32) == 0);
    CHECK(memcmp(a.coverage_digest, diagnostic_summary.coverage_digest, 32) == 0);
    CHECK(unity_shaderlab_lift_capture_make_evidence(
              capture, subject, WHOLE_SHADER_PLANE_RUNTIME_SELECTION, &invalid) ==
          WHOLE_SHADER_EVIDENCE_INVALID_PLANE);
    CHECK(!invalid);
    printf("domain=pass dxbc=pass diagnostics=pass bindings=pass compile_items=%llu\n",
           (unsigned long long)a.expected_item_count);
    for (size_t mutation = 0; mutation < 16; ++mutation) {
        WholeShaderSubjectDescriptor changed = descriptor;
        switch (mutation) {
        case 0:
            changed.target_shader_path_id++;
            break;
        case 1:
            changed.target_occurrence_digest[0] ^= 1;
            break;
        case 2:
            changed.target_serialized_file_digest[0] ^= 1;
            break;
        case 3:
            changed.target_object_payload_digest[0] ^= 1;
            break;
        case 4:
            changed.schema_authority_digest[0] ^= 1;
            break;
        case 5:
            changed.candidate_source_digest[0] ^= 1;
            break;
        case 6:
            changed.compiler_profile_digest[0] ^= 1;
            break;
        case 7:
            changed.compiler_session_digest[0] ^= 1;
            break;
        case 8:
            changed.verification_scope_digest[0] ^= 1;
            break;
        case 9:
            changed.build_platform++;
            break;
        case 10:
            changed.compiler_platform++;
            break;
        case 11:
            changed.graphics_api++;
            break;
        case 12:
            changed.candidate_logical_name = "Different/Shader";
            break;
        case 13:
            changed.unity_version = "different";
            break;
        case 14:
            changed.serialized_target_platform++;
            break;
        case 15:
            changed.dependency_map_digest[0] ^= 1;
            break;
        }
        WholeShaderSubject *mismatch = NULL;
        CHECK(whole_shader_subject_create(&mismatch, &changed) == WHOLE_SHADER_SUBJECT_OK);
        CHECK(unity_shaderlab_lift_capture_make_evidence(capture, mismatch,
                                                         WHOLE_SHADER_PLANE_FULL_DXBC, &invalid) ==
              WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
        CHECK(!invalid);
        whole_shader_subject_free(mismatch);
    }
    WholeShaderEvidence *structure = NULL;
    UnityShaderLabStructuralEvidenceReport structure_report;
    CHECK(unity_shaderlab_lift_capture_structural_evidence(
              capture, catalog, catalog->records, registry, subject, &structure,
              &structure_report) == WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary structural_summary;
    CHECK(whole_shader_evidence_describe(structure, &structural_summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(structural_summary.status == expected_structure);
    if (expected_structure == WHOLE_SHADER_PLANE_PASS)
        CHECK(structural_summary.matched_item_count == 3);
    if (expected_structure == WHOLE_SHADER_PLANE_UNAVAILABLE) {
        CHECK(!structure_report.emitted && !structural_summary.complete);
    } else {
        CHECK(structure_report.emission_attempted && structure_report.emitted);
    }
    CHECK(structure_report.target_structure.status == SHADERLAB_STRUCTURE_OK &&
          structure_report.candidate_structure.status == SHADERLAB_STRUCTURE_OK);
    printf("structure=%s covered=%zu/%zu matched=%llu/3\n",
           whole_shader_plane_status_name(structural_summary.status),
           structure_report.target_structure.covered_semantic_fields,
           structure_report.candidate_structure.covered_semantic_fields,
           (unsigned long long)structural_summary.matched_item_count);
    whole_shader_evidence_free(structure);
    structure = NULL;
    WholeShaderSubjectDescriptor wrong_release = descriptor;
    wrong_release.candidate_release_digest[0] ^= 1;
    WholeShaderSubject *wrong = NULL;
    CHECK(whole_shader_subject_create(&wrong, &wrong_release) == WHOLE_SHADER_SUBJECT_OK);
    CHECK(unity_shaderlab_lift_capture_structural_evidence(
              capture, catalog, catalog->records, registry, wrong, &structure, &structure_report) ==
          WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!structure && !structure_report.emission_attempted);
    whole_shader_subject_free(wrong);

    WholeShaderEvidence *dependencies = NULL;
    UnityShaderDependencyEvidenceReport dependency_report;
    printf("target_dependencies=%s pass=%d stage=%d program=%d\n",
           unity_shader_dependency_status_name(capture_report->dependencies.status),
           capture_report->dependencies.pass_index, capture_report->dependencies.stage_index,
           capture_report->dependencies.subprogram_index);
    CHECK(capture_report->dependencies.status == UNITY_SHADER_DEPENDENCIES_OK);
    CHECK(unity_shaderlab_lift_capture_dependency_evidence(
              capture, catalog, catalog->records, registry, subject, &dependencies,
              &dependency_report) == WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary dependency_summary;
    CHECK(whole_shader_evidence_describe(dependencies, &dependency_summary) ==
          WHOLE_SHADER_EVIDENCE_OK);
    CHECK(dependency_summary.status == (expected_structure == WHOLE_SHADER_PLANE_UNAVAILABLE
                                            ? WHOLE_SHADER_PLANE_UNAVAILABLE
                                            : WHOLE_SHADER_PLANE_PASS));
    printf("dependencies=%s programs=%zu/%zu\n",
           whole_shader_plane_status_name(dependency_summary.status),
           dependency_report.target_dependencies.subprogram_count,
           dependency_report.candidate_dependencies.subprogram_count);
    whole_shader_evidence_free(dependencies);
    dependencies = NULL;
    CHECK(whole_shader_subject_create(&wrong, &wrong_release) == WHOLE_SHADER_SUBJECT_OK);
    CHECK(unity_shaderlab_lift_capture_dependency_evidence(
              capture, catalog, catalog->records, registry, wrong, &dependencies,
              &dependency_report) == WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!dependencies);
    whole_shader_subject_free(wrong);

    /* These compiler planes alone cannot certify whole-shader equivalence. */
    WholeShaderCertificateInput *certificate = NULL;
    CHECK(whole_shader_certificate_input_create(&certificate, subject,
                                                WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(certificate, dxbc) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    CHECK(whole_shader_certificate_input_add_evidence(certificate, bindings) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    WholeShaderCertificateReport report;
    (void)whole_shader_certificate_evaluate(certificate, &report);
    CHECK(!report.d3d11_byte_equivalence_certified && !report.d3d11_logical_equivalence_certified);
    whole_shader_certificate_input_free(certificate);
    whole_shader_evidence_free(diagnostics);
    whole_shader_evidence_free(domain);
    whole_shader_evidence_free(dxbc);
    whole_shader_evidence_free(bindings);
    whole_shader_subject_free(subject);
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 5 || argc == 7);
    CHECK(argc != 7 || strcmp(argv[6], "pass") == 0 || strcmp(argv[6], "fail") == 0 ||
          strcmp(argv[6], "unavailable") == 0);
    UnityShaderLabLiftCapture *capture = NULL;
    UnityShaderLabLiftCaptureReport report;
    CHECK(unity_shaderlab_lift_capture(NULL, &capture, &report) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    CHECK(!capture && !unity_shaderlab_lift_capture_result(NULL));
    unity_shaderlab_lift_capture_free(NULL);
    UnityShaderDependencyReport missing;
    CHECK(unity_shader_dependency_closure(NULL, &missing) ==
          UNITY_SHADER_DEPENDENCIES_INVALID_ARGUMENT);
    CHECK(unity_shader_dependency_closure(NULL, NULL) ==
          UNITY_SHADER_DEPENDENCIES_INVALID_ARGUMENT);
    WholeShaderEvidence *absent = NULL;
    UnityShaderLabStructuralEvidenceReport absent_report;
    CHECK(unity_shaderlab_lift_capture_structural_evidence(NULL, NULL, NULL, NULL, NULL, &absent,
                                                           &absent_report) ==
          WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!absent && !absent_report.emission_attempted);

    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    CHECK(typetree_schema_registry_import_file_replace(&registry, CAPTURE_REGISTRY) ==
          TYPETREE_SCHEMA_OK);
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = &registry;
    options.retain_source_snapshots = true;
    const char *path = argc >= 5 ? argv[1] : CAPTURE_EMPTY_FIXTURE;
    CHECK(shader_catalog_build(&path, 1, &options, &catalog) == SHADER_CATALOG_OK);
    CHECK(catalog.record_count == 1 && shader_catalog_is_complete(&catalog));
    ShaderObject boundary_object;
    shader_object_init(&boundary_object);
    ShaderCatalogObjectReport boundary_report;
    CHECK(shader_catalog_decode_object(&catalog, catalog.records, &registry, &boundary_object,
                                       &boundary_report) == SHADER_CATALOG_OBJECT_OK);
    CHECK(check_dependency_boundaries(&boundary_object) == 0);
    shader_object_dispose(&boundary_object);
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19;
    profile.valid_apis = 295472;
    strcpy(profile.provenance, "synthetic-unit-test");
    if (argc >= 5)
        CHECK(unity_compile_profile_load(argv[2], &profile) == UNITY_COMPILE_PROFILE_OK);
    UnityCompilerBroker *broker =
        unity_compiler_broker_create_lazy(argc >= 5 ? argv[3] : ".", argc >= 5 ? argv[4] : NULL);
    CHECK(broker);
    HLSLLiftLimits limits = {2, 128, 60000};
    UnityShaderLabLiftCaptureInput input = {
        .catalog = &catalog,
        .record = catalog.records,
        .registry = &registry,
        .profile = &profile,
        .broker = broker,
        .source_path = "Assets/Captured.shader",
        .source_directory = argc >= 5 ? argv[3] : ".",
        .source_basename = "Captured.shader",
        .limits = &limits,
    };
    CHECK(unity_shaderlab_lift_capture(&input, NULL, &report) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    CHECK(unity_shaderlab_lift_capture(&input, &capture, NULL) ==
          UNITY_SHADERLAB_CAPTURE_INVALID_ARGUMENT);
    ShaderCatalogRecord unowned = *catalog.records;
    input.record = &unowned;
    CHECK(unity_shaderlab_lift_capture(&input, &capture, &report) ==
          UNITY_SHADERLAB_CAPTURE_SOURCE_UNAVAILABLE);
    CHECK(report.source_status == SHADER_CATALOG_OBJECT_RECORD_NOT_OWNED && !capture);
    input.record = catalog.records;
    UnityShaderLabLiftCaptureStatus status =
        unity_shaderlab_lift_capture(&input, &capture, &report);
    printf("capture=%d lift=%d archive=%d\n", status, report.lift_status, report.archive_status);
    if (argc == 1) {
        CHECK(status == UNITY_SHADERLAB_CAPTURE_ARCHIVE_UNAVAILABLE && !capture);
        CHECK(report.archive_status == SHADER_OBJECT_D3D11_PLATFORM_ABSENT);
        UnityCompilerBrokerStats stats;
        unity_compiler_broker_get_stats(broker, &stats);
        CHECK(stats.compiler_process_starts == 0);
    } else {
        CHECK(status == UNITY_SHADERLAB_CAPTURE_OK && capture);
        const UnityShaderLabLiftArtifact *accepted =
            unity_shaderlab_lift_accepted(unity_shaderlab_lift_capture_result(capture));
        CHECK(accepted && accepted->high_level && accepted->certified_pass_count > 0);
        uint8_t digest[32];
        common_sha256(accepted->source.buf, accepted->source.len, digest);
        CHECK(memcmp(digest, report.accepted_source_digest, 32) == 0);
        CHECK(unity_compile_profile_fingerprint(&profile, digest) == UNITY_COMPILE_PROFILE_OK);
        CHECK(memcmp(digest, report.profile_digest, 32) == 0);
        ShaderCatalog released;
        shader_catalog_init(&released);
        const char *released_path = argc == 7 ? argv[5] : path;
        CHECK(shader_catalog_build(&released_path, 1, &options, &released) == SHADER_CATALOG_OK);
        CHECK(released.record_count == 1 && shader_catalog_is_complete(&released));
        WholeShaderPlaneStatus expected_structure = WHOLE_SHADER_PLANE_PASS;
        if (argc == 7 && strcmp(argv[6], "fail") == 0)
            expected_structure = WHOLE_SHADER_PLANE_FAIL;
        if (argc == 7 && strcmp(argv[6], "unavailable") == 0)
            expected_structure = WHOLE_SHADER_PLANE_UNAVAILABLE;
        CHECK(check_evidence(capture, &released, &registry, &report, expected_structure) == 0);
        shader_catalog_dispose(&released);
        printf("high_level=%d helper=%d passes=%zu\n", accepted->high_level,
               accepted->unity_uv_helpers, accepted->certified_pass_count);
    }
    unity_shaderlab_lift_capture_free(capture);
    unity_compiler_broker_destroy(broker);
    shader_catalog_dispose(&catalog);
    typetree_schema_registry_dispose(&registry);
    return 0;
}
