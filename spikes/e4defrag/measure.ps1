<#
.SYNOPSIS
    Measures whether `e4defrag` closes the gap between in-place compaction and a
    full export/import rebuild. Spike for issue #65.

.DESCRIPTION
    `compact` is `fstrim` in the guest plus an unattached `CompactVirtualDisk`.
    The one thing it cannot recover is free space ext4 has scattered such that no
    discardable region is large enough to hand back: the blocks are free to the
    filesystem, but the VHDX still has them allocated.

    Rebuilding through `wsl --export` / `--import` does recover it, because the
    new disk is written from scratch. That is what wslcompact does. It costs a
    full temporary copy of the rootfs, time proportional to the data, and a
    replaced registry entry.

    The question this answers: does `e4defrag` before `fstrim` get close enough
    to a rebuild that a rebuild command is not worth building?

    Four measurements of size-on-disk, all on the same deliberately fragmented
    scratch distribution:

        1. baseline, fragmented
        2. fstrim + compact          -- what `compact` does today
        3. e4defrag + fstrim + compact
        4. wsl --export + --import   -- the full rebuild

    The number that decides #67 is 3 against 4.

.PARAMETER SizeMegabytes
    How much junk to write before fragmenting it. Bigger is a clearer signal and
    a slower run.

.PARAMETER FileKilobytes
    How big each junk file is. The fragmentation pattern is "write N files,
    delete every other one", so this sets how finely the free space is cut up.
    Large files leave large contiguous holes, which is the easy case; small
    files leave the scattered free space `e4defrag` is supposed to help with.

.PARAMETER KeepDistro
    Leave the scratch distribution registered, to poke at it afterwards.

.NOTES
    Only ever touches a `wsldisk-test-spike-*` distribution imported from the
    pinned Alpine fixture, and unregisters it at the end even if the script
    throws.

    It does run `wsl --shutdown`, because per decision D9 the utility VM holds
    every attached disk for as long as any distribution runs -- there is no way
    to compact anything without stopping all of WSL. That stops the caller's
    distributions and Docker Desktop's too.
#>
[CmdletBinding()]
param(
    [int]$SizeMegabytes = 1024,
    [int]$FileKilobytes = 4096,
    [switch]$KeepDistro
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$name = "wsldisk-test-spike-$([System.Diagnostics.Process]::GetCurrentProcess().Id)"
$dir = Join-Path $env:TEMP $name
$vhdx = Join-Path $dir 'ext4.vhdx'

function Guest {
    param([string]$Script)
    # stderr is dropped: every `--exec` prints a "Failed to translate" line per
    # Windows PATH entry, which is chatter rather than failure.
    $out = wsl.exe -d $name --exec /bin/sh -c $Script 2>$null
    if ($LASTEXITCODE -ne 0) { throw "guest command failed ($LASTEXITCODE): $Script" }
    return $out
}

# `GetCompressedFileSizeW` rather than shelling out to `dir`: a bare `cmd` finds
# msys's `cmd` script on this machine, and `Length` reports the logical size,
# which for a sparse VHDX is not what the volume gives back. Size on disk is the
# number the user notices.
Add-Type -Namespace Spike -Name Win32 -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern uint GetCompressedFileSizeW(string name, out uint high);
'@

function SizeOnDisk {
    [uint32]$high = 0
    $low = [Spike.Win32]::GetCompressedFileSizeW($vhdx, [ref]$high)
    if ($low -eq 0xFFFFFFFF -and [System.Runtime.InteropServices.Marshal]::GetLastWin32Error() -ne 0) {
        throw "could not measure $vhdx"
    }
    return ([int64]$high -shl 32) -bor [int64]$low
}

function ReleaseDisk {
    # D9: nothing releases one disk but stopping every distribution.
    wsl.exe --shutdown 2>&1 | Out-Null
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $handle = [System.IO.File]::Open($vhdx, 'Open', 'ReadWrite', 'None')
            $handle.Close()
            return
        } catch { Start-Sleep -Milliseconds 500 }
    }
    throw "the disk was still held after 20 seconds"
}

function Compact {
    ReleaseDisk
    & (Join-Path $repoRoot 'build/x64-debug/src/cli/Debug/wsldisk.exe') compact --file $vhdx --json 2>&1 |
        Out-Null
}

$results = [ordered]@{}

try {
    $rootfs = Join-Path $repoRoot 'tests/fixtures/cache/alpine-minirootfs-3.22.4-x86_64.tar.gz'
    if (-not (Test-Path $rootfs)) { throw "run scripts/fetch-fixtures.ps1 first: $rootfs" }

    Write-Host "importing $name ..."
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    wsl.exe --import $name $dir $rootfs --version 2 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "import failed" }

    # A sparse disk hands space back by itself, which is a different scenario.
    wsl.exe --manage $name --set-sparse false 2>&1 | Out-Null

    Write-Host "installing e4defrag ..."
    $tools = Guest 'apk add --no-cache e2fsprogs-extra >/dev/null 2>&1; command -v e4defrag || echo MISSING'
    $hasE4defrag = ($tools -join '') -notmatch 'MISSING'
    Write-Host "  e4defrag: $(if ($hasE4defrag) { 'available' } else { 'NOT AVAILABLE' })"

    # Fragment: write many small files, then delete every other one. `conv=fsync`
    # is not decoration -- without it the guest page cache absorbs the write and
    # the delete drops it before the kernel writes it out, so the disk never
    # grows and the whole measurement is of nothing.
    $count = [int](($SizeMegabytes * 1024) / $FileKilobytes)
    Write-Host "writing ${SizeMegabytes} MiB as $count files of ${FileKilobytes} KiB, then deleting every other one ..."
    Guest "mkdir -p /junk; i=0; while [ `$i -lt $count ]; do dd if=/dev/urandom of=/junk/f`$i bs=1K count=$FileKilobytes conv=fsync >/dev/null 2>&1; i=`$((i+1)); done; sync" | Out-Null
    Guest "i=0; while [ `$i -lt $count ]; do [ `$((i % 2)) -eq 0 ] && rm -f /junk/f`$i; i=`$((i+1)); done; sync" | Out-Null

    ReleaseDisk
    $results['1. fragmented baseline'] = SizeOnDisk

    Write-Host "fstrim + compact ..."
    Guest 'fstrim -v / >/dev/null 2>&1 || fstrim / >/dev/null 2>&1 || true' | Out-Null
    Compact
    $results['2. fstrim + compact'] = SizeOnDisk

    if ($hasE4defrag) {
        Write-Host "e4defrag + fstrim + compact ..."
        Guest 'e4defrag / >/dev/null 2>&1 || true' | Out-Null
        Guest 'fstrim -v / >/dev/null 2>&1 || fstrim / >/dev/null 2>&1 || true' | Out-Null
        Compact
        $results['3. e4defrag + fstrim + compact'] = SizeOnDisk
    }

    Write-Host "export + import (the rebuild) ..."
    $tar = Join-Path $env:TEMP "$name.tar"
    $rebuiltDir = "$dir-rebuilt"
    ReleaseDisk
    wsl.exe --export $name $tar 2>&1 | Out-Null
    wsl.exe --import "$name-rebuilt" $rebuiltDir $tar --version 2 2>&1 | Out-Null
    $rebuiltVhdx = Join-Path $rebuiltDir 'ext4.vhdx'
    wsl.exe --shutdown 2>&1 | Out-Null
    [uint32]$high = 0
    $low = [Spike.Win32]::GetCompressedFileSizeW($rebuiltVhdx, [ref]$high)
    $results['4. export + import'] = ([int64]$high -shl 32) -bor [int64]$low

    Write-Host ""
    Write-Host "size on disk, bytes:"
    foreach ($k in $results.Keys) {
        Write-Host ("  {0,-34} {1,15:N0}" -f $k, $results[$k])
    }
    if ($results.Contains('3. e4defrag + fstrim + compact')) {
        $gap = $results['3. e4defrag + fstrim + compact'] - $results['4. export + import']
        Write-Host ""
        Write-Host ("  e4defrag path is {0:N0} bytes larger than a rebuild" -f $gap)
    }
} finally {
    if (-not $KeepDistro) {
        Write-Host ""
        Write-Host "cleaning up ..."
        foreach ($d in @($name, "$name-rebuilt")) {
            wsl.exe --unregister $d 2>&1 | Out-Null
        }
        Remove-Item $dir, "$dir-rebuilt", (Join-Path $env:TEMP "$name.tar") -Recurse -Force -ErrorAction SilentlyContinue
    }
}
