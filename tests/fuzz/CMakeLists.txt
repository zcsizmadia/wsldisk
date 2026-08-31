# libFuzzer targets. clang-cl only: MSVC has no libFuzzer, so these are built by
# the x64-fuzz preset and skipped everywhere else rather than failing the build.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "WSLDISK_BUILD_FUZZERS requires clang-cl (libFuzzer is not available in "
        "MSVC). Use the x64-fuzz preset.")
endif()

# Registers one fuzz target and a ctest case that replays its seed corpus.
#
# The ctest case is the part that runs on every pull request: it is a few
# milliseconds of regression cover proving the corpus still parses and the
# invariants still hold. Actual fuzzing happens in nightly.yml, which runs the
# same binary with a time budget.
function(wsldisk_add_fuzzer name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE wsldisk::lib)
    # -fsanitize=fuzzer instruments at compile time. It cannot be passed to the
    # link: CMake drives lld-link directly rather than through the clang driver,
    # which rejects it as an unknown argument (and /WX makes that fatal). Name the
    # runtime library explicitly instead -- CompilerFlags.cmake put its directory
    # on the search path.
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)

    file(GLOB fuzzer_runtime "${WSLDISK_CLANG_RUNTIME_DIR}/clang_rt.fuzzer-*.lib")
    if(fuzzer_runtime STREQUAL "")
        message(FATAL_ERROR "libFuzzer runtime not found in ${WSLDISK_CLANG_RUNTIME_DIR}.")
    endif()
    list(GET fuzzer_runtime 0 fuzzer_runtime)
    target_link_libraries(${name} PRIVATE "${fuzzer_runtime}")

    # The sanitizer runtimes are built against the DLL CRT, so linking them into
    # our static-CRT binaries reports LNK4217 for every libc symbol they call
    # directly instead of through an import. It is benign, and /WX would
    # otherwise turn it into a link failure.
    target_link_options(${name} PRIVATE /IGNORE:4217)
    set_target_properties(${name} PROPERTIES FOLDER "tests/fuzz")

    # One corpus per target: an input that exercises the size parser tells the
    # list decoder nothing, and a shared directory makes each target replay the
    # other's inputs for no coverage.
    set(corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")
    add_test(NAME "fuzz.${name}.corpus"
             COMMAND ${name} "${corpus}" -runs=0)
    set_tests_properties("fuzz.${name}.corpus" PROPERTIES LABELS fuzz)
endfunction()

wsldisk_add_fuzzer(fuzz_parse_size fuzz_parse_size.cpp)
wsldisk_add_fuzzer(fuzz_parse_wsl_list fuzz_parse_wsl_list.cpp)
wsldisk_add_fuzzer(fuzz_parse_distro fuzz_parse_distro.cpp)
wsldisk_add_fuzzer(fuzz_parse_config fuzz_parse_config.cpp)
# The distro fuzzer drives enumeration through the in-memory registry fake.
target_include_directories(fuzz_parse_distro PRIVATE "${CMAKE_SOURCE_DIR}/tests/fakes")
