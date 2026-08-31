#include "platform/editor.h"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "errors.h"
#include "platform/win32_api.h"

using wsldisk::platform::launch_editor;
using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::Win32Api;

namespace {

const auto process_handle = reinterpret_cast<HANDLE>(0x70);
const auto thread_handle = reinterpret_cast<HANDLE>(0x71);

/// A table where the editor starts and exits cleanly, recording the command
/// line it was given.
Win32Api starts_cleanly(std::wstring& recorded) {
    Win32Api api;
    api.create_process = [&recorded](LPCWSTR, LPWSTR command_line, LPSECURITY_ATTRIBUTES,
                                     LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                     LPPROCESS_INFORMATION information) -> BOOL {
        recorded = command_line;
        information->hProcess = process_handle;
        information->hThread = thread_handle;
        return TRUE;
    };
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_OBJECT_0; };
    api.close_handle = [](HANDLE) -> BOOL { return TRUE; };
    return api;
}

}  // namespace

TEST_CASE("the editor is run with the file as its argument", "[platform][editor]") {
    std::wstring command_line;
    const ScopedWin32Api scoped{starts_cleanly(command_line)};

    REQUIRE(launch_editor("notepad", R"(C:\config.toml)").has_value());

    CHECK(command_line == LR"(notepad C:\config.toml)");
}

TEST_CASE("a path with a space is quoted", "[platform][editor]") {
    // `%APPDATA%` runs through the account name, and an account name with a
    // space in it is not unusual.
    std::wstring command_line;
    const ScopedWin32Api scoped{starts_cleanly(command_line)};

    REQUIRE(launch_editor("notepad", R"(C:\Users\A Name\config.toml)").has_value());

    CHECK(command_line == LR"(notepad "C:\Users\A Name\config.toml")");
}

TEST_CASE("the editor command is passed through exactly as given", "[platform][editor]") {
    std::wstring command_line;
    const ScopedWin32Api scoped{starts_cleanly(command_line)};

    // Arrives already quoted, because whether it needs quoting is
    // `editor_command`'s decision: it is the one with a filesystem to ask
    // whether the whole string names a program. Quoting here instead made
    // `EDITOR=code --wait` unlaunchable -- CreateProcessW took the whole thing
    // as the executable's name.
    REQUIRE(launch_editor(R"("C:\Program Files\editor.exe")", R"(C:\config.toml)").has_value());

    CHECK(command_line == LR"("C:\Program Files\editor.exe" C:\config.toml)");
}

TEST_CASE("the editor's own exit code is ignored", "[platform][editor]") {
    // Editors do not agree on what a non-zero exit means, and the file is the
    // only thing that matters.
    std::wstring command_line;
    Win32Api api = starts_cleanly(command_line);
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_OBJECT_0; };
    const ScopedWin32Api scoped{api};

    CHECK(launch_editor("notepad", R"(C:\config.toml)").has_value());
}

TEST_CASE("an editor that will not start says what to do", "[platform][editor]") {
    Win32Api api;
    api.create_process = [](LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                            LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) -> BOOL { return FALSE; };
    api.get_last_error = []() -> DWORD { return ERROR_FILE_NOT_FOUND; };
    const ScopedWin32Api scoped{api};

    const auto status = launch_editor("no-such-editor", R"(C:\config.toml)");

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().message.find("no-such-editor") != std::string::npos);
    // A remedy that names both ways out, because the tool cannot pick an editor
    // for the user.
    CHECK(status.error().remedy.find("%EDITOR%") != std::string::npos);
    CHECK(status.error().remedy.find("config.toml") != std::string::npos);
}

TEST_CASE("a wait that fails is reported", "[platform][editor]") {
    std::wstring command_line;
    Win32Api api = starts_cleanly(command_line);
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_FAILED; };
    api.get_last_error = []() -> DWORD { return ERROR_INVALID_HANDLE; };
    const ScopedWin32Api scoped{api};

    const auto status = launch_editor("notepad", R"(C:\config.toml)");

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().message.find("wait for notepad to close") != std::string::npos);
}

TEST_CASE("the editor is waited for without a timeout", "[platform][editor]") {
    // The user is editing. There is no length of time after which giving up on
    // them would be right.
    std::wstring command_line;
    DWORD requested = 0;
    Win32Api api = starts_cleanly(command_line);
    api.wait_for_single_object = [&requested](HANDLE, DWORD milliseconds) -> DWORD {
        requested = milliseconds;
        return WAIT_OBJECT_0;
    };
    const ScopedWin32Api scoped{api};

    REQUIRE(launch_editor("notepad", R"(C:\config.toml)").has_value());

    CHECK(requested == INFINITE);
}

TEST_CASE("an editor with arguments is launched as a command line", "[platform][editor]") {
    // `EDITOR=code --wait` is what git's own documentation recommends, and what
    // most VS Code users have set. It used to fail with "file not found" for a
    // program plainly on PATH.
    std::wstring command_line;
    const ScopedWin32Api scoped{starts_cleanly(command_line)};

    REQUIRE(launch_editor("code --wait", R"(C:\config.toml)").has_value());

    CHECK(command_line == LR"(code --wait C:\config.toml)");
}
