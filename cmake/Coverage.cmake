# Source-based coverage via clang-cl. This is the implementation that gates CI
# (docs/TESTING.md): it is the only one of the two that reports branch coverage.
# OpenCppCoverage runs as a secondary, non-gating report and needs no build flags.

if(NOT WSLDISK_ENABLE_COVERAGE)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "WSLDISK_ENABLE_COVERAGE requires clang-cl "
        "(current compiler: ${CMAKE_CXX_COMPILER_ID}). Use the x64-coverage preset.")
endif()

target_compile_options(wsldisk_flags INTERFACE -fprofile-instr-generate -fcoverage-mapping)
target_link_options(wsldisk_flags INTERFACE -fprofile-instr-generate)

# The profile runtime writes one .profraw per process; %p keeps parallel ctest runs apart.
set(WSLDISK_PROFILE_DIR "${CMAKE_BINARY_DIR}/profraw")
file(MAKE_DIRECTORY "${WSLDISK_PROFILE_DIR}")

find_program(LLVM_PROFDATA_EXE NAMES llvm-profdata REQUIRED)
find_program(LLVM_COV_EXE NAMES llvm-cov REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# `cmake --build --preset x64-coverage --target coverage` produces coverage.lcov,
# an HTML report, and fails if any threshold is missed.
add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${WSLDISK_PROFILE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WSLDISK_PROFILE_DIR}"
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
    COMMAND ${CMAKE_COMMAND}
            -DPROFILE_DIR=${WSLDISK_PROFILE_DIR}
            -DBINARY_DIR=${CMAKE_BINARY_DIR}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DLLVM_PROFDATA=${LLVM_PROFDATA_EXE}
            -DLLVM_COV=${LLVM_COV_EXE}
            -DPYTHON=${Python3_EXECUTABLE}
            -P "${CMAKE_SOURCE_DIR}/cmake/RunCoverage.cmake"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running tests and enforcing the 100% coverage gate"
    USES_TERMINAL
    VERBATIM)
