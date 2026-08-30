wsldisk_add_test_suite(wsldisk_contract_tests contract
    ../support/fail_without_dialogs.cpp
    filesystem_contract_test.cpp
    filesystem_scan_contract_test.cpp
    registry_contract_test.cpp
    virtual_disk_contract_test.cpp
    wsl_host_contract_test.cpp
    cli_smoke_test.cpp)

target_link_libraries(wsldisk_contract_tests PRIVATE wsldisk::lib virtdisk uuid)

# The smoke test runs the real executable, which is also how main.cpp gets
# covered; the generator expression resolves per configuration.
target_compile_definitions(wsldisk_contract_tests
    PRIVATE WSLDISK_EXE_PATH="$<TARGET_FILE:wsldisk>")
add_dependencies(wsldisk_contract_tests wsldisk)
