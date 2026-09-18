cmake_minimum_required(VERSION 3.10)

project(DXBCSandbox LANGUAGES C)

include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(DXBCSANDBOX_UNITY_COMMON_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/UnityCommon"
    CACHE PATH "UnityCommon source directory (defaults to the submodule)")
if(NOT TARGET UnityCommon::serialized)
    if(NOT EXISTS "${DXBCSANDBOX_UNITY_COMMON_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "UnityCommon is missing. Run git submodule update --init --recursive "
            "or set DXBCSANDBOX_UNITY_COMMON_DIR.")
    endif()
    add_subdirectory("${DXBCSANDBOX_UNITY_COMMON_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/unity-common")
endif()

include(cmake/Options.cmake)

add_library(dxbc_build_options INTERFACE)
if(MSVC)
    target_compile_options(dxbc_build_options INTERFACE /W4 /WX)
else()
    target_compile_options(dxbc_build_options INTERFACE
        -Wall -Wextra -Werror)
endif()

function(dxbc_enable_utf8_command_line target)
    if(MINGW)
        # LINK_FLAGS is used instead of target_link_options so the declared
        # CMake 3.10 minimum remains truthful.
        set_property(TARGET ${target} APPEND_STRING
            PROPERTY LINK_FLAGS " -municode")
    endif()
endfunction()

include(cmake/Targets.cmake)
include(cmake/Unity.cmake)
if(BUILD_TESTING)
    include(cmake/Tests.cmake)
endif()

message(STATUS
    "DXBCSandbox: portable core=ON, asset CLI=${DXBCSANDBOX_BUILD_ASSET_CLI}, "
    "tests=${BUILD_TESTING}")
message(STATUS
    "DXBCSandbox: Unity compiler=${DXBCSANDBOX_BUILD_UNITY_COMPILER}, "
    "oracle=${DXBCSANDBOX_BUILD_UNITY_ORACLE}, "
    "finite visual gate=${DXBCSANDBOX_BUILD_UNITY_FINITE_VISUAL_GATE}, "
    "live Unity tests=${DXBCSANDBOX_REGISTER_LIVE_UNITY_TESTS}, "
    "macOS ABI=${DXBCSANDBOX_BUILD_MACOS_ABI_TEST}, "
    "Windows D3D=${DXBCSANDBOX_BUILD_WINDOWS_D3D_TEST}")
