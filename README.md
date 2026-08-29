# wsldisk

> Compact, shrink, move, inspect and snapshot WSL2 virtual disks — from one native, dependency-free Windows CLI.

**Status:** planning / pre-alpha. Nothing is implemented yet. See [PLAN.md](PLAN.md) and [ROADMAP.md](ROADMAP.md).

## Why

Every WSL2 distribution (and Docker Desktop's data volume) lives in an `ext4.vhdx` file that
**only ever grows**. Reclaiming space today means a fragile ritual of `fstrim`, `wsl --shutdown`,
`Optimize-VHD` (Hyper-V module only — not on Windows Home) or a hand-written `diskpart` script.
Moving a distro to another drive means `wsl --export` / `--import` and re-fixing the default user.
Backups are full tarballs. Microsoft's sparse-VHD auto-reclaim is behind `--allow-unsafe` because
it can corrupt data.

Dozens of PowerShell scripts each solve one slice of this. `wsldisk` aims to be the single,
tested, native tool that covers the whole lifecycle of a WSL2 disk.

## Planned features

| Command | What it does |
|---|---|
| `wsldisk list` | All distros + Docker Desktop volumes: path, virtual size, actual size on disk, used inside ext4, sparse flag, WSL version |
| `wsldisk compact <distro>` | `fstrim` → shutdown → compact via Virtual Disk API (no Hyper-V module needed) → report reclaimed bytes |
| `wsldisk shrink <distro> --to 64G` | Actually reduce the **maximum** virtual size: `e2fsck` + `resize2fs` inside the distro, then `ResizeVirtualDisk` |
| `wsldisk grow <distro> --to 512G` | Grow the virtual disk and the ext4 filesystem in one step |
| `wsldisk move <distro> D:\WSL\` | Move the `.vhdx` and repoint the registry `BasePath` — no export/import, preserves default user and settings |
| `wsldisk snapshot` / `restore` | Fast snapshots (VHDX copy or export tar), optional incremental/dedup backends, retention policy, scheduling |
| `wsldisk doctor` | Detect the `--allow-unsafe` sparse foot-gun, orphaned VHDX files, distros with wrong `BasePath`, disks near capacity |
| `wsldisk mount <file.vhdx>` | Attach any VHDX read-only into a distro for forensics/recovery |
| `wsldisk info` / `usage` / `orphans` / `lock` | Detail view, where space goes inside the guest, unreferenced VHDX files, which process holds the disk |
| `wsldisk trim` / `clean` / `verify` | fstrim only; purge known caches (apt, npm, pip, docker, journal…) then compact; read-only `e2fsck` |
| `wsldisk clone` / `migrate` / `default-user` / `set-sparse` | Scratch copies, move all distros to a new drive, fix `DefaultUid`, guided sparse toggle |
| `wsldisk rescue` | Shell in a helper distro with a broken distro's disk mounted rw |
| `wsldisk schedule` / `config` / `completion` | Task Scheduler jobs, settings, shell completions |

Full command tree and per-command specs in [PLAN.md](PLAN.md) §4.

**Scope:** WSL2 only. WSL1 distributions have no virtual disk; `wsldisk list` shows them, every other command refuses with a hint to convert (`wsl --set-version <distro> 2`).

## Design principles

1. **Native.** Direct use of `virtdisk.h`, the registry and the WSL COM/`wslapi` surface. No `diskpart` scraping, no Hyper-V module dependency, no PowerShell runtime.
2. **Safe by default.** Refuse to touch a disk that is attached/running. Dry-run everything. Verify checksums on move. Never enable sparse mode unless explicitly asked.
3. **Single static binary.** `wsldisk.exe`, distributable via `winget` and `scoop`.
4. **Scriptable.** `--json` output on every command; exit codes that mean something.
5. **Works on Windows Home** — the Virtual Disk Service API is available everywhere Hyper-V's PowerShell module is not.
6. **Fully tested.** 100% line/branch coverage enforced in CI, plus real-Win32 contract tests, end-to-end integration tests on throwaway distros, and nightly fuzzing. This tool touches root filesystems; nothing untested ships.
7. **Fully automated.** GitHub Actions for lint, multi-toolchain builds, coverage gate, ASan, CodeQL, integration, and signed releases published to winget and scoop.

## Tech stack

Modern C++ (C++20/23), CMake + vcpkg, [WIL](https://github.com/microsoft/wil), [CLI11](https://github.com/CLIUtils/CLI11), [fmt](https://github.com/fmtlib/fmt), Catch2. Details in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Documents

- [PLAN.md](PLAN.md) — detailed project plan: scope, user stories, technical approach, risks, decisions
- [ROADMAP.md](ROADMAP.md) — milestones and task checklists
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module layout, key Windows APIs, workflows
- [docs/TESTING.md](docs/TESTING.md) — testing policy, 100% coverage gate, fakes, scenarios
- [docs/CI.md](docs/CI.md) — GitHub workflows: ci, nightly, release, codeql, dependabot
- [docs/RESEARCH.md](docs/RESEARCH.md) — prior art and reference material
- [CONTRIBUTING.md](CONTRIBUTING.md) — dev environment, coding standards

## License

[MIT](LICENSE)
