<#
.SYNOPSIS
    Runs everything the CI lint job runs, locally.

.DESCRIPTION
    The lint job is the easiest check to fail from a laptop, because most of its
    tools are not part of a normal C++ toolchain. This script runs the same set so
    a failure costs seconds instead of a CI round trip:

        clang-format   from the Visual Studio LLVM tools or a standalone LLVM
        clang-tidy     ditto -- must be the version pinned in CI, not VS's
        actionlint     downloaded on demand, pinned
        ruff           downloaded on demand, pinned
        pytest         for the coverage gate's own tests
        markdownlint   only if npx can reach the registry; skipped otherwise

    Every tool is reported as pass, fail or skipped, and the script exits non-zero
    if any of them failed.

.PARAMETER Configure
    Re-run `cmake --preset x64-lint` first. clang-tidy needs the compile database;
    pass this after adding or removing a source file.

.EXAMPLE
    . .\scripts\dev-shell.ps1
    .\scripts\lint.ps1 -Configure
#>
[CmdletBinding()]
param(
    [switch]$Configure
)

$ErrorActionPreference = 'Stop'

# Keep these in step with .github/workflows/ci.yml.
$actionlintVersion = '1.7.12'
$ruffVersion = '0.16.5'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolCache = Join-Path $repoRoot 'build/lint-tools'
New-Item -ItemType Directory -Force -Path $toolCache | Out-Null

$results = [ordered]@{}

function Invoke-Check {
    param([string]$Name, [scriptblock]$Body)

    Write-Host ""
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    try {
        & $Body
        if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
            $script:results[$Name] = 'FAIL'
        } else {
            $script:results[$Name] = 'pass'
        }
    } catch {
        Write-Host $_.Exception.Message -ForegroundColor Yellow
        $script:results[$Name] = 'SKIPPED'
    }
}

function Get-PinnedTool {
    param([string]$Name, [string]$Url, [string]$Exe)

    $dir = Join-Path $toolCache $Name
    $path = Join-Path $dir $Exe
    if (Test-Path $path) { return $path }

    Write-Host "  fetching $Name ..."
    $zip = Join-Path $toolCache "$Name.zip"
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
    Expand-Archive $zip -DestinationPath $dir -Force
    Remove-Item $zip -Force

    $found = Get-ChildItem -Recurse -Path $dir -Filter $Exe | Select-Object -First 1
    if (-not $found) { throw "$Exe not found in the $Name archive" }
    return $found.FullName
}

Push-Location $repoRoot
try {
    $sources = Get-ChildItem -Recurse -Path src, tests -Include *.cpp, *.h, *.in |
        ForEach-Object { $_.FullName }

    Invoke-Check 'clang-format' {
        clang-format --dry-run --Werror @sources
    }

    if ($Configure) {
        Invoke-Check 'cmake --preset x64-lint' { cmake --preset x64-lint }
    }

    Invoke-Check 'clang-tidy' {
        if (-not (Test-Path 'build/x64-lint/compile_commands.json')) {
            throw "no compile database; re-run with -Configure"
        }
        $cpp = Get-ChildItem -Recurse -Path src -Include *.cpp | ForEach-Object { $_.FullName }
        clang-tidy -p build/x64-lint --extra-arg=-Wno-unused-command-line-argument @cpp
    }

    Invoke-Check 'actionlint' {
        $url = "https://github.com/rhysd/actionlint/releases/download/v$actionlintVersion/actionlint_${actionlintVersion}_windows_amd64.zip"
        $exe = Get-PinnedTool -Name "actionlint-$actionlintVersion" -Url $url -Exe 'actionlint.exe'
        & $exe
    }

    Invoke-Check 'ruff' {
        $url = "https://github.com/astral-sh/ruff/releases/download/$ruffVersion/ruff-x86_64-pc-windows-msvc.zip"
        $exe = Get-PinnedTool -Name "ruff-$ruffVersion" -Url $url -Exe 'ruff.exe'
        & $exe check scripts/
    }

    Invoke-Check 'pytest' {
        python -m pytest scripts/tests -q
    }

    Invoke-Check 'markdownlint' {
        if (-not (Get-Command npx -ErrorAction SilentlyContinue)) {
            throw "npx not on PATH"
        }
        # Distinguish "the registry would not give us the tool" from "the tool
        # found problems". Behind a proxied or restricted npm registry the first
        # is a skip -- CI stays authoritative for markdown -- while the second
        # must fail here.
        $output = npx --yes markdownlint-cli2 "**/*.md" "!build/**" 2>&1
        $exit = $LASTEXITCODE
        $text = $output | Out-String
        if ($exit -ne 0 -and $text -match 'npm error|ENOTFOUND|ECONNREFUSED|E403|EAI_AGAIN') {
            throw "npm registry unreachable for markdownlint-cli2"
        }
        Write-Host $text
        $global:LASTEXITCODE = $exit
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "=== summary ===" -ForegroundColor Cyan
foreach ($name in $results.Keys) {
    $status = $results[$name]
    $colour = switch ($status) { 'pass' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ("{0,-24} {1}" -f $name, $status) -ForegroundColor $colour
}

if ($results.Values -contains 'FAIL') {
    Write-Host ""
    Write-Host "lint failed" -ForegroundColor Red
    exit 1
}
exit 0
