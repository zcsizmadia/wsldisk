# Contributing

## Prerequisites

- Windows 11 (or 10 22H2) with WSL2 installed
- Visual Studio 2022 17.10+ with "Desktop development with C++" (MSVC, Windows 11 SDK, CMake, Ninja) — or Build Tools
- [vcpkg](https://github.com/microsoft/vcpkg), cloned and bootstrapped:

  ```powershell
  git clone https://github.com/microsoft/vcpkg C:\src\vcpkg
  C:\src\vcpkg\bootstrap-vcpkg.bat
  ```

  Use a **full** clone, not `--depth 1`: vcpkg needs history to resolve ports at
  the pinned baseline. The bundled copy inside Visual Studio has no ports tree
  and cannot be used.
- Optional but recommended: the "C++ Clang tools for Windows" VS component or a
  standalone [LLVM](https://github.com/llvm/llvm-project/releases) install —
  `clang-cl` for the ASan job, `llvm-cov`/`llvm-profdata` for the coverage gate,
  `clang-format`/`clang-tidy` for lint.

## Build

`scripts/dev-shell.ps1` puts MSVC, CMake, Ninja, the LLVM tools and vcpkg on
PATH in one step. Dot-source it so the environment sticks:

```powershell
. .\scripts\dev-shell.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

It finds vcpkg via `VCPKG_ROOT`, a `vcpkg` checkout next to this repository, or
`%USERPROFILE%\vcpkg` — pass `-VcpkgRoot` to override.

### Presets

| Preset | What it is for |
|---|---|
| `x64-debug`, `x64-release` | MSVC, x64. Ninja Multi-Config, so either preset can build either configuration. |
| `arm64-release` | MSVC, arm64, cross-compiled |
| `x64-clang`, `arm64-clang` | clang-cl, for the second-opinion diagnostics CI runs |
| `x64-coverage` | clang-cl with source-based coverage; hosts the `coverage` target |
| `x64-asan` | MSVC with AddressSanitizer (clang-cl's Windows ASan miscompiles exception handling) |
| `x64-lint` | single-config generator, only to produce `compile_commands.json` for clang-tidy |
| `x64-fuzz` | clang-cl with libFuzzer; `ctest -L fuzz` replays the corpus |

Test presets `x64-debug-unit`, `x64-debug-contract` and `x64-debug-integration`
select one layer of the pyramid.

Integration tests need WSL and create/destroy a throwaway distro named
`wsldisk-test-*`. They register in CTest either way but skip themselves unless
the environment variable is set:

```powershell
$env:WSLDISK_INTEGRATION = 1
ctest --preset x64-debug-integration
```

`scripts/fetch-fixtures.ps1` downloads the SHA256-pinned Alpine rootfs the
integration suite imports.

## Coding standards

- C++23, `/W4 /permissive- /utf-8`, warnings are errors
- RAII everywhere; WIL for handles and Win32 errors; no raw `new`/`delete`
- `std::expected` for expected failures, exceptions for programmer/unexpected errors
- `<windows.h>` only under `src/lib/platform/`, and every Win32 call goes through
  the `Win32Api` table in `platform/win32_api.h` so tests can inject failures
- Every operation gets: `plan()` support for `--dry-run`, a unit test with fakes, an integration test
- Every user-facing error carries a remedy
- Tests cover behaviour, not counters. A defaulted (`= default`) special member is
  not counted for coverage, and code no test can reach is marked with an
  `LCOV_EXCL_*` comment saying why — see [docs/TESTING.md](docs/TESTING.md). Never
  delete a defensive check, or reshape working code, to satisfy the instrumenter.
- Run the whole lint job locally before pushing. **clang-format and clang-tidy
  no longer run on pull requests** -- they need a vcpkg restore and an LLVM
  install, which made them one of the two slowest checks gating a review, so
  they run on `main` instead. Locally is now the first place they run:

  ```powershell
  . .\scripts\dev-shell.ps1
  .\scripts\lint.ps1
  ```

  It runs clang-format, clang-tidy, actionlint, ruff and pytest, downloading the
  pinned actionlint and ruff binaries on demand, and reports each as pass, fail or
  skipped. markdownlint is skipped when the npm registry is unreachable.

  **clang-tidy does not run on pull requests**, so this is where it happens
  before merge. It takes about three minutes -- one process per file across the
  cores. `-Jobs` sets how many at once; `-Changed` narrows it to the sources
  that differ from `origin/main`, and falls back to everything when a header
  changed, because a header change moves every translation unit that includes
  it.

  ```powershell
  .\scripts\lint.ps1 -Changed
  ```

  **clang-format and clang-tidy must be the LLVM version CI pins**, which the
  script checks and refuses to run without. Visual Studio ships its own copies
  several major versions behind and puts them on PATH first: its clang-format
  reformats differently, so a file that is clean locally fails in CI, and its
  clang-tidy has been seen to crash inside MSVC's `<format>`. Put the standalone
  LLVM's `bin` ahead of Visual Studio's. The compile database clang-tidy needs is
  refreshed automatically when it no longer lists every source.

- **Run AddressSanitizer before pushing.** It gates pull requests as well, so
  this is not the only place it happens -- but finding it here costs a few
  minutes instead of a CI round trip, and it catches what the ordinary builds
  cannot. A dangling capture that every other configuration happily passed, for
  instance:

  ```powershell
  cmake --preset x64-asan
  cmake --build build/x64-asan --config Debug
  ctest --test-dir build/x64-asan -C Debug -L "unit|contract" --output-on-failure
  ```

- `.clang-format` is authoritative; run before committing:

  ```powershell
  $files = Get-ChildItem -Recurse -Path src, tests -Include *.cpp, *.h, *.in | ForEach-Object { $_.FullName }
  clang-format -i @files
  ```

## Tests & coverage

- **100% line, branch and function coverage is required.** CI fails otherwise. See [docs/TESTING.md](docs/TESTING.md).
- Run the gate locally before pushing (needs `llvm-cov`, `llvm-profdata` and Python 3.10+):

  ```powershell
  cmake --preset x64-coverage
  cmake --build --preset x64-coverage
  cmake --build --preset x64-coverage-gate   # runs the tests, then enforces the thresholds
  ```

  The `coverage` target writes `build/x64-coverage/coverage.lcov` and an HTML
  report next to it, then fails if any threshold is missed.
- Contract tests (`-L contract`) hit real Win32 on temp files and a scratch registry key; they need no WSL and must stay hermetic (clean up everything they create).
- Coverage exclusions (`LCOV_EXCL_*`) are for provably unreachable code only and require a justifying comment.
- Test case names must not start with `--`: CTest passes the name to Catch2 as a
  filter, and Catch2 parses a leading `--` as one of its own options.

## Commits & PRs

- Conventional Commits (`feat:`, `fix:`, `docs:`, `test:`, `chore:`)
- One logical change per PR; link the roadmap item or issue
- CI must be green (lint, all build legs, coverage gate, ASan, CodeQL); a maintainer reviews data-safety-relevant changes (anything under `ops/`)
- Workflows are described in [docs/CI.md](docs/CI.md); changes to `.github/` need `actionlint` to pass and actions pinned by SHA

## Safety rule

Never test destructive operations against a distro you care about. Use `wsl --import` of a tiny rootfs into a temp directory.
