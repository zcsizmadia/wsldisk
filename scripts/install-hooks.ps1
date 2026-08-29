<#
.SYNOPSIS
    Points this checkout's git hooks at the versioned ones in .githooks.

.DESCRIPTION
    Sets `core.hooksPath`, so the hooks live in the repository and stay in sync
    for everyone instead of being copied into .git/hooks once and forgotten.

        pwsh ./scripts/install-hooks.ps1

    Undo with:

        git config --unset core.hooksPath
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    git config core.hooksPath .githooks
    Write-Host "core.hooksPath = .githooks"
    Write-Host "Hooks installed: $((Get-ChildItem .githooks -File).Name -join ', ')"
    Write-Host "Bypass a hook for one commit with: git commit --no-verify"
}
finally {
    Pop-Location
}
