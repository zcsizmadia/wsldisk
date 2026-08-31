# JSON output

Every command that produces data takes `--json`. This document is the schema.

The one exception is `completion`, which prints a shell script to be sourced.
There is nothing for `--json` to mean there, so it is not accepted — a usage
error rather than a flag that is taken and ignored.

**Schema version: 1.** It changes when a field is removed or its meaning
changes; adding a field is not a breaking change, so parse leniently and ignore
what you do not recognise.

## Rules that hold everywhere

- **Sizes are integers, in bytes.** Never `"14.2 GiB"`. A consumer that wants a
  human string can format one; a consumer given a human string cannot get the
  number back.
- **One object per line** when a command reports many things, a single object
  otherwise. This is JSON Lines, not a JSON array: a long `list` can be streamed
  and a partial read is still parseable up to the last newline.
- **Errors go to stdout as an object too.** That is the promise `--json` makes:
  stdout is parseable whether or not the command worked. Without `--json` errors
  go to stderr, where a human expects them.
- **`--verbose` never goes to stdout.** It goes to stderr, so `-v --json`
  together still produce clean output.
- **Absent is not zero.** A field that could not be measured is *missing*, not
  `0` or `null`. `guest_used` is absent for a stopped distribution because
  nothing has asked the guest; reporting `0` would say the distribution is
  empty.

The golden files under
[`tests/unit/golden/`](../tests/unit/golden/) are the source of truth: they are
byte-compared on every build, so a change to any of this fails CI.

## `list`

One object per distribution. Golden:
[`list-json.txt`](../tests/unit/golden/list-json.txt).

```json
{"allocated_bytes":15032385536,"default":true,"file_size":15032385536,"flavor":"ubuntu","guest_free":966367641600,"guest_used":8589934592,"guid":"{4d1297e9-bac4-4da1-9867-a2ab591e9581}","name":"Ubuntu","os_version":"24.04","reclaimable":6442450944,"size_on_disk":15032385536,"sparse":true,"version":2,"vhdx_path":"C:\\wsl\\Ubuntu\\ext4.vhdx","virtual_size":1099511627776}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `name` | string | yes | Distribution name, as `wsl.exe` knows it |
| `guid` | string | yes | Registry key name, braces included |
| `version` | number | yes | 1 or 2. WSL1 has no virtual disk |
| `default` | bool | yes | Whether this is the default distribution |
| `vhdx_path` | string | yes | Full path to the `.vhdx` |
| `flavor` | string | no | `Flavor` from the registry, when recorded at import |
| `os_version` | string | no | `OsVersion` from the registry |
| `virtual_size` | number | no | The maximum the disk can grow to |
| `file_size` | number | no | Logical size of the file |
| `size_on_disk` | number | no | What it occupies on the volume — the number users notice |
| `allocated_bytes` | number | no | Sum of the file's allocated ranges |
| `sparse` | bool | no | Whether the sparse attribute is set. It cannot tell you *how much* of the file is real; that is `allocated_bytes` |
| `guest_used` | number | no | From `df` inside the guest. Needs the distribution running, or `--probe` |
| `guest_free` | number | no | Likewise |
| `reclaimable` | number | no | `size_on_disk` minus `guest_used`. An estimate — see [COMPACT.md](COMPACT.md#what-reclaimable-means) |
| `notes` | array of strings | no | Anything that could not be measured, and why |

## `info`

A single object: every `list` field, plus the ones only worth printing for one
distribution. Golden: [`info-modern.txt`](../tests/unit/golden/info-modern.txt)
(the table form; the JSON is asserted to be a superset of the `list` line).

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `registry_key` | string | yes | Full key path under HKCU |
| `base_path` | string | yes | `BasePath` exactly as stored, `\\?\` prefix and all |
| `vhd_file_name` | string | no | Absent on the legacy packaged layout, which has no such value |
| `modern` | bool | yes | Whether `Modern=1` is set |
| `default_uid` | number | yes | `DefaultUid` |
| `flags` | number | yes | Raw `Flags` value |
| `flags_decoded` | string | yes | Human reading of the same, e.g. `interop, append-nt-path` |
| `running` | bool | no | Absent when `wsl.exe` could not be asked |
| `block_size` | number | no | VHDX block size |
| `sector_size` | number | no | VHDX sector size |
| `parent_path` | string | no | Set only for a differencing disk |

## `trim`

```json
{"bytes_offered":1078939029504,"distribution":"Ubuntu","note":"that figure is the free extent of the disk, not space reclaimed: compaction is what shrinks the file","trimmed":true}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `distribution` | string | yes | The distribution that was trimmed |
| `trimmed` | bool | yes | Always `true` on success; the exit code carries failure |
| `bytes_offered` | number | no | What `fstrim` reported. **Absent when it said nothing** |
| `note` | string | yes | Why `bytes_offered` is not a saving |

`bytes_offered` is deliberately not called `bytes_freed`. It is the free extent
of the whole virtual disk, which was **three orders of magnitude** out from the
space actually freed when measured. See
[COMPACT.md](COMPACT.md#what-fstrim-reports-is-not-this).

## `compact`

One object per target, so `--all` streams.

```json
{"compacted":true,"reclaimed":5368709120,"size_after":9663676416,"size_before":15032385536,"target":"Ubuntu"}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `target` | string | yes | Distribution name, or the path for `--file` |
| `compacted` | bool | yes | Whether this target succeeded |
| `size_before` | number | no | Size on disk before. Absent when it could not be measured |
| `size_after` | number | no | Likewise, after |
| `reclaimed` | number | no | `size_before - size_after`, saturating at zero. Absent unless both ends were measured |
| `error` | string | no | Present when `compacted` is false: the message |
| `remedy` | string | no | What to do about it |
| `exit_code` | number | no | The code this failure would exit with |

A failed target is still an object on stdout. `--all` reports every target it
tried and exits with the first failure's code.

## `orphans`

One object per disk that no distribution claims. Golden:
[`orphans-table.txt`](../tests/unit/golden/orphans-table.txt) (the table form).

```json
{"path":"C:\\Users\\example\\AppData\\Local\\Docker\\wsl\\disk\\docker_data.vhdx","size_on_disk":72809086976}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `path` | string | yes | Full path to the `.vhdx` |
| `size_on_disk` | number | no | Absent when the file could not be measured |

Nothing is printed when nothing was found — an empty stream, not an empty
object.

**Not everything here is unused.** Docker Desktop's `docker_data.vhdx` holds
every volume you have and no distribution claims it. `orphans --delete` refuses
anything another process has open (exit 11), but the decision is still yours.

## `relink`

A single object. `orphans --relink <distro> --to <path>` is the same command and
emits the same object.

```json
{"base_path":"D:\\wsl\\Ubuntu","distribution":"Ubuntu","relinked":true,"vhdx_path":"D:\\wsl\\Ubuntu\\ext4.vhdx"}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `distribution` | string | yes | The distribution that was repointed |
| `vhdx_path` | string | yes | The `.vhdx` it now uses, as it was given on the command line |
| `base_path` | string | yes | What was written to the registry's `BasePath` |
| `relinked` | bool | yes | Always `true` on success; the exit code carries failure |

`base_path` is not always `vhdx_path`'s parent spelled the obvious way. WSL
stores that value in whichever prefix form the distribution already used, and
`relink` preserves it: an entry written as `\\?\C:\...` stays extended-length.
Docker Desktop's entry is one of those, and normalising it would be a change to
a value Docker owns. Read `base_path` rather than deriving it.

The distribution is started with `/bin/true` after the write, as a smoke test.
If it does not boot the registry is put back and the command fails, so a
`"relinked":true` means the new path was proved to work, not merely written.

## `config`

A single object.

```json
{"path":"C:\\Users\\example\\AppData\\Roaming\\wsldisk\\config.toml","settings":{"compact.restart":"false","compact.trim":"true","scan.dirs":"","wsl.unlock_timeout_seconds":"90"},"wslconfig":{"defaultVhdSize":"256GB"}}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `path` | string | yes | Where the config file is, whether or not it exists |
| `settings` | object | yes | Every setting, as strings. Missing file means defaults |
| `wslconfig` | object | no | The disk-relevant `.wslconfig` keys, read-only. Absent when none are set |

`settings` values are strings even for booleans and numbers, because they are
the same strings `config get` prints and `config set` accepts.

`wsldisk` never writes `.wslconfig`.

### The verbs

```json
{"path":"C:\\Users\\example\\AppData\\Roaming\\wsldisk\\config.toml"}
{"key":"compact.trim","value":"true"}
```

| Verb | Object |
|---|---|
| `config` | the full settings object, described above |
| `config path` | `{"path": ...}` |
| `config get` | every setting as one object |
| `config get <key>` | `{"key": ..., "value": ...}` |
| `config set <key> <value>` | `{"key": ..., "value": ...}` — the value as stored, which is what was parsed rather than what was typed |

## `--dry-run`

Every command that mutates takes `--dry-run`, and under `--json` it emits the
plan as one object per target rather than the human plan:

```json
{"distribution":"Ubuntu","dry_run":true,"steps":["run fstrim in Ubuntu","stop Ubuntu and wait for its disk"],"warnings":[{"message":"compaction rewrites the disk file and cannot be undone","remedy":"nothing inside the distribution changes; only unused blocks go"}]}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `distribution` / `target` | string | yes | What the plan is about. `compact` uses `target`, because it may be a loose `--file` rather than a distribution |
| `dry_run` | bool | yes | Always `true`; present so a reader cannot mistake a plan for a result |
| `steps` | array of string | yes | What would run, in order |
| `warnings` | array of object | yes | `{message, remedy}`. Always present; empty when the plan has none |

The exit code is the refusal's own code, not a generic one: a dry run that would
have been refused with `distro-busy` (11) exits 11, the same as the real run
would. That is the point of pre-checking.

## `orphans --delete`

One object per file, so a script can tell what went.

```json
{"path":"C:\\Users\\example\\AppData\\Local\\wsl\\Removed-Distro\\ext4.vhdx","deleted":true}
{"path":"D:\\held\\ext4.vhdx","deleted":false,"error":"D:\\held\\ext4.vhdx is in use by another process"}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `path` | string | yes | The file |
| `deleted` | bool | yes | Whether it went |
| `error` | string | no | Why not. Present exactly when `deleted` is `false` |
| `dry_run` | bool | no | `true` under `--dry-run`, where `deleted` is always `false` |

Under `--json` the table, the total, the "not everything here is unused" warning
and the confirmation prompt are all suppressed — stdout is the objects and
nothing else. `--yes` still applies.

**A partial failure exits 5 (`partial`), not 11.** 11 means the disks were held;
5 means some went and some did not, and the objects say which. Only an attempt
where *nothing* could be deleted exits 11.

## Errors

Any command with `--json` reports a failure as one object on **stdout**:

```json
{"error":"distro-busy","exit_code":11,"message":"C:\\...\\ext4.vhdx is still open in docker-desktop","remedy":"the WSL utility VM keeps every disk open while any distribution runs; re-run with --shutdown to stop them all, or close them yourself first"}
```

| Field | Type | Always? | Meaning |
|---|---|---|---|
| `error` | string | yes | Stable token — branch on this, not on the message |
| `exit_code` | number | yes | The process exit code |
| `message` | string | yes | What happened |
| `remedy` | string | no | What to do about it. Absent only when nothing helps |

`error` is stable across versions; `message` and `remedy` are prose and may be
reworded.

## Exit codes

The exit code is part of the interface and does not change.

| Code | `error` token | Meaning |
|---|---|---|
| 0 | — | Success |
| 1 | `generic` | Something went wrong that fits no other category |
| 2 | `usage` | The command line was malformed, or the arguments contradict |
| 3 | `preflight` | A check refused before anything ran. Nothing changed |
| 4 | `needs-elevation` | Needs an elevated token and could not get one |
| 5 | `partial` | Some steps succeeded and some did not; the output says which |
| 6 | `integrity-check-failed` | A read-only integrity check found problems |
| 10 | `distro-not-found` | No distribution of that name is registered |
| 11 | `distro-busy` | It is running, or its disk is held open by another process |

## Scripting notes

```powershell
# Every distribution over 10 GB on disk
wsldisk list --json | ForEach-Object { $_ | ConvertFrom-Json } |
    Where-Object { $_.size_on_disk -gt 10GB } | Select-Object name, size_on_disk
```

```bash
# Total reclaimable, in bytes
wsldisk list --json | jq -s 'map(.reclaimable // 0) | add'
```

`// 0` is not decoration: `reclaimable` is absent for a stopped distribution,
and a script that assumed it was always there would produce `null` for the sum.
