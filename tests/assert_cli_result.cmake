if(NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED_EXIT OR
   NOT DEFINED EXPECTED_REGEX)
    message(FATAL_ERROR
        "PROGRAM, EXPECTED_EXIT, and EXPECTED_REGEX are required")
endif()

# Verification-profile environment must not make parse-only tests dependent on
# the developer machine that runs CTest.
foreach(variable IN ITEMS
        DXBC_COMPILE_PROFILE
        DXBC_BUILD_PLATFORM
        DXBC_VALID_APIS
        DXBC_D3D11_PLATFORM_CAPS
        DXBC_GLCORE_PLATFORM_CAPS)
    unset(ENV{${variable}})
endforeach()

execute_process(
    COMMAND "${PROGRAM}" ${ARGUMENTS}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)

if(NOT result MATCHES "^-?[0-9]+$" OR
   NOT result EQUAL EXPECTED_EXIT)
    message(FATAL_ERROR
        "unexpected exit ${result}; expected ${EXPECTED_EXIT}\n"
        "stdout:\n${stdout}\nstderr:\n${stderr}")
endif()

string(CONCAT transcript "${stdout}" "${stderr}")
if(NOT transcript MATCHES "${EXPECTED_REGEX}")
    message(FATAL_ERROR
        "expected output regex did not match: ${EXPECTED_REGEX}\n"
        "stdout:\n${stdout}\nstderr:\n${stderr}")
endif()
