#pragma once

#include "errors.h"
#include "model/distro.h"

namespace wsldisk::cli {

/// Refuses a WSL1 distribution.
///
/// WSL1 has no VHDX at all -- its filesystem lives in ordinary NTFS
/// directories -- so every command here except `list` has nothing to act on
/// (PLAN.md D8). This is a shared preflight rather than a check inside each
/// command so the refusal, the exit code and the wording cannot drift apart.
///
/// `list` deliberately does not call it: showing a WSL1 distribution alongside
/// the others is how the user finds out it is the odd one.
[[nodiscard]] Status require_wsl2(const model::Distro& distro);

}  // namespace wsldisk::cli
