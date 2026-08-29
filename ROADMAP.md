# wsldisk — Roadmap

_Last updated: 2026-08-29. Dates are targets, not promises._

Legend: `[ ]` todo · `[~]` in progress · `[x]` done

---

## M0 — Foundations & spikes (≈ 2 weeks)

Goal: de-risk the unknowns, have a compiling skeleton and CI.

**Repo & tooling**
- [ ] CMake presets (`x64-debug`, `x64-release`, `arm64-release`), vcpkg manifest, Ninja
- [ ] `libwsldisk` static lib + `wsldisk` exe + `tests` targets; `wsldisk --version` works
- [ ] `.clang-format`, `.clang-tidy`, `.editorconfig`, pre-commit hook script
- [ ] GitHub Actions: build (MSVC + clang-cl, x64 + arm64), unit tests, clang-tidy, ASan debug job
- [ ] Verify WSL2 works on `windows-2025` hosted runners (nested virt); decide integration-test strategy
- [ ] Issue/PR templates, CODEOWNERS, Dependabot for vcpkg baseline + actions

**Technical spikes** (throwaway code under `spikes/`, results recorded in `docs/RESEARCH.md`)
- [ ] `CompactVirtualDisk` unattached vs attached-RO: measure reclaimed bytes after `fstrim` on a 20 GB test distro
- [ ] Confirm `ResizeVirtualDisk` shrink path + `resize2fs` via `wsl --mount --vhd --bare` (same distro terminated)
- [ ] `WslLaunch` as uid 0 with non-root default user — does it work, or need `wsl.exe -u root`?
- [ ] Registry layout across WSL inbox 1.x / Store 2.x: `BasePath` prefix forms, `VhdFileName`, `Flags` bits (sparse)
- [ ] Docker Desktop VHDX lock behaviour when Docker is "stopped" vs quit
- [ ] Elevation relaunch + named-pipe result streaming prototype

**Exit criteria:** CI green, all spikes answered in RESEARCH.md, open questions in PLAN.md §8 resolved or converted to issues.

---

## M1 — `list` + `compact` (v0.1.0, ≈ 3 weeks)

Goal: the 80% use case — see where the space is and get it back — shippable.

- [ ] `IRegistry`, `IVirtualDisk`, `IWslHost`, `IFileSystem` interfaces + Win32 implementations + fakes
- [ ] Distro enumeration (registry) with WSL1/WSL2 detection and default marker
- [ ] Size probes: virtual size, actual on-disk (`GetCompressedFileSize`), sparse detection, guest `df` (`--probe`)
- [ ] `wsldisk list` table + `--json` + `--scan` for orphaned VHDX
- [ ] `Plan → Execute → Verify` operation framework with progress sink and undo stack
- [ ] `CompactOperation`: preflight, `fstrim`, terminate, wait-for-unlock, compact (unattached), report
- [ ] Attached-RO "full" compaction when elevated; `--elevate` relaunch
- [ ] `--all`, `--dry-run`, `--no-trim`, `--restart`, `--file <vhdx>`
- [ ] Exit code scheme, `-v` logging, `--log`
- [ ] Unit tests for planner/preflight; integration test: import Alpine, bloat, compact, assert shrink
- [ ] README usage docs, `docs/COMPACT.md` explaining what happens and why
- [ ] Release pipeline: tag → build → zip + SHA256 + provenance attestation → GitHub Release
- [ ] First winget + scoop manifests (may land in M2 if name check pending)

**Exit criteria:** A Windows Home user with no Hyper-V module can `winget install wsldisk` and reclaim space from Ubuntu and Docker Desktop with one command.

---

## M2 — `move`, `relink`, `grow`, `shrink` (v0.2.0, ≈ 4 weeks)

- [ ] `MoveOperation`: preflight (fs type, free space, lock), sparse-preserving copy with progress, hash `--verify`, registry repoint, start test, rollback, source cleanup; same-volume fast path
- [ ] `wsldisk relink <distro> <path>`
- [ ] `GrowOperation`: `ResizeVirtualDisk` + `resize2fs`; detect partitioned disks and refuse
- [ ] Helper-distro mechanism (tiny Alpine rootfs, on-demand import/remove) or `--via <distro>`
- [ ] `ShrinkOperation`: fit check with margin, `e2fsck -f`, `resize2fs <size>`, `ResizeVirtualDisk` (safe flag only), compact, `e2fsck -n` verify
- [ ] `wsldisk mount` / `unmount` wrappers (read-only default)
- [ ] Feature detection of `wsl.exe` capabilities (`--manage`, `--mount --vhd`, version)
- [ ] Integration tests for move (cross-volume via VHD-backed second volume in CI or `subst`), grow, shrink
- [ ] `docs/MOVE.md`, `docs/RESIZE.md`

**Exit criteria:** Full disk lifecycle (compact/grow/shrink/move) without touching `diskpart`, PowerShell or `wsl --export`.

---

## M3 — `snapshot`, `restore`, `doctor`, `schedule` (v0.3.0, ≈ 4 weeks)

- [ ] Snapshot repo layout + `manifest.json` schema; config in `%APPDATA%\wsldisk\config.toml`
- [ ] `copy` backend (sparse-preserving) with `--keep-last/--keep-daily/--keep-weekly` retention
- [ ] `tar` backend via `wsl --export` (format flags where supported)
- [ ] `restore` in place and `--as <newname>` (new GUID registry entry)
- [ ] `differencing` backend (experimental, behind `--experimental`)
- [ ] Hook scripts (`pre-snapshot`, `post-snapshot`) for restic/borg/kopia hand-off
- [ ] `wsldisk doctor` checks + `--fix` remediations (sparse warning, orphans, broken BasePath, bloat, capacity, `.wslconfig` sanity)
- [ ] `wsldisk schedule install|remove|status` via Task Scheduler COM
- [ ] Integration tests: snapshot → mutate → restore → verify content
- [ ] `docs/SNAPSHOTS.md`, `docs/DOCTOR.md`

**Exit criteria:** Nightly unattended compact + snapshot with retention on a dev machine, zero manual steps.

---

## M4 — Hardening & 1.0 (≈ 3 weeks)

- [ ] Test matrix: Windows 10 22H2, Windows 11 24H2/25H2; WSL inbox, WSL Store 2.x latest and latest-1; Home + Pro
- [ ] ARM64 build tested on real hardware
- [ ] Fuzz registry/`wsl.exe` output parsers (libFuzzer via clang-cl)
- [ ] Error-message review: every failure tells the user what to do next
- [ ] Localization-safe: never parse localized `wsl.exe` text on hot paths
- [ ] Performance: compact/move progress accuracy; large-disk (500 GB+) test
- [ ] Security review: elevation IPC, path handling (`\\?\`, junctions), no writes outside intended dirs
- [ ] Authenticode signing (if certificate available) / SmartScreen reputation plan
- [ ] `CHANGELOG.md`, semantic versioning policy, support policy (last two WSL releases)
- [ ] Announce: microsoft/WSL discussions, r/bashonubuntuonwindows, Hacker News, dev.to write-up

**Exit criteria:** v1.0.0 tagged; no known data-safety issues; winget/scoop current.

---

## Later / v2 ideas (unscheduled)

- Tray/WinUI 3 GUI on top of `libwsldisk` (space usage at a glance, one-click compact)
- `wsldisk stats` history + sparkline of disk growth over time
- Auto-compact policy daemon (compact when idle and reclaimable > N GB)
- Deduplicated snapshot backend built-in (content-defined chunking) instead of external tools
- Support Hyper-V VM disks and Windows Sandbox explicitly
- PowerShell module wrapper (`Get-WslDisk`, `Optimize-WslDisk`) generated from `--json`
- Upstream: propose `wsl --manage --compact` to microsoft/WSL, referencing this tool's implementation
- Localization (resource strings)

---

## Versioning

- `0.x`: features land per milestone; CLI may change with notice in CHANGELOG.
- `1.0`: CLI and `--json` schema are stable; breaking changes only in `2.0`.
