# Architecture

## Layout

```text
wsldisk/
├── CMakeLists.txt, CMakePresets.json, vcpkg.json
├── src/
│   ├── lib/                        # libwsldisk (static, no console I/O)
│   │   ├── interfaces.h            # IRegistry, IVirtualDisk, IWslHost, IFileSystem, IClock
│   │   ├── errors.{h,cpp}          # ErrorCode → exit code, Result/Status aliases
│   │   ├── platform/               # the only code that touches Win32
│   │   │   ├── win32_api.{h,cpp}   # the fault-injection table everything below calls through
│   │   │   ├── win32_error.{h,cpp} # DWORD → Error, with a remedy
│   │   │   ├── scoped_handle.h     # RAII for HANDLE, checks null *and* INVALID_HANDLE_VALUE
│   │   │   ├── registry.{h,cpp}    # HKCU\...\Lxss
│   │   │   ├── virtual_disk.{h,cpp}# OpenVirtualDisk V2 + CompactVirtualDisk
│   │   │   ├── filesystem.{h,cpp}  # sizes, sparse ranges, scans, text files, is_locked
│   │   │   ├── wsl_host.{h,cpp}    # wsl.exe process wrapper
│   │   │   ├── editor.{h,cpp}      # %EDITOR% for `config edit`
│   │   │   └── clock.{h,cpp}
│   │   ├── model/                  # plain data and parsers; no Win32, no I/O
│   │   │   ├── distro.{h,cpp}      # Distro, DistroList, registry enumeration
│   │   │   ├── disk_info.{h,cpp}   # measurement, `df` parsing
│   │   │   ├── orphans.{h,cpp}     # canonical paths, scan patterns, find_orphans
│   │   │   ├── config.{h,cpp}      # config.toml and .wslconfig
│   │   │   ├── size.{h,cpp}        # parse_size / format_size
│   │   │   ├── text.{h,cpp}        # UTF-8 ↔ UTF-16
│   │   │   └── wsl_output.{h,cpp}  # decoding what wsl.exe prints
│   │   └── ops/                    # one class per operation, interfaces only
│   │       ├── operation.h         # IOperation, Plan, Report, ProgressSink, UndoStack
│   │       ├── runner.{h,cpp}      # plan → execute → verify → rollback
│   │       ├── trim.{h,cpp}
│   │       ├── compact.{h,cpp}
│   │       └── relink.{h,cpp}
│   └── cli/                        # wsldisk.exe
│       ├── main.cpp, app.{h,cpp}   # argv → run(), and the top-level handler
│       ├── commands.{h,cpp}        # the whole CLI11 tree, built in one place
│       ├── *_command.{h,cpp}       # one pair per subcommand
│       ├── render.{h,cpp}          # Table, Details, JSON lines
│       ├── progress.{h,cpp}        # ConsoleSink
│       ├── lookup.{h,cpp}          # find a distribution by name, with "did you mean"
│       ├── preflight.{h,cpp}       # the shared WSL1 refusal
│       ├── options.{h,cpp}         # the flags every command shares
│       └── logger.{h,cpp}
├── tests/
│   ├── fakes/                      # FakeRegistry, FakeVirtualDisk, FakeWslHost,
│   │                               # FakeFileSystem, FakeClock, lxss_hives.h
│   ├── unit/                       # Catch2; mirrors src/ path for path
│   │   └── golden/                 # byte-compared output, incl. completion scripts
│   ├── contract/                   # the same wrappers against real Win32
│   ├── integration/                # real WSL2; ScratchDistro; WSLDISK_INTEGRATION=1
│   ├── fuzz/                       # libFuzzer targets + seed corpora
│   └── fixtures/                   # pinned Alpine rootfs manifest
├── scripts/                        # dev-shell, lint, coverage gate, fixtures
├── docs/
└── .github/workflows/
```

`commands.cpp` is worth singling out. It builds the entire CLI11 tree, and both
`run()` and `wsldisk completion` call it -- so the generated completion scripts
describe the flags that exist rather than a copy of them that can drift.

## Dependency rule

`cli → ops → interfaces ← platform`. Operations never include Win32 headers; they talk to
interfaces. `platform/` is the only place that includes `<windows.h>` and `<virtdisk.h>`. Tests use fakes implementing the interfaces.

## Operation lifecycle

```cpp
struct StepPlan { std::string description; bool mutates; std::optional<std::string> undo_description; };
struct Plan     { std::vector<StepPlan> steps; Estimate estimate; std::vector<Warning> warnings; };

class IOperation {
public:
    virtual Result<Plan>   plan()                           = 0;  // preflight, read-only
    virtual Result<Report> execute(ProgressSink&)           = 0;  // runs steps, pushes undo
    virtual Status         verify()                         = 0;
    virtual void           rollback(ProgressSink&) noexcept = 0;  // best effort, LIFO undo
};
```

`ops::run(operation, sink, options)` drives the lifecycle; an operation never
calls its own `rollback`. `--dry-run` stops after `plan()` and renders it. A
failure in `execute()` unwinds the undo stack.

A **verify** failure deliberately does *not* roll back. Execution reported
success, so the undo entries describe changes made on purpose; what failed is
the check that they added up to the intended result. Undoing on that signal
would turn "the tool is unsure" into "the tool changed your disk again".

`irreversible_steps_are_last(plan)` exists so an operation can assert its own
ordering in a test: once a point of no return has passed, a rollback can no
longer restore the starting state.

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

- `OpenVirtualDisk` for an unattached compact/resize uses `OPEN_VIRTUAL_DISK_VERSION_2` parameters with `VIRTUAL_DISK_ACCESS_NONE`, which is the only mask V2 accepts — `VIRTUAL_DISK_ACCESS_METAOPS` with V2 fails to open with `ERROR_INVALID_PARAMETER`. V1 parameters with `METAOPS` compact unelevated as well; V2 is preferred because a wrong mask fails at open rather than part-way through the operation (measured, docs/RESEARCH.md).
- `CompactVirtualDisk` supports `OVERLAPPED`; poll `GetVirtualDiskOperationProgress` every ~250 ms for the progress bar.
- `ResizeVirtualDisk`: prefer `RESIZE_VIRTUAL_DISK_FLAG_RESIZE_TO_SMALLEST_SAFE_VIRTUAL_SIZE` for shrink; **never** pass `ALLOW_UNSAFE_VIRTUAL_SIZE`.
- `GetVirtualDiskInformation` with `GET_VIRTUAL_DISK_INFO_SIZE` gives `VirtualSize`, `PhysicalSize`, `BlockSize`, `SectorSize`.
- Actual on-disk bytes: `GetCompressedFileSizeW` (accounts for NTFS sparse/compressed); sparseness: `FILE_ATTRIBUTE_SPARSE_FILE` + `FSCTL_QUERY_ALLOCATED_RANGES`.
- Registry: `HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\{GUID}` values `DistributionName` (REG_SZ), `BasePath` (REG_SZ, may be `\\?\C:\...`), `Version` (DWORD 1|2), `Flags` (DWORD), `DefaultUid` (DWORD), `VhdFileName` (REG_SZ, newer), `State`. `Lxss\DefaultDistribution` (REG_SZ GUID).
- **`wslapi.dll` is unusable.** Every entry point returns `E_ACCESSDENIED` from an unpackaged process -- even for a distribution name that does not exist -- so `WslLaunch`, `WslIsDistributionRegistered` and `WslGetDistributionConfiguration` are all out. Enumeration comes from the registry, and guest commands from `wsl.exe -d <d> -u root --exec ...` read as UTF-8 (measured, docs/RESEARCH.md).
- **`wsl --exec` does not search PATH**: `--exec blkid` fails with `execvpe(blkid) failed` even though the child environment has `/sbin` on PATH. Always pass an absolute path (`/sbin/fstrim`, `/sbin/e2fsck`). Guest tooling is not a given either — a stock Alpine rootfs has busybox `blkid`/`blockdev`/`fstrim` but no e2fsprogs.
- `wsl.exe` output is UTF-16LE (often with BOM) and localized. Parse only `--list --verbose`-style tabular output by column, never by header text; prefer registry.

## Error handling

**Nothing in `src/` throws.** Every fallible call returns `Result<T>`
(`std::expected<T, Error>`) or `Status` (`std::expected<void, Error>`), and
`platform/` converts each Win32 failure into `Error{code, message, remedy}` at
the point where it still knows what was being attempted -- which is the only
place that can write a useful remedy.

`main_entry` is `noexcept` and catches anything the standard library might throw
(a broken stream, an allocation failure), because an exception escaping `main`
surfaces as a crash dialog rather than a message.

`Error::remedy` may be empty only when no action helps. `error_from_win32`
leaves it empty for a code it does not map, so call sites that know more --
"run wsl.exe" knows the machine may have no WSL -- fill it in.

The CLI maps `code` to the process exit code; the table is in
[JSON.md](JSON.md#exit-codes) and is part of the interface.
and prints `remedy` — every error should tell the user what to do next.
