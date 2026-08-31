#include "editor.h"

#include <windows.h>

#include <format>
#include <string>
#include <tuple>

#include "../model/text.h"
#include "scoped_handle.h"
#include "win32_api.h"
#include "win32_error.h"

namespace wsldisk::platform {
namespace {

/// Quotes a command-line argument if it needs it.
///
/// A config path under `%APPDATA%` goes through `C:\Users\<name>\AppData`, and
/// a user whose account name has a space in it is not unusual.
///
/// Only ever applied to the *file*. `command` arrives ready to use, because
/// whether it needs quoting cannot be decided here: `C:\Program Files\ed.exe`
/// does and `code --wait` must not, and telling them apart means asking the
/// filesystem whether the whole string names a program. The caller has an
/// `IFileSystem`; this does not. Quoting the command here made
/// `EDITOR=code --wait` -- the setting git's own documentation recommends --
/// fail with "file not found" for a program plainly on PATH, because
/// `CreateProcessW` took `"code --wait"` as the executable's name.
[[nodiscard]] std::wstring quoted(const std::wstring& argument) {
    if (argument.find(L' ') == std::wstring::npos) {
        return argument;
    }
    return L'"' + argument + L'"';
}

}  // namespace

Status launch_editor(std::string_view command, const std::filesystem::path& file) {
    // CreateProcessW writes to the command line buffer, so it has to be a
    // writable, NUL-terminated array of our own.
    std::wstring command_line = model::to_wide(command) + L' ' + quoted(file.wstring());
    command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    // No CREATE_NO_WINDOW: a console editor needs the console it was started
    // from, and a GUI one ignores the flag anyway.
    const BOOL started = win32().create_process(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
                                                nullptr, nullptr, &startup, &process);
    if (started == FALSE) {
        Error error = error_from_win32(win32().get_last_error(), std::format("run {}", command));
        error.remedy = std::format("set %EDITOR% to something on PATH, or edit {} by hand", file.string());
        return std::unexpected(std::move(error));
    }

    const ScopedHandle process_handle{process.hProcess};
    const ScopedHandle thread_handle{process.hThread};

    // No timeout: the user is editing, and there is no length of time after
    // which giving up on them would be right.
    if (win32().wait_for_single_object(process_handle.get(), INFINITE) == WAIT_FAILED) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), std::format("wait for {} to close", command)));
    }
    return {};
}

}  // namespace wsldisk::platform
