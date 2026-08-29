# Integration cases need a real WSL2 install and create/destroy a throwaway
# distro. They are always registered but skip themselves unless
# WSLDISK_INTEGRATION=1 is set, so `ctest -L integration` is meaningful on a
# developer box without turning every other run red.
wsldisk_add_test_suite(wsldisk_integration_tests integration
    integration_fixture.cpp
    integration_fixture.h
    wsl_presence_test.cpp)

target_link_libraries(wsldisk_integration_tests PRIVATE wsldisk::lib)
target_include_directories(wsldisk_integration_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
