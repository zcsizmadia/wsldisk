# Testing Policy

**Requirement: 100% line and branch coverage of `libwsldisk` and the CLI, enforced in CI.
A PR that lowers coverage below 100% fails.** The tool rewrites people's root filesystems;
untested code is not shipped.

## Test pyramid

| Layer | Target | Runs where | Tooling |
|---|---|---|---|
| Unit | `ops/`, `model/`, `cli/` rendering & parsing | Every PR, all platforms | Catch2 v3, fakes for all interfaces |
| Platform contract | `platform/` wrappers against real Win32 (VHDX create/compact/resize on temp files, registry under a test hive, sparse file ops) | Every PR, Windows runner, **no WSL needed** | Catch2, `HKCU\Software\wsldisk-test` scratch key, `%TEMP%` VHDX files |
| Integration | End-to-end commands against a throwaway WSL2 distro | Every PR (if hosted runner supports WSL2) or nightly on self-hosted | Catch2 `[integration]` tag, `WSLDISK_INTEGRATION=1` |
| Fuzz | Registry value parsing, `wsl.exe` output parsing, `manifest.json`, size-string parsing (`64G`) | Nightly | libFuzzer via clang-cl, corpora in `tests/fuzz/corpus/` |
| Mutation (advisory) | Detect tests that pass with mutated code | Weekly | `mull` or manual review of survivors; report only |

## Coverage

- **Tool:** OpenCppCoverage (MSVC) for Windows-native coverage; clang-cl `-fprofile-instr-generate -fcoverage-mapping` + `llvm-cov` as the reference implementation because it gives branch coverage. CI publishes both; the **clang-cl/llvm-cov number gates**.
- **Scope:** everything under `src/`. `tests/`, `spikes/`, vcpkg dependencies excluded.
- **Threshold:** 100% lines, 100% branches, 100% functions. Enforced by `llvm-cov export` → `scripts/check-coverage.py` failing under threshold.
- **Exclusions:** the only permitted exclusion is `// LCOV_EXCL_LINE` / `// LCOV_EXCL_START..STOP` on truly unreachable defensive code (e.g. `std::unreachable()`, `default:` after an exhaustive `switch` on an enum with `-Wswitch`). Each exclusion needs a comment justifying it and is reviewed. Target: fewer than 10 in the whole codebase.
- **How 100% is achievable:** Win32 calls live only in `platform/`, wrapped as thin functions; each failure branch is exercised by the contract tests (bad path, locked file, access denied via ACL on a temp file, invalid handle) or by a **fault-injection shim**: `platform/` calls Win32 through a `Win32Api` table that tests can replace to return arbitrary error codes. Every `THROW_IF_WIN32_ERROR` therefore has a test that triggers it.
- **Reporting:** Codecov upload with `fail_ci_if_error`, PR comment with diff coverage, badge in README. HTML report attached as a workflow artifact.

## Fakes and fixtures

- `FakeRegistry` — in-memory hive with canned distros (inbox 1.x layout, Store 2.x layout, `\\?\` prefixes, sparse flag, missing `VhdFileName`, broken `BasePath`).
- `FakeVirtualDisk` — tracks virtual/physical size, attached state, parent; compaction shrinks physical to a configured value; can be told to fail any call.
- `FakeWslHost` — scripted responses for launch/terminate/mount; records commands run inside the "distro"; can simulate a running distro holding the lock.
- `FakeFileSystem` — in-memory files with sparse ranges, volume types (NTFS/ReFS/FAT), free space, copy failures at byte N.
- `FakeClock` — deterministic timeouts for wait-for-unlock loops.
- Fixture rootfs: a ~3 MB Alpine minirootfs tarball downloaded by `scripts/fetch-fixtures.ps1` (SHA256-pinned, cached in CI). Integration tests import it as `wsldisk-test-<guid>` under `%TEMP%` and always remove it in a `finally`/scope guard, even on failure.

## Integration test scenarios (must all exist before the corresponding command ships)

- `list` shows the test distro with correct path, version, virtual and actual sizes; `--json` schema validates.
- `compact`: write 2 GB of random data in the guest, delete it, `compact`, assert actual size drops ≥ 1.5 GB; distro still boots; file checksums of remaining files unchanged.
- `compact --dry-run` changes nothing (hash VHDX before/after).
- `compact` against a running distro that refuses to terminate → exit code 11, nothing changed.
- `grow --to`: guest `df` shows new size; `shrink --to`: refuses when data doesn't fit; succeeds otherwise; `e2fsck -n` clean.
- `move` to another directory and to a VHD-backed second volume (created in CI with `New-VHD`/`diskpart`-free API); default user, GUID, flags preserved; source removed only after boot verification; simulated failure mid-copy → rollback leaves original intact and registry unchanged.
- `relink` to a wrong path → distro fails to start → rollback.
- `snapshot` → mutate → `restore` → content matches snapshot; retention deletes the right ones; `restore --as` creates a bootable second distro.
- `doctor` on a hive with each defect present flags exactly that defect; `--fix` remediates.
- Elevation path: run under a non-admin token in CI (`runas /trustlevel:0x20000`) and assert exit code 4 with the right message when full mode is requested.

## Unit test conventions

- One test file per source file, mirrored path (`src/lib/ops/compact_op.cpp` → `tests/unit/ops/compact_op_test.cpp`).
- Test names read as behaviour: `SCENARIO("compact refuses a running distro")`.
- No sleeping; use `FakeClock`. No real filesystem or registry in unit tests.
- Golden files for table and `--json` output under `tests/unit/golden/`, updated only with `--update-golden` and reviewed in the diff.

## Definition of done for any change

1. Unit tests for every new branch.
2. Contract or integration test if it touches `platform/` or a command.
3. Coverage still 100% (CI enforces).
4. Golden output updated if user-visible output changed.
5. `docs/` updated if behaviour changed.
