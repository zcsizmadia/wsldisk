#include "integration_fixture.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

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

}  // namespace wsldisk::testing
