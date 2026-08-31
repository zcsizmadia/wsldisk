<#
.SYNOPSIS
    Runs everything the CI lint job runs, locally.

.DESCRIPTION
    The lint job is the easiest check to fail from a laptop, because most of its
    tools are not part of a normal C++ toolchain. This script runs the same set so
    a failure costs seconds instead of a CI round trip:

        clang-format   the standalone LLVM pinned in CI, not Visual Studio's
        clang-tidy     ditto -- both are checked, and a mismatch is a failure
        actionlint     downloaded on demand, pinned
        ruff           downloaded on demand, pinned
        pytest         for the coverage gate's own tests
        markdownlint   only if npx can reach the registry; skipped otherwise

    Every tool is reported as pass, fail or skipped, and the script exits non-zero
    if any of them failed.

.PARAMETER Configure
    Re-run `cmake --preset x64-lint` before anything else. Rarely needed: the
    compile database clang-tidy uses is refreshed automatically when it no longer
    lists every source. Use this to pick up a change the file list does not show,
    such as an edit to the preset itself.

.PARAMETER Changed
    Run clang-tidy over only the sources that differ from `origin/main`, instead
    of all of them.

    For the inner loop, where the alternative is not running it at all. A changed
    header is a different matter -- every translation unit that includes it could
    have moved -- so if one has changed this checks everything anyway rather than
    quietly checking less than it claims.

.PARAMETER Jobs
    How many clang-tidy processes to run at once. Defaults to two fewer than the
    machine has cores, which leaves it usable while the check runs.

.EXAMPLE
    . .\scripts\dev-shell.ps1
    .\scripts\lint.ps1

.EXAMPLE
    .\scripts\lint.ps1 -Changed
#>
[CmdletBinding()]
param(
    [switch]$Configure,
    [switch]$Changed,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

# Keep these in step with .github/workflows/ci.yml.
$actionlintVersion = '1.7.12'
$ruffVersion = '0.16.5'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolCache = Join-Path $repoRoot 'build/lint-tools'
New-Item -ItemType Directory -Force -Path $toolCache | Out-Null

$results = [ordered]@{}

# A check that throws is normally reported as SKIPPED: it could not run, which is
# what an unreachable download or a missing optional tool looks like. Some
# conditions must not be waved through that way -- the wrong clang-tidy would
# report findings that do not match CI -- so a message with this prefix is a
# deliberate failure rather than an inability to run.
$lintFailurePrefix = 'lint-failure: '

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
        $message = $_.Exception.Message
        if ($message.StartsWith($script:lintFailurePrefix)) {
            Write-Host $message.Substring($script:lintFailurePrefix.Length) -ForegroundColor Red
            $script:results[$Name] = 'FAIL'
        } else {
            Write-Host $message -ForegroundColor Yellow
            $script:results[$Name] = 'SKIPPED'
        }
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

function Get-PinnedLlvmVersion {
    <#
    .SYNOPSIS
        The LLVM version CI installs, read from the toolchain action.

    .DESCRIPTION
        There is one source of truth, the same way the vcpkg baseline is read
        from the manifest rather than repeated here.
    #>
    $action = Join-Path $repoRoot '.github/actions/setup-toolchain/action.yml'
    $match = Select-String -Path $action -Pattern "^\s*default:\s*'([0-9]+\.[0-9]+\.[0-9]+)'" |
        Select-Object -First 1
    if (-not $match) { throw "could not read the pinned LLVM version from $action" }
    return $match.Matches[0].Groups[1].Value
}

function Assert-PinnedTool {
    <#
    .SYNOPSIS
        Fails unless the named LLVM tool is the version CI pins.

    .DESCRIPTION
        Visual Studio ships its own clang-format and clang-tidy and puts them on
        PATH ahead of a standalone LLVM. They are several major versions behind:
        clang-format reformats differently, so a file that is clean locally fails
        in CI, and VS's clang-tidy has been seen to crash outright inside MSVC's
        <format>. Either way the local run stops meaning anything, so this is a
        failure rather than something to warn about.
    #>
    param([string]$Name)

    $tool = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $tool) { throw "$Name not found on PATH" }

    $pinned = Get-PinnedLlvmVersion
    $reported = & $Name --version | Select-String -Pattern 'version\s+([0-9]+\.[0-9]+\.[0-9]+)'
    if (-not $reported) { throw "could not read the version of $($tool.Source)" }
    $actual = $reported.Matches[0].Groups[1].Value

    if ($actual -ne $pinned) {
        throw ($script:lintFailurePrefix +
               "$Name $actual at $($tool.Source) is not the pinned $pinned. " +
               "Put the pinned LLVM's bin directory ahead of Visual Studio's on PATH.")
    }
}

function Test-StaleCompileDatabase {
    <#
    .SYNOPSIS
        Whether the compile database is missing or no longer lists every source.

    .DESCRIPTION
        clang-tidy falls back to default flags for a file the database does not
        name, which for this project means no C++23 and no include paths -- so
        it reports a cascade of nonsense ("no template named 'Result'", "method
        can be made static" on a virtual override) that has nothing to do with
        the code. Reconfiguring is cheaper than reading those.
    #>
    param([string[]]$Sources)

    $database = Join-Path $repoRoot 'build/x64-lint/compile_commands.json'
    if (-not (Test-Path $database)) { return $true }

    $known = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in (Get-Content $database -Raw | ConvertFrom-Json)) {
        [void]$known.Add([System.IO.Path]::GetFullPath($entry.file))
    }
    foreach ($source in $Sources) {
        if (-not $known.Contains([System.IO.Path]::GetFullPath($source))) { return $true }
    }
    return $false
}

function Select-ChangedSources {
    <#
    .SYNOPSIS
        The sources under `src/` that differ from `origin/main`, for `-Changed`.

    .DESCRIPTION
        Committed on this branch, staged, or merely saved -- all three count, so
        the check covers the work in front of you rather than the last commit.

        A changed header returns everything. clang-tidy sees a translation unit,
        not a file: edit `interfaces.h` and any of the thirty-eight could have
        moved, and a check that reported "1 file" there would be claiming a pass
        it had not earned.
    #>
    param([string[]]$All)

    $paths = @()
    $base = git merge-base HEAD origin/main 2>$null
    if ($LASTEXITCODE -eq 0 -and $base) { $paths += git diff --name-only --diff-filter=d $base }
    $paths += git diff --name-only --diff-filter=d
    $paths += git diff --name-only --diff-filter=d --cached
    $global:LASTEXITCODE = 0

    $touched = @($paths | Where-Object { $_ -like 'src/*' } | Sort-Object -Unique)
    if ($touched | Where-Object { $_ -like '*.h' }) {
        Write-Host "  a header changed, so every translation unit is in scope"
        return $All
    }

    $wanted = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $touched) {
        [void]$wanted.Add([System.IO.Path]::GetFullPath((Join-Path $repoRoot $path)))
    }
    return @($All | Where-Object { $wanted.Contains($_) })
}

Push-Location $repoRoot
try {
    $sources = Get-ChildItem -Recurse -Path src, tests -Include *.cpp, *.h, *.in |
        ForEach-Object { $_.FullName }

    Invoke-Check 'clang-format' {
        Assert-PinnedTool 'clang-format'
        clang-format --dry-run --Werror @sources
    }

    if ($Configure) {
        Invoke-Check 'cmake --preset x64-lint' { cmake --preset x64-lint }
    }

    Invoke-Check 'clang-tidy' {
        Assert-PinnedTool 'clang-tidy'

        $cpp = Get-ChildItem -Recurse -Path src -Include *.cpp | ForEach-Object { $_.FullName }
        if (Test-StaleCompileDatabase $cpp) {
            Write-Host "  compile database is stale; reconfiguring ..."
            cmake --preset x64-lint | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "cmake --preset x64-lint failed" }
        }

        $targets = if ($Changed) { Select-ChangedSources $cpp } else { $cpp }
        if ($targets.Count -eq 0) {
            Write-Host "  nothing changed under src/"
            $global:LASTEXITCODE = 0
            return
        }

        # One process per file, across the cores. clang-tidy treats every
        # translation unit independently anyway, so this is the same work in the
        # same order of magnitude of CPU -- it was simply being done on one core,
        # which is how a full pass came to take a quarter of an hour on a
        # twenty-core machine and became something to skip rather than run.
        $jobs = if ($Jobs -gt 0) { $Jobs } else { [Math]::Max(1, [Environment]::ProcessorCount - 2) }
        $database = Join-Path $repoRoot 'build/x64-lint'
        Write-Host "  $($targets.Count) file(s), $jobs at a time"

        $reports = $targets | ForEach-Object -ThrottleLimit $jobs -Parallel {
            $lines = & clang-tidy -p $using:database --quiet `
                --extra-arg=-Wno-unused-command-line-argument $_ 2>&1
            # "213847 warnings generated." is the front end counting diagnostics
            # it then suppressed, one line per file. Thirty-eight of those buried
            # the four findings that mattered.
            $kept = $lines | Where-Object { $_ -notmatch '^\d+ warnings generated\.$' }
            [pscustomobject]@{
                Failed = ($LASTEXITCODE -ne 0)
                Output = ($kept | Out-String)
            }
        }

        foreach ($report in $reports) {
            if ($report.Output.Trim()) { Write-Host $report.Output.TrimEnd() }
        }
        # Exit code alone, as before: whether a finding is fatal is `.clang-tidy`'s
        # decision, and this has to agree with CI about it rather than invent a
        # stricter rule of its own.
        $global:LASTEXITCODE = if (@($reports | Where-Object { $_.Failed }).Count -gt 0) { 1 } else { 0 }
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
