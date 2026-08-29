<#
.SYNOPSIS
    Downloads the pinned Alpine rootfs used by the integration tests and
    verifies its SHA256.

.DESCRIPTION
    The tarball is described by tests/fixtures/alpine-rootfs.json. Nothing is
    trusted from the network: a download whose digest does not match the pinned
    value is deleted and the script fails.

    Prints the path to the verified tarball on success, so callers can do:

        $rootfs = .\scripts\fetch-fixtures.ps1

.PARAMETER Architecture
    Rootfs architecture: x86_64 (default) or aarch64.

.PARAMETER Destination
    Directory to cache the tarball in. Defaults to tests/fixtures/cache, which
    is git-ignored.

.PARAMETER Force
    Re-download even when a verified copy is already cached.
#>
[CmdletBinding()]
param(
    [ValidateSet('x86_64', 'aarch64')]
    [string]$Architecture = 'x86_64',

    [string]$Destination,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $repoRoot 'tests/fixtures/alpine-rootfs.json'
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json

$artifact = $manifest.artifacts.$Architecture
if (-not $artifact) {
    throw "No rootfs pinned for architecture '$Architecture' in $manifestPath."
}

if (-not $Destination) {
    $Destination = Join-Path $repoRoot 'tests/fixtures/cache'
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$target = Join-Path $Destination $artifact.file

function Test-Digest {
    param([string]$Path, [string]$Expected)
    if (-not (Test-Path $Path)) { return $false }
    return (Get-FileHash -Path $Path -Algorithm SHA256).Hash -ieq $Expected
}

if (-not $Force -and (Test-Digest -Path $target -Expected $artifact.sha256)) {
    Write-Verbose "Using cached $target"
    return $target
}

$url = "$($manifest.mirror)/$($manifest.branch)/releases/$Architecture/$($artifact.file)"
Write-Verbose "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing

if (-not (Test-Digest -Path $target -Expected $artifact.sha256)) {
    $actual = (Get-FileHash -Path $target -Algorithm SHA256).Hash
    Remove-Item $target -Force
    throw @"
SHA256 mismatch for $($artifact.file).
  expected: $($artifact.sha256)
  actual:   $actual
The download was discarded. Either the mirror served a bad file, or
tests/fixtures/alpine-rootfs.json needs updating on purpose.
"@
}

return $target
