# SPDX-License-Identifier: GPL-3.0-only
# A cache-only fake toolchain keeps this failure-accounting test independent
# of Unity installation, licensing, and compiler process execution.
if(NOT DEFINED VERIFIER OR NOT DEFINED FIXTURE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "VERIFIER, FIXTURE and TEST_ROOT are required")
endif()
set(root "${TEST_ROOT}/golden-baseline-test")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/includes" "${root}/cases/complete"
                    "${root}/cases/missing-source" "${root}/cases/missing-target"
                    "${root}/cases/empty")
file(WRITE "${root}/compiler" "unlaunchable compiler fixture")
file(WRITE "${root}/glslang" "glslang fixture")
file(WRITE "${root}/dxcompiler" "dxcompiler fixture")
foreach(name flags.txt source.shader target.bin)
    configure_file("${FIXTURE}/${name}" "${root}/cases/complete/${name}" COPYONLY)
endforeach()
configure_file("${FIXTURE}/target.bin" "${root}/cases/missing-source/target.bin" COPYONLY)
configure_file("${FIXTURE}/flags.txt" "${root}/cases/missing-source/flags.txt" COPYONLY)
configure_file("${FIXTURE}/flags.txt" "${root}/cases/missing-target/flags.txt" COPYONLY)
set(ENV{DXBC_UNITY_CONTENTS_PATH} "${root}")
set(ENV{DXBC_UNITY_COMPILER_PATH} "${root}/compiler")
set(ENV{DXBC_UNITY_BUILTIN_INCLUDES_PATH} "${root}/includes")
set(ENV{DXBC_UNITY_PLAYBACK_ENGINES_PATH} "${root}")
set(ENV{DXBC_UNITY_GLSLANG_PATH} "${root}/glslang")
set(ENV{DXBC_UNITY_DXCOMPILER_PATH} "${root}/dxcompiler")
set(ENV{DXBC_USC_CACHE_ONLY} "1")
set(ENV{DXBC_USC_CACHE_DIR} "${root}/empty-cache")
execute_process(COMMAND "${VERIFIER}" --reconstruct
    --golden-dir "${root}/cases" --project-root "${root}"
    --includes-dir "${root}/includes" --report "${root}/ledger.jsonl"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT output MATCHES "process starts: 0")
    message(FATAL_ERROR "Expected a complete failed run without Unity: ${result}\n${output}\n${error}")
endif()
file(STRINGS "${root}/ledger.jsonl" records REGEX "\"event\":\"record\"")
file(STRINGS "${root}/ledger.jsonl" cases REGEX "\"event\":\"case\"")
file(STRINGS "${root}/ledger.jsonl" misses REGEX "\"reason\":\"cache_only_miss\"")
file(STRINGS "${root}/ledger.jsonl" skipped REGEX "\"reason\":\"case_setup_failed\"")
list(LENGTH records record_count)
list(LENGTH cases case_count)
list(LENGTH misses miss_count)
list(LENGTH skipped skipped_count)
if(NOT record_count EQUAL 4 OR NOT case_count EQUAL 4 OR
   NOT miss_count EQUAL 2 OR NOT skipped_count EQUAL 2)
    message(FATAL_ERROR "Lost denominator or failure reason: ${record_count}/${case_count}/${miss_count}/${skipped_count}")
endif()
file(READ "${root}/ledger.jsonl" ledger)
if(NOT ledger MATCHES "\"discovered_cases\":4" OR
   NOT ledger MATCHES "\"cases\":4,\"failed_cases\":4" OR
   NOT ledger MATCHES "\"target_domain_available\":false" OR
   ledger MATCHES "${root}" OR ledger MATCHES "\"exact\":true")
    message(FATAL_ERROR "Invalid or private ledger data: ${ledger}")
endif()
# An existing report is immutable: a rerun must fail before overwriting it.
execute_process(COMMAND "${VERIFIER}" --golden-dir "${root}/cases"
    --project-root "${root}" --report "${root}/ledger.jsonl"
    RESULT_VARIABLE repeated OUTPUT_QUIET ERROR_QUIET)
file(READ "${root}/ledger.jsonl" preserved)
if(NOT repeated EQUAL 1 OR NOT preserved STREQUAL ledger)
    message(FATAL_ERROR "An existing baseline was overwritten")
endif()
file(REMOVE_RECURSE "${root}")
