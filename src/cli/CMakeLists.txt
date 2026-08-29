find_package(CLI11 CONFIG REQUIRED)

# The command surface lives in a library so unit tests can drive `run()` in
# process, with captured streams, instead of shelling out to the executable.
add_library(wsldisk_cli_lib STATIC
    app.cpp
    app.h)

add_library(wsldisk::cli ALIAS wsldisk_cli_lib)

target_link_libraries(wsldisk_cli_lib PUBLIC wsldisk::lib CLI11::CLI11)
target_include_directories(wsldisk_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(wsldisk_cli_lib PROPERTIES FOLDER "wsldisk")

add_executable(wsldisk main.cpp)
target_link_libraries(wsldisk PRIVATE wsldisk::cli)
set_target_properties(wsldisk PROPERTIES OUTPUT_NAME "wsldisk" FOLDER "wsldisk")
