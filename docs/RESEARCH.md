# Research & prior art

_Snapshot as of 2026-08-29. Spike results from M0 go at the bottom._

## Existing tools (all PowerShell/scripts unless noted)

| Tool | Does | Gaps |
|---|---|---|
| [okibcn/wslcompact](https://github.com/okibcn/wslcompact) | Most popular compactor; no admin needed (export/import under the hood) | Slow on large disks; rewrites the whole disk; no move/snapshot/shrink |
| [MechC-ODE/wsl2-shrink](https://github.com/MechC-ODE/wsl2-shrink) | Automates fstrim + Optimize-VHD/diskpart | Wrapper only |
| [brooks-code/WSL-VHDX-Compact](https://github.com/brooks-code/WSL-VHDX-Compact) | fstrim → shutdown → Optimize-VHD, diskpart fallback | Wrapper only |
| [Haenes/wsl2-compact](https://github.com/Haenes/wsl2-compact) | Compaction scripts | Wrapper only |
| [pxlrbt/move-wsl](https://github.com/pxlrbt/move-wsl) | Move distro via export/import | Slow; loses default user unless fixed manually |
| [Jammrock/Move-WSL2NewDrive](https://github.com/Jammrock/Move-WSL2NewDrive) | Menu-driven move | Same |
| [marvint24/wsl-backup-tool](https://github.com/marvint24/wsl-backup-tool) | GUI backup (Go/Wails), alpha | Full exports only |
| [augustoconconi/wsl-backup](https://github.com/augustoconconi/wsl-backup) | Snapshot script | Full exports only |
| `wsl --manage --set-sparse` | Auto-reclaim via TRIM | Behind `--allow-unsafe` since 2.5.6 due to corruption; unreliable ([WSL#12103](https://github.com/microsoft/WSL/issues/12103)) |
| `wsl --manage --resize` (2.5+) | Grow virtual size | Does not run `resize2fs`; no shrink |
| `Optimize-VHD` | Compact | Requires Hyper-V PowerShell module (not on Home) |
| `diskpart compact vdisk` | Compact | Manual script, no progress, admin |

## Reference reading

- Hanselman — [Shrink your WSL2 Virtual Disks and Docker Images](https://www.hanselman.com/blog/shrink-your-wsl2-virtual-disks-and-docker-images-and-reclaim-disk-space)
- Stephen Rees-Carter — [How to Shrink a WSL2 Virtual Disk](https://stephenreescarter.net/how-to-shrink-a-wsl2-virtual-disk/)
- Daniel Cosenza — [Sparse VHD Support in WSL](https://danielcosenza.com/posts/wsl-sparse-vhd/)
- VRAM Lab — [Sparse VHD vs diskpart, measured](https://vramlab.com/posts/wsl2-sparse-vhd-cannot-compact/)
- ashn — [How to fix WSL2 disk space bloat](https://www.ashn.dev/blog/2025-08-14-how-to-fix-wsl2-disk-space-bloat.html)
- Microsoft Q&A — [sparse vhd does not shrink / cannot compact](https://learn.microsoft.com/en-us/answers/questions/1526083/in-wsl2-with-sparse-vhd-the-storage-usage-does-not)
- Microsoft Docs — [Manage disk space (WSL)](https://learn.microsoft.com/en-us/windows/wsl/disk-space), [Virtual Disk API](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/), [wslapi.h](https://learn.microsoft.com/en-us/windows/win32/api/wslapi/)
- Source — [microsoft/WSL](https://github.com/microsoft/WSL) (C++; see `src/windows/service` for registry layout and disk handling), [microsoft/wil](https://github.com/microsoft/wil)

## M0 spike results

Measured on Windows 11 26200.9168, WSL 2.7.8.0 (Store), kernel 6.18.33.1-1, with
Ubuntu 24.04, Docker Desktop and Rancher Desktop installed. Every experiment used
a throwaway distribution imported from the pinned Alpine 3.22.4 rootfs and
unregistered afterwards; no real distribution was read, written or compacted.
Usernames are redacted as `<user>`.

### Compaction (issue #1) — answered, and the API shape in the plan is wrong

**Unattached compaction works, needs no administrator rights, and reclaims
everything `fstrim` freed — but only through the V2 open parameters.**

Method: import Alpine, `dd` 1 GiB of `/dev/urandom` to `/big.bin`, delete it,
`fstrim /`, `wsl --shutdown`, then `OpenVirtualDisk` + `CompactVirtualDisk`
from an **unelevated** process, trying four parameter shapes against the same file.

| `OpenVirtualDisk` shape | Access mask | Result |
|---|---|---|
| `OPEN_VIRTUAL_DISK_VERSION_1` (`RWDepth = 1`) | `METAOPS` | opens, then `CompactVirtualDisk` fails with **5 (ERROR_ACCESS_DENIED)** |
| `NULL` parameters | `METAOPS` | opens, then compact fails with **5** |
| **`OPEN_VIRTUAL_DISK_VERSION_2`** | **`VIRTUAL_DISK_ACCESS_NONE`** | **opens and compacts, rc = 0** |
| `OPEN_VIRTUAL_DISK_VERSION_2` | `METAOPS` | fails to open with 87 (`ERROR_INVALID_PARAMETER`) |

Sizes for the successful run:

| | bytes |
|---|---|
| after import | 79,691,776 |
| after writing 1 GiB | 1,145,044,992 |
| after `rm` + `fstrim` (file does not shrink by itself) | 1,145,044,992 |
| after `CompactVirtualDisk` | 71,303,168 |
| **reclaimed** | **1,073,741,824 (exactly 1 GiB)** |

Elapsed: **0.2 s**. The distribution booted normally afterwards.

**Consequences:**

1. **PLAN.md §4.2 step 4 is wrong as written.** It specifies
   `OpenVirtualDisk` with `VIRTUAL_DISK_ACCESS_METAOPS`; that combination opens
   the handle successfully and then fails the compaction with access denied, which
   is a confusing way to fail. The working combination is
   `OPEN_VIRTUAL_DISK_VERSION_2` with `VIRTUAL_DISK_ACCESS_NONE`, exactly as
   docs/ARCHITECTURE.md suggested — that guidance is now a requirement, not a
   preference. `METAOPS` must not be combined with V2 parameters at all.
2. **The unelevated path is the default, not a fallback.** `fstrim` followed by
   unattached compaction reclaimed 100% of the freed space without administrator
   rights. The attached read-only "full" mode is not needed for this case and can
   stay an opt-in for disks that were never trimmed. This is the result the whole
   Windows Home premise rested on.
3. **`fstrim -av` is not portable.** busybox (Alpine, and therefore the test
   fixture) rejects `-a`: `fstrim: unrecognized option: a`. Only
   `fstrim [-v] <mountpoint>` works everywhere, so the guest command must be
   `fstrim /` with `-v` attempted and its failure tolerated.
4. `fstrim /` reported `1078939029504 bytes trimmed` — the whole free extent of
   the 1 TB default `vhdSize`, not the 1 GiB actually freed. The number is not a
   useful measure of what compaction will reclaim; report before/after file sizes
   instead.

### VHDX lock behaviour (issues #1, #5) — answered, and it contradicts the plan

`wsl --terminate <distro>` **does not release the distribution's VHDX** while the
WSL utility VM is still running for another distribution.

- while the scratch distro runs: file locked (expected)
- after `wsl --terminate`: the distro disappears from `wsl --list --running`, but
  the VHDX **stays locked for at least 300 s** — polled every 5 s for five
  minutes, never released — and `wsl --manage --set-sparse` fails with
  `Wsl/Service/ERROR_SHARING_VIOLATION`
- after `wsl --shutdown`: the handle **is** released, within one poll interval

The disk is not released on a timer: it is held for as long as the utility VM
lives, and the VM lives as long as any distribution is running.

**Who holds it:** Restart Manager reports `pid 4 System` — the file is attached to
the host storage stack for the utility VM, not open in a user-mode process.
Stopped distributions report no holder at all.

**Consequences:**

- PLAN.md §4.2 step 3 ("`wsl --terminate <distro>` — only this distro, do not
  `--shutdown` others") is not sufficient on any machine where another
  distribution is running, which is the normal case with Docker Desktop
  installed. Recorded as decision **D9**: `compact` terminates the target, and if
  the disk is still locked it exits 11 naming the distributions that are keeping
  the VM alive, and tells the user to re-run with `--shutdown`. It never stops
  another distribution without being asked.
- `wsldisk lock` cannot name a user process for a VM-attached disk. It has to
  distinguish two cases and give different remedies: `System`/pid 4 means "the WSL
  VM still has it attached, run `wsl --shutdown`", while a real pid means "quit
  that application". Reporting "held by System" alone would be useless.

### Registry layout (issue #4) — answered

`HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss` holds one `{GUID}` subkey
per distribution plus these values at the root:

| Value | Type | Observed |
|---|---|---|
| `DefaultDistribution` | REG_SZ | `{GUID}` of the default |
| `DefaultVersion` | DWORD | `2` |
| `NatIpAddress` | REG_SZ | present even with `networkingMode=mirrored` |
| `OOBEComplete` | DWORD | `1` |

Per distribution:

| Value | Type | Notes |
|---|---|---|
| `DistributionName` | REG_SZ | the name `wsl -l` shows |
| `BasePath` | REG_SZ | **prefix form varies per distro on the same machine** |
| `VhdFileName` | REG_SZ | `ext4.vhdx` on every distro seen, but must not be assumed |
| `Version` | DWORD | `1` or `2` |
| `Flags` | DWORD | `15` (`0xF`) on every distro seen |
| `DefaultUid` | DWORD | `1000` for Ubuntu, `0` for the appliance distros |
| `State` | DWORD | `1` |
| `Modern` | DWORD | `1` — not in our docs; marks the non-MSIX install layout |
| `Flavor` | REG_SZ | `ubuntu`, `alpine`, … — detected from `/etc/os-release` on import |
| `OsVersion` | REG_SZ | `24.04`, `3.22.4` — likewise |
| `RunOOBE` | DWORD | `0` after first run |
| `ShortcutPath` | REG_SZ | Start Menu `.lnk`; absent for `docker-desktop` |
| `TerminalProfilePath` | REG_SZ | Windows Terminal fragment; absent for `docker-desktop` |
| `DockerDesktopBuildNumber` | REG_SZ | Docker Desktop only |

**Findings that change the plan:**

1. **`BasePath` prefix forms differ between distributions on one machine.**
   Ubuntu and both Rancher distros store a bare path
   (`C:\Users\<user>\AppData\Local\wsl\{GUID}`); `docker-desktop` stores
   `\\?\C:\Users\<user>\AppData\Local\Docker\wsl\main`. `move` and `relink` must
   preserve whatever form was already there rather than normalising — PLAN.md
   §4.4 step 4 anticipated this, and it is now confirmed rather than assumed.
2. **Modern WSL does not use the packaged path PLAN.md §1 describes.** Ubuntu
   24.04 installed by `wsl --install` lives in `%LOCALAPPDATA%\wsl\{GUID}\ext4.vhdx`,
   not `%LOCALAPPDATA%\Packages\<pkg>\LocalState\ext4.vhdx`. `Modern=1` marks the
   new layout. Both belong in the `orphans` scan list, and §1 should describe the
   packaged path as legacy.
3. **`docker-desktop-data` no longer exists** on current Docker Desktop — there is
   a single `docker-desktop` distribution whose VHDX is 96 MiB, with image data
   held elsewhere. PLAN.md §3 and §4.2 name `docker-desktop-data` as the thing
   users want to compact; that is version-dependent and must be discovered from
   the registry, never hardcoded.
4. **`Flags` was `15` everywhere, so the sparse bit could not be identified.**
   `WSL_DISTRIBUTION_FLAGS` documents only bits 0–2 (interop, NT path, drive
   mounting); bit 3 is undocumented but always set. No distribution here has
   sparse mode on, so `list` should read sparseness from
   `FILE_ATTRIBUTE_SPARSE_FILE` / `FSCTL_QUERY_ALLOCATED_RANGES` rather than from
   `Flags`.
5. **`Flavor`/`OsVersion` are populated by `wsl --import`** from the rootfs, so
   `info` can show the guest OS without booting the distribution.

### Hosted-runner WSL2 (issue #7) — answered

The integration job runs on hosted `windows-2025`: `wsl --install --no-distribution`,
`wsl --update`, importing the pinned Alpine rootfs and booting it all succeed, in
about 1.5 minutes end to end. Nested virtualisation is available on the hosted
image and no self-hosted runner is needed.

### Incidental

`wsl.exe` prints `Failed to translate '<path>'` to stderr for every Windows PATH
entry it cannot map when the calling process has a POSIX-style PATH. It is noise,
not an error, but `IWslHost` must not treat stderr output as failure and should
consider passing `WSLENV`/a clean environment.

### Still open

- Shrink via `wsl --mount --vhd --bare` (#2).
- `WslLaunch` as uid 0 with a non-root default user (#3) — the spikes above used
  `wsl.exe -u root --exec`, which works; the `wslapi.dll` path is untested.
- Elevation relaunch and named-pipe IPC (#6) — now lower priority, since the
  common compaction path needs no elevation at all.
