#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "errors.h"
#include "interfaces.h"

namespace wsldisk::testing {

/// An in-memory `IVirtualDisk`.
///
/// A disk is a `VirtualDiskInfo` plus what compaction should leave behind, so a
/// test can say "this 20 GB disk compacts to 2 GB" without a real VHDX. Every
/// call can be told to fail, and openings and compactions are recorded so an
/// operation's behaviour -- particularly that `--dry-run` compacts nothing -- can
/// be asserted rather than assumed.
class FakeVirtualDisk final : public IVirtualDisk {
public:
    FakeVirtualDisk() = default;

    /// `IVirtualDisk` deletes move to stop slicing through a base reference.
    /// This one is `final`, so there is nothing to slice.
    FakeVirtualDisk(FakeVirtualDisk&& other) noexcept
        : IVirtualDisk(),
          disks_(std::move(other.disks_)),
          open_failure_(std::move(other.open_failure_)),
          compact_failure_(std::move(other.compact_failure_)),
          information_failure_(std::move(other.information_failure_)),
          create_failure_(std::move(other.create_failure_)),
          opened_(std::move(other.opened_)),
          compacted_(std::move(other.compacted_)),
          created_(std::move(other.created_)) {}

    struct Disk {
        VirtualDiskInfo info;
        /// What `physical_size` becomes after a successful compaction.
        std::uint64_t physical_size_after_compact = 0;
        /// How many progress callbacks a compaction reports before finishing.
        int progress_steps = 2;
    };

    void add_disk(const std::filesystem::path& path, Disk disk) { disks_[path.wstring()] = std::move(disk); }

    void fail_open(Error error) { open_failure_ = std::move(error); }

    void fail_compact(Error error) { compact_failure_ = std::move(error); }

    /// Makes `information()` fail on a handle that opened. A disk can be
    /// openable and still refuse to describe itself.
    void fail_information(Error error) { information_failure_ = std::move(error); }

    void fail_create(Error error) { create_failure_ = std::move(error); }

    [[nodiscard]] const std::vector<std::wstring>& opened() const noexcept { return opened_; }

    [[nodiscard]] const std::vector<std::wstring>& compacted() const noexcept { return compacted_; }

    [[nodiscard]] const std::vector<std::wstring>& created() const noexcept { return created_; }

    /// State after any compactions, so a test can assert what a disk ended up as.
    [[nodiscard]] const Disk* disk(const std::filesystem::path& path) const {
        const auto it = disks_.find(path.wstring());
        return it == disks_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] Result<std::unique_ptr<IVirtualDiskHandle>> open(
        const std::filesystem::path& path) const override {
        if (open_failure_) {
            return std::unexpected(*open_failure_);
        }
        const auto it = disks_.find(path.wstring());
        if (it == disks_.end()) {
            return fail(ErrorCode::Preflight, "no such virtual disk",
                        "check that the path exists and is spelled correctly");
        }
        opened_.push_back(path.wstring());
        return std::make_unique<Handle>(*this, path.wstring());
    }

    [[nodiscard]] Status create(const std::filesystem::path& path,
                                std::uint64_t maximum_size) const override {
        if (create_failure_) {
            return std::unexpected(*create_failure_);
        }
        created_.push_back(path.wstring());
        Disk disk;
        disk.info.virtual_size = maximum_size;
        disks_[path.wstring()] = disk;
        return {};
    }

private:
    class Handle final : public IVirtualDiskHandle {
    public:
        Handle(const FakeVirtualDisk& owner, std::wstring path) : owner_(owner), path_(std::move(path)) {}

        [[nodiscard]] Result<VirtualDiskInfo> information() const override {
            if (owner_.information_failure_) {
                return std::unexpected(*owner_.information_failure_);
            }
            return owner_.disks_.at(path_).info;
        }

        [[nodiscard]] Status compact(const ProgressCallback& progress) override {
            if (owner_.compact_failure_) {
                return std::unexpected(*owner_.compact_failure_);
            }
            Disk& disk = owner_.disks_.at(path_);
            for (int step = 1; step <= disk.progress_steps; ++step) {
                if (!progress(DiskProgress{.current = static_cast<std::uint64_t>(step),
                                           .total = static_cast<std::uint64_t>(disk.progress_steps)})) {
                    return fail(ErrorCode::Partial, "compaction was cancelled",
                                "re-run to finish reclaiming space");
                }
            }
            disk.info.physical_size = disk.physical_size_after_compact;
            owner_.compacted_.push_back(path_);
            return {};
        }

    private:
        const FakeVirtualDisk& owner_;
        std::wstring path_;
    };

    // Mutable so the const `open`/`create` of the interface can still record
    // what happened; the fake is a test double, not a value type.
    mutable std::map<std::wstring, Disk> disks_;
    std::optional<Error> open_failure_;
    std::optional<Error> compact_failure_;
    std::optional<Error> information_failure_;
    std::optional<Error> create_failure_;
    mutable std::vector<std::wstring> opened_;
    mutable std::vector<std::wstring> compacted_;
    mutable std::vector<std::wstring> created_;
};

}  // namespace wsldisk::testing
