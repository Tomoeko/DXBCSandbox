if(BUILD_TESTING)
    set(DXBCSANDBOX_REGRESSION_CORPUS ""
        CACHE FILEPATH
        "Exact pinned Unity Shader bundle used by regression-corpus tests")

    set(_dxbc_has_regression_corpus OFF)
    if(NOT "${DXBCSANDBOX_REGRESSION_CORPUS}" STREQUAL "")
        get_filename_component(_dxbc_regression_corpus
            "${DXBCSANDBOX_REGRESSION_CORPUS}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_dxbc_regression_corpus}" OR
           IS_DIRECTORY "${_dxbc_regression_corpus}")
            message(FATAL_ERROR
                "DXBCSANDBOX_REGRESSION_CORPUS must name the pinned "
                "regression bundle file: "
                "${_dxbc_regression_corpus}")
        endif()
        set(_dxbc_has_regression_corpus ON)
        message(STATUS
            "DXBCSandbox: regression corpus=${_dxbc_regression_corpus}")
    else()
        message(STATUS
            "DXBCSandbox: regression-corpus tests disabled; set "
            "DXBCSANDBOX_REGRESSION_CORPUS to the exact pinned fixture "
            "to enable them")
    endif()

    # Manual, argument-driven inspection utilities. They are portable but are
    # intentionally not registered with CTest because they need corpus paths.
    add_executable(test_parser tests/test_parser.c)
    target_link_libraries(test_parser PRIVATE dxbc_core dxbc_build_options)

    add_executable(test_roundtrip tests/test_roundtrip.c)
    target_link_libraries(test_roundtrip PRIVATE dxbc_core dxbc_build_options)

    add_executable(test_unity_include_scan_units
        tests/test_unity_include_scan_units.c src/compiler/unity_include_scan.c)
    target_include_directories(test_unity_include_scan_units PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_unity_include_scan_units PRIVATE dxbc_build_options)
    add_test(NAME unity_include_scan_units COMMAND test_unity_include_scan_units)

    add_executable(test_bundle_corpus tests/test_bundle_corpus.c)
    target_link_libraries(test_bundle_corpus PRIVATE
        dxbc_core dxbc_build_options)

    function(dxbc_add_core_test target source test_name)
        add_executable(${target} ${source})
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(${target} PRIVATE dxbc_core dxbc_build_options)
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    dxbc_add_core_test(test_shader_common_units
        tests/test_shader_common_units.c shader_common_units)
    dxbc_add_core_test(test_executable_resource_integration
        tests/test_executable_resource_integration.c
        executable_resource_integration)
    set(_dxbc_resource_test_bin
        "${CMAKE_CURRENT_BINARY_DIR}/resource-layout/bin")
    set_target_properties(test_executable_resource_integration PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_dxbc_resource_test_bin}")
    foreach(_dxbc_config ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${_dxbc_config}" _dxbc_config_upper)
        set_target_properties(test_executable_resource_integration PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${_dxbc_config_upper}"
            "${_dxbc_resource_test_bin}")
    endforeach()
    set(_dxbc_resource_test_share
        "${CMAKE_CURRENT_BINARY_DIR}/resource-layout/share/dxbc-sandbox/unity/Editor")
    file(MAKE_DIRECTORY "${_dxbc_resource_test_share}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/unity/Editor/DXBCShaderImportGate.cs"
        "${_dxbc_resource_test_share}/DXBCShaderImportGate.cs" COPYONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/unity/Editor/DXBCShaderBundleGate.cs"
        "${_dxbc_resource_test_share}/DXBCShaderBundleGate.cs" COPYONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/unity/Editor/DXBCFiniteVisualGate.cs"
        "${_dxbc_resource_test_share}/DXBCFiniteVisualGate.cs" COPYONLY)

    dxbc_add_core_test(test_unity_player_shader_caps_units
        tests/test_unity_player_shader_caps_units.c
        unity_player_shader_caps_units)
    dxbc_add_core_test(test_compute_shader_object_units
        tests/test_compute_shader_object_units.c compute_shader_object_units)
    dxbc_add_core_test(test_material_object_units
        tests/test_material_object_units.c material_object_units)
    target_compile_definitions(test_material_object_units PRIVATE
        MATERIAL_OBJECT_TEST_SCHEMA_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
    dxbc_add_core_test(test_material_yaml_units
        tests/test_material_yaml_units.c material_yaml_units)
    dxbc_add_core_test(test_unity_texture_object_units
        tests/test_unity_texture_object_units.c unity_texture_object_units)
    if(_dxbc_has_regression_corpus)
        dxbc_add_core_test(test_shader_catalog_batch_units
            tests/test_shader_catalog_batch_units.c
            shader_catalog_batch_units)
        target_link_libraries(test_shader_catalog_batch_units PRIVATE
            UnityCommon::test_support)
        target_compile_definitions(test_shader_catalog_batch_units PRIVATE
            DXBC_TEST_PLAYER_SCHEMA_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
            DXBC_TEST_SHADER_BUNDLE="${_dxbc_regression_corpus}")
        dxbc_add_core_test(test_shader_batch_meta_units
            tests/test_shader_batch_meta_units.c shader_batch_meta_units)
        target_compile_definitions(test_shader_batch_meta_units PRIVATE
            DXBC_TEST_PLAYER_SCHEMA_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
            DXBC_TEST_SHADER_BUNDLE="${_dxbc_regression_corpus}")
        dxbc_add_core_test(test_material_catalog_batch_units
            tests/test_material_catalog_batch_units.c
            material_catalog_batch_units)
        target_compile_definitions(test_material_catalog_batch_units PRIVATE
            DXBC_TEST_PLAYER_SCHEMA_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
            DXBC_TEST_SHADER_BUNDLE="${_dxbc_regression_corpus}")
    endif()
    dxbc_add_core_test(test_verification_scope_units
        tests/test_verification_scope_units.c verification_scope_units)
    dxbc_add_core_test(test_whole_shader_certificate_units
        tests/test_whole_shader_certificate_units.c
        whole_shader_certificate_units)
    dxbc_add_core_test(test_whole_shader_subject_evidence_units
        tests/test_whole_shader_subject_evidence_units.c
        whole_shader_subject_evidence_units)
    dxbc_add_core_test(test_release_shader_evidence_units
        tests/test_release_shader_evidence_units.c release_shader_evidence_units)
    target_link_libraries(test_release_shader_evidence_units PRIVATE UnityCommon::test_support)
    target_compile_definitions(test_release_shader_evidence_units PRIVATE
        DXBC_RELEASE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/release_shader/empty.assets"
        DXBC_RELEASE_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
    if(_dxbc_has_regression_corpus)
        dxbc_add_core_test(test_release_shader_object_certificate_units
            tests/test_release_shader_object_certificate_units.c
            release_shader_object_certificate_units)
        target_compile_definitions(
            test_release_shader_object_certificate_units PRIVATE
            DXBC_TEST_PLAYER_SCHEMA_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
            DXBC_TEST_SHADER_BUNDLE="${_dxbc_regression_corpus}")
        dxbc_add_core_test(test_release_shader_certificate_job_units
            tests/test_release_shader_certificate_job_units.c
            release_shader_certificate_job_units)
        target_compile_definitions(
            test_release_shader_certificate_job_units PRIVATE
            DXBC_TEST_PLAYER_SCHEMA_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
            DXBC_TEST_SHADER_BUNDLE="${_dxbc_regression_corpus}")
    endif()

    if(DXBCSANDBOX_BUILD_ASSET_CLI)
        add_executable(test_cli_extract_report_units
            tests/test_cli_extract_report_units.c src/cli/shaderlab_lift_cli.c)
        target_include_directories(test_cli_extract_report_units PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(test_cli_extract_report_units PRIVATE
            ${_dxbc_asset_cli_library} dxbc_build_options)
        add_test(NAME cli_extract_report_units COMMAND test_cli_extract_report_units)
        if(DXBCSANDBOX_BUILD_UNITY_COMPILER)
            target_compile_definitions(test_cli_extract_report_units PRIVATE DXBCSANDBOX_CLI_UNITY_COMPILER=1)
        endif()
        add_test(NAME shader_cli_help
            COMMAND dxbc_sandbox_cli --help)
        add_test(NAME player_build_settings_cli_help
            COMMAND player_build_settings_cli --help)
        if(_dxbc_has_regression_corpus)
            add_test(NAME shader_cli_list_one
                COMMAND dxbc_sandbox_cli list
                    "${_dxbc_regression_corpus}"
                    --index 1 --format json --schema-registry
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
            add_test(NAME shader_cli_list_table_concise
                COMMAND dxbc_sandbox_cli list
                    "${_dxbc_regression_corpus}"
                    --index 1 --format table --schema-registry
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
            set_tests_properties(shader_cli_list_table_concise PROPERTIES
                FAIL_REGULAR_EXPRESSION "Serialized sources:")
            add_test(NAME shader_cli_list_table_sources
                COMMAND dxbc_sandbox_cli list
                    "${_dxbc_regression_corpus}"
                    --index 1 --format table --sources --schema-registry
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
            set_tests_properties(shader_cli_list_table_sources PROPERTIES
                PASS_REGULAR_EXPRESSION "Serialized sources:")
            add_test(NAME shader_cli_list_graphics_kind
                COMMAND dxbc_sandbox_cli list
                    "${_dxbc_regression_corpus}"
                    --index 1 --kind graphics --format json --schema-registry
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
            set_tests_properties(shader_cli_list_graphics_kind PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "\"selection_kind\":\"graphics\".*\"kind\":\"graphics\"")
            add_test(NAME shader_cli_rejects_invalid_kind
                COMMAND dxbc_sandbox_cli list
                    "${_dxbc_regression_corpus}"
                    --kind texture --schema-registry
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
            set_tests_properties(shader_cli_rejects_invalid_kind PROPERTIES
                WILL_FAIL TRUE)
            add_test(NAME shader_cli_rejects_list_output
                COMMAND dxbc_sandbox_cli list
                    "${_dxbc_regression_corpus}"
                    --out ignored --schema-registry
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
            set_tests_properties(shader_cli_rejects_list_output PROPERTIES
                WILL_FAIL TRUE)
        endif()
        add_test(NAME shader_cli_rejects_unrelated_extract
            COMMAND dxbc_sandbox_cli extract
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
                --all --out dxbc_cli_should_remain_empty
                --format json --schema-registry
                "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
        set_tests_properties(shader_cli_rejects_unrelated_extract PROPERTIES
            WILL_FAIL TRUE)
        if(_dxbc_has_regression_corpus)
            add_test(NAME shader_cli_rejects_malformed_id
                COMMAND dxbc_sandbox_cli list
                    "${_dxbc_regression_corpus}"
                    --id not-hex:1 --schema-registry
                    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry")
            set_tests_properties(shader_cli_rejects_malformed_id PROPERTIES
                WILL_FAIL TRUE)
        endif()
        add_test(NAME release_shader_certificate_cli_help
            COMMAND release_shader_certificate_cli --help)
        add_test(NAME release_shader_certificate_cli_rejects_invalid_format
            COMMAND release_shader_certificate_cli compare
                --expected expected.bundle --actual actual.bundle
                --pairs pairs.tsv --schema-registry registry.tsv
                --format yaml --report report.json)
        set_tests_properties(
            release_shader_certificate_cli_rejects_invalid_format PROPERTIES
            WILL_FAIL TRUE)
    endif()
    dxbc_add_core_test(test_dxbc_units
        tests/test_dxbc_units.c dxbc_units)
    target_sources(test_dxbc_units PRIVATE tests/test_fixture.c)
    target_include_directories(test_dxbc_units PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(test_dxbc_units PRIVATE
        DXBC_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/unlit_color/target.bin"
        DXBC_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        DXBC_PREVIEW3D_SLICED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/preview3d_sliced_ps.dxbc.b64")
    dxbc_add_core_test(test_hlsl_dataflow_units
        tests/test_hlsl_dataflow_units.c hlsl_dataflow_units)
    dxbc_add_core_test(test_hlsl_ast_units
        tests/test_hlsl_ast_units.c hlsl_ast_units)
    dxbc_add_core_test(test_compiler_model_units
        tests/test_compiler_model_units.c compiler_model_units)
    dxbc_add_core_test(test_cbuffer_projection_units
        tests/test_cbuffer_projection_units.c cbuffer_projection_units)
    dxbc_add_core_test(test_usil_validation_units
        tests/test_usil_validation_units.c usil_validation_units)
    dxbc_add_core_test(test_cbuffer_emission_units
        tests/test_cbuffer_emission_units.c cbuffer_emission_units)
    dxbc_add_core_test(test_hlsl_diagnostic_units
        tests/test_hlsl_diagnostic_units.c hlsl_diagnostic_units)
    dxbc_add_core_test(test_shaderlab_state_units
        tests/test_shaderlab_state_units.c shaderlab_state_units)
    if(_dxbc_has_regression_corpus)
        dxbc_add_core_test(test_shaderlab_structural_certificate_units
            tests/test_shaderlab_structural_certificate_units.c
            shaderlab_structural_certificate_units)
        target_compile_definitions(
            test_shaderlab_structural_certificate_units PRIVATE
            DXBC_TEST_PLAYER_SCHEMA_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
            DXBC_TEST_SHADER_BUNDLE="${_dxbc_regression_corpus}")
    endif()
    dxbc_add_core_test(test_shaderlab_stage_units
        tests/test_shaderlab_stage_units.c shaderlab_stage_units)
    target_sources(test_shaderlab_stage_units PRIVATE tests/test_shaderlab_fixture.c)
    target_compile_definitions(test_shaderlab_stage_units PRIVATE
        SHADERLAB_STAGE_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/unlit_color/target.bin"
        SHADERLAB_EXPRESSION_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/expression_shaderlab/target.bin"
        SHADERLAB_CONDITIONAL_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/conditional_shaderlab/target.bin"
        SHADERLAB_LOOP_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/counted_loop_shaderlab/target.bin"
        SHADERLAB_FUNCTION_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/function_shaderlab/target.bin"
        SHADERLAB_UNITY_UV_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unity_uv_shaderlab/target.bin")
    dxbc_add_core_test(test_shaderlab_variant_domain_units
        tests/test_shaderlab_variant_domain_units.c
        shaderlab_variant_domain_units)
    dxbc_add_core_test(test_shaderlab_variant_plan_units
        tests/test_shaderlab_variant_plan_units.c
        shaderlab_variant_plan_units)
    dxbc_add_core_test(test_shaderlab_source_units
        tests/test_shaderlab_source_units.c shaderlab_source_units)
    dxbc_add_core_test(test_dxbc_document_units
        tests/test_dxbc_document_units.c dxbc_document_units)
    target_compile_definitions(test_dxbc_document_units PRIVATE
        DXBC_DOCUMENT_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/unlit_color/target.bin")
    dxbc_add_core_test(test_dxbc_compare_units
        tests/test_dxbc_compare_units.c dxbc_compare_units)
    dxbc_add_core_test(test_dxbc_stage_contract_units
        tests/test_dxbc_stage_contract_units.c dxbc_stage_contract_units)
    dxbc_add_core_test(test_geometry_usil_units
        tests/test_geometry_usil_units.c geometry_usil_units)
    target_sources(test_geometry_usil_units PRIVATE tests/test_fixture.c)
    target_compile_definitions(test_geometry_usil_units PRIVATE
        DXBC_WIREFRAME_GS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/spatial_mapping_wireframe_gs.dxbc.b64"
        DXBC_LIMIT_TEST2_GS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/limit_test2_extrusion_gs.dxbc.b64")
    dxbc_add_core_test(test_tessellation_usil_units
        tests/test_tessellation_usil_units.c tessellation_usil_units)
    target_compile_definitions(test_tessellation_usil_units PRIVATE
        DXBC_LIMIT_TEST2_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/limit_test2/target.bin")
    dxbc_add_core_test(test_precision_collision_units
        tests/test_precision_collision_units.c precision_collision_units)
    target_sources(test_precision_collision_units PRIVATE tests/test_fixture.c)
    target_include_directories(test_precision_collision_units PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(test_precision_collision_units PRIVATE
        DXBC_PRECISION_HALF_D3D_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/precision_half_d3d_fragment.b64"
        DXBC_PRECISION_FLOAT_D3D_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/precision_float_d3d_fragment.b64"
        DXBC_PRECISION_HALF_GL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/precision_half_glcore_linked.b64"
        DXBC_PRECISION_FLOAT_GL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/precision_float_glcore_linked.b64")
    dxbc_add_core_test(test_usbd_units
        tests/test_usbd_units.c usbd_units)
    target_compile_definitions(test_usbd_units PRIVATE
        DXBC_USBD_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/unlit_color/target.bin")
    dxbc_add_core_test(test_metadata_units
        tests/test_metadata_units.c metadata_units)
    dxbc_add_core_test(test_serialized_glcore_target_units
        tests/test_serialized_glcore_target_units.c
        serialized_glcore_target_units)
    dxbc_add_core_test(test_variant_key_units
        tests/test_variant_key_units.c variant_key_units)
    dxbc_add_core_test(test_oracle_pack_units
        tests/test_oracle_pack_units.c oracle_pack_units)
    dxbc_add_core_test(test_oracle_metadata_units
        tests/test_oracle_metadata_units.c oracle_metadata_units)
    if(_dxbc_has_regression_corpus)
        dxbc_add_core_test(test_shader_schema_profile
            tests/test_shader_schema_profile.c shader_schema_profile)
        target_compile_definitions(test_shader_schema_profile PRIVATE
            SHADER_SCHEMA_PROFILE_REGISTRY="${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3.35f1-shader.registry"
            SHADER_SCHEMA_PROFILE_BUNDLE="${_dxbc_regression_corpus}")
    endif()

    add_executable(test_compile_profile_units
        tests/test_compile_profile_units.c)
    target_link_libraries(test_compile_profile_units PRIVATE
        dxbc_compile_profile dxbc_build_options)
    add_test(NAME compile_profile_units COMMAND test_compile_profile_units)
    add_executable(test_unity_player_profile_units tests/test_unity_player_profile_units.c)
    target_link_libraries(test_unity_player_profile_units PRIVATE
        dxbc_compile_profile dxbc_build_options)
    target_compile_definitions(test_unity_player_profile_units PRIVATE
        DXBC_TEST_PLAYER_PROFILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/player_profile/metadata.assets")
    add_test(NAME unity_player_profile_units COMMAND test_unity_player_profile_units)

    if(DXBCSANDBOX_BUILD_UNITY_IMPORT_GATE)
        add_executable(test_unity_shader_import_gate_units
            tests/unity/test_unity_shader_import_gate_units.c)
        target_link_libraries(test_unity_shader_import_gate_units PRIVATE
            unity_shader_import_gate dxbc_build_options)
        add_test(NAME unity_shader_import_gate_units
            COMMAND test_unity_shader_import_gate_units)
        add_test(NAME unity_shader_import_gate_help
            COMMAND unity_shader_import_gate_cli --help)
    endif()

    if(DXBCSANDBOX_BUILD_UNITY_BUNDLE_GATE)
        add_executable(test_unity_shader_bundle_gate_units
            tests/unity/test_unity_shader_bundle_gate_units.c)
        target_link_libraries(test_unity_shader_bundle_gate_units PRIVATE
            unity_shader_bundle_gate dxbc_build_options)
        add_test(NAME unity_shader_bundle_gate_units
            COMMAND test_unity_shader_bundle_gate_units)
        add_test(NAME unity_shader_bundle_gate_help
            COMMAND unity_shader_bundle_gate_cli --help)
    endif()

    if(DXBCSANDBOX_BUILD_UNITY_FINITE_VISUAL_GATE)
        add_executable(fake_finite_visual_unity
            tests/unity/fake_finite_visual_unity.c)
        target_link_libraries(fake_finite_visual_unity PRIVATE
            dxbc_core dxbc_build_options)
        dxbc_enable_utf8_command_line(fake_finite_visual_unity)

        add_executable(test_unity_finite_visual_gate_units
            tests/unity/test_unity_finite_visual_gate_units.c)
        target_link_libraries(test_unity_finite_visual_gate_units PRIVATE
            unity_finite_visual_gate dxbc_build_options)
        target_compile_definitions(test_unity_finite_visual_gate_units PRIVATE
            DXBC_TEST_FINITE_VISUAL_BRIDGE="${CMAKE_CURRENT_SOURCE_DIR}/resources/unity/Editor/DXBCFiniteVisualGate.cs"
            DXBC_TEST_FINITE_VISUAL_SHADER="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/unlit_color/source.shader"
            DXBC_TEST_FINITE_VISUAL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/resources/unity/fixtures/finite_visual_propertyless.tsv")
        add_dependencies(test_unity_finite_visual_gate_units
            fake_finite_visual_unity)
        set(_dxbc_finite_visual_unit_work
            "${CMAKE_CURRENT_BINARY_DIR}/finite-visual-unit-work")
        file(MAKE_DIRECTORY "${_dxbc_finite_visual_unit_work}")
        add_test(NAME unity_finite_visual_gate_units
            COMMAND test_unity_finite_visual_gate_units
                $<TARGET_FILE:fake_finite_visual_unity>)
        set_tests_properties(unity_finite_visual_gate_units PROPERTIES
            WORKING_DIRECTORY "${_dxbc_finite_visual_unit_work}")
        add_test(NAME unity_finite_visual_gate_help
            COMMAND unity_finite_visual_gate_cli --help)
        set_tests_properties(unity_finite_visual_gate_help PROPERTIES
            PASS_REGULAR_EXPRESSION
                "never a universal visual-equivalence certificate")
    endif()

    if(DXBCSANDBOX_BUILD_UNITY_COMPILER)
        dxbc_add_core_test(test_helpers_units
            tests/test_helpers_units.c standalone_project_discovery)
        target_compile_definitions(test_helpers_units PRIVATE
            DXBC_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
        add_executable(test_compiler_client_units
            tests/test_compiler_client_units.c)
        target_include_directories(test_compiler_client_units PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(test_compiler_client_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME compiler_client_units COMMAND test_compiler_client_units)

        add_executable(test_compiler_session_report_units
            tests/test_compiler_session_report_units.c)
        target_link_libraries(test_compiler_session_report_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME compiler_session_report_units
            COMMAND test_compiler_session_report_units)
        add_test(NAME compiler_session_cli_help
            COMMAND compiler_session_cli --help)
        set_tests_properties(compiler_session_cli_help PROPERTIES
            PASS_REGULAR_EXPRESSION "all 25 feature/version records")
        add_test(NAME compiler_session_cli_rejects_invalid_format
            COMMAND compiler_session_cli --format yaml)
        set_tests_properties(
            compiler_session_cli_rejects_invalid_format PROPERTIES
            WILL_FAIL TRUE)

        add_executable(test_compiler_cache_hardening_units
            tests/test_compiler_cache_hardening_units.c)
        target_include_directories(test_compiler_cache_hardening_units PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(test_compiler_cache_hardening_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME compiler_cache_hardening_units
            COMMAND test_compiler_cache_hardening_units)

        add_executable(test_compiler_broker_units
            tests/test_compiler_broker_units.c)
        target_include_directories(test_compiler_broker_units PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(test_compiler_broker_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_dependencies(test_compiler_broker_units
            test_compiler_client_units)
        add_test(NAME compiler_broker_units COMMAND test_compiler_broker_units)

        add_executable(test_unity_uv_helper_units tests/test_unity_uv_helper_units.c)
        target_link_libraries(test_unity_uv_helper_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME unity_uv_helper_units COMMAND test_unity_uv_helper_units)

        add_executable(test_compile_authority_units
            tests/test_compile_authority_units.c)
        target_link_libraries(test_compile_authority_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME compile_authority_units COMMAND
            test_compile_authority_units)

        add_executable(test_reflection_certificate_units
            tests/test_reflection_certificate_units.c)
        target_link_libraries(test_reflection_certificate_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME reflection_certificate_units COMMAND
            test_reflection_certificate_units)

        add_executable(test_shaderlab_mapping_units
            tests/test_shaderlab_mapping_units.c)
        target_link_libraries(test_shaderlab_mapping_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME shaderlab_mapping_units COMMAND test_shaderlab_mapping_units)

        add_executable(test_shaderlab_lift_units
            tests/test_shaderlab_lift_units.c tests/test_shaderlab_fixture.c)
        target_include_directories(test_shaderlab_lift_units PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(test_shaderlab_lift_units PRIVATE
            SHADERLAB_EXPRESSION_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/expression_shaderlab/target.bin"
            SHADERLAB_UNITY_UV_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unity_uv_shaderlab/target.bin")
        target_link_libraries(test_shaderlab_lift_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME shaderlab_lift_units COMMAND test_shaderlab_lift_units)

        add_executable(test_generated_domain_certifier_units
            tests/test_generated_domain_certifier_units.c)
        target_include_directories(test_generated_domain_certifier_units
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(test_generated_domain_certifier_units PRIVATE
            unity_compiler_support dxbc_build_options)
        add_test(NAME generated_domain_certifier_units COMMAND
            test_generated_domain_certifier_units)

        # Exercises the strict flags/USBD/source parsers and disassembly
        # normalizer without discovering or launching UnityShaderCompiler.
        add_test(NAME unity_golden_verifier_units COMMAND
            unity_golden_verifier --self-test)
        add_test(NAME unity_golden_baseline_accounting COMMAND
            "${CMAKE_COMMAND}"
            "-DVERIFIER=$<TARGET_FILE:unity_golden_verifier>"
            "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/unlit_color"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_golden_baseline.cmake")

        if(DXBCSANDBOX_REGISTER_LIVE_UNITY_TESTS)
            add_test(NAME unity_copy_lift_live
                COMMAND unity_golden_verifier --self-test-lifts)
            set_tests_properties(unity_copy_lift_live PROPERTIES
                RUN_SERIAL TRUE
                LABELS "live-unity")
            add_test(NAME unity_precision_collision_live
                COMMAND unity_precision_collision_probe)
            set_tests_properties(unity_precision_collision_live PROPERTIES
                RUN_SERIAL TRUE
                LABELS "live-unity")
            add_test(NAME unity_compiler_diagnostic_live
                COMMAND unity_compiler_diagnostic_probe)
            set_tests_properties(unity_compiler_diagnostic_live PROPERTIES
                RUN_SERIAL TRUE
                LABELS "live-unity")
        endif()
    endif()
endif()
