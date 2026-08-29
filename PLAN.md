# wsldisk — Project Plan

_Last updated: 2026-08-29_

## 1. Problem statement

WSL2 stores each distribution's root filesystem in a dynamically expanding VHDX
(`%LOCALAPPDATA%\Packages\<pkg>\LocalState\ext4.vhdx`, or a custom `--location`).
Docker Desktop, Rancher Desktop and Podman Desktop do the same for their data volumes.
These files:

- grow whenever the guest writes and **never shrink** when the guest deletes;
- can only be compacted with Hyper-V's `Optimize-VHD` (absent on Windows Home) or a
  manual `diskpart` script, both requiring `wsl --shutdown` first;
- cannot have their *maximum* size reduced without a `resize2fs` dance nobody documents well;
- can be moved only via `wsl --export`/`--import` (slow, loses default user, breaks
  `wsl --set-default`) or by hand-editing the registry;
- have no snapshot mechanism beyond full tar exports;
- have a Microsoft-provided sparse mode (`--set-sparse`) that as of WSL 2.5.6 requires
  `--allow-unsafe` because of data-corruption reports.

The result is a landscape of ~10 partially maintained PowerShell scripts, each solving one
piece (see [docs/RESEARCH.md](docs/RESEARCH.md)). Users routinely lose 50–200 GB to bloated
VHDX files without knowing it.

## 2. Goals

1. One native CLI that covers **inspect → compact → shrink/grow → move → snapshot/restore → diagnose**.
2. Works on **every Windows edition** that runs WSL2 (Home included) by calling the Virtual
   Disk Service API directly.
3. **Safe**: cannot corrupt a running distro; dry-run everywhere; verification after every
   destructive step; clear rollback story.
4. **Fast**: compaction via the API is I/O bound only; move is a file move + registry write.
5. **Scriptable & observable**: `--json`, meaningful exit codes, `--verbose` tracing.
6. Distributable as a single `wsldisk.exe` via winget/scoop/GitHub Releases.

### Non-goals (v1)

- GUI (a tray app / WinUI front-end is a possible v2 — the core is designed as a library to allow it).
- Managing WSL1 distros (they are directories, not VHDX; `list` will show them, other commands refuse).
- Replacing `wsl.exe` for install/uninstall/run.
- Managing generic Hyper-V VMs' disks (may work incidentally via `mount`/`compact --file`, not a target).
- Linux-side agent that must be installed inside the distro. All guest work is done by
  invoking standard tools (`fstrim`, `e2fsck`, `resize2fs`, `df`) through the WSL launch API.

## 3. Target users & user stories

**Developer on a laptop** — "My C: drive has 4 GB free. Show me where the space is and give it back."
→ `wsldisk list`, `wsldisk compact --all`.

**Docker Desktop user** — "`docker system prune` freed 60 GB but Windows still shows it used."
→ `wsldisk compact docker-desktop-data` (Docker Desktop's VHDX is discovered automatically).

**Power user with a small SSD** — "Move Ubuntu to D: without re-importing and losing my default user."
→ `wsldisk move Ubuntu D:\WSL`.

**Someone who set a 1 TB max by mistake** — "Cap the disk at 100 GB."
→ `wsldisk shrink Ubuntu --to 100G` (runs `resize2fs` for you, refuses if data doesn't fit).

**Cautious user** — "Snapshot before I upgrade the distro, restore if it breaks."
→ `wsldisk snapshot Ubuntu -m "pre-24.04"`, `wsldisk restore Ubuntu <id>`.

**Someone who ran `--set-sparse --allow-unsafe`** — "Why is my disk not shrinking / why did I get corruption?"
→ `wsldisk doctor` flags sparse distros and offers to turn it off and compact instead.

**CI / fleet admin** — "Nightly job: compact every distro on every dev box, log JSON."
→ `wsldisk compact --all --json --quiet` from Task Scheduler; `wsldisk schedule` helper.

## 4. Feature specification

### 4.1 `list` (M1)

Enumerate all WSL2 disks and print a table.

Sources:
- Registry `HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\{GUID}`: `DistributionName`, `BasePath`, `Version`, `Flags`, `DefaultUid`, `VhdFileName` (present in newer WSL).
- `Lxss\DefaultDistribution` for the default marker.
- Docker Desktop / Rancher / Podman: they register as regular distros (`docker-desktop`, `docker-desktop-data`, `rancher-desktop-data`, `podman-machine-*`), so no special casing beyond labeling.
- Optional `--scan <dir>` to find orphaned `*.vhdx` not referenced by any registry entry.

Columns: name, default marker, WSL version, state (running/stopped via `wsl -l -v` or `WslIsDistributionRegistered`+running check), VHDX path, virtual (max) size, actual size on disk (`GetCompressedFileSize` — respects NTFS sparse/compressed), guest used/free (via `df` in the distro, only if running or `--probe`), sparse flag (registry `Flags` bit / `FSCTL_QUERY_ALLOCATED_RANGES`), reclaimable estimate = actual − guest used.

### 4.2 `compact` (M1)

Reclaim unused space in the VHDX without changing its maximum size.

Workflow:
1. Preflight: distro exists, is WSL2, VHDX not currently attached (check via `GetVirtualDiskPhysicalPath` / open with exclusive access), enough free space on host volume for the operation.
2. Unless `--no-trim`: start distro, run `fstrim -av` (or `fstrim /`) as root via `WslLaunch` with uid 0. Record output. This converts deleted-but-allocated ext4 blocks into discards that zero/unmap the VHDX blocks.
3. `wsl --terminate <distro>` (only this distro — do not `--shutdown` others unless `--all`). Wait for the VHDX handle to become available (poll with timeout).
4. Open the VHDX with `OpenVirtualDisk` (`VIRTUAL_DISK_ACCESS_METAOPS`), attach read-only with `ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY | NO_DRIVE_LETTER | NO_LOCAL_HOST` when elevated (enables the "full" compaction mode that consults the file system bitmap); otherwise fall back to unattached `CompactVirtualDisk` (zero-block mode, still effective after `fstrim`).
5. `CompactVirtualDisk` with `OVERLAPPED` + `GetVirtualDiskOperationProgress` polling → progress bar.
6. Detach, close. Report before/after actual size, elapsed, method used.
7. Optionally restart the distro if it was running before (`--restart`).

Flags: `--all`, `--no-trim`, `--dry-run`, `--json`, `--file <path.vhdx>` (compact an arbitrary VHDX, e.g. detached Docker volume), `--restart`.

Non-elevated behavior: `CompactVirtualDisk` on an unattached disk works without admin. Attach requires admin. Strategy: attempt unelevated; if attach is required for full mode and `--elevate` is set (or user is prompted in interactive mode), relaunch self with `runas` and stream results back through a named pipe.

### 4.3 `shrink` / `grow` (M2)

Change the *maximum* virtual size.

`grow <distro> --to <size>`:
1. Terminate distro. `ResizeVirtualDisk` to new size.
2. Start distro, run `resize2fs <rootdev>` (root device discovered via `findmnt -no SOURCE /`; in WSL2 it is `/dev/sdX`). If the disk is partitioned (rare; some imports), refuse with guidance.
3. Verify `df` reflects new size.

Equivalent to `wsl --manage <distro> --resize <size>` in WSL ≥ 2.5 but also fixes the filesystem, which `wsl.exe` does not.

`shrink <distro> --to <size>`:
1. Preflight: guest used bytes + safety margin (default 10%, min 2 GB) ≤ target, else refuse.
2. Start distro. `e2fsck -f -y` requires the fs unmounted — root is mounted. Approach: attach the VHDX **read-write** to a *helper* distro (or the same distro via `wsl --mount --vhd --bare`) while the owning distro is terminated, then run `e2fsck -f` and `resize2fs <dev> <size>` from there. This is the same mechanism `wsl --mount --vhd` uses and needs no admin when done through `wsl.exe --mount`; direct `AttachVirtualDisk` path needs admin.
3. Detach. `ResizeVirtualDisk` with `RESIZE_VIRTUAL_DISK_FLAG_RESIZE_TO_SMALLEST_SAFE_VIRTUAL_SIZE` or explicit size (never below the fs size; the API refuses unsafe shrink unless `ALLOW_UNSAFE_VIRTUAL_SIZE`, which we never pass).
4. Compact (shrink leaves the file large otherwise).
5. Verify by mounting read-only and running `e2fsck -n`.

Helper distro: a tiny built-in busybox/Alpine rootfs (~5 MB) that `wsldisk` can import on demand as `wsldisk-helper` and remove afterwards; opt-in with `--helper` or auto if no other distro is available. Alternative: use any other registered running distro the user names with `--via <distro>`.

### 4.4 `move` (M2)

Relocate a distro's VHDX to a new directory/drive.

1. Preflight: target volume is NTFS (ReFS also OK; FAT/exFAT refused — no sparse, 4 GB limit), free space ≥ actual size, distro terminated, not a Store-packaged install being moved *into* a packaged folder.
2. Copy with `CopyFileEx` (progress callback, preserves sparse ranges via `COPY_FILE_ALLOW_DECRYPTED_DESTINATION`; fall back to manual block copy with `FSCTL_SET_SPARSE`+`FSCTL_QUERY_ALLOCATED_RANGES` to preserve sparseness).
3. Verify: size match + optional `--verify` full hash (BLAKE3/xxh3, streamed).
4. Update registry `BasePath` (and `VhdFileName` if present). Note: `BasePath` in the registry uses the `\\?\` prefix for newer distros — preserve whatever form was there.
5. Start distro, run `true`; if start fails, restore old registry value and keep both copies. If success, delete source (unless `--keep-source`).
6. Same-volume move: just `MoveFileEx` + registry write (instant).

Also expose `wsldisk relink <distro> <path>` — registry-only repoint for users who already moved the file manually.

### 4.5 `snapshot` / `restore` / `snapshots` (M3)

Snapshot backends, selected per repo config:

- **`copy`** (default): terminate distro, copy VHDX to `<repo>\<distro>\<timestamp>.vhdx` preserving sparseness, write `manifest.json` (distro metadata: name, GUID, default uid, flags, WSL version, size, hash). Restore = copy back + registry fix-up, or `--as <newname>` to import as a new distro (registry entry + new GUID).
- **`tar`**: `wsl --export` (respects `--format tar.gz/tar.xz/vhd` on newer WSL). Slower, portable, importable with plain `wsl.exe`.
- **`differencing`**: create a child VHDX (`CreateVirtualDisk` with `ParentPath`) and repoint the distro to the child — instant snapshot, copy-on-write. Restore = delete child, repoint to parent. Power feature; requires care because WSL must be pointed at the leaf. Listed as *experimental* in v1.
- **Dedup/incremental** via external tool (`restic`/`borg`/`kopia`) — `wsldisk` produces a stable, sparse VHDX and hands off; hook scripts. Not built in.

Retention: `wsldisk snapshot --keep-last 5 --keep-daily 7`. Scheduling: `wsldisk schedule install --daily 03:00` registers a Task Scheduler job.

### 4.6 `doctor` (M3)

Checks and, with `--fix`, remediation:

- Sparse flag set on a distro → warn (corruption reports, WSL #12103), offer `--set-sparse false` + compact.
- Orphaned VHDX files in common locations not referenced by registry → offer delete / relink.
- Registry `BasePath` pointing to a missing file → offer relink.
- VHDX actual size > guest used + 20% → recommend compact.
- Virtual size near guest used → recommend grow.
- `.wslconfig` sanity: `memory`, `swap`, `swapFile` path exists, `vhdSize` default.
- Host volume free space < 10% or < 2× largest VHDX.
- WSL version and whether `--manage --resize` etc. are available (feature detection).

### 4.7 `mount` / `unmount` (M3)

`wsldisk mount <file.vhdx> [--into <distro>] [--rw]` — thin, safer wrapper around `wsl --mount --vhd` with auto-detection of partitions/filesystem and a read-only default. Used internally by `shrink`.

### 4.8 Global behaviour

- `--json` on every command → machine-readable object, one per line for `--all`.
- `--dry-run` prints the plan and preflight results, changes nothing.
- `--yes` skips confirmations; interactive prompts only when stdin is a TTY.
- Exit codes: 0 ok, 1 generic error, 2 usage, 3 preflight failed (nothing changed), 4 needs elevation, 5 partially completed (see output), 10 distro not found, 11 distro running/locked.
- Logging: `--verbose`/`-v`, `--log <file>`; ETW provider optional later.
- Locale: English only in v1; all strings in one table for later i18n.

## 5. Technical approach

### 5.1 Language & toolchain

- **C++23** with MSVC (VS 2022 17.10+); clang-cl kept building in CI for diagnostics.
- CMake ≥ 3.28 presets, vcpkg manifest mode, Ninja.
- Static CRT (`/MT`) for a self-contained binary; x64 and ARM64 builds.
- Warnings as errors, `/W4 /permissive- /utf-8 /Zc:preprocessor`, clang-tidy, ASan in CI debug builds.

### 5.2 Libraries

| Purpose | Library | Notes |
|---|---|---|
| Win32 RAII/error handling | [WIL](https://github.com/microsoft/wil) | `wil::unique_handle`, `THROW_IF_WIN32_ERROR`, registry helpers. Used by WSL itself. |
| CLI | [CLI11](https://github.com/CLIUtils/CLI11) | Subcommands, validators, config file support |
| Formatting | `std::format` / [fmt](https://github.com/fmtlib/fmt) | fmt for older toolchains, colors |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) | `--json` output, manifests |
| Config | [toml++](https://github.com/marzer/tomlplusplus) | `%APPDATA%\wsldisk\config.toml` |
| Hashing | [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) (C impl) or xxHash | move/snapshot verification |
| Tests | [Catch2](https://github.com/catchorg/Catch2) v3 | unit + integration |
| Progress/TUI | [indicators](https://github.com/p-ranav/indicators) | progress bars; FTXUI later if a TUI is added |

### 5.3 Key Windows APIs

- **Virtual Disk Service** (`virtdisk.h`, `VirtDisk.lib`): `OpenVirtualDisk`, `AttachVirtualDisk`, `DetachVirtualDisk`, `CompactVirtualDisk`, `ResizeVirtualDisk`, `CreateVirtualDisk` (differencing), `GetVirtualDiskInformation` (size, physical size, parent, identifier), `GetVirtualDiskOperationProgress`, `GetVirtualDiskPhysicalPath`.
- **Registry**: `HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss` — distro enumeration and `BasePath` repointing.
- **WSL API** (`wslapi.h`, `wslapi.dll`): `WslIsDistributionRegistered`, `WslGetDistributionConfiguration`, `WslLaunch` (run `fstrim`/`resize2fs` as uid 0 with piped stdout). For features not in `wslapi.dll` (terminate, mount, manage), shell out to `wsl.exe` with UTF-16 output parsing, isolated behind an interface so it can be swapped for the COM `ILxssUserSession` surface from the open-sourced [microsoft/WSL](https://github.com/microsoft/WSL) later.
- **File system**: `CopyFileEx`, `MoveFileEx`, `GetCompressedFileSizeW`, `FSCTL_QUERY_ALLOCATED_RANGES`, `FSCTL_SET_SPARSE`, `GetDiskFreeSpaceEx`, `GetVolumeInformation` (fs type).
- **Elevation**: `ShellExecuteEx` with `runas`, named pipe for IPC; `CheckTokenMembership` to detect admin.
- **Task Scheduler**: `ITaskService` COM for `schedule`.

### 5.4 Architecture

Library-first: `libwsldisk` (static lib, no I/O to console) + `wsldisk.exe` (CLI) + tests. Detailed module breakdown in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). All Windows/WSL calls go through small interfaces (`IVirtualDisk`, `IWslHost`, `IRegistry`, `IFileSystem`) so the operation logic (`CompactOperation`, `MoveOperation`, …) is unit-testable with fakes.

Every operation is modeled as: `Plan` (preflight, produces a list of steps + estimated effect, used by `--dry-run`) → `Execute(progress sink)` → `Verify` → `Result`. Steps that mutate state carry an `undo` where possible (registry writes, file moves).

### 5.5 Testing strategy

- **Unit**: operations against fakes (registry with canned distros, fake virtual disk that tracks sizes).
- **Integration** (Windows runner, needs WSL): create a throwaway Alpine distro via `wsl --import` from a tiny rootfs checked into test fixtures (or downloaded and cached), fill it with data, delete, run `compact`, assert size dropped; `move` it to a temp dir and back; `shrink`/`grow`; `snapshot`/`restore`. GitHub-hosted `windows-2025` runners support nested virtualization and WSL2 — validate early (M0); fall back to a self-hosted runner if not.
- **VHDX-only tests** without WSL: create VHDX with `CreateVirtualDisk`, format via `diskpart`-free path (attach + `FormatEx` is messy — instead just test compact/resize on raw VHDX with written zero/non-zero blocks).
- **Property/fuzz**: registry value parsing, `wsl.exe` output parsing (UTF-16LE with BOM, localized text — never parse localized strings; use `--json`-less structured sources where possible).

### 5.6 Distribution

- GitHub Releases: `wsldisk-<ver>-x64.zip`, `-arm64.zip`, `.msix`? (no — keep plain exe + optional MSI later). Signed with Sigstore/cosign attestations; Authenticode signing if a certificate becomes available (SmartScreen).
- winget manifest (`zcsizmadia.wsldisk`), scoop bucket entry, Chocolatey optional.
- SBOM via vcpkg export; Dependabot for vcpkg baseline.

## 6. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Corrupting a user's distro (shrink/move/snapshot) | Catastrophic | Never operate on attached disks; verify after each step; keep source until verified; `e2fsck -n` post-checks; loud `--dry-run` culture; integration tests on throwaway distros before every release |
| `wsl.exe` output parsing is localized/fragile | Medium | Prefer registry + `wslapi.dll` + WSL COM; only parse `wsl.exe` where unavoidable and tolerate UTF-16 BOM, CRLF, localized headers by column position/heuristics; feature-detect WSL version |
| WSL internal COM interfaces change between releases | Medium | Abstract behind `IWslHost`; ship `wsl.exe` fallback |
| Store-packaged WSL vs inbox WSL differences (paths, `VhdFileName`, `\\?\` prefixes) | Medium | Test matrix across WSL 1.x inbox, 2.x store; read both `BasePath` and `VhdFileName` |
| Admin requirement for attach | UX | Unelevated compact path works post-`fstrim`; elevate only for full mode/shrink and explain why |
| Docker Desktop holds its VHDX open even when "stopped" | UX | Detect lock, tell user to quit Docker Desktop (`com.docker.backend`), offer `--wait` |
| Sparse-mode distros behave differently (file is NTFS-sparse; sizes misleading) | Medium | Use `GetCompressedFileSize` and allocated ranges; show sparse flag; doctor warning |
| Limited Windows CI for WSL integration tests | Schedule | Validate GitHub runner nested virt in M0; self-hosted fallback |
| Scope creep into GUI/agent | Schedule | Non-goals section; library-first so GUI can come later without rewrite |

## 7. Decisions log

| # | Decision | Rationale |
|---|---|---|
| D1 | Modern C++ over Rust/Go | Whole job is Win32/COM (`virtdisk.h`, registry, WSL COM); WSL itself is C++; WIL makes it ergonomic; single static binary |
| D2 | Library-first (`libwsldisk`) | Enables tests with fakes and a future GUI |
| D3 | Virtual Disk API, not `diskpart`/`Optimize-VHD` | Works on Home; progress reporting; no text scraping |
| D4 | Move via registry `BasePath`, not export/import | Instant, preserves default user/flags/GUID |
| D5 | Never enable sparse mode on behalf of the user | Data-corruption reports; compact is the safe alternative |
| D6 | `copy` snapshot backend default; differencing disks experimental | Simplicity and safety first |
| D7 | MIT license | Maximum adoption; compatible with WIL (MIT) and all chosen deps |

## 8. Open questions

- Should `shrink` require the helper distro, or is `wsl --mount --vhd --bare` into the *same* distro (after terminate) sufficient in all WSL ≥ 2.0 versions? Prototype in M0.
- Can `WslLaunch` reliably run as uid 0 when the distro's default user is non-root and `sudo` isn't passwordless? (It takes a `useCurrentWorkingDirectory` and uses the default uid — need `wsl -u root` fallback.)
- Does `CompactVirtualDisk` on an unattached disk reclaim blocks zeroed by `fstrim`'s discard, or does WSL translate discard to `FSCTL_SET_ZERO_DATA`/unmap such that nothing further is needed? Measure in M0.
- ARM64 CI availability.
- Name collision check: `wsldisk` on winget/scoop/crates — confirm free before first release.

## 9. Success metrics (12 months after 1.0)

- 1k+ GitHub stars; winget installs tracked via release download counts.
- Zero data-loss issues attributable to `wsldisk`.
- Referenced from the microsoft/WSL docs/discussions as the recommended community tool for disk management.
