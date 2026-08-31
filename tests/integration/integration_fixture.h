#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wsldisk::testing {

struct ProcessResult {
    int exit_code = 0;
    std::string output;
};

/// True when WSLDISK_INTEGRATION is set to something other than "0".
[[nodiscard]] bool integration_enabled();

/// True when this looks like a CI runner rather than someone's desktop.
///
/// The difference matters for one thing only: `wsl --shutdown` stops every
/// distribution on the machine. On a runner that is nobody's problem; on a
/// developer box it kills whatever they had open, so they get told first.
[[nodiscard]] bool running_on_ci();

/// Runs `wsl.exe` with the given arguments and captures its output as UTF-8.
///
/// wsl.exe writes UTF-16LE to a pipe, so the bytes are transcoded here rather
/// than in the tests; production code goes through `IWslHost` instead.
[[nodiscard]] ProcessResult run_wsl(std::span<const std::string> arguments);

/// The pinned Alpine rootfs, or an empty path when it has not been fetched.
///
/// `scripts/fetch-fixtures.ps1` downloads it and checks the digest. A test that
/// needs it skips rather than fails when it is missing: not having run the
/// fetch script is a setup gap, not a defect in the code under test.
[[nodiscard]] std::filesystem::path pinned_rootfs();

/// Why this machine cannot run an integration test, or nothing when it can.
///
/// One function so every test skips for the same reasons in the same words,
/// rather than each one carrying its own list and drifting.
[[nodiscard]] std::optional<std::string> integration_blocker();

/// A throwaway WSL2 distribution, imported on construction and unregistered on
/// destruction -- including when the test body throws, which is the whole
/// reason it is a destructor rather than a cleanup call at the end.
///
/// The name is prefixed `wsldisk-test-` and carries this process id, so a run
/// can never touch a distribution someone actually uses, and a crashed run
/// leaves something obviously disposable behind.
class ScratchDistro {
public:
    /// Imports the pinned rootfs. `valid()` is false if the import failed --
    /// check it before using anything else.
    explicit ScratchDistro(std::string_view label);

    ~ScratchDistro();

    ScratchDistro(const ScratchDistro&) = delete;
    ScratchDistro& operator=(const ScratchDistro&) = delete;
    ScratchDistro(ScratchDistro&&) = delete;
    ScratchDistro& operator=(ScratchDistro&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return imported_; }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Where `wsl --import` was told to put it.
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    /// The disk itself, at the location it was imported to.
    [[nodiscard]] std::filesystem::path vhdx() const { return directory_ / "ext4.vhdx"; }

    /// Runs a command in the guest, passing `argv` straight through `--exec`.
    ///
    /// Never `sh -c`. The guest shell is busybox ash, which quotes differently
    /// from bash, and a command assembled into one string is a command whose
    /// behaviour depends on what happens to be in it. `argv[0]` must be an
    /// absolute guest path, because `--exec` does not search PATH.
    [[nodiscard]] ProcessResult run(std::span<const std::string> argv) const;

    /// One program with no arguments, which is most of what tests need.
    [[nodiscard]] ProcessResult run(const std::string& program) const;

    /// Whether the distribution starts and runs a command.
    ///
    /// The cheapest possible assertion that a disk still works, and the one
    /// every mutating test ends with.
    [[nodiscard]] bool boots() const;

    /// Stops it, so its disk can be moved.
    void terminate() const;

    /// Stops it and waits until Windows says nothing holds its disk open.
    ///
    /// `wsl --terminate` returns before the utility VM has closed the file, and
    /// on some machines the VM keeps it open until it idles out (D9). So:
    /// terminate, poll, and only if that is not enough fall back to
    /// `wsl --shutdown` -- which is certain but stops every distribution on the
    /// machine, and says so first when that machine is somebody's desktop.
    /// Returns false if the disk is still held after all that.
    [[nodiscard]] bool release_disk() const;

    /// Turns WSL's automatic space reclaim on or off for this distribution.
    ///
    /// Off is what `compact` exists for: without sparse mode the .vhdx only
    /// ever grows, which is the case the whole project is about. WSL 2.5+
    /// creates disks sparse on some machines, and a test that assumed otherwise
    /// would pass or fail depending on whose WSL it ran on. Returns false when
    /// this build of wsl.exe will not do it.
    [[nodiscard]] bool set_sparse(bool sparse) const;

    /// Writes `megabytes` of random data to `/junk.bin` and flushes it.
    ///
    /// `conv=fsync` is not decoration. Without it `dd` returns as soon as the
    /// guest page cache has the data, a later delete drops it before the kernel
    /// ever writes it out, and the .vhdx never grows -- so a compaction test
    /// would measure nothing and report that nothing was reclaimed. Measured
    /// while writing this: 512 MiB written without fsync grew the file by
    /// 33 MiB; with fsync, by 1.1 GiB.
    [[nodiscard]] bool write_junk(std::uint64_t megabytes) const;

    /// Removes `/junk.bin`. ext4 does not hand the blocks back to the host file
    /// on its own, which is exactly what leaves something for `compact` to
    /// reclaim.
    [[nodiscard]] bool delete_junk() const;

    /// The SHA-256 of a guest file, for proving a disk operation left the
    /// contents alone. Absent when the file is not there.
    [[nodiscard]] std::optional<std::string> file_hash(const std::string& guest_path) const;

    /// Removes `path` too when this goes out of scope, after the unregister.
    ///
    /// A test that moves the disk somewhere else has a second directory to
    /// clean up, and the order matters: unregistering deletes the disk from
    /// wherever the registry points, so the directory holding it can only go
    /// afterwards.
    void also_remove(std::filesystem::path path);

private:
    std::string name_;
    std::filesystem::path directory_;
    std::vector<std::filesystem::path> extra_;
    bool imported_ = false;
};

}  // namespace wsldisk::testing
