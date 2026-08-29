# CI / GitHub Workflows

All workflows live in `.github/workflows/`. Every job pins actions by SHA, uses least-privilege
`permissions:`, and caches vcpkg binaries (`x-gha` binary cache) so a full PR run stays under
~15 minutes.

## `ci.yml` — pull requests and pushes to `main`

Matrix: `{ msvc, clang-cl } × { x64, arm64 } × { Debug, Release }` on `windows-2025`
(arm64 cross-compiles; arm64 tests run only on a self-hosted arm64 runner if available, otherwise build-only).

Jobs:
1. **lint** — `clang-format --dry-run --Werror`, `clang-tidy` (via `compile_commands.json`), `cmake-format`, markdown lint, `actionlint` for workflows.
2. **build-test** — configure with preset, build, `ctest -L unit`, `ctest -L contract` (real Win32, no WSL). Uploads test logs (JUnit via Catch2 reporter) for the PR summary.
3. **coverage** — clang-cl x64 Debug with `-fprofile-instr-generate -fcoverage-mapping`; runs unit + contract (+ integration when available); `llvm-profdata merge`, `llvm-cov export -format=lcov`; `scripts/check-coverage.py --lines 100 --branches 100 --functions 100`; uploads to Codecov; attaches HTML report artifact. **Required check.**
4. **asan** — clang-cl Debug with `-fsanitize=address`, unit + contract tests. **Required check.**
5. **integration** — `WSLDISK_INTEGRATION=1`, installs WSL (`wsl --install --no-distribution`), fetches the pinned Alpine fixture, runs `ctest -L integration`. Runs on hosted `windows-2025` if the M0 spike confirms nested virtualisation works; otherwise on `[self-hosted, windows, wsl2]` and only for pushes to `main` + `pull_request_target` from trusted authors. Always cleans up test distros in a `post` step.
6. **package** — Release x64/arm64 static builds, `wsldisk --version` smoke test, zip + SHA256SUMS, uploaded as artifacts (consumed by `release.yml`).

Branch protection on `main`: lint, build-test (all matrix legs), coverage, asan, package required; linear history; signed commits encouraged.

## `nightly.yml` — schedule `0 3 * * *`

- Full integration suite on the self-hosted runner across WSL versions (`wsl --update` latest, plus pinned previous release via `wsl --update --pre-release` matrix / MSI install of last-1).
- Fuzzers (libFuzzer, 10 min each target) with corpus persisted as artifact; crashes open an issue via `peter-evans/create-issue-from-file`.
- Large-disk test: 200 GB sparse VHDX compact/move timing, results appended to `docs/perf/` on `main` via a bot commit.
- Mutation testing report (advisory).
- Dependency drift: `vcpkg x-update-baseline --dry-run` diff surfaced as a job summary.

## `release.yml` — on tag `v*.*.*`

1. Re-run the full `ci.yml` matrix (no reuse of PR artifacts).
2. Build Release x64 + arm64, generate SBOM (`vcpkg export --sbom` / CycloneDX), `SHA256SUMS`.
3. Sign: Sigstore `cosign sign-blob` + GitHub artifact attestations (`actions/attest-build-provenance`); Authenticode via Azure Trusted Signing when a certificate is configured (secret gated).
4. Create GitHub Release with generated notes from Conventional Commits (`release-drafter` / `git-cliff`), attach zips, sums, SBOM, attestations.
5. Open PRs to `microsoft/winget-pkgs` (`wingetcreate update`) and the scoop bucket with the new version and hashes.
6. Post-release smoke: fresh `windows-2025` runner does `winget install zcsizmadia.wsldisk` (once published) and runs `wsldisk --version`, `wsldisk list --json`.

## `codeql.yml`

CodeQL C/C++ on PRs and weekly; `security-extended` query pack. Results block merge at `high`+.

## `dependabot.yml`

- `github-actions` weekly.
- `vcpkg` baseline bump via a scheduled workflow (`vcpkg-baseline.yml`) that opens a PR with the diff, since Dependabot lacks native vcpkg support.

## `stale.yml`, `labeler.yml`

Housekeeping: label PRs by path (`area:platform`, `area:ops`, `docs`), stale issues after 90 days with `needs-info`.

## Reusable pieces

- `.github/actions/setup-toolchain/` — composite action: install Ninja, LLVM (pinned version), OpenCppCoverage, set up vcpkg with binary cache, export `VCPKG_ROOT`.
- `.github/actions/wsl-fixture/` — composite action: fetch/cache Alpine rootfs (SHA256-pinned), import as `wsldisk-test-<run_id>`, register cleanup.
- `scripts/check-coverage.py` — parses lcov, enforces thresholds, prints per-file gaps as GitHub annotations.

## Secrets

`CODECOV_TOKEN`, `WINGET_GITHUB_TOKEN` (PAT scoped to fork of winget-pkgs), optional `AZURE_TRUSTED_SIGNING_*`. No secrets are available to `pull_request` from forks; the integration job uses `pull_request_target` with an explicit `safe-to-test` label gate.

## Implementation notes

Recorded as the workflows were written; each is a deliberate deviation from the
design above, not an oversight.

- **`clang-tidy` is advisory for now.** clang-tidy 18 crashes parsing the C++23
  standard headers shipped with MSVC 14.4x, which makes its findings unreliable
  rather than merely noisy. The step runs with `continue-on-error: true`; it
  becomes blocking once a pinned LLVM analyses the tree cleanly.
- **The lint job configures the `x64-lint` preset**, a single-config Ninja build,
  purely to produce `compile_commands.json`. A multi-config database lists every
  source once per configuration, which would triple lint time, and its
  `@...modmap` arguments are not something clang-tidy can consume. The project
  sets `CMAKE_CXX_SCAN_FOR_MODULES OFF` for the same reason (no modules are used).
- **The vcpkg binary cache uses `actions/cache` over a local directory**
  (`VCPKG_DEFAULT_BINARY_CACHE`) rather than the `x-gha` provider, which needs
  `ACTIONS_RUNTIME_TOKEN` exported into the job.
- **vcpkg is cloned in full, then checked out at the manifest baseline.** A
  shallow clone cannot resolve a port at a pinned version.
- **`wsl-fixture` has no post step.** Composite actions cannot register one, so
  every job that imports a scratch distro pairs it with
  `.github/actions/wsl-cleanup` guarded by `if: always()`. The cleanup sweeps
  every distribution named `wsldisk-test-*`, not just the current run's, so a
  leak from an earlier job cannot poison a self-hosted runner.
- **The integration job runs on hosted `windows-2025`** and is gated for fork
  pull requests behind the `safe-to-test` label. Whether nested virtualisation on
  hosted runners is reliable enough to keep it there is the open M0 spike.

### Corrections after the first CI run

The first run of these workflows failed six of fifteen checks. What changed, and
why, so the reasoning is not lost:

- **arm64 legs could not find a compiler.** `ilammy/msvc-dev-cmd` was given
  `arch: arm64`, which selects the arm64-*hosted* toolset; the runners are x64,
  so nothing was installed and `cl` never reached PATH. The cross-compile form is
  `amd64_arm64`, which `setup-toolchain` now derives from its `architecture` input.
- **Coverage could not link.** CMake drives clang-cl targets through `lld-link`
  rather than the clang driver, so `-fprofile-instr-generate` arrived as an
  unknown linker argument and `/WX` made it fatal. The objects already request
  the profile runtime through a `/DEFAULTLIB:` directive, so the fix is to put
  clang's compiler-rt directory on the library search path and pass no link flag
  at all. `-print-runtime-dir` names a per-target layout some LLVM packages do
  not ship, so `lib/windows` is tried as well.
- **Codecov failed the job.** No `CODECOV_TOKEN` is configured, and the step ran
  with `fail_ci_if_error: true`. Codecov is reporting, not gating --
  `check-coverage.py` is the gate and has already run by then -- so the upload is
  now skipped without a token and never fails the job.
- **The ASan job now uses MSVC, not clang-cl.** clang-cl's Windows ASan
  miscompiles exception handling on the toolchains we build with: a twenty-line
  program that throws and catches faults with an access violation inside the
  catch block, using clang-cl's own driver. MSVC's ASan handles it, links
  statically with `/MT`, and accepts the debug CRT, so no special vcpkg triplet is
  needed. `_DISABLE_STRING_ANNOTATION` / `_DISABLE_VECTOR_ANNOTATION` are defined
  because the vcpkg dependencies are not built with ASan and the STL's container
  annotations are an all-or-nothing choice for the whole image.
- **The integration job stays on hosted runners.** It passed on
  `windows-2025` on the first attempt, which answers the M0 spike; the
  self-hosted fallback is not needed.

`llvm-cov` also reports "3 functions have mismatched data" while merging. Those
are inline functions from excluded third-party headers; every file under `src/`
is present in the report with its full function list.
