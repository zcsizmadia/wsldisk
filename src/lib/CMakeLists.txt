# Configure-time git stamp. Re-running CMake refreshes it; a stale value in an
# incremental build is harmless because the version banner is informational.
find_package(Git QUIET)
set(WSLDISK_GIT_REVISION "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE git_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE git_result)
    if(git_result EQUAL 0 AND NOT git_revision STREQUAL "")
        set(WSLDISK_GIT_REVISION "${git_revision}")
    endif()
endif()

configure_file(version.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/version.cpp" @ONLY)

add_library(wsldisk_lib STATIC
    errors.cpp
    errors.h
    interfaces.h
    version.h
    "${CMAKE_CURRENT_BINARY_DIR}/version.cpp"

    model/disk_info.cpp
    model/disk_info.h
    model/distro.cpp
    model/distro.h
    model/size.cpp
    model/size.h
    model/text.cpp
    model/text.h
    model/wsl_output.cpp
    model/wsl_output.h

    ops/operation.cpp
    ops/operation.h
    ops/runner.cpp
    ops/runner.h

    platform/clock.cpp
    platform/clock.h
    platform/filesystem.cpp
    platform/filesystem.h
    platform/registry.cpp
    platform/registry.h
    platform/scoped_handle.h
    platform/virtual_disk.cpp
    platform/virtual_disk.h
    platform/win32_api.cpp
    platform/win32_api.h
    platform/win32_error.cpp
    platform/win32_error.h
    platform/wsl_host.cpp
    platform/wsl_host.h)

add_library(wsldisk::lib ALIAS wsldisk_lib)

target_include_directories(wsldisk_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# VirtDisk.lib is the Virtual Disk Service; it is not in the default link set.
target_link_libraries(wsldisk_lib PUBLIC wsldisk::flags PRIVATE virtdisk uuid)

set_target_properties(wsldisk_lib PROPERTIES FOLDER "wsldisk")
