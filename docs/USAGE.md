# `wsldisk usage`

Where the space inside a distribution went.

```text
wsldisk usage Ubuntu
```

`compact` answers "give me the space back". This answers the question that comes
before it — and the one you need if `compact` did not give back as much as you
hoped, because a disk full of things you still have is not a disk with slack in
it.

## What it does

Runs `du` and `df` inside the guest and prints what it found, biggest first.

```text
SIZE       WHAT                 CLEARABLE  PATH
2.7 GiB    docker storage       no         /var/lib/docker
965.5 MiB  logs                 no         /var/log
840.0 MiB  systemd journal      yes        /var/log/journal
194.4 MiB  apt package lists    yes        /var/lib/apt/lists
176.2 MiB  apt package cache    yes        /var/cache/apt/archives

4.0 GiB found, of 10.2 GiB the guest reports in use
rows marked no hold things wsldisk cannot judge -- images you built, logs
something may be reading
/var/log contains other rows above; its size is not added twice
```

**It is read-only.** It runs two commands that measure and deletes nothing,
ever. [`wsldisk clean`](../ROADMAP.md) is the command that will act on this, and
it is deliberately separate.

## The clearable column

`yes` means emptying it loses only things that can be fetched or regenerated —
package caches, build caches, the journal.

`no` does **not** mean dangerous. It means wsldisk cannot tell whether the
contents matter. `/var/lib/docker` holds images and volumes you built;
`/var/log` holds logs something may still be reading. Deciding those are
disposable is yours to do, not the tool's — which is why `docker system prune`
is named in a note and never run for you.

## Nesting

Some entries live inside others. `/var/log/journal` is inside `/var/log`, and
`~/.cache/pip` is inside `~/.cache`. Both are worth reporting: you want to know
the specific one *and* the whole.

Only one of them can be added to a total, so the nested one is left out of the
`found` figure and the containing row says so. Without that the table would
present the same gigabytes twice as if they were two separate findings.

## Found, against in use

The `found` total is what the catalogue accounts for. It is almost always less
than what the guest reports in use, and the gap is your actual files — source
trees, databases, whatever you keep in there.

A large gap is not a problem to fix. It means the space is going to things
wsldisk has no catalogue entry for, which is what a working machine looks like.

`usage --by-directory` will break that gap down into the largest directories;
it is [#69](https://github.com/zcsizmadia/wsldisk/issues/69).

## The catalogue

`data/caches.toml`, compiled into the binary so the tool stays a single file.
Each entry is a path, a label, whether it is clearable, and a note:

```toml
[[cache]]
path = "/var/cache/apt/archives"
label = "apt package cache"
safe = true
note = "downloaded .deb files; `apt clean` empties it and apt re-fetches on demand"
```

A path starting with `~/` is expanded per user from `getent passwd`, so one
entry covers every account on the machine rather than assuming there is one.

Adding a package manager is a line of TOML, not a code change. That is the
point of keeping it as data.

## Options

```text
--top N     show only the largest N entries
--json      one object, with every entry as a member of `entries`
--verbose   say which path is being measured, on stderr
```

`--top` shortens the table and does **not** change the arithmetic: the `found`
total still counts everything that was measured, because a total that quietly
dropped the rows you did not print would be wrong rather than brief.

## It can be slow

`du` on a large filesystem takes minutes. `--verbose` says which path is being
measured so a long run does not look like a hang; under `--json` those lines go
to stderr, leaving stdout parseable.

## Guest paths

The paths in the output are your own file names, including home directories.
They go to stdout because you asked for them, and they never reach a test
fixture, a golden file or a log.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | measured |
| 3 | a WSL1 distribution, which has no virtual disk |
| 10 | no distribution by that name |

## What this does not do

Delete anything, suggest running `docker system prune` for you, or start a
distribution to measure it. Measuring a guest by booting it changes the thing
being measured, which is the same rule `info --probe` follows.
