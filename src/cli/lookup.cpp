#include "lookup.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

#include "logger.h"

namespace wsldisk::cli {
namespace {

/// How different a name may be and still be offered as "did you mean". Beyond
/// this a suggestion is noise: it stops looking like help and starts looking
/// like the tool guessing.
constexpr std::size_t suggestion_limit = 4;

/// Levenshtein distance, case-insensitively.
[[nodiscard]] std::size_t distance(std::string_view left, std::string_view right) {
    const auto lower = [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    };

    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t index = 0; index <= right.size(); ++index) {
        previous[index] = index;
    }

    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= right.size(); ++column) {
            const std::size_t substitution =
                previous[column - 1] + (lower(left[row - 1]) == lower(right[column - 1]) ? 0 : 1);
            current[column] = std::min({current[column - 1] + 1, previous[column] + 1, substitution});
        }
        previous.swap(current);
    }
    return previous[right.size()];
}

}  // namespace

std::vector<std::string> nearest_names(std::string_view name, const std::vector<std::string>& registered) {
    std::vector<std::pair<std::size_t, std::string>> scored;
    for (const std::string& candidate : registered) {
        if (const std::size_t score = distance(name, candidate); score <= suggestion_limit) {
            scored.emplace_back(score, candidate);
        }
    }
    std::ranges::sort(scored);

    std::vector<std::string> names;
    names.reserve(scored.size());
    for (const auto& [score, candidate] : scored) {
        names.push_back(candidate);
    }
    return names;
}

Error distro_not_found(std::string_view name, const std::vector<std::string>& registered) {
    const std::vector<std::string> suggestions = nearest_names(name, registered);
    std::string remedy = "run `wsldisk list` to see what is registered";
    if (!suggestions.empty()) {
        std::string names;
        for (const std::string& candidate : suggestions) {
            if (!names.empty()) {
                names += ", ";
            }
            names += candidate;
        }
        remedy = std::format("did you mean {}? `wsldisk list` shows them all", names);
    }
    return Error{ErrorCode::DistroNotFound, std::format("no distribution named {}", name), std::move(remedy)};
}

Result<model::Distro> find_distro(const IRegistry& registry, std::string_view name, ILogger& logger) {
    const auto distros = model::enumerate(registry);
    if (!distros.has_value()) {
        return std::unexpected(distros.error());
    }
    for (const std::string& warning : distros->warnings) {
        logger.warn(warning);
    }

    if (const model::Distro* found = distros->find(name); found != nullptr) {
        return *found;
    }

    std::vector<std::string> registered;
    registered.reserve(distros->distros.size());
    for (const model::Distro& distro : distros->distros) {
        registered.push_back(distro.name);
    }
    return std::unexpected(distro_not_found(name, registered));
}

}  // namespace wsldisk::cli
