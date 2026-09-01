#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "errors.h"
#include "interfaces.h"

namespace wsldisk::testing {

/// An in-memory `IFileSystem`.
///
/// A test describes files by their sizes and, where it matters, their allocated
/// ranges, so "a 1 TiB disk occupying 14 GiB on a volume with 8 GiB free" is
/// three lines rather than a fixture. Deletions are recorded so an operation can
/// be asserted to have removed nothing on `--dry-run`.
class FakeFileSystem final : public IFileSystem {
public:
    FakeFileSystem() = default;

    /// `IFileSystem` deletes move to stop slicing through a base reference. This
    /// one is `final`, so there is nothing to slice.
    FakeFileSystem(FakeFileSystem&& other) noexcept
        : IFileSystem(),
          files_(std::move(other.files_)),
          volumes_(std::move(other.volumes_)),
          variables_(std::move(other.variables_)),
          failure_(std::move(other.failure_)),
          remove_failure_(std::move(other.remove_failure_)),
          size_on_disk_failure_(std::move(other.size_on_disk_failure_)),
          directory_failures_(std::move(other.directory_failures_)),
          write_failure_(std::move(other.write_failure_)),
          text_(std::move(other.text_)),
          created_directories_(std::move(other.created_directories_)),
          variable_failures_(std::move(other.variable_failures_)),
          locked_(std::move(other.locked_)),
          removed_(std::move(other.removed_)) {}

    struct File {
        std::uint64_t size = 0;
        /// Defaults to `size` when left alone: a file that is not sparse
        /// occupies what it says it does.
        std::optional<std::uint64_t> size_on_disk;
        bool sparse = false;
        bool is_directory = false;
        /// Left empty for a non-sparse file, which reports one range covering
        /// the whole length.
        std::vector<AllocatedRange> ranges;
    };

    void add_file(const std::filesystem::path& path, File file) { files_[path.wstring()] = std::move(file); }

    void add_directory(const std::filesystem::path& path) {
        File directory;
        directory.is_directory = true;
        files_[path.wstring()] = directory;
    }

    void set_volume(const std::filesystem::path& root, VolumeInfo info) {
        volumes_[root.wstring()] = std::move(info);
    }

    /// One `%NAME%` expansion. Anything not set expands to itself.
    void set_variable(std::wstring name, std::wstring value) {
        variables_[std::move(name)] = std::move(value);
    }

    /// Makes every query fail, for the "the volume went away" paths.
    void fail_queries(Error error) { failure_ = std::move(error); }

    void fail_remove(Error error) { remove_failure_ = std::move(error); }

    /// Makes the size lookup fail while the directory listing still works --
    /// a file deleted between the scan and the measurement.
    void fail_size_on_disk(Error error) { size_on_disk_failure_ = std::move(error); }

    /// Makes listing one directory fail, leaving every other listing working.
    ///
    /// A scan reads several directories and reports the ones it could not,
    /// rather than giving up. A fake that fails every listing can only ever
    /// exercise the give-up path.
    void fail_directory(const std::filesystem::path& directory, Error error) {
        directory_failures_.insert_or_assign(directory.wstring(), std::move(error));
    }

    [[nodiscard]] const std::vector<std::wstring>& removed() const noexcept { return removed_; }

    [[nodiscard]] bool exists(const std::filesystem::path& path) const override {
        return files_.contains(path.wstring());
    }

    [[nodiscard]] Result<std::uint64_t> file_size(const std::filesystem::path& path) const override {
        const auto file = find(path);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        return (*file)->size;
    }

    [[nodiscard]] Result<std::uint64_t> file_size_on_disk(const std::filesystem::path& path) const override {
        if (size_on_disk_failure_) {
            return std::unexpected(*size_on_disk_failure_);
        }
        const auto file = find(path);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        return (*file)->size_on_disk.value_or((*file)->size);
    }

    [[nodiscard]] Result<bool> is_sparse(const std::filesystem::path& path) const override {
        const auto file = find(path);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        return (*file)->sparse;
    }

    [[nodiscard]] Result<VolumeInfo> volume_info(const std::filesystem::path& path) const override {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        const auto volume = volumes_.find(path.root_path().wstring());
        if (volume == volumes_.end()) {
            return fail(ErrorCode::Preflight, "no such volume", "check the drive letter");
        }
        return volume->second;
    }

    [[nodiscard]] Result<std::vector<DirectoryEntry>> list_directory(
        const std::filesystem::path& directory, std::wstring_view pattern) const override {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        if (const auto named = directory_failures_.find(directory.wstring());
            named != directory_failures_.end()) {
            return std::unexpected(named->second);
        }
        std::vector<DirectoryEntry> entries;
        for (const auto& [path, file] : files_) {
            const std::filesystem::path candidate{path};
            if (candidate.parent_path() != directory) {
                continue;
            }
            if (!matches(candidate.filename().wstring(), pattern)) {
                continue;
            }
            entries.push_back(DirectoryEntry{.path = candidate,
                                             .size = file.is_directory ? 0 : file.size,
                                             .is_directory = file.is_directory});
        }
        return entries;
    }

    [[nodiscard]] Result<std::vector<AllocatedRange>> allocated_ranges(
        const std::filesystem::path& path) const override {
        const auto file = find(path);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        if (!(*file)->ranges.empty()) {
            return (*file)->ranges;
        }
        if ((*file)->size == 0) {
            return std::vector<AllocatedRange>{};
        }
        return std::vector<AllocatedRange>{AllocatedRange{.offset = 0, .length = (*file)->size}};
    }

    /// Marks a file as held open by something else.
    void lock_file(const std::filesystem::path& path) { locked_.insert(path.wstring()); }

    [[nodiscard]] Result<bool> is_locked(const std::filesystem::path& path) const override {
        const auto file = find(path);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        return locked_.contains(path.wstring());
    }

    [[nodiscard]] Status remove(const std::filesystem::path& path) override {
        if (remove_failure_) {
            return std::unexpected(*remove_failure_);
        }
        if (!files_.contains(path.wstring())) {
            return fail(ErrorCode::Preflight, "no such file", "check the path");
        }
        files_.erase(path.wstring());
        removed_.push_back(path.wstring());
        return {};
    }

    [[nodiscard]] Status copy_file_sparse(
        const std::filesystem::path& from, const std::filesystem::path& to,
        const std::function<bool(std::uint64_t copied, std::uint64_t total)>& progress) override {
        if (copy_failure_) {
            return std::unexpected(*copy_failure_);
        }
        const auto source = files_.find(from.wstring());
        if (source == files_.end()) {
            return fail(ErrorCode::Preflight, "no such file", "check the path");
        }
        if (files_.contains(to.wstring())) {
            return fail(ErrorCode::Preflight, "the destination already exists", "move it aside first");
        }

        // Reported in the same shape the real one reports it -- allocated bytes,
        // not logical length -- so a test can tell the two apart. A caller that
        // reads the total as the file size would look right against a fake that
        // used the file size.
        const std::uint64_t total = source->second.size_on_disk.value_or(source->second.size);
        for (const std::uint64_t mark : copy_progress_marks_) {
            if (!progress(std::min(mark, total), total)) {
                // The partial file is left behind, as the real one leaves it.
                files_[to.wstring()] = source->second;
                copied_.emplace_back(from.wstring(), to.wstring());
                return fail(ErrorCode::Partial, "the copy was cancelled", "delete the partial file");
            }
        }

        files_[to.wstring()] = source->second;
        if (const auto text = text_.find(from.wstring()); text != text_.end()) {
            text_[to.wstring()] = text->second;
        }
        copied_.emplace_back(from.wstring(), to.wstring());
        return {};
    }

    [[nodiscard]] Status rename(const std::filesystem::path& from, const std::filesystem::path& to) override {
        if (rename_failure_) {
            return std::unexpected(*rename_failure_);
        }
        const auto source = files_.find(from.wstring());
        if (source == files_.end()) {
            return fail(ErrorCode::Preflight, "no such file", "check the path");
        }
        if (files_.contains(to.wstring())) {
            return fail(ErrorCode::Preflight, "the destination already exists", "move it aside first");
        }
        files_[to.wstring()] = source->second;
        files_.erase(from.wstring());
        if (const auto text = text_.find(from.wstring()); text != text_.end()) {
            text_[to.wstring()] = text->second;
            text_.erase(from.wstring());
        }
        renamed_.emplace_back(from.wstring(), to.wstring());
        return {};
    }

    [[nodiscard]] Result<bool> same_volume(const std::filesystem::path& first,
                                           const std::filesystem::path& second) const override {
        if (same_volume_failure_) {
            return std::unexpected(*same_volume_failure_);
        }
        return first.root_path() == second.root_path();
    }

    /// Where `progress` is called during a copy, in bytes. Empty means it is
    /// never called, which is what a copy of nothing looks like.
    void set_copy_progress(std::vector<std::uint64_t> marks) { copy_progress_marks_ = std::move(marks); }

    void fail_copy(Error error) { copy_failure_ = std::move(error); }

    void fail_rename(Error error) { rename_failure_ = std::move(error); }

    void fail_same_volume(Error error) { same_volume_failure_ = std::move(error); }

    /// Every copy that was made, source then destination.
    [[nodiscard]] const std::vector<std::pair<std::wstring, std::wstring>>& copied() const noexcept {
        return copied_;
    }

    [[nodiscard]] const std::vector<std::pair<std::wstring, std::wstring>>& renamed() const noexcept {
        return renamed_;
    }

    /// Gives `path` text contents, creating it if it is not there.
    void add_text_file(const std::filesystem::path& path, std::string contents) {
        File file;
        file.size = contents.size();
        file.size_on_disk = contents.size();
        files_[path.wstring()] = file;
        text_[path.wstring()] = std::move(contents);
    }

    /// What a file holds now, after any writes.
    [[nodiscard]] std::optional<std::string> text_of(const std::filesystem::path& path) const {
        const auto found = text_.find(path.wstring());
        return found == text_.end() ? std::nullopt : std::optional{found->second};
    }

    void fail_write(Error error) { write_failure_ = std::move(error); }

    /// Makes expanding one `%NAME%` fail, leaving every other expansion working.
    ///
    /// A command can look up two variables and handle their failures
    /// differently -- `config` treats a missing `%APPDATA%` as fatal and a
    /// missing `%USERPROFILE%` as "there is no .wslconfig". Failing every
    /// expansion can only ever exercise the first.
    void fail_variable(std::wstring name, Error error) {
        variable_failures_.insert_or_assign(std::move(name), std::move(error));
    }

    /// Every directory `create_directories` was asked for, in order.
    [[nodiscard]] const std::vector<std::wstring>& created_directories() const noexcept {
        return created_directories_;
    }

    [[nodiscard]] Result<std::string> read_text_file(const std::filesystem::path& path) const override {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        const auto found = text_.find(path.wstring());
        if (found == text_.end()) {
            return fail(ErrorCode::Preflight, "no such file", "check that the path exists");
        }
        return found->second;
    }

    [[nodiscard]] Status write_text_file(const std::filesystem::path& path,
                                         std::string_view contents) override {
        if (write_failure_) {
            return std::unexpected(*write_failure_);
        }
        add_text_file(path, std::string{contents});
        return {};
    }

    [[nodiscard]] Status create_directories(const std::filesystem::path& path) override {
        if (write_failure_) {
            return std::unexpected(*write_failure_);
        }
        created_directories_.push_back(path.wstring());
        add_directory(path);
        return {};
    }

    [[nodiscard]] Result<std::filesystem::path> expand_environment(
        const std::filesystem::path& path) const override {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        std::wstring text = path.wstring();
        for (const auto& [name, error] : variable_failures_) {
            if (text.find(L"%" + name + L"%") != std::wstring::npos) {
                return std::unexpected(error);
            }
        }
        for (const auto& [name, value] : variables_) {
            const std::wstring token = L"%" + name + L"%";
            for (std::size_t at = text.find(token); at != std::wstring::npos;
                 at = text.find(token, at + value.size())) {
                text.replace(at, token.size(), value);
            }
        }
        return std::filesystem::path{text};
    }

private:
    [[nodiscard]] Result<const File*> find(const std::filesystem::path& path) const {
        if (failure_) {
            return std::unexpected(*failure_);
        }
        const auto file = files_.find(path.wstring());
        if (file == files_.end()) {
            return fail(ErrorCode::Preflight, "no such file", "check that the path exists");
        }
        return &file->second;
    }

    /// The subset of Win32 wildcards the scans actually use: `*` and `?`.
    [[nodiscard]] static bool matches(std::wstring_view name, std::wstring_view pattern) {
        if (pattern.empty()) {
            return true;
        }
        if (pattern.front() == L'*') {
            const std::wstring_view rest = pattern.substr(1);
            for (std::size_t at = 0; at <= name.size(); ++at) {
                if (matches(name.substr(at), rest)) {
                    return true;
                }
            }
            return false;
        }
        if (name.empty()) {
            return false;
        }
        if (pattern.front() != L'?' && pattern.front() != name.front()) {
            return false;
        }
        return matches(name.substr(1), pattern.substr(1));
    }

    std::map<std::wstring, File> files_;
    std::map<std::wstring, VolumeInfo> volumes_;
    std::map<std::wstring, std::wstring> variables_;
    std::optional<Error> failure_;
    std::optional<Error> remove_failure_;
    std::optional<Error> size_on_disk_failure_;
    std::map<std::wstring, Error> directory_failures_;
    std::optional<Error> write_failure_;
    std::map<std::wstring, std::string> text_;
    std::vector<std::wstring> created_directories_;
    std::map<std::wstring, Error> variable_failures_;
    std::set<std::wstring> locked_;
    std::vector<std::wstring> removed_;
    std::optional<Error> copy_failure_;
    std::optional<Error> rename_failure_;
    std::optional<Error> same_volume_failure_;
    std::vector<std::uint64_t> copy_progress_marks_;
    std::vector<std::pair<std::wstring, std::wstring>> copied_;
    std::vector<std::pair<std::wstring, std::wstring>> renamed_;
};

}  // namespace wsldisk::testing
