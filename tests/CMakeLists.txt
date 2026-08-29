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
        # One profile per process, so parallel ctest runs do not clobber each other.
        list(APPEND discover_properties
             ENVIRONMENT "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/profraw/${name}-%p.profraw")
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
