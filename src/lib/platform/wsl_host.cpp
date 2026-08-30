#include "wsl_host.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <utility>

#include "../model/text.h"
#include "../model/wsl_output.h"
#include "scoped_handle.h"
#include "win32_api.h"
#include "win32_error.h"

namespace wsldisk::platform {
namespace {

/// How long each wait slice is. Short enough that a full pipe is drained before
/// the child blocks on it, long enough not to spin.
constexpr DWORD poll_slice_ms = 50;

/// Read buffer for one pipe read. Pipe capacity is 64 KiB by default, so this
/// drains a full buffer in a handful of passes.
constexpr DWORD read_chunk = 8192;

/// A pipe with the parent's end guarded and the child's end inheritable.
struct Pipe {
    ScopedHandle read;
    ScopedHandle write;
};

/// Creates a pipe and makes only the child's end inheritable.
///
/// The parent's end must not be inheritable, or it stays open inside the child
/// and whoever is waiting for end-of-file waits forever.
[[nodiscard]] Status make_pipe(Pipe& pipe, bool child_reads) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    if (win32().create_pipe(pipe.read.put(), pipe.write.put(), &attributes, 0) == FALSE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), "create a pipe for the wsl.exe output"));
    }
    HANDLE parent_end = child_reads ? pipe.write.get() : pipe.read.get();
    if (win32().set_handle_information(parent_end, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        return std::unexpected(
            error_from_win32(win32().get_last_error(), "configure the wsl.exe output pipe"));
    }
    return {};
}

/// Reads whatever is already buffered, without blocking.
///
/// A broken pipe means the child closed its end, which is the normal end of the
/// stream rather than a failure -- so PeekNamedPipe failing ends the drain
/// quietly. A read that fails mid-stream is treated the same way: the exit code
/// is what decides success, and truncated noise on stderr must not turn a
/// working command into an error.
void drain(HANDLE pipe, std::string& into) {
    DWORD available = 0;
    while (win32().peek_named_pipe(pipe, nullptr, 0, nullptr, &available, nullptr) != FALSE &&
           available > 0) {
        std::array<char, read_chunk> buffer{};
        const DWORD wanted = available < read_chunk ? available : read_chunk;
        DWORD read = 0;
        if (win32().read_file(pipe, buffer.data(), wanted, &read, nullptr) == FALSE || read == 0) {
            return;
        }
        into.append(buffer.data(), read);
    }
}

}  // namespace

std::wstring build_command_line(const std::vector<std::wstring>& arguments) {
    // The rules CommandLineToArgvW documents: backslashes are literal unless
    // they precede a quote, in which case they need doubling.
    std::wstring line;
    for (const std::wstring& argument : arguments) {
        if (!line.empty()) {
            line.push_back(L' ');
        }

        const bool needs_quotes =
            argument.empty() || argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
        if (!needs_quotes) {
            line += argument;
            continue;
        }

        line.push_back(L'"');
        for (std::size_t index = 0; index < argument.size(); ++index) {
            std::size_t backslashes = 0;
            while (index < argument.size() && argument[index] == L'\\') {
                ++backslashes;
                ++index;
            }
            if (index == argument.size()) {
                // Trailing backslashes precede the closing quote, so they double.
                line.append(backslashes * 2, L'\\');
                break;
            }
            if (argument[index] == L'"') {
                line.append((backslashes * 2) + 1, L'\\');
            } else {
                line.append(backslashes, L'\\');
            }
            line.push_back(argument[index]);
        }
        line.push_back(L'"');
    }
    return line;
}

Result<WslRawResult> run_wsl(const std::vector<std::wstring>& arguments, std::chrono::milliseconds timeout) {
    // Standard input is a pipe whose write end is closed the moment the child is
    // running, so anything that reads stdin sees end-of-file immediately. The
    // alternative -- a null handle with STARTF_USESTDHANDLES -- hands the child
    // an invalid stdin, and a child that reads it can block until the timeout
    // fires instead of answering.
    Pipe input;
    Pipe output;
    Pipe errors;
    if (const Status ready = make_pipe(input, true); !ready.has_value()) {
        return std::unexpected(ready.error());
    }
    if (const Status ready = make_pipe(output, false); !ready.has_value()) {
        return std::unexpected(ready.error());
    }
    if (const Status ready = make_pipe(errors, false); !ready.has_value()) {
        return std::unexpected(ready.error());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.read.get();
    startup.hStdOutput = output.write.get();
    startup.hStdError = errors.write.get();

    // CreateProcessW may write to the command line buffer, so it cannot be the
    // string's own storage in a const context.
    std::vector<std::wstring> full{L"wsl.exe"};
    full.insert(full.end(), arguments.begin(), arguments.end());
    std::wstring command_line = build_command_line(full);
    command_line.push_back(L'\0');

    PROCESS_INFORMATION process{};
    const BOOL started = win32().create_process(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                                                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (started == FALSE) {
        // error_from_win32 leaves the remedy empty for a code it does not map,
        // and "failed to run wsl.exe: unexpected Win32 error" on its own tells
        // the user nothing. This call site knows what was being launched, so it
        // can say what to check -- a machine with no WSL installed is by far the
        // most common way to get here.
        Error error = error_from_win32(win32().get_last_error(), "run wsl.exe");
        error.remedy = "check that WSL is installed and on PATH; `wsl --status` should answer";
        return std::unexpected(std::move(error));
    }

    const ScopedHandle process_handle{process.hProcess};
    const ScopedHandle thread_handle{process.hThread};

    // The parent's copies of the write ends must go, or the read ends never
    // report end-of-file even after the child exits. Closing the stdin pipe's
    // write end is what gives the child end-of-file on stdin.
    input.write.close();
    output.write.close();
    errors.write.close();

    WslRawResult result;
    std::chrono::milliseconds remaining = timeout;
    // Draining happens on every pass: a child that fills a pipe buffer blocks
    // until the parent reads it, and a child blocked that way would never reach
    // the exit this loop is waiting for.
    while (true) {  // LCOV_EXCL_BR_LINE -- every exit is a return or a break
        drain(output.read.get(), result.standard_output);
        drain(errors.read.get(), result.standard_error);

        const auto slice =
            remaining.count() < poll_slice_ms ? static_cast<DWORD>(remaining.count()) : poll_slice_ms;
        const DWORD waited = win32().wait_for_single_object(process_handle.get(), slice);
        if (waited == WAIT_FAILED) {
            return std::unexpected(error_from_win32(win32().get_last_error(), "wait for wsl.exe to finish"));
        }
        if (waited == WAIT_OBJECT_0) {
            break;
        }

        remaining -= std::chrono::milliseconds{slice};
        if (remaining <= std::chrono::milliseconds::zero()) {
            std::ignore = win32().terminate_process(process_handle.get(), 1);
            return fail(ErrorCode::Generic,
                        std::format("wsl.exe did not finish within {} ms", timeout.count()),
                        "re-run with a longer timeout, or check whether WSL is responding with "
                        "`wsl --status`");
        }
    }

    // Anything written between the last drain and the exit is still in the pipe.
    drain(output.read.get(), result.standard_output);
    drain(errors.read.get(), result.standard_error);

    DWORD exit_code = 0;
    if (win32().get_exit_code_process(process_handle.get(), &exit_code) == FALSE) {
        return std::unexpected(error_from_win32(win32().get_last_error(), "read the exit code of wsl.exe"));
    }
    result.exit_code = static_cast<int>(exit_code);
    return result;
}

namespace {

/// Bytes of a captured stream, for the decoders.
[[nodiscard]] std::span<const std::byte> as_bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Runs a `wsl.exe` command whose only interesting answer is "did it work".
[[nodiscard]] Status run_for_status(const std::vector<std::wstring>& arguments,
                                    std::chrono::milliseconds timeout, std::string_view what) {
    const auto result = run_wsl(arguments, timeout);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }
    if (result->exit_code != 0) {
        return fail(ErrorCode::Generic, std::format("{} failed (wsl.exe exited {})", what, result->exit_code),
                    "run the same command by hand to see what WSL reports");
    }
    return {};
}

/// How long the short, non-guest commands are given. They either answer quickly
/// or WSL itself is wedged, and waiting longer helps nobody.
constexpr std::chrono::milliseconds control_timeout{60'000};

}  // namespace

Result<std::vector<std::string>> WslExeHost::running() const {
    const auto result = run_wsl({L"--list", L"--running", L"--quiet"}, control_timeout);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }
    // Exit code 1 with no names is what an idle WSL reports, so it is not an
    // error -- an empty list is the answer.
    if (result->exit_code != 0 && !result->standard_output.empty()) {
        return fail(ErrorCode::Generic, std::format("wsl.exe --list --running exited {}", result->exit_code),
                    "run `wsl --list --running` by hand to see what WSL reports");
    }
    return model::parse_distribution_list(as_bytes(result->standard_output));
}

Status WslExeHost::terminate(std::string_view name) const {
    return run_for_status({L"--terminate", model::to_wide(name)}, control_timeout,
                          std::format("terminating {}", name));
}

Status WslExeHost::shutdown() const {
    return run_for_status({L"--shutdown"}, control_timeout, "shutting WSL down");
}

Result<WslCommandResult> WslExeHost::run_as_root(std::string_view name, std::span<const std::string> argv,
                                                 std::chrono::milliseconds timeout) const {
    if (argv.empty()) {
        return fail(ErrorCode::Usage, "no command was given to run in the distribution",
                    "pass the absolute path of the guest program to run");
    }
    // --exec does not search PATH. Refusing here turns what would be WSL's
    // `execvpe(...)` at runtime into an error naming the actual problem.
    if (!argv.front().starts_with('/')) {
        return fail(ErrorCode::Usage,
                    std::format("`{}` is not an absolute path in the distribution", argv.front()),
                    "wsl --exec does not search PATH; pass a full path such as /usr/sbin/fstrim");
    }

    std::vector<std::wstring> arguments{L"-d", model::to_wide(name), L"-u", L"root", L"--exec"};
    for (const std::string& piece : argv) {
        arguments.push_back(model::to_wide(piece));
    }

    const auto raw = run_wsl(arguments, timeout);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    // The guest's streams are the guest's bytes: UTF-8, not wsl.exe's UTF-16.
    return WslCommandResult{.exit_code = raw->exit_code,
                            .standard_output = raw->standard_output,
                            .standard_error = raw->standard_error};
}

Status WslExeHost::mount_bare(const std::filesystem::path& vhdx) const {
    return run_for_status({L"--mount", vhdx.wstring(), L"--vhd", L"--bare"}, control_timeout,
                          std::format("attaching {}", vhdx.string()));
}

Status WslExeHost::unmount(const std::filesystem::path& vhdx) const {
    return run_for_status({L"--unmount", vhdx.wstring()}, control_timeout,
                          std::format("detaching {}", vhdx.string()));
}

}  // namespace wsldisk::platform
