#include "distro.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string_view>

#include "text.h"

namespace wsldisk::model {
namespace {

/// Relative to whatever the registry implementation is rooted at, which is
/// HKEY_CURRENT_USER in production and a scratch key in the contract tests.
constexpr std::wstring_view lxss_key_path = LR"(Software\Microsoft\Windows\CurrentVersion\Lxss)";

constexpr std::wstring_view extended_prefix = LR"(\\?\)";
constexpr std::wstring_view extended_unc_prefix = LR"(\\?\UNC\)";

/// What WSL uses when `VhdFileName` is absent, which is the whole legacy MSIX
/// layout (spike #4).
constexpr std::wstring_view default_vhd_name = L"ext4.vhdx";

[[nodiscard]] bool equals_ignoring_case(std::string_view left, std::string_view right) {
    return std::ranges::equal(
        left, right, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}

/// Reads the values of one `{GUID}` subkey, remembering the first failure.
///
/// A distribution needs a dozen values and any read can fail, which as a chain
/// of `if (!x) return unexpected` is a dozen near-identical error paths that say
/// nothing individually. This keeps the first failure and turns every later read
/// into a no-op, so the whole key is one check at the end.
///
/// A value that is simply *absent* is not a failure: half the layout is
/// optional, and which half depends on how the distribution was installed.
class KeyReader {
public:
    KeyReader(const IRegistry& registry, std::wstring key) : registry_(&registry), key_(std::move(key)) {}

    [[nodiscard]] std::optional<std::wstring> text(std::wstring_view value) {
        if (failure_) {
            return std::nullopt;
        }
        auto read = registry_->read_string(key_, value);
        if (!read.has_value()) {
            failure_ = read.error();
            return std::nullopt;
        }
        return *read;
    }

    [[nodiscard]] std::uint32_t number(std::wstring_view value, std::uint32_t fallback) {
        if (failure_) {
            return fallback;
        }
        const auto read = registry_->read_dword(key_, value);
        if (!read.has_value()) {
            failure_ = read.error();
            return fallback;
        }
        return read->value_or(fallback);
    }

    [[nodiscard]] const std::optional<Error>& failure() const noexcept { return failure_; }

private:
    // A pointer rather than a reference: a reference member would make the
    // class silently non-assignable and clang-tidy rightly objects.
    const IRegistry* registry_;
    std::wstring key_;
    std::optional<Error> failure_;
};

}  // namespace

std::wstring_view lxss_key() {
    return lxss_key_path;
}

std::wstring strip_extended_prefix(std::wstring_view path) {
    // The UNC form has to be checked first: it starts with the plain prefix, and
    // stripping only that would leave `UNC\server\share`, which resolves to
    // nothing.
    if (path.starts_with(extended_unc_prefix)) {
        return LR"(\\)" + std::wstring{path.substr(extended_unc_prefix.size())};
    }
    if (path.starts_with(extended_prefix)) {
        return std::wstring{path.substr(extended_prefix.size())};
    }
    return std::wstring{path};
}

std::filesystem::path vhdx_path_for(std::wstring_view base_path, std::wstring_view vhd_file_name) {
    const std::wstring base = strip_extended_prefix(base_path);
    const std::wstring_view name = vhd_file_name.empty() ? default_vhd_name : vhd_file_name;
    return std::filesystem::path{base} / name;
}

const Distro* DistroList::default_distro() const noexcept {
    const auto found = std::ranges::find_if(distros, [](const Distro& distro) { return distro.is_default; });
    return found == distros.end() ? nullptr : &*found;
}

const Distro* DistroList::find(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(
        distros, [name](const Distro& distro) { return equals_ignoring_case(distro.name, name); });
    return found == distros.end() ? nullptr : &*found;
}

Result<DistroList> enumerate(const IRegistry& registry) {
    const auto guids = registry.subkeys(lxss_key_path);
    if (!guids.has_value()) {
        return std::unexpected(guids.error());
    }

    // Absent or dangling are both normal: a machine can have no default, and the
    // value can outlive the distribution it names.
    const auto default_guid = registry.read_string(lxss_key_path, L"DefaultDistribution");
    if (!default_guid.has_value()) {
        return std::unexpected(default_guid.error());
    }

    DistroList result;
    for (const std::wstring& guid : *guids) {
        KeyReader key{registry, std::wstring{lxss_key_path} + L"\\" + guid};

        const auto name = key.text(L"DistributionName");
        const auto base_path = key.text(L"BasePath");
        const auto vhd_file_name = key.text(L"VhdFileName");
        const auto flavor = key.text(L"Flavor");
        const auto os_version = key.text(L"OsVersion");
        const auto version = key.number(L"Version", 2);
        const auto default_uid = key.number(L"DefaultUid", 0);
        const auto flags = key.number(L"Flags", 0);
        const auto state = key.number(L"State", 0);
        const auto modern = key.number(L"Modern", 0);
        if (key.failure()) {
            return std::unexpected(*key.failure());
        }

        // A key with no name is not a distribution, and one with no BasePath
        // names no disk. `orphans` reports these; enumeration only has to
        // survive them, because one unusable key must not stop the command
        // reporting every distribution that is fine.
        //
        // Empty counts as absent. A value can be present and blank -- an
        // interrupted install writes the key before it writes the name -- and a
        // distribution with an empty name is one `wsl.exe` cannot be asked
        // about, while an empty BasePath resolves to a bare `ext4.vhdx`
        // relative to whatever the process's current directory happens to be.
        if (!name.has_value() || name->empty()) {
            result.warnings.push_back(
                std::format("registry key {} has no DistributionName and was skipped", to_utf8(guid)));
            continue;
        }
        if (!base_path.has_value() || base_path->empty()) {
            result.warnings.push_back(
                std::format("distribution {} has no BasePath and was skipped", to_utf8(*name)));
            continue;
        }

        result.distros.push_back(
            Distro{.name = to_utf8(*name),
                   .guid = to_utf8(guid),
                   .version = version,
                   .base_path = *base_path,
                   .vhdx_path = vhdx_path_for(*base_path, vhd_file_name.value_or(std::wstring{})),
                   .default_uid = default_uid,
                   .flags = flags,
                   .state = state,
                   .is_default = default_guid->has_value() && **default_guid == guid,
                   .modern = modern != 0,
                   .flavor = to_utf8(flavor.value_or(std::wstring{})),
                   .os_version = to_utf8(os_version.value_or(std::wstring{}))});
    }
    return result;
}

}  // namespace wsldisk::model
