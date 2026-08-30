#include "platform/wsl_host.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "errors.h"
#include "platform/win32_api.h"

using wsldisk::ErrorCode;
using wsldisk::platform::build_command_line;
using wsldisk::platform::run_wsl;
using wsldisk::platform::ScopedWin32Api;
using wsldisk::platform::Win32Api;
using wsldisk::platform::WslExeHost;
using namespace std::chrono_literals;

namespace {

/// Handle values the fake table hands out. Any non-null value will do; distinct
/// ones make a failed assertion say which handle was involved.
const auto stdin_read = reinterpret_cast<HANDLE>(0x08);
const auto stdin_write = reinterpret_cast<HANDLE>(0x09);
const auto stdout_read = reinterpret_cast<HANDLE>(0x10);
const auto stdout_write = reinterpret_cast<HANDLE>(0x11);
const auto stderr_read = reinterpret_cast<HANDLE>(0x20);
const auto stderr_write = reinterpret_cast<HANDLE>(0x21);
const auto process_handle = reinterpret_cast<HANDLE>(0x30);
const auto thread_handle = reinterpret_cast<HANDLE>(0x31);

/// A scripted `wsl.exe` run: what each pipe produces and how the child ends.
struct FakeProcess {
    std::string standard_output;
    std::string standard_error;
    DWORD exit_code = 0;
    /// How many waits report "still running" before the process is signalled.
    /// -1 never finishes, which is how the timeout path is reached.
    int waits_before_exit = 0;
    /// The command line CreateProcess was handed, for asserting arguments.
    std::wstring command_line;
    /// Counts pipe creations so they map to stdin, stdout and stderr in order.
    int pipes_created = 0;
    bool terminated = false;
};

/// Builds a table that runs `process` to completion through the real code path.
Win32Api table_for(FakeProcess& process) {
    Win32Api api;

    api.create_pipe = [&process](PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES, DWORD) -> BOOL {
        // stdin, then stdout, then stderr -- run_wsl's order.
        switch (process.pipes_created++) {
            case 0:
                *read = stdin_read;
                *write = stdin_write;
                break;
            case 1:
                *read = stdout_read;
                *write = stdout_write;
                break;
            default:
                *read = stderr_read;
                *write = stderr_write;
                break;
        }
        return TRUE;
    };
    api.set_handle_information = [](HANDLE, DWORD, DWORD) -> BOOL { return TRUE; };
    api.close_handle = [](HANDLE) -> BOOL { return TRUE; };

    api.create_process = [&process](LPCWSTR, LPWSTR command_line, LPSECURITY_ATTRIBUTES,
                                    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                    LPPROCESS_INFORMATION information) -> BOOL {
        process.command_line = command_line;
        information->hProcess = process_handle;
        information->hThread = thread_handle;
        return TRUE;
    };

    // Each stream is handed over in one read, so `available` drops to zero on
    // the second peek and the drain loop terminates.
    api.peek_named_pipe = [&process](HANDLE pipe, LPVOID, DWORD, LPDWORD, LPDWORD available,
                                     LPDWORD) -> BOOL {
        const std::string& source = pipe == stdout_read ? process.standard_output : process.standard_error;
        *available = static_cast<DWORD>(source.size());
        return TRUE;
    };
    api.read_file = [&process](HANDLE pipe, LPVOID buffer, DWORD to_read, LPDWORD read,
                               LPOVERLAPPED) -> BOOL {
        std::string& source = pipe == stdout_read ? process.standard_output : process.standard_error;
        const DWORD count = std::min(to_read, static_cast<DWORD>(source.size()));
        std::memcpy(buffer, source.data(), count);
        source.erase(0, count);
        *read = count;
        return TRUE;
    };

    api.wait_for_single_object = [&process](HANDLE, DWORD) -> DWORD {
        if (process.waits_before_exit < 0) {
            return WAIT_TIMEOUT;
        }
        if (process.waits_before_exit > 0) {
            --process.waits_before_exit;
            return WAIT_TIMEOUT;
        }
        return WAIT_OBJECT_0;
    };
    api.get_exit_code_process = [&process](HANDLE, LPDWORD code) -> BOOL {
        *code = process.exit_code;
        return TRUE;
    };
    api.terminate_process = [&process](HANDLE, UINT) -> BOOL {
        process.terminated = true;
        return TRUE;
    };
    api.get_last_error = []() -> DWORD { return ERROR_ACCESS_DENIED; };
    return api;
}

/// UTF-16LE bytes as a byte string, which is what a pipe carrying wsl.exe's own
/// output actually contains.
std::string utf16le(std::u16string_view units) {
    std::string bytes;
    for (const char16_t unit : units) {
        bytes.push_back(static_cast<char>(unit & 0xFF));
        bytes.push_back(static_cast<char>((unit >> 8) & 0xFF));
    }
    return bytes;
}

}  // namespace

TEST_CASE("build_command_line leaves simple arguments alone", "[platform][wsl]") {
    CHECK(build_command_line({L"wsl.exe", L"--shutdown"}) == L"wsl.exe --shutdown");
}

TEST_CASE("build_command_line quotes an argument with a space", "[platform][wsl]") {
    // A distribution named "Ubuntu 22.04 LTS" is three arguments without this.
    CHECK(build_command_line({L"--terminate", L"Ubuntu 22.04 LTS"}) == LR"(--terminate "Ubuntu 22.04 LTS")");
}

TEST_CASE("build_command_line quotes an empty argument", "[platform][wsl]") {
    CHECK(build_command_line({L"-d", L""}) == LR"(-d "")");
}

TEST_CASE("build_command_line escapes an embedded quote", "[platform][wsl]") {
    CHECK(build_command_line({LR"(a"b)"}) == LR"("a\"b")");
}

TEST_CASE("build_command_line doubles backslashes before a quote", "[platform][wsl]") {
    CHECK(build_command_line({LR"(a\"b)"}) == LR"("a\\\"b")");
}

TEST_CASE("build_command_line doubles trailing backslashes", "[platform][wsl]") {
    // Without doubling, the closing quote would be escaped by the backslash and
    // the argument would swallow everything after it.
    CHECK(build_command_line({LR"(C:\path with space\)"}) == LR"("C:\path with space\\")");
}

TEST_CASE("build_command_line keeps interior backslashes single", "[platform][wsl]") {
    CHECK(build_command_line({LR"(C:\wsl\ext4.vhdx)"}) == LR"(C:\wsl\ext4.vhdx)");
}

TEST_CASE("build_command_line produces nothing for no arguments", "[platform][wsl]") {
    CHECK(build_command_line({}).empty());
}

TEST_CASE("run_wsl captures both streams and the exit code", "[platform][wsl]") {
    FakeProcess process;
    process.standard_output = "out";
    process.standard_error = "err";
    process.exit_code = 3;
    const ScopedWin32Api scoped{table_for(process)};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE(result.has_value());
    CHECK(result->exit_code == 3);
    CHECK(result->standard_output == "out");
    CHECK(result->standard_error == "err");
}

TEST_CASE("run_wsl runs wsl.exe with the arguments it was given", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    REQUIRE(run_wsl({L"--terminate", L"Alpine"}, 5s).has_value());

    CHECK(process.command_line.starts_with(L"wsl.exe --terminate Alpine"));
}

TEST_CASE("run_wsl keeps draining while the child is still running", "[platform][wsl]") {
    // Output that arrives before the process exits must not be lost, which is
    // the whole reason the pipes are drained inside the wait loop.
    FakeProcess process;
    process.standard_output = "streamed";
    process.waits_before_exit = 2;
    const ScopedWin32Api scoped{table_for(process)};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE(result.has_value());
    CHECK(result->standard_output == "streamed");
}

TEST_CASE("run_wsl reports a pipe that cannot be created", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.create_pipe = [](PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("pipe") != std::string::npos);
}

TEST_CASE("run_wsl reports whichever pipe fails", "[platform][wsl]") {
    // Three pipes are created -- stdin, stdout, stderr -- and each is a separate
    // failure path. A test that only fails the first leaves the other two
    // untested, and they are the ones that leak the pipes already created.
    const int failing = GENERATE(0, 1, 2);
    CAPTURE(failing);

    FakeProcess process;
    Win32Api api = table_for(process);
    int calls = 0;
    api.create_pipe = [&calls, failing](PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES, DWORD) -> BOOL {
        if (calls++ != failing) {
            *read = stdout_read;
            *write = stdout_write;
            return TRUE;
        }
        return FALSE;
    };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("pipe") != std::string::npos);
}

TEST_CASE("run_wsl reports a handle that cannot be made non-inheritable", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.set_handle_information = [](HANDLE, DWORD, DWORD) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("pipe") != std::string::npos);
}

TEST_CASE("run_wsl reports wsl.exe failing to start", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.create_process = [](LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                            LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("wsl.exe") != std::string::npos);
    // Every error carries a remedy. The Win32 mapper has none for an unmapped
    // code, so this call site supplies the one that actually helps.
    CHECK(result.error().remedy.find("wsl --status") != std::string::npos);
}

TEST_CASE("run_wsl reports a failed wait", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.wait_for_single_object = [](HANDLE, DWORD) -> DWORD { return WAIT_FAILED; };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("wait") != std::string::npos);
}

TEST_CASE("run_wsl reports an unreadable exit code", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.get_exit_code_process = [](HANDLE, LPDWORD) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("exit code") != std::string::npos);
}

TEST_CASE("run_wsl terminates a command that outlives its timeout", "[platform][wsl]") {
    FakeProcess process;
    process.waits_before_exit = -1;
    const ScopedWin32Api scoped{table_for(process)};

    const auto result = run_wsl({L"--status"}, 120ms);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("did not finish") != std::string::npos);
    CHECK_FALSE(result.error().remedy.empty());
    CHECK(process.terminated);
}

TEST_CASE("run_wsl clamps the final wait slice to the time left", "[platform][wsl]") {
    // A timeout shorter than one poll slice must not wait longer than asked.
    FakeProcess process;
    process.waits_before_exit = -1;
    DWORD longest = 0;
    Win32Api api = table_for(process);
    api.wait_for_single_object = [&longest](HANDLE, DWORD milliseconds) -> DWORD {
        longest = std::max(longest, milliseconds);
        return WAIT_TIMEOUT;
    };
    const ScopedWin32Api scoped{api};

    CHECK_FALSE(run_wsl({L"--status"}, 10ms).has_value());
    CHECK(longest == 10);
}

TEST_CASE("run_wsl stops draining a pipe that reports no data", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.peek_named_pipe = [](HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD available, LPDWORD) -> BOOL {
        *available = 0;
        return TRUE;
    };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE(result.has_value());
    CHECK(result->standard_output.empty());
}

TEST_CASE("run_wsl treats a broken pipe as the end of the stream", "[platform][wsl]") {
    // The child closing its end is how a stream normally ends, not a failure.
    FakeProcess process;
    Win32Api api = table_for(process);
    api.peek_named_pipe = [](HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD, LPDWORD) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE(result.has_value());
    CHECK(result->standard_output.empty());
}

TEST_CASE("run_wsl stops draining when a read fails", "[platform][wsl]") {
    FakeProcess process;
    process.standard_output = "partial";
    Win32Api api = table_for(process);
    api.read_file = [](HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE(result.has_value());
    CHECK(result->standard_output.empty());
}

TEST_CASE("run_wsl stops draining on a zero-length read", "[platform][wsl]") {
    FakeProcess process;
    process.standard_output = "partial";
    Win32Api api = table_for(process);
    api.read_file = [](HANDLE, LPVOID, DWORD, LPDWORD read, LPOVERLAPPED) -> BOOL {
        *read = 0;
        return TRUE;
    };
    const ScopedWin32Api scoped{api};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE(result.has_value());
    CHECK(result->standard_output.empty());
}

TEST_CASE("running lists the distributions wsl.exe reports", "[platform][wsl]") {
    FakeProcess process;
    process.standard_output = utf16le(u"Ubuntu\r\nAlpine\r\n");
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    const auto names = host.running();

    REQUIRE(names.has_value());
    REQUIRE(names->size() == 2);
    CHECK((*names)[0] == "Ubuntu");
    CHECK((*names)[1] == "Alpine");
}

TEST_CASE("running reports nothing when no distribution is up", "[platform][wsl]") {
    // An idle WSL exits non-zero with an empty list; that is an answer, not a
    // failure, and treating it as one would break `compact` on an idle machine.
    FakeProcess process;
    process.exit_code = 1;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    const auto names = host.running();

    REQUIRE(names.has_value());
    CHECK(names->empty());
}

TEST_CASE("running reports a genuine failure", "[platform][wsl]") {
    FakeProcess process;
    process.standard_output = utf16le(u"Ubuntu\r\n");
    process.exit_code = 1;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    const auto names = host.running();

    REQUIRE_FALSE(names.has_value());
    CHECK(names.error().message.find("--list") != std::string::npos);
}

TEST_CASE("running propagates a process that could not start", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.create_process = [](LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                            LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const WslExeHost host;
    CHECK_FALSE(host.running().has_value());
}

TEST_CASE("terminate names the distribution on the command line", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    REQUIRE(host.terminate("Alpine").has_value());

    CHECK(process.command_line == L"wsl.exe --terminate Alpine");
}

TEST_CASE("terminate reports a non-zero exit", "[platform][wsl]") {
    FakeProcess process;
    process.exit_code = 1;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    const auto status = host.terminate("Alpine");

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().message.find("Alpine") != std::string::npos);
}

TEST_CASE("terminate propagates a process that could not start", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.create_process = [](LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                            LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const WslExeHost host;
    CHECK_FALSE(host.terminate("Alpine").has_value());
}

TEST_CASE("shutdown asks WSL to stop everything", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    REQUIRE(host.shutdown().has_value());

    CHECK(process.command_line == L"wsl.exe --shutdown");
}

TEST_CASE("run_as_root builds the -u root --exec form", "[platform][wsl]") {
    FakeProcess process;
    process.standard_output = "1078939029504 bytes trimmed";
    const ScopedWin32Api scoped{table_for(process)};

    const std::vector<std::string> argv{"/usr/sbin/fstrim", "-v", "/"};
    const WslExeHost host;
    const auto result = host.run_as_root("Alpine", argv, 30s);

    REQUIRE(result.has_value());
    CHECK(process.command_line == L"wsl.exe -d Alpine -u root --exec /usr/sbin/fstrim -v /");
    // The guest's output is UTF-8, so it arrives byte for byte.
    CHECK(result->standard_output == "1078939029504 bytes trimmed");
}

TEST_CASE("run_as_root reports the guest's exit code without calling it a failure", "[platform][wsl]") {
    // A non-zero guest command is an answer the caller interprets, not an error
    // in running it -- `e2fsck -n` exits non-zero to report findings.
    FakeProcess process;
    process.exit_code = 4;
    const ScopedWin32Api scoped{table_for(process)};

    const std::vector<std::string> argv{"/usr/sbin/e2fsck", "-n", "/dev/sdc"};
    const WslExeHost host;
    const auto result = host.run_as_root("Alpine", argv, 30s);

    REQUIRE(result.has_value());
    CHECK(result->exit_code == 4);
    CHECK_FALSE(result->succeeded());
}

TEST_CASE("run_as_root refuses a relative program", "[platform][wsl]") {
    // --exec does not search PATH; catching it here names the real problem
    // instead of surfacing WSL's execvpe error.
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const std::vector<std::string> argv{"fstrim", "/"};
    const WslExeHost host;
    const auto result = host.run_as_root("Alpine", argv, 30s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Usage);
    CHECK(result.error().remedy.find("does not search PATH") != std::string::npos);
    CHECK(process.command_line.empty());
}

TEST_CASE("run_as_root refuses an empty command", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    const auto result = host.run_as_root("Alpine", {}, 30s);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Usage);
}

TEST_CASE("run_as_root propagates a process that could not start", "[platform][wsl]") {
    FakeProcess process;
    Win32Api api = table_for(process);
    api.create_process = [](LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                            LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) -> BOOL { return FALSE; };
    const ScopedWin32Api scoped{api};

    const std::vector<std::string> argv{"/bin/true"};
    const WslExeHost host;
    CHECK_FALSE(host.run_as_root("Alpine", argv, 30s).has_value());
}

TEST_CASE("run_as_root handles a distribution name needing quotes", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const std::vector<std::string> argv{"/bin/true"};
    const WslExeHost host;
    REQUIRE(host.run_as_root("Ubuntu 22.04 LTS", argv, 30s).has_value());

    CHECK(process.command_line == LR"(wsl.exe -d "Ubuntu 22.04 LTS" -u root --exec /bin/true)");
}

TEST_CASE("run_as_root carries a non-ASCII distribution name through", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const std::vector<std::string> argv{"/bin/true"};
    const WslExeHost host;
    // UTF-8 for テスト, which has to survive the widen on the way to wsl.exe.
    REQUIRE(host.run_as_root("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88", argv, 30s).has_value());

    CHECK(process.command_line.find(L"\u30C6\u30B9\u30C8") != std::wstring::npos);
}

TEST_CASE("mount_bare attaches the disk without mounting a filesystem", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    REQUIRE(host.mount_bare("C:\\wsl\\ext4.vhdx").has_value());

    CHECK(process.command_line == LR"(wsl.exe --mount C:\wsl\ext4.vhdx --vhd --bare)");
}

TEST_CASE("mount_bare reports a non-zero exit", "[platform][wsl]") {
    FakeProcess process;
    process.exit_code = 1;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    CHECK_FALSE(host.mount_bare("C:\\wsl\\ext4.vhdx").has_value());
}

TEST_CASE("unmount detaches the disk", "[platform][wsl]") {
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    REQUIRE(host.unmount("C:\\wsl\\ext4.vhdx").has_value());

    CHECK(process.command_line == LR"(wsl.exe --unmount C:\wsl\ext4.vhdx)");
}

TEST_CASE("a host owned through the interface destroys cleanly", "[platform][wsl]") {
    // Operations hold an IWslHost, so the base is destroyed polymorphically.
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const std::unique_ptr<wsldisk::IWslHost> host = std::make_unique<WslExeHost>();
    CHECK(host->shutdown().has_value());
}

TEST_CASE("run_wsl drains output larger than one read chunk", "[platform][wsl]") {
    // The read buffer is 8 KiB and a pipe holds 64 KiB, so anything a guest
    // command actually prints takes several passes through the drain loop.
    FakeProcess process;
    process.standard_output = std::string(20000, 'x');
    const ScopedWin32Api scoped{table_for(process)};

    const auto result = run_wsl({L"--status"}, 5s);

    REQUIRE(result.has_value());
    CHECK(result->standard_output.size() == 20000);
}

TEST_CASE("terminate quotes an empty distribution name", "[platform][wsl]") {
    // Not a name anyone should pass, but it must reach wsl.exe as one empty
    // argument rather than disappearing off the command line.
    FakeProcess process;
    const ScopedWin32Api scoped{table_for(process)};

    const WslExeHost host;
    REQUIRE(host.terminate("").has_value());

    CHECK(process.command_line == LR"(wsl.exe --terminate "")");
}
