# Contributing

## Prerequisites

- Windows 11 (or 10 22H2) with WSL2 installed
- Visual Studio 2022 17.10+ with "Desktop development with C++" (MSVC, Windows 11 SDK, CMake, Ninja) — or Build Tools
- [vcpkg](https://github.com/microsoft/vcpkg) (`VCPKG_ROOT` set); dependencies come from `vcpkg.json`
- Optional: LLVM/clang-cl for clang-tidy and the ASan job

## Build

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Integration tests need WSL and create/destroy a throwaway distro named `wsldisk-test-*`:

```powershell
$env:WSLDISK_INTEGRATION = 1
ctest --preset x64-debug -L integration
```

## Coding standards

- C++23, `/W4 /permissive- /utf-8`, warnings are errors
- RAII everywhere; WIL for handles and Win32 errors; no raw `new`/`delete`
- `std::expected` for expected failures, exceptions for programmer/unexpected errors
- `<windows.h>` only under `src/lib/platform/`
- Every operation gets: `plan()` support for `--dry-run`, a unit test with fakes, an integration test
- Every user-facing error carries a remedy
- `.clang-format` is authoritative; run before committing

## Commits & PRs

- Conventional Commits (`feat:`, `fix:`, `docs:`, `test:`, `chore:`)
- One logical change per PR; link the roadmap item or issue
- CI must be green; a maintainer reviews data-safety-relevant changes (anything under `ops/`)

## Safety rule

Never test destructive operations against a distro you care about. Use `wsl --import` of a tiny rootfs into a temp directory.
