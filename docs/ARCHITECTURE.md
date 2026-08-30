# Architecture

## Layout

```text
wsldisk/
├── CMakeLists.txt, CMakePresets.json, vcpkg.json
├── src/
│   ├── lib/                     # libwsldisk (static library, no console I/O)
│   │   ├── platform/            # thin RAII wrappers over Win32/COM (WIL-based)
│   │   │   ├── virtual_disk.{h,cpp}   # OpenVirtualDisk/Compact/Resize/Attach/Detach/Create
│   │   │   ├── registry.{h,cpp}       # HKCU\...\Lxss access
│   │   │   ├── wsl_host.{h,cpp}       # wslapi.dll + wsl.exe process wrapper (+ COM later)
│   │   │   ├── filesystem.{h,cpp}     # sizes, sparse ranges, CopyFileEx, volume info
│   │   │   ├── elevation.{h,cpp}      # IsElevated, RelaunchElevated, pipe IPC
│   │   │   └── task_scheduler.{h,cpp}
│   │   ├── model/               # plain data: Distro, DiskInfo, Snapshot, Plan, Result
│   │   ├── ops/                 # one class per operation, depends only on interfaces
│   │   │   ├── operation.h            # Plan → Execute(ProgressSink&) → Verify → Result
│   │   │   ├── list_op.cpp
│   │   │   ├── compact_op.cpp
│   │   │   ├── resize_op.cpp          # grow + shrink
│   │   │   ├── move_op.cpp
│   │   │   ├── snapshot_op.cpp / restore_op.cpp
│   │   │   └── doctor_op.cpp
│   │   ├── interfaces.h         # IVirtualDisk, IWslHost, IRegistry, IFileSystem, IClock
│   │   └── errors.h             # error codes → exit codes, std::expected aliases
│   └── cli/                     # wsldisk.exe: CLI11 commands, table/JSON renderers, progress bars
├── tests/
│   ├── unit/                    # Catch2 + fakes/ (FakeRegistry, FakeVirtualDisk, ...)
│   ├── integration/             # requires WSL; gated by WSLDISK_INTEGRATION=1
│   └── fixtures/                # tiny Alpine rootfs tarball (or download script)
├── spikes/                      # M0 throwaway experiments (deleted or archived after M0)
├── docs/
├── packaging/                   # winget, scoop manifests; release scripts
└── .github/workflows/
```

## Dependency rule

`cli → ops → interfaces ← platform`. Operations never include Win32 headers; they talk to
interfaces. `platform/` is the only place that includes `<windows.h>`, `<virtdisk.h>`,
`<wslapi.h>`. Tests use fakes implementing the interfaces.

## Operation lifecycle

```cpp
struct StepPlan { std::string description; bool mutates; std::optional<std::string> undo_description; };
struct Plan     { std::vector<StepPlan> steps; Estimate estimate; std::vector<Warning> warnings; };

class IOperation {
public:
    virtual std::expected<Plan, Error>   plan()                          = 0;  // preflight, read-only
    virtual std::expected<Result, Error> execute(ProgressSink&)          = 0;  // runs steps, pushes undo
    virtual std::expected<void, Error>   verify()                        = 0;
    virtual void                         rollback(ProgressSink&) noexcept = 0; // best effort, LIFO undo
};
```

`--dry-run` calls `plan()` and renders it. `execute()` wraps each mutating step so that a failure
triggers `rollback()` automatically (registry write undo, file move undo). Steps that cannot be
undone (compaction, resize2fs shrink) are ordered last and preceded by a verify checkpoint.

## Key workflows

### compact

```text
preflight ──► fstrim / (as root) ──► terminate ──► wait unlock
          ──► still locked? ──► refuse (exit 11) unless --shutdown ──► wsl --shutdown
          ──► OpenVirtualDisk(V2 params, ACCESS_NONE) ──► CompactVirtualDisk(+progress)
          ──► measure ──► [restart]
```

The attach-read-only "full" mode is an opt-in for untrimmed disks, not part of this
path: after `fstrim` the unattached compaction already reclaims everything, without
administrator rights (PLAN.md D10).

### shrink

```text
terminate ──► wsl --mount --vhd --bare (any other running distro will do)
   ──► find the new /dev/sdX by diffing /proc/partitions
   ──► resize2fs -P ──► refuse if the target is below that floor
   ──► e2fsck -f -y ──► resize2fs <dev> <size> ──► wsl --unmount
   ──► ResizeVirtualDisk(size) ──► CompactVirtualDisk ──► mount RO + e2fsck -n ──► unmount
```

The fit check comes from `resize2fs -P`, not from the guest byte count: on a
default 1 TiB disk the floor is around 11 GiB however little is stored, because
the inode table was sized for 1 TiB.

### move

```text
preflight(fs, space, lock) ──► terminate ──► sparse-aware copy(+progress) ──► [hash verify]
   ──► registry BasePath (undoable) ──► start distro smoke test ──► delete source | rollback
```

## Windows API notes

- `OpenVirtualDisk` for an unattached compact/resize **must** use `OPEN_VIRTUAL_DISK_VERSION_2` parameters with `VIRTUAL_DISK_ACCESS_NONE`. `VIRTUAL_DISK_ACCESS_METAOPS` is not an alternative: with V1 or null parameters the handle opens and `CompactVirtualDisk` then returns `ERROR_ACCESS_DENIED`, and `METAOPS` combined with V2 parameters fails to open with `ERROR_INVALID_PARAMETER` (measured, docs/RESEARCH.md).
- `CompactVirtualDisk` supports `OVERLAPPED`; poll `GetVirtualDiskOperationProgress` every ~250 ms for the progress bar.
- `ResizeVirtualDisk`: prefer `RESIZE_VIRTUAL_DISK_FLAG_RESIZE_TO_SMALLEST_SAFE_VIRTUAL_SIZE` for shrink; **never** pass `ALLOW_UNSAFE_VIRTUAL_SIZE`.
- `GetVirtualDiskInformation` with `GET_VIRTUAL_DISK_INFO_SIZE` gives `VirtualSize`, `PhysicalSize`, `BlockSize`, `SectorSize`.
- Actual on-disk bytes: `GetCompressedFileSizeW` (accounts for NTFS sparse/compressed); sparseness: `FILE_ATTRIBUTE_SPARSE_FILE` + `FSCTL_QUERY_ALLOCATED_RANGES`.
- Registry: `HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\{GUID}` values `DistributionName` (REG_SZ), `BasePath` (REG_SZ, may be `\\?\C:\...`), `Version` (DWORD 1|2), `Flags` (DWORD), `DefaultUid` (DWORD), `VhdFileName` (REG_SZ, newer), `State`. `Lxss\DefaultDistribution` (REG_SZ GUID).
- `wslapi.dll`: `WslLaunch(distro, cmd, useCwd, stdin, stdout, stderr, &process)` — runs as the distro's default uid; for root use `wsl.exe -d <d> -u root --exec ...` and read UTF-8 stdout.
- **`wsl --exec` does not search PATH**: `--exec blkid` fails with `execvpe(blkid) failed` even though the child environment has `/sbin` on PATH. Always pass an absolute path (`/sbin/fstrim`, `/sbin/e2fsck`). Guest tooling is not a given either — a stock Alpine rootfs has busybox `blkid`/`blockdev`/`fstrim` but no e2fsprogs.
- `wsl.exe` output is UTF-16LE (often with BOM) and localized. Parse only `--list --verbose`-style tabular output by column, never by header text; prefer registry.

## Error handling

`platform/` throws `wil::ResultException` on unexpected Win32 failures and returns
`std::expected` for expected conditions (not found, locked, access denied). `ops/` convert
everything into `Error{code, message, remedy}`; the CLI maps `code` to exit codes (see PLAN §4.8)
and prints `remedy` — every error should tell the user what to do next.
