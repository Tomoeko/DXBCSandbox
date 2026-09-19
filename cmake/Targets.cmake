set(COMMON_SOURCES
    src/common/unity_asset_guid.c
    src/common/shader_artifact.c
    src/common/variant_key.c
    src/common/oracle_pack.c
    src/common/oracle_metadata.c
    src/common/shader_stage.c
    src/common/shaderlab_source.c
)

set(IO_SOURCES
    src/io/material_object.c
    src/io/unity_player_shader_caps.c
    src/io/serialized_shader_profile.c
    src/io/serialized_shader.c
    src/io/compute_shader_artifact.c
    src/io/compute_shader_object.c
    src/io/unity_texture_object.c
    src/io/shader_object.c
    src/io/subprogram_metadata.c
    src/io/parameter_layout.c
    src/io/shader_blob_archive.c
    src/io/serialized_glcore_target.c
    src/app/shader_catalog.c
    src/app/shader_catalog_pptr.c
    src/app/shader_batch.c
    src/app/material_batch.c
    src/app/native_texture_batch.c
    src/app/verification_scope.c
    src/app/whole_shader_subject.c
    src/app/whole_shader_evidence.c
    src/app/whole_shader_certificate.c
    src/app/release_shader_object_certificate.c
    src/app/release_shader_certificate_job.c
    src/app/glcore_link_certificate.c
)

set(DXBC_SOURCES
    src/dxbc/dxbc_container.c
    src/dxbc/dxbc_compare.c
    src/dxbc/dxbc_document.c
    src/dxbc/dxbc_stage_contract.c
    src/dxbc/usbd.c
    src/dxbc/dxbc_parser.c
    src/dxbc/dxbc_hash.c
    src/dxbc/dxbc_operand.c
    src/dxbc/dxbc_instruction.c
    src/dxbc/dxbc_decoder.c
    src/translation/usil.c
    src/translation/usil_validation.c
    src/translation/dxbc_cbuffer_projection.c
    src/translation/hlsl_emitter.c
    src/translation/hlsl_emitter_body.c
    src/translation/hlsl_emitter_declarations.c
    src/translation/hlsl_emitter_interface.c
    src/translation/hlsl_emitter_tessellation.c
    src/translation/hlsl_emitter_resources.c
    src/translation/hlsl_analysis_liveness.c
    src/translation/hlsl_analysis_patterns.c
    src/translation/hlsl_analysis_swizzle.c
    src/translation/hlsl_cfg.c
    src/translation/hlsl_provenance.c
    src/translation/hlsl_use_def.c
    src/translation/hlsl_ssa.c
    src/translation/hlsl_copy_lift.c
    src/translation/hlsl_lift_transaction.c
    src/translation/hlsl_ast.c
    src/translation/hlsl_expression_lift.c
    src/translation/hlsl_value_analysis.c
    src/translation/hlsl_storage_plan.c
    src/translation/hlsl_semantic.c
    src/translation/hlsl_compiler_model.c
    src/translation/hlsl_emitter_analysis.c
    src/translation/hlsl_emitter_format.c
    src/translation/hlsl_emitter_metadata.c
    src/translation/hlsl_emitter_ops.c
    src/translation/hlsl_emitter_ops_arithmetic.c
    src/translation/hlsl_emitter_ops_bitwise.c
    src/translation/hlsl_emitter_ops_comparison.c
    src/translation/hlsl_emitter_ops_conversion.c
    src/translation/hlsl_emitter_ops_flow.c
    src/translation/hlsl_emitter_ops_texture.c
    src/translation/hlsl_emitter_utils.c
    src/translation/shaderlab_emitter.c
    src/translation/shaderlab_stage.c
    src/translation/shaderlab_state.c
    src/translation/shaderlab_structural_certificate.c
    src/translation/shaderlab_variant_domain.c
    src/translation/shaderlab_variant_plan.c
    src/translation/unity_yaml.c
    src/translation/material_yaml_emitter.c
    src/translation/native_texture_yaml_emitter.c
)

# This library is the portable, dependency-free product boundary. OraclePack
# is a deterministic file format and remains here; live Unity invocation does
# not.
add_library(dxbc_core STATIC
    ${COMMON_SOURCES}
    ${IO_SOURCES}
    ${DXBC_SOURCES}
)
add_library(DXBCSandbox::core ALIAS dxbc_core)
target_include_directories(dxbc_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dxbc_core
    PUBLIC UnityCommon::serialized
    PRIVATE dxbc_build_options)

if(DXBCSANDBOX_BUILD_ASSET_CLI)
    add_executable(dxbc_sandbox_cli src/cli/dxbc_sandbox_cli.c)
    set_target_properties(dxbc_sandbox_cli PROPERTIES
        OUTPUT_NAME "dxbc-sandbox")
    target_link_libraries(dxbc_sandbox_cli PRIVATE
        dxbc_core dxbc_build_options)

    add_executable(player_build_settings_cli
        src/cli/player_build_settings_cli.c)
    set_target_properties(player_build_settings_cli PROPERTIES
        OUTPUT_NAME "dxbc-player-metadata")
    target_link_libraries(player_build_settings_cli PRIVATE
        dxbc_core dxbc_build_options)

    add_executable(release_shader_certificate_cli
        src/cli/release_shader_certificate_cli.c)
    set_target_properties(release_shader_certificate_cli PROPERTIES
        OUTPUT_NAME "dxbc-release-shader-certificate")
    target_link_libraries(release_shader_certificate_cli PRIVATE
        dxbc_core dxbc_build_options)

    # Preserve the historical executable name and one-input positional mode
    # while routing it through the same catalog and batch implementation.
    add_executable(asset_client_cli src/cli/dxbc_sandbox_cli.c)
    target_compile_definitions(asset_client_cli PRIVATE
        DXBCSANDBOX_LEGACY_ASSET_CLI=1)
    target_link_libraries(asset_client_cli PRIVATE dxbc_core dxbc_build_options)

    dxbc_enable_utf8_command_line(dxbc_sandbox_cli)
    dxbc_enable_utf8_command_line(asset_client_cli)
    dxbc_enable_utf8_command_line(player_build_settings_cli)
    dxbc_enable_utf8_command_line(release_shader_certificate_cli)

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/schemas")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
        "${CMAKE_CURRENT_BINARY_DIR}/schemas/unity-2021.3-player-shader.registry"
        COPYONLY)

    install(TARGETS dxbc_sandbox_cli asset_client_cli
        player_build_settings_cli release_shader_certificate_cli
        RUNTIME DESTINATION bin)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/schemas/unity-2021.3-player-shader.registry"
        DESTINATION share/dxbc-sandbox)
endif()

# Compile profiles are portable captured authority. This library/tool does not
# start UnityShaderCompiler and is intentionally independent of the live
# compiler support option.
add_library(dxbc_compile_profile STATIC
    src/compiler/unity_compile_profile.c)
add_library(DXBCSandbox::compile_profile ALIAS dxbc_compile_profile)
target_include_directories(dxbc_compile_profile
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dxbc_compile_profile
    PUBLIC dxbc_core
    PRIVATE dxbc_build_options)

if(DXBCSANDBOX_BUILD_PROFILE_TOOL)
    add_executable(compile_profile_cli
        src/compiler/unity_compile_profile_cli.c)
    target_link_libraries(compile_profile_cli PRIVATE
        dxbc_compile_profile dxbc_build_options)
    dxbc_enable_utf8_command_line(compile_profile_cli)
    install(TARGETS compile_profile_cli RUNTIME DESTINATION bin)
endif()

# OraclePack validation is deliberately part of the C-only boundary.  This
# tool opens and fully validates captured packs without discovering or
# launching Unity; it is not the compiler-backed replay verifier below.
if(DXBCSANDBOX_BUILD_ORACLE_PACK_TOOL)
    add_executable(oracle_pack_cli src/common/oracle_pack_cli.c)
    target_link_libraries(oracle_pack_cli PRIVATE
        dxbc_core dxbc_build_options)
    dxbc_enable_utf8_command_line(oracle_pack_cli)
    install(TARGETS oracle_pack_cli RUNTIME DESTINATION bin)
endif()

if(DXBCSANDBOX_BUILD_TYPETREE_SCHEMA_TOOL)
    add_executable(typetree_schema_cli src/io/typetree_schema_cli.c)
    target_link_libraries(typetree_schema_cli PRIVATE
        dxbc_core dxbc_build_options)
    dxbc_enable_utf8_command_line(typetree_schema_cli)
    install(TARGETS typetree_schema_cli RUNTIME DESTINATION bin)
endif()

# Golden target extraction is part of evidence ingestion, not live compiler
# invocation. It uses the same strict UnityFS/SerializedFile/TypeTree/player
# parsers as the product and remains available in a Unity-off C-only build.
if(DXBCSANDBOX_BUILD_GOLDEN_TARGET_TOOL)
    add_executable(golden_target_cli src/io/golden_target_cli.c)
    target_link_libraries(golden_target_cli PRIVATE
        dxbc_core dxbc_build_options)
    dxbc_enable_utf8_command_line(golden_target_cli)
    install(TARGETS golden_target_cli RUNTIME DESTINATION bin)
endif()
