#include "integration_fixture.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>
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

/// Whether an environment variable is set to anything but "0".
[[nodiscard]] bool flag_is_set(const char* name) {
    std::size_t length = 0;
    std::array<char, 16> value{};
    if (::getenv_s(&length, value.data(), value.size(), name) != 0 || length == 0) {
        return false;
    }
    return std::string{value.data()} != "0";
}

/// The first whitespace-separated word of `text`.
///
/// `sha256sum` prints "<hash>  <path>"; only the hash is wanted, and a guest
/// path with a space in it would otherwise arrive attached to it.
[[nodiscard]] std::string first_word(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_first_of(" \t\r\n", begin);
    return std::string{text.substr(begin, end == std::string_view::npos ? end : end - begin)};
}

}  // namespace

bool integration_enabled() {
    return flag_is_set("WSLDISK_INTEGRATION");
}

bool running_on_ci() {
    // What every CI system this could plausibly run on sets, GitHub Actions
    // included. Absent means somebody's desktop, and being wrong that way round
    // only costs a warning nobody needed.
    return flag_is_set("CI");
}

ProcessResult run_wsl(std::span<const std::string> arguments) {
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

std::optional<std::string> integration_blocker() {
    if (!integration_enabled()) {
        return "set WSLDISK_INTEGRATION=1 to run integration tests";
    }
    if (pinned_rootfs().empty()) {
        return "run scripts/fetch-fixtures.ps1 to download the pinned rootfs";
    }
    return std::nullopt;
}

ScratchDistro::ScratchDistro(std::string_view label)
    : name_(std::format("wsldisk-test-{}-{}", label, ::GetCurrentProcessId())),
      directory_(std::filesystem::temp_directory_path() / name_) {
    const std::filesystem::path rootfs = pinned_rootfs();
    if (rootfs.empty()) {
        return;
    }

    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);

    const std::vector<std::string> import{"--import",     name_,       quoted(directory_),
                                          quoted(rootfs), "--version", "2"};
    imported_ = run_wsl(import).exit_code == 0;
}

ScratchDistro::~ScratchDistro() {
    if (imported_) {
        // Unregister deletes the disk wherever BasePath now points, which is
        // the point: a test that moved it must not leave one behind.
        const std::vector<std::string> unregister{"--unregister", name_};
        static_cast<void>(run_wsl(unregister));
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
    for (const std::filesystem::path& path : extra_) {
        std::filesystem::remove_all(path, ignored);
    }
}

ProcessResult ScratchDistro::run(std::span<const std::string> argv) const {
    std::vector<std::string> command{"-d", name_, "--exec"};
    command.insert(command.end(), argv.begin(), argv.end());
    return run_wsl(command);
}

ProcessResult ScratchDistro::run(const std::string& program) const {
    const std::array<std::string, 1> argv{program};
    return run(argv);
}

bool ScratchDistro::boots() const {
    return run("/bin/true").exit_code == 0;
}

void ScratchDistro::terminate() const {
    const std::vector<std::string> arguments{"--terminate", name_};
    static_cast<void>(run_wsl(arguments));
}

bool ScratchDistro::release_disk() const {
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

    // The fallback stops every distribution on the machine (D9). On a runner
    // that is nobody's problem; on a desktop the developer deserves to know why
    // their other windows just went quiet.
    if (!running_on_ci()) {
        std::fprintf(stderr,
                     "wsldisk integration: %s still holds its disk after terminate; running "
                     "`wsl --shutdown`, which stops every distribution on this machine.\n",
                     name_.c_str());
    }
    const std::vector<std::string> shutdown{"--shutdown"};
    static_cast<void>(run_wsl(shutdown));
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (free_now()) {
            return true;
        }
        ::Sleep(500);
    }
    return false;
}

bool ScratchDistro::set_sparse(bool sparse) const {
    // Changing it needs the disk free, and terminating alone does not do that:
    // the utility VM holds every attached disk while any distribution runs
    // (D9). `release_disk` is the thing that actually gets it back.
    if (!release_disk()) {
        return false;
    }
    const std::vector<std::string> arguments{"--manage", name_, "--set-sparse", sparse ? "true" : "false"};
    return run_wsl(arguments).exit_code == 0;
}

bool ScratchDistro::write_junk(std::uint64_t megabytes) const {
    const std::vector<std::string> argv{
        "/bin/dd",   "if=/dev/urandom", "of=/junk.bin", "bs=1M", std::format("count={}", megabytes),
        "conv=fsync"};
    if (run(argv).exit_code != 0) {
        return false;
    }
    // Belt and braces: `conv=fsync` flushes the file, `sync` flushes the
    // filesystem metadata that says how big it is.
    return run("/bin/sync").exit_code == 0;
}

bool ScratchDistro::delete_junk() const {
    const std::array<std::string, 2> argv{"/bin/rm", "/junk.bin"};
    return run(argv).exit_code == 0;
}

std::optional<std::string> ScratchDistro::file_hash(const std::string& guest_path) const {
    // `/usr/bin`, not `/bin`: on Alpine `sha256sum` is a busybox symlink that
    // only exists under /usr/bin, and on merged-usr distributions /bin points
    // at /usr/bin anyway. Guessing /bin here failed on the fixture itself.
    const std::array<std::string, 2> argv{"/usr/bin/sha256sum", guest_path};
    const ProcessResult result = run(argv);
    if (result.exit_code != 0) {
        return std::nullopt;
    }

    // The guest's output is mixed with wsl.exe's own stderr chatter -- one
    // "Failed to translate" line per Windows PATH entry on every --exec. The
    // hash is the first 64-character hex word anywhere in it.
    for (std::string_view rest = result.output; !rest.empty();) {
        const auto newline = rest.find('\n');
        const std::string_view line = rest.substr(0, newline);
        rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);

        const std::string word = first_word(line);
        if (word.size() == 64 && std::ranges::all_of(word, [](unsigned char character) {
                return std::isxdigit(character) != 0;
            })) {
            return word;
        }
    }
    return std::nullopt;
}

void ScratchDistro::also_remove(std::filesystem::path path) {
    extra_.push_back(std::move(path));
}

}  // namespace wsldisk::testing
