# `wsldisk move`

Relocates a distribution's virtual disk to another directory or drive.

```text
wsldisk move Ubuntu D:\WSL
```

## Why not `wsl --export` and `wsl --import`

That is WSL's own answer, and it costs you things you did not agree to give up:

- **the default user** — the distribution comes back as `root` and stays that
  way until you fix `DefaultUid` by hand
- **the flags** — interop, PATH appending and drive mounting all revert
- **the GUID** — anything that recorded it, including Docker Desktop, is now
  pointing at a distribution that no longer exists
- **the time** — it writes the whole filesystem out as a tar and reads it back

None of that is necessary. The disk is a file and the registry says where it is;
moving one and rewriting the other keeps everything else exactly as it was. That
is decision D4 in [PLAN.md](../PLAN.md).

## What it does, in order

The order is the whole safety argument. Your only copy of the data is the source
file, right up until the new one has been proved to boot — so nothing deletes it
before then, and everything before that point can be undone.

1. **Copy** the disk to the new directory, preserving its holes. On the same
   volume this is a rename instead, which is instant and moves no bytes.
2. **Repoint** the registry: `BasePath`, and `VhdFileName` where the
   distribution has one.
3. **Start** the distribution and run a command in it.
4. **Delete** the original — and only here, and only if step 3 passed.

A failure at step 3 puts the registry back, removes the copy and leaves you with
the working distribution you started with. Not two halves of one.

## Preflight

It refuses, before changing anything, when:

- the distribution is **running** — `wsl --terminate` it first. This is not
  caution for its own sake: the start test in step 3 would run inside the guest
  that is already booted, from the *old* disk, and pass without testing anything.
- the target volume is **FAT or exFAT** — no sparse file support, and a 4 GB
  file size limit that a WSL disk passes on day one
- the target volume **has no room**, measured against what the disk actually
  occupies rather than the virtual size it reports. A disk holding 12 GiB needs
  12 GiB of space even when it calls itself a terabyte; judging it by that number
  would refuse every move anyone ever wanted to make.
- there is **already a `.vhdx`** at the destination. The likeliest thing sitting
  there is your own previous attempt, and overwriting it would destroy a disk.
- the disk **is not where the registry says it is** — there is nothing to move,
  and [`wsldisk relink`](../README.md) is the command that fixes that

## Sparse files, and why the copy is not `CopyFileEx`

A `.vhdx` grows as it fills, so its logical length is roughly its current size —
**not** the virtual capacity. A 12 GiB Ubuntu inside a disk that reports a 1 TiB
virtual size is a 12 GiB *file*; the terabyte is a number recorded inside it.

What does vary is sparseness. WSL 2.5 and later create the disk sparse on some
machines, and `wsldisk set-sparse` can turn it on deliberately — at which point
the file has real holes, and a copy that walked its logical length would fill
them in. The disk would arrive intact but noticeably larger than it left.

So `move` creates the destination sparse, gives it the source's logical length,
and writes only the ranges the filesystem reports as allocated. On a disk that is
not sparse this is the same as copying it whole, which is the common case and
costs nothing.

Verified against real NTFS in the contract tests: a 64 MiB sparse file with two
1 MiB islands of data copies to a file that is still under 8 MiB on disk, with
identical allocated ranges and byte-for-byte identical contents.

## Options

```text
--keep-source   leave the original file where it was
--dry-run       print the plan and change nothing
--json          one object describing what happened
```

`--keep-source` costs the space twice, which is why it is not the default — but
it is a reasonable thing to want of an operation this size, and it turns the move
into a copy you can delete yourself once you are satisfied.

It also gives up the same-volume fast path, necessarily: a rename *moves* the
file, so there is nothing left where it was. Asking for the original to stay
means copying even when one volume would have done.

## Same volume

`wsldisk move Ubuntu C:\WSL` from somewhere else on `C:` is a rename plus a
registry write. No bytes move, no free space is needed, and there is no fourth
step because there is nothing left behind to delete. `--dry-run` says so.

Unless you pass `--keep-source`, which asks for something a rename cannot give.

## Output

```text
> wsldisk move Ubuntu D:\WSL
  copy C:\Users\example\AppData\Local\wsl\{...}\ext4.vhdx to D:\WSL\ext4.vhdx ...
  point Ubuntu at D:\WSL\ext4.vhdx ...
  start Ubuntu to check the new path works ...
  delete C:\Users\example\AppData\Local\wsl\{...}\ext4.vhdx ...
Ubuntu now lives at D:\WSL\ext4.vhdx (12.0 GiB)
```

`--json` gives one object, including `renamed` so a script can tell a rename from
a copy — one takes no time and the other takes as long as the disk is big. See
[JSON.md](JSON.md).

## Exit codes

| Code | Meaning |
|---|---|
| 0 | moved |
| 3 | a preflight check refused it; nothing changed |
| 5 | the move worked but something afterwards did not |
| 10 | no distribution by that name |
| 11 | the distribution is running |

## What this does not do

Moving the disk does not move anything else the distribution has on the old
drive — nothing else lives there. WSL keeps the whole filesystem inside the
`.vhdx`, which is what makes this a file move rather than a migration.

`wsldisk migrate <dir>`, which loops this over every distribution with one plan
and one confirmation, is M3.
