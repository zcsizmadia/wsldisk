<#
.SYNOPSIS
    Rewrites the golden files the CLI renderer tests compare against.

.DESCRIPTION
    Golden files exist so a change to user-visible output is a change someone
    had to look at: the diff in the pull request is the review. Nothing rewrites
    them by accident, so regenerating is this separate, deliberate step.

    Run it, then read `git diff tests/unit/golden/` before committing. If the
    diff is not what you meant to change, the test was right.

.EXAMPLE
    . .\scripts\dev-shell.ps1
    .\scripts\update-golden.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/x64-debug',
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

Push-Location $repoRoot
try {
    cmake --build $BuildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    # The test binary directly rather than through ctest: ctest did not pass the
    # environment variable down to the test process, so the files were never
    # rewritten and the script reported success anyway.
    $exe = Join-Path $BuildDir "tests/unit/$Configuration/wsldisk_unit_tests.exe"
    if (-not (Test-Path $exe)) { throw "no unit test binary at $exe" }

    $env:WSLDISK_UPDATE_GOLDEN = '1'
    try {
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "the unit suite failed; golden files may be incomplete" }
    } finally {
        Remove-Item Env:\WSLDISK_UPDATE_GOLDEN
    }

    Write-Host ""
    Write-Host "Golden files rewritten. Review them before committing:" -ForegroundColor Cyan
    Write-Host "    git diff tests/unit/golden/"
} finally {
    Pop-Location
}
