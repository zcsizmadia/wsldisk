#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "errors.h"
#include "interfaces.h"

namespace wsldisk::testing {

/// An in-memory `IWslHost`.
///
/// Nothing here runs a process. A test says which distributions are running and
/// what a given guest command returns, and the fake records every call so an
/// operation's behaviour can be asserted -- that `compact` shut WSL down before
/// touching the disk, that `--dry-run` ran nothing, that `trim` used an absolute
/// path.
class FakeWslHost final : public IWslHost {
public:
    FakeWslHost() = default;

    /// `IWslHost` deletes move to stop slicing through a base reference. This
    /// one is `final`, so there is nothing to slice.
    FakeWslHost(FakeWslHost&& other) noexcept
        : IWslHost(),
          running_(std::move(other.running_)),
          responses_(std::move(other.responses_)),
          calls_(std::move(other.calls_)),
          running_failure_(std::move(other.running_failure_)),
          terminate_failure_(std::move(other.terminate_failure_)),
          shutdown_failure_(std::move(other.shutdown_failure_)),
          mount_failure_(std::move(other.mount_failure_)),
          command_failure_(std::move(other.command_failure_)),
          command_failure_from_(other.command_failure_from_),
          commands_(std::move(other.commands_)),
          terminated_(std::move(other.terminated_)),
          mounted_(std::move(other.mounted_)),
          unmounted_(std::move(other.unmounted_)),
          shutdowns_(other.shutdowns_) {}

    /// One recorded `run_as_root` call.
    struct Invocation {
        std::string distribution;
        std::vector<std::string> argv;
        std::chrono::milliseconds timeout{};
    };

    void set_running(std::vector<std::string> names) { running_ = std::move(names); }

    /// Scripts what a guest command returns, keyed on its `argv[0]`.
    void on_command(std::string program, WslCommandResult result) {
        responses_[std::move(program)] = {std::move(result)};
    }

    /// Scripts successive answers for one program.
    ///
    /// The first call gets the first entry, and the last entry repeats once the
    /// list runs out. An operation that retries a command with different
    /// arguments -- `fstrim -v` then plain `fstrim` -- needs the two calls to
    /// answer differently, which one canned response cannot do.
    void on_commands(std::string program, std::vector<WslCommandResult> results) {
        responses_[std::move(program)] = std::move(results);
    }

    void fail_running(Error error) { running_failure_ = std::move(error); }

    void fail_terminate(Error error) { terminate_failure_ = std::move(error); }

    void fail_shutdown(Error error) { shutdown_failure_ = std::move(error); }

    void fail_mount(Error error) { mount_failure_ = std::move(error); }

    /// Makes running a guest command fail outright -- wsl.exe not answering,
    /// rather than the command inside returning non-zero. The two are different
    /// and callers report them differently.
    void fail_command(Error error) { command_failure_ = std::move(error); }

    /// The same, but only from the `nth` call onwards (1-based).
    ///
    /// A caller that retries has two calls to wsl.exe, and the second failing is
    /// a different path from the first failing. Failing everything can only ever
    /// exercise the first.
    void fail_command_from(std::size_t nth, Error error) {
        command_failure_ = std::move(error);
        command_failure_from_ = nth;
    }

    [[nodiscard]] const std::vector<Invocation>& commands() const noexcept { return commands_; }

    [[nodiscard]] const std::vector<std::string>& terminated() const noexcept { return terminated_; }

    [[nodiscard]] const std::vector<std::string>& mounted() const noexcept { return mounted_; }

    [[nodiscard]] const std::vector<std::string>& unmounted() const noexcept { return unmounted_; }

    [[nodiscard]] int shutdowns() const noexcept { return shutdowns_; }

    [[nodiscard]] Result<std::vector<std::string>> running() const override {
        if (running_failure_) {
            return std::unexpected(*running_failure_);
        }
        return running_;
    }

    [[nodiscard]] Status terminate(std::string_view name) const override {
        if (terminate_failure_) {
            return std::unexpected(*terminate_failure_);
        }
        terminated_.emplace_back(name);
        std::erase(running_, std::string{name});
        return {};
    }

    [[nodiscard]] Status shutdown() const override {
        if (shutdown_failure_) {
            return std::unexpected(*shutdown_failure_);
        }
        ++shutdowns_;
        running_.clear();
        return {};
    }

    [[nodiscard]] Result<WslCommandResult> run_as_root(std::string_view name,
                                                       std::span<const std::string> argv,
                                                       std::chrono::milliseconds timeout) const override {
        // The real wrapper refuses a relative argv[0] because --exec does not
        // search PATH. The fake refuses it too, so a test cannot pass against
        // the fake and then fail against WSL.
        if (argv.empty()) {
            return fail(ErrorCode::Usage, "no command was given to run in the distribution",
                        "pass the absolute path of the guest program to run");
        }
        if (!argv.front().starts_with('/')) {
            return fail(ErrorCode::Usage,
                        "`" + argv.front() + "` is not an absolute path in the distribution",
                        "wsl --exec does not search PATH; pass a full path such as /usr/sbin/fstrim");
        }

        if (command_failure_ && commands_.size() + 1 >= command_failure_from_) {
            return std::unexpected(*command_failure_);
        }

        commands_.push_back(Invocation{
            .distribution = std::string{name}, .argv = {argv.begin(), argv.end()}, .timeout = timeout});

        const auto response = responses_.find(argv.front());
        if (response == responses_.end() || response->second.empty()) {
            return WslCommandResult{};
        }
        // Successive calls walk the list; the last entry repeats.
        const std::size_t index = std::min(calls_[argv.front()]++, response->second.size() - 1);
        return response->second[index];
    }

    [[nodiscard]] Status mount_bare(const std::filesystem::path& vhdx) const override {
        if (mount_failure_) {
            return std::unexpected(*mount_failure_);
        }
        mounted_.push_back(vhdx.string());
        return {};
    }

    [[nodiscard]] Status unmount(const std::filesystem::path& vhdx) const override {
        if (mount_failure_) {
            return std::unexpected(*mount_failure_);
        }
        unmounted_.push_back(vhdx.string());
        return {};
    }

private:
    // Mutable so the const interface can still record what happened; the fake is
    // a test double, not a value type.
    mutable std::vector<std::string> running_;
    std::map<std::string, std::vector<WslCommandResult>> responses_;
    mutable std::map<std::string, std::size_t> calls_;
    std::optional<Error> running_failure_;
    std::optional<Error> terminate_failure_;
    std::optional<Error> shutdown_failure_;
    std::optional<Error> mount_failure_;
    std::optional<Error> command_failure_;
    std::size_t command_failure_from_ = 1;
    mutable std::vector<Invocation> commands_;
    mutable std::vector<std::string> terminated_;
    mutable std::vector<std::string> mounted_;
    mutable std::vector<std::string> unmounted_;
    mutable int shutdowns_ = 0;
};

}  // namespace wsldisk::testing
