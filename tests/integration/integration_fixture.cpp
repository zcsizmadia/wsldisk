#include "integration_fixture.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "platform/filesystem.h"

namespace wsldisk::testing {
namespace {

/// wsl.exe emits UTF-16LE. Reading it as bytes and dropping the NUL padding is
/// enough for the ASCII output these tests look at; anything richer belongs
/// behind `IWslHost`, not in a test helper.
std::string strip_utf16_padding(const std::string& raw) {
    std::string result;
    result.reserve(raw.size() / 2);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\0') {
            result.push_back(raw[i]);
        }
    }
    return result;
}

/// Quotes a path for a command line. Every path here comes from %TEMP% or the
/// repository, both of which can contain spaces.
std::string quoted(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

}  // namespace

bool integration_enabled() {
    std::size_t length = 0;
    std::array<char, 16> value{};
    if (::getenv_s(&length, value.data(), value.size(), "WSLDISK_INTEGRATION") != 0 || length == 0) {
        return false;
    }
    return std::string{value.data()} != "0";
}

ProcessResult run_wsl(const std::vector<std::string>& arguments) {
    std::string command = "\"wsl.exe\"";
    for (const auto& argument : arguments) {
        command += " " + argument;
    }
    command = "\"" + command + "\" 2>&1";

    std::FILE* pipe = ::_popen(command.c_str(), "rb");
    if (pipe == nullptr) {
        return {.exit_code = -1, .output = {}};
    }

    std::string raw;
    std::array<char, 1024> buffer{};
    std::size_t read = 0;
    while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        raw.append(buffer.data(), read);
    }

    return {.exit_code = ::_pclose(pipe), .output = strip_utf16_padding(raw)};
}

std::filesystem::path pinned_rootfs() {
    // Located from the source tree rather than the build directory: the fetch
    // script caches it next to the manifest that pins its digest.
    const std::filesystem::path cache = std::filesystem::path{WSLDISK_FIXTURE_DIR} / "cache";
    std::error_code ignored;
    if (!std::filesystem::is_directory(cache, ignored)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator{cache, ignored}) {
        if (entry.path().filename().string().starts_with("alpine-minirootfs-")) {
            return entry.path();
        }
    }
    return {};
}

TempDistro::TempDistro(const std::string& suffix)
    : name_("wsldisk-test-" + suffix + "-" + std::to_string(::GetCurrentProcessId())),
      directory_(std::filesystem::temp_directory_path() / name_) {
    const std::filesystem::path rootfs = pinned_rootfs();
    if (rootfs.empty()) {
        return;
    }

    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);

    const ProcessResult imported =
        run_wsl({"--import", name_, quoted(directory_), quoted(rootfs), "--version", "2"});
    imported_ = imported.exit_code == 0;
}

TempDistro::~TempDistro() {
    if (imported_) {
        // Unregister deletes the disk wherever BasePath now points, which is
        // the point: a test that moved it must not leave one behind.
        static_cast<void>(run_wsl({"--unregister", name_}));
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
    for (const std::filesystem::path& path : extra_) {
        std::filesystem::remove_all(path, ignored);
    }
}

ProcessResult TempDistro::run(const std::string& command) const {
    return run_wsl({"-d", name_, "--exec", command});
}

void TempDistro::terminate() const {
    static_cast<void>(run_wsl({"--terminate", name_}));
}

bool TempDistro::release_disk() const {
    const platform::Win32FileSystem filesystem;
    const auto free_now = [&filesystem, this]() {
        const auto locked = filesystem.is_locked(vhdx());
        return locked.has_value() && !*locked;
    };

    terminate();
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (free_now()) {
            return true;
        }
        ::Sleep(500);
    }

    static_cast<void>(run_wsl({"--shutdown"}));
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (free_now()) {
            return true;
        }
        ::Sleep(500);
    }
    return false;
}

void TempDistro::also_remove(std::filesystem::path path) {
    extra_.push_back(std::move(path));
}

}  // namespace wsldisk::testing
