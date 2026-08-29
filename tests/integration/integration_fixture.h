#pragma once

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

}  // namespace wsldisk::testing
