# One interface target carrying the project-wide compile/link settings.
# Everything under src/ links it; third-party code never does.

add_library(wsldisk_flags INTERFACE)
add_library(wsldisk::flags ALIAS wsldisk_flags)

target_compile_features(wsldisk_flags INTERFACE cxx_std_23)

target_compile_definitions(wsldisk_flags INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    NOGDI
    STRICT
    # Target Windows 10 1809 (17763): the oldest release WSL2 runs on.
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000006)

if(MSVC)
    target_compile_options(wsldisk_flags INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /EHsc
        /MP
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/Gy>)

    target_link_options(wsldisk_flags INTERFACE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>)

    if(WSLDISK_WARNINGS_AS_ERRORS)
        target_compile_options(wsldisk_flags INTERFACE /WX)
        target_link_options(wsldisk_flags INTERFACE /WX)
    endif()

    # clang-cl accepts the MSVC flags above but needs a few of its own warnings tamed
    # to match /W4 semantics without drowning in pedantic noise.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(wsldisk_flags INTERFACE
            -Wno-unused-command-line-argument
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic)
    endif()
endif()

if(WSLDISK_ENABLE_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "WSLDISK_ENABLE_ASAN is only wired up for MSVC/clang-cl.")
    endif()
    target_compile_options(wsldisk_flags INTERFACE /fsanitize=address)
    # ASan is incompatible with edit-and-continue debug info and with /INCREMENTAL.
    target_compile_options(wsldisk_flags INTERFACE $<$<CONFIG:Debug>:/Zi>)
    target_link_options(wsldisk_flags INTERFACE /INCREMENTAL:NO)
endif()
