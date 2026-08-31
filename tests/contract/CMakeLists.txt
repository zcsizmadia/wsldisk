wsldisk_add_test_suite(wsldisk_contract_tests contract
    ../support/fail_without_dialogs.cpp
    completion_contract_test.cpp
    config_contract_test.cpp
    disk_info_contract_test.cpp
    distro_contract_test.cpp
    filesystem_contract_test.cpp
    filesystem_scan_contract_test.cpp
    orphans_contract_test.cpp
    registry_contract_test.cpp
    virtual_disk_contract_test.cpp
    wsl_host_contract_test.cpp
    cli_smoke_test.cpp)

# `wsldisk::cli` for the completion scripts: they are generated from the real
# command tree, and only the CLI library has one.
target_link_libraries(wsldisk_contract_tests PRIVATE wsldisk::lib wsldisk::cli virtdisk uuid)

# The smoke test runs the real executable, which is also how main.cpp gets
# covered; the generator expression resolves per configuration.
target_compile_definitions(wsldisk_contract_tests
    PRIVATE WSLDISK_EXE_PATH="$<TARGET_FILE:wsldisk>")
add_dependencies(wsldisk_contract_tests wsldisk)
