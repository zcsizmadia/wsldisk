#include "catalogue.h"

#include <format>
#include <toml++/toml.hpp>

#include "catalogue_data.h"

namespace wsldisk::model {
namespace {

/// Reads a required string, or says which entry and which key were wrong.
[[nodiscard]] Result<std::string> required_string(const toml::table& entry, std::string_view key,
                                                  std::size_t index) {
    const auto* const value = entry[key].as_string();
    if (value == nullptr) {
        return fail(ErrorCode::Usage, std::format("cache entry {} has no `{}`", index + 1, key),
                    "every [[cache]] needs a path, a label and a safe flag");
    }
    return value->get();
}

}  // namespace

bool path_contains(std::string_view outer, std::string_view inner) {
    if (outer == inner) {
        return false;
    }
    if (!inner.starts_with(outer)) {
        return false;
    }
    // A separator has to follow, or `/var/log` would claim `/var/logbook`.
    // `outer` never carries a trailing slash: the catalogue is checked for that.
    //
    // `inner` is necessarily the longer string by here -- it starts with `outer`
    // and is not equal to it -- so indexing past `outer`'s length is safe.
    return inner[outer.size()] == '/';
}

Result<std::vector<CacheEntry>> parse_catalogue(std::string_view text) {
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& error) {
        return fail(ErrorCode::Usage,
                    std::format("the cache catalogue is malformed: {}", error.description()),
                    "this is a bug in wsldisk; please report it");
    }

    const auto* const entries = table["cache"].as_array();
    if (entries == nullptr) {
        return fail(ErrorCode::Usage, "the cache catalogue has no [[cache]] entries",
                    "this is a bug in wsldisk; please report it");
    }

    std::vector<CacheEntry> catalogue;
    catalogue.reserve(entries->size());
    for (std::size_t index = 0; index < entries->size(); ++index) {
        const auto* const entry = entries->get(index)->as_table();
        if (entry == nullptr) {
            return fail(ErrorCode::Usage, std::format("cache entry {} is not a table", index + 1),
                        "this is a bug in wsldisk; please report it");
        }

        const auto path = required_string(*entry, "path", index);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        const auto label = required_string(*entry, "label", index);
        if (!label.has_value()) {
            return std::unexpected(label.error());
        }
        const auto* const safe = (*entry)["safe"].as_boolean();
        if (safe == nullptr) {
            return fail(ErrorCode::Usage, std::format("cache entry {} has no `safe`", index + 1),
                        "every [[cache]] needs a path, a label and a safe flag");
        }

        // A relative path would be measured against whatever the guest's working
        // directory happened to be, which is not a thing anyone meant to write.
        if (!path->starts_with('/') && !path->starts_with("~/")) {
            return fail(ErrorCode::Usage,
                        std::format("cache entry {} has a relative path `{}`", index + 1, *path),
                        "paths are absolute inside the guest, or start with ~/ for a home directory");
        }
        // A trailing slash would break the containment check, which compares the
        // separator after a prefix.
        if (path->ends_with('/')) {
            return fail(ErrorCode::Usage,
                        std::format("cache entry {} ends with a slash: `{}`", index + 1, *path),
                        "write the path without a trailing slash");
        }

        CacheEntry parsed;
        parsed.path = *path;
        parsed.label = *label;
        parsed.safe = safe->get();
        if (const auto* const note = (*entry)["note"].as_string(); note != nullptr) {
            parsed.note = note->get();
        }
        catalogue.push_back(std::move(parsed));
    }
    return catalogue;
}

const std::vector<CacheEntry>& cache_catalogue() {
    // Parsed once. The file is compiled in, so it cannot change under us and a
    // second parse would answer the same thing more slowly.
    static const std::vector<CacheEntry> catalogue = [] {
        auto parsed = parse_catalogue(embedded_caches_toml);
        // A malformed embedded catalogue is a build that should not have been
        // made; the file is compiled in and the tests parse it, so the empty
        // half is unreachable from a build that exists. It stays because
        // answering with nothing beats aborting a tool the user is holding.
        return parsed.has_value() ? std::move(*parsed)  // LCOV_EXCL_BR_LINE
                                  : std::vector<CacheEntry>{};
    }();
    return catalogue;
}

}  // namespace wsldisk::model
