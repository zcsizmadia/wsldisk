# Integration cases need a real WSL2 install and create/destroy a throwaway
# distro. They are always registered but skip themselves unless
# WSLDISK_INTEGRATION=1 is set, so `ctest -L integration` is meaningful on a
# developer box without turning every other run red.
# `fail_without_dialogs.cpp` turns a debug-CRT assertion into a failure with a
# message instead of a modal dialog nobody is there to click. The unit and
# contract suites have always linked it; this one did not, and it is the suite
# most likely to trip one -- it drives real WSL.
wsldisk_add_test_suite(wsldisk_integration_tests integration
    ../support/fail_without_dialogs.cpp
    compact_integration_test.cpp
    fixture_test.cpp
    integration_fixture.cpp
    integration_fixture.h
    move_integration_test.cpp
    relink_integration_test.cpp
    trim_integration_test.cpp
    wsl_presence_test.cpp)

target_link_libraries(wsldisk_integration_tests PRIVATE wsldisk::lib)
target_include_directories(wsldisk_integration_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The pinned rootfs is cached next to the manifest that pins its digest, in the
# source tree rather than the build directory.
target_compile_definitions(wsldisk_integration_tests
    PRIVATE WSLDISK_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
