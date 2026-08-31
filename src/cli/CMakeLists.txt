find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# The command surface lives in a library so unit tests can drive `run()` in
# process, with captured streams, instead of shelling out to the executable.
add_library(wsldisk_cli_lib STATIC
    app.cpp
    app.h
    commands.cpp
    commands.h
    compact_command.cpp
    compact_command.h
    completion_command.cpp
    completion_command.h
    config_command.cpp
    config_command.h
    info_command.cpp
    info_command.h
    list_command.cpp
    list_command.h
    logger.cpp
    logger.h
    lookup.cpp
    lookup.h
    options.cpp
    options.h
    orphans_command.cpp
    orphans_command.h
    preflight.cpp
    preflight.h
    progress.cpp
    progress.h
    render.cpp
    render.h
    trim_command.cpp
    trim_command.h)

add_library(wsldisk::cli ALIAS wsldisk_cli_lib)

target_link_libraries(wsldisk_cli_lib PUBLIC wsldisk::lib CLI11::CLI11 nlohmann_json::nlohmann_json)
target_include_directories(wsldisk_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(wsldisk_cli_lib PROPERTIES FOLDER "wsldisk")

add_executable(wsldisk main.cpp)
target_link_libraries(wsldisk PRIVATE wsldisk::cli)
set_target_properties(wsldisk PROPERTIES OUTPUT_NAME "wsldisk" FOLDER "wsldisk")
