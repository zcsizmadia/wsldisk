#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "errors.h"
#include "list_command.h"
#include "options.h"

namespace CLI {
class App;
}  // namespace CLI

namespace wsldisk::cli {

class ILogger;

/// What `info` was asked about.
struct InfoOptions {
    std::string name;
    /// Start the distribution if it is stopped, to read guest usage.
    bool probe = false;
};

/// The registered names closest to `name`, for the not-found remedy.
///
/// A wrong name is nearly always a typo or a case difference, and listing what
/// *is* registered turns a dead end into a next step. Ordered by edit distance,
/// closest first, and empty when nothing is close enough to be a plausible
/// correction.
[[nodiscard]] std::vector<std::string> nearest_names(std::string_view name,
                                                     const std::vector<std::string>& registered);

/// Finds one distribution and measures it.
///
/// Fails with `DistroNotFound` when the name is not registered, naming the
/// closest matches in the remedy.
[[nodiscard]] Result<ListRow> gather_one(const Services& services, const InfoOptions& options,
                                         ILogger& logger);

/// Renders everything known about one distribution, as aligned `key: value`
/// lines rather than a table -- there is one subject and twenty fields, which is
/// a list, not a grid.
void render_details(const ListRow& row, std::ostream& out);

/// The same, as a single JSON object.
void render_details_json(const ListRow& row, std::ostream& out);

/// Gathers and renders, returning the process exit code.
[[nodiscard]] int run_info(const Services& services, const InfoOptions& options, const GlobalOptions& global,
                           ILogger& logger, std::ostream& out, std::ostream& err);

/// Adds the `info` subcommand to `app`.
void add_info_command(CLI::App& app, GlobalOptions& global, InfoOptions& options);

}  // namespace wsldisk::cli
