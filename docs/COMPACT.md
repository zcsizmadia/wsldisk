# Compaction

What `wsldisk compact` does, why it is safe, and why the awkward parts are
awkward. Every number here was measured; the spikes behind them are in
[RESEARCH.md](RESEARCH.md).

## The problem

A WSL2 distribution lives in an `ext4.vhdx` that **only ever grows**. Delete
20 GB inside the guest and the file on your C: drive stays exactly the same
size. The space is free as far as ext4 is concerned and still occupied as far as
Windows is concerned.

Two things have to happen to get it back:

1. The guest has to tell the disk which blocks it is no longer using. That is
   `fstrim`.
2. The disk has to stop reserving those blocks in the file. That is
   `CompactVirtualDisk`.

Doing only the first leaves the file the same size. Doing only the second
reclaims almost nothing, because the disk was never told anything was free.

## What `compact` does

```text
wsldisk compact Ubuntu
```

1. **`fstrim /` in the guest.** Discards the blocks ext4 has freed. Skip it with
   `--no-trim` if you have already trimmed.
2. **Stop the distribution and wait for the disk.** See
   [below](#why-terminate-is-not-enough) — this is the step that most often
   refuses.
3. **`CompactVirtualDisk` on the unattached file.** No administrator rights, no
   Hyper-V module.
4. **Report what changed**, as the size on disk before and after.

```text
Ubuntu: 5.0 GiB reclaimed (14.0 GiB to 9.0 GiB)
```

Nothing inside the distribution changes. The filesystem is not rewritten, files
are not moved, and the distribution boots afterwards exactly as it did before.
What goes is the space the `.vhdx` was reserving for blocks nothing is using.

## Why `--terminate` is not enough

The WSL utility VM keeps **every attached disk** open for as long as **any**
distribution is running. Stopping the one you want to compact does not release
its disk if anything else is still up — including Docker Desktop, which counts.

Measured: after `wsl --terminate`, the handle was still held after five minutes
of polling. It is not released on a timer. See decision D9 in
[PLAN.md](../PLAN.md).

So `compact` terminates the target, polls briefly, and if the disk is still held
it **refuses** and tells you who is holding it:

```text
error: C:\Users\example\...\ext4.vhdx is still open in docker-desktop,
rancher-desktop -- the WSL utility VM keeps every disk open while any
distribution runs; re-run with --shutdown to stop them all, or close them
yourself first
```

Exit code 11.

`--shutdown` is what lets it proceed, and it is opt-in for a reason: the only
way to release one disk is to stop **all** of them. Doing that silently would
kill whatever you had running in another window, including containers. That is
your call to make, not the tool's.

```text
wsldisk compact Ubuntu --shutdown
```

## Why no administrator rights are needed

`CompactVirtualDisk` on an **unattached** disk needs no elevation. This is the
result the whole project rests on: it is what makes reclaiming space work on
Windows Home, where the Hyper-V PowerShell module — and therefore
`Optimize-VHD` — does not exist.

Measured (spike #1): a 1 GiB file written into a fresh Alpine distribution and
deleted, then `fstrim` and an unelevated compaction, reclaimed **exactly
1,073,741,824 bytes** in **0.2 s**.

The disk is opened with `OPEN_VIRTUAL_DISK_VERSION_2` parameters and
`VIRTUAL_DISK_ACCESS_NONE`, which is the only mask V2 accepts. The older V1
shape with `VIRTUAL_DISK_ACCESS_METAOPS` also compacts unelevated, but V1
accepts masks that open successfully and then fail at the compaction — after the
user has been told their disk is about to shrink. V2 fails at the open or not at
all. See decision D10.

There is a second, "full" mode that attaches the disk read-only so the
compaction can consult the filesystem bitmap. It **does** need administrator
rights, and after `fstrim` it has nothing left to find. It is deferred to M2 as
an opt-in for disks that were never trimmed.

## What "reclaimable" means

`wsldisk list` and `wsldisk info` report a **reclaimable** figure:

```text
NAME      VER  STATE    SIZE ON DISK  GUEST USED  RECLAIMABLE  PATH
Ubuntu *  2    running  13.8 GiB      10.0 GiB    3.7 GiB      C:\Users\example\...
```

That is `size on disk` minus `guest used`: the space the file occupies on your
drive that the guest filesystem is not using for anything. It is an estimate of
what a compaction could return, and it is only available when the distribution
is running or `--probe` is passed, because the guest-used half comes from `df`
inside the guest.

It is not a promise. Compaction works in blocks, and a block with one live byte
in it stays.

### What `fstrim` reports is *not* this

`fstrim -v` prints a "bytes trimmed" figure, and it is wildly misleading.
Measured: after freeing 1 GiB, `fstrim /` reported **1,078,939,029,504 bytes** —
the entire free extent of the 1 TB default virtual size, three orders of
magnitude out.

`wsldisk trim` reports the number because it is the only thing `fstrim` says,
and labels it every single time:

```text
Ubuntu: trimmed. fstrim reported 1004.8 GiB.
that figure is the free extent of the disk, not space reclaimed: compaction is what shrinks the file
run `wsldisk compact Ubuntu` to shrink the file itself
```

In `--json` the field is `bytes_offered`, not `bytes_freed`, for the same
reason.

## Why sparse mode is not the answer

WSL can mark a `.vhdx` sparse (`wsl --manage <distro> --set-sparse true`), which
makes the disk hand space back automatically. It sounds like it makes `compact`
unnecessary. Two problems:

- Microsoft put automatic sparse mode behind `--allow-unsafe` in WSL 2.5.6 after
  reports of **data corruption** ([WSL#12103](https://github.com/microsoft/WSL/issues/12103)).
- It is off for most existing distributions, and turning it on does not shrink a
  disk that has already grown.

`wsldisk` never enables sparse mode on its own. A guided `set-sparse` command
that prints the caveat and requires `--i-understand` is planned for M2.

## Compacting something that is not a distribution

Docker Desktop keeps a `docker_data.vhdx` that no WSL distribution claims and
that holds every volume you have. `wsldisk orphans` will find it; `compact
--file` can shrink it:

```text
wsldisk compact --file C:\Users\example\AppData\Local\Docker\wsl\disk\docker_data.vhdx
```

There is no guest to trim and nothing to terminate, so this is the compaction
alone — and it still refuses if another process has the file open, which Docker
does whenever it is running.

## Everything else

```text
wsldisk compact --all             # every WSL2 distribution
wsldisk compact Ubuntu --dry-run  # print the steps, change nothing
wsldisk compact Ubuntu --restart  # start it again afterwards if it was running
wsldisk compact Ubuntu --json     # one object, for scripts
```

`--all` keeps going past a distribution that fails and exits with the first
failure's code, because stopping at the first one leaves you to work out how far
it got. WSL1 distributions are skipped: they have no virtual disk.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Compacted, or nothing needed doing |
| 2 | The command line was wrong — no target, or two |
| 3 | A preflight refused: WSL1, or the disk is not where the registry says |
| 5 | The disk grew during compaction (verify failed) |
| 11 | Something is holding the disk — see [above](#why-terminate-is-not-enough) |

The full list is in [JSON.md](JSON.md#exit-codes).
