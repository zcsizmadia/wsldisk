find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# Catch2's JUnit reporter writes into this directory but will not create it.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-results")

# Registers one Catch2 binary and discovers its cases under `label`, which is
# what `ctest -L unit|contract|integration` and the CI jobs select on.
function(wsldisk_add_test_suite name label)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE wsldisk::flags Catch2::Catch2WithMain)
    set_target_properties(${name} PROPERTIES FOLDER "tests")

    set(discover_properties LABELS ${label})

    if(WSLDISK_ENABLE_COVERAGE)
        # %m, not %p. ctest runs every Catch2 case as its own process, so a suite
        # is a couple of hundred short-lived processes; Windows recycles PIDs
        # aggressively enough that two of them collide, and with %p the second
        # truncates the first's profile. That loses whichever branches the first
        # process was the only one to reach, which shows up as a coverage gate
        # that fails on unrelated lines and passes again on a re-run. %m keys the
        # file on the binary's module signature instead and merges concurrent
        # writers, so the count of files stays fixed and nothing is dropped.
        list(APPEND discover_properties
             ENVIRONMENT "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/profraw/${name}-%m.profraw")
    endif()

    catch_discover_tests(${name}
        TEST_PREFIX "${label}."
        REPORTER JUnit
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/test-results"
        OUTPUT_SUFFIX ".xml"
        PROPERTIES ${discover_properties})
endfunction()

add_subdirectory(unit)
add_subdirectory(contract)
add_subdirectory(integration)

if(WSLDISK_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
