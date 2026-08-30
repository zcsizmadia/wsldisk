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

### Compaction (issue #1) — answered

**Unattached compaction works, needs no administrator rights, and reclaims
everything `fstrim` freed.**

Method: import Alpine, `dd` 1 GiB of `/dev/urandom` to `/big.bin`, delete it,
`fstrim /`, `wsl --shutdown`, then `OpenVirtualDisk` + `CompactVirtualDisk`
from an **unelevated** process, trying several parameter shapes against the same file.

> **Corrected 2026-08-30.** The access-mask rows below originally reported that
> `VIRTUAL_DISK_ACCESS_METAOPS` opens but fails to compact. That was wrong: the
> spike's P/Invoke declared `ACCESS_METAOPS = 0x00020000`, which is
> `VIRTUAL_DISK_ACCESS_ATTACH_RW`, not `METAOPS` (`0x00200000`). Attaching
> read-write is what needs elevation, so the spike measured an attach denial and
> attributed it to `METAOPS`. Re-measured with the correct constant while
> building the #21 contract test; the table now reflects the corrected run.
> The reclaim figures below were always measured through V2 + `ACCESS_NONE` and
> are unaffected.

| `OpenVirtualDisk` shape | Access mask | Result |
|---|---|---|
| `OPEN_VIRTUAL_DISK_VERSION_1` (`RWDepth = 1`) | `METAOPS` (`0x00200000`) | opens and compacts, rc = 0 |
| `OPEN_VIRTUAL_DISK_VERSION_1` | `ATTACH_RW` (`0x00020000`) | opens, then compact fails with **5 (ERROR_ACCESS_DENIED)** |
| `OPEN_VIRTUAL_DISK_VERSION_1` | `NONE` | opens, then compact fails with **5** |
| **`OPEN_VIRTUAL_DISK_VERSION_2`** | **`VIRTUAL_DISK_ACCESS_NONE`** | **opens and compacts, rc = 0** |
| `OPEN_VIRTUAL_DISK_VERSION_2` | `METAOPS` | fails to open with 87 (`ERROR_INVALID_PARAMETER`) |
| `OPEN_VIRTUAL_DISK_VERSION_2` | `ATTACH_RW` | fails to open with 87 |

The rule the corrected run shows is that the V2 parameters accept
`VIRTUAL_DISK_ACCESS_NONE` and nothing else — any non-zero mask is rejected at
open with 87 — while V1 derives its rights from the mask and so needs `METAOPS`
to compact. Both `V1 + METAOPS` and `V2 + NONE` compact unelevated.

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

1. **PLAN.md §4.2 step 4 is workable, but `OPEN_VIRTUAL_DISK_VERSION_2` with
   `VIRTUAL_DISK_ACCESS_NONE` is the shape to use.** The plan's original
   `VIRTUAL_DISK_ACCESS_METAOPS` does compact unelevated, so it was not the bug
   this section first claimed. The V2 shape is preferred because it has exactly
   one valid spelling: the mask must be `VIRTUAL_DISK_ACCESS_NONE`, and every
   other value fails loudly at open with 87. The V1 shape has two spellings that
   both open and only one that compacts — `NONE` opens and then fails at the
   compaction with `ERROR_ACCESS_DENIED` — so a mistake there surfaces late,
   after the preflight has already told the user their disk is about to shrink.
   `METAOPS` must still not be combined with V2 parameters.
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

### Shrink mechanism (issue #2) — answered, no helper distro needed

`wsl --mount --vhd --bare` attaches a terminated distribution's VHDX into the
running VM, **unelevated**, and any other running distribution can then fsck and
resize it. A dedicated helper distribution is not required as a *mechanism*; it
is only a convenience for when no other distribution is available.

Method: import a target and a helper from the Alpine fixture, mark the target,
`wsl --shutdown`, start only the helper, mount the target's disk bare, and work
on it from the helper.

| Step | Result |
|---|---|
| `wsl --mount <vhdx> --vhd --bare` | exit 0, **no administrator rights** |
| Device it becomes | a new `/dev/sdX` (`/dev/sdf` in this run) |
| `blkid` | `UUID="f954...6b76" TYPE="ext4"` |
| `blockdev --getsize64` | 1099511627776 (1 TiB) |
| `e2fsck -fn` | clean: `545/67108864 files (0.2% non-contiguous), 4497461/268435456 blocks` |
| `resize2fs -P` | `Estimated minimum size of the filesystem: 2655555` blocks |
| `wsl --unmount` | exit 0 |
| Target afterwards | boots, marker file intact |

**Finding the device: diff `/proc/partitions` across the mount.** Size is useless
for identification — every WSL disk is 1 TiB by default, so this machine already
had three identical 1 TiB devices before mounting a fourth. `blkid` confirms the
filesystem afterwards, but only the diff says *which* device is ours.

**`resize2fs -P` is the preflight `shrink` should use, and the floor is higher
than expected.** On a nearly empty distribution whose disk is the default 1 TiB,
the reported minimum was 2,655,555 blocks — about **10.9 GiB** at 4 KiB blocks —
because the inode table was sized for a 1 TiB filesystem (67 million inodes).
PLAN.md §4.3 proposed "guest used bytes + 10% margin" as the fit check; that
would happily accept a target far below what `resize2fs` can actually produce.
Ask the filesystem instead, and report the floor in the refusal message.

**Guest tooling is not a given.** The stock Alpine minirootfs has `/sbin/apk`,
`/sbin/blkid`, `/sbin/blockdev` and `/sbin/fstrim` — all busybox applets — but
**no `e2fsck` and no `resize2fs`**. `apk add e2fsprogs-extra` installed them from
inside WSL in a few seconds, so the network is available, but that makes `shrink`
depend on the guest having a package manager and connectivity. The helper
distribution wsldisk ships or imports must include e2fsprogs rather than assume it.

**`wsl --exec` needs absolute paths.** Every probe first failed with
`execvpe(blkid) failed: No such file or directory`, including `apk`, even though
the child environment reports
`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:...`. The
lookup does not use that PATH. `IWslHost` must therefore invoke `/sbin/fstrim`,
`/sbin/e2fsck` and friends by absolute path — which is why the compaction spike,
which happened to use `/sbin/fstrim`, worked first time.

**Driving `wsl.exe` from PowerShell has two traps** worth knowing before the
integration helpers are written: an embedded quote does not survive
PowerShell → `wsl.exe` → `sh -c`, so guest commands should avoid `sh -c`
altogether; and with `$ErrorActionPreference = 'Stop'`, the `Failed to translate`
chatter `wsl.exe` writes to stderr is treated as a fatal native-command error.

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

### `wslapi.dll` and uid 0 (issue #3) — answered, and it removes an API from the plan

**`wslapi.dll` is unusable from an ordinary unpackaged process.** Every entry
point tried returns the same refusal regardless of its argument:

| Distribution | `WslIsDistributionRegistered` | `WslGetDistributionConfiguration` |
|---|---|---|
| `Ubuntu` (registered for months) | `False` | `0x80070005` E_ACCESSDENIED |
| `docker-desktop` | `False` | `0x80070005` |
| `rancher-desktop`, `rancher-desktop-data` | `False` | `0x80070005` |
| a freshly imported distro | `False` | `0x80070005` |
| the same, after it has been started | `False` | `0x80070005` |
| **a name that does not exist at all** | `False` | `0x80070005` |

Measured from a 64-bit unelevated process with `wslapi.dll` present in
`System32`, while `wsl.exe --list` listed all four distributions.

The last row is the informative one: a name that does not exist returns exactly
the same status as one that does, so the call is being rejected before it ever
looks at the argument. The most likely reason is that these APIs require the
caller to have a package identity — they exist for the MSIX distribution
launchers that ship on the Store — but whatever the mechanism, the behaviour is
uniform refusal, and running elevated was not tested because an API that needed
administrator rights to enumerate distributions would be unusable for `list`
anyway.

**Consequences for the plan:**

1. **PLAN.md §5.3 lists `wslapi.dll` as a primary API.** It cannot be, and the
   conflict is with a goal rather than a detail: goal 6 is a single
   `wsldisk.exe` distributed through winget and scoop, which is exactly the
   unpackaged shape these APIs refuse. `IWslHost` is therefore `wsl.exe` plus the
   registry, with the COM `ILxssUserSession` surface from the open-sourced
   microsoft/WSL as the later upgrade path that §5.3 already anticipated.
2. **The uid-0 question is moot as originally posed.** `WslLaunch` takes no uid —
   it runs as the distribution's `DefaultUid` — so the plan's "`WslLaunch` with
   uid 0" was never going to work as written, and the function is unreachable
   regardless. `wsl.exe -d <distro> -u root --exec /absolute/path` is the
   mechanism, and the compaction spike already proved it: `fstrim` ran as root
   against a distribution whose default user was root, and `-u root` overrides
   `DefaultUid` in every case.
3. **Enumeration comes from the registry, which the registry spike already
   mapped.** Nothing is lost: `WslGetDistributionConfiguration` would have
   returned version, `DefaultUid` and flags, all of which are registry values we
   read directly.
4. **The risk table's mitigation needs rewording.** "Prefer registry +
   `wslapi.dll` + WSL COM" leaves only registry and COM; `wsl.exe` output parsing
   is not a last resort but the primary mechanism for terminate, mount and
   manage, so tolerating its UTF-16, CRLF and localisation matters more than the
   table implied.

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

- Elevation relaunch and named-pipe IPC (#6) — now lower priority, since the
  common compaction path needs no elevation at all.
