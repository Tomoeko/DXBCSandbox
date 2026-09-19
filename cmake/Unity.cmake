# C launchers create isolated projects and load their installed Editor bridges.
function(dxbc_add_unity_gate target output_name bridge)
    set(bridge_dir "${CMAKE_CURRENT_BINARY_DIR}/share/dxbc-sandbox/unity/Editor")
    file(MAKE_DIRECTORY "${bridge_dir}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/unity/Editor/${bridge}"
        "${bridge_dir}/${bridge}" COPYONLY)
    add_library(${target} STATIC ${ARGN})
    add_library(DXBCSandbox::${target} ALIAS ${target})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${target} PUBLIC dxbc_core PRIVATE dxbc_build_options)
    add_executable(${target}_cli src/compiler/${target}_cli.c)
    set_target_properties(${target}_cli PROPERTIES OUTPUT_NAME "${output_name}")
    target_link_libraries(${target}_cli PRIVATE ${target} dxbc_build_options)
    dxbc_enable_utf8_command_line(${target}_cli)
    install(TARGETS ${target}_cli RUNTIME DESTINATION bin)
    install(FILES resources/unity/Editor/${bridge}
        DESTINATION share/dxbc-sandbox/unity/Editor)
endfunction()

if(DXBCSANDBOX_BUILD_UNITY_IMPORT_GATE)
    dxbc_add_unity_gate(unity_shader_import_gate dxbc-unity-import-gate
        DXBCShaderImportGate.cs src/compiler/unity_shader_import_gate.c)
endif()
if(DXBCSANDBOX_BUILD_UNITY_BUNDLE_GATE)
    dxbc_add_unity_gate(unity_shader_bundle_gate dxbc-unity-bundle-gate
        DXBCShaderBundleGate.cs src/compiler/unity_shader_bundle_gate.c
        src/compiler/unity_shader_bundle_evidence.c)
endif()
if(DXBCSANDBOX_BUILD_UNITY_FINITE_VISUAL_GATE)
    dxbc_add_unity_gate(unity_finite_visual_gate dxbc-unity-finite-visual-gate
        DXBCFiniteVisualGate.cs src/compiler/unity_finite_visual_gate.c
        src/compiler/unity_finite_visual_gate_run.c)
    install(FILES resources/unity/fixtures/finite_visual_propertyless.tsv
        DESTINATION share/dxbc-sandbox/unity/fixtures)
endif()

if(DXBCSANDBOX_BUILD_UNITY_COMPILER)
    set(COMPILER_SOURCES
        src/compiler/unity_compiler_client.c
        src/compiler/unity_compiler_cache.c
        src/compiler/unity_include_scan.c
        src/compiler/unity_include_closure.c
        src/compiler/unity_compiler_broker.c
        src/compiler/unity_compiler_singleflight.c
        src/compiler/unity_compile_authority.c
        src/compiler/unity_uv_helper.c
        src/compiler/unity_compiler_session_report.c
        src/compiler/unity_reflection_certificate.c
        src/compiler/unity_generated_domain_certifier.c
        src/compiler/unity_shaderlab_mapping.c
        src/compiler/unity_shaderlab_lift_capture.c
        src/compiler/unity_shaderlab_lift.c
        src/compiler/unity_shaderlab_lift_report.c
        src/compiler/unity_shaderlab_lift_batch.c
    )

    add_library(unity_compiler_support STATIC ${COMPILER_SOURCES})
    add_library(DXBCSandbox::unity_compiler_support ALIAS
        unity_compiler_support)
    target_include_directories(unity_compiler_support
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(unity_compiler_support
        PUBLIC dxbc_core dxbc_compile_profile Threads::Threads
        PRIVATE dxbc_build_options)

    if(DXBCSANDBOX_BUILD_ASSET_CLI)
        foreach(_dxbc_asset_cli dxbc_sandbox_cli asset_client_cli)
            target_compile_definitions(${_dxbc_asset_cli} PRIVATE DXBCSANDBOX_CLI_UNITY_COMPILER=1)
        endforeach()
    endif()

    # C-only replacement for the historical Python golden runner. One broker
    # and one persistent UnityShaderCompiler process serve the whole corpus.
    get_filename_component(_dxbc_golden_repository_root
        "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
    set(_dxbc_golden_include_authority "")
    add_executable(unity_golden_verifier
        src/compiler/unity_golden_verifier.c)
    target_compile_definitions(unity_golden_verifier PRIVATE
        DXBC_GOLDEN_DEFAULT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        DXBC_GOLDEN_DEFAULT_PROJECT_ROOT="${_dxbc_golden_repository_root}"
        DXBC_GOLDEN_DEFAULT_INCLUDES="${_dxbc_golden_include_authority}"
        DXBC_GOLDEN_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/unlit_color/target.bin")
    target_link_libraries(unity_golden_verifier PRIVATE
        unity_compiler_support dxbc_build_options)

    # Explicit profile/root inputs; run manually against the selected toolchain.
    add_executable(unity_uv_helper_probe tests/probes/unity_uv_helper_probe.c)
    target_link_libraries(unity_uv_helper_probe PRIVATE
        unity_compiler_support dxbc_build_options)

    # Reproducible information-loss witness: two ShaderLab sources produce
    # identical stripped D3D11 bytes but distinct linked GLCore precision.
    # It uses one persistent compiler channel for all requests.
    add_executable(unity_precision_collision_probe
        tests/probes/rw_precision_probe.c)
    target_compile_definitions(unity_precision_collision_probe PRIVATE
        DXBC_PRECISION_DEFAULT_PROJECT_ROOT="${_dxbc_golden_repository_root}"
        DXBC_PRECISION_DEFAULT_INCLUDES="${_dxbc_golden_include_authority}")
    target_link_libraries(unity_precision_collision_probe PRIVATE
        unity_compiler_support dxbc_build_options)

    # Live severity witness for the shared `err:` callback: preprocessing
    # emits a retained type-zero timing note, while successful D3D compilation
    # emits warning 3206 as a retained actionable type-one diagnostic.
    add_executable(unity_compiler_diagnostic_probe
        tests/probes/compiler_diagnostic_probe.c)
    target_compile_definitions(unity_compiler_diagnostic_probe PRIVATE
        DXBC_DIAGNOSTIC_DEFAULT_PROJECT_ROOT="${_dxbc_golden_repository_root}"
        DXBC_DIAGNOSTIC_DEFAULT_INCLUDES="${_dxbc_golden_include_authority}")
    target_link_libraries(unity_compiler_diagnostic_probe PRIVATE
        unity_compiler_support dxbc_build_options)

    # Read-only capture of the complete initializeCompiler authority. It
    # initializes one pinned process, emits no shader request, and shuts the
    # process down immediately after the typed session record is retained.
    add_executable(compiler_session_cli
        src/cli/compiler_session_cli.c)
    set_target_properties(compiler_session_cli PROPERTIES
        OUTPUT_NAME "dxbc-compiler-session")
    # This target is installed. Keep its defaults relative and operator-
    # overrideable instead of embedding the build host's source path.
    target_link_libraries(compiler_session_cli PRIVATE
        unity_compiler_support dxbc_build_options)
    install(TARGETS compiler_session_cli RUNTIME DESTINATION bin)

    if(DXBCSANDBOX_BUILD_UNITY_ORACLE)
        add_executable(test_shaderlab_roundtrip
            tests/test_shaderlab_roundtrip.c)
        target_include_directories(test_shaderlab_roundtrip PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(test_shaderlab_roundtrip PRIVATE
            unity_compiler_support dxbc_build_options)
        if(BUILD_TESTING)
            add_test(NAME shaderlab_roundtrip_help
                COMMAND test_shaderlab_roundtrip --help)
            set_tests_properties(shaderlab_roundtrip_help PROPERTIES
                PASS_REGULAR_EXPRESSION "--direct-only")
            add_test(NAME shaderlab_roundtrip_domain_only_requires_dxbc
                COMMAND test_shaderlab_roundtrip ignored ignored ignored
                    --domain-only)
            set_tests_properties(
                shaderlab_roundtrip_domain_only_requires_dxbc PROPERTIES
                WILL_FAIL TRUE)
            add_test(NAME shaderlab_roundtrip_domain_only_pair_parses
                COMMAND ${CMAKE_COMMAND} -E env
                    DXBC_COMPILE_PROFILE=
                    DXBC_BUILD_PLATFORM=
                    DXBC_VALID_APIS=
                    DXBC_D3D11_PLATFORM_CAPS=
                    DXBC_GLCORE_PLATFORM_CAPS=
                    $<TARGET_FILE:test_shaderlab_roundtrip>
                    ignored ignored ignored --domain-only --dxbc-only)
            set_tests_properties(
                shaderlab_roundtrip_domain_only_pair_parses PROPERTIES
                WILL_FAIL TRUE)
            add_test(NAME shaderlab_roundtrip_direct_only_parses
                COMMAND ${CMAKE_COMMAND}
                    -DPROGRAM=$<TARGET_FILE:test_shaderlab_roundtrip>
                    "-DARGUMENTS=ignored;ignored;ignored;--direct-only;--dxbc-only"
                    -DEXPECTED_EXIT=1
                    "-DEXPECTED_REGEX=Exact verification requires explicit"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_cli_result.cmake)
            add_test(NAME shaderlab_roundtrip_narrow_modes_conflict
                COMMAND ${CMAKE_COMMAND}
                    -DPROGRAM=$<TARGET_FILE:test_shaderlab_roundtrip>
                    "-DARGUMENTS=ignored;ignored;ignored;--domain-only;--direct-only;--dxbc-only"
                    -DEXPECTED_EXIT=1
                    "-DEXPECTED_REGEX=mutually exclusive"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_cli_result.cmake)
            add_test(NAME shaderlab_roundtrip_direct_only_rejects_duplicate
                COMMAND ${CMAKE_COMMAND}
                    -DPROGRAM=$<TARGET_FILE:test_shaderlab_roundtrip>
                    "-DARGUMENTS=ignored;ignored;ignored;--direct-only;--direct-only;--dxbc-only"
                    -DEXPECTED_EXIT=1
                    "-DEXPECTED_REGEX=specified only once"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_cli_result.cmake)
        endif()
    endif()
endif()

if(DXBCSANDBOX_BUILD_MACOS_ABI_TEST)
    add_executable(test_unity_roundtrip tests/abi/test_unity_roundtrip.c)
    target_link_libraries(test_unity_roundtrip PRIVATE
        dxbc_core dxbc_build_options ${CMAKE_DL_LIBS})
endif()

if(DXBCSANDBOX_BUILD_WINDOWS_D3D_TEST)
    add_executable(test_hlsl_compile
        tests/abi/test_hlsl_compile.c
        tests/abi/test_hlsl_compile_verify.c
        tests/abi/d3dcompiler_hook.c)
    target_include_directories(test_hlsl_compile PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(test_hlsl_compile PRIVATE
        dxbc_core dxbc_build_options)
endif()
