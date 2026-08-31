#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "errors.h"
#include "model/distro.h"

namespace wsldisk {
class IRegistry;
}  // namespace wsldisk

namespace wsldisk::cli {

class ILogger;

/// The registered names closest to `name`, for the not-found remedy.
///
/// A wrong name is nearly always a typo or a case difference, and listing what
/// *is* registered turns a dead end into a next step. Ordered by edit distance,
/// closest first, and empty when nothing is close enough to be a plausible
/// correction.
[[nodiscard]] std::vector<std::string> nearest_names(std::string_view name,
                                                     const std::vector<std::string>& registered);

/// The error every command gives for a name that is not registered.
///
/// One definition rather than one per command: the exit code, the wording and
/// the "did you mean" all have to be the same whichever command the user typed.
[[nodiscard]] Error distro_not_found(std::string_view name, const std::vector<std::string>& registered);

/// Finds one registered distribution by name.
///
/// For the commands that act on a distribution rather than describe it: it
/// reads the registry and nothing else, where `info` measures the disk as well.
/// Enumeration warnings go to `logger`, because a key that had to be skipped is
/// worth saying even when the one being looked for was found.
[[nodiscard]] Result<model::Distro> find_distro(const IRegistry& registry, std::string_view name,
                                                ILogger& logger);

}  // namespace wsldisk::cli
