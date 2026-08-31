#pragma once

#include <iosfwd>
#include <string_view>
#include <vector>

#include "errors.h"
#include "interfaces.h"
#include "model/config.h"
#include "model/disk_info.h"
#include "model/distro.h"
#include "options.h"

namespace CLI {
class App;
}  // namespace CLI

namespace wsldisk::cli {

class ILogger;

/// Everything `list` needs, so the command can be driven entirely from fakes.
struct Services {
    /// The registry and the filesystem are not const: `orphans --relink`
    /// writes a registry value and `orphans --delete` removes a file.
    /// Everything read-only simply does not call those methods, which is a
    /// clearer contract than a const pointer and a const_cast at the two places
    /// that need to write.
    IRegistry* registry = nullptr;
    IFileSystem* filesystem = nullptr;
    const IVirtualDisk* disks = nullptr;
    const IWslHost* host = nullptr;
    /// Only the waiting commands need it, but it is here rather than passed
    /// separately so a command cannot be wired up half from services and half
    /// from arguments.
    const IClock* clock = nullptr;

    /// The settings from `config.toml`, already loaded.
    ///
    /// By value with the struct's own defaults, not a pointer: `config.toml` was
    /// parsed, validated, written by `config set` and displayed by `config` for
    /// the whole of M1 while *nothing read it*, because loading it was the
    /// caller's job and no caller did it. A nullable pointer would leave the
    /// same hole open. A default-constructed `Config` is exactly the behaviour
    /// the commands had before, so a test that ignores this field is unaffected.
    model::Config config;
};

/// One row of the listing.
struct ListRow {
    model::Distro distro;
    model::DiskInfo info;
    /// Whether the distribution is running, when that could be determined.
    /// Unknown rather than false when `wsl.exe` did not answer, because
    /// "stopped" and "we could not ask" lead to different next steps.
    std::optional<bool> running;
};

/// What `list` was asked to do.
struct ListOptions {
    /// Start stopped distributions to read guest usage. Off by default: listing
    /// must not change what is running.
    bool probe = false;
};

/// Gathers the rows. Separate from rendering so the table and the JSON cannot
/// disagree about what was measured.
///
/// Fails only when the registry itself cannot be read. A distribution whose disk
/// is missing still produces a row with unknown disk columns -- that is exactly
/// what `orphans --relink` exists to repair, and hiding it would hide the
/// problem.
///
/// `probe_only` restricts guest probing to the distribution of that name.
///
/// Probing a stopped distribution starts it. `info <name> --probe` asks about
/// one, so it must not boot the rest -- and it used to: it enumerated with
/// probing on for every row and filtered afterwards, which on a Docker machine
/// started four distributions and then left the utility VM holding every disk,
/// so the user's next `compact` refused with exit 11. Empty means every row,
/// which is what `list --probe` wants.
[[nodiscard]] Result<std::vector<ListRow>> gather(const Services& services, const ListOptions& options,
                                                  ILogger& logger, std::string_view probe_only = {});

/// Renders the rows as a table.
void render_table(const std::vector<ListRow>& rows, std::ostream& out);

/// Renders the rows as one JSON object per line.
void render_json(const std::vector<ListRow>& rows, std::ostream& out);

/// Gathers and renders, returning the process exit code.
///
/// Everything `list` decides lives here rather than in `app.cpp`, which only
/// wires the real services in. That is what lets both outcomes -- a listing and
/// a registry that cannot be read -- be driven from fakes; in `app.cpp` which
/// one happened would depend on whether the machine running the tests has WSL.
[[nodiscard]] int run_list(const Services& services, const ListOptions& options, const GlobalOptions& global,
                           ILogger& logger, std::ostream& out, std::ostream& err);

/// Adds the `list` subcommand to `app`.
void add_list_command(CLI::App& app, GlobalOptions& global, ListOptions& options);

}  // namespace wsldisk::cli
