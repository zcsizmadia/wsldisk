wsldisk_add_test_suite(wsldisk_unit_tests unit
    ../support/fail_without_dialogs.cpp
    errors_test.cpp
    version_test.cpp
    fakes/fake_registry_test.cpp
    model/disk_info_test.cpp
    model/distro_test.cpp
    model/size_test.cpp
    model/text_test.cpp
    model/wsl_output_test.cpp
    platform/clock_test.cpp
    platform/filesystem_scan_test.cpp
    platform/filesystem_test.cpp
    platform/registry_test.cpp
    platform/virtual_disk_test.cpp
    platform/win32_error_test.cpp
    platform/wsl_host_test.cpp
    cli/app_test.cpp)

target_link_libraries(wsldisk_unit_tests PRIVATE wsldisk::lib wsldisk::cli)
# The in-memory fakes are shared by every suite that drives an operation.
target_include_directories(wsldisk_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests/fakes")
