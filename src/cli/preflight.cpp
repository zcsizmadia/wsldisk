#include "preflight.h"

#include <format>

namespace wsldisk::cli {

Status require_wsl2(const model::Distro& distro) {
    if (distro.is_wsl2()) {
        return {};
    }
    return fail(ErrorCode::Preflight,
                std::format("{} is a WSL1 distribution and has no virtual disk", distro.name),
                std::format("convert it with `wsl --set-version {} 2`, then try again", distro.name));
}

}  // namespace wsldisk::cli
