option(DXBCSANDBOX_BUILD_ASSET_CLI
       "Build the portable asset-bundle to ShaderLab command-line tool" ON)
option(DXBCSANDBOX_BUILD_PROFILE_TOOL
       "Build the portable Unity compile-profile import/validation tool" ON)
option(DXBCSANDBOX_BUILD_ORACLE_PACK_TOOL
       "Build the portable OraclePack validation/inspection tool" ON)
option(DXBCSANDBOX_BUILD_TYPETREE_SCHEMA_TOOL
       "Build the portable pinned TypeTree schema registry tool" ON)
option(DXBCSANDBOX_BUILD_GOLDEN_TARGET_TOOL
       "Build the portable Unity bundle to exact USBD target extractor" ON)
option(DXBCSANDBOX_BUILD_UNITY_IMPORT_GATE
       "Build the isolated Unity ShaderImporter diagnostic gate" ON)
option(DXBCSANDBOX_BUILD_UNITY_BUNDLE_GATE
       "Build the isolated Unity ShaderImporter/AssetBundle gate" ON)
option(DXBCSANDBOX_BUILD_UNITY_FINITE_VISUAL_GATE
       "Build the isolated finite Unity pixel-observation gate" ON)

# This UnityShaderCompiler backend is specifically the macOS 2021.3 protocol
# launcher: in addition to POSIX sockets/threads it resolves an app bundle,
# dylibs, and DYLD_LIBRARY_PATH.  Do not advertise it on another UNIX merely
# because that host has pthreads; other hosts need a separately validated
# launcher backend.
set(DXBCSANDBOX_UNITY_COMPILER_AVAILABLE OFF)
set(_dxbc_unity_default OFF)
if(APPLE)
    find_package(Threads QUIET)
    if(Threads_FOUND)
        set(DXBCSANDBOX_UNITY_COMPILER_AVAILABLE ON)
    endif()
endif()

option(DXBCSANDBOX_BUILD_UNITY_COMPILER
       "Build the optional macOS UnityShaderCompiler 2021.3 protocol backend"
       ${_dxbc_unity_default})
option(DXBCSANDBOX_BUILD_UNITY_ORACLE
       "Build the compiler-backed ShaderLab/oracle verifier"
       ${DXBCSANDBOX_BUILD_UNITY_COMPILER})
option(DXBCSANDBOX_BUILD_MACOS_ABI_TEST
       "Build the version-specific UnityPlayer.dylib ABI diagnostic" OFF)
option(DXBCSANDBOX_BUILD_WINDOWS_D3D_TEST
       "Build the x64 Windows D3DCompiler hook verifier" OFF)
option(DXBCSANDBOX_REGISTER_LIVE_UNITY_TESTS
       "Register tests that launch the installed UnityShaderCompiler" OFF)

if(DXBCSANDBOX_BUILD_UNITY_COMPILER AND
   NOT DXBCSANDBOX_UNITY_COMPILER_AVAILABLE)
    message(FATAL_ERROR
        "DXBCSANDBOX_BUILD_UNITY_COMPILER requires macOS and Threads")
endif()
if(DXBCSANDBOX_BUILD_UNITY_ORACLE AND
   NOT DXBCSANDBOX_BUILD_UNITY_COMPILER)
    message(FATAL_ERROR
        "DXBCSANDBOX_BUILD_UNITY_ORACLE requires "
        "DXBCSANDBOX_BUILD_UNITY_COMPILER=ON")
endif()
if(DXBCSANDBOX_REGISTER_LIVE_UNITY_TESTS AND
   NOT DXBCSANDBOX_BUILD_UNITY_COMPILER)
    message(FATAL_ERROR
        "DXBCSANDBOX_REGISTER_LIVE_UNITY_TESTS requires "
        "DXBCSANDBOX_BUILD_UNITY_COMPILER=ON")
endif()
if(DXBCSANDBOX_BUILD_MACOS_ABI_TEST AND NOT APPLE)
    message(FATAL_ERROR
        "DXBCSANDBOX_BUILD_MACOS_ABI_TEST is available only on macOS")
endif()
if(DXBCSANDBOX_BUILD_WINDOWS_D3D_TEST AND NOT WIN32)
    message(FATAL_ERROR
        "DXBCSANDBOX_BUILD_WINDOWS_D3D_TEST is available only on Windows")
endif()
if(DXBCSANDBOX_BUILD_WINDOWS_D3D_TEST AND
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "The D3DCompiler hook verifier contains x64-specific ABI offsets")
endif()
if(DXBCSANDBOX_BUILD_WINDOWS_D3D_TEST)
    include(CheckCSourceCompiles)
    check_c_source_compiles(
        "#if !defined(_M_X64) && !defined(__x86_64__)\n#error x64 required\n#endif\nint main(void) { return 0; }"
        DXBCSANDBOX_WINDOWS_X64_AVAILABLE)
    if(NOT DXBCSANDBOX_WINDOWS_X64_AVAILABLE)
        message(FATAL_ERROR
            "The D3DCompiler hook verifier is available only for Windows x64")
    endif()
endif()

