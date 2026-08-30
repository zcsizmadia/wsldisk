#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "errors.h"
#include "interfaces.h"

namespace wsldisk::testing {

/// An in-memory `IRegistry`.
///
/// Keys are flat paths (`L"Lxss\\{guid}"`), which is how the production code
/// addresses them, so nothing here has to model a tree. Any operation can be
/// told to fail, so callers can be tested against a registry that breaks
/// half-way through an enumeration.
class FakeRegistry final : public IRegistry {
public:
    using Value = std::variant<std::wstring, std::uint32_t>;

    FakeRegistry() = default;

    /// `IRegistry` deletes copy and move, which is right for a polymorphic base:
    /// it stops a derived object being sliced through a base reference. This one
    /// is `final`, so there is nothing to slice, and the hive builders in
    /// `lxss_hives.h` want to return a populated registry by value.
    FakeRegistry(FakeRegistry&& other) noexcept
        : IRegistry(),
          keys_(std::move(other.keys_)),
          failure_(std::move(other.failure_)),
          value_failures_(std::move(other.value_failures_)),
          write_failures_(std::move(other.write_failures_)),
          writes_(std::move(other.writes_)) {}

    FakeRegistry& operator=(FakeRegistry&& other) noexcept {
        keys_ = std::move(other.keys_);
        failure_ = std::move(other.failure_);
        value_failures_ = std::move(other.value_failures_);
        write_failures_ = std::move(other.write_failures_);
        writes_ = std::move(other.writes_);
        return *this;
    }

    /// Sets a value, creating the key if needed.
    void set(std::wstring key, std::wstring value, Value data) {
        keys_[key][std::move(value)] = std::move(data);
    }

    /// Creates a key with no values, so an empty subkey is representable.
    void add_key(std::wstring key) { keys_.try_emplace(std::move(key), ValueMap{}); }

    /// Makes every subsequent call fail with `error`.
    void fail_with(Error error) { failure_ = std::move(error); }

    /// Makes reads of one named value fail, leaving every other read working.
    ///
    /// Enumeration reads a dozen values per distribution and stops at the first
    /// failure, so a fake that fails everything can only ever exercise the first
    /// read. This reaches the others.
    void fail_value(std::wstring value, Error error) {
        value_failures_.insert_or_assign(std::move(value), std::move(error));
    }

    /// Makes writes of one named value fail, leaving every other write working.
    ///
    /// An operation writes more than one value and puts back what it already
    /// wrote when a later one fails. A fake that fails every write can only
    /// ever exercise the first, which is the case with nothing to undo.
    void fail_write(std::wstring value, Error error) {
        write_failures_.insert_or_assign(std::move(value), std::move(error));
    }

    /// Records every `write_string`, so an operation's rollback can be asserted.
    struct Write {
        std::wstring key;
        std::wstring value;
        std::wstring data;
    };

    [[nodiscard]] const std::vector<Write>& writes() const noexcept { return writes_; }

    [[nodiscard]] Result<std::vector<std::wstring>> subkeys(std::wstring_view key) const override {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        // Direct children only: "Lxss\{a}" is a subkey of "Lxss", "Lxss\{a}\b" is not.
        const std::wstring prefix = std::wstring{key} + L"\\";
        std::vector<std::wstring> names;
        for (const auto& [path, values] : keys_) {
            if (!path.starts_with(prefix)) {
                continue;
            }
            const std::wstring tail = path.substr(prefix.size());
            if (tail.find(L'\\') == std::wstring::npos) {
                names.push_back(tail);
            }
        }
        return names;
    }

    [[nodiscard]] Result<std::optional<std::wstring>> read_string(std::wstring_view key,
                                                                  std::wstring_view value) const override {
        return read<std::wstring>(key, value);
    }

    [[nodiscard]] Result<std::optional<std::uint32_t>> read_dword(std::wstring_view key,
                                                                  std::wstring_view value) const override {
        return read<std::uint32_t>(key, value);
    }

    [[nodiscard]] Status write_string(std::wstring_view key, std::wstring_view value,
                                      std::wstring_view data) override {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        if (const auto named = write_failures_.find(std::wstring{value}); named != write_failures_.end()) {
            return std::unexpected(named->second);
        }
        writes_.push_back({std::wstring{key}, std::wstring{value}, std::wstring{data}});
        keys_[std::wstring{key}][std::wstring{value}] = std::wstring{data};
        return {};
    }

private:
    using ValueMap = std::map<std::wstring, Value>;

    template <typename T>
    [[nodiscard]] Result<std::optional<T>> read(std::wstring_view key, std::wstring_view value) const {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        if (const auto named = value_failures_.find(std::wstring{value}); named != value_failures_.end()) {
            return std::unexpected(named->second);
        }
        const auto key_it = keys_.find(std::wstring{key});
        if (key_it == keys_.end()) {
            return fail(ErrorCode::Generic, "no such key", "check the key path");
        }
        const auto value_it = key_it->second.find(std::wstring{value});
        if (value_it == key_it->second.end()) {
            return std::optional<T>{};
        }
        if (!std::holds_alternative<T>(value_it->second)) {
            return fail(ErrorCode::Generic, "value has the wrong type",
                        "the WSL registry layout has changed");
        }
        return std::optional{std::get<T>(value_it->second)};
    }

    std::map<std::wstring, ValueMap> keys_;
    std::optional<Error> failure_;
    std::map<std::wstring, Error> value_failures_;
    std::map<std::wstring, Error> write_failures_;
    std::vector<Write> writes_;
};

}  // namespace wsldisk::testing
