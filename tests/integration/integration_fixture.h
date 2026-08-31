#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace wsldisk::testing {

struct ProcessResult {
    int exit_code = 0;
    std::string output;
};

/// True when WSLDISK_INTEGRATION is set to something other than "0".
[[nodiscard]] bool integration_enabled();

/// Runs `wsl.exe` with the given arguments and captures its output as UTF-8.
///
/// wsl.exe writes UTF-16LE to a pipe, so the bytes are transcoded here rather
/// than in the tests; production code goes through `IWslHost` instead.
[[nodiscard]] ProcessResult run_wsl(const std::vector<std::string>& arguments);

/// The pinned Alpine rootfs, or an empty path when it has not been fetched.
///
/// `scripts/fetch-fixtures.ps1` downloads it and checks the digest. A test that
/// needs it skips rather than fails when it is missing: not having run the
/// fetch script is a setup gap, not a defect in the code under test.
[[nodiscard]] std::filesystem::path pinned_rootfs();

/// A throwaway WSL2 distribution, imported on construction and unregistered on
/// destruction.
///
/// Every integration case that mutates anything runs against one of these. The
/// name is prefixed `wsldisk-test-` and carries this process id, so a run can
/// never touch a distribution someone actually uses, and a crashed run leaves
/// something obviously disposable behind.
class TempDistro {
public:
    /// Imports the pinned rootfs. `valid()` is false if the import failed --
    /// check it before using anything else.
    explicit TempDistro(const std::string& suffix);

    ~TempDistro();

    TempDistro(const TempDistro&) = delete;
    TempDistro& operator=(const TempDistro&) = delete;
    TempDistro(TempDistro&&) = delete;
    TempDistro& operator=(TempDistro&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return imported_; }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Where `wsl --import` was told to put it.
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    /// The disk itself, at the location it was imported to.
    [[nodiscard]] std::filesystem::path vhdx() const { return directory_ / "ext4.vhdx"; }

    /// Runs a command in the guest. Used to assert the distribution still boots.
    [[nodiscard]] ProcessResult run(const std::string& command) const;

    /// Stops it, so its disk can be moved.
    void terminate() const;

    /// Stops it and waits until Windows says nothing holds its disk open.
    ///
    /// `wsl --terminate` returns before the utility VM has closed the file, and
    /// on some machines the VM keeps it open until it idles out. So: terminate,
    /// poll, and only if that is not enough fall back to `wsl --shutdown`,
    /// which is certain but stops every distribution the developer running the
    /// suite is using. Returns false if the disk is still held after that.
    [[nodiscard]] bool release_disk() const;

    /// Turns WSL's automatic space reclaim on or off for this distribution.
    ///
    /// Off is what `compact` exists for: without sparse mode the .vhdx only
    /// ever grows, which is the case the whole project is about. WSL 2.5+
    /// creates disks sparse on some machines, and a test that assumed otherwise
    /// would pass or fail depending on whose WSL it ran on. Returns false when
    /// this build of wsl.exe will not do it.
    [[nodiscard]] bool set_sparse(bool sparse) const;

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
