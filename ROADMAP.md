# wsldisk — Roadmap

_Last updated: 2026-08-29. Dates are targets, not promises._

Legend: `[ ]` todo · `[~]` in progress · `[x]` done

---

## M0 — Foundations & spikes (≈ 2 weeks)

Goal: de-risk the unknowns, have a compiling skeleton and CI.

**Repo & tooling**

- [x] CMake presets (`x64-debug`, `x64-release`, `arm64-release`, plus `x64-clang`, `arm64-clang`, `x64-coverage`, `x64-asan`, `x64-lint`), vcpkg manifest with a pinned baseline, Ninja Multi-Config
- [x] `libwsldisk` static lib + `wsldisk` exe + `tests` targets; `wsldisk --version` works
- [x] `.clang-format`, `.clang-tidy`, `.editorconfig`, `.gitattributes`, pre-commit hook (`.githooks/`, `scripts/install-hooks.ps1`)
- [x] `scripts/dev-shell.ps1` — one-step local MSVC + CMake + Ninja + LLVM + vcpkg environment
- [x] `ci.yml`: lint job (clang-format, markdownlint, actionlint, ruff; **clang-tidy advisory** — see below); build-test matrix MSVC + clang-cl × x64 + arm64 × Debug + Release; JUnit test reports
- [x] `ci.yml`: **coverage job with 100% line/branch/function gate** (`scripts/check-coverage.py`), Codecov upload, HTML artifact
- [x] `ci.yml`: ASan job, integration job, package job (Release zips + SHA256SUMS)
- [x] `Win32Api` fault-injection table in `platform/` so every error branch is testable
- [x] Test skeleton: `tests/unit` (Catch2), `tests/contract` (real Win32 on temp files), `tests/integration` (gated by `WSLDISK_INTEGRATION`)
- [ ] `tests/fuzz` targets and `nightly.yml` ([#10](https://github.com/zcsizmadia/wsldisk/issues/10))
- [ ] Fakes for the remaining interfaces (`FakeRegistry`, `FakeVirtualDisk`, `FakeWslHost`, `FakeFileSystem`, `FakeClock`) — land with the interfaces in M1
- [x] Make `clang-tidy` a blocking gate ([#8](https://github.com/zcsizmadia/wsldisk/issues/8)) — needed a pinned LLVM 20; the crash was specific to clang-tidy 18
- [x] Verify WSL2 works on `windows-2025` hosted runners; decide hosted vs self-hosted ([#7](https://github.com/zcsizmadia/wsldisk/issues/7))
- [x] `codeql.yml`, `dependabot.yml` (actions), `vcpkg-baseline.yml`, `labeler.yml`, `stale.yml`
- [x] Composite actions: `setup-toolchain`, `wsl-fixture` (SHA-pinned Alpine 3.22.4 rootfs), `wsl-cleanup`
- [ ] Branch protection on `main`: all CI jobs required, linear history ([#9](https://github.com/zcsizmadia/wsldisk/issues/9))
- [x] Issue/PR templates, CODEOWNERS, `SECURITY.md`

**Technical spikes** (throwaway code under `spikes/`, results recorded in `docs/RESEARCH.md`)

- [x] `CompactVirtualDisk` unattached vs attached-RO: measure reclaimed bytes after `fstrim` ([#1](https://github.com/zcsizmadia/wsldisk/issues/1))
- [x] Confirm `ResizeVirtualDisk` shrink path + `resize2fs` via `wsl --mount --vhd --bare` ([#2](https://github.com/zcsizmadia/wsldisk/issues/2))
- [ ] `WslLaunch` as uid 0 with a non-root default user ([#3](https://github.com/zcsizmadia/wsldisk/issues/3))
- [x] Registry layout across WSL inbox 1.x / Store 2.x ([#4](https://github.com/zcsizmadia/wsldisk/issues/4))
- [x] Docker Desktop VHDX lock behaviour when Docker is "stopped" vs quit ([#5](https://github.com/zcsizmadia/wsldisk/issues/5))
- [ ] Elevation relaunch + named-pipe result streaming prototype ([#6](https://github.com/zcsizmadia/wsldisk/issues/6))

**Exit criteria:** CI green with the 100% coverage gate passing on the skeleton, all spikes answered in RESEARCH.md, open questions in PLAN.md §8 resolved or converted to issues.

---

## M1 — `list`, `info`, `compact`, `trim`, `orphans` (v0.1.0, ≈ 3–4 weeks)

Goal: the 80% use case — see where the space is and get it back — shippable.

- [ ] `IRegistry`, `IVirtualDisk`, `IWslHost`, `IFileSystem` interfaces + Win32 implementations + fakes
- [ ] Distro enumeration (registry) with WSL1/WSL2 detection and default marker; WSL1 entries listed but refused by all other commands (exit 3, conversion hint)
- [ ] Size probes: virtual size, actual on-disk (`GetCompressedFileSize`), sparse detection, guest `df` (`--probe`)
- [ ] `wsldisk list` table + `--json` + `--scan` for orphaned VHDX
- [ ] `Plan → Execute → Verify` operation framework with progress sink and undo stack
- [ ] `CompactOperation`: preflight, `fstrim`, terminate, wait-for-unlock, compact (unattached), report
- [ ] Attached-RO "full" compaction when elevated; `--elevate` relaunch
- [ ] `--all`, `--dry-run`, `--no-trim`, `--restart`, `--file <vhdx>`
- [ ] `wsldisk info <distro>` detail view + `--json`
- [ ] `wsldisk trim <distro>` (fstrim only, no shutdown)
- [ ] `wsldisk orphans` with default scan dirs, `--delete`, `--relink`
- [ ] `wsldisk config get|set|edit|path`; `config.toml` schema
- [ ] `wsldisk completion powershell|bash|zsh`
- [ ] Exit code scheme, `-v` logging, `--log`
- [ ] Unit tests (100% coverage) for `list`/`compact` planner, preflight, renderers; golden files for table + JSON
- [ ] Contract tests: `CompactVirtualDisk`/`GetVirtualDiskInformation`/sparse-size probes on temp VHDX; registry enumeration on scratch hive
- [ ] Integration tests: import Alpine, bloat, compact, assert shrink & boot; `--dry-run` no-op; running-distro refusal (exit 11); elevation refusal (exit 4)
- [ ] Fuzz targets: registry values, `wsl.exe -l -v` output, size strings — wired into `nightly.yml`
- [ ] README usage docs, `docs/COMPACT.md` explaining what happens and why
- [ ] `release.yml`: tag → full matrix → SBOM → Sigstore + attestations → GitHub Release with generated notes → post-install smoke
- [ ] `nightly.yml`: integration across WSL latest / latest-1, fuzzers, large-disk perf
- [ ] First winget + scoop manifests (may land in M2 if name check pending)

**Exit criteria:** A Windows Home user with no Hyper-V module can `winget install wsldisk` and reclaim space from Ubuntu and Docker Desktop with one command.

---

## M2 — `move`, `relink`, `grow`, `shrink`, `usage`, `clean`, `verify` (v0.2.0, ≈ 5 weeks)

- [ ] `MoveOperation`: preflight (fs type, free space, lock), sparse-preserving copy with progress, hash `--verify`, registry repoint, start test, rollback, source cleanup; same-volume fast path
- [ ] `wsldisk relink <distro> <path>`
- [ ] `GrowOperation`: `ResizeVirtualDisk` + `resize2fs`; detect partitioned disks and refuse
- [ ] Helper-distro mechanism (tiny Alpine rootfs, on-demand import/remove) or `--via <distro>`
- [ ] `ShrinkOperation`: fit check with margin, `e2fsck -f`, `resize2fs <size>`, `ResizeVirtualDisk` (safe flag only), compact, `e2fsck -n` verify
- [ ] `wsldisk mount` / `unmount` wrappers (read-only default)
- [ ] `wsldisk verify <distro>` (VHDX metadata + `e2fsck -n`, exit 6 on errors)
- [ ] `wsldisk usage <distro>` with curated cache catalogue (`data/caches.toml`)
- [ ] `wsldisk clean <distro>` per-category flags, `--dry-run`, `--compact` chaining; never outside catalogue paths
- [ ] `wsldisk default-user <distro> [name|uid]`
- [ ] `wsldisk set-sparse <distro> on|off` with caveat gate and post-off compact
- [ ] `wsldisk lock <distro>` via Restart Manager; reused in preflight error messages
- [ ] Feature detection of `wsl.exe` capabilities (`--manage`, `--mount --vhd`, version)
- [ ] Unit tests (100% coverage) incl. rollback paths with injected failures at every mutating step
- [ ] Contract tests: `ResizeVirtualDisk` grow/shrink-safe on temp VHDX; sparse-preserving copy on real NTFS; FAT/exFAT refusal
- [ ] Integration tests for move (cross-volume via VHD-backed second volume in CI), relink failure rollback, grow, shrink fit-refusal and success with `e2fsck -n`
- [ ] `release.yml` extended: winget (`wingetcreate`) and scoop bucket PRs
- [ ] `docs/MOVE.md`, `docs/RESIZE.md`

**Exit criteria:** Full disk lifecycle (compact/grow/shrink/move) without touching `diskpart`, PowerShell or `wsl --export`.

---

## M3 — `snapshot`, `restore`, `clone`, `rescue`, `migrate`, `doctor`, `schedule` (v0.3.0, ≈ 5 weeks)

- [ ] Snapshot repo layout + `manifest.json` schema; config in `%APPDATA%\wsldisk\config.toml`
- [ ] `copy` backend (sparse-preserving) with `--keep-last/--keep-daily/--keep-weekly` retention
- [ ] `tar` backend via `wsl --export` (format flags where supported)
- [ ] `restore` in place and `--as <newname>` (new GUID registry entry)
- [ ] `differencing` backend (experimental, behind `--experimental`)
- [ ] `wsldisk snapshots list|prune|show`
- [ ] `wsldisk clone <distro> <newname>` (VHDX copy + fresh GUID registry entry)
- [ ] `wsldisk rescue <distro>` (helper distro, target mounted rw at `/mnt/rescue`)
- [ ] `wsldisk migrate <dir>` (plan + confirm, loops `move`, `--exclude-docker`)
- [ ] Hook scripts (`pre-snapshot`, `post-snapshot`) for restic/borg/kopia hand-off
- [ ] `wsldisk doctor` checks + `--fix` remediations (sparse warning, orphans, broken BasePath, bloat, capacity, `.wslconfig` sanity)
- [ ] `wsldisk schedule install|remove|status` via Task Scheduler COM
- [ ] Unit tests (100% coverage) incl. retention policy edge cases, manifest schema validation, every `doctor` check with/without defect
- [ ] Integration tests: snapshot → mutate → restore → verify content; `restore --as`; retention; `doctor --fix` per defect; scheduled task registered and removed
- [ ] Fuzz target: `manifest.json` parser
- [ ] `docs/SNAPSHOTS.md`, `docs/DOCTOR.md`

**Exit criteria:** Nightly unattended compact + snapshot with retention on a dev machine, zero manual steps.

---

## M4 — Hardening & 1.0 (≈ 3 weeks)

- [ ] Test matrix: Windows 10 22H2, Windows 11 24H2/25H2; WSL inbox, WSL Store 2.x latest and latest-1; Home + Pro
- [ ] ARM64 build tested on real hardware
- [ ] Fuzz corpora reviewed; zero open fuzz crashes; mutation-testing survivors triaged
- [ ] Coverage exclusions audited (< 10 `LCOV_EXCL` in codebase, each justified)
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

- `import`/`export` wrappers with progress, `--vhd`, `--default-user`, `--location`; `import --from-docker-image <ref>`
- `export-docker <distro>` — rootfs → OCI/Docker image tarball
- `stats [--record]` — size history + growth trends/sparklines
- `convert <vhd|vmdk|qcow2>` → VHDX + register
- Tray/WinUI 3 GUI on top of `libwsldisk` (space usage at a glance, one-click compact)
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
