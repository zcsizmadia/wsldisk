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

    $env:WSLDISK_UPDATE_GOLDEN = '1'
    try {
        ctest --test-dir $BuildDir -C $Configuration -R '^unit\.' --output-on-failure
    } finally {
        Remove-Item Env:\WSLDISK_UPDATE_GOLDEN
    }

    Write-Host ""
    Write-Host "Golden files rewritten. Review them before committing:" -ForegroundColor Cyan
    Write-Host "    git diff tests/unit/golden/"
} finally {
    Pop-Location
}
