#pragma once

#include <string>
#include <vector>

#include "../interfaces.h"

namespace wsldisk::platform {

/// `IWslHost` implemented by running `wsl.exe`.
///
/// The process is started with both streams on pipes and no window. Both pipes
/// are drained while the child runs rather than after it exits: a child that
/// fills a pipe buffer blocks until someone reads it, and `--exec` output is
/// not bounded by anything this side controls.
///
/// Timeouts are enforced by the wrapper, not by WSL. A command that outlives
/// its budget is terminated and reported as a failure, so a hung guest command
/// cannot hang the tool.
class WslExeHost final : public IWslHost {
public:
    [[nodiscard]] Result<std::vector<std::string>> running() const override;
    [[nodiscard]] Status terminate(std::string_view name) const override;
    [[nodiscard]] Status shutdown() const override;
    [[nodiscard]] Result<WslCommandResult> run_as_root(std::string_view name,
                                                       std::span<const std::string> argv,
                                                       std::chrono::milliseconds timeout) const override;
    [[nodiscard]] Status mount_bare(const std::filesystem::path& vhdx) const override;
    [[nodiscard]] Status unmount(const std::filesystem::path& vhdx) const override;
};

/// Raw bytes of one finished `wsl.exe` run, before either stream is decoded.
///
/// The two streams do not share an encoding: `wsl.exe`'s own output is UTF-16LE
/// while a guest command's output is UTF-8, and only the caller knows which it
/// asked for. Exposed for tests.
struct WslRawResult {
    int exit_code = 0;
    std::string standard_output;
    std::string standard_error;
};

/// Runs `wsl.exe` with `arguments` and returns both streams undecoded.
///
/// Exposed so the contract tests can drive the real executable without going
/// through a command-shaped wrapper.
[[nodiscard]] Result<WslRawResult> run_wsl(const std::vector<std::wstring>& arguments,
                                           std::chrono::milliseconds timeout);

/// Builds a Windows command line from already-quoted-free arguments, applying
/// the CommandLineToArgvW quoting rules. Exposed for tests: a distribution name
/// with a space or a quote in it is the case that breaks naive concatenation.
[[nodiscard]] std::wstring build_command_line(const std::vector<std::wstring>& arguments);

}  // namespace wsldisk::platform
