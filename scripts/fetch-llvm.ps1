<#
.SYNOPSIS
    Downloads the pinned LLVM into build/lint-tools so clang-tidy, clang-format,
    the coverage build and the fuzzers can run on a developer machine.

.DESCRIPTION
    CI installs LLVM with `choco install llvm --version=<pinned>`, which needs
    administrator rights. This does not: it unpacks the official release archive
    under build/lint-tools/ and prints the directory to put on PATH.

    It exists because two of the six checks in `scripts/lint.ps1` could not run
    here at all. clang-format and clang-tidy refuse to run against anything but
    the pinned version -- correctly, since a different one disagrees about both
    formatting and diagnostics -- and the coverage preset needs clang-cl,
    llvm-profdata and llvm-cov, so `x64-coverage` could not even be configured.
    The result was that coverage failures were only ever found in CI, one full
    round trip at a time.

    The version is read from `.github/actions/setup-toolchain/action.yml`, so
    this cannot drift from what CI uses.

.PARAMETER Force
    Download again even if the directory is already there.

.EXAMPLE
    .\scripts\fetch-llvm.ps1
    $env:PATH = "$(.\scripts\fetch-llvm.ps1 -Quiet);$env:PATH"
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolCache = Join-Path $repoRoot 'build/lint-tools'

function Get-PinnedVersion {
    $action = Join-Path $repoRoot '.github/actions/setup-toolchain/action.yml'
    if (-not (Test-Path $action)) { throw "not found: $action" }
    $match = [regex]::Match((Get-Content $action -Raw), "llvm-version:[\s\S]*?default:\s*'([0-9.]+)'")
    if (-not $match.Success) { throw "could not read the pinned LLVM version from $action" }
    return $match.Groups[1].Value
}

$version = Get-PinnedVersion
$dir = Join-Path $toolCache "llvm-$version"
$bin = Join-Path $dir 'bin'

if ((Test-Path (Join-Path $bin 'clang-tidy.exe')) -and -not $Force) {
    if (-not $Quiet) { Write-Host "LLVM $version already at $bin" }
    return $bin
}

# The .tar.xz rather than the NSIS installer: it unpacks without elevation and
# without touching the registry or the system PATH. tar.exe ships with Windows
# 10 1803+ and handles xz.
$archive = "clang+llvm-$version-x86_64-pc-windows-msvc.tar.xz"
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$archive"
$download = Join-Path $toolCache $archive

New-Item -ItemType Directory -Force -Path $toolCache | Out-Null

if (-not (Test-Path $download)) {
    if (-not $Quiet) { Write-Host "fetching LLVM $version (about 900 MB) ..." }
    try {
        Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing
    } catch {
        Remove-Item $download -Force -ErrorAction SilentlyContinue
        throw "could not download $url : $($_.Exception.Message)"
    }
}

if (-not $Quiet) { Write-Host "unpacking ..." }
$staging = Join-Path $toolCache "llvm-$version-staging"
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $staging | Out-Null

# Windows' own bsdtar, by full path. A bare `tar.exe` finds msys or Git-for-
# Windows tar first on many developer machines, and those read `C:\...` as a
# remote `host:path` and fail with "Cannot connect to C: resolve failed".
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path $tar)) { throw "not found: $tar (Windows 10 1803+ ships bsdtar)" }

# bsdtar handles .tar.xz directly; no separate xz step.
& $tar -xf $download -C $staging
if ($LASTEXITCODE -ne 0) { throw "tar failed to unpack $download (exit $LASTEXITCODE)" }

$unpacked = Get-ChildItem $staging -Directory | Select-Object -First 1
if (-not $unpacked) { throw "the archive unpacked to nothing" }

Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
Move-Item $unpacked.FullName $dir
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $download -Force -ErrorAction SilentlyContinue

foreach ($tool in 'clang-cl.exe', 'clang-tidy.exe', 'clang-format.exe', 'llvm-cov.exe', 'llvm-profdata.exe') {
    if (-not (Test-Path (Join-Path $bin $tool))) { throw "$tool is missing from the archive" }
}

if (-not $Quiet) {
    Write-Host ""
    Write-Host "LLVM $version is at $bin"
    Write-Host "Put it ahead of Visual Studio's copy on PATH:"
    Write-Host "    `$env:PATH = `"$bin;`$env:PATH`""
    Write-Host ""
    Write-Host "Then `scripts/lint.ps1` runs clang-format and clang-tidy, and"
    Write-Host "`cmake --preset x64-coverage` configures."
}
return $bin
