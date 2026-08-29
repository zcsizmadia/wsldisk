# One interface target carrying the project-wide compile/link settings.
# Everything under src/ links it; third-party code never does.

# Locates the directory holding clang's compiler-rt libraries (the ASan and
# profile runtimes).
#
# CMake links clang-cl targets with lld-link directly rather than through the
# clang driver, so the driver never gets to add its own library search path: an
# instrumented build fails at link time with an undefined `__asan_*` or
# `__llvm_profile_*` symbol unless the directory is added explicitly.
#
# `-print-runtime-dir` names the per-target layout, which some LLVM packages do
# not ship; those keep the older `lib/windows` directory instead, so both are
# tried.
function(wsldisk_find_clang_runtime_dir out_var)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-runtime-dir
        OUTPUT_VARIABLE printed_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE print_result)

    set(candidates "")
    if(print_result EQUAL 0 AND NOT printed_dir STREQUAL "")
        file(TO_CMAKE_PATH "${printed_dir}" printed_dir)
        list(APPEND candidates "${printed_dir}")
        get_filename_component(runtime_parent "${printed_dir}" DIRECTORY)
        list(APPEND candidates "${runtime_parent}/windows")
    endif()

    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

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

if(WSLDISK_ENABLE_COVERAGE)
    wsldisk_find_clang_runtime_dir(WSLDISK_CLANG_RUNTIME_DIR)
    if(WSLDISK_CLANG_RUNTIME_DIR STREQUAL "")
        message(FATAL_ERROR
            "Could not locate clang's compiler-rt directory. An instrumented "
            "build cannot link without it; check the LLVM installation.")
    endif()
    target_link_directories(wsldisk_flags INTERFACE "${WSLDISK_CLANG_RUNTIME_DIR}")
    message(STATUS "clang runtime libraries: ${WSLDISK_CLANG_RUNTIME_DIR}")
endif()

if(WSLDISK_ENABLE_ASAN)
    # MSVC's AddressSanitizer, not clang-cl's. clang-cl's Windows ASan miscompiles
    # exception handling on the toolchains we build with: a plain throw/catch
    # faults with an access violation inside the catch block. MSVC's works, links
    # statically with /MT, and accepts the debug CRT, so the ASan build needs no
    # special triplet.
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "WSLDISK_ENABLE_ASAN requires the MSVC compiler; use the x64-asan preset.")
    endif()

    target_compile_options(wsldisk_flags INTERFACE /fsanitize=address)
    # ASan is incompatible with edit-and-continue debug info and with /INCREMENTAL.
    target_compile_options(wsldisk_flags INTERFACE $<$<CONFIG:Debug>:/Zi>)
    target_link_options(wsldisk_flags INTERFACE /INCREMENTAL:NO)

    # The MSVC STL's ASan container annotations are an all-or-nothing choice for
    # everything linked into the image, and the vcpkg dependencies are not built
    # with ASan -- linking mismatched objects fails with LNK2038 on
    # `annotate_string`, `annotate_vector`, `annotate_optional` and whatever the
    # STL adds next.
    #
    # `_DISABLE_STL_ANNOTATION` is the umbrella that implies all of them, so a new
    # annotated container cannot break this build again; the two specific macros
    # are kept for STL versions that predate it. The cost is detection of
    # overflows *within* a std::string, std::vector or std::optional buffer; heap
    # and stack overflows and use-after-free are all still caught.
    target_compile_definitions(wsldisk_flags INTERFACE
        _DISABLE_STL_ANNOTATION=1
        _DISABLE_STRING_ANNOTATION=1
        _DISABLE_VECTOR_ANNOTATION=1)
endif()
