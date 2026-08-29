# Script mode (cmake -P): merge .profraw files, export lcov + HTML, enforce thresholds.

file(GLOB profraw_files "${PROFILE_DIR}/*.profraw")
if(profraw_files STREQUAL "")
    message(FATAL_ERROR "No .profraw files in ${PROFILE_DIR} — did the tests run with LLVM_PROFILE_FILE set?")
endif()

set(profdata "${BINARY_DIR}/coverage.profdata")
execute_process(
    COMMAND "${LLVM_PROFDATA}" merge -sparse -o "${profdata}" ${profraw_files}
    COMMAND_ERROR_IS_FATAL ANY)

# Every instrumented binary must be passed to llvm-cov; the first is the primary object.
file(GLOB_RECURSE test_binaries "${BINARY_DIR}/tests/*.exe")
file(GLOB_RECURSE cli_binaries "${BINARY_DIR}/src/cli/*.exe")
set(all_binaries ${test_binaries} ${cli_binaries})
if(all_binaries STREQUAL "")
    message(FATAL_ERROR "No instrumented binaries found under ${BINARY_DIR}.")
endif()

list(POP_FRONT all_binaries primary)
set(object_args "")
foreach(bin IN LISTS all_binaries)
    list(APPEND object_args -object "${bin}")
endforeach()

# Only src/ is in scope: tests, spikes and vcpkg dependencies are excluded.
set(ignore_args
    -ignore-filename-regex=[/\\](tests|spikes|vcpkg_installed|_deps)[/\\]
    -ignore-filename-regex=[/\\]Program\ Files)

execute_process(
    COMMAND "${LLVM_COV}" export "${primary}" ${object_args}
            -instr-profile "${profdata}" -format=lcov ${ignore_args}
    OUTPUT_FILE "${BINARY_DIR}/coverage.lcov"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(
    COMMAND "${LLVM_COV}" show "${primary}" ${object_args}
            -instr-profile "${profdata}" -format=html -show-branches=count
            -output-dir "${BINARY_DIR}/coverage-html" ${ignore_args}
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(
    COMMAND "${PYTHON}" "${SOURCE_DIR}/scripts/check-coverage.py"
            "${BINARY_DIR}/coverage.lcov"
            --lines 100 --branches 100 --functions 100
    COMMAND_ERROR_IS_FATAL ANY)

message(STATUS "Coverage report: ${BINARY_DIR}/coverage-html/index.html")
