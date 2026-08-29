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

_(fill in during M0)_

- Compact unattached vs attached-RO after fstrim: 
- Shrink via `wsl --mount --vhd --bare` on same distro: 
- `WslLaunch` as uid 0: 
- Registry `BasePath` forms observed (inbox 1.x / Store 2.x): 
- Docker Desktop VHDX lock behaviour: 
- Hosted runner WSL2 support: 
