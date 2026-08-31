#pragma once

#include <filesystem>
#include <string_view>

#include "../errors.h"

namespace wsldisk::platform {

/// Opens `file` in `command` and waits for it to close.
///
/// Waiting is the point: `config edit` re-reads the file afterwards and tells
/// the user if they have left it unparseable, which it can only do once the
/// editor has finished writing. An editor that forks and returns immediately
/// (some GUI ones do) simply means the check runs against the file as it was,
/// which is no worse than not checking.
///
/// The editor's own exit code is ignored. Editors do not agree on what a
/// non-zero exit means, and the file is the only thing that matters.
[[nodiscard]] Status launch_editor(std::string_view command, const std::filesystem::path& file);

}  // namespace wsldisk::platform
