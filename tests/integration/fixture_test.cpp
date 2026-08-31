// The harness tests itself.
//
// Every other integration case trusts `ScratchDistro` to import a distribution,
// give it back afterwards, and answer questions about the guest truthfully. If
// any of that is wrong, those cases fail for reasons that have nothing to do
// with the code they are testing -- or worse, pass while leaving a distribution
// registered on the machine that ran them.

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "integration_fixture.h"
#include "platform/filesystem.h"

using wsldisk::platform::Win32FileSystem;
using wsldisk::testing::integration_blocker;
using wsldisk::testing::ProcessResult;
using wsldisk::testing::run_wsl;
using wsldisk::testing::ScratchDistro;

namespace {

constexpr std::uint64_t mebibyte = 1024ULL * 1024;

[[nodiscard]] bool ready() {
    if (const auto blocker = integration_blocker(); blocker.has_value()) {
        SKIP(*blocker);
    }
    return true;
}

/// Whether WSL currently has a distribution of this name.
[[nodiscard]] bool registered(const std::string& name) {
    const std::vector<std::string> arguments{"--list", "--quiet"};
    return run_wsl(arguments).output.find(name) != std::string::npos;
}

}  // namespace

TEST_CASE("the fixture imports a distribution that boots", "[integration]") {
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"fixture"};
    REQUIRE(distro.valid());

    // Named so nobody could mistake it for something of theirs, and so a
    // crashed run leaves something obviously disposable behind.
    CHECK(distro.name().starts_with("wsldisk-test-"));
    CHECK(distro.name().find(std::to_string(::GetCurrentProcessId())) != std::string::npos);
    CHECK(registered(distro.name()));
    CHECK(distro.boots());

    const Win32FileSystem filesystem;
    CHECK(filesystem.exists(distro.vhdx()));
}

TEST_CASE("the fixture gives the distribution back when it goes out of scope", "[integration]") {
    if (!ready()) {
        return;
    }

    std::string name;
    std::filesystem::path directory;
    {
        const ScratchDistro distro{"cleanup"};
        REQUIRE(distro.valid());
        name = distro.name();
        directory = distro.directory();
        REQUIRE(registered(name));
    }

    CHECK_FALSE(registered(name));
    CHECK_FALSE(std::filesystem::exists(directory));
}

TEST_CASE("the fixture cleans up even when the test body throws", "[integration]") {
    // The whole reason cleanup is a destructor and not a call at the end. A
    // failing assertion in Catch2 *is* an exception, so a fixture that only
    // cleaned up on the happy path would leave a distribution registered every
    // time a test failed -- exactly when nobody is watching.
    if (!ready()) {
        return;
    }

    std::string name;
    try {
        const ScratchDistro distro{"throwing"};
        REQUIRE(distro.valid());
        name = distro.name();
        REQUIRE(registered(name));
        throw std::runtime_error{"what a failing test body looks like"};
    } catch (const std::runtime_error&) {  // NOLINT(bugprone-empty-catch) -- deliberate
    }

    REQUIRE_FALSE(name.empty());
    CHECK_FALSE(registered(name));
}

TEST_CASE("the fixture passes argv straight through rather than through a shell", "[integration]") {
    // Never `sh -c`: the guest shell is busybox ash, and a command assembled
    // into one string is a command whose behaviour depends on what is in it.
    // An argument with a space in it is the cheapest way to tell the two apart
    // -- through a shell it would arrive as two arguments.
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"argv"};
    REQUIRE(distro.valid());

    const std::vector<std::string> argv{"/bin/echo", "one two"};
    const ProcessResult result = distro.run(argv);

    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("one two") != std::string::npos);
}

TEST_CASE("the fixture reports a guest command that failed", "[integration]") {
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"failing"};
    REQUIRE(distro.valid());

    CHECK(distro.run("/bin/false").exit_code != 0);
    // A program that is not there is a failure too, not a silent success.
    CHECK(distro.run("/bin/no-such-program").exit_code != 0);
}

TEST_CASE("stderr chatter from wsl.exe is not a failure", "[integration]") {
    // Every `--exec` prints one "Failed to translate '<path>'" line per Windows
    // PATH entry. Treating output as the success signal would fail every guest
    // command on a machine with a normal PATH; the exit code is the only thing
    // that can be trusted.
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"chatter"};
    REQUIRE(distro.valid());

    const ProcessResult result = distro.run("/bin/true");

    CHECK(result.exit_code == 0);
}

TEST_CASE("write_junk grows the disk and delete_junk leaves it grown", "[integration]") {
    // Both halves matter. If the write did not reach the disk there would be
    // nothing for `compact` to reclaim, and if the delete gave the space back
    // by itself there would be nothing for `compact` to do.
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"junk"};
    REQUIRE(distro.valid());
    static_cast<void>(distro.set_sparse(false));

    const Win32FileSystem filesystem;
    const auto empty = filesystem.file_size_on_disk(distro.vhdx());
    REQUIRE(empty.has_value());

    constexpr std::uint64_t megabytes = 64;
    REQUIRE(distro.write_junk(megabytes));
    REQUIRE(distro.delete_junk());
    REQUIRE(distro.release_disk());

    const auto grown = filesystem.file_size_on_disk(distro.vhdx());
    REQUIRE(grown.has_value());
    if (*grown <= *empty) {
        // A sparse disk hands the space back on its own, which is a different
        // scenario and not this one.
        SKIP("this disk reclaims by itself; there is nothing to measure");
    }
    CHECK(*grown - *empty >= megabytes * mebibyte / 2);
}

TEST_CASE("file_hash reads a hash out of the guest", "[integration]") {
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"hash"};
    REQUIRE(distro.valid());

    const auto hash = distro.file_hash("/etc/os-release");

    REQUIRE(hash.has_value());
    // A SHA-256 and nothing else: the path `sha256sum` prints after it, and the
    // wsl.exe chatter around it, must not come along.
    CHECK(hash->size() == 64);
    CHECK(std::ranges::all_of(*hash, [](unsigned char character) { return std::isxdigit(character) != 0; }));

    // The same file twice is the same hash; a different file is not.
    CHECK(distro.file_hash("/etc/os-release") == hash);
    CHECK(distro.file_hash("/etc/hostname") != hash);
}

TEST_CASE("file_hash reports a file that is not there rather than guessing", "[integration]") {
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"nohash"};
    REQUIRE(distro.valid());

    CHECK_FALSE(distro.file_hash("/no/such/file").has_value());
}

TEST_CASE("release_disk hands the disk back", "[integration]") {
    if (!ready()) {
        return;
    }

    const ScratchDistro distro{"release"};
    REQUIRE(distro.valid());
    // Started, so the utility VM is holding the disk open when this begins.
    REQUIRE(distro.boots());

    REQUIRE(distro.release_disk());

    const Win32FileSystem filesystem;
    const auto locked = filesystem.is_locked(distro.vhdx());
    REQUIRE(locked.has_value());
    CHECK_FALSE(*locked);
}

TEST_CASE("two scratch distributions do not collide", "[integration]") {
    // The D9 scenario needs a second distribution running while the first is
    // acted on, so they have to be independently nameable and independently
    // removable.
    if (!ready()) {
        return;
    }

    const ScratchDistro first{"pair-a"};
    const ScratchDistro second{"pair-b"};
    REQUIRE(first.valid());
    REQUIRE(second.valid());

    CHECK(first.name() != second.name());
    CHECK(first.directory() != second.directory());
    CHECK(first.boots());
    CHECK(second.boots());
}

TEST_CASE("also_remove cleans up a directory the test made", "[integration]") {
    if (!ready()) {
        return;
    }

    std::filesystem::path extra;
    {
        ScratchDistro distro{"extra"};
        REQUIRE(distro.valid());

        extra = distro.directory().parent_path() / (distro.name() + "-extra");
        std::error_code failed;
        std::filesystem::create_directories(extra, failed);
        REQUIRE_FALSE(failed);
        distro.also_remove(extra);
        REQUIRE(std::filesystem::exists(extra));
    }

    CHECK_FALSE(std::filesystem::exists(extra));
}

TEST_CASE("an invalid fixture leaves nothing behind", "[integration]") {
    // `valid()` false means the import failed. Whatever went wrong, the
    // destructor still has to be safe to run -- it is going to run.
    if (!ready()) {
        return;
    }

    std::filesystem::path directory;
    {
        // A name WSL will refuse: it already exists by the time the second one
        // is constructed.
        const ScratchDistro first{"duplicate"};
        REQUIRE(first.valid());
        const ScratchDistro second{"duplicate"};
        CHECK_FALSE(second.valid());
        directory = second.directory();
        CHECK(directory == first.directory());
    }

    CHECK_FALSE(std::filesystem::exists(directory));
}
