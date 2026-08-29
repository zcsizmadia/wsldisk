wsldisk_add_test_suite(wsldisk_unit_tests unit
    errors_test.cpp
    version_test.cpp
    model/size_test.cpp
    platform/filesystem_test.cpp
    platform/win32_error_test.cpp
    cli/app_test.cpp)

target_link_libraries(wsldisk_unit_tests PRIVATE wsldisk::lib wsldisk::cli)
